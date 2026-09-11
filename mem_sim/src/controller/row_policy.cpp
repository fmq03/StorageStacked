// RowPolicyEngine：实现 open-page、closed-page 和 ClosedCAP 相关的行关闭决策。
// 它只维护每个 bank 的列访问计数与 pending-precharge 标志，不直接发命令。
#include "hbm_sim/controller/row_policy.hpp"

#include <algorithm>
#include <stdexcept>

#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {

void RowPolicyEngine::reset(std::size_t bank_count, int cap) {
  if (bank_count == 0) {
    throw std::invalid_argument("row-policy bank_count must be positive");
  }
  if (cap <= 0) {
    throw std::invalid_argument("closed-page cap must be positive");
  }
  // cap=1 表示第一次列访问后就尝试关闭 row，可近似 closed-page，但仍走
  // ClosedCAP 的统计/升级路径。
  cap_ = cap;
  column_accesses_.assign(bank_count, 0);
  precharge_pending_.assign(bank_count, false);
}

bool RowPolicyEngine::should_schedule_precharge(
    RowPolicyKind policy,
    int flat_bank,
    int active_requests) const {
  if (policy != RowPolicyKind::ClosedCap || !valid_bank(flat_bank)) {
    return false;
  }
  if (active_requests > 0 || precharge_pending_[flat_bank]) {
    // 只在没有 active request 持有该 bank 时注入显式 PRE。否则维护 PRE 可能
    // 把 active_buffer 中请求刚打开的 row 关掉。
    return false;
  }
  return column_accesses_[flat_bank] >= cap_;
}

bool RowPolicyEngine::should_auto_precharge(RowPolicyKind policy, int flat_bank) const {
  if (policy != RowPolicyKind::ClosedCap || !valid_bank(flat_bank)) {
    return false;
  }
  return column_accesses_[flat_bank] >= cap_;
}

void RowPolicyEngine::mark_precharge_pending(int flat_bank) {
  if (valid_bank(flat_bank)) {
    precharge_pending_[flat_bank] = true;
  }
}

void RowPolicyEngine::on_issue(RowPolicyKind policy, int flat_bank, Command issued) {
  if (policy != RowPolicyKind::ClosedCap) {
    return;
  }

  const auto& meta = command_meta(issued);
  if (meta.all_bank && meta.closing) {
    // PREab 会关闭整个 channel 中所有 open row，因此 ClosedCAP 的 per-bank
    // 访问计数和 pending 标志都要清掉。
    std::fill(column_accesses_.begin(), column_accesses_.end(), 0);
    std::fill(precharge_pending_.begin(), precharge_pending_.end(), false);
    return;
  }

  if (!valid_bank(flat_bank)) {
    return;
  }

  if (meta.closing) {
    // RDA/WRA/PREpb 等关闭目标 bank 后，重新开始计数。
    column_accesses_[flat_bank] = 0;
    precharge_pending_[flat_bank] = false;
    return;
  }

  if (meta.accessing) {
    // 只有真正访问 open row 的列命令才计入 cap；CAS_RD/CAS_WR 不搬运数据，不计数。
    column_accesses_[flat_bank]++;
  }
}

bool RowPolicyEngine::valid_bank(int flat_bank) const {
  return flat_bank >= 0 && flat_bank < static_cast<int>(column_accesses_.size());
}

}  // namespace hbm_sim
