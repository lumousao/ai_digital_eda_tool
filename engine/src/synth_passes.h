/**
 * Synthesis Passes - Uses native RTLIL
 */

#ifndef SYNTH_PASSES_H
#define SYNTH_PASSES_H

#include "rtlil.h"
#include <string>

namespace SynthPasses {

struct SynthStats {
    size_t cell_count;
    size_t wire_count;
    size_t dff_count;
    size_t lut_count;
    size_t and_count;
    size_t or_count;
    size_t not_count;
    size_t xor_count;
    size_t other_count;
    char *report;
};

int synthesize(RTLIL::Design *design, const char *module_name);
SynthStats get_stats(RTLIL::Design *design, const char *module_name);
SynthStats get_stats_from_source(const char *source_code, const char *module_name);
char *to_verilog(RTLIL::Design *design, const char *module_name);

} // namespace SynthPasses

#endif /* SYNTH_PASSES_H */
