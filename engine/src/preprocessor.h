/**
 * Verilog Preprocessor
 *
 * Implements `define, `ifdef, `ifndef, `elsif, `else, `endif, `include, `undef
 * Macro expansion with substitution.
 *
 * References:
 * - IEEE 1364-2005 Section 19
 * - industry-standard preprocessor
 */

#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <string>
#include <map>
#include <vector>
#include <set>

namespace VerilogParser {

struct MacroDefinition {
    std::string name;
    std::string value;
    std::vector<std::string> args; // empty for simple macros
    bool has_args;

    MacroDefinition() : has_args(false) {}
    MacroDefinition(const std::string &n, const std::string &v)
        : name(n), value(v), has_args(false) {}
};

class Preprocessor {
public:
    Preprocessor();

    // Run full preprocessing on source code
    std::string process(const std::string &source, const std::string &filename = "<input>");

    // Add a built-in define (e.g., __ICARUS__, SYNTHESIS)
    void define(const std::string &name, const std::string &value);
    void undef(const std::string &name);

    // Include path management
    void addIncludePath(const std::string &path) { include_paths_.push_back(path); }
    void clearIncludePaths() { include_paths_.clear(); }

private:
    std::map<std::string, MacroDefinition> defines_;
    std::set<std::string> included_files_; // prevent recursive include
    std::string current_file_;
    int current_line_;
    std::vector<std::string> include_paths_; // search paths for `include

    // Process a single line, handling preprocessor directives
    std::string processLine(const std::string &line);

    // Handle `define directives
    void handleDefine(const std::string &line);

    // Handle `include directives
    std::string handleInclude(const std::string &line);

    // Handle `ifdef/`ifndef/`elsif/`else/`endif
    std::string handleConditional(const std::string &line, const std::string &full_source,
                                   size_t &pos, size_t end);

    // Expand macros in a line of text
    std::string expandMacros(const std::string &text);

    // Find end of conditional block
    size_t findConditionalEnd(const std::string &source, size_t start, size_t end);

    // Read file contents
    std::string readFile(const std::string &path);
};

} // namespace VerilogParser

#endif /* PREPROCESSOR_H */
