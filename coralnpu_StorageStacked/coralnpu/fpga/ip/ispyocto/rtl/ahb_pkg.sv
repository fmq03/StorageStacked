// Copyright 2026 Google LLC
// Copyright lowRISC contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package ahb_pkg;

  typedef struct packed {
    logic [top_pkg::TL_AW-1:0] haddr;
    logic [2:0]                hsize;
    logic [2:0]                hburst;
    logic [top_pkg::TL_DW-1:0] hwdata;
    logic [1:0]                htrans;
    logic                      hwrite;
    logic                      hsel;
    logic                      hready;  // as hready_in for ahb device
  } ahb_h2d_t;

  typedef struct packed {
    logic [top_pkg::TL_DW-1:0] hrdata;
    logic                      hready;
    logic                      hresp;
  } ahb_d2h_t;

endpackage
