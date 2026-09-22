module fdi_rx_unpack #(
    parameter int FDI_PLP_W = bridge_pkg::FDI_PLP_W
) (
    input  logic                          clk,
    input  logic                          rst_n,
    input  logic                          link_active,
    input  logic                          fdi_rx_valid,
    output logic                          fdi_rx_ready,
    input  logic [FDI_PLP_W-1:0]          fdi_rx_plp,

    output logic                          msg_valid,
    input  logic                          msg_ready,
    output logic [3:0]                    msg_type,
    output logic [1:0]                    msg_rp,
    output logic [5:0]                    msg_granules,
    output logic [bridge_pkg::MSG_MAX_W-1:0] msg_data,

    output logic                          header_credit_valid,
    output logic [15:0]                   header_credit,

    output logic                          error_valid,
    output logic [3:0]                    error_code
);
    import bridge_pkg::*;

    logic [7:0] flit_payload [0:PAYLOAD_BYTES-1];
    logic [7:0] message_buf [0:MSG_MAX_BYTES-1];
    logic [GRANULE_COUNT-1:0] starts;
    logic flit_loaded;
    logic emit_pending;
    logic [5:0] scan_granule;
    logic [5:0] message_have;
    logic [5:0] message_total;
    logic [3:0] current_type;
    logic [1:0] current_rp;

    integer i;
    logic [5:0] header_total;

    always_comb begin
        header_total = message_granules(
            flit_payload[scan_granule*GRANULE_BYTES])[5:0];
    end

    assign fdi_rx_ready = link_active && !flit_loaded && !emit_pending && !msg_valid;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            flit_loaded <= 1'b0;
            emit_pending <= 1'b0;
            msg_valid <= 1'b0;
            msg_type <= '0;
            msg_rp <= '0;
            msg_granules <= '0;
            msg_data <= '0;
            starts <= '0;
            scan_granule <= '0;
            message_have <= '0;
            message_total <= '0;
            current_type <= '0;
            current_rp <= '0;
            error_valid <= 1'b0;
            error_code <= RX_ERR_NONE;
            header_credit_valid <= 1'b0;
            header_credit <= '0;
            for (i = 0; i < PAYLOAD_BYTES; i = i + 1)
                flit_payload[i] <= '0;
            for (i = 0; i < MSG_MAX_BYTES; i = i + 1)
                message_buf[i] <= '0;
        end else begin
            error_valid <= 1'b0;
            header_credit_valid <= 1'b0;

            if (msg_valid && msg_ready)
                msg_valid <= 1'b0;

            if (fdi_rx_valid && fdi_rx_ready) begin
                if ((fdi_rx_plp[7:0] & 8'h0c) != 0 ||
                    (fdi_rx_plp[23:16] & 8'h0f) != 0 ||
                    (fdi_rx_plp[55:48] & 8'h0f) != 0 ||
                    (fdi_rx_plp[71:64] & 8'h0f) != 0) begin
                    error_valid <= 1'b1;
                    error_code <= RX_ERR_HEADER_RSVD;
                end else if (fdi_rx_plp[1:0] != 2'b00) begin
                    error_valid <= 1'b1;
                    error_code <= RX_ERR_FDID;
                end else begin
                    header_credit_valid <= 1'b1;
                    header_credit <= {fdi_rx_plp[47:40], fdi_rx_plp[39:32]};
                    starts[3:0] <= fdi_rx_plp[7:4];
                    starts[11:4] <= fdi_rx_plp[15:8];
                    starts[15:12] <= fdi_rx_plp[23:20];
                    starts[23:16] <= fdi_rx_plp[31:24];
                    starts[27:24] <= fdi_rx_plp[55:52];
                    starts[35:28] <= fdi_rx_plp[63:56];
                    starts[39:36] <= fdi_rx_plp[71:68];
                    starts[47:40] <= fdi_rx_plp[79:72];
                    for (i = 0; i < PAYLOAD_BYTES; i = i + 1)
                        flit_payload[i] <= fdi_rx_plp[8*(PROTO_HEADER_BYTES+i) +: 8];
                    scan_granule <= '0;
                    flit_loaded <= 1'b1;
                end
            end

            if (emit_pending && !msg_valid) begin
                msg_type <= current_type;
                msg_rp <= current_rp;
                msg_granules <= message_total;
                for (i = 0; i < MSG_MAX_BYTES; i = i + 1)
                    msg_data[8*i +: 8] <= message_buf[i];
                msg_valid <= 1'b1;
                emit_pending <= 1'b0;
                message_have <= '0;
            end else if (flit_loaded && !emit_pending && !msg_valid) begin
                if (scan_granule == 6'(GRANULE_COUNT)) begin
                    flit_loaded <= 1'b0;
                end else if (message_have != 0) begin
                    if (starts[scan_granule]) begin
                        error_valid <= 1'b1;
                        error_code <= RX_ERR_START_OVERLAP;
                        flit_loaded <= 1'b0;
                        message_have <= '0;
                    end else begin
                        for (i = 0; i < GRANULE_BYTES; i = i + 1)
                            message_buf[message_have*GRANULE_BYTES+i] <=
                                flit_payload[scan_granule*GRANULE_BYTES+i];
                        scan_granule <= scan_granule + 1'b1;
                        message_have <= message_have + 1'b1;
                        if (message_have + 1'b1 == message_total)
                            emit_pending <= 1'b1;
                    end
                end else if (!starts[scan_granule]) begin
                    scan_granule <= scan_granule + 1'b1;
                end else begin
                    if (header_total == 0 || header_total * GRANULE_BYTES > MSG_MAX_BYTES) begin
                        error_valid <= 1'b1;
                        error_code <= RX_ERR_BAD_MESSAGE;
                        flit_loaded <= 1'b0;
                    end else begin
                        current_type <= flit_payload[scan_granule*GRANULE_BYTES][7:4];
                        current_rp <= flit_payload[scan_granule*GRANULE_BYTES][3:2];
                        message_total <= header_total;
                        for (i = 0; i < GRANULE_BYTES; i = i + 1)
                            message_buf[i] <= flit_payload[scan_granule*GRANULE_BYTES+i];
                        scan_granule <= scan_granule + 1'b1;
                        message_have <= 6'd1;
                        if (header_total == 1)
                            emit_pending <= 1'b1;
                    end
                end
            end
        end
    end

`ifndef SYNTHESIS
    property p_rx_stable;
        @(posedge clk) disable iff (!rst_n)
        fdi_rx_valid && !fdi_rx_ready |=> $stable(fdi_rx_plp);
    endproperty
    assert property (p_rx_stable);

    property p_msg_stable;
        @(posedge clk) disable iff (!rst_n)
        msg_valid && !msg_ready |=> $stable({msg_type, msg_rp, msg_granules, msg_data});
    endproperty
    assert property (p_msg_stable);
`endif
endmodule
