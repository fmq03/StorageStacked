module bridge_response_gen #(
    parameter int DATA_W = 256
) (
    input  logic                              response_available,
    input  logic                              parent_valid,
    input  logic                              parent_write,
    input  logic                              parent_synthetic,
    input  logic [1:0]                        parent_rp,
    input  logic [15:0]                       parent_user,
    input  logic [9:0]                        parent_id,
    input  logic [1:0]                        parent_status,
    input  logic [8:0]                        parent_emit_beat,
    input  logic [7:0]                        parent_len,
    input  logic [DATA_W-1:0]                 parent_read_data,
    output logic                              rsp_msg_valid,
    output logic [5:0]                        rsp_msg_granules,
    output logic [bridge_pkg::MSG_MAX_W-1:0]  rsp_msg_data
);
    import bridge_pkg::*;
    localparam int DATA_BYTES = DATA_W / 8;

    logic [15:0] response_word;

    initial begin
        if (DATA_W != 256)
            $error("first response generator revision requires DATA_W=256");
    end

    always_comb begin
        rsp_msg_valid = 1'b0;
        rsp_msg_granules = '0;
        rsp_msg_data = '0;
        response_word = '0;

        if (response_available && parent_valid) begin
            rsp_msg_valid = 1'b1;
            rsp_msg_data[15:8] = parent_user[15:8];
            rsp_msg_data[23:16] = parent_user[7:0];
            if (parent_write) begin
                rsp_msg_granules = 6'(WRESP_GRANULES);
                rsp_msg_data[7:0] = {MSG_WRITE_RESP, parent_rp, 2'b00};
                response_word = {parent_id, parent_status, 4'b0000};
            end else begin
                rsp_msg_granules = 6'd8;
                rsp_msg_data[7:0] = {MSG_READ_DATA, parent_rp, 2'b00};
                response_word = {parent_id, parent_status,
                                 parent_emit_beat == {1'b0, parent_len}, 3'b000};
                for (int j = 0; j < DATA_BYTES; j = j + 1) begin
                    rsp_msg_data[8*(5+j) +: 8] = parent_synthetic
                        ? 8'b0
                        : parent_read_data[8*(DATA_BYTES-1-j) +: 8];
                end
            end
            rsp_msg_data[31:24] = response_word[15:8];
            rsp_msg_data[39:32] = response_word[7:0];
        end
    end
endmodule
