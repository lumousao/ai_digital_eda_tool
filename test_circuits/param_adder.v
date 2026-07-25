// Test 2: Parameterized counter with generate-like structure
module param_adder #(parameter WIDTH = 8) (
  input  [WIDTH-1:0] a, b,
  input  cin,
  output [WIDTH-1:0] sum,
  output cout
);
  assign {cout, sum} = a + b + cin;
endmodule
