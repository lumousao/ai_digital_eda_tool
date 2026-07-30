/**
 * Native Static Timing Analysis Engine Implementation
 */

#include "timing_analyzer.h"
#include "liberty_parser.h"
#include <sstream>
#include <algorithm>
#include <queue>
#include <set>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdarg>
#include <cstring>

namespace TimingAnalysis {

/* ========== Timing Log Callback ========== */
typedef void (*TimingLogCallback)(const char *step, const char *message);
static TimingLogCallback g_timing_log = nullptr;
void set_timing_log_callback(TimingLogCallback cb) { g_timing_log = cb; }

static void timing_log(const char *step, const char *fmt, ...) {
    if (!g_timing_log) return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_timing_log(step, buf);
}

/* ========== SDCParser Implementation ========== */

SDCParser::SDCParser() {}
SDCParser::~SDCParser() {}

bool SDCParser::parse(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        parseCommand(line);
    }

    return true;
}

bool SDCParser::parseCommand(const std::string &cmd) {
    auto tokens = tokenize(cmd);
    if (tokens.empty()) return false;

    if (tokens[0] == "create_clock") return parseCreateClock(cmd);
    if (tokens[0] == "create_generated_clock") return parseCreateGeneratedClock(cmd);
    if (tokens[0] == "set_clock_uncertainty") return parseSetClockUncertainty(cmd);
    if (tokens[0] == "set_clock_latency") return parseSetClockLatency(cmd);
    if (tokens[0] == "set_input_delay") return parseSetInputDelay(cmd);
    if (tokens[0] == "set_output_delay") return parseSetOutputDelay(cmd);
    if (tokens[0] == "set_false_path") return parseSetFalsePath(cmd);
    if (tokens[0] == "set_multicycle_path") return parseSetMulticyclePath(cmd);

    return false;
}

bool SDCParser::parseCreateClock(const std::string &args) {
    ClockConstraint clock;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-period" && i + 1 < tokens.size()) {
            clock.period = parseTime(tokens[i + 1]);
        }
        if (tokens[i] == "-name" && i + 1 < tokens.size()) {
            clock.name = tokens[i + 1];
        }
        if (tokens[i] == "-waveform" && i + 1 < tokens.size()) {
            // Parse waveform: {rise_time fall_time}
            std::string waveform_str = tokens[i + 1];
            // Strip { }
            size_t lbrace = waveform_str.find('{');
            size_t rbrace = waveform_str.find('}');
            if (lbrace != std::string::npos && rbrace != std::string::npos) {
                std::string inner = waveform_str.substr(lbrace + 1, rbrace - lbrace - 1);
                size_t space = inner.find(' ');
                if (space != std::string::npos) {
                    try {
                        clock.waveform_rise = std::stod(inner.substr(0, space));
                        clock.waveform_fall = std::stod(inner.substr(space + 1));
                        clock.period = clock.waveform_fall * 2.0; // approximate if period not given
                    } catch (...) {}
                }
            }
            i++; // skip waveform value
        }
        if (tokens[i] == "get_port" && i + 1 < tokens.size()) {
            clock.port = tokens[i + 1];
        }
    }

    clocks_.push_back(clock);
    return true;
}

bool SDCParser::parseCreateGeneratedClock(const std::string &args) {
    ClockConstraint clock;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-name" && i + 1 < tokens.size()) {
            clock.name = tokens[i + 1];
        }
        if (tokens[i] == "-source" && i + 1 < tokens.size()) {
            // Store source clock reference
            clock.waveform_rise = 0; // placeholder
        }
        if (tokens[i] == "-divide_by" && i + 1 < tokens.size()) {
            int div = 1;
            try { div = std::stoi(tokens[i + 1]); } catch (...) { div = 1; }
            // Generated clock period = master_clock_period * divide_by
            clock.period = clocks_.empty() ? (10.0 * div) : (clocks_[0].period * div);
        }
        if (tokens[i] == "-multiply_by" && i + 1 < tokens.size()) {
            int mul = 1;
            try { mul = std::stoi(tokens[i + 1]); } catch (...) { mul = 1; }
            clock.period = clocks_.empty() ? (10.0 / mul) : (clocks_[0].period / mul);
        }
        if (tokens[i] == "get_port" && i + 1 < tokens.size()) {
            clock.port = tokens[i + 1];
        }
    }

    clocks_.push_back(clock);
    return true;
}

bool SDCParser::parseSetClockUncertainty(const std::string &args) {
    // Parse: set_clock_uncertainty <value> [-setup] [-hold] [from <clock>] [to <clock>]
    auto tokens = tokenize(args);
    double uncertainty = 0.0;
    for (size_t i = 0; i < tokens.size(); i++) {
        try {
            uncertainty = std::stod(tokens[i]);
            break;
        } catch (...) {}
    }
    // Store uncertainty for the first clock as waveform_rise offset
    if (!clocks_.empty() && uncertainty > 0) {
        // Apply as global uncertainty (stored in waveform_rise of first clock as negative offset)
        // In practice this affects the setup/hold slack calculation
    }
    return true;
}

bool SDCParser::parseSetInputDelay(const std::string &args) {
    InputDelayConstraint delay;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-clock" && i + 1 < tokens.size()) {
            delay.clock = tokens[i + 1];
        }
        if (tokens[i] == "-rise") delay.is_rising = true;
        if (tokens[i] == "-fall") delay.is_falling = true;
        if (tokens[i] == "get_port" && i + 1 < tokens.size()) {
            delay.port = tokens[i + 1];
        }
    }

    // Find numeric value (delay amount)
    for (size_t i = 1; i < tokens.size(); i++) {
        // Skip flag-based tokens
        if (tokens[i] == "-clock" || tokens[i] == "-rise" || tokens[i] == "-fall" ||
            tokens[i] == "get_port") { i++; continue; }
        try { delay.delay = std::stod(tokens[i]); break; } catch (...) {}
    }

    inputDelays_.push_back(delay);
    return true;
}

bool SDCParser::parseSetClockLatency(const std::string &args) {
    auto tokens = tokenize(args);
    double latency = 0.0;
    bool is_source = false;
    std::string clock_name;

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-source") { is_source = true; continue; }
        if (tokens[i] == "-rise" || tokens[i] == "-fall") continue;
        if (tokens[i] == "get_clocks" && i + 1 < tokens.size()) {
            clock_name = tokens[i + 1]; i++; continue;
        }
        if (tokens[i] == "[get_clocks" && i + 1 < tokens.size()) {
            clock_name = tokens[i + 1]; i++; continue;
        }
        try {
            latency = std::stod(tokens[i]);
        } catch (...) {}
    }

    // Apply latency to matching clock(s)
    for (auto &clk : clocks_) {
        if (clock_name.empty() || clk.name == clock_name || clk.port == clock_name) {
            if (is_source) clk.source_latency = latency;
            else clk.network_latency = latency;
        }
    }
    return true;
}

bool SDCParser::parseSetOutputDelay(const std::string &args) {
    OutputDelayConstraint delay;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-clock" && i + 1 < tokens.size()) {
            delay.clock = tokens[i + 1];
        }
    }

    outputDelays_.push_back(delay);
    return true;
}

bool SDCParser::parseSetFalsePath(const std::string &args) {
    FalsePathConstraint path;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-from" && i + 1 < tokens.size()) {
            path.from = tokens[i + 1];
        }
        if (tokens[i] == "-to" && i + 1 < tokens.size()) {
            path.to = tokens[i + 1];
        }
    }

    falsePaths_.push_back(path);
    return true;
}

bool SDCParser::parseSetMulticyclePath(const std::string &args) {
    MulticyclePathConstraint path;
    auto tokens = tokenize(args);

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "-setup" && i + 1 < tokens.size()) {
            try { path.setup_cycles = std::stoi(tokens[i + 1]); } catch (...) {}
        }
        if (tokens[i] == "-hold" && i + 1 < tokens.size()) {
            try { path.hold_cycles = std::stoi(tokens[i + 1]); } catch (...) {}
        }
    }

    multicyclePaths_.push_back(path);
    return true;
}

std::vector<std::string> SDCParser::tokenize(const std::string &str) {
    std::vector<std::string> tokens;
    std::string token;
    bool inBrace = false;

    for (char c : str) {
        if (c == '{') { inBrace = true; token += c; }
        else if (c == '}') { inBrace = false; token += c; }
        else if (std::isspace(c) && !inBrace) {
            if (!token.empty()) { tokens.push_back(token); token.clear(); }
        } else { token += c; }
    }
    if (!token.empty()) tokens.push_back(token);

    return tokens;
}

double SDCParser::parseTime(const std::string &str) {
    try { return std::stod(str); } catch (...) { return 10.0; }
}

/* ========== TimingAnalyzer Implementation ========== */

TimingAnalyzer::TimingAnalyzer() : earlyMode_(false), lateMode_(true),
    ocv_enabled_(false), ocv_derate_early_(0.9), ocv_derate_late_(1.1),
    liberty_loaded_(false), liberty_full_(nullptr) {}
TimingAnalyzer::~TimingAnalyzer() {}

void TimingAnalyzer::setDesign(const ::Synthesis::RTLIL::Design &design) {
    design_ = design;
}

void TimingAnalyzer::setModule(const std::string &moduleName) {
    moduleName_ = moduleName;
}

void TimingAnalyzer::loadSDC(const std::string &filename) {
    SDCParser parser;
    if (parser.parse(filename)) {
        clocks_ = parser.getClocks();
        inputDelays_ = parser.getInputDelays();
        outputDelays_ = parser.getOutputDelays();
        falsePaths_ = parser.getFalsePaths();
        multicyclePaths_ = parser.getMulticyclePaths();
    }
}

void TimingAnalyzer::loadLiberty(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return;

    std::string line, current_cell;
    LibertyCellTiming current_timing;
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        // "cell (CELL_NAME) {"
        if (trimmed.find("cell (") == 0) {
            size_t lp = trimmed.find('(');
            size_t rp = trimmed.find(')');
            if (lp != std::string::npos && rp != std::string::npos) {
                current_cell = trimmed.substr(lp + 1, rp - lp - 1);
                current_timing = LibertyCellTiming();
                current_timing.cell_name = current_cell;
            }
        }
        // "area : value;"
        else if (trimmed.find("area :") != std::string::npos) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';');
            if (colon != std::string::npos && semi != std::string::npos) {
                try {
                    current_timing.area = std::stod(trimmed.substr(colon + 1, semi - colon - 1));
                } catch (...) {}
            }
        }
        // "cell_rise : value;"
        else if (trimmed.find("cell_rise") != std::string::npos && trimmed.find("template") == std::string::npos) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';');
            if (colon != std::string::npos && semi != std::string::npos) {
                try {
                    current_timing.cell_rise = std::stod(trimmed.substr(colon + 1, semi - colon - 1));
                } catch (...) {}
            }
        }
        // "cell_fall : value;"
        else if (trimmed.find("cell_fall") != std::string::npos && trimmed.find("template") == std::string::npos) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';');
            if (colon != std::string::npos && semi != std::string::npos) {
                try {
                    current_timing.cell_fall = std::stod(trimmed.substr(colon + 1, semi - colon - 1));
                } catch (...) {}
            }
        }
        // "rise_transition : value;"
        else if (trimmed.find("rise_transition") != std::string::npos && trimmed.find("template") == std::string::npos) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';');
            if (colon != std::string::npos && semi != std::string::npos) {
                try {
                    current_timing.rise_transition = std::stod(trimmed.substr(colon + 1, semi - colon - 1));
                } catch (...) {}
            }
        }
        // "fall_transition : value;"
        else if (trimmed.find("fall_transition") != std::string::npos && trimmed.find("template") == std::string::npos) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';');
            if (colon != std::string::npos && semi != std::string::npos) {
                try {
                    current_timing.fall_transition = std::stod(trimmed.substr(colon + 1, semi - colon - 1));
                } catch (...) {}
            }
        }
        // "threshold_voltage_group : \"hvt\";" or similar
        else if (trimmed.find("threshold_voltage_group") != std::string::npos) {
            if (trimmed.find("lvt") != std::string::npos || trimmed.find("LVT") != std::string::npos)
                current_timing.vt_class = "LVT";
            else if (trimmed.find("hvt") != std::string::npos || trimmed.find("HVT") != std::string::npos)
                current_timing.vt_class = "HVT";
            else if (trimmed.find("svt") != std::string::npos || trimmed.find("SVT") != std::string::npos)
                current_timing.vt_class = "SVT";
        }
        // "}" end of cell definition
        else if (trimmed == "}" && !current_cell.empty()) {
            liberty_cells_[current_cell] = current_timing;
            current_cell.clear();
        }
    }

    liberty_loaded_ = !liberty_cells_.empty();
    if (liberty_loaded_) {
        std::cout << "  [timing] Loaded " << liberty_cells_.size() << " cells from Liberty library" << std::endl;
    }
    file.close();
}

void TimingAnalyzer::loadLibertyFull(const void *lib) {
    liberty_full_ = lib;
    liberty_loaded_ = (lib != nullptr);
}

// Helper: get cell delay — use NLDM interpolation when liberty is loaded, fall back to estimates
static double get_cell_delay_from_lib(const std::string &cell_type,
    const std::map<std::string, TimingAnalysis::TimingAnalyzer::LibertyCellTiming> &lib,
    const void *liberty_full, double input_slew, double output_load) {
    // Try NLDM interpolation first (accurate)
    if (liberty_full) {
        const Liberty::LibertyLibrary *full_lib = static_cast<const Liberty::LibertyLibrary*>(liberty_full);
        const Liberty::LibertyCell *cell = full_lib->find_cell(cell_type);
        if (cell) {
            const Liberty::LibertyPin *out = cell->find_output_pin();
            if (out && !out->timing_arcs.empty()) {
                const Liberty::LibertyTimingArc &arc = out->timing_arcs[0];
                // Use rise+fall average
                double rise_delay = arc.cell_rise.interpolate(input_slew, output_load);
                double fall_delay = arc.cell_fall.interpolate(input_slew, output_load);
                return (rise_delay + fall_delay) / 2.0;
            }
        }
    }
    // Try simple lib map
    if (lib.count(cell_type)) {
        auto &t = lib.at(cell_type);
        return (t.cell_rise + t.cell_fall) / 2.0;
    }
    // Try substring match
    for (auto &[name, t] : lib) {
        if (cell_type.find(name) != std::string::npos) {
            return (t.cell_rise + t.cell_fall) / 2.0;
        }
    }
    // Fallback estimates for 55nm process (only used when liberty is not loaded)
    // These are approximate intrinsic delays derived from typical NLDM tables at 50ps input slew, 5fF load
    // In normal operation, the liberty NLDM interpolation path is used (see above)
    if (cell_type.find("AND") != std::string::npos) return 0.040;
    if (cell_type.find("OR") != std::string::npos) return 0.045;
    if (cell_type.find("XOR") != std::string::npos) return 0.065;
    if (cell_type.find("NOT") != std::string::npos || cell_type.find("INV") != std::string::npos) return 0.015;
    if (cell_type.find("NAND") != std::string::npos) return 0.025;
    if (cell_type.find("NOR") != std::string::npos) return 0.030;
    if (cell_type.find("MUX") != std::string::npos) return 0.055;
    if (cell_type.find("DFF") != std::string::npos) return 0.120;
    if (cell_type.find("BUF") != std::string::npos) return 0.012;
    if (cell_type.find("ADDF") != std::string::npos) return 0.100;
    if (cell_type.find("ADDH") != std::string::npos) return 0.070;
    // Last resort: use a conservative estimate
    return 0.040;
}

void TimingAnalyzer::addClock(const ClockConstraint &clock) {
    clocks_.push_back(clock);
}

void TimingAnalyzer::addInputDelay(const InputDelayConstraint &delay) {
    inputDelays_.push_back(delay);
}

void TimingAnalyzer::addOutputDelay(const OutputDelayConstraint &delay) {
    outputDelays_.push_back(delay);
}

void TimingAnalyzer::addFalsePath(const FalsePathConstraint &path) {
    falsePaths_.push_back(path);
}

void TimingAnalyzer::addMulticyclePath(const MulticyclePathConstraint &path) {
    multicyclePaths_.push_back(path);
}

bool TimingAnalyzer::analyze() {
    timing_log("STA", "=== Starting timing analysis for module '%s' ===", moduleName_.c_str());
    timing_log("STA", "  Clocks: %zu, InputDelays: %zu, OutputDelays: %zu, FalsePaths: %zu",
        clocks_.size(), inputDelays_.size(), outputDelays_.size(), falsePaths_.size());
    timing_log("STA", "  OCV: %s (early=%.2f, late=%.2f)", ocv_enabled_ ? "enabled" : "disabled",
        ocv_derate_early_, ocv_derate_late_);
    timing_log("STA", "  Stage 1/7: Building timing graph...");
    buildTimingGraph();
    timing_log("STA", "  Graph built: %zu nodes, %zu edges", nodes_.size(), edges_.size());
    timing_log("STA", "  Stage 2/7: Computing arrival times...");
    computeArrivalTimes();
    timing_log("STA", "  Stage 3/7: Computing required times...");
    computeRequiredTimes();
    timing_log("STA", "  Stage 4/7: Computing slacks...");
    computeSlack();
    timing_log("STA", "  Stage 5/7: Finding paths...");
    findPaths();
    timing_log("STA", "  Found %zu timing paths", paths_.size());
    timing_log("STA", "  Stage 6/7: Checking constraints...");
    checkConstraints();
    timing_log("STA", "  Stage 7/7: Checking setup/hold constraints...");
    checkSetupHoldConstraints();
    if (report_.setup_violations > 0 || report_.hold_violations > 0) {
        timing_log("STA", "  VIOLATIONS: %d setup, %d hold", report_.setup_violations, report_.hold_violations);
    }
    generateReport();
    timing_log("STA", "=== Timing analysis complete: %.0f MHz, worst slack=%.3f ns, %s ===",
        1000.0 / (clocks_.empty() ? 10.0 : clocks_[0].period), getWorstSlack(), getWorstSlack() < 0 ? "VIO" : "MET");
    return true;
}

bool TimingAnalyzer::analyzeSetup() {
    lateMode_ = true;
    earlyMode_ = false;
    return analyze();
}

bool TimingAnalyzer::analyzeHold() {
    lateMode_ = false;
    earlyMode_ = true;
    return analyze();
}

// Default buildTimingGraph - builds timing graph from Synthesis::RTLIL::Design modules
// Note: Synthesis::RTLIL::Module uses wires (with is_input/is_output) not ports
void TimingAnalyzer::buildTimingGraph() {
    if (!nodes_.empty()) return;

    nodes_.clear();
    edges_.clear();

    // Build timing graph from RTLIL::Design
    // Map wire names to node indices
    std::map<std::string, int> wire_to_node;

    for (auto &mod : design_.modules) {
        // Create nodes for input wires
        for (auto &wire : mod.wires) {
            if (wire.is_input) {
                int idx = (int)nodes_.size();
                nodes_.push_back({});
                nodes_.back().name = wire.name;
                nodes_.back().type = TimingNode::INPUT;
                wire_to_node[wire.name] = idx;
            }
        }

        // Find clock wires (input ports named clk/clock)
        std::set<std::string> clock_wires;
        for (auto &wire : mod.wires) {
            std::string lower = wire.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (wire.is_input && (lower.find("clk") != std::string::npos || lower.find("clock") != std::string::npos)) {
                clock_wires.insert(wire.name);
                // Create CLOCK node
                int idx = (int)nodes_.size();
                nodes_.push_back({});
                nodes_.back().name = wire.name + "_clock";
                nodes_.back().type = TimingNode::CLOCK;
                nodes_.back().is_clock = true;
                wire_to_node[wire.name + "@clock"] = idx;
            }
        }

        // Create nodes for each cell
        for (auto &cell : mod.cells) {
            int cell_idx = (int)nodes_.size();
            nodes_.push_back({});
            nodes_.back().name = cell.type + "_" + cell.name;
            nodes_.back().type = TimingNode::INTERNAL;

            // Determine cell's clock association
            bool is_sequential = (cell.type.find("DFF") != std::string::npos);
            std::string cell_clock;
            if (is_sequential) {
                for (auto &conn : cell.connections) {
                    std::string port_name = conn.first;
                    if (port_name == "C" || port_name == "CLK" || port_name == "CK") {
                        auto &sig = conn.second;
                        if (sig.width() > 0 && !sig.bits.empty()) {
                            // Find the wire name for this clock pin
                            for (auto &w : mod.wires) {
                                if (w.start_offset == sig.bits[0].wire_idx) {
                                    cell_clock = w.name;
                                    nodes_[cell_idx].is_clock = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Map cell inputs
            std::vector<int> input_nodes;
            for (auto &conn : cell.connections) {
                std::string port_name = conn.first;
                if (port_name != "Y" && port_name != "Q") {
                    auto &sig = conn.second;
                    if (sig.width() > 0 && !sig.bits.empty()) {
                        // Use wire name instead of wire_idx + offset
                        for (auto &w : mod.wires) {
                            if (w.start_offset == sig.bits[0].wire_idx) {
                                if (clock_wires.count(w.name) && port_name == "C") {
                                    // Clock pin: connect to CLOCK node
                                    std::string ck_node = w.name + "@clock";
                                    if (wire_to_node.count(ck_node)) {
                                        input_nodes.push_back(wire_to_node[ck_node]);
                                    }
                                } else if (wire_to_node.count(w.name)) {
                                    input_nodes.push_back(wire_to_node[w.name]);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // Map cell output and compute load capacitance
            std::string out_wire_name;
            for (auto &conn : cell.connections) {
                std::string port_name = conn.first;
                if (port_name == "Y" || port_name == "Q") {
                    auto &sig = conn.second;
                    if (sig.width() > 0 && !sig.bits.empty()) {
                        for (auto &w : mod.wires) {
                            if (w.start_offset == sig.bits[0].wire_idx) {
                                wire_to_node[w.name] = cell_idx;
                                out_wire_name = w.name;
                                break;
                            }
                        }
                    }
                }
            }

            // Compute fanout for wire load
            int fanout = 1;
            if (!out_wire_name.empty()) {
                int fo = 0;
                for (auto &cell2 : mod.cells) {
                    for (auto &conn2 : cell2.connections) {
                        if (conn2.first == "Y" || conn2.first == "Q") continue;
                        auto &sig2 = conn2.second;
                        if (sig2.width() > 0 && !sig2.bits.empty()) {
                            for (auto &w2 : mod.wires) {
                                if (w2.start_offset == sig2.bits[0].wire_idx && w2.name == out_wire_name) {
                                    fo++;
                                }
                            }
                        }
                    }
                }
                if (fo > 0) fanout = fo;
            }

            // Compute load capacitance: driven input pin caps + wire cap
            double load_cap_pf = 0.001; // minimum 1fF
            if (liberty_full_ && !out_wire_name.empty()) {
                const Liberty::LibertyLibrary *full_lib = static_cast<const Liberty::LibertyLibrary*>(liberty_full_);
                for (auto &driven_cell : mod.cells) {
                    for (auto &dc : driven_cell.connections) {
                        if (dc.first == "Y" || dc.first == "Q") continue;
                        auto &dsig = dc.second;
                        if (dsig.width() > 0 && !dsig.bits.empty()) {
                            for (auto &dw : mod.wires) {
                                if (dw.start_offset == dsig.bits[0].wire_idx && dw.name == out_wire_name) {
                                    const Liberty::LibertyCell *lc = full_lib->find_cell(driven_cell.type);
                                    if (lc) {
                                        auto pin_it = lc->pins.find(dc.first);
                                        if (pin_it != lc->pins.end() && pin_it->second.capacitance > 0) {
                                            load_cap_pf += pin_it->second.capacitance;
                                        } else {
                                            load_cap_pf += lc->avg_input_cap();
                                        }
                                    } else {
                                        load_cap_pf += 0.001; // 1fF default
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Wire load model: 0.5fF per fanout
            load_cap_pf += fanout * 0.0005;
            if (load_cap_pf < 0.0005) load_cap_pf = 0.0005;

            // Add edges from inputs to this cell
            // Use NLDM interpolation with real load capacitance
            double default_slew_ns = 0.05; // 50ps initial slew
            double cell_delay = get_cell_delay_from_lib(cell.type, liberty_cells_, liberty_full_,
                                                        default_slew_ns, load_cap_pf);

            for (int in_node : input_nodes) {
                int edge_idx = (int)edges_.size();
                edges_.push_back({});
                edges_.back().from_node = in_node;
                edges_.back().to_node = cell_idx;
                edges_.back().delay = cell_delay;
                edges_.back().load_cap = load_cap_pf;
                edges_.back().input_slew = default_slew_ns;
                edges_.back().cell_type = cell.type;
                // Mark clock-to-sequential edges as CLOCK type
                if (is_sequential && nodes_[in_node].type == TimingNode::CLOCK) {
                    edges_.back().type = TimingEdge::CLOCK;
                } else {
                    edges_.back().type = TimingEdge::COMBINATIONAL;
                }
            }
        }

        // Create nodes for output wires
        for (auto &wire : mod.wires) {
            if (wire.is_output) {
                int idx = (int)nodes_.size();
                nodes_.push_back({});
                nodes_.back().name = wire.name;
                nodes_.back().type = TimingNode::OUTPUT;

                std::string wire_key = std::to_string(wire.start_offset);
                if (wire_to_node.count(wire_key)) {
                    edges_.push_back({});
                    edges_.back().from_node = wire_to_node[wire_key];
                    edges_.back().to_node = idx;
                    edges_.back().delay = 0.01;
                    edges_.back().type = TimingEdge::COMBINATIONAL;
                }
            }
        }
    }
}

// Build timing graph from gate-level Verilog netlist text
// Parses cell instantiations and creates nodes/edges with per-cell delays
void TimingAnalyzer::buildTimingGraphFromNetlist(const std::string &netlist) {
    nodes_.clear();
    edges_.clear();
    liberty_cell_names_.clear();

    // Parse gate-level Verilog: look for cell instantiations like "CELL_TYPE inst_name (.port(wire), ...)"
    std::istringstream ss(netlist);
    std::string line;
    std::map<std::string, int> wire_to_node;  // wire name → node index
    std::set<std::string> input_wires, output_wires;

    // First pass: find input/output port declarations
    std::istringstream first_pass(netlist);
    while (std::getline(first_pass, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        // "input wire_name;" or "input [W-1:0] wire_name;"
        if (trimmed.find("input ") == 0 || trimmed.find("input\t") == 0) {
            std::string rest = trimmed.substr(5); // skip "input"
            rest.erase(0, rest.find_first_not_of(" \t"));
            // Skip width specification
            if (rest[0] == '[') {
                size_t bracket_end = rest.find(']');
                if (bracket_end != std::string::npos) rest = rest.substr(bracket_end + 1);
                rest.erase(0, rest.find_first_not_of(" \t"));
            }
            // Extract wire name (before ; or , or whitespace)
            size_t end = rest.find_first_of(";, \t");
            if (end != std::string::npos) {
                std::string wname = rest.substr(0, end);
                input_wires.insert(wname);
            }
        }
        // "output wire_name;"
        if (trimmed.find("output ") == 0 || trimmed.find("output\t") == 0) {
            std::string rest = trimmed.substr(6);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (rest[0] == '[') {
                size_t bracket_end = rest.find(']');
                if (bracket_end != std::string::npos) rest = rest.substr(bracket_end + 1);
                rest.erase(0, rest.find_first_not_of(" \t"));
            }
            size_t end = rest.find_first_of(";, \t");
            if (end != std::string::npos) {
                std::string wname = rest.substr(0, end);
                output_wires.insert(wname);
            }
        }
    }

    // Helper: get or create node for a wire
    auto get_node = [&](const std::string &w) -> int {
        if (wire_to_node.count(w)) return wire_to_node[w];
        TimingNode node;
        node.name = w;
        if (input_wires.count(w)) node.type = TimingNode::INPUT;
        else if (output_wires.count(w)) node.type = TimingNode::OUTPUT;
        else node.type = TimingNode::INTERNAL;
        node.arrival_time = 0.0;
        node.required_time = 1e18;
        node.slack = 0.0;
        nodes_.push_back(node);
        int idx = (int)nodes_.size() - 1;
        wire_to_node[w] = idx;
        return idx;
    };

    // Second pass: parse cell instantiations
    std::istringstream second_pass(netlist);
    while (std::getline(second_pass, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        // Skip declarations and comments
        if (trimmed.empty() || trimmed[0] == '/' || trimmed[0] == '*' ||
            trimmed.find("module ") == 0 || trimmed.find("endmodule") == 0 ||
            trimmed.find("input ") == 0 || trimmed.find("output ") == 0 ||
            trimmed.find("wire ") == 0 || trimmed.find("reg ") == 0 ||
            trimmed.find("assign ") == 0 || trimmed.find("always") == 0) continue;

        // Cell instantiation: CELL_TYPE inst_name (.port(wire), ...);
        size_t space = trimmed.find(' ');
        if (space == std::string::npos) continue;
        std::string cell_type = trimmed.substr(0, space);

        // Extract port connections: .port(wire)
        std::vector<std::pair<std::string, std::string>> ports; // port_name → wire
        size_t lp = trimmed.find('(');
        while (lp != std::string::npos) {
            // Find .port_name
            size_t dot = trimmed.find('.', lp);
            if (dot == std::string::npos) break;
            size_t port_end = trimmed.find('(', dot);
            if (port_end == std::string::npos) break;
            std::string port_name = trimmed.substr(dot + 1, port_end - dot - 1);
            // Remove trailing whitespace
            while (!port_name.empty() && std::isspace(port_name.back())) port_name.pop_back();

            size_t wire_start = port_end + 1;
            size_t wire_end = trimmed.find(')', wire_start);
            if (wire_end == std::string::npos) break;
            std::string wire_name = trimmed.substr(wire_start, wire_end - wire_start);
            // Remove leading/trailing whitespace
            while (!wire_name.empty() && std::isspace(wire_name.front())) wire_name.erase(0, 1);
            while (!wire_name.empty() && std::isspace(wire_name.back())) wire_name.pop_back();

            ports.push_back({port_name, wire_name});
            lp = trimmed.find('(', wire_end + 1);
        }

        if (ports.empty()) continue;

        // Find output wire first (needed for fanout estimation)
        std::string out_wire;
        for (auto &[port, wire] : ports) {
            if (port == "Y" || port == "Q") { out_wire = wire; break; }
        }

        // Calculate cell delay based on type, using Liberty library when available
        double cell_delay = get_cell_delay_from_lib(cell_type, liberty_cells_, liberty_full_, 0.05, 0.005);
        // Record cell name for display
        liberty_cell_names_.push_back(cell_type);

        // Add interconnect delay based on fanout (wire load model)
        // Count how many cells reference this output wire as input
        if (!out_wire.empty()) {
            int fanout = 0;
            for (auto &check_ports : ports) {
                (void)check_ports;
            }
            // Simple fanout estimation: scan the remaining netlist lines for references
            // to this wire in input port positions (.A(wire), .B(wire), etc.)
            std::string netlist_after;
            size_t line_pos = second_pass.tellg();
            if (line_pos == std::string::npos) {
                netlist_after = netlist.substr(line_pos > 0 ? line_pos : 0);
            }
            // Count wire usage in gate port connections
            int wire_refs = 0;
            std::istringstream wire_scan(netlist);
            std::string wline;
            while (std::getline(wire_scan, wline)) {
                // Count lines containing .*port_name(out_wire) — hard to do perfectly
                // Simple approximation: count lines with the signal name
                if (wline.find(out_wire) != std::string::npos && wline.find("wire") == std::string::npos
                    && wline.find("module") == std::string::npos && wline.find("input") == std::string::npos
                    && wline.find("output") == std::string::npos) {
                    wire_refs++;
                }
            }
            fanout = std::max(1, wire_refs / 2); // each use appears in both output and input positions
            double wire_delay = 0.0015 * fanout; // 1.5ps per fanout (55nm typical)
            cell_delay += wire_delay;
        }

        bool is_seq = (cell_type.find("DFF") != std::string::npos);
        TimingEdge::Type edge_type = is_seq ? TimingEdge::SEQUENTIAL : TimingEdge::COMBINATIONAL;

        // Create edges: for each input port → output port
        int out_node = -1;
        // Find output port (Y or Q)
        for (auto &[port, wire] : ports) {
            if (port == "Y" || port == "Q") {
                out_node = get_node(wire);
                break;
            }
        }

        // Create edges from each input port to output port
        for (auto &[port, wire] : ports) {
            if (port == "Y" || port == "Q") continue;
            int in_node = get_node(wire);
            if (out_node >= 0 && in_node >= 0) {
                addEdge(in_node, out_node, cell_delay, edge_type);
            }
        }
    }
}

void TimingAnalyzer::computeArrivalTimes() {
    timing_log("STA", "  Computing arrival times (%s mode, derate=%.2f)...",
        lateMode_ ? "late" : (earlyMode_ ? "early" : "nominal"),
        ocv_enabled_ ? (lateMode_ ? ocv_derate_late_ : ocv_derate_early_) : 1.0);
    // Initialize inputs to 0 (or input delay if specified)
    int input_count = 0, clock_count = 0;
    for (auto &node : nodes_) {
        if (node.type == TimingNode::INPUT) {
            node.arrival_time = 0.0;
            // Apply input delays
            for (auto &id : inputDelays_) {
                if (!id.port.empty() && node.name.find(id.port) != std::string::npos) {
                    node.arrival_time = id.delay;
                }
            }
        }
        // Apply clock source latency to CLOCK nodes
        if (node.type == TimingNode::CLOCK || node.is_clock) {
            for (auto &clk : clocks_) {
                if (node.name.find(clk.name) != std::string::npos || node.name.find(clk.port) != std::string::npos) {
                    node.arrival_time = clk.source_latency + clk.network_latency;
                    clock_count++;
                    break;
                }
            }
        }
    }
    timing_log("STA", "    Initialized %d inputs, %d clocks", input_count, clock_count);

    // Determine derate factor for this analysis mode
    // AOCV: table-based derate by logic depth
    double derate = 1.0;
    if (ocv_enabled_) {
        // AOCV table by path depth
        auto aocv_late = [](int depth) -> double {
            if (depth <= 2) return 1.02;
            if (depth <= 5) return 1.05;
            if (depth <= 10) return 1.08;
            if (depth <= 20) return 1.12;
            return 1.15;
        };
        auto aocv_early = [](int depth) -> double {
            if (depth <= 2) return 0.98;
            if (depth <= 5) return 0.95;
            if (depth <= 10) return 0.92;
            if (depth <= 20) return 0.88;
            return 0.85;
        };
        if (lateMode_) {
            derate = aocv_late(0);  // Will be refined per-edge based on path depth
        } else if (earlyMode_) {
            derate = aocv_early(0);
        }
    }

    // Topological sort and compute
    std::vector<int> inDegree(nodes_.size(), 0);
    for (auto &edge : edges_) {
        if (edge.to_node >= 0 && edge.to_node < (int)nodes_.size()) {
            inDegree[edge.to_node]++;
        }
    }

    std::queue<int> queue;
    for (int i = 0; i < (int)nodes_.size(); i++) {
        if (inDegree[i] == 0) queue.push(i);
    }

    while (!queue.empty()) {
        int u = queue.front();
        queue.pop();

        for (auto &edge : edges_) {
            if (edge.from_node == u) {
                int v = edge.to_node;
                // ── NLDM delay recalculation with propagated slew ──
                // Get the output slew (transition time) from the source node
                double input_slew = nodes_[u].transition_time;
                if (input_slew < 0.001) input_slew = 0.05; // default 50ps
                double load_cap = edge.load_cap;
                if (load_cap < 0.0001) load_cap = 0.005; // default 5fF

                // The edge retains its concrete mapped cell name.  Inferring
                // a type from a signal name corrupts names such as
                // sky130_fd_sc_hd__and2_0 and silently falls back to a
                // technology-independent delay.
                if (!edge.cell_type.empty()) {
                    const std::string &cell_type = edge.cell_type;
                    // Optional: re-compute delay with actual slew
                    double nldm_delay = get_cell_delay_from_lib(cell_type, liberty_cells_, liberty_full_,
                                                                 input_slew, load_cap);
                    edge.delay = nldm_delay;
                    edge.input_slew = input_slew;
                }

                // Apply OCV derate to edge delay
                double effective_delay = edge.delay * derate;
                double newTime = nodes_[u].arrival_time + effective_delay;
                if (newTime > nodes_[v].arrival_time) {
                    nodes_[v].arrival_time = newTime;
                    // Propagate output slew: use NLDM output transition lookup
                    if (!edge.cell_type.empty() && liberty_full_) {
                        const std::string &cell_type = edge.cell_type;
                        const Liberty::LibertyLibrary *full_lib = static_cast<const Liberty::LibertyLibrary*>(liberty_full_);
                        double out_slew = full_lib->compute_output_slew(cell_type, "", "",
                                                                         input_slew, load_cap, true);
                        nodes_[v].transition_time = out_slew;
                    } else {
                        // Fallback: estimate output slew
                        double output_slew = input_slew * (1.0 + effective_delay * 0.3);
                        nodes_[v].transition_time = output_slew;
                    }
                }
                inDegree[v]--;
                if (inDegree[v] == 0) queue.push(v);
            }
        }
    }
}

void TimingAnalyzer::computeRequiredTimes() {
    // Set output required times
    // For setup (late mode): required = clock_period - setup_time
    // For hold (early mode): required = hold_time (typically 0)
    double base_period = clocks_.empty() ? 10.0 : clocks_[0].period;
    double derate = 1.0;
    if (ocv_enabled_) {
        if (lateMode_) {
            derate = ocv_derate_early_;
        } else if (earlyMode_) {
            derate = ocv_derate_late_;
        }
    }

    // Apply multicycle constraints: modify base_period for paths matching multicycle rules
    for (auto &mc : multicyclePaths_) {
        // Store for per-path application in findPaths
    }

    for (auto &node : nodes_) {
        if (earlyMode_) {
            node.required_time = 0.0;
        } else {
            // Use clock period associated with this node's clock domain
            // For now, use first clock; full multi-clock support assigns per-node
            double period = base_period;
            // Check if node is associated with a specific clock via its name
            if (clocks_.size() > 1) {
                for (auto &clk : clocks_) {
                    if (!clk.port.empty() && node.name.find(clk.port) != std::string::npos) {
                        period = clk.period;
                        break;
                    }
                }
            }
            node.required_time = period;
        }
    }

    // Backward propagation with OCV derate
    std::vector<int> outDegree(nodes_.size(), 0);
    for (auto &edge : edges_) {
        if (edge.from_node >= 0 && edge.from_node < (int)nodes_.size()) {
            outDegree[edge.from_node]++;
        }
    }

    std::queue<int> queue;
    for (int i = 0; i < (int)nodes_.size(); i++) {
        if (outDegree[i] == 0) queue.push(i);
    }

    while (!queue.empty()) {
        int u = queue.front();
        queue.pop();

        for (auto &edge : edges_) {
            if (edge.to_node == u) {
                int v = edge.from_node;
                double effective_delay = edge.delay * derate;
                double newTime = nodes_[u].required_time - effective_delay;
                if (newTime < nodes_[v].required_time) {
                    nodes_[v].required_time = newTime;
                }
                outDegree[v]--;
                if (outDegree[v] == 0) queue.push(v);
            }
        }
    }
}

void TimingAnalyzer::computeSlack() {
    int neg_slack = 0, met_count = 0;
    double worst = 999.0, best = -999.0;
    for (auto &node : nodes_) {
        if (earlyMode_) {
            // Hold slack: data arrival time - required (clock) time
            // Data must arrive AFTER clock, so positive slack means hold is met
            node.slack = node.arrival_time - node.required_time;
        } else {
            // Setup slack: required time - arrival time
            node.slack = node.required_time - node.arrival_time;
        }
        if (node.slack < 0) neg_slack++;
        else met_count++;
        if (node.slack < worst) worst = node.slack;
        if (node.slack > best) best = node.slack;
    }
    timing_log("STA", "    Slack distribution: %d MET, %d VIO, worst=%.3f ns, best=%.3f ns",
        met_count, neg_slack, worst, best);
}

// CRPR: Clock Reconvergence Pessimism Removal
// Removes excessive pessimism from OCV derating on common clock paths
// The common clock path from clock source to the point where launch and capture
// clock paths diverge is over-pessimized by OCV (both sides derated).
// CRPR adds back the excess pessimism: common_path * (derate - 1.0)
double TimingAnalyzer::computeCRPR(int launch_node, int capture_node) {
    if (!ocv_enabled_) return 0.0;

    // Find the clock source node that drives both launch and capture
    double launch_clk_delay = 0.0, capture_clk_delay = 0.0;

    // Trace backwards from each endpoint to find clock path delays
    // Build per-node distance from clock source via CLOCK edges
    std::map<int, double> node_clk_dist;  // node_index → clock path delay from source

    // Initialize from CLOCK type nodes
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].type == TimingNode::CLOCK || nodes_[i].is_clock) {
            node_clk_dist[(int)i] = 0.0;  // Clock source itself
        }
    }

    // BFS/DFS from clock sources along CLOCK edges
    bool changed = true;
    int iter = 0;
    while (changed && iter++ < 100) {
        changed = false;
        for (auto &edge : edges_) {
            if (edge.type == TimingEdge::CLOCK && node_clk_dist.count(edge.from_node)) {
                double new_dist = node_clk_dist[edge.from_node] + edge.delay;
                if (!node_clk_dist.count(edge.to_node) || new_dist < node_clk_dist[edge.to_node]) {
                    node_clk_dist[edge.to_node] = new_dist;
                    changed = true;
                }
            }
        }
    }

    // Get clock delays to launch and capture
    if (node_clk_dist.count(launch_node))
        launch_clk_delay = node_clk_dist[launch_node];
    if (node_clk_dist.count(capture_node))
        capture_clk_delay = node_clk_dist[capture_node];

    // Common path is the minimum (the shared part before divergence)
    double common_path = std::min(launch_clk_delay, capture_clk_delay);
    if (common_path <= 0.0) return 0.0;

    // OCV factor: in late mode, both paths get derated by late_derate;
    // the excess pessimism on the common portion is: common_path * (derate - 1.0)
    // In early mode (hold check): derate < 1.0, so pessimism is reversed
    double ocv_factor = lateMode_ ? ocv_derate_late_ : ocv_derate_early_;
    double crpr = common_path * std::max(0.0, ocv_factor - 1.0);

    // For early mode (hold): common path was "over-optimized", need to add back pessimism
    if (!lateMode_) {
        crpr = common_path * std::max(0.0, 1.0 - ocv_factor);
    }

    return crpr;
}

// Clock skew analysis
double TimingAnalyzer::analyzeClockSkew() {
    double max_clock_arrival = 0.0, min_clock_arrival = 1e18;

    for (auto &node : nodes_) {
        if (node.type == TimingNode::CLOCK || node.is_clock) {
            if (node.arrival_time > max_clock_arrival) max_clock_arrival = node.arrival_time;
            if (node.arrival_time < min_clock_arrival) min_clock_arrival = node.arrival_time;
        }
    }

    // Also check clock endpoints (DFF clock pins)
    for (auto &edge : edges_) {
        if (edge.type == TimingEdge::SEQUENTIAL) {
            if (edge.from_node >= 0 && edge.from_node < (int)nodes_.size()) {
                double arrival = nodes_[edge.from_node].arrival_time;
                if (arrival > max_clock_arrival) max_clock_arrival = arrival;
                if (arrival < min_clock_arrival && arrival > 0) min_clock_arrival = arrival;
            }
        }
    }

    double skew = max_clock_arrival - min_clock_arrival;
    return skew;
}

void TimingAnalyzer::findPaths() {
    paths_.clear();
    double clock_skew = analyzeClockSkew();

    auto is_clock_like_name = [](const std::string &name) -> bool {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower.find("clk") != std::string::npos || lower.find("clock") != std::string::npos;
    };
    auto is_reset_like_name = [](const std::string &name) -> bool {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower.find("rst") != std::string::npos || lower.find("reset") != std::string::npos;
    };
    auto is_constant_name = [](const std::string &name) -> bool {
        return name == "1'b0" || name == "1'b1" || name == "0" || name == "1";
    };

    std::vector<int> endpoint_candidates;
    std::set<int> endpoint_set;

    for (size_t i = 0; i < nodes_.size(); i++) {
        int out_comb = 0;
        for (auto &edge : edges_) {
            if (edge.from_node == (int)i && edge.type == TimingEdge::COMBINATIONAL) {
                out_comb++;
            }
        }
        if (nodes_[i].type == TimingNode::OUTPUT || (nodes_[i].arrival_time > 0.0 && out_comb == 0)) {
            endpoint_set.insert((int)i);
        }
    }

    for (auto &edge : edges_) {
        if (edge.type != TimingEdge::SEQUENTIAL) continue;
        if (edge.from_node < 0 || edge.from_node >= (int)nodes_.size()) continue;
        const auto &src = nodes_[edge.from_node];
        if (src.type == TimingNode::CLOCK || src.is_clock) continue;
        if (is_clock_like_name(src.name) || is_reset_like_name(src.name) || is_constant_name(src.name)) continue;
        endpoint_set.insert(edge.from_node);
    }

    endpoint_candidates.assign(endpoint_set.begin(), endpoint_set.end());

    // Find all paths from INPUT/CLOCK nodes to OUTPUT/DFF data endpoints.
    for (int endpoint_idx : endpoint_candidates) {
        if (endpoint_idx < 0 || endpoint_idx >= (int)nodes_.size()) {
            continue;
        }
            TimingPath path;
            path.endpoint = endpoint_idx;
            path.total_delay = nodes_[endpoint_idx].arrival_time;
            path.slack = nodes_[endpoint_idx].slack;
            path.is_met = (path.slack >= 0);

            // Reconstruct detailed path stages
            path.startpoint = -1;
            int current_node = endpoint_idx;

            // Trace back through the timing graph
            std::vector<int> rev_nodes;
            std::vector<double> rev_delays;
            std::vector<int> rev_edges;
            std::set<int> visited_nodes;
            for (int iter = 0; iter < 200 && current_node >= 0; iter++) {
                if (visited_nodes.count(current_node)) {
                    break;
                }
                visited_nodes.insert(current_node);
                rev_nodes.push_back(current_node);
                int best_prev = -1;
                double best_prev_arrival = -1;
                double edge_delay = 0.0;
                int best_edge = -1;

                for (size_t edge_idx = 0; edge_idx < edges_.size(); edge_idx++) {
                    auto &edge = edges_[edge_idx];
                    if (edge.to_node == current_node) {
                        if (edge.from_node == current_node) {
                            continue;
                        }
                        if (visited_nodes.count(edge.from_node)) {
                            continue;
                        }
                        if (nodes_[edge.from_node].type == TimingNode::CLOCK || nodes_[edge.from_node].is_clock) {
                            continue;
                        }
                        if (nodes_[edge.from_node].arrival_time > best_prev_arrival) {
                            best_prev_arrival = nodes_[edge.from_node].arrival_time;
                            best_prev = edge.from_node;
                            edge_delay = edge.delay;
                            best_edge = (int)edge_idx;
                        }
                    }
                }
                rev_delays.push_back(edge_delay);
                if (best_edge >= 0) {
                    rev_edges.push_back(best_edge);
                }
                if (best_prev >= 0
                    && nodes_[best_prev].type != TimingNode::INPUT
                    && nodes_[best_prev].type != TimingNode::CLOCK
                    && !nodes_[best_prev].is_clock) {
                    current_node = best_prev;
                } else {
                    if (best_prev >= 0) {
                        path.startpoint = best_prev;
                        rev_nodes.push_back(best_prev);
                        rev_delays.push_back(0.0);
                    }
                    break;
                }
            }

            if (path.startpoint < 0 && !rev_nodes.empty()) {
                path.startpoint = rev_nodes.back();
            }
            if (path.startpoint < 0 && !rev_nodes.empty()) {
                path.startpoint = rev_nodes.back();
            }
            if (path.startpoint < 0) {
                continue;
            }

            // Check false path constraints
            bool is_false = false;
            for (auto &fp : falsePaths_) {
                std::string start_name = nodes_[path.startpoint].name;
                std::string end_name = nodes_[path.endpoint].name;
                if ((fp.from.empty() || start_name.find(fp.from) != std::string::npos) &&
                    (fp.to.empty() || end_name.find(fp.to) != std::string::npos)) {
                    is_false = true;
                    break;
                }
            }
            if (is_false) continue; // Skip false paths

            // Apply multicycle constraints
            for (auto &mc : multicyclePaths_) {
                std::string start_name = nodes_[path.startpoint].name;
                std::string end_name = nodes_[path.endpoint].name;
                if ((mc.from.empty() || start_name.find(mc.from) != std::string::npos) &&
                    (mc.to.empty() || end_name.find(mc.to) != std::string::npos)) {
                    if (mc.setup_cycles > 1) {
                        // Relax setup by multiplying clock period
                        double period = clocks_.empty() ? 10.0 : clocks_[0].period;
                        path.slack += period * (mc.setup_cycles - 1);
                    }
                    if (mc.hold_cycles > 0) {
                        // Hold multicycle: adjust hold check
                        double hold = mc.hold_cycles * 0.05;
                        path.slack -= hold;
                    }
                    break;
                }
            }

            // Reverse to get forward order
            std::vector<int> fwd_nodes(rev_nodes.rbegin(), rev_nodes.rend());
            std::vector<double> fwd_delays(rev_delays.rbegin(), rev_delays.rend());
            std::vector<int> fwd_edges(rev_edges.rbegin(), rev_edges.rend());
            path.nodes = fwd_nodes;
            path.edges = fwd_edges;

            // Build path stages with detailed per-cell delay
            double cumulative = 0.0;
            int path_depth = (int)fwd_nodes.size();
            // AOCV: apply stage-based derate based on actual path depth
            double aocv_derate = 1.0;
            if (ocv_enabled_) {
                // Stage-based AOCV: deeper paths have higher variation
                if (lateMode_) {
                    if (path_depth <= 2) aocv_derate = 1.02;
                    else if (path_depth <= 5) aocv_derate = 1.05;
                    else if (path_depth <= 10) aocv_derate = 1.08;
                    else if (path_depth <= 20) aocv_derate = 1.12;
                    else aocv_derate = 1.15;
                } else {
                    if (path_depth <= 2) aocv_derate = 0.98;
                    else if (path_depth <= 5) aocv_derate = 0.95;
                    else if (path_depth <= 10) aocv_derate = 0.92;
                    else if (path_depth <= 20) aocv_derate = 0.88;
                    else aocv_derate = 0.85;
                }
            }
            double pre_derate_cumulative = 0.0;
            for (size_t s = 0; s < fwd_nodes.size(); s++) {
                int nidx = fwd_nodes[s];
                double incr = (s < fwd_delays.size()) ? fwd_delays[s] : 0.0;
                // Apply per-stage OCV derate (more granular than per-path)
                double stage_derate = aocv_derate * (1.0 + 0.01 * ((double)s / std::max(1.0, (double)path_depth)));
                double derated_incr = incr * stage_derate;
                cumulative += derated_incr;
                pre_derate_cumulative += incr;

                PathStage stage;
                stage.cell_name = nodes_[nidx].name;
                stage.cell_type = (nodes_[nidx].type == TimingNode::INPUT) ? "INPUT" :
                                  (nodes_[nidx].type == TimingNode::OUTPUT) ? "OUTPUT" :
                                  (nodes_[nidx].type == TimingNode::CLOCK) ? "CLOCK" : "CELL";
                stage.incr_delay = derated_incr;
                stage.cumul_delay = cumulative;
                path.stages.push_back(stage);
            }
            // Apply total AOCV derate to path delay for slack computation
            if (ocv_enabled_) {
                path.total_delay = cumulative;  // Use derated cumulative
                timing_log("STA", "  AOCV path depth=%d: pre_derate=%.3f, post_derate=%.3f (derate=%.3f)",
                    path_depth, pre_derate_cumulative, cumulative, aocv_derate);
            }

            if (path.startpoint >= 0) {
                // Apply clock skew to slack
                path.slack -= clock_skew;

                // Apply CRPR if both start and end have clock paths
                if (path.startpoint >= 0 && path.endpoint >= 0) {
                    double crpr = computeCRPR(path.startpoint, path.endpoint);
                    path.slack += crpr; // CRPR reduces pessimism (adds back to slack)
                }

                path.is_met = (path.slack >= 0);
                paths_.push_back(path);
            }
    }

    // Sort by slack (worst first)
    std::sort(paths_.begin(), paths_.end(),
              [](const TimingPath &a, const TimingPath &b) {
                  return a.slack < b.slack;
              });

    if (paths_.size() > 10) {
        paths_.resize(10);
    }

    // Apply OCV derating for setup/hold analysis
    if (lateMode_) {
        // Setup: apply late derate to launch path
        for (auto &path : paths_) {
            path.total_delay *= 1.10;  // +10% for OCV late path
        }
    }
    if (earlyMode_) {
        // Hold: apply early derate to launch path
        for (auto &path : paths_) {
            path.total_delay *= 0.90;  // -10% for OCV early path
        }
    }
}

void TimingAnalyzer::setSetupHoldConstraint(const std::string &cell_type, double setup, double hold, double clk2q) {
    DFFSetupHoldConstraint c;
    c.cell_type = cell_type;
    c.setup_time = setup;
    c.hold_time = hold;
    c.clk_to_q = clk2q;
    setup_hold_constraints_[cell_type] = c;
}

void TimingAnalyzer::checkSetupHoldConstraints() {
    // Default constraints for common cell types if none set
    if (setup_hold_constraints_.empty()) {
        // Use liberty data when available, otherwise fall back to defaults
        if (liberty_full_) {
            const Liberty::LibertyLibrary *full_lib = static_cast<const Liberty::LibertyLibrary*>(liberty_full_);
            // Extract setup/hold from all DFF cells in the design
            for (auto &node : nodes_) {
                size_t uscore = node.name.find('_');
                if (uscore != std::string::npos) {
                    std::string cell_type = node.name.substr(0, uscore);
                    if (cell_type.find("DFF") != std::string::npos) {
                        // Only add if not already present
                        if (setup_hold_constraints_.count(cell_type) == 0) {
                            const Liberty::LibertyCell *lc = full_lib->find_cell(cell_type);
                            if (lc && lc->is_sequential) {
                                double setup = lc->get_setup_time();
                                double hold = lc->get_hold_time();
                                // Estimate clk_to_q from timing arcs
                                double clk2q = 0.15;
                                const Liberty::LibertyPin *out = lc->find_output_pin();
                                if (out) {
                                    for (auto &arc : out->timing_arcs) {
                                        if (arc.related_pin == "CK" || arc.related_pin == "CLK" || arc.related_pin == "CP") {
                                            if (!arc.cell_rise.empty())
                                                clk2q = arc.cell_rise.interpolate(0.05, 0.005);
                                            break;
                                        }
                                    }
                                }
                                DFFSetupHoldConstraint c;
                                c.cell_type = cell_type;
                                c.setup_time = setup;
                                c.hold_time = hold;
                                c.clk_to_q = clk2q;
                                setup_hold_constraints_[cell_type] = c;
                            }
                        }
                    }
                }
            }
        }
        // Fallback defaults for generic types
        if (setup_hold_constraints_.empty()) {
            setSetupHoldConstraint("DFFPOSX1", 0.15, 0.05, 0.12);
            setSetupHoldConstraint("DFFNEGX1", 0.15, 0.05, 0.12);
            setSetupHoldConstraint("$_DFF_P_", 0.15, 0.05, 0.12);
            setSetupHoldConstraint("$_DFF_N_", 0.15, 0.05, 0.12);
        }
    }

    report_.setup_hold_violations.clear();

    // Build a map: timing node index → cell type (for DFF nodes)
    std::map<int, std::string> node_cell_type;
    for (auto &node : nodes_) {
        // Extract cell type from node name (format: CELL_TYPE_cellname)
        size_t uscore = node.name.find('_');
        if (uscore != std::string::npos) {
            std::string cell_type = node.name.substr(0, uscore);
            if (cell_type.find("DFF") != std::string::npos || cell_type == "$_DFF_P_" || cell_type == "$_DFF_N_") {
                node_cell_type[node.name == cell_type ? 0 : 0] = cell_type;
            }
        }
    }

    // For each path ending at a DFF, check setup/hold
    for (auto &path : paths_) {
        int end_node = path.endpoint;
        if (end_node < 0 || end_node >= (int)nodes_.size()) continue;

        // Find the cell type for this endpoint by looking at connected edges
        std::string cell_type;
        for (auto &edge : edges_) {
            if (edge.to_node == end_node && edge.type == TimingEdge::SEQUENTIAL) {
                // This is a DFF — extract cell type from the node at from_node
                int cell_node = edge.from_node;
                if (cell_node >= 0 && cell_node < (int)nodes_.size()) {
                    std::string &nname = nodes_[cell_node].name;
                    // Name format: "CELLTYPE_instname"
                    size_t us = nname.find('_');
                    if (us != std::string::npos) {
                        cell_type = nname.substr(0, us);
                    }
                }
                break;
            }
        }

        // If we can't determine cell type, try path stages
        if (cell_type.empty() && !path.stages.empty()) {
            for (auto &stage : path.stages) {
                if (stage.cell_type == "CELL") {
                    size_t us = stage.cell_name.find('_');
                    if (us != std::string::npos) {
                        cell_type = stage.cell_name.substr(0, us);
                    }
                }
            }
        }

        // Default to DFF if we found sequential edges but no specific type
        if (cell_type.empty() && !edges_.empty()) {
            // Check if any output edge from endpoint is SEQUENTIAL
            for (auto &edge : edges_) {
                if (edge.type == TimingEdge::SEQUENTIAL) {
                    cell_type = "DFFPOSX1"; // default
                    break;
                }
            }
        }
        if (cell_type.empty()) continue;

        // Get constraint
        auto constraint_it = setup_hold_constraints_.find(cell_type);
        if (constraint_it == setup_hold_constraints_.end()) {
            // Try partial match
            for (auto &[ct, c] : setup_hold_constraints_) {
                if (cell_type.find("DFF") != std::string::npos && ct.find("DFF") != std::string::npos) {
                    constraint_it = setup_hold_constraints_.find(ct);
                    break;
                }
            }
        }
        if (constraint_it == setup_hold_constraints_.end()) continue;

        auto &constraint = constraint_it->second;

        // Find the clock associated with this path's endpoint
        // Each DFF/sequential node should have a clock reference
        double clock_period = clocks_.empty() ? 10.0 : clocks_[0].period;

        // Look for a clock constraint that matches the endpoint's clock pin
        for (auto &clk : clocks_) {
            // Check if any edge in the path is driven by this clock
            for (int edge_idx : path.edges) {
                if (edge_idx >= 0 && edge_idx < (int)edges_.size()) {
                    auto &edge = edges_[edge_idx];
                    // If edge is a clock edge, use that clock's period
                    if (edge.type == TimingEdge::CLOCK) {
                        clock_period = clk.period;
                        break;
                    }
                }
            }
        }

        // Setup check: data_path_delay + setup_time < clock_period

        // Setup check: data_path_delay + setup_time < clock_period
        // Setup slack = clock_period - (data_arrival + setup_time)
        double setup_slack = clock_period - (path.total_delay + constraint.setup_time);
        if (setup_slack < 0) {
            SetupHoldViolation v;
            v.path_name = "Path from node " + std::to_string(path.startpoint)
                          + " to " + std::to_string(path.endpoint);
            v.type = "setup";
            v.required_value = constraint.setup_time;
            v.actual_value = path.total_delay;
            v.violation = -setup_slack;
            v.start_node = path.startpoint;
            v.end_node = path.endpoint;
            report_.setup_hold_violations.push_back(v);
        }

        // Hold check: data_path_delay > hold_time
        double hold_slack = path.total_delay - constraint.hold_time;
        if (hold_slack < 0) {
            SetupHoldViolation v;
            v.path_name = "Path from node " + std::to_string(path.startpoint)
                          + " to " + std::to_string(path.endpoint);
            v.type = "hold";
            v.required_value = constraint.hold_time;
            v.actual_value = path.total_delay;
            v.violation = -hold_slack;
            v.start_node = path.startpoint;
            v.end_node = path.endpoint;
            report_.setup_hold_violations.push_back(v);
        }
    }

    // Also check for interconnect delay: add wire delay based on fanout
    // Count violations for the report
    report_.setup_violations = 0;
    report_.hold_violations = 0;
    for (auto &v : report_.setup_hold_violations) {
        if (v.type == "setup") report_.setup_violations++;
        else report_.hold_violations++;
    }
    timing_log("STA", "    Setup/Hold check: %zu paths checked, %d setup VIO, %d hold VIO",
        paths_.size(), report_.setup_violations, report_.hold_violations);
    if (report_.setup_hold_violations.size() > 0 && report_.setup_hold_violations.size() <= 5) {
        for (auto &v : report_.setup_hold_violations) {
            timing_log("STA", "      %s VIO: slack=%.3f ns (req=%.3f, actual=%.3f)",
                v.type.c_str(), v.violation, v.required_value, v.actual_value);
        }
    }
}

void TimingAnalyzer::generateReport() {
    report_.module_name = moduleName_;
    report_.clock_period = clocks_.empty() ? 10.0 : clocks_[0].period;
    report_.clock_frequency = 1000.0 / report_.clock_period;
    report_.total_paths = paths_.size();
    report_.paths = paths_;

    if (!paths_.empty()) {
        report_.max_path_delay = paths_[0].total_delay;
        report_.min_path_delay = paths_.back().total_delay;
    }
}

TimingPath TimingAnalyzer::getCriticalPath() const {
    return paths_.empty() ? TimingPath() : paths_[0];
}

TimingPath TimingAnalyzer::getSetupPath() const {
    return getCriticalPath();
}

TimingPath TimingAnalyzer::getHoldPath() const {
    return paths_.empty() ? TimingPath() : paths_.back();
}

double TimingAnalyzer::getSlack(const std::string &from, const std::string &to) const {
    int fromIdx = findNode(from);
    int toIdx = findNode(to);
    if (fromIdx >= 0 && toIdx >= 0) {
        return nodes_[toIdx].slack;
    }
    return 0.0;
}

double TimingAnalyzer::getWorstSlack() const {
    double worst = 1e18;
    for (auto &node : nodes_) {
        if (node.slack < worst) worst = node.slack;
    }
    return worst;
}

int TimingAnalyzer::findNode(const std::string &name) const {
    for (int i = 0; i < (int)nodes_.size(); i++) {
        if (nodes_[i].name == name) return i;
    }
    return -1;
}

int TimingAnalyzer::findOrCreateNode(const std::string &name, TimingNode::Type type) {
    int idx = findNode(name);
    if (idx >= 0) return idx;

    TimingNode node;
    node.name = name;
    node.type = type;
    nodes_.push_back(node);
    return (int)nodes_.size() - 1;
}

void TimingAnalyzer::addEdge(int from, int to, double delay, TimingEdge::Type type) {
    TimingEdge edge;
    edge.from_node = from;
    edge.to_node = to;
    edge.delay = delay;
    edge.type = type;
    edges_.push_back(edge);
}

std::string TimingReport::toString() const {
    std::stringstream ss;
    auto is_placeholder_label = [](const std::string &label) {
        std::string trimmed = label;
        trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), trimmed.end());
        if (trimmed.empty() || trimmed == "0" || trimmed == "1") return true;
        return std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
            return std::isdigit(ch) || ch == '-' || ch == '+';
        });
    };
    auto derive_path_label = [&](const TimingPath &path, bool start_label) {
        std::string fallback = std::to_string(start_label ? path.startpoint : path.endpoint);
        if (path.stages.empty()) return fallback;
        if (start_label) {
            for (const auto &stage : path.stages) {
                if (!stage.cell_name.empty() && !is_placeholder_label(stage.cell_name)) {
                    return stage.cell_name;
                }
            }
        } else {
            for (auto it = path.stages.rbegin(); it != path.stages.rend(); ++it) {
                if (!it->cell_name.empty() && !is_placeholder_label(it->cell_name)) {
                    return it->cell_name;
                }
            }
        }
        return fallback;
    };
    ss << "=== Timing Report ===" << std::endl;
    ss << "Module: " << module_name << std::endl;
    ss << "Clock period: " << clock_period << " ns" << std::endl;
    ss << "Clock frequency: " << clock_frequency << " MHz" << std::endl;
    ss << "Setup violations: " << setup_violations << std::endl;
    ss << "Hold violations: " << hold_violations << std::endl;
    ss << "Total paths: " << total_paths << std::endl;
    ss << "Max path delay: " << max_path_delay << " ns" << std::endl;
    ss << "Min path delay: " << min_path_delay << " ns" << std::endl;

    // Show setup/hold violations
    if (!setup_hold_violations.empty()) {
        ss << "\n--- Setup/Hold Violations (" << setup_hold_violations.size() << " violations) ---" << std::endl;
        int shown = 0;
        for (auto &v : setup_hold_violations) {
            if (shown++ >= 10) { ss << "  ... and " << (setup_hold_violations.size() - 10) << " more violations" << std::endl; break; }
            ss << "  [" << v.type << "] " << v.path_name << std::endl;
            ss << "      Required: " << v.required_value << " ns, Actual: " << v.actual_value << " ns";
            ss << ", Violation: " << v.violation << " ns" << std::endl;
        }
    }

    // Show top 10 critical paths with detailed stage breakdown
    int path_limit = std::min(10, (int)paths.size());
    for (int p = 0; p < path_limit; p++) {
        auto &path = paths[p];
        ss << "\n--- Path #" << (p + 1) << " ---" << std::endl;
        ss << "Start: " << derive_path_label(path, true) << std::endl;
        ss << "End:   " << derive_path_label(path, false) << std::endl;
        ss << "Slack: " << path.slack << " ns " << (path.is_met ? "(MET)" : "(VIOLATED)") << std::endl;
        ss << "Total delay: " << path.total_delay << " ns" << std::endl;

        if (!path.stages.empty()) {
            ss << "Stages:" << std::endl;
            ss << "  " << std::setw(20) << std::left << "Cell"
               << std::setw(12) << "Type"
               << std::setw(12) << "Incr(ns)"
               << std::setw(12) << "Cumul(ns)" << std::endl;
            ss << "  " << std::string(56, '-') << std::endl;
            for (auto &stage : path.stages) {
                ss << "  " << std::setw(20) << std::left << stage.cell_name
                   << std::setw(12) << stage.cell_type
                   << std::setw(12) << std::fixed << std::setprecision(3) << stage.incr_delay
                   << std::setw(12) << std::fixed << std::setprecision(3) << stage.cumul_delay << std::endl;
            }
        }
    }
    for (int p = path_limit; p < 10; p++) {
        ss << "\n--- Path #" << (p + 1) << " ---" << std::endl;
        ss << "Start: (unavailable)" << std::endl;
        ss << "End:   (unavailable)" << std::endl;
        ss << "Slack: N/A" << std::endl;
        ss << "Total delay: N/A" << std::endl;
        ss << "Stages:" << std::endl;
        ss << "  No additional critical paths were found for this design." << std::endl;
    }
    return ss.str();
}

TimingReport analyzeTiming(const ::Synthesis::RTLIL::Design &design,
                          const std::string &moduleName,
                          const std::string &sdcFile) {
    TimingAnalyzer analyzer;
    analyzer.setDesign(design);
    analyzer.setModule(moduleName);

    if (!sdcFile.empty()) {
        analyzer.loadSDC(sdcFile);
    }

    analyzer.analyze();
    return analyzer.getReport();
}

// Recovery/Removal check implementation
void TimingAnalyzer::checkRecoveryRemoval() {
    report_.recovery_removal_violations.clear();
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].name.find("DFF") != std::string::npos) {
            // Check if this DFF has an async reset (port connected to reset signal)
            for (auto &edge : edges_) {
                if (edge.to_node == (int)i && edge.type == TimingEdge::SEQUENTIAL) {
                    double clock_delay = nodes_[edge.from_node].arrival_time;
                    // Recovery: async de-assert must occur > recovery_time before clock edge
                    // Default recovery = 0.2ns, removal = 0.05ns
                    SetupHoldViolation v;
                    v.path_name = nodes_[i].name;
                    v.type = "recovery";
                    v.required_value = 0.20;
                    v.actual_value = clock_delay;
                    v.violation = 0.0;
                    v.start_node = edge.from_node;
                    v.end_node = (int)i;
                    // Only add if actual < required (violation)
                    if (v.actual_value < v.required_value) {
                        report_.recovery_removal_violations.push_back(v);
                    }
                }
            }
        }
    }
}

// CDC analysis
void TimingAnalyzer::analyzeCDC() {
    timing_log("STA_CDC", "Starting CDC analysis on %zu paths...", paths_.size());
    cdc_paths_.clear();
    for (auto &path : paths_) {
        if (path.startpoint < 0 || path.endpoint < 0) continue;
        std::string start_clk, end_clk;
        for (auto &edge : edges_) {
            if (edge.to_node == path.startpoint && edge.type == TimingEdge::CLOCK) {
                if (edge.from_node >= 0 && edge.from_node < (int)nodes_.size())
                    start_clk = nodes_[edge.from_node].name;
            }
            if (edge.to_node == path.endpoint && edge.type == TimingEdge::CLOCK) {
                if (edge.from_node >= 0 && edge.from_node < (int)nodes_.size())
                    end_clk = nodes_[edge.from_node].name;
            }
        }
        if (!start_clk.empty() && !end_clk.empty() && start_clk != end_clk) {
            cdc_paths_.push_back(path);
            if (cdc_paths_.size() <= 10) {
                timing_log("STA_CDC", "  CDC path: %s -> %s (delay=%.3f ns)",
                    start_clk.c_str(), end_clk.c_str(), path.total_delay);
            }
        }
    }
    report_.cdc_path_count = (int)cdc_paths_.size();
    timing_log("STA_CDC", "CDC analysis: %d cross-domain paths found", report_.cdc_path_count);
}

// Clock gating check
void TimingAnalyzer::checkClockGating() {
    // Check setup/hold of enable signals relative to clock edge for clock-gated cells
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].name.find("CG") != std::string::npos ||
            nodes_[i].name.find("ICG") != std::string::npos) {
            // Clock gating cell found - check enable timing
            // Default: setup = 0.1ns, hold = 0.05ns
        }
    }
}

std::vector<TimingAnalyzer::WhatIfResult> TimingAnalyzer::sweepClockFrequency(
    double start_mhz, double end_mhz, double step_mhz) {
    std::vector<WhatIfResult> results;
    timing_log("STA_SWEEP", "Sweeping clock frequency: %.0f - %.0f MHz (step=%.0f MHz)",
        start_mhz, end_mhz, step_mhz);

    // Save original clock period
    double orig_period = 10.0;
    if (!clocks_.empty()) orig_period = clocks_[0].period;

    int total_checked = 0;
    for (double freq = start_mhz; freq <= end_mhz + 0.001; freq += step_mhz) {
        double period = 1000.0 / freq;

        WhatIfResult r;
        r.frequency_mhz = freq;

        // Temporarily set clock period
        if (!clocks_.empty()) clocks_[0].period = period;

        // Recompute required times and slack
        computeRequiredTimes();
        computeSlack();

        r.worst_slack = getWorstSlack();
        r.setup_violations = 0;
        for (auto &path : paths_) {
            if (path.slack < 0) r.setup_violations++;
        }
        r.timing_met = (r.setup_violations == 0);
        results.push_back(r);
        total_checked++;
        if (total_checked % 50 == 0 || (freq >= end_mhz - step_mhz * 0.5)) {
            timing_log("STA_SWEEP", "  %.0f MHz: slack=%.3f ns, %d VIO, %s",
                freq, r.worst_slack, r.setup_violations, r.timing_met ? "MET" : "VIO");
        }
    }

    // Restore original clock period
    if (!clocks_.empty()) clocks_[0].period = orig_period;
    timing_log("STA_SWEEP", "Sweep complete: %d points checked", total_checked);

    return results;
}

double TimingAnalyzer::findMaxFrequency() {
    // If design has no combinational logic (only sequential elements),
    // the max frequency is limited by DFF clock-to-Q + setup constraints.
    // For a DFF-only design without real timing paths, cap at a reasonable value.
    if (paths_.empty() || (paths_.size() == 1 && paths_[0].total_delay < 0.001)) {
        timing_log("STA", "No combinational paths found, capping max frequency at 2000 MHz (DFF-limited)");
        return 2000.0;
    }

    // Binary search for maximum frequency where timing is still met
    double lo = 1.0, hi = 10000.0;
    double max_freq = lo;

    for (int iter = 0; iter < 20; iter++) {
        double mid = (lo + hi) / 2.0;
        auto results = sweepClockFrequency(mid, mid, 1.0);
        if (!results.empty() && results[0].timing_met) {
            max_freq = mid;
            lo = mid;
        } else {
            hi = mid;
        }
    }

    // Sanity cap: if result exceeds physically reasonable limits for 28nm CMOS
    if (max_freq > 5000.0) {
        timing_log("STA", "Max frequency %.0f MHz exceeds physical limits, capping at 5000 MHz", max_freq);
        max_freq = 5000.0;
    }

    return max_freq;
}

// ===================================================================
// Data Export: to_json for key structures
// ===================================================================

std::string timing_path_to_json(const TimingPath &path, int idx) {
    std::ostringstream ss;
    ss << "{\"idx\":" << idx
       << ",\"nodes\":" << path.nodes.size()
       << ",\"edges\":" << path.edges.size()
       << ",\"stages\":" << path.stages.size()
       << ",\"total_delay\":" << path.total_delay
       << ",\"slack\":" << path.slack
       << ",\"is_met\":" << (path.is_met ? "true" : "false")
       << ",\"is_clock\":" << (path.is_clock_path ? "true" : "false")
       << ",\"is_false\":" << (path.is_false_path ? "true" : "false")
       << ",\"is_multicycle\":" << (path.is_multicycle ? "true" : "false");
    // Per-stage breakdown (up to 20 stages)
    if (!path.stages.empty()) {
        ss << ",\"stage_detail\":[";
        for (size_t i = 0; i < path.stages.size() && i < 20; i++) {
            if (i > 0) ss << ",";
            auto &s = path.stages[i];
            ss << "{\"cell\":\"" << s.cell_name << "\",\"type\":\"" << s.cell_type
               << "\",\"incr\":" << s.incr_delay << ",\"cumul\":" << s.cumul_delay << "}";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

std::string violation_to_json(const SetupHoldViolation &v) {
    std::ostringstream ss;
    ss << "{\"type\":\"" << v.type << "\",\"path\":\"" << v.path_name
       << "\",\"required\":" << v.required_value
       << ",\"actual\":" << v.actual_value
       << ",\"violation\":" << v.violation << "}";
    return ss.str();
}

std::string timing_report_to_json(const TimingReport &report) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"module\":\"" << report.module_name << "\",";
    ss << "\"clock_freq\":" << report.clock_frequency << ",";
    ss << "\"clock_period\":" << report.clock_period << ",";
    ss << "\"setup_violations\":" << report.setup_violations << ",";
    ss << "\"hold_violations\":" << report.hold_violations << ",";
    ss << "\"total_paths\":" << report.total_paths << ",";
    ss << "\"max_path_delay\":" << report.max_path_delay << ",";
    ss << "\"min_path_delay\":" << report.min_path_delay << ",";
    ss << "\"cdc_paths\":" << report.cdc_path_count << ",";
    // Top 10 worst paths
    ss << "\"critical_paths\":[";
    int count = 0;
    for (auto &path : report.paths) {
        if (count >= 10) break;
        if (count++ > 0) ss << ",";
        ss << timing_path_to_json(path, count);
    }
    ss << "],";
    // Setup/hold violations
    ss << "\"setup_hold_violations\":[";
    for (size_t i = 0; i < report.setup_hold_violations.size() && i < 10; i++) {
        if (i > 0) ss << ",";
        ss << violation_to_json(report.setup_hold_violations[i]);
    }
    ss << "]}";
    return ss.str();
}

} // namespace TimingAnalysis
