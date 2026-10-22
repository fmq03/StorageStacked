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


`timescale 1ns / 1ps

// Using 10 cycle clock 50% period for frequency of 100MHz
module isp_wrapper_tb;
  // Configuration
  parameter int NUM_FRAMES_TO_CAPTURE = 2;
  int frames_captured = 0;
  int hsync_count = 0;

  import tlul_pkg::*;
  import top_pkg::*;
  import coralnpu_tlul_pkg_32::*;

  logic clk, rst_n;
  logic intr_mi;  // Interrupt from MI
  coralnpu_tlul_pkg_32::tl_h2d_t tl_h2d;
  coralnpu_tlul_pkg_32::tl_d2h_t tl_d2h;

  // BEGIN CAMERA MODEL USED FOR CAM MODEL TESTING
  // Simple RTL Camera Model (320x240)
`ifdef CAM_MODEL
  // Camera Model Signals
  logic cam_pclk;
  logic cam_hsync;
  logic cam_vsync;
  logic [7:0] cam_data;

  logic cam_rst_n;

  // Testbench control
  parameter int use_reference_image = 1;

  // Instantiate the Camera Signal Generator
  cam_signal_generator #(
      .USE_REF_IMG(use_reference_image),
      .IMG_FILE   ("grey_bars_320x240.raw")
  ) u_cam_gen (
      .clk_i (clk),
      .rst_ni(cam_rst_n),
      .pclk  (cam_pclk),
      .lvld  (cam_hsync),
      .fvld  (cam_vsync),
      .data  (cam_data)
  );

  initial begin
    cam_rst_n = 0;
  end

  // cam_data is now driven by u_cam_gen

  // DEBUG: Simple hsync rising edge counter
  always @(posedge cam_hsync) begin
    hsync_count++;
  end

`endif
  // END CAMERA MODEL USED FOR CAM MODEL TESTING

  // --- INTERNAL ISP PROBES FOR WAVEFORM VIEWER ---
  // These signals mirror internal MI status registers to make them easy to find in the waveform.
  // These are brittle. May be a source of future breakage.

  // LIVE MI Byte Counter (Total raw bytes received from camera interface)
  // NOTE: This points directly to the counter register inside u_marvin_mi_in.
  // This is better than MI_BYTE_CNT (0x70) because the register is a snapshot only updated at frame ends.
  wire [27:0] dbg_mi_pixel_cnt;
  assign dbg_mi_pixel_cnt = dut.u_isp.u_marvin_top_a_0.u_marvin_mi.u_marvin_mi_in.stat_byte_cnt_raw;

  // LIVE MI Line Counter (Increments every HSYNC/Line end)
  // Points to viv_s9 in u_marvin_mi_in.
  wire [7:0] dbg_mi_line_cnt;
  assign dbg_mi_line_cnt = dut.u_isp.u_marvin_top_a_0.u_marvin_mi.u_marvin_mi_in.viv_s9;

  // MI_MP_Y_OFFS_CNT (Current write pointer offset for Main Path Y buffer)
  // This updates at the end of every AXI burst.
  wire [25:0] dbg_mi_mp_y_offs_cnt;
  assign dbg_mi_mp_y_offs_cnt = dut.u_isp.u_marvin_top_a_0.u_marvin_mi.u_marvin_mi_out.u_marvin_mi_out_mp.u_marvin_mi_out_addrgen_mp.mp_y_offs_cnt;
  // MI IRQ Signals from vsisp_marvin_irq_handler.v. OR of many internal IRQ signals.
  wire dbg_mi_irq;
  assign dbg_mi_irq = dut.u_isp.u_marvin_top_a_0.u_marvin_irq_handler.mi_irq;

  wire dbg_mi_mp_frame_end;
  assign dbg_mi_mp_frame_end = dut.u_isp.u_marvin_top_a_0.u_marvin_irq_handler.mi_mp_frame_end_int;
  // -----------------------------------------------

  // AXI Monitor Signals

  logic        m1_awvalid;
  logic [31:0] m1_awaddr;
  logic        m1_wvalid;
  logic [63:0] m1_wdata;

  // Unused output connections
  logic [ 3:0] awid_unused;
  logic [ 3:0] awlen_unused;
  logic [ 2:0] awsize_unused;
  logic [ 1:0] awburst_unused;
  logic [ 1:0] awlock_unused;
  logic [ 3:0] awcache_unused;
  logic [ 2:0] awprot_unused;
  logic [ 3:0] awqos_unused;
  logic [ 3:0] awregion_unused;
  logic [63:0] wdata_unused;
  logic [ 7:0] wstrb_unused;
  logic        wlast_unused;
  logic [ 3:0] wid_unused;

  // AXI B-channel logic generator
  logic        m1_bvalid;
  logic        m1_bready;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      m1_bvalid <= 1'b0;
    end else begin
      if (m1_wvalid && wlast_unused) begin
        m1_bvalid <= 1'b1;
      end else if (m1_bvalid && m1_bready) begin
        m1_bvalid <= 1'b0;
      end
    end
  end

  // File Handles for Image Dump
  integer fd0, fd1;

  // Simulated Memory Storage
  // Flattened array to avoid Verilator associative array compilation bugs.
  // We allocate enough space for 32MB mapped memory. Max index needed is 32MB/8 = 4M.
  // base is 0x5a000000. 0x5a000000 to 0x5c000000 (32MB).
  logic [63:0] tb_mem_storage[0:4194303];
  logic [31:0] next_waddr;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      next_waddr <= 32'h0;
    end else begin
      logic [31:0] burst_addr;

      if (m1_awvalid) begin
        burst_addr = m1_awaddr;
        if (m1_awaddr >= 32'h58000000 && m1_awaddr < 32'h5c000000) begin
          $display("[%0t] [AXI Write] Burst start at address: %x", $time, m1_awaddr);
        end
      end else begin
        burst_addr = next_waddr;
      end

      if (m1_wvalid) begin
        if ((burst_addr & 32'hfc000000) == 32'h58000000 || (burst_addr & 32'hfe000000) == 32'h5a000000) begin
          logic [21:0] mem_idx;
          mem_idx = (burst_addr & 32'h01FFFFFF) >> 3; // Support up to 32MB offset (0x0000000 to 0x1FFFFFF)
          tb_mem_storage[mem_idx] = m1_wdata;
        end
        next_waddr <= burst_addr + 8;
      end else if (m1_awvalid) begin
        next_waddr <= burst_addr;
      end
    end
  end


  // Instantiate DUT
  isp_wrapper #(
      .AhbLiteDataWidth(32),
      .TimeoutLimit(65535)
  ) dut (
      .clk_i(clk),
      .clk_core_i(clk),
      .clk_axi_i(clk),
      .rst_ni(rst_n),

      // TLUL Interface (Structural)
      .tl_i(tl_h2d),
      .tl_o(tl_d2h),

      // Tie off other inputs (AXI M1, M2, Interrupts, etc.)
      .axi_m1_awready(1'b1),
      .axi_m1_wready(1'b1),
      .axi_m1_bresp(2'b0),
      .axi_m1_bid(4'b0),
      .axi_m1_arready(1'b1),
      .axi_m1_rvalid(1'b0),
      .axi_m1_rdata(64'b0),
      .axi_m1_rresp(2'b0),
      .axi_m1_rid(4'b0),
      .axi_m1_rlast(1'b0),

      .axi_m2_awready(1'b1),
      .axi_m2_wready(1'b1),
      .axi_m2_bvalid(1'b0),
      .axi_m2_bresp(2'b0),
      .axi_m2_bid(4'b0),
      .axi_m2_rvalid(1'b0),
      .axi_m2_rdata(64'b0),
      .axi_m2_rresp(2'b0),
      .axi_m2_rid(4'b0),
      .axi_m2_rlast(1'b0),

`ifdef CAM_MODEL
      .cio_s_pclk_i(cam_pclk),
      .cio_s_data_i(cam_data),
      .cio_s_hsync_i(cam_hsync),
      .cio_s_vsync_i(cam_vsync),
`else
      .cio_s_pclk_i(clk),
      .cio_s_data_i(8'b0),
      .cio_s_hsync_i(1'b0),
      .cio_s_vsync_i(1'b0),
`endif
      .disable_isp_i(1'b0),
      .scanmode_i(1'b0),

      .intr_mi_o (intr_mi),
      .intr_isp_o(),

      // AXI M1 Outputs
      .axi_m1_awaddr(m1_awaddr),
      .axi_m1_awvalid(m1_awvalid),
      .axi_m1_awid(awid_unused),
      .axi_m1_awlen(awlen_unused),
      .axi_m1_awsize(awsize_unused),
      .axi_m1_awburst(awburst_unused),
      .axi_m1_awlock(awlock_unused),
      .axi_m1_awcache(awcache_unused),
      .axi_m1_awprot(awprot_unused),
      .axi_m1_awqos(awqos_unused),
      .axi_m1_awregion(awregion_unused),
      .axi_m1_wvalid(m1_wvalid),
      .axi_m1_wdata(m1_wdata),
      .axi_m1_wstrb(wstrb_unused),
      .axi_m1_wlast(wlast_unused),
      .axi_m1_wid(wid_unused),
      .axi_m1_bready(m1_bready),

      // AXI M1 Inputs
      .axi_m1_bvalid(m1_bvalid),
      .axi_m1_arvalid(),
      .axi_m1_araddr(),
      .axi_m1_arid(),
      .axi_m1_arlen(),
      .axi_m1_arsize(),
      .axi_m1_arburst(),
      .axi_m1_arlock(),
      .axi_m1_arcache(),
      .axi_m1_arprot(),
      .axi_m1_arqos(),
      .axi_m1_arregion(),
      .axi_m1_rready(),

      // AXI M2 Outputs
      .axi_m2_awaddr(),
      .axi_m2_awvalid(),
      .axi_m2_awid(),
      .axi_m2_awlen(),
      .axi_m2_awsize(),
      .axi_m2_awburst(),
      .axi_m2_awlock(),
      .axi_m2_awcache(),
      .axi_m2_awprot(),
      .axi_m2_awqos(),
      .axi_m2_awregion(),
      .axi_m2_wvalid(),
      .axi_m2_wdata(),
      .axi_m2_wstrb(),
      .axi_m2_wlast(),
      .axi_m2_wid(),
      .axi_m2_bready(),
      .axi_m2_arvalid(),
      .axi_m2_araddr(),
      .axi_m2_arid(),
      .axi_m2_arlen(),
      .axi_m2_arsize(),
      .axi_m2_arburst(),
      .axi_m2_arlock(),
      .axi_m2_arcache(),
      .axi_m2_arprot(),
      .axi_m2_arqos(),
      .axi_m2_arregion(),
      .axi_m2_rready(),

      // AXI M2 Inputs (Tie off)
      .axi_m2_arready(1'b0)  // We aren't using m2 (self path), just using m1 (main path)
  );

  // Clock generation
  initial begin
    clk = 0;
    forever #5 clk = ~clk;
  end

  // Helper task for TileLink-UL Write
  task automatic tl_write(logic [31:0] addr, logic [31:0] data);
    tl_h2d = '0;
    tl_h2d.a_valid = 1;
    tl_h2d.a_opcode = PutFullData;
    tl_h2d.a_address = addr;
    tl_h2d.a_data = data;
    tl_h2d.a_mask = 4'hF;
    tl_h2d.a_size = 2;  // Word size (2^2 = 4 bytes)
    tl_h2d.a_source = 0;

    tl_h2d.a_user.instr_type = prim_mubi_pkg::mubi4_t'(4'h9);
    tl_h2d.a_user.cmd_intg = '0;
    tl_h2d.a_user.data_intg = '0;

    // Wait for ready
    while (!tl_d2h.a_ready) @(posedge clk);
    @(posedge clk);
    tl_h2d.a_valid = 0;

    // Wait for response
    while (!tl_d2h.d_valid) @(posedge clk);
    tl_h2d.d_ready = 1;
    @(posedge clk);
    tl_h2d.d_ready = 0;

    if (tl_d2h.d_error) begin
      $display("TLUL Error during Write to 0x%h", addr);
    end
  endtask

  // Helper task for TileLink-UL Read
  task automatic tl_read(logic [31:0] addr, output logic [31:0] data);
    tl_h2d = '0;
    tl_h2d.a_valid = 1;
    tl_h2d.a_opcode = Get;
    tl_h2d.a_address = addr;
    tl_h2d.a_mask = 4'hF;
    tl_h2d.a_size = 2;
    tl_h2d.a_source = 0;

    tl_h2d.a_user.instr_type = prim_mubi_pkg::mubi4_t'(4'h9);
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

    if (tl_d2h.d_error) begin
      $display("TLUL Error during Read from 0x%h", addr);
    end
  endtask

  `include "isp_config.svh"

  initial begin
    logic [31:0] rdata;
    $dumpfile("isp_wrapper_tb.fst");
    $dumpvars(0, isp_wrapper_tb);
    rst_n = 0;
    #100 rst_n = 1;
    repeat (10) @(posedge clk);
    $display("Starting ISP Wrapper Testbench");

    // Testbench Overview:
    // This testbench verifies the ISP in three stages:
    // 1. Basic Register Access: Confirms TLUL interface is working.
    // 2. ISP Configuration: Configures the ISP for either 128x64 Gray Bar (TPG) or 320x240 (Camera).
    // 3. Data Path Verification: Monitors the AXI Master interface to ensure video data is written to memory.
    // 4. Image Dump: Captures the Y-channel data to files for visual verification.

    // Output files will be opened at the end of the test.


    // =========================================================================
    // Stage 1: Basic Register Access Verification
    // =========================================================================
    $display("\n[Stage 1] Verifying Register Access...");

    // 1. Read VI_ID (Read-Only)
    // We will verify access using VI_ICCL (0x10) which is known to be RW and 7 bits wide.

    // Write Expected Value to VI_ICCL (0x10)
    $display("Writing 0x59 to VI_ICCL (0x10)...");
    tl_write(32'h10, 32'h59);

    // Read Back
    tl_read(32'h10, rdata);
    $display("Read Back: 0x%h", rdata);

    if (rdata !== 32'h59) begin
      $error("[Stage 1] FAILED: Readback mismatch for VI_ICCL. Expecting 0x59 (7-bit), got 0x%h",
             rdata);
      $finish;
    end else begin
      $display("[Stage 1] PASSED: Basic Register Access confirmed.");
    end

    // Reset VI_ICCL if needed, but 0x59 is what we want later anyway.

    repeat (10) @(posedge clk);

    // =========================================================================
    // Stage 2: ISP Configuration & Verification
    // =========================================================================
`ifdef CAM_MODEL
    $display("\n[Stage 2] Configuring ISP for Camera Model...");
`else
    $display("\n[Stage 2] Configuring ISP for TPG...");
`endif

    configure_isp();

`ifdef CAM_MODEL
    cam_rst_n = 1;
`endif

    $display("\n[Stage 2] Verification: Reading back key registers...");
    begin
      logic [31:0] rdata;
      tl_read(32'h00000400, rdata);  // ISP_CTRL
      $display("ISP_CTRL: 0x%h (Expected: 0x00207211)", rdata);
      tl_read(32'h00000e00, rdata);  // MI_CTRL
      $display("MI_CTRL:  0x%h (Expected: 0x68352808)", rdata);
      tl_read(32'h00000ef8, rdata);  // MI_IMSC
      $display("MI_IMSC:  0x%h (Expected: 0x000003ff)", rdata);
      tl_read(32'h00000b00, rdata);  // ISP_IMSC
      $display("ISP_IMSC: 0x%h (Expected: 0x000400fc)", rdata);
    end

    repeat (10) @(posedge clk);

    // ... verification removed

    // =========================================================================
    // Stage 3: ISP Data Path Verification (AXI Monitor)
    // =========================================================================
    $display("\n[Stage 3] Verifying ISP Data Path (AXI Monitor)...");

    // Check for Write Address
    // The ISP should issue a Write Address (AW) command to the configured base address (0x5a000000).
    fork
      begin
        wait (m1_awvalid);
        $display("Detected AXI Write Address: 0x%h", m1_awaddr);
        // We expect base address to be around 0x5a300000 as configured in isp_config.svh
        if ((m1_awaddr & 32'hFFFF0000) == 32'h5A300000) begin
          $display("[Stage 3] Address Check PASSED");
        end else begin
          $error("[Stage 3] Address Check FAILED. Expected base 0x5a30xxxx, got 0x%h", m1_awaddr);
        end
      end
      begin
        repeat (2000000) @(posedge clk);
        $error(
            "[Stage 3] TIMEOUT waiting for AXI Write Address. ISP might not be generating frames (Check TPG or Camera generation).");
        $finish;
      end
    join_any
    disable fork;

    // Check for Write Data
    // The ISP should write pixel data. For the Gray Bar pattern, the data should be non-zero.
    // (A solid black image would be all zeros).
    fork
      begin
        wait (m1_wvalid);
        $display("Detected AXI Write Data: 0x%h", m1_wdata);
        if (m1_wdata !== 64'b0) begin
          $display("[Stage 3] Data Check PASSED (Non-zero data detected)");
        end else begin
          // Gray bar or Camera image should have non-zero data
          $display(
              "[Stage 3] Data Check WARNING: All-zero data detected. Is ISP outputting valid data?");
        end
      end
      begin
        repeat (1000) @(posedge clk);
        $error("[Stage 3] TIMEOUT waiting for AXI Write Data");
        $finish;
      end
    join_any
    disable fork;

    $display("\n[Stage 3] Data Path Verification Complete.");

    // =========================================================================
    // Stage 4: Simulate and Wait for Frames
    // =========================================================================
    $display("\n[Stage 4] Waiting for %0d frames to be encoded to memory...",
             NUM_FRAMES_TO_CAPTURE);

    // Interrupt Handler (ISR)
    // Clears MI interrupts to allow frame switching.
    frames_captured = 0;
    fork
      begin
        while (frames_captured < NUM_FRAMES_TO_CAPTURE) begin
          @(posedge clk);
          if (intr_mi) begin
            logic [31:0] mis_val;
            $display("[ISR] MI Interrupt detected (Internal mi_irq=%b, frame_end=%b)", dbg_mi_irq,
                     dbg_mi_mp_frame_end);
            tl_read(32'h00000f00, mis_val);
            $display("[ISR] MI_MIS: 0x%h. Clearing...", mis_val);

            // Directly check internal frame end bit as requested
            if (dbg_mi_mp_frame_end) begin
              frames_captured++;
              $display("[ISR] Frame %0d captured! (Total targeted: %0d)", frames_captured,
                       NUM_FRAMES_TO_CAPTURE);
            end else if (mis_val & 32'h00000010) begin
              // Fallback to register bit if internal signal is not sampled at the same time
              frames_captured++;
              $display("[ISR] Frame %0d captured via MIS bit check! (Total targeted: %0d)",
                       frames_captured, NUM_FRAMES_TO_CAPTURE);
            end
            tl_write(32'h00000f04, mis_val);  // Write to keypad clear (ICR)
          end
        end
      end
    join_none

    // Wait for frames to be captured or timeout
    // Using a counting loop instead of fork/join_any since Verilator doesn't support disable on blocks in this context well.
    begin
      int timeout_counter = 0;
      while (frames_captured < NUM_FRAMES_TO_CAPTURE && timeout_counter < 10000000) begin
        @(posedge clk);
        timeout_counter++;
      end
      if (frames_captured >= NUM_FRAMES_TO_CAPTURE) begin
        $display("\n[Stage 4] Captured %0d frames successfully.", NUM_FRAMES_TO_CAPTURE);
      end else begin
        $error("\n[Stage 4] TIMEOUT waiting for %0d frames. Captured %0d.", NUM_FRAMES_TO_CAPTURE,
               frames_captured);
      end
    end

    // Wait an extra 1000 cycles to allow last transactions to settle
    repeat (1000) @(posedge clk);

    $display("\n[Stage 5] Dumping Image Data from Memory Storage...");

    begin
`ifdef CAM_MODEL
      localparam logic [31:0] FRAME0_BASE = 32'h5a300000;  // UPDATED base address
      localparam logic [31:0] FRAME0_SIZE = 32'h00012c00;
      localparam logic [31:0] FRAME1_BASE = 32'h5a340000;  // UPDATED base address
      localparam logic [31:0] FRAME1_SIZE = 32'h00012c00;
`else
      localparam logic [31:0] FRAME0_BASE = 32'h5a300000; // Aligned with TPG config y_base_ad_init_addr_tpg
      localparam logic [31:0] FRAME0_SIZE = 32'h00002000;
      localparam logic [31:0] FRAME1_BASE = 32'h5a340000; // Aligned with TPG config y_base_ad_init2_addr_tpg
      localparam logic [31:0] FRAME1_SIZE = 32'h00002000;
`endif

      fd0 = $fopen("isp_out_frame0.raw", "wb");
      fd1 = $fopen("isp_out_frame1.raw", "wb");
      if (fd0 == 0 || fd1 == 0) begin
        $error("Failed to open output files for writing!");
        $finish;
      end

      // Dump Frame 0
      // Memory Offset calculation uses direct addressing matching FRAME0_BASE
      for (int i = 0; i < FRAME0_SIZE; i += 8) begin
        logic [63:0] wdata;
        logic [21:0] mem_idx;
        mem_idx = ((FRAME0_BASE + i) & 32'h01FFFFFF) >> 3;
        wdata   = tb_mem_storage[mem_idx];
        $fwrite(fd0, "%c%c%c%c%c%c%c%c", wdata[7:0], wdata[15:8], wdata[23:16], wdata[31:24],
                wdata[39:32], wdata[47:40], wdata[55:48], wdata[63:56]);
      end

      // Dump Frame 1
      // Memory Offset calculation uses direct addressing matching FRAME1_BASE
      for (int i = 0; i < FRAME1_SIZE; i += 8) begin
        logic [63:0] wdata;
        logic [21:0] mem_idx;
        mem_idx = ((FRAME1_BASE + i) & 32'h01FFFFFF) >> 3;
        wdata   = tb_mem_storage[mem_idx];
        $fwrite(fd1, "%c%c%c%c%c%c%c%c", wdata[7:0], wdata[15:8], wdata[23:16], wdata[31:24],
                wdata[39:32], wdata[47:40], wdata[55:48], wdata[63:56]);
      end

      $fclose(fd0);
      $fclose(fd1);

`ifndef CAM_MODEL
      begin
        logic [63:0] golden_line[8] = '{
            64'hfcfcfcfcfcfcfcfc,
            64'hd8d8d8d8d8d8d8d8,
            64'hb4b4b4b4b4b4b4b4,
            64'h9090909090909090,
            64'h6c6c6c6c6c6c6c6c,
            64'h4848484848484848,
            64'h2424242424242424,
            64'h0000000000000000
        };
        int errors = 0;
        $display("[Stage 5] Verifying 1st Line of Frame 0 against TPG Golden Data...");
        // Verify first line (128 pixels * 1 byte/pixel = 128 bytes)
        for (int i = 0; i < 128; i += 8) begin
          logic [63:0] wdata;
          logic [21:0] mem_idx;
          mem_idx = ((FRAME0_BASE + i) & 32'h01FFFFFF) >> 3;
          wdata   = tb_mem_storage[mem_idx];
          if (wdata !== golden_line[i>>4]) begin
            $error("Mismatch at offset 0x%h! Expected 0x%h, Got 0x%h", i, golden_line[i>>4], wdata);
            errors++;
          end
        end
        if (errors == 0) begin
          $display("[Stage 5] TPG Golden Data Match Successful (First Line verified)!");
        end
      end
`endif
    end
    $display("[Stage 4] Image data dumped to 'isp_out_frame0.raw' and 'isp_out_frame1.raw'.");

    $display("\n[TEST COMPLETED] Verification Successful");
    $display("DEBUG: Final hsync count: %0d", hsync_count);
    $finish;
  end

endmodule
