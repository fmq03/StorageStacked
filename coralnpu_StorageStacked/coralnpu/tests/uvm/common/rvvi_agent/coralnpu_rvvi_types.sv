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
// Description: Enumeration variable
//----------------------------------------------------------------------------
typedef enum {
  INSN_UNSET,
  // I ISA
  ADD,
  SUB,
  XOR,
  OR,
  AND,
  SLL,
  SRL,
  SRA,
  SLT,
  SLTU,
  ADDI,
  XORI,
  ORI,
  ANDI,
  SLLI,
  SRLI,
  SRAI,
  SLTI,
  SLTIU,
  LB,
  LH,
  LW,
  LBU,
  LHU,
  SB,
  SH,
  SW,
  BEQ,
  BNE,
  BLT,
  BGE,
  BLTU,
  BGEU,
  JAL,
  JALR,
  LUI,
  AUIPC,
  ECALL,
  EBREAK,
  FENCE,
  V_INSTR,
  FP_INSTR,
  M_INSTR,
  CSR_INSTR,
  Zbb_INSTR,
  Zifencei_INSTR,
  MPAUSE
} insn_name_enum;

typedef enum {
  FORMAT_UNSET,
  R_Type,
  I_Type,
  S_Type,
  B_Type,
  U_Type,
  J_Type,
  Fence_Type,
  VSET_Type,
  ALU_Type,
  LD_Type,
  ST_Type,
  FP_Type,
  M_Type,
  CSR_Type,
  Zbb_Type,
  USER_DEFINED
} fmt_typ_enum;

typedef enum {
  ISA_UNSET,
  RV32I,
  RV32M,
  RV32F,
  RV32Zicsr,
  RV32Zbb,
  RV32Zifencei,
  RV32V,
  Custom
} isa_enum;

//list as feature list shown
typedef enum logic [11:0] {
  //floating point register
  fflags    = 12'h001,
  frm       = 12'h002,
  fcsr      = 12'h003,
  //privileged CSR register
  mstatus   = 12'h300,
  misa      = 12'h301,
  mie       = 12'h304,
  mtvec     = 12'h305,
  mscratch  = 12'h340,
  mepc      = 12'h341,
  mcause    = 12'h342,
  mtval     = 12'h343,
  mcycle    = 12'hb00,
  mcycleh   = 12'hb80,
  minstret  = 12'hb02,
  minstreth = 12'hb82,
  mvendorid = 12'hf11,
  marchid   = 12'hf12,
  mimpid    = 12'hf13,
  mhartid   = 12'hf14,
  //custom register
  mcontext0 = 12'h7c0,
  mcontext1 = 12'h7c1,
  mcontext2 = 12'h7c2,
  mcontext3 = 12'h7c3,
  mcontext4 = 12'h7c4,
  mcontext5 = 12'h7c5,
  mcontext6 = 12'h7c6,
  mcontext7 = 12'h7c7,
  mpc       = 12'h7e0,
  msp       = 12'h7e1,
  kisa      = 12'hfc0,
  kscm0     = 12'hfc4,
  kscm1     = 12'hfc8,
  kscm2     = 12'hfcc,
  kscm3     = 12'hfd0,
  kscm4     = 12'hfd4,
  //vector csr
  vstart    = 12'h008,
  vxsat     = 12'h009,
  vxrm      = 12'h00a,
  vcsr      = 12'h00f,
  vl        = 12'hc20,
  vtype     = 12'hc21,
  vlenb     = 12'hc22
} csr_enum_e;

typedef enum logic [2:0] {
  SEW8 = 3'b000,
  SEW16 = 3'b001,
  SEW32 = 3'b010,
  SEW_LAST = 3'b111
} sew_e;

typedef enum int {
  EEW_NONE = 0,
  EEW1 = 1,
  EEW8 = 8,
  EEW16 = 16,
  EEW32 = 32,
  EEW64 = 64
} eew_e;

typedef enum logic {
  UNDISTURB = 0,
  AGNOSTIC  = 1
} agnostic_e;

typedef enum logic [2:0] {
  LMUL1_4   = 3'b110,
  LMUL1_2   = 3'b111,
  LMUL1     = 3'b000,
  LMUL2     = 3'b001,
  LMUL4     = 3'b010,
  LMUL8     = 3'b011,
  LMUL_LAST = 3'b100
} lmul_e;

typedef enum {
  EMUL_NONE,
  EMUL1_4,
  EMUL1_2,
  EMUL1,
  EMUL2,
  EMUL4,
  EMUL8
} emul_e;

typedef enum logic [1:0] {
  RNU = 0,
  RNE = 1,
  RDN = 2,
  ROD = 3
} vxrm_e;

typedef enum logic [2:0] {
  OPIVV = 3'b000,  // vs2,      vs1, vd
  OPFVV = 3'b001,  // vs2,      vs1, vd/rd. float, not support
  OPMVV = 3'b010,  // vs2,      vs1, vd/rd
  OPIVI = 3'b011,  // vs2, imm[4:0], vd
  OPIVX = 3'b100,  // vs2,      rs1, vd
  OPFVF = 3'b101,  // vs2,      rs1, vd. float, not support
  OPMVX = 3'b110,  // vs2,      rs1, vd/rd
  OPCFG = 3'b111   // vset* instructions
} alu_type_e;

typedef enum logic [7:0] {
  // OPI
  VADD                 = 8'b00_000_000,
  VSUB                 = 8'b00_000_010,
  VRSUB                = 8'b00_000_011,
  VADC                 = 8'b00_010_000,
  VMADC                = 8'b00_010_001,
  VSBC                 = 8'b00_010_010,
  VMSBC                = 8'b00_010_011,
  VAND                 = 8'b00_001_001,
  VOR                  = 8'b00_001_010,
  VXOR                 = 8'b00_001_011,
  VSLL                 = 8'b00_100_101,
  VSRL                 = 8'b00_101_000,
  VSRA                 = 8'b00_101_001,
  VNSRL                = 8'b00_101_100,
  VNSRA                = 8'b00_101_101,
  VMSEQ                = 8'b00_011_000,
  VMSNE                = 8'b00_011_001,
  VMSLTU               = 8'b00_011_010,
  VMSLT                = 8'b00_011_011,
  VMSLEU               = 8'b00_011_100,
  VMSLE                = 8'b00_011_101,
  VMSGTU               = 8'b00_011_110,
  VMSGT                = 8'b00_011_111,
  VMINU                = 8'b00_000_100,
  VMIN                 = 8'b00_000_101,
  VMAXU                = 8'b00_000_110,
  VMAX                 = 8'b00_000_111,
  VMERGE_VMVV          = 8'b00_010_111,  // vm=0: vmerge; vm=1: vmv.v
  VSADDU               = 8'b00_100_000,
  VSADD                = 8'b00_100_001,
  VSSUBU               = 8'b00_100_010,
  VSSUB                = 8'b00_100_011,
  VSMUL_VMVNRR         = 8'b00_100_111,  // .vv,.vx: vsmul; .vi: vmvnrr
  VSSRL                = 8'b00_101_010,
  VSSRA                = 8'b00_101_011,
  VNCLIPU              = 8'b00_101_110,
  VNCLIP               = 8'b00_101_111,
  // Vector Reduction Operations in OPI
  VWREDSUMU            = 8'b00_110_000,
  VWREDSUM             = 8'b00_110_001,
  // Vector Permutation Operations in OPI
  VSLIDEUP_RGATHEREI16 = 8'b00_001_110,
  VSLIDEDOWN           = 8'b00_001_111,
  VRGATHER             = 8'b00_001_100,
  // OPM
  VWADDU               = 8'b01_110_000,
  VWADD                = 8'b01_110_001,
  VWADDU_W             = 8'b01_110_100,
  VWADD_W              = 8'b01_110_101,
  VWSUBU               = 8'b01_110_010,
  VWSUB                = 8'b01_110_011,
  VWSUBU_W             = 8'b01_110_110,
  VWSUB_W              = 8'b01_110_111,
  VXUNARY0             = 8'b01_010_010,  // VZEXT/VSEXT
  VMUL                 = 8'b01_100_101,
  VMULH                = 8'b01_100_111,
  VMULHU               = 8'b01_100_100,
  VMULHSU              = 8'b01_100_110,
  VDIVU                = 8'b01_100_000,
  VDIV                 = 8'b01_100_001,
  VREMU                = 8'b01_100_010,
  VREM                 = 8'b01_100_011,
  VWMUL                = 8'b01_111_011,
  VWMULU               = 8'b01_111_000,
  VWMULSU              = 8'b01_111_010,
  VMACC                = 8'b01_101_101,
  VNMSAC               = 8'b01_101_111,
  VMADD                = 8'b01_101_001,
  VNMSUB               = 8'b01_101_011,
  VWMACCU              = 8'b01_111_100,
  VWMACC               = 8'b01_111_101,
  VWMACCUS             = 8'b01_111_110,
  VWMACCSU             = 8'b01_111_111,
  VAADDU               = 8'b01_001_000,
  VAADD                = 8'b01_001_001,
  VASUBU               = 8'b01_001_010,
  VASUB                = 8'b01_001_011,
  VREDSUM              = 8'b01_000_000,
  VREDAND              = 8'b01_000_001,
  VREDOR               = 8'b01_000_010,
  VREDXOR              = 8'b01_000_011,
  VREDMINU             = 8'b01_000_100,
  VREDMIN              = 8'b01_000_101,
  VREDMAXU             = 8'b01_000_110,
  VREDMAX              = 8'b01_000_111,
  VMAND                = 8'b01_011_001,
  VMOR                 = 8'b01_011_010,
  VMXOR                = 8'b01_011_011,
  VMORN                = 8'b01_011_100,
  VMNAND               = 8'b01_011_101,
  VMNOR                = 8'b01_011_110,
  VMANDN               = 8'b01_011_000,
  VMXNOR               = 8'b01_011_111,
  VMUNARY0             = 8'b01_010_100,  // vmsbf, vmsof, vmsif, viota, vid
  VSLIDE1UP            = 8'b01_001_110,
  VSLIDE1DOWN          = 8'b01_001_111,
  VCOMPRESS            = 8'b01_010_111,
  VWXUNARY0            = 8'b01_010_000,  // vcpop.m, vfirst.m and vmv
  ALU_UNUSE_INST       = 8'b11_111_111
} alu_inst_e;

typedef enum logic [4:0] {
  VZEXT_VF4     = 5'b00100,
  VSEXT_VF4     = 5'b00101,
  VZEXT_VF2     = 5'b00110,
  VSEXT_VF2     = 5'b00111,
  VXUNARY0_NONE = 5'b11111
} vxunary0_e;

// vcpop.m, vfirst.m and vmv
typedef enum logic [4:0] {
  VMV_X_S        = 5'b00000,
  VCPOP          = 5'b10000,
  VFIRST         = 5'b10001,
  VWXUNARY0_NONE = 5'b11111
} vwxunary0_e;
parameter logic [4:0] VMV_S_X = 5'b00000;

// vmsbf, vmsof, vmsif, viota, vid
typedef enum logic [4:0] {
  VMSBF         = 5'b00001,
  VMSOF         = 5'b00010,
  VMSIF         = 5'b00011,
  VIOTA         = 5'b10000,
  VID           = 5'b10001,
  VMUNARY0_NONE = 5'b11111
} vmunary0_e;

typedef enum logic [1:0] {
  LSU_US = 2'b00,  // unit-stride
  LSU_UI = 2'b01,  // indexed-unordered
  LSU_CS = 2'b10,  // strided
  LSU_OI = 2'b11   // indexed-ordered
} lsu_mop_e;

typedef enum logic [4:0] {
  NORMAL        = 5'b0_0000,  // unit-stride load/store
  WHOLE_REG     = 5'b0_1000,  // unit-stride, whole register load/store
  MASK          = 5'b0_1011,  // unit-stride, mask load/store, EEW=8
  FOF           = 5'b1_0000,  // unit-stride fault-only-first load
  LSU_UMOP_NONE = 5'b1_1111
} lsu_umop_e;

typedef enum logic [2:0] {
  NF1,
  NF2,
  NF3,
  NF4,
  NF5,
  NF6,
  NF7,
  NF8
} lsu_nf_e;

typedef enum logic [2:0] {
  NR1 = 3'b000,
  NR2 = 3'b001,
  NR3 = 3'b010,
  NR4 = 3'b011,
  NR5 = 3'b100,
  NR6 = 3'b101,
  NR7 = 3'b110,
  NR8 = 3'b111
} lsu_nr_e;

typedef enum logic [2:0] {
  LSU_8BIT  = 3'b000,
  LSU_16BIT = 3'b101,
  LSU_32BIT = 3'b110,
  LSU_64BIT = 3'b111
} lsu_width_e;

typedef enum int {
  VL,
  VS,
  VLM,
  VSM,
  VLS,
  VSS,
  VLUX,
  VLOX,
  VSUX,
  VSOX,
  VLFF,
  VLSEG,
  VSSEG,
  VLSEGFF,
  VLSSEG,
  VSSSEG,
  VLUXSEG,
  VLOXSEG,
  VSUXSEG,
  VSOXSEG,
  VLR,
  VSR,
  LSU_UNUSE_INST
} lsu_inst_e;

typedef enum {
  VSETVLI,
  VSETIVLI,
  VSETVL
} vset_inst_e;

typedef enum {
  XRF,
  VRF,
  IMM,
  UIMM,
  UNUSE
} operand_type_e;

typedef enum {
  FLW,
  FSW,
  FADD_S,
  FMUL_S,
  FSUB_S,
  FDIV_S,
  FSQRT_S,
  FMIN_S,
  FMAX_S,
  FMADD_S,
  FMSUB_S,
  FNMSUB_S,
  FNMADD_S,
  FCVT_W_S,
  FCVT_S_W,
  FCVT_WU_S,
  FCVT_S_WU,
  FSGNJ_S,
  FSGNJN_S,
  FSGNJX_S,
  FMV_X_W,
  FMV_W_X,
  FEQ_S,
  FLT_S,
  FLE_S,
  FCLASS
} floating_e;  //scalar floating instruction

typedef enum logic [2:0] {
  F_RNE = 3'b000,
  F_RTZ = 3'b001,
  F_RDN = 3'b010,
  F_RUP = 3'b011,
  F_RMM = 3'b100,
  F_RESERVE1 = 3'b101,
  F_RESERVE2 = 3'b110,
  F_DYN = 3'b111
} rounding_mode_e;

typedef enum logic [2:0] {
  MUL,
  MULH,
  MULHSU,
  MULHU,
  DIV,
  DIVU,
  REM,
  REMU
} mult_div_e;

typedef enum logic [2:0] {
  CSRRW  = 3'b001,
  CSRRS  = 3'b010,
  CSRRC  = 3'b011,
  CSRRWI = 3'b101,
  CSRRSI = 3'b110,
  CSRRCI = 3'b111
} zicsr_e;

typedef enum {
  ANDN,
  ORN,
  XNOR,
  CLZ,
  CTZ,
  CPOP,
  MAX,
  MAXU,
  MIN,
  MINU,
  SEXT_B,
  SEXT_H,
  ZEXT_H,
  ROL,
  ROR,
  RORI,
  ORC_B,
  REV8
} bit_manipulation_e;

typedef enum {FENCE_I} fencei_e;
