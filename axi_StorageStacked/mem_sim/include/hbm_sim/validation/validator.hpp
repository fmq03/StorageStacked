#pragma once

// Validation 层命令流审计接口。Validator 独立于 Controller 在线状态，
// 用重放方式检查命令序列是否满足协议状态和 timing 约束。

#include <string>
#include <vector>
#include <cstddef>

#include "hbm_sim/controller/command.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

struct CommandValidationReport {
  // checked_commands 是本次 validator 实际重放的命令条数。CLI 输出它之后，
  // 实验记录就能区分“没有开启 validation”和“开启后确实审计了 N 条命令”。
  std::size_t checked_commands = 0;

  // 下面这些计数不是错误数，而是覆盖率摘要。它们帮助研究者确认当前 trace
  // 是否真正覆盖了 bus、edge、state、timing、tFAW、WCK 等验证路径。
  std::size_t bus_checks = 0;
  std::size_t edge_checks = 0;
  std::size_t edge_pairing_checks = 0;
  std::size_t state_checks = 0;
  std::size_t timing_constraint_checks = 0;
  std::size_t timing_constraint_updates = 0;
  std::size_t faw_events_checked = 0;
  std::size_t wck_window_checks = 0;

  // errors 保留前若干条错误文本。Validator 会限制最大错误数，避免坏 trace
  // 产生海量输出把真正重要的信息淹没。
  std::vector<std::string> errors;

  // ok() 只是语法糖；调用方可以直接查看 errors 获得详细失败原因。
  bool ok() const { return errors.empty(); }
};

// 后处理 command trace validation hook。它不替代 Controller 在线 timing gate，
// 而是像 Ramulator plugin 一样对最终命令流做独立审计，便于和 golden trace
// 或 Ramulator2.1 differential flow 对接。
CommandValidationReport validate_command_trace(const DramSpec& spec,
                                                const std::vector<IssuedCommand>& trace);

}  // namespace hbm_sim
