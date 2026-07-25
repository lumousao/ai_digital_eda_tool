// Test 1: Simple AND gate (minimal circuit, 2 cells)
module and_gate (
  input  a,
  input  b,
  output y
);
  assign y = a & b;
endmodule
