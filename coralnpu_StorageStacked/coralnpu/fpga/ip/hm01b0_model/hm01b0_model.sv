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

/* verilator lint_off UNUSEDSIGNAL */
module hm01b0_model #(
  parameter logic [6:0] I2C_ADDR = 7'h24
) (
  input clk_i, input rst_ni,
  input scl_i, input sda_i,
  output logic sda_en_o
);
  typedef enum logic [2:0] { Idle, Addr, AckAddr, Data, AckData } slave_state_e;
  slave_state_e state;
  logic [7:0] shift_reg;
  logic [7:0] regs [65536];
  logic [15:0] addr_ptr;
  logic [3:0] bit_cnt;
  logic [1:0] addr_byte_cnt;
  logic scl_q, sda_q;
  logic rw;
  logic start_det, stop_det, scl_rise, scl_fall;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      scl_q <= 1; sda_q <= 1;
    end else begin
      scl_q <= scl_i; sda_q <= sda_i;
    end
  end

  assign start_det = scl_q && sda_q && !sda_i;
  assign stop_det  = scl_q && !sda_q && sda_i;
  assign scl_rise  = !scl_q && scl_i;
  assign scl_fall  = scl_q && !scl_i;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state <= Idle; sda_en_o <= 0; bit_cnt <= 0; rw <= 0; shift_reg <= 0;
      addr_byte_cnt <= 0; addr_ptr <= 0;
      for (int i=0; i<65536; i++) regs[i] = 8'h0;
      regs[16'h0000] = 8'h01; // HM01B0_MODEL_ID_H
      regs[16'h0001] = 8'hB0; // HM01B0_MODEL_ID_L
    end else if (start_det) begin
      $display("[%0t] HM01B0 Model: START detected", $time);
      state <= Addr; bit_cnt <= 0; sda_en_o <= 0;
    end else if (stop_det) begin
      $display("[%0t] HM01B0 Model: STOP detected", $time);
      state <= Idle; sda_en_o <= 0;
    end else begin
      case (state)
        Addr: begin
          if (scl_rise) begin
            shift_reg <= {shift_reg[6:0], sda_i};
            if (bit_cnt == 7) begin
              if ({shift_reg[6:0], sda_i}[7:1] == I2C_ADDR) begin
                $display("[%0t] HM01B0 Model: Addr Match! rw=%b", $time, {shift_reg[6:0], sda_i}[0]);
                state <= AckAddr; rw <= {shift_reg[6:0], sda_i}[0];
                if (!{shift_reg[6:0], sda_i}[0]) addr_byte_cnt <= 0;
              end else state <= Idle;
              bit_cnt <= 0;
            end else bit_cnt <= bit_cnt + 1;
          end
        end
        AckAddr: begin
          if (scl_fall) sda_en_o <= 1;
          else if (scl_rise) begin
            state <= Data; bit_cnt <= 0;
            if (rw) begin
              shift_reg <= regs[addr_ptr];
              $display("[%0t] HM01B0 Model: Read Reg 0x%h = 0x%h", $time, addr_ptr, regs[addr_ptr]);
            end
          end
        end
        Data: begin
          if (scl_fall) begin
             if (rw) begin
                sda_en_o <= !shift_reg[7];
                shift_reg <= {shift_reg[6:0], 1'b0};
             end else sda_en_o <= 0;
          end else if (scl_rise) begin
            if (!rw) shift_reg <= {shift_reg[6:0], sda_i};
            if (bit_cnt == 7) begin
              state <= AckData; bit_cnt <= 0;
              if (!rw) begin
                if (addr_byte_cnt == 0) begin
                   addr_ptr[15:8] <= {shift_reg[6:0], sda_i};
                   addr_byte_cnt <= 1;
                end else if (addr_byte_cnt == 1) begin
                   addr_ptr[7:0] <= {shift_reg[6:0], sda_i};
                   addr_byte_cnt <= 2;
                end else begin
                   regs[addr_ptr] <= {shift_reg[6:0], sda_i};
                   $display("[%0t] HM01B0 Model: Write Reg 0x%h = 0x%h", $time, addr_ptr, {shift_reg[6:0], sda_i});
                   addr_ptr <= addr_ptr + 16'h1;
                end
              end
            end else bit_cnt <= bit_cnt + 1;
          end
        end
        AckData: begin
          if (scl_fall) begin
            if (!rw) sda_en_o <= 1;
            else sda_en_o <= 0;
          end else if (scl_rise) begin
            if (rw && sda_i) begin
               $display("[%0t] HM01B0 Model: Master NACK detected, going to Idle", $time);
               state <= Idle;
            end else begin
               state <= Data; bit_cnt <= 0;
               if (rw) begin
                  addr_ptr <= addr_ptr + 16'h1;
                  shift_reg <= regs[addr_ptr + 16'h1];
               end
            end
          end
        end
        default: state <= Idle;
      endcase
    end
  end
endmodule
