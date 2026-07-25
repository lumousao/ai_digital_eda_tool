#include "src/sim_engine.h"
#include <iostream>
#include <fstream>

int main() {
    // Read RTL file
    std::ifstream rtl_file("workspace/default/src/sync_fifo.v");
    std::string rtl_code((std::istreambuf_iterator<char>(rtl_file)),
                          std::istreambuf_iterator<char>());

    // Read testbench file
    std::ifstream tb_file("workspace/default/tb/sync_fifo_tb.v");
    std::string tb_code((std::istreambuf_iterator<char>(tb_file)),
                        std::istreambuf_iterator<char>());

    std::cout << "=== Testing Simulation Engine ===" << std::endl;
    std::cout << "RTL: " << rtl_code.length() << " bytes" << std::endl;
    std::cout << "TB: " << tb_code.length() << " bytes" << std::endl;

    // Run simulation
    SimKernel kernel;
    bool result = kernel.simulate(rtl_code, tb_code, "sync_fifo", "clk", 200, 5.0);

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << (result ? "YES" : "NO") << std::endl;
    std::cout << "Output: " << kernel.output() << std::endl;

    return 0;
}
