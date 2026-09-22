module bridge_write_fsm #(
    parameter int DATA_W = 256,
    parameter int MC_BYTES = 32
) (
    input  logic [63:0] issue_parent_addr,
    input  logic [2:0]  issue_parent_size,
    input  logic [7:0]  issue_parent_len,
    input  logic [8:0]  issue_parent_beat,
    input  logic [DATA_W-1:0] issue_parent_data,
    input  logic [DATA_W/8-1:0] issue_parent_mask,
    output logic [63:0] issue_addr,
    output logic [5:0]  issue_bytes,
    output logic [5:0]  issue_beats,
    output logic [DATA_W-1:0] issue_data,
    output logic [DATA_W/8-1:0] issue_mask,

    input  logic [63:0] pair_parent_addr,
    input  logic [2:0]  pair_parent_size,
    input  logic [7:0]  pair_parent_len,
    input  logic [8:0]  pair_parent_issue_beat,
    input  logic [8:0]  pair_parent_wdata_beats,
    input  logic [DATA_W-1:0] pair_parent_data,
    input  logic [DATA_W/8-1:0] pair_parent_mask,
    input  logic [DATA_W-1:0] pair_beat_data,
    input  logic [DATA_W/8-1:0] pair_beat_mask,
    output logic [DATA_W-1:0] pair_merged_data,
    output logic [DATA_W/8-1:0] pair_merged_mask,
    output logic pair_chunk_complete
);
    localparam int DATA_BYTES = DATA_W / 8;

    logic [6:0] issue_beat_bytes;
    logic [15:0] issue_remaining_bytes;
    logic [6:0] issue_boundary_bytes;
    logic [6:0] issue_transaction_bytes;
    logic [6:0] pair_beat_bytes;
    logic [63:0] pair_beat_addr;
    logic [5:0] pair_lane;
    logic [8:0] pair_chunk_beats;

    initial begin
        if (DATA_W != 256 || MC_BYTES != DATA_BYTES)
            $error("first write FSM revision requires DATA_W=256 and MC_BYTES=32");
    end

    always_comb begin
        issue_beat_bytes = 7'(1) << issue_parent_size;
        issue_addr =
            issue_parent_addr + issue_parent_beat * issue_beat_bytes;
        issue_remaining_bytes =
            (16'({1'b0, issue_parent_len}) + 16'd1 -
             16'(issue_parent_beat)) * 16'(issue_beat_bytes);
        issue_boundary_bytes =
            7'(64'(MC_BYTES) - (issue_addr % 64'(MC_BYTES)));
        issue_transaction_bytes =
            issue_remaining_bytes < 16'(issue_boundary_bytes)
                ? 7'(issue_remaining_bytes) : issue_boundary_bytes;
        issue_bytes = 6'(issue_transaction_bytes);
        issue_beats = 6'(issue_transaction_bytes / issue_beat_bytes);
        issue_data = issue_parent_data;
        issue_mask = issue_parent_mask;

        pair_beat_bytes = 7'(1) << pair_parent_size;
        pair_beat_addr = pair_parent_addr +
                         pair_parent_wdata_beats * pair_beat_bytes;
        pair_lane = 6'(pair_beat_addr % DATA_BYTES);
        pair_chunk_beats = 9'((64'(MC_BYTES) -
            ((pair_parent_addr +
              pair_parent_issue_beat * pair_beat_bytes) %
                64'(MC_BYTES))) /
            64'(pair_beat_bytes));
        if (pair_chunk_beats >
            ({1'b0, pair_parent_len} + 9'd1 - pair_parent_issue_beat))
            pair_chunk_beats =
                {1'b0, pair_parent_len} + 9'd1 - pair_parent_issue_beat;

        pair_merged_data = pair_parent_data;
        pair_merged_mask = pair_parent_mask;
        for (int j = 0; j < DATA_BYTES; j = j + 1) begin
            logic [8:0] chunk_offset;
            chunk_offset =
                (pair_parent_wdata_beats - pair_parent_issue_beat) *
                    9'(pair_beat_bytes) + 9'(j);
            if (j < pair_beat_bytes &&
                int'(pair_lane) + j < DATA_BYTES &&
                int'(chunk_offset) < DATA_BYTES) begin
                pair_merged_data[8*int'(chunk_offset) +: 8] =
                    pair_beat_data[8*(int'(pair_lane) + j) +: 8];
                pair_merged_mask[int'(chunk_offset)] =
                    pair_beat_mask[int'(pair_lane) + j];
            end
        end
        pair_chunk_complete =
            pair_parent_wdata_beats + 9'd1 ==
                pair_parent_issue_beat + pair_chunk_beats;
    end
endmodule
