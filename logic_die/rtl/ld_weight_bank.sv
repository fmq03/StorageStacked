// Digital weight-store contract. One synchronous read and one write port.
// The portable implementation maps to registers unless a foundry SRAM is bound.
module ld_weight_bank #(
    parameter WORDS=1024, parameter AW=$clog2(WORDS)
) (
    input clk, input we, input [AW-1:0] waddr, input [15:0] wdata,
    input re, input [AW-1:0] raddr, output reg [15:0] rdata
);
    reg [15:0] mem [0:WORDS-1];
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        if (re) rdata <= mem[raddr];
    end
endmodule
