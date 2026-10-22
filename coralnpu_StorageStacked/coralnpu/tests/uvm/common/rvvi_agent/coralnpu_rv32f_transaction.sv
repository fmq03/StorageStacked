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

//--------------------------------------------------------------------------------
// Class: coralnpu_rv32f_transaction
// Description:  Defines a transaction item for RV32F floating-point instructions.
//--------------------------------------------------------------------------------
class coralnpu_rv32f_transaction extends coralnpu_rvvi_decode_transaction;

  logic [4:0] fd_addr;
  bit signed [(`FLEN-1):0] fd_val;
  logic [4:0] fs1_addr;
  bit signed [(`FLEN-1):0] fs1_val;
  logic [4:0] fs2_addr;
  bit signed [(`FLEN-1):0] fs2_val;
  logic [4:0] fs3_addr;
  bit signed [(`FLEN-1):0] fs3_val;
  logic [4:0] rs3_addr;
  bit signed [(`XLEN-1):0] rs3_val;
  logic [2:0] rm;  //insn[14:12]
  floating_e inst_name;
  //floating_point csr
  logic [(`FLEN-1):0] fflags;
  logic [(`FLEN-1):0] frm;
  logic [(`FLEN-1):0] fcsr;

  `uvm_object_utils_begin(coralnpu_rv32f_transaction)
    `uvm_field_int(fd_addr, UVM_DEFAULT)
    `uvm_field_int(fd_val, UVM_DEFAULT)
    `uvm_field_int(fs1_addr, UVM_DEFAULT)
    `uvm_field_int(fs1_val, UVM_DEFAULT)
    `uvm_field_int(fs2_addr, UVM_DEFAULT)
    `uvm_field_int(fs2_val, UVM_DEFAULT)
    `uvm_field_int(fs3_addr, UVM_DEFAULT)
    `uvm_field_int(fs3_val, UVM_DEFAULT)
    `uvm_field_int(rs3_addr, UVM_DEFAULT)
    `uvm_field_int(rs3_val, UVM_DEFAULT)
    `uvm_field_int(rm, UVM_DEFAULT)
    `uvm_field_int(fflags, UVM_DEFAULT)
    `uvm_field_int(frm, UVM_DEFAULT)
    `uvm_field_int(fcsr, UVM_DEFAULT)
    `uvm_field_enum(floating_e, inst_name, UVM_DEFAULT)
  `uvm_object_utils_end

  function new(string name = "coralnpu_rv32f_transaction");
    super.new(name);
  endfunction : new

endclass : coralnpu_rv32f_transaction
