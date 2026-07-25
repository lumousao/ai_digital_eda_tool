/**
 * Timing Graph - Static Timing Analysis implementation
 *
 * References OpenSTA design patterns:
 * - Topological sort for arrival time (like OpenSTA's Graph::visit())
 * - Back-traversal for required time (like OpenSTA's Search::requiredTime())
 * - Setup/Hold check (like OpenSTA's CheckTiming)
 */

#include "timing_graph.h"
#include <sstream>
#include <fstream>
#include <queue>
#include <cstring>
#include <cmath>

// ==================== Liberty Parser ====================

static std::string trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> split_tokens(const std::string &s) {
    std::vector<std::string> tokens;
    std::string token;
    for (char c : s) {
        if (std::isspace(c) || c == '(' || c == ')' || c == ';' || c == ',' || c == '{' || c == '}') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

std::map<std::string, LibertyCellTiming> LibertyParser::parse(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_string(buffer.str());
}

std::map<std::string, LibertyCellTiming> LibertyParser::parse_string(const std::string &content) {
    std::map<std::string, LibertyCellTiming> cells;
    std::istringstream stream(content);
    std::string line;

    std::string current_cell;
    std::string current_pin;
    LibertyCellTiming *cell = nullptr;
    bool in_cell = false;
    bool in_pin = false;
    bool in_timing = false;
    bool in_setup = false;
    bool in_hold = false;
    bool in_ff = false;

    while (std::getline(stream, line)) {
        // Remove comments
        size_t comment = line.find("//");
        if (comment != std::string::npos) line = line.substr(0, comment);
        comment = line.find("/*");
        if (comment != std::string::npos) {
            size_t end_comment = line.find("*/", comment);
            if (end_comment != std::string::npos) {
                line = line.substr(0, comment) + line.substr(end_comment + 2);
            } else {
                line = line.substr(0, comment);
            }
        }

        std::vector<std::string> tokens = split_tokens(trim(line));
        if (tokens.empty()) continue;

        // Library-level
        if (tokens[0] == "library") continue;

        // Cell
        if (tokens[0] == "cell" && tokens.size() >= 2) {
            current_cell = tokens[1];
            // Remove quotes
            if (!current_cell.empty() && current_cell.front() == '"')
                current_cell = current_cell.substr(1, current_cell.size() - 2);
            cells[current_cell] = LibertyCellTiming();
            cell = &cells[current_cell];
            cell->name = current_cell;
            in_cell = true;
            in_pin = false;
            in_timing = false;
            in_ff = false;
            continue;
        }

        if (!in_cell) continue;

        // FF declaration
        if (tokens[0] == "ff") {
            in_ff = true;
            // Parse ff(IQ, IQN) { ... }
            continue;
        }

        if (in_ff && tokens[0] == "clocked_on") {
            // clocked_on: C;
            if (cell) cell->clk_to_q = 0.15; // default clk-to-q
            in_ff = false;
            continue;
        }

        // Area
        if (tokens[0] == "area" && tokens.size() >= 3 && cell) {
            cell->area = std::stod(tokens[2]);
            continue;
        }

        // Pin
        if (tokens[0] == "pin" && tokens.size() >= 2 && cell) {
            current_pin = tokens[1];
            if (!current_pin.empty() && current_pin.front() == '"')
                current_pin = current_pin.substr(1, current_pin.size() - 2);
            cell->pins[current_pin] = LibertyCellTiming::PinInfo();
            in_pin = true;
            in_timing = false;
            continue;
        }

        if (!in_pin) continue;

        // Pin attributes
        if (tokens[0] == "direction" && tokens.size() >= 3 && cell) {
            std::string dir = tokens[2];
            if (dir == "input") cell->pins[current_pin].is_input = true;
            else if (dir == "output") cell->pins[current_pin].is_output = true;
            continue;
        }
        if (tokens[0] == "clock" && tokens.size() >= 3 && cell) {
            if (tokens[2] == "true") cell->pins[current_pin].is_clock = true;
            continue;
        }
        if (tokens[0] == "capacitance" && tokens.size() >= 3 && cell) {
            cell->pins[current_pin].capacitance = std::stod(tokens[2]);
            continue;
        }

        // Timing
        if (tokens[0] == "timing") {
            in_timing = true;
            in_setup = false;
            in_hold = false;
            continue;
        }

        if (in_timing) {
            if (tokens[0] == "setup") { in_setup = true; continue; }
            if (tokens[0] == "hold") { in_hold = true; continue; }

            // Parse timing template values
            if (tokens[0] == "values" && cell) {
                // Extract first value as delay approximation
                for (const auto &t : tokens) {
                    if (t == "values") continue;
                    if (t == "(" || t == ")" || t == ";") continue;
                    try {
                        double val = std::stod(t);
                        if (in_setup && cell) {
                            cell->setup_time = std::max(cell->setup_time, val);
                        } else if (in_hold && cell) {
                            cell->hold_time = std::max(cell->hold_time, val);
                        } else if (cell) {
                            // Store as cell delay
                            cell->cell_delay[current_pin]["Y"] =
                                std::max(cell->cell_delay[current_pin]["Y"], val);
                        }
                        break; // Use first value as representative
                    } catch (...) {}
                }
                continue;
            }

            if (tokens[0] == "}" || tokens[0] == "end") {
                in_timing = false;
                in_setup = false;
                in_hold = false;
                continue;
            }
        }

        // End of pin/cell
        if (tokens[0] == "}" || tokens[0] == "end") {
            if (in_pin) in_pin = false;
            else if (in_cell) { in_cell = false; cell = nullptr; }
        }
    }

    return cells;
}

// ==================== Timing Analyzer ====================

TimingAnalyzer::TimingAnalyzer() : total_cells_(0), total_dff_(0), critical_path_delay_(0),
                                    wire_load_(WireLoadModel()) {}
TimingAnalyzer::~TimingAnalyzer() {}

void TimingAnalyzer::load_library(const std::string &filename) {
    library_ = LibertyParser::parse(filename);
}

void TimingAnalyzer::set_clock(const ClockDef &clk) {
    clock_ = clk;
}

void TimingAnalyzer::build_graph(const std::string &synth_output,
                                  const std::string &module_name) {
    nodes_.clear();
    edges_.clear();
    node_map_.clear();

    // Parse synthesis stat output to extract cell instances
    // Format: "count area cell_type" or "count cell_type"
    std::istringstream stream(synth_output);
    std::string line;
    bool in_target_module = false;
    int node_id = 0;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);

        // Detect module
        if (trimmed.find("===") != std::string::npos &&
            trimmed.find(module_name) != std::string::npos) {
            in_target_module = true;
            continue;
        }

        if (!in_target_module) continue;

        // Stop at next section
        if (trimmed.find("===") != std::string::npos ||
            trimmed.find("--") != std::string::npos ||
            trimmed.find("End of") != std::string::npos) {
            break;
        }

        // Parse cell instances
        std::vector<std::string> tokens = split_tokens(trimmed);
        if (tokens.size() < 2) continue;

        // Try to parse: "count cell_type" or "count area cell_type"
        if (tokens[0].find_first_of("0123456789") == std::string::npos) continue;

        int count = 0;
        std::string cell_type;

        if (tokens.size() >= 3) {
            // "count area cell_type"
            try {
                count = std::stoi(tokens[0]);
                cell_type = tokens[2];
            } catch (...) { continue; }
        } else {
            // "count cell_type"
            try {
                count = std::stoi(tokens[0]);
                cell_type = tokens[1];
            } catch (...) { continue; }
        }

        // Skip metadata
        if (cell_type == "cells" || cell_type == "wires" || cell_type == "ports" ||
            cell_type == "wire" || cell_type == "wire bits" ||
            cell_type.find("public") != std::string::npos) {
            continue;
        }

        // Add cell instances to graph
        for (int i = 0; i < count; i++) {
            std::string inst_name = cell_type + "_" + std::to_string(node_id);
            add_cell_instance(inst_name, cell_type, {});
            node_id++;
        }

        // Track DFF count
        if (cell_type.find("DFF") != std::string::npos) {
            total_dff_ += count;
        }
    }

    total_cells_ = node_id;
}

void TimingAnalyzer::add_cell_instance(const std::string &name, const std::string &type,
                                        const std::map<std::string, std::string> &connections) {
    TimingNode node;
    node.id = (int)nodes_.size();
    node.name = name;
    node.cell_type = type;
    node.is_register = (type.find("DFF") != std::string::npos);

    // Determine pins from library
    auto it = library_.find(type);
    if (it != library_.end()) {
        for (auto &[pin_name, pin_info] : it->second.pins) {
            if (pin_info.is_input && !pin_info.is_clock) {
                node.fanin.push_back(node.id); // self-reference for now
            }
            if (pin_info.is_output) {
                node.output_pin = pin_name;
            }
        }
    }

    node_map_[name] = node.id;
    nodes_.push_back(node);
}

void TimingAnalyzer::add_port(const std::string &name, bool is_input, bool is_output) {
    TimingNode node;
    node.id = (int)nodes_.size();
    node.name = name;
    node.is_input_port = is_input;
    node.is_output_port = is_output;
    node_map_[name] = node.id;
    nodes_.push_back(node);
}

TimingResult TimingAnalyzer::analyze() {
    TimingResult result;

    // Set clock period
    result.clock_period = clock_.period;
    result.clock_frequency_mhz = 1000.0 / clock_.period;
    result.total_cells = total_cells_;
    result.total_dff = total_dff_;

    if (nodes_.empty()) {
        return result;
    }

    // Step 1: Assign arrival times
    // Input ports: arrival = 0 (with optional input delay)
    // Registers: arrival = clk_to_q
    // Combinational: arrival = max(fanin arrival + cell delay)
    calc_arrival_times();

    // Step 2: Assign required times
    // Output ports: required = clock_period (with setup margin)
    // Registers: required = clock_period - setup_time
    // Back-traverse from outputs
    calc_required_times();

    // Step 3: Calculate slack
    calc_slack();

    // Step 4: Find critical path
    find_critical_path();

    // Collect results
    result.wns = 0;
    result.tns = 0;
    result.wns_hold = 0;
    result.tns_hold = 0;
    result.setup_violations = 0;
    result.hold_violations = 0;
    result.nodes = nodes_;
    result.critical_path_delay = critical_path_delay_;

    for (auto &node : nodes_) {
        if (node.slack < 0) {
            result.setup_violations++;
            result.tns += node.slack;
            if (node.slack < result.wns) result.wns = node.slack;
        }
        // Hold slack: arrival - hold_time
        if (node.is_register) {
            double hold_slack = node.arrival_time - get_hold_time(node.cell_type);
            if (hold_slack < 0) {
                result.hold_violations++;
                result.tns_hold += hold_slack;
                if (hold_slack < result.wns_hold) result.wns_hold = hold_slack;
            }
        }
    }

    result.critical_path = critical_path_;
    return result;
}

void TimingAnalyzer::calc_arrival_times() {
    // Topological sort (BFS from inputs/clock sources)
    std::vector<int> in_degree(nodes_.size(), 0);
    for (auto &edge : edges_) {
        if (edge.to_node < (int)in_degree.size()) {
            in_degree[edge.to_node]++;
        }
    }

    // Initialize input ports and clock sources with arrival = 0
    for (auto &node : nodes_) {
        if (node.is_input_port || node.is_clock_source) {
            node.arrival_time = 0;
        } else if (node.is_register) {
            // Registers: arrival = clk_to_q
            node.arrival_time = get_clk_to_q(node.cell_type);
        } else {
            node.arrival_time = 0;
        }
    }

    // BFS topological sort
    std::queue<int> queue;
    for (auto &node : nodes_) {
        if (node.is_input_port || node.is_clock_source || node.is_register) {
            queue.push(node.id);
        }
    }

    std::set<int> visited;
    while (!queue.empty()) {
        int nid = queue.front();
        queue.pop();
        if (visited.count(nid)) continue;
        visited.insert(nid);

        // Update fanout arrival times (include wire delay)
        for (auto &edge : edges_) {
            if (edge.from_node == nid) {
                int fid = edge.to_node;
                if (fid >= 0 && fid < (int)nodes_.size()) {
                    // Wire delay based on fanout (wire load model)
                    int fanout_count = 0;
                    for (auto &e2 : edges_) {
                        if (e2.from_node == nid) fanout_count++;
                    }
                    double wire_delay = wire_load_.estimate_delay(fanout_count);
                    double new_arrival = nodes_[nid].arrival_time + edge.delay + wire_delay;
                    if (new_arrival > nodes_[fid].arrival_time) {
                        nodes_[fid].arrival_time = new_arrival;
                    }
                    in_degree[fid]--;
                    if (in_degree[fid] <= 0) {
                        queue.push(fid);
                    }
                }
            }
        }
    }

    // For nodes without fanin (not connected), estimate arrival from cell delay
    for (auto &node : nodes_) {
        if (node.arrival_time == 0 && !node.is_input_port && !node.is_clock_source) {
            node.arrival_time = get_clk_to_q(node.cell_type);
        }
    }
}

void TimingAnalyzer::calc_required_times() {
    // Back-traverse from outputs and register inputs
    // Output ports: required = clock_period - uncertainty
    // Register D pins: required = clock_period - setup_time

    double default_required = clock_.period - clock_.uncertainty;

    for (auto &node : nodes_) {
        if (node.is_output_port) {
            node.required_time = default_required;
        } else if (node.is_register) {
            node.required_time = clock_.period - get_setup_time(node.cell_type);
        } else {
            node.required_time = default_required;
        }
    }

    // Back-propagate required times through combinational logic
    // (simplified: assign based on fanout requirements)
    for (int i = (int)nodes_.size() - 1; i >= 0; i--) {
        auto &node = nodes_[i];
        for (auto &edge : edges_) {
            if (edge.to_node == node.id) {
                int fid = edge.from_node;
                if (fid >= 0 && fid < (int)nodes_.size()) {
                    double new_required = node.required_time - edge.delay;
                    if (new_required < nodes_[fid].required_time || nodes_[fid].required_time == 0) {
                        nodes_[fid].required_time = new_required;
                    }
                }
            }
        }
    }
}

void TimingAnalyzer::calc_slack() {
    for (auto &node : nodes_) {
        node.slack = node.required_time - node.arrival_time;
    }
}

void TimingAnalyzer::find_critical_path() {
    // Find the node with worst (most negative) slack
    int worst_node = -1;
    double worst_slack = 0;

    for (auto &node : nodes_) {
        if (!node.is_clock_source && node.slack < worst_slack) {
            worst_slack = node.slack;
            worst_node = node.id;
        }
    }

    // If no negative slack, find the node with smallest positive slack
    if (worst_node < 0) {
        double min_slack = 1e18;
        for (auto &node : nodes_) {
            if (!node.is_clock_source && node.slack < min_slack) {
                min_slack = node.slack;
                worst_node = node.id;
            }
        }
    }

    // Trace back from worst node to build critical path
    critical_path_.clear();
    critical_path_delay_ = 0;

    if (worst_node >= 0 && worst_node < (int)nodes_.size()) {
        std::set<int> visited;
        int current = worst_node;
        while (current >= 0 && current < (int)nodes_.size() && !visited.count(current)) {
            visited.insert(current);
            critical_path_.push_back(current);
            critical_path_delay_ += get_cell_delay(nodes_[current].cell_type, "", "Y");

            // Find worst fanin
            int worst_fanin = -1;
            double worst_arrival = -1;
            for (auto &edge : edges_) {
                if (edge.to_node == current) {
                    int fid = edge.from_node;
                    if (fid >= 0 && fid < (int)nodes_.size() &&
                        nodes_[fid].arrival_time > worst_arrival) {
                        worst_arrival = nodes_[fid].arrival_time;
                        worst_fanin = fid;
                    }
                }
            }
            current = worst_fanin;
        }
    }
}

double TimingAnalyzer::get_cell_delay(const std::string &cell_type,
                                       const std::string &from_pin,
                                       const std::string &to_pin) {
    auto it = library_.find(cell_type);
    if (it != library_.end()) {
        auto &delays = it->second.cell_delay;
        auto pin_it = delays.find(from_pin);
        if (pin_it != delays.end()) {
            auto out_it = pin_it->second.find(to_pin);
            if (out_it != pin_it->second.end()) {
                return out_it->second;
            }
        }
        // Use first available delay
        if (!delays.empty()) {
            return delays.begin()->second.begin()->second;
        }
    }

    // Default delays based on cell type
    if (cell_type.find("DFF") != std::string::npos) return 0.15;
    if (cell_type.find("NAND") != std::string::npos) return 0.03;
    if (cell_type.find("NOR") != std::string::npos) return 0.03;
    if (cell_type.find("NOT") != std::string::npos) return 0.02;
    if (cell_type.find("AND") != std::string::npos) return 0.05;
    if (cell_type.find("OR") != std::string::npos) return 0.05;
    if (cell_type.find("XOR") != std::string::npos) return 0.08;
    if (cell_type.find("MUX") != std::string::npos) return 0.06;
    return 0.04;
}

double TimingAnalyzer::get_setup_time(const std::string &cell_type) {
    auto it = library_.find(cell_type);
    if (it != library_.end()) return it->second.setup_time;
    return 0.1; // default
}

double TimingAnalyzer::get_hold_time(const std::string &cell_type) {
    auto it = library_.find(cell_type);
    if (it != library_.end()) return it->second.hold_time;
    return 0.05; // default
}

double TimingAnalyzer::get_clk_to_q(const std::string &cell_type) {
    auto it = library_.find(cell_type);
    if (it != library_.end()) return it->second.clk_to_q;
    return 0.15; // default
}

std::vector<std::string> TimingAnalyzer::get_critical_path() {
    std::vector<std::string> path;
    for (int nid : critical_path_) {
        if (nid >= 0 && nid < (int)nodes_.size()) {
            path.push_back(nodes_[nid].name);
        }
    }
    return path;
}
