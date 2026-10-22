module arb_round_robin(
  grant,
  req,
  clk,
  rst_n
);
  parameter REQ_NUM         = 2;

  input   logic               clk;
  input   logic               rst_n;
  input   logic [REQ_NUM-1:0] req;
  output  logic [REQ_NUM-1:0] grant;

// ---internal signal definition--------------------------------------
  logic [REQ_NUM-1:0]   prio;
  logic [REQ_NUM-1:0]   prio_new;
  logic                 prio_en;
  logic [2*REQ_NUM-1:0] grant_tmp;
  
  assign grant_tmp  = {req,req} & ~({req,req} - (2*REQ_NUM)'(prio));
  assign grant      = grant_tmp[2*REQ_NUM-1:REQ_NUM] | grant_tmp[REQ_NUM-1:0];
  
  assign prio_en    = |req;
  assign prio_new   = {grant[REQ_NUM-2:0],grant[REQ_NUM-1]};

  edff #(
    .T      (logic [REQ_NUM-1:0]),
    .INIT   ((REQ_NUM)'('b1))
  ) priority_reg (
    .q      (prio), 
    .e      (prio_en), 
    .d      (prio_new), 
    .clk    (clk), 
    .rst_n  (rst_n)
  );

endmodule
