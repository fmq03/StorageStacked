// Self-checking HIF traffic source for connection testing; NOT Bridge RTL.
module HifDemoMaster (
    input logic core_ddrc_core_clk,
    input logic core_ddrc_rstn,
    output logic hif_cmd_valid,
    input logic hif_cmd_stall,
    output logic [1:0] hif_cmd_type,
    output logic [39:0] hif_cmd_addr,
    output logic [1:0] hif_cmd_pri,
    output logic [52:0] hif_cmd_token,
    output logic hif_cmd_length,
    output logic [18:0] hif_cmd_wdata_ptr,
    output logic hif_cmd_autopre,
    output logic [10:0] hif_cmd_latency,
    input logic hif_lpr_credit,
    input logic hif_hpr_credit,
    input logic hif_wr_credit,
    input logic [1:0] hif_wrecc_credit,
    input logic hif_wdata_ptr_valid,
    input logic [18:0] hif_wdata_ptr,
    input logic hif_wdata_ptr_addr_err,
    output logic hif_wdata_valid,
    input logic hif_wdata_stall,
    output logic [255:0] hif_wdata,
    output logic [31:0] hif_wdata_mask,
    output logic hif_wdata_end,
    input logic hif_rdata_valid,
    input logic hif_rdata_end,
    input logic [52:0] hif_rdata_token,
    input logic [255:0] hif_rdata,
    input logic hif_rdata_addr_err,
    input logic hif_rdata_uncorr_ecc_err,
    input logic hif_rdata_crc_err,
    output logic hif_go2critical_lpr,
    output logic hif_go2critical_hpr,
    output logic hif_go2critical_wr,
    output logic done,
    output logic failed
);
    typedef enum logic [2:0] {WRITE_CMD, WAIT_POINTER, WRITE_DATA, WAIT_CREDIT,
                              READ_CMD, READ_DATA, FINISHED} state_t;
    state_t state;
    logic write_beat, read_beat, write_credit_seen;
    logic unused;
    assign unused = ^{hif_lpr_credit, hif_hpr_credit, hif_wrecc_credit};
    always_comb begin
        hif_cmd_valid = state == WRITE_CMD || state == READ_CMD;
        hif_cmd_type = state == READ_CMD ? 2'b01 : 2'b00;
        hif_cmd_addr = 40'h400; // byte address 0x1000, address unit 4B
        hif_cmd_pri = 2'b00;
        hif_cmd_token = 53'h10000000000055;
        hif_cmd_length = 0;
        hif_cmd_wdata_ptr = 19'd22;
        hif_cmd_autopre = 0;
        hif_cmd_latency = 0;
        hif_wdata_valid = state == WRITE_DATA;
        hif_wdata_mask = '1;
        hif_wdata_end = write_beat;
        for (int b = 0; b < 32; b++)
            hif_wdata[8*b +: 8] = 8'(b + (write_beat ? 32 : 0));
        hif_go2critical_lpr = 0;
        hif_go2critical_hpr = 0;
        hif_go2critical_wr = 0;
        done = state == FINISHED;
    end
    always_ff @(posedge core_ddrc_core_clk) begin
        if (!core_ddrc_rstn) begin
            state <= WRITE_CMD;
            write_beat <= 0;
            read_beat <= 0;
            write_credit_seen <= 0;
            failed <= 0;
        end else begin
            if (hif_wr_credit) write_credit_seen <= 1;
            case (state)
                WRITE_CMD: if (!hif_cmd_stall) state <= WAIT_POINTER;
                WAIT_POINTER: if (hif_wdata_ptr_valid) begin
                    if (hif_wdata_ptr != 19'd22 || hif_wdata_ptr_addr_err) failed <= 1;
                    state <= WRITE_DATA;
                end
                WRITE_DATA: if (!hif_wdata_stall) begin
                    write_beat <= 1;
                    if (write_beat) state <= WAIT_CREDIT;
                end
                WAIT_CREDIT: if (write_credit_seen || hif_wr_credit) state <= READ_CMD;
                READ_CMD: if (!hif_cmd_stall) state <= READ_DATA;
                READ_DATA: if (hif_rdata_valid) begin
                    if (hif_rdata_token != 53'h10000000000055 ||
                        hif_rdata_addr_err || hif_rdata_uncorr_ecc_err || hif_rdata_crc_err ||
                        hif_rdata_end != read_beat) failed <= 1;
                    for (int b = 0; b < 32; b++)
                        if (hif_rdata[8*b +: 8] != 8'(b + (read_beat ? 32 : 0))) failed <= 1;
                    read_beat <= 1;
                    if (hif_rdata_end) state <= FINISHED;
                end
                default: state <= FINISHED;
            endcase
        end
    end
endmodule
