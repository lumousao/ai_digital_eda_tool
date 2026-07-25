/**
 * Technology / Corner Database
 *
 * Auto-detects process corners from Liberty (.lib) files,
 * groups them by process name, and provides per-corner
 * library paths for multi-corner timing/power analysis.
 *
 * Reference: OpenSTA's corner/liberty management pattern.
 */

use std::fmt;
use std::path::{Path, PathBuf};

/// Corner type classification
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CornerType {
    /// Typical-Typical (nominal process, voltage, temperature)
    TT,
    /// Fast-Fast (fast process, high voltage, low temperature variant)
    FF,
    /// Slow-Slow (slow process, low voltage, high temperature variant)
    SS,
}

impl fmt::Display for CornerType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CornerType::TT => write!(f, "TT"),
            CornerType::FF => write!(f, "FF"),
            CornerType::SS => write!(f, "SS"),
        }
    }
}

impl CornerType {
    pub fn label(&self) -> &str {
        match self {
            CornerType::TT => "Typical",
            CornerType::FF => "Fast",
            CornerType::SS => "Slow",
        }
    }
}

/// Represents a single liberty corner file
#[derive(Debug, Clone)]
pub struct LibCorner {
    /// Absolute path to the .lib file
    pub file_path: PathBuf,
    /// Library name from header (e.g. "ics55_LLSC_H9CR_typ_tt_1p2_25")
    pub lib_name: String,
    /// Process name (e.g. "ics55_LLSC_H9CR")
    pub process: String,
    /// Corner type (TT, FF, or SS)
    pub corner_type: CornerType,
    /// Nominal voltage in Volts
    pub voltage: f64,
    /// Nominal temperature in Celsius
    pub temperature: f64,
    /// RC type from filename (cbest, rcbest, cworst, rcworst, typ)
    pub rc_type: String,
    /// Short display name (e.g. "tt_1p2_25")
    pub short_name: String,
    /// Number of cells in library (from header parse)
    pub cell_count: i32,
}

/// All corners grouped by process name
#[derive(Debug, Clone)]
pub struct ProcessGroup {
    pub process_name: String,
    /// Sorted corners: TT first, then FF, then SS
    pub corners: Vec<LibCorner>,
}

impl ProcessGroup {
    /// Get TT corner if available
    pub fn get_tt(&self) -> Option<&LibCorner> {
        self.corners.iter().find(|c| c.corner_type == CornerType::TT)
    }

    /// Get all FF corners
    pub fn get_ff_corners(&self) -> Vec<&LibCorner> {
        self.corners.iter().filter(|c| c.corner_type == CornerType::FF).collect()
    }

    /// Get all SS corners
    pub fn get_ss_corners(&self) -> Vec<&LibCorner> {
        self.corners.iter().filter(|c| c.corner_type == CornerType::SS).collect()
    }

    /// Get the worst-case corner (SS corner with highest temperature)
    pub fn get_worst_corner(&self) -> Option<&LibCorner> {
        self.corners.iter()
            .filter(|c| c.corner_type == CornerType::SS)
            .max_by(|a, b| a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal)
                .then(a.voltage.partial_cmp(&b.voltage).unwrap_or(std::cmp::Ordering::Equal).reverse()))
    }

    /// Get the best-case corner (FF corner with lowest temperature)
    pub fn get_best_corner(&self) -> Option<&LibCorner> {
        self.corners.iter()
            .filter(|c| c.corner_type == CornerType::FF)
            .min_by(|a, b| a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal))
    }

    /// Total number of corners
    pub fn corner_count(&self) -> usize {
        self.corners.len()
    }
}

/// Main database for technology/corner management
pub struct CornerDatabase {
    pub processes: Vec<ProcessGroup>,
    pub active_process: Option<String>,
    pub lib_dir: PathBuf,
    /// If true, analyze all corners; if false, use default/selected corner
    pub multi_corner: bool,
}

impl CornerDatabase {
    /// Auto-detect all liberty files in the given directory,
    /// parse their headers, and group by process name.
    pub fn auto_detect(lib_dir: &Path) -> Self {
        let mut all_corners: Vec<LibCorner> = Vec::new();
        let lib_dir = lib_dir.to_path_buf();

        // Scan for .lib files
        if let Ok(entries) = std::fs::read_dir(&lib_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                let fname = path.file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_default();

                if !fname.ends_with(".lib") {
                    continue;
                }

                // Try to parse corner info from filename and library header
                if let Some(corner) = Self::parse_corner_from_file(&path, &fname) {
                    all_corners.push(corner);
                }
            }
        }

        // Group by process name
        let mut groups: std::collections::BTreeMap<String, Vec<LibCorner>> = std::collections::BTreeMap::new();
        for corner in all_corners {
            groups.entry(corner.process.clone()).or_default().push(corner);
        }

        // Build ProcessGroups with sorted corners
        let mut processes: Vec<ProcessGroup> = Vec::new();
        for (process_name, mut corners) in groups {
            // Sort: TT first, FF, then SS; within each type sort by rc_type then temperature
            corners.sort_by(|a, b| {
                let type_order = |c: &LibCorner| match c.corner_type {
                    CornerType::TT => 0,
                    CornerType::FF => 1,
                    CornerType::SS => 2,
                };
                let ord = type_order(a).cmp(&type_order(b));
                if ord != std::cmp::Ordering::Equal { return ord; }
                let rc_order = |c: &LibCorner| match c.rc_type.as_str() {
                    "typ" => 0,
                    "cbest" => 1, "rcbest" => 2,
                    "cworst" => 3, "rcworst" => 4,
                    _ => 5,
                };
                rc_order(a).cmp(&rc_order(b))
                    .then(a.temperature.partial_cmp(&b.temperature).unwrap_or(std::cmp::Ordering::Equal))
            });

            processes.push(ProcessGroup { process_name, corners });
        }

        // Sort processes: special "demo" first, then by name
        processes.sort_by(|a, b| {
            if a.process_name == "demo" { return std::cmp::Ordering::Less; }
            if b.process_name == "demo" { return std::cmp::Ordering::Greater; }
            a.process_name.cmp(&b.process_name)
        });

        // Default: first non-demo process, or demo if only one
        let default_process = processes.iter()
            .find(|p| p.process_name != "demo")
            .or_else(|| processes.first())
            .map(|p| p.process_name.clone());

        CornerDatabase {
            processes,
            active_process: default_process,
            lib_dir,
            multi_corner: true,  // Default to multi-corner mode
        }
    }

    /// Parse corner info from a .lib file (fast: filename parsing + lightweight header read)
    fn parse_corner_from_file(file_path: &Path, fname: &str) -> Option<LibCorner> {
        // First, try to get library name from the file header (one line read)
        let lib_name = Self::read_library_name(file_path).unwrap_or_else(|| fname.replace(".lib", ""));

        // Detect if it's a demo/simple library (cmos_cells.lib style)
        if lib_name == "demo" || fname == "cmos_cells.lib" {
            return Some(LibCorner {
                file_path: file_path.to_path_buf(),
                lib_name: "demo".to_string(),
                process: "demo".to_string(),
                corner_type: CornerType::TT,
                voltage: 3.3,
                temperature: 25.0,
                rc_type: "typ".to_string(),
                short_name: "demo".to_string(),
                cell_count: 10,
            });
        }

        // Parse native filename pattern: {process}_{corner}_{rc}_{voltage}_{temp}_nldm.lib
        let stem = fname.strip_suffix(".lib").unwrap_or(fname);
        let stem = stem.strip_suffix("_nldm").unwrap_or(stem);

        let parts: Vec<&str> = stem.split('_').collect();
        if parts.len() < 5 {
            // Can't parse, create generic entry
            return Some(LibCorner {
                file_path: file_path.to_path_buf(),
                lib_name: lib_name.clone(),
                process: lib_name.clone(),
                corner_type: CornerType::TT,
                voltage: 1.2,
                temperature: 25.0,
                rc_type: "typ".to_string(),
                short_name: lib_name.chars().take(15).collect::<String>(),
                cell_count: 0,
            });
        }

        // Find corner type marker in parts
        // Pattern: ... [corner_type] [rc_type] [voltage] [temp] ...
        // Corner markers: "typ", "tt", "ff", "ss"
        let mut corner_type = CornerType::TT;
        let mut rc_type = String::from("typ");
        let mut voltage = 1.2;
        let mut temperature = 25.0;
        let mut corner_idx = 0;
        let mut rc_idx = 0;
        let mut process_parts: Vec<&str> = Vec::new();

        // Find the corner marker position
        for (i, part) in parts.iter().enumerate() {
            let lower = part.to_lowercase();
            if lower == "typ" || lower == "tt" {
                corner_type = CornerType::TT;
                corner_idx = i;
                break;
            } else if lower == "ff" {
                corner_type = CornerType::FF;
                corner_idx = i;
                break;
            } else if lower == "ss" {
                corner_type = CornerType::SS;
                corner_idx = i;
                break;
            }
        }

        // Everything before corner_idx is the process name
        process_parts = parts[..corner_idx].to_vec();
        let process = process_parts.join("_");

        // RC type: next part after corner type
        if corner_idx + 1 < parts.len() {
            rc_idx = corner_idx + 1;
            rc_type = parts[rc_idx].to_lowercase();
        }

        // Voltage: part after RC type
        if rc_idx + 1 < parts.len() {
            voltage = Self::parse_voltage(parts[rc_idx + 1]);
            // Check if next part is actually temperature (sometimes voltage is at position rc_idx+1 but path varies)
        }

        // Temperature: last numeric-ish part
        if parts.len() >= rc_idx + 2 {
            temperature = Self::parse_temperature(parts[parts.len() - 1]);
        }

        // Double check: if the part after rc_type looks like a temperature (not a voltage),
        // then voltage might be missing
        let voltage_part = parts[rc_idx + 1].to_lowercase();
        if !voltage_part.contains('p') && !voltage_part.contains('.') {
            voltage = 1.2; // default
            temperature = Self::parse_temperature(parts[rc_idx + 1]);
        }

        let short_name = format!("{}_{}_{:.0}_{}",
            match corner_type { CornerType::TT => "tt", CornerType::FF => "ff", CornerType::SS => "ss" },
            rc_type,
            voltage * 100.0,
            temperature);

        Some(LibCorner {
            file_path: file_path.to_path_buf(),
            lib_name: lib_name.clone(),
            process,
            corner_type,
            voltage,
            temperature,
            rc_type,
            short_name,
            cell_count: 0, // Will be populated by C API if needed
        })
    }

    /// Read just the library name from a .lib file (first line only)
    fn read_library_name(path: &Path) -> Option<String> {
        use std::io::{BufRead, BufReader};
        let file = std::fs::File::open(path).ok()?;
        let mut reader = BufReader::new(file);
        let mut first_line = String::new();
        reader.read_line(&mut first_line).ok()?;

        // Parse "library (NAME) {" or "library(NAME) {"
        if let Some(lp) = first_line.find("library") {
            let after = &first_line[lp + 7..];  // skip "library"
            if let Some(lparen) = after.find('(') {
                if let Some(rparen) = after[lparen..].find(')') {
                    let name = &after[lparen + 1..lparen + rparen];
                    return Some(name.trim().to_string());
                }
            }
        }
        None
    }

    /// Parse voltage from string like "1p2" -> 1.2, "1p08" -> 1.08, "1p32" -> 1.32, "1.2" -> 1.2
    fn parse_voltage(s: &str) -> f64 {
        let s = s.to_lowercase();
        // Handle "1p2" format
        if s.contains('p') && !s.contains('.') {
            let replaced = s.replace('p', ".");
            return replaced.parse().unwrap_or(1.2);
        }
        s.parse().unwrap_or(1.2)
    }

    /// Parse temperature from string like "25" -> 25.0, "125" -> 125.0, "m40" -> -40.0
    fn parse_temperature(s: &str) -> f64 {
        let s = s.to_lowercase();
        if s.starts_with('m') {
            // Negative: "m40" -> -40
            let val: f64 = s[1..].parse().unwrap_or(25.0);
            -val
        } else {
            s.parse().unwrap_or(25.0)
        }
    }

    // === Public API ===

    /// Get the currently active process group
    pub fn get_active_group(&self) -> Option<&ProcessGroup> {
        let active = self.active_process.as_deref()?;
        self.processes.iter().find(|p| p.process_name == active)
    }

    /// Get all corners for the active process
    pub fn get_active_corners(&self) -> Vec<&LibCorner> {
        match self.get_active_group() {
            Some(group) => group.corners.iter().collect(),
            None => vec![],
        }
    }

    /// Get default liberty file path (TT corner, or first corner)
    pub fn get_default_liberty(&self) -> Option<PathBuf> {
        let group = self.get_active_group()?;
        // Prefer TT corner for default
        group.get_tt()
            .or_else(|| group.corners.first())
            .map(|c| c.file_path.clone())
    }

    /// Get liberty paths for all corners of active process
    pub fn get_all_liberty_paths(&self) -> Vec<PathBuf> {
        self.get_active_corners().iter().map(|c| c.file_path.clone()).collect()
    }

    /// Get liberty path for a specific corner
    pub fn liberty_path_for_corner(&self, corner: &LibCorner) -> Option<PathBuf> {
        Some(corner.file_path.clone())
    }

    /// Set active process by name
    pub fn set_active_process(&mut self, name: &str) -> Result<(), String> {
        if self.processes.iter().any(|p| p.process_name == name) {
            self.active_process = Some(name.to_string());
            Ok(())
        } else {
            Err(format!("Process '{}' not found. Available: {}",
                name,
                self.processes.iter().map(|p| p.process_name.as_str()).collect::<Vec<_>>().join(", ")))
        }
    }

    /// Enable/disable multi-corner mode
    pub fn set_multi_corner(&mut self, enabled: bool) {
        self.multi_corner = enabled;
    }

    /// List all process names
    pub fn list_processes(&self) -> Vec<&str> {
        self.processes.iter().map(|p| p.process_name.as_str()).collect()
    }

    /// Get summary string for display
    pub fn summary(&self) -> String {
        let mut lines = Vec::new();
        for process in &self.processes {
            let tt_count = process.corners.iter().filter(|c| c.corner_type == CornerType::TT).count();
            let ff_count = process.corners.iter().filter(|c| c.corner_type == CornerType::FF).count();
            let ss_count = process.corners.iter().filter(|c| c.corner_type == CornerType::SS).count();
            lines.push(format!(
                "  {} ({} corners: TT={}, FF={}, SS={})",
                process.process_name, process.corners.len(), tt_count, ff_count, ss_count
            ));
        }
        lines.join("\n")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_voltage() {
        assert!((CornerDatabase::parse_voltage("1p2") - 1.2).abs() < 0.01);
        assert!((CornerDatabase::parse_voltage("1p08") - 1.08).abs() < 0.01);
        assert!((CornerDatabase::parse_voltage("1p32") - 1.32).abs() < 0.01);
        assert!((CornerDatabase::parse_voltage("1.2") - 1.2).abs() < 0.01);
    }

    #[test]
    fn test_parse_temperature() {
        assert!((CornerDatabase::parse_temperature("25") - 25.0).abs() < 0.01);
        assert!((CornerDatabase::parse_temperature("125") - 125.0).abs() < 0.01);
        assert!((CornerDatabase::parse_temperature("m40") - (-40.0)).abs() < 0.01);
        assert!((CornerDatabase::parse_temperature("m55") - (-55.0)).abs() < 0.01);
    }

    #[test]
    fn test_auto_detect() {
        let db = CornerDatabase::auto_detect(Path::new("libs"));
        for process in &db.processes {
            println!("Process: {} ({} corners)", process.process_name, process.corners.len());
            for corner in &process.corners {
                println!("  {:20} {:>3} {:>5.2}V {:>6.1}C  {}",
                    corner.short_name, corner.corner_type,
                    corner.voltage, corner.temperature, corner.rc_type);
            }
        }
    }
}
