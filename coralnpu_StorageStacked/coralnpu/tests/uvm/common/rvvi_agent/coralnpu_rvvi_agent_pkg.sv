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

`ifndef CORALNPU_RVVI_AGENT_PKG_SV
`define CORALNPU_RVVI_AGENT_PKG_SV

`include "uvm_pkg.sv"
`include "coralnpu_macros.sv"
`include "coralnpu_rvvi_monitor_if.sv"

//----------------------------------------------------------------------------
// Package: coralnpu_rvvi_agent_pkg
// Description: Package holding rvvi agent component.
//----------------------------------------------------------------------------
package coralnpu_rvvi_agent_pkg;

  import uvm_pkg::*;

  //macros & types
  `include "coralnpu_rvvi_types.sv"

  //transaction
  `include "coralnpu_rvvi_transaction.sv"
  `include "coralnpu_rvvi_decode_transaction.sv"
  `include "coralnpu_rv32i_transaction.sv"  //Integer Instruction Set
  `include "coralnpu_rv32v_transaction.sv"  //Vector Operations
  `include "coralnpu_rv32f_transaction.sv"  //Single-Precision Floating-Point
  `include "coralnpu_rv32m_transaction.sv"  //Integer Multiplication and Division
  `include "coralnpu_rv32zicsr_transaction.sv"  //Control and Status Register(CSR) Instructions
  `include "coralnpu_rv32zbb_transaction.sv"  // bit_manipulation Instructions
  `include "coralnpu_rv32zifencei_transaction.sv"  // Instruction-Fetch Fence
  //monitor
  `include "coralnpu_rvvi_monitor.sv"

  //coverage component
  `include "coralnpu_cov.sv"

  // component part
  `include "coralnpu_rvvi_agent.sv"

endpackage : coralnpu_rvvi_agent_pkg

`endif  // coralnpu_RVVI_AGENT_PKG_SV
