
`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif

module rvv_backend_decode_unit_de2
(
  lcmd_valid,
  lcmd,
  uop_index_remain,
  uop_valid,
  uop
);
//
// interface signals
//
  // CQ to Decoder unit signals
  input   logic                           lcmd_valid;
  input   LCMD_t                          lcmd;
  input   logic   [`UOP_INDEX_WIDTH-1:0]  uop_index_remain;
  
  // Decoder unit to Uops Queue signals
  output  logic       [`NUM_DE_UOP-1:0]   uop_valid;
  output  UOP_QUEUE_t [`NUM_DE_UOP-1:0]   uop;

//
// internal signals
//
  logic                                   valid_ari;
  logic                                   valid_lsu;
  // decoded arithmetic uops
  logic           [`NUM_DE_UOP-1:0]       uop_valid_ari;
  UOP_QUEUE_t     [`NUM_DE_UOP-1:0]       uop_ari;
  // decoded LSU uops
  logic           [`NUM_DE_UOP-1:0]       uop_valid_lsu;
  UOP_QUEUE_t     [`NUM_DE_UOP-1:0]       uop_lsu;

//
// decode
//
  // decode opcode
  assign valid_lsu  = lcmd_valid & ((lcmd.cmd.opcode==LOAD) | (lcmd.cmd.opcode==STORE));
  assign valid_ari  = lcmd_valid & (lcmd.cmd.opcode==RVV);
  
  // decode LSU instruction 
  rvv_backend_decode_unit_lsu_de2 u_lsu_decode_de2
  (
    .lcmd_valid         (valid_lsu),
    .lcmd               (lcmd),
    .uop_index_remain   (uop_index_remain),
    .uop_valid          (uop_valid_lsu),
    .uop                (uop_lsu)  
  );

  // decode arithmetic instruction
  rvv_backend_decode_unit_ari_de2 u_ari_decode_de2
  (
    .lcmd_valid         (valid_ari),
    .lcmd               (lcmd),
    .uop_index_remain   (uop_index_remain),
    .uop_valid          (uop_valid_ari),
    .uop                (uop_ari)  
  );

  // output
  always_comb begin 
    uop_valid     = 'b0;
    uop           = 'b0;
    
    case(1'b1)
      valid_lsu: begin
        uop_valid = uop_valid_lsu;
        uop       = uop_lsu;
      end
  
      valid_ari: begin
        uop_valid = uop_valid_ari;
        uop       = uop_ari;
      end
    endcase
  end

endmodule
