// adder

module adder (
  sum,cout,
  a,b,cin
);

  parameter ADD_NUM = 1;
  parameter ADD_WIDTH = 1;
  

  output logic [ADD_NUM-1:0]  cout;
  output logic [ADD_NUM-1:0][ADD_WIDTH-1:0] sum;
  input  logic [ADD_NUM-1:0]  cin;
  input  logic [ADD_NUM-1:0][ADD_WIDTH-1:0] a;
  input  logic [ADD_NUM-1:0][ADD_WIDTH-1:0] b;

  genvar i;
  generate
    for(i=0; i<ADD_NUM; i++) assign {cout[i],sum[i]} = {1'b0, a[i]} + {1'b0, b[i]} + cin[i];
  endgenerate

endmodule
