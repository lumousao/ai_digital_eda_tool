/**
 * Enhanced Lint Check - Comprehensive RTL linting
 *
 * Features:
 * - Combinational loop detection (DFS-based)
 * - Unused signal detection (input/output/wire/reg)
 * - Inferred latch detection
 * - Incomplete sensitivity list detection
 * - Width mismatch detection
 * - Multiple driver detection
 * - Unconnected port detection
 * - Synthesizability check (unsupported constructs)
 * - Clock domain crossing detection (basic CDC)
 * - DFF without reset detection
 * - Case statement completeness check
 * - Blocking vs non-blocking assignment check
 * - Fanout check
 * - Signal naming convention check
 * - Combinational feedback check
 */

#include "lint_check.h"
#include <sstream>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <regex>

namespace LintCheck {

// Helper: check if a string starts with a prefix
static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Helper: remove escape character from RTLIL names
static std::string clean_name(const std::string &name) {
    if (name.empty()) return name;
    if (name[0] == '\\') return name.substr(1);
    return name;
}

// ==========================================================================
// RTL-level lint checks (operate on Verilog source, not RTLIL)
// ==========================================================================

struct RtlLintState {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> infos;
    std::string module_name;

    // Track all signals declared in the module
    std::set<std::string> declared_signals;
    std::set<std::string> used_signals;
    std::set<std::string> driven_signals;
    std::set<std::string> input_ports;
    std::set<std::string> output_ports;
    std::set<std::string> inout_ports;
    std::set<std::string> reg_signals;
    std::set<std::string> wire_signals;

    // Track clocks used in sensitivity lists
    std::set<std::string> clocks;
    std::set<std::string> async_resets;

    // Track case/if completeness
    bool has_default_case = false;
    bool has_else_clause = false;
    int case_statement_count = 0;
    int if_without_else_count = 0;

    // Track blocking/non-blocking usage
    int blocking_assign_in_sequential = 0;
    int nonblocking_assign_in_combinational = 0;
    int always_ff_count = 0;
    int always_comb_count = 0;
    int always_latch_count = 0;
    int always_level_count = 0;

    // Track sensitivity list completeness
    std::set<std::string> sensitivity_signals;

    // Track DFFs
    int dff_without_reset = 0;
    int dff_with_reset = 0;

    // Signal naming conventions
    int naming_violations = 0;

    // Latch inference detection
    bool found_latch = false;

    // Synthesizability
    bool has_delay_statements = false;
    bool has_unsupported_construct = false;

    void add_error(const std::string &msg) {
        errors.push_back(msg);
    }

    void add_warning(const std::string &msg) {
        warnings.push_back(msg);
    }

    void add_info(const std::string &msg) {
        infos.push_back(msg);
    }
};

/// Parse Verilog source for lint checking (line-by-line analysis)
static void analyze_verilog_source(const std::string &src, RtlLintState &state) {
    std::istringstream stream(src);
    std::string line;
    int line_num = 0;
    bool in_always = false;
    bool in_always_ff = false;
    bool in_always_comb = false;
    bool in_comment_block = false;
    std::string current_always_sensitivity;
    std::set<std::string> current_sensitivity_signals;

    while (std::getline(stream, line)) {
        line_num++;
        std::string trimmed = line;
        // Remove leading/trailing whitespace
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        // Skip comments
        if (trimmed.empty() || trimmed.substr(0, 2) == "//") continue;
        if (in_comment_block) {
            size_t end = trimmed.find("*/");
            if (end != std::string::npos) {
                in_comment_block = false;
                trimmed = trimmed.substr(end + 2);
            } else {
                continue;
            }
        }
        size_t comment_start = trimmed.find("//");
        if (comment_start != std::string::npos) {
            trimmed = trimmed.substr(0, comment_start);
        }
        size_t block_comment_start = trimmed.find("/*");
        if (block_comment_start != std::string::npos) {
            size_t block_comment_end = trimmed.find("*/", block_comment_start + 2);
            if (block_comment_end != std::string::npos) {
                trimmed = trimmed.substr(0, block_comment_start) + trimmed.substr(block_comment_end + 2);
            } else {
                trimmed = trimmed.substr(0, block_comment_start);
                in_comment_block = true;
            }
        }
        if (trimmed.empty()) continue;

        // Detect module declaration
        if (starts_with(trimmed, "module ")) {
            size_t paren = trimmed.find('(');
            if (paren != std::string::npos) {
                std::string rest = trimmed.substr(paren);
                // Extract port list
                std::string ports_str;
                if (rest.find(')') != std::string::npos) {
                    ports_str = rest.substr(1, rest.find(')') - 1);
                }
                // Parse ports
                std::stringstream port_ss(ports_str);
                std::string port;
                while (std::getline(port_ss, port, ',')) {
                    port.erase(0, port.find_first_not_of(" \t\r\n"));
                    port.erase(port.find_last_not_of(" \t\r\n") + 1);
                    if (!port.empty()) {
                        state.declared_signals.insert(port);
                    }
                }
            }
        }

        // Detect input/output/inout declarations
        std::regex decl_regex("(input|output|inout)\\s+(reg|wire|logic)?\\s*((:\\s*\\w+\\s+)?\\[?[^;]*\\]?)?\\s*(\\w+)");
        std::smatch match;
        if (std::regex_search(trimmed, match, decl_regex)) {
            std::string dir = match[1];
            std::string sig_name = match[5];
            // Handle multi-signal declarations: "input wire [3:0] a, b, c;"
            // Simple: just capture the first signal name
            state.declared_signals.insert(sig_name);
            if (dir == "input") state.input_ports.insert(sig_name);
            else if (dir == "output") {
                state.output_ports.insert(sig_name);
                if (trimmed.find("reg") != std::string::npos || trimmed.find("logic") != std::string::npos) {
                    state.reg_signals.insert(sig_name);
                }
            }
            else if (dir == "inout") state.inout_ports.insert(sig_name);
        }

        // Detect wire declarations
        std::regex wire_regex("wire\\s+((:\\s*\\w+\\s+)?\\[?[^;]*\\]?)?\\s*(\\w+)");
        if (std::regex_search(trimmed, match, wire_regex)) {
            std::string sig_name = match[3];
            state.wire_signals.insert(sig_name);
            state.declared_signals.insert(sig_name);
        }

        // Detect reg declarations
        std::regex reg_regex("reg\\s+((:\\s*\\w+\\s+)?\\[?[^;]*\\]?)?\\s*(\\w+)");
        if (std::regex_search(trimmed, match, reg_regex)) {
            std::string sig_name = match[3];
            state.reg_signals.insert(sig_name);
            state.declared_signals.insert(sig_name);
        }

        // Detect always blocks
        if (trimmed.find("always") != std::string::npos) {
            in_always = true;
            in_always_comb = false;
            in_always_ff = false;
            current_sensitivity_signals.clear();

            if (trimmed.find("always_ff") != std::string::npos) {
                in_always_ff = true;
                state.always_ff_count++;
            } else if (trimmed.find("always_comb") != std::string::npos) {
                in_always_comb = true;
                state.always_comb_count++;
            } else if (trimmed.find("always_latch") != std::string::npos) {
                state.always_latch_count++;
            } else {
                state.always_level_count++;
            }

            // Parse sensitivity list: @(posedge clk or negedge rst_n)
            size_t at_pos = trimmed.find('@');
            if (at_pos != std::string::npos) {
                size_t paren_start = trimmed.find('(', at_pos);
                size_t paren_end = trimmed.find(')', at_pos);
                if (paren_start != std::string::npos && paren_end != std::string::npos) {
                    current_always_sensitivity = trimmed.substr(paren_start + 1, paren_end - paren_start - 1);
                    state.sensitivity_signals.insert(current_always_sensitivity);

                    // Parse sensitivity list signals
                    std::string sens = current_always_sensitivity;
                    std::stringstream sens_ss(sens);
                    std::string item;
                    while (std::getline(sens_ss, item, ' ')) {
                        item.erase(0, item.find_first_not_of(" \t\r\n"));
                        item.erase(item.find_last_not_of(" \t\r\n") + 1);
                        if (item == "or" || item == "," || item.empty()) continue;
                        if (item == "posedge" || item == "negedge" || item == "edge") continue;
                        if (!item.empty()) {
                            current_sensitivity_signals.insert(item);
                            if (item.find("rst") != std::string::npos || item.find("reset") != std::string::npos) {
                                state.async_resets.insert(item);
                            } else {
                                state.clocks.insert(item);
                            }
                        }
                    }
                }
            }
        }

        // Detect edge sensitivity
        if (trimmed.find("@(posedge") != std::string::npos || trimmed.find("@(negedge") != std::string::npos) {
            // Already handled above
        }

        // Detect assign statements
        if (trimmed.find("assign ") != std::string::npos) {
            // Extract LHS signal
            size_t eq_pos = trimmed.find('=');
            if (eq_pos != std::string::npos) {
                std::string lhs = trimmed.substr(trimmed.find("assign") + 7, eq_pos - trimmed.find("assign") - 7);
                lhs.erase(0, lhs.find_first_not_of(" \t"));
                lhs.erase(lhs.find_last_not_of(" \t") + 1);
                // Handle part-select: "sig[3:0]" -> "sig"
                size_t bracket = lhs.find('[');
                if (bracket != std::string::npos) lhs = lhs.substr(0, bracket);
                state.driven_signals.insert(lhs);
            }
        }

        // Detect blocking assignments in always blocks (=)
        if (in_always && trimmed.find('=') != std::string::npos && trimmed.find("<=") == std::string::npos
            && trimmed.find("==") == std::string::npos && trimmed.find("!=") == std::string::npos
            && trimmed.find(">=") == std::string::npos && trimmed.find("<=") == std::string::npos) {
            if (in_always_ff && trimmed.find("always") == std::string::npos) {
                state.blocking_assign_in_sequential++;
            }
        }

        // Detect non-blocking assignments (<=)
        if (trimmed.find("<=") != std::string::npos && trimmed.find("<=") == trimmed.find_last_of('=')
            && trimmed.find("==") == std::string::npos) {
            if (in_always_comb || in_always) {
                state.nonblocking_assign_in_combinational++;
            }
        }

        // Detect case statements
        if (trimmed.find("case ") != std::string::npos || trimmed.find("case(") != std::string::npos) {
            state.case_statement_count++;
        }
        if (trimmed.find("default:") != std::string::npos || trimmed.find("default :") != std::string::npos) {
            state.has_default_case = true;
        }

        // Detect if without else
        if (trimmed.find("if ") != std::string::npos || trimmed.find("if(") != std::string::npos) {
            if (in_always) {
                // Simple heuristic: count if we find a matching else later
            }
        }
        if (trimmed.find("else") != std::string::npos && trimmed.find("else if") == std::string::npos) {
            state.has_else_clause = true;
        }

        // Detect #delay statements
        if (trimmed.find('#') != std::string::npos) {
            size_t hash_pos = trimmed.find('#');
            // Check if it's a delay: # followed by digit
            if (hash_pos + 1 < trimmed.size() && std::isdigit(trimmed[hash_pos + 1])) {
                state.has_delay_statements = true;
            }
        }

        // Detect DFF instances (check for DFF cell instantiation)
        if (trimmed.find("DFF") != std::string::npos) {
            if (trimmed.find("rst") != std::string::npos || trimmed.find("reset") != std::string::npos
                || trimmed.find("RN") != std::string::npos || trimmed.find("SN") != std::string::npos) {
                state.dff_with_reset++;
            } else {
                state.dff_without_reset++;
            }
        }

        // Track used signals (RHS of assignments, in expressions)
        // Simple: any identifier not in a declaration is a usage
        // This is a heuristic — full analysis needs AST

        // Detect end of always block
        if (trimmed == "end" || trimmed.find("end //") == 0 || trimmed.find("end /*") == 0) {
            // Edge case: end might end an always begin..end
        }
        if (trimmed.find("endmodule") != std::string::npos) {
            break; // Only process first module for now
        }
    }

    // ===== Run checks on collected data =====

    // 1. Check for unused input ports
    for (const auto &port : state.input_ports) {
        if (state.used_signals.find(port) == state.used_signals.end()) {
            state.add_warning("Unused input port: '" + port + "'");
        }
    }

    // 2. Check for unused output ports
    for (const auto &port : state.output_ports) {
        if (state.driven_signals.find(port) == state.driven_signals.end()) {
            state.add_warning("Output port may not be driven: '" + port + "'");
        }
    }

    // 3. Check for blocking assignments in sequential always blocks
    if (state.blocking_assign_in_sequential > 0) {
        state.add_warning("Blocking assignments (=) detected in " +
            std::to_string(state.blocking_assign_in_sequential) +
            " places within sequential always block. Use non-blocking (<=) instead.");
    }

    // 4. Check for non-blocking assignments in combinational always blocks
    if (state.nonblocking_assign_in_combinational > 0) {
        state.add_warning("Non-blocking assignments (<=) detected in " +
            std::to_string(state.nonblocking_assign_in_combinational) +
            " places within combinational always block. Use blocking (=) instead.");
    }

    // 5. Check for case without default
    if (state.case_statement_count > 0 && !state.has_default_case) {
        state.add_warning("Case statement without default clause may infer latch. Add default to avoid latch inference.");
    }

    // 6. Check for incomplete sensitivity list
    if (state.always_level_count > 0) {
        // Check if sensitivity list uses @(*) or @*
        bool has_auto_sens = false;
        for (const auto &sens : state.sensitivity_signals) {
            if (sens.find('*') != std::string::npos || sens.find("(*)") != std::string::npos) {
                has_auto_sens = true;
                break;
            }
        }
        if (!has_auto_sens && state.always_level_count > 0) {
            // Check if it's a posedge/negedge block (sequential)
            bool is_seq = false;
            for (const auto &sens : state.sensitivity_signals) {
                if (sens.find("posedge") != std::string::npos || sens.find("negedge") != std::string::npos) {
                    is_seq = true;
                    break;
                }
            }
            if (!is_seq && state.always_level_count > 0) {
                state.add_warning("Combinational always block without @(*) or @*. "
                    "Use @(*) for auto-sensitivity to avoid simulation-synthesis mismatch.");
            }
        }
    }

    // 7. Check for #delay statements
    if (state.has_delay_statements) {
        state.add_warning("#delay statements detected. These are not synthesizable and should be removed.");
    }

    // 8. Check for DFF without reset
    if (state.dff_without_reset > 0) {
        state.add_warning(std::to_string(state.dff_without_reset) +
            " DFF instances without reset detected. Consider adding reset for predictable behavior.");
    }

    // 9. Check signal naming conventions
    for (const auto &sig : state.declared_signals) {
        bool has_upper = false;
        bool has_lower = false;
        bool has_underscore = false;
        for (char c : sig) {
            if (std::isupper(c)) has_upper = true;
            if (std::islower(c)) has_lower = true;
            if (c == '_') has_underscore = true;
        }
        // Check for mixed case without underscore (camelCase without clear prefix)
        if (has_upper && has_lower && !has_underscore && sig.length() > 3) {
            state.naming_violations++;
        }
    }
    if (state.naming_violations > 3) {
        state.add_info("Consider using consistent naming conventions (e.g., snake_case for signals).");
    }
}

// ==========================================================================
// RTLIL-level lint checks (operate on synthesized netlist)
// ==========================================================================

LintResult lint_module(RTLIL::Design *design, const std::string &module_name) {
    LintResult result;
    result.passed = true;
    result.warning_count = 0;
    result.error_count = 0;
    std::ostringstream report;

    RTLIL::Module *mod = design->findModule(RTLIL::IdString("$" + module_name));
    if (!mod) {
        result.passed = false;
        result.error_count = 1;
        report << "Error: Module '" << module_name << "' not found" << std::endl;
        result.report = report.str();
        return result;
    }

    // ========== 1. Check for undriven wires ==========
    {
        int undriven_count = 0;
        for (auto &it : mod->wires_) {
            RTLIL::Wire *wire = it.second;
            if (!wire) continue;
            if (wire->port_input_) continue; // Input ports don't need to be driven

            bool driven = false;
            // Check if any cell drives this wire
            for (auto &cell_it : mod->cells_) {
                RTLIL::Cell *cell = cell_it.second;
                if (!cell) continue;
                for (auto &conn : cell->connections_) {
                    // Output ports of cells: Y, Q, etc.
                    if (conn.first == RTLIL::IdString("\\Y") ||
                        conn.first == RTLIL::IdString("\\Q") ||
                        conn.first == RTLIL::IdString("\\QN")) {
                        for (auto &bit : conn.second.bits_) {
                            if (bit.wire_idx >= 0 && (size_t)bit.wire_idx < mod->wires_.size()) {
                                auto wit = mod->wires_.begin();
                                std::advance(wit, bit.wire_idx);
                                if (wit->second == wire) {
                                    driven = true;
                                    break;
                                }
                            }
                        }
                        if (driven) break;
                    }
                }
                if (driven) break;
            }

            // Also check if it's driven by port connections (module-level)
            // Module-level assigns are represented as cells with type $connect or $assert

            if (!driven && !wire->name.empty() && wire->name.str() != "\\$verilator") {
                undriven_count++;
                std::string wname = clean_name(wire->name.str());
                if (!wname.empty() && wname[0] != '$') {
                    if (undriven_count <= 20) {
                        report << "Warning: Wire '" << wname << "' is not driven\n";
                    }
                }
            }
        }
        if (undriven_count > 20) {
            report << "Warning: " << (undriven_count - 20) << " more undriven wires (suppressed)\n";
        }
        result.warning_count += undriven_count;
    }

    // ========== 2. Check for multi-driver nets ==========
    {
        std::map<int, int> driver_count;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            for (auto &conn : cell->connections_) {
                std::string port = conn.first.str();
                if (port == "\\Y" || port == "\\Q" || port == "\\QN") {
                    for (auto &bit : conn.second.bits_) {
                        if (bit.wire_idx >= 0) {
                            driver_count[bit.wire_idx]++;
                        }
                    }
                }
            }
        }
        int multi_driver_errors = 0;
        for (auto &[wire_idx, count] : driver_count) {
            if (count > 1) {
                multi_driver_errors++;
                if (multi_driver_errors <= 10) {
                    report << "Error: Wire idx " << wire_idx << " has " << count
                           << " drivers (multi-driver conflict)\n";
                }
            }
        }
        if (multi_driver_errors > 10) {
            report << "Error: " << (multi_driver_errors - 10) << " more multi-driver wires (suppressed)\n";
        }
        result.error_count += multi_driver_errors;
    }

    // ========== 3. Check for combinational loops (DFS-based) ==========
    {
        // Build adjacency graph: wire -> wires it feeds into
        // Edge: cell A's output wire -> cell B's input wire
        std::map<int, std::vector<int>> adj;  // wire_idx -> [wire_idxes it feeds]
        std::map<int, std::vector<int>> radj; // reverse adjacency for cycle detection

        // Collect all sequential cells (DFFs) to exclude from loop detection
        std::set<std::string> sequential_cell_types = {"$dff", "$dffe", "$dffsr", "$sdff", "$adff",
            "$dlatch", "$dlatchsr"};
        std::set<int> sequential_output_wires;

        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            std::string type = cell->type_name().str();

            // Check if it's a sequential cell
            bool is_seq = false;
            for (auto &seq_type : sequential_cell_types) {
                if (type.find(seq_type) != std::string::npos) {
                    is_seq = true;
                    break;
                }
            }

            if (is_seq) {
                // Mark Q output wires as sequential (they break combinational loops)
                auto q_it = cell->connections_.find(RTLIL::IdString("\\Q"));
                if (q_it != cell->connections_.end()) {
                    for (auto &bit : q_it->second.bits_) {
                        if (bit.wire_idx >= 0) {
                            sequential_output_wires.insert(bit.wire_idx);
                        }
                    }
                }
                continue;
            }

            // Combinational cell: find output wire and input wires
            std::string out_wire_str;
            int out_wire_idx = -1;

            // Check Y output
            auto y_it = cell->connections_.find(RTLIL::IdString("\\Y"));
            if (y_it != cell->connections_.end()) {
                for (auto &bit : y_it->second.bits_) {
                    if (bit.wire_idx >= 0) {
                        out_wire_idx = bit.wire_idx;
                        out_wire_str = std::to_string(bit.wire_idx);
                        break;
                    }
                }
            }
            // Check Q output (for combinational feedback through sequential)
            if (out_wire_idx < 0) {
                auto q_it = cell->connections_.find(RTLIL::IdString("\\Q"));
                if (q_it != cell->connections_.end()) {
                    for (auto &bit : q_it->second.bits_) {
                        if (bit.wire_idx >= 0) {
                            out_wire_idx = bit.wire_idx;
                            out_wire_str = std::to_string(bit.wire_idx);
                            break;
                        }
                    }
                }
            }

            if (out_wire_idx < 0 || out_wire_str.empty()) continue;

            // Add edges from input wires to output wire
            for (auto &conn : cell->connections_) {
                std::string port = conn.first.str();
                if (port == "\\Y" || port == "\\Q" || port == "\\QN") continue;
                for (auto &bit : conn.second.bits_) {
                    if (bit.wire_idx >= 0 && bit.wire_idx != out_wire_idx) {
                        adj[bit.wire_idx].push_back(out_wire_idx);
                        radj[out_wire_idx].push_back(bit.wire_idx);
                    }
                }
            }
        }

        // DFS-based cycle detection (only on combinational nodes)
        // Ignore edges that go through sequential output wires
        std::set<int> visited;
        std::set<int> in_stack;
        std::function<bool(int)> dfs_cycle = [&](int node) -> bool {
            if (sequential_output_wires.count(node)) return false; // Sequential breaks the loop
            visited.insert(node);
            in_stack.insert(node);

            auto it = adj.find(node);
            if (it != adj.end()) {
                for (int neighbor : it->second) {
                    if (sequential_output_wires.count(neighbor)) continue;
                    if (in_stack.count(neighbor)) {
                        return true; // Cycle detected
                    }
                    if (!visited.count(neighbor)) {
                        if (dfs_cycle(neighbor)) return true;
                    }
                }
            }

            in_stack.erase(node);
            return false;
        };

        int loop_count = 0;
        for (auto &[node, _] : adj) {
            if (!visited.count(node) && !sequential_output_wires.count(node)) {
                if (dfs_cycle(node)) {
                    loop_count++;
                    if (loop_count <= 5) {
                        report << "Error: Combinational loop detected involving wire idx " << node << "\n";
                    }
                }
            }
        }
        if (loop_count > 5) {
            report << "Error: " << (loop_count - 5) << " more combinational loops detected (suppressed)\n";
        }
        result.error_count += loop_count;
    }

    // ========== 4. Check for unconnected ports ==========
    {
        int unconnected_count = 0;
        for (auto &wit : mod->wires_) {
            RTLIL::Wire *wire = wit.second;
            if (!wire || wire->port_id() == 0) continue;

            bool connected = false;
            for (auto &cell_it : mod->cells_) {
                RTLIL::Cell *cell = cell_it.second;
                if (!cell) continue;
                for (auto &conn : cell->connections_) {
                    for (auto &bit : conn.second.bits_) {
                        if (bit.wire_idx >= 0 && (size_t)bit.wire_idx < mod->wires_.size()) {
                            auto w_it = mod->wires_.begin();
                            std::advance(w_it, bit.wire_idx);
                            if (w_it->second == wire) {
                                connected = true;
                                break;
                            }
                        }
                    }
                    if (connected) break;
                }
                if (connected) break;
            }

            if (!connected) {
                unconnected_count++;
                std::string wname = clean_name(wire->name.str());
                if (wire->port_output_) {
                    report << "Warning: Output port '" << wname << "' is not connected\n";
                    result.warning_count++;
                } else if (wire->port_input_) {
                    report << "Warning: Input port '" << wname << "' is not connected\n";
                    result.warning_count++;
                }
            }
        }
    }

    // ========== 5. Check for max fanout violations ==========
    {
        const int MAX_FANOUT = 32;
        std::map<int, int> fanout_count;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            for (auto &conn : cell->connections_) {
                std::string port = conn.first.str();
                if (port == "\\Y" || port == "\\Q" || port == "\\QN") continue;
                for (auto &bit : conn.second.bits_) {
                    if (bit.wire_idx >= 0) {
                        fanout_count[bit.wire_idx]++;
                    }
                }
            }
        }
        int fanout_warnings = 0;
        for (auto &[wire_idx, count] : fanout_count) {
            if (count > MAX_FANOUT) {
                fanout_warnings++;
                if (fanout_warnings <= 10) {
                    report << "Warning: Wire idx " << wire_idx << " has fanout " << count
                           << " (max " << MAX_FANOUT << ")\n";
                }
            }
        }
        if (fanout_warnings > 10) {
            report << "Warning: " << (fanout_warnings - 10) << " more high-fanout wires (suppressed)\n";
        }
        result.warning_count += fanout_warnings;
    }

    // ========== 6. Check for width mismatches ==========
    {
        int width_mismatches = 0;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            int in_width = 0, out_width = 0;
            for (auto &conn : cell->connections_) {
                if (conn.first == RTLIL::IdString("\\Y") || conn.first == RTLIL::IdString("\\Q")) {
                    out_width = conn.second.width();
                } else if (conn.first != RTLIL::IdString("\\Y") &&
                           conn.first != RTLIL::IdString("\\Q") &&
                           conn.first != RTLIL::IdString("\\QN") &&
                           conn.first != RTLIL::IdString("\\CLK") &&
                           conn.first != RTLIL::IdString("\\R") &&
                           conn.first != RTLIL::IdString("\\S")) {
                    if (conn.second.width() > 0)
                        in_width = std::max(in_width, conn.second.width());
                }
            }
            if (out_width > 0 && in_width > 0 && out_width != in_width) {
                if (std::abs(out_width - in_width) > 1) {
                    width_mismatches++;
                    if (width_mismatches <= 10) {
                        report << "Warning: Width mismatch in cell '" << clean_name(cell->name.str())
                               << "': output=" << out_width << " input=" << in_width << "\n";
                    }
                }
            }
        }
        if (width_mismatches > 10) {
            report << "Warning: " << (width_mismatches - 10) << " more width mismatches (suppressed)\n";
        }
        result.warning_count += width_mismatches;
    }

    // ========== 7. Check for latch inference candidates ==========
    {
        int latch_count = 0;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            std::string type = cell->type_name().str();
            if (type.find("$dlatch") != std::string::npos || type.find("$_DLATCH") != std::string::npos) {
                latch_count++;
                if (latch_count <= 5) {
                    report << "Warning: Inferred latch '" << clean_name(cell->name.str())
                           << "' (type: " << type << "). Check for incomplete case/if statements.\n";
                }
            }
        }
        if (latch_count > 5) {
            report << "Warning: " << (latch_count - 5) << " more inferred latches (suppressed)\n";
        }
        if (latch_count > 0) {
            result.warning_count += latch_count;
        }
    }

    // ========== 8. Check for clock domain crossings (basic) ==========
    {
        // Identify all clock signals
        std::set<std::string> clock_signals;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            auto clk_it = cell->connections_.find(RTLIL::IdString("\\CLK"));
            if (clk_it != cell->connections_.end()) {
                for (auto &bit : clk_it->second.bits_) {
                    if (bit.wire_idx >= 0) {
                        auto w_it = mod->wires_.begin();
                        std::advance(w_it, bit.wire_idx);
                        clock_signals.insert(clean_name(w_it->second->name.str()));
                    }
                }
            }
        }

        if (clock_signals.size() > 1) {
            // Multiple clocks detected — CDC check
            // Count fanout of each clock to detect potential CDC issues
            std::map<std::string, int> clock_fanout;
            for (auto &cell_it : mod->cells_) {
                RTLIL::Cell *cell = cell_it.second;
                if (!cell) continue;
                auto clk_it = cell->connections_.find(RTLIL::IdString("\\CLK"));
                if (clk_it != cell->connections_.end()) {
                    for (auto &bit : clk_it->second.bits_) {
                        if (bit.wire_idx >= 0) {
                            auto w_it = mod->wires_.begin();
                            std::advance(w_it, bit.wire_idx);
                            clock_fanout[clean_name(w_it->second->name.str())]++;
                        }
                    }
                }
            }

            std::string clock_details;
            for (auto &[clk, cnt] : clock_fanout) {
                if (!clock_details.empty()) clock_details += ", ";
                clock_details += clk + "(" + std::to_string(cnt) + " loads)";
            }
            report << "Info: Multiple clock domains detected: " << clock_details << "\n";
            report << "Info: Check for proper CDC synchronization (dual-flop synchronizer) "
                      "if signals cross clock domains\n";
            result.warning_count += 0; // Don't count as warning, just info
        }
    }

    // ========== 9. Check for flip-flops without reset ==========
    {
        int dff_no_reset = 0, dff_with_reset = 0;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            std::string type = cell->type_name().str();
            if (type.find("$dff") != std::string::npos || type.find("$sdff") != std::string::npos) {
                bool has_reset = false;
                // Check for reset ports
                for (auto &conn : cell->connections_) {
                    std::string port = conn.first.str();
                    if (port == "\\R" || port == "\\S" || port == "\\RN" || port == "\\SN") {
                        has_reset = true;
                        break;
                    }
                }
                if (has_reset) {
                    dff_with_reset++;
                } else {
                    dff_no_reset++;
                }
            }
        }
        if (dff_no_reset > 0) {
            report << "Warning: " << dff_no_reset << " flip-flops without reset. "
                   << "(" << dff_with_reset << " with reset)\n";
            result.warning_count++;
        }
    }

    // ========== 10. Check for constant-driven flip-flops ==========
    {
        int const_dff = 0;
        for (auto &cell_it : mod->cells_) {
            RTLIL::Cell *cell = cell_it.second;
            if (!cell) continue;
            std::string type = cell->type_name().str();
            if (type.find("$dff") != std::string::npos) {
                auto d_it = cell->connections_.find(RTLIL::IdString("\\D"));
                if (d_it != cell->connections_.end()) {
                    bool all_const = true;
                    for (auto &bit : d_it->second.bits_) {
                        if (!bit.is_constant()) {
                            all_const = false;
                            break;
                        }
                    }
                    if (all_const) {
                        const_dff++;
                    }
                }
            }
        }
        if (const_dff > 0) {
            report << "Warning: " << const_dff << " flip-flops with constant input (may be optimized away)\n";
            result.warning_count++;
        }
    }

    result.passed = (result.error_count == 0);
    result.report = report.str();
    return result;
}

/// Run source-level lint checks on Verilog RTL before synthesis
LintResult lint_source(const std::string &source_code, const std::string &module_name) {
    LintResult result;
    result.passed = true;
    result.warning_count = 0;
    result.error_count = 0;
    std::ostringstream report;

    report << "=== Source-Level Lint Check for '" << module_name << "' ===\n\n";

    RtlLintState state;
    state.module_name = module_name;
    analyze_verilog_source(source_code, state);

    // Report findings
    for (const auto &err : state.errors) {
        report << "Error: " << err << "\n";
        result.error_count++;
    }
    for (const auto &warn : state.warnings) {
        report << "Warning: " << warn << "\n";
        result.warning_count++;
    }
    for (const auto &info : state.infos) {
        report << "Info: " << info << "\n";
    }

    result.passed = (result.error_count == 0);
    result.report = report.str();
    return result;
}

} // namespace LintCheck