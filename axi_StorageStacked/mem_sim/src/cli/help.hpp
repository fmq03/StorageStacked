#pragma once

namespace hbm_sim::cli {

// 输出独立命令行工具的稳定用法说明。
// main.cpp 只负责解析参数和装配仿真流程，用户可见的 option 文档集中放在这里，
// 避免 CLI 入口随着选项增长而变成难以阅读的长文本块。
void print_help(const char* argv0);

}  // namespace hbm_sim::cli
