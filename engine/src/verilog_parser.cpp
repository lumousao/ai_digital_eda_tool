/**
 * Verilog Parser - Simplified but robust parser for RTLIL conversion
 *
 * Handles Verilog-2001 module declarations, ports, wires, regs,
 * always/initial blocks, assign statements, and basic generate.
 * Designed to never crash - skip unrecognized constructs gracefully.
 */

#include "verilog_parser.h"
#include "rtlil.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

// Internal namespace to avoid ODR collision with full parser's Token/Lexer/Parser
namespace VerilogParserSimple {

// Local ParseError (different from VerilogParser::ParseError in the header)
struct ParseError {
    std::string filename;
    int line = 0;
    int column = 0;
    std::string message;
    std::string severity = "error";
};

enum TokenType {
    TOK_EOF, TOKIdentifier, TOKNumber, TOKString,
    TOKModule, TOKEndmodule, TOKInput, TOKOutput, TOKInout,
    TOKWire, TOKReg, TOKLogic, TOKInteger, TOKReal, TOKTime,
    TOKParameter, TOKLocalparam, TOKGenvar,
    TOKAssign, TOKAlways, TOKInitial, TOKBegin, TOKEnd,
    TOKIf, TOKElse, TOKCase, TOKCasex, TOKCasez, TOKEndcase, TOKDefault,
    TOKFor, TOKWhile, TOKRepeat, TOKForever,
    TOKPosedge, TOKNegedge, TOKOr, TOKSigned, TOKUnsigned,
    TOKGenerate, TOKEndgenerate,
    TOKFunction, TOKEndfunction, TOKTask, TOKEndtask,
    TOKLParen, TOKRParen, TOKLBrace, TOKRBrace, TOKLBracket, TOKRBracket,
    TOKSemicolon, TOKColon, TOKComma, TOKDot, TOKHash, TOKAt,
    TOKPlus, TOKMinus, TOKStar, TOKSlash, TOKPercent,
    TOKAmp, TOKPipe, TOKCaret, TOKTilde, TOKBang,
    TOKLT, TOKGT, TOKLE, TOKGE, TOK_EQ, TOK_NE,
    TOK_AND, TOK_OR, TOK_QMARK, TOK_COLON,
    TOKAssignOp, TOKNonBlock,
    TOK_SYS_TASK,  // $display, $finish, etc.
};

struct Token { TokenType type; std::string value; int line, col; };

class Lexer {
    const char *code_; size_t len_, pos_; int line_, col_; std::string fn_;
    bool peeked_; Token peek_val_;
public:
    Lexer(const char *c, size_t l, const char *f) : code_(c), len_(l), pos_(0), line_(1), col_(1), fn_(f), peeked_(false) {}
    Token next() { if (peeked_) { peeked_ = false; return peek_val_; } return nextToken(); }
    Token peek() { if (!peeked_) { peek_val_ = nextToken(); peeked_ = true; } return peek_val_; }
    const std::string &filename() const { return fn_; }
private:
    char cur() const { return pos_ < len_ ? code_[pos_] : '\0'; }
    char peekChar() const { return pos_ + 1 < len_ ? code_[pos_ + 1] : '\0'; }
    void advance() { if (pos_ < len_) { if (code_[pos_] == '\n') { line_++; col_ = 1; } else { col_++; } pos_++; } }
    void skipWS() {
        while (pos_ < len_) {
            char c = cur();
            if (c == ' ' || c == '\t' || c == '\r') { advance(); }
            else if (c == '\n') { advance(); }
            else if (c == '/' && peekChar() == '/') { while (pos_ < len_ && cur() != '\n') advance(); }
            else if (c == '/' && peekChar() == '*') { advance(); advance(); while (pos_ < len_) { if (cur() == '*' && peekChar() == '/') { advance(); advance(); break; } advance(); } }
            else break;
        }
    }
    Token nextToken() {
        skipWS(); if (pos_ >= len_) return {TOK_EOF, "", line_, col_};
        char c = cur(); int sl = line_, sc = col_;

        // Numbers: decimal, sized (4'd5, 32'hFF), binary (1'b0)
        if (std::isdigit(c) || (c == '\'' && pos_ + 1 < len_)) {
            std::string v;
            while (pos_ < len_ && (std::isdigit(cur()) || cur() == '_' || cur() == '\'' ||
                   cur() == 'x' || cur() == 'X' || cur() == 'z' || cur() == 'Z' ||
                   cur() == 'b' || cur() == 'B' || cur() == 'h' || cur() == 'H' ||
                   cur() == 'd' || cur() == 'D' || std::isxdigit(cur()))) {
                v += cur(); advance();
            }
            return {TOKNumber, v, sl, sc};
        }

        // Identifiers and keywords
        if (std::isalpha(c) || c == '_' || c == '\\' || c == '$') {
            std::string v;
            if (c == '\\') { v += cur(); advance(); while (pos_ < len_ && cur() != ' ' && cur() != '\n' && cur() != '\t') { v += cur(); advance(); } }
            else { while (pos_ < len_ && (std::isalnum(cur()) || cur() == '_' || cur() == '$')) { v += cur(); advance(); } }
            TokenType t = TOKIdentifier;
            if (v=="module") t=TOKModule; else if (v=="endmodule") t=TOKEndmodule;
            else if (v=="input") t=TOKInput; else if (v=="output") t=TOKOutput; else if (v=="inout") t=TOKInout;
            else if (v=="wire") t=TOKWire; else if (v=="reg") t=TOKReg; else if (v=="logic") t=TOKLogic;
            else if (v=="integer") t=TOKInteger; else if (v=="real") t=TOKReal; else if (v=="time") t=TOKTime;
            else if (v=="parameter") t=TOKParameter; else if (v=="localparam") t=TOKLocalparam;
            else if (v=="genvar") t=TOKGenvar;
            else if (v=="assign") t=TOKAssign; else if (v=="always") t=TOKAlways; else if (v=="initial") t=TOKInitial;
            else if (v=="begin") t=TOKBegin; else if (v=="end") t=TOKEnd;
            else if (v=="if") t=TOKIf; else if (v=="else") t=TOKElse;
            else if (v=="case") t=TOKCase; else if (v=="casex") t=TOKCasex; else if (v=="casez") t=TOKCasez;
            else if (v=="endcase") t=TOKEndcase; else if (v=="default") t=TOKDefault;
            else if (v=="for") t=TOKFor; else if (v=="while") t=TOKWhile;
            else if (v=="repeat") t=TOKRepeat; else if (v=="forever") t=TOKForever;
            else if (v=="posedge") t=TOKPosedge; else if (v=="negedge") t=TOKNegedge;
            else if (v=="or") t=TOKOr;
            else if (v=="signed") t=TOKSigned; else if (v=="unsigned") t=TOKUnsigned;
            else if (v=="generate") t=TOKGenerate; else if (v=="endgenerate") t=TOKEndgenerate;
            else if (v=="function") t=TOKFunction; else if (v=="endfunction") t=TOKEndfunction;
            else if (v=="task") t=TOKTask; else if (v=="endtask") t=TOKEndtask;
            return {t, v, sl, sc};
        }

        // Strings
        if (c == '"') {
            std::string v; advance();
            while (pos_ < len_ && cur() != '"') { if (cur() == '\\') { advance(); } v += cur(); advance(); }
            if (pos_ < len_) advance(); // closing quote
            return {TOKString, v, sl, sc};
        }

        // Operators and punctuation
        advance();
        switch (c) {
            case '(': return {TOKLParen,"(",sl,sc}; case ')': return {TOKRParen,")",sl,sc};
            case '{': return {TOKLBrace,"{",sl,sc}; case '}': return {TOKRBrace,"}",sl,sc};
            case '[': return {TOKLBracket,"[",sl,sc}; case ']': return {TOKRBracket,"]",sl,sc};
            case ';': return {TOKSemicolon,";",sl,sc}; case ':': return {TOKColon,":",sl,sc};
            case ',': return {TOKComma,",",sl,sc}; case '.': return {TOKDot,".",sl,sc};
            case '#': return {TOKHash,"#",sl,sc};
            case '@': return {TOKAt,"@",sl,sc};
            case '+': return {TOKPlus,"+",sl,sc}; case '-': return {TOKMinus,"-",sl,sc};
            case '*': return {TOKStar,"*",sl,sc}; case '/': return {TOKSlash,"/",sl,sc};
            case '%': return {TOKPercent,"%",sl,sc};
            case '&': if (peekChar()=='&') { advance(); return {TOK_AND,"&&",sl,sc}; } return {TOKAmp,"&",sl,sc};
            case '|': if (peekChar()=='|') { advance(); return {TOK_OR,"||",sl,sc}; } return {TOKPipe,"|",sl,sc};
            case '^': return {TOKCaret,"^",sl,sc}; case '~': return {TOKTilde,"~",sl,sc};
            case '!': if (peekChar()=='=') { advance(); return {TOK_NE,"!=",sl,sc}; } return {TOKBang,"!",sl,sc};
            case '<':
                if (peekChar()=='=') { advance(); return {TOKNonBlock,"<=",sl,sc}; }
                if (peekChar()=='<') { advance(); return {TOKLT,"<<",sl,sc}; }
                return {TOKLT,"<",sl,sc};
            case '>':
                if (peekChar()=='=') { advance(); return {TOKGE,">=",sl,sc}; }
                if (peekChar()=='>') { advance(); return {TOKGT,">>",sl,sc}; }
                return {TOKGT,">",sl,sc};
            case '=':
                if (peekChar()=='=') { advance(); return {TOK_EQ,"==",sl,sc}; }
                return {TOKAssignOp,"=",sl,sc};
            case '?': return {TOK_QMARK,"?",sl,sc};
            default: return {TOK_EOF, std::string(1,c), sl, sc};
        }
    }
};

class Parser {
    RTLIL::Design *design_; Lexer lexer_; RTLIL::Module *cur_; std::vector<ParseError> errors_;
    void error(const std::string &msg) { Token t=lexer_.peek(); ParseError e; e.message=msg; e.filename=lexer_.filename(); e.line=t.line; e.column=t.col; errors_.push_back(e); }
    Token advance() { return lexer_.next(); }

    // Skip to semicolon, respecting nesting
    void skipToSemi() {
        int depth = 0;
        for (int guard = 0; guard < 100000; guard++) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF || t.type == TOKEndmodule) break;
            if (t.type == TOKSemicolon && depth == 0) { advance(); return; }
            if (t.type == TOKLParen || t.type == TOKLBrace || t.type == TOKLBracket) depth++;
            if (t.type == TOKRParen || t.type == TOKRBrace || t.type == TOKRBracket) { if (depth > 0) depth--; }
            advance();
        }
    }

    // Skip a begin...end block
    void skipBlock() {
        int depth = 0;
        bool has_begin = (lexer_.peek().type == TOKBegin);
        if (has_begin) advance();
        for (int guard = 0; guard < 100000; guard++) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF) break;
            if (t.type == TOKEnd) {
                if (depth == 0) { advance(); return; }
                depth--;
            }
            if (t.type == TOKBegin || t.type == TOKLParen || t.type == TOKLBrace) depth++;
            advance();
        }
    }

    // Skip parenthesized expression list: (...)
    void skipParens() {
        int depth = 0;
        for (int guard = 0; guard < 100000; guard++) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF) break;
            if (t.type == TOKLParen) depth++;
            if (t.type == TOKRParen) {
                if (depth == 0) { advance(); return; }
                depth--;
            }
            advance();
        }
    }

    // Skip bracket expression: [...]
    void skipBracket() {
        int depth = 0;
        for (int guard = 0; guard < 100000; guard++) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF) break;
            if (t.type == TOKLBracket) depth++;
            if (t.type == TOKRBracket) {
                if (depth == 0) { advance(); return; }
                depth--;
            }
            advance();
        }
    }

    // Skip a statement (handles if/else, case, for, while, etc.)
    void skipStatement() {
        Token t = lexer_.peek();
        if (t.type == TOKBegin) { skipBlock(); return; }
        if (t.type == TOKIf) { skipIfStatement(); return; }
        if (t.type == TOKCase || t.type == TOKCasex || t.type == TOKCasez) { skipCaseStatement(); return; }
        if (t.type == TOKFor) { skipForLoop(); return; }
        if (t.type == TOKWhile) { skipWhileLoop(); return; }
        if (t.type == TOKRepeat || t.type == TOKForever) { advance(); skipStatement(); return; }
        skipToSemi();
    }

    void skipIfStatement() {
        advance(); // 'if'
        if (lexer_.peek().type == TOKLParen) skipParens();
        skipStatement();
        if (lexer_.peek().type == TOKElse) { advance(); skipStatement(); }
    }

    void skipCaseStatement() {
        advance(); // case/casex/casez
        if (lexer_.peek().type == TOKLParen) skipParens();
        for (int guard = 0; guard < 100000; guard++) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF || t.type == TOKEndcase) { if (t.type == TOKEndcase) advance(); return; }
            skipStatement();
        }
    }

    void skipForLoop() {
        advance(); // 'for'
        if (lexer_.peek().type == TOKLParen) {
            advance(); // '('
            skipToSemi(); // init
            skipToSemi(); // condition
            // increment - skip to matching ')'
            int depth = 1;
            for (int guard = 0; guard < 100000; guard++) {
                Token t = lexer_.peek();
                if (t.type == TOK_EOF) break;
                if (t.type == TOKRParen) { depth--; if (depth == 0) { advance(); break; } }
                if (t.type == TOKLParen) depth++;
                advance();
            }
        }
        skipStatement();
    }

    void skipWhileLoop() {
        advance(); // 'while'
        if (lexer_.peek().type == TOKLParen) skipParens();
        skipStatement();
    }

    // Parse port declaration: input/output/inout [wire/reg/logic] [signed] [range] name, name;
    void parsePortDecl(int dir) {
        advance(); // consume input/output/inout
        Token t = lexer_.peek();

        // Skip optional type qualifiers
        while (t.type == TOKLogic || t.type == TOKSigned || t.type == TOKUnsigned ||
               t.type == TOKWire || t.type == TOKReg || t.type == TOKInteger ||
               t.type == TOKReal || t.type == TOKTime) {
            advance(); t = lexer_.peek();
        }

        // Skip optional range [MSB:LSB]
        int width = 1;
        if (t.type == TOKLBracket) {
            advance();
            // Parse range - find matching ]
            int depth = 1;
            std::string range_str;
            while (depth > 0 && lexer_.peek().type != TOK_EOF) {
                t = advance();
                if (t.type == TOKLBracket) depth++;
                if (t.type == TOKRBracket) depth--;
                if (depth > 0) range_str += t.value;
            }
            // Parse width from range like "31:0" or "7:0"
            size_t colon = range_str.find(':');
            if (colon != std::string::npos) {
                try {
                    int msb = std::stoi(range_str.substr(0, colon));
                    int lsb = std::stoi(range_str.substr(colon + 1));
                    width = std::abs(msb - lsb) + 1;
                } catch (...) {
                    width = 32;
                }
            } else {
                // Single number like "WIDTH-1" - can't parse, default to 32
                width = 32;
            }
        }

        // Parse port names (comma-separated)
        while (true) {
            t = advance();
            if (t.type != TOKIdentifier || t.type == TOK_EOF) break;

            // Check if this is an array dimension
            if (lexer_.peek().type == TOKLBracket) {
                // Memory/array declaration - skip dimensions
                while (lexer_.peek().type == TOKLBracket) skipBracket();
            }

            RTLIL::Wire *w = cur_->addWire(RTLIL::IdString("$" + t.value), width);
            if (dir == 1) w->port_input_ = RTLIL::PD_INPUT;
            if (dir == 2) w->port_output_ = RTLIL::PD_OUTPUT;
            if (dir == 3) { w->port_input_ = RTLIL::PD_INPUT; w->port_output_ = RTLIL::PD_OUTPUT; }
            w->port_id_ = cur_->wire_count();

            t = lexer_.peek();
            if (t.type == TOKComma) { advance(); continue; }
            break;
        }
        t = lexer_.peek();
        if (t.type == TOKSemicolon) advance(); else skipToSemi();
    }

    // Parse wire declaration
    void parseWireDecl() {
        advance(); Token t = lexer_.peek();
        while (t.type == TOKSigned || t.type == TOKUnsigned) { advance(); t = lexer_.peek(); }
        int width = 1;
        if (t.type == TOKLBracket) {
            advance(); int d = 1;
            while (d > 0 && t.type != TOK_EOF) { t = advance(); if (t.type == TOKLBracket) d++; if (t.type == TOKRBracket) d--; }
            width = 32;
        }
        while (true) {
            t = advance();
            if (t.type != TOKIdentifier || t.type == TOK_EOF) break;
            // Handle array dimensions
            while (lexer_.peek().type == TOKLBracket) skipBracket();
            cur_->addWire(RTLIL::IdString("$" + t.value), width);
            t = lexer_.peek();
            if (t.type == TOKComma) { advance(); continue; }
            break;
        }
        t = lexer_.peek();
        if (t.type == TOKSemicolon) advance();
    }

    // Parse reg/logic declaration
    void parseRegDecl() {
        advance(); Token t = lexer_.peek();
        while (t.type == TOKSigned || t.type == TOKUnsigned) { advance(); t = lexer_.peek(); }
        int width = 1;
        if (t.type == TOKLBracket) {
            advance(); int d = 1;
            while (d > 0 && t.type != TOK_EOF) { t = advance(); if (t.type == TOKLBracket) d++; if (t.type == TOKRBracket) d--; }
            width = 32;
        }
        while (true) {
            t = advance();
            if (t.type != TOKIdentifier || t.type == TOK_EOF) break;
            // Handle array dimensions (reg [31:0] mem [0:255])
            while (lexer_.peek().type == TOKLBracket) skipBracket();
            RTLIL::Wire *e = cur_->findWire(RTLIL::IdString("$" + t.value));
            if (e) { e->width_ = width; } else { cur_->addWire(RTLIL::IdString("$" + t.value), width); }
            t = lexer_.peek();
            if (t.type == TOKComma) { advance(); continue; }
            break;
        }
        t = lexer_.peek();
        if (t.type == TOKSemicolon) advance();
    }

    // Parse parameter/localparam declaration
    void parseParameter() {
        advance(); // consume parameter/localparam
        // Skip optional type
        Token t = lexer_.peek();
        while (t.type == TOKSigned || t.type == TOKUnsigned || t.type == TOKInteger ||
               t.type == TOKReal || t.type == TOKLogic) { advance(); t = lexer_.peek(); }
        // Parse name = value
        while (true) {
            t = lexer_.peek();
            if (t.type != TOKIdentifier || t.type == TOK_EOF) break;
            advance(); // name
            t = lexer_.peek();
            if (t.type == TOKAssignOp) {
                advance(); // =
                // Skip value expression
                int depth = 0;
                for (int guard = 0; guard < 100000; guard++) {
                    t = lexer_.peek();
                    if (t.type == TOK_EOF) break;
                    if (t.type == TOKComma && depth == 0) break;
                    if (t.type == TOKSemicolon && depth == 0) break;
                    if (t.type == TOKLParen || t.type == TOKLBrace) depth++;
                    if (t.type == TOKRParen || t.type == TOKRBrace) { if (depth > 0) depth--; }
                    advance();
                }
            }
            t = lexer_.peek();
            if (t.type == TOKComma) { advance(); continue; }
            break;
        }
        t = lexer_.peek();
        if (t.type == TOKSemicolon) advance(); else skipToSemi();
    }

    // Parse assign statement
    void parseAssign() {
        advance(); // 'assign'
        // Skip the assignment expression
        skipToSemi();
        cur_->addCell(RTLIL::IdString("$assign"), RTLIL::IdString("$assign"));
    }

    // Parse always block
    void parseAlways() {
        advance(); // 'always'
        Token t = lexer_.peek();

        // Parse sensitivity list
        if (t.type == TOKAt) {
            advance(); // consume '@'
            t = lexer_.peek();
        }
        if (t.type == TOKLParen) {
            advance(); // consume '('
            // Skip sensitivity list: find matching ')'
            int depth = 1; // we already consumed '('
            for (int guard = 0; guard < 10000; guard++) {
                t = lexer_.peek();
                if (t.type == TOK_EOF) break;
                if (t.type == TOKLParen) depth++;
                if (t.type == TOKRParen) {
                    depth--;
                    if (depth == 0) { advance(); break; }
                }
                advance();
            }
        }

        // Parse body
        t = lexer_.peek();
        if (t.type == TOKBegin) { skipBlock(); }
        else { skipToSemi(); }

        cur_->addProcess(RTLIL::IdString("$proc" + std::to_string(cur_->process_count())));
    }

    // Parse module
    void parseModule() {
        advance(); // 'module'
        Token name = advance(); // module name
        cur_ = design_->addModule(RTLIL::IdString("$" + name.value));

        Token t = lexer_.peek();

        // Handle #(parameter ...) - skip parameter list
        if (t.type == TOKHash) {
            advance(); // '#'
            if (lexer_.peek().type == TOKLParen) {
                advance(); // '('
                // Skip parameter declarations inside #( ... )
                int depth = 1;
                for (int guard = 0; guard < 100000; guard++) {
                    t = lexer_.peek();
                    if (t.type == TOK_EOF) break;
                    if (t.type == TOKLParen) depth++;
                    if (t.type == TOKRParen) {
                        depth--;
                        if (depth == 0) { advance(); break; }
                    }
                    advance();
                }
            }
            t = lexer_.peek();
        }

        // Handle port list (...)
        if (t.type == TOKLParen) {
            advance(); // '('
            // Skip port names in module header
            int depth = 1;
            for (int guard = 0; guard < 100000; guard++) {
                t = lexer_.peek();
                if (t.type == TOK_EOF) break;
                if (t.type == TOKLParen) depth++;
                if (t.type == TOKRParen) {
                    depth--;
                    if (depth == 0) { advance(); break; }
                }
                advance();
            }
        }

        t = lexer_.peek();
        if (t.type == TOKSemicolon) advance();

        // Parse module body
        int max = 200000;
        while (max-- > 0) {
            t = lexer_.peek();
            if (t.type == TOKEndmodule || t.type == TOK_EOF) break;

            if (t.type == TOKInput)  { parsePortDecl(1); continue; }
            if (t.type == TOKOutput) { parsePortDecl(2); continue; }
            if (t.type == TOKInout)  { parsePortDecl(3); continue; }
            if (t.type == TOKWire)   { parseWireDecl(); continue; }
            if (t.type == TOKReg || t.type == TOKLogic) { parseRegDecl(); continue; }
            if (t.type == TOKInteger || t.type == TOKReal || t.type == TOKTime) { parseRegDecl(); continue; }
            if (t.type == TOKParameter || t.type == TOKLocalparam) { parseParameter(); continue; }
            if (t.type == TOKAssign) { parseAssign(); continue; }
            if (t.type == TOKAlways) { parseAlways(); continue; }
            if (t.type == TOKInitial) { advance(); skipBlock(); continue; }
            if (t.type == TOKGenvar) { skipToSemi(); continue; }
            if (t.type == TOKGenerate) { skipBlock(); continue; } // endgenerate handled by skipBlock
            if (t.type == TOKFunction) { skipToSemi(); /* skip function body */ skipBlock(); continue; }
            if (t.type == TOKTask) { skipToSemi(); skipBlock(); continue; }

            // Unknown token - skip to next semicolon or block
            error("Unexpected token in module body: " + t.value);
            advance();
        }

        if (lexer_.peek().type == TOKEndmodule) advance();
        cur_ = nullptr;
    }

public:
    Parser(RTLIL::Design *d, const char *c, size_t l, const char *f)
        : design_(d), lexer_(c, l, f), cur_(nullptr) {}

    std::vector<ParseError> parse() {
        int m = 100000;
        while (m-- > 0) {
            Token t = lexer_.peek();
            if (t.type == TOK_EOF) break;
            if (t.type == TOKModule) { parseModule(); continue; }
            // Skip anything outside modules
            advance();
        }
        return errors_;
    }
};

} // namespace VerilogParserSimple

// Public API in VerilogParser namespace (declared in verilog_parser.h)
namespace VerilogParser {

std::vector<ParseError> parse_file(RTLIL::Design *design, const char *filename) {
    std::ifstream f(filename);
    if (!f.is_open()) {
        ParseError e; e.message = "Cannot open: " + std::string(filename); e.filename = filename; return {e};
    }
    std::stringstream buf; buf << f.rdbuf(); std::string content = buf.str();
    return parse_string(design, content.c_str(), content.size(), filename);
}

std::vector<ParseError> parse_string(RTLIL::Design *design, const char *code, size_t len, const char *name) {
    VerilogParserSimple::Parser p(design, code, len, name);
    auto simple_errors = p.parse();
    // Convert SimpleParseError to VerilogParser::ParseError
    std::vector<ParseError> errors;
    for (auto &e : simple_errors) {
        ParseError pe;
        pe.filename = e.filename;
        pe.line = e.line;
        pe.column = e.column;
        pe.message = e.message;
        pe.severity = e.severity;
        errors.push_back(pe);
    }
    return errors;
}

} // namespace VerilogParser
