// Scheduler：在候选命令视图上实现 FCFS/FR-FCFS 选择逻辑。
// Controller 先把“该请求下一条命令是什么、是否 eligible/ready”算好，
// Scheduler 只处理排序语义，便于后续对齐 Ramulator2.1 scheduler 插件。
#include "hbm_sim/controller/scheduler.hpp"
#include <stdexcept>

namespace hbm_sim {

std::optional<SchedulerCandidateView> select_scheduled_request(
    SchedulerKind kind,
    const std::vector<SchedulerCandidateView>& candidates) {
  if (kind != SchedulerKind::FCFS && kind != SchedulerKind::FRFCFS)
    throw std::invalid_argument("unsupported scheduler kind");
  std::optional<SchedulerCandidateView> first_eligible;
  std::optional<SchedulerCandidateView> first_ready;

  for (const auto& candidate : candidates) {
    if (!candidate.eligible) {
      continue;
    }

    if (!first_eligible.has_value() || candidate.arrival < first_eligible->arrival) {
      first_eligible = candidate;
    }

    if (candidate.timing_ready &&
        (!first_ready.has_value() || candidate.arrival < first_ready->arrival)) {
      first_ready = candidate;
    }
  }

  // FCFS 不允许绕过最老请求：最早 eligible 的请求未 ready 时，本队列本周期
  // 保持空发射。这和 Ramulator 中“先按队列顺序，再检查命令可发性”的语义一致。
  if (kind == SchedulerKind::FCFS) {
    if (first_eligible.has_value() && first_eligible->timing_ready) {
      return first_eligible;
    }
    return std::nullopt;
  }

  // FRFCFS 允许在 eligible 请求中优先挑 timing-ready 的最老请求。
  return first_ready;
}

}  // namespace hbm_sim
