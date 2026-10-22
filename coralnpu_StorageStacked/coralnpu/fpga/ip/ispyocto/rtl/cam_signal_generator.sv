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

/**
 * Module: cam_signal_generator
 * Description: Recreates camera signal patterns (PCLK, HSYNC/LVLD, VSYNC/FVLD)
 * based on CSV timing analysis from scope capture of camera waveform, with
 * specific gating and delay requirements.
 */

module cam_signal_generator (
    input wire clk_i,  // External clock
    input wire rst_ni,  // Asynchronous active-low reset
    output wire pclk,  // Gated clock output
    output reg lvld,  // Line Valid (HSYNC)
    output reg fvld,  // Frame Valid (VSYNC)
    output reg [7:0] data  // Pixel data output
);

  // Timing Parameters (Derived from CSV analysis)
  parameter H_ACTIVE = 324;
  parameter H_BLANK = 52;
  parameter H_TOTAL = 376;
  parameter V_ACTIVE = 244;

  // Raw Image Support
  parameter USE_REF_IMG = 0;
  parameter IMG_FILE = "grey_bars_320x240.raw";

  // Reset Delay
  parameter RST_DELAY_CYCLES = 0;

  // Sequence Delays
  parameter PCLK_GATED_DELAY = 2000;  // PCLK completely off
  parameter PCLK_PRE_DELAY = 200;  // PCLK on before signals
  parameter PCLK_POST_DELAY = 2;  // PCLK remains on after signals drop

  // Internal State Machine
  typedef enum reg [1:0] {
    ST_GATED_IDLE,  // PCLK OFF
    ST_PRE_ACTIVE,  // PCLK ON, Signals LOW
    ST_FRAME_DATA,  // PCLK ON, Signals ACTIVE
    ST_POST_ACTIVE  // PCLK ON, Signals LOW (Trailing cycles)
  } state_t;

  state_t state;

  // 10x Clock Divider (50MHz -> 5MHz)
  reg [3:0] clk_div_cnt;
  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      clk_div_cnt <= 4'd0;
    end else begin
      clk_div_cnt <= (clk_div_cnt == 4'd9) ? 4'd0 : clk_div_cnt + 4'd1;
    end
  end
  wire clk_div = (clk_div_cnt < 4'd5);

  // Delayed Reset Logic
  logic rst_n;
  reg [31:0] rst_delay_cnt;

  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rst_n <= 1'b0;
      rst_delay_cnt <= 0;
    end else begin
      if (rst_delay_cnt < RST_DELAY_CYCLES) begin
        rst_delay_cnt <= rst_delay_cnt + 1;
        rst_n <= 1'b0;
      end else begin
        rst_n <= 1'b1;
      end
    end
  end

  // Counters
  reg [12:0] delay_cnt;
  reg [9:0] h_cnt;
  reg [8:0] v_cnt;

  // Memory for Raw Image Data
  reg [7:0] img_mem[0:(H_ACTIVE*V_ACTIVE)-1];
  integer img_fd;

  initial begin
    if (USE_REF_IMG) begin
      img_fd = $fopen(IMG_FILE, "rb");
      if (img_fd == 0) begin
        $display("[cam_signal_generator] ERROR: Failed to open image file: %s", IMG_FILE);
      end else begin
        $display("[cam_signal_generator] Loading reference image from %s", IMG_FILE);
        void'($fread(img_mem, img_fd));
        $fclose(img_fd);
      end
    end
  end

  // ---------------------------------------------------------
  // 1. Glitch-Free Clock Gating Logic
  // ---------------------------------------------------------
  // We enable PCLK for PRE, DATA, and POST states.
  // Latching on negedge of internal_clk prevents glitches on PCLK posedge.
  reg pclk_en;
  always @(negedge clk_div or negedge rst_n) begin
    if (!rst_n) begin
      pclk_en <= 1'b0;
    end else begin
      pclk_en <= (state == ST_PRE_ACTIVE || state == ST_FRAME_DATA || state == ST_POST_ACTIVE);
    end
  end

  assign pclk = clk_div & pclk_en;

  // ---------------------------------------------------------
  // 2. Control State Machine (Runs on Internal Master Clock)
  // ---------------------------------------------------------
  always @(posedge clk_div or negedge rst_n) begin
    if (!rst_n) begin
      state     <= ST_GATED_IDLE;
      delay_cnt <= 0;
      h_cnt     <= 0;
      v_cnt     <= 0;
    end else begin
      case (state)

        // PCLK is gated (off). Wait 5000 master clock cycles.
        ST_GATED_IDLE: begin
          h_cnt <= 0;
          v_cnt <= 0;
          if (delay_cnt < PCLK_GATED_DELAY - 1) begin
            delay_cnt <= delay_cnt + 1;
          end else begin
            delay_cnt <= 0;
            state     <= ST_PRE_ACTIVE;
          end
        end

        // PCLK starts running. Wait 200 cycles before driving signals.
        ST_PRE_ACTIVE: begin
          if (delay_cnt < PCLK_PRE_DELAY - 1) begin
            delay_cnt <= delay_cnt + 1;
          end else begin
            delay_cnt <= 0;
            state     <= ST_FRAME_DATA;
          end
        end

        // Generate active frame pattern (HSYNC/VSYNC pulses)
        ST_FRAME_DATA: begin
          if (h_cnt < H_TOTAL - 1) begin
            h_cnt <= h_cnt + 1;
          end else begin
            h_cnt <= 0;
            if (v_cnt < V_ACTIVE - 1) begin
              v_cnt <= v_cnt + 1;
            end else begin
              // Last pixel of last line reached
              v_cnt <= 0;
              delay_cnt <= 0;
              state <= ST_POST_ACTIVE;
            end
          end
        end

        // Trailing cycles: PCLK continues after fvld/lvld are low.
        ST_POST_ACTIVE: begin
          if (delay_cnt < PCLK_POST_DELAY - 1) begin
            delay_cnt <= delay_cnt + 1;
          end else begin
            delay_cnt <= 0;
            state     <= ST_GATED_IDLE;  // Loop back to start
          end
        end

        default: state <= ST_GATED_IDLE;
      endcase
    end
  end

  // ---------------------------------------------------------
  // 3. Output Signal Drivers (Drive on Falling Edge of PCLK)
  // ---------------------------------------------------------
  // By updating on negedge of clk_i, we ensure lvld/fvld
  // transition at the falling edge of pclk.
  always @(negedge clk_div or negedge rst_n) begin
    if (!rst_n) begin
      lvld <= 1'b0;
      fvld <= 1'b0;
      data <= 8'h00;
    end else begin
      if (state == ST_FRAME_DATA) begin
        fvld <= (v_cnt < V_ACTIVE);
        lvld <= (h_cnt < H_ACTIVE) && (v_cnt < V_ACTIVE);

        if (lvld) begin
          if (USE_REF_IMG) begin
            data <= img_mem[v_cnt*H_ACTIVE+h_cnt];
          end else begin
            data <= 8'h6F;
          end
        end else begin
          data <= 8'h00;
        end
      end else begin
        lvld <= 1'b0;
        fvld <= 1'b0;
        data <= 8'h00;
      end
    end
  end

endmodule
