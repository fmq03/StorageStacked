
module rvv_backend_mul_unit_mul8 (
  res,
  src0, 
  src0_is_signed,
  src1, 
  src1_is_signed
);

parameter MUL_WIDTH = `BYTE_WIDTH;

input   [MUL_WIDTH-1:0]   src0;
input                     src0_is_signed;
input   [MUL_WIDTH-1:0]   src1;
input                     src1_is_signed;
output  [2*MUL_WIDTH-1:0] res;

logic                     src0_sgn;
logic                     src1_sgn;

assign src0_sgn = src0_is_signed&src0[MUL_WIDTH-1];
assign src1_sgn = src1_is_signed&src1[MUL_WIDTH-1];

assign res = {{MUL_WIDTH{src0_sgn}}, src0} * {{MUL_WIDTH{src1_sgn}}, src1};

endmodule
