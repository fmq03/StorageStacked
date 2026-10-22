#pragma once

// Controller 内部 bank-local 状态。该头只描述单个 bank 的打开行和局部 timing gate；
// 跨 bank/group/channel 的 timing 状态放在 TimingEngine 中，二者职责不要混用。

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

// 每个物理 bank 的局部状态。next_* 字段只保存真正 bank-private 的约束；
// 跨 bank/group/channel 的约束放到 TimingEngine，避免作用域混在一起。
struct BankState {
  // -1 表示 closed bank；非负值表示当前打开的 row id。
  int open_row = -1;
  // bank-local ACT gate：受 nRC 或 PRE->ACT 的 nRP 更新。
  Cycle next_act = 0;
  // bank-local PRE gate：受 nRAS、nRTP、写恢复路径更新。
  Cycle next_pre = 0;
  // bank-local RD gate：受 nRCDRD 或上一条 RD 的 nCCDL 更新。
  Cycle next_rd = 0;
  // bank-local WR gate：受 nRCDWR 或上一条 WR 的 nCCDL 更新。
  Cycle next_wr = 0;
  // 同一 bank 内任意列命令间隔。跨 bank 的列约束由 TimingEngine 维护。
  Cycle next_any_col = 0;
  // LPDDR ACT1 后允许 ACT2 的最早 cycle。
  Cycle next_act2 = 0;
  // LPDDR ACT2 最晚发射点。达到 deadline 后必须优先服务 owning request。
  Cycle act2_deadline = 0;
  // true 表示 ACT1 已发出但 ACT2 尚未完成，此时该 bank 不能接受其他请求插队。
  bool activating = false;
};

}  // namespace hbm_sim
