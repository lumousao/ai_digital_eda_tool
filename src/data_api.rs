/**
 * Data Output API - Structured internal data output for LLM judgment and detail.log
 *
 * Provides:
 * - Structured metrics snapshot for LLM consumption at every flow step
 * - Detail logging to detail.log with consistent format
 * - Data consistency cross-checks between components
 * - Flow state serialization for LLM context injection
 */

use std::collections::HashMap;
use serde::{Deserialize, Serialize};

/// Comprehensive snapshot of the current flow state for LLM consumption
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FlowStateSnapshot {
    /// Flow identification
    pub module_name: String,
    pub timestamp: String,
    pub flow_step: String,
    pub step_index: usize,
    pub total_steps: usize,

    /// RTL metrics
    pub rtl_lines: usize,
    pub rtl_modules: Vec<String>,
    pub rtl_ports: usize,
    pub rtl_wires: usize,

    /// Parse/Lint state
    pub parse_errors: Vec<String>,
    pub parse_warnings: Vec<String>,
    pub lint_passed: bool,
    pub lint_warnings: usize,
    pub lint_errors: usize,

    /// Synthesis state
    pub synth_cell_count: usize,
    pub synth_dff_count: usize,
    pub synth_wire_count: usize,
    pub synth_port_count: usize,
    pub synth_area_ge: f64,
    pub synth_area_um2: f64,
    pub synth_logic_depth: i32,
    pub synth_cell_breakdown: Vec<(String, usize)>,
    pub synth_lib_name: String,
    pub synth_from_lib: bool,

    /// Timing state
    pub timing_max_freq_mhz: f64,
    pub timing_slack_ns: f64,
    pub timing_arrival_ns: f64,
    pub timing_required_ns: f64,
    pub timing_met: bool,
    pub timing_critical_path: String,

    /// Simulation state
    pub sim_passed: bool,
    pub sim_cycles: i32,
    pub sim_errors: Vec<String>,

    /// Power state
    pub power_total_mw: f64,
    pub power_static_mw: f64,
    pub power_dynamic_mw: f64,
    pub power_internal_mw: f64,
    pub power_clock_mw: f64,
    pub power_leakage_mw: f64,

    /// Formal state
    pub formal_equivalent: bool,
    pub formal_checks: usize,

    /// Design constraints (from goals)
    pub constraint_freq_mhz: f64,
    pub constraint_area_ge: Option<f64>,
    pub constraint_power_mw: Option<f64>,

    /// Iteration state
    pub iteration_count: usize,
    pub max_iterations: usize,
    pub previous_decisions: Vec<PreviousDecision>,
    pub error_history: Vec<ErrorEvent>,

    /// LLM interaction stats
    pub llm_requests: usize,
    pub llm_tokens_prompt: u64,
    pub llm_tokens_completion: u64,
    pub llm_cost_usd: f64,

    /// System metrics
    pub elapsed_ms: u128,
    pub memory_mb: u64,

    /// Cross-check summary
    pub data_consistency_checks: Vec<ConsistencyCheck>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PreviousDecision {
    pub step: String,
    pub action: String,
    pub reason: String,
    pub result: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ErrorEvent {
    pub step: String,
    pub error_type: String,
    pub message: String,
    pub resolved: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ConsistencyCheck {
    pub name: String,
    pub passed: bool,
    pub detail: String,
}

impl FlowStateSnapshot {
    pub fn new(module_name: &str, flow_step: &str, step_index: usize, total_steps: usize) -> Self {
        FlowStateSnapshot {
            module_name: module_name.to_string(),
            timestamp: chrono_str(),
            flow_step: flow_step.to_string(),
            step_index,
            total_steps,
            rtl_lines: 0,
            rtl_modules: Vec::new(),
            rtl_ports: 0,
            rtl_wires: 0,
            parse_errors: Vec::new(),
            parse_warnings: Vec::new(),
            lint_passed: true,
            lint_warnings: 0,
            lint_errors: 0,
            synth_cell_count: 0,
            synth_dff_count: 0,
            synth_wire_count: 0,
            synth_port_count: 0,
            synth_area_ge: 0.0,
            synth_area_um2: 0.0,
            synth_logic_depth: 0,
            synth_cell_breakdown: Vec::new(),
            synth_lib_name: String::new(),
            synth_from_lib: false,
            timing_max_freq_mhz: 0.0,
            timing_slack_ns: 0.0,
            timing_arrival_ns: 0.0,
            timing_required_ns: 0.0,
            timing_met: false,
            timing_critical_path: String::new(),
            sim_passed: false,
            sim_cycles: 0,
            sim_errors: Vec::new(),
            power_total_mw: 0.0,
            power_static_mw: 0.0,
            power_dynamic_mw: 0.0,
            power_internal_mw: 0.0,
            power_clock_mw: 0.0,
            power_leakage_mw: 0.0,
            formal_equivalent: false,
            formal_checks: 0,
            constraint_freq_mhz: 100.0,
            constraint_area_ge: None,
            constraint_power_mw: None,
            iteration_count: 0,
            max_iterations: 10,
            previous_decisions: Vec::new(),
            error_history: Vec::new(),
            llm_requests: 0,
            llm_tokens_prompt: 0,
            llm_tokens_completion: 0,
            llm_cost_usd: 0.0,
            elapsed_ms: 0,
            memory_mb: 0,
            data_consistency_checks: Vec::new(),
        }
    }

    /// Format as compact JSON for LLM consumption
    pub fn to_llm_context(&self) -> String {
        let mut ctx = String::from("## CURRENT FLOW STATE\n\n");

        // Flow progress
        ctx.push_str(&format!("**Flow**: {} [{}/{}]", self.flow_step, self.step_index + 1, self.total_steps));
        if !self.error_history.is_empty() {
            let unresolved = self.error_history.iter().filter(|e| !e.resolved).count();
            if unresolved > 0 {
                ctx.push_str(&format!(" | ⚠ **{} unresolved errors**", unresolved));
            }
        }
        ctx.push_str("\n\n");

        // Key metrics (compact table)
        ctx.push_str("### Key Metrics\n");
        ctx.push_str("| Metric | Value | Status |\n");
        ctx.push_str("|--------|-------|--------|\n");

        // RTL
        ctx.push_str(&format!("| RTL Lines | {} | {} |\n",
            self.rtl_lines, if self.rtl_lines > 0 { "✓" } else { "✗" }));
        // Lint
        ctx.push_str(&format!("| Lint | {} errors, {} warnings | {} |\n",
            self.lint_errors, self.lint_warnings,
            if self.lint_passed { "✓" } else { "✗" }));
        // Synthesis
        ctx.push_str(&format!("| Synthesis | {} cells, {:.0} GE, {} DFF | {} |\n",
            self.synth_cell_count, self.synth_area_ge, self.synth_dff_count,
            if self.synth_cell_count > 0 { "✓" } else { "—" }));
        // Timing
        if self.timing_max_freq_mhz > 0.0 {
            ctx.push_str(&format!("| Timing | {:.0} MHz, {:.2} ns slack | {} |\n",
                self.timing_max_freq_mhz, self.timing_slack_ns,
                if self.timing_met { "✓" } else { "✗" }));
        }
        // Power
        if self.power_total_mw > 0.0 {
            ctx.push_str(&format!("| Power | {:.3} mW total | — |\n", self.power_total_mw));
        }
        // Simulation
        ctx.push_str(&format!("| Simulation | {} cycles | {} |\n",
            self.sim_cycles, if self.sim_passed { "✓ PASS" } else { "—" }));
        // Formal
        if self.formal_checks > 0 {
            ctx.push_str(&format!("| Formal | {} checks | {} |\n",
                self.formal_checks, if self.formal_equivalent { "✓ EQUIV" } else { "✗" }));
        }

        // Constraints vs actual
        ctx.push_str("\n### Design Constraints\n");
        ctx.push_str(&format!("- Target frequency: {:.0} MHz\n", self.constraint_freq_mhz));
        if let Some(area) = self.constraint_area_ge {
            ctx.push_str(&format!("- Target area: ≤ {:.0} GE\n", area));
            ctx.push_str(&format!("- Actual area: {:.0} GE {}",
                self.synth_area_ge, if self.synth_area_ge <= area { "✓" } else { "✗ VIOLATION" }));
            ctx.push('\n');
        }
        if let Some(power) = self.constraint_power_mw {
            ctx.push_str(&format!("- Target power: ≤ {:.3} mW\n", power));
            ctx.push_str(&format!("- Actual power: {:.3} mW {}",
                self.power_total_mw, if self.power_total_mw <= power { "✓" } else { "✗ VIOLATION" }));
            ctx.push('\n');
        }

        // Errors
        if !self.error_history.is_empty() {
            ctx.push_str("\n### Error History\n");
            for (i, err) in self.error_history.iter().rev().take(5).enumerate() {
                ctx.push_str(&format!("{}. **{}** at {}: {} {}\n",
                    i + 1,
                    err.error_type,
                    err.step,
                    err.message,
                    if err.resolved { "✓" } else { "⚠ pending" }));
            }
        }

        // Previous decisions
        if !self.previous_decisions.is_empty() {
            ctx.push_str("\n### Recent Decisions\n");
            for d in self.previous_decisions.iter().rev().take(3) {
                ctx.push_str(&format!("- {}: {} → {} ({})\n",
                    d.step, d.action, d.result, d.reason));
            }
        }

        // Consistency checks
        let failed_checks: Vec<_> = self.data_consistency_checks.iter()
            .filter(|c| !c.passed).collect();
        if !failed_checks.is_empty() {
            ctx.push_str("\n### ⚠ Data Consistency Issues\n");
            for c in &failed_checks {
                ctx.push_str(&format!("- {}: {}\n", c.name, c.detail));
            }
        }

        ctx
    }

    /// Format as detail.log entry
    pub fn to_detail_log(&self) -> String {
        let mut lines = Vec::new();
        lines.push(format!("[{}] [SNAPSHOT] step={} idx={}/{} module={}",
            self.timestamp, self.flow_step, self.step_index + 1, self.total_steps, self.module_name));

        lines.push(format!("  rtl: lines={} modules={:?} ports={} wires={}",
            self.rtl_lines, self.rtl_modules, self.rtl_ports, self.rtl_wires));
        if !self.parse_errors.is_empty() {
            lines.push(format!("  parse_errors: {:?}", self.parse_errors));
        }
        lines.push(format!("  lint: passed={} errors={} warnings={}",
            self.lint_passed, self.lint_errors, self.lint_warnings));
        lines.push(format!("  synth: cells={} dff={} area_ge={:.0} area_um2={:.2} depth={} lib={}",
            self.synth_cell_count, self.synth_dff_count, self.synth_area_ge,
            self.synth_area_um2, self.synth_logic_depth,
            if !self.synth_lib_name.is_empty() { &self.synth_lib_name } else { "none" }));
        if !self.synth_cell_breakdown.is_empty() {
            let cells_str: Vec<_> = self.synth_cell_breakdown.iter()
                .map(|(t, c)| format!("{}:{}", t, c)).collect();
            lines.push(format!("  cell_breakdown: {}", cells_str.join(",")));
        }
        lines.push(format!("  timing: freq={:.0}MHz slack={:.3}ns met={} arrival={:.3}ns required={:.3}ns",
            self.timing_max_freq_mhz, self.timing_slack_ns, self.timing_met,
            self.timing_arrival_ns, self.timing_required_ns));
        lines.push(format!("  sim: passed={} cycles={} errors={}",
            self.sim_passed, self.sim_cycles, self.sim_errors.len()));
        lines.push(format!("  power: total={:.3} static={:.3} dynamic={:.3} internal={:.3} clock={:.3}",
            self.power_total_mw, self.power_static_mw, self.power_dynamic_mw,
            self.power_internal_mw, self.power_clock_mw));
        lines.push(format!("  formal: equivalent={} checks={}", self.formal_equivalent, self.formal_checks));
        lines.push(format!("  constraints: freq={:.0}MHz area={:?} power={:?}",
            self.constraint_freq_mhz, self.constraint_area_ge, self.constraint_power_mw));
        lines.push(format!("  iteration: {}/{} decisions={} errors={}",
            self.iteration_count, self.max_iterations,
            self.previous_decisions.len(), self.error_history.len()));
        lines.push(format!("  llm: requests={} prompt_tokens={} completion_tokens={} cost=${:.4}",
            self.llm_requests, self.llm_tokens_prompt, self.llm_tokens_completion, self.llm_cost_usd));
        lines.push(format!("  system: elapsed_ms={} memory_mb={}", self.elapsed_ms, self.memory_mb));

        for c in &self.data_consistency_checks {
            lines.push(format!("  consistency: {} passed={} detail={}",
                c.name, c.passed, c.detail));
        }

        lines.join("\n")
    }

    /// Run data consistency checks across the state
    pub fn verify_consistency(&mut self) {
        self.data_consistency_checks.clear();

        // Check: cell count from breakdown matches total
        if !self.synth_cell_breakdown.is_empty() && self.synth_cell_count > 0 {
            let breakdown_total: usize = self.synth_cell_breakdown.iter().map(|(_, c)| c).sum();
            let match_cells = breakdown_total == self.synth_cell_count;
            self.data_consistency_checks.push(ConsistencyCheck {
                name: "synth_cell_count".into(),
                passed: match_cells,
                detail: format!("breakdown_total={} reported={}", breakdown_total, self.synth_cell_count),
            });
        }

        // Check: DFF count from breakdown vs reported
        if !self.synth_cell_breakdown.is_empty() && self.synth_dff_count > 0 {
            let dff_from_breakdown: usize = self.synth_cell_breakdown.iter()
                .filter(|(t, _)| t.starts_with("DFF") || t.contains("dff") || t.contains("DFF"))
                .map(|(_, c)| c).sum();
            if dff_from_breakdown > 0 {
                self.data_consistency_checks.push(ConsistencyCheck {
                    name: "synth_dff_count".into(),
                    passed: dff_from_breakdown == self.synth_dff_count,
                    detail: format!("breakdown_dff={} reported_dff={}", dff_from_breakdown, self.synth_dff_count),
                });
            }
        }

        // Check: area from lib vs GE
        if self.synth_from_lib && self.synth_area_um2 > 0.0 && self.synth_area_ge > 0.0 {
            self.data_consistency_checks.push(ConsistencyCheck {
                name: "area_dual_source".into(),
                passed: true, // Both present, ratio varies by process
                detail: format!("ge={:.0} um2={:.4} ratio={:.2}",
                    self.synth_area_ge, self.synth_area_um2,
                    self.synth_area_ge / self.synth_area_um2.max(0.001)),
            });
        }

        // Check: timing consistency (if met, slack should be >= 0)
        if self.timing_max_freq_mhz > 0.0 {
            let timing_ok = !self.timing_met || self.timing_slack_ns >= -0.001;
            self.data_consistency_checks.push(ConsistencyCheck {
                name: "timing_slack_sign".into(),
                passed: timing_ok,
                detail: format!("met={} slack={:.3}ns", self.timing_met, self.timing_slack_ns),
            });
        }

        // Check: formal requires synthesis
        if self.formal_checks > 0 && self.synth_cell_count == 0 {
            self.data_consistency_checks.push(ConsistencyCheck {
                name: "formal_needs_synth".into(),
                passed: false,
                detail: "Formal checks present but no synthesis cells".into(),
            });
        }

        // Check: iteration count matches previous_decisions count
        let decision_steps: Vec<_> = self.previous_decisions.iter()
            .map(|d| d.step.clone()).collect();
        let unique_steps: Vec<_> = {
            let mut seen = Vec::new();
            for s in &decision_steps {
                if !seen.contains(s) { seen.push(s.clone()); }
            }
            seen
        };
        self.data_consistency_checks.push(ConsistencyCheck {
            name: "iteration_plausible".into(),
            passed: self.iteration_count <= self.max_iterations,
            detail: format!("iterations={}/{} unique_steps={}",
                self.iteration_count, self.max_iterations, unique_steps.len()),
        });
    }
}

/// API for building a flow snapshot incrementally
pub struct FlowSnapshotBuilder {
    pub snapshot: FlowStateSnapshot,
}

impl FlowSnapshotBuilder {
    pub fn new(module: &str, step: &str, idx: usize, total: usize) -> Self {
        FlowSnapshotBuilder {
            snapshot: FlowStateSnapshot::new(module, step, idx, total),
        }
    }

    pub fn with_rtl(mut self, lines: usize, modules: Vec<String>, ports: usize, wires: usize) -> Self {
        self.snapshot.rtl_lines = lines;
        self.snapshot.rtl_modules = modules;
        self.snapshot.rtl_ports = ports;
        self.snapshot.rtl_wires = wires;
        self
    }

    pub fn with_parse_errors(mut self, errors: Vec<String>, warnings: Vec<String>) -> Self {
        self.snapshot.parse_errors = errors;
        self.snapshot.parse_warnings = warnings;
        self
    }

    pub fn with_lint(mut self, passed: bool, errors: usize, warnings: usize) -> Self {
        self.snapshot.lint_passed = passed;
        self.snapshot.lint_errors = errors;
        self.snapshot.lint_warnings = warnings;
        self
    }

    pub fn with_synthesis(mut self, cells: usize, dff: usize, wires: usize, ports: usize,
                          area_ge: f64, area_um2: f64, logic_depth: i32,
                          cell_breakdown: Vec<(String, usize)>, lib_name: &str, from_lib: bool) -> Self {
        self.snapshot.synth_cell_count = cells;
        self.snapshot.synth_dff_count = dff;
        self.snapshot.synth_wire_count = wires;
        self.snapshot.synth_port_count = ports;
        self.snapshot.synth_area_ge = area_ge;
        self.snapshot.synth_area_um2 = area_um2;
        self.snapshot.synth_logic_depth = logic_depth;
        self.snapshot.synth_cell_breakdown = cell_breakdown;
        self.snapshot.synth_lib_name = lib_name.to_string();
        self.snapshot.synth_from_lib = from_lib;
        self
    }

    pub fn with_timing(mut self, max_freq_mhz: f64, slack_ns: f64, arrival_ns: f64,
                       required_ns: f64, met: bool, critical_path: &str) -> Self {
        self.snapshot.timing_max_freq_mhz = max_freq_mhz;
        self.snapshot.timing_slack_ns = slack_ns;
        self.snapshot.timing_arrival_ns = arrival_ns;
        self.snapshot.timing_required_ns = required_ns;
        self.snapshot.timing_met = met;
        self.snapshot.timing_critical_path = critical_path.to_string();
        self
    }

    pub fn with_simulation(mut self, passed: bool, cycles: i32, errors: Vec<String>) -> Self {
        self.snapshot.sim_passed = passed;
        self.snapshot.sim_cycles = cycles;
        self.snapshot.sim_errors = errors;
        self
    }

    pub fn with_power(mut self, total: f64, static_p: f64, dynamic: f64, internal: f64, clock: f64, leakage: f64) -> Self {
        self.snapshot.power_total_mw = total;
        self.snapshot.power_static_mw = static_p;
        self.snapshot.power_dynamic_mw = dynamic;
        self.snapshot.power_internal_mw = internal;
        self.snapshot.power_clock_mw = clock;
        self.snapshot.power_leakage_mw = leakage;
        self
    }

    pub fn with_formal(mut self, equivalent: bool, checks: usize) -> Self {
        self.snapshot.formal_equivalent = equivalent;
        self.snapshot.formal_checks = checks;
        self
    }

    pub fn with_constraints(mut self, freq_mhz: f64, area_ge: Option<f64>, power_mw: Option<f64>) -> Self {
        self.snapshot.constraint_freq_mhz = freq_mhz;
        self.snapshot.constraint_area_ge = area_ge;
        self.snapshot.constraint_power_mw = power_mw;
        self
    }

    pub fn with_iteration(mut self, count: usize, max: usize) -> Self {
        self.snapshot.iteration_count = count;
        self.snapshot.max_iterations = max;
        self
    }

    pub fn with_llm_stats(mut self, requests: usize, prompt_tokens: u64, completion_tokens: u64, cost_usd: f64) -> Self {
        self.snapshot.llm_requests = requests;
        self.snapshot.llm_tokens_prompt = prompt_tokens;
        self.snapshot.llm_tokens_completion = completion_tokens;
        self.snapshot.llm_cost_usd = cost_usd;
        self
    }

    pub fn with_system(mut self, elapsed_ms: u128, memory_mb: u64) -> Self {
        self.snapshot.elapsed_ms = elapsed_ms;
        self.snapshot.memory_mb = memory_mb;
        self
    }

    pub fn add_decision(mut self, step: &str, action: &str, reason: &str, result: &str) -> Self {
        self.snapshot.previous_decisions.push(PreviousDecision {
            step: step.to_string(),
            action: action.to_string(),
            reason: reason.to_string(),
            result: result.to_string(),
        });
        self
    }

    pub fn add_error(mut self, step: &str, error_type: &str, message: &str, resolved: bool) -> Self {
        self.snapshot.error_history.push(ErrorEvent {
            step: step.to_string(),
            error_type: error_type.to_string(),
            message: message.to_string(),
            resolved,
        });
        self
    }

    pub fn build(mut self) -> FlowStateSnapshot {
        self.snapshot.verify_consistency();
        self.snapshot
    }
}

/// Build a prompt for the LLM to make a flow decision
/// This is the core API that integrates LLM judgment into every step
pub fn build_llm_decision_prompt(snapshot: &FlowStateSnapshot) -> String {
    let mut prompt = String::new();
    prompt.push_str("Decide the next EDA flow action from the current step snapshot.\n");
    prompt.push_str("Return JSON only: {\"a\":\"p|r|b|o|x\",\"t\":\"step|\",\"r\":\"short_code\",\"k\":\"short_hint|\"}\n");
    prompt.push_str("Rules: p=proceed, r=retry current step, b=back to target step, o=optimize, x=abort.\n");
    prompt.push_str("Use short snake_case codes only. No prose. No markdown.\n\n");
    prompt.push_str(&snapshot.to_llm_context());
    prompt
}

/// Parse LLM's decision response
pub fn parse_llm_decision(response: &str) -> Result<FlowDecision, String> {
    // Extract JSON from response
    let json_str = if let Some(start) = response.find('{') {
        if let Some(end) = response.rfind('}') {
            &response[start..=end]
        } else {
            return Err("No JSON object found".to_string());
        }
    } else {
        return Err("No JSON object found".to_string());
    };

    let json: serde_json::Value = serde_json::from_str(json_str)
        .map_err(|e| format!("JSON parse error: {}", e))?;

    let action = json
        .get("action")
        .and_then(|v| v.as_str())
        .or_else(|| json.get("a").and_then(|v| v.as_str()))
        .unwrap_or("proceed")
        .to_string();
    let target = json
        .get("target")
        .and_then(|v| v.as_str())
        .or_else(|| json.get("t").and_then(|v| v.as_str()))
        .unwrap_or("")
        .to_string();
    let reason = json
        .get("reason")
        .and_then(|v| v.as_str())
        .or_else(|| json.get("r").and_then(|v| v.as_str()))
        .unwrap_or("")
        .to_string();
    let suggestions = json
        .get("suggestions")
        .and_then(|v| v.as_str())
        .or_else(|| json.get("k").and_then(|v| v.as_str()))
        .map(|s| s.to_string())
        .filter(|s| !s.trim().is_empty());

    let action_enum = match action.as_str() {
        "proceed" | "p" => FlowAction::Proceed,
        "retry" | "r" => FlowAction::Retry,
        "back" | "b" => FlowAction::Back,
        "optimize" | "o" => FlowAction::Optimize,
        "abort" | "x" => FlowAction::Abort,
        _ => FlowAction::Proceed,
    };

    Ok(FlowDecision {
        action: action_enum,
        target,
        reason,
        suggestions,
    })
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FlowDecision {
    pub action: FlowAction,
    pub target: String,
    pub reason: String,
    pub suggestions: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum FlowAction {
    Proceed,
    Retry,
    Back,
    Optimize,
    Abort,
}

impl FlowAction {
    pub fn as_str(&self) -> &str {
        match self {
            FlowAction::Proceed => "PROCEED",
            FlowAction::Retry => "RETRY",
            FlowAction::Back => "BACK",
            FlowAction::Optimize => "OPTIMIZE",
            FlowAction::Abort => "ABORT",
        }
    }
}

/// Incremental detail log writer that also produces LLM-ready context
pub struct DataApi {
    pub snapshots: Vec<FlowStateSnapshot>,
    pub decisions: Vec<FlowDecision>,
}

impl DataApi {
    pub fn new() -> Self {
        DataApi {
            snapshots: Vec::new(),
            decisions: Vec::new(),
        }
    }

    /// Add a state snapshot
    pub fn add_snapshot(&mut self, snapshot: FlowStateSnapshot) {
        self.snapshots.push(snapshot);
    }

    /// Add a flow decision
    pub fn add_decision(&mut self, decision: FlowDecision) {
        self.decisions.push(decision);
    }

    /// Build complete flow summary for final report
    pub fn build_flow_summary(&self) -> String {
        let mut summary = String::from("# Flow Execution Summary\n\n");

        // Step timeline
        summary.push_str("## Step Timeline\n\n");
        summary.push_str("| Step | Outcome | Key Metrics |\n");
        summary.push_str("|------|---------|-------------|\n");
        for s in &self.snapshots {
            let outcome = if s.parse_errors.is_empty() && s.lint_passed && s.synth_cell_count > 0 {
                "✓"
            } else if !s.error_history.is_empty() {
                "⚠"
            } else {
                "—"
            };
            let metrics = format!("{} lines, {} cells, {:.0} MHz",
                s.rtl_lines, s.synth_cell_count, s.timing_max_freq_mhz);
            summary.push_str(&format!("| {} | {} | {} |\n", s.flow_step, outcome, metrics));
        }
        summary.push('\n');

        // Decision log
        summary.push_str("## Decision Log\n\n");
        for d in &self.decisions {
            summary.push_str(&format!("- **{}** → {}: {}\n", d.action.as_str(), d.target, d.reason));
            if let Some(ref sug) = d.suggestions {
                summary.push_str(&format!("  - Suggestion: {}\n", sug));
            }
        }
        summary.push('\n');

        // Final state
        if let Some(last) = self.snapshots.last() {
            summary.push_str("## Final State\n\n");
            summary.push_str(&format!("- Module: {}\n", last.module_name));
            summary.push_str(&format!("- Cells: {} ({} DFF)\n", last.synth_cell_count, last.synth_dff_count));
            summary.push_str(&format!("- Area: {:.0} GE", last.synth_area_ge));
            if last.synth_from_lib {
                summary.push_str(&format!(" ({:.4} µm²)", last.synth_area_um2));
            }
            summary.push('\n');
            if last.timing_max_freq_mhz > 0.0 {
                summary.push_str(&format!("- Max Frequency: {:.0} MHz (slack: {:.3} ns)\n",
                    last.timing_max_freq_mhz, last.timing_slack_ns));
            }
            if last.power_total_mw > 0.0 {
                summary.push_str(&format!("- Power: {:.3} mW (static: {:.3}, dynamic: {:.3})\n",
                    last.power_total_mw, last.power_static_mw, last.power_dynamic_mw));
            }
            if !last.data_consistency_checks.is_empty() {
                summary.push_str(&format!("- Consistency checks: {}/{} passed\n",
                    last.data_consistency_checks.iter().filter(|c| c.passed).count(),
                    last.data_consistency_checks.len()));
            }
        }

        summary
    }
}

/// Get current timestamp as formatted string
fn chrono_str() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let d = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    let secs = d.as_secs();
    // Basic UTC datetime formatting
    let days_since_epoch = secs / 86400;
    let time_of_day = secs % 86400;
    let hours = time_of_day / 3600;
    let minutes = (time_of_day % 3600) / 60;
    let seconds = time_of_day % 60;

    // Calculate year/month/day from known epoch (1970-01-01)
    let mut y = 1970i64;
    let mut remaining_days = days_since_epoch as i64;
    loop {
        let days_in_year = if is_leap(y) { 366 } else { 365 };
        if remaining_days < days_in_year { break; }
        remaining_days -= days_in_year;
        y += 1;
    }
    let months_days = if is_leap(y) {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut m = 0usize;
    for (i, &md) in months_days.iter().enumerate() {
        if remaining_days < md as i64 { m = i + 1; break; }
        remaining_days -= md as i64;
        m = i + 1;
    }
    let d = remaining_days + 1;
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, hours, minutes, seconds)
}

fn is_leap(y: i64) -> bool {
    (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_snapshot_builder() {
        let snapshot = FlowSnapshotBuilder::new("counter", "synthesize", 3, 8)
            .with_rtl(45, vec!["counter".into()], 4, 12)
            .with_synthesis(15, 4, 8, 4, 350.0, 0.0, 3,
                vec![("DFF".into(), 4), ("AND".into(), 8), ("OR".into(), 3)], "", false)
            .with_timing(250.0, 0.5, 3.2, 3.7, true, "reg0/Q → gate1/A")
            .with_constraints(200.0, None, None)
            .build();

        assert_eq!(snapshot.module_name, "counter");
        assert_eq!(snapshot.synth_cell_count, 15);
        assert_eq!(snapshot.timing_max_freq_mhz, 250.0);
        assert_eq!(snapshot.timing_met, true);
        assert!(snapshot.data_consistency_checks.len() >= 1);
    }

    #[test]
    fn test_consistency_checks() {
        let mut snapshot = FlowSnapshotBuilder::new("test", "lint", 1, 5)
            .with_rtl(50, vec!["test".into()], 3, 10)
            .build();

        // Test consistency check on cell count
        snapshot.synth_cell_count = 15;
        snapshot.synth_cell_breakdown = vec![
            ("DFF".into(), 4),
            ("AND".into(), 8),
            ("OR".into(), 3),
        ];
        snapshot.verify_consistency();

        // Should have at least the cell_count check
        let cell_check = snapshot.data_consistency_checks.iter()
            .find(|c| c.name == "synth_cell_count");
        assert!(cell_check.is_some());
        assert!(cell_check.unwrap().passed);
    }

    #[test]
    fn test_llm_context_format() {
        let snapshot = FlowSnapshotBuilder::new("alu", "synthesize", 4, 8)
            .with_rtl(120, vec!["alu".into()], 6, 45)
            .with_synthesis(184, 0, 45, 6, 780.0, 0.0, 14,
                vec![("AND".into(), 100), ("OR".into(), 60), ("NOT".into(), 24)],
                "", false)
            .with_timing(150.0, 0.2, 5.5, 5.7, true, "a[3] → result[7]")
            .with_constraints(100.0, None, None)
            .add_decision("parse", "proceed", "syntax ok", "parse passed")
            .build();

        let ctx = snapshot.to_llm_context();
        assert!(ctx.contains("alu"));
        assert!(ctx.contains("184 cells"));
        assert!(ctx.contains("150 MHz"));
        assert!(ctx.contains("parse passed"));
    }

    #[test]
    fn test_parse_decision() {
        let resp = r#"{"action": "retry", "target": "synthesize", "reason": "timing violation - negative slack", "suggestions": "Add pipeline stage to reduce logic depth"}"#;
        let decision = parse_llm_decision(resp).unwrap();
        assert_eq!(decision.action, FlowAction::Retry);
        assert_eq!(decision.target, "synthesize");
        assert!(decision.suggestions.is_some());
    }

    #[test]
    fn test_parse_proceed_decision() {
        let resp = r#"{"action": "proceed", "target": "timing", "reason": "synthesis successful"}"#;
        let decision = parse_llm_decision(resp).unwrap();
        assert_eq!(decision.action, FlowAction::Proceed);
        assert_eq!(decision.target, "timing");
    }
}
