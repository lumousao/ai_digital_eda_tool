// 16-bit MAC — multiply-accumulate
module mac16 (
  input         clk,
  input         rst_n,
  input         en,
  input  [15:0] a,
  input  [15:0] b,
  input         clear,
  output reg [31:0] result
);
  wire [31:0] product;
  assign product = a * b;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      result <= 32'b0;
    else if (clear)
      result <= 32'b0;
    else if (en)
      result <= result + product;
  end
endmodule
