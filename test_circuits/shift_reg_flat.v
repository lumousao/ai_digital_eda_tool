// Test 4b: Shift register — flat version (works with current parser)
module shift_reg_flat #(parameter WIDTH=8, STAGES=4) (
  input  clk, rst_n, en,
  input  [WIDTH-1:0] din,
  output [WIDTH-1:0] dout
);
  reg [WIDTH-1:0] sr0, sr1, sr2, sr3;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      sr0 <= 0; sr1 <= 0; sr2 <= 0; sr3 <= 0;
    end else if (en) begin
      sr0 <= din;
      sr1 <= sr0;
      sr2 <= sr1;
      sr3 <= sr2;
    end
  end
  assign dout = sr3;
endmodule
