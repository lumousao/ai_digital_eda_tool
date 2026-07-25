// Test circuit: 8-bit comparator for verifying LT/GT/EQ/NE synthesis
module comparator_8bit (
  input  [7:0] a,
  input  [7:0] b,
  output       a_eq_b,
  output       a_ne_b,
  output       a_lt_b,
  output       a_gt_b,
  output       a_le_b,
  output       a_ge_b
);
  assign a_eq_b = (a == b);
  assign a_ne_b = (a != b);
  assign a_lt_b = (a < b);
  assign a_gt_b = (a > b);
  assign a_le_b = (a <= b);
  assign a_ge_b = (a >= b);
endmodule
