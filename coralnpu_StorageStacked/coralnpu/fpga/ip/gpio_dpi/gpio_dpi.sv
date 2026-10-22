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

module gpio_dpi #(
    parameter int Width = 8
) (
    input logic clk_i,
    input logic rst_ni,
    input logic [Width-1:0] gpio_o,
    input logic [Width-1:0] gpio_en_o,
    output logic [Width-1:0] gpio_i
);

  import "DPI-C" function chandle gpio_dpi_init();
  import "DPI-C" function void gpio_dpi_close(chandle c_context);
  import "DPI-C" function void gpio_dpi_tick(
    chandle c_context,
    int gpio_o,
    int gpio_en_o,
    output int gpio_i
  );

  chandle c_context;
  logic [31:0] gpio_i_int;

  assign gpio_i = gpio_i_int[Width-1:0];

  initial begin
    c_context = gpio_dpi_init();
  end

  always_ff @(posedge clk_i) begin
    if (rst_ni) begin
      int gpio_in_val;
      gpio_dpi_tick(c_context, {32'b0 | gpio_o}, {32'b0 | gpio_en_o}, gpio_in_val);
      gpio_i_int <= gpio_in_val;
    end else begin
      gpio_i_int <= 0;
    end
  end

  final begin
    gpio_dpi_close(c_context);
  end

endmodule
