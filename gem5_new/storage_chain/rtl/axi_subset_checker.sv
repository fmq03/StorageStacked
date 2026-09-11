`timescale 1ns/1ps

// Lightweight checker for the AXI subset used by this project.  It enforces
// the central ready/valid rule on all five channels: once VALID is asserted,
// VALID and payload must remain stable until a handshake occurs.

module axi_subset_checker #(
    parameter integer ADDR_WIDTH = 32,
    parameter integer DATA_WIDTH = 64,
    parameter integer ID_WIDTH   = 4,
    parameter integer USER_WIDTH = 2
) (
    input wire clk,
    input wire rst_n,

    input wire [ID_WIDTH-1:0] axi_awid,
    input wire [ADDR_WIDTH-1:0] axi_awaddr,
    input wire [7:0] axi_awlen,
    input wire [2:0] axi_awsize,
    input wire [1:0] axi_awburst,
    input wire [USER_WIDTH-1:0] axi_awuser,
    input wire axi_awvalid,
    input wire axi_awready,

    input wire [DATA_WIDTH-1:0] axi_wdata,
    input wire [DATA_WIDTH/8-1:0] axi_wstrb,
    input wire axi_wlast,
    input wire axi_wvalid,
    input wire axi_wready,

    input wire [ID_WIDTH-1:0] axi_bid,
    input wire [1:0] axi_bresp,
    input wire axi_bvalid,
    input wire axi_bready,

    input wire [ID_WIDTH-1:0] axi_arid,
    input wire [ADDR_WIDTH-1:0] axi_araddr,
    input wire [7:0] axi_arlen,
    input wire [2:0] axi_arsize,
    input wire [1:0] axi_arburst,
    input wire [USER_WIDTH-1:0] axi_aruser,
    input wire axi_arvalid,
    input wire axi_arready,

    input wire [ID_WIDTH-1:0] axi_rid,
    input wire [DATA_WIDTH-1:0] axi_rdata,
    input wire [1:0] axi_rresp,
    input wire axi_rlast,
    input wire axi_rvalid,
    input wire axi_rready
);
    localparam integer AW_WIDTH = ID_WIDTH + ADDR_WIDTH + 8 + 3 + 2 + USER_WIDTH;
    localparam integer W_WIDTH  = DATA_WIDTH + DATA_WIDTH/8 + 1;
    localparam integer B_WIDTH  = ID_WIDTH + 2;
    localparam integer AR_WIDTH = AW_WIDTH;
    localparam integer R_WIDTH  = ID_WIDTH + DATA_WIDTH + 2 + 1;

    reg aw_stalled;
    reg w_stalled;
    reg b_stalled;
    reg ar_stalled;
    reg r_stalled;
    reg [AW_WIDTH-1:0] aw_held;
    reg [W_WIDTH-1:0] w_held;
    reg [B_WIDTH-1:0] b_held;
    reg [AR_WIDTH-1:0] ar_held;
    reg [R_WIDTH-1:0] r_held;

    wire [AW_WIDTH-1:0] aw_payload = {
        axi_awid, axi_awaddr, axi_awlen, axi_awsize, axi_awburst, axi_awuser
    };
    wire [W_WIDTH-1:0] w_payload = {axi_wdata, axi_wstrb, axi_wlast};
    wire [B_WIDTH-1:0] b_payload = {axi_bid, axi_bresp};
    wire [AR_WIDTH-1:0] ar_payload = {
        axi_arid, axi_araddr, axi_arlen, axi_arsize, axi_arburst, axi_aruser
    };
    wire [R_WIDTH-1:0] r_payload = {axi_rid, axi_rdata, axi_rresp, axi_rlast};

    always @(posedge clk) begin
        if (!rst_n) begin
            aw_stalled <= 1'b0;
            w_stalled  <= 1'b0;
            b_stalled  <= 1'b0;
            ar_stalled <= 1'b0;
            r_stalled  <= 1'b0;
            aw_held    <= {AW_WIDTH{1'b0}};
            w_held     <= {W_WIDTH{1'b0}};
            b_held     <= {B_WIDTH{1'b0}};
            ar_held    <= {AR_WIDTH{1'b0}};
            r_held     <= {R_WIDTH{1'b0}};
        end else begin
            if (aw_stalled && (!axi_awvalid || aw_payload !== aw_held))
                $fatal(1, "AXI checker: AW changed while READY was low");
            if (w_stalled && (!axi_wvalid || w_payload !== w_held))
                $fatal(1, "AXI checker: W changed while READY was low");
            if (b_stalled && (!axi_bvalid || b_payload !== b_held))
                $fatal(1, "AXI checker: B changed while READY was low");
            if (ar_stalled && (!axi_arvalid || ar_payload !== ar_held))
                $fatal(1, "AXI checker: AR changed while READY was low");
            if (r_stalled && (!axi_rvalid || r_payload !== r_held))
                $fatal(1, "AXI checker: R changed while READY was low");

            aw_stalled <= axi_awvalid && !axi_awready;
            w_stalled  <= axi_wvalid && !axi_wready;
            b_stalled  <= axi_bvalid && !axi_bready;
            ar_stalled <= axi_arvalid && !axi_arready;
            r_stalled  <= axi_rvalid && !axi_rready;
            if (axi_awvalid && !axi_awready) aw_held <= aw_payload;
            if (axi_wvalid && !axi_wready)   w_held  <= w_payload;
            if (axi_bvalid && !axi_bready)   b_held  <= b_payload;
            if (axi_arvalid && !axi_arready) ar_held <= ar_payload;
            if (axi_rvalid && !axi_rready)   r_held  <= r_payload;
        end
    end
endmodule
