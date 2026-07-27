/**
 * Project Manager - Per-project workspace, history, and switching
 */

use std::fs;
use std::path::{Path, PathBuf};
use serde::{Deserialize, Serialize};

/// Project metadata
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProjectMeta {
    pub name: String,
    pub created: String,
    pub last_active: String,
    pub modules: Vec<String>,
    pub description: Option<String>,
}

/// Project manager for workspace operations
pub struct ProjectManager {
    workspace: PathBuf,
    current_project: Option<PathBuf>,
    current_meta: Option<ProjectMeta>,
}

/// Full project configuration with settings persistence for cross-session restore
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ProjectConfig {
    pub project_name: String,
    pub top_module: Option<String>,
    pub clock_frequency_mhz: f64,
    pub technology: Option<String>,
    pub liberty_file: Option<String>,
    pub sdc_file: Option<String>,
    pub max_cycles: i32,
    pub opt_level: i32,
    pub enable_timing: bool,
    pub enable_power: bool,
    pub enable_formal: bool,
    pub last_rtl_code: Option<String>,
    pub last_synth_info: Option<SynthSnapshot>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SynthSnapshot {
    pub cell_count: usize,
    pub dff_count: usize,
    pub area_ge: f64,
    pub logic_depth: i32,
    pub max_frequency_mhz: f64,
}

impl Default for ProjectConfig {
    fn default() -> Self {
        ProjectConfig {
            project_name: "default".to_string(),
            top_module: None,
            clock_frequency_mhz: 100.0,
            technology: None,
            liberty_file: None,
            sdc_file: None,
            max_cycles: 1000,
            opt_level: 2,
            enable_timing: true,
            enable_power: true,
            enable_formal: true,
            last_rtl_code: None,
            last_synth_info: None,
        }
    }
}

impl ProjectManager {
    /// Save full project configuration for cross-session persistence
    pub fn save_config(&self, config: &ProjectConfig) -> Result<(), String> {
        let project_dir = self.current_project.as_ref()
            .ok_or("No project loaded")?;
        let config_path = project_dir.join("config.json");
        let json = serde_json::to_string_pretty(config)
            .map_err(|e| format!("Failed to serialize config: {}", e))?;
        fs::write(&config_path, json)
            .map_err(|e| format!("Failed to write config: {}", e))?;
        Ok(())
    }

    /// Load project configuration from a previous session
    pub fn load_config(&self) -> Option<ProjectConfig> {
        let project_dir = self.current_project.as_ref()?;
        let config_path = project_dir.join("config.json");
        if config_path.exists() {
            let content = fs::read_to_string(&config_path).ok()?;
            serde_json::from_str::<ProjectConfig>(&content).ok()
        } else {
            Some(ProjectConfig::default())
        }
    }

    /// Open an existing project directory in-place.
    pub fn open_project_dir(&mut self, project_dir: &Path) -> Result<PathBuf, String> {
        if !project_dir.exists() {
            return Err(format!("Project directory not found: {}", project_dir.display()));
        }
        if !project_dir.is_dir() {
            return Err(format!("Not a directory: {}", project_dir.display()));
        }

        let project_dir = project_dir
            .canonicalize()
            .unwrap_or_else(|_| project_dir.to_path_buf());

        for dir in ["src", "tb", "sdc", "syn", "sim", "formal", "history", "logs", "report", "exchange"] {
            fs::create_dir_all(project_dir.join(dir))
                .map_err(|e| format!("Failed to create {}: {}", dir, e))?;
        }

        let name = project_dir
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("project")
            .to_string();

        let meta_path = project_dir.join("project.json");
        let meta = if meta_path.exists() {
            let content = fs::read_to_string(&meta_path)
                .map_err(|e| format!("Failed to read metadata: {}", e))?;
            serde_json::from_str::<ProjectMeta>(&content)
                .unwrap_or(ProjectMeta {
                    name: name.clone(),
                    created: self.current_timestamp(),
                    last_active: self.current_timestamp(),
                    modules: Vec::new(),
                    description: None,
                })
        } else {
            ProjectMeta {
                name: name.clone(),
                created: self.current_timestamp(),
                last_active: self.current_timestamp(),
                modules: Vec::new(),
                description: None,
            }
        };

        let mut updated_meta = meta.clone();
        updated_meta.last_active = self.current_timestamp();
        let meta_json = serde_json::to_string_pretty(&updated_meta)
            .map_err(|e| format!("Failed to serialize metadata: {}", e))?;
        fs::write(&meta_path, meta_json)
            .map_err(|e| format!("Failed to write metadata: {}", e))?;

        self.current_project = Some(project_dir.clone());
        self.current_meta = Some(updated_meta);
        Ok(project_dir)
    }

    pub fn new(workspace: PathBuf) -> Self {
        fs::create_dir_all(&workspace).ok();
        ProjectManager {
            workspace,
            current_project: None,
            current_meta: None,
        }
    }

    /// List all projects in workspace
    pub fn list_projects(&self) -> Vec<ProjectMeta> {
        let mut projects = Vec::new();
        if let Ok(entries) = fs::read_dir(&self.workspace) {
            for entry in entries.flatten() {
                if entry.path().is_dir() {
                    let name = entry.file_name().to_string_lossy().to_string();
                    if name.starts_with('.') || name == "logs" { continue; }
                    let meta_path = entry.path().join("project.json");
                    if meta_path.exists() {
                        if let Ok(content) = fs::read_to_string(&meta_path) {
                            if let Ok(meta) = serde_json::from_str::<ProjectMeta>(&content) {
                                projects.push(meta);
                            }
                        }
                    } else if entry.path().join("src").exists() {
                        // Legacy project without metadata - create one
                        let meta = ProjectMeta {
                            name: name.clone(),
                            created: String::new(),
                            last_active: String::new(),
                            modules: Vec::new(),
                            description: None,
                        };
                        projects.push(meta);
                    }
                }
            }
        }
        projects.sort_by(|a, b| a.name.cmp(&b.name));
        projects
    }

    /// Create a new project with full directory structure
    pub fn create_project(&mut self, name: &str) -> Result<PathBuf, String> {
        let project_dir = self.workspace.join(name);
        if project_dir.exists() {
            return Err(format!("Project '{}' already exists", name));
        }

        // Create directory structure
        let dirs = ["src", "tb", "sdc", "syn", "sim", "formal", "history"];
        for dir in &dirs {
            fs::create_dir_all(project_dir.join(dir))
                .map_err(|e| format!("Failed to create {}: {}", dir, e))?;
        }

        // Create project metadata
        let meta = ProjectMeta {
            name: name.to_string(),
            created: self.current_timestamp(),
            last_active: self.current_timestamp(),
            modules: Vec::new(),
            description: None,
        };
        let meta_json = serde_json::to_string_pretty(&meta)
            .map_err(|e| format!("Failed to serialize metadata: {}", e))?;
        fs::write(project_dir.join("project.json"), meta_json)
            .map_err(|e| format!("Failed to write metadata: {}", e))?;

        // Create empty conversation history
        fs::write(project_dir.join("history").join("conversation.jsonl"), "")
            .ok();

        self.current_project = Some(project_dir.clone());
        self.current_meta = Some(meta);

        Ok(project_dir)
    }

    /// Load an existing project
    pub fn load_project(&mut self, name: &str) -> Result<PathBuf, String> {
        let project_dir = self.workspace.join(name);
        if !project_dir.exists() {
            return Err(format!("Project '{}' not found", name));
        }
        self.open_project_dir(&project_dir)
    }

    /// Get current project directory
    pub fn current_project(&self) -> Option<&Path> {
        self.current_project.as_deref()
    }

    /// Get current project name
    pub fn current_name(&self) -> Option<String> {
        self.current_meta.as_ref().map(|m| m.name.clone())
    }

    /// Save conversation history for current project
    pub fn save_conversation(&self, messages: &[crate::llm::Message]) -> Result<(), String> {
        let project_dir = self.current_project.as_ref()
            .ok_or("No project loaded")?;

        let history_dir = project_dir.join("history");
        fs::create_dir_all(&history_dir).ok();

        let conv_path = history_dir.join("conversation.jsonl");
        let mut content = String::new();
        for msg in messages {
            let line = serde_json::to_string(msg).unwrap_or_default();
            content.push_str(&line);
            content.push('\n');
        }
        fs::write(&conv_path, content)
            .map_err(|e| format!("Failed to save conversation: {}", e))?;
        Ok(())
    }

    /// Load conversation history for current project
    pub fn load_conversation(&self) -> Vec<crate::llm::Message> {
        let Some(project_dir) = &self.current_project else {
            return Vec::new();
        };

        let conv_path = project_dir.join("history").join("conversation.jsonl");
        if !conv_path.exists() {
            return Vec::new();
        }

        let content = fs::read_to_string(&conv_path).unwrap_or_default();
        let mut messages = Vec::new();
        for line in content.lines() {
            if line.trim().is_empty() { continue; }
            if let Ok(msg) = serde_json::from_str::<crate::llm::Message>(line) {
                messages.push(msg);
            }
        }
        messages
    }

    /// Add a module to the current project's metadata
    pub fn add_module(&mut self, module_name: &str) {
        if let Some(ref mut meta) = self.current_meta {
            if !meta.modules.contains(&module_name.to_string()) {
                meta.modules.push(module_name.to_string());
                // Save updated metadata
                if let Some(ref project_dir) = self.current_project {
                    let meta_json = serde_json::to_string_pretty(meta).unwrap_or_default();
                    fs::write(project_dir.join("project.json"), meta_json).ok();
                }
            }
        }
    }

    /// Get list of modules in current project
    #[allow(dead_code)]
    pub fn modules(&self) -> Vec<String> {
        self.current_meta.as_ref()
            .map(|m| m.modules.clone())
            .unwrap_or_default()
    }

    /// Scan project src/ directory for all .v files and extract module names
    pub fn scan_modules(&self) -> Vec<String> {
        let Some(project_dir) = &self.current_project else {
            return Vec::new();
        };

        let src_dir = project_dir.join("src");
        let mut modules = Vec::new();

        if src_dir.exists() {
            if let Ok(entries) = fs::read_dir(&src_dir) {
                for entry in entries.flatten() {
                    let name = entry.file_name().to_string_lossy().to_string();
                    if name.ends_with(".v") || name.ends_with(".sv") {
                        if let Ok(content) = fs::read_to_string(entry.path()) {
                            // Extract module names from this file
                            for line in content.lines() {
                                let trimmed = line.trim();
                                if trimmed.starts_with("module ") {
                                    let rest = &trimmed["module ".len()..];
                                    let mod_name: String = rest.chars()
                                        .take_while(|c| !c.is_whitespace() && *c != '(' && *c != '#')
                                        .collect();
                                    if !mod_name.is_empty() {
                                        modules.push(mod_name);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        modules
    }

    /// Get src directory for current project
    pub fn src_dir(&self) -> Option<PathBuf> {
        self.current_project.as_ref().map(|p| p.join("src"))
    }

    /// Get subdirectory for current project
    #[allow(dead_code)]
    pub fn subdir(&self, name: &str) -> Option<PathBuf> {
        self.current_project.as_ref().map(|p| p.join(name))
    }

    fn current_timestamp(&self) -> String {
        // Simple timestamp without external deps
        format!("{:?}", std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs())
    }
}
