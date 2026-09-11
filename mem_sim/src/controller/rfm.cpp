// RfmManager：维护 HBM RAA / LPDDR PRAC 类 ACT 计数，并在达到阈值时生成 RFM 请求。
// per-bank 与 all-bank RFM 策略在这里选择；实际 RFMpb/RFMab 命令仍走统一维护路径。
#include "hbm_sim/controller/rfm.hpp"

#include <algorithm>

namespace hbm_sim {

void RfmManager::reset(const DramSpec& spec) {
  validate_spec(spec);
  // 每个 bank 一份 ACT 计数和 pending 标记。pending 标记用于避免阈值已触发但
  // RFMPB/RFMAB 还没发出前，连续 ACT 反复生成重复维护请求。
  const std::size_t bank_count = static_cast<std::size_t>(spec.total_banks());
  act_count_since_rfm_.assign(bank_count, 0);
  rfm_pending_bank_.assign(bank_count, false);
  const std::size_t rank_count =
      static_cast<std::size_t>(std::max(1, spec.org.channels) *
                               std::max(1, spec.org.pseudo_channels) *
                               std::max(1, spec.org.sids) *
                               std::max(1, spec.org.ranks));
  rfm_pending_all_bank_.assign(rank_count, false);
}

std::optional<RfmMaintenanceCommand> RfmManager::on_activate(
    const DramSpec& spec,
    const DecodedAddress& decoded,
    Stats& stats) {
  if (!spec.supports_rfm || spec.rfm_act_threshold <= 0) {
    // supports_rfm 关闭时，ACT 计数路径完全旁路。这样 HBM3/LPDDR5 preset 不会
    // 因 RFM 参数默认值而产生维护命令。
    return std::nullopt;
  }

  int flat = decoded.flat_bank(spec);
  if (!valid_bank(flat)) {
    return std::nullopt;
  }

  act_count_since_rfm_[flat]++;
  if (act_count_since_rfm_[flat] < spec.rfm_act_threshold) {
    // 未达到 RAA/PRAC 阈值时只累计计数，不影响普通请求调度。
    return std::nullopt;
  }

  if (spec.rfm_policy == MaintenancePolicyKind::AllBank) {
    const int target_rank = rank_index(spec, decoded);
    if (target_rank < 0 ||
        target_rank >= static_cast<int>(rfm_pending_all_bank_.size()) ||
        rfm_pending_all_bank_[static_cast<std::size_t>(target_rank)]) {
      return std::nullopt;
    }
    rfm_pending_all_bank_[static_cast<std::size_t>(target_rank)] = true;
    // RFMab 的 all-bank 作用域是目标 rank；其他 rank 可以独立触发自己的维护。
    stats.rfm_events++;
    stats.rfm_all_bank_events++;
    return RfmMaintenanceCommand{Command::RFMAB, decoded};
  }

  if (rfm_pending_bank_[flat]) {
    return std::nullopt;
  }
  rfm_pending_bank_[flat] = true;
  // per-bank RFM 只针对触发阈值的 bank。Controller 后续可能需要先 PREpb，
  // 因此真正计数递减要等 CommandExecutor 看到 RFMPB 发出。
  stats.rfm_events++;
  stats.rfm_per_bank_events++;
  return RfmMaintenanceCommand{Command::RFMPB, decoded};
}

void RfmManager::on_rfmpb(const DramSpec& spec, const DecodedAddress& decoded, Stats& stats) {
  int flat = decoded.flat_bank(spec);
  if (!valid_bank(flat)) {
    return;
  }

  // RFM 后不是简单清零，而是按 rfm_decrement 递减。这样可以表达标准/厂商表中
  // “一次 mitigation 消耗若干 activation credit”的行为。
  act_count_since_rfm_[flat] = std::max(0, act_count_since_rfm_[flat] - decrement_value(spec));
  rfm_pending_bank_[flat] = false;
  stats.rfm_decrements++;
}

void RfmManager::on_rfmab(const DramSpec& spec,
                          const DecodedAddress& decoded,
                          Stats& stats) {
  int decrement = decrement_value(spec);
  const auto [begin, end] = rank_bank_range(spec, decoded);
  for (int flat = begin; flat < end; ++flat) {
    act_count_since_rfm_[static_cast<std::size_t>(flat)] =
        std::max(0, act_count_since_rfm_[static_cast<std::size_t>(flat)] -
                        decrement);
    rfm_pending_bank_[static_cast<std::size_t>(flat)] = false;
  }
  const int target_rank = rank_index(spec, decoded);
  if (target_rank >= 0 &&
      target_rank < static_cast<int>(rfm_pending_all_bank_.size())) {
    rfm_pending_all_bank_[static_cast<std::size_t>(target_rank)] = false;
  }
  stats.rfm_decrements++;
}

int RfmManager::rank_index(const DramSpec& spec,
                           const DecodedAddress& decoded) const {
  const int channel =
      std::clamp(decoded.channel, 0, std::max(1, spec.org.channels) - 1);
  const int pc = std::clamp(decoded.pseudo_channel, 0,
                            std::max(1, spec.org.pseudo_channels) - 1);
  const int sid =
      std::clamp(decoded.sid, 0, std::max(1, spec.org.sids) - 1);
  const int rank =
      std::clamp(decoded.rank, 0, std::max(1, spec.org.ranks) - 1);
  return ((channel * std::max(1, spec.org.pseudo_channels) + pc) *
              std::max(1, spec.org.sids) +
          sid) *
             std::max(1, spec.org.ranks) +
         rank;
}

std::pair<int, int> RfmManager::rank_bank_range(
    const DramSpec& spec, const DecodedAddress& decoded) const {
  const int banks_per_rank =
      std::max(1, spec.org.bank_groups * spec.org.banks_per_group);
  const int begin = rank_index(spec, decoded) * banks_per_rank;
  return {begin, std::min(static_cast<int>(act_count_since_rfm_.size()),
                          begin + banks_per_rank)};
}

bool RfmManager::valid_bank(int flat_bank) const {
  return flat_bank >= 0 && flat_bank < static_cast<int>(act_count_since_rfm_.size());
}

int RfmManager::decrement_value(const DramSpec& spec) const {
  int fallback = spec.rfm_act_threshold > 0 ? spec.rfm_act_threshold : 1;
  // 若配置没有显式给 rfm_decrement，默认等于阈值，表示一次 RFM 刚好抵消一次触发。
  return std::max(1, spec.rfm_decrement > 0 ? spec.rfm_decrement : fallback);
}

}  // namespace hbm_sim
