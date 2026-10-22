// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module clk_table
  import tlul_pkg::*;
  import coralnpu_tlul_pkg_32::*;
#(
    parameter int MainFreqMhz = 50,
    parameter int IspFreqMhz  = 10,
    parameter int SpimFreqMhz = 100
) (
    input clk_i,
    input rst_ni,

    // TileLink-UL Interface
    input  coralnpu_tlul_pkg_32::tl_h2d_t tl_i,
    output coralnpu_tlul_pkg_32::tl_d2h_t tl_o,

    // Integrity Error Signal
    output logic intg_error_o
);

  typedef enum logic [1:0] {
    AddrMagic = 2'h0,
    AddrMain  = 2'h1,
    AddrIsp   = 2'h2,
    AddrSpim  = 2'h3
  } addr_e;

  assign intg_error_o = 1'b0;

  logic [31:0] rdata;
  always_comb begin
    rdata = 32'h0;
    unique case (tl_i.a_address[3:2])
      AddrMagic: rdata = 32'h434C4B54;  // "CLKT"
      AddrMain:  rdata = 32'(MainFreqMhz);
      AddrIsp:   rdata = 32'(IspFreqMhz);
      AddrSpim:  rdata = 32'(SpimFreqMhz);
      default:   rdata = 32'h0;
    endcase
  end

  tl_d_op_e d_opcode;
  assign d_opcode = (tl_i.a_opcode == Get) ? AccessAckData : AccessAck;

  // Simple combinational response (matches standard TLUL leaf pattern).
  // Xbar owns integrity gen on the device D channel, so leave user fields 0.
  assign tl_o.a_ready = tl_i.d_ready;
  assign tl_o.d_valid = tl_i.a_valid;
  assign tl_o.d_opcode = d_opcode;
  assign tl_o.d_param = 3'h0;
  assign tl_o.d_size = tl_i.a_size;
  assign tl_o.d_source = tl_i.a_source;
  assign tl_o.d_sink = 1'b0;
  assign tl_o.d_data = rdata;
  assign tl_o.d_user.rsp_intg = '0;
  assign tl_o.d_user.data_intg = '0;
  assign tl_o.d_error = 1'b0;
endmodule
