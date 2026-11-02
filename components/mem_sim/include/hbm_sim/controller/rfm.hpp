#pragma once

// Controller 子模块：RFM/PRAC 计数与维护请求生成器。
// 它记录 ACT 累计次数并选择 RFMpb/RFMab，具体发射仍由 Controller 仲裁。

#include <optional>
#include <utility>
#include <vector>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/stats/stats.hpp"

namespace hbm_sim {

struct RfmMaintenanceCommand {
  // 由 rfm_policy 决定：per-bank 策略返回 RFMpb，all-bank 策略返回 RFMab。
  Command command = Command::RFMPB;
  // 对 RFMpb 表示目标 bank；对 RFMab 表示触发 RFM 的 channel/rank 作用域。
  DecodedAddress decoded;
};

// RfmManager 保存 HBM RAA/RFM 相关计数。Controller 在 ACT/ACT1 后通知它，
// 它只决定是否需要插入 RFMPB；真正的队列仲裁、PRE/RFM timing 和状态更新
// 仍沿用 Controller 的统一维护命令路径。
class RfmManager {
 public:
  // bank_count 通常等于 spec.total_banks()。每个 flat bank 独立维护 ACT 计数。
  void reset(const DramSpec& spec);
  // Controller 在每次 ACT/ACT1 成功发出后调用。达到阈值时返回需要排队的 RFM 命令；
  // 未达到阈值或已有 pending RFM 时返回 nullopt。
  std::optional<RfmMaintenanceCommand> on_activate(const DramSpec& spec,
                                                   const DecodedAddress& decoded,
                                                   Stats& stats);
  // RFMpb 真正发出后调用，执行 per-bank 计数递减并清 pending 标志。
  void on_rfmpb(const DramSpec& spec, const DecodedAddress& decoded, Stats& stats);
  // RFMab 真正发出后调用，只递减目标 rank 的计数并清该 rank pending 标志。
  void on_rfmab(const DramSpec& spec, const DecodedAddress& decoded, Stats& stats);

 private:
  bool valid_bank(int flat_bank) const;
  int rank_index(const DramSpec& spec, const DecodedAddress& decoded) const;
  std::pair<int, int> rank_bank_range(const DramSpec& spec,
                                      const DecodedAddress& decoded) const;
  // RFM decrement 使用显式配置；若未配置则退回阈值，表示一次 RFM 抵消一轮阈值积累。
  int decrement_value(const DramSpec& spec) const;

  // 每个 flat bank 自上次 RFM 以来累计的 ACT/ACT1 次数。
  std::vector<int> act_count_since_rfm_;
  // 防止同一 bank 在 RFM 尚未服务时重复插入 RFMpb。
  std::vector<bool> rfm_pending_bank_;
  // all-bank 策略下每个 rank 独立防止尚未服务的 RFMab 重复插入。
  std::vector<bool> rfm_pending_all_bank_;
};

}  // namespace hbm_sim
