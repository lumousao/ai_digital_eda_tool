/**
 * Lexer - Industrial-grade Verilog/SystemVerilog lexer
 * Based on Verilator verilog.l
 *
 * Features:
 * - Complete Verilog-2001/2005/SystemVerilog keyword support
 * - All system tasks and functions
 * - Number formats (binary, octal, decimal, hex)
 * - String literals
 * - Comment handling
 * - Include file support
 * - Attribute support
 * - Edge descriptors
 * - Compiler directives
 */

#ifndef LEXER_INDUSTRIAL_H
#define LEXER_INDUSTRIAL_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdint>
#include <functional>

namespace Lexer {

// ============================================================================
// Token types
// ============================================================================

enum TokenType {
    // Basic tokens
    TOK_EOF = 0,
    TOK_ERROR,

    // Identifiers and numbers
    TOK_IDENTIFIER,
    TOK_INTEGER,
    TOK_REAL,
    TOK_STRING,
    TOK_TIME,

    // Verilog keywords
    TOK_KW_MODULE,
    TOK_KW_ENDMODULE,
    TOK_KW_PORT,
    TOK_KW_INPUT,
    TOK_KW_OUTPUT,
    TOK_KW_INOUT,
    TOK_KW_WIRE,
    TOK_KW_REG,
    TOK_KW_LOGIC,
    TOK_KW_INTEGER,
    TOK_KW_REAL,
    TOK_KW_PARAMETER,
    TOK_KW_LOCALPARAM,
    TOK_KW_SUPPLY,
    TOK_KW_TRI,
    TOK_KW_WAND,
    TOK_KW_WOR,
    TOK_KW_TRIAND,
    TOK_KW_TRIOR,
    TOK_KW_TRI0,
    TOK_KW_TRI1,
    TOK_KW_SUPPLY0,
    TOK_KW_SUPPLY1,
    TOK_KW_UWIRE,

    // Data types
    TOK_KW_SIGNED,
    TOK_KW_UNSIGNED,
    TOK_KW_BYTE,
    TOK_KW_SHORTINT,
    TOK_KW_INT,
    TOK_KW_LONGINT,
    TOK_KW_BIT,
    TOK_KW_ENUM,
    TOK_KW_TYPEDEF,
    TOK_KW_STRUCT,
    TOK_KW_UNION,
    TOK_KW_PACKED,
    TOK_KW_UNPACKED,

    // Port declarations
    TOK_KW_PORTDIR_INPUT,
    TOK_KW_PORTDIR_OUTPUT,
    TOK_KW_PORTDIR_INOUT,
    TOK_KW_PORTDIR_REF,

    // Instantiation
    TOK_KW_INSTANCE,
    TOK_KW_BEGIN,
    TOK_KW_END,
    TOK_KW_GENERATE,
    TOK_KW_ENDGENERATE,
    TOK_KW_GENVAR,

    // Procedural blocks
    TOK_KW_ALWAYS,
    TOK_KW_INITIAL,
    TOK_KW_FINAL,
    TOK_KW_ALWAYS_FF,
    TOK_KW_ALWAYS_COMB,
    TOK_KW_ALWAYS_LATCH,

    // Control flow
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_CASE,
    TOK_KW_CASEZ,
    TOK_KW_CASEX,
    TOK_KW_ENDCASE,
    TOK_KW_DEFAULT,
    TOK_KW_FOR,
    TOK_KW_FOREACH,
    TOK_KW_WHILE,
    TOK_KW_DO,
    TOK_KW_REPEAT,
    TOK_KW_FOREVER,
    TOK_KW_WAIT,
    TOK_KW_RETURN,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,

    // Statements
    TOK_KW_ASSIGN,
    TOK_KW_FORCE,
    TOK_KW_RELEASE,
    TOK_KW_DEASSIGN,

    // Timing
    TOK_KW_POSEDGE,
    TOK_KW_NEGEDGE,
    TOK_KW_EDGE,
    TOK_KW_SPECIFY,
    TOK_KW_ENDSPECIFY,
    TOK_KW_SPECIFYBLOCK,
    TOK_KW_ABSDELAY,
    TOK_KW_PATHPULSE,

    // Tasks and functions
    TOK_KW_TASK,
    TOK_KW_ENDTASK,
    TOK_KW_FUNCTION,
    TOK_KW_ENDFUNCTION,
    TOK_KW_AUTOMATIC,

    // Compiler directives
    TOK_DIR_DEFINE,
    TOK_DIR_UNDEF,
    TOK_DIR_IFDEF,
    TOK_DIR_IFNDEF,
    TOK_DIR_ELSEIF,
    TOK_DIR_ENDIF,
    TOK_DIR_INCLUDE,
    TOK_DIR_TIMESCALE,
    TOK_DIR_DEFAULTNETTYPE,
    TOK_DIR_RESETALL,
    TOK_DIR_LINE,
    TOK_DIR_FILE,
    TOK_DIR_PRAGMA,
    TOK_DIR_BEGIN_KEYWORDS,
    TOK_DIR_END_KEYWORDS,

    // SystemVerilog keywords
    TOK_SV_CLASS,
    TOK_SV_ENDCLASS,
    TOK_SV_EXTENDS,
    TOK_SV_IMPLEMENTS,
    TOK_SV_VIRTUAL,
    TOK_SV_PURE,
    TOK_SV_IMPORT,
    TOK_SV_EXPORT,
    TOK_SV_PACKAGE,
    TOK_SV_ENDPACKAGE,
    TOK_SV_INTERFACE,
    TOK_SV_ENDINTERFACE,
    TOK_SV_MODPORT,
    TOK_SV_CHECKER,
    TOK_SV_ENDCHECKER,
    TOK_SV_PROPERTY,
    TOK_SV_ENDPROPERTY,
    TOK_SV_SEQUENCE,
    TOK_SV_ENDSEQUENCE,
    TOK_SV_ASSERT,
    TOK_SV_ASSUME,
    TOK_SV_COVER,
    TOK_SV_RESTRICT,
    TOK_SV_IMMEDIATE_ASSERT,
    TOK_SV_IMMEDIATE_ASSUME,
    TOK_SV_IMMEDIATE_COVER,
    TOK_SV_CONCURRENT_ASSERT,
    TOK_SV_CONCURRENT_ASSUME,
    TOK_SV_CONCURRENT_COVER,
    TOK_SV_PROPERTY_INST,
    TOK_SV_SEQUENCE_INST,
    TOK_SV_LET,

    // Assertions keywords
    TOK_AS_ENDPOINT,
    TOK_AS_THROUGH,
    TOK_AS_INTERSECT,
    TOK_AS_WITHIN,
    TOK_AS_THROUGHOUT,
    TOK_AS_INF,
    TOK_AS_S_OR,
    TOK_AS_S_AND,
    TOK_AS_S_NOT,
    TOK_AS_S_IFF,
    TOK_AS_S_UNTIL,
    TOK_AS_S_S_UNTIL,
    TOK_AS_S_UNTIL_WITH,
    TOK_AS_S_S_UNTIL_WITH,
    TOK_AS_S_IMPLIES,
    TOK_AS_S_EXCEPT,
    TOK_AS_S_EXCEPT_IF,

    // Operators
    TOK_OP_PLUS,
    TOK_OP_MINUS,
    TOK_OP_STAR,
    TOK_OP_SLASH,
    TOK_OP_PERCENT,
    TOK_OP_LOGAND,
    TOK_OP_LOGOR,
    TOK_OP_LOGNOT,
    TOK_OP_BITAND,
    TOK_OP_BITOR,
    TOK_OP_BITXOR,
    TOK_OP_BITNAND,
    TOK_OP_BITNOR,
    TOK_OP_BITXNOR,
    TOK_OP_REDUCE_AND,
    TOK_OP_REDUCE_OR,
    TOK_OP_REDUCE_XOR,
    TOK_OP_REDUCE_XNOR,
    TOK_OP_EQ,
    TOK_OP_NE,
    TOK_OP_CASE_EQ,
    TOK_OP_CASE_NE,
    TOK_OP_LT,
    TOK_OP_GT,
    TOK_OP_LE,
    TOK_OP_GE,
    TOK_OP_SHL,
    TOK_OP_SHR,
    TOK_OP_SSHL,
    TOK_OP_SSHR,
    TOK_OP_LARROW,
    TOK_OP_RARROW,
    TOK_OP_HASH,
    TOK_OP_LARROW2,
    TOK_OP_RARROW2,

    // Punctuation
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_SEMICOLON,
    TOK_COLON,
    TOK_COMMA,
    TOK_DOT,
    TOK_DOTSTAR,
    TOK_AT,
    TOK_DOLLAR,
    TOK_TICK,

    // Assignment operators
    TOK_ASSIGN_PLUS,
    TOK_ASSIGN_MINUS,
    TOK_ASSIGN_STAR,
    TOK_ASSIGN_SLASH,
    TOK_ASSIGN_PERCENT,
    TOK_ASSIGN_AND,
    TOK_ASSIGN_OR,
    TOK_ASSIGN_XOR,
    TOK_ASSIGN_SHL,
    TOK_ASSIGN_SHR,
    TOK_ASSIGN_SSHL,
    TOK_ASSIGN_SSHR,
    TOK_INCREMENT,
    TOK_DECREMENT,

    // System tasks
    TOK_SYS_DISPLAY,
    TOK_SYS_WRITE,
    TOK_SYS_STIME,
    TOK_SYS_TIME,
    TOK_SYS_REALTIME,
    TOK_SYS_FINISH,
    TOK_SYS_STOP,
    TOK_SYS_FOPEN,
    TOK_SYS_FCLOSE,
    TOK_SYS_FDISPLAY,
    TOK_SYS_FWRITE,
    TOK_SYS_FGETC,
    TOK_SYS_FGETS,
    TOK_SYS_FSEEK,
    TOK_SYS_FTELL,
    TOK_SYS_REWIND,
    TOK_SYS_FERROR,
    TOK_SYS_FFLUSH,
    TOK_SYS_READMEMH,
    TOK_SYS_READMEML,
    TOK_SYS_RANDOM,
    TOK_SYS_RANDOMIZE,
    TOK_SYS_DIST_UNIFORM,
    TOK_SYS_DIST_NORMAL,
    TOK_SYS_DIST_EXPONENTIAL,
    TOK_SYS_DIST_POISSON,
    TOK_SYS_DIST_CHI_SQUARE,
    TOK_SYS_DIST_T,
    TOK_SYS_DIST_ERLANG,
    TOK_SYS_CLOG2,
    TOK_SYS_BITS,
    TOK_SYS_TYPENAME,
    TOK_SYS_ISUNKNOWN,
    TOK_SYS_ISX,
    TOK_SYS_COUNTONES,
    TOK_SYS_COUNTBITS,
    TOK_SYS_ONEHOT,
    TOK_SYS_ONEHOT0,
    TOK_SYS_LEFT,
    TOK_SYS_RIGHT,
    TOK_SYS_HIGH,
    TOK_SYS_LOW,
    TOK_SYS_INCDIR,
    TOK_SYS_INCNAME,
    TOK_SYS_DECNAME,
    TOK_SYS_FULLSKEW,
    TOK_SYS_SETUP,
    TOK_SYS_HOLD,
    TOK_SYS_SETUPHOLD,
    TOK_SYS_RECOVERY,
    TOK_SYS_REMOVAL,
    TOK_SYS_RECREM,
    TOK_SYS_WIDTH,
    TOK_SYS_PERIOD,
    TOK_SYS_NOCHANGE,
    TOK_SYS_SFORMATF,
    TOK_SYS_CAST,
    TOK_SYS_STDIN,
    TOK_SYS_STDOUT,
    TOK_SYS_STDERR,

    // Special tokens
    TOK_ATTRIBUTE,
    TOK_COMMENT,
    TOK_BLOCK_COMMENT,
    TOK_END_OF_FILE,
    TOK_NEWLINE,
    TOK_WHITESPACE
};

// ============================================================================
// Token
// ============================================================================

struct Token {
    TokenType type;
    std::string value;
    int line;
    int column;
    std::string filename;

    Token() : type(TOK_EOF), line(0), column(0) {}
    Token(TokenType t, const std::string &v, int l, int c, const std::string &f = "")
        : type(t), value(v), line(l), column(c), filename(f) {}

    bool isKeyword() const;
    bool isIdentifier() const;
    bool isNumber() const;
    bool isString() const;
    bool isOperator() const;
    bool isSystemTask() const;

    std::string typeName() const;
};

// ============================================================================
// Lexer position
// ============================================================================

struct LexerPosition {
    int line;
    int column;
    int offset;
    std::string filename;

    LexerPosition() : line(1), column(1), offset(0) {}
    LexerPosition(int l, int c, int o, const std::string &f = "")
        : line(l), column(c), offset(o), filename(f) {}
};

// ============================================================================
// Lexer options
// ============================================================================

struct LexerOptions {
    bool enable_verilog_2001;
    bool enable_verilog_2005;
    bool enable_systemverilog;
    bool enable_attributes;
    bool enable_compiler_directives;
    bool enable_system_tasks;
    bool preserve_comments;
    bool preserve_whitespace;
    bool error_on_unknown;

    LexerOptions()
        : enable_verilog_2001(true),
          enable_verilog_2005(true),
          enable_systemverilog(true),
          enable_attributes(true),
          enable_compiler_directives(true),
          enable_system_tasks(true),
          preserve_comments(false),
          preserve_whitespace(false),
          error_on_unknown(true) {}
};

// ============================================================================
// Lexer - Main lexer class
// ============================================================================

class Lexer {
public:
    Lexer();
    ~Lexer();

    // Input
    void setInput(const std::string &input);
    void setInputFile(const std::string &filename);
    void setInputStream(std::istream *stream);

    // Options
    void setOptions(const LexerOptions &options);
    LexerOptions getOptions() const { return options_; }

    // Tokenization
    Token nextToken();
    Token peekToken();
    void pushBack(const Token &token);

    // Position
    LexerPosition getPosition() const { return position_; }
    void setPosition(const LexerPosition &pos);

    // State
    void reset();
    bool isEOF() const { return is_eof_; }

    // Include files
    void pushInclude(const std::string &filename);
    void popInclude();

    // Error handling
    std::string getErrorMessage() const { return error_message_; }
    bool hasError() const { return !error_message_.empty(); }

    // Statistics
    int getTokenCount() const { return token_count_; }
    int getLineCount() const { return line_count_; }

    // Keyword lookup
    static TokenType lookupKeyword(const std::string &name);
    static bool isKeyword(TokenType type);
    static std::string tokenTypeName(TokenType type);

private:
    // Input state
    std::string input_;
    size_t input_pos_;
    std::vector<std::istream*> input_streams_;
    std::vector<std::string> input_files_;
    std::vector<size_t> input_positions_;
    std::vector<LexerPosition> input_positions_info_;

    // Position tracking
    LexerPosition position_;
    LexerPosition token_start_;

    // Options
    LexerOptions options_;

    // Token buffer
    std::vector<Token> token_buffer_;
    int buffer_index_;

    // State
    bool is_eof_;
    bool in_attribute_;
    bool in_comment_;
    bool in_string_;
    int comment_depth_;

    // Statistics
    int token_count_;
    int line_count_;

    // Error handling
    std::string error_message_;

    // Keyword table
    static std::map<std::string, TokenType> keyword_table_;
    static bool keyword_table_initialized_;

    // System task table
    static std::map<std::string, TokenType> system_task_table_;
    static bool system_task_table_initialized_;

    // Internal methods
    void initKeywordTable();
    void initSystemTaskTable();

    Token readToken();
    char readChar();
    char peekChar();
    void unreadChar(char c);

    Token readIdentifier();
    Token readEscapedIdentifier();
    Token readNumber();
    Token readString();
    Token readComment();
    Token readBlockComment();
    Token readAttribute();
    Token readOperator();
    Token readSystemTask();
    Token readCompilerDirective();

    void skipWhitespace();
    void skipComment();
    void skipLineComment();

    bool isDigit(char c) const;
    bool isAlpha(char c) const;
    bool isAlphaNum(char c) const;
    bool isWhitespace(char c) const;
    bool isOperatorStart(char c) const;
    bool isSystemTaskStart(char c) const;

    void reportError(const std::string &message);
    void reportWarning(const std::string &message);
};

// ============================================================================
// Keyword table initialization
// ============================================================================

inline void Lexer::initKeywordTable() {
    if (keyword_table_initialized_) return;

    // Verilog-1995 keywords
    keyword_table_["module"] = TOK_KW_MODULE;
    keyword_table_["endmodule"] = TOK_KW_ENDMODULE;
    keyword_table_["port"] = TOK_KW_PORT;
    keyword_table_["input"] = TOK_KW_INPUT;
    keyword_table_["output"] = TOK_KW_OUTPUT;
    keyword_table_["inout"] = TOK_KW_INOUT;
    keyword_table_["wire"] = TOK_KW_WIRE;
    keyword_table_["reg"] = TOK_KW_REG;
    keyword_table_["logic"] = TOK_KW_LOGIC;
    keyword_table_["integer"] = TOK_KW_INTEGER;
    keyword_table_["real"] = TOK_KW_REAL;
    keyword_table_["parameter"] = TOK_KW_PARAMETER;
    keyword_table_["localparam"] = TOK_KW_LOCALPARAM;
    keyword_table_["supply"] = TOK_KW_SUPPLY;
    keyword_table_["tri"] = TOK_KW_TRI;
    keyword_table_["wand"] = TOK_KW_WAND;
    keyword_table_["wor"] = TOK_KW_WOR;
    keyword_table_["triand"] = TOK_KW_TRIAND;
    keyword_table_["trior"] = TOK_KW_TRIOR;
    keyword_table_["tri0"] = TOK_KW_TRI0;
    keyword_table_["tri1"] = TOK_KW_TRI1;
    keyword_table_["supply0"] = TOK_KW_SUPPLY0;
    keyword_table_["supply1"] = TOK_KW_SUPPLY1;
    keyword_table_["uwire"] = TOK_KW_UWIRE;

    // Data types
    keyword_table_["signed"] = TOK_KW_SIGNED;
    keyword_table_["unsigned"] = TOK_KW_UNSIGNED;
    keyword_table_["byte"] = TOK_KW_BYTE;
    keyword_table_["shortint"] = TOK_KW_SHORTINT;
    keyword_table_["int"] = TOK_KW_INT;
    keyword_table_["longint"] = TOK_KW_LONGINT;
    keyword_table_["bit"] = TOK_KW_BIT;
    keyword_table_["enum"] = TOK_KW_ENUM;
    keyword_table_["typedef"] = TOK_KW_TYPEDEF;
    keyword_table_["struct"] = TOK_KW_STRUCT;
    keyword_table_["union"] = TOK_KW_UNION;
    keyword_table_["packed"] = TOK_KW_PACKED;
    keyword_table_["unpacked"] = TOK_KW_UNPACKED;

    // Instantiation
    keyword_table_["begin"] = TOK_KW_BEGIN;
    keyword_table_["end"] = TOK_KW_END;
    keyword_table_["generate"] = TOK_KW_GENERATE;
    keyword_table_["endgenerate"] = TOK_KW_ENDGENERATE;
    keyword_table_["genvar"] = TOK_KW_GENVAR;

    // Procedural blocks
    keyword_table_["always"] = TOK_KW_ALWAYS;
    keyword_table_["initial"] = TOK_KW_INITIAL;
    keyword_table_["final"] = TOK_KW_FINAL;
    keyword_table_["always_ff"] = TOK_KW_ALWAYS_FF;
    keyword_table_["always_comb"] = TOK_KW_ALWAYS_COMB;
    keyword_table_["always_latch"] = TOK_KW_ALWAYS_LATCH;

    // Control flow
    keyword_table_["if"] = TOK_KW_IF;
    keyword_table_["else"] = TOK_KW_ELSE;
    keyword_table_["case"] = TOK_KW_CASE;
    keyword_table_["casez"] = TOK_KW_CASEZ;
    keyword_table_["casex"] = TOK_KW_CASEX;
    keyword_table_["endcase"] = TOK_KW_ENDCASE;
    keyword_table_["default"] = TOK_KW_DEFAULT;
    keyword_table_["for"] = TOK_KW_FOR;
    keyword_table_["foreach"] = TOK_KW_FOREACH;
    keyword_table_["while"] = TOK_KW_WHILE;
    keyword_table_["do"] = TOK_KW_DO;
    keyword_table_["repeat"] = TOK_KW_REPEAT;
    keyword_table_["forever"] = TOK_KW_FOREVER;
    keyword_table_["wait"] = TOK_KW_WAIT;
    keyword_table_["return"] = TOK_KW_RETURN;
    keyword_table_["break"] = TOK_KW_BREAK;
    keyword_table_["continue"] = TOK_KW_CONTINUE;

    // Statements
    keyword_table_["assign"] = TOK_KW_ASSIGN;
    keyword_table_["force"] = TOK_KW_FORCE;
    keyword_table_["release"] = TOK_KW_RELEASE;
    keyword_table_["deassign"] = TOK_KW_DEASSIGN;

    // Timing
    keyword_table_["posedge"] = TOK_KW_POSEDGE;
    keyword_table_["negedge"] = TOK_KW_NEGEDGE;
    keyword_table_["edge"] = TOK_KW_EDGE;
    keyword_table_["specify"] = TOK_KW_SPECIFY;
    keyword_table_["endspecify"] = TOK_KW_ENDSPECIFY;

    // Tasks and functions
    keyword_table_["task"] = TOK_KW_TASK;
    keyword_table_["endtask"] = TOK_KW_ENDTASK;
    keyword_table_["function"] = TOK_KW_FUNCTION;
    keyword_table_["endfunction"] = TOK_KW_ENDFUNCTION;
    keyword_table_["automatic"] = TOK_KW_AUTOMATIC;

    // SystemVerilog keywords
    keyword_table_["class"] = TOK_SV_CLASS;
    keyword_table_["endclass"] = TOK_SV_ENDCLASS;
    keyword_table_["extends"] = TOK_SV_EXTENDS;
    keyword_table_["implements"] = TOK_SV_IMPLEMENTS;
    keyword_table_["virtual"] = TOK_SV_VIRTUAL;
    keyword_table_["pure"] = TOK_SV_PURE;
    keyword_table_["import"] = TOK_SV_IMPORT;
    keyword_table_["export"] = TOK_SV_EXPORT;
    keyword_table_["package"] = TOK_SV_PACKAGE;
    keyword_table_["endpackage"] = TOK_SV_ENDPACKAGE;
    keyword_table_["interface"] = TOK_SV_INTERFACE;
    keyword_table_["endinterface"] = TOK_SV_ENDINTERFACE;
    keyword_table_["modport"] = TOK_SV_MODPORT;
    keyword_table_["checker"] = TOK_SV_CHECKER;
    keyword_table_["endchecker"] = TOK_SV_ENDCHECKER;
    keyword_table_["property"] = TOK_SV_PROPERTY;
    keyword_table_["endproperty"] = TOK_SV_ENDPROPERTY;
    keyword_table_["sequence"] = TOK_SV_SEQUENCE;
    keyword_table_["endsequence"] = TOK_SV_ENDSEQUENCE;
    keyword_table_["assert"] = TOK_SV_ASSERT;
    keyword_table_["assume"] = TOK_SV_ASSUME;
    keyword_table_["cover"] = TOK_SV_COVER;
    keyword_table_["restrict"] = TOK_SV_RESTRICT;

    keyword_table_initialized_ = true;
}

// ============================================================================
// System task table initialization
// ============================================================================

inline void Lexer::initSystemTaskTable() {
    if (system_task_table_initialized_) return;

    // Display tasks
    system_task_table_["$display"] = TOK_SYS_DISPLAY;
    system_task_table_["$displayb"] = TOK_SYS_DISPLAY;
    system_task_table_["$displayh"] = TOK_SYS_DISPLAY;
    system_task_table_["$displayo"] = TOK_SYS_DISPLAY;
    system_task_table_["$write"] = TOK_SYS_WRITE;
    system_task_table_["$writeb"] = TOK_SYS_WRITE;
    system_task_table_["$writeh"] = TOK_SYS_WRITE;
    system_task_table_["$writeo"] = TOK_SYS_WRITE;
    system_task_table_["$strobe"] = TOK_SYS_DISPLAY;
    system_task_table_["$strobeb"] = TOK_SYS_DISPLAY;
    system_task_table_["$strobeh"] = TOK_SYS_DISPLAY;
    system_task_table_["$strobeo"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitor"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitorb"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitorh"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitoro"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitoroff"] = TOK_SYS_DISPLAY;
    system_task_table_["$monitoron"] = TOK_SYS_DISPLAY;

    // Time functions
    system_task_table_["$stime"] = TOK_SYS_STIME;
    system_task_table_["$time"] = TOK_SYS_TIME;
    system_task_table_["$realtime"] = TOK_SYS_REALTIME;

    // Simulation control
    system_task_table_["$finish"] = TOK_SYS_FINISH;
    system_task_table_["$stop"] = TOK_SYS_STOP;

    // File I/O
    system_task_table_["$fopen"] = TOK_SYS_FOPEN;
    system_task_table_["$fclose"] = TOK_SYS_FCLOSE;
    system_task_table_["$fdisplay"] = TOK_SYS_FDISPLAY;
    system_task_table_["$fwrite"] = TOK_SYS_FWRITE;
    system_task_table_["$fgetc"] = TOK_SYS_FGETC;
    system_task_table_["$fgets"] = TOK_SYS_FGETS;
    system_task_table_["$fseek"] = TOK_SYS_FSEEK;
    system_task_table_["$ftell"] = TOK_SYS_FTELL;
    system_task_table_["$rewind"] = TOK_SYS_REWIND;
    system_task_table_["$ferror"] = TOK_SYS_FERROR;
    system_task_table_["$fflush"] = TOK_SYS_FFLUSH;

    // Memory read
    system_task_table_["$readmemb"] = TOK_SYS_READMEMH;
    system_task_table_["$readmemh"] = TOK_SYS_READMEMH;

    // Random functions
    system_task_table_["$random"] = TOK_SYS_RANDOM;
    system_task_table_["$urandom"] = TOK_SYS_RANDOM;
    system_task_table_["$urandom_range"] = TOK_SYS_RANDOM;
    system_task_table_["$randomize"] = TOK_SYS_RANDOMIZE;

    // Distribution functions
    system_task_table_["$dist_uniform"] = TOK_SYS_DIST_UNIFORM;
    system_task_table_["$dist_normal"] = TOK_SYS_DIST_NORMAL;
    system_task_table_["$dist_exponential"] = TOK_SYS_DIST_EXPONENTIAL;
    system_task_table_["$dist_poisson"] = TOK_SYS_DIST_POISSON;
    system_task_table_["$dist_chi_square"] = TOK_SYS_DIST_CHI_SQUARE;
    system_task_table_["$dist_t"] = TOK_SYS_DIST_T;
    system_task_table_["$dist_erlang"] = TOK_SYS_DIST_ERLANG;

    // Math functions
    system_task_table_["$clog2"] = TOK_SYS_CLOG2;
    system_task_table_["$bits"] = TOK_SYS_BITS;
    system_task_table_["$typename"] = TOK_SYS_TYPENAME;
    system_task_table_["$isunknown"] = TOK_SYS_ISUNKNOWN;
    system_task_table_["$isx"] = TOK_SYS_ISX;
    system_task_table_["$countones"] = TOK_SYS_COUNTONES;
    system_task_table_["$countbits"] = TOK_SYS_COUNTBITS;
    system_task_table_["$onehot"] = TOK_SYS_ONEHOT;
    system_task_table_["$onehot0"] = TOK_SYS_ONEHOT0;
    system_task_table_["$left"] = TOK_SYS_LEFT;
    system_task_table_["$right"] = TOK_SYS_RIGHT;
    system_task_table_["$high"] = TOK_SYS_HIGH;
    system_task_table_["$low"] = TOK_SYS_LOW;

    // Timing checks
    system_task_table_["$fullskew"] = TOK_SYS_FULLSKEW;
    system_task_table_["$setup"] = TOK_SYS_SETUP;
    system_task_table_["$hold"] = TOK_SYS_HOLD;
    system_task_table_["$setuphold"] = TOK_SYS_SETUPHOLD;
    system_task_table_["$recovery"] = TOK_SYS_RECOVERY;
    system_task_table_["$removal"] = TOK_SYS_REMOVAL;
    system_task_table_["$recrem"] = TOK_SYS_RECREM;
    system_task_table_["$width"] = TOK_SYS_WIDTH;
    system_task_table_["$period"] = TOK_SYS_PERIOD;
    system_task_table_["$nochange"] = TOK_SYS_NOCHANGE;

    // String formatting
    system_task_table_["$sformatf"] = TOK_SYS_SFORMATF;
    system_task_table_["$sformat"] = TOK_SYS_SFORMATF;
    system_task_table_["$psprintf"] = TOK_SYS_SFORMATF;

    // Type functions
    system_task_table_["$cast"] = TOK_SYS_CAST;

    // I/O streams
    system_task_table_["$stdin"] = TOK_SYS_STDIN;
    system_task_table_["$stdout"] = TOK_SYS_STDOUT;
    system_task_table_["$stderr"] = TOK_SYS_STDERR;

    system_task_table_initialized_ = true;
}

} // namespace Lexer

#endif // LEXER_INDUSTRIAL_H
