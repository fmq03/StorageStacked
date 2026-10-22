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


module isp_wrapper
  import tlul_pkg::*;
  import coralnpu_tlul_pkg_32::*;
#(
    parameter int AhbLiteDataWidth = 32,
    parameter int TimeoutLimit     = 65535
) (
    input                                 clk_i,       //tlul/ahb clk
    input                                 clk_core_i,  //isp core clk
    input                                 clk_axi_i,   //axi clk
    input                                 rst_ni,
    //tlul bus
    input  coralnpu_tlul_pkg_32::tl_h2d_t tl_i,
    output coralnpu_tlul_pkg_32::tl_d2h_t tl_o,

    // AXI M1 Port
    output axi_m1_awvalid,
    input axi_m1_awready,
    output [31:0] axi_m1_awaddr,
    output [3:0] axi_m1_awid,
    output [3:0] axi_m1_awlen,
    output [2:0] axi_m1_awsize,
    output [1:0] axi_m1_awburst,
    output [1:0] axi_m1_awlock,
    output [3:0] axi_m1_awcache,
    output [2:0] axi_m1_awprot,
    output [3:0] axi_m1_awqos,
    output [3:0] axi_m1_awregion,
    output axi_m1_wvalid,
    input axi_m1_wready,
    output [63:0] axi_m1_wdata,
    output [7:0] axi_m1_wstrb,
    output axi_m1_wlast,
    output [3:0] axi_m1_wid,
    input axi_m1_bvalid,
    output axi_m1_bready,
    input [1:0] axi_m1_bresp,
    input [3:0] axi_m1_bid,
    input axi_m1_arready,
    output axi_m1_arvalid,
    output [31:0] axi_m1_araddr,
    output [3:0] axi_m1_arid,
    output [3:0] axi_m1_arlen,
    output [2:0] axi_m1_arsize,
    output [1:0] axi_m1_arburst,
    output [1:0] axi_m1_arlock,
    output [3:0] axi_m1_arcache,
    output [2:0] axi_m1_arprot,
    output [3:0] axi_m1_arqos,
    output [3:0] axi_m1_arregion,
    input axi_m1_rvalid,
    output axi_m1_rready,
    input [63:0] axi_m1_rdata,
    input [1:0] axi_m1_rresp,
    input [3:0] axi_m1_rid,
    input axi_m1_rlast,

    // AXI M2 Port (SP)
    output axi_m2_awvalid,
    input axi_m2_awready,
    output [31:0] axi_m2_awaddr,
    output [3:0] axi_m2_awid,
    output [3:0] axi_m2_awlen,
    output [2:0] axi_m2_awsize,
    output [1:0] axi_m2_awburst,
    output [1:0] axi_m2_awlock,
    output [3:0] axi_m2_awcache,
    output [2:0] axi_m2_awprot,
    output [3:0] axi_m2_awqos,
    output [3:0] axi_m2_awregion,
    output axi_m2_wvalid,
    input axi_m2_wready,
    output [63:0] axi_m2_wdata,
    output [7:0] axi_m2_wstrb,
    output axi_m2_wlast,
    output [3:0] axi_m2_wid,
    input axi_m2_bvalid,
    output axi_m2_bready,
    input [1:0] axi_m2_bresp,
    input [3:0] axi_m2_bid,
    input axi_m2_arready,  // unused
    output axi_m2_arvalid,  // unused
    output [31:0] axi_m2_araddr,  // unused
    output [3:0] axi_m2_arid,  // unused
    output [3:0] axi_m2_arlen,  // unused
    output [2:0] axi_m2_arsize,  // unused
    output [1:0] axi_m2_arburst,  // unused
    output [1:0] axi_m2_arlock,  // unused
    output [3:0] axi_m2_arcache,  // unused
    output [2:0] axi_m2_arprot,  // unused
    output [3:0] axi_m2_arqos,  // unused
    output [3:0] axi_m2_arregion,  // unused
    input axi_m2_rvalid,  // unused
    output axi_m2_rready,  // unused
    input [63:0] axi_m2_rdata,  // unused
    input [1:0] axi_m2_rresp,  // unused
    input [3:0] axi_m2_rid,  // unused
    input axi_m2_rlast,  // unused

    //sensor signal
    input cio_s_pclk_i,  //sensor clk
    input [7:0] cio_s_data_i,
    input cio_s_hsync_i,
    input cio_s_vsync_i,
    //interrupt signal
    output intr_mi_o,
    output intr_isp_o,
    //debug signals
    // intr_mi_o and intr_isp_o are already declared above
    //the others
    input disable_isp_i,
    input scanmode_i
);

  ahb_pkg::ahb_h2d_t ahb_h2d;
  ahb_pkg::ahb_d2h_t ahb_d2h;

  // tl_i and tl_o are now module ports
  // assign tl_o_d_corrupt = tl_o.d_corrupt; // Not in some tl_d2h_t definitions, check if needed

  axi_pkg::axi_req_t axi_o;
  axi_pkg::axi_rsp_t axi_i;

  // Assignments for AXI M1
  assign axi_m1_awvalid = axi_o.awvalid;
  assign axi_i.awready = axi_m1_awready;
  assign axi_m1_awaddr = axi_o.awaddr;
  assign axi_m1_awid = axi_o.awid;
  assign axi_m1_awlen = axi_o.awlen[3:0];
  assign axi_m1_awsize = axi_o.awsize;
  assign axi_m1_awburst = axi_o.awburst;
  assign axi_m1_awlock = axi_o.awlock;
  assign axi_m1_awcache = axi_o.awcache;
  assign axi_m1_awprot = axi_o.awprot;
  assign axi_m1_awqos = '0;
  assign axi_m1_awregion = '0;
  assign axi_m1_wvalid = axi_o.wvalid;
  assign axi_i.wready = axi_m1_wready;
  assign axi_m1_wdata = axi_o.wdata;
  assign axi_m1_wstrb = axi_o.wstrb;
  assign axi_m1_wlast = axi_o.wlast;
  assign axi_m1_wid = axi_o.wid;
  assign axi_i.bvalid = axi_m1_bvalid;
  assign axi_m1_bready = axi_o.bready;
  assign axi_i.bresp = axi_m1_bresp;
  assign axi_i.bid = axi_m1_bid;

  // Read channel unused by ISP M1 (it's write only?) - Double check if ISP reads from M1
  assign axi_i.arready = axi_m1_arready;
  assign axi_m1_arvalid = axi_o.arvalid;
  assign axi_m1_araddr = axi_o.araddr;
  assign axi_m1_arid = axi_o.arid;
  assign axi_m1_arlen = axi_o.arlen[3:0];
  assign axi_m1_arsize = axi_o.arsize;
  assign axi_m1_arburst = axi_o.arburst;
  assign axi_m1_arlock = axi_o.arlock;
  assign axi_m1_arcache = axi_o.arcache;
  assign axi_m1_arprot = axi_o.arprot;
  assign axi_m1_arqos = '0;
  assign axi_m1_arregion = '0;
  assign axi_i.rvalid = axi_m1_rvalid;
  assign axi_m1_rready = axi_o.rready;
  assign axi_i.rdata = axi_m1_rdata;
  assign axi_i.rresp = axi_m1_rresp;
  assign axi_i.rid = axi_m1_rid;
  assign axi_i.rlast = axi_m1_rlast;


  axi_pkg::axi_req_t axi_sp_o;
  axi_pkg::axi_rsp_t axi_sp_i;

  // Assignments for AXI M2
  assign axi_m2_awvalid = axi_sp_o.awvalid;
  assign axi_sp_i.awready = axi_m2_awready;
  assign axi_m2_awaddr = axi_sp_o.awaddr;
  assign axi_m2_awid = axi_sp_o.awid;
  assign axi_m2_awlen = axi_sp_o.awlen[3:0];
  assign axi_m2_awsize = axi_sp_o.awsize;
  assign axi_m2_awburst = axi_sp_o.awburst;
  assign axi_m2_awlock = axi_sp_o.awlock;
  assign axi_m2_awcache = axi_sp_o.awcache;
  assign axi_m2_awprot = axi_sp_o.awprot;
  assign axi_m2_awqos = '0;
  assign axi_m2_awregion = '0;
  assign axi_m2_wvalid = axi_sp_o.wvalid;
  assign axi_sp_i.wready = axi_m2_wready;
  assign axi_m2_wdata = axi_sp_o.wdata;
  assign axi_m2_wstrb = axi_sp_o.wstrb;
  assign axi_m2_wlast = axi_sp_o.wlast;
  assign axi_m2_wid = axi_sp_o.wid;
  assign axi_sp_i.bvalid = axi_m2_bvalid;
  assign axi_m2_bready = axi_sp_o.bready;
  assign axi_sp_i.bresp = axi_m2_bresp;
  assign axi_sp_i.bid = axi_m2_bid;

  // Read channel unused by ISP M2
  assign axi_sp_i.arready = 1'b0;
  // assign axi_m2_arvalid = 1'b0;
  assign axi_m2_arqos = '0;
  assign axi_m2_arregion = '0;
  // ... (Assign others to 0 or leave unconnected if output)
  assign axi_sp_i.rvalid = 1'b0;
  assign axi_sp_i.rdata = '0;
  assign axi_sp_i.rresp = 2'b00;
  assign axi_sp_i.rid = '0;
  assign axi_sp_i.rlast = 1'b1;

  wire [2:0] tl_d_opcode;
  // unused sram port
  assign isp_caddr_o = '0;
  // unused sram port
  assign isp_sp_caddr_o = '0;

  // Integrity is regenerated by the upstream xbar on the device port; drive
  // d_user to zero and wire d fields directly.
  assign tl_o.d_user = '0;
  assign tl_o.d_opcode = tlul_pkg::tl_d_op_e'(tl_d_opcode);
  assign tl_o.d_param = 3'h0;
  assign tl_o.d_sink = 1'b0;

  wire [31:0] tl_i_a_address_masked;
  assign tl_i_a_address_masked = tl_i.a_address & 32'h00FFFFFF;
  tlul2ahblite #(
      .TlulDataWidth(top_pkg::TL_DW),
      .AhbLiteDataWidth(AhbLiteDataWidth),
      .IdWidth(top_pkg::TL_AIW),
      .SizeWidth(top_pkg::TL_SZW),
      .TimeoutLimit(TimeoutLimit)
  ) u_tlul2ahblite (
      // Outputs
      .a_ready  (tl_o.a_ready),
      .d_error  (tl_o.d_error),
      .d_source (tl_o.d_source),
      .d_opcode (tl_d_opcode),
      .d_size   (tl_o.d_size),
      .d_valid  (tl_o.d_valid),
      .d_data   (tl_o.d_data),
      .haddr    (ahb_h2d.haddr),
      .hburst   (ahb_h2d.hburst),
      .hready   (ahb_h2d.hready),
      .hsel     (ahb_h2d.hsel),
      .hsize    (ahb_h2d.hsize),
      .htrans   (ahb_h2d.htrans),
      .hwdata   (ahb_h2d.hwdata),
      .hwrite   (ahb_h2d.hwrite),
      // Inputs
      .clk      (clk_i),
      .rstn     (rst_ni),
      // Convert PutPartialData (1) to PutFullData (0) since AHB-Lite here ignores byte enables 
      // and tlul_dec rejects PutPartialData.
      .a_opcode (tl_i.a_opcode == tlul_pkg::PutPartialData ? tlul_pkg::PutFullData : tl_i.a_opcode),
      // .a_opcode          (tl_i.a_opcode),
      .a_size   (tl_i.a_size),
      .a_source (tl_i.a_source),
      // .a_address         (tl_i.a_address),
      .a_address(tl_i_a_address_masked),
      .a_mask   (tl_i.a_mask),
      .a_data   (tl_i.a_data),
      .a_valid  (tl_i.a_valid),
      .d_ready  (tl_i.d_ready),
      .hrdata   (ahb_d2h.hrdata),
      .hreadyout(ahb_d2h.hready),
      .hresp    (ahb_d2h.hresp)
  );

  logic [1:0] hresp_marvin_out_pre;
  assign ahb_d2h.hresp = hresp_marvin_out_pre[0];

  VSISP_MARVIN_TOP_X u_isp (  /*AUTOINST*/
      //MP Outputs
      .axi_m1_marvin_awvalid(axi_o.awvalid),
      .axi_m1_marvin_awaddr(axi_o.awaddr[31:3]),
      .axi_m1_marvin_awlen(axi_o.awlen[3:0]),
      .axi_m1_marvin_awsize(axi_o.awsize[2:0]),
      .axi_m1_marvin_awburst(axi_o.awburst[1:0]),
      .axi_m1_marvin_awlock(axi_o.awlock[1:0]),
      .axi_m1_marvin_awcache(axi_o.awcache[3:0]),
      .axi_m1_marvin_awprot(axi_o.awprot[2:0]),
      .axi_m1_marvin_awid(axi_o.awid[3:0]),
      .axi_m1_marvin_wvalid(axi_o.wvalid),
      .axi_m1_marvin_wlast(axi_o.wlast),
      .axi_m1_marvin_wdata(axi_o.wdata[63:0]),
      .axi_m1_marvin_wstrb(axi_o.wstrb[7:0]),
      .axi_m1_marvin_wid(axi_o.wid[3:0]),
      .axi_m1_marvin_bready(axi_o.bready),
      // SP Outputs
      .axi_m2_marvin_awvalid(axi_sp_o.awvalid),
      .axi_m2_marvin_awaddr(axi_sp_o.awaddr[31:3]),
      .axi_m2_marvin_awlen(axi_sp_o.awlen[3:0]),
      .axi_m2_marvin_awsize(axi_sp_o.awsize[2:0]),
      .axi_m2_marvin_awburst(axi_sp_o.awburst[1:0]),
      .axi_m2_marvin_awlock(axi_sp_o.awlock[1:0]),
      .axi_m2_marvin_awcache(axi_sp_o.awcache[3:0]),
      .axi_m2_marvin_awprot(axi_sp_o.awprot[2:0]),
      .axi_m2_marvin_awid(axi_sp_o.awid[3:0]),
      .axi_m2_marvin_wvalid(axi_sp_o.wvalid),
      .axi_m2_marvin_wlast(axi_sp_o.wlast),
      .axi_m2_marvin_wdata(axi_sp_o.wdata[63:0]),
      .axi_m2_marvin_wstrb(axi_sp_o.wstrb[7:0]),
      .axi_m2_marvin_wid(axi_sp_o.wid[3:0]),
      .axi_m2_marvin_bready(axi_sp_o.bready),
      .hrdata_s(ahb_d2h.hrdata),
      .hresp_s                            (hresp_marvin_out_pre), // Widen single bit to 2 bits for port compatibility vsisp_marvin
      .hready_s(ahb_d2h.hready),
      .mi_irq(intr_mi_o),
      .isp_irq(intr_isp_o),

      // Unused Debug Outputs
      .out_y_r_frame_start  (),
      .out_y_r_frame_end    (),
      .out_y_r_line_start   (),
      .out_y_r_line_end     (),
      .out_cb_g_frame_start (),
      .out_cb_g_frame_end   (),
      .out_cb_g_line_start  (),
      .out_cb_g_line_end    (),
      .out_cr_b_frame_start (),
      .out_cr_b_frame_end   (),
      .out_cr_b_line_start  (),
      .out_cr_b_line_end    (),
      .out_y_r_val_stream   (),
      .out_y_r_data_stream  (),
      .out_cb_g_val_stream  (),
      .out_cb_g_data_stream (),
      .out_cr_b_val_stream  (),
      .out_cr_b_data_stream (),
      // Inputs
      .clk                  (clk_core_i),
      .reset_n              (rst_ni),
      .sclk                 (cio_s_pclk_i),
      .s_hclk               (clk_i),
      .m_hclk               (clk_axi_i),
      .axi_m1_marvin_awready(axi_i.awready),
      .axi_m1_marvin_wready (axi_i.wready),
      .axi_m1_marvin_bvalid (axi_i.bvalid),
      .axi_m1_marvin_bresp  (axi_i.bresp[1:0]),
      .axi_m1_marvin_bid    (axi_i.bid[3:0]),
      //SP
      .axi_m2_marvin_awready(axi_sp_i.awready),
      .axi_m2_marvin_wready (axi_sp_i.wready),
      .axi_m2_marvin_bvalid (axi_sp_i.bvalid),
      .axi_m2_marvin_bresp  (axi_sp_i.bresp[1:0]),
      .axi_m2_marvin_bid    (axi_sp_i.bid[3:0]),
      .hsel_s               (ahb_h2d.hsel),
      .haddr_s              (ahb_h2d.haddr),
      .htrans_s             (ahb_h2d.htrans),
      .hwrite_s             (ahb_h2d.hwrite),
      .hwdata_s             (ahb_h2d.hwdata),
      .s_data               ({4'h0, cio_s_data_i}),
      .s_hsync              (cio_s_hsync_i),
      .s_vsync              (cio_s_vsync_i),
      .s_valid              (cio_s_hsync_i),
      .disable_isp          (disable_isp_i),
      .scan_mode            (scanmode_i),
      .out_y_r_ack_stream   (1'b1),
      .out_cb_g_ack_stream  (1'b1),
      .out_cr_b_ack_stream  (1'b1)
  );


  // Removed axi2sramcrs instances
  `define SIMULATION_QUIET
`ifndef SIMULATION_QUIET
  // Edge detection registers to ensure messages are logged once per event occurrence
  logic tl_a_active_q;  // Tracks if TLUL request handshake was active last cycle
  logic tl_a_stall_q;  // Tracks if TLUL request was stalled last cycle
  logic tl_d_active_q;  // Tracks if TLUL response handshake was active last cycle
  logic ahb_req_active_q;  // Tracks if AHB request was active last cycle
  logic ahb_rsp_active_q;  // Tracks if AHB response was active last cycle

  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin  // Reset state for tracking registers
      tl_a_active_q    <= 1'b0;  // Initialize TLUL A active state
      tl_a_stall_q     <= 1'b0;  // Initialize TLUL A stall state
      tl_d_active_q    <= 1'b0;  // Initialize TLUL D active state
      ahb_req_active_q <= 1'b0;  // Initialize AHB request active state
      ahb_rsp_active_q <= 1'b0;  // Initialize AHB response active state
    end else begin
      // Monitor TileLink A-Channel (Requests from Master)
      if (tl_i.a_valid) begin  // Check if a valid TileLink request is present
        if (tl_o.a_ready) begin  // Check if the slave is ready to accept the request
          // Log the transaction once upon successful handshake
          if (!tl_a_active_q)  // Ensure request is logged only on transition to active
            $display(
                "[ISP_WRAPPER_DEBUG] TLUL REQ: addr=0x%h, masked_addr=0x%h, opcode=%d, data=0x%h, size=%0d, mask=0x%h",
                tl_i.a_address,
                tl_i_a_address_masked,
                tl_i.a_opcode,
                tl_i.a_data,
                tl_i.a_size,
                tl_i.a_mask
            );  // Log address, opcode and data
          tl_a_active_q <= 1'b1;  // Mark request handshake as active
          tl_a_stall_q  <= 1'b0;  // Reset stall state
        end else begin  // Slave is not ready, request is stalling
          // Log the stall event only on the first cycle it occurs
          if (!tl_a_stall_q)  // Check if this is the start of a stall
            $display(
                "[ISP_WRAPPER_DEBUG] TLUL REQ STALLED: addr=0x%h", tl_i.a_address
            );  // Log the stalled address
          tl_a_active_q <= 1'b0;  // Request handshake is not active
          tl_a_stall_q  <= 1'b1;  // Mark as stalling
        end
      end else begin  // No valid request present
        tl_a_active_q <= 1'b0;  // Reset active state
        tl_a_stall_q  <= 1'b0;  // Reset stall state
      end

      // Monitor TileLink D-Channel (Responses from Slave)
      if (tl_o.d_valid && tl_i.d_ready) begin  // Check if a valid response handshake is occurring
        // Log the response once upon successful handshake
        if (!tl_d_active_q)  // Ensure response is logged only on transition to active
          $display(
              "[ISP_WRAPPER_DEBUG] TLUL RSP: opcode=%d, data=0x%h, d_error=%b (err_reg=%b, timeout=%b, ahb_err=%b)",
              tl_o.d_opcode,
              tl_o.d_data,
              tl_o.d_error,
              u_tlul2ahblite.u_tlul_dec.error,
              u_tlul2ahblite.u_tlul_dec.timeout,
              |u_tlul2ahblite.resp
          );  // Log response opcode, data and error status
        tl_d_active_q <= 1'b1;  // Mark response handshake as active
      end else begin  // No valid response handshake
        tl_d_active_q <= 1'b0;  // Reset active state
      end

      // Monitor AHB-Lite Address Phase (Master signaling)
      if (ahb_h2d.hsel && ahb_d2h.hready && ahb_h2d.htrans > 1) begin // Check for a valid AHB transaction (NONSEQ or SEQ)
        // Log the AHB request once per transaction
        if (!ahb_req_active_q)  // Ensure AHB request is logged only once
          $display(
              "[ISP_WRAPPER_DEBUG] AHB REQ: haddr=0x%h, hwrite=%b, htrans=%d, hwdata=0x%h",
              ahb_h2d.haddr,
              ahb_h2d.hwrite,
              ahb_h2d.htrans,
              ahb_h2d.hwdata
          );  // Log address, write status, transaction type and write data
        ahb_req_active_q <= 1'b1;  // Mark AHB request as active
      end else begin  // No AHB transaction active
        ahb_req_active_q <= 1'b0;  // Reset active state
      end

      // Monitor AHB-Lite Data Phase (Slave signaling ready)
      if (ahb_d2h.hready && ahb_h2d.htrans != 0) begin // Check if AHB slave has asserted ready during a transaction
        // Log the AHB response once when ready out is high
        if (!ahb_rsp_active_q)  // Ensure AHB response is logged only once
          $display(
              "[ISP_WRAPPER_DEBUG] AHB RSP: rdata=0x%h, readyout=%b", ahb_d2h.hrdata, ahb_d2h.hready
          );  // Log read data and ready status
        ahb_rsp_active_q <= 1'b1;  // Mark AHB response as active
      end else begin  // AHB slave not ready or idle
        ahb_rsp_active_q <= 1'b0;  // Reset active state
      end
    end
  end
`endif

endmodule
