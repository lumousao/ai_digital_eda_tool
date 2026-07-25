/* Gate-level netlist by ai_digital synthesis */
/* Module: combinational_mix Cells: 13 */

module combinational_mix (a, b, c, d, nand_out, nor_out, xnor_out, and_or_out);
  input a;
  input b;
  input c;
  input d;
  output nand_out;
  output nor_out;
  output xnor_out;
  output and_or_out;
  wire _bin0;
  wire _bin10;
  wire _bin15;
  wire _bin17;
  wire _bin19;
  wire _bin5;
  wire _unary12;
  wire _unary2;
  wire _unary7;
  wire a;
  wire and_or_out;
  wire b;
  wire c;
  wire d;
  wire nand_out;
  wire nor_out;
  wire xnor_out;

  AND2X1 _c1 (.A(a), .B(b), .Y(_bin0));
  INVX1 _c3 (.A(_bin0), .Y(_unary2));
  BUFX2 _c4 (.A(_unary2), .Y(nand_out));
  OR2X1 _c6 (.A(c), .B(d), .Y(_bin5));
  INVX1 _c8 (.A(_bin5), .Y(_unary7));
  BUFX2 _c9 (.A(_unary7), .Y(nor_out));
  XOR2X1 _c11 (.A(a), .B(b), .Y(_bin10));
  INVX1 _c13 (.A(_bin10), .Y(_unary12));
  BUFX2 _c14 (.A(_unary12), .Y(xnor_out));
  AND2X1 _c16 (.A(a), .B(b), .Y(_bin0));
  AND2X1 _c18 (.A(c), .B(d), .Y(_bin17));
  OR2X1 _c20 (.A(_bin0), .B(_bin17), .Y(_bin19));
  BUFX2 _c21 (.A(_bin19), .Y(and_or_out));
endmodule
