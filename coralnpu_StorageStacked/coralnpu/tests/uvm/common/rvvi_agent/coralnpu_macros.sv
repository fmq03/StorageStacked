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
// Description: Defines
//----------------------------------------------------------------------------
`ifndef CORALNPU_MACROS_SV
`define CORALNPU_MACROS_SV

// for base vseq pre_body phase object get, this define only exist when
// Synopsys vip used,if env not use vip, this define must be add by user
`ifndef SVT_UVM_12_OR_HIGHER
`define SVT_UVM_12_OR_HIGHER
`endif

// The maximum permissible instruction length in bits
`ifndef ILEN
`define ILEN 32
`endif

//The maximum permissible General purpose register size in bits
`ifndef XLEN
`define XLEN 32
`endif

//The maximum permissible Floating point register size in bits
`ifndef FLEN
`define FLEN 32
`endif

//The maximum permissible Vector register size in bits
`ifndef VLEN
`define VLEN 128
`endif

//The number of harts that will be reported on RVVI interface
`ifndef NHART
`define NHART 1
`endif

//The maximum number of instructions that can be retired during a valid event on RVVI interface
`ifndef RETIRE
`define RETIRE 8
`endif

`endif
