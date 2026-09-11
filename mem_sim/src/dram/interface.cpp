// Interface accounting model：把协议保护、metadata lane 和命令级 CA parity
// 统一折算成 bit/byte 开销。这个模块故意保持纯函数，便于 tests 和 CLI 复用。
#include "hbm_sim/dram/interface.hpp"

#include <algorithm>

namespace hbm_sim {

int lpddr_metadata_lane_bits_per_request(const DramSpec& spec) {
  if (!spec.lpddr_family) {
    return 0;
  }
  return std::max(std::max(0, spec.lpddr_dbi_bits_per_request),
                  std::max(0, spec.lpddr_link_ecc_bits_per_request));
}

int request_protocol_overhead_bits(const DramSpec& spec) {
  return std::max(0, spec.metadata_bits_per_request) +
         std::max(0, spec.ecc_bits_per_request) +
         std::max(0, spec.hbm_link_crc_bits_per_request) +
         std::max(0, spec.hbm_ras_metadata_bits_per_request) +
         std::max(0, spec.hbm_ecc_bits_per_request) +
         lpddr_metadata_lane_bits_per_request(spec);
}

int request_interface_bytes(const DramSpec& spec, std::size_t payload_bytes) {
  const std::size_t payload_bits = std::max<std::size_t>(1, payload_bytes) * 8;
  const std::size_t total_bits =
      payload_bits + static_cast<std::size_t>(request_protocol_overhead_bits(spec));
  return static_cast<int>((total_bits + 7) / 8);
}

int request_interface_bytes(const DramSpec& spec) {
  const std::size_t payload_bytes =
      static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  return request_interface_bytes(spec, payload_bytes);
}

int command_interface_overhead_bits(const DramSpec& spec, Command command) {
  if (command == Command::NOP) {
    return 0;
  }
  int bits = 0;
  if (spec.lpddr_family && spec.lpddr_ca_parity_enabled) {
    bits += std::max(0, spec.lpddr_ca_parity_bits_per_command);
  }
  return bits;
}

}  // namespace hbm_sim
