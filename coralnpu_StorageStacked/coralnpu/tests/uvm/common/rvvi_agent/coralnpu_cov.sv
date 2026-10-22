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
// Description: Sample Coverage
//----------------------------------------------------------------------------
`ifndef CORALNPU_COV_SV
`define CORALNPU_COV_SV

`uvm_analysis_imp_decl(_rv32i)
`uvm_analysis_imp_decl(_rv32v)
`uvm_analysis_imp_decl(_rv32f)
`uvm_analysis_imp_decl(_rv32m)
`uvm_analysis_imp_decl(_rv32zicsr)
`uvm_analysis_imp_decl(_rv32zbb)
`uvm_analysis_imp_decl(_rv32zifencei)
`uvm_analysis_imp_decl(_custom)

//----------------------------------------------------------------------------
// Class: coralnpu_cov
//----------------------------------------------------------------------------
class coralnpu_cov extends uvm_component;
  `uvm_component_utils(coralnpu_cov)

  coralnpu_rv32i_transaction rv32i_trans;
  uvm_analysis_imp_rv32i #(coralnpu_rv32i_transaction, coralnpu_cov) imp_rv32i;

  coralnpu_rv32v_transaction rv32v_trans;
  uvm_analysis_imp_rv32v #(coralnpu_rv32v_transaction, coralnpu_cov) imp_rv32v;

  coralnpu_rv32f_transaction rv32f_trans;
  uvm_analysis_imp_rv32f #(coralnpu_rv32f_transaction, coralnpu_cov) imp_rv32f;

  coralnpu_rv32m_transaction rv32m_trans;
  uvm_analysis_imp_rv32m #(coralnpu_rv32m_transaction, coralnpu_cov) imp_rv32m;

  coralnpu_rv32zicsr_transaction rv32zicsr_trans;
  uvm_analysis_imp_rv32zicsr #(coralnpu_rv32zicsr_transaction, coralnpu_cov) imp_rv32zicsr;

  coralnpu_rv32zbb_transaction rv32zbb_trans;
  uvm_analysis_imp_rv32zbb #(coralnpu_rv32zbb_transaction, coralnpu_cov) imp_rv32zbb;

  coralnpu_rv32zifencei_transaction rv32zifencei_trans;
  uvm_analysis_imp_rv32zifencei #(coralnpu_rv32zifencei_transaction, coralnpu_cov) imp_rv32zifencei;

  coralnpu_rvvi_decode_transaction custom_trans;
  uvm_analysis_imp_custom #(coralnpu_rvvi_decode_transaction, coralnpu_cov) imp_custom;

  `include "Cvgrp_RV32_I.sv"
  `include "Cvgrp_RV32_M.sv"
  `include "Cvgrp_RV32_F.sv"
  `include "Cvgrp_RV32_V.sv"
  `include "Cvgrp_RV32_Zicsr.sv"
  `include "Cvgrp_RV32_Zbb.sv"
  `include "Cvgrp_RV32_Zifencei.sv"
  `include "Cvgrp_Custom.sv"
  function new(string name = "coralnpu_cov", uvm_component parent = null);
    super.new(name, parent);
    cvgrp_RV32_I        = new();
    rv32i_trans         = new;

    cvgrp_RV32_V        = new();
    rv32v_trans         = new;

    cvgrp_RV32_F        = new();
    rv32f_trans         = new;

    cvgrp_RV32_M        = new();
    rv32m_trans         = new;

    cvgrp_RV32_Zicsr    = new();
    rv32zicsr_trans     = new;

    cvgrp_RV32_Zbb      = new();
    rv32zbb_trans       = new;

    cvgrp_RV32_Zifencei = new();
    rv32zifencei_trans  = new;

    cvgrp_Custom        = new();
    custom_trans        = new;
  endfunction

  virtual function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    imp_rv32i = new("imp_rv32i", this);
    imp_rv32v = new("imp_rv32v", this);
    imp_rv32f = new("imp_rv32f", this);
    imp_rv32m = new("imp_rv32m", this);
    imp_rv32zicsr = new("imp_rv32zicsr", this);
    imp_rv32zbb = new("imp_rv32zbb", this);
    imp_rv32zifencei = new("imp_rv32zifencei", this);
    imp_custom = new("imp_custom", this);
  endfunction

  virtual function void end_of_elaboration_phase(uvm_phase phase);
    super.end_of_elaboration_phase(phase);
  endfunction

  //sample rv32i_transaction from coralnpu monitor
  virtual function void write_rv32i(coralnpu_rv32i_transaction t);
    rv32i_trans.copy(t);
    cvgrp_RV32_I.sample();
  endfunction

  //sample rv32v_transaction from coralnpu monitor
  virtual function void write_rv32v(coralnpu_rv32v_transaction t);
    rv32v_trans.copy(t);
    cvgrp_RV32_V.sample();
  endfunction

  //sample rv32f_transaction from coralnpu monitor
  virtual function void write_rv32f(coralnpu_rv32f_transaction t);
    rv32f_trans.copy(t);
    cvgrp_RV32_F.sample();
  endfunction

  //sample rv32m_transaction from coralnpu monitor
  virtual function void write_rv32m(coralnpu_rv32m_transaction t);
    rv32m_trans.copy(t);
    cvgrp_RV32_M.sample();
  endfunction

  //sample rv32zicsr_transaction from coralnpu monitor
  virtual function void write_rv32zicsr(coralnpu_rv32zicsr_transaction t);
    rv32zicsr_trans.copy(t);
    cvgrp_RV32_Zicsr.sample();
  endfunction

  //sample rv32zbb_transaction from coralnpu monitor
  virtual function void write_rv32zbb(coralnpu_rv32zbb_transaction t);
    rv32zbb_trans.copy(t);
    cvgrp_RV32_Zbb.sample();
  endfunction

  //sample rv32zifencei_transaction from coralnpu monitor
  virtual function void write_rv32zifencei(coralnpu_rv32zifencei_transaction t);
    rv32zifencei_trans.copy(t);
    cvgrp_RV32_Zifencei.sample();
  endfunction

  //sample custom transaction from coralnpu monitor
  virtual function void write_custom(coralnpu_rvvi_decode_transaction t);
    custom_trans.copy(t);
    cvgrp_Custom.sample();
  endfunction
endclass

`endif
