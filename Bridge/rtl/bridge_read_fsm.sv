module bridge_read_fsm #(
    parameter int MC_BYTES = 32
) (
    input  logic [63:0] parent_addr,
    input  logic [2:0]  parent_size,
    input  logic [7:0]  parent_len,
    input  logic [8:0]  parent_issue_beat,
    output logic [63:0] issue_addr,
    output logic [5:0]  issue_bytes,
    output logic [5:0]  issue_beats
);
    logic [6:0] beat_bytes;
    logic [15:0] remaining_bytes;
    logic [6:0] boundary_bytes;
    logic [6:0] transaction_bytes;

    always_comb begin
        beat_bytes = 7'(1) << parent_size;
        issue_addr = parent_addr + parent_issue_beat * beat_bytes;
        remaining_bytes =
            (16'({1'b0, parent_len}) + 16'd1 -
             16'(parent_issue_beat)) * 16'(beat_bytes);
        boundary_bytes =
            7'(64'(MC_BYTES) - (issue_addr % 64'(MC_BYTES)));
        transaction_bytes = remaining_bytes < 16'(boundary_bytes)
            ? 7'(remaining_bytes) : boundary_bytes;
        issue_bytes = 6'(transaction_bytes);
        issue_beats = 6'(transaction_bytes / beat_bytes);
    end
endmodule
