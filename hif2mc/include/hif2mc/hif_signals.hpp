#pragma once
#include <array>
#include <cstdint>

namespace hif2mc {
// v1 wire profile: 256-bit HIF data; byte i is vector[8*i +: 8].
using DataBeat = std::array<std::uint8_t, 32>;
enum class Command : std::uint8_t { Write = 0, Read = 1, Rmw = 2 };

// Directions are relative to the model (MC side). clk is represented by tick().
struct Inputs {
  bool core_ddrc_rstn = true;
  bool hif_cmd_valid = false;
  std::uint8_t hif_cmd_type = 0;
  std::uint64_t hif_cmd_addr = 0;
  std::uint8_t hif_cmd_pri = 0;
  std::uint64_t hif_cmd_token = 0;
  std::uint8_t hif_cmd_length = 0;
  std::uint32_t hif_cmd_wdata_ptr = 0;
  bool hif_cmd_autopre = false;
  std::uint16_t hif_cmd_latency = 0;
  bool hif_wdata_valid = false;
  DataBeat hif_wdata{};
  std::uint32_t hif_wdata_mask = 0;
  bool hif_wdata_end = false;
  bool hif_go2critical_lpr = false;
  bool hif_go2critical_hpr = false;
  bool hif_go2critical_wr = false;
};
struct Outputs {
  bool hif_cmd_stall = true;
  bool hif_wdata_stall = true;
  std::uint8_t hif_lpr_credit = 0;
  std::uint8_t hif_hpr_credit = 0;
  bool hif_wr_credit = false;
  std::uint8_t hif_wrecc_credit = 0;
  bool hif_wdata_ptr_valid = false;
  std::uint32_t hif_wdata_ptr = 0;
  bool hif_wdata_ptr_addr_err = false;
  bool hif_rdata_valid = false;
  bool hif_rdata_end = false;
  std::uint64_t hif_rdata_token = 0;
  DataBeat hif_rdata{};
  bool hif_rdata_addr_err = false;
  bool hif_rdata_uncorr_ecc_err = false;
  bool hif_rdata_crc_err = false;
};
} // namespace hif2mc
