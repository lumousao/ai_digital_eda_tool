// 4-bit counter with async reset — test circuit
module counter_4bit (
  input        clk,
  input        rst_n,
  input        en,
  output reg [3:0] q
);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      q <= 4'b0;
    else if (en)
      q <= q + 4'd1;
  end
endmodule
