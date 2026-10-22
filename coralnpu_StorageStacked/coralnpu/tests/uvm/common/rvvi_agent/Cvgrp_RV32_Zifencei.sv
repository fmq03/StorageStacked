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
// Description:covergroup for RV32 Zifencei instruction
//----------------------------------------------------------------------------
covergroup cvgrp_RV32_Zifencei;

  option.per_instance = 1;

  //base cover
  fence_i: coverpoint rv32zifencei_trans.inst_name iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {FENCE_I}; option.weight = 1;
  }

  //funct12
  funct12: coverpoint rv32zifencei_trans.funct12 iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {0}; option.weight = 1;
  }

  //RD (GPR) register assignment
  rd_addr: coverpoint rv32zifencei_trans.rd_addr iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {0}; option.weight = 1;
  }

  //RS1 (GPR) register assignment
  rs1_addr: coverpoint rv32zifencei_trans.rs1_addr iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {0}; option.weight = 1;
  }

  //RD value
  rd_val: coverpoint unsigned'(rv32zifencei_trans.rd_val) iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {32'b0}; option.weight = 1;
  }

  //RS1 value
  rs1_val: coverpoint unsigned'(rv32zifencei_trans.rs1_val) iff (rv32zifencei_trans.trap == 0) {
    bins b0 = {32'b0}; option.weight = 1;
  }

  // base cross
  //FENCE_I instruction crosspoints
  //Cross FENCE_I instruction and register assignment
  cr_fence_i_rs1_funct12_rd: cross fence_i, rs1_addr, funct12, rd_addr{
    option.weight = 1;
  }

  //Cross FENCE_I instruction and RS1 bits
  cr_fence_i_rs1_val: cross fence_i, rs1_val{
    option.weight = 1;
  }

  //Cross FENCE_I instruction and RD bits
  cr_fence_i_rd_val: cross fence_i, rd_val{
    option.weight = 1;
  }

endgroup
