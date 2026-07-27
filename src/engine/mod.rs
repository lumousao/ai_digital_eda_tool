/**
 * Engine module - FFI bindings to the C++ RTL engine
 */

pub mod ffi;

use ffi::*;
use std::sync::{Mutex, MutexGuard, OnceLock};

static ENGINE_FFI_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

fn engine_ffi_lock() -> MutexGuard<'static, ()> {
    ENGINE_FFI_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

pub struct SynthStats {
    pub cell_count: usize,
    pub wire_count: usize,
    pub dff_count: usize,
    pub lut_count: usize,
    pub and_count: usize,
    pub or_count: usize,
    pub not_count: usize,
    pub xor_count: usize,
    #[allow(dead_code)]
    pub other_count: usize,
    pub report: String,
}

#[allow(dead_code)]
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
    pub timing_met: bool,
    pub report: String,
}

/// Safe wrapper around the RTL design
#[allow(dead_code)]
pub struct Design {
    ptr: *mut RtlDesign,
}

unsafe impl Send for Design {}
unsafe impl Sync for Design {}

#[allow(dead_code)]
impl Design {
    /// Create a new empty design
    pub fn new() -> Self {
        unsafe {
            Design {
                ptr: rtl_design_new(),
            }
        }
    }

    /// Parse a Verilog file
    pub fn parse_file(&self, filename: &str) -> Result<(), String> {
        let c_name = std::ffi::CString::new(filename).unwrap();
        unsafe {
            let err = rtl_parse_verilog(self.ptr, c_name.as_ptr());
            if err.is_null() {
                Ok(())
            } else {
                let msg = cstr_to_string((*err).message);
                let code = (*err).code;
                rtl_error_free(err);
                Err(format!("Parse error (code {}): {}", code, msg))
            }
        }
    }

    /// Parse Verilog from a string
    pub fn parse_str(&self, code: &str, name: &str) -> Result<(), String> {
        // Remove null bytes and normalize line endings
        let clean_code: String = code.chars()
            .filter(|&c| c != '\0')
            .map(|c| if c == '\r' { '\n' } else { c })
            .collect();

        let c_code = match std::ffi::CString::new(clean_code.as_bytes()) {
            Ok(c) => c,
            Err(e) => return Err(format!("CString conversion failed: {}", e)),
        };
        let c_name = std::ffi::CString::new(name).unwrap();
        let code_bytes = clean_code.as_bytes();
        // Parse the code
        let _ffi_guard = engine_ffi_lock();
        unsafe {
            let err = rtl_parse_verilog_str(
                self.ptr,
                c_code.as_ptr(),
                code_bytes.len(),
                c_name.as_ptr(),
            );
            if err.is_null() {
                Ok(())
            } else {
                let msg = cstr_to_string((*err).message);
                let code = (*err).code;
                rtl_error_free(err);
                Err(format!("Parse error (code {}): {}", code, msg))
            }
        }
    }

    /// Get number of modules
    pub fn module_count(&self) -> usize {
        unsafe { rtl_design_module_count(self.ptr) }
    }

    /// Get module name by index
    pub fn module_name(&self, index: usize) -> Option<String> {
        unsafe {
            let name = rtl_design_module_name(self.ptr, index);
            if name.is_null() {
                None
            } else {
                Some(cstr_to_string(name))
            }
        }
    }

    /// Run lint check on a module
    pub fn lint_check(&self, module_name: &str) -> LintResult {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let result = rtl_lint_check(self.ptr, c_name.as_ptr());
            let report = cstr_to_string(result.report);
            let out = LintResult {
                passed: result.passed != 0,
                warning_count: result.warning_count,
                error_count: result.error_count,
                report,
            };
            rtl_lint_result_free(&result as *const _ as *mut _);
            out
        }
    }

    /// Run source-level lint check on Verilog source code
    pub fn lint_source(source_code: &str, module_name: &str) -> LintResult {
        let c_code = std::ffi::CString::new(source_code).unwrap_or_default();
        let c_name = std::ffi::CString::new(module_name).unwrap_or_default();
        unsafe {
            let result = ffi::rtl_lint_source(c_code.as_ptr(), c_name.as_ptr());
            let report = cstr_to_string(result.report);
            let out = LintResult {
                passed: result.passed != 0,
                warning_count: result.warning_count,
                error_count: result.error_count,
                report,
            };
            rtl_lint_result_free(&result as *const _ as *mut _);
            out
        }
    }

    /// Run synthesis on a module
    pub fn synthesize(&self, module_name: &str) -> Result<(), String> {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let err = rtl_synthesize(self.ptr, c_name.as_ptr());
            if err.is_null() {
                Ok(())
            } else {
                let msg = cstr_to_string((*err).message);
                rtl_error_free(err);
                Err(msg)
            }
        }
    }

    /// Get synthesis statistics
    pub fn synth_stats(&self, module_name: &str) -> SynthStats {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let stats = rtl_synth_stats(self.ptr, c_name.as_ptr());
            let report = cstr_to_string(stats.report);
            let out = SynthStats {
                cell_count: stats.cell_count,
                wire_count: stats.wire_count,
                dff_count: stats.dff_count,
                lut_count: stats.lut_count,
                and_count: stats.and_count,
                or_count: stats.or_count,
                not_count: stats.not_count,
                xor_count: stats.xor_count,
                other_count: stats.other_count,
                report,
            };
            rtl_synth_stats_free(&stats as *const _ as *mut _);
            out
        }
    }

    /// Get synthesis stats directly from Verilog source code (bypasses C++ parser limitations)
    pub fn synth_stats_from_source(source_code: &str, module_name: &str) -> SynthStats {
        let c_code = match std::ffi::CString::new(source_code.as_bytes()) {
            Ok(c) => c,
            Err(_) => return SynthStats { cell_count: 0, wire_count: 0, dff_count: 0,
                lut_count: 0, and_count: 0, or_count: 0, not_count: 0, xor_count: 0,
                other_count: 0, report: String::new() },
        };
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let stats = rtl_synth_stats_from_source(c_code.as_ptr(), c_name.as_ptr());
            let report = cstr_to_string(stats.report);
            let out = SynthStats {
                cell_count: stats.cell_count,
                wire_count: stats.wire_count,
                dff_count: stats.dff_count,
                lut_count: stats.lut_count,
                and_count: stats.and_count,
                or_count: stats.or_count,
                not_count: stats.not_count,
                xor_count: stats.xor_count,
                other_count: stats.other_count,
                report,
            };
            rtl_synth_stats_free(&stats as *const _ as *mut _);
            out
        }
    }

    /// Run timing estimation
    pub fn estimate_timing(&self, module_name: &str) -> TimingReport {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let report = rtl_estimate_timing(self.ptr, c_name.as_ptr());
            let report_text = cstr_to_string(report.report);
            let out = TimingReport {
                area_ge: report.area_ge,
                delay_ns: report.delay_ns,
                power_mw: report.power_mw,
                logic_depth: report.logic_depth,
                total_gates: report.total_gates,
                dff_count: report.dff_count,
                clock_period_ns: report.clock_period_ns,
                arrival_time_ns: report.arrival_time_ns,
                required_time_ns: report.required_time_ns,
                slack_ns: report.slack_ns,
                timing_met: report.timing_met != 0,
                report: report_text,
            };
            rtl_timing_report_free(&report as *const _ as *mut _);
            out
        }
    }

    /// Run detailed timing analysis using Liberty library
    pub fn timing_analysis(&self, synth_output: &str, module_name: &str,
                           liberty_file: Option<&str>, clock_period: f64) -> TimingReport {
        let c_output = std::ffi::CString::new(synth_output).unwrap();
        let c_name = std::ffi::CString::new(module_name).unwrap();
        let c_lib = liberty_file.map(|s| std::ffi::CString::new(s).unwrap());
        let c_lib_ptr = c_lib.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());

        let _ffi_guard = engine_ffi_lock();
        unsafe {
            let report = rtl_timing_analysis(c_output.as_ptr(), c_name.as_ptr(), c_lib_ptr, clock_period);
            let report_text = cstr_to_string(report.report);
            let out = TimingReport {
                area_ge: report.area_ge,
                delay_ns: report.delay_ns,
                power_mw: report.power_mw,
                logic_depth: report.logic_depth,
                total_gates: report.total_gates,
                dff_count: report.dff_count,
                clock_period_ns: report.clock_period_ns,
                arrival_time_ns: report.arrival_time_ns,
                required_time_ns: report.required_time_ns,
                slack_ns: report.slack_ns,
                timing_met: report.timing_met != 0,
                report: report_text,
            };
            rtl_timing_report_free(&report as *const _ as *mut _);
            out
        }
    }

    /// Run per-corner timing analysis from the corner's Liberty tables.
    pub fn timing_analysis_corner(&self, synth_output: &str, module_name: &str,
        liberty_file: &str, corner_type: &str, voltage: f64,
        temperature: f64, clock_period: f64) -> TimingReport {
        let c_output = std::ffi::CString::new(synth_output).unwrap();
        let c_name = std::ffi::CString::new(module_name).unwrap();
        let c_lib = std::ffi::CString::new(liberty_file).unwrap();
        let c_corner = std::ffi::CString::new(corner_type).unwrap();

        unsafe {
            let report = rtl_timing_analysis_corner(
                c_output.as_ptr(), c_name.as_ptr(),
                c_lib.as_ptr(), c_corner.as_ptr(), voltage, temperature, clock_period);
            let report_text = cstr_to_string(report.report);
            let out = TimingReport {
                area_ge: report.area_ge,
                delay_ns: report.delay_ns,
                power_mw: report.power_mw,
                logic_depth: report.logic_depth,
                total_gates: report.total_gates,
                dff_count: report.dff_count,
                clock_period_ns: report.clock_period_ns,
                arrival_time_ns: report.arrival_time_ns,
                required_time_ns: report.required_time_ns,
                slack_ns: report.slack_ns,
                timing_met: report.timing_met != 0,
                report: report_text,
            };
            rtl_timing_report_free(&report as *const _ as *mut _);
            out
        }
    }

    /// Generate SDC constraint file
    pub fn generate_sdc(&self, module_name: &str, clock_period: f64,
                         clock_port: &str, liberty_file: Option<&str>) -> String {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        let c_port = std::ffi::CString::new(clock_port).unwrap();
        let c_lib = liberty_file.map(|s| std::ffi::CString::new(s).unwrap());
        let c_lib_ptr = c_lib.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());

        unsafe {
            let ptr = rtl_generate_sdc(c_name.as_ptr(), clock_period, c_port.as_ptr(), c_lib_ptr);
            let result = cstr_to_string(ptr);
            libc::free(ptr as *mut libc::c_void);
            result
        }
    }

    /// Run clock frequency scan
    pub fn clock_scan(&self, synth_output: &str, module_name: &str,
                       liberty_file: Option<&str>,
                       min_period: f64, max_period: f64, step: f64) -> Vec<TimingReport> {
        let c_output = std::ffi::CString::new(synth_output).unwrap();
        let c_name = std::ffi::CString::new(module_name).unwrap();
        let c_lib = liberty_file.map(|s| std::ffi::CString::new(s).unwrap());
        let c_lib_ptr = c_lib.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());

        unsafe {
            let mut results_ptr: *mut ffi::TimingReport = std::ptr::null_mut();
            let mut count: i32 = 0;

            rtl_clock_scan(c_output.as_ptr(), c_name.as_ptr(), c_lib_ptr,
                           min_period, max_period, step,
                           &mut results_ptr, &mut count);

            let mut reports = Vec::new();
            for i in 0..count as usize {
                let report = &*results_ptr.add(i);
                let report_text = cstr_to_string(report.report);
                reports.push(TimingReport {
                    area_ge: report.area_ge,
                    delay_ns: report.delay_ns,
                    power_mw: report.power_mw,
                    logic_depth: report.logic_depth,
                    total_gates: report.total_gates,
                    dff_count: report.dff_count,
                    clock_period_ns: report.clock_period_ns,
                    arrival_time_ns: report.arrival_time_ns,
                    required_time_ns: report.required_time_ns,
                    slack_ns: report.slack_ns,
                    timing_met: report.timing_met != 0,
                    report: report_text,
                });
                libc::free(report.report as *mut libc::c_void);
            }
            libc::free(results_ptr as *mut libc::c_void);

            reports
        }
    }

    /// Export synthesized module as Verilog
    pub fn to_verilog(&self, module_name: &str) -> String {
        let c_name = std::ffi::CString::new(module_name).unwrap();
        unsafe {
            let ptr = rtl_to_verilog(self.ptr, c_name.as_ptr());
            let result = cstr_to_string(ptr);
            libc::free(ptr as *mut libc::c_void);
            result
        }
    }

    /// Get design summary as JSON
    pub fn summary(&self) -> String {
        unsafe {
            let ptr = rtl_design_summary(self.ptr);
            let result = cstr_to_string(ptr);
            libc::free(ptr as *mut libc::c_void);
            result
        }
    }
}

impl Drop for Design {
    fn drop(&mut self) {
        unsafe {
            rtl_design_free(self.ptr);
        }
    }
}

/// Lint check result
#[derive(Debug)]
pub struct LintResult {
    pub passed: bool,
    pub warning_count: i32,
    pub error_count: i32,
    pub report: String,
}

/// Simulation result
#[derive(Debug)]
pub struct SimResult {
    pub passed: bool,
    #[allow(dead_code)]
    pub exit_code: i32,
    pub time_steps: i32,
    pub output: String,
    pub vcd_file: String,
}

/// Get engine version
pub fn version() -> String {
    unsafe { cstr_to_string(rtl_engine_version()) }
}

/// Run simulation using built-in simulator
#[allow(dead_code)]
pub fn simulate(rtl_code: &str, tb_code: &str, module_name: &str,
                clk_port: &str, num_cycles: i32, half_period_ns: f64) -> SimResult {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_tb = std::ffi::CString::new(tb_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_clk = std::ffi::CString::new(clk_port).unwrap();

    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let result = rtl_simulate(
            c_rtl.as_ptr(), c_tb.as_ptr(), c_mod.as_ptr(), c_clk.as_ptr(),
            num_cycles, half_period_ns,
        );

        let out = SimResult {
            passed: result.passed != 0,
            exit_code: result.exit_code,
            time_steps: result.time_steps,
            output: cstr_to_string(result.output),
            vcd_file: cstr_to_string(result.vcd_file),
        };

        rtl_sim_result_free(&result as *const _ as *mut _);
        out
    }
}

/// Run formal equivalence check.
///
/// `Ok(true)` and `Ok(false)` are explicit engine verdicts. `Err` means the
/// comparison could not establish either result and must never be treated as
/// equivalence.
pub fn formal_check(rtl_code: &str, gate_code: &str, module_name: &str) -> Result<bool, String> {
    let c_rtl = std::ffi::CString::new(rtl_code)
        .map_err(|_| "RTL source contains an embedded NUL byte".to_string())?;
    let c_gate = std::ffi::CString::new(gate_code)
        .map_err(|_| "gate-level source contains an embedded NUL byte".to_string())?;
    let c_mod = std::ffi::CString::new(module_name)
        .map_err(|_| "module name contains an embedded NUL byte".to_string())?;

    let _ffi_guard = engine_ffi_lock();
    match unsafe { rtl_formal_check(c_rtl.as_ptr(), c_gate.as_ptr(), c_mod.as_ptr()) } {
        1 => Ok(true),
        0 => Ok(false),
        status => Err(format!(
            "formal equivalence check was inconclusive (engine status {})",
            status
        )),
    }
}

/// Run simulation with memory limit
pub fn simulate_with_limit(rtl_code: &str, tb_code: &str, module_name: &str,
                           clk_port: &str, num_cycles: i32, half_period_ns: f64,
                           memory_limit_mb: usize) -> SimResult {
    // Default 30 second timeout
    simulate_with_limit_and_timeout(rtl_code, tb_code, module_name, clk_port,
                                    num_cycles, half_period_ns, memory_limit_mb, 30)
}

/// Run simulation with memory limit and timeout
pub fn simulate_with_limit_and_timeout(rtl_code: &str, tb_code: &str, module_name: &str,
                                       clk_port: &str, num_cycles: i32, half_period_ns: f64,
                                       memory_limit_mb: usize, timeout_seconds: i32) -> SimResult {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_tb = std::ffi::CString::new(tb_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_clk = std::ffi::CString::new(clk_port).unwrap();

    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let result = rtl_simulate_with_limit_and_timeout(
            c_rtl.as_ptr(), c_tb.as_ptr(), c_mod.as_ptr(), c_clk.as_ptr(),
            num_cycles, half_period_ns, memory_limit_mb, timeout_seconds,
        );

        let out = SimResult {
            passed: result.passed != 0,
            exit_code: result.exit_code,
            time_steps: result.time_steps,
            output: cstr_to_string(result.output),
            vcd_file: cstr_to_string(result.vcd_file),
        };

        rtl_sim_result_free(&result as *const _ as *mut _);
        out
    }
}

/// Get available system memory in MB
pub fn get_available_memory_mb() -> u64 {
    if let Ok(meminfo) = std::fs::read_to_string("/proc/meminfo") {
        for line in meminfo.lines() {
            if line.starts_with("MemAvailable:") {
                let parts: Vec<&str> = line.split_whitespace().collect();
                if parts.len() >= 2 {
                    if let Ok(kb) = parts[1].parse::<u64>() {
                        return kb / 1024;
                    }
                }
            }
        }
    }
    4096 // default 4GB
}

/// Get current process memory usage in MB
#[allow(dead_code)]
pub fn get_process_memory_mb() -> u64 {
    unsafe { rtl_get_process_memory_mb() as u64 }
}

/// Get system information as a struct
pub struct SystemInfo {
    pub cpu_cores: i32,
    pub cpu_threads: i32,
    pub cpu_model: String,
    pub total_ram_mb: u64,
    pub available_ram_mb: u64,
    pub process_rss_mb: u64,
    pub load_1min: f64,
}

pub fn get_system_info() -> SystemInfo {
    unsafe {
        let json_ptr = rtl_get_system_info_json();
        let json_str = std::ffi::CStr::from_ptr(json_ptr).to_string_lossy().to_string();
        rtl_free_system_info(json_ptr);
        // Parse JSON
        let v: serde_json::Value = serde_json::from_str(&json_str).unwrap_or_default();
        SystemInfo {
            cpu_cores: v["cpu_cores"].as_i64().unwrap_or(1) as i32,
            cpu_threads: v["cpu_threads"].as_i64().unwrap_or(1) as i32,
            cpu_model: v["cpu_model"].as_str().unwrap_or("unknown").to_string(),
            total_ram_mb: v["total_ram_mb"].as_u64().unwrap_or(0),
            available_ram_mb: v["available_ram_mb"].as_u64().unwrap_or(0),
            process_rss_mb: v["process_rss_mb"].as_u64().unwrap_or(0),
            load_1min: v["load_1min"].as_f64().unwrap_or(0.0),
        }
    }
}

pub fn get_cpu_cores() -> i32 { unsafe { rtl_get_cpu_cores() } }
pub fn get_cpu_threads() -> i32 { unsafe { rtl_get_cpu_threads() } }
pub fn get_available_ram_mb() -> u64 { unsafe { rtl_get_available_ram_mb() as u64 } }
pub fn get_load_1min() -> f64 { unsafe { rtl_get_load_1min() } }

/// Run simulation with multi-threading support
#[allow(dead_code)]
pub fn simulate_mt(rtl_code: &str, tb_code: &str, module_name: &str,
                   clk_port: &str, num_cycles: i32, half_period_ns: f64,
                   num_threads: i32) -> SimResult {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_tb = std::ffi::CString::new(tb_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_clk = std::ffi::CString::new(clk_port).unwrap();

    unsafe {
        let result = rtl_simulate_mt(
            c_rtl.as_ptr(), c_tb.as_ptr(), c_mod.as_ptr(), c_clk.as_ptr(),
            num_cycles, half_period_ns, num_threads,
        );

        let out = SimResult {
            passed: result.passed != 0,
            exit_code: result.exit_code,
            time_steps: result.time_steps,
            output: cstr_to_string(result.output),
            vcd_file: cstr_to_string(result.vcd_file),
        };

        rtl_sim_result_free(&result as *const _ as *mut _);
        out
    }
}

/// Run auto-fix loop: detect issues → optimize → retry
#[allow(dead_code)]
pub fn run_auto_fix(design: &Design, module_name: &str) -> bool {
    let c_mod = std::ffi::CString::new(module_name).unwrap();

    unsafe {
        rtl_run_auto_fix(design.ptr, c_mod.as_ptr()) == 0
    }
}

/// Get engine info as JSON
#[allow(dead_code)]
pub fn engine_info() -> String {
    unsafe {
        let ptr = rtl_engine_info();
        let result = cstr_to_string(ptr);
        libc::free(ptr as *mut libc::c_void);
        result
    }
}

/// Get real-time monitoring status as JSON
#[allow(dead_code)]
pub fn get_realtime_status(design: &Design, module_name: &str) -> String {
    let c_mod = std::ffi::CString::new(module_name).unwrap();

    unsafe {
        let ptr = rtl_get_realtime_status(design.ptr, c_mod.as_ptr());
        let result = cstr_to_string(ptr);
        libc::free(ptr as *mut libc::c_void);
        result
    }
}

/// Data detection port configuration
#[allow(dead_code)]
pub struct DataDetectPort {
    pub signal_name: String,
    pub port_type: String,
    pub width: i32,
    pub detect_toggle: bool,
    pub detect_transition: bool,
    pub detect_value_change: bool,
    pub threshold: i32,
}

/// Add a data detection port for monitoring
#[allow(dead_code)]
pub fn add_data_detect_port(design: &Design, module_name: &str, port: &DataDetectPort) -> Result<(), String> {
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_signal = std::ffi::CString::new(port.signal_name.clone()).unwrap();
    let c_port_type = std::ffi::CString::new(port.port_type.clone()).unwrap();

    let mut ffi_port = ffi::RtlDataDetectPort {
        signal_name: c_signal.as_ptr() as *mut i8,
        port_type: c_port_type.as_ptr() as *mut i8,
        width: port.width,
        detect_toggle: port.detect_toggle as i32,
        detect_transition: port.detect_transition as i32,
        detect_value_change: port.detect_value_change as i32,
        threshold: port.threshold,
    };

    unsafe {
        let result = rtl_add_data_detect_port(design.ptr, c_mod.as_ptr(), &mut ffi_port);
        if result == 0 {
            Ok(())
        } else {
            Err("Failed to add data detection port".to_string())
        }
    }
}

/// Helper: convert C string to Rust String
unsafe fn cstr_to_string(ptr: *const i8) -> String {
    if ptr.is_null() {
        return String::new();
    }
    std::ffi::CStr::from_ptr(ptr)
        .to_string_lossy()
        .into_owned()
}

/// Result of real logic synthesis
pub struct SynthRealResult {
    pub gate_verilog: String,
    pub report: String,
    pub cell_count: usize,
    pub wire_count: usize,
    pub dff_count: usize,
    pub port_count: usize,
    pub area_ge: f64,
    pub area_um2: f64,
    pub area_from_lib: bool,
    pub lib_name: String,
    pub logic_depth: i32,
    pub success: bool,
    pub error: String,
    pub cell_counts: Vec<(String, usize)>,
}

impl SynthRealResult {
    /// Convert synthesis result to stat output format for timing analysis
    /// Format: "cell_type count" (one per line), followed by the gate netlist.
    pub fn to_stat_output(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!("       {} cells\n", self.cell_count));
        out.push_str(&format!("       logic_depth: {}\n", self.logic_depth));
        for (cell_type, count) in &self.cell_counts {
            out.push_str(&format!("       {} {}\n", cell_type, count));
        }
        if self.gate_verilog.contains("module ") && self.gate_verilog.contains("endmodule") {
            out.push('\n');
            out.push_str(self.gate_verilog.trim());
            out.push('\n');
        }
        out
    }
}

impl Clone for SynthRealResult {
    fn clone(&self) -> Self {
        SynthRealResult {
            gate_verilog: self.gate_verilog.clone(),
            report: self.report.clone(),
            cell_count: self.cell_count,
            wire_count: self.wire_count,
            dff_count: self.dff_count,
            port_count: self.port_count,
            area_ge: self.area_ge,
            area_um2: self.area_um2,
            area_from_lib: self.area_from_lib,
            lib_name: self.lib_name.clone(),
            logic_depth: self.logic_depth,
            success: self.success,
            error: self.error.clone(),
            cell_counts: self.cell_counts.clone(),
        }
    }
}

/// Run real logic synthesis on RTL code with optional Liberty library.
/// When liberty_path is Some, uses real cell areas (µm²) from the library.
/// When None, falls back to GE estimates.
pub fn synthesize_real_with_lib(rtl_code: &str, module_name: &str, liberty_path: Option<&str>) -> SynthRealResult {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_lib = liberty_path.map(|p| std::ffi::CString::new(p).unwrap());

    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let result = if let Some(ref lib) = c_lib {
            ffi::rtl_synthesize_real_with_lib(c_rtl.as_ptr(), c_mod.as_ptr(), lib.as_ptr())
        } else {
            ffi::rtl_synthesize_real(c_rtl.as_ptr(), c_mod.as_ptr())
        };

        let mut cell_counts = Vec::new();
        if !result.cell_types.is_null() && !result.cell_type_counts.is_null() {
            for i in 0..result.num_cell_types {
                let type_name = cstr_to_string(*result.cell_types.add(i));
                let count = *result.cell_type_counts.add(i);
                cell_counts.push((type_name, count));
            }
        }

        let out = SynthRealResult {
            gate_verilog: cstr_to_string(result.gate_verilog),
            report: cstr_to_string(result.report),
            cell_count: result.cell_count,
            wire_count: result.wire_count,
            dff_count: result.dff_count,
            port_count: result.port_count,
            area_ge: result.area_ge,
            area_um2: result.area_um2,
            area_from_lib: result.area_from_lib != 0,
            lib_name: cstr_to_string(result.lib_name),
            logic_depth: result.logic_depth,
            success: result.success != 0,
            error: cstr_to_string(result.error),
            cell_counts,
        };

        ffi::rtl_synth_result_free(&result as *const _ as *mut _);
        out
    }
}

/// Run real logic synthesis on RTL code.
/// Generates a gate-level netlist with standard cell instances.
pub fn synthesize_real(rtl_code: &str, module_name: &str) -> SynthRealResult {
    synthesize_real_with_lib(rtl_code, module_name, None)
}

/// Frequency-optimized synthesis: iteratively optimizes to meet target frequency ratio
pub fn synthesize_freq_optimized(rtl_code: &str, module_name: &str, liberty_path: Option<&str>,
                                  constraint_mhz: i32, target_ratio: f64) -> SynthRealResult {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap_or_default();
    let c_mod = std::ffi::CString::new(module_name).unwrap_or_default();
    let c_lib = liberty_path.map(|p| std::ffi::CString::new(p).unwrap_or_default());

    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let lib_ptr = c_lib.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());
        let result_ffi = ffi::rtl_synthesize_freq_optimized(
            c_rtl.as_ptr(), c_mod.as_ptr(), lib_ptr,
            constraint_mhz, target_ratio
        );

        let mut cell_counts = Vec::new();
        if !result_ffi.cell_types.is_null() && !result_ffi.cell_type_counts.is_null() {
            for i in 0..result_ffi.num_cell_types {
                let type_name = cstr_to_string(*result_ffi.cell_types.add(i));
                let count = *result_ffi.cell_type_counts.add(i);
                cell_counts.push((type_name, count));
            }
        }

        let out = SynthRealResult {
            gate_verilog: cstr_to_string(result_ffi.gate_verilog),
            report: cstr_to_string(result_ffi.report),
            cell_count: result_ffi.cell_count,
            wire_count: result_ffi.wire_count,
            dff_count: result_ffi.dff_count,
            port_count: result_ffi.port_count,
            area_ge: result_ffi.area_ge,
            area_um2: result_ffi.area_um2,
            area_from_lib: result_ffi.area_from_lib != 0,
            lib_name: cstr_to_string(result_ffi.lib_name),
            logic_depth: result_ffi.logic_depth,
            success: result_ffi.success != 0,
            error: cstr_to_string(result_ffi.error),
            cell_counts,
        };

        unsafe { ffi::rtl_synth_result_free(&result_ffi as *const _ as *mut _); }
        out
    }
}

/// Liberty file header info (lightweight parse, no cell data)
pub struct LibertyInfo {
    pub library_name: String,
    pub nom_process: f64,
    pub nom_temperature: f64,
    pub nom_voltage: f64,
    pub default_op_conditions: String,
    pub cell_count: i32,
}

/// Parse liberty file header information (fast - only reads header).
/// Returns None if the file cannot be opened or parsed.
pub fn parse_liberty_info(file_path: &str) -> Option<LibertyInfo> {
    let c_path = std::ffi::CString::new(file_path).ok()?;
    unsafe {
        let info = ffi::rtl_parse_liberty_info(c_path.as_ptr());
        if info.library_name.is_null() {
            return None;
        }
        let result = LibertyInfo {
            library_name: cstr_to_string(info.library_name),
            nom_process: info.nom_process,
            nom_temperature: info.nom_temperature,
            nom_voltage: info.nom_voltage,
            default_op_conditions: cstr_to_string(info.default_op_conditions),
            cell_count: info.cell_count,
        };
        // Free the FFI struct (but not the strings, we've copied them)
        let mut info_copy = info;
        ffi::rtl_liberty_info_free(&mut info_copy as *mut _);
        Some(result)
    }
}

/// Set per-module log callbacks for engine internal data output
pub type EngineLogCallback = extern "C" fn(category: *const std::ffi::c_char, message: *const std::ffi::c_char);

pub fn set_synth_log_callback(cb: EngineLogCallback) {
    unsafe { ffi::rtl_set_synth_log_callback(cb); }
}

pub fn set_sim_log_callback(cb: EngineLogCallback) {
    unsafe { ffi::rtl_set_sim_log_callback(cb); }
}

pub fn set_timing_log_callback(cb: EngineLogCallback) {
    unsafe { ffi::rtl_set_timing_log_callback(cb); }
}

pub fn set_power_log_callback(cb: EngineLogCallback) {
    unsafe { ffi::rtl_set_power_log_callback(cb); }
}

pub fn set_formal_log_callback(cb: EngineLogCallback) {
    unsafe { ffi::rtl_set_formal_log_callback(cb); }
}

/// Get toggle counts JSON from simulation for power analysis
pub fn get_toggle_counts_json(rtl_code: &str, tb_code: &str, module_name: &str) -> String {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_tb = std::ffi::CString::new(tb_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let ptr = ffi::rtl_get_toggle_counts_json(c_rtl.as_ptr(), c_tb.as_ptr(), c_mod.as_ptr());
        let result = cstr_to_string(ptr);
        libc::free(ptr as *mut libc::c_void);
        result
    }
}

/// Power analysis result from C++ engine
pub struct PowerAnalysisResult {
    pub total_power_uw: f64,
    pub static_power_uw: f64,
    pub dynamic_power_uw: f64,
    pub internal_power_uw: f64,
    pub switching_power_uw: f64,
    pub clock_power_uw: f64,
    pub leakage_power_uw: f64,
    pub report: String,
}

/// Run power analysis on gate-level netlist using real liberty library
pub fn analyze_power(gate_netlist: &str, module_name: &str, liberty_path: &str, clock_freq_mhz: f64) -> PowerAnalysisResult {
    analyze_power_with_activity(gate_netlist, module_name, liberty_path, clock_freq_mhz, None)
}

/// Run power analysis using Liberty plus optional simulation switching activity JSON.
pub fn analyze_power_with_activity(
    gate_netlist: &str,
    module_name: &str,
    liberty_path: &str,
    clock_freq_mhz: f64,
    activity_json: Option<&str>,
) -> PowerAnalysisResult {
    let c_netlist = std::ffi::CString::new(gate_netlist).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let c_lib = std::ffi::CString::new(liberty_path).unwrap();
    let c_activity = activity_json.map(|json| std::ffi::CString::new(json).unwrap_or_default());
    let activity_ptr = c_activity
        .as_ref()
        .map(|json| json.as_ptr())
        .unwrap_or(std::ptr::null());
    unsafe {
        let r = ffi::rtl_power_analyze(
            c_netlist.as_ptr(), c_mod.as_ptr(), c_lib.as_ptr(),
            clock_freq_mhz, activity_ptr);
        let report = cstr_to_string(r.report);
        let result = PowerAnalysisResult {
            total_power_uw: r.total_power_uw,
            static_power_uw: r.static_power_uw,
            dynamic_power_uw: r.dynamic_power_uw,
            internal_power_uw: r.internal_power_uw,
            switching_power_uw: r.switching_power_uw,
            clock_power_uw: r.clock_power_uw,
            leakage_power_uw: r.leakage_power_uw,
            report,
        };
        ffi::rtl_power_result_free(&r as *const _ as *mut _);
        result
    }
}

/// Get simulation coverage JSON (toggles, branches, expressions, conditions, FSM states)
pub fn get_sim_coverage_json(rtl_code: &str, tb_code: &str, module_name: &str) -> String {
    let c_rtl = std::ffi::CString::new(rtl_code).unwrap();
    let c_tb = std::ffi::CString::new(tb_code).unwrap();
    let c_mod = std::ffi::CString::new(module_name).unwrap();
    let _ffi_guard = engine_ffi_lock();
    unsafe {
        let ptr = ffi::rtl_get_sim_coverage_json(c_rtl.as_ptr(), c_tb.as_ptr(), c_mod.as_ptr());
        let result = cstr_to_string(ptr);
        libc::free(ptr as *mut libc::c_void);
        result
    }
}

#[cfg(test)]
mod tests {
    use super::{formal_check, simulate_with_limit, synthesize_real};
    use std::collections::{BTreeSet, HashMap};
    use std::fs;

    fn parse_vcd_signal_changes(vcd: &str) -> HashMap<String, Vec<(i32, String)>> {
        let mut codes: HashMap<String, String> = HashMap::new();
        let mut changes: HashMap<String, Vec<(i32, String)>> = HashMap::new();
        let mut current_time = 0;
        let mut in_header = true;
        let mut scope_stack: Vec<String> = Vec::new();

        for raw_line in vcd.lines() {
            let line = raw_line.trim();
            if line.is_empty() {
                continue;
            }
            if in_header {
                if let Some(rest) = line.strip_prefix("$scope ") {
                    let parts: Vec<&str> = rest.split_whitespace().collect();
                    if parts.len() >= 2 {
                        scope_stack.push(parts[1].to_string());
                    }
                } else if line.starts_with("$upscope") {
                    scope_stack.pop();
                } else if line.starts_with("$var ") {
                    let parts: Vec<&str> = line.split_whitespace().collect();
                    if parts.len() >= 5 {
                        let base_name = parts[4].to_string();
                        let hier_name = if scope_stack.is_empty() {
                            base_name.clone()
                        } else {
                            format!("{}.{}", scope_stack.join("."), base_name)
                        };
                        codes.insert(parts[3].to_string(), hier_name);
                    }
                } else if line == "$enddefinitions $end" {
                    in_header = false;
                }
                continue;
            }

            if let Some(rest) = line.strip_prefix('#') {
                current_time = rest.parse::<i32>().unwrap_or(current_time);
                continue;
            }
            if line.starts_with('$') {
                continue;
            }
            if let Some(rest) = line.strip_prefix('b') {
                let mut parts = rest.split_whitespace();
                if let (Some(value), Some(code)) = (parts.next(), parts.next()) {
                    if let Some(name) = codes.get(code) {
                        changes
                            .entry(name.clone())
                            .or_default()
                            .push((current_time, value.to_string()));
                    }
                }
            }
        }

        changes
    }

    #[test]
    fn counter_vcd_preserves_delay_and_output_progression() {
        let rtl = include_str!("../../test_circuits/counter_4bit.v");
        let tb = r#"
`timescale 1ns / 1ps
module counter_4bit_tb;
    reg clk;
    reg rst_n;
    reg en;
    wire [3:0] q;

    counter_4bit uut(.clk(clk), .rst_n(rst_n), .en(en), .q(q));

    initial begin
        clk = 0;
        repeat (20) #5 clk = ~clk;
    end

    initial begin
        rst_n = 0;
        en = 0;
        #12 rst_n = 1;
        #8 en = 1;
        #40 en = 0;
        #10 $finish;
    end

    initial begin
        $dumpfile("counter_4bit_tb.vcd");
        $dumpvars(0, counter_4bit_tb);
    end
endmodule
"#;

        let result = simulate_with_limit(rtl, tb, "counter_4bit", "clk", 200, 5.0, 256);
        assert!(result.passed, "simulation failed: {}", result.output);
        assert!(!result.vcd_file.is_empty(), "missing vcd path");

        let vcd = fs::read_to_string(&result.vcd_file).expect("read vcd");
        let changes = parse_vcd_signal_changes(&vcd);
        let clk = changes.get("counter_4bit_tb.clk").expect("clk changes");
        let q = changes.get("counter_4bit_tb.q").expect("q changes");

        assert!(q.iter().any(|(_, value)| value == "0001"), "q never reached 0001: {:?}", q);
        assert!(q.iter().any(|(_, value)| value == "0010"), "q never reached 0010: {:?}", q);

        let clk_times: Vec<i32> = clk.iter().skip(1).map(|(t, _)| *t).take(6).collect();
        assert!(
            clk_times.windows(2).all(|w| (w[1] - w[0]) >= 5),
            "clock edges are too dense, delay handling is still broken: {:?}",
            clk_times
        );
    }

    #[test]
    fn counter_tb_with_posedge_and_negedge_waits_completes() {
        let rtl = r#"
module counter_4bit(
    input clk,
    input rst,
    input en,
    output reg [3:0] q
);

always @(posedge clk) begin
    if (rst)
        q <= 4'b0000;
    else if (en)
        q <= q + 4'd1;
end

endmodule
"#;
        let tb = r#"
`timescale 1ns / 1ps

module counter_4bit_tb;

    reg  clk;
    reg  rst;
    reg  en;
    wire [3:0] q;

    counter_4bit uut (
        .clk(clk),
        .rst(rst),
        .en(en),
        .q(q)
    );

    always #5 clk = ~clk;

    initial begin
        $dumpfile("counter_4bit_tb.vcd");
        $dumpvars(0, counter_4bit_tb);
    end

    initial begin
        clk = 0;
        rst = 1;
        en  = 0;

        @(posedge clk);
        @(posedge clk);

        @(negedge clk);
        if (q !== 4'b0000) begin
            $display("FAIL: Reset not working at time %0t", $time);
            $finish;
        end

        rst = 0;
        @(posedge clk);
        @(negedge clk);
        if (q !== 4'b0000) begin
            $display("FAIL: Counter changed without enable at time %0t", $time);
            $finish;
        end

        en = 1;
        repeat (4) @(posedge clk);
        en = 0;
        @(negedge clk);
        if (q !== 4'd4) begin
            $display("FAIL: Counter expected 4, got %d at time %0t", q, $time);
            $finish;
        end

        repeat (2) @(posedge clk);
        @(negedge clk);
        if (q !== 4'd4) begin
            $display("FAIL: Counter changed without enable at time %0t", $time);
            $finish;
        end

        en = 1;
        @(posedge clk);
        en = 0;
        @(negedge clk);
        if (q !== 4'd5) begin
            $display("FAIL: Counter expected 5, got %d at time %0t", q, $time);
            $finish;
        end

        rst = 1;
        @(posedge clk);
        @(negedge clk);
        if (q !== 4'b0000) begin
            $display("FAIL: Reset did not work while enabled at time %0t", $time);
            $finish;
        end

        rst = 0;
        @(posedge clk);
        @(negedge clk);
        if (q !== 4'b0000) begin
            $display("FAIL: Counter changed after reset release without enable");
            $finish;
        end

        $display("PASS: All tests completed");
        $finish;
    end

endmodule
"#;

        let result = simulate_with_limit(rtl, tb, "counter_4bit", "clk", 400, 5.0, 256);
        assert!(result.passed, "simulation failed: {}", result.output);
        assert!(
            result.output.contains("PASS: All tests completed"),
            "expected PASS banner, got: {}",
            result.output
        );
    }

    #[test]
    fn combinational_and_gate_vcd_tracks_output_changes() {
        let rtl = include_str!("../../test_circuits/and_gate.v");
        let tb = r#"
`timescale 1ns / 1ps
module and_gate_tb;
    reg a;
    reg b;
    wire y;

    and_gate uut(.a(a), .b(b), .y(y));

    initial begin
        $dumpfile("and_gate_tb.vcd");
        $dumpvars(0, and_gate_tb);
        a = 0; b = 0;
        #5 a = 1;
        #5 b = 1;
        #5 a = 0;
        #5 $finish;
    end
endmodule
"#;

        let result = simulate_with_limit(rtl, tb, "and_gate", "clk", 50, 5.0, 256);
        assert!(result.passed, "simulation failed: {}", result.output);
        let vcd = fs::read_to_string(&result.vcd_file).expect("read vcd");
        let changes = parse_vcd_signal_changes(&vcd);
        let y = changes.get("and_gate_tb.y").expect("y changes");
        assert!(y.iter().any(|(_, value)| value == "1"), "and output never became 1: {:?}", y);
        assert!(y.iter().any(|(_, value)| value == "0"), "and output never returned to 0: {:?}", y);
    }

    #[test]
    fn alu_vcd_tracks_multiple_result_values() {
        let rtl = include_str!("../../test_circuits/alu_8bit.v");
        let tb = r#"
`timescale 1ns / 1ps
module alu_8bit_tb;
    reg [7:0] a;
    reg [7:0] b;
    reg [2:0] op;
    wire [7:0] result;
    wire zero;
    wire carry;

    alu_8bit uut(.a(a), .b(b), .op(op), .result(result), .zero(zero), .carry(carry));

    initial begin
        $dumpfile("alu_8bit_tb.vcd");
        $dumpvars(0, alu_8bit_tb);
        a = 8'h03; b = 8'h02; op = 3'd0;
        #5 op = 3'd2;
        #5 op = 3'd4;
        #5 a = 8'h0f; b = 8'h01; op = 3'd6;
        #5 $finish;
    end
endmodule
"#;

        let result = simulate_with_limit(rtl, tb, "alu_8bit", "clk", 50, 5.0, 256);
        assert!(result.passed, "simulation failed: {}", result.output);
        let vcd = fs::read_to_string(&result.vcd_file).expect("read vcd");
        let changes = parse_vcd_signal_changes(&vcd);
        let result_sig = changes.get("alu_8bit_tb.result").expect("result changes");

        let mut unique = BTreeSet::new();
        for (_, value) in result_sig {
            unique.insert(value.clone());
        }
        assert!(
            unique.len() >= 3,
            "alu result did not show enough distinct waveform states: {:?}",
            unique
        );
    }

    #[test]
    fn formal_check_completes_for_counter_gate_netlist() {
        let rtl = include_str!("../../test_circuits/counter_4bit.v");
        let synth = synthesize_real(rtl, "counter_4bit");
        assert!(synth.success, "real synthesis failed: {}", synth.error);
        assert!(
            formal_check(rtl, &synth.gate_verilog, "counter_4bit")
                .expect("formal checker returned no verdict"),
            "formal equivalence check reported non-equivalent"
        );
    }

    #[test]
    fn formal_check_completes_for_unsigned_8bit_multiplier_gate_netlist() {
        let rtl = r#"
module formal_mul8(input [7:0] a, input [7:0] b, output [15:0] product);
    assign product = a * b;
endmodule
"#;
        let synth = synthesize_real(rtl, "formal_mul8");
        assert!(synth.success, "real synthesis failed: {}", synth.error);
        for pin in [".A()", ".B()", ".S()", ".Y()"] {
            assert!(
                !synth.gate_verilog.contains(pin),
                "synthesis emitted an unconnected gate pin {pin}"
            );
        }
        assert!(
            formal_check(rtl, &synth.gate_verilog, "formal_mul8")
                .expect("formal checker returned no verdict"),
            "formal equivalence check reported non-equivalent multiplier"
        );
    }

    #[test]
    fn formal_check_completes_for_procedural_8bit_multiplier() {
        let rtl = include_str!("../../workspace/default/src/mul8.v");
        let synth = synthesize_real(rtl, "mul8");
        assert!(synth.success, "real synthesis failed: {}", synth.error);
        assert!(synth.cell_count > 0, "procedural multiplier produced no cells");
        assert!(
            formal_check(rtl, &synth.gate_verilog, "mul8")
                .expect("formal checker returned no verdict"),
            "formal equivalence check reported non-equivalent procedural multiplier"
        );
    }

    #[test]
    fn formal_check_procedural_multiplier_is_name_and_width_independent() {
        let rtl = r#"
module generic_shift_mul(
    input [3:0] lhs,
    input [4:0] rhs,
    output reg [8:0] result
);
    integer index;
    reg [8:0] running_total;
    always @(*) begin
        running_total = 0;
        for (index = 0; index < 5; index = index + 1) begin
            if (rhs[index])
                running_total = running_total + (lhs << index);
        end
        result = running_total;
    end
endmodule
"#;
        let synth = synthesize_real(rtl, "generic_shift_mul");
        assert!(synth.success, "real synthesis failed: {}", synth.error);
        assert!(synth.cell_count > 0, "generic procedural multiplier produced no cells");
        assert!(
            formal_check(rtl, &synth.gate_verilog, "generic_shift_mul")
                .expect("formal checker returned no verdict"),
            "formal equivalence check reported non-equivalent generic procedural multiplier"
        );
    }

    #[test]
    fn synthesis_and_formal_cover_hierarchical_datapath() {
        let rtl = include_str!("../../test_circuits/top_hierarchy.v");
        let synth = synthesize_real(rtl, "top_hierarchy");
        assert!(synth.success, "hierarchical synthesis failed: {}", synth.error);
        assert!(synth.cell_count > 0, "hierarchical synthesis produced no cells");
        assert!(
            formal_check(rtl, &synth.gate_verilog, "top_hierarchy")
                .expect("hierarchical formal checker returned no verdict"),
            "hierarchical RTL and gate netlist are not equivalent"
        );
    }

    #[test]
    fn formal_check_rejects_functionally_modified_gate_netlist() {
        let rtl = r#"
module formal_and(input a, input b, output y);
    assign y = a & b;
endmodule
"#;
        let synth = synthesize_real(rtl, "formal_and");
        assert!(synth.success, "real synthesis failed: {}", synth.error);

        let mutated = synth
            .gate_verilog
            .replacen("AND", "OR", 1);
        assert_ne!(mutated, synth.gate_verilog, "test could not mutate an AND cell");
        assert_eq!(
            formal_check(rtl, &mutated, "formal_and")
                .expect("formal checker returned no verdict for mutated netlist"),
            false,
            "functionally modified gate netlist was incorrectly accepted"
        );
    }

    #[test]
    fn formal_check_models_clock_enable_semantics() {
        let rtl = r#"
module formal_dffe(input clk, input en, input d, output reg q);
    always @(posedge clk) begin
        if (en) q <= d;
    end
endmodule
"#;
        let gate = r#"
module formal_dffe(clk, en, d, q);
    input clk;
    input en;
    input d;
    output q;
    $_DFFE_PP_ u_dff (.C(clk), .D(d), .E(en), .Q(q));
endmodule
"#;
        assert!(
            formal_check(rtl, gate, "formal_dffe")
                .expect("formal checker returned no verdict for DFFE model"),
            "DFFE model did not preserve active-high clock-enable behavior"
        );
    }
}
