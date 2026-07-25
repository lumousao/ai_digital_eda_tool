/* Gate-level netlist by ai_digital synthesis */
/* Module: top Cells: 2 */

module top (a, b, y);
  input a;
  input b;
  output y;
  wire _bin0;

  AND2X1 _c1 (.A(a), .B(b), .Y(_bin0));
  BUFX2 _c2 (.A(_bin0), .Y(y));
endmodule
