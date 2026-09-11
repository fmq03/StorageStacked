#pragma once

// DRAM 命令合法性状态机接口。它只判断状态语义，不判断 timing ready 或调度优先级。

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

enum class BankProtocolState {
  // 没有打开行，也没有未完成的 split activate。
  Closed,
  // LPDDR ACT1 已发出但 ACT2 尚未完成。
  Activating,
  // 已有一行处于 open 状态。
  Opened,
};

struct CommandStateSnapshot {
  // bank 的抽象协议状态。Controller/Validator 会从各自内部状态压缩到这里。
  BankProtocolState bank_state = BankProtocolState::Closed;
  // 当前请求的目标 row 是否等于 open row。RD/WR/CAS 必须 row_hit。
  bool row_hit = false;
  // LPDDR WCK 是否已经 ready。HBM 路径通常保持 true。
  bool wck_ready = true;
  // all-bank maintenance/PREab 需要知道作用域内是否还有 busy bank。
  bool any_bank_busy = false;
  // 当前标准是否使用 ACT1/ACT2。
  bool split_activate = false;
  // 当前标准是否需要 LPDDR CAS/WCK 语义。
  bool lpddr_family = false;
  // 当前请求是否为 Controller 内部维护请求。普通请求不能发 REF/RFM。
  bool maintenance = false;
  // 当前请求是否已经发出 ACT1。ACT2 必须属于 owning request。
  bool issued_first_activate = false;
  // 控制类状态：MR/training/DVFS/低功耗/RAS 命令不直接依赖单个 bank 的 open row，
  // 但必须知道 channel 是否处于低功耗、self-refresh、训练或 DVFS 切换窗口。
  bool low_power_active = false;
  bool self_refresh_active = false;
  bool training_active = false;
  bool dvfs_transition_active = false;
  // true 表示 LPDDR channel 已经历显式 DVFS，且当前 policy 要求重新 WCK_TRAIN
  // 后才能再次发 CAS/RD/WR。它不是“训练命令正在执行”，而是“训练债务未清”。
  bool wck_training_required = false;
  bool ras_ecc_supported = false;
  bool link_retry_supported = false;
};

struct CommandStateResult {
  // true 表示语义状态允许该命令；还需要 timing_ok/bus_matches 才能真正发射。
  bool legal = false;
  // 调试信息。当前在线路径只用 legal，validator/未来日志可打印 reason。
  const char* reason = "unknown";
};

// 只检查状态语义，不检查时序和总线。保持这个函数纯净，方便离线 validator 重用。
CommandStateResult check_command_state(const CommandStateSnapshot& state, Command cmd);

}  // namespace hbm_sim
