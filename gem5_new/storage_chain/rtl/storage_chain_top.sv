`timescale 1ns/1ps

// Transparent AXI4 observation boundary used by the RTL regression.
//
// This module is deliberately not a UCIe link, memory controller, DFI PHY or
// memory timing model.  Every AXI signal passes through combinationally; the
// passive monitor emits HETTrace-v2-shaped events and the checker observes
// ready/valid stability.  Production storage timing is evaluated offline by
// converting the captured HETTrace to the external mem_sim/hbm_sim format.

module storage_chain_top #(
    parameter integer ADDR_WIDTH       = 32,
    parameter integer DATA_WIDTH       = 64,
    parameter integer ID_WIDTH         = 4,
    parameter integer USER_WIDTH       = 2,
    parameter logic [63:0] TICKS_PER_CYCLE = 64'd1000
) (
    input  wire                      clk,
    input  wire                      rst_n,

    input  wire [ID_WIDTH-1:0]       s_axi_awid,
    input  wire [ADDR_WIDTH-1:0]     s_axi_awaddr,
    input  wire [7:0]                s_axi_awlen,
    input  wire [2:0]                s_axi_awsize,
    input  wire [1:0]                s_axi_awburst,
    input  wire [USER_WIDTH-1:0]     s_axi_awuser,
    input  wire                      s_axi_awvalid,
    output wire                      s_axi_awready,

    input  wire [DATA_WIDTH-1:0]     s_axi_wdata,
    input  wire [DATA_WIDTH/8-1:0]   s_axi_wstrb,
    input  wire                      s_axi_wlast,
    input  wire                      s_axi_wvalid,
    output wire                      s_axi_wready,

    output wire [ID_WIDTH-1:0]       s_axi_bid,
    output wire [1:0]                s_axi_bresp,
    output wire                      s_axi_bvalid,
    input  wire                      s_axi_bready,

    input  wire [ID_WIDTH-1:0]       s_axi_arid,
    input  wire [ADDR_WIDTH-1:0]     s_axi_araddr,
    input  wire [7:0]                s_axi_arlen,
    input  wire [2:0]                s_axi_arsize,
    input  wire [1:0]                s_axi_arburst,
    input  wire [USER_WIDTH-1:0]     s_axi_aruser,
    input  wire                      s_axi_arvalid,
    output wire                      s_axi_arready,

    output wire [ID_WIDTH-1:0]       s_axi_rid,
    output wire [DATA_WIDTH-1:0]     s_axi_rdata,
    output wire [1:0]                s_axi_rresp,
    output wire                      s_axi_rlast,
    output wire                      s_axi_rvalid,
    input  wire                      s_axi_rready,

    output wire [ID_WIDTH-1:0]       m_axi_awid,
    output wire [ADDR_WIDTH-1:0]     m_axi_awaddr,
    output wire [7:0]                m_axi_awlen,
    output wire [2:0]                m_axi_awsize,
    output wire [1:0]                m_axi_awburst,
    output wire [USER_WIDTH-1:0]     m_axi_awuser,
    output wire                      m_axi_awvalid,
    input  wire                      m_axi_awready,

    output wire [DATA_WIDTH-1:0]     m_axi_wdata,
    output wire [DATA_WIDTH/8-1:0]   m_axi_wstrb,
    output wire                      m_axi_wlast,
    output wire                      m_axi_wvalid,
    input  wire                      m_axi_wready,

    input  wire [ID_WIDTH-1:0]       m_axi_bid,
    input  wire [1:0]                m_axi_bresp,
    input  wire                      m_axi_bvalid,
    output wire                      m_axi_bready,

    output wire [ID_WIDTH-1:0]       m_axi_arid,
    output wire [ADDR_WIDTH-1:0]     m_axi_araddr,
    output wire [7:0]                m_axi_arlen,
    output wire [2:0]                m_axi_arsize,
    output wire [1:0]                m_axi_arburst,
    output wire [USER_WIDTH-1:0]     m_axi_aruser,
    output wire                      m_axi_arvalid,
    input  wire                      m_axi_arready,

    input  wire [ID_WIDTH-1:0]       m_axi_rid,
    input  wire [DATA_WIDTH-1:0]     m_axi_rdata,
    input  wire [1:0]                m_axi_rresp,
    input  wire                      m_axi_rlast,
    input  wire                      m_axi_rvalid,
    output wire                      m_axi_rready,

    output wire                      trace_valid,
    output wire [63:0]               trace_tick,
    output wire [63:0]               trace_addr,
    output wire [63:0]               trace_strb,
    output wire [31:0]               trace_size,
    output wire [31:0]               trace_ctx,
    output wire [31:0]               trace_seq,
    output wire [31:0]               trace_txn,
    output wire [15:0]               trace_src_id,
    output wire [15:0]               trace_axi_id,
    output wire                      trace_op,
    output wire [2:0]                trace_chan,
    output wire [7:0]                trace_axi_len,
    output wire [2:0]                trace_axi_size,
    output wire [1:0]                trace_burst,
    output wire [1:0]                trace_resp,
    output wire [7:0]                trace_user,
    output wire [7:0]                trace_flags,
    output wire [31:0]               trace_event_count,
    output wire [31:0]               trace_transaction_count
);
    assign m_axi_awid    = s_axi_awid;
    assign m_axi_awaddr  = s_axi_awaddr;
    assign m_axi_awlen   = s_axi_awlen;
    assign m_axi_awsize  = s_axi_awsize;
    assign m_axi_awburst = s_axi_awburst;
    assign m_axi_awuser  = s_axi_awuser;
    assign m_axi_awvalid = s_axi_awvalid;
    assign s_axi_awready = m_axi_awready;

    assign m_axi_wdata   = s_axi_wdata;
    assign m_axi_wstrb   = s_axi_wstrb;
    assign m_axi_wlast   = s_axi_wlast;
    assign m_axi_wvalid  = s_axi_wvalid;
    assign s_axi_wready  = m_axi_wready;

    assign s_axi_bid     = m_axi_bid;
    assign s_axi_bresp   = m_axi_bresp;
    assign s_axi_bvalid  = m_axi_bvalid;
    assign m_axi_bready  = s_axi_bready;

    assign m_axi_arid    = s_axi_arid;
    assign m_axi_araddr  = s_axi_araddr;
    assign m_axi_arlen   = s_axi_arlen;
    assign m_axi_arsize  = s_axi_arsize;
    assign m_axi_arburst = s_axi_arburst;
    assign m_axi_aruser  = s_axi_aruser;
    assign m_axi_arvalid = s_axi_arvalid;
    assign s_axi_arready = m_axi_arready;

    assign s_axi_rid     = m_axi_rid;
    assign s_axi_rdata   = m_axi_rdata;
    assign s_axi_rresp   = m_axi_rresp;
    assign s_axi_rlast   = m_axi_rlast;
    assign s_axi_rvalid  = m_axi_rvalid;
    assign m_axi_rready  = s_axi_rready;

    axi_subset_checker #(
        .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH(ID_WIDTH), .USER_WIDTH(USER_WIDTH)
    ) u_axi_checker (
        .clk(clk), .rst_n(rst_n),
        .axi_awid(s_axi_awid), .axi_awaddr(s_axi_awaddr),
        .axi_awlen(s_axi_awlen), .axi_awsize(s_axi_awsize),
        .axi_awburst(s_axi_awburst), .axi_awuser(s_axi_awuser),
        .axi_awvalid(s_axi_awvalid), .axi_awready(s_axi_awready),
        .axi_wdata(s_axi_wdata), .axi_wstrb(s_axi_wstrb),
        .axi_wlast(s_axi_wlast), .axi_wvalid(s_axi_wvalid),
        .axi_wready(s_axi_wready),
        .axi_bid(s_axi_bid), .axi_bresp(s_axi_bresp),
        .axi_bvalid(s_axi_bvalid), .axi_bready(s_axi_bready),
        .axi_arid(s_axi_arid), .axi_araddr(s_axi_araddr),
        .axi_arlen(s_axi_arlen), .axi_arsize(s_axi_arsize),
        .axi_arburst(s_axi_arburst), .axi_aruser(s_axi_aruser),
        .axi_arvalid(s_axi_arvalid), .axi_arready(s_axi_arready),
        .axi_rid(s_axi_rid), .axi_rdata(s_axi_rdata),
        .axi_rresp(s_axi_rresp), .axi_rlast(s_axi_rlast),
        .axi_rvalid(s_axi_rvalid), .axi_rready(s_axi_rready)
    );

    axi_hettrace_monitor #(
        .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH(ID_WIDTH), .USER_WIDTH(USER_WIDTH),
        .TICKS_PER_CYCLE(TICKS_PER_CYCLE)
    ) trace_monitor (
        .clk(clk), .rst_n(rst_n),
        .axi_awid(s_axi_awid), .axi_awaddr(s_axi_awaddr),
        .axi_awlen(s_axi_awlen), .axi_awsize(s_axi_awsize),
        .axi_awburst(s_axi_awburst), .axi_awuser(s_axi_awuser),
        .axi_awvalid(s_axi_awvalid), .axi_awready(s_axi_awready),
        .axi_wdata(s_axi_wdata), .axi_wstrb(s_axi_wstrb),
        .axi_wlast(s_axi_wlast), .axi_wvalid(s_axi_wvalid),
        .axi_wready(s_axi_wready),
        .axi_bid(s_axi_bid), .axi_bresp(s_axi_bresp),
        .axi_bvalid(s_axi_bvalid), .axi_bready(s_axi_bready),
        .axi_arid(s_axi_arid), .axi_araddr(s_axi_araddr),
        .axi_arlen(s_axi_arlen), .axi_arsize(s_axi_arsize),
        .axi_arburst(s_axi_arburst), .axi_aruser(s_axi_aruser),
        .axi_arvalid(s_axi_arvalid), .axi_arready(s_axi_arready),
        .axi_rid(s_axi_rid), .axi_rdata(s_axi_rdata),
        .axi_rresp(s_axi_rresp), .axi_rlast(s_axi_rlast),
        .axi_rvalid(s_axi_rvalid), .axi_rready(s_axi_rready),
        .trace_valid(trace_valid), .trace_tick(trace_tick),
        .trace_addr(trace_addr), .trace_strb(trace_strb),
        .trace_size(trace_size), .trace_ctx(trace_ctx),
        .trace_seq(trace_seq), .trace_txn(trace_txn),
        .trace_src_id(trace_src_id), .trace_axi_id(trace_axi_id),
        .trace_op(trace_op), .trace_chan(trace_chan),
        .trace_axi_len(trace_axi_len), .trace_axi_size(trace_axi_size),
        .trace_burst(trace_burst), .trace_resp(trace_resp),
        .trace_user(trace_user), .trace_flags(trace_flags),
        .trace_event_count(trace_event_count),
        .trace_transaction_count(trace_transaction_count)
    );
endmodule
