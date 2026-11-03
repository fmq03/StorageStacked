#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hif2mc {
using Bytes = std::vector<std::uint8_t>;
enum class Status {
  Ok,
  EccCorrected,
  Uninitialized,
  DataMismatch,
  Retry,
  Timeout,
  EccUncorrectable,
  Invalid
};
struct BackendRequest {
  std::uint64_t id = 0, host_id = 0, address = 0;
  std::uint32_t index = 0, count = 1;
  std::size_t bytes = 0;
  bool write = false;
  int qos = 0;
  Bytes data, mask;
};
struct BackendEvent {
  enum class Kind { Issued, Completed } kind = Kind::Issued;
  std::uint64_t host_id = 0;
  std::uint32_t index = 0;
  bool write = false;
  Status status = Status::Ok;
  Bytes data, initialized_mask;
};
// No simulator-specific objects or HIF-specific concepts cross this boundary.
class Backend {
public:
  virtual ~Backend() = default;
  virtual bool try_submit(const BackendRequest &) = 0;
  virtual void tick() = 0;
  virtual std::optional<BackendEvent> pop_event() = 0;
  virtual std::size_t transaction_bytes() const = 0;
  virtual double tick_period_ps() const = 0;
  virtual std::uint64_t capacity_bytes() const = 0;
  virtual bool idle() const = 0;
  virtual void finish() = 0;
};
} // namespace hif2mc
