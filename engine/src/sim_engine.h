/**
 * Professional Behavioral Simulation Engine
 *
 * Architecture:
 * - Event-driven simulation kernel with delta-cycle accuracy
 * - AST-based evaluation (uses parser_full.cpp AST)
 * - Supports: always, initial, assign, if/else, case, for, while
 * - Supports: +, -, *, /, %, &, |, ^, ~, <<, >>, ==, !=, <, >, <=, >=, &&, ||, !
 * - Supports: $display, $finish, $dumpfile, $dumpvars
 * - VCD waveform output
 * - Multi-module instantiation with port mapping
 *
 * References:
 * - industry-standard simulator
 * - Icarus Verilog vvp/
 * - IEEE 1364-2005
 */

#ifndef SIM_ENGINE_H
#define SIM_ENGINE_H

#include "verilog_parser_full.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <functional>
#include <sstream>
#include <cstdint>

namespace SimEngine {

// Forward declarations
class SimKernel;
class ModuleInstance;

/* ========== Signal Value (4-state + strength) ========== */
enum Strength { STR_HIGHZ = 0, STR_SMALL = 1, STR_MEDIUM = 2, STR_WEAK = 3,
                STR_LARGE = 4, STR_PULL = 5, STR_STRONG = 6, STR_SUPPLY = 7 };

struct Value {
    enum State { ZERO = 0, ONE = 1, X = 2, Z = 3 };
    std::vector<State> bits;
    Strength strength;  // Per-value strength (simplified: single strength for all bits)

    Value() : bits(32, ZERO), strength(STR_STRONG) {}
    Value(int width, int64_t val) : bits(width, ZERO), strength(STR_STRONG) {
        for (int i = 0; i < width && i < 64; i++)
            bits[i] = ((val >> i) & 1) ? ONE : ZERO;
    }
    Value(int width, int64_t val, Strength s) : bits(width, ZERO), strength(s) {
        for (int i = 0; i < width && i < 64; i++)
            bits[i] = ((val >> i) & 1) ? ONE : ZERO;
    }
    Value(const std::string &binary) : strength(STR_STRONG) {
        bits.resize(binary.size());
        for (size_t i = 0; i < binary.size(); i++) {
            char c = binary[binary.size() - 1 - i];
            bits[i] = (c == '1') ? ONE : (c == 'x' || c == 'X') ? X : (c == 'z' || c == 'Z') ? Z : ZERO;
        }
    }

    int width() const { return (int)bits.size(); }
    int64_t to_int() const {
        int64_t r = 0;
        for (int i = 0; i < width() && i < 64; i++)
            if (bits[i] == ONE) r |= (1LL << i);
        return r;
    }
    std::string to_string() const {
        std::string r;
        for (int i = width() - 1; i >= 0; i--)
            r += (bits[i] == ONE) ? '1' : (bits[i] == X) ? 'x' : (bits[i] == Z) ? 'z' : '0';
        return r.empty() ? "0" : r;
    }
    std::string to_dec() const { return std::to_string(to_int()); }
    std::string to_hex() const {
        std::string r;
        for (int i = 0; i < width(); i += 4) {
            int nib = 0;
            for (int j = 0; j < 4 && i+j < width(); j++)
                if (bits[i+j] == ONE) nib |= (1 << j);
            r = (nib < 10 ? char('0'+nib) : char('a'+nib-10)) + r;
        }
        return r.empty() ? "0" : r;
    }

    // X/Z detection
    bool is_x() const { for (auto b : bits) if (b == X) return true; return false; }
    bool has_xz() const { for (auto b : bits) if (b == X || b == Z) return true; return false; }
    bool is_z() const { for (auto b : bits) if (b == Z) return true; return false; }

    // Arithmetic: propagate X/Z properly
    // Width rules follow IEEE 1364-2005 section 5.4.1:
    //   Max(L(i1), L(i2)) for context-determined expressions
    // To preserve full precision, the result retains the wider width.
    // Note: multiplication width is special - see operator* below.
    Value operator+(const Value &o) const {
        int w = std::max(width(), o.width()) + 1;  // +1 for carry
        if (has_xz() || o.has_xz()) {
            Value r(w, (int64_t)(to_int() + o.to_int()));
            for (int i = 0; i < w && i < 64; i++) {
                State a = (i < width()) ? bits[i] : ZERO;
                State b = (i < o.width()) ? o.bits[i] : ZERO;
                if (a == X || a == Z || b == X || b == Z) r.bits[i] = X;
            }
            return r;
        }
        return Value(w, (int64_t)(to_int() + o.to_int()));
    }
    Value operator-(const Value &o) const {
        int w = std::max(width(), o.width()) + 1;  // +1 for borrow
        if (has_xz() || o.has_xz()) {
            Value r(w, (int64_t)(to_int() - o.to_int()));
            for (int i = 0; i < w && i < 64; i++) {
                State a = (i < width()) ? bits[i] : ZERO;
                State b = (i < o.width()) ? o.bits[i] : ZERO;
                if (a == X || a == Z || b == X || b == Z) r.bits[i] = X;
            }
            return r;
        }
        return Value(w, (int64_t)(to_int() - o.to_int()));
    }
    Value operator*(const Value &o) const {
        // Full product width = sum of operand widths (preserves all bits)
        int w = width() + o.width();
        if (w < 32) w = 32;  // minimum 32-bit for compatibility
        if (has_xz() || o.has_xz()) {
            Value r(w, (int64_t)(to_int() * o.to_int()));
            for (int i = 0; i < w && i < 64; i++) {
                State a = (i < width()) ? bits[i] : ZERO;
                State b = (i < o.width()) ? o.bits[i] : ZERO;
                if (a == X || a == Z || b == X || b == Z) r.bits[i] = X;
            }
            return r;
        }
        return Value(w, (int64_t)(to_int() * o.to_int()));
    }
    Value operator/(const Value &o) const {
        int w = std::max(width(), o.width());
        if (w < 32) w = 32;
        if (has_xz() || o.has_xz()) return Value(w, 0);
        int64_t d = o.to_int();
        return Value(w, d ? (int64_t)(to_int() / d) : 0);
    }
    Value operator%(const Value &o) const {
        int w = std::max(width(), o.width());
        if (w < 32) w = 32;
        if (has_xz() || o.has_xz()) return Value(w, 0);
        int64_t d = o.to_int();
        return Value(w, d ? (int64_t)(to_int() % d) : 0);
    }

    // Bitwise: IEEE 1364-2005 Table 5-21 Z resolution
    // Z truth table: 0&z=0, 1&z=x, x&z=x, z&z=x
    //                0|z=x, 1|z=1, x|z=x, z|z=x
    //                0^z=x, 1^z=x, x^z=x, z^z=x
    Value bw_and(const Value &o) const {
        int w=std::max(width(),o.width()); Value r(w,0);
        for(int i=0;i<w;i++) {
            State a=(i<width())?bits[i]:ZERO;
            State b=(i<o.width())?o.bits[i]:ZERO;
            if(a==ZERO||b==ZERO) r.bits[i]=ZERO;
            else if(a==ONE&&b==ONE) r.bits[i]=ONE;
            else r.bits[i]=X;  // X or Z with anything non-zero → X
        }
        return r;
    }
    Value bw_or(const Value &o) const {
        int w=std::max(width(),o.width()); Value r(w,0);
        for(int i=0;i<w;i++) {
            State a=(i<width())?bits[i]:ZERO;
            State b=(i<o.width())?o.bits[i]:ZERO;
            if(a==ONE||b==ONE) r.bits[i]=ONE;
            else if(a==ZERO&&b==ZERO) r.bits[i]=ZERO;
            else r.bits[i]=X;  // X or Z with unknown → X
        }
        return r;
    }
    Value bw_xor(const Value &o) const {
        int w=std::max(width(),o.width()); Value r(w,0);
        for(int i=0;i<w;i++) {
            State a=(i<width())?bits[i]:ZERO;
            State b=(i<o.width())?o.bits[i]:ZERO;
            if(a==X||a==Z||b==X||b==Z) r.bits[i]=X;
            else r.bits[i]=(a!=b)?ONE:ZERO;
        }
        return r;
    }
    Value bw_not() const {
        Value r(width(),0);
        for(int i=0;i<width();i++) {
            if(bits[i]==X||bits[i]==Z) r.bits[i]=X;
            else r.bits[i]=(bits[i]==ZERO)?ONE:ZERO;
        }
        return r;
    }

    // Reduction: properly handle X/Z by returning State instead of bool
    // IEEE: reduction of X produces X, which in decision contexts is treated as false
    // but we preserve the distinction for propagation
    Value red_and_val() const {
        bool has_unknown = false;
        for(auto b:bits) { if(b==X||b==Z) has_unknown=true; else if(b!=ONE) return Value(1,0); }
        if(has_unknown) { Value r(1,0); r.bits[0]=X; return r; }
        return bits.empty() ? Value(1,0) : Value(1,1);
    }
    Value red_or_val() const {
        bool has_unknown = false;
        for(auto b:bits) { if(b==X||b==Z) has_unknown=true; else if(b==ONE) return Value(1,1); }
        if(has_unknown) { Value r(1,0); r.bits[0]=X; return r; }
        return Value(1,0);
    }
    Value red_xor_val() const {
        for(auto b:bits) if(b==X||b==Z) { Value r(1,0); r.bits[0]=X; return r; }
        int c=0; for(auto b:bits) if(b==ONE) c++;
        return Value(1, c%2==1 ? 1 : 0);
    }
    // Legacy bool versions for backward compat (X/Z → false/true)
    bool red_and() const { return red_and_val().to_int() != 0; }
    bool red_or() const { return red_or_val().to_int() != 0; }
    bool red_xor() const { return red_xor_val().to_int() != 0; }

    // Comparison
    bool eq(const Value &o) const { if(has_xz()||o.has_xz()) return false; return to_int() == o.to_int(); }
    bool ne(const Value &o) const { if(has_xz()||o.has_xz()) return true; return to_int() != o.to_int(); }
    bool lt(const Value &o) const { if(has_xz()||o.has_xz()) return false; return to_int() < o.to_int(); }
    bool gt(const Value &o) const { if(has_xz()||o.has_xz()) return false; return to_int() > o.to_int(); }
    bool le(const Value &o) const { if(has_xz()||o.has_xz()) return false; return to_int() <= o.to_int(); }
    bool ge(const Value &o) const { if(has_xz()||o.has_xz()) return false; return to_int() >= o.to_int(); }

    // Shift
    Value shl(int s) const { if(has_xz()) { Value r(width(),0); for(int i=0;i<width();i++) r.bits[i]=X; return r; } return Value(width(), (int64_t)(to_int() << s)); }
    Value shr(int s) const { if(has_xz()) { Value r(width(),0); for(int i=0;i<width();i++) r.bits[i]=X; return r; } return Value(width(), (int64_t)(to_int() >> s)); }

    // Resize
    Value resize(int w) const {
        Value r(w, 0);
        for (int i = 0; i < std::min(width(), w); i++) r.bits[i] = bits[i];
        return r;
    }

    bool is_true() const { if(has_xz()) return false; return to_int() != 0; }
    void set_bit(int pos, State s) { if (pos >= 0 && pos < width()) bits[pos] = s; }
    State get_bit(int pos) const { return (pos >= 0 && pos < width()) ? bits[pos] : ZERO; }
};

/* ========== Event ========== */
struct Event {
    enum Type { ASSIGN, POSEDGE, NEGEDGE, DISPLAY, FINISH, DELAY };
    Type type;
    int time;           // absolute time in ns
    std::string target; // signal name
    Value value;
    std::string message;
    Event(Type t, int tm) : type(t), time(tm) {}
    bool operator>(const Event &o) const { return time > o.time; }
};

/* ========== Signal ========== */
struct Signal {
    std::string name;
    int width;
    Value current;
    Value next;
    bool is_port;
    enum Dir { NONE, INPUT, OUTPUT, INOUT } dir;
    bool is_reg;
    bool force_active;     // force/release tracking
    Value force_value;     // forced value when force is active
    enum NetType { WIRE, WAND, WOR, TRI, TRIAND, TRIOR, TRI0, TRI1, SUPPLY0, SUPPLY1 } net_type;
    bool assign_active;    // procedural continuous assign active
    Value assign_value;    // procedurally assigned value
    Signal() : width(1), is_port(false), dir(NONE), is_reg(false), force_active(false),
               net_type(WIRE), assign_active(false) {}
};

/* ========== Memory Array ========== */
struct MemoryArray {
    std::string name;
    int word_width;        // bits per word
    int num_words;         // number of words
    int addr_bits;         // address bits (log2(num_words))
    std::vector<Value> data;  // stored data
    MemoryArray() : word_width(8), num_words(256), addr_bits(8) {}
    MemoryArray(const std::string &n, int w, int count)
        : name(n), word_width(w), num_words(count), data(count) {
        // compute address bits
        int bits = 0;
        int c = count - 1;
        while (c > 0) { bits++; c >>= 1; }
        if (bits < 1) bits = 1;
        addr_bits = bits;
        // initialize all entries to X
        for (int i = 0; i < count; i++) {
            data[i] = Value(w, 0);
            for (int j = 0; j < w; j++) data[i].bits[j] = Value::X;
        }
    }
};

/* ========== Module Instance ========== */
class ModuleInstance {
public:
    std::string name;
    std::string module_name;
    ModuleInstance *parent;
    std::map<std::string, Signal> signals;
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> always_blocks;
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> initial_blocks;
    std::vector<std::shared_ptr<VerilogParser::ASTNode>> assign_stmts;
    std::vector<std::shared_ptr<VerilogParser::ASTNode>> assertions; // immediate/concurrent assertions
    std::vector<std::pair<std::string, std::shared_ptr<VerilogParser::ASTNode>>> instances; // child name -> instantiation AST
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> parameters;
    std::map<std::string, std::string> port_map; // child port -> parent signal
    std::vector<std::string> display_outputs;
    std::map<std::string, MemoryArray> memories;  // memory arrays

    // $monitor tracking: list of signals to monitor
    struct MonitorEntry {
        std::vector<std::string> signal_names;
        std::string format;
        std::map<std::string, Value> prev_values;  // for change detection
    };
    std::vector<MonitorEntry> monitor_list;
    bool monitor_triggered;  // re-trigger monitor after any signal change

    // Named events (event type in Verilog)
    std::map<std::string, bool> named_events;  // event_name → triggered flag

    // Specify block path delays: "from_signal->to_signal" → delay_in_ns
    std::map<std::string, double> specify_delays;

    ModuleInstance(const std::string &n, const std::string &mod)
        : name(n), module_name(mod), parent(nullptr), monitor_triggered(true) {}

    Signal *find_signal(const std::string &name);
    void set_signal(const std::string &name, const Value &val);
    Value get_signal(const std::string &name);
};

/* ========== Simulation Kernel ========== */
class SimKernel {
public:
    SimKernel();
    ~SimKernel();

    // Load design from parsed AST
    bool load(VerilogParser::ParseResult &parse_result);
    bool load_module(const std::string &code);

    // Configuration
    void set_clock(const std::string &port, int period_ns);
    void set_reset(const std::string &port, bool active_low, int duration_ns);
    void set_max_cycles(int n) { max_cycles_ = n; }
    void set_timescale(int ns) { timescale_ns_ = ns; }
    void set_memory_limit(size_t mb) { memory_limit_mb_ = mb; }
    void set_timeout_seconds(int s) { timeout_seconds_ = s; }

    // Run simulation
    bool run();
    bool run_cycles(int n);

    // Results
    bool passed() const { return passed_; }
    const std::string &output() const { return output_; }
    const std::string &vcd() const { return vcd_; }
    int current_time() const { return current_time_; }
    int cycle_count() const { return cycle_count_; }

    // Access
    ModuleInstance *top() { return top_.get(); }
    const std::map<std::string, int64_t> &toggle_counts() const { return toggle_counts_; }
    double toggle_rate(const std::string &signal) const {
        auto it = toggle_counts_.find(signal);
        if (it == toggle_counts_.end() || cycle_count_ == 0) return 0.0;
        return (double)it->second / (double)cycle_count_;
    }

private:
    std::unique_ptr<ModuleInstance> top_;
    std::vector<std::unique_ptr<ModuleInstance>> child_instances_; // child module instances
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> module_defs; // module_name -> AST
    int current_time_;
    int max_cycles_;
    int timescale_ns_;
    int time_precision_;     // $timeformat precision
    std::string time_unit_str_; // $timeformat unit suffix (ns/ps/us/ms)
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> event_queue_;
    std::string clock_port_;
    int clock_period_;
    std::string reset_port_;
    bool reset_active_low_;
    int reset_duration_;
    bool passed_;
    std::string output_;
    std::string vcd_;
    std::string vcd_filename_;     // $dumpfile target
    bool vcd_dump_enabled_;        // $dumpvars / $dumpon
    int vcd_level_;                // $dumpvars level (0=all)
    int vcd_size_limit_;           // $dumplimit
    bool vcd_dump_all_;            // $dumpall
    std::set<std::string> vcd_signals_; // $dumpvars signal list
    std::string fsdb_;
    int cycle_count_;
    bool finish_requested_;
    bool stop_requested_;    // $stop pause flag (resumable)
    std::vector<std::string> display_lines_;
    size_t memory_limit_mb_;  // 0 = no limit
    int timeout_seconds_;     // 0 = no timeout
    int timing_check_count_;  // count of $setup/$hold checks

    // Plusargs: passed via +arg or +arg=value during simulation
    std::map<std::string, std::string> plusargs_;

    // NBA (Non-Blocking Assignment) queue
    std::queue<Event> nba_queue_;
    // Strobe queue for deferred $strobe execution
    std::vector<std::shared_ptr<VerilogParser::ASTNode>> strobe_queue_;

    // IEEE 1364 5-region event queues
    std::queue<Event> active_queue_;    // continuous assigns, blocking assigns, $display
    std::queue<Event> inactive_queue_;  // #0 delay events
    // nba_queue_ is the NBA region queue
    std::queue<Event> monitor_queue_;   // $monitor events
    // strobe_queue_ is the Postponed region queue

    // Global timing scheduler: delay threads sorted by wake-up time
    struct DelayThread {
        int wake_time;           // absolute time to wake
        std::string thread_name; // init/always/fork thread identifier
        std::shared_ptr<VerilogParser::ASTNode> stmt;
        size_t stmt_idx;
        int repeat_remaining;
    };
    std::vector<DelayThread> delay_threads_;

    // Task/Function call stack
    struct CallFrame {
        std::string func_name;
        std::map<std::string, Value> locals;
        std::map<std::string, Value> params;
        size_t return_pc;       // instruction pointer to resume after call
        std::shared_ptr<VerilogParser::ASTNode> func_body;
        Value return_value;
    };
    std::vector<CallFrame> call_stack_;

    // User-defined task/function definitions: func_name → AST node
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> task_defs_;
    std::map<std::string, std::shared_ptr<VerilogParser::ASTNode>> func_defs_;

    // Process/Thread for fork/join
    struct Process {
        std::shared_ptr<VerilogParser::ASTNode> stmt;
        size_t stmt_idx;
        int delay_remaining;
        bool done;
        enum Type { JOIN, JOIN_ANY, JOIN_NONE } join_type;
        Process() : stmt_idx(0), delay_remaining(0), done(false), join_type(JOIN) {}
    };
    std::vector<Process> fork_processes_;

    public:
    // Coverage tracking data
    struct CoverageData {
        int total_toggles;
        int covered_toggles;
        int total_branches;      // if/else decision points
        int covered_branches;    // taken branches
        int total_expressions;   // sub-expression truth table entries
        int covered_expressions; // hit entries
        int total_conditions;    // condition coverage
        int covered_conditions;  // hit conditions
        int total_fsm_states;    // FSM state coverage
        int covered_fsm_states;  // hit states
        int total_assertions;    // assertion coverage
        int covered_assertions;  // triggered assertions
        std::map<std::string, std::set<int>> fsm_states; // fsm_name → {states visited}
        CoverageData() : total_toggles(0), covered_toggles(0),
            total_branches(0), covered_branches(0),
            total_expressions(0), covered_expressions(0),
            total_conditions(0), covered_conditions(0),
            total_fsm_states(0), covered_fsm_states(0),
            total_assertions(0), covered_assertions(0) {}
    };
    const CoverageData &coverage() const { return coverage_; }

private:
    CoverageData coverage_;

    // VCD signal change tracking: time -> [(signal_name, value_string)]
    std::map<int, std::vector<std::pair<std::string, std::string>>> vcd_changes_;
    std::map<std::string, Value> vcd_initial_snapshot_;

    // Toggle count tracking for SAIF/power analysis
    std::map<std::string, int64_t> toggle_counts_;

    // Build module hierarchy
    void build_hierarchy(ModuleInstance *inst, const std::string &module_name);
    void elaborate();

    // Simulation
    void schedule_event(const Event &e);
    void process_events();
    void generate_clock();
    void apply_reset();

    // AST evaluation
    Value eval_expr(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void exec_stmt(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node, int &delay_acc);
    int exec_stmt_with_delay(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void exec_always(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void exec_initial(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void exec_assign(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void eval_continuous_assigns();

    // $display / $monitor / $strobe / $finish
    void handle_display(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void handle_monitor(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    std::string format_strobe_output(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    void handle_readmem(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node, bool is_hex);
    void handle_writemem(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node, bool is_hex);
    void handle_finish();

    // Task/Function call support
    int handle_user_task(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node);
    bool handle_user_function(ModuleInstance *ctx, std::shared_ptr<VerilogParser::ASTNode> node, Value &result);

    // VCD / FSDB / SAIF
    void generate_vcd();
    void generate_fsdb();
    std::string generate_saif();

    // Helpers
    std::string get_node_attr(std::shared_ptr<VerilogParser::ASTNode> node, const std::string &key);
    std::string get_identifier(std::shared_ptr<VerilogParser::ASTNode> node);
    std::string get_connection_name(std::shared_ptr<VerilogParser::ASTNode> node);
    int64_t get_number(std::shared_ptr<VerilogParser::ASTNode> node);
    bool read_connection(ModuleInstance *parent, const std::string &connection,
                         int width, Value &value);
    bool write_connection(ModuleInstance *parent, const std::string &connection,
                          const Value &value);
    Signal *find_signal_hier(const std::string &path);
};

/* ========== Logging Callback ========== */
typedef void (*SimLogCallback)(const char *category, const char *message);

/* ========== Main API ========== */
struct SimResult {
    bool passed;
    int exit_code;
    int time_steps;
    std::string output;
    std::string vcd_file;
    std::map<std::string, int64_t> toggle_counts;  // signal name → toggle count
    // Coverage data
    int total_toggles = 0;
    int covered_toggles = 0;
    int total_branches = 0;
    int covered_branches = 0;
    int total_expressions = 0;
    int covered_expressions = 0;
    int total_conditions = 0;
    int covered_conditions = 0;
    int total_fsm_states = 0;
    int covered_fsm_states = 0;
    int total_assertions = 0;
    int covered_assertions = 0;
};

SimResult simulate_code(const std::string &rtl_code, const std::string &tb_code,
                        const std::string &module_name, int max_cycles = 1000,
                        size_t memory_limit_mb = 0, int timeout_seconds = 0);

// Toggle data export (C API for Rust FFI bridge to power analysis)
#ifdef __cplusplus
extern "C" {
#endif
char *rtl_get_toggle_counts_json(const char *rtl_code, const char *tb_code,
                                  const char *module_name);
char *rtl_export_saif(const char *rtl_code, const char *tb_code,
                       const char *module_name);
char *rtl_get_sim_coverage_json(const char *rtl_code, const char *tb_code,
                                 const char *module_name);
void rtl_set_synth_log_callback(SimLogCallback cb);
void rtl_set_timing_log_callback(SimLogCallback cb);
void rtl_set_power_log_callback(SimLogCallback cb);
void rtl_set_formal_log_callback(SimLogCallback cb);
#ifdef __cplusplus
}
#endif

// Set log callback for detailed simulation logging
void set_log_callback(SimLogCallback cb);

// Get current process memory usage in MB
size_t get_process_memory_mb();

// Get system CPU info (cores count, model name)
struct SystemInfo {
    int cpu_cores;
    int cpu_threads;
    size_t total_ram_mb;
    size_t available_ram_mb;
    size_t process_rss_mb;
    double cpu_usage_pct;     // current process CPU usage
    double load_1min;
    char cpu_model[256];
};
SystemInfo get_system_info();

} // namespace SimEngine

#endif // SIM_ENGINE_H
