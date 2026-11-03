#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace hif2mc {
// CONTRACT: these are project policies, not a vendor HIF compliance claim.
enum class CreditPolicy { Unspecified, McCommandIssued, BackendCompleted };
struct Config {
  std::size_t address_unit_bytes = 0;
  std::size_t command_bytes = 0;
  std::uint64_t hif_period_ps = 0;
  std::size_t lpr_credits = 0, hpr_credits = 0, write_credits = 0;
  std::size_t max_inflight = 0;
  CreditPolicy credit_policy = CreditPolicy::Unspecified;
  unsigned address_bits = 40, token_bits = 53, pointer_bits = 19;
  unsigned pointer_delay_cycles = 2;
  std::size_t write_data_fifo_beats = 2;
  unsigned write_assembly_interval = 1;
  bool enable_rmw = false;
  bool reuse_credit_same_edge = true;
  bool check_stalled_inputs = true;

  // Explicit, named example: WPS BL16/FREQ_RATIO=4/DRAM_DATA_WIDTH=32.
  // Capacities and 2ns clock are project choices. Never implicit defaults.
  static Config reference_64b() {
    Config c;
    c.address_unit_bytes = 4;
    c.command_bytes = 64;
    c.hif_period_ps = 2000;
    c.lpr_credits = 24;
    c.hpr_credits = 8;
    c.write_credits = 32;
    c.max_inflight = 128;
    c.credit_policy = CreditPolicy::McCommandIssued;
    c.enable_rmw = true;
    return c;
  }
  void validate() const;
};
class ProtocolError : public std::runtime_error {
public:
  explicit ProtocolError(const std::string &what) : std::runtime_error(what) {}
};
} // namespace hif2mc
