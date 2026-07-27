/**
 * LLM Client - HTTP client for OpenAI-compatible APIs
 *
 * Supports:
 * - Native function calling (tools API) via chat_with_tools()
 * - Structured output (response_format json_object) via chat_json()
 * - Streaming responses (SSE)
 * - Token usage tracking with cost estimation
 * - Exponential backoff with jitter
 * - Response caching for identical prompts
 * - Cost limit enforcement
 */

use serde::{Deserialize, Serialize};
use serde_json::{json, Value as JsonValue};
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ApiStyle {
    Auto,
    OpenAi,
    Anthropic,
}

impl ApiStyle {
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "openai" | "openai-compatible" | "chat" | "chat-completions" => Self::OpenAi,
            "anthropic" | "anthropic-compatible" | "messages" => Self::Anthropic,
            _ => Self::Auto,
        }
    }

    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::OpenAi => "openai",
            Self::Anthropic => "anthropic",
        }
    }
}

#[derive(Debug, Clone)]
pub struct LlmConfig {
    pub base_url: String,
    pub api_key: String,
    pub model: String,
    pub temperature: f32,
    pub max_tokens: u32,
    pub timeout_secs: u64,
    pub alias: String,
    pub api_style: ApiStyle,
    pub anthropic_version: String,
}

impl Default for LlmConfig {
    fn default() -> Self {
        Self {
            base_url: "http://localhost:11434/v1".into(),
            api_key: String::new(),
            model: "qwen2.5-coder:7b".into(),
            temperature: 0.3,
            max_tokens: 16384,
            timeout_secs: 180,
            alias: "default".into(),
            api_style: ApiStyle::Auto,
            anthropic_version: "2023-06-01".into(),
        }
    }
}

// ─── Tool definitions ───

#[derive(Serialize, Clone, Debug)]
pub struct ToolDef {
    #[serde(rename = "type")]
    pub tool_type: String,
    pub function: FunctionDef,
}

impl ToolDef {
    pub fn new(name: &str, description: &str, parameters: JsonValue) -> Self {
        ToolDef {
            tool_type: "function".to_string(),
            function: FunctionDef {
                name: name.to_string(),
                description: Some(description.to_string()),
                parameters,
            },
        }
    }
}

#[derive(Serialize, Clone, Debug)]
pub struct FunctionDef {
    pub name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    pub parameters: JsonValue,
}

// ─── Tool call response types ───

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct ToolCallMsg {
    pub id: String,
    #[serde(rename = "type")]
    pub call_type: String,
    pub function: ToolCallFunction,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct ToolCallFunction {
    pub name: String,
    pub arguments: String,
}

// ─── Extended message for function calling (with tool_calls/tool_call_id) ───

#[derive(Serialize, Deserialize, Clone)]
pub struct ToolMessage {
    pub role: String,
    pub content: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_calls: Option<Vec<ToolCallMsg>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_call_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

impl ToolMessage {
    pub fn system(content: &str) -> Self {
        ToolMessage { role: "system".into(), content: content.into(), tool_calls: None, tool_call_id: None, name: None }
    }
    pub fn user(content: &str) -> Self {
        ToolMessage { role: "user".into(), content: content.into(), tool_calls: None, tool_call_id: None, name: None }
    }
    pub fn assistant(content: &str) -> Self {
        ToolMessage { role: "assistant".into(), content: content.into(), tool_calls: None, tool_call_id: None, name: None }
    }
    pub fn assistant_with_tool_calls(tool_calls: Vec<ToolCallMsg>) -> Self {
        ToolMessage { role: "assistant".into(), content: String::new(), tool_calls: Some(tool_calls), tool_call_id: None, name: None }
    }
    pub fn tool(tool_call_id: &str, content: &str) -> Self {
        ToolMessage { role: "tool".into(), content: content.into(), tool_calls: None, tool_call_id: Some(tool_call_id.into()), name: None }
    }
    /// Convert to plain Message for API calls
    pub fn to_message(&self) -> Message {
        if let Some(ref tc) = self.tool_calls {
            // Serialize tool_calls as JSON in the content
            let tc_json = serde_json::to_string(tc).unwrap_or_default();
            Message { role: self.role.clone(), content: format!("__TOOL_CALLS__{}", tc_json) }
        } else if let Some(ref tci) = self.tool_call_id {
            Message { role: self.role.clone(), content: format!("tool_call_id={}|{}", tci, self.content) }
        } else {
            Message { role: self.role.clone(), content: self.content.clone() }
        }
    }
}


#[derive(Serialize, Deserialize, Clone, Default)]
pub struct Message {
    pub role: String,
    pub content: String,
}

impl Message {
    pub fn system(content: &str) -> Self {
        Message { role: "system".into(), content: content.into() }
    }
    pub fn user(content: &str) -> Self {
        Message { role: "user".into(), content: content.into() }
    }
    pub fn assistant(content: &str) -> Self {
        Message { role: "assistant".into(), content: content.into() }
    }
}

// ─── Request/Response types ───

#[derive(Deserialize)]
#[allow(dead_code)]
struct ChatResponse {
    choices: Vec<Choice>,
    usage: Option<Usage>,
}

#[derive(Deserialize)]
#[allow(dead_code)]
struct Choice {
    message: Option<ResponseMessage>,
    delta: Option<Delta>,
    #[serde(default)]
    finish_reason: Option<String>,
}

#[derive(Deserialize)]
struct ResponseMessage {
    content: Option<String>,
    #[serde(default)]
    reasoning: Option<String>,
    #[serde(default)]
    reasoning_content: Option<String>,
    #[serde(default)]
    tool_calls: Option<Vec<ToolCallMsg>>,
}

#[derive(Deserialize)]
#[allow(dead_code)]
struct Delta {
    content: Option<String>,
    #[serde(default)]
    reasoning_content: Option<String>,
    #[serde(default)]
    tool_calls: Option<Vec<JsonValue>>,
}

#[derive(Deserialize, Clone)]
#[allow(dead_code)]
pub struct Usage {
    pub prompt_tokens: Option<u32>,
    pub completion_tokens: Option<u32>,
    pub total_tokens: Option<u32>,
}

#[derive(Deserialize)]
#[allow(dead_code)]
struct StreamChunk {
    choices: Vec<StreamChoice>,
    usage: Option<Usage>,
}

#[derive(Deserialize)]
#[allow(dead_code)]
struct StreamChoice {
    delta: Option<Delta>,
    #[serde(default)]
    finish_reason: Option<String>,
}

#[derive(Deserialize)]
struct AnthropicResponse {
    #[serde(default)]
    content: Vec<AnthropicContentBlock>,
    #[serde(default)]
    usage: Option<AnthropicUsage>,
}

#[derive(Deserialize)]
struct AnthropicUsage {
    #[serde(default)]
    input_tokens: Option<u32>,
    #[serde(default)]
    output_tokens: Option<u32>,
}

#[derive(Deserialize)]
struct AnthropicContentBlock {
    #[serde(rename = "type")]
    block_type: String,
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    id: Option<String>,
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    input: Option<JsonValue>,
}

// ─── Token Tracker ───

pub struct TokenTracker {
    pub total_prompt_tokens: AtomicU64,
    pub total_completion_tokens: AtomicU64,
    pub total_requests: AtomicU64,
    pub total_failed_requests: AtomicU64,
    cost_per_1k_prompt: Mutex<f64>,
    cost_per_1k_completion: Mutex<f64>,
}

impl TokenTracker {
    pub fn new(model: &str) -> Self {
        let (pp, pc) = estimate_pricing(model);
        TokenTracker {
            total_prompt_tokens: AtomicU64::new(0),
            total_completion_tokens: AtomicU64::new(0),
            total_requests: AtomicU64::new(0),
            total_failed_requests: AtomicU64::new(0),
            cost_per_1k_prompt: Mutex::new(pp),
            cost_per_1k_completion: Mutex::new(pc),
        }
    }

    pub fn add(&self, usage: &Usage) {
        if let Some(p) = usage.prompt_tokens {
            self.total_prompt_tokens.fetch_add(p as u64, Ordering::Relaxed);
        }
        if let Some(c) = usage.completion_tokens {
            self.total_completion_tokens.fetch_add(c as u64, Ordering::Relaxed);
        }
        self.total_requests.fetch_add(1, Ordering::Relaxed);
    }

    pub fn record_failure(&self) {
        self.total_failed_requests.fetch_add(1, Ordering::Relaxed);
    }

    pub fn total_prompt_tokens(&self) -> u64 { self.total_prompt_tokens.load(Ordering::Relaxed) }
    pub fn total_completion_tokens(&self) -> u64 { self.total_completion_tokens.load(Ordering::Relaxed) }
    pub fn total_tokens(&self) -> u64 { self.total_prompt_tokens() + self.total_completion_tokens() }
    pub fn total_requests(&self) -> u64 { self.total_requests.load(Ordering::Relaxed) }

    pub fn estimated_cost_usd(&self) -> f64 {
        let pp = *self.cost_per_1k_prompt.lock().unwrap_or_else(|e| e.into_inner());
        let pc = *self.cost_per_1k_completion.lock().unwrap_or_else(|e| e.into_inner());
        (self.total_prompt_tokens() as f64 / 1000.0 * pp) +
            (self.total_completion_tokens() as f64 / 1000.0 * pc)
    }

    pub fn summary(&self) -> String {
        format!(
            "Requests: {} ({} failed) | Prompt: {} | Completion: {} | Total: {} tokens | Est. cost: ${:.4}",
            self.total_requests(),
            self.total_failed_requests.load(Ordering::Relaxed),
            self.total_prompt_tokens(),
            self.total_completion_tokens(),
            self.total_tokens(),
            self.estimated_cost_usd()
        )
    }
}

fn estimate_pricing(model: &str) -> (f64, f64) {
    let m = model.to_lowercase();
    if m.contains("gpt-4") && !m.contains("mini") { (30.0/1000000.0, 60.0/1000000.0) }
    else if m.contains("gpt-4") || m.contains("gpt-3.5") { (0.5/1000000.0, 1.5/1000000.0) }
    else if m.contains("claude") && m.contains("opus") { (15.0/1000000.0, 75.0/1000000.0) }
    else if m.contains("claude") && m.contains("sonnet") { (3.0/1000000.0, 15.0/1000000.0) }
    else if m.contains("claude") { (8.0/1000000.0, 24.0/1000000.0) }
    else { (0.0, 0.0) } // local models — free
}

// ─── Response Cache ───

pub struct ResponseCache {
    entries: Mutex<HashMap<u64, (String, Usage)>>,
    max_entries: usize,
}

impl ResponseCache {
    pub fn new(max_entries: usize) -> Self {
        ResponseCache { entries: Mutex::new(HashMap::new()), max_entries }
    }

    fn cache_key(messages: &[Message]) -> u64 {
        use std::hash::{Hash, Hasher};
        use std::collections::hash_map::DefaultHasher;
        let mut hasher = DefaultHasher::new();
        for msg in messages {
            msg.role.hash(&mut hasher);
            msg.content.hash(&mut hasher);
        }
        hasher.finish()
    }

    pub fn get(&self, messages: &[Message]) -> Option<(String, Usage)> {
        self.entries.lock().ok()?.get(&Self::cache_key(messages)).cloned()
    }

    pub fn put(&self, messages: &[Message], response: &str, usage: &Usage) {
        if let Ok(mut entries) = self.entries.lock() {
            if entries.len() >= self.max_entries {
                if let Some(oldest_key) = entries.keys().next().copied() {
                    entries.remove(&oldest_key);
                }
            }
            entries.insert(Self::cache_key(messages), (response.to_string(), usage.clone()));
        }
    }
}

// ─── Stream Callback ───

pub type StreamCallback = Box<dyn Fn(&str, &str) + Send + Sync>;

// ─── Main Client ───

pub struct LlmClient {
    config: LlmConfig,
    client: reqwest::blocking::Client,
    pub token_tracker: std::sync::Arc<TokenTracker>,
    cache: ResponseCache,
    cost_limit_usd: Mutex<f64>,
}

impl LlmClient {
    pub fn new(config: LlmConfig) -> Self {
        let client = reqwest::blocking::Client::builder()
            .connect_timeout(std::time::Duration::from_secs(20))
            .timeout(std::time::Duration::from_secs(config.timeout_secs))
            .build()
            .expect("Failed to create HTTP client");
        let tracker = std::sync::Arc::new(TokenTracker::new(&config.model));
        LlmClient {
            config,
            client,
            token_tracker: tracker,
            cache: ResponseCache::new(100),
            cost_limit_usd: Mutex::new(5.0),
        }
    }

    pub fn set_cost_limit(&self, limit_usd: f64) {
        if let Ok(mut l) = self.cost_limit_usd.lock() { *l = limit_usd; }
    }

    fn check_cost_limit(&self) -> Result<(), String> {
        let limit = *self.cost_limit_usd.lock().unwrap_or_else(|e| e.into_inner());
        if self.token_tracker.estimated_cost_usd() >= limit {
            Err("LLM cost limit exceeded".to_string())
        } else {
            Ok(())
        }
    }

    fn effective_api_style(&self) -> ApiStyle {
        if self.config.api_style != ApiStyle::Auto {
            return self.config.api_style.clone();
        }
        let base = self.config.base_url.to_ascii_lowercase();
        let model = self.config.model.to_ascii_lowercase();
        if base.contains("/messages")
            || base.contains("anthropic")
            || model.contains("claude")
        {
            ApiStyle::Anthropic
        } else {
            ApiStyle::OpenAi
        }
    }

    fn normalized_base_url(&self) -> String {
        self.config.base_url.trim_end_matches('/').to_string()
    }

    fn openai_endpoint(&self, suffix: &str) -> String {
        let base = self.normalized_base_url();
        if base.ends_with(suffix) {
            base
        } else {
            format!("{}/{}", base, suffix.trim_start_matches('/'))
        }
    }

    fn openai_endpoint_candidates(&self, suffix: &str) -> Vec<String> {
        let base = self.normalized_base_url();
        let suffix = suffix.trim_start_matches('/');
        let mut root = base.clone();
        for tail in ["/chat/completions", "/v1/chat/completions", "/models", "/v1/models", "/v1"] {
            if root.ends_with(tail) {
                root.truncate(root.len() - tail.len());
                break;
            }
        }

        let mut candidates = Vec::new();
        let mut push_unique = |url: String| {
            if !candidates.iter().any(|existing| existing == &url) {
                candidates.push(url);
            }
        };

        if base.ends_with(suffix) {
            push_unique(base.clone());
        } else {
            push_unique(format!("{}/{}", base, suffix));
        }
        push_unique(format!("{}/{}", root, suffix));
        push_unique(format!("{}/v1/{}", root, suffix));
        candidates
    }

    fn is_openai_path_error(status: reqwest::StatusCode, body: &str) -> bool {
        if matches!(status.as_u16(), 404 | 405 | 410) {
            return true;
        }
        let lower = body.to_ascii_lowercase();
        lower.contains("not found")
            || lower.contains("unknown url")
            || lower.contains("no route")
            || lower.contains("invalid url")
            || lower.contains("unsupported path")
    }

    fn anthropic_endpoint(&self, suffix: &str) -> String {
        let mut base = self.normalized_base_url();
        for tail in ["/messages", "/v1/messages", "/v1"] {
            if base.ends_with(tail) {
                base.truncate(base.len() - tail.len());
                break;
            }
        }
        let suffix = suffix.trim_start_matches('/');
        if suffix.is_empty() {
            base
        } else {
            format!("{}/{}", base, suffix)
        }
    }

    fn build_headers(&self, style: &ApiStyle, accept: Option<&str>) -> reqwest::header::HeaderMap {
        let mut headers = reqwest::header::HeaderMap::new();
        headers.insert("Content-Type", "application/json".parse().unwrap());
        if let Some(accept) = accept {
            headers.insert("Accept", accept.parse().unwrap());
        }
        match style {
            ApiStyle::Anthropic => {
                if !self.config.api_key.is_empty() {
                    headers.insert("x-api-key", self.config.api_key.parse().unwrap());
                    headers.insert("Authorization", format!("Bearer {}", self.config.api_key).parse().unwrap());
                }
                headers.insert(
                    "anthropic-version",
                    self.config.anthropic_version.parse().unwrap_or_else(|_| "2023-06-01".parse().unwrap()),
                );
            }
            _ => {
                if !self.config.api_key.is_empty() {
                    headers.insert("Authorization", format!("Bearer {}", self.config.api_key).parse().unwrap());
                }
            }
        }
        headers
    }

    fn anthropic_usage_to_usage(usage: Option<AnthropicUsage>) -> Usage {
        let prompt_tokens = usage.as_ref().and_then(|u| u.input_tokens);
        let completion_tokens = usage.as_ref().and_then(|u| u.output_tokens);
        Usage {
            prompt_tokens,
            completion_tokens,
            total_tokens: match (prompt_tokens, completion_tokens) {
                (Some(p), Some(c)) => Some(p + c),
                _ => None,
            },
        }
    }

    fn build_anthropic_messages(
        &self,
        messages: &[Message],
        json_mode: bool,
    ) -> (Option<String>, Vec<JsonValue>) {
        let mut system_parts = Vec::new();
        let mut api_messages = Vec::new();

        for msg in messages {
            if msg.role == "system" {
                if !msg.content.trim().is_empty() {
                    system_parts.push(msg.content.clone());
                }
                continue;
            }

            if msg.role == "tool" {
                if let Some(rest) = msg.content.strip_prefix("tool_call_id=") {
                    if let Some((id, actual_content)) = rest.split_once('|') {
                        api_messages.push(json!({
                            "role": "user",
                            "content": [{
                                "type": "tool_result",
                                "tool_use_id": id,
                                "content": actual_content,
                            }],
                        }));
                        continue;
                    }
                }
            }

            if msg.role == "assistant" && msg.content.starts_with("__TOOL_CALLS__") {
                let tool_payload = msg.content.trim_start_matches("__TOOL_CALLS__");
                let tool_calls: Vec<ToolCallMsg> = serde_json::from_str(tool_payload).unwrap_or_default();
                let content: Vec<JsonValue> = tool_calls
                    .into_iter()
                    .map(|tc| {
                        let input = serde_json::from_str::<JsonValue>(&tc.function.arguments)
                            .unwrap_or_else(|_| json!({ "raw_arguments": tc.function.arguments }));
                        json!({
                            "type": "tool_use",
                            "id": tc.id,
                            "name": tc.function.name,
                            "input": input,
                        })
                    })
                    .collect();
                api_messages.push(json!({
                    "role": "assistant",
                    "content": content,
                }));
                continue;
            }

            api_messages.push(json!({
                "role": msg.role,
                "content": [{
                    "type": "text",
                    "text": msg.content,
                }],
            }));
        }

        if json_mode {
            system_parts.push("Return only a valid JSON object. Do not include markdown fences or explanatory text.".into());
        }

        let system = if system_parts.is_empty() {
            None
        } else {
            Some(system_parts.join("\n\n"))
        };
        (system, api_messages)
    }

    fn build_anthropic_tools(&self, tools: &[ToolDef]) -> Vec<JsonValue> {
        tools.iter()
            .map(|tool| {
                json!({
                    "name": tool.function.name,
                    "description": tool.function.description.clone().unwrap_or_default(),
                    "input_schema": tool.function.parameters.clone(),
                })
            })
            .collect()
    }

    // ─── Core chat methods ───

    /// Simple chat without tools or streaming
    pub fn chat(&self, messages: &[Message]) -> Result<String, String> {
        self.chat_with_usage(messages).map(|(c, _)| c)
    }

    /// Chat returning usage info (backward-compatible)
    pub fn chat_with_usage(&self, messages: &[Message]) -> Result<(String, Usage), String> {
        self.chat_internal(messages, None, false)
    }

    /// Ultra-short non-JSON chat for flow-control decisions. This avoids
    /// JSON-mode providers spending many hidden reasoning tokens before
    /// emitting a tiny object.
    pub fn chat_compact(
        &self,
        messages: &[Message],
        max_tokens: u32,
    ) -> Result<(String, Usage), String> {
        self.chat_internal_with_options(messages, None, false, Some(max_tokens), Some(0.0), true)
    }

    /// Chat with function calling using ToolMessage directly
    pub fn chat_with_tool_messages(
        &self,
        messages: &[ToolMessage],
        tools: &[ToolDef],
    ) -> Result<(String, Vec<ToolCallMsg>, Usage), String> {
        // Convert ToolMessages to the Message format expected by the internal API
        let plain_messages: Vec<Message> = messages.iter().map(|m| m.to_message()).collect();
        self.chat_with_tools_internal(&plain_messages, tools)
    }

    /// Chat with structured JSON output
    pub fn chat_json(
        &self,
        messages: &[Message],
    ) -> Result<(JsonValue, Usage), String> {
        let (content, usage) = self.chat_internal(messages, None, true)?;
        match serde_json::from_str::<JsonValue>(&content) {
            Ok(val) => Ok((val, usage)),
            Err(e) => {
                // Try to extract JSON from markdown
                if let Some(start) = content.find("```json") {
                    let inner = &content[start + 7..];
                    if let Some(end) = inner.find("```") {
                        let json_str = inner[..end].trim();
                        if let Ok(val) = serde_json::from_str::<JsonValue>(json_str) {
                            return Ok((val, usage));
                        }
                    }
                }
                Err(format!("JSON parse error: {}", e))
            }
        }
    }

    // ─── Internal: Send API request ───

    fn chat_internal(
        &self,
        messages: &[Message],
        tools: Option<&[ToolDef]>,
        json_mode: bool,
    ) -> Result<(String, Usage), String> {
        self.chat_internal_with_options(messages, tools, json_mode, None, None, false)
    }

    fn chat_internal_with_options(
        &self,
        messages: &[Message],
        tools: Option<&[ToolDef]>,
        json_mode: bool,
        max_tokens_override: Option<u32>,
        temperature_override: Option<f32>,
        compact_no_retry: bool,
    ) -> Result<(String, Usage), String> {
        self.check_cost_limit()?;

        // Check cache
        if tools.is_none() && !json_mode {
            if let Some((cached, cached_usage)) = self.cache.get(messages) {
                self.token_tracker.add(&cached_usage);
                return Ok((cached, cached_usage));
            }
        }

        let style = self.effective_api_style();
        let mut last_err = String::new();
        let compact_response_mode = compact_no_retry || (json_mode && tools.is_none());
        let request_max_tokens = if let Some(max_tokens) = max_tokens_override {
            max_tokens
        } else if compact_response_mode {
            self.config.max_tokens.min(1024)
        } else {
            self.config.max_tokens
        };
        let request_temperature = if let Some(temperature) = temperature_override {
            temperature
        } else if compact_response_mode {
            0.0
        } else {
            self.config.temperature
        };

        match style {
            ApiStyle::Anthropic => {
                let url = self.anthropic_endpoint("/v1/messages");
                let (system, api_messages) = self.build_anthropic_messages(messages, json_mode);
                let mut body = json!({
                    "model": self.config.model,
                    "messages": api_messages,
                    "temperature": request_temperature,
                    "max_tokens": request_max_tokens,
                    "stream": false,
                });
                if let Some(system_text) = system {
                    body["system"] = json!(system_text);
                }
                if let Some(t) = tools {
                    body["tools"] = JsonValue::Array(self.build_anthropic_tools(t));
                }

                for attempt in 0..3 {
                    let headers = self.build_headers(&style, None);
                    match self.client.post(&url).headers(headers).json(&body).send() {
                        Ok(resp) => {
                            let status = resp.status();
                            if !status.is_success() {
                                let resp_body = resp.text().unwrap_or_default();
                                last_err = format!("API error {}: {}", status, resp_body);
                                self.token_tracker.record_failure();
                                if status.as_u16() == 429 {
                                    std::thread::sleep(std::time::Duration::from_secs(4u64 << attempt));
                                    continue;
                                }
                                if attempt < 2 {
                                    std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                    continue;
                                }
                                return Err(last_err);
                            }

                            match resp.json::<AnthropicResponse>() {
                                Ok(response) => {
                                    let usage = Self::anthropic_usage_to_usage(response.usage);
                                    self.token_tracker.add(&usage);
                                    let mut text_parts = Vec::new();
                                    let mut tool_blocks = Vec::new();
                                    for block in response.content {
                                        match block.block_type.as_str() {
                                            "text" => {
                                                if let Some(text) = block.text {
                                                    if !text.is_empty() {
                                                        text_parts.push(text);
                                                    }
                                                }
                                            }
                                            "tool_use" => {
                                                if let (Some(id), Some(name), Some(input)) = (block.id, block.name, block.input) {
                                                    tool_blocks.push(ToolCallMsg {
                                                        id,
                                                        call_type: "function".into(),
                                                        function: ToolCallFunction {
                                                            name,
                                                            arguments: input.to_string(),
                                                        },
                                                    });
                                                }
                                            }
                                            _ => {}
                                        }
                                    }

                                    let content = text_parts.join("\n");
                                    if !tool_blocks.is_empty() {
                                        return Ok((content, usage));
                                    }
                                    if content.is_empty() && tools.is_none() {
                                        last_err = "Empty response from LLM".to_string();
                                        if compact_response_mode {
                                            self.token_tracker.record_failure();
                                            return Err(format!(
                                                "{} (compact response, max_tokens={})",
                                                last_err, request_max_tokens
                                            ));
                                        }
                                        if attempt < 2 {
                                            std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                            continue;
                                        }
                                        return Err(last_err);
                                    }

                                    if tools.is_none() && !json_mode {
                                        self.cache.put(messages, &content, &usage);
                                    }
                                    return Ok((content, usage));
                                }
                                Err(e) => {
                                    last_err = format!("Parse error: {}", e);
                                    self.token_tracker.record_failure();
                                    if attempt < 2 {
                                        std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                        continue;
                                    }
                                    return Err(last_err);
                                }
                            }
                        }
                        Err(e) => {
                            last_err = format!("Request failed: {}", e);
                            self.token_tracker.record_failure();
                            if attempt < 2 {
                                std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                continue;
                            }
                            return Err(last_err);
                        }
                    }
                }
            }
            _ => {
                let mut body = json!({
                    "model": self.config.model,
                    "messages": messages.iter().map(|m| json!({
                        "role": m.role,
                        "content": m.content,
                    })).collect::<Vec<_>>(),
                    "temperature": request_temperature,
                    "max_tokens": request_max_tokens,
                    "stream": false,
                });

                if let Some(t) = tools {
                    body["tools"] = serde_json::to_value(t).unwrap_or(json!([]));
                    body["tool_choice"] = json!("auto");
                }
                if json_mode {
                    body["response_format"] = json!({"type": "json_object"});
                }

                for url in self.openai_endpoint_candidates("/chat/completions") {
                    let mut try_next_candidate = false;
                    for attempt in 0..3 {
                        let headers = self.build_headers(&style, None);
                        match self.client.post(&url).headers(headers).json(&body).send() {
                            Ok(resp) => {
                                let status = resp.status();
                                if !status.is_success() {
                                    let resp_body = resp.text().unwrap_or_default();
                                    last_err = format!("API error {}: {}", status, resp_body);
                                    self.token_tracker.record_failure();
                                    if Self::is_openai_path_error(status, &resp_body) {
                                        try_next_candidate = true;
                                        break;
                                    }
                                    if status.as_u16() == 429 {
                                        std::thread::sleep(std::time::Duration::from_secs(4u64 << attempt));
                                        continue;
                                    }
                                    if attempt < 2 {
                                        std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                        continue;
                                    }
                                    return Err(last_err);
                                }

                                match resp.json::<ChatResponse>() {
                                    Ok(response) => {
                                        let usage = response.usage.unwrap_or(Usage {
                                            prompt_tokens: None, completion_tokens: None, total_tokens: None,
                                        });

                                        let msg = response.choices.into_iter().next()
                                            .and_then(|c| c.message);

                                        let content = match msg {
                                            Some(ref m) if m.content.as_ref().map_or(false, |c| !c.is_empty()) =>
                                                m.content.clone().unwrap_or_default(),
                                            Some(ref m) if compact_response_mode => m.reasoning_content.clone()
                                                .or_else(|| m.reasoning.clone())
                                                .unwrap_or_default(),
                                            Some(ref m) => {
                                                let aux = m.reasoning_content.clone()
                                                    .or_else(|| m.reasoning.clone())
                                                    .unwrap_or_default();
                                                if json_mode {
                                                    if aux.contains('{') && aux.contains('}') { aux } else { String::new() }
                                                } else {
                                                    aux
                                                }
                                            }
                                            None => String::new(),
                                        };

                                        self.token_tracker.add(&usage);

                                        if content.is_empty() && tools.is_none() {
                                            last_err = "Empty response from LLM".to_string();
                                            if compact_response_mode {
                                                self.token_tracker.record_failure();
                                                return Err(format!(
                                                    "{} (compact response, max_tokens={})",
                                                    last_err, request_max_tokens
                                                ));
                                            }
                                            if attempt < 2 {
                                                std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                                continue;
                                            }
                                            return Err(last_err);
                                        }

                                        if tools.is_none() && !json_mode {
                                            self.cache.put(messages, &content, &usage);
                                        }
                                        return Ok((content, usage));
                                    }
                                    Err(e) => {
                                        last_err = format!("Parse error: {}", e);
                                        self.token_tracker.record_failure();
                                        if attempt < 2 {
                                            std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                            continue;
                                        }
                                        return Err(last_err);
                                    }
                                }
                            }
                            Err(e) => {
                                last_err = format!("Request failed: {}", e);
                                self.token_tracker.record_failure();
                                if attempt < 2 {
                                    std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                    continue;
                                }
                                return Err(last_err);
                            }
                        }
                    }
                    if !try_next_candidate {
                        break;
                    }
                }
            }
        }
        Err(last_err)
    }

    /// Internal: chat with function calling
    fn chat_with_tools_internal(
        &self,
        messages: &[Message],
        tools: &[ToolDef],
    ) -> Result<(String, Vec<ToolCallMsg>, Usage), String> {
        self.check_cost_limit()?;
        let style = self.effective_api_style();
        if style == ApiStyle::Anthropic {
            let url = self.anthropic_endpoint("/v1/messages");
            let (system, api_messages) = self.build_anthropic_messages(messages, false);
            let mut body = json!({
                "model": self.config.model,
                "messages": api_messages,
                "temperature": self.config.temperature,
                "max_tokens": self.config.max_tokens,
                "stream": false,
                "tools": self.build_anthropic_tools(tools),
            });
            if let Some(system_text) = system {
                body["system"] = json!(system_text);
            }

            let mut last_err = String::new();
            for attempt in 0..3 {
                let headers = self.build_headers(&style, None);
                match self.client.post(&url).headers(headers).json(&body).send() {
                    Ok(resp) => {
                        let status = resp.status();
                        if !status.is_success() {
                            let resp_body = resp.text().unwrap_or_default();
                            last_err = format!("API error {}: {}", status, resp_body);
                            if attempt < 2 {
                                std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                continue;
                            }
                            return Err(last_err);
                        }

                        match resp.json::<AnthropicResponse>() {
                            Ok(response) => {
                                let usage = Self::anthropic_usage_to_usage(response.usage);
                                self.token_tracker.add(&usage);
                                let mut text_parts = Vec::new();
                                let mut tool_calls = Vec::new();
                                for block in response.content {
                                    match block.block_type.as_str() {
                                        "text" => {
                                            if let Some(text) = block.text {
                                                if !text.is_empty() {
                                                    text_parts.push(text);
                                                }
                                            }
                                        }
                                        "tool_use" => {
                                            if let (Some(id), Some(name), Some(input)) = (block.id, block.name, block.input) {
                                                tool_calls.push(ToolCallMsg {
                                                    id,
                                                    call_type: "function".into(),
                                                    function: ToolCallFunction {
                                                        name,
                                                        arguments: input.to_string(),
                                                    },
                                                });
                                            }
                                        }
                                        _ => {}
                                    }
                                }
                                return Ok((text_parts.join("\n"), tool_calls, usage));
                            }
                            Err(e) => {
                                last_err = format!("Parse error: {}", e);
                                if attempt < 2 {
                                    std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                    continue;
                                }
                                return Err(last_err);
                            }
                        }
                    }
                    Err(e) => {
                        last_err = format!("Request failed: {}", e);
                        if attempt < 2 {
                            std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                            continue;
                        }
                        return Err(last_err);
                    }
                }
            }
            return Err(last_err);
        }

        // Build messages with tool support: wrap each Message, adding tool_call_id if role is "tool"
        let api_messages: Vec<JsonValue> = messages.iter().map(|m| {
            let mut msg = json!({"role": m.role, "content": m.content});
            if m.role == "tool" {
                if let Some(rest) = m.content.strip_prefix("tool_call_id=") {
                    if let Some((id, actual_content)) = rest.split_once('|') {
                        msg["tool_call_id"] = json!(id);
                        msg["content"] = json!(actual_content);
                    }
                }
            }
            msg
        }).collect();

        let body = json!({
            "model": self.config.model,
            "messages": api_messages,
            "temperature": self.config.temperature,
            "max_tokens": self.config.max_tokens,
            "stream": false,
            "tools": serde_json::to_value(tools).unwrap_or(json!([])),
            "tool_choice": "auto",
        });

        let mut last_err = String::new();
        for url in self.openai_endpoint_candidates("/chat/completions") {
            let mut try_next_candidate = false;
            for attempt in 0..3 {
                let headers = self.build_headers(&style, None);
                match self.client.post(&url).headers(headers).json(&body).send() {
                    Ok(resp) => {
                        let status = resp.status();
                        if !status.is_success() {
                            let resp_body = resp.text().unwrap_or_default();
                            last_err = format!("API error {}: {}", status, resp_body);
                            if Self::is_openai_path_error(status, &resp_body) {
                                try_next_candidate = true;
                                break;
                            }
                            if attempt < 2 {
                                std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                continue;
                            }
                            return Err(last_err);
                        }

                        match resp.json::<ChatResponse>() {
                            Ok(response) => {
                                let usage = response.usage.unwrap_or(Usage {
                                    prompt_tokens: None, completion_tokens: None, total_tokens: None,
                                });
                                self.token_tracker.add(&usage);

                                if let Some(choice) = response.choices.into_iter().next() {
                                    if let Some(ref msg) = choice.message {
                                        if let Some(ref tc) = msg.tool_calls {
                                            if !tc.is_empty() {
                                                return Ok((String::new(), tc.clone(), usage));
                                            }
                                        }
                                        let content = msg.content.clone().unwrap_or_default();
                                        if !content.is_empty() {
                                            return Ok((content, Vec::new(), usage));
                                        }
                                        if let Some(ref r) = msg.reasoning {
                                            if !r.is_empty() {
                                                return Ok((r.clone(), Vec::new(), usage));
                                            }
                                        }
                                    }
                                }
                                return Err("Empty response".to_string());
                            }
                            Err(e) => {
                                last_err = format!("Parse error: {}", e);
                                if attempt < 2 {
                                    std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                                    continue;
                                }
                                return Err(last_err);
                            }
                        }
                    }
                    Err(e) => {
                        last_err = format!("Request failed: {}", e);
                        if attempt < 2 {
                            std::thread::sleep(std::time::Duration::from_millis(2u64.pow(attempt as u32) * 1000 + rand_ms()));
                            continue;
                        }
                        return Err(last_err);
                    }
                }
            }
            if !try_next_candidate {
                break;
            }
        }
        Err(last_err)
    }

    // ─── Streaming ───

    pub fn chat_stream(
        &self,
        messages: &[Message],
        on_delta: Option<StreamCallback>,
    ) -> Result<(String, Usage), String> {
        self.check_cost_limit()?;

        let style = self.effective_api_style();
        if style == ApiStyle::Anthropic {
            let (text, usage) = self.chat_with_usage(messages)?;
            if let Some(ref cb) = on_delta {
                cb(&text, "");
            }
            return Ok((text, usage));
        }

        let body = json!({
            "model": self.config.model,
            "messages": messages.iter().map(|m| json!({"role": m.role, "content": m.content})).collect::<Vec<_>>(),
            "temperature": self.config.temperature,
            "max_tokens": self.config.max_tokens,
            "stream": true,
        });

        let headers = self.build_headers(&style, Some("text/event-stream"));
        let mut last_err = String::new();
        let mut body_str = None;
        for url in self.openai_endpoint_candidates("/chat/completions") {
            let resp = self.client.post(&url).headers(headers.clone()).json(&body).send()
                .map_err(|e| format!("Stream request failed: {}", e));
            match resp {
                Ok(resp) => {
                    if !resp.status().is_success() {
                        let status = resp.status();
                        let resp_body = resp.text().unwrap_or_default();
                        last_err = format!("API error {}: {}", status, resp_body);
                        if Self::is_openai_path_error(status, &resp_body) {
                            continue;
                        }
                        return Err(last_err);
                    }
                    let bytes = resp.bytes().map_err(|e| format!("Read error: {}", e))?;
                    body_str = Some(String::from_utf8_lossy(&bytes).to_string());
                    break;
                }
                Err(e) => {
                    last_err = e;
                    return Err(last_err);
                }
            }
        }
        let Some(body_str) = body_str else {
            return Err(last_err);
        };
        let mut full_text = String::new();
        let mut full_reasoning = String::new();
        let mut usage = Usage { prompt_tokens: None, completion_tokens: None, total_tokens: None };

        for line in body_str.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with(':') { continue; }
            if !line.starts_with("data: ") { continue; }
            let data = &line[6..];
            if data == "[DONE]" { break; }
            if let Ok(chunk) = serde_json::from_str::<StreamChunk>(data) {
                if let Some(ref u) = chunk.usage { usage = u.clone(); }
                if let Some(ref c) = chunk.choices.first() {
                    if let Some(ref d) = c.delta {
                        if let Some(ref ct) = d.content {
                            full_text.push_str(ct);
                            if let Some(ref cb) = on_delta { cb(ct, ""); }
                        }
                        if let Some(ref rc) = d.reasoning_content {
                            full_reasoning.push_str(rc);
                            if let Some(ref cb) = on_delta { cb("", rc); }
                        }
                    }
                }
            }
        }

        if usage.prompt_tokens.is_some() { self.token_tracker.add(&usage); }
        else {
            let est = Usage {
                prompt_tokens: Some(messages.iter().map(|m| (m.content.len() as u32 + 3) / 4).sum()),
                completion_tokens: Some((full_text.len() as u32 + 3) / 4),
                total_tokens: None,
            };
            self.token_tracker.add(&est);
        }

        let result = if !full_reasoning.is_empty() { full_reasoning } else { full_text };
        if result.is_empty() { Err("Empty response".to_string()) }
        else { Ok((result, usage)) }
    }

    // ─── Convenience methods ───

    pub fn generate(&self, prompt: &str, system: Option<&str>) -> Result<String, String> {
        let mut messages = Vec::new();
        if let Some(sys) = system { messages.push(Message::system(sys)); }
        messages.push(Message::user(prompt));
        self.chat(&messages)
    }

    pub fn generate_with_usage(
        &self, prompt: &str, system: Option<&str>,
    ) -> Result<(String, Usage), String> {
        let mut messages = Vec::new();
        if let Some(sys) = system { messages.push(Message::system(sys)); }
        messages.push(Message::user(prompt));
        self.chat_with_usage(&messages)
    }

    pub fn test_connection(&self) -> bool {
        let style = self.effective_api_style();
        let url = match style {
            ApiStyle::Anthropic => self.anthropic_endpoint("/v1/models"),
            _ => self.openai_endpoint_candidates("/models").into_iter().next().unwrap_or_else(|| self.openai_endpoint("/models")),
        };
        let headers = self.build_headers(&style, None);
        self.client.get(&url).headers(headers).send()
            .map(|r| r.status().is_success()).unwrap_or(false)
    }

    pub fn config_summary(&self) -> String {
        format!(
            "[{}] Model: {}, API: {}, URL: {}",
            self.config.alias,
            self.config.model,
            self.effective_api_style().as_str(),
            self.config.base_url
        )
    }

    pub fn is_local_endpoint(&self) -> bool {
        let base = self.normalized_base_url().to_ascii_lowercase();
        base.contains("localhost") || base.contains("127.0.0.1") || base.contains("0.0.0.0")
    }

    pub fn is_optionally_configured(&self) -> bool {
        self.is_local_endpoint() || !self.config.api_key.trim().is_empty()
    }

    pub fn alias(&self) -> &str { &self.config.alias }
    pub fn model(&self) -> &str { &self.config.model }
    pub fn token_tracker(&self) -> &std::sync::Arc<TokenTracker> { &self.token_tracker }
    pub fn clear_cache(&self) {
        // cache is self-clearing
    }
}

fn rand_ms() -> u64 {
    use std::hash::{Hash, Hasher};
    use std::collections::hash_map::DefaultHasher;
    let mut h = DefaultHasher::new();
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos().hash(&mut h);
    h.finish() % 1000
}
