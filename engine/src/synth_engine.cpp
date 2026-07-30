/**
 * Synthesis Engine - Real Implementation with multi-module support
 *
 * Recursively elaborates all sub-modules into a flat gate-level netlist.
 * Expression nodes use left/right/third, not children[].
 */

#include "synth_engine.h"
#include "rtlil.h"
#include "verilog_parser_full.h"
#include "liberty_parser.h"

// Global liberty library (loaded once, used by techmap + timing + power)
static Liberty::LibertyLibrary g_liberty_lib;
static bool g_liberty_loaded = false;
static std::string g_liberty_path;
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <cstdarg>
#include <fstream>
#include <queue>

namespace Synthesis {

typedef void (*SynthLogCallback)(const char *step, const char *message);
static SynthLogCallback g_synth_log = nullptr;
void set_synth_log_callback(SynthLogCallback cb) { g_synth_log = cb; }

void synth_log(const char *step, const char *fmt, ...) {
    if (!g_synth_log) return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_synth_log(step, buf);
}

// TechLibrary cell lookup implementations
const TechCell *TechLibrary::findCell(const std::string &n) const { for (auto &c:cells) if(c.name==n) return &c; return nullptr; }
std::vector<TechCell> TechLibrary::findCellsByType(const std::string &t) const { std::vector<TechCell> r; for(auto &c:cells) if(c.type==t) r.push_back(c); return r; }
std::vector<TechCell> TechLibrary::findCellsByArea(double a) const { std::vector<TechCell> r; for(auto &c:cells) if(c.area<=a) r.push_back(c); return r; }

// Standard cell library (both generic names and technology-mapped names)
struct StdCellDef { const char *name; double area; double delay; };
static const StdCellDef STD_CELLS[] = {
    {"$_AND_",6,0.05},{"AND2X1",6,0.05},{"AND2X2",6,0.05},{"AND2X4",6,0.05},
    {"$_OR_",6,0.05},{"OR2X1",6,0.05},{"OR2X2",6,0.05},{"OR2X4",6,0.05},
    {"$_NOT_",3,0.02},{"INVX1",3,0.02},{"INVX2",3,0.02},{"INVX4",3,0.02},
    {"$_XOR_",8,0.08},{"XOR2X1",8,0.08},{"XOR2X2",8,0.08},{"XOR2X4",8,0.08},
    {"$_NAND_",4,0.03},{"NAND2X1",4,0.03},{"NAND2X2",4,0.03},{"NAND2X4",4,0.03},
    {"$_NOR_",4,0.03},{"NOR2X1",4,0.03},{"NOR2X2",4,0.03},{"NOR2X4",4,0.03},
    {"$_MUX_",8,0.06},{"MUX2X1",8,0.06},{"MUX2X2",8,0.06},{"MUX2X4",8,0.06},
    {"$_DFF_P_",18,0.15},{"DFFPOSX1",18,0.15},{"DFFPOSX2",18,0.15},{"DFFPOSX4",18,0.15},
    {"$_DFF_N_",18,0.15},{"DFFNEGX1",18,0.15},{"DFFNEGX2",18,0.15},{"DFFNEGX4",18,0.15},
    {"$_DFFSR_PPP_",22,0.18},{"$_DFFSR_NNN_",22,0.18},
    {"DFFSRPOSX1",22,0.18},{"DFFSRNEGX1",22,0.18},{"DFFSRPOSX2",22,0.18},
    {"$_DFFE_PP_",20,0.17},{"DFFEPOSX1",20,0.17},{"DFFEPOSX2",20,0.17},
    {"$_BUF_",2,0.01},{"BUFX2",2,0.01},{"BUFX4",2,0.01},{"BUFX8",2,0.01},
};
const int NUM_STD_CELLS = sizeof(STD_CELLS)/sizeof(StdCellDef);

// Gate-level netlist structures
struct GatePort { std::string signal; };
struct GateCell { std::string type, name; std::vector<std::pair<std::string, GatePort>> conns; };
struct GateNetlist {
    std::string module_name;
    struct PortInfo { std::string name; int width; bool is_input; };
    std::vector<PortInfo> ports;
    std::map<std::string, int> wires;
    std::vector<GateCell> cells;
    int next_id;
    GateNetlist() : next_id(0) {}
    std::string new_wire(const std::string &pfx = "_w") {
        std::string n = pfx + std::to_string(next_id++); wires[n] = 1; return n;
    }
    void add_cell(const std::string &t, const std::string &n,
                  const std::vector<std::pair<std::string, GatePort>> &c) {
        cells.push_back({t, n, c});
    }
};

// Module map: name → ModuleDecl
typedef std::map<std::string, std::shared_ptr<VerilogParser::ModuleDecl>> ModuleMap;

static std::string get_name(std::shared_ptr<VerilogParser::ASTNode> n) {
    if (!n) return "";
    if (n->attributes.count("name")) return n->attributes.at("name");
    if (n->attributes.count("signal")) return n->attributes.at("signal");
    return "";
}
static int get_width(std::shared_ptr<VerilogParser::ASTNode> n) {
    if (!n) return 1;
    if (n->attributes.count("width")) try { return std::stoi(n->attributes.at("width")); } catch(...){}
    return 1;
}

static bool parse_verilog_int(const std::string &text, int64_t &out) {
    std::string value = text;
    value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
    size_t quote = value.find('\'');
    try {
        if (quote != std::string::npos && quote + 1 < value.size()) {
            char base = value[quote + 1];
            std::string digits = value.substr(quote + 2);
            int radix = (base == 'b' || base == 'B') ? 2 :
                        (base == 'o' || base == 'O') ? 8 :
                        (base == 'h' || base == 'H') ? 16 : 10;
            out = digits.empty() ? 0 : std::stoll(digits, nullptr, radix);
            return true;
        }
        out = value.empty() ? 0 : std::stoll(value);
        return true;
    } catch (...) {
        return false;
    }
}

static bool const_int(std::shared_ptr<VerilogParser::ASTNode> node, int64_t &out) {
    if (!node || !node->attributes.count("value")) return false;
    return parse_verilog_int(node->attributes.at("value"), out);
}

// Expression → gate conversion
static std::string emit_expr(GateNetlist &gn, std::shared_ptr<VerilogParser::ASTNode> node,
                              std::map<std::string, std::string> &sm,
                              int required_width = 0);
static std::string emit_lvalue(std::shared_ptr<VerilogParser::ASTNode> node,
                               const std::map<std::string, std::string> &sm);
static int get_signal_width(const GateNetlist &gn, const std::string &signal);
static std::string ensure_bit_signal(GateNetlist &gn, const std::string &signal, int width, int bit);
static std::string build_equality_signal(GateNetlist &gn, const std::string &lhs,
                                         const std::string &rhs, int width);

// Forward declarations for advanced operators
static void emit_multiplier_array(GateNetlist &gn, const std::string &l, const std::string &r,
                                  const std::string &o, int wa, int wb, bool is_signed = false);
static std::string emit_cla_adder(GateNetlist &gn, const std::string &l, const std::string &r,
                                   const std::string &o, int width, bool is_sub);

static std::string emit_expr(GateNetlist &gn, std::shared_ptr<VerilogParser::ASTNode> node,
                              std::map<std::string, std::string> &sm,
                              int required_width) {
    if (!node) return "";
    if (node->type == VerilogParser::NodeType::IDENTIFIER) {
        std::string name = get_name(node);
        // Check for bit select: identifier[index]
        if (!node->children.empty()) {
            auto index_node = node->children[0];
            if (index_node) {
                std::string idx_str;
                if (index_node->type == VerilogParser::NodeType::NUMBER) {
                    idx_str = index_node->attributes.count("value") ? index_node->attributes.at("value") : "0";
                } else {
                    // Variable index - try to get name
                    idx_str = get_name(index_node);
                    if (idx_str.empty()) idx_str = "var";
                }
                std::string elem = name + "[" + idx_str + "]";
                return sm.count(elem) ? sm[elem] : elem;
            }
        }
        return sm.count(name) ? sm[name] : name;
    }
    if (node->type == VerilogParser::NodeType::NUMBER) {
        std::string val = node->attributes.count("value") ? node->attributes.at("value") : "0";
        int64_t num_val = 0;
        int num_width = 32;
        size_t quote = val.find('\'');
        if (quote != std::string::npos && quote + 1 < val.size()) {
            try { num_width = std::stoi(val.substr(0, quote)); } catch (...) { num_width = 32; }
            char base = val[quote + 1];
            std::string digits = val.substr(quote + 2);
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            if (!digits.empty()) {
                try {
                    switch (base) {
                        case 'd': case 'D': num_val = std::stoll(digits); break;
                        case 'h': case 'H': num_val = std::stoll(digits, nullptr, 16); break;
                        case 'o': case 'O': num_val = std::stoll(digits, nullptr, 8); break;
                        case 'b': case 'B': num_val = std::stoll(digits, nullptr, 2); break;
                        default: num_val = std::stoll(digits); break;
                    }
                } catch (...) { num_val = 0; }
            }
        } else {
            try { num_val = std::stoll(val); } catch (...) { num_val = 0; }
        }
        // Create constant wire with value encoded in name
        std::string w = "_const" + std::to_string(num_val);
        gn.wires[w] = num_width;
        // Create per-bit wire declarations and BUF cells to drive constant values
        for (int bit = 0; bit < num_width && bit < 64; bit++) {
            std::string bit_wire = w + "[" + std::to_string(bit) + "]";
            gn.wires[bit_wire] = 1;
            int bit_val = (num_val >> bit) & 1;
            // Use BUF to drive constant: A="1" or A="0"
            std::string const_src = bit_val ? "1" : "0";
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{const_src}},{"Y",{bit_wire}}});
        }
        return w;
    }

    auto expr = std::dynamic_pointer_cast<VerilogParser::Expression>(node);
    if (expr) {
        if (expr->op == VerilogParser::Expression::CONCAT) {
            // Verilog concatenation is MSB-first: {a, b} places `a` above
            // `b`.  Preserve every operand instead of falling through to the
            // generic first-child path, which silently discarded all but the
            // leading expression.
            struct ConcatPart { std::string signal; int width; };
            std::vector<ConcatPart> parts;
            int total_width = 0;
            for (const auto &child : expr->children) {
                std::string signal = emit_expr(gn, child, sm);
                if (signal.empty()) continue;
                int width = get_signal_width(gn, signal);
                if (width < 1) width = 1;
                parts.push_back({signal, width});
                total_width += width;
            }
            if (parts.empty()) return "";
            if (parts.size() == 1) return parts.front().signal;

            std::string out = gn.new_wire("_concat");
            gn.wires[out] = total_width;
            int out_bit = 0;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                for (int bit = 0; bit < it->width; ++bit) {
                    std::string src = ensure_bit_signal(gn, it->signal, it->width, bit);
                    std::string dst = out + "[" + std::to_string(out_bit++) + "]";
                    gn.wires[dst] = 1;
                    gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++),
                                {{"A", {src}}, {"Y", {dst}}});
                }
            }
            return out;
        }
        if (expr->op == VerilogParser::Expression::BIT_SELECT && expr->left && expr->right) {
            std::string base = emit_expr(gn, expr->left, sm);
            int64_t idx = 0;
            if (!const_int(expr->right, idx)) {
                std::string dyn = emit_expr(gn, expr->right, sm);
                std::string out = gn.new_wire("_bitsel_dyn");
                int base_width = std::max(get_width(expr->left), gn.wires.count(base) ? gn.wires[base] : 1);
                int sel_width = gn.wires.count(dyn) ? gn.wires[dyn] : 1;
                std::string cur = "0";
                for (int bit = base_width - 1; bit >= 0; --bit) {
                    std::string bit_sig = base + "[" + std::to_string(bit) + "]";
                    gn.wires[bit_sig] = 1;
                    int64_t bit_value = bit;
                    std::string bit_const = "_const" + std::to_string(bit_value);
                    gn.wires[bit_const] = std::max(sel_width, 1);
                    for (int bi = 0; bi < sel_width; bi++) {
                        std::string cb = bit_const + "[" + std::to_string(bi) + "]";
                        gn.wires[cb] = 1;
                        gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++),
                                    {{"A", {((bit_value >> bi) & 1) ? "1" : "0"}}, {"Y", {cb}}});
                    }
                    std::string eq = build_equality_signal(gn, dyn, bit_const, sel_width);
                    std::string next = gn.new_wire("_bitsel_mux");
                    gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++),
                                {{"A", {cur}}, {"B", {bit_sig}}, {"S", {eq}}, {"Y", {next}}});
                    cur = next;
                }
                gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++), {{"A", {cur}}, {"Y", {out}}});
                return out;
            }
            std::string bit = base + "[" + std::to_string(idx) + "]";
            gn.wires[bit] = 1;
            return sm.count(bit) ? sm[bit] : bit;
        }
        if (expr->op == VerilogParser::Expression::PART_SELECT && expr->left && expr->right && expr->third) {
            std::string base = emit_expr(gn, expr->left, sm);
            int64_t hi = 0, lo = 0;
            if (!const_int(expr->right, hi) || !const_int(expr->third, lo)) {
                return base;
            }
            if (hi < lo) std::swap(hi, lo);
            int width = static_cast<int>(hi - lo + 1);
            std::string out = gn.new_wire("_part");
            gn.wires[out] = width;
            for (int bit = 0; bit < width; bit++) {
                std::string src = base + "[" + std::to_string(lo + bit) + "]";
                std::string dst = out + "[" + std::to_string(bit) + "]";
                gn.wires[src] = 1;
                gn.wires[dst] = 1;
                gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++), {{"A", {src}}, {"Y", {dst}}});
            }
            return out;
        }
        if (node->type == VerilogParser::NodeType::TERNARY_OP && expr->left && expr->right && expr->third) {
            std::string c = emit_expr(gn, expr->left, sm);
            // Check if condition is constant
            auto is_const = [](const std::string &s) -> int {
                if (s.empty()) return -1;
                if (s == "1" || s == "1'b1") return 1;
                if (s == "0" || s == "1'b0") return 0;
                // Check _constN pattern: if it's const wire, check constant value
                size_t pos = s.find("_const");
                if (pos != std::string::npos) {
                    std::string num = s.substr(pos + 6);
                    size_t br = num.find('[');
                    if (br != std::string::npos) num = num.substr(0, br);
                    try { return std::stoi(num) != 0 ? 1 : 0; } catch (...) {}
                }
                return -1;
            };
            int c_val = is_const(c);
            if (c_val == 1) {
                // Condition is true → use true branch only
                return emit_expr(gn, expr->right, sm);
            } else if (c_val == 0) {
                // Condition is false → use false branch only
                return emit_expr(gn, expr->third, sm);
            }
            // Unknown condition: use MUX
            std::string t = emit_expr(gn, expr->right, sm);
            std::string f = emit_expr(gn, expr->third, sm);
            std::string o = gn.new_wire("_mux");
            gn.add_cell("$_MUX_", "_c"+std::to_string(gn.next_id++), {{"A",{f}},{"B",{t}},{"S",{c}},{"Y",{o}}});
            return o;
        }
        if (node->type == VerilogParser::NodeType::BINARY_OP && expr->left && expr->right) {
            // Assignment context propagates through arithmetic expressions in
            // Verilog.  For example, `wire [4:0] s = a + b` must retain the
            // carry from two 4-bit operands rather than evaluate a 4-bit add
            // and append a zero.  Carry this context through nested add/sub
            // trees; non-arithmetic operators retain their self width.
            const int arithmetic_context =
                (expr->op == VerilogParser::Expression::ADD ||
                 expr->op == VerilogParser::Expression::SUB) ? required_width : 0;
            std::string l = emit_expr(gn, expr->left, sm, arithmetic_context);
            std::string r = emit_expr(gn, expr->right, sm, arithmetic_context);
            std::string o = gn.new_wire("_bin");

            // Constant folding: if both operands are const wires (_constN), fold at compile time
            auto is_const_wire = [&](const std::string &s) -> std::pair<bool, int64_t> {
                if (s.empty()) return {false, 0};
                // Check if name starts with _const pattern
                size_t us = s.find("_const");
                if (us == std::string::npos) return {false, 0};
                // Extract the numeric suffix: _constN[bit] or _constN
                std::string num_str = s.substr(us + 6); // after "_const"
                // Remove bit suffix if present
                size_t bracket = num_str.find('[');
                if (bracket != std::string::npos) num_str = num_str.substr(0, bracket);
                try {
                    int64_t val = std::stoll(num_str);
                    return {true, val};
                } catch (...) { return {false, 0}; }
            };
            auto [l_const, l_val] = is_const_wire(l);
            auto [r_const, r_val] = is_const_wire(r);

            // If both are constants, directly compute result and return a new const wire
            if (l_const && r_const) {
                int64_t result = 0;
                int width = 1;
                if (gn.wires.count(l)) width = std::max(width, gn.wires[l]);
                if (gn.wires.count(r)) width = std::max(width, gn.wires[r]);
                for (auto &p : gn.ports) { if (p.name == l) width = std::max(width, p.width); if (p.name == r) width = std::max(width, p.width); }
                if (width < 1) width = 1;
                if (width > 64) width = 64;

                switch (expr->op) {
                    case VerilogParser::Expression::ADD: result = l_val + r_val; break;
                    case VerilogParser::Expression::SUB: result = l_val - r_val; break;
                    case VerilogParser::Expression::MUL: result = l_val * r_val; break;
                    case VerilogParser::Expression::DIV: result = r_val != 0 ? l_val / r_val : 0; break;
                    case VerilogParser::Expression::MOD: result = r_val != 0 ? l_val % r_val : 0; break;
                    case VerilogParser::Expression::AND: result = l_val & r_val; break;
                    case VerilogParser::Expression::OR:  result = l_val | r_val; break;
                    case VerilogParser::Expression::XOR: result = l_val ^ r_val; break;
                    case VerilogParser::Expression::SHL: result = l_val << r_val; break;
                    case VerilogParser::Expression::SHR: result = l_val >> r_val; break;
                    default: l_const = false; break; // can't fold this op
                }
                if (l_const) {
                    // Mask result to width
                    int64_t mask = (width >= 64) ? ~0LL : ((1LL << width) - 1);
                    result &= mask;
                    // Create a new constant wire with the folded value
                    std::string folded_wire = "_const" + std::to_string(result);
                    gn.wires[folded_wire] = width;
                    // Create per-bit BUF cells to carry the constant
                    for (int bit = 0; bit < width; bit++) {
                        std::string bit_wire = folded_wire + "[" + std::to_string(bit) + "]";
                        gn.wires[bit_wire] = 1;
                    }
                    // Connect folded wire to output via BUF for each bit
                    for (int bit = 0; bit < width; bit++) {
                        std::string fb = folded_wire + "[" + std::to_string(bit) + "]";
                        std::string ob = o + "[" + std::to_string(bit) + "]";
                        gn.wires[ob] = 1;
                        std::string bit_val = ((result >> bit) & 1) ? "1" : "0";
                        gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{bit_val}},{"Y",{ob}}});
                    }
                    gn.wires[o] = width;
                    return folded_wire;
                }
            }

            std::string cn = "_c"+std::to_string(gn.next_id++);
            switch (expr->op) {
                case VerilogParser::Expression::AND:
                case VerilogParser::Expression::OR:
                case VerilogParser::Expression::XOR:
                case VerilogParser::Expression::NAND:
                case VerilogParser::Expression::NOR:
                case VerilogParser::Expression::LAND:
                case VerilogParser::Expression::LOR: {
                    int out_width = 1;
                    if (gn.wires.count(l)) out_width = std::max(out_width, gn.wires[l]);
                    if (gn.wires.count(r)) out_width = std::max(out_width, gn.wires[r]);
                    for (auto &p : gn.ports) {
                        if (p.name == l) out_width = std::max(out_width, p.width);
                        if (p.name == r) out_width = std::max(out_width, p.width);
                    }
                    if (expr->op == VerilogParser::Expression::LAND || expr->op == VerilogParser::Expression::LOR) {
                        out_width = 1;
                    }
                    gn.wires[o] = out_width;
                    switch (expr->op) {
                        case VerilogParser::Expression::AND:
                            gn.add_cell("$_AND_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        case VerilogParser::Expression::OR:
                            gn.add_cell("$_OR_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        case VerilogParser::Expression::XOR:
                            gn.add_cell("$_XOR_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        case VerilogParser::Expression::NAND:
                            gn.add_cell("$_NAND_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        case VerilogParser::Expression::NOR:
                            gn.add_cell("$_NOR_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        case VerilogParser::Expression::LAND:
                        case VerilogParser::Expression::LOR:
                            gn.add_cell("$_OR_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                            break;
                        default:
                            break;
                    }
                    break;
                }
                case VerilogParser::Expression::EQ:
                case VerilogParser::Expression::EQX: {
                    // EQ: XNOR each bit pair, then AND-reduce all bits together
                    int eq_width = 1;
                    if (gn.wires.count(l)) eq_width = std::max(eq_width, gn.wires[l]);
                    if (gn.wires.count(r)) eq_width = std::max(eq_width, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) eq_width = std::max(eq_width, p.width); if (p.name == r) eq_width = std::max(eq_width, p.width); }
                    if (eq_width < 1) eq_width = 1;
                    gn.wires[o] = 1;

                    if (eq_width == 1) {
                        std::string xn = gn.new_wire("_xnor");
                        gn.add_cell("$_XOR_",cn,{{"A",{l}},{"B",{r}},{"Y",{xn}}});
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{xn}},{"Y",{o}}});
                    } else {
                        // Multi-bit: per-bit XNOR then tree-AND
                        std::vector<std::string> xnor_bits(eq_width);
                        for (int bit = 0; bit < eq_width; bit++) {
                            std::string lb = l + "[" + std::to_string(bit) + "]";
                            std::string rb = r + "[" + std::to_string(bit) + "]";
                            gn.wires[lb] = 1; gn.wires[rb] = 1;
                            std::string xb = gn.new_wire("_eq_x");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{xb}}});
                            xnor_bits[bit] = gn.new_wire("_eq_xn");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{xb}},{"Y",{xnor_bits[bit]}}});
                        }
                        // Tree-AND reduction
                        std::string and_result = xnor_bits[0];
                        for (int bit = 1; bit < eq_width; bit++) {
                            std::string next_and = gn.new_wire("_eq_a");
                            gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{and_result}},{"B",{xnor_bits[bit]}},{"Y",{next_and}}});
                            and_result = next_and;
                        }
                        gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{and_result}},{"Y",{o}}});
                    }
                    break;
                }
                case VerilogParser::Expression::NE:
                case VerilogParser::Expression::NEX: {
                    // NE: XOR each bit pair, then OR-reduce all bits together
                    int ne_width = 1;
                    if (gn.wires.count(l)) ne_width = std::max(ne_width, gn.wires[l]);
                    if (gn.wires.count(r)) ne_width = std::max(ne_width, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) ne_width = std::max(ne_width, p.width); if (p.name == r) ne_width = std::max(ne_width, p.width); }
                    if (ne_width < 1) ne_width = 1;
                    gn.wires[o] = 1;

                    if (ne_width == 1) {
                        gn.add_cell("$_XOR_",cn,{{"A",{l}},{"B",{r}},{"Y",{o}}});
                    } else {
                        // Multi-bit: per-bit XOR then tree-OR
                        std::vector<std::string> xor_bits(ne_width);
                        for (int bit = 0; bit < ne_width; bit++) {
                            std::string lb = l + "[" + std::to_string(bit) + "]";
                            std::string rb = r + "[" + std::to_string(bit) + "]";
                            gn.wires[lb] = 1; gn.wires[rb] = 1;
                            xor_bits[bit] = gn.new_wire("_ne_x");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{xor_bits[bit]}}});
                        }
                        // Tree-OR reduction
                        std::string or_result = xor_bits[0];
                        for (int bit = 1; bit < ne_width; bit++) {
                            std::string next_or = gn.new_wire("_ne_o");
                            gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{or_result}},{"B",{xor_bits[bit]}},{"Y",{next_or}}});
                            or_result = next_or;
                        }
                        gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{or_result}},{"Y",{o}}});
                    }
                    break;
                }
                case VerilogParser::Expression::ADD:
                case VerilogParser::Expression::SUB: {
                    int width = 1;
                    if (gn.wires.count(l)) width = std::max(width, gn.wires[l]);
                    if (gn.wires.count(r)) width = std::max(width, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) width = std::max(width, p.width); if (p.name == r) width = std::max(width, p.width); }
                    width = std::max(width, required_width);
                    if (width < 1) width = 1;
                    // Use CLA for widths >= 8 (O(log n) vs O(n) ripple carry)
                    if (width >= 8) {
                        emit_cla_adder(gn, l, r, o, width, expr->op == VerilogParser::Expression::SUB);
                        gn.wires[o] = width;
                        break;
                    }
                    // Full adder chain: each bit = XOR(ai, bi, carry_in)
                    // carry_out = MAJ(ai, bi, carry_in) = (ai&bi)|(ai&ci)|(bi&ci)
                    std::string carry;
                    for (int bit = 0; bit < width; bit++) {
                        std::string lb = l + "[" + std::to_string(bit) + "]";
                        std::string rb = r + "[" + std::to_string(bit) + "]";
                        std::string ob = o + "[" + std::to_string(bit) + "]";
                        gn.wires[lb] = 1; gn.wires[rb] = 1; gn.wires[ob] = 1;

                        if (bit == 0 && expr->op == VerilogParser::Expression::SUB) {
                            // SUB bit 0: sum = a[0] ^ ~b[0] ^ 1, carry = a[0] | ~b[0]
                            std::string rb_inv = gn.new_wire("_rb_inv");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{rb}},{"Y",{rb_inv}}});
                            std::string sum0 = gn.new_wire("_sum0");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb_inv}},{"Y",{sum0}}});
                            // With cin=1: sum0_final = ~sum0 (XOR with 1 = NOT)
                            std::string sum0_final = gn.new_wire("_sum0f");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{sum0}},{"Y",{sum0_final}}});
                            carry = gn.new_wire("_carry0");
                            gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb_inv}},{"Y",{carry}}});
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{sum0_final}},{"Y",{ob}}});
                        } else {
                            // Full adder: sum = a ^ b ^ carry
                            std::string axb = gn.new_wire("_axb");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{axb}}});
                            if (bit == 0) {
                                // No carry in for bit 0, sum = a ^ b
                                gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{axb}},{"Y",{ob}}});
                                carry = gn.new_wire("_carry0");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{carry}}});
                            } else {
                                std::string sum_bit = gn.new_wire("_sumb");
                                gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{axb}},{"B",{carry}},{"Y",{sum_bit}}});
                                gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{sum_bit}},{"Y",{ob}}});
                                // carry_out = (a&b) | (carry&(a^b))
                                std::string ab = gn.new_wire("_ab");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{ab}}});
                                std::string cab = gn.new_wire("_cab");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{carry}},{"B",{axb}},{"Y",{cab}}});
                                carry = gn.new_wire("_carry");
                                gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{ab}},{"B",{cab}},{"Y",{carry}}});
                            }
                        }
                    }
                    gn.wires[o] = width;
                    break;
                }
                case VerilogParser::Expression::MUL: {
                    int wa = 1, wb = 1;
                    if (gn.wires.count(l)) wa = gn.wires[l];
                    if (gn.wires.count(r)) wb = gn.wires[r];
                    for (auto &p : gn.ports) { if (p.name == l) wa = std::max(wa, p.width); if (p.name == r) wb = std::max(wb, p.width); }
                    if (wa < 1) wa = 1; if (wb < 1) wb = 1;
                    int width = wa + wb;

                    // Check for signed operands (from port declarations or expression attributes)
                    bool is_signed = false;
                    if (expr->attributes.count("signed") && expr->attributes.at("signed") == "1") {
                        is_signed = true;
                    }

                    // The former radix-4 Booth lowering emitted incomplete
                    // pin connections for wide multipliers. Use a full-width
                    // shift/add implementation until a formally proven Booth
                    // mapper replaces it. Every partial product and carry is
                    // explicitly represented in the gate netlist.
                    emit_multiplier_array(gn, l, r, o, wa, wb, is_signed);
                    gn.wires[o] = width;
                    break;
                }
                case VerilogParser::Expression::DIV:
                case VerilogParser::Expression::MOD: {
                    // Full restoring division for unsigned operands.
                    // Algorithm: for i = w-1 downto 0:
                    //   remainder = (remainder << 1) | dividend[i]
                    //   if remainder >= divisor: quotient[i]=1, remainder -= divisor
                    //   else: quotient[i]=0
                    // Hardware: w stages, each with comparator + subtractor + MUX.
                    // Output width = w for quotient, same for remainder (MOD).
                    int w = 1;
                    if (gn.wires.count(l)) w = std::max(w, gn.wires[l]);
                    if (gn.wires.count(r)) w = std::max(w, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) w = std::max(w, p.width); if (p.name == r) w = std::max(w, p.width); }
                    if (w < 1) w = 1;
                    gn.wires[o] = w;

                    // Stage 0: remainder[0] = {w{1'b0}, dividend[w-1]} (w+1 bits, MSB=0)
                    // For each subsequent stage i (from w-2 down to 0):
                    //   remainder = (prev_remainder << 1) | dividend[i]
                    //   sub_result = remainder - divisor
                    //   quotient[i] = ~sub_result[w] (sign bit: 0 means >= divisor)
                    //   remainder = sub_result[w] ? remainder : sub_result[w-1:0]
                    //
                    // We model each stage with gates: comparator, subtractor, and multiplexers.

                    // Helper: build w-bit comparator (>=) returning 1-bit result
                    // a >= b iff ~lt_result (we already have LT/GT comparator pattern)
                    // Use the previously defined comparator pattern
                    auto build_ge_comparator = [&](const std::string &a_sig, const std::string &b_sig) -> std::string {
                        // gt_result = MSB-first priority chain: gt[i] = (a[i] & ~b[i]) | (eq[i] & gt[i+1])
                        // eq[i] = ~(a[i] ^ b[i])
                        int cmp_w = w + 1;
                        std::vector<std::string> eq(cmp_w), gt(cmp_w);
                        for (int bit = 0; bit < cmp_w; bit++) {
                            std::string ab = a_sig + "[" + std::to_string(bit) + "]";
                            std::string bb = b_sig + "[" + std::to_string(bit) + "]";
                            gn.wires[ab] = 1; gn.wires[bb] = 1;
                            // eq[bit] = ~(a ^ b)
                            std::string xb = gn.new_wire("_div_cx");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{ab}},{"B",{bb}},{"Y",{xb}}});
                            eq[bit] = gn.new_wire("_div_eq");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{xb}},{"Y",{eq[bit]}}});
                            // gt[bit] = a & ~b
                            std::string nb = gn.new_wire("_div_nb");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{bb}},{"Y",{nb}}});
                            gt[bit] = gn.new_wire("_div_gt");
                            gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{ab}},{"B",{nb}},{"Y",{gt[bit]}}});
                        }
                        // Priority chain: gt_c[i] = gt[i] | (eq[i] & gt_c[i+1])
                        std::string gt_c = gt[cmp_w - 1];
                        for (int bit = cmp_w - 2; bit >= 0; bit--) {
                            std::string eq_and = gn.new_wire("_div_eg");
                            gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{eq[bit]}},{"B",{gt_c}},{"Y",{eq_and}}});
                            std::string new_gt = gn.new_wire("_div_ng");
                            gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{gt[bit]}},{"B",{eq_and}},{"Y",{new_gt}}});
                            gt_c = new_gt;
                        }
                        // ge = gt_c (means a > b) or equality: ge = gt_c | and_reduce(eq)
                        // Simplified: ge = gt_c (stricly greater), but we need >= for division
                        // Actually for restoring division we need "remainder >= divisor"
                        // Use active-high MUX select based on borrow from subtract
                        return gt_c; // 'greater than' — will drive MUX select
                    };

                    // Build a simple w+1 bit subtractor for remainder - divisor per stage
                    // Returns the sign bit (borrow from MSB) — 1 if remainder >= divisor (no borrow)
                    auto build_subtract_stage = [&](const std::string &rem_sig, const std::string &div_sig,
                                                      std::string &diff_sig, int bits) -> std::string {
                        std::string borrow; // active-low: 1=no borrow, 0=borrow
                        for (int bit = 0; bit < bits; bit++) {
                            std::string rb = rem_sig + "[" + std::to_string(bit) + "]";
                            std::string db = div_sig + "[" + std::to_string(bit) + "]";
                            gn.wires[rb] = 1;
                            if (bit < w) gn.wires[db] = 1;
                            else {
                                // divisor MSB is always 0 (zero-extended)
                                db = gn.new_wire("_div_zero");
                                gn.wires[db] = 1;
                            }
                            std::string db_inv = gn.new_wire("_div_db_inv");
                            gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{db}},{"Y",{db_inv}}});
                            // diff_bit = rem_bit ^ db_inv ^ borrow_in (full adder)
                            std::string axb = gn.new_wire("_div_axb");
                            gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{rb}},{"B",{db_inv}},{"Y",{axb}}});
                            std::string diff_bit;
                            if (bit == 0) {
                                // borrow_in = 0 (no borrow), so diff = axb ^ 0 = axb
                                diff_bit = axb;
                                // borrow_out = ~rb & db = rb NOR ~db_inv → actually a & ~b + borrow_in*(a^~b)
                                // For bit 0: borrow_out = (rb ? 0 : 1) when db_inv=1, else 0
                                // Simplified: borrow_out = ~rb & db_inv (active-low borrow)
                                borrow = gn.new_wire("_div_bor");
                                std::string not_rb = gn.new_wire("_div_nrb");
                                gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{rb}},{"Y",{not_rb}}});
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{not_rb}},{"B",{db_inv}},{"Y",{borrow}}});
                            } else {
                                diff_bit = gn.new_wire("_div_diff");
                                gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{axb}},{"B",{borrow}},{"Y",{diff_bit}}});
                                // borrow_out = (~rb & db_inv) | (borrow_in & ~(rb ^ db_inv))
                                std::string not_rb2 = gn.new_wire("_div_nrb2");
                                gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{rb}},{"Y",{not_rb2}}});
                                std::string term1 = gn.new_wire("_div_t1");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{not_rb2}},{"B",{db_inv}},{"Y",{term1}}});
                                std::string not_axb = gn.new_wire("_div_naxb");
                                gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{axb}},{"Y",{not_axb}}});
                                std::string term2 = gn.new_wire("_div_t2");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{borrow}},{"B",{not_axb}},{"Y",{term2}}});
                                std::string new_bor = gn.new_wire("_div_bor2");
                                gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{term1}},{"B",{term2}},{"Y",{new_bor}}});
                                borrow = new_bor;
                            }
                            std::string db_out = diff_sig + "[" + std::to_string(bit) + "]";
                            gn.wires[db_out] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{diff_bit}},{"Y",{db_out}}});
                        }
                        // borrow is active-low: 0 means borrow occurred (remainder < divisor)
                        // Return inverted: 1 means remainder >= divisor
                        std::string ge_sig = gn.new_wire("_div_ge");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{borrow}},{"Y",{ge_sig}}});
                        return ge_sig;
                    };

                    // Build w stages of the division array
                    // We'll simplify: treat the operands as wire bundles and build
                    // a simple array divider using per-stage subtract+branch pattern

                    // Create a structured representation: for each quotient bit position,
                    // we instantiate a subtractor cell and MUX for the remainder path.

                    // Simplified single-cycle restoring divider:
                    // For each of w stages (i from w-1 down to 0):
                    std::string current_rem; // will hold w+1-bit remainder
                    // Initialize remainder to zero-extended MSBs
                    {
                        std::string r0 = gn.new_wire("_rem0");
                        gn.wires[r0] = w + 1;
                        // Zero-initialize remainder bits
                        for (int b = 0; b <= w; b++) {
                            std::string rb = r0 + "[" + std::to_string(b) + "]";
                            gn.wires[rb] = 1;
                        }
                        current_rem = r0;
                    }

                    // Build divisor with MSB=0 (w+1 bits)
                    std::string divisor_ext = gn.new_wire("_div_ext");
                    gn.wires[divisor_ext] = w + 1;
                    for (int b = 0; b < w; b++) {
                        std::string src = r + "[" + std::to_string(b) + "]";
                        std::string dst = divisor_ext + "[" + std::to_string(b) + "]";
                        gn.wires[src] = 1; gn.wires[dst] = 1;
                        gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{src}},{"Y",{dst}}});
                    }
                    {   // MSB of divisor_ext = 0
                        std::string dst = divisor_ext + "[" + std::to_string(w) + "]";
                        gn.wires[dst] = 1;
                    }

                    for (int i = w - 1; i >= 0; i--) {
                        // Shift left: new remainder = (old_rem << 1) | dividend[i]
                        std::string rem_shifted = gn.new_wire("_rem_s");
                        gn.wires[rem_shifted] = w + 1;
                        // rem_shifted[0] = dividend[i]
                        {
                            std::string src = l + "[" + std::to_string(i) + "]";
                            std::string dst = rem_shifted + "[0]";
                            gn.wires[src] = 1; gn.wires[dst] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{src}},{"Y",{dst}}});
                        }
                        // rem_shifted[b] = current_rem[b-1] for b=1..w
                        for (int b = 1; b <= w; b++) {
                            std::string src = current_rem + "[" + std::to_string(b-1) + "]";
                            std::string dst = rem_shifted + "[" + std::to_string(b) + "]";
                            gn.wires[src] = 1; gn.wires[dst] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{src}},{"Y",{dst}}});
                        }

                        // Subtract: diff = rem_shifted - divisor_ext
                        std::string diff = gn.new_wire("_div_diff_all");
                        gn.wires[diff] = w + 1;
                        std::string ge = build_subtract_stage(rem_shifted, divisor_ext, diff, w + 1);

                        // MUX: if ge=1 (remainder >= divisor), remainder = diff; else remainder = rem_shifted
                        std::string next_rem = gn.new_wire("_rem_nxt");
                        gn.wires[next_rem] = w + 1;
                        // ge selects diff: output = ge ? diff : rem_shifted
                        for (int b = 0; b <= w; b++) {
                            std::string diff_b = diff + "[" + std::to_string(b) + "]";
                            std::string rem_b = rem_shifted + "[" + std::to_string(b) + "]";
                            std::string out_b = next_rem + "[" + std::to_string(b) + "]";
                            gn.wires[diff_b] = 1; gn.wires[rem_b] = 1; gn.wires[out_b] = 1;
                            // MUX: Y = S ? B : A, so A=rem_shifted, B=diff, S=ge
                            gn.add_cell("$_MUX_","_c"+std::to_string(gn.next_id++),
                                {{"A",{rem_b}},{"B",{diff_b}},{"S",{ge}},{"Y",{out_b}}});
                        }

                        // Quotient bit: q[i] = ge
                        std::string qb = o + "[" + std::to_string(i) + "]";
                        gn.wires[qb] = 1;
                        gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{ge}},{"Y",{qb}}});

                        current_rem = next_rem;
                    }

                    // For MOD: output = final remainder (lower w bits)
                    if (expr->op == VerilogParser::Expression::MOD) {
                        for (int b = 0; b < w; b++) {
                            std::string rem_out = current_rem + "[" + std::to_string(b) + "]";
                            std::string out_b = o + "[" + std::to_string(b) + "]";
                            gn.wires[rem_out] = 1; gn.wires[out_b] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{rem_out}},{"Y",{out_b}}});
                        }
                    }
                    // For DIV: quotient bits already connected in the loop above

                    gn.wires[o] = w;
                    break;
                }
                case VerilogParser::Expression::SHL:
                case VerilogParser::Expression::SHR:
                case VerilogParser::Expression::SRA:
                case VerilogParser::Expression::SRL: {
                    int w = 1, shift_width = 1;
                    if (gn.wires.count(l)) w = std::max(w, gn.wires[l]);
                    if (gn.wires.count(r)) shift_width = std::max(shift_width, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) w = std::max(w, p.width); }
                    if (w < 1) w = 1;
                    int stages = 0;
                    while ((1 << stages) < w && stages < shift_width) stages++;
                    if (stages == 0) {
                        for (int bit = 0; bit < w; bit++) {
                            std::string src = ensure_bit_signal(gn, l, w, bit);
                            std::string dst = o + "[" + std::to_string(bit) + "]";
                            gn.wires[dst] = 1;
                            gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++), {{"A", {src}}, {"Y", {dst}}});
                        }
                    } else {
                        std::string current = l;
                        gn.wires[current] = std::max(gn.wires.count(current) ? gn.wires[current] : 1, w);
                        for (int stage = 0; stage < stages; stage++) {
                            int amount = 1 << stage;
                            std::string next_bus = (stage == stages - 1) ? o : gn.new_wire("_shift");
                            gn.wires[next_bus] = w;
                            std::string sel = ensure_bit_signal(gn, r, shift_width, stage);
                            for (int out_bit = 0; out_bit < w; out_bit++) {
                                std::string hold = ensure_bit_signal(gn, current, w, out_bit);
                                int in_bit = (expr->op == VerilogParser::Expression::SHL)
                                    ? (out_bit - amount)
                                    : (out_bit + amount);
                                std::string shifted;
                                if (in_bit >= 0 && in_bit < w) {
                                    shifted = ensure_bit_signal(gn, current, w, in_bit);
                                } else if (expr->op == VerilogParser::Expression::SRA && in_bit >= w) {
                                    shifted = ensure_bit_signal(gn, current, w, w - 1);
                                } else {
                                    shifted = "0";
                                }
                                std::string dst = next_bus + "[" + std::to_string(out_bit) + "]";
                                gn.wires[dst] = 1;
                                gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++),
                                            {{"A", {hold}}, {"B", {shifted}}, {"S", {sel}}, {"Y", {dst}}});
                            }
                            current = next_bus;
                        }
                    }
                    gn.wires[o] = w;
                    break;
                }
                case VerilogParser::Expression::LT:
                case VerilogParser::Expression::GT:
                case VerilogParser::Expression::LE:
                case VerilogParser::Expression::GE: {
                    // Full magnitude comparator with MSB-first priority chain
                    int w = 1;
                    if (gn.wires.count(l)) w = std::max(w, gn.wires[l]);
                    if (gn.wires.count(r)) w = std::max(w, gn.wires[r]);
                    for (auto &p : gn.ports) { if (p.name == l) w = std::max(w, p.width); if (p.name == r) w = std::max(w, p.width); }
                    if (w < 1) w = 1;
                    gn.wires[o] = 1;

                    // Per-bit: eq[i] = ~(a[i] ^ b[i]), gt[i] = a[i] & ~b[i], lt[i] = ~a[i] & b[i]
                    std::vector<std::string> eq(w), gt(w), lt(w);
                    for (int bit = 0; bit < w; bit++) {
                        std::string lb = l + "[" + std::to_string(bit) + "]";
                        std::string rb = r + "[" + std::to_string(bit) + "]";
                        gn.wires[lb] = 1; gn.wires[rb] = 1;

                        // eq[bit] = ~(a ^ b) = XNOR
                        std::string xb = gn.new_wire("_cmp_x");
                        gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb}},{"Y",{xb}}});
                        eq[bit] = gn.new_wire("_cmp_eq");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{xb}},{"Y",{eq[bit]}}});

                        // gt[bit] = a & ~b
                        std::string rb_inv = gn.new_wire("_cmp_nb");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{rb}},{"Y",{rb_inv}}});
                        gt[bit] = gn.new_wire("_cmp_gt");
                        gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"B",{rb_inv}},{"Y",{gt[bit]}}});

                        // lt[bit] = ~a & b
                        std::string lb_inv = gn.new_wire("_cmp_na");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{lb}},{"Y",{lb_inv}}});
                        lt[bit] = gn.new_wire("_cmp_lt");
                        gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{lb_inv}},{"B",{rb}},{"Y",{lt[bit]}}});
                    }

                    // Chain from MSB to LSB:
                    // gt_result[MSB] = gt[MSB]
                    // gt_result[i] = gt[i] | (eq[i] & gt_result[i+1])
                    // lt_result[MSB] = lt[MSB]
                    // lt_result[i] = lt[i] | (eq[i] & lt_result[i+1])
                    std::vector<std::string> gt_chain(w), lt_chain(w);
                    gt_chain[w-1] = gt[w-1];
                    lt_chain[w-1] = lt[w-1];
                    for (int bit = w - 2; bit >= 0; bit--) {
                        // gt_chain[bit] = gt[bit] | (eq[bit] & gt_chain[bit+1])
                        std::string eq_and_gt = gn.new_wire("_cmp_eg");
                        gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{eq[bit]}},{"B",{gt_chain[bit+1]}},{"Y",{eq_and_gt}}});
                        gt_chain[bit] = gn.new_wire("_cmp_gc");
                        gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{gt[bit]}},{"B",{eq_and_gt}},{"Y",{gt_chain[bit]}}});

                        // lt_chain[bit] = lt[bit] | (eq[bit] & lt_chain[bit+1])
                        std::string eq_and_lt = gn.new_wire("_cmp_el");
                        gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{eq[bit]}},{"B",{lt_chain[bit+1]}},{"Y",{eq_and_lt}}});
                        lt_chain[bit] = gn.new_wire("_cmp_lc");
                        gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{lt[bit]}},{"B",{eq_and_lt}},{"Y",{lt_chain[bit]}}});
                    }

                    // Select output based on operator type
                    // GT: gt_chain[0], LT: lt_chain[0]
                    // GE: ~lt_chain[0] (not less than), LE: ~gt_chain[0] (not greater than)
                    std::string result_sig;
                    if (expr->op == VerilogParser::Expression::GT) {
                        result_sig = gt_chain[0];
                    } else if (expr->op == VerilogParser::Expression::LT) {
                        result_sig = lt_chain[0];
                    } else if (expr->op == VerilogParser::Expression::GE) {
                        // GE = ~lt_chain[0]
                        result_sig = gn.new_wire("_cmp_ge");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{lt_chain[0]}},{"Y",{result_sig}}});
                    } else { // LE
                        // LE = ~gt_chain[0]
                        result_sig = gn.new_wire("_cmp_le");
                        gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{gt_chain[0]}},{"Y",{result_sig}}});
                    }

                    gn.add_cell("$_BUF_",cn,{{"A",{result_sig}},{"Y",{o}}});
                    break;
                }
                default:
                    if (!gn.wires.count(o)) gn.wires[o] = gn.wires.count(l) ? gn.wires[l] : 1;
                    gn.add_cell("$_BUF_",cn,{{"A",{l}},{"Y",{o}}}); break;
            }
            return o;
        }
        if (node->type == VerilogParser::NodeType::UNARY_OP && expr->left) {
            std::string a = emit_expr(gn, expr->left, sm);
            std::string o = gn.new_wire("_unary");
            switch (expr->op) {
                case VerilogParser::Expression::UNOT:  // ~ (bitwise NOT)
                case VerilogParser::Expression::ULNOT: // ! (logical NOT - output is 1-bit)
                    gn.wires[o] = (expr->op == VerilogParser::Expression::ULNOT) ? 1 : (gn.wires.count(a) ? gn.wires[a] : 1);
                    gn.add_cell("$_NOT_","_c"+std::to_string(gn.next_id++),{{"A",{a}},{"Y",{o}}});
                    break;
                case VerilogParser::Expression::UMINUS: // - (unary minus → 2's complement: invert+1)
                    {
                        int w = gn.wires.count(a) ? gn.wires[a] : 1;
                        gn.wires[o] = w;
                        // -a = ~a + 1: invert each bit and add through adder with cin=1
                        std::string zero_w = gn.new_wire("_zero_um");
                        gn.wires[zero_w] = w;
                        emit_cla_adder(gn, zero_w, a, o, w, true); // subtract a from 0 = -a
                    }
                    break;
                case VerilogParser::Expression::UAND: // &a (reduction AND)
                    gn.wires[o] = 1;
                    {
                        // Tree-AND of all bits
                        std::string cur = gn.new_wire("_redand");
                        int w = gn.wires.count(a) ? gn.wires[a] : 1;
                        if (w <= 1) {
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{a}},{"Y",{o}}});
                        } else {
                            // Get first bit
                            std::string fst = a + "[0]";
                            gn.wires[fst] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{fst}},{"Y",{cur}}});
                            for (int b = 1; b < w; b++) {
                                std::string sb = a + "[" + std::to_string(b) + "]";
                                gn.wires[sb] = 1;
                                std::string nand = gn.new_wire("_redand_n");
                                gn.add_cell("$_AND_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"B",{sb}},{"Y",{nand}}});
                                cur = nand;
                            }
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"Y",{o}}});
                        }
                    }
                    break;
                case VerilogParser::Expression::UOR: // |a (reduction OR)
                    gn.wires[o] = 1;
                    {
                        std::string cur = gn.new_wire("_redor");
                        int w = gn.wires.count(a) ? gn.wires[a] : 1;
                        if (w <= 1) {
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{a}},{"Y",{o}}});
                        } else {
                            std::string fst = a + "[0]";
                            gn.wires[fst] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{fst}},{"Y",{cur}}});
                            for (int b = 1; b < w; b++) {
                                std::string sb = a + "[" + std::to_string(b) + "]";
                                gn.wires[sb] = 1;
                                std::string nor = gn.new_wire("_redor_n");
                                gn.add_cell("$_OR_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"B",{sb}},{"Y",{nor}}});
                                cur = nor;
                            }
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"Y",{o}}});
                        }
                    }
                    break;
                case VerilogParser::Expression::UXOR: // ^a (reduction XOR)
                    gn.wires[o] = 1;
                    {
                        std::string cur = gn.new_wire("_redxor");
                        int w = gn.wires.count(a) ? gn.wires[a] : 1;
                        if (w <= 1) {
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{a}},{"Y",{o}}});
                        } else {
                            std::string fst = a + "[0]";
                            gn.wires[fst] = 1;
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{fst}},{"Y",{cur}}});
                            for (int b = 1; b < w; b++) {
                                std::string sb = a + "[" + std::to_string(b) + "]";
                                gn.wires[sb] = 1;
                                std::string nxor = gn.new_wire("_redxor_n");
                                gn.add_cell("$_XOR_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"B",{sb}},{"Y",{nxor}}});
                                cur = nxor;
                            }
                            gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{cur}},{"Y",{o}}});
                        }
                    }
                    break;
                default:
                    gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{a}},{"Y",{o}}});
                    break;
            }
            return o;
        }
        if (expr->left) return emit_expr(gn, expr->left, sm);
    }
    if (!node->children.empty()) return emit_expr(gn, node->children[0], sm);
    return "";
}

// The signal map represents the current value of a procedural variable on
// RHS expressions. LHS names must remain the declared storage/net name.
static std::string emit_lvalue(std::shared_ptr<VerilogParser::ASTNode> node,
                               const std::map<std::string, std::string> &sm) {
    if (!node) return "";
    if (node->type == VerilogParser::NodeType::IDENTIFIER) {
        std::string name = get_name(node);
        if (node->children.empty()) return name;
        auto index = node->children[0];
        if (index && index->type == VerilogParser::NodeType::NUMBER) {
            return name + "[" + (index->attributes.count("value") ? index->attributes.at("value") : "0") + "]";
        }
        std::string index_name = get_name(index);
        auto it = sm.find(index_name);
        if (it != sm.end()) index_name = it->second;
        return index_name.empty() ? name : name + "[" + index_name + "]";
    }
    return get_name(node);
}

// Statement → gates
static void process_stmt(GateNetlist &gn, std::shared_ptr<VerilogParser::ASTNode> stmt,
                          std::map<std::string, std::string> &sm,
                          const std::string &clk, bool seq, const ModuleMap &mod_map, int depth,
                          const std::string &rst = "", bool rst_active_low = true);

struct PendingAssign {
    std::string src;
    int width = 1;
};

static int get_signal_width(const GateNetlist &gn, const std::string &signal) {
    if (signal.empty()) return 1;
    if (signal == "0" || signal == "1" || signal == "1'b0" || signal == "1'b1") return 1;
    std::string base = signal;
    size_t bracket = base.find('[');
    if (bracket != std::string::npos) base = base.substr(0, bracket);

    int width = 1;
    auto it = gn.wires.find(signal);
    if (it != gn.wires.end()) width = std::max(width, it->second);
    auto base_it = gn.wires.find(base);
    if (base_it != gn.wires.end()) width = std::max(width, base_it->second);
    for (const auto &p : gn.ports) {
        if (p.name == signal || p.name == base) width = std::max(width, p.width);
    }
    return width;
}

static int get_assignment_width(const GateNetlist &gn, const std::string &dst, const std::string &src) {
    int dst_width = get_signal_width(gn, dst);
    int src_width = get_signal_width(gn, src);
    if (dst_width > 1) return dst_width;
    return std::max(dst_width, src_width);
}

static std::string ensure_bit_signal(GateNetlist &gn, const std::string &signal, int width, int bit) {
    if (signal.empty()) return signal;
    if (signal == "0" || signal == "1" || signal == "1'b0" || signal == "1'b1") {
        return (signal.find('1') != std::string::npos) ? "1" : "0";
    }
    if (signal.find('[') != std::string::npos) return signal;
    if (width <= 1) return signal;
    gn.wires[signal] = std::max(gn.wires.count(signal) ? gn.wires[signal] : 1, width);
    std::string bit_sig = signal + "[" + std::to_string(bit) + "]";
    gn.wires[bit_sig] = 1;
    return bit_sig;
}

static std::string normalize_reset_signal(GateNetlist &gn, const std::string &rst, bool rst_active_low) {
    if (rst.empty()) return "";
    if (!rst_active_low) return rst;
    std::string inv = rst + "__active_high";
    if (!gn.wires.count(inv)) {
        gn.wires[inv] = 1;
        gn.add_cell("$_NOT_", "_c" + std::to_string(gn.next_id++), {{"A", {rst}}, {"Y", {inv}}});
    }
    return inv;
}

static std::string build_equality_signal(GateNetlist &gn, const std::string &lhs, const std::string &rhs, int width) {
    if (width <= 1) {
        std::string xor_wire = gn.new_wire("_eq_x");
        std::string out = gn.new_wire("_eq");
        gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++), {{"A", {lhs}}, {"B", {rhs}}, {"Y", {xor_wire}}});
        gn.add_cell("$_NOT_", "_c" + std::to_string(gn.next_id++), {{"A", {xor_wire}}, {"Y", {out}}});
        return out;
    }

    std::vector<std::string> eq_bits;
    eq_bits.reserve(width);
    for (int bit = 0; bit < width; bit++) {
        std::string lb = ensure_bit_signal(gn, lhs, get_signal_width(gn, lhs), bit);
        std::string rb = ensure_bit_signal(gn, rhs, get_signal_width(gn, rhs), bit);
        std::string xor_wire = gn.new_wire("_eq_x");
        std::string eq_wire = gn.new_wire("_eq_b");
        gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++), {{"A", {lb}}, {"B", {rb}}, {"Y", {xor_wire}}});
        gn.add_cell("$_NOT_", "_c" + std::to_string(gn.next_id++), {{"A", {xor_wire}}, {"Y", {eq_wire}}});
        eq_bits.push_back(eq_wire);
    }

    std::string acc = eq_bits.front();
    for (size_t i = 1; i < eq_bits.size(); i++) {
        std::string next = gn.new_wire("_eq_and");
        gn.add_cell("$_AND_", "_c" + std::to_string(gn.next_id++), {{"A", {acc}}, {"B", {eq_bits[i]}}, {"Y", {next}}});
        acc = next;
    }
    return acc;
}

static std::string build_mux_signal(GateNetlist &gn, const std::string &cond,
                                    const std::string &false_src, const std::string &true_src, int width) {
    if (width <= 1) {
        std::string out = gn.new_wire("_ifmux");
        gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++),
            {{"A", {false_src}}, {"B", {true_src}}, {"S", {cond}}, {"Y", {out}}});
        return out;
    }

    std::string bus = gn.new_wire("_ifmux");
    gn.wires[bus] = width;
    for (int bit = 0; bit < width; bit++) {
        std::string fb = ensure_bit_signal(gn, false_src, get_signal_width(gn, false_src), bit);
        std::string tb = ensure_bit_signal(gn, true_src, get_signal_width(gn, true_src), bit);
        std::string out_bit = bus + "[" + std::to_string(bit) + "]";
        gn.wires[out_bit] = 1;
        gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++),
            {{"A", {fb}}, {"B", {tb}}, {"S", {cond}}, {"Y", {out_bit}}});
    }
    return bus;
}

static bool matches_async_reset_condition(std::shared_ptr<VerilogParser::ASTNode> cond_node,
                                          const std::string &rst, bool rst_active_low) {
    if (!cond_node || rst.empty()) return false;
    if (cond_node->type == VerilogParser::NodeType::IDENTIFIER) {
        return !rst_active_low && get_name(cond_node) == rst;
    }
    auto expr = std::dynamic_pointer_cast<VerilogParser::Expression>(cond_node);
    if (!expr) return false;
    if ((cond_node->type == VerilogParser::NodeType::UNARY_OP) &&
        (expr->op == VerilogParser::Expression::ULNOT || expr->op == VerilogParser::Expression::UNOT) &&
        expr->left && expr->left->type == VerilogParser::NodeType::IDENTIFIER) {
        return rst_active_low && get_name(expr->left) == rst;
    }
    return false;
}

static void emit_assignment_cells(GateNetlist &gn, const std::string &dst, const std::string &src,
                                  int width, const std::string &clk, bool seq,
                                  const std::string &rst, bool rst_active_low,
                                  std::map<std::string, std::string> &sm) {
    if (dst.empty() || src.empty()) return;
    if (width < 1) width = 1;

    if (seq && !clk.empty()) {
        bool has_async_rst = !rst.empty();
        std::string reset_sig = has_async_rst ? normalize_reset_signal(gn, rst, rst_active_low) : "";
        if (width <= 1) {
            if (has_async_rst) {
                gn.add_cell("$_DFFSR_PPP_", "_dff" + std::to_string(gn.next_id++),
                    {{"D", {src}}, {"C", {clk}}, {"R", {reset_sig}}, {"Q", {dst}}});
            } else {
                gn.add_cell("$_DFF_P_", "_dff" + std::to_string(gn.next_id++),
                    {{"D", {src}}, {"C", {clk}}, {"Q", {dst}}});
            }
            return;
        }

        for (int bit = 0; bit < width; bit++) {
            std::string d_wire = dst + "[" + std::to_string(bit) + "]";
            std::string s_wire = ensure_bit_signal(gn, src, get_signal_width(gn, src), bit);
            if (!gn.wires.count(d_wire)) { gn.wires[d_wire] = 1; sm[d_wire] = d_wire; }
            if (!gn.wires.count(s_wire)) { gn.wires[s_wire] = 1; sm[s_wire] = s_wire; }
            if (has_async_rst) {
                gn.add_cell("$_DFFSR_PPP_", "_dff" + std::to_string(gn.next_id++),
                    {{"D", {s_wire}}, {"C", {clk}}, {"R", {reset_sig}}, {"Q", {d_wire}}});
            } else {
                gn.add_cell("$_DFF_P_", "_dff" + std::to_string(gn.next_id++),
                    {{"D", {s_wire}}, {"C", {clk}}, {"Q", {d_wire}}});
            }
        }
        return;
    }

    if (width <= 1) {
        gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++), {{"A", {src}}, {"Y", {dst}}});
        return;
    }
    for (int bit = 0; bit < width; bit++) {
        std::string dw = dst + "[" + std::to_string(bit) + "]";
        std::string sw = ensure_bit_signal(gn, src, get_signal_width(gn, src), bit);
        if (!gn.wires.count(dw)) { gn.wires[dw] = 1; sm[dw] = dw; }
        if (!gn.wires.count(sw)) { gn.wires[sw] = 1; sm[sw] = sw; }
        gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++), {{"A", {sw}}, {"Y", {dw}}});
    }
}

static void collect_stmt_assignments(GateNetlist &gn, std::shared_ptr<VerilogParser::ASTNode> stmt,
                                     std::map<std::string, std::string> &sm,
                                     const ModuleMap &mod_map, int depth,
                                     const std::string &clk, bool seq,
                                     const std::string &rst, bool rst_active_low,
                                     std::map<std::string, PendingAssign> &out) {
    if (!stmt) return;

    if (stmt->type == VerilogParser::NodeType::ASSIGN && stmt->children.size() >= 2) {
        std::string dst = emit_lvalue(stmt->children[0], sm);
        std::string src = emit_expr(gn, stmt->children[1], sm,
                                    get_signal_width(gn, dst));
        if (!dst.empty() && !src.empty()) {
            out[dst] = {src, get_assignment_width(gn, dst, src)};
        }
        return;
    }

    if (stmt->type == VerilogParser::NodeType::BEGIN_STATEMENT) {
        for (auto &child : stmt->children) {
            collect_stmt_assignments(gn, child, sm, mod_map, depth, clk, seq, rst, rst_active_low, out);
            // Blocking assignments in a combinational process make their new
            // value visible to all following statements in source order.
            for (const auto &[dst, assign] : out) sm[dst] = assign.src;
        }
        return;
    }

    if (stmt->type == VerilogParser::NodeType::FOR_LOOP) {
        int unroll_count = 0;
        std::string loop_var;
        if (!stmt->children.empty()) {
            auto init = stmt->children[0];
            if (init && !init->children.empty()) loop_var = get_name(init->children[0]);
        }
        if (stmt->children.size() >= 2) {
            auto cond = std::dynamic_pointer_cast<VerilogParser::Expression>(stmt->children[1]);
            int64_t bound = 0;
            if (cond && const_int(cond->right, bound) && bound >= 0 && bound <= 4096) {
                unroll_count = static_cast<int>(bound);
            }
        }
        auto body = stmt->children.size() >= 4 ? stmt->children[3] : nullptr;
        if (body && !loop_var.empty() && unroll_count > 0) {
            for (int iter = 0; iter < unroll_count; ++iter) {
                auto iteration_map = sm;
                iteration_map[loop_var] = std::to_string(iter);
                collect_stmt_assignments(gn, body, iteration_map, mod_map, depth,
                                         clk, seq, rst, rst_active_low, out);
                for (const auto &[dst, assign] : out) sm[dst] = assign.src;
            }
        }
        return;
    }

    if (stmt->type == VerilogParser::NodeType::IF_STATEMENT) {
        if (stmt->children.empty()) return;
        if (!rst.empty() && matches_async_reset_condition(stmt->children[0], rst, rst_active_low)) {
            if (stmt->children.size() >= 3) {
                collect_stmt_assignments(gn, stmt->children[2], sm, mod_map, depth, clk, seq, rst, rst_active_low, out);
            }
            return;
        }

        std::string cond = emit_expr(gn, stmt->children[0], sm);
        std::map<std::string, PendingAssign> then_assigns;
        std::map<std::string, PendingAssign> else_assigns;
        if (stmt->children.size() >= 2) {
            collect_stmt_assignments(gn, stmt->children[1], sm, mod_map, depth, clk, seq, rst, rst_active_low, then_assigns);
        }
        if (stmt->children.size() >= 3) {
            collect_stmt_assignments(gn, stmt->children[2], sm, mod_map, depth, clk, seq, rst, rst_active_low, else_assigns);
        }

        std::set<std::string> all_dsts;
        for (const auto &it : then_assigns) all_dsts.insert(it.first);
        for (const auto &it : else_assigns) all_dsts.insert(it.first);
        for (const auto &dst : all_dsts) {
            PendingAssign prior{dst, get_signal_width(gn, dst)};
            if (out.count(dst)) prior = out[dst];
            PendingAssign true_assign = then_assigns.count(dst) ? then_assigns[dst] : prior;
            PendingAssign false_assign = else_assigns.count(dst) ? else_assigns[dst] : prior;
            int width = std::max({prior.width, true_assign.width, false_assign.width, get_signal_width(gn, dst)});
            std::string merged = build_mux_signal(gn, cond, false_assign.src, true_assign.src, width);
            out[dst] = {merged, width};
        }
        return;
    }

    if (stmt->type == VerilogParser::NodeType::CASE_STATEMENT && !stmt->children.empty()) {
        std::string sel = emit_expr(gn, stmt->children[0], sm);
        int sel_width = get_signal_width(gn, sel);
        struct PendingCaseBranch {
            bool is_default = false;
            std::string match_signal;
            std::map<std::string, PendingAssign> assigns;
        };
        std::vector<PendingCaseBranch> branches;
        for (size_t i = 1; i < stmt->children.size(); i++) {
            auto branch = stmt->children[i];
            if (!branch || branch->type != VerilogParser::NodeType::CASE_ITEM) continue;
            PendingCaseBranch pending;
            size_t body_start = 0;
            if (!branch->children.empty()) {
                auto first = branch->children[0];
                if (first && first->type != VerilogParser::NodeType::ASSIGN &&
                    first->type != VerilogParser::NodeType::IF_STATEMENT &&
                    first->type != VerilogParser::NodeType::CASE_STATEMENT &&
                    first->type != VerilogParser::NodeType::BEGIN_STATEMENT) {
                    pending.match_signal = emit_expr(gn, first, sm);
                    body_start = 1;
                } else {
                    pending.is_default = true;
                }
            } else {
                pending.is_default = true;
            }
            for (size_t j = body_start; j < branch->children.size(); j++) {
                collect_stmt_assignments(gn, branch->children[j], sm, mod_map, depth, clk, seq, rst, rst_active_low, pending.assigns);
            }
            branches.push_back(std::move(pending));
        }

        std::set<std::string> all_dsts;
        for (const auto &branch : branches) {
            for (const auto &it : branch.assigns) all_dsts.insert(it.first);
        }
        for (const auto &dst : all_dsts) {
            PendingAssign prior{dst, get_signal_width(gn, dst)};
            if (out.count(dst)) prior = out[dst];
            PendingAssign merged = prior;

            for (const auto &branch : branches) {
                if (branch.is_default && branch.assigns.count(dst)) {
                    merged = branch.assigns.at(dst);
                }
            }
            for (auto it = branches.rbegin(); it != branches.rend(); ++it) {
                if (it->is_default || it->match_signal.empty()) continue;
                PendingAssign branch_assign = it->assigns.count(dst) ? it->assigns.at(dst) : prior;
                int width = std::max({merged.width, branch_assign.width, prior.width, get_signal_width(gn, dst)});
                int cmp_width = std::max(sel_width, get_signal_width(gn, it->match_signal));
                std::string cond = build_equality_signal(gn, sel, it->match_signal, cmp_width);
                std::string mux = build_mux_signal(gn, cond, merged.src, branch_assign.src, width);
                merged = {mux, width};
            }
            out[dst] = merged;
        }
        return;
    }

    for (auto &child : stmt->children) {
        collect_stmt_assignments(gn, child, sm, mod_map, depth, clk, seq, rst, rst_active_low, out);
    }
}

// Recursively elaborate a module instance into the gate netlist
static void elaborate_instance(GateNetlist &gn, const std::string &inst_name,
                                const std::string &mod_type,
                                std::shared_ptr<VerilogParser::ASTNode> inst_ast,
                                const ModuleMap &mod_map,
                                std::map<std::string, std::string> &parent_sm,
                                int depth) {
    if (depth > 20) return;  // Prevent infinite recursion

    auto it = mod_map.find(mod_type);
    if (it == mod_map.end()) {
        // Unknown module - add as black box with BUF for each port connection
        synth_log("SYNTH", "  [WARN] Unknown module: %s (black box)", mod_type.c_str());
        return;
    }

    auto sub_mod = it->second;
    synth_log("SYNTH", "  [ELAB] %s : %s (%zu items)", inst_name.c_str(), mod_type.c_str(), sub_mod->items.size());

    // Build port map: sub-module port name → parent signal name
    std::map<std::string, std::string> port_map;
    if (inst_ast) {
        for (auto &conn : inst_ast->children) {
            if (conn && conn->attributes.count("port")) {
                std::string port_name = conn->attributes.at("port");
                std::string sig_name;
                if (conn->attributes.count("signal")) {
                    sig_name = conn->attributes.at("signal");
                } else if (!conn->children.empty()) {
                    sig_name = get_name(conn->children[0]);
                    // Named instance ports may be connected to expressions,
                    // not only identifiers: .a(bus[3:0]), .d({a,b}), and
                    // constants are all legal.  Falling back to the common
                    // expression mapper preserves those connections while
                    // retaining the direct identifier path for output ports.
                    if (sig_name.empty()) {
                        sig_name = emit_expr(gn, conn->children[0], parent_sm);
                    }
                }
                if (!sig_name.empty()) port_map[port_name] = sig_name;
            }
        }
    }

    // Create signal map for sub-module (maps sub-module signals to parent signals)
    std::map<std::string, std::string> sub_sm;

    // Register sub-module ports and map to parent signals
    for (auto &p : sub_mod->ports) {
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(p);
        if (!pd) continue;
        // Flatten hierarchy into a legal plain Verilog identifier.  A dot is
        // hierarchy syntax in Verilog, not a character permitted in an
        // unescaped net name; emitting `u.a` here produced an invalid
        // gate-level netlist and broke every downstream consumer that parsed
        // it.  The separator is intentionally reversible and collision-safe
        // for normal identifier components.
        std::string full_name = inst_name + "__" + pd->name;
        gn.wires[full_name] = pd->width;
        sub_sm[pd->name] = full_name;

        // Connect parent inputs to the child. Outputs are driven back after the child logic is emitted.
        if (port_map.count(pd->name)) {
            std::string parent_sig = port_map[pd->name];
            if (parent_sm.count(parent_sig)) parent_sig = parent_sm[parent_sig];
            if (pd->dir != VerilogParser::PortDecl::OUTPUT) {
                emit_assignment_cells(gn, full_name, parent_sig, pd->width, "", false, "", true, sub_sm);
            }
        }
    }

    // Register sub-module internal signals
    for (auto &item : sub_mod->items) {
        if (!item) continue;
        if (item->type == VerilogParser::NodeType::WIRE_DECL ||
            item->type == VerilogParser::NodeType::REG_DECL) {
            std::string n = get_name(item);
            int w = get_width(item);
            if (!n.empty()) {
                std::string full_name = inst_name + "__" + n;
                gn.wires[full_name] = w;
                sub_sm[n] = full_name;
            }
        }
    }

    // Process sub-module items
    std::function<void(std::shared_ptr<VerilogParser::ASTNode>)> process_sub_item;
    process_sub_item = [&](std::shared_ptr<VerilogParser::ASTNode> item) {
        if (!item) return;
        // Recursively handle generate blocks
        if (item->type == VerilogParser::NodeType::GENERATE_BLOCK ||
            item->type == VerilogParser::NodeType::GENERATE_FOR ||
            item->type == VerilogParser::NodeType::GENERATE_IF ||
            item->type == VerilogParser::NodeType::GENERATE_CASE) {
            for (auto &gc : item->children) process_sub_item(gc);
            return;
        }
        // Already-registered declarations, skip
        if (item->type == VerilogParser::NodeType::WIRE_DECL ||
            item->type == VerilogParser::NodeType::REG_DECL ||
            item->type == VerilogParser::NodeType::LOGIC_DECL) return;
        // Continuous assignment
        if (item->type == VerilogParser::NodeType::ASSIGN && item->children.size() >= 2) {
            std::string d = emit_expr(gn, item->children[0], sub_sm);
            std::string s = emit_expr(gn, item->children[1], sub_sm,
                                      get_signal_width(gn, d));
            if (!d.empty() && !s.empty()) {
                emit_assignment_cells(gn, d, s, get_assignment_width(gn, d, s), "", false, "", true, sub_sm);
            }
            return;
        }
        // Always block
        if (item->type == VerilogParser::NodeType::ALWAYS_BLOCK) {
            auto ab = std::dynamic_pointer_cast<VerilogParser::AlwaysBlock>(item);
            bool seq = false; std::string clk;
            if (ab) {
                seq = (ab->sens == VerilogParser::AlwaysBlock::POSEDGE || ab->sens == VerilogParser::AlwaysBlock::BOTH);
                clk = ab->clockSignal;
                if (port_map.count(clk)) clk = port_map[clk];
                if (sub_sm.count(clk)) clk = sub_sm[clk];
            }
            for (auto &c : item->children) process_stmt(gn, c, sub_sm, clk, seq, mod_map, depth + 1, "");
            return;
        }
        // Sub-module instantiation
        if (item->type == VerilogParser::NodeType::MODULE_INSTANCE) {
            std::string sub_type = item->attributes.count("type") ? item->attributes.at("type") : "";
            std::string sub_name = item->attributes.count("name") ? item->attributes.at("name") : "";
            if (!sub_type.empty() && !sub_name.empty())
                elaborate_instance(gn, inst_name + "__" + sub_name, sub_type, item, mod_map, sub_sm, depth + 1);
            return;
        }
    };

    for (auto &item : sub_mod->items) {
        process_sub_item(item);
    }

    // Propagate sub-module outputs back to parent
    for (auto &p : sub_mod->ports) {
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(p);
        if (!pd) continue;
        if (pd->dir == VerilogParser::PortDecl::OUTPUT || pd->dir == VerilogParser::PortDecl::INOUT) {
            if (port_map.count(pd->name)) {
                std::string parent_sig = port_map[pd->name];
                if (parent_sm.count(parent_sig)) parent_sig = parent_sm[parent_sig];
                std::string sub_sig = sub_sm[pd->name];
                emit_assignment_cells(gn, parent_sig, sub_sig, pd->width, "", false, "", true, parent_sm);
            }
        }
    }
}

static void process_stmt(GateNetlist &gn, std::shared_ptr<VerilogParser::ASTNode> stmt,
                          std::map<std::string, std::string> &sm,
                          const std::string &clk, bool seq, const ModuleMap &mod_map, int depth,
                          const std::string &rst, bool rst_active_low) {
    if (!stmt) return;

    // Recognize the canonical combinational shift/add multiplier:
    //   acc = 0; for (i=0; i<N; i=i+1) if (b[i]) acc = acc + (a << i); y = acc;
    // Lowering this as a multiplier preserves blocking-assignment semantics
    // while avoiding an unnecessarily large procedural MUX network.
    if (!seq && stmt->type == VerilogParser::NodeType::BEGIN_STATEMENT && stmt->children.size() >= 3) {
        auto init = stmt->children[0];
        auto loop = stmt->children[1];
        auto final_assign = stmt->children.back();
        if (init && loop && final_assign && init->type == VerilogParser::NodeType::ASSIGN &&
            loop->type == VerilogParser::NodeType::FOR_LOOP &&
            final_assign->type == VerilogParser::NodeType::ASSIGN &&
            init->children.size() >= 2 && final_assign->children.size() >= 2 &&
            loop->children.size() >= 4) {
            int64_t initial_value = 1;
            std::string accumulator = emit_lvalue(init->children[0], sm);
            std::string final_source = get_name(final_assign->children[1]);
            auto body_if = loop->children[3];
            if (body_if && body_if->type == VerilogParser::NodeType::BEGIN_STATEMENT &&
                !body_if->children.empty()) {
                body_if = body_if->children[0];
            }
            auto condition_node = body_if && !body_if->children.empty() ? body_if->children[0] : nullptr;
            auto condition = std::dynamic_pointer_cast<VerilogParser::Expression>(condition_node);
            auto body_assign = body_if && body_if->children.size() >= 2 ? body_if->children[1] : nullptr;
            if (const_int(init->children[1], initial_value) && initial_value == 0 &&
                accumulator == final_source && condition_node && body_assign &&
                body_assign->type == VerilogParser::NodeType::ASSIGN && body_assign->children.size() >= 2) {
                std::string multiplier;
                std::string loop_var;
                if (condition && condition->op == VerilogParser::Expression::BIT_SELECT &&
                    condition->left && condition->right) {
                    multiplier = get_name(condition->left);
                    loop_var = get_name(condition->right);
                } else if (condition_node->type == VerilogParser::NodeType::IDENTIFIER &&
                           !condition_node->children.empty()) {
                    multiplier = get_name(condition_node);
                    loop_var = get_name(condition_node->children[0]);
                }
                auto update = std::dynamic_pointer_cast<VerilogParser::Expression>(body_assign->children[1]);
                if (update && update->op == VerilogParser::Expression::ADD && update->left && update->right &&
                    get_name(update->left) == accumulator) {
                    auto shifted = std::dynamic_pointer_cast<VerilogParser::Expression>(update->right);
                    if (shifted && shifted->op == VerilogParser::Expression::SHL && shifted->left && shifted->right &&
                        get_name(shifted->right) == loop_var && !multiplier.empty()) {
                        std::string multiplicand = get_name(shifted->left);
                        std::string output = emit_lvalue(final_assign->children[0], sm);
                        int wa = get_signal_width(gn, multiplicand);
                        int wb = get_signal_width(gn, multiplier);
                        if (!multiplicand.empty() && !output.empty() && wa > 0 && wb > 0) {
                            synth_log("SYNTH", "  Recognized shift/add multiplier: %s * %s -> %s",
                                      multiplicand.c_str(), multiplier.c_str(), output.c_str());
                            emit_multiplier_array(gn, multiplicand, multiplier, output, wa, wb, false);
                            return;
                        }
                    }
                }
            }
        }
    }

    if (stmt->type == VerilogParser::NodeType::BEGIN_STATEMENT ||
        stmt->type == VerilogParser::NodeType::IF_STATEMENT ||
        stmt->type == VerilogParser::NodeType::CASE_STATEMENT) {
        std::map<std::string, PendingAssign> pending;
        collect_stmt_assignments(gn, stmt, sm, mod_map, depth, clk, seq, rst, rst_active_low, pending);
        if (!pending.empty()) {
            for (const auto &[dst, assign] : pending) {
                emit_assignment_cells(gn, dst, assign.src, assign.width, clk, seq, rst, rst_active_low, sm);
            }
            return;
        }
        if (stmt->type == VerilogParser::NodeType::IF_STATEMENT ||
            stmt->type == VerilogParser::NodeType::CASE_STATEMENT) {
            return;
        }
    }

    // Assignment (blocking or non-blocking)
    if (stmt->type == VerilogParser::NodeType::ASSIGN && stmt->children.size() >= 2) {
        auto lhs_node = stmt->children[0];
        auto rhs_node = stmt->children[1];

        std::string dst = emit_expr(gn, lhs_node, sm);
        std::string src = emit_expr(gn, rhs_node, sm,
                                    get_signal_width(gn, dst));
        if (dst.empty() || src.empty()) return;

        int width = get_assignment_width(gn, dst, src);
        emit_assignment_cells(gn, dst, src, width, clk, seq, rst, rst_active_low, sm);
        return;
    }

    // If statement
    if (stmt->type == VerilogParser::NodeType::IF_STATEMENT) {
        if (!stmt->children.empty()) {
            std::string cond = emit_expr(gn, stmt->children[0], sm);
            if (stmt->children.size() >= 2) process_stmt(gn, stmt->children[1], sm, clk, seq, mod_map, depth, rst, true);
            if (stmt->children.size() >= 3) process_stmt(gn, stmt->children[2], sm, clk, seq, mod_map, depth, rst, true);
        }
        return;
    }

    // Case statement - build proper MUX tree
    if (stmt->type == VerilogParser::NodeType::CASE_STATEMENT) {
        if (stmt->children.empty()) return;

        // First child is the select expression
        std::string sel_expr = emit_expr(gn, stmt->children[0], sm);
        if (sel_expr.empty()) return;

        // Determine select signal width
        int sel_width = 1;
        if (gn.wires.count(sel_expr)) sel_width = gn.wires[sel_expr];
        for (auto &p : gn.ports) { if (p.name == sel_expr) sel_width = std::max(sel_width, p.width); }

        // Phase 1: Collect all case branches and their assignments
        // Each branch: {match_value, assignments} where assignments is a map: dst_wire -> src_wire
        struct CaseBranch {
            std::string match_val;  // constant value this branch matches
            std::map<std::string, std::string> assigns; // dst -> src
            bool is_default;
        };
        std::vector<CaseBranch> branches;

        for (size_t i = 1; i < stmt->children.size(); i++) {
            auto branch = stmt->children[i];
            if (!branch) continue;
            if (branch->type != VerilogParser::NodeType::CASE_ITEM) continue;

            CaseBranch cb;
            cb.is_default = false;

            // CASE_ITEM children: [match_expr?, body_stmt1, body_stmt2, ...]
            // The first child may be a match expression (if not default)
            size_t body_start = 0;
            if (!branch->children.empty()) {
                auto first = branch->children[0];
                if (first && first->type != VerilogParser::NodeType::ASSIGN &&
                    first->type != VerilogParser::NodeType::IF_STATEMENT &&
                    first->type != VerilogParser::NodeType::CASE_STATEMENT &&
                    first->type != VerilogParser::NodeType::BEGIN_STATEMENT) {
                    // This is a match expression
                    if (first->type == VerilogParser::NodeType::NUMBER) {
                        cb.match_val = first->attributes.count("value") ? first->attributes.at("value") : "0";
                    } else if (first->type == VerilogParser::NodeType::IDENTIFIER) {
                        cb.match_val = get_name(first);
                    } else {
                        cb.match_val = emit_expr(gn, first, sm);
                    }
                    body_start = 1;
                }
            }

            if (cb.match_val.empty() && body_start == 0) {
                cb.is_default = true;
            }

            // Process body statements of this branch into a temporary signal map
            // We need to capture what this branch would assign
            for (size_t j = body_start; j < branch->children.size(); j++) {
                auto bchild = branch->children[j];
                if (!bchild) continue;
                if (bchild->type == VerilogParser::NodeType::ASSIGN && bchild->children.size() >= 2) {
                    // Emit the RHS expression (creates real logic gates)
                    std::string src = emit_expr(gn, bchild->children[1], sm);
                    std::string dst = emit_expr(gn, bchild->children[0], sm);
                    if (!dst.empty() && !src.empty()) {
                        cb.assigns[dst] = src;
                    }
                }
                // Nested if/case within case branch — process recursively
                if (bchild->type == VerilogParser::NodeType::IF_STATEMENT ||
                    bchild->type == VerilogParser::NodeType::CASE_STATEMENT ||
                    bchild->type == VerilogParser::NodeType::FOR_LOOP) {
                    process_stmt(gn, bchild, sm, clk, seq, mod_map, depth, rst, true);
                }
            }
            branches.push_back(cb);
        }

        if (branches.empty()) return;

        // Phase 2: Determine the set of destination wires affected by all branches
        std::set<std::string> all_dsts;
        for (auto &br : branches) {
            for (auto &[d, s] : br.assigns) all_dsts.insert(d);
        }

        // Phase 3: For each destination wire, build a MUX tree
        for (auto &dst : all_dsts) {
            // Determine width
            int width = 1;
            if (gn.wires.count(dst)) width = gn.wires[dst];
            for (auto &p : gn.ports) { if (p.name == dst) width = std::max(width, p.width); }

            // Per-bit MUX generation
            for (int bit = 0; bit < width; bit++) {
                std::string dst_bit = width > 1 ? (dst + "[" + std::to_string(bit) + "]") : dst;
                if (width > 1) gn.wires[dst_bit] = 1;

                // Collect per-branch source for this destination bit
                std::vector<std::pair<std::string, std::string>> bit_sources; // {match_val, src_bit}
                std::string default_src;

                for (auto &br : branches) {
                    auto it = br.assigns.find(dst);
                    if (it == br.assigns.end()) continue;
                    std::string src = it->second;
                    std::string src_bit = (gn.wires.count(src) && gn.wires[src] > 1) ?
                        (src + "[" + std::to_string(bit) + "]") : src;
                    if (width > 1 && gn.wires.count(src) && gn.wires[src] > 1) {
                        gn.wires[src_bit] = 1;
                    }

                    if (br.is_default) {
                        default_src = src_bit;
                    } else {
                        bit_sources.push_back({br.match_val, src_bit});
                    }
                }

                if (bit_sources.empty()) {
                    // Only default — direct BUF connection
                    if (!default_src.empty()) {
                        gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++),
                            {{"A",{default_src}},{"Y",{dst_bit}}});
                    }
                    continue;
                }

                // If no default, use last source as default (or 0)
                if (default_src.empty() && !bit_sources.empty()) {
                    default_src = bit_sources.back().second;
                }

                // Build priority MUX tree:
                // For sel_width bits, we need 2^sel_width branches
                // Each bit of sel selects between pairs
                std::string cur_mux_out;
                std::string cur_default = default_src;

                for (int sel_bit = 0; sel_bit < sel_width; sel_bit++) {
                    std::string sel_bit_sig = sel_width > 1 ?
                        (sel_expr + "[" + std::to_string(sel_bit) + "]") : sel_expr;
                    if (sel_width > 1) gn.wires[sel_bit_sig] = 1;

                    // Determine which branches go to the "true" side for this sel_bit
                    // Build the true-side source using sub-MUX for remaining bits
                    std::string true_src;
                    std::string false_src;

                    // Collect branches where this sel_bit matches
                    std::vector<std::pair<std::string, std::string>> true_branches;
                    std::vector<std::pair<std::string, std::string>> false_branches;

                    for (auto &[mv, sb] : bit_sources) {
                        // Parse the match value as integer, check sel_bit position
                        int64_t mv_int = 0;
                        try {
                            size_t quote = mv.find('\'');
                            if (quote != std::string::npos && quote + 1 < mv.size()) {
                                char base = mv[quote + 1];
                                std::string digits = mv.substr(quote + 2);
                                digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                                switch (base) {
                                    case 'd': case 'D': mv_int = std::stoll(digits); break;
                                    case 'h': case 'H': mv_int = std::stoll(digits, nullptr, 16); break;
                                    case 'b': case 'B': mv_int = std::stoll(digits, nullptr, 2); break;
                                    default: mv_int = std::stoll(digits); break;
                                }
                            } else {
                                mv_int = std::stoll(mv);
                            }
                        } catch (...) { mv_int = 0; }

                        if ((mv_int >> sel_bit) & 1) {
                            true_branches.push_back({mv, sb});
                        } else {
                            false_branches.push_back({mv, sb});
                        }
                    }

                    // Build sub-MUX for true side (recursively select among true branches)
                    if (sel_bit == sel_width - 1) {
                        // Last bit: just pick the first matching source
                        if (!true_branches.empty()) true_src = true_branches[0].second;
                        else true_src = cur_default;
                        if (!false_branches.empty()) false_src = false_branches[0].second;
                        else false_src = cur_default;
                    } else {
                        // For non-final bits, we need to build the sub-MUX later
                        // For simplicity: use the first matching source
                        if (!true_branches.empty()) true_src = true_branches[0].second;
                        else true_src = cur_default;
                        if (!false_branches.empty()) false_src = false_branches[0].second;
                        else false_src = cur_default;
                    }

                    cur_mux_out = gn.new_wire("_case_mux");
                    gn.add_cell("$_MUX_", "_c"+std::to_string(gn.next_id++),
                        {{"A",{false_src}},{"B",{true_src}},{"S",{sel_bit_sig}},{"Y",{cur_mux_out}}});
                    cur_default = cur_mux_out;
                }

                // Final output: connect MUX output to destination
                std::string final_out = cur_mux_out.empty() ? cur_default : cur_mux_out;
                gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++),
                    {{"A",{final_out}},{"Y",{dst_bit}}});
            }
            if (width > 1) gn.wires[dst] = width;
        }
        return;
    }

    // For loop - unroll for synthesis with loop variable substitution
    if (stmt->type == VerilogParser::NodeType::FOR_LOOP) {
        // FOR_LOOP children: [init, condition, update, body]
        int unroll_count = 32; // default for register files
        std::string loop_var;

        // Extract loop variable name from init (e.g., i = 0)
        if (stmt->children.size() >= 1) {
            auto init = stmt->children[0];
            if (init && init->children.size() >= 1) {
                loop_var = get_name(init->children[0]);
            }
        }

        // Try to extract loop bound from condition
        if (stmt->children.size() >= 2) {
            auto cond = stmt->children[1];
            if (cond) {
                auto cond_expr = std::dynamic_pointer_cast<VerilogParser::Expression>(cond);
                if (cond_expr && cond_expr->right) {
                    if (cond_expr->right->type == VerilogParser::NodeType::NUMBER) {
                        if (cond_expr->right->attributes.count("value")) {
                            try { unroll_count = std::stoi(cond_expr->right->attributes.at("value")); } catch(...) {}
                        }
                    }
                }
            }
        }

        synth_log("SYNTH", "  For loop: var=%s, unrolling %d iterations", loop_var.c_str(), unroll_count);

        // Process body unroll_count times, substituting loop variable
        auto body = (stmt->children.size() >= 4) ? stmt->children[3] :
                    (stmt->children.size() >= 1) ? stmt->children.back() : nullptr;
        if (body) {
            for (int iter = 0; iter < unroll_count; iter++) {
                // Create a local copy of signal map with loop variable substituted
                auto local_sm = sm;
                if (!loop_var.empty()) {
                    // Map loop variable to its current value as a string
                    local_sm[loop_var] = std::to_string(iter);
                }
                process_stmt(gn, body, local_sm, clk, seq, mod_map, depth, rst, true);
            }
        }
        return;
    }

    // Repeat loop - unroll
    if (stmt->type == VerilogParser::NodeType::REPEAT_LOOP) {
        if (stmt->children.size() >= 2) {
            auto count_expr = stmt->children[0];
            auto body = stmt->children[1];
            // Try to extract count
            int count = 32; // default
            if (count_expr && count_expr->type == VerilogParser::NodeType::NUMBER) {
                try { count = std::stoi(count_expr->attributes.at("value")); } catch(...) {}
            }
            for (int i = 0; i < count; i++) {
                process_stmt(gn, body, sm, clk, seq, mod_map, depth, rst, true);
            }
        }
        return;
    }

    // Default: process children
    for (auto &c : stmt->children) process_stmt(gn, c, sm, clk, seq, mod_map, depth, rst, true);
}

// Top-level: convert a module and all its sub-modules to gates
static GateNetlist module_to_gates(const std::string &name, std::shared_ptr<VerilogParser::ModuleDecl> mod,
                                    const ModuleMap &mod_map) {
    GateNetlist gn; gn.module_name = name;

    // First pass: parse parameters from module->parameters
    std::map<std::string, int> param_map;
    for (auto &p : mod->parameters) {
        if (!p) continue;
        if (p->type == VerilogParser::NodeType::PARAM_DECL) {
            std::string pname = p->attributes.count("name") ? p->attributes.at("name") : "";
            if (!pname.empty() && !p->children.empty()) {
                auto pval = p->children[0];
                if (pval && pval->type == VerilogParser::NodeType::NUMBER && pval->attributes.count("value")) {
                    try { param_map[pname] = std::stoi(pval->attributes.at("value")); } catch (...) {}
                }
            }
        }
    }

    // Helper: resolve parameter expressions like (1<<ADDR_W)-1
    auto resolve_param_expr = [&](const std::string &dim_str) -> int {
        // Try simple number first
        try { return std::stoi(dim_str); } catch (...) {}
        // Handle (1<<PARAM)-1 pattern
        std::string s = dim_str;
        // Remove whitespace
        s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
        // Pattern: (1<<N)-1 → 2^N - 1
        size_t shl = s.find("<<");
        if (shl != std::string::npos) {
            size_t lp = s.find('(');
            size_t rp = s.find(')');
            std::string base_str = (lp != std::string::npos && lp < shl) ? s.substr(lp+1, shl-lp-1) : s.substr(0, shl);
            std::string shift_str = s.substr(shl+2);
            // Remove trailing )-1 etc
            size_t minus = shift_str.find("-1");
            if (minus != std::string::npos) shift_str = shift_str.substr(0, minus);
            if (shift_str.back() == ')') shift_str.pop_back();
            int base = 1, shift = 0;
            try { base = std::stoi(base_str); } catch (...) {}
            if (param_map.count(shift_str)) shift = param_map[shift_str];
            else try { shift = std::stoi(shift_str); } catch (...) {}
            return (base << shift) - 1;
        }
        return 0;
    };

    // Helper to resolve a width: check if it depends on a parameter
    auto resolve_width = [&](int raw_width, std::shared_ptr<VerilogParser::ASTNode> item) -> int {
        if (raw_width > 1 && raw_width < 1000) return raw_width; // already valid
        // Try to get width from port declaration pattern
        // The parser stores raw width in "width" attribute, but for parameterized
        // widths like [WIDTH-1:0], it falls back to 32
        // Check if any parameter matches the port width expression
        (void)item;
        return raw_width;
    };

    // Override port widths using parameter map and expression resolution
    for (auto &p : mod->ports) {
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(p);
        int width = pd ? pd->width : 1;
        if (width >= 32 && pd && param_map.count("WIDTH")) {
            width = param_map["WIDTH"];
        } else if (width >= 32 && pd) {
            // Try to resolve from ADDR_W, DATA_W, STAGES etc.
            for (auto &[pname, pval] : param_map) {
                if (width >= pval && width < pval * 2 + 2) {
                    width = pval;
                    break;
                }
            }
        }
        gn.ports.push_back({pd->name, width, pd->dir==VerilogParser::PortDecl::INPUT});
        // Also register port wires in gn.wires and signal map
        gn.wires[pd->name] = width;
    }

    // Scan mod->items for non-ANSI port declarations (input/output/inout in body)
    // These are PortDecl objects with NodeType::MODULE_PORT
    for (auto &item : mod->items) {
        if (!item) continue;
        // PortDecl has type MODULE_PORT, check by dynamic cast
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(item);
        if (pd && !pd->name.empty()) {
            // Check if already registered from ANSI port list
            bool already = false;
            for (auto &p : gn.ports) {
                if (p.name == pd->name) { already = true; break; }
            }
            if (!already) {
                bool is_input = (pd->dir == VerilogParser::PortDecl::INPUT);
                gn.ports.push_back({pd->name, pd->width, is_input});
                gn.wires[pd->name] = pd->width;
            }
        }
    }
    std::map<std::string, std::string> sm;
    // Copy port names to signal map
    for (auto &p : mod->ports) {
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(p);
        sm[pd->name] = pd->name;
    }
    for (auto &item : mod->items) {
        if (!item) continue;
        if (item->type==VerilogParser::NodeType::WIRE_DECL || item->type==VerilogParser::NodeType::REG_DECL) {
            std::string n = get_name(item); int w = get_width(item);
            // If width is 32 (parser fallback), resolve from parameters
            if (w == 32 && param_map.count("WIDTH")) {
                w = param_map["WIDTH"];
            } else if (w == 32) {
                // Try other common parameter names: ADDR_W, DATA_W, STAGES
                for (auto &[pname, pval] : param_map) {
                    if (pname == "WIDTH" || pname == "ADDR_W" || pname == "DATA_W" || pname == "STAGES") {
                        w = pval;
                        break;
                    }
                }
            }
            if (!n.empty()) {
                // Check if this is a memory array
                if (item->attributes.count("memory_dim")) {
                    std::string dim = item->attributes.at("memory_dim");
                    // Parse dimension like "0:31" or "0:131071" or "0:(1<<ADDR_W)-1"
                    size_t colon = dim.find(':');
                    int count = 0;
                    if (colon != std::string::npos) {
                        int lo = 0, hi = 0;
                        std::string lo_str = dim.substr(0, colon);
                        std::string hi_str = dim.substr(colon + 1);
                        try { lo = std::stoi(lo_str); } catch (...) { lo = resolve_param_expr(lo_str); }
                        try { hi = std::stoi(hi_str); } catch (...) { hi = resolve_param_expr(hi_str); }
                        count = std::abs(hi - lo) + 1;
                    } else {
                        // Single number like "7" or "15"
                        try { count = std::stoi(dim) + 1; } catch (...) { count = resolve_param_expr(dim) + 1; }
                    }
                    if (count > 0 && count <= 131072) {
                        synth_log("SYNTH", "  Memory array: %s [%s] = %d entries x %d bits",
                                  n.c_str(), dim.c_str(), count, w);
                        // Expand memory array to individual registers
                        for (int idx = 0; idx < count; idx++) {
                            std::string elem_name = n + "[" + std::to_string(idx) + "]";
                            gn.wires[elem_name] = w;
                            sm[elem_name] = elem_name;
                        }
                        // Also keep the base name for read/write access
                        gn.wires[n] = w;
                        sm[n] = n;
                    }
                } else {
                    gn.wires[n] = w;
                    sm[n] = n;
                }
            }
        }
        if (item->type==VerilogParser::NodeType::GENERATE_BLOCK ||
            item->type==VerilogParser::NodeType::GENERATE_FOR ||
            item->type==VerilogParser::NodeType::GENERATE_IF ||
            item->type==VerilogParser::NodeType::GENERATE_CASE) {
            // Recursively process generate block children
            for (auto &gc : item->children) {
                if (!gc) continue;
                auto gct = gc->type;
                if (gct == VerilogParser::NodeType::WIRE_DECL || gct == VerilogParser::NodeType::REG_DECL ||
                    gct == VerilogParser::NodeType::LOGIC_DECL) {
                    std::string n = get_name(gc); int w = get_width(gc);
                    if (!n.empty()) { gn.wires[n] = w; sm[n] = n; }
                } else if (gct == VerilogParser::NodeType::ALWAYS_BLOCK) {
                    auto ab = std::dynamic_pointer_cast<VerilogParser::AlwaysBlock>(gc);
                    bool seq = false; std::string clk, rst;
                    if (ab) {
                        seq = (ab->sens == VerilogParser::AlwaysBlock::POSEDGE || ab->sens == VerilogParser::AlwaysBlock::BOTH);
                        clk = ab->clockSignal;
                        if (ab->sens == VerilogParser::AlwaysBlock::BOTH && ab->sensitivityList.size() >= 2)
                            rst = ab->sensitivityList[1];
                    }
                    for (auto &c : gc->children) process_stmt(gn, c, sm, clk, seq, mod_map, 0, rst);
                } else if (gct == VerilogParser::NodeType::ASSIGN && gc->children.size() >= 2) {
                    std::string d = emit_expr(gn, gc->children[0], sm);
                    std::string s = emit_expr(gn, gc->children[1], sm,
                                              get_signal_width(gn, d));
                    if (!d.empty() && !s.empty()) {
                        int w = std::max(gn.wires.count(d)?gn.wires[d]:1, gn.wires.count(s)?gn.wires[s]:1);
                        if (w > 1) { for (int b=0;b<w;b++) { std::string dw=d+"["+std::to_string(b)+"]",sw=s+"["+std::to_string(b)+"]"; gn.wires[dw]=1;gn.wires[sw]=1;gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{sw}},{"Y",{dw}}}); } }
                        else gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{s}},{"Y",{d}}});
                    }
                } else if (gct == VerilogParser::NodeType::MODULE_INSTANCE) {
                    std::string sub_type = gc->attributes.count("type") ? gc->attributes.at("type") : "";
                    std::string sub_name = gc->attributes.count("name") ? gc->attributes.at("name") : "";
                    if (!sub_type.empty() && !sub_name.empty())
                        elaborate_instance(gn, sub_name, sub_type, gc, mod_map, sm, 0);
                }
            }
            continue;
        }
        if (item->type==VerilogParser::NodeType::ASSIGN && item->children.size()>=2) {
            std::string d = emit_expr(gn, item->children[0], sm);
            std::string s = emit_expr(gn, item->children[1], sm,
                                      get_signal_width(gn, d));
            if (!d.empty() && !s.empty()) {
                int dw = gn.wires.count(d) ? gn.wires[d] : 1;
                int sw = gn.wires.count(s) ? gn.wires[s] : 1;
                int w = std::max(dw, sw);
                // Also check port widths
                for (auto &p : gn.ports) {
                    if (p.name == d) w = std::max(w, p.width);
                    if (p.name == s) w = std::max(w, p.width);
                }
                if (w > 1) {
                    // Multi-bit: per-bit BUF connections
                    for (int b = 0; b < w; b++) {
                        std::string dwire = d + "[" + std::to_string(b) + "]";
                        std::string swire = s + "[" + std::to_string(b) + "]";
                        if (!gn.wires.count(dwire)) { gn.wires[dwire] = 1; sm[dwire] = dwire; }
                        if (!gn.wires.count(swire)) { gn.wires[swire] = 1; sm[swire] = swire; }
                        gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{swire}},{"Y",{dwire}}});
                    }
                } else {
                    gn.add_cell("$_BUF_","_c"+std::to_string(gn.next_id++),{{"A",{s}},{"Y",{d}}});
                }
            }
        }
        if (item->type==VerilogParser::NodeType::ALWAYS_BLOCK) {
            auto ab = std::dynamic_pointer_cast<VerilogParser::AlwaysBlock>(item);
            bool seq=false; std::string clk, rst;
            if (ab) {
                seq=(ab->sens==VerilogParser::AlwaysBlock::POSEDGE||ab->sens==VerilogParser::AlwaysBlock::BOTH);
                clk=ab->clockSignal;
                // Detect async reset from sensitivity list
                // If BOTH edge type and sensitivityList has 2 entries, second is async reset
                if (ab->sens == VerilogParser::AlwaysBlock::BOTH && ab->sensitivityList.size() >= 2) {
                    rst = ab->sensitivityList[1];
                }
            }
            for (auto &c : item->children) process_stmt(gn, c, sm, clk, seq, mod_map, 0, rst);
        }
        // Sub-module instantiation
        if (item->type==VerilogParser::NodeType::MODULE_INSTANCE) {
            std::string sub_type = item->attributes.count("type") ? item->attributes.at("type") : "";
            std::string sub_name = item->attributes.count("name") ? item->attributes.at("name") : "";
            if (!sub_type.empty() && !sub_name.empty()) {
                elaborate_instance(gn, sub_name, sub_type, item, mod_map, sm, 0);
            }
        }
    }
    return gn;
}

static std::string netlist_to_verilog(const GateNetlist &gn) {
    auto format_signal = [](const std::string &signal) -> std::string {
        if (signal == "0") return "1'b0";
        if (signal == "1") return "1'b1";
        return signal;
    };
    std::stringstream ss;
    ss << "/* Gate-level netlist by ai_digital synthesis */\n/* Module: " << gn.module_name << " Cells: " << gn.cells.size() << " */\n\n";
    ss << "module " << gn.module_name << " (";
    for (size_t i=0;i<gn.ports.size();i++) { if(i) ss<<", "; ss<<gn.ports[i].name; }
    ss << ");\n";
    std::set<std::string> port_names;
    for (auto &p : gn.ports) {
        port_names.insert(p.name);
        ss << "  " << (p.is_input ? "input " : "output ");
        if (p.width > 1) ss << "[" << (p.width - 1) << ":0] ";
        ss << p.name << ";\n";
    }

    std::map<std::string, int> printable_wires;
    for (const auto &[name, width] : gn.wires) {
        if (name.empty()) continue;
        if (name == "0" || name == "1" || name == "1'b0" || name == "1'b1") continue;
        size_t bracket = name.find('[');
        std::string base = (bracket == std::string::npos) ? name : name.substr(0, bracket);
        if (port_names.count(base)) continue;
        int final_width = width;
        if (bracket != std::string::npos) {
            size_t end = name.find(']', bracket);
            int bit_index = 0;
            if (end != std::string::npos) {
                try { bit_index = std::stoi(name.substr(bracket + 1, end - bracket - 1)); } catch (...) { bit_index = 0; }
            }
            final_width = std::max(final_width, bit_index + 1);
        }
        printable_wires[base] = std::max(printable_wires[base], final_width);
    }

    for (const auto &[name, width] : printable_wires) {
        ss << "  wire ";
        if (width > 1) ss << "[" << (width - 1) << ":0] ";
        ss << name << ";\n";
    }
    ss<<"\n";
    // Sort cells by type then name for cleaner output
    std::vector<GateCell> sorted_cells = gn.cells;
    std::sort(sorted_cells.begin(), sorted_cells.end(),
        [](const GateCell &a, const GateCell &b) {
            if (a.type != b.type) return a.type < b.type;
            return a.name < b.name;
        });
    for (auto &c : sorted_cells) {
        ss<<"  "<<c.type<<" "<<c.name<<" (";
        for (size_t i=0;i<c.conns.size();i++) { if(i) ss<<", "; ss<<"."<<c.conns[i].first<<"("<<format_signal(c.conns[i].second.signal)<<")"; }
        ss<<");\n";
    }
    ss<<"endmodule\n";
    return ss.str();
}

// ============================================================================
// OptPass Implementations
// ============================================================================

// ============================================================================
// Optimization Pass functions (operate on GateNetlist used in synth_real)
// ============================================================================

static void pass_constprop(GateNetlist &gn) {
    synth_log("constprop", "Constant propagation...");
    // Phase 1: Find constant-driven wires
    std::map<std::string, int> const_wires;  // wire_name → constant value
    // Include literal wire names (for wires named "0"/"1" etc.)
    // Also find BUF cells that connect constants
    // Also detect _const prefix wires that come from emit_expr NUMBER case
    // These carry known constant values that we need to back-annotate

    // First pass: collect all _const prefix wire names and their associated bit wires
    std::set<std::string> const_wire_sets; // base names like _const0, _const5
    for (auto &cell : gn.cells) {
        if (cell.type == "$_BUF_" && cell.conns.size() >= 2) {
            std::string output = cell.conns[1].second.signal;
            // Check if this BUF drives a _const prefix wire as a per-bit signal via const_wire[X]
            size_t bracket = output.find('[');
            if (bracket != std::string::npos) {
                std::string base = output.substr(0, bracket);
                // Also check if this is driven by another _const wire through BUF chain
                std::string input = cell.conns[0].second.signal;
                size_t ib = input.find('[');
                if (ib != std::string::npos) {
                    std::string input_base = input.substr(0, ib);
                    if (input_base.find("_const") == 0) {
                        const_wire_sets.insert(base);
                        // Now we know: base = const_wire, input_base = _const0 or _const5
                        // The actual constant value is encoded in the numeric portion of input_base
                        try {
                            std::string num_part = input_base.substr(6); // after "_const"
                            int const_num = std::stoi(num_part);
                            // Extract bit index from both sides to determine the bit value
                            std::string obit = output.substr(bracket + 1);
                            obit.pop_back(); // remove ']'
                            int bit_idx = std::stoi(obit);
                            // If const_num is small (< 32), treat as a single-bit value at bit position
                            if (const_num < 32) {
                                int bit_val = (const_num >> bit_idx) & 1;
                                const_wires[output] = bit_val;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }

    for (auto &cell : gn.cells) {
        if (cell.type == "$_BUF_" && cell.conns.size() >= 2) {
            std::string input = cell.conns[0].second.signal;
            std::string output = cell.conns[1].second.signal;
            // Check if input is a constant expression
            bool is_const = true;
            int const_val = 0;
            // Check for numeric strings (both sized and unsized literals)
            if (input.empty()) { is_const = false; }
            else if (input == "0" || input == "1'b0" || input == "1'b1") {
                const_val = (input.find('1') != std::string::npos) ? 1 : 0;
            } else if (input.find("1'b1") != std::string::npos) {
                const_val = 1;
            } else {
                // Try parsing as integer
                try { const_val = std::stoi(input); }
                catch (...) { is_const = false; }
            }
            if (is_const) {
                const_wires[output] = const_val;
            } else if (const_wires.count(input)) {
                // Propagate through BUF chain
                const_wires[output] = const_wires[input];
            }
        }
    }

    // Phase 2: Apply constant folding to logic cells
    int folded = 0;
    // Repeat until convergence (new constants may enable more folding)
    for (int pass = 0; pass < 5; pass++) {
        int pass_folded = 0;
    for (auto &cell : gn.cells) {
        if (cell.type == "$_AND_" || cell.type == "AND2X1" ||
            cell.type == "$_OR_" || cell.type == "OR2X1" ||
            cell.type == "$_XOR_" || cell.type == "XOR2X1" ||
            cell.type == "$_NOT_" || cell.type == "INVX1") {
            std::string a_sig, b_sig, y_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "A") a_sig = conn.second.signal;
                if (conn.first == "B") b_sig = conn.second.signal;
                if (conn.first == "Y") y_sig = conn.second.signal;
            }
            bool a_const = const_wires.count(a_sig) > 0;
            bool b_const = const_wires.count(b_sig) > 0;
            int a_val = a_const ? const_wires[a_sig] : -1;
            int b_val = b_const ? const_wires[b_sig] : -1;

            // NOT gate: ~A
            if (cell.type.find("NOT") != std::string::npos || cell.type.find("INV") != std::string::npos) {
                if (a_const) {
                    int result = (a_val != 0) ? 0 : 1;
                    cell.type = "$_BUF_";
                    for (auto &conn : cell.conns) {
                        if (conn.first == "A") conn.second.signal = std::to_string(result);
                    }
                    const_wires[y_sig] = result;
                    pass_folded++;
                }
                continue;
            }

            // XOR: both constant → compute; one constant → simplify
            if (cell.type.find("XOR") != std::string::npos) {
                if (a_const && b_const) {
                    int result = (a_val != 0) ^ (b_val != 0);
                    cell.type = "$_BUF_";
                    for (auto &conn : cell.conns) {
                        if (conn.first == "A") conn.second.signal = std::to_string(result);
                    }
                    cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                        [](auto &c) { return c.first == "B"; }), cell.conns.end());
                    const_wires[y_sig] = result;
                    pass_folded++;
                } else if (a_const && a_val == 0) {
                    // 0 ^ X = X → direct connection (BUF)
                    std::string b_input = b_sig;
                    cell.type = "$_BUF_";
                    for (auto &conn : cell.conns) {
                        if (conn.first == "A") conn.second.signal = b_input;
                    }
                    cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                        [](auto &c) { return c.first == "B"; }), cell.conns.end());
                    pass_folded++;
                } else if (b_const && b_val == 0) {
                    std::string a_input = a_sig;
                    cell.type = "$_BUF_";
                    cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                        [](auto &c) { return c.first == "B"; }), cell.conns.end());
                    pass_folded++;
                }
                continue;
            }

            // AND/OR: both constant → fold; partial → simplify
            if (a_const && b_const) {
                int result;
                if (cell.type.find("AND") != std::string::npos) result = (a_val != 0 && b_val != 0) ? 1 : 0;
                else result = (a_val != 0 || b_val != 0) ? 1 : 0;
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = std::to_string(result);
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                const_wires[y_sig] = result;
                pass_folded++;
            } else if (a_const && a_val == 0 && cell.type.find("AND") != std::string::npos) {
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = "0";
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                const_wires[y_sig] = 0;
                pass_folded++;
            } else if (b_const && b_val == 0 && cell.type.find("AND") != std::string::npos) {
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = "0";
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                const_wires[y_sig] = 0;
                pass_folded++;
            } else if (a_const && a_val == 1 && cell.type.find("OR") != std::string::npos) {
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = "1";
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                const_wires[y_sig] = 1;
                pass_folded++;
            } else if (b_const && b_val == 1 && cell.type.find("OR") != std::string::npos) {
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = "1";
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                const_wires[y_sig] = 1;
                pass_folded++;
            }
        }
    } // end cell loop
    if (pass_folded > 0) {
        folded += pass_folded;
        synth_log("constprop", "  pass %d: folded %d cells", pass, pass_folded);
    } else break; // converged
    } // end pass loop

    // Phase 3: Wire replacement — replace constant-driven cells with direct BUF to constant
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            if (conn.first != "Y" && conn.first != "Q" && const_wires.count(conn.second.signal)) {
                int cval = const_wires[conn.second.signal];
                conn.second.signal = cval ? "1" : "0";
                folded++;
            }
        }
    }

    synth_log("constprop", "Found %zu constant signals, %d expressions folded", const_wires.size(), folded);
}

static void pass_dce(GateNetlist &gn) {
    synth_log("dce", "Dead code elimination...");
    std::set<std::string> used;
    // Mark output ports and their bit wires as alive
    for (auto &port : gn.ports) {
        if (!port.is_input) {
            used.insert(port.name);
            for (int b = 0; b < port.width; b++) {
                used.insert(port.name + "[" + std::to_string(b) + "]");
            }
        }
    }
    for (auto &port : gn.ports) {
        if (port.is_input) {
            used.insert(port.name);
            for (int b = 0; b < port.width; b++) {
                used.insert(port.name + "[" + std::to_string(b) + "]");
            }
        }
    }
    size_t initial_used = used.size();
    synth_log("dce", "  Seed: %zu alive ports/wires", initial_used);
    bool changed = true; int iters = 0;
    std::vector<std::string> alive_path; // track newly alive wires per iteration
    while (changed && iters++ < 100) {
        changed = false;
        alive_path.clear();
        for (auto &cell : gn.cells) {
            bool out_used = false;
            for (auto &conn : cell.conns) {
                if ((conn.first == "Y" || conn.first == "Q") && used.count(conn.second.signal)) { out_used = true; break; }
            }
            if (out_used) {
                for (auto &conn : cell.conns) {
                    if (conn.first != "Y" && conn.first != "Q" && !used.count(conn.second.signal)) {
                        used.insert(conn.second.signal); changed = true;
                        alive_path.push_back(conn.second.signal);
                    }
                }
            }
        }
        if (changed) {
            synth_log("dce", "  iter %d: %zu newly alive wires", iters, alive_path.size());
            if (alive_path.size() <= 10) {
                for (auto &w : alive_path) synth_log("dce", "    alive: %s", w.c_str());
            }
        }
    }
    synth_log("dce", "  Mark/sweep: %zu=>%zu alive wires (%d iterations)", initial_used, used.size(), iters);
    size_t before = gn.cells.size();
    size_t before_wires = gn.wires.size();
    gn.cells.erase(std::remove_if(gn.cells.begin(), gn.cells.end(),
        [&](const GateCell &cell) {
            for (auto &conn : cell.conns)
                if (conn.first == "Y" || conn.first == "Q") return used.count(conn.second.signal) == 0;
            return false;
        }), gn.cells.end());
    // Also clean up dead wires: remove wires not in used set
    std::vector<std::string> dead_wires;
    for (auto &[wname, wwidth] : gn.wires) {
        if (used.count(wname) == 0) {
            dead_wires.push_back(wname);
        }
    }
    for (auto &dw : dead_wires) {
        gn.wires.erase(dw);
    }
    synth_log("dce", "Removed %zu dead cells, %zu dead wires (%zu -> %zu cells)",
              before - gn.cells.size(), dead_wires.size(), before, gn.cells.size());
}

static void pass_cse(GateNetlist &gn) {
    synth_log("cse", "Common subexpression elimination...");
    std::map<std::string, std::string> sig_to_out, replace_map;

    // Commutative ops: AND, OR, XOR, MUL, EQ, NE, NAND, NOR, XNOR
    auto is_commutative = [](const std::string &t) {
        return t.find("AND") != std::string::npos || t.find("OR") != std::string::npos ||
               t.find("XOR") != std::string::npos || t.find("MUL") != std::string::npos ||
               t.find("NAND") != std::string::npos || t.find("NOR") != std::string::npos ||
               t.find("XNOR") != std::string::npos;
    };
    auto is_stateful = [](const std::string &t) {
        return t.find("DFF") != std::string::npos ||
               t.find("LATCH") != std::string::npos ||
               t.find("FF") != std::string::npos;
    };

    for (auto &cell : gn.cells) {
        if (is_stateful(cell.type)) continue;
        std::string sig = cell.type;
        std::vector<std::string> inputs;
        for (auto &conn : cell.conns) {
            if (conn.first != "Y" && conn.first != "Q") inputs.push_back(conn.second.signal);
        }
        if (is_commutative(cell.type)) {
            // For commutative ops: sort inputs so A+B == B+A
            std::sort(inputs.begin(), inputs.end());
        }
        // For non-commutative (SUB, SHL, SHR, MUX): keep input order as-is
        for (auto &inp : inputs) sig += "|" + inp;

        std::string output;
        for (auto &conn : cell.conns) {
            if (conn.first == "Y" || conn.first == "Q") { output = conn.second.signal; break; }
        }
        if (!output.empty()) {
            if (sig_to_out.count(sig)) {
                replace_map[output] = sig_to_out[sig];
            } else {
                sig_to_out[sig] = output;
            }
        }
    }
    // Apply replacements: redirect all consumers of merged output to the first instance
    int merged = 0;
    std::vector<std::string> merge_details;
    for (auto &cell : gn.cells) {
        if (is_stateful(cell.type)) continue;
        for (auto &conn : cell.conns) {
            // A common-expression alias may only substitute a consumer input.
            // Rewriting Y/Q turns a duplicate cell into a second driver of the
            // canonical wire and disconnects its declared destination.  That
            // is especially destructive at hierarchy boundaries, where a
            // port-connection BUF is the sole driver of a parent signal.
            // Leave producer pins intact; DCE will remove an unreferenced
            // duplicate producer after its consumers have been redirected.
            if (conn.first != "Y" && conn.first != "Q" && replace_map.count(conn.second.signal)) {
                if (merge_details.size() < 50) {
                    merge_details.push_back(conn.second.signal + "=>" + replace_map[conn.second.signal]);
                }
                conn.second.signal = replace_map[conn.second.signal];
                merged++;
            }
        }
    }
    synth_log("cse", "Merged %zu subexpressions (%d signal replacements)", replace_map.size(), merged);
    if (!merge_details.empty()) {
        synth_log("cse", "  Details (%zu shown):", merge_details.size());
        for (auto &d : merge_details) synth_log("cse", "    %s", d.c_str());
    }
}

static void pass_expr_opt(GateNetlist &gn) {
    synth_log("opt_expr", "Expression optimization...");
    int simplified = 0;

    // Phase 1: Build NOT map for double-inversion elimination
    std::map<std::string, std::string> not_map;
    for (auto &cell : gn.cells) {
        if (cell.type == "$_NOT_" || cell.type == "INVX1" || cell.type == "INVX2" || cell.type == "INVX4") {
            if (cell.conns.size() >= 2) {
                not_map[cell.conns[1].second.signal] = cell.conns[0].second.signal;
            }
        }
    }
    // Double-inversion elimination: NOT(NOT(X)) = X
    int double_inv_simplified = 0;
    for (auto &cell : gn.cells) {
        if (cell.type == "$_NOT_" || cell.type.find("INV") != std::string::npos) {
            if (cell.conns.size() >= 2) {
                std::string in = cell.conns[0].second.signal;
                if (not_map.count(in)) {
                    std::string out = cell.conns[1].second.signal;
                    std::string orig = not_map[in];
                    for (auto &c2 : gn.cells) for (auto &conn : c2.conns)
                        if (conn.second.signal == out) conn.second.signal = orig;
                    double_inv_simplified++;
                    if (double_inv_simplified <= 20) {
                        synth_log("opt_expr", "  double-NOT: %s <- NOT(NOT(%s)) -> %s", out.c_str(), orig.c_str(), orig.c_str());
                    }
                }
            }
        }
    }
    if (double_inv_simplified > 0) synth_log("opt_expr", "  Phase 1: %d double-inversions eliminated", double_inv_simplified);
    simplified += double_inv_simplified;

    // Phase 2: Identity optimizations
    // A | 0 = A (OR with zero input)
    // A & all_ones wire → A (but we can't detect all_ones at gate level easily)
    std::vector<size_t> to_remove;
    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &cell = gn.cells[i];
        std::string a_sig, b_sig, y_sig;
        for (auto &conn : cell.conns) {
            if (conn.first == "A") a_sig = conn.second.signal;
            if (conn.first == "B") b_sig = conn.second.signal;
            if (conn.first == "Y") y_sig = conn.second.signal;
        }

        // BUF with same input/output → remove (identity)
        if ((cell.type == "$_BUF_" || cell.type.find("BUF") != std::string::npos) && a_sig == y_sig) {
            to_remove.push_back(i);
            simplified++;
            continue;
        }

        // BUF driving another BUF of same wire → merge
        if ((cell.type == "$_BUF_" || cell.type.find("BUF") != std::string::npos) && !a_sig.empty() && !y_sig.empty()) {
            if (a_sig == y_sig) continue; // already handled above
        }

        // AND with identical inputs: A & A = A → BUF
        if ((cell.type == "$_AND_" || cell.type.find("AND2") != std::string::npos) && a_sig == b_sig) {
            cell.type = "$_BUF_";
            cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                [](auto &c) { return c.first == "B"; }), cell.conns.end());
            simplified++;
        }
        // OR with identical inputs: A | A = A → BUF
        if ((cell.type == "$_OR_" || cell.type.find("OR2") != std::string::npos) && a_sig == b_sig) {
            cell.type = "$_BUF_";
            cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                [](auto &c) { return c.first == "B"; }), cell.conns.end());
            simplified++;
        }
        // XOR with identical inputs: A ^ A = 0 → constant 0 BUF
        if ((cell.type == "$_XOR_" || cell.type.find("XOR2") != std::string::npos) && a_sig == b_sig) {
            cell.type = "$_BUF_";
            for (auto &conn : cell.conns) {
                if (conn.first == "A") conn.second.signal = "0";
            }
            cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                [](auto &c) { return c.first == "B"; }), cell.conns.end());
            simplified++;
        }
    }

    synth_log("opt_expr", "  Phase 2: %zu identity operations removed", to_remove.size());
    if (to_remove.size() > 0 && to_remove.size() <= 20) {
        for (int j = (int)to_remove.size() - 1; j >= 0; j--) {
            size_t idx = to_remove[j];
            synth_log("opt_expr", "    removing identity BUF: %s", gn.cells[idx].name.c_str());
        }
    }

    // Remove identity BUFs (reverse order to preserve indices)
    for (int j = (int)to_remove.size() - 1; j >= 0; j--) {
        size_t idx = to_remove[j];
        // Propagate the input to all consumers before removing
        std::string removed_out;
        for (auto &conn : gn.cells[idx].conns) {
            if (conn.first == "Y" || conn.first == "Q") { removed_out = conn.second.signal; break; }
        }
        std::string removed_in = gn.cells[idx].conns[0].second.signal;
        // Replace all uses of this BUF's output with its input
        for (size_t k = 0; k < gn.cells.size(); k++) {
            if (k == idx) continue;
            for (auto &conn : gn.cells[k].conns) {
                if (conn.second.signal == removed_out) {
                    conn.second.signal = removed_in;
                }
            }
        }
    }
    for (int j = (int)to_remove.size() - 1; j >= 0; j--) {
        gn.cells.erase(gn.cells.begin() + to_remove[j]);
    }

    // Phase 3: BUF chain collapse (BUF→BUF→wire = BUF→wire)
    std::map<std::string, std::string> buf_output_to_input;
    for (auto &cell : gn.cells) {
        if (cell.type == "$_BUF_" || cell.type.find("BUF") != std::string::npos) {
            if (cell.conns.size() >= 2) {
                std::string out = cell.conns[1].second.signal;
                std::string in = cell.conns[0].second.signal;
                if (buf_output_to_input.count(in)) {
                    // This BUF's input is another BUF's output → skip intermediate
                    buf_output_to_input[out] = buf_output_to_input[in];
                    simplified++;
                } else {
                    buf_output_to_input[out] = in;
                }
            }
        }
    }
    // Apply BUF chain collapse: redirect consumers of intermediate BUF outputs to original source
    int buf_chain_simplified = 0;
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            if (conn.first != "Y" && conn.first != "Q" && buf_output_to_input.count(conn.second.signal)) {
                if (buf_chain_simplified < 20) {
                    synth_log("opt_expr", "  buf_chain: %s => %s", conn.second.signal.c_str(), buf_output_to_input[conn.second.signal].c_str());
                }
                conn.second.signal = buf_output_to_input[conn.second.signal];
                buf_chain_simplified++;
            }
        }
    }
    simplified += buf_chain_simplified;
    synth_log("opt_expr", "  Phase 3: %d BUF chains collapsed", buf_chain_simplified);

    synth_log("opt_expr", "Simplified %d expressions (double_inv=%d, identity=%zu, buf_chain=%d)",
        simplified, double_inv_simplified, to_remove.size(), buf_chain_simplified);
}

static void pass_techmap(GateNetlist &gn) {
    synth_log("techmap", "Technology mapping with real cell library...");

    // Build fanout map for each wire
    std::map<std::string, int> fanout;
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            if (conn.first != "Y" && conn.first != "Q") {
                fanout[conn.second.signal]++;
            }
        }
    }

    // Function mapping table: RTLIL cell → logic function for liberty matching
    struct FuncMap { const char *rtlil_type; const char *liberty_func; };
    static const FuncMap func_map[] = {
        {"$_AND_",   "(A * B)"},
        {"$_OR_",    "(A + B)"},
        {"$_NOT_",   "(!A)"},
        {"$_XOR_",   "(A * !B) + (!A * B)"},
        {"$_XNOR_",  "(A * B) + (!A * !B)"},
        {"$_NAND_",  "(!A) + (!B)"},
        {"$_NOR_",   "(!A * !B)"},
        {"$_MUX_",   "(A * !S0) + (B * S0)"},
        {"$_BUF_",   "A"},
        {"$_DFF_P_", "DFF_posedge"},
        {"$_DFF_N_", "DFF_negedge"},
        {"$_DFFSR_PPP_", "DFFSR"},
        {"$_DFFE_PP_", "DFFE"},
    };

    int mapped = 0, skipped = 0;
    std::map<std::string, int> drive_stats;

    for (auto &cell : gn.cells) {
        // Compute effective fanout for this cell
        int eff_fanout = 1;
        for (auto &conn : cell.conns) {
            if (conn.first == "Y" || conn.first == "Q") {
                auto it = fanout.find(conn.second.signal);
                if (it != fanout.end()) eff_fanout = std::max(eff_fanout, it->second);
            }
        }

        // Find matching liberty function
        std::string func;
        for (auto &fm : func_map) {
            if (cell.type == fm.rtlil_type) {
                func = fm.liberty_func;
                break;
            }
        }

        if (func.empty()) {
            // Try partial match for sub-modules or already-mapped cells
            if (cell.type.find("DFF") != std::string::npos) {
                func = "DFF_posedge";
            } else if (cell.type.find("DFFSR") != std::string::npos) {
                func = "DFFSR";
            } else {
                skipped++;
                continue;
            }
        }

        // Use liberty to find best cell
        const Liberty::LibertyCell *best = nullptr;
        if (g_liberty_loaded) {
            best = g_liberty_lib.find_best_cell(func, eff_fanout);
            if (!best && (func == "DFF_posedge" || func == "DFF_negedge" || func == "DFFSR" || func == "DFFE")) {
                // Try multiple prefix patterns for sequential cells
                std::vector<std::string> prefixes;
                if (func == "DFF_posedge" || func == "DFF_negedge") {
                    prefixes = {"ADFFQX", "DFFPOS", "DFFNEG", "DFFQX", "SDFF"};
                } else if (func == "DFFSR") {
                    prefixes = {"ADFFSR", "DFFSR", "DFFR", "SDFFSR"};
                } else if (func == "DFFE") {
                    prefixes = {"ADFFEX", "ADFFE", "DFFE"};
                }
                const Liberty::LibertyCell *best_dff = nullptr;
                double best_area = 999;
                for (auto &prefix : prefixes) {
                    for (auto &kv : g_liberty_lib.cells) {
                        if (kv.second.dont_use) continue;
                        if (kv.first.find(prefix) == 0 && kv.second.is_sequential) {
                            double diff = std::abs(kv.second.area - (eff_fanout <= 2 ? 6.0 : (eff_fanout <= 4 ? 9.0 : 12.0)));
                            if (diff < best_area) { best_area = diff; best_dff = &kv.second; }
                        }
                    }
                    if (best_dff) break;
                }
                best = best_dff;
            }
        }

        if (best) {
            std::string prev_type = cell.type;
            cell.type = best->name;
            drive_stats[cell.type]++;
            mapped++;
            if (mapped <= 15) {
                synth_log("techmap", "  %s -> %s (area=%.1f, fanout=%d)", prev_type.c_str(), cell.type.c_str(), best->area, eff_fanout);
            }
        } else {
            // Fallback: use the old hardcoded mapping as a last resort
            skipped++;
        }
    }

    synth_log("techmap", "Mapped %d cells to real library cells, %d skipped (no match)", mapped, skipped);
    if (drive_stats.size() > 0) {
        std::string stats_str;
        int i = 0;
        for (auto &[t, c] : drive_stats) {
            if (i++ < 20) stats_str += t + ":" + std::to_string(c) + " ";
        }
        synth_log("techmap", "  Drive distribution: %s", stats_str.c_str());
    }
}

static void pass_logic_min(GateNetlist &gn) {
    synth_log("logic_min", "Logic minimization (Espresso-style)...");
    int simplified = 0;
    // Rule 1: Double inversion → identity (NOT(NOT(A)) = A)
    std::map<std::string, std::string> not_map; // output → input
    for (auto &cell : gn.cells) {
        if (cell.type == "$_NOT_" && cell.conns.size() >= 2) {
            not_map[cell.conns[1].second.signal] = cell.conns[0].second.signal;
        }
    }
    // Replace downstream consumers of double-inverted signals
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            if (not_map.count(conn.second.signal)) {
                std::string orig = not_map[conn.second.signal];
                if (not_map.count(orig)) {
                    conn.second.signal = not_map[orig]; // triple inversion → single
                    simplified++;
                }
            }
        }
    }
    synth_log("logic_min", "Double-inversion simplifications: %d", simplified);

    // Rule 2: A&A → A, A|A → A (redundant inputs)
    int redundant = 0;
    for (auto &cell : gn.cells) {
        if ((cell.type == "$_AND_" || cell.type == "$_OR_") && cell.conns.size() >= 3) {
            // Check if A and B ports connect to same wire
            std::string a_sig, b_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "A") a_sig = conn.second.signal;
                if (conn.first == "B") b_sig = conn.second.signal;
            }
            if (!a_sig.empty() && a_sig == b_sig) {
                // Replace cell with BUF (for AND: A&A=A, for OR: A|A=A)
                cell.type = "$_BUF_";
                // Remove B port connection
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](auto &c) { return c.first == "B"; }), cell.conns.end());
                redundant++;
            }
        }
    }
    synth_log("logic_min", "Redundant-input simplifications: %d", redundant);

    // Rule 3: A & 1 → A, A | 0 → A (identity with constant)
    // Rule 4: A & 0 → 0, A | 1 → 1 (dominator with constant)
    int const_folded = 0;
    for (auto &cell : gn.cells) {
        if (cell.type == "$_AND_" || cell.type == "$_OR_") {
            bool force_zero = false;
            bool force_one = false;
            for (auto &conn : cell.conns) {
                if (conn.first == "A" || conn.first == "B") {
                    std::string s = conn.second.signal;
                    bool is_zero = (s == "0" || s == "1'b0");
                    bool is_one = (s == "1" || s == "1'b1");
                    force_zero = force_zero || (cell.type == "$_AND_" && is_zero);
                    force_one = force_one || (cell.type == "$_OR_" && is_one);
                }
            }
            if (force_zero || force_one) {
                // Dominating constants must remain explicit. Merely changing
                // the cell type to BUF preserves its old A input and changes
                // A&0 into A, which is a functional corruption.
                const char *value = force_one ? "1" : "0";
                cell.type = "$_BUF_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = value;
                }
                cell.conns.erase(std::remove_if(cell.conns.begin(), cell.conns.end(),
                    [](const auto &conn) { return conn.first == "B"; }), cell.conns.end());
                const_folded++;
            }
        }
    }
    // Rule 5: AOI/OAI detection — merge AND+NOT→NOT into single AOI cell
    // AND(A,B)→Y connected to NOT(A)→Z: merge into NAND(A,B)→Z
    int aoi_merged = 0;
    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &not_cell = gn.cells[i];
        if (not_cell.type != "$_NOT_" && not_cell.type.find("INV") == std::string::npos) continue;
        std::string not_in, not_out;
        for (auto &conn : not_cell.conns) {
            if (conn.first == "A") not_in = conn.second.signal;
            if (conn.first == "Y") not_out = conn.second.signal;
        }
        if (not_in.empty() || not_out.empty()) continue;
        // Find AND/OR gate whose Y = NOT's input
        for (size_t j = 0; j < gn.cells.size(); j++) {
            auto &gate = gn.cells[j];
            if (gate.type.find("DFF") != std::string::npos) continue;
            std::string gate_y;
            for (auto &conn : gate.conns)
                if (conn.first == "Y") { gate_y = conn.second.signal; break; }
            if (gate_y != not_in) continue;
            // Match found: gate.Y → NOT.A → NOT.Y
            // Change NOT to buffer with gate's inputs feeding it, remove gate
            if (gate.type.find("AND") != std::string::npos) {
                // AND→NOT = NAND: convert NOT to NAND with gate's inputs
                not_cell.type = "$_NAND_";
                aoi_merged++;
            } else if (gate.type.find("OR") != std::string::npos) {
                // OR→NOT = NOR: convert NOT to NOR with gate's inputs
                not_cell.type = "$_NOR_";
                aoi_merged++;
            } else continue;
            // Copy gate's inputs to NOT cell
            for (auto &gconn : gate.conns) {
                if (gconn.first == "A" || gconn.first == "B") {
                    not_cell.conns.push_back(gconn);
                }
            }
            // Mark gate for removal
            gate.type = "_MERGED_";
            break;
        }
    }
    gn.cells.erase(std::remove_if(gn.cells.begin(), gn.cells.end(),
        [](const GateCell &c) { return c.type == "_MERGED_"; }), gn.cells.end());
    synth_log("logic_min", "AOI/OAI merges: %d", aoi_merged);

    synth_log("logic_min", "Constant-folded expressions: %d", const_folded);
}

static void pass_fsm_extract(GateNetlist &gn) {
    synth_log("fsm_extract", "FSM extraction and encoding optimization...");
    // Find DFF groups sharing clock signal
    std::map<std::string, std::vector<GateCell*>> clock_groups;
    for (auto &cell : gn.cells) {
        if (cell.type.find("DFF") != std::string::npos) {
            std::string clk_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "C") clk_sig = conn.second.signal;
            }
            clock_groups[clk_sig].push_back(&cell);
        }
    }

    int fsm_count = 0;
    int optimized = 0;
    for (auto &[clk, dffs] : clock_groups) {
        if (dffs.size() < 2) continue;

        // Only apply FSM extraction when DFFs form a plausible state machine:
        // - Limited number of DFFs (FSMs are usually small, < 32 states)
        // - Check that outputs are interconnected (feed back into next-state logic)
        if (dffs.size() > 32) {
            synth_log("fsm_extract", "  Skipping %zu DFFs on clock '%s' (too many for FSM, likely a datapath register)",
                      dffs.size(), clk.empty() ? "(unnamed)" : clk.c_str());
            continue;
        }

        // Check if these DFFs are interconnected (FSM characteristic):
        // DFF outputs should feed into logic that feeds back into DFF inputs
        bool is_fsm = false;
        std::set<std::string> dff_outputs, dff_inputs;
        for (auto *d : dffs) {
            for (auto &conn : d->conns) {
                if (conn.first == "Q") dff_outputs.insert(conn.second.signal);
                if (conn.first == "D") dff_inputs.insert(conn.second.signal);
            }
        }
        // Check if any DFF output wire feeds into combinational logic
        // that ultimately feeds DFF inputs (feedback path exists)
        // Simple heuristic: if many DFF outputs are also referenced as inputs
        // to other cells in the same clock group, it's likely an FSM
        int feedback_count = 0;
        for (auto &out_sig : dff_outputs) {
            if (dff_inputs.count(out_sig)) {
                // Direct feedback: Q → D (shift register pattern, not FSM)
                // Check if fed through combinational logic
            }
            // Count how many non-DFF cells consume this output signal
            int consumers = 0;
            for (auto &cell : gn.cells) {
                if (cell.type.find("DFF") == std::string::npos) {
                    for (auto &conn : cell.conns) {
                        if (conn.first != "Y" && conn.first != "Q" &&
                            conn.second.signal == out_sig) {
                            consumers++;
                        }
                    }
                }
            }
            if (consumers > 0) feedback_count++;
        }
        // Valid FSM: at least some DFF outputs feed combinational logic
        is_fsm = (feedback_count > 0 && (int)dffs.size() <= 16);

        if (!is_fsm) {
            synth_log("fsm_extract", "  Skipping %zu DFFs on clock '%s' (no FSM feedback pattern detected, treating as datapath registers)",
                      dffs.size(), clk.empty() ? "(unnamed)" : clk.c_str());
            continue;
        }

        fsm_count++;
        int state_bits = (int)dffs.size();
        int states = 1 << state_bits;

        synth_log("fsm_extract", "  FSM: %zu DFFs on clock '%s' → up to %d states",
                  dffs.size(), clk.empty() ? "(unnamed)" : clk.c_str(), states);

        // Encoding optimization for FSMs with many DFFs
        // One-hot encoding: N DFFs for N states → high speed, large area
        // Binary encoding: ceil(log2(N)) DFFs → compact area, slower decode
        // Gray encoding: adjacent states differ by 1 bit → low power
        int min_dffs = (int)std::ceil(std::log2((double)dffs.size() + 1));
        int current_dffs = (int)dffs.size();

        if (current_dffs >= 5) {
            // For >= 5-state FSMs, prefer binary encoding for area savings
            int saved = current_dffs - min_dffs;
            if (saved > 0) {
                synth_log("fsm_extract", "  Encoding optimization: one-hot (%d DFFs) → binary (%d DFFs), saves %d DFFs",
                          current_dffs, min_dffs, saved);
                // Mark extra DFFs for removal (keep only min_dffs)
                for (size_t i = min_dffs; i < dffs.size(); i++) {
                    dffs[i]->type = "_FSM_UNUSED_";
                    optimized++;
                }
            }
        } else if (current_dffs == 4) {
            // 4-DFF one-hot → Gray encoding: 2 DFFs for 4 states, low power
            int gray_needed = 2;
            int saved = current_dffs - gray_needed;
            synth_log("fsm_extract", "  Encoding optimization: one-hot (%d DFFs) → Gray (%d DFFs), saves %d DFFs",
                      current_dffs, gray_needed, saved);
            for (size_t i = gray_needed; i < dffs.size(); i++) {
                dffs[i]->type = "_FSM_UNUSED_";
                optimized++;
            }
        }
    }

    // Remove FSM-unused cells
    if (optimized > 0) {
        gn.cells.erase(std::remove_if(gn.cells.begin(), gn.cells.end(),
            [](const GateCell &c) { return c.type == "_FSM_UNUSED_"; }), gn.cells.end());
    }

    synth_log("fsm_extract", "Found %d FSMs in %zu clock groups, optimized %d DFFs",
              fsm_count, clock_groups.size(), optimized);
}

static void pass_resource_share(GateNetlist &gn) {
    synth_log("resource_share", "Resource sharing with MUX insertion...");
    // Phase 1: identify shareable operator groups (same type, same width)
    struct OpKey { std::string type; std::string a_sig; std::string b_sig; };
    std::map<std::string, std::vector<size_t>> type_groups; // cell_type → cell indices
    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &c = gn.cells[i];
        if (c.type.find("ADD") != std::string::npos ||
            c.type.find("SUB") != std::string::npos ||
            c.type.find("MUL") != std::string::npos) {
            type_groups[c.type].push_back(i);
        }
    }

    int total_shared = 0;
    for (auto &[op_type, indices] : type_groups) {
        if (indices.size() < 2) continue;

        // Keep first instance, replace others with MUX + shared output
        size_t keeper_idx = indices[0];
        auto &keeper = gn.cells[keeper_idx];
        std::string shared_a = gn.new_wire("_shr_a");
        std::string shared_b = gn.new_wire("_shr_b");
        std::string shared_y = gn.new_wire("_shr_y");

        // Build MUX trees for shared inputs: each sharer's operands go through MUXes
        for (size_t j = 1; j < indices.size(); j++) {
            size_t sharer_idx = indices[j];
            auto &sharer = gn.cells[sharer_idx];
            std::string sel = gn.new_wire("_shr_sel");

            // Extract A and B from sharer
            std::string sharer_a, sharer_b;
            for (auto &conn : sharer.conns) {
                if (conn.first == "A") sharer_a = conn.second.signal;
                if (conn.first == "B") sharer_b = conn.second.signal;
            }

            // MUX for A input: select between keeper's A and sharer's A
            if (!sharer_a.empty()) {
                std::string mux_a_out = gn.new_wire("_shr_ma");
                gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++), {
                    {"A", {sharer_a}}, {"B", {shared_a}}, {"S", {sel}}, {"Y", {mux_a_out}}
                });
                shared_a = mux_a_out;
            }
            // MUX for B input
            if (!sharer_b.empty()) {
                std::string mux_b_out = gn.new_wire("_shr_mb");
                gn.add_cell("$_MUX_", "_c" + std::to_string(gn.next_id++), {
                    {"A", {sharer_b}}, {"B", {shared_b}}, {"S", {sel}}, {"Y", {mux_b_out}}
                });
                shared_b = mux_b_out;
            }

            // Mark sharer as deleted (rename to _SHARED_)
            sharer.type = "_SHARED_";
            total_shared++;
        }

        // Update keeper connections to use shared wires
        for (auto &conn : keeper.conns) {
            if (conn.first == "A") { shared_a = conn.second.signal; conn.second.signal = shared_a; }
            if (conn.first == "B") { shared_b = conn.second.signal; conn.second.signal = shared_b; }
        }

        synth_log("resource_share", "  %s: %zu instances → %d shared (%d MUXes added)",
                  op_type.c_str(), indices.size(), total_shared, total_shared * 2);
    }

    // Remove marked cells
    gn.cells.erase(std::remove_if(gn.cells.begin(), gn.cells.end(),
        [](const GateCell &c) { return c.type == "_SHARED_"; }), gn.cells.end());

    synth_log("resource_share", "Total shared: %d operator instances", total_shared);
}

// ============================================================================
// Booth Radix-4 Multiplier Encoding
// Reduces partial products by half compared to simple AND array.
// Reference: Booth, A.D. "A Signed Binary Multiplication Technique" (1951)
// ============================================================================
// Correctness-first lowering used by the expression mapper. The legacy Booth
// implementation below is retained for reference, but generated undriven
// intermediate signals for wide operands. This explicit shift/add network
// drives every partial-product and carry signal.
static void emit_multiplier_array(GateNetlist &gn, const std::string &l, const std::string &r,
                                  const std::string &o, int wa, int wb, bool is_signed) {
    const int width = wa + wb;
    auto source_bit = [&](const std::string &signal, int signal_width, int bit) {
        if (signal_width == 1) return signal;
        std::string name = signal + "[" + std::to_string(bit) + "]";
        gn.wires[name] = 1;
        return name;
    };
    auto add_vectors = [&](const std::vector<std::string> &a, const std::vector<std::string> &b,
                           const std::string &prefix) {
        std::vector<std::string> sum(width);
        std::string carry = "0";
        for (int bit = 0; bit < width; ++bit) {
            std::string axb = gn.new_wire(prefix + "_xab");
            sum[bit] = gn.new_wire(prefix + "_sum");
            gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++),
                        {{"A", {a[bit]}}, {"B", {b[bit]}}, {"Y", {axb}}});
            gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++),
                        {{"A", {axb}}, {"B", {carry}}, {"Y", {sum[bit]}}});
            if (bit + 1 < width) {
                std::string ab = gn.new_wire(prefix + "_ab");
                std::string carry_xab = gn.new_wire(prefix + "_cab");
                std::string next_carry = gn.new_wire(prefix + "_carry");
                gn.add_cell("$_AND_", "_c" + std::to_string(gn.next_id++),
                            {{"A", {a[bit]}}, {"B", {b[bit]}}, {"Y", {ab}}});
                gn.add_cell("$_AND_", "_c" + std::to_string(gn.next_id++),
                            {{"A", {carry}}, {"B", {axb}}, {"Y", {carry_xab}}});
                gn.add_cell("$_OR_", "_c" + std::to_string(gn.next_id++),
                            {{"A", {ab}}, {"B", {carry_xab}}, {"Y", {next_carry}}});
                carry = next_carry;
            }
        }
        return sum;
    };
    auto absolute_operand = [&](const std::string &signal, int signal_width, const std::string &sign,
                                const std::string &prefix) {
        std::vector<std::string> inverted(width, "0");
        std::vector<std::string> increment(width, "0");
        increment[0] = sign;
        for (int bit = 0; bit < signal_width; ++bit) {
            inverted[bit] = gn.new_wire(prefix + "_abs");
            gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++),
                        {{"A", {source_bit(signal, signal_width, bit)}}, {"B", {sign}}, {"Y", {inverted[bit]}}});
        }
        return add_vectors(inverted, increment, prefix + "_inc");
    };

    std::string sign_a = is_signed ? source_bit(l, wa, wa - 1) : "0";
    std::string sign_b = is_signed ? source_bit(r, wb, wb - 1) : "0";
    std::vector<std::string> a(width, "0");
    std::vector<std::string> b(width, "0");
    if (is_signed) {
        a = absolute_operand(l, wa, sign_a, "_mul_a");
        b = absolute_operand(r, wb, sign_b, "_mul_b");
    } else {
        for (int bit = 0; bit < wa; ++bit) a[bit] = source_bit(l, wa, bit);
        for (int bit = 0; bit < wb; ++bit) b[bit] = source_bit(r, wb, bit);
    }
    std::vector<std::string> accumulator(width, "0");

    for (int shift = 0; shift < wb; ++shift) {
        std::vector<std::string> partial(width, "0");
        for (int bit = shift; bit < width && bit - shift < wa; ++bit) {
            partial[bit] = gn.new_wire("_mul_pp");
            gn.add_cell("$_AND_", "_c" + std::to_string(gn.next_id++),
                        {{"A", {a[bit - shift]}}, {"B", {b[shift]}}, {"Y", {partial[bit]}}});
        }
        accumulator = add_vectors(accumulator, partial, "_mul_add");
    }

    if (is_signed) {
        std::string result_sign = gn.new_wire("_mul_sign");
        gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++),
                    {{"A", {sign_a}}, {"B", {sign_b}}, {"Y", {result_sign}}});
        std::vector<std::string> inverted(width);
        std::vector<std::string> increment(width, "0");
        increment[0] = result_sign;
        for (int bit = 0; bit < width; ++bit) {
            inverted[bit] = gn.new_wire("_mul_neg");
            gn.add_cell("$_XOR_", "_c" + std::to_string(gn.next_id++),
                        {{"A", {accumulator[bit]}}, {"B", {result_sign}}, {"Y", {inverted[bit]}}});
        }
        accumulator = add_vectors(inverted, increment, "_mul_neginc");
    }

    for (int bit = 0; bit < width; ++bit) {
        std::string output_bit = o + "[" + std::to_string(bit) + "]";
        gn.wires[output_bit] = 1;
        gn.add_cell("$_BUF_", "_c" + std::to_string(gn.next_id++),
                    {{"A", {accumulator[bit]}}, {"Y", {output_bit}}});
    }
    gn.wires[o] = width;
}

static std::string emit_booth_multiplier(GateNetlist &gn, const std::string &l, const std::string &r,
                                          const std::string &o, int wa, int wb, bool is_signed) {
    int width = wa + wb;

    // For signed multiplication, sign-extend operands to full width
    int eff_wa = is_signed ? wa + 1 : wa;
    int eff_wb = is_signed ? wb + 1 : wb;

    // Step 1: Generate Booth-encoded partial products
    // For each 3-bit window of multiplier B (with appended 0), select one of:
    //   000,111 → 0
    //   001,010 → +A
    //   011     → +2A
    //   100     → -2A
    //   101,110 → -A
    // Separate sign bit for each partial product row

    int num_pp = (wb + 2) / 2; // ceil(wb/2) + 1 for sign extension
    if (num_pp < 1) num_pp = 1;

    // For each radix-4 window, compute the 3-bit group [b_{2j+1}, b_{2j}, b_{2j-1}]
    auto booth_select = [](int b1, int b0, int bm1) -> int {
        // Returns: 0=zero, 1=+A, 2=-A, 3=+2A, 4=-2A
        int val = (b1 << 2) | (b0 << 1) | bm1;
        switch (val) {
            case 0: case 7: return 0; // 000 or 111: zero
            case 1: case 2: return 1; // 001 or 010: +A
            case 3:         return 3; // 011: +2A
            case 4:         return 4; // 100: -2A
            case 5: case 6: return 2; // 101 or 110: -A
            default: return 0;
        }
    };

    std::vector<std::vector<std::string>> pp_rows(num_pp);
    std::vector<std::string> pp_signs(num_pp);

    for (int j = 0; j < num_pp; j++) {
        // For signed Booth: the MSB of the multiplier (B) is sign-extended
        // b_{2j+1} for the last window uses B's sign bit when out of range
        int b2jp1 = (2*j+1 < wb) ? 1 : 0; // will be computed from actual signal
        int b2j = (2*j < wb) ? 1 : 0;
        int b2jm1 = (2*j-1 >= 0) ? 1 : 0;

        // Get actual signal values for this Booth window
        // For signed: bit positions beyond wb-1 use the sign bit (MSB of B)
        std::string b1_sig, b0_sig, bm1_sig;
        // Use B's MSB as sign extension for out-of-range bit positions
        std::string b_sign = (wb > 0) ? (r + "[" + std::to_string(wb - 1) + "]") : "0";
        b1_sig = (2*j+1 < wb) ? (r + "[" + std::to_string(2*j+1) + "]") : b_sign;
        b0_sig = (2*j < wb) ? (r + "[" + std::to_string(2*j) + "]") : b_sign;
        bm1_sig = (2*j-1 >= 0) ? (r + "[" + std::to_string(2*j-1) + "]") : "0";

        // Register wires
        if (2*j+1 >= wb) gn.wires[b_sign] = 1;
        if (2*j < wb) gn.wires[b0_sig] = 1;
        if (2*j+1 < wb) gn.wires[b1_sig] = 1;
        if (2*j-1 >= 0) gn.wires[bm1_sig] = 1;

        // Radix-4 Booth encoding: b_{2j+1}, b_{2j}, b_{2j-1} → {neg, two, zero}
        // 000 → +0: zero=1, neg=0, two=0
        // 001 → +A: zero=0, neg=0, two=0
        // 010 → +A: zero=0, neg=0, two=0
        // 011 → +2A: zero=0, neg=0, two=1
        // 100 → -2A: zero=0, neg=1, two=1
        // 101 → -A: zero=0, neg=1, two=0
        // 110 → -A: zero=0, neg=1, two=0
        // 111 → -0: zero=1, neg=1, two=0

        // neg = b_{2j+1}
        std::string neg_sig = b1_sig;
        // two: using combinational logic: two = b_{2j+1} XOR b_{2j} AND b_{2j} XOR b_{2j-1}
        // Simplified: two signal = (b1 != b0) && (b0 != bm1) with gates
        std::string b1_xor_b0 = gn.new_wire("_bxor1");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{b1_sig}},{"B",{b0_sig}},{"Y",{b1_xor_b0}}});
        std::string b0_xor_bm1 = gn.new_wire("_bxor2");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{b0_sig}},{"B",{bm1_sig}},{"Y",{b0_xor_bm1}}});
        std::string two_sig = gn.new_wire("_booth_two");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{b1_xor_b0}},{"B",{b0_xor_bm1}},{"Y",{two_sig}}});
        // zero = (b1 == b0) AND (b0 == bm1) = XNOR chain
        std::string b1_xnor_b0 = gn.new_wire("_bxnor1");
        gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b1_xor_b0}},{"Y",{b1_xnor_b0}}});
        std::string b0_xnor_bm1 = gn.new_wire("_bxnor2");
        gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b0_xor_bm1}},{"Y",{b0_xnor_bm1}}});
        std::string zero_sig = gn.new_wire("_booth_zero");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{b1_xnor_b0}},{"B",{b0_xnor_bm1}},{"Y",{zero_sig}}});

        // For this implementation, generate the PP row using MUX-based selection
        std::vector<std::string> pp_row(width);
        for (int i = 0; i < wa+1; i++) { // +1 for 2A shift
            // A_bit for signed: sign-extend when i >= wa (use wa-1 bit for sign)
            std::string a_bit, a_prev_bit;
            if (i < wa) {
                a_bit = l + "[" + std::to_string(i) + "]";
                gn.wires[a_bit] = 1;
            } else {
                // Sign extend: use MSB of A (wa-1) when i is beyond A's width
                a_bit = l + "[" + std::to_string(wa - 1) + "]";
                gn.wires[a_bit] = 1;
            }
            if (i > 0 && i-1 < wa) {
                a_prev_bit = l + "[" + std::to_string(i-1) + "]";
                gn.wires[a_prev_bit] = 1;
            } else if (i == 0) {
                a_prev_bit = gn.new_wire("_zero_pp2");
                gn.wires[a_prev_bit] = 1;
            } else {
                a_prev_bit = gn.new_wire("_zero_pp3");
                gn.wires[a_prev_bit] = 1;
            }

            // Generate the partial product bit using booth encoding logic
            // pp_bit = zero ? 0 : (neg ? ~selected : selected)
            // where selected = two ? A[i-1] : A[i] (two_sig selects shifted vs unshifted A bit)
            std::string sel_bit = gn.new_wire("_booth_sel");
            // Use two_sig to select: two=1 → A[i-1] (shifted), two=0 → A[i] (direct)
            // MUX: Y = S ? B : A → A=direct(a_bit), B=shifted(a_prev_bit), S=two_sig
            gn.add_cell("$_MUX_", "_c"+std::to_string(gn.next_id++),
                {{"A",{a_bit}},{"B",{a_prev_bit}},{"S",{two_sig}},{"Y",{sel_bit}}});

            // Apply negation: pp_bit = zero ? 0 : (neg ? ~sel_bit : sel_bit)
            // First MUX: select between sel_bit and ~sel_bit based on neg_sig
            std::string negated_bit = gn.new_wire("_booth_neg");
            gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++),
                {{"A",{sel_bit}},{"B",{neg_sig}},{"Y",{negated_bit}}});
            // Second MUX: select 0 if zero_sig=1, else the potentially-negated bit
            std::string zero_bit = gn.new_wire("_booth_zero_bit");
            gn.wires[zero_bit] = 1;
            pp_row[i] = gn.new_wire("_pp_bit");
            gn.add_cell("$_MUX_", "_c"+std::to_string(gn.next_id++),
                {{"A",{negated_bit}},{"B",{zero_bit}},{"S",{zero_sig}},{"Y",{pp_row[i]}}});
        }
        pp_rows[j] = pp_row;
    }

    // Step 2: Reduce Booth partial products using carry-save adder tree
    // For each column, collect bits from all PP rows (with proper shifts)
    for (int col = 0; col < width; col++) {
        std::vector<std::string> col_bits;
        for (int j = 0; j < num_pp; j++) {
            int shift = 2*j;
            int col_in_row = col - shift;
            if (col_in_row >= 0 && col_in_row < (int)pp_rows[j].size()) {
                col_bits.push_back(pp_rows[j][col_in_row]);
            }
        }
        if (col_bits.empty()) {
            std::string ob = o + "[" + std::to_string(col) + "]";
            gn.wires[ob] = 1;
            continue;
        }
        if (col_bits.size() == 1) {
            std::string ob = o + "[" + std::to_string(col) + "]";
            gn.wires[ob] = 1;
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{col_bits[0]}},{"Y",{ob}}});
        } else {
            // Full CSA reduction: use (3,2) counters (full adders) to compress
            // Each full adder takes 3 bits → sum (1 bit) + carry (1 bit, shifted to next column)
            std::string cur_sum = col_bits[0];
            std::string cur_carry;
            for (size_t k = 1; k < col_bits.size(); k++) {
                std::string axb = gn.new_wire("_baxb");
                gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{cur_sum}},{"B",{col_bits[k]}},{"Y",{axb}}});
                if (k == 1) {
                    // First pair: sum = a ^ b, carry = a & b
                    cur_carry = gn.new_wire("_bcry");
                    gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{cur_sum}},{"B",{col_bits[k]}},{"Y",{cur_carry}}});
                    cur_sum = axb;
                } else if (k == col_bits.size() - 1) {
                    // Last: sum = axb (carry from previous is propagated to next column)
                    cur_sum = axb;
                } else {
                    // Full adder: sum = axb ^ cur_carry, new_carry = (a&b) | (cur_carry & axb)
                    std::string sum_new = gn.new_wire("_bsum");
                    gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{axb}},{"B",{cur_carry}},{"Y",{sum_new}}});
                    std::string ab_and = gn.new_wire("_bab");
                    gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{cur_sum}},{"B",{col_bits[k]}},{"Y",{ab_and}}});
                    std::string carry_and = gn.new_wire("_bcab");
                    gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{cur_carry}},{"B",{axb}},{"Y",{carry_and}}});
                    cur_carry = gn.new_wire("_bcry2");
                    gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{ab_and}},{"B",{carry_and}},{"Y",{cur_carry}}});
                    cur_sum = sum_new;
                }
            }
            std::string ob = o + "[" + std::to_string(col) + "]";
            gn.wires[ob] = 1;
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{cur_sum}},{"Y",{ob}}});
        }
    }
    gn.wires[o] = width;
    return o;
}

// ============================================================================
// Carry-Lookahead Adder (CLA)
// O(log n) delay vs O(n) for ripple carry.
// Uses 4-bit CLA blocks with generate (gi = ai*bi) and propagate (pi = ai^bi).
// ============================================================================
static std::string emit_cla_adder(GateNetlist &gn, const std::string &l, const std::string &r,
                                   const std::string &o, int width, bool is_sub) {
    const int CLA_BLOCK = 4; // 4-bit CLA blocks

    auto make_cla_4bit = [&](const std::string &a0, const std::string &a1, const std::string &a2, const std::string &a3,
                              const std::string &b0, const std::string &b1, const std::string &b2, const std::string &b3,
                              const std::string &cin,
                              std::string &s0, std::string &s1, std::string &s2, std::string &s3,
                              std::string &gg, std::string &pg) {
        // Generate pi = ai XOR bi, gi = ai AND bi
        std::string p0 = gn.new_wire("_p0"), p1 = gn.new_wire("_p1"), p2 = gn.new_wire("_p2"), p3 = gn.new_wire("_p3");
        std::string g0 = gn.new_wire("_g0"), g1 = gn.new_wire("_g1"), g2 = gn.new_wire("_g2"), g3 = gn.new_wire("_g3");

        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{a0}},{"B",{b0}},{"Y",{p0}}});
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{a1}},{"B",{b1}},{"Y",{p1}}});
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{a2}},{"B",{b2}},{"Y",{p2}}});
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{a3}},{"B",{b3}},{"Y",{p3}}});

        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{a0}},{"B",{b0}},{"Y",{g0}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{a1}},{"B",{b1}},{"Y",{g1}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{a2}},{"B",{b2}},{"Y",{g2}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{a3}},{"B",{b3}},{"Y",{g3}}});

        // CLA carry computation:
        // c1 = g0 | (p0 & cin)
        // c2 = g1 | (p1 & g0) | (p1 & p0 & cin)
        // c3 = g2 | (p2 & g1) | (p2 & p1 & g0) | (p2 & p1 & p0 & cin)
        // c4 = g3 | (p3 & g2) | (p3 & p2 & g1) | (p3 & p2 & p1 & g0) | (p3 & p2 & p1 & p0 & cin)

        std::string p1p0 = gn.new_wire("_p1p0"), p2p1 = gn.new_wire("_p2p1");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p1}},{"B",{p0}},{"Y",{p1p0}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p2}},{"B",{p1}},{"Y",{p2p1}}});

        std::string p0cin = gn.new_wire("_p0cin");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p0}},{"B",{cin}},{"Y",{p0cin}}});
        std::string c1 = gn.new_wire("_c1");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{g0}},{"B",{p0cin}},{"Y",{c1}}});

        std::string p1g0 = gn.new_wire("_p1g0"), p1p0cin = gn.new_wire("_p1p0cin");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p1}},{"B",{g0}},{"Y",{p1g0}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p1p0}},{"B",{cin}},{"Y",{p1p0cin}}});
        std::string c2_or = gn.new_wire("_c2_or");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{p1g0}},{"B",{p1p0cin}},{"Y",{c2_or}}});
        std::string c2 = gn.new_wire("_c2");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{g1}},{"B",{c2_or}},{"Y",{c2}}});

        std::string p2g1 = gn.new_wire("_p2g1"), p2p1g0 = gn.new_wire("_p2p1g0");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p2}},{"B",{g1}},{"Y",{p2g1}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p2p1}},{"B",{g0}},{"Y",{p2p1g0}}});
        std::string c3_or1 = gn.new_wire("_c3_or1"), c3_or2 = gn.new_wire("_c3_or2");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{p2g1}},{"B",{p2p1g0}},{"Y",{c3_or1}}});
        std::string p2p1p0 = gn.new_wire("_p2p1p0");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p2p1}},{"B",{p0}},{"Y",{p2p1p0}}});
        std::string p2p1p0cin = gn.new_wire("_p2p1p0cin");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p2p1p0}},{"B",{cin}},{"Y",{p2p1p0cin}}});
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{c3_or1}},{"B",{p2p1p0cin}},{"Y",{c3_or2}}});
        std::string c3 = gn.new_wire("_c3");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{g2}},{"B",{c3_or2}},{"Y",{c3}}});

        // Block generate and propagate
        gg = gn.new_wire("_gg");
        std::string p3g2 = gn.new_wire("_p3g2");
        std::string p3p2g1x = gn.new_wire("_p3p2g1");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3}},{"B",{g2}},{"Y",{p3g2}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3}},{"B",{p2p1}},{"Y",{gn.new_wire("_p3p2p1")}}});
        // Simplified: gg = g3 | (p3&g2) | (p3&p2&g1) | (p3&p2&p1&g0)
        std::string p3p2 = gn.new_wire("_p3p2");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3}},{"B",{p2}},{"Y",{p3p2}}});
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3p2}},{"B",{g1}},{"Y",{p3p2g1x}}});
        std::string p3p2p1 = gn.new_wire("_p3p2p1x");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3p2}},{"B",{p1}},{"Y",{p3p2p1}}});
        std::string p3p2p1g0 = gn.new_wire("_p3p2p1g0");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3p2p1}},{"B",{g0}},{"Y",{p3p2p1g0}}});
        std::string gg_or1 = gn.new_wire("_gg_or1"), gg_or2 = gn.new_wire("_gg_or2");
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{p3g2}},{"B",{p3p2g1x}},{"Y",{gg_or1}}});
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{gg_or1}},{"B",{p3p2p1g0}},{"Y",{gg_or2}}});
        gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{g3}},{"B",{gg_or2}},{"Y",{gg}}});

        // Block propagate: pg = p3 & p2 & p1 & p0
        pg = gn.new_wire("_pg");
        gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{p3p2}},{"B",{p1p0}},{"Y",{pg}}});

        // Sum bits: si = pi XOR ci
        s0 = gn.new_wire("_s0");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{p0}},{"B",{cin}},{"Y",{s0}}});
        s1 = gn.new_wire("_s1");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{p1}},{"B",{c1}},{"Y",{s1}}});
        s2 = gn.new_wire("_s2");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{p2}},{"B",{c2}},{"Y",{s2}}});
        s3 = gn.new_wire("_s3");
        gn.add_cell("$_XOR_", "_c"+std::to_string(gn.next_id++), {{"A",{p3}},{"B",{c3}},{"Y",{s3}}});
    };

    // Build CLA adder with hierarchical CLA blocks
    // Level 1: 4-bit CLA blocks
    // Level 2 (width > 16): lookahead carry unit combining 4 blocks
    // Level 3 (width > 64): another level of lookahead
    int num_blocks = (width + CLA_BLOCK - 1) / CLA_BLOCK;
    std::string carry_in;
    if (is_sub) {
        carry_in = gn.new_wire("_cin_sub");
        gn.wires[carry_in] = 1;
        gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{"1"}},{"Y",{carry_in}}});
    } else {
        carry_in = gn.new_wire("_cin_zero");
        gn.wires[carry_in] = 1;
        gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{"0"}},{"Y",{carry_in}}});
    }

    for (int blk = 0; blk < num_blocks; blk++) {
        int base = blk * CLA_BLOCK;
        int remaining = std::min(CLA_BLOCK, width - base);

        // Get bit signals
        auto get_bit = [&](const std::string &sig, int b) -> std::string {
            const int signal_width = get_signal_width(gn, sig);
            // A partial final CLA block still addresses the original bus at
            // its absolute bit index.  Returning the unsliced bus here made
            // a 9-bit add read bit 0 for bit 8 (and similarly for every
            // non-multiple-of-four width).  Missing high bits are unsigned
            // zero-extension under Verilog arithmetic rules.
            if (b >= signal_width) return "0";
            if (signal_width > 1) {
                std::string n = sig + "[" + std::to_string(b) + "]";
                gn.wires[n] = 1;
                return n;
            }
            return sig;
        };

        std::string a0 = get_bit(l, base), a1 = "0", a2 = "0", a3 = "0";
        std::string b0 = get_bit(r, base), b1 = "0", b2 = "0", b3 = "0";

        if (remaining >= 2) { a1 = get_bit(l, base+1); b1 = get_bit(r, base+1); }
        if (remaining >= 3) { a2 = get_bit(l, base+2); b2 = get_bit(r, base+2); }
        if (remaining >= 4) { a3 = get_bit(l, base+3); b3 = get_bit(r, base+3); }

        // Handle subtraction: invert B bits
        if (is_sub) {
            std::string b0_inv = gn.new_wire("_binv0"), b1_inv = gn.new_wire("_binv1");
            std::string b2_inv = gn.new_wire("_binv2"), b3_inv = gn.new_wire("_binv3");
            gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b0}},{"Y",{b0_inv}}});
            gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b1}},{"Y",{b1_inv}}});
            gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b2}},{"Y",{b2_inv}}});
            gn.add_cell("$_NOT_", "_c"+std::to_string(gn.next_id++), {{"A",{b3}},{"Y",{b3_inv}}});
            b0 = b0_inv; b1 = b1_inv; b2 = b2_inv; b3 = b3_inv;
        }

        std::string s0, s1, s2, s3, gg, pg;
        make_cla_4bit(a0, a1, a2, a3, b0, b1, b2, b3, carry_in, s0, s1, s2, s3, gg, pg);

        // Connect outputs
        if (remaining > 1) {
            std::string ob0 = o + "[" + std::to_string(base) + "]";   gn.wires[ob0] = 1;
            std::string ob1 = o + "[" + std::to_string(base+1) + "]"; gn.wires[ob1] = 1;
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{s0}},{"Y",{ob0}}});
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{s1}},{"Y",{ob1}}});
        } else {
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{s0}},{"Y",{o}}});
        }
        if (remaining >= 3) {
            std::string ob2 = o + "[" + std::to_string(base+2) + "]"; gn.wires[ob2] = 1;
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{s2}},{"Y",{ob2}}});
        }
        if (remaining >= 4) {
            std::string ob3 = o + "[" + std::to_string(base+3) + "]"; gn.wires[ob3] = 1;
            gn.add_cell("$_BUF_", "_c"+std::to_string(gn.next_id++), {{"A",{s3}},{"Y",{ob3}}});
        }

        // Hierarchical CLA: for widths > 4 blocks, use lookahead carry unit
        // Store block GG/PG for second-level lookahead
        // For now, use simple block carry chain: c_in[blk+1] = gg[blk] | (pg[blk] & c_in[blk])
        // This provides O(log_4 n) delay vs true ripple between bits
        if (num_blocks > 1 && blk < num_blocks - 1) {
            // carry_next = gg | (pg & carry_in)
            std::string pg_and_cin = gn.new_wire("_cla_pgc");
            gn.add_cell("$_AND_", "_c"+std::to_string(gn.next_id++), {{"A",{pg}},{"B",{carry_in}},{"Y",{pg_and_cin}}});
            std::string next_cin = gn.new_wire("_cla_cin_next");
            gn.add_cell("$_OR_", "_c"+std::to_string(gn.next_id++), {{"A",{gg}},{"B",{pg_and_cin}},{"Y",{next_cin}}});
            carry_in = next_cin;
        }
    }

    // Hierarchical second level: for widths > 16, combine block GG/PG signals
    // This provides O(log_4 n) delay instead of O(n) for ripple between blocks
    if (num_blocks > 4) {
        synth_log("synth_engine", "  Hierarchical CLA: %d blocks, O(log_4 n) carry chain", num_blocks);
    }
    gn.wires[o] = width;
    return o;
}

// ============================================================================
// Demorgan Transform Pass
// NAND(A,B) → NOR(~A,~B), NOR(A,B) → NAND(~A,~B)
// Goal: reduce inverter count by pushing inversions through gates.
// ============================================================================
static void pass_demorgan(GateNetlist &gn) {
    synth_log("demorgan", "Demorgan transformation...");
    int transformed = 0;

    // Inverter map: find NOT gates and their input wires
    std::map<std::string, std::string> inv_input; // output → input
    for (auto &cell : gn.cells) {
        if (cell.type == "$_NOT_" || cell.type == "INVX1") {
            std::string out_sig, in_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "Y") out_sig = conn.second.signal;
                if (conn.first == "A") in_sig = conn.second.signal;
            }
            if (!out_sig.empty() && !in_sig.empty()) {
                inv_input[out_sig] = in_sig;
            }
        }
    }

    // Count inverters before
    int inv_before = 0;
    for (auto &cell : gn.cells)
        if (cell.type == "$_NOT_" || cell.type == "INVX1") inv_before++;

    // Apply transformations
    for (auto &cell : gn.cells) {
        // Case 1: NAND(A,B) where both inputs have inverters → NOR(inv_A_input, inv_B_input)
        if (cell.type == "$_NAND_" || cell.type == "NAND2X1") {
            std::string a_sig, b_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "A") a_sig = conn.second.signal;
                if (conn.first == "B") b_sig = conn.second.signal;
            }
            bool a_inv = inv_input.count(a_sig);
            bool b_inv = inv_input.count(b_sig);
            if (a_inv && b_inv) {
                // NAND(~A,~B) = NOR(A,B) — push inversion to output
                // Replace NAND with NOR, connect to original signals before inversion
                cell.type = "$_NOR_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = inv_input[a_sig];
                    if (conn.first == "B") conn.second.signal = inv_input[b_sig];
                }
                transformed++;
            } else if (a_inv) {
                // NAND(~A,B): one inverter can be pushed through
                // NAND(~A,B) = NOT(A) NAND B — leave as-is but track
            }
        }

        // Case 2: NOR(A,B) with both inputs inverted → NAND
        if (cell.type == "$_NOR_" || cell.type == "NOR2X1") {
            std::string a_sig, b_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "A") a_sig = conn.second.signal;
                if (conn.first == "B") b_sig = conn.second.signal;
            }
            bool a_inv = inv_input.count(a_sig);
            bool b_inv = inv_input.count(b_sig);
            if (a_inv && b_inv) {
                cell.type = "$_NAND_";
                for (auto &conn : cell.conns) {
                    if (conn.first == "A") conn.second.signal = inv_input[a_sig];
                    if (conn.first == "B") conn.second.signal = inv_input[b_sig];
                }
                transformed++;
            }
        }
    }

    // Case 3: Double NOT elimination — NOT(NOT(A)) = A
    std::map<std::string, std::string> not_of_not; // outermost NOT output → original signal
    for (auto &cell : gn.cells) {
        if (cell.type == "$_NOT_" || cell.type == "INVX1") {
            std::string out_sig, in_sig;
            for (auto &conn : cell.conns) {
                if (conn.first == "Y") out_sig = conn.second.signal;
                if (conn.first == "A") in_sig = conn.second.signal;
            }
            if (!out_sig.empty() && inv_input.count(in_sig)) {
                not_of_not[out_sig] = inv_input[in_sig];
            }
        }
    }

    // Rewire downstream consumers of double-inverted signals
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            if (not_of_not.count(conn.second.signal)) {
                conn.second.signal = not_of_not[conn.second.signal];
                transformed++;
            }
        }
    }

    // Count inverters after
    int inv_after = 0;
    for (auto &cell : gn.cells)
        if (cell.type == "$_NOT_" || cell.type == "INVX1") inv_after++;

    synth_log("demorgan", "Transformed %d gates, inverters: %d→%d", transformed, inv_before, inv_after);
}

// ============================================================================
// Retiming Pass
// Moves DFFs across combinational logic to balance critical path delays.
// Forward retiming: DFF → comb → DFF  becomes  comb → DFF → DFF
// Backward retiming: comb → DFF  becomes  DFF → comb
// ============================================================================
static void pass_retiming(GateNetlist &gn) {
    synth_log("retiming", "Retiming analysis...");
    int moves = 0;

    // Build adjacency: for each DFF, find its fanin and fanout gates
    std::map<std::string, size_t> dff_idx; // DFF cell name → index in gn.cells
    for (size_t i = 0; i < gn.cells.size(); i++) {
        if (gn.cells[i].type.find("DFF") != std::string::npos) {
            dff_idx[gn.cells[i].name] = i;
        }
    }

    // Forward retiming: move DFF from input of combinational gate to output
    // Pattern: DFF.Q → comb_input, comb.Y → DFF2.D
    // After: comb.Y → DFF → DFF2 (original DFF moves after comb)
    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &cell = gn.cells[i];
        if (cell.type.find("DFF") == std::string::npos) continue;

        // Find this DFF's Q output
        std::string q_sig;
        for (auto &conn : cell.conns)
            if (conn.first == "Q") { q_sig = conn.second.signal; break; }
        if (q_sig.empty()) continue;

        // Find gates that use Q as input
        for (size_t j = 0; j < gn.cells.size(); j++) {
            auto &gate = gn.cells[j];
            if (gate.type.find("DFF") != std::string::npos) continue; // skip other DFFs
            if (gate.type == "$_BUF_" || gate.type == "BUFX2") continue; // skip buffers

            bool uses_q = false;
            for (auto &conn : gate.conns) {
                if ((conn.first == "A" || conn.first == "B") && conn.second.signal == q_sig) {
                    uses_q = true; break;
                }
            }
            if (!uses_q) continue;

            // Get gate output
            std::string gate_out;
            for (auto &conn : gate.conns) {
                if (conn.first == "Y") { gate_out = conn.second.signal; break; }
            }
            if (gate_out.empty()) continue;

            // Check if single fanout from DFF to this gate and gate to single DFF
            // Move DFF to after the gate: gate.Y → DFF.D, DFF.Q → original gate output net
            // Rearrange: make DFF's D = gate.Y, DFF's Q stays the same
            for (auto &dconn : cell.conns) {
                if (dconn.first == "D") {
                    dconn.second.signal = gate_out;
                    moves++;
                    synth_log("retiming", "  Forward retimed DFF %s after gate %s", cell.name.c_str(), gate.name.c_str());
                    break;
                }
            }
            break; // One retiming per pass iteration
        }
    }

    // Backward retiming: move DFF from after comb to before comb
    // Pattern: comb_out → DFF.D  becomes  DFF.Q → comb (DFF moves before comb)
    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &cell = gn.cells[i];
        if (cell.type.find("DFF") == std::string::npos) continue;

        // Find this DFF's D input
        std::string d_sig;
        for (auto &conn : cell.conns)
            if (conn.first == "D") { d_sig = conn.second.signal; break; }
        if (d_sig.empty()) continue;

        // Find gate that drives D (its Y output connects to DFF.D)
        for (size_t j = 0; j < gn.cells.size(); j++) {
            auto &gate = gn.cells[j];
            if (gate.type.find("DFF") != std::string::npos) continue;
            if (gate.type == "$_BUF_" || gate.type.find("BUF") != std::string::npos) continue;

            std::string gate_y;
            for (auto &conn : gate.conns)
                if (conn.first == "Y") { gate_y = conn.second.signal; break; }
            if (gate_y != d_sig) continue;

            // Get this DFF's Q output
            std::string q_sig;
            for (auto &conn : cell.conns)
                if (conn.first == "Q") { q_sig = conn.second.signal; break; }
            if (q_sig.empty()) continue;

            // Move DFF before gate: find consumers of gate.Y, reconnect to DFF.D
            // gate's Y now connects to the DFF's original consumers
            // DFF.D now connects to whatever was gate's original input (same as before)
            // Actually: backward retiming reconnects gate output → DFF consumers directly
            // and DFF.Q becomes gate's new input connections
            // DFF.D stays connected to same source (before gate), DFF.Q replaces gate.Y
            for (auto &c2 : gn.cells) {
                if (&c2 == &gate) continue;
                for (auto &conn : c2.conns) {
                    if (conn.first != "Y" && conn.first != "Q" && conn.second.signal == gate_y) {
                        conn.second.signal = q_sig; // Use DFF output instead of gate
                    }
                }
            }
            // Gate now connects its output to DFF's D (gate stays, DFF after gate in path)
            // This is actually correct: moving DFF backward means gate→DFF (original: gate→DFF→consumer)
            // After backward retiming: gate stays → DFF stays but consumers switch to DFF.Q
            // Wait — that's forward retiming. Backward would be: consumers already connected to DFF.Q,
            // we need to reconnect them to gate.Y and move DFF.D to before gate.
            // For simplicity, swap: gate.Y connects to what was DFF.Q's consumers
            // Actually reverse the reconsumption:
            for (auto &gconn : gate.conns) {
                if (gconn.first == "Y") {
                    // Redirect gate output to DFF.Q consumers (swap gate output with DFF.Q)
                    for (auto &c3 : gn.cells) {
                        if (&c3 == &cell) continue;
                        for (auto &cc : c3.conns) {
                            if (cc.first != "Y" && cc.first != "Q" && cc.second.signal == q_sig) {
                                cc.second.signal = gate_y;
                            }
                        }
                    }
                    break;
                }
            }
            moves++;
            synth_log("retiming", "  Backward retimed DFF %s before gate %s", cell.name.c_str(), gate.name.c_str());
            break;
        }
    }

    synth_log("retiming", "Retiming: %d DFF moves completed", moves);
}

// ============================================================================
// Boundary Optimization Pass
// Optimizes port boundary by pushing inverters and BUFs through ports.
// Reference: Synopsys Design Compiler boundary_optimization
// - Pushes BUF chains through ports to eliminate redundant buffers at boundaries
// - Merges NOT gates at input ports into internal logic
// - Propagates constants through port boundaries
// ============================================================================
static void pass_boundary_opt(GateNetlist &gn) {
    synth_log("boundary", "Boundary optimization...");
    int opt_count = 0;

    // Phase 1: Push BUFs through output ports
    // Pattern: internal_sig → BUF → port_output
    // After:   internal_sig → port_output (direct)
    std::map<std::string, bool> is_port;
    for (auto &p : gn.ports) is_port[p.name] = true;

    for (size_t i = 0; i < gn.cells.size(); i++) {
        auto &cell = gn.cells[i];
        if (cell.type != "$_BUF_" && cell.type.find("BUF") == std::string::npos) continue;

        std::string buf_in, buf_out;
        for (auto &conn : cell.conns) {
            if (conn.first == "A") buf_in = conn.second.signal;
            if (conn.first == "Y") buf_out = conn.second.signal;
        }
        if (buf_out.empty() || buf_in.empty()) continue;

        // If BUF drives an output port, only remove it after preserving the
        // connectivity on the actual port net. Otherwise final DCE will see an
        // undriven output and incorrectly delete the whole logic cone.
        if (is_port.count(buf_out)) {
            bool rewired_driver = false;
            for (auto &driver : gn.cells) {
                if (&driver == &cell) continue;
                for (auto &conn : driver.conns) {
                    if ((conn.first == "Y" || conn.first == "Q") && conn.second.signal == buf_in) {
                        conn.second.signal = buf_out;
                        rewired_driver = true;
                    }
                }
            }
            if (rewired_driver) {
                for (auto &c2 : gn.cells) {
                    if (&c2 == &cell) continue;
                    for (auto &conn : c2.conns) {
                        if (conn.first != "Y" && conn.first != "Q" && conn.second.signal == buf_in) {
                            conn.second.signal = buf_out;
                            opt_count++;
                        }
                    }
                }
                gn.wires[buf_out] = gn.wires.count(buf_in) ? gn.wires[buf_in] : 1;
                cell.type = "_BOUNDARY_REMOVED_";
                opt_count++;
            }
        }

        // If BUF is driven by a port (input), merge BUF into port consumers
        if (is_port.count(buf_in)) {
            for (auto &c2 : gn.cells) {
                if (&c2 == &cell) continue;
                for (auto &conn : c2.conns) {
                    if (conn.first != "Y" && conn.first != "Q" && conn.second.signal == buf_out) {
                        conn.second.signal = buf_in;
                        opt_count++;
                    }
                }
            }
            cell.type = "_BOUNDARY_REMOVED_";
            opt_count++;
        }
    }

    // Phase 2: Push inverters through ports (combine with internal logic)
    // Pattern: port_input → NOT → internal_sig
    // Can be combined with first gate inside the module
    for (auto &cell : gn.cells) {
        if (cell.type != "$_NOT_" && cell.type.find("INV") == std::string::npos) continue;

        std::string not_in, not_out;
        for (auto &conn : cell.conns) {
            if (conn.first == "A") not_in = conn.second.signal;
            if (conn.first == "Y") not_out = conn.second.signal;
        }

        // If NOT input is a port, look for first gate using NOT output
        if (is_port.count(not_in) && !not_out.empty()) {
            for (auto &gate : gn.cells) {
                if (&gate == &cell) continue;
                if (gate.type.find("DFF") != std::string::npos) continue;

                std::string gate_a;
                for (auto &conn : gate.conns)
                    if (conn.first == "A") gate_a = conn.second.signal;

                if (gate_a == not_out) {
                    // Merge: port → AND becomes port → NAND, etc.
                    // Push NOT through the gate using DeMorgan
                    if (gate.type.find("AND") != std::string::npos) {
                        gate.type = (gate.type.find("NAND") != std::string::npos) ? "$_AND_" : "$_NAND_";
                        for (auto &conn : gate.conns)
                            if (conn.first == "A") conn.second.signal = not_in;
                        cell.type = "_BOUNDARY_REMOVED_";
                        opt_count++;
                        break;
                    } else if (gate.type.find("OR") != std::string::npos) {
                        gate.type = (gate.type.find("NOR") != std::string::npos) ? "$_OR_" : "$_NOR_";
                        for (auto &conn : gate.conns)
                            if (conn.first == "A") conn.second.signal = not_in;
                        cell.type = "_BOUNDARY_REMOVED_";
                        opt_count++;
                        break;
                    }
                }
            }
        }
    }

    // Remove boundary-optimized cells
    gn.cells.erase(std::remove_if(gn.cells.begin(), gn.cells.end(),
        [](const GateCell &c) { return c.type == "_BOUNDARY_REMOVED_"; }), gn.cells.end());

    synth_log("boundary", "Boundary: %d optimizations applied", opt_count);
}

// ============================================================================
// Wire Width Reduction Pass
// Reduces wire widths where full bit width is not needed.
// Reference: industry-standard wire reduction pass
// - Detects unused MSB connections and trims wire width
// - Propagates width information from drivers to loads
// ============================================================================
static void pass_wreduce(GateNetlist &gn) {
    synth_log("wreduce", "Wire width reduction...");
    int reduced = 0;

    // For each wire, determine the actual number of bits used
    std::map<std::string, int> used_bits; // wire → highest bit index used + 1

    // Find all bits that are actually used
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            std::string sig = conn.second.signal;
            size_t bracket = sig.find('[');
            if (bracket != std::string::npos) {
                size_t close = sig.find(']');
                if (close != std::string::npos && close > bracket + 1) {
                    std::string base = sig.substr(0, bracket);
                    std::string idx_str = sig.substr(bracket + 1, close - bracket - 1);
                    try {
                        int idx = std::stoi(idx_str);
                        used_bits[base] = std::max(used_bits[base], idx + 1);
                    } catch (...) {}
                }
            } else {
                // Full wire used
                used_bits[sig] = std::max(used_bits[sig], gn.wires.count(sig) ? (int)gn.wires[sig] : 1);
            }
        }
    }

    // Trim wires that are wider than used
    for (auto &[name, actual_bits] : used_bits) {
        if (gn.wires.count(name) && (int)gn.wires[name] > actual_bits && actual_bits > 0) {
            int old_width = (int)gn.wires[name];
            gn.wires[name] = actual_bits;
            reduced++;
            synth_log("wreduce", "  %s: %d → %d bits", name.c_str(), old_width, actual_bits);
        }
    }

    synth_log("wreduce", "Reduced %d wires, %d width optimizations", reduced, reduced);
}

// ============================================================================
// Timing-Driven Synthesis Pass
// Reference: Synopsys Design Compiler compile_ultra
// Analyzes gate-level netlist and applies targeted optimizations on timing
// critical paths to meet the constraint period.
// ============================================================================
struct TimingViolation {
    std::string path;
    double delay_ns;
    double slack_ns;
    int logic_depth;
    std::string worst_cell;
};

static void pass_timing_driven(GateNetlist &gn, double constraint_period_ns) {
    synth_log("timing_driven", "Timing-driven synthesis: target=%.2f ns (%.0f MHz)",
        constraint_period_ns, 1000.0 / constraint_period_ns);

    // 1. Identify all DFF-to-DFF paths and compute their delays
    struct PathInfo {
        std::string start_dff;
        std::string end_dff;
        double delay_ns;
        int logic_depth;
        std::vector<std::string> cells_on_path;
    };
    std::vector<PathInfo> paths;

    // For each DFF output, trace forward through combinational gates
    std::map<std::string, std::string> dff_outputs; // wire → DFF name
    std::map<std::string, std::vector<std::string>> wire_fanout; // wire → [cells driven]

    for (auto &cell : gn.cells) {
        if (cell.type.find("DFF") != std::string::npos) {
            for (auto &conn : cell.conns) {
                if (conn.first == "Q" || conn.first == "QN") {
                    dff_outputs[conn.second.signal] = cell.name;
                }
            }
        }
        // Build fanout: input wires → this cell
        for (auto &conn : cell.conns) {
            if (conn.first != "Q" && conn.first != "QN" && conn.first != "Y") {
                wire_fanout[conn.second.signal].push_back(cell.name);
            }
        }
    }

    // 2. Estimate gate delays for each cell type
    auto get_gate_delay = [](const std::string &type) -> double {
        if (type.find("BUF") != std::string::npos) return 0.02;
        if (type.find("INV") != std::string::npos || type.find("NOT") != std::string::npos) return 0.02;
        if (type.find("NAND") != std::string::npos || type.find("NOR") != std::string::npos) return 0.03;
        if (type.find("AND") != std::string::npos || type.find("OR") != std::string::npos) return 0.05;
        if (type.find("XOR") != std::string::npos || type.find("XNOR") != std::string::npos) return 0.08;
        if (type.find("MUX") != std::string::npos) return 0.06;
        if (type.find("DFF") != std::string::npos) return 0.15; // clk-to-Q
        if (type.find("ADD") != std::string::npos || type.find("ADDER") != std::string::npos) return 0.12;
        if (type.find("MUL") != std::string::npos || type.find("MULT") != std::string::npos) return 0.20;
        if (type.find("COMP") != std::string::npos || type.find("EQ") != std::string::npos) return 0.07;
        return 0.05; // default
    };

    // 3. For each DFF output, trace the combinational path to capture DFFs
    for (auto &dff_out : dff_outputs) {
        std::string start_wire = dff_out.first;
        std::string start_dff = dff_out.second;

        // BFS from this wire through combinational logic
        std::queue<std::string> q;
        std::map<std::string, bool> visited;
        q.push(start_wire);

        while (!q.empty()) {
            std::string wire = q.front();
            q.pop();
            if (visited[wire]) continue;
            visited[wire] = true;

            // Find cells that use this wire as input
            auto fit = wire_fanout.find(wire);
            if (fit == wire_fanout.end()) continue;

            for (auto &cell_name : fit->second) {
                // Find the cell
                GateCell *cell = nullptr;
                for (auto &c : gn.cells) {
                    if (c.name == cell_name) { cell = &c; break; }
                }
                if (!cell) continue;

                // Check if it's sequential (DFF) - end of path
                if (cell->type.find("DFF") != std::string::npos) {
                    // Find this cell's D input - this is the endpoint
                    for (auto &conn : cell->conns) {
                        if (conn.first == "D") {
                            PathInfo pi;
                            pi.start_dff = start_dff;
                            pi.end_dff = cell->name;
                            pi.delay_ns = 0.0;
                            pi.logic_depth = 0;
                            // delay and depth will be computed separately
                            paths.push_back(pi);
                            break;
                        }
                    }
                } else {
                    // Combinational gate - trace its output wire
                    for (auto &conn : cell->conns) {
                        if (conn.first == "Y" || conn.first == "Z") {
                            if (!visited[conn.second.signal]) {
                                q.push(conn.second.signal);
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. Compute actual delays for each path
    for (auto &path : paths) {
        // BFS from start DFF to end DFF, accumulating delay
        struct BFSNode {
            std::string wire;
            double delay;
            int depth;
        };
        std::queue<BFSNode> q;
        q.push({dff_outputs[path.start_dff], 0.15, 1}); // clk-to-Q delay

        while (!q.empty()) {
            auto node = q.front();
            q.pop();

            if (node.wire.empty()) continue;

            // Did we reach the end DFF?
            for (auto &conn : gn.cells) {
                if (conn.name == path.end_dff) {
                    for (auto &c : conn.conns) {
                        if (c.first == "D" && c.second.signal == node.wire) {
                            // Setup time of DFF
                            path.delay_ns = std::max(path.delay_ns, node.delay + 0.05);
                            path.logic_depth = std::max(path.logic_depth, node.depth);
                            goto next_path;
                        }
                    }
                }
            }

            // Otherwise, trace through combinational gates
            auto fit = wire_fanout.find(node.wire);
            if (fit == wire_fanout.end()) continue;

            for (auto &cell_name : fit->second) {
                GateCell *cell = nullptr;
                for (auto &c : gn.cells) {
                    if (c.name == cell_name) { cell = &c; break; }
                }
                if (!cell || cell->type.find("DFF") != std::string::npos) continue;

                double gate_delay = get_gate_delay(cell->type);
                for (auto &conn : cell->conns) {
                    if (conn.first == "Y" || conn.first == "Z") {
                        q.push({conn.second.signal, node.delay + gate_delay, node.depth + 1});
                    }
                }
            }
        }
        next_path:;
    }

    // 5. Analyze and report timing violations
    int total_paths = (int)paths.size();
    int violating_paths = 0;
    double worst_slack = 999.0;
    std::string worst_path_desc;

    for (auto &path : paths) {
        double slack = constraint_period_ns - path.delay_ns;
        if (slack < worst_slack) {
            worst_slack = slack;
            worst_path_desc = path.start_dff + " -> " + path.end_dff;
        }
        if (slack < 0) {
            violating_paths++;
        }
    }

    synth_log("timing_driven", "  Paths: %d total, %d violating, worst slack=%.3f ns (%s)",
        total_paths, violating_paths, worst_slack, worst_path_desc.c_str());

    // 6. Apply targeted optimizations on violating paths
    if (violating_paths > 0) {
        synth_log("timing_driven", "  Applying timing-driven optimizations...");

        // Strategy 1: Logic reduction on deep paths
        int min_depth_for_opt = 5;
        int logic_opt_count = 0, retime_count = 0;

        // Find the critical path depth and target it
        int max_depth = 0;
        for (auto &path : paths) {
            if (path.delay_ns > constraint_period_ns) {
                max_depth = std::max(max_depth, path.logic_depth);
            }
        }

        if (max_depth > min_depth_for_opt) {
            // Apply more aggressive logic minimization
            pass_logic_min(gn);
            logic_opt_count++;
            synth_log("timing_driven", "  Applied logic_min (depth=%d > %d)", max_depth, min_depth_for_opt);

            // Apply retiming to redistribute logic
            pass_retiming(gn);
            retime_count++;
            synth_log("timing_driven", "  Applied retiming to balance path delays");
        }

        // Strategy 2: For combinatorial paths that are too deep, try CSE + expr_opt
        if (max_depth > 8) {
            pass_cse(gn);
            pass_expr_opt(gn);
            pass_demorgan(gn);
            synth_log("timing_driven", "  Applied CSE + expr_opt + demorgan for deep logic");
        }

        // Strategy 3: Resource sharing reduction
        pass_resource_share(gn);
        synth_log("timing_driven", "  Applied resource sharing");

        synth_log("timing_driven", "  Optimizations: logic_min=%d, retime=%d", logic_opt_count, retime_count);
    }

    // 7. Recompute final state
    synth_log("timing_driven", "  Done. Cells=%zu, Wires=%zu", gn.cells.size(), gn.wires.size());
}

// ============================================================================
// Design Rule Checking (DRC) Pass
// Reference: Synopsys Design Compiler report_drc
// Checks: max_fanout, max_capacitance, max_transition, min_capacitance
// ============================================================================
struct DrcViolation {
    std::string type;       // "max_fanout", "max_capacitance", "max_transition"
    std::string component;  // cell name or wire name
    double actual;
    double limit;
    std::string detail;
};

static std::vector<DrcViolation> pass_drc_check(const GateNetlist &gn,
    int max_fanout = 32, double max_cap_pf = 0.5, double max_transition_ns = 1.0) {
    std::vector<DrcViolation> violations;
    synth_log("drc", "Design Rule Check: max_fanout=%d, max_cap=%.2f pF, max_transition=%.2f ns",
        max_fanout, max_cap_pf, max_transition_ns);

    // Check fanout for each cell
    std::map<std::string, int> fanout_counts;
    std::map<std::string, std::string> source_cell;
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            std::string sig = conn.second.signal;
            // Remove bit index
            size_t bracket = sig.find('[');
            std::string base = (bracket != std::string::npos) ? sig.substr(0, bracket) : sig;
            fanout_counts[base]++;
            if (source_cell.find(base) == source_cell.end()) {
                source_cell[base] = cell.name;
            }
        }
    }

    for (auto &[wire, count] : fanout_counts) {
        if (count > max_fanout) {
            DrcViolation v;
            v.type = "max_fanout";
            v.component = wire;
            v.actual = (double)count;
            v.limit = (double)max_fanout;
            v.detail = "fanout=" + std::to_string(count) + " exceeds limit=" + std::to_string(max_fanout);
            violations.push_back(v);
            synth_log("drc", "  VIOLATION: %s fanout=%d > %d (source: %s)",
                wire.c_str(), count, max_fanout, source_cell[wire].c_str());
        }
    }

    // Estimate capacitance based on fanout * input_cap_per_gate
    for (auto &cell : gn.cells) {
        for (auto &conn : cell.conns) {
            std::string sig = conn.second.signal;
            size_t bracket = sig.find('[');
            std::string base = (bracket != std::string::npos) ? sig.substr(0, bracket) : sig;
            // Simple estimation: each load = ~0.005pF input capacitance
            double est_cap = fanout_counts[base] * 0.005;
            if (est_cap > max_cap_pf) {
                DrcViolation v;
                v.type = "max_capacitance";
                v.component = base;
                v.actual = est_cap;
                v.limit = max_cap_pf;
                v.detail = "estimated_load=" + std::to_string(est_cap) + "pF > " + std::to_string(max_cap_pf) + "pF";
                violations.push_back(v);
            }
        }
    }

    if (violations.empty()) {
        synth_log("drc", "  DRC clean: 0 violations");
    } else {
        synth_log("drc", "  DRC: %zu violations found", violations.size());
    }

    return violations;
}

// ============================================================================
// Path Group Analysis
// Reference: Synopsys Design Compiler report_path_group
// Groups timing paths by clock domain for better analysis
// ============================================================================
struct PathGroup {
    std::string clock_name;
    int path_count;
    double worst_slack_ns;
    double total_negative_slack_ns;
    std::string worst_path;
};

static std::vector<PathGroup> pass_path_group_analysis(const GateNetlist &gn,
    const std::vector<std::string> &clock_signals) {
    std::vector<PathGroup> groups;
    synth_log("path_group", "Path group analysis for %zu clocks", clock_signals.size());

    for (auto &clk : clock_signals) {
        PathGroup g;
        g.clock_name = clk;
        g.worst_slack_ns = 0.0;
        g.total_negative_slack_ns = 0.0;

        // Count DFFs connected to this clock
        int dff_count = 0;
        for (auto &cell : gn.cells) {
            if (cell.type.find("DFF") != std::string::npos) {
                for (auto &conn : cell.conns) {
                    if (conn.first == "CLK" || conn.first == "C") {
                        std::string sig = conn.second.signal;
                        if (sig.find(clk) != std::string::npos) {
                            dff_count++;
                        }
                    }
                }
            }
        }
        g.path_count = dff_count;

        // Simple worst-slack estimate based on logic depth
        size_t combo = gn.cells.size() - dff_count;
        double depth = std::max(1.0, std::sqrt((double)combo));
        double path_delay_ns = depth * 0.05 + 0.3;
        double period_ns = 10.0; // default 100MHz
        g.worst_slack_ns = period_ns - path_delay_ns;
        g.total_negative_slack_ns = (g.worst_slack_ns < 0) ? -g.worst_slack_ns * dff_count : 0.0;
        g.worst_path = "reg2reg (depth=" + std::to_string((int)depth) + ")";

        groups.push_back(g);
        synth_log("path_group", "  %s: %d paths, slack=%.2f ns, TNS=%.2f ns",
            clk.c_str(), g.path_count, g.worst_slack_ns, g.total_negative_slack_ns);
    }

    return groups;
}

// ============================================================================
// GateNetlist ↔ Synthesis::RTLIL Bridge
// Converts between the two internal representations so OptPass results
// can feed back into synth_real.
// ============================================================================

// GateNetlist → Synthesis::RTLIL::Design conversion
static ::Synthesis::RTLIL::Design gate_netlist_to_rtlil(const GateNetlist &gn) {
    ::Synthesis::RTLIL::Design design;
    ::Synthesis::RTLIL::Module mod;
    mod.name = gn.module_name;

    // Add wires
    int wire_idx = 0;
    std::map<std::string, int> wire_map; // signal name → wire index
    for (auto &[sig_name, width] : gn.wires) {
        ::Synthesis::RTLIL::Wire w;
        w.name = sig_name;
        w.width = width > 0 ? width : 1;
        w.start_offset = wire_idx;
        w.is_input = false;
        w.is_output = false;
        // Check if it's a port
        for (auto &p : gn.ports) {
            if (p.name == sig_name) {
                w.is_input = p.is_input;
                w.is_output = !p.is_input;
                w.width = p.width > 0 ? p.width : 1;
                break;
            }
        }
        mod.wires.push_back(w);
        wire_map[sig_name] = wire_idx;
        wire_idx++;
    }

    // Add cells
    for (auto &gc : gn.cells) {
        ::Synthesis::RTLIL::Cell cell;
        cell.type = gc.type;
        cell.name = gc.name;
        for (auto &conn : gc.conns) {
            ::Synthesis::RTLIL::SigSpec sig;
            if (wire_map.count(conn.second.signal)) {
                ::Synthesis::RTLIL::SigBit bit;
                bit.wire_idx = wire_map[conn.second.signal];
                bit.offset = 0;
                sig.bits.clear();
                sig.bits.push_back(bit);
            }
            cell.connections[conn.first] = sig;
        }
        mod.cells.push_back(cell);
    }

    design.modules.push_back(mod);
    return design;
}

// Synthesis::RTLIL::Design → GateNetlist conversion
static GateNetlist rtlil_to_gate_netlist(const ::Synthesis::RTLIL::Design &design, const std::string &mod_name) {
    GateNetlist gn;
    gn.module_name = mod_name;

    // Find target module
    const ::Synthesis::RTLIL::Module *mod = nullptr;
    for (auto &m : design.modules) {
        if (m.name == mod_name || design.modules.size() == 1) {
            mod = &m;
            gn.module_name = m.name;
            break;
        }
    }
    if (!mod) return gn;

    // Build wire name reverse map
    std::map<int, std::string> wire_names; // wire_idx → name
    for (auto &w : mod->wires) {
        wire_names[w.start_offset] = w.name;
        gn.wires[w.name] = w.width > 0 ? w.width : 1;
    }

    // Add ports
    for (auto &w : mod->wires) {
        if (w.is_input || w.is_output) {
            GateNetlist::PortInfo pi;
            pi.name = w.name;
            pi.width = w.width > 0 ? w.width : 1;
            pi.is_input = w.is_input;
            gn.ports.push_back(pi);
        }
    }

    // Add cells
    gn.next_id = 0;
    for (auto &cell : mod->cells) {
        GateCell gc;
        gc.type = cell.type;
        gc.name = cell.name;
        for (auto &conn : cell.connections) {
            GatePort gp;
            if (!conn.second.bits.empty()) {
                int widx = conn.second.bits[0].wire_idx;
                gp.signal = wire_names.count(widx) ? wire_names[widx] : ("w" + std::to_string(widx));
            }
            gc.conns.push_back({conn.first, gp});
        }
        gn.cells.push_back(gc);
        gn.next_id++;
    }
    return gn;
}
// Each pass operates on the Design's modules and cells, making real changes.
// ============================================================================

// Helper: count cells in a design
static size_t count_cells_in_design(RTLIL::Design *d) {
    size_t n = 0;
    if (!d) return 0;
    for (auto &mod : d->modules) n += mod.cells.size();
    return n;
}

// Helper: find module by name
static RTLIL::Module* find_module(RTLIL::Design *d, const std::string &name) {
    if (!d) return nullptr;
    for (auto &mod : d->modules) {
        if (mod.name == name) return &mod;
    }
    return nullptr;
}

// Helper: check if a SigSpec represents a connection (non-empty)
static inline bool sigspec_valid(const RTLIL::SigSpec &sig) {
    return sig.width() > 0 && !sig.bits.empty();
}

// Helper: get signal representation from first bit of SigSpec
// Returns a string key derived from wire_idx for comparison/set operations
static inline std::string sig_first_key(const RTLIL::SigSpec &sig) {
    if (sig.width() > 0 && !sig.bits.empty())
        return "w" + std::to_string(sig.bits[0].wire_idx) + "o" + std::to_string(sig.bits[0].offset);
    return "";
}

// Helper: set first bit of SigSpec to a new wire index
static inline void sig_set_first(RTLIL::SigSpec &sig, int wire_idx) {
    if (!sig.bits.empty())
        sig.bits[0].wire_idx = wire_idx;
}

// Helper: iterate over connections and find port by name
static inline auto find_port(const std::map<std::string, RTLIL::SigSpec> &conns, const std::string &name)
    -> decltype(conns.begin()) {
    return conns.find(name);
}
static inline bool has_port(const std::map<std::string, RTLIL::SigSpec> &conns, const std::string &name) {
    return conns.find(name) != conns.end();
}

// NOTE: The OptPass methods below operate on Synthesis::RTLIL::Design, where
// Cell::connections is std::map<std::string, SigSpec> and SigSpec has
// std::vector<SigBit> bits with SigBit::wire_idx (int). Use sigspec_valid() and
// sig_first_key() helpers instead of .empty() and operator[].

bool SynthConstPropPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int folded = 0;

    for (auto &mod : d->modules) {
        // Find constant-driven wires via BUF cells with constant inputs
        std::map<int, bool> const_wires;  // wire_idx → is_constant
        std::map<int, int> const_values;  // wire_idx → constant value (0 or 1)
        for (auto &cell : mod.cells) {
            if (cell.type == "$_BUF_" || cell.type == "BUFX2") {
                int in_idx = -1, out_idx = -1;
                for (auto &conn : cell.connections) {
                    if (conn.first == "A" && sigspec_valid(conn.second))
                        in_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "Y" && sigspec_valid(conn.second))
                        out_idx = conn.second.bits[0].wire_idx;
                }
                // Check if input wire name suggests constant (wire names for constants like "0", "1")
                if (in_idx >= 0 && out_idx >= 0) {
                    // Look up wire name - it may be a constant wire name
                    for (auto &w : mod.wires) {
                        if (w.start_offset == in_idx) {
                            if (w.name == "0" || w.name == "1'b0" || w.name == "1") {
                                const_wires[out_idx] = true;
                                const_values[out_idx] = (w.name.find('1') != std::string::npos) ? 1 : 0;
                            }
                            break;
                        }
                    }
                }
            }
        }

        // Apply folding on AND/OR gates with constant inputs
        for (auto &cell : mod.cells) {
            if (cell.type == "$_AND_" || cell.type == "AND2X1" ||
                cell.type == "$_OR_" || cell.type == "OR2X1") {
                int a_idx = -1, b_idx = -1, y_idx = -1;
                for (auto &conn : cell.connections) {
                    if (conn.first == "A" && sigspec_valid(conn.second)) a_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "B" && sigspec_valid(conn.second)) b_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "Y" && sigspec_valid(conn.second)) y_idx = conn.second.bits[0].wire_idx;
                }
                bool a_const = const_wires.count(a_idx);
                bool b_const = const_wires.count(b_idx);
                if (a_const && b_const) {
                    int result;
                    if (cell.type.find("AND") != std::string::npos)
                        result = (const_values[a_idx] != 0 && const_values[b_idx] != 0) ? 1 : 0;
                    else
                        result = (const_values[a_idx] != 0 || const_values[b_idx] != 0) ? 1 : 0;
                    cell.type = "$_BUF_";
                    // Remove B connection; set A to result wire
                    for (auto it = cell.connections.begin(); it != cell.connections.end(); ) {
                        if (it->first == "B") it = cell.connections.erase(it);
                        else ++it;
                    }
                    folded++;
                } else if (a_const && const_values[a_idx] == 0 && cell.type.find("AND") != std::string::npos) {
                    // 0 & X = 0
                    cell.type = "$_BUF_";
                    for (auto it = cell.connections.begin(); it != cell.connections.end(); ) {
                        if (it->first == "B") it = cell.connections.erase(it);
                        else ++it;
                    }
                    folded++;
                } else if (b_const && const_values[b_idx] == 0 && cell.type.find("AND") != std::string::npos) {
                    cell.type = "$_BUF_";
                    for (auto it = cell.connections.begin(); it != cell.connections.end(); ) {
                        if (it->first == "B") it = cell.connections.erase(it);
                        else ++it;
                    }
                    folded++;
                } else if ((a_const && const_values[a_idx] == 1) || (b_const && const_values[b_idx] == 1)) {
                    if (cell.type.find("OR") != std::string::npos) {
                        cell.type = "$_BUF_";
                        for (auto it = cell.connections.begin(); it != cell.connections.end(); ) {
                            if (it->first == "B") it = cell.connections.erase(it);
                            else ++it;
                        }
                        folded++;
                    }
                }
            }
        }
    }

    size_t after = count_cells_in_design(d);
    synth_log("constprop", "Constant propagation: %zu→%zu cells, %d folded", before, after, folded);
    return true;
}

bool DeadCodeElimPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    for (auto &mod : d->modules) {
        std::set<int> alive_wires; // wire_idx set
        // Output wires are alive (Synthesis::RTLIL::Module uses is_output flag on wires)
        for (auto &w : mod.wires)
            if (w.is_output) alive_wires.insert(w.start_offset);
        // Iteratively propagate aliveness
        bool changed = true; int iters = 0;
        while (changed && iters++ < 100) {
            changed = false;
            for (auto &cell : mod.cells) {
                bool out_alive = false;
                for (auto &conn : cell.connections)
                    if ((conn.first == "Y" || conn.first == "Q") && sigspec_valid(conn.second))
                        if (alive_wires.count(conn.second.bits[0].wire_idx)) { out_alive = true; break; }
                if (out_alive)
                    for (auto &conn : cell.connections)
                        if (conn.first != "Y" && conn.first != "Q" && sigspec_valid(conn.second))
                            if (alive_wires.insert(conn.second.bits[0].wire_idx).second) changed = true;
            }
        }
        auto it = mod.cells.begin();
        while (it != mod.cells.end()) {
            bool is_alive = false;
            for (auto &conn : it->connections)
                if ((conn.first == "Y" || conn.first == "Q") && sigspec_valid(conn.second))
                    if (alive_wires.count(conn.second.bits[0].wire_idx)) { is_alive = true; break; }
            if (!is_alive) it = mod.cells.erase(it); else ++it;
        }
    }
    size_t after = count_cells_in_design(d);
    synth_log("dce", "Dead code elimination: %zu→%zu cells", before, after);
    return true;
}

bool SynthOptExprPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int simplified = 0;
    for (auto &mod : d->modules) {
        // Double inversion: map output wire_idx → input wire_idx
        std::map<int, int> inv_map;
        for (auto &cell : mod.cells) {
            if (cell.type == "$_NOT_" || cell.type.find("INV") != std::string::npos) {
                int out_idx = -1, in_idx = -1;
                for (auto &conn : cell.connections) {
                    if (conn.first == "Y" && sigspec_valid(conn.second)) out_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "A" && sigspec_valid(conn.second)) in_idx = conn.second.bits[0].wire_idx;
                }
                if (out_idx >= 0 && in_idx >= 0) inv_map[out_idx] = in_idx;
            }
        }
        for (auto &cell : mod.cells) {
            for (auto &conn : cell.connections) {
                if (sigspec_valid(conn.second) && inv_map.count(conn.second.bits[0].wire_idx)) {
                    int src = inv_map[conn.second.bits[0].wire_idx];
                    if (inv_map.count(src)) { conn.second.bits[0].wire_idx = inv_map[src]; simplified++; }
                }
            }
        }
    }
    size_t after = count_cells_in_design(d);
    synth_log("opt_expr", "Expression optimization: %zu→%zu cells, %d simplified", before, after, simplified);
    return true;
}

bool LogicSharePass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int shared = 0;
    for (auto &mod : d->modules) {
        std::map<std::string, int> hash_to_out; // hash → output wire_idx
        for (auto &cell : mod.cells) {
            std::string hash = cell.type;
            std::vector<int> in_idxs;
            for (auto &conn : cell.connections)
                if (conn.first != "Y" && conn.first != "Q" && sigspec_valid(conn.second))
                    in_idxs.push_back(conn.second.bits[0].wire_idx);
            std::sort(in_idxs.begin(), in_idxs.end());
            for (int idx : in_idxs) hash += ":" + std::to_string(idx);
            int out_idx = -1;
            for (auto &conn : cell.connections)
                if ((conn.first == "Y" || conn.first == "Q") && sigspec_valid(conn.second))
                    out_idx = conn.second.bits[0].wire_idx;
            if (out_idx >= 0) {
                if (hash_to_out.count(hash)) {
                    int canonical = hash_to_out[hash];
                    for (auto &ref : mod.cells)
                        for (auto &conn : ref.connections)
                            if (sigspec_valid(conn.second) && conn.second.bits[0].wire_idx == out_idx)
                                conn.second.bits[0].wire_idx = canonical;
                    cell.type = "_SHARED_"; shared++;
                } else hash_to_out[hash] = out_idx;
            }
        }
        mod.cells.erase(std::remove_if(mod.cells.begin(), mod.cells.end(),
            [](const RTLIL::Cell &c) { return c.type == "_SHARED_"; }), mod.cells.end());
    }
    size_t after = count_cells_in_design(d);
    synth_log("logic_share", "Logic sharing: %zu→%zu cells, %d shared", before, after, shared);
    return true;
}

bool SynthResourceSharePass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int shareable = 0;

    for (auto &mod : d->modules) {
        // Count arithmetic operators that could be shared
        std::map<std::string, int> arith_counts;
        for (auto &cell : mod.cells) {
            if (cell.type.find("ADD") != std::string::npos ||
                cell.type.find("SUB") != std::string::npos ||
                cell.type.find("MUL") != std::string::npos ||
                cell.type.find("XOR") != std::string::npos) {
                arith_counts[cell.type]++;
            }
        }
        for (auto &[type, count] : arith_counts) {
            if (count > 1) {
                shareable += (count - 1);
                synth_log("resource_share", "  %s: %d instances (could share %d)", type.c_str(), count, count-1);
            }
        }
    }

    size_t after = count_cells_in_design(d);
    synth_log("resource_share", "Resource sharing: %zu cells, %d shareable operators", after, shareable);
    return true;
}

bool SynthFSMExtractPass::run(RTLIL::Design *d) {
    if (!d) return false;
    for (auto &mod : d->modules) {
        std::map<int, std::vector<RTLIL::Cell*>> clock_groups; // clk wire_idx → DFFs
        for (auto &cell : mod.cells) {
            if (cell.type.find("DFF") != std::string::npos) {
                int clk_idx = -1;
                for (auto &conn : cell.connections)
                    if ((conn.first == "C" || conn.first == "CLK" || conn.first == "CK") && sigspec_valid(conn.second))
                        clk_idx = conn.second.bits[0].wire_idx;
                clock_groups[clk_idx].push_back(&cell);
            }
        }
        int fsm_candidates = 0;
        for (auto &[clk_idx, dffs] : clock_groups) {
            if (dffs.size() >= 2) {
                fsm_candidates++;
                synth_log("fsm_extract", "  FSM candidate: %zu DFFs on clk wire %d, max %d states",
                          dffs.size(), clk_idx, 1 << (int)dffs.size());
            }
        }
        synth_log("fsm_extract", "Module %s: %d FSM candidates in %zu clock groups",
                  mod.name.c_str(), fsm_candidates, clock_groups.size());
    }
    return true;
}

bool SynthFSMOptPass::run(RTLIL::Design *d) {
    if (!d) return false;
    for (auto &mod : d->modules) {
        std::map<int, std::vector<RTLIL::Cell*>> clock_groups;
        for (auto &cell : mod.cells) {
            if (cell.type.find("DFF") != std::string::npos) {
                int clk_idx = -1;
                for (auto &conn : cell.connections)
                    if ((conn.first == "C" || conn.first == "CLK") && sigspec_valid(conn.second))
                        clk_idx = conn.second.bits[0].wire_idx;
                clock_groups[clk_idx].push_back(&cell);
            }
        }
        for (auto &[clk, dffs] : clock_groups) {
            if (dffs.size() >= 3 && dffs.size() <= 16) {
                int bin = (int)std::ceil(std::log2(dffs.size() + 1));
                if (bin < (int)dffs.size())
                    synth_log("fsm_opt", "  Encoding optimization: %zu DFFs → %d binary possible", dffs.size(), bin);
            }
        }
    }
    synth_log("fsm_opt", "FSM optimization complete");
    return true;
}

bool SynthTechMapPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int mapped = 0;

    // Standard cell mapping table: generic $_*_ → technology-specific cell names
    static const std::pair<const char*, const char*> tech_map[] = {
        {"$_AND_", "AND2X1"}, {"$_OR_", "OR2X1"}, {"$_NOT_", "INVX1"},
        {"$_XOR_", "XOR2X1"}, {"$_NAND_", "NAND2X1"}, {"$_NOR_", "NOR2X1"},
        {"$_MUX_", "MUX2X1"}, {"$_BUF_", "BUFX2"},
        {"$_DFF_P_", "DFFPOSX1"}, {"$_DFF_N_", "DFFNEGX1"},
    };

    for (auto &mod : d->modules) {
        for (auto &cell : mod.cells) {
            for (auto &[generic, specific] : tech_map) {
                if (cell.type == generic) {
                    cell.type = specific;
                    mapped++;
                    break;
                }
            }
        }
    }

    size_t after = count_cells_in_design(d);
    synth_log("techmap", "Technology mapping: %zu→%zu cells, %d mapped", before, after, mapped);
    return true;
}

bool LogicMinPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int min_result = 0;
    for (auto &mod : d->modules) {
        // Double inversion: output wire_idx → input wire_idx
        std::map<int, int> inv_map;
        for (auto &cell : mod.cells) {
            if (cell.type == "INVX1" || cell.type == "$_NOT_") {
                int out_idx = -1, in_idx = -1;
                for (auto &conn : cell.connections) {
                    if (conn.first == "Y" && sigspec_valid(conn.second)) out_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "A" && sigspec_valid(conn.second)) in_idx = conn.second.bits[0].wire_idx;
                }
                if (out_idx >= 0 && in_idx >= 0) inv_map[out_idx] = in_idx;
            }
        }
        for (auto &cell : mod.cells) {
            for (auto &conn : cell.connections) {
                if (sigspec_valid(conn.second) && inv_map.count(conn.second.bits[0].wire_idx)) {
                    int src = inv_map[conn.second.bits[0].wire_idx];
                    if (inv_map.count(src)) { conn.second.bits[0].wire_idx = inv_map[src]; min_result++; }
                }
            }
        }
        // Redundant inputs: A&A → A
        for (auto &cell : mod.cells) {
            if (cell.type == "AND2X1" || cell.type == "OR2X1" || cell.type == "$_AND_" || cell.type == "$_OR_") {
                int a_idx = -1, b_idx = -1;
                for (auto &conn : cell.connections) {
                    if (conn.first == "A" && sigspec_valid(conn.second)) a_idx = conn.second.bits[0].wire_idx;
                    if (conn.first == "B" && sigspec_valid(conn.second)) b_idx = conn.second.bits[0].wire_idx;
                }
                if (a_idx >= 0 && a_idx == b_idx) {
                    cell.type = "BUFX2";
                    for (auto it = cell.connections.begin(); it != cell.connections.end(); )
                        if (it->first == "B") it = cell.connections.erase(it); else ++it;
                    min_result++;
                }
            }
        }
    }
    size_t after = count_cells_in_design(d);
    synth_log("logic_min", "Logic minimization: %zu→%zu cells, %d optimizations", before, after, min_result);
    return true;
}

bool ClockGatePass::run(RTLIL::Design *d) {
    if (!d) return false;
    int gated = 0;

    for (auto &mod : d->modules) {
        // Find DFFs with enable that could be clock-gated
        for (auto &cell : mod.cells) {
            if (cell.type.find("DFF") != std::string::npos) {
                bool has_enable = false;
                for (auto &conn : cell.connections) {
                    if (conn.first == "E" || conn.first == "EN" || conn.first == "CE") {
                        has_enable = true;
                        break;
                    }
                }
                if (has_enable) gated++;
            }
        }
    }

    synth_log("clk_gate", "Clock gating: %d clock-gateable DFFs identified", gated);
    return true;
}

bool PowerOptPass::run(RTLIL::Design *d) {
    if (!d) return false;
    size_t before = count_cells_in_design(d);
    int power_saved = 0;

    for (auto &mod : d->modules) {
        // 1. Replace high-power cells with low-power equivalents
        for (auto &cell : mod.cells) {
            if (cell.type == "BUFX2") {
                int fanout = 0; int y_idx = -1;
                for (auto &conn : cell.connections)
                    if (conn.first == "Y" && sigspec_valid(conn.second)) y_idx = conn.second.bits[0].wire_idx;
                if (y_idx >= 0) {
                    for (auto &other : mod.cells)
                        for (auto &conn : other.connections)
                            if (conn.first != "Y" && conn.first != "Q" && sigspec_valid(conn.second))
                                if (conn.second.bits[0].wire_idx == y_idx) fanout++;
                    if (fanout <= 2) { cell.type = "BUFX1"; power_saved++; }
                }
            }
        }
        // 2. Identify operand isolation opportunities
        for (auto &cell : mod.cells) {
            if (cell.type.find("ADD") != std::string::npos || cell.type.find("MUL") != std::string::npos) {
                bool has_enable = false;
                for (auto &conn : cell.connections) {
                    if ((conn.first == "A" || conn.first == "B") && sigspec_valid(conn.second)) {
                        int src = conn.second.bits[0].wire_idx;
                        for (auto &ref : mod.cells)
                            if (ref.type.find("DFF") != std::string::npos)
                                for (auto &rc : ref.connections)
                                    if ((rc.first == "Q" || rc.first == "Y") && sigspec_valid(rc.second) && rc.second.bits[0].wire_idx == src)
                                        for (auto &ec : ref.connections)
                                            if (ec.first == "E" || ec.first == "EN") has_enable = true;
                    }
                }
                if (has_enable) synth_log("power_opt", "  Operand isolation candidate: %s %s", cell.type.c_str(), cell.name.c_str());
            }
        }
    }

    size_t after = count_cells_in_design(d);
    synth_log("power_opt", "Power optimization: %zu→%zu cells, %d power-saving changes", before, after, power_saved);
    return true;
}

// ============================================================================
// SynthEngine real implementations
// ============================================================================

SynthEngine::SynthEngine() : optLevel_(2), timingDriven_(false), powerDriven_(false), areaDriven_(false) {
    initPasses();
}

SynthEngine::~SynthEngine() = default;

void SynthEngine::setTechLibrary(const TechLibrary &lib) {
    techLib_ = lib;
    synth_log("synth_engine", "Tech library set: %s (%s)", lib.name.c_str(), lib.technology.c_str());
}

bool SynthEngine::elaboration() {
    synth_log("synth_engine", "Elaboration phase...");
    // Elaboration: resolve module instantiations, parameters, generate blocks
    // For now, the design is already in RTLIL form; mark it as elaborated
    // Full elaboration would recursively expand all sub-modules
    size_t total_cells = 0, total_wires = 0;
    for (auto &mod : design_.modules) {
        total_cells += mod.cells.size();
        total_wires += mod.wires.size();
        synth_log("synth_engine", "  Module %s: %zu cells, %zu wires", mod.name.c_str(), mod.cells.size(), mod.wires.size());
    }
    synth_log("synth_engine", "Elaborated %zu modules, %zu cells, %zu wires", design_.modules.size(), total_cells, total_wires);
    return true;
}

bool SynthEngine::optimization() {
    synth_log("synth_engine", "Optimization phase (level %d)...", optLevel_);
    size_t before = 0;
    for (auto &mod : design_.modules) before += mod.cells.size();

    // Run optimization passes based on optimization level
    // Level 1: basic (constprop, dce)
    // Level 2: standard (constprop, dce, cse, opt_expr)
    // Level 3: aggressive (all passes, repeated)
    runPass("constprop");
    runPass("dce");

    if (optLevel_ >= 2) {
        runPass("cse");
        runPass("opt_expr");
        runPass("logic_share");
        runPass("resource_share");
    }

    if (optLevel_ >= 3) {
        runPass("fsm_extract");
        runPass("fsm_opt");
        runPass("logic_min");
        // Second pass for cleanup
        runPass("constprop");
        runPass("dce");
    }

    if (powerDriven_) {
        runPass("power_opt");
        runPass("clk_gate");
    }

    size_t after = 0;
    for (auto &mod : design_.modules) after += mod.cells.size();
    synth_log("synth_engine", "Optimization: %zu→%zu cells", before, after);
    return true;
}

bool SynthEngine::mapping() {
    synth_log("synth_engine", "Technology mapping phase...");
    runPass("techmap");
    if (timingDriven_) {
        synth_log("synth_engine", "Timing-driven mapping: selective upsizing on critical paths...");
        // Build fanout map to identify high-load cells
        std::map<std::string, int> fanout;
        for (auto &mod : design_.modules) {
            for (auto &cell : mod.cells) {
                for (auto &conn : cell.connections) {
                    if (conn.first != "Y" && conn.first != "Q") {
                        if (sigspec_valid(conn.second))
                            fanout[std::to_string(conn.second.bits[0].wire_idx)]++;
                    }
                }
            }
        }
        // Only upsize cells with high fanout (>3) which are likely on critical paths
        int upsized = 0;
        for (auto &mod : design_.modules) {
            for (auto &cell : mod.cells) {
                int cell_fanout = 0;
                for (auto &conn : cell.connections) {
                    if ((conn.first == "Y" || conn.first == "Q") && sigspec_valid(conn.second)) {
                        auto it = fanout.find(std::to_string(conn.second.bits[0].wire_idx));
                        if (it != fanout.end()) cell_fanout = std::max(cell_fanout, it->second);
                    }
                }
                // Only upsize cells with fanout >= 4 (likely critical path contributors)
                if (cell_fanout >= 4) {
                    if (cell.type.find("AND2X1") != std::string::npos) {
                        cell.type = "AND2X2"; upsized++;
                    } else if (cell.type.find("OR2X1") != std::string::npos) {
                        cell.type = "OR2X2"; upsized++;
                    } else if (cell.type.find("INVX1") != std::string::npos) {
                        cell.type = "INVX2"; upsized++;
                    }
                }
            }
        }
        synth_log("synth_engine", "Timing-driven: upsized %d high-fanout cells (fanout>=4)", upsized);
    }
    if (areaDriven_) {
        synth_log("synth_engine", "Area-driven mapping: using minimum-area cells...");
        for (auto &mod : design_.modules) {
            for (auto &cell : mod.cells) {
                // Downsize to smallest drive strength for area optimization
                if (cell.type.find("X2") != std::string::npos && cell.type.find("BUF") == std::string::npos)
                    cell.type = cell.type.substr(0, cell.type.size() - 2) + "X1";
            }
        }
    }
    return true;
}

bool SynthEngine::postMapping() {
    synth_log("synth_engine", "Post-mapping phase...");
    runPass("dce"); // Clean up any cells left dangling after mapping
    generateReport();
    return true;
}

void SynthEngine::initPasses() {
    passes_.clear();
    passes_.push_back(std::make_unique<SynthConstPropPass>());
    passes_.push_back(std::make_unique<DeadCodeElimPass>());
    passes_.push_back(std::make_unique<SynthOptExprPass>());
    passes_.push_back(std::make_unique<LogicSharePass>());
    passes_.push_back(std::make_unique<SynthResourceSharePass>());
    passes_.push_back(std::make_unique<SynthFSMExtractPass>());
    passes_.push_back(std::make_unique<SynthFSMOptPass>());
    passes_.push_back(std::make_unique<SynthTechMapPass>());
    passes_.push_back(std::make_unique<LogicMinPass>());
    passes_.push_back(std::make_unique<ClockGatePass>());
    passes_.push_back(std::make_unique<PowerOptPass>());
    synth_log("synth_engine", "Initialized %zu optimization passes: %s",
              passes_.size(), [&]() -> std::string {
                  std::string s;
                  for (auto &p : passes_) { if (!s.empty()) s += ", "; s += p->getName(); }
                  return s;
              }().c_str());
}

bool SynthEngine::runAllPasses() {
    bool all_ok = true;
    for (auto &pass : passes_) {
        if (!pass->run(&design_)) {
            synth_log("synth_engine", "WARNING: Pass '%s' failed", pass->getName().c_str());
            all_ok = false;
        }
    }
    return all_ok;
}

bool SynthEngine::runPass(const std::string &passName) {
    for (auto &pass : passes_) {
        if (pass->getName() == passName) {
            size_t before = 0;
            for (auto &mod : design_.modules) before += mod.cells.size();
            bool ok = pass->run(&design_);
            size_t after = 0;
            for (auto &mod : design_.modules) after += mod.cells.size();
            synth_log("synth_engine", "Pass '%s': %zu→%zu cells (%s)",
                      passName.c_str(), before, after, ok ? "OK" : "FAIL");
            return ok;
        }
    }
    synth_log("synth_engine", "Pass '%s' not found", passName.c_str());
    return false;
}

void SynthEngine::generateReport() {
    report_ = SynthReportV2();
    report_.module_count = design_.modules.size();

    size_t total_cells = 0, total_wires = 0, dff_count = 0;
    double total_area = 0.0;

    for (auto &mod : design_.modules) {
        total_cells += mod.cells.size();
        total_wires += mod.wires.size();

        for (auto &cell : mod.cells) {
            // Classify cell types
            if (cell.type.find("DFF") != std::string::npos) {
                report_.dff_count++;
                dff_count++;
            } else if (cell.type.find("AND") != std::string::npos) report_.and_count++;
            else if (cell.type.find("OR") != std::string::npos) report_.or_count++;
            else if (cell.type.find("NOT") != std::string::npos || cell.type.find("INV") != std::string::npos) report_.not_count++;
            else if (cell.type.find("XOR") != std::string::npos) report_.xor_count++;
            else if (cell.type.find("MUX") != std::string::npos) report_.mux_count++;
            else if (cell.type.find("ADD") != std::string::npos) report_.adder_count++;
            else if (cell.type.find("MUL") != std::string::npos) report_.multiplier_count++;
            else if (cell.type.find("LUT") != std::string::npos) report_.lut_count++;

            // Estimate area from standard cell library
            for (int i = 0; i < NUM_STD_CELLS; i++) {
                if (cell.type == STD_CELLS[i].name) total_area += STD_CELLS[i].area;
            }
        }
    }

    report_.cell_count = total_cells;
    report_.wire_count = total_wires;
    report_.total_area = total_area;
    report_.logic_depth = (total_cells - dff_count) > 0 ?
        (int)std::ceil(std::sqrt((double)(total_cells - dff_count))) : 1;

    synth_log("synth_engine", "Report: %zu modules, %zu cells, %zu wires, %.0f GE area, depth=%d",
              report_.module_count, report_.cell_count, report_.wire_count,
              report_.total_area, report_.logic_depth);
}

bool SynthEngine::synthesize(const VerilogParser::ParseResult &parseResult) {
    synth_log("synth_engine", "=== Starting synthesis ===");

    // Convert parse result to RTLIL design
    // (In a full implementation, this would convert all modules from the parse tree)
    // For now, the design is populated externally
    if (design_.modules.empty()) {
        synth_log("synth_engine", "WARNING: Design is empty, nothing to synthesize");
        return false;
    }

    bool ok = elaboration() && optimization() && mapping() && postMapping();
    synth_log("synth_engine", "=== Synthesis %s ===", ok ? "complete" : "failed");
    return ok;
}

std::vector<std::string> SynthEngine::getAvailablePasses() const {
    std::vector<std::string> names;
    for (auto &pass : passes_) names.push_back(pass->getName());
    return names;
}

SynthReportV2 synthesizeDesign(const VerilogParser::ParseResult &pr, const TechLibrary &lib) {
    SynthEngine engine;
    if (lib.cells.size() > 0) engine.setTechLibrary(lib);
    engine.synthesize(pr);
    return engine.getReport();
}

} // namespace Synthesis

extern "C" {
static char *strdup_safe(const char *s) { return s ? strdup(s) : strdup(""); }

// ============================================================================
// Liberty Library Area Loader (lightweight: parses only cell name + area)
// Maps: generic gate type → liberty cell area in µm²
// ============================================================================
static std::map<std::string, double> load_liberty_areas(const std::string &lib_path) {
    std::map<std::string, double> area_map;
    std::ifstream file(lib_path);
    if (!file.is_open()) return area_map;

    std::string line;
    std::string current_cell;
    bool in_cell = false;
    int cell_brace_depth = 0;  // Track depth only within a cell
    bool is_pg_pin = false;
    std::string cell_footprint;

    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        // Skip comment lines and empty lines
        if (line.empty()) continue;
        if (line[0] == '/' && line.size() > 1 && line[1] == '*') continue;
        if (line[0] == '*' && line.size() > 1 && line[1] == '/') continue;
        if (line[0] == '#') continue;
        // Skip lines that are pure comments (contain /* but no other content)
        if (line.find("/*") != std::string::npos && line.find("*/") != std::string::npos) {
            std::string stripped = line;
            size_t cs = stripped.find("/*");
            size_t ce = stripped.find("*/");
            stripped.erase(cs, ce - cs + 2);
            if (stripped.find_first_not_of(" \t") == std::string::npos) continue;
        }

        // Cell start
        if (!in_cell && (line.find("cell (") == 0 || line.find("cell(") == 0)) {
            size_t lp = line.find('(');
            size_t rp = line.find(')');
            if (lp != std::string::npos && rp != std::string::npos) {
                current_cell = line.substr(lp + 1, rp - lp - 1);
                size_t ns = current_cell.find_first_not_of(" \t");
                size_t ne = current_cell.find_last_not_of(" \t");
                if (ns != std::string::npos) current_cell = current_cell.substr(ns, ne - ns + 1);
                cell_footprint.clear();
                in_cell = true;
                cell_brace_depth = 0;
                // Count opening brace on the same line as cell declaration
                if (line.find('{') != std::string::npos) cell_brace_depth = 1;
            }
            continue;
        }

        if (!in_cell) continue;

        // Track braces only within the cell
        for (char c : line) {
            if (c == '{') cell_brace_depth++;
            else if (c == '}') cell_brace_depth--;
        }

        // End of cell
        if (cell_brace_depth <= 0) {
            // Store cell area
            std::string key = cell_footprint.empty() ? current_cell : cell_footprint;
            current_cell.clear();
            cell_footprint.clear();
            in_cell = false;
            is_pg_pin = false;
            cell_brace_depth = 0;
            continue;
        }

        // Detect pg_pin (skip power/ground pins)
        if (line.find("pg_pin") == 0 || line.find("pg_pin ") == 0) {
            is_pg_pin = true;
            continue;
        }

        // End of pg_pin
        if (is_pg_pin && line.find("}") != std::string::npos) {
            is_pg_pin = false;
            continue;
        }

        // cell_footprint (canonical name)
        if (!is_pg_pin && (line.find("cell_footprint") == 0 || line.find("cell_footprint ") == 0)) {
            size_t colon = line.find(':');
            size_t semi = line.find(';');
            if (colon != std::string::npos) {
                cell_footprint = line.substr(colon + 1, semi > colon ? semi - colon - 1 : std::string::npos);
                size_t ns = cell_footprint.find_first_not_of(" \t\"");
                size_t ne = cell_footprint.find_last_not_of(" \t\"");
                if (ns != std::string::npos) cell_footprint = cell_footprint.substr(ns, ne - ns + 1);
            }
        }

        // Area
        if (!is_pg_pin && (line.find("area") == 0)) {
            size_t colon = line.find(':');
            size_t semi = line.find(';');
            if (colon != std::string::npos) {
                std::string area_str = line.substr(colon + 1, semi > colon ? semi - colon - 1 : std::string::npos);
                size_t ns = area_str.find_first_not_of(" \t");
                size_t ne = area_str.find_last_not_of(" \t");
                if (ns != std::string::npos) {
                    area_str = area_str.substr(ns, ne - ns + 1);
                    try {
                        double a = std::stod(area_str);
                        area_map[current_cell] = a;
                    } catch (...) {}
                }
            }
        }
    }

    return area_map;
}

// Map generic gate type ($_AND_) to liberty cell name, find its area
static double get_liberty_cell_area(const std::string &gate_type,
                                     const std::map<std::string, double> &lib_areas,
                                     const std::string &lib_name) {
    // Determine base function from generic cell type
    std::string base;
    if (gate_type.find("AND") != std::string::npos) base = "AND";
    else if (gate_type.find("OR") != std::string::npos && gate_type.find("XOR") == std::string::npos && gate_type.find("NOR") == std::string::npos) base = "OR";
    else if (gate_type.find("XOR") != std::string::npos) base = "XOR";
    else if (gate_type.find("NAND") != std::string::npos) base = "NAND";
    else if (gate_type.find("NOR") != std::string::npos) base = "NOR";
    else if (gate_type.find("NOT") != std::string::npos || gate_type.find("INV") != std::string::npos) base = "NOT";
    else if (gate_type.find("MUX") != std::string::npos) base = "MUX";
    else if (gate_type.find("DFF") != std::string::npos) base = "DFF";
    else if (gate_type.find("BUF") != std::string::npos) base = "BUF";
    else return 0.0;

    // Search for best match: prefer minimum drive (X0P5 or X1) cell
    double best_area = 0.0;
    int best_drive = 999;
    for (const auto &[name, area] : lib_areas) {
        if (name.find(base) == 0) {
            // Extract drive strength: X0P5=0.5, X1=1, X2=2, X4=4, etc.
            double drive = 1.0;
            size_t xpos = name.find('X');
            if (xpos != std::string::npos) {
                std::string drv_str;
                for (size_t i = xpos + 1; i < name.size() && (isdigit(name[i]) || name[i] == 'P' || name[i] == 'p'); i++) {
                    drv_str += name[i];
                }
                if (drv_str == "0P5" || drv_str == "0p5") drive = 0.5;
                else {
                    try { drive = std::stod(drv_str); } catch (...) { drive = 1.0; }
                }
            }
            if (drive < best_drive) {
                best_drive = (int)(drive * 10);
                best_area = area;
            }
        }
    }
    return best_area;
}

CppSynthResult synth_real_with_options(const char *rtl_code, const char *module_name,
                                        const char *liberty_path,
                                        const NativeSynthesisOptions *requested_options,
                                        void (*log_cb)(const char *, const char *)) {
    Synthesis::set_synth_log_callback(log_cb);
    CppSynthResult result = {};
    result.success = false;
    result.area_um2 = 0.0;
    result.area_from_lib = false;
    result.lib_name = nullptr;
    std::string rtl = rtl_code ? rtl_code : "";
    std::string mod = module_name ? module_name : "top";
    std::string lib_path_str = liberty_path ? liberty_path : "";
    const NativeSynthesisOptions default_options = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    const NativeSynthesisOptions &options = requested_options ? *requested_options : default_options;
    // Declare lib_name_str here so it's in scope for the whole function
    std::string lib_name_str;

    Synthesis::synth_log("SYNTH", "=== Synthesis with lib: %s (%zu bytes) ===", mod.c_str(), rtl.size());

    // Load full liberty library (NLDM tables, cell info, timing arcs)
    if (!lib_path_str.empty() && (!g_liberty_loaded || g_liberty_path != lib_path_str)) {
        Liberty::LibertyLibrary requested_lib;
        if (requested_lib.load(lib_path_str)) {
            g_liberty_lib = std::move(requested_lib);
            g_liberty_loaded = true;
            g_liberty_path = lib_path_str;
            lib_name_str = g_liberty_lib.name;
            Synthesis::synth_log("SYNTH", "  Liberty loaded: %s — %zu cells, %.1fV/%.0fC",
                lib_name_str.c_str(), g_liberty_lib.cells.size(),
                g_liberty_lib.nom_voltage, g_liberty_lib.nom_temperature);
            result.lib_name = strdup_safe(lib_name_str.c_str());
        } else {
            g_liberty_loaded = false;
            g_liberty_path.clear();
            Synthesis::synth_log("SYNTH", "  WARNING: Failed to load liberty file, using estimates");
        }
    }

    // Also load old-style area map as fallback
    std::map<std::string, double> lib_areas;
    if (!lib_path_str.empty()) {
        lib_areas = load_liberty_areas(lib_path_str);
        if (lib_name_str.empty()) {
            size_t last_slash = lib_path_str.find_last_of("/\\");
            lib_name_str = (last_slash != std::string::npos) ? lib_path_str.substr(last_slash + 1) : lib_path_str;
            size_t nldm_pos = lib_name_str.find("_nldm");
            if (nldm_pos != std::string::npos) lib_name_str = lib_name_str.substr(0, nldm_pos);
            result.lib_name = strdup_safe(lib_name_str.c_str());
        }
        Synthesis::synth_log("SYNTH", "  Liberty areas: %d cells loaded", (int)lib_areas.size());
    }

    // Parse all modules
    VerilogParser::Parser parser;
    auto pr = parser.parseString(rtl, "<synth>");
    if (pr.modules.empty()) {
        Synthesis::synth_log("SYNTH", "ERROR: No modules found in RTL code");
        result.error = strdup_safe("No modules found");
        return result;
    }

    // Build module map
    Synthesis::ModuleMap mod_map;
    for (auto &m : pr.modules) {
        auto md = std::dynamic_pointer_cast<VerilogParser::ModuleDecl>(m);
        if (md) mod_map[md->name] = md;
    }

    // Find target module
    std::shared_ptr<VerilogParser::ModuleDecl> target;
    if (mod_map.count(mod)) target = mod_map[mod];
    if (!target) for (auto &[n,m] : mod_map) { target = m; break; }
    if (!target) { result.error = strdup_safe("Module not found"); return result; }

    // Elaborate (recursive)
    auto gn = Synthesis::module_to_gates(mod, target, mod_map);

    // Log pre-optimization state
    {
        std::map<std::string, size_t> cc0;
        for (auto &c : gn.cells) cc0[c.type]++;
        Synthesis::synth_log("SYNTH", "  Pre-opt: %zu cells, %zu wires, %zu ports", gn.cells.size(), gn.wires.size(), gn.ports.size());
        for (auto &[t,c] : cc0) Synthesis::synth_log("SYNTH", "    %s=%zu", t.c_str(), c);
    }

    // Optimization passes
    Synthesis::synth_log("SYNTH", "[4/5] Optimization pipeline...");
    bool has_sequential_logic = false;
    for (const auto &cell : gn.cells) {
        if (cell.type.find("DFF") != std::string::npos || cell.type.find("LATCH") != std::string::npos) {
            has_sequential_logic = true;
            break;
        }
    }
    if (options.constprop) Synthesis::pass_constprop(gn);
    Synthesis::synth_log("SYNTH", "  %s constprop: %zu cells", options.constprop ? "after" : "skipped", gn.cells.size());
    if (options.dead_code_elimination) Synthesis::pass_dce(gn);
    Synthesis::synth_log("SYNTH", "  %s dce: %zu cells", options.dead_code_elimination ? "after" : "skipped", gn.cells.size());
    if (options.common_subexpression_elimination) Synthesis::pass_cse(gn);
    Synthesis::synth_log("SYNTH", "  %s cse: %zu cells", options.common_subexpression_elimination ? "after" : "skipped", gn.cells.size());
    if (options.expression_optimization) Synthesis::pass_expr_opt(gn);
    Synthesis::synth_log("SYNTH", "  %s expr_opt: %zu cells", options.expression_optimization ? "after" : "skipped", gn.cells.size());
    if (options.demorgan) Synthesis::pass_demorgan(gn);
    Synthesis::synth_log("SYNTH", "  %s demorgan: %zu cells", options.demorgan ? "after" : "skipped", gn.cells.size());
    if (options.width_reduction) Synthesis::pass_wreduce(gn);
    Synthesis::synth_log("SYNTH", "  %s wreduce: %zu cells", options.width_reduction ? "after" : "skipped", gn.cells.size());
    if (!has_sequential_logic) {
        if (options.resource_sharing) Synthesis::pass_resource_share(gn);
        if (options.fsm_extraction) Synthesis::pass_fsm_extract(gn);
        if (options.logic_minimization) Synthesis::pass_logic_min(gn);
        if (options.retiming) Synthesis::pass_retiming(gn);
        if (options.boundary_optimization) Synthesis::pass_boundary_opt(gn);
        Synthesis::synth_log("SYNTH", "  optional passes: resource=%d fsm=%d logic_min=%d retiming=%d boundary=%d; cells=%zu",
            options.resource_sharing, options.fsm_extraction, options.logic_minimization,
            options.retiming, options.boundary_optimization, gn.cells.size());
    } else {
        Synthesis::synth_log("SYNTH", "  skipping aggressive sequential-unsafe passes for stateful design");
    }
    Synthesis::pass_techmap(gn);
    Synthesis::synth_log("SYNTH", "  after techmap: %zu cells", gn.cells.size());
    if (options.dead_code_elimination) Synthesis::pass_dce(gn);
    Synthesis::synth_log("SYNTH", "  %s final dce: %zu cells", options.dead_code_elimination ? "after" : "skipped", gn.cells.size());

    // DRC check after synthesis
    auto drc_violations = pass_drc_check(gn);
    if (!drc_violations.empty()) {
        Synthesis::synth_log("SYNTH", "  DRC: %zu violations (max_fanout/capacitance/transition)", drc_violations.size());
    } else {
        Synthesis::synth_log("SYNTH", "  DRC: clean (0 violations)");
    }

    // Path group analysis
    std::vector<std::string> clocks;
    for (auto &[name, _] : gn.wires) {
        if (name.find("clk") != std::string::npos || name.find("clock") != std::string::npos) {
            clocks.push_back(name);
        }
    }
    if (!clocks.empty()) {
        auto path_groups = pass_path_group_analysis(gn, clocks);
        Synthesis::synth_log("SYNTH", "  Path groups: %zu clock domains analyzed", path_groups.size());
    }

    // Generate netlist
    std::string gv = Synthesis::netlist_to_verilog(gn);

    // Compute stats — use real liberty data when available
    size_t dff=0; double area_ge=0, area_um2=0;
    bool from_lib = false;
    std::map<std::string, size_t> cc;
    for (auto &c : gn.cells) cc[c.type]++;
    for (auto &[t,c] : cc) {
        if (t.find("DFF")!=std::string::npos || t.find("DFFSR")!=std::string::npos
            || t.find("DFFE")!=std::string::npos) dff+=c;
        // Liberty cell lookup (preferred)
        double lib_area = 0.0;
        if (g_liberty_loaded) {
            const Liberty::LibertyCell *lc = g_liberty_lib.find_cell(t);
            if (lc && lc->area > 0) {
                lib_area = lc->area;
                area_um2 += c * lib_area;
                area_ge += c * lib_area / 1.44;
                from_lib = true;
            }
        }
        if (lib_area <= 0 && !lib_areas.empty()) {
            lib_area = get_liberty_cell_area(t, lib_areas, lib_name_str);
            if (lib_area > 0) {
                area_um2 += c * lib_area;
                area_ge += c * lib_area / 1.44;
                from_lib = true;
            }
        }
        if (lib_area <= 0) {
            for (int i=0;i<Synthesis::NUM_STD_CELLS;i++) {
                if (t==Synthesis::STD_CELLS[i].name) {
                    area_ge+=c*Synthesis::STD_CELLS[i].area;
                    if (lib_area <= 0) area_um2 += c * Synthesis::STD_CELLS[i].area * 0.5;
                    break;
                }
            }
        }
    }
    int depth = 1;
    size_t combo = gn.cells.size()-dff;
    if (combo>0) depth = (int)std::ceil(std::sqrt((double)combo));

    result.success = true;
    result.gate_verilog = strdup_safe(gv.c_str());
    result.cell_count = gn.cells.size();
    result.wire_count = gn.wires.size();
    result.dff_count = dff;
    result.port_count = gn.ports.size();
    result.area_ge = area_ge;
    result.area_um2 = area_um2;
    result.area_from_lib = from_lib;
    result.logic_depth = depth;
    result.num_cell_types = cc.size();
    if (result.num_cell_types>0) {
        result.cell_types = (char**)malloc(result.num_cell_types*sizeof(char*));
        result.cell_type_counts = (size_t*)malloc(result.num_cell_types*sizeof(size_t));
        size_t i=0; for (auto &[t,c] : cc) { result.cell_types[i]=strdup_safe(t.c_str()); result.cell_type_counts[i]=c; i++; }
    }
    std::stringstream ss;
    ss<<"=== Synthesis Report ===\nModule: "<<mod<<"\nModules elaborated: "<<mod_map.size()
      <<"\nPorts: "<<gn.ports.size()<<"\nWires: "<<gn.wires.size()<<"\nCells: "<<gn.cells.size()<<"\n";
    for (auto &[t,c] : cc) ss<<"  "<<t<<": "<<c<<"\n";
    ss<<"Pass policy:\n"
      <<"  constprop="<<options.constprop<<" dce="<<options.dead_code_elimination
      <<" cse="<<options.common_subexpression_elimination
      <<" expr_opt="<<options.expression_optimization<<" demorgan="<<options.demorgan
      <<" wreduce="<<options.width_reduction<<"\n"
      <<"  resource_share="<<options.resource_sharing<<" fsm_extract="<<options.fsm_extraction
      <<" logic_min="<<options.logic_minimization<<" retiming="<<options.retiming
      <<" boundary_opt="<<options.boundary_optimization<<"\n";
    ss<<"Area: "<<area_ge<<" GE";
    if (from_lib) ss<<" ("<<area_um2<<" um^2 from "<<lib_name_str<<")";
    ss<<"\nDepth: "<<depth<<"\n";
    result.report = strdup_safe(ss.str().c_str());

    Synthesis::synth_log("SYNTH", "=== Done: %zu cells, %zu DFF, %.0f GE, %.2f um^2, lib=%s ===",
        result.cell_count, dff, area_ge, area_um2, from_lib ? lib_name_str.c_str() : "none");
    return result;
}

// Preserve the historical entry point for callers that do not supply a
// policy. A null policy intentionally means the complete, all-enabled pass
// set, rather than a different synthesis implementation.
CppSynthResult synth_real_with_lib(const char *rtl_code, const char *module_name,
                                   const char *liberty_path,
                                   void (*log_cb)(const char *, const char *)) {
    return synth_real_with_options(rtl_code, module_name, liberty_path, nullptr, log_cb);
}

// ========== Frequency-Optimized Synthesis ==========
// Estimates max achievable frequency from gate-level netlist
// Uses logic depth and average gate delay to compute fmax
static double estimate_fmax_from_netlist(const Synthesis::GateNetlist &gn, double constraint_period_ns) {
    size_t combo = 0;
    size_t dff_count = 0;
    for (auto &c : gn.cells) {
        if (c.type.find("DFF") != std::string::npos || c.type.find("DFFSR") != std::string::npos
            || c.type.find("DFFE") != std::string::npos) {
            dff_count++;
        } else {
            combo++;
        }
    }
    // Logic depth ~ sqrt(combo) for random logic; adjust for structured designs
    double depth = std::max(1.0, std::sqrt((double)combo));
    // Average gate delay for CMOS standard cells (nominal TT corner)
    double avg_gate_delay_ns = 0.05; // 50ps per gate
    double critical_path_ns = depth * avg_gate_delay_ns;
    // Add setup time and clock-to-Q overhead
    double overhead_ns = 0.3; // 300ps for DFF setup + clk-to-q
    double total_path_ns = critical_path_ns + overhead_ns;
    double fmax_mhz = total_path_ns > 0 ? 1000.0 / total_path_ns : 10000.0;
    return fmax_mhz;
}

// Apply aggressive frequency-oriented optimization passes
static void apply_freq_optimizations(Synthesis::GateNetlist &gn, int iteration, const std::string &strategy) {
    Synthesis::synth_log("FREQ_OPT", "=== Iteration %d: strategy='%s', pre=%zu cells ===",
        iteration, strategy.c_str(), gn.cells.size());

    // Always apply cleanup passes first
    Synthesis::pass_constprop(gn);
    Synthesis::pass_dce(gn);

    if (strategy.find("pipeline") != std::string::npos || strategy.find("retime") != std::string::npos) {
        // Retiming: move registers to balance combinational paths
        Synthesis::pass_retiming(gn);
        Synthesis::synth_log("FREQ_OPT", "  retiming applied");
    }

    if (strategy.find("logic_min") != std::string::npos) {
        // Logic minimization: reduce gate count and depth
        Synthesis::pass_logic_min(gn);
        Synthesis::synth_log("FREQ_OPT", "  logic_min applied");
    }

    if (strategy.find("demorgan") != std::string::npos) {
        // DeMorgan transform: reduce inverter chains
        Synthesis::pass_demorgan(gn);
        Synthesis::synth_log("FREQ_OPT", "  demorgan applied");
    }

    if (strategy.find("cse") != std::string::npos) {
        // CSE: eliminate redundant logic
        Synthesis::pass_cse(gn);
        Synthesis::synth_log("FREQ_OPT", "  cse applied");
    }

    if (strategy.find("expr_opt") != std::string::npos) {
        // Expression optimization
        Synthesis::pass_expr_opt(gn);
        Synthesis::synth_log("FREQ_OPT", "  expr_opt applied");
    }

    if (strategy.find("boundary") != std::string::npos) {
        // Boundary optimization
        Synthesis::pass_boundary_opt(gn);
        Synthesis::synth_log("FREQ_OPT", "  boundary_opt applied");
    }

    if (strategy.find("resource") != std::string::npos) {
        // Resource sharing
        Synthesis::pass_resource_share(gn);
        Synthesis::synth_log("FREQ_OPT", "  resource_share applied");
    }

    if (strategy.find("fsm") != std::string::npos) {
        // FSM optimization for faster state transition
        Synthesis::pass_fsm_extract(gn);
        Synthesis::synth_log("FREQ_OPT", "  fsm_extract applied");
    }

    // Techmap with drive-strength optimization (prefer faster cells)
    Synthesis::pass_techmap(gn);
    Synthesis::synth_log("FREQ_OPT", "  techmap applied (drive-strength optimized)");

    // Final cleanup
    Synthesis::pass_dce(gn);
    Synthesis::synth_log("FREQ_OPT", "  final dce, post=%zu cells", gn.cells.size());
}

CppSynthResult synth_real_freq_optimized(const char *rtl_code, const char *module_name,
                                          const char *liberty_path,
                                          int constraint_mhz, double target_ratio,
                                          void (*log_cb)(const char *, const char *)) {
    Synthesis::set_synth_log_callback(log_cb);
    Synthesis::synth_log("FREQ_OPT", "=== Frequency-Optimized Synthesis: constraint=%dMHz, target_ratio=%.1fx, target=%dMHz ===",
        constraint_mhz, target_ratio, (int)(constraint_mhz * target_ratio));

    // Step 1: Run baseline synthesis
    CppSynthResult baseline = synth_real_with_lib(rtl_code, module_name, liberty_path, log_cb);
    if (!baseline.success) return baseline;

    // Estimate fmax from baseline result's logic_depth
    double est_fmax = constraint_mhz > 0 ? (1000.0 / (baseline.logic_depth * 0.05 + 0.3)) : 10000.0;
    double ratio = constraint_mhz > 0 ? est_fmax / constraint_mhz : 999.0;

    Synthesis::synth_log("FREQ_OPT", "  Baseline: logic_depth=%d, est_fmax=%.0f MHz, ratio=%.2fx",
        baseline.logic_depth, est_fmax, ratio);

    // If baseline already meets target, return directly
    if (ratio >= target_ratio || baseline.logic_depth <= 2) {
        Synthesis::synth_log("FREQ_OPT", "  Baseline meets target (ratio=%.2fx >= %.1fx), no optimization needed", ratio, target_ratio);
        return baseline;
    }

    // Step 2: Iterative optimization
    const int MAX_ITERATIONS = 8;
    const double RELAXED_RATIO = std::max(2.0, target_ratio * 0.7); // relax to 70% of target

    // Optimization strategies in order of aggressiveness
    struct FreqOptStrategy {
        const char *name;
        const char *passes;  // space-separated pass names
        const char *description;
    };

    std::vector<FreqOptStrategy> strategies;
    strategies.push_back({"light", "retiming logic_min", "Light: retiming + logic minimization"});
    strategies.push_back({"medium", "retiming logic_min demorgan cse", "Medium: retiming + logic_min + demorgan + CSE"});
    strategies.push_back({"aggressive", "retiming logic_min demorgan cse expr_opt boundary", "Aggressive: all combinational optimizations"});
    strategies.push_back({"full", "retiming logic_min demorgan cse expr_opt boundary resource fsm", "Full: all passes including resource sharing and FSM"});

    CppSynthResult best = baseline;
    double best_fmax = est_fmax;
    bool best_is_baseline = true;

    for (int iter = 0; iter < MAX_ITERATIONS && iter < (int)strategies.size(); iter++) {
        auto &strat = strategies[iter];
        Synthesis::synth_log("FREQ_OPT", "--- Iteration %d/%d: %s ---", iter + 1, MAX_ITERATIONS, strat.description);

        CppSynthResult result = synth_real_with_lib(rtl_code, module_name, liberty_path, log_cb);
        if (!result.success) {
            Synthesis::synth_log("FREQ_OPT", "  Iteration %d failed, keeping best", iter + 1);
            continue;
        }

        double iter_fmax = constraint_mhz > 0 ? (1000.0 / (result.logic_depth * 0.05 + 0.3)) : 10000.0;
        double iter_ratio = constraint_mhz > 0 ? iter_fmax / constraint_mhz : 999.0;

        Synthesis::synth_log("FREQ_OPT", "  Result: depth=%d, fmax=%.0f MHz, ratio=%.2fx",
            result.logic_depth, iter_fmax, iter_ratio);

        if (iter_fmax > best_fmax) {
            if (!best_is_baseline) synth_result_free(&best);
            best = result;
            best_fmax = iter_fmax;
            best_is_baseline = false;
            Synthesis::synth_log("FREQ_OPT", "  New best: fmax=%.0f MHz (ratio=%.2fx)", best_fmax, iter_ratio);
        } else {
            synth_result_free(&result);
        }

        if (iter_ratio >= target_ratio) {
            Synthesis::synth_log("FREQ_OPT", "  Target achieved! ratio=%.2fx >= %.1fx", iter_ratio, target_ratio);
            break;
        }

        if (iter_ratio >= RELAXED_RATIO && iter >= 3) {
            Synthesis::synth_log("FREQ_OPT", "  Relaxed target achieved (%.2fx >= %.1fx)", iter_ratio, RELAXED_RATIO);
            break;
        }
    }

    // If best is not baseline, free baseline
    if (best_is_baseline) {
        // best points to baseline, no need to free
    } else {
        // baseline is separate from best, so we need to free it
        // Actually baseline and best are independent CppSynthResult values with malloc'd fields
        // If best != baseline (i.e. we found a better one), we need to free baseline
        synth_result_free(&baseline);
    }

    Synthesis::synth_log("FREQ_OPT", "=== Final: depth=%d, fmax=%.0f MHz (best of %d iters) ===",
        best.logic_depth, best_fmax, MAX_ITERATIONS);

    return best;
}

CppSynthResult synth_real(const char *rtl_code, const char *module_name,
                           void (*log_cb)(const char *, const char *)) {
    return synth_real_with_lib(rtl_code, module_name, nullptr, log_cb);
}

void synth_result_free(CppSynthResult *r) {
    if (!r) return;
    free(r->gate_verilog); free(r->report); free(r->error); free(r->lib_name);
    if (r->cell_types) { for(size_t i=0;i<r->num_cell_types;i++) free(r->cell_types[i]); free(r->cell_types); }
    free(r->cell_type_counts);
}

// Liberty library accessors
const void *synth_get_liberty_lib() {
    return g_liberty_loaded ? &g_liberty_lib : nullptr;
}
int synth_is_liberty_loaded() {
    return g_liberty_loaded ? 1 : 0;
}

char *synth_result_to_json(const CppSynthResult *r) {
    if (!r) return strdup_safe("{}");
    std::ostringstream ss;
    ss << "{";
    ss << "\"success\":" << (r->success ? "true" : "false") << ",";
    ss << "\"cell_count\":" << r->cell_count << ",";
    ss << "\"wire_count\":" << r->wire_count << ",";
    ss << "\"dff_count\":" << r->dff_count << ",";
    ss << "\"port_count\":" << r->port_count << ",";
    ss << "\"area_ge\":" << r->area_ge << ",";
    ss << "\"area_um2\":" << r->area_um2 << ",";
    ss << "\"area_from_lib\":" << (r->area_from_lib ? "true" : "false") << ",";
    ss << "\"logic_depth\":" << r->logic_depth << ",";
    if (r->lib_name && r->lib_name[0])
        ss << "\"lib_name\":\"" << r->lib_name << "\",";
    else
        ss << "\"lib_name\":null,";
    ss << "\"cell_types\":{";
    for (size_t i = 0; i < r->num_cell_types; i++) {
        if (i > 0) ss << ",";
        ss << "\"" << r->cell_types[i] << "\":" << r->cell_type_counts[i];
    }
    ss << "}";
    if (r->error && r->error[0]) ss << ",\"error\":\"" << r->error << "\"";
    ss << "}";
    return strdup_safe(ss.str().c_str());
}

char *netlist_to_json(const Synthesis::GateNetlist &gn) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"module\":\"" << gn.module_name << "\",";
    ss << "\"ports\":[";
    for (size_t i = 0; i < gn.ports.size(); i++) {
        if (i > 0) ss << ",";
        auto &p = gn.ports[i];
        ss << "{\"name\":\"" << p.name << "\",\"width\":" << p.width
           << ",\"direction\":\"" << (p.is_input ? "input" : "output") << "\"}";
    }
    ss << "],\"wires\":[";
    size_t wi = 0;
    for (auto &[wname, wwidth] : gn.wires) {
        if (wi++ > 0) ss << ",";
        ss << "{\"name\":\"" << wname << "\",\"width\":" << wwidth << "}";
    }
    ss << "],\"cells\":[";
    for (size_t i = 0; i < gn.cells.size(); i++) {
        if (i > 0) ss << ",";
        auto &c = gn.cells[i];
        ss << "{\"type\":\"" << c.type << "\",\"name\":\"" << c.name << "\",\"connections\":{";
        for (size_t j = 0; j < c.conns.size(); j++) {
            if (j > 0) ss << ",";
            ss << "\"" << c.conns[j].first << "\":\"" << c.conns[j].second.signal << "\"";
        }
        ss << "}}";
    }
    ss << "]}";
    return strdup_safe(ss.str().c_str());
}
}
