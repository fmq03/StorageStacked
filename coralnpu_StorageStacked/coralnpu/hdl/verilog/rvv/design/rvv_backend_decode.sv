
`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif

module rvv_backend_decode
(
  inst_valid,
  inst,
  lcmd_valid,
  lcmd
);
//
// interface signals
//
  input   logic   [`NUM_DE_INST-1:0]  inst_valid; 
  input   RVVCmd  [`NUM_DE_INST-1:0]  inst; 

  output  logic   [`NUM_DE_INST-1:0]  lcmd_valid;
  output  LCMD_t  [`NUM_DE_INST-1:0]  lcmd;

//
// decode
//
  genvar                              i;

  // decode unit
  generate 
    for (i=0;i<`NUM_DE_INST;i=i+1) begin: DECODE_UNIT
      rvv_backend_decode_unit u_decode_unit
      (
        .inst_valid   (inst_valid[i]),
        .inst         (inst[i]),
        .lcmd_valid   (lcmd_valid[i]),
        .lcmd         (lcmd[i])  
      );    
    end
  endgenerate
  
endmodule
