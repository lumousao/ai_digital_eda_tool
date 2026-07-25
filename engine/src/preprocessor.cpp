/**
 * Verilog Preprocessor Implementation
 */

#include "preprocessor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace VerilogParser {

Preprocessor::Preprocessor() : current_line_(0) {
    // Built-in defines
    define("SYNTHESIS", "1");
    define("AI_DIGITAL", "1");
}

void Preprocessor::define(const std::string &name, const std::string &value) {
    MacroDefinition m(name, value);
    defines_[name] = m;
}

void Preprocessor::undef(const std::string &name) {
    defines_.erase(name);
}

std::string Preprocessor::process(const std::string &source, const std::string &filename) {
    current_file_ = filename;
    current_line_ = 0;
    std::ostringstream result;

    std::istringstream stream(source);
    std::string line;
    bool skipping = false;          // inside false branch of conditional
    std::vector<bool> skip_stack;   // stack of skip states for nested conditionals
    std::vector<bool> branch_taken; // whether we've taken a branch at this level

    while (std::getline(stream, line)) {
        current_line_++;

        // Replace __FILE__ and __LINE__ built-in macros
        {
            size_t fp = 0;
            while ((fp = line.find("__FILE__", fp)) != std::string::npos) {
                line.replace(fp, 8, "\"" + current_file_ + "\"");
                fp += current_file_.size() + 2;
            }
            fp = 0;
            while ((fp = line.find("__LINE__", fp)) != std::string::npos) {
                std::string line_num = std::to_string(current_line_);
                line.replace(fp, 8, line_num);
                fp += line_num.size();
            }
        }

        // Handle `line directive for source tracking
        if (line.find("`line") == 0) {
            std::istringstream ls(line.substr(5));
            int new_line; std::string new_file;
            ls >> new_line;
            std::getline(ls, new_file);
            if (!new_file.empty()) {
                size_t f = new_file.find_first_not_of(" \t\"");
                size_t l = new_file.find_last_not_of(" \t\"");
                if (f != std::string::npos && l != std::string::npos)
                    current_file_ = new_file.substr(f, l - f + 1);
                else if (f != std::string::npos)
                    current_file_ = new_file.substr(f);
            }
            current_line_ = new_line - 1; // will be incremented at next loop
            result << "\n";
            continue;
        }

        // Trim leading whitespace (but keep track for output formatting)
        size_t first_non_space = line.find_first_not_of(" \t\r");
        if (first_non_space == std::string::npos) {
            if (!skipping) result << line << "\n";
            continue;
        }

        std::string prefix = line.substr(0, first_non_space);
        std::string content = line.substr(first_non_space);

        // Check for preprocessor directives
        if (content[0] == '`') {
            size_t space_pos = content.find(' ');
            std::string directive;
            std::string args;
            if (space_pos != std::string::npos) {
                directive = content.substr(1, space_pos - 1);
                args = content.substr(space_pos + 1);
            } else {
                directive = content.substr(1);
                args = "";
            }

            if (directive == "define") {
                if (!skipping) handleDefine(line);
                continue;
            }
            else if (directive == "undef") {
                if (!skipping) {
                    // Trim args
                    while (!args.empty() && std::isspace(args.back())) args.pop_back();
                    undef(args);
                }
                continue;
            }
            else if (directive == "ifdef") {
                skip_stack.push_back(skipping);
                branch_taken.push_back(false);
                if (!skipping) {
                    std::string name = args;
                    while (!name.empty() && std::isspace(name.back())) name.pop_back();
                    bool is_defined = defines_.count(name) > 0;
                    if (is_defined) {
                        skipping = false;
                        branch_taken.back() = true;
                    } else {
                        skipping = true;
                    }
                }
                continue;
            }
            else if (directive == "ifndef") {
                skip_stack.push_back(skipping);
                branch_taken.push_back(false);
                if (!skipping) {
                    std::string name = args;
                    while (!name.empty() && std::isspace(name.back())) name.pop_back();
                    bool is_defined = defines_.count(name) > 0;
                    if (!is_defined) {
                        skipping = false;
                        branch_taken.back() = true;
                    } else {
                        skipping = true;
                    }
                }
                continue;
            }
            else if (directive == "elsif" || directive == "elsifdef") {
                if (skip_stack.empty()) continue;
                if (skip_stack.back()) {
                    // Parent was already skipping
                    continue;
                }
                if (branch_taken.back()) {
                    skipping = true; // already took a branch
                } else {
                    std::string name = args;
                    while (!name.empty() && std::isspace(name.back())) name.pop_back();
                    bool is_defined = defines_.count(name) > 0;
                    if (is_defined) {
                        skipping = false;
                        branch_taken.back() = true;
                    } else {
                        skipping = true;
                    }
                }
                continue;
            }
            else if (directive == "else") {
                if (skip_stack.empty()) continue;
                if (skip_stack.back()) {
                    continue;
                }
                if (branch_taken.back()) {
                    skipping = true;
                } else {
                    skipping = false;
                    branch_taken.back() = true;
                }
                continue;
            }
            else if (directive == "endif") {
                if (!skip_stack.empty()) {
                    skipping = skip_stack.back();
                    skip_stack.pop_back();
                    branch_taken.pop_back();
                }
                continue;
            }
            else if (directive == "include") {
                if (!skipping) {
                    std::string included = handleInclude(line);
                    result << included;
                }
                continue;
            }
            // Other directives (`timescale, `resetall, etc.) — pass through
            if (!skipping) result << prefix << content << "\n";
            continue;
        }

        // Regular line — expand macros if not skipping
        if (!skipping) {
            std::string expanded = expandMacros(content);
            result << prefix << expanded << "\n";
        }
    }

    return result.str();
}

void Preprocessor::handleDefine(const std::string &line) {
    // Find `define keyword
    size_t pos = line.find("`define");
    if (pos == std::string::npos) return;
    pos += 7; // skip "`define"

    // Skip whitespace
    while (pos < line.size() && std::isspace(line[pos])) pos++;
    if (pos >= line.size()) return;

    // Read macro name
    size_t name_start = pos;
    while (pos < line.size() && (std::isalnum(line[pos]) || line[pos] == '_')) pos++;
    if (pos == name_start) return; // no valid name
    std::string name = line.substr(name_start, pos - name_start);

    // Check for argument list: `define MACRO(a, b) value
    bool has_args = false;
    std::vector<std::string> args;
    std::string value;

    if (pos < line.size() && line[pos] == '(') {
        // Has arguments
        has_args = true;
        pos++; // skip '('
        std::string arg_list;
        while (pos < line.size() && line[pos] != ')') {
            if (std::isalnum(line[pos]) || line[pos] == '_') {
                size_t arg_start = pos;
                while (pos < line.size() && (std::isalnum(line[pos]) || line[pos] == '_')) pos++;
                std::string arg = line.substr(arg_start, pos - arg_start);
                args.push_back(arg);
            } else {
                pos++;
            }
        }
        if (pos < line.size()) pos++; // skip ')'
    }

    // Skip whitespace before value
    while (pos < line.size() && std::isspace(line[pos])) pos++;

    // Read value (rest of line, with possible continuation via \)
    std::string val;
    if (pos < line.size()) {
        val = line.substr(pos);
        // Strip trailing backslash-newline marker
        while (!val.empty() && val.back() == '\\') val.pop_back();
    }

    MacroDefinition m(name, val);
    m.has_args = has_args;
    m.args = args;
    defines_[name] = m;
}

std::string Preprocessor::handleInclude(const std::string &line) {
    // Extract filename from `include "file.v" or `include <file.v>
    size_t q1 = line.find('"');
    size_t q2 = line.find('<');
    size_t start_quote = std::min(q1 == std::string::npos ? line.size() : q1,
                                   q2 == std::string::npos ? line.size() : q2);
    if (start_quote == std::string::npos || start_quote >= line.size()) return "";

    char end_char = (line[start_quote] == '"') ? '"' : '>';
    size_t end_quote = line.find(end_char, start_quote + 1);
    if (end_quote == std::string::npos) return "";

    std::string filename = line.substr(start_quote + 1, end_quote - start_quote - 1);

    // Prevent recursive includes
    if (included_files_.count(filename)) return "";
    included_files_.insert(filename);

    std::string content = readFile(filename);

    // If not found in current directory, try include paths
    if (content.empty() && !include_paths_.empty()) {
        for (auto &inc_path : include_paths_) {
            std::string full_path = inc_path + "/" + filename;
            content = readFile(full_path);
            if (!content.empty()) break;
        }
    }

    if (content.empty()) return "";

    // Recursively preprocess included file
    return process(content, filename);
}

std::string Preprocessor::expandMacros(const std::string &text) {
    std::string result = text;

    // Iterate over all defined macros and substitute
    // Simple text replacement for simple macros, function-like for arg macros
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 10) {
        changed = false;
        iterations++;

        for (auto &[name, macro] : defines_) {
            if (macro.has_args) {
                // Function-like macro: `MACRO(arg1, arg2)
                size_t pos = 0;
                std::string search = "`" + name;
                while ((pos = result.find(search, pos)) != std::string::npos) {
                    // Check for '(' after the macro name
                    size_t after_name = pos + search.size();
                    size_t lparen = after_name;
                    while (lparen < result.size() && std::isspace(result[lparen])) lparen++;
                    if (lparen < result.size() && result[lparen] == '(') {
                        // Extract arguments with proper nested paren handling
                        size_t rparen_pos = lparen + 1;
                        int paren_depth = 0;
                        while (rparen_pos < result.size()) {
                            if (result[rparen_pos] == '(') paren_depth++;
                            else if (result[rparen_pos] == ')') {
                                if (paren_depth == 0) break;
                                paren_depth--;
                            }
                            rparen_pos++;
                        }
                        if (rparen_pos < result.size()) {
                            std::string args_str = result.substr(lparen + 1, rparen_pos - lparen - 1);
                            // Split by comma with paren-depth tracking
                            std::vector<std::string> actual_args;
                            int depth = 0;
                            std::string current_arg;
                            for (size_t ai = 0; ai < args_str.size(); ai++) {
                                if (args_str[ai] == '(') depth++;
                                else if (args_str[ai] == ')') depth--;
                                else if (args_str[ai] == ',' && depth == 0) {
                                    // Trim
                                    size_t f = current_arg.find_first_not_of(" \t");
                                    size_t l = current_arg.find_last_not_of(" \t");
                                    if (f != std::string::npos)
                                        actual_args.push_back(current_arg.substr(f, l - f + 1));
                                    else
                                        actual_args.push_back("");
                                    current_arg.clear();
                                    continue;
                                }
                                current_arg += args_str[ai];
                            }
                            // Last arg
                            if (!current_arg.empty() || !args_str.empty()) {
                                size_t f = current_arg.find_first_not_of(" \t");
                                size_t l = current_arg.find_last_not_of(" \t");
                                if (f != std::string::npos)
                                    actual_args.push_back(current_arg.substr(f, l - f + 1));
                                else
                                    actual_args.push_back("");
                            }

                            // Substitute arguments into value
                            std::string substituted = macro.value;
                            for (size_t i = 0; i < macro.args.size() && i < actual_args.size(); i++) {
                                std::string placeholder = macro.args[i];
                                size_t p;
                                while ((p = substituted.find(placeholder)) != std::string::npos) {
                                    substituted.replace(p, placeholder.size(), actual_args[i]);
                                }
                            }

                            // Handle `##` (token pasting): remove any remaining ## between tokens
                            {
                                size_t tp;
                                while ((tp = substituted.find("##")) != std::string::npos) {
                                    substituted.erase(tp, 2);
                                }
                            }

                            // Handle `#stringify` by replacing `#arg` with "arg_value"
                            for (size_t i = 0; i < macro.args.size() && i < actual_args.size(); i++) {
                                std::string pattern = "#" + macro.args[i];
                                size_t sp;
                                while ((sp = substituted.find(pattern)) != std::string::npos) {
                                    substituted.replace(sp, pattern.size(), "\"" + actual_args[i] + "\"");
                                }
                            }

                            result.replace(pos, rparen_pos - pos + 1, substituted);
                            changed = true;
                            pos += substituted.size();
                            continue;
                        }
                    }
                    pos += search.size();
                }
            } else {
                // Simple macro: replace `NAME with value
                std::string search = "`" + name;
                size_t pos = 0;
                while ((pos = result.find(search, pos)) != std::string::npos) {
                    // Verify this is a word boundary (followed by non-identifier char)
                    size_t after = pos + search.size();
                    if (after >= result.size() || !std::isalnum(result[after]) && result[after] != '_') {
                        result.replace(pos, search.size(), macro.value);
                        changed = true;
                        pos += macro.value.size();
                    } else {
                        pos += search.size();
                    }
                }
            }
        }
    }

    return result;
}

size_t Preprocessor::findConditionalEnd(const std::string &source, size_t start, size_t end) {
    int depth = 1;
    size_t pos = start;
    while (pos < end && depth > 0) {
        if (source[pos] == '`') {
            size_t line_end = source.find('\n', pos);
            if (line_end == std::string::npos) line_end = end;
            std::string line = source.substr(pos, line_end - pos);
            // Check for `ifdef, `ifndef, `endif
            if (line.find("`ifdef") == 0 || line.find("`ifndef") == 0) {
                depth++;
            } else if (line.find("`endif") == 0 || line.find("`elsif") == 0
                       || (line.find("`else") == 0 && line.size() < 7)) {
                depth--;
                if (depth == 0) return pos;
            }
        }
        pos++;
    }
    return end;
}

std::string Preprocessor::readFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        // Try relative to current file directory
        std::string dir;
        size_t slash = current_file_.find_last_of("/\\");
        if (slash != std::string::npos) {
            dir = current_file_.substr(0, slash + 1);
            file.open(dir + path);
            if (!file.is_open()) return "";
        } else {
            return "";
        }
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace VerilogParser
