// IEEE arithmetic primitives from Berkeley HardFloat; RNE, tininess after rounding.
module ld_fp32_add(input [31:0] a, b, output [31:0] y, output [4:0] flags);
    wire [32:0] ra, rb, ry;
    fNToRecFN #(8,24) ca(a,ra), cb(b,rb);
    addRecFN #(8,24) op(1'b1,1'b0,ra,rb,3'b000,ry,flags);
    recFNToFN #(8,24) cy(ry,y);
endmodule
module ld_fp32_mul(input [31:0] a, b, output [31:0] y, output [4:0] flags);
    wire [32:0] ra, rb, ry;
    fNToRecFN #(8,24) ca(a,ra), cb(b,rb);
    mulRecFN #(8,24) op(1'b1,ra,rb,3'b000,ry,flags);
    recFNToFN #(8,24) cy(ry,y);
endmodule
