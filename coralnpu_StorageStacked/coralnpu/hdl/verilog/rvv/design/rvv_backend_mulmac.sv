// description: 
// This is the top module of MUL/MAC wrapper
// Contains instantiation of MUL_ex and MAC_ex
// Contains ex arbiter
//
// feature list:
// 1. Instantiation of MUL ex and MAC ex
// 2. Arbitration to uop0/1 to use MUL or MAC
// 3. Pop MUL_RS 


`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif

module rvv_backend_mulmac (
  clk,
  rst_n,
  trap_flush_rvv,
  uop_valid_rs2ex,
  mac_uop_rs2ex,
  pop,
  res_valid_ex2rob,
  res_ex2rob,
  res_ready_rob2ex
);

//global signals
input   logic                     clk;
input   logic                     rst_n;
input   logic                     trap_flush_rvv;

//MUL_RS to MUL_EX 
input   logic     [`NUM_MUL-1:0]  uop_valid_rs2ex;
input   MUL_RS_t  [`NUM_MUL-1:0]  mac_uop_rs2ex;
output  logic     [`NUM_MUL-1:0]  pop;

//MUL_EX to ROB
output  logic     [`NUM_MUL-1:0]  res_valid_ex2rob;
output  PU2ROB_t  [`NUM_MUL-1:0]  res_ex2rob;
input   logic     [`NUM_MUL-1:0]  res_ready_rob2ex;

// Wires & Regs
logic             [`NUM_MUL-1:0]  mac_valid;
MUL_RS_t          [`NUM_MUL-1:0]  mac_uop;
logic             [`NUM_MUL-1:0]  mac_ready;
logic             [`NUM_MUL-1:0]  mac_pipe_vld_en;
logic             [`NUM_MUL-1:0]  mac_pipe_data_en;

genvar                            i;

// handshake
assign mac_ready = ~res_valid_ex2rob | res_ready_rob2ex;

always_comb begin
  case(mac_ready)
    2'b01: begin
      mac_valid[0]  = uop_valid_rs2ex[0];
      mac_valid[1]  = 'b0;
      mac_uop[0]    = mac_uop_rs2ex[0];
      mac_uop[1]    = 'b0;
      pop[0]        = uop_valid_rs2ex[0];
      pop[1]        = 'b0;
    end
    2'b10: begin
      mac_valid[0]  = 'b0;
      mac_valid[1]  = uop_valid_rs2ex[0];
      mac_uop[0]    = 'b0;
      mac_uop[1]    = mac_uop_rs2ex[0];
      pop[0]        = uop_valid_rs2ex[0];
      pop[1]        = 'b0;
    end
    2'b11: begin
      mac_valid[0]  = uop_valid_rs2ex[0];
      mac_valid[1]  = uop_valid_rs2ex[1];
      mac_uop[0]    = mac_uop_rs2ex[0];
      mac_uop[1]    = mac_uop_rs2ex[1];
      pop[0]        = uop_valid_rs2ex[0];
      pop[1]        = uop_valid_rs2ex[1];
    end
    default: begin
      mac_valid[0]  = 'b0;
      mac_valid[1]  = 'b0;
      mac_uop[0]    = 'b0;
      mac_uop[1]    = 'b0;
      pop[0]        = 'b0;
      pop[1]        = 'b0;
    end
  endcase
end

// pipe register enable
assign mac_pipe_vld_en  = mac_valid | res_valid_ex2rob&res_ready_rob2ex;  // enable for pipeline vld register
assign mac_pipe_data_en = mac_valid;                                      // enable for pipeline data register

// Inst of MAC-ex
generate
  for(i=0;i<`NUM_MUL;i++) begin: INST_MAC
    rvv_backend_mac_unit #(
    ) u_mac (
      // Outputs
      .mac2rob_uop_valid  (res_valid_ex2rob[i]),
      .mac2rob_uop_data   (res_ex2rob[i]),
      // Inputs
      .clk                (clk), 
      .rst_n              (rst_n), 
      .rs2mac_uop_valid   (mac_valid[i]), 
      .rs2mac_uop_data    (mac_uop[i]),
      .mac_pipe_vld_en    (mac_pipe_vld_en[i]),
      .mac_pipe_data_en   (mac_pipe_data_en[i]),
      .trap_flush_rvv     (trap_flush_rvv)
    );
  end

endgenerate

endmodule
