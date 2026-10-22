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

module s25fl512s_dpi (
    input  logic clk_i,
    input  logic rst_ni,
    input  logic sck_i,
    input  logic csb_i,
    input  logic mosi_i,
    input  logic flash_rst_ni,
    output logic miso_o
);

  import "DPI-C" function chandle s25fl512s_dpi_init();
  import "DPI-C" function void s25fl512s_dpi_close(chandle c_context);
  import "DPI-C" function void s25fl512s_dpi_reset(chandle c_context);

  chandle c_context;

  logic   miso_q;
  assign miso_o = miso_q;

  initial begin
    c_context = s25fl512s_dpi_init();
  end

  import "DPI-C" task s25fl512s_dpi_tick(
    chandle c_context,
    input bit sck,
    input bit csb,
    input bit mosi,
    input bit rst_ni,
    output bit miso
  );

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      s25fl512s_dpi_reset(c_context);
      miso_q <= 1'b0;
    end else begin
      bit miso_next;
      s25fl512s_dpi_tick(c_context, sck_i, csb_i, mosi_i, flash_rst_ni, miso_next);
      miso_q <= miso_next;
    end
  end

  final begin
    s25fl512s_dpi_close(c_context);
  end

endmodule
