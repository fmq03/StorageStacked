#pragma once

// Controller 子模块：跨层级 timing 状态机。
// 它维护 table-driven constraints、tFAW 和 WCK 窗口，是把 timing
// 作用域显式化的核心。

#include <array>
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/common.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// Ramulator 风格的 per-scope timing 状态。不同约束可以绑定到 channel、
// pseudo-channel、rank、bank-group 或 bank 等层级。
struct TimingScopeState {
  // 通用 row bus gate：用于避免同一作用域内同 cycle 重复行命令。
  Cycle next_row = 0;
  // activate 间隔 gate：由 nRRDS/nRRDL 更新。
  Cycle next_act = 0;
  // column bus gate：由 nCCDS 更新，限制同一 column scope 的列命令节奏。
  Cycle next_col = 0;
  // LPDDR CAS gate：避免 CAS_RD/CAS_WR 在 WCK 窗口建立过程中过密。
  Cycle next_cas = 0;
  // LPDDR WCK 建立完成时间；RD/WR 必须在该时间之后。
  Cycle wck_ready_at = 0;
  // LPDDR WCK 活跃窗口结束时间；过期后下一次 RD/WR 需要重新 CAS。
  Cycle wck_active_until = 0;
  // activation_scope 内的 ACT/ACT1/REFpb/REFdb 历史，用于实现 tFAW。
  std::deque<Cycle> recent_acts;
  // 表驱动 TimingConstraint 的 ready time。下标来自 command_index(Command)。
  std::array<Cycle, kCommandCount> next_command{};
};

// TimingEngine 负责跨 bank/scope 的 timing 状态。Controller 仍拥有 bank-local
// open row、nRCD、nRP、nRTP 等状态；TimingEngine 专管 scope 级总线、tFAW、
// WCK ready window 和 table-driven preceding->following 约束。
class TimingEngine {
public:
  TimingEngine() = default;
  // 构造时立即按 DramSpec 的各类 scope 维度分配 timing bucket。
  explicit TimingEngine(const DramSpec &spec) { reset(spec); }

  // reset() 会丢弃所有历史 timing 状态。Controller 构造和测试重新配置 spec
  // 后调用。
  void reset(const DramSpec &spec);

  // row_state/column_state 使用 spec.row_bus_scope/spec.column_bus_scope
  // 指定的作用域， 因此 HBM 可以按 pseudo-channel 建 bucket，LPDDR 可以按
  // channel 或 subchannel 建 bucket。
  TimingScopeState &row_state(const DramSpec &spec,
                              const DecodedAddress &decoded);
  const TimingScopeState &row_state(const DramSpec &spec,
                                    const DecodedAddress &decoded) const;
  TimingScopeState &column_state(const DramSpec &spec,
                                 const DecodedAddress &decoded);
  const TimingScopeState &column_state(const DramSpec &spec,
                                       const DecodedAddress &decoded) const;
  // bank_group_state 固定按 BankGroup 作用域，用于 same/different BG 的
  // nCCD/nRRD 差异。
  TimingScopeState &bank_group_state(const DramSpec &spec,
                                     const DecodedAddress &decoded);
  const TimingScopeState &bank_group_state(const DramSpec &spec,
                                           const DecodedAddress &decoded) const;
  // wck_state 使用 spec.wck_scope。LPDDR6 可按 subchannel 建 WCK 窗口，LPDDR5
  // 可按 channel。
  TimingScopeState &wck_state(const DramSpec &spec,
                              const DecodedAddress &decoded);
  const TimingScopeState &wck_state(const DramSpec &spec,
                                    const DecodedAddress &decoded) const;

  // 检查 table-driven constraint 和 LPDDR6 REFdb 计数/双目标是否 ready。
  // 这里不处理 window>0 的 tFAW 类约束，tFAW 走 recent_acts 专门路径。
  bool constraint_ready(const DramSpec &spec, const DecodedAddress &decoded,
                        Command cmd, Cycle clk) const;
  // 返回表驱动与 REFdb 计数约束的最早发射 tick。重复刷新本轮已访问的 Bank
  // 时返回 Cycle 最大值：须先完成轮转/同步，单纯等待无法就绪。LPDDR 用它避免在总线换向
  // gate 尚未结束时过早建立一个注定会过期的 WCK/CAS 窗口。
  Cycle constraint_ready_at(const DramSpec &spec, const DecodedAddress &decoded,
                            Command cmd) const;
  // 一条命令发出后，把所有以它为 preceding 的 constraint 写入对应 scope
  // bucket。
  void apply_constraints(const DramSpec &spec, const DecodedAddress &decoded,
                         Command issued, Cycle clk);

  // tFAW 的历史窗口需要随时间清理，否则 recent_acts 会无限增长。
  void prune_recent_acts(const DramSpec &spec, Cycle clk);
  // ACT/ACT1/REFpb/REFdb 这类会占用 activation window 的事件调用
  // record_activate()。
  void record_activate(const DramSpec &spec, const DecodedAddress &decoded,
                       Cycle clk);
  // faw_ready() 只检查窗口中是否少于 4 个事件；具体 nRRD 仍由其他
  // gate/constraint 控制。
  bool faw_ready(const DramSpec &spec, const DecodedAddress &decoded) const;
  // LPDDR 数据命令是否落在 WCK ready window 内。
  bool wck_ready_for_data(const DramSpec &spec, const DecodedAddress &decoded,
                          Cycle clk) const;

private:
  // scope_count/scope_index 是本模块最核心的地址到 bucket 映射逻辑。
  // 如果未来补更多 JEDEC 层级，例如 bank-pair 或 die-stack
  // scope，应从这里扩展。
  std::size_t scope_count(const DramSpec &spec, TimingScope scope) const;
  std::size_t scope_index(const DramSpec &spec, TimingScope scope,
                          const DecodedAddress &decoded) const;
  Cycle timing_delay(const DramSpec &spec, int cycles) const;
  // mutable_scope/scope_state 给 table-driven constraint 使用，因为每条
  // constraint 自己声明作用域，不能只走 row/column/wck 固定路径。
  TimingScopeState &mutable_scope(const DramSpec &spec, TimingScope scope,
                                  const DecodedAddress &decoded);
  const TimingScopeState &scope_state(const DramSpec &spec, TimingScope scope,
                                      const DecodedAddress &decoded) const;
  // sibling constraint 需要直接更新“同一父级下除当前 bucket 外”的状态。
  // 例如 SID 的父级是 pseudo-channel，BankGroup 的父级是 rank。
  std::pair<std::size_t, std::size_t>
  sibling_range(const DramSpec &spec, TimingScope scope,
                const DecodedAddress &decoded) const;
  std::vector<TimingScopeState> &scope_vector(TimingScope scope);

  // 下面这些 vector 都是“按某个 DRAM 层级展开的 timing bucket 数组”。
  // bucket 内保存 ready time 和历史事件；DecodedAddress 通过 scope_index()
  // 映射到数组下标。
  std::vector<TimingScopeState> channel_scopes_;
  std::vector<TimingScopeState> pseudo_channel_scopes_;
  std::vector<TimingScopeState> sid_scopes_;
  std::vector<TimingScopeState> rank_scopes_;
  // LPDDR6: one refresh-row bank counter per Channel/Subchannel/SID/Rank.
  // A complete sweep advances the refresh row and selects tDBR2DBR_L.
  struct RefdbState {
    int bank_count = 0;
    Cycle ready = 0;
    std::vector<bool> visited_pairs;
  };
  std::vector<RefdbState> refdb_states_;
  // activation_scopes_ 的作用域来自 spec.activation_scope，专门服务 tFAW。
  std::vector<TimingScopeState> activation_scopes_;
  // row_scopes_/column_scopes_ 的作用域来自
  // spec.row_bus_scope/spec.column_bus_scope。
  std::vector<TimingScopeState> row_scopes_;
  std::vector<TimingScopeState> column_scopes_;
  std::vector<TimingScopeState> bank_group_scopes_;
  std::vector<TimingScopeState> bank_scopes_;
  // wck_scopes_ 只在 LPDDR family 中真正有意义，HBM 下保持默认状态即可。
  std::vector<TimingScopeState> wck_scopes_;
};

} // namespace hbm_sim
