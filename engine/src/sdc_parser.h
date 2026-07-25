/**
 * SDC Parser - Industrial-grade SDC constraint parser
 * Based on OpenSTA sdc/
 *
 * Features:
 * - Complete SDC command support
 * - Clock definitions
 * - Input/Output delays
 * - Timing exceptions
 * - Case analysis
 * - False paths
 * - Multicycle paths
 * - Min/Max delays
 * - Clock groups
 * - Clock uncertainty
 * - Clock latency
 * - Transition time
 * - Capacitance
 * - Drive strength
 * - Input transition
 * - Max fanout
 * - Max capacitance
 * - Max transition
 * - Timing derate
 * - Disable timing
 * - Group path
 */

#ifndef SDC_PARSER_H
#define SDC_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>

namespace SDC {

// ============================================================================
// Forward declarations
// ============================================================================

struct SdcParser;
struct SdcCommand;
struct Clock;
struct TimingException;

// ============================================================================
// SDC Object types
// ============================================================================

enum class ObjectType {
    PORT,
    PIN,
    CELL,
    MODULE,
    CLOCK,
    NONE
};

// ============================================================================
// Time units
// ============================================================================

enum class TimeUnit {
    NS,
    PS,
    MS,
    US,
    S
};

// ============================================================================
// SDC Command types
// ============================================================================

enum class CommandType {
    // Clock commands
    CREATE_CLOCK,
    CREATE_GENERATED_CLOCK,

    // Delay commands
    SET_INPUT_DELAY,
    SET_OUTPUT_DELAY,
    SET_MAX_DELAY,
    SET_MIN_DELAY,
    SET_MAX_TIME_BOMB,
    SET_MIN_TIME_BOMB,

    // Timing exceptions
    SET_MULTICYCLE_PATH,
    SET_FALSE_PATH,
    SET_CLOCK_DOMAIN_CROSSING,
    SET_DISABLE_TIMING,
    SET_MIN_PULSE_WIDTH,

    // Clock properties
    SET_CLOCK_LATENCY,
    SET_CLOCK_UNCERTAINTY,
    SET_CLOCK_TRANSITION,
    SET_CLOCK_GROUPS,
    SET_CLOCK_SENSITIVITY,

    // Design rules
    SET_MAX_CAPACITANCE,
    SET_MIN_CAPACITANCE,
    SET_MAX_FANOUT,
    SET_MAX_TRANSITION,
    SET_MAX_AREA,
    SET_MAX_LEAKAGE_POWER,
    SET_MAX_DYNAMIC_POWER,
    SET_MAX_TOTAL_POWER,

    // Case analysis
    SET_CASE_ANALYSIS,

    // Don't touch
    SET_DONT_TOUCH,
    SET_DONT_TOUCH_NETWORK,
    SIZE_ONLY,

    // Timing derate
    SET_TIMING_DERATE,

    // Group path
    GROUP_PATH,

    // Wire load
    SET_WIRE_LOAD,
    SET_WIRE_LOAD_MODE,
    SET_WIRE_LOAD_MIN_BLOCK_SIZE,
    SET_WIRE_LOAD_MODEL,

    // Operating conditions
    SET_OPERATING_CONDITIONS,
    SET_MIN_LIBRARY,
    SET_MAX_LIBRARY,

    // Leakage optimization
    SET_LEAKAGE_OPTIMIZATION,

    // Critical range
    SET_CRITICAL_RANGE,

    // Fanout
    SET_FANOUT_LOAD,

    // Drive
    SET_DRIVE_CELL,
    SET_DRIVING_CELL,

    // Input transition
    SET_INPUT_TRANSITION,

    // Map
    SET_MAP,
    SET_MAP_DELETE,

    // Version
    CURRENT_VERSION,

    // Reset
    RESET_PATH,

    // No change
    SET_NOCHANGE,

    // Path delay
    SET_PATH_DELAY,

    // Noise
    SET_NOISE_MARGIN,
    SET_NOISE_GUARD_BAND,

    // Threshold
    SET_THRESHOLD_VOLTAGE_GROUP,

    // Power
    SET_POWER_ACTIVITY,
    SET_POWER_UP_VALUE,

    // Timing
    SET_TIMING_DELAY_TYPE
};

// ============================================================================
// SDC Command
// ============================================================================

struct SdcCommand {
    CommandType type;
    std::string target;
    std::string object_type;
    std::vector<std::string> objects;
    std::vector<std::string> arguments;
    std::vector<std::string> from_pins;
    std::vector<std::string> to_pins;
    std::vector<std::string> through_pins;
    double value;
    std::string clock_name;
    std::string reference_pin;
    bool rising;
    bool falling;
    bool both_edges;
    bool setup;
    bool hold;
    bool invert;
    int edge_index;
    int cycles;
    bool add;
    bool remove;

    SdcCommand() : type(CommandType::CREATE_CLOCK), value(0.0),
                   rising(true), falling(true), both_edges(false),
                   setup(true), hold(true), invert(false),
                   edge_index(0), cycles(1), add(true), remove(false) {}
};

// ============================================================================
// Clock
// ============================================================================

struct Clock {
    std::string name;
    std::vector<std::string> pins;
    double period;
    double start_time;
    double duty_cycle;
    bool is_generated;
    std::string source_pin;
    bool source_master;
    int divide_by;
    int multiply_by;
    bool invert;
    bool add_latency;
    std::vector<std::pair<bool, double>> edges;

    Clock() : period(0.0), start_time(0.0), duty_cycle(0.5),
              is_generated(false), source_master(false),
              divide_by(1), multiply_by(1), invert(false), add_latency(false) {}

    double waveform(bool rising) const {
        return rising ? start_time : start_time + period * duty_cycle;
    }
};

// ============================================================================
// Timing Exception
// ============================================================================

struct TimingException {
    enum Type {
        FALSE_PATH,
        MULTICYCLE_PATH,
        MAX_DELAY,
        MIN_DELAY,
        MAX_TIME_BOMB,
        MIN_TIME_BOMB,
        SET_NOCHANGE,
        RESET_PATH
    };

    Type type;
    std::vector<std::string> from_pins;
    std::vector<std::string> to_pins;
    std::vector<std::string> through_pins;
    std::string clock_name;
    bool rising;
    bool falling;
    int cycles;
    double delay_value;
    bool setup;
    bool hold;

    TimingException() : type(FALSE_PATH), rising(true), falling(true),
                        cycles(1), delay_value(0.0), setup(true), hold(true) {}
};

// ============================================================================
// SDC Parser
// ============================================================================

class SdcParser {
public:
    SdcParser();
    ~SdcParser();

    // Parse SDC file
    bool parse(const std::string &filename);
    bool parseString(const std::string &content);

    // Get results
    std::vector<SdcCommand> getCommands() const { return commands_; }
    std::vector<Clock> getClocks() const { return clocks_; }
    std::vector<TimingException> getExceptions() const { return exceptions_; }

    // Query
    Clock *findClock(const std::string &name);
    std::vector<SdcCommand> getCommandsByType(CommandType type) const;
    std::vector<TimingException> getExceptionsByType(TimingException::Type type) const;

    // Error handling
    bool hasError() const { return !error_message_.empty(); }
    std::string getErrorMessage() const { return error_message_; }
    int getErrorLine() const { return error_line_; }

    // Reset
    void reset();

private:
    // Commands
    std::vector<SdcCommand> commands_;
    std::vector<Clock> clocks_;
    std::vector<TimingException> exceptions_;

    // State
    std::string current_line_;
    int current_line_number_;
    size_t current_pos_;

    // Error handling
    std::string error_message_;
    int error_line_;

    // Internal methods
    void parseLine(const std::string &line);
    void parseCommand(const std::vector<std::string> &tokens);

    // Command parsers
    void parseCreateClock(const std::vector<std::string> &tokens);
    void parseCreateGeneratedClock(const std::vector<std::string> &tokens);
    void parseSetInputDelay(const std::vector<std::string> &tokens);
    void parseSetOutputDelay(const std::vector<std::string> &tokens);
    void parseSetMaxDelay(const std::vector<std::string> &tokens);
    void parseSetMinDelay(const std::vector<std::string> &tokens);
    void parseSetMulticyclePath(const std::vector<std::string> &tokens);
    void parseSetFalsePath(const std::vector<std::string> &tokens);
    void parseSetClockDomainCrossing(const std::vector<std::string> &tokens);
    void parseSetDisableTiming(const std::vector<std::string> &tokens);
    void parseSetClockLatency(const std::vector<std::string> &tokens);
    void parseSetClockUncertainty(const std::vector<std::string> &tokens);
    void parseSetClockTransition(const std::vector<std::string> &tokens);
    void parseSetClockGroups(const std::vector<std::string> &tokens);
    void parseSetMaxCapacitance(const std::vector<std::string> &tokens);
    void parseSetMinCapacitance(const std::vector<std::string> &tokens);
    void parseSetMaxFanout(const std::vector<std::string> &tokens);
    void parseSetMaxTransition(const std::vector<std::string> &tokens);
    void parseSetMaxArea(const std::vector<std::string> &tokens);
    void parseSetCaseAnalysis(const std::vector<std::string> &tokens);
    void parseSetDontTouch(const std::vector<std::string> &tokens);
    void parseSetTimingDerate(const std::vector<std::string> &tokens);
    void parseGroupPath(const std::vector<std::string> &tokens);
    void parseSetWireLoad(const std::vector<std::string> &tokens);
    void parseSetOperatingConditions(const std::vector<std::string> &tokens);
    void parseCurrentVersion(const std::vector<std::string> &tokens);

    // Utility methods
    std::vector<std::string> tokenize(const std::string &line);
    std::vector<std::string> parseObjectList(const std::vector<std::string> &tokens, size_t &pos);
    double parseTime(const std::string &str);
    double parseCapacitance(const std::string &str);
    double parseTransition(const std::string &str);
    std::string parseObjectName(const std::vector<std::string> &tokens, size_t &pos);
    std::vector<std::string> parsePinList(const std::vector<std::string> &tokens, size_t &pos);
    bool parseFlag(const std::vector<std::string> &tokens, size_t &pos, const std::string &flag);
    double parseValue(const std::vector<std::string> &tokens, size_t &pos);
    int parseInteger(const std::vector<std::string> &tokens, size_t &pos);

    void reportError(const std::string &message);
    void reportWarning(const std::string &message);
};

// ============================================================================
// SDC Writer
// ============================================================================

class SdcWriter {
public:
    SdcWriter();
    ~SdcWriter();

    // Write SDC file
    bool write(const std::string &filename, const std::vector<SdcCommand> &commands);
    bool writeString(std::string &content, const std::vector<SdcCommand> &commands);

    // Error handling
    bool hasError() const { return !error_message_.empty(); }
    std::string getErrorMessage() const { return error_message_; }

private:
    std::string error_message_;

    // Command writers
    std::string writeCreateClock(const SdcCommand &cmd);
    std::string writeCreateGeneratedClock(const SdcCommand &cmd);
    std::string writeSetInputDelay(const SdcCommand &cmd);
    std::string writeSetOutputDelay(const SdcCommand &cmd);
    std::string writeSetMaxDelay(const SdcCommand &cmd);
    std::string writeSetMinDelay(const SdcCommand &cmd);
    std::string writeSetMulticyclePath(const SdcCommand &cmd);
    std::string writeSetFalsePath(const SdcCommand &cmd);
    std::string writeSetClockDomainCrossing(const SdcCommand &cmd);
    std::string writeSetDisableTiming(const SdcCommand &cmd);
    std::string writeSetClockLatency(const SdcCommand &cmd);
    std::string writeSetClockUncertainty(const SdcCommand &cmd);
    std::string writeSetClockTransition(const SdcCommand &cmd);
    std::string writeSetClockGroups(const SdcCommand &cmd);
    std::string writeSetMaxCapacitance(const SdcCommand &cmd);
    std::string writeSetMinCapacitance(const SdcCommand &cmd);
    std::string writeSetMaxFanout(const SdcCommand &cmd);
    std::string writeSetMaxTransition(const SdcCommand &cmd);
    std::string writeSetMaxArea(const SdcCommand &cmd);
    std::string writeSetCaseAnalysis(const SdcCommand &cmd);
    std::string writeSetDontTouch(const SdcCommand &cmd);
    std::string writeSetTimingDerate(const SdcCommand &cmd);
    std::string writeGroupPath(const SdcCommand &cmd);

    // Utility methods
    std::string formatTime(double time);
    std::string formatCapacitance(double cap);
    std::string formatTransition(double trans);
    std::string formatObjectList(const std::vector<std::string> &objects);
};

// ============================================================================
// Helper functions
// ============================================================================

// Parse SDC file
SdcParser parseSdcFile(const std::string &filename);

// Write SDC file
bool writeSdcFile(const std::string &filename, const std::vector<SdcCommand> &commands);

// Apply SDC constraints to design
void applySdcConstraints(const std::vector<SdcCommand> &commands, void *design);

// Convert SDC commands to internal representation
void convertToInternal(const std::vector<SdcCommand> &commands, void *timing_engine);

} // namespace SDC

#endif // SDC_PARSER_H
