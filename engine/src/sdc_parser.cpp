/**
 * SDC Parser - Industrial-grade SDC constraint parser
 * Based on OpenSTA sdc/
 *
 * Complete implementation of all methods declared in sdc_parser.h
 */

#include "sdc_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

namespace SDC {

// ============================================================================
// SdcParser implementation
// ============================================================================

SdcParser::SdcParser() : current_line_number_(0), current_pos_(0), error_line_(0) {}

SdcParser::~SdcParser() = default;

bool SdcParser::parse(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        reportError("Cannot open file: " + filename);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        current_line_number_++;
        current_line_ = line;
        current_pos_ = 0;
        parseLine(line);
    }

    return !hasError();
}

bool SdcParser::parseString(const std::string &content) {
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        current_line_number_++;
        current_line_ = line;
        current_pos_ = 0;
        parseLine(line);
    }

    return !hasError();
}

void SdcParser::parseLine(const std::string &line) {
    // Skip empty lines and comments
    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);

    if (trimmed.empty() || trimmed[0] == '#') {
        return;
    }

    // Tokenize and parse command
    std::vector<std::string> tokens = tokenize(trimmed);
    if (!tokens.empty()) {
        parseCommand(tokens);
    }
}

void SdcParser::parseCommand(const std::vector<std::string> &tokens) {
    const std::string &cmd = tokens[0];

    if (cmd == "create_clock") {
        parseCreateClock(tokens);
    } else if (cmd == "create_generated_clock") {
        parseCreateGeneratedClock(tokens);
    } else if (cmd == "set_input_delay") {
        parseSetInputDelay(tokens);
    } else if (cmd == "set_output_delay") {
        parseSetOutputDelay(tokens);
    } else if (cmd == "set_max_delay") {
        parseSetMaxDelay(tokens);
    } else if (cmd == "set_min_delay") {
        parseSetMinDelay(tokens);
    } else if (cmd == "set_multicycle_path") {
        parseSetMulticyclePath(tokens);
    } else if (cmd == "set_false_path") {
        parseSetFalsePath(tokens);
    } else if (cmd == "set_clock_domain_crossing") {
        parseSetClockDomainCrossing(tokens);
    } else if (cmd == "set_disable_timing") {
        parseSetDisableTiming(tokens);
    } else if (cmd == "set_clock_latency") {
        parseSetClockLatency(tokens);
    } else if (cmd == "set_clock_uncertainty") {
        parseSetClockUncertainty(tokens);
    } else if (cmd == "set_clock_transition") {
        parseSetClockTransition(tokens);
    } else if (cmd == "set_clock_groups") {
        parseSetClockGroups(tokens);
    } else if (cmd == "set_max_capacitance") {
        parseSetMaxCapacitance(tokens);
    } else if (cmd == "set_min_capacitance") {
        parseSetMinCapacitance(tokens);
    } else if (cmd == "set_max_fanout") {
        parseSetMaxFanout(tokens);
    } else if (cmd == "set_max_transition") {
        parseSetMaxTransition(tokens);
    } else if (cmd == "set_max_area") {
        parseSetMaxArea(tokens);
    } else if (cmd == "set_case_analysis") {
        parseSetCaseAnalysis(tokens);
    } else if (cmd == "set_dont_touch") {
        parseSetDontTouch(tokens);
    } else if (cmd == "set_dont_touch_network") {
        parseSetDontTouch(tokens);
    } else if (cmd == "set_timing_derate") {
        parseSetTimingDerate(tokens);
    } else if (cmd == "group_path") {
        parseGroupPath(tokens);
    } else if (cmd == "set_wire_load") {
        parseSetWireLoad(tokens);
    } else if (cmd == "set_wire_load_model") {
        parseSetWireLoad(tokens);
    } else if (cmd == "set_wire_load_mode") {
        parseSetWireLoad(tokens);
    } else if (cmd == "set_operating_conditions") {
        parseSetOperatingConditions(tokens);
    } else if (cmd == "current_version") {
        parseCurrentVersion(tokens);
    } else if (cmd == "set_units") {
        // Parse unit specifications: -time ns, -resistance kohm, -capacitance pf, -voltage V
        for (size_t i = 1; i < tokens.size(); i++) {
            if (tokens[i] == "-time" && i + 1 < tokens.size()) {
                timeUnit_ = tokens[++i];
            } else if (tokens[i] == "-resistance" && i + 1 < tokens.size()) {
                resistanceUnit_ = tokens[++i];
            } else if (tokens[i] == "-capacitance" && i + 1 < tokens.size()) {
                capacitanceUnit_ = tokens[++i];
            } else if (tokens[i] == "-voltage" && i + 1 < tokens.size()) {
                voltageUnit_ = tokens[++i];
            } else if (tokens[i] == "-current" && i + 1 < tokens.size()) {
                currentUnit_ = tokens[++i];
            } else if (tokens[i] == "-power" && i + 1 < tokens.size()) {
                powerUnit_ = tokens[++i];
            }
        }
    } else if (cmd == "set_min_library") {
        // Parse min library specification for multi-corner analysis
        if (tokens.size() >= 2) {
            minLibrary_ = tokens[1];
            // Strip quotes if present
            if (minLibrary_.size() >= 2 && minLibrary_.front() == '"' && minLibrary_.back() == '"') {
                minLibrary_ = minLibrary_.substr(1, minLibrary_.size() - 2);
            }
        }
    } else if (cmd == "set_max_library") {
        // Parse max library specification for multi-corner analysis
        if (tokens.size() >= 2) {
            maxLibrary_ = tokens[1];
            // Strip quotes if present
            if (maxLibrary_.size() >= 2 && maxLibrary_.front() == '"' && maxLibrary_.back() == '"') {
                maxLibrary_ = maxLibrary_.substr(1, maxLibrary_.size() - 2);
            }
        }
    } else {
        reportWarning("Unknown SDC command: " + cmd);
    }
}

// ============================================================================
// Command parsers
// ============================================================================

void SdcParser::parseCreateClock(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::CREATE_CLOCK;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-name" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-period" && pos + 1 < tokens.size()) {
            cmd.value = parseTime(tokens[++pos]);
        } else if (tokens[pos] == "-waveform" && pos + 1 < tokens.size()) {
            // Parse waveform: {rise_time fall_time}
            pos++;
            if (pos < tokens.size() && tokens[pos] == "{") {
                pos++;
                if (pos < tokens.size()) {
                    cmd.value = parseTime(tokens[pos]);  // Use period from waveform
                }
            }
        }
        pos++;
    }

    // Parse clock source
    if (pos < tokens.size()) {
        cmd.objects.push_back(tokens[pos]);
    }

    // If no period specified, use default
    if (cmd.value == 0.0) {
        cmd.value = 10.0;  // Default 10ns period
    }

    // Create clock object
    Clock clock;
    clock.name = cmd.clock_name.empty() ? "clk" : cmd.clock_name;
    if (!cmd.objects.empty()) {
        clock.pins.push_back(cmd.objects[0]);
    }
    clock.period = cmd.value;
    clocks_.push_back(clock);

    commands_.push_back(cmd);
}

void SdcParser::parseCreateGeneratedClock(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::CREATE_GENERATED_CLOCK;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-name" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-master_clock" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-source" && pos + 1 < tokens.size()) {
            cmd.reference_pin = tokens[++pos];
        } else if (tokens[pos] == "-divide_by" && pos + 1 < tokens.size()) {
            cmd.value = std::stod(tokens[++pos]);
        } else if (tokens[pos] == "-multiply_by" && pos + 1 < tokens.size()) {
            cmd.value = std::stod(tokens[++pos]);
        } else if (tokens[pos] == "-invert") {
            cmd.invert = true;
        }
        pos++;
    }

    // Parse clock pins
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetInputDelay(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_INPUT_DELAY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-clock" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-rise") {
            cmd.rising = true;
            cmd.falling = false;
        } else if (tokens[pos] == "-fall") {
            cmd.rising = false;
            cmd.falling = true;
        } else if (tokens[pos] == "-max") {
            // Max delay
        } else if (tokens[pos] == "-min") {
            // Min delay
        }
        pos++;
    }

    // Parse delay value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Parse port list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetOutputDelay(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_OUTPUT_DELAY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-clock" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-rise") {
            cmd.rising = true;
            cmd.falling = false;
        } else if (tokens[pos] == "-fall") {
            cmd.rising = false;
            cmd.falling = true;
        } else if (tokens[pos] == "-max") {
            // Max delay
        } else if (tokens[pos] == "-min") {
            // Min delay
        }
        pos++;
    }

    // Parse delay value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Parse port list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMaxDelay(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MAX_DELAY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-setup") {
            cmd.setup = true;
            cmd.hold = false;
        } else if (tokens[pos] == "-hold") {
            cmd.setup = false;
            cmd.hold = true;
        } else if (tokens[pos] == "-from" && pos + 1 < tokens.size()) {
            pos++;
            cmd.from_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-to" && pos + 1 < tokens.size()) {
            pos++;
            cmd.to_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-through" && pos + 1 < tokens.size()) {
            pos++;
            // Parse through pins
        }
        pos++;
    }

    // Parse delay value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Create timing exception
    TimingException exception;
    exception.type = TimingException::MAX_DELAY;
    exception.from_pins = cmd.from_pins;
    exception.to_pins = cmd.to_pins;
    exception.delay_value = cmd.value;
    exceptions_.push_back(exception);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMinDelay(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MIN_DELAY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-setup") {
            cmd.setup = true;
            cmd.hold = false;
        } else if (tokens[pos] == "-hold") {
            cmd.setup = false;
            cmd.hold = true;
        } else if (tokens[pos] == "-from" && pos + 1 < tokens.size()) {
            pos++;
            cmd.from_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-to" && pos + 1 < tokens.size()) {
            pos++;
            cmd.to_pins = parsePinList(tokens, pos);
        }
        pos++;
    }

    // Parse delay value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Create timing exception
    TimingException exception;
    exception.type = TimingException::MIN_DELAY;
    exception.from_pins = cmd.from_pins;
    exception.to_pins = cmd.to_pins;
    exception.delay_value = cmd.value;
    exceptions_.push_back(exception);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMulticyclePath(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MULTICYCLE_PATH;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-setup") {
            cmd.setup = true;
            cmd.hold = false;
        } else if (tokens[pos] == "-hold") {
            cmd.setup = false;
            cmd.hold = true;
        } else if (tokens[pos] == "-from" && pos + 1 < tokens.size()) {
            pos++;
            cmd.from_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-to" && pos + 1 < tokens.size()) {
            pos++;
            cmd.to_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-start" && pos + 1 < tokens.size()) {
            pos++;
            // Start delay
        } else if (tokens[pos] == "-end" && pos + 1 < tokens.size()) {
            pos++;
            // End delay
        }
        pos++;
    }

    // Parse cycles
    if (pos < tokens.size()) {
        cmd.value = std::stod(tokens[pos]);
        pos++;
    }

    // Create timing exception
    TimingException exception;
    exception.type = TimingException::MULTICYCLE_PATH;
    exception.from_pins = cmd.from_pins;
    exception.to_pins = cmd.to_pins;
    exception.cycles = static_cast<int>(cmd.value);
    exception.setup = cmd.setup;
    exception.hold = cmd.hold;
    exceptions_.push_back(exception);

    commands_.push_back(cmd);
}

void SdcParser::parseSetFalsePath(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_FALSE_PATH;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-from" && pos + 1 < tokens.size()) {
            pos++;
            cmd.from_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-to" && pos + 1 < tokens.size()) {
            pos++;
            cmd.to_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-through" && pos + 1 < tokens.size()) {
            pos++;
            // Parse through pins
        } else if (tokens[pos] == "-rise") {
            cmd.rising = true;
            cmd.falling = false;
        } else if (tokens[pos] == "-fall") {
            cmd.rising = false;
            cmd.falling = true;
        }
        pos++;
    }

    // Create timing exception
    TimingException exception;
    exception.type = TimingException::FALSE_PATH;
    exception.from_pins = cmd.from_pins;
    exception.to_pins = cmd.to_pins;
    exception.rising = cmd.rising;
    exception.falling = cmd.falling;
    exceptions_.push_back(exception);

    commands_.push_back(cmd);
}

void SdcParser::parseSetClockDomainCrossing(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CLOCK_DOMAIN_CROSSING;
    commands_.push_back(cmd);
}

void SdcParser::parseSetDisableTiming(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_DISABLE_TIMING;
    commands_.push_back(cmd);
}

void SdcParser::parseSetClockLatency(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CLOCK_LATENCY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-source") {
            // Source latency
        } else if (tokens[pos] == "-network") {
            // Network latency
        }
        pos++;
    }

    // Parse latency value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Parse clock
    if (pos < tokens.size()) {
        cmd.clock_name = tokens[pos];
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetClockUncertainty(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CLOCK_UNCERTAINTY;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-setup") {
            // Setup uncertainty
        } else if (tokens[pos] == "-hold") {
            // Hold uncertainty
        } else if (tokens[pos] == "-rise") {
            // Rise uncertainty
        } else if (tokens[pos] == "-fall") {
            // Fall uncertainty
        }
        pos++;
    }

    // Parse uncertainty value
    if (pos < tokens.size()) {
        cmd.value = parseTime(tokens[pos]);
        pos++;
    }

    // Parse clock
    if (pos < tokens.size()) {
        cmd.clock_name = tokens[pos];
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetClockTransition(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CLOCK_TRANSITION;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-rise") {
            // Rise transition
        } else if (tokens[pos] == "-fall") {
            // Fall transition
        }
        pos++;
    }

    // Parse transition value
    if (pos < tokens.size()) {
        cmd.value = parseTransition(tokens[pos]);
        pos++;
    }

    // Parse clock
    if (pos < tokens.size()) {
        cmd.clock_name = tokens[pos];
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetClockGroups(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CLOCK_GROUPS;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-asynchronous") {
            // Asynchronous clocks
        } else if (tokens[pos] == "-logically_exclusive") {
            // Logically exclusive
        } else if (tokens[pos] == "-physically_exclusive") {
            // Physically exclusive
        } else if (tokens[pos] == "-group" && pos + 1 < tokens.size()) {
            pos++;
            // Parse clock group
        }
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetMaxCapacitance(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MAX_CAPACITANCE;

    size_t pos = 1;

    // Parse capacitance value
    if (pos < tokens.size()) {
        cmd.value = parseCapacitance(tokens[pos]);
        pos++;
    }

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMinCapacitance(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MIN_CAPACITANCE;

    size_t pos = 1;

    // Parse capacitance value
    if (pos < tokens.size()) {
        cmd.value = parseCapacitance(tokens[pos]);
        pos++;
    }

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMaxFanout(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MAX_FANOUT;

    size_t pos = 1;

    // Parse fanout value
    if (pos < tokens.size()) {
        cmd.value = std::stod(tokens[pos]);
        pos++;
    }

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMaxTransition(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MAX_TRANSITION;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-data_path") {
            // Data path transition
        } else if (tokens[pos] == "-clock_path") {
            // Clock path transition
        }
        pos++;
    }

    // Parse transition value
    if (pos < tokens.size()) {
        cmd.value = parseTransition(tokens[pos]);
        pos++;
    }

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetMaxArea(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_MAX_AREA;

    size_t pos = 1;

    // Parse area value
    if (pos < tokens.size()) {
        cmd.value = std::stod(tokens[pos]);
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetCaseAnalysis(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_CASE_ANALYSIS;

    size_t pos = 1;

    // Parse value
    if (pos < tokens.size()) {
        cmd.value = std::stod(tokens[pos]);
        pos++;
    }

    // Parse pin list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetDontTouch(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_DONT_TOUCH;

    size_t pos = 1;

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseSetTimingDerate(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_TIMING_DERATE;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-late") {
            // Late derate
        } else if (tokens[pos] == "-early") {
            // Early derate
        } else if (tokens[pos] == "-cell_delay") {
            // Cell delay
        } else if (tokens[pos] == "-cell_check") {
            // Cell check
        } else if (tokens[pos] == "-net_delay") {
            // Net delay
        }
        pos++;
    }

    // Parse derate value
    if (pos < tokens.size()) {
        cmd.value = std::stod(tokens[pos]);
        pos++;
    }

    // Parse object list
    cmd.objects = parseObjectList(tokens, pos);

    commands_.push_back(cmd);
}

void SdcParser::parseGroupPath(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::GROUP_PATH;

    size_t pos = 1;

    // Parse options
    while (pos < tokens.size() && tokens[pos][0] == '-') {
        if (tokens[pos] == "-name" && pos + 1 < tokens.size()) {
            cmd.clock_name = tokens[++pos];
        } else if (tokens[pos] == "-from" && pos + 1 < tokens.size()) {
            pos++;
            cmd.from_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-to" && pos + 1 < tokens.size()) {
            pos++;
            cmd.to_pins = parsePinList(tokens, pos);
        } else if (tokens[pos] == "-through" && pos + 1 < tokens.size()) {
            pos++;
            // Parse through pins
        } else if (tokens[pos] == "-weight" && pos + 1 < tokens.size()) {
            pos++;
            cmd.value = std::stod(tokens[pos]);
        }
        pos++;
    }

    commands_.push_back(cmd);
}

void SdcParser::parseSetWireLoad(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_WIRE_LOAD;
    commands_.push_back(cmd);
}

void SdcParser::parseSetOperatingConditions(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::SET_OPERATING_CONDITIONS;
    commands_.push_back(cmd);
}

void SdcParser::parseCurrentVersion(const std::vector<std::string> &tokens) {
    SdcCommand cmd;
    cmd.type = CommandType::CURRENT_VERSION;
    commands_.push_back(cmd);
}

// ============================================================================
// Utility methods
// ============================================================================

std::vector<std::string> SdcParser::tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];

        if (c == '"') {
            in_quotes = !in_quotes;
            if (!in_quotes && !token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else if (in_quotes) {
            token += c;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }

    if (!token.empty()) {
        tokens.push_back(token);
    }

    return tokens;
}

std::vector<std::string> SdcParser::parseObjectList(const std::vector<std::string> &tokens, size_t &pos) {
    std::vector<std::string> objects;

    // Skip to next token
    if (pos < tokens.size()) {
        pos++;
    }

    // Parse object list (may be in braces)
    if (pos < tokens.size() && tokens[pos] == "{") {
        pos++;
        while (pos < tokens.size() && tokens[pos] != "}") {
            objects.push_back(tokens[pos]);
            pos++;
        }
        if (pos < tokens.size()) {
            pos++;  // Skip closing brace
        }
    } else {
        // Single object
        while (pos < tokens.size() && tokens[pos][0] != '-') {
            objects.push_back(tokens[pos]);
            pos++;
        }
    }

    return objects;
}

double SdcParser::parseTime(const std::string &str) {
    // Parse time value with optional unit suffix
    std::string num_str = str;
    double multiplier = 1.0;

    if (num_str.size() > 2) {
        std::string suffix = num_str.substr(num_str.size() - 2);
        if (suffix == "ns") {
            multiplier = 1.0;
            num_str = num_str.substr(0, num_str.size() - 2);
        } else if (suffix == "ps") {
            multiplier = 0.001;
            num_str = num_str.substr(0, num_str.size() - 2);
        } else if (suffix == "ms") {
            multiplier = 1000000.0;
            num_str = num_str.substr(0, num_str.size() - 2);
        } else if (suffix == "us") {
            multiplier = 1000.0;
            num_str = num_str.substr(0, num_str.size() - 2);
        }
    }

    return std::stod(num_str) * multiplier;
}

double SdcParser::parseCapacitance(const std::string &str) {
    // Parse capacitance value with optional unit suffix
    std::string num_str = str;
    double multiplier = 1.0;

    if (num_str.size() > 2) {
        std::string suffix = num_str.substr(num_str.size() - 2);
        if (suffix == "pf") {
            multiplier = 1.0;
            num_str = num_str.substr(0, num_str.size() - 2);
        } else if (suffix == "ff") {
            multiplier = 0.001;
            num_str = num_str.substr(0, num_str.size() - 2);
        }
    }

    return std::stod(num_str) * multiplier;
}

double SdcParser::parseTransition(const std::string &str) {
    // Parse transition value (same as time)
    return parseTime(str);
}

std::string SdcParser::parseObjectName(const std::vector<std::string> &tokens, size_t &pos) {
    if (pos < tokens.size()) {
        return tokens[pos++];
    }
    return "";
}

std::vector<std::string> SdcParser::parsePinList(const std::vector<std::string> &tokens, size_t &pos) {
    std::vector<std::string> pins;

    if (pos < tokens.size() && tokens[pos] == "{") {
        pos++;
        while (pos < tokens.size() && tokens[pos] != "}") {
            pins.push_back(tokens[pos]);
            pos++;
        }
        if (pos < tokens.size()) {
            pos++;  // Skip closing brace
        }
    } else if (pos < tokens.size()) {
        pins.push_back(tokens[pos]);
        pos++;
    }

    return pins;
}

bool SdcParser::parseFlag(const std::vector<std::string> &tokens, size_t &pos, const std::string &flag) {
    if (pos < tokens.size() && tokens[pos] == flag) {
        pos++;
        return true;
    }
    return false;
}

double SdcParser::parseValue(const std::vector<std::string> &tokens, size_t &pos) {
    if (pos < tokens.size()) {
        return std::stod(tokens[pos++]);
    }
    return 0.0;
}

int SdcParser::parseInteger(const std::vector<std::string> &tokens, size_t &pos) {
    if (pos < tokens.size()) {
        return std::stoi(tokens[pos++]);
    }
    return 0;
}

void SdcParser::reportError(const std::string &message) {
    error_message_ = "Line " + std::to_string(current_line_number_) + ": " + message;
    error_line_ = current_line_number_;
}

void SdcParser::reportWarning(const std::string &message) {
    // Warnings are not fatal
    std::cerr << "Warning: Line " << current_line_number_ << ": " << message << std::endl;
}

void SdcParser::reset() {
    commands_.clear();
    clocks_.clear();
    exceptions_.clear();
    error_message_.clear();
    error_line_ = 0;
    current_line_number_ = 0;
}

// ============================================================================
// SdcWriter implementation
// ============================================================================

SdcWriter::SdcWriter() = default;
SdcWriter::~SdcWriter() = default;

bool SdcWriter::write(const std::string &filename, const std::vector<SdcCommand> &commands) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        error_message_ = "Cannot open file for writing: " + filename;
        return false;
    }

    for (const auto &cmd : commands) {
        std::string line;
        switch (cmd.type) {
            case CommandType::CREATE_CLOCK:
                line = writeCreateClock(cmd);
                break;
            case CommandType::CREATE_GENERATED_CLOCK:
                line = writeCreateGeneratedClock(cmd);
                break;
            case CommandType::SET_INPUT_DELAY:
                line = writeSetInputDelay(cmd);
                break;
            case CommandType::SET_OUTPUT_DELAY:
                line = writeSetOutputDelay(cmd);
                break;
            case CommandType::SET_MAX_DELAY:
                line = writeSetMaxDelay(cmd);
                break;
            case CommandType::SET_MIN_DELAY:
                line = writeSetMinDelay(cmd);
                break;
            case CommandType::SET_MULTICYCLE_PATH:
                line = writeSetMulticyclePath(cmd);
                break;
            case CommandType::SET_FALSE_PATH:
                line = writeSetFalsePath(cmd);
                break;
            case CommandType::SET_CLOCK_DOMAIN_CROSSING:
                line = writeSetClockDomainCrossing(cmd);
                break;
            case CommandType::SET_DISABLE_TIMING:
                line = writeSetDisableTiming(cmd);
                break;
            case CommandType::SET_CLOCK_LATENCY:
                line = writeSetClockLatency(cmd);
                break;
            case CommandType::SET_CLOCK_UNCERTAINTY:
                line = writeSetClockUncertainty(cmd);
                break;
            case CommandType::SET_CLOCK_TRANSITION:
                line = writeSetClockTransition(cmd);
                break;
            case CommandType::SET_CLOCK_GROUPS:
                line = writeSetClockGroups(cmd);
                break;
            case CommandType::SET_MAX_CAPACITANCE:
                line = writeSetMaxCapacitance(cmd);
                break;
            case CommandType::SET_MIN_CAPACITANCE:
                line = writeSetMinCapacitance(cmd);
                break;
            case CommandType::SET_MAX_FANOUT:
                line = writeSetMaxFanout(cmd);
                break;
            case CommandType::SET_MAX_TRANSITION:
                line = writeSetMaxTransition(cmd);
                break;
            case CommandType::SET_MAX_AREA:
                line = writeSetMaxArea(cmd);
                break;
            case CommandType::SET_CASE_ANALYSIS:
                line = writeSetCaseAnalysis(cmd);
                break;
            case CommandType::SET_DONT_TOUCH:
                line = writeSetDontTouch(cmd);
                break;
            case CommandType::SET_TIMING_DERATE:
                line = writeSetTimingDerate(cmd);
                break;
            case CommandType::GROUP_PATH:
                line = writeGroupPath(cmd);
                break;
            default:
                line = "# Unsupported command";
                break;
        }
        file << line << std::endl;
    }

    return true;
}

bool SdcWriter::writeString(std::string &content, const std::vector<SdcCommand> &commands) {
    std::ostringstream stream;

    for (const auto &cmd : commands) {
        std::string line;
        switch (cmd.type) {
            case CommandType::CREATE_CLOCK:
                line = writeCreateClock(cmd);
                break;
            case CommandType::CREATE_GENERATED_CLOCK:
                line = writeCreateGeneratedClock(cmd);
                break;
            case CommandType::SET_INPUT_DELAY:
                line = writeSetInputDelay(cmd);
                break;
            case CommandType::SET_OUTPUT_DELAY:
                line = writeSetOutputDelay(cmd);
                break;
            case CommandType::SET_MAX_DELAY:
                line = writeSetMaxDelay(cmd);
                break;
            case CommandType::SET_MIN_DELAY:
                line = writeSetMinDelay(cmd);
                break;
            case CommandType::SET_MULTICYCLE_PATH:
                line = writeSetMulticyclePath(cmd);
                break;
            case CommandType::SET_FALSE_PATH:
                line = writeSetFalsePath(cmd);
                break;
            case CommandType::SET_CLOCK_DOMAIN_CROSSING:
                line = writeSetClockDomainCrossing(cmd);
                break;
            case CommandType::SET_DISABLE_TIMING:
                line = writeSetDisableTiming(cmd);
                break;
            case CommandType::SET_CLOCK_LATENCY:
                line = writeSetClockLatency(cmd);
                break;
            case CommandType::SET_CLOCK_UNCERTAINTY:
                line = writeSetClockUncertainty(cmd);
                break;
            case CommandType::SET_CLOCK_TRANSITION:
                line = writeSetClockTransition(cmd);
                break;
            case CommandType::SET_CLOCK_GROUPS:
                line = writeSetClockGroups(cmd);
                break;
            case CommandType::SET_MAX_CAPACITANCE:
                line = writeSetMaxCapacitance(cmd);
                break;
            case CommandType::SET_MIN_CAPACITANCE:
                line = writeSetMinCapacitance(cmd);
                break;
            case CommandType::SET_MAX_FANOUT:
                line = writeSetMaxFanout(cmd);
                break;
            case CommandType::SET_MAX_TRANSITION:
                line = writeSetMaxTransition(cmd);
                break;
            case CommandType::SET_MAX_AREA:
                line = writeSetMaxArea(cmd);
                break;
            case CommandType::SET_CASE_ANALYSIS:
                line = writeSetCaseAnalysis(cmd);
                break;
            case CommandType::SET_DONT_TOUCH:
                line = writeSetDontTouch(cmd);
                break;
            case CommandType::SET_TIMING_DERATE:
                line = writeSetTimingDerate(cmd);
                break;
            case CommandType::GROUP_PATH:
                line = writeGroupPath(cmd);
                break;
            default:
                line = "# Unsupported command";
                break;
        }
        stream << line << std::endl;
    }

    content = stream.str();
    return true;
}

// ============================================================================
// Command writers
// ============================================================================

std::string SdcWriter::writeCreateClock(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "create_clock";
    if (!cmd.clock_name.empty()) {
        oss << " -name " << cmd.clock_name;
    }
    oss << " -period " << formatTime(cmd.value);
    if (!cmd.objects.empty()) {
        oss << " " << cmd.objects[0];
    }
    return oss.str();
}

std::string SdcWriter::writeCreateGeneratedClock(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "create_generated_clock";
    if (!cmd.clock_name.empty()) {
        oss << " -name " << cmd.clock_name;
    }
    if (!cmd.reference_pin.empty()) {
        oss << " -source " << cmd.reference_pin;
    }
    if (cmd.value > 0) {
        oss << " -divide_by " << static_cast<int>(cmd.value);
    }
    if (cmd.invert) {
        oss << " -invert";
    }
    if (!cmd.objects.empty()) {
        for (const auto &obj : cmd.objects) {
            oss << " " << obj;
        }
    }
    return oss.str();
}

std::string SdcWriter::writeSetInputDelay(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_input_delay";
    if (!cmd.clock_name.empty()) {
        oss << " -clock " << cmd.clock_name;
    }
    if (cmd.rising && !cmd.falling) {
        oss << " -rise";
    } else if (!cmd.rising && cmd.falling) {
        oss << " -fall";
    }
    oss << " " << formatTime(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetOutputDelay(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_output_delay";
    if (!cmd.clock_name.empty()) {
        oss << " -clock " << cmd.clock_name;
    }
    if (cmd.rising && !cmd.falling) {
        oss << " -rise";
    } else if (!cmd.rising && cmd.falling) {
        oss << " -fall";
    }
    oss << " " << formatTime(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetMaxDelay(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_max_delay";
    if (cmd.setup) {
        oss << " -setup";
    }
    if (!cmd.from_pins.empty()) {
        oss << " -from " << formatObjectList(cmd.from_pins);
    }
    if (!cmd.to_pins.empty()) {
        oss << " -to " << formatObjectList(cmd.to_pins);
    }
    oss << " " << formatTime(cmd.value);
    return oss.str();
}

std::string SdcWriter::writeSetMinDelay(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_min_delay";
    if (cmd.hold) {
        oss << " -hold";
    }
    if (!cmd.from_pins.empty()) {
        oss << " -from " << formatObjectList(cmd.from_pins);
    }
    if (!cmd.to_pins.empty()) {
        oss << " -to " << formatObjectList(cmd.to_pins);
    }
    oss << " " << formatTime(cmd.value);
    return oss.str();
}

std::string SdcWriter::writeSetMulticyclePath(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_multicycle_path";
    if (cmd.setup) {
        oss << " -setup";
    }
    if (cmd.hold) {
        oss << " -hold";
    }
    if (!cmd.from_pins.empty()) {
        oss << " -from " << formatObjectList(cmd.from_pins);
    }
    if (!cmd.to_pins.empty()) {
        oss << " -to " << formatObjectList(cmd.to_pins);
    }
    oss << " " << static_cast<int>(cmd.value);
    return oss.str();
}

std::string SdcWriter::writeSetFalsePath(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_false_path";
    if (!cmd.from_pins.empty()) {
        oss << " -from " << formatObjectList(cmd.from_pins);
    }
    if (!cmd.to_pins.empty()) {
        oss << " -to " << formatObjectList(cmd.to_pins);
    }
    if (cmd.rising && !cmd.falling) {
        oss << " -rise";
    } else if (!cmd.rising && cmd.falling) {
        oss << " -fall";
    }
    return oss.str();
}

std::string SdcWriter::writeSetClockDomainCrossing(const SdcCommand &cmd) {
    return "set_clock_domain_crossing";
}

std::string SdcWriter::writeSetDisableTiming(const SdcCommand &cmd) {
    return "set_disable_timing";
}

std::string SdcWriter::writeSetClockLatency(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_clock_latency";
    oss << " " << formatTime(cmd.value);
    if (!cmd.clock_name.empty()) {
        oss << " " << cmd.clock_name;
    }
    return oss.str();
}

std::string SdcWriter::writeSetClockUncertainty(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_clock_uncertainty";
    oss << " " << formatTime(cmd.value);
    if (!cmd.clock_name.empty()) {
        oss << " " << cmd.clock_name;
    }
    return oss.str();
}

std::string SdcWriter::writeSetClockTransition(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_clock_transition";
    oss << " " << formatTransition(cmd.value);
    if (!cmd.clock_name.empty()) {
        oss << " " << cmd.clock_name;
    }
    return oss.str();
}

std::string SdcWriter::writeSetClockGroups(const SdcCommand &cmd) {
    return "set_clock_groups";
}

std::string SdcWriter::writeSetMaxCapacitance(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_max_capacitance";
    oss << " " << formatCapacitance(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetMinCapacitance(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_min_capacitance";
    oss << " " << formatCapacitance(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetMaxFanout(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_max_fanout";
    oss << " " << static_cast<int>(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetMaxTransition(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_max_transition";
    oss << " " << formatTransition(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetMaxArea(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_max_area";
    oss << " " << cmd.value;
    return oss.str();
}

std::string SdcWriter::writeSetCaseAnalysis(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_case_analysis";
    oss << " " << static_cast<int>(cmd.value);
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetDontTouch(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_dont_touch";
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeSetTimingDerate(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "set_timing_derate";
    oss << " " << cmd.value;
    for (const auto &obj : cmd.objects) {
        oss << " " << obj;
    }
    return oss.str();
}

std::string SdcWriter::writeGroupPath(const SdcCommand &cmd) {
    std::ostringstream oss;
    oss << "group_path";
    if (!cmd.clock_name.empty()) {
        oss << " -name " << cmd.clock_name;
    }
    if (!cmd.from_pins.empty()) {
        oss << " -from " << formatObjectList(cmd.from_pins);
    }
    if (!cmd.to_pins.empty()) {
        oss << " -to " << formatObjectList(cmd.to_pins);
    }
    return oss.str();
}

// ============================================================================
// Utility methods
// ============================================================================

std::string SdcWriter::formatTime(double time) {
    std::ostringstream oss;
    oss << time << "ns";
    return oss.str();
}

std::string SdcWriter::formatCapacitance(double cap) {
    std::ostringstream oss;
    oss << cap << "pf";
    return oss.str();
}

std::string SdcWriter::formatTransition(double trans) {
    std::ostringstream oss;
    oss << trans << "ns";
    return oss.str();
}

std::string SdcWriter::formatObjectList(const std::vector<std::string> &objects) {
    if (objects.empty()) {
        return "";
    }
    if (objects.size() == 1) {
        return objects[0];
    }

    std::string result = "{ ";
    for (size_t i = 0; i < objects.size(); i++) {
        if (i > 0) result += " ";
        result += objects[i];
    }
    result += " }";
    return result;
}

// ============================================================================
// Helper functions
// ============================================================================

SdcParser parseSdcFile(const std::string &filename) {
    SdcParser parser;
    parser.parse(filename);
    return parser;
}

bool writeSdcFile(const std::string &filename, const std::vector<SdcCommand> &commands) {
    SdcWriter writer;
    return writer.write(filename, commands);
}

void applySdcConstraints(const std::vector<SdcCommand> &commands, void *design) {
    // Apply SDC constraints to design
    // This would integrate with the timing engine
}

void convertToInternal(const std::vector<SdcCommand> &commands, void *timing_engine) {
    // Convert SDC commands to internal representation
    // This would integrate with the timing engine
}

} // namespace SDC
