/**
 * Complete Lexer for Verilog-2001/2005/SystemVerilog
 *
 * References:
 * - Verilator src/V3Lex.cpp
 * - Yosys frontends/verilog/verilog_lexer.l
 */

#ifndef LEXER_H
#define LEXER_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <set>

namespace VerilogParser {

/* ========== Token Types ========== */
enum class TokenType {
    // EOF
    TOK_EOF,

    // Literals
    TOK_INTEGER,
    TOK_REAL,
    TOK_BINARY,
    TOK_OCTAL,
    TOK_HEX,
    TOK_STRING,

    // Identifiers
    TOK_IDENTIFIER,
    TOK_SYS_TASK,       // $display, $finish, etc.

    // Keywords - Verilog-2001/2005
    TOK_MODULE, TOK_ENDMODULE,
    TOK_INPUT, TOK_OUTPUT, TOK_INOUT,
    TOK_WIRE, TOK_REG, TOK_LOGIC, TOK_INTEGER_KW, TOK_REAL_KW, TOK_TIME,
    TOK_PARAMETER, TOK_LOCALPARAM,
    TOK_ASSIGN, TOK_ALWAYS, TOK_INITIAL,
    TOK_BEGIN, TOK_END,
    TOK_IF, TOK_ELSE,
    TOK_CASE, TOK_CASEX, TOK_CASEZ, TOK_ENDCASE,
    TOK_DEFAULT,
    TOK_FOR, TOK_WHILE, TOK_REPEAT, TOK_FOREVER, TOK_FOREACH,
    TOK_POSEDGE, TOK_NEGEDGE,
    TOK_AND_T, TOK_OR_T, TOK_NOT_T, TOK_XOR_T, TOK_XNOR_T, TOK_NAND_T, TOK_NOR_T,
    TOK_BUF, TOK_BUFIF0, TOK_BUFIF1, TOK_NOTIF0, TOK_NOTIF1,
    TOK_PULLUP, TOK_PULLDOWN,
    TOK_TRIREG, TOK_TRI, TOK_TRI0, TOK_TRI1,
    TOK_WAND_T, TOK_WOR_T,
    TOK_SUPPLY0, TOK_SUPPLY1,
    TOK_CMOS, TOK_RCMOS, TOK_NMOS, TOK_PMOS,
    TOK_TRANIF0, TOK_TRANIF1, TOK_RTRANIF0, TOK_RTRANIF1,
    TOK_TRAN, TOK_RTRAN,
    TOK_DEASSIGN, TOK_FORCE, TOK_RELEASE, TOK_WAIT_T,
    TOK_PRIMITIVE, TOK_ENDPRIMITIVE,
    TOK_TABLE, TOK_ENDTABLE,
    TOK_SPECIFY, TOK_ENDSPECIFY, TOK_SPECPARAM, TOK_DEFPARAM,
    TOK_FUNCTION, TOK_ENDFUNCTION,
    TOK_TASK, TOK_ENDTASK,
    TOK_GENERATE, TOK_ENDGENERATE, TOK_GENVAR,
    TOK_SIGNED, TOK_UNSIGNED,
    TOK_SMALL, TOK_MEDIUM, TOK_LARGE,
    TOK_WEAK0, TOK_WEAK1, TOK_STRONG0, TOK_STRONG1,
    TOK_HIGHZ0, TOK_HIGHZ1,

    // SystemVerilog keywords
    TOK_CLASS, TOK_ENDCLASS, TOK_EXTENDS, TOK_IMPLEMENTS,
    TOK_INTERFACE, TOK_ENDINTERFACE, TOK_MODPORT,
    TOK_PACKAGE, TOK_ENDPACKAGE,
    TOK_IMPORT, TOK_EXPORT, TOK_SCOPE, TOK_CONTEXT,
    TOK_ALWAYS_FF, TOK_ALWAYS_COMB, TOK_ALWAYS_LATCH,
    TOK_FINAL, TOK_TIMEUNIT, TOK_TIMEPRECISION,
    TOK_DEFAULT_NETTYPE, TOK_CELLDEFINE, TOK_ENDCELLDEFINE,
    TOK_ASSERT_T, TOK_ASSUME_T, TOK_COVER_T, TOK_COVERPOINT,
    TOK_PROPERTY, TOK_SEQUENCE, TOK_ENDPROPERTY, TOK_ENDSEQUENCE,
    TOK_LET, TOK_CONST, TOK_TYPE_T, TOK_TYPEDEF,
    TOK_STRUCT, TOK_UNION, TOK_PACKED, TOK_UNPACKED, TOK_ENUM,
    TOK_CHANDLE, TOK_VIRTUAL, TOK_STATIC, TOK_AUTOMATIC,
    TOK_PROTECTED, TOK_LOCAL_T, TOK_REF,
    TOK_RAND, TOK_RANDC, TOK_CONSTRAINT, TOK_UNIQUE, TOK_PRIORITY,
    TOK_CROSS, TOK_COVERGROUP, TOK_ENDGROUP,
    TOK_WITH_T, TOK_OPTION, TOK_SAMPLE,
    TOK_PROCESS, TOK_JOIN, TOK_JOIN_ANY, TOK_JOIN_NONE,
    TOK_FORK, TOK_DISABLE, TOK_FORKJOIN,
    TOK_MAILBOX, TOK_SEMAPHORE,
    TOK_STRING_T, TOK_EVENT_T, TOK_INT, TOK_BYTE,
    TOK_SHORTINT, TOK_LONGINT, TOK_BIT_T,
    TOK_INTF, TOK_TRI_T, TOK_TRIAND, TOK_TRIOR,
    TOK_WAND_K, TOK_WOR_K,
    TOK_TRIREG_T, TOK_TRI0_T, TOK_TRI1_T,
    TOK_WIRE_T, TOK_UWIRE,
    TOK_SUPPLY0_T, TOK_SUPPLY1_T,
    TOK_SMALL_T, TOK_MEDIUM_T, TOK_LARGE_T,
    TOK_VECTORED, TOK_SCALARED,

    // Operators
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_EXCLAIM,
    TOK_QUESTION, TOK_COLON, TOK_SEMICOLON, TOK_COMMA, TOK_DOT,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE, TOK_LANGLE, TOK_RANGLE, TOK_HASH, TOK_AT,

    // Assignment (=) - must be before two-char operators
    TOK_ASSIGN_OP,
    // Two-char operators
    TOK_EQ, TOK_NEQ, TOK_EQL, TOK_NEL,
    TOK_LEQ, TOK_GEQ, TOK_LTLT, TOK_GTGT, TOK_GTGTGT,
    TOK_CASE_EQ, TOK_CASE_NEQ,    // === and !==
    TOK_COLON_PLUS, TOK_COLON_MINUS, // +: and -: indexed part select
    TOK_AMPAMP, TOK_PIPEPIPE, TOK_CARAMP,
    TOK_STARSTAR, TOK_PLUSPLUS, TOK_MINUSMINUS,

    // Three-char operators
    TOK_LTLTEQ, TOK_GTGTEQ,

    // Assignment operators
    TOK_PLUSEQ, TOK_MINUSEQ, TOK_STAREQ, TOK_SLASHEQ, TOK_PERCENTEQ,
    TOK_AMPEQ, TOK_PIPEEQ, TOK_CARETEQ,

    // Compiler directives
    TOK_DEFINE, TOK_UNDEF, TOK_IFDEF, TOK_IFNDEF,
    TOK_ELSIF, TOK_ENDIF,
    TOK_INCLUDE, TOK_TIMESCALE,

    // Special
    TOK_COMMENT,
    TOK_ERROR,
};

/* ========== Token ========== */
struct Token {
    TokenType type;
    std::string value;
    int line;
    int column;
    std::string filename;

    Token() : type(TokenType::TOK_EOF), line(0), column(0) {}
    Token(TokenType t, const std::string &v, int l, int c, const std::string &fn = "")
        : type(t), value(v), line(l), column(c), filename(fn) {}
};

/* ========== Lexer Class ========== */
class Lexer {
public:
    Lexer(const char *code, size_t len, const std::string &filename = "<input>");
    ~Lexer();

    // Get next token
    Token next();

    // Peek at next token
    Token peek();

    // Peek at nth token (0-based)
    Token peekN(int n);

    // Get current location
    int getLine() const { return line_; }
    int getColumn() const { return column_; }
    std::string getFilename() const { return filename_; }

    // Enable/disable features
    void enableSystemVerilog(bool enable) { systemVerilog_ = enable; }
    void enableDebug(bool enable) { debug_ = enable; }

    // Get all tokens (for debugging)
    std::vector<Token> tokenize();

private:
    const char *code_;
    size_t len_;
    size_t pos_;
    int line_;
    int column_;
    std::string filename_;
    bool systemVerilog_;
    bool debug_;

    // Token buffer
    std::vector<Token> tokenBuffer_;
    int bufferPos_;

    // Read next token from source (bypassing buffer)
    Token readFromSource();

    // Keyword map
    static std::map<std::string, TokenType> keywords_;
    static std::set<std::string> systemTasks_;
    static bool keywordsInitialized_;

    void initKeywords();

    // Helper methods
    char current() const;
    char peekChar() const;
    void advance();
    void skipWhitespace();

    // Token reading methods
    Token readIdentifier();
    Token readNumber();
    Token readString();
    Token readOperator();

    // Keyword recognition
    TokenType lookupKeyword(const std::string &name);
};

// Get token type name as string
std::string tokenTypeName(TokenType type);

} // namespace VerilogParser

#endif /* LEXER_H */
