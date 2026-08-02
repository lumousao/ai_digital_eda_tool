/**
 * AI RTL Simulator - Interactive CLI
 *
 * An agent-driven tool for RTL generation, simulation, and synthesis.
 * Uses LLM to generate Verilog code, then validates with lint and synthesis.
 *
 * The synthesis, simulation, timing, formal, and power engines are compiled
 * from this repository's local C++ sources and invoked through local FFI.
 */

mod agent;
mod apr;
mod data_api;
mod engine;
mod gui;
mod gui_exchange;
mod layout;
mod llm;
mod project;
mod repl;
mod tech;
mod terminal;

use clap::Parser;
use colored::Colorize;
use std::io::{self, Write};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "ai_digital")]
#[command(about = "AI-driven RTL generation, simulation, and synthesis tool")]
#[command(version)]
struct Cli {
    /// Path to config YAML file
    #[arg(short, long, default_value = "config.yaml")]
    config: PathBuf,

    /// LLM base URL
    #[arg(long)]
    llm_url: Option<String>,

    /// LLM model name
    #[arg(long)]
    llm_model: Option<String>,

    /// LLM API key
    #[arg(long)]
    llm_key: Option<String>,

    /// Generate RTL from description (non-interactive mode)
    #[arg(short, long)]
    describe: Option<String>,

    /// Top module name
    #[arg(long, default_value = "top")]
    top: String,

    /// Existing RTL file to lint/synthesize
    #[arg(short, long)]
    rtl: Option<PathBuf>,

    /// Run lint check
    #[arg(short, long)]
    lint: bool,

    /// Run synthesis
    #[arg(short, long)]
    synthesize: bool,

    /// Check tool availability
    #[arg(long)]
    check: bool,

    /// Start GUI mode (Tcl/Tk desktop application)
    #[arg(short = 'G', long = "gui")]
    gui: bool,

    /// Technology/process selection (e.g., "ics55_LLSC_H9CR")
    #[arg(short = 't', long)]
    technology: Option<String>,

    /// Liberty library directory (default: libs/)
    #[arg(long, default_value = "libs")]
    lib_dir: PathBuf,

    /// Verbose output: show full content without folding
    #[arg(short = 'v', long)]
    verbose: bool,

    /// Pipe mode: read commands from stdin line-by-line, print plain output.
    /// Used by the GUI to embed the CLI interactive session over a pipe.
    /// Disables colors, spinners, and raw-mode terminal handling.
    #[arg(long)]
    pipe: bool,
}

fn find_liberty_lib(technology: &Option<String>, lib_dir: &PathBuf) -> Option<String> {
    use crate::tech::CornerDatabase;
    let mut db = CornerDatabase::auto_detect(lib_dir);
    if let Some(ref tech) = technology {
        db.set_active_process(tech).ok()?;
    }
    db.get_default_liberty().map(|p| p.to_string_lossy().to_string())
}

fn load_config(path: &PathBuf) -> llm::LlmConfig {
    let mut config = llm::LlmConfig::default();
    let mut apis: std::collections::HashMap<String, llm::LlmConfig> = std::collections::HashMap::new();
    let mut api_order: Vec<String> = Vec::new(); // Track insertion order
    let mut active_api = String::new();
    let mut current_api_alias = String::new();
    let mut current_api_config = llm::LlmConfig::default();

    if path.exists() {
        if let Ok(content) = std::fs::read_to_string(path) {
            let mut in_apis = false;

            for line in content.lines() {
                let trimmed = line.trim();
                if trimmed.is_empty() || trimmed.starts_with('#') {
                    continue;
                }

                // Detect "apis:" section
                if trimmed == "apis:" {
                    in_apis = true;
                    continue;
                }

                // Detect top-level keys (exit apis section)
                // Check original line for indentation before trimming
                if in_apis && !line.starts_with(' ') && !line.starts_with('\t') {
                    in_apis = false;
                    // Save previous API if any
                    if !current_api_alias.is_empty() {
                        current_api_config.alias = current_api_alias.clone();
                        apis.insert(current_api_alias.clone(), current_api_config.clone());
                        api_order.push(current_api_alias.clone());
                        current_api_alias.clear();
                        current_api_config = llm::LlmConfig::default();
                    }
                }

                if in_apis {
                    // Check if this is a new API alias (indented exactly 2 spaces, ending with ':')
                    let indent_count = line.chars().take_while(|c| *c == ' ').count();
                    let stripped = trimmed.trim_end_matches(':');
                    if trimmed.ends_with(':') && indent_count == 2 {
                        // Save previous API if any
                        if !current_api_alias.is_empty() {
                            current_api_config.alias = current_api_alias.clone();
                            apis.insert(current_api_alias.clone(), current_api_config.clone());
                            api_order.push(current_api_alias.clone());
                        }
                        current_api_alias = stripped.to_string();
                        current_api_config = llm::LlmConfig::default();
                        continue;
                    }

                    // Parse API config fields (indented 4 spaces)
                    if let Some((key, value)) = trimmed.split_once(':') {
                        let key = key.trim();
                        let value = value.trim().trim_matches('"').trim_matches('\'');
                        match key {
                            "base_url" => current_api_config.base_url = value.to_string(),
                            "api_key" => current_api_config.api_key = value.to_string(),
                            "model" => current_api_config.model = value.to_string(),
                            "api_style" => current_api_config.api_style = llm::ApiStyle::parse(value),
                            "anthropic_version" => current_api_config.anthropic_version = value.to_string(),
                            "temperature" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.temperature = t;
                                }
                            }
                            "max_tokens" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.max_tokens = t;
                                }
                            }
                            "timeout" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.timeout_secs = t;
                                }
                            }
                            _ => {}
                        }
                    }
                } else {
                    // Top-level keys
                    if let Some((key, value)) = trimmed.split_once(':') {
                        let key = key.trim();
                        let value = value.trim().trim_matches('"').trim_matches('\'');
                        if key == "active_api" {
                            active_api = value.to_string();
                        }
                    }
                }
            }

            // Save last API if in apis section
            if in_apis && !current_api_alias.is_empty() {
                current_api_config.alias = current_api_alias.clone();
                apis.insert(current_api_alias.clone(), current_api_config.clone());
                api_order.push(current_api_alias.clone());
            }
        }
    }

    // Select the active API (prefer explicit active_api, then first in config order)
    if !apis.is_empty() {
        if let Some(api_config) = apis.get(&active_api) {
            config = api_config.clone();
        } else if let Some(first_alias) = api_order.first() {
            if let Some(api_config) = apis.get(first_alias) {
                config = api_config.clone();
            }
        }
    }

    config
}

/// Load all API profiles from config
fn load_all_apis(path: &PathBuf) -> std::collections::HashMap<String, llm::LlmConfig> {
    let mut apis: std::collections::HashMap<String, llm::LlmConfig> = std::collections::HashMap::new();
    let mut current_api_alias = String::new();
    let mut current_api_config = llm::LlmConfig::default();

    if path.exists() {
        if let Ok(content) = std::fs::read_to_string(path) {
            let mut in_apis = false;

            for line in content.lines() {
                let trimmed = line.trim();
                if trimmed.is_empty() || trimmed.starts_with('#') {
                    continue;
                }

                if trimmed == "apis:" {
                    in_apis = true;
                    continue;
                }

                // Detect top-level keys (exit apis section)
                if in_apis && !line.starts_with(' ') && !line.starts_with('\t') {
                    in_apis = false;
                    if !current_api_alias.is_empty() {
                        current_api_config.alias = current_api_alias.clone();
                        apis.insert(current_api_alias.clone(), current_api_config.clone());
                        current_api_alias.clear();
                        current_api_config = llm::LlmConfig::default();
                    }
                }

                if in_apis {
                    // Check if this is a new API alias (indented exactly 2 spaces, ending with ':')
                    let indent_count = line.chars().take_while(|c| *c == ' ').count();
                    let stripped = trimmed.trim_end_matches(':');
                    if trimmed.ends_with(':') && indent_count == 2 {
                        if !current_api_alias.is_empty() {
                            current_api_config.alias = current_api_alias.clone();
                            apis.insert(current_api_alias.clone(), current_api_config.clone());
                        }
                        current_api_alias = stripped.to_string();
                        current_api_config = llm::LlmConfig::default();
                        continue;
                    }

                    // Parse API config fields (indented 4 spaces)
                    if let Some((key, value)) = trimmed.split_once(':') {
                        let key = key.trim();
                        let value = value.trim().trim_matches('"').trim_matches('\'');
                        match key {
                            "base_url" => current_api_config.base_url = value.to_string(),
                            "api_key" => current_api_config.api_key = value.to_string(),
                            "model" => current_api_config.model = value.to_string(),
                            "api_style" => current_api_config.api_style = llm::ApiStyle::parse(value),
                            "anthropic_version" => current_api_config.anthropic_version = value.to_string(),
                            "temperature" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.temperature = t;
                                }
                            }
                            "max_tokens" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.max_tokens = t;
                                }
                            }
                            "timeout" => {
                                if let Ok(t) = value.parse() {
                                    current_api_config.timeout_secs = t;
                                }
                            }
                            _ => {}
                        }
                    }
                }
            }

            if in_apis && !current_api_alias.is_empty() {
                current_api_config.alias = current_api_alias.clone();
                apis.insert(current_api_alias.clone(), current_api_config.clone());
            }
        }
    }

    apis
}

/// Interactive initial setup when no config file exists
fn initial_setup(config_path: &PathBuf) -> llm::LlmConfig {

    println!("{}", "╔══════════════════════════════════════════╗".bright_cyan());
    println!("{}", "║       AI RTL Simulator - Initial Setup   ║".bright_cyan());
    println!("{}", "╚══════════════════════════════════════════╝".bright_cyan());
    println!();
    println!("  No config file found at: {}", config_path.display());
    println!("  Let's set up your configuration.");
    println!();

    let mut config = llm::LlmConfig::default();
    let mut buf = String::new();

    // Base URL
    print!("  {} LLM Base URL [{}]: ", "→".blue(), config.base_url);
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() { config.base_url = val; }

    // API Key
    print!("  {} API Key [{}]: ", "→".blue(), if config.api_key.is_empty() { "(empty)" } else { "(set)" });
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() { config.api_key = val; }

    // Model
    print!("  {} Model [{}]: ", "→".blue(), config.model);
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() { config.model = val; }

    // Temperature
    print!("  {} Temperature [{}]: ", "→".blue(), config.temperature);
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() {
        if let Ok(t) = val.parse() { config.temperature = t; }
    }

    // Max tokens
    print!("  {} Max tokens [{}]: ", "→".blue(), config.max_tokens);
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() {
        if let Ok(t) = val.parse() { config.max_tokens = t; }
    }

    // Timeout
    print!("  {} Timeout (seconds) [{}]: ", "→".blue(), config.timeout_secs);
    io::stdout().flush().ok();
    buf.clear();
    io::stdin().read_line(&mut buf).ok();
    let val = buf.trim().to_string();
    if !val.is_empty() {
        if let Ok(t) = val.parse() { config.timeout_secs = t; }
    }

    // Write config file
    println!();
    let yaml_content = format!(
        r#"# ai_digital Configuration
# ================================

# Multiple API profiles - first one is default
apis:
  default:
    base_url: "{}"
    api_key: "{}"
    model: "{}"
    api_style: "{}"
    anthropic_version: "{}"
    temperature: {}
    max_tokens: {}
    timeout: {}

# Active API alias (must match a key in apis)
active_api: "default"
"#,
        config.base_url, config.api_key, config.model,
        config.api_style.as_str(), config.anthropic_version,
        config.temperature, config.max_tokens, config.timeout_secs
    );

    if let Err(e) = std::fs::write(config_path, &yaml_content) {
        println!("  {} Failed to write config: {}", "✗".red(), e);
    } else {
        println!("  {} Config saved to: {}", "✓".green(), config_path.display());
    }

    println!();
    config
}

fn main() {
    let cli = Cli::parse();

    // GUI mode: launch Tcl/Tk desktop GUI
    if cli.gui {
        let _ = gui::run();
        return;
    }

    // Pipe mode: embed the CLI as a subprocess (used by the GUI).
    // Read commands from stdin line-by-line, emit plain output, no colors/spinners.
    if cli.pipe {
        run_pipe_mode(&cli);
        return;
    }

    // Check if config file exists, if not enter initial setup
    // BUT if user passed --synthesize/--lint/--rtl/--describe, skip setup and use defaults
    let has_explicit_action = cli.describe.is_some() || cli.rtl.is_some() || cli.lint || cli.synthesize;
    if !cli.config.exists() && !cli.check && !has_explicit_action {
        let config = initial_setup(&cli.config);

        // Test connection
        println!("  {} Testing API connection...", "▶".blue());
        let client = llm::LlmClient::new(config.clone());
        if client.test_connection() {
            println!("  {} Connection successful!", "✓".green());
        } else {
            println!("  {} Connection failed (check your API key and URL)", "⚠".yellow());
            println!("  {} You can re-run to reconfigure, or edit: {}", "  ".dimmed(), cli.config.display());
        }
        println!();

        // Start interactive mode
        let all_apis = load_all_apis(&cli.config);
        let corner_db = tech::CornerDatabase::auto_detect(&cli.lib_dir);
        let mut repl = repl::Repl::new(config, all_apis, cli.config.clone(), corner_db);
        repl.run();
        return;
    }

    let mut llm_config = load_config(&cli.config);

    // Override with CLI arguments
    if let Some(url) = &cli.llm_url {
        llm_config.base_url = url.clone();
    }
    if let Some(model) = &cli.llm_model {
        llm_config.model = model.clone();
    }
    if let Some(key) = &cli.llm_key {
        llm_config.api_key = key.clone();
    }

    // Check mode
    if cli.check {
        println!("{}", "Tool Check:".bright_cyan().bold());
        println!("  Engine: {}", engine::version());
        println!("  LLM: {}", llm::LlmClient::new(llm_config).config_summary());

        // Technology corner detection
        let corner_db = tech::CornerDatabase::auto_detect(&cli.lib_dir);
        if !corner_db.processes.is_empty() {
            println!();
            println!("{}", "Technology Libraries:".bright_cyan().bold());
            println!("{}", corner_db.summary());
        }
        return;
    }

    // Non-interactive mode
    if let Some(description) = &cli.describe {
        println!("{}", "Non-interactive mode:".bright_cyan());
        println!("  Description: {}", description);
        println!("  Top module: {}", cli.top);
        println!();

        let design = engine::Design::new();
        let llm = llm::LlmClient::new(llm_config);

        // Generate RTL
        println!("  {} Generating RTL...", "▶".blue());
        let prompt = format!(
            "Generate synthesizable Verilog RTL for: {}\n\nOutput ONLY the Verilog code in a ```verilog block.",
            description
        );
        match llm.generate(&prompt, Some(repl::SYSTEM_RTL_GEN)) {
            Ok(response) => {
                if let Some(code) = repl::extract_verilog(&response) {
                    let filename = format!("{}.v", cli.top);
                    std::fs::write(&filename, &code).expect("Failed to write file");
                    println!("  {} Saved: {}", "✓".green(), filename);

                    if cli.lint || cli.synthesize {
                        let _ = design.parse_str(&code, &cli.top);
                    }

                    if cli.lint {
                        println!("  {} Lint checking...", "▶".blue());
                        let result = design.lint_check(&cli.top);
                        if result.passed {
                            println!("  {} Lint passed", "✓".green());
                        } else {
                            println!("  {} Lint failed", "✗".red());
                            println!("{}", result.report);
                        }
                    }

                    if cli.synthesize {
                        println!("  {} Synthesizing...", "▶".blue());
                        let lib_path = find_liberty_lib(&cli.technology, &cli.lib_dir);
                        let synth = if let Some(ref lib) = lib_path {
                            engine::synthesize_real_with_lib(&code, &cli.top, Some(lib))
                        } else {
                            engine::synthesize_real(&code, &cli.top)
                        };
                        if synth.success {
                            println!("  {} Synthesis completed", "✓".green());
                            println!("{}", synth.report);
                            // Save gate-level netlist
                            let gate_path = format!("{}_synth_gate.v", cli.top);
                            std::fs::write(&gate_path, &synth.gate_verilog).ok();
                            println!("  {} Gate netlist: {}", "✓".green(), gate_path);
                        } else {
                            println!("  {} Synthesis failed: {}", "✗".red(), synth.error);
                        }
                    }
                } else {
                    println!("  {} No Verilog code found in LLM response", "⚠".yellow());
                    // Foldable display: show first 5 lines, rest folded unless --verbose
                    let lines: Vec<&str> = response.lines().collect();
                    if cli.verbose || lines.len() <= 5 {
                        println!("  Response:\n{}", response);
                    } else {
                        println!("  Response ({} lines, {}use --verbose to expand):", lines.len(), "┄ ".dimmed());
                        for line in lines.iter().take(5) {
                            println!("    {}", line);
                        }
                        println!("    {} ({} more lines folded)", "...".dimmed(), lines.len() - 5);
                    }
                }
            }
            Err(e) => println!("  {} LLM error: {}", "✗".red(), e),
        }
        return;
    }

    // Existing RTL mode
    if let Some(rtl_path) = &cli.rtl {
        let code = std::fs::read_to_string(rtl_path).expect("Failed to read RTL file");
        let design = engine::Design::new();
        let module_name = cli.top.clone();

        let _ = design.parse_str(&code, &module_name);

        if cli.lint {
            println!("  {} Lint checking...", "▶".blue());
            let result = design.lint_check(&module_name);
            if result.passed {
                println!("  {} Lint passed", "✓".green());
            } else {
                println!("  {} Lint failed", "✗".red());
                println!("{}", result.report);
            }
        }

        if cli.synthesize {
            println!("  {} Synthesizing...", "▶".blue());
            let lib_path = find_liberty_lib(&cli.technology, &cli.lib_dir);
            let synth = if let Some(ref lib) = lib_path {
                engine::synthesize_real_with_lib(&code, &module_name, Some(lib))
            } else {
                engine::synthesize_real(&code, &module_name)
            };
            if synth.success {
                println!("  {} Synthesis completed", "✓".green());
                println!("{}", synth.report);
                let gate_path = format!("{}_synth_gate.v", module_name);
                std::fs::write(&gate_path, &synth.gate_verilog).ok();
                println!("  {} Gate netlist: {}", "✓".green(), gate_path);
            } else {
                println!("  {} Synthesis failed: {}", "✗".red(), synth.error);
            }
        }

        if !cli.lint && !cli.synthesize {
            println!("  RTL loaded: {} ({} wires, {} cells)", module_name,
                design.summary().len(), 0);
            println!("  Use --lint or --synthesize to run checks");
        }
        return;
    }

    // Interactive mode (default)
    let all_apis = load_all_apis(&cli.config);
    let mut corner_db = tech::CornerDatabase::auto_detect(&cli.lib_dir);
    if let Some(ref tech_name) = cli.technology {
        corner_db.set_active_process(tech_name).ok();
    }
    let mut repl = repl::Repl::new(llm_config, all_apis, cli.config.clone(), corner_db);
    repl.run();
}

/// Pipe mode: embed the CLI as a subprocess (used by GUI).
/// Reads commands from stdin line-by-line, outputs plain text without ANSI codes.
fn run_pipe_mode(cli: &Cli) {
    let llm_config = load_config(&cli.config);
    let all_apis = load_all_apis(&cli.config);
    let mut corner_db = tech::CornerDatabase::auto_detect(&cli.lib_dir);
    if let Some(ref tech_name) = cli.technology {
        corner_db.set_active_process(tech_name).ok();
    }
    let mut repl = repl::Repl::new(llm_config, all_apis, cli.config.clone(), corner_db);
    repl.run_pipe();
}
