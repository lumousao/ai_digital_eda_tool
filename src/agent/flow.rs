/**
 * Flow Decision Engine - LLM-driven flow controller
 *
 * Design principle: "API is the brain" — the LLM decides all iteration, fix, and
 * optimization directions. The agent only provides tool information, suggested
 * workflows, and internal interfaces; the LLM makes all decisions.
 *
 * This replaces the hardcoded auto-fix loop and fixed step sequences with an
 * intelligent, data-driven decision process.
 */

use std::collections::HashMap;
use serde::{Deserialize, Serialize};
use crate::llm::LlmClient;
use crate::agent::{Agent, ThinkingMode, AgentContext, ProjectState};

/// Result of executing one flow step
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StepResult {
    pub step_name: String,
    pub success: bool,
    pub data: HashMap<String, String>,
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
    pub metrics: HashMap<String, f64>,
    pub raw_output: String,
    pub execution_time_ms: u64,
}

impl StepResult {
    pub fn new(name: &str) -> Self {
        StepResult {
            step_name: name.to_string(),
            success: true,
            data: HashMap::new(),
            errors: Vec::new(),
            warnings: Vec::new(),
            metrics: HashMap::new(),
            raw_output: String::new(),
            execution_time_ms: 0,
        }
    }

    pub fn with_error(mut self, err: &str) -> Self {
        self.success = false;
        self.errors.push(err.to_string());
        self
    }

    pub fn with_warning(mut self, warn: &str) -> Self {
        self.warnings.push(warn.to_string());
        self
    }

    pub fn with_metric(mut self, key: &str, val: f64) -> Self {
        self.metrics.insert(key.to_string(), val);
        self
    }

    pub fn with_data(mut self, key: &str, val: &str) -> Self {
        self.data.insert(key.to_string(), val.to_string());
        self
    }

    /// Format for LLM consumption — structured JSON
    pub fn to_llm_format(&self) -> String {
        let status = if self.success { "PASS" } else { "FAIL" };
        let mut s = format!(
            "## Step: {} [{}, {}ms]\n",
            self.step_name, status, self.execution_time_ms
        );

        if !self.errors.is_empty() {
            s.push_str("Errors:\n");
            for e in &self.errors {
                s.push_str(&format!("  - {}\n", e));
            }
        }
        if !self.warnings.is_empty() {
            s.push_str("Warnings:\n");
            for w in &self.warnings {
                s.push_str(&format!("  - {}\n", w));
            }
        }
        if !self.metrics.is_empty() {
            s.push_str("Metrics:\n");
            let mut sorted: Vec<_> = self.metrics.iter().collect();
            sorted.sort_by(|a, b| a.0.cmp(b.0));
            for (k, v) in sorted {
                s.push_str(&format!("  {}: {:.4}\n", k, v));
            }
        }
        if !self.data.is_empty() {
            s.push_str("Data:\n");
            let mut sorted: Vec<_> = self.data.iter().collect();
            sorted.sort_by(|a, b| a.0.cmp(b.0));
            for (k, v) in sorted {
                if v.len() > 200 {
                    s.push_str(&format!("  {}: {}...\n", k, &v[..200]));
                } else {
                    s.push_str(&format!("  {}: {}\n", k, v));
                }
            }
        }
        s
    }
}

/// Definition of a step that can be executed in the flow
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StepDefinition {
    pub name: String,
    pub description: String,
    /// Steps that must complete successfully before this one
    pub dependencies: Vec<String>,
    /// If this step fails, must fix before continuing to dependents
    pub is_critical: bool,
    /// Can this step be retried directly?
    pub can_retry: bool,
    /// Does retrying require code changes (from LLM)?
    pub retry_with_changes: bool,
    /// If this step fails, which upstream steps should be revisited
    pub parent_steps: Vec<String>,
    /// What this step produces (outputs used by downstream steps)
    pub outputs: Vec<String>,
    /// What this step consumes (inputs from upstream steps)
    pub inputs: Vec<String>,
}

impl StepDefinition {
    pub fn new(name: &str, description: &str) -> Self {
        StepDefinition {
            name: name.to_string(),
            description: description.to_string(),
            dependencies: Vec::new(),
            is_critical: true,
            can_retry: true,
            retry_with_changes: false,
            parent_steps: Vec::new(),
            outputs: Vec::new(),
            inputs: Vec::new(),
        }
    }

    pub fn depends_on(mut self, step: &str) -> Self {
        self.dependencies.push(step.to_string());
        self
    }

    pub fn parent_on_fail(mut self, step: &str) -> Self {
        self.parent_steps.push(step.to_string());
        self
    }

    pub fn produces(mut self, output: &str) -> Self {
        self.outputs.push(output.to_string());
        self
    }

    pub fn consumes(mut self, input: &str) -> Self {
        self.inputs.push(input.to_string());
        self
    }
}

/// LLM's decision after analyzing step results
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum FlowAction {
    /// Continue to the next step
    Continue { next_step: String },
    /// Retry the current step (with or without changes)
    Retry { step: String, reason: String, suggested_changes: Option<String> },
    /// Go back to a previous step and redo from there
    BackToStep { step: String, reason: String },
    /// Abort the flow entirely
    Abort { reason: String },
    /// Flow completed successfully
    Complete { summary: String },
}

/// Flow Decision Engine — the core of LLM-driven iteration
pub struct FlowDecisionEngine {
    /// LLM client for making decisions
    llm: LlmClient,
    /// All step results collected so far (in order)
    step_results: Vec<StepResult>,
    /// Available steps that can be executed
    available_steps: Vec<StepDefinition>,
    /// Planned step order (can be modified by LLM)
    planned_order: Vec<String>,
    /// Current step index
    current_index: usize,
    /// Maximum total iterations (safety limit)
    max_iterations: usize,
    /// Total steps executed so far
    total_executions: usize,
    /// Conversation history with LLM about flow decisions
    decision_history: Vec<String>,
    /// Module name being worked on
    module_name: String,
}

impl FlowDecisionEngine {
    pub fn new(llm: LlmClient, module_name: &str) -> Self {
        let engine = FlowDecisionEngine {
            llm,
            step_results: Vec::new(),
            available_steps: Vec::new(),
            planned_order: Vec::new(),
            current_index: 0,
            max_iterations: 50,
            total_executions: 0,
            decision_history: Vec::new(),
            module_name: module_name.to_string(),
        };
        engine
    }

    /// Register available steps
    pub fn register_steps(&mut self, steps: Vec<StepDefinition>) {
        self.available_steps = steps;
    }

    /// Set the planned execution order
    pub fn set_plan(&mut self, order: Vec<String>) {
        self.planned_order = order;
    }

    /// Build the default EDA flow steps
    pub fn build_default_flow() -> Vec<StepDefinition> {
        vec![
            StepDefinition::new("parse", "Parse Verilog RTL code into AST")
                .produces("ast")
                .depends_on(""),
            StepDefinition::new("lint", "Run lint checks on parsed design")
                .produces("lint_result")
                .depends_on("parse")
                .parent_on_fail("parse"),
            StepDefinition::new("elaborate", "Elaborate design hierarchy, resolve parameters")
                .produces("elaborated_design")
                .depends_on("parse")
                .parent_on_fail("parse"),
            StepDefinition::new("simulate", "Run behavioral simulation with testbench")
                .produces("simulation_result")
                .consumes("elaborated_design")
                .depends_on("elaborate")
                .parent_on_fail("parse"),
            StepDefinition::new("synthesize", "Synthesize RTL to gate-level netlist")
                .produces("gate_netlist")
                .consumes("elaborated_design")
                .depends_on("elaborate")
                .parent_on_fail("parse"),
            StepDefinition::new("timing", "Run static timing analysis")
                .produces("timing_report")
                .consumes("gate_netlist")
                .depends_on("synthesize")
                .parent_on_fail("synthesize"),
            StepDefinition::new("formal", "Run formal verification (equivalence check)")
                .produces("formal_result")
                .consumes("gate_netlist")
                .depends_on("synthesize")
                .parent_on_fail("synthesize"),
            StepDefinition::new("power", "Run power analysis")
                .produces("power_report")
                .consumes("gate_netlist")
                .depends_on("synthesize")
                .parent_on_fail("synthesize"),
            StepDefinition::new("area", "Compute area report")
                .produces("area_report")
                .consumes("gate_netlist")
                .depends_on("synthesize"),
            StepDefinition::new("report", "Generate final comprehensive report")
                .produces("final_report")
                .consumes("timing_report")
                .depends_on("timing"),
        ]
    }

    /// Add a step result
    pub fn add_result(&mut self, result: StepResult) {
        self.step_results.push(result);
    }

    /// Ask the LLM to decide what to do next based on all results
    pub fn decide_next(&self) -> Result<FlowAction, String> {
        if self.total_executions >= self.max_iterations {
            return Ok(FlowAction::Abort {
                reason: format!("Max iterations ({}) reached", self.max_iterations),
            });
        }

        // Check if all planned steps completed successfully
        if self.current_index >= self.planned_order.len() {
            let all_ok = self.step_results.iter().all(|r| r.success);
            if all_ok {
                return Ok(FlowAction::Complete {
                    summary: format!(
                        "All {} steps completed successfully. Total executions: {}",
                        self.step_results.len(),
                        self.total_executions
                    ),
                });
            }
        }

        // Build the decision prompt for the LLM
        let prompt = self.build_decision_prompt();
        let messages = vec![
            crate::llm::Message {
                role: "system".into(),
                content: Self::flow_decision_system_prompt(),
            },
            crate::llm::Message {
                role: "user".into(),
                content: prompt,
            },
        ];

        let (response, _) = self.llm.chat_with_usage(&messages)?;

        // Parse LLM's decision
        self.parse_llm_decision(&response)
    }

    /// Build the system prompt for flow decisions
    fn flow_decision_system_prompt() -> String {
        r#"You are an EDA flow controller. Your job is to analyze the results of each step
in an RTL design flow and decide what to do next.

Available actions:
1. CONTINUE - Move to the next planned step
2. RETRY - Retry the current step (with optional code changes)
3. BACK_TO_STEP - Go back to a previous step and redo from there
4. ABORT - Stop the flow (unrecoverable error or max iterations)

Decision criteria:
- If a step FAILS with a syntax error, go BACK to "parse" step
- If a step FAILS with a simulation mismatch, RETRY "simulate" with testbench fixes
- If synthesis produces too many cells, RETRY "synthesize" with optimization suggestions
- If timing has negative slack, go BACK to "synthesize" to try different optimization
- If formal verification finds a counterexample, RETRY with design fixes
- If a step has WARNINGS but succeeds, CONTINUE (warnings are non-critical)
- If the same step fails 3+ times with the same error, ABORT

Output format (JSON only):
{"action": "continue", "next_step": "step_name"}
{"action": "retry", "step": "step_name", "reason": "...", "suggested_changes": "..."}
{"action": "back_to_step", "step": "step_name", "reason": "..."}
{"action": "abort", "reason": "..."}
{"action": "complete", "summary": "..."}
"#.to_string()
    }

    /// Build the decision prompt with all step results
    fn build_decision_prompt(&self) -> String {
        let mut prompt = String::new();

        prompt.push_str(&format!("# EDA Flow Status for module: {}\n\n", self.module_name));

        // Planned order
        prompt.push_str("## Planned Flow Order\n");
        for (i, step) in self.planned_order.iter().enumerate() {
            let marker = if i == self.current_index { " ← CURRENT" } else { "" };
            let status = if i < self.current_index {
                let result = &self.step_results[i];
                if result.success { " ✓" } else { " ✗" }
            } else {
                ""
            };
            prompt.push_str(&format!("{}. {}{}{}\n", i + 1, step, status, marker));
        }
        prompt.push_str("\n");

        // Available steps with descriptions
        prompt.push_str("## Available Steps\n");
        for step in &self.available_steps {
            prompt.push_str(&format!(
                "- **{}**: {}\n  Dependencies: [{}]\n  On failure, revisit: [{}]\n",
                step.name,
                step.description,
                step.dependencies.join(", "),
                step.parent_steps.join(", "),
            ));
        }
        prompt.push_str("\n");

        // Step results (most important)
        prompt.push_str("## Step Results\n\n");
        if self.step_results.is_empty() {
            prompt.push_str("(No steps executed yet)\n\n");
        } else {
            for (i, result) in self.step_results.iter().enumerate() {
                prompt.push_str(&format!("### {}. {}\n", i + 1, result.to_llm_format()));
                prompt.push_str("\n");
            }
        }

        // Error history analysis
        let failures: Vec<_> = self.step_results.iter()
            .filter(|r| !r.success)
            .collect();
        if !failures.is_empty() {
            prompt.push_str("## Failure Analysis\n");
            for f in &failures {
                prompt.push_str(&format!("- {}: {}\n", f.step_name, f.errors.join("; ")));
            }

            // Count repeated failures
            let mut fail_counts: HashMap<String, usize> = HashMap::new();
            for f in &failures {
                *fail_counts.entry(f.step_name.clone()).or_insert(0) += 1;
            }
            prompt.push_str("\nFailure counts:\n");
            for (step, count) in &fail_counts {
                prompt.push_str(&format!("- {}: {} times\n", step, count));
            }
        }

        // Request decision
        prompt.push_str("\n## Decision Request\n");
        prompt.push_str("Based on the above results, decide the next action.\n");
        prompt.push_str("Output a JSON object with the action.\n");

        prompt
    }

    /// Parse LLM's JSON decision
    fn parse_llm_decision(&self, response: &str) -> Result<FlowAction, String> {
        // Extract JSON from response (may contain surrounding text)
        let json_str = if let Some(start) = response.find('{') {
            if let Some(end) = response.rfind('}') {
                &response[start..=end]
            } else {
                return Err("No JSON found in response".to_string());
            }
        } else {
            return Err("No JSON found in response".to_string());
        };

        let json: serde_json::Value = serde_json::from_str(json_str)
            .map_err(|e| format!("Failed to parse JSON: {}", e))?;

        let action = json["action"].as_str().unwrap_or("continue");

        match action {
            "continue" => {
                let next = json["next_step"].as_str().unwrap_or("");
                Ok(FlowAction::Continue {
                    next_step: if next.is_empty() {
                        // Default: next in planned order
                        if self.current_index < self.planned_order.len() {
                            self.planned_order[self.current_index].clone()
                        } else {
                            "report".to_string()
                        }
                    } else {
                        next.to_string()
                    },
                })
            }
            "retry" => {
                let step = json["step"].as_str().unwrap_or("").to_string();
                let reason = json["reason"].as_str().unwrap_or("unspecified").to_string();
                let changes = json["suggested_changes"].as_str().map(|s| s.to_string());
                Ok(FlowAction::Retry { step, reason, suggested_changes: changes })
            }
            "back_to_step" => {
                let step = json["step"].as_str().unwrap_or("parse").to_string();
                let reason = json["reason"].as_str().unwrap_or("unspecified").to_string();
                Ok(FlowAction::BackToStep { step, reason })
            }
            "abort" => {
                let reason = json["reason"].as_str().unwrap_or("unspecified").to_string();
                Ok(FlowAction::Abort { reason })
            }
            "complete" => {
                let summary = json["summary"].as_str().unwrap_or("Flow complete").to_string();
                Ok(FlowAction::Complete { summary })
            }
            _ => Err(format!("Unknown action: {}", action)),
        }
    }

    /// Get the current step name
    pub fn current_step(&self) -> Option<&str> {
        if self.current_index < self.planned_order.len() {
            Some(&self.planned_order[self.current_index])
        } else {
            None
        }
    }

    /// Advance to the next step
    pub fn advance(&mut self) {
        self.current_index += 1;
        self.total_executions += 1;
    }

    /// Go back to a specific step
    pub fn go_back_to(&mut self, step_name: &str) {
        if let Some(pos) = self.planned_order.iter().position(|s| s == step_name) {
            // Remove results from that step onwards
            if pos < self.step_results.len() {
                self.step_results.truncate(pos);
            }
            self.current_index = pos;
        }
    }

    /// Get step definition by name
    pub fn get_step(&self, name: &str) -> Option<&StepDefinition> {
        self.available_steps.iter().find(|s| s.name == name)
    }

    /// Get all results for a specific step
    pub fn get_results_for(&self, step_name: &str) -> Vec<&StepResult> {
        self.step_results.iter()
            .filter(|r| r.step_name == step_name)
            .collect()
    }

    /// Check if all dependencies for a step are satisfied
    pub fn dependencies_satisfied(&self, step_name: &str) -> bool {
        if let Some(step) = self.get_step(step_name) {
            for dep in &step.dependencies {
                if dep.is_empty() { continue; }
                let dep_results = self.get_results_for(dep);
                if dep_results.is_empty() || !dep_results.last().unwrap().success {
                    return false;
                }
            }
        }
        true
    }

    /// Build a context summary for the agent
    pub fn build_agent_context(&self) -> String {
        let mut ctx = String::new();
        ctx.push_str(&format!("Module: {}\n", self.module_name));
        ctx.push_str(&format!("Flow progress: {}/{}\n", self.current_index, self.planned_order.len()));
        ctx.push_str("Planned steps:\n");
        for (i, step) in self.planned_order.iter().enumerate() {
            let status = if i < self.current_index { "done" } else if i == self.current_index { "current" } else { "pending" };
            ctx.push_str(&format!("  {}. {} ({})\n", i + 1, step, status));
        }
        ctx.push_str("\nResults so far:\n");
        for result in &self.step_results {
            ctx.push_str(&result.to_llm_format());
        }
        ctx
    }

    /// Get EDA-specific tool definitions for the flow
    pub fn eda_tool_definitions() -> Vec<crate::agent::ToolDefinition> {
        vec![
            crate::agent::ToolDefinition {
                name: "parse_verilog".into(),
                description: "Parse Verilog RTL code into AST. Input: Verilog source code string. Output: parse result with module list.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "code": {"type": "string", "description": "Verilog RTL source code"}
                    },
                    "required": ["code"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "lint_check".into(),
                description: "Run lint check on a parsed module. Input: module name. Output: lint result with warnings/errors.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "module": {"type": "string", "description": "Module name to lint"}
                    },
                    "required": ["module"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "synthesize".into(),
                description: "Synthesize RTL to gate-level netlist. Input: module name. Output: gate count, area, cell type breakdown, netlist Verilog.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "module": {"type": "string", "description": "Module name to synthesize"}
                    },
                    "required": ["module"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "simulate".into(),
                description: "Run behavioral simulation. Input: module name, testbench code. Output: simulation result (PASS/FAIL), output text, VCD data.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "module": {"type": "string", "description": "Module name to simulate"},
                        "testbench": {"type": "string", "description": "Testbench Verilog code"}
                    },
                    "required": ["module"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "timing_analysis".into(),
                description: "Run static timing analysis on gate netlist. Input: netlist Verilog, clock period. Output: slack, critical path, timing violations.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "netlist": {"type": "string", "description": "Gate-level Verilog netlist"},
                        "clock_period_ns": {"type": "number", "description": "Clock period in nanoseconds"}
                    },
                    "required": ["netlist"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "power_analysis".into(),
                description: "Run power analysis on gate netlist. Input: netlist Verilog, clock frequency. Output: static/dynamic/internal/clock/glitch power breakdown.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "netlist": {"type": "string", "description": "Gate-level Verilog netlist"},
                        "freq_mhz": {"type": "number", "description": "Clock frequency in MHz"}
                    },
                    "required": ["netlist"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "formal_verify".into(),
                description: "Run formal equivalence checking between RTL and gate netlist. Input: RTL code, gate netlist. Output: EQUIVALENT/DIFFERENT with counterexample.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "rtl": {"type": "string", "description": "Original RTL Verilog code"},
                        "gate_netlist": {"type": "string", "description": "Gate-level Verilog netlist"}
                    },
                    "required": ["rtl", "gate_netlist"]
                }),
            },
            crate::agent::ToolDefinition {
                name: "area_report".into(),
                description: "Generate area report from synthesis results. Output: per-cell-type area breakdown, total area, utilization, die area.".into(),
                parameters: serde_json::json!({
                    "type": "object",
                    "properties": {
                        "netlist": {"type": "string", "description": "Gate-level Verilog netlist"}
                    },
                    "required": ["netlist"]
                }),
            },
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_step_result_formatting() {
        let result = StepResult::new("parse")
            .with_data("modules", "counter_4bit")
            .with_metric("parse_time_ms", 12.5)
            .with_warning("unconnected port: unused_output");
        let formatted = result.to_llm_format();
        assert!(formatted.contains("PASS"));
        assert!(formatted.contains("parse_time_ms"));
        assert!(formatted.contains("unconnected port"));
    }

    #[test]
    fn test_default_flow_has_all_steps() {
        let steps = FlowDecisionEngine::build_default_flow();
        let names: Vec<_> = steps.iter().map(|s| s.name.clone()).collect();
        assert!(names.contains(&"parse".to_string()));
        assert!(names.contains(&"simulate".to_string()));
        assert!(names.contains(&"synthesize".to_string()));
        assert!(names.contains(&"timing".to_string()));
        assert!(names.contains(&"formal".to_string()));
        assert!(names.contains(&"power".to_string()));
    }

    #[test]
    fn test_parse_continue() {
        // Test JSON parsing without needing an LLM client
        let json = r#"{"action": "continue", "next_step": "simulate"}"#;
        let val: serde_json::Value = serde_json::from_str(json).unwrap();
        assert_eq!(val["action"].as_str().unwrap(), "continue");
        assert_eq!(val["next_step"].as_str().unwrap(), "simulate");
    }

    #[test]
    fn test_parse_back_to_step() {
        let json = r#"{"action": "back_to_step", "step": "parse", "reason": "syntax error in generated code"}"#;
        let val: serde_json::Value = serde_json::from_str(json).unwrap();
        assert_eq!(val["action"].as_str().unwrap(), "back_to_step");
        assert_eq!(val["step"].as_str().unwrap(), "parse");
    }
}
