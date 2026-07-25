/**
 * Synthesis Wrapper - C API for calling libyosys directly
 *
 * This wraps the Yosys C++ library into a C API that can be called from Rust FFI.
 */

#ifndef YOSYS_WRAPPER_H
#define YOSYS_WRAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the Synthesis engine. Must be called before any other function.
 */
void synth_wrapper_init(void);

/**
 * Shutdown the Synthesis engine. Call when done.
 */
void synth_wrapper_shutdown(void);

/**
 * Execute a synthesis command string.
 * Example: "read_verilog file.v; synth; stat"
 * Returns 0 on success, non-zero on error.
 */
int synth_wrapper_run(const char *script);

/**
 * Read a Verilog file into the current design.
 * Returns 0 on success.
 */
int synth_wrapper_read_verilog(const char *filename);

/**
 * Run full synthesis on the current design.
 * top_module: top module name (NULL for auto-detect)
 * Returns 0 on success.
 */
int synth_wrapper_synth(const char *top_module);

/**
 * Run synthesis and output statistics.
 * Returns a JSON string with synthesis results (caller must free).
 */
char *synth_wrapper_synth_and_stat(const char *top_module);

/**
 * Get the last command output/log.
 * Returns a string (caller must free).
 */
char *synth_wrapper_get_output(void);

/**
 * Write the current design to a Verilog file.
 * Returns 0 on success.
 */
int synth_wrapper_write_verilog(const char *filename);

/**
 * Free a string returned by this API.
 */
void synth_wrapper_free(char *str);

#ifdef __cplusplus
}
#endif

#endif /* YOSYS_WRAPPER_H */
