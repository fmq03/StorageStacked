// 命令合法性状态机：只回答“当前 bank/维护状态下这条命令是否语义合法”。
// timing ready、总线占用、调度优先级由其他模块处理，避免把状态合法性和时序混在一起。
#include "hbm_sim/dram/state.hpp"

namespace hbm_sim {

CommandStateResult check_command_state(const CommandStateSnapshot& state, Command cmd) {
  // 这里刻意只用 CommandStateSnapshot，不读取 BankState/TimingEngine。这样同一套
  // 状态机既能服务在线 Controller，也能服务离线 Validator 或未来插件。
  switch (cmd) {
    case Command::ACT:
      // 非 split 标准中，ACT 只能从 closed bank 发出。
      return {!state.split_activate && state.bank_state == BankProtocolState::Closed,
              "ACT requires a closed non-split bank"};
    case Command::ACT1:
      // LPDDR split activate 第一阶段，同样要求 bank 完全 closed。
      return {state.split_activate && state.bank_state == BankProtocolState::Closed,
              "ACT1 requires a closed split-activate bank"};
    case Command::ACT2:
      // ACT2 必须属于发过 ACT1 的 owning request；仅有 bank=Activating 不够。
      return {state.split_activate && state.bank_state == BankProtocolState::Activating &&
                  state.issued_first_activate,
              "ACT2 requires the owning ACT1 request"};
    case Command::PRE:
    case Command::PREPB:
      return {state.bank_state == BankProtocolState::Opened, "PREpb requires an opened bank"};
    case Command::PREAB:
      return {state.any_bank_busy, "PREab requires at least one busy bank"};
    case Command::CASRD:
    case Command::CASWR:
      // CAS 不是通用列命令，只在 LPDDR row-hit 后用于准备 WCK/data 命令。
      return {state.lpddr_family && state.bank_state == BankProtocolState::Opened && state.row_hit &&
                  !state.wck_training_required,
              "CAS requires an opened LPDDR row and no pending WCK training"};
    case Command::RD:
    case Command::WR:
    case Command::RDA:
    case Command::WRA:
      // HBM row-hit 后可以直接 RD/WR；LPDDR 还必须等待 WCK ready window。
      return {state.bank_state == BankProtocolState::Opened && state.row_hit &&
                  (!state.lpddr_family || (state.wck_ready && !state.wck_training_required)),
              "RD/WR requires row hit and, for LPDDR, trained active WCK"};
    case Command::REFPB:
    case Command::REFDB:
    case Command::RFMPB:
      // per-bank 维护是内部维护请求，且要求目标 bank closed。普通 frontend 请求
      // 不能直接生成 REF/RFM。
      return {state.maintenance && state.bank_state == BankProtocolState::Closed,
              "per-bank maintenance requires a closed target bank"};
    case Command::REFAB:
    case Command::RFMAB:
      // all-bank 维护要求作用域内所有 bank idle；Controller 会在不满足时先插入 PREab。
      return {state.maintenance && !state.any_bank_busy,
              "all-bank maintenance requires all banks idle"};
    case Command::MRW:
    case Command::MRR:
      return {state.maintenance && !state.any_bank_busy && !state.low_power_active &&
                  !state.self_refresh_active && !state.training_active && !state.dvfs_transition_active,
              "mode register access requires an idle channel outside low-power/training/DVFS"};
    case Command::WCKSYNC:
      return {state.lpddr_family && !state.low_power_active && !state.self_refresh_active &&
                  !state.training_active && !state.dvfs_transition_active &&
                  !state.wck_training_required,
              "WCK sync requires LPDDR mode outside low-power/training/DVFS and no pending WCK training"};
    case Command::WCKTRAIN:
      return {state.lpddr_family && state.maintenance && !state.any_bank_busy &&
                  !state.low_power_active && !state.self_refresh_active && !state.dvfs_transition_active,
              "WCK training requires an idle LPDDR channel"};
    case Command::DVFS:
      return {state.lpddr_family && state.maintenance && !state.any_bank_busy &&
                  !state.low_power_active && !state.self_refresh_active && !state.training_active,
              "DVFS transition requires an idle LPDDR channel"};
    case Command::PDE:
      return {state.maintenance && !state.any_bank_busy && !state.low_power_active &&
                  !state.self_refresh_active && !state.training_active && !state.dvfs_transition_active,
              "power-down entry requires an idle active channel"};
    case Command::PDX:
      return {state.maintenance && state.low_power_active && !state.self_refresh_active,
              "power-down exit requires power-down state"};
    case Command::SREFEN:
      return {state.maintenance && !state.any_bank_busy && !state.self_refresh_active &&
                  !state.training_active && !state.dvfs_transition_active,
              "self-refresh entry requires an idle active channel"};
    case Command::SREFEX:
      return {state.maintenance && state.self_refresh_active,
              "self-refresh exit requires self-refresh state"};
    case Command::ECCSCRUB:
      return {state.maintenance && state.ras_ecc_supported && !state.any_bank_busy &&
                  !state.low_power_active && !state.self_refresh_active,
              "ECC scrub requires RAS/ECC support and an idle channel"};
    case Command::RASERR:
      return {state.maintenance && (state.ras_ecc_supported || state.link_retry_supported) &&
                  !state.training_active && !state.dvfs_transition_active,
              "RAS/link recovery requires RAS/ECC or link retry support"};
    case Command::NOP:
      return {false, "NOP is not issuable"};
  }
  return {false, "unknown command"};
}

}  // namespace hbm_sim
