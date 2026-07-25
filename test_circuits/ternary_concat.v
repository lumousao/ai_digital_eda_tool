// Test 6: Ternary operator + concatenation + conditional
module ternary_concat (
  input  [7:0] a, b,
  input  sel,
  output [15:0] result
);
  assign result = sel ? {a, b} : {b, a};
endmodule
