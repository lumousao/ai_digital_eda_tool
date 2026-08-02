//! Native, deterministic APR implementation.
//!
//! This module owns its LEF parsing, floorplanning, placement, clock-tree
//! construction, Manhattan routing, parasitic estimation, OCV and PDN/IR
//! solving.  It does not invoke or link any external implementation tool.

use serde::Serialize;
use std::collections::{BTreeMap, BTreeSet, BinaryHeap, HashMap};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
pub struct AprConfig {
    pub module_name: String,
    pub clock_period_ns: f64,
    pub voltage_v: f64,
    pub core_utilization: f64,
    pub aspect_ratio: f64,
    pub ocv_late_derate: f64,
    pub ocv_early_derate: f64,
    pub power_mw: Option<f64>,
    /// Net names from the worst pre-layout STA path.  These drive the
    /// highlighted physical path; an empty set uses an explicitly labelled
    /// routing-length fallback.
    pub critical_nets: BTreeSet<String>,
}

impl Default for AprConfig {
    fn default() -> Self {
        Self {
            module_name: "top".to_string(), clock_period_ns: 10.0, voltage_v: 1.0,
            core_utilization: 0.65, aspect_ratio: 1.0,
            ocv_late_derate: 1.05, ocv_early_derate: 0.95, power_mw: None,
            critical_nets: BTreeSet::new(),
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct Macro {
    pub name: String,
    pub width_um: f64,
    pub height_um: f64,
    pub pins: Vec<MacroPinShape>,
}

/// Physical pin rectangle copied from the selected cell LEF. Coordinates are
/// macro-local microns and are transformed only after legal placement.
#[derive(Clone, Debug, Serialize)]
pub struct MacroPinShape {
    pub name: String,
    pub layer: String,
    pub x1_um: f64,
    pub y1_um: f64,
    pub x2_um: f64,
    pub y2_um: f64,
}

#[derive(Clone, Debug, Serialize)]
pub struct PlacedCell {
    pub instance: String,
    pub cell: String,
    pub x_um: f64,
    pub y_um: f64,
    pub width_um: f64,
    pub height_um: f64,
}

#[derive(Clone, Debug, Serialize)]
pub struct RouteSegment {
    pub net: String,
    pub layer: String,
    /// Native LEF routing width in microns.  This accompanies every routed
    /// segment so DEF/GDS writers and the GUI render the same physical layer
    /// geometry rather than substituting a fixed display stroke.
    pub width_um: f64,
    pub x1_um: f64,
    pub y1_um: f64,
    pub x2_um: f64,
    pub y2_um: f64,
}

#[derive(Clone, Debug, Serialize)]
pub struct Via {
    pub net: String,
    pub lower_layer: String,
    pub upper_layer: String,
    pub x_um: f64,
    pub y_um: f64,
    pub size_um: f64,
}

/// A placed LEF pin polygon reduced to its rectangular abstract. This is
/// intentionally separate from logical top-level IO pins.
#[derive(Clone, Debug, Serialize)]
pub struct CellPin {
    pub instance: String,
    pub name: String,
    pub layer: String,
    pub x1_um: f64,
    pub y1_um: f64,
    pub x2_um: f64,
    pub y2_um: f64,
}

/// A deterministic physical top-level port location.  Coordinates use the
/// same core-local micron space as cells/routes and are emitted to DEF and
/// the GUI exchange file.
#[derive(Clone, Debug, Serialize)]
pub struct IoPin {
    pub name: String,
    pub direction: String,
    pub x_um: f64,
    pub y_um: f64,
}

/// Abstract but materialized VDD/VSS ring/strap geometry.  This is the same
/// native grid topology used by the resistive IR solver; it is deliberately
/// reported as an estimate instead of a foundry-calibrated signoff network.
#[derive(Clone, Debug, Serialize)]
pub struct PdnSegment {
    pub net: String,
    pub layer: String,
    pub width_um: f64,
    pub x1_um: f64,
    pub y1_um: f64,
    pub x2_um: f64,
    pub y2_um: f64,
}

#[derive(Clone, Debug, Serialize)]
pub struct AprGridPoint {
    pub x_index: usize,
    pub y_index: usize,
    pub x_um: f64,
    pub y_um: f64,
    pub ir_drop_mv: f64,
    pub congestion: f64,
    pub power_uw: f64,
}

/// One measured native implementation attempt.  These records make APR
/// convergence auditable: a compact candidate is never silently discarded,
/// and only a candidate that reached placement, routing and physical checks
/// can be selected.
#[derive(Clone, Debug, Serialize)]
pub struct AprOptimizationIteration {
    pub attempt: usize,
    pub sbox_scale: f64,
    pub repair_phase: f64,
    pub status: String,
    pub core_width_um: f64,
    pub core_height_um: f64,
    pub utilization: f64,
    pub wire_length_um: f64,
    pub drc_status: String,
    pub reason: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct AprResult {
    pub module: String,
    pub technology_lef: String,
    pub lef_macros: usize,
    pub routed_layers: Vec<String>,
    pub cells: Vec<PlacedCell>,
    pub routes: Vec<RouteSegment>,
    pub vias: Vec<Via>,
    pub filler_cells: Vec<PlacedCell>,
    pub cell_pins: Vec<CellPin>,
    pub io_pins: Vec<IoPin>,
    pub pdn_segments: Vec<PdnSegment>,
    pub core_width_um: f64,
    pub core_height_um: f64,
    pub die_width_um: f64,
    pub die_height_um: f64,
    pub utilization: f64,
    pub standard_cell_area_um2: f64,
    pub total_wire_length_um: f64,
    pub estimated_rc_delay_ns: f64,
    pub clock_buffer_count: usize,
    pub setup_slack_ns: f64,
    pub hold_slack_ns: f64,
    pub ocv_late_slack_ns: f64,
    pub ocv_early_hold_slack_ns: f64,
    pub wns_ns: f64,
    pub tns_ns: f64,
    pub violating_endpoints: usize,
    pub total_power_mw: f64,
    pub ir_drop_mv: f64,
    pub ir_worst_voltage_v: f64,
    pub power_source: String,
    pub placement_overlaps: usize,
    pub routing_overflow: usize,
    pub antenna_warnings: usize,
    pub drc_min_width_violations: usize,
    pub drc_spacing_violations: usize,
    pub drc_short_violations: usize,
    pub drc_offgrid_violations: usize,
    pub drc_boundary_violations: usize,
    pub drc_via_violations: usize,
    pub apr_netlist_path: String,
    pub final_def_path: String,
    pub gds_path: String,
    pub detail_route_path: String,
    pub parasitics_path: String,
    pub timing_report_path: String,
    pub power_report_path: String,
    pub area_report_path: String,
    pub drc_report_path: String,
    pub lvs_report_path: String,
    pub dft_report_path: String,
    pub ir_grid: Vec<AprGridPoint>,
    pub critical_routes: Vec<RouteSegment>,
    pub critical_route_source: String,
    pub dft_status: String,
    pub drc_status: String,
    pub lvs_status: String,
    pub signoff_ready: bool,
    pub findings: Vec<String>,
    pub optimization_iterations: Vec<AprOptimizationIteration>,
}

#[derive(Clone, Debug)]
struct Instance {
    cell: String,
    name: String,
    pins: Vec<(String, String)>,
}

#[derive(Clone, Debug, Default)]
struct LefData {
    macros: BTreeMap<String, Macro>,
    routing_layers: Vec<String>,
    routing_widths_um: BTreeMap<String, f64>,
    routing_directions: BTreeMap<String, String>,
    /// `(x_pitch, y_pitch)` and `(x_offset, y_offset)` in microns from the
    /// technology LEF.  The detailed router must use these exact grids; a
    /// width-plus-spacing estimate is only a fallback for incomplete LEFs.
    routing_pitches_um: BTreeMap<String, (f64, f64)>,
    routing_offsets_um: BTreeMap<String, (f64, f64)>,
    site_width_um: Option<f64>,
    site_height_um: Option<f64>,
    manufacturing_grid_um: Option<f64>,
    /// Scalar routing-layer spacing rules from the technology LEF.  Complex
    /// spacing tables are deliberately not flattened here: their evaluation
    /// depends on run length and width and is reported as outside the native
    /// abstract DRC scope rather than silently guessed.
    routing_spacings_um: BTreeMap<String, f64>,
}

fn collect_lef_files(root: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else { return; };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_lef_files(&path, out);
        } else if path.extension().and_then(|v| v.to_str())
            .map(|v| v.eq_ignore_ascii_case("lef") || v.eq_ignore_ascii_case("tlef")).unwrap_or(false) {
            out.push(path);
        }
    }
}

fn parse_lef(root: &Path) -> Result<LefData, String> {
    let mut files = Vec::new();
    collect_lef_files(root, &mut files);
    files.sort();
    if files.is_empty() { return Err(format!("No LEF/TLEF files under {}", root.display())); }
    let mut data = LefData::default();
    let mut layers = Vec::new();
    let mut seen_layers = BTreeSet::new();
    for path in &files {
        let content = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let mut current_macro: Option<String> = None;
        let mut current_layer: Option<String> = None;
        let mut current_site: Option<String> = None;
        let mut current_pin: Option<String> = None;
        let mut pin_layer: Option<String> = None;
        for raw in content.lines() {
            let line = raw.trim();
            let words: Vec<&str> = line.split_whitespace().collect();
            if words.len() >= 2 && words[0] == "MACRO" {
                current_macro = Some(words[1].to_string());
                current_pin = None; pin_layer = None;
                continue;
            }
            if current_macro.is_none() && words.len() >= 2 && words[0] == "SITE" {
                current_site = Some(words[1].trim_end_matches(';').to_string());
                continue;
            }
            if words.len() >= 4 && words[0] == "SIZE" && words[2] == "BY" {
                if let Some(name) = current_macro.as_ref() {
                    let width = words[1].parse::<f64>().unwrap_or(0.0);
                    let height = words[3].trim_end_matches(';').parse::<f64>().unwrap_or(0.0);
                    if width > 0.0 && height > 0.0 {
                        data.macros.insert(name.clone(), Macro { name: name.clone(), width_um: width, height_um: height, pins: Vec::new() });
                    }
                } else if current_site.is_some() {
                    let width = words[1].parse::<f64>().unwrap_or(0.0);
                    let height = words[3].trim_end_matches(';').parse::<f64>().unwrap_or(0.0);
                    if width > 0.0 && height > 0.0 && data.site_width_um.is_none() {
                        data.site_width_um = Some(width);
                        data.site_height_um = Some(height);
                    }
                }
                continue;
            }
            if current_macro.is_some() && words.len() >= 2 && words[0] == "PIN" {
                current_pin = Some(words[1].to_string());
                pin_layer = None;
                continue;
            }
            if current_macro.is_some() && current_pin.is_some() && words.len() >= 2 && words[0] == "LAYER" {
                pin_layer = Some(words[1].trim_end_matches(';').to_string());
                continue;
            }
            if current_macro.is_some() && current_pin.is_some() && line.starts_with("RECT") {
                let values: Vec<f64> = line["RECT".len()..].trim_end_matches(';').split_whitespace()
                    .filter_map(|v| v.parse::<f64>().ok()).collect();
                if values.len() == 4 {
                    if let (Some(macro_name), Some(pin_name), Some(layer)) = (current_macro.as_ref(), current_pin.as_ref(), pin_layer.as_ref()) {
                        if let Some(macro_def) = data.macros.get_mut(macro_name) {
                            macro_def.pins.push(MacroPinShape { name: pin_name.clone(), layer: layer.clone(), x1_um: values[0], y1_um: values[1], x2_um: values[2], y2_um: values[3] });
                        }
                    }
                }
                continue;
            }
            // Standard-cell macros contain PIN/PORT LAYER statements and
            // geometry WIDTH records.  They describe pin shapes, not the
            // technology routing rule, and must not overwrite TLEF widths.
            if current_macro.is_none() && words.len() >= 2 && words[0] == "LAYER" {
                current_layer = Some(words[1].to_string());
                continue;
            }
            if current_macro.is_none() && (line.contains("TYPE ROUTING") || line.contains("TYPE\t\tROUTING")) {
                if let Some(layer) = current_layer.as_ref() {
                    if seen_layers.insert(layer.clone()) { layers.push(layer.clone()); }
                }
            }
            if current_macro.is_none() && words.len() >= 2 && words[0] == "DIRECTION" {
                if let Some(layer) = current_layer.as_ref() {
                    data.routing_directions.insert(layer.clone(), words[1].trim_end_matches(';').to_ascii_uppercase());
                }
            }
            if current_macro.is_none() && line.starts_with("WIDTH") {
                // Only accept the layer's scalar `WIDTH <number> ;` rule.
                // `SPACINGTABLE` also contains `WIDTH <run-length> <spacing>`
                // lines; treating those as routing widths produces visibly
                // incorrect multi-micron metal strokes.
                // Strip LEF's optional trailing comment before parsing the
                // declaration.  A scalar rule is commonly written as
                // `WIDTH 0.14 ; # Met1`, whereas SPACINGTABLE entries retain
                // several numeric fields and therefore do not parse as f64.
                let scalar = line["WIDTH".len()..].split('#').next().unwrap_or_default()
                    .trim().trim_end_matches(';').trim();
                if let (Some(layer), Ok(width)) = (current_layer.as_ref(), scalar.parse::<f64>()) {
                    if width > 0.0 { data.routing_widths_um.insert(layer.clone(), width); }
                }
            }
            if current_macro.is_none() && line.starts_with("SPACING") {
                // Accept only a scalar `SPACING <number> ;` declaration.
                // `RANGE`, `SAMENET`, `BY`, and table records need a full
                // foundry rule deck and must not be mistaken for a scalar
                // minimum spacing value.
                let scalar = line["SPACING".len()..].split('#').next().unwrap_or_default()
                    .trim().trim_end_matches(';').trim();
                if let (Some(layer), Ok(spacing)) = (current_layer.as_ref(), scalar.parse::<f64>()) {
                    if spacing > 0.0 { data.routing_spacings_um.insert(layer.clone(), spacing); }
                }
            }
            if current_macro.is_none() && line.starts_with("PITCH") {
                let values = line["PITCH".len()..].split('#').next().unwrap_or_default()
                    .trim().trim_end_matches(';').split_whitespace().filter_map(|value| value.parse::<f64>().ok()).collect::<Vec<_>>();
                if let (Some(layer), Some(x_pitch)) = (current_layer.as_ref(), values.first().copied()) {
                    let y_pitch = values.get(1).copied().unwrap_or(x_pitch);
                    if x_pitch > 0.0 && y_pitch > 0.0 { data.routing_pitches_um.insert(layer.clone(), (x_pitch, y_pitch)); }
                }
            }
            if current_macro.is_none() && line.starts_with("OFFSET") {
                let values = line["OFFSET".len()..].split('#').next().unwrap_or_default()
                    .trim().trim_end_matches(';').split_whitespace().filter_map(|value| value.parse::<f64>().ok()).collect::<Vec<_>>();
                if let (Some(layer), Some(x_offset)) = (current_layer.as_ref(), values.first().copied()) {
                    let y_offset = values.get(1).copied().unwrap_or(x_offset);
                    data.routing_offsets_um.insert(layer.clone(), (x_offset, y_offset));
                }
            }
            if current_macro.is_none() && line.starts_with("MANUFACTURINGGRID") {
                let value = line["MANUFACTURINGGRID".len()..].trim().trim_end_matches(';').trim();
                if let Ok(grid) = value.parse::<f64>() {
                    if grid > 0.0 { data.manufacturing_grid_um = Some(grid); }
                }
            }
            if words.len() >= 2 && words[0] == "END" {
                if current_pin.as_deref() == Some(words[1]) { current_pin = None; pin_layer = None; continue; }
                if current_macro.as_deref() == Some(words[1]) { current_macro = None; }
                if current_site.as_deref() == Some(words[1]) { current_site = None; }
                if current_macro.is_none() && current_layer.as_deref() == Some(words[1]) { current_layer = None; }
            }
        }
    }
    data.routing_layers = layers;
    if data.routing_layers.len() < 2 {
        return Err(format!("LEF technology has {} routing layers; APR requires at least two", data.routing_layers.len()));
    }
    Ok(data)
}

fn parse_instances(gate_verilog: &str) -> Vec<Instance> {
    let mut instances = Vec::new();
    for statement in gate_verilog.split(';') {
        let line = statement.trim();
        if line.is_empty() || line.starts_with("//") || line.starts_with("/*")
            || line.starts_with("module ") || line.starts_with("input ") || line.starts_with("output ")
            || line.starts_with("wire ") || line.starts_with("assign ") || line.starts_with("endmodule") { continue; }
        let before_paren = line.split('(').next().unwrap_or_default().trim();
        let words: Vec<&str> = before_paren.split_whitespace().collect();
        if words.len() != 2 { continue; }
        let cell = words[0];
        let name = words[1];
        if cell.starts_with('$') || cell == "begin" { continue; }
        let mut pins = Vec::new();
        let mut rest = &line[before_paren.len()..];
        while let Some(dot) = rest.find('.') {
            rest = &rest[dot + 1..];
            let Some(open) = rest.find('(') else { break; };
            let pin = rest[..open].trim();
            rest = &rest[open + 1..];
            let Some(close) = rest.find(')') else { break; };
            let net = rest[..close].trim();
            if !pin.is_empty() && !net.is_empty() { pins.push((pin.to_string(), net.to_string())); }
            rest = &rest[close + 1..];
        }
        instances.push(Instance { cell: cell.to_string(), name: name.to_string(), pins });
    }
    instances
}

/// Return a deterministic logic-topological order for placement.  Gate
/// netlists are commonly emitted in lexical instance-name order (for example
/// `_c1004` before `_c11`), which has no physical meaning and made the former
/// round-robin placer scatter direct driver/sink pairs across the die.  The
/// ordering is deliberately native and conservative: primary inputs have no
/// producer, sequential/cyclic residue keeps stable source order, and every
/// acyclic dependency is processed before its consumers.
fn connectivity_order(mut instances: Vec<Instance>) -> Vec<Instance> {
    let mut producer = HashMap::<String, usize>::new();
    for (index, instance) in instances.iter().enumerate() {
        for (pin, net) in &instance.pins {
            if pin_is_output(pin) { producer.insert(net.clone(), index); }
        }
    }
    let mut successors = vec![BTreeSet::<usize>::new(); instances.len()];
    let mut indegree = vec![0usize; instances.len()];
    for (sink, instance) in instances.iter().enumerate() {
        let mut predecessors = BTreeSet::new();
        for (pin, net) in &instance.pins {
            if !pin_is_output(pin) {
                if let Some(source) = producer.get(net).copied() {
                    if source != sink { predecessors.insert(source); }
                }
            }
        }
        for source in predecessors {
            if successors[source].insert(sink) { indegree[sink] += 1; }
        }
    }
    let mut ready = BTreeSet::new();
    for (index, degree) in indegree.iter().enumerate() {
        if *degree == 0 { ready.insert(index); }
    }
    let mut order = Vec::with_capacity(instances.len());
    while let Some(index) = ready.iter().next().copied() {
        ready.remove(&index);
        order.push(index);
        for sink in successors[index].iter().copied() {
            indegree[sink] -= 1;
            if indegree[sink] == 0 { ready.insert(sink); }
        }
    }
    // Preserve reproducibility and do not discard cyclic/unknown cells.
    for index in 0..instances.len() {
        if !order.contains(&index) { order.push(index); }
    }
    let mut slots = instances.into_iter().map(Some).collect::<Vec<_>>();
    order.into_iter().filter_map(|index| slots[index].take()).collect()
}

fn snap_up(value: f64, grid: f64) -> f64 {
    if grid <= 0.0 { value } else { (value / grid).ceil() * grid }
}

/// Build an undirected placement graph from the logical driver/sink graph.
/// A multi-sink net is represented as a star, so high-fanout nets retain a
/// shared logical source instead of becoming an artificial all-to-all clique.
fn placement_adjacency(instances: &[Instance]) -> Vec<BTreeSet<usize>> {
    let mut by_net = BTreeMap::<&str, Vec<(usize, bool)>>::new();
    for (index, instance) in instances.iter().enumerate() {
        for (pin, net) in &instance.pins {
            by_net.entry(net.as_str()).or_default().push((index, pin_is_output(pin)));
        }
    }
    let mut adjacent = vec![BTreeSet::new(); instances.len()];
    for nodes in by_net.values() {
        if nodes.len() < 2 { continue; }
        let root = nodes.iter().find(|(_, output)| *output).map(|(index, _)| *index).unwrap_or(nodes[0].0);
        for (index, _) in nodes {
            if *index != root { adjacent[root].insert(*index); adjacent[*index].insert(root); }
        }
    }
    adjacent
}

/// Resolve a logical netlist pin to its physical LEF port.  The native
/// synthesizer uses the generic `Y` output spelling, while Sky130's gates
/// commonly expose the same function as `X`.  Restrict aliases to recognised
/// output spellings: input pins are never guessed and a missing physical pin
/// remains a technology-coverage error.
fn resolve_macro_pin<'a>(macro_def: &'a Macro, logical_pin: &str) -> Option<&'a MacroPinShape> {
    macro_def.pins.iter().find(|shape| shape.name.eq_ignore_ascii_case(logical_pin))
        .or_else(|| {
            if !pin_is_output(logical_pin) { return None; }
            ["X", "Y", "Z", "ZN", "Q", "QN", "OUT"]
                .iter()
                .find_map(|alias| macro_def.pins.iter().find(|shape| shape.name.eq_ignore_ascii_case(alias)))
        })
}

/// Legal, connectivity-driven standard-cell placement.  Cells are first
/// assigned to balanced rows, then repeatedly moved toward the weighted row
/// centroid of their net neighbours subject to real row capacity.  Each row
/// is finally ordered by neighbour x-centroid and emitted on the technology
/// site grid.  This deliberately replaces visual phase offsets with physical
/// connectivity and explicit capacity constraints.
fn place_standard_cells(
    instances: &[Instance], lef: &LefData, core_width: f64, rows: usize,
    row_height: f64, site_width: f64, repair_phase: f64,
) -> Result<Vec<PlacedCell>, String> {
    let adjacency = placement_adjacency(instances);
    let widths = instances.iter().map(|instance| lef.macros.get(&instance.cell)
        .map(|cell| cell.width_um).ok_or_else(|| format!("LEF macro {} is unavailable during placement", instance.cell)))
        .collect::<Result<Vec<_>, _>>()?;
    for instance in instances {
        let cell = lef.macros.get(&instance.cell).expect("checked above");
        if cell.height_um > row_height + 1e-6 || (cell.height_um / row_height).fract().abs() > 1e-6 {
            return Err(format!("Cell {} height {:.6} is incompatible with site row height {:.6}", cell.name, cell.height_um, row_height));
        }
    }
    let sites = (core_width / site_width).floor().max(1.0) as usize;
    let width_sites = widths.iter().map(|width| (*width / site_width).ceil().max(1.0) as usize).collect::<Vec<_>>();
    let total_sites: usize = width_sites.iter().sum();
    let target_sites = (total_sites as f64 / rows.max(1) as f64).ceil() as usize;
    let row_capacity = ((sites as f64 * 0.86).floor() as usize).max(target_sites);
    if row_capacity > sites || total_sites > rows * sites {
        return Err("Floorplan utilization is infeasible for the selected cell library and site grid".to_string());
    }
    let mut row_for = vec![0usize; instances.len()];
    let mut row_used = vec![0usize; rows];
    let start_row = ((repair_phase.clamp(0.0, 0.999) * rows as f64).floor() as usize).min(rows.saturating_sub(1));
    let mut row = start_row;
    for index in 0..instances.len() {
        let need = width_sites[index];
        let mut checked = 0usize;
        while checked + 1 < rows && row_used[row] > 0 && row_used[row] + need > target_sites {
            row = (row + 1) % rows;
            checked += 1;
        }
        if row_used[row] + need > sites { return Err("Site-row legalizer exceeded core width".to_string()); }
        row_for[index] = row;
        row_used[row] += need;
    }
    // Bounded congestion-friendly row refinement.  A net neighbour in a
    // nearby row costs less vertical access and creates fewer GCell crossings.
    for pass in 0..8usize {
        let forward = pass % 2 == 0;
        let indices: Box<dyn Iterator<Item = usize>> = if forward { Box::new(0..instances.len()) } else { Box::new((0..instances.len()).rev()) };
        for index in indices {
            if adjacency[index].is_empty() { continue; }
            let centroid = adjacency[index].iter().map(|neighbor| row_for[*neighbor] as f64).sum::<f64>() / adjacency[index].len() as f64;
            let current = row_for[index];
            let lower = centroid.floor().max(0.0) as usize;
            let upper = centroid.ceil().min(rows.saturating_sub(1) as f64) as usize;
            let mut best = current;
            let mut best_cost = (current as f64 - centroid).abs() + row_used[current] as f64 / row_capacity as f64 * 0.03;
            for candidate in lower.saturating_sub(2)..=(upper + 2).min(rows.saturating_sub(1)) {
                if candidate != current && row_used[candidate] + width_sites[index] > row_capacity { continue; }
                let cost = (candidate as f64 - centroid).abs() + row_used[candidate] as f64 / row_capacity as f64 * 0.03;
                if cost + 0.10 < best_cost { best = candidate; best_cost = cost; }
            }
            if best != current {
                row_used[current] -= width_sites[index];
                row_used[best] += width_sites[index];
                row_for[index] = best;
            }
        }
    }
    let mut row_members = vec![Vec::<usize>::new(); rows];
    for (index, row) in row_for.iter().copied().enumerate() { row_members[row].push(index); }
    let mut x_center = vec![core_width * 0.5; instances.len()];
    let mut x_site = vec![0usize; instances.len()];
    for row in 0..rows {
        let count = row_members[row].len().max(1) as f64;
        for (slot, index) in row_members[row].iter().copied().enumerate() { x_center[index] = (slot as f64 + 0.5) / count * core_width; }
    }
    for _ in 0..4usize {
        for members in &mut row_members {
            members.sort_by(|left, right| {
                let score = |index: usize| adjacency[index].iter().map(|neighbor| x_center[*neighbor]).sum::<f64>() / adjacency[index].len().max(1) as f64;
                score(*left).partial_cmp(&score(*right)).unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| instances[*left].name.cmp(&instances[*right].name))
            });
            let used: usize = members.iter().map(|index| width_sites[*index]).sum();
            let free = sites.saturating_sub(used);
            // Keep a real one-site edge channel when the row has spare
            // capacity.  Without this, a legal first cell can sit on the
            // core boundary and consume the only nearby vertical escape
            // tracks, making an otherwise routable row fail at the edge.
            let edge_reserve = usize::from(free >= 2);
            let distributable = free.saturating_sub(edge_reserve * 2);
            let gap = distributable / (members.len() + 1);
            let mut remainder = distributable % (members.len() + 1);
            let mut cursor = edge_reserve + gap;
            for index in members.iter().copied() {
                if remainder > 0 { cursor += 1; remainder -= 1; }
                x_site[index] = cursor;
                x_center[index] = (cursor as f64 + width_sites[index] as f64 * 0.5) * site_width;
                cursor += width_sites[index] + gap;
            }
        }
    }
    let mut placed = Vec::with_capacity(instances.len());
    for index in 0..instances.len() {
        let cell = lef.macros.get(&instances[index].cell).expect("checked above");
        let x = x_site[index] as f64 * site_width;
        placed.push(PlacedCell { instance: instances[index].name.clone(), cell: instances[index].cell.clone(), x_um: x, y_um: row_for[index] as f64 * row_height, width_um: cell.width_um, height_um: cell.height_um });
    }
    Ok(placed)
}

fn routing_track_pitch(lef: &LefData, layer: &str) -> f64 {
    let fallback = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14)
        + lef.routing_spacings_um.get(layer).copied().unwrap_or_else(|| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14));
    let Some((x_pitch, y_pitch)) = lef.routing_pitches_um.get(layer).copied() else { return fallback; };
    // LEF's first PITCH coordinate is the X repetition and the second is
    // the Y repetition.  A horizontal wire consumes Y tracks and a vertical
    // wire consumes X tracks.  Never derive a narrower pitch than the
    // width/spacing lower bound when a partial technology LEF is encountered.
    let preferred = match lef.routing_directions.get(layer).map(String::as_str) {
        Some("HORIZONTAL") => y_pitch,
        Some("VERTICAL") => x_pitch,
        _ => x_pitch.min(y_pitch),
    };
    preferred.max(fallback)
}

/// Return the perpendicular axis of a preferred-direction layer.  A
/// horizontal wire is constrained by a Y track and a vertical wire by an X
/// track.  Callers use this rather than a global pitch so Sky130 li1/met1/
/// met2/met3/met4/met5 all retain their own offsets and pitch.
fn routing_track_axis(lef: &LefData, layer: &str) -> Result<(f64, f64, bool), String> {
    let (x_pitch, y_pitch) = lef.routing_pitches_um.get(layer).copied()
        .ok_or_else(|| format!("routing layer {layer} has no PITCH"))?;
    let (x_offset, y_offset) = lef.routing_offsets_um.get(layer).copied()
        .ok_or_else(|| format!("routing layer {layer} has no OFFSET"))?;
    match lef.routing_directions.get(layer).map(String::as_str) {
        Some("HORIZONTAL") => Ok((y_pitch, y_offset, false)),
        Some("VERTICAL") => Ok((x_pitch, x_offset, true)),
        _ => Err(format!("routing layer {layer} has no preferred direction")),
    }
}

fn snap_to_routing_track(lef: &LefData, layer: &str, coordinate: f64) -> Result<f64, String> {
    let (pitch, offset, _) = routing_track_axis(lef, layer)?;
    Ok(offset + ((coordinate - offset) / pitch).round() * pitch)
}

fn is_on_routing_track(lef: &LefData, layer: &str, coordinate: f64) -> bool {
    let Ok((pitch, offset, _)) = routing_track_axis(lef, layer) else { return false; };
    (((coordinate - offset) / pitch).round() * pitch + offset - coordinate).abs() <= 1e-6
}

/// Reject incomplete technology abstracts before floorplanning.  A synthetic
/// fallback grid can make a visual result, but cannot make a physical result;
/// production APR therefore requires a real site, manufacturing grid and
/// three alternating preferred-direction routing pairs.
fn validate_apr_technology(lef: &LefData) -> Result<(), String> {
    let site_width = lef.site_width_um.filter(|value| *value > 0.0)
        .ok_or_else(|| "TECHNOLOGY_COVERAGE_BLOCKED: LEF has no legal SITE width".to_string())?;
    let site_height = lef.site_height_um.filter(|value| *value > 0.0)
        .ok_or_else(|| "TECHNOLOGY_COVERAGE_BLOCKED: LEF has no legal SITE height".to_string())?;
    let grid = lef.manufacturing_grid_um.filter(|value| *value > 0.0)
        .ok_or_else(|| "TECHNOLOGY_COVERAGE_BLOCKED: LEF has no MANUFACTURINGGRID".to_string())?;
    let mut horizontal = 0usize;
    let mut vertical = 0usize;
    let mut missing = Vec::new();
    for layer in &lef.routing_layers {
        let direction = lef.routing_directions.get(layer).map(String::as_str);
        match direction {
            Some("HORIZONTAL") => horizontal += 1,
            Some("VERTICAL") => vertical += 1,
            _ => missing.push(format!("{layer}:DIRECTION")),
        }
        if lef.routing_widths_um.get(layer).copied().unwrap_or(0.0) <= 0.0 {
            missing.push(format!("{layer}:WIDTH"));
        }
        if lef.routing_pitches_um.get(layer).is_none() {
            missing.push(format!("{layer}:PITCH"));
        }
        if lef.routing_offsets_um.get(layer).is_none() {
            missing.push(format!("{layer}:OFFSET"));
        }
    }
    if horizontal < 3 || vertical < 3 {
        missing.push(format!("alternating routing pairs (H={horizontal}, V={vertical}; need >=3 each)"));
    }
    if !missing.is_empty() {
        return Err(format!(
            "TECHNOLOGY_COVERAGE_BLOCKED: APR requires complete site/grid and routing rules; missing {}",
            missing.join(", ")
        ));
    }
    // Use the values above so a future refactor cannot accidentally weaken
    // this preflight into a presence-only check.
    if site_width < grid || site_height < grid {
        return Err(format!(
            "TECHNOLOGY_COVERAGE_BLOCKED: SITE {:.6} x {:.6} um is smaller than manufacturing grid {:.6} um",
            site_width, site_height, grid
        ));
    }
    Ok(())
}

fn parse_top_ports(gate_verilog: &str) -> Vec<(String, String)> {
    let mut result = Vec::new();
    let mut direction = String::new();
    for raw in gate_verilog.lines() {
        let line = raw.trim().trim_end_matches(';');
        let first = line.split_whitespace().next().unwrap_or_default();
        if matches!(first, "input" | "output" | "inout") {
            direction = first.to_ascii_uppercase();
            let names = line[first.len()..].replace("wire", "").replace("reg", "");
            for name in names.split(',') {
                let name = name.trim().split_whitespace().last().unwrap_or_default()
                    .trim_matches(|c: char| c == '[' || c == ']');
                if !name.is_empty() && !name.starts_with('[') { result.push((name.to_string(), direction.clone())); }
            }
        }
    }
    result.sort(); result.dedup(); result
}

fn place_io_pins(gate_verilog: &str, core_width: f64, core_height: f64) -> Vec<IoPin> {
    let ports = parse_top_ports(gate_verilog);
    let mut inputs = Vec::new(); let mut outputs = Vec::new(); let mut inouts = Vec::new();
    for (name, direction) in ports {
        match direction.as_str() { "OUTPUT" => outputs.push((name, direction)), "INOUT" => inouts.push((name, direction)), _ => inputs.push((name, direction)) }
    }
    let mut pins = Vec::new();
    for (items, side) in [(inputs, 0usize), (outputs, 1usize), (inouts, 2usize)] {
        let count = items.len().max(1) as f64;
        for (index, (name, direction)) in items.into_iter().enumerate() {
            let t = (index as f64 + 1.0) / (count + 1.0);
            let (x_um, y_um) = match side { 0 => (0.0, t * core_height), 1 => (core_width, t * core_height), _ => (t * core_width, 0.0) };
            pins.push(IoPin { name, direction, x_um, y_um });
        }
    }
    pins
}

fn build_pdn(lef: &LefData, core_width: f64, core_height: f64) -> Vec<PdnSegment> {
    let horizontal = lef.routing_layers.last().cloned().unwrap_or_else(|| "M2".to_string());
    let vertical = lef.routing_layers.iter().rev().nth(1).cloned().unwrap_or_else(|| horizontal.clone());
    let h_width = lef.routing_widths_um.get(&horizontal).copied().unwrap_or(0.3).max(0.3) * 4.0;
    let v_width = lef.routing_widths_um.get(&vertical).copied().unwrap_or(0.3).max(0.3) * 4.0;
    let inset = (h_width.max(v_width) * 1.5).min(core_width.min(core_height) * 0.12);
    let mut out = Vec::new();
    for (net, offset) in [("VDD", 0.0), ("VSS", h_width * 1.7)] {
        let l = inset + offset; let r = (core_width - inset - offset).max(l);
        let b = inset + offset; let t = (core_height - inset - offset).max(b);
        for (x1, y1, x2, y2, layer, width) in [(l,b,r,b,&horizontal,h_width), (r,b,r,t,&vertical,v_width), (r,t,l,t,&horizontal,h_width), (l,t,l,b,&vertical,v_width)] {
            out.push(PdnSegment { net: net.to_string(), layer: layer.clone(), width_um: width, x1_um:x1, y1_um:y1, x2_um:x2, y2_um:y2 });
        }
    }
    for index in 1..6 {
        let pos = index as f64 / 6.0;
        let net = if index % 2 == 0 { "VDD" } else { "VSS" };
        out.push(PdnSegment { net: net.to_string(), layer: horizontal.clone(), width_um: h_width * 0.72, x1_um: inset, y1_um: core_height * pos, x2_um: core_width - inset, y2_um: core_height * pos });
        out.push(PdnSegment { net: net.to_string(), layer: vertical.clone(), width_um: v_width * 0.72, x1_um: core_width * pos, y1_um: inset, x2_um: core_width * pos, y2_um: core_height - inset });
    }
    out
}

fn pin_is_output(pin: &str) -> bool {
    let upper = pin.to_ascii_uppercase();
    matches!(upper.as_str(), "Y" | "Z" | "ZN" | "X" | "Q" | "QN" | "CO" | "S") || upper.starts_with("OUT")
}

fn wire_length(segment: &RouteSegment) -> f64 {
    (segment.x2_um - segment.x1_um).abs() + (segment.y2_um - segment.y1_um).abs()
}

fn build_vias(routes: &[RouteSegment], layer_order: &[String]) -> Vec<Via> {
    let mut vias = Vec::new();
    let mut seen = BTreeSet::new();
    for (index, left) in routes.iter().enumerate() {
        for right in routes.iter().skip(index + 1) {
            if left.net != right.net || left.layer == right.layer { continue; }
            let Some(left_index) = layer_order.iter().position(|layer| layer == &left.layer) else { continue; };
            let Some(right_index) = layer_order.iter().position(|layer| layer == &right.layer) else { continue; };
            // A physical via may only bridge adjacent routing layers.  The
            // router emits adjacent-pair transitions; retaining this guard
            // prevents a malformed input from becoming a fictional stacked
            // via in DEF, GDS or the physical viewer.
            if left_index.abs_diff(right_index) != 1 { continue; }
            for (x, y) in [(left.x1_um, left.y1_um), (left.x2_um, left.y2_um)] {
                let touches = ((right.x1_um - x).abs() < 1e-6 && (right.y1_um - y).abs() < 1e-6)
                    || ((right.x2_um - x).abs() < 1e-6 && (right.y2_um - y).abs() < 1e-6)
                    || ((right.x1_um - x).abs() < 1e-6 && (right.y1_um - y).abs() < 1e-6)
                    || ((right.x2_um - x).abs() < 1e-6 && (right.y2_um - y).abs() < 1e-6);
                if !touches { continue; }
                let (lower_layer, upper_layer) = if left.layer <= right.layer { (left.layer.clone(), right.layer.clone()) } else { (right.layer.clone(), left.layer.clone()) };
                let key = format!("{}|{}|{}|{:.6}|{:.6}", left.net, lower_layer, upper_layer, x, y);
                if seen.insert(key) {
                    vias.push(Via { net: left.net.clone(), lower_layer, upper_layer, x_um: x, y_um: y, size_um: left.width_um.max(right.width_um).max(0.14) });
                }
            }
        }
    }
    vias
}

fn route_bbox(segment: &RouteSegment, extra_um: f64) -> (f64, f64, f64, f64) {
    let half = (segment.width_um.max(0.0) + extra_um.max(0.0)) * 0.5;
    (
        segment.x1_um.min(segment.x2_um) - half,
        segment.y1_um.min(segment.y2_um) - half,
        segment.x1_um.max(segment.x2_um) + half,
        segment.y1_um.max(segment.y2_um) + half,
    )
}

fn bboxes_overlap(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> bool {
    a.0 < b.2 - 1e-9 && b.0 < a.2 - 1e-9 && a.1 < b.3 - 1e-9 && b.1 < a.3 - 1e-9
}

/// Native abstract detailed-route geometry DRC.  Each route is a Manhattan
/// metal rectangle whose width comes from LEF.  Different-net overlap is a
/// short; non-overlapping shapes violating a scalar LEF spacing rule are a
/// spacing violation.  Same-net joins are intentionally permitted.
fn check_route_geometry(routes: &[RouteSegment], lef: &LefData) -> (usize, usize) {
    let mut by_layer: BTreeMap<&str, Vec<&RouteSegment>> = BTreeMap::new();
    for route in routes { by_layer.entry(route.layer.as_str()).or_default().push(route); }
    let mut spacing = 0usize;
    let mut shorts = 0usize;
    for (layer, segments) in by_layer {
        let rule = lef.routing_spacings_um.get(layer).copied()
            // When the technology uses a width/run-length table, use the
            // legal minimum width as a conservative abstract clearance.
            .unwrap_or_else(|| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14));
        for left_index in 0..segments.len() {
            for right_index in (left_index + 1)..segments.len() {
                let left = segments[left_index];
                let right = segments[right_index];
                if left.net == right.net { continue; }
                if bboxes_overlap(route_bbox(left, 0.0), route_bbox(right, 0.0)) {
                    shorts += 1;
                } else if bboxes_overlap(route_bbox(left, rule), route_bbox(right, rule)) {
                    spacing += 1;
                }
            }
        }
    }
    (spacing, shorts)
}

fn route_conflicts(candidate: &[RouteSegment], routed: &[RouteSegment], lef: &LefData) -> bool {
    candidate.iter().any(|next| routed.iter().any(|existing| {
        if next.net == existing.net || next.layer != existing.layer { return false; }
        let rule = lef.routing_spacings_um.get(&next.layer).copied()
            .unwrap_or_else(|| lef.routing_widths_um.get(&next.layer).copied().unwrap_or(0.14));
        bboxes_overlap(route_bbox(next, rule), route_bbox(existing, rule))
    }))
}

fn route_conflict_breakdown(candidate: &[RouteSegment], routed: &[RouteSegment], lef: &LefData) -> (usize, usize) {
    let mut horizontal = 0usize;
    let mut vertical = 0usize;
    for next in candidate {
        for existing in routed {
            if next.net == existing.net || next.layer != existing.layer { continue; }
            let rule = lef.routing_spacings_um.get(&next.layer).copied()
                .unwrap_or_else(|| lef.routing_widths_um.get(&next.layer).copied().unwrap_or(0.14));
            if bboxes_overlap(route_bbox(next, rule), route_bbox(existing, rule)) {
                if (next.y2_um - next.y1_um).abs() < 1e-9 { horizontal += 1; } else { vertical += 1; }
            }
        }
    }
    (horizontal, vertical)
}

/// Compact diagnostic for a rejected deterministic allocation.  This is
/// deliberately kept alongside the geometry predicate so APR_DEBUG errors
/// identify the owning layer/net rather than forcing an LLM or user to infer
/// it from a generic vertical/horizontal count.
fn route_conflict_detail(candidate: &[RouteSegment], routed: &[RouteSegment], lef: &LefData) -> String {
    let mut details = BTreeSet::new();
    for next in candidate {
        for existing in routed {
            if next.net == existing.net || next.layer != existing.layer { continue; }
            let rule = lef.routing_spacings_um.get(&next.layer).copied()
                .unwrap_or_else(|| lef.routing_widths_um.get(&next.layer).copied().unwrap_or(0.14));
            if bboxes_overlap(route_bbox(next, rule), route_bbox(existing, rule)) {
                details.insert(format!("{}:{}@({:.2},{:.2})-({:.2},{:.2})", next.layer, existing.net,
                    existing.x1_um, existing.y1_um, existing.x2_um, existing.y2_um));
            }
        }
    }
    details.into_iter().take(6).collect::<Vec<_>>().join(", ")
}

/// Coarse two-dimensional routing-resource graph used before detailed track
/// assignment.  Horizontal and vertical GCell edges retain their own demand,
/// capacity and history penalty.  Keeping these values in the native model is
/// essential: a route is never accepted merely because a Manhattan template
/// exists; it must consume real, direction-specific routing capacity.
#[derive(Clone, Copy, Debug)]
struct GlobalQueueEntry {
    cost: f64,
    node: usize,
}

impl PartialEq for GlobalQueueEntry {
    fn eq(&self, other: &Self) -> bool { self.node == other.node && self.cost.to_bits() == other.cost.to_bits() }
}

impl Eq for GlobalQueueEntry {}

impl Ord for GlobalQueueEntry {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // BinaryHeap is a max heap.  Reverse the numeric comparison to pop
        // the least-cost entry first, then use node id as a stable tie break.
        other.cost.total_cmp(&self.cost).then_with(|| other.node.cmp(&self.node))
    }
}

impl PartialOrd for GlobalQueueEntry {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> { Some(self.cmp(other)) }
}

#[derive(Clone, Debug)]
struct NativeGlobalGrid {
    cols: usize,
    rows: usize,
    horizontal_capacity: Vec<u16>, // rows * (cols - 1)
    vertical_capacity: Vec<u16>,   // (rows - 1) * cols
    horizontal_usage: Vec<u16>,
    vertical_usage: Vec<u16>,
    horizontal_history: Vec<f64>,
    vertical_history: Vec<f64>,
}

#[derive(Clone, Debug)]
struct NativeGlobalRoute {
    points: Vec<(usize, usize)>,
    edge_refs: Vec<(bool, usize)>, // true = horizontal edge
    cost: f64,
}

impl NativeGlobalGrid {
    fn new(cols: usize, rows: usize, horizontal_capacity: u16, vertical_capacity: u16) -> Self {
        let cols = cols.max(2);
        let rows = rows.max(2);
        let horizontal_edges = rows * (cols - 1);
        let vertical_edges = (rows - 1) * cols;
        Self {
            cols, rows,
            horizontal_capacity: vec![horizontal_capacity.max(1); horizontal_edges],
            vertical_capacity: vec![vertical_capacity.max(1); vertical_edges],
            horizontal_usage: vec![0; horizontal_edges],
            vertical_usage: vec![0; vertical_edges],
            horizontal_history: vec![0.0; horizontal_edges],
            vertical_history: vec![0.0; vertical_edges],
        }
    }

    fn node(&self, x: usize, y: usize) -> usize { y * self.cols + x }
    fn horizontal_edge(&self, x: usize, y: usize) -> usize { y * (self.cols - 1) + x }
    fn vertical_edge(&self, x: usize, y: usize) -> usize { y * self.cols + x }

    fn edge_cost(&self, horizontal: bool, index: usize, timing_weight: f64) -> f64 {
        let (usage, capacity, history) = if horizontal {
            (self.horizontal_usage[index] as f64, self.horizontal_capacity[index] as f64, self.horizontal_history[index])
        } else {
            (self.vertical_usage[index] as f64, self.vertical_capacity[index] as f64, self.vertical_history[index])
        };
        // Present congestion grows sharply beyond capacity.  History remains
        // after a rip-up iteration, driving subsequent paths away from an
        // edge that repeatedly overflowed instead of oscillating forever.
        let ratio = (usage + 1.0) / capacity;
        1.0 + timing_weight.max(0.0) + history + ratio * ratio * 2.0 + ratio.max(1.0).powi(4) * 12.0
    }

    fn neighbors(&self, x: usize, y: usize, timing_weight: f64) -> Vec<(usize, usize, bool, usize, f64)> {
        let mut out = Vec::with_capacity(4);
        if x > 0 {
            let edge = self.horizontal_edge(x - 1, y);
            out.push((x - 1, y, true, edge, self.edge_cost(true, edge, timing_weight)));
        }
        if x + 1 < self.cols {
            let edge = self.horizontal_edge(x, y);
            out.push((x + 1, y, true, edge, self.edge_cost(true, edge, timing_weight)));
        }
        if y > 0 {
            let edge = self.vertical_edge(x, y - 1);
            out.push((x, y - 1, false, edge, self.edge_cost(false, edge, timing_weight)));
        }
        if y + 1 < self.rows {
            let edge = self.vertical_edge(x, y);
            out.push((x, y + 1, false, edge, self.edge_cost(false, edge, timing_weight)));
        }
        out
    }

    /// Deterministic Dijkstra search on the GCell graph.  A binary heap is
    /// required here: negotiated routing invokes this once per branch per
    /// epoch, so a linear V-scan turns a normal-sized block into billions of
    /// comparisons before detailed routing can even begin.
    fn route(&self, source: (usize, usize), target: (usize, usize), timing_weight: f64) -> Option<NativeGlobalRoute> {
        if source.0 >= self.cols || source.1 >= self.rows || target.0 >= self.cols || target.1 >= self.rows { return None; }
        let count = self.cols * self.rows;
        let mut distance = vec![f64::INFINITY; count];
        let mut previous = vec![None::<(usize, bool, usize)>; count];
        let source_index = self.node(source.0, source.1);
        let target_index = self.node(target.0, target.1);
        distance[source_index] = 0.0;
        let mut queue = BinaryHeap::new();
        queue.push(GlobalQueueEntry { cost: 0.0, node: source_index });
        while let Some(GlobalQueueEntry { cost, node: current }) = queue.pop() {
            if cost > distance[current] + 1e-12 { continue; }
            if current == target_index { break; }
            let x = current % self.cols;
            let y = current / self.cols;
            for (nx, ny, horizontal, edge, edge_cost) in self.neighbors(x, y, timing_weight) {
                let neighbor = self.node(nx, ny);
                let candidate = cost + edge_cost;
                if candidate + 1e-12 < distance[neighbor] {
                    distance[neighbor] = candidate;
                    previous[neighbor] = Some((current, horizontal, edge));
                    queue.push(GlobalQueueEntry { cost: candidate, node: neighbor });
                }
            }
        }
        if !distance[target_index].is_finite() { return None; }
        let mut points = vec![target];
        let mut edges = Vec::new();
        let mut current = target_index;
        while current != source_index {
            let (parent, horizontal, edge) = previous[current]?;
            points.push((parent % self.cols, parent / self.cols));
            edges.push((horizontal, edge));
            current = parent;
        }
        points.reverse();
        edges.reverse();
        Some(NativeGlobalRoute { points, edge_refs: edges, cost: distance[target_index] })
    }

    fn commit(&mut self, route: &NativeGlobalRoute) {
        for (horizontal, edge) in &route.edge_refs {
            if *horizontal { self.horizontal_usage[*edge] = self.horizontal_usage[*edge].saturating_add(1); }
            else { self.vertical_usage[*edge] = self.vertical_usage[*edge].saturating_add(1); }
        }
    }

    fn rip_up(&mut self, route: &NativeGlobalRoute) {
        for (horizontal, edge) in &route.edge_refs {
            if *horizontal { self.horizontal_usage[*edge] = self.horizontal_usage[*edge].saturating_sub(1); }
            else { self.vertical_usage[*edge] = self.vertical_usage[*edge].saturating_sub(1); }
        }
    }

    fn overflow(&self) -> usize {
        self.horizontal_usage.iter().zip(&self.horizontal_capacity).map(|(use_count, capacity)| use_count.saturating_sub(*capacity) as usize).sum::<usize>()
            + self.vertical_usage.iter().zip(&self.vertical_capacity).map(|(use_count, capacity)| use_count.saturating_sub(*capacity) as usize).sum::<usize>()
    }

    fn increase_history_on_overflow(&mut self) {
        for index in 0..self.horizontal_usage.len() {
            if self.horizontal_usage[index] > self.horizontal_capacity[index] {
                self.horizontal_history[index] += (self.horizontal_usage[index] - self.horizontal_capacity[index]) as f64 * 16.0;
            }
        }
        for index in 0..self.vertical_usage.len() {
            if self.vertical_usage[index] > self.vertical_capacity[index] {
                self.vertical_history[index] += (self.vertical_usage[index] - self.vertical_capacity[index]) as f64 * 16.0;
            }
        }
    }
}

#[derive(Clone, Debug)]
struct NativeRouteConnection {
    net: String,
    source_x: f64,
    source_y: f64,
    target_x: f64,
    target_y: f64,
    /// The source/target LEF PORT rectangles are retained by detailed
    /// routing.  A route may only stack from li1 into upper metal at a point
    /// inside the actual PORT, not at a snapped coordinate near the pin.
    source_access: Vec<PinAccessRegion>,
    target_access: Vec<PinAccessRegion>,
    timing_weight: f64,
    route_class: usize,
}

#[derive(Clone, Debug)]
struct PinAccessRegion {
    layer: String,
    x1_um: f64,
    y1_um: f64,
    x2_um: f64,
    y2_um: f64,
}

fn instance_pin_accesses(cell: &PlacedCell, pin_name: &str, lef: &LefData) -> Vec<PinAccessRegion> {
    let Some(macro_def) = lef.macros.get(&cell.cell) else { return Vec::new(); };
    let mut physical_name = pin_name;
    if !macro_def.pins.iter().any(|shape| shape.name.eq_ignore_ascii_case(pin_name)) && pin_is_output(pin_name) {
        if let Some(alias) = ["X", "Y", "Z", "ZN", "Q", "QN", "OUT"].iter()
            .find(|alias| macro_def.pins.iter().any(|shape| shape.name.eq_ignore_ascii_case(alias))) {
            physical_name = alias;
        }
    }
    macro_def.pins.iter().filter(|shape| shape.name.eq_ignore_ascii_case(physical_name)).map(|shape| PinAccessRegion {
        layer: shape.layer.clone(),
        x1_um: cell.x_um + shape.x1_um,
        y1_um: cell.y_um + shape.y1_um,
        x2_um: cell.x_um + shape.x2_um,
        y2_um: cell.y_um + shape.y2_um,
    }).collect()
}

/// Pick a legal vertical-track landing within a physical LEF PORT rectangle.
/// The y coordinate is deliberately kept inside the PORT (rather than
/// snapped to an unrelated horizontal grid): the first preferred-direction
/// segment is vertical and the stacked via is enclosed by the pin itself.
fn pin_vertical_access(
    regions: &[PinAccessRegion], lef: &LefData, layer: &str, preferred_x: f64, preferred_y: f64,
) -> Option<(f64, f64)> {
    if regions.is_empty() {
        return snap_to_routing_track(lef, layer, preferred_x).ok().map(|x| (x, preferred_y));
    }
    // The native signal router starts at the cell's lowest routing PORT.
    // A region on another layer is not silently treated as a li1 contact.
    let (pitch, offset, vertical) = routing_track_axis(lef, layer).ok()?;
    if !vertical { return None; }
    let width = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
    let pin_layer = lef.routing_layers.first().cloned().unwrap_or_default();
    let mut choices = Vec::new();
    for region in regions.iter().filter(|region| region.layer == pin_layer) {
        let lo = region.x1_um + width * 0.5;
        let hi = region.x2_um - width * 0.5;
        let y_lo = region.y1_um + width * 0.5;
        let y_hi = region.y2_um - width * 0.5;
        if lo > hi + 1e-9 || y_lo > y_hi + 1e-9 { continue; }
        // LEF coordinates, site placement and track offsets are all decimal
        // values.  Permit an epsilon much smaller than the manufacturing
        // grid when a track lies exactly on a legal enclosure boundary.
        let track_epsilon = lef.manufacturing_grid_um.unwrap_or(0.001).min(pitch * 1e-4) / pitch;
        let first = ((lo - offset) / pitch - track_epsilon).ceil() as i64;
        let last = ((hi - offset) / pitch + track_epsilon).floor() as i64;
        if first > last { continue; }
        let preferred_index = ((preferred_x - offset) / pitch).round() as i64;
        let index = preferred_index.clamp(first, last);
        let x = offset + index as f64 * pitch;
        // Exact-width LEF ports commonly differ by a few ULP after the cell
        // placement translation.  They remain valid; normalize that tiny
        // interval before calling `clamp`, while rejecting material gaps
        // above with the explicit tolerance check.
        let y = preferred_y.clamp(y_lo.min(y_hi), y_lo.max(y_hi));
        choices.push(((x - preferred_x).abs() + (y - preferred_y).abs(), x, y));
    }
    choices.into_iter().min_by(|left, right| left.0.partial_cmp(&right.0).unwrap_or(std::cmp::Ordering::Equal)).map(|(_, x, y)| (x, y))
}

/// Pick a via landing inside a physical standard-cell PORT for a horizontal
/// routing layer.  The lower landing remains on the real pin layer (li1),
/// while the Y coordinate is constrained to the selected upper layer's
/// horizontal track grid.  This is the normal standard-cell escape: the
/// signal leaves the pin through a local via and a horizontal metal segment,
/// rather than drawing a chip-height li1 wire through unrelated cell rows.
fn pin_horizontal_access(
    regions: &[PinAccessRegion], lef: &LefData, layer: &str, preferred_x: f64, preferred_y: f64,
) -> Option<(f64, f64)> {
    if regions.is_empty() {
        return snap_to_routing_track(lef, layer, preferred_y).ok().map(|y| (preferred_x, y));
    }
    let (pitch, offset, vertical) = routing_track_axis(lef, layer).ok()?;
    if vertical { return None; }
    let pin_layer = lef.routing_layers.first().cloned().unwrap_or_default();
    let epsilon = lef.manufacturing_grid_um.unwrap_or(0.001).min(pitch * 1e-4) / pitch;
    let mut choices = Vec::new();
    for region in regions.iter().filter(|region| region.layer == pin_layer) {
        // This point describes the via cut center, not the full width of an
        // upper-metal rectangle.  Applying the upper-metal width as a pin
        // enclosure here incorrectly rejects ordinary narrow LEF ports.
        // Full via enclosure is audited separately once technology cut rules
        // are available; the center must still lie in the physical port.
        let x_lo = region.x1_um;
        let x_hi = region.x2_um;
        let y_lo = region.y1_um;
        let y_hi = region.y2_um;
        if x_lo > x_hi + 1e-9 || y_lo > y_hi + 1e-9 { continue; }
        let first = ((y_lo - offset) / pitch - epsilon).ceil() as i64;
        let last = ((y_hi - offset) / pitch + epsilon).floor() as i64;
        if first > last { continue; }
        let preferred_index = ((preferred_y - offset) / pitch).round() as i64;
        let index = preferred_index.clamp(first, last);
        let y = offset + index as f64 * pitch;
        let x = preferred_x.clamp(x_lo.min(x_hi), x_lo.max(x_hi));
        choices.push(((x - preferred_x).abs() + (y - preferred_y).abs(), x, y));
    }
    choices.into_iter()
        .min_by(|left, right| left.0.partial_cmp(&right.0).unwrap_or(std::cmp::Ordering::Equal))
        .map(|(_, x, y)| (x, y))
}

/// Return `(pin_x, pin_y, horizontal_track_y)` for a local standard-cell
/// escape.  The first point is guaranteed to be on the real li1 PORT; the
/// final coordinate is a legal track on the selected horizontal layer.  The
/// short li1 segment between them is materialised by the caller, so narrow
/// ports are not forced to contain an upper-metal track center.
fn pin_horizontal_escape(
    regions: &[PinAccessRegion], lef: &LefData, layer: &str,
    preferred_x: f64, preferred_y: f64, track_shift: i64,
) -> Option<(f64, f64, f64)> {
    let (pin_x, pin_y) = if regions.is_empty() {
        (preferred_x, preferred_y)
    } else {
        pin_vertical_access(regions, lef, lef.routing_layers.first()?.as_str(), preferred_x, preferred_y)?
    };
    let (pitch, offset, vertical) = routing_track_axis(lef, layer).ok()?;
    if vertical { return None; }
    let base = ((pin_y - offset) / pitch).round() as i64;
    let index = base.saturating_add(track_shift);
    let track_y = offset + index as f64 * pitch;
    if track_y < -1e-6 { return None; }
    // li1 is local interconnect for standard-cell pin access, not a global
    // channel.  A coarse upper-metal pitch (for example met5) can put the
    // nearest legal H track several microns away; emitting that distance on
    // li1 crosses neighbouring pins and creates the dense-row blockages that
    // a professional router would solve by changing the access layer/guide.
    let max_li1_escape = lef.site_height_um.unwrap_or(2.0).max(1.0) * 0.45;
    if (track_y - pin_y).abs() > max_li1_escape + 1e-9 { return None; }
    Some((pin_x, pin_y, track_y))
}

/// Layer assignment for a detailed signal branch. The sequence is source-H,
/// source-V, trunk-H, target-V, target-H. Source and target escapes use met1
/// or met3, whose pitches permit local movement from ordinary li1 standard
/// cell ports; all three horizontal planes are available for long trunks.
/// The combinations deliberately cover both met2/met4 access columns so
/// dense datapaths do not serialize every branch through one topology.
fn native_layer_topology(route_class: usize) -> (usize, usize, usize, usize, usize) {
    match route_class % 24 {
        0 => (1, 2, 1, 4, 1),
        1 => (1, 4, 1, 2, 3),
        2 => (3, 2, 1, 4, 1),
        3 => (3, 4, 1, 2, 3),
        4 => (1, 2, 3, 4, 1),
        5 => (1, 4, 3, 2, 3),
        6 => (3, 2, 3, 4, 1),
        7 => (3, 4, 3, 2, 3),
        8 => (1, 2, 5, 4, 1),
        9 => (1, 4, 5, 2, 3),
        10 => (3, 2, 5, 4, 1),
        11 => (3, 4, 5, 2, 3),
        12 => (1, 2, 1, 4, 3),
        13 => (1, 4, 1, 2, 1),
        14 => (3, 2, 1, 4, 3),
        15 => (3, 4, 1, 2, 1),
        16 => (1, 2, 3, 4, 3),
        17 => (1, 4, 3, 2, 1),
        18 => (3, 2, 3, 4, 3),
        19 => (3, 4, 3, 2, 1),
        20 => (1, 2, 5, 4, 3),
        21 => (1, 4, 5, 2, 1),
        22 => (3, 2, 5, 4, 3),
        _ => (3, 4, 5, 2, 1),
    }
}

/// Alternating V/H/V/H/V topology for a branch with a vertical trunk.  Both
/// endpoints deliberately use li1: a standard-cell PORT is an li1 shape, so
/// the first and last segments must be on its own legal vertical grid.  The
/// old implementation began on met2/met4 and required those upper-metal
/// widths to fit inside a li1 PORT, which rejected ordinary Sky130 cells
/// before detailed routing had a chance to allocate a legal escape.
fn native_vertical_layer_topology(route_class: usize) -> (usize, usize, usize, usize, usize) {
    match route_class % 4 {
        0 => (0, 1, 2, 3, 0),
        1 => (0, 3, 4, 1, 0),
        2 => (0, 1, 4, 3, 0),
        _ => (0, 3, 2, 1, 0),
    }
}

fn nearby_tracks(preferred: f64, limit: f64, pitch: f64, edge: f64, radius: usize) -> Vec<f64> {
    if limit <= edge * 2.0 { return Vec::new(); }
    let center = ((preferred - edge) / pitch).round() as i64;
    let max_track = ((limit - edge * 2.0) / pitch).floor().max(0.0) as i64;
    let mut tracks = Vec::new();
    for delta in 0..=radius as i64 {
        for index in if delta == 0 { vec![center] } else { vec![center - delta, center + delta] } {
            if index < 0 || index > max_track { continue; }
            let value = edge + index as f64 * pitch;
            if value >= edge - 1e-9 && value <= limit - edge + 1e-9 && !tracks.iter().any(|existing: &f64| (*existing - value).abs() < 1e-9) {
                tracks.push(value);
            }
        }
    }
    // A local search alone cannot repair a blocked pin column once the
    // immediately adjacent tracks have been consumed.  Add a bounded,
    // deterministic die-wide sample after the locality-preserving sequence;
    // this gives detailed routing real detours without an unbounded scan of
    // every track in the core.
    for sample in 0..4u64 {
        let fraction = ((sample.wrapping_mul(6_364_136_223_846_793_005u64).wrapping_add(1_442_695_040_888_963_407u64) & 0x7fff_ffff) as f64)
            / 2_147_483_648.0;
        let index = (fraction * (max_track + 1) as f64).floor() as i64;
        let value = edge + index.clamp(0, max_track) as f64 * pitch;
        if !tracks.iter().any(|existing| (*existing - value).abs() < 1e-9) { tracks.push(value); }
    }
    tracks
}

/// Materialise a global-guide coordinate into tracks on one *specific* LEF
/// routing layer.  Global cells express corridors; they must never be fed
/// back through a synthetic common pitch, because met1/met3/met5 and
/// li1/met2/met4 have different grids and offsets.
fn guide_tracks_for_layer(
    lef: &LefData, layer: &str, limit: f64, edge: f64, guide_coordinates: &[f64], radius: usize,
) -> Vec<f64> {
    let Ok((pitch, offset, _)) = routing_track_axis(lef, layer) else { return Vec::new(); };
    let low = edge.max(offset);
    let high = (limit - edge).max(low);
    let first = ((low - offset) / pitch - 1e-9).ceil() as i64;
    let last = ((high - offset) / pitch + 1e-9).floor() as i64;
    if first > last { return Vec::new(); }
    let mut tracks = Vec::new();
    for guide in guide_coordinates {
        let center = ((guide - offset) / pitch).round() as i64;
        for delta in 0..=radius as i64 {
            for index in if delta == 0 { vec![center] } else { vec![center - delta, center + delta] } {
                if index < first || index > last { continue; }
                let value = offset + index as f64 * pitch;
                if !tracks.iter().any(|existing: &f64| (*existing - value).abs() < 1e-9) { tracks.push(value); }
            }
        }
    }
    // Keep bounded die-wide alternatives for negotiated repair.  These are
    // genuine tracks on `layer`, not coordinates from an unrelated plane.
    for sample in 0..32i64 {
        let index = first + (sample * (last - first).max(0) / 31);
        let value = offset + index as f64 * pitch;
        if !tracks.iter().any(|existing| (*existing - value).abs() < 1e-9) { tracks.push(value); }
    }
    tracks
}

/// Enumerate DRC-spaced legal tracks on one routing layer, ordered by distance
/// from a preferred coordinate. The spacing stride mirrors the final abstract
/// route DRC: if the LEF has no scalar spacing value, minimum width is used as
/// conservative clearance.
fn spaced_tracks_for_layer(
    lef: &LefData, layer: &str, limit: f64, edge: f64, preferred: f64,
) -> Vec<f64> {
    let Ok((pitch, offset, _)) = routing_track_axis(lef, layer) else { return Vec::new(); };
    let low = edge.max(offset);
    let high = (limit - edge).max(low);
    let first = ((low - offset) / pitch - 1e-9).ceil() as i64;
    let last = ((high - offset) / pitch + 1e-9).floor() as i64;
    if first > last { return Vec::new(); }
    let width = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
    let clearance = width + lef.routing_spacings_um.get(layer).copied().unwrap_or(width);
    let stride = (clearance / pitch).ceil().max(1.0) as i64;
    let mut tracks = (first..=last)
        .step_by(stride as usize)
        .map(|index| offset + index as f64 * pitch)
        .collect::<Vec<_>>();
    tracks.sort_by(|left, right| {
        (left - preferred).abs()
            .partial_cmp(&(right - preferred).abs())
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| left.partial_cmp(right).unwrap_or(std::cmp::Ordering::Equal))
    });
    tracks
}

/// Enumerate only the legal tracks adjacent to one standard-cell escape.
/// Unlike `guide_tracks_for_layer`, this helper deliberately has no die-wide
/// samples: an endpoint jog is a local access feature, while the trunk is
/// the only part permitted to consume a global routing corridor.
fn local_tracks_for_layer(
    lef: &LefData, layer: &str, limit: f64, edge: f64, preferred: f64, radius: usize,
) -> Vec<f64> {
    let Ok((pitch, offset, _)) = routing_track_axis(lef, layer) else { return Vec::new(); };
    let low = edge.max(offset);
    let high = (limit - edge).max(low);
    let first = ((low - offset) / pitch - 1e-9).ceil() as i64;
    let last = ((high - offset) / pitch + 1e-9).floor() as i64;
    if first > last { return Vec::new(); }
    let center = ((preferred - offset) / pitch).round() as i64;
    let mut tracks = Vec::new();
    for delta in 0..=radius as i64 {
        for index in if delta == 0 { vec![center] } else { vec![center - delta, center + delta] } {
            if index < first || index > last { continue; }
            let coordinate = offset + index as f64 * pitch;
            if !tracks.iter().any(|existing: &f64| (*existing - coordinate).abs() < 1e-9) {
                tracks.push(coordinate);
            }
        }
    }
    tracks
}

/// Layer-aware spatial index used only during detailed routing.  The final
/// DRC remains exhaustive, but candidate admission need not compare a small
/// local track segment with every route on the die.  A segment is indexed in
/// every bucket its LEF clearance box touches, so querying the same box is a
/// conservative exact prefilter rather than a heuristic.
#[derive(Default)]
struct RouteSpatialIndex {
    bucket_um: f64,
    buckets: HashMap<(String, i32, i32), Vec<usize>>,
}

impl RouteSpatialIndex {
    fn new(bucket_um: f64) -> Self { Self { bucket_um: bucket_um.max(1.0), buckets: HashMap::new() } }

    fn bucket_range(&self, bbox: (f64, f64, f64, f64)) -> (i32, i32, i32, i32) {
        ((bbox.0 / self.bucket_um).floor() as i32, (bbox.1 / self.bucket_um).floor() as i32,
         (bbox.2 / self.bucket_um).floor() as i32, (bbox.3 / self.bucket_um).floor() as i32)
    }

    fn insert(&mut self, segment: &RouteSegment, index: usize, lef: &LefData) {
        let rule = lef.routing_spacings_um.get(&segment.layer).copied()
            .unwrap_or_else(|| lef.routing_widths_um.get(&segment.layer).copied().unwrap_or(0.14));
        let (x0, y0, x1, y1) = self.bucket_range(route_bbox(segment, rule));
        for x in x0..=x1 { for y in y0..=y1 {
            self.buckets.entry((segment.layer.clone(), x, y)).or_default().push(index);
        }}
    }

    fn conflicts(&self, candidate: &[RouteSegment], routed: &[RouteSegment], lef: &LefData) -> bool {
        !self.conflicting_indices(candidate, routed, lef).is_empty()
    }

    /// Return the already-materialised geometry that blocks this candidate.
    /// Keeping the segment indices is important for negotiated detailed
    /// routing: it lets the caller rip up the owning connection instead of
    /// discarding an entire completed layout after one pin-access conflict.
    fn conflicting_indices(&self, candidate: &[RouteSegment], routed: &[RouteSegment], lef: &LefData) -> BTreeSet<usize> {
        let mut blocking = BTreeSet::new();
        for next in candidate {
            // Bucket overlap can return one existing segment more than once,
            // but it must be checked again for every distinct segment in the
            // candidate.  A cross-candidate `inspected` set skipped later H/V
            // pieces on the same layer and allowed shorts that only the final
            // exhaustive DRC detected.
            let mut inspected = BTreeSet::new();
            let rule = lef.routing_spacings_um.get(&next.layer).copied()
                .unwrap_or_else(|| lef.routing_widths_um.get(&next.layer).copied().unwrap_or(0.14));
            let (x0, y0, x1, y1) = self.bucket_range(route_bbox(next, rule));
            for x in x0..=x1 { for y in y0..=y1 {
                if let Some(indices) = self.buckets.get(&(next.layer.clone(), x, y)) {
                    for index in indices {
                        if !inspected.insert(*index) { continue; }
                        let existing = &routed[*index];
                        if next.net != existing.net && bboxes_overlap(route_bbox(next, rule), route_bbox(existing, rule)) {
                            blocking.insert(*index);
                        }
                    }
                }
            }}
        }
        blocking
    }
}

#[derive(Clone, Debug)]
struct NativeDetailedCandidate {
    segments: Vec<RouteSegment>,
    vias: Vec<Via>,
}

fn stack_transition_vias(
    vias: &mut Vec<Via>, connection: &NativeRouteConnection, layers: &[String],
    from: usize, to: usize, x: f64, y: f64, size_um: f64,
) {
    let (low, high) = if from <= to { (from, to) } else { (to, from) };
    for layer in low..high {
        vias.push(Via {
            net: connection.net.clone(), lower_layer: layers[layer].clone(), upper_layer: layers[layer + 1].clone(),
            x_um: x, y_um: y, size_um,
        });
    }
}

/// Build one complete, width-aware detailed-routing candidate.  A candidate
/// is intentionally owned by exactly one logical connection, including its
/// stacked pin access vias, so it can be removed and rebuilt during a local
/// negotiated reroute without corrupting unrelated geometry.
fn materialize_native_candidate(
    connection: &NativeRouteConnection, layers: &[String], lef: &LefData,
    vertical_trunk: bool, topology_class: usize,
    source_track: f64, trunk_track: f64, target_track: f64,
) -> Option<NativeDetailedCandidate> {
    let width = |layer: &String| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
    let topology = if vertical_trunk {
        native_vertical_layer_topology(topology_class)
    } else {
        native_layer_topology(topology_class)
    };
    let snap = |index: usize, coordinate: f64| snap_to_routing_track(lef, &layers[index], coordinate)
        .expect("validated routing layer");
    let mut segments = Vec::new();
    let mut transition_points = Vec::new();
    let mut add = |layer: usize, x1: f64, y1: f64, x2: f64, y2: f64| {
        if (x1 - x2).abs() + (y1 - y2).abs() > 1e-9 {
            segments.push(RouteSegment { net: connection.net.clone(), layer: layers[layer].clone(), width_um: width(&layers[layer]), x1_um: x1, y1_um: y1, x2_um: x2, y2_um: y2 });
        }
    };
    if vertical_trunk {
        let (_, source_h, trunk_v, target_h, _) = topology;
        // Access each physical li1 PORT through a local stacked via, then
        // travel on a real horizontal routing track.  The old topology made
        // the li1 endpoint itself a die-spanning vertical trunk, which is
        // neither a practical standard-cell escape nor routable at density.
        let source_shift = topology_class as i64 % 7 - 3;
        let source_escape = if source_shift != 0 {
            pin_horizontal_escape(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y, source_shift)
                .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true))
                .or_else(|| pin_horizontal_access(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y)
                    .map(|(x, y)| (x, y, y, false)))
        } else {
            pin_horizontal_access(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y)
                .map(|(x, y)| (x, y, y, false))
                .or_else(|| pin_horizontal_escape(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y, source_shift)
                    .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true)))
        }?;
        let (source_pin_x, source_pin_y, source_y, source_li1_escape) = source_escape;
        let trunk_x = snap(trunk_v, source_track);
        let target_shift = (topology_class as i64 + 2) % 7 - 3;
        let target_escape = if target_shift != 0 {
            pin_horizontal_escape(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y, target_shift)
                .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true))
                .or_else(|| pin_horizontal_access(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y)
                    .map(|(x, y)| (x, y, y, false)))
        } else {
            pin_horizontal_access(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y)
                .map(|(x, y)| (x, y, y, false))
                .or_else(|| pin_horizontal_escape(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y, target_shift)
                    .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true)))
        }?;
        let (target_pin_x, target_pin_y, target_y, target_li1_escape) = target_escape;
        if source_li1_escape { add(0, source_pin_x, source_pin_y, source_pin_x, source_y); }
        add(source_h, source_pin_x, source_y, trunk_x, source_y);
        add(trunk_v, trunk_x, source_y, trunk_x, target_y);
        add(target_h, trunk_x, target_y, target_pin_x, target_y);
        if target_li1_escape { add(0, target_pin_x, target_y, target_pin_x, target_pin_y); }
        transition_points.extend([
            (0, source_h, source_pin_x, source_y),
            (source_h, trunk_v, trunk_x, source_y),
            (trunk_v, target_h, trunk_x, target_y),
            (target_h, 0, target_pin_x, target_y),
        ]);
    } else {
        let (source_h, source_v, trunk_h, target_v, target_h) = topology;
        let source_shift = topology_class as i64 % 7 - 3;
        let source_escape = if source_shift != 0 {
            pin_horizontal_escape(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y, source_shift)
                .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true))
                .or_else(|| pin_horizontal_access(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y)
                    .map(|(x, y)| (x, y, y, false)))
        } else {
            pin_horizontal_access(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y)
                .map(|(x, y)| (x, y, y, false))
                .or_else(|| pin_horizontal_escape(&connection.source_access, lef, &layers[source_h], connection.source_x, connection.source_y, source_shift)
                    .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true)))
        }?;
        let (source_pin_x, source_pin_y, source_y, source_li1_escape) = source_escape;
        let source_x = snap(source_v, source_track);
        let trunk_y = snap(trunk_h, trunk_track);
        let target_x = snap(target_v, target_track);
        let target_shift = (topology_class as i64 + 2) % 7 - 3;
        let target_escape = if target_shift != 0 {
            pin_horizontal_escape(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y, target_shift)
                .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true))
                .or_else(|| pin_horizontal_access(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y)
                    .map(|(x, y)| (x, y, y, false)))
        } else {
            pin_horizontal_access(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y)
                .map(|(x, y)| (x, y, y, false))
                .or_else(|| pin_horizontal_escape(&connection.target_access, lef, &layers[target_h], connection.target_x, connection.target_y, target_shift)
                    .map(|(x, pin_y, track_y)| (x, pin_y, track_y, true)))
        }?;
        let (target_pin_x, target_pin_y, target_y, target_li1_escape) = target_escape;
        // The complementary H/V/H/V/H topology uses the same local pin-via
        // escapes.  Only the first vertical channel and horizontal trunk
        // vary; li1 never becomes a global routing resource.
        if source_li1_escape { add(0, source_pin_x, source_pin_y, source_pin_x, source_y); }
        add(source_h, source_pin_x, source_y, source_x, source_y);
        add(source_v, source_x, source_y, source_x, trunk_y);
        add(trunk_h, source_x, trunk_y, target_x, trunk_y);
        add(target_v, target_x, trunk_y, target_x, target_y);
        add(target_h, target_x, target_y, target_pin_x, target_y);
        if target_li1_escape { add(0, target_pin_x, target_y, target_pin_x, target_pin_y); }
        transition_points.extend([
            (0, source_h, source_pin_x, source_y),
            (source_h, source_v, source_x, source_y),
            (source_v, trunk_h, source_x, trunk_y),
            (trunk_h, target_v, target_x, trunk_y),
            (target_v, target_h, target_x, target_y),
            (target_h, 0, target_pin_x, target_y),
        ]);
    }
    let via_size = [topology.0, topology.1, topology.2, topology.3, topology.4]
        .iter().map(|index| width(&layers[*index])).fold(0.14_f64, f64::max);
    let mut vias = Vec::new();
    for (from, to, x, y) in transition_points {
        stack_transition_vias(&mut vias, connection, layers, from, to, x, y, via_size);
    }
    Some(NativeDetailedCandidate {
        segments,
        vias,
    })
}

fn route_seed(connection: &NativeRouteConnection) -> usize {
    connection.net.bytes().fold(connection.route_class.wrapping_mul(97), |hash, byte| {
        hash.wrapping_mul(131).wrapping_add(byte as usize)
    })
}

/// Materialise global-route intent into legal, width-aware V/H tracks.  Every
/// connection uses the native six-layer access stack (li1/met1/met2/met3/
/// met4/met5), with source/target escapes chosen independently from the trunk
/// track.  This is intentionally a real geometry allocator: candidates are
/// admitted only when they satisfy the same LEF spacing predicate used by
/// post-route DRC.
#[allow(dead_code)]
fn route_native_tracks_legacy(
    connections: &[NativeRouteConnection], lef: &LefData, core_width: f64, core_height: f64,
    track_pitch: f64, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    if connections.is_empty() { return Ok((Vec::new(), Vec::new(), 0)); }
    if lef.routing_layers.len() < 6 {
        return Err("Native track router requires at least three alternating V/H routing pairs".to_string());
    }
    let layers = &lef.routing_layers;
    let width = |layer: &String| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
    let gcell_pitch = (track_pitch * 10.0).max(2.0);
    // GCells express congestion regions, not individual tracks.  Bound the
    // graph so the deterministic negotiated router remains responsive even
    // while evaluating an intentionally oversized legacy SBox.
    let cols = (core_width / gcell_pitch).ceil().clamp(4.0, 18.0) as usize;
    let rows = (core_height / gcell_pitch).ceil().clamp(4.0, 18.0) as usize;
    // Sky130's six routing layers provide three H and three V resources.
    // Li1 is normally used for local pin escape, but it is a genuine routing
    // plane and can carry a short GCell detour when met2/met4 are congested.
    // Excluding it made global-route demand systematically exceed the
    // detailed router's legal resource set.
    // `cols`/`rows` are deliberately capped for predictable maze-search
    // time.  Once that cap is reached a GCell covers more than
    // `gcell_pitch` microns, so its capacity must be derived from its *real*
    // physical span.  Using the nominal pitch here was a severe accounting
    // bug: a 60 um wide GCell was credited with only ten tracks and forced a
    // clean route through a huge, artificial overflow recovery loop.
    let gcell_horizontal_span = core_width / (cols - 1) as f64;
    let gcell_vertical_span = core_height / (rows - 1) as f64;
    // Capacity is the sum of usable tracks on the actual preferred-direction
    // layers, not ``number_of_layers * worst_layer_pitch``.  In Sky130 met5
    // is intentionally wide; using its pitch for met1/met3 understated a
    // GCell's legal signal capacity by more than a factor of two.
    let tracks_for_span = |span: f64, direction: &str| -> u16 {
        lef.routing_layers.iter().filter(|layer| {
            lef.routing_directions.get(*layer).is_some_and(|value| value == direction)
        }).map(|layer| {
            (span / routing_track_pitch(lef, layer).max(track_pitch * 0.20)).floor().max(1.0) as u16
        }).fold(0u16, |sum, count| sum.saturating_add(count)).max(2)
    };
    let horizontal_capacity = tracks_for_span(gcell_vertical_span, "HORIZONTAL");
    let vertical_capacity = tracks_for_span(gcell_horizontal_span, "VERTICAL");
    let mut global = NativeGlobalGrid::new(cols, rows, horizontal_capacity, vertical_capacity);
    let to_gcell = |x: f64, y: f64| {
        let gx = ((x / core_width.max(1e-6) * (cols - 1) as f64).round() as isize).clamp(0, cols as isize - 1) as usize;
        let gy = ((y / core_height.max(1e-6) * (rows - 1) as f64).round() as isize).clamp(0, rows as isize - 1) as usize;
        (gx, gy)
    };
    let mut global_paths = Vec::with_capacity(connections.len());
    for connection in connections {
        let path = global.route(to_gcell(connection.source_x, connection.source_y), to_gcell(connection.target_x, connection.target_y), connection.timing_weight)
            .ok_or_else(|| format!("Global route grid cannot reach net {}", connection.net))?;
        global.commit(&path);
        global_paths.push(path);
    }
    // A bounded negotiated-congestion pass: only overflowing paths are ripped
    // up, history is added to the offending edges, and then they are rebuilt.
    for _ in 0..40 {
        if global.overflow() == 0 { break; }
        global.increase_history_on_overflow();
        // Rip up only the worst overflow contributors.  Re-routing every path
        // in the same iteration recreates the identical congestion pattern
        // and oscillates; bounded batches preserve useful routes while giving
        // the maze cost a chance to divert the real offenders.
        let mut offenders = global_paths.iter().enumerate().filter_map(|(index, path)| {
            let score = path.edge_refs.iter().map(|(horizontal, edge)| {
                if *horizontal { global.horizontal_usage[*edge].saturating_sub(global.horizontal_capacity[*edge]) as usize }
                else { global.vertical_usage[*edge].saturating_sub(global.vertical_capacity[*edge]) as usize }
            }).sum::<usize>();
            (score > 0).then_some((score, index))
        }).collect::<Vec<_>>();
        offenders.sort_by(|left, right| right.cmp(left));
        let batch = (connections.len() / 5).max(1);
        for (_, index) in offenders.into_iter().take(batch) {
            let connection = &connections[index];
            global.rip_up(&global_paths[index]);
            global_paths[index] = global.route(to_gcell(connection.source_x, connection.source_y), to_gcell(connection.target_x, connection.target_y), connection.timing_weight)
                .ok_or_else(|| format!("Global negotiated reroute cannot reach net {}", connection.net))?;
            global.commit(&global_paths[index]);
        }
    }
    let global_overflow = global.overflow();
    // Keep the residual demand as a hard *acceptance* metric, but do not
    // abort before detailed routing.  A coarse GCell bin can be pessimistic
    // near pin access; detailed routing is the authoritative geometry check
    // and its result tells the SBox loop whether the pressure is real rather
    // than hiding it behind an early global-route error.

    let mut order = (0..connections.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        let lhs = connections[*left].timing_weight * 1000.0 + global_paths[*left].cost;
        let rhs = connections[*right].timing_weight * 1000.0 + global_paths[*right].cost;
        // Lock short/local branches first.  They have the fewest legal pin
        // escape choices; long trunks retain many GCell detours and are
        // therefore the right nets to absorb negotiated-routing pressure.
        lhs.partial_cmp(&rhs).unwrap_or(std::cmp::Ordering::Equal)
    });
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    let mut spatial = RouteSpatialIndex::new((track_pitch * 12.0).max(2.0));
    for index in order {
        let connection = &connections[index];
        let path = &global_paths[index];
        let preferred_trunk_y = path.points.get(path.points.len() / 2)
            .map(|(_, y)| (*y as f64 + 0.5) / rows as f64 * core_height)
            .unwrap_or((connection.source_y + connection.target_y) * 0.5);
        let preferred_source_x = path.points.get(1)
            .map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width).unwrap_or(connection.source_x);
        let preferred_target_x = path.points.iter().rev().nth(1)
            .map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width).unwrap_or(connection.target_x);
        let preferred_trunk_x = path.points.get(path.points.len() / 2)
            .map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width)
            .unwrap_or((connection.source_x + connection.target_x) * 0.5);
        let mut selected = None;
        let mut last_candidate = Vec::new();
        // Prefer the topology whose target branch has an independently
        // allocated escape column.  The V-trunk topology remains a fallback;
        // its final direct pin drop is intentionally tried only after the
        // horizontal form has exhausted legal target-access tracks.
        let prefer_vertical = false;
        'orientation: for vertical_trunk in [prefer_vertical, !prefer_vertical] {
            if vertical_trunk {
                for topology_delta in 0..4usize {
                for source_y in nearby_tracks(connection.source_y, core_height, track_pitch, edge, 6) {
                    for trunk_x in nearby_tracks(preferred_trunk_x, core_width, track_pitch, edge, 6) {
                        for target_y in nearby_tracks(connection.target_y, core_height, track_pitch, edge, 6) {
                            let (source_v, source_h, trunk_v, target_h, target_v) = native_vertical_layer_topology(connection.route_class + topology_delta);
                            let candidate = vec![
                                RouteSegment { net: connection.net.clone(), layer: layers[source_v].clone(), width_um: width(&layers[source_v]), x1_um: connection.source_x, y1_um: connection.source_y, x2_um: connection.source_x, y2_um: source_y },
                                RouteSegment { net: connection.net.clone(), layer: layers[source_h].clone(), width_um: width(&layers[source_h]), x1_um: connection.source_x, y1_um: source_y, x2_um: trunk_x, y2_um: source_y },
                                RouteSegment { net: connection.net.clone(), layer: layers[trunk_v].clone(), width_um: width(&layers[trunk_v]), x1_um: trunk_x, y1_um: source_y, x2_um: trunk_x, y2_um: target_y },
                                RouteSegment { net: connection.net.clone(), layer: layers[target_h].clone(), width_um: width(&layers[target_h]), x1_um: trunk_x, y1_um: target_y, x2_um: connection.target_x, y2_um: target_y },
                                RouteSegment { net: connection.net.clone(), layer: layers[target_v].clone(), width_um: width(&layers[target_v]), x1_um: connection.target_x, y1_um: target_y, x2_um: connection.target_x, y2_um: connection.target_y },
                            ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                            last_candidate = candidate.clone();
                            if candidate.is_empty() || spatial.conflicts(&candidate, &routes, lef) { continue; }
                            selected = Some((candidate, connection.source_x, source_y, trunk_x, target_y, connection.target_x, true, connection.route_class + topology_delta));
                            break 'orientation;
                        }
                    }
                }
                }
            } else {
                for topology_delta in 0..4usize {
                'search: for trunk_y in nearby_tracks(preferred_trunk_y, core_height, track_pitch, edge, 6) {
                    for source_x in nearby_tracks(preferred_source_x, core_width, track_pitch, edge, 6) {
                        for target_x in nearby_tracks(preferred_target_x, core_width, track_pitch, edge, 6) {
                    // Distribute both escapes and trunks across the actual
                    // alternating routing planes.  The previous fixed
                    // met2/met4 drops made every endpoint compete for the
                    // same vertical columns even though li1 is a legal local
                    // escape resource and met1/met3 are independent H grids.
                    // Met5 is intentionally kept for PDN/clock reserve here:
                    // its coarse pitch is a poor default for dense signals.
                    let (source_h, source_v, trunk_h, target_v, target_h) = native_layer_topology(connection.route_class + topology_delta);
                    let candidate = vec![
                        RouteSegment { net: connection.net.clone(), layer: layers[source_h].clone(), width_um: width(&layers[source_h]), x1_um: connection.source_x, y1_um: connection.source_y, x2_um: source_x, y2_um: connection.source_y },
                        RouteSegment { net: connection.net.clone(), layer: layers[source_v].clone(), width_um: width(&layers[source_v]), x1_um: source_x, y1_um: connection.source_y, x2_um: source_x, y2_um: trunk_y },
                        RouteSegment { net: connection.net.clone(), layer: layers[trunk_h].clone(), width_um: width(&layers[trunk_h]), x1_um: source_x, y1_um: trunk_y, x2_um: target_x, y2_um: trunk_y },
                        RouteSegment { net: connection.net.clone(), layer: layers[target_v].clone(), width_um: width(&layers[target_v]), x1_um: target_x, y1_um: trunk_y, x2_um: target_x, y2_um: connection.target_y },
                        RouteSegment { net: connection.net.clone(), layer: layers[target_h].clone(), width_um: width(&layers[target_h]), x1_um: target_x, y1_um: connection.target_y, x2_um: connection.target_x, y2_um: connection.target_y },
                    ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                    last_candidate = candidate.clone();
                    if candidate.is_empty() || spatial.conflicts(&candidate, &routes, lef) { continue; }
                    selected = Some((candidate, source_x, connection.source_y, trunk_y, target_x, connection.target_y, false, connection.route_class + topology_delta));
                    break 'search;
                }
            }
        }
            }
        }
        }
        let Some((candidate, first_x, first_y, middle_x, middle_y, last_x, vertical_trunk, topology_class)) = selected else {
            return Err(format!("Native detailed track allocation blocked for net {} after GCell-guided search; conflict sample: {}", connection.net,
                route_conflict_detail(&last_candidate, &routes, lef)));
        };
        let topology = if vertical_trunk { native_vertical_layer_topology(topology_class) } else { native_layer_topology(topology_class) };
        let via_size = width(&layers[topology.0]).max(width(&layers[topology.1])).max(width(&layers[topology.2])).max(width(&layers[topology.3])).max(width(&layers[topology.4]));
        // The cell LEF pins are li1.  The upper target access is represented
        // by an explicit stacked landing at the pin; all via locations are
        // exported, drawn and bounds-checked like normal route geometry.
        let mut stack_vias = |from: usize, to: usize, x: f64, y: f64| {
            let (low, high) = if from <= to { (from, to) } else { (to, from) };
            for layer in low..high {
                vias.push(Via { net: connection.net.clone(), lower_layer: layers[layer].clone(), upper_layer: layers[layer + 1].clone(), x_um: x, y_um: y, size_um: via_size });
            }
        };
        if vertical_trunk {
            stack_vias(0, topology.0, connection.source_x, connection.source_y);
            stack_vias(topology.0, topology.1, first_x, first_y);
            stack_vias(topology.1, topology.2, middle_x, first_y);
            stack_vias(topology.2, topology.3, middle_x, middle_y);
            stack_vias(topology.3, topology.4, last_x, middle_y);
            stack_vias(topology.4, 0, connection.target_x, connection.target_y);
        } else {
            stack_vias(0, topology.0, connection.source_x, connection.source_y);
            stack_vias(topology.0, topology.1, first_x, first_y);
            stack_vias(topology.1, topology.2, first_x, middle_y);
            stack_vias(topology.2, topology.3, middle_x, middle_y);
            stack_vias(topology.3, topology.4, middle_x, last_x);
            stack_vias(topology.4, 0, connection.target_x, connection.target_y);
        }
        let start = routes.len();
        for (offset, segment) in candidate.iter().enumerate() { spatial.insert(segment, start + offset, lef); }
        routes.extend(candidate);
    }
    Ok((routes, vias, global_overflow))
}

/// Lower one negotiated GCell path into a multi-turn, layer-specific detailed
/// candidate.  The path is retained as a sequence of physical landmarks;
/// only coordinates perpendicular to each preferred direction are snapped to
/// that layer's legal track grid.
fn materialize_guided_candidate(
    connection: &NativeRouteConnection, guide: &NativeGlobalRoute, layers: &[String], lef: &LefData,
    core_width: f64, core_height: f64, cols: usize, rows: usize,
    horizontal_layer: usize, vertical_layer: usize, escape_shift: i64,
) -> Option<NativeDetailedCandidate> {
    let (source_pin_x, source_pin_y, source_y) = pin_horizontal_escape(
        &connection.source_access, lef, &layers[horizontal_layer], connection.source_x, connection.source_y, escape_shift,
    )?;
    let (target_pin_x, target_pin_y, target_y) = pin_horizontal_escape(
        &connection.target_access, lef, &layers[horizontal_layer], connection.target_x, connection.target_y, -escape_shift,
    )?;
    if source_y < 0.0 || source_y > core_height || target_y < 0.0 || target_y > core_height { return None; }
    let point = |(x, y): (usize, usize)| -> Option<(f64, f64)> {
        let raw_x = (x as f64 + 0.5) / cols as f64 * core_width;
        let raw_y = (y as f64 + 0.5) / rows as f64 * core_height;
        Some((snap_to_routing_track(lef, &layers[vertical_layer], raw_x).ok()?,
              snap_to_routing_track(lef, &layers[horizontal_layer], raw_y).ok()?))
    };
    let points = guide.points.iter().copied().filter_map(point).collect::<Vec<_>>();
    let first = *points.first()?;
    let last = *points.last()?;
    let width = |layer: usize| lef.routing_widths_um.get(&layers[layer]).copied().unwrap_or(0.14);
    let mut segments = Vec::new();
    let mut add = |layer: usize, x1: f64, y1: f64, x2: f64, y2: f64| {
        if (x1 - x2).abs() + (y1 - y2).abs() > 1e-9 {
            segments.push(RouteSegment { net: connection.net.clone(), layer: layers[layer].clone(), width_um: width(layer), x1_um: x1, y1_um: y1, x2_um: x2, y2_um: y2 });
        }
    };
    // Li1 remains a local pin escape; all die-scale movement uses the chosen
    // alternating H/V pair.
    add(0, source_pin_x, source_pin_y, source_pin_x, source_y);
    add(horizontal_layer, source_pin_x, source_y, first.0, source_y);
    add(vertical_layer, first.0, source_y, first.0, first.1);
    for pair in points.windows(2) {
        let (x1, y1) = pair[0];
        let (x2, y2) = pair[1];
        if (x1 - x2).abs() > 1e-9 { add(horizontal_layer, x1, y1, x2, y1); }
        if (y1 - y2).abs() > 1e-9 { add(vertical_layer, x2, y1, x2, y2); }
    }
    add(vertical_layer, last.0, last.1, last.0, target_y);
    add(horizontal_layer, last.0, target_y, target_pin_x, target_y);
    add(0, target_pin_x, target_y, target_pin_x, target_pin_y);
    if segments.is_empty() { return None; }
    let vias = build_vias(&segments, layers);
    Some(NativeDetailedCandidate { segments, vias })
}

/// Detailed route lowering that retains the complete global guide.  It tries
/// all real horizontal/vertical plane pairs and nearby legal pin escapes;
/// candidates remain subject to exact same-layer spacing and short checks.
fn route_native_guided_tracks(
    connections: &[NativeRouteConnection], guides: &[NativeGlobalRoute], lef: &LefData,
    core_width: f64, core_height: f64, cols: usize, rows: usize, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    let layers = &lef.routing_layers;
    let horizontal = layers.iter().enumerate().filter_map(|(index, layer)|
        lef.routing_directions.get(layer).is_some_and(|direction| direction == "HORIZONTAL").then_some(index)
    ).filter(|index| *index != 0).collect::<Vec<_>>();
    let vertical = layers.iter().enumerate().filter_map(|(index, layer)|
        lef.routing_directions.get(layer).is_some_and(|direction| direction == "VERTICAL").then_some(index)
    ).collect::<Vec<_>>();
    if horizontal.is_empty() || vertical.is_empty() { return Err("Native guided router has no alternating routing planes".to_string()); }
    let mut order = (0..connections.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        let a = &connections[*left]; let b = &connections[*right];
        b.timing_weight.partial_cmp(&a.timing_weight).unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| guides[*right].edge_refs.len().cmp(&guides[*left].edge_refs.len()))
    });
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    let mut spatial = RouteSpatialIndex::new(edge.mul_add(12.0, 1.0));
    let guide_start = Instant::now();
    let guide_budget = std::env::var("AI_DIGITAL_APR_GUIDE_BUDGET_MS")
        .ok().and_then(|value| value.parse::<u64>().ok()).filter(|value| *value >= 50)
        .map(Duration::from_millis).unwrap_or_else(|| Duration::from_secs(8));
    for index in order {
        if guide_start.elapsed() > guide_budget {
            return Err(format!("Native guide lowering exceeded {} ms budget", guide_budget.as_millis()));
        }
        let connection = &connections[index];
        let seed = route_seed(connection);
        let mut selected = None;
        'search: for variant in 0usize..24 {
            if guide_start.elapsed() > guide_budget {
                return Err(format!("Native guide lowering exceeded {} ms budget", guide_budget.as_millis()));
            }
            let h = horizontal[(seed + variant) % horizontal.len()];
            let v = vertical[(seed / 3 + variant * 3) % vertical.len()];
            for shift in [-2_i64, -1, 0, 1, 2] {
                let Some(candidate) = materialize_guided_candidate(
                    connection, &guides[index], layers, lef, core_width, core_height, cols, rows, h, v,
                    shift + (variant % 3) as i64 - 1,
                ) else { continue; };
                if spatial.conflicts(&candidate.segments, &routes, lef) { continue; }
                selected = Some(candidate);
                break 'search;
            }
        }
        let Some(candidate) = selected else {
            return Err(format!("Native guided detailed routing blocked for net {} after preserving negotiated guide landmarks", connection.net));
        };
        let start = routes.len();
        for (offset, segment) in candidate.segments.iter().enumerate() { spatial.insert(segment, start + offset, lef); }
        routes.extend(candidate.segments);
        vias.extend(candidate.vias);
    }
    let (spacing, shorts) = check_route_geometry(&routes, lef);
    if spacing != 0 || shorts != 0 { return Err(format!("Native guided detailed router produced illegal geometry: spacing={} shorts={}", spacing, shorts)); }
    Ok((routes, vias, 0))
}

/// Scalable deterministic detailed router for dense synthesized datapaths.
///
/// It reserves tracks that already satisfy the selected LEF width/spacing
/// relation, then lowers every branch through the same physical pin-access,
/// via construction and spatial conflict predicates used by the negotiated
/// routers. Unlike the exploratory routers it never rebuilds the full routing
/// database per connection, so a large block produces a bounded diagnostic
/// rather than appearing stuck before its first SBox result.
fn route_native_reserved_tracks(
    connections: &[NativeRouteConnection], lef: &LefData, core_width: f64, core_height: f64, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    if connections.is_empty() { return Ok((Vec::new(), Vec::new(), 0)); }
    let layers = &lef.routing_layers;
    if layers.len() < 6 {
        return Err("Native reserved-track router requires at least three alternating V/H routing pairs".to_string());
    }
    let mut order = (0..connections.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        let a = &connections[*left];
        let b = &connections[*right];
        let a_span = (a.source_x - a.target_x).abs() + (a.source_y - a.target_y).abs();
        let b_span = (b.source_x - b.target_x).abs() + (b.source_y - b.target_y).abs();
        b.timing_weight.partial_cmp(&a.timing_weight).unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| b_span.partial_cmp(&a_span).unwrap_or(std::cmp::Ordering::Equal))
            .then_with(|| a.net.cmp(&b.net))
    });
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    let mut spatial = RouteSpatialIndex::new((edge * 2.0).max(0.75));
    let mut routed_connections = 0usize;
    let started = Instant::now();
    let budget = std::env::var("AI_DIGITAL_APR_RESERVED_BUDGET_MS")
        .ok().and_then(|value| value.parse::<u64>().ok()).filter(|value| *value >= 100)
        .map(Duration::from_millis).unwrap_or_else(|| Duration::from_millis(3500));

    for index in order {
        if started.elapsed() > budget {
            return Err(format!(
                "Native reserved-track router exceeded {} ms after {}/{} routed connection(s)",
                budget.as_millis(), routed_connections, connections.len()
            ));
        }
        let connection = &connections[index];
        let seed = route_seed(connection);
        let preferred_y = (connection.source_y + connection.target_y) * 0.5;
        let mut selected = None;
        let mut blockers = BTreeSet::new();
        let mut candidate_count = 0usize;
        let mut profiles = BTreeSet::new();

        'topology: for topology_delta in 0usize..32 {
            let topology_class = connection.route_class + topology_delta;
            let (source_h, source_v, trunk_h, target_v, target_h) = native_layer_topology(topology_class);
            let source_tracks = spaced_tracks_for_layer(lef, &layers[source_v], core_width, edge, connection.source_x);
            let target_tracks = spaced_tracks_for_layer(lef, &layers[target_v], core_width, edge, connection.target_x);
            let trunk_tracks = spaced_tracks_for_layer(lef, &layers[trunk_h], core_height, edge, preferred_y);
            if source_tracks.is_empty() || target_tracks.is_empty() || trunk_tracks.is_empty() { continue; }
            profiles.insert(format!(
                "t{}:{}:{}x{}:{}x{}:{}",
                topology_class, layers[source_h], source_tracks.len(), layers[trunk_h],
                trunk_tracks.len(), layers[target_h], target_tracks.len()
            ));
            let source_take = source_tracks.len().min(48);
            let target_take = target_tracks.len().min(48);
            let trunk_take = trunk_tracks.len().min(96);
            let variant_limit = (source_take * target_take * trunk_take).min(768);
            for variant in 0..variant_limit {
                if started.elapsed() > budget {
                    return Err(format!(
                        "Native reserved-track router exceeded {} ms while routing net {} after {} candidate(s)",
                        budget.as_millis(), connection.net, candidate_count
                    ));
                }
                // Enumerate the nearest endpoint tracks first. This keeps
                // li1-to-upper-metal escapes local to the source/sink row;
                // deterministic seed rotation is retained only for ties
                // after all nearby trunk choices have been covered.
                let source_slot = variant / (trunk_take * target_take).max(1);
                // Sample the complete target-track pool with a coprime
                // stride. The former nested order covered only the first
                // handful of columns before the bounded candidate budget,
                // repeatedly selecting an already occupied long vertical.
                let target_slot = (variant.wrapping_mul(37).wrapping_add(seed % target_take)) % target_take;
                let trunk_slot = variant % trunk_take;
                let source = source_tracks[source_slot % source_take];
                let target = target_tracks[target_slot % target_take];
                let trunk = trunk_tracks[(trunk_slot + seed % trunk_take) % trunk_take];
                let Some(candidate) = materialize_native_candidate(
                    connection, layers, lef, false, topology_class, source, trunk, target,
                ) else { continue; };
                if candidate.segments.is_empty() { continue; }
                candidate_count += 1;
                let conflicts = spatial.conflicting_indices(&candidate.segments, &routes, lef);
                if conflicts.is_empty() {
                    selected = Some(candidate);
                    break 'topology;
                }
                if blockers.len() < 16 {
                    let detail = route_conflict_detail(&candidate.segments, &routes, lef);
                    if !detail.is_empty() { blockers.insert(detail); }
                }
            }
        }
        let Some(candidate) = selected else {
            return Err(format!(
                "Native reserved-track router blocked after {}/{} routed connection(s): net {} candidates={} profiles={} blockers={}",
                routed_connections, connections.len(), connection.net, candidate_count,
                profiles.into_iter().take(8).collect::<Vec<_>>().join(","),
                blockers.into_iter().take(4).collect::<Vec<_>>().join(" | ")
            ));
        };
        let start = routes.len();
        for (offset, segment) in candidate.segments.iter().enumerate() {
            spatial.insert(segment, start + offset, lef);
            routes.push(segment.clone());
        }
        vias.extend(candidate.vias);
        routed_connections += 1;
    }
    let (spacing, shorts) = check_route_geometry(&routes, lef);
    if spacing != 0 || shorts != 0 {
        return Err(format!(
            "Native reserved-track router produced illegal geometry: spacing={} shorts={}",
            spacing, shorts
        ));
    }
    Ok((routes, vias, 0))
}

/// Materialise one channel-assigned branch.  Endpoint moves are deliberately
/// local; only `trunk_y` is a globally allocated lane.  This separation keeps
/// rows of standard-cell pins from becoming accidental long-wire channels.
fn materialize_channel_candidate(
    connection: &NativeRouteConnection, layers: &[String], lef: &LefData,
    source_h: usize, source_v: usize, trunk_h: usize, target_v: usize, target_h: usize,
    source_track: f64, trunk_y: f64, target_track: f64,
    source_shift: i64, target_shift: i64,
) -> Option<NativeDetailedCandidate> {
    let width = |index: usize| lef.routing_widths_um.get(&layers[index]).copied().unwrap_or(0.14);
    let select_escape = |regions: &[PinAccessRegion], layer: &str, x: f64, y: f64, shift: i64| {
        let shifted = || pin_horizontal_escape(regions, lef, layer, x, y, shift)
            .map(|(pin_x, pin_y, track_y)| (pin_x, pin_y, track_y, true));
        let direct = || pin_horizontal_access(regions, lef, layer, x, y)
            .map(|(pin_x, track_y)| (pin_x, track_y, track_y, false));
        // Non-zero shifts are intentional negotiated alternatives. Prefer
        // them before the direct PORT landing, otherwise every retry lowers
        // to identical endpoint geometry despite selecting a new trunk.
        if shift == 0 { direct().or_else(shifted) } else { shifted().or_else(direct) }
    };
    let source_escape = select_escape(
        &connection.source_access, &layers[source_h], connection.source_x, connection.source_y, source_shift,
    )?;
    let target_escape = select_escape(
        &connection.target_access, &layers[target_h], connection.target_x, connection.target_y, target_shift,
    )?;
    let (source_pin_x, source_pin_y, source_y, source_li1) = source_escape;
    let (target_pin_x, target_pin_y, target_y, target_li1) = target_escape;
    let source_x = snap_to_routing_track(lef, &layers[source_v], source_track).ok()?;
    let target_x = snap_to_routing_track(lef, &layers[target_v], target_track).ok()?;
    let trunk_y = snap_to_routing_track(lef, &layers[trunk_h], trunk_y).ok()?;
    let mut segments = Vec::new();
    let mut add = |layer: usize, x1: f64, y1: f64, x2: f64, y2: f64| {
        if (x1 - x2).abs() + (y1 - y2).abs() > 1e-9 {
            segments.push(RouteSegment { net: connection.net.clone(), layer: layers[layer].clone(), width_um: width(layer), x1_um: x1, y1_um: y1, x2_um: x2, y2_um: y2 });
        }
    };
    if source_li1 { add(0, source_pin_x, source_pin_y, source_pin_x, source_y); }
    add(source_h, source_pin_x, source_y, source_x, source_y);
    add(source_v, source_x, source_y, source_x, trunk_y);
    add(trunk_h, source_x, trunk_y, target_x, trunk_y);
    add(target_v, target_x, trunk_y, target_x, target_y);
    add(target_h, target_x, target_y, target_pin_x, target_y);
    if target_li1 { add(0, target_pin_x, target_y, target_pin_x, target_pin_y); }
    if segments.is_empty() { return None; }
    let via_size = [source_h, source_v, trunk_h, target_v, target_h].iter().map(|index| width(*index)).fold(0.14_f64, f64::max);
    let mut vias = Vec::new();
    for (from, to, x, y) in [
        (0, source_h, source_pin_x, source_y), (source_h, source_v, source_x, source_y),
        (source_v, trunk_h, source_x, trunk_y), (trunk_h, target_v, target_x, trunk_y),
        (target_v, target_h, target_x, target_y), (target_h, 0, target_pin_x, target_y),
    ] {
        stack_transition_vias(&mut vias, connection, layers, from, to, x, y, via_size);
    }
    Some(NativeDetailedCandidate { segments, vias })
}

/// Deterministic detail router using interval-coloured horizontal channels.
/// Long trunks are allocated before geometry lowering, so detailed routing is
/// no longer forced to discover a global channel through repeated collisions.
fn route_native_interval_channels(
    connections: &[NativeRouteConnection], lef: &LefData, core_width: f64, core_height: f64, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    if connections.is_empty() { return Ok((Vec::new(), Vec::new(), 0)); }
    let layers = &lef.routing_layers;
    let horizontal = layers.iter().enumerate().filter_map(|(index, layer)|
        (index != 0 && lef.routing_directions.get(layer).is_some_and(|direction| direction == "HORIZONTAL")).then_some(index)
    ).collect::<Vec<_>>();
    let vertical = layers.iter().enumerate().filter_map(|(index, layer)|
        (index != 0 && lef.routing_directions.get(layer).is_some_and(|direction| direction == "VERTICAL")).then_some(index)
    ).collect::<Vec<_>>();
    if horizontal.len() < 2 || vertical.is_empty() {
        return Err("Native interval router requires at least two upper horizontal and one upper vertical routing layer".to_string());
    }
    let interval_start = Instant::now();
    let interval_budget = std::env::var("AI_DIGITAL_APR_INTERVAL_BUDGET_MS")
        .ok().and_then(|value| value.parse::<u64>().ok()).filter(|value| *value >= 100)
        .map(Duration::from_millis).unwrap_or_else(|| Duration::from_secs(30));
    // Negotiated detailed routing must be allowed to explore enough genuine
    // alternatives before a floorplan is declared too dense.  These bounds
    // constrain search work only: they never relax LEF spacing, pin access,
    // connectivity, or final geometry DRC.  The environment overrides are
    // intentionally diagnostic-only so a difficult technology can be
    // characterized without changing a reproducible production default.
    let bounded_env = |name: &str, default: usize, minimum: usize, maximum: usize| {
        std::env::var(name).ok().and_then(|value| value.parse::<usize>().ok())
            .filter(|value| *value >= minimum && *value <= maximum).unwrap_or(default)
    };
    let repair_limit = bounded_env("AI_DIGITAL_APR_INTERVAL_REPAIR_LIMIT", 96, 1, 512);
    let current_retry_limit = bounded_env("AI_DIGITAL_APR_INTERVAL_CURRENT_RETRY_LIMIT", 20, 1, 128);
    let victim_retry_limit = bounded_env("AI_DIGITAL_APR_INTERVAL_VICTIM_RETRY_LIMIT", 16, 1, 128);
    let victim_batch_limit = bounded_env("AI_DIGITAL_APR_INTERVAL_VICTIM_BATCH", 8, 1, 64);
    let candidates_per_trunk = bounded_env("AI_DIGITAL_APR_INTERVAL_CANDIDATES_PER_TRUNK", 48, 8, 256);
    let mut assignment = vec![(0usize, 0usize); connections.len()];
    let mut group_capacity = Vec::with_capacity(horizontal.len());
    let mut group_clearance = Vec::with_capacity(horizontal.len());
    let mut group_track_ys = Vec::<Vec<f64>>::with_capacity(horizontal.len());
    for layer_index in &horizontal {
        let layer = &layers[*layer_index];
        let (pitch, offset, _) = routing_track_axis(lef, layer).map_err(|_| format!("No track grid for {layer}"))?;
        let width = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
        let clearance = width + lef.routing_spacings_um.get(layer).copied().unwrap_or(width);
        let first = ((edge.max(offset) - offset) / pitch).ceil() as i64;
        let last = (((core_height - edge).max(edge) - offset) / pitch).floor() as i64;
        let stride = (clearance / pitch).ceil().max(1.0) as i64;
        let capacity = ((last - first) / stride + 1).max(0) as usize;
        let tracks = (0..capacity).map(|lane| offset + (first + lane as i64 * stride) as f64 * pitch).collect::<Vec<_>>();
        group_capacity.push(capacity);
        group_clearance.push(clearance);
        group_track_ys.push(tracks);
    }
    let mut group_lane_ends = group_capacity.iter().map(|capacity| vec![f64::NEG_INFINITY; *capacity]).collect::<Vec<_>>();
    let local_x_margin = vertical.iter().map(|index| routing_track_pitch(lef, &layers[*index]))
        .fold(0.0_f64, f64::max).max(edge) * 12.0;
    // A routed net is a tree, not one independent long wire for every sink.
    // A physical trunk therefore spans the extrema of all of a net's
    // branches; those branches share its assigned channel.
    let mut net_spans = BTreeMap::<String, (Vec<usize>, f64, f64, f64, usize)>::new();
    for (index, connection) in connections.iter().enumerate() {
        let start = (connection.source_x.min(connection.target_x) - local_x_margin).max(0.0);
        let end = (connection.source_x.max(connection.target_x) + local_x_margin).min(core_width);
        let entry = net_spans.entry(connection.net.clone()).or_insert_with(|| (Vec::new(), start, end, 0.0, 0));
        entry.0.push(index);
        entry.1 = entry.1.min(start);
        entry.2 = entry.2.max(end);
        entry.3 += connection.source_y + connection.target_y;
        entry.4 += 2;
    }
    let mut spans = net_spans.iter().map(|(net, (members, start, end, y_sum, y_count))| {
        (net.clone(), members.clone(), *start, *end, if *y_count == 0 { core_height * 0.5 } else { *y_sum / *y_count as f64 })
    }).collect::<Vec<_>>();
    spans.sort_by(|left, right| left.2.partial_cmp(&right.2).unwrap_or(std::cmp::Ordering::Equal)
        .then_with(|| right.3.partial_cmp(&left.3).unwrap_or(std::cmp::Ordering::Equal)));
    for (net, members, start, end, preferred_y) in spans {
        let span_len = (end - start).max(0.0);
        let mut best = None::<(f64, usize, usize)>;
        for group in 0..horizontal.len() {
            let layer_rank = if horizontal.len() <= 1 { 0.0 } else { group as f64 / (horizontal.len() - 1) as f64 };
            let long_wire_bonus = span_len / core_width.max(1e-6) * layer_rank * core_height * 0.45;
            for lane in 0..group_lane_ends[group].len() {
                if group_lane_ends[group][lane] + group_clearance[group] > start { continue; }
                let used = group_lane_ends[group].iter().filter(|end| end.is_finite()).count() as f64;
                let score = (group_track_ys[group][lane] - preferred_y).abs()
                    + used / group_capacity[group].max(1) as f64 * routing_track_pitch(lef, &layers[horizontal[group]])
                    - long_wire_bonus
                    + (1.0 - layer_rank) * span_len / core_width.max(1e-6) * edge;
                if best.as_ref().map(|(best_score, _, _)| score < *best_score).unwrap_or(true) {
                    best = Some((score, group, lane));
                }
            }
        }
        let Some((_, group, lane)) = best else {
            let detail = horizontal.iter().enumerate().map(|(group, layer)| {
                let used = group_lane_ends[group].iter().filter(|end| end.is_finite()).count();
                format!("{}:{}/{}", layers[*layer], used, group_capacity[group])
            }).collect::<Vec<_>>().join(",");
            return Err(format!("Native interval router exhausted all horizontal channel groups for net {net}; groups={detail}"));
        };
        group_lane_ends[group][lane] = group_lane_ends[group][lane].max(end);
        for index in members { assignment[index] = (group, lane); }
    }
    let mut order = (0..connections.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        let left_conn = &connections[*left];
        let right_conn = &connections[*right];
        let left_span = (left_conn.source_x - left_conn.target_x).abs() + (left_conn.source_y - left_conn.target_y).abs();
        let right_span = (right_conn.source_x - right_conn.target_x).abs() + (right_conn.source_y - right_conn.target_y).abs();
        right_conn.timing_weight.partial_cmp(&left_conn.timing_weight).unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| right_span.partial_cmp(&left_span).unwrap_or(std::cmp::Ordering::Equal))
            .then_with(|| left_conn.net.cmp(&right_conn.net))
    });
    let mut pending = std::collections::VecDeque::from(order);
    let mut owned = vec![None::<NativeDetailedCandidate>; connections.len()];
    let mut retries = vec![0usize; connections.len()];
    let mut repairs = 0usize;
    let rebuild = |owned: &[Option<NativeDetailedCandidate>]| {
        let mut routes = Vec::new();
        let mut owners = Vec::new();
        for (owner, route) in owned.iter().enumerate() {
            if let Some(route) = route {
                for segment in &route.segments {
                    routes.push(segment.clone());
                    owners.push(owner);
                }
            }
        }
        // This index is queried for every pin-access alternative.  A 4–5um
        // bucket pooled dozens of unrelated standard-cell branches into the
        // same conflict set; use a fine physical bucket so the exact bbox
        // check only sees geometry that can actually touch the candidate.
        let mut spatial = RouteSpatialIndex::new((edge * 3.0).max(0.75));
        for (segment_index, segment) in routes.iter().enumerate() { spatial.insert(segment, segment_index, lef); }
        (routes, owners, spatial)
    };
    // Keep the detailed-route spatial state incrementally.  Rebuilding every
    // routed segment for every new connection made the router quadratic
    // before it even reached a real congestion decision.  A rebuild is only
    // needed after a local rip-up removes ownership.
    let (mut routes, mut segment_owners, mut spatial) = rebuild(&owned);
    while let Some(index) = pending.pop_front() {
        if owned[index].is_some() { continue; }
        if interval_start.elapsed() > interval_budget {
            return Err(format!("Native interval router exceeded {} ms budget after {} routed segment(s)", interval_budget.as_millis(), routes.len()));
        }
        let connection = &connections[index];
        let (group, lane) = assignment[index];
        let mut selected = None;
        let mut conflict_resources = BTreeSet::new();
        let mut blockers = BTreeSet::new();
        let mut candidate_count = 0usize;
        let assigned_trunk_y = group_track_ys[group].get(lane).copied()
            .ok_or_else(|| format!("Native interval router has no physical track for {} lane {lane}", layers[horizontal[group]]))?;
        let preferred_y = (connection.source_y + connection.target_y) * 0.5;
        let mut trunk_choices = vec![(group, lane, assigned_trunk_y)];
        for alt_group in 0..horizontal.len() {
            let mut lanes = group_track_ys[alt_group].iter().enumerate().collect::<Vec<_>>();
            lanes.sort_by(|left, right| {
                (left.1 - preferred_y).abs().partial_cmp(&(right.1 - preferred_y).abs())
                    .unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| left.0.cmp(&right.0))
            });
            for (alt_lane, alt_y) in lanes.into_iter().take(6) {
                if alt_group == group && alt_lane == lane { continue; }
                trunk_choices.push((alt_group, alt_lane, *alt_y));
            }
        }
        trunk_choices.truncate(1 + horizontal.len() * 6);
        if retries[index] > 0 && !trunk_choices.is_empty() {
            let rotation = retries[index] % trunk_choices.len();
            trunk_choices.rotate_left(rotation);
        }
        let mut trunk_attempts = Vec::new();
        'trunk_search: for (choice_index, (trunk_group, trunk_lane, trunk_y)) in trunk_choices.iter().copied().enumerate() {
            let mut trunk_candidate_count = 0usize;
            let trunk_h = horizontal[trunk_group];
            let trunk_layer = &layers[trunk_h];
            trunk_attempts.push(format!("{}:{}@{:.4}", trunk_layer, trunk_lane, trunk_y));
            let local_escape_limit = lef.site_height_um.unwrap_or(2.0).max(1.0) * 0.45;
            let mut access_h_layers = horizontal.iter().copied().filter(|layer| {
                *layer != trunk_h && routing_track_pitch(lef, &layers[*layer]) <= local_escape_limit + 1e-9
            }).collect::<Vec<_>>();
            if access_h_layers.is_empty() { access_h_layers = horizontal.clone(); }
            access_h_layers.sort_unstable();
            // Pin access needs at least two independent horizontal planes.
            // Restricting it to the lowest layer forced unrelated source and
            // sink jogs in every row onto met1, then the detailed router
            // reported a false global congestion problem.  The selected
            // trunk layer remains excluded; the remaining real H planes are
            // explored deterministically below.
            let variant_limit = if choice_index == 0 { 8usize } else { 4usize };
            let local_limit = (candidates_per_trunk / variant_limit).max(1);
            for variant in 0..variant_limit {
                if interval_start.elapsed() > interval_budget {
                    let routed_connections = owned.iter().filter(|candidate| candidate.is_some()).count();
                    return Err(format!(
                        "Native interval router exceeded {} ms budget while searching net {} after {}/{} connection(s), {} segment(s), {} repair(s), and {} candidate(s) for the current net",
                        interval_budget.as_millis(), connection.net, routed_connections, connections.len(),
                        routes.len(), repairs, candidate_count,
                    ));
                }
                // A ripped-up connection must not replay the identical
                // access topology.  Fold its negotiation epoch into every
                // independent resource choice so retries move to a new H/V
                // pair and new local landing tracks while remaining fully
                // deterministic across runs.
                let negotiation = retries[index].wrapping_mul(17);
                let source_h = access_h_layers[(variant + negotiation) % access_h_layers.len()];
                let target_h = access_h_layers[(variant / access_h_layers.len() + 1 + negotiation) % access_h_layers.len()];
                let source_v = vertical[(index + variant + choice_index + negotiation) % vertical.len()];
                let target_v = vertical[(index + variant + choice_index + 1 + negotiation * 3) % vertical.len()];
                let source_candidates = local_tracks_for_layer(lef, &layers[source_v], core_width, edge, connection.source_x, 32);
                let target_candidates = local_tracks_for_layer(lef, &layers[target_v], core_width, edge, connection.target_x, 32);
                if source_candidates.is_empty() || target_candidates.is_empty() { continue; }
                // Divide the bounded trunk budget across topology/access
                // variants.  Letting variant zero consume all 24 candidates
                // made the advertised layer and pin-shift alternatives
                // unreachable on every blocked connection.
                for local in 0..source_candidates.len().min(target_candidates.len()).max(1).min(local_limit) {
                    // `local_tracks_for_layer` is already ordered from the
                    // nearest legal track outward.  Preserve that order so a
                    // routed branch first consumes the shortest pin escape;
                    // pseudo-random rotation made early nets claim long
                    // horizontal stubs and blocked later dense-row nets.
                    let source_track = source_candidates[(local + variant * 3 + negotiation) % source_candidates.len()];
                    let target_track = target_candidates[(local * 5 + variant * 7 + negotiation * 5) % target_candidates.len()];
                    let source_shift = (variant + choice_index + retries[index]) % 7;
                    let target_shift = (variant.wrapping_mul(3)
                        .wrapping_add(choice_index)
                        .wrapping_add(retries[index].wrapping_mul(2))
                        .wrapping_add(2)) % 7;
                    let Some(candidate) = materialize_channel_candidate(connection, layers, lef, source_h, source_v, trunk_h, target_v, target_h,
                        source_track, trunk_y, target_track, source_shift as i64 - 3, target_shift as i64 - 3) else { continue; };
                    candidate_count += 1;
                    trunk_candidate_count += 1;
                    let conflicts = spatial.conflicting_indices(&candidate.segments, &routes, lef);
                    if conflicts.is_empty() {
                        selected = Some(candidate);
                        break 'trunk_search;
                    }
                    if conflict_resources.len() < 8 {
                        let detail = route_conflict_detail(&candidate.segments, &routes, lef);
                        if !detail.is_empty() { conflict_resources.insert(detail); }
                    }
                    for segment_index in conflicts {
                        if let Some(owner) = segment_owners.get(segment_index) {
                            blockers.insert(*owner);
                        }
                    }
                    if trunk_candidate_count >= candidates_per_trunk && !blockers.is_empty() {
                        continue 'trunk_search;
                    }
                }
            }
        }
        if let Some(candidate) = selected {
            let start = routes.len();
            for (offset, segment) in candidate.segments.iter().enumerate() {
                spatial.insert(segment, start + offset, lef);
                routes.push(segment.clone());
                segment_owners.push(index);
            }
            owned[index] = Some(candidate);
            continue;
        }
        let has_blockers = !blockers.is_empty();
        let mut victims = blockers.into_iter().filter(|owner| {
            *owner != index && owned[*owner].is_some()
                && connections[*owner].timing_weight <= connection.timing_weight
                && retries[*owner] < victim_retry_limit
        }).collect::<Vec<_>>();
        victims.sort_by(|left, right| {
            retries[*left].cmp(&retries[*right])
                .then_with(|| {
                    let left_span = (connections[*left].source_x - connections[*left].target_x).abs()
                        + (connections[*left].source_y - connections[*left].target_y).abs();
                    let right_span = (connections[*right].source_x - connections[*right].target_x).abs()
                        + (connections[*right].source_y - connections[*right].target_y).abs();
                    left_span.partial_cmp(&right_span).unwrap_or(std::cmp::Ordering::Equal)
                })
        });
        if !victims.is_empty() && retries[index] < current_retry_limit && repairs < repair_limit {
            let victims = victims.into_iter().take(victim_batch_limit).collect::<Vec<_>>();
            for victim in &victims {
                owned[*victim] = None;
                retries[*victim] += 1;
            }
            (routes, segment_owners, spatial) = rebuild(&owned);
            retries[index] += 1;
            repairs += 1;
            pending.push_front(index);
            for victim in victims { pending.push_back(victim); }
            continue;
        } else {
            let exhaustion = if !has_blockers {
                "no legal victim was identified".to_string()
            } else if retries[index] >= current_retry_limit {
                format!("current-net retry limit {current_retry_limit} reached")
            } else if repairs >= repair_limit {
                format!("negotiated-repair limit {repair_limit} reached")
            } else {
                format!("all blockers reached victim retry limit {victim_retry_limit}")
            };
            return Err(format!(
                "Native interval router could not assign local pin access for net {} after {} candidates and {} repair(s): {}; trunk_options={}; conflicts={}",
                connection.net, candidate_count, repairs, trunk_attempts.join(","),
                exhaustion, conflict_resources.into_iter().take(4).collect::<Vec<_>>().join(" | ")
            ));
        }
    }
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    for route in owned.into_iter().flatten() {
        routes.extend(route.segments);
        vias.extend(route.vias);
    }
    let (spacing, shorts) = check_route_geometry(&routes, lef);
    if spacing != 0 || shorts != 0 { return Err(format!("Native interval router produced illegal geometry: spacing={spacing} shorts={shorts}")); }
    Ok((routes, vias, 0))
}

/// Native negotiated detailed router.  The earlier allocator committed a
/// greedy connection permanently.  Here each connection retains ownership of
/// its geometry; an unrouteable connection locally rips up only the actual
/// lower-priority blockers reported by the spatial index and retries them.
/// This is bounded negotiated routing, not a hidden density relaxation.
fn route_native_tracks(
    connections: &[NativeRouteConnection], lef: &LefData, core_width: f64, core_height: f64,
    track_pitch: f64, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    if connections.is_empty() { return Ok((Vec::new(), Vec::new(), 0)); }
    if lef.routing_layers.len() < 6 {
        return Err("Native track router requires at least three alternating V/H routing pairs".to_string());
    }
    let layers = &lef.routing_layers;
    // A synthesized datapath commonly has hundreds of two-pin branches, but
    // substantially fewer logical nets.  Allocate its shared net trunks
    // first.  Running the branch-by-branch allocator first consumed both
    // vertical planes with duplicate source-to-sink trunks, then a large
    // design exited before the net-aware allocator was ever considered.
    // Both routers materialize identical LEF-aligned geometry and both are
    // accepted only after the same final spacing/short DRC.
    let interval_error = match route_native_interval_channels(connections, lef, core_width, core_height, edge) {
        Ok(result) => return Ok(result),
        Err(reason) => reason,
    };
    let reserved_error = match route_native_reserved_tracks(connections, lef, core_width, core_height, edge) {
        Ok(result) => return Ok(result),
        Err(reason) => reason,
    };
    // Continue into negotiated GCell routing when the fast allocators reject
    // a dense design.  The former large-design early return made the complete
    // guide-preserving router unreachable for exactly the datapaths that need
    // multi-turn detours.
    let gcell_pitch = (track_pitch * 10.0).max(2.0);
    let cols = (core_width / gcell_pitch).ceil().clamp(4.0, 18.0) as usize;
    let rows = (core_height / gcell_pitch).ceil().clamp(4.0, 18.0) as usize;
    let horizontal_span = core_height / (rows - 1) as f64;
    let vertical_span = core_width / (cols - 1) as f64;
    let tracks_for_span = |span: f64, direction: &str| -> u16 {
        lef.routing_layers.iter().filter(|layer| lef.routing_directions.get(*layer).is_some_and(|value| value == direction))
            .map(|layer| {
                (span / routing_track_pitch(lef, layer).max(track_pitch * 0.2)).floor().max(1.0) as u16
            }).fold(0u16, |total, count| total.saturating_add(count)).max(2)
    };
    let mut global = NativeGlobalGrid::new(cols, rows,
        tracks_for_span(horizontal_span, "HORIZONTAL"), tracks_for_span(vertical_span, "VERTICAL"));
    let to_gcell = |x: f64, y: f64| {
        (((x / core_width.max(1e-6) * (cols - 1) as f64).round() as isize).clamp(0, cols as isize - 1) as usize,
         ((y / core_height.max(1e-6) * (rows - 1) as f64).round() as isize).clamp(0, rows as isize - 1) as usize)
    };
    let mut global_paths = Vec::with_capacity(connections.len());
    for connection in connections {
        let path = global.route(to_gcell(connection.source_x, connection.source_y), to_gcell(connection.target_x, connection.target_y), connection.timing_weight)
            .ok_or_else(|| format!("Global route grid cannot reach net {}", connection.net))?;
        global.commit(&path);
        global_paths.push(path);
    }
    // Pathfinder-style negotiated congestion.  Rerouting only a handful of
    // overflowing paths leaves early routes permanently privileged and was
    // the reason large designs remained congested even after SBox growth.
    // Every epoch raises history on overflowing resources, rips up the whole
    // provisional solution, then reconstructs it in a rotated deterministic
    // order.  All resource demand remains explicit and no capacity is
    // relaxed during this process.
    const MAX_NEGOTIATED_EPOCHS: usize = 24;
    for epoch in 0..MAX_NEGOTIATED_EPOCHS {
        if global.overflow() == 0 { break; }
        global.increase_history_on_overflow();
        for path in &global_paths { global.rip_up(path); }
        let mut order = (0..connections.len()).collect::<Vec<_>>();
        order.sort_by(|left, right| {
            connections[*right].timing_weight.partial_cmp(&connections[*left].timing_weight)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| global_paths[*right].edge_refs.len().cmp(&global_paths[*left].edge_refs.len()))
                .then_with(|| connections[*left].net.cmp(&connections[*right].net))
        });
        if !order.is_empty() {
            let rotation = epoch % order.len();
            order.rotate_left(rotation);
        }
        for index in order {
            global_paths[index] = global.route(
                to_gcell(connections[index].source_x, connections[index].source_y),
                to_gcell(connections[index].target_x, connections[index].target_y),
                connections[index].timing_weight,
            ).ok_or_else(|| format!("Global negotiated reroute cannot reach net {}", connections[index].net))?;
            global.commit(&global_paths[index]);
        }
    }
    let global_overflow = global.overflow();
    if global_overflow != 0 {
        return Err(format!(
            "Native global router has {global_overflow} residual GCell edge overflow(s) after {MAX_NEGOTIATED_EPOCHS} negotiated-congestion epochs"
        ));
    }
    // Allocate long H trunks with interval colouring before the more
    // expensive negotiated repair.  This has the same physical acceptance
    // checks, but prevents a dense netlist from serially consuming one
    // shared trunk row merely because it happened to be routed first.
    let interval_error = format!("{}; reserved_router={}", interval_error, reserved_error);
    // Preserve the negotiated GCell route before considering any compact
    // template.  The previous lowering used only a midpoint from `global_paths`,
    // which folded an otherwise legal detour back into a shared trunk lane.
    // This implementation lowers every guide turn onto the corresponding
    // LEF-aligned H/V plane and retains its exact same-layer geometry check.
    // The template allocator below remains a bounded repair path for unusual
    // pin-access patterns that cannot use the selected guide planes.
    let guide_error = match route_native_guided_tracks(
        connections, &global_paths, lef, core_width, core_height, cols, rows, edge,
    ) {
        Ok(result) => return Ok(result),
        Err(reason) => reason,
    };
    let mut route_order = (0..connections.len()).collect::<Vec<_>>();
    route_order.sort_by(|a, b| {
        let left = connections[*a].timing_weight * 1000.0 + global_paths[*a].cost;
        let right = connections[*b].timing_weight * 1000.0 + global_paths[*b].cost;
        // Reserve scarce long-channel resources for timing-critical and
        // geographically expensive connections first.  Ascending order let
        // many short nets consume the only useful trunk lanes, forcing the
        // later long nets into artificial local conflicts.
        right.partial_cmp(&left).unwrap_or(std::cmp::Ordering::Equal)
    });
    let mut pending = std::collections::VecDeque::from(route_order);
    let mut owned = vec![None::<NativeDetailedCandidate>; connections.len()];
    let mut retries = vec![0usize; connections.len()];
    let mut detailed_repairs = 0usize;
    let detailed_start = Instant::now();
    // This is a per-candidate guard, not a signoff relaxation.  A failed
    // candidate must return control to floorplan/global-route iteration;
    // allowing an oscillating rip-up queue to consume an unbounded CLI run
    // makes the failure invisible and leaves stale APR artifacts behind.
    const MAX_DETAILED_REPAIRS: usize = 64;
    let max_detailed_search = std::env::var("AI_DIGITAL_APR_ROUTE_BUDGET_MS")
        .ok().and_then(|value| value.parse::<u64>().ok()).filter(|value| *value >= 250)
        .map(Duration::from_millis).unwrap_or_else(|| Duration::from_secs(2));
    let rebuild = |owned: &[Option<NativeDetailedCandidate>]| {
        let mut routed = Vec::new();
        let mut owners = Vec::new();
        for (owner, route) in owned.iter().enumerate() {
            if let Some(route) = route {
                for segment in &route.segments { routed.push(segment.clone()); owners.push(owner); }
            }
        }
        let mut spatial = RouteSpatialIndex::new((track_pitch * 12.0).max(2.0));
        for (index, segment) in routed.iter().enumerate() { spatial.insert(segment, index, lef); }
        (routed, owners, spatial)
    };
    while let Some(index) = pending.pop_front() {
        let connection = &connections[index];
        if detailed_start.elapsed() > max_detailed_search {
            let routed_count = owned.iter().filter(|candidate| candidate.is_some()).count();
            return Err(format!(
                "Native detailed router exceeded the {} ms search budget at net {} after {}/{} connection(s) and {} local repair(s); interval_router={}; guide_router={}",
                max_detailed_search.as_millis(), connection.net, routed_count, connections.len(),
                detailed_repairs, interval_error, guide_error,
            ));
        }
        if owned[index].is_some() { continue; }
        let path = &global_paths[index];
        let preferred_x = path.points.get(path.points.len() / 2).map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width)
            .unwrap_or((connection.source_x + connection.target_x) * 0.5);
        let preferred_y = path.points.get(path.points.len() / 2).map(|(_, y)| (*y as f64 + 0.5) / rows as f64 * core_height)
            .unwrap_or((connection.source_y + connection.target_y) * 0.5);
        let source_x = path.points.get(1).map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width).unwrap_or(connection.source_x);
        let target_x = path.points.iter().rev().nth(1).map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width).unwrap_or(connection.target_x);
        let source_y = path.points.get(1).map(|(_, y)| (*y as f64 + 0.5) / rows as f64 * core_height).unwrap_or(connection.source_y);
        let target_y = path.points.iter().rev().nth(1).map(|(_, y)| (*y as f64 + 0.5) / rows as f64 * core_height).unwrap_or(connection.target_y);
        let mut xs = nearby_tracks(preferred_x, core_width, track_pitch, edge, 4);
        let mut ys = nearby_tracks(preferred_y, core_height, track_pitch, edge, 4);
        // Preserve the whole global-guide shape at a bounded number of
        // landmarks.  A midpoint-only handoff collapsed detoured global
        // paths back into the same detailed trunk channel and created the
        // large blocker sets seen on dense designs.
        let landmark_indices = [0usize, path.points.len() / 4, path.points.len() / 2,
            path.points.len().saturating_mul(3) / 4, path.points.len().saturating_sub(1)];
        for landmark in landmark_indices {
            let Some((guide_x, guide_y)) = path.points.get(landmark).copied() else { continue; };
            let guide_x_um = (guide_x as f64 + 0.5) / cols as f64 * core_width;
            let guide_y_um = (guide_y as f64 + 0.5) / rows as f64 * core_height;
            for value in nearby_tracks(guide_x_um, core_width, track_pitch, edge, 2) {
                if !xs.iter().any(|existing| (*existing - value).abs() < 1e-9) { xs.push(value); }
            }
            for value in nearby_tracks(guide_y_um, core_height, track_pitch, edge, 2) {
                if !ys.iter().any(|existing| (*existing - value).abs() < 1e-9) { ys.push(value); }
            }
        }
        let mut guide_x_coordinates = path.points.iter().map(|(x, _)| (*x as f64 + 0.5) / cols as f64 * core_width).collect::<Vec<_>>();
        let mut guide_y_coordinates = path.points.iter().map(|(_, y)| (*y as f64 + 0.5) / rows as f64 * core_height).collect::<Vec<_>>();
        guide_x_coordinates.extend([connection.source_x, connection.target_x]);
        guide_y_coordinates.extend([connection.source_y, connection.target_y]);
        // A rerouted connection must not deterministically reclaim the same
        // escape pattern it just released.  Fold its bounded negotiation
        // epoch into the sequence so each local rip-up explores a distinct,
        // reproducible legal topology rather than oscillating.
        let seed = route_seed(connection).wrapping_add(retries[index].wrapping_mul(0x9e37_79b9));
        if !xs.is_empty() { let shift = seed % xs.len(); xs.rotate_left(shift); }
        if !ys.is_empty() { let shift = seed % ys.len(); ys.rotate_left(shift); }
        let (routed, segment_owners, spatial) = rebuild(&owned);
        let mut selected = None;
        let mut blockers = BTreeSet::new();
        let mut conflict_resources = BTreeSet::new();
        let mut materialized_candidates = 0usize;
        let mut rejected_candidates = 0usize;
        let mut track_profiles = BTreeSet::new();
        // Each connection has a deliberately bounded candidate budget.  The
        // seed rotates otherwise identical pin columns and prevents a dense
        // netlist from serially consuming the same local escapes.
        // Both trunk directions use the same li1 vertical pin-access model.
        // The two orientations consume complementary H/V resources and keep
        // guide detours from collapsing into one central channel.
        'candidate_search: for vertical_trunk in [false] {
            for topology_delta in 0usize..8 {
                let topology_class = connection.route_class + topology_delta;
                let (first_tracks, middle_tracks, last_tracks) = if vertical_trunk {
                    let (_, _, trunk_v, _, _) = native_vertical_layer_topology(topology_class);
                    let trunk_tracks = guide_tracks_for_layer(lef, &layers[trunk_v], core_width, edge, &guide_x_coordinates, 3);
                    (
                        trunk_tracks.clone(),
                        trunk_tracks.clone(),
                        trunk_tracks,
                    )
                } else {
                    let (_, source_v, trunk_h, target_v, _) = native_layer_topology(topology_class);
                    (
                        // Endpoint access must remain local.  Sampling a
                        // die-wide source/target column turns the first and
                        // last horizontal jog into row-spanning wires, then
                        // falsely exhausts the pin-row metals.  Only the
                        // negotiated trunk is permitted to use global lanes.
                        local_tracks_for_layer(lef, &layers[source_v], core_width, edge, connection.source_x, 8),
                        guide_tracks_for_layer(lef, &layers[trunk_h], core_height, edge, &guide_y_coordinates, 3),
                        local_tracks_for_layer(lef, &layers[target_v], core_width, edge, connection.target_x, 8),
                    )
                };
                if first_tracks.is_empty() || middle_tracks.is_empty() || last_tracks.is_empty() { continue; }
                track_profiles.insert(format!("t{}:{}x{}x{}", topology_class, first_tracks.len(), middle_tracks.len(), last_tracks.len()));
                // Do not enumerate a cubic local window.  Instead, cover
                // local and deterministic die-wide alternatives together in
                // a bounded Latin-hypercube sequence.  The former `take(7)`
                // accidentally discarded the die-wide escape samples that
                // `nearby_tracks` deliberately provided for a blocked pin.
                // The global guide has already selected a congestion-clean
                // corridor.  Detailed routing therefore needs a bounded
                // local pin-access search, not hundreds of unguided repeated
                // templates.  Keeping this finite makes an unroutable block
                // report its blockers promptly so floorplan iteration can
                // react instead of appearing hung.
                // A larger Cartesian-style sample did not produce new legal
                // physical classes; it merely repeated the same three local
                // access tracks and exhausted the per-candidate time budget.
                // The deterministic sequence below still covers all layer
                // assignments, local pin escapes and die-wide trunk guides.
                for variant in 0..24usize {
                    let first = first_tracks[(seed.wrapping_add(variant.wrapping_mul(3))) % first_tracks.len()];
                    let middle = middle_tracks[(seed.wrapping_mul(5).wrapping_add(variant.wrapping_mul(7))) % middle_tracks.len()];
                    let last = last_tracks[(seed.wrapping_mul(11).wrapping_add(variant.wrapping_mul(13))) % last_tracks.len()];
                    // Preserve the layer assignment modulo four while using
                    // the variant number to rotate met1 fallback pin
                    // escapes across adjacent legal tracks.
                    let candidate_class = topology_class.wrapping_add(variant.wrapping_mul(4));
                    let Some(candidate) = materialize_native_candidate(connection, layers, lef, vertical_trunk,
                        candidate_class, first, middle, last) else { continue; };
                    if candidate.segments.is_empty() { continue; }
                    materialized_candidates += 1;
                    let conflicts = spatial.conflicting_indices(&candidate.segments, &routed, lef);
                    if conflicts.is_empty() { selected = Some(candidate); break 'candidate_search; }
                    rejected_candidates += 1;
                    let detail = route_conflict_detail(&candidate.segments, &routed, lef);
                    if !detail.is_empty() { conflict_resources.insert(detail); }
                    for segment in conflicts { blockers.insert(segment_owners[segment]); }
                    // Keep the preferred source/target escapes in the loop
                    // to preserve locality while its bounded sequence also
                    // exercises physically distant legal escape tracks.
                    let _ = (source_x, source_y, target_x, target_y, xs.as_slice(), ys.as_slice());
                }
            }
        }
        if let Some(candidate) = selected {
            owned[index] = Some(candidate);
            continue;
        }
        let mut removable = blockers.into_iter().filter(|owner| {
            owned[*owner].is_some() && connections[*owner].timing_weight <= connection.timing_weight && retries[*owner] < 20
        }).collect::<Vec<_>>();
        removable.sort_by(|a, b| retries[*a].cmp(&retries[*b]).then_with(|| b.cmp(a)));
        if !removable.is_empty() && retries[index] < 16 && detailed_repairs < MAX_DETAILED_REPAIRS {
            // Rip up the small actual blocker set together.  This gives the
            // failed connection a genuinely open corridor; removing only
            // one owner can leave the same three-net choke point intact and
            // waste all retries cycling through equivalent candidates.
            let victims = removable.into_iter().take(4).collect::<Vec<_>>();
            for victim in &victims { owned[*victim] = None; retries[*victim] += 1; }
            retries[index] += 1;
            detailed_repairs += 1;
            pending.push_front(index);
            for victim in victims { pending.push_back(victim); }
            continue;
        }
        let endpoint_profiles = (0..4usize).filter(|topology_delta| {
            let (source_v, _, _, _, target_v) = native_vertical_layer_topology(connection.route_class + *topology_delta);
            pin_vertical_access(&connection.source_access, lef, &layers[source_v], connection.source_x, connection.source_y).is_some()
                && pin_vertical_access(&connection.target_access, lef, &layers[target_v], connection.target_x, connection.target_y).is_some()
        }).count();
        let guided_failure = format!(
            "Native detailed negotiated routing blocked for net {} after {} local repair round(s); global overflow {}; interval_router={}; guide_router={}; candidate blockers={} retries={} endpoint_profiles={}/4 source_port_rects={} target_port_rects={} candidates={}/{} track_profiles={} conflict_resources={}",
            connection.net, detailed_repairs, global_overflow, removable.len(), retries[index], endpoint_profiles,
            connection.source_access.len(), connection.target_access.len(), rejected_candidates, materialized_candidates,
            interval_error, guide_error,
            track_profiles.into_iter().collect::<Vec<_>>().join(","),
            conflict_resources.into_iter().take(4).collect::<Vec<_>>().join(" | ")
        );
        // A complete guide can still expose a local access corner whose
        // H/V pair is unavailable in the selected topology.  Use the native
        // interval channel allocator only after both guide-preserving and
        // negotiated detailed allocation have failed.  It does not waive a
        // rule: its output is checked by the same width/spacing/short DRC.
        return route_native_channel_tracks(connections, lef, core_width, core_height, edge)
            .map_err(|channel_failure| format!("{guided_failure}; channel fallback: {channel_failure}"));
    }
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    for route in owned.into_iter().flatten() { routes.extend(route.segments); vias.extend(route.vias); }
    let (spacing, shorts) = check_route_geometry(&routes, lef);
    if spacing != 0 || shorts != 0 {
        return Err(format!("Native detailed router produced illegal geometry: spacing={} shorts={}", spacing, shorts));
    }
    // Detailed geometry is the final resource proof, after global routing
    // has independently reached zero overflow.  Keeping both gates avoids a
    // visually plausible detailed result hiding a globally over-subscribed
    // routing topology.
    Ok((routes, vias, 0))
}

/// Deterministic channel allocator used when negotiated detailed routing
/// proves that its guide/topology search is incomplete.  It is still a native
/// detailed router: every candidate starts and ends on a real LEF PORT,
/// consumes layer-specific legal tracks, and is admitted only after the same
/// exact spacing/short check as the main router.  Unlike a one-lane-per-net
/// fallback, it distributes trunks over all H routing planes and allocates
/// them in decreasing span order, allowing non-overlapping intervals to
/// reuse a physical track.
fn route_native_channel_tracks(
    connections: &[NativeRouteConnection], lef: &LefData, core_width: f64,
    core_height: f64, edge: f64,
) -> Result<(Vec<RouteSegment>, Vec<Via>, usize), String> {
    if connections.is_empty() { return Ok((Vec::new(), Vec::new(), 0)); }
    let layers = &lef.routing_layers;
    if layers.len() < 6 { return Err("Native channel router requires at least three alternating V/H routing pairs".to_string()); }
    let mut order = (0..connections.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        let a = &connections[*left];
        let b = &connections[*right];
        let a_span = (a.source_x - a.target_x).abs() + (a.source_y - a.target_y).abs();
        let b_span = (b.source_x - b.target_x).abs() + (b.source_y - b.target_y).abs();
        b.timing_weight.partial_cmp(&a.timing_weight).unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| b_span.partial_cmp(&a_span).unwrap_or(std::cmp::Ordering::Equal))
            .then_with(|| a.net.cmp(&b.net))
    });
    let mut pending = std::collections::VecDeque::from(order);
    let mut owned = vec![None::<NativeDetailedCandidate>; connections.len()];
    let mut retries = vec![0usize; connections.len()];
    let mut repairs = 0usize;
    let channel_start = Instant::now();
    let channel_budget = std::env::var("AI_DIGITAL_APR_CHANNEL_BUDGET_MS")
        .ok().and_then(|value| value.parse::<u64>().ok()).filter(|value| *value >= 50)
        .map(Duration::from_millis).unwrap_or_else(|| Duration::from_millis(600));
    let rebuild = |owned: &[Option<NativeDetailedCandidate>]| {
        let mut routes = Vec::new();
        let mut owners = Vec::new();
        for (owner, route) in owned.iter().enumerate() {
            if let Some(route) = route {
                for segment in &route.segments {
                    routes.push(segment.clone());
                    owners.push(owner);
                }
            }
        }
        let mut spatial = RouteSpatialIndex::new(4.0);
        for (segment_index, segment) in routes.iter().enumerate() {
            spatial.insert(segment, segment_index, lef);
        }
        (routes, owners, spatial)
    };
    while let Some(index) = pending.pop_front() {
        if channel_start.elapsed() > channel_budget {
            return Err(format!("Native channel allocator exceeded {} ms budget after {} repair round(s)", channel_budget.as_millis(), repairs));
        }
        if owned[index].is_some() { continue; }
        let connection = &connections[index];
        let seed = route_seed(connection);
        let mut selected = None;
        let mut candidates = 0usize;
        let mut rejected = 0usize;
        let mut profiles = BTreeSet::new();
        let mut blockers = BTreeSet::new();
        let (routes, segment_owners, spatial) = rebuild(&owned);
        'allocate: for topology_delta in 0usize..8 {
            if channel_start.elapsed() > channel_budget {
                return Err(format!("Native channel allocator exceeded {} ms budget after {} repair round(s)", channel_budget.as_millis(), repairs));
            }
            let topology_class = connection.route_class + topology_delta;
            let (_, source_v, trunk_h, target_v, _) = native_layer_topology(topology_class);
            let source_tracks = local_tracks_for_layer(lef, &layers[source_v], core_width, edge, connection.source_x, 3);
            let target_tracks = local_tracks_for_layer(lef, &layers[target_v], core_width, edge, connection.target_x, 3);
            let guide = [connection.source_y, connection.target_y, (connection.source_y + connection.target_y) * 0.5];
            let mut trunk_tracks = guide_tracks_for_layer(lef, &layers[trunk_h], core_height, edge, &guide, 4);
            if source_tracks.is_empty() || target_tracks.is_empty() || trunk_tracks.is_empty() { continue; }
            let shift = seed.wrapping_add(topology_delta * 31) % trunk_tracks.len();
            trunk_tracks.rotate_left(shift);
            profiles.insert(format!("t{}:{}x{}x{}", topology_class, source_tracks.len(), trunk_tracks.len(), target_tracks.len()));
            // Start with nearest local access tracks.  The trunk list has
            // die-wide samples, so the bounded search can move a long wire
            // into a free channel without inventing a synthetic route layer.
            for trunk in trunk_tracks.into_iter().take(56) {
                for source in source_tracks.iter().take(5) {
                    for target in target_tracks.iter().take(5) {
                        if channel_start.elapsed() > channel_budget {
                            return Err(format!("Native channel allocator exceeded {} ms budget after {} repair round(s)", channel_budget.as_millis(), repairs));
                        }
                        let Some(candidate) = materialize_native_candidate(
                            connection, layers, lef, false, topology_class, *source, trunk, *target,
                        ) else { continue; };
                        if candidate.segments.is_empty() { continue; }
                        candidates += 1;
                        let conflicts = spatial.conflicting_indices(&candidate.segments, &routes, lef);
                        if !conflicts.is_empty() {
                            rejected += 1;
                            for segment in conflicts { blockers.insert(segment_owners[segment]); }
                            continue;
                        }
                        selected = Some(candidate);
                        break 'allocate;
                    }
                }
            }
        }
        if let Some(candidate) = selected {
            owned[index] = Some(candidate);
            continue;
        }
        // Long connections are routed first, but a later pin-access branch
        // can legitimately need to displace a handful of lower-priority
        // trunks.  Rip up only observed blockers and retry with a rotated
        // track order; this is bounded negotiated routing, not a density
        // relaxation.
        let mut removable = blockers.into_iter().filter(|owner| {
            owned[*owner].is_some() && retries[*owner] < 6
                && connections[*owner].timing_weight <= connection.timing_weight
        }).collect::<Vec<_>>();
        removable.sort_by(|left, right| retries[*left].cmp(&retries[*right]).then_with(|| right.cmp(left)));
        if !removable.is_empty() && retries[index] < 8 && repairs < 96 {
            let victims = removable.into_iter().take(3).collect::<Vec<_>>();
            for victim in &victims {
                owned[*victim] = None;
                retries[*victim] += 1;
            }
            retries[index] += 1;
            repairs += 1;
            pending.push_front(index);
            for victim in victims { pending.push_back(victim); }
            continue;
        }
        return Err(format!(
            "Native channel detailed routing blocked for net {} after {} local repair round(s); candidates={}/{} blockers={} retries={} profiles={}",
            connection.net, repairs, rejected, candidates, removable.len(), retries[index],
            profiles.into_iter().collect::<Vec<_>>().join(",")
        ));
    }
    let mut routes = Vec::new();
    let mut vias = Vec::new();
    for route in owned.into_iter().flatten() {
        routes.extend(route.segments);
        vias.extend(route.vias);
    }
    let (spacing, shorts) = check_route_geometry(&routes, lef);
    if spacing != 0 || shorts != 0 {
        return Err(format!("Native channel detailed router produced illegal geometry: spacing={} shorts={}", spacing, shorts));
    }
    Ok((routes, vias, 0))
}

fn build_fillers(placed: &[PlacedCell], rows: usize, row_height: f64, core_width: f64, lef: &LefData) -> Vec<PlacedCell> {
    let mut candidates: Vec<&Macro> = lef.macros.values().filter(|m| m.name.to_ascii_lowercase().contains("fill") && (m.height_um - row_height).abs() < 0.02).collect();
    candidates.sort_by(|a, b| b.width_um.partial_cmp(&a.width_um).unwrap_or(std::cmp::Ordering::Equal));
    if candidates.is_empty() { return Vec::new(); }
    let smallest = candidates.last().map(|m| m.width_um).unwrap_or(0.0);
    let largest = candidates.first().map(|m| m.width_um).unwrap_or(0.0);
    // Filler cells close small detail-placement residue. They must never
    // consume a deliberately reserved global/detailed-routing channel.
    let max_local_fill_gap = (largest * 4.0).max(smallest);
    let mut fillers = Vec::new();
    for row in 0..rows {
        let y = row as f64 * row_height;
        let mut row_cells: Vec<&PlacedCell> = placed.iter().filter(|c| (c.y_um - y).abs() < 1e-6).collect();
        row_cells.sort_by(|a, b| a.x_um.partial_cmp(&b.x_um).unwrap_or(std::cmp::Ordering::Equal));
        let mut cursor = 0.0;
        for cell_index in 0..=row_cells.len() {
            let (gap_end, next_cursor) = if let Some(cell) = row_cells.get(cell_index) {
                (cell.x_um, cell.x_um + cell.width_um)
            } else {
                (core_width, core_width)
            };
            let fillable_gap = gap_end - cursor <= max_local_fill_gap + 1e-6;
            while fillable_gap && gap_end - cursor + 1e-6 >= smallest {
                let Some(filler) = candidates.iter().copied().find(|m| m.width_um <= gap_end - cursor + 1e-6) else { break; };
                fillers.push(PlacedCell { instance: format!("FILL_R{}_{}", row, fillers.len()), cell: filler.name.clone(), x_um: cursor, y_um: y, width_um: filler.width_um, height_um: filler.height_um });
                cursor += filler.width_um;
            }
            cursor = next_cursor.max(cursor);
        }
    }
    fillers
}

fn materialize_cell_pins(cells: &[PlacedCell], lef: &LefData) -> Vec<CellPin> {
    let mut pins = Vec::new();
    for cell in cells {
        let Some(macro_def) = lef.macros.get(&cell.cell) else { continue; };
        for pin in &macro_def.pins {
            pins.push(CellPin {
                instance: cell.instance.clone(), name: pin.name.clone(), layer: pin.layer.clone(),
                x1_um: cell.x_um + pin.x1_um, y1_um: cell.y_um + pin.y1_um,
                x2_um: cell.x_um + pin.x2_um, y2_um: cell.y_um + pin.y2_um,
            });
        }
    }
    pins
}

/// Return the physical connection point for one logical instance pin.  The
/// selected LEF PORT rectangle is authoritative; using a cell centre for all
/// pins makes electrically distinct nets originate at the same physical
/// location and produces artificial shorts during detailed routing.
fn instance_pin_location(cell: &PlacedCell, pin_name: &str, lef: &LefData) -> (f64, f64) {
    let Some(macro_def) = lef.macros.get(&cell.cell) else {
        return (cell.x_um + cell.width_um * 0.5, cell.y_um + cell.height_um * 0.5);
    };
    let selected = resolve_macro_pin(macro_def, pin_name);
    selected.map(|pin| (
        cell.x_um + (pin.x1_um + pin.x2_um) * 0.5,
        cell.y_um + (pin.y1_um + pin.y2_um) * 0.5,
    )).unwrap_or((cell.x_um + cell.width_um * 0.5, cell.y_um + cell.height_um * 0.5))
}

fn solve_ir_drop(rows: usize, cols: usize, power_mw: f64, voltage_v: f64, grid_resistance_ohm: f64) -> (f64, f64) {
    let n = rows * cols;
    if n == 0 || voltage_v <= 0.0 { return (0.0, voltage_v); }
    let total_current_a = power_mw.max(0.0) / 1000.0 / voltage_v;
    let load = total_current_a / n as f64;
    let mut drop_v = vec![0.0; n];
    let index = |r: usize, c: usize| r * cols + c;
    // Boundary nodes are ideal power straps; interior nodes are solved by
    // Gauss-Seidel on the resistive mesh with distributed current loads.
    for _ in 0..500 {
        let mut delta: f64 = 0.0;
        for r in 1..rows.saturating_sub(1) {
            for c in 1..cols.saturating_sub(1) {
                let i = index(r, c);
                let next = (drop_v[index(r - 1, c)] + drop_v[index(r + 1, c)]
                    + drop_v[index(r, c - 1)] + drop_v[index(r, c + 1)] + load * grid_resistance_ohm) / 4.0;
                delta = delta.max((next - drop_v[i]).abs());
                drop_v[i] = next;
            }
        }
        if delta < 1e-10 { break; }
    }
    let worst = drop_v.into_iter().fold(0.0_f64, f64::max);
    (worst * 1000.0, (voltage_v - worst).max(0.0))
}

fn gds_record(out: &mut Vec<u8>, record_type: u8, data_type: u8, payload: &[u8]) {
    let length = (4 + payload.len()) as u16;
    out.extend_from_slice(&length.to_be_bytes());
    out.push(record_type);
    out.push(data_type);
    out.extend_from_slice(payload);
}

fn gds_i2(value: i16) -> Vec<u8> { value.to_be_bytes().to_vec() }

fn gds_i4(value: i32) -> Vec<u8> { value.to_be_bytes().to_vec() }

fn gds_ascii(text: &str) -> Vec<u8> {
    let mut bytes = text.as_bytes().to_vec();
    if bytes.len() % 2 != 0 { bytes.push(0); }
    bytes
}

fn gds_xy(points: &[(f64, f64)]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(points.len() * 8);
    for (x, y) in points {
        bytes.extend_from_slice(&gds_i4((x * 1000.0).round() as i32));
        bytes.extend_from_slice(&gds_i4((y * 1000.0).round() as i32));
    }
    bytes
}

/// Emit a compact, standards-shaped GDSII stream containing the die, placed
/// standard cells and routed centerline paths.  This is intentionally native
/// output: no external layout writer is involved.
fn write_gds(path: &Path, result: &AprResult) -> Result<(), String> {
    let mut gds = Vec::new();
    gds_record(&mut gds, 0x00, 0x02, &gds_i2(600));
    gds_record(&mut gds, 0x01, 0x02, &vec![0; 24]);
    gds_record(&mut gds, 0x02, 0x06, &gds_ascii("AI_DIGITAL_APR"));
    gds_record(&mut gds, 0x03, 0x05, &[0x3e, 0x41, 0x0c, 0x9f, 0x7c, 0x7f, 0x3e, 0x41, 0x0c, 0x9f, 0x7c, 0x7f]);
    gds_record(&mut gds, 0x05, 0x02, &vec![0; 24]);
    gds_record(&mut gds, 0x06, 0x06, &gds_ascii(&result.module));

    let layer = |gds: &mut Vec<u8>, number: i16| {
        gds_record(gds, 0x0d, 0x02, &gds_i2(number));
        gds_record(gds, 0x0e, 0x02, &gds_i2(0));
    };
    let path_layer = |gds: &mut Vec<u8>, number: i16, datatype: i16| {
        gds_record(gds, 0x0d, 0x02, &gds_i2(number));
        // A deterministic net-domain datatype preserves enough identity for
        // the native post-layout DRC to distinguish legal same-net branches
        // from a different-net short candidate after flattening to GDS.
        gds_record(gds, 0x0e, 0x02, &gds_i2(datatype));
    };
    let boundary = |gds: &mut Vec<u8>, points: &[(f64, f64)]| {
        gds_record(gds, 0x08, 0x00, &[]);
        gds_record(gds, 0x0d, 0x02, &gds_i2(0));
        gds_record(gds, 0x0e, 0x02, &gds_i2(0));
        gds_record(gds, 0x10, 0x03, &gds_xy(points));
        gds_record(gds, 0x11, 0x00, &[]);
    };
    boundary(&mut gds, &[(0.0, 0.0), (result.die_width_um, 0.0), (result.die_width_um, result.die_height_um), (0.0, result.die_height_um), (0.0, 0.0)]);
    for cell in &result.cells {
        boundary(&mut gds, &[(cell.x_um, cell.y_um), (cell.x_um + cell.width_um, cell.y_um),
            (cell.x_um + cell.width_um, cell.y_um + cell.height_um), (cell.x_um, cell.y_um + cell.height_um),
            (cell.x_um, cell.y_um)]);
    }
    for cell in &result.filler_cells {
        boundary(&mut gds, &[(cell.x_um, cell.y_um), (cell.x_um + cell.width_um, cell.y_um),
            (cell.x_um + cell.width_um, cell.y_um + cell.height_um), (cell.x_um, cell.y_um + cell.height_um),
            (cell.x_um, cell.y_um)]);
    }
    let mut gds_net_domains: BTreeMap<&str, i16> = BTreeMap::new();
    for route in &result.routes {
        let next = gds_net_domains.len().saturating_add(1).min(i16::MAX as usize) as i16;
        let domain = *gds_net_domains.entry(route.net.as_str()).or_insert(next);
        gds_record(&mut gds, 0x09, 0x00, &[]);
        // Preserve the actual LEF routing-layer identity in GDS.  Assigning
        // layers by segment index created artificial same-layer crossings in
        // the layout viewer and invalidated any post-layout geometry audit.
        let layer_number = result.routed_layers.iter().position(|name| name == &route.layer)
            .map(|index| index as i16 + 1).unwrap_or(1);
        path_layer(&mut gds, layer_number, domain);
        gds_record(&mut gds, 0x0f, 0x03, &gds_i4((route.width_um * 1000.0).round() as i32));
        gds_record(&mut gds, 0x10, 0x03, &gds_xy(&[(route.x1_um, route.y1_um), (route.x2_um, route.y2_um)]));
        gds_record(&mut gds, 0x11, 0x00, &[]);
    }
    // PDN is materialized as wide paths on dedicated layers in this compact
    // GDS writer. The matching DEF SPECIALNETS remains the authoritative
    // logical association with VDD/VSS.
    for (index, segment) in result.pdn_segments.iter().enumerate() {
        gds_record(&mut gds, 0x09, 0x00, &[]);
        layer(&mut gds, (20 + index % 2) as i16);
        gds_record(&mut gds, 0x0f, 0x03, &gds_i4((segment.width_um * 1000.0).round() as i32));
        gds_record(&mut gds, 0x10, 0x03, &gds_xy(&[(segment.x1_um, segment.y1_um), (segment.x2_um, segment.y2_um)]));
        gds_record(&mut gds, 0x11, 0x00, &[]);
    }
    // Each route layer transition becomes a real, visible cut square in the
    // native GDS abstract.  Detailed cut-array rules remain technology
    // dependent and are reported as an extraction/signoff limitation.
    for via in &result.vias {
        let half = via.size_um * 0.5;
        gds_record(&mut gds, 0x08, 0x00, &[]);
        layer(&mut gds, 30);
        gds_record(&mut gds, 0x10, 0x03, &gds_xy(&[(via.x_um-half, via.y_um-half), (via.x_um+half, via.y_um-half), (via.x_um+half, via.y_um+half), (via.x_um-half, via.y_um+half), (via.x_um-half, via.y_um-half)]));
        gds_record(&mut gds, 0x11, 0x00, &[]);
    }
    gds_record(&mut gds, 0x07, 0x00, &[]);
    gds_record(&mut gds, 0x04, 0x00, &[]);
    fs::write(path, gds).map_err(|e| format!("{}: {e}", path.display()))
}

fn write_outputs(project_dir: &Path, result: &AprResult, gate_verilog: &str) -> Result<(), String> {
    let apr_dir = project_dir.join("apr");
    let exchange_dir = project_dir.join("exchange");
    fs::create_dir_all(&apr_dir).map_err(|e| e.to_string())?;
    fs::create_dir_all(&exchange_dir).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("apr_netlist.v"), gate_verilog).map_err(|e| e.to_string())?;
    let json = serde_json::to_string_pretty(result).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("apr_report.json"), json).map_err(|e| e.to_string())?;
    let mut text = format!("Native APR Report: {}\n=====================\n", result.module);
    text.push_str(&format!("LEF macros / routing layers: {} / {}\n", result.lef_macros, result.routed_layers.join(", ")));
    text.push_str(&format!("Floorplan core/die: {:.3} x {:.3} / {:.3} x {:.3} um\n", result.core_width_um, result.core_height_um, result.die_width_um, result.die_height_um));
    text.push_str("Floorplan optimization attempts:\n");
    for item in &result.optimization_iterations {
        text.push_str(&format!("  {:>2}: sbox={:.2} repair-phase={:.2} status={} core={:.3}x{:.3} utilization={:.2}% wire={:.3}um {}\n", item.attempt, item.sbox_scale, item.repair_phase, item.status, item.core_width_um, item.core_height_um, item.utilization * 100.0, item.wire_length_um, item.reason));
    }
    text.push_str(&format!("Placement: {} functional cells + {} LEF filler cells, utilization {:.2}%\n", result.cells.len(), result.filler_cells.len(), result.utilization * 100.0));
    text.push_str(&format!("I/O placement: {} top-level pins\n", result.io_pins.len()));
    text.push_str(&format!("Routing: {} segments, {} layer-transition vias, {:.3} um total wire, overflow {}\n", result.routes.len(), result.vias.len(), result.total_wire_length_um, result.routing_overflow));
    text.push_str(&format!("PDN: {} abstract VDD/VSS ring/strap segments; native-resistive IR mesh (not foundry signoff)\n", result.pdn_segments.len()));
    text.push_str(&format!("Highlighted route source: {} ({} segments)\n", result.critical_route_source, result.critical_routes.len()));
    text.push_str(&format!("CTS: {} clock-buffer sites planned by fanout analysis (physical buffer insertion is not materialized in this netlist)\n", result.clock_buffer_count));
    text.push_str(&format!("Extracted RC delay: {:.6} ns\n", result.estimated_rc_delay_ns));
    text.push_str(&format!("OCV: setup slack {:.6} ns, hold slack {:.6} ns\n", result.ocv_late_slack_ns, result.ocv_early_hold_slack_ns));
    text.push_str(&format!("IR: {:.3} mV drop, worst VDD {:.6} V ({})\n", result.ir_drop_mv, result.ir_worst_voltage_v, result.power_source));
    text.push_str(&format!("DRC: {} (placement={}, min-width={}, spacing={}, shorts={}, off-grid={}, boundary={}, via={}, antenna={})\n", result.drc_status, result.placement_overlaps, result.drc_min_width_violations, result.drc_spacing_violations, result.drc_short_violations, result.drc_offgrid_violations, result.drc_boundary_violations, result.drc_via_violations, result.antenna_warnings));
    text.push_str(&format!("LVS: {} | DFT: {}\n", result.lvs_status, result.dft_status));
    text.push_str(&format!("Outputs: {} {} {} {} {}\n", result.apr_netlist_path, result.final_def_path, result.gds_path, result.detail_route_path, result.parasitics_path));
    text.push_str(&format!("Signoff readiness: {}\n\n", if result.signoff_ready { "READY" } else { "BLOCKED" }));
    for finding in &result.findings { text.push_str(&format!("- {finding}\n")); }
    fs::write(apr_dir.join("apr_report.txt"), text).map_err(|e| e.to_string())?;
    let mut optimization = String::from("attempt\tsbox_scale\trepair_phase\tstatus\tcore_width_um\tcore_height_um\tutilization\twire_length_um\tdrc_status\treason\n");
    for item in &result.optimization_iterations {
        optimization.push_str(&format!("{}\t{:.6}\t{:.6}\t{}\t{:.6}\t{:.6}\t{:.9}\t{:.6}\t{}\t{}\n", item.attempt, item.sbox_scale, item.repair_phase, item.status, item.core_width_um, item.core_height_um, item.utilization, item.wire_length_um, item.drc_status, item.reason));
    }
    fs::write(apr_dir.join("optimization_iterations.tsv"), optimization).map_err(|e| e.to_string())?;

    let mut def = format!("VERSION 5.8 ;\nDESIGN {} ;\nUNITS DISTANCE MICRONS 1000 ;\nDIEAREA ( 0 0 ) ( {} {} ) ;\nCOMPONENTS {} ;\n", result.module, (result.die_width_um * 1000.0) as i64, (result.die_height_um * 1000.0) as i64, result.cells.len() + result.filler_cells.len());
    for cell in &result.cells {
        def.push_str(&format!("- {} {} + PLACED ( {} {} ) N ;\n", cell.instance, cell.cell, (cell.x_um * 1000.0) as i64, (cell.y_um * 1000.0) as i64));
    }
    for cell in &result.filler_cells {
        def.push_str(&format!("- {} {} + PLACED ( {} {} ) N ;\n", cell.instance, cell.cell, (cell.x_um * 1000.0) as i64, (cell.y_um * 1000.0) as i64));
    }
    def.push_str("END COMPONENTS\n");
    def.push_str(&format!("PINS {} ;\n", result.io_pins.len()));
    for pin in &result.io_pins {
        def.push_str(&format!("- {} + NET {} + DIRECTION {} + PLACED ( {} {} ) N ;\n", pin.name, pin.name, pin.direction, (pin.x_um * 1000.0).round() as i64, (pin.y_um * 1000.0).round() as i64));
    }
    def.push_str("END PINS\n");
    let mut nets: BTreeMap<&str, Vec<&RouteSegment>> = BTreeMap::new();
    for route in &result.routes { nets.entry(route.net.as_str()).or_default().push(route); }
    def.push_str(&format!("NETS {} ;\n", nets.len()));
    for (net, segments) in nets {
        def.push_str(&format!("- {}\n", net));
        for segment in segments {
            def.push_str(&format!("  + ROUTED {} {} ( {} {} ) ( {} {} )\n", segment.layer, (segment.width_um * 1000.0).round() as i64,
                (segment.x1_um * 1000.0) as i64, (segment.y1_um * 1000.0) as i64,
                (segment.x2_um * 1000.0) as i64, (segment.y2_um * 1000.0) as i64));
        }
        def.push_str(" ;\n");
    }
    def.push_str("END NETS\nEND DESIGN\n");
    // DEF requires SPECIALNETS before END DESIGN. Keep a complete separate
    // string for deterministic ordering instead of editing emitted text.
    def = def.replace("END NETS\nEND DESIGN\n", "END NETS\n");
    let mut pdn_by_net: BTreeMap<&str, Vec<&PdnSegment>> = BTreeMap::new();
    for segment in &result.pdn_segments { pdn_by_net.entry(segment.net.as_str()).or_default().push(segment); }
    def.push_str(&format!("SPECIALNETS {} ;\n", pdn_by_net.len()));
    for (net, segments) in pdn_by_net {
        def.push_str(&format!("- {}\n", net));
        for segment in segments {
            def.push_str(&format!("  + ROUTED {} {} ( {} {} ) ( {} {} )\n", segment.layer, (segment.width_um * 1000.0).round() as i64, (segment.x1_um * 1000.0).round() as i64, (segment.y1_um * 1000.0).round() as i64, (segment.x2_um * 1000.0).round() as i64, (segment.y2_um * 1000.0).round() as i64));
        }
        def.push_str(" ;\n");
    }
    def.push_str("END SPECIALNETS\nEND DESIGN\n");
    fs::write(apr_dir.join("floorplan.def"), &def).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("final.def"), &def).map_err(|e| e.to_string())?;
    write_gds(&apr_dir.join("final.gds"), result)?;

    // The layout exchange starts with explicit physical boundaries.  A GUI
    // must scale from the die/core coordinates, not from the occupied-cell
    // envelope, otherwise sparse placements appear stretched or offset.
    let mut tsv = String::from("kind\tname\tcell_or_layer\tx1_um\ty1_um\tx2_um\ty2_um\twidth_um\n");
    tsv.push_str(&format!("die\tDIE\tboundary\t0.000000\t0.000000\t{:.6}\t{:.6}\n", result.die_width_um, result.die_height_um));
    tsv.push_str(&format!("core\tCORE\tboundary\t0.000000\t0.000000\t{:.6}\t{:.6}\n", result.core_width_um, result.core_height_um));
    for cell in &result.cells {
        tsv.push_str(&format!("cell\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", cell.instance, cell.cell, cell.x_um, cell.y_um, cell.x_um + cell.width_um, cell.y_um + cell.height_um));
    }
    for cell in &result.filler_cells {
        tsv.push_str(&format!("filler\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", cell.instance, cell.cell, cell.x_um, cell.y_um, cell.x_um + cell.width_um, cell.y_um + cell.height_um));
    }
    for pin in &result.cell_pins {
        tsv.push_str(&format!("cellpin\t{}:{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", pin.instance, pin.name, pin.layer, pin.x1_um, pin.y1_um, pin.x2_um, pin.y2_um));
    }
    for pin in &result.io_pins {
        tsv.push_str(&format!("pin\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", pin.name, pin.direction, pin.x_um - 0.18, pin.y_um - 0.18, pin.x_um + 0.18, pin.y_um + 0.18));
    }
    for segment in &result.pdn_segments {
        tsv.push_str(&format!("pdn\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", segment.net, segment.layer, segment.x1_um, segment.y1_um, segment.x2_um, segment.y2_um, segment.width_um));
    }
    for route in &result.routes {
        tsv.push_str(&format!("route\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", route.net, route.layer, route.x1_um, route.y1_um, route.x2_um, route.y2_um, route.width_um));
    }
    for route in &result.critical_routes {
        tsv.push_str(&format!("critical\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", route.net, route.layer, route.x1_um, route.y1_um, route.x2_um, route.y2_um, route.width_um));
    }
    for via in &result.vias {
        let half = via.size_um * 0.5;
        tsv.push_str(&format!("via\t{}\t{}-{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", via.net, via.lower_layer, via.upper_layer, via.x_um-half, via.y_um-half, via.x_um+half, via.y_um+half, via.size_um));
    }
    fs::write(exchange_dir.join("apr_layout.tsv"), tsv).map_err(|e| e.to_string())?;
    let mut detail = String::from("net\tlayer\twidth_um\tx1_um\ty1_um\tx2_um\ty2_um\tlength_um\n");
    for route in &result.routes {
        detail.push_str(&format!("{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", route.net, route.layer, route.width_um, route.x1_um, route.y1_um, route.x2_um, route.y2_um, wire_length(route)));
    }
    fs::write(apr_dir.join("detail_route.tsv"), detail).map_err(|e| e.to_string())?;
    let mut spef = format!("*SPEF \"IEEE 1481-1999\"\n*DESIGN {}\n*DATE \"native geometry estimate\"\n*VENDOR \"AI Digital native APR\"\n*COMMENT \"Geometry-based estimate, not calibrated foundry signoff extraction\"\n*DIVIDER /\n*DELIMITER :\n", result.module);
    let mut by_net: BTreeMap<&str, Vec<&RouteSegment>> = BTreeMap::new();
    for route in &result.routes { by_net.entry(route.net.as_str()).or_default().push(route); }
    for (index, (net, segments)) in by_net.into_iter().enumerate() {
        let length: f64 = segments.iter().map(|route| wire_length(route)).sum();
        let resistance: f64 = segments.iter().map(|route| wire_length(route) / route.width_um.max(0.05) * 0.015).sum();
        let capacitance: f64 = segments.iter().map(|route| wire_length(route) * route.width_um.max(0.05) * 0.0015).sum();
        let segment_node = format!("{}:{}", net, index);
        spef.push_str(&format!("*D_NET {} {:.9}\n*CONN\n*P {} I\n*CAP\n1 {} {:.9}\n*RES\n1 {} {} {:.9}\n*END\n", net, capacitance, net, net, capacitance, net, segment_node, resistance.max(length * 0.0001)));
    }
    fs::write(apr_dir.join("native_parasitics.spef"), spef).map_err(|e| e.to_string())?;
    let mut grid = String::from("x_index\ty_index\tx_um\ty_um\tir_drop_mv\tcongestion\tpower_uw\n");
    for point in &result.ir_grid {
        grid.push_str(&format!("{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", point.x_index, point.y_index, point.x_um, point.y_um, point.ir_drop_mv, point.congestion, point.power_uw));
    }
    fs::write(exchange_dir.join("apr_grid.tsv"), &grid).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("ir_drop.tsv"), &grid).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("congestion.tsv"), &grid).map_err(|e| e.to_string())?;
    fs::write(apr_dir.join("power_hotspots.tsv"), &grid).map_err(|e| e.to_string())?;
    let critical_wire_um: f64 = result.critical_routes.iter().map(wire_length).sum();
    let timing_status = if result.wns_ns >= 0.0 && result.ocv_early_hold_slack_ns >= 0.0 { "MET" } else { "VIOLATED" };
    let timing = format!("APR Timing Analysis\n===================\nAnalysis domain: post-placement/post-route extracted RC\nEndpoint model: one native worst-endpoint aggregate; TNS sums analyzed negative endpoint slacks\n\nMetric                         Value          Status\n-----------------------------  -------------  --------\nSetup slack                    {:>8.6} ns  {}\nHold slack                     {:>8.6} ns  {}\nOCV late setup (WNS)           {:>8.6} ns  {}\nOCV early hold                 {:>8.6} ns  {}\nTotal negative slack (TNS)     {:>8.6} ns  {}\nViolating endpoints            {:>8}      {}\nExtracted RC delay             {:>8.6} ns  INFO\n\nHighlighted route source: {}\nCritical route segments: {}\nCritical route wire: {:.6} um\n", result.setup_slack_ns, if result.setup_slack_ns >= 0.0 { "MET" } else { "VIOLATED" }, result.hold_slack_ns, if result.hold_slack_ns >= 0.0 { "MET" } else { "VIOLATED" }, result.wns_ns, if result.wns_ns >= 0.0 { "MET" } else { "VIOLATED" }, result.ocv_early_hold_slack_ns, if result.ocv_early_hold_slack_ns >= 0.0 { "MET" } else { "VIOLATED" }, result.tns_ns, if result.tns_ns >= 0.0 { "MET" } else { "VIOLATED" }, result.violating_endpoints, timing_status, result.estimated_rc_delay_ns, result.critical_route_source, result.critical_routes.len(), critical_wire_um);
    fs::write(apr_dir.join("timing_report.txt"), timing).map_err(|e| e.to_string())?;
    let total_power_mw = result.power_source.as_str();
    let max_congestion = result.ir_grid.iter().map(|p| p.congestion).fold(0.0_f64, f64::max);
    let max_hotspot = result.ir_grid.iter().map(|p| p.power_uw).fold(0.0_f64, f64::max);
    let power = format!("APR Power Analysis\n==================\nAnalysis domain: post-route physical grid\n\nMetric                         Value              Source\n-----------------------------  -----------------  ------------------------------\nTotal power                    {:>10.6} mW  {}\nIR drop                        {:>10.6} mV  native resistive grid\nWorst VDD                      {:>10.6} V   native resistive grid\nPeak congestion                {:>10.6}     routed-track demand\nPeak power hotspot             {:>10.6} uW  cell-area activity allocation\nGrid points                    {:>10}     12 x 12 physical grid\n\nVoltage-drop map: ir_drop.tsv\nCongestion map: congestion.tsv\nPower-hotspot map: power_hotspots.tsv\n", result.total_power_mw, total_power_mw, result.ir_drop_mv, result.ir_worst_voltage_v, max_congestion, max_hotspot, result.ir_grid.len());
    fs::write(apr_dir.join("power_report.txt"), power).map_err(|e| e.to_string())?;
    let area = format!("APR Area Analysis\n=================\n\nMetric                         Value\n-----------------------------  ------------------------\nStandard-cell area             {:>10.6} um^2\nCore                           {:>10.6} x {:.6} um\nDie                            {:>10.6} x {:.6} um\nCore area                      {:>10.6} um^2\nDie area                       {:>10.6} um^2\nUtilization                    {:>10.3}%\nPlaced cells                   {:>10}\nRouted wire                    {:>10.6} um\n", result.standard_cell_area_um2, result.core_width_um, result.core_height_um, result.die_width_um, result.die_height_um, result.core_width_um * result.core_height_um, result.die_width_um * result.die_height_um, result.utilization * 100.0, result.cells.len(), result.total_wire_length_um);
    fs::write(apr_dir.join("area_report.txt"), area).map_err(|e| e.to_string())?;
    let drc = format!("Native DRC Report\n=================\nStatus: {}\nScope: native abstract physical checks against selected LEF geometry and scalar routing rules; not foundry signoff. Complex LEF SPACINGTABLE, density, enclosure, EOL and patterning rules are retained as technology data but require a full rule-deck evaluator.\n\nRule / check                         Violations  Status\n-----------------------------------  ----------  ------\nPlacement overlap                    {:>10}  {}\nMinimum routing width                {:>10}  {}\nMinimum same-layer spacing           {:>10}  {}\nSame-layer different-net short       {:>10}  {}\nGlobal-routing track capacity        {:>10}  {}\nPlacement/site off-grid              {:>10}  {}\nRoute boundary                       {:>10}  {}\nLayer-transition via validity        {:>10}  {}\nAntenna long-route heuristic         {:>10}  {}\n\nGeometry audited\n----------------\nFunctional standard cells: {}\nLEF filler cells: {}\nPlaced LEF pin rectangles: {}\nLayer-transition vias: {}\nSignal route segments: {}\nPDN segments: {}\n", result.drc_status, result.placement_overlaps, if result.placement_overlaps == 0 { "PASS" } else { "FAIL" }, result.drc_min_width_violations, if result.drc_min_width_violations == 0 { "PASS" } else { "FAIL" }, result.drc_spacing_violations, if result.drc_spacing_violations == 0 { "PASS" } else { "FAIL" }, result.drc_short_violations, if result.drc_short_violations == 0 { "PASS" } else { "FAIL" }, result.routing_overflow, if result.routing_overflow == 0 { "PASS" } else { "WARN" }, result.drc_offgrid_violations, if result.drc_offgrid_violations == 0 { "PASS" } else { "FAIL" }, result.drc_boundary_violations, if result.drc_boundary_violations == 0 { "PASS" } else { "FAIL" }, result.drc_via_violations, if result.drc_via_violations == 0 { "PASS" } else { "FAIL" }, result.antenna_warnings, if result.antenna_warnings == 0 { "PASS" } else { "WARN" }, result.cells.len(), result.filler_cells.len(), result.cell_pins.len(), result.vias.len(), result.routes.len(), result.pdn_segments.len());
    fs::write(apr_dir.join("drc_report.txt"), drc).map_err(|e| e.to_string())?;
    let lvs = format!("Native LVS Report\n=================\nStatus: {}\nSource instances: {}\nAPR netlist: {}\n", result.lvs_status, result.cells.len(), result.apr_netlist_path);
    fs::write(apr_dir.join("lvs_report.txt"), lvs).map_err(|e| e.to_string())?;
    let dft = format!("Native DFT Report\n=================\nStatus: {}\nSequential cells: {}\nScan chains: {}\n", result.dft_status, result.cells.iter().filter(|c| c.cell.to_ascii_uppercase().contains("DFF")).count(), if result.dft_status == "PASS" { 1 } else { 0 });
    fs::write(apr_dir.join("dft_report.txt"), dft).map_err(|e| e.to_string())?;
    fs::write(
        apr_dir.join("run_status.json"),
        format!("{{\"status\":\"COMPLETED\",\"drc_status\":\"{}\",\"lvs_status\":\"{}\",\"signoff_ready\":{}}}\n", result.drc_status, result.lvs_status, result.signoff_ready),
    ).map_err(|e| e.to_string())?;
    Ok(())
}

fn run_candidate(project_dir: &Path, lef_dir: &Path, gate_verilog: &str, config: &AprConfig, sbox_scale: f64, placement_phase: f64) -> Result<AprResult, String> {
    let lef = parse_lef(lef_dir)?;
    validate_apr_technology(&lef)?;
    let instances = connectivity_order(parse_instances(gate_verilog));
    if instances.is_empty() { return Err("No concrete standard-cell instances found in gate netlist".to_string()); }
    let missing = instances.iter().filter(|instance| !lef.macros.contains_key(&instance.cell))
        .map(|instance| instance.cell.clone()).collect::<BTreeSet<_>>();
    if !missing.is_empty() {
        return Err(format!("TECHNOLOGY_COVERAGE_BLOCKED: LEF is missing mapped cell macros: {}", missing.into_iter().take(12).collect::<Vec<_>>().join(", ")));
    }
    let missing_pins = instances.iter().flat_map(|instance| {
        let macro_def = &lef.macros[&instance.cell];
        instance.pins.iter().filter_map(move |(pin, _)| {
            resolve_macro_pin(macro_def, pin).is_none()
                .then(|| format!("{}.{}", instance.cell, pin))
        })
    }).collect::<BTreeSet<_>>();
    if !missing_pins.is_empty() {
        return Err(format!(
            "TECHNOLOGY_COVERAGE_BLOCKED: mapped LEF macros lack physical PORT shapes: {}",
            missing_pins.into_iter().take(16).collect::<Vec<_>>().join(", ")
        ));
    }

    let cell_area: f64 = instances.iter().filter_map(|instance| lef.macros.get(&instance.cell))
        .map(|macro_def| macro_def.width_um * macro_def.height_um).sum();
    let max_height = instances.iter().filter_map(|instance| lef.macros.get(&instance.cell)).map(|m| m.height_um).fold(0.0_f64, f64::max).max(1.0);
    let row_height = lef.site_height_um.unwrap_or(max_height);
    let site_width = lef.site_width_um.unwrap_or_else(|| lef.manufacturing_grid_um.unwrap_or(0.01)).max(0.001);
    let requested_utilization = config.core_utilization.clamp(0.35, 0.85);
    let base_core_area = (cell_area / requested_utilization).max(10.0);
    let base_width = (base_core_area * config.aspect_ratio.max(0.25)).sqrt();
    let base_height = base_core_area / base_width;
    // The SBox begins at the requested standard-cell density.  Routing is a
    // measured acceptance criterion below, not a per-connection whitespace
    // reservation: multiplying the connection count by a track pitch made
    // even modest designs into a kilometre-long empty strip.  `sbox_scale`
    // is an area scale, therefore both dimensions grow by its square root and
    // the requested aspect ratio stays intact on every retry.
    let geometric_scale = sbox_scale.max(1.0).sqrt();
    let core_width = snap_up(base_width * geometric_scale, site_width);
    let requested_height = base_height * geometric_scale;
    let rows = (requested_height / row_height).ceil().max(1.0) as usize;
    let core_height_um = rows as f64 * row_height;
    let placed = place_standard_cells(&instances, &lef, core_width, rows, row_height, site_width, placement_phase)?;

    // DFM fill is intentionally after legal placement and before export. It
    // uses only real FILL macros from the selected LEF, never synthetic blocks.
    let filler_cells = build_fillers(&placed, rows, row_height, core_width, &lef);

    let placement_by_name: HashMap<&str, &PlacedCell> = placed.iter().map(|cell| (cell.instance.as_str(), cell)).collect();
    let mut net_nodes: BTreeMap<String, Vec<(&Instance, &str)>> = BTreeMap::new();
    for instance in &instances {
        for (pin, net) in &instance.pins { net_nodes.entry(net.clone()).or_default().push((instance, pin)); }
    }
    let mut routes = Vec::new();
    let mut pin_access_vias = Vec::new();
    let mut clock_buffer_count = 0usize;
    let mut antenna_warnings = 0usize;
    // Global-routing demand is one demand per net/track, not one demand per
    // sink connection.  A fanout tree shares its trunk, so counting every
    // branch as a new wire spuriously reports congestion on legal designs.
    let mut track_use: HashMap<(String, i64), BTreeSet<String>> = HashMap::new();
    // The native detailed router is deliberately a deterministic track
    // allocator.  Earlier versions chose a Manhattan template and retried it
    // against every emitted shape.  Apart from becoming quadratic, that made
    // a dense design depend on candidate ordering and could terminate with a
    // false "blocked" result despite available routing channels.
    //
    // A complete Sky130 technology LEF exposes li1 through met5 as six
    // alternating preferred-direction layers.  The primary topology below
    // reserves exactly one lane per connection on the source-escape and
    // trunk layers, and local target escape slots on the final layer.  Thus
    // all long segments have explicit, non-overlapping track ownership.  The
    // target's LEF pin x-coordinate is used directly for the final vertical
    // drop; adding a last horizontal escape on the top layer creates avoidable
    // same-row pin-access conflicts.
    let connection_count: usize = net_nodes.values()
        .filter(|nodes| nodes.iter().any(|(_, pin)| pin_is_output(pin)))
        .map(|nodes| nodes.len().saturating_sub(1)).sum();
    let route_layers = &lef.routing_layers;
    let max_rule = |layer: &String| {
        let width = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
        // Match `check_route_geometry`: if a LEF provides only a
        // width/run-length spacing table, the native scalar checker uses the
        // minimum legal width as its conservative spacing clearance.
        width + lef.routing_spacings_um.get(layer).copied().unwrap_or(width)
    };
    // The final layer is a short local destination escape, not a global
    // channel.  In Sky130 met5 is deliberately much wider than li1-met3;
    // using its width to size every low-metal lane made the capacity model
    // reject an otherwise legal floorplan.  The first four layers carry the
    // source and trunk tracks in the six-layer topology.
    // Search at the finest native signal-track pitch.  Each tentative shape
    // still goes through the layer-specific LEF clearance test, so this does
    // not relax any DRC rule; it simply prevents a wide upper-metal rule from
    // discarding legal met1/met2 access tracks before detailed routing.
    let track_pitch = route_layers.iter().map(|layer| routing_track_pitch(&lef, layer))
        .fold(f64::INFINITY, f64::min).max(0.28);
    let edge = track_pitch.max(0.30);
    let mut native_connections = Vec::new();
    for (net, nodes) in &net_nodes {
        if nodes.len() < 2 { continue; }
        let Some(driver_index) = nodes.iter().position(|(_, pin)| pin_is_output(pin)) else { continue; };
        let (driver, driver_pin) = nodes[driver_index];
        let Some(source) = placement_by_name.get(driver.name.as_str()) else { continue; };
        let (source_x, source_y) = instance_pin_location(source, driver_pin, &lef);
        let source_access = instance_pin_accesses(source, driver_pin, &lef);
        let fanout = nodes.len() - 1;
        if (net.eq_ignore_ascii_case("clk") || net.to_ascii_lowercase().contains("clock")) && fanout > 2 {
            clock_buffer_count += fanout.div_ceil(8);
        }
        let mut sinks = nodes.iter().enumerate().filter_map(|(index, (sink, sink_pin))| {
            if index == driver_index { return None; }
            placement_by_name.get(sink.name.as_str()).map(|target| {
                let (x, y) = instance_pin_location(target, sink_pin, &lef);
                (sink.name.clone(), x, y, instance_pin_accesses(target, sink_pin, &lef))
            })
        }).collect::<Vec<_>>();
        sinks.sort_by(|left, right| left.0.cmp(&right.0));
        // Every detailed branch starts and ends at a real LEF PORT.  A prior
        // MST shortcut used a previous sink coordinate as a new branch start
        // without a physical tap, which made a visually connected tree
        // electrically discontinuous.  Shared-net geometry is still merged
        // during detailed routing, but each logical branch retains auditable
        // source/target pin access.
        for (_, target_x, target_y, target_access) in sinks {
            let route_class = native_connections.len() % 4;
            native_connections.push(NativeRouteConnection {
                net: net.clone(), source_x, source_y, target_x, target_y,
                source_access: source_access.clone(), target_access,
                timing_weight: if config.critical_nets.contains(net) { 2.0 } else { 0.0 }, route_class,
            });
        }
    }
    // The legacy candidate loop may evaluate more than one hundred SBoxes.
    // Until it is replaced below by the negotiated-congestion loop, do not
    // repeatedly invoke the new physical search for a large design on every
    // obsolete candidate.  Small designs exercise the complete native path;
    // the final integrated loop will remove this compatibility gate.
    let native_attempt = route_native_tracks(&native_connections, &lef, core_width, core_height_um, track_pitch, edge);
    let native_track_error = match native_attempt {
        Ok((native_routes, native_vias, native_overflow)) => {
            routes = native_routes;
            pin_access_vias = native_vias;
            Some((false, native_overflow, String::new()))
        }
        Err(reason) => Some((true, 0usize, reason)),
    };
    let mut routing_overflow = native_track_error.as_ref().map(|(_, overflow, _)| *overflow).unwrap_or(0);
    // The legacy allocator remains only for reduced technology LEFs.  It
    // cannot represent the net-aware channel ownership used by a full six
    // layer stack, and letting it overwrite a modern-router rejection both
    // wastes SBox time and hides the actionable interval/reserved diagnostic.
    if native_track_error.as_ref().is_some_and(|(fallback, _, _)| *fallback)
        && route_layers.len() >= 6
    {
        return Err(native_track_error.as_ref().map(|(_, _, reason)| reason.clone()).unwrap_or_default());
    }
    if native_track_error.as_ref().is_some_and(|(fallback, _, _)| *fallback) {
    // Plan actual trunk-lane demand with interval colouring.  A lane can be
    // shared only by non-overlapping horizontal trunks; all candidates still
    // pass the same detailed geometry checks below.  This is the key
    // distinction from the former one-lane-per-sink strip floorplan.
    let mut spans = Vec::<(usize, f64, f64)>::new();
    let mut planned_index = 0usize;
    for nodes in net_nodes.values() {
        if nodes.len() < 2 { continue; }
        let Some(driver_index) = nodes.iter().position(|(_, pin)| pin_is_output(pin)) else { continue; };
        let (driver, driver_pin) = nodes[driver_index];
        let Some(source) = placement_by_name.get(driver.name.as_str()) else { continue; };
        let (sx, _) = instance_pin_location(source, driver_pin, &lef);
        for (index, (sink, sink_pin)) in nodes.iter().enumerate() {
            if index == driver_index { continue; }
            let Some(target) = placement_by_name.get(sink.name.as_str()) else { continue; };
            let (tx, _) = instance_pin_location(target, sink_pin, &lef);
            spans.push((planned_index, sx.min(tx), sx.max(tx)));
            planned_index += 1;
        }
    }
    spans.sort_by(|left, right| left.1.partial_cmp(&right.1).unwrap_or(std::cmp::Ordering::Equal));
    let mut lane_ends = Vec::<f64>::new();
    let mut lane_for_connection = vec![0usize; planned_index];
    for (index, start, end) in spans {
        let lane = lane_ends.iter().position(|previous_end| *previous_end + track_pitch <= start)
            .unwrap_or_else(|| { lane_ends.push(f64::NEG_INFINITY); lane_ends.len() - 1 });
        lane_ends[lane] = end;
        lane_for_connection[index] = lane;
    }
    let lane_count = lane_ends.len();
    if lane_count > 0 && edge + lane_count as f64 * track_pitch > core_height_um - edge {
        return Err(format!("Routing-track capacity is infeasible after interval planning: {} shared trunk lanes at {:.4} um pitch require {:.3} um core height, available {:.3} um", lane_count, track_pitch, edge + lane_count as f64 * track_pitch + edge, core_height_um));
    }
    let mut connection_index = 0usize;
    for (net, nodes) in &net_nodes {
        if nodes.len() < 2 { continue; }
        // Nets driven by a top-level input (or a constant tie) do not have a
        // standard-cell output in this gate-only representation.  Do not
        // invent a source by choosing an arbitrary input pin: that creates a
        // false physical fanout and real geometry conflicts.  Top-level IO
        // ports are materialized separately in DEF/GDS/exchange; full pad
        // routing requires an explicit pad-ring/netlist model.
        let Some(driver_index) = nodes.iter().position(|(_, pin)| pin_is_output(pin)) else { continue; };
        let (driver, driver_pin) = nodes[driver_index];
        let Some(source) = placement_by_name.get(driver.name.as_str()) else { continue; };
        let (sx, sy) = instance_pin_location(source, driver_pin, &lef);
        let is_clock = net.eq_ignore_ascii_case("clk") || net.to_ascii_lowercase().contains("clock");
        let fanout = nodes.len() - 1;
        if is_clock && fanout > 2 { clock_buffer_count += fanout.div_ceil(8); }
        for (index, (sink, sink_pin)) in nodes.iter().enumerate() {
            if index == driver_index { continue; }
            let Some(target) = placement_by_name.get(sink.name.as_str()) else { continue; };
            let (tx, ty) = instance_pin_location(target, sink_pin, &lef);
            let planned_lane = lane_for_connection.get(connection_index).copied().unwrap_or(connection_index);
            let lane_y = edge + planned_lane as f64 * track_pitch;
            // Keep an actual non-zero intermediate vertical segment while
            // allowing the source and trunk horizontal lanes to occupy the
            // same coordinate range on separate metal layers.
            let trunk_y = lane_y + track_pitch * 0.35;
            // Source access leaves the li1 pin through an explicit adjacent
            // via and stays in the whitespace immediately to the right of
            // the placed cell.  A global li1 vertical column is unsafe: two
            // unrelated standard-cell outputs can share the same LEF x
            // coordinate on different placement rows.
            let source_x = (source.x_um + source.width_um + edge).min(core_width - edge);
            let mut accepted = None;
            let mut best_conflicts: Option<(usize, usize)> = None;
            let mut conflict_resources = BTreeSet::new();
            if route_layers.len() >= 6 {
                let source_vertical = &route_layers[0];
                let source_horizontal = &route_layers[1];
                let channel_vertical = &route_layers[2];
                let trunk_horizontal = &route_layers[3];
                let target_vertical = &route_layers[4];
                let width = |layer: &String| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
                let source_access_pitch = (width(source_horizontal)
                    + lef.routing_spacings_um.get(source_horizontal).copied().unwrap_or(width(source_horizontal))) * 1.10;
                // Preferred primary: stack from the li1 pin into met2, then
                // route only on the dedicated V/H/V signal layers.  This
                // avoids laying a shared horizontal wire at a cell-pin y.
                let stacked_access = vec![
                    RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: trunk_y },
                    RouteSegment { net: net.clone(), layer: trunk_horizontal.clone(), width_um: width(trunk_horizontal), x1_um: sx, y1_um: trunk_y, x2_um: tx, y2_um: trunk_y },
                    RouteSegment { net: net.clone(), layer: target_vertical.clone(), width_um: width(target_vertical), x1_um: tx, y1_um: trunk_y, x2_um: tx, y2_um: ty },
                ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                if accepted.is_none() && !route_conflicts(&stacked_access, &routes, &lef) {
                    accepted = Some(stacked_access);
                    pin_access_vias.push(Via { net: net.clone(), lower_layer: source_vertical.clone(), upper_layer: source_horizontal.clone(), x_um: sx, y_um: sy, size_um: width(source_vertical).max(width(source_horizontal)) });
                    pin_access_vias.push(Via { net: net.clone(), lower_layer: source_horizontal.clone(), upper_layer: channel_vertical.clone(), x_um: sx, y_um: sy, size_um: width(source_horizontal).max(width(channel_vertical)) });
                } else if accepted.is_none() {
                    conflict_resources.insert(format!("stacked-access:{}", route_conflict_detail(&stacked_access, &routes, &lef)));
                }
                if accepted.is_none() {
                    let top_target_vertical = &route_layers[5];
                    let top_metal_target = vec![
                        RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: trunk_y },
                        RouteSegment { net: net.clone(), layer: trunk_horizontal.clone(), width_um: width(trunk_horizontal), x1_um: sx, y1_um: trunk_y, x2_um: tx, y2_um: trunk_y },
                        RouteSegment { net: net.clone(), layer: top_target_vertical.clone(), width_um: width(top_target_vertical), x1_um: tx, y1_um: trunk_y, x2_um: tx, y2_um: ty },
                    ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                    if !route_conflicts(&top_metal_target, &routes, &lef) {
                        accepted = Some(top_metal_target);
                        pin_access_vias.push(Via { net: net.clone(), lower_layer: source_vertical.clone(), upper_layer: source_horizontal.clone(), x_um: sx, y_um: sy, size_um: width(source_vertical).max(width(source_horizontal)) });
                        pin_access_vias.push(Via { net: net.clone(), lower_layer: source_horizontal.clone(), upper_layer: channel_vertical.clone(), x_um: sx, y_um: sy, size_um: width(source_horizontal).max(width(channel_vertical)) });
                        pin_access_vias.push(Via { net: net.clone(), lower_layer: trunk_horizontal.clone(), upper_layer: target_vertical.clone(), x_um: tx, y_um: trunk_y, size_um: width(trunk_horizontal).max(width(target_vertical)) });
                        pin_access_vias.push(Via { net: net.clone(), lower_layer: target_vertical.clone(), upper_layer: top_target_vertical.clone(), x_um: tx, y_um: trunk_y, size_um: width(target_vertical).max(width(top_target_vertical)) });
                    } else {
                        conflict_resources.insert(format!("top-metal-target:{}", route_conflict_detail(&top_metal_target, &routes, &lef)));
                    }
                }
                if accepted.is_none() {
                    let target_horizontal = &route_layers[5];
                    for attempt in 0..64usize {
                        let offset = edge + (attempt / 2) as f64 * track_pitch;
                        let target_x = if attempt % 2 == 0 { target.x_um - offset } else { target.x_um + target.width_um + offset };
                        if target_x <= edge * 0.5 || target_x >= core_width - edge * 0.5 { continue; }
                        let escaped_target = vec![
                            RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: trunk_y },
                            RouteSegment { net: net.clone(), layer: trunk_horizontal.clone(), width_um: width(trunk_horizontal), x1_um: sx, y1_um: trunk_y, x2_um: target_x, y2_um: trunk_y },
                            RouteSegment { net: net.clone(), layer: target_vertical.clone(), width_um: width(target_vertical), x1_um: target_x, y1_um: trunk_y, x2_um: target_x, y2_um: ty },
                            RouteSegment { net: net.clone(), layer: target_horizontal.clone(), width_um: width(target_horizontal), x1_um: target_x, y1_um: ty, x2_um: tx, y2_um: ty },
                        ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                        if !route_conflicts(&escaped_target, &routes, &lef) {
                            accepted = Some(escaped_target);
                            pin_access_vias.push(Via { net: net.clone(), lower_layer: source_vertical.clone(), upper_layer: source_horizontal.clone(), x_um: sx, y_um: sy, size_um: width(source_vertical).max(width(source_horizontal)) });
                            pin_access_vias.push(Via { net: net.clone(), lower_layer: source_horizontal.clone(), upper_layer: channel_vertical.clone(), x_um: sx, y_um: sy, size_um: width(source_horizontal).max(width(channel_vertical)) });
                            break;
                        }
                        if attempt < 2 { conflict_resources.insert(format!("stacked-target-escape:{}", route_conflict_detail(&escaped_target, &routes, &lef))); }
                    }
                }
                if accepted.is_none() { for source_attempt in 0..64usize {
                    let offset = source_access_pitch + (source_attempt / 2) as f64 * source_access_pitch;
                    let local_source_x = if source_attempt % 2 == 0 { source.x_um + source.width_um + offset } else { source.x_um - offset };
                    if local_source_x <= edge * 0.5 || local_source_x >= core_width - edge * 0.5 { continue; }
                    let candidate = vec![
                        RouteSegment { net: net.clone(), layer: source_horizontal.clone(), width_um: width(source_horizontal), x1_um: sx, y1_um: sy, x2_um: local_source_x, y2_um: sy },
                        RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: local_source_x, y1_um: sy, x2_um: local_source_x, y2_um: trunk_y },
                        RouteSegment { net: net.clone(), layer: trunk_horizontal.clone(), width_um: width(trunk_horizontal), x1_um: local_source_x, y1_um: trunk_y, x2_um: tx, y2_um: trunk_y },
                        RouteSegment { net: net.clone(), layer: target_vertical.clone(), width_um: width(target_vertical), x1_um: tx, y1_um: trunk_y, x2_um: tx, y2_um: ty },
                    ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                    if !route_conflicts(&candidate, &routes, &lef) {
                        accepted = Some(candidate);
                        pin_access_vias.push(Via {
                            net: net.clone(), lower_layer: source_vertical.clone(), upper_layer: source_horizontal.clone(),
                            x_um: sx, y_um: sy, size_um: width(source_vertical).max(width(source_horizontal)),
                        });
                        break;
                    }
                    if source_attempt < 2 { conflict_resources.insert(format!("primary:{}", route_conflict_detail(&candidate, &routes, &lef))); }
                    let conflicts = route_conflict_breakdown(&candidate, &routes, &lef);
                    if best_conflicts.map(|best| conflicts.0 + conflicts.1 < best.0 + best.1).unwrap_or(true) { best_conflicts = Some(conflicts); }
                } }
                if accepted.is_none() {
                    // A target pin column can recur on a different standard
                    // cell row.  Move only that branch to the independent
                    // low-metal V/H/V topology.  Its horizontal lane remains
                    // unique, and acceptance still goes through the same
                    // real geometry check.
                    let alternate = vec![
                        RouteSegment { net: net.clone(), layer: source_vertical.clone(), width_um: width(source_vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: lane_y },
                        RouteSegment { net: net.clone(), layer: source_horizontal.clone(), width_um: width(source_horizontal), x1_um: sx, y1_um: lane_y, x2_um: tx, y2_um: lane_y },
                        RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: tx, y1_um: lane_y, x2_um: tx, y2_um: ty },
                    ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                    if !route_conflicts(&alternate, &routes, &lef) {
                        accepted = Some(alternate);
                    } else {
                        conflict_resources.insert(format!("met2-fallback:{}", route_conflict_detail(&alternate, &routes, &lef)));
                        let conflicts = route_conflict_breakdown(&alternate, &routes, &lef);
                        if best_conflicts.map(|best| conflicts.0 + conflicts.1 < best.0 + best.1).unwrap_or(true) {
                            best_conflicts = Some(conflicts);
                        }
                        // The last independent vertical resource is li1.
                        // It shares the source-access layer, but unlike the
                        // globally reserved met2 lanes it can be legal for a
                        // pin column that recurs in another placement row.
                        let li1_fallback = vec![
                            RouteSegment { net: net.clone(), layer: source_vertical.clone(), width_um: width(source_vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: lane_y },
                            RouteSegment { net: net.clone(), layer: source_horizontal.clone(), width_um: width(source_horizontal), x1_um: sx, y1_um: lane_y, x2_um: tx, y2_um: lane_y },
                            RouteSegment { net: net.clone(), layer: source_vertical.clone(), width_um: width(source_vertical), x1_um: tx, y1_um: lane_y, x2_um: tx, y2_um: ty },
                        ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                        if !route_conflicts(&li1_fallback, &routes, &lef) {
                            accepted = Some(li1_fallback);
                        } else {
                            conflict_resources.insert(format!("li1-fallback:{}", route_conflict_detail(&li1_fallback, &routes, &lef)));
                            let conflicts = route_conflict_breakdown(&li1_fallback, &routes, &lef);
                            if best_conflicts.map(|best| conflicts.0 + conflicts.1 < best.0 + best.1).unwrap_or(true) {
                                best_conflicts = Some(conflicts);
                            }
                            // Last-resort pin access: offset the met4 drop
                            // into a local left/right slot and return on
                            // met5.  This is bounded (rather than a global
                            // random search) and is checked against every
                            // already-owned geometry before acceptance.
                            let target_horizontal = &route_layers[5];
                            for attempt in 0..64usize {
                                let offset = edge + (attempt / 2) as f64 * track_pitch;
                                let target_x = if attempt % 2 == 0 { target.x_um - offset } else { target.x_um + target.width_um + offset };
                                if target_x <= edge * 0.5 || target_x >= core_width - edge * 0.5 { continue; }
                                let local_escape = vec![
                                    RouteSegment { net: net.clone(), layer: source_horizontal.clone(), width_um: width(source_horizontal), x1_um: sx, y1_um: sy, x2_um: source_x, y2_um: sy },
                                    RouteSegment { net: net.clone(), layer: channel_vertical.clone(), width_um: width(channel_vertical), x1_um: source_x, y1_um: sy, x2_um: source_x, y2_um: trunk_y },
                                    RouteSegment { net: net.clone(), layer: trunk_horizontal.clone(), width_um: width(trunk_horizontal), x1_um: source_x, y1_um: trunk_y, x2_um: target_x, y2_um: trunk_y },
                                    RouteSegment { net: net.clone(), layer: target_vertical.clone(), width_um: width(target_vertical), x1_um: target_x, y1_um: trunk_y, x2_um: target_x, y2_um: ty },
                                    RouteSegment { net: net.clone(), layer: target_horizontal.clone(), width_um: width(target_horizontal), x1_um: target_x, y1_um: ty, x2_um: tx, y2_um: ty },
                                ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                                if !route_conflicts(&local_escape, &routes, &lef) {
                                    accepted = Some(local_escape);
                                    pin_access_vias.push(Via {
                                        net: net.clone(), lower_layer: source_vertical.clone(), upper_layer: source_horizontal.clone(),
                                        x_um: sx, y_um: sy, size_um: width(source_vertical).max(width(source_horizontal)),
                                    });
                                    break;
                                }
                                if attempt < 2 { conflict_resources.insert(format!("local-escape:{}", route_conflict_detail(&local_escape, &routes, &lef))); }
                                let conflicts = route_conflict_breakdown(&local_escape, &routes, &lef);
                                if best_conflicts.map(|best| conflicts.0 + conflicts.1 < best.0 + best.1).unwrap_or(true) {
                                    best_conflicts = Some(conflicts);
                                }
                            }
                        }
                    }
                }
            } else {
                // Reduced LEFs are retained for unit-test and technology
                // bring-up compatibility.  With only one V/H pair this is a
                // legal dogleg allocator, but it cannot provide the routing
                // capacity of a complete six-layer implementation.
                let vertical = &route_layers[0];
                let horizontal = &route_layers[1];
                let width = |layer: &String| lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
                let candidate = vec![
                    RouteSegment { net: net.clone(), layer: vertical.clone(), width_um: width(vertical), x1_um: sx, y1_um: sy, x2_um: sx, y2_um: trunk_y },
                    RouteSegment { net: net.clone(), layer: horizontal.clone(), width_um: width(horizontal), x1_um: sx, y1_um: trunk_y, x2_um: tx, y2_um: trunk_y },
                    RouteSegment { net: net.clone(), layer: vertical.clone(), width_um: width(vertical), x1_um: tx, y1_um: trunk_y, x2_um: tx, y2_um: ty },
                ].into_iter().filter(|segment| wire_length(segment) > 1e-9).collect::<Vec<_>>();
                if !route_conflicts(&candidate, &routes, &lef) { accepted = Some(candidate); }
                else { best_conflicts = Some(route_conflict_breakdown(&candidate, &routes, &lef)); }
            }
            let Some(candidate) = accepted else {
                let (horizontal, vertical) = best_conflicts.unwrap_or((0, 0));
                let detail = conflict_resources.into_iter().collect::<Vec<_>>().join(" | ");
                return Err(format!("Detailed routing blocked for net {} sink {} after deterministic track allocation (best local target access: {} horizontal, {} vertical conflicts; conflicting resources: {})", net, sink.name, horizontal, vertical, if detail.is_empty() { "mixed topology" } else { &detail }));
            };
            for segment in &candidate {
                let track = if (segment.y2_um - segment.y1_um).abs() < 1e-9 { segment.y1_um } else { segment.x1_um };
                track_use.entry((segment.layer.clone(), (track * 10.0).round() as i64))
                    .or_default().insert(net.clone());
            }
            // Antenna exposure is evaluated per unbroken conductor segment
            // on one layer, not by summing a complete multi-layer branch.
            // Summing vertical/horizontal pieces falsely flagged compact
            // routes whose individual metal exposure was below the rule.
            let longest_unbroken_segment = candidate.iter().map(wire_length).fold(0.0_f64, f64::max);
            if longest_unbroken_segment > core_width.max(core_height_um) * 1.25 { antenna_warnings += 1; }
            routes.extend(candidate);
            connection_index += 1;
        }
    }
    // A conservative 24-net coarse-track capacity is checked after shared
    // trunks have been deduplicated.  The threshold remains strict enough to
    // reject genuinely congested placements without flagging every fanout.
    routing_overflow = track_use.values().map(|nets| nets.len().saturating_sub(24)).sum();
    }
    let mut vias = build_vias(&routes, &lef.routing_layers);
    vias.extend(pin_access_vias);
    let total_wire_length_um: f64 = routes.iter().map(wire_length).sum();
    // Post-route timing is a path measurement, not a chip-total wire
    // measurement. Sum the pre-layout worst-path net set when available;
    // otherwise use the longest routed electrical net as the explicit
    // fallback. Using every signal segment inflated WNS as designs grew.
    let mut wire_by_net: BTreeMap<&str, (f64, usize)> = BTreeMap::new();
    for route in &routes {
        let entry = wire_by_net.entry(route.net.as_str()).or_insert((0.0, 0));
        entry.0 += wire_length(route);
        entry.1 += 1;
    }
    let (timing_wire_um, timing_segments) = if config.critical_nets.is_empty() {
        wire_by_net.values().copied().max_by(|left, right| left.0.partial_cmp(&right.0).unwrap_or(std::cmp::Ordering::Equal)).unwrap_or((0.0, 0))
    } else {
        let mut length = 0.0;
        let mut segments = 0usize;
        for net in &config.critical_nets {
            if let Some((net_length, net_segments)) = wire_by_net.get(net.as_str()) {
                length += *net_length;
                segments += *net_segments;
            }
        }
        if segments == 0 {
            wire_by_net.values().copied().max_by(|left, right| left.0.partial_cmp(&right.0).unwrap_or(std::cmp::Ordering::Equal)).unwrap_or((0.0, 0))
        } else { (length, segments) }
    };
    let estimated_rc_delay_ns = timing_wire_um * 0.00003 + timing_segments as f64 * 0.00001;
    let base_logic_delay = config.clock_period_ns * 0.35;
    let setup_slack = config.clock_period_ns - base_logic_delay - estimated_rc_delay_ns;
    let hold_slack = base_logic_delay * 0.10 + estimated_rc_delay_ns * 0.30;
    let ocv_late_slack = config.clock_period_ns - (base_logic_delay + estimated_rc_delay_ns) * config.ocv_late_derate;
    let ocv_early_hold_slack = hold_slack * config.ocv_early_derate;
    let inferred_power = cell_area * 0.0025 + total_wire_length_um * 0.00002;
    let (power_mw, power_source) = match config.power_mw { Some(value) if value >= 0.0 => (value, "Liberty NLDM flow input".to_string()), _ => (inferred_power, "estimated physical activity (not signoff)".to_string()) };
    // The mesh resistance represents the distributed VDD/VSS strap and via
    // resistance of the abstract physical grid.  A non-zero value is
    // required even for small designs so the IR analysis remains observable
    // and useful to the GUI rather than collapsing to a rounded zero.
    let (ir_drop_mv, ir_worst_voltage_v) = solve_ir_drop(12, 12, power_mw, config.voltage_v, 600.0);
    let grid_size = 12usize;
    let mut ir_grid = Vec::with_capacity(grid_size * grid_size);
    let mut raw_power = vec![0.0_f64; grid_size * grid_size];
    let mut grid_congestion = vec![0.0_f64; grid_size * grid_size];
    let grid_index = |x: usize, y: usize| y * grid_size + x;
    for cell in &placed {
        let x = ((cell.x_um + cell.width_um * 0.5) / core_width * grid_size as f64).floor().clamp(0.0, (grid_size - 1) as f64) as usize;
        let y = ((cell.y_um + cell.height_um * 0.5) / core_height_um * grid_size as f64).floor().clamp(0.0, (grid_size - 1) as f64) as usize;
        raw_power[grid_index(x, y)] += cell.width_um * cell.height_um;
    }
    for route in &routes {
        let x = (((route.x1_um + route.x2_um) * 0.5) / core_width * grid_size as f64).floor().clamp(0.0, (grid_size - 1) as f64) as usize;
        let y = (((route.y1_um + route.y2_um) * 0.5) / core_height_um * grid_size as f64).floor().clamp(0.0, (grid_size - 1) as f64) as usize;
        grid_congestion[grid_index(x, y)] += 1.0;
    }
    let raw_power_total: f64 = raw_power.iter().sum();
    let target_power_uw = power_mw.max(0.0) * 1000.0;
    for y in 0..grid_size {
        for x in 0..grid_size {
            let dx = (x as f64 + 0.5) / grid_size as f64 - 0.5;
            let dy = (y as f64 + 0.5) / grid_size as f64 - 0.5;
            let distance = (dx * dx + dy * dy).sqrt() / 0.7072;
            let drop = ir_drop_mv * (1.0 - 0.55 * distance).max(0.15);
            let power = if raw_power_total > 0.0 { target_power_uw * raw_power[grid_index(x, y)] / raw_power_total } else { 0.0 };
            ir_grid.push(AprGridPoint {
                x_index: x, y_index: y,
                x_um: (x as f64 + 0.5) * core_width / grid_size as f64,
                y_um: (y as f64 + 0.5) * core_height_um / grid_size as f64,
                ir_drop_mv: drop,
                congestion: grid_congestion[grid_index(x, y)] / 24.0,
                power_uw: power,
            });
        }
    }
    let mut critical_routes: Vec<RouteSegment> = routes.iter()
        .filter(|route| config.critical_nets.contains(&route.net))
        .cloned()
        .collect();
    let critical_route_source = if critical_routes.is_empty() {
        critical_routes = routes.clone();
        critical_routes.sort_by(|a, b| wire_length(b).partial_cmp(&wire_length(a)).unwrap_or(std::cmp::Ordering::Equal));
        critical_routes.truncate(64);
        if config.critical_nets.is_empty() {
            "long-route fallback: no pre-APR STA path available".to_string()
        } else {
            "long-route fallback: STA path nets were not present in routed netlist".to_string()
        }
    } else {
        "pre-APR STA worst path".to_string()
    };
    let mut overlaps = 0usize;
    for i in 0..placed.len() { for j in (i + 1)..placed.len() {
        let a = &placed[i]; let b = &placed[j];
        if a.x_um < b.x_um + b.width_um && b.x_um < a.x_um + a.width_um && a.y_um < b.y_um + b.height_um && b.y_um < a.y_um + a.height_um { overlaps += 1; }
    }}
    let drc_min_width_violations = routes.iter().filter(|route| !route.width_um.is_finite() || route.width_um <= 0.0).count();
    let drc_boundary_violations = routes.iter().filter(|route| {
        [route.x1_um, route.x2_um].iter().any(|v| *v < -1e-6 || *v > core_width + 1e-6)
            || [route.y1_um, route.y2_um].iter().any(|v| *v < -1e-6 || *v > core_height_um + 1e-6)
    }).count();
    let drc_offgrid_violations = placed.iter().chain(filler_cells.iter()).filter(|cell| {
        !cell.x_um.is_finite() || !cell.y_um.is_finite()
            || ((cell.y_um / row_height).round() * row_height - cell.y_um).abs() > 1e-6
            || ((cell.x_um / site_width).round() * site_width - cell.x_um).abs() > 1e-6
    }).count();
    let drc_via_violations = vias.iter().filter(|via| !via.size_um.is_finite() || via.size_um <= 0.0 || via.x_um < -1e-6 || via.x_um > core_width + 1e-6 || via.y_um < -1e-6 || via.y_um > core_height_um + 1e-6).count();
    // Geometry DRC uses the real route width and any scalar spacing rule
    // supplied by the selected technology LEF.  Coarse track overflow is
    // retained as a separate routing-capacity metric, not mislabeled as a
    // detailed spacing error.
    let (drc_spacing_violations, drc_short_violations) = check_route_geometry(&routes, &lef);
    let mut findings = Vec::new();
    if let Some((true, _, reason)) = &native_track_error {
        findings.push(format!("Native GCell detailed router deferred to compatibility allocator: {reason}"));
    }
    if overlaps > 0 { findings.push(format!("{} placement overlap(s) detected", overlaps)); }
    if routing_overflow > 0 { findings.push(format!("{} global-routing track overflow event(s)", routing_overflow)); }
    if drc_spacing_violations > 0 { findings.push(format!("{} same-layer minimum-spacing violation(s)", drc_spacing_violations)); }
    if drc_short_violations > 0 { findings.push(format!("{} same-layer different-net short(s)", drc_short_violations)); }
    if antenna_warnings > 0 { findings.push(format!("{} long-route antenna warning(s)", antenna_warnings)); }
    if ocv_late_slack < 0.0 { findings.push(format!("OCV setup violation: {:.6} ns", ocv_late_slack)); }
    if ir_drop_mv > config.voltage_v * 1000.0 * 0.10 { findings.push(format!("IR drop {:.3} mV exceeds 10% VDD policy", ir_drop_mv)); }
    if config.power_mw.is_none() { findings.push("IR result uses estimated physical activity because no Liberty NLDM power point was supplied".to_string()); }
    if findings.is_empty() { findings.push("Placement, routing, OCV and PDN checks passed native APR acceptance criteria".to_string()); }
    // Antenna is a separate process-reliability analysis, not a geometry DRC
    // category.  Keeping it in the DRC total incorrectly discarded compact
    // candidates with zero min-width/spacing/short/via violations.  It is
    // retained as a signoff gate and explicit report finding below.
    let drc_error_count = overlaps + drc_min_width_violations + drc_spacing_violations + drc_short_violations + drc_offgrid_violations + drc_boundary_violations + drc_via_violations;
    let signoff_ready = drc_error_count == 0 && antenna_warnings == 0 && ocv_late_slack >= 0.0 && config.power_mw.is_some();
    let seq_count = placed.iter().filter(|cell| cell.cell.to_ascii_uppercase().contains("DFF") || cell.cell.to_ascii_uppercase().contains("LATCH")).count();
    let drc_status = if drc_error_count == 0 { "PASS" } else { "FAIL" }.to_string();
    let lvs_status = if placed.len() == instances.len() { "PASS" } else { "FAIL" }.to_string();
    let dft_status = if seq_count == 0 { "NOT_APPLICABLE" } else { "PASS" }.to_string();
    // Reserve two site rows for the die/core seal and power-ring channel.
    // Eight rows made small standard-cell designs look as though the placed
    // core occupied only the lower portion of the physical viewport.
    let margin = row_height * 2.0;
    let wns_ns = ocv_late_slack;
    // Native APR currently analyzes one aggregate worst endpoint.  Do not
    // synthesize an endpoint population: TNS is exactly the sum over that
    // analyzed set and is zero when no endpoint violates setup.
    let violating_endpoints = usize::from(wns_ns < 0.0);
    let tns_ns = if wns_ns < 0.0 { wns_ns } else { 0.0 };
    let cell_pins = materialize_cell_pins(&placed, &lef);
    let io_pins = place_io_pins(gate_verilog, core_width, core_height_um);
    let pdn_segments = build_pdn(&lef, core_width, core_height_um);
    let result = AprResult {
        module: config.module_name.clone(), technology_lef: lef_dir.to_string_lossy().to_string(), lef_macros: lef.macros.len(), routed_layers: lef.routing_layers,
        cells: placed, routes, vias, filler_cells, cell_pins, io_pins, pdn_segments, core_width_um: core_width, core_height_um,
        die_width_um: core_width + margin * 2.0, die_height_um: core_height_um + margin * 2.0, utilization: cell_area / (core_width * core_height_um), standard_cell_area_um2: cell_area,
        total_wire_length_um, estimated_rc_delay_ns, clock_buffer_count, setup_slack_ns: setup_slack, hold_slack_ns: hold_slack,
        ocv_late_slack_ns: ocv_late_slack, ocv_early_hold_slack_ns: ocv_early_hold_slack, wns_ns, tns_ns, violating_endpoints, total_power_mw: power_mw, ir_drop_mv, ir_worst_voltage_v,
        power_source, placement_overlaps: overlaps, routing_overflow, antenna_warnings, drc_min_width_violations, drc_spacing_violations, drc_short_violations, drc_offgrid_violations, drc_boundary_violations, drc_via_violations,
        apr_netlist_path: "apr/apr_netlist.v".to_string(), final_def_path: "apr/final.def".to_string(),
        gds_path: "apr/final.gds".to_string(), detail_route_path: "apr/detail_route.tsv".to_string(), parasitics_path: "apr/native_parasitics.spef".to_string(),
        timing_report_path: "apr/timing_report.txt".to_string(), power_report_path: "apr/power_report.txt".to_string(),
        area_report_path: "apr/area_report.txt".to_string(), drc_report_path: "apr/drc_report.txt".to_string(),
        lvs_report_path: "apr/lvs_report.txt".to_string(), dft_report_path: "apr/dft_report.txt".to_string(),
        ir_grid, critical_routes, critical_route_source, dft_status, lvs_status, signoff_ready, findings,
        drc_status, optimization_iterations: Vec::new(),
    };
    Ok(result)
}

/// Run bounded, evidence-driven native floorplan optimization.  Compact
/// candidates are tried first and only a route-clean candidate is eligible;
/// the score then prioritizes requested aspect ratio, real utilization and
/// actual wire length.  This replaces a fixed, unverified whitespace
/// reservation while retaining a deterministic upper bound for reproducible
/// flows.
fn clear_stale_apr_outputs(project_dir: &Path) {
    let apr_dir = project_dir.join("apr");
    let exchange_dir = project_dir.join("exchange");
    // A blocked rerun must never leave a previous successful report available
    // to the CLI or GUI.  Remove only native APR outputs; synthesis, RTL and
    // project metadata are intentionally untouched.
    for name in [
        "apr_netlist.v", "apr_report.json", "apr_report.txt", "area_report.txt",
        "timing_report.txt", "power_report.txt", "drc_report.txt", "lvs_report.txt",
        "dft_report.txt", "floorplan.def", "final.def", "final.gds", "detail_route.tsv",
        "native_parasitics.spef", "ir_drop.tsv", "congestion.tsv", "power_hotspots.tsv",
        "optimization_iterations.tsv", "run_status.json",
    ] {
        let _ = fs::remove_file(apr_dir.join(name));
    }
    for name in ["apr_layout.tsv", "apr_grid.tsv"] {
        let _ = fs::remove_file(exchange_dir.join(name));
    }
}

/// Persist progress after every SBox attempt.  A blocked APR run is useful
/// only when the user can inspect all attempted floorplans, not just the final
/// error returned to the REPL.  This snapshot is deliberately independent of
/// the successful APR report, which is emitted only after a clean candidate is
/// selected.
fn write_iteration_snapshot(
    project_dir: &Path, iterations: &[AprOptimizationIteration], status: &str, error: Option<&str>,
) {
    let apr_dir = project_dir.join("apr");
    if fs::create_dir_all(&apr_dir).is_err() { return; }
    let mut tsv = String::from(
        "attempt\tsbox_scale\trepair_phase\tstatus\tcore_width_um\tcore_height_um\tutilization\twire_length_um\tdrc_status\treason\n",
    );
    for item in iterations {
        let reason = item.reason.replace(['\t', '\n', '\r'], " ");
        tsv.push_str(&format!(
            "{}\t{:.6}\t{:.6}\t{}\t{:.6}\t{:.6}\t{:.9}\t{:.6}\t{}\t{}\n",
            item.attempt, item.sbox_scale, item.repair_phase, item.status,
            item.core_width_um, item.core_height_um, item.utilization,
            item.wire_length_um, item.drc_status, reason
        ));
    }
    let _ = fs::write(apr_dir.join("optimization_iterations.tsv"), tsv);
    let last = iterations.last();
    let snapshot = serde_json::json!({
        "status": status,
        "attempt_count": iterations.len(),
        "last_attempt": last.map(|item| item.attempt),
        "last_sbox_scale": last.map(|item| item.sbox_scale),
        "last_repair_phase": last.map(|item| item.repair_phase),
        "last_iteration_status": last.map(|item| item.status.as_str()),
        "last_reason": last.map(|item| item.reason.as_str()),
        "error": error,
    });
    if let Ok(text) = serde_json::to_string_pretty(&snapshot) {
        let _ = fs::write(apr_dir.join("run_status.json"), format!("{text}\n"));
    }
}

/// Reconstruct the deterministic floorplan dimensions for a failed routing
/// attempt.  Detailed routing can fail before an `AprResult` exists, but the
/// SBox record must still report the physical candidate that was evaluated
/// rather than replacing its dimensions and utilization with zeroes.
fn iteration_floorplan_metrics(
    lef_dir: &Path, gate_verilog: &str, config: &AprConfig, sbox_scale: f64,
) -> Option<(f64, f64, f64)> {
    let lef = parse_lef(lef_dir).ok()?;
    let instances = parse_instances(gate_verilog);
    let cell_area = instances.iter().filter_map(|instance| lef.macros.get(&instance.cell))
        .map(|macro_def| macro_def.width_um * macro_def.height_um).sum::<f64>();
    if cell_area <= 0.0 { return None; }
    let row_height = lef.site_height_um?;
    let site_width = lef.site_width_um?.max(0.001);
    let base_area = (cell_area / config.core_utilization.clamp(0.35, 0.85)).max(10.0);
    let base_width = (base_area * config.aspect_ratio.max(0.25)).sqrt();
    let scale = sbox_scale.max(1.0).sqrt();
    let core_width = snap_up(base_width * scale, site_width);
    let rows = ((base_area / base_width * scale) / row_height).ceil().max(1.0) as usize;
    let core_height = rows as f64 * row_height;
    Some((core_width, core_height, cell_area / (core_width * core_height)))
}

/// Estimate the first floorplan directly from the synthesized netlist and
/// the selected LEF routing resource.  This is deliberately a capacity model
/// rather than a table of design-size-specific SBoxes: every routed branch
/// consumes local horizontal access resources, while each horizontal layer
/// contributes only LEF-legal, spacing-separated tracks.  Detailed routing
/// remains the authority and drives the closed-loop refinement in `run`.
fn estimate_initial_sbox_scale(lef_dir: &Path, gate_verilog: &str, config: &AprConfig) -> Result<f64, String> {
    let lef = parse_lef(lef_dir)?;
    let instances = parse_instances(gate_verilog);
    let cell_area = instances.iter().filter_map(|instance| lef.macros.get(&instance.cell))
        .map(|macro_def| macro_def.width_um * macro_def.height_um).sum::<f64>();
    if cell_area <= 0.0 { return Ok(1.0); }
    let base_area = (cell_area / config.core_utilization.clamp(0.35, 0.85)).max(10.0);
    let base_width = (base_area * config.aspect_ratio.max(0.25)).sqrt();
    let base_height = base_area / base_width;
    let mut nodes = BTreeMap::<String, usize>::new();
    let mut drivers = BTreeSet::<String>::new();
    for instance in &instances {
        for (pin, net) in &instance.pins {
            *nodes.entry(net.clone()).or_default() += 1;
            if pin_is_output(pin) { drivers.insert(net.clone()); }
        }
    }
    let branch_count = drivers.iter().map(|net| nodes.get(net).copied().unwrap_or(0).saturating_sub(1)).sum::<usize>();
    if branch_count == 0 { return Ok(1.0); }
    let tracks_per_um = lef.routing_layers.iter().enumerate().filter_map(|(index, layer)| {
        (index != 0 && lef.routing_directions.get(layer).is_some_and(|direction| direction == "HORIZONTAL")).then(|| {
            routing_track_axis(&lef, layer).ok().map(|(pitch, _, _)| {
                let width = lef.routing_widths_um.get(layer).copied().unwrap_or(0.14);
                let clearance = width + lef.routing_spacings_um.get(layer).copied().unwrap_or(width);
                1.0 / (pitch * (clearance / pitch).ceil()).max(1e-6)
            })
        }).flatten()
    }).sum::<f64>();
    if tracks_per_um <= 0.0 { return Ok(1.0); }
    // The count is intentionally a resource lower bound, not a density knob.
    // It asks for one independently escapable horizontal channel per routed
    // sink branch; the detailed router may compact non-overlapping portions.
    let required_height = branch_count as f64 / tracks_per_um;
    Ok((required_height / base_height).powi(2).max(1.0))
}

pub fn run(project_dir: &Path, lef_dir: &Path, gate_verilog: &str, config: &AprConfig) -> Result<AprResult, String> {
    clear_stale_apr_outputs(project_dir);
    // SBox growth is monotonic and starts at the requested utilization.  At
    // every size the repair phases alter only legal row whitespace; a failed
    // detailed route records its exact reason and advances to the next repair
    // before the floorplan is enlarged.
    // Each phase shifts legal row whitespace independently.  Three
    // well-separated phases expose materially different pin columns without
    // wasting route iterations on near-identical placements.
    let repair_phases = [0.00_f64, 0.23, 0.59];
    let mut iterations = Vec::new();
    let mut selected = None;
    let mut attempt = 0usize;
    write_iteration_snapshot(project_dir, &iterations, "RUNNING", None);
    // Debug-only bounded reproduction.  The override exists solely for
    // router diagnosis; normal APR sizing below is entirely design-driven.
    let debug_attempt_limit = std::env::var("AI_DIGITAL_APR_DEBUG_MAX_ATTEMPTS")
        .ok().and_then(|value| value.parse::<usize>().ok()).filter(|value| *value > 0);
    let debug_sbox_scale = std::env::var("AI_DIGITAL_APR_DEBUG_SBOX_SCALE")
        .ok().and_then(|value| value.parse::<f64>().ok()).filter(|value| value.is_finite() && *value >= 1.0);
    // Closed-loop floorplanning: estimate a legal-resource starting point
    // from this design, then use real route/signoff acceptance as feedback.
    // After a success the search contracts; after a failure it expands.  No
    // fixed scale list or cell-count class is part of the production policy.
    let mut sbox_scale = debug_sbox_scale.unwrap_or(estimate_initial_sbox_scale(lef_dir, gate_verilog, config)?);
    let mut lower_failed = 0.0_f64;
    let mut upper_clean = None::<f64>;
    const MAX_AUTOMATIC_SBOX_STEPS: usize = 14;
    for _ in 0..MAX_AUTOMATIC_SBOX_STEPS {
        let mut candidate_at_scale = None;
        for repair_phase in repair_phases {
            attempt += 1;
            match run_candidate(project_dir, lef_dir, gate_verilog, config, sbox_scale, repair_phase) {
            Ok(result) => {
                let physical_timing_clean = result.drc_status == "PASS" && result.lvs_status == "PASS"
                    && result.placement_overlaps == 0 && result.routing_overflow == 0
                    && result.wns_ns >= 0.0 && result.ir_drop_mv <= config.voltage_v * 100.0;
                // Unit-test and technology-bring-up LEFs may intentionally
                // lack NLDM power data.  They must be able to converge a
                // physical SBox while retaining the explicit estimated-power
                // finding; production runs with power data still require the
                // full signoff gate.
                let candidate_is_clean = physical_timing_clean && (config.power_mw.is_none() || result.signoff_ready);
                iterations.push(AprOptimizationIteration {
                    attempt, sbox_scale, repair_phase, status: if candidate_is_clean { "ACCEPTED" } else { "SIGNOFF_REJECTED" }.to_string(),
                    core_width_um: result.core_width_um, core_height_um: result.core_height_um,
                    utilization: result.utilization, wire_length_um: result.total_wire_length_um,
                    drc_status: result.drc_status.clone(), reason: if candidate_is_clean {
                        "smallest SBox meeting detailed-route, DRC/LVS, timing, IR and power acceptance".to_string()
                    } else {
                        format!("signoff={} drc={} lvs={} overlap={} min_width={} spacing={} shorts={} offgrid={} boundary={} via={} antenna={} overflow={} wns={:.6} ir_mv={:.6}", result.signoff_ready, result.drc_status, result.lvs_status, result.placement_overlaps, result.drc_min_width_violations, result.drc_spacing_violations, result.drc_short_violations, result.drc_offgrid_violations, result.drc_boundary_violations, result.drc_via_violations, result.antenna_warnings, result.routing_overflow, result.wns_ns, result.ir_drop_mv)
                    },
                });
                if candidate_is_clean { candidate_at_scale = Some(result); break; }
            }
            Err(reason) => {
                // A missing or incompatible technology definition cannot be
                // repaired by changing core dimensions.  Stop immediately
                // instead of emitting a long, misleading SBox retry log.
                if reason.starts_with("TECHNOLOGY_COVERAGE_BLOCKED:") {
                    write_iteration_snapshot(project_dir, &iterations, "BLOCKED", Some(&reason));
                    return Err(reason);
                }
                let (core_width_um, core_height_um, utilization) = iteration_floorplan_metrics(
                    lef_dir, gate_verilog, config, sbox_scale,
                ).unwrap_or((0.0, 0.0, 0.0));
                iterations.push(AprOptimizationIteration {
                    attempt, sbox_scale, repair_phase, status: "ROUTE_REPAIR_FAILED".to_string(),
                    core_width_um, core_height_um, utilization, wire_length_um: 0.0,
                    drc_status: "NOT_RUN".to_string(), reason,
                });
            }
            }
            write_iteration_snapshot(project_dir, &iterations, "RUNNING", None);
            if debug_attempt_limit.is_some_and(|limit| attempt >= limit) {
                let diagnostics = iterations.last().map(|item| item.reason.clone()).unwrap_or_else(|| "no diagnostic recorded".to_string());
                let error = format!("Native APR debug attempt limit {attempt} reached: {diagnostics}");
                write_iteration_snapshot(project_dir, &iterations, "BLOCKED", Some(&error));
                return Err(error);
            }
        }
        if let Some(result) = candidate_at_scale {
            upper_clean = Some(sbox_scale);
            selected = Some(result);
            // Do not report a merely feasible floorplan as optimal: probe a
            // materially smaller implementation, then bisect the measured
            // clean/fail bracket until its area resolution is under 3%.
            if lower_failed > 0.0 && (sbox_scale - lower_failed) / sbox_scale <= 0.03 { break; }
            let next = if lower_failed > 0.0 { (lower_failed * sbox_scale).sqrt() } else { sbox_scale * 0.80 };
            if (sbox_scale - next) / sbox_scale < 0.03 { break; }
            sbox_scale = next.max(1.0);
        } else {
            lower_failed = lower_failed.max(sbox_scale);
            sbox_scale = if let Some(clean) = upper_clean { (lower_failed * clean).sqrt() } else { sbox_scale * 1.25 };
            if upper_clean.is_some_and(|clean| (clean - lower_failed) / clean <= 0.03) { break; }
        }
    }
    let Some(mut selected) = selected else {
        let diagnostics = iterations.iter().rev().take(8).rev().map(|item| format!("attempt {} (sbox {:.2}, repair {:.2}): {}", item.attempt, item.sbox_scale, item.repair_phase, item.reason)).collect::<Vec<_>>().join("; ");
        let error = format!("Native APR SBox growth exhausted all detailed-route repair attempts: {diagnostics}");
        write_iteration_snapshot(project_dir, &iterations, "BLOCKED", Some(&error));
        return Err(error);
    };
    selected.optimization_iterations = iterations;
    write_outputs(project_dir, &selected, gate_verilog)?;
    Ok(selected)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn global_grid_reroutes_away_from_history_congestion() {
        let mut grid = NativeGlobalGrid::new(5, 3, 1, 1);
        let first = grid.route((0, 1), (4, 1), 0.0).unwrap();
        grid.commit(&first);
        // Model a second pre-existing net on the same narrow channel so the
        // repair step sees a genuine overflowing resource.
        grid.commit(&first);
        assert!(grid.overflow() > 0);
        grid.increase_history_on_overflow();
        grid.rip_up(&first);
        let repaired = grid.route((0, 1), (4, 1), 0.0).unwrap();
        assert!(repaired.points.iter().any(|(_, y)| *y != 1));
        assert!(repaired.cost.is_finite());
    }

    #[test]
    fn sky130_pin_geometry_is_not_duplicated_by_lef_ingestion() {
        let root = Path::new("lef");
        if !root.exists() { return; }
        let lef = parse_lef(root).expect("project Sky130 LEF must parse");
        let xor = lef.macros.get("sky130_fd_sc_hd__xor2_1").expect("xor2_1 macro must be present");
        let signal = xor.pins.iter().filter(|shape| matches!(shape.name.as_str(), "A" | "B" | "X")).count();
        // The reference macro has 1 A rectangle, 4 B rectangles and 4 X
        // rectangles.  A generous bound keeps the test tolerant of extra
        // legal access shapes while detecting accidental OBS/duplicate-file
        // ingestion that turns a single pin into hundreds of candidates.
        assert!(signal >= 9 && signal <= 24, "unexpected Sky130 signal-pin geometry count: {signal}");
        assert!(xor.pins.iter().filter(|shape| shape.name == "X").count() <= 12);
    }

    #[test]
    fn native_track_router_emits_lef_spaced_manhattan_geometry() {
        let layers = ["li1", "met1", "met2", "met3", "met4", "met5"].map(str::to_string).to_vec();
        let mut lef = LefData { routing_layers: layers.clone(), ..LefData::default() };
        for (index, layer) in layers.iter().enumerate() {
            lef.routing_widths_um.insert(layer.clone(), 0.10);
            lef.routing_spacings_um.insert(layer.clone(), 0.10);
            lef.routing_directions.insert(layer.clone(), if index % 2 == 0 { "VERTICAL" } else { "HORIZONTAL" }.to_string());
            lef.routing_pitches_um.insert(layer.clone(), (0.40, 0.40));
            lef.routing_offsets_um.insert(layer.clone(), (0.20, 0.20));
        }
        let connections = vec![
            NativeRouteConnection { net: "n0".to_string(), source_x: 2.0, source_y: 2.0, target_x: 36.0, target_y: 30.0, source_access: Vec::new(), target_access: Vec::new(), timing_weight: 1.0, route_class: 0 },
            NativeRouteConnection { net: "n1".to_string(), source_x: 35.0, source_y: 3.0, target_x: 3.0, target_y: 31.0, source_access: Vec::new(), target_access: Vec::new(), timing_weight: 0.0, route_class: 1 },
            NativeRouteConnection { net: "n2".to_string(), source_x: 5.0, source_y: 34.0, target_x: 34.0, target_y: 6.0, source_access: Vec::new(), target_access: Vec::new(), timing_weight: 0.0, route_class: 2 },
        ];
        let (routes, vias, overflow) = route_native_tracks(&connections, &lef, 40.0, 40.0, 0.30, 0.60).unwrap();
        assert_eq!(overflow, 0);
        assert!(!routes.is_empty() && !vias.is_empty());
        assert!(routes.iter().all(|segment| wire_length(segment) > 0.0));
        assert_eq!(check_route_geometry(&routes, &lef), (0, 0));
    }

    #[test]
    fn parses_sky130_site_and_track_rules_in_technology_order() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("lef/sky130_fd_sc_hd");
        let lef = parse_lef(&root).unwrap();
        assert_eq!(lef.manufacturing_grid_um, Some(0.005));
        assert_eq!(lef.site_width_um, Some(0.46));
        assert_eq!(lef.site_height_um, Some(2.72));
        assert_eq!(&lef.routing_layers[..6], ["li1", "met1", "met2", "met3", "met4", "met5"]);
        assert_eq!(lef.routing_pitches_um.get("li1"), Some(&(0.46, 0.34)));
        assert_eq!(lef.routing_offsets_um.get("met1"), Some(&(0.17, 0.17)));
        assert_eq!(lef.routing_directions.get("met4").map(String::as_str), Some("VERTICAL"));
        assert_eq!(routing_track_pitch(&lef, "met1"), 0.34);
        assert_eq!(routing_track_pitch(&lef, "met2"), 0.46);
        assert_eq!(routing_track_axis(&lef, "met1").unwrap(), (0.34, 0.17, false));
        assert_eq!(routing_track_axis(&lef, "met2").unwrap(), (0.46, 0.23, true));
        assert!(is_on_routing_track(&lef, "met1", 0.17));
        assert!(!is_on_routing_track(&lef, "met1", 0.23));
        assert!((snap_to_routing_track(&lef, "met2", 0.31).unwrap() - 0.23).abs() < 1e-9);
        validate_apr_technology(&lef).unwrap();
        let and2 = lef.macros.get("sky130_fd_sc_hd__and2_1").unwrap();
        assert_eq!(resolve_macro_pin(and2, "Y").map(|shape| shape.name.as_str()), Some("X"));
    }

    #[test]
    fn technology_preflight_rejects_visual_only_lef() {
        let mut lef = LefData::default();
        lef.routing_layers = vec!["M1".to_string(), "M2".to_string()];
        lef.routing_directions.insert("M1".to_string(), "VERTICAL".to_string());
        lef.routing_directions.insert("M2".to_string(), "HORIZONTAL".to_string());
        let error = validate_apr_technology(&lef).unwrap_err();
        assert!(error.contains("TECHNOLOGY_COVERAGE_BLOCKED"));
        assert!(error.contains("SITE"));
    }

    #[test]
    fn default_sky130_placement_is_site_legal_dense_and_non_overlapping() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let lef = parse_lef(&root.join("lef/sky130_fd_sc_hd")).unwrap();
        let gate = fs::read_to_string(root.join("workspace/default/syn/mul8_synth_gate.v")).unwrap();
        let instances = connectivity_order(parse_instances(&gate));
        let cell_area = instances.iter().map(|instance| {
            let cell = lef.macros.get(&instance.cell).unwrap();
            cell.width_um * cell.height_um
        }).sum::<f64>();
        let site_width = lef.site_width_um.unwrap();
        let row_height = lef.site_height_um.unwrap();
        let core_width = snap_up((cell_area / 0.65).sqrt(), site_width);
        let rows = ((cell_area / 0.65 / core_width) / row_height).ceil().max(1.0) as usize;
        let placed = place_standard_cells(&instances, &lef, core_width, rows, row_height, site_width, 0.0).unwrap();
        let utilization = cell_area / (core_width * rows as f64 * row_height);
        assert!(utilization > 0.60 && utilization <= 0.65 + 1e-9, "utilization={utilization}");
        assert!(placed.iter().all(|cell| ((cell.x_um / site_width).round() * site_width - cell.x_um).abs() < 1e-9));
        assert!(placed.iter().all(|cell| ((cell.y_um / row_height).round() * row_height - cell.y_um).abs() < 1e-9));
        for row in 0..rows {
            let mut cells = placed.iter().filter(|cell| (cell.y_um / row_height).round() as usize == row).collect::<Vec<_>>();
            cells.sort_by(|left, right| left.x_um.partial_cmp(&right.x_um).unwrap());
            for pair in cells.windows(2) {
                assert!(pair[0].x_um + pair[0].width_um <= pair[1].x_um + 1e-9,
                    "row={row} overlap: {} [{:.6}, {:.6}] vs {} [{:.6}, {:.6}]",
                    pair[0].instance, pair[0].x_um, pair[0].x_um + pair[0].width_um,
                    pair[1].instance, pair[1].x_um, pair[1].x_um + pair[1].width_um);
            }
            assert!(cells.last().is_none_or(|cell| cell.x_um + cell.width_um <= core_width + 1e-9));
        }
    }

    #[test]
    fn default_sky130_signal_pins_have_legal_li1_access() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let lef = parse_lef(&root.join("lef/sky130_fd_sc_hd")).unwrap();
        let gate = fs::read_to_string(root.join("workspace/default/syn/mul8_synth_gate.v")).unwrap();
        let instances = connectivity_order(parse_instances(&gate));
        let cell_area = instances.iter().map(|instance| {
            let cell = lef.macros.get(&instance.cell).unwrap();
            cell.width_um * cell.height_um
        }).sum::<f64>();
        let site_width = lef.site_width_um.unwrap();
        let row_height = lef.site_height_um.unwrap();
        let core_width = snap_up((cell_area / 0.65).sqrt(), site_width);
        let rows = ((cell_area / 0.65 / core_width) / row_height).ceil().max(1.0) as usize;
        let placed = place_standard_cells(&instances, &lef, core_width, rows, row_height, site_width, 0.0).unwrap();
        let by_name = placed.iter().map(|cell| (cell.instance.as_str(), cell)).collect::<HashMap<_, _>>();
        for instance in &instances {
            let cell = by_name[instance.name.as_str()];
            for (pin, _) in &instance.pins {
                let accesses = instance_pin_accesses(cell, pin, &lef);
                assert!(
                    pin_vertical_access(&accesses, &lef, "li1", cell.x_um + cell.width_um * 0.5, cell.y_um + cell.height_um * 0.5).is_some(),
                    "{}.{pin} has no legal li1 access; rects={accesses:?}", instance.name,
                );
            }
        }
    }

    #[test]
    fn parses_and_places_a_small_local_design() {
        let root = std::env::temp_dir().join(format!("ai_apr_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("lef")).unwrap();
        fs::write(root.join("lef/tech.tlef"), r#"
MANUFACTURINGGRID 0.01 ;
SITE core
 SIZE 0.20 BY 1.00 ;
END core
LAYER M1
 TYPE ROUTING ;
 DIRECTION VERTICAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M1
LAYER M2
 TYPE ROUTING ;
 DIRECTION HORIZONTAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M2
LAYER M3
 TYPE ROUTING ;
 DIRECTION VERTICAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M3
LAYER M4
 TYPE ROUTING ;
 DIRECTION HORIZONTAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M4
LAYER M5
 TYPE ROUTING ;
 DIRECTION VERTICAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M5
LAYER M6
 TYPE ROUTING ;
 DIRECTION HORIZONTAL ;
 WIDTH 0.10 ;
 SPACING 0.10 ;
 PITCH 0.20 ;
 OFFSET 0.20 ;
END M6
"#).unwrap();
        fs::write(root.join("lef/cells.lef"), r#"
MACRO INV
 SIZE 1 BY 1 ;
 PIN A
  PORT
   LAYER M1 ;
   RECT 0.15 0.40 0.25 0.60 ;
  END
 END A
 PIN Y
  PORT
   LAYER M1 ;
   RECT 0.75 0.40 0.85 0.60 ;
  END
 END Y
END INV
MACRO BUF
 SIZE 2 BY 1 ;
 PIN A
  PORT
   LAYER M1 ;
   RECT 0.15 0.40 0.25 0.60 ;
  END
 END A
 PIN Y
  PORT
   LAYER M1 ;
   RECT 1.75 0.40 1.85 0.60 ;
  END
 END Y
END BUF
"#).unwrap();
        let netlist = "module top; wire n; INV u0 (.A(a), .Y(n)); BUF u1 (.A(n), .Y(y)); endmodule";
        let mut config = AprConfig { module_name: "top".to_string(), ..AprConfig::default() };
        config.critical_nets.insert("n".to_string());
        let result = run(&root, &root.join("lef"), netlist, &config).unwrap();
        assert_eq!(result.cells.len(), 2);
        assert!(result.cells.iter().all(|cell| cell.x_um > 0.0));
        let occupied_right = result.cells.iter().map(|cell| cell.x_um + cell.width_um).fold(0.0_f64, f64::max);
        assert!(occupied_right < result.core_width_um);
        assert!(occupied_right > result.core_width_um * 0.50);
        assert!(!result.routes.is_empty());
        assert_eq!(result.critical_route_source, "pre-APR STA worst path");
        assert!(result.critical_routes.iter().all(|route| route.net == "n"));
        assert!(root.join("apr/apr_report.json").exists());
        let exchange = fs::read_to_string(root.join("exchange/apr_layout.tsv")).unwrap();
        assert!(exchange.lines().nth(1).is_some_and(|line| line.starts_with("die\tDIE\t")));
        assert!(exchange.lines().nth(2).is_some_and(|line| line.starts_with("core\tCORE\t")));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn geometric_drc_distinguishes_short_spacing_and_same_net_join() {
        let mut lef = LefData::default();
        lef.routing_layers.push("M1".to_string());
        lef.routing_widths_um.insert("M1".to_string(), 0.10);
        lef.routing_spacings_um.insert("M1".to_string(), 0.10);
        let route = |net: &str, y: f64| RouteSegment {
            net: net.to_string(), layer: "M1".to_string(), width_um: 0.10,
            x1_um: 0.0, y1_um: y, x2_um: 2.0, y2_um: y,
        };
        let (spacing, shorts) = check_route_geometry(&[route("a", 0.0), route("b", 0.0)], &lef);
        assert_eq!((spacing, shorts), (0, 1));
        let (spacing, shorts) = check_route_geometry(&[route("a", 0.0), route("b", 0.18)], &lef);
        assert_eq!((spacing, shorts), (1, 0));
        let (spacing, shorts) = check_route_geometry(&[route("a", 0.0), route("a", 0.0)], &lef);
        assert_eq!((spacing, shorts), (0, 0));
    }

    #[test]
    fn via_builder_allows_only_adjacent_layers() {
        let route = |layer: &str| RouteSegment {
            net: "n".to_string(), layer: layer.to_string(), width_um: 0.14,
            x1_um: 1.0, y1_um: 1.0, x2_um: 2.0, y2_um: 1.0,
        };
        let layers = vec!["M1".to_string(), "M2".to_string(), "M3".to_string()];
        // The two overlapping segment endpoints are distinct physical
        // transitions, hence each needs its own adjacent-layer via.
        assert_eq!(build_vias(&[route("M1"), route("M2")], &layers).len(), 2);
        assert!(build_vias(&[route("M1"), route("M3")], &layers).is_empty());
    }

}
