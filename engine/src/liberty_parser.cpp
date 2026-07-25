/**
 * Liberty (.lib) Parser — Full recursive-descent NLDM-compatible parser
 *
 * Liberty format grammar:
 *   library      := "library" "(" name ")" "{" library_body "}"
 *   library_body := (attribute | cell_def | group_def)*
 *   cell_def     := "cell" "(" name ")" "{" cell_body "}"
 *   cell_body    := (attribute | pin_def | pg_pin_def | leakage_def | group_def)*
 *   pin_def      := "pin" "(" name ")" "{" pin_body "}"
 *   pin_body     := (attribute | timing_def | internal_power_def | group_def)*
 *   timing_def   := "timing" "(" ")" "{" timing_body "}"
 *   group_def    := keyword "(" name ")" "{" group_body "}"
 *   attribute    := keyword ":" value ";"  |  keyword value ";"  |  keyword "(" args ")" ";"
 *
 * Strategy: Recursive descent with explicit brace counting.
 * Each parse_xxx function consumes its own closing "}".
 */
#include "liberty_parser.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>

namespace Liberty {

/* =========================================================================
 * Minimal Tokenizer — just enough to split Liberty tokens
 * ========================================================================= */
class Tokenizer {
public:
    explicit Tokenizer(const std::string &s) : s_(s), pos_(0), len_(s.size()) {}

    enum Type { END, KW, STR, NUM, LBR, RBR, SEMI, COLON, LPAREN, RPAREN, COMMA, BACKSLASH, EQUALS };

    struct Token { Type type; std::string text; };

    Token peek() {
        size_t saved = pos_;
        Token t = next();
        pos_ = saved;
        return t;
    }

    Token next() {
        skip();
        if (pos_ >= len_) return {END, ""};
        char c = s_[pos_];
        switch (c) {
        case '{': pos_++; return {LBR, "{"};
        case '}': pos_++; return {RBR, "}"};
        case ';': pos_++; return {SEMI, ";"};
        case ':': pos_++; return {COLON, ":"};
        case '(': pos_++; return {LPAREN, "("};
        case ')': pos_++; return {RPAREN, ")"};
        case ',': pos_++; return {COMMA, ","};
        case '\\': pos_++; return {BACKSLASH, "\\"};
        case '=': pos_++; return {EQUALS, "="};
        case '"': return read_string();
        default:
            if (c == '-' || c == '+' || c == '.' || std::isdigit(c)) {
                Token t = read_number();
                if (t.type == NUM || t.type == KW) return t;
                // fall through to keyword
            }
            return read_keyword();
        }
    }

private:
    std::string s_;
    size_t pos_, len_;

    void skip() {
        while (pos_ < len_) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { pos_++; continue; }
            // C comment /* ... */
            if (c == '/' && pos_ + 1 < len_ && s_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < len_ && !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) pos_++;
                if (pos_ + 1 < len_) pos_ += 2;
                continue;
            }
            // C++ comment //
            if (c == '/' && pos_ + 1 < len_ && s_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < len_ && s_[pos_] != '\n') pos_++;
                continue;
            }
            break;
        }
    }

    Token read_string() {
        pos_++; // opening "
        std::string s;
        while (pos_ < len_ && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < len_) { pos_++; s += s_[pos_]; }
            else s += s_[pos_];
            pos_++;
        }
        if (pos_ < len_) pos_++; // closing "
        return {STR, s};
    }

    Token read_number() {
        std::string s;
        if (pos_ < len_ && (s_[pos_] == '-' || s_[pos_] == '+')) { s += s_[pos_]; pos_++; }
        while (pos_ < len_ && (std::isdigit(s_[pos_]) || s_[pos_] == '.' ||
               s_[pos_] == 'e' || s_[pos_] == 'E')) {
            if ((s_[pos_] == 'e' || s_[pos_] == 'E') && pos_ + 1 < len_ &&
                (s_[pos_ + 1] == '-' || s_[pos_ + 1] == '+')) {
                s += s_[pos_]; pos_++; s += s_[pos_]; pos_++;
                continue;
            }
            s += s_[pos_]; pos_++;
        }
        bool has_digit = false;
        for (char ch : s) if (std::isdigit(ch)) { has_digit = true; break; }
        return has_digit ? Token{NUM, s} : Token{KW, ""};
    }

    Token read_keyword() {
        std::string s;
        while (pos_ < len_ && s_[pos_] != ' ' && s_[pos_] != '\t' && s_[pos_] != '\n' &&
               s_[pos_] != '\r' && s_[pos_] != '{' && s_[pos_] != '}' &&
               s_[pos_] != ';' && s_[pos_] != ':' && s_[pos_] != '(' &&
               s_[pos_] != ')' && s_[pos_] != ',' && s_[pos_] != '\\' &&
               s_[pos_] != '=' && s_[pos_] != '"') {
            s += s_[pos_]; pos_++;
        }
        return {KW, s};
    }
};

/* =========================================================================
 * Recursive-descent Parser
 * ========================================================================= */
class Parser {
public:
    Parser(const std::string &content) : tok_(content), lib_(nullptr) {}

    bool parse(LibertyLibrary &lib) {
        lib_ = &lib;
        try {
            parse_library();
            return true;
        } catch (const std::string &e) {
            std::cerr << "Liberty parse error: " << e << std::endl;
            return false;
        }
    }

private:
    Tokenizer tok_;
    LibertyLibrary *lib_;

    double to_dbl(const std::string &s) { try { return std::stod(s); } catch (...) { return 0.0; } }

    // ── Helpers ──

    // expect a specific token type, return its text (or throw)
    std::string expect(Tokenizer::Type t, const char *what) {
        auto tok = tok_.next();
        if (tok.type != t) {
            throw std::string("Expected ") + what + " but got '" + tok.text + "'";
        }
        return tok.text;
    }

    // Parse "(name)" or "(name1, name2)" — return the name (first arg)
    std::string parse_paren_name() {
        expect(Tokenizer::LPAREN, "(");
        auto t = tok_.next();
        std::string name;
        if (t.type == Tokenizer::KW || t.type == Tokenizer::STR || t.type == Tokenizer::NUM) {
            name = t.text;
        }
        // skip rest until )
        while (t.type != Tokenizer::RPAREN && t.type != Tokenizer::END) {
            t = tok_.next();
        }
        return name;
    }

    // Skip tokens until "}" at the current nesting level
    void skip_to_rbrace() {
        int depth = 1;
        while (depth > 0) {
            auto t = tok_.next();
            if (t.type == Tokenizer::END) return;
            if (t.type == Tokenizer::LBR) depth++;
            if (t.type == Tokenizer::RBR) depth--;
        }
    }

    // ── Top-level parsing ──

    void parse_library() {
        auto t = tok_.next();
        if (t.type != Tokenizer::KW || t.text != "library") {
            throw std::string("Expected 'library'");
        }
        lib_->name = parse_paren_name();
        expect(Tokenizer::LBR, "{");

        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            parse_library_item();
        }
        expect(Tokenizer::RBR, "}"); // library close
        // Reached EOF — success
    }

    void parse_library_item() {
        auto kw = tok_.next();
        if (kw.type == Tokenizer::END || kw.type == Tokenizer::RBR) return;

        if (kw.text == "cell") {
            parse_cell_def();
            return;
        }

        // Library attribute or group — parse name if exists, then body or value
        auto peek = tok_.peek();

        if (peek.type == Tokenizer::LPAREN) {
            // keyword (name) { body } or keyword (name) ;
            std::string name = parse_paren_name();
            auto p2 = tok_.peek();
            if (p2.type == Tokenizer::LBR) {
                tok_.next(); // {
                handle_library_group(kw.text, name);
                skip_to_rbrace(); // skip body if not handled
            } else if (p2.type == Tokenizer::SEMI) {
                tok_.next(); // ;
                handle_library_attr(kw.text, name);
            }
        } else if (peek.type == Tokenizer::COLON) {
            tok_.next(); // :
            auto val = tok_.next(); // value
            if (val.type == Tokenizer::KW || val.type == Tokenizer::STR || val.type == Tokenizer::NUM) {
                handle_library_attr(kw.text, val.text);
            }
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
        } else if (peek.type == Tokenizer::LBR) {
            tok_.next(); // {
            skip_to_rbrace(); // skip body
        } else {
            // keyword value ;
            auto val = tok_.next();
            handle_library_attr(kw.text, val.text);
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
        }
    }

    void handle_library_attr(const std::string &key, const std::string &val) {
        if (key == "nom_voltage") lib_->nom_voltage = to_dbl(val);
        else if (key == "nom_temperature") lib_->nom_temperature = to_dbl(val);
        else if (key == "nom_process") lib_->nom_process = to_dbl(val);
        else if (key == "time_unit") lib_->time_unit = to_dbl(val);
        else if (key == "voltage_unit") lib_->voltage_unit = to_dbl(val);
        else if (key == "capacitive_load_unit") lib_->capacitive_load_unit = to_dbl(val);
        else if (key == "leakage_power_unit") lib_->leakage_power_unit = to_dbl(val);
        else if (key == "current_unit") lib_->current_unit = to_dbl(val);
        else if (key == "default_max_transition") lib_->default_max_transition = to_dbl(val);
        else if (key == "default_fanout_load") lib_->default_fanout_load = to_dbl(val);
        else if (key == "default_output_pin_cap") lib_->default_output_pin_cap = to_dbl(val);
        else if (key == "slew_derate_from_library") lib_->slew_derate_from_library = to_dbl(val);
    }

    void handle_library_group(const std::string &key, const std::string &name) {
        // Groups like operating_conditions, lu_table_template, etc. — body already skipped
        (void)key; (void)name;
    }

    // ── Cell ──

    void parse_cell_def() {
        LibertyCell cell;
        cell.name = parse_paren_name();
        expect(Tokenizer::LBR, "{");

        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            parse_cell_item(cell);
        }
        tok_.next(); // consume closing }

        // Store cell
        lib_->cells[cell.name] = cell;
        lib_->cell_names.push_back(cell.name);
    }

    void parse_cell_item(LibertyCell &cell) {
        auto kw = tok_.next();
        if (kw.type == Tokenizer::END || kw.type == Tokenizer::RBR) return;

        auto p = tok_.peek();

        if (kw.text == "pin") {
            LibertyPin pin;
            pin.name = parse_paren_name();
            if (tok_.peek().type == Tokenizer::LBR) {
                tok_.next(); // {
                parse_pin_body(pin);
            }
            cell.pins[pin.name] = pin;
            if (pin.name == "C" || pin.name == "CK" || pin.name == "CLK" ||
                pin.name == "CP" || pin.name == "G" || pin.name == "GN") {
                cell.is_sequential = true;
            }
            return;
        }

        if (kw.text == "pg_pin") {
            parse_paren_name();
            if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); skip_to_rbrace(); }
            return;
        }

        if (kw.text == "leakage_power") {
            // State-dependent leakage: either "() {" or value on same line
            std::string when;
            double val = 0;
            if (p.type == Tokenizer::LPAREN) {
                parse_paren_name();
                if (tok_.peek().type == Tokenizer::LBR) {
                    tok_.next(); // {
                    while (true) {
                        auto ik = tok_.peek();
                        if (ik.type == Tokenizer::RBR || ik.type == Tokenizer::END) break;
                        auto item = tok_.next();
                        if (item.text == "when") {
                            if (tok_.peek().type == Tokenizer::COLON) { tok_.next(); }
                            auto v = tok_.next();
                            if (v.type == Tokenizer::STR) when = v.text;
                        } else if (item.text == "value") {
                            if (tok_.peek().type == Tokenizer::COLON) { tok_.next(); }
                            auto v = tok_.next();
                            val = to_dbl(v.text);
                        }
                        if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                    }
                    tok_.next(); // }
                }
            }
            if (!when.empty()) cell.state_leakages[when] = val;
            else cell.cell_leakage_power = val > 0 ? val : cell.cell_leakage_power;
            return;
        }

        // Simple attributes: keyword : value ; or keyword value ;
        if (p.type == Tokenizer::COLON) {
            tok_.next(); // :
            auto v = tok_.next();
            std::string val = (v.type == Tokenizer::KW || v.type == Tokenizer::STR || v.type == Tokenizer::NUM) ? v.text : "";
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();

            if (kw.text == "area") cell.area = to_dbl(val);
            else if (kw.text == "cell_footprint") cell.footprint = val;
            else if (kw.text == "cell_leakage_power") cell.cell_leakage_power = to_dbl(val);
            else if (kw.text == "dont_use") cell.dont_use = true;
        } else if (p.type == Tokenizer::KW || p.type == Tokenizer::STR || p.type == Tokenizer::NUM) {
            auto v = tok_.next();
            std::string val = v.text;
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();

            if (kw.text == "area") cell.area = to_dbl(val);
            else if (kw.text == "cell_leakage_power") cell.cell_leakage_power = to_dbl(val);
            else if (kw.text == "dont_use" || kw.text == "dont_touch") {
                if (kw.text == "dont_use") cell.dont_use = true;
                else cell.dont_touch = true;
            }
        } else if (p.type == Tokenizer::LBR) {
            tok_.next(); // {
            // Detect sequential cells from attribute names in cell body
            if (kw.text.find("FF") != std::string::npos ||
                kw.text.find("LAT") != std::string::npos) {
                cell.is_sequential = true;
            }
            skip_to_rbrace();
        }
    }

    // ── Pin ──

    void parse_pin_body(LibertyPin &pin) {
        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            parse_pin_item(pin);
        }
        tok_.next(); // consume closing }
    }

    void parse_pin_item(LibertyPin &pin) {
        auto kw = tok_.next();
        if (kw.type == Tokenizer::END || kw.type == Tokenizer::RBR) return;

        auto p = tok_.peek();

        // "timing () { ... }" — parse NLDM timing arcs
        if (kw.text == "timing") {
            LibertyTimingArc arc;
            if (p.type == Tokenizer::LPAREN) parse_paren_name(); // skip "()"
            if (tok_.peek().type == Tokenizer::LBR) {
                tok_.next(); // {
                parse_timing_body(arc);
            }
            pin.timing_arcs.push_back(arc);
            return;
        }

        // "internal_power () { ... }" — parse NLDM power arcs
        if (kw.text == "internal_power") {
            LibertyPowerArc arc;
            if (p.type == Tokenizer::LPAREN) parse_paren_name(); // skip "()"
            if (tok_.peek().type == Tokenizer::LBR) {
                tok_.next(); // {
                parse_power_body(arc);
            }
            pin.power_arcs.push_back(arc);
            return;
        }

        // Groups with name: "output_voltage (name) { ... }"
        if (p.type == Tokenizer::LPAREN) {
            parse_paren_name();
            if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); skip_to_rbrace(); }
            return;
        }

        if (p.type == Tokenizer::COLON) {
            tok_.next(); // :
            auto v = tok_.next();
            std::string val = (v.type == Tokenizer::KW || v.type == Tokenizer::STR || v.type == Tokenizer::NUM) ? v.text : "";
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();

            if (kw.text == "direction") pin.direction = val;
            else if (kw.text == "function") pin.function = val;
            else if (kw.text == "capacitance") pin.capacitance = to_dbl(val);
            else if (kw.text == "max_capacitance") pin.max_capacitance = to_dbl(val);
            else if (kw.text == "max_transition") pin.max_transition = to_dbl(val);
            else if (kw.text == "rise_capacitance") pin.rise_capacitance = to_dbl(val);
            else if (kw.text == "fall_capacitance") pin.fall_capacitance = to_dbl(val);
        } else if (p.type == Tokenizer::KW || p.type == Tokenizer::STR || p.type == Tokenizer::NUM) {
            auto v = tok_.next();
            std::string val = v.text;
            if (tok_.peek().type == Tokenizer::SEMI) tok_.next();

            if (kw.text == "direction") pin.direction = val;
        } else if (p.type == Tokenizer::LBR) {
            tok_.next(); skip_to_rbrace();
        }
    }

    // ── NLDM Table parsing helpers ──

    /** Parse comma-separated list of doubles: "val1, val2, ..." */
    std::vector<double> parse_double_list() {
        std::vector<double> result;
        auto t = tok_.next(); // string with comma-separated values
        std::string s = t.text;
        // Split by commas
        size_t pos = 0;
        while (pos < s.size()) {
            // Skip whitespace
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            std::string num = s.substr(pos, comma - pos);
            if (!num.empty()) {
                try { result.push_back(std::stod(num)); }
                catch (...) { result.push_back(0.0); }
            }
            pos = comma + 1;
        }
        return result;
    }

    /** Parse a complete NLDM table: index_1 (...) ; index_2 (...) ; values ( \ "row1"\ ... ) ; */
    LibertyTable parse_nldm_table() {
        LibertyTable tbl;
        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            if (kw.type == Tokenizer::KW) {
                std::string key = kw.text;
                tok_.next(); // consume keyword
                auto p = tok_.peek();
                if (key == "index_1" || key == "index_2") {
                    if (p.type == Tokenizer::LPAREN) {
                        tok_.next(); // (
                        auto str_tok = tok_.next(); // "val1, val2, ..."
                        if (tok_.peek().type == Tokenizer::RPAREN) tok_.next(); // )
                        // Parse the string as comma-separated list
                        std::vector<double> vals;
                        std::string s = str_tok.text;
                        size_t pos = 0;
                        while (pos < s.size()) {
                            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
                            size_t comma = s.find(',', pos);
                            if (comma == std::string::npos) comma = s.size();
                            std::string num = s.substr(pos, comma - pos);
                            if (!num.empty()) {
                                try { vals.push_back(std::stod(num)); }
                                catch (...) { vals.push_back(0.0); }
                            }
                            pos = comma + 1;
                        }
                        if (key == "index_1") tbl.index_1 = vals;
                        else tbl.index_2 = vals;
                        if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                    }
                } else if (key == "values") {
                    if (p.type == Tokenizer::LPAREN) {
                        tok_.next(); // (
                        // Parse rows: \ "row1", \ "row2", ... or just "row1", "row2"
                        while (true) {
                            auto vk = tok_.peek();
                            if (vk.type == Tokenizer::RPAREN || vk.type == Tokenizer::END) break;
                            if (vk.type == Tokenizer::BACKSLASH) tok_.next(); // consume backslash
                            if (tok_.peek().type == Tokenizer::RPAREN) break;
                            auto row_tok = tok_.next(); // "val1, val2, ..."
                            if (row_tok.type == Tokenizer::STR || row_tok.type == Tokenizer::KW) {
                                std::string s = row_tok.text;
                                std::vector<double> row;
                                size_t pos = 0;
                                while (pos < s.size()) {
                                    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
                                    size_t comma = s.find(',', pos);
                                    if (comma == std::string::npos) comma = s.size();
                                    std::string num = s.substr(pos, comma - pos);
                                    if (!num.empty()) {
                                        try { row.push_back(std::stod(num)); }
                                        catch (...) { row.push_back(0.0); }
                                    }
                                    pos = comma + 1;
                                }
                                if (!row.empty()) tbl.values.push_back(row);
                            }
                            if (tok_.peek().type == Tokenizer::COMMA) tok_.next(); // consume comma between rows
                        }
                        if (tok_.peek().type == Tokenizer::RPAREN) tok_.next(); // )
                        if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                    }
                } else {
                    // Unknown keyword in table — skip
                    if (p.type == Tokenizer::LPAREN) {
                        parse_paren_name();
                        if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); skip_to_rbrace(); }
                        else if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                    } else if (p.type == Tokenizer::COLON) {
                        tok_.next(); // :
                        tok_.next(); // value
                        if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                    }
                }
            } else {
                break;
            }
        }
        // Consume the closing } of the table body (e.g., cell_rise (...) { ... })
        if (tok_.peek().type == Tokenizer::RBR) tok_.next();
        return tbl;
    }

    /** Parse timing body: related_pin, timing_sense, timing_type, when, cell_rise/fall, etc. */
    void parse_timing_body(LibertyTimingArc &arc) {
        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            auto item = tok_.next();

            if (item.type == Tokenizer::RBR) return;

            auto p = tok_.peek();

            if (item.text == "related_pin") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::STR || v.type == Tokenizer::KW) arc.related_pin = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "timing_sense") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::KW) arc.timing_sense = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "timing_type") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::KW) arc.timing_type = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "when" || item.text == "sdf_cond") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::STR) arc.when = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "cell_rise") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name(); // template name
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.cell_rise = parse_nldm_table(); }
                }
            } else if (item.text == "cell_fall") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.cell_fall = parse_nldm_table(); }
                }
            } else if (item.text == "rise_transition") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.rise_transition = parse_nldm_table(); }
                }
            } else if (item.text == "fall_transition") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.fall_transition = parse_nldm_table(); }
                }
            } else if (item.text == "rise_constraint") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.rise_constraint = parse_nldm_table(); }
                }
            } else if (item.text == "fall_constraint") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.fall_constraint = parse_nldm_table(); }
                }
            } else if (item.text == "related_pg_pin" || item.text == "related_ground_pin" || item.text == "related_power_pin") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                tok_.next(); // value
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else {
                // Unknown — skip
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); skip_to_rbrace(); }
                    else if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                } else if (p.type == Tokenizer::COLON) {
                    tok_.next(); tok_.next();
                    if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                } else if (p.type == Tokenizer::LBR) {
                    tok_.next(); skip_to_rbrace();
                }
            }
        }
        tok_.next(); // consume closing }
    }

    /** Parse internal_power body: related_pin, when, rise_power, fall_power */
    void parse_power_body(LibertyPowerArc &arc) {
        while (true) {
            auto kw = tok_.peek();
            if (kw.type == Tokenizer::RBR || kw.type == Tokenizer::END) break;
            auto item = tok_.next();

            if (item.type == Tokenizer::RBR) return;

            auto p = tok_.peek();

            if (item.text == "related_pin") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::STR || v.type == Tokenizer::KW) arc.related_pin = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "when") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                auto v = tok_.next();
                if (v.type == Tokenizer::STR) arc.when = v.text;
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else if (item.text == "rise_power") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.rise_power = parse_nldm_table(); }
                }
            } else if (item.text == "fall_power") {
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); arc.fall_power = parse_nldm_table(); }
                }
            } else if (item.text == "related_pg_pin" || item.text == "related_ground_pin" || item.text == "related_power_pin") {
                if (p.type == Tokenizer::COLON) { tok_.next(); }
                tok_.next(); // value
                if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
            } else {
                // Unknown — skip
                if (p.type == Tokenizer::LPAREN) {
                    parse_paren_name();
                    if (tok_.peek().type == Tokenizer::LBR) { tok_.next(); skip_to_rbrace(); }
                    else if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                } else if (p.type == Tokenizer::COLON) {
                    tok_.next(); tok_.next();
                    if (tok_.peek().type == Tokenizer::SEMI) tok_.next();
                } else if (p.type == Tokenizer::LBR) {
                    tok_.next(); skip_to_rbrace();
                }
            }
        }
        tok_.next(); // consume closing }
    }
};

/* =========================================================================
 * Public API
 * ========================================================================= */

bool LibertyLibrary::load(const std::string &filename) {
    this->filename = filename;

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Cannot open liberty file: " << filename << std::endl;
        return false;
    }

    std::string content, line;
    while (std::getline(file, line)) content += line + "\n";
    file.close();

    if (content.empty()) return false;

    Parser parser(content);
    return parser.parse(*this);
}

} // namespace Liberty