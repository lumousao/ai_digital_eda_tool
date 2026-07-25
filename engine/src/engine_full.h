/**
 * Complete RTL Engine
 * 
 * Integrates all components:
 * - Complete Verilog-2001/2005/SystemVerilog parser
 * - Complete synthesis framework
 * - Complete simulation engine
 * - Complete static timing analysis
 * 
 * This is the main entry point for the complete RTL toolchain.
 */

#ifndef ENGINE_FULL_H
#define ENGINE_FULL_H

#include "verilog_parser_full.h"
#include "synthesis.h"
#include "simulator_full.h"
#include "timing_full.h"
#include <string>
#include <vector>
#include <memory>

namespace Engine {

/* ========== Engine Configuration ========== */
struct EngineConfig {
    bool enableSystemVerilog;
    bool enableOptimization;
    bool enableTimingDriven;
    bool enablePowerDriven;
    int optimizationEffort;
    int verbosity;
    
    EngineConfig() : enableSystemVerilog(true), enableOptimization(true),
                     enableTimingDriven(false), enablePowerDriven(false),
                     optimizationEffort(1), verbosity(0) {}
};

/* ========== Engine Result ========== */
struct EngineResult {
    bool success;
    std::string errorMessage;
    std::vector<std::string> warnings;
    
    // Parse results
    VerilogParser::ParseResult parseResult;
    
    // Synthesis results
    Synthesis::SynthReport synthReport;
    
    // Simulation results
    Simulator::SimResult simResult;
    
    // Timing results
    Timing::TimingReport timingReport;
    
    EngineResult() : success(false) {}
    
    std::string toString() const;
};

/* ========== RTL Engine ========== */
class RTLEngine {
public:
    RTLEngine();
    ~RTLEngine();
    
    // Configuration
    void setConfig(const EngineConfig &config);
    EngineConfig getConfig() const { return config_; }
    
    // Parse Verilog
    bool parseFile(const std::string &filename);
    bool parseString(const std::string &code, const std::string &name = "<input>");
    bool parseFiles(const std::vector<std::string> &filenames);
    
    // Synthesize
    bool synthesize();
    bool synthesizeWithLibrary(const Synthesis::Library &lib);
    
    // Simulate
    bool simulate(int numCycles = 100);
    bool simulateWithTestbench(const std::string &tbCode);
    
    // Timing analysis
    bool analyzeTiming();
    bool analyzeTimingWithSdc(const std::string &sdcFile);
    
    // Full flow
    bool runFullFlow(const std::string &filename);
    bool runFullFlowWithConfig(const std::string &filename, const EngineConfig &config);
    
    // Get results
    const EngineResult &getResult() const { return result_; }
    
    // Get components
    const VerilogParser::Parser &getParser() const { return parser_; }
    const Synthesis::SynthesisEngine &getSynthesizer() const { return synthesizer_; }
    const Simulator::SimulationEngine &getSimulator() const { return simulator_; }
    const Timing::TimingAnalyzer &getTimingAnalyzer() const { return timingAnalyzer_; }
    
    // Export results
    bool exportVerilog(const std::string &filename);
    bool exportVcd(const std::string &filename);
    bool exportSdc(const std::string &filename);
    bool exportReport(const std::string &filename);
    
    // Debug
    void setDebug(bool enable) { debug_ = enable; }
    void setVerbose(bool enable) { verbose_ = enable; }
    
    // Statistics
    size_t getModuleCount() const;
    size_t getCellCount() const;
    size_t getWireCount() const;
    double getTotalArea() const;
    double getTotalPower() const;
    
private:
    // Components
    VerilogParser::Parser parser_;
    Synthesis::SynthesisEngine synthesizer_;
    Simulator::SimulationEngine simulator_;
    Timing::TimingAnalyzer timingAnalyzer_;
    
    // Configuration
    EngineConfig config_;
    
    // Results
    EngineResult result_;
    
    // Debug
    bool debug_;
    bool verbose_;
    
    // Helper methods
    void initialize();
    bool validateInput(const std::string &code);
    void generateReport();
    void logMessage(const std::string &msg, int level = 0);
};

/* ========== Utility Functions ========== */

// Parse Verilog file
VerilogParser::ParseResult parseVerilogFile(const std::string &filename);

// Parse Verilog string
VerilogParser::ParseResult parseVerilogString(const std::string &code, 
                                             const std::string &name = "<input>");

// Synthesize design
Synthesis::SynthReport synthesizeDesign(const VerilogParser::ParseResult &result);

// Simulate design
Simulator::SimResult simulateDesign(const VerilogParser::ParseResult &result,
                                   int numCycles = 100);

// Analyze timing
Timing::TimingReport analyzeTiming(const Synthesis::RTLIL::Design &design,
                                  const std::string &moduleName);

// Full flow
EngineResult runFullFlow(const std::string &filename);

// Version information
std::string getEngineVersion();
std::string getEngineBuildInfo();

} // namespace Engine

#endif /* ENGINE_FULL_H */
