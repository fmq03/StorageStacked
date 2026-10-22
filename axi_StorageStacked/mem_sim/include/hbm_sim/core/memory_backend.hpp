#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hbm_sim/core/common.hpp"
#include "hbm_sim/core/storage_key.hpp"

namespace hbm_sim {

using ByteVector = std::vector<std::uint8_t>;

enum class MemoryBackendKind {
  Sparse,
  MmapSparse,
  ChunkFile,
};

std::string to_string(MemoryBackendKind kind);
MemoryBackendKind parse_memory_backend_kind(const std::string& value);

struct MemoryBackendOptions {
  MemoryBackendKind kind = MemoryBackendKind::Sparse;
  std::uint64_t capacity_bytes = 0;
  std::string data_file;
  std::string init_file;
  std::string meta_file;
  std::string presence_file;
  std::size_t chunk_size_bytes = 2 * 1024 * 1024;
  std::size_t chunk_cache_entries = 16;
};

// BackendLine 只保存持久状态。物理坐标可由地址和 storage_key 确定，
// 由 MemoryImage 重建，不在每个数据块中重复保存。
struct BackendLine {
  ByteVector bytes;
  ByteVector initialized_mask;
  bool initialized = false;
  std::uint64_t version = 0;
  std::uint64_t last_writer_request_id = 0;
  Cycle last_write_cycle = 0;
  Cycle last_access_cycle = 0;
  std::uint32_t checksum = 0;
  std::uint64_t ecc_hamming = 0;
  bool ecc_overall = false;
  int ecc_parity_bits = 0;
  bool ecc_valid = false;
  bool ecc_uncorrectable = false;
  std::uint64_t ecc_error_injections = 0;
  StorageKey storage_key;
};

class MemoryBackend {
 public:
  virtual ~MemoryBackend() = default;

  virtual MemoryBackendKind kind() const = 0;
  virtual std::size_t line_size() const = 0;
  virtual std::uint64_t capacity_bytes() const = 0;
  virtual std::uint64_t allocated_lines() const = 0;
  virtual std::uint64_t unique_written_lines() const = 0;
  // all_addresses() 检查的数据块槽位数。稀疏表枚举已分配项，位图文件检查逻辑空间。
  virtual std::uint64_t address_scan_lines() const = 0;
  virtual bool load(Address base, BackendLine& line) const = 0;
  // base 在写入前尚未分配时返回 true。
  virtual bool store(Address base, const BackendLine& line) = 0;
  virtual std::optional<Address> address_for_storage_key(const StorageKey& key) const = 0;
  virtual std::vector<Address> all_addresses() const = 0;
  virtual void clear() = 0;
  virtual void flush() = 0;
};

std::unique_ptr<MemoryBackend> make_memory_backend(std::size_t line_size,
                                                   const MemoryBackendOptions& options);

}  // namespace hbm_sim
