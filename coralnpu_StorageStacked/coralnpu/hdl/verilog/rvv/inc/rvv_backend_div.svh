`ifndef DIV_DEFINE_SVH
`define DIV_DEFINE_SVH

typedef struct packed {
`ifdef TB_SUPPORT
  logic   [`PC_WIDTH-1:0]         uop_pc;
`endif
  FUNCT6_u                        uop_funct6;
  logic   [`FUNCT3_WIDTH-1:0]     uop_funct3;
  logic   [`ROB_DEPTH_WIDTH-1:0]  rob_entry;
  EEW_e                           vs2_eew;
} DIV_RES_t;

`ifdef ZVE32F_ON
typedef struct packed {
`ifdef TB_SUPPORT
  logic   [`PC_WIDTH-1:0]         uop_pc;
`endif
  logic   [`ROB_DEPTH_WIDTH-1:0]  rob_entry;
} FDIV_RES_t;
`endif

`endif // DIV_DEFINE_SVH
