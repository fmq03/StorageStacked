`timescale 1ns/1ps

// Minimal, synthesizable XPU-command to AXI4 master transactor.
//
// The XPU side submits one single-beat read or write command and does not see
// completion until the AXI B/R response has returned.  This is the important
// timing boundary: downstream READY backpressure and response latency are
// therefore visible to the producer instead of being hidden by a functional
// memory callback.
//
// This first version intentionally supports one outstanding command.  It is a
// reference integration block for CPU/NPU/GPU adapters, not a full AXI DMA.

module xpu_axi_master #(
    parameter integer ADDR_WIDTH = 32,
    parameter integer DATA_WIDTH = 64,
    parameter integer ID_WIDTH   = 4,
    parameter integer USER_WIDTH = 2
) (
    input  wire                      clk,
    input  wire                      rst_n,

    input  wire                      cmd_valid,
    output wire                      cmd_ready,
    input  wire                      cmd_write,
    input  wire [ADDR_WIDTH-1:0]     cmd_addr,
    input  wire [DATA_WIDTH-1:0]     cmd_wdata,
    input  wire [DATA_WIDTH/8-1:0]   cmd_wstrb,
    input  wire [ID_WIDTH-1:0]       cmd_id,
    input  wire [USER_WIDTH-1:0]     cmd_src,

    output reg                       rsp_valid,
    input  wire                      rsp_ready,
    output reg                       rsp_write,
    output reg  [DATA_WIDTH-1:0]     rsp_rdata,
    output reg  [1:0]                rsp_resp,
    output reg  [ID_WIDTH-1:0]       rsp_id,

    output reg  [ID_WIDTH-1:0]       m_axi_awid,
    output reg  [ADDR_WIDTH-1:0]     m_axi_awaddr,
    output wire [7:0]                m_axi_awlen,
    output wire [2:0]                m_axi_awsize,
    output wire [1:0]                m_axi_awburst,
    output reg  [USER_WIDTH-1:0]     m_axi_awuser,
    output reg                       m_axi_awvalid,
    input  wire                      m_axi_awready,

    output reg  [DATA_WIDTH-1:0]     m_axi_wdata,
    output reg  [DATA_WIDTH/8-1:0]   m_axi_wstrb,
    output wire                      m_axi_wlast,
    output reg                       m_axi_wvalid,
    input  wire                      m_axi_wready,

    input  wire [ID_WIDTH-1:0]       m_axi_bid,
    input  wire [1:0]                m_axi_bresp,
    input  wire                      m_axi_bvalid,
    output wire                      m_axi_bready,

    output reg  [ID_WIDTH-1:0]       m_axi_arid,
    output reg  [ADDR_WIDTH-1:0]     m_axi_araddr,
    output wire [7:0]                m_axi_arlen,
    output wire [2:0]                m_axi_arsize,
    output wire [1:0]                m_axi_arburst,
    output reg  [USER_WIDTH-1:0]     m_axi_aruser,
    output reg                       m_axi_arvalid,
    input  wire                      m_axi_arready,

    input  wire [ID_WIDTH-1:0]       m_axi_rid,
    input  wire [DATA_WIDTH-1:0]     m_axi_rdata,
    input  wire [1:0]                m_axi_rresp,
    input  wire                      m_axi_rlast,
    input  wire                      m_axi_rvalid,
    output wire                      m_axi_rready
);
    localparam integer STRB_WIDTH = DATA_WIDTH / 8;
    localparam integer AXI_SIZE   = $clog2(STRB_WIDTH);

    localparam [2:0] ST_IDLE    = 3'd0;
    localparam [2:0] ST_WR_SEND = 3'd1;
    localparam [2:0] ST_WR_WAIT = 3'd2;
    localparam [2:0] ST_RD_SEND = 3'd3;
    localparam [2:0] ST_RD_WAIT = 3'd4;
    localparam [2:0] ST_RETURN  = 3'd5;

    reg [2:0] state;
    reg [ID_WIDTH-1:0] pending_id;

    assign cmd_ready      = (state == ST_IDLE);
    assign m_axi_awlen    = 8'd0;
    assign m_axi_awsize   = AXI_SIZE[2:0];
    assign m_axi_awburst  = 2'b01;
    assign m_axi_wlast    = 1'b1;
    assign m_axi_bready   = (state == ST_WR_WAIT) && !rsp_valid;
    assign m_axi_arlen    = 8'd0;
    assign m_axi_arsize   = AXI_SIZE[2:0];
    assign m_axi_arburst  = 2'b01;
    assign m_axi_rready   = (state == ST_RD_WAIT) && !rsp_valid;

    always @(posedge clk) begin
        if (!rst_n) begin
            state         <= ST_IDLE;
            pending_id    <= {ID_WIDTH{1'b0}};
            rsp_valid     <= 1'b0;
            rsp_write     <= 1'b0;
            rsp_rdata     <= {DATA_WIDTH{1'b0}};
            rsp_resp      <= 2'b00;
            rsp_id        <= {ID_WIDTH{1'b0}};
            m_axi_awid    <= {ID_WIDTH{1'b0}};
            m_axi_awaddr  <= {ADDR_WIDTH{1'b0}};
            m_axi_awuser  <= {USER_WIDTH{1'b0}};
            m_axi_awvalid <= 1'b0;
            m_axi_wdata   <= {DATA_WIDTH{1'b0}};
            m_axi_wstrb   <= {STRB_WIDTH{1'b0}};
            m_axi_wvalid  <= 1'b0;
            m_axi_arid    <= {ID_WIDTH{1'b0}};
            m_axi_araddr  <= {ADDR_WIDTH{1'b0}};
            m_axi_aruser  <= {USER_WIDTH{1'b0}};
            m_axi_arvalid <= 1'b0;
        end else begin
            case (state)
                ST_IDLE: begin
                    if (cmd_valid && cmd_ready) begin
                        pending_id <= cmd_id;
                        if (cmd_write) begin
                            m_axi_awid    <= cmd_id;
                            m_axi_awaddr  <= cmd_addr;
                            m_axi_awuser  <= cmd_src;
                            m_axi_awvalid <= 1'b1;
                            m_axi_wdata   <= cmd_wdata;
                            m_axi_wstrb   <= cmd_wstrb;
                            m_axi_wvalid  <= 1'b1;
                            state         <= ST_WR_SEND;
                        end else begin
                            m_axi_arid    <= cmd_id;
                            m_axi_araddr  <= cmd_addr;
                            m_axi_aruser  <= cmd_src;
                            m_axi_arvalid <= 1'b1;
                            state         <= ST_RD_SEND;
                        end
                    end
                end

                ST_WR_SEND: begin
                    if (m_axi_awvalid && m_axi_awready)
                        m_axi_awvalid <= 1'b0;
                    if (m_axi_wvalid && m_axi_wready)
                        m_axi_wvalid <= 1'b0;
                    if ((!m_axi_awvalid || m_axi_awready) &&
                        (!m_axi_wvalid || m_axi_wready))
                        state <= ST_WR_WAIT;
                end

                ST_WR_WAIT: begin
                    if (m_axi_bvalid && m_axi_bready) begin
                        if (m_axi_bid != pending_id)
                            $fatal(1, "XPU AXI master: BID mismatch");
                        rsp_write <= 1'b1;
                        rsp_rdata <= {DATA_WIDTH{1'b0}};
                        rsp_resp  <= m_axi_bresp;
                        rsp_id    <= m_axi_bid;
                        rsp_valid <= 1'b1;
                        state     <= ST_RETURN;
                    end
                end

                ST_RD_SEND: begin
                    if (m_axi_arvalid && m_axi_arready) begin
                        m_axi_arvalid <= 1'b0;
                        state         <= ST_RD_WAIT;
                    end
                end

                ST_RD_WAIT: begin
                    if (m_axi_rvalid && m_axi_rready) begin
                        if (m_axi_rid != pending_id || !m_axi_rlast)
                            $fatal(1, "XPU AXI master: RID/RLAST mismatch");
                        rsp_write <= 1'b0;
                        rsp_rdata <= m_axi_rdata;
                        rsp_resp  <= m_axi_rresp;
                        rsp_id    <= m_axi_rid;
                        rsp_valid <= 1'b1;
                        state     <= ST_RETURN;
                    end
                end

                ST_RETURN: begin
                    if (rsp_valid && rsp_ready) begin
                        rsp_valid <= 1'b0;
                        state     <= ST_IDLE;
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end
endmodule
