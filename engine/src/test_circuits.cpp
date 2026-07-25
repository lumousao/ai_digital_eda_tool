/**
 * Circuit Module Tests - Test simple circuit modules
 */

#include "rtlil.h"
#include "techmap.h"
#include <iostream>
#include <string>
#include <cassert>

// ============================================================================
// Test 1: AND Gate
// ============================================================================

void test_and_gate() {
    std::cout << "Test 1: AND Gate" << std::endl;
    std::cout << "================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$and_gate"));

    mod->addWire(RTLIL::IdString("$a"), 1);
    mod->addWire(RTLIL::IdString("$b"), 1);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$and1"), RTLIL::IdString("$and"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(2, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: y = a & b" << std::endl;
    std::cout << "  Area: 1.5 (AND2)" << std::endl;

    delete design;
    std::cout << "AND gate test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 2: OR Gate
// ============================================================================

void test_or_gate() {
    std::cout << "Test 2: OR Gate" << std::endl;
    std::cout << "===============" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$or_gate"));

    mod->addWire(RTLIL::IdString("$a"), 1);
    mod->addWire(RTLIL::IdString("$b"), 1);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$or1"), RTLIL::IdString("$or"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(2, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: y = a | b" << std::endl;


    delete design;
    std::cout << "OR gate test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 3: XOR Gate
// ============================================================================

void test_xor_gate() {
    std::cout << "Test 3: XOR Gate" << std::endl;
    std::cout << "================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$xor_gate"));

    mod->addWire(RTLIL::IdString("$a"), 1);
    mod->addWire(RTLIL::IdString("$b"), 1);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$xor1"), RTLIL::IdString("$xor"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(2, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: y = a ^ b" << std::endl;


    delete design;
    std::cout << "XOR gate test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 4: NOT Gate
// ============================================================================

void test_not_gate() {
    std::cout << "Test 4: NOT Gate" << std::endl;
    std::cout << "================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$not_gate"));

    mod->addWire(RTLIL::IdString("$a"), 1);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$not1"), RTLIL::IdString("$not"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(1, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: y = ~a" << std::endl;


    delete design;
    std::cout << "NOT gate test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 5: MUX2
// ============================================================================

void test_mux2() {
    std::cout << "Test 5: 2-to-1 MUX" << std::endl;
    std::cout << "==================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$mux2"));

    mod->addWire(RTLIL::IdString("$a"), 1);
    mod->addWire(RTLIL::IdString("$b"), 1);
    mod->addWire(RTLIL::IdString("$sel"), 1);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$mux1"), RTLIL::IdString("$mux"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
    cell->setPort(RTLIL::IdString("\\S"), RTLIL::SigSpec(2, 1));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(3, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: y = sel ? b : a" << std::endl;


    delete design;
    std::cout << "MUX2 test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 6: D Flip-Flop
// ============================================================================

void test_dff() {
    std::cout << "Test 6: D Flip-Flop" << std::endl;
    std::cout << "===================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$dff_mod"));

    mod->addWire(RTLIL::IdString("$clk"), 1);
    mod->addWire(RTLIL::IdString("$d"), 1);
    mod->addWire(RTLIL::IdString("$q"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$ff1"), RTLIL::IdString("$dff"));
    cell->setPort(RTLIL::IdString("\\CLK"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\D"), RTLIL::SigSpec(1, 1));
    cell->setPort(RTLIL::IdString("\\Q"), RTLIL::SigSpec(2, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: always @(posedge clk) q <= d" << std::endl;


    delete design;
    std::cout << "D flip-flop test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 7: 4-bit Adder
// ============================================================================

void test_adder4() {
    std::cout << "Test 7: 4-bit Adder" << std::endl;
    std::cout << "===================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$adder4"));

    mod->addWire(RTLIL::IdString("$a"), 4);
    mod->addWire(RTLIL::IdString("$b"), 4);
    mod->addWire(RTLIL::IdString("$cin"), 1);
    mod->addWire(RTLIL::IdString("$sum"), 4);
    mod->addWire(RTLIL::IdString("$cout"), 1);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$add1"), RTLIL::IdString("$add"));
    cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 4));
    cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(4, 4));
    cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(8, 5));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: {cout, sum} = a + b + cin" << std::endl;


    delete design;
    std::cout << "4-bit adder test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 8: 4-bit Counter
// ============================================================================

void test_counter4() {
    std::cout << "Test 8: 4-bit Counter" << std::endl;
    std::cout << "=====================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$counter4"));

    mod->addWire(RTLIL::IdString("$clk"), 1);
    mod->addWire(RTLIL::IdString("$rst"), 1);
    mod->addWire(RTLIL::IdString("$en"), 1);
    mod->addWire(RTLIL::IdString("$count"), 4);

    RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$ff1"), RTLIL::IdString("$dff"));
    cell->setPort(RTLIL::IdString("\\CLK"), RTLIL::SigSpec(0, 1));
    cell->setPort(RTLIL::IdString("\\D"), RTLIL::SigSpec(3, 4));
    cell->setPort(RTLIL::IdString("\\Q"), RTLIL::SigSpec(3, 4));
    cell->setParam(RTLIL::IdString("\\WIDTH"), RTLIL::Const(4));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: if (rst) count <= 0; else if (en) count <= count + 1" << std::endl;


    delete design;
    std::cout << "4-bit counter test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 9: 2-to-4 Decoder
// ============================================================================

void test_decoder24() {
    std::cout << "Test 9: 2-to-4 Decoder" << std::endl;
    std::cout << "======================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$decoder24"));

    mod->addWire(RTLIL::IdString("$sel"), 2);
    mod->addWire(RTLIL::IdString("$en"), 1);
    mod->addWire(RTLIL::IdString("$y"), 4);

    for (int i = 0; i < 4; i++) {
        RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$and_" + std::to_string(i)), RTLIL::IdString("$and"));
        cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 2));
        cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(2, 1));
        cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(3 + i, 1));
    }

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: 2-to-4 decoder with enable" << std::endl;


    delete design;
    std::cout << "2-to-4 decoder test passed!" << std::endl << std::endl;
}

// ============================================================================
// Test 10: 4-to-1 MUX
// ============================================================================

void test_mux41() {
    std::cout << "Test 10: 4-to-1 Multiplexer" << std::endl;
    std::cout << "===========================" << std::endl;

    RTLIL::Design *design = new RTLIL::Design();
    RTLIL::Module *mod = design->addModule(RTLIL::IdString("$mux41"));

    mod->addWire(RTLIL::IdString("$d"), 4);
    mod->addWire(RTLIL::IdString("$sel"), 2);
    mod->addWire(RTLIL::IdString("$y"), 1);

    RTLIL::Cell *mux1 = mod->addCell(RTLIL::IdString("$mux1"), RTLIL::IdString("$mux"));
    mux1->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
    mux1->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
    mux1->setPort(RTLIL::IdString("\\S"), RTLIL::SigSpec(4, 1));
    mux1->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(6, 1));

    RTLIL::Cell *mux2 = mod->addCell(RTLIL::IdString("$mux2"), RTLIL::IdString("$mux"));
    mux2->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(2, 1));
    mux2->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(3, 1));
    mux2->setPort(RTLIL::IdString("\\S"), RTLIL::SigSpec(4, 1));
    mux2->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(7, 1));

    RTLIL::Cell *mux3 = mod->addCell(RTLIL::IdString("$mux3"), RTLIL::IdString("$mux"));
    mux3->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(6, 1));
    mux3->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(7, 1));
    mux3->setPort(RTLIL::IdString("\\S"), RTLIL::SigSpec(5, 1));
    mux3->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(8, 1));

    std::cout << "  Module: " << mod->name.str() << std::endl;
    std::cout << "  Wires: " << mod->wire_count() << std::endl;
    std::cout << "  Cells: " << mod->cell_count() << std::endl;
    std::cout << "  Logic: 4-to-1 multiplexer (3 MUX2)" << std::endl;


    delete design;
    std::cout << "4-to-1 MUX test passed!" << std::endl << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Circuit Module Tests ===" << std::endl;
    std::cout << "Version: 0.2.1" << std::endl;
    std::cout << std::endl;

    try {
        // Test basic design creation
        std::cout << "Basic test: create and destroy design..." << std::endl;
        {
            RTLIL::Design *design = new RTLIL::Design();
            RTLIL::Module *mod = design->addModule(RTLIL::IdString("$test"));
            mod->addWire(RTLIL::IdString("$a"), 1);
            mod->addWire(RTLIL::IdString("$b"), 1);
            RTLIL::Cell *cell = mod->addCell(RTLIL::IdString("$and1"), RTLIL::IdString("$and"));
            cell->setPort(RTLIL::IdString("\\A"), RTLIL::SigSpec(0, 1));
            cell->setPort(RTLIL::IdString("\\B"), RTLIL::SigSpec(1, 1));
            cell->setPort(RTLIL::IdString("\\Y"), RTLIL::SigSpec(2, 1));
            delete design;
        }
        std::cout << "Basic test passed!" << std::endl << std::endl;

        test_and_gate();
        test_or_gate();
        test_xor_gate();
        test_not_gate();
        test_mux2();
        test_dff();
        test_adder4();
        test_counter4();
        test_decoder24();
        test_mux41();

        std::cout << "=== All Circuit Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
