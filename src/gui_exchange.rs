use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use crate::project::SynthesisOptions;

#[derive(Clone, Debug, Default)]
pub struct GuiTechnologyCorner {
    pub technology: String,
    pub corner: String,
    pub corner_type: String,
    pub library: String,
    pub voltage: f64,
    pub temperature: f64,
    pub cells: i32,
    pub time_unit: String,
    pub voltage_unit: String,
    pub leakage_power_unit: String,
    pub capacitance_unit: String,
    pub is_synthesis: bool,
}

#[derive(Clone, Debug, Default)]
pub struct GuiSyncContext {
    pub project_name: String,
    pub project_dir: PathBuf,
    pub module_name: String,
    pub modules: Vec<String>,
    pub current_step: String,
    pub step_status: String,
    pub status_text: String,
    pub constraint_freq_mhz: i32,
    pub last_error: String,
    pub active_technology: String,
    pub technology_corners: Vec<GuiTechnologyCorner>,
    pub synthesis_options: SynthesisOptions,
}

#[derive(Clone, Debug, Default)]
pub struct GuiStateFiles {
    pub state_path: PathBuf,
}

fn formal_status_from_report(report: &str) -> &'static str {
    let upper = report.to_ascii_uppercase();
    if upper.contains("FORMAL VERIFICATION: FAIL")
        || upper.contains("RTL AND GATE-LEVEL NETLIST DIFFER")
        || upper.contains("RTL != GATE-LEVEL")
        || upper.contains("RTL VS GATE-LEVEL: DIFFERENT")
    {
        "FAIL"
    } else if upper.contains("FORMAL VERIFICATION: PASS")
        || upper.contains("RTL AND GATE-LEVEL NETLIST ARE EQUIVALENT")
        || upper.contains("RTL == GATE-LEVEL")
        || upper.contains("RTL VS GATE-LEVEL: EQUIVALENT")
    {
        "PASS"
    } else {
        "UNKNOWN"
    }
}

pub fn write_state(ctx: &GuiSyncContext) -> Result<GuiStateFiles, String> {
    let exchange_dir = ctx.project_dir.join("exchange");
    fs::create_dir_all(&exchange_dir)
        .map_err(|e| format!("Failed to create exchange dir: {}", e))?;

    let rtl_path = preferred_source_path(&ctx.project_dir, &ctx.module_name);
    let tb_path = existing_path(&ctx.project_dir.join("tb").join(format!("{}_tb.v", ctx.module_name)));
    let sdc_path = existing_path(&ctx.project_dir.join("sdc").join(format!("{}.sdc", ctx.module_name)));
    let max_sdc_path = existing_path(&ctx.project_dir.join("sdc").join(format!("{}_max_freq.sdc", ctx.module_name)));
    let gate_path = existing_path(&ctx.project_dir.join("syn").join(format!("{}_synth_gate.v", ctx.module_name)));
    let sim_report_path = existing_path(&ctx.project_dir.join("sim").join("sim_report.txt"));
    let timing_report_path = existing_path(&ctx.project_dir.join("syn").join("timing_report.txt"));
    let power_report_path = existing_path(&ctx.project_dir.join("syn").join("power_report.txt"));
    let synth_report_path = existing_path(&ctx.project_dir.join("syn").join(format!("{}_native_synth_report.txt", ctx.module_name)))
        .or_else(|| existing_path(&ctx.project_dir.join("syn").join("synth_report.txt")));
    let formal_report_path = existing_path(&ctx.project_dir.join("formal").join("formal_report.txt"))
        .or_else(|| existing_path(&ctx.project_dir.join("formal").join("equiv_result.txt")));
    let formal_points_path = existing_path(&ctx.project_dir.join("formal").join("equiv_points.txt"));
    let report_md_path = existing_path(&ctx.project_dir.join("REPORT.md"));
    let report_rpt_path = existing_path(&ctx.project_dir.join("report").join("report.rpt"));
    let report_json_path = existing_path(&ctx.project_dir.join("report").join("report.json"));
    let detail_log_path = existing_path(&ctx.project_dir.join("logs").join("detail.log"));
    let conversation_path = existing_path(&ctx.project_dir.join("history").join("conversation.jsonl"));
    let apr_report_path = existing_path(&ctx.project_dir.join("apr").join("apr_report.txt"));
    let apr_layout_path = existing_path(&ctx.project_dir.join("exchange").join("apr_layout.tsv"));
    let apr_grid_path = existing_path(&ctx.project_dir.join("exchange").join("apr_grid.tsv"));
    let apr_netlist_path = existing_path(&ctx.project_dir.join("apr").join("apr_netlist.v"));
    let apr_final_def_path = existing_path(&ctx.project_dir.join("apr").join("final.def"));
    let apr_floorplan_def_path = existing_path(&ctx.project_dir.join("apr").join("floorplan.def"));
    let apr_gds_path = existing_path(&ctx.project_dir.join("apr").join("final.gds"));
    let apr_detail_route_path = existing_path(&ctx.project_dir.join("apr").join("detail_route.tsv"));
    let apr_timing_report_path = existing_path(&ctx.project_dir.join("apr").join("timing_report.txt"));
    let apr_power_report_path = existing_path(&ctx.project_dir.join("apr").join("power_report.txt"));
    let apr_area_report_path = existing_path(&ctx.project_dir.join("apr").join("area_report.txt"));
    let apr_drc_report_path = existing_path(&ctx.project_dir.join("apr").join("drc_report.txt"));
    let apr_lvs_report_path = existing_path(&ctx.project_dir.join("apr").join("lvs_report.txt"));
    let apr_dft_report_path = existing_path(&ctx.project_dir.join("apr").join("dft_report.txt"));
    let apr_report_json_path = existing_path(&ctx.project_dir.join("apr").join("apr_report.json"));
    let apr_prediction_path = existing_path(&ctx.project_dir.join("apr").join("llm_prediction.txt"));
    let apr_report_json = apr_report_json_path.as_ref()
        .and_then(|path| fs::read_to_string(path).ok())
        .and_then(|text| serde_json::from_str::<Value>(&text).ok());

    let report_json = report_json_path
        .as_ref()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|s| serde_json::from_str::<Value>(&s).ok());

    let sim_report = read_optional_text(&sim_report_path);
    let timing_report = read_optional_text(&timing_report_path)
        .or_else(|| json_string(&report_json, &["timing", "report"]));
    let power_report = read_optional_text(&power_report_path);
    let synth_report = read_optional_text(&synth_report_path);
    let formal_report = read_optional_text(&formal_report_path);
    let formal_points = read_optional_text(&formal_points_path);
    let report_rpt = read_optional_text(&report_rpt_path);
    let rtl_code = read_optional_text(&rtl_path);
    let gate_netlist = read_optional_text(&gate_path);
    let sim_vcd_path = preferred_waveform_path(&ctx.project_dir, &ctx.module_name, sim_report.as_deref());

    let cell_count = json_usize(&report_json, &["cell_count"])
        .or_else(|| gate_netlist.as_deref().map(estimate_gate_cell_count))
        .unwrap_or(0);
    let dff_count = json_usize(&report_json, &["dff_count"])
        .or_else(|| gate_netlist.as_deref().map(estimate_gate_dff_count))
        .unwrap_or(0);
    let wire_count = json_usize(&report_json, &["wire_count"]).unwrap_or(0);
    let port_count = json_usize(&report_json, &["port_count"]).unwrap_or(0);
    let area_ge = json_f64(&report_json, &["area_ge"]).unwrap_or(0.0);
    let area_um2 = json_f64(&report_json, &["area_um2"]).unwrap_or(0.0);
    let logic_depth = json_i64(&report_json, &["logic_depth"]).unwrap_or(0);
    let max_freq_mhz = json_f64(&report_json, &["max_freq_mhz"])
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Max frequency"))
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Clock frequency:"))
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Constraint frequency"))
        .unwrap_or(0.0);
    let timing_slack_ns = json_f64(&report_json, &["timing", "slack_ns"])
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Slack"))
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Slack:"))
        .unwrap_or(0.0);
    let timing_arrival_ns = json_f64(&report_json, &["timing", "arrival_time_ns"])
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Arrival Time"))
        .unwrap_or(0.0);
    let timing_required_ns = json_f64(&report_json, &["timing", "required_time_ns"])
        .or_else(|| parse_timing_scalar(timing_report.as_deref(), "Required Time"))
        .unwrap_or(0.0);
    let timing_met = json_bool(&report_json, &["timing", "timing_met"])
        .or_else(|| timing_report.as_deref().map(report_timing_met))
        .unwrap_or(false);
    let mut power_total_mw = json_f64(&report_json, &["power", "total_uw"]).unwrap_or(0.0) / 1000.0;
    let mut power_static_mw = json_f64(&report_json, &["power", "static_uw"]).unwrap_or(0.0) / 1000.0;
    let mut power_dynamic_mw = json_f64(&report_json, &["power", "dynamic_uw"]).unwrap_or(0.0) / 1000.0;

    let sim_passed = sim_report.as_ref().map(|r| r.contains("Status: PASS") || r.contains("Simulation: PASS")).unwrap_or(false);
    let sim_cycles = sim_report.as_ref().and_then(|r| extract_prefixed_int(r, "Time steps:")).unwrap_or(0);
    let formal_status = formal_report
        .as_ref()
        .map(|r| formal_status_from_report(r).to_string())
        .unwrap_or_else(|| "NOT_RUN".to_string());

    let area_exchange = build_area_exchange(
        cell_count,
        dff_count,
        wire_count,
        port_count,
        area_ge,
        area_um2,
        logic_depth,
        &report_json,
        synth_report.as_deref(),
    );
    let summary_exchange = build_summary_exchange(
        ctx,
        cell_count,
        dff_count,
        area_ge,
        max_freq_mhz,
        timing_slack_ns,
        timing_met,
        sim_passed,
        &formal_status,
        timing_report.as_deref(),
        formal_points.as_deref(),
    );
    let hierarchy_exchange = build_hierarchy_exchange(
        &ctx.project_dir,
        &ctx.module_name,
        area_ge,
        cell_count,
    );
    let timing_paths_exchange = build_timing_paths_exchange(timing_report.as_deref(), &report_json);
    let technology_exchange = build_technology_exchange(&ctx.technology_corners);
    let power_detail_json = json_string(&report_json, &["power", "detail"]);
    let power_corners_exchange = build_power_corners_exchange(
        &report_json,
        power_report
            .as_deref()
            .or(report_rpt.as_deref())
            .or(power_detail_json.as_deref()),
    );
    // The per-corner report is produced directly by the current power run,
    // while report.json is only rewritten after a completed full flow. Prefer
    // the current constraint-point row so a failed signoff run cannot leave
    // the power cards showing values from an older report than the chart.
    if let Some((static_uw, dynamic_uw, total_uw)) =
        power_card_from_corner_exchange(&power_corners_exchange, ctx.constraint_freq_mhz)
    {
        power_static_mw = static_uw / 1000.0;
        power_dynamic_mw = dynamic_uw / 1000.0;
        power_total_mw = total_uw / 1000.0;
    }
    let power_exchange = build_power_exchange(
        power_total_mw,
        power_static_mw,
        power_dynamic_mw,
        ctx.constraint_freq_mhz,
        report_rpt.as_deref(),
    );

    let rtl_exchange_path = write_exchange_file(&exchange_dir, "rtl_source.v", rtl_code.as_deref());
    let gate_exchange_path = write_exchange_file(&exchange_dir, "gate_netlist.v", gate_netlist.as_deref());
    let sim_exchange_path = write_exchange_file(&exchange_dir, "simulation_result.txt", sim_report.as_deref());
    let timing_exchange_path = write_exchange_file(&exchange_dir, "timing_result.txt", timing_report.as_deref());
    let formal_exchange_path = write_exchange_file(&exchange_dir, "formal_result.txt", formal_report.as_deref());
    let formal_points_exchange_path = write_exchange_file(&exchange_dir, "formal_points.txt", formal_points.as_deref());
    let area_exchange_path = write_exchange_file(&exchange_dir, "area_result.txt", Some(&area_exchange));
    let power_exchange_path = write_exchange_file(&exchange_dir, "power_result.txt", Some(&power_exchange));
    let summary_exchange_path = write_exchange_file(&exchange_dir, "summary_result.txt", Some(&summary_exchange));
    let hierarchy_exchange_path = write_exchange_file(&exchange_dir, "hierarchy_tree.tsv", Some(&hierarchy_exchange));
    let timing_paths_exchange_path = write_exchange_file(&exchange_dir, "timing_paths.tsv", Some(&timing_paths_exchange));
    let technology_exchange_path = write_exchange_file(&exchange_dir, "technologies.tsv", Some(&technology_exchange));
    let power_corners_exchange_path = exchange_dir.join("power_corners.tsv");
    // A synthesis/formal/simulation refresh has no per-corner power payload.
    // Do not erase a valid chart produced by the preceding timing/full-flow
    // step merely because that unrelated command refreshed GUI state.
    let power_corners_to_write = if has_power_corner_rows(&power_corners_exchange) {
        power_corners_exchange
    } else {
        fs::read_to_string(&power_corners_exchange_path)
            .ok()
            .filter(|existing| has_power_corner_rows(existing))
            .unwrap_or(power_corners_exchange)
    };
    fs::write(&power_corners_exchange_path, power_corners_to_write)
        .map_err(|e| format!("Failed to write {}: {}", power_corners_exchange_path.display(), e))?;

    let state_path = exchange_dir.join("gui_state.tcl");
    let mut state = String::new();
    state.push_str("unset -nocomplain ::gui_state\n");
    state.push_str("array set ::gui_state {}\n");
    set_string(&mut state, "version", "0.7.0");
    set_string(&mut state, "project_name", &ctx.project_name);
    set_string(&mut state, "project_dir", &ctx.project_dir.to_string_lossy());
    set_string(&mut state, "module_name", &ctx.module_name);
    set_string(&mut state, "modules", &ctx.modules.join("\n"));
    set_string(&mut state, "current_step", &ctx.current_step);
    set_string(&mut state, "step_status", &ctx.step_status);
    set_string(&mut state, "status_text", &ctx.status_text);
    set_string(&mut state, "last_error", &ctx.last_error);
    set_string(&mut state, "rtl_path", &path_string(&rtl_path));
    set_string(&mut state, "tb_path", &path_string(&tb_path));
    set_string(&mut state, "sdc_path", &path_string(&sdc_path));
    set_string(&mut state, "max_sdc_path", &path_string(&max_sdc_path));
    set_string(&mut state, "gate_path", &path_string(&gate_path));
    set_string(&mut state, "sim_report_path", &path_string(&sim_report_path));
    set_string(&mut state, "sim_vcd_path", &path_string(&sim_vcd_path));
    set_string(&mut state, "timing_report_path", &path_string(&timing_report_path));
    set_string(&mut state, "synth_report_path", &path_string(&synth_report_path));
    set_string(&mut state, "formal_report_path", &path_string(&formal_report_path));
    set_string(&mut state, "formal_points_path", &path_string(&formal_points_path));
    set_string(&mut state, "report_md_path", &path_string(&report_md_path));
    set_string(&mut state, "report_rpt_path", &path_string(&report_rpt_path));
    set_string(&mut state, "report_json_path", &path_string(&report_json_path));
    set_string(&mut state, "detail_log_path", &path_string(&detail_log_path));
    set_string(&mut state, "conversation_path", &path_string(&conversation_path));
    set_string(&mut state, "apr_report_path", &path_string(&apr_report_path));
    set_string(&mut state, "apr_layout_path", &path_string(&apr_layout_path));
    set_string(&mut state, "apr_grid_path", &path_string(&apr_grid_path));
    set_string(&mut state, "apr_netlist_path", &path_string(&apr_netlist_path));
    set_string(&mut state, "apr_final_def_path", &path_string(&apr_final_def_path));
    set_string(&mut state, "apr_floorplan_def_path", &path_string(&apr_floorplan_def_path));
    set_string(&mut state, "apr_gds_path", &path_string(&apr_gds_path));
    set_string(&mut state, "apr_detail_route_path", &path_string(&apr_detail_route_path));
    set_string(&mut state, "apr_timing_report_path", &path_string(&apr_timing_report_path));
    set_string(&mut state, "apr_power_report_path", &path_string(&apr_power_report_path));
    set_string(&mut state, "apr_area_report_path", &path_string(&apr_area_report_path));
    set_string(&mut state, "apr_drc_report_path", &path_string(&apr_drc_report_path));
    set_string(&mut state, "apr_lvs_report_path", &path_string(&apr_lvs_report_path));
    set_string(&mut state, "apr_dft_report_path", &path_string(&apr_dft_report_path));
    set_string(&mut state, "apr_report_json_path", &path_string(&apr_report_json_path));
    set_string(&mut state, "apr_prediction_path", &path_string(&apr_prediction_path));
    set_string(&mut state, "apr_signoff_status", apr_report_json.as_ref().and_then(|v| v.get("signoff_ready")).map(|v| if v.as_bool().unwrap_or(false) { "READY" } else { "BLOCKED" }).unwrap_or("NOT_RUN"));
    set_string(&mut state, "apr_drc_status", apr_report_json.as_ref().and_then(|v| v.get("drc_status")).and_then(|v| v.as_str()).unwrap_or("NOT_RUN"));
    set_string(&mut state, "apr_lvs_status", apr_report_json.as_ref().and_then(|v| v.get("lvs_status")).and_then(|v| v.as_str()).unwrap_or("NOT_RUN"));
    set_string(&mut state, "apr_dft_status", apr_report_json.as_ref().and_then(|v| v.get("dft_status")).and_then(|v| v.as_str()).unwrap_or("NOT_RUN"));
    set_string(&mut state, "apr_critical_route_source", apr_report_json.as_ref().and_then(|v| v.get("critical_route_source")).and_then(|v| v.as_str()).unwrap_or("not available"));
    for key in [
        "core_width_um", "core_height_um", "die_width_um", "die_height_um",
        "ir_drop_mv", "ir_worst_voltage_v", "ocv_late_slack_ns", "ocv_early_hold_slack_ns",
        "wns_ns", "tns_ns", "total_power_mw", "standard_cell_area_um2",
        "total_wire_length_um", "utilization", "violating_endpoints",
    ] {
        let value = apr_report_json.as_ref().and_then(|v| v.get(key)).and_then(|v| v.as_f64())
            .map(|v| format!("{v:.6}"))
            .unwrap_or_default();
        set_string(&mut state, &format!("apr_{key}"), &value);
    }
    for key in ["cells", "routes", "ir_grid", "critical_routes"] {
        let value = apr_report_json.as_ref().and_then(|v| v.get(key)).and_then(|v| v.as_array())
            .map(|v| v.len().to_string())
            .unwrap_or_default();
        set_string(&mut state, &format!("apr_{key}_count"), &value);
    }
    set_string(&mut state, "exchange_rtl_path", &rtl_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_gate_path", &gate_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_sim_path", &sim_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_timing_path", &timing_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_formal_path", &formal_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_formal_points_path", &formal_points_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_area_path", &area_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_power_path", &power_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_summary_path", &summary_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_hierarchy_path", &hierarchy_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_timing_paths_path", &timing_paths_exchange_path.to_string_lossy());
    set_string(&mut state, "exchange_technology_path", &technology_exchange_path.to_string_lossy());
    set_string(&mut state, "active_technology", &ctx.active_technology);
    set_int(&mut state, "opt_constprop", ctx.synthesis_options.constprop as i64);
    set_int(&mut state, "opt_dead_code_elimination", ctx.synthesis_options.dead_code_elimination as i64);
    set_int(&mut state, "opt_common_subexpression_elimination", ctx.synthesis_options.common_subexpression_elimination as i64);
    set_int(&mut state, "opt_expression_optimization", ctx.synthesis_options.expression_optimization as i64);
    set_int(&mut state, "opt_demorgan", ctx.synthesis_options.demorgan as i64);
    set_int(&mut state, "opt_width_reduction", ctx.synthesis_options.width_reduction as i64);
    set_int(&mut state, "opt_resource_sharing", ctx.synthesis_options.resource_sharing as i64);
    set_int(&mut state, "opt_fsm_extraction", ctx.synthesis_options.fsm_extraction as i64);
    set_int(&mut state, "opt_logic_minimization", ctx.synthesis_options.logic_minimization as i64);
    set_int(&mut state, "opt_retiming", ctx.synthesis_options.retiming as i64);
    set_int(&mut state, "opt_boundary_optimization", ctx.synthesis_options.boundary_optimization as i64);
    set_string(&mut state, "exchange_power_corners_path", &power_corners_exchange_path.to_string_lossy());
    set_int(&mut state, "constraint_freq_mhz", ctx.constraint_freq_mhz as i64);
    set_int(&mut state, "cell_count", cell_count as i64);
    set_int(&mut state, "dff_count", dff_count as i64);
    set_int(&mut state, "wire_count", wire_count as i64);
    set_int(&mut state, "port_count", port_count as i64);
    set_float(&mut state, "area_ge", area_ge);
    set_float(&mut state, "area_um2", area_um2);
    set_int(&mut state, "logic_depth", logic_depth);
    set_float(&mut state, "max_freq_mhz", max_freq_mhz);
    set_float(&mut state, "timing_slack_ns", timing_slack_ns);
    set_float(&mut state, "timing_arrival_ns", timing_arrival_ns);
    set_float(&mut state, "timing_required_ns", timing_required_ns);
    set_int(&mut state, "timing_met", if timing_met { 1 } else { 0 });
    set_float(&mut state, "power_total_mw", power_total_mw);
    set_float(&mut state, "power_static_mw", power_static_mw);
    set_float(&mut state, "power_dynamic_mw", power_dynamic_mw);
    set_string(&mut state, "formal_status", &formal_status);
    set_int(&mut state, "simulation_passed", if sim_passed { 1 } else { 0 });
    set_int(&mut state, "simulation_cycles", sim_cycles as i64);
    set_string(&mut state, "last_updated", &chrono_simple());

    fs::write(&state_path, state)
        .map_err(|e| format!("Failed to write GUI state: {}", e))?;

    Ok(GuiStateFiles { state_path })
}

pub fn event_line(state_path: &Path, step: &str, status: &str) -> String {
    format!(
        "@@GUI_EVENT\tSTATE\tpath_hex={}\tstep={}\tstatus={}",
        hex_encode(state_path.to_string_lossy().as_bytes()),
        sanitize_token(step),
        sanitize_token(status),
    )
}

fn preferred_source_path(project_dir: &Path, module_name: &str) -> Option<PathBuf> {
    let explicit = project_dir.join("src").join(format!("{}.v", module_name));
    if explicit.exists() {
        return Some(explicit);
    }
    let explicit_sv = project_dir.join("src").join(format!("{}.sv", module_name));
    if explicit_sv.exists() {
        return Some(explicit_sv);
    }
    let src_dir = project_dir.join("src");
    if let Ok(entries) = fs::read_dir(&src_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()).map(|e| e == "v" || e == "sv").unwrap_or(false) {
                return Some(path);
            }
        }
    }
    None
}

fn existing_path(path: &Path) -> Option<PathBuf> {
    if path.exists() {
        Some(path.to_path_buf())
    } else {
        None
    }
}

fn read_optional_text(path: &Option<PathBuf>) -> Option<String> {
    path.as_ref().and_then(|p| fs::read_to_string(p).ok())
}

fn preferred_waveform_path(project_dir: &Path, module_name: &str, sim_report: Option<&str>) -> Option<PathBuf> {
    let sim_dir = project_dir.join("sim");
    let canonical = sim_dir.join(format!("{}_tb.vcd", module_name));
    if canonical.exists() {
        return Some(canonical);
    }

    if let Ok(entries) = fs::read_dir(&sim_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|ext| ext.to_str()) == Some("vcd") {
                if path != canonical && fs::copy(&path, &canonical).is_ok() {
                    return Some(canonical.clone());
                }
                return Some(path);
            }
        }
    }

    let source_path = sim_report.and_then(extract_waveform_path)?;
    let source_path = PathBuf::from(source_path);
    if !source_path.exists() {
        return None;
    }
    if fs::copy(&source_path, &canonical).is_ok() {
        Some(canonical)
    } else {
        Some(source_path)
    }
}

fn extract_waveform_path(report: &str) -> Option<String> {
    report.lines().find_map(|line| {
        let trimmed = line.trim();
        trimmed
            .strip_prefix("Waveform: VCD saved to ")
            .map(|rest| rest.trim().to_string())
    })
}

fn extract_prefixed_int(text: &str, prefix: &str) -> Option<i64> {
    text.lines()
        .find_map(|line| {
            let trimmed = line.trim();
            if let Some(rest) = trimmed.strip_prefix(prefix) {
                rest.trim().split_whitespace().next()?.parse::<i64>().ok()
            } else {
                None
            }
        })
}

fn build_area_exchange(
    cell_count: usize,
    dff_count: usize,
    wire_count: usize,
    port_count: usize,
    area_ge: f64,
    area_um2: f64,
    logic_depth: i64,
    report_json: &Option<Value>,
    synth_report: Option<&str>,
) -> String {
    let mut text = String::new();
    text.push_str("Area Analysis\n");
    text.push_str("==============================\n");
    text.push_str(&format!("Total Cells : {}\n", cell_count));
    text.push_str(&format!("DFF Count   : {}\n", dff_count));
    text.push_str(&format!("Wire Count  : {}\n", wire_count));
    text.push_str(&format!("Port Count  : {}\n", port_count));
    text.push_str(&format!("Area (um^2) : {:.3}\n", area_um2));
    text.push_str(&format!("GE Reference : {:.3}\n", area_ge));
    text.push_str(&format!("Logic Depth : {}\n", logic_depth));
    if let Some(cells) = report_json
        .as_ref()
        .and_then(|v| v.get("cells"))
        .and_then(|v| v.as_array())
    {
        text.push_str("\nCell Breakdown\n");
        text.push_str("------------------------------\n");
        for cell in cells {
            let cell_type = cell.get("type").and_then(|v| v.as_str()).unwrap_or("unknown");
            let count = cell.get("count").and_then(|v| v.as_u64()).unwrap_or(0);
            let total = cell.get("total_area_um2").and_then(|v| v.as_f64()).unwrap_or(0.0);
            text.push_str(&format!("{:<20} {:>6}  {:>10.3} um^2\n", cell_type, count, total));
        }
    }
    if let Some(report) = synth_report {
        text.push_str("\nSynthesis Report\n");
        text.push_str("------------------------------\n");
        text.push_str(report);
        if !report.ends_with('\n') {
            text.push('\n');
        }
    }
    text
}

fn build_power_exchange(
    total_mw: f64,
    static_mw: f64,
    dynamic_mw: f64,
    freq_mhz: i32,
    report_rpt: Option<&str>,
) -> String {
    let mut text = String::new();
    text.push_str("Power Analysis\n");
    text.push_str("==============================\n");
    text.push_str(&format!("Constraint MHz : {}\n", freq_mhz));
    text.push_str(&format!("Total Power    : {:.6} mW\n", total_mw));
    text.push_str(&format!("Static Power   : {:.6} mW\n", static_mw));
    text.push_str(&format!("Dynamic Power  : {:.6} mW\n", dynamic_mw));

    if let Some(rpt) = report_rpt {
        if let Some(section) = extract_section(rpt, "--- POWER REPORT ---", "--- VERIFICATION STATUS ---") {
            text.push_str("\nDetailed Report\n");
            text.push_str("------------------------------\n");
            text.push_str(&section);
            if !section.ends_with('\n') {
                text.push('\n');
            }
        }
    }
    text
}

fn build_summary_exchange(
    ctx: &GuiSyncContext,
    cell_count: usize,
    dff_count: usize,
    area_ge: f64,
    max_freq_mhz: f64,
    slack_ns: f64,
    timing_met: bool,
    sim_passed: bool,
    formal_status: &str,
    timing_report: Option<&str>,
    formal_points: Option<&str>,
) -> String {
    let mut text = String::new();
    text.push_str("Project Summary\n");
    text.push_str("==============================\n");
    text.push_str(&format!("Project       : {}\n", ctx.project_name));
    text.push_str(&format!("Module        : {}\n", ctx.module_name));
    text.push_str(&format!("Current Step  : {}\n", ctx.current_step));
    text.push_str(&format!("Step Status   : {}\n", ctx.step_status));
    text.push_str(&format!("Constraint    : {} MHz\n", ctx.constraint_freq_mhz));
    text.push_str(&format!("Cells / DFF   : {} / {}\n", cell_count, dff_count));
    text.push_str(&format!("Area          : {:.3} GE\n", area_ge));
    text.push_str(&format!("Max Freq      : {:.3} MHz\n", max_freq_mhz));
    text.push_str(&format!("Slack         : {:.3} ns ({})\n", slack_ns, if timing_met { "MET" } else { "VIO" }));
    text.push_str(&format!("Simulation    : {}\n", if sim_passed { "PASS" } else { "NOT_PASS" }));
    text.push_str(&format!("Formal        : {}\n", formal_status));
    if !ctx.status_text.is_empty() {
        text.push_str(&format!("Status Text   : {}\n", ctx.status_text));
    }
    if !ctx.last_error.is_empty() {
        text.push_str(&format!("Last Error    : {}\n", ctx.last_error));
    }

    if let Some(report) = timing_report {
        let critical_paths = parse_timing_paths_from_report(report);
        if critical_paths.iter().any(|path| path.available) {
            text.push_str("\nTop 5 Critical Paths\n");
            text.push_str("------------------------------\n");
            for path in critical_paths.iter().filter(|path| path.available).take(5) {
                text.push_str(&format!(
                    "#{:<2} {:<20} -> {:<20} delay {:>7.3} ns  slack {:>7.3} ns  stages {}\n",
                    path.index,
                    shorten_text(&path.startpoint, 20),
                    shorten_text(&path.endpoint, 20),
                    path.delay_ns,
                    path.slack_ns,
                    path.stages.len()
                ));
            }
        }
    }

    if let Some(points) = formal_points {
        let extracted = extract_formal_point_lines(points, 10);
        if !extracted.is_empty() {
            text.push_str("\nFormal Equivalence Points\n");
            text.push_str("------------------------------\n");
            for line in extracted {
                text.push_str(&line);
                text.push('\n');
            }
        }
    }
    text
}

fn parse_timing_scalar(report: Option<&str>, key: &str) -> Option<f64> {
    let report = report?;
    for line in report.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix(key) {
            return extract_first_f64(rest);
        }
        if trimmed.starts_with(key) {
            return extract_first_f64(trimmed);
        }
    }
    None
}

fn extract_first_f64(text: &str) -> Option<f64> {
    let mut buf = String::new();
    let mut seen = false;
    for ch in text.chars() {
        if ch.is_ascii_digit() || ch == '.' || (ch == '-' && !seen) {
            buf.push(ch);
            seen = true;
        } else if seen {
            break;
        }
    }
    if buf.is_empty() || buf == "-" {
        None
    } else {
        buf.parse::<f64>().ok()
    }
}

fn report_timing_met(report: &str) -> bool {
    report
        .lines()
        .find(|line| line.contains("Timing status"))
        .map(|line| line.contains("MET"))
        .unwrap_or_else(|| report.contains("Slack:") && !report.contains("VIOLATED"))
}

fn estimate_gate_cell_count(netlist: &str) -> usize {
    gate_instance_lines(netlist).count()
}

fn estimate_gate_dff_count(netlist: &str) -> usize {
    gate_instance_lines(netlist)
        .filter(|line| line.to_ascii_uppercase().contains("DFF") || line.to_ascii_uppercase().contains("LATCH"))
        .count()
}

fn gate_instance_lines(netlist: &str) -> impl Iterator<Item = &str> {
    netlist.lines().map(str::trim).filter(|line| {
        !line.is_empty()
            && !line.starts_with("//")
            && !line.starts_with("module ")
            && !line.starts_with("input ")
            && !line.starts_with("output ")
            && !line.starts_with("wire ")
            && !line.starts_with("reg ")
            && !line.starts_with("assign ")
            && !line.starts_with("always ")
            && !line.starts_with("initial ")
            && !line.starts_with("endmodule")
            && line.contains('(')
            && line.ends_with(");")
    })
}

fn extract_formal_point_lines(points: &str, limit: usize) -> Vec<String> {
    let mut out = Vec::new();
    let mut keep = false;
    for line in points.lines() {
        let trimmed = line.trim();
        if trimmed == "Output Equivalence Points:" || trimmed == "Sequential Observation Points:" {
            keep = true;
            continue;
        }
        if trimmed.ends_with(':') {
            keep = false;
            continue;
        }
        if keep && trimmed.starts_with('-') {
            out.push(trimmed.to_string());
            if out.len() >= limit {
                break;
            }
        }
    }
    out
}

fn shorten_text(text: &str, max_chars: usize) -> String {
    let mut chars = text.chars();
    let shortened: String = chars.by_ref().take(max_chars).collect();
    if chars.next().is_some() && max_chars >= 3 {
        format!("{}...", shortened.chars().take(max_chars - 3).collect::<String>())
    } else {
        shortened
    }
}

fn build_hierarchy_exchange(
    project_dir: &Path,
    top_module: &str,
    total_area_ge: f64,
    total_cells: usize,
) -> String {
    let modules = read_project_modules(project_dir);
    if modules.is_empty() {
        return format!(
            "depth\tpath\tmodule\tarea_ge\tcells\tweight\n0\t{}\t{}\t{:.6}\t{}\t1.000000\n",
            top_module,
            top_module,
            total_area_ge.max(0.0),
            total_cells
        );
    }

    let root = if modules.contains_key(top_module) {
        top_module.to_string()
    } else {
        modules.keys().next().cloned().unwrap_or_else(|| top_module.to_string())
    };

    let mut memo = HashMap::new();
    let mut rows = vec!["depth\tpath\tmodule\tarea_ge\tcells\tweight".to_string()];
    append_hierarchy_rows(
        &root,
        &root,
        0,
        total_area_ge.max(0.0),
        total_cells,
        &modules,
        &mut memo,
        &mut rows,
        &mut HashSet::new(),
    );
    rows.join("\n") + "\n"
}

fn tsv_field(value: &str) -> String {
    value.replace(['\t', '\r', '\n'], " ")
}

fn build_technology_exchange(corners: &[GuiTechnologyCorner]) -> String {
    let mut rows = vec!["technology\tcorner\ttype\tlibrary\tvoltage_v\ttemperature_c\tcells\ttime_unit\tvoltage_unit\tleakage_power_unit\tcapacitance_unit\tsynthesis".to_string()];
    for corner in corners {
        rows.push(format!("{}\t{}\t{}\t{}\t{:.6}\t{:.3}\t{}\t{}\t{}\t{}\t{}\t{}",
            tsv_field(&corner.technology),
            tsv_field(&corner.corner),
            tsv_field(&corner.corner_type),
            tsv_field(&corner.library),
            corner.voltage,
            corner.temperature,
            corner.cells,
            tsv_field(&corner.time_unit),
            tsv_field(&corner.voltage_unit),
            tsv_field(&corner.leakage_power_unit),
            tsv_field(&corner.capacitance_unit),
            if corner.is_synthesis { 1 } else { 0 },
        ));
    }
    rows.push(String::new());
    rows.join("\n")
}

fn append_power_corner_rows_from_json(rows: &mut Vec<String>, report_json: &Option<Value>, key: &str, label: &str) {
    let Some(entries) = report_json
        .as_ref()
        .and_then(|v| v.get("power"))
        .and_then(|v| v.get(key))
        .and_then(|v| v.as_array())
    else {
        return;
    };

    for entry in entries {
        let frequency = entry.get("frequency_mhz").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let corner = entry.get("corner").and_then(|v| v.as_str()).unwrap_or("unknown");
        let corner_type = entry.get("type").and_then(|v| v.as_str()).unwrap_or("NA");
        let voltage = entry.get("voltage").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let static_uw = entry.get("static_uw").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let dynamic_uw = entry.get("dynamic_uw").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let total_uw = entry.get("total_uw").and_then(|v| v.as_f64()).unwrap_or(0.0);
        rows.push(format!(
            "{}\t{:.3}\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}",
            tsv_field(label),
            frequency,
            tsv_field(corner),
            tsv_field(corner_type),
            voltage,
            static_uw,
            dynamic_uw,
            total_uw,
        ));
    }
}

fn build_power_corners_exchange(report_json: &Option<Value>, detail: Option<&str>) -> String {
    let mut rows = vec!["analysis\tfrequency_mhz\tcorner\ttype\tvoltage_v\tstatic_uw\tdynamic_uw\ttotal_uw".to_string()];
    // syn/power_report.txt is produced by the operation currently running.
    // report.json is only finalized after a successful full flow, so stale JSON
    // must never overwrite fresh per-corner power data after a failed signoff.
    let mut analysis = String::new();
    let mut frequency = 0.0;
    for line in detail.unwrap_or_default().lines() {
        let trimmed = line.trim();
        if let Some(open) = trimmed.rfind('(') {
            if trimmed.ends_with("MHz)") {
                analysis = trimmed[..open].trim().to_string();
                frequency = trimmed[open + 1..trimmed.len() - 1]
                    .trim_end_matches("MHz").trim().parse().unwrap_or(0.0);
                continue;
            }
        }
        let fields: Vec<&str> = trimmed.split_whitespace().collect();
        // Reports produced by the native NLDM analyzer optionally append a
        // source column (for example, "NLDM").  Older reports omit it, so
        // accept both forms while keeping the numeric columns authoritative.
        if fields.len() < 6 || analysis.is_empty() || fields[0] == "Corner" || fields[0].starts_with('-') {
            continue;
        }
        let corner_type = fields[1].to_ascii_uppercase();
        if !matches!(corner_type.as_str(), "TT" | "FF" | "SS" | "FS" | "SF") || !fields[2].ends_with('V') {
            continue;
        }
        let voltage = fields[2].trim_end_matches('V').parse::<f64>();
        let static_uw = fields[3].parse::<f64>();
        let dynamic_uw = fields[4].parse::<f64>();
        let total_uw = fields[5].parse::<f64>();
        if let (Ok(voltage), Ok(static_uw), Ok(dynamic_uw), Ok(total_uw)) =
            (voltage, static_uw, dynamic_uw, total_uw)
        {
            rows.push(format!("{}\t{:.3}\t{}\t{}\t{:.6}\t{:.6}\t{:.6}\t{:.6}",
                tsv_field(&analysis), frequency, tsv_field(fields[0]), tsv_field(fields[1]),
                voltage, static_uw, dynamic_uw, total_uw));
        }
    }
    if rows.len() == 1 {
        append_power_corner_rows_from_json(
            &mut rows,
            report_json,
            "constraint_corner_results",
            "Multi-Corner Power Analysis",
        );
        append_power_corner_rows_from_json(
            &mut rows,
            report_json,
            "max_corner_results",
            "Max Frequency Power",
        );
    }
    rows.push(String::new());
    rows.join("\n")
}

fn has_power_corner_rows(content: &str) -> bool {
    content.lines().skip(1).any(|line| !line.trim().is_empty())
}

/// Return the displayed power-card values from the current constraint table.
/// The nominal TT row is the synthesis/display default; if it is unavailable,
/// retain the first row at the requested operating point.
fn power_card_from_corner_exchange(
    content: &str,
    constraint_freq_mhz: i32,
) -> Option<(f64, f64, f64)> {
    let mut first_row = None;
    let mut constraint_row = None;
    for line in content.lines().skip(1) {
        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() < 8 {
            continue;
        }
        let (Ok(frequency), Ok(static_uw), Ok(dynamic_uw), Ok(total_uw)) = (
            fields[1].parse::<f64>(),
            fields[5].parse::<f64>(),
            fields[6].parse::<f64>(),
            fields[7].parse::<f64>(),
        ) else {
            continue;
        };
        let row = (static_uw, dynamic_uw, total_uw);
        if first_row.is_none() {
            first_row = Some(row);
        }
        if fields[0] == "Multi-Corner Power Analysis"
            && (frequency - constraint_freq_mhz as f64).abs() < 0.001
        {
            if fields[3] == "TT" {
                return Some(row);
            }
            if constraint_row.is_none() {
                constraint_row = Some(row);
            }
        }
    }
    constraint_row.or(first_row)
}

#[derive(Clone, Debug, Default)]
struct GuiTimingStage {
    name: String,
    kind: String,
    incr_delay_ns: f64,
    cumul_delay_ns: f64,
}

#[derive(Clone, Debug, Default)]
struct GuiTimingPath {
    index: usize,
    available: bool,
    met: bool,
    startpoint: String,
    endpoint: String,
    delay_ns: f64,
    slack_ns: f64,
    stages: Vec<GuiTimingStage>,
}

fn build_timing_paths_exchange(timing_report: Option<&str>, report_json: &Option<Value>) -> String {
    let mut out = String::from(
        "path_index\tavailable\tmet\tstartpoint\tendpoint\tdelay_ns\tslack_ns\tstage_count\tstage_index\tstage_name\tstage_type\tincr_delay_ns\tcumul_delay_ns\n"
    );
    let mut parsed = timing_report
        .map(parse_timing_paths_from_report)
        .unwrap_or_default();
    if parsed.is_empty() {
        parsed = timing_paths_from_json(report_json);
    }
    if parsed.is_empty() {
        parsed = (1..=10)
            .map(|index| GuiTimingPath {
                index,
                available: false,
                startpoint: "(unavailable)".to_string(),
                endpoint: "(unavailable)".to_string(),
                ..Default::default()
            })
            .collect();
    }
    while parsed.len() < 10 {
        let index = parsed.len() + 1;
        parsed.push(GuiTimingPath {
            index,
            available: false,
            startpoint: "(unavailable)".to_string(),
            endpoint: "(unavailable)".to_string(),
            ..Default::default()
        });
    }

    for path in parsed.iter().take(10) {
        if path.stages.is_empty() {
            out.push_str(&format!(
                "{}\t{}\t{}\t{}\t{}\t{:.6}\t{:.6}\t0\t-1\t\t\t0.000000\t0.000000\n",
                path.index,
                bool_flag(path.available),
                bool_flag(path.met),
                tsv_escape(&path.startpoint),
                tsv_escape(&path.endpoint),
                path.delay_ns,
                path.slack_ns
            ));
            continue;
        }
        for (stage_index, stage) in path.stages.iter().enumerate() {
            out.push_str(&format!(
                "{}\t{}\t{}\t{}\t{}\t{:.6}\t{:.6}\t{}\t{}\t{}\t{}\t{:.6}\t{:.6}\n",
                path.index,
                bool_flag(path.available),
                bool_flag(path.met),
                tsv_escape(&path.startpoint),
                tsv_escape(&path.endpoint),
                path.delay_ns,
                path.slack_ns,
                path.stages.len(),
                stage_index,
                tsv_escape(&stage.name),
                tsv_escape(&stage.kind),
                stage.incr_delay_ns,
                stage.cumul_delay_ns
            ));
        }
    }

    out
}

fn timing_paths_from_json(report_json: &Option<Value>) -> Vec<GuiTimingPath> {
    let Some(paths) = report_json
        .as_ref()
        .and_then(|v| v.get("timing"))
        .and_then(|v| v.get("critical_paths"))
        .and_then(|v| v.as_array())
    else {
        return Vec::new();
    };

    let mut out = Vec::new();
    for path in paths {
        let stages = path
            .get("stages")
            .and_then(|v| v.as_array())
            .cloned()
            .unwrap_or_default();
        let stages = stages
            .iter()
            .map(|stage| GuiTimingStage {
                name: stage.get("cell_name").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                kind: stage.get("cell_type").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                incr_delay_ns: stage.get("incr_delay_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
                cumul_delay_ns: stage.get("cumul_delay_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
            })
            .collect::<Vec<_>>();
        out.push(GuiTimingPath {
            index: path.get("index").and_then(|v| v.as_u64()).unwrap_or(0) as usize,
            available: path.get("available").and_then(|v| v.as_bool()).unwrap_or(false),
            met: path.get("met").and_then(|v| v.as_bool()).unwrap_or(false),
            startpoint: path.get("startpoint").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            endpoint: path.get("endpoint").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            delay_ns: path.get("delay_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
            slack_ns: path.get("slack_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
            stages,
        });
    }
    out
}

fn parse_timing_paths_from_report(report: &str) -> Vec<GuiTimingPath> {
    let mut paths = Vec::new();
    let mut current: Option<GuiTimingPath> = None;
    let mut in_stages = false;

    for line in report.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("--- Path #") {
            if let Some(path) = current.take() {
                paths.push(path);
            }
            let index = rest
                .trim_end_matches("---")
                .trim()
                .parse::<usize>()
                .unwrap_or(paths.len() + 1);
            current = Some(GuiTimingPath {
                index,
                available: true,
                ..Default::default()
            });
            in_stages = false;
            continue;
        }
        let Some(path) = current.as_mut() else {
            continue;
        };
        if let Some(rest) = trimmed.strip_prefix("Start:") {
            path.startpoint = rest.trim().to_string();
            if path.startpoint == "(unavailable)" {
                path.available = false;
            }
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("End:") {
            path.endpoint = rest.trim().to_string();
            if path.endpoint == "(unavailable)" {
                path.available = false;
            }
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("Slack:") {
            let token = rest.split_whitespace().next().unwrap_or_default();
            if let Ok(slack) = token.parse::<f64>() {
                path.slack_ns = slack;
                path.met = rest.contains("(MET)");
            } else {
                path.available = false;
            }
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("Total delay:") {
            let token = rest.split_whitespace().next().unwrap_or_default();
            if let Ok(delay) = token.parse::<f64>() {
                path.delay_ns = delay;
            } else {
                path.available = false;
            }
            continue;
        }
        if trimmed == "Stages:" {
            in_stages = true;
            continue;
        }
        if !in_stages || trimmed.is_empty() || trimmed.starts_with("Cell") || trimmed.starts_with("---") {
            continue;
        }
        if trimmed.starts_with("No additional critical paths") {
            path.available = false;
            continue;
        }
        let cols: Vec<&str> = trimmed.split_whitespace().collect();
        if cols.len() < 4 {
            continue;
        }
        path.stages.push(GuiTimingStage {
            name: cols[..cols.len() - 3].join(" "),
            kind: cols[cols.len() - 3].to_string(),
            incr_delay_ns: cols[cols.len() - 2].parse::<f64>().unwrap_or(0.0),
            cumul_delay_ns: cols[cols.len() - 1].parse::<f64>().unwrap_or(0.0),
        });
    }
    if let Some(path) = current.take() {
        paths.push(path);
    }
    paths
}

#[derive(Clone, Debug)]
struct ModuleInfo {
    base_weight: f64,
    children: Vec<ModuleInstance>,
}

#[derive(Clone, Debug)]
struct ModuleInstance {
    module_name: String,
    instance_name: String,
}

fn read_project_modules(project_dir: &Path) -> HashMap<String, ModuleInfo> {
    let src_dir = project_dir.join("src");
    let mut blocks = Vec::new();
    if let Ok(entries) = fs::read_dir(&src_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            let ext = path.extension().and_then(|e| e.to_str()).unwrap_or_default();
            if ext != "v" && ext != "sv" {
                continue;
            }
            if let Ok(content) = fs::read_to_string(&path) {
                blocks.extend(extract_module_blocks(&content));
            }
        }
    }

    let known: HashSet<String> = blocks.iter().map(|(name, _)| name.clone()).collect();
    let mut modules = HashMap::new();
    for (name, body) in blocks {
        let children = parse_module_instances(&body, &known);
        let base_weight = estimate_module_weight(&body, children.len());
        modules.insert(name, ModuleInfo { base_weight, children });
    }
    modules
}

fn extract_module_blocks(text: &str) -> Vec<(String, String)> {
    let mut blocks = Vec::new();
    let mut offset = 0usize;
    while let Some(start_rel) = text[offset..].find("module") {
        let start = offset + start_rel;
        let after = text.get(start + "module".len()..).unwrap_or_default();
        if !after.chars().next().map(|c| c.is_whitespace()).unwrap_or(false) {
            offset = start + "module".len();
            continue;
        }
        let Some(end_rel) = text[start..].find("endmodule") else {
            break;
        };
        let end = start + end_rel + "endmodule".len();
        let block = text[start..end].to_string();
        if let Some(name) = parse_module_name(&block) {
            blocks.push((name, block));
        }
        offset = end;
    }
    blocks
}

fn parse_module_name(block: &str) -> Option<String> {
    let rest = block.strip_prefix("module")?.trim_start();
    let name: String = rest
        .chars()
        .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
        .collect();
    if name.is_empty() { None } else { Some(name) }
}

fn bool_flag(value: bool) -> &'static str {
    if value { "1" } else { "0" }
}

fn tsv_escape(value: &str) -> String {
    value.replace('\t', " ").replace('\n', " ").replace('\r', " ").trim().to_string()
}

fn parse_module_instances(body: &str, known: &HashSet<String>) -> Vec<ModuleInstance> {
    let mut instances = Vec::new();
    let normalized = body.replace('\n', " ").replace('\t', " ");
    for raw_stmt in normalized.split(';') {
        let stmt = raw_stmt.split("//").next().unwrap_or_default().trim();
        if stmt.is_empty() || is_non_instance_statement(stmt) || !stmt.contains('(') {
            continue;
        }
        if let Some(instance) = parse_instance_statement(stmt, known) {
            instances.push(instance);
        }
    }
    instances
}

fn is_non_instance_statement(stmt: &str) -> bool {
    [
        "module",
        "endmodule",
        "input",
        "output",
        "inout",
        "wire",
        "reg",
        "logic",
        "assign",
        "always",
        "initial",
        "if",
        "else",
        "case",
        "for",
        "while",
        "generate",
        "endgenerate",
        "begin",
        "end",
        "parameter",
        "localparam",
        "typedef",
    ]
    .iter()
    .any(|kw| stmt.starts_with(kw))
}

fn parse_instance_statement(stmt: &str, known: &HashSet<String>) -> Option<ModuleInstance> {
    let mut candidates: Vec<&String> = known.iter().filter(|name| stmt.starts_with(name.as_str())).collect();
    candidates.sort_by_key(|name| std::cmp::Reverse(name.len()));

    for module_name in candidates {
        let rest = stmt.get(module_name.len()..)?.trim_start();
        if !rest.starts_with(|c: char| c.is_whitespace() || c == '#') {
            continue;
        }
        let rest = strip_parameter_block(rest);
        let instance_name: String = rest
            .chars()
            .skip_while(|c| c.is_whitespace())
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
            .collect();
        if instance_name.is_empty() {
            continue;
        }
        return Some(ModuleInstance {
            module_name: module_name.clone(),
            instance_name,
        });
    }
    None
}

fn strip_parameter_block(rest: &str) -> &str {
    let trimmed = rest.trim_start();
    if !trimmed.starts_with('#') {
        return trimmed;
    }
    let mut depth = 0i32;
    let mut seen_open = false;
    for (idx, ch) in trimmed.char_indices() {
        if ch == '(' {
            depth += 1;
            seen_open = true;
        } else if ch == ')' && seen_open {
            depth -= 1;
            if depth == 0 {
                return trimmed[idx + 1..].trim_start();
            }
        }
    }
    trimmed
}

fn estimate_module_weight(body: &str, child_count: usize) -> f64 {
    let mut score = 1.0 + child_count as f64;
    for token in ["assign", "always", "if", "case", "for", "wire", "reg"] {
        score += body.matches(token).count() as f64 * 0.5;
    }
    score.max(1.0)
}

fn subtree_weight(
    module_name: &str,
    modules: &HashMap<String, ModuleInfo>,
    memo: &mut HashMap<String, f64>,
    stack: &mut HashSet<String>,
) -> f64 {
    if let Some(weight) = memo.get(module_name) {
        return *weight;
    }
    let Some(info) = modules.get(module_name) else {
        return 1.0;
    };
    if !stack.insert(module_name.to_string()) {
        return info.base_weight.max(1.0);
    }

    let mut total = info.base_weight.max(1.0);
    for child in &info.children {
        total += subtree_weight(&child.module_name, modules, memo, stack).max(1.0);
    }

    stack.remove(module_name);
    memo.insert(module_name.to_string(), total);
    total
}

fn append_hierarchy_rows(
    module_name: &str,
    path: &str,
    depth: usize,
    area_ge: f64,
    cells: usize,
    modules: &HashMap<String, ModuleInfo>,
    memo: &mut HashMap<String, f64>,
    rows: &mut Vec<String>,
    stack: &mut HashSet<String>,
) {
    let weight = subtree_weight(module_name, modules, memo, &mut HashSet::new()).max(1.0);
    rows.push(format!(
        "{}\t{}\t{}\t{:.6}\t{}\t{:.6}",
        depth, path, module_name, area_ge.max(0.0), cells, weight
    ));

    let Some(info) = modules.get(module_name) else {
        return;
    };
    if info.children.is_empty() || !stack.insert(path.to_string()) {
        return;
    }

    let child_weights: Vec<f64> = info
        .children
        .iter()
        .map(|child| subtree_weight(&child.module_name, modules, memo, &mut HashSet::new()).max(1.0))
        .collect();
    let total_child_weight: f64 = child_weights.iter().sum();
    if total_child_weight <= 0.0 {
        stack.remove(path);
        return;
    }

    for (idx, child) in info.children.iter().enumerate() {
        let share = child_weights[idx] / total_child_weight;
        let child_area = area_ge * share;
        let child_cells = ((cells as f64) * share).round() as usize;
        let child_path = format!("{}/{}", path, child.instance_name);
        append_hierarchy_rows(
            &child.module_name,
            &child_path,
            depth + 1,
            child_area,
            child_cells,
            modules,
            memo,
            rows,
            stack,
        );
    }

    stack.remove(path);
}

fn extract_section(text: &str, start_marker: &str, end_marker: &str) -> Option<String> {
    let start = text.find(start_marker)?;
    let rest = &text[start..];
    let end = rest.find(end_marker).unwrap_or(rest.len());
    Some(rest[..end].trim().to_string())
}

fn write_exchange_file(exchange_dir: &Path, name: &str, content: Option<&str>) -> PathBuf {
    let path = exchange_dir.join(name);
    let _ = fs::write(&path, content.unwrap_or(""));
    path
}

fn set_string(out: &mut String, key: &str, value: &str) {
    out.push_str(&format!(
        "set ::gui_state({}) [binary decode hex {{{}}}]\n",
        key,
        hex_encode(value.as_bytes())
    ));
}

fn set_int(out: &mut String, key: &str, value: i64) {
    out.push_str(&format!("set ::gui_state({}) {}\n", key, value));
}

fn set_float(out: &mut String, key: &str, value: f64) {
    out.push_str(&format!("set ::gui_state({}) {:.12}\n", key, value));
}

fn path_string(path: &Option<PathBuf>) -> String {
    path.as_ref()
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_default()
}

fn json_f64(root: &Option<Value>, path: &[&str]) -> Option<f64> {
    let mut current = root.as_ref()?;
    for key in path {
        current = current.get(*key)?;
    }
    current.as_f64()
}

fn json_i64(root: &Option<Value>, path: &[&str]) -> Option<i64> {
    let mut current = root.as_ref()?;
    for key in path {
        current = current.get(*key)?;
    }
    current.as_i64()
}

fn json_usize(root: &Option<Value>, path: &[&str]) -> Option<usize> {
    let mut current = root.as_ref()?;
    for key in path {
        current = current.get(*key)?;
    }
    current.as_u64().map(|v| v as usize)
}

fn json_bool(root: &Option<Value>, path: &[&str]) -> Option<bool> {
    let mut current = root.as_ref()?;
    for key in path {
        current = current.get(*key)?;
    }
    current.as_bool()
}

fn json_string(root: &Option<Value>, path: &[&str]) -> Option<String> {
    let mut current = root.as_ref()?;
    for key in path {
        current = current.get(*key)?;
    }
    current.as_str().map(|s| s.to_string())
}

fn sanitize_token(token: &str) -> String {
    token
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' })
        .collect()
}

pub fn hex_encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(nibble_to_hex((b >> 4) & 0x0f));
        out.push(nibble_to_hex(b & 0x0f));
    }
    out
}

fn chrono_simple() -> String {
    unsafe {
        let mut now: libc::time_t = 0;
        libc::time(&mut now as *mut libc::time_t);
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&now as *const libc::time_t, &mut tm as *mut libc::tm).is_null() {
            return "1970-01-01".to_string();
        }
        format!("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)
    }
}

fn nibble_to_hex(n: u8) -> char {
    match n {
        0..=9 => (b'0' + n) as char,
        _ => (b'a' + (n - 10)) as char,
    }
}

#[cfg(test)]
mod tests {
    use super::{build_power_corners_exchange, power_card_from_corner_exchange};
    use serde_json::json;

    #[test]
    fn power_card_prefers_current_constraint_corner_values() {
        let tsv = concat!(
            "analysis\tfrequency_mhz\tcorner\ttype\tvoltage_v\tstatic_uw\tdynamic_uw\ttotal_uw\n",
            "Max Frequency Power\t1135.000\ttt\tTT\t1.200000\t74.300000\t3795.900000\t5009.000000\n",
            "Multi-Corner Power Analysis\t100.000\ttt\tTT\t1.200000\t74.300000\t334.400000\t509.100000\n",
        );

        assert_eq!(
            power_card_from_corner_exchange(tsv, 100),
            Some((74.3, 334.4, 509.1))
        );
    }

    #[test]
    fn current_power_detail_overrides_stale_report_json() {
        let stale_json = Some(json!({
            "power": {
                "constraint_corner_results": [{
                    "frequency_mhz": 100.0,
                    "corner": "typ_tt_1p2_25",
                    "type": "TT",
                    "voltage": 1.2,
                    "static_uw": 74.3,
                    "dynamic_uw": 334.4,
                    "total_uw": 509.1
                }]
            }
        }));
        let current = concat!(
            "  Multi-Corner Power Analysis (100 MHz)\n",
            "  Corner Type Voltage Static Dynamic Total Source\n",
            "  typ_tt_1p2_25 TT 1.2V 74.3 434.8 509.1 ESTIMATED_PVT\n",
            "  ff_cbest_1p32_125 FF 1.32V 580.4 641.2 1221.6 ESTIMATED_PVT\n",
        );

        let exchange = build_power_corners_exchange(&stale_json, Some(current));
        assert!(exchange.contains("ff_cbest_1p32_125"));
        assert!(exchange.contains("1221.600000"));
        assert!(!exchange.contains("334.400000"));
    }
}
