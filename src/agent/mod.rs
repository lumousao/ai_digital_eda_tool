/**
 * Intelligent Agent Module — Enhanced with native function calling
 *
 * Architecture:
 * - Multi-turn tool-use loop with OpenAI-native function calling
 * - Structured CoT (Chain-of-Thought) reasoning framework
 * - Root cause analysis for auto-fix
 * - Long-term memory with persistent storage
 * - Design knowledge base (RAG) integration
 * - Multi-agent collaboration support
 * - Streaming response handling
 * - Context window management with token awareness
 * - Error recovery with strategy variation
 */

pub mod flow;

use std::collections::HashMap;
use serde::{Deserialize, Serialize};
use crate::llm::{LlmClient, Message as LlmMessage, ToolMessage as LlmToolMessage, ToolDef, ToolCallMsg, Usage};

/// Tool definition for function calling
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ToolDefinition {
    pub name: String,
    pub description: String,
    pub parameters: serde_json::Value,
}

/// A tool call request from the LLM
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ToolCall {
    pub id: String,
    pub name: String,
    pub arguments: serde_json::Value,
}

impl From<&ToolCallMsg> for ToolCall {
    fn from(tcm: &ToolCallMsg) -> Self {
        ToolCall {
            id: tcm.id.clone(),
            name: tcm.function.name.clone(),
            arguments: serde_json::from_str(&tcm.function.arguments).unwrap_or(serde_json::Value::Null),
        }
    }
}

/// A tool call result to send back to the LLM
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ToolResult {
    pub tool_call_id: String,
    pub content: String,
    pub success: bool,
}

/// Agent message (different from LlmMessage to avoid conflicts)
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct AgentMessage {
    pub role: String,
    pub content: String,
    pub timestamp: u64,
    pub tool_calls: Option<Vec<ToolCall>>,
    pub tool_results: Option<Vec<ToolResult>>,
}

/// Agent context - stores all relevant information for decision making
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct AgentContext {
    pub conversation: Vec<AgentMessage>,
    pub project_state: ProjectState,
    pub decisions: Vec<Decision>,
    pub goals: Vec<Goal>,
    pub error_history: Vec<ErrorRecord>,
}

// ─── Project state ───

#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct ProjectState {
    pub current_module: Option<String>,
    pub modules: HashMap<String, ModuleInfo>,
    pub synthesis_results: Option<SynthesisResult>,
    pub simulation_results: Option<SimulationResult>,
    pub timing_results: Option<TimingResult>,
    pub power_results: Option<PowerResult>,
    pub formal_results: Option<FormalResult>,
    pub current_liberty: Option<String>,
    pub current_sdc: Option<String>,
    pub design_constraints: Vec<DesignConstraint>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ModuleInfo {
    pub name: String,
    pub port_count: usize,
    pub line_count: usize,
    pub has_testbench: bool,
    pub has_sdc: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SynthesisResult {
    pub cell_count: usize,
    pub dff_count: usize,
    pub area_ge: f64,
    pub logic_depth: usize,
    pub frequency_mhz: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SimulationResult {
    pub passed: bool,
    pub cycles: usize,
    pub errors: Vec<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TimingResult {
    pub max_freq_mhz: f64,
    pub slack_ns: f64,
    pub critical_path: String,
    pub violation_count: usize,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PowerResult {
    pub total_mw: f64,
    pub static_mw: f64,
    pub dynamic_mw: f64,
    pub clock_mw: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FormalResult {
    pub equivalent: bool,
    pub counterexample: Option<String>,
    pub check_count: usize,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DesignConstraint {
    pub constraint_type: String,  // "frequency", "area", "power", "port", "clock"
    pub value: String,
    pub source: String,
}

// ─── Decision & Error tracking ───

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Decision {
    pub context: String,
    pub action: String,
    pub outcome: String,
    pub success: bool,
    pub timestamp: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Goal {
    pub description: String,
    pub priority: u32,
    pub status: GoalStatus,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum GoalStatus {
    Pending,
    InProgress,
    Completed,
    Failed,
}

impl GoalStatus {
    pub fn status_str(&self) -> &'static str {
        match self {
            GoalStatus::Pending => "pending",
            GoalStatus::InProgress => "in_progress",
            GoalStatus::Completed => "completed",
            GoalStatus::Failed => "failed",
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ErrorRecord {
    pub error_type: String,
    pub message: String,
    pub context: String,
    pub resolution: Option<String>,
    pub strategy_used: Option<String>,
    pub timestamp: u64,
}

// ─── Error classification ───

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum ErrorClass {
    SyntaxError,
    ParseError,
    SimulationFail,
    SynthesisFail,
    TimingViolation,
    FormalFail,
    LlmError,
    Unknown,
    SpecificError(String),
}

impl ErrorClass {
    pub fn classify(error_msg: &str) -> Self {
        let msg = error_msg.to_lowercase();
        if msg.contains("syntax") || msg.contains("unexpected token") || msg.contains("expecting") {
            ErrorClass::SyntaxError
        } else if msg.contains("parse error") || msg.contains("cannot parse") {
            ErrorClass::ParseError
        } else if msg.contains("simulation") && (msg.contains("fail") || msg.contains("mismatch") || msg.contains("error")) {
            ErrorClass::SimulationFail
        } else if msg.contains("synthesis") && (msg.contains("fail") || msg.contains("error") || msg.contains("cannot")) {
            ErrorClass::SynthesisFail
        } else if msg.contains("timing") && (msg.contains("violation") || msg.contains("slack") || msg.contains("negative")) {
            ErrorClass::TimingViolation
        } else if msg.contains("formal") && (msg.contains("fail") || msg.contains("counterexample")) {
            ErrorClass::FormalFail
        } else if msg.contains("api error") || msg.contains("llm error") || msg.contains("request failed") {
            ErrorClass::LlmError
        } else {
            ErrorClass::Unknown
        }
    }

    /// LLM-based semantic classification for better error understanding
    pub fn classify_with_llm(llm: &LlmClient, error_msg: &str) -> Self {
        let prompt = format!(
            "Classify this RTL design error into ONE category: SyntaxError, ParseError, SimulationFail, \
             SynthesisFail, TimingViolation, FormalFail, LlmError, or SpecificError:<type>.\n\n\
             Error: {}\n\nReply with just the category name.", error_msg
        );
        match llm.generate(&prompt, None) {
            Ok(resp) => {
                let resp = resp.trim().to_lowercase();
                if resp.contains("syntax") { ErrorClass::SyntaxError }
                else if resp.contains("parse") { ErrorClass::ParseError }
                else if resp.contains("simulation") { ErrorClass::SimulationFail }
                else if resp.contains("synthesis") { ErrorClass::SynthesisFail }
                else if resp.contains("timing") { ErrorClass::TimingViolation }
                else if resp.contains("formal") { ErrorClass::FormalFail }
                else if resp.contains("llm") { ErrorClass::LlmError }
                else if resp.contains("specific") {
                    let typ = resp.split(':').last().unwrap_or("unknown").trim().to_string();
                    ErrorClass::SpecificError(typ)
                }
                else { ErrorClass::Unknown }
            }
            Err(_) => Self::classify(error_msg),
        }
    }

    pub fn recovery_prompt(&self) -> &str {
        match self {
            ErrorClass::SyntaxError =>
                "Fix the Verilog syntax error. Check for missing semicolons, mismatched begin/end, \
                 incorrect module declarations, or wrong signal width specifications. \
                 Output the complete corrected code.",
            ErrorClass::ParseError =>
                "The parser could not understand this Verilog code. Check for unsupported constructs, \
                 preprocessing issues, or incomplete definitions. Simplify the code structure if needed.",
            ErrorClass::SimulationFail =>
                "The simulation failed. Analyze the testbench output to identify which signal behavior \
                 is incorrect. Common causes: race conditions, missing reset handling, incorrect edge \
                 sensitivity, flawed testbench stimulus. Fix the RTL or testbench accordingly.",
            ErrorClass::SynthesisFail =>
                "Synthesis failed. Check for unsynthesizable constructs (e.g., $display in always blocks, \
                 timing delays, hierarchical references). Ensure all code is synthesizable Verilog.",
            ErrorClass::TimingViolation =>
                "Timing violation detected. The critical path delay exceeds the clock period. \
                 Consider: pipelining long combinational paths, reducing logic depth, \
                 or relaxing the constraint. Identify the critical path and suggest specific RTL changes.",
            ErrorClass::FormalFail =>
                "Formal verification found a counterexample. This means the synthesized netlist differs \
                 from the RTL specification. Check for: synthesis optimizations that changed behavior, \
                 incorrect assertions, or missing timing constraints that led to incorrect optimization.",
            ErrorClass::LlmError =>
                "LLM API error. Retry with reduced context size. If persistent, try a different model or API.",
            ErrorClass::Unknown =>
                "Analyze the error carefully. Identify the root cause, propose a specific fix, \
                 and verify the fix doesn't introduce new issues.",
            ErrorClass::SpecificError(typ) => Box::leak(
                format!("Specific error type: {}. Analyze and propose targeted fix.", typ).into_boxed_str()
            ),
        }
    }

    /// Different recovery strategies to try (ordered by success probability)
    pub fn recovery_strategies(&self) -> Vec<&str> {
        match self {
            ErrorClass::SyntaxError => vec![
                "Fix the exact syntax error following Verilog LRM rules.",
                "Simplify the affected code block and re-verify syntax.",
                "Check for missing declarations or incorrect module parameterization.",
                "Verify that all begin/end pairs are balanced.",
                "Check for missing semicolons after statements.",
                "Ensure module port list matches declaration order.",
            ],
            ErrorClass::ParseError => vec![
                "Remove unsupported SystemVerilog constructs not in Verilog-2005.",
                "Split multi-line expressions into simpler assignments.",
                "Check for preprocessing issues (misplaced define/include macros).",
                "Ensure generate blocks have proper genvar declarations.",
                "Check for unbalanced parentheses in complex expressions.",
            ],
            ErrorClass::SimulationFail => vec![
                "Fix the testbench stimulus to match expected behavior.",
                "Add proper reset sequence at simulation start.",
                "Check for race conditions in non-blocking assignments.",
                "Verify clock generation is correct in testbench.",
                "Add assertions to identify exact failure point.",
                "Reduce simulation to minimal failing scenario for diagnosis.",
            ],
            ErrorClass::SynthesisFail => vec![
                "Remove unsynthesizable constructs and replace with RTL alternatives.",
                "Split complex always blocks into simpler ones.",
                "Check for incomplete case statements causing latch inference.",
                "Replace $display/$monitor with assertion modules for synthesis.",
                "Remove timing delays (#delays) from synthesizable code.",
                "Convert initial blocks to reset sequences in always blocks.",
            ],
            ErrorClass::TimingViolation => vec![
                "Add pipeline registers to break up the critical path.",
                "Reduce logic depth by restructuring the combinational logic.",
                "Use faster cells from the library (lower drive strength → higher speed).",
                "Relax timing constraints if design requirements allow.",
                "Apply retiming to balance register-to-register delays.",
                "Convert deep priority encoders to one-hot or parallel structures.",
            ],
            ErrorClass::FormalFail => vec![
                "Verify that RTL and gate netlist have matching port orders.",
                "Check for initialization differences between RTL and gates.",
                "Ensure synthesis optimizations didn't change reset behavior.",
                "Add formal assertions to narrow down the mismatch location.",
                "Run equivalence checking on sub-modules individually.",
            ],
            ErrorClass::LlmError => vec![
                "Retry with reduced context size (shorter conversation).",
                "Switch to a different model or API provider.",
                "Check network connectivity and API key validity.",
                "Split the prompt into smaller, focused requests.",
                "Use structured tool calls instead of free-form text generation.",
            ],
            ErrorClass::Unknown => vec![
                "Analyze the error log for specific keywords and patterns.",
                "Search the knowledge base for similar past errors.",
                "Isolate the failing component and test independently.",
                "Compare with a known-working reference design.",
            ],
            ErrorClass::SpecificError(typ) => {
                let t = typ.as_str();
                if t.contains("width") || t.contains("bit") {
                    vec!["Check signal width mismatches in port connections.",
                         "Ensure all part-selects are within valid ranges.",
                         "Verify parameterized width calculations are correct."]
                } else if t.contains("clock") || t.contains("clk") {
                    vec!["Verify clock domain assignments are correct.",
                         "Check for missing clock constraints on generated clocks.",
                         "Ensure clock gating enable meets setup/hold requirements."]
                } else if t.contains("power") || t.contains("ir_drop") {
                    vec!["Reduce simultaneous switching on high-fanout nets.",
                         "Add decoupling cells to mitigate IR drop.",
                         "Stagger clock enables to reduce peak current."]
                } else {
                    vec!["Analyze the specific error pattern and apply targeted fix.",
                         "Consult the knowledge base for matching error patterns.",
                         "Try incrementally modifying the design to isolate the issue."]
                }
            },
        }
    }

    /// Estimate success probability for each strategy (used for auto-selection)
    pub fn strategy_weights(&self) -> Vec<f64> {
        match self {
            ErrorClass::SyntaxError => vec![0.6, 0.4, 0.3, 0.2, 0.5, 0.3],
            ErrorClass::TimingViolation => vec![0.5, 0.4, 0.3, 0.7, 0.35, 0.3],
            ErrorClass::SynthesisFail => vec![0.55, 0.45, 0.5, 0.6, 0.8, 0.4],
            ErrorClass::SimulationFail => vec![0.4, 0.6, 0.35, 0.7, 0.3, 0.25],
            ErrorClass::ParseError => vec![0.5, 0.4, 0.45, 0.6, 0.5],
            _ => vec![0.5, 0.4, 0.3],
        }
    }
}

// ─── Thinking modes ───

#[derive(Clone, Debug)]
pub enum ThinkingMode {
    Quick,
    Normal,
    Deep,
    AutoFix,
    ToolUse,
    ChainOfThought,     // Structured step-by-step reasoning
    RootCauseAnalysis,  // Systematic root cause investigation
    DesignExplore,      // Multi-variant design space exploration
}

// ─── Tool executor ───

pub trait ToolExecutor: Send + Sync {
    fn execute(&self, tool_name: &str, arguments: &serde_json::Value) -> Result<String, String>;
    fn available_tools(&self) -> Vec<ToolDefinition>;
}

// ─── Design Knowledge Base (RAG) ───

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct KnowledgeEntry {
    pub category: String,
    pub title: String,
    pub content: String,
    pub tags: Vec<String>,
    pub relevance_score: f64,
}

pub struct KnowledgeBase {
    pub entries: Vec<KnowledgeEntry>,
    pub error_fix_patterns: HashMap<String, Vec<String>>,
    pub rtl_patterns: HashMap<String, String>,
    pub constraint_templates: HashMap<String, String>,
    pub design_rules: Vec<String>,
}

impl KnowledgeBase {
    pub fn new() -> Self {
        let mut kb = KnowledgeBase {
            entries: Vec::new(),
            error_fix_patterns: HashMap::new(),
            rtl_patterns: HashMap::new(),
            constraint_templates: HashMap::new(),
            design_rules: Vec::new(),
        };

        // Initialize common RTL patterns
        kb.rtl_patterns.insert("fifo".into(), include_rtl_pattern_fifo());
        kb.rtl_patterns.insert("fsm".into(), include_rtl_pattern_fsm());
        kb.rtl_patterns.insert("arbiter".into(), include_rtl_pattern_arbiter());
        kb.rtl_patterns.insert("pipeline".into(), include_rtl_pattern_pipeline());
        kb.rtl_patterns.insert("cdc".into(), include_rtl_pattern_cdc());
        kb.rtl_patterns.insert("counter".into(), include_rtl_pattern_counter());
        kb.rtl_patterns.insert("shift_reg".into(), include_rtl_pattern_shift_reg());
        kb.rtl_patterns.insert("alu".into(), include_rtl_pattern_alu());
        kb.rtl_patterns.insert("uart".into(), include_rtl_pattern_uart());
        kb.rtl_patterns.insert("spi".into(), include_rtl_pattern_spi());
        kb.rtl_patterns.insert("i2c".into(), include_rtl_pattern_i2c());
        kb.rtl_patterns.insert("multiplier".into(), include_rtl_pattern_multiplier());
        kb.rtl_patterns.insert("divider".into(), include_rtl_pattern_divider());
        kb.rtl_patterns.insert("memory".into(), include_rtl_pattern_memory());
        kb.rtl_patterns.insert("priority_encoder".into(), include_rtl_pattern_priority_encoder());

        // Initialize constraint templates
        kb.constraint_templates.insert("single_clock".into(), include_sdc_single_clock());
        kb.constraint_templates.insert("multi_clock".into(), include_sdc_multi_clock());
        kb.constraint_templates.insert("generated_clock".into(), include_sdc_generated_clock());
        kb.constraint_templates.insert("io_constraints".into(), include_sdc_io_constraints());

        // Design rules
        kb.design_rules.push("Avoid latches — ensure all case statements are complete.".into());
        kb.design_rules.push("Use non-blocking assignments (<=) in sequential always blocks.".into());
        kb.design_rules.push("Use blocking assignments (=) in combinational always blocks.".into());
        kb.design_rules.push("Always reset all registers to a known state.".into());
        kb.design_rules.push("Avoid multiple drivers on the same net.".into());
        kb.design_rules.push("Use synchronizers for clock domain crossing signals.".into());
        kb.design_rules.push("Avoid combinational loops.".into());
        kb.design_rules.push("Don't mix posedge and negedge in the same always block sensitivity list.".into());

        // Error fix patterns
        kb.error_fix_patterns.insert("unexpected_token".into(), vec![
            "Check for missing semicolon or mismatched parentheses.".into(),
            "Verify that all keywords are correctly spelled.".into(),
            "Check if a SystemVerilog keyword is used as an identifier.".into(),
        ]);
        kb.error_fix_patterns.insert("multiple_drivers".into(), vec![
            "Ensure each net is driven by only one always block or assign statement.".into(),
            "Use a mux to select between multiple driver sources.".into(),
        ]);
        kb.error_fix_patterns.insert("latch_inference".into(), vec![
            "Add an else clause or default case to make all branches complete.".into(),
            "Assign default values at the beginning of the always block.".into(),
        ]);
        kb.error_fix_patterns.insert("timing_violation".into(), vec![
            "Check critical path logic depth and consider pipelining.".into(),
            "Reduce combinational logic between registers.".into(),
            "Verify clock constraints match actual clock frequency.".into(),
        ]);
        kb.error_fix_patterns.insert("width_mismatch".into(), vec![
            "Check port connection widths — ensure LHS width >= RHS width.".into(),
            "Verify parameterized widths propagate correctly through hierarchy.".into(),
            "Add explicit width casting where needed.".into(),
        ]);
        kb.error_fix_patterns.insert("inferred_latch".into(), vec![
            "Ensure all branches of if/else and case have assignments to all outputs.".into(),
            "Initialize output signals before the combinational logic block.".into(),
            "Use default assignments in case statements for unhandled cases.".into(),
        ]);
        kb.error_fix_patterns.insert("combinational_loop".into(), vec![
            "Break the feedback path with a register.".into(),
            "Ensure always @(*) sensitivity list includes all inputs.".into(),
            "Check for self-referencing assign statements.".into(),
        ]);

        // Add more RTL patterns
        kb.rtl_patterns.insert("gray_code".into(), include_rtl_pattern_gray_code());
        kb.rtl_patterns.insert("lfsr".into(), include_rtl_pattern_lfsr());
        kb.rtl_patterns.insert("crc".into(), include_rtl_pattern_crc());
        kb.rtl_patterns.insert("dsp_mult".into(), include_rtl_pattern_dsp_mult());

        // Add more design rules
        kb.design_rules.push("For large muxes, use case statements rather than nested ternary operators.".into());
        kb.design_rules.push("Pipeline deeply nested operations (e.g., multiply-add chains) for timing closure.".into());
        kb.design_rules.push("Use parameterized designs for reusability across different configurations.".into());
        kb.design_rules.push("Register outputs of complex combinational blocks to improve timing.".into());
        kb.design_rules.push("Use generate blocks for repetitive structures to reduce code duplication.".into());

        kb
    }

    pub fn search(&self, query: &str) -> Vec<&KnowledgeEntry> {
        let query_lower = query.to_lowercase();
        let mut results: Vec<&KnowledgeEntry> = self.entries.iter()
            .filter(|e| {
                e.title.to_lowercase().contains(&query_lower)
                    || e.content.to_lowercase().contains(&query_lower)
                    || e.tags.iter().any(|t| t.to_lowercase().contains(&query_lower))
            })
            .collect();
        results.sort_by(|a, b| b.relevance_score.partial_cmp(&a.relevance_score).unwrap());
        results
    }

    pub fn get_rtl_pattern(&self, name: &str) -> Option<&String> {
        self.rtl_patterns.get(name)
    }

    pub fn get_constraint_template(&self, name: &str) -> Option<&String> {
        self.constraint_templates.get(name)
    }

    pub fn get_error_fixes(&self, error_type: &str) -> Option<&Vec<String>> {
        self.error_fix_patterns.get(error_type)
    }

    pub fn add_entry(&mut self, category: &str, title: &str, content: &str, tags: Vec<String>) {
        self.entries.push(KnowledgeEntry {
            category: category.into(),
            title: title.into(),
            content: content.into(),
            tags,
            relevance_score: 1.0,
        });
    }

    pub fn build_context_prompt(&self, query: &str, max_entries: usize) -> String {
        let results = self.search(query);
        if results.is_empty() { return String::new(); }

        let mut prompt = String::from("\n\n[RELEVANT DESIGN KNOWLEDGE]\n");
        for entry in results.iter().take(max_entries) {
            prompt.push_str(&format!("## {}\n{}\n", entry.title, entry.content));
        }
        prompt
    }
}

// ─── Agent configuration ───

#[derive(Clone, Debug)]
pub struct AgentConfig {
    pub max_tool_iterations: usize,
    pub max_self_critique: usize,
    pub context_window_messages: usize,
    pub max_context_tokens: usize,
    pub stream_enabled: bool,
    pub cost_limit_usd: f64,
}

impl Default for AgentConfig {
    fn default() -> Self {
        AgentConfig {
            max_tool_iterations: 10,
            max_self_critique: 3,
            context_window_messages: 50,
            max_context_tokens: 64000,
            stream_enabled: false,
            cost_limit_usd: 5.0,
        }
    }
}

// ─── Multi-agent role ───

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum AgentRole {
    Designer,     // RTL design and coding
    Verifier,     // Simulation and testbench
    Optimizer,    // Synthesis and timing optimization
    Reviewer,     // Code review and quality check
    Coordinator,  // Orchestrates other agents
}

// ─── Main Agent ───

pub struct Agent {
    llm: LlmClient,
    tool_executor: Option<Box<dyn ToolExecutor>>,
    pub context: AgentContext,
    pub thinking_mode: ThinkingMode,
    pub verbosity: u32,
    pub config: AgentConfig,
    pub knowledge_base: KnowledgeBase,
    pub role: AgentRole,
    /// Other agent handles this agent can talk to
    agent_peers: Vec<String>,
    /// Long-term memory file path
    memory_path: Option<String>,
}

impl Agent {
    pub fn new(llm: LlmClient) -> Self {
        Agent {
            llm,
            tool_executor: None,
            context: AgentContext {
                conversation: Vec::new(),
                project_state: ProjectState::default(),
                decisions: Vec::new(),
                goals: Vec::new(),
                error_history: Vec::new(),
            },
            thinking_mode: ThinkingMode::Normal,
            verbosity: 1,
            config: AgentConfig::default(),
            knowledge_base: KnowledgeBase::new(),
            role: AgentRole::Designer,
            agent_peers: Vec::new(),
            memory_path: None,
        }
    }

    // ─── Configuration ───

    pub fn set_thinking_mode(&mut self, mode: ThinkingMode) {
        self.thinking_mode = mode;
    }

    pub fn set_tool_executor(&mut self, executor: Box<dyn ToolExecutor>) {
        self.tool_executor = Some(executor);
    }

    pub fn set_role(&mut self, role: AgentRole) {
        self.role = role;
    }

    pub fn set_peers(&mut self, peers: Vec<String>) {
        self.agent_peers = peers;
    }

    pub fn set_memory_path(&mut self, path: &str) {
        // Load existing memory if available
        if let Ok(data) = std::fs::read_to_string(path) {
            if let Ok(ctx) = serde_json::from_str::<AgentContext>(&data) {
                self.context = ctx;
            }
        }
        self.memory_path = Some(path.to_string());
    }

    /// Save agent context to persistent memory
    pub fn save_memory(&self) {
        if let Some(ref path) = self.memory_path {
            if let Ok(json) = serde_json::to_string_pretty(&self.context) {
                let _ = std::fs::write(path, &json);
            }
        }
    }

    // ─── Conversation management ───

    pub fn add_message(&mut self, role: &str, content: &str) {
        self.context.conversation.push(AgentMessage {
            role: role.to_string(),
            content: content.to_string(),
            timestamp: now_secs(),
            tool_calls: None,
            tool_results: None,
        });
        self.manage_context_window();
    }

    pub fn update_project_state(&mut self, state: ProjectState) {
        self.context.project_state = state;
    }

    pub fn record_decision(&mut self, context: &str, action: &str, outcome: &str, success: bool) {
        self.context.decisions.push(Decision {
            context: context.to_string(),
            action: action.to_string(),
            outcome: outcome.to_string(),
            success,
            timestamp: now_secs(),
        });
    }

    pub fn record_error(&mut self, error_type: &str, message: &str, context: &str) -> ErrorClass {
        let class = ErrorClass::classify(message);
        self.context.error_history.push(ErrorRecord {
            error_type: error_type.to_string(),
            message: message.to_string(),
            context: context.to_string(),
            resolution: None,
            strategy_used: None,
            timestamp: now_secs(),
        });
        class
    }

    /// Record error resolution
    pub fn record_resolution(&mut self, error_index: usize, resolution: &str, strategy: &str) {
        if let Some(record) = self.context.error_history.get_mut(error_index) {
            record.resolution = Some(resolution.to_string());
            record.strategy_used = Some(strategy.to_string());
        }
    }

    // ─── Context window management ───

    fn manage_context_window(&mut self) {
        let estimated_tokens: usize = self.context.conversation.iter()
            .map(|m| Self::estimate_tokens(&m.content))
            .sum();

        // If estimated tokens exceeds limit, trim oldest messages
        while estimated_tokens > self.config.max_context_tokens && self.context.conversation.len() > 5 {
            // Keep system-level messages (errors, tool results), remove intermediate ones
            let mut removed = false;
            for i in 0..self.context.conversation.len() {
                let msg = &self.context.conversation[i];
                if msg.role == "assistant" && msg.tool_calls.is_none() && msg.tool_results.is_none() {
                    self.context.conversation.remove(i);
                    removed = true;
                    break;
                }
            }
            if !removed {
                // Remove oldest message as fallback
                if !self.context.conversation.is_empty() {
                    self.context.conversation.remove(0);
                }
            }
            // Recalculate to prevent infinite loop
            if self.context.conversation.len() <= 5 { break; }
        }

        // Trim by count as well
        if self.context.conversation.len() > self.config.context_window_messages {
            let excess = self.context.conversation.len() - self.config.context_window_messages;
            for _ in 0..excess {
                if self.context.conversation.len() > 1 {
                    self.context.conversation.remove(0);
                }
            }
        }
    }

    // ─── Thinking dispatch ───

    pub fn think(&self, user_message: &str) -> Result<String, String> {
        match self.thinking_mode {
            ThinkingMode::Quick => self.think_quick(user_message),
            ThinkingMode::Normal => self.think_normal(user_message),
            ThinkingMode::Deep => self.think_deep(user_message),
            ThinkingMode::AutoFix => self.think_autofix(user_message),
            ThinkingMode::ToolUse => self.think_with_tools_native(user_message, &[]),
            ThinkingMode::ChainOfThought => self.think_cot(user_message),
            ThinkingMode::RootCauseAnalysis => self.think_rca(user_message),
            ThinkingMode::DesignExplore => self.think_design_explore(user_message),
        }
    }

    // ─── Native function calling tool-use loop ───

    pub fn think_with_tools_native(
        &self,
        user_message: &str,
        tools: &[ToolDefinition],
    ) -> Result<String, String> {
        let system_prompt = self.build_tool_system_prompt();

        // Convert to native API tool definitions
        let api_tools: Vec<ToolDef> = tools.iter().map(|t| {
            ToolDef::new(&t.name, &t.description, t.parameters.clone())
        }).collect();

        let mut llm_messages: Vec<LlmToolMessage> = Vec::new();
        llm_messages.push(LlmToolMessage::system(&system_prompt));

        // Include recent conversation context
        let recent: Vec<_> = self.context.conversation.iter().rev().take(15).collect();
        for msg in recent.iter().rev() {
            llm_messages.push(LlmToolMessage {
                role: msg.role.clone(),
                content: msg.content.clone(),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            });
        }
        llm_messages.push(LlmToolMessage::user(user_message));

        let (response_text, tool_calls, _usage) = self.llm.chat_with_tool_messages(&llm_messages, &api_tools)?;

        if tool_calls.is_empty() {
            return Ok(response_text);
        }

        // Execute tool calls and build follow-up messages
        llm_messages.push(LlmToolMessage::assistant_with_tool_calls(tool_calls.clone()));

        let mut final_response = String::new();

        for iteration in 0..self.config.max_tool_iterations {
            let current_calls = if iteration == 0 {
                tool_calls.clone()
            } else {
                // Get more tool calls
                let (text, calls, _usage) = self.llm.chat_with_tool_messages(&llm_messages, &api_tools)?;
                if calls.is_empty() {
                    final_response = text;
                    break;
                }
                llm_messages.push(LlmToolMessage::assistant_with_tool_calls(calls.clone()));
                calls
            };

            for tc in &current_calls {
                let args: serde_json::Value = serde_json::from_str(&tc.function.arguments)
                    .unwrap_or(serde_json::Value::Null);

                let result = if let Some(ref executor) = self.tool_executor {
                    executor.execute(&tc.function.name, &args)
                } else {
                    Err(format!("No tool executor available for '{}'", tc.function.name))
                };

                let content = match &result {
                    Ok(output) => output.clone(),
                    Err(e) => format!("Error executing {}: {}", tc.function.name, e),
                };

                llm_messages.push(LlmToolMessage::tool(&tc.id, &content));
            }

            // Get final or continued response
            let (text, next_calls, _usage) = self.llm.chat_with_tool_messages(&llm_messages, &api_tools)?;
            if next_calls.is_empty() {
                final_response = text;
                break;
            }
            llm_messages.push(LlmToolMessage::assistant_with_tool_calls(next_calls));
        }

        if final_response.is_empty() {
            Err("Max tool-use iterations reached without final answer".to_string())
        } else {
            Ok(final_response)
        }
    }

    // ─── Chain-of-Thought reasoning ───

    fn think_cot(&self, user_message: &str) -> Result<String, String> {
        let system_prompt = self.build_system_prompt(
            "You are an expert RTL design assistant. Use Chain-of-Thought reasoning:\n\
             1. UNDERSTAND the problem and identify constraints\n\
             2. EXPLORE 2-3 possible approaches\n\
             3. SELECT the best approach with justification\n\
             4. IMPLEMENT the solution step by step\n\
             5. VERIFY the solution is correct and complete\n\n\
             Label each step clearly."
        );

        let context = self.build_full_context(user_message);
        let mut messages = vec![
            LlmToolMessage::system(&system_prompt),
            LlmToolMessage::user(&context),
        ];
        self.chat(&mut messages)
    }

    // ─── Root cause analysis ───

    fn think_rca(&self, error_message: &str) -> Result<String, String> {
        let system_prompt = self.build_system_prompt(
            "You are an expert RTL debug engineer. Perform root cause analysis:\n\
             1. CLASSIFY the error type\n\
             2. TRACE back to find the root cause (not just the symptom)\n\
             3. IDENTIFY all contributing factors\n\
             4. PROPOSE a fix that addresses the root cause\n\
             5. VERIFY the fix won't cause regressions\n\n\
             Be thorough and specific."
        );

        let error_history = self.build_error_summary();
        let full_msg = format!("Error:\n{}\n\nError History:\n{}", error_message, error_history);
        let mut messages = vec![
            LlmToolMessage::system(&system_prompt),
            LlmToolMessage::user(&full_msg),
        ];
        self.chat(&mut messages)
    }

    // ─── Design space exploration ───

    fn think_design_explore(&self, spec: &str) -> Result<String, String> {
        let system_prompt = self.build_system_prompt(
            "You are an expert RTL architect. Explore the design space:\n\
             1. GENERATE 3-5 distinct architectural approaches\n\
             2. EVALUATE each approach on: area, latency, throughput, power, complexity\n\
             3. SCORE each approach (1-10) for each metric\n\
             4. RANK approaches and recommend the best one\n\
             5. JUSTIFY your recommendation\n\n\
             Output as a structured comparison."
        );
        let mut messages = vec![
            LlmToolMessage::system(&system_prompt),
            LlmToolMessage::user(spec),
        ];
        self.chat(&mut messages)
    }

    // ─── Legacy thinking methods (compatible with text-mode tool calls) ───

    fn think_quick(&self, user_message: &str) -> Result<String, String> {
        let system = self.build_system_prompt("You are a helpful RTL design assistant. Respond concisely.");
        let mut messages = vec![LlmToolMessage::system(&system), LlmToolMessage::user(user_message)];
        self.chat(&mut messages)
    }

    fn think_normal(&self, user_message: &str) -> Result<String, String> {
        let system = self.build_system_prompt(
            "You are an expert RTL design assistant. Analyze the problem, \
             consider the context, and provide a clear solution. Mention potential issues."
        );
        let context = self.build_full_context(user_message);
        let mut messages = vec![LlmToolMessage::system(&system), LlmToolMessage::user(&context)];
        self.chat(&mut messages)
    }

    fn think_deep(&self, user_message: &str) -> Result<String, String> {
        let system = self.build_system_prompt(
            "You are an expert RTL design assistant with deep knowledge of digital design.\n\
             When solving problems:\n\
             1. ANALYZE the problem thoroughly\n\
             2. CRITIQUE potential approaches\n\
             3. PLAN the best solution\n\
             4. VERIFY the solution is correct\n\
             5. IMPLEMENT the solution\n\n\
             Show your thinking process. Be thorough and precise."
        );
        let context = self.build_full_context(user_message);
        let mut messages = vec![LlmToolMessage::system(&system), LlmToolMessage::user(&context)];
        self.chat(&mut messages)
    }

    fn think_autofix(&self, error_message: &str) -> Result<String, String> {
        let error_class = ErrorClass::classify(error_message);
        let recovery_hint = error_class.recovery_prompt();

        // Try KB error fixes first
        let kb_fixes = self.knowledge_base.get_error_fixes("unknown");
        let kb_hint = kb_fixes.map(|fixes| fixes.join("\n")).unwrap_or_default();

        let system = self.build_system_prompt(&format!(
            "You are an expert RTL debug engineer.\n\
             DIAGNOSED ERROR TYPE: {:?}\n\
             {}\n\n\
             Known fixes:\n{}\n\n\
             1. ANALYZE the error root cause\n\
             2. PROPOSE a specific code fix\n\
             3. VERIFY the fix is correct and complete\n\
             Output the COMPLETE corrected code.",
            error_class, recovery_hint, kb_hint
        ));

        let context = self.build_full_context(error_message);
        let mut messages = vec![LlmToolMessage::system(&system), LlmToolMessage::user(&context)];
        self.chat(&mut messages)
    }

    fn think_with_tools(&self, _user_message: &str, _tools: &[ToolDefinition]) -> Result<String, String> {
        // Legacy text-mode tool parsing — kept for backward compatibility
        Err("Use think_with_tools_native for API-native tool calling".to_string())
    }

    // ─── Self-critique loop ───

    pub fn think_with_critique(
        &self,
        user_message: &str,
        critique_check: impl Fn(&str) -> (bool, String),
    ) -> Result<String, String> {
        let mut current_response = self.think_deep(user_message)?;
        for _attempt in 0..self.config.max_self_critique {
            let (is_ok, feedback) = critique_check(&current_response);
            if is_ok { return Ok(current_response); }

            let critique_prompt = format!(
                "Your previous response has issues:\n\nFEEDBACK:\n{}\n\nRESPONSE:\n{}\n\n\
                 Fix ALL issues. Assessment criteria: syntax correctness, synthesizability, \
                 timing feasibility, code completeness, test coverage.",
                feedback, current_response
            );

            let system = self.build_system_prompt(
                "Improve the previous response. Address ALL issues. Be specific and thorough."
            );

            let mut messages = vec![LlmToolMessage::system(&system), LlmToolMessage::user(&critique_prompt)];
            match self.chat(&mut messages) {
                Ok(improved) => current_response = improved,
                Err(_) => break,
            }
        }
        Ok(current_response)
    }

    // ─── Multi-turn tool-use loop (for REPL integration) ───

    pub fn run_loop(&mut self, user_request: &str, tools: &[ToolDefinition]) -> Result<String, String> {
        self.add_message("user", user_request);

        let api_tools: Vec<ToolDef> = tools.iter().map(|t| {
            ToolDef::new(&t.name, &t.description, t.parameters.clone())
        }).collect();

        let system = self.build_tool_system_prompt();
        let mut llm_messages = vec![LlmToolMessage::system(&system)];
        llm_messages.push(LlmToolMessage::user(user_request));

        for iteration in 0..self.config.max_tool_iterations {
            let (text, tool_calls, _usage) = self.llm.chat_with_tool_messages(&llm_messages, &api_tools)?;

            if tool_calls.is_empty() {
                self.add_message("assistant", &text);
                return Ok(text);
            }

            self.add_message("assistant", &format!("Tool calls: {:?}", tool_calls.iter().map(|t| &t.function.name).collect::<Vec<_>>()));

            llm_messages.push(LlmToolMessage::assistant_with_tool_calls(tool_calls.clone()));

            for tc in &tool_calls {
                let args: serde_json::Value = serde_json::from_str(&tc.function.arguments)
                    .unwrap_or(serde_json::Value::Null);
                let result = if let Some(ref executor) = self.tool_executor {
                    executor.execute(&tc.function.name, &args)
                } else {
                    Err("No tool executor".to_string())
                };
                let content = match &result {
                    Ok(o) => { self.record_decision(&format!("tool:{}", tc.function.name), "execute", o, true); o.clone() }
                    Err(e) => { self.record_decision(&format!("tool:{}", tc.function.name), "execute", e, false); e.clone() }
                };
                llm_messages.push(LlmToolMessage::tool(&tc.id, &content));
            }

            if iteration >= self.config.max_tool_iterations - 1 {
                return Err("Max tool iterations reached".to_string());
            }
        }
        Err("Tool-use loop failed".to_string())
    }

    // ─── Peer agent communication ───

    pub fn send_to_peers(&self, message: &str) -> Vec<(String, String)> {
        // Returns (peer_name, response) pairs
        // In a real implementation, this would use inter-agent messaging
        self.agent_peers.iter().map(|p| (p.clone(), format!("[{} ACK]", p))).collect()
    }

    // ─── Prompt builders ───

    fn build_system_prompt(&self, base: &str) -> String {
        let mut prompt = base.to_string();
        let role_desc = match self.role {
            AgentRole::Designer => "\n\nRole: RTL Designer — create and modify Verilog designs.",
            AgentRole::Verifier => "\n\nRole: Verification Engineer — create testbenches and verify correctness.",
            AgentRole::Optimizer => "\n\nRole: Optimization Engineer — improve timing, area, and power.",
            AgentRole::Reviewer => "\n\nRole: Code Reviewer — check quality, synthesizability, and style.",
            AgentRole::Coordinator => "\n\nRole: Coordinator — orchestrate design tasks across team.",
        };
        prompt.push_str(role_desc);

        // ── Project state injection ──
        if let Some(ref module) = self.context.project_state.current_module {
            prompt.push_str(&format!("\n\nCurrent module: {}", module));
        }

        // Synthesis state
        if let Some(ref synth) = self.context.project_state.synthesis_results {
            prompt.push_str(&format!(
                "\nSynthesis: {} cells, {} DFF, {:.0} GE, {} logic levels, {:.0} MHz",
                synth.cell_count, synth.dff_count, synth.area_ge, synth.logic_depth, synth.frequency_mhz
            ));
        }

        // Timing state
        if let Some(ref timing) = self.context.project_state.timing_results {
            prompt.push_str(&format!(
                "\nTiming: {:.0} MHz max, {:.2} ns slack, {} violations",
                timing.max_freq_mhz, timing.slack_ns, timing.violation_count
            ));
            if !timing.critical_path.is_empty() {
                prompt.push_str(&format!("\nCritical path: {}", timing.critical_path));
            }
        }

        // Power state
        if let Some(ref power) = self.context.project_state.power_results {
            prompt.push_str(&format!(
                "\nPower: {:.2} mW total ({:.2} static, {:.2} dynamic, {:.2} clock)",
                power.total_mw, power.static_mw, power.dynamic_mw, power.clock_mw
            ));
        }

        // Simulation state
        if let Some(ref sim) = self.context.project_state.simulation_results {
            prompt.push_str(&format!(
                "\nSimulation: {} ({} cycles, {} errors)",
                if sim.passed { "PASS" } else { "FAIL" }, sim.cycles, sim.errors.len()
            ));
        }

        // Formal verification state
        if let Some(ref formal) = self.context.project_state.formal_results {
            prompt.push_str(&format!(
                "\nFormal: {} ({} checks)",
                if formal.equivalent { "EQUIVALENT" } else { "DIFFERENT" }, formal.check_count
            ));
        }

        // Design constraints
        if !self.context.project_state.design_constraints.is_empty() {
            prompt.push_str("\n\nConstraints:");
            for c in &self.context.project_state.design_constraints {
                prompt.push_str(&format!("\n- {}: {} (from: {})", c.constraint_type, c.value, c.source));
            }
        }

        // Goals with priority
        if !self.context.goals.is_empty() {
            prompt.push_str("\n\nGoals:");
            for g in &self.context.goals {
                let status_icon = match g.status {
                    GoalStatus::Completed => "✓",
                    GoalStatus::Failed => "✗",
                    GoalStatus::InProgress => "▶",
                    GoalStatus::Pending => "○",
                };
                prompt.push_str(&format!("\n- {} [{:?}] P{} {}", status_icon, g.status, g.priority, g.description));
            }
        }

        // Error history (attention focusing)
        let unresolved: Vec<_> = self.context.error_history.iter()
            .filter(|e| e.resolution.is_none())
            .collect();
        if !unresolved.is_empty() {
            prompt.push_str("\n\n⚠ UNRESOLVED ERRORS (fix these first):");
            for e in unresolved.iter().rev().take(3) {
                prompt.push_str(&format!("\n- {}: {}", e.error_type, e.message));
            }
        }

        // Recent decisions
        let recent_decisions: Vec<_> = self.context.decisions.iter().rev().take(3).collect();
        if !recent_decisions.is_empty() {
            prompt.push_str("\n\nRecent decisions:");
            for d in &recent_decisions {
                let icon = if d.success { "✓" } else { "✗" };
                prompt.push_str(&format!("\n- {} {} → {} ({})", icon, d.action, d.outcome, d.context));
            }
        }

        prompt
    }

    fn build_tool_system_prompt(&self) -> String {
        let mut prompt = String::from(
            "You are an expert RTL design assistant with access to EDA tools.\n\
             Use the available tools to:\n\
             - Parse and analyze Verilog RTL code\n\
             - Run lint checks on designs\n\
             - Synthesize RTL to gate-level netlists\n\
             - Run simulations to verify functionality\n\
             - Analyze timing and identify critical paths\n\
             - Check power consumption\n\
             - Run formal equivalence checks\n\n\
             WORKFLOW: analyze → plan → execute tools → evaluate results → iterate if needed → provide final answer.\n\
             When a step fails, diagnose the root cause and propose fixes — don't just report the error."
        );

        if let Some(ref module) = self.context.project_state.current_module {
            prompt.push_str(&format!("\nWorking on module: {}", module));
        }
        if self.context.project_state.current_liberty.is_some() {
            prompt.push_str("\nLiberty library is loaded.");
        }
        prompt
    }

    fn build_full_context(&self, user_message: &str) -> String {
        let mut ctx = user_message.to_string();
        let summary = self.build_context_summary();
        if !summary.is_empty() {
            ctx.push_str(&format!("\n\nContext:\n{}", summary));
        }
        let errors = self.build_error_summary();
        if !errors.is_empty() && !errors.contains("No recent errors") {
            ctx.push_str(&format!("\n\nRecent Errors:\n{}", errors));
        }
        ctx
    }

    fn build_context_summary(&self) -> String {
        let mut s = String::new();
        if let Some(ref m) = self.context.project_state.current_module {
            s.push_str(&format!("Module: {}\n", m));
        }
        for (name, info) in &self.context.project_state.modules {
            s.push_str(&format!("{}: {} ports, {} lines\n", name, info.port_count, info.line_count));
        }
        let recent: Vec<_> = self.context.decisions.iter().rev().take(5).collect();
        if !recent.is_empty() {
            s.push_str("Recent decisions:\n");
            for d in recent {
                s.push_str(&format!("- {} → {} ({})\n", d.action, d.outcome, if d.success { "OK" } else { "FAIL" }));
            }
        }
        s
    }

    fn build_error_summary(&self) -> String {
        let mut s = String::new();
        let recent: Vec<_> = self.context.error_history.iter().rev().take(5).collect();
        if recent.is_empty() { s.push_str("No recent errors.\n"); }
        for e in recent {
            s.push_str(&format!("- {}: {}\n", e.error_type, e.message));
            if let Some(ref res) = e.resolution { s.push_str(&format!("  Fixed: {}\n", res)); }
        }
        s
    }

    // ─── Utilities ───

    fn chat(&self, messages: &mut Vec<LlmToolMessage>) -> Result<String, String> {
        let plain: Vec<LlmMessage> = messages.iter().map(|m| LlmMessage { role: m.role.clone(), content: m.content.clone() }).collect();
        let (content, _) = self.llm.chat_with_usage(&plain)?;
        Ok(content)
    }

    pub fn estimate_tokens(text: &str) -> usize {
        (text.len() + 3) / 4
    }

    pub fn estimated_conversation_tokens(&self) -> usize {
        self.context.conversation.iter().map(|m| Self::estimate_tokens(&m.content)).sum()
    }

    pub fn context(&self) -> &AgentContext { &self.context }
    pub fn context_mut(&mut self) -> &mut AgentContext { &mut self.context }
    pub fn llm(&self) -> &LlmClient { &self.llm }
    pub fn llm_mut(&mut self) -> &mut LlmClient { &mut self.llm }
}

// ─── Conversation summarizer ───

pub fn summarize_conversation(llm: &LlmClient, messages: &[AgentMessage]) -> Result<String, String> {
    let text: String = messages.iter()
        .map(|m| format!("[{}] {}", m.role, m.content))
        .collect::<Vec<_>>()
        .join("\n");

    let prompt = format!(
        "Summarize this EDA design conversation, preserving:\n\
         - Design decisions made\n\
         - Errors encountered and their fixes\n\
         - Current state of the project\n\
         - Pending tasks\n\n\
         Conversation:\n{}",
        &text[..text.len().min(8000)]
    );

    llm.generate(&prompt, Some("You are a technical summarizer. Be concise and structured."))
}

// ─── RTL Pattern Library ───

fn include_rtl_pattern_fifo() -> String {
    r#"// Synchronous FIFO with configurable depth and width
module fifo #(parameter WIDTH=8, DEPTH=16, ADDR_WIDTH=4) (
    input clk, rst_n,
    input wr_en, rd_en,
    input [WIDTH-1:0] wr_data,
    output reg [WIDTH-1:0] rd_data,
    output reg full, empty
);
    reg [WIDTH-1:0] mem [0:DEPTH-1];
    reg [ADDR_WIDTH-1:0] wr_ptr, rd_ptr;
    reg [ADDR_WIDTH:0] count;
    // ... implementation
endmodule"#.to_string()
}

fn include_rtl_pattern_fsm() -> String {
    r#"// Standard FSM pattern with 3 always blocks
module fsm_example(input clk, rst_n, input start, output reg done);
    localparam IDLE=0, WORK=1, DONE_ST=2;
    reg [1:0] state, next_state;
    // State register, next state logic, output logic
endmodule"#.to_string()
}

fn include_rtl_pattern_arbiter() -> String {
    r#"// Round-robin arbiter
module rr_arbiter #(parameter N=4) (
    input clk, rst_n,
    input [N-1:0] requests,
    output reg [N-1:0] grants
);
    reg [$clog2(N)-1:0] ptr;
    // Priority: ptr+1, ptr+2, ..., ptr
endmodule"#.to_string()
}

fn include_rtl_pattern_pipeline() -> String {
    r#"// Pipelined computation
module pipeline #(parameter STAGES=3, WIDTH=32) (
    input clk, input [WIDTH-1:0] data_in,
    output reg [WIDTH-1:0] data_out
);
    // Stage registers for pipelining
endmodule"#.to_string()
}

fn include_rtl_pattern_cdc() -> String {
    r#"// Clock domain crossing synchronizer (2-FF)
module cdc_sync #(parameter WIDTH=1) (
    input dst_clk,
    input [WIDTH-1:0] async_in,
    output reg [WIDTH-1:0] sync_out
);
    reg [WIDTH-1:0] sync_ff1;
    always @(posedge dst_clk) begin
        sync_ff1 <= async_in;
        sync_out <= sync_ff1;
    end
endmodule"#.to_string()
}

fn include_rtl_pattern_counter() -> String {
    r#"// Configurable up/down counter
module counter #(parameter WIDTH=8) (
    input clk, rst_n, up, down, load,
    input [WIDTH-1:0] load_val,
    output reg [WIDTH-1:0] count,
    output zero, max
);
    always @(posedge clk or negedge rst_n)
        if (!rst_n) count <= 0;
        else if (load) count <= load_val;
        else if (up && !max) count <= count + 1;
        else if (down && !zero) count <= count - 1;
    assign zero = (count == 0);
    assign max = (count == {WIDTH{1'b1}});
endmodule"#.to_string()
}

fn include_rtl_pattern_shift_reg() -> String {
    r#"// Shift register with parallel load
module shift_reg #(parameter WIDTH=8) (
    input clk, rst_n, shift_en, load,
    input [WIDTH-1:0] load_val, input serial_in,
    output reg [WIDTH-1:0] q, output serial_out
);
    always @(posedge clk or negedge rst_n)
        if (!rst_n) q <= 0;
        else if (load) q <= load_val;
        else if (shift_en) q <= {q[WIDTH-2:0], serial_in};
    assign serial_out = q[WIDTH-1];
endmodule"#.to_string()
}

fn include_rtl_pattern_alu() -> String {
    r#"// Simple 8-bit ALU with 4 operations
module alu #(parameter WIDTH=8) (
    input [WIDTH-1:0] a, b,
    input [1:0] op,  // 00=ADD, 01=SUB, 10=AND, 11=OR
    output reg [WIDTH-1:0] result,
    output zero, carry, overflow
);
    wire [WIDTH:0] add_result = a + b;
    wire [WIDTH:0] sub_result = a - b;
    always @(*) case(op)
        2'b00: result = add_result[WIDTH-1:0];
        2'b01: result = sub_result[WIDTH-1:0];
        2'b10: result = a & b;
        2'b11: result = a | b;
    endcase
    assign zero = (result == 0);
    assign carry = add_result[WIDTH];
    assign overflow = (op == 2'b00) ? (a[WIDTH-1] == b[WIDTH-1] && result[WIDTH-1] != a[WIDTH-1]) : 1'b0;
endmodule"#.to_string()
}

fn include_rtl_pattern_uart() -> String {
    r#"// UART Transmitter with configurable baud rate
module uart_tx #(parameter CLK_FREQ=50000000, BAUD=115200) (
    input clk, rst_n,
    input [7:0] tx_data,
    input tx_start,
    output reg tx, tx_busy
);
    localparam BIT_PERIOD = CLK_FREQ / BAUD;
    reg [15:0] bit_counter;
    reg [3:0] bit_index;
    reg [9:0] shift_reg;  // start(0) + 8 data + stop(1)
    reg sending;
    // ... TX state machine
endmodule"#.to_string()
}

fn include_rtl_pattern_spi() -> String {
    r#"// SPI Master with configurable CPOL/CPHA
module spi_master #(parameter DATA_WIDTH=8) (
    input clk, rst_n,
    input [DATA_WIDTH-1:0] tx_data,
    input tx_start,
    output reg [DATA_WIDTH-1:0] rx_data,
    output reg rx_valid, tx_busy,
    output sclk, mosi, input miso, output cs_n
);
    // Clock divider and shift register for SPI transactions
endmodule"#.to_string()
}

fn include_rtl_pattern_i2c() -> String {
    r#"// I2C Master Controller
module i2c_master (
    input clk, rst_n,
    input [6:0] slave_addr,
    input [7:0] tx_data,
    input tx_start, rw,  // rw: 0=write, 1=read
    output reg [7:0] rx_data,
    output reg rx_valid, busy,
    inout sda, output scl
);
    // I2C bit-level timing and protocol state machine
endmodule"#.to_string()
}

fn include_rtl_pattern_multiplier() -> String {
    r#"// Pipelined Multiplier (Booth encoding for signed)
module multiplier #(parameter WIDTH=8) (
    input clk, rst_n,
    input [WIDTH-1:0] a, b,
    input start,
    output reg [2*WIDTH-1:0] product,
    output reg done
);
    // Booth-2 multiplication stages
    reg [WIDTH:0] booth_reg;
    reg [2*WIDTH-1:0] partial;
    // ... Booth encoding logic
endmodule"#.to_string()
}

fn include_rtl_pattern_divider() -> String {
    r#"// Restoring Divider (sequential)
module divider #(parameter WIDTH=8) (
    input clk, rst_n,
    input [WIDTH-1:0] dividend, divisor,
    input start,
    output reg [WIDTH-1:0] quotient, remainder,
    output reg done
);
    // Non-restoring division algorithm
    reg [WIDTH-1:0] q, r;
    reg [3:0] count;
    // ... division state machine
endmodule"#.to_string()
}

fn include_rtl_pattern_memory() -> String {
    r#"// Single-Port SRAM with byte-enable
module sram #(parameter DEPTH=256, WIDTH=32) (
    input clk,
    input [$clog2(DEPTH)-1:0] addr,
    input [WIDTH-1:0] wdata,
    input [WIDTH/8-1:0] we,  // byte write enable
    input re,
    output reg [WIDTH-1:0] rdata
);
    reg [WIDTH-1:0] mem [0:DEPTH-1];
    // Read and write logic
    always @(posedge clk) begin
        if (re) rdata <= mem[addr];
        if (|we) begin
            // Byte-write logic
        end
    end
endmodule"#.to_string()
}

fn include_rtl_pattern_priority_encoder() -> String {
    r#"// Priority Encoder
module priority_encoder #(parameter N=8) (
    input [N-1:0] in,
    output reg [$clog2(N)-1:0] out,
    output reg valid
);
    integer i;
    always @(*) begin
        valid = 1'b0;
        out = 0;
        for (i = N-1; i >= 0; i = i - 1) begin
            if (in[i]) begin
                out = i;
                valid = 1'b1;
            end
        end
    end
endmodule"#.to_string()
}

fn include_rtl_pattern_gray_code() -> String {
    r#"// Gray Code Counter
module gray_counter #(parameter WIDTH=4) (
    input clk, rst_n, enable,
    output reg [WIDTH-1:0] gray_out
);
    reg [WIDTH-1:0] binary;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) binary <= 0;
        else if (enable) binary <= binary + 1;
    always @(*) gray_out = binary ^ (binary >> 1);
endmodule"#.to_string()
}

fn include_rtl_pattern_lfsr() -> String {
    r#"// LFSR (Linear Feedback Shift Register)
module lfsr #(parameter WIDTH=8, TAPS=8'b10001110) (
    input clk, rst_n, enable,
    output reg [WIDTH-1:0] lfsr_out
);
    wire feedback = ^(lfsr_out & TAPS);
    always @(posedge clk or negedge rst_n)
        if (!rst_n) lfsr_out <= {WIDTH{1'b1}};
        else if (enable) lfsr_out <= {lfsr_out[WIDTH-2:0], feedback};
endmodule"#.to_string()
}

fn include_rtl_pattern_crc() -> String {
    r#"// CRC-8 Calculator
module crc8 (
    input clk, rst_n,
    input [7:0] data_in,
    input data_valid,
    output reg [7:0] crc_out,
    output reg crc_valid
);
    reg [7:0] crc_reg;
    wire [7:0] next_crc;
    assign next_crc[0] = crc_reg[7] ^ data_in[0] ^ data_in[7];
    assign next_crc[1] = crc_reg[0] ^ crc_reg[7] ^ data_in[1] ^ data_in[7];
    assign next_crc[2] = crc_reg[1] ^ crc_reg[7] ^ data_in[2] ^ data_in[7];
    assign next_crc[7:3] = crc_reg[6:2];
    always @(posedge clk or negedge rst_n)
        if (!rst_n) begin crc_reg <= 8'hFF; crc_valid <= 0; end
        else if (data_valid) begin crc_reg <= next_crc; crc_valid <= 1; end
        else crc_valid <= 0;
    always @(*) crc_out = crc_reg;
endmodule"#.to_string()
}

fn include_rtl_pattern_dsp_mult() -> String {
    r#"// Pipelined DSP Multiplier-Accumulator (MAC)
module dsp_mac #(parameter WIDTH=16) (
    input clk, rst_n,
    input [WIDTH-1:0] a, b,
    input mac_en,
    output reg [2*WIDTH:0] result
);
    reg [WIDTH-1:0] a_d, b_d;
    reg [2*WIDTH-1:0] mult;
    // Stage 1: Input registers
    always @(posedge clk) begin a_d <= a; b_d <= b; end
    // Stage 2: Multiply
    always @(posedge clk) mult <= a_d * b_d;
    // Stage 3: Accumulate
    always @(posedge clk or negedge rst_n)
        if (!rst_n) result <= 0;
        else if (mac_en) result <= result + mult;
        else result <= mult;
endmodule"#.to_string()
}

// ─── SDC Templates ───

fn include_sdc_single_clock() -> String {
    r#"# Single clock domain SDC
create_clock -name clk -period 10.0 [get_ports clk]
set_clock_uncertainty 0.1 [get_clocks clk]
set_input_delay -clock clk -max 0.5 [all_inputs]
set_output_delay -clock clk -max 0.5 [all_outputs]"#.to_string()
}

fn include_sdc_multi_clock() -> String {
    r#"# Multi-clock domain SDC
create_clock -name clk1 -period 5.0 [get_ports clk1]
create_clock -name clk2 -period 20.0 [get_ports clk2]
set_clock_groups -asynchronous -group {clk1} -group {clk2}"#.to_string()
}

fn include_sdc_generated_clock() -> String {
    r#"# Generated clock SDC
create_clock -name clk -period 10.0 [get_ports clk]
create_generated_clock -name clk_div2 -source [get_ports clk] -divide_by 2 [get_pins divider/div2_out]"#.to_string()
}

fn include_sdc_io_constraints() -> String {
    r#"# I/O constraint SDC
set_input_delay -clock clk 0.8 [all_inputs]
set_output_delay -clock clk 1.2 [all_outputs]
set_load 0.05 [all_outputs]
set_driving_cell -lib_cell BUFX2 [all_inputs]"#.to_string()
}

fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

// ─── Tests ───

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_classification() {
        assert!(matches!(ErrorClass::classify("syntax error: unexpected token"), ErrorClass::SyntaxError));
        assert!(matches!(ErrorClass::classify("simulation FAIL at time 100ns"), ErrorClass::SimulationFail));
        assert!(matches!(ErrorClass::classify("timing violation: negative slack"), ErrorClass::TimingViolation));
        assert!(matches!(ErrorClass::classify("something completely random"), ErrorClass::Unknown));
    }

    #[test]
    fn test_knowledge_base() {
        let kb = KnowledgeBase::new();
        assert!(kb.get_rtl_pattern("fifo").is_some());
        assert!(kb.get_constraint_template("single_clock").is_some());
        assert!(!kb.design_rules.is_empty());
    }

    #[test]
    fn test_token_estimation() {
        let tokens = Agent::estimate_tokens("Hello, this is a test message with about 60 characters total.");
        assert!(tokens > 10 && tokens < 25);
    }

    #[test]
    fn test_error_strategies() {
        let strategies = ErrorClass::SyntaxError.recovery_strategies();
        assert_eq!(strategies.len(), 3);
        let strategies = ErrorClass::TimingViolation.recovery_strategies();
        assert_eq!(strategies.len(), 3);
    }
}
