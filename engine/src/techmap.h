/**
 * Technology Mapping - Industrial-grade technology mapping framework
 *
 * Features:
 * - ASIC standard cell library
 * - FPGA LUT mapping
 * - Technology independent optimization
 * - Cell selection and area/timing optimization
 * - Clock tree synthesis
 * - Buffer insertion
 * - Wire load estimation
 */

#ifndef TECHMAP_INDUSTRIAL_H
#define TECHMAP_INDUSTRIAL_H

#include "rtlil.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>

namespace TechMap {

// ============================================================================
// Forward declarations
// ============================================================================

struct TechLibrary;
struct TechCell;
struct TechPin;
struct TechArc;
struct TechMapper;
struct TechOptimizer;

// ============================================================================
// Cell pin types
// ============================================================================

enum class PinType {
    INPUT,
    OUTPUT,
    INOUT,
    POWER,
    GROUND,
    CLOCK
};

// ============================================================================
// Cell function types
// ============================================================================

enum class CellFunction {
    // Logic gates
    BUF,
    INV,
    AND2,
    AND3,
    AND4,
    OR2,
    OR3,
    OR4,
    NAND2,
    NAND3,
    NAND4,
    NOR2,
    NOR3,
    NOR4,
    XOR2,
    XNOR2,
    AOI21,
    AOI22,
    OAI21,
    OAI22,
    MUX2,
    MUX4,
    MUX8,

    // Sequential
    DFF,
    DFFE,
    DFFR,
    DFFS,
    DFFRS,
    LATCH,
    LATCHR,
    LATCHS,

    // Arithmetic
    FA,
    HA,
    ADDER,
    SUBTRACTOR,
    MULTIPLIER,

    // Memory
    RAM,
    ROM,
    FLIPFLOP,

    // Special
    BUFG,
    BUFGCTRL,
    PLL,
    DLL,
    IOBUF,
    IBUF,
    OBUF,
    TBUF
};

// ============================================================================
// Cell timing model
// ============================================================================

struct CellTiming {
    double rise_delay;
    double fall_delay;
    double rise_transition;
    double fall_transition;
    double setup_time;
    double hold_time;
    double recovery_time;
    double removal_time;
    double clock_to_q;
    double max_capacitance;
    double max_transition;

    CellTiming() : rise_delay(0.0), fall_delay(0.0),
                   rise_transition(0.0), fall_transition(0.0),
                   setup_time(0.0), hold_time(0.0),
                   recovery_time(0.0), removal_time(0.0),
                   clock_to_q(0.0), max_capacitance(1e30),
                   max_transition(1e30) {}
};

// ============================================================================
// Cell power model
// ============================================================================

struct CellPower {
    double leakage_power;
    double internal_power;
    double switching_power;
    double dynamic_power;

    CellPower() : leakage_power(0.0), internal_power(0.0),
                  switching_power(0.0), dynamic_power(0.0) {}
};

// ============================================================================
// TechPin - Technology cell pin
// ============================================================================

struct TechPin {
    std::string name;
    PinType type;
    double capacitance;
    double max_capacitance;
    double max_transition;
    double drive_resistance;
    std::vector<std::string> functions;

    TechPin() : type(PinType::INPUT), capacitance(0.0),
                max_capacitance(1e30), max_transition(1e30),
                drive_resistance(0.0) {}
};

// ============================================================================
// TechArc - Technology cell arc
// ============================================================================

struct TechArc {
    std::string from_pin;
    std::string to_pin;
    CellTiming timing;
    bool is_setup;
    bool is_hold;
    bool is_clock;
    bool is_reset;

    TechArc() : is_setup(false), is_hold(false), is_clock(false), is_reset(false) {}
};

// ============================================================================
// TechCell - Technology cell
// ============================================================================

struct TechCell {
    std::string name;
    std::string cell_type;
    CellFunction function;
    std::vector<TechPin> pins;
    std::vector<TechArc> arcs;
    CellTiming timing;
    CellPower power;
    double area;
    double leakage;
    bool is_sequential;
    bool is_clocked;
    bool is_buffer;
    bool is_inverter;
    std::string verilog_model;

    TechCell() : function(CellFunction::BUF), area(0.0), leakage(0.0),
                 is_sequential(false), is_clocked(false),
                 is_buffer(false), is_inverter(false) {}

    TechPin *findPin(const std::string &name);
    const TechPin *findPin(const std::string &name) const;
    TechArc *findArc(const std::string &from, const std::string &to);
    bool hasPin(const std::string &name) const;
    int getInputCount() const;
    int getOutputCount() const;
};

// ============================================================================
// TechLibrary - Technology library
// ============================================================================

struct TechLibrary {
    std::string name;
    std::string vendor;
    std::string technology;
    std::vector<TechCell> cells;
    std::map<std::string, TechCell*> cell_map;
    std::map<CellFunction, std::vector<TechCell*>> function_map;

    // Library properties
    double nom_voltage;
    double nom_temperature;
    double nom_capacitance;
    double default_max_transition;
    double default_max_capacitance;
    double default_max_fanout;
    double default_wire_load;
    std::string default_wire_load_mode;

    TechLibrary() : nom_voltage(1.0), nom_temperature(25.0),
                    nom_capacitance(0.0), default_max_transition(0.5),
                    default_max_capacitance(0.5), default_max_fanout(40),
                    default_wire_load(0.0), default_wire_load_mode("enclosed") {}

    TechCell *findCell(const std::string &name);
    std::vector<TechCell*> getCellsByFunction(CellFunction func);
    std::vector<TechCell*> getBuffers();
    std::vector<TechCell*> getInverters();
    std::vector<TechCell*> getFlipFlops();
    std::vector<TechCell*> getLatches();
    TechCell *getBestCell(CellFunction func, const std::string &constraint = "area");
    TechCell *getSmallestCell(CellFunction func);
    TechCell *getFastestCell(CellFunction func);

    void addCell(const TechCell &cell);
    void buildIndex();
};

// ============================================================================
// TechMapper - Technology mapper
// ============================================================================

struct TechMapper {
    TechLibrary *library;

    TechMapper() : library(nullptr) {}
    TechMapper(TechLibrary *lib) : library(lib) {}

    // Map RTL to technology
    void mapModule(RTLIL::Module *mod);
    void mapCell(RTLIL::Module *mod, RTLIL::Cell *cell);

    // Cell mapping
    RTLIL::Cell *mapLogic(RTLIL::Module *mod, const std::string &type,
                          const std::vector<RTLIL::SigSpec> &inputs,
                          const RTLIL::SigSpec &output);
    RTLIL::Cell *mapMux(RTLIL::Module *mod, const RTLIL::SigSpec &A,
                        const RTLIL::SigSpec &B, const RTLIL::SigSpec &S,
                        const RTLIL::SigSpec &Y);
    RTLIL::Cell *mapFF(RTLIL::Module *mod, const RTLIL::SigSpec &D,
                       const RTLIL::SigSpec &Q, const RTLIL::SigSpec &CLK,
                       const RTLIL::SigSpec &EN = RTLIL::SigSpec(),
                       const RTLIL::SigSpec &RST = RTLIL::SigSpec());

    // Buffer insertion
    void insertBuffers(RTLIL::Module *mod);
    void insertBuffer(RTLIL::Module *mod, const RTLIL::SigSpec &signal,
                      const std::string &drive_strength = "1");

    // Clock tree synthesis
    void clockTreeSynthesis(RTLIL::Module *mod);

    // Wire load estimation
    double estimateWireLoad(const RTLIL::SigSpec &sig);
};

// ============================================================================
// TechOptimizer - Technology optimizer
// ============================================================================

struct TechOptimizer {
    TechLibrary *library;

    TechOptimizer() : library(nullptr) {}
    TechOptimizer(TechLibrary *lib) : library(lib) {}

    // Area optimization
    void optimizeArea(RTLIL::Module *mod);
    void replaceCell(RTLIL::Module *mod, RTLIL::Cell *old_cell,
                     TechCell *new_cell);

    // Timing optimization
    void optimizeTiming(RTLIL::Module *mod);
    void optimizeCriticalPath(RTLIL::Module *mod, const std::string &path);

    // Power optimization
    void optimizePower(RTLIL::Module *mod);
    void insertClockGating(RTLIL::Module *mod);
    void insertOperandIsolation(RTLIL::Module *mod);

    // Cell resizing
    void resizeCells(RTLIL::Module *mod);
    void resizeForTiming(RTLIL::Module *mod, const std::string &constraint);
    void resizeForArea(RTLIL::Module *mod);

    // Duplicate elimination
    void eliminateDuplicates(RTLIL::Module *mod);
    void mergeIdenticalCells(RTLIL::Module *mod);

    // Statistics
    double getArea(RTLIL::Module *mod);
    double getLeakagePower(RTLIL::Module *mod);
    int getCellCount(RTLIL::Module *mod);
    int getBufferCount(RTLIL::Module *mod);
};

// ============================================================================
// ASIC standard cell library
// ============================================================================

struct AsicLib {
    static TechLibrary createNangate45();
    static TechLibrary createSkywater130();
    static TechLibrary createTSMC65();
    static TechLibrary createGeneric();
};

// ============================================================================
// FPGA library
// ============================================================================

struct FpgaLib {
    static TechLibrary createXilinx7Series();
    static TechLibrary createXilinxUltraScale();
    static TechLibrary createIntelStratix();
    static TechLibrary createLatticeECP5();
    static TechLibrary createGeneric();
};

// ============================================================================
// Helper functions
// ============================================================================

// Create technology library
TechLibrary createStandardCellLibrary(const std::string &name = "generic");

// Map function to cell
CellFunction mapFunctionToCell(const std::string &func);

// Get cell function name
std::string getCellFunctionName(CellFunction func);

// Create cell from function
TechCell createCellFromFunction(CellFunction func, const std::string &prefix = "");

// Optimize technology
void optimizeTechnology(RTLIL::Module *mod, TechLibrary *lib);

// Print technology statistics
void printTechStats(RTLIL::Module *mod, TechLibrary *lib);

} // namespace TechMap

#endif // TECHMAP_INDUSTRIAL_H
