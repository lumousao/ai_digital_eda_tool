/**
 * Simulator - Uses native RTLIL
 */

#ifndef SIMULATOR_H
#define SIMULATOR_H

#include "simulator_full.h"

namespace Simulator {

// SimResult is defined in simulator_full.h

SimResult simulate(RTLIL::Design *design, const std::string &module_name,
                   const std::string &testbench_code, int max_cycles = 100);

} // namespace Simulator

#endif /* SIMULATOR_H */
