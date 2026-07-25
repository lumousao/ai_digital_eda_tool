/**
 * Complete Static Timing Analysis Engine
 * 
 * References:
 * - OpenSTA (complete timing analysis)
 * - PrimeTime (industry standard)
 * - Synopsys Design Constraints (SDC)
 * 
 * This is a complete static timing analysis engine.
 */

#ifndef TIMING_FULL_H
#define TIMING_FULL_H

#include "synthesis.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace Timing {

/* ========== Timing Constraints ========== */
struct Clock {
    std::string name;
    double period;
    double waveformRise;
    double waveformFall;
    std::string port;
    std::string source;  // For generated clocks

    Clock() : period(10.0), waveformRise(0.0), waveformFall(5.0) {}
};

struct InputDelay {
    std::string port;
    double delay;
    std::string clock;
    bool isRising;
    
    InputDelay() : delay(0.0), isRising(true) {}
};

struct OutputDelay {
    std::string port;
    double delay;
    std::string clock;
    bool isRising;
    
    OutputDelay() : delay(0.0), isRising(true) {}
};

struct FalsePath {
    std::string from;
    std::string to;
    std::string through;
    
    FalsePath() = default;
};

struct MulticyclePath {
    std::string from;
    std::string to;
    int cycles;
    bool is_setup;  // true=setup multicycle, false=hold multicycle

    MulticyclePath() : cycles(1), is_setup(true) {}
};

// Additional SDC constraint types
struct ClockUncertainty {
    double setup;
    double hold;
    ClockUncertainty() : setup(0.0), hold(0.0) {}
};

struct ClockLatency {
    double value;
    bool is_source;  // true=source latency, false=network latency
    bool rise;       // true=rise, false=fall
    ClockLatency() : value(0.0), is_source(true), rise(true) {}
};

struct MaxDelayConstraint {
    std::string from, to;
    double delay;
    MaxDelayConstraint() : delay(0.0) {}
};

struct MinDelayConstraint {
    std::string from, to;
    double delay;
    MinDelayConstraint() : delay(0.0) {}
};

struct TimingDerate {
    double factor;
    bool early;      // true=early/min derate, false=late/max derate
    bool cell_check; // true=cell check, false=data path
    TimingDerate() : factor(1.0), early(true), cell_check(false) {}
};

/* ========== Timing Graph ========== */
struct TimingNode {
    std::string name;
    enum Type { INPUT, OUTPUT, INTERNAL };
    Type type;
    int arrivalTime;
    int requiredTime;
    int slack;
    bool isClock;
    bool isConstrained;
    
    TimingNode() : type(INTERNAL), arrivalTime(0), requiredTime(0), 
                   slack(0), isClock(false), isConstrained(false) {}
};

struct TimingEdge {
    int fromNode;
    int toNode;
    double delay;
    double transition;
    enum Type { COMBINATIONAL, SEQUENTIAL, CLOCK };
    Type type;
    
    TimingEdge() : fromNode(-1), toNode(-1), delay(0.0), transition(0.0), 
                   type(COMBINATIONAL) {}
};

struct TimingPath {
    std::vector<int> nodes;
    std::vector<int> edges;
    double totalDelay;
    double slack;
    bool isMet;
    bool isClockPath;
    bool isFalsePath;
    int startNode;
    int endNode;

    TimingPath() : totalDelay(0.0), slack(0.0), isMet(true),
                   isClockPath(false), isFalsePath(false),
                   startNode(-1), endNode(-1) {}
};

/* ========== Timing Report ========== */
struct TimingReport {
    std::string moduleName;
    double clockPeriod;
    double clockFrequency;
    int setupViolations;
    int holdViolations;
    int totalPaths;
    int maxPathDelay;
    int minPathDelay;
    std::vector<TimingPath> paths;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    
    TimingReport() : clockPeriod(10.0), clockFrequency(100.0),
                     setupViolations(0), holdViolations(0),
                     totalPaths(0), maxPathDelay(0), minPathDelay(0) {}
    
    std::string toString() const;
};

/* ========== Timing Analyzer ========== */
class TimingAnalyzer {
public:
    TimingAnalyzer();
    ~TimingAnalyzer();
    
    // Set design
    void setDesign(const Synthesis::RTLIL::Design &design);
    void setModule(const std::string &moduleName);
    
    // Set constraints
    void setClock(const Clock &clock);
    void setInputDelay(const InputDelay &delay);
    void setOutputDelay(const OutputDelay &delay);
    void addFalsePath(const FalsePath &path);
    void addMulticyclePath(const MulticyclePath &path);
    
    // Load SDC file
    bool loadSdc(const std::string &filename);
    
    // Run timing analysis
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
    void setAnalysisMode(const std::string &mode) { analysisMode_ = mode; }
    
    // Debug
    void setDebug(bool enable) { debug_ = enable; }
    void setVerbose(bool enable) { verbose_ = enable; }
    
private:
    // Design
    Synthesis::RTLIL::Design design_;
    std::string moduleName_;
    
    // Constraints
    Clock clock_;
    std::vector<InputDelay> inputDelays_;
    std::vector<OutputDelay> outputDelays_;
    std::vector<FalsePath> falsePaths_;
    std::vector<MulticyclePath> multicyclePaths_;
    
    // Timing graph
    std::vector<TimingNode> nodes_;
    std::vector<TimingEdge> edges_;
    std::vector<TimingPath> paths_;
    
    // Report
    TimingReport report_;
    
    // Configuration
    bool earlyMode_;
    bool lateMode_;
    std::string analysisMode_;
    bool debug_;
    bool verbose_;
    
    // Helper methods
    void buildTimingGraph();
    void computeArrivalTimes();
    void computeRequiredTimes();
    void computeSlack();
    void findPaths();
    void checkConstraints();
    void generateReport();
    
    // Graph traversal
    void traverseForward(int nodeIdx, int time);
    void traverseBackward(int nodeIdx, int time);
    void findLongestPath();
    void findShortestPath();
    
    // Constraint checking
    bool checkSetupConstraint(const TimingPath &path);
    bool checkHoldConstraint(const TimingPath &path);
    bool isFalsePath(const TimingPath &path) const;
    
    // Utility methods
    int findNode(const std::string &name) const;
    int findOrCreateNode(const std::string &name, TimingNode::Type type);
    void addEdge(int from, int to, double delay, TimingEdge::Type type);
};

/* ========== SDC Parser ========== */
class SdcParser {
public:
    SdcParser();
    ~SdcParser();
    
    // Parse SDC file
    bool parse(const std::string &filename);
    
    // Get results
    const std::vector<Clock> &getClocks() const { return clocks_; }
    const std::vector<InputDelay> &getInputDelays() const { return inputDelays_; }
    const std::vector<OutputDelay> &getOutputDelays() const { return outputDelays_; }
    const std::vector<FalsePath> &getFalsePaths() const { return falsePaths_; }
    const std::vector<MulticyclePath> &getMulticyclePaths() const { return multicyclePaths_; }
    const std::vector<ClockUncertainty> &getClockUncertainties() const { return clockUncertainties_; }
    const std::vector<ClockLatency> &getClockLatencies() const { return clockLatencies_; }
    const std::vector<MaxDelayConstraint> &getMaxDelayConstraints() const { return maxDelayConstraints_; }
    const std::vector<MinDelayConstraint> &getMinDelayConstraints() const { return minDelayConstraints_; }
    const std::vector<TimingDerate> &getTimingDerates() const { return timingDerates_; }
    const std::map<std::string, double> &getDriveConstraints() const { return driveConstraints_; }
    const std::map<std::string, double> &getLoadConstraints() const { return loadConstraints_; }
    const std::vector<std::pair<std::string,std::string>> &getClockGroupExclusions() const { return clockGroupExclusions_; }

private:
    std::vector<Clock> clocks_;
    std::vector<InputDelay> inputDelays_;
    std::vector<OutputDelay> outputDelays_;
    std::vector<FalsePath> falsePaths_;
    std::vector<MulticyclePath> multicyclePaths_;
    std::vector<ClockUncertainty> clockUncertainties_;
    std::vector<ClockLatency> clockLatencies_;
    std::vector<MaxDelayConstraint> maxDelayConstraints_;
    std::vector<MinDelayConstraint> minDelayConstraints_;
    std::vector<TimingDerate> timingDerates_;
    std::map<std::string, double> driveConstraints_;
    std::map<std::string, double> loadConstraints_;
    std::vector<std::pair<std::string,std::string>> clockGroupExclusions_;

    // Parser methods
    bool parseCommand(const std::string &cmd);
    bool parseCreateClock(const std::string &args);
    bool parseCreateGeneratedClock(const std::string &args);
    bool parseSetInputDelay(const std::string &args);
    bool parseSetOutputDelay(const std::string &args);
    bool parseSetFalsePath(const std::string &args);
    bool parseSetMulticyclePath(const std::string &args);
    bool parseSetClockGroups(const std::string &args);
    bool parseSetClockUncertainty(const std::string &args);
    bool parseSetClockLatency(const std::string &args);
    bool parseSetMaxDelay(const std::string &args);
    bool parseSetMinDelay(const std::string &args);
    bool parseSetDrive(const std::string &args);
    bool parseSetLoad(const std::string &args);
    bool parseSetTimingDerate(const std::string &args);

    // Utility methods
    std::vector<std::string> tokenize(const std::string &str);
    double parseTime(const std::string &str);
    std::string parsePort(const std::string &str);
};

/* ========== Liberty Library ========== */
struct LibertyCell {
    std::string name;
    std::string type;
    double area;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::map<std::string, double> delays;
    std::map<std::string, double> powers;
    
    LibertyCell() : area(0.0) {}
};

struct LibertyLibrary {
    std::string name;
    std::string technology;
    double voltage;
    double temperature;
    std::vector<LibertyCell> cells;
    
    const LibertyCell *findCell(const std::string &name) const;
    double getCellDelay(const std::string &cellName, const std::string &input, 
                        const std::string &output) const;
};

/* ========== Timing Calculator ========== */
class TimingCalculator {
public:
    TimingCalculator();
    ~TimingCalculator();
    
    // Set library
    void setLibrary(const LibertyLibrary &lib);
    
    // Calculate delay
    double calculateGateDelay(const std::string &cellType, 
                             const std::vector<double> &inputSlews,
                             double outputCap);
    double calculateWireDelay(double length, double capacitance);
    double calculateNetDelay(const std::string &netName);
    
    // Calculate arrival time
    double calculateArrivalTime(const TimingPath &path);
    
    // Calculate required time
    double calculateRequiredTime(const TimingPath &path, double clockPeriod);
    
    // Calculate slack
    double calculateSlack(double arrivalTime, double requiredTime);
    
    // Calculate frequency
    double calculateMaxFrequency(const TimingReport &report);
    
private:
    LibertyLibrary library_;
    
    // Delay models
    double wireDelayModel(double length, double cap);
    double gateDelayModel(const LibertyCell &cell, double inputSlew, double outputCap);
    
    // Power models
    double calculateTogglePower(const std::string &cellType, double frequency);
    double calculateLeakagePower(const std::string &cellType);
};

/* ========== Main Timing Function ========== */
TimingReport analyzeTiming(const Synthesis::RTLIL::Design &design,
                          const std::string &moduleName,
                          const std::string &sdcFile = "",
                          const LibertyLibrary &lib = LibertyLibrary());

} // namespace Timing

#endif /* TIMING_FULL_H */
