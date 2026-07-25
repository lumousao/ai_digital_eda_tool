// Test 5: Memory with read/write
module simple_mem #(parameter ADDR_W=4, DATA_W=8) (
  input  clk, we,
  input  [ADDR_W-1:0] addr,
  input  [DATA_W-1:0] din,
  output [DATA_W-1:0] dout
);
  reg [DATA_W-1:0] mem [0:(1<<ADDR_W)-1];
  always @(posedge clk) begin
    if (we) mem[addr] <= din;
  end
  assign dout = mem[addr];
endmodule
