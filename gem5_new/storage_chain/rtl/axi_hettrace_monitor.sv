`timescale 1ns/1ps

// Passive AXI4-to-HETTrace event monitor for the reference RTL boundary.
//
// The module has no READY/VALID outputs and therefore cannot backpressure or
// otherwise change the observed AXI traffic.  Each accepted AXI channel beat
// is exposed with the field semantics of HETTrace v2.  File I/O, address-map
// filtering, and conversion to the external mem_sim format belong to the
// software writer/tooling; this synthesizable block is only the event boundary.
//
// The surrounding reference adapter is deliberately single-outstanding and
// permits at most one channel handshake per cycle.  Keeping that restriction
// explicit lets this compact interface expose one record per cycle without a
// hidden queue that could drop simultaneous channel events.

module axi_hettrace_monitor #(
    parameter integer ADDR_WIDTH      = 32,
    parameter integer DATA_WIDTH      = 64,
    parameter integer ID_WIDTH        = 4,
    parameter integer USER_WIDTH      = 2,
    parameter logic [63:0] TICKS_PER_CYCLE = 64'd10000
) (
    input  wire                      clk,
    input  wire                      rst_n,

    input  wire [ID_WIDTH-1:0]       axi_awid,
    input  wire [ADDR_WIDTH-1:0]     axi_awaddr,
    input  wire [7:0]                axi_awlen,
    input  wire [2:0]                axi_awsize,
    input  wire [1:0]                axi_awburst,
    input  wire [USER_WIDTH-1:0]     axi_awuser,
    input  wire                      axi_awvalid,
    input  wire                      axi_awready,

    input  wire [DATA_WIDTH-1:0]     axi_wdata,
    input  wire [DATA_WIDTH/8-1:0]   axi_wstrb,
    input  wire                      axi_wlast,
    input  wire                      axi_wvalid,
    input  wire                      axi_wready,

    input  wire [ID_WIDTH-1:0]       axi_bid,
    input  wire [1:0]                axi_bresp,
    input  wire                      axi_bvalid,
    input  wire                      axi_bready,

    input  wire [ID_WIDTH-1:0]       axi_arid,
    input  wire [ADDR_WIDTH-1:0]     axi_araddr,
    input  wire [7:0]                axi_arlen,
    input  wire [2:0]                axi_arsize,
    input  wire [1:0]                axi_arburst,
    input  wire [USER_WIDTH-1:0]     axi_aruser,
    input  wire                      axi_arvalid,
    input  wire                      axi_arready,

    input  wire [ID_WIDTH-1:0]       axi_rid,
    input  wire [DATA_WIDTH-1:0]     axi_rdata,
    input  wire [1:0]                axi_rresp,
    input  wire                      axi_rlast,
    input  wire                      axi_rvalid,
    input  wire                      axi_rready,

    output reg                       trace_valid,
    output reg  [63:0]               trace_tick,
    output reg  [63:0]               trace_addr,
    output reg  [63:0]               trace_strb,
    output reg  [31:0]               trace_size,
    output reg  [31:0]               trace_ctx,
    output reg  [31:0]               trace_seq,
    output reg  [31:0]               trace_txn,
    output reg  [15:0]               trace_src_id,
    output reg  [15:0]               trace_axi_id,
    output reg                       trace_op,
    output reg  [2:0]                trace_chan,
    output reg  [7:0]                trace_axi_len,
    output reg  [2:0]                trace_axi_size,
    output reg  [1:0]                trace_burst,
    output reg  [1:0]                trace_resp,
    output reg  [7:0]                trace_user,
    output reg  [7:0]                trace_flags,

    output reg  [31:0]               trace_event_count,
    output reg  [31:0]               trace_transaction_count
);
    localparam integer SRC_COUNT = 1 << USER_WIDTH;

    localparam [2:0] CHAN_AW = 3'd0;
    localparam [2:0] CHAN_W  = 3'd1;
    localparam [2:0] CHAN_B  = 3'd2;
    localparam [2:0] CHAN_AR = 3'd3;
    localparam [2:0] CHAN_R  = 3'd4;

    localparam [7:0] FLAG_BURST_BEAT = 8'h01;
    localparam [7:0] FLAG_LAST       = 8'h20;

    wire aw_fire = axi_awvalid && axi_awready;
    wire w_fire  = axi_wvalid  && axi_wready;
    wire b_fire  = axi_bvalid  && axi_bready;
    wire ar_fire = axi_arvalid && axi_arready;
    wire r_fire  = axi_rvalid  && axi_rready;
    wire [2:0] fire_count = {2'b0, aw_fire} + {2'b0, w_fire} +
                            {2'b0, b_fire}  + {2'b0, ar_fire} +
                            {2'b0, r_fire};

    reg [63:0] global_tick;
    reg [31:0] next_seq [0:SRC_COUNT-1];
    reg [31:0] next_txn [0:SRC_COUNT-1];

    reg write_active;
    reg [63:0] write_addr;
    reg [ID_WIDTH-1:0] write_id;
    reg [USER_WIDTH-1:0] write_src;
    reg [31:0] write_txn;
    reg [7:0] write_len;
    reg [2:0] write_size;
    reg [1:0] write_burst;
    reg [7:0] write_beat;

    reg read_active;
    reg [63:0] read_addr;
    reg [ID_WIDTH-1:0] read_id;
    reg [USER_WIDTH-1:0] read_src;
    reg [31:0] read_txn;
    reg [7:0] read_len;
    reg [2:0] read_size;
    reg [1:0] read_burst;
    reg [7:0] read_beat;

    integer i;

    task automatic set_common;
        input [USER_WIDTH-1:0] src;
        input [ID_WIDTH-1:0] id;
        input [31:0] txn;
        input op;
        input [2:0] chan;
        begin
            trace_valid  <= 1'b1;
            trace_tick   <= global_tick;
            trace_ctx    <= {{(32-ID_WIDTH){1'b0}}, id};
            trace_seq    <= next_seq[src];
            trace_txn    <= txn;
            trace_src_id <= {{(16-USER_WIDTH){1'b0}}, src};
            trace_axi_id <= {{(16-ID_WIDTH){1'b0}}, id};
            trace_op     <= op;
            trace_chan   <= chan;
            trace_user   <= {{(8-USER_WIDTH){1'b0}}, src};
            next_seq[src] <= next_seq[src] + 1'b1;
            trace_event_count <= trace_event_count + 1'b1;
        end
    endtask

    initial begin
        if (DATA_WIDTH % 8 != 0 || DATA_WIDTH > 512)
            $fatal(1, "HETTrace monitor requires a byte-wide bus no wider than 512 bits");
        if (USER_WIDTH > 8)
            $fatal(1, "HETTrace v2 USER field is limited to 8 bits");
        if (ID_WIDTH > 16)
            $fatal(1, "HETTrace v2 AXI ID field is limited to 16 bits");
        if (ADDR_WIDTH > 64)
            $fatal(1, "HETTrace v2 address field is limited to 64 bits");
        if (TICKS_PER_CYCLE <= 0)
            $fatal(1, "TICKS_PER_CYCLE must be positive");
    end

    always @(posedge clk) begin
        if (!rst_n) begin
            trace_valid             <= 1'b0;
            trace_tick              <= 64'd0;
            trace_addr              <= 64'd0;
            trace_strb              <= 64'd0;
            trace_size              <= 32'd0;
            trace_ctx               <= 32'd0;
            trace_seq               <= 32'd0;
            trace_txn               <= 32'd0;
            trace_src_id            <= 16'd0;
            trace_axi_id            <= 16'd0;
            trace_op                <= 1'b0;
            trace_chan              <= CHAN_AW;
            trace_axi_len           <= 8'd0;
            trace_axi_size          <= 3'd0;
            trace_burst             <= 2'b01;
            trace_resp              <= 2'b00;
            trace_user              <= 8'd0;
            trace_flags             <= 8'd0;
            trace_event_count       <= 32'd0;
            trace_transaction_count <= 32'd0;
            global_tick             <= 64'd0;
            write_active            <= 1'b0;
            write_addr              <= 64'd0;
            write_id                <= {ID_WIDTH{1'b0}};
            write_src               <= {USER_WIDTH{1'b0}};
            write_txn               <= 32'd0;
            write_len               <= 8'd0;
            write_size              <= 3'd0;
            write_burst             <= 2'b01;
            write_beat              <= 8'd0;
            read_active             <= 1'b0;
            read_addr               <= 64'd0;
            read_id                 <= {ID_WIDTH{1'b0}};
            read_src                <= {USER_WIDTH{1'b0}};
            read_txn                <= 32'd0;
            read_len                <= 8'd0;
            read_size               <= 3'd0;
            read_burst              <= 2'b01;
            read_beat               <= 8'd0;
            for (i = 0; i < SRC_COUNT; i = i + 1) begin
                next_seq[i] <= 32'd0;
                next_txn[i] <= 32'd0;
            end
        end else begin
            global_tick <= global_tick + TICKS_PER_CYCLE;
            trace_valid <= 1'b0;

            if (fire_count > 1)
                $fatal(1, "reference HETTrace monitor saw simultaneous AXI handshakes");

            if (aw_fire) begin
                if (write_active)
                    $fatal(1, "HETTrace monitor saw overlapping writes");
                set_common(axi_awuser, axi_awid, next_txn[axi_awuser], 1'b1, CHAN_AW);
                trace_addr     <= {{(64-ADDR_WIDTH){1'b0}}, axi_awaddr};
                trace_strb     <= 64'd0;
                trace_size     <= ({24'd0, axi_awlen} + 32'd1) << axi_awsize;
                trace_axi_len  <= axi_awlen;
                trace_axi_size <= axi_awsize;
                trace_burst    <= axi_awburst;
                trace_resp     <= 2'b00;
                trace_flags    <= 8'd0;
                write_active   <= 1'b1;
                write_addr     <= {{(64-ADDR_WIDTH){1'b0}}, axi_awaddr};
                write_id       <= axi_awid;
                write_src      <= axi_awuser;
                write_txn      <= next_txn[axi_awuser];
                write_len      <= axi_awlen;
                write_size     <= axi_awsize;
                write_burst    <= axi_awburst;
                write_beat     <= 8'd0;
                next_txn[axi_awuser] <= next_txn[axi_awuser] + 1'b1;
                trace_transaction_count <= trace_transaction_count + 1'b1;
            end else if (w_fire) begin
                if (!write_active)
                    $fatal(1, "HETTrace monitor saw W without AW");
                if (axi_wlast != (write_beat == write_len))
                    $fatal(1, "HETTrace monitor saw WLAST inconsistent with AWLEN");
                set_common(write_src, write_id, write_txn, 1'b1, CHAN_W);
                trace_addr     <= write_addr + ({{56{1'b0}}, write_beat} << write_size);
                trace_strb     <= {{(64-DATA_WIDTH/8){1'b0}}, axi_wstrb};
                trace_size     <= 32'd1 << write_size;
                trace_axi_len  <= write_len;
                trace_axi_size <= write_size;
                trace_burst    <= write_burst;
                trace_resp     <= 2'b00;
                trace_flags    <= (write_beat != 0 ? FLAG_BURST_BEAT : 8'd0) |
                                  (axi_wlast ? FLAG_LAST : 8'd0);
                write_beat     <= write_beat + 1'b1;
            end else if (b_fire) begin
                if (!write_active || axi_bid != write_id)
                    $fatal(1, "HETTrace monitor saw B without matching AW/W");
                set_common(write_src, write_id, write_txn, 1'b1, CHAN_B);
                trace_addr     <= write_addr;
                trace_strb     <= 64'd0;
                trace_size     <= 32'd0;
                trace_axi_len  <= write_len;
                trace_axi_size <= write_size;
                trace_burst    <= write_burst;
                trace_resp     <= axi_bresp;
                trace_flags    <= FLAG_LAST;
                write_active   <= 1'b0;
            end else if (ar_fire) begin
                if (read_active)
                    $fatal(1, "HETTrace monitor saw overlapping reads");
                set_common(axi_aruser, axi_arid, next_txn[axi_aruser], 1'b0, CHAN_AR);
                trace_addr     <= {{(64-ADDR_WIDTH){1'b0}}, axi_araddr};
                trace_strb     <= 64'd0;
                trace_size     <= ({24'd0, axi_arlen} + 32'd1) << axi_arsize;
                trace_axi_len  <= axi_arlen;
                trace_axi_size <= axi_arsize;
                trace_burst    <= axi_arburst;
                trace_resp     <= 2'b00;
                trace_flags    <= 8'd0;
                read_active    <= 1'b1;
                read_addr      <= {{(64-ADDR_WIDTH){1'b0}}, axi_araddr};
                read_id        <= axi_arid;
                read_src       <= axi_aruser;
                read_txn       <= next_txn[axi_aruser];
                read_len       <= axi_arlen;
                read_size      <= axi_arsize;
                read_burst     <= axi_arburst;
                read_beat      <= 8'd0;
                next_txn[axi_aruser] <= next_txn[axi_aruser] + 1'b1;
                trace_transaction_count <= trace_transaction_count + 1'b1;
            end else if (r_fire) begin
                if (!read_active || axi_rid != read_id)
                    $fatal(1, "HETTrace monitor saw R without matching AR");
                if (axi_rlast != (read_beat == read_len))
                    $fatal(1, "HETTrace monitor saw RLAST inconsistent with ARLEN");
                set_common(read_src, read_id, read_txn, 1'b0, CHAN_R);
                trace_addr     <= read_addr + ({{56{1'b0}}, read_beat} << read_size);
                trace_strb     <= 64'd0;
                trace_size     <= 32'd1 << read_size;
                trace_axi_len  <= read_len;
                trace_axi_size <= read_size;
                trace_burst    <= read_burst;
                trace_resp     <= axi_rresp;
                trace_flags    <= (read_beat != 0 ? FLAG_BURST_BEAT : 8'd0) |
                                  (axi_rlast ? FLAG_LAST : 8'd0);
                read_beat      <= read_beat + 1'b1;
                if (axi_rlast)
                    read_active <= 1'b0;
            end
        end
    end

    // Data is intentionally absent from HETTrace v2.  Referencing the signals
    // here is unnecessary: WDATA/RDATA remain on the AXI functional path while
    // timing replay consumes only request type, address, and injection tick.

endmodule
