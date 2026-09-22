module aou_msg_decode #(
    parameter int DATA_W = 256
) (
    input  logic                         msg_valid,
    output logic                         msg_ready,
    input  logic [3:0]                   msg_type,
    input  logic [1:0]                   msg_rp,
    input  logic [5:0]                   msg_granules,
    input  logic [bridge_pkg::MSG_MAX_W-1:0] msg_data,

    output logic                         req_valid,
    input  logic                         req_ready,
    output logic                         req_write,
    output logic [1:0]                   req_rp,
    output logic [15:0]                  req_user,
    output logic [9:0]                   req_id,
    output logic [2:0]                   req_size,
    output logic [7:0]                   req_len,
    output logic [3:0]                   req_qos,
    output logic [63:0]                  req_addr,
    output logic [5:0]                   req_granules,

    output logic                         wdata_valid,
    input  logic                         wdata_ready,
    output logic [1:0]                   wdata_rp,
    output logic [15:0]                  wdata_user,
    output logic [DATA_W-1:0]            wdata_data,
    output logic [DATA_W/8-1:0]          wdata_mask,
    output logic [5:0]                   wdata_granules,

    output logic                         misc_valid,
    input  logic                         misc_ready,
    output logic [bridge_pkg::MSG_MAX_W-1:0] misc_data,
    output logic [5:0]                   misc_granules,
    output logic                         unsupported
);
    import bridge_pkg::*;
    localparam int DATA_BYTES = DATA_W / 8;
    integer i;
    logic full_write;

    always_comb begin
        req_valid = 1'b0;
        wdata_valid = 1'b0;
        misc_valid = 1'b0;
        unsupported = 1'b0;
        msg_ready = 1'b0;

        req_write = msg_type == MSG_WRITE_REQ;
        req_rp = msg_rp;
        req_user = {msg_data[15:8], msg_data[23:16]};
        req_id = {msg_data[31:24], msg_data[39:38]};
        req_size = msg_data[37:35];
        req_len = msg_data[47:40];
        req_qos = msg_data[51:48];
        req_addr = {msg_data[63:56], msg_data[71:64], msg_data[79:72],
                    msg_data[87:80], msg_data[95:88], msg_data[103:96],
                    msg_data[111:104], msg_data[119:112]};
        req_granules = msg_granules;

        wdata_rp = msg_rp;
        wdata_user = {msg_data[15:8], msg_data[23:16]};
        wdata_data = '0;
        wdata_mask = '0;
        wdata_granules = msg_granules;
        full_write = msg_type == MSG_WRITE_FULL;
        for (i = 0; i < DATA_BYTES; i = i + 1) begin
            wdata_data[8*i +: 8] = msg_data[8*(3+DATA_BYTES-1-i) +: 8];
            if (full_write)
                wdata_mask[i] = 1'b1;
            else
                wdata_mask[i] = msg_data[8*(3+DATA_BYTES+(DATA_BYTES/8-1-(i/8))) + (i%8)];
        end

        misc_data = msg_data;
        misc_granules = msg_granules;

        case (msg_type)
            MSG_WRITE_REQ, MSG_READ_REQ: begin
                req_valid = msg_valid;
                msg_ready = req_ready;
            end
            MSG_WRITE_DATA, MSG_WRITE_FULL: begin
                if (msg_data[1:0] != 2'b00) begin
                    unsupported = msg_valid;
                    msg_ready = 1'b1;
                end else begin
                    wdata_valid = msg_valid;
                    msg_ready = wdata_ready;
                end
            end
            MSG_MISC: begin
                misc_valid = msg_valid;
                msg_ready = misc_ready;
            end
            default: begin
                unsupported = msg_valid;
                msg_ready = 1'b1;
            end
        endcase
    end
endmodule
