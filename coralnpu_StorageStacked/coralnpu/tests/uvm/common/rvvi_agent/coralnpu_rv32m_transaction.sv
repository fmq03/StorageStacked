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
// Class: coralnpu_rv32m_transaction
// Description:  Defines a transaction item for Integer Multiplication and Division
// instructions.
//--------------------------------------------------------------------------------
class coralnpu_rv32m_transaction extends coralnpu_rvvi_decode_transaction;

  mult_div_e inst_name;

  `uvm_object_utils_begin(coralnpu_rv32m_transaction)
    `uvm_field_enum(mult_div_e, inst_name, UVM_DEFAULT)
  `uvm_object_utils_end

  function new(string name = "coralnpu_rv32m_transaction");
    super.new(name);
  endfunction : new

endclass : coralnpu_rv32m_transaction
