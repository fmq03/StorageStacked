module storage_bridge_rx #(
    parameter int FDI_PLP_W = bridge_pkg::FDI_PLP_W,
    parameter int DATA_W = 256
) (
    input  logic                 clk,
    input  logic                 rst_n,
    input  logic                 link_active,
    input  logic                 fdi_rx_valid,
    output logic                 fdi_rx_ready,
    input  logic [FDI_PLP_W-1:0] fdi_rx_plp,

    output logic                 req_valid,
    input  logic                 req_ready,
    output logic                 req_write,
    output logic [1:0]           req_rp,
    output logic [15:0]          req_user,
    output logic [9:0]           req_id,
    output logic [2:0]           req_size,
    output logic [7:0]           req_len,
    output logic [3:0]           req_qos,
    output logic [63:0]          req_addr,
    output logic [5:0]           req_granules,

    output logic                 wdata_valid,
    input  logic                 wdata_ready,
    output logic [1:0]           wdata_rp,
    output logic [15:0]          wdata_user,
    output logic [DATA_W-1:0]    wdata_data,
    output logic [DATA_W/8-1:0]  wdata_mask,
    output logic [5:0]           wdata_granules,

    output logic                 misc_valid,
    input  logic                 misc_ready,
    output logic [bridge_pkg::MSG_MAX_W-1:0] misc_data,
    output logic [5:0]           misc_granules,
    output logic                 header_credit_valid,
    output logic [15:0]          header_credit,
    output logic                 error_valid,
    output logic [3:0]           error_code,
    output logic                 unsupported
);
    logic msg_valid;
    logic msg_ready;
    logic [3:0] msg_type;
    logic [1:0] msg_rp;
    logic [5:0] msg_granules;
    logic [bridge_pkg::MSG_MAX_W-1:0] msg_data;

    fdi_rx_unpack #(.FDI_PLP_W(FDI_PLP_W)) unpack (
        .clk, .rst_n, .link_active,
        .fdi_rx_valid, .fdi_rx_ready, .fdi_rx_plp,
        .msg_valid, .msg_ready, .msg_type, .msg_rp, .msg_granules, .msg_data,
        .header_credit_valid, .header_credit,
        .error_valid, .error_code
    );

    aou_msg_decode #(.DATA_W(DATA_W)) decode (
        .msg_valid, .msg_ready, .msg_type, .msg_rp, .msg_granules, .msg_data,
        .req_valid, .req_ready, .req_write, .req_rp, .req_user, .req_id,
        .req_size, .req_len, .req_qos, .req_addr, .req_granules,
        .wdata_valid, .wdata_ready, .wdata_rp, .wdata_user, .wdata_data,
        .wdata_mask, .wdata_granules,
        .misc_valid, .misc_ready, .misc_data, .misc_granules, .unsupported
    );
endmodule
