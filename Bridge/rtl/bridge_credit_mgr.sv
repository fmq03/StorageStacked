module bridge_credit_mgr #(
    parameter int RP_COUNT = 1,
    parameter int REQUEST_GRANULES = 3,
    parameter int WDATA_GRANULES = 8
) (
    input logic clk, rst_n, link_active,
    input logic req_release0_valid, req_release0_write,
    input logic [1:0] req_release0_rp,
    input logic [5:0] req_release0_granules,
    input logic req_release1_valid, req_release1_write,
    input logic [1:0] req_release1_rp,
    input logic [5:0] req_release1_granules,
    input logic wdata_release_valid,
    input logic [1:0] wdata_release_rp,
    input logic [5:0] wdata_release_granules,
    input logic header_credit_valid,
    input logic [15:0] header_credit,
    input logic misc_valid,
    input logic [bridge_pkg::MSG_MAX_W-1:0] misc_data,
    output logic grant_valid,
    input logic grant_ready,
    output logic [15:0] grant_field,
    input logic rsp_valid,
    input logic [3:0] rsp_type,
    input logic [1:0] rsp_rp,
    input logic [5:0] rsp_granules,
    output logic rsp_credit_ok,
    output logic [3:0] rdata_credit_available,
    output logic [3:0] wresp_credit_available,
    input logic rsp_fire,
    output logic error,
    output logic [11:0] debug_available_rdata,
    output logic [11:0] debug_available_wresp
);
    /* verilator lint_off WIDTHTRUNC */
    import bridge_pkg::*;
    logic [11:0] pending_wreq [0:RP_COUNT-1];
    logic [11:0] pending_rreq [0:RP_COUNT-1];
    logic [11:0] pending_wdata [0:RP_COUNT-1];
    logic [11:0] available_rdata [0:RP_COUNT-1];
    logic [11:0] available_wresp [0:RP_COUNT-1];
    logic [11:0] next_pending_wreq [0:RP_COUNT-1];
    logic [11:0] next_pending_rreq [0:RP_COUNT-1];
    logic [11:0] next_pending_wdata [0:RP_COUNT-1];
    logic [11:0] next_available_rdata [0:RP_COUNT-1];
    logic [11:0] next_available_wresp [0:RP_COUNT-1];
    logic [1:0] grant_rp, grant_rr, header_rp;
    logic grant_found, next_error;
    logic [2:0] wreq_enc, rreq_enc, wdata_enc, header_rdata_enc;
    logic [1:0] header_wresp_enc;
    logic [7:0] wreq_amount, rreq_amount, wdata_amount;
    logic _unused;

    function automatic logic [7:0] decode_amount(input logic [2:0] enc);
        case (enc)
            3'd0: decode_amount = 0;
            3'd1: decode_amount = 1;
            3'd2: decode_amount = 4;
            3'd3: decode_amount = 8;
            3'd4: decode_amount = 16;
            3'd5: decode_amount = 32;
            3'd6: decode_amount = 64;
            default: decode_amount = 128;
        endcase
    endfunction

    function automatic logic [2:0] encode_amount(input logic [11:0] amount);
        if (amount >= 128) encode_amount = 7;
        else if (amount >= 64) encode_amount = 6;
        else if (amount >= 32) encode_amount = 5;
        else if (amount >= 16) encode_amount = 4;
        else if (amount >= 8) encode_amount = 3;
        else if (amount >= 4) encode_amount = 2;
        else if (amount >= 1) encode_amount = 1;
        else encode_amount = 0;
    endfunction

    function automatic logic stream_bit(
        input logic [MSG_MAX_W-1:0] data, input int unsigned position
    );
        stream_bit = data[(position / 8) * 8 + (7 - (position % 8))];
    endfunction

    function automatic logic [2:0] stream_get3(
        input logic [MSG_MAX_W-1:0] data, input int unsigned position
    );
        stream_get3 = {stream_bit(data, position),
                       stream_bit(data, position + 1),
                       stream_bit(data, position + 2)};
    endfunction

    function automatic logic [1:0] stream_get2(
        input logic [MSG_MAX_W-1:0] data, input int unsigned position
    );
        stream_get2 = {stream_bit(data, position), stream_bit(data, position + 1)};
    endfunction

    always_comb begin
        grant_found = 1'b0;
        grant_rp = grant_rr;
        for (int n = 0; n < RP_COUNT; n = n + 1) begin
            logic [1:0] candidate;
            candidate = 2'((int'(grant_rr) + n) % RP_COUNT);
            if (!grant_found &&
                (pending_wreq[candidate] != 0 || pending_rreq[candidate] != 0 ||
                 pending_wdata[candidate] != 0)) begin
                grant_found = 1'b1;
                grant_rp = candidate;
            end
        end
        wreq_enc = encode_amount(pending_wreq[grant_rp]);
        rreq_enc = encode_amount(pending_rreq[grant_rp]);
        wdata_enc = encode_amount(pending_wdata[grant_rp]);
        wreq_amount = decode_amount(wreq_enc);
        rreq_amount = decode_amount(rreq_enc);
        wdata_amount = decode_amount(wdata_enc);
        grant_field = {grant_rp, 2'b00, 3'b000, wdata_enc, rreq_enc, wreq_enc};
        grant_valid = link_active && grant_found;

        header_rp = header_credit[15:14];
        header_rdata_enc = header_credit[11:9];
        header_wresp_enc = header_credit[13:12];
        rsp_credit_ok = 1'b1;
        rdata_credit_available = '0;
        wresp_credit_available = '0;
        for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
            rdata_credit_available[rp] = available_rdata[rp] >= 12'd8;
            wresp_credit_available[rp] = available_wresp[rp] >= 12'd1;
        end
        if (rsp_valid && int'(rsp_rp) >= RP_COUNT)
            rsp_credit_ok = 1'b0;
        else if (rsp_valid && rsp_type == MSG_READ_DATA)
            rsp_credit_ok = available_rdata[rsp_rp] >= 12'(rsp_granules);
        else if (rsp_valid && rsp_type == MSG_WRITE_RESP)
            rsp_credit_ok = available_wresp[rsp_rp] >= 12'(rsp_granules);
    end

    always_comb begin
        next_error = 1'b0;
        for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
            next_pending_wreq[rp] = pending_wreq[rp];
            next_pending_rreq[rp] = pending_rreq[rp];
            next_pending_wdata[rp] = pending_wdata[rp];
            next_available_rdata[rp] = available_rdata[rp];
            next_available_wresp[rp] = available_wresp[rp];
        end
        if (grant_valid && grant_ready) begin
            next_pending_wreq[grant_rp] = next_pending_wreq[grant_rp] - 12'(wreq_amount);
            next_pending_rreq[grant_rp] = next_pending_rreq[grant_rp] - 12'(rreq_amount);
            next_pending_wdata[grant_rp] = next_pending_wdata[grant_rp] - 12'(wdata_amount);
        end
        if (req_release0_valid && int'(req_release0_rp) < RP_COUNT) begin
            if (req_release0_write)
                next_pending_wreq[req_release0_rp] = next_pending_wreq[req_release0_rp] +
                                                     12'(req_release0_granules);
            else
                next_pending_rreq[req_release0_rp] = next_pending_rreq[req_release0_rp] +
                                                     12'(req_release0_granules);
        end else if (req_release0_valid) next_error = 1'b1;
        if (req_release1_valid && int'(req_release1_rp) < RP_COUNT) begin
            if (req_release1_write)
                next_pending_wreq[req_release1_rp] = next_pending_wreq[req_release1_rp] +
                                                     12'(req_release1_granules);
            else
                next_pending_rreq[req_release1_rp] = next_pending_rreq[req_release1_rp] +
                                                     12'(req_release1_granules);
        end else if (req_release1_valid) next_error = 1'b1;
        if (wdata_release_valid && int'(wdata_release_rp) < RP_COUNT)
            next_pending_wdata[wdata_release_rp] = next_pending_wdata[wdata_release_rp] +
                                                   12'(wdata_release_granules);
        else if (wdata_release_valid) next_error = 1'b1;

        if (header_credit_valid && int'(header_rp) < RP_COUNT) begin
            next_available_rdata[header_rp] = next_available_rdata[header_rp] +
                12'(decode_amount(header_rdata_enc));
            next_available_wresp[header_rp] = next_available_wresp[header_rp] +
                12'(decode_amount({1'b0, header_wresp_enc}));
        end
        if (misc_valid && misc_data[7:4] == MSG_MISC && misc_data[3:1] == 3'b100) begin
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                next_available_rdata[rp] = next_available_rdata[rp] +
                    12'(decode_amount(stream_get3(misc_data, 43 + 3 * rp)));
                next_available_wresp[rp] = next_available_wresp[rp] +
                    12'(decode_amount({1'b0, stream_get2(misc_data, 55 + 2 * rp)}));
            end
        end
        if (rsp_fire && int'(rsp_rp) >= RP_COUNT) next_error = 1'b1;
        else if (rsp_fire && rsp_type == MSG_READ_DATA) begin
            if (next_available_rdata[rsp_rp] < 12'(rsp_granules)) next_error = 1'b1;
            else next_available_rdata[rsp_rp] = next_available_rdata[rsp_rp] -
                                                 12'(rsp_granules);
        end else if (rsp_fire && rsp_type == MSG_WRITE_RESP) begin
            if (next_available_wresp[rsp_rp] < 12'(rsp_granules)) next_error = 1'b1;
            else next_available_wresp[rsp_rp] = next_available_wresp[rsp_rp] -
                                                 12'(rsp_granules);
        end
    end

    assign debug_available_rdata = available_rdata[0];
    assign debug_available_wresp = available_wresp[0];
    assign _unused = ^header_credit[8:0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            grant_rr <= '0;
            error <= 1'b0;
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                pending_wreq[rp] <= 12'(REQUEST_GRANULES);
                pending_rreq[rp] <= 12'(REQUEST_GRANULES);
                pending_wdata[rp] <= 12'(WDATA_GRANULES);
                available_rdata[rp] <= '0;
                available_wresp[rp] <= '0;
            end
        end else begin
            if (grant_valid && grant_ready)
                grant_rr <= 2'((int'(grant_rp) + 1) % RP_COUNT);
            error <= next_error;
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                pending_wreq[rp] <= next_pending_wreq[rp];
                pending_rreq[rp] <= next_pending_rreq[rp];
                pending_wdata[rp] <= next_pending_wdata[rp];
                available_rdata[rp] <= next_available_rdata[rp];
                available_wresp[rp] <= next_available_wresp[rp];
            end
        end
    end
    /* verilator lint_on WIDTHTRUNC */
endmodule
