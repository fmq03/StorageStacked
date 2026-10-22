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

module tlul2ahblite #(
    parameter TlulDataWidth    = 32,
    parameter AhbLiteDataWidth = 32,
    parameter IdWidth          = 8,
    parameter TimeoutLimit     = 16'hffff,
    parameter SizeWidth        = 2
) (
    clk,
    rstn,
    // TL-UL interface
    a_opcode,
    a_size,
    a_source,
    a_address,
    a_mask,
    a_data,
    a_valid,
    a_ready,

    d_ready,
    d_error,
    d_source,
    d_opcode,
    d_size,
    d_valid,
    d_data,

    // AhbLite signals
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
    hready
);

  localparam TlulStrbWidth = TlulDataWidth >> 3;
  localparam AhbLiteStrbWidth = AhbLiteDataWidth >> 3;
  localparam CounterWidth = $bits(TimeoutLimit);

  input clk;
  input rstn;
  //TL-UL interface
  input [2:0] a_opcode;
  input [SizeWidth-1:0] a_size;
  input [IdWidth-1:0] a_source;
  input [31:0] a_address;
  input [TlulStrbWidth-1:0] a_mask;
  input [TlulDataWidth-1:0] a_data;
  input a_valid;
  output a_ready;

  input d_ready;
  output d_error;
  output [IdWidth-1:0] d_source;
  output [2:0] d_opcode;
  output [SizeWidth-1:0] d_size;
  output d_valid;
  output [TlulDataWidth-1:0] d_data;

  // AHB Lite interface
  output [31:0] haddr;
  output [2:0] hburst;
  output hready;
  output hsel;
  output [2:0] hsize;
  output [1:0] htrans;
  output [AhbLiteDataWidth-1:0] hwdata;
  output hwrite;
  input [AhbLiteDataWidth-1:0] hrdata;
  input hreadyout;
  input hresp;



  /*AUTOWIRE*/
  // Beginning of automatic wires (for undeclared instantiated-module outputs)
  wire [TlulDataWidth-1:0] rdata_internal;  // From u_ahblite_enc of ahblite_enc.v
  wire [TlulStrbWidth-1:0] strb_internal;  // From u_tlul_dec of tlul_dec.v
  wire [TlulDataWidth-1:0] wdata_internal;  // From u_tlul_dec of tlul_dec.v
  // End of automatics
  wire [31:0] addr;  // From u_tlul_dec of tlul_dec.v
  wire busy;  // From u_ahblite_enc of ahblite_enc.v
  wire ren;  // From u_tlul_dec of tlul_dec.v
  wire [IdWidth-1:0] req_id;  // From u_tlul_dec of tlul_dec.v
  wire [1:0] resp;  // From u_ahblite_enc of ahblite_enc.v
  wire [IdWidth-1:0] resp_id;  // From u_ahblite_enc of ahblite_enc.v
  wire resp_ready;  // From u_tlul_dec of tlul_dec.v
  wire resp_valid;  // From u_ahblite_enc of ahblite_enc.v
  wire rvalid_internal;  // From u_tlul_dec of tlul_dec.v
  wire [SizeWidth-1:0] size;  // From u_tlul_dec of tlul_dec.v
  wire wen;  // From u_tlul_dec of tlul_dec.v
  wire wvalid_internal;  // From u_tlul_dec of tlul_dec.v
  // End of automatics
  tlul_dec #(
      .TlulDataWidth(TlulDataWidth),
      .IdWidth      (IdWidth),
      .TimeoutLimit (TimeoutLimit)
  ) u_tlul_dec (  /*AUTOINST*/
      // Outputs
      .a_ready(a_ready),
      .d_error(d_error),
      .d_source(d_source[IdWidth-1:0]),
      .d_opcode(d_opcode[2:0]),
      .d_size(d_size),
      .d_valid(d_valid),
      .d_data(d_data[TlulDataWidth-1:0]),
      .size(size),
      .req_id(req_id[IdWidth-1:0]),
      .strb_internal(strb_internal[TlulStrbWidth-1:0]),
      .rvalid_internal(rvalid_internal),
      .wvalid_internal(wvalid_internal),
      .ren(ren),
      .wen(wen),
      .resp_ready(resp_ready),
      .addr(addr[31:0]),
      .wdata_internal(wdata_internal[TlulDataWidth-1:0]),
      // Inputs
      .clk(clk),
      .rstn(rstn),
      .a_opcode(a_opcode[2:0]),
      .a_size(a_size),
      .a_source(a_source[IdWidth-1:0]),
      .a_address(a_address[31:0]),
      .a_mask(a_mask[TlulStrbWidth-1:0]),
      .a_data(a_data[TlulDataWidth-1:0]),
      .a_valid(a_valid),
      .d_ready(d_ready),
      .rdata_internal(rdata_internal[TlulDataWidth-1:0]),
      .busy(busy),
      .resp_valid(resp_valid),
      .resp_id(resp_id[IdWidth-1:0]),
      .resp(resp[1:0])
  );



  ahblite_enc #(
      .AhbLiteDataWidth(AhbLiteDataWidth),
      .TlulDataWidth(TlulDataWidth),
      .IdWidth(IdWidth)
  ) u_ahblite_enc (  /*AUTOINST*/
      // Outputs
      .haddr(haddr[31:0]),
      .hburst(hburst[2:0]),
      .hsize(hsize[2:0]),
      .htrans(htrans[1:0]),
      .hwdata(hwdata[AhbLiteDataWidth-1:0]),
      .hwrite(hwrite),
      .hsel(hsel),
      .hready(hready),
      .rdata_internal(rdata_internal[TlulDataWidth-1:0]),
      .busy(busy),
      .resp_valid(resp_valid),
      .resp_id(resp_id[IdWidth-1:0]),
      .resp(resp[1:0]),
      // Inputs
      .clk(clk),
      .rstn(rstn),
      .hrdata(hrdata[AhbLiteDataWidth-1:0]),
      .hreadyout(hreadyout),
      .hresp(hresp),
      .size(size),
      .req_id(req_id[IdWidth-1:0]),
      .strb_internal(strb_internal),
      .rvalid_internal(rvalid_internal),
      .wvalid_internal(wvalid_internal),
      .ren(ren),
      .wen(wen),
      .resp_ready(resp_ready),
      .addr(addr[31:0]),
      .wdata_internal(wdata_internal[TlulDataWidth-1:0])
  );


endmodule
