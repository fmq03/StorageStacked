// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//----------------------------------------------------------------------------
// Interface: coralnpu_rvvi_monitor_if
// Description: Debug interface for rvvi signals
//----------------------------------------------------------------------------
interface coralnpu_rvvi_monitor_if #(
    parameter XLEN = 32,
    parameter FLEN = 32,
    parameter VLEN = 128
);

  //GPR register
  logic [(`XLEN-1):0] gpr_reg_val_pre[32];
  //vector register
  logic [(`VLEN-1):0] v_reg_val_pre[128];
  //float register
  logic [(`FLEN-1):0] f_reg_val_pre[32];
  //for Hazard check
  logic [4:0] rs1_pre;
  logic [4:0] rs2_pre;
  logic [4:0] rd_pre;
  logic [4:0] vs1_pre;
  logic [4:0] vs2_pre;
  logic [4:0] vd_pre;
  logic [4:0] fs1_pre;
  logic [4:0] fs2_pre;
  logic [4:0] fs3_pre;
  logic [4:0] fd_pre;
  logic [11:0] csr_pre;
  logic [4:0] prev_inst_name;
  //CSR register value if update
  logic [(`XLEN-1):0] mstatus;
  logic [(`XLEN-1):0] misa;
  logic [(`XLEN-1):0] mie;
  logic [(`XLEN-1):0] mtvec;
  logic [(`XLEN-1):0] mscratch;
  logic [(`XLEN-1):0] mepc;
  logic [(`XLEN-1):0] mcause;
  logic [(`XLEN-1):0] mtval;
  logic [(`XLEN-1):0] mcycle;
  logic [(`XLEN-1):0] mcycleh;
  logic [(`XLEN-1):0] minstret;
  logic [(`XLEN-1):0] minstreth;
  logic [(`XLEN-1):0] mvendorid;
  logic [(`XLEN-1):0] marchid;
  logic [(`XLEN-1):0] mimpid;
  logic [(`XLEN-1):0] mhartid;
  //vector csr
  logic [(`XLEN-1):0] vstart;
  logic [(`XLEN-1):0] vxsat;
  logic [(`XLEN-1):0] vxrm;
  logic [(`XLEN-1):0] vcsr;
  logic [(`XLEN-1):0] vl;
  logic [(`XLEN-1):0] vtype;
  logic [(`XLEN-1):0] vlenb;
  //rvvi vtype
  logic [0:0] vtype_vma;
  logic [0:0] vtype_vta;
  logic [2:0] vtype_vsew;
  logic [2:0] vtype_vlmul;
  logic [0:0] vtype_vill;
  //floating-point
  logic [(`FLEN-1):0] fflags;
  logic [(`FLEN-1):0] frm;
  logic [(`FLEN-1):0] fcsr;
  //custom csr
  logic [(`XLEN-1):0] mcontext0;
  logic [(`XLEN-1):0] mcontext1;
  logic [(`XLEN-1):0] mcontext2;
  logic [(`XLEN-1):0] mcontext3;
  logic [(`XLEN-1):0] mcontext4;
  logic [(`XLEN-1):0] mcontext5;
  logic [(`XLEN-1):0] mcontext6;
  logic [(`XLEN-1):0] mcontext7;
  logic [(`XLEN-1):0] mpc;
  logic [(`XLEN-1):0] msp;
  logic [(`XLEN-1):0] kisa;
  logic [(`XLEN-1):0] kscm0;
  logic [(`XLEN-1):0] kscm1;
  logic [(`XLEN-1):0] kscm2;
  logic [(`XLEN-1):0] kscm3;
  logic [(`XLEN-1):0] kscm4;
  //retire/trap instruction name,128 width for ASCII string display
  logic [127:0] insn[7:0];
  //load store
  logic [127:0] lsu_nf[7:0];
  logic [127:0] lsu_mop[7:0];
  logic [127:0] lsu_umop[7:0];
  logic [127:0] lsu_eew[7:0];
  //vm
  logic [0:0] vm;
  //vector register
  logic [4:0] vd_addr[7:0];
  logic [4:0] vs2_addr[7:0];
  logic [4:0] vs1_addr[7:0];
  logic [`VLEN-1:0] vs1_val[7:0];
  logic [`VLEN-1:0] vs2_val[7:0];
  logic [`VLEN-1:0] vd_val[7:0];
  logic [`VLEN-1:0] v0_val[7:0];
  //gpr register
  logic [4:0] rs3_addr[7:0];
  logic [4:0] rd_addr[7:0];
  logic [4:0] rs2_addr[7:0];
  logic [4:0] rs1_addr[7:0];
  logic [`XLEN-1:0] rs1_val[7:0];
  logic [`XLEN-1:0] rs2_val[7:0];
  logic [`XLEN-1:0] rs3_val[7:0];
  logic [`XLEN-1:0] rd_val[7:0];
  //floating register
  logic [4:0] fd_addr[7:0];
  logic [4:0] fs3_addr[7:0];
  logic [4:0] fs2_addr[7:0];
  logic [4:0] fs1_addr[7:0];
  logic [`FLEN-1:0] fs1_val[7:0];
  logic [`FLEN-1:0] fs2_val[7:0];
  logic [`FLEN-1:0] fs3_val[7:0];
  logic [`FLEN-1:0] fd_val[7:0];
  //vset{}vl{} configuration from instruction decode
  logic [0:0] vma;
  logic [0:0] vta;
  logic [2:0] vsew;
  logic [2:0] vlmul;  //0-LMUL1 1-LMUL2 2-LMUL4 3-LMUL8 6-LMUL1_4 7-LMUL1_2
  logic [4:0] avl;
  //csr register
  logic [4:0] csr_addr[7:0];
  logic [`XLEN-1:0] csr_val[7:0];
endinterface : coralnpu_rvvi_monitor_if
