/**
 * Complete Simulation Engine - Event-Driven Behavioral Simulator
 *
 * References:
 * - industry-standard simulator
 * - Icarus Verilog vvp/
 * - Verilog-2005 standard
 */

#ifndef SIMULATOR_FULL_H
#define SIMULATOR_FULL_H

#include "rtlil.h"
#include "verilog_parser_full.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdint>
#include <functional>
#include <optional>
#include <fstream>

namespace Simulator {

/* ========== Signal Value ========== */
struct SignalValue {
    std::vector<uint8_t> bits;  // Each bit: 0, 1, x, z
    
    SignalValue() = default;
    SignalValue(int width) : bits(width, 0) {}
    SignalValue(int width, int val);
    SignalValue(const std::string &binary);  // "1010" format
    
    int width() const { return (int)bits.size(); }
    int to_int() const;
    std::string to_string() const;
    std::string to_hex() const;
    std::string to_binary() const;
    
    void set_bit(int pos, uint8_t val);
    uint8_t get_bit(int pos) const;
    
    bool operator==(const SignalValue &o) const;
    bool operator!=(const SignalValue &o) const { return !(*this == o); }
    
    // Arithmetic operators
    SignalValue operator+(const SignalValue &o) const;
    SignalValue operator-(const SignalValue &o) const;
    SignalValue operator*(const SignalValue &o) const;
    SignalValue operator/(const SignalValue &o) const;
    SignalValue operator%(const SignalValue &o) const;
    
    // Bitwise operators
    SignalValue operator&(const SignalValue &o) const;
    SignalValue operator|(const SignalValue &o) const;
    SignalValue operator^(const SignalValue &o) const;
    SignalValue operator~() const;
    
    // Shift operators
    SignalValue operator<<(int shift) const;
    SignalValue operator>>(int shift) const;
    SignalValue shift_left_arithmetic(int shift) const;  // Arithmetic shift left
    SignalValue shift_right_arithmetic(int shift) const;  // Arithmetic shift right
    
    // Comparison operators
    bool operator<(const SignalValue &o) const;
    bool operator>(const SignalValue &o) const;
    bool operator<=(const SignalValue &o) const;
    bool operator>=(const SignalValue &o) const;
    
    // Reduction operators
    bool uand() const;  // AND reduction
    bool uor() const;   // OR reduction
    bool uxor() const;  // XOR reduction
    
    // Utility methods
    SignalValue resize(int new_width) const;
    SignalValue sign_extend(int new_width) const;
    SignalValue zero_extend(int new_width) const;
    
    static SignalValue from_int(int val, int width);
    static SignalValue from_hex(const std::string &hex, int width);
    static SignalValue from_binary(const std::string &bin, int width);
    static SignalValue x_val(int width);  // All x's
    static SignalValue z_val(int width);  // All z's
};

/* ========== Event ========== */
struct Event {
    enum Type { ASSIGN, DISPLAY, FINISH, DELAY, POSEDGE, NEGEDGE };
    Type type;
    int time;
    std::string target;
    SignalValue value;
    std::string message;
    
    Event() : type(ASSIGN), time(0) {}
    Event(Type t, int t_time, const std::string &tgt, const SignalValue &val)
        : type(t), time(t_time), target(tgt), value(val) {}
};

/* ========== Simulation Context ========== */
class SimContext {
public:
    SimContext();
    ~SimContext();
    
    // Load design
    void loadDesign(const RTLIL::Design &design);
    void loadModule(const std::string &moduleName);
    
    // Set input values
    void setInput(const std::string &name, const SignalValue &value);
    void setInputBit(const std::string &name, int bit, uint8_t value);
    
    // Get signal values
    SignalValue getSignal(const std::string &name) const;
    uint8_t getSignalBit(const std::string &name, int bit) const;
    
    // Run simulation
    void run(int numCycles);
    void runUntil(int time);
    void step();
    
    // Time management
    int getCurrentTime() const { return currentTime_; }
    void advanceTime(int delta);
    
    // Event queue
    void scheduleEvent(const Event &event);
    void processEvents();
    
    // VCD output
    void dumpVcd(const std::string &filename) const;
    void dumpWaveform(std::ostream &out) const;
    
    // Debug
    void setDebug(bool enable) { debug_ = enable; }
    void setVerbose(bool enable) { verbose_ = enable; }
    
    // Statistics
    int getEventCount() const { return eventCount_; }
    int getToggleCount() const { return toggleCount_; }

    // Helper methods (public for engine access)
    void initializeSignals();
    void evaluateCombinational();
    void evaluateSequential();
    void applyUpdates();

    // Signal storage (public for testbench access)
    std::map<std::string, SignalValue> signals_;
    std::map<std::string, SignalValue> nextSignals_;

private:
    // Design
    RTLIL::Design design_;
    std::string moduleName_;
    
    // Event queue
    std::vector<Event> eventQueue_;
    std::vector<Event> pendingEvents_;
    
    // Time
    int currentTime_;
    int cycleCount_;
    
    // Statistics
    int eventCount_;
    int toggleCount_;
    
    // Debug
    bool debug_;
    bool verbose_;
    
    // Helper methods (moved to public)
    void evaluateAlwaysBlock(const RTLIL::Process &proc);
    void checkAssertions();
    
    // Gate evaluation
    SignalValue evaluateGate(const RTLIL::Cell &cell);
    SignalValue evaluateAnd(const SignalValue &a, const SignalValue &b);
    SignalValue evaluateOr(const SignalValue &a, const SignalValue &b);
    SignalValue evaluateXor(const SignalValue &a, const SignalValue &b);
    SignalValue evaluateNot(const SignalValue &a);
    SignalValue evaluateMux(const SignalValue &a, const SignalValue &b, const SignalValue &sel);
    SignalValue evaluateDff(const SignalValue &d, const SignalValue &clk, const SignalValue &rst);
    
    // Expression evaluation
    SignalValue evaluateExpression(const std::string &expr);
    SignalValue evaluateBinaryOp(const std::string &op, const SignalValue &a, const SignalValue &b);
    SignalValue evaluateUnaryOp(const std::string &op, const SignalValue &a);
};

/* ========== Testbench ========== */
class Testbench {
public:
    Testbench();
    ~Testbench();
    
    // Load testbench
    bool load(const std::string &filename);
    bool loadString(const std::string &code);
    
    // Run testbench
    bool run(SimContext &ctx);
    
    // Check results
    bool checkExpected(const std::string &signal, const SignalValue &expected);
    bool checkAllPassed() const;
    
    // Get report
    std::string getReport() const;
    
    // Callbacks
    using Callback = std::function<void(SimContext &, int)>;
    void setMonitorCallback(Callback cb) { monitorCb_ = cb; }
    void setFinishCallback(Callback cb) { finishCb_ = cb; }
    
private:
    std::string code_;
    std::vector<Event> events_;
    std::map<std::string, SignalValue> expectedValues_;
    bool passed_;
    std::string report_;
    
    Callback monitorCb_;
    Callback finishCb_;
    
    // Parser helpers
    bool parseTestbench();
    bool parseStimulus();
    bool parseExpected();
};

/* ========== Simulation Engine ========== */
class SimulationEngine {
public:
    SimulationEngine();
    ~SimulationEngine();
    
    // Load design
    bool loadDesign(const VerilogParser::ParseResult &parseResult);
    bool loadGateLevel(const RTLIL::Design &design);
    
    // Load testbench
    bool loadTestbench(const std::string &filename);
    bool loadTestbenchString(const std::string &code);
    
    // Run simulation
    bool simulate(int numCycles = 100);
    bool simulateWithTimeout(int numCycles, int timeout);
    
    // Get results
    bool getPassed() const { return passed_; }
    std::string getReport() const;
    std::string getVcd() const;
    
    // Configuration
    void setClockPeriod(int period) { clockPeriod_ = period; }
    void setClockPort(const std::string &port) { clockPort_ = port; }
    void setResetPort(const std::string &port) { resetPort_ = port; }
    void setResetActiveLow(bool activeLow) { resetActiveLow_ = activeLow; }
    
    // Debug
    void setDebug(bool enable) { debug_ = enable; }
    void setVerbose(bool enable) { verbose_ = enable; }
    
    // Statistics
    int getCycleCount() const { return cycleCount_; }
    int getEventCount() const { return ctx_ ? ctx_->getEventCount() : 0; }
    int getToggleCount() const { return ctx_ ? ctx_->getToggleCount() : 0; }
    
private:
    std::unique_ptr<SimContext> ctx_;
    std::unique_ptr<Testbench> tb_;
    bool passed_;
    int clockPeriod_;
    std::string clockPort_;
    std::string resetPort_;
    bool resetActiveLow_;
    int cycleCount_;
    bool debug_;
    bool verbose_;
    std::string report_;
    
    // Helper methods
    void generateClock();
    void applyReset();
    void checkResults();
};

/* ========== Main Simulation Function ========== */
struct SimResult {
    bool passed;
    int timeSteps;
    std::string output;
    std::string vcdFile;
    std::string report;
    
    SimResult() : passed(false), timeSteps(0) {}
};

SimResult simulateDesign(const VerilogParser::ParseResult &parseResult,
                        const std::string &testbenchCode = "",
                        int numCycles = 100);

} // namespace Simulator

#endif /* SIMULATOR_FULL_H */
