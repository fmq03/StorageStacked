module ld_radix_topk #(
    parameter N=32, parameter IW=$clog2(N)
) (
    input clk, rst_n,
    input in_valid, output in_ready,
    input [N*32-1:0] scores, input [N-1:0] eligible, input [5:0] k,
    output reg out_valid, input out_ready,
    output reg [N-1:0] selected,
    output reg [N*IW-1:0] indices,
    output reg [5:0] count
);
    reg busy;
    reg [31:0] keys[0:N-1];
    reg [N-1:0] available, contenders;
    reg [5:0] remaining;
    reg [4:0] bit_pos;
    reg [N-1:0] ones, survivors;
    integer winner, population, i;
    reg found;
    function automatic [31:0] ordered(input [31:0] f);
        reg [31:0] z;
        begin
            z=(f[30:0]==0) ? 32'b0 : f;
            ordered=z[31] ? ~z : (z ^ 32'h80000000);
        end
    endfunction
    assign in_ready= !busy && !out_valid;
    always @* begin
        ones=0; population=0;
        for (integer j=0;j<N;j=j+1) begin
            ones[j]=contenders[j] && keys[j][bit_pos];
            if (eligible[j]) population=population+1;
        end
        survivors=(|ones) ? ones : contenders;
        winner=0; found=0;
        for (integer j=0;j<N;j=j+1) begin
            if (survivors[j] && !found) begin winner=j; found=1; end
        end
    end
    always @(posedge clk) begin
        if (!rst_n) begin
            busy<=0; out_valid<=0; selected<=0; indices<=0; count<=0;
            available<=0; contenders<=0; remaining<=0; bit_pos<=31;
        end else begin
            if (out_valid && out_ready) out_valid<=0;
            if (in_valid && in_ready) begin
                for (i=0;i<N;i=i+1) keys[i]<=ordered(scores[i*32+:32]);
                available<=eligible; contenders<=eligible; bit_pos<=31;
                selected<=0; indices<=0; count<=0;
                remaining<= (k>population) ? 6'(population) : k;
                if (k==0 || population==0) out_valid<=1;
                else busy<=1;
            end else if (busy) begin
                if (bit_pos!=0) begin
                    contenders<=survivors; bit_pos<=bit_pos-1'b1;
                end else begin
                    selected[winner]<=1;
                    indices[count*IW+:IW]<=IW'(winner);
                    count<=count+1'b1;
                    if (remaining==1) begin busy<=0; out_valid<=1; end
                    else begin
                        available[winner]<=0;
                        contenders<=available & ~(N'(1) << winner);
                        bit_pos<=31; remaining<=remaining-1'b1;
                    end
                end
            end
        end
    end
endmodule
