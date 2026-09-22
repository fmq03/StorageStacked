module fdi_tx_packer #(
    parameter int FDI_PLP_W = bridge_pkg::FDI_PLP_W
) (
    input  logic                 clk,
    input  logic                 rst_n,
    input  logic                 link_active,
    input  logic                 msg_valid,
    output logic                 msg_ready,
    input  logic [5:0]           msg_granules,
    input  logic [bridge_pkg::MSG_MAX_W-1:0] msg_data,
    input  logic                 credit_valid,
    output logic                 credit_ready,
    input  logic [15:0]          credit_field,
    output logic                 fdi_tx_valid,
    input  logic                 fdi_tx_ready,
    output logic [FDI_PLP_W-1:0] fdi_tx_plp,
    output logic                 busy
);
    import bridge_pkg::*;
    localparam int USED_W = $clog2(GRANULE_COUNT + 1);

    logic [FDI_PLP_W-1:0] build_plp;
    logic [USED_W-1:0] build_used;
    logic build_has_credit;
    logic spill_valid;
    logic [MSG_MAX_W-1:0] spill_data;
    logic [5:0] spill_granules;
    logic [5:0] spill_offset;
    logic output_slot;
    logic [6:0] build_space;

    function automatic int unsigned msg_start_bit(input int unsigned granule);
        if (granule < 4) msg_start_bit = 4 + granule;
        else if (granule < 12) msg_start_bit = 4 + granule;
        else if (granule < 16) msg_start_bit = 8 + granule;
        else if (granule < 24) msg_start_bit = 8 + granule;
        else if (granule < 28) msg_start_bit = 28 + granule;
        else if (granule < 36) msg_start_bit = 28 + granule;
        else if (granule < 40) msg_start_bit = 32 + granule;
        else msg_start_bit = 32 + granule;
    endfunction

    assign output_slot = link_active && (!fdi_tx_valid || fdi_tx_ready);
    assign build_space = 7'(GRANULE_COUNT) - 7'(build_used);
    assign msg_ready = link_active && !spill_valid && build_space != 0 &&
        (7'(msg_granules) <= build_space || output_slot);
    assign credit_ready = link_active && !build_has_credit;
    assign busy = fdi_tx_valid || build_used != 0 || build_has_credit || spill_valid;

    always_ff @(posedge clk or negedge rst_n) begin : p_pack
        logic [FDI_PLP_W-1:0] work_plp;
        logic [USED_W-1:0] work_used;
        logic work_has_credit;
        logic source_present;
        logic source_external;
        logic [MSG_MAX_W-1:0] source_data;
        logic [6:0] source_total;
        logic [6:0] source_offset;
        logic [6:0] source_remaining;
        logic [6:0] take;
        logic flush;
        if (!rst_n) begin
            build_plp <= '0;
            build_used <= '0;
            build_has_credit <= 1'b0;
            spill_valid <= 1'b0;
            spill_data <= '0;
            spill_granules <= '0;
            spill_offset <= '0;
            fdi_tx_valid <= 1'b0;
            fdi_tx_plp <= '0;
        end else begin
            work_plp = build_plp;
            work_used = build_used;
            work_has_credit = build_has_credit;
            source_present = 1'b0;
            source_external = 1'b0;
            source_data = '0;
            source_total = '0;
            source_offset = '0;
            source_remaining = '0;
            take = '0;
            flush = 1'b0;

            if (fdi_tx_valid && fdi_tx_ready)
                fdi_tx_valid <= 1'b0;

            if (credit_valid && credit_ready) begin
                work_plp[39:32] = credit_field[7:0];
                work_plp[47:40] = credit_field[15:8];
                work_has_credit = 1'b1;
            end

            if (spill_valid) begin
                source_present = 1'b1;
                source_data = spill_data;
                source_total = 7'(spill_granules);
                source_offset = 7'(spill_offset);
            end else if (msg_valid && msg_ready) begin
                source_present = 1'b1;
                source_external = 1'b1;
                source_data = msg_data;
                source_total = 7'(msg_granules);
                source_offset = '0;
                work_plp[msg_start_bit(int'(work_used))] = 1'b1;
            end

            if (source_present) begin
                source_remaining = source_total - source_offset;
                take = source_remaining < (7'(GRANULE_COUNT) - 7'(work_used))
                    ? source_remaining
                    : (7'(GRANULE_COUNT) - 7'(work_used));
                for (int g = 0; g < GRANULE_COUNT; g = g + 1) begin
                    for (int b = 0; b < GRANULE_BYTES; b = b + 1) begin
                        if (g < take)
                            work_plp[8*(PROTO_HEADER_BYTES +
                                (int'(work_used) + g) * GRANULE_BYTES + b) +: 8] =
                                source_data[8*((int'(source_offset) + g) *
                                    GRANULE_BYTES + b) +: 8];
                    end
                end
                work_used = USED_W'(7'(work_used) + take);
                if (source_remaining > take) begin
                    spill_valid <= 1'b1;
                    spill_data <= source_data;
                    spill_granules <= 6'(source_total);
                    spill_offset <= 6'(source_offset + take);
                end else if (!source_external) begin
                    spill_valid <= 1'b0;
                    spill_offset <= '0;
                end
            end

            if (work_used == USED_W'(GRANULE_COUNT) && output_slot)
                flush = 1'b1;
            else if (!source_present &&
                     (work_used != 0 || work_has_credit) && output_slot)
                flush = 1'b1;

            if (flush) begin
                fdi_tx_plp <= work_plp;
                fdi_tx_valid <= 1'b1;
                build_plp <= '0;
                build_used <= '0;
                build_has_credit <= 1'b0;
            end else begin
                build_plp <= work_plp;
                build_used <= work_used;
                build_has_credit <= work_has_credit;
            end
        end
    end

`ifndef SYNTHESIS
    property p_tx_stable;
        @(posedge clk) disable iff (!rst_n)
        fdi_tx_valid && !fdi_tx_ready |=> $stable(fdi_tx_plp);
    endproperty
    assert property (p_tx_stable);

    property p_msg_shape;
        @(posedge clk) disable iff (!rst_n)
        msg_valid |-> msg_granules > 0 &&
            msg_granules <= 6'(MSG_MAX_BYTES / GRANULE_BYTES);
    endproperty
    assert property (p_msg_shape);
`endif
endmodule
