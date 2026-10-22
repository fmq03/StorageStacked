// Copyright 2026 Google LLC
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

package i2c_master_pkg;

  // Register Offsets
  localparam logic [11:0] INTR_STATE = 12'h000;
  localparam logic [11:0] INTR_ENABLE = 12'h004;
  localparam logic [11:0] CTRL = 12'h008;
  localparam logic [11:0] STATUS = 12'h00C;
  localparam logic [11:0] FDATA = 12'h010;
  localparam logic [11:0] FIFO_CTRL = 12'h014;
  localparam logic [11:0] CLK_DIV = 12'h018;

  // I2C Command Bits for FDATA Write
  localparam int FDATA_START = 8;
  localparam int FDATA_STOP = 9;
  localparam int FDATA_READ = 10;

  typedef enum logic [2:0] {
    Idle,
    Start,
    Addr,
    AckAddr,
    Write,
    Read,
    AckData,
    Stop
  } i2c_state_e;

endpackage
