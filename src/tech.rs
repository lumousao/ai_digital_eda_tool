/**
 * Technology library discovery and corner metadata.
 *
 * Each direct child of libs/ is one technology. Every Liberty file below that
 * directory is one analysis corner. The directory name is authoritative; file
 * names and Liberty headers are only used to describe and rank corners.
 */

use std::collections::BTreeMap;
use std::fmt;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};

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
        let active_process = processes.iter()
            .find(|process| process.process_name != "demo")
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
            if trimmed.starts_with("cell") && Self::group_name(trimmed).is_some() {
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
        let prefix = format!("{}_", process);
        let candidate = stem.strip_prefix(&prefix).unwrap_or(stem);
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
}
