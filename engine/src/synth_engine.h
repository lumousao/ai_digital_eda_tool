/**
 * Native Logic Synthesis Engine
 *
 * References:
 * - industry-standard synthesis (complete synthesis flow)
 * - industry-standard optimization (optimization passes)
 * - industry-standard technology mapping (technology mapping)
 * - ABC (logic synthesis and verification)
 * - Synopsys Design Compiler (industry standard)
 *
 * Features:
 * - Complete Verilog-2001/2005/SystemVerilog support
 * - Multi-level logic optimization
 * - Technology mapping (ASIC/FPGA)
 * - FSM extraction and optimization
 * - Resource sharing
 * - Clock gating
 * - Power optimization
 * - Area optimization
 * - Timing-driven synthesis
 */

#ifndef SYNTH_ENGINE_H
#define SYNTH_ENGINE_H

#include "synthesis.h"
#include "verilog_parser_full.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace Synthesis {

/* ========== Technology Library ========== */
struct TechCell {
    std::string name;
    std::string type;
    std::string category;  // "combinational", "sequential", "IO"
    double area;
    double leakage_power;
    std::map<std::string, double> delay;  // input_pin -> output_pin delay
    std::map<std::string, double> power;  // toggle power
    std::string function;  // Boolean function
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;

    TechCell() : area(0.0), leakage_power(0.0) {}
};

struct TechLibrary {
    std::string name;
    std::string technology;  // "asic", "fpga"
    double voltage;
    double temperature;
    std::vector<TechCell> cells;

    const TechCell *findCell(const std::string &name) const;
    std::vector<TechCell> findCellsByType(const std::string &type) const;
    std::vector<TechCell> findCellsByArea(double max_area) const;
};

/* ========== Optimization Passes ========== */
class OptPass {
public:
    virtual ~OptPass() = default;
    virtual std::string getName() const = 0;
    virtual bool run(RTLIL::Design *design) = 0;
};

// Constant propagation and folding
class SynthConstPropPass : public OptPass {
public:
    std::string getName() const override { return "constprop"; }
    bool run(RTLIL::Design *design) override;
};

// Dead code elimination
class DeadCodeElimPass : public OptPass {
public:
    std::string getName() const override { return "dce"; }
    bool run(RTLIL::Design *design) override;
};

// Expression optimization
class SynthOptExprPass : public OptPass {
public:
    std::string getName() const override { return "opt_expr"; }
    bool run(RTLIL::Design *design) override;
};

// Logic sharing
class LogicSharePass : public OptPass {
public:
    std::string getName() const override { return "logic_share"; }
    bool run(RTLIL::Design *design) override;
};

// Resource sharing
class SynthResourceSharePass : public OptPass {
public:
    std::string getName() const override { return "resource_share"; }
    bool run(RTLIL::Design *design) override;
};

// FSM extraction
class SynthFSMExtractPass : public OptPass {
public:
    std::string getName() const override { return "fsm_extract"; }
    bool run(RTLIL::Design *design) override;
};

// FSM optimization
class SynthFSMOptPass : public OptPass {
public:
    std::string getName() const override { return "fsm_opt"; }
    bool run(RTLIL::Design *design) override;
};

// Technology mapping
class SynthTechMapPass : public OptPass {
public:
    std::string getName() const override { return "techmap"; }
    bool run(RTLIL::Design *design) override;
};

// Logic minimization
class LogicMinPass : public OptPass {
public:
    std::string getName() const override { return "logic_min"; }
    bool run(RTLIL::Design *design) override;
};

// Clock gating
class ClockGatePass : public OptPass {
public:
    std::string getName() const override { return "clk_gate"; }
    bool run(RTLIL::Design *design) override;
};

// Power optimization
class PowerOptPass : public OptPass {
public:
    std::string getName() const override { return "power_opt"; }
    bool run(RTLIL::Design *design) override;
};

/* ========== FSM ========== */
struct FSMState {
    std::string name;
    int encoding;
    std::vector<std::string> transitions;  // (next_state, condition)
    std::vector<std::string> outputs;

    FSMState() : encoding(0) {}
};

struct FSM {
    std::string name;
    std::string clock;
    std::string reset;
    std::vector<std::string> states;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::map<std::string, FSMState> state_map;
    std::string current_state;
    std::string next_state;

    FSM() = default;
};

/* ========== Synthesis Report ========== */
struct SynthReportV2 {
    size_t module_count;
    size_t wire_count;
    size_t cell_count;
    size_t dff_count;
    size_t lut_count;
    size_t and_count;
    size_t or_count;
    size_t not_count;
    size_t xor_count;
    size_t mux_count;
    size_t adder_count;
    size_t multiplier_count;
    size_t fsm_count;
    double total_area;
    double total_power;
    double critical_path_delay;
    int logic_depth;
    std::vector<std::string> optimizations_applied;

    SynthReportV2() : module_count(0), wire_count(0), cell_count(0), dff_count(0),
                    lut_count(0), and_count(0), or_count(0), not_count(0),
                    xor_count(0), mux_count(0), adder_count(0), multiplier_count(0),
                    fsm_count(0), total_area(0.0), total_power(0.0),
                    critical_path_delay(0.0), logic_depth(0) {}

    std::string toString() const;
};

/* ========== Synthesis Engine ========== */
class SynthEngine {
public:
    SynthEngine();
    ~SynthEngine();

    // Set technology library
    void setTechLibrary(const TechLibrary &lib);

    // Run full synthesis flow
    bool synthesize(const VerilogParser::ParseResult &parseResult);

    // Run specific optimization pass
    bool runPass(const std::string &passName);

    // Get results
    const ::Synthesis::RTLIL::Design &getDesign() const { return design_; }
    const TechLibrary &getTechLibrary() const { return techLib_; }
    const SynthReportV2 &getReport() const { return report_; }

    // Configuration
    void setOptimizationLevel(int level) { optLevel_ = level; }
    void setTargetTechnology(const std::string &tech) { targetTech_ = tech; }
    void setTimingDriven(bool enable) { timingDriven_ = enable; }
    void setPowerDriven(bool enable) { powerDriven_ = enable; }
    void setAreaDriven(bool enable) { areaDriven_ = enable; }

    // Get optimization passes
    std::vector<std::string> getAvailablePasses() const;

private:
    RTLIL::Design design_;
    TechLibrary techLib_;
    SynthReportV2 report_;
    int optLevel_;
    std::string targetTech_;
    bool timingDriven_;
    bool powerDriven_;
    bool areaDriven_;

    // Optimization passes
    std::vector<std::unique_ptr<OptPass>> passes_;

    // Synthesis steps
    bool elaboration();
    bool optimization();
    bool mapping();
    bool postMapping();

    // Helper methods
    void initPasses();
    bool runAllPasses();
    void generateReport();

    // FSM operations
    std::vector<FSM> extractFSMs();
    void optimizeFSM(FSM &fsm);
    void mapFSM(const FSM &fsm);

    // Technology mapping
    void mapToASIC();
    void mapToFPGA();
};

/* ========== Main Synthesis Function ========== */
SynthReportV2 synthesizeDesign(const VerilogParser::ParseResult &parseResult,
                            const TechLibrary &techLib = TechLibrary());

} // namespace Synthesis

/* ========== C API for Real Synthesis ========== */
#ifdef __cplusplus
extern "C" {
#endif

struct CppSynthResult {
    char *gate_verilog;
    char *report;
    size_t cell_count;
    size_t wire_count;
    size_t dff_count;
    size_t port_count;
    double area_ge;
    double area_um2;       // µm² from liberty library (0 if not available)
    bool area_from_lib;    // true if area_um2 comes from real lib
    char *lib_name;        // liberty library name used (NULL if none)
    int logic_depth;
    bool success;
    char *error;
    char **cell_types;
    size_t *cell_type_counts;
    size_t num_cell_types;
};

// Pass policy for the repository-native gate-netlist optimizer. All fields are
// integers to keep the boundary stable for Rust/C FFI callers.
struct NativeSynthesisOptions {
    int constprop;
    int dead_code_elimination;
    int common_subexpression_elimination;
    int expression_optimization;
    int demorgan;
    int width_reduction;
    int resource_sharing;
    int fsm_extraction;
    int logic_minimization;
    int retiming;
    int boundary_optimization;
};

CppSynthResult synth_real(const char *rtl_code, const char *module_name,
                           void (*log_cb)(const char *, const char *));
CppSynthResult synth_real_with_lib(const char *rtl_code, const char *module_name,
                                    const char *liberty_path,
                                    void (*log_cb)(const char *, const char *));
CppSynthResult synth_real_with_options(const char *rtl_code, const char *module_name,
                                        const char *liberty_path,
                                        const NativeSynthesisOptions *options,
                                        void (*log_cb)(const char *, const char *));
// Frequency-optimized synthesis: iteratively applies optimization passes
// to push max frequency toward target (default: 3x constraint)
CppSynthResult synth_real_freq_optimized(const char *rtl_code, const char *module_name,
                                          const char *liberty_path,
                                          int constraint_mhz, double target_ratio,
                                          void (*log_cb)(const char *, const char *));
void synth_result_free(CppSynthResult *r);

// Data export: serialize CppSynthResult to JSON string (caller must free)
char *synth_result_to_json(const CppSynthResult *r);

// Liberty library accessor: for use by timing/power analysis
const void *synth_get_liberty_lib();
int synth_is_liberty_loaded();

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_ENGINE_H */
