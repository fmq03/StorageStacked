#pragma once

// Controller 子模块：refresh 维护请求生成器。
// 它只决定何时、对哪些 bank/channel 生成 REF 请求，不直接修改 DRAM 状态。

#include <vector>

#include "hbm_sim/core/request.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

struct MaintenanceCommand {
  // 最终维护命令，例如 REFpb、REFdb、REFab。若 bank 尚未 idle，
  // Controller::next_command() 会先为该请求派生 PREpb/PREab。
  Command command = Command::NOP;
  // 维护目标。对 all-bank 命令，decoded.channel/rank 等字段用于限定作用域；
  // bank 字段本身不表示单个被刷新 bank。
  DecodedAddress decoded;
};

struct RefreshTickResult {
  // started_batch 表示本 tick 到达 refresh interval。即使命令因为队列满稍后才服务，
  // batch 仍应计数，便于观察 refresh pressure。
  bool started_batch = false;
  bool postponed = false;
  bool pulled_in = false;
  int credit = 0;
  // 本 batch 生成的维护命令。per-bank HBM 会为每个 channel/PC 生成目标，
  // LPDDR6 dual-bank 会生成 REFdb，all-bank 策略会生成 REFab。
  std::vector<MaintenanceCommand> commands;
};

// RefreshManager 只维护 JEDEC refresh 轮转节奏和目标 bank 生成。
// 命令是否能立即进入 priority buffer、是否满足 PRE/REF timing，仍由 Controller
// 的状态机和 TimingEngine 路径决定。
class RefreshManager {
 public:
  // reset() 初始化轮转游标和下一次 refresh 时间。clk 通常为 controller 当前 cycle。
  void reset(const DramSpec& spec, Cycle clk);
  // tick() 在到达 interval 时返回一批维护命令；没有到期时 commands 为空。
  RefreshTickResult tick(const DramSpec& spec, Cycle clk, bool prefer_postpone, bool allow_pull_in);

 private:
  struct RankRefreshState {
    Cycle next_refresh_cycle = 0;
    int sid_cursor = 0;
    int flat_bank_cursor = 0;
    // 正数是已经到期但尚未执行的 obligation；负数是提前执行、等待未来
    // deadline 抵消的 refresh。该定义使 postpone/pull-in 都满足账本守恒。
    int credit = 0;
    int postpone_count = 0;
    int pullin_count = 0;
  };

  Cycle timing_delay(const DramSpec& spec, int cycles) const;
  int refresh_interval(const DramSpec& spec) const;
  std::vector<MaintenanceCommand> seed_refresh_batch(const DramSpec& spec,
                                                      int rank,
                                                      RankRefreshState& state);
  int aggregate_credit() const;

  std::vector<RankRefreshState> rank_states_;
  int rank_cursor_ = 0;
};

}  // namespace hbm_sim
