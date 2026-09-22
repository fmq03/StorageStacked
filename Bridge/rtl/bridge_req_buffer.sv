module bridge_req_buffer #(
    parameter int DATA_W = 256,
    parameter int RP_COUNT = 1,
    parameter int REQUEST_FIFO_DEPTH = 4,
    parameter int WDATA_FIFO_DEPTH = 16
) (
    input  logic                clk,
    input  logic                rst_n,
    input  logic                req_valid,
    output logic                req_ready,
    input  logic                req_write,
    input  logic [1:0]          req_rp,
    input  logic [15:0]         req_user,
    input  logic [9:0]          req_id,
    input  logic [2:0]          req_size,
    input  logic [7:0]          req_len,
    input  logic [3:0]          req_qos,
    input  logic [63:0]         req_addr,
    input  logic [5:0]          req_granules,
    input  logic                req_pop,
    input  logic [1:0]          req_pop_rp,
    output logic                req_head_write,
    output logic [15:0]         req_head_user,
    output logic [9:0]          req_head_id,
    output logic [2:0]          req_head_size,
    output logic [7:0]          req_head_len,
    output logic [3:0]          req_head_qos,
    output logic [63:0]         req_head_addr,
    output logic [5:0]          req_head_granules,
    output logic [$clog2(REQUEST_FIFO_DEPTH+1)-1:0]
        req_fifo_count [0:RP_COUNT-1],

    input  logic                wdata_valid,
    output logic                wdata_ready,
    input  logic [1:0]          wdata_rp,
    input  logic [15:0]         wdata_user,
    input  logic [DATA_W-1:0]   wdata_data,
    input  logic [DATA_W/8-1:0] wdata_mask,
    input  logic [5:0]          wdata_granules,
    input  logic                wdata_pop,
    input  logic [1:0]          wdata_pop_rp,
    output logic [15:0]         wdata_head_user,
    output logic [DATA_W-1:0]   wdata_head_data,
    output logic [DATA_W/8-1:0] wdata_head_mask,
    output logic [5:0]          wdata_head_granules,
    output logic [$clog2(WDATA_FIFO_DEPTH+1)-1:0]
        wdata_fifo_count [0:RP_COUNT-1]
);
    localparam int REQ_W = 1 + 16 + 10 + 3 + 8 + 4 + 64 + 6;
    localparam int WDATA_W = 16 + DATA_W + DATA_W/8 + 6;

    logic [REQ_W-1:0] req_push_data;
    logic [WDATA_W-1:0] wdata_push_data;
    logic req_push_valid [0:RP_COUNT-1];
    logic req_push_ready [0:RP_COUNT-1];
    logic req_pop_valid [0:RP_COUNT-1];
    logic req_pop_ready [0:RP_COUNT-1];
    logic [REQ_W-1:0] req_pop_data [0:RP_COUNT-1];
    logic wdata_push_valid [0:RP_COUNT-1];
    logic wdata_push_ready [0:RP_COUNT-1];
    logic wdata_pop_valid [0:RP_COUNT-1];
    logic wdata_pop_ready [0:RP_COUNT-1];
    logic [WDATA_W-1:0] wdata_pop_data [0:RP_COUNT-1];
    logic [REQ_W-1:0] req_selected;
    logic [WDATA_W-1:0] wdata_selected;

    assign req_push_data = {req_write, req_user, req_id, req_size, req_len,
                            req_qos, req_addr, req_granules};
    assign wdata_push_data =
        {wdata_user, wdata_data, wdata_mask, wdata_granules};
    assign {req_head_write, req_head_user, req_head_id, req_head_size,
            req_head_len, req_head_qos, req_head_addr, req_head_granules} =
        req_selected;
    assign {wdata_head_user, wdata_head_data, wdata_head_mask,
            wdata_head_granules} = wdata_selected;

    always_comb begin
        req_ready = 1'b0;
        wdata_ready = 1'b0;
        req_selected = '0;
        wdata_selected = '0;
        for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
            if (int'(req_rp) == rp)
                req_ready = req_push_ready[rp];
            if (int'(req_pop_rp) == rp)
                req_selected = req_pop_data[rp];
            if (int'(wdata_rp) == rp)
                wdata_ready = wdata_push_ready[rp];
            if (int'(wdata_pop_rp) == rp)
                wdata_selected = wdata_pop_data[rp];
        end
    end

    for (genvar rp = 0; rp < RP_COUNT; rp = rp + 1) begin : g_rp_fifo
        assign req_push_valid[rp] = req_valid && int'(req_rp) == rp;
        assign req_pop_ready[rp] =
            req_pop && int'(req_pop_rp) == rp && req_pop_valid[rp];
        bridge_fifo #(.WIDTH(REQ_W), .DEPTH(REQUEST_FIFO_DEPTH)) req_fifo (
            .clk, .rst_n, .push_valid(req_push_valid[rp]),
            .push_ready(req_push_ready[rp]), .push_data(req_push_data),
            .pop_valid(req_pop_valid[rp]), .pop_ready(req_pop_ready[rp]),
            .pop_data(req_pop_data[rp]), .count(req_fifo_count[rp])
        );

        assign wdata_push_valid[rp] =
            wdata_valid && int'(wdata_rp) == rp;
        assign wdata_pop_ready[rp] =
            wdata_pop && int'(wdata_pop_rp) == rp && wdata_pop_valid[rp];
        bridge_fifo #(.WIDTH(WDATA_W), .DEPTH(WDATA_FIFO_DEPTH)) wdata_fifo (
            .clk, .rst_n, .push_valid(wdata_push_valid[rp]),
            .push_ready(wdata_push_ready[rp]), .push_data(wdata_push_data),
            .pop_valid(wdata_pop_valid[rp]), .pop_ready(wdata_pop_ready[rp]),
            .pop_data(wdata_pop_data[rp]), .count(wdata_fifo_count[rp])
        );
    end

endmodule
