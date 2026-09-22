module bridge_transaction_engine #(
    parameter int DATA_W = 256,
    parameter int RP_COUNT = 1,
    parameter int MC_TAG_W = 32,
    parameter int MC_BYTES = 32,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MC_INFLIGHT = 8,
    parameter int REQUEST_FIFO_DEPTH = 4,
    parameter int WDATA_FIFO_DEPTH = 16,
    parameter int MAX_BURST_BEATS = 256
) (
    input  logic                 clk,
    input  logic                 rst_n,

    input  logic                 req_valid,
    output logic                 req_ready,
    input  logic                 req_write,
    input  logic [1:0]           req_rp,
    input  logic [15:0]          req_user,
    input  logic [9:0]           req_id,
    input  logic [2:0]           req_size,
    input  logic [7:0]           req_len,
    input  logic [3:0]           req_qos,
    input  logic [63:0]          req_addr,
    input  logic [5:0]           req_granules,

    input  logic                 wdata_valid,
    output logic                 wdata_ready,
    input  logic [1:0]           wdata_rp,
    input  logic [15:0]          wdata_user,
    input  logic [DATA_W-1:0]    wdata_data,
    input  logic [DATA_W/8-1:0]  wdata_mask,
    input  logic [5:0]           wdata_granules,
    input  logic [3:0]           rdata_credit_available,
    input  logic [3:0]           wresp_credit_available,

    output logic                 req_release0_valid,
    output logic                 req_release0_write,
    output logic [1:0]           req_release0_rp,
    output logic [5:0]           req_release0_granules,
    output logic                 req_release1_valid,
    output logic                 req_release1_write,
    output logic [1:0]           req_release1_rp,
    output logic [5:0]           req_release1_granules,
    output logic                 wdata_release_valid,
    output logic [1:0]           wdata_release_rp,
    output logic [5:0]           wdata_release_granules,

    output logic                 mc_req_valid,
    input  logic                 mc_req_ready,
    output logic                 mc_req_write,
    output logic [63:0]          mc_req_addr,
    output logic [5:0]           mc_req_bytes,
    output logic [DATA_W-1:0]    mc_req_data,
    output logic [DATA_W/8-1:0]  mc_req_byte_mask,
    output logic [MC_TAG_W-1:0]  mc_req_tag,
    output logic [3:0]           mc_req_qos,

    input  logic                 mc_rsp_valid,
    output logic                 mc_rsp_ready,
    input  logic [MC_TAG_W-1:0]  mc_rsp_tag,
    input  logic [2:0]           mc_rsp_status,
    input  logic [DATA_W-1:0]    mc_rsp_data,

    output logic                 rsp_msg_valid,
    input  logic                 rsp_msg_ready,
    output logic [5:0]           rsp_msg_granules,
    output logic [bridge_pkg::MSG_MAX_W-1:0] rsp_msg_data,
    output logic                 busy,
    output logic                 protocol_error,
    output logic [3:0]           debug_response_count,
    output logic [8:0]           debug_head_issue,
    output logic [8:0]           debug_head_done,
    output logic [8:0]           debug_head_wdata,
    output logic [7:0]           debug_head_len,
    output logic                 debug_head_write
);
    /* verilator lint_off WIDTHTRUNC */
    import bridge_pkg::*;
    localparam int DATA_BYTES = DATA_W / 8;
    localparam int PARENT_W = (MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING);
    localparam int COUNT_W = $clog2(MAX_OUTSTANDING + 1);
    localparam int CHILD_COUNT_W = $clog2(MC_INFLIGHT + 1);
    localparam int REQ_COUNT_W = $clog2(REQUEST_FIFO_DEPTH + 1);
    localparam int WDATA_COUNT_W = $clog2(WDATA_FIFO_DEPTH + 1);

    logic parent_valid [0:MAX_OUTSTANDING-1];
    logic parent_write [0:MAX_OUTSTANDING-1];
    logic parent_synthetic [0:MAX_OUTSTANDING-1];
    logic [1:0] parent_rp [0:MAX_OUTSTANDING-1];
    logic [15:0] parent_user [0:MAX_OUTSTANDING-1];
    logic [9:0] parent_id [0:MAX_OUTSTANDING-1];
    logic [2:0] parent_size [0:MAX_OUTSTANDING-1];
    logic [7:0] parent_len [0:MAX_OUTSTANDING-1];
    logic [3:0] parent_qos [0:MAX_OUTSTANDING-1];
    logic [63:0] parent_addr [0:MAX_OUTSTANDING-1];
    logic [5:0] parent_req_granules [0:MAX_OUTSTANDING-1];
    logic parent_req_released [0:MAX_OUTSTANDING-1];
    logic [8:0] parent_issue_beat [0:MAX_OUTSTANDING-1];
    logic [8:0] parent_done_beats [0:MAX_OUTSTANDING-1];
    logic [8:0] parent_wdata_beats [0:MAX_OUTSTANDING-1];
    logic [8:0] parent_emit_beat [0:MAX_OUTSTANDING-1];
    logic [2:0] parent_status [0:MAX_OUTSTANDING-1];
    logic parent_wbuf_valid [0:MAX_OUTSTANDING-1];
    logic [DATA_W-1:0] parent_wbuf_data [0:MAX_OUTSTANDING-1];
    logic [DATA_BYTES-1:0] parent_wbuf_mask [0:MAX_OUTSTANDING-1];
    logic [DATA_W-1:0] parent_read_data
        [0:MAX_OUTSTANDING-1][0:MAX_BURST_BEATS-1];
    logic parent_read_done
        [0:MAX_OUTSTANDING-1][0:MAX_BURST_BEATS-1];

    logic [PARENT_W-1:0] response_order
        [0:RP_COUNT-1][0:MAX_OUTSTANDING-1];
    logic [PARENT_W-1:0] write_order
        [0:RP_COUNT-1][0:MAX_OUTSTANDING-1];
    logic [PARENT_W-1:0] response_head [0:RP_COUNT-1];
    logic [PARENT_W-1:0] response_tail [0:RP_COUNT-1];
    logic [PARENT_W-1:0] write_head [0:RP_COUNT-1];
    logic [PARENT_W-1:0] write_tail [0:RP_COUNT-1];
    logic [COUNT_W-1:0] response_count;
    logic [COUNT_W-1:0] response_rp_count [0:RP_COUNT-1];
    logic [COUNT_W-1:0] write_count [0:RP_COUNT-1];

    logic child_valid [0:MC_INFLIGHT-1];
    logic [MC_TAG_W-1:0] child_tag [0:MC_INFLIGHT-1];
    logic [CHILD_COUNT_W-1:0] child_count;
    logic [MC_TAG_W-1:0] next_tag;
    logic [PARENT_W-1:0] complete_parent;
    logic [7:0] complete_beat;
    logic [5:0] complete_beats;
    logic [5:0] complete_bytes;

    logic [WDATA_COUNT_W-1:0] wdata_fifo_count [0:RP_COUNT-1];
    logic [REQ_COUNT_W-1:0] req_fifo_count [0:RP_COUNT-1];
    logic req_head_write;
    logic [15:0] req_head_user;
    logic [9:0] req_head_id;
    logic [2:0] req_head_size;
    logic [7:0] req_head_len;
    logic [3:0] req_head_qos;
    logic [63:0] req_head_addr;
    logic [5:0] req_head_granules;
    logic [15:0] wdata_head_user;
    logic [DATA_W-1:0] wdata_head_data;
    logic [DATA_BYTES-1:0] wdata_head_mask;
    logic [5:0] wdata_head_granules;

    logic free_parent_found;
    logic [PARENT_W-1:0] free_parent;
    logic free_child_found;
    logic issue_found;
    logic [PARENT_W-1:0] issue_parent;
    logic [PARENT_W-1:0] issue_rr;
    logic issue_out_valid;
    logic issue_out_write;
    logic [PARENT_W-1:0] issue_out_parent;
    logic [7:0] issue_out_beat;
    logic [5:0] issue_out_beats;
    logic [5:0] issue_out_bytes;
    logic [63:0] issue_out_addr;
    logic [DATA_W-1:0] issue_out_data;
    logic [DATA_BYTES-1:0] issue_out_mask;
    logic [MC_TAG_W-1:0] issue_out_tag;
    logic [3:0] issue_out_qos;
    logic rsp_match_found;
    logic response_available;
    logic [PARENT_W-1:0] response_parent;
    logic [1:0] response_rp, response_rr;
    logic write_target_available;
    logic [PARENT_W-1:0] write_target;
    logic [1:0] pair_rp, pair_rr;
    logic request_available;
    logic [1:0] request_rp, request_rr;

    logic [63:0] issue_beat_addr;
    logic [5:0] issue_transaction_bytes;
    logic [5:0] issue_transaction_beats;
    logic [DATA_W-1:0] issue_build_data;
    logic [DATA_BYTES-1:0] issue_build_mask;
    logic [63:0] read_issue_addr, write_issue_addr;
    logic [5:0] read_issue_bytes, read_issue_beats;
    logic [5:0] write_issue_bytes, write_issue_beats;
    logic [DATA_W-1:0] write_issue_data;
    logic [DATA_BYTES-1:0] write_issue_mask;
    logic [DATA_W-1:0] pair_merged_data;
    logic [DATA_BYTES-1:0] pair_merged_mask;
    logic pair_chunk_complete;
    logic [DATA_W-1:0] response_selected_data;
    logic request_pop, wdata_pair, issue_fire, mc_rsp_fire, rsp_fire;
    logic retire_parent, retire_write, push_parent, push_write;

    function automatic logic [PARENT_W-1:0] parent_next(
        input logic [PARENT_W-1:0] value
    );
        if (int'(value) == MAX_OUTSTANDING - 1) parent_next = '0;
        else parent_next = value + 1'b1;
    endfunction

    initial begin
        if (DATA_W != 256 || MC_BYTES != DATA_BYTES)
            $error("P4 engine currently requires DATA_W=256 and MC_BYTES=DATA_BYTES");
        if (MAX_OUTSTANDING < 1 || MC_INFLIGHT < 1 || REQUEST_FIFO_DEPTH < 1 ||
            WDATA_FIFO_DEPTH < 1)
            $error("outstanding and inflight depths must be positive");
        if (RP_COUNT < 1 || RP_COUNT > 4)
            $error("RP_COUNT must be 1..4");
    end

    always_comb begin
        free_parent_found = 1'b0;
        free_parent = '0;
        for (int p = 0; p < MAX_OUTSTANDING; p = p + 1) begin
            if (!free_parent_found && !parent_valid[p]) begin
                free_parent_found = 1'b1;
                free_parent = PARENT_W'(p);
            end
        end

        busy = response_count != 0 || child_count != 0 || issue_out_valid ||
               write_target_available;
        for (int rp = 0; rp < RP_COUNT; rp = rp + 1)
            busy |= req_fifo_count[rp] != 0 || write_count[rp] != 0 ||
                    wdata_fifo_count[rp] != 0;
        debug_response_count = 4'(response_count);
        debug_head_issue = response_available ? parent_issue_beat[response_parent] : '0;
        debug_head_done = response_available ? parent_done_beats[response_parent] : '0;
        debug_head_wdata = response_available ? parent_wdata_beats[response_parent] : '0;
        debug_head_len = response_available ? parent_len[response_parent] : '0;
        debug_head_write = response_available && parent_write[response_parent];

        issue_beat_addr = parent_write[issue_parent]
            ? write_issue_addr : read_issue_addr;
        issue_transaction_bytes = parent_write[issue_parent]
            ? write_issue_bytes : read_issue_bytes;
        issue_transaction_beats = parent_write[issue_parent]
            ? write_issue_beats : read_issue_beats;
        issue_build_data = parent_write[issue_parent]
            ? write_issue_data : '0;
        issue_build_mask = parent_write[issue_parent]
            ? write_issue_mask : '0;

        mc_req_valid = issue_out_valid;
        mc_req_write = issue_out_write;
        mc_req_addr = issue_out_addr;
        mc_req_bytes = issue_out_bytes;
        mc_req_data = issue_out_data;
        mc_req_byte_mask = issue_out_mask;
        mc_req_tag = issue_out_tag;
        mc_req_qos = issue_out_qos;

        mc_rsp_ready = rsp_match_found;

        response_selected_data = parent_read_data[response_parent]
            [parent_emit_beat[response_parent][7:0]];
    end

    bridge_req_buffer #(
        .DATA_W(DATA_W), .RP_COUNT(RP_COUNT),
        .REQUEST_FIFO_DEPTH(REQUEST_FIFO_DEPTH),
        .WDATA_FIFO_DEPTH(WDATA_FIFO_DEPTH)
    ) req_buffer (
        .clk, .rst_n,
        .req_valid, .req_ready, .req_write, .req_rp, .req_user, .req_id,
        .req_size, .req_len, .req_qos, .req_addr, .req_granules,
        .req_pop(request_pop), .req_pop_rp(request_rp),
        .req_head_write, .req_head_user, .req_head_id, .req_head_size,
        .req_head_len, .req_head_qos, .req_head_addr, .req_head_granules,
        .req_fifo_count,
        .wdata_valid, .wdata_ready, .wdata_rp, .wdata_user, .wdata_data,
        .wdata_mask, .wdata_granules, .wdata_pop(wdata_pair),
        .wdata_pop_rp(pair_rp), .wdata_head_user, .wdata_head_data,
        .wdata_head_mask, .wdata_head_granules, .wdata_fifo_count
    );

    bridge_read_fsm #(.MC_BYTES(MC_BYTES)) read_fsm (
        .parent_addr(parent_addr[issue_parent]),
        .parent_size(parent_size[issue_parent]),
        .parent_len(parent_len[issue_parent]),
        .parent_issue_beat(parent_issue_beat[issue_parent]),
        .issue_addr(read_issue_addr), .issue_bytes(read_issue_bytes),
        .issue_beats(read_issue_beats)
    );

    bridge_write_fsm #(.DATA_W(DATA_W), .MC_BYTES(MC_BYTES)) write_fsm (
        .issue_parent_addr(parent_addr[issue_parent]),
        .issue_parent_size(parent_size[issue_parent]),
        .issue_parent_len(parent_len[issue_parent]),
        .issue_parent_beat(parent_issue_beat[issue_parent]),
        .issue_parent_data(parent_wbuf_data[issue_parent]),
        .issue_parent_mask(parent_wbuf_mask[issue_parent]),
        .issue_addr(write_issue_addr), .issue_bytes(write_issue_bytes),
        .issue_beats(write_issue_beats), .issue_data(write_issue_data),
        .issue_mask(write_issue_mask),
        .pair_parent_addr(parent_addr[write_target]),
        .pair_parent_size(parent_size[write_target]),
        .pair_parent_len(parent_len[write_target]),
        .pair_parent_issue_beat(parent_issue_beat[write_target]),
        .pair_parent_wdata_beats(parent_wdata_beats[write_target]),
        .pair_parent_data(parent_wbuf_data[write_target]),
        .pair_parent_mask(parent_wbuf_mask[write_target]),
        .pair_beat_data(wdata_head_data), .pair_beat_mask(wdata_head_mask),
        .pair_merged_data, .pair_merged_mask, .pair_chunk_complete
    );

    bridge_outstanding #(
        .MC_TAG_W(MC_TAG_W), .MAX_OUTSTANDING(MAX_OUTSTANDING),
        .MC_INFLIGHT(MC_INFLIGHT)
    ) outstanding (
        .clk, .rst_n, .alloc_valid(issue_fire),
        .alloc_parent(issue_out_parent), .alloc_beat(issue_out_beat),
        .alloc_beats(issue_out_beats), .alloc_bytes(issue_out_bytes),
        .alloc_available(free_child_found), .alloc_tag(next_tag),
        .complete_valid(mc_rsp_valid), .complete_tag(mc_rsp_tag),
        .complete_match(rsp_match_found), .complete_parent,
        .complete_beat, .complete_beats, .complete_bytes,
        .count(child_count), .child_valid, .child_tag
    );

    bridge_reorder_buffer #(
        .DATA_W(DATA_W), .MAX_OUTSTANDING(MAX_OUTSTANDING),
        .MAX_BURST_BEATS(MAX_BURST_BEATS)
    ) reorder (
        .clk, .rst_n, .parent_alloc_valid(request_pop),
        .parent_alloc(free_parent),
        .read_issue_valid(issue_fire && !issue_out_write),
        .read_issue_parent(issue_out_parent),
        .read_issue_beat(issue_out_beat),
        .read_issue_beats(issue_out_beats),
        .complete_valid(mc_rsp_fire && !parent_write[complete_parent]),
        .complete_parent, .complete_beat, .complete_beats, .complete_bytes,
        .complete_data(mc_rsp_data),
        .complete_parent_size(parent_size[complete_parent]),
        .complete_parent_addr(parent_addr[complete_parent]),
        .read_data(parent_read_data), .read_done(parent_read_done)
    );

    bridge_scheduler #(
        .RP_COUNT(RP_COUNT), .MAX_OUTSTANDING(MAX_OUTSTANDING),
        .REQUEST_FIFO_DEPTH(REQUEST_FIFO_DEPTH),
        .WDATA_FIFO_DEPTH(WDATA_FIFO_DEPTH),
        .MAX_BURST_BEATS(MAX_BURST_BEATS)
    ) scheduler (
        .issue_rr, .request_rr, .pair_rr, .response_rr,
        .parent_valid, .parent_write, .parent_synthetic, .parent_len,
        .parent_issue_beat, .parent_done_beats, .parent_wdata_beats,
        .parent_emit_beat, .parent_wbuf_valid, .parent_read_done,
        .response_order, .write_order, .response_head, .write_head,
        .response_rp_count, .write_count, .req_fifo_count,
        .wdata_fifo_count, .rdata_credit_available, .wresp_credit_available,
        .request_available, .request_rp, .issue_found, .issue_parent,
        .write_target_available, .write_target, .pair_rp,
        .response_available, .response_parent, .response_rp
    );

    bridge_response_gen #(.DATA_W(DATA_W)) response_gen (
        .response_available,
        .parent_valid(parent_valid[response_parent]),
        .parent_write(parent_write[response_parent]),
        .parent_synthetic(parent_synthetic[response_parent]),
        .parent_rp(parent_rp[response_parent]),
        .parent_user(parent_user[response_parent]),
        .parent_id(parent_id[response_parent]),
        .parent_status(parent_status[response_parent][1:0]),
        .parent_emit_beat(parent_emit_beat[response_parent]),
        .parent_len(parent_len[response_parent]),
        .parent_read_data(response_selected_data),
        .rsp_msg_valid, .rsp_msg_granules, .rsp_msg_data
    );

    assign request_pop = request_available && free_parent_found &&
        response_count < COUNT_W'(MAX_OUTSTANDING);
    assign wdata_pair = write_target_available;
    assign issue_fire = mc_req_valid && mc_req_ready;
    assign mc_rsp_fire = mc_rsp_valid && mc_rsp_ready;
    assign rsp_fire = rsp_msg_valid && rsp_msg_ready;
    assign retire_parent = rsp_fire && response_available &&
        (parent_write[response_parent] ||
         parent_emit_beat[response_parent] == {1'b0, parent_len[response_parent]});
    assign retire_write = wdata_pair && write_target_available &&
        parent_wdata_beats[write_target] == {1'b0, parent_len[write_target]};
    assign push_parent = request_pop;
    assign push_write = request_pop && req_head_write;

    // Request slots are released only when the request first enters the
    // memory-controller queue. Synthetic errors have no MC transaction, so
    // their slot is released when the error response enters TX.
    assign req_release0_valid = issue_fire &&
        !parent_req_released[issue_out_parent];
    assign req_release0_write = issue_out_write;
    assign req_release0_rp = parent_rp[issue_out_parent];
    assign req_release0_granules = parent_req_granules[issue_out_parent];
    assign req_release1_valid = retire_parent &&
        parent_synthetic[response_parent] &&
        !parent_req_released[response_parent];
    assign req_release1_write = parent_write[response_parent];
    assign req_release1_rp = parent_rp[response_parent];
    assign req_release1_granules = parent_req_granules[response_parent];

    // WDATA credit follows the reserved parent buffer, not the ingress FIFO.
    assign wdata_release_valid = wdata_pair;
    assign wdata_release_rp = pair_rp;
    assign wdata_release_granules = wdata_head_granules;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            response_count <= '0;
            issue_rr <= '0;
            request_rr <= '0;
            pair_rr <= '0;
            response_rr <= '0;
            issue_out_valid <= 1'b0;
            issue_out_write <= 1'b0;
            issue_out_parent <= '0;
            issue_out_beat <= '0;
            issue_out_beats <= '0;
            issue_out_bytes <= '0;
            issue_out_addr <= '0;
            issue_out_data <= '0;
            issue_out_mask <= '0;
            issue_out_tag <= '0;
            issue_out_qos <= '0;
            protocol_error <= 1'b0;

            for (int p = 0; p < MAX_OUTSTANDING; p = p + 1) begin
                parent_valid[p] <= 1'b0;
                parent_write[p] <= 1'b0;
                parent_synthetic[p] <= 1'b0;
                parent_rp[p] <= '0;
                parent_user[p] <= '0;
                parent_id[p] <= '0;
                parent_size[p] <= '0;
                parent_len[p] <= '0;
                parent_qos[p] <= '0;
                parent_addr[p] <= '0;
                parent_req_granules[p] <= '0;
                parent_req_released[p] <= 1'b0;
                parent_issue_beat[p] <= '0;
                parent_done_beats[p] <= '0;
                parent_wdata_beats[p] <= '0;
                parent_emit_beat[p] <= '0;
                parent_status[p] <= '0;
                parent_wbuf_valid[p] <= 1'b0;
                parent_wbuf_data[p] <= '0;
                parent_wbuf_mask[p] <= '0;
            end
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                response_head[rp] <= '0;
                response_tail[rp] <= '0;
                response_rp_count[rp] <= '0;
                write_head[rp] <= '0;
                write_tail[rp] <= '0;
                write_count[rp] <= '0;
                for (int p = 0; p < MAX_OUTSTANDING; p = p + 1)
                    response_order[rp][p] <= '0;
                for (int p = 0; p < MAX_OUTSTANDING; p = p + 1)
                    write_order[rp][p] <= '0;
            end
        end else begin
            protocol_error <= 1'b0;

            if (request_pop)
                request_rr <= 2'((int'(request_rp) + 1) % RP_COUNT);

            if (!issue_out_valid && issue_found && free_child_found &&
                child_count < CHILD_COUNT_W'(MC_INFLIGHT)) begin
                issue_out_valid <= 1'b1;
                issue_out_write <= parent_write[issue_parent];
                issue_out_parent <= issue_parent;
                issue_out_beat <= parent_issue_beat[issue_parent][7:0];
                issue_out_beats <= issue_transaction_beats;
                issue_out_bytes <= 6'(issue_transaction_bytes);
                issue_out_addr <= issue_beat_addr;
                issue_out_data <= issue_build_data;
                issue_out_mask <= issue_build_mask;
                issue_out_tag <= next_tag;
                issue_out_qos <= parent_qos[issue_parent];
            end
            if (issue_fire)
                issue_out_valid <= 1'b0;

            case ({push_parent, retire_parent})
                2'b10: response_count <= response_count + 1'b1;
                2'b01: response_count <= response_count - 1'b1;
                default: response_count <= response_count;
            endcase
            if (push_parent) begin
                response_order[request_rp][response_tail[request_rp]] <= free_parent;
                response_tail[request_rp] <= parent_next(response_tail[request_rp]);
            end
            if (retire_parent)
                response_head[response_rp] <=
                    parent_next(response_head[response_rp]);
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                case ({push_parent && int'(request_rp) == rp,
                       retire_parent && int'(response_rp) == rp})
                    2'b10: response_rp_count[rp] <= response_rp_count[rp] + 1'b1;
                    2'b01: response_rp_count[rp] <= response_rp_count[rp] - 1'b1;
                    default: response_rp_count[rp] <= response_rp_count[rp];
                endcase
            end

            if (push_write) begin
                write_order[request_rp][write_tail[request_rp]] <= free_parent;
                write_tail[request_rp] <= parent_next(write_tail[request_rp]);
            end
            if (retire_write)
                write_head[pair_rp] <= parent_next(write_head[pair_rp]);
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                case ({push_write && int'(request_rp) == rp,
                       retire_write && int'(pair_rp) == rp})
                    2'b10: write_count[rp] <= write_count[rp] + 1'b1;
                    2'b01: write_count[rp] <= write_count[rp] - 1'b1;
                    default: write_count[rp] <= write_count[rp];
                endcase
            end

            if (request_pop) begin
                parent_valid[free_parent] <= 1'b1;
                parent_write[free_parent] <= req_head_write;
                parent_synthetic[free_parent] <=
                    req_head_size > 3'($clog2(DATA_BYTES)) ||
                    (req_head_addr &
                        ((64'(1) << req_head_size) - 1)) != 0 ||
                    ((req_head_addr & 64'hfff) +
                        ((64'(req_head_len) + 1) << req_head_size) > 4096);
                parent_rp[free_parent] <= request_rp;
                parent_user[free_parent] <= req_head_user;
                parent_id[free_parent] <= req_head_id;
                parent_size[free_parent] <= req_head_size;
                parent_len[free_parent] <= req_head_len;
                parent_qos[free_parent] <= req_head_qos;
                parent_addr[free_parent] <= req_head_addr;
                parent_req_granules[free_parent] <= req_head_granules;
                parent_req_released[free_parent] <= 1'b0;
                parent_issue_beat[free_parent] <= '0;
                parent_done_beats[free_parent] <= '0;
                parent_wdata_beats[free_parent] <= '0;
                parent_emit_beat[free_parent] <= '0;
                parent_status[free_parent] <=
                    (req_head_size > 3'($clog2(DATA_BYTES)) ||
                     (req_head_addr &
                         ((64'(1) << req_head_size) - 1)) != 0 ||
                     ((req_head_addr & 64'hfff) +
                         ((64'(req_head_len) + 1) << req_head_size) > 4096))
                    ? 3'd2 : 3'd0;
                parent_wbuf_valid[free_parent] <= 1'b0;
                parent_wbuf_data[free_parent] <= '0;
                parent_wbuf_mask[free_parent] <= '0;
            end

            if (wdata_pair) begin
                parent_wbuf_data[write_target] <= pair_merged_data;
                parent_wbuf_mask[write_target] <= pair_merged_mask;
                parent_wbuf_valid[write_target] <= pair_chunk_complete;
                parent_wdata_beats[write_target] <=
                    parent_wdata_beats[write_target] + 1'b1;
                pair_rr <= 2'((int'(pair_rp) + 1) % RP_COUNT);
            end

            if (issue_fire) begin
                parent_issue_beat[issue_out_parent] <=
                    parent_issue_beat[issue_out_parent] + 9'(issue_out_beats);
                if (issue_out_write) begin
                    parent_wbuf_valid[issue_out_parent] <= 1'b0;
                    parent_wbuf_data[issue_out_parent] <= '0;
                    parent_wbuf_mask[issue_out_parent] <= '0;
                end
                issue_rr <= parent_next(issue_out_parent);
                if (!parent_req_released[issue_out_parent])
                    parent_req_released[issue_out_parent] <= 1'b1;
            end

            if (mc_rsp_fire) begin
                if (parent_status[complete_parent] == 0 && mc_rsp_status != 0)
                    parent_status[complete_parent] <= mc_rsp_status;
                parent_done_beats[complete_parent] <=
                    parent_done_beats[complete_parent] + 9'(complete_beats);
            end else if (mc_rsp_valid && !rsp_match_found) begin
                protocol_error <= 1'b1;
            end

            if (rsp_fire && response_available) begin
                response_rr <= 2'((int'(response_rp) + 1) % RP_COUNT);
                if (!parent_write[response_parent] && !retire_parent)
                    parent_emit_beat[response_parent] <=
                        parent_emit_beat[response_parent] + 1'b1;
                if (retire_parent) begin
                    parent_valid[response_parent] <= 1'b0;
                    parent_wbuf_valid[response_parent] <= 1'b0;
                end
            end
        end
    end

    logic _unused;
    assign _unused = ^wdata_head_user;

    bridge_assertions #(
        .DATA_W(DATA_W), .MC_TAG_W(MC_TAG_W), .MC_BYTES(MC_BYTES),
        .RP_COUNT(RP_COUNT), .MAX_OUTSTANDING(MAX_OUTSTANDING),
        .MC_INFLIGHT(MC_INFLIGHT), .REQUEST_FIFO_DEPTH(REQUEST_FIFO_DEPTH),
        .WDATA_FIFO_DEPTH(WDATA_FIFO_DEPTH)
    ) assertions (
        .clk, .rst_n,
        .mc_req_valid, .mc_req_ready, .mc_req_write, .mc_req_addr,
        .mc_req_bytes, .mc_req_data, .mc_req_byte_mask, .mc_req_tag,
        .mc_req_qos, .rsp_msg_valid, .rsp_msg_ready, .rsp_msg_granules,
        .rsp_msg_data, .response_count, .child_count, .req_fifo_count,
        .wdata_fifo_count, .response_rp_count, .write_count,
        .child_valid, .child_tag
    );
    /* verilator lint_on WIDTHTRUNC */
endmodule
