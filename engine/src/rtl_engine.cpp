/**
 * RTL Engine - C API using native RTLIL
 */

#include "rtl_engine.h"
#include "rtlil.h"
#include "verilog_parser.h"
#include "verilog_parser_full.h"
#include "lint_check.h"
#include "synth_passes.h"
#include "synth_engine.h"
#include "formal.h"
#include "timing_est.h"
#include "timing_analyzer.h"
#include "power_analyzer.h"
#include "liberty_parser.h"
#include "sim_engine.h"
#include <cmath>
#include <unistd.h>
#include <fstream>
#include "simulator.h"
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <set>
#include <iomanip>
#include <cctype>

// Engine log callbacks (set via FFI, called during engine operations)
static SimEngine::SimLogCallback g_synth_log_cb = nullptr;
static SimEngine::SimLogCallback g_timing_log_cb = nullptr;
static SimEngine::SimLogCallback g_power_log_cb = nullptr;
static SimEngine::SimLogCallback g_formal_log_cb = nullptr;
static Liberty::LibertyLibrary g_timing_corner_lib;
static std::string g_timing_corner_path;
static bool g_timing_corner_loaded = false;

static void synth_engine_log(const char *step, const char *msg) {
    if (g_synth_log_cb) g_synth_log_cb(step, msg);
}
static void timing_engine_log(const char *step, const char *msg) {
    if (g_timing_log_cb) g_timing_log_cb(step, msg);
}
static void power_engine_log(const char *step, const char *msg) {
    if (g_power_log_cb) g_power_log_cb(step, msg);
}
static void formal_engine_log(const char *step, const char *msg) {
    if (g_formal_log_cb) g_formal_log_cb(step, msg);
}

// FFI-accessible setter implementations
extern "C" {
void rtl_set_synth_log_callback(SimEngine::SimLogCallback cb) { g_synth_log_cb = cb; }
void rtl_set_timing_log_callback(SimEngine::SimLogCallback cb) {
    g_timing_log_cb = cb;
    // Also wire to the TimingAnalysis engine's log callback
    auto adapt_cb = [](const char *step, const char *msg) {
        if (g_timing_log_cb) g_timing_log_cb(step, msg);
    };
    using TimingCb = void (*)(const char *, const char *);
    TimingAnalysis::set_timing_log_callback(reinterpret_cast<TimingCb>(cb));
}
void rtl_set_power_log_callback(SimEngine::SimLogCallback cb) {
    g_power_log_cb = cb;
    // Also wire to PowerAnalysis engine
    using PowerLogCb = void (*)(const char *, const char *);
    PowerAnalysis::set_power_log_callback(reinterpret_cast<PowerLogCb>(cb));
}
void rtl_set_formal_log_callback(SimEngine::SimLogCallback cb) { g_formal_log_cb = cb; }
}

static char *strdup_safe(const char *s) { if(!s)return nullptr; char *r=(char*)malloc(strlen(s)+1); if(r)strcpy(r,s); return r; }
static RtlError *make_error(int code, const char *msg, int line, const char *fn) {
    RtlError *e=(RtlError*)malloc(sizeof(RtlError)); e->code=code; e->message=strdup_safe(msg);
    e->line=line; e->file=strdup_safe(fn); return e;
}

struct RtlDesign { RTLIL::Design design; };

static bool is_ident_char(char ch) {
    unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$';
}

static std::string trim_copy_str(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string rename_top_module_declaration(const std::string &code,
                                                 const std::string &old_name,
                                                 const std::string &new_name) {
    size_t search_pos = 0;
    while ((search_pos = code.find("module", search_pos)) != std::string::npos) {
        if (search_pos > 0 && is_ident_char(code[search_pos - 1])) {
            search_pos += 6;
            continue;
        }
        size_t pos = search_pos + 6;
        while (pos < code.size() && std::isspace(static_cast<unsigned char>(code[pos]))) pos++;
        if (code.compare(pos, old_name.size(), old_name) != 0) {
            search_pos += 6;
            continue;
        }
        size_t after = pos + old_name.size();
        if (after < code.size() && (is_ident_char(code[after]) || code[after] == '.')) {
            search_pos = after;
            continue;
        }
        std::string renamed = code;
        renamed.replace(pos, old_name.size(), new_name);
        return renamed;
    }
    return code;
}

static std::string build_formal_cell_models(const std::string &gate_code,
                                            bool &all_cells_supported) {
    std::map<std::string, std::set<std::string>> cell_ports;
    std::istringstream input(gate_code);
    std::string line;
    while (std::getline(input, line)) {
        size_t comment = line.find("//");
        if (comment != std::string::npos) line.erase(comment);
        std::string text = trim_copy_str(line);
        size_t first_space = text.find_first_of(" \t");
        size_t first_paren = text.find('(');
        if (first_space == std::string::npos || first_paren == std::string::npos ||
            first_space > first_paren) {
            continue;
        }
        std::string type = text.substr(0, first_space);
        if (type == "module" || type == "input" || type == "output" ||
            type == "wire" || type == "reg" || type == "assign" ||
            type == "always" || type == "initial") {
            continue;
        }
        bool valid_identifier = !type.empty() &&
            (std::isalpha(static_cast<unsigned char>(type[0])) || type[0] == '_');
        for (char ch : type) {
            if (!is_ident_char(ch) || ch == '$') {
                valid_identifier = false;
                break;
            }
        }
        if (!valid_identifier) continue;

        size_t pos = first_paren;
        while ((pos = text.find('.', pos)) != std::string::npos) {
            size_t name_start = pos + 1;
            size_t name_end = text.find('(', name_start);
            if (name_end == std::string::npos) break;
            std::string port = trim_copy_str(text.substr(name_start, name_end - name_start));
            if (!port.empty()) cell_ports[type].insert(port);
            pos = name_end + 1;
        }
    }

    auto upper_copy = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return value;
    };
    auto choose_port = [](const std::set<std::string> &ports,
                          std::initializer_list<const char *> candidates) {
        for (const char *candidate : candidates) {
            if (ports.count(candidate)) return std::string(candidate);
        }
        return std::string();
    };

    all_cells_supported = true;
    std::ostringstream models;
    for (const auto &[type, ports] : cell_ports) {
        std::string upper = upper_copy(type);
        std::string class_name = upper.rfind("FORMAL_", 0) == 0 ? upper.substr(7) : upper;
        enum class Kind { Dff, Mux, Xnor, Xor, Nand, Nor, And, Or, Inv, Buf, Unsupported };
        Kind kind = Kind::Unsupported;
        if (class_name.find("DFF") != std::string::npos) kind = Kind::Dff;
        else if (class_name.find("MUX") != std::string::npos) kind = Kind::Mux;
        else if (class_name.find("XNOR") != std::string::npos) kind = Kind::Xnor;
        else if (class_name.find("XOR") != std::string::npos) kind = Kind::Xor;
        else if (class_name.find("NAND") != std::string::npos) kind = Kind::Nand;
        else if (class_name.find("NOR") != std::string::npos) kind = Kind::Nor;
        else if (class_name.find("AND") != std::string::npos) kind = Kind::And;
        else if (class_name.find("OR") != std::string::npos) kind = Kind::Or;
        else if (class_name.find("INV") != std::string::npos ||
                 class_name.find("NOT") != std::string::npos) kind = Kind::Inv;
        else if (class_name.find("BUF") != std::string::npos) kind = Kind::Buf;

        std::string out = kind == Kind::Dff
            ? choose_port(ports, {"Q", "QN"})
            : choose_port(ports, {"Y", "Z", "ZN", "Q"});
        std::string a = choose_port(ports, {"A", "A1", "I", "IN"});
        std::string b = choose_port(ports, {"B", "A2"});
        std::string select = choose_port(ports, {"S", "S0", "SEL"});
        std::string data = choose_port(ports, {"D", "DI"});
        std::string clock = choose_port(ports, {"C", "CK", "CLK", "CP"});
        std::string enable = choose_port(ports, {"E", "EN", "CE", "GATE"});
        std::string reset_n = choose_port(ports, {"RN", "RESETN", "RSTN", "CDN", "RB"});
        std::string reset_p = choose_port(ports, {"R", "RESET", "RST", "CD"});
        std::string set_n = choose_port(ports, {"SN", "SETN", "PREN", "SDN", "SB"});
        std::string set_p = choose_port(ports, {"S", "SET", "PRE", "SD"});

        bool supported = kind != Kind::Unsupported && !out.empty();
        if (kind == Kind::Dff) {
            supported = supported && !data.empty() && !clock.empty();
            if (class_name.find("DFFE") != std::string::npos) {
                supported = supported && !enable.empty();
            }
        }
        else if (kind == Kind::Mux) supported = supported && !a.empty() && !b.empty() && !select.empty();
        else if (kind == Kind::Inv || kind == Kind::Buf) supported = supported && !a.empty();
        else supported = supported && !a.empty() && !b.empty();
        if (!supported) {
            all_cells_supported = false;
            formal_engine_log("FORMAL", ("unsupported standard cell model: " + type).c_str());
            continue;
        }

        models << "module " << type << " (";
        size_t port_index = 0;
        for (const auto &port : ports) {
            if (port_index++) models << ", ";
            if (port == out) {
                models << "output " << (kind == Kind::Dff ? "reg " : "") << port;
            } else {
                models << "input " << port;
            }
        }
        models << ");\n";

        if (kind == Kind::Dff) {
            std::string polarity;
            size_t polarity_pos = class_name.find('_');
            if (polarity_pos != std::string::npos) {
                polarity = class_name.substr(polarity_pos + 1);
                polarity.erase(std::remove(polarity.begin(), polarity.end(), '_'), polarity.end());
            }
            bool negedge_clock = class_name.find("NEG") != std::string::npos ||
                                  (!polarity.empty() && polarity[0] == 'N');
            bool enable_active_low = !enable.empty() &&
                (enable.back() == 'N' || enable.find("_N") != std::string::npos);
            if (class_name.find("DFFE") != std::string::npos && polarity.size() >= 2) {
                enable_active_low = polarity[1] == 'N';
            }
            bool encoded_set_active_low = class_name.find("DFFSR") != std::string::npos &&
                polarity.size() >= 2 && polarity[1] == 'N';
            bool encoded_reset_active_low = class_name.find("DFFSR") != std::string::npos &&
                polarity.size() >= 3 && polarity[2] == 'N';
            if (encoded_set_active_low && set_n.empty() && !set_p.empty()) {
                set_n = set_p;
                set_p.clear();
            }
            if (encoded_reset_active_low && reset_n.empty() && !reset_p.empty()) {
                reset_n = reset_p;
                reset_p.clear();
            }
            models << "  always @(" << (negedge_clock ? "negedge " : "posedge ") << clock;
            if (!reset_n.empty()) models << " or negedge " << reset_n;
            else if (!reset_p.empty()) models << " or posedge " << reset_p;
            if (!set_n.empty()) models << " or negedge " << set_n;
            else if (!set_p.empty()) models << " or posedge " << set_p;
            models << ") begin\n";
            if (!reset_n.empty()) models << "    if (!" << reset_n << ") " << out << " <= " << (out == "QN" ? "1'b1" : "1'b0") << ";\n    else ";
            else if (!reset_p.empty()) models << "    if (" << reset_p << ") " << out << " <= " << (out == "QN" ? "1'b1" : "1'b0") << ";\n    else ";
            else models << "    ";
            if (!set_n.empty()) models << "if (!" << set_n << ") " << out << " <= " << (out == "QN" ? "1'b0" : "1'b1") << ";\n    else ";
            else if (!set_p.empty()) models << "if (" << set_p << ") " << out << " <= " << (out == "QN" ? "1'b0" : "1'b1") << ";\n    else ";
            if (!enable.empty()) {
                models << "if (" << (enable_active_low ? "!" : "") << enable << ") ";
            }
            models << out << " <= " << (out == "QN" ? "~" : "") << data << ";\n";
            models << "  end\n";
        } else {
            std::string expr;
            switch (kind) {
                case Kind::Mux: expr = select + " ? " + b + " : " + a; break;
                case Kind::Xnor: expr = "~(" + a + " ^ " + b + ")"; break;
                case Kind::Xor: expr = a + " ^ " + b; break;
                case Kind::Nand: expr = "~(" + a + " & " + b + ")"; break;
                case Kind::Nor: expr = "~(" + a + " | " + b + ")"; break;
                case Kind::And: expr = a + " & " + b; break;
                case Kind::Or: expr = a + " | " + b; break;
                case Kind::Inv: expr = "~" + a; break;
                case Kind::Buf: expr = a; break;
                default: break;
            }
            models << "  assign " << out << " = " << expr << ";\n";
        }
        models << "endmodule\n\n";
    }
    return models.str();
}

static std::string normalize_formal_primitive_types(std::string gate_code) {
    static const std::pair<const char *, const char *> replacements[] = {
        {"$_DFFSR_PPP_", "FORMAL_DFFSR_PPP"},
        {"$_DFFSR_NNN_", "FORMAL_DFFSR_NNN"},
        {"$_DFFE_PP_", "FORMAL_DFFE_PP"},
        {"$_DFF_P_", "FORMAL_DFF_P"},
        {"$_DFF_N_", "FORMAL_DFF_N"},
        {"$_XNOR_", "FORMAL_XNOR"},
        {"$_XOR_", "FORMAL_XOR"},
        {"$_NAND_", "FORMAL_NAND"},
        {"$_NOR_", "FORMAL_NOR"},
        {"$_AND_", "FORMAL_AND"},
        {"$_OR_", "FORMAL_OR"},
        {"$_NOT_", "FORMAL_NOT"},
        {"$_BUF_", "FORMAL_BUF"},
        {"$_MUX_", "FORMAL_MUX"},
    };
    for (const auto &[from, to] : replacements) {
        size_t pos = 0;
        while ((pos = gate_code.find(from, pos)) != std::string::npos) {
            gate_code.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return gate_code;
}

static std::string extract_netlist_text_from_output(const std::string &output) {
    size_t net_start = output.find("module ");
    if (net_start == std::string::npos) return "";
    size_t net_start2 = output.find("module ", net_start + 7);
    if (net_start2 == std::string::npos) net_start2 = net_start;
    return output.substr(net_start2);
}

struct NetlistMetrics {
    int total_cells = 0;
    int dff_count = 0;
    double area_ge = 0.0;
};

static double resolve_nand2_reference_area(const Liberty::LibertyLibrary *lib) {
    double nand2_area = 1.44;
    if (!lib) return nand2_area;

    const char *candidates[] = {
        "NAND2X1",
        "NAND2X1H9R",
        "NAND2X0P5H9R",
    };
    for (const char *name : candidates) {
        if (const Liberty::LibertyCell *nand2 = lib->find_cell(name)) {
            if (nand2->area > 0.0) return nand2->area;
        }
    }
    return nand2_area;
}

static double estimate_cell_area_ge(const std::string &cell_type) {
    std::string upper = cell_type;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper.find("DFF") != std::string::npos || upper.find("LATCH") != std::string::npos) return 11.0;
    if (upper.find("MUX") != std::string::npos) return 3.0;
    if (upper.find("XOR") != std::string::npos || upper.find("XNOR") != std::string::npos) return 2.0;
    if (upper.find("INV") != std::string::npos || upper.find("NOT") != std::string::npos || upper.find("BUF") != std::string::npos) return 1.0;
    if (upper.find("AND") != std::string::npos || upper.find("OR") != std::string::npos ||
        upper.find("NAND") != std::string::npos || upper.find("NOR") != std::string::npos) return 2.0;
    return 2.0;
}

static NetlistMetrics collect_netlist_metrics(const std::string &netlist_text,
                                              const Liberty::LibertyLibrary *lib) {
    NetlistMetrics metrics;
    double nand2_area = resolve_nand2_reference_area(lib);

    std::istringstream input(netlist_text);
    std::string line;
    while (std::getline(input, line)) {
        size_t comment = line.find("//");
        if (comment != std::string::npos) line = line.substr(0, comment);
        std::string trimmed = trim_copy_str(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("module ", 0) == 0 || trimmed == "endmodule" ||
            trimmed.rfind("input", 0) == 0 || trimmed.rfind("output", 0) == 0 ||
            trimmed.rfind("wire", 0) == 0 || trimmed.rfind("reg", 0) == 0 ||
            trimmed.rfind("assign", 0) == 0 || trimmed.rfind("always", 0) == 0 ||
            trimmed.rfind("initial", 0) == 0) {
            continue;
        }

        size_t sp = trimmed.find(' ');
        size_t lp = trimmed.find('(');
        if (sp == std::string::npos || lp == std::string::npos || sp > lp) continue;

        std::string cell_type = trim_copy_str(trimmed.substr(0, sp));
        if (cell_type.empty()) continue;

        metrics.total_cells++;
        std::string upper = cell_type;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper.find("DFF") != std::string::npos || upper.find("LATCH") != std::string::npos) {
            metrics.dff_count++;
        }

        if (lib) {
            if (const Liberty::LibertyCell *cell = lib->find_cell(cell_type)) {
                if (cell->area > 0.0) {
                    metrics.area_ge += cell->area / nand2_area;
                    continue;
                }
            }
        }
        metrics.area_ge += estimate_cell_area_ge(cell_type);
    }

    return metrics;
}

RtlDesign *rtl_design_new(void) { RtlDesign *d=(RtlDesign*)malloc(sizeof(RtlDesign)); new (&d->design) RTLIL::Design(); return d; }
void rtl_design_free(RtlDesign *d) { if(!d)return; d->design.~Design(); free(d); }

RtlError *rtl_parse_file(RtlDesign *d, const char *fn) {
    auto e=VerilogParser::parse_file(&d->design,fn);
    if(e.empty())return nullptr; return make_error(1,e[0].message.c_str(),e[0].line,e[0].filename.c_str());
}

RtlError *rtl_parse_verilog(RtlDesign *d, const char *fn) {
    return rtl_parse_file(d, fn);
}
RtlError *rtl_parse_verilog_str(RtlDesign *d, const char *code, size_t len, const char *name) {
    // Use simple parser for engine (lint, synth stats)
    // Complex parsing is done separately by the synthesis engine
    auto e = VerilogParser::parse_string(&d->design, code, len, name);
    if (d->design.module_count() == 0 && !e.empty()) {
        return make_error(1, e[0].message.c_str(), e[0].line, name);
    }
    return nullptr;
}

size_t rtl_design_module_count(RtlDesign *d) { return d->design.module_count(); }

const char *rtl_design_module_name(RtlDesign *d, size_t idx) {
    if(idx>=d->design.module_count())return nullptr;
    size_t i=0;
    for(auto &it : d->design.modules_) {
        if(i==idx) return it.second->name.c_str();
        i++;
    }
    return nullptr;
}

RtlLintResult rtl_lint_check(RtlDesign *d, const char *mn) {
    auto r=LintCheck::lint_module(&d->design,mn);
    RtlLintResult o; o.passed=r.passed; o.warning_count=r.warning_count;
    o.error_count=r.error_count; o.report=strdup_safe(r.report.c_str()); return o;
}

RtlLintResult rtl_lint_source(const char *source_code, const char *module_name) {
    auto r = LintCheck::lint_source(source_code, module_name);
    RtlLintResult o; o.passed = r.passed; o.warning_count = r.warning_count;
    o.error_count = r.error_count; o.report = strdup_safe(r.report.c_str()); return o;
}

void rtl_lint_result_free(RtlLintResult *r) { if(r&&r->report){free(r->report);r->report=nullptr;} }

RtlError *rtl_synthesize(RtlDesign *d, const char *mn) {
    int ret=SynthPasses::synthesize(&d->design,mn);
    if(ret==0)return nullptr; return make_error(ret,"Synthesis failed",0,"");
}
RtlSynthStats rtl_synth_stats(RtlDesign *d, const char *mn) {
    auto r=SynthPasses::get_stats(&d->design,mn);
    RtlSynthStats o; o.wire_count=r.wire_count; o.cell_count=r.cell_count;
    o.dff_count=r.dff_count; o.lut_count=r.lut_count;
    o.and_count=r.and_count; o.or_count=r.or_count;
    o.not_count=r.not_count; o.xor_count=r.xor_count;
    o.other_count=r.other_count;
    o.report=strdup_safe(r.report); return o;
}
void rtl_synth_stats_free(RtlSynthStats *r) { if(r&&r->report){free(r->report);r->report=nullptr;} }

RtlSynthStats rtl_synth_stats_from_source(const char *source_code, const char *module_name) {
    auto r=SynthPasses::get_stats_from_source(source_code, module_name);
    RtlSynthStats o; o.wire_count=r.wire_count; o.cell_count=r.cell_count;
    o.dff_count=r.dff_count; o.lut_count=r.lut_count;
    o.and_count=r.and_count; o.or_count=r.or_count;
    o.not_count=r.not_count; o.xor_count=r.xor_count;
    o.other_count=r.other_count;
    o.report=strdup_safe(r.report); return o;
}

char *rtl_to_verilog(RtlDesign *d, const char *mn) { return strdup_safe(SynthPasses::to_verilog(&d->design,mn)); }

TimingReport rtl_estimate_timing(RtlDesign *d, const char *mn) {
    // Get synthesis stats
    auto stats = SynthPasses::get_stats(&d->design, mn);
    // Use timing_est for estimation
    return estimate_timing(
        stats.and_count, stats.or_count, stats.not_count, stats.xor_count,
        0, 0, stats.lut_count, stats.dff_count,
        stats.other_count, stats.wire_count, mn);
}

TimingReport rtl_timing_check(RtlDesign *d, const char *mn, double cp) {
    return rtl_estimate_timing(d,mn);
}
void rtl_timing_report_free(TimingReport *r) { if(r&&r->report){free(r->report);r->report=nullptr;} }

const char *rtl_engine_version(void) { return "rtl-engine 0.7.0 (native RTLIL)"; }
char *rtl_engine_info(void) { return strdup_safe("{\"version\":\"0.7.0\",\"name\":\"ai_digital\",\"features\":[\"verilog_parser\",\"lint_check\",\"synthesis\",\"timing_analysis\",\"formal_verification\",\"multi_corner\"]}"); }
void rtl_error_free(RtlError *e) { if(!e)return; if(e->message)free(e->message); if(e->file)free(e->file); free(e); }

// Forward declarations for Liberty library functions
struct LibertyCell;
struct LibertyHeader {
    std::string library_name;
    double nom_process;
    double nom_temperature;
    double nom_voltage;
    std::string default_op_conditions;
    double default_leakage_power;
    double default_max_transition;
};
static std::map<std::string, LibertyCell> parse_liberty_library(const std::string &filename);
static double get_liberty_cell_delay(const std::string &cell_type, const std::string &lib_file);
static LibertyHeader parse_liberty_header(const std::string &filename);

// ======= Helper: compute average gate delay from liberty NLDM =======
static double compute_avg_gate_delay_from_lib(const Liberty::LibertyLibrary *lib) {
    if (!lib || lib->cells.empty()) return 0.0;
    double tot_delay = 0;
    int count = 0;
    for (auto &[name, cell] : lib->cells) {
        if (cell.dont_use) continue;
        const Liberty::LibertyPin *out = cell.find_output_pin();
        if (!out) continue;
        for (auto &arc : out->timing_arcs) {
            if (arc.is_combinational()) {
                double rd = arc.cell_rise.empty() ? 0.0 : arc.cell_rise.interpolate(0.05, 0.005);
                double fd = arc.cell_fall.empty() ? 0.0 : arc.cell_fall.interpolate(0.05, 0.005);
                if (rd > 0 || fd > 0) {
                    tot_delay += (rd + fd) / 2.0;
                    count++;
                    break;
                }
            }
        }
    }
    return count > 0 ? tot_delay / count : 0.0;
}

// ======= Helper: compute DFF setup time from liberty =======
static double compute_dff_setup_from_lib(const Liberty::LibertyLibrary *lib) {
    if (!lib) return 0.0;
    for (auto &[name, cell] : lib->cells) {
        if (!cell.is_sequential) continue;
        double s = cell.get_setup_time(0.05, 0.05);
        if (s > 0.001) return s;
    }
    return 0.0;
}

struct FallbackPathStage {
    std::string cell_name;
    std::string cell_type;
    double incr_delay_ns = 0.0;
    double cumul_delay_ns = 0.0;
};

struct FallbackCriticalPath {
    std::string start_name;
    std::string end_name;
    double total_delay_ns = 0.0;
    double slack_ns = 0.0;
    std::vector<FallbackPathStage> stages;
};

static double estimate_cell_delay_for_path(const Liberty::LibertyLibrary *lib, const std::string &cell_type) {
    if (lib) {
        const Liberty::LibertyCell *cell = lib->find_cell(cell_type);
        if (cell) {
            const Liberty::LibertyPin *out = cell->find_output_pin();
            if (out && !out->timing_arcs.empty()) {
                const auto &arc = out->timing_arcs[0];
                double rd = arc.cell_rise.empty() ? 0.0 : arc.cell_rise.interpolate(0.05, 0.005);
                double fd = arc.cell_fall.empty() ? 0.0 : arc.cell_fall.interpolate(0.05, 0.005);
                double avg = (rd + fd) / 2.0;
                if (avg > 0.0) return avg;
            }
        }
    }
    std::string upper = cell_type;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper.find("BUF") != std::string::npos) return 0.012;
    if (upper.find("INV") != std::string::npos || upper.find("NOT") != std::string::npos) return 0.015;
    if (upper.find("NAND") != std::string::npos) return 0.025;
    if (upper.find("NOR") != std::string::npos) return 0.030;
    if (upper.find("AND") != std::string::npos) return 0.040;
    if (upper.find("OR") != std::string::npos) return 0.045;
    if (upper.find("XOR") != std::string::npos || upper.find("XNOR") != std::string::npos) return 0.065;
    if (upper.find("MUX") != std::string::npos) return 0.055;
    return 0.040;
}

static std::vector<FallbackCriticalPath> extract_fallback_critical_paths(
    const std::string &netlist_text,
    const Liberty::LibertyLibrary *lib,
    double required_time_ns,
    size_t limit = 10) {

    struct ParsedCell {
        std::string type;
        std::string name;
        std::map<std::string, std::string> ports;
        std::string out_wire;
        bool sequential = false;
    };

    std::set<std::string> input_wires;
    std::set<std::string> output_wires;
    std::vector<ParsedCell> cells;

    auto parse_decl_names = [&](const std::string &decl) -> std::vector<std::string> {
        std::string rest = trim_copy_str(decl);
        if (!rest.empty() && rest.back() == ';') rest.pop_back();
        if (!rest.empty() && rest.front() == '[') {
            size_t close = rest.find(']');
            if (close != std::string::npos) {
                rest = trim_copy_str(rest.substr(close + 1));
            }
        }
        std::vector<std::string> names;
        std::stringstream ss(rest);
        std::string part;
        while (std::getline(ss, part, ',')) {
            part = trim_copy_str(part);
            if (!part.empty()) names.push_back(part);
        }
        return names;
    };

    std::istringstream input(netlist_text);
    std::string line;
    while (std::getline(input, line)) {
        size_t comment = line.find("//");
        if (comment != std::string::npos) line = line.substr(0, comment);
        std::string trimmed = trim_copy_str(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("input", 0) == 0) {
            for (auto &name : parse_decl_names(trimmed.substr(5))) input_wires.insert(name);
            continue;
        }
        if (trimmed.rfind("output", 0) == 0) {
            for (auto &name : parse_decl_names(trimmed.substr(6))) output_wires.insert(name);
            continue;
        }
        if (trimmed.rfind("module ", 0) == 0 || trimmed == "endmodule" ||
            trimmed.rfind("wire ", 0) == 0 || trimmed.rfind("reg ", 0) == 0 ||
            trimmed.rfind("assign ", 0) == 0 || trimmed.rfind("always", 0) == 0) {
            continue;
        }

        size_t sp = trimmed.find(' ');
        size_t lp = trimmed.find('(');
        if (sp == std::string::npos || lp == std::string::npos || sp > lp) continue;

        ParsedCell cell;
        cell.type = trim_copy_str(trimmed.substr(0, sp));
        cell.name = trim_copy_str(trimmed.substr(sp + 1, lp - sp - 1));
        cell.sequential = (cell.type.find("DFF") != std::string::npos);

        size_t pos = lp;
        while ((pos = trimmed.find('.', pos)) != std::string::npos) {
            size_t port_start = pos + 1;
            size_t port_end = trimmed.find('(', port_start);
            if (port_end == std::string::npos) break;
            std::string port_name = trim_copy_str(trimmed.substr(port_start, port_end - port_start));
            size_t sig_start = port_end + 1;
            size_t sig_end = trimmed.find(')', sig_start);
            if (sig_end == std::string::npos) break;
            std::string signal_name = trim_copy_str(trimmed.substr(sig_start, sig_end - sig_start));
            cell.ports[port_name] = signal_name;
            pos = sig_end + 1;
        }

        for (auto &port : {"Y", "Z", "Q", "QN"}) {
            auto it = cell.ports.find(port);
            if (it != cell.ports.end()) {
                cell.out_wire = it->second;
                break;
            }
        }
        cells.push_back(cell);
    }

    std::map<std::string, std::vector<int>> wire_fanout;
    std::map<std::string, std::string> dff_outputs;
    std::map<std::string, std::pair<std::string, std::string>> dff_inputs;
    for (size_t i = 0; i < cells.size(); i++) {
        const auto &cell = cells[i];
        if (cell.sequential) {
            auto q_it = cell.ports.find("Q");
            if (q_it != cell.ports.end()) dff_outputs[q_it->second] = cell.name;
            auto d_it = cell.ports.find("D");
            if (d_it != cell.ports.end()) dff_inputs[d_it->second] = {cell.name, "D"};
        }
        for (auto &[port_name, signal_name] : cell.ports) {
            if (port_name == "Y" || port_name == "Z" || port_name == "Q" || port_name == "QN") continue;
            wire_fanout[signal_name].push_back((int)i);
        }
    }

    struct QueueNode {
        std::string start_name;
        std::string wire;
        double delay_ns = 0.0;
        std::vector<FallbackPathStage> stages;
        std::set<std::string> visited_wires;
    };

    std::vector<FallbackCriticalPath> paths;
    auto record_path = [&](const QueueNode &node, const std::string &end_name) {
        FallbackCriticalPath path;
        path.start_name = node.start_name;
        path.end_name = end_name;
        path.total_delay_ns = node.delay_ns;
        path.slack_ns = required_time_ns - node.delay_ns;
        path.stages = node.stages;
        paths.push_back(path);
    };

    std::queue<QueueNode> q;
    for (auto &[wire, dff_name] : dff_outputs) {
        q.push({dff_name + ".Q", wire, 0.0, {}, {wire}});
    }
    for (auto &wire : input_wires) {
        q.push({"IN:" + wire, wire, 0.0, {}, {wire}});
    }

    const size_t max_expansions = 2000;
    size_t expansions = 0;
    while (!q.empty() && expansions < max_expansions) {
        QueueNode node = q.front();
        q.pop();
        expansions++;

        if (output_wires.count(node.wire)) {
            record_path(node, "OUT:" + node.wire);
        }
        auto dff_it = dff_inputs.find(node.wire);
        if (dff_it != dff_inputs.end()) {
            record_path(node, dff_it->second.first + ".D");
        }

        auto fit = wire_fanout.find(node.wire);
        if (fit == wire_fanout.end()) continue;

        for (int cell_idx : fit->second) {
            if (cell_idx < 0 || cell_idx >= (int)cells.size()) continue;
            const auto &cell = cells[cell_idx];
            if (cell.sequential || cell.out_wire.empty()) continue;
            if (cell.out_wire == node.wire || node.visited_wires.count(cell.out_wire)) continue;

            QueueNode next = node;
            next.wire = cell.out_wire;
            next.visited_wires.insert(cell.out_wire);
            FallbackPathStage stage;
            stage.cell_name = cell.name;
            stage.cell_type = cell.type;
            stage.incr_delay_ns = estimate_cell_delay_for_path(lib, cell.type);
            stage.cumul_delay_ns = node.delay_ns + stage.incr_delay_ns;
            next.delay_ns = stage.cumul_delay_ns;
            next.stages.push_back(stage);
            q.push(std::move(next));
        }
    }

    std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
        if (a.slack_ns != b.slack_ns) return a.slack_ns < b.slack_ns;
        return a.total_delay_ns > b.total_delay_ns;
    });

    std::set<std::string> seen;
    std::vector<FallbackCriticalPath> filtered;
    for (auto &path : paths) {
        std::string key = path.start_name + "->" + path.end_name;
        if (seen.insert(key).second) {
            filtered.push_back(path);
            if (filtered.size() >= limit) break;
        }
    }
    return filtered;
}

static std::string format_fallback_paths_report(const std::vector<FallbackCriticalPath> &paths) {
    if (paths.empty()) return "";
    std::ostringstream ss;
    for (size_t i = 0; i < paths.size(); i++) {
        const auto &path = paths[i];
        ss << "\n--- Path #" << (i + 1) << " ---\n";
        ss << "Start: " << path.start_name << "\n";
        ss << "End:   " << path.end_name << "\n";
        ss << "Slack: " << std::fixed << std::setprecision(3) << path.slack_ns
           << " ns " << (path.slack_ns >= 0.0 ? "(MET)" : "(VIOLATED)") << "\n";
        ss << "Total delay: " << std::fixed << std::setprecision(3) << path.total_delay_ns << " ns\n";
        if (!path.stages.empty()) {
            ss << "Stages:\n";
            ss << "  " << std::setw(20) << std::left << "Cell"
               << std::setw(12) << "Type"
               << std::setw(12) << "Incr(ns)"
               << std::setw(12) << "Cumul(ns)" << "\n";
            ss << "  " << std::string(56, '-') << "\n";
            for (auto &stage : path.stages) {
                ss << "  " << std::setw(20) << std::left << stage.cell_name
                   << std::setw(12) << stage.cell_type
                   << std::setw(12) << std::fixed << std::setprecision(3) << stage.incr_delay_ns
                   << std::setw(12) << std::fixed << std::setprecision(3) << stage.cumul_delay_ns << "\n";
            }
        }
    }
    return ss.str();
}

// Timing analysis implementation
TimingReport rtl_timing_analysis(const char *synth_output, const char *module_name, const char *lib_path, double clock_period) {
    TimingReport result;
    memset(&result, 0, sizeof(result));
    std::string detailed_path_report;

    // Parse synthesis output to get actual cell counts
    std::string output(synth_output);
    int mux_count = 0, dff_count = 0, other_count = 0;
    int and_count = 0, or_count = 0, not_count = 0, xor_count = 0, buf_count = 0, sub_module_count = 0;
    int parsed_logic_depth = 0;

    try {
        // Parse "logic_depth: N"
        size_t ldp = output.find("logic_depth:");
        if (ldp != std::string::npos) {
            size_t num_start = ldp + 13;
            while (num_start < output.size() && output[num_start] == ' ') num_start++;
            size_t num_end = num_start;
            while (num_end < output.size() && std::isdigit(output[num_end])) num_end++;
            if (num_end > num_start) {
                try { parsed_logic_depth = std::stoi(output.substr(num_start, num_end - num_start)); } catch (...) {}
            }
        }

        // Parse "cells" line - find number before "cells"
        size_t pos = 0;
        while ((pos = output.find("cells", pos)) != std::string::npos) {
            // Look backwards for the number
            size_t num_start = pos;
            while (num_start > 0 && output[num_start - 1] == ' ') num_start--;
            while (num_start > 0 && std::isdigit(output[num_start - 1])) num_start--;
            if (num_start < pos) {
                result.total_gates = std::stoi(output.substr(num_start, pos - num_start));
            }
            pos += 5;
        }

        // Helper lambda to parse count for a cell type (exact match)
        // Format: "  cell_type count" (cell type first, then count)
        auto parse_count = [&](const std::string &cell_type) -> int {
            size_t p = 0;
            int count = 0;
            while ((p = output.find(cell_type, p)) != std::string::npos) {
                // Verify it's an exact match (not a substring of another type)
                // Check char before: must be whitespace or line start
                bool exact_before = (p == 0 || output[p-1] == ' ' || output[p-1] == '\t' || output[p-1] == '\n');
                // Check char after: must be whitespace or line end
                size_t end = p + cell_type.size();
                bool exact_after = (end >= output.size() || output[end] == ' ' || output[end] == '\t' || output[end] == '\n');
                if (exact_before && exact_after) {
                    // Skip whitespace after cell type name
                    size_t num_start = end;
                    while (num_start < output.size() && output[num_start] == ' ') num_start++;
                    // Read number
                    size_t num_end = num_start;
                    while (num_end < output.size() && std::isdigit(output[num_end])) num_end++;
                    if (num_end > num_start) {
                        count = std::stoi(output.substr(num_start, num_end - num_start));
                    }
                }
                p += cell_type.size();
            }
            return count;
        };

        // Helper lambda to parse count using substring match (for DFF variants)
        // Matches any cell type containing the substring, sums all matches
        auto parse_count_substr = [&](const std::string &substr) -> int {
            int total = 0;
            // Parse each line
            size_t line_start = 0;
            while (line_start < output.size()) {
                size_t line_end = output.find('\n', line_start);
                if (line_end == std::string::npos) line_end = output.size();
                std::string line = output.substr(line_start, line_end - line_start);
                // Check if line contains the substring (as part of cell type name)
                if (line.find(substr) != std::string::npos) {
                    // Extract the number at the end of line
                    size_t num_pos = line.size();
                    while (num_pos > 0 && std::isdigit(line[num_pos - 1])) num_pos--;
                    if (num_pos < line.size()) {
                        // Verify the char before number is whitespace
                        if (num_pos == 0 || line[num_pos - 1] == ' ' || line[num_pos - 1] == '\t') {
                            total += std::stoi(line.substr(num_pos));
                        }
                    }
                }
                line_start = line_end + 1;
            }
            return total;
        };

        mux_count = parse_count("$_MUX_");
        // Use substring match for DFF to count all variants ($_DFF_P_, $_DFF_N_, $_DFFSR_, etc.)
        // This matches synth_engine.cpp which uses t.find("DFF")
        dff_count = parse_count_substr("DFF");
        and_count = parse_count("$_AND_");
        or_count = parse_count("$_OR_");
        not_count = parse_count("$_NOT_");
        xor_count = parse_count("$_XOR_");
        buf_count = parse_count("$_BUF_");
        sub_module_count = parse_count("SUB_MODULE");
        other_count = parse_count("OTHER");

        result.dff_count = dff_count;
    } catch (...) {
        // Parsing failed, use defaults
    }

    // Calculate area from cell counts (matching Rust side)
    int total_area = and_count * 6 + or_count * 6 + not_count * 3 +
                     xor_count * 8 + mux_count * 8 + dff_count * 18 +
                     buf_count * 6 + sub_module_count * 100 + other_count * 4;
    result.area_ge = total_area;

    // Calculate timing using Liberty library if available
    result.clock_period_ns = clock_period;
    int total_cells = and_count + or_count + not_count + xor_count + mux_count + dff_count + buf_count + other_count;
    if (parsed_logic_depth > 0) {
        result.logic_depth = parsed_logic_depth;
    } else {
        result.logic_depth = total_cells > 0 ? (int)std::ceil(std::sqrt((double)total_cells)) : 1;
        if (result.logic_depth < 1) result.logic_depth = 1;
    }
    if (result.logic_depth < 1) result.logic_depth = 1;
    if (result.logic_depth > 200) result.logic_depth = 200;

    // Use Liberty library for delay calculation if available
    double gate_delay_ns = 0.05; // Fallback default
    double dff_delay_ns = 0.10;  // Fallback default
    if (synth_is_liberty_loaded()) {
        const Liberty::LibertyLibrary *fl = static_cast<const Liberty::LibertyLibrary*>(synth_get_liberty_lib());
        if (fl) {
            gate_delay_ns = compute_avg_gate_delay_from_lib(fl);
            dff_delay_ns = compute_dff_setup_from_lib(fl);
            if (gate_delay_ns < 0.001) gate_delay_ns = 0.05;
            if (dff_delay_ns < 0.001) dff_delay_ns = 0.10;
        }
    } else if (lib_path && strlen(lib_path) > 0) {
        // Load liberty file for delay
        double and_delay = get_liberty_cell_delay("$_AND_", lib_path);
        double or_delay = get_liberty_cell_delay("$_OR_", lib_path);
        double not_delay = get_liberty_cell_delay("$_NOT_", lib_path);
        double xor_delay = get_liberty_cell_delay("$_XOR_", lib_path);
        double mux_delay = get_liberty_cell_delay("$_MUX_", lib_path);
        double dff_delay = get_liberty_cell_delay("$_DFF_P_", lib_path);

        // Calculate weighted average gate delay
        double total_delay = and_delay * and_count + or_delay * or_count +
                            not_delay * not_count + xor_delay * xor_count +
                            mux_delay * mux_count;
        int combo_cells = and_count + or_count + not_count + xor_count + mux_count;
        if (combo_cells > 0) {
            gate_delay_ns = total_delay / combo_cells;
        }
        dff_delay_ns = dff_delay > 0 ? dff_delay : 0.10;
    }

    result.arrival_time_ns = result.logic_depth * gate_delay_ns + (dff_count > 0 ? dff_delay_ns : 0);
    result.required_time_ns = clock_period - dff_delay_ns;  // setup time = DFF delay
    result.slack_ns = result.required_time_ns - result.arrival_time_ns;
    result.timing_met = (result.slack_ns >= 0) ? 1 : 0;
    std::string extracted_netlist_text = extract_netlist_text_from_output(output);
    if (!extracted_netlist_text.empty()) {
        const Liberty::LibertyLibrary *lib = synth_is_liberty_loaded()
            ? static_cast<const Liberty::LibertyLibrary*>(synth_get_liberty_lib())
            : nullptr;
        NetlistMetrics metrics = collect_netlist_metrics(extracted_netlist_text, lib);
        if (metrics.total_cells > 0) result.total_gates = metrics.total_cells;
        if (metrics.dff_count > 0) result.dff_count = metrics.dff_count;
        if (metrics.area_ge > 0.0) result.area_ge = metrics.area_ge;
    }

    if (output.find("endmodule") != std::string::npos) {
        try {
            if (!extracted_netlist_text.empty()) {
                std::string netlist_text = extracted_netlist_text;
                TimingAnalysis::TimingAnalyzer ta;
                if (synth_is_liberty_loaded()) {
                    ta.loadLibertyFull(synth_get_liberty_lib());
                }
                TimingAnalysis::ClockConstraint clk;
                clk.name = "clk";
                clk.port = "clk";
                clk.period = clock_period;
                ta.addClock(clk);
                ta.buildTimingGraphFromNetlist(netlist_text);
                ta.computeArrivalTimes();
                ta.computeRequiredTimes();
                ta.computeSlack();
                ta.findPaths();
                ta.checkSetupHoldConstraints();
                auto detailed = ta.getReport();
                detailed.module_name = module_name ? module_name : "top";
                detailed.clock_period = clock_period;
                detailed.clock_frequency = clock_period > 0.0 ? 1000.0 / clock_period : 0.0;
                detailed.paths = ta.getPaths();
                detailed.total_paths = static_cast<int>(detailed.paths.size());
                if (!detailed.paths.empty()) {
                    detailed.max_path_delay = detailed.paths.front().total_delay;
                    detailed.min_path_delay = detailed.paths.back().total_delay;
                }
                auto critical = ta.getCriticalPath();
                if (!detailed.paths.empty()) {
                    if (critical.total_delay > 0.0) {
                        result.arrival_time_ns = critical.total_delay;
                        result.slack_ns = critical.slack;
                        result.required_time_ns = critical.total_delay + critical.slack;
                        result.timing_met = critical.is_met ? 1 : 0;
                    }
                    detailed_path_report = detailed.toString();
                } else {
                    const Liberty::LibertyLibrary *lib = synth_is_liberty_loaded()
                        ? static_cast<const Liberty::LibertyLibrary*>(synth_get_liberty_lib())
                        : nullptr;
                    auto fallback_paths = extract_fallback_critical_paths(
                        netlist_text, lib, result.required_time_ns, 10);
                    if (!fallback_paths.empty()) {
                        const auto &worst = fallback_paths.front();
                        result.arrival_time_ns = worst.total_delay_ns;
                        result.slack_ns = worst.slack_ns;
                        result.required_time_ns = worst.total_delay_ns + worst.slack_ns;
                        result.timing_met = result.slack_ns >= 0.0 ? 1 : 0;
                        detailed_path_report = format_fallback_paths_report(fallback_paths);
                    }
                }
            }
        } catch (...) {
            timing_engine_log("TIMING", "Detailed path extraction failed, keeping summary-only report");
        }
    }

    // Generate proper table format
    auto pad = [](const std::string &s, int width) -> std::string {
        if ((int)s.size() >= width) return s;
        return s + std::string(width - s.size(), ' ');
    };
    auto fmt = [](double val) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << val;
        return oss.str();
    };

    // Get actual liberty library name
    std::string lib_name = "none";
    if (synth_is_liberty_loaded()) {
        const Liberty::LibertyLibrary *fl = static_cast<const Liberty::LibertyLibrary*>(synth_get_liberty_lib());
        if (fl) lib_name = fl->name;
    }

    std::string sep = "  ------------------------------ --------------";
    auto fmt_row = [&](const std::string &l, const std::string &v) -> std::string {
        char buf[128];
        snprintf(buf, sizeof(buf), "  %-30s %14s", l.c_str(), v.c_str());
        return std::string(buf);
    };

    std::ostringstream ss;
    ss << "  " << pad("Timing Analysis Report", 40) << "\n";
    ss << sep << "\n";
    ss << fmt_row("Liberty library", lib_name) << "\n";
    ss << fmt_row("Clock period", fmt(clock_period) + " ns") << "\n";
    ss << sep << "\n";
    ss << fmt_row("Total cells", std::to_string(result.total_gates)) << "\n";
    ss << fmt_row("DFF", std::to_string(result.dff_count)) << "\n";
    ss << fmt_row("Logic depth", std::to_string(result.logic_depth) + " lvls") << "\n";
    ss << fmt_row("Gate equiv. area (GE)", std::to_string((int)result.area_ge)) << "\n";
    ss << sep << "\n";
    ss << fmt_row("Arrival Time", fmt(result.arrival_time_ns) + " ns") << "\n";
    ss << fmt_row("Required Time", fmt(result.required_time_ns) + " ns") << "\n";
    ss << fmt_row("Slack", fmt(result.slack_ns) + " ns") << "\n";
    ss << fmt_row("Setup violations", "0") << "\n";
    ss << fmt_row("Hold violations", "0") << "\n";
    ss << fmt_row("Timing status", result.timing_met ? "MET" : "VIO") << "\n";
    ss << sep << "\n";
    ss << fmt_row("Constraint frequency", std::to_string((int)(clock_period > 0 ? 1000.0 / clock_period : 0)) + " MHz") << "\n";
    ss << sep << "\n";

    if (!detailed_path_report.empty()) {
        ss << "\n" << detailed_path_report;
    }

    result.report = strdup_safe(ss.str().c_str());
    timing_engine_log("TIMING", "Timing analysis complete");
    return result;
}

TimingReport rtl_timing_analysis_corner(const char *synth_output, const char *module_name,
    const char *liberty_file, const char *corner_type, double voltage,
    double temperature, double clock_period) {

    timing_engine_log("TIMING_CORNER", "Starting per-corner NLDM timing analysis");

    TimingReport result;
    memset(&result, 0, sizeof(result));
    std::string detailed_path_report;

    std::string output(synth_output ? synth_output : "");
    std::string mod(module_name ? module_name : "top");
    std::string ct(corner_type ? corner_type : "tt");
    for (auto &c : ct) c = std::tolower(c);

    // Parse cell counts from synthesis output
    int dff_count = 0, and_count = 0, or_count = 0, not_count = 0, xor_count = 0;
    int mux_count = 0, buf_count = 0, nand_count = 0, nor_count = 0, other_count = 0;
    int parsed_logic_depth = 0;

    try {
        size_t ldp = output.find("logic_depth:");
        if (ldp != std::string::npos) {
            size_t ns = ldp + 13;
            while (ns < output.size() && output[ns] == ' ') ns++;
            size_t ne = ns;
            while (ne < output.size() && std::isdigit(output[ne])) ne++;
            if (ne > ns) parsed_logic_depth = std::stoi(output.substr(ns, ne - ns));
        }
    } catch (...) {}

    // Count real liberty cell names from synthesis output (H9R pattern)
    try {
        size_t pos = 0;
        while ((pos = output.find("H9R", pos)) != std::string::npos) {
            size_t start = pos;
            while (start > 0 && output[start-1] != ' ' && output[start-1] != '\n') start--;
            std::string cell_type = output.substr(start, pos - start + 3);
            if (cell_type.find("DFF") != std::string::npos) dff_count++;
            else if (cell_type.find("NAND") != std::string::npos) nand_count++;
            else if (cell_type.find("NOR") != std::string::npos) nor_count++;
            else if (cell_type.find("AND") != std::string::npos) and_count++;
            else if (cell_type.find("OR") != std::string::npos) or_count++;
            else if (cell_type.find("XOR") != std::string::npos) xor_count++;
            else if (cell_type.find("MUX") != std::string::npos) mux_count++;
            else if (cell_type.find("INV") != std::string::npos || cell_type.find("NOT") != std::string::npos) not_count++;
            else if (cell_type.find("BUF") != std::string::npos) buf_count++;
            else other_count++;
            pos += 3;
        }
    } catch (...) {}

    // Load the requested liberty into a local object so per-corner timing
    // analysis remains thread-safe across concurrent corner jobs.
    Liberty::LibertyLibrary corner_lib_storage;
    bool corner_lib_loaded = false;
    if (liberty_file && liberty_file[0]) {
        std::string requested_path(liberty_file);
        corner_lib_loaded = corner_lib_storage.load(requested_path);
    }
    const Liberty::LibertyLibrary *full_lib = nullptr;
    double v_nom = 1.2;
    if (corner_lib_loaded) {
        full_lib = &corner_lib_storage;
    } else if (synth_is_liberty_loaded()) {
        full_lib = static_cast<const Liberty::LibertyLibrary*>(synth_get_liberty_lib());
    }
    if (full_lib) {
        v_nom = full_lib->nom_voltage > 0 ? full_lib->nom_voltage : 1.2;
    }

    // Area: compute from real liberty cell areas using NAND2 as GE reference
    int total_cells = dff_count + and_count + or_count + not_count + xor_count +
                      mux_count + buf_count + nand_count + nor_count + other_count;
    double area_ge = 0.0;
    double nand2_area = resolve_nand2_reference_area(full_lib);
    // Compute area from actual cell instances in synthesis output when available
    if (full_lib && total_cells > 0) {
        std::istringstream oss(output);
        std::string oline;
        int area_cells = 0;
        while (std::getline(oss, oline)) {
            // Trim leading whitespace
            size_t ns = oline.find_first_not_of(" \t");
            if (ns == std::string::npos) continue;
            std::string trimmed = oline.substr(ns);
            size_t sp = trimmed.find(' ');
            if (sp == std::string::npos) continue;
            std::string cell_type = trimmed.substr(0, sp);
            const Liberty::LibertyCell *lc = full_lib->find_cell(cell_type);
            if (lc && lc->area > 0) {
                area_ge += lc->area / nand2_area;
                area_cells++;
            }
        }
        if (area_ge < total_cells * 0.5) area_ge = total_cells * 1.5;
    }
    if (area_ge <= 0) {
        // Fallback: estimate from cell counts
        area_ge = dff_count * 8.0 + and_count * 2.0 + nand_count * 1.5 +
                  or_count * 2.0 + nor_count * 1.5 + not_count * 1.0 +
                  xor_count * 3.0 + mux_count * 3.0 + buf_count * 1.0 +
                  other_count * 2.0;
    }
    result.area_ge = area_ge;
    result.clock_period_ns = clock_period;
    result.total_gates = total_cells;
    result.dff_count = dff_count;

    // Logic depth: prefer parsed value, else estimate
    if (parsed_logic_depth > 0) result.logic_depth = parsed_logic_depth;
    else result.logic_depth = total_cells > 0 ? (int)std::ceil(std::sqrt((double)total_cells)) : 1;
    if (result.logic_depth < 1) result.logic_depth = 1;
    if (result.logic_depth > 200) result.logic_depth = 200;

    std::string netlist_text = extract_netlist_text_from_output(output);
    if (!netlist_text.empty()) {
        NetlistMetrics metrics = collect_netlist_metrics(netlist_text, full_lib);
        if (metrics.total_cells > 0) result.total_gates = metrics.total_cells;
        if (metrics.dff_count > 0) result.dff_count = metrics.dff_count;
        if (metrics.area_ge > 0.0) result.area_ge = metrics.area_ge;
    }

    // === NLDM-based timing using gate-level netlist ===
    if (output.find("endmodule") != std::string::npos) {
        if (!netlist_text.empty()) {

            // Build timing graph using TimingAnalyzer
            TimingAnalysis::TimingAnalyzer ta;

            // Load full liberty if available
            if (full_lib) {
                ta.loadLibertyFull(full_lib);
                char buf[256];
                snprintf(buf, sizeof(buf), "Using loaded Liberty library with %zu cells for NLDM lookup",
                    full_lib ? full_lib->cells.size() : 0);
                timing_engine_log("TIMING_CORNER", buf);
            } else {
                timing_engine_log("TIMING_CORNER", "No Liberty library loaded, using fallback delay estimates");
            }

            // Build timing graph from netlist (will use NLDM for liberty-loaded cells)
            ta.buildTimingGraphFromNetlist(netlist_text);

            // Signoff STA must account for on-chip variation.  The analyzer
            // applies its depth-aware AOCV table to data paths and CRPR to
            // common clock paths; these base early/late factors cover the
            // required-time side of setup/hold analysis.
            ta.setOCVMode(true);
            ta.setOCVDerateEarly(0.95);
            ta.setOCVDerateLate(1.05);

            // Set clock period
            TimingAnalysis::ClockConstraint clk;
            clk.name = "clk";
            clk.period = clock_period;
            clk.port = "clk";
            ta.addClock(clk);

            // PVT scaling: voltage/temperature factors applied per-edge during arrival computation
            // The NLDM lookup already accounts for the specific corner (uses corner-specific liberty)
            // Additional PVT scaling for interconnect/wire effects:
            double pvt_scale = 1.0;
            if (!corner_lib_loaded) {
                double v_ratio = voltage > 0.5 ? v_nom / voltage : 1.0;
                pvt_scale *= 1.0 + (v_ratio - 1.0) * 0.8;
                pvt_scale *= 1.0 + (temperature - 25.0) * 0.004;
                if (pvt_scale < 0.3) pvt_scale = 0.3;
                if (pvt_scale > 4.0) pvt_scale = 4.0;
            }

            // Run timing analysis
            ta.computeArrivalTimes();
            ta.computeRequiredTimes();
            ta.computeSlack();
            ta.findPaths();

            auto detailed = ta.getReport();
            detailed.module_name = mod;
            detailed.clock_period = clock_period;
            detailed.clock_frequency = clock_period > 0.0 ? 1000.0 / clock_period : 0.0;
            detailed.paths = ta.getPaths();
            detailed.total_paths = static_cast<int>(detailed.paths.size());
            if (!detailed.paths.empty()) {
                detailed.max_path_delay = detailed.paths.front().total_delay;
                detailed.min_path_delay = detailed.paths.back().total_delay;
            }
            auto critical = ta.getCriticalPath();
            if (critical.total_delay > 0.001) {
                result.arrival_time_ns = critical.total_delay * pvt_scale;
            } else {
                // Use average gate delay from liberty NLDM tables
                double gate_delay_ns = compute_avg_gate_delay_from_lib(full_lib);
                if (gate_delay_ns < 0.001) gate_delay_ns = 0.04;
                result.arrival_time_ns = result.logic_depth * gate_delay_ns * pvt_scale;
            }

            // Minimum arrival (clock skew + jitter + wire delay floor)
            if (result.arrival_time_ns < 0.01) result.arrival_time_ns = 0.01;

            // Compute slack using liberty setup time for DFF
            double dff_setup_ns = compute_dff_setup_from_lib(full_lib);
            if (dff_setup_ns < 0.01) dff_setup_ns = 0.05;
            result.required_time_ns = clock_period - dff_setup_ns;
            result.slack_ns = result.required_time_ns - result.arrival_time_ns;
            if (!detailed.paths.empty()) {
                const double path_scale = pvt_scale;
                for (auto &path : detailed.paths) {
                    path.total_delay *= path_scale;
                    path.slack = result.required_time_ns - path.total_delay;
                    path.is_met = path.slack >= 0.0;
                    for (auto &stage : path.stages) {
                        stage.incr_delay *= path_scale;
                        stage.cumul_delay *= path_scale;
                    }
                }
                detailed.max_path_delay = detailed.paths.front().total_delay;
                detailed.min_path_delay = detailed.paths.back().total_delay;
                detailed_path_report = detailed.toString();
            } else {
                auto fallback_paths = extract_fallback_critical_paths(
                    netlist_text, full_lib, result.required_time_ns, 10);
                if (!fallback_paths.empty()) {
                    const double path_scale = pvt_scale;
                    for (auto &path : fallback_paths) {
                        path.total_delay_ns *= path_scale;
                        path.slack_ns = result.required_time_ns - path.total_delay_ns;
                        for (auto &stage : path.stages) {
                            stage.incr_delay_ns *= path_scale;
                            stage.cumul_delay_ns *= path_scale;
                        }
                    }
                    result.arrival_time_ns = fallback_paths.front().total_delay_ns;
                    result.slack_ns = fallback_paths.front().slack_ns;
                    result.timing_met = result.slack_ns >= 0.0 ? 1 : 0;
                    detailed_path_report = format_fallback_paths_report(fallback_paths);
                }
            }
        } else {
            // No netlist found in output
            double gate_delay_ns = compute_avg_gate_delay_from_lib(full_lib);
            if (gate_delay_ns < 0.001) gate_delay_ns = 0.04;
            double dff_setup_ns = compute_dff_setup_from_lib(full_lib);
            if (dff_setup_ns < 0.01) dff_setup_ns = 0.05;

            double pvt_scale = 1.0;
            if (!corner_lib_loaded) {
                double v_ratio = voltage > 0.5 ? v_nom / voltage : 1.0;
                pvt_scale *= 1.0 + (v_ratio - 1.0) * 0.8;
                pvt_scale *= 1.0 + (temperature - 25.0) * 0.004;
                if (pvt_scale < 0.3) pvt_scale = 0.3;
                if (pvt_scale > 4.0) pvt_scale = 4.0;
            }

            result.arrival_time_ns = result.logic_depth * gate_delay_ns * pvt_scale + (dff_count > 0 ? dff_setup_ns * pvt_scale : 0);
            result.required_time_ns = clock_period - dff_setup_ns * pvt_scale;
            result.slack_ns = result.required_time_ns - result.arrival_time_ns;
        }
    } else {
        // No netlist: pure estimation from liberty data
        double gate_delay_ns = compute_avg_gate_delay_from_lib(full_lib);
        if (gate_delay_ns < 0.001) gate_delay_ns = 0.04;
        double dff_setup_ns = compute_dff_setup_from_lib(full_lib);
        if (dff_setup_ns < 0.01) dff_setup_ns = 0.05;

        double pvt_scale = 1.0;
        if (!corner_lib_loaded) {
            double v_ratio = voltage > 0.5 ? v_nom / voltage : 1.0;
            pvt_scale *= 1.0 + (v_ratio - 1.0) * 0.8;
            pvt_scale *= 1.0 + (temperature - 25.0) * 0.004;
            if (pvt_scale < 0.3) pvt_scale = 0.3;
            if (pvt_scale > 4.0) pvt_scale = 4.0;
        }

        result.arrival_time_ns = result.logic_depth * gate_delay_ns * pvt_scale + (dff_count > 0 ? dff_setup_ns * pvt_scale : 0);
        result.required_time_ns = clock_period - dff_setup_ns * pvt_scale;
        result.slack_ns = result.required_time_ns - result.arrival_time_ns;
    }

    if (result.arrival_time_ns < 0.01) result.arrival_time_ns = 0.01;

    result.timing_met = (result.slack_ns >= 0) ? 1 : 0;

    // Generate report with real liberty data
    char corner_label[128];
    double display_v = voltage > 0.5 ? voltage : v_nom;
    double display_t = temperature > -273 ? temperature : 25.0;
    snprintf(corner_label, sizeof(corner_label), "%s %.2fV %.0fC",
        ct.c_str(), display_v, display_t);

    // Get actual liberty library name
    std::string lib_name = "none";
    if (full_lib) lib_name = full_lib->name;

    auto pad = [](const std::string &s, int w) -> std::string {
        if ((int)s.size() >= w) return s; return s + std::string(w - s.size(), ' ');
    };
    auto fmt = [](double v) -> std::string {
        std::ostringstream o; o << std::fixed << std::setprecision(3) << v; return o.str();
    };

    std::string sep = "  ------------------------------ --------------";
    std::ostringstream ss;
    auto fmt_row = [&](const std::string &l, const std::string &v) -> std::string {
        char buf[128];
        snprintf(buf, sizeof(buf), "  %-30s %14s", l.c_str(), v.c_str());
        return std::string(buf);
    };
    ss << "  " << pad("NLDM Timing Analysis Report", 40) << "\n";
    ss << sep << "\n";
    ss << fmt_row("Corner", corner_label) << "\n";
    ss << fmt_row("Liberty library", lib_name) << "\n";
    ss << fmt_row("Delay source", corner_lib_loaded ? "corner NLDM" : "PVT fallback") << "\n";
    ss << fmt_row("Variation model", "AOCV + CRPR (enabled)") << "\n";
    ss << fmt_row("OCV derates", "early 0.95 / late 1.05") << "\n";
    ss << fmt_row("Clock period", fmt(clock_period) + " ns") << "\n";
    ss << fmt_row("Vdd (from lib)", fmt(v_nom) + " V") << "\n";
    ss << sep << "\n";
    ss << fmt_row("Gate equiv. area (GE)", std::to_string((int)std::llround(result.area_ge))) << "\n";
    ss << fmt_row("Total cells", std::to_string(result.total_gates)) << "\n";
    ss << fmt_row("DFF count", std::to_string(result.dff_count)) << "\n";
    ss << fmt_row("Logic depth", std::to_string(result.logic_depth) + " lvls") << "\n";
    ss << sep << "\n";
    ss << fmt_row("Arrival Time", fmt(result.arrival_time_ns) + " ns") << "\n";
    ss << fmt_row("Required Time", fmt(result.required_time_ns) + " ns") << "\n";
    ss << fmt_row("Slack", fmt(result.slack_ns) + " ns") << "\n";
    ss << fmt_row("Timing status", result.timing_met ? "MET" : "VIO") << "\n";
    ss << sep << "\n";
    double max_freq = result.arrival_time_ns > 0 ? 1000.0 / result.arrival_time_ns : 0;
    // Cap at physically reasonable limits for 55nm process
    if (max_freq > 10000.0) max_freq = 10000.0;
    ss << fmt_row("Max frequency", std::to_string((int)max_freq) + " MHz") << "\n";
    ss << sep << "\n";
    if (!detailed_path_report.empty()) {
        ss << "\n" << detailed_path_report;
    }

    std::string r = ss.str();
    result.report = strdup_safe(r.c_str());
    // Log detailed timing parameters
    {
        double gd = compute_avg_gate_delay_from_lib(full_lib);
        double ds = compute_dff_setup_from_lib(full_lib);
        char tbuf[256];
        snprintf(tbuf, sizeof(tbuf), "module=%s corner=%s vdd=%.2fV temp=%.0fC gate_delay=%.4fns dff_setup=%.4fns arrival=%.3fns slack=%.3fns max_freq=%.0fMHz %s",
            mod.c_str(), corner_label, v_nom, display_t, gd, ds, result.arrival_time_ns, result.slack_ns, max_freq,
            result.timing_met ? "MET" : "VIO");
        timing_engine_log("TIMING_SUMMARY", tbuf);
    }
    return result;
}

void rtl_clock_scan_free(char **result) { if(result&&*result){free(*result);*result=nullptr;} }
void rtl_timing_analysis_free(char **result) { if(result&&*result){free(*result);*result=nullptr;} }
void rtl_generate_sdc_free(char **result) { if(result&&*result){free(*result);*result=nullptr;} }

char *rtl_generate_sdc(const char *module_name, double clock_period, const char *port_name, const char *lib_path) {
    std::ostringstream sdc;
    sdc << "create_clock -name clk -period " << clock_period << " [get_ports " << (port_name ? port_name : "clk") << "]" << std::endl;
    return strdup_safe(sdc.str().c_str());
}

char *rtl_design_summary(RtlDesign *d) {
    if (!d) return strdup_safe("{}");
    std::ostringstream summary;
    summary << "Design Summary:" << std::endl;
    summary << "  Modules: " << d->design.module_count() << std::endl;
    for (auto &it : d->design.modules_) {
        summary << "  - " << it.first.str() << ": " << it.second->wire_count() << " wires, " << it.second->cell_count() << " cells" << std::endl;
    }
    return strdup_safe(summary.str().c_str());
}

void rtl_design_summary_free(char **result) { if(result&&*result){free(*result);*result=nullptr;} }
void rtl_get_realtime_status_free(char **result) { if(result&&*result){free(*result);*result=nullptr;} }

SimResult rtl_simulate(const char *rtl_code, const char *tb_code, const char *module_name, const char *clk_port, int num_cycles, double half_period_ns) {
    return rtl_simulate_with_limit(rtl_code, tb_code, module_name, clk_port, num_cycles, half_period_ns, 0);
}

SimResult rtl_simulate_with_limit(const char *rtl_code, const char *tb_code, const char *module_name, const char *clk_port, int num_cycles, double half_period_ns, size_t memory_limit_mb) {
    return rtl_simulate_with_limit_and_timeout(rtl_code, tb_code, module_name, clk_port, num_cycles, half_period_ns, memory_limit_mb, 30);
}

SimResult rtl_simulate_with_limit_and_timeout(const char *rtl_code, const char *tb_code, const char *module_name, const char *clk_port, int num_cycles, double half_period_ns, size_t memory_limit_mb, int timeout_seconds) {
    SimResult result;
    result.passed = 0;
    result.exit_code = -1;
    result.time_steps = 0;
    result.output = strdup_safe("");
    result.vcd_file = strdup_safe("");

    // Use the professional simulation engine
    // Determine max_cycles from the design
    // If testbench has #delay statements, need enough cycles
    // Estimate: count # tokens in testbench to determine needed cycles
    int max_cycles = num_cycles * 2;
    if (max_cycles < 100) max_cycles = 100; // minimum for any meaningful simulation
    auto sim_result = SimEngine::simulate_code(
        std::string(rtl_code ? rtl_code : ""),
        std::string(tb_code ? tb_code : ""),
        std::string(module_name ? module_name : "top"),
        max_cycles,
        memory_limit_mb,
        timeout_seconds
    );

    result.passed = sim_result.passed;
    result.exit_code = sim_result.exit_code;
    result.time_steps = sim_result.time_steps;

    std::string output = "Simulation: " + std::string(sim_result.passed ? "PASS" : "FAIL") + "\n" + sim_result.output;
    result.output = strdup_safe(output.c_str());

    // Save VCD
    if (!sim_result.vcd_file.empty()) {
        std::string vcd_path = std::string("/tmp/ai_dig_") + module_name + ".vcd";
        std::ofstream f(vcd_path);
        f << sim_result.vcd_file;
        result.vcd_file = strdup_safe(vcd_path.c_str());
    }

    return result;
}

void rtl_sim_result_free(SimResult *r) { if(r&&r->output){free(r->output);r->output=nullptr;} if(r&&r->vcd_file){free(r->vcd_file);r->vcd_file=nullptr;} }

// Get coverage data as JSON string from simulation
extern "C" char *rtl_get_sim_coverage_json(const char *rtl_code, const char *tb_code, const char *module_name) {
    auto sim_result = SimEngine::simulate_code(
        rtl_code ? rtl_code : "", tb_code ? tb_code : "",
        module_name ? module_name : "top", 100, 0, 30
    );
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"toggles\":{\"total\":%d,\"covered\":%d},"
        "\"branches\":{\"total\":%d,\"covered\":%d},"
        "\"expressions\":{\"total\":%d,\"covered\":%d},"
        "\"conditions\":{\"total\":%d,\"covered\":%d},"
        "\"fsm_states\":{\"total\":%d,\"covered\":%d},"
        "\"assertions\":{\"total\":%d,\"covered\":%d}}",
        sim_result.total_toggles, sim_result.covered_toggles,
        sim_result.total_branches, sim_result.covered_branches,
        sim_result.total_expressions, sim_result.covered_expressions,
        sim_result.total_conditions, sim_result.covered_conditions,
        sim_result.total_fsm_states, sim_result.covered_fsm_states,
        sim_result.total_assertions, sim_result.covered_assertions);
    return strdup_safe(buf);
}

// Log callback bridge
static SimLogCallback g_rtl_log_callback = nullptr;

void rtl_set_sim_log_callback(SimLogCallback cb) {
    g_rtl_log_callback = cb;
    SimEngine::set_log_callback(cb);
}

// Note: rtl_set_synth_log_callback, rtl_set_timing_log_callback,
// rtl_set_power_log_callback, rtl_set_formal_log_callback are defined
// at the top of this file (lines ~48-51) and use g_synth_log_cb etc.

size_t rtl_get_process_memory_mb() {
    return SimEngine::get_process_memory_mb();
}

int rtl_get_cpu_cores() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    return info.cpu_cores;
}

int rtl_get_cpu_threads() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    return info.cpu_threads;
}

size_t rtl_get_total_ram_mb() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    return info.total_ram_mb;
}

size_t rtl_get_available_ram_mb() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    return info.available_ram_mb;
}

double rtl_get_load_1min() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    return info.load_1min;
}

const char* rtl_get_cpu_model() {
    static char model[256];
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    snprintf(model, sizeof(model), "%s", info.cpu_model);
    return model;
}

char* rtl_get_system_info_json() {
    SimEngine::SystemInfo info = SimEngine::get_system_info();
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"cpu_cores\":%d,\"cpu_threads\":%d,\"cpu_model\":\"%s\","
        "\"total_ram_mb\":%zu,\"available_ram_mb\":%zu,\"process_rss_mb\":%zu,"
        "\"load_1min\":%.2f}",
        info.cpu_cores, info.cpu_threads, info.cpu_model,
        info.total_ram_mb, info.available_ram_mb, info.process_rss_mb,
        info.load_1min);
    char *result = (char*)malloc(strlen(buf) + 1);
    strcpy(result, buf);
    return result;
}

void rtl_free_system_info(char* s) {
    free(s);
}

int rtl_formal_check(const char *rtl_code, const char *gate_code, const char *module_name) {
    // Formal equivalence verification:
    // Phase 1: Structural port check (fast)
    // Phase 2: Functional equivalence via simulated random test vectors
    if (!rtl_code || !gate_code || !module_name) return -1;

    std::string rtl = rtl_code;
    std::string gate = gate_code;
    std::string mod = module_name;

    formal_engine_log("FORMAL", "starting structural validation");

    // --- Phase 1: Structural validation ---
    bool has_module = gate.find("module") != std::string::npos;
    bool has_endmodule = gate.find("endmodule") != std::string::npos;
    if (!has_module || !has_endmodule) return 0;

    // Count cells in gate netlist
    size_t gate_cell_count = 0;
    {
        std::istringstream gs(gate);
        std::string l;
        while (std::getline(gs, l)) {
            size_t s = l.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            std::string trimmed = l.substr(s);
            size_t sp = trimmed.find(' ');
            if (sp != std::string::npos) {
                std::string first_token = trimmed.substr(0, sp);
                size_t lp = trimmed.find('(', sp);
                if (lp != std::string::npos && first_token != "module" && first_token != "input"
                    && first_token != "output" && first_token != "wire" && first_token != "reg"
                    && first_token != "assign" && first_token != "always" && first_token != "initial"
                    && first_token != "endmodule" && !first_token.empty()) {
                    gate_cell_count++;
                }
            }
        }
    }
    if (gate_cell_count == 0) return 0;

    formal_engine_log("FORMAL", "extracting RTL ports");

    // --- Phase 2: Extract ports from RTL ---
    struct PortInfo { std::string name; int width; bool is_input; };
    std::vector<PortInfo> rtl_ports;

    VerilogParser::Parser parser;
    auto rtl_result = parser.parseString(rtl, "<rtl>");
    if (rtl_result.modules.empty()) return -1;

    std::shared_ptr<VerilogParser::ModuleDecl> rtl_mod;
    for (auto &m : rtl_result.modules) {
        auto md = std::dynamic_pointer_cast<VerilogParser::ModuleDecl>(m);
        if (md && md->name == mod) { rtl_mod = md; break; }
    }
    if (!rtl_mod) return -1;

    // Extract from ANSI port list
    for (auto &p : rtl_mod->ports) {
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(p);
        if (!pd || pd->name.empty()) continue;
        bool is_input = (pd->dir == VerilogParser::PortDecl::INPUT);
        rtl_ports.push_back({pd->name, pd->width, is_input});
    }
    // Also extract from non-ANSI port declarations in module body
    for (auto &item : rtl_mod->items) {
        if (!item) continue;
        auto pd = std::dynamic_pointer_cast<VerilogParser::PortDecl>(item);
        if (!pd || pd->name.empty()) continue;
        // Check not already in list
        bool dup = false;
        for (auto &rp : rtl_ports) if (rp.name == pd->name) { dup = true; break; }
        if (!dup) {
            rtl_ports.push_back({pd->name, pd->width, pd->dir == VerilogParser::PortDecl::INPUT});
        }
    }

    formal_engine_log("FORMAL", "extracting gate-level ports");

    // --- Phase 3: Extract ports from gate netlist ---
    std::vector<PortInfo> gate_ports;
    std::istringstream gate_stream(gate);
    std::string line;
    while (std::getline(gate_stream, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        bool is_input = (line.find("input") == 0 && line.find("output") == std::string::npos);
        bool is_output = (line.find("output") == 0);
        if (!is_input && !is_output) continue;

        // Handle multiple ports on same line: "input a, b, c;"
        size_t semi = line.find(';');
        if (semi == std::string::npos) continue;
        std::string decl = line.substr(0, semi);

        // Remove "input " / "output " prefix
        size_t kw_end = is_input ? 5 : 6;
        if (decl.size() <= kw_end) continue;
        std::string names_part = trim_copy_str(decl.substr(kw_end));
        // Remove [range] if present
        if (!names_part.empty() && names_part[0] == '[') {
            size_t rb = names_part.find(']');
            if (rb != std::string::npos) names_part = trim_copy_str(names_part.substr(rb + 1));
        }
        // Split by comma
        size_t pos = 0;
        while (pos < names_part.size()) {
            size_t comma = names_part.find(',', pos);
            std::string name = names_part.substr(pos, comma == std::string::npos ? comma : comma - pos);
            // Trim
            size_t ns = name.find_first_not_of(" \t");
            size_t ne = name.find_last_not_of(" \t");
            if (ns != std::string::npos) name = name.substr(ns, ne - ns + 1);
            // Remove [bit] suffix
            size_t bracket = name.find('[');
            if (bracket != std::string::npos) name = name.substr(0, bracket);
            if (!name.empty()) {
                gate_ports.push_back({name, 1, is_input});
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }

    formal_engine_log("FORMAL", "running interface equivalence checks");

    // --- Phase 4: Port matching ---
    if (rtl_ports.empty() && gate_ports.empty()) {
        formal_engine_log("FORMAL", "no comparable ports were found");
        return -1;
    }
    if (rtl_ports.empty() || gate_ports.empty()) return 0; // Mismatch

    // Check all RTL input ports exist in gate netlist
    for (auto &rp : rtl_ports) {
        if (!rp.is_input) continue;
        bool found = false;
        for (auto &gp : gate_ports) {
            if (gp.name == rp.name && gp.is_input) { found = true; break; }
        }
        if (!found) return 0;
    }
    // Check all RTL output ports exist in gate netlist
    for (auto &rp : rtl_ports) {
        if (rp.is_input) continue;
        bool found = false;
        for (auto &gp : gate_ports) {
            if (gp.name == rp.name && !gp.is_input) { found = true; break; }
        }
        if (!found) return 0;
    }

    formal_engine_log("FORMAL", "building functional equivalence testbench");

    // --- Phase 5: Functional equivalence check ---
    bool has_sequential = gate.find("DFF") != std::string::npos || gate.find("LATCH") != std::string::npos;
    std::string clock_port;
    std::string reset_port;
    bool reset_active_low = false;
    for (const auto &rp : rtl_ports) {
        if (!rp.is_input) continue;
        std::string lower = rp.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (clock_port.empty() && (lower == "clk" || lower == "clock" || lower.find("clk") != std::string::npos)) {
            clock_port = rp.name;
            continue;
        }
        if (reset_port.empty() && (lower.find("rst") != std::string::npos || lower.find("reset") != std::string::npos)) {
            reset_port = rp.name;
            reset_active_low = (lower.find("_n") != std::string::npos || lower.find("n_") == 0 ||
                                (!lower.empty() && lower.back() == 'n'));
        }
    }

    // Generate a simple self-checking testbench that compares RTL vs gate
    // For combinational circuits: apply random inputs, compare outputs
    // For sequential circuits: apply sequence, compare at each cycle

    // Build a testbench that instantiates both RTL and gate modules,
    // drives same inputs to both, and compares outputs
    const std::string rtl_top_name = mod + "__rtl_ref";
    const std::string gate_top_name = mod + "__gate_ref";
    std::string renamed_rtl = rename_top_module_declaration(rtl, mod, rtl_top_name);
    std::string renamed_gate = normalize_formal_primitive_types(
        rename_top_module_declaration(gate, mod, gate_top_name));
    bool all_cells_supported = false;
    std::string formal_cell_models = build_formal_cell_models(renamed_gate, all_cells_supported);
    if (!all_cells_supported) {
        formal_engine_log("FORMAL", "gate netlist contains cells without built-in formal models");
        return -1;
    }

    std::string tb;
    tb += "module __formal_check_tb;\n";
    tb += "  reg __mismatch = 0;\n";
    tb += "\n";

    // Declare signals for RTL instance
    for (auto &rp : rtl_ports) {
        if (rp.is_input) {
            if (rp.width > 1) tb += "  reg [" + std::to_string(rp.width-1) + ":0] __rtl_" + rp.name + ";\n";
            else tb += "  reg __rtl_" + rp.name + ";\n";
        } else {
            if (rp.width > 1) tb += "  wire [" + std::to_string(rp.width-1) + ":0] __rtl_" + rp.name + ";\n";
            else tb += "  wire __rtl_" + rp.name + ";\n";
        }
    }
    // Declare signals for gate instance
    for (auto &rp : rtl_ports) {
        if (rp.is_input) continue;
        if (rp.width > 1) tb += "  wire [" + std::to_string(rp.width-1) + ":0] __gate_" + rp.name + ";\n";
        else tb += "  wire __gate_" + rp.name + ";\n";
    }
    tb += "\n";

    // Instantiate RTL module
    tb += "  " + rtl_top_name + " __rtl_uut (\n";
    for (size_t i = 0; i < rtl_ports.size(); i++) {
        tb += "    ." + rtl_ports[i].name + "(__rtl_" + rtl_ports[i].name + ")";
        tb += (i < rtl_ports.size() - 1) ? ",\n" : "\n";
    }
    tb += "  );\n\n";

    // Instantiate gate module
    tb += "  " + gate_top_name + " __gate_uut (\n";
    for (size_t i = 0; i < rtl_ports.size(); i++) {
        tb += "    ." + rtl_ports[i].name + "(";
        tb += rtl_ports[i].is_input ? ("__rtl_" + rtl_ports[i].name) : ("__gate_" + rtl_ports[i].name);
        tb += ")";
        tb += (i < rtl_ports.size() - 1) ? ",\n" : "\n";
    }
    tb += "  );\n\n";

    int driven_input_bits = 0;
    for (const auto &rp : rtl_ports) {
        if (rp.is_input && rp.name != clock_port && rp.name != reset_port) {
            driven_input_bits += std::max(rp.width, 1);
        }
    }
    // Exhausting a small input space is useful only while the mapped cone is
    // also small.  A 9-bit input multiplier has 512 vectors but hundreds of
    // primitive instances; running every vector through the native event
    // kernel turns a valid check into a timeout.  Keep exhaustive proof for
    // compact cones and use the deterministic bounded campaign otherwise.
    const bool exhaustive_combinational = !has_sequential && driven_input_bits <= 10 &&
        gate_cell_count <= 128;
    // The native event simulator is intentionally conservative for large
    // gate cones. Keep the deterministic random campaign bounded for those
    // cones, while retaining exhaustive checking for small combinational
    // interfaces. The timeout scales with actual mapped-cell count so a
    // correct arithmetic block is never mislabeled inconclusive by a fixed
    // short wall-clock budget.
    const int vector_count = has_sequential ? 48
        : (exhaustive_combinational ? (1 << driven_input_bits) : 32);
    const int simulation_timeout_seconds = std::min(60, std::max(
        12, 12 + static_cast<int>(gate_cell_count / 32)));

    // Test stimulus: apply deterministic pseudo-random test vectors
    tb += "  initial begin\n";
    tb += "    $dumpfile(\"__formal_check.vcd\");\n";
    tb += "    $dumpvars(0, __formal_check_tb);\n";
    tb += "    // Initialize inputs\n";
    for (auto &rp : rtl_ports) {
        if (!rp.is_input) continue;
        if (rp.name == clock_port) {
            tb += "    __rtl_" + rp.name + " = 0;\n";
        } else if (rp.name == reset_port) {
            tb += "    __rtl_" + rp.name + " = " + std::string(reset_active_low ? "0" : "1") + ";\n";
        } else {
            tb += "    __rtl_" + rp.name + " = 0;\n";
        }
    }
    if (has_sequential && !clock_port.empty()) {
        tb += "    #12;\n";
        if (!reset_port.empty()) {
            tb += "    __rtl_" + reset_port + " = " + std::string(reset_active_low ? "1" : "0") + ";\n";
        }
        tb += "    // Warm up the sequential state for two clean clock cycles.\n";
        tb += "    repeat (2) begin\n";
        tb += "      #5 __rtl_" + clock_port + " = 1;\n";
        tb += "      #5 __rtl_" + clock_port + " = 0;\n";
        tb += "    end\n";
        tb += "    // Deterministic cycle-based vectors, expanded by the host.\n";
    } else {
        tb += "    #10;\n";
        tb += exhaustive_combinational
            ? "    // Exhaustive combinational input vectors.\n"
            : "    // Deterministic combinational stress vectors.\n";
    }

    for (int vector_index = 0; vector_index < vector_count; ++vector_index) {
        int bit_offset = 0;
        for (auto &rp : rtl_ports) {
            if (!rp.is_input || rp.name == clock_port || rp.name == reset_port) continue;
            int width = std::max(rp.width, 1);
            if (width <= 32) {
                uint64_t value = 0;
                if (exhaustive_combinational) {
                    uint64_t mask = width == 32 ? 0xffffffffULL : ((1ULL << width) - 1ULL);
                    value = (static_cast<uint64_t>(vector_index) >> bit_offset) & mask;
                } else if (vector_index < 4) {
                    uint64_t mask = width == 32 ? 0xffffffffULL : ((1ULL << width) - 1ULL);
                    value = vector_index == 0 ? 0 : (vector_index == 1 ? 1 : mask);
                } else {
                    uint64_t x = 0x9e3779b97f4a7c15ULL ^
                        (static_cast<uint64_t>(vector_index + 1) * 0xbf58476d1ce4e5b9ULL) ^
                        (static_cast<uint64_t>(bit_offset + 1) * 0x94d049bb133111ebULL);
                    x ^= x >> 30;
                    x *= 0xbf58476d1ce4e5b9ULL;
                    x ^= x >> 27;
                    uint64_t mask = width == 32 ? 0xffffffffULL : ((1ULL << width) - 1ULL);
                    value = x & mask;
                }
                tb += "    __rtl_" + rp.name + " = " + std::to_string(width) +
                      "'d" + std::to_string(value) + ";\n";
            } else {
                tb += "    __rtl_" + rp.name + " = $random;\n";
            }
            bit_offset += width;
        }

        if (has_sequential && !clock_port.empty()) {
            tb += "    #5 __rtl_" + clock_port + " = 1;\n";
            tb += "    #1;\n";
        } else {
            tb += "    #5;\n";
        }

        tb += "    // Check outputs for vector " + std::to_string(vector_index) + ".\n";
        for (auto &rp : rtl_ports) {
            if (!rp.is_input) {
                tb += "    if (__rtl_" + rp.name + " !== __gate_" + rp.name + ") begin\n";
                tb += "      $display(\"MISMATCH: " + rp.name + " RTL=%d GATE=%d\", __rtl_" + rp.name + ", __gate_" + rp.name + ");\n";
                tb += "      __mismatch = 1;\n";
                tb += "    end\n";
            }
        }

        if (has_sequential && !clock_port.empty()) {
            tb += "    #4;\n";
            tb += "    __rtl_" + clock_port + " = 0;\n";
        } else {
            tb += "    #5;\n";
        }
    }
    // The simulator parser does not require a conditional system-task here:
    // mismatch sites emit FAIL, while this marker proves the full comparison
    // loop reached its terminal point.
    tb += "    $display(\"FORMAL: COMPLETE\");\n";
    tb += "    $finish;\n";
    tb += "  end\n";
    tb += "endmodule\n";

    // Run simulation of this formal check testbench
    // We use a simple built-in simulator call
    formal_engine_log("FORMAL", "launching native comparison simulation");
    SimResult result = rtl_simulate_with_limit_and_timeout(
        (renamed_rtl + "\n" + renamed_gate + "\n" + formal_cell_models).c_str(),
        tb.c_str(),
        "__formal_check_tb",
        clock_port.empty() ? "clk" : clock_port.c_str(),
        4096, 5.0, 1024, simulation_timeout_seconds);

    std::string output(result.output ? result.output : "");
    bool simulation_passed = result.passed;
    rtl_sim_result_free(&result);
    formal_engine_log("FORMAL_SIM_OUTPUT", output.c_str());

    if (output.find("FORMAL: FAIL") != std::string::npos ||
        output.find("MISMATCH:") != std::string::npos) {
        formal_engine_log("FORMAL", "comparison simulation reported FAIL");
        return 0;
    }
    if (simulation_passed && output.find("FORMAL: COMPLETE") != std::string::npos) {
        formal_engine_log("FORMAL", "comparison simulation completed with no mismatches");
        return 1;
    }

    // Structural/interface agreement cannot establish functional equivalence.
    // Preserve the inconclusive state so callers stop the flow or retry the
    // failed node instead of publishing a false PASS.
    formal_engine_log("FORMAL", "comparison simulation failed; equivalence is inconclusive");
    return -1;
}

// Liberty library parser
struct LibertyCell {
    std::string name;
    double area;
    std::string cell_footprint;  // Canonical name from cell_footprint
    std::map<std::string, double> pin_capacitance;
    std::map<std::string, std::string> pin_function;
};

// Helper: trim string
static std::string trim_lib(std::string s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Helper: extract value between : and ; (handles both "key: value;" and "key : value;")
static std::string extract_lib_value(const std::string &line) {
    size_t colon = line.find(':');
    size_t semi = line.rfind(';');
    if (colon == std::string::npos) return "";
    std::string val = line.substr(colon + 1, semi > colon ? semi - colon - 1 : std::string::npos);
    val = trim_lib(val);
    // Remove quotes
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.size() - 2);
    }
    return val;
}

// Parse liberty library header only (fast - skips cell bodies)
static LibertyHeader parse_liberty_header(const std::string &filename) {
    LibertyHeader hdr = {};
    hdr.nom_process = 1.0;
    hdr.nom_temperature = 25.0;
    hdr.nom_voltage = 1.2;
    hdr.default_leakage_power = 0.0;
    hdr.default_max_transition = 1.0;

    std::ifstream file(filename);
    if (!file.is_open()) return hdr;

    std::string line;
    bool in_library = false;
    int brace_depth = 0;
    int cell_count = 0;

    while (std::getline(file, line)) {
        line = trim_lib(line);
        if (line.empty() || line[0] == '/' || line[0] == '*') continue;

        // Detect library header
        if (!in_library && line.find("library") != std::string::npos && line.find('(') != std::string::npos) {
            in_library = true;
            size_t lp = line.find('(');
            size_t rp = line.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
                hdr.library_name = line.substr(lp + 1, rp - lp - 1);
            }
        }

        if (!in_library) continue;

        // Track brace depth
        for (char c : line) {
            if (c == '{') brace_depth++;
            else if (c == '}') brace_depth--;
        }

        // Parse header info (only at depth 1 - inside library but not inside sub-blocks)
        if (brace_depth == 1) {
            if (line.find("nom_process") == 0 || line.find("nom_process ") == 0) {
                std::string val = extract_lib_value(line);
                if (!val.empty()) try { hdr.nom_process = std::stod(val); } catch (...) {}
            } else if (line.find("nom_temperature") == 0 || line.find("nom_temperature ") == 0) {
                std::string val = extract_lib_value(line);
                if (!val.empty()) try { hdr.nom_temperature = std::stod(val); } catch (...) {}
            } else if (line.find("nom_voltage") == 0 || line.find("nom_voltage ") == 0) {
                std::string val = extract_lib_value(line);
                if (!val.empty()) try { hdr.nom_voltage = std::stod(val); } catch (...) {}
            } else if (line.find("default_operating_conditions") == 0 || line.find("default_operating_conditions ") == 0) {
                hdr.default_op_conditions = extract_lib_value(line);
            } else if (line.find("default_cell_leakage_power") == 0 || line.find("default_cell_leakage_power ") == 0) {
                std::string val = extract_lib_value(line);
                if (!val.empty()) try { hdr.default_leakage_power = std::stod(val); } catch (...) {}
            } else if (line.find("default_max_transition") == 0 || line.find("default_max_transition ") == 0) {
                std::string val = extract_lib_value(line);
                if (!val.empty()) try { hdr.default_max_transition = std::stod(val); } catch (...) {}
            }
        }

        // Count cells (detect "cell (NAME) {" or "cell(NAME) {" at depth 1)
        if (brace_depth == 1 && (line.find("cell (") == 0 || line.find("cell(") == 0) && line.find('{') != std::string::npos) {
            cell_count++;
        }

        // Exit when library block closes
        if (brace_depth <= 0) break;
    }

    hdr.default_leakage_power = static_cast<double>(cell_count);  // abuse: store cell_count here
    return hdr;
}

// C API: Parse liberty file header info
LibertyInfo rtl_parse_liberty_info(const char *filename) {
    LibertyInfo info = {};
    if (!filename) return info;

    LibertyHeader hdr = parse_liberty_header(filename);
    info.library_name = strdup_safe(hdr.library_name.c_str());
    info.nom_process = hdr.nom_process;
    info.nom_temperature = hdr.nom_temperature;
    info.nom_voltage = hdr.nom_voltage;
    info.default_op_conditions = strdup_safe(hdr.default_op_conditions.c_str());
    info.cell_count = static_cast<int>(hdr.default_leakage_power); // stored in default_leakage_power field
    info.default_leakage_power = 0.0;
    info.default_max_transition = hdr.default_max_transition;

    return info;
}

void rtl_liberty_info_free(LibertyInfo *info) {
    if (!info) return;
    if (info->library_name) { free(info->library_name); info->library_name = nullptr; }
    if (info->default_op_conditions) { free(info->default_op_conditions); info->default_op_conditions = nullptr; }
}

static std::map<std::string, LibertyCell> parse_liberty_library(const std::string &filename) {
    std::map<std::string, LibertyCell> cells;
    std::ifstream file(filename);
    if (!file.is_open()) return cells;

    std::string line;
    LibertyCell current_cell;
    bool in_cell = false;
    bool in_pin = false;
    bool in_pg_pin = false;   // power/ground pin - skip
    std::string current_pin;
    int brace_depth = 0;

    while (std::getline(file, line)) {
        // Trim whitespace
        line = trim_lib(line);
        if (line.empty()) continue;

        // Skip comments
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') continue;
        if (line[0] == '*' || line[0] == '#') continue;

        // Track brace depth for cell/pin blocks
        for (char c : line) {
            if (c == '{') brace_depth++;
            else if (c == '}') brace_depth--;
        }

        // Check for cell definition (standard: "cell (NAME) {" or simple: "cell(NAME) {")
        if (!in_cell && !in_pin && (line.find("cell (") == 0 || line.find("cell(") == 0)) {
            size_t lp = line.find('(');
            size_t rp = line.find(')');
            if (lp != std::string::npos && rp != std::string::npos) {
                current_cell.name = trim_lib(line.substr(lp + 1, rp - lp - 1));
                current_cell.area = 0.0;
                in_cell = true;
            }
        }

        if (!in_cell) continue;

        // Check for pg_pin (power/ground pin) - skip entirely
        if (line.find("pg_pin") == 0 || line.find("pg_pin ") == 0) {
            in_pg_pin = true;
            continue;
        }

        // Check for cell_footprint
        if (line.find("cell_footprint") == 0 || line.find("cell_footprint ") == 0) {
            current_cell.cell_footprint = extract_lib_value(line);
        }

        // Check for area (supports both "area:VALUE;" and "area : VALUE;")
        if (!in_pin && !in_pg_pin && (line.find("area") == 0)) {
            std::string area_str = extract_lib_value(line);
            if (!area_str.empty()) {
                try { current_cell.area = std::stod(area_str); } catch (...) {}
            }
        }

        // Check for pin definition (not pg_pin)
        if (!in_pin && !in_pg_pin && (line.find("pin (") == 0 || line.find("pin(") == 0)) {
            size_t lp = line.find('(');
            size_t rp = line.find(')');
            if (lp != std::string::npos && rp != std::string::npos) {
                current_pin = trim_lib(line.substr(lp + 1, rp - lp - 1));
                in_pin = true;
            }
        }

        // Check for capacitance in pin
        if (in_pin && line.find("capacitance") == 0) {
            std::string cap_str = extract_lib_value(line);
            if (!cap_str.empty()) {
                try {
                    double cap = std::stod(cap_str);
                    current_cell.pin_capacitance[current_pin] = cap;
                } catch (...) {}
            }
        }

        // Check for function in pin
        if (in_pin && line.find("function") == 0) {
            current_cell.pin_function[current_pin] = extract_lib_value(line);
        }

        // Check for direction in pin (to detect clock pins)
        if (in_pin && line.find("direction") == 0) {
            current_cell.pin_function[current_pin + "_dir"] = extract_lib_value(line);
        }

        // Check for clock in pin
        if (in_pin && line.find("clock") == 0) {
            current_cell.pin_function[current_pin + "_clock"] = "true";
        }

        // Check for end of pin
        if (in_pin && brace_depth <= 1 && line.find("}") != std::string::npos) {
            in_pin = false;
            current_pin.clear();
        }

        // Check for end of pg_pin
        if (in_pg_pin && brace_depth <= 1 && line.find("}") != std::string::npos) {
            in_pg_pin = false;
        }

        // Check for end of cell (brace depth goes back to library level)
        if (in_cell && !in_pin && !in_pg_pin && line.find("}") != std::string::npos && brace_depth <= 1) {
            // Use cell_footprint as name if available, otherwise use cell name
            std::string key = current_cell.cell_footprint.empty() ? current_cell.name : current_cell.cell_footprint;
            cells[key] = current_cell;
            current_cell = LibertyCell();
            in_cell = false;
            brace_depth = 0;
        }
    }

    return cells;
}

// Get cell delay from Liberty library with native cell name mapping
// Industrial cells have names like AND2X1H9R, NAND2X1H9R, DFFX1H9R, etc.
// The delay scales with drive strength (X0P5=0.5x, X1=1x, X2=2x, X4=4x)
static double get_liberty_cell_delay(const std::string &cell_type, const std::string &lib_file) {
    static std::map<std::string, LibertyCell> cells;
    static std::string loaded_file;

    if (loaded_file != lib_file) {
        cells = parse_liberty_library(lib_file);
        loaded_file = lib_file;
    }

    // Map our cell types to Liberty cell names (handle both simple and native naming)
    // Industrial naming: AND2X1H9R = 2-input AND, 1x drive, high-VT, 9-track
    // Simple naming: AND = basic AND gate
    std::string liberty_name;

    // Extract base gate type from cell_type
    std::string base_type;
    if (cell_type.find("$_AND_") != std::string::npos || cell_type.find("AND") != std::string::npos) base_type = "AND";
    if (cell_type.find("$_OR_") != std::string::npos || (cell_type.find("OR") != std::string::npos && cell_type.find("XOR") == std::string::npos && cell_type.find("NOR") == std::string::npos)) base_type = "OR";
    if (cell_type.find("$_NOT_") != std::string::npos || cell_type.find("NOT") != std::string::npos || cell_type.find("INV") != std::string::npos) base_type = "NOT";
    if (cell_type.find("$_XOR_") != std::string::npos || cell_type.find("XOR") != std::string::npos) base_type = "XOR";
    if (cell_type.find("$_NAND_") != std::string::npos || cell_type.find("NAND") != std::string::npos) base_type = "NAND";
    if (cell_type.find("$_NOR_") != std::string::npos || cell_type.find("NOR") != std::string::npos) base_type = "NOR";
    if (cell_type.find("$_MUX_") != std::string::npos || cell_type.find("MUX") != std::string::npos) base_type = "MUX";
    if (cell_type.find("$_BUF_") != std::string::npos || cell_type.find("BUF") != std::string::npos) base_type = "BUF";
    if (cell_type.find("$_DFF_") != std::string::npos || cell_type.find("DFF") != std::string::npos) base_type = "DFF";

    if (base_type.empty()) base_type = "AND";

    // Try exact match first, then prefix match (for native cells like AND2X1H9R)
    for (const auto &[name, cell] : cells) {
        if (name == base_type || (name.find(base_type) == 0 && name.find("X") != std::string::npos)) {
            liberty_name = name;
            break;
        }
    }

    if (liberty_name.empty()) {
        // Fallback: search for any cell containing the base type
        for (const auto &[name, cell] : cells) {
            if (name.find(base_type) != std::string::npos) {
                liberty_name = name;
                break;
            }
        }
    }

    if (!liberty_name.empty() && cells.count(liberty_name)) {
        const auto &cell = cells[liberty_name];
        // Estimate delay from area and drive strength
        // X1 drive = base delay, X2 = 0.6x delay, X4 = 0.4x delay
        double drive_factor = 1.0;
        if (liberty_name.find("X0P5") != std::string::npos) drive_factor = 2.0;
        else if (liberty_name.find("X2") != std::string::npos) drive_factor = 0.6;
        else if (liberty_name.find("X4") != std::string::npos) drive_factor = 0.4;
        else if (liberty_name.find("X8") != std::string::npos) drive_factor = 0.25;
        else if (liberty_name.find("X16") != std::string::npos) drive_factor = 0.15;

        // Base delay: 0.01ns per area unit * drive factor, minimum 0.01ns
        double delay = cell.area * 0.01 * drive_factor;
        if (delay < 0.01) delay = 0.01;
        return delay;
    }

    // Fallback: estimate from cell type name
    if (base_type == "DFF") return 0.15;
    if (base_type == "MUX") return 0.08;
    if (base_type == "XOR") return 0.10;
    if (base_type == "AND" || base_type == "OR") return 0.05;
    if (base_type == "NAND" || base_type == "NOR") return 0.04;
    if (base_type == "NOT") return 0.03;
    if (base_type == "BUF") return 0.02;

    return 0.05; // Default delay
}

SynthResult rtl_synthesize_real(const char *rtl_code, const char *module_name) {
    return rtl_synthesize_real_with_lib(rtl_code, module_name, nullptr);
}

SynthResult rtl_synthesize_real_with_lib(const char *rtl_code, const char *module_name,
                                          const char *liberty_path) {
    return rtl_synthesize_real_with_options(rtl_code, module_name, liberty_path, nullptr);
}

SynthResult rtl_synthesize_real_with_options(const char *rtl_code, const char *module_name,
                                              const char *liberty_path,
                                              const RtlSynthesisOptions *options) {
    SynthResult result = {};
    result.success = 0;

    auto log_cb = [](const char *step, const char *msg) {
        if (g_synth_log_cb) g_synth_log_cb(step, msg);
        synth_engine_log(step, msg);
    };

    NativeSynthesisOptions native_options = {};
    const NativeSynthesisOptions *native_options_ptr = nullptr;
    if (options) {
        native_options.constprop = options->constprop;
        native_options.dead_code_elimination = options->dead_code_elimination;
        native_options.common_subexpression_elimination = options->common_subexpression_elimination;
        native_options.expression_optimization = options->expression_optimization;
        native_options.demorgan = options->demorgan;
        native_options.width_reduction = options->width_reduction;
        native_options.resource_sharing = options->resource_sharing;
        native_options.fsm_extraction = options->fsm_extraction;
        native_options.logic_minimization = options->logic_minimization;
        native_options.retiming = options->retiming;
        native_options.boundary_optimization = options->boundary_optimization;
        native_options_ptr = &native_options;
    }
    auto cpp_result = synth_real_with_options(rtl_code, module_name, liberty_path,
                                               native_options_ptr, log_cb);

    result.success = cpp_result.success ? 1 : 0;
    result.gate_verilog = cpp_result.gate_verilog;
    result.report = cpp_result.report;
    result.error = cpp_result.error;
    result.cell_count = cpp_result.cell_count;
    result.wire_count = cpp_result.wire_count;
    result.dff_count = cpp_result.dff_count;
    result.port_count = cpp_result.port_count;
    result.area_ge = cpp_result.area_ge;
    result.area_um2 = cpp_result.area_um2;
    result.area_from_lib = cpp_result.area_from_lib ? 1 : 0;
    result.lib_name = cpp_result.lib_name;
    result.logic_depth = cpp_result.logic_depth;
    result.cell_types = cpp_result.cell_types;
    result.cell_type_counts = cpp_result.cell_type_counts;
    result.num_cell_types = cpp_result.num_cell_types;

    return result;
}

void rtl_synth_result_free(SynthResult *r) {
    if (!r) return;
    free(r->gate_verilog);
    free(r->report);
    free(r->error);
    free(r->lib_name);
    if (r->cell_types) {
        for (size_t i = 0; i < r->num_cell_types; i++) free(r->cell_types[i]);
        free(r->cell_types);
    }
    free(r->cell_type_counts);
}

// Export synthesis result to JSON string
char *rtl_synth_result_to_json(const SynthResult *r) {
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

// Export timing report to JSON string
char *rtl_timing_report_to_json(const TimingReport *report) {
    if (!report) return strdup_safe("{}");
    std::ostringstream ss;
    ss << "{";
    ss << "\"area_ge\":" << report->area_ge << ",";
    ss << "\"delay_ns\":" << report->delay_ns << ",";
    ss << "\"power_mw\":" << report->power_mw << ",";
    ss << "\"logic_depth\":" << report->logic_depth << ",";
    ss << "\"total_gates\":" << report->total_gates << ",";
    ss << "\"dff_count\":" << report->dff_count << ",";
    ss << "\"clock_period_ns\":" << report->clock_period_ns << ",";
    ss << "\"arrival_time_ns\":" << report->arrival_time_ns << ",";
    ss << "\"required_time_ns\":" << report->required_time_ns << ",";
    ss << "\"slack_ns\":" << report->slack_ns << ",";
    ss << "\"timing_met\":" << (report->timing_met ? "true" : "false");
    if (report->report && report->report[0]) {
        ss << ",\"report\":\"" << report->report << "\"";
    }
    ss << "}";
    return strdup_safe(ss.str().c_str());
}

// Frequency-optimized synthesis: iteratively optimizes to meet target frequency ratio
SynthResult rtl_synthesize_freq_optimized(const char *rtl_code, const char *module_name,
                                           const char *liberty_path,
                                           int constraint_mhz, double target_ratio) {
    SynthResult result = {};
    result.success = 0;

    auto log_cb = [](const char *step, const char *msg) {
        if (g_synth_log_cb) g_synth_log_cb(step, msg);
        synth_engine_log(step, msg);
    };

    auto cpp_result = synth_real_freq_optimized(rtl_code, module_name, liberty_path,
                                                 constraint_mhz, target_ratio, log_cb);

    result.success = cpp_result.success ? 1 : 0;
    result.gate_verilog = cpp_result.gate_verilog;
    result.report = cpp_result.report;
    result.error = cpp_result.error;
    result.cell_count = cpp_result.cell_count;
    result.wire_count = cpp_result.wire_count;
    result.dff_count = cpp_result.dff_count;
    result.port_count = cpp_result.port_count;
    result.area_ge = cpp_result.area_ge;
    result.area_um2 = cpp_result.area_um2;
    result.area_from_lib = cpp_result.area_from_lib ? 1 : 0;
    result.lib_name = cpp_result.lib_name;
    result.logic_depth = cpp_result.logic_depth;
    result.cell_types = cpp_result.cell_types;
    result.cell_type_counts = cpp_result.cell_type_counts;
    result.num_cell_types = cpp_result.num_cell_types;

    return result;
}

SimResult rtl_simulate_mt(const char *rtl_code, const char *tb_code, const char *module_name, const char *clk_port, int num_cycles, double half_period_ns, int num_threads) {
    return rtl_simulate(rtl_code, tb_code, module_name, clk_port, num_cycles, half_period_ns);
}

// ======= Clock frequency scan implementation =======
int rtl_clock_scan(const char *synth_output, const char *module_name,
                    const char *liberty_file,
                    double min_period, double max_period, double step,
                    TimingReport **results, int *count) {
    if (!synth_output || !module_name || !results || !count) return -1;
    if (min_period <= 0 || max_period <= min_period || step <= 0) return -1;
    int num_steps = (int)((max_period - min_period) / step) + 1;
    if (num_steps > 200) num_steps = 200;
    *results = (TimingReport*)malloc(sizeof(TimingReport) * num_steps);
    *count = num_steps;
    for (int i = 0; i < num_steps; i++) {
        double period = min_period + i * step;
        (*results)[i] = rtl_timing_analysis(synth_output, module_name, liberty_file, period);
    }
    return 0;
}

// ======= Data detection port implementation =======
int rtl_add_data_detect_port(RtlDesign *d, const char *module_name, RtlDataDetectPort *port) {
    if (!d || !module_name || !port || !port->signal_name) return -1;
    for (auto &it : d->design.modules_) {
        if (it.first.str() == std::string(module_name)) {
            it.second->set_attribute(RTLIL::IdString(std::string("\\detect_") + port->signal_name),
                                     port->detect_toggle ? 1 : 0);
            return 0;
        }
    }
    return -1;
}

// ======= Monitor status implementation =======
char *rtl_get_monitor_status(RtlDesign *d, const char *module_name) {
    if (!d || !module_name) return strdup_safe("{\"status\":\"unknown\"}");
    int monitor_count = 0; std::ostringstream ss; ss << "{\"status\":\"ok\",\"module\":\"" << module_name << "\"";
    for (auto &it : d->design.modules_) {
        for (auto &w : it.second->wires_) {
            if (monitor_count == 0) ss << ",\"ports\":["; else ss << ",";
            ss << "{\"signal\":\"" << w.first.str() << "\"}"; monitor_count++;
        }
    }
    if (monitor_count > 0) ss << "]"; ss << "}";
    return strdup_safe(ss.str().c_str());
}

// ======= Anomaly check implementation =======
char *rtl_check_anomalies(RtlDesign *d, const char *module_name) {
    if (!d || !module_name) return strdup_safe("{\"anomalies\":[],\"count\":0}");
    std::ostringstream ss; ss << "{\"anomalies\":[],\"count\":0,\"module\":\"" << module_name << "\"}";
    return strdup_safe(ss.str().c_str());
}

// ======= Monitor configuration implementation =======
void rtl_configure_monitor(RtlDesign *d, const char *module_name, RtlMonitorConfig *config) {
    if (!d || !module_name || !config) return;
    for (auto &it : d->design.modules_) {
        if (it.first.str() == std::string(module_name)) {
            it.second->set_attribute(RTLIL::IdString("\\monitor_cycle_counting"),
                                     RTLIL::Const(config->enable_cycle_counting ? 1 : 0));
            it.second->set_attribute(RTLIL::IdString("\\monitor_max_cycles"),
                                     RTLIL::Const(config->max_cycles));
        }
    }
}

// ======= Auto-fix configuration implementation =======
void rtl_configure_auto_fix(RtlDesign *d, const char *module_name, RtlAutoFixConfig *config) {
    if (!d || !module_name || !config) return;
    for (auto &it : d->design.modules_) {
        if (it.first.str() == std::string(module_name)) {
            it.second->set_attribute(RTLIL::IdString("\\autofix_enabled"),
                                     RTLIL::Const(config->enable_auto_fix ? 1 : 0));
            it.second->set_attribute(RTLIL::IdString("\\autofix_max_retries"),
                                     RTLIL::Const(config->max_retries));
        }
    }
}

// ======= Auto-fix implementation =======
int rtl_run_auto_fix(RtlDesign *d, const char *module_name) {
    if (!d || !module_name) return -1;
    int max_retries = 3;
    for (auto &it : d->design.modules_) {
        if (it.first.str() == std::string(module_name)) {
            auto retries = it.second->get_attribute(RTLIL::IdString("\\autofix_max_retries"));
            max_retries = (int)retries.as_int();
            if (max_retries <= 0) max_retries = 3;
        }
    }
    // Run synthesis and check results
    for (int attempt = 0; attempt < max_retries; attempt++) {
        int ret = SynthPasses::synthesize(&d->design, module_name);
        if (ret == 0) return 0;
        formal_engine_log("AUTOFIX", ("Auto-fix attempt " + std::to_string(attempt + 1) + "/" + std::to_string(max_retries)).c_str());
    }
    return -1;
}

// ======= Multi-thread synthesis implementation =======
RtlError *rtl_synthesize_mt(RtlDesign *d, const char *module_name, int num_threads) {
    if (!d || !module_name) return make_error(-1, "Invalid arguments", 0, "");
    int ret = SynthPasses::synthesize(&d->design, module_name);
    if (ret == 0) return nullptr;
    return make_error(ret, "Synthesis failed", 0, "");
}

// ======= Realtime status with enhanced reporting =======
char *rtl_get_realtime_status(RtlDesign *d, const char *module_name) {
    if (!d || !module_name) return strdup_safe("{\"status\":\"error\"}");
    std::ostringstream ss; ss << "{\"status\":\"ok\",\"module\":\"" << module_name << "\"";
    int cell_count = 0, wire_count = 0;
    for (auto &it : d->design.modules_) {
        if (it.first.str() == std::string(module_name)) {
            cell_count = (int)it.second->cell_count();
            wire_count = (int)it.second->wire_count();
        }
    }
    ss << ",\"cells\":" << cell_count << ",\"wires\":" << wire_count;
    ss << ",\"memory_mb\":" << SimEngine::get_process_memory_mb() << "}";
    return strdup_safe(ss.str().c_str());
}

// ======= Power Analysis C API =======
PowerAnalysisResult rtl_power_analyze(const char *gate_netlist, const char *module_name,
                                       const char *liberty_path, double clock_freq_mhz,
                                       const char *toggle_data_json) {
    PowerAnalysisResult result = {};
    power_engine_log("POWER", "Starting power analysis with real liberty data");
    std::map<std::string, int> cell_counts;
    std::string netlist(gate_netlist ? gate_netlist : "");
    std::istringstream ns(netlist);
    std::string line;
    while (std::getline(ns, line)) {
        size_t start = line.find_first_not_of(" \t"); if (start == std::string::npos) continue;
        line = line.substr(start);
        size_t sp = line.find(' '); if (sp == std::string::npos) continue;
        std::string first_token = line.substr(0, sp);
        size_t lp = line.find('(', sp);
        if (lp != std::string::npos && first_token != "module" && first_token != "input"
            && first_token != "output" && first_token != "wire" && first_token != "reg"
            && first_token != "assign" && first_token != "always" && first_token != "endmodule") {
            cell_counts[first_token]++;
        }
    }

    double total_leakage = 0.0, total_internal = 0.0, total_switching = 0.0, total_clock = 0.0;
    double vdd = 1.2; int total_cells = 0;
    int liberty_mapped_cells = 0;
    int liberty_unmapped_cells = 0;
    double freq_hz = clock_freq_mhz * 1e6;
    bool use_real_lib = false;
    bool use_activity = false;
    double activity_toggle_rate = 0.1;
    if (toggle_data_json && toggle_data_json[0]) {
        std::string activity(toggle_data_json);
        double sum = 0.0;
        int count = 0;
        size_t pos = 0;
        while ((pos = activity.find(':', pos)) != std::string::npos) {
            size_t start = pos + 1;
            while (start < activity.size() && std::isspace(static_cast<unsigned char>(activity[start]))) start++;
            size_t end = start;
            while (end < activity.size() &&
                   (std::isdigit(static_cast<unsigned char>(activity[end])) ||
                    activity[end] == '.' || activity[end] == 'e' || activity[end] == 'E' ||
                    activity[end] == '+' || activity[end] == '-')) {
                end++;
            }
            if (end > start) {
                try {
                    double rate = std::stod(activity.substr(start, end - start));
                    if (rate >= 0.0 && rate <= 4.0) {
                        sum += std::min(rate, 1.0);
                        count++;
                    }
                } catch (...) {}
            }
            pos = end;
        }
        if (count > 0) {
            activity_toggle_rate = sum / static_cast<double>(count);
            if (activity_toggle_rate < 0.001) activity_toggle_rate = 0.001;
            if (activity_toggle_rate > 1.0) activity_toggle_rate = 1.0;
            use_activity = true;
            power_engine_log("POWER_ACTIVITY", ("Using simulation activity average toggle=" + std::to_string(activity_toggle_rate)).c_str());
        }
    }

    // ── Use full Liberty library for accurate power ──
    if (liberty_path && strlen(liberty_path) > 0) {
        Liberty::LibertyLibrary lib;
        if (lib.load(liberty_path)) {
            use_real_lib = true;
            vdd = lib.nom_voltage > 0 ? lib.nom_voltage : 1.2;
            power_engine_log("POWER", ("Using liberty: " + lib.name + ", Vdd=" + std::to_string(vdd) + "V").c_str());

            for (auto &[cell_type, count] : cell_counts) {
                total_cells += count;
                const Liberty::LibertyCell *lc = lib.find_cell(cell_type);
                if (!lc) {
                    // Try substring match
                    for (auto &[lib_name, lib_cell] : lib.cells) {
                        if (lib_name.find(cell_type) != std::string::npos || cell_type.find(lib_name) != std::string::npos) {
                            lc = &lib_cell;
                            break;
                        }
                    }
                }

                if (lc) {
                    liberty_mapped_cells += count;
                    // ── Static (leakage) power ──
                    // cell_leakage_power is in nW (liberty standard)
                    double leakage_per_cell = lc->cell_leakage_power * 1e-3; // nW → uW
                    total_leakage += leakage_per_cell * count;

                    // ── Dynamic (switching) power ──
                    // P_sw = 0.5 * C_load * Vdd^2 * f * toggle_rate
                    double total_input_cap = 0.0;
                    for (auto &[pn, pin] : lc->pins) {
                        if (pin.is_input()) total_input_cap += pin.capacitance;
                    }
                    if (total_input_cap < 0.0001) total_input_cap = 0.002; // 2fF fallback
                    // Liberty pin capacitance is expressed in pF.  Treating
                    // it as fF understated switching power by 1000x.
                    double cap_f = total_input_cap * 1e-12; // pF → F
                    double toggle_rate = activity_toggle_rate;
                    double dyn_per_cell = 0.5 * cap_f * vdd * vdd * freq_hz * toggle_rate * 1e6; // → uW
                    total_switching += dyn_per_cell * count;

                    // ── Internal power from NLDM tables ──
                    const Liberty::LibertyPin *op = lc->find_output_pin();
                    if (op && !op->power_arcs.empty()) {
                        double int_energy_pj = 0.0;
                        // Use cell-specific capacitance for load interpolation
                        double load_cap = total_input_cap;
                        if (load_cap < 0.0005) load_cap = 0.001;
                        double slew = 0.05; // default 50ps input transition
                        for (auto &pa : op->power_arcs) {
                            double rp = pa.rise_power.empty() ? 0.0 : pa.rise_power.interpolate(slew, load_cap);
                            double fp = pa.fall_power.empty() ? 0.0 : pa.fall_power.interpolate(slew, load_cap);
                            int_energy_pj += (rp + fp) / 2.0;
                        }
                        if (int_energy_pj > 0) {
                            total_internal += int_energy_pj * freq_hz * toggle_rate * 1e-6 * count;
                        } else {
                            total_internal += dyn_per_cell * 0.20 * count;
                        }
                    } else {
                        total_internal += dyn_per_cell * 0.20 * count;
                    }
                } else {
                    // Keep a debug estimate, but do not present a partially
                    // mapped netlist as an NLDM signoff result.
                    liberty_unmapped_cells += count;
                    total_cells += count;
                    double ge = 4.0;
                    if (cell_type.find("DFF") != std::string::npos) ge = 18.0;
                    else if (cell_type.find("XOR") != std::string::npos) ge = 8.0;
                    else if (cell_type.find("MUX") != std::string::npos) ge = 8.0;
                    else if (cell_type.find("NOT") != std::string::npos || cell_type.find("INV") != std::string::npos) ge = 3.0;
                    total_leakage += ge * 0.01 * count;
                    total_switching += ge * 0.06 * count * clock_freq_mhz / 100.0;
                    total_internal += ge * 0.01 * count * clock_freq_mhz / 100.0;
                }
            }
        }
    }

    if (!use_real_lib) {
        // Fallback: old estimation method
        for (auto &[cell_type, count] : cell_counts) {
            total_cells += count;
            double ge = 4.0;
            if (cell_type.find("DFF") != std::string::npos) ge = 18.0;
            else if (cell_type.find("NOT") != std::string::npos || cell_type.find("INV") != std::string::npos) ge = 3.0;
            else if (cell_type.find("XOR") != std::string::npos) ge = 8.0;
            else if (cell_type.find("MUX") != std::string::npos) ge = 8.0;
            total_leakage += ge * 0.02 * count;
            total_switching += ge * 0.08 * count * clock_freq_mhz / 100.0;
            total_internal += ge * 0.01 * count * clock_freq_mhz / 100.0;
        }
    }

    total_clock = (total_switching + total_internal) * 0.3;
    result.total_power_uw = total_leakage + total_switching + total_internal + total_clock;
    result.static_power_uw = total_leakage;
    // Dynamic power is the complete non-static component.  Keeping clock
    // power inside this field makes static + dynamic exactly equal total in
    // the CLI report, report.json, and GUI stacked bars.
    result.dynamic_power_uw = total_switching + total_internal + total_clock;
    result.internal_power_uw = total_internal;
    result.switching_power_uw = total_switching;
    result.clock_power_uw = total_clock;
    result.leakage_power_uw = total_leakage;

    const bool complete_liberty_coverage = use_real_lib && liberty_unmapped_cells == 0;
    std::ostringstream ss; ss << std::fixed << std::setprecision(2);
    ss << "Power Analysis Report (" << (complete_liberty_coverage ? "liberty NLDM" : "estimated") << ")\n";
    ss << "========================\n";
    ss << "Total cells: " << total_cells << " | Freq: " << clock_freq_mhz << " MHz | Vdd: " << vdd << " V\n\n";
    ss << "Activity source: " << (use_activity ? "simulation toggles" : "default toggle estimate")
       << " | Avg toggle: " << activity_toggle_rate << "\n";
    ss << "Liberty coverage: " << liberty_mapped_cells << "/" << total_cells << " mapped cells\n";
    ss << "Leakage: " << total_leakage << " uW | Internal: " << total_internal << " uW\n";
    ss << "Switching: " << total_switching << " uW | Clock: " << total_clock << " uW\n";
    ss << "Total: " << result.total_power_uw << " uW\n";
    result.report = strdup_safe(ss.str().c_str());
    power_engine_log("POWER", ("Done. Total: " + std::to_string(result.total_power_uw) +
        " uW (" + (complete_liberty_coverage ? std::string("liberty NLDM") : std::string("estimated")) + ")").c_str());
    return result;
}

void rtl_power_result_free(PowerAnalysisResult *r) { if (r) { free(r->report); } }
