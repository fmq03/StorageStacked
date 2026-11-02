#pragma once

// Controller 子模块：调度器接口。输入是候选命令视图，输出是被选中的 request index。

#include <optional>
#include <vector>

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

// SchedulerCandidateView 是 Controller 暴露给调度器的最小只读视图。
// Controller 仍负责生成下一条命令、检查状态合法性和 timing；调度器只实现
// Ramulator 风格的队列选择语义，避免把协议细节泄漏进策略模块。
struct SchedulerCandidateView {
  std::size_t request_index = 0;
  Cycle arrival = 0;
  Command command = Command::NOP;
  bool eligible = false;
  bool timing_ready = false;
};

// 在一组候选中选择本周期应该服务的请求。返回的是候选视图本身，调用方再把
// request_index 映射回对应 request buffer。
std::optional<SchedulerCandidateView> select_scheduled_request(
    SchedulerKind kind,
    const std::vector<SchedulerCandidateView>& candidates);

}  // namespace hbm_sim
