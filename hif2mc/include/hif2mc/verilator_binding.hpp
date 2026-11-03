#pragma once
#include "hif2mc/hif_signals.hpp"
#include <cstddef>

namespace hif2mc {
// Thin, stateless connection to a Verilated top exposing the documented HIF
// port names. A differently named top needs an explicit wrapper/port mapping.
// Do not use internal Vstorage_bridge_top members as a substitute for HIF.
template <class Top> Inputs sample_hif_inputs(const Top &top) {
  Inputs i;
  i.core_ddrc_rstn = top.core_ddrc_rstn;
  i.hif_cmd_valid = top.hif_cmd_valid;
  i.hif_cmd_type = top.hif_cmd_type;
  i.hif_cmd_addr = top.hif_cmd_addr;
  i.hif_cmd_pri = top.hif_cmd_pri;
  i.hif_cmd_token = top.hif_cmd_token;
  i.hif_cmd_length = top.hif_cmd_length;
  i.hif_cmd_wdata_ptr = top.hif_cmd_wdata_ptr;
  i.hif_cmd_autopre = top.hif_cmd_autopre;
  i.hif_cmd_latency = top.hif_cmd_latency;
  i.hif_wdata_valid = top.hif_wdata_valid;
  i.hif_wdata_mask = top.hif_wdata_mask;
  i.hif_wdata_end = top.hif_wdata_end;
  for (std::size_t b = 0; b < 32; ++b)
    i.hif_wdata[b] =
        static_cast<std::uint8_t>(top.hif_wdata[b / 4] >> (8 * (b % 4)));
  i.hif_go2critical_lpr = top.hif_go2critical_lpr;
  i.hif_go2critical_hpr = top.hif_go2critical_hpr;
  i.hif_go2critical_wr = top.hif_go2critical_wr;
  return i;
}
template <class Top> void drive_hif_outputs(Top &top, const Outputs &o) {
  top.hif_cmd_stall = o.hif_cmd_stall;
  top.hif_wdata_stall = o.hif_wdata_stall;
  top.hif_lpr_credit = o.hif_lpr_credit;
  top.hif_hpr_credit = o.hif_hpr_credit;
  top.hif_wr_credit = o.hif_wr_credit;
  top.hif_wrecc_credit = o.hif_wrecc_credit;
  top.hif_wdata_ptr_valid = o.hif_wdata_ptr_valid;
  top.hif_wdata_ptr = o.hif_wdata_ptr;
  top.hif_wdata_ptr_addr_err = o.hif_wdata_ptr_addr_err;
  top.hif_rdata_valid = o.hif_rdata_valid;
  top.hif_rdata_end = o.hif_rdata_end;
  top.hif_rdata_token = o.hif_rdata_token;
  for (std::size_t w = 0; w < 8; ++w) {
    std::uint32_t word = 0;
    for (std::size_t b = 0; b < 4; ++b)
      word |= std::uint32_t(o.hif_rdata[4 * w + b]) << (8 * b);
    top.hif_rdata[w] = word;
  }
  top.hif_rdata_addr_err = o.hif_rdata_addr_err;
  top.hif_rdata_uncorr_ecc_err = o.hif_rdata_uncorr_ecc_err;
  top.hif_rdata_crc_err = o.hif_rdata_crc_err;
}
} // namespace hif2mc
