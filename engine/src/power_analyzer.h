#include "synthesis.h"
/**
 * Native Power Analysis Engine
 *
 * References:
 * - PrimePower (Synopsys power analysis)
 * - Voltus (Cadence power analysis)
 * - PowerArtist (Synopsys power optimization)
 *
 * Features:
 * - Static power analysis (leakage)
 * - Dynamic power analysis (switching)
 * - Glitch power analysis
 * - Clock power analysis
 * - Power optimization
 * - Power-aware synthesis
 */

#ifndef POWER_ANALYZER_H
#define POWER_ANALYZER_H

#include "synthesis.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace PowerAnalysis {

/* ========== Power Model ========== */
struct PowerCell {
    std::string name;
    std::string type;
    double leakage_power;      // Static power (mW)
    double dynamic_power;      // Dynamic power (mW/MHz)
    double capacitance;        // Input capacitance (fF)
    double switching_factor;   // Toggle rate (0-1)
    // NLDM 2D table: internal_power[index_transition][output_cap]
    std::vector<double> index_transition;  // input transition time axis values (ns)
    std::vector<double> index_capacitance; // output load capacitance axis values (fF)
    std::vector<std::vector<double>> internal_power_table; // [trans][cap] values in pJ or mW
    bool has_nldm_table;       // true if NLDM table data is populated

    PowerCell() : leakage_power(0.0), dynamic_power(0.0),
                  capacitance(0.0), switching_factor(0.0), has_nldm_table(false) {}

    /// Lookup internal power using bilinear interpolation
    /// @param trans input transition time (ns)
    /// @param cap output load capacitance (fF)
    /// @return interpolated internal power (same unit as table, typically pJ)
    double lookup_internal_power(double trans, double cap) const {
        if (!has_nldm_table || internal_power_table.empty()) return 0.0;
        if (index_transition.size() < 2 || index_capacitance.size() < 2) return 0.0;
        int nx = (int)index_transition.size();
        int ny = (int)index_capacitance.size();

        // Find bounding indices for transition (x-axis)
        int ix = 0;
        if (trans <= index_transition[0]) ix = 0;
        else if (trans >= index_transition[nx-1]) ix = nx - 2;
        else {
            for (int i = 0; i < nx - 1; i++) {
                if (trans >= index_transition[i] && trans <= index_transition[i+1]) {
                    ix = i; break;
                }
            }
        }

        // Find bounding indices for capacitance (y-axis)
        int iy = 0;
        if (cap <= index_capacitance[0]) iy = 0;
        else if (cap >= index_capacitance[ny-1]) iy = ny - 2;
        else {
            for (int j = 0; j < ny - 1; j++) {
                if (cap >= index_capacitance[j] && cap <= index_capacitance[j+1]) {
                    iy = j; break;
                }
            }
        }

        // Bilinear interpolation
        double x0 = index_transition[ix], x1 = index_transition[ix + 1];
        double y0 = index_capacitance[iy], y1 = index_capacitance[iy + 1];
        double v00 = internal_power_table[ix][iy], v01 = internal_power_table[ix][iy + 1];
        double v10 = internal_power_table[ix + 1][iy], v11 = internal_power_table[ix + 1][iy + 1];

        double dx = (trans - x0) / (x1 - x0);
        double dy = (cap - y0) / (y1 - y0);

        // Interpolate in x: v0 = v00 + dx*(v10-v00), v1 = v01 + dx*(v11-v01)
        double v0 = v00 + dx * (v10 - v00);
        double v1 = v01 + dx * (v11 - v01);

        // Interpolate in y
        return v0 + dy * (v1 - v0);
    }
};

struct PowerLibrary {
    std::string name;
    std::vector<PowerCell> cells;
    double nominal_voltage;       // Nominal supply voltage from Liberty (V)
    double process_factor;        // Process scaling factor (1.0 = typical)
    double temperature;           // Operating temperature (°C)

    PowerLibrary() : nominal_voltage(1.0), process_factor(1.0), temperature(25.0) {}
    const PowerCell *findCell(const std::string &type) const;
};

/* ========== Power Report ========== */
struct PowerReport {
    std::string module_name;
    double total_static_power;    // Total leakage power (mW)
    double total_dynamic_power;   // Total switching power (mW)
    double total_clock_power;     // Clock tree power (mW)
    double total_internal_power;  // Internal power (mW)
    double total_glitch_power;    // Glitch/hazard power (mW)
    double total_power;           // Total power (mW)
    double clock_frequency;       // Clock frequency (MHz)

    std::vector<std::pair<std::string, double>> cell_power;  // Per-cell power
    std::vector<std::pair<std::string, double>> net_power;   // Per-net power
    std::vector<std::pair<std::string, double>> clock_power; // Per-clock power

    PowerReport() : total_static_power(0.0), total_dynamic_power(0.0),
                    total_clock_power(0.0), total_internal_power(0.0),
                    total_glitch_power(0.0), total_power(0.0), clock_frequency(100.0) {}

    std::string toString() const;
    std::string toCSV() const;
};

/* ========== Power Analyzer ========== */
class PowerAnalyzer {
public:
    PowerAnalyzer();
    ~PowerAnalyzer();

    // Set design
    void setDesign(const ::Synthesis::RTLIL::Design &design);
    void setModule(const std::string &moduleName);

    // Set power library
    void setPowerLibrary(const PowerLibrary &lib);

    // Set clock frequency
    void setClockFrequency(double freq_mhz);

    // Run analysis
    PowerReport analyze();

    // Get report
    const PowerReport &getReport() const { return report_; }

    // Configuration
    void setEnableGlitchAnalysis(bool enable) { enableGlitch_ = enable; }
    void setEnableClockAnalysis(bool enable) { enableClock_ = enable; }

    // Toggle data from simulation (SAIF)
    void loadToggleData(const std::map<std::string, double> &toggle_rates);
    void loadSAIF(const std::string &filename);  // Parse SAIF file

    // Set timing context for accurate NLDM lookups
    void setTimingContext(const std::map<std::string, double> &slews,
                          const std::map<std::string, double> &caps) {
        timing_slews_ = slews;
        timing_caps_ = caps;
    }

private:
    ::Synthesis::RTLIL::Design design_;
    std::string moduleName_;
    PowerLibrary powerLib_;
    PowerReport report_;
    double clockFrequency_;
    bool enableGlitch_;
    bool enableClock_;
    std::map<std::string, double> toggle_data_; // signal → toggle rate
    std::map<std::string, double> timing_slews_; // cell_key → input_transition (ns)
    std::map<std::string, double> timing_caps_;  // cell_key → output_capacitance (fF)

    // Helper methods
    void analyzeStaticPower();
    void analyzeDynamicPower();
    void analyzeClockPower();
    void analyzeInternalPower();
    void analyzeGlitchPower();
    void generateReport();

    // Toggle rate estimation
    double estimateToggleRate(const std::string &signal);
    double estimateToggleRate(const ::Synthesis::RTLIL::Cell &cell);
};

/* ========== Power Optimizer ========== */
class PowerOptimizer {
public:
    PowerOptimizer();
    ~PowerOptimizer();

    // Optimize power
    bool optimize(::Synthesis::RTLIL::Design *design, const PowerReport &report);

    // Clock gating
    bool insertClockGating(::Synthesis::RTLIL::Design *design);

    // Operand isolation
    bool isolateOperands(::Synthesis::RTLIL::Design *design);

    // Toggle reduction
    bool reduceToggles(::Synthesis::RTLIL::Design *design);

private:
    // Helper methods
    bool identifyClockGatingOpportunities(::Synthesis::RTLIL::Design *design);
    bool identifyIsolationOpportunities(::Synthesis::RTLIL::Design *design);
};

/* ========== Main Power Function ========== */
PowerReport analyzePower(const ::Synthesis::RTLIL::Design &design,
                        const std::string &moduleName,
                        double clockFrequency = 100.0);

/// Parse SAIF toggle data from a string (public utility)
std::map<std::string, double> parseSAIF(const std::string &saif_content);

// Set power log callback
void set_power_log_callback(void (*cb)(const char *, const char *));

} // namespace PowerAnalysis

#endif /* POWER_ANALYZER_H */
