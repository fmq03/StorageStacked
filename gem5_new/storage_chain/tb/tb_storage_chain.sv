`timescale 1ns/1ps

module tb_storage_chain;
    localparam integer ADDR_WIDTH = 32;
    localparam integer DATA_WIDTH = 64;
    localparam integer ID_WIDTH   = 4;
    localparam integer USER_WIDTH = 2;
    localparam [ADDR_WIDTH-1:0] MEM_BASE = 32'h9000_0000;

    localparam [USER_WIDTH-1:0] SRC_HOST   = 2'd0;
    localparam [USER_WIDTH-1:0] SRC_VORTEX = 2'd1;
    localparam [USER_WIDTH-1:0] SRC_NPU    = 2'd2;

    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;

    reg cmd_valid;
    wire cmd_ready;
    reg cmd_write;
    reg [ADDR_WIDTH-1:0] cmd_addr;
    reg [DATA_WIDTH-1:0] cmd_wdata;
    reg [DATA_WIDTH/8-1:0] cmd_wstrb;
    reg [ID_WIDTH-1:0] cmd_id;
    reg [USER_WIDTH-1:0] cmd_src;
    wire rsp_valid;
    reg rsp_ready;
    wire rsp_write;
    wire [DATA_WIDTH-1:0] rsp_rdata;
    wire [1:0] rsp_resp;
    wire [ID_WIDTH-1:0] rsp_id;

    wire [ID_WIDTH-1:0] s_awid;
    wire [ADDR_WIDTH-1:0] s_awaddr;
    wire [7:0] s_awlen;
    wire [2:0] s_awsize;
    wire [1:0] s_awburst;
    wire [USER_WIDTH-1:0] s_awuser;
    wire s_awvalid;
    wire s_awready;
    wire [DATA_WIDTH-1:0] s_wdata;
    wire [DATA_WIDTH/8-1:0] s_wstrb;
    wire s_wlast;
    wire s_wvalid;
    wire s_wready;
    wire [ID_WIDTH-1:0] s_bid;
    wire [1:0] s_bresp;
    wire s_bvalid;
    wire s_bready;
    wire [ID_WIDTH-1:0] s_arid;
    wire [ADDR_WIDTH-1:0] s_araddr;
    wire [7:0] s_arlen;
    wire [2:0] s_arsize;
    wire [1:0] s_arburst;
    wire [USER_WIDTH-1:0] s_aruser;
    wire s_arvalid;
    wire s_arready;
    wire [ID_WIDTH-1:0] s_rid;
    wire [DATA_WIDTH-1:0] s_rdata;
    wire [1:0] s_rresp;
    wire s_rlast;
    wire s_rvalid;
    wire s_rready;

    wire [ID_WIDTH-1:0] m_awid;
    wire [ADDR_WIDTH-1:0] m_awaddr;
    wire [7:0] m_awlen;
    wire [2:0] m_awsize;
    wire [1:0] m_awburst;
    wire [USER_WIDTH-1:0] m_awuser;
    wire m_awvalid;
    reg m_awready;
    wire [DATA_WIDTH-1:0] m_wdata;
    wire [DATA_WIDTH/8-1:0] m_wstrb;
    wire m_wlast;
    wire m_wvalid;
    reg m_wready;
    reg [ID_WIDTH-1:0] m_bid;
    reg [1:0] m_bresp;
    reg m_bvalid;
    wire m_bready;
    wire [ID_WIDTH-1:0] m_arid;
    wire [ADDR_WIDTH-1:0] m_araddr;
    wire [7:0] m_arlen;
    wire [2:0] m_arsize;
    wire [1:0] m_arburst;
    wire [USER_WIDTH-1:0] m_aruser;
    wire m_arvalid;
    reg m_arready;
    reg [ID_WIDTH-1:0] m_rid;
    reg [DATA_WIDTH-1:0] m_rdata;
    reg [1:0] m_rresp;
    reg m_rlast;
    reg m_rvalid;
    wire m_rready;

    wire trace_valid;
    wire [63:0] trace_tick;
    wire [63:0] trace_addr;
    wire [63:0] trace_strb;
    wire [31:0] trace_size;
    wire [31:0] trace_ctx;
    wire [31:0] trace_seq;
    wire [31:0] trace_txn;
    wire [15:0] trace_src_id;
    wire [15:0] trace_axi_id;
    wire trace_op;
    wire [2:0] trace_chan;
    wire [7:0] trace_axi_len;
    wire [2:0] trace_axi_size;
    wire [1:0] trace_burst;
    wire [1:0] trace_resp;
    wire [7:0] trace_user;
    wire [7:0] trace_flags;
    wire [31:0] trace_event_count;
    wire [31:0] trace_transaction_count;

    xpu_axi_master #(
        .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH(ID_WIDTH), .USER_WIDTH(USER_WIDTH)
    ) xpu (
        .clk(clk), .rst_n(rst_n),
        .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .cmd_write(cmd_write), .cmd_addr(cmd_addr),
        .cmd_wdata(cmd_wdata), .cmd_wstrb(cmd_wstrb),
        .cmd_id(cmd_id), .cmd_src(cmd_src),
        .rsp_valid(rsp_valid), .rsp_ready(rsp_ready),
        .rsp_write(rsp_write), .rsp_rdata(rsp_rdata),
        .rsp_resp(rsp_resp), .rsp_id(rsp_id),
        .m_axi_awid(s_awid), .m_axi_awaddr(s_awaddr),
        .m_axi_awlen(s_awlen), .m_axi_awsize(s_awsize),
        .m_axi_awburst(s_awburst), .m_axi_awuser(s_awuser),
        .m_axi_awvalid(s_awvalid), .m_axi_awready(s_awready),
        .m_axi_wdata(s_wdata), .m_axi_wstrb(s_wstrb),
        .m_axi_wlast(s_wlast), .m_axi_wvalid(s_wvalid),
        .m_axi_wready(s_wready),
        .m_axi_bid(s_bid), .m_axi_bresp(s_bresp),
        .m_axi_bvalid(s_bvalid), .m_axi_bready(s_bready),
        .m_axi_arid(s_arid), .m_axi_araddr(s_araddr),
        .m_axi_arlen(s_arlen), .m_axi_arsize(s_arsize),
        .m_axi_arburst(s_arburst), .m_axi_aruser(s_aruser),
        .m_axi_arvalid(s_arvalid), .m_axi_arready(s_arready),
        .m_axi_rid(s_rid), .m_axi_rdata(s_rdata),
        .m_axi_rresp(s_rresp), .m_axi_rlast(s_rlast),
        .m_axi_rvalid(s_rvalid), .m_axi_rready(s_rready)
    );

    storage_chain_top #(
        .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH(ID_WIDTH), .USER_WIDTH(USER_WIDTH),
        .TICKS_PER_CYCLE(64'd1000)
    ) dut (
        .clk(clk), .rst_n(rst_n),
        .s_axi_awid(s_awid), .s_axi_awaddr(s_awaddr),
        .s_axi_awlen(s_awlen), .s_axi_awsize(s_awsize),
        .s_axi_awburst(s_awburst), .s_axi_awuser(s_awuser),
        .s_axi_awvalid(s_awvalid), .s_axi_awready(s_awready),
        .s_axi_wdata(s_wdata), .s_axi_wstrb(s_wstrb),
        .s_axi_wlast(s_wlast), .s_axi_wvalid(s_wvalid),
        .s_axi_wready(s_wready),
        .s_axi_bid(s_bid), .s_axi_bresp(s_bresp),
        .s_axi_bvalid(s_bvalid), .s_axi_bready(s_bready),
        .s_axi_arid(s_arid), .s_axi_araddr(s_araddr),
        .s_axi_arlen(s_arlen), .s_axi_arsize(s_arsize),
        .s_axi_arburst(s_arburst), .s_axi_aruser(s_aruser),
        .s_axi_arvalid(s_arvalid), .s_axi_arready(s_arready),
        .s_axi_rid(s_rid), .s_axi_rdata(s_rdata),
        .s_axi_rresp(s_rresp), .s_axi_rlast(s_rlast),
        .s_axi_rvalid(s_rvalid), .s_axi_rready(s_rready),
        .m_axi_awid(m_awid), .m_axi_awaddr(m_awaddr),
        .m_axi_awlen(m_awlen), .m_axi_awsize(m_awsize),
        .m_axi_awburst(m_awburst), .m_axi_awuser(m_awuser),
        .m_axi_awvalid(m_awvalid), .m_axi_awready(m_awready),
        .m_axi_wdata(m_wdata), .m_axi_wstrb(m_wstrb),
        .m_axi_wlast(m_wlast), .m_axi_wvalid(m_wvalid),
        .m_axi_wready(m_wready),
        .m_axi_bid(m_bid), .m_axi_bresp(m_bresp),
        .m_axi_bvalid(m_bvalid), .m_axi_bready(m_bready),
        .m_axi_arid(m_arid), .m_axi_araddr(m_araddr),
        .m_axi_arlen(m_arlen), .m_axi_arsize(m_arsize),
        .m_axi_arburst(m_arburst), .m_axi_aruser(m_aruser),
        .m_axi_arvalid(m_arvalid), .m_axi_arready(m_arready),
        .m_axi_rid(m_rid), .m_axi_rdata(m_rdata),
        .m_axi_rresp(m_rresp), .m_axi_rlast(m_rlast),
        .m_axi_rvalid(m_rvalid), .m_axi_rready(m_rready),
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

    // Downstream functional BFM.  It intentionally stalls AW/W/AR so the
    // transparent boundary and checker see real backpressure; its delays are
    // test mechanics, not claimed memory timing.
    reg [7:0] memory [0:255];
    reg write_address_valid;
    reg [ADDR_WIDTH-1:0] write_address;
    reg [ID_WIDTH-1:0] write_id;
    reg read_pending;
    reg [ADDR_WIDTH-1:0] read_address;
    reg [ID_WIDTH-1:0] read_id;
    integer b_delay;
    integer r_delay;
    integer cycle_count;
    integer i;
    integer offset;
    reg saw_stall;

    always @(posedge clk) begin
        if (!rst_n) begin
            m_awready <= 1'b0;
            m_wready <= 1'b0;
            m_bvalid <= 1'b0;
            m_bid <= {ID_WIDTH{1'b0}};
            m_bresp <= 2'b00;
            m_arready <= 1'b0;
            m_rvalid <= 1'b0;
            m_rid <= {ID_WIDTH{1'b0}};
            m_rdata <= {DATA_WIDTH{1'b0}};
            m_rresp <= 2'b00;
            m_rlast <= 1'b1;
            write_address_valid <= 1'b0;
            write_address <= {ADDR_WIDTH{1'b0}};
            write_id <= {ID_WIDTH{1'b0}};
            read_pending <= 1'b0;
            read_address <= {ADDR_WIDTH{1'b0}};
            read_id <= {ID_WIDTH{1'b0}};
            b_delay <= 0;
            r_delay <= 0;
            cycle_count <= 0;
            saw_stall <= 1'b0;
            for (i = 0; i < 256; i = i + 1)
                memory[i] = 8'd0;
        end else begin
            cycle_count <= cycle_count + 1;
            m_awready <= !write_address_valid && !m_bvalid &&
                         (cycle_count[1:0] != 2'b00);
            m_wready <= write_address_valid && !m_bvalid && cycle_count[0];
            m_arready <= !read_pending && !m_rvalid &&
                         (cycle_count[1:0] == 2'b10);

            if ((m_awvalid && !m_awready) || (m_wvalid && !m_wready) ||
                (m_arvalid && !m_arready))
                saw_stall <= 1'b1;

            if (m_awvalid && m_awready) begin
                if (m_awlen != 0 || m_awsize != 3 || m_awburst != 2'b01 ||
                    m_awuser != cmd_src)
                    $fatal(1, "downstream BFM received unsupported AW");
                write_address_valid <= 1'b1;
                write_address <= m_awaddr;
                write_id <= m_awid;
                m_awready <= 1'b0;
            end
            if (m_wvalid && m_wready) begin
                if (!write_address_valid || !m_wlast)
                    $fatal(1, "downstream BFM received W without AW/WLAST");
                offset = write_address - MEM_BASE;
                if (offset < 0 || offset + DATA_WIDTH/8 > 256)
                    $fatal(1, "downstream BFM write out of range");
                for (i = 0; i < DATA_WIDTH/8; i = i + 1)
                    if (m_wstrb[i])
                        memory[offset + i] <= m_wdata[i*8 +: 8];
                m_bid <= write_id;
                m_bresp <= 2'b00;
                b_delay <= 3;
                write_address_valid <= 1'b0;
                m_wready <= 1'b0;
            end
            if (b_delay > 0) begin
                b_delay <= b_delay - 1;
                if (b_delay == 1)
                    m_bvalid <= 1'b1;
            end
            if (m_bvalid && m_bready)
                m_bvalid <= 1'b0;

            if (m_arvalid && m_arready) begin
                if (m_arlen != 0 || m_arsize != 3 || m_arburst != 2'b01 ||
                    m_aruser != cmd_src)
                    $fatal(1, "downstream BFM received unsupported AR");
                read_pending <= 1'b1;
                read_address <= m_araddr;
                read_id <= m_arid;
                r_delay <= 4;
                m_arready <= 1'b0;
            end
            if (r_delay > 0) begin
                r_delay <= r_delay - 1;
                if (r_delay == 1) begin
                    offset = read_address - MEM_BASE;
                    if (offset < 0 || offset + DATA_WIDTH/8 > 256)
                        $fatal(1, "downstream BFM read out of range");
                    for (i = 0; i < DATA_WIDTH/8; i = i + 1)
                        m_rdata[i*8 +: 8] <= memory[offset + i];
                    m_rid <= read_id;
                    m_rresp <= 2'b00;
                    m_rlast <= 1'b1;
                    m_rvalid <= 1'b1;
                end
            end
            if (m_rvalid && m_rready) begin
                m_rvalid <= 1'b0;
                read_pending <= 1'b0;
            end
        end
    end

    integer channel_count [0:4];
    integer source_count [0:2];
    integer expected_seq [0:2];
    reg [63:0] last_tick;
    reg write_trace_active [0:2];
    reg read_trace_active [0:2];
    reg [31:0] write_trace_txn [0:2];
    reg [31:0] read_trace_txn [0:2];
    integer source_index;

    always @(negedge clk) begin
        if (!rst_n) begin
            last_tick = 0;
            for (source_index = 0; source_index < 3;
                 source_index = source_index + 1) begin
                source_count[source_index] = 0;
                expected_seq[source_index] = 0;
                write_trace_active[source_index] = 1'b0;
                read_trace_active[source_index] = 1'b0;
                write_trace_txn[source_index] = 0;
                read_trace_txn[source_index] = 0;
            end
            for (source_index = 0; source_index < 5;
                 source_index = source_index + 1)
                channel_count[source_index] = 0;
        end else if (trace_valid) begin
            source_index = int'(trace_src_id);
            if (source_index < 0 || source_index > 2)
                $fatal(1, "trace source out of range");
            if (trace_tick <= last_tick || trace_tick % 1000 != 0)
                $fatal(1, "trace tick is not strictly monotonic/aligned");
            if (trace_seq != expected_seq[source_index])
                $fatal(1, "trace sequence mismatch for source %0d", source_index);
            if (trace_user != trace_src_id[7:0] ||
                trace_axi_id != trace_ctx[15:0])
                $fatal(1, "trace source/ID metadata mismatch");
            if (trace_axi_len != 0 || trace_axi_size != 3 ||
                trace_burst != 2'b01)
                $fatal(1, "trace AXI shape mismatch");
            if (trace_addr < {32'd0, MEM_BASE} ||
                trace_addr >= {32'd0, MEM_BASE} + 64'd256)
                $fatal(1, "trace address out of test range");

            case (trace_chan)
                3'd0: begin
                    if (!trace_op || trace_size != 8 || trace_strb != 0 ||
                        write_trace_active[source_index])
                        $fatal(1, "bad AW trace event");
                    write_trace_active[source_index] = 1'b1;
                    write_trace_txn[source_index] = trace_txn;
                end
                3'd1: begin
                    if (!trace_op || trace_size != 8 ||
                        !write_trace_active[source_index] ||
                        trace_txn != write_trace_txn[source_index] ||
                        ((trace_flags & 8'h20) == 0))
                        $fatal(1, "bad W trace event");
                    if ((source_index == int'(SRC_NPU) &&
                         trace_strb != 64'h0f) ||
                        (source_index != int'(SRC_NPU) &&
                         trace_strb != 64'hff))
                        $fatal(1, "WSTRB not preserved in trace event");
                end
                3'd2: begin
                    if (!trace_op || trace_size != 0 || trace_resp != 0 ||
                        !write_trace_active[source_index] ||
                        trace_txn != write_trace_txn[source_index] ||
                        ((trace_flags & 8'h20) == 0))
                        $fatal(1, "bad B trace event");
                    write_trace_active[source_index] = 1'b0;
                end
                3'd3: begin
                    if (trace_op || trace_size != 8 || trace_strb != 0 ||
                        read_trace_active[source_index])
                        $fatal(1, "bad AR trace event");
                    read_trace_active[source_index] = 1'b1;
                    read_trace_txn[source_index] = trace_txn;
                end
                3'd4: begin
                    if (trace_op || trace_size != 8 || trace_resp != 0 ||
                        !read_trace_active[source_index] ||
                        trace_txn != read_trace_txn[source_index] ||
                        ((trace_flags & 8'h20) == 0))
                        $fatal(1, "bad R trace event");
                    read_trace_active[source_index] = 1'b0;
                end
                default: $fatal(1, "unknown trace channel");
            endcase
            last_tick = trace_tick;
            expected_seq[source_index] = expected_seq[source_index] + 1;
            source_count[source_index] = source_count[source_index] + 1;
            channel_count[trace_chan] = channel_count[trace_chan] + 1;
        end
    end

    task automatic issue_command;
        input write;
        input [USER_WIDTH-1:0] source;
        input [ID_WIDTH-1:0] id;
        input [ADDR_WIDTH-1:0] address;
        input [DATA_WIDTH-1:0] data;
        input [DATA_WIDTH/8-1:0] strobe;
        input [DATA_WIDTH-1:0] expected_read;
        begin
            @(negedge clk);
            cmd_write = write;
            cmd_src = source;
            cmd_id = id;
            cmd_addr = address;
            cmd_wdata = data;
            cmd_wstrb = strobe;
            cmd_valid = 1'b1;
            while (!cmd_ready)
                @(negedge clk);
            @(negedge clk);
            cmd_valid = 1'b0;
            while (!rsp_valid)
                @(negedge clk);
            if (rsp_write !== write || rsp_id !== id || rsp_resp !== 2'b00)
                $fatal(1, "XPU response metadata mismatch");
            if (!write && rsp_rdata !== expected_read)
                $fatal(1, "read data mismatch: got %h expected %h",
                       rsp_rdata, expected_read);
            repeat (2) @(negedge clk);
            rsp_ready = 1'b1;
            @(negedge clk);
            rsp_ready = 1'b0;
        end
    endtask

    initial begin
        cmd_valid = 1'b0;
        cmd_write = 1'b0;
        cmd_addr = 0;
        cmd_wdata = 0;
        cmd_wstrb = 0;
        cmd_id = 0;
        cmd_src = 0;
        rsp_ready = 1'b0;

        repeat (4) @(posedge clk);
        rst_n = 1'b1;

        issue_command(1'b1, SRC_HOST, 4'h1, MEM_BASE,
                      64'hdead_beef_cafe_babe, 8'hff, 64'd0);
        issue_command(1'b1, SRC_NPU, 4'h2, MEM_BASE + 8,
                      64'h1122_3344_5566_7788, 8'h0f, 64'd0);
        issue_command(1'b0, SRC_HOST, 4'h3, MEM_BASE,
                      64'd0, 8'd0, 64'hdead_beef_cafe_babe);
        issue_command(1'b0, SRC_NPU, 4'h4, MEM_BASE + 8,
                      64'd0, 8'd0, 64'h0000_0000_5566_7788);
        issue_command(1'b1, SRC_VORTEX, 4'h5, MEM_BASE + 16,
                      64'h0123_4567_89ab_cdef, 8'hff, 64'd0);
        issue_command(1'b0, SRC_VORTEX, 4'h6, MEM_BASE + 16,
                      64'd0, 8'd0, 64'h0123_4567_89ab_cdef);

        repeat (3) @(negedge clk);
        if (!saw_stall)
            $fatal(1, "test never exercised AXI backpressure");
        if (trace_event_count != 15 || trace_transaction_count != 6)
            $fatal(1, "trace totals mismatch events=%0d txns=%0d",
                   trace_event_count, trace_transaction_count);
        for (i = 0; i < 5; i = i + 1)
            if (channel_count[i] != 3)
                $fatal(1, "channel %0d count=%0d expected 3", i,
                       channel_count[i]);
        for (i = 0; i < 3; i = i + 1)
            if (source_count[i] != 5 || expected_seq[i] != 5 ||
                write_trace_active[i] || read_trace_active[i])
                $fatal(1, "source %0d trace accounting mismatch", i);

        $display("AXI TRACE PASS: 6 transactions, 15 AW/W/B/AR/R events, 3 sources");
        $display("PASS boundary is transparent; WSTRB, ID, USER, RESP and backpressure preserved");
        $finish;
    end

    initial begin
        #100000;
        $fatal(1, "timeout");
    end
endmodule
