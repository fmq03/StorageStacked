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

module i2c_master
  import i2c_master_pkg::*;
  import tlul_pkg::*;
  import coralnpu_tlul_pkg_32::*;
#(
    parameter int FifoDepth = 4
) (
    input clk_i,
    input rst_ni,
    input coralnpu_tlul_pkg_32::tl_h2d_t tl_i,
    output coralnpu_tlul_pkg_32::tl_d2h_t tl_o,
    input scl_i,
    output logic scl_o,
    scl_en_o,
    input sda_i,
    output logic sda_o,
    sda_en_o,
    output logic intr_o,
    input logic intg_error_i
);
  logic [31:0] reg_intr_state, reg_intr_enable, reg_ctrl, reg_clk_div;

  // Simple synchronous FIFO for transmit data and commands
  logic [10:0] fifo_mem[FifoDepth];
  logic [$clog2(FifoDepth):0] fifo_wptr, fifo_rptr;
  logic fifo_full, fifo_empty;
  assign fifo_full = (fifo_wptr[$clog2(
      FifoDepth
  )-1:0] == fifo_rptr[$clog2(
      FifoDepth
  )-1:0]) && (fifo_wptr[$clog2(
      FifoDepth
  )] != fifo_rptr[$clog2(
      FifoDepth
  )]);
  assign fifo_empty = (fifo_wptr == fifo_rptr);

  logic [7:0] rx_fifo_data;
  logic rx_fifo_valid;
  logic tl_d_valid, tl_d_error;
  tl_d_op_e tl_d_opcode;
  logic [top_pkg::TL_DW-1:0] tl_d_data;

  logic rx_fifo_clr;

  // TileLink-UL interface logic with backpressure support for FIFO
  assign tl_o.a_ready = !tl_d_valid && !( (tl_i.a_opcode == PutFullData || tl_i.a_opcode == PutPartialData) && addr_offset == FDATA && fifo_full );
  assign tl_o.d_valid = tl_d_valid;
  assign tl_o.d_opcode = tl_d_opcode;
  assign tl_o.d_data = tl_d_data;
  assign tl_o.d_error = tl_d_error | intg_error_i;
  assign tl_o.d_param = 3'h0;
  assign tl_o.d_size = tl_i.a_size;
  assign tl_o.d_source = tl_i.a_source;
  assign tl_o.d_sink = 1'b0;
  assign tl_o.d_user = '0;

  logic [11:0] addr_offset;
  assign addr_offset = tl_i.a_address[11:0];

  logic fifo_pop;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      tl_d_valid <= 0;
      tl_d_opcode <= AccessAck;
      tl_d_data <= 0;
      tl_d_error <= 0;
      reg_intr_state <= 0;
      reg_intr_enable <= 0;
      reg_ctrl <= 0;
      reg_clk_div <= 32'h10;
      fifo_wptr <= 0;
      fifo_rptr <= 0;
      rx_fifo_clr <= 0;
    end else begin
      rx_fifo_clr <= 0;
      if (fifo_pop && !fifo_empty) fifo_rptr <= fifo_rptr + 1;

      // Raise tx_idle interrupt when master is idle and FIFO is empty
      if (fifo_empty && state == StIdle && reg_ctrl[0]) reg_intr_state[0] <= 1'b1;

      if (tl_i.a_valid && tl_o.a_ready) begin
        tl_d_valid <= 1;
        tl_d_error <= 0;
        if (intg_error_i) begin
          tl_d_opcode <= (tl_i.a_opcode == Get) ? AccessAckData : AccessAck;
          tl_d_data   <= 32'hFFFFFFFF;
          tl_d_error  <= 1;
        end else if (tl_i.a_opcode == PutFullData || tl_i.a_opcode == PutPartialData) begin
          tl_d_opcode <= AccessAck;
          case (addr_offset)
            INTR_STATE:  reg_intr_state <= reg_intr_state & ~tl_i.a_data;
            INTR_ENABLE: reg_intr_enable <= tl_i.a_data;
            CTRL:        reg_ctrl <= tl_i.a_data;
            CLK_DIV:     reg_clk_div <= tl_i.a_data;
            FDATA: begin
              fifo_mem[fifo_wptr[$clog2(
                  FifoDepth
              )-1:0]] <= {
                tl_i.a_data[FDATA_READ],
                tl_i.a_data[FDATA_STOP],
                tl_i.a_data[FDATA_START],
                tl_i.a_data[7:0]
              };
              fifo_wptr <= fifo_wptr + 1;
            end
            default:     tl_d_error <= 1;
          endcase
        end else begin
          tl_d_opcode <= AccessAckData;
          case (addr_offset)
            INTR_STATE: tl_d_data <= reg_intr_state;
            INTR_ENABLE: tl_d_data <= reg_intr_enable;
            CTRL: tl_d_data <= reg_ctrl;
            STATUS: tl_d_data <= {24'h0, 4'h0, 1'b0, rx_fifo_valid, !fifo_empty, state != StIdle};
            CLK_DIV: tl_d_data <= reg_clk_div;
            FDATA: begin
              tl_d_data   <= {24'h0, rx_fifo_data};
              rx_fifo_clr <= 1'b1;
            end
            default: begin
              tl_d_data  <= 32'hDEADBEEF;
              tl_d_error <= 1;
            end
          endcase
        end
      end else if (tl_i.d_ready && tl_d_valid) begin
        tl_d_valid <= 0;
      end
    end
  end

  assign intr_o = |(reg_intr_state & reg_intr_enable);

  // I2C Master FSM
  typedef enum logic [2:0] {
    StIdle,
    StStart,
    StBit,
    StAck,
    StStop
  } master_state_e;
  master_state_e state;
  logic [31:0] clk_cnt;
  logic [1:0] phase;
  logic [3:0] bit_cnt;
  logic [7:0] shift_reg;
  logic cur_start, cur_stop, cur_read;
  logic was_stop;  // Track if bus was released via STOP

  assign scl_o = 0;
  assign sda_o = 0;  // Driving logic uses _en_o (active-low open-drain)

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state <= StIdle;
      clk_cnt <= 0;
      phase <= 0;
      bit_cnt <= 0;
      shift_reg <= 0;
      scl_en_o <= 0;
      sda_en_o <= 0;
      fifo_pop <= 0;
      rx_fifo_valid <= 0;
      rx_fifo_data <= 0;
      cur_start <= 0;
      cur_stop <= 0;
      cur_read <= 0;
      was_stop <= 1;
    end else begin
      fifo_pop <= 1'b0;
      if (rx_fifo_clr) rx_fifo_valid <= 1'b0;

      if (clk_cnt < reg_clk_div) begin
        clk_cnt <= clk_cnt + 1;
      end else begin
        clk_cnt <= 0;
        case (state)
          StIdle: begin
            if (was_stop) begin
              scl_en_o <= 0;
              sda_en_o <= 0;
            end else begin
              scl_en_o <= 1;
            end  // Keep SCL low between bytes

            if (!fifo_empty && reg_ctrl[0]) begin
              {cur_read, cur_stop, cur_start, shift_reg} <= fifo_mem[fifo_rptr[$clog2(
                  FifoDepth
              )-1:0]];
              bit_cnt <= 7;
              fifo_pop <= 1;
              state <= fifo_mem[fifo_rptr[$clog2(FifoDepth)-1:0]][8] ? StStart : StBit;
              phase <= 2'h0;
            end
          end
          StStart: begin
            was_stop <= 0;
            case (phase)
              2'h0: begin
                sda_en_o <= 0;
                scl_en_o <= 0;
                phase <= 2'h1;
              end
              2'h1: begin
                sda_en_o <= 1;
                scl_en_o <= 0;
                phase <= 2'h2;
              end
              2'h2: begin
                sda_en_o <= 1;
                scl_en_o <= 1;
                phase <= 2'h3;
              end
              default: begin
                state <= StBit;
                phase <= 2'h0;
              end
            endcase
          end
          StBit: begin
            case (phase)
              2'h0: begin
                scl_en_o <= 1;
                sda_en_o <= cur_read ? 0 : !shift_reg[7];
                phase <= 2'h1;
              end
              2'h1: begin
                scl_en_o <= 0;
                phase <= 2'h2;
              end
              2'h2: begin
                if (cur_read) shift_reg <= {shift_reg[6:0], sda_i};
                phase <= 2'h3;
              end
              default: begin
                scl_en_o <= 1;
                if (!cur_read) shift_reg <= {shift_reg[6:0], 1'b0};
                if (bit_cnt == 0) begin
                  state <= StAck;
                  phase <= 2'h0;
                end else begin
                  bit_cnt <= bit_cnt - 1;
                  phase   <= 2'h0;
                end
              end
            endcase
          end
          StAck: begin
            case (phase)
              2'h0: begin
                scl_en_o <= 1;
                sda_en_o <= (cur_read && !cur_stop) ? 1'b1 : 1'b0;
                phase <= 2'h1;
              end  // Read: ACK if more bytes, NACK if last. Write: Pull SDA Low (no wait, Write is sda_en_o=0 to receive ACK)
              2'h1: begin
                scl_en_o <= 0;
                phase <= 2'h2;
              end
              2'h2: begin
                phase <= 2'h3;
              end
              default: begin
                scl_en_o <= 1;
                if (cur_read) begin
                  rx_fifo_data  <= shift_reg;
                  rx_fifo_valid <= 1;
                end
                if (cur_stop) state <= StStop;
                else state <= StIdle;
                phase <= 2'h0;
              end
            endcase
          end
          StStop: begin
            case (phase)
              2'h0: begin
                scl_en_o <= 1;
                sda_en_o <= 1;
                phase <= 2'h1;
              end
              2'h1: begin
                scl_en_o <= 0;
                sda_en_o <= 1;
                phase <= 2'h2;
              end  // SCL High
              2'h2: begin
                scl_en_o <= 0;
                sda_en_o <= 0;
                phase <= 2'h3;
              end  // SDA High
              default: begin
                state <= StIdle;
                phase <= 2'h0;
                was_stop <= 1;
              end
            endcase
          end
          default: state <= StIdle;
        endcase
      end
    end
  end
endmodule
