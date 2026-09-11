#pragma once

// 被动多 stack 存储模型接口。
//
// 这里的 StackModel/MultiStackMemoryModel 是 RTL MC 后端可驱动的 memory device
// 抽象：它维护真实 payload、物理坐标、功耗/热统计和命令事件，不承担
// UCIe/Bridge/MC frontend 的事务调度职责。

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "hbm_sim/core/data.hpp"

namespace hbm_sim {

inline constexpr int kDefaultStackCount = 6;

struct StackReadResult {
  int stack_id = 0;
  Address address = 0;
  ByteVector data;
  bool initialized = false;
  std::optional<DataBlockMetadata> metadata;
};

struct StackCommand {
  // stack_id 由外部 Bridge-MC Frontend 或测试入口决定；被动模型只做分发。
  int stack_id = 0;
  // Command-level 入口面向 MC slice：模型记录事件、维护行缓冲/数据/热功耗，
  // 但不替代 MC slice 做 JEDEC timing 调度。
  Command command = Command::NOP;
  // decoded 是 stack-local DRAM 坐标，不携带跨 stack 路由语义。
  DecodedAddress decoded;
  Cycle cycle = 0;
  // payload_bytes 用于没有真实 payload 的 RD/WR 事件能量统计；为 0 时按
  // spec.transaction_bytes() 或 payload.size() 推导。
  std::size_t payload_bytes = 0;
  // address 只有在命令需要访问真实 payload 时使用，例如 RD/WR/RDA/WRA。
  Address address = 0;
  bool has_address = false;
  ByteVector payload;
  ByteVector mask;
  bool has_mask = false;
  std::uint64_t request_id = 0;
};

struct StackCommandResult {
  int stack_id = 0;
  bool read_data_valid = false;
  ByteVector read_data;
  bool initialized = false;
  std::optional<DataBlockMetadata> metadata;
};

class StackModel {
 public:
  StackModel(int stack_id, DramSpec spec, StorageModelOptions storage_options = {});

  int stack_id() const { return stack_id_; }
  const DramSpec& spec() const { return spec_; }
  MemoryImage& memory_image() { return memory_image_; }
  const MemoryImage& memory_image() const { return memory_image_; }
  PhysicalStorageStats storage_stats() const { return memory_image_.storage_stats(); }

  StackReadResult read(Address address,
                       std::size_t size,
                       const DecodedAddress* decoded = nullptr);
  void write(Address address,
             const ByteVector& data,
             const ByteVector* mask = nullptr,
             const DecodedAddress* decoded = nullptr,
             std::uint64_t request_id = 0,
             Cycle cycle = 0);
  StackCommandResult issue_command(const StackCommand& command);

 private:
  int stack_id_ = 0;
  DramSpec spec_;
  MemoryImage memory_image_;

  void check_stack_id(int stack_id) const;
};

class MultiStackMemoryModel {
 public:
  // 默认使用 6 个 stack，匹配当前项目设定；显式 stack_count 构造保留用于
  // 1/2/4/8 等对比实验。
  explicit MultiStackMemoryModel(DramSpec spec,
                                 StorageModelOptions storage_options = {});
  MultiStackMemoryModel(DramSpec spec,
                        int stack_count,
                        StorageModelOptions storage_options = {});

  int stack_count() const { return static_cast<int>(stacks_.size()); }
  StackModel& stack(int stack_id);
  const StackModel& stack(int stack_id) const;

  StackReadResult read(int stack_id,
                       Address address,
                       std::size_t size,
                       const DecodedAddress* decoded = nullptr);
  void write(int stack_id,
             Address address,
             const ByteVector& data,
             const ByteVector* mask = nullptr,
             const DecodedAddress* decoded = nullptr,
             std::uint64_t request_id = 0,
             Cycle cycle = 0);
  StackCommandResult issue_command(const StackCommand& command);

  std::vector<PhysicalStorageStats> per_stack_storage_stats() const;
  PhysicalStorageStats storage_stats() const;

 private:
  DramSpec spec_;
  std::vector<std::unique_ptr<StackModel>> stacks_;

  StackModel& checked_stack(int stack_id);
  const StackModel& checked_stack(int stack_id) const;
};

PhysicalStorageStats merge_physical_storage_stats(const std::vector<PhysicalStorageStats>& stats);

}  // namespace hbm_sim
