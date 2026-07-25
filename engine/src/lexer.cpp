/**
 * Complete Lexer for Verilog-2001/2005/SystemVerilog
 */

#include "lexer.h"
#include <cctype>
#include <algorithm>
#include <iostream>

namespace VerilogParser {


std::map<std::string, TokenType> Lexer::keywords_;
std::set<std::string> Lexer::systemTasks_;
bool Lexer::keywordsInitialized_ = false;

Lexer::Lexer(const char *code, size_t len, const std::string &filename)
    : code_(code), len_(len), pos_(0), line_(1), column_(1),
      filename_(filename), systemVerilog_(true), debug_(false),
      bufferPos_(0) {
    if (!keywordsInitialized_) {
        initKeywords();
        keywordsInitialized_ = true;
    }
}

Lexer::~Lexer() {}

void Lexer::initKeywords() {
    keywords_["module"]      = TokenType::TOK_MODULE;
    keywords_["endmodule"]   = TokenType::TOK_ENDMODULE;
    keywords_["input"]       = TokenType::TOK_INPUT;
    keywords_["output"]      = TokenType::TOK_OUTPUT;
    keywords_["inout"]       = TokenType::TOK_INOUT;
    keywords_["wire"]        = TokenType::TOK_WIRE;
    keywords_["reg"]         = TokenType::TOK_REG;
    keywords_["logic"]       = TokenType::TOK_LOGIC;
    keywords_["integer"]     = TokenType::TOK_INTEGER;
    keywords_["real"]        = TokenType::TOK_REAL_KW;
    keywords_["time"]        = TokenType::TOK_TIME;
    keywords_["parameter"]   = TokenType::TOK_PARAMETER;
    keywords_["localparam"]  = TokenType::TOK_LOCALPARAM;
    keywords_["assign"]      = TokenType::TOK_ASSIGN;
    keywords_["always"]      = TokenType::TOK_ALWAYS;
    keywords_["initial"]     = TokenType::TOK_INITIAL;
    keywords_["begin"]       = TokenType::TOK_BEGIN;
    keywords_["end"]         = TokenType::TOK_END;
    keywords_["if"]          = TokenType::TOK_IF;
    keywords_["else"]        = TokenType::TOK_ELSE;
    keywords_["case"]        = TokenType::TOK_CASE;
    keywords_["casex"]       = TokenType::TOK_CASEX;
    keywords_["casez"]       = TokenType::TOK_CASEZ;
    keywords_["endcase"]     = TokenType::TOK_ENDCASE;
    keywords_["default"]     = TokenType::TOK_DEFAULT;
    keywords_["for"]         = TokenType::TOK_FOR;
    keywords_["while"]       = TokenType::TOK_WHILE;
    keywords_["repeat"]      = TokenType::TOK_REPEAT;
    keywords_["forever"]     = TokenType::TOK_FOREVER;
    keywords_["posedge"]     = TokenType::TOK_POSEDGE;
    keywords_["negedge"]     = TokenType::TOK_NEGEDGE;
    keywords_["and"]         = TokenType::TOK_AND_T;
    keywords_["or"]          = TokenType::TOK_OR_T;
    keywords_["not"]         = TokenType::TOK_NOT_T;
    keywords_["xor"]         = TokenType::TOK_XOR_T;
    keywords_["xnor"]        = TokenType::TOK_XNOR_T;
    keywords_["nand"]        = TokenType::TOK_NAND_T;
    keywords_["nor"]         = TokenType::TOK_NOR_T;
    keywords_["buf"]         = TokenType::TOK_BUF;
    keywords_["primitive"]   = TokenType::TOK_PRIMITIVE;
    keywords_["endprimitive"]= TokenType::TOK_ENDPRIMITIVE;
    keywords_["specify"]     = TokenType::TOK_SPECIFY;
    keywords_["endspecify"]  = TokenType::TOK_ENDSPECIFY;
    keywords_["function"]    = TokenType::TOK_FUNCTION;
    keywords_["endfunction"] = TokenType::TOK_ENDFUNCTION;
    keywords_["task"]        = TokenType::TOK_TASK;
    keywords_["endtask"]     = TokenType::TOK_ENDTASK;
    keywords_["generate"]    = TokenType::TOK_GENERATE;
    keywords_["endgenerate"] = TokenType::TOK_ENDGENERATE;
    keywords_["genvar"]      = TokenType::TOK_GENVAR;
    keywords_["signed"]      = TokenType::TOK_SIGNED;
    keywords_["unsigned"]    = TokenType::TOK_UNSIGNED;
    keywords_["class"]       = TokenType::TOK_CLASS;
    keywords_["endclass"]    = TokenType::TOK_ENDCLASS;
    keywords_["extends"]     = TokenType::TOK_EXTENDS;
    keywords_["interface"]   = TokenType::TOK_INTERFACE;
    keywords_["endinterface"]= TokenType::TOK_ENDINTERFACE;
    keywords_["modport"]     = TokenType::TOK_MODPORT;
    keywords_["package"]     = TokenType::TOK_PACKAGE;
    keywords_["endpackage"]  = TokenType::TOK_ENDPACKAGE;
    keywords_["import"]      = TokenType::TOK_IMPORT;
    keywords_["export"]      = TokenType::TOK_EXPORT;
    keywords_["timeunit"]    = TokenType::TOK_TIMEUNIT;
    keywords_["timeprecision"] = TokenType::TOK_TIMEPRECISION;
    keywords_["default_nettype"] = TokenType::TOK_DEFAULT_NETTYPE;
    keywords_["celldefine"]  = TokenType::TOK_CELLDEFINE;
    keywords_["endcelldefine"] = TokenType::TOK_ENDCELLDEFINE;
    keywords_["always_ff"]   = TokenType::TOK_ALWAYS_FF;
    keywords_["always_comb"] = TokenType::TOK_ALWAYS_COMB;
    keywords_["always_latch"]= TokenType::TOK_ALWAYS_LATCH;
    keywords_["assert"]      = TokenType::TOK_ASSERT_T;
    keywords_["assume"]      = TokenType::TOK_ASSUME_T;
    keywords_["cover"]       = TokenType::TOK_COVER_T;
    keywords_["covergroup"]  = TokenType::TOK_COVERGROUP;
    keywords_["endgroup"]    = TokenType::TOK_ENDGROUP;
    keywords_["coverpoint"]  = TokenType::TOK_COVERPOINT;
    keywords_["cross"]       = TokenType::TOK_CROSS;
    keywords_["property"]    = TokenType::TOK_PROPERTY;
    keywords_["unique"]      = TokenType::TOK_UNIQUE;
    keywords_["priority"]    = TokenType::TOK_PRIORITY;
    keywords_["typedef"]     = TokenType::TOK_TYPEDEF;
    keywords_["struct"]      = TokenType::TOK_STRUCT;
    keywords_["union"]       = TokenType::TOK_UNION;
    keywords_["enum"]        = TokenType::TOK_ENUM;
    keywords_["virtual"]     = TokenType::TOK_VIRTUAL;
    keywords_["static"]      = TokenType::TOK_STATIC;
    keywords_["automatic"]   = TokenType::TOK_AUTOMATIC;
    keywords_["rand"]        = TokenType::TOK_RAND;
    keywords_["randc"]       = TokenType::TOK_RANDC;
    keywords_["constraint"]  = TokenType::TOK_CONSTRAINT;
    keywords_["fork"]        = TokenType::TOK_FORK;
    keywords_["join"]        = TokenType::TOK_JOIN;
    keywords_["join_any"]    = TokenType::TOK_JOIN_ANY;
    keywords_["join_none"]   = TokenType::TOK_JOIN_NONE;
    keywords_["disable"]     = TokenType::TOK_DISABLE;
    keywords_["foreach"]     = TokenType::TOK_FOREACH;
    keywords_["string"]      = TokenType::TOK_STRING_T;
    keywords_["event"]       = TokenType::TOK_EVENT_T;
    keywords_["int"]         = TokenType::TOK_INT;
    keywords_["byte"]        = TokenType::TOK_BYTE;
    keywords_["bit"]         = TokenType::TOK_BIT_T;
    keywords_["ref"]         = TokenType::TOK_REF;
    keywords_["triand"]      = TokenType::TOK_TRIAND;
    keywords_["trior"]       = TokenType::TOK_TRIOR;

    systemTasks_ = {
        "$display", "$write", "$monitor", "$finish", "$stop",
        "$readmemh", "$readmemb", "$time", "$realtime", "$clog2",
        "$bits", "$typename", "$left", "$right", "$low", "$high",
        "$size", "$signed", "$unsigned"
    };
}

char Lexer::current() const { return pos_ < len_ ? code_[pos_] : '\0'; }
char Lexer::peekChar() const { return (pos_ + 1) < len_ ? code_[pos_ + 1] : '\0'; }

void Lexer::advance() {
    if (pos_ < len_) {
        if (code_[pos_] == '\n') { line_++; column_ = 1; }
        else { column_++; }
        pos_++;
    }
}

void Lexer::skipWhitespace() {
    while (pos_ < len_) {
        char c = current();
        if (std::isspace(c)) { advance(); }
        else if (c == '/' && peekChar() == '/') { while (pos_ < len_ && current() != '\n') advance(); }
        else if (c == '/' && peekChar() == '*') {
            advance(); advance();
            while (pos_ < len_ - 1) {
                if (current() == '*' && peekChar() == '/') { advance(); advance(); break; }
                advance();
            }
        }
        else if (c == '`') { while (pos_ < len_ && current() != '\n') advance(); }
        else { break; }
    }
}

Token Lexer::readIdentifier() {
    size_t start = pos_;
    int startLine = line_, startCol = column_;

    if (current() == '\\') {
        advance();
        while (pos_ < len_ && current() != ' ' && current() != '\n' && current() != '\t') advance();
    } else {
        while (pos_ < len_ && (std::isalnum(current()) || current() == '_' || current() == '$')) advance();
    }

    std::string value(code_ + start, pos_ - start);
    if (value[0] == '$') return Token(TokenType::TOK_SYS_TASK, value, startLine, startCol, filename_);

    TokenType type = lookupKeyword(value);
    return Token(type, value, startLine, startCol, filename_);
}

Token Lexer::readNumber() {
    size_t start = pos_;
    int startLine = line_, startCol = column_;

    // Read digits first (e.g., "16" in "16'h0001")
    while (pos_ < len_ && (std::isdigit(current()) || current() == '_')) advance();

    // Check for sized literal: digits'base_digits (e.g., 16'h0001, 8'b1010, 32'd100)
    if (pos_ < len_ && current() == '\'') {
        advance(); // consume '\''
        // Consume optional base letter (b/B/h/H/d/D/o/O)
        if (pos_ < len_ && (current() == 'b' || current() == 'B' || current() == 'h' || current() == 'H' ||
                            current() == 'd' || current() == 'D' || current() == 'o' || current() == 'O')) advance();
        // Consume value digits (may include x, z, _)
        while (pos_ < len_ && (std::isalnum(current()) || current() == '_' || current() == 'x' || current() == 'z' ||
                                current() == 'X' || current() == 'Z')) advance();
    }
    // Check for unsigned literal: 'base_digits (e.g., 'h0001)
    else if (pos_ < len_ && current() == '\'') {
        advance();
        if (pos_ < len_ && (current() == 'b' || current() == 'B' || current() == 'h' || current() == 'H' ||
                            current() == 'd' || current() == 'D' || current() == 'o' || current() == 'O')) advance();
        while (pos_ < len_ && (std::isalnum(current()) || current() == '_' || current() == 'x' || current() == 'z')) advance();
    }
    // Plain number (possibly with dots, exponents for real)
    else {
        while (pos_ < len_ && (std::isdigit(current()) || current() == '_' || current() == '.' ||
                                current() == 'e' || current() == 'E' ||
                                current() == 'x' || current() == 'X' || current() == 'z' || current() == 'Z')) advance();
    }

    return Token(TokenType::TOK_INTEGER, std::string(code_ + start, pos_ - start), startLine, startCol, filename_);
}

Token Lexer::readString() {
    int startLine = line_, startCol = column_;
    advance();
    std::string value;
    while (pos_ < len_ && current() != '"') {
        if (current() == '\\') {
            advance();
            switch (current()) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                default: value += current(); break;
            }
        } else { value += current(); }
        advance();
    }
    if (pos_ < len_) advance();
    return Token(TokenType::TOK_STRING, value, startLine, startCol, filename_);
}

Token Lexer::readOperator() {
    int startLine = line_, startCol = column_;
    char c = current();

    if (pos_ + 2 < len_) {
        char c2 = peekChar(), c3 = code_[pos_ + 2];
        if (c == '<' && c2 == '<' && c3 == '=') { advance(); advance(); advance(); return Token(TokenType::TOK_LTLTEQ, "<<=", startLine, startCol, filename_); }
        if (c == '>' && c2 == '>' && c3 == '=') { advance(); advance(); advance(); return Token(TokenType::TOK_GTGTEQ, ">>=", startLine, startCol, filename_); }
        if (c == '>' && c2 == '>' && c3 == '>') { advance(); advance(); advance(); return Token(TokenType::TOK_GTGTGT, ">>>", startLine, startCol, filename_); }
        if (c == '!' && c2 == '=' && c3 == '=') { advance(); advance(); advance(); return Token(TokenType::TOK_CASE_NEQ, "!==", startLine, startCol, filename_); }
        if (c == '=' && c2 == '=' && c3 == '=') { advance(); advance(); advance(); return Token(TokenType::TOK_CASE_EQ, "===", startLine, startCol, filename_); }
    }

    if (pos_ + 1 < len_) {
        char c2 = peekChar();
        if (c == '=' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_EQ, "==", startLine, startCol, filename_); }
        if (c == '!' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_NEQ, "!=", startLine, startCol, filename_); }
        if (c == '<' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_LEQ, "<=", startLine, startCol, filename_); }
        if (c == '>' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_GEQ, ">=", startLine, startCol, filename_); }
        if (c == '<' && c2 == '<') { advance(); advance(); return Token(TokenType::TOK_LTLT, "<<", startLine, startCol, filename_); }
        if (c == '>' && c2 == '>') { advance(); advance(); return Token(TokenType::TOK_GTGT, ">>", startLine, startCol, filename_); }
        if (c == '&' && c2 == '&') { advance(); advance(); return Token(TokenType::TOK_AMPAMP, "&&", startLine, startCol, filename_); }
        if (c == '|' && c2 == '|') { advance(); advance(); return Token(TokenType::TOK_PIPEPIPE, "||", startLine, startCol, filename_); }
        if (c == '+' && c2 == ':') { advance(); advance(); return Token(TokenType::TOK_COLON_PLUS, "+:", startLine, startCol, filename_); }
        if (c == '-' && c2 == ':') { advance(); advance(); return Token(TokenType::TOK_COLON_MINUS, "-:", startLine, startCol, filename_); }
        if (c == '*' && c2 == '*') { advance(); advance(); return Token(TokenType::TOK_STARSTAR, "**", startLine, startCol, filename_); }
        if (c == '+' && c2 == '+') { advance(); advance(); return Token(TokenType::TOK_PLUSPLUS, "++", startLine, startCol, filename_); }
        if (c == '-' && c2 == '-') { advance(); advance(); return Token(TokenType::TOK_MINUSMINUS, "--", startLine, startCol, filename_); }
        if (c == '+' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_PLUSEQ, "+=", startLine, startCol, filename_); }
        if (c == '-' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_MINUSEQ, "-=", startLine, startCol, filename_); }
        if (c == '*' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_STAREQ, "*=", startLine, startCol, filename_); }
        if (c == '/' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_SLASHEQ, "/=", startLine, startCol, filename_); }
        if (c == '&' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_AMPEQ, "&=", startLine, startCol, filename_); }
        if (c == '|' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_PIPEEQ, "|=", startLine, startCol, filename_); }
        if (c == '^' && c2 == '=') { advance(); advance(); return Token(TokenType::TOK_CARETEQ, "^=", startLine, startCol, filename_); }
    }

    advance();
    switch (c) {
        case '+': return Token(TokenType::TOK_PLUS, "+", startLine, startCol, filename_);
        case '-': return Token(TokenType::TOK_MINUS, "-", startLine, startCol, filename_);
        case '*': return Token(TokenType::TOK_STAR, "*", startLine, startCol, filename_);
        case '/': return Token(TokenType::TOK_SLASH, "/", startLine, startCol, filename_);
        case '%': return Token(TokenType::TOK_PERCENT, "%", startLine, startCol, filename_);
        case '&': return Token(TokenType::TOK_AMP, "&", startLine, startCol, filename_);
        case '|': return Token(TokenType::TOK_PIPE, "|", startLine, startCol, filename_);
        case '^': return Token(TokenType::TOK_CARET, "^", startLine, startCol, filename_);
        case '~': return Token(TokenType::TOK_TILDE, "~", startLine, startCol, filename_);
        case '!': return Token(TokenType::TOK_EXCLAIM, "!", startLine, startCol, filename_);
        case '?': return Token(TokenType::TOK_QUESTION, "?", startLine, startCol, filename_);
        case ':': return Token(TokenType::TOK_COLON, ":", startLine, startCol, filename_);
        case ';': return Token(TokenType::TOK_SEMICOLON, ";", startLine, startCol, filename_);
        case ',': return Token(TokenType::TOK_COMMA, ",", startLine, startCol, filename_);
        case '.': return Token(TokenType::TOK_DOT, ".", startLine, startCol, filename_);
        case '(': return Token(TokenType::TOK_LPAREN, "(", startLine, startCol, filename_);
        case ')': return Token(TokenType::TOK_RPAREN, ")", startLine, startCol, filename_);
        case '[': return Token(TokenType::TOK_LBRACKET, "[", startLine, startCol, filename_);
        case ']': return Token(TokenType::TOK_RBRACKET, "]", startLine, startCol, filename_);
        case '{': return Token(TokenType::TOK_LBRACE, "{", startLine, startCol, filename_);
        case '}': return Token(TokenType::TOK_RBRACE, "}", startLine, startCol, filename_);
        case '<': return Token(TokenType::TOK_LANGLE, "<", startLine, startCol, filename_);
        case '>': return Token(TokenType::TOK_RANGLE, ">", startLine, startCol, filename_);
        case '@': return Token(TokenType::TOK_AT, "@", startLine, startCol, filename_);
        case '#': return Token(TokenType::TOK_HASH, "#", startLine, startCol, filename_);
        case '=': return Token(TokenType::TOK_ASSIGN_OP, "=", startLine, startCol, filename_);
        default: return Token(TokenType::TOK_ERROR, std::string(1, c), startLine, startCol, filename_);
    }
}

Token Lexer::next() {
    // If there are buffered tokens from peek(), return and consume them first
    if (bufferPos_ < (int)tokenBuffer_.size()) {
        Token tok = tokenBuffer_[bufferPos_];
        bufferPos_++;
        // If we've consumed all buffered tokens, clear the buffer
        if (bufferPos_ >= (int)tokenBuffer_.size()) {
            tokenBuffer_.clear();
            bufferPos_ = 0;
        }
        return tok;
    }
    // No buffered tokens - read from source
    return readFromSource();
}

Token Lexer::readFromSource() {
    skipWhitespace();
    if (pos_ >= len_) return Token(TokenType::TOK_EOF, "", line_, column_, filename_);
    char c = current();
    if (std::isalpha(c) || c == '_' || c == '\\' || c == '$') return readIdentifier();
    if (std::isdigit(c) || (c == '\'' && pos_ + 1 < len_ && std::isdigit(code_[pos_ + 1]))) return readNumber();
    if (c == '"') return readString();
    if (std::ispunct(c)) return readOperator();
    advance();
    return Token(TokenType::TOK_ERROR, std::string(1, c), line_, column_, filename_);
}

Token Lexer::peek() {
    if (bufferPos_ >= (int)tokenBuffer_.size()) tokenBuffer_.push_back(readFromSource());
    return tokenBuffer_[bufferPos_];
}

Token Lexer::peekN(int n) {
    while (bufferPos_ + n >= (int)tokenBuffer_.size()) tokenBuffer_.push_back(readFromSource());
    return tokenBuffer_[bufferPos_ + n];
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    Token tok;
    do { tok = next(); tokens.push_back(tok); } while (tok.type != TokenType::TOK_EOF);
    return tokens;
}

TokenType Lexer::lookupKeyword(const std::string &name) {
    auto it = keywords_.find(name);
    return (it != keywords_.end()) ? it->second : TokenType::TOK_IDENTIFIER;
}

std::string tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::TOK_EOF: return "EOF";
        case TokenType::TOK_INTEGER: return "integer_literal";
        case TokenType::TOK_REAL: return "real_literal";
        case TokenType::TOK_STRING: return "string_literal";
        case TokenType::TOK_IDENTIFIER: return "identifier";
        case TokenType::TOK_SYS_TASK: return "system_task";
        case TokenType::TOK_MODULE: return "module";
        case TokenType::TOK_ENDMODULE: return "endmodule";
        case TokenType::TOK_INPUT: return "input";
        case TokenType::TOK_OUTPUT: return "output";
        case TokenType::TOK_WIRE: return "wire";
        case TokenType::TOK_REG: return "reg";
        case TokenType::TOK_LOGIC: return "logic";
        case TokenType::TOK_ASSIGN: return "assign";
        case TokenType::TOK_ALWAYS: return "always";
        case TokenType::TOK_BEGIN: return "begin";
        case TokenType::TOK_END: return "end";
        case TokenType::TOK_IF: return "if";
        case TokenType::TOK_ELSE: return "else";
        case TokenType::TOK_CASE: return "case";
        case TokenType::TOK_LPAREN: return "(";
        case TokenType::TOK_RPAREN: return ")";
        case TokenType::TOK_SEMICOLON: return ";";
        case TokenType::TOK_ERROR: return "error";
        default: return "token";
    }
}

} // namespace VerilogParser
