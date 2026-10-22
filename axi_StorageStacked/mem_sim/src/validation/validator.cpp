// Command trace validator：Ramulator plugin 风格的离线命令流审计器。
// 它会独立重放 bank 状态、WCK 状态、tFAW 和 timing constraints，用来捕获
// Controller 在线路径可能遗漏的协议错误，也便于后续接 golden trace 对比。
#include "hbm_sim/validation/validator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <sstream>
#include <utility>

#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {
namespace {

constexpr std::size_t kMaxValidationErrors = 64;

bool decoded_in_range(const DramSpec &spec, const IssuedCommand &issued) {
  const Organization &o = spec.org;
  const DecodedAddress &d = issued.decoded;
  return issued.stack_id >= 0 && d.channel >= 0 && d.channel < o.channels &&
         d.pseudo_channel >= 0 &&
         d.pseudo_channel < o.pseudo_channels && d.sid >= 0 &&
         d.sid < o.sids && d.rank >= 0 && d.rank < o.ranks &&
         d.bank_group >= 0 && d.bank_group < o.bank_groups && d.bank >= 0 &&
         d.bank < o.banks_per_group && d.row >= 0 && d.row < o.rows &&
         d.column >= 0 && d.column < o.columns;
}

struct ValidatorScopeState {
  // next_command 与 TimingEngine::TimingScopeState 同义：在某个 scope bucket
  // 内， 每种命令下一次允许发射的最早 cycle。Validator
  // 独立维护它，避免信任在线状态。
  std::array<Cycle, kCommandCount> next_command{};
  // recent_acts 用于重放 tFAW/activation window。
  std::deque<Cycle> recent_acts;
  // LPDDR WCK ready window。CAS_RD/CAS_WR 设置窗口，RD/WR 必须落在窗口内。
  Cycle wck_ready_at = 0;
  Cycle wck_active_until = 0;
  // 控制状态重放：低功耗/self-refresh 是 channel 级状态，不属于单个 bank。
  bool power_down = false;
  bool self_refresh = false;
  // DVFS 后如果 policy 要求 WCK retrain，这个标志会阻止后续 CAS/RD/WR，
  // 直到 trace 中出现显式 WCK_TRAIN。
  bool wck_retrain_required = false;
};

struct ValidatorBankState {
  // Validator 自己维护一份最小 bank 状态，不复用 Controller::BankState。
  // 这样 trace validation
  // 能成为真正独立的后处理插件，而不是在线状态的回放打印。
  int open_row = -1;
  bool activating = false;
  int activating_row = -1;
  std::uint64_t activating_request = 0;
  Cycle next_act2 = 0;
  Cycle act2_deadline = 0;

  bool busy() const { return activating || open_row >= 0; }
  bool opened_row(int row) const { return !activating && open_row == row; }
};

struct ValidatorHbmPairingState {
  // 这组字段复刻 Controller::RisingEdgeCommandInfo。Validator 按 channel
  // 独立维护它，用来检查 falling-edge PRE 是否满足上一条 rising row command
  // 留下的 HBM4 edge pairing window。
  Command command = Command::NOP;
  int pseudo_channel = -1;
  int bank_key = -1;
  Cycle next_pairing_falling_edge = 0;
};

Cycle timing_delay(const DramSpec &spec, int cycles) {
  if (cycles <= 0) {
    return 0;
  }
  // Validator 必须使用和 Controller 完全相同的 nCK->tick 换算，否则合法 trace
  // 会被离线检查误判为 timing violation。
  return static_cast<Cycle>(cycles) *
         static_cast<Cycle>(std::max(1, spec.tick_multiplier));
}

std::size_t scope_count(const DramSpec &spec, TimingScope scope) {
  const Organization &o = spec.org;
  // 这里刻意复制 TimingEngine 的
  // scope_count/scope_index，而不是直接复用私有方法。 Validator
  // 作为“独立插件”应能发现 TimingEngine 自身遗漏的错误。
  switch (scope) {
  case TimingScope::Channel:
    return static_cast<std::size_t>(std::max(1, o.channels));
  case TimingScope::PseudoChannel:
    return static_cast<std::size_t>(std::max(1, o.channels) *
                                    std::max(1, o.pseudo_channels));
  case TimingScope::Sid:
    return static_cast<std::size_t>(std::max(1, o.channels) *
                                    std::max(1, o.pseudo_channels) *
                                    std::max(1, o.sids));
  case TimingScope::Rank:
    return static_cast<std::size_t>(std::max(1, o.channels) *
                                    std::max(1, o.pseudo_channels) *
                                    std::max(1, o.sids) * std::max(1, o.ranks));
  case TimingScope::BankGroup:
    return static_cast<std::size_t>(std::max(1, o.channels) *
                                    std::max(1, o.pseudo_channels) *
                                    std::max(1, o.sids) * std::max(1, o.ranks) *
                                    std::max(1, o.bank_groups));
  case TimingScope::Bank:
    return static_cast<std::size_t>(
        std::max(1, o.channels) * std::max(1, o.pseudo_channels) *
        std::max(1, o.sids) * std::max(1, o.ranks) *
        std::max(1, o.bank_groups) * std::max(1, o.banks_per_group));
  }
  return 1;
}

std::size_t scope_index(const DramSpec &spec, TimingScope scope,
                        const DecodedAddress &d) {
  const Organization &o = spec.org;
  std::size_t x = static_cast<std::size_t>(
      std::clamp(d.channel, 0, std::max(1, o.channels) - 1));
  if (scope == TimingScope::Channel) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.pseudo_channels)) +
      static_cast<std::size_t>(
          std::clamp(d.pseudo_channel, 0, std::max(1, o.pseudo_channels) - 1));
  if (scope == TimingScope::PseudoChannel) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.sids)) +
      static_cast<std::size_t>(std::clamp(d.sid, 0, std::max(1, o.sids) - 1));
  if (scope == TimingScope::Sid) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.ranks)) +
      static_cast<std::size_t>(std::clamp(d.rank, 0, std::max(1, o.ranks) - 1));
  if (scope == TimingScope::Rank) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.bank_groups)) +
      static_cast<std::size_t>(
          std::clamp(d.bank_group, 0, std::max(1, o.bank_groups) - 1));
  if (scope == TimingScope::BankGroup) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.banks_per_group)) +
      static_cast<std::size_t>(
          std::clamp(d.bank, 0, std::max(1, o.banks_per_group) - 1));
  return x;
}

std::pair<std::size_t, std::size_t>
sibling_scope_range(const DramSpec &spec, TimingScope scope,
                    const DecodedAddress &decoded) {
  const Organization &o = spec.org;
  std::size_t begin = 0;
  std::size_t count = 1;
  switch (scope) {
  case TimingScope::Channel:
    count = static_cast<std::size_t>(std::max(1, o.channels));
    break;
  case TimingScope::PseudoChannel:
    begin = scope_index(spec, TimingScope::Channel, decoded) *
            static_cast<std::size_t>(std::max(1, o.pseudo_channels));
    count = static_cast<std::size_t>(std::max(1, o.pseudo_channels));
    break;
  case TimingScope::Sid:
    begin = scope_index(spec, TimingScope::PseudoChannel, decoded) *
            static_cast<std::size_t>(std::max(1, o.sids));
    count = static_cast<std::size_t>(std::max(1, o.sids));
    break;
  case TimingScope::Rank:
    begin = scope_index(spec, TimingScope::Sid, decoded) *
            static_cast<std::size_t>(std::max(1, o.ranks));
    count = static_cast<std::size_t>(std::max(1, o.ranks));
    break;
  case TimingScope::BankGroup:
    begin = scope_index(spec, TimingScope::Rank, decoded) *
            static_cast<std::size_t>(std::max(1, o.bank_groups));
    count = static_cast<std::size_t>(std::max(1, o.bank_groups));
    break;
  case TimingScope::Bank:
    begin = scope_index(spec, TimingScope::BankGroup, decoded) *
            static_cast<std::size_t>(std::max(1, o.banks_per_group));
    count = static_cast<std::size_t>(std::max(1, o.banks_per_group));
    break;
  }
  const std::size_t total = scope_count(spec, scope);
  return {std::min(begin, total), std::min(begin + count, total)};
}

bool contains_command(const std::vector<Command> &commands, Command command) {
  return std::find(commands.begin(), commands.end(), command) != commands.end();
}

void add_error(CommandValidationReport &report, const IssuedCommand &issued,
               const std::string &message) {
  if (report.errors.size() >= kMaxValidationErrors) {
    return;
  }
  std::ostringstream os;
  os << "cycle " << issued.cycle << " request " << issued.request_id << " "
     << to_string(issued.command) << ": " << message;
  report.errors.push_back(os.str());
}

bool is_faw_event(Command command) {
  // 当前模型把 ACT/ACT1 和 per-bank/dual-bank refresh 都作为 activation-window
  // 事件。 这是保守近似，和 TimingEngine::record_activate() 保持一致。
  return command == Command::ACT || command == Command::ACT1 ||
         command == Command::REFPB || command == Command::REFDB;
}

bool is_data_command(Command command) {
  return command == Command::RD || command == Command::RDA ||
         command == Command::WR || command == Command::WRA;
}

bool is_falling_edge_pre(Command command) {
  return command == Command::PREPB || command == Command::PREAB;
}

int hbm_bank_key(const DramSpec &spec, const DecodedAddress &decoded) {
  return (decoded.sid * std::max(1, spec.org.bank_groups) +
          decoded.bank_group) *
             std::max(1, spec.org.banks_per_group) +
         decoded.bank;
}

std::size_t scope_slot(TimingScope scope) {
  return static_cast<std::size_t>(scope);
}

std::pair<std::size_t, std::size_t>
channel_bank_range(const DramSpec &spec, const DecodedAddress &decoded) {
  const std::size_t banks_per_channel =
      static_cast<std::size_t>(std::max(1, spec.banks_per_channel()));
  const int channel =
      std::clamp(decoded.channel, 0, std::max(1, spec.org.channels) - 1);
  const std::size_t begin =
      static_cast<std::size_t>(channel) * banks_per_channel;
  return {begin, std::min(scope_count(spec, TimingScope::Bank),
                          begin + banks_per_channel)};
}

bool any_bank_busy_in_channel(const DramSpec &spec,
                              const std::vector<ValidatorBankState> &banks,
                              const DecodedAddress &decoded) {
  const auto [begin, end] = channel_bank_range(spec, decoded);
  return std::any_of(
      banks.begin() + static_cast<std::ptrdiff_t>(begin),
      banks.begin() + static_cast<std::ptrdiff_t>(end),
      [](const ValidatorBankState &bank) { return bank.busy(); });
}

std::pair<std::size_t, std::size_t>
rank_bank_range(const DramSpec &spec, const DecodedAddress &decoded) {
  // PREab/REFab/RFMab 的 all-bank 指目标 rank 中的全部 bank。使用 Rank
  // scope 索引可同时保留 channel/PC/SID 层级，避免多 rank trace 自洽地误关
  // 其他 rank。
  const std::size_t banks_per_rank = static_cast<std::size_t>(
      std::max(1, spec.org.bank_groups * spec.org.banks_per_group));
  const std::size_t begin =
      scope_index(spec, TimingScope::Rank, decoded) * banks_per_rank;
  const std::size_t end =
      std::min(scope_count(spec, TimingScope::Bank), begin + banks_per_rank);
  return {begin, end};
}

bool any_bank_busy_in_rank(const DramSpec &spec,
                           const std::vector<ValidatorBankState> &banks,
                           const DecodedAddress &decoded) {
  auto [begin, end] = rank_bank_range(spec, decoded);
  for (std::size_t i = begin; i < end; i++) {
    if (banks[i].busy()) {
      return true;
    }
  }
  return false;
}

bool any_bank_activating_in_rank(const DramSpec &spec,
                                 const std::vector<ValidatorBankState> &banks,
                                 const DecodedAddress &decoded) {
  const auto [begin, end] = rank_bank_range(spec, decoded);
  return std::any_of(
      banks.begin() + static_cast<std::ptrdiff_t>(begin),
      banks.begin() + static_cast<std::ptrdiff_t>(end),
      [](const ValidatorBankState &bank) { return bank.activating; });
}

void close_rank_banks(const DramSpec &spec,
                      std::vector<ValidatorBankState> &banks,
                      const DecodedAddress &decoded) {
  auto [begin, end] = rank_bank_range(spec, decoded);
  for (std::size_t i = begin; i < end; i++) {
    banks[i].open_row = -1;
    banks[i].activating = false;
    banks[i].activating_row = -1;
    banks[i].activating_request = 0;
  }
}

void validate_hbm_edge_pairing_matrix(const DramSpec &spec,
                                      const ValidatorHbmPairingState &pairing,
                                      CommandValidationReport &report,
                                      const IssuedCommand &issued) {
  if (!spec.hbm_strict_edge_pairing || !is_falling_edge_pre(issued.command)) {
    return;
  }
  report.edge_pairing_checks++;
  if (issued.cycle != pairing.next_pairing_falling_edge ||
      issued.decoded.pseudo_channel != pairing.pseudo_channel) {
    return;
  }

  const auto &rising_meta = command_meta(pairing.command);
  bool legal = true;
  if (rising_meta.all_bank) {
    legal = false;
  } else {
    legal = issued.command == Command::PREPB &&
            hbm_bank_key(spec, issued.decoded) != pairing.bank_key;
  }

  if (!legal) {
    add_error(report, issued, "violates HBM edge pairing matrix");
  }
}

void record_hbm_rising_row_command(const DramSpec &spec,
                                   ValidatorHbmPairingState &pairing,
                                   const IssuedCommand &issued) {
  pairing.command = issued.command;
  pairing.pseudo_channel = issued.decoded.pseudo_channel;
  pairing.bank_key = command_meta(issued.command).all_bank
                         ? -1
                         : hbm_bank_key(spec, issued.decoded);
  pairing.next_pairing_falling_edge =
      issued.cycle + (issued.command == Command::ACT ? 3 : 1);
}

void validate_and_replay_bank_state(
    const DramSpec &spec, std::vector<ValidatorBankState> &banks,
    std::vector<ValidatorScopeState> &channel_states,
    CommandValidationReport &report, const IssuedCommand &issued) {
  // 对每条命令先检查状态前置条件，再按命令副作用更新 replay 状态。
  // 即使发现错误也继续更新能确定的状态，目的是在一次报告中暴露更多问题。
  std::size_t idx = scope_index(spec, TimingScope::Bank, issued.decoded);
  ValidatorBankState &bank = banks[idx];
  ValidatorScopeState &channel_state =
      channel_states[scope_index(spec, TimingScope::Channel, issued.decoded)];
  auto require_state = [&](bool condition, const std::string &message) {
    if (!condition) {
      add_error(report, issued, message);
    }
  };

  switch (issued.command) {
  case Command::ACT:
    require_state(!spec.split_activate,
                  "ACT is illegal for split-activate standards");
    require_state(!bank.busy(), "ACT requires a closed target bank");
    bank.open_row = issued.decoded.row;
    bank.activating = false;
    bank.activating_row = -1;
    bank.activating_request = 0;
    break;
  case Command::ACT1:
    require_state(spec.split_activate,
                  "ACT1 is illegal for non split-activate standards");
    require_state(!bank.busy(), "ACT1 requires a closed target bank");
    require_state(
        !any_bank_activating_in_rank(spec, banks, issued.decoded),
        "ACT1 requires the previous ACT1 in this rank to complete ACT2");
    bank.open_row = -1;
    bank.activating = true;
    bank.activating_row = issued.decoded.row;
    bank.activating_request = issued.request_id;
    bank.next_act2 =
        issued.cycle + timing_delay(spec, std::max(1, spec.timing.nAADMin));
    bank.act2_deadline =
        issued.cycle +
        timing_delay(spec, std::max(spec.timing.nAADMin, spec.timing.nAADMax));
    break;
  case Command::ACT2:
    require_state(spec.split_activate,
                  "ACT2 is illegal for non split-activate standards");
    require_state(bank.activating, "ACT2 requires an activating target bank");
    require_state(bank.activating_request == issued.request_id,
                  "ACT2 must belong to the request that issued ACT1");
    require_state(bank.activating_row == issued.decoded.row,
                  "ACT2 row does not match the row selected by ACT1");
    require_state(issued.cycle >= bank.next_act2,
                  "ACT2 issued before nAADMin earliest cycle");
    require_state(issued.cycle <= bank.act2_deadline,
                  "ACT2 issued after nAADMax deadline");
    bank.activating = false;
    bank.open_row = issued.decoded.row;
    bank.activating_row = -1;
    bank.activating_request = 0;
    bank.next_act2 = 0;
    bank.act2_deadline = 0;
    break;
  case Command::PRE:
  case Command::PREPB:
    require_state(!bank.activating && bank.open_row >= 0,
                  "PREpb requires an opened, non-activating target bank");
    bank.open_row = -1;
    bank.activating = false;
    bank.activating_row = -1;
    bank.activating_request = 0;
    break;
  case Command::PREAB:
    require_state(any_bank_busy_in_rank(spec, banks, issued.decoded),
                  "PREab requires at least one busy bank in its target rank");
    close_rank_banks(spec, banks, issued.decoded);
    break;
  case Command::CASRD:
  case Command::CASWR:
    require_state(spec.lpddr_family,
                  "CAS_RD/CAS_WR are only legal for LPDDR-family standards");
    require_state(bank.opened_row(issued.decoded.row),
                  "CAS_RD/CAS_WR require an opened target row");
    require_state(!channel_state.wck_retrain_required,
                  "CAS_RD/CAS_WR require WCK_TRAIN after DVFS");
    break;
  case Command::RD:
  case Command::WR:
    require_state(bank.opened_row(issued.decoded.row),
                  "RD/WR require an opened target row");
    require_state(!channel_state.wck_retrain_required,
                  "RD/WR require WCK_TRAIN after DVFS");
    break;
  case Command::RDA:
  case Command::WRA:
    require_state(bank.opened_row(issued.decoded.row),
                  "RDA/WRA require an opened target row");
    require_state(!channel_state.wck_retrain_required,
                  "RDA/WRA require WCK_TRAIN after DVFS");
    bank.open_row = -1;
    break;
  case Command::REFPB:
  case Command::RFMPB:
    require_state(!bank.busy(),
                  "per-bank REF/RFM requires a closed target bank");
    break;
  case Command::REFDB: {
    require_state(spec.lpddr_dual_bank_refresh,
                  "REFdb requires LPDDR dual-bank refresh support");
    DecodedAddress partner = lpddr_refdb_partner(spec, issued.decoded);
    const ValidatorBankState &partner_bank =
        banks[scope_index(spec, TimingScope::Bank, partner)];
    require_state(!bank.busy(), "REFdb requires the target bank to be closed");
    require_state(!partner_bank.busy(),
                  "REFdb requires the paired bank to be closed");
    break;
  }
  case Command::REFAB:
  case Command::RFMAB:
    // all-bank refresh/RFM 不能在目标 rank 内仍有打开行或 split-activate
    // 未完成时发生。 Controller 在线路径会先插入 PREab；Validator
    // 用这条规则检查 trace 是否真的做到。
    require_state(
        !any_bank_busy_in_rank(spec, banks, issued.decoded),
        "all-bank REF/RFM requires all banks in the target rank to be idle");
    break;
  case Command::MRW:
  case Command::MRR:
    require_state(!any_bank_busy_in_channel(spec, banks, issued.decoded),
                  "MRW/MRR require all banks in the channel to be idle");
    require_state(
        !channel_state.power_down && !channel_state.self_refresh,
        "MRW/MRR cannot be issued while the channel is in low power state");
    break;
  case Command::WCKSYNC:
    require_state(spec.lpddr_family, "WCK_SYNC requires LPDDR-family standard");
    require_state(!channel_state.self_refresh,
                  "WCK_SYNC cannot be issued in self-refresh");
    require_state(!channel_state.wck_retrain_required,
                  "WCK_SYNC cannot replace required WCK_TRAIN after DVFS");
    break;
  case Command::WCKTRAIN:
    require_state(spec.lpddr_family,
                  "WCK_TRAIN requires LPDDR-family standard");
    require_state(!any_bank_busy_in_channel(spec, banks, issued.decoded),
                  "WCK_TRAIN requires all banks in the channel to be idle");
    require_state(
        !channel_state.power_down && !channel_state.self_refresh,
        "WCK_TRAIN cannot be issued while the channel is in low power state");
    channel_state.wck_retrain_required = false;
    break;
  case Command::DVFS:
    require_state(spec.lpddr_family,
                  "DVFS transition requires LPDDR-family standard");
    require_state(
        !any_bank_busy_in_channel(spec, banks, issued.decoded),
        "DVFS transition requires all banks in the channel to be idle");
    if (spec.lpddr_requires_wck_retrain_after_dvfs()) {
      channel_state.wck_retrain_required = true;
    }
    break;
  case Command::PDE:
    require_state(!any_bank_busy_in_channel(spec, banks, issued.decoded),
                  "PDE requires all banks in the channel to be idle");
    require_state(!channel_state.power_down && !channel_state.self_refresh,
                  "PDE requires active state");
    channel_state.power_down = true;
    break;
  case Command::PDX:
    require_state(channel_state.power_down, "PDX requires power-down state");
    channel_state.power_down = false;
    break;
  case Command::SREFEN:
    require_state(!any_bank_busy_in_channel(spec, banks, issued.decoded),
                  "SREFEN requires all banks in the channel to be idle");
    require_state(!channel_state.self_refresh, "SREFEN requires active state");
    channel_state.self_refresh = true;
    break;
  case Command::SREFEX:
    require_state(channel_state.self_refresh,
                  "SREFEX requires self-refresh state");
    channel_state.self_refresh = false;
    break;
  case Command::ECCSCRUB:
    require_state(spec.supports_ecc || spec.lpddr_link_ecc_enabled,
                  "ECC_SCRUB requires ECC/link-ECC support");
    require_state(!any_bank_busy_in_channel(spec, banks, issued.decoded),
                  "ECC_SCRUB requires all banks in the channel to be idle");
    break;
  case Command::RASERR:
    require_state(spec.supports_ecc || spec.lpddr_link_ecc_enabled ||
                      spec.hbm_link_retry_enabled || spec.lpddr_link_protection,
                  "RAS_ERR requires RAS/ECC/link retry support");
    break;
  case Command::NOP:
    add_error(report, issued, "NOP should not appear in issued command trace");
    break;
  }
}

} // namespace

static CommandValidationReport
validate_single_stack_trace(const DramSpec &spec,
                            const std::vector<IssuedCommand> &trace) {
  CommandValidationReport report;
  // bank_states 重放命令语义；constraint_scopes 重放表驱动
  // timing；activation_scopes 重放 tFAW；wck_scopes 重放 LPDDR WCK
  // 窗口。四者共同覆盖协议合法性。
  std::vector<ValidatorBankState> bank_states(
      scope_count(spec, TimingScope::Bank));
  std::array<std::vector<ValidatorScopeState>, 6> constraint_scopes;
  for (TimingScope scope :
       {TimingScope::Channel, TimingScope::PseudoChannel, TimingScope::Sid,
        TimingScope::Rank, TimingScope::BankGroup, TimingScope::Bank}) {
    constraint_scopes[scope_slot(scope)].assign(scope_count(spec, scope), {});
  }
  std::vector<ValidatorScopeState> activation_scopes(
      scope_count(spec, spec.activation_scope));
  std::vector<ValidatorScopeState> wck_scopes(
      scope_count(spec, spec.wck_scope));
  std::vector<ValidatorScopeState> channel_states(
      scope_count(spec, TimingScope::Channel));
  std::vector<ValidatorHbmPairingState> hbm_pairing_states(
      scope_count(spec, TimingScope::Channel));
  // Independent replay: no calls to the online TimingEngine.
  std::vector<int> refresh_bank_count(scope_count(spec, TimingScope::Rank), 0);
  std::vector<Cycle> refresh_next(scope_count(spec, TimingScope::Rank), 0);
  std::vector<std::set<int>> refreshed_banks(scope_count(spec, TimingScope::Rank));

  auto scope_state_for =
      [&](TimingScope scope,
          const DecodedAddress &decoded) -> ValidatorScopeState & {
    return constraint_scopes[scope_slot(scope)]
                            [scope_index(spec, scope, decoded)];
  };

  for (const auto &issued : trace) {
    report.checked_commands++;
    if (!decoded_in_range(spec, issued)) {
      add_error(report, issued,
                "contains an out-of-range stack or DRAM coordinate");
      // scope_index() 的夹紧只用于内部数组防御；非法外部事件不能重放到
      // 最近的合法 bank，否则坏 trace 可能被错误地判定为通过。
      continue;
    }
    const auto &meta = command_meta(issued.command);
    if (spec.lpddr_dual_bank_refresh) {
      const auto rank = scope_index(spec, TimingScope::Rank, issued.decoded);
      if (issued.command == Command::REFDB) {
        report.timing_constraint_checks++;
        if (issued.cycle < refresh_next[rank])
          add_error(report, issued, "violates REFdb refresh-counter S/L interval");
        const int pairs_per_row = spec.org.bank_groups * spec.org.banks_per_group / 2;
        const auto partner = lpddr_refdb_partner(spec, issued.decoded);
        for (int bg : {issued.decoded.bank_group, partner.bank_group}) {
          const int bank = bg * spec.org.banks_per_group + issued.decoded.bank;
          if (!refreshed_banks[rank].insert(bank).second)
            add_error(report, issued, "REFdb repeats a bank before completing the refresh-row sweep");
        }
        ++refresh_bank_count[rank];
        const bool next_row = refresh_bank_count[rank] == pairs_per_row;
        if (next_row) {
          refresh_bank_count[rank] = 0;
          refreshed_banks[rank].clear();
        }
        int gap = spec.timing.nREFDB2REFDBS > 0
                      ? spec.timing.nREFDB2REFDBS : spec.timing.nRRDL;
        if (next_row && spec.timing.nREFDB2REFDBL > 0)
          gap = spec.timing.nREFDB2REFDBL;
        refresh_next[rank] = issued.cycle + timing_delay(spec, gap);
      } else if (issued.command == Command::REFAB) {
        refresh_bank_count[rank] = 0;
        refreshed_banks[rank].clear();
        refresh_next[rank] = issued.cycle + timing_delay(spec, spec.timing.nRFC);
      } else if (issued.command == Command::SREFEX) {
        const std::size_t ranks_per_channel =
            spec.org.pseudo_channels * spec.org.sids * spec.org.ranks;
        const std::size_t first = issued.decoded.channel * ranks_per_channel;
        for (std::size_t i = first; i < first + ranks_per_channel; ++i) {
          refresh_bank_count[i] = 0;
          refresh_next[i] = 0;
          refreshed_banks[i].clear();
        }
      }
    }

    if (spec.dual_command_bus) {
      report.bus_checks++;
      // 双总线模式要求命令类型和 bus 字段一致。这个检查能捕捉 Controller
      // 错把 row 命令放到 column bus 或把 HBM 命令放到 unified bus 的问题。
      if (issued.bus == BusClass::Row && !meta.row_command) {
        add_error(report, issued, "non-row command issued on row bus");
      }
      if (issued.bus == BusClass::Column && !meta.column_command) {
        add_error(report, issued, "non-column command issued on column bus");
      }
      if (issued.bus == BusClass::Unified) {
        add_error(report, issued,
                  "HBM dual-command mode issued command on unified bus");
      }
    }

    if (spec.hbm_edge_pairing) {
      report.edge_checks++;
      // HBM edge pairing 检查使用 trace cycle 奇偶：当前模型约定奇数 tick 为
      // rising， 偶数 tick 为 falling。falling edge 只能用于受限
      // PRE；严格模式还会 重放上一条 rising row command 留下的 pairing window。
      if (meta.column_command && (issued.cycle % 2) == 0) {
        add_error(report, issued, "column command issued on falling edge");
      }
      if ((issued.cycle % 2) == 0 && issued.command != Command::PREPB &&
          issued.command != Command::PREAB) {
        add_error(report, issued, "falling-edge command is not PREpb/PREab");
      }
      auto &pairing_state = hbm_pairing_states[scope_index(
          spec, TimingScope::Channel, issued.decoded)];
      if ((issued.cycle % 2) == 0) {
        validate_hbm_edge_pairing_matrix(spec, pairing_state, report, issued);
      } else if (issued.bus == BusClass::Row && meta.row_command) {
        record_hbm_rising_row_command(spec, pairing_state, issued);
      }
    }

    report.state_checks++;
    validate_and_replay_bank_state(spec, bank_states, channel_states, report,
                                   issued);

    for (const auto &constraint : spec.timing_constraints) {
      if (constraint.window > 0 ||
          !contains_command(constraint.following, issued.command)) {
        continue;
      }
      report.timing_constraint_checks++;
      ValidatorScopeState &state =
          scope_state_for(constraint.scope, issued.decoded);
      Cycle ready = state.next_command[command_index(issued.command)];
      if (issued.command == Command::REFDB && spec.lpddr_dual_bank_refresh &&
          constraint.scope == TimingScope::Bank)
        ready = std::max(ready, scope_state_for(TimingScope::Bank,
                         lpddr_refdb_partner(spec, issued.decoded))
                         .next_command[command_index(issued.command)]);
      if (issued.cycle < ready) {
        // 先检查 ready，再应用当前命令的 preceding 约束，等价于在线路径中的
        // timing_ok() -> issue() -> apply_constraints() 顺序。
        std::ostringstream os;
        os << "violates table timing constraint before cycle " << ready;
        add_error(report, issued, os.str());
      }
    }

    if (is_faw_event(issued.command)) {
      report.faw_events_checked++;
      ValidatorScopeState &state = activation_scopes[scope_index(
          spec, spec.activation_scope, issued.decoded)];
      Cycle window = timing_delay(spec, std::max(1, spec.timing.nFAW));
      while (!state.recent_acts.empty() &&
             issued.cycle >= state.recent_acts.front() &&
             issued.cycle - state.recent_acts.front() >= window) {
        state.recent_acts.pop_front();
      }
      if (state.recent_acts.size() >= 4) {
        // 当前命令加入前窗口内已有 4 个事件，说明这条命令会成为第五个，违反
        // tFAW。
        add_error(report, issued, "violates tFAW activation window");
      }
      state.recent_acts.push_back(issued.cycle);
    }

    if (spec.lpddr_family && spec.lpddr_wck_mode != LpddrWckMode::AlwaysOn) {
      ValidatorScopeState &wck =
          wck_scopes[scope_index(spec, spec.wck_scope, issued.decoded)];
      if (is_data_command(issued.command) &&
          !(issued.cycle >= wck.wck_ready_at &&
            issued.cycle < wck.wck_active_until)) {
        report.wck_window_checks++;
        // LPDDR 的 RD/WR 不只要求 bank row-hit，还必须落在 WCK ready window。
        // 这个检查专门捕捉 CAS_RD/CAS_WR 过早、过晚或缺失的问题。
        add_error(report, issued,
                  "LPDDR data command outside WCK ready window");
      } else if (is_data_command(issued.command)) {
        report.wck_window_checks++;
      }
      if (issued.command == Command::CASRD ||
          issued.command == Command::CASWR ||
          issued.command == Command::WCKSYNC) {
        // CAS 命令建立初始 WCK window；后续 RD/WR 会把 active_until 延长到
        // 覆盖数据 burst 和 postamble。
        const int sync_latency = issued.command == Command::WCKSYNC
                                     ? spec.timing.nWCKSYNC
                                     : spec.timing.nWCK2CK;
        wck.wck_ready_at =
            issued.cycle + timing_delay(spec, std::max(1, sync_latency));
        wck.wck_active_until =
            issued.cycle +
            timing_delay(spec, std::max(spec.timing.nWCKPST, sync_latency + 1));
      } else if (issued.command == Command::WCKTRAIN) {
        wck.wck_ready_at =
            issued.cycle +
            timing_delay(spec, std::max(1, spec.timing.nWCKTRAIN));
        wck.wck_active_until =
            wck.wck_ready_at +
            timing_delay(spec, std::max(1, spec.timing.nWCKPST));
      } else if (issued.command == Command::RD ||
                 issued.command == Command::RDA) {
        wck.wck_active_until =
            std::max(wck.wck_active_until,
                     issued.cycle +
                         timing_delay(spec, spec.timing.nCL + spec.timing.nBL +
                                                spec.timing.nWCKPST));
      } else if (issued.command == Command::WR ||
                 issued.command == Command::WRA) {
        wck.wck_active_until =
            std::max(wck.wck_active_until,
                     issued.cycle +
                         timing_delay(spec, spec.timing.nCWL + spec.timing.nBL +
                                                spec.timing.nWCKPST));
      }
    }

    for (const auto &constraint : spec.timing_constraints) {
      if (constraint.window > 0 ||
          !contains_command(constraint.preceding, issued.command)) {
        continue;
      }
      const Cycle ready =
          issued.cycle + timing_delay(spec, std::max(0, constraint.latency));
      // 应用 preceding->following 约束，为后续命令写入最早发射 cycle。
      report.timing_constraint_updates++;
      auto update = [&](ValidatorScopeState &state) {
        for (Command following : constraint.following) {
          const auto idx = command_index(following);
          state.next_command[idx] = std::max(state.next_command[idx], ready);
        }
      };
      if (!constraint.sibling) {
        update(scope_state_for(constraint.scope, issued.decoded));
        if (issued.command == Command::REFDB && spec.lpddr_dual_bank_refresh &&
            constraint.scope == TimingScope::Bank)
          update(scope_state_for(TimingScope::Bank,
                                 lpddr_refdb_partner(spec, issued.decoded)));
      } else {
        auto &scopes = constraint_scopes[scope_slot(constraint.scope)];
        const std::size_t current =
            scope_index(spec, constraint.scope, issued.decoded);
        const auto [begin, end] =
            sibling_scope_range(spec, constraint.scope, issued.decoded);
        for (std::size_t index = begin; index < end; ++index) {
          if (index != current)
            update(scopes[index]);
        }
      }
    }
  }

  for (const auto &bank : bank_states) {
    if (bank.activating) {
      report.errors.push_back(
          "trace ended with ACT1 still waiting for mandatory ACT2");
      break;
    }
  }

  if (report.errors.size() == kMaxValidationErrors) {
    report.errors.push_back("validation stopped after too many errors");
  }
  return report;
}

CommandValidationReport
validate_command_trace(const DramSpec &spec,
                       const std::vector<IssuedCommand> &trace) {
  validate_spec(spec);
  // JEDEC 状态和 timing 约束都止于 stack 边界；逐 stack 重放可避免独立
  // stack 在同一拍访问相同局部坐标时产生伪冲突。
  std::vector<int> stack_ids;
  for (const auto &command : trace) {
    if (std::find(stack_ids.begin(), stack_ids.end(), command.stack_id) ==
        stack_ids.end()) {
      stack_ids.push_back(command.stack_id);
    }
  }
  if (stack_ids.empty())
    stack_ids.push_back(0);

  CommandValidationReport total;
  for (int stack_id : stack_ids) {
    std::vector<IssuedCommand> local;
    for (const auto &command : trace) {
      if (command.stack_id == stack_id)
        local.push_back(command);
    }
    CommandValidationReport part = validate_single_stack_trace(spec, local);
    total.checked_commands += part.checked_commands;
    total.bus_checks += part.bus_checks;
    total.edge_checks += part.edge_checks;
    total.edge_pairing_checks += part.edge_pairing_checks;
    total.state_checks += part.state_checks;
    total.timing_constraint_checks += part.timing_constraint_checks;
    total.timing_constraint_updates += part.timing_constraint_updates;
    total.faw_events_checked += part.faw_events_checked;
    total.wck_window_checks += part.wck_window_checks;
    for (const auto &error : part.errors) {
      total.errors.push_back("stack " + std::to_string(stack_id) + ": " +
                             error);
    }
  }
  return total;
}

} // namespace hbm_sim
