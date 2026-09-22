module bridge_assertions #(
    parameter int DATA_W = 256,
    parameter int MC_TAG_W = 32,
    parameter int MC_BYTES = 32,
    parameter int RP_COUNT = 1,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MC_INFLIGHT = 8,
    parameter int REQUEST_FIFO_DEPTH = 4,
    parameter int WDATA_FIFO_DEPTH = 16
) (
    input logic clk,
    input logic rst_n,
    input logic mc_req_valid,
    input logic mc_req_ready,
    input logic mc_req_write,
    input logic [63:0] mc_req_addr,
    input logic [5:0] mc_req_bytes,
    input logic [DATA_W-1:0] mc_req_data,
    input logic [DATA_W/8-1:0] mc_req_byte_mask,
    input logic [MC_TAG_W-1:0] mc_req_tag,
    input logic [3:0] mc_req_qos,
    input logic rsp_msg_valid,
    input logic rsp_msg_ready,
    input logic [5:0] rsp_msg_granules,
    input logic [bridge_pkg::MSG_MAX_W-1:0] rsp_msg_data,
    input logic [$clog2(MAX_OUTSTANDING+1)-1:0] response_count,
    input logic [$clog2(MC_INFLIGHT+1)-1:0] child_count,
    input logic [$clog2(REQUEST_FIFO_DEPTH+1)-1:0]
        req_fifo_count [0:RP_COUNT-1],
    input logic [$clog2(WDATA_FIFO_DEPTH+1)-1:0]
        wdata_fifo_count [0:RP_COUNT-1],
    input logic [$clog2(MAX_OUTSTANDING+1)-1:0]
        response_rp_count [0:RP_COUNT-1],
    input logic [$clog2(MAX_OUTSTANDING+1)-1:0]
        write_count [0:RP_COUNT-1],
    input logic child_valid [0:MC_INFLIGHT-1],
    input logic [MC_TAG_W-1:0] child_tag [0:MC_INFLIGHT-1]
);
`ifndef SYNTHESIS
    property p_mc_control_stable;
        @(posedge clk) disable iff (!rst_n)
        mc_req_valid && !mc_req_ready |=>
            $stable({mc_req_write, mc_req_addr, mc_req_bytes, mc_req_qos});
    endproperty
    assert property (p_mc_control_stable) else $error("stalled MC control changed");

    property p_mc_data_stable;
        @(posedge clk) disable iff (!rst_n)
        mc_req_valid && !mc_req_ready |=> $stable(mc_req_data);
    endproperty
    assert property (p_mc_data_stable) else $error("stalled MC data changed");

    property p_mc_mask_stable;
        @(posedge clk) disable iff (!rst_n)
        mc_req_valid && !mc_req_ready |=> $stable(mc_req_byte_mask);
    endproperty
    assert property (p_mc_mask_stable) else $error("stalled MC mask changed");

    property p_mc_tag_stable;
        @(posedge clk) disable iff (!rst_n)
        mc_req_valid && !mc_req_ready |=> $stable(mc_req_tag);
    endproperty
    assert property (p_mc_tag_stable) else $error("stalled MC tag changed");

    property p_rsp_msg_stable;
        @(posedge clk) disable iff (!rst_n)
        rsp_msg_valid && !rsp_msg_ready |=>
            $stable({rsp_msg_granules, rsp_msg_data});
    endproperty
    assert property (p_rsp_msg_stable);

    property p_capacity;
        @(posedge clk) disable iff (!rst_n)
        response_count <= $clog2(MAX_OUTSTANDING+1)'(MAX_OUTSTANDING) &&
        child_count <= $clog2(MC_INFLIGHT+1)'(MC_INFLIGHT);
    endproperty
    assert property (p_capacity);

    property p_mc_shape;
        @(posedge clk) disable iff (!rst_n)
        mc_req_valid |-> mc_req_bytes > 0 &&
            mc_req_bytes <= 6'(MC_BYTES) &&
            (mc_req_addr % 64'(MC_BYTES)) + 64'(mc_req_bytes) <=
                64'(MC_BYTES);
    endproperty
    assert property (p_mc_shape);

    always_ff @(posedge clk) begin
        if (rst_n) begin
            for (int rp = 0; rp < RP_COUNT; rp = rp + 1) begin
                assert (req_fifo_count[rp] <=
                        $clog2(REQUEST_FIFO_DEPTH+1)'(REQUEST_FIFO_DEPTH));
                assert (wdata_fifo_count[rp] <=
                        $clog2(WDATA_FIFO_DEPTH+1)'(WDATA_FIFO_DEPTH));
                assert (response_rp_count[rp] <=
                        $clog2(MAX_OUTSTANDING+1)'(MAX_OUTSTANDING));
                assert (write_count[rp] <=
                        $clog2(MAX_OUTSTANDING+1)'(MAX_OUTSTANDING));
            end
            for (int a = 0; a < MC_INFLIGHT; a = a + 1)
                for (int b = a + 1; b < MC_INFLIGHT; b = b + 1)
                    assert (!(child_valid[a] && child_valid[b] &&
                              child_tag[a] == child_tag[b]));
        end
    end
`endif
endmodule
