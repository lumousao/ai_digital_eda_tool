#include "synthesis.h"
/**
 * Native Static Timing Analysis Engine
 *
 * References:
 * - OpenSTA (complete timing analysis)
 * - PrimeTime (industry standard)
 * - Synopsys Design Constraints (SDC)
 * - Cadence Tempus (timing signoff)
 *
 * Features:
 * - Complete path analysis
 * - Clock tree analysis
 * - Multi-corner analysis
 * - On-chip variation (OCV)
 * - Setup/Hold analysis
 * - Clock gating analysis
 * - Power-aware timing
 */

#ifndef TIMING_ANALYZER_H
#define TIMING_ANALYZER_H

#include "synthesis.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace TimingAnalysis {

/* ========== Timing Constraints ========== */
struct ClockConstraint {
    std::string name;
    std::string port;
    double period;
    double waveform_rise;
    double waveform_fall;
    double duty_cycle;
    double source_latency;   // clock source latency (ns)
    double network_latency;  // clock network latency (ns)

    ClockConstraint() : period(10.0), waveform_rise(0.0), waveform_fall(5.0),
                        duty_cycle(0.5), source_latency(0.0), network_latency(0.0) {}
};

struct InputDelayConstraint {
    std::string port;
    std::string clock;
    double delay;
    bool is_rising;
    bool is_falling;

    InputDelayConstraint() : delay(0.0), is_rising(true), is_falling(true) {}
};

struct OutputDelayConstraint {
    std::string port;
    std::string clock;
    double delay;
    bool is_rising;
    bool is_falling;

    OutputDelayConstraint() : delay(0.0), is_rising(true), is_falling(true) {}
};

struct FalsePathConstraint {
    std::string from;
    std::string to;
    std::string through;
    std::string comment;

    FalsePathConstraint() = default;
};

struct MulticyclePathConstraint {
    std::string from;
    std::string to;
    int setup_cycles;
    int hold_cycles;

    MulticyclePathConstraint() : setup_cycles(1), hold_cycles(0) {}
};

struct MaxDelayConstraint {
    std::string from;
    std::string to;
    double delay;

    MaxDelayConstraint() : delay(0.0) {}
};

struct MinDelayConstraint {
    std::string from;
    std::string to;
    double delay;

    MinDelayConstraint() : delay(0.0) {}
};

/* ========== Timing Graph ========== */
struct TimingNode {
    std::string name;
    enum Type { INPUT, OUTPUT, INTERNAL, CLOCK };
    Type type;
    double arrival_time;
    double required_time;
    double slack;
    bool is_clock;
    bool is_constrained;
    double transition_time;

    TimingNode() : type(INTERNAL), arrival_time(0.0), required_time(0.0),
                   slack(0.0), is_clock(false), is_constrained(false),
                   transition_time(0.0) {}
};

struct TimingEdge {
    int from_node;
    int to_node;
    double delay;
    double transition;
    double load_cap;              // output load capacitance (pF) for NLDM lookup
    double input_slew;            // input transition time (ns) for NLDM lookup
    enum Type { COMBINATIONAL, SEQUENTIAL, CLOCK };
    Type type;
    bool is_false_path;
    bool is_multicycle;
    int multicycle_cycles;

    TimingEdge() : from_node(-1), to_node(-1), delay(0.0), transition(0.0),
                   load_cap(0.005), input_slew(0.05),
                   type(COMBINATIONAL), is_false_path(false),
                   is_multicycle(false), multicycle_cycles(1) {}
};

/// Per-stage delay detail for detailed path reporting
struct PathStage {
    std::string cell_name;
    std::string cell_type;
    double incr_delay;
    double cumul_delay;

    PathStage() : incr_delay(0.0), cumul_delay(0.0) {}
};

struct TimingPath {
    std::vector<int> nodes;
    std::vector<int> edges;
    std::vector<PathStage> stages;  // Detailed per-stage delay breakdown
    double total_delay;
    double slack;
    bool is_met;
    bool is_clock_path;
    bool is_false_path;
    bool is_multicycle;
    int startpoint;
    int endpoint;

    TimingPath() : total_delay(0.0), slack(0.0), is_met(true),
                   is_clock_path(false), is_false_path(false),
                   is_multicycle(false), startpoint(-1), endpoint(-1) {}
};

/// Setup/hold constraint for sequential cells
struct DFFSetupHoldConstraint {
    std::string cell_type;
    double setup_time;   // ns — data must arrive this long before clock
    double hold_time;    // ns — data must stay stable this long after clock
    double clk_to_q;     // ns — clock-to-output delay

    DFFSetupHoldConstraint() : setup_time(0.1), hold_time(0.05), clk_to_q(0.15) {}
};

/// Setup/hold violation record
struct SetupHoldViolation {
    std::string path_name;
    std::string type;      // "setup" or "hold"
    double required_value; // required setup/hold time
    double actual_value;   // actual arrival
    double violation;      // amount of violation (>0 means violation)
    int start_node;
    int end_node;

    SetupHoldViolation() : required_value(0), actual_value(0), violation(0), start_node(-1), end_node(-1) {}
};

/* ========== Timing Report ========== */
struct TimingReport {
    std::string module_name;
    double clock_period;
    double clock_frequency;
    int setup_violations;
    int hold_violations;
    int total_paths;
    double max_path_delay;
    double min_path_delay;
    std::vector<TimingPath> paths;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<SetupHoldViolation> setup_hold_violations;
    std::vector<SetupHoldViolation> recovery_removal_violations;
    int cdc_path_count;

    TimingReport() : clock_period(10.0), clock_frequency(100.0),
                     setup_violations(0), hold_violations(0),
                     total_paths(0), max_path_delay(0.0),
                     min_path_delay(0.0), cdc_path_count(0) {}

    std::string toString() const;
};

/* ========== SDC Parser ========== */
class SDCParser {
public:
    SDCParser();
    ~SDCParser();

    bool parse(const std::string &filename);

    const std::vector<ClockConstraint> &getClocks() const { return clocks_; }
    const std::vector<InputDelayConstraint> &getInputDelays() const { return inputDelays_; }
    const std::vector<OutputDelayConstraint> &getOutputDelays() const { return outputDelays_; }
    const std::vector<FalsePathConstraint> &getFalsePaths() const { return falsePaths_; }
    const std::vector<MulticyclePathConstraint> &getMulticyclePaths() const { return multicyclePaths_; }

private:
    std::vector<ClockConstraint> clocks_;
    std::vector<InputDelayConstraint> inputDelays_;
    std::vector<OutputDelayConstraint> outputDelays_;
    std::vector<FalsePathConstraint> falsePaths_;
    std::vector<MulticyclePathConstraint> multicyclePaths_;
    std::vector<MaxDelayConstraint> maxDelays_;
    std::vector<MinDelayConstraint> minDelays_;

    bool parseCommand(const std::string &cmd);
    bool parseCreateClock(const std::string &args);
    bool parseCreateGeneratedClock(const std::string &args);
    bool parseSetClockUncertainty(const std::string &args);
    bool parseSetClockLatency(const std::string &args);
    bool parseSetInputDelay(const std::string &args);
    bool parseSetOutputDelay(const std::string &args);
    bool parseSetFalsePath(const std::string &args);
    bool parseSetMulticyclePath(const std::string &args);

    std::vector<std::string> tokenize(const std::string &str);
    double parseTime(const std::string &str);
};

/* ========== Timing Analyzer ========== */
class TimingAnalyzer {
public:
    TimingAnalyzer();
    ~TimingAnalyzer();

    // Set design
    void setDesign(const ::Synthesis::RTLIL::Design &design);
    void setModule(const std::string &moduleName);

    // Load constraints
    void loadSDC(const std::string &filename);
    void loadLiberty(const std::string &filename);
    void addClock(const ClockConstraint &clock);
    void addInputDelay(const InputDelayConstraint &delay);
    void addOutputDelay(const OutputDelayConstraint &delay);
    void addFalsePath(const FalsePathConstraint &path);
    void addMulticyclePath(const MulticyclePathConstraint &path);

    // Run analysis
    bool analyze();
    bool analyzeSetup();
    bool analyzeHold();

    // Get results
    const TimingReport &getReport() const { return report_; }

    // Get timing graph
    const std::vector<TimingNode> &getNodes() const { return nodes_; }
    const std::vector<TimingEdge> &getEdges() const { return edges_; }
    const std::vector<TimingPath> &getPaths() const { return paths_; }

    // Get critical path
    TimingPath getCriticalPath() const;
    TimingPath getSetupPath() const;
    TimingPath getHoldPath() const;

    // Get slack
    double getSlack(const std::string &from, const std::string &to) const;
    double getWorstSlack() const;

    // Configuration
    void setEarlyMode(bool early) { earlyMode_ = early; }
    void setLateMode(bool late) { lateMode_ = late; }

    // OCV (On-Chip Variation) configuration
    void setOCVMode(bool enable) { ocv_enabled_ = enable; }
    void setOCVDerateEarly(double derate) { ocv_derate_early_ = derate; }
    void setOCVDerateLate(double derate) { ocv_derate_late_ = derate; }
    bool isOCVEnabled() const { return ocv_enabled_; }
    double getOCVDerateEarly() const { return ocv_derate_early_; }
    double getOCVDerateLate() const { return ocv_derate_late_; }

    // Build timing graph from gate-level netlist text (public API)
    void buildTimingGraphFromNetlist(const std::string &netlist);

    // Load full Liberty library for NLDM lookup
    void loadLibertyFull(const void *lib);
    bool hasLiberty() const { return liberty_full_ != nullptr; }

    // Get cell names mapped via liberty for display
    std::map<std::string, int> getLibertyCellsForDisplay() const {
        std::map<std::string, int> result;
        for (auto &cell_name : liberty_cell_names_) result[cell_name]++;
        return result;
    }

    // Core timing computation methods (public for external use)
    void computeArrivalTimes();
    void computeRequiredTimes();
    void computeSlack();
    void findPaths();

    // Setup/hold constraint checking
    void setSetupHoldConstraint(const std::string &cell_type, double setup, double hold, double clk2q);
    void checkSetupHoldConstraints();

    // Recovery/Removal checking
    void checkRecoveryRemoval();

    // CDC analysis
    void analyzeCDC();
    const std::vector<TimingPath> &getCdcPaths() const { return cdc_paths_; }

    // Clock gating check
    void checkClockGating();

    // CRPR and clock skew
    double computeCRPR(int launch_node, int capture_node);
    double analyzeClockSkew();

    // What-if analysis
    struct WhatIfResult {
        double frequency_mhz;
        double worst_slack;
        int setup_violations;
        bool timing_met;
    };
    std::vector<WhatIfResult> sweepClockFrequency(double start_mhz, double end_mhz, double step_mhz);
    double findMaxFrequency();

private:
    ::Synthesis::RTLIL::Design design_;
    std::string moduleName_;

    // Constraints
    std::vector<ClockConstraint> clocks_;
    std::vector<InputDelayConstraint> inputDelays_;
    std::vector<OutputDelayConstraint> outputDelays_;
    std::vector<FalsePathConstraint> falsePaths_;
    std::vector<MulticyclePathConstraint> multicyclePaths_;

    // Timing graph
    std::vector<TimingNode> nodes_;
    std::vector<TimingEdge> edges_;
    std::vector<TimingPath> paths_;

    // Report
    TimingReport report_;

    // Configuration
    bool earlyMode_;
    bool lateMode_;

    // OCV configuration
    bool ocv_enabled_;
    double ocv_derate_early_;   // e.g., 0.9 (fast paths get 10% faster)
    double ocv_derate_late_;    // e.g., 1.1 (slow paths get 10% slower)

    // DFF setup/hold constraints (cell_type → constraint)
    std::map<std::string, DFFSetupHoldConstraint> setup_hold_constraints_;

    // CDC paths
    std::vector<TimingPath> cdc_paths_;

    // Liberty NLDM library data for cell delay lookup (public for external helper)
public:
    struct LibertyCellTiming {
        std::string cell_name;
        double cell_rise;         // intrinsic delay rise (ns)
        double cell_fall;         // intrinsic delay fall (ns)
        double rise_transition;   // output transition rise (ns)
        double fall_transition;   // output transition fall (ns)
        double area;
        std::string vt_class;     // LVT, HVT, SVT
    };

private:
    std::map<std::string, LibertyCellTiming> liberty_cells_;
    bool liberty_loaded_;
    const void *liberty_full_;  // Liberty::LibertyLibrary pointer for NLDM
    std::vector<std::string> liberty_cell_names_;  // cell names found during netlist parsing

    // Helper methods
    void buildTimingGraph();
    void checkConstraints();
    void generateReport();

    int findNode(const std::string &name) const;
    int findOrCreateNode(const std::string &name, TimingNode::Type type);
    void addEdge(int from, int to, double delay, TimingEdge::Type type);
};

/* ========== Main Timing Function ========== */
TimingReport analyzeTiming(const ::Synthesis::RTLIL::Design &design,
                          const std::string &moduleName,
                          const std::string &sdcFile = "");

// Set timing log callback (C-compatible)
void set_timing_log_callback(void (*cb)(const char *, const char *));

// Data export: serialize timing structures to JSON
std::string timing_path_to_json(const TimingPath &path, int idx);
std::string violation_to_json(const SetupHoldViolation &v);
std::string timing_report_to_json(const TimingReport &report);

} // namespace TimingAnalysis

#endif /* TIMING_ANALYZER_H */
