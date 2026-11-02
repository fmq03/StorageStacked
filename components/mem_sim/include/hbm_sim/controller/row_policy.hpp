#pragma once

// Controller 子模块：行策略状态。当前支持 open-page、closed-page 和 ClosedCAP。

#include <cstddef>
#include <vector>

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

// RowPolicyEngine 保存 Ramulator ClosedCAP 风格策略需要的 per-bank 状态。
// OpenPage/ClosedPage 仍然是“选择 RD/RDA 或 WR/WRA”的无状态策略；
// ClosedCAP 需要记录同一 bank 连续列访问次数，并在达到 cap 后触发 AP/PRE。
class RowPolicyEngine {
 public:
  RowPolicyEngine() = default;
  RowPolicyEngine(std::size_t bank_count, int cap) { reset(bank_count, cap); }

  // bank_count 和 cap 都必须为正；非法库调用直接报错，不静默替换配置。
  void reset(std::size_t bank_count, int cap);

  // ClosedCAP 达到 cap 且该 bank 没有 active request 时，Controller 可以注入显式 PREpb。
  bool should_schedule_precharge(RowPolicyKind policy, int flat_bank,
                                 int active_requests) const;
  // ClosedCAP 达到 cap 且当前数据命令 timing 允许时，Controller 可以把 RD/WR 升级为 RDA/WRA。
  bool should_auto_precharge(RowPolicyKind policy, int flat_bank) const;
  // 显式 PREpb 已经进入 priority buffer，避免同一 bank 重复注入多个 PRE。
  void mark_precharge_pending(int flat_bank);
  // 每次命令发射后更新访问计数：列访问递增，closing/all-bank closing 清零。
  void on_issue(RowPolicyKind policy, int flat_bank, Command issued);

  std::size_t bank_count() const { return column_accesses_.size(); }

 private:
  bool valid_bank(int flat_bank) const;

  int cap_ = 1;
  std::vector<int> column_accesses_;
  std::vector<bool> precharge_pending_;
};

}  // namespace hbm_sim
