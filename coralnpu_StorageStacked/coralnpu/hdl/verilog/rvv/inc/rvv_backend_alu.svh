`ifndef ALU_DEFINE_SVH
`define ALU_DEFINE_SVH

typedef enum logic [0:0]{
  ADDSUB_VADD, 
  ADDSUB_VSUB
} ADDSUB_e;   

typedef union packed {
  logic   [`VLEN-1:0]             v0;  
  logic   [`VLEN-1:0]             src2;
} v0_src2_u;

typedef union packed {
  logic   [`VLEN-1:0]             vd;
  logic   [`VLEN-1:0]             src1;
} vd_src1_u;

typedef union packed {
  logic   [`VLENB-1:0]            vsaturate;  
  logic   [`VLENB-1:0]            cout;
} info_u;

typedef struct packed {
`ifdef TB_SUPPORT
  logic   [`PC_WIDTH-1:0]         uop_pc;
`endif
  logic   [`ROB_DEPTH_WIDTH-1:0]  rob_entry;
  ADDSUB_e                        opcode;
  FUNCT6_u                        uop_funct6;
  logic   [`FUNCT3_WIDTH-1:0]     uop_funct3;
  logic                           is_addsub;
  logic                           is_cmp;
  logic   [`VSTART_WIDTH-1:0]     vstart;
  logic   [`VL_WIDTH-1:0]         vl;       
  logic                           vm;               
  RVVXRM                          vxrm;       
  v0_src2_u                       v0_src2;
  vd_src1_u                       vd_src1;
  EEW_e                           vs2_eew;
  logic   [`VLEN-1:0]             w_data;             
  logic                           w_valid;
  info_u                          vsat_cout;
  logic   [`VLENB-1:0]            src2_sgn;
  logic   [`VLENB-1:0]            src1_sgn;
  logic                           last_uop_valid;
  logic   [$clog2(`EMUL_MAX)-1:0] uop_index;          
} PIPE_DATA_t;

`endif // ALU_DEFINE_SVH
