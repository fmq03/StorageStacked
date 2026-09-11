#pragma once

// Controller 子模块：执行已经选中的命令并更新状态。
// 它是命令状态转换的唯一集中入口，避免 ACT/RD/REF/RFM 的副作用散落到调度代码。

#include <optional>
#include <utility>
#include <vector>

#include "hbm_sim/dram/bank_state.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/controller/rfm.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/stats/stats.hpp"
#include "hbm_sim/controller/timing.hpp"

namespace hbm_sim {

struct CommandExecutionResult {
  // ACT/ACT1 触发 RAA/PRAC 阈值时，Controller 需要把 RFMPB/RFMAB
  // 维护请求放入统一 priority path。Executor 只报告命令，不直接碰队列。
  std::optional<RfmMaintenanceCommand> rfm_command;
};

// CommandExecutor 执行单条 DRAM 命令的状态转换。它对应 Ramulator 风格中
// command lambda / state transition 的职责：更新 bank-local 状态、scope timing、
// WCK 窗口、RFM 计数和命令统计。Controller 只负责仲裁、发射和请求生命周期。
class CommandExecutor {
 public:
  CommandExecutor(const DramSpec& spec,
                  std::vector<BankState>& banks,
                  TimingEngine& timing_engine,
                  RfmManager& rfm_manager,
                  Stats& stats);

  CommandExecutionResult issue(Request& req, Command issued, Cycle clk);

 private:
  Cycle timing_delay(int cycles) const;
  void close_rank_banks(const DecodedAddress& decoded, Cycle clk);
  void gate_rank_banks_after(const DecodedAddress& decoded, Cycle ready_at);
  std::pair<int, int> rank_bank_range(const DecodedAddress& decoded) const;

  const DramSpec& spec_;
  std::vector<BankState>& banks_;
  TimingEngine& timing_engine_;
  RfmManager& rfm_manager_;
  Stats& stats_;
};

}  // namespace hbm_sim
