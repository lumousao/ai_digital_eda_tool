// Test 1: Pure combinational with assign — NAND + NOR + XNOR
module combinational_mix (
  input  a, b, c, d,
  output nand_out, nor_out, xnor_out, and_or_out
);
  assign nand_out = ~(a & b);
  assign nor_out  = ~(c | d);
  assign xnor_out = ~(a ^ b);
  assign and_or_out = (a & b) | (c & d);
endmodule
