/**
 * Enhanced Lint Check - Uses native RTLIL and source-level analysis
 */

#ifndef LINT_CHECK_H
#define LINT_CHECK_H

#include "rtlil.h"
#include <string>

namespace LintCheck {

struct LintResult {
    bool passed;
    int warning_count;
    int error_count;
    std::string report;
};

/// Run RTLIL-level lint checks on a synthesized module
LintResult lint_module(RTLIL::Design *design, const std::string &module_name);

/// Run source-level lint checks on Verilog RTL code before synthesis
/// Detects: blocking/non-blocking misuse, latch inference, incomplete sensitivity,
///          case without default, #delay statements, unused signals, etc.
LintResult lint_source(const std::string &source_code, const std::string &module_name);

} // namespace LintCheck

#endif /* LINT_CHECK_H */