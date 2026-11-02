#pragma once

// Core 层地址映射接口：把上层 byte address 映射到 DRAM 层级坐标。
// 所有 frontend 在生成 Request 前都应使用这里，而不是在 Controller 内重复解码。

#include <utility>

#include "hbm_sim/core/common.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// 物理地址解码后的 DRAM 层级坐标。请求按 cache line 粒度建模，因此 column
// 表示 cache-line column，而不是 byte offset。
struct DecodedAddress {
  // 层级字段都保持为 int，是因为它们只作为数组下标/比较键使用；
  // 原始地址本身仍保存在 Request::address 中。
  int channel = 0;
  int pseudo_channel = 0;
  int sid = 0;
  int rank = 0;
  int bank_group = 0;
  int bank = 0;
  int row = 0;
  int column = 0;

  // 将多维 bank 坐标压平成 banks_ 数组下标。row/column 不参与 bank 选择。
  int flat_bank(const DramSpec& spec) const {
    // 压平顺序是 bank-state 数组的规范存储顺序，和具体 address_mapping
    // 模板无关；映射模板只决定每个字段的取值来自哪些地址位。
    int x = channel;
    x = x * spec.org.pseudo_channels + pseudo_channel;
    x = x * spec.org.sids + sid;
    x = x * spec.org.ranks + rank;
    x = x * spec.org.bank_groups + bank_group;
    x = x * spec.org.banks_per_group + bank;
    return x;
  }
};

// 多 stack 系统在进入单颗 HBM 的 AddressMapper 前，先把系统地址拆成
// stack_id 和 stack-local 地址。Interleaved 适合带宽聚合，Blocked 适合把
// 每颗 stack 暴露成连续 NUMA 区间。
enum class StackMappingKind {
  Interleaved,
  Blocked,
};

struct StackAddress {
  int stack = 0;
  Address local_address = 0;
};

class StackAddressMapper {
 public:
  StackAddressMapper(int stack_count,
                     std::uint64_t interleave_bytes,
                     std::uint64_t stack_capacity_bytes,
                     StackMappingKind kind = StackMappingKind::Interleaved);

  StackAddress decode(Address system_address) const;
  Address encode(int stack, Address local_address) const;

 private:
  int stack_count_ = 1;
  std::uint64_t interleave_bytes_ = 256;
  std::uint64_t stack_capacity_bytes_ = 0;
  StackMappingKind kind_ = StackMappingKind::Interleaved;
};

class AddressMapper {
 public:
  explicit AddressMapper(DramSpec spec) : spec_(std::move(spec)) {
    validate_spec(spec_);
  }

  // 按 DramSpec::address_mapping 指定的模板，把 byte address 映射到 DRAM
  // 层级坐标。模板名按高位到低位解释，与常见论文/Ramulator 习惯一致。
  DecodedAddress decode(Address address) const;

 private:
  DramSpec spec_;
};

}  // namespace hbm_sim
