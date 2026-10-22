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

module tlul_dec #(
    parameter TlulDataWidth = 32,
    parameter IdWidth       = 8,
    parameter DeviceType    = 0,        // 1:AXI 0:AHB
    parameter TimeoutLimit  = 16'hffff
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

  localparam TlulStrbWidth = TlulDataWidth >> 3;
  localparam TlulDBWidth = TlulDataWidth >> 3;
  localparam TlulSizeWidth = $clog2($clog2(TlulDBWidth)) + 1;

  localparam CounterWidth = $bits(TimeoutLimit);

  localparam IDLE = 3'h0;
  localparam READ = 3'h1;
  localparam WRITE = 3'h2;
  localparam WAIT_RESP = 3'h3;
  localparam ERROR_HANDLER = 3'h4;

  localparam Get = 3'h4;
  localparam PutFullData = 3'h0;
  localparam PutPartialData = 3'h1;
  localparam AccessAck = 3'h0;
  localparam AccessAckData = 3'h1;

  input clk;
  input rstn;

  // TL-UL interface
  input [2:0] a_opcode;
  input [TlulSizeWidth-1:0] a_size;
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
  output [TlulSizeWidth-1:0] d_size;
  output d_valid;
  output [TlulDataWidth-1:0] d_data;

  // internal signals
  output [TlulSizeWidth-1:0] size;
  output [IdWidth-1:0] req_id;
  output [TlulStrbWidth-1:0] strb_internal;
  output rvalid_internal;
  output wvalid_internal;
  output ren;
  output wen;
  output resp_ready;
  output [31:0] addr;
  output [TlulDataWidth-1:0] wdata_internal;
  input [TlulDataWidth-1:0] rdata_internal;
  input busy;
  input resp_valid;
  input [IdWidth-1:0] resp_id;
  input [1:0] resp;

  reg  [TlulSizeWidth-1:0] size;
  reg  [      IdWidth-1:0] req_id;
  reg  [TlulStrbWidth-1:0] strb_internal;
  reg                      ren;
  reg                      wen;
  reg  [             31:0] addr;
  reg  [TlulDataWidth-1:0] wdata_internal;

  wire                     a_ack;
  wire                     d_ack;




  reg  [ CounterWidth-1:0] counter;
  reg  [              2:0] current_state;
  reg  [              2:0] next_state;
  reg                      timeout;
  reg                      inter_resp_valid;
  reg                      error;
  reg                      addr_sz_chk;
  reg                      mask_chk;
  reg                      fulldata_chk;

  wire [TlulStrbWidth-1:0] mask;
  wire                     err_internal;
  wire                     opcode_allowed;
  wire                     a_config_allowed;

  assign a_ack = a_valid & a_ready;
  assign d_ack = d_valid & d_ready;

  /*************************************************************************************/
  //FSM
  /**************************************************************************************/
  always @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      current_state <= 3'h0;
    end else if (current_state != next_state) begin
      current_state <= next_state;
    end
  end

  always @(*) begin
    case (current_state)
      IDLE: begin
        if (a_ack & !err_internal & a_opcode == Get) next_state = READ;
        else if (a_ack & !err_internal & (a_opcode == PutFullData || a_opcode == PutPartialData))
          next_state = WRITE;
        else if (a_ack & err_internal) next_state = ERROR_HANDLER;
        else next_state = IDLE;
      end
      READ: begin
        if (timeout) next_state = ERROR_HANDLER;
        else if (!busy) next_state = WAIT_RESP;
        else next_state = READ;
      end
      WRITE: begin
        if (timeout) next_state = ERROR_HANDLER;
        else if (!busy) next_state = WAIT_RESP;
        else next_state = WRITE;
      end
      WAIT_RESP: begin
        if (timeout) next_state = ERROR_HANDLER;
        else if (d_ack) next_state = IDLE;
        else next_state = WAIT_RESP;
      end
      ERROR_HANDLER: begin
        if (d_ready) next_state = IDLE;
        else next_state = ERROR_HANDLER;
      end
      default: begin
        next_state = IDLE;
      end
    endcase
  end

  always @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      inter_resp_valid <= 1'b0;
      error            <= 1'b0;
      ren              <= 1'b0;
      wen              <= 1'b0;
    end else begin
      case (next_state)
        IDLE: begin
          inter_resp_valid <= 1'b0;
          error            <= 1'b0;
          ren              <= 1'b0;
          wen              <= 1'b0;
        end
        READ: begin
          ren <= 1'b1;
        end
        WRITE: begin
          wen <= 1'b1;
        end
        WAIT_RESP: begin
        end
        ERROR_HANDLER: begin
          inter_resp_valid <= 1'b1;
          error            <= 1'b1;
        end
        default: begin
          inter_resp_valid <= 1'b0;
          error            <= 1'b0;
          ren              <= 1'b0;
          wen              <= 1'b0;
        end
      endcase
    end
  end

  assign a_ready = (current_state == READ || current_state == WRITE || current_state == WAIT_RESP || current_state == ERROR_HANDLER) ? 1'b0:1'b1;
  assign rvalid_internal = (current_state == READ) ? 1'b1 : 1'b0;
  assign wvalid_internal = (current_state == WRITE) ? 1'b1 : 1'b0;
  assign d_opcode = (error | wen) ? AccessAck : AccessAckData;

  /*************************************************************************************/
  //Time Counter
  /*************************************************************************************/

  always @(posedge clk or negedge rstn) begin
    if (!rstn) counter <= TimeoutLimit - 3;
    else if (d_ack) counter <= TimeoutLimit - 3;
    else if (counter == {CounterWidth{1'b0}}) counter <= counter;
    else if (~a_ready) counter <= counter - 1;
  end

  always @(posedge clk or negedge rstn) begin
    if (!rstn) timeout <= 1'b0;
    else if (d_ack) timeout <= 1'b0;
    else if (counter == {CounterWidth{1'b0}}) timeout <= 1'b1;
  end

  /*************************************************************************************
//Error Check
*************************************************************************************/
  assign err_internal = ~(opcode_allowed & a_config_allowed);

  // opcode check
  generate
    if (DeviceType == 1)  //1: AXI
      assign opcode_allowed = (a_opcode == PutFullData)
                            | (a_opcode == PutPartialData)
                            | (a_opcode == Get);
    else  //0:  AHB
      assign opcode_allowed = (a_opcode == PutFullData) | (a_opcode == Get);
  endgenerate

  //addr/mask/size check
  assign a_config_allowed = addr_sz_chk
                          & mask_chk
                          & (a_opcode == Get | a_opcode == PutPartialData | fulldata_chk);
  assign mask = (1'b1 << a_address[TlulSizeWidth-1:0]);

  always @(*) begin
    if (a_valid) begin
      case (a_size)
        'h0: begin
          addr_sz_chk  = 1'b1;
          mask_chk     = ~|(a_mask & ~mask);
          fulldata_chk = |(a_mask & mask);
        end
        'h1: begin
          addr_sz_chk  = ~a_address[0];
          // check inactive lanes if lower 2B, check a_mask[3:2], if upper 2B, a_mask[1:0]
          mask_chk     = (a_address[1]) ? ~|(a_mask & 4'b0011) : ~|(a_mask & 4'b1100);
          fulldata_chk = (a_address[1]) ? &a_mask[3:2] : &a_mask[1:0];
        end
        'h2: begin
          addr_sz_chk  = ~|a_address[TlulSizeWidth-1:0];
          mask_chk     = 1'b1;
          fulldata_chk = &a_mask[3:0];
        end
        default: begin
          addr_sz_chk  = 1'b0;
          mask_chk     = 1'b0;
          fulldata_chk = 1'b0;
        end
      endcase
    end else begin
      addr_sz_chk  = 1'b1;
      mask_chk     = 1'b1;
      fulldata_chk = 1'b1;
    end
  end

  /*************************************************************************************/
  //directly transmit signals
  /*************************************************************************************/
  assign d_valid = (error | timeout) ? inter_resp_valid : resp_valid;
  assign resp_ready = d_ready;
  assign d_error = (error | timeout) ? error : |resp;
  assign d_source = (error | timeout) ? req_id : resp_id;
  assign d_data = (error | timeout) ? {TlulDataWidth{1'b0}} : rdata_internal;
  assign d_size = size;

  always @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      size           <= {TlulSizeWidth{1'b0}};
      req_id         <= {IdWidth{1'b0}};
      strb_internal  <= {TlulStrbWidth{1'b0}};
      wdata_internal <= {TlulDataWidth{1'b0}};
      addr           <= 32'h0;
    end else if (a_ack) begin
      size           <= a_size;
      req_id         <= a_source;
      strb_internal  <= a_mask;
      wdata_internal <= a_data;
      addr           <= a_address;
    end
  end


endmodule
