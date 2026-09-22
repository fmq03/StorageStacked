module storage_bridge_top #(
    parameter int FDI_PLP_W = bridge_pkg::FDI_PLP_W,
    parameter int ADDR_W = 64,
    parameter int MC_DATA_W = 256,
    parameter int MC_MASK_W = MC_DATA_W / 8,
    parameter int MC_TAG_W = 32,
    parameter int RP_COUNT = 1,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MC_INFLIGHT = 8
) (
    input  logic                   clk,
    input  logic                   rst_n,
    input  logic                   link_active,
    input  logic                   fdi_rx_valid,
    output logic                   fdi_rx_ready,
    input  logic [FDI_PLP_W-1:0]   fdi_rx_plp,
    output logic                   fdi_tx_valid,
    input  logic                   fdi_tx_ready,
    output logic [FDI_PLP_W-1:0]   fdi_tx_plp,
    output logic                   mc_req_valid,
    input  logic                   mc_req_ready,
    output logic                   mc_req_write,
    output logic [ADDR_W-1:0]      mc_req_addr,
    output logic [5:0]             mc_req_bytes,
    output logic [MC_DATA_W-1:0]   mc_req_data,
    output logic [MC_MASK_W-1:0]   mc_req_byte_mask,
    output logic [MC_TAG_W-1:0]    mc_req_tag,
    output logic [3:0]             mc_req_qos,
    input  logic                   mc_rsp_valid,
    output logic                   mc_rsp_ready,
    input  logic [MC_TAG_W-1:0]    mc_rsp_tag,
    input  logic [2:0]             mc_rsp_status,
    input  logic [MC_DATA_W-1:0]   mc_rsp_data,
    output logic                   protocol_error,
    output logic [7:0]             protocol_error_code,
    output logic [11:0]            debug_available_rdata,
    output logic [11:0]            debug_available_wresp,
    output logic [3:0]             debug_response_count,
    output logic [8:0]             debug_head_issue,
    output logic [8:0]             debug_head_done,
    output logic [8:0]             debug_head_wdata,
    output logic [7:0]             debug_head_len,
    output logic                   debug_head_write,
    output logic                   busy
);
    import bridge_pkg::*;
    logic req_valid, req_ready, req_write;
    logic [1:0] req_rp, wdata_rp;
    logic [15:0] req_user, wdata_user;
    logic [9:0] req_id;
    logic [2:0] req_size;
    logic [7:0] req_len;
    logic [3:0] req_qos;
    logic [63:0] req_addr;
    logic [5:0] req_granules, wdata_granules;
    logic wdata_valid, wdata_ready;
    logic [MC_DATA_W-1:0] wdata_data;
    logic [MC_MASK_W-1:0] wdata_mask;
    logic misc_valid, rx_error, unsupported;
    logic [3:0] rx_error_code;
    logic [MSG_MAX_W-1:0] misc_data;
    logic [5:0] misc_granules;
    logic header_credit_valid;
    logic [15:0] header_credit;
    logic rsp_msg_valid, rsp_msg_ready;
    logic [5:0] rsp_msg_granules;
    logic [MSG_MAX_W-1:0] rsp_msg_data;
    logic engine_error;
    logic credit_error, grant_valid, grant_ready, rsp_credit_ok;
    logic [15:0] grant_field;
    logic engine_rsp_ready;
    logic engine_busy, tx_busy;
    logic [3:0] rdata_credit_available, wresp_credit_available;
    logic req_release0_valid, req_release0_write;
    logic [1:0] req_release0_rp;
    logic [5:0] req_release0_granules;
    logic req_release1_valid, req_release1_write;
    logic [1:0] req_release1_rp;
    logic [5:0] req_release1_granules;
    logic wdata_release_valid;
    logic [1:0] wdata_release_rp;
    logic [5:0] wdata_release_granules;

    initial begin
        if (ADDR_W != 64 || MC_DATA_W != 256 || MC_MASK_W != 32)
            $error("first RTL revision requires ADDR_W=64 and MC_DATA_W=256");
        if (RP_COUNT < 1 || RP_COUNT > 4)
            $error("RP_COUNT must be 1..4");
    end

    storage_bridge_rx #(.FDI_PLP_W(FDI_PLP_W), .DATA_W(MC_DATA_W)) rx (
        .clk, .rst_n, .link_active, .fdi_rx_valid, .fdi_rx_ready, .fdi_rx_plp,
        .req_valid, .req_ready, .req_write, .req_rp, .req_user, .req_id,
        .req_size, .req_len, .req_qos, .req_addr, .req_granules,
        .wdata_valid, .wdata_ready, .wdata_rp, .wdata_user, .wdata_data,
        .wdata_mask, .wdata_granules,
        .misc_valid, .misc_ready(1'b1), .misc_data, .misc_granules,
        .header_credit_valid, .header_credit,
        .error_valid(rx_error), .error_code(rx_error_code), .unsupported
    );

    bridge_transaction_engine #(
        .DATA_W(MC_DATA_W), .RP_COUNT(RP_COUNT), .MC_TAG_W(MC_TAG_W),
        .MAX_OUTSTANDING(MAX_OUTSTANDING), .MC_INFLIGHT(MC_INFLIGHT)
    ) engine (
        .clk, .rst_n,
        .req_valid, .req_ready, .req_write, .req_rp, .req_user, .req_id,
        .req_size, .req_len, .req_qos, .req_addr, .req_granules,
        .wdata_valid, .wdata_ready, .wdata_rp, .wdata_user, .wdata_data, .wdata_mask,
        .wdata_granules,
        .rdata_credit_available, .wresp_credit_available,
        .req_release0_valid, .req_release0_write, .req_release0_rp,
        .req_release0_granules,
        .req_release1_valid, .req_release1_write, .req_release1_rp,
        .req_release1_granules,
        .wdata_release_valid, .wdata_release_rp, .wdata_release_granules,
        .mc_req_valid, .mc_req_ready, .mc_req_write, .mc_req_addr, .mc_req_bytes,
        .mc_req_data, .mc_req_byte_mask, .mc_req_tag, .mc_req_qos,
        .mc_rsp_valid, .mc_rsp_ready, .mc_rsp_tag, .mc_rsp_status, .mc_rsp_data,
        .rsp_msg_valid, .rsp_msg_ready(engine_rsp_ready), .rsp_msg_granules, .rsp_msg_data,
        .busy(engine_busy), .protocol_error(engine_error),
        .debug_response_count, .debug_head_issue, .debug_head_done,
        .debug_head_wdata, .debug_head_len, .debug_head_write
    );

    bridge_credit_mgr #(
        .RP_COUNT(RP_COUNT), .REQUEST_GRANULES(12), .WDATA_GRANULES(64)
    ) credits (
        .clk, .rst_n, .link_active,
        .req_release0_valid, .req_release0_write, .req_release0_rp,
        .req_release0_granules,
        .req_release1_valid, .req_release1_write, .req_release1_rp,
        .req_release1_granules,
        .wdata_release_valid, .wdata_release_rp, .wdata_release_granules,
        .header_credit_valid, .header_credit,
        .misc_valid, .misc_data,
        .grant_valid, .grant_ready, .grant_field,
        .rsp_valid(rsp_msg_valid), .rsp_type(rsp_msg_data[7:4]),
        .rsp_rp(rsp_msg_data[3:2]),
        .rsp_granules(rsp_msg_granules), .rsp_credit_ok,
        .rdata_credit_available, .wresp_credit_available,
        .rsp_fire(rsp_msg_valid && engine_rsp_ready), .error(credit_error),
        .debug_available_rdata, .debug_available_wresp
    );

    assign engine_rsp_ready = rsp_msg_ready && rsp_credit_ok;

    fdi_tx_packer #(.FDI_PLP_W(FDI_PLP_W)) tx (
        .clk, .rst_n, .link_active,
        .msg_valid(rsp_msg_valid && rsp_credit_ok), .msg_ready(rsp_msg_ready),
        .msg_granules(rsp_msg_granules), .msg_data(rsp_msg_data),
        .credit_valid(grant_valid), .credit_ready(grant_ready),
        .credit_field(grant_field), .fdi_tx_valid, .fdi_tx_ready, .fdi_tx_plp,
        .busy(tx_busy)
    );

    assign busy = engine_busy || tx_busy;

    assign protocol_error = rx_error || unsupported || engine_error || credit_error ||
                            (req_valid && int'(req_rp) >= RP_COUNT) ||
                            (wdata_valid && int'(wdata_rp) >= RP_COUNT);
    always_comb begin
        protocol_error_code = 8'h00;
        if (rx_error) protocol_error_code = {4'h1, rx_error_code};
        else if (unsupported) protocol_error_code = 8'h20;
        else if (engine_error) protocol_error_code = 8'h30;
        else if (credit_error) protocol_error_code = 8'h40;
        else if (req_valid && int'(req_rp) >= RP_COUNT) protocol_error_code = 8'h50;
        else if (wdata_valid && int'(wdata_rp) >= RP_COUNT) protocol_error_code = 8'h51;
    end

    logic _unused;
    assign _unused = ^{req_granules, wdata_granules, misc_valid, misc_data,
                       misc_granules, rx_error_code, wdata_user,
                       MAX_OUTSTANDING, MC_INFLIGHT};
endmodule
