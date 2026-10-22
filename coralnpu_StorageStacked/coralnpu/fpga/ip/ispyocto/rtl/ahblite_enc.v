// Copyright 2026 Google LLC
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

// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

module ahblite_enc #(
    parameter AhbLiteDataWidth = 32,
    parameter TlulDataWidth    = 32,
    parameter IdWidth          = 8
) (
    clk,
    rstn,
    // AHB Lite signals
    haddr,
    hburst,
    hsize,
    htrans,
    hwdata,
    hwrite,
    hrdata,
    hreadyout,
    hresp,
    hsel,
    hready,
    // internal signals
    size,
    req_id,
    strb_internal,
    rvalid_internal,
    wvalid_internal,
    ren,
    wen,
    resp_ready,
    addr,
    wdata_internal,
    rdata_internal,
    busy,
    resp_valid,
    resp_id,
    resp
);

  localparam TlulStrbWidth = AhbLiteDataWidth >> 3;
  localparam TlulDBWidth = TlulDataWidth >> 3;
  localparam TlulSizeWidth = $clog2($clog2(TlulDBWidth)) + 1;


  input clk;
  input rstn;

  // AHB Lite signals
  output [31:0] haddr;
  output [2:0] hburst;
  output [2:0] hsize;
  output [1:0] htrans;
  output [AhbLiteDataWidth-1:0] hwdata;
  output hwrite;
  input [AhbLiteDataWidth-1:0] hrdata;
  input hreadyout;
  input hresp;
  output hsel;
  output hready;

  // internal signals
  input [TlulSizeWidth-1:0] size;
  input [IdWidth-1:0] req_id;
  input [TlulStrbWidth-1:0] strb_internal;
  input rvalid_internal;
  input wvalid_internal;
  input ren;
  input wen;
  input resp_ready;
  input [31:0] addr;
  input [TlulDataWidth-1:0] wdata_internal;
  output [TlulDataWidth-1:0] rdata_internal;
  output busy;
  output resp_valid;
  output [IdWidth-1:0] resp_id;
  output [1:0] resp;

  localparam NONSEQ = 2'b10;
  localparam IDLE = 2'b00;
  localparam SINGLE = 3'b000;



  // directly
  assign hwrite = wvalid_internal;
  assign htrans = (wvalid_internal | rvalid_internal) ? NONSEQ : IDLE;
  assign haddr = (wvalid_internal | rvalid_internal) ? addr : 32'b0;
  assign hsize = (wvalid_internal | rvalid_internal) ? {1'b0, size} : 3'b0;
  assign hburst = SINGLE;
  assign busy = ~hreadyout;
  assign resp_id = req_id;
  assign hready = hreadyout;
  assign hsel = 1'b1;


  // Req Handler
  assign hwdata = (wen & (~wvalid_internal)) ? wdata_internal : 32'b0;
  assign resp_valid = hreadyout & ((wen & (~wvalid_internal)) | (ren & (~rvalid_internal)));
  assign rdata_internal = (ren & resp_valid) ? hrdata : 32'b0;



  // resp error check
  reg  hresp_reg;
  reg  hreadyout_reg;
  reg  hresp_error;
  wire hresp_flag;
  wire hreadyout_flag;

  always @(posedge clk or negedge rstn) begin
    if (!rstn) hresp_reg <= 1'b0;
    else hresp_reg <= hresp;
  end

  always @(posedge clk or negedge rstn) begin
    if (!rstn) hreadyout_reg <= 1'b0;
    else hreadyout_reg <= hreadyout;
  end

  assign hresp_flag = hresp & hresp_reg;
  assign hreadyout_flag = hreadyout & ~hreadyout_reg;


  always @(posedge clk or negedge rstn) begin
    if (!rstn) hresp_error <= 1'b0;
    else if (~(wen | ren)) hresp_error <= 1'b0;
    else if (hreadyout_flag & hresp_flag) hresp_error <= 1'b1;
    else hresp_error <= hresp_error;
  end

  assign resp = {1'b0, ((wen | ren) & (hresp_flag | hresp_error))};

endmodule
