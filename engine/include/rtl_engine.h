/**
 * RTL Engine - C API for Rust FFI
 *
 * References design patterns from:
 * - industry-standard RTLIL (RTLIL data structures)
 * - industry-standard parser (parser patterns)
 * - industry-standard optimization (optimization passes)
 */

#ifndef RTL_ENGINE_H
#define RTL_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Opaque handles ========== */
typedef struct RtlDesign RtlDesign;
typedef struct RtlModule RtlModule;

/* ========== Error handling ========== */
typedef struct RtlError {
    int code;           /* 0 = success */
    char *message;      /* error message (caller must free) */
    int line;           /* source line number (0 if N/A) */
    char *file;         /* source file name (caller must free) */
} RtlError;

/* Free an error struct */
void rtl_error_free(RtlError *err);

/* ========== Design lifecycle ========== */

/* Create a new empty design */
RtlDesign *rtl_design_new(void);

/* Destroy a design and free all resources */
void rtl_design_free(RtlDesign *design);

/* ========== Verilog parsing ========== */
/* References: industry-standard Verilog parser */

/**
 * Parse a Verilog file and add modules to the design.
 * Returns NULL on success, or an error with details.
 */
RtlError *rtl_parse_verilog(RtlDesign *design, const char *filename);

/**
 * Parse Verilog from a string buffer.
 */
RtlError *rtl_parse_verilog_str(RtlDesign *design, const char *code,
                                 size_t len, const char *name);

/* ========== Module access ========== */

/* Get the number of modules in the design */
size_t rtl_design_module_count(RtlDesign *design);

/* Get module name by index. Returns NULL if index out of range. */
const char *rtl_design_module_name(RtlDesign *design, size_t index);

/* ========== Lint checking ========== */
/* References: industry-standard lint tools, V3Undriven.cpp, V3Width.cpp */

typedef struct RtlLintResult {
    int passed;             /* 1 if passed, 0 if errors found */
    int warning_count;
    int error_count;
    char *report;           /* human-readable report (caller must free) */
} RtlLintResult;

/**
 * Run lint checks on a specific module.
 * Checks: undriven signals, unused signals, width mismatches, etc.
 */
RtlLintResult rtl_lint_check(RtlDesign *design, const char *module_name);

/* Run source-level lint checks on Verilog RTL code before synthesis */
RtlLintResult rtl_lint_source(const char *source_code, const char *module_name);

/* Free a lint result */
void rtl_lint_result_free(RtlLintResult *result);

/* ========== Synthesis ========== */
/* References: industry-standard synthesis, passes/opt/ */

typedef struct RtlSynthStats {
    size_t cell_count;
    size_t wire_count;
    size_t dff_count;       /* flip-flops */
    size_t lut_count;       /* look-up tables */
    size_t and_count;
    size_t or_count;
    size_t not_count;
    size_t xor_count;
    size_t other_count;
    char *report;           /* human-readable report (caller must free) */
} RtlSynthStats;

/**
 * Run synthesis on a module.
 * Maps behavioral RTL to gate-level primitives.
 */
RtlError *rtl_synthesize(RtlDesign *design, const char *module_name);

/**
 * Get synthesis statistics after rtl_synthesize().
 */
RtlSynthStats rtl_synth_stats(RtlDesign *design, const char *module_name);

/**
 * Get synthesis statistics directly from Verilog source code.
 * Bypasses the C++ parser's limited cell detection.
 */
RtlSynthStats rtl_synth_stats_from_source(const char *source_code, const char *module_name);

/* Free synth stats */
void rtl_synth_stats_free(RtlSynthStats *stats);

/* ========== Timing Estimation ========== */

typedef struct TimingReport {
    double area_ge;
    double delay_ns;
    double power_mw;
    int logic_depth;
    int total_gates;
    int dff_count;
    double clock_period_ns;
    double arrival_time_ns;
    double required_time_ns;
    double slack_ns;
    int timing_met;            /* 1 = MET, 0 = VIO */
    char *report;
} TimingReport;

TimingReport rtl_estimate_timing(RtlDesign *design, const char *module_name);
void rtl_timing_report_free(TimingReport *report);

/**
 * Run detailed timing analysis using Liberty library.
 * synth_output: full synthesis stat output
 * module_name: target module name
 * liberty_file: path to liberty file (NULL for default)
 * clock_period: clock period in ns
 */
TimingReport rtl_timing_analysis(const char *synth_output, const char *module_name,
                                  const char *liberty_file, double clock_period);

/**
 * Run per-corner timing analysis using the specified Liberty library.
 * PVT scaling is used only if the corner library cannot be loaded.
 *
 * @param synth_output synthesis output text
 * @param module_name target module name
 * @param corner_type "ss", "ff", or "tt"
 * @param voltage supply voltage in Volts (e.g. 1.08, 1.2, 1.32)
 * @param temperature junction temperature in Celsius (e.g. -40, 25, 125)
 * @param clock_period clock period in ns
 */
TimingReport rtl_timing_analysis_corner(const char *synth_output, const char *module_name,
    const char *liberty_file, const char *corner_type, double voltage,
    double temperature, double clock_period);

/**
 * Generate SDC constraint file for the design.
 * Returns the SDC content as a string (caller must free).
 */
char *rtl_generate_sdc(const char *module_name, double clock_period,
                        const char *clock_port, const char *liberty_file);

/**
 * Scan multiple clock frequencies to find max frequency where timing is MET.
 * Returns array of timing reports for each frequency tested.
 * The caller must free each report and the array.
 */
int rtl_clock_scan(const char *synth_output, const char *module_name,
                    const char *liberty_file,
                    double min_period, double max_period, double step,
                    TimingReport **results, int *count);

/* ========== Output ========== */

/**
 * Write synthesized design back to Verilog.
 * Returns the Verilog code as a string (caller must free).
 */
char *rtl_to_verilog(RtlDesign *design, const char *module_name);

/**
 * Get a summary of the design (JSON format).
 * Returns a JSON string (caller must free).
 */
char *rtl_design_summary(RtlDesign *design);

/* ========== Utilities ========== */

/* Get engine version string */
const char *rtl_engine_version(void);

/* ========== Simulator ========== */

typedef struct SimResult {
    int passed;             /* 1 if PASS, 0 if FAIL */
    int exit_code;          /* Process exit code */
    int time_steps;         /* Number of time steps */
    char *output;           /* Captured output (caller must free) */
    char *vcd_file;         /* VCD file path (caller must free) */
} SimResult;

/**
 * Run simulation on RTL code with testbench.
 * Uses the built-in simulator engine (no external tools).
 *
 * @param rtl_code RTL source code
 * @param tb_code Testbench source code
 * @param module_name Top module name
 * @param clk_port Clock port name
 * @param num_cycles Number of clock cycles to simulate
 * @param half_period_ns Half-clock period in ns
 * @return Simulation result (caller must free output and vcd_file)
 */
SimResult rtl_simulate(const char *rtl_code, const char *tb_code,
                       const char *module_name, const char *clk_port,
                       int num_cycles, double half_period_ns);

/**
 * Free simulation result.
 */
void rtl_sim_result_free(SimResult *result);

/**
 * Run simulation with memory limit.
 */
SimResult rtl_simulate_with_limit(const char *rtl_code, const char *tb_code,
                                  const char *module_name, const char *clk_port,
                                  int num_cycles, double half_period_ns,
                                  size_t memory_limit_mb);

/**
 * Run simulation with memory limit and timeout.
 */
SimResult rtl_simulate_with_limit_and_timeout(const char *rtl_code, const char *tb_code,
                                              const char *module_name, const char *clk_port,
                                              int num_cycles, double half_period_ns,
                                              size_t memory_limit_mb, int timeout_seconds);

/* Log callback type */
typedef void (*SimLogCallback)(const char *category, const char *message);

/**
 * Set simulation log callback for detailed logging.
 */
void rtl_set_sim_log_callback(SimLogCallback cb);

/**
 * Set per-module engine log callbacks for detailed internal data logging.
 * These allow the Rust layer to receive C++ engine internal state and
 * record it in detail.log for debugging and verification.
 */
void rtl_set_synth_log_callback(SimLogCallback cb);
void rtl_set_timing_log_callback(SimLogCallback cb);
void rtl_set_power_log_callback(SimLogCallback cb);
void rtl_set_formal_log_callback(SimLogCallback cb);

/**
 * Get current process memory usage in MB.
 */
size_t rtl_get_process_memory_mb(void);

/**
 * Get system information (CPU, RAM, load)
 */
int rtl_get_cpu_cores(void);
int rtl_get_cpu_threads(void);
size_t rtl_get_total_ram_mb(void);
size_t rtl_get_available_ram_mb(void);
double rtl_get_load_1min(void);
const char* rtl_get_cpu_model(void);
char* rtl_get_system_info_json(void);
void rtl_free_system_info(char* s);

/**
 * Run formal equivalence check between RTL and gate-level netlist.
 * Uses exhaustive vectors for small combinational interfaces and deterministic
 * functional regression for larger combinational and sequential designs.
 *
 * @param rtl_code Original RTL source
 * @param gate_code Gate-level netlist source
 * @param module_name Module name to check
 * @return 1 if equivalent, 0 if not, -1 if the check is inconclusive or invalid
 */
int rtl_formal_check(const char *rtl_code, const char *gate_code,
                     const char *module_name);

/* ========== Real Synthesis ========== */

typedef struct SynthResult {
    char *gate_verilog;       /* Gate-level Verilog netlist */
    char *report;             /* Synthesis report text */
    size_t cell_count;        /* Total cell count */
    size_t wire_count;        /* Wire count */
    size_t dff_count;         /* DFF count */
    size_t port_count;        /* Port count */
    double area_ge;           /* Area in gate equivalents */
    double area_um2;          /* Area in µm² from liberty (0 if not available) */
    int area_from_lib;        /* 1 if area_um2 comes from real lib */
    char *lib_name;           /* Liberty library name used */
    int logic_depth;          /* Logic depth */
    int success;              /* 1 if successful */
    char *error;              /* Error message if failed */
    char **cell_types;        /* Cell type names */
    size_t *cell_type_counts; /* Cell type counts */
    size_t num_cell_types;    /* Number of cell types */
} SynthResult;

typedef struct RtlSynthesisOptions {
    int constprop;
    int dead_code_elimination;
    int common_subexpression_elimination;
    int expression_optimization;
    int demorgan;
    int width_reduction;
    int resource_sharing;
    int fsm_extraction;
    int logic_minimization;
    int retiming;
    int boundary_optimization;
} RtlSynthesisOptions;

/**
 * Run real logic synthesis on RTL code.
 * Generates a gate-level netlist with standard cell instances.
 *
 * @param rtl_code RTL source code
 * @param module_name Top module name
 * @return SynthResult (caller must free with synth_result_free)
 */
SynthResult rtl_synthesize_real(const char *rtl_code, const char *module_name);

/**
 * Run real logic synthesis on RTL code with Liberty library.
 * Uses real liberty cell areas (µm²) instead of GE estimates.
 *
 * @param rtl_code RTL source code
 * @param module_name Top module name
 * @param liberty_path Path to .lib file (NULL for default GE mode)
 * @return SynthResult (caller must free with synth_result_free)
 */
SynthResult rtl_synthesize_real_with_lib(const char *rtl_code, const char *module_name,
                                          const char *liberty_path);

/** Run native logic synthesis with an explicit per-pass policy. */
SynthResult rtl_synthesize_real_with_options(const char *rtl_code, const char *module_name,
                                              const char *liberty_path,
                                              const RtlSynthesisOptions *options);

/**
 * Frequency-optimized synthesis.
 * Iteratively applies optimization passes to push max frequency toward target_ratio * constraint_mhz.
 * If target cannot be reached, relaxes to best achievable.
 */
SynthResult rtl_synthesize_freq_optimized(const char *rtl_code, const char *module_name,
                                           const char *liberty_path,
                                           int constraint_mhz, double target_ratio);

/**
 * Free synthesis result.
 */
void rtl_synth_result_free(SynthResult *result);

/**
 * Export synthesis result to JSON string (caller must free).
 * Provides structured access to all synthesis output data.
 */
char *rtl_synth_result_to_json(const SynthResult *result);

/**
 * Export timing report to JSON string (caller must free).
 */
char *rtl_timing_report_to_json(const TimingReport *report);

/* ========== Data Detection Ports ========== */

typedef struct RtlDataDetectPort {
    char *signal_name;      /* Signal to monitor */
    char *port_type;        /* "input", "output", "internal" */
    int width;              /* Signal width */
    int detect_toggle;      /* Detect toggle events */
    int detect_transition;  /* Detect 0->1 or 1->0 transitions */
    int detect_value_change;/* Detect any value change */
    int threshold;          /* Threshold for anomaly detection */
} RtlDataDetectPort;

/**
 * Add a data detection port for monitoring.
 * Returns 0 on success, -1 on error.
 */
int rtl_add_data_detect_port(RtlDesign *design, const char *module_name,
                             RtlDataDetectPort *port);

/**
 * Get monitoring status for a module.
 * Returns JSON string with monitoring data (caller must free).
 */
char *rtl_get_monitor_status(RtlDesign *design, const char *module_name);

/**
 * Check for anomalies in monitored signals.
 * Returns JSON string with anomaly list (caller must free).
 */
char *rtl_check_anomalies(RtlDesign *design, const char *module_name);

/* ========== Real-time Monitoring ========== */

typedef struct RtlMonitorConfig {
    int enable_cycle_counting;   /* Enable cycle counting */
    int enable_signal_toggles;   /* Enable signal toggle tracking */
    int enable_anomaly_detection;/* Enable anomaly detection */
    int max_cycles;              /* Maximum cycles before timeout */
    double timeout_seconds;      /* Timeout in seconds */
} RtlMonitorConfig;

/**
 * Configure real-time monitoring for a module.
 */
void rtl_configure_monitor(RtlDesign *design, const char *module_name,
                           RtlMonitorConfig *config);

/**
 * Get real-time status as JSON (caller must free).
 */
char *rtl_get_realtime_status(RtlDesign *design, const char *module_name);

/* ========== Automatic Retry/Optimization ========== */

typedef struct RtlAutoFixConfig {
    int enable_auto_fix;         /* Enable automatic retry/optimization */
    int max_retries;             /* Maximum retry attempts */
    int enable_timing_opt;       /* Enable timing optimization */
    int enable_area_opt;         /* Enable area optimization */
    int enable_power_opt;        /* Enable power optimization */
} RtlAutoFixConfig;

/**
 * Configure automatic retry/optimization.
 */
void rtl_configure_auto_fix(RtlDesign *design, const char *module_name,
                            RtlAutoFixConfig *config);

/**
 * Run auto-fix loop: detect issues → optimize → retry.
 * Returns 0 on success, -1 if max retries exceeded.
 */
int rtl_run_auto_fix(RtlDesign *design, const char *module_name);

/* ========== Multi-threading ========== */

/**
 * Run simulation with multi-threading support.
 * Uses thread pool for parallel evaluation of independent blocks.
 */
SimResult rtl_simulate_mt(const char *rtl_code, const char *tb_code,
                          const char *module_name, const char *clk_port,
                          int num_cycles, double half_period_ns,
                          int num_threads);

/**
 * Run synthesis with multi-threading support.
 * Uses thread pool for parallel processing of modules.
 */
RtlError *rtl_synthesize_mt(RtlDesign *design, const char *module_name,
                            int num_threads);

/* ========== Utilities ========== */

/**
 * Liberty library header information (fast parse, no cell data).
 * Used for multi-corner detection and process grouping.
 */
typedef struct LibertyInfo {
    char *library_name;              /* Library name from header */
    double nom_process;              /* Nominal process value */
    double nom_temperature;          /* Nominal temperature in Celsius */
    double nom_voltage;              /* Nominal voltage in Volts */
    char *default_op_conditions;     /* Default operating conditions */
    int cell_count;                  /* Number of cell definitions */
    double default_leakage_power;    /* Default cell leakage power */
    double default_max_transition;   /* Default max transition */
} LibertyInfo;

/**
 * Parse liberty library header information.
 * Fast parse - only reads header and counts cells, not full cell data.
 * Use for auto-detection and corner grouping.
 *
 * @param filename Path to .lib file
 * @return LibertyInfo (caller must free with rtl_liberty_info_free)
 */
LibertyInfo rtl_parse_liberty_info(const char *filename);

/**
 * Free liberty info struct and all allocated strings.
 */
void rtl_liberty_info_free(LibertyInfo *info);

/**
 * Get engine build info (version, features, thread count).
 * Returns JSON string (caller must free).
 */
char *rtl_engine_info(void);

/* ========== Power Analysis ========== */

typedef struct PowerAnalysisResult {
    double total_power_uw;
    double static_power_uw;
    double dynamic_power_uw;
    double internal_power_uw;
    double switching_power_uw;
    double clock_power_uw;
    double leakage_power_uw;
    char *report;          /* human-readable report (caller must free) */
} PowerAnalysisResult;

/**
 * Run power analysis on a gate-level netlist.
 * Uses Liberty library for cell power data if available.
 *
 * @param gate_netlist   Gate-level Verilog netlist text
 * @param module_name    Top module name
 * @param liberty_path   Path to .lib file (NULL for RTL estimation)
 * @param clock_freq_mhz Clock frequency in MHz
 * @param toggle_data_json JSON with toggle rate data (NULL for default)
 * @return PowerAnalysisResult (caller must free report with rtl_power_result_free)
 */
PowerAnalysisResult rtl_power_analyze(const char *gate_netlist, const char *module_name,
                                       const char *liberty_path, double clock_freq_mhz,
                                       const char *toggle_data_json);

/**
 * Free power analysis result.
 */
void rtl_power_result_free(PowerAnalysisResult *result);

#ifdef __cplusplus
}
#endif

#endif /* RTL_ENGINE_H */
