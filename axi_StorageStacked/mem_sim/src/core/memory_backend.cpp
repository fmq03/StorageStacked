#include "hbm_sim/core/memory_backend.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>

namespace hbm_sim {
namespace {

constexpr std::array<char, 8> kMetaMagic{'H', 'B', 'M', 'B', 'A', 'C', 'K', '\0'};
constexpr std::uint32_t kBackendVersion = 3;
constexpr std::uint8_t kInitializedFlag = 1u << 0;
constexpr std::uint8_t kEccOverallFlag = 1u << 1;
constexpr std::uint8_t kEccValidFlag = 1u << 2;
constexpr std::uint8_t kEccUncorrectableFlag = 1u << 3;

struct DiskHeader {
  std::array<char, 8> magic{};
  std::uint32_t version = 0;
  std::uint32_t record_size = 0;
  std::uint64_t line_size = 0;
  std::uint64_t capacity_bytes = 0;
  std::array<std::uint64_t, 4> reserved{};
};

static_assert(sizeof(DiskHeader) == 64);

struct DiskLineMeta {
  std::uint64_t version = 0;
  std::uint64_t last_writer_request_id = 0;
  std::uint64_t last_write_cycle = 0;
  std::uint64_t last_access_cycle = 0;
  std::uint64_t ecc_hamming = 0;
  std::uint64_t ecc_error_injections = 0;
  std::uint32_t checksum = 0;
  std::int32_t ecc_parity_bits = 0;
  std::array<std::int32_t, 9> storage{};
  std::uint8_t flags = 0;
  std::array<std::uint8_t, 7> reserved{};
};

static_assert(std::is_trivially_copyable_v<DiskLineMeta>);

std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs, std::string_view what) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(what) + " size overflow");
  }
  return lhs * rhs;
}

std::size_t checked_size(std::uint64_t value, std::string_view what) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::overflow_error(std::string(what) + " does not fit host size_t");
  }
  return static_cast<std::size_t>(value);
}

std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

bool bit_is_set(const std::uint8_t* bits, std::uint64_t index) {
  return (bits[index / 8] & static_cast<std::uint8_t>(1u << (index % 8))) != 0;
}

void set_bit(std::uint8_t* bits, std::uint64_t index, bool value) {
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << (index % 8));
  if (value) {
    bits[index / 8] |= mask;
  } else {
    bits[index / 8] &= static_cast<std::uint8_t>(~mask);
  }
}

template <typename Visitor>
void visit_set_bits(const std::uint8_t* bits,
                    std::uint64_t bit_count,
                    Visitor&& visitor) {
  const std::uint64_t byte_count = ceil_div(bit_count, 8);
  for (std::uint64_t byte_index = 0; byte_index < byte_count; byte_index++) {
    unsigned value = bits[byte_index];
    while (value != 0) {
      const unsigned bit = std::countr_zero(value);
      const std::uint64_t index = byte_index * 8 + bit;
      if (index >= bit_count || !visitor(index)) {
        return;
      }
      value &= value - 1;
    }
  }
}

std::string require_data_path(const MemoryBackendOptions& options) {
  if (options.data_file.empty()) {
    throw std::invalid_argument("file-backed memory backend requires memory_data_file");
  }
  return options.data_file;
}

std::string sidecar_path(const std::string& configured,
                         const std::string& data_path,
                         std::string_view suffix) {
  return configured.empty() ? data_path + std::string(suffix) : configured;
}

class FileDescriptor {
 public:
  FileDescriptor() = default;

  FileDescriptor(const std::string& path, std::uint64_t expected_size)
      : path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      throw std::runtime_error("failed to open memory backend file " + path + ": " +
                               std::strerror(errno));
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
      throw std::runtime_error("failed to stat memory backend file " + path + ": " +
                               std::strerror(errno));
    }
    original_size_ = static_cast<std::uint64_t>(st.st_size);
    if (original_size_ == 0) {
      resize(expected_size);
    } else if (original_size_ != expected_size) {
      throw std::runtime_error("memory backend file size mismatch for " + path +
                               ": expected " + std::to_string(expected_size) +
                               ", got " + std::to_string(original_size_));
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept {
    *this = std::move(other);
  }

  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      path_ = std::move(other.path_);
      original_size_ = other.original_size_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~FileDescriptor() {
    close();
  }

  int get() const { return fd_; }
  std::uint64_t original_size() const { return original_size_; }
  const std::string& path() const { return path_; }

  void resize(std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error("memory backend file exceeds host off_t range: " + path_);
    }
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
      throw std::runtime_error("failed to resize memory backend file " + path_ + ": " +
                               std::strerror(errno));
    }
  }

  void reset_sparse(std::uint64_t size) {
    resize(0);
    resize(size);
  }

  void sync() const {
    if (::fsync(fd_) != 0) {
      throw std::runtime_error("failed to sync memory backend file " + path_ + ": " +
                               std::strerror(errno));
    }
  }

 private:
  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_ = -1;
  std::string path_;
  std::uint64_t original_size_ = 0;
};

void pread_fill(int fd, void* data, std::size_t size, std::uint64_t offset) {
  auto* out = static_cast<std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t count = ::pread(fd,
                            out + done,
                            size - done,
                            static_cast<off_t>(offset + done));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("memory backend pread failed: " +
                               std::string(std::strerror(errno)));
    }
    if (count == 0) {
      std::fill(out + done, out + size, 0);
      return;
    }
    done += static_cast<std::size_t>(count);
  }
}

void pwrite_all(int fd, const void* data, std::size_t size, std::uint64_t offset) {
  const auto* input = static_cast<const std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t count = ::pwrite(fd,
                             input + done,
                             size - done,
                             static_cast<off_t>(offset + done));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("memory backend pwrite failed: " +
                               std::string(std::strerror(errno)));
    }
    if (count == 0) {
      throw std::runtime_error(
          "memory backend pwrite made no progress");
    }
    done += static_cast<std::size_t>(count);
  }
}

DiskHeader make_header(std::size_t line_size, std::uint64_t capacity_bytes) {
  DiskHeader header;
  header.magic = kMetaMagic;
  header.version = kBackendVersion;
  header.record_size = static_cast<std::uint32_t>(sizeof(DiskLineMeta));
  header.line_size = line_size;
  header.capacity_bytes = capacity_bytes;
  return header;
}

void validate_header(const DiskHeader& header,
                     const std::string& path,
                     std::size_t line_size,
                     std::uint64_t capacity_bytes) {
  if (header.magic != kMetaMagic ||
      header.version != kBackendVersion ||
      header.record_size != sizeof(DiskLineMeta) ||
      header.line_size != line_size ||
      header.capacity_bytes != capacity_bytes) {
    throw std::runtime_error("incompatible memory backend metadata header: " + path);
  }
}

void validate_header_counters(const DiskHeader& header,
                              const std::string& path,
                              std::uint64_t line_count) {
  const std::uint64_t allocated = header.reserved[0];
  const std::uint64_t written = header.reserved[1];
  if (allocated > line_count || written > allocated) {
    throw std::runtime_error("invalid memory backend counters in metadata header: " + path);
  }
}

DiskLineMeta encode_meta(const BackendLine& line) {
  DiskLineMeta meta;
  meta.version = line.version;
  meta.last_writer_request_id = line.last_writer_request_id;
  meta.last_write_cycle = line.last_write_cycle;
  meta.last_access_cycle = line.last_access_cycle;
  meta.ecc_hamming = line.ecc_hamming;
  meta.ecc_error_injections = line.ecc_error_injections;
  meta.checksum = line.checksum;
  meta.ecc_parity_bits = line.ecc_parity_bits;
  meta.storage = {
      line.storage_key.stack,
      line.storage_key.channel,
      line.storage_key.pseudo_channel,
      line.storage_key.sid,
      line.storage_key.rank,
      line.storage_key.bank_group,
      line.storage_key.bank,
      line.storage_key.row,
      line.storage_key.column,
  };
  if (line.initialized) meta.flags |= kInitializedFlag;
  if (line.ecc_overall) meta.flags |= kEccOverallFlag;
  if (line.ecc_valid) meta.flags |= kEccValidFlag;
  if (line.ecc_uncorrectable) meta.flags |= kEccUncorrectableFlag;
  return meta;
}

void decode_meta(const DiskLineMeta& meta, BackendLine& line) {
  line.initialized = (meta.flags & kInitializedFlag) != 0;
  line.version = meta.version;
  line.last_writer_request_id = meta.last_writer_request_id;
  line.last_write_cycle = meta.last_write_cycle;
  line.last_access_cycle = meta.last_access_cycle;
  line.checksum = meta.checksum;
  line.ecc_hamming = meta.ecc_hamming;
  line.ecc_overall = (meta.flags & kEccOverallFlag) != 0;
  line.ecc_parity_bits = meta.ecc_parity_bits;
  line.ecc_valid = (meta.flags & kEccValidFlag) != 0;
  line.ecc_uncorrectable = (meta.flags & kEccUncorrectableFlag) != 0;
  line.ecc_error_injections = meta.ecc_error_injections;
  line.storage_key = StorageKey{
      meta.storage[0],
      meta.storage[1],
      meta.storage[2],
      meta.storage[3],
      meta.storage[4],
      meta.storage[5],
      meta.storage[6],
      meta.storage[7],
      meta.storage[8],
  };
}

void validate_line(Address base,
                   const BackendLine& line,
                   std::size_t line_size,
                   std::uint64_t capacity_bytes) {
  if (base % static_cast<Address>(line_size) != 0) {
    throw std::invalid_argument("memory backend address is not line aligned");
  }
  if (line.bytes.size() != line_size || line.initialized_mask.size() != line_size) {
    throw std::invalid_argument("memory backend line has incorrect payload or init-mask size");
  }
  if (capacity_bytes != 0 &&
      (base >= capacity_bytes || capacity_bytes - base < line_size)) {
    throw std::out_of_range("memory backend address exceeds configured capacity");
  }
}

class SparseMemoryBackend final : public MemoryBackend {
 public:
  SparseMemoryBackend(std::size_t line_size, std::uint64_t capacity_bytes)
      : line_size_(line_size), capacity_bytes_(capacity_bytes) {}

  MemoryBackendKind kind() const override { return MemoryBackendKind::Sparse; }
  std::size_t line_size() const override { return line_size_; }
  std::uint64_t capacity_bytes() const override { return capacity_bytes_; }
  std::uint64_t allocated_lines() const override { return lines_.size(); }
  std::uint64_t unique_written_lines() const override { return unique_written_lines_; }
  std::uint64_t address_scan_lines() const override { return lines_.size(); }

  bool load(Address base, BackendLine& line) const override {
    auto it = lines_.find(base);
    if (it == lines_.end()) return false;
    line = it->second;
    return true;
  }

  bool store(Address base, const BackendLine& line) override {
    validate_line(base, line, line_size_, capacity_bytes_);
    auto it = lines_.find(base);
    const bool inserted = it == lines_.end();
    const bool was_written = !inserted && it->second.version > 0;
    const bool is_written = line.version > 0;
    if (!inserted) {
      reverse_.erase(it->second.storage_key);
      it->second = line;
    } else {
      lines_.emplace(base, line);
    }
    reverse_[line.storage_key] = base;
    if (!was_written && is_written) unique_written_lines_++;
    if (was_written && !is_written) unique_written_lines_--;
    return inserted;
  }

  std::optional<Address> address_for_storage_key(const StorageKey& key) const override {
    auto it = reverse_.find(key);
    if (it == reverse_.end()) return std::nullopt;
    return it->second;
  }

  std::vector<Address> all_addresses() const override {
    std::vector<Address> addresses;
    addresses.reserve(lines_.size());
    for (const auto& [base, line] : lines_) {
      (void)line;
      addresses.push_back(base);
    }
    std::sort(addresses.begin(), addresses.end());
    return addresses;
  }

  void clear() override {
    lines_.clear();
    reverse_.clear();
    unique_written_lines_ = 0;
  }

  void flush() override {}

 private:
  std::size_t line_size_;
  std::uint64_t capacity_bytes_;
  std::unordered_map<Address, BackendLine> lines_;
  std::unordered_map<StorageKey, Address, StorageKeyHash> reverse_;
  std::uint64_t unique_written_lines_ = 0;
};

class MappedFile {
 public:
  MappedFile(const std::string& path, std::uint64_t size)
      : file_(path, size), size_(size) {
    map();
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  ~MappedFile() {
    unmap();
  }

  std::uint8_t* data() { return static_cast<std::uint8_t*>(mapping_); }
  const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(mapping_); }
  std::uint64_t original_size() const { return file_.original_size(); }
  const std::string& path() const { return file_.path(); }

  void reset_sparse() {
    unmap();
    file_.reset_sparse(size_);
    map();
  }

  void sync() const {
    if (::msync(mapping_, checked_size(size_, "mmap region"), MS_SYNC) != 0) {
      throw std::runtime_error("failed to sync mmap memory backend file " + file_.path() + ": " +
                               std::strerror(errno));
    }
    file_.sync();
  }

 private:
  void map() {
    mapping_ = ::mmap(nullptr,
                      checked_size(size_, "mmap region"),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      file_.get(),
                      0);
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      throw std::runtime_error("failed to mmap memory backend file " + file_.path() + ": " +
                               std::strerror(errno));
    }
  }

  void unmap() {
    if (mapping_ != nullptr) {
      ::munmap(mapping_, checked_size(size_, "mmap region"));
      mapping_ = nullptr;
    }
  }

  FileDescriptor file_;
  std::uint64_t size_;
  void* mapping_ = nullptr;
};

class MmapSparseMemoryBackend final : public MemoryBackend {
 public:
  MmapSparseMemoryBackend(std::size_t line_size, const MemoryBackendOptions& options)
      : line_size_(line_size),
        capacity_bytes_(options.capacity_bytes),
        line_count_(capacity_bytes_ / line_size_),
        init_bytes_(ceil_div(capacity_bytes_, 8)),
        presence_bytes_(ceil_div(line_count_, 8)),
        meta_bytes_(sizeof(DiskHeader) +
                    checked_mul(line_count_, sizeof(DiskLineMeta), "metadata")),
        data_(require_data_path(options), capacity_bytes_),
        init_(sidecar_path(options.init_file, options.data_file, ".init"), init_bytes_),
        meta_(sidecar_path(options.meta_file, options.data_file, ".meta"), meta_bytes_),
        presence_(sidecar_path(options.presence_file, options.data_file, ".present"),
                  checked_mul(presence_bytes_, 2, "presence maps")) {
    if (capacity_bytes_ == 0 || capacity_bytes_ % line_size_ != 0) {
      throw std::invalid_argument("mmap_sparse capacity must be a non-zero multiple of line_size");
    }
    initialize_or_validate_header();
  }

  MemoryBackendKind kind() const override { return MemoryBackendKind::MmapSparse; }
  std::size_t line_size() const override { return line_size_; }
  std::uint64_t capacity_bytes() const override { return capacity_bytes_; }
  std::uint64_t allocated_lines() const override { return allocated_lines_; }
  std::uint64_t unique_written_lines() const override { return unique_written_lines_; }
  std::uint64_t address_scan_lines() const override { return line_count_; }

  bool load(Address base, BackendLine& line) const override {
    const std::uint64_t index = checked_index(base);
    if (!bit_is_set(presence_.data(), index)) return false;
    const std::uint64_t byte_offset = index * line_size_;
    line.bytes.assign(data_.data() + byte_offset,
                      data_.data() + byte_offset + line_size_);
    line.initialized_mask.assign(line_size_, 0);
    for (std::size_t i = 0; i < line_size_; i++) {
      if (bit_is_set(init_.data(), byte_offset + i)) {
        line.initialized_mask[i] = 0xff;
      }
    }
    decode_meta(meta_records()[index], line);
    return true;
  }

  bool store(Address base, const BackendLine& line) override {
    validate_line(base, line, line_size_, capacity_bytes_);
    const std::uint64_t index = checked_index(base);
    const bool inserted = !bit_is_set(presence_.data(), index);
    const std::uint64_t byte_offset = index * line_size_;
    std::copy(line.bytes.begin(), line.bytes.end(), data_.data() + byte_offset);
    for (std::size_t i = 0; i < line_size_; i++) {
      set_bit(init_.data(), byte_offset + i, line.initialized_mask[i] != 0);
    }
    meta_records()[index] = encode_meta(line);
    if (inserted) {
      set_bit(presence_.data(), index, true);
      allocated_lines_++;
    }
    const bool was_written = bit_is_set(written_bits(), index);
    if (line.version > 0 && !was_written) {
      set_bit(written_bits(), index, true);
      unique_written_lines_++;
    } else if (line.version == 0 && was_written) {
      set_bit(written_bits(), index, false);
      unique_written_lines_--;
    }
    return inserted;
  }

  std::optional<Address> address_for_storage_key(const StorageKey& key) const override {
    const auto* records = meta_records();
    std::optional<Address> result;
    visit_set_bits(presence_.data(), line_count_, [&](std::uint64_t index) {
      BackendLine line;
      decode_meta(records[index], line);
      if (line.storage_key == key) {
        result = index * line_size_;
        return false;
      }
      return true;
    });
    return result;
  }

  std::vector<Address> all_addresses() const override {
    std::vector<Address> addresses;
    addresses.reserve(checked_size(allocated_lines_, "allocated line count"));
    visit_set_bits(presence_.data(), line_count_, [&](std::uint64_t index) {
      addresses.push_back(index * line_size_);
      return true;
    });
    return addresses;
  }

  void clear() override {
    data_.reset_sparse();
    init_.reset_sparse();
    meta_.reset_sparse();
    presence_.reset_sparse();
    const DiskHeader header = make_header(line_size_, capacity_bytes_);
    std::memcpy(meta_.data(), &header, sizeof(header));
    allocated_lines_ = 0;
    unique_written_lines_ = 0;
  }

  void flush() override {
    DiskHeader* header = reinterpret_cast<DiskHeader*>(meta_.data());
    header->reserved[0] = allocated_lines_;
    header->reserved[1] = unique_written_lines_;
    data_.sync();
    init_.sync();
    meta_.sync();
    presence_.sync();
  }

 private:
  std::uint64_t checked_index(Address base) const {
    if (base % static_cast<Address>(line_size_) != 0 ||
        base >= capacity_bytes_ ||
        capacity_bytes_ - base < line_size_) {
      throw std::out_of_range("mmap_sparse address exceeds configured capacity");
    }
    return base / line_size_;
  }

  DiskLineMeta* meta_records() {
    return reinterpret_cast<DiskLineMeta*>(meta_.data() + sizeof(DiskHeader));
  }

  const DiskLineMeta* meta_records() const {
    return reinterpret_cast<const DiskLineMeta*>(meta_.data() + sizeof(DiskHeader));
  }

  std::uint8_t* written_bits() {
    return presence_.data() + presence_bytes_;
  }

  const std::uint8_t* written_bits() const {
    return presence_.data() + presence_bytes_;
  }

  void initialize_or_validate_header() {
    if (meta_.original_size() == 0) {
      const DiskHeader header = make_header(line_size_, capacity_bytes_);
      std::memcpy(meta_.data(), &header, sizeof(header));
      return;
    }
    DiskHeader header;
    std::memcpy(&header, meta_.data(), sizeof(header));
    validate_header(header, meta_.path(), line_size_, capacity_bytes_);
    validate_header_counters(header, meta_.path(), line_count_);
    allocated_lines_ = header.reserved[0];
    unique_written_lines_ = header.reserved[1];
  }

  std::size_t line_size_;
  std::uint64_t capacity_bytes_;
  std::uint64_t line_count_;
  std::uint64_t init_bytes_;
  std::uint64_t presence_bytes_;
  std::uint64_t meta_bytes_;
  MappedFile data_;
  MappedFile init_;
  MappedFile meta_;
  MappedFile presence_;
  std::uint64_t allocated_lines_ = 0;
  std::uint64_t unique_written_lines_ = 0;
};

class ChunkFileMemoryBackend final : public MemoryBackend {
 public:
  ChunkFileMemoryBackend(std::size_t line_size, const MemoryBackendOptions& options)
      : line_size_(line_size),
        capacity_bytes_(options.capacity_bytes),
        line_count_(capacity_bytes_ / line_size_),
        chunk_size_(options.chunk_size_bytes),
        lines_per_chunk_(chunk_size_ / line_size_),
        cache_limit_(std::max<std::size_t>(1, options.chunk_cache_entries)),
        presence_bytes_(ceil_div(line_count_, 8)),
        data_(require_data_path(options), capacity_bytes_),
        init_(sidecar_path(options.init_file, options.data_file, ".init"),
              ceil_div(capacity_bytes_, 8)),
        meta_(sidecar_path(options.meta_file, options.data_file, ".meta"),
              sizeof(DiskHeader) + checked_mul(line_count_, sizeof(DiskLineMeta), "metadata")),
        presence_(sidecar_path(options.presence_file, options.data_file, ".present"),
                  checked_mul(presence_bytes_, 2, "presence maps")) {
    if (capacity_bytes_ == 0 || capacity_bytes_ % line_size_ != 0) {
      throw std::invalid_argument("chunk_file capacity must be a non-zero multiple of line_size");
    }
    if (chunk_size_ % line_size_ != 0 || chunk_size_ % 8 != 0) {
      throw std::invalid_argument("memory_chunk_size must be a multiple of line_size and 8");
    }
    initialize_or_validate_header();
  }

  ~ChunkFileMemoryBackend() override {
    try {
      // 正常仿真通过 MemoryImage::flush_backend() 显式传播失败；析构只兜底。
      flush();
    } catch (...) {
    }
  }

  MemoryBackendKind kind() const override { return MemoryBackendKind::ChunkFile; }
  std::size_t line_size() const override { return line_size_; }
  std::uint64_t capacity_bytes() const override { return capacity_bytes_; }
  std::uint64_t allocated_lines() const override { return allocated_lines_; }
  std::uint64_t unique_written_lines() const override { return unique_written_lines_; }
  std::uint64_t address_scan_lines() const override { return line_count_; }

  bool load(Address base, BackendLine& line) const override {
    const std::uint64_t index = checked_index(base);
    if (!bit_is_set(presence_.data(), index)) return false;
    CachedChunk& chunk = ensure_chunk(index / lines_per_chunk_);
    const std::size_t local_line = static_cast<std::size_t>(index % lines_per_chunk_);
    const std::size_t byte_offset = local_line * line_size_;
    line.bytes.assign(chunk.data.begin() + static_cast<std::ptrdiff_t>(byte_offset),
                      chunk.data.begin() + static_cast<std::ptrdiff_t>(byte_offset + line_size_));
    line.initialized_mask.assign(line_size_, 0);
    for (std::size_t i = 0; i < line_size_; i++) {
      if (bit_is_set(chunk.init.data(), byte_offset + i)) {
        line.initialized_mask[i] = 0xff;
      }
    }
    decode_meta(chunk.meta[local_line], line);
    return true;
  }

  bool store(Address base, const BackendLine& line) override {
    validate_line(base, line, line_size_, capacity_bytes_);
    const std::uint64_t index = checked_index(base);
    const bool inserted = !bit_is_set(presence_.data(), index);
    CachedChunk& chunk = ensure_chunk(index / lines_per_chunk_);
    const std::size_t local_line = static_cast<std::size_t>(index % lines_per_chunk_);
    const std::size_t byte_offset = local_line * line_size_;
    std::copy(line.bytes.begin(), line.bytes.end(), chunk.data.begin() + byte_offset);
    for (std::size_t i = 0; i < line_size_; i++) {
      set_bit(chunk.init.data(), byte_offset + i, line.initialized_mask[i] != 0);
    }
    chunk.meta[local_line] = encode_meta(line);
    chunk.dirty = true;
    if (inserted) {
      set_bit(presence_.data(), index, true);
      allocated_lines_++;
    }
    const bool was_written = bit_is_set(written_bits(), index);
    if (line.version > 0 && !was_written) {
      set_bit(written_bits(), index, true);
      unique_written_lines_++;
    } else if (line.version == 0 && was_written) {
      set_bit(written_bits(), index, false);
      unique_written_lines_--;
    }
    return inserted;
  }

  std::optional<Address> address_for_storage_key(const StorageKey& key) const override {
    std::optional<Address> result;
    visit_set_bits(presence_.data(), line_count_, [&](std::uint64_t index) {
      BackendLine line;
      if (load(index * line_size_, line) && line.storage_key == key) {
        result = index * line_size_;
        return false;
      }
      return true;
    });
    return result;
  }

  std::vector<Address> all_addresses() const override {
    std::vector<Address> addresses;
    addresses.reserve(checked_size(allocated_lines_, "allocated line count"));
    visit_set_bits(presence_.data(), line_count_, [&](std::uint64_t index) {
      addresses.push_back(index * line_size_);
      return true;
    });
    return addresses;
  }

  void clear() override {
    cache_.clear();
    allocated_lines_ = 0;
    unique_written_lines_ = 0;
    data_.reset_sparse(capacity_bytes_);
    init_.reset_sparse(ceil_div(capacity_bytes_, 8));
    meta_.reset_sparse(sizeof(DiskHeader) +
                       checked_mul(line_count_, sizeof(DiskLineMeta), "metadata"));
    presence_.reset_sparse();
    const DiskHeader header = make_header(line_size_, capacity_bytes_);
    pwrite_all(meta_.get(), &header, sizeof(header), 0);
  }

  void flush() override {
    for (auto& [index, chunk] : cache_) {
      write_chunk(index, chunk);
    }
    DiskHeader header;
    pread_fill(meta_.get(), &header, sizeof(header), 0);
    header.reserved[0] = allocated_lines_;
    header.reserved[1] = unique_written_lines_;
    pwrite_all(meta_.get(), &header, sizeof(header), 0);
    data_.sync();
    init_.sync();
    meta_.sync();
    presence_.sync();
  }

 private:
  struct CachedChunk {
    ByteVector data;
    ByteVector init;
    std::vector<DiskLineMeta> meta;
    std::uint64_t last_use = 0;
    bool dirty = false;
  };

  std::uint64_t checked_index(Address base) const {
    if (base % static_cast<Address>(line_size_) != 0 ||
        base >= capacity_bytes_ ||
        capacity_bytes_ - base < line_size_) {
      throw std::out_of_range("chunk_file address exceeds configured capacity");
    }
    return base / line_size_;
  }

  std::size_t lines_in_chunk(std::uint64_t chunk_index) const {
    const std::uint64_t first = chunk_index * lines_per_chunk_;
    return checked_size(std::min<std::uint64_t>(lines_per_chunk_, line_count_ - first),
                        "chunk line count");
  }

  CachedChunk& ensure_chunk(std::uint64_t chunk_index) const {
    auto it = cache_.find(chunk_index);
    if (it != cache_.end()) {
      it->second.last_use = ++use_clock_;
      return it->second;
    }
    if (cache_.size() >= cache_limit_) {
      auto victim = std::min_element(cache_.begin(), cache_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second.last_use < rhs.second.last_use;
      });
      write_chunk(victim->first, victim->second);
      cache_.erase(victim);
    }

    const std::size_t chunk_lines = lines_in_chunk(chunk_index);
    const std::size_t chunk_bytes = chunk_lines * line_size_;
    CachedChunk chunk;
    chunk.data.resize(chunk_bytes, 0);
    chunk.init.resize(checked_size(ceil_div(chunk_bytes, 8), "chunk init map"), 0);
    chunk.meta.resize(chunk_lines);
    chunk.last_use = ++use_clock_;

    const std::uint64_t first_line = chunk_index * lines_per_chunk_;
    const std::uint64_t byte_offset = first_line * line_size_;
    pread_fill(data_.get(), chunk.data.data(), chunk.data.size(), byte_offset);
    pread_fill(init_.get(), chunk.init.data(), chunk.init.size(), byte_offset / 8);
    pread_fill(meta_.get(),
               chunk.meta.data(),
               chunk.meta.size() * sizeof(DiskLineMeta),
               sizeof(DiskHeader) + first_line * sizeof(DiskLineMeta));
    return cache_.emplace(chunk_index, std::move(chunk)).first->second;
  }

  void write_chunk(std::uint64_t chunk_index, CachedChunk& chunk) const {
    if (!chunk.dirty) return;
    const std::uint64_t first_line = chunk_index * lines_per_chunk_;
    const std::uint64_t byte_offset = first_line * line_size_;
    pwrite_all(data_.get(), chunk.data.data(), chunk.data.size(), byte_offset);
    pwrite_all(init_.get(), chunk.init.data(), chunk.init.size(), byte_offset / 8);
    pwrite_all(meta_.get(),
               chunk.meta.data(),
               chunk.meta.size() * sizeof(DiskLineMeta),
               sizeof(DiskHeader) + first_line * sizeof(DiskLineMeta));
    chunk.dirty = false;
  }

  void initialize_or_validate_header() {
    if (meta_.original_size() == 0) {
      const DiskHeader header = make_header(line_size_, capacity_bytes_);
      pwrite_all(meta_.get(), &header, sizeof(header), 0);
      return;
    }
    DiskHeader header;
    pread_fill(meta_.get(), &header, sizeof(header), 0);
    validate_header(header, meta_.path(), line_size_, capacity_bytes_);
    validate_header_counters(header, meta_.path(), line_count_);
    allocated_lines_ = header.reserved[0];
    unique_written_lines_ = header.reserved[1];
  }

  std::uint8_t* written_bits() {
    return presence_.data() + presence_bytes_;
  }

  const std::uint8_t* written_bits() const {
    return presence_.data() + presence_bytes_;
  }

  std::size_t line_size_;
  std::uint64_t capacity_bytes_;
  std::uint64_t line_count_;
  std::size_t chunk_size_;
  std::uint64_t lines_per_chunk_;
  std::size_t cache_limit_;
  std::uint64_t presence_bytes_;
  mutable FileDescriptor data_;
  mutable FileDescriptor init_;
  mutable FileDescriptor meta_;
  MappedFile presence_;
  mutable std::unordered_map<std::uint64_t, CachedChunk> cache_;
  mutable std::uint64_t use_clock_ = 0;
  std::uint64_t allocated_lines_ = 0;
  std::uint64_t unique_written_lines_ = 0;
};

}  // namespace

std::string to_string(MemoryBackendKind kind) {
  switch (kind) {
    case MemoryBackendKind::Sparse: return "sparse";
    case MemoryBackendKind::MmapSparse: return "mmap_sparse";
    case MemoryBackendKind::ChunkFile: return "chunk_file";
  }
  return "sparse";
}

MemoryBackendKind parse_memory_backend_kind(const std::string& value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (normalized == "sparse") return MemoryBackendKind::Sparse;
  if (normalized == "mmap_sparse" || normalized == "mmap") return MemoryBackendKind::MmapSparse;
  if (normalized == "chunk_file" || normalized == "chunk") return MemoryBackendKind::ChunkFile;
  throw std::invalid_argument("unsupported memory backend: " + value +
                              " (implemented: sparse, mmap_sparse, chunk_file)");
}

std::unique_ptr<MemoryBackend> make_memory_backend(std::size_t line_size,
                                                   const MemoryBackendOptions& options) {
  if (line_size == 0) {
    throw std::invalid_argument("memory backend line_size must be > 0");
  }
  if (options.kind != MemoryBackendKind::Sparse) {
    if (options.capacity_bytes == 0 ||
        options.capacity_bytes % static_cast<std::uint64_t>(line_size) != 0) {
      throw std::invalid_argument(
          "file-backed memory capacity must be a non-zero multiple of line_size");
    }
    if (options.data_file.empty()) {
      throw std::invalid_argument("file-backed memory backend requires memory_data_file");
    }
  }
  if (options.kind == MemoryBackendKind::ChunkFile &&
      (options.chunk_size_bytes < line_size ||
       options.chunk_size_bytes % line_size != 0 ||
       options.chunk_size_bytes % 8 != 0)) {
    throw std::invalid_argument(
        "memory_chunk_size must be >= line_size and a multiple of line_size and 8");
  }
  switch (options.kind) {
    case MemoryBackendKind::Sparse:
      return std::make_unique<SparseMemoryBackend>(line_size, options.capacity_bytes);
    case MemoryBackendKind::MmapSparse:
      return std::make_unique<MmapSparseMemoryBackend>(line_size, options);
    case MemoryBackendKind::ChunkFile:
      return std::make_unique<ChunkFileMemoryBackend>(line_size, options);
  }
  throw std::invalid_argument("unsupported memory backend");
}

}  // namespace hbm_sim
