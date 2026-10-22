// RefreshManager：维护 JEDEC refresh 节奏和目标 bank/dual-bank/all-bank 轮转。
// 它只生成维护命令请求；是否需要 PRE、何时真正发 REF，仍由 Controller 的
// priority path、CommandState 和 TimingEngine 统一裁决。
#include "hbm_sim/controller/refresh.hpp"

#include <algorithm>

#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {

void RefreshManager::reset(const DramSpec &spec, Cycle clk) {
  validate_spec(spec);
  rank_cursor_ = 0;
  // per-bank refresh 用 nREFIpb；all-bank refresh 用 nREFI。若某个 preset 没有
  // 给 nREFIpb，则回退到 nREFI，保证 refresh manager 不因未建 per-bank
  // 表而崩溃。
  int interval = refresh_interval(spec);
  rank_states_.assign(static_cast<std::size_t>(std::max(1, spec.org.ranks)),
                      {});
  for (auto &state : rank_states_) {
    state.next_refresh_cycle = clk + timing_delay(spec, interval);
  }
}

RefreshTickResult RefreshManager::tick(const DramSpec &spec, Cycle clk,
                                       bool prefer_postpone,
                                       bool allow_pull_in) {
  RefreshTickResult result;
  int interval = refresh_interval(spec);
  if (!spec.supports_refresh || interval <= 0) {
    return result;
  }

  if (rank_states_.size() !=
      static_cast<std::size_t>(std::max(1, spec.org.ranks))) {
    reset(spec, clk);
  }

  const Cycle interval_ticks = timing_delay(spec, interval);
  std::vector<bool> newly_due(rank_states_.size(), false);
  // deadline 是周期性义务来源，而不是“下一次允许发 refresh”的 gate。每跨过
  // 一个 deadline 就增加一项 obligation；提前刷新留下的负 credit 会在这里
  // 自然抵消，推迟刷新留下的正 credit 则一直保留到真正发出命令。
  for (std::size_t rank = 0; rank < rank_states_.size(); ++rank) {
    auto &state = rank_states_[rank];
    while (clk >= state.next_refresh_cycle) {
      state.credit++;
      state.next_refresh_cycle += interval_ticks;
      newly_due[rank] = true;
    }
    if (state.credit >= 0)
      state.pullin_count = 0;
  }

  const int credit_limit = std::max(0, spec.refresh_credit_limit);
  for (std::size_t offset = 0; offset < rank_states_.size(); ++offset) {
    const int rank = (rank_cursor_ + static_cast<int>(offset)) %
                     static_cast<int>(rank_states_.size());
    auto &state = rank_states_[static_cast<std::size_t>(rank)];
    if (state.credit <= 0)
      continue;

    const bool can_postpone =
        newly_due[static_cast<std::size_t>(rank)] && prefer_postpone &&
        spec.refresh_postpone_limit > 0 &&
        state.postpone_count < spec.refresh_postpone_limit &&
        (credit_limit == 0 || state.credit <= credit_limit);
    if (can_postpone) {
      state.postpone_count++;
      result.postponed = true;
      result.credit = aggregate_credit();
      rank_cursor_ = (rank + 1) % static_cast<int>(rank_states_.size());
      return result;
    }

    // 已 postpone 的 obligation 在普通工作结束或达到上限后偿还。若仍在两个
    // deadline 之间且普通工作没有结束，则继续等待，不把每个 tick 重复计为
    // postpone。
    if (prefer_postpone && !newly_due[static_cast<std::size_t>(rank)] &&
        state.postpone_count > 0 &&
        state.postpone_count < spec.refresh_postpone_limit) {
      continue;
    }

    result.started_batch = true;
    result.commands = seed_refresh_batch(spec, rank, state);
    state.credit--;
    if (state.credit <= 0)
      state.postpone_count = 0;
    result.credit = aggregate_credit();
    rank_cursor_ = (rank + 1) % static_cast<int>(rank_states_.size());
    return result;
  }

  if (allow_pull_in && spec.refresh_pullin_limit > 0) {
    for (std::size_t offset = 0; offset < rank_states_.size(); ++offset) {
      const int rank = (rank_cursor_ + static_cast<int>(offset)) %
                       static_cast<int>(rank_states_.size());
      auto &state = rank_states_[static_cast<std::size_t>(rank)];
      const bool in_window =
          clk + timing_delay(spec, std::max(1, interval / 2)) >=
          state.next_refresh_cycle;
      if (state.credit == 0 && in_window &&
          state.pullin_count < spec.refresh_pullin_limit) {
        result.started_batch = true;
        result.pulled_in = true;
        result.commands = seed_refresh_batch(spec, rank, state);
        state.credit--;
        state.pullin_count++;
        result.credit = aggregate_credit();
        rank_cursor_ = (rank + 1) % static_cast<int>(rank_states_.size());
        return result;
      }
    }
  }

  result.credit = aggregate_credit();
  return result;
}

Cycle RefreshManager::timing_delay(const DramSpec &spec, int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) *
         static_cast<Cycle>(std::max(1, spec.tick_multiplier));
}

int RefreshManager::refresh_interval(const DramSpec &spec) const {
  int interval =
      spec.refresh_policy == MaintenancePolicyKind::AllBank
          ? spec.timing.nREFI
          : (spec.timing.nREFIpb > 0 ? spec.timing.nREFIpb : spec.timing.nREFI);
  if (spec.refresh_temperature_mode != RefreshTemperatureMode::Normal) {
    interval =
        std::max(1, interval / std::max(1, spec.refresh_high_temp_multiplier));
  }
  return interval;
}

std::vector<MaintenanceCommand>
RefreshManager::seed_refresh_batch(const DramSpec &spec, int rank,
                                   RankRefreshState &state) {
  std::vector<MaintenanceCommand> commands;
  if (spec.refresh_policy == MaintenancePolicyKind::AllBank) {
    // Controller 的 spec 已在 MemorySystem 中本地化为单 channel，但一个
    // controller 仍可能包含多个 pseudo-channel/SID。CommandState 对 REFab
    // 的 rank scope 也包含 PC/SID，因此每个 scope 都需要一条命令；只生成
    // PC0/SID0 会遗漏其余 bank。
    for (int pc = 0; pc < std::max(1, spec.org.pseudo_channels); ++pc) {
      for (int sid = 0; sid < std::max(1, spec.org.sids); ++sid) {
        DecodedAddress d;
        d.pseudo_channel = pc;
        d.sid = sid;
        d.rank = rank;
        commands.push_back(MaintenanceCommand{Command::REFAB, d});
      }
    }
    return commands;
  }

  const int bank_groups = std::max(1, spec.org.bank_groups);
  const int banks_per_group = std::max(1, spec.org.banks_per_group);
  const int positions_per_sid =
      spec.lpddr_dual_bank_refresh
          ? std::max(1, bank_groups / 2) * banks_per_group
          : bank_groups * banks_per_group;
  int sid_count = std::max(1, spec.org.sids);
  int flat = state.flat_bank_cursor % positions_per_sid;
  int sid = state.sid_cursor % sid_count;

  // per-bank/dual-bank refresh 轮转顺序：
  // 1. 在当前 SID 内按 flat bank 递增；
  // 2. 一个 SID 内所有 bank 轮完后切到下一个 SID；
  // 3. 每个 refresh tick 对所有 channel/pseudo-channel 的同一 bank
  // 位置发一批命令。
  //
  // 这种设计让 refresh 行为可预测，便于测试，也接近 JEDEC 表中按 bank
  // 地址轮转的思路。
  for (int ch = 0; ch < std::max(1, spec.org.channels); ch++) {
    for (int pc = 0; pc < std::max(1, spec.org.pseudo_channels); pc++) {
      DecodedAddress d;
      d.channel = ch;
      d.pseudo_channel = pc;
      d.sid = sid;
      d.rank = rank;
      if (spec.lpddr_dual_bank_refresh) {
        // REFdb 的两个目标共享 BA，第二目标用 dBG 编码。基线轮转表为
        // BG0<->BG1、BG2<->BG3；这里只生成偶数 BG 的 primary，避免同一
        // pair 在下一次 deadline 被反向重复刷新。
        d.bank_group = (flat / banks_per_group) * 2;
        d.bank = flat % banks_per_group;
      } else {
        d.bank_group = flat / banks_per_group;
        d.bank = flat % banks_per_group;
      }
      commands.push_back(MaintenanceCommand{
          spec.lpddr_dual_bank_refresh ? Command::REFDB : Command::REFPB,
          d,
      });
    }
  }

  state.flat_bank_cursor++;
  if (state.flat_bank_cursor >= positions_per_sid) {
    state.flat_bank_cursor = 0;
    state.sid_cursor = (state.sid_cursor + 1) % sid_count;
  }
  return commands;
}

int RefreshManager::aggregate_credit() const {
  int total = 0;
  for (const auto &state : rank_states_)
    total += state.credit;
  return total;
}

} // namespace hbm_sim
