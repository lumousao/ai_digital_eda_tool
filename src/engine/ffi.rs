/**
 * Raw FFI bindings to the C++ RTL engine
 */

use std::os::raw::c_char;

#[repr(C)]
pub struct RtlSynthesisOptions {
    pub constprop: i32,
    pub dead_code_elimination: i32,
    pub common_subexpression_elimination: i32,
    pub expression_optimization: i32,
    pub demorgan: i32,
    pub width_reduction: i32,
    pub resource_sharing: i32,
    pub fsm_extraction: i32,
    pub logic_minimization: i32,
    pub retiming: i32,
    pub boundary_optimization: i32,
}

#[repr(C)]
pub struct RtlDesign {
    _private: [u8; 0],
}

#[repr(C)]
pub struct RtlError {
    pub code: i32,
    pub message: *mut c_char,
    pub line: i32,
    pub file: *mut c_char,
}

#[repr(C)]
pub struct RtlLintResult {
    pub passed: i32,
    pub warning_count: i32,
    pub error_count: i32,
    pub report: *mut c_char,
}

#[repr(C)]
pub struct RtlSynthStats {
    pub cell_count: usize,
    pub wire_count: usize,
    pub dff_count: usize,
    pub lut_count: usize,
    pub and_count: usize,
    pub or_count: usize,
    pub not_count: usize,
    pub xor_count: usize,
    pub other_count: usize,
    pub report: *mut c_char,
}

#[repr(C)]
pub struct TimingReport {
    pub area_ge: f64,
    pub delay_ns: f64,
    pub power_mw: f64,
    pub logic_depth: i32,
    pub total_gates: i32,
    pub dff_count: i32,
    pub clock_period_ns: f64,
    pub arrival_time_ns: f64,
    pub required_time_ns: f64,
    pub slack_ns: f64,
    pub timing_met: i32,
    pub report: *mut c_char,
}

#[repr(C)]
pub struct SimResult {
    pub passed: i32,
    pub exit_code: i32,
    pub time_steps: i32,
    pub output: *mut c_char,
    pub vcd_file: *mut c_char,
}

#[repr(C)]
pub struct SynthResultFFI {
    pub gate_verilog: *mut c_char,
    pub report: *mut c_char,
    pub cell_count: usize,
    pub wire_count: usize,
    pub dff_count: usize,
    pub port_count: usize,
    pub area_ge: f64,
    pub area_um2: f64,
    pub area_from_lib: i32,
    pub lib_name: *mut c_char,
    pub logic_depth: i32,
    pub success: i32,
    pub error: *mut c_char,
    pub cell_types: *mut *mut c_char,
    pub cell_type_counts: *mut usize,
    pub num_cell_types: usize,
}

#[allow(dead_code)]
extern "C" {
    pub fn rtl_error_free(err: *mut RtlError);
    pub fn rtl_design_new() -> *mut RtlDesign;
    pub fn rtl_design_free(design: *mut RtlDesign);
    pub fn rtl_parse_verilog(design: *mut RtlDesign, filename: *const c_char) -> *mut RtlError;
    pub fn rtl_parse_verilog_str(
        design: *mut RtlDesign,
        code: *const c_char,
        len: usize,
        name: *const c_char,
    ) -> *mut RtlError;
    pub fn rtl_design_module_count(design: *mut RtlDesign) -> usize;
    pub fn rtl_design_module_name(design: *mut RtlDesign, index: usize) -> *const c_char;
    pub fn rtl_lint_check(design: *mut RtlDesign, module_name: *const c_char) -> RtlLintResult;
    pub fn rtl_lint_source(source_code: *const c_char, module_name: *const c_char) -> RtlLintResult;
    pub fn rtl_lint_result_free(result: *mut RtlLintResult);
    pub fn rtl_synthesize(design: *mut RtlDesign, module_name: *const c_char) -> *mut RtlError;
    pub fn rtl_synth_stats(design: *mut RtlDesign, module_name: *const c_char) -> RtlSynthStats;
    pub fn rtl_synth_stats_free(stats: *mut RtlSynthStats);
    pub fn rtl_synth_stats_from_source(source_code: *const c_char, module_name: *const c_char) -> RtlSynthStats;
    pub fn rtl_estimate_timing(design: *mut RtlDesign, module_name: *const c_char) -> TimingReport;
    pub fn rtl_timing_report_free(report: *mut TimingReport);
    pub fn rtl_timing_analysis(
        synth_output: *const c_char,
        module_name: *const c_char,
        liberty_file: *const c_char,
        clock_period: f64,
    ) -> TimingReport;

    pub fn rtl_timing_analysis_corner(
        synth_output: *const c_char,
        module_name: *const c_char,
        liberty_file: *const c_char,
        corner_type: *const c_char,
        voltage: f64,
        temperature: f64,
        clock_period: f64,
    ) -> TimingReport;

    pub fn rtl_generate_sdc(
        module_name: *const c_char,
        clock_period: f64,
        clock_port: *const c_char,
        liberty_file: *const c_char,
    ) -> *mut c_char;

    pub fn rtl_clock_scan(
        synth_output: *const c_char,
        module_name: *const c_char,
        liberty_file: *const c_char,
        min_period: f64,
        max_period: f64,
        step: f64,
        results: *mut *mut TimingReport,
        count: *mut i32,
    ) -> i32;
    pub fn rtl_to_verilog(design: *mut RtlDesign, module_name: *const c_char) -> *mut c_char;
    pub fn rtl_design_summary(design: *mut RtlDesign) -> *mut c_char;
    pub fn rtl_engine_version() -> *const c_char;

    // Simulator
    pub fn rtl_simulate(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
        clk_port: *const c_char,
        num_cycles: i32,
        half_period_ns: f64,
    ) -> SimResult;
    pub fn rtl_simulate_with_limit(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
        clk_port: *const c_char,
        num_cycles: i32,
        half_period_ns: f64,
        memory_limit_mb: usize,
    ) -> SimResult;
    pub fn rtl_simulate_with_limit_and_timeout(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
        clk_port: *const c_char,
        num_cycles: i32,
        half_period_ns: f64,
        memory_limit_mb: usize,
        timeout_seconds: i32,
    ) -> SimResult;
    pub fn rtl_sim_result_free(result: *mut SimResult);

    // Log callback
    pub fn rtl_set_sim_log_callback(cb: extern "C" fn(*const c_char, *const c_char));
    pub fn rtl_set_synth_log_callback(cb: extern "C" fn(*const c_char, *const c_char));
    pub fn rtl_set_timing_log_callback(cb: extern "C" fn(*const c_char, *const c_char));
    pub fn rtl_set_power_log_callback(cb: extern "C" fn(*const c_char, *const c_char));
    pub fn rtl_set_formal_log_callback(cb: extern "C" fn(*const c_char, *const c_char));
    pub fn rtl_get_process_memory_mb() -> usize;
    pub fn rtl_get_cpu_cores() -> i32;
    pub fn rtl_get_cpu_threads() -> i32;
    pub fn rtl_get_total_ram_mb() -> usize;
    pub fn rtl_get_available_ram_mb() -> usize;
    pub fn rtl_get_load_1min() -> f64;
    pub fn rtl_get_cpu_model() -> *const c_char;
    pub fn rtl_get_system_info_json() -> *mut c_char;
    pub fn rtl_free_system_info(s: *mut c_char);

    // Toggle data for power analysis
    pub fn rtl_get_toggle_counts_json(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
    ) -> *mut c_char;

    // Coverage data from simulation
    pub fn rtl_get_sim_coverage_json(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
    ) -> *mut c_char;

    // Formal verification
    pub fn rtl_formal_check(
        rtl_code: *const c_char,
        gate_code: *const c_char,
        module_name: *const c_char,
    ) -> i32;

    // Real synthesis
    pub fn rtl_synthesize_real(
        rtl_code: *const c_char,
        module_name: *const c_char,
    ) -> SynthResultFFI;
    pub fn rtl_synthesize_real_with_lib(
        rtl_code: *const c_char,
        module_name: *const c_char,
        liberty_path: *const c_char,
    ) -> SynthResultFFI;
    pub fn rtl_synthesize_real_with_options(
        rtl_code: *const c_char,
        module_name: *const c_char,
        liberty_path: *const c_char,
        options: *const RtlSynthesisOptions,
    ) -> SynthResultFFI;
    pub fn rtl_synthesize_freq_optimized(
        rtl_code: *const c_char,
        module_name: *const c_char,
        liberty_path: *const c_char,
        constraint_mhz: i32,
        target_ratio: f64,
    ) -> SynthResultFFI;
    pub fn rtl_synth_result_free(result: *mut SynthResultFFI);

    // Data export: JSON serialization of results
    pub fn rtl_synth_result_to_json(result: *const SynthResultFFI) -> *mut c_char;
    pub fn rtl_timing_report_to_json(report: *const TimingReport) -> *mut c_char;

    // Liberty info parsing
    pub fn rtl_parse_liberty_info(filename: *const c_char) -> LibertyInfoFFI;
    pub fn rtl_liberty_info_free(info: *mut LibertyInfoFFI);

    // Data detection ports
    pub fn rtl_add_data_detect_port(
        design: *mut RtlDesign,
        module_name: *const c_char,
        port: *mut RtlDataDetectPort,
    ) -> i32;

    pub fn rtl_get_monitor_status(
        design: *mut RtlDesign,
        module_name: *const c_char,
    ) -> *mut c_char;

    pub fn rtl_check_anomalies(
        design: *mut RtlDesign,
        module_name: *const c_char,
    ) -> *mut c_char;

    // Real-time monitoring
    pub fn rtl_configure_monitor(
        design: *mut RtlDesign,
        module_name: *const c_char,
        config: *mut RtlMonitorConfig,
    );

    pub fn rtl_get_realtime_status(
        design: *mut RtlDesign,
        module_name: *const c_char,
    ) -> *mut c_char;

    // Auto-fix
    pub fn rtl_configure_auto_fix(
        design: *mut RtlDesign,
        module_name: *const c_char,
        config: *mut RtlAutoFixConfig,
    );

    pub fn rtl_run_auto_fix(
        design: *mut RtlDesign,
        module_name: *const c_char,
    ) -> i32;

    // Multi-threading
    pub fn rtl_simulate_mt(
        rtl_code: *const c_char,
        tb_code: *const c_char,
        module_name: *const c_char,
        clk_port: *const c_char,
        num_cycles: i32,
        half_period_ns: f64,
        num_threads: i32,
    ) -> SimResult;

    pub fn rtl_synthesize_mt(
        design: *mut RtlDesign,
        module_name: *const c_char,
        num_threads: i32,
    ) -> *mut RtlError;

    // Engine info
    pub fn rtl_engine_info() -> *mut c_char;

    // Power analysis
    pub fn rtl_power_analyze(
        gate_netlist: *const c_char,
        module_name: *const c_char,
        liberty_path: *const c_char,
        clock_freq_mhz: f64,
        toggle_data_json: *const c_char,
    ) -> PowerAnalysisResultFFI;
    pub fn rtl_power_result_free(result: *mut PowerAnalysisResultFFI);
}

#[repr(C)]
pub struct RtlDataDetectPort {
    pub signal_name: *mut c_char,
    pub port_type: *mut c_char,
    pub width: i32,
    pub detect_toggle: i32,
    pub detect_transition: i32,
    pub detect_value_change: i32,
    pub threshold: i32,
}

#[repr(C)]
pub struct RtlMonitorConfig {
    pub enable_cycle_counting: i32,
    pub enable_signal_toggles: i32,
    pub enable_anomaly_detection: i32,
    pub max_cycles: i32,
    pub timeout_seconds: f64,
}

#[repr(C)]
pub struct RtlAutoFixConfig {
    pub enable_auto_fix: i32,
    pub max_retries: i32,
    pub enable_timing_opt: i32,
    pub enable_area_opt: i32,
    pub enable_power_opt: i32,
}

#[repr(C)]
pub struct LibertyInfoFFI {
    pub library_name: *mut c_char,
    pub nom_process: f64,
    pub nom_temperature: f64,
    pub nom_voltage: f64,
    pub default_op_conditions: *mut c_char,
    pub cell_count: i32,
    pub default_leakage_power: f64,
    pub default_max_transition: f64,
}

#[repr(C)]
pub struct PowerAnalysisResultFFI {
    pub total_power_uw: f64,
    pub static_power_uw: f64,
    pub dynamic_power_uw: f64,
    pub internal_power_uw: f64,
    pub switching_power_uw: f64,
    pub clock_power_uw: f64,
    pub leakage_power_uw: f64,
    pub report: *mut c_char,
}

// Link to C standard library for free()
#[allow(dead_code)]
extern "C" {
    pub fn free(ptr: *mut std::ffi::c_void);
}
