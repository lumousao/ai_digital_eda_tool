/**
 * Liberty (.lib) Parser — Full NLDM-compatible parser for standard cell libraries
 *
 * Parses Cadence Liberate-generated .lib files with:
 * - Library header (PVT, units, templates)
 * - Cell definitions (area, leakage, pins, timing arcs, power arcs)
 * - NLDM 7x7 lookup tables (delay, transition, constraint, power)
 * - Bilinear interpolation for table lookups
 *
 * This is the foundation for realistic synthesis, STA, and power analysis.
 */
#ifndef LIBERTY_PARSER_H
#define LIBERTY_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>

namespace Liberty {

/* =========================================================================
 * NLDM Table Data
 * ========================================================================= */

struct LibertyTable {
    std::string template_name;
    std::vector<double> index_1;   // input transition (ns)
    std::vector<double> index_2;   // output load capacitance (pF)
    std::vector<std::vector<double>> values;  // rows × cols

    bool empty() const { return values.empty(); }

    /** Bilinear interpolation: given (x, y), interpolate in the 2D table */
    double interpolate(double x, double y) const {
        if (values.empty()) return 0.0;
        int rows = (int)index_1.size();
        int cols = (int)index_2.size();
        if (rows < 2 || cols < 2) {
            // Single-point fallback: return nearest value
            return values[0][0];
        }

        // Clamp input values to index range
        x = std::max(index_1[0], std::min(x, index_1[rows - 1]));
        y = std::max(index_2[0], std::min(y, index_2[cols - 1]));

        // Find bracketing indices for x (index_1)
        int i1 = 0, i2 = rows - 1;
        for (int i = 0; i < rows - 1; i++) {
            if (x >= index_1[i] && x <= index_1[i + 1]) {
                i1 = i; i2 = i + 1; break;
            }
        }

        // Find bracketing indices for y (index_2)
        int j1 = 0, j2 = cols - 1;
        for (int j = 0; j < cols - 1; j++) {
            if (y >= index_2[j] && y <= index_2[j + 1]) {
                j1 = j; j2 = j + 1; break;
            }
        }

        double x1 = index_1[i1], x2 = index_1[i2];
        double y1 = index_2[j1], y2 = index_2[j2];

        double dx = (x2 - x1 > 0) ? (x - x1) / (x2 - x1) : 0.0;
        double dy = (y2 - y1 > 0) ? (y - y1) / (y2 - y1) : 0.0;

        double v11 = values[i1][j1], v12 = values[i1][j2];
        double v21 = values[i2][j1], v22 = values[i2][j2];

        // Bilinear interpolation
        double v1 = v11 + (v21 - v11) * dx;
        double v2 = v12 + (v22 - v12) * dx;
        return v1 + (v2 - v1) * dy;
    }
};

/* =========================================================================
 * Timing Arc
 * ========================================================================= */

struct LibertyTimingArc {
    std::string related_pin;
    std::string timing_sense;   // positive_unate, negative_unate, non_unate
    std::string timing_type;    // combinational, rising_edge, falling_edge, setup_rising, hold_rising, etc.
    std::string when;           // condition, e.g. "B" or "!A"

    LibertyTable cell_rise;
    LibertyTable cell_fall;
    LibertyTable rise_transition;
    LibertyTable fall_transition;
    LibertyTable rise_constraint;   // for setup/hold
    LibertyTable fall_constraint;   // for setup/hold

    bool is_setup() const { return timing_type.find("setup") != std::string::npos; }
    bool is_hold() const { return timing_type.find("hold") != std::string::npos; }
    bool is_combinational() const {
        return timing_type == "combinational" || timing_type.empty();
    }
};

/* =========================================================================
 * Power Arc
 * ========================================================================= */

struct LibertyPowerArc {
    std::string related_pin;
    std::string when;
    LibertyTable rise_power;
    LibertyTable fall_power;
};

/* =========================================================================
 * Pin
 * ========================================================================= */

struct LibertyPin {
    std::string name;
    std::string direction;       // input, output, inout, internal
    std::string function;        // logic function, e.g. "(A * B)"
    double capacitance;          // input pin capacitance (pF)
    double max_capacitance;      // max output load (pF)
    double max_transition;       // max output transition (ns)
    double rise_capacitance;
    double fall_capacitance;

    std::vector<LibertyTimingArc> timing_arcs;
    std::vector<LibertyPowerArc> power_arcs;

    LibertyPin() : capacitance(0.0), max_capacitance(0.0), max_transition(0.0),
                   rise_capacitance(0.0), fall_capacitance(0.0) {}

    bool is_input() const { return direction == "input"; }
    bool is_output() const { return direction == "output"; }
};

/* =========================================================================
 * Cell
 * ========================================================================= */

struct LibertyCell {
    std::string name;
    std::string footprint;
    double area;                   // area in library units
    double cell_leakage_power;     // default leakage in library units (nW)
    bool is_sequential;
    bool dont_use;
    bool dont_touch;

    std::map<std::string, LibertyPin> pins;

    // State-dependent leakage
    std::map<std::string, double> state_leakages; // "!A&B" → leakage_value

    LibertyCell() : area(0.0), cell_leakage_power(0.0), is_sequential(false),
                    dont_use(false), dont_touch(false) {}

    const LibertyPin* find_output_pin() const {
        for (auto &[name, pin] : pins) {
            if (pin.is_output()) return &pin;
        }
        return nullptr;
    }

    const LibertyPin* find_input_pin(const std::string &pname) const {
        auto it = pins.find(pname);
        if (it != pins.end() && it->second.is_input()) return &it->second;
        return nullptr;
    }

    /** Get average input pin capacitance (fF) for load calculation */
    double avg_input_cap() const {
        double total = 0.0;
        int count = 0;
        for (auto &[name, pin] : pins) {
            if (pin.is_input()) { total += pin.capacitance; count++; }
        }
        return count > 0 ? total / count : 0.001;
    }

    /** Get setup time from liberty timing arcs */
    double get_setup_time(double input_slew_ns = 0.05, double clk_slew_ns = 0.05) const {
        for (auto &[name, pin] : pins) {
            for (auto &arc : pin.timing_arcs) {
                if (arc.is_setup()) {
                    // Use the larger of rise_constraint and fall_constraint
                    double r = arc.rise_constraint.empty() ? 0.0 : arc.rise_constraint.interpolate(input_slew_ns, clk_slew_ns);
                    double f = arc.fall_constraint.empty() ? 0.0 : arc.fall_constraint.interpolate(input_slew_ns, clk_slew_ns);
                    double val = std::max(std::abs(r), std::abs(f));
                    return val > 0 ? val : 0.05; // positive means setup time
                }
            }
        }
        return 0.05; // fallback
    }

    /** Get hold time from liberty timing arcs */
    double get_hold_time(double input_slew_ns = 0.05, double clk_slew_ns = 0.05) const {
        for (auto &[name, pin] : pins) {
            for (auto &arc : pin.timing_arcs) {
                if (arc.is_hold()) {
                    double r = arc.rise_constraint.empty() ? 0.0 : arc.rise_constraint.interpolate(input_slew_ns, clk_slew_ns);
                    double f = arc.fall_constraint.empty() ? 0.0 : arc.fall_constraint.interpolate(input_slew_ns, clk_slew_ns);
                    // Hold values are typically negative in liberty (data can arrive after clock edge)
                    // Absolute value is the required hold time
                    double val = std::max(std::abs(r), std::abs(f));
                    return val > 0 ? val : 0.05;
                }
            }
        }
        return 0.05; // fallback
    }

    /** Match cell function against a simplified boolean expression */
    bool matches_function(const std::string &func) const {
        const LibertyPin *out = find_output_pin();
        if (!out || out->function.empty()) return false;
        std::string f = simplify_func(out->function);
        std::string target = simplify_func(func);
        return f == target;
    }

private:
public: // temporarily public for debugging
    static std::string simplify_func(const std::string &s) {
        // Remove whitespace, parens, and normalize operators
        std::string r;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '(' || c == ')') continue;
            // Liberty source data commonly uses '&'/'|' while the native
            // RTL mapper uses '*'/'+'.  Canonicalize both spellings before
            // comparison so imported foundry libraries retain their native
            // Boolean functions without leaving generic cells unmapped.
            if (c == '&') r += '*';
            else if (c == '|') r += '+';
            else r += c;
        }
        return r;
    }
};

/* =========================================================================
 * Library
 * ========================================================================= */

struct LibertyLibrary {
    std::string name;
    std::string filename;
    double nom_voltage;
    double nom_temperature;
    double nom_process;
    double time_unit;              // scale factor for time values
    double voltage_unit;
    double capacitive_load_unit;   // scale factor for capacitance
    double leakage_power_unit;     // scale factor for leakage
    double current_unit;
    double default_max_transition;
    double default_fanout_load;
    double default_output_pin_cap;
    double slew_derate_from_library;
    double slew_lower_threshold_pct_fall;
    double slew_upper_threshold_pct_fall;
    double slew_lower_threshold_pct_rise;
    double slew_upper_threshold_pct_rise;
    double input_threshold_pct_fall;
    double input_threshold_pct_rise;
    double output_threshold_pct_fall;
    double output_threshold_pct_rise;

    std::map<std::string, LibertyCell> cells;
    std::vector<std::string> cell_names;  // ordered for iteration

    LibertyLibrary() : nom_voltage(1.2), nom_temperature(25.0), nom_process(1.0),
        time_unit(1.0), voltage_unit(1.0), capacitive_load_unit(1.0),
        leakage_power_unit(1.0), current_unit(1.0),
        default_max_transition(0.8), default_fanout_load(1.0),
        default_output_pin_cap(0.0), slew_derate_from_library(0.5),
        slew_lower_threshold_pct_fall(30.0), slew_upper_threshold_pct_fall(70.0),
        slew_lower_threshold_pct_rise(30.0), slew_upper_threshold_pct_rise(70.0),
        input_threshold_pct_fall(50.0), input_threshold_pct_rise(50.0),
        output_threshold_pct_fall(50.0), output_threshold_pct_rise(50.0) {}

    bool load(const std::string &filename);

    const LibertyCell* find_cell(const std::string &name) const {
        auto it = cells.find(name);
        return (it != cells.end()) ? &it->second : nullptr;
    }

    /** Find cells matching a given function template */
    std::vector<const LibertyCell*> find_cells_by_function(const std::string &func) const {
        std::vector<const LibertyCell*> result;
        for (auto &[name, cell] : cells) {
            if (cell.matches_function(func)) result.push_back(&cell);
        }
        return result;
    }

    /** Find the best matching cell for a logic function and fanout */
    const LibertyCell* find_best_cell(const std::string &func, int fanout) const {
        auto matches = find_cells_by_function(func);
        if (matches.empty()) return nullptr;

        // Prefer cells without dont_use
        std::vector<const LibertyCell*> usable;
        for (auto *c : matches) {
            if (!c->dont_use) usable.push_back(c);
        }
        if (usable.empty()) usable = matches;

        // Select drive strength based on fanout
        const LibertyCell *best = usable[0];
        double best_area = best->area;

        double target_area = 1.0;
        if (fanout <= 2) target_area = 1.0;
        else if (fanout <= 4) target_area = 2.0;
        else if (fanout <= 8) target_area = 4.0;
        else target_area = 8.0;

        for (auto *c : usable) {
            double diff = std::abs(c->area - target_area);
            double best_diff = std::abs(best_area - target_area);
            if (diff < best_diff) {
                best = c;
                best_area = c->area;
            }
        }
        return best;
    }

    /** Get total input capacitance for a cell (sum of input pin caps) */
    double cell_input_cap(const std::string &cell_name) const {
        const LibertyCell *c = find_cell(cell_name);
        if (!c) return 0.001; // default 1fF
        double total = 0.0;
        for (auto &[name, pin] : c->pins) {
            if (pin.is_input()) total += pin.capacitance;
        }
        return total > 0 ? total : 0.001;
    }

    /** Get output pin capacitance for load calculation */
    double cell_output_cap(const std::string &cell_name) const {
        const LibertyCell *c = find_cell(cell_name);
        if (!c) return 0.0;
        return c->find_output_pin() ? c->find_output_pin()->capacitance : 0.0;
    }

    /** Get timing arc for a specific input→output path */
    const LibertyTimingArc* get_timing_arc(const std::string &cell_name,
                                            const std::string &input_pin,
                                            const std::string &output_pin) const {
        const LibertyCell *c = find_cell(cell_name);
        if (!c) return nullptr;
        auto pin_it = c->pins.find(output_pin);
        if (pin_it == c->pins.end()) return nullptr;
        for (auto &arc : pin_it->second.timing_arcs) {
            if (arc.related_pin == input_pin && arc.is_combinational()) {
                return &arc;
            }
        }
        return nullptr;
    }

    /** Get setup/hold constraint arc for a sequential cell */
    const LibertyTimingArc* get_setup_arc(const std::string &cell_name) const {
        const LibertyCell *c = find_cell(cell_name);
        if (!c) return nullptr;
        for (auto &[name, pin] : c->pins) {
            for (auto &arc : pin.timing_arcs) {
                if (arc.is_setup()) return &arc;
            }
        }
        return nullptr;
    }

    const LibertyTimingArc* get_hold_arc(const std::string &cell_name) const {
        const LibertyCell *c = find_cell(cell_name);
        if (!c) return nullptr;
        for (auto &[name, pin] : c->pins) {
            for (auto &arc : pin.timing_arcs) {
                if (arc.is_hold()) return &arc;
            }
        }
        return nullptr;
    }

    /** Compute cell delay given input_slew and output_load via NLDM interpolation */
    double compute_cell_delay(const std::string &cell_name,
                              const std::string &input_pin,
                              const std::string &output_pin,
                              double input_slew_ns, double output_load_pf,
                              bool rising_output) const {
        const LibertyTimingArc *arc = get_timing_arc(cell_name, input_pin, output_pin);
        if (!arc) return estimate_fallback_delay(cell_name);

        const LibertyTable &tbl = rising_output ? arc->cell_rise : arc->cell_fall;
        if (tbl.empty()) return estimate_fallback_delay(cell_name);

        return tbl.interpolate(input_slew_ns, output_load_pf);
    }

    /** Compute output transition (slew) */
    double compute_output_slew(const std::string &cell_name,
                               const std::string &input_pin,
                               const std::string &output_pin,
                               double input_slew_ns, double output_load_pf,
                               bool rising_output) const {
        const LibertyTimingArc *arc = get_timing_arc(cell_name, input_pin, output_pin);
        if (!arc) return input_slew_ns * 1.5; // rough estimate

        const LibertyTable &tbl = rising_output ? arc->rise_transition : arc->fall_transition;
        if (tbl.empty()) return input_slew_ns * 1.5;

        return tbl.interpolate(input_slew_ns, output_load_pf);
    }

    /** Get cell leakage power */
    double get_cell_leakage(const std::string &cell_name) const {
        const LibertyCell *c = find_cell(cell_name);
        return c ? c->cell_leakage_power : 0.0;
    }

private:
    double estimate_fallback_delay(const std::string &cell_name) const {
        if (cell_name.find("DFF") != std::string::npos) return 0.150;
        if (cell_name.find("NAND") != std::string::npos) return 0.030;
        if (cell_name.find("NOR") != std::string::npos) return 0.030;
        if (cell_name.find("XOR") != std::string::npos) return 0.080;
        if (cell_name.find("AND") != std::string::npos) return 0.050;
        if (cell_name.find("OR") != std::string::npos) return 0.050;
        if (cell_name.find("NOT") != std::string::npos || cell_name.find("INV") != std::string::npos) return 0.020;
        if (cell_name.find("MUX") != std::string::npos) return 0.060;
        if (cell_name.find("BUF") != std::string::npos) return 0.010;
        if (cell_name.find("ADDF") != std::string::npos) return 0.120;
        if (cell_name.find("ADDH") != std::string::npos) return 0.080;
        return 0.050;
    }
};

} // namespace Liberty

#endif // LIBERTY_PARSER_H
