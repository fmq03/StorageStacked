
`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif

module rvv_backend_decode_de2
(
  clk, 
  rst_n,
  lcmd_valid,
  lcmd,
  pop,
  push,
  uop,
  uq_ready,
  trap_flush_rvv 
);
//
// interface signals
//
  // global signal
  input   logic                         clk;
  input   logic                         rst_n;
  // signals from command queue
  input   logic   [`NUM_DE_INST-1:0]    lcmd_valid;
  input   LCMD_t  [`NUM_DE_INST-1:0]    lcmd;
  output  logic   [`NUM_DE_INST-1:0]    pop;
  // signals from Uops Quue
  output  logic   [`NUM_DE_UOP-1:0]     push;
  output  UOP_QUEUE_t [`NUM_DE_UOP-1:0] uop;
  input   logic   [`NUM_DE_UOP-1:0]     uq_ready;
  // trap-flush
  input   logic                         trap_flush_rvv;  

//
// internal signals
//
  // the decoded uops
  logic       [`NUM_DE_INST-1:0][`NUM_DE_UOP-1:0] de_uop_valid;
  UOP_QUEUE_t [`NUM_DE_INST-1:0][`NUM_DE_UOP-1:0] de_uop;
  // uop index from controller
  logic       [`UOP_INDEX_WIDTH-1:0]              uop_index_remain;
  // for-loop
  genvar                                          i;

//
// decode
//
  // decode unit
  rvv_backend_decode_unit_de2 u_decode_unit0_de2
  (
    .lcmd_valid             (lcmd_valid[0]),
    .lcmd                   (lcmd[0]),
    .uop_index_remain       (uop_index_remain),
    .uop_valid              (de_uop_valid[0]),
    .uop                    (de_uop[0])
  );
   
  generate 
    for (i=1;i<`NUM_DE_INST;i=i+1) begin: DECODE_UNIT
      rvv_backend_decode_unit_de2 u_decode_unit_de2
      (
        .lcmd_valid         (lcmd_valid[i]),
        .lcmd               (lcmd[i]),
        .uop_index_remain   ({`UOP_INDEX_WIDTH{1'b0}}),
        .uop_valid          (de_uop_valid[i]),
        .uop                (de_uop[i])
      );    
    end
  endgenerate
  
  // decode controller
  rvv_backend_decode_ctrl u_decode_ctrl
  (
    .clk                    (clk),
    .rst_n                  (rst_n),
    .de_uop_valid           (de_uop_valid),
    .de_uop                 (de_uop),
    .uop_index_remain       (uop_index_remain),
    .pop                    (pop),
    .push                   (push),
    .uop                    (uop),
    .uq_ready               (uq_ready),
    .trap_flush_rvv         (trap_flush_rvv)
  );

endmodule
