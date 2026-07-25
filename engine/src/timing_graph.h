/**
 * Timing Graph - Static Timing Analysis engine
 *
 * References OpenSTA source code design patterns:
 * - search/Sta.hh: Main STA API
 * - dcalc/DelayCalcBase.hh: Delay calculation
 * - liberty/Liberty.cc: Liberty cell timing model
 * - search/CheckTiming.hh: Timing checks (setup/hold)
 *
 * Implements:
 * - Timing graph construction from gate-level netlist
 * - Arrival time calculation (topological sort)
 * - Required time calculation (from clock constraints)
 * - Setup/Hold slack calculation
 * - Critical path reporting
 */

#ifndef TIMING_GRAPH_H
#define TIMING_GRAPH_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

// Liberty cell timing data
struct LibertyCellTiming {
    std::string name;
    double area;
    // Pin timing: pin_name -> {capacitance, is_clock, is_input, is_output}
    struct PinInfo {
        double capacitance;
        bool is_clock;
        bool is_input;
        bool is_output;
    };
    std::map<std::string, PinInfo> pins;

    // Delay tables: input_pin -> output_pin -> delay (ns)
    // Simplified: single delay value per pin pair
    std::map<std::string, std::map<std::string, double>> cell_delay;

    // Setup/Hold times for sequential cells
    double setup_time;
    double hold_time;
    double clk_to_q;  // Clock-to-output delay

    LibertyCellTiming() : area(0), setup_time(0), hold_time(0), clk_to_q(0) {}
};

// Timing node (cell instance or port)
struct TimingNode {
    int id;
    std::string name;
    std::string cell_type;    // Liberty cell type (empty for ports)
    double arrival_time;      // Calculated arrival time
    double required_time;     // Calculated required time
    double slack;             // required - arrival
    bool is_clock_source;     // Is this a clock source?
    bool is_input_port;       // Is this an input port?
    bool is_output_port;      // Is this an output port?
    bool is_register;         // Is this a register (DFF)?

    // Connected pins
    std::vector<int> fanin;   // Input node IDs
    std::vector<int> fanout;  // Output node IDs

    // Pin names for this node
    std::string input_pin;    // Which input pin of the cell
    std::string output_pin;   // Which output pin of the cell

    TimingNode() : id(-1), arrival_time(0), required_time(0), slack(0),
                   is_clock_source(false), is_input_port(false), is_output_port(false),
                   is_register(false) {}
};

// Timing edge (connection between nodes)
struct TimingEdge {
    int from_node;
    int to_node;
    std::string from_pin;
    std::string to_pin;
    double delay;             // Edge delay (ns)
    double capacitance;       // Load capacitance (pF)

    TimingEdge() : from_node(-1), to_node(-1), delay(0), capacitance(0) {}
};

// Wire load model (referenced from liberty wire capacitance model)
struct WireLoadModel {
    double wire_cap_per_unit;    // Wire capacitance per unit length (pF/um)
    double wire_res_per_unit;    // Wire resistance per unit length (kOhm/um)
    double default_fanout;       // Default fanout for estimation
    double wire_delay_base;      // Base wire delay (ns)

    // Estimate wire delay based on fanout
    double estimate_delay(int fanout) const {
        // Linear model: delay = base + fanout * factor
        return wire_delay_base + fanout * 0.005; // 5ps per fanout
    }

    // Estimate wire capacitance based on fanout
    double estimate_capacitance(int fanout) const {
        return default_fanout * fanout * wire_cap_per_unit;
    }

    WireLoadModel() : wire_cap_per_unit(0.001), wire_res_per_unit(0.1),
                      default_fanout(1.0), wire_delay_base(0.01) {}
};

// Clock definition
struct ClockDef {
    std::string name;
    double period;            // Clock period (ns)
    double source_latency;    // Source latency (ns)
    double network_latency;   // Clock network latency (ns)
    double uncertainty;       // Clock uncertainty (ns)
    double rise_edge;         // Rise edge time (ns)
    double fall_edge;         // Fall edge time (ns)

    ClockDef() : period(10.0), source_latency(0), network_latency(0),
                 uncertainty(0.1), rise_edge(0), fall_edge(5.0) {}
};

// Timing analysis result
struct TimingResult {
    // Overall metrics
    double wns;               // Worst negative slack
    double tns;               // Total negative slack
    double wns_hold;
    double tns_hold;
    int setup_violations;
    int hold_violations;
    int total_paths;

    // Clock info
    double clock_period;
    double clock_frequency_mhz;

    // Critical path
    std::vector<int> critical_path;
    double critical_path_delay;

    // Per-node timing
    std::vector<TimingNode> nodes;

    // Summary
    int total_cells;
    int total_nets;
    int total_dff;
    int logic_depth;

    TimingResult() : wns(0), tns(0), wns_hold(0), tns_hold(0),
                     setup_violations(0), hold_violations(0), total_paths(0),
                     clock_period(10), clock_frequency_mhz(100),
                     critical_path_delay(0), total_cells(0), total_nets(0),
                     total_dff(0), logic_depth(0) {}
};

// Liberty library parser
class LibertyParser {
public:
    static std::map<std::string, LibertyCellTiming> parse(const std::string &filename);
    static std::map<std::string, LibertyCellTiming> parse_string(const std::string &content);
};

// Timing graph builder and analyzer
class TimingAnalyzer {
public:
    TimingAnalyzer();
    ~TimingAnalyzer();

    // Load liberty library
    void load_library(const std::string &filename);

    // Build timing graph from synthesis stat output
    void build_graph(const std::string &synth_output, const std::string &module_name);

    // Set clock definition
    void set_clock(const ClockDef &clk);

    // Run timing analysis (references OpenSTA's approach)
    TimingResult analyze();

    // Get critical path
    std::vector<std::string> get_critical_path();

private:
    std::map<std::string, LibertyCellTiming> library_;
    ClockDef clock_;
    std::vector<TimingNode> nodes_;
    std::vector<TimingEdge> edges_;
    std::map<std::string, int> node_map_;  // name -> node id
    int total_cells_;
    int total_dff_;
    std::vector<int> critical_path_;
    double critical_path_delay_;
    WireLoadModel wire_load_;

    // Build graph from cell instances
    void add_cell_instance(const std::string &name, const std::string &type,
                           const std::map<std::string, std::string> &connections);

    // Add port
    void add_port(const std::string &name, bool is_input, bool is_output);

    // Topological sort for arrival time calculation
    void calc_arrival_times();

    // Calculate required times from clock constraints
    void calc_required_times();

    // Calculate slack
    void calc_slack();

    // Find critical path
    void find_critical_path();

    // Get cell delay from library
    double get_cell_delay(const std::string &cell_type, const std::string &from_pin,
                          const std::string &to_pin);

    // Get setup/hold time from library
    double get_setup_time(const std::string &cell_type);
    double get_hold_time(const std::string &cell_type);
    double get_clk_to_q(const std::string &cell_type);
};

#endif /* TIMING_GRAPH_H */
