// Test 3: 8-bit ALU (中等复杂度, ~100+ cells)
// Supports: ADD, SUB, AND, OR, XOR, NOT, LSHIFT, RSHIFT
module alu_8bit (
  input        [7:0] a,
  input        [7:0] b,
  input        [2:0] op,
  output reg   [7:0] result,
  output             zero,
  output             carry
);
  reg [8:0] tmp; // 9-bit for carry
  always @(*) begin
    case (op)
      3'd0: result = a + b;       // ADD
      3'd1: result = a - b;       // SUB
      3'd2: result = a & b;       // AND
      3'd3: result = a | b;       // OR
      3'd4: result = a ^ b;       // XOR
      3'd5: result = ~a;          // NOT
      3'd6: result = a << b[2:0]; // LSHIFT
      3'd7: result = a >> b[2:0]; // RSHIFT
      default: result = 8'b0;
    endcase
  end
  assign zero = (result == 8'b0);
  assign carry = (a + b) > 8'hFF;
endmodule
