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

//--------------------------------------------------------------------------
// Class: coralnpu_rvvi_monitor
// Description: Monitors the rvviTrace interface and triggers an event upon
//              instruction retirement.
//--------------------------------------------------------------------------
class coralnpu_rvvi_monitor extends uvm_monitor;
  `uvm_component_utils(coralnpu_rvvi_monitor)
  // Use fully parameterized virtual interface type
  virtual rvviTrace #(
      .ILEN  (`ILEN),
      .XLEN  (`XLEN),
      .FLEN  (`FLEN),
      .VLEN  (`VLEN),
      .NHART (`NHART),
      .RETIRE(`RETIRE)
  ) rvvi_vif;
  virtual coralnpu_rvvi_monitor_if #(
      .XLEN(`XLEN),
      .FLEN(`FLEN),
      .VLEN(`VLEN)
  ) rvvi_monitor_vif;
  uvm_event instruction_retired_event;

  bit [(`XLEN-1):0] gpr_reg_val[32];
  bit [(`VLEN-1):0] v_reg_val[32];
  bit [(`FLEN-1):0] f_reg_val[32];

  //v inst name string
  string asm_inst = "none";

  uvm_analysis_port #(coralnpu_rv32i_transaction) rv32i_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32v_transaction) rv32v_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32f_transaction) rv32f_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32m_transaction) rv32m_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32zicsr_transaction) rv32zicsr_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32zbb_transaction) rv32zbb_item_collected_port;
  uvm_analysis_port #(coralnpu_rv32zifencei_transaction) rv32zifencei_item_collected_port;
  uvm_analysis_port #(coralnpu_rvvi_decode_transaction) custom_item_collected_port;

  function new(string name = "coralnpu_rvvi_monitor", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  virtual function void build_phase(uvm_phase phase);
    super.build_phase(phase);

    rv32i_item_collected_port = new("rv32i_item_collected_port", this);
    rv32v_item_collected_port = new("rv32v_item_collected_port", this);
    rv32f_item_collected_port = new("rv32f_item_collected_port", this);
    rv32m_item_collected_port = new("rv32m_item_collected_port", this);
    rv32zicsr_item_collected_port = new("rv32zicsr_item_collected_port", this);
    rv32zbb_item_collected_port = new("rv32zbb_item_collected_port", this);
    rv32zifencei_item_collected_port = new("rv32zifencei_item_collected_port", this);
    custom_item_collected_port = new("custom_item_collected_port", this);
    if (!uvm_config_db#(virtual rvviTrace #(
            .ILEN  (`ILEN),
            .XLEN  (`XLEN),
            .FLEN  (`FLEN),
            .VLEN  (`VLEN),
            .NHART (`NHART),
            .RETIRE(`RETIRE)
        ))::get(
            this, "", "rvvi_vif", rvvi_vif
        )) begin
      `uvm_fatal(get_type_name(), "RVVI virtual interface not found!")
    end
    if (!uvm_config_db#(virtual coralnpu_rvvi_monitor_if #(
            .XLEN(`XLEN),
            .FLEN(`FLEN),
            .VLEN(`VLEN)
        ))::get(
            this, "", "rvvi_monitor_vif", rvvi_monitor_vif
        )) begin
      `uvm_fatal(get_type_name(), "RVVI minitor interface not found!")
    end
    if (!uvm_config_db#(uvm_event)::get(
            this, "", "instruction_retired_event", instruction_retired_event
        )) begin
      `uvm_fatal(get_type_name(), "Instruction retired event handle not found!")
    end
  endfunction

  virtual task run_phase(uvm_phase phase);
    coralnpu_rvvi_transaction trans_collected;
    bit[4:0] gpr_reg;
    bit[4:0] v_reg;
    bit[4:0] f_reg;
    bit any_instruction_retired;
    bit[1023:0] x_wdata_tmp;
    bit[1023:0] v_wdata_tmp;
    bit[1023:0] f_wdata_tmp;
    bit[4095:0] csr_addr;
    csr_enum_e    csr_enum;

    `uvm_info(get_type_name(), "RVVI Monitor run phase starting.", UVM_MEDIUM);

    forever begin
      any_instruction_retired = 1'b0;
      @(posedge rvvi_vif.clk);
      // collect a instruction details from RVVI interface
      for (int i = 0; i < `RETIRE; i++) begin
        if (rvvi_vif.valid[0][i]) begin  // Assuming NHART=1
          trans_collected           = coralnpu_rvvi_transaction::type_id::create("trans_collected");
          trans_collected.order     = rvvi_vif.order[0][i];
          trans_collected.pc        = rvvi_vif.pc_rdata[0][i];
          trans_collected.insn      = rvvi_vif.insn[0][i];
          trans_collected.trap      = rvvi_vif.trap[0][i];
          trans_collected.halt      = rvvi_vif.halt[0][i];
          trans_collected.x_wdata   = rvvi_vif.x_wdata[0][i];
          trans_collected.x_wb      = rvvi_vif.x_wb[0][i];
          trans_collected.f_wdata   = rvvi_vif.f_wdata[0][i];
          trans_collected.f_wb      = rvvi_vif.f_wb[0][i];
          trans_collected.v_wdata   = rvvi_vif.v_wdata[0][i];
          trans_collected.v_wb      = rvvi_vif.v_wb[0][i];
          trans_collected.csr_wb    = rvvi_vif.csr_wb[0][i];
          trans_collected.csr_wdata = rvvi_vif.csr[0][i];
          //record gpr register update
          if (rvvi_vif.x_wb[0][i] > 0) begin
            gpr_reg = $clog2(rvvi_vif.x_wb[0][i]);
            x_wdata_tmp = rvvi_vif.x_wdata[0][i] >> (gpr_reg * `XLEN);
            gpr_reg_val[gpr_reg] = x_wdata_tmp[31:0];
          end

          //record v register update
          if (rvvi_vif.v_wb[0][i] > 0) begin
            v_reg = $clog2(rvvi_vif.v_wb[0][i]);
            v_wdata_tmp = rvvi_vif.v_wdata[0][i] >> (v_reg * `VLEN);
            v_reg_val[v_reg] = v_wdata_tmp[127:0];
          end

          //record f register update
          if (rvvi_vif.f_wb[0][i] > 0) begin
            f_reg = $clog2(rvvi_vif.f_wb[0][i]);
            f_wdata_tmp = rvvi_vif.f_wdata[0][i] >> (f_reg * `FLEN);
            f_reg_val[f_reg] = f_wdata_tmp[31:0];
          end
          any_instruction_retired = 1'b1;

          //Update gpr value to debug interafce
          if (rvvi_vif.x_wb[0][i] > 0) begin
            rvvi_monitor_vif.gpr_reg_val_pre[gpr_reg] = x_wdata_tmp[31:0];
          end
          //Upadte vreg value to debug interafce
          if (rvvi_vif.v_wb[0][i] > 0) begin
            rvvi_monitor_vif.v_reg_val_pre[v_reg] = v_wdata_tmp[127:0];
          end
          //Update float value to debug interafce
          if (rvvi_vif.f_wb[0][i] > 0) begin
            rvvi_monitor_vif.f_reg_val_pre[f_reg] = f_wdata_tmp[31:0];
          end

          //Update csr value to debug interafce
          if (rvvi_vif.csr_wb[0][i] > 0) begin
            for (int csr_index = 0; csr_index < 4095; csr_index++) begin
              if (rvvi_vif.csr_wb[0][i][csr_index] == 1) begin
                csr_enum = csr_enum_e'(csr_index);
                case (csr_enum)
                  mstatus: begin
                    rvvi_monitor_vif.mstatus = rvvi_vif.csr[0][i][csr_index];
                  end
                  misa: begin
                    rvvi_monitor_vif.misa = rvvi_vif.csr[0][i][csr_index];
                  end
                  mie: begin
                    rvvi_monitor_vif.mie = rvvi_vif.csr[0][i][csr_index];
                  end
                  mtvec: begin
                    rvvi_monitor_vif.mtvec = rvvi_vif.csr[0][i][csr_index];
                  end
                  mscratch: begin
                    rvvi_monitor_vif.mscratch = rvvi_vif.csr[0][i][csr_index];
                  end
                  mepc: begin
                    rvvi_monitor_vif.mepc = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcause: begin
                    rvvi_monitor_vif.mcause = rvvi_vif.csr[0][i][csr_index];
                  end
                  mtval: begin
                    rvvi_monitor_vif.mtval = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcycle: begin
                    rvvi_monitor_vif.mcycle = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcycleh: begin
                    rvvi_monitor_vif.mcycleh = rvvi_vif.csr[0][i][csr_index];
                  end
                  minstret: begin
                    rvvi_monitor_vif.minstret = rvvi_vif.csr[0][i][csr_index];
                  end
                  minstreth: begin
                    rvvi_monitor_vif.minstreth = rvvi_vif.csr[0][i][csr_index];
                  end
                  //vector csr register
                  vstart: begin
                    rvvi_monitor_vif.vstart = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vstart  = rvvi_monitor_vif.vstart;
                  end
                  vxsat: begin
                    rvvi_monitor_vif.vxsat = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vxsat  = rvvi_monitor_vif.vxsat;
                  end
                  vxrm: begin
                    rvvi_monitor_vif.vxrm = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vxrm  = vxrm_e'(rvvi_monitor_vif.vxrm);
                  end
                  vcsr: begin
                    rvvi_monitor_vif.vcsr = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vcsr  = rvvi_monitor_vif.vcsr;
                  end
                  vl: begin
                    rvvi_monitor_vif.vl = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vl  = rvvi_monitor_vif.vl;
                  end
                  vtype: begin
                    rvvi_monitor_vif.vtype = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vtype = rvvi_monitor_vif.vtype;
                    rvvi_monitor_vif.vtype_vma = rvvi_monitor_vif.vtype[7];
                    trans_collected.vtype_vma = agnostic_e'(rvvi_monitor_vif.vtype_vma);
                    rvvi_monitor_vif.vtype_vta = rvvi_monitor_vif.vtype[6];
                    trans_collected.vtype_vta = agnostic_e'(rvvi_monitor_vif.vtype_vta);
                    rvvi_monitor_vif.vtype_vsew = rvvi_monitor_vif.vtype[5:3];
                    trans_collected.vtype_vsew = sew_e'(rvvi_monitor_vif.vtype_vsew);
                    rvvi_monitor_vif.vtype_vlmul = rvvi_monitor_vif.vtype[2:0];
                    trans_collected.vtype_vlmul = lmul_e'(rvvi_monitor_vif.vtype_vlmul);
                    rvvi_monitor_vif.vtype_vill = rvvi_monitor_vif.vtype[`XLEN-1];
                    trans_collected.vtype_vill = rvvi_monitor_vif.vtype_vill;
                  end
                  vlenb: begin
                    rvvi_monitor_vif.vlenb = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.vlenb  = rvvi_monitor_vif.vlenb;
                  end
                  //floating-point csr
                  fflags: begin
                    rvvi_monitor_vif.fflags = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.fflags  = rvvi_monitor_vif.fflags;
                  end
                  frm: begin
                    rvvi_monitor_vif.frm = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.frm  = rvvi_monitor_vif.frm;
                  end
                  fcsr: begin
                    rvvi_monitor_vif.fcsr = rvvi_vif.csr[0][i][csr_index];
                    trans_collected.fcsr  = rvvi_monitor_vif.fcsr;
                  end
                  //privileged csr
                  mvendorid: begin
                    rvvi_monitor_vif.mvendorid = rvvi_vif.csr[0][i][csr_index];
                  end
                  marchid: begin
                    rvvi_monitor_vif.marchid = rvvi_vif.csr[0][i][csr_index];
                  end
                  mimpid: begin
                    rvvi_monitor_vif.mimpid = rvvi_vif.csr[0][i][csr_index];
                  end
                  mhartid: begin
                    rvvi_monitor_vif.mhartid = rvvi_vif.csr[0][i][csr_index];
                  end
                  //custom
                  mcontext0: begin
                    rvvi_monitor_vif.mcontext0 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext1: begin
                    rvvi_monitor_vif.mcontext1 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext2: begin
                    rvvi_monitor_vif.mcontext2 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext3: begin
                    rvvi_monitor_vif.mcontext3 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext4: begin
                    rvvi_monitor_vif.mcontext4 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext5: begin
                    rvvi_monitor_vif.mcontext5 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext6: begin
                    rvvi_monitor_vif.mcontext6 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mcontext7: begin
                    rvvi_monitor_vif.mcontext7 = rvvi_vif.csr[0][i][csr_index];
                  end
                  mpc: begin
                    rvvi_monitor_vif.mpc = rvvi_vif.csr[0][i][csr_index];
                  end
                  msp: begin
                    rvvi_monitor_vif.msp = rvvi_vif.csr[0][i][csr_index];
                  end
                  kisa: begin
                    rvvi_monitor_vif.kisa = rvvi_vif.csr[0][i][csr_index];
                  end
                  kscm0: begin
                    rvvi_monitor_vif.kscm0 = rvvi_vif.csr[0][i][csr_index];
                  end
                  kscm1: begin
                    rvvi_monitor_vif.kscm1 = rvvi_vif.csr[0][i][csr_index];
                  end
                  kscm2: begin
                    rvvi_monitor_vif.kscm2 = rvvi_vif.csr[0][i][csr_index];
                  end
                  kscm3: begin
                    rvvi_monitor_vif.kscm3 = rvvi_vif.csr[0][i][csr_index];
                  end
                  kscm4: begin
                    rvvi_monitor_vif.kscm4 = rvvi_vif.csr[0][i][csr_index];
                  end
                endcase
              end
            end
          end
          `uvm_info(get_full_name(), $sformatf("Seen instruction:\n%s", trans_collected.sprint()),
                    UVM_HIGH)

          //decode rvvi transaction
          decode(trans_collected, i);
        end
      end
      if (any_instruction_retired) begin
        instruction_retired_event.trigger();
      end
    end
  endtask

  // **********************************************************
  // task: decode
  // Parse out the instruction details such as ISA group,instruction name,working register address,working register values,etc
  // args0:  transaction from rvvi interafce
  // **********************************************************
  virtual task decode(coralnpu_rvvi_transaction rvvi_trans, bit [7:0] retire);
    fmt_typ_enum fmt_typ;
    bit [(`ILEN-1):0] insn;
    insn_name_enum insn_name;
    isa_enum isa_name;

    coralnpu_rv32i_transaction rv32i_trans;
    coralnpu_rv32v_transaction rv32v_trans;
    coralnpu_rv32f_transaction rv32f_trans;
    coralnpu_rv32m_transaction rv32m_trans;
    coralnpu_rv32zicsr_transaction rv32zicsr_trans;
    coralnpu_rv32zbb_transaction rv32zbb_trans;
    coralnpu_rv32zifencei_transaction rv32zifencei_trans;
    coralnpu_rvvi_decode_transaction custom_trans;

    insn = rvvi_trans.insn;

    //get format type
    fmt_typ = get_fmt_typ(insn);

    get_insn_name(insn, insn_name, isa_name);

    //Update instruction name to debug interafce
    rvvi_monitor_vif.insn[retire] = insn_name.name();

    case (isa_name)
      RV32I: begin
        rv32i_trans = new();
        rv32i_trans.insn = insn;
        rv32i_trans.insn_name = insn_name;
        rv32i_trans.pc = rvvi_trans.pc;
        rv32i_trans.order = rvvi_trans.order;
        rv32i_trans.trap = rvvi_trans.trap;
        rv32i_trans.halt = rvvi_trans.halt;
        integer_insn_decode(rv32i_trans, fmt_typ);
        //transmit trasaction to covergroup to sample
        rv32i_item_collected_port.write(rv32i_trans);
        `uvm_info(get_full_name(), $sformatf("Seen decoded instruction:\n%s", rv32i_trans.sprint()),
                  UVM_HIGH)
        //xreg
        rvvi_monitor_vif.rd_addr[retire]  = rv32i_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32i_trans.rs1_addr;
        rvvi_monitor_vif.rs2_addr[retire] = rv32i_trans.rs2_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32i_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32i_trans.rs1_val;
        rvvi_monitor_vif.rs2_val[retire]  = rv32i_trans.rs2_val;
      end

      RV32V: begin
        rv32v_trans             = new();
        rv32v_trans.insn        = insn;
        rv32v_trans.pc          = rvvi_trans.pc;
        rv32v_trans.order       = rvvi_trans.order;
        rv32v_trans.trap        = rvvi_trans.trap;
        rv32v_trans.halt        = rvvi_trans.halt;
        rv32v_trans.inst_type   = fmt_typ;
        rv32v_trans.insn_name   = V_INSTR;
        //vector csr value from rvvi
        rv32v_trans.vstart      = rvvi_trans.vstart;
        rv32v_trans.vxsat       = rvvi_trans.vxsat;
        rv32v_trans.vxrm        = rvvi_trans.vxrm;
        rv32v_trans.vcsr        = rvvi_trans.vcsr;
        rv32v_trans.vl          = rvvi_trans.vl;
        rv32v_trans.vtype       = rvvi_trans.vtype;
        rv32v_trans.vlenb       = rvvi_trans.vlenb;
        rv32v_trans.vtype_vma   = rvvi_trans.vtype_vma;
        rv32v_trans.vtype_vta   = rvvi_trans.vtype_vta;
        rv32v_trans.vtype_vsew  = rvvi_trans.vtype_vsew;
        rv32v_trans.vtype_vlmul = rvvi_trans.vtype_vlmul;
        rv32v_trans.vtype_vill  = rvvi_trans.vtype_vill;
        vector_insn_decode(rv32v_trans, fmt_typ);
        rv32v_item_collected_port.write(rv32v_trans);
        `uvm_info(get_full_name(), $sformatf(
                  "Seen decoded vector instruction:\n%s", rv32v_trans.sprint()), UVM_HIGH)
        rvvi_monitor_vif.insn[retire]     = asm_inst;
        rvvi_monitor_vif.lsu_nf[retire]   = rv32v_trans.lsu_nf.name();
        rvvi_monitor_vif.lsu_mop[retire]  = rv32v_trans.lsu_mop.name();
        rvvi_monitor_vif.lsu_umop[retire] = rv32v_trans.lsu_umop.name();
        rvvi_monitor_vif.vm[retire]       = rv32v_trans.vm;
        rvvi_monitor_vif.vd_addr[retire]  = rv32v_trans.vd_addr;
        rvvi_monitor_vif.vs2_addr[retire] = rv32v_trans.vs2_addr;
        rvvi_monitor_vif.vs1_addr[retire] = rv32v_trans.vs1_addr;
        rvvi_monitor_vif.vd_val[retire]   = rv32v_trans.vd_val;
        rvvi_monitor_vif.vs2_val[retire]  = rv32v_trans.vs2_val;
        rvvi_monitor_vif.vs1_val[retire]  = rv32v_trans.vs1_val;
        rvvi_monitor_vif.v0_val[retire]   = rv32v_trans.v0_val;
        //xreg
        rvvi_monitor_vif.rd_addr[retire]  = rv32v_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32v_trans.rs1_addr;
        rvvi_monitor_vif.rs2_addr[retire] = rv32v_trans.rs2_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32v_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32v_trans.rs1_val;
        rvvi_monitor_vif.rs2_val[retire]  = rv32v_trans.rs2_val;
      end
      RV32F: begin
        rv32f_trans = new();
        rv32f_trans.insn = insn;
        rv32f_trans.insn_name = FP_INSTR;
        //fp csr value from rvvi
        rv32f_trans.fflags = rvvi_trans.fflags;
        rv32f_trans.frm = rvvi_trans.frm;
        rv32f_trans.fcsr = rvvi_trans.fcsr;
        rv32f_trans.pc = rvvi_trans.pc;
        rv32f_trans.order = rvvi_trans.order;
        rv32f_trans.trap = rvvi_trans.trap;
        rv32f_trans.halt = rvvi_trans.halt;
        floating_point_insn_decode(rv32f_trans, fmt_typ);
        rvvi_monitor_vif.insn[retire] = rv32f_trans.inst_name.name();
        //transmit trasaction to covergroup to sample
        rv32f_item_collected_port.write(rv32f_trans);
        `uvm_info(get_full_name(), $sformatf(
                  "Seen decoded fp instruction:\n%s", rv32f_trans.sprint()), UVM_HIGH)
        rvvi_monitor_vif.insn[retire]     = rv32f_trans.inst_name.name();
        rvvi_monitor_vif.fd_addr[retire]  = rv32f_trans.fd_addr;
        rvvi_monitor_vif.fs3_addr[retire] = rv32f_trans.fs3_addr;
        rvvi_monitor_vif.fs2_addr[retire] = rv32f_trans.fs2_addr;
        rvvi_monitor_vif.fs1_addr[retire] = rv32f_trans.fs1_addr;
        rvvi_monitor_vif.fd_val[retire]   = rv32f_trans.fd_val;
        rvvi_monitor_vif.fs3_val[retire]  = rv32f_trans.fs3_val;
        rvvi_monitor_vif.fs2_val[retire]  = rv32f_trans.fs2_val;
        rvvi_monitor_vif.fs1_val[retire]  = rv32f_trans.fs1_val;
        //xreg
        rvvi_monitor_vif.rd_addr[retire]  = rv32f_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32f_trans.rs1_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32f_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32f_trans.rs1_val;
      end
      RV32M: begin
        rv32m_trans = new();
        rv32m_trans.insn = insn;
        rv32m_trans.insn_name = M_INSTR;
        rv32m_trans.pc = rvvi_trans.pc;
        rv32m_trans.order = rvvi_trans.order;
        rv32m_trans.trap = rvvi_trans.trap;
        rv32m_trans.halt = rvvi_trans.halt;
        mult_div_insn_decode(rv32m_trans, fmt_typ);
        //transmit trasaction to covergroup to sample
        rv32m_item_collected_port.write(rv32m_trans);
        `uvm_info(get_full_name(), $sformatf("Seen decoded m instruction:\n%s", rv32m_trans.sprint()
                  ), UVM_HIGH)
        //xreg
        rvvi_monitor_vif.insn[retire]     = rv32m_trans.inst_name.name();
        rvvi_monitor_vif.rd_addr[retire]  = rv32m_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32m_trans.rs1_addr;
        rvvi_monitor_vif.rs2_addr[retire] = rv32m_trans.rs2_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32m_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32m_trans.rs1_val;
        rvvi_monitor_vif.rs2_val[retire]  = rv32m_trans.rs2_val;
      end
      RV32Zicsr: begin
        rv32zicsr_trans = new();
        rv32zicsr_trans.insn = insn;
        rv32zicsr_trans.insn_name = CSR_INSTR;
        rv32zicsr_trans.pc = rvvi_trans.pc;
        rv32zicsr_trans.order = rvvi_trans.order;
        rv32zicsr_trans.trap = rvvi_trans.trap;
        rv32zicsr_trans.halt = rvvi_trans.halt;
        csr_insn_decode(rv32zicsr_trans, fmt_typ);
        //transmit trasaction to covergroup to sample
        rv32zicsr_item_collected_port.write(rv32zicsr_trans);
        `uvm_info(get_full_name(), $sformatf(
                  "Seen decoded instruction:\n%s", rv32zicsr_trans.sprint()), UVM_HIGH)
        rvvi_monitor_vif.insn[retire]     = rv32zicsr_trans.inst_name.name();
        rvvi_monitor_vif.rd_addr[retire]  = rv32zicsr_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32zicsr_trans.rs1_addr;
        rvvi_monitor_vif.csr_addr[retire] = rv32zicsr_trans.csr_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32zicsr_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32zicsr_trans.rs1_val;
        rvvi_monitor_vif.csr_val[retire]  = rv32zicsr_trans.csr_val;
      end
      RV32Zbb: begin
        rv32zbb_trans = new();
        rv32zbb_trans.insn = insn;
        rv32zbb_trans.insn_name = Zbb_INSTR;
        rv32zbb_trans.pc = rvvi_trans.pc;
        rv32zbb_trans.order = rvvi_trans.order;
        rv32zbb_trans.halt = rvvi_trans.halt;
        bit_manipulation_insn_decode(rv32zbb_trans, fmt_typ);
        //transmit trasaction to covergroup to sample
        rv32zbb_item_collected_port.write(rv32zbb_trans);
        `uvm_info(get_full_name(), $sformatf("Seen decoded instruction:\n%s", rv32zbb_trans.sprint()
                  ), UVM_HIGH)
        rvvi_monitor_vif.insn[retire]     = rv32zbb_trans.inst_name.name();
        rvvi_monitor_vif.rd_addr[retire]  = rv32zbb_trans.rd_addr;
        rvvi_monitor_vif.rs1_addr[retire] = rv32zbb_trans.rs1_addr;
        rvvi_monitor_vif.rs2_addr[retire] = rv32zbb_trans.rs2_addr;
        rvvi_monitor_vif.rd_val[retire]   = rv32zbb_trans.rd_val;
        rvvi_monitor_vif.rs1_val[retire]  = rv32zbb_trans.rs1_val;
        rvvi_monitor_vif.rs2_val[retire]  = rv32zbb_trans.rs2_val;
      end
      RV32Zifencei: begin
        rv32zifencei_trans = new();
        rv32zifencei_trans.insn = insn;
        rv32zifencei_trans.insn_name = Zifencei_INSTR;
        rv32zifencei_trans.pc = rvvi_trans.pc;
        rv32zifencei_trans.order = rvvi_trans.order;
        rv32zifencei_trans.halt = rvvi_trans.halt;
        fencei_insn_decode(rv32zifencei_trans, fmt_typ);
        //transmit trasaction to covergroup to sample
        rv32zifencei_item_collected_port.write(rv32zifencei_trans);
        `uvm_info(get_full_name(), $sformatf(
                  "Seen decoded instruction:\n%s", rv32zifencei_trans.sprint()), UVM_HIGH)
        rvvi_monitor_vif.insn[retire] = rv32zifencei_trans.inst_name.name();
      end
      Custom: begin
        custom_trans = new();
        custom_trans.insn = insn;
        custom_trans.insn_name = insn_name;
        custom_trans.pc = rvvi_trans.pc;
        custom_trans.order = rvvi_trans.order;
        custom_item_collected_port.write(custom_trans);
        `uvm_info(get_full_name(), $sformatf("Seen decoded instruction:\n%s", custom_trans.sprint()
                  ), UVM_HIGH)
        rvvi_monitor_vif.insn[retire] = custom_trans.insn_name.name();
      end
    endcase
  endtask

  // **********************************************************
  // task: integer_insn_decode
  // Parse out the instruction details which uses integer registers
  // args0:  transaction handle that store instrction details
  // args1:  instrcuction format type
  // **********************************************************
  virtual function void integer_insn_decode(coralnpu_rv32i_transaction trans, fmt_typ_enum fmt_typ);
    bit [31:0] insn = trans.insn;
    case (fmt_typ)
      R_Type: begin
        trans.rd_addr  = insn[11:7];
        trans.rs1_addr = insn[19:15];
        trans.rs2_addr = insn[24:20];
        trans.rs1_val  = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs1_addr];
        trans.rs2_val  = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs2_addr];
        trans.rd_val   = gpr_reg_val[trans.rd_addr];
        //GPR Hazard check
        if (trans.rd_addr == rvvi_monitor_vif.rd_pre) begin
          trans.waw_hazard_hit = 1;
        end
        if(trans.rd_addr == rvvi_monitor_vif.rs1_pre || trans.rd_addr == rvvi_monitor_vif.rs2_pre)begin
          trans.war_hazard_hit = 1;
        end
        if(trans.rs1_addr == rvvi_monitor_vif.rd_pre || trans.rs2_addr == rvvi_monitor_vif.rd_pre)begin
          trans.raw_hazard_hit = 1;
        end
        //Update rs1_pre,rs2_pre,rd_pre
        rvvi_monitor_vif.rs1_pre = trans.rs1_addr;
        rvvi_monitor_vif.rs2_pre = trans.rs2_addr;
        rvvi_monitor_vif.rd_pre  = trans.rd_addr;
      end
      I_Type: begin
        trans.rd_addr = insn[11:7];
        trans.rd_val = gpr_reg_val[trans.rd_addr];
        trans.rs1_addr = insn[19:15];
        trans.rs1_val = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs1_addr];
        trans.imm_12bit = insn[31:20];
        //GPR Hazard check
        if (trans.rd_addr == rvvi_monitor_vif.rd_pre) begin
          trans.waw_hazard_hit = 1;
        end
        if(trans.rd_addr == rvvi_monitor_vif.rs1_pre || trans.rd_addr == rvvi_monitor_vif.rs2_pre)begin
          trans.war_hazard_hit = 1;
        end
        if (trans.rs1_addr == rvvi_monitor_vif.rd_pre) begin
          trans.raw_hazard_hit = 1;
        end
        //Update rs1_pre,rd_pre
        rvvi_monitor_vif.rs1_pre = trans.rs1_addr;
        rvvi_monitor_vif.rd_pre  = trans.rd_addr;
      end
      S_Type: begin
        trans.rs1_addr  = insn[19:15];
        trans.rs2_addr  = insn[24:20];
        trans.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs1_addr];
        trans.rs2_val   = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs2_addr];
        trans.imm_12bit = {insn[31:25], insn[11:7]};
        //GPR Hazard check
        if(trans.rs1_addr == rvvi_monitor_vif.rd_pre || trans.rs2_addr == rvvi_monitor_vif.rd_pre)begin
          trans.raw_hazard_hit = 1;
        end
        //Update rs1_pre,rs2_pre
        rvvi_monitor_vif.rs1_pre = trans.rs1_addr;
        rvvi_monitor_vif.rs2_pre = trans.rs2_addr;
      end
      B_Type: begin
        trans.rs1_addr  = insn[19:15];
        trans.rs2_addr  = insn[24:20];
        trans.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs1_addr];
        trans.rs2_val   = rvvi_monitor_vif.gpr_reg_val_pre[trans.rs2_addr];
        trans.imm_12bit = {insn[31], insn[7], insn[30:25], insn[11:8]};
        //GPR Hazard check
        if(trans.rs1_addr == rvvi_monitor_vif.rd_pre || trans.rs2_addr == rvvi_monitor_vif.rd_pre)begin
          trans.raw_hazard_hit = 1;
        end
        //Update rs1_pre,rs2_pre
        rvvi_monitor_vif.rs1_pre = trans.rs1_addr;
        rvvi_monitor_vif.rs2_pre = trans.rs2_addr;
      end
      U_Type: begin
        trans.rd_addr = insn[11:7];
        trans.rd_val = gpr_reg_val[trans.rd_addr];
        trans.imm_20bit = {insn[31:12]};
        //GPR Hazard check
        if (trans.rd_addr == rvvi_monitor_vif.rd_pre) begin
          trans.waw_hazard_hit = 1;
        end
        if(trans.rd_addr == rvvi_monitor_vif.rs1_pre || trans.rd_addr == rvvi_monitor_vif.rs2_pre)begin
          trans.war_hazard_hit = 1;
        end
        //Update rd_pre
        rvvi_monitor_vif.rd_pre = trans.rd_addr;
      end
      J_Type: begin
        trans.rd_addr = insn[11:7];
        trans.rd_val = gpr_reg_val[trans.rd_addr];
        trans.imm_20bit = {insn[31], insn[19:12], insn[20], insn[30:21]};
        //GPR Hazard check
        if (trans.rd_addr == rvvi_monitor_vif.rd_pre) begin
          trans.waw_hazard_hit = 1;
        end
        if(trans.rd_addr == rvvi_monitor_vif.rs1_pre || trans.rd_addr == rvvi_monitor_vif.rs2_pre)begin
          trans.war_hazard_hit = 1;
        end
        //Update rd_pre
        rvvi_monitor_vif.rd_pre = trans.rd_addr;
      end
      Fence_Type: begin
        trans.rd_addr = 0;
        trans.rd_val = 0;
        trans.rs1_addr = 0;
        trans.rs1_val = 0;
        trans.fm = insn[31:28];
        trans.pred = insn[27:24];
        trans.succ = insn[23:20];
      end
    endcase
  endfunction

  // **********************************************************
  // task: csr_insn_decode
  // Parse out CSR instructions details
  // args0:  CSR transaction handle
  // args1:  instruction format type
  // **********************************************************
  virtual function void csr_insn_decode(coralnpu_rv32zicsr_transaction tr, fmt_typ_enum fmt_typ);
    bit [2:0] funct3 = tr.insn[14:12];

    tr.rd_addr  = tr.insn[11:7];
    tr.csr_addr = tr.insn[31:20];
    tr.rs1_addr = tr.insn[19:15];

    tr.rd_val   = gpr_reg_val[tr.rd_addr];
    tr.csr_val  = get_csr_value(tr.csr_addr);

    if (funct3 inside {3'b101, 3'b110, 3'b111}) begin
      tr.is_imm  = 1;
      tr.rs1_val = unsigned'(tr.insn[19:15]);
    end else begin
      tr.is_imm  = 0;
      tr.rs1_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    end

    case (funct3)
      3'b001: tr.inst_name = CSRRW;
      3'b010: tr.inst_name = CSRRS;
      3'b011: tr.inst_name = CSRRC;
      3'b101: tr.inst_name = CSRRWI;
      3'b110: tr.inst_name = CSRRSI;
      3'b111: tr.inst_name = CSRRCI;
    endcase

    // Hazard check
    if((tr.rd_addr == rvvi_monitor_vif.rd_pre && tr.rd_addr != 0) ||
       (tr.csr_addr == rvvi_monitor_vif.csr_pre &&
       (tr.inst_name inside {CSRRW, CSRRWI} &&
       rvvi_monitor_vif.prev_inst_name inside {CSRRW, CSRRWI}))) begin
      tr.waw_hazard_hit = 1;
    end
    if((tr.rd_addr == rvvi_monitor_vif.rs1_pre && tr.rd_addr != 0) ||
   (tr.csr_addr == rvvi_monitor_vif.csr_pre &&
    tr.inst_name inside {CSRRW, CSRRWI} &&
    rvvi_monitor_vif.prev_inst_name inside {CSRRS, CSRRC, CSRRSI, CSRRCI })) begin
      tr.war_hazard_hit = 1;
    end
    if((!tr.is_imm && tr.rs1_addr == rvvi_monitor_vif.rd_pre && tr.rs1_addr != 0) ||
       (tr.csr_addr == rvvi_monitor_vif.csr_pre && tr.rd_addr!=0 &&
        rvvi_monitor_vif.prev_inst_name inside {CSRRW, CSRRWI})) begin
      tr.raw_hazard_hit = 1;
    end
    rvvi_monitor_vif.prev_inst_name = tr.inst_name;
    rvvi_monitor_vif.rs1_pre = tr.is_imm ? '0 : tr.rs1_addr;
    rvvi_monitor_vif.rd_pre = tr.rd_addr;
    rvvi_monitor_vif.csr_pre = tr.csr_addr;
  endfunction

  function bit [31:0] get_csr_value(bit [11:0] csr_addr);
    case (csr_addr)
      mstatus:   return rvvi_monitor_vif.mstatus;
      misa:      return rvvi_monitor_vif.misa;
      mie:       return rvvi_monitor_vif.mie;
      mtvec:     return rvvi_monitor_vif.mtvec;
      mscratch:  return rvvi_monitor_vif.mscratch;
      mepc:      return rvvi_monitor_vif.mepc;
      mcause:    return rvvi_monitor_vif.mcause;
      mtval:     return rvvi_monitor_vif.mtval;
      mcycle:    return rvvi_monitor_vif.mcycle;
      mcycleh:   return rvvi_monitor_vif.mcycleh;
      minstret:  return rvvi_monitor_vif.minstret;
      minstreth: return rvvi_monitor_vif.minstreth;
      fflags:    return rvvi_monitor_vif.fflags;
      frm:       return rvvi_monitor_vif.frm;
      fcsr:      return rvvi_monitor_vif.fcsr;
      mvendorid: return rvvi_monitor_vif.mvendorid;
      marchid:   return rvvi_monitor_vif.marchid;
      mimpid:    return rvvi_monitor_vif.mimpid;
      mhartid:   return rvvi_monitor_vif.mhartid;
      mcontext0: return rvvi_monitor_vif.mcontext0;
      mcontext1: return rvvi_monitor_vif.mcontext1;
      mcontext2: return rvvi_monitor_vif.mcontext2;
      mcontext3: return rvvi_monitor_vif.mcontext3;
      mcontext4: return rvvi_monitor_vif.mcontext4;
      mcontext5: return rvvi_monitor_vif.mcontext5;
      mcontext6: return rvvi_monitor_vif.mcontext6;
      mcontext7: return rvvi_monitor_vif.mcontext7;
      mcontext7: return rvvi_monitor_vif.mcontext7;
      mpc:       return rvvi_monitor_vif.mpc;
      msp:       return rvvi_monitor_vif.msp;
      kisa:      return rvvi_monitor_vif.kisa;
      kscm0:     return rvvi_monitor_vif.kscm0;
      kscm1:     return rvvi_monitor_vif.kscm1;
      kscm2:     return rvvi_monitor_vif.kscm2;
      kscm3:     return rvvi_monitor_vif.kscm3;
      kscm4:     return rvvi_monitor_vif.kscm4;
      default:   return 32'h0;
    endcase
  endfunction

  // **********************************************************
  // task: bit_manipulation_insn_decode
  // Parse out the Zbb instruction details
  // **********************************************************
  virtual function void bit_manipulation_insn_decode(coralnpu_rv32zbb_transaction tr,
                                                     fmt_typ_enum fmt_typ);
    bit [ 2:0] funct3 = tr.insn[14:12];
    bit [ 6:0] funct7 = tr.insn[31:25];
    bit [ 6:0] opcode = tr.insn[6:0];
    bit [11:0] imm = tr.insn[31:20];

    tr.rd_addr = tr.insn[11:7];
    tr.rs1_addr = tr.insn[19:15];
    tr.rd_val = gpr_reg_val[tr.rd_addr];
    tr.rs1_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    tr.shamt = tr.insn[24:20];
    if (opcode == 7'b0110011 && tr.inst_name != ZEXT_H) begin
      tr.rs2_addr = tr.insn[24:20];
      tr.rs2_val  = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];
    end
    if (opcode == 7'b0110011) begin
      case (funct7)
        7'b0100000: begin
          case (funct3)
            3'b111: tr.inst_name = ANDN;
            3'b110: tr.inst_name = ORN;
            3'b100: tr.inst_name = XNOR;
          endcase
        end
        7'b0000101: begin
          case (funct3)
            3'b110: tr.inst_name = MAX;
            3'b111: tr.inst_name = MAXU;
            3'b100: tr.inst_name = MIN;
            3'b101: tr.inst_name = MINU;
          endcase
        end
        7'b0000100: begin
          case (funct3)
            3'b100: tr.inst_name = ZEXT_H;
          endcase
        end
        7'b0110000: begin
          case (funct3)
            3'b001: tr.inst_name = ROL;
            3'b101: tr.inst_name = ROR;
          endcase
        end
      endcase
    end else begin
      if (imm == 12'b011000000000 && funct3 == 3'b001) tr.inst_name = CLZ;
      else if (imm == 12'b011000000001 && funct3 == 3'b001) tr.inst_name = CTZ;
      else if (imm == 12'b011000000010 && funct3 == 3'b001) tr.inst_name = CPOP;
      else if (imm == 12'b011000000100 && funct3 == 3'b001) tr.inst_name = SEXT_B;
      else if (imm == 12'b011000000101 && funct3 == 3'b001) tr.inst_name = SEXT_H;
      else if (funct7 == 7'b0110000 && funct3 == 3'b101) tr.inst_name = RORI;
      else if (imm == 12'b001010000111 && funct3 == 3'b101) tr.inst_name = ORC_B;
      else if (imm == 12'b011010011000 && funct3 == 3'b101) tr.inst_name = REV8;
    end
    //GPR Hazard check
    if (tr.rd_addr == rvvi_monitor_vif.rd_pre && tr.rd_addr != 0) begin
      tr.waw_hazard_hit = 1;
    end
    if (opcode == 7'b0110011 && tr.inst_name != ZEXT_H) begin
      if((tr.rd_addr == rvvi_monitor_vif.rs1_pre && tr.rd_addr !=0) || (tr.rd_addr == rvvi_monitor_vif.rs2_pre && tr.rd_addr !=0))begin
        tr.war_hazard_hit = 1;
      end
    end else begin
      if (tr.rd_addr == rvvi_monitor_vif.rs1_pre && tr.rd_addr != 0) begin
        tr.war_hazard_hit = 1;
      end
    end
    if (opcode == 7'b0110011 && tr.inst_name != ZEXT_H) begin
      if((tr.rs1_addr == rvvi_monitor_vif.rd_pre && rvvi_monitor_vif.rd_pre != 0) ||
           (tr.rs2_addr == rvvi_monitor_vif.rd_pre && rvvi_monitor_vif.rd_pre != 0)) begin
        tr.raw_hazard_hit = 1;
      end
    end else begin
      if (tr.rs1_addr == rvvi_monitor_vif.rd_pre && rvvi_monitor_vif.rd_pre != 0) begin
        tr.raw_hazard_hit = 1;
      end
    end
    //Update rs1_pre,rd_pre
    rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
    if (opcode == 7'b0110011 && tr.inst_name != ZEXT_H) begin
      rvvi_monitor_vif.rs2_pre = tr.rs2_addr;
    end
    rvvi_monitor_vif.rd_pre = tr.rd_addr;
  endfunction

  // **********************************************************
  // task: fencei_insn_decode
  // Parse out the instruction-fetch fence details
  // **********************************************************
  virtual function void fencei_insn_decode(coralnpu_rv32zifencei_transaction tr,
                                           fmt_typ_enum fmt_typ);
    tr.funct12 = tr.insn[31:20];
    tr.rd_addr = tr.insn[11:7];
    tr.rs1_addr = tr.insn[19:15];
    tr.rd_val = gpr_reg_val[tr.rd_addr];
    tr.rs1_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    tr.inst_name = FENCE_I;
    rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
    rvvi_monitor_vif.rd_pre = tr.rd_addr;
  endfunction

  // **********************************************************
  // task: mult_div_insn_decode
  // Parse out the Multiplication and Division instruction details
  // **********************************************************
  virtual function void mult_div_insn_decode(coralnpu_rv32m_transaction tr, fmt_typ_enum fmt_typ);
    bit [2:0] funct3;

    funct3      = tr.insn[14:12];
    tr.rd_addr  = tr.insn[11:7];
    tr.rs1_addr = tr.insn[19:15];
    tr.rs2_addr = tr.insn[24:20];
    tr.rd_val   = gpr_reg_val[tr.rd_addr];
    tr.rs1_val  = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    tr.rs2_val  = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];

    case (funct3)
      3'b000: begin
        tr.inst_name = MUL;
      end
      3'b001: begin
        tr.inst_name = MULH;
      end
      3'b010: begin
        tr.inst_name = MULHSU;
      end
      3'b011: begin
        tr.inst_name = MULHU;
      end
      3'b100: begin
        tr.inst_name = DIV;
      end
      3'b101: begin
        tr.inst_name = DIVU;
      end
      3'b110: begin
        tr.inst_name = REM;
      end
      3'b111: begin
        tr.inst_name = REMU;
      end
    endcase
    //GPR Hazard check
    if (tr.rd_addr == rvvi_monitor_vif.rd_pre) begin
      tr.waw_hazard_hit = 1;
    end
    if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre) begin
      tr.war_hazard_hit = 1;
    end
    if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.rs2_addr == rvvi_monitor_vif.rd_pre) begin
      tr.raw_hazard_hit = 1;
    end
    //Update rs1_pre,rs2_pre,rd_pre
    rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
    rvvi_monitor_vif.rs2_pre = tr.rs2_addr;
    rvvi_monitor_vif.rd_pre  = tr.rd_addr;
  endfunction

  // **********************************************************
  // task: vector_insn_decode
  // Parse out the vector instruction details
  // args0:  transaction handle that store instrction details
  // args1:  instrcuction format type
  // **********************************************************
  virtual function void vector_insn_decode(coralnpu_rv32v_transaction tr, fmt_typ_enum fmt_typ);
    bit [31:0] bin_inst = tr.insn;
    tr.inst_type = fmt_typ;
    tr.vm        = bin_inst[25];
    case (tr.inst_type)
      LD_Type, ST_Type: begin
        tr.lsu_nf    = lsu_nf_e'(bin_inst[31:29]);
        tr.lsu_mop   = lsu_mop_e'(bin_inst[27:26]);
        tr.lsu_width = lsu_width_e'(bin_inst[14:12]);
        if (tr.lsu_width == LSU_8BIT) tr.lsu_eew = eew_e'(EEW8);
        if (tr.lsu_width == LSU_16BIT) tr.lsu_eew = eew_e'(EEW16);
        if (tr.lsu_width == LSU_32BIT) tr.lsu_eew = eew_e'(EEW32);
        if (tr.lsu_width == LSU_64BIT) tr.lsu_eew = eew_e'(EEW64);
      end
      ALU_Type: begin
        tr.alu_inst[5:0] = bin_inst[31:26];
        tr.alu_type      = alu_type_e'(bin_inst[14:12]);
      end
      VSET_Type: begin
        tr.vset = bin_inst[31:26];
        tr.vm   = 1;
      end
    endcase
    case (tr.inst_type)
      ST_Type: begin
        tr.vd_addr = bin_inst[11:7];
        tr.vd_val  = v_reg_val[tr.vd_addr];
      end
      LD_Type, ALU_Type: begin
        tr.src3_type = UNUSE;
      end
      VSET_Type: begin
        tr.rd_addr = bin_inst[11:7];
        tr.rd_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
        tr.src3_type = UNUSE;
      end
    endcase
    //VSET decode
    if (tr.inst_type == VSET_Type) begin
      tr.rd_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
      if (tr.vset[5] == 0) begin  // vsetvli
        tr.vset_inst           = VSETVLI;
        tr.rs1_addr            = bin_inst[19:15];
        tr.rs1_val             = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
        rvvi_monitor_vif.avl   = tr.rs1_val;
        rvvi_monitor_vif.vlmul = bin_inst[22:20];
        rvvi_monitor_vif.vsew  = bin_inst[25:23];
        rvvi_monitor_vif.vta   = bin_inst[26];
        rvvi_monitor_vif.vma   = bin_inst[27];
        tr.src2_type           = operand_type_e'(IMM);
        tr.imm                 = bin_inst[24:20];
        //Hazard check
        if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
        if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
          tr.war_hazard_hit = 1;
        if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
        //Update rs1_pre,rd_pre
        rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
        rvvi_monitor_vif.rd_pre  = tr.rd_addr;
      end else if (tr.vset[5:4] == 2'b11) begin  //vsetivli
        tr.vset_inst           = VSETIVLI;
        rvvi_monitor_vif.avl   = bin_inst[19:15];
        rvvi_monitor_vif.vlmul = bin_inst[22:20];
        rvvi_monitor_vif.vsew  = bin_inst[25:23];
        rvvi_monitor_vif.vta   = bin_inst[26];
        rvvi_monitor_vif.vma   = bin_inst[27];
        tr.src1_type           = operand_type_e'(UIMM);
        tr.uimm                = bin_inst[19:15];
        if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
        //Update rd_pre
        rvvi_monitor_vif.rd_pre = tr.rd_addr;
      end else if (tr.vset == 6'b100000) begin  //vsetvl
        tr.vset_inst           = VSETVL;
        tr.rs1_addr            = bin_inst[19:15];
        tr.rs2_addr            = bin_inst[24:20];
        tr.rs1_val             = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
        tr.rs2_val             = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];
        rvvi_monitor_vif.avl   = tr.rs1_val;
        rvvi_monitor_vif.vlmul = tr.rs2_val[2:0];
        rvvi_monitor_vif.vsew  = tr.rs2_val[5:3];
        rvvi_monitor_vif.vta   = tr.rs2_val[6];
        rvvi_monitor_vif.vma   = tr.rs2_val[7];
        //GPR Hazard check
        if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
        if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
          tr.war_hazard_hit = 1;
        if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.rs2_addr == rvvi_monitor_vif.rd_pre)
          tr.raw_hazard_hit = 1;
        //Update rs1_pre,rs2_pre,rd_pre
        rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
        rvvi_monitor_vif.rs2_pre = tr.rs2_addr;
        rvvi_monitor_vif.rd_pre  = tr.rd_addr;
      end
    end
    tr.avl   = rvvi_monitor_vif.avl;
    tr.vlmul = lmul_e'(rvvi_monitor_vif.vlmul);
    tr.vsew  = sew_e'(rvvi_monitor_vif.vsew);
    tr.vta   = agnostic_e'(rvvi_monitor_vif.vta);
    tr.vma   = agnostic_e'(rvvi_monitor_vif.vma);
    //ALU decode
    if (tr.inst_type == ALU_Type) begin
      if (tr.alu_type inside {OPIVV, OPIVX, OPIVI}) begin
        tr.alu_inst[7:6] = 2'b00;
        if (tr.alu_type == OPIVV) begin
          if (tr.alu_inst inside {VMERGE_VMVV} && tr.vm == 1) begin  //vmv.v.v
            tr.dest_type = operand_type_e'(VRF);
            tr.src2_type = operand_type_e'(UNUSE);
            tr.src1_type = operand_type_e'(VRF);
            tr.vs1_addr  = bin_inst[19:15];
            tr.vs1_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs1_addr];
            tr.vd_addr   = bin_inst[11:7];
            tr.vd_val    = v_reg_val[tr.vd_addr];
            if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
            if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
              tr.war_hazard_hit = 1;
            if (tr.vs1_addr == rvvi_monitor_vif.vd_pre) tr.raw_hazard_hit = 1;
            //Update vs1_pre,vd_pre
            rvvi_monitor_vif.vs1_pre = tr.vs1_addr;
            rvvi_monitor_vif.vd_pre  = tr.vd_addr;
          end else begin
            tr.dest_type = operand_type_e'(VRF);
            tr.src2_type = operand_type_e'(VRF);
            tr.src1_type = operand_type_e'(VRF);
            tr.vs1_addr  = bin_inst[19:15];
            tr.vs1_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs1_addr];
            tr.vs2_addr  = bin_inst[24:20];
            tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
            tr.vd_addr   = bin_inst[11:7];
            tr.vd_val    = v_reg_val[tr.vd_addr];
            //V Register Hazard check
            if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
            if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
              tr.war_hazard_hit = 1;
            if (tr.vs1_addr == rvvi_monitor_vif.vd_pre || tr.vs2_addr == rvvi_monitor_vif.vd_pre)
              tr.raw_hazard_hit = 1;
            //Update vs1_pre,vs2_pre,vd_pre
            rvvi_monitor_vif.vs1_pre = tr.vs1_addr;
            rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
            rvvi_monitor_vif.vd_pre  = tr.vd_addr;
          end
        end
        if (tr.alu_type == OPIVX) begin
          if (tr.alu_inst inside {VMERGE_VMVV} && tr.vm == 1) begin  //vmv.v.x
            tr.vd_addr   = bin_inst[11:7];
            tr.vd_val    = v_reg_val[tr.vd_addr];
            tr.rs1_addr  = bin_inst[19:15];
            tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
            tr.dest_type = operand_type_e'(VRF);
            tr.src2_type = operand_type_e'(UNUSE);
            tr.src1_type = operand_type_e'(XRF);
            //Register Hazard check
            if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
            if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
              tr.war_hazard_hit = 1;
            if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
            //Update rs1_pre,vd_pre
            rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
            rvvi_monitor_vif.vd_pre  = tr.vd_addr;
          end else begin
            tr.vd_addr   = bin_inst[11:7];
            tr.vd_val    = v_reg_val[tr.vd_addr];
            tr.rs1_addr  = bin_inst[19:15];
            tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
            tr.vs2_addr  = bin_inst[24:20];
            tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
            tr.dest_type = operand_type_e'(VRF);
            tr.src2_type = operand_type_e'(VRF);
            tr.src1_type = operand_type_e'(XRF);
            //Register Hazard check
            if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
            if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
              tr.war_hazard_hit = 1;
            if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.vs2_addr == rvvi_monitor_vif.vd_pre)
              tr.raw_hazard_hit = 1;
            //Update rs1_pre,vs2_pre,vd_pre
            rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
            rvvi_monitor_vif.vd_pre  = tr.vd_addr;
            rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          end
        end
        if (tr.alu_type == OPIVI) begin
          //dest type
          tr.dest_type    = operand_type_e'(VRF);
          tr.vd_addr = bin_inst[11:7];
          tr.vd_val  = v_reg_val[tr.vd_addr];
          //src1 type
          if(tr.alu_inst inside {VSLL, VSRL, VSRA, VNSRL, VNSRA,
                      VSSRL, VSSRA, VNCLIPU, VNCLIP,VRGATHER}) begin //uimm
            tr.src1_type = UIMM;
            tr.uimm      = bin_inst[19:15];
          end else if (tr.alu_inst inside {VSMUL_VMVNRR})
            tr.src1_type = operand_type_e'(UNUSE);  //not have rs1, nr=1,2,4,8
          else begin  //imm
            tr.src1_type = IMM;
            tr.imm       = bin_inst[19:15];
          end
          //src2 type
          if (tr.alu_inst inside {VMERGE_VMVV} && tr.vm == 1)
            tr.src2_type = operand_type_e'(UNUSE);  //vmv.v.i
          else begin
            tr.src2_type = operand_type_e'(VRF);
            tr.vs2_addr  = bin_inst[24:20];
            tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
            if (tr.vs2_addr == rvvi_monitor_vif.vd_pre) tr.raw_hazard_hit = 1;
            //Update vs2_pre
            rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          end
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          //Update vd_pre
          rvvi_monitor_vif.vd_pre = tr.vd_addr;
        end
      end  //OPI
      //OPM
      if (tr.alu_type inside {OPMVV, OPMVX}) begin
        tr.alu_inst[7:6] = 2'b01;
        if (tr.alu_type == OPMVV) begin
          //dest type
          //vcpop.m, vfirst.m and vmv.x.s writes the result to a scalar x register
          if (tr.alu_inst == VWXUNARY0) begin
            tr.dest_type = operand_type_e'(XRF);
            tr.rd_addr   = bin_inst[11:7];
            tr.rd_val    = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
            if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
            if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
              tr.war_hazard_hit = 1;
            rvvi_monitor_vif.rd_pre = tr.rd_addr;
          end else begin
            tr.dest_type = operand_type_e'(VRF);
            tr.vd_addr   = bin_inst[11:7];
            tr.vd_val    = v_reg_val[tr.vd_addr];
            if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
            if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
              tr.war_hazard_hit = 1;
            rvvi_monitor_vif.vd_pre = tr.vd_addr;
          end
          //source2 type
          //vid.v not have src2
          if (tr.alu_inst == VMUNARY0 && bin_inst[19:15] == vmunary0_e'(VID))
            tr.src2_type = operand_type_e'(UNUSE);
          else begin
            tr.src2_type = operand_type_e'(VRF);
            tr.vs2_addr  = bin_inst[24:20];
            tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
            if (tr.vs2_addr == rvvi_monitor_vif.vd_pre) tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          end
          //source1 type
          //vzext/vsext, vmsbf, vmsof, vmsif, viota, vid, vcpop.m, vfirst.m and vmv.x.s
          if (tr.alu_inst == VXUNARY0 || tr.alu_inst == VWXUNARY0 || tr.alu_inst == VMUNARY0)
            tr.src1_type = operand_type_e'(UNUSE);
          else begin
            tr.src1_type = operand_type_e'(VRF);
            tr.vs1_addr  = bin_inst[19:15];
            tr.vs1_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs1_addr];
            if (tr.vs1_addr == rvvi_monitor_vif.vd_pre) tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.vs1_pre = tr.vs1_addr;
          end
        end
        if (tr.alu_type == OPMVX) begin
          tr.vd_addr   = bin_inst[11:7];
          tr.vd_val    = v_reg_val[tr.vd_addr];
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.dest_type = operand_type_e'(VRF);
          tr.src1_type = operand_type_e'(XRF);
          if (tr.alu_inst == VWXUNARY0) begin  //vmv.s.x  vd(0),not have vs2
            tr.src2_type = operand_type_e'(UNUSE);
          end else begin
            tr.src2_type = operand_type_e'(VRF);
            tr.vs2_addr  = bin_inst[24:20];
            tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
            if (tr.vs2_addr == rvvi_monitor_vif.vd_pre) tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          end
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
          //Update rs1_pre,vs2_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
      end  //OPM
    end  //ALU

    //LD
    if (tr.inst_type == LD_Type) begin
      tr.vd_addr = bin_inst[11:7];
      tr.vd_val = v_reg_val[tr.vd_addr];
      tr.src3_type = operand_type_e'(UNUSE);
      case (tr.lsu_mop)
        LSU_US: begin  //vd rs1
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.dest_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(UNUSE);
          tr.src1_type = operand_type_e'(XRF);
          tr.lsu_umop  = lsu_umop_e'(bin_inst[24:20]);
          case (tr.lsu_umop)
            NORMAL: begin  //unit-stride
              case (tr.lsu_nf)
                NF1: begin
                  tr.lsu_inst = VL;
                end
                NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
                  tr.lsu_inst = VLSEG;
                end
              endcase
            end
            FOF: begin  //unit-stride fault-only-first
              case (tr.lsu_nf)
                NF1: begin
                  tr.lsu_inst = VLFF;
                end
                NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
                  tr.lsu_inst = VLSEGFF;
                end
              endcase
            end
            MASK: begin
              tr.lsu_inst = VLM;
            end
            WHOLE_REG: begin
              tr.lsu_inst = VLR;
            end
          endcase
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
          //Update rs1_pre,vs2_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
        LSU_CS: begin  //vd rs1 rs2
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.rs2_addr  = bin_inst[24:20];
          tr.rs2_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];
          tr.dest_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(XRF);
          tr.src1_type = operand_type_e'(XRF);
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VLS;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VLSSEG;
            end
          endcase
          // Hazard check vd rs1 rs2
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.rs2_addr == rvvi_monitor_vif.rd_pre)
            tr.raw_hazard_hit = 1;
          //Update rs1_pre,rs2_pre,vd_pre
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.rs2_pre = tr.rs2_addr;
        end
        LSU_UI, LSU_OI: begin  //vd rs1 vs2
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.vs2_addr  = bin_inst[24:20];
          tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
          tr.dest_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(VRF);
          tr.src1_type = operand_type_e'(XRF);
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.vs2_addr == rvvi_monitor_vif.vd_pre)
            tr.raw_hazard_hit = 1;
          //Update rs1_pre,vs2_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
        LSU_UI: begin  //vs3 vs2 rs1
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VLUX;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VLUXSEG;
            end
          endcase
        end
        LSU_OI: begin
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VLOX;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VLOXSEG;
            end
          endcase
        end
      endcase
    end  //LD
    //STORE
    if (tr.inst_type == ST_Type) begin
      tr.vd_addr = bin_inst[11:7];
      tr.vd_val = v_reg_val[tr.vd_addr];
      tr.dest_type = operand_type_e'(UNUSE);
      case (tr.lsu_mop)
        LSU_US: begin  //vs3 rs1
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.src3_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(UNUSE);
          tr.src1_type = operand_type_e'(XRF);
          tr.lsu_umop  = lsu_umop_e'(bin_inst[24:20]);
          case (tr.lsu_umop)
            NORMAL: begin
              case (tr.lsu_nf)
                NF1: begin
                  tr.lsu_inst = VS;
                end
                NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
                  tr.lsu_inst = VSSEG;
                end
              endcase
            end
            MASK: begin
              tr.lsu_inst = VSM;
            end
            WHOLE_REG: begin
              tr.lsu_inst = VSR;
            end
          endcase
          //Register Hazard check vs3 rs1
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
          //Update rs1_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
        LSU_CS: begin  //vs3 rs1 rs2
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.rs2_addr  = bin_inst[24:20];
          tr.rs2_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];
          tr.src3_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(XRF);
          tr.src1_type = operand_type_e'(XRF);
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VSS;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VSSSEG;
            end
          endcase
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.rs2_addr == rvvi_monitor_vif.rd_pre)
            tr.raw_hazard_hit = 1;
          //Update rs1_pre,rs2_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.rs2_pre = tr.rs2_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
        LSU_UI, LSU_OI: begin  //vs3 rs1 vs2
          tr.rs1_addr  = bin_inst[19:15];
          tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
          tr.vs2_addr  = bin_inst[24:20];
          tr.vs2_val   = rvvi_monitor_vif.v_reg_val_pre[tr.vs2_addr];
          tr.src3_type = operand_type_e'(VRF);
          tr.src2_type = operand_type_e'(VRF);
          tr.src1_type = operand_type_e'(XRF);
          //Register Hazard check
          if (tr.vd_addr == rvvi_monitor_vif.vd_pre) tr.waw_hazard_hit = 1;
          if (tr.vd_addr == rvvi_monitor_vif.vs1_pre || tr.vd_addr == rvvi_monitor_vif.vs2_pre)
            tr.war_hazard_hit = 1;
          if (tr.rs1_addr == rvvi_monitor_vif.rd_pre || tr.vs2_addr == rvvi_monitor_vif.vd_pre)
            tr.raw_hazard_hit = 1;
          //Update rs1_pre,rs2_pre,vd_pre
          rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
          rvvi_monitor_vif.vs2_pre = tr.vs2_addr;
          rvvi_monitor_vif.vd_pre  = tr.vd_addr;
        end
        LSU_UI: begin
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VSUX;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VSUXSEG;
            end
          endcase
        end
        LSU_OI: begin
          case (tr.lsu_nf)
            NF1: begin
              tr.lsu_inst = VSOX;
            end
            NF2, NF3, NF4, NF5, NF6, NF7, NF8: begin
              tr.lsu_inst = VSOXSEG;
            end
          endcase
        end
      endcase
    end  //ST
    //cast special inst
    if(tr.src1_type == UNUSE && tr.inst_type == ALU_Type && tr.alu_inst == VXUNARY0) begin//zero/sign extend
      if (!$cast(tr.src1_func_vext, tr.insn[19:15])) tr.src1_func_vext = VXUNARY0_NONE;
    end
    if (tr.src1_type == UNUSE && tr.inst_type == ALU_Type && tr.alu_inst == VWXUNARY0) begin
      if (!$cast(tr.src1_func_vwxunary0, tr.insn[19:15])) tr.src1_func_vwxunary0 = VWXUNARY0_NONE;
    end
    if (tr.src2_type == UNUSE && tr.inst_type == ALU_Type && tr.alu_inst == VWXUNARY0) begin
      if (!$cast(tr.src2_func_vwxunary0, tr.insn[24:20])) tr.src2_func_vwxunary0 = VWXUNARY0_NONE;
    end
    if (tr.src1_type == UNUSE && tr.inst_type == ALU_Type && tr.alu_inst == VMUNARY0) begin
      if (!$cast(tr.src1_func_vmunary0, tr.insn[19:15])) tr.src1_func_vmunary0 = VMUNARY0_NONE;
    end

    if (tr.src2_type == XRF) tr.rs2_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs2_addr];
    else tr.rs2_val = 'x;
    if (tr.src1_type == XRF) tr.rs1_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    else tr.rs1_val = 'x;
    if (tr.dest_type == XRF) tr.rd_val = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
    else tr.rd_val = 'x;
    decode_vtype(tr);
    asm_string_gen(tr);
  endfunction

  // **********************************************************
  // task: floating_point_insn_decode
  // Decode scalar floating point instructions
  // **********************************************************
  virtual function void floating_point_insn_decode(coralnpu_rv32f_transaction tr,
                                                   fmt_typ_enum fmt_typ);
    bit [31:0] bin_inst = tr.insn;

    case (bin_inst[6:0])
      7'b000_0111: begin
        tr.inst_name = FLW;
        tr.imm_12bit = bin_inst[31:20];
        tr.fd_addr   = bin_inst[11:7];
        tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
        tr.rs1_addr  = bin_inst[19:15];
        tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
        //Hazard check and update
        if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
        if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.fd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
          tr.war_hazard_hit = 1;
        if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
        rvvi_monitor_vif.fd_pre  = tr.fd_addr;
        rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
      end
      7'b010_0111: begin
        tr.inst_name = FSW;
        tr.imm_12bit = {bin_inst[31:25], bin_inst[11:7]};
        tr.fs2_addr  = bin_inst[24:20];
        tr.fs2_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
        tr.rs1_addr  = bin_inst[19:15];
        tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
        //Hazard check and update
        if (tr.fs2_addr == rvvi_monitor_vif.fd_pre || tr.rs1_addr == rvvi_monitor_vif.rd_pre)
          tr.raw_hazard_hit = 1;
        rvvi_monitor_vif.fs2_pre = tr.fs2_addr;
        rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
      end
      7'b101_0011: begin
        if (bin_inst[31:29] == 0 || bin_inst[31:29] == 1 || bin_inst[31:29] == 5) begin
          tr.fs2_addr = bin_inst[24:20];
          tr.fs2_val  = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
          tr.fs1_addr = bin_inst[19:15];
          tr.fs1_val  = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
        end
        case (bin_inst[31:29])
          3'b000: begin
            tr.rm      = bin_inst[14:12];
            tr.fd_addr = bin_inst[11:7];
            tr.fd_val  = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
            if (bin_inst[28:27] == 0) tr.inst_name = FADD_S;
            else if (bin_inst[28:27] == 1) tr.inst_name = FSUB_S;
            else if (bin_inst[28:27] == 2) tr.inst_name = FMUL_S;
            else tr.inst_name = FDIV_S;
            //Hazard check and update
            if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
            if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.fd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
              tr.war_hazard_hit = 1;
            if (tr.fs1_addr == rvvi_monitor_vif.fd_pre || tr.fs2_addr == rvvi_monitor_vif.fd_pre)
              tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.fd_pre  = tr.fd_addr;
            rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            rvvi_monitor_vif.fs2_pre = tr.fs2_addr;
          end
          3'b001: begin
            tr.fd_addr = bin_inst[11:7];
            tr.fd_val  = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
            if (bin_inst[28:27] == 1 && bin_inst[14:12] == 0) tr.inst_name = FMIN_S;
            if (bin_inst[28:27] == 1 && bin_inst[14:12] == 1) tr.inst_name = FMAX_S;
            if (bin_inst[28:27] == 0 && bin_inst[14:12] == 0) tr.inst_name = FSGNJ_S;
            if (bin_inst[28:27] == 0 && bin_inst[14:12] == 1) tr.inst_name = FSGNJN_S;
            if (bin_inst[28:27] == 0 && bin_inst[14:12] == 2) tr.inst_name = FSGNJX_S;
            //Hazard check and update
            if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
            if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.fd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
              tr.war_hazard_hit = 1;
            if (tr.fs1_addr == rvvi_monitor_vif.fd_pre || tr.fs2_addr == rvvi_monitor_vif.fd_pre)
              tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.fd_pre  = tr.fd_addr;
            rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            rvvi_monitor_vif.fs2_pre = tr.fs2_addr;
          end
          3'b101: begin
            tr.rd_addr = bin_inst[11:7];
            tr.rd_val  = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
            if (bin_inst[14:12] == 0) tr.inst_name = FLE_S;
            if (bin_inst[14:12] == 1) tr.inst_name = FLT_S;
            if (bin_inst[14:12] == 2) tr.inst_name = FEQ_S;
            //Hazard check and update
            if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
            if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
              tr.war_hazard_hit = 1;
            if (tr.fs1_addr == rvvi_monitor_vif.fd_pre || tr.fs2_addr == rvvi_monitor_vif.fd_pre)
              tr.raw_hazard_hit = 1;
            rvvi_monitor_vif.rd_pre  = tr.rd_addr;
            rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            rvvi_monitor_vif.fs2_pre = tr.fs2_addr;
          end
          3'b110: begin  //not have src2
            tr.rm = bin_inst[14:12];
            if (bin_inst[28:27] == 0 && bin_inst[24:20] == 0) begin
              tr.inst_name = FCVT_W_S;
              tr.rd_addr   = bin_inst[11:7];
              tr.rd_val    = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
              tr.fs1_addr  = bin_inst[19:15];
              tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
              //Hazard check and update
              if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
              if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
                tr.war_hazard_hit = 1;
              if (tr.fs1_addr == rvvi_monitor_vif.fd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.rd_pre  = tr.rd_addr;
              rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            end else if (bin_inst[28:27] == 0 && bin_inst[24:20] == 1) begin
              tr.inst_name = FCVT_WU_S;
              tr.rd_addr   = bin_inst[11:7];
              tr.rd_val    = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
              tr.fs1_addr  = bin_inst[19:15];
              tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
              //Hazard check and update
              if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
              if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
                tr.war_hazard_hit = 1;
              if (tr.fs1_addr == rvvi_monitor_vif.fd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.rd_pre  = tr.rd_addr;
              rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            end else if (bin_inst[28:27] == 2 && bin_inst[24:20] == 0) begin
              tr.inst_name = FCVT_S_W;
              tr.fd_addr   = bin_inst[11:7];
              tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
              tr.rs1_addr  = bin_inst[19:15];
              tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
              //Hazard check and update
              if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
              if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.rd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
                tr.war_hazard_hit = 1;
              if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.fd_pre  = tr.fd_addr;
              rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
            end else if (bin_inst[28:27] == 2 && bin_inst[24:20] == 1) begin
              tr.inst_name = FCVT_S_WU;
              tr.fd_addr   = bin_inst[11:7];
              tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
              tr.rs1_addr  = bin_inst[19:15];
              tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
              //Hazard check and update
              if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
              if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.rd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
                tr.war_hazard_hit = 1;
              if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.fd_pre  = tr.fd_addr;
              rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
            end
          end
          3'b111: begin  //not have src2
            if (bin_inst[28:27] == 0 && bin_inst[14:12] == 0) begin
              tr.inst_name = FMV_X_W;
              tr.rd_addr   = bin_inst[11:7];
              tr.rd_val    = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
              tr.fs1_addr  = bin_inst[19:15];
              tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
              //Hazard check and update
              if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
              if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
                tr.war_hazard_hit = 1;
              if (tr.fs1_addr == rvvi_monitor_vif.fd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.rd_pre  = tr.rd_addr;
              rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            end else if (bin_inst[28:27] == 0 && bin_inst[14:12] == 1) begin
              tr.inst_name = FCLASS;
              tr.rd_addr   = bin_inst[11:7];
              tr.rd_val    = rvvi_monitor_vif.gpr_reg_val_pre[tr.rd_addr];
              tr.fs1_addr  = bin_inst[19:15];
              tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
              //Hazard check and update
              if (tr.rd_addr == rvvi_monitor_vif.rd_pre) tr.waw_hazard_hit = 1;
              if (tr.rd_addr == rvvi_monitor_vif.rs1_pre || tr.rd_addr == rvvi_monitor_vif.rs2_pre)
                tr.war_hazard_hit = 1;
              if (tr.fs1_addr == rvvi_monitor_vif.fd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.rd_pre  = tr.rd_addr;
              rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            end else if (bin_inst[28:27] == 2 && bin_inst[14:12] == 0) begin
              tr.inst_name = FMV_W_X;
              tr.fd_addr   = bin_inst[11:7];
              tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
              tr.rs1_addr  = bin_inst[19:15];
              tr.rs1_val   = rvvi_monitor_vif.gpr_reg_val_pre[tr.rs1_addr];
              //Hazard check and update
              if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
              if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.rd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
                tr.war_hazard_hit = 1;
              if (tr.rs1_addr == rvvi_monitor_vif.rd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.fd_pre  = tr.fd_addr;
              rvvi_monitor_vif.rs1_pre = tr.rs1_addr;
            end
          end
          3'b010: begin  //not have src2
            if (bin_inst[28:27] == 2'b11 && bin_inst[24:20] == 0) begin
              tr.rm        = bin_inst[14:12];
              tr.inst_name = FSQRT_S;
              tr.fd_addr   = bin_inst[11:7];
              tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
              tr.fs1_addr  = bin_inst[19:15];
              tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
              //Hazard check and update
              if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
              if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.rd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
                tr.war_hazard_hit = 1;
              if (tr.fs1_addr == rvvi_monitor_vif.fd_pre) tr.raw_hazard_hit = 1;
              rvvi_monitor_vif.fd_pre  = tr.fd_addr;
              rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
            end
          end
        endcase
      end
      7'b100_0011: begin  //use src3
        tr.rm        = bin_inst[14:12];
        tr.inst_name = FMADD_S;
        tr.fs3_addr  = bin_inst[31:27];
        tr.fs3_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs3_addr];
        tr.fs2_addr  = bin_inst[24:20];
        tr.fs2_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
        tr.fs1_addr  = bin_inst[19:15];
        tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
        tr.fd_addr   = bin_inst[11:7];
        tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
      end
      7'b100_0111: begin  //use src3
        tr.rm        = bin_inst[14:12];
        tr.inst_name = FMSUB_S;
        tr.fs3_addr  = bin_inst[31:27];
        tr.fs3_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs3_addr];
        tr.fs2_addr  = bin_inst[24:20];
        tr.fs2_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
        tr.fs1_addr  = bin_inst[19:15];
        tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
        tr.fd_addr   = bin_inst[11:7];
        tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
      end
      7'b100_1011: begin  //use src3
        tr.rm        = bin_inst[14:12];
        tr.inst_name = FNMSUB_S;
        tr.fs3_addr  = bin_inst[31:27];
        tr.fs3_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs3_addr];
        tr.fs2_addr  = bin_inst[24:20];
        tr.fs2_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
        tr.fs1_addr  = bin_inst[19:15];
        tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
        tr.fd_addr   = bin_inst[11:7];
        tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
      end
      7'b100_1111: begin  //use src3
        tr.rm        = bin_inst[14:12];
        tr.inst_name = FNMADD_S;
        tr.fs3_addr  = bin_inst[31:27];
        tr.fs3_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs3_addr];
        tr.fs2_addr  = bin_inst[24:20];
        tr.fs2_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs2_addr];
        tr.fs1_addr  = bin_inst[19:15];
        tr.fs1_val   = rvvi_monitor_vif.f_reg_val_pre[tr.fs1_addr];
        tr.fd_addr   = bin_inst[11:7];
        tr.fd_val    = rvvi_monitor_vif.f_reg_val_pre[tr.fd_addr];
      end
    endcase
    if(tr.inst_name == FMADD_S || tr.inst_name == FMSUB_S || tr.inst_name == FNMSUB_S || tr.inst_name == FNMADD_S)begin
      //Hazard check and update
      if (tr.fd_addr == rvvi_monitor_vif.fd_pre) tr.waw_hazard_hit = 1;
      if(tr.fd_addr == rvvi_monitor_vif.fs1_pre || tr.fd_addr == rvvi_monitor_vif.fs2_pre || tr.fd_addr == rvvi_monitor_vif.fs3_pre)
        tr.war_hazard_hit = 1;
      if(tr.fs1_addr == rvvi_monitor_vif.fd_pre || tr.fs2_addr == rvvi_monitor_vif.fd_pre || tr.fs3_addr == rvvi_monitor_vif.fd_pre)
        tr.raw_hazard_hit = 1;
      rvvi_monitor_vif.fd_pre  = tr.fd_addr;
      rvvi_monitor_vif.fs1_pre = tr.fs1_addr;
      rvvi_monitor_vif.fs2_pre = tr.fs2_addr;
      rvvi_monitor_vif.fs3_pre = tr.fs3_addr;
    end
  endfunction

  // **********************************************************
  // task: decode_vtype
  // Decode vector configuration values
  // **********************************************************
  virtual function void decode_vtype(coralnpu_rv32v_transaction tr);
    // Calculate eew/emul
    if(tr.inst_type == ALU_Type && (tr.alu_inst inside {VADC, VSBC, VMADC, VMSBC, VMERGE_VMVV}))begin
      tr.use_vm_to_cal = 1;
      tr.v0_val = rvvi_monitor_vif.v_reg_val_pre[0];
    end else tr.use_vm_to_cal = 0;

    tr.is_widen_inst          = tr.inst_type == ALU_Type &&  (tr.alu_inst inside {VWADDU, VWADD, VWADDU_W, VWADD_W, VWSUBU, VWSUB, VWSUBU_W, VWSUB_W,
                                                                       VWMUL, VWMULU, VWMULSU, VWMACCU, VWMACC, VWMACCUS, VWMACCSU});
    tr.is_widen_vs2_inst      = tr.inst_type == ALU_Type &&  (tr.alu_inst inside {VWADD_W, VWADDU_W, VWSUBU_W, VWSUB_W});
    tr.is_narrow_inst         = tr.inst_type == ALU_Type &&  (tr.alu_inst inside {VNSRL, VNSRA, VNCLIPU, VNCLIP});
    tr.is_reduction_inst      = tr.inst_type == ALU_Type &&  (tr.alu_inst inside {VREDSUM, VREDAND, VREDOR, VREDXOR,
                                                                       VREDMINU,VREDMIN,VREDMAXU,VREDMAX,
                                                                       VWREDSUMU,VWREDSUM});
    tr.eew = 8 << tr.vsew;
    tr.dest_eew = tr.eew;
    tr.src3_eew = tr.eew;
    tr.src2_eew = tr.eew;
    tr.src1_eew = tr.eew;

    tr.emul = emul_e'(2.0 ** signed'(tr.vlmul));
    tr.dest_emul = tr.emul;
    tr.src3_emul = tr.emul;
    tr.src2_emul = tr.emul;
    tr.src1_emul = tr.emul;

    case (tr.inst_type)
      LD_Type: begin
        case (tr.lsu_mop)
          LSU_US: begin
            case (tr.lsu_umop)
              MASK: begin
                tr.dest_eew  = eew_e'(EEW8);
                tr.dest_emul = EMUL1;
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = int'($ceil(tr.avl / 8.0));
              end
              WHOLE_REG: begin
                tr.dest_eew  = tr.lsu_eew;
                tr.dest_emul = emul_e'(tr.lsu_nf + 1);
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = tr.dest_emul * `VLEN / tr.dest_eew;
              end
              default: begin
                tr.dest_eew  = tr.lsu_eew;
                tr.dest_emul = emul_e'(tr.dest_eew * tr.emul / tr.eew);
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = tr.avl;
              end
            endcase
          end
          LSU_CS: begin
            tr.dest_eew  = tr.lsu_eew;
            tr.dest_emul = emul_e'(tr.dest_eew * tr.emul / tr.eew);
            tr.src2_eew  = eew_e'(EEW32);
            tr.src2_emul = EMUL1;
            tr.src1_eew  = eew_e'(EEW32);
            tr.src1_emul = EMUL1;
            tr.evl       = tr.avl;
          end
          LSU_UI, LSU_OI: begin
            tr.dest_eew  = tr.eew;
            tr.dest_emul = tr.emul;
            tr.src2_eew  = tr.lsu_eew;
            tr.src2_emul = emul_e'(tr.src2_eew * tr.emul / tr.eew);
            tr.src1_eew  = eew_e'(EEW32);
            tr.src1_emul = EMUL1;
            tr.evl       = tr.avl;
          end
        endcase
      end
      ST_Type: begin
        case (tr.lsu_mop)
          LSU_US: begin
            case (tr.lsu_umop)
              MASK: begin
                tr.src3_eew  = eew_e'(EEW8);
                tr.src3_emul = EMUL1;
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = int'($ceil(tr.avl / 8.0));
              end
              WHOLE_REG: begin
                tr.src3_eew  = tr.lsu_eew;
                tr.src3_emul = emul_e'(tr.lsu_nf + 1);
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = tr.src3_emul * `VLEN / tr.src3_eew;
              end
              default: begin
                tr.src3_eew  = tr.lsu_eew;
                tr.src3_emul = emul_e'(tr.src3_eew * tr.emul / tr.eew);
                tr.src2_eew  = eew_e'(EEW_NONE);
                tr.src2_emul = EMUL_NONE;
                tr.src1_eew  = eew_e'(EEW32);
                tr.src1_emul = EMUL1;
                tr.evl       = tr.avl;
              end
            endcase
          end
          LSU_CS: begin
            tr.src3_eew  = tr.lsu_eew;
            tr.src3_emul = emul_e'(tr.src3_eew * tr.emul / tr.eew);
            tr.src2_eew  = eew_e'(EEW32);
            tr.src2_emul = EMUL1;
            tr.src1_eew  = eew_e'(EEW32);
            tr.src1_emul = EMUL1;
            tr.evl       = tr.avl;
          end
          LSU_UI, LSU_OI: begin
            tr.src3_eew  = tr.eew;
            tr.src3_emul = tr.emul;
            tr.src2_eew  = tr.lsu_eew;
            tr.src2_emul = emul_e'(tr.src2_eew * tr.emul / tr.eew);
            tr.src1_eew  = eew_e'(EEW32);
            tr.src1_emul = EMUL1;
            tr.evl       = tr.avl;
          end
        endcase
      end
      ALU_Type: begin
        // Widen
        if (tr.is_widen_inst) begin
          tr.dest_eew  = tr.dest_eew * 2;
          tr.dest_emul = emul_e'(tr.dest_emul * 2);
        end
        if (tr.is_widen_vs2_inst) begin
          tr.src2_eew  = tr.src2_eew * 2;
          tr.src2_emul = emul_e'(tr.src2_emul * 2);
        end
        // Narrow
        if (tr.is_narrow_inst) begin
          tr.src2_eew  = tr.src2_eew * 2;
          tr.src2_emul = emul_e'(tr.src2_emul * 2);
        end
        // vxunary0
        if (tr.alu_inst == VXUNARY0) begin
          if (tr.insn[19:15] inside {vxunary0_e'(VSEXT_VF2), vxunary0_e'(VZEXT_VF2)}) begin
            tr.src2_eew  = tr.src2_eew / 2;
            tr.src2_emul = emul_e'(tr.src2_emul / 2);
          end
          if (tr.insn[19:15] inside {vxunary0_e'(VSEXT_VF4), vxunary0_e'(VZEXT_VF4)}) begin
            tr.src2_eew  = tr.src2_eew / 4;
            tr.src2_emul = emul_e'(tr.src2_emul / 4);
          end
        end
        // mask producing inst
        if (tr.alu_inst inside {VMAND, VMOR, VMXOR, VMORN, VMNAND, VMNOR, VMANDN, VMXNOR}) begin
          tr.dest_eew  = eew_e'(EEW1);
          tr.dest_emul = emul_e'(tr.dest_emul * tr.dest_eew / tr.eew);
          tr.src2_eew  = eew_e'(EEW1);
          tr.src2_emul = emul_e'(tr.src2_emul * tr.src2_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if(tr.alu_inst inside {VMADC, VMSBC, VMSEQ, VMSNE, VMSLTU,
                              VMSLT, VMSLEU, VMSLE, VMSGTU, VMSGT}) begin
          tr.dest_eew  = eew_e'(EEW1);
          tr.dest_emul = emul_e'(tr.dest_emul * tr.dest_eew / tr.eew);
        end
        if(tr.alu_inst inside {VMUNARY0} && tr.insn[19:15] inside {vmunary0_e'(VMSBF), vmunary0_e'(VMSOF), vmunary0_e'(VMSIF)}) begin
          tr.dest_eew  = eew_e'(EEW1);
          tr.dest_emul = emul_e'(tr.dest_emul * tr.dest_eew / tr.eew);
          tr.src2_eew  = eew_e'(EEW1);
          tr.src2_emul = emul_e'(tr.src2_emul * tr.src2_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if(tr.alu_inst inside {VMUNARY0} && tr.insn[19:15] inside {vmunary0_e'(VIOTA), vmunary0_e'(VID)}) begin
          tr.src2_eew  = eew_e'(EEW1);
          tr.src2_emul = emul_e'(tr.src2_emul * tr.src2_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if(tr.alu_inst inside {VWXUNARY0} && tr.alu_type == OPMVV && tr.insn[19:15] inside {vwxunary0_e'(VCPOP), vwxunary0_e'(VFIRST)}) begin
          tr.src2_eew  = eew_e'(EEW1);
          tr.src2_emul = emul_e'(tr.src2_emul * tr.src2_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if(tr.alu_inst inside {VWXUNARY0} && tr.alu_type == OPMVV && tr.insn[19:15] inside {vwxunary0_e'(VMV_X_S)} && tr.vm == 1) begin
          tr.src2_eew  = eew_e'(EEW1);
          tr.src2_emul = emul_e'(tr.src2_emul * tr.src2_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if(tr.alu_inst inside {VWXUNARY0} && tr.alu_type == OPMVX && tr.insn[24:20] inside {VMV_S_X} && tr.vm ==1) begin
          tr.dest_eew  = eew_e'(EEW1);
          tr.dest_emul = emul_e'(tr.dest_emul * tr.dest_eew / tr.eew);
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        // Reduction inst
        if (tr.is_reduction_inst) begin
          tr.dest_emul = EMUL1;
          tr.src1_emul = EMUL1;
          if (tr.alu_inst inside {VWREDSUM, VWREDSUMU}) begin
            tr.dest_eew = tr.dest_eew * 2;
            tr.src1_eew = tr.src1_eew * 2;
          end
        end
        if(tr.alu_inst == VSMUL_VMVNRR && tr.alu_type == OPIVI && tr.src1_type == operand_type_e'(UNUSE)) begin
          // vmv<nr>r
          if (tr.insn[19:15] inside {0, 1, 3, 7}) begin
            tr.dest_emul = emul_e'(tr.insn[19:15] + 1);
            tr.src2_emul = emul_e'(tr.insn[19:15] + 1);
            tr.evl       = tr.dest_emul * `VLEN / tr.dest_eew;
          end
        end
        // Permutation Instructions.
        if (tr.alu_inst == VSLIDEUP_RGATHEREI16 && tr.alu_type == OPIVV) begin
          tr.src1_eew  = eew_e'(EEW16);
          tr.src1_emul = emul_e'(tr.src1_emul * tr.src1_eew / tr.eew);
        end
        if (tr.alu_inst == VCOMPRESS && tr.alu_type == OPMVV) begin
          tr.src1_eew  = eew_e'(EEW1);
          tr.src1_emul = EMUL1;
        end
      end  // ALU
    endcase

    // dest is xrf
    if (tr.dest_type == XRF) begin
      tr.dest_eew  = eew_e'(EEW32);
      tr.dest_emul = EMUL1;
    end

    // decode vlmax
    tr.vlmax_max = 8 * `VLEN / 8;
    if(tr.inst_type == ALU_Type && tr.alu_inst inside {VMAND, VMOR, VMXOR, VMORN, VMNAND, VMNOR, VMANDN, VMXNOR}) begin
      tr.vlmax = tr.vlmax_max;
    end
      else if(tr.inst_type == ALU_Type &&tr. alu_inst inside {VMUNARY0} && tr.insn[19:15] inside {VMSBF, VMSOF, VMSIF}) begin
      tr.vlmax = tr.vlmax_max;
    end
      else if(tr.inst_type == ALU_Type &&tr. alu_inst inside {VMSEQ, VMSNE, VMSLTU, VMSLT, VMSLEU, VMSLE, VMSGTU, VMSGT, VMADC, VMSBC}) begin
      if (tr.vlmul[2])  // fraction_lmul
        tr.vlmax = (`VLENB >> tr.vsew);
      else tr.vlmax = ((`VLENB << tr.vlmul) >> tr.vsew);
    end else begin
      if (tr.vlmul[2])  // fraction_lmul
        tr.vlmax = ((`VLENB >> (~tr.vlmul + 3'b1)) >> tr.vsew);
      else tr.vlmax = ((`VLENB << tr.vlmul) >> tr.vsew);
    end

  endfunction : decode_vtype

  // **********************************************************
  // task: asm_string_gen
  // Get decoded instr string
  // **********************************************************
  function void asm_string_gen(coralnpu_rv32v_transaction tr);
    string suff = "";
    string suf0 = "";
    string src0 = "";
    string suf1 = "";
    string src1 = "";
    string suf2 = "";
    string src2 = "";
    string suf3 = "";
    string src3 = "";
    string dest = "";
    string sufd = "";
    string operands = "";
    string comm = "# an example";
    // Inst name
    case (tr.inst_type)
      LD_Type, ST_Type: begin
        asm_inst = tr.lsu_inst.name();
        case (tr.lsu_mop)
          LSU_US: begin
            case (tr.lsu_umop)
              MASK: begin
                asm_inst = tr.lsu_inst.name();
              end
              WHOLE_REG: begin
                if (tr.inst_type == LD_Type) begin
                  asm_inst = $sformatf("vl%0dre%0d", tr.lsu_nf + 1, tr.lsu_eew);
                end
                if (tr.inst_type == ST_Type) begin
                  asm_inst = $sformatf("vs%0dre%0d", tr.lsu_nf + 1, tr.lsu_eew);
                end
              end
              default: begin
                if (tr.lsu_nf == NF1) begin
                  asm_inst = $sformatf("%se%0d", asm_inst, tr.lsu_eew);
                end else begin
                  asm_inst = $sformatf("%s%0de%0d", asm_inst, tr.lsu_nf + 1, tr.lsu_eew);
                end
              end
            endcase
          end
          LSU_CS: begin
            if (tr.lsu_nf == NF1) begin
              asm_inst = $sformatf("%se%0d", asm_inst, tr.lsu_eew);
            end else begin
              asm_inst = $sformatf("%s%0de%0d", asm_inst, tr.lsu_nf + 1, tr.lsu_eew);
            end
          end
          LSU_UI, LSU_OI: begin
            if (tr.lsu_nf == NF1) begin
              asm_inst = $sformatf("%sei%0d", asm_inst, tr.lsu_eew);
            end else begin
              asm_inst = $sformatf("%s%0dei%0d", asm_inst, tr.lsu_nf + 1, tr.lsu_eew);
            end
          end
        endcase
      end
      ALU_Type: begin
        if (tr.alu_inst inside {VWADDU_W, VWADD_W, VWSUBU_W, VWSUB_W}) begin
          asm_inst = tr.alu_inst.name();
          asm_inst = asm_inst.substr(0, asm_inst.len() - 3);
        end else if (tr.alu_inst inside {VXUNARY0}) begin
          if (tr.insn[19:15] inside {VZEXT_VF4, VZEXT_VF2}) asm_inst = "vzext";
          if (tr.insn[19:15] inside {VSEXT_VF4, VSEXT_VF2}) asm_inst = "vsext";
        end else if (tr.alu_inst inside {VMERGE_VMVV}) begin
          if (tr.vm == 1) asm_inst = "vmv.v";
          else asm_inst = "vmerge";
        end else if (tr.alu_inst inside {VWXUNARY0}) begin
          if (tr.src1_type == operand_type_e'(UNUSE)) begin
            if (tr.insn[19:15] == vwxunary0_e'(VMV_X_S)) asm_inst = "vmv.x.s";
            else asm_inst = tr.src1_func_vwxunary0.name();
          end
          if (tr.src2_type == operand_type_e'(UNUSE)) begin
            if (tr.insn[24:20] == VMV_S_X) asm_inst = "vmv.s.x";
            else asm_inst = tr.src2_func_vwxunary0.name();
          end
        end else if (tr.alu_inst inside {VMUNARY0}) begin
          asm_inst = tr.src1_func_vmunary0.name();
        end else if (tr.alu_inst inside {VSMUL_VMVNRR}) begin
          if (tr.alu_type inside {OPIVI}) begin
            if (tr.insn[19:15] inside {0, 1, 3, 7})
              asm_inst = $sformatf("vmv%0dr.v", tr.insn[19:15] + 1);
            else asm_inst = "vmv?r.v";
          end else if (tr.alu_type inside {OPIVV, OPIVX}) begin
            asm_inst = "vsmul";
          end
        end else if (tr.alu_inst inside {VSLIDEUP_RGATHEREI16}) begin
          if (tr.alu_type inside {OPIVX, OPIVI}) asm_inst = "vslideup";
          else if (tr.alu_type inside {OPIVV}) asm_inst = "vrgatheri16";
          else asm_inst = "?";
        end else begin
          asm_inst = tr.alu_inst.name();
        end
      end
      VSET_Type: begin
        if (tr.vset[5] == 0) begin
          asm_inst = "vsetvli";
        end else if (tr.vset[5:4] == 2'b11) begin
          asm_inst = "vsetivli";
        end else if (tr.vset == 6'b100000) begin
          asm_inst = "vsetvl";
        end
      end
    endcase
    asm_inst = asm_inst.toupper();
    `uvm_info("RVVI_MON", $sformatf("asm_inst is %s", asm_inst), UVM_LOW);

    // src1
    case (tr.inst_type)
      LD_Type, ST_Type: begin
        suf1 = "";
        src1 = $sformatf("(x%0d)", tr.rs1_addr);
      end
      ALU_Type: begin
        case (tr.src1_type)
          VRF: begin
            if(tr.inst_type == ALU_Type && tr.alu_inst inside {VMAND, VMOR, VMXOR, VMORN, VMNAND, VMNOR, VMANDN, VMXNOR}) begin
              suf1 = "m";
              src1 = $sformatf("v%0d", tr.vs1_addr);
            end else begin
              suf1 = "v";
              src1 = $sformatf("v%0d", tr.vs1_addr);
            end
          end
          XRF: begin
            suf1 = "x";
            src1 = $sformatf("x%0d", tr.rs1_addr);
          end
          IMM: begin
            suf1 = "i";
            src1 = $sformatf("%0d", $signed(tr.imm));
          end
          UIMM: begin
            suf1 = "i";
            src1 = $sformatf("%0d", $unsigned(tr.uimm));
          end
          UNUSE: begin
            if(tr.inst_type == ALU_Type && tr.alu_inst == VXUNARY0 && tr.insn[19:15] inside{VSEXT_VF4, VZEXT_VF4}) begin
              suf1 = "f4";
              src1 = "";
            end else if(tr.inst_type == ALU_Type && tr.alu_inst == VXUNARY0 && tr.insn[19:15] inside{VSEXT_VF2, VZEXT_VF2}) begin
              suf1 = "f2";
              src1 = "";
            end
          end
          default: begin
            suf1 = "?";
            src1 = "?";
          end
        endcase
      end
    endcase

    // src2
    case (tr.inst_type)
      LD_Type, ST_Type: begin
        case (tr.src2_type)
          VRF: begin
            suf2 = "";
            src2 = $sformatf("v%0d", tr.vs2_addr);
          end
          XRF: begin
            suf2 = "";
            src2 = $sformatf("x%0d", tr.rs2_addr);
          end
          UNUSE: begin
            suf2 = "";
            src2 = "";
          end
          default: begin
            suf2 = "?";
            src2 = "?";
          end
        endcase
      end
      ALU_Type: begin
        case (tr.src2_type)
          VRF: begin
            if(tr.inst_type == ALU_Type && tr.alu_inst inside {VWADDU_W, VWADD_W, VWSUBU_W, VWSUB_W}) begin
              suf2 = "w";
              src2 = $sformatf("v%0d", tr.vs2_addr);
            end else if(tr.inst_type == ALU_Type && tr.alu_inst inside {VMAND, VMOR, VMXOR, VMORN, VMNAND, VMNOR, VMANDN, VMXNOR,VMUNARY0}) begin
              suf2 = "m";
              src2 = $sformatf("v%0d", tr.vs2_addr);
            end else if (tr.inst_type == ALU_Type && tr.alu_inst inside {VWXUNARY0}) begin
              if(tr.src1_type == operand_type_e'(UNUSE) && tr.insn[19:15] inside {vwxunary0_e'(VCPOP), vwxunary0_e'(VFIRST)}) begin
                suf2 = "m";
                src2 = $sformatf("v%0d", tr.vs2_addr);
              end else if(tr.src1_type == operand_type_e'(UNUSE) && tr.insn[19:15] inside {vwxunary0_e'(VMV_X_S)}) begin // vmv.x.s
                suf2 = "s";
                src2 = $sformatf("v%0d", tr.insn[24:20]);
              end else begin
                suf2 = "v";
                src2 = $sformatf("v%0d", tr.vs2_addr);
              end
            end else begin
              suf2 = "v";
              src2 = $sformatf("v%0d", tr.vs2_addr);
            end
          end
          XRF: begin
            suf2 = "x";
            src2 = $sformatf("x%0d", tr.rs2_addr);
          end
          IMM: begin
            suf2 = "i";
            src2 = $sformatf("%0d", $signed(tr.rs2_addr));
          end
          UNUSE: begin
            suf2 = "";
            src2 = "";
          end
          default: begin
            suf2 = "?";
            src2 = "?";
          end
        endcase
      end
    endcase

    // vm
    if (tr.vm == 0) begin
      if (tr.inst_type == ALU_Type && tr.use_vm_to_cal == 1) begin
        suf0 = "m";
        src0 = "v0";
      end else begin
        suf0 = "";
        src0 = "v0.t";
      end
    end else begin
      suf0 = "";
      src0 = "";
    end

    // dest/src3
    case (tr.inst_type)
      LD_Type: begin
        sufd = "v";
        case (tr.dest_type)
          VRF: dest = $sformatf("v%0d", tr.vd_addr);
          XRF: dest = $sformatf("x%0d", tr.rd_addr);
          default: dest = "?";
        endcase
        suf3 = "";
        src3 = "";
      end
      ST_Type: begin
        sufd = "";
        dest = "";
        suf3 = "v";
        case (tr.src3_type)
          VRF: src3 = $sformatf("v%0d", tr.vd_addr);
          XRF: src3 = $sformatf("x%0d", tr.vd_addr);
          default: src3 = "?";
        endcase
      end
      ALU_Type: begin
        sufd = "";
        case (tr.dest_type)
          VRF: dest = $sformatf("v%0d", tr.vd_addr);
          XRF: dest = $sformatf("x%0d", tr.rd_addr);
          default: dest = "?";
        endcase
        suf3 = "";
        src3 = "";
      end
    endcase

    suff = $sformatf("%s%s%s%s%s", sufd, suf3, suf2, suf1, suf0);

    // Comments
    comm = $sformatf("# vlmul=%0s, vsew=%0s, vl=%0d", tr.vlmul.name(), tr.vsew.name(), tr.avl);
    if (tr.inst_type inside {LD_Type, ST_Type} && tr.src1_type == XRF)
      comm = $sformatf("%s, base=0x%8x", comm, tr.rs1_val);
    if (tr.inst_type inside {LD_Type, ST_Type} && tr.lsu_mop == LSU_CS && tr.src2_type == XRF)
      comm = $sformatf("%s, const_stride=%0d", comm, $signed(tr.rs2_val));

    // asm string
    if (tr.inst_type == ST_Type) dest = src3;
    if (tr.vm)
      if (tr.src1_type == operand_type_e'(UNUSE))
        tr.asm_string = $sformatf("%s.%s %s, %s %s", asm_inst, suff, dest, src2, comm);
      else tr.asm_string = $sformatf("%s.%s %s, %s, %s %s", asm_inst, suff, dest, src2, src1, comm);
    else if (tr.src1_type == operand_type_e'(UNUSE))
      tr.asm_string = $sformatf("%s.%s %s, %s, %s %s", asm_inst, suff, dest, src2, src0, comm);
    else
      tr.asm_string = $sformatf(
          "%s.%s %s, %s, %s, %s %s", asm_inst, suff, dest, src2, src1, src0, comm
      );
  endfunction : asm_string_gen

  // **********************************************************
  // task: get_insn_name
  // Get instruction name & ISA group
  // args0:  instruction bytes from rvvi interafce
  // args1:  output,instruction name
  // args2:  output,ISA group
  // **********************************************************

  virtual function void get_insn_name(bit [(`ILEN-1):0] insn, ref insn_name_enum insn_name,
                                      ref isa_enum isa_name);
    bit [2:0] funct3;
    bit [6:0] funct7;
    bit [6:0] opcode;
    bit [11:0] imm;
    fmt_typ_enum fmt_typ;

    fmt_typ = get_fmt_typ(insn);
    opcode  = insn[6:0];

    case (fmt_typ)
      R_Type: begin
        funct3 = insn[14:12];
        funct7 = insn[31:25];
        if (funct3 == 3'h0 && funct7 == 7'h00) begin
          insn_name = ADD;
          isa_name  = RV32I;
        end else if (funct3 == 3'h0 && funct7 == 7'h20) begin
          insn_name = SUB;
          isa_name  = RV32I;
        end else if (funct3 == 3'h4 && funct7 == 7'h00) begin
          insn_name = XOR;
          isa_name  = RV32I;
        end else if (funct3 == 3'h6 && funct7 == 7'h00) begin
          insn_name = OR;
          isa_name  = RV32I;
        end else if (funct3 == 3'h7 && funct7 == 7'h00) begin
          insn_name = AND;
          isa_name  = RV32I;
        end else if (funct3 == 3'h1 && funct7 == 7'h00) begin
          insn_name = SLL;
          isa_name  = RV32I;
        end else if (funct3 == 3'h5 && funct7 == 7'h00) begin
          insn_name = SRL;
          isa_name  = RV32I;
        end else if (funct3 == 3'h5 && funct7 == 7'h20) begin
          insn_name = SRA;
          isa_name  = RV32I;
        end else if (funct3 == 3'h2 && funct7 == 7'h00) begin
          insn_name = SLT;
          isa_name  = RV32I;
        end else if (funct3 == 3'h3 && funct7 == 7'h00) begin
          insn_name = SLTU;
          isa_name  = RV32I;
        end
      end
      I_Type: begin
        funct3 = insn[14:12];
        imm    = insn[31:20];
        funct7 = imm[11:5];//imm[11:5]
        if (opcode == 7'b0010011) begin
          case (funct3)
            3'h0: begin
              insn_name = ADDI;
              isa_name  = RV32I;
            end
            3'h4: begin
              insn_name = XORI;
              isa_name  = RV32I;
            end
            3'h6: begin
              insn_name = ORI;
              isa_name  = RV32I;
            end
            3'h7: begin
              insn_name = ANDI;
              isa_name  = RV32I;
            end
            3'h1: begin
              insn_name = SLLI;
              isa_name  = RV32I;
            end
            3'h5:
            if (funct7 == 7'h00) begin
              insn_name = SRLI;
              isa_name  = RV32I;
            end else if (funct7 == 7'h20) begin
              insn_name = SRAI;
              isa_name  = RV32I;
            end
            3'h2: begin
              insn_name = SLTI;
              isa_name  = RV32I;
            end
            3'h3: begin
              insn_name = SLTIU;
              isa_name  = RV32I;
            end
          endcase
        end else if (opcode == 7'b0000011) begin
          case (funct3)
            3'h0: begin
              insn_name = LB;
              isa_name  = RV32I;
            end
            3'h1: begin
              insn_name = LH;
              isa_name  = RV32I;
            end
            3'h2: begin
              insn_name = LW;
              isa_name  = RV32I;
            end
            3'h4: begin
              insn_name = LBU;
              isa_name  = RV32I;
            end
            3'h5: begin
              insn_name = LHU;
              isa_name  = RV32I;
            end
          endcase
        end else if (opcode == 7'b1100111 && funct3 == 3'h0) begin
          insn_name = JALR;
          isa_name  = RV32I;
        end else if (opcode == 7'b1110011 && funct3 == 3'h0) begin
          case (imm)
            12'h0: begin
              insn_name = ECALL;
              isa_name  = RV32I;
            end
            12'h1: begin
              insn_name = EBREAK;
              isa_name  = RV32I;
            end
          endcase
        end
      end
      S_Type: begin
        funct3 = insn[14:12];
        case (funct3)
          3'h0: begin
            insn_name = SB;
            isa_name  = RV32I;
          end
          3'h1: begin
            insn_name = SH;
            isa_name  = RV32I;
          end
          3'h2: begin
            insn_name = SW;
            isa_name  = RV32I;
          end
        endcase
      end
      B_Type: begin
        funct3 = insn[14:12];
        case (funct3)
          3'h0: begin
            insn_name = BEQ;
            isa_name  = RV32I;
          end
          3'h1: begin
            insn_name = BNE;
            isa_name  = RV32I;
          end
          3'h4: begin
            insn_name = BLT;
            isa_name  = RV32I;
          end
          3'h5: begin
            insn_name = BGE;
            isa_name  = RV32I;
          end
          3'h6: begin
            insn_name = BLTU;
            isa_name  = RV32I;
          end
          3'h7: begin
            insn_name = BGEU;
            isa_name  = RV32I;
          end
        endcase
      end
      U_Type: begin
        if (opcode == 7'b0110111) begin
          insn_name = LUI;
          isa_name  = RV32I;
        end else if (opcode == 7'b0010111) begin
          insn_name = AUIPC;
          isa_name  = RV32I;
        end
      end
      J_Type: begin
        if (opcode == 7'b1101111) begin
          insn_name = JAL;
          isa_name  = RV32I;
        end
      end
      Fence_Type: begin
        funct3 = insn[14:12];
        if (funct3 == 3'h0) begin
          insn_name = FENCE;
          isa_name  = RV32I;
        end else if (funct3 == 3'h1) isa_name = RV32Zifencei;
      end
      VSET_Type, ALU_Type, LD_Type, ST_Type: begin
        isa_name = RV32V;
      end
      FP_Type: begin
        isa_name = RV32F;
      end
      M_Type: begin
        isa_name = RV32M;
      end
      CSR_Type: begin
        isa_name = RV32Zicsr;
      end
      Zbb_Type: begin
        isa_name = RV32Zbb;
      end
      USER_DEFINED: begin
        if (insn == 32'h8000073) begin
          insn_name = MPAUSE;
          isa_name  = Custom;
        end
      end
    endcase
  endfunction

  //get format type,eg:R-Type,I-Type...
  virtual function fmt_typ_enum get_fmt_typ(bit [(`ILEN-1):0] insn);
    fmt_typ_enum       fmt_typ;
    bit          [6:0] opcode;
    bit          [2:0] inst_14_12;  //for vset
    bit          [6:0] funct7;  //inst[31:25]
    bit          [2:0] funct3;

    opcode = insn[6:0];
    inst_14_12 = insn[14:12];
    funct7 = insn[31:25];
    funct3 = insn[14:12];
    case (opcode)
      7'b0110011: begin
        case (funct7)
          7'b0000001: fmt_typ = M_Type;
          7'b0100000: begin
              case(funct3)
                3'b0, 3'b101:    fmt_typ = R_Type;
                3'b111, 3'b110, 3'b100: fmt_typ = Zbb_Type;
              endcase
            end
          7'b0: fmt_typ = R_Type;
          7'b0000101, 7'b0000100, 7'b0110000: fmt_typ = Zbb_Type;
        endcase
      end
      7'b0000011, 7'b1100111: fmt_typ = I_Type;
      7'b0010011:
      if ((funct3 == 3'h1 && funct7 != 0) || (funct3 == 3'h5 && !(funct7 inside {7'h00, 7'h20})))
        fmt_typ = Zbb_Type;
      else fmt_typ = I_Type;
      7'b1110011:
      if (funct3 == 3'b000) fmt_typ = I_Type;
      else fmt_typ = CSR_Type;
      7'b0100011: fmt_typ = S_Type;
      7'b1100011: fmt_typ = B_Type;
      7'b1101111: fmt_typ = J_Type;
      7'b0110111, 7'b0010111: fmt_typ = U_Type;
      7'b0001111: fmt_typ = Fence_Type;

      //v-isa
      7'b1010111:
      if (inst_14_12 == 3'b111) fmt_typ = VSET_Type;
      else fmt_typ = ALU_Type;
      7'b0000111:
      if (inst_14_12 == 3'b010)
        fmt_typ = FP_Type;  //f load/store have same opcode with v load/store
      else fmt_typ = LD_Type;
      7'b0100111:
      if (inst_14_12 == 3'b010) fmt_typ = FP_Type;
      else fmt_typ = ST_Type;

      //floating-point
      7'b1010011, 7'b1000011, 7'b1000111, 7'b1001011, 7'b1001111: fmt_typ = FP_Type;
      default: fmt_typ = FORMAT_UNSET;
    endcase
    //user defined
    if (insn == 32'h8000073) fmt_typ = USER_DEFINED;
    return fmt_typ;

  endfunction

endclass : coralnpu_rvvi_monitor
