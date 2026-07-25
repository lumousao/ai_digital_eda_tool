/**
 * Synthesis Wrapper - C API implementation
 *
 * Calls synthesis library directly instead of spawning an external process.
 */

#include "synth_wrapper.h"

// Core synthesis engine headers
#include "synth_core.h"
#include "rtlil.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

static bool g_initialized = false;

void synth_wrapper_init(void) {
    if (g_initialized) return;
    SYNTH_NAMESPACE::synth_setup();
    g_initialized = true;
}

void synth_wrapper_shutdown(void) {
    if (!g_initialized) return;
    SYNTH_NAMESPACE::synth_shutdown();
    g_initialized = false;
}

int synth_wrapper_run(const char *script) {
    if (!g_initialized) return -1;
    try {
        std::string cmd(script);
        std::istringstream stream(cmd);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            size_t end = line.find_last_not_of(" \t");
            if (end != std::string::npos) line = line.substr(0, end + 1);
            if (!line.empty()) {
                SYNTH_NAMESPACE::run_pass(line);
            }
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

int synth_wrapper_read_verilog(const char *filename) {
    if (!g_initialized) return -1;
    try {
        std::string cmd = std::string("read_verilog ") + filename;
        SYNTH_NAMESPACE::run_pass(cmd);
        return 0;
    } catch (...) {
        return -1;
    }
}

int synth_wrapper_synth(const char *top_module) {
    if (!g_initialized) return -1;
    try {
        std::string cmd = "synth";
        if (top_module && top_module[0] != '\0') {
            cmd += std::string(" -top ") + top_module;
        }
        SYNTH_NAMESPACE::run_pass(cmd);
        return 0;
    } catch (...) {
        return -1;
    }
}

char *synth_wrapper_synth_and_stat(const char *top_module) {
    if (!g_initialized) {
        const char *err = "Synthesis engine not initialized";
        char *r = (char *)malloc(strlen(err) + 1);
        strcpy(r, err);
        return r;
    }
    try {
        // Run synthesis
        std::string cmd = "synth";
        if (top_module && top_module[0] != '\0') {
            cmd += std::string(" -top ") + top_module;
        }
        SYNTH_NAMESPACE::run_pass(cmd);

        // Run stat
        SYNTH_NAMESPACE::run_pass("stat");

        // Get the current design stats
        SYNTH_NAMESPACE::RTLIL::Design *design = SYNTH_NAMESPACE::synth_get_design();
        if (!design) {
            const char *err = "No design loaded";
            char *r = (char *)malloc(strlen(err) + 1);
            strcpy(r, err);
            return r;
        }

        // Build a simple report from the design
        std::stringstream ss;
        ss << "=== Synthesis Complete ===\n";

        int mod_count = 0;
        for (auto mod : design->modules()) {
            mod_count++;
            ss << "\nModule: " << SYNTH_NAMESPACE::RTLIL::id2cstr(mod->name) << "\n";
            ss << "  Wires: " << mod->wires.size() << "\n";
            ss << "  Cells: " << mod->cells.size() << "\n";

            // Count cell types
            std::map<std::string, int> cell_counts;
            for (auto cell : mod->cells()) {
                std::string type = SYNTH_NAMESPACE::RTLIL::id2cstr(cell->type);
                cell_counts[type]++;
            }

            for (auto &[type, count] : cell_counts) {
                ss << "  " << type << ": " << count << "\n";
            }
        }

        std::string result = ss.str();
        char *r = (char *)malloc(result.size() + 1);
        memcpy(r, result.c_str(), result.size() + 1);
        return r;
    } catch (const std::exception &e) {
        const char *err = e.what();
        char *r = (char *)malloc(strlen(err) + 1);
        strcpy(r, err);
        return r;
    } catch (...) {
        const char *err = "Unknown error during synthesis";
        char *r = (char *)malloc(strlen(err) + 1);
        strcpy(r, err);
        return r;
    }
}

char *synth_wrapper_get_output(void) {
    const char *msg = "Use synth_and_stat for results";
    char *r = (char *)malloc(strlen(msg) + 1);
    strcpy(r, msg);
    return r;
}

int synth_wrapper_write_verilog(const char *filename) {
    if (!g_initialized) return -1;
    try {
        std::string cmd = std::string("write_verilog ") + filename;
        SYNTH_NAMESPACE::run_pass(cmd);
        return 0;
    } catch (...) {
        return -1;
    }
}

void synth_wrapper_free(char *str) {
    if (str) free(str);
}
