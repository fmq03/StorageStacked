#pragma once
#include "hif2mc/backend.hpp"
#include "hif2mc/config.hpp"
#include "hif2mc/hif_signals.hpp"
#include <array>
#include <memory>

namespace hif2mc {
struct Statistics {
  std::uint64_t accepted_reads = 0, accepted_writes = 0, accepted_rmws = 0;
  std::uint64_t completed_reads = 0, completed_writes = 0, completed_rmws = 0;
  std::uint64_t submitted_children = 0, submission_retries = 0;
  std::uint64_t backend_ticks = 0, command_stalls = 0, write_data_stalls = 0;
  std::uint64_t credit_fallbacks = 0, uninitialized_reads = 0;
  std::uint64_t address_errors = 0;
  std::array<std::uint64_t, 3> returned_credits{};
};
class Adapter {
public:
  Adapter(Config, std::unique_ptr<Backend>);
  ~Adapter();
  Adapter(const Adapter &) = delete;
  Adapter &operator=(const Adapter &) = delete;
  // Pure combinational evaluation: safe to call repeatedly before an edge.
  Outputs evaluate(const Inputs &) const;
  // Consume the outputs from evaluate(inputs) at exactly one rising edge.
  // Outputs prepared here are sampled at the NEXT rising edge.
  void tick(const Inputs &);
  bool idle() const;
  void finish();
  std::uint64_t cycle() const;
  const Statistics &stats() const;
  const Backend &backend() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hif2mc
