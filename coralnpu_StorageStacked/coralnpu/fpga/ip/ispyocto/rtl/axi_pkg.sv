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

package axi_pkg;

  localparam int AXI_AW = 32;
  localparam int AXI_DW = 64;
  localparam int AXI_IW = 4;
  localparam int AXI_DBW = (AXI_DW >> 3);

  typedef struct packed {
    logic               awvalid;
    logic [AXI_IW-1:0]  awid;
    logic [AXI_AW-1:0]  awaddr;
    logic [7:0]         awlen;
    logic [2:0]         awsize;
    logic [1:0]         awburst;
    logic [1:0]         awlock;
    logic [3:0]         awcache;
    logic [2:0]         awprot;
    logic [AXI_IW-1:0]  wid;
    logic               wvalid;
    logic [AXI_DW-1:0]  wdata;
    logic [AXI_DBW-1:0] wstrb;
    logic               wlast;
    logic               bready;
    //
    logic               arvalid;
    logic [AXI_IW-1:0]  arid;
    logic [AXI_AW-1:0]  araddr;
    logic [7:0]         arlen;
    logic [2:0]         arsize;
    logic [1:0]         arburst;
    logic [1:0]         arlock;
    logic [3:0]         arcache;
    logic [2:0]         arprot;
    logic               rready;
  } axi_req_t;

  // default value of axi_req_t
  parameter axi_req_t AXI_REQ_DEFAULT = '{
      awvalid: 1'b0,
      awid: {AXI_IW{1'b0}},
      awaddr: {AXI_AW{1'b0}},
      awlen: 8'b0,
      awsize: 3'b0,
      awburst: 2'b0,
      awlock: 2'b0,
      awcache: 4'b0,
      awprot: 3'b0,
      wid: {AXI_IW{1'b0}},
      wvalid: 1'b0,
      wdata: {AXI_DW{1'b0}},
      wstrb: {AXI_DBW{1'b0}},
      wlast: 1'b0,
      bready: 1'b0,
      //
      arvalid:
      1'b0,
      arid: {AXI_IW{1'b0}},
      araddr: {AXI_AW{1'b0}},
      arlen: 8'b0,
      arsize: 3'b0,
      arburst: 2'b0,
      arlock: 2'b0,
      arcache: 4'b0,
      arprot: 3'b0,
      rready: 1'b0
  };

  typedef struct packed {
    logic              awready;
    logic              wready;
    logic              arready;
    logic [AXI_IW-1:0] rid;
    logic [AXI_DW-1:0] rdata;
    logic [1:0]        rresp;
    logic              rlast;
    logic              rvalid;
    logic              bvalid;
    logic [1:0]        bresp;
    logic [AXI_IW-1:0] bid;
  } axi_rsp_t;

  // default value of axi_rsp_t
  parameter axi_rsp_t AXI_RSP_DEFAULT = '{
      awready: 1'b0,
      wready: 1'b0,
      arready: 1'b0,
      rid: {AXI_IW{1'b0}},
      rdata: {AXI_DW{1'b0}},
      rresp: 2'b0,
      rlast: 1'b0,
      rvalid: 1'b0,
      bvalid: 1'b0,
      bresp: 2'b0,
      bid: {AXI_IW{1'b0}}
  };
endpackage
