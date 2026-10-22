// description: 
// 1. Instantiate rvv_backend_alu_unit and connect to ALU Reservation Station and ROB.
//
// feature list:
// 1. The number of ALU units (`NUM_ALU) is configurable.
// 2. The size of vector length (`VLEN) is configurable.

`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif

module rvv_backend_alu
(
  clk,
  rst_n,
  pop,
  uop_valid,
  uop,
  result_valid,
  result,
  result_ready,
  trap_flush_rvv
);

//
// interface signals
//
  // global signal
  input   logic                         clk;
  input   logic                         rst_n;
  // ALU RS to ALU unit
  output  logic       [`NUM_ALU-1:0]    pop;
  input   logic       [`NUM_ALU-1:0]    uop_valid;    
  input   ALU_RS_t    [`NUM_ALU-1:0]    uop;
  // submit ALU result to ROB
  output  logic       [`NUM_ALU-1:0]    result_valid;
  output  PU2ROB_t    [`NUM_ALU-1:0]    result;
  input   logic       [`NUM_ALU-1:0]    result_ready;
  // trap-flush
  input   logic                         trap_flush_rvv; 

//
// internal signals
//
  // multi-issue to ALU
  logic               [`NUM_ALU-1:0]    alu_valid;
  ALU_RS_t            [`NUM_ALU-1:0]    alu_uop;
  logic               [`NUM_ALU-1:0]    alu_pop;
  // for-loop
  genvar                                i;

//
// Instantiate 2 rvv_backend_alu_unit
//
  always_comb begin
    case(result_ready)
      2'b01: begin
        alu_valid[0]  = uop_valid[0]; 
        alu_valid[1]  = 'b0; 
        alu_uop[0]    = uop[0];
        alu_uop[1]    = 'b0;
        pop[0]        = alu_pop[0]; 
        pop[1]        = 'b0; 
      end
      2'b10: begin
        alu_valid[0]  = 'b0; 
        alu_valid[1]  = uop_valid[0] & (!uop[0].is_cmp); 
        alu_uop[0]    = 'b0;
        alu_uop[1]    = uop[0];
        pop[0]        = alu_pop[1]; 
        pop[1]        = 'b0; 
      end
      2'b11: begin
        alu_valid[0]  = uop_valid[0]; 
        alu_valid[1]  = uop_valid[1] & (!uop[1].is_cmp); 
        alu_uop[0]    = uop[0];
        alu_uop[1]    = uop[1];
        pop[0]        = alu_pop[0]; 
        pop[1]        = alu_pop[1]; 
      end
      default: begin
        alu_valid[0]  = 'b0; 
        alu_valid[1]  = 'b0; 
        alu_uop[0]    = 'b0;
        alu_uop[1]    = 'b0;
        pop[0]        = 'b0; 
        pop[1]        = 'b0; 
      end
    endcase
  end
  
  rvv_backend_alu_unit #(
    .CMP_SUPPORT    (1'b1)
  ) u_alu_cmp_unit (
    // inputs
    .clk            (clk),
    .rst_n          (rst_n),
    .alu_uop_valid  (alu_valid[0]),
    .alu_uop        (alu_uop[0]),
    .result_ready   (result_ready[0]),
    // outputs
    .pop_rs         (alu_pop[0]),
    .result_valid   (result_valid[0]),
    .result         (result[0]),
    // trap-flush
    .trap_flush_rvv (trap_flush_rvv)
  );

  // instantiate
  generate
    for (i=1;i<`NUM_ALU;i=i+1) begin: ALU_UNIT
      rvv_backend_alu_unit u_alu_unit
        (
          // inputs
          .clk            (clk),
          .rst_n          (rst_n),
          .alu_uop_valid  (alu_valid[i]),
          .alu_uop        (alu_uop[i]),
          .result_ready   (result_ready[i]),
          // outputs
          .pop_rs         (alu_pop[i]),
          .result_valid   (result_valid[i]),
          .result         (result[i]),
          // trap-flush
          .trap_flush_rvv (trap_flush_rvv)
        );
    end
  endgenerate

endmodule
