// handshake_ff - flip-flop with handshake mechanism

module handshake_ff (
  indata,
  invalid,
  inready,
  outdata,
  outvalid,
  outready,
  c,
  clk, rst_n
);
  parameter type T = logic;
  parameter T INIT  = T'('0);

  output T        outdata;
  output logic    outvalid;
  input  logic    outready;

  input  T        indata;
  input  logic    invalid;
  output logic    inready;
  input  logic    c;
  input  logic    clk;
  input  logic    rst_n;

  logic           data_en;
  logic           valid_en;

  assign data_en = invalid & inready;
  edff #(.T(T)) data_reg (.q(outdata), .d(indata), .e(data_en), .clk(clk), .rst_n(rst_n));

  assign valid_en = invalid & inready | outvalid & outready;
  cdffr #(.T(logic)) valid_reg (.q(outvalid), .d(invalid), .c(c), .e(valid_en), .clk(clk), .rst_n(rst_n));

  assign inready = ~outvalid | outready;
endmodule
