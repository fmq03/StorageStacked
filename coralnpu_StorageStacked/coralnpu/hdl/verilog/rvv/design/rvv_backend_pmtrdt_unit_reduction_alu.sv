// description

`ifndef HDL_VERILOG_RVV_DESIGN_RVV_SVH
`include "rvv_backend.svh"
`endif
`ifndef RVV_ASSERT__SVH
`include "rvv_backend_sva.svh"
`endif
`ifndef PMTRDT_DEFINE_SVH
`include "rvv_backend_pmtrdt.svh"
`endif

module rvv_backend_pmtrdt_unit_reduction_alu
(
  src1,
  src2,
  ctrl,
  dst
);

// ---parameter definition--------------------------------------------
  parameter  ALU_WIDTH = 32;
  localparam ALU_BYTE = ALU_WIDTH/8;

// ---port definition-------------------------------------------------
  input [ALU_BYTE-1:0][7:0] src1, src2;
  input RDT_ALU_t           ctrl;
  output logic [ALU_BYTE-1:0][7:0] dst;

// ---internal signal definition--------------------------------------
  logic [ALU_BYTE-1:0][8:0] src1_tmp, src2_tmp;
  logic [ALU_BYTE-1:0]      cin, cout; // carry in, carry out
  logic [ALU_BYTE-1:0][7:0] sum_dst;
  logic [ALU_BYTE-1:0][7:0] and_dst,  or_dst, xor_dst;
  logic [ALU_BYTE-1:0]      lt; // less than

  logic [3:0] element_byte; 

  genvar i;

// ---code start------------------------------------------------------
  always_comb begin
    case (ctrl.vs2_eew)
      EEW64: element_byte = 4'h8;
      EEW32: element_byte = 4'h4;
      EEW16: element_byte = 4'h2;
      default: element_byte = 4'h1; //EEW8
    endcase
  end

  //src2_tmp data
  generate
    for (i=0; i<ALU_BYTE; i++) begin : gen_src2_tmp
      always_comb begin
        case(ctrl.uop_funct6)
          VREDMAX,
          VREDMIN: src2_tmp[i] = (i%element_byte)==(element_byte-1) ? {src2[i][7],src2[i]} : {1'b0,src2[i]}; 
          //VREDMAXU, VREDMINU,
          //VMUNARY0, VWRXUNARY0, VREDSUM, VWREDSUMU,
          //VWREDSUM, VREDAND, VREDOR, VREDXOR
          default: src2_tmp[i] = {1'b0, src2[i]}; 
        endcase
      end
    end
  endgenerate

  //src1_tmp data
  generate
    for (i=0; i<ALU_BYTE; i++) begin : gen_src1_tmp
      always_comb begin
        case(ctrl.uop_funct6)
          VREDMAX,
          VREDMIN: src1_tmp[i] = (i%element_byte)==(element_byte-1) ? ~{src1[i][7],src1[i]} : ~{1'b0,src1[i]}; 
          VREDMAXU,
          VREDMINU: src1_tmp[i] = ~{1'b0, src1[i]};
          //VMUNARY0, VWRXUNARY0, VREDSUM, VWREDSUMU,
          //VWREDSUM, VREDAND, VREDOR, VREDXOR
          default: src1_tmp[i] = {1'b0, src1[i]}; 
        endcase
      end
    end
  endgenerate

  // cin data
  generate
    always_comb begin
      case (ctrl.uop_funct6)
        VREDMAXU,
        VREDMAX,
        VREDMINU,
        VREDMIN: cin[0] = ~1'b0; 
        //VMUNARY0, VWRXUNARY0, VREDSUM, VWREDSUMU,
        //VWREDSUM, VREDAND, VREDOR, VREDXOR
        default: cin[0] = 1'b0; 
      endcase
    end
    for (i=1; i<ALU_BYTE; i++) begin : gen_cin
      always_comb begin
        case (ctrl.uop_funct6)
          VREDMAXU,
          VREDMAX,
          VREDMINU,
          VREDMIN: cin[i] = i%element_byte==0 ? ~1'b0 : ~cout[i-1]; 
          //VMUNARY0, VWRXUNARY0, VREDSUM, VWREDSUMU,
          //VWREDSUM, VREDAND, VREDOR, VREDXOR,
          default: cin[i] = i%element_byte==0 ? 1'b0 : cout[i-1]; 
        endcase
      end
    end //for (i=0; i<ALU_BYTE; i++) begin : gen_cin
  endgenerate

  generate
    for (i=0;i<ALU_BYTE;i++) begin : gen_byte_alu
      assign {cout[i],sum_dst[i]} = src2_tmp[i] + src1_tmp[i] + cin[i];
      assign and_dst[i] = src2[i][7:0] & src1[i][7:0];
      assign or_dst[i]  = src2[i][7:0] | src1[i][7:0];
      assign xor_dst[i] = src2[i][7:0] ^ src1[i][7:0];
      assign lt[i] = cout[i];
      always_comb begin
        case(ctrl.uop_funct6)
          VMUNARY0,
          VWRXUNARY0,
          VREDSUM,
          VWREDSUMU,
          VWREDSUM: dst[i] = sum_dst[i][7:0];
          VREDMAXU,
          VREDMAX: dst[i] = ~lt[(i/element_byte+1)*element_byte-1] ? src2[i][7:0] : src1[i][7:0];
          VREDMINU,
          VREDMIN: dst[i] = lt[(i/element_byte+1)*element_byte-1] ? src2[i][7:0] : src1[i][7:0];
          VREDAND: dst[i] = and_dst[i];
          VREDOR:  dst[i] = or_dst[i];
          default: dst[i] = xor_dst[i]; //VREDXOR
        endcase
      end
    end
  endgenerate

endmodule
