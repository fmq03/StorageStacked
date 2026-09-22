package bridge_pkg;
    parameter int FDI_PLP_BYTES = 250;
    parameter int FDI_PLP_W = FDI_PLP_BYTES * 8;
    parameter int PROTO_HEADER_BYTES = 10;
    parameter int PAYLOAD_BYTES = 240;
    parameter int GRANULE_BYTES = 5;
    parameter int GRANULE_COUNT = 48;
    parameter int MSG_MAX_BYTES = 150;
    parameter int MSG_MAX_W = MSG_MAX_BYTES * 8;
    parameter int WREQ_GRANULES = 3;
    parameter int RREQ_GRANULES = 3;
    parameter int WRESP_GRANULES = 1;

    typedef enum logic [3:0] {
        MSG_MISC       = 4'h0,
        MSG_WRITE_REQ  = 4'h1,
        MSG_READ_REQ   = 4'h2,
        MSG_WRITE_DATA = 4'h3,
        MSG_READ_DATA  = 4'h4,
        MSG_WRITE_RESP = 4'h5,
        MSG_WRITE_FULL = 4'h6
    } msg_type_e;

    typedef enum logic [3:0] {
        RX_ERR_NONE          = 4'd0,
        RX_ERR_HEADER_RSVD   = 4'd1,
        RX_ERR_FDID          = 4'd2,
        RX_ERR_BAD_MESSAGE   = 4'd3,
        RX_ERR_START_OVERLAP = 4'd4
    } rx_error_e;

    function automatic int unsigned message_granules(input logic [7:0] header);
        logic [3:0] kind;
        logic [1:0] dlength;
        logic [2:0] misc_op;
        kind = header[7:4];
        dlength = header[1:0];
        misc_op = header[3:1];
        case (kind)
            MSG_WRITE_REQ: message_granules = WREQ_GRANULES;
            MSG_READ_REQ: message_granules = RREQ_GRANULES;
            MSG_WRITE_RESP: message_granules = WRESP_GRANULES;
            MSG_WRITE_DATA: begin
                case (dlength)
                    2'd0: message_granules = 8;
                    2'd1: message_granules = 15;
                    2'd2: message_granules = 30;
                    default: message_granules = 0;
                endcase
            end
            MSG_WRITE_FULL: begin
                case (dlength)
                    2'd0: message_granules = 7;
                    2'd1: message_granules = 14;
                    2'd2: message_granules = 27;
                    default: message_granules = 0;
                endcase
            end
            MSG_READ_DATA: begin
                case (dlength)
                    2'd0: message_granules = 8;
                    2'd1: message_granules = 14;
                    2'd2: message_granules = 27;
                    default: message_granules = 0;
                endcase
            end
            MSG_MISC: begin
                case (misc_op)
                    3'b000: message_granules = 1;
                    3'b100: message_granules = 2;
                    default: message_granules = 0;
                endcase
            end
            default: message_granules = 0;
        endcase
    endfunction
endpackage
