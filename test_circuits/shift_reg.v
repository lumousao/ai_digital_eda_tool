// Test 4: Shift register with enable
module shift_reg #(parameter WIDTH=8, STAGES=4) (
  input  clk, rst_n, en,
  input  [WIDTH-1:0] din,
  output [WIDTH-1:0] dout
);
  reg [WIDTH-1:0] sr [0:STAGES-1];
  integer i;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      for (i = 0; i < STAGES; i = i + 1) sr[i] <= 0;
    end else if (en) begin
      sr[0] <= din;
      for (i = 1; i < STAGES; i = i + 1) sr[i] <= sr[i-1];
    end
  end
  assign dout = sr[STAGES-1];
endmodule
