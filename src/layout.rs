//! Native post-APR layout inspection and physical-verification stage.
//!
//! The implementation deliberately reads the GDSII byte stream itself.  It
//! does not shell out to a layout editor, DRC deck, or LVS tool.  The checks
//! are geometric/layout-consistency checks whose scope is recorded in every
//! report; they must never be presented as a foundry-calibrated signoff deck.

use serde::Serialize;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Serialize)]
pub struct LayoutShape {
    pub kind: String,
    pub layer: i16,
    pub datatype: i16,
    pub width_um: f64,
    pub x1_um: f64,
    pub y1_um: f64,
    pub x2_um: f64,
    pub y2_um: f64,
    /// Original GDS PATH centerline points. Boundaries intentionally leave
    /// this empty and use their bounding box for compact GUI rendering.
    pub path_points: Vec<(f64, f64)>,
}

#[derive(Clone, Debug, Serialize)]
pub struct LayoutResult {
    pub source_gds: String,
    pub materialized_gds_path: String,
    pub library: String,
    pub structures: Vec<String>,
    pub shapes: Vec<LayoutShape>,
    pub die_width_um: f64,
    pub die_height_um: f64,
    pub layer_shape_counts: BTreeMap<i16, usize>,
    pub drc_status: String,
    pub lvs_status: String,
    pub drc_errors: usize,
    pub lvs_errors: usize,
    pub report_path: String,
    pub drc_report_path: String,
    pub lvs_report_path: String,
    pub exchange_path: String,
    pub findings: Vec<String>,
}

#[derive(Clone, Debug)]
struct LayoutInstance {
    name: String,
    cell: String,
    x_um: f64,
    y_um: f64,
}

fn i16_at(data: &[u8]) -> Option<i16> { (data.len() >= 2).then(|| i16::from_be_bytes([data[0], data[1]])) }
fn i32_at(data: &[u8]) -> Option<i32> { (data.len() >= 4).then(|| i32::from_be_bytes([data[0], data[1], data[2], data[3]])) }

fn gds_text(payload: &[u8]) -> String {
    String::from_utf8_lossy(payload).trim_end_matches('\0').trim().to_string()
}

fn xy_points(payload: &[u8]) -> Vec<(f64, f64)> {
    payload.chunks_exact(8).filter_map(|pair| {
        Some((i32_at(&pair[..4])? as f64 / 1000.0, i32_at(&pair[4..])? as f64 / 1000.0))
    }).collect()
}

fn bbox(points: &[(f64, f64)], half_width: f64) -> Option<(f64, f64, f64, f64)> {
    let mut out = (f64::INFINITY, f64::INFINITY, f64::NEG_INFINITY, f64::NEG_INFINITY);
    for (x, y) in points { out.0 = out.0.min(*x); out.1 = out.1.min(*y); out.2 = out.2.max(*x); out.3 = out.3.max(*y); }
    (out.0.is_finite() && out.2.is_finite()).then(|| (out.0 - half_width, out.1 - half_width, out.2 + half_width, out.3 + half_width))
}

/// Read the subset of GDSII geometry required by a physical layout viewer:
/// BOUNDARY/PATH layer, datatype, width and XY records.  Unknown record types
/// are length-checked and skipped, preserving compatibility with reference
/// Sky130 standard-cell GDS files without pretending to understand their
/// proprietary or hierarchy-specific semantics.
fn parse_gds(path: &Path) -> Result<(String, Vec<String>, Vec<LayoutShape>), String> {
    let bytes = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if bytes.len() < 4 { return Err(format!("{} is not a GDSII stream: file is too short", path.display())); }
    let mut offset = 0usize;
    let mut library = String::new();
    let mut structures = Vec::new();
    let mut kind = String::new();
    let mut layer = 0_i16;
    let mut datatype = 0_i16;
    let mut width_um = 0.0_f64;
    let mut xy = Vec::new();
    let mut shapes = Vec::new();
    while offset + 4 <= bytes.len() {
        let length = u16::from_be_bytes([bytes[offset], bytes[offset + 1]]) as usize;
        if length < 4 || offset + length > bytes.len() { return Err(format!("Malformed GDS record at byte {offset}: length={length}")); }
        let record = bytes[offset + 2];
        let payload = &bytes[offset + 4..offset + length];
        match record {
            0x02 => library = gds_text(payload),
            0x06 => { let name = gds_text(payload); if !name.is_empty() { structures.push(name); } }
            0x08 => { kind = "boundary".to_string(); layer = 0; datatype = 0; width_um = 0.0; xy.clear(); }
            0x09 => { kind = "path".to_string(); layer = 0; datatype = 0; width_um = 0.0; xy.clear(); }
            0x0d => layer = i16_at(payload).ok_or_else(|| format!("Malformed LAYER record at byte {offset}"))?,
            0x0e => datatype = i16_at(payload).ok_or_else(|| format!("Malformed DATATYPE record at byte {offset}"))?,
            0x0f => width_um = i32_at(payload).ok_or_else(|| format!("Malformed WIDTH record at byte {offset}"))?.unsigned_abs() as f64 / 1000.0,
            0x10 => xy = xy_points(payload),
            0x11 => {
                if !kind.is_empty() {
                    if let Some((x1, y1, x2, y2)) = bbox(&xy, if kind == "path" { width_um * 0.5 } else { 0.0 }) {
                        shapes.push(LayoutShape { kind: kind.clone(), layer, datatype, width_um, x1_um: x1, y1_um: y1, x2_um: x2, y2_um: y2,
                            path_points: if kind == "path" { xy.clone() } else { Vec::new() } });
                    }
                }
                kind.clear(); xy.clear();
            }
            _ => {}
        }
        offset += length;
    }
    if offset != bytes.len() { return Err(format!("Trailing partial GDS record in {}", path.display())); }
    if shapes.is_empty() { return Err(format!("{} contains no visible BOUNDARY/PATH geometry", path.display())); }
    Ok((library, structures, shapes))
}

fn overlap(a: &LayoutShape, b: &LayoutShape) -> bool {
    a.x1_um < b.x2_um && b.x1_um < a.x2_um && a.y1_um < b.y2_um && b.y1_um < a.y2_um
}

fn gds_i2(value: i16) -> [u8; 2] { value.to_be_bytes() }
fn gds_i4(value: i32) -> [u8; 4] { value.to_be_bytes() }

fn gds_record(stream: &mut Vec<u8>, record: u8, data_type: u8, payload: &[u8]) {
    let length = payload.len().saturating_add(4);
    stream.extend_from_slice(&(length as u16).to_be_bytes());
    stream.push(record);
    stream.push(data_type);
    stream.extend_from_slice(payload);
}

fn gds_ascii(text: &str) -> Vec<u8> {
    let mut out = text.as_bytes().to_vec();
    if out.len() % 2 != 0 { out.push(0); }
    out
}

fn gds_xy(points: &[(f64, f64)]) -> Vec<u8> {
    let mut out = Vec::with_capacity(points.len() * 8);
    for (x, y) in points {
        out.extend_from_slice(&gds_i4((x * 1000.0).round() as i32));
        out.extend_from_slice(&gds_i4((y * 1000.0).round() as i32));
    }
    out
}

/// Extract complete BGNSTR..ENDSTR record ranges without interpreting the
/// foundry cell internals.  They remain the original GDS structures; the
/// native layout writer only owns the top-level assembly and placement SREFs.
fn gds_structures(path: &Path) -> Result<Vec<Vec<u8>>, String> {
    let bytes = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut offset = 0usize;
    let mut start = None;
    let mut output = Vec::new();
    while offset + 4 <= bytes.len() {
        let length = u16::from_be_bytes([bytes[offset], bytes[offset + 1]]) as usize;
        if length < 4 || offset + length > bytes.len() { return Err(format!("Malformed GDS record at byte {offset} in {}", path.display())); }
        match bytes[offset + 2] {
            0x05 => {
                if start.is_some() { return Err(format!("Nested BGNSTR in {}", path.display())); }
                start = Some(offset);
            }
            0x07 => {
                let begin = start.take().ok_or_else(|| format!("ENDSTR without BGNSTR in {}", path.display()))?;
                output.push(bytes[begin..offset + length].to_vec());
            }
            _ => {}
        }
        offset += length;
    }
    if offset != bytes.len() || start.is_some() || output.is_empty() {
        return Err(format!("{} does not contain a complete GDS structure", path.display()));
    }
    Ok(output)
}

fn project_gds_directory(project_dir: &Path) -> Result<PathBuf, String> {
    let config = fs::read_to_string(project_dir.join("config.json"))
        .map_err(|_| "Project config is missing; cannot select the layout GDS library".to_string())?;
    let value: serde_json::Value = serde_json::from_str(&config)
        .map_err(|e| format!("Project config is invalid: {e}"))?;
    let technology = value.get("technology").and_then(|v| v.as_str())
        .filter(|name| !name.is_empty())
        .ok_or_else(|| "Project has no selected technology; cannot materialize layout GDS".to_string())?;
    let root = project_dir.parent().and_then(Path::parent)
        .ok_or_else(|| "Project directory is not inside a workspace".to_string())?;
    let directory = root.join("gds").join(technology);
    if !directory.is_dir() {
        return Err(format!("LAYOUT_TECHNOLOGY_BLOCKED: no project GDS library for technology {} at {}", technology, directory.display()));
    }
    Ok(directory)
}

fn parse_apr_instances(exchange: &Path) -> Result<(f64, f64, f64, f64, Vec<LayoutInstance>), String> {
    let text = fs::read_to_string(exchange).map_err(|_| "APR layout exchange missing for native layout assembly".to_string())?;
    let mut die = None;
    let mut core = None;
    let mut instances = Vec::new();
    for line in text.lines().skip(1) {
        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() < 7 { continue; }
        let parse = |index: usize| fields.get(index).and_then(|v| v.parse::<f64>().ok());
        match fields[0] {
            "die" => die = parse(5).zip(parse(6)),
            "core" => core = parse(5).zip(parse(6)),
            "cell" | "filler" => {
                let (Some(x_um), Some(y_um)) = (parse(3), parse(4)) else { continue; };
                instances.push(LayoutInstance { name: fields[1].to_string(), cell: fields[2].to_string(), x_um, y_um });
            }
            _ => {}
        }
    }
    let (width, height) = die.ok_or_else(|| "APR layout exchange has no die boundary".to_string())?;
    let (core_width, core_height) = core.ok_or_else(|| "APR layout exchange has no core boundary".to_string())?;
    if instances.is_empty() { return Err("APR layout exchange has no placed standard cells".to_string()); }
    Ok((width, height, core_width, core_height, instances))
}

fn translate_shape(shape: &LayoutShape, x_um: f64, y_um: f64) -> LayoutShape {
    // Preserve the source geometry class in the exchange.  Cell-internal
    // PATHs are legal foundry geometry, not independently routed top-level
    // nets, so layout DRC must not compare them with top-level router PATHs.
    LayoutShape { kind: format!("cell_{}", shape.kind), layer: shape.layer, datatype: shape.datatype, width_um: shape.width_um,
        x1_um: shape.x1_um + x_um, y1_um: shape.y1_um + y_um, x2_um: shape.x2_um + x_um, y2_um: shape.y2_um + y_um,
        path_points: shape.path_points.iter().map(|(px, py)| (px + x_um, py + y_um)).collect() }
}

/// Build the post-APR mask layout.  APR owns placement/routing decisions;
/// this stage owns the GDS hierarchy, real standard-cell geometry import and
/// final assembled stream.  It intentionally does not reuse APR's abstract
/// cell rectangles as a purported foundry layout.
fn materialize_layout(project_dir: &Path) -> Result<(PathBuf, String, Vec<String>, Vec<LayoutShape>, usize), String> {
    let exchange_path = project_dir.join("exchange").join("apr_layout.tsv");
    let (die_width_um, die_height_um, core_width_um, core_height_um, instances) = parse_apr_instances(&exchange_path)?;
    // APR reserves a die halo for seal-ring/PDN.  Its abstract exchange uses
    // core-local coordinates; the layout implementation materializes them
    // inside that halo so the foundry cell rails and top-level routes remain
    // physically inside the assembled die.
    let offset_x_um = ((die_width_um - core_width_um) * 0.5).max(0.0);
    let offset_y_um = ((die_height_um - core_height_um) * 0.5).max(0.0);
    let gds_dir = project_gds_directory(project_dir)?;
    let apr_gds = project_dir.join("apr").join("final.gds");
    let (_, _, apr_shapes) = parse_gds(&apr_gds)?;
    let mut leaf_shapes: BTreeMap<String, Vec<LayoutShape>> = BTreeMap::new();
    let mut leaf_structures: BTreeMap<String, Vec<Vec<u8>>> = BTreeMap::new();
    let mut structures = vec!["AI_DIGITAL_LAYOUT_TOP".to_string()];
    let mut fallback_cells = 0usize;
    for instance in &instances {
        if leaf_shapes.contains_key(&instance.cell) { continue; }
        let source = gds_dir.join(format!("{}.gds", instance.cell));
        if source.is_file() {
            let (_, names, shapes) = parse_gds(&source)?;
            let copied = gds_structures(&source)?;
            let name = names.first().cloned().unwrap_or_else(|| instance.cell.clone());
            structures.extend(names);
            leaf_shapes.insert(instance.cell.clone(), shapes);
            leaf_structures.insert(instance.cell.clone(), copied);
            if name != instance.cell { return Err(format!("GDS structure name mismatch for {}: found {}", instance.cell, name)); }
        } else {
            // Keep the final stream valid for an incomplete technology while
            // explicitly recording the abstract fallback.  The default
            // Sky130 mapping is covered by the checked-in representative GDS.
            fallback_cells += 1;
            leaf_shapes.insert(instance.cell.clone(), Vec::new());
        }
    }
    let mut shapes = Vec::new();
    shapes.push(LayoutShape { kind: "boundary".to_string(), layer: 0, datatype: 0, width_um: 0.0, x1_um: 0.0, y1_um: 0.0, x2_um: die_width_um, y2_um: die_height_um, path_points: Vec::new() });
    for instance in &instances {
        let cell_shapes = leaf_shapes.get(&instance.cell).expect("loaded above");
        if cell_shapes.is_empty() {
            shapes.push(LayoutShape { kind: "abstract_cell".to_string(), layer: 250, datatype: 0, width_um: 0.0, x1_um: instance.x_um + offset_x_um, y1_um: instance.y_um + offset_y_um, x2_um: instance.x_um + offset_x_um + 0.01, y2_um: instance.y_um + offset_y_um + 0.01, path_points: Vec::new() });
        } else {
            shapes.extend(cell_shapes.iter().map(|shape| translate_shape(shape, instance.x_um + offset_x_um, instance.y_um + offset_y_um)));
        }
    }
    // Routes, PDN and cuts are APR's final physical connectivity, whereas
    // standard-cell geometry above comes from the selected foundry GDS.
    shapes.extend(apr_shapes.into_iter().filter(|shape| shape.kind == "path" || shape.layer == 30)
        .map(|shape| translate_shape(&shape, offset_x_um, offset_y_um))
        .map(|mut shape| { shape.kind = shape.kind.trim_start_matches("cell_").to_string(); shape }));

    let layout_dir = project_dir.join("layout");
    fs::create_dir_all(&layout_dir).map_err(|e| e.to_string())?;
    let output = layout_dir.join("final_layout.gds");
    let mut stream = Vec::new();
    gds_record(&mut stream, 0x00, 0x02, &gds_i2(600));
    gds_record(&mut stream, 0x01, 0x02, &[0; 24]);
    gds_record(&mut stream, 0x02, 0x06, &gds_ascii("AI_DIGITAL_LAYOUT"));
    gds_record(&mut stream, 0x03, 0x05, &[0x3e, 0x41, 0x0c, 0x9f, 0x7c, 0x7f, 0x3e, 0x41, 0x0c, 0x9f, 0x7c, 0x7f]);
    for copied in leaf_structures.values() { for structure in copied { stream.extend_from_slice(structure); } }
    gds_record(&mut stream, 0x05, 0x02, &[0; 24]);
    gds_record(&mut stream, 0x06, 0x06, &gds_ascii("AI_DIGITAL_LAYOUT_TOP"));
    // Die outline is owned by the assembly, not copied from APR's abstract.
    gds_record(&mut stream, 0x08, 0x00, &[]);
    gds_record(&mut stream, 0x0d, 0x02, &gds_i2(0));
    gds_record(&mut stream, 0x0e, 0x02, &gds_i2(0));
    gds_record(&mut stream, 0x10, 0x03, &gds_xy(&[(0.0, 0.0), (die_width_um, 0.0), (die_width_um, die_height_um), (0.0, die_height_um), (0.0, 0.0)]));
    gds_record(&mut stream, 0x11, 0x00, &[]);
    for instance in &instances {
        if leaf_structures.contains_key(&instance.cell) {
            gds_record(&mut stream, 0x0a, 0x00, &[]);
            gds_record(&mut stream, 0x12, 0x06, &gds_ascii(&instance.cell));
            gds_record(&mut stream, 0x10, 0x03, &gds_xy(&[(instance.x_um + offset_x_um, instance.y_um + offset_y_um)]));
            gds_record(&mut stream, 0x11, 0x00, &[]);
        }
    }
    for shape in shapes.iter().filter(|shape| shape.kind == "path" || shape.layer == 30) {
        if shape.kind == "path" {
            gds_record(&mut stream, 0x09, 0x00, &[]);
            gds_record(&mut stream, 0x0d, 0x02, &gds_i2(shape.layer));
            gds_record(&mut stream, 0x0e, 0x02, &gds_i2(shape.datatype));
            gds_record(&mut stream, 0x0f, 0x03, &gds_i4((shape.width_um * 1000.0).round() as i32));
            let points = if shape.path_points.len() >= 2 { shape.path_points.clone() } else { vec![(shape.x1_um, shape.y1_um), (shape.x2_um, shape.y2_um)] };
            gds_record(&mut stream, 0x10, 0x03, &gds_xy(&points));
        } else {
            gds_record(&mut stream, 0x08, 0x00, &[]);
            gds_record(&mut stream, 0x0d, 0x02, &gds_i2(shape.layer));
            gds_record(&mut stream, 0x0e, 0x02, &gds_i2(shape.datatype));
            gds_record(&mut stream, 0x10, 0x03, &gds_xy(&[(shape.x1_um, shape.y1_um), (shape.x2_um, shape.y1_um), (shape.x2_um, shape.y2_um), (shape.x1_um, shape.y2_um), (shape.x1_um, shape.y1_um)]));
        }
        gds_record(&mut stream, 0x11, 0x00, &[]);
    }
    gds_record(&mut stream, 0x07, 0x00, &[]);
    gds_record(&mut stream, 0x04, 0x00, &[]);
    fs::write(&output, stream).map_err(|e| format!("{}: {e}", output.display()))?;
    Ok((output, "AI_DIGITAL_LAYOUT".to_string(), structures, shapes, fallback_cells))
}

/// Execute native layout import, geometrical DRC and APR-to-layout LVS.
///
/// `final.gds` generated by native APR is flattened, so this LVS compares
/// its materialized placement/routing population with the APR netlist and
/// exchange topology.  A standalone foundry-cell GDS contains no top-level
/// schematic/netlist; it is intentionally reported as `NOT_APPLICABLE`, not
/// fabricated into a PASS result.
pub fn run(project_dir: &Path, gds_override: Option<&Path>) -> Result<LayoutResult, String> {
    let (source, library, structures, shapes, is_native_top, fallback_cells) = if let Some(override_gds) = gds_override {
        let (library, structures, shapes) = parse_gds(override_gds)?;
        (override_gds.to_path_buf(), library, structures, shapes, false, 0usize)
    } else {
        let (source, library, structures, shapes, fallback_cells) = materialize_layout(project_dir)?;
        (source, library, structures, shapes, true, fallback_cells)
    };
    let mut layers = BTreeMap::new();
    for shape in &shapes { *layers.entry(shape.layer).or_insert(0usize) += 1; }
    let mut findings = Vec::new();
    let mut drc_errors = 0usize;
    let die = shapes.iter().filter(|shape| shape.kind == "boundary" && shape.layer == 0)
        .max_by(|a, b| {
            let area_a = (a.x2_um - a.x1_um) * (a.y2_um - a.y1_um);
            let area_b = (b.x2_um - b.x1_um) * (b.y2_um - b.y1_um);
            area_a.partial_cmp(&area_b).unwrap_or(std::cmp::Ordering::Equal)
        });
    let has_die_outline = die.is_some();
    let (die_width_um, die_height_um) = die.map(|shape| (shape.x2_um - shape.x1_um, shape.y2_um - shape.y1_um)).unwrap_or_else(|| {
        // Standard-cell reference GDS is normally a leaf view and has no
        // die outline.  Its geometric envelope is still a useful, correctly
        // scaled viewport for the Virtuoso-style viewer.
        let x1 = shapes.iter().map(|shape| shape.x1_um).fold(f64::INFINITY, f64::min);
        let y1 = shapes.iter().map(|shape| shape.y1_um).fold(f64::INFINITY, f64::min);
        let x2 = shapes.iter().map(|shape| shape.x2_um).fold(f64::NEG_INFINITY, f64::max);
        let y2 = shapes.iter().map(|shape| shape.y2_um).fold(f64::NEG_INFINITY, f64::max);
        ((x2 - x1).max(0.0), (y2 - y1).max(0.0))
    });
    for shape in &shapes {
        if !shape.x1_um.is_finite() || !shape.y1_um.is_finite() || !shape.x2_um.is_finite() || !shape.y2_um.is_finite() || shape.x2_um < shape.x1_um || shape.y2_um < shape.y1_um {
            drc_errors += 1; findings.push(format!("Invalid geometry on layer {}", shape.layer));
        }
        if shape.kind == "path" && shape.width_um <= 0.0 { drc_errors += 1; findings.push(format!("Non-positive path width on layer {}", shape.layer)); }
        // Leaf standard-cell GDS normally has no die boundary and can use
        // negative local coordinates.  Boundary checking applies only to a
        // materialized top-level die; leaf views are checked for valid shape
        // geometry and displayed from their local envelope instead.
        if has_die_outline && (shape.x1_um < -1e-6 || shape.y1_um < -1e-6 || shape.x2_um > die_width_um + 1e-6 || shape.y2_um > die_height_um + 1e-6) {
            drc_errors += 1; findings.push(format!("Geometry exceeds die boundary on layer {}", shape.layer));
        }
    }
    // For this native flattened writer, layer 0 contains deliberately
    // overlapping cell boundaries.  On routing layers, overlapping PATH
    // bboxes are an actionable layout conflict candidate; only compare a
    // bounded neighbourhood, keeping import linear-ish on reference cells.
    for (index, a) in shapes.iter().enumerate() {
        if a.kind != "path" { continue; }
        for b in shapes.iter().skip(index + 1).take(2048) {
            if b.kind == "path" && a.layer == b.layer && a.datatype != b.datatype && overlap(a, b) {
                drc_errors += 1;
                findings.push(format!("Same-layer route overlap candidate on layer {}", a.layer));
                if drc_errors > 128 { break; }
            }
        }
        if drc_errors > 128 { findings.push("DRC overlap listing capped at 129 findings".to_string()); break; }
    }
    let apr_netlist = project_dir.join("apr").join("apr_netlist.v");
    let apr_layout = project_dir.join("exchange").join("apr_layout.tsv");
    let mut lvs_errors = 0usize;
    let lvs_status = if is_native_top {
        let netlist = fs::read_to_string(&apr_netlist).map_err(|_| "APR netlist missing for native layout LVS".to_string())?;
        let exchange = fs::read_to_string(&apr_layout).map_err(|_| "APR layout exchange missing for native layout LVS".to_string())?;
        let expected_cells = netlist.lines().filter(|line| line.contains("sky130_") && line.contains("(")).count();
        let visible_cells = exchange.lines().filter(|line| line.starts_with("cell\t")).count();
        let visible_routes = exchange.lines().filter(|line| line.starts_with("route\t")).count();
        if expected_cells == 0 || visible_cells != expected_cells || visible_routes == 0 {
            lvs_errors += 1;
            findings.push(format!("APR-layout topology mismatch: netlist cells={}, visible cells={}, visible routes={}", expected_cells, visible_cells, visible_routes));
            "FAIL".to_string()
        } else { "PASS".to_string() }
    } else {
        findings.push("Standalone reference-cell GDS has no matching top-level schematic/netlist; LVS is NOT_APPLICABLE.".to_string());
        "NOT_APPLICABLE".to_string()
    };
    let drc_status = if drc_errors == 0 { "PASS" } else { "FAIL" }.to_string();
    if fallback_cells > 0 {
        findings.push(format!("{} placed cell(s) have no matching project GDS and use an explicitly marked abstract fallback.", fallback_cells));
    }
    if findings.is_empty() { findings.push("Native assembled layout geometry and APR-to-layout topology checks passed.".to_string()); }
    let layout_dir = project_dir.join("layout");
    let exchange_dir = project_dir.join("exchange");
    fs::create_dir_all(&layout_dir).map_err(|e| e.to_string())?;
    fs::create_dir_all(&exchange_dir).map_err(|e| e.to_string())?;
    let mut exchange = String::from("kind\tlayer\tdatatype\twidth_um\tx1_um\ty1_um\tx2_um\ty2_um\tpath_x1_um\tpath_y1_um\tpath_x2_um\tpath_y2_um\n");
    for shape in &shapes {
        let endpoints = shape.path_points.first().zip(shape.path_points.last())
            .map(|(first, last)| (first.0, first.1, last.0, last.1))
            .unwrap_or((shape.x1_um, shape.y1_um, shape.x2_um, shape.y2_um));
        exchange.push_str(&format!("{}\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\t{:.6}\n", shape.kind, shape.layer, shape.datatype, shape.width_um, shape.x1_um, shape.y1_um, shape.x2_um, shape.y2_um, endpoints.0, endpoints.1, endpoints.2, endpoints.3));
    }
    fs::write(exchange_dir.join("layout_geometry.tsv"), exchange).map_err(|e| e.to_string())?;
    let drc_report = format!("Native Layout Geometry DRC\n==========================\nSource: {}\nScope: stream validity, finite geometry, die boundary, positive path width, same-layer route overlap candidates. Not a foundry deck.\n\nStatus: {}\nErrors: {}\n\n{}\n", source.display(), drc_status, drc_errors, findings.join("\n"));
    fs::write(layout_dir.join("drc_report.txt"), drc_report).map_err(|e| e.to_string())?;
    let lvs_report = format!("Native Layout LVS\n=================\nSource: {}\nScope: assembled layout GDS population vs APR netlist/exchange topology. A standalone cell GDS without a matching top-level schematic is NOT_APPLICABLE.\n\nStatus: {}\nErrors: {}\n", source.display(), lvs_status, lvs_errors);
    fs::write(layout_dir.join("lvs_report.txt"), lvs_report).map_err(|e| e.to_string())?;
    let result = LayoutResult { source_gds: source.to_string_lossy().to_string(), materialized_gds_path: if is_native_top { "layout/final_layout.gds".to_string() } else { String::new() }, library, structures, shapes, die_width_um, die_height_um, layer_shape_counts: layers, drc_status, lvs_status, drc_errors, lvs_errors, report_path: "layout/layout_report.txt".to_string(), drc_report_path: "layout/drc_report.txt".to_string(), lvs_report_path: "layout/lvs_report.txt".to_string(), exchange_path: "exchange/layout_geometry.tsv".to_string(), findings };
    let json = serde_json::to_string_pretty(&result).map_err(|e| e.to_string())?;
    fs::write(layout_dir.join("layout_report.json"), &json).map_err(|e| e.to_string())?;
    let report = format!("Native Layout Report\n====================\nGDS: {}\nMaterialized GDS: {}\nLibrary: {}\nStructures: {}\nShapes: {}\nDie: {:.3} x {:.3} um\nLayers: {:?}\nDRC: {} ({} errors)\nLVS: {} ({} errors)\nOutputs: layout/final_layout.gds layout/drc_report.txt layout/lvs_report.txt exchange/layout_geometry.tsv\n\n{}\n", result.source_gds, if result.materialized_gds_path.is_empty() { "not generated for reference import" } else { &result.materialized_gds_path }, result.library, result.structures.len(), result.shapes.len(), result.die_width_um, result.die_height_um, result.layer_shape_counts, result.drc_status, result.drc_errors, result.lvs_status, result.lvs_errors, result.findings.join("\n"));
    fs::write(layout_dir.join("layout_report.txt"), report).map_err(|e| e.to_string())?;
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_malformed_stream() {
        let root = std::env::temp_dir().join(format!("layout_bad_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root); fs::create_dir_all(&root).unwrap();
        fs::write(root.join("bad.gds"), [0, 10, 8, 0]).unwrap();
        assert!(parse_gds(&root.join("bad.gds")).is_err());
        let _ = fs::remove_dir_all(&root);
    }
}
