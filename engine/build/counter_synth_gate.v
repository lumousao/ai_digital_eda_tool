/* Gate-level netlist by ai_digital synthesis */
/* Module: counter Cells: 5 */

module counter (clk, rst_n, en, count);
  input clk;
  input rst_n;
  input en;
  output[7:0] count;
  wire clk;
  wire[7:0] count;
  wire count[0];
  wire count[1];
  wire count[2];
  wire count[3];
  wire count[4];
  wire count[5];
  wire count[6];
  wire count[7];
  wire en;
  wire rst_n;

  DFFSRPOSX1 _dff3 (.D(count[0]), .C(clk), .R(rst_n), .Q(count[0]));
  DFFSRPOSX1 _dff4 (.D(count[1]), .C(clk), .R(rst_n), .Q(count[1]));
  DFFSRPOSX1 _dff5 (.D(count[2]), .C(clk), .R(rst_n), .Q(count[2]));
  DFFSRPOSX1 _dff6 (.D(count[3]), .C(clk), .R(rst_n), .Q(count[3]));
  DFFSRPOSX1 _dff7 (.D(count[4]), .C(clk), .R(rst_n), .Q(count[4]));
endmodule
