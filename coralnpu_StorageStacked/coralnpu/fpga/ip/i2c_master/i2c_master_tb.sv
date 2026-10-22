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

module i2c_master_tb;
  import i2c_master_pkg::*;
  import tlul_pkg::*;
  import coralnpu_tlul_pkg_32::*;

  typedef struct packed {
    logic [15:0] addr;
    logic [7:0]  data;
  } init_reg_t;

  logic clk, rst_n;
  coralnpu_tlul_pkg_32::tl_h2d_t tl_h2d;
  coralnpu_tlul_pkg_32::tl_d2h_t tl_d2h;
  wire scl_bus, sda_bus;
  logic m_scl_en, m_sda_en, s_sda_en, hm_sda_en;
  logic dummy_scl_o, dummy_sda_o;

  assign scl_bus = m_scl_en ? 1'b0 : 1'bz;
  assign (pull1, pull0) scl_bus = 1'b1;
  assign sda_bus = (m_sda_en | s_sda_en | hm_sda_en) ? 1'b0 : 1'bz;
  assign (pull1, pull0) sda_bus = 1'b1;

  i2c_master_top dut (
      .clk_i(clk),
      .rst_ni(rst_n),
      .tl_i(tl_h2d),
      .tl_o(tl_d2h),
      .scl_i(scl_bus),
      .scl_o(dummy_scl_o),
      .scl_en_o(m_scl_en),
      .sda_i(sda_bus),
      .sda_o(dummy_sda_o),
      .sda_en_o(m_sda_en)
  );

  i2c_slave_model #(
      .I2C_ADDR(7'h55)
  ) i_slave (
      .clk_i(clk),
      .rst_ni(rst_n),
      .scl_i(scl_bus),
      .sda_i(sda_bus),
      .sda_en_o(s_sda_en)
  );

  hm01b0_model #(
      .I2C_ADDR(7'h24)
  ) i_hm01b0 (
      .clk_i(clk),
      .rst_ni(rst_n),
      .scl_i(scl_bus),
      .sda_i(sda_bus),
      .sda_en_o(hm_sda_en)
  );

  int unsigned cycles = 0;
  always @(posedge clk) begin
    cycles <= cycles + 1;
  end

  initial begin
    clk = 0;
    $dumpfile("trace.fst");
    $dumpvars(0, i2c_master_tb);
    forever #5 clk = ~clk;
  end

  initial begin
    #20000000;  // 20ms Watchdog
    $display("[%0d] WATCHDOG TIMEOUT", cycles);
    $finish;
  end

  // Robust TileLink tasks with proper Integrity
  task automatic tl_write(logic [31:0] addr, logic [31:0] data);
    tl_h2d.a_valid = 1;
    tl_h2d.a_opcode = PutFullData;
    tl_h2d.a_address = addr;
    tl_h2d.a_data = data;
    tl_h2d.a_mask = 4'hF;
    tl_h2d.a_size = 2;
    tl_h2d.a_user.instr_type = prim_mubi_pkg::mubi4_t'(4'h6);  // MuBi4False
    tl_h2d.a_user.cmd_intg = '0;
    tl_h2d.a_user.data_intg = '0;
    while (!tl_d2h.a_ready) @(posedge clk);
    @(posedge clk);
    tl_h2d.a_valid = 0;
    while (!tl_d2h.d_valid) @(posedge clk);
    tl_h2d.d_ready = 1;
    @(posedge clk);
    tl_h2d.d_ready = 0;
    repeat (2) @(posedge clk);
  endtask

  task automatic tl_read(logic [31:0] addr, output logic [31:0] data);
    tl_h2d.a_valid = 1;
    tl_h2d.a_opcode = Get;
    tl_h2d.a_address = addr;
    tl_h2d.a_data = 32'h0;
    tl_h2d.a_mask = 4'hF;
    tl_h2d.a_size = 2;
    tl_h2d.a_user.instr_type = prim_mubi_pkg::mubi4_t'(4'h6);  // MuBi4False
    tl_h2d.a_user.cmd_intg = '0;
    tl_h2d.a_user.data_intg = '0;
    while (!tl_d2h.a_ready) @(posedge clk);
    @(posedge clk);
    tl_h2d.a_valid = 0;
    while (!tl_d2h.d_valid) @(posedge clk);
    data = tl_d2h.d_data;
    tl_h2d.d_ready = 1;
    @(posedge clk);
    tl_h2d.d_ready = 0;
    repeat (2) @(posedge clk);
  endtask

  task automatic wait_idle();
    logic [31:0] status;
    int count = 0;
    do begin
      tl_read(32'h00C, status);  // STATUS
      count++;
    end while ((status[0] || status[1]) && count < 2000);  // busy or !fifo_empty
    if (count >= 2000) $display("[%0d] wait_idle TIMEOUT status=0x%h", cycles, status);
  endtask

  // Helper tasks for HM01B0 (16-bit address)
  task automatic hm_write_reg(logic [15:0] reg_addr, logic [7:0] data);
    $display("[%0d] HM01B0 Write: Reg 0x%h = 0x%h", cycles, reg_addr, data);
    tl_write(32'h010, {21'h0, 1'b0, 1'b0, 1'b1, 7'h24, 1'b0});  // START, ADDR 0x24, W
    tl_write(32'h010, {24'h0, reg_addr[15:8]});  // ADDR H
    tl_write(32'h010, {24'h0, reg_addr[7:0]});  // ADDR L
    tl_write(32'h010, {21'h0, 1'b0, 1'b1, 1'b0, data});  // STOP, DATA
    wait_idle();
  endtask

  task automatic hm_read_reg(logic [15:0] reg_addr, output logic [7:0] data);
    logic [31:0] rdata;
    $display("[%0d] HM01B0 Read: Reg 0x%h", cycles, reg_addr);
    tl_write(32'h010, {21'h0, 1'b0, 1'b0, 1'b1, 7'h24, 1'b0});  // START, ADDR 0x24, W
    tl_write(32'h010, {24'h0, reg_addr[15:8]});  // ADDR H
    tl_write(32'h010, {24'h0, reg_addr[7:0]});  // ADDR L
    tl_write(32'h010, {21'h0, 1'b0, 1'b0, 1'b1, 7'h24, 1'b1});  // RESTART, ADDR 0x24, R
    tl_write(32'h010, {21'h0, 1'b1, 1'b1, 1'b0, 8'h00});  // READ, STOP
    wait_idle();
    tl_read(32'h010, rdata);
    data = rdata[7:0];
    $display("[%0d] HM01B0 Read Result: 0x%h", cycles, data);
  endtask

  initial begin
    logic [31:0] rdata;
    logic [7:0] hm_data;
    int err_count;

    init_reg_t init_regs[] = '{
        '{16'h0103, 8'h00},  // HM01B0_SW_RESET
        '{16'h0100, 8'h00},  // HM01B0_MODE_SELECT
        '{16'h0101, 8'h03},  // HM01B0_IMAGE_ORIENTATION (Non-SPARROW)
        '{16'h1003, 8'h08},  // HM01B0_BLC_TGT
        '{16'h1007, 8'h08},  // HM01B0_BLC2_TGT
        '{16'h3044, 8'h0A},
        '{16'h3045, 8'h00},
        '{16'h3047, 8'h0A},
        '{16'h3050, 8'hC0},
        '{16'h3051, 8'h42},
        '{16'h3052, 8'h50},
        '{16'h3053, 8'h00},
        '{16'h3054, 8'h03},
        '{16'h3055, 8'hF7},
        '{16'h3056, 8'hF8},
        '{16'h3057, 8'h29},
        '{16'h3058, 8'h1F},
        '{16'h3059, 8'h1E},  // HM01B0_BIT_CONTROL
        '{16'h3064, 8'h00},  // HM01B0_SYNC_EN
        '{16'h3065, 8'h04},  // HM01B0_OUTPUT_PIN_STATUS_CTRL
        '{16'h1000, 8'h43},  // HM01B0_BLC_CFG
        '{16'h1001, 8'h40},
        '{16'h1002, 8'h32},
        '{16'h0350, 8'h7F},
        '{16'h1006, 8'h01},  // HM01B0_BLC2_EN
        '{16'h1008, 8'h00},
        '{16'h1009, 8'hA0},
        '{16'h100A, 8'h60},
        '{16'h100B, 8'h90},
        '{16'h100C, 8'h40},
        '{16'h1012, 8'h00},  // HM01B0_VSYNC_HSYNC_PIXEL_SHIFT_EN
        '{16'h2000, 8'h07},  // HM01B0_STATISTIC_CTRL
        '{16'h2003, 8'h00},
        '{16'h2004, 8'h1C},
        '{16'h2007, 8'h00},
        '{16'h2008, 8'h58},
        '{16'h200B, 8'h00},
        '{16'h200C, 8'h7A},
        '{16'h200F, 8'h00},
        '{16'h2010, 8'hB8},
        '{16'h2013, 8'h00},  // HM01B0_MD_LROI_Y_START_H
        '{16'h2014, 8'h58},  // HM01B0_MD_LROI_Y_START_L
        '{16'h2017, 8'h00},  // HM01B0_MD_LROI_X_END_H
        '{16'h2018, 8'h9B},  // HM01B0_MD_LROI_X_END_L
        '{16'h2100, 8'h01},  // HM01B0_AE_CTRL
        '{16'h2104, 8'h07},  // HM01B0_CONVERGE_OUT_TH
        '{16'h2105, 8'h02},  // HM01B0_MAX_INTG_H  (30Fps)
        '{16'h2106, 8'h14},  // HM01B0_MAX_INTG_L
        '{16'h2108, 8'h03},  // HM01B0_MAX_AGAIN_FULL
        '{16'h2109, 8'h03},  // HM01B0_MAX_AGAIN_BIN2
        '{16'h210B, 8'h80},  // HM01B0_MAX_DGAIN
        '{16'h210F, 8'h00},  // HM01B0_FS_60HZ_H
        '{16'h2110, 8'h85},  // HM01B0_FS_60HZ_L
        '{16'h2111, 8'h00},  // HM01B0_FS_50HZ_H
        '{16'h2112, 8'hA0},  // HM01B0_FS_50HZ_L
        '{16'h2150, 8'h03},  // HM01B0_MD_CTRL
        '{16'h0340, 8'h02},  // HM01B0_FRAME_LENGTH_LINES_H
        '{16'h0341, 8'h16},  // HM01B0_FRAME_LENGTH_LINES_L
        '{16'h0342, 8'h01},  // HM01B0_LINE_LENGTH_PCK_H
        '{16'h0343, 8'h78},  // HM01B0_LINE_LENGTH_PCK_L
        '{16'h3010, 8'h01},  // HM01B0_QVGA_WIN_EN
        '{16'h0383, 8'h01},  // HM01B0_READOUT_X
        '{16'h0387, 8'h01},  // HM01B0_READOUT_Y
        '{16'h0390, 8'h00},  // HM01B0_BINNING_MODE
        '{16'h3011, 8'h70},  // HM01B0_SIX_BIT_MODE_EN
        '{16'h3059, 8'h02},  // HM01B0_BIT_CONTROL
        '{16'h3060, 8'h01},  // HM01B0_OSC_CLK_DIV
        '{16'h0104, 8'h01},  // HM01B0_GRP_PARAM_HOLD
        '{16'h0100, 8'h05}  // HM01B0_MODE_SELECT
    };

    tl_h2d = '0;
    rst_n  = 0;
    #100 rst_n = 1;
    repeat (10) @(posedge clk);
    $display("[%0d] Starting I2C Master End-to-End TB", cycles);

    // Configure and Enable Master
    tl_write(32'h018, 32'h8);  // CLK_DIV
    tl_write(32'h008, 32'h1);  // CTRL (enable)

    // Transaction 1: Write 0xDE to Slave Register 2
    $display("[%0d] Writing 0xDE to Slave Reg 2", cycles);
    tl_write(32'h010, 32'h1AA);  // START, 0x55, W
    tl_write(32'h010, 32'h002);  // REG 2
    tl_write(32'h010, 32'h2DE);  // DATA 0xDE, STOP
    wait_idle();

    // Transaction 2: Read back from Slave Register 2
    $display("[%0d] Reading back from Slave Reg 2", cycles);
    tl_write(32'h010, 32'h1AA);
    tl_write(32'h010, 32'h002);
    tl_write(32'h010, 32'h1AB);  // RESTART, 0x55, R
    tl_write(32'h010, 32'h600);  // READ, STOP
    wait_idle();
    tl_read(32'h010, rdata);
    $display("[%0d] Slave Read Result: 0x%h (Expected 0xDE)", cycles, rdata[7:0]);

    // --- HM01B0 Camera Model Tests ---
    $display("[%0d] Starting HM01B0 Model Tests", cycles);

    // Read Model ID
    hm_read_reg(16'h0000, hm_data);  // ID H
    if (hm_data == 8'h01) $display("HM01B0 ID_H PASSED");
    else $display("HM01B0 ID_H FAILED");

    hm_read_reg(16'h0001, hm_data);  // ID L
    if (hm_data == 8'hB0) $display("HM01B0 ID_L PASSED");
    else $display("HM01B0 ID_L FAILED");

    // Initialization Sequence (thorough)
    foreach (init_regs[i]) begin
      hm_write_reg(init_regs[i].addr, init_regs[i].data);
    end

    // Verify written values
    err_count = 0;
    begin
      logic [7:0] expected_vals[int];
      foreach (init_regs[i]) begin
        expected_vals[init_regs[i].addr] = init_regs[i].data;
      end
      foreach (expected_vals[addr]) begin
        hm_read_reg(addr[15:0], hm_data);
        if (hm_data != expected_vals[addr]) begin
          $display("HM01B0 Write/Read FAILED at 0x%h: Expected 0x%h, Got 0x%h", addr[15:0],
                   expected_vals[addr], hm_data);
          err_count++;
        end
      end
    end

    if (err_count == 0) $display("HM01B0 All Initialization Registers Write/Read PASSED");
    else $display("HM01B0 Initialization Register Write/Read FAILED with %0d errors", err_count);

    $display("ALL TESTS COMPLETED");
    repeat (100) @(posedge clk);
    $finish;
  end
endmodule
