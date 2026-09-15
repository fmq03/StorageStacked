// Local implementation of the CLZ interface required by the pinned BSG fork.
module bsg_counting_leading_zeros #(
    parameter width_p=23, parameter count_width_p=$clog2(width_p+1)
) (input [width_p-1:0] a_i, output reg [count_width_p-1:0] num_zero_o);
    reg found;
    always @* begin
        num_zero_o=count_width_p'(width_p); found=0;
        for(integer i=width_p-1;i>=0;i=i-1) begin
            if(a_i[i] && !found) begin num_zero_o=count_width_p'(width_p-1-i); found=1; end
        end
    end
endmodule
