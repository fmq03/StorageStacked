module bridge_outstanding #(
    parameter int MC_TAG_W = 32,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MC_INFLIGHT = 8
) (
    input  logic clk,
    input  logic rst_n,
    input  logic alloc_valid,
    input  logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        alloc_parent,
    input  logic [7:0] alloc_beat,
    input  logic [5:0] alloc_beats,
    input  logic [5:0] alloc_bytes,
    output logic alloc_available,
    output logic [MC_TAG_W-1:0] alloc_tag,
    input  logic complete_valid,
    input  logic [MC_TAG_W-1:0] complete_tag,
    output logic complete_match,
    output logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        complete_parent,
    output logic [7:0] complete_beat,
    output logic [5:0] complete_beats,
    output logic [5:0] complete_bytes,
    output logic [$clog2(MC_INFLIGHT+1)-1:0] count,
    output logic child_valid [0:MC_INFLIGHT-1],
    output logic [MC_TAG_W-1:0] child_tag [0:MC_INFLIGHT-1]
);
    localparam int PARENT_W =
        (MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING);
    localparam int CHILD_W =
        (MC_INFLIGHT <= 2) ? 1 : $clog2(MC_INFLIGHT);

    logic [PARENT_W-1:0] child_parent [0:MC_INFLIGHT-1];
    logic [7:0] child_beat [0:MC_INFLIGHT-1];
    logic [5:0] child_beats [0:MC_INFLIGHT-1];
    logic [5:0] child_bytes [0:MC_INFLIGHT-1];
    logic [CHILD_W-1:0] free_child;
    logic [CHILD_W-1:0] matched_child;
    logic [MC_TAG_W-1:0] next_tag;

    assign alloc_tag = next_tag;
    assign complete_parent = child_parent[matched_child];
    assign complete_beat = child_beat[matched_child];
    assign complete_beats = child_beats[matched_child];
    assign complete_bytes = child_bytes[matched_child];

    always_comb begin
        alloc_available = 1'b0;
        free_child = '0;
        for (int c = 0; c < MC_INFLIGHT; c = c + 1) begin
            if (!alloc_available && !child_valid[c]) begin
                alloc_available = 1'b1;
                free_child = CHILD_W'(c);
            end
        end

        complete_match = 1'b0;
        matched_child = '0;
        for (int c = 0; c < MC_INFLIGHT; c = c + 1) begin
            if (!complete_match && child_valid[c] &&
                child_tag[c] == complete_tag) begin
                complete_match = 1'b1;
                matched_child = CHILD_W'(c);
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count <= '0;
            next_tag <= {{(MC_TAG_W-1){1'b0}}, 1'b1};
            for (int c = 0; c < MC_INFLIGHT; c = c + 1) begin
                child_valid[c] <= 1'b0;
                child_tag[c] <= '0;
                child_parent[c] <= '0;
                child_beat[c] <= '0;
                child_beats[c] <= '0;
                child_bytes[c] <= '0;
            end
        end else begin
            if (alloc_valid) begin
                child_valid[free_child] <= 1'b1;
                child_tag[free_child] <= next_tag;
                child_parent[free_child] <= alloc_parent;
                child_beat[free_child] <= alloc_beat;
                child_beats[free_child] <= alloc_beats;
                child_bytes[free_child] <= alloc_bytes;
                next_tag <= next_tag + 1'b1;
            end
            if (complete_valid && complete_match)
                child_valid[matched_child] <= 1'b0;
            case ({alloc_valid, complete_valid && complete_match})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
        end
    end
endmodule
