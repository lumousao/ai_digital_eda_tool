// Test 8: Multi-module hierarchy
module sub_adder (
  input  [3:0] a, b,
  output [4:0] sum
);
  assign sum = a + b;
endmodule

module sub_mult (
  input  [3:0] a, b,
  output [7:0] prod
);
  assign prod = a * b;
endmodule

module top_hierarchy (
  input  [3:0] x, y, z,
  output [8:0] result
);
  wire [4:0] add_out;
  wire [7:0] mul_out;
  sub_adder u_add (.a(x), .b(y), .sum(add_out));
  sub_mult  u_mul (.a(add_out[3:0]), .b(z), .prod(mul_out));
  assign result = mul_out + {4'b0, add_out};
endmodule
