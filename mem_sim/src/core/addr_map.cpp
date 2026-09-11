// Core 层地址映射：把 byte address 解码成 channel/PC/SID/rank/BG/bank/row/column。
// 模板名遵循论文/Ramulator 常见的高位到低位命名方式，但实现时按低位逐段取值，
// 因为 cache-line index 的低位最先决定连续访问如何跨 channel 或 column 分布。
#include "hbm_sim/core/addr_map.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace hbm_sim {
namespace {

std::uint64_t checked_multiply(std::uint64_t lhs, std::uint64_t rhs,
                               const char *context) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(context) +
                              " exceeds Address space");
  }
  return lhs * rhs;
}

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs,
                          const char *context) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    throw std::overflow_error(std::string(context) +
                              " exceeds Address space");
  }
  return lhs + rhs;
}

} // namespace

StackAddressMapper::StackAddressMapper(int stack_count,
                                       std::uint64_t interleave_bytes,
                                       std::uint64_t stack_capacity_bytes,
                                       StackMappingKind kind)
    : stack_count_(stack_count),
      interleave_bytes_(interleave_bytes),
      stack_capacity_bytes_(stack_capacity_bytes),
      kind_(kind) {
  if (stack_count_ <= 0) throw std::invalid_argument("stack_count must be positive");
  if (interleave_bytes_ == 0) throw std::invalid_argument("stack_interleave_bytes must be positive");
  if (kind_ == StackMappingKind::Blocked && stack_capacity_bytes_ == 0) {
    throw std::invalid_argument("blocked stack mapping requires non-zero stack capacity");
  }
}

StackAddress StackAddressMapper::decode(Address system_address) const {
  if (stack_count_ == 1) {
    if (stack_capacity_bytes_ != 0 && system_address >= stack_capacity_bytes_) {
      throw std::out_of_range("system address exceeds stack capacity");
    }
    return {0, system_address};
  }
  if (kind_ == StackMappingKind::Blocked) {
    std::uint64_t stack = system_address / stack_capacity_bytes_;
    if (stack >= static_cast<std::uint64_t>(stack_count_)) {
      throw std::out_of_range("system address exceeds aggregate multi-stack capacity");
    }
    return {static_cast<int>(stack), system_address % stack_capacity_bytes_};
  }
  const std::uint64_t stripe = system_address / interleave_bytes_;
  const std::uint64_t offset = system_address % interleave_bytes_;
  const int stack = static_cast<int>(stripe % static_cast<std::uint64_t>(stack_count_));
  const std::uint64_t local_stripe = stripe / static_cast<std::uint64_t>(stack_count_);
  const Address local_address = checked_add(
      checked_multiply(local_stripe, interleave_bytes_,
                       "decoded stack-local address"),
      offset, "decoded stack-local address");
  if (stack_capacity_bytes_ != 0 && local_address >= stack_capacity_bytes_) {
    throw std::out_of_range("system address exceeds aggregate multi-stack capacity");
  }
  return {stack, local_address};
}

Address StackAddressMapper::encode(int stack, Address local_address) const {
  if (stack < 0 || stack >= stack_count_) throw std::out_of_range("stack_id out of range");
  if (stack_capacity_bytes_ != 0 && local_address >= stack_capacity_bytes_) {
    throw std::out_of_range("local address exceeds stack capacity");
  }
  if (stack_count_ == 1) return local_address;
  if (kind_ == StackMappingKind::Blocked) {
    if (local_address >= stack_capacity_bytes_) {
      throw std::out_of_range("local address exceeds stack capacity");
    }
    return checked_add(
        checked_multiply(static_cast<Address>(stack), stack_capacity_bytes_,
                         "blocked system address"),
        local_address, "blocked system address");
  }
  const std::uint64_t local_stripe = local_address / interleave_bytes_;
  const std::uint64_t offset = local_address % interleave_bytes_;
  const std::uint64_t stripe = checked_add(
      checked_multiply(local_stripe,
                       static_cast<std::uint64_t>(stack_count_),
                       "interleaved system stripe"),
      static_cast<std::uint64_t>(stack), "interleaved system stripe");
  return checked_add(
      checked_multiply(stripe, interleave_bytes_,
                       "interleaved system address"),
      offset, "interleaved system address");
}

// 从低位开始按维度大小取模，并把商留给下一层维度。可以把它理解成把
// cache-line 编号解释成一个混合进制数。
static int take_mod(std::uint64_t& value, int mod) {
  // safe_mod 防止错误配置把某个维度设为 0 后触发除零。此处选择退化为 1，
  // 意味着该维度只有一个合法取值 0，仿真仍可继续暴露其他问题。
  int safe_mod = std::max(1, mod);
  int result = static_cast<int>(value % static_cast<std::uint64_t>(safe_mod));
  // value 被原地除以当前维度大小，剩余高位继续映射到下一层级。
  value /= static_cast<std::uint64_t>(safe_mod);
  return result;
}

static void apply_lpddr_efficiency(const DramSpec& spec, DecodedAddress& decoded) {
  if (spec.lpddr_family && spec.lpddr_efficiency_mode != LpddrEfficiencyMode::Normal) {
    // LPDDR6 efficiency mode 下 secondary sub-channel interface 关闭，
    // host 命令经 primary sub-channel 访问两组 bank array。
    decoded.pseudo_channel = 0;
  }
}

static void take_bank_composite(std::uint64_t& line, const DramSpec& spec, DecodedAddress& decoded) {
  // “Ba” 在模板里代表 bank 相关选择。HBM/LPDDR 还有 pseudo-channel/SID，
  // 这里把它们放在 bank composite 内，保证模板无需为每个标准发明新名字。
  decoded.bank = take_mod(line, spec.org.banks_per_group);
  decoded.bank_group = take_mod(line, spec.org.bank_groups);
  decoded.pseudo_channel = take_mod(line, spec.org.pseudo_channels);
  decoded.sid = take_mod(line, spec.org.sids);
}

DecodedAddress AddressMapper::decode(Address address) const {
  // byte address 的 transaction 内偏移不参与 DRAM 结构映射。host line 可以比
  // DRAM transaction 更大，例如 HBM4 64B cache line 会拆成两个 32B column access。
  std::uint64_t line =
      address / static_cast<std::uint64_t>(std::max(1, spec_.transaction_bytes()));

  DecodedAddress decoded;
  switch (spec_.address_mapping) {
    case AddressMappingKind::Default:
      // 低位优先映射到 column/bank 等字段，使顺序流量先扫列，再跨 bank/group。
      // 这样 stream workload 更容易形成 row hit，用来观察 open-row 策略收益；
      // random workload 则会更频繁地产生 row conflict。
      decoded.column = take_mod(line, spec_.org.columns);
      take_bank_composite(line, spec_, decoded);
      decoded.rank = take_mod(line, spec_.org.ranks);
      decoded.channel = take_mod(line, spec_.org.channels);
      decoded.row = take_mod(line, spec_.org.rows);
      break;
    case AddressMappingKind::RoBaRaCoCh:
      // 高位到低位 Ro-Ba-Ra-Co-Ch，所以低位优先取 Ch, Co, Ra, Ba, Ro。
      decoded.channel = take_mod(line, spec_.org.channels);
      decoded.column = take_mod(line, spec_.org.columns);
      decoded.rank = take_mod(line, spec_.org.ranks);
      take_bank_composite(line, spec_, decoded);
      decoded.row = take_mod(line, spec_.org.rows);
      break;
    case AddressMappingKind::ChRaBaRoCo:
      // 高位到低位 Ch-Ra-Ba-Ro-Co，所以低位优先取 Co, Ro, Ba, Ra, Ch。
      decoded.column = take_mod(line, spec_.org.columns);
      decoded.row = take_mod(line, spec_.org.rows);
      take_bank_composite(line, spec_, decoded);
      decoded.rank = take_mod(line, spec_.org.ranks);
      decoded.channel = take_mod(line, spec_.org.channels);
      break;
    case AddressMappingKind::RoCoRaBaCh:
      // 高位到低位 Ro-Co-Ra-Ba-Ch，所以低位优先取 Ch, Ba, Ra, Co, Ro。
      decoded.channel = take_mod(line, spec_.org.channels);
      take_bank_composite(line, spec_, decoded);
      decoded.rank = take_mod(line, spec_.org.ranks);
      decoded.column = take_mod(line, spec_.org.columns);
      decoded.row = take_mod(line, spec_.org.rows);
      break;
  }
  apply_lpddr_efficiency(spec_, decoded);
  return decoded;
}

}  // namespace hbm_sim
