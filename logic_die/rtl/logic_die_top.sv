// Memory-side descriptor engine: PREPARE=1, QUERY=2, PREPARE_QUERY=3.
module logic_die_top #(
    parameter CHUNKS=32, parameter DIM=128, parameter LANES=4,
    parameter CW=$clog2(CHUNKS), parameter DW=$clog2(DIM),
    parameter BANK_WORDS=((CHUNKS+LANES-1)/LANES)*DIM,
    parameter BW=$clog2(BANK_WORDS)
) (
    input clk, rst_n,
    input csr_we, input [9:0] csr_addr, input [31:0] csr_wdata,
    input [3:0] csr_wstrb, output reg [31:0] csr_rdata, output csr_error, output csr_stall,
    output dma_valid, input dma_ready, output [31:0] dma_addr,
    input dma_rsp_valid, output dma_rsp_ready,
    input [255:0] dma_rsp_data, input dma_rsp_error,
    output busy, output reg done, output reg error,
    output reg [31:0] result_mask, output reg [5:0] result_count,
    output [5:0] debug_state
);
    typedef enum logic [5:0] {
        IDLE, P_CLEAR, K_REQ, K_WAIT, K_EAT, P_MEAN,
        Q_REQ, Q_WAIT, Q_EAT, S_READ, S_MUL, S_ACC, S_STORE,
        TOP_START, TOP_WAIT
    } state_t;
    state_t state;
    reg [31:0] k_base, q_base, epoch, resident_epoch, resident_base;
    reg [5:0] chunk_count, top_k, current_chunk, resident_chunks;
    reg [3:0] chunk_log2, resident_log2;
    reg resident, run_query;
    reg [31:0] next_addr, cycles, pool_cycles, score_cycles, top_cycles, dma_beats;
    reg [31:0] pool[0:DIM-1], accum[0:LANES-1], product[0:LANES-1];
    reg [15:0] query[0:DIM-1];
    reg [31:0] score[0:CHUNKS-1];
    reg [CW-1:0] chunk;
    reg [5:0] group_base;
    reg [DW-1:0] dim_index;
    reg [11:0] token_index;
    reg [3:0] line_index;
    reg [255:0] line_data;
    reg [CHUNKS*CW-1:0] result_indices;
    wire [15:0] scalar=line_data[line_index*16+:16];
    wire [31:0] pooled, scaled;
    wire [4:0] pool_flags, scale_flags;
    wire [7:0] scale_exp=8'd127-{4'b0,chunk_log2};
    ld_fp32_add pool_op(pool[dim_index],{scalar,16'b0},pooled,pool_flags);
    ld_fp32_mul scale_op(pool[dim_index],{1'b0,scale_exp,23'b0},scaled,scale_flags);
    function automatic [15:0] bf16_rne(input [31:0] f);
        reg [31:0] rounded;
        begin
            rounded=f+32'h00007fff+{31'b0,f[16]};
            bf16_rne=((&f[30:23]) && (|f[22:0])) ? 16'h7fc0 : rounded[31:16];
        end
    endfunction
    wire [BW-1:0] wa=BW'((int'(chunk)/LANES)*DIM+int'(dim_index));
    wire [BW-1:0] ra=BW'((int'(group_base)/LANES)*DIM+int'(dim_index));
    wire [31:0] mul_y[0:LANES-1], add_y[0:LANES-1];
    wire [4:0] mul_flags[0:LANES-1], add_flags[0:LANES-1];
    wire [15:0] weight[0:LANES-1];
    reg dot_bad;
    always @* begin
        dot_bad=0;
        for(integer j=0;j<LANES;j=j+1)
            if(int'(group_base)+j<int'(current_chunk) && (&add_y[j][30:23])) dot_bad=1;
    end
    genvar g;
    generate for(g=0;g<LANES;g=g+1) begin: cells
        ld_weight_bank #(BANK_WORDS,BW) weights(
            .clk(clk), .we(state==P_MEAN && int'(chunk)%LANES==g),
            .waddr(wa), .wdata(bf16_rne(scaled)), .re(state==S_READ),
            .raddr(ra), .rdata(weight[g]));
        ld_fp32_mul mult({weight[g],16'b0},{query[dim_index],16'b0},mul_y[g],mul_flags[g]);
        ld_fp32_add add(accum[g],product[g],add_y[g],add_flags[g]);
    end endgenerate

    wire [CHUNKS*32-1:0] score_bus;
    wire [CHUNKS-1:0] eligible;
    generate for(g=0;g<CHUNKS;g=g+1) begin: score_pack
        assign score_bus[g*32+:32]=score[g];
        assign eligible[g]=(g<current_chunk);
    end endgenerate
    wire top_ready, top_valid;
    wire [CHUNKS-1:0] selected;
    wire [CHUNKS*CW-1:0] top_indices;
    wire [5:0] selected_count;
    wire [5:0] k_select= (top_k-1'b1 < current_chunk) ? top_k-1'b1 : current_chunk;
    ld_radix_topk #(CHUNKS,CW) topk(
        .clk(clk),.rst_n(rst_n),.in_valid(state==TOP_START),.in_ready(top_ready),
        .scores(score_bus),.eligible(eligible),.k(k_select),
        .out_valid(top_valid),.out_ready(state==TOP_WAIT),.selected(selected),
        .indices(top_indices),.count(selected_count));
    assign busy=(state!=IDLE);
    assign debug_state=state;
    assign dma_valid=(state==K_REQ || state==Q_REQ);
    assign dma_addr=next_addr;
    assign dma_rsp_ready=(state==K_WAIT || state==Q_WAIT);
    // Completion reads reserve a response slot while local DMA keeps progressing.
    assign csr_stall=busy && csr_addr>=10'h300 && csr_addr<10'h340;
    wire config_bad=chunk_count==0 || chunk_count>CHUNKS || top_k==0 ||
        top_k>CHUNKS || current_chunk>=chunk_count || chunk_log2>12 ||
        k_base[4:0]!=0 || q_base[4:0]!=0;
    wire query_bad= !resident || resident_epoch!=epoch || resident_base!=k_base ||
        resident_chunks!=chunk_count || resident_log2!=chunk_log2;
    wire write_addr_ok=csr_addr==0 || csr_addr==4 || csr_addr==8 ||
        csr_addr==12 || csr_addr==16 || csr_addr==20 || csr_addr==24 || csr_addr==28;
    wire config_write_bad=(csr_addr==12 && (csr_wdata==0 || csr_wdata>CHUNKS)) ||
        (csr_addr==16 && csr_wdata>12) ||
        (csr_addr==20 && (csr_wdata==0 || csr_wdata>CHUNKS)) ||
        (csr_addr==24 && csr_wdata>=CHUNKS);
    assign csr_error=csr_we && (busy || csr_wstrb!=4'hf || !write_addr_ok || config_write_bad ||
        (csr_addr==0 && (csr_wdata<1 || csr_wdata>3 || config_bad ||
        (csr_wdata==2 && query_bad))));
    always @* begin
        csr_rdata=0;
        case(csr_addr)
            0: csr_rdata={28'b0,resident,error,done,busy};
            4: csr_rdata=k_base; 8: csr_rdata=q_base; 12: csr_rdata={26'b0,chunk_count};
            16: csr_rdata={28'b0,chunk_log2}; 20: csr_rdata={26'b0,top_k};
            24: csr_rdata={26'b0,current_chunk}; 28: csr_rdata=epoch;
            32: csr_rdata=resident_epoch;
            36: csr_rdata=cycles; 40: csr_rdata=pool_cycles; 44: csr_rdata=score_cycles;
            48: csr_rdata=top_cycles; 52: csr_rdata=dma_beats;
            64: csr_rdata=result_mask; 68: csr_rdata={26'b0,result_count};
            768: csr_rdata={28'b0,resident,error,done,busy};
            772: csr_rdata=result_mask; 776: csr_rdata={26'b0,result_count};
            780: csr_rdata=cycles; 784: csr_rdata=dma_beats;
            default: if(csr_addr>=128 && csr_addr<128+CHUNKS*4 && csr_addr[1:0]==0)
                csr_rdata={{(32-CW){1'b0}},result_indices[((csr_addr-128)/4)*CW+:CW]};
        endcase
    end
    integer i;
    always @(posedge clk) begin
        if (!rst_n) begin
            state<=IDLE; done<=0; error<=0; resident<=0; result_mask<=0; result_count<=0;
            k_base<=0; q_base<=0; epoch<=0; resident_epoch<=0; resident_base<=0;
            chunk_count<=6'(CHUNKS); chunk_log2<=5; top_k<=12; current_chunk<=6'(CHUNKS-1);
            resident_chunks<=0; resident_log2<=0; run_query<=0;
            next_addr<=0; chunk<=0; dim_index<=0; token_index<=0; line_index<=0;
            group_base<=0; line_data<=0; result_indices<=0;
            cycles<=0; pool_cycles<=0; score_cycles<=0; top_cycles<=0; dma_beats<=0;
            for(i=0;i<LANES;i=i+1) begin accum[i]<=0; product[i]<=0; end
        end else begin
            if(busy) cycles<=cycles+1'b1;
            if(state>=P_CLEAR && state<=P_MEAN) pool_cycles<=pool_cycles+1'b1;
            if(state>=Q_REQ && state<=S_STORE) score_cycles<=score_cycles+1'b1;
            if(state==TOP_START || state==TOP_WAIT) top_cycles<=top_cycles+1'b1;
            if(csr_we && !csr_error) begin
                case(csr_addr)
                    4:k_base<=csr_wdata; 8:q_base<=csr_wdata;
                    12:chunk_count<=csr_wdata[5:0]; 16:chunk_log2<=csr_wdata[3:0];
                    20:top_k<=csr_wdata[5:0]; 24:current_chunk<=csr_wdata[5:0]; 28:epoch<=csr_wdata;
                    0:begin
                        done<=0; error<=0; cycles<=0; pool_cycles<=0; score_cycles<=0; top_cycles<=0;
                        dma_beats<=0; result_mask<=0; result_count<=0; result_indices<=0;
                        run_query<=(csr_wdata==3); dim_index<=0; token_index<=0; chunk<=0;
                        if(csr_wdata==2) begin state<=Q_REQ; next_addr<=q_base; end
                        else begin state<=P_CLEAR; resident<=0; next_addr<=k_base; end
                    end
                    default:begin end
                endcase
            end
            case(state)
                IDLE:begin end
                P_CLEAR:begin
                    pool[dim_index]<=0;
                    if(dim_index==DIM-1) begin dim_index<=0; token_index<=0; state<=K_REQ; end
                    else dim_index<=dim_index+1'b1;
                end
                K_REQ:if(dma_ready) begin state<=K_WAIT; next_addr<=next_addr+32; dma_beats<=dma_beats+1'b1; end
                Q_REQ:if(dma_ready) begin state<=Q_WAIT; next_addr<=next_addr+32; dma_beats<=dma_beats+1'b1; end
                K_WAIT,Q_WAIT:if(dma_rsp_valid) begin
                    if(dma_rsp_error) begin state<=IDLE; error<=1; done<=1; resident<=0; end
                    else begin line_data<=dma_rsp_data; line_index<=0; state<=(state==K_WAIT)?K_EAT:Q_EAT; end
                end
                K_EAT:begin
                    if((&scalar[14:7]) || (&pooled[30:23])) begin state<=IDLE; error<=1; done<=1; resident<=0; end
                    else begin
                        pool[dim_index]<=pooled; line_index<=line_index+1'b1;
                        dim_index<=dim_index+1'b1;
                        if(dim_index==DIM-1) begin
                            dim_index<=0;
                            if({1'b0,token_index}+1'b1==(13'd1 << chunk_log2)) begin state<=P_MEAN; end
                            else begin token_index<=token_index+1'b1; state<=K_REQ; end
                        end else if(line_index==15) state<=K_REQ;
                    end
                end
                P_MEAN:begin
                    if(dim_index==DIM-1) begin
                        dim_index<=0;
                        if({1'b0,chunk}+1'b1==chunk_count) begin
                            resident<=1; resident_epoch<=epoch; resident_base<=k_base;
                            resident_chunks<=chunk_count; resident_log2<=chunk_log2;
                            if(run_query) begin state<=Q_REQ; next_addr<=q_base; end
                            else begin state<=IDLE; done<=1; end
                        end else begin chunk<=chunk+1'b1; state<=P_CLEAR; end
                    end else dim_index<=dim_index+1'b1;
                end
                Q_EAT:begin
                    if(&scalar[14:7]) begin state<=IDLE; error<=1; done<=1; end
                    else begin
                        query[dim_index]<=scalar; dim_index<=dim_index+1'b1; line_index<=line_index+1'b1;
                        if(dim_index==DIM-1) begin
                            dim_index<=0; group_base<=0;
                            for(i=0;i<LANES;i=i+1) accum[i]<=0;
                            state<=current_chunk==0 ? TOP_START : S_READ;
                        end else if(line_index==15) state<=Q_REQ;
                    end
                end
                S_READ:state<=S_MUL;
                S_MUL:begin
                    for(i=0;i<LANES;i=i+1) product[i]<=mul_y[i];
                    state<=S_ACC;
                end
                S_ACC:begin
                    if(dot_bad) begin state<=IDLE; error<=1; done<=1; end
                    else begin
                        for(i=0;i<LANES;i=i+1) accum[i]<=add_y[i];
                        if(dim_index==DIM-1) state<=S_STORE;
                        else begin dim_index<=dim_index+1'b1; state<=S_READ; end
                    end
                end
                S_STORE:begin
                    for(i=0;i<LANES;i=i+1) begin
                        if(int'(group_base)+i<CHUNKS) score[int'(group_base)+i]<=accum[i];
                        accum[i]<=0;
                    end
                    dim_index<=0;
                    if(int'(group_base)+LANES>=int'(current_chunk)) state<=TOP_START;
                    else begin group_base<=group_base+6'(LANES); state<=S_READ; end
                end
                TOP_START:if(top_ready) state<=TOP_WAIT;
                TOP_WAIT:if(top_valid) begin
                    result_mask<=32'(selected) | (32'd1<<current_chunk);
                    result_count<=selected_count+1'b1;
                    result_indices[0+:CW]<=current_chunk[CW-1:0];
                    for(i=1;i<CHUNKS;i=i+1) result_indices[i*CW+:CW]<=top_indices[(i-1)*CW+:CW];
                    state<=IDLE; done<=1;
                end
                default:begin state<=IDLE; error<=1; done<=1; resident<=0; end
            endcase
        end
    end
endmodule
