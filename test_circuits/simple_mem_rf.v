// Test 5b: Simple memory — register-file style (works with current parser)
module simple_mem_rf #(parameter ADDR_W=4, DATA_W=8) (
  input  clk, we,
  input  [ADDR_W-1:0] addr,
  input  [DATA_W-1:0] din,
  output [DATA_W-1:0] dout
);
  // Expand as flat registers for small address space
  reg [DATA_W-1:0] mem0, mem1, mem2, mem3;
  reg [DATA_W-1:0] mem4, mem5, mem6, mem7;
  reg [DATA_W-1:0] mem8, mem9, mem10, mem11;
  reg [DATA_W-1:0] mem12, mem13, mem14, mem15;

  always @(posedge clk) begin
    if (we) begin
      case (addr)
        4'd0:  mem0  <= din;  4'd1:  mem1  <= din;
        4'd2:  mem2  <= din;  4'd3:  mem3  <= din;
        4'd4:  mem4  <= din;  4'd5:  mem5  <= din;
        4'd6:  mem6  <= din;  4'd7:  mem7  <= din;
        4'd8:  mem8  <= din;  4'd9:  mem9  <= din;
        4'd10: mem10 <= din;  4'd11: mem11 <= din;
        4'd12: mem12 <= din;  4'd13: mem13 <= din;
        4'd14: mem14 <= din;  4'd15: mem15 <= din;
      endcase
    end
  end

  assign dout = (addr == 0) ? mem0  : (addr == 1)  ? mem1  :
                (addr == 2) ? mem2  : (addr == 3)  ? mem3  :
                (addr == 4) ? mem4  : (addr == 5)  ? mem5  :
                (addr == 6) ? mem6  : (addr == 7)  ? mem7  :
                (addr == 8) ? mem8  : (addr == 9)  ? mem9  :
                (addr == 10)? mem10 : (addr == 11) ? mem11 :
                (addr == 12)? mem12 : (addr == 13) ? mem13 :
                (addr == 14)? mem14 : mem15;
endmodule
