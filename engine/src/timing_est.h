/**
 * Timing Estimator - lightweight area/delay estimation
 * Based on gate-level statistics from synthesis.
 *
 * Estimates:
 * - Area (gate equivalents)
 * - Critical path delay (logic depth * gate delay)
 * - Dynamic power (switching * capacitance)
 */

#ifndef TIMING_EST_H
#define TIMING_EST_H

// TimingReport is defined in rtl_engine.h (C API header)
// Include it here for C++ usage
#include "../include/rtl_engine.h"

/**
 * Estimate timing from gate counts.
 * Uses typical gate delays for a generic 28nm process.
 */
TimingReport estimate_timing(
    int and_count, int or_count, int not_count, int xor_count,
    int nand_count, int nor_count, int mux_count, int dff_count,
    int other_count, int wire_count,
    const char *module_name
);

void timing_report_free(TimingReport *report);

#endif /* TIMING_EST_H */
