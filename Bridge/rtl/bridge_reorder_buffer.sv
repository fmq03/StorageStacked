module bridge_reorder_buffer #(
    parameter int DATA_W = 256,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MAX_BURST_BEATS = 256
) (
    input logic clk,
    input logic rst_n,
    input logic parent_alloc_valid,
    input logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        parent_alloc,
    input logic read_issue_valid,
    input logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        read_issue_parent,
    input logic [7:0] read_issue_beat,
    input logic [5:0] read_issue_beats,
    input logic complete_valid,
    input logic [((MAX_OUTSTANDING <= 2) ? 1 : $clog2(MAX_OUTSTANDING))-1:0]
        complete_parent,
    input logic [7:0] complete_beat,
    input logic [5:0] complete_beats,
    input logic [5:0] complete_bytes,
    input logic [DATA_W-1:0] complete_data,
    input logic [2:0] complete_parent_size,
    input logic [63:0] complete_parent_addr,
    output logic [DATA_W-1:0] read_data
        [0:MAX_OUTSTANDING-1][0:MAX_BURST_BEATS-1],
    output logic read_done
        [0:MAX_OUTSTANDING-1][0:MAX_BURST_BEATS-1]
);
    localparam int DATA_BYTES = DATA_W / 8;

    initial begin
        if (DATA_W != 256)
            $error("first reorder buffer revision requires DATA_W=256");
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int p = 0; p < MAX_OUTSTANDING; p = p + 1)
                for (int b = 0; b < MAX_BURST_BEATS; b = b + 1) begin
                    read_data[p][b] <= '0;
                    read_done[p][b] <= 1'b0;
                end
        end else begin
            if (parent_alloc_valid)
                for (int b = 0; b < MAX_BURST_BEATS; b = b + 1)
                    read_done[parent_alloc][b] <= 1'b0;

            if (read_issue_valid)
                for (int b = 0; b < DATA_BYTES; b = b + 1)
                    if (b < read_issue_beats)
                        read_data[read_issue_parent]
                            [int'(read_issue_beat) + b] <= '0;

            if (complete_valid) begin
                for (int j = 0; j < DATA_BYTES; j = j + 1) begin
                    logic [8:0] target_beat;
                    logic [6:0] target_lane;
                    target_beat = 9'(complete_beat) +
                        9'(j / (32'(1) << complete_parent_size));
                    target_lane = 7'((complete_parent_addr +
                        target_beat * (64'(1) << complete_parent_size)) %
                        64'(DATA_BYTES)) +
                        7'(j % (32'(1) << complete_parent_size));
                    if (j < complete_bytes)
                        read_data[complete_parent]
                            [target_beat[7:0]][8*int'(target_lane) +: 8]
                            <= complete_data[8*j +: 8];
                end
                for (int b = 0; b < DATA_BYTES; b = b + 1)
                    if (b < complete_beats)
                        read_done[complete_parent]
                            [int'(complete_beat) + b] <= 1'b1;
            end
        end
    end
endmodule
