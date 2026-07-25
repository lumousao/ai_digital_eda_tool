// Test 7: Deeply nested if-else with multiple operators
module nested_ops (
  input  [7:0] a, b, c,
  input  [1:0] op,
  output reg [15:0] result
);
  always @(*) begin
    if (op == 2'd0)
      result = a + b;
    else if (op == 2'd1)
      result = a - b;
    else if (op == 2'd2) begin
      if (a > b) result = a * c;
      else result = b * c;
    end else
      result = (a < b) ? (a + c) : (b - c);
  end
endmodule
