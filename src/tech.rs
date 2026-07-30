/**
 * Technology library discovery and corner metadata.
 *
 * Each direct child of libs/ is one technology. Every Liberty file below that
 * directory is one analysis corner. The directory name is authoritative; file
 * names and Liberty headers are only used to describe and rank corners.
 */

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use serde::Serialize;
use serde_json::Value;

const SKY130_PROCESS: &str = "sky130_fd_sc_hd";
const SKY130_REPRESENTATIVE_CORNERS: [(&str, f64, f64); 5] = [
    ("tt_025C_1v80", 1.80, 25.0),
    ("tt_100C_1v80", 1.80, 100.0),
    ("ff_n40C_1v65", 1.65, -40.0),
    ("ss_n40C_1v40", 1.40, -40.0),
    ("ss_100C_1v60", 1.60, 100.0),
];

#[derive(Debug, Clone)]
pub struct Sky130ImportSummary {
    pub corners: usize,
    pub cells_per_corner: usize,
    pub lef_macros: usize,
    pub output_lib_dir: PathBuf,
    pub output_lef_dir: PathBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CornerType {
    TT,
    FF,
    SS,
}

impl fmt::Display for CornerType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            CornerType::TT => "TT",
            CornerType::FF => "FF",
            CornerType::SS => "SS",
        })
    }
}

impl CornerType {
    pub fn label(self) -> &'static str {
        match self {
            CornerType::TT => "Typical",
            CornerType::FF => "Fast",
            CornerType::SS => "Slow",
        }
    }

    pub fn engine_name(self) -> &'static str {
        match self {
            CornerType::TT => "tt",
            CornerType::FF => "ff",
            CornerType::SS => "ss",
        }
    }
}

#[derive(Debug, Clone)]
pub struct LibCorner {
    pub file_path: PathBuf,
    pub lib_name: String,
    pub process: String,
    pub corner_type: CornerType,
    pub voltage: f64,
    pub temperature: f64,
    pub process_value: f64,
    pub rc_type: String,
    pub short_name: String,
    pub cell_count: i32,
    pub time_unit: String,
    pub voltage_unit: String,
    pub leakage_power_unit: String,
    pub capacitive_load_unit: String,
    pub default_operating_conditions: String,
}

#[derive(Debug, Clone)]
pub struct ProcessGroup {
    pub process_name: String,
    pub directory: PathBuf,
    pub corners: Vec<LibCorner>,
}

/// Evidence collected before a technology is allowed to drive a signoff flow.
/// A readable Liberty file is not enough: synthesis needs a functional cell
/// basis, power signoff needs NLDM tables at every selected corner, and APR
/// needs a matching LEF macro library.
#[derive(Debug, Clone, Serialize)]
pub struct TechnologyCoverage {
    pub technology: String,
    pub liberty_corners: usize,
    pub structurally_valid_corners: usize,
    pub liberty_cells: usize,
    pub functional_families: Vec<String>,
    pub missing_functional_families: Vec<String>,
    pub nldm_corners: usize,
    pub lef_directory: String,
    pub lef_files: usize,
    pub lef_macros: usize,
    pub routing_layers: usize,
    pub synthesis_ready: bool,
    pub power_signoff_ready: bool,
    pub apr_ready: bool,
    pub findings: Vec<String>,
}

impl TechnologyCoverage {
    pub fn blocked_reason(&self) -> String {
        self.findings.join("; ")
    }

    pub fn text_report(&self) -> String {
        let mut out = String::new();
        out.push_str("Technology Coverage Preflight\n");
        out.push_str("=============================\n");
        out.push_str(&format!("Technology: {}\n", self.technology));
        out.push_str(&format!("Liberty corners: {}\n", self.liberty_corners));
        out.push_str(&format!("Structurally valid Liberty corners: {}/{}\n", self.structurally_valid_corners, self.liberty_corners));
        out.push_str(&format!("Liberty cells: {}\n", self.liberty_cells));
        out.push_str(&format!("Functional families: {}\n", self.functional_families.join(", ")));
        out.push_str(&format!("Missing families: {}\n", if self.missing_functional_families.is_empty() { "none".to_string() } else { self.missing_functional_families.join(", ") }));
        out.push_str(&format!("NLDM-ready corners: {}/{}\n", self.nldm_corners, self.liberty_corners));
        out.push_str(&format!("LEF directory: {}\n", self.lef_directory));
        out.push_str(&format!("LEF files/macros/routing layers: {}/{}/{}\n", self.lef_files, self.lef_macros, self.routing_layers));
        out.push_str(&format!("Synthesis eligibility: {}\n", if self.synthesis_ready { "READY" } else { "BLOCKED" }));
        out.push_str(&format!("Power signoff eligibility: {}\n", if self.power_signoff_ready { "READY" } else { "BLOCKED" }));
        out.push_str(&format!("APR eligibility: {}\n", if self.apr_ready { "READY" } else { "BLOCKED" }));
        out.push_str("\nFindings:\n");
        for finding in &self.findings { out.push_str(&format!("- {finding}\n")); }
        out
    }
}

impl ProcessGroup {
    pub fn get_tt(&self) -> Option<&LibCorner> {
        self.corners.iter().find(|corner| corner.corner_type == CornerType::TT)
    }

    /// Synthesis is performed against a nominal library. Prefer a true typical
    /// RC corner closest to 25 C and the technology's median supply voltage.
    pub fn get_synthesis_corner(&self) -> Option<&LibCorner> {
        if self.corners.is_empty() {
            return None;
        }
        let mut voltages: Vec<f64> = self.corners.iter()
            .map(|corner| corner.voltage)
            .filter(|voltage| *voltage > 0.0)
            .collect();
        voltages.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let nominal_voltage = voltages.get(voltages.len() / 2).copied().unwrap_or(1.0);

        self.corners.iter().min_by(|a, b| {
            let score = |corner: &LibCorner| {
                let type_penalty = if corner.corner_type == CornerType::TT { 0.0 } else { 1000.0 };
                let rc_penalty = if matches!(corner.rc_type.as_str(), "typ" | "tt" | "nom") { 0.0 } else { 100.0 };
                type_penalty + rc_penalty
                    + (corner.temperature - 25.0).abs()
                    + (corner.voltage - nominal_voltage).abs() * 100.0
            };
            score(a).partial_cmp(&score(b)).unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| a.short_name.cmp(&b.short_name))
        })
    }

    pub fn get_worst_corner(&self) -> Option<&LibCorner> {
        self.corners.iter()
            .filter(|corner| corner.corner_type == CornerType::SS)
            .max_by(|a, b| {
                a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| b.voltage.partial_cmp(&a.voltage).unwrap_or(std::cmp::Ordering::Equal))
            })
            .or_else(|| self.corners.iter().max_by(|a, b| {
                a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| b.voltage.partial_cmp(&a.voltage).unwrap_or(std::cmp::Ordering::Equal))
            }))
    }

    pub fn get_best_corner(&self) -> Option<&LibCorner> {
        self.corners.iter()
            .filter(|corner| corner.corner_type == CornerType::FF)
            .min_by(|a, b| {
                a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| b.voltage.partial_cmp(&a.voltage).unwrap_or(std::cmp::Ordering::Equal))
            })
            .or_else(|| self.corners.first())
    }

    pub fn corner_count(&self) -> usize {
        self.corners.len()
    }
}

#[derive(Debug)]
pub struct CornerDatabase {
    pub processes: Vec<ProcessGroup>,
    pub active_process: Option<String>,
    pub lib_dir: PathBuf,
    pub multi_corner: bool,
}

#[derive(Debug, Default)]
struct LibertyHeader {
    library_name: String,
    nom_process: Option<f64>,
    nom_temperature: Option<f64>,
    nom_voltage: Option<f64>,
    time_unit: String,
    voltage_unit: String,
    leakage_power_unit: String,
    capacitive_load_unit: String,
    default_operating_conditions: String,
    cell_count: i32,
}

impl CornerDatabase {
    pub fn auto_detect(lib_dir: &Path) -> Self {
        let lib_dir = fs::canonicalize(lib_dir).unwrap_or_else(|_| lib_dir.to_path_buf());
        let mut processes = Vec::new();

        if let Ok(entries) = fs::read_dir(&lib_dir) {
            let mut technology_dirs: Vec<PathBuf> = entries.flatten()
                .map(|entry| entry.path())
                .filter(|path| path.is_dir())
                .collect();
            technology_dirs.sort();
            for directory in technology_dirs {
                let process_name = directory.file_name()
                    .map(|name| name.to_string_lossy().to_string())
                    .unwrap_or_default();
                if process_name.starts_with('.') || process_name.is_empty() {
                    continue;
                }
                let mut files = Vec::new();
                Self::collect_liberty_files(&directory, &mut files);
                files.sort();
                let mut corners: Vec<LibCorner> = files.iter()
                    .filter_map(|path| Self::parse_corner_from_file(path, &process_name))
                    .collect();
                Self::sort_corners(&mut corners);
                if !corners.is_empty() {
                    processes.push(ProcessGroup { process_name, directory, corners });
                }
            }
        }

        processes.sort_by(|a, b| a.process_name.cmp(&b.process_name));
        // Sky130 is the checked-in complete default PDK.  Other directories
        // can be selected per project, but directory sort order must not make
        // a partial/reference 55 nm library the implicit default.
        let active_process = processes.iter()
            .find(|process| process.process_name == SKY130_PROCESS)
            .or_else(|| processes.iter().find(|process| process.process_name != "demo"))
            .or_else(|| processes.first())
            .map(|process| process.process_name.clone());

        Self { processes, active_process, lib_dir, multi_corner: true }
    }

    fn collect_liberty_files(directory: &Path, files: &mut Vec<PathBuf>) {
        let Ok(entries) = fs::read_dir(directory) else { return; };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                Self::collect_liberty_files(&path, files);
            } else if path.extension().and_then(|ext| ext.to_str())
                .map(|ext| ext.eq_ignore_ascii_case("lib"))
                .unwrap_or(false)
            {
                files.push(fs::canonicalize(&path).unwrap_or(path));
            }
        }
    }

    fn sort_corners(corners: &mut [LibCorner]) {
        corners.sort_by(|a, b| {
            let type_order = |corner: &LibCorner| match corner.corner_type {
                CornerType::TT => 0,
                CornerType::FF => 1,
                CornerType::SS => 2,
            };
            type_order(a).cmp(&type_order(b))
                .then_with(|| a.rc_type.cmp(&b.rc_type))
                .then_with(|| a.voltage.partial_cmp(&b.voltage).unwrap_or(std::cmp::Ordering::Equal))
                .then_with(|| a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal))
                .then_with(|| a.lib_name.cmp(&b.lib_name))
        });
    }

    fn parse_corner_from_file(file_path: &Path, process: &str) -> Option<LibCorner> {
        let header = Self::read_liberty_header(file_path)?;
        let stem = file_path.file_stem()?.to_string_lossy();
        let normalized_stem = stem.strip_suffix("_nldm").unwrap_or(&stem);
        let lower = normalized_stem.to_ascii_lowercase();
        let lower_lib = header.library_name.to_ascii_lowercase();

        let corner_type = if Self::has_token(&lower, "ff") || Self::has_token(&lower_lib, "ff") {
            CornerType::FF
        } else if Self::has_token(&lower, "ss") || Self::has_token(&lower_lib, "ss") {
            CornerType::SS
        } else {
            CornerType::TT
        };

        let rc_type = ["rcworst", "cworst", "rcbest", "cbest", "typ", "nom", "tt"]
            .iter()
            .find(|token| Self::has_token(&lower, token) || Self::has_token(&lower_lib, token))
            .copied()
            .unwrap_or("typ")
            .to_string();

        let voltage = header.nom_voltage
            .or_else(|| Self::filename_voltage(normalized_stem))
            .unwrap_or(1.0);
        let temperature = header.nom_temperature
            .or_else(|| Self::filename_temperature(normalized_stem))
            .unwrap_or(25.0);
        let short_name = Self::corner_short_name(normalized_stem, process, corner_type, &rc_type, voltage, temperature);

        Some(LibCorner {
            file_path: file_path.to_path_buf(),
            lib_name: if header.library_name.is_empty() { normalized_stem.to_string() } else { header.library_name },
            process: process.to_string(),
            corner_type,
            voltage,
            temperature,
            process_value: header.nom_process.unwrap_or(1.0),
            rc_type,
            short_name,
            cell_count: header.cell_count,
            time_unit: header.time_unit,
            voltage_unit: header.voltage_unit,
            leakage_power_unit: header.leakage_power_unit,
            capacitive_load_unit: header.capacitive_load_unit,
            default_operating_conditions: header.default_operating_conditions,
        })
    }

    fn read_liberty_header(path: &Path) -> Option<LibertyHeader> {
        let file = fs::File::open(path).ok()?;
        let mut header = LibertyHeader::default();
        for line in BufReader::new(file).lines().map_while(Result::ok) {
            let trimmed = line.trim();
            if header.library_name.is_empty() && trimmed.starts_with("library") {
                header.library_name = Self::group_name(trimmed).unwrap_or_default();
            }
            if Self::is_cell_declaration(trimmed) && Self::group_name(trimmed).is_some() {
                header.cell_count += 1;
            }
            if header.nom_process.is_none() { header.nom_process = Self::numeric_attribute(trimmed, "nom_process"); }
            if header.nom_temperature.is_none() { header.nom_temperature = Self::numeric_attribute(trimmed, "nom_temperature"); }
            if header.nom_voltage.is_none() { header.nom_voltage = Self::numeric_attribute(trimmed, "nom_voltage"); }
            Self::string_attribute(trimmed, "time_unit", &mut header.time_unit);
            Self::string_attribute(trimmed, "voltage_unit", &mut header.voltage_unit);
            Self::string_attribute(trimmed, "leakage_power_unit", &mut header.leakage_power_unit);
            Self::string_attribute(trimmed, "default_operating_conditions", &mut header.default_operating_conditions);
            if header.capacitive_load_unit.is_empty() && trimmed.starts_with("capacitive_load_unit") {
                header.capacitive_load_unit = Self::group_name(trimmed).unwrap_or_default();
            }
        }
        if header.library_name.is_empty() {
            header.library_name = path.file_stem()?.to_string_lossy().to_string();
        }
        Some(header)
    }

    fn group_name(line: &str) -> Option<String> {
        let start = line.find('(')? + 1;
        let end = line[start..].find(')')? + start;
        Some(line[start..end].trim().trim_matches('"').to_string())
    }

    fn numeric_attribute(line: &str, key: &str) -> Option<f64> {
        let remainder = line.strip_prefix(key)?.trim_start();
        let remainder = remainder.strip_prefix(':')?.trim_start();
        remainder.trim_end_matches(';').trim().parse().ok()
    }

    fn string_attribute(line: &str, key: &str, target: &mut String) {
        if !target.is_empty() { return; }
        let Some(remainder) = line.strip_prefix(key) else { return; };
        let Some(remainder) = remainder.trim_start().strip_prefix(':') else { return; };
        *target = remainder.trim().trim_end_matches(';').trim_matches('"').to_string();
    }

    fn has_token(value: &str, token: &str) -> bool {
        value.split(|ch: char| !ch.is_ascii_alphanumeric()).any(|part| part == token)
    }

    fn filename_voltage(stem: &str) -> Option<f64> {
        stem.split('_').rev().find_map(|part| {
            let lower = part.to_ascii_lowercase();
            if lower.contains('p') && lower.chars().all(|ch| ch.is_ascii_digit() || ch == 'p') {
                lower.replace('p', ".").parse().ok()
            } else { None }
        })
    }

    fn filename_temperature(stem: &str) -> Option<f64> {
        stem.split('_').rev().find_map(|part| {
            let lower = part.to_ascii_lowercase();
            if let Some(value) = lower.strip_prefix('m') {
                return value.parse::<f64>().ok().map(|number| -number);
            }
            if lower.chars().all(|ch| ch.is_ascii_digit()) {
                return lower.parse().ok();
            }
            None
        })
    }

    fn corner_short_name(stem: &str, process: &str, corner_type: CornerType, rc_type: &str, voltage: f64, temperature: f64) -> String {
        // SkyWater names use a double underscore between the process family
        // and corner (sky130_fd_sc_hd__tt...), while older libraries use one.
        // Normalize both forms so the UI and report identifiers never start
        // with an artificial underscore.
        let double_prefix = format!("{}__", process);
        let single_prefix = format!("{}_", process);
        let candidate = stem.strip_prefix(&double_prefix)
            .or_else(|| stem.strip_prefix(&single_prefix))
            .unwrap_or(stem);
        if !candidate.is_empty() && candidate.len() <= 72 {
            return candidate.to_string();
        }
        let temperature = if temperature < 0.0 { format!("m{:.0}", -temperature) } else { format!("{:.0}", temperature) };
        format!("{}_{}_{}_{temperature}", corner_type.engine_name(), rc_type, Self::voltage_token(voltage))
    }

    fn voltage_token(voltage: f64) -> String {
        let mut value = format!("{voltage:.3}");
        while value.ends_with('0') { value.pop(); }
        value.trim_end_matches('.').replace('.', "p")
    }

    pub fn get_active_group(&self) -> Option<&ProcessGroup> {
        let active = self.active_process.as_deref()?;
        self.processes.iter().find(|process| process.process_name == active)
    }

    pub fn get_active_corners(&self) -> Vec<&LibCorner> {
        self.get_active_group().map(|group| group.corners.iter().collect()).unwrap_or_default()
    }

    pub fn get_default_liberty(&self) -> Option<PathBuf> {
        self.get_active_group()?.get_synthesis_corner().map(|corner| corner.file_path.clone())
    }

    pub fn get_synthesis_corner(&self) -> Option<&LibCorner> {
        self.get_active_group()?.get_synthesis_corner()
    }

    pub fn get_all_liberty_paths(&self) -> Vec<PathBuf> {
        self.get_active_corners().iter().map(|corner| corner.file_path.clone()).collect()
    }

    pub fn liberty_path_for_corner(&self, corner: &LibCorner) -> Option<PathBuf> {
        corner.file_path.is_file().then(|| corner.file_path.clone())
    }

    pub fn set_active_process(&mut self, name: &str) -> Result<(), String> {
        if self.processes.iter().any(|process| process.process_name == name) {
            self.active_process = Some(name.to_string());
            Ok(())
        } else {
            Err(format!("Technology '{}' not found. Available: {}", name, self.list_processes().join(", ")))
        }
    }

    pub fn set_multi_corner(&mut self, enabled: bool) { self.multi_corner = enabled; }

    /// Inspect the active technology from source data.  This deliberately
    /// avoids using synthesis heuristics: a PVT library with insufficient
    /// boolean coverage or without NLDM tables is a technology-data problem,
    /// never a reason to regenerate RTL.
    pub fn active_coverage(&self) -> Option<TechnologyCoverage> {
        let group = self.get_active_group()?;
        let mut liberty_cells = BTreeSet::new();
        let mut nldm_corners = 0usize;
        let mut structurally_valid_corners = 0usize;
        for corner in &group.corners {
            let (cells, has_nldm, structurally_valid) = Self::scan_liberty_contents(&corner.file_path);
            liberty_cells.extend(cells);
            if structurally_valid {
                structurally_valid_corners += 1;
                if has_nldm { nldm_corners += 1; }
            }
        }

        let mut families = BTreeSet::new();
        for cell in &liberty_cells {
            let name = cell.to_ascii_lowercase();
            if name.contains("inv") { families.insert("inverter".to_string()); }
            if name.contains("buf") { families.insert("buffer".to_string()); }
            if name.contains("and") && !name.contains("nand") { families.insert("and".to_string()); }
            if name.contains("or") && !name.contains("nor") && !name.contains("xor") { families.insert("or".to_string()); }
            if name.contains("xor") { families.insert("xor".to_string()); }
            if name.contains("dff") || name.contains("df") || name.contains("sdff") { families.insert("flip_flop".to_string()); }
        }
        let required = ["inverter", "buffer", "and", "or", "xor"];
        let missing_functional_families = required.iter()
            .filter(|family| !families.contains(**family))
            .map(|family| (*family).to_string())
            .collect::<Vec<_>>();

        let lef_root = self.lib_dir.parent().unwrap_or(&self.lib_dir)
            .join("lef").join(&group.process_name);
        let (lef_files, lef_macros, routing_layers) = Self::scan_lef_directory(&lef_root);
        // A standard-cell library can implement the primitive basis through
        // complex gates (AOI/OAI) rather than explicit INV/OR/XOR names.
        // Treat family-name recognition as evidence, not as the final mapping
        // verdict.  The post-synthesis generic-cell gate is authoritative.
        let has_synthesis_basis = !group.corners.is_empty()
            && liberty_cells.len() >= 20
            && families.contains("buffer");
        let synthesis_ready = structurally_valid_corners == group.corners.len() && has_synthesis_basis;
        let power_signoff_ready = synthesis_ready && nldm_corners == group.corners.len();
        let apr_ready = synthesis_ready && lef_macros > 0 && routing_layers >= 2;
        let mut findings = Vec::new();
        if group.corners.is_empty() { findings.push("No Liberty corners were discovered".to_string()); }
        if structurally_valid_corners != group.corners.len() {
            findings.push(format!("Liberty structural validation passed only {}/{} corners (unbalanced braces or truncated source)", structurally_valid_corners, group.corners.len()));
        }
        if structurally_valid_corners == group.corners.len() && !has_synthesis_basis {
            findings.push(format!(
                "Library has insufficient baseline synthesis coverage (cells={}, buffer_present={})",
                liberty_cells.len(), families.contains("buffer")
            ));
        } else if !missing_functional_families.is_empty() {
            findings.push(format!(
                "Cell-name family evidence is incomplete ({}) but the post-synthesis mapping coverage gate remains authoritative",
                missing_functional_families.join(", ")
            ));
        }
        if nldm_corners != group.corners.len() {
            findings.push(format!("NLDM timing/power tables are present in only {}/{} Liberty corners", nldm_corners, group.corners.len()));
        }
        if lef_macros == 0 { findings.push(format!("No LEF macros found under {}", lef_root.display())); }
        if lef_macros > 0 && routing_layers < 2 {
            findings.push(format!("LEF library has only {} routing layer(s); APR requires at least two", routing_layers));
        }
        if findings.is_empty() { findings.push("Technology source coverage is suitable for native synthesis, APR, and multi-corner signoff".to_string()); }

        Some(TechnologyCoverage {
            technology: group.process_name.clone(),
            liberty_corners: group.corners.len(),
            structurally_valid_corners,
            liberty_cells: liberty_cells.len(),
            functional_families: families.into_iter().collect(),
            missing_functional_families,
            nldm_corners,
            lef_directory: lef_root.to_string_lossy().to_string(),
            lef_files,
            lef_macros,
            routing_layers,
            synthesis_ready,
            power_signoff_ready,
            apr_ready,
            findings,
        })
    }

    fn scan_liberty_contents(path: &Path) -> (BTreeSet<String>, bool, bool) {
        let mut cells = BTreeSet::new();
        let mut has_timing = false;
        let mut has_power = false;
        let mut brace_depth = 0i64;
        let mut in_quote = false;
        let Ok(file) = fs::File::open(path) else { return (cells, false, false); };
        for line in BufReader::new(file).lines().map_while(Result::ok) {
            let trimmed = line.trim();
            if Self::is_cell_declaration(trimmed) {
                if let Some(name) = Self::group_name(trimmed) { cells.insert(name); }
            }
            // NLDM needs both delay and power table families.  Do not accept
            // scalar capacitance/leakage data as a signoff substitute.
            has_timing |= trimmed.starts_with("cell_rise") || trimmed.starts_with("cell_fall");
            has_power |= trimmed.starts_with("rise_power") || trimmed.starts_with("fall_power");
            for ch in trimmed.chars() {
                if ch == '"' { in_quote = !in_quote; continue; }
                if !in_quote && ch == '{' { brace_depth += 1; }
                if !in_quote && ch == '}' { brace_depth -= 1; }
            }
        }
        (cells, has_timing && has_power, brace_depth == 0 && !in_quote)
    }

    fn scan_lef_directory(directory: &Path) -> (usize, usize, usize) {
        let mut files = Vec::new();
        Self::collect_lef_files(directory, &mut files);
        let mut macros = BTreeSet::new();
        let mut routing_layers = BTreeSet::new();
        for path in &files {
            let Ok(file) = fs::File::open(path) else { continue; };
            let mut in_layer = false;
            let mut layer_name = String::new();
            for line in BufReader::new(file).lines().map_while(Result::ok) {
                let trimmed = line.trim();
                if trimmed.starts_with("MACRO ") {
                    if let Some(name) = trimmed.split_whitespace().nth(1) { macros.insert(name.to_string()); }
                }
                if trimmed.starts_with("LAYER ") {
                    in_layer = true;
                    layer_name = trimmed.split_whitespace().nth(1).unwrap_or_default().to_string();
                }
                if in_layer && (trimmed.contains("TYPE ROUTING") || trimmed.contains("TYPE\t\tROUTING")) {
                    routing_layers.insert(layer_name.clone());
                }
                if in_layer && trimmed.starts_with("END ") { in_layer = false; }
            }
        }
        (files.len(), macros.len(), routing_layers.len())
    }

    fn is_cell_declaration(value: &str) -> bool {
        let rest = value.strip_prefix("cell").unwrap_or_default();
        (rest.starts_with('(') || rest.starts_with(char::is_whitespace))
            && Self::group_name(value).is_some()
    }

    fn collect_lef_files(directory: &Path, files: &mut Vec<PathBuf>) {
        let Ok(entries) = fs::read_dir(directory) else { return; };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                Self::collect_lef_files(&path, files);
            } else if path.extension().and_then(|ext| ext.to_str())
                .map(|ext| ext.eq_ignore_ascii_case("lef") || ext.eq_ignore_ascii_case("tlef")).unwrap_or(false) {
                files.push(path);
            }
        }
    }

    pub fn list_processes(&self) -> Vec<&str> {
        self.processes.iter().map(|process| process.process_name.as_str()).collect()
    }

    pub fn summary(&self) -> String {
        self.processes.iter().map(|process| {
            let mut counts: BTreeMap<String, usize> = BTreeMap::new();
            for corner in &process.corners { *counts.entry(corner.corner_type.to_string()).or_default() += 1; }
            format!("  {} ({} corners: TT={}, FF={}, SS={})",
                process.process_name, process.corners.len(),
                counts.get("TT").copied().unwrap_or(0),
                counts.get("FF").copied().unwrap_or(0),
                counts.get("SS").copied().unwrap_or(0))
        }).collect::<Vec<_>>().join("\n")
    }
}

/// Reconstruct five signoff-relevant Sky130 Liberty corners and their LEF
/// macro library from the checked-out SkyWater source tree.  SkyWater ships
/// per-cell JSON timing views, so this is a local format conversion rather
/// than an invocation of a third-party implementation tool.
pub fn import_sky130_reference(
    reference_root: &Path,
    libs_root: &Path,
    lef_root: &Path,
) -> Result<Sky130ImportSummary, String> {
    let cells_root = reference_root.join("cells");
    let technology_lef = reference_root.join("tech").join("sky130_fd_sc_hd.tlef");
    if !cells_root.is_dir() || !technology_lef.is_file() {
        return Err(format!(
            "Sky130 reference must contain cells/ and tech/sky130_fd_sc_hd.tlef: {}",
            reference_root.display()
        ));
    }

    let mut json_files = Vec::new();
    collect_files_matching(&cells_root, ".lib.json", &mut json_files);
    json_files.sort();
    if json_files.is_empty() {
        return Err(format!("No Sky130 per-cell Liberty JSON files under {}", cells_root.display()));
    }

    let output_lib_dir = libs_root.join(SKY130_PROCESS);
    let output_lef_dir = lef_root.join(SKY130_PROCESS);
    fs::create_dir_all(&output_lib_dir).map_err(|e| e.to_string())?;
    fs::create_dir_all(&output_lef_dir).map_err(|e| e.to_string())?;

    let mut expected_cells = None::<usize>;
    for (corner, voltage, temperature) in SKY130_REPRESENTATIVE_CORNERS {
        let suffix = format!("__{corner}.lib.json");
        let files: Vec<&PathBuf> = json_files.iter()
            .filter(|path| path.file_name().is_some_and(|name| name.to_string_lossy().ends_with(&suffix)))
            .collect();
        if files.len() < 100 {
            return Err(format!("Sky130 corner {corner} has only {} cell JSON files; refusing incomplete import", files.len()));
        }
        if let Some(expected) = expected_cells {
            if files.len() != expected {
                return Err(format!("Sky130 corner {corner} has {} cells, expected {expected}; source corner coverage is inconsistent", files.len()));
            }
        } else {
            expected_cells = Some(files.len());
        }
        let lib_name = format!("{SKY130_PROCESS}__{corner}");
        let out_path = output_lib_dir.join(format!("{lib_name}.lib"));
        write_sky130_corner(&out_path, &lib_name, corner, voltage, temperature, &files)?;
    }

    let mut lef_files = Vec::new();
    collect_sky130_macro_lefs(&cells_root, &mut lef_files);
    lef_files.sort();
    if lef_files.len() < 100 {
        return Err(format!("Sky130 reference has only {} standard-cell LEF files", lef_files.len()));
    }
    write_concatenated_lef(&output_lef_dir.join("standard_cells.lef"), &lef_files)?;
    fs::copy(&technology_lef, output_lef_dir.join("technology.tlef"))
        .map_err(|e| format!("copy {}: {e}", technology_lef.display()))?;

    Ok(Sky130ImportSummary {
        corners: SKY130_REPRESENTATIVE_CORNERS.len(),
        cells_per_corner: expected_cells.unwrap_or(0),
        lef_macros: lef_files.len(),
        output_lib_dir,
        output_lef_dir,
    })
}

fn collect_files_matching(root: &Path, suffix: &str, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else { return; };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_files_matching(&path, suffix, out);
        } else if path.file_name().is_some_and(|name| name.to_string_lossy().ends_with(suffix)) {
            out.push(path);
        }
    }
}

fn collect_sky130_macro_lefs(root: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else { return; };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_sky130_macro_lefs(&path, out);
            continue;
        }
        let name = path.file_name().map(|name| name.to_string_lossy()).unwrap_or_default();
        if name.ends_with(".lef") && !name.ends_with(".magic.lef") && name.starts_with("sky130_fd_sc_hd__") {
            out.push(path);
        }
    }
}

fn write_sky130_corner(
    output: &Path,
    lib_name: &str,
    corner: &str,
    voltage: f64,
    temperature: f64,
    files: &[&PathBuf],
) -> Result<(), String> {
    let temporary = output.with_extension("lib.tmp");
    let file = fs::File::create(&temporary).map_err(|e| format!("{}: {e}", temporary.display()))?;
    let mut writer = std::io::BufWriter::new(file);
    writeln!(writer, "/* Generated locally from SkyWater per-cell Liberty JSON. */").map_err(|e| e.to_string())?;
    writeln!(writer, "library ({lib_name}) {{").map_err(|e| e.to_string())?;
    writeln!(writer, "  time_unit : \"1ns\";\n  voltage_unit : \"1V\";\n  leakage_power_unit : \"1nW\";\n  capacitive_load_unit (1, pf);").map_err(|e| e.to_string())?;
    writeln!(writer, "  nom_process : 1;\n  nom_temperature : {temperature};\n  nom_voltage : {voltage};").map_err(|e| e.to_string())?;
    writeln!(writer, "  default_operating_conditions : {corner};\n  operating_conditions ({corner}) {{ process : 1; voltage : {voltage}; temperature : {temperature}; }}").map_err(|e| e.to_string())?;
    for source in files {
        let content = fs::read_to_string(source).map_err(|e| format!("{}: {e}", source.display()))?;
        let value: Value = serde_json::from_str(&content).map_err(|e| format!("{}: {e}", source.display()))?;
        // PVT is a property of the Liberty file, not of the physical cell.
        // Every corner must expose the same macro name so a netlist mapped at
        // TT can be timed, powered and placed against FF/SS views as well.
        let source_name = source.file_name().and_then(|name| name.to_str())
            .ok_or_else(|| format!("invalid Sky130 JSON file name: {}", source.display()))?;
        let suffix = format!("__{corner}.lib.json");
        let name = source_name.strip_suffix(&suffix)
            .ok_or_else(|| format!("Sky130 JSON does not match requested corner {corner}: {}", source.display()))?;
        writeln!(writer, "  cell ({name}) {{").map_err(|e| e.to_string())?;
        emit_liberty_object(&mut writer, &value, 4)?;
        writeln!(writer, "  }}").map_err(|e| e.to_string())?;
    }
    writeln!(writer, "}}").map_err(|e| e.to_string())?;
    writer.flush().map_err(|e| e.to_string())?;
    fs::rename(&temporary, output).map_err(|e| format!("{} -> {}: {e}", temporary.display(), output.display()))
}

fn emit_liberty_object(writer: &mut impl Write, value: &Value, indent: usize) -> Result<(), String> {
    let object = value.as_object().ok_or_else(|| "Sky130 cell JSON root is not an object".to_string())?;
    for (key, child) in object {
        emit_liberty_item(writer, key, child, indent)?;
    }
    Ok(())
}

fn emit_liberty_item(writer: &mut impl Write, key: &str, value: &Value, indent: usize) -> Result<(), String> {
    let prefix = " ".repeat(indent);
    let (group, args) = split_group_key(key);
    match value {
        Value::Object(_) => {
            writeln!(writer, "{prefix}{group} ({args}) {{").map_err(|e| e.to_string())?;
            emit_liberty_object(writer, value, indent + 2)?;
            writeln!(writer, "{prefix}}}").map_err(|e| e.to_string())?;
        }
        Value::Array(entries) if key == "values" => {
            let rows = entries.iter().map(liberty_row).collect::<Result<Vec<_>, _>>()?;
            writeln!(writer, "{prefix}values ({});", rows.iter().map(|row| format!("\"{row}\"")).collect::<Vec<_>>().join(", \\\n"))
                .map_err(|e| e.to_string())?;
        }
        Value::Array(entries) if key == "index_1" || key == "index_2" || key == "index_3" => {
            let row = entries.iter().map(liberty_scalar).collect::<Result<Vec<_>, _>>()?.join(", ");
            writeln!(writer, "{prefix}{key} (\"{row}\");").map_err(|e| e.to_string())?;
        }
        Value::Array(entries) => {
            for entry in entries {
                writeln!(writer, "{prefix}{group} ({args}) {{").map_err(|e| e.to_string())?;
                if entry.is_object() {
                    emit_liberty_object(writer, entry, indent + 2)?;
                } else {
                    writeln!(writer, "{}value : {};", " ".repeat(indent + 2), liberty_scalar(entry)?).map_err(|e| e.to_string())?;
                }
                writeln!(writer, "{prefix}}}").map_err(|e| e.to_string())?;
            }
        }
        _ => writeln!(writer, "{prefix}{key} : {};", liberty_scalar(value)?).map_err(|e| e.to_string())?,
    }
    Ok(())
}

fn split_group_key(key: &str) -> (&str, String) {
    let mut parts = key.split(',');
    let group = parts.next().unwrap_or(key);
    let args = parts.collect::<Vec<_>>().join(", ");
    (group, args)
}

fn liberty_scalar(value: &Value) -> Result<String, String> {
    match value {
        Value::String(text) => Ok(format!("\"{}\"", text.replace('\\', "\\\\").replace('"', "\\\""))),
        Value::Number(number) => Ok(number.to_string()),
        Value::Bool(value) => Ok(if *value { "true".to_string() } else { "false".to_string() }),
        Value::Null => Ok("\"\"".to_string()),
        _ => Err("Sky130 Liberty JSON contains an unsupported scalar value".to_string()),
    }
}

fn liberty_row(value: &Value) -> Result<String, String> {
    match value {
        Value::Array(values) => values.iter().map(liberty_scalar).collect::<Result<Vec<_>, _>>()
            .map(|values| values.into_iter().map(|value| value.trim_matches('"').to_string()).collect::<Vec<_>>().join(", ")),
        _ => liberty_scalar(value).map(|value| value.trim_matches('"').to_string()),
    }
}

fn write_concatenated_lef(output: &Path, source_files: &[PathBuf]) -> Result<(), String> {
    let temporary = output.with_extension("lef.tmp");
    let file = fs::File::create(&temporary).map_err(|e| format!("{}: {e}", temporary.display()))?;
    let mut writer = std::io::BufWriter::new(file);
    writeln!(writer, "# Combined locally from SkyWater standard-cell LEF macros.").map_err(|e| e.to_string())?;
    for source in source_files {
        let content = fs::read_to_string(source).map_err(|e| format!("{}: {e}", source.display()))?;
        writer.write_all(content.as_bytes()).map_err(|e| e.to_string())?;
        if !content.ends_with('\n') { writeln!(writer).map_err(|e| e.to_string())?; }
    }
    writer.flush().map_err(|e| e.to_string())?;
    fs::rename(&temporary, output).map_err(|e| format!("{} -> {}: {e}", temporary.display(), output.display()))
}

/// Read actual physical areas keyed by concrete Liberty cell name.  This is
/// intentionally independent of synthesis so reporting and GUI exchanges use
/// the same foundry numbers as the mapper.
pub fn liberty_cell_areas(path: &Path) -> BTreeMap<String, f64> {
    let mut areas = BTreeMap::new();
    let Ok(file) = fs::File::open(path) else { return areas; };
    let mut current = None::<String>;
    let mut depth = 0i64;
    for line in BufReader::new(file).lines().map_while(Result::ok) {
        let trimmed = line.trim();
        if current.is_none() && CornerDatabase::is_cell_declaration(trimmed) {
            current = CornerDatabase::group_name(trimmed);
            depth = trimmed.chars().filter(|ch| *ch == '{').count() as i64
                - trimmed.chars().filter(|ch| *ch == '}').count() as i64;
            continue;
        }
        if let Some(name) = current.as_ref() {
            if trimmed.starts_with("area") {
                if let Some(value) = CornerDatabase::numeric_attribute(trimmed, "area") {
                    areas.insert(name.clone(), value);
                }
            }
            depth += trimmed.chars().filter(|ch| *ch == '{').count() as i64;
            depth -= trimmed.chars().filter(|ch| *ch == '}').count() as i64;
            if depth <= 0 { current = None; }
        }
    }
    areas
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("ai_digital_tech_{name}_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        root
    }

    fn write_lib(path: &Path, name: &str, voltage: f64, temperature: f64, cells: usize) {
        let mut content = format!("library ({name}) {{\n  time_unit : \"1ns\";\n  voltage_unit : \"1V\";\n  leakage_power_unit : \"1nW\";\n  capacitive_load_unit (1,pf);\n  nom_process : 1;\n  nom_temperature : {temperature};\n  nom_voltage : {voltage};\n  default_operating_conditions : OP;\n");
        for index in 0..cells { content.push_str(&format!("  cell (C{index}) {{ area : 1; }}\n")); }
        content.push_str("}\n");
        fs::write(path, content).unwrap();
    }

    #[test]
    fn discovers_directory_technologies_and_header_metadata() {
        let root = temp_root("discover");
        let tech = root.join("node55");
        fs::create_dir_all(&tech).unwrap();
        write_lib(&tech.join("node55_typ_tt_1p2_25.lib"), "node55_tt", 1.2, 25.0, 3);
        write_lib(&tech.join("node55_ss_rcworst_1p08_125.lib"), "node55_ss", 1.08, 125.0, 2);
        fs::write(root.join("legacy.lib"), "library (ignored) {}\n").unwrap();

        let db = CornerDatabase::auto_detect(&root);
        assert_eq!(db.list_processes(), vec!["node55"]);
        let group = db.get_active_group().unwrap();
        assert_eq!(group.corner_count(), 2);
        assert_eq!(group.get_synthesis_corner().unwrap().lib_name, "node55_tt");
        assert_eq!(group.get_synthesis_corner().unwrap().cell_count, 3);
        assert_eq!(group.get_worst_corner().unwrap().temperature, 125.0);
        assert_eq!(group.corners[0].time_unit, "1ns");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn technology_name_is_directory_name_not_library_prefix() {
        let root = temp_root("directory_name");
        let tech = root.join("custom_process");
        fs::create_dir_all(&tech).unwrap();
        write_lib(&tech.join("vendor_fast.lib"), "unrelated_library_ff", 0.9, -40.0, 1);
        let db = CornerDatabase::auto_detect(&root);
        assert_eq!(db.processes[0].process_name, "custom_process");
        assert_eq!(db.processes[0].corners[0].process, "custom_process");
        assert_eq!(db.processes[0].corners[0].corner_type, CornerType::FF);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn discovers_checked_in_sky130_representative_corners() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("libs");
        let db = CornerDatabase::auto_detect(&root);
        let sky = db.processes.iter().find(|p| p.process_name == "sky130_fd_sc_hd")
            .expect("checked-in Sky130 process should be discoverable");
        assert_eq!(sky.corners.len(), 5);
        assert_eq!(sky.corners.iter().filter(|c| c.corner_type == CornerType::TT).count(), 2);
        assert_eq!(sky.corners.iter().filter(|c| c.corner_type == CornerType::FF).count(), 1);
        assert_eq!(sky.corners.iter().filter(|c| c.corner_type == CornerType::SS).count(), 2);
        assert!(sky.corners.iter().any(|c| (c.voltage - 1.80).abs() < 1e-9 && (c.temperature - 25.0).abs() < 1e-9));
        // The checked-in seed is intentionally compact before a local source
        // import; after `/tech import-sky130` every corner has the complete
        // per-cell source set.  Discovery must accept both states.
        assert!(sky.corners.iter().all(|c| c.cell_count >= 6));
    }

    #[test]
    fn blocks_truncated_liberty_before_any_design_flow() {
        let container = temp_root("truncated");
        let libs = container.join("libs");
        let process = libs.join("broken");
        fs::create_dir_all(&process).unwrap();
        // Deliberately omit the closing groups, matching the failure mode of
        // a file cut during PDK import.
        fs::write(process.join("broken_tt.lib"), "library (broken) {\ncell (BUFX1) {\n pin (A) { direction : input; }\n pin (Y) { direction : output; function : \\\"A\\\"; cell_rise (x) { } rise_power (x) { }\n").unwrap();
        let db = CornerDatabase::auto_detect(&libs);
        let coverage = db.active_coverage().unwrap();
        assert_eq!(coverage.structurally_valid_corners, 0);
        assert!(!coverage.synthesis_ready);
        assert!(!coverage.power_signoff_ready);
        assert!(coverage.blocked_reason().contains("structural validation"));
        fs::remove_dir_all(container).unwrap();
    }
}
