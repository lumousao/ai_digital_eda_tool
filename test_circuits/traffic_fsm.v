// Test 3: FSM — 3-state traffic light controller
module traffic_fsm (
  input  clk, rst_n,
  output reg [2:0] lights  // {red, yellow, green}
);
  parameter RED=3'b100, YELLOW=3'b010, GREEN=3'b001;
  reg [1:0] state, next_state;
  reg [3:0] counter;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= 2'd0;
      counter <= 4'd0;
    end else begin
      state <= next_state;
      counter <= counter + 4'd1;
    end
  end

  always @(*) begin
    case (state)
      2'd0: begin lights = RED;    next_state = (counter == 10) ? 2'd1 : 2'd0; end
      2'd1: begin lights = GREEN;  next_state = (counter == 10) ? 2'd2 : 2'd1; end
      2'd2: begin lights = YELLOW; next_state = (counter == 4)  ? 2'd0 : 2'd2; end
      default: begin lights = RED; next_state = 2'd0; end
    endcase
  end
endmodule
