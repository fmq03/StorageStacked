#pragma once

// Validation 层命令 trace 导出接口。导出的 CSV 可用于后处理、golden trace 对比和调试。

#include <iosfwd>
#include <string>
#include <vector>

#include "hbm_sim/controller/command.hpp"

namespace hbm_sim {

// Ramulator plugin 风格的轻量 command trace recorder。当前先提供后处理 CSV
// 输出接口，后续可以直接作为运行时 observer 接入 Controller。
void write_command_trace_csv(std::ostream& os, const std::vector<IssuedCommand>& trace);
void write_command_trace_csv(const std::string& path, const std::vector<IssuedCommand>& trace);

}  // namespace hbm_sim
