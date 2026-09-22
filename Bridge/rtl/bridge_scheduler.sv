module bridge_scheduler #(
    parameter int RP_COUNT = 1,
    parameter int MAX_OUTSTANDING = 8,
    parameter int REQUEST_FIFO_DEPTH = 4,
    parameter int WDATA_FIFO_DEPTH = 16,
    parameter int MAX_BURST_BEATS = 256
) (
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        issue_rr,
    input  logic [1:0] request_rr,
    input  logic [1:0] pair_rr,
    input  logic [1:0] response_rr,
    input  logic parent_valid [0:MAX_OUTSTANDING-1],
    input  logic parent_write [0:MAX_OUTSTANDING-1],
    input  logic parent_synthetic [0:MAX_OUTSTANDING-1],
    input  logic [7:0] parent_len [0:MAX_OUTSTANDING-1],
    input  logic [8:0] parent_issue_beat [0:MAX_OUTSTANDING-1],
    input  logic [8:0] parent_done_beats [0:MAX_OUTSTANDING-1],
    input  logic [8:0] parent_wdata_beats [0:MAX_OUTSTANDING-1],
    input  logic [8:0] parent_emit_beat [0:MAX_OUTSTANDING-1],
    input  logic parent_wbuf_valid [0:MAX_OUTSTANDING-1],
    input  logic parent_read_done
        [0:MAX_OUTSTANDING-1][0:MAX_BURST_BEATS-1],
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        response_order [0:RP_COUNT-1][0:MAX_OUTSTANDING-1],
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        write_order [0:RP_COUNT-1][0:MAX_OUTSTANDING-1],
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        response_head [0:RP_COUNT-1],
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        write_head [0:RP_COUNT-1],
    input  logic [$clog2(MAX_OUTSTANDING+1)-1:0]
        response_rp_count [0:RP_COUNT-1],
    input  logic [$clog2(MAX_OUTSTANDING+1)-1:0]
        write_count [0:RP_COUNT-1],
    input  logic [$clog2(REQUEST_FIFO_DEPTH+1)-1:0]
        req_fifo_count [0:RP_COUNT-1],
    input  logic [$clog2(WDATA_FIFO_DEPTH+1)-1:0]
        wdata_fifo_count [0:RP_COUNT-1],
    input  logic [3:0] rdata_credit_available,
    input  logic [3:0] wresp_credit_available,
    output logic request_available,
    output logic [1:0] request_rp,
    output logic issue_found,
    output logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        issue_parent,
    output logic write_target_available,
    output logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        write_target,
    output logic [1:0] pair_rp,
    output logic response_available,
    output logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        response_parent,
    output logic [1:0] response_rp
);
    localparam int PARENT_W =
        (MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING);
    localparam int RP_INDEX_W = (RP_COUNT <= 2) ? 1 : $clog2(RP_COUNT);

    always_comb begin
        request_available = 1'b0;
        request_rp = request_rr;
        for (int n = 0; n < RP_COUNT; n = n + 1) begin
            logic [RP_INDEX_W-1:0] candidate_index;
            candidate_index =
                RP_INDEX_W'((int'(request_rr) + n) % RP_COUNT);
            if (!request_available && req_fifo_count[candidate_index] != 0) begin
                request_available = 1'b1;
                request_rp = 2'(candidate_index);
            end
        end

        issue_found = 1'b0;
        issue_parent = '0;
        for (int n = 0; n < MAX_OUTSTANDING; n = n + 1) begin
            logic [PARENT_W-1:0] candidate;
            candidate = PARENT_W'((int'(issue_rr) + n) % MAX_OUTSTANDING);
            if (!issue_found && parent_valid[candidate] &&
                !parent_synthetic[candidate] &&
                parent_issue_beat[candidate] <= {1'b0, parent_len[candidate]} &&
                (!parent_write[candidate] || parent_wbuf_valid[candidate])) begin
                issue_found = 1'b1;
                issue_parent = candidate;
            end
        end

        write_target_available = 1'b0;
        write_target = '0;
        pair_rp = pair_rr;
        for (int n = 0; n < RP_COUNT; n = n + 1) begin
            logic [RP_INDEX_W-1:0] candidate_index;
            logic [PARENT_W-1:0] candidate_parent;
            candidate_index =
                RP_INDEX_W'((int'(pair_rr) + n) % RP_COUNT);
            candidate_parent =
                write_order[candidate_index][write_head[candidate_index]];
            if (!write_target_available && write_count[candidate_index] != 0 &&
                wdata_fifo_count[candidate_index] != 0 &&
                !parent_wbuf_valid[candidate_parent] &&
                parent_wdata_beats[candidate_parent] <=
                    {1'b0, parent_len[candidate_parent]}) begin
                write_target_available = 1'b1;
                write_target = candidate_parent;
                pair_rp = 2'(candidate_index);
            end
        end

        response_available = 1'b0;
        response_parent = '0;
        response_rp = response_rr;
        for (int n = 0; n < RP_COUNT; n = n + 1) begin
            logic [RP_INDEX_W-1:0] candidate_index;
            logic [PARENT_W-1:0] candidate_parent;
            logic candidate_done;
            logic candidate_has_credit;
            candidate_index =
                RP_INDEX_W'((int'(response_rr) + n) % RP_COUNT);
            candidate_parent =
                response_order[candidate_index][response_head[candidate_index]];
            candidate_done = 1'b0;
            candidate_has_credit = 1'b0;
            if (response_rp_count[candidate_index] != 0 &&
                parent_valid[candidate_parent]) begin
                if (parent_write[candidate_parent]) begin
                    candidate_done = parent_synthetic[candidate_parent] ||
                        (parent_issue_beat[candidate_parent] >
                             {1'b0, parent_len[candidate_parent]} &&
                         parent_done_beats[candidate_parent] >
                             {1'b0, parent_len[candidate_parent]});
                    candidate_has_credit =
                        wresp_credit_available[2'(candidate_index)];
                end else begin
                    candidate_done = parent_synthetic[candidate_parent] ||
                        parent_read_done[candidate_parent]
                            [parent_emit_beat[candidate_parent][7:0]];
                    candidate_has_credit =
                        rdata_credit_available[2'(candidate_index)];
                end
            end
            if (!response_available && candidate_done && candidate_has_credit) begin
                response_available = 1'b1;
                response_parent = candidate_parent;
                response_rp = 2'(candidate_index);
            end
        end
    end
endmodule
