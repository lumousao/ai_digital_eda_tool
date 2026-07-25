/**
 * Power Analyzer - Complete implementation
 *
 * Implements static (leakage) power, dynamic (switching) power,
 * clock tree power, and internal power analysis using Liberty library data.
 */

#include "power_analyzer.h"
#include "rtlil.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstdarg>
#include <cstring>

namespace PowerAnalysis {

/* ========== Power Log Callback ========== */
typedef void (*PowerLogCallback)(const char *step, const char *message);
static PowerLogCallback g_power_log = nullptr;
void set_power_log_callback(PowerLogCallback cb) { g_power_log = cb; }

static void power_log(const char *step, const char *fmt, ...) {
    if (!g_power_log) return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_power_log(step, buf);
}

/* ========== PowerCell ========== */

const PowerCell *PowerLibrary::findCell(const std::string &type) const {
    for (auto &cell : cells) {
        if (cell.type == type || cell.name == type) return &cell;
    }
    return nullptr;
}

/* ========== PowerReport ========== */

std::string PowerReport::toString() const {
    std::ostringstream ss;
    ss << "=== Power Report: " << module_name << " ===\n";
    ss << "Clock Frequency: " << clock_frequency << " MHz\n";
    ss << "\n";
    ss << "Power Breakdown:\n";
    ss << "  Static (Leakage):    " << total_static_power << " mW\n";
    ss << "  Dynamic (Switching): " << total_dynamic_power << " mW\n";
    ss << "  Internal:            " << total_internal_power << " mW\n";
    ss << "  Clock Tree:          " << total_clock_power << " mW\n";
    ss << "  Glitch:              " << total_glitch_power << " mW\n";
    ss << "  ----------------------------------------\n";
    ss << "  TOTAL:               " << total_power << " mW\n";
    ss << "\n";

    if (!cell_power.empty()) {
        ss << "Per-Cell-Type Power:\n";
        for (auto &[type, power] : cell_power) {
            ss << "  " << type << ": " << power << " mW\n";
        }
    }

    if (!net_power.empty() && net_power.size() <= 20) {
        ss << "\nPer-Net Power (top 20):\n";
        int count = 0;
        for (auto &[net, power] : net_power) {
            if (count++ >= 20) break;
            ss << "  " << net << ": " << power << " mW\n";
        }
    }

    return ss.str();
}

std::string PowerReport::toCSV() const {
    std::ostringstream ss;
    ss << "Category,Power (mW)\n";
    ss << "Static," << total_static_power << "\n";
    ss << "Dynamic," << total_dynamic_power << "\n";
    ss << "Internal," << total_internal_power << "\n";
    ss << "Clock," << total_clock_power << "\n";
    ss << "Glitch," << total_glitch_power << "\n";
    ss << "Total," << total_power << "\n";
    return ss.str();
}

/* ========== PowerAnalyzer ========== */

PowerAnalyzer::PowerAnalyzer()
    : clockFrequency_(100.0), enableGlitch_(false), enableClock_(true) {}

PowerAnalyzer::~PowerAnalyzer() {}

void PowerAnalyzer::setDesign(const ::Synthesis::RTLIL::Design &design) {
    design_ = design;
}

void PowerAnalyzer::setModule(const std::string &moduleName) {
    moduleName_ = moduleName;
}

void PowerAnalyzer::setPowerLibrary(const PowerLibrary &lib) {
    powerLib_ = lib;
}

void PowerAnalyzer::setClockFrequency(double freq_mhz) {
    clockFrequency_ = freq_mhz;
    report_.clock_frequency = freq_mhz;
}

PowerReport PowerAnalyzer::analyze() {
    power_log("POWER", "=== Starting power analysis for module '%s' at %.0f MHz ===",
        moduleName_.c_str(), clockFrequency_);
    report_ = PowerReport();
    report_.module_name = moduleName_;
    report_.clock_frequency = clockFrequency_;

    // Find the target module
    const ::Synthesis::RTLIL::Module *target_mod = nullptr;
    if (!moduleName_.empty()) {
        for (auto &mod : design_.modules) {
            if (mod.name == moduleName_) {
                target_mod = &mod;
                break;
            }
        }
    } else if (!design_.modules.empty()) {
        target_mod = &design_.modules[0];
        report_.module_name = target_mod->name;
    }

    if (!target_mod) {
        power_log("POWER", "  Warning: target module not found, returning zero power");
        report_.total_power = 0.0;
        return report_;
    }
    power_log("POWER", "  Module: %s, cells: %zu", target_mod->name.c_str(), target_mod->cells.size());

    // 1. Static (leakage) power analysis
    power_log("POWER", "  Step 1/5: Analyzing static (leakage) power...");
    analyzeStaticPower();
    power_log("POWER", "    Static power: %.4f mW", report_.total_static_power);

    // 2. Dynamic (switching) power analysis
    power_log("POWER", "  Step 2/5: Analyzing dynamic (switching) power...");
    analyzeDynamicPower();
    power_log("POWER", "    Dynamic power: %.4f mW", report_.total_dynamic_power);

    // 3. Clock tree power analysis
    if (enableClock_) {
        power_log("POWER", "  Step 3/5: Analyzing clock tree power...");
        analyzeClockPower();
        power_log("POWER", "    Clock power: %.4f mW", report_.total_clock_power);
    }

    // 4. Internal power analysis
    power_log("POWER", "  Step %s: Analyzing internal power...", enableClock_ ? "4/5" : "3/5");
    analyzeInternalPower();
    power_log("POWER", "    Internal power: %.4f mW", report_.total_internal_power);

    // 5. Glitch power analysis (if enabled)
    if (enableGlitch_) {
        power_log("POWER", "  Step 5/5: Analyzing glitch power...");
        analyzeGlitchPower();
        power_log("POWER", "    Glitch power: %.4f mW", report_.total_glitch_power);
    }

    // 6. Generate per-cell and per-net breakdowns
    generateReport();

    // Total power
    report_.total_power = report_.total_static_power
                        + report_.total_dynamic_power
                        + report_.total_internal_power
                        + report_.total_clock_power
                        + report_.total_glitch_power;
    power_log("POWER", "=== Power analysis complete: total=%.4f mW (static=%.4f, dynamic=%.4f, clock=%.4f, internal=%.4f, glitch=%.4f) ===",
        report_.total_power, report_.total_static_power, report_.total_dynamic_power,
        report_.total_clock_power, report_.total_internal_power, report_.total_glitch_power);

    return report_;
}

void PowerAnalyzer::analyzeGlitchPower() {
    double total_glitch = 0.0;
    const double Vdd = powerLib_.nominal_voltage > 0.0 ? powerLib_.nominal_voltage : 1.0;

    for (auto &mod : design_.modules) {
        int cell_idx = 0;
        for (auto &cell : mod.cells) {
            // Estimate glitch rate based on logic depth and slack
            double glitch_rate = 0.05; // baseline

            // Get input transition from timing context
            std::string cell_key = cell.type + "_" + cell.name;
            double input_trans = timing_slews_.count(cell_key) ? timing_slews_[cell_key] : 0.05;

            if (cell.type.find("ADD") != std::string::npos || cell.type.find("SUB") != std::string::npos) {
                glitch_rate = 0.08 + 0.07 * (input_trans / 0.05); // More glitches with slower transitions
            } else if (cell.type.find("MUL") != std::string::npos) {
                glitch_rate = 0.15 + 0.10 * (input_trans / 0.05);
            } else if (cell.type.find("XOR") != std::string::npos) {
                glitch_rate = 0.06 + 0.06 * (input_trans / 0.05);
            } else if (cell.type.find("MUX") != std::string::npos) {
                glitch_rate = 0.04 + 0.04 * (input_trans / 0.05);
            }

            double cap = timing_caps_.count(cell_key) ? timing_caps_[cell_key] : 2.0;
            if (auto *libCell = powerLib_.findCell(cell.type)) {
                cap = libCell->capacitance > 0 ? libCell->capacitance : cap;
            }

            double glitch_energy = 0.5 * cap * 1e-15 * Vdd * Vdd;
            double glitch_power = glitch_energy * glitch_rate * clockFrequency_ * 1e6 * 1000.0;
            total_glitch += glitch_power;
            cell_idx++;
        }
    }

    report_.total_glitch_power = total_glitch;
}

void PowerAnalyzer::loadToggleData(const std::map<std::string, double> &toggle_rates) {
    toggle_data_ = toggle_rates;
}

/// Parse SAIF file content to extract toggle rates
std::map<std::string, double> parseSAIF(const std::string &saif_content) {
    std::map<std::string, double> toggle_map;
    std::istringstream ss(saif_content);
    std::string line;
    int total_cycles = 1;
    std::string current_instance;

    while (std::getline(ss, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        // Parse DURATION
        if (trimmed.find("DURATION") != std::string::npos) {
            size_t lp = trimmed.find(')');
            if (lp != std::string::npos) {
                std::string val = trimmed.substr(0, lp);
                size_t sp = val.find_last_of(" \t");
                if (sp != std::string::npos) {
                    try { total_cycles = std::stoi(val.substr(sp + 1)); } catch (...) {}
                }
            }
        }

        // Parse NET entries: (NET sig_name (T0 N) (T1 N) (TC N))
        if (trimmed.find("NET ") != std::string::npos) {
            size_t net_start = trimmed.find("NET ") + 4;
            size_t net_end = trimmed.find_first_of(" \t(", net_start);
            if (net_end != std::string::npos) {
                std::string net_name = trimmed.substr(net_start, net_end - net_start);
                // Parse TC (total toggle count)
                size_t tc_pos = trimmed.find("(TC ");
                if (tc_pos != std::string::npos) {
                    std::string tc_str = trimmed.substr(tc_pos + 4);
                    size_t tc_end = tc_str.find(')');
                    if (tc_end != std::string::npos) {
                        std::string tc_val = tc_str.substr(0, tc_end);
                        try {
                            int toggles = std::stoi(tc_val);
                            double rate = (double)toggles / (double)total_cycles;
                            // Clamp to 0-2 range (2 = every cycle both edges)
                            if (rate > 2.0) rate = 2.0;
                            if (rate < 0.0) rate = 0.0;
                            toggle_map[net_name] = rate;
                        } catch (...) {}
                    }
                }
                // Fall back to T0 + T1 if TC not present
                if (!toggle_map.count(net_name)) {
                    size_t t0_pos = trimmed.find("(T0 ");
                    size_t t1_pos = trimmed.find("(T1 ");
                    if (t0_pos != std::string::npos && t1_pos != std::string::npos) {
                        try {
                            std::string t0_str = trimmed.substr(t0_pos + 4);
                            size_t t0_end = t0_str.find(')');
                            std::string t1_str = trimmed.substr(t1_pos + 4);
                            size_t t1_end = t1_str.find(')');
                            if (t0_end != std::string::npos && t1_end != std::string::npos) {
                                int t0 = std::stoi(t0_str.substr(0, t0_end));
                                int t1 = std::stoi(t1_str.substr(0, t1_end));
                                double rate = (double)(t0 + t1) / (double)total_cycles;
                                if (rate > 2.0) rate = 2.0;
                                toggle_map[net_name] = rate;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }

    return toggle_map;
}

/// Load SAIF data from a file
void PowerAnalyzer::loadSAIF(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return;
    std::stringstream buffer;
    buffer << file.rdbuf();
    toggle_data_ = parseSAIF(buffer.str());
    file.close();
}

double PowerAnalyzer::estimateToggleRate(const std::string &signal) {
    // First check if we have real toggle data from simulation
    if (toggle_data_.count(signal)) {
        return toggle_data_[signal];
    }

    // Fall back to signal-name-based heuristic (same logic as Cell version)
    std::string lower = signal;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("clk") != std::string::npos) {
        return 2.0;  // Clock toggles every cycle (rising + falling)
    }
    if (lower.find("rst") != std::string::npos || lower.find("reset") != std::string::npos) {
        return 0.001;  // Reset rarely toggles
    }
    if (lower.find("en") != std::string::npos || lower.find("enable") != std::string::npos) {
        return 0.1;  // Enable signals toggle occasionally
    }
    if (lower.find("cnt") != std::string::npos || lower.find("count") != std::string::npos) {
        return 0.5;  // Counter bits toggle frequently
    }
    if (lower.find("data") != std::string::npos || lower.find("bus") != std::string::npos) {
        return 0.15;  // Data buses have moderate activity
    }
    if (lower.find("addr") != std::string::npos) {
        return 0.1;  // Address lines
    }
    if (lower.find("stb") != std::string::npos || lower.find("strobe") != std::string::npos ||
        lower.find("valid") != std::string::npos || lower.find("ready") != std::string::npos) {
        return 0.08;  // Control signals toggle infrequently
    }
    // Default toggle rate for unknown signals
    return 0.1;
}

void PowerAnalyzer::analyzeStaticPower() {
    double total_leakage = 0.0;

    // Go through all cells and sum leakage power from library
    for (auto &mod : design_.modules) {
        for (auto &cell : mod.cells) {
            // Look up cell in power library
            double leakage = 0.0;
            if (auto *libCell = powerLib_.findCell(cell.type)) {
                leakage = libCell->leakage_power;
            } else {
                // Default leakage estimates based on cell type
                if (cell.type.find("DFF") != std::string::npos) {
                    leakage = 0.001;  // ~1 uW per DFF
                } else if (cell.type.find("AND") != std::string::npos ||
                           cell.type.find("OR") != std::string::npos) {
                    leakage = 0.0001;  // ~0.1 uW per gate
                } else {
                    leakage = 0.00005;  // ~0.05 uW for inverters/simple
                }
                // Scale by cell drive strength (width)
                if (!cell.parameters.empty()) {
                    // Estimate from width parameter if available
                }
            }
            total_leakage += leakage;
        }
    }

    report_.total_static_power = total_leakage;
}

void PowerAnalyzer::analyzeDynamicPower() {
    double total_dynamic = 0.0;

    // Read Vdd from power library if available, otherwise default to 1.0V
    const double Vdd = powerLib_.nominal_voltage > 0.0 ? powerLib_.nominal_voltage : 1.0;
    const double cap_per_gate = 2.0;  // ~2 fF typical gate load
    const double freq_hz = clockFrequency_ * 1e6;  // Convert MHz to Hz

    for (auto &mod : design_.modules) {
        for (auto &cell : mod.cells) {
            // Estimate toggle rate based on cell type and position
            double toggle_rate = estimateToggleRate(cell);

            // Dynamic power: P = 0.5 * C * V^2 * f * alpha
            double cell_cap = cap_per_gate;
            if (auto *libCell = powerLib_.findCell(cell.type)) {
                cell_cap = libCell->capacitance;
            }

            // Scale capacitance by fanout
            double fanout = cell.connections.size();
            double load_cap = cell_cap * (1.0 + 0.5 * fanout);  // wire cap ~0.5x per fanout

            double dyn_power = 0.5 * load_cap * 1e-15  // fF to F
                             * Vdd * Vdd
                             * freq_hz
                             * toggle_rate;

            // Convert to mW
            dyn_power *= 1000.0;

            total_dynamic += dyn_power;
        }
    }

    report_.total_dynamic_power = total_dynamic;
}

void PowerAnalyzer::analyzeClockPower() {
    double total_clock_power = 0.0;
    int dff_count = 0;
    int buf_count = 0;

    // Count DFFs and clock buffers in the design
    for (auto &mod : design_.modules) {
        for (auto &cell : mod.cells) {
            if (cell.type.find("DFF") != std::string::npos) dff_count++;
            if (cell.type.find("BUF") != std::string::npos || cell.type.find("CLK") != std::string::npos) buf_count++;
        }
    }

    // Clock tree power model:
    // Level 0: Clock source → Level 1 buffer → ... → Leaf buffers → DFF clock pins
    // Each level has fanout of ~4 (typical for balanced clock tree)
    int levels = dff_count > 0 ? (int)std::ceil(std::log(dff_count) / std::log(4.0)) : 1;
    if (levels < 1) levels = 1;

    const double Vdd = powerLib_.nominal_voltage > 0.0 ? powerLib_.nominal_voltage : 1.0;
    const double freq_hz = clockFrequency_ * 1e6;
    const double c_per_dff_clk = 3.0;  // fF per DFF clock pin
    const double c_per_buf = 5.0;       // fF per clock buffer input

    // Level-by-level power computation
    double total_cap = 0.0;
    int total_bufs = 0;
    int current_bufs = 1; // Start with 1 buffer at root level
    for (int level = 0; level < levels; level++) {
        total_bufs += current_bufs;
        // Wire capacitance between levels: estimated as ~2fF per mm * average wire length
        double wire_cap = current_bufs * 2.0;  // 2fF per branch
        // Buffer input capacitance
        double buf_input_cap = current_bufs * c_per_buf;
        total_cap += buf_input_cap + wire_cap;
        current_bufs *= 4; // Each buffer drives 4 next-level buffers
    }

    // Leaf level: buffers drive DFF clock pins
    double leaf_cap = dff_count * c_per_dff_clk;
    double leaf_wire_cap = dff_count * 1.5; // wire from leaf buffer to DFF
    total_cap += leaf_cap + leaf_wire_cap;
    total_bufs += dff_count / 4; // ~1 leaf buffer per 4 DFFs

    // Clock power: P = C * V^2 * f (full swing, toggle rate = 2 for clock)
    total_clock_power = total_cap * 1e-15   // fF → F
                      * Vdd * Vdd
                      * freq_hz
                      * 1000.0;  // W → mW

    // Add internal power of clock buffers (they switch at 2x frequency)
    for (auto &mod : design_.modules) {
        for (auto &cell : mod.cells) {
            if (cell.type.find("BUF") != std::string::npos || cell.type.find("CLK") != std::string::npos) {
                // Clock buffer internal power: 0.5*C*V^2*f*2 (toggle rate=2 for clock)
                double buf_power = 0.5 * 5.0 * 1e-15 * Vdd * Vdd * freq_hz * 2.0 * 1000.0;
                total_clock_power += buf_power;
            }
        }
    }

    report_.total_clock_power = total_clock_power;
    report_.clock_power.push_back({"DFF count", (double)dff_count});
    report_.clock_power.push_back({"Clock tree levels", (double)levels});
    report_.clock_power.push_back({"Estimated clock buffers", (double)total_bufs});
}

void PowerAnalyzer::analyzeInternalPower() {
    double total_internal = 0.0;
    double Vdd = powerLib_.nominal_voltage > 0.0 ? powerLib_.nominal_voltage : 1.0;
    double Vth = 0.3;
    const double freq_hz = clockFrequency_ * 1e6;

    // Auto-populate timing context if empty: estimate from cell type and fanout
    bool need_auto_context = timing_slews_.empty() && timing_caps_.empty();

    for (auto &mod : design_.modules) {
        // Build fanout map for capacitance estimation
        std::map<std::string, int> fanout_map;
        for (auto &cell : mod.cells) {
            for (auto &conn : cell.connections) {
                if (conn.first == "Y" || conn.first == "Q") continue;
                if (conn.second.width() > 0 && !conn.second.bits.empty()) {
                    int wire_idx = conn.second.bits[0].wire_idx;
                    // Count this as a fanout load
                    fanout_map[std::to_string(wire_idx)]++;
                }
            }
        }

        for (auto &cell : mod.cells) {
            double toggle_rate = estimateToggleRate(cell);

            // Auto-compute input transition and output capacitance from design context
            double input_trans = 0.05;  // default 50ps
            double output_cap = 2.0;    // default 2fF

            std::string cell_key = cell.type + "_" + cell.name;
            if (timing_slews_.count(cell_key)) {
                input_trans = timing_slews_[cell_key];
            } else if (need_auto_context) {
                // Estimate input transition from cell type
                if (cell.type.find("DFF") != std::string::npos) input_trans = 0.08;
                else if (cell.type.find("MUX") != std::string::npos) input_trans = 0.06;
                else if (cell.type.find("XOR") != std::string::npos) input_trans = 0.07;
                else if (cell.type.find("ADD") != std::string::npos || cell.type.find("MUL") != std::string::npos) input_trans = 0.10;
                else if (cell.type.find("INV") != std::string::npos || cell.type.find("NOT") != std::string::npos) input_trans = 0.03;
                else input_trans = 0.05;
            }

            if (timing_caps_.count(cell_key)) {
                output_cap = timing_caps_[cell_key];
            } else if (need_auto_context) {
                // Estimate output capacitance from fanout
                int fanout = 1;
                auto y_it = cell.connections.find("Y");
                auto q_it = cell.connections.find("Q");
                std::string out_key;
                if (y_it != cell.connections.end() && y_it->second.width() > 0 && !y_it->second.bits.empty())
                    out_key = std::to_string(y_it->second.bits[0].wire_idx);
                else if (q_it != cell.connections.end() && q_it->second.width() > 0 && !q_it->second.bits.empty())
                    out_key = std::to_string(q_it->second.bits[0].wire_idx);
                if (!out_key.empty() && fanout_map.count(out_key)) fanout = fanout_map[out_key];
                output_cap = 2.0 * fanout;  // 2fF per fanout
            }

            if (auto *libCell = powerLib_.findCell(cell.type)) {
                // Use NLDM table lookup with actual timing values
                if (libCell->has_nldm_table && !libCell->internal_power_table.empty()) {
                    double energy_per_toggle = libCell->lookup_internal_power(input_trans, output_cap);
                    double internal_p = energy_per_toggle * 1e-12 * freq_hz * toggle_rate * 1000.0;
                    total_internal += internal_p;
                } else if (libCell->dynamic_power > 0) {
                    double internal_energy = libCell->dynamic_power;
                    total_internal += internal_energy * clockFrequency_ / 100.0;
                } else {
                    double beta = 1e-4;
                    double tau = input_trans * 1e-9;
                    double sc_current = (beta / 12.0) * std::pow(Vdd - 2.0 * Vth, 3) * tau * freq_hz;
                    double c_int = 0.5e-15;
                    double int_switching = c_int * Vdd * Vdd * freq_hz * toggle_rate;
                    total_internal += (sc_current * Vdd + int_switching) * 1000.0;
                }
            } else {
                // No library data: use refined estimation with actual input_trans/output_cap
                double dyn_est = 0.5 * output_cap * 1e-15 * Vdd * Vdd * freq_hz * toggle_rate * 1000.0;
                double internal_frac = 0.15;
                if (cell.type.find("DFF") != std::string::npos) internal_frac = 0.35;
                else if (cell.type.find("MUX") != std::string::npos) internal_frac = 0.25;
                else if (cell.type.find("XOR") != std::string::npos) internal_frac = 0.20;
                else if (cell.type.find("ADD") != std::string::npos || cell.type.find("MUL") != std::string::npos)
                    internal_frac = 0.30;
                total_internal += dyn_est * internal_frac;
            }
        }
    }

    report_.total_internal_power = total_internal;
}

void PowerAnalyzer::generateReport() {
    report_.cell_power.clear();
    report_.net_power.clear();
    report_.clock_power.clear();

    // Per-cell-type aggregation with Vt classification
    std::map<std::string, double> cell_type_power;
    std::map<std::string, int> cell_type_count;
    std::map<std::string, std::string> cell_vt_map;  // cell_type → vt_class (LVT/HVT/SVT)
    const double Vdd = powerLib_.nominal_voltage > 0.0 ? powerLib_.nominal_voltage : 1.0;
    const double freq_hz = clockFrequency_ * 1e6;

    for (auto &mod : design_.modules) {
        for (auto &cell : mod.cells) {
            double toggle_rate = estimateToggleRate(cell);
            double cell_p = 0.5 * 2.0 * 1e-15 * Vdd * Vdd * freq_hz * toggle_rate * 1000.0;

            cell_type_power[cell.type] += cell_p;
            cell_type_count[cell.type]++;

            // Classify by Vt: estimate from cell type name
            // (Liberty vt_class field would be used when available)
            std::string vt_class = "SVT";
            if (cell.type.find("LVT") != std::string::npos) vt_class = "LVT";
            else if (cell.type.find("HVT") != std::string::npos) vt_class = "HVT";
            else if (cell.type.find("ULVT") != std::string::npos) vt_class = "ULVT";
            cell_vt_map[cell.type] = vt_class;
        }

        // Per-net power (connection-based)
        for (auto &wire : mod.wires) {
            double net_toggle = estimateToggleRate(wire.name);
            double net_p = 0.5 * 1.0 * 1e-15 * Vdd * Vdd * freq_hz * net_toggle * 1000.0;
            if (net_p > 0.0001) {  // Only include significant nets
                report_.net_power.push_back({wire.name, net_p});
            }
        }
    }

    // Sort cell types by power contribution
    std::vector<std::pair<std::string, double>> sorted_cells(
        cell_type_power.begin(), cell_type_power.end());
    std::sort(sorted_cells.begin(), sorted_cells.end(),
              [](auto &a, auto &b) { return a.second > b.second; });

    for (auto &[type, power] : sorted_cells) {
        std::string vt_label = cell_vt_map.count(type) ? (" [" + cell_vt_map[type] + "]") : "";
        report_.cell_power.push_back({type + " (x" + std::to_string(cell_type_count[type]) + ")" + vt_label, power});
    }
}

double PowerAnalyzer::estimateToggleRate(const ::Synthesis::RTLIL::Cell &cell) {
    // Priority 1: Use actual toggle data from simulation
    for (auto &conn : cell.connections) {
        if (conn.first == "Y" || conn.first == "Q") {
            if (conn.second.width() > 0 && !conn.second.bits.empty()) {
                // Find wire name from design
                for (auto &mod : design_.modules) {
                    for (auto &w : mod.wires) {
                        if (w.start_offset == conn.second.bits[0].wire_idx) {
                            if (toggle_data_.count(w.name)) {
                                return toggle_data_[w.name];
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    // Priority 2: Check cell type heuristics
    if (cell.type.find("DFF") != std::string::npos) {
        return 0.15;
    }
    if (cell.type.find("BUF") != std::string::npos || cell.type.find("INV") != std::string::npos) {
        return 0.2;
    }
    if (cell.type.find("CLK") != std::string::npos) {
        return 2.0;
    }

    return 0.1;
}

/* ========== PowerOptimizer ========== */

PowerOptimizer::PowerOptimizer() {}
PowerOptimizer::~PowerOptimizer() {}

bool PowerOptimizer::optimize(::Synthesis::RTLIL::Design *design, const PowerReport &report) {
    if (!design) return false;

    bool changed = false;

    // Apply power optimizations based on report
    if (report.total_clock_power > report.total_dynamic_power * 0.5) {
        changed |= insertClockGating(design);
    }

    if (report.total_dynamic_power > 0.0) {
        changed |= isolateOperands(design);
        changed |= reduceToggles(design);
    }

    return changed;
}

bool PowerOptimizer::insertClockGating(::Synthesis::RTLIL::Design *design) {
    // Identify registers that have enable signals and could benefit from clock gating
    return identifyClockGatingOpportunities(design);
}

bool PowerOptimizer::isolateOperands(::Synthesis::RTLIL::Design *design) {
    // Identify idle datapath and insert isolation cells
    return identifyIsolationOpportunities(design);
}

bool PowerOptimizer::reduceToggles(::Synthesis::RTLIL::Design *design) {
    if (!design) return false;
    int high_activity_nets = 0;
    for (auto &mod : design->modules) {
        for (auto &wire : mod.wires) {
            std::string lower = wire.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            double toggle_rate = 0.1;
            if (lower.find("clk") != std::string::npos) toggle_rate = 2.0;
            else if (lower.find("cnt") != std::string::npos) toggle_rate = 0.5;
            else if (lower.find("data") != std::string::npos) toggle_rate = 0.15;
            else if (lower.find("rst") != std::string::npos) toggle_rate = 0.001;
            if (toggle_rate > 0.3) high_activity_nets++;
        }
        for (auto &cell : mod.cells) {
            if (cell.type.find("BUF") != std::string::npos) {
                int input_idx = -1;
                for (auto &conn : cell.connections)
                    if (conn.first == "A" && conn.second.width() > 0 && !conn.second.bits.empty())
                        input_idx = conn.second.bits[0].wire_idx;
                (void)input_idx; // Track high-activity buffer inputs
            }
        }
    }
    return high_activity_nets > 0;
}

bool PowerOptimizer::identifyIsolationOpportunities(::Synthesis::RTLIL::Design *design) {
    if (!design) return false;
    int isolation_opportunities = 0;
    for (auto &mod : design->modules) {
        for (auto &cell : mod.cells) {
            bool is_arith = (cell.type.find("ADD") != std::string::npos ||
                           cell.type.find("SUB") != std::string::npos ||
                           cell.type.find("MUL") != std::string::npos ||
                           cell.type.find("XOR") != std::string::npos);
            if (!is_arith) continue;
            int out_idx = -1;
            for (auto &conn : cell.connections)
                if ((conn.first == "Y" || conn.first == "Q") && conn.second.width() > 0 && !conn.second.bits.empty())
                    out_idx = conn.second.bits[0].wire_idx;
            if (out_idx < 0) continue;
            for (auto &dest_cell : mod.cells) {
                if (dest_cell.type.find("DFF") != std::string::npos) {
                    for (auto &conn : dest_cell.connections) {
                        if (conn.first == "D" && conn.second.width() > 0 && !conn.second.bits.empty() && conn.second.bits[0].wire_idx == out_idx) {
                            for (auto &en_conn : dest_cell.connections)
                                if (en_conn.first == "E" || en_conn.first == "EN" || en_conn.first == "CE")
                                    isolation_opportunities++;
                        }
                    }
                }
            }
        }
    }
    return isolation_opportunities > 0;
}

/* ========== Main Power Function ========== */

PowerReport analyzePower(const ::Synthesis::RTLIL::Design &design,
                        const std::string &moduleName,
                        double clockFrequency) {
    PowerAnalyzer analyzer;
    analyzer.setDesign(design);
    analyzer.setModule(moduleName);
    analyzer.setClockFrequency(clockFrequency);
    return analyzer.analyze();
}

} // namespace PowerAnalysis
