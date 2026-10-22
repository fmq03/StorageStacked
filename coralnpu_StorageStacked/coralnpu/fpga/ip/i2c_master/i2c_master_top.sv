// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module i2c_master_top
  import i2c_master_pkg::*;
  import tlul_pkg::*;
  import coralnpu_tlul_pkg_32::*;
#(
    parameter int FifoDepth = 16
) (
    input clk_i,
    input rst_ni,

    // TileLink-UL Interface
    input  coralnpu_tlul_pkg_32::tl_h2d_t tl_i,
    output coralnpu_tlul_pkg_32::tl_d2h_t tl_o,

    // I2C Interface
    input scl_i,
    output logic scl_o,
    output logic scl_en_o,
    input sda_i,
    output logic sda_o,
    output logic sda_en_o
);

  i2c_master #(
      .FifoDepth(FifoDepth)
  ) i_core (
      .clk_i,
      .rst_ni,
      .tl_i,
      .tl_o,
      .scl_i,
      .scl_o,
      .scl_en_o,
      .sda_i,
      .sda_o,
      .sda_en_o,
      .intr_o(),
      .intg_error_i(1'b0)
  );

endmodule
