/**
 * Lexer - Verilog/SystemVerilog lexer
 * Based on industry-standard lexer reference
 *
 * Complete implementation of all methods declared in lexer_full.h
 */

#include "lexer_full.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cassert>

namespace Lexer {

// ============================================================================
// Static member initialization
// ============================================================================

std::map<std::string, TokenType> Lexer::keyword_table_;
bool Lexer::keyword_table_initialized_ = false;
std::map<std::string, TokenType> Lexer::system_task_table_;
bool Lexer::system_task_table_initialized_ = false;

// ============================================================================
// Token implementation
// ============================================================================

bool Token::isKeyword() const {
    return type >= TOK_KW_MODULE && type <= TOK_KW_CONTINUE;
}

bool Token::isIdentifier() const {
    return type == TOK_IDENTIFIER;
}

bool Token::isNumber() const {
    return type == TOK_INTEGER || type == TOK_REAL;
}

bool Token::isString() const {
    return type == TOK_STRING;
}

bool Token::isOperator() const {
    return type >= TOK_OP_PLUS && type <= TOK_TICK;
}

bool Token::isSystemTask() const {
    return type >= TOK_SYS_DISPLAY && type <= TOK_SYS_STDERR;
}

std::string Token::typeName() const {
    return Lexer::tokenTypeName(type);
}

// ============================================================================
// Lexer implementation
// ============================================================================

Lexer::Lexer()
    : input_pos_(0), buffer_index_(0), is_eof_(false),
      in_attribute_(false), in_comment_(false), in_string_(false),
      comment_depth_(0), token_count_(0), line_count_(1) {
    initKeywordTable();
    initSystemTaskTable();
}

Lexer::~Lexer() = default;

void Lexer::setInput(const std::string &input) {
    input_ = input;
    input_pos_ = 0;
    is_eof_ = false;
    position_ = LexerPosition(1, 1, 0);
}

void Lexer::setInputFile(const std::string &filename) {
    std::ifstream file(filename);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        setInput(buffer.str());
        input_files_.push_back(filename);
    }
}

void Lexer::setInputStream(std::istream *stream) {
    if (stream) {
        std::stringstream buffer;
        buffer << stream->rdbuf();
        setInput(buffer.str());
    }
}

void Lexer::setOptions(const LexerOptions &options) {
    options_ = options;
}

Token Lexer::nextToken() {
    // Check buffer first
    if (buffer_index_ < (int)token_buffer_.size()) {
        return token_buffer_[buffer_index_++];
    }

    Token token;
    do {
        token = readToken();
    } while (token.type == TOK_WHITESPACE || token.type == TOK_NEWLINE);

    token_count_++;
    return token;
}

Token Lexer::peekToken() {
    if (buffer_index_ < (int)token_buffer_.size()) {
        return token_buffer_[buffer_index_];
    }

    Token token = nextToken();
    token_buffer_.push_back(token);
    buffer_index_--;
    return token;
}

void Lexer::pushBack(const Token &token) {
    token_buffer_.push_back(token);
    buffer_index_--;
}

void Lexer::setPosition(const LexerPosition &pos) {
    position_ = pos;
    input_pos_ = pos.offset;
}

void Lexer::reset() {
    input_pos_ = 0;
    is_eof_ = false;
    in_attribute_ = false;
    in_comment_ = false;
    in_string_ = false;
    comment_depth_ = 0;
    token_count_ = 0;
    line_count_ = 1;
    token_buffer_.clear();
    buffer_index_ = 0;
    error_message_.clear();
}

void Lexer::pushInclude(const std::string &filename) {
    input_streams_.push_back(nullptr);
    input_files_.push_back(filename);
    input_positions_.push_back(input_pos_);
    input_positions_info_.push_back(position_);

    // Open new file
    std::ifstream *file = new std::ifstream(filename);
    if (file->is_open()) {
        input_streams_.push_back(file);
    } else {
        delete file;
        reportError("Cannot open include file: " + filename);
    }
}

void Lexer::popInclude() {
    if (!input_streams_.empty()) {
        std::istream *stream = input_streams_.back();
        input_streams_.pop_back();
        delete stream;

        input_files_.pop_back();
        input_pos_ = input_positions_.back();
        input_positions_.pop_back();
        position_ = input_positions_info_.back();
        input_positions_info_.pop_back();
    }
}

// ============================================================================
// Internal methods
// ============================================================================

Token Lexer::readToken() {
    skipWhitespace();

    if (is_eof_) {
        return Token(TOK_EOF, "", position_.line, position_.column, position_.filename);
    }

    char c = readChar();
    token_start_ = position_;

    // Handle special characters
    if (c == '/' && peekChar() == '/') {
        skipLineComment();
        return readToken();
    }

    if (c == '/' && peekChar() == '*') {
        readBlockComment();
        return readToken();
    }

    if (c == '`') {
        return readCompilerDirective();
    }

    if (c == '"') {
        return readString();
    }

    if (c == '@') {
        return Token(TOK_AT, "@", token_start_.line, token_start_.column, token_start_.filename);
    }

    if (c == '#') {
        return Token(TOK_OP_HASH, "#", token_start_.line, token_start_.column, token_start_.filename);
    }

    if (c == '$') {
        return readSystemTask();
    }

    if (c == '\\') {
        return readEscapedIdentifier();
    }

    // Identifiers and keywords
    if (isAlpha(c) || c == '_') {
        unreadChar(c);
        return readIdentifier();
    }

    // Numbers
    if (isDigit(c)) {
        unreadChar(c);
        return readNumber();
    }

    // Operators and punctuation
    return readOperator();
}

void Lexer::skipWhitespace() {
    while (!is_eof_) {
        char c = peekChar();
        if (c == ' ' || c == '\t' || c == '\r') {
            readChar();
        } else if (c == '\n') {
            readChar();
            line_count_++;
        } else if (c == '/' && peekChar() == '/') {
            skipLineComment();
        } else if (c == '/' && peekChar() == '*') {
            // Don't skip block comments - they might be needed
            break;
        } else {
            break;
        }
    }
}

void Lexer::skipLineComment() {
    while (!is_eof_) {
        char c = readChar();
        if (c == '\n' || c == '\r') {
            line_count_++;
            return;
        }
    }
}

void Lexer::skipComment() {
    while (!is_eof_) {
        char c = readChar();
        if (c == '\n') {
            line_count_++;
        }
        if (c == '*' && peekChar() == '/') {
            readChar();
            return;
        }
    }
}

Token Lexer::readIdentifier() {
    std::string id;
    char c;

    while ((c = peekChar()) && (isAlphaNum(c) || c == '_' || c == '$')) {
        id += readChar();
    }

    // Check if it's a keyword
    TokenType type = lookupKeyword(id);
    if (type != TOK_IDENTIFIER) {
        return Token(type, id, token_start_.line, token_start_.column, token_start_.filename);
    }

    return Token(TOK_IDENTIFIER, id, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readEscapedIdentifier() {
    std::string id;
    id += readChar();  // backslash

    char c;
    while ((c = peekChar()) && !isspace(c)) {
        id += readChar();
    }

    return Token(TOK_IDENTIFIER, id, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readNumber() {
    std::string num;
    char c;

    // Handle Verilog number format: <width>'<base><value>
    if (peekChar() == '\'' || (peekChar() >= '0' && peekChar() <= '9' && peekChar() != '0')) {
        // Check for width
        while ((c = peekChar()) && (isDigit(c) || c == '_')) {
            num += readChar();
        }

        if (peekChar() == '\'') {
            num += readChar();  // tick

            // Base letter
            c = peekChar();
            if (c == 's' || c == 'S') {
                num += readChar();
                c = peekChar();
            }

            if (c == 'b' || c == 'B' || c == 'o' || c == 'O' ||
                c == 'd' || c == 'D' || c == 'h' || c == 'H') {
                num += readChar();

                // Value
                while ((c = peekChar()) && (isAlphaNum(c) || c == '_' || c == '?' || c == 'x' || c == 'X' || c == 'z' || c == 'Z')) {
                    num += readChar();
                }
            }
        }
    } else {
        // Simple decimal or real number
        while ((c = peekChar()) && (isDigit(c) || c == '_')) {
            num += readChar();
        }

        // Check for real number
        if (peekChar() == '.') {
            num += readChar();
            while ((c = peekChar()) && (isDigit(c) || c == '_')) {
                num += readChar();
            }
        }

        // Check for exponent
        if (peekChar() == 'e' || peekChar() == 'E') {
            num += readChar();
            if (peekChar() == '+' || peekChar() == '-') {
                num += readChar();
            }
            while ((c = peekChar()) && isDigit(c)) {
                num += readChar();
            }
        }
    }

    // Determine token type
    if (num.find('.') != std::string::npos ||
        num.find('e') != std::string::npos ||
        num.find('E') != std::string::npos) {
        return Token(TOK_REAL, num, token_start_.line, token_start_.column, token_start_.filename);
    }

    return Token(TOK_INTEGER, num, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readString() {
    std::string str;
    str += readChar();  // opening quote

    char c;
    while ((c = readChar()) && c != '"') {
        if (c == '\\') {
            char next = readChar();
            switch (next) {
                case 'n': str += '\n'; break;
                case 't': str += '\t'; break;
                case '\\': str += '\\'; break;
                case '"': str += '"'; break;
                default: str += next; break;
            }
        } else {
            str += c;
        }
    }

    str += '"';  // closing quote
    return Token(TOK_STRING, str, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readBlockComment() {
    std::string comment;
    comment += readChar();  // /
    comment += readChar();  // *

    char c;
    while ((c = readChar())) {
        comment += c;
        if (c == '*' && peekChar() == '/') {
            comment += readChar();
            break;
        }
        if (c == '\n') {
            line_count_++;
        }
    }

    if (options_.preserve_comments) {
        return Token(TOK_BLOCK_COMMENT, comment, token_start_.line, token_start_.column, token_start_.filename);
    }

    // Skip comment and return next token
    return readToken();
}

Token Lexer::readAttribute() {
    std::string attr;
    attr += readChar();  // (*

    char c;
    while ((c = readChar())) {
        if (c == '*' && peekChar() == ')') {
            attr += readChar();
            break;
        }
        attr += c;
        if (c == '\n') {
            line_count_++;
        }
    }

    return Token(TOK_ATTRIBUTE, attr, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readSystemTask() {
    std::string task;
    task += readChar();  // $

    char c;
    while ((c = peekChar()) && (isAlphaNum(c) || c == '_')) {
        task += readChar();
    }

    // Look up system task
    auto it = system_task_table_.find(task);
    if (it != system_task_table_.end()) {
        return Token(it->second, task, token_start_.line, token_start_.column, token_start_.filename);
    }

    return Token(TOK_IDENTIFIER, task, token_start_.line, token_start_.column, token_start_.filename);
}

Token Lexer::readOperator() {
    char c = peekChar();
    std::string op;

    switch (c) {
        case '+':
            op += readChar();
            if (peekChar() == '+') { op += readChar(); return Token(TOK_INCREMENT, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_PLUS, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_PLUS, op, token_start_.line, token_start_.column, token_start_.filename);

        case '-':
            op += readChar();
            if (peekChar() == '-') { op += readChar(); return Token(TOK_DECREMENT, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_MINUS, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_MINUS, op, token_start_.line, token_start_.column, token_start_.filename);

        case '*':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_STAR, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_STAR, op, token_start_.line, token_start_.column, token_start_.filename);

        case '/':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_SLASH, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_SLASH, op, token_start_.line, token_start_.column, token_start_.filename);

        case '%':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_PERCENT, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_PERCENT, op, token_start_.line, token_start_.column, token_start_.filename);

        case '&':
            op += readChar();
            if (peekChar() == '&') { op += readChar(); return Token(TOK_OP_LOGAND, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_AND, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_BITAND, op, token_start_.line, token_start_.column, token_start_.filename);

        case '|':
            op += readChar();
            if (peekChar() == '|') { op += readChar(); return Token(TOK_OP_LOGOR, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_OR, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_BITOR, op, token_start_.line, token_start_.column, token_start_.filename);

        case '^':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_XOR, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_BITXOR, op, token_start_.line, token_start_.column, token_start_.filename);

        case '~':
            op += readChar();
            return Token(TOK_OP_LOGNOT, op, token_start_.line, token_start_.column, token_start_.filename);

        case '=':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); if (peekChar() == '=') { op += readChar(); return Token(TOK_OP_CASE_EQ, op, token_start_.line, token_start_.column, token_start_.filename); } return Token(TOK_OP_EQ, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_EQ, op, token_start_.line, token_start_.column, token_start_.filename);

        case '!':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); if (peekChar() == '=') { op += readChar(); return Token(TOK_OP_CASE_NE, op, token_start_.line, token_start_.column, token_start_.filename); } return Token(TOK_OP_NE, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_LOGNOT, op, token_start_.line, token_start_.column, token_start_.filename);

        case '<':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_OP_LE, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '<') { op += readChar(); if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_SHL, op, token_start_.line, token_start_.column, token_start_.filename); } return Token(TOK_OP_SHL, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_LT, op, token_start_.line, token_start_.column, token_start_.filename);

        case '>':
            op += readChar();
            if (peekChar() == '=') { op += readChar(); return Token(TOK_OP_GE, op, token_start_.line, token_start_.column, token_start_.filename); }
            if (peekChar() == '>') { op += readChar(); if (peekChar() == '=') { op += readChar(); return Token(TOK_ASSIGN_SHR, op, token_start_.line, token_start_.column, token_start_.filename); } return Token(TOK_OP_SHR, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_OP_GT, op, token_start_.line, token_start_.column, token_start_.filename);

        case '(':
            op += readChar();
            if (peekChar() == '*') { op += readChar(); return readAttribute(); }
            return Token(TOK_LPAREN, op, token_start_.line, token_start_.column, token_start_.filename);

        case ')':
            op += readChar();
            return Token(TOK_RPAREN, op, token_start_.line, token_start_.column, token_start_.filename);

        case '[':
            op += readChar();
            return Token(TOK_LBRACKET, op, token_start_.line, token_start_.column, token_start_.filename);

        case ']':
            op += readChar();
            return Token(TOK_RBRACKET, op, token_start_.line, token_start_.column, token_start_.filename);

        case '{':
            op += readChar();
            return Token(TOK_LBRACE, op, token_start_.line, token_start_.column, token_start_.filename);

        case '}':
            op += readChar();
            return Token(TOK_RBRACE, op, token_start_.line, token_start_.column, token_start_.filename);

        case ';':
            op += readChar();
            return Token(TOK_SEMICOLON, op, token_start_.line, token_start_.column, token_start_.filename);

        case ':':
            op += readChar();
            if (peekChar() == ':') { op += readChar(); return Token(TOK_OP_LARROW2, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_COLON, op, token_start_.line, token_start_.column, token_start_.filename);

        case ',':
            op += readChar();
            return Token(TOK_COMMA, op, token_start_.line, token_start_.column, token_start_.filename);

        case '.':
            op += readChar();
            if (peekChar() == '*') { op += readChar(); return Token(TOK_DOTSTAR, op, token_start_.line, token_start_.column, token_start_.filename); }
            return Token(TOK_DOT, op, token_start_.line, token_start_.column, token_start_.filename);

        default:
            op += readChar();
            return Token(TOK_ERROR, op, token_start_.line, token_start_.column, token_start_.filename);
    }
}

Token Lexer::readCompilerDirective() {
    std::string directive;
    directive += readChar();  // `

    char c;
    while ((c = peekChar()) && (isAlphaNum(c) || c == '_')) {
        directive += readChar();
    }

    // Map directive to token type
    if (directive == "`define") return Token(TOK_DIR_DEFINE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`undef") return Token(TOK_DIR_UNDEF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`ifdef") return Token(TOK_DIR_IFDEF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`ifndef") return Token(TOK_DIR_IFNDEF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`elsif") return Token(TOK_DIR_ELSEIF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`else") return Token(TOK_DIR_ELSEIF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`endif") return Token(TOK_DIR_ENDIF, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`include") return Token(TOK_DIR_INCLUDE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`timescale") return Token(TOK_DIR_TIMESCALE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`default_nettype") return Token(TOK_DIR_DEFAULTNETTYPE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`resetall") return Token(TOK_DIR_RESETALL, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`line") return Token(TOK_DIR_LINE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`__FILE__") return Token(TOK_DIR_FILE, directive, token_start_.line, token_start_.column, token_start_.filename);
    if (directive == "`pragma") return Token(TOK_DIR_PRAGMA, directive, token_start_.line, token_start_.column, token_start_.filename);

    return Token(TOK_IDENTIFIER, directive, token_start_.line, token_start_.column, token_start_.filename);
}

char Lexer::readChar() {
    if (is_eof_) return '\0';

    char c = input_[input_pos_++];
    if (c == '\n') {
        line_count_++;
        position_.line++;
        position_.column = 1;
    } else {
        position_.column++;
    }
    position_.offset = input_pos_;

    if (input_pos_ >= input_.size()) {
        is_eof_ = true;
    }

    return c;
}

char Lexer::peekChar() {
    if (is_eof_) return '\0';
    return input_[input_pos_];
}

void Lexer::unreadChar(char c) {
    if (c == '\n') {
        line_count_--;
        position_.line--;
        position_.column = 1;
    } else {
        position_.column--;
    }
    input_pos_--;
    position_.offset = input_pos_;
    is_eof_ = false;
}

bool Lexer::isDigit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::isAlpha(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::isAlphaNum(char c) const {
    return isAlpha(c) || isDigit(c);
}

bool Lexer::isWhitespace(char c) const {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool Lexer::isOperatorStart(char c) const {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
           c == '&' || c == '|' || c == '^' || c == '~' ||
           c == '=' || c == '!' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == ';' || c == ':' ||
           c == ',' || c == '.' || c == '@' || c == '#';
}

bool Lexer::isSystemTaskStart(char c) const {
    return c == '$';
}

void Lexer::reportError(const std::string &message) {
    error_message_ = "Line " + std::to_string(position_.line) +
                     ", Column " + std::to_string(position_.column) +
                     ": " + message;
}

void Lexer::reportWarning(const std::string &message) {
    // Warnings are not stored, just printed
}

// ============================================================================
// Static methods
// ============================================================================

TokenType Lexer::lookupKeyword(const std::string &name) {
    if (!keyword_table_initialized_) {
        keyword_table_["module"] = TOK_KW_MODULE;
        keyword_table_["endmodule"] = TOK_KW_ENDMODULE;
        keyword_table_["input"] = TOK_KW_INPUT;
        keyword_table_["output"] = TOK_KW_OUTPUT;
        keyword_table_["inout"] = TOK_KW_INOUT;
        keyword_table_["wire"] = TOK_KW_WIRE;
        keyword_table_["reg"] = TOK_KW_REG;
        keyword_table_["logic"] = TOK_KW_LOGIC;
        keyword_table_["always"] = TOK_KW_ALWAYS;
        keyword_table_["initial"] = TOK_KW_INITIAL;
        keyword_table_["begin"] = TOK_KW_BEGIN;
        keyword_table_["end"] = TOK_KW_END;
        keyword_table_["if"] = TOK_KW_IF;
        keyword_table_["else"] = TOK_KW_ELSE;
        keyword_table_["case"] = TOK_KW_CASE;
        keyword_table_["endcase"] = TOK_KW_ENDCASE;
        keyword_table_["for"] = TOK_KW_FOR;
        keyword_table_["while"] = TOK_KW_WHILE;
        keyword_table_["posedge"] = TOK_KW_POSEDGE;
        keyword_table_["negedge"] = TOK_KW_NEGEDGE;
        keyword_table_["assign"] = TOK_KW_ASSIGN;
        keyword_table_["parameter"] = TOK_KW_PARAMETER;
        keyword_table_["localparam"] = TOK_KW_LOCALPARAM;
        keyword_table_["integer"] = TOK_KW_INTEGER;
        keyword_table_["real"] = TOK_KW_REAL;
        keyword_table_["signed"] = TOK_KW_SIGNED;
        keyword_table_["unsigned"] = TOK_KW_UNSIGNED;
        keyword_table_["function"] = TOK_KW_FUNCTION;
        keyword_table_["endfunction"] = TOK_KW_ENDFUNCTION;
        keyword_table_["task"] = TOK_KW_TASK;
        keyword_table_["endtask"] = TOK_KW_ENDTASK;
        keyword_table_["class"] = TOK_SV_CLASS;
        keyword_table_["endclass"] = TOK_SV_ENDCLASS;
        keyword_table_["package"] = TOK_SV_PACKAGE;
        keyword_table_["endpackage"] = TOK_SV_ENDPACKAGE;
        keyword_table_["interface"] = TOK_SV_INTERFACE;
        keyword_table_["endinterface"] = TOK_SV_ENDINTERFACE;
        keyword_table_["import"] = TOK_SV_IMPORT;
        keyword_table_["export"] = TOK_SV_EXPORT;
        keyword_table_["virtual"] = TOK_SV_VIRTUAL;
        keyword_table_["pure"] = TOK_SV_PURE;
        keyword_table_["assert"] = TOK_SV_ASSERT;
        keyword_table_["assume"] = TOK_SV_ASSUME;
        keyword_table_["cover"] = TOK_SV_COVER;
        keyword_table_["property"] = TOK_SV_PROPERTY;
        keyword_table_["sequence"] = TOK_SV_SEQUENCE;
        keyword_table_["struct"] = TOK_KW_STRUCT;
        keyword_table_["union"] = TOK_KW_UNION;
        keyword_table_["enum"] = TOK_KW_ENUM;
        keyword_table_["typedef"] = TOK_KW_TYPEDEF;
        keyword_table_["generate"] = TOK_KW_GENERATE;
        keyword_table_["endgenerate"] = TOK_KW_ENDGENERATE;
        keyword_table_["genvar"] = TOK_KW_GENVAR;
        keyword_table_["forever"] = TOK_KW_FOREVER;
        keyword_table_["repeat"] = TOK_KW_REPEAT;
        keyword_table_["wait"] = TOK_KW_WAIT;
        keyword_table_["force"] = TOK_KW_FORCE;
        keyword_table_["release"] = TOK_KW_RELEASE;
        keyword_table_["specify"] = TOK_KW_SPECIFY;
        keyword_table_["endspecify"] = TOK_KW_ENDSPECIFY;
        keyword_table_["default"] = TOK_KW_DEFAULT;
        keyword_table_["automatic"] = TOK_KW_AUTOMATIC;
        keyword_table_initialized_ = true;
    }
    auto it = keyword_table_.find(name);
    if (it != keyword_table_.end()) {
        return it->second;
    }
    return TOK_IDENTIFIER;
}

bool Lexer::isKeyword(TokenType type) {
    return type >= TOK_KW_MODULE && type <= TOK_KW_CONTINUE;
}

std::string Lexer::tokenTypeName(TokenType type) {
    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_IDENTIFIER: return "IDENTIFIER";
        case TOK_INTEGER: return "INTEGER";
        case TOK_REAL: return "REAL";
        case TOK_STRING: return "STRING";
        case TOK_TIME: return "TIME";

        // Keywords
        case TOK_KW_MODULE: return "MODULE";
        case TOK_KW_ENDMODULE: return "ENDMODULE";
        case TOK_KW_PORT: return "PORT";
        case TOK_KW_INPUT: return "INPUT";
        case TOK_KW_OUTPUT: return "OUTPUT";
        case TOK_KW_INOUT: return "INOUT";
        case TOK_KW_WIRE: return "WIRE";
        case TOK_KW_REG: return "REG";
        case TOK_KW_LOGIC: return "LOGIC";
        case TOK_KW_INTEGER: return "INTEGER";
        case TOK_KW_REAL: return "REAL";
        case TOK_KW_PARAMETER: return "PARAMETER";
        case TOK_KW_LOCALPARAM: return "LOCALPARAM";

        // Control flow
        case TOK_KW_IF: return "IF";
        case TOK_KW_ELSE: return "ELSE";
        case TOK_KW_CASE: return "CASE";
        case TOK_KW_ENDCASE: return "ENDCASE";
        case TOK_KW_FOR: return "FOR";
        case TOK_KW_WHILE: return "WHILE";

        // Procedural blocks
        case TOK_KW_ALWAYS: return "ALWAYS";
        case TOK_KW_INITIAL: return "INITIAL";
        case TOK_KW_BEGIN: return "BEGIN";
        case TOK_KW_END: return "END";

        // Operators
        case TOK_OP_PLUS: return "PLUS";
        case TOK_OP_MINUS: return "MINUS";
        case TOK_OP_STAR: return "STAR";
        case TOK_OP_SLASH: return "SLASH";
        case TOK_OP_EQ: return "EQ";
        case TOK_OP_NE: return "NE";
        case TOK_OP_LT: return "LT";
        case TOK_OP_GT: return "GT";
        case TOK_OP_LE: return "LE";
        case TOK_OP_GE: return "GE";

        // Punctuation
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_SEMICOLON: return "SEMICOLON";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";

        // System tasks
        case TOK_SYS_DISPLAY: return "SYS_DISPLAY";
        case TOK_SYS_WRITE: return "SYS_WRITE";
        case TOK_SYS_FINISH: return "SYS_FINISH";
        case TOK_SYS_STOP: return "SYS_STOP";

        default: return "UNKNOWN";
    }
}

} // namespace Lexer
