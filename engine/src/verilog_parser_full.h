/**
 * Complete Verilog-2001/2005/SystemVerilog Parser
 * 
 * References:
 * - Industry-standard SystemVerilog parser reference
 * - Industry-standard Verilog parser reference
 * - IEEE 1364-2005 (Verilog-2005)
 * - IEEE 1800-2012 (SystemVerilog)
 * 
 * This is a complete parser supporting all language features.
 */

#ifndef VERILOG_PARSER_FULL_H
#define VERILOG_PARSER_FULL_H

#include "lexer.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

// TokenType is defined in lexer.h inside namespace VerilogParser

namespace VerilogParser {

/* ========== Source Location ========== */
struct SourceLocation {
    std::string filename;
    int line;
    int column;
    
    SourceLocation() : line(0), column(0) {}
    SourceLocation(const std::string &fn, int l, int c) 
        : filename(fn), line(l), column(c) {}
};

/* ========== AST Node Types ========== */
enum class NodeType {
    // Module
    MODULE,
    MODULE_INSTANCE,
    MODULE_PORT,
    
    // Declarations
    WIRE_DECL,
    REG_DECL,
    LOGIC_DECL,
    INTEGER_DECL,
    REAL_DECL,
    TIME_DECL,
    PARAM_DECL,
    LOCALPARAM_DECL,

    // SystemVerilog type declarations
    TYPEDEF_DECL,
    ENUM_DECL,
    STRUCT_DECL,
    UNION_DECL,
    
    // Ports
    INPUT_PORT,
    OUTPUT_PORT,
    INOUT_PORT,
    
    // Statements
    ASSIGN,
    ALWAYS_BLOCK,
    INITIAL_BLOCK,
    GENERATE_BLOCK,
    BEGIN_STATEMENT,
    WAIT_FOR_EDGE,  // @(posedge signal) or @(negedge signal)
    FORKJOIN,       // fork ... join
    DISABLE_STMT,   // disable block_name

    // Control Flow
    IF_STATEMENT,
    CASE_STATEMENT,
    CASE_ITEM,
    FOR_LOOP,
    WHILE_LOOP,
    FOREVER_LOOP,
    FOREACH_LOOP,
    REPEAT_LOOP,
    
    // Expressions
    BINARY_OP,
    UNARY_OP,
    TERNARY_OP,
    CONCATENATION,
    PART_SELECT,
    BIT_SELECT,
    FUNCTION_CALL,
    TASK_CALL,
    
    // Literals
    NUMBER,
    STRING,
    IDENTIFIER,
    
    // SystemVerilog
    CLASS_DECL,
    INTERFACE_DECL,
    MODPORT_DECL,
    PACKAGE_DECL,
    FUNCTION_DECL,
    TASK_DECL,
    ASSERTION,
    COVERGROUP,
    
    // Generate
    GENERATE_IF,
    GENERATE_FOR,
    GENERATE_CASE,
    
    // Specify
    SPECIFY_BLOCK,
    SPECIFY_PATH,
    
    // UDP
    UDP_DECL,
    UDP_TABLE,
    
    // Compiler Directives
    DEFINE,
    IFDEF,
    INCLUDE,
    TIMESCALE,
};

/* ========== AST Node ========== */
struct ASTNode {
    NodeType type;
    SourceLocation loc;
    std::vector<std::shared_ptr<ASTNode>> children;
    std::map<std::string, std::string> attributes;
    
    ASTNode(NodeType t) : type(t) {}
    virtual ~ASTNode() = default;
    
    void addChild(std::shared_ptr<ASTNode> child) {
        children.push_back(child);
    }
    
    void setAttribute(const std::string &key, const std::string &value) {
        attributes[key] = value;
    }
};

/* ========== Module ========== */
struct ModuleDecl : ASTNode {
    std::string name;
    std::vector<std::shared_ptr<ASTNode>> parameters;
    std::vector<std::shared_ptr<ASTNode>> ports;
    std::vector<std::shared_ptr<ASTNode>> items;

    ModuleDecl() : ASTNode(NodeType::MODULE) {}
};

/* ========== Port ========== */
struct PortDecl : ASTNode {
    enum Direction { INPUT, OUTPUT, INOUT };
    Direction dir;
    std::string name;
    int width;
    bool isSigned;
    bool isReg;
    bool isLogic;
    
    PortDecl() : ASTNode(NodeType::MODULE_PORT), dir(INPUT), width(1), 
                 isSigned(false), isReg(false), isLogic(false) {}
};

/* ========== Expression ========== */
struct Expression : ASTNode {
    enum Op {
        // Binary
        ADD, SUB, MUL, DIV, MOD,
        AND, OR, XOR, XNOR, NAND, NOR,
        EQ, NE, EQX, NEX, CASE_EQ, CASE_NE,
        LT, GT, LE, GE,
        SHL, SHR, SLL, SRL, SSA, SRA,
        LAND, LOR,
        
        // Unary
        UMINUS, UPLUS, UAND, UOR, UXOR, UNOT, ULNOT, UNEG,
        
        // Ternary
        TERNARY,
        
        // Others
        CONCAT,
        PART_SELECT,
        BIT_SELECT,
        FUNCTION_CALL,
    };
    
    Op op;
    std::shared_ptr<ASTNode> left;
    std::shared_ptr<ASTNode> right;
    std::shared_ptr<ASTNode> third;  // For ternary
    
    Expression() : ASTNode(NodeType::BINARY_OP), op(ADD) {}
};

/* ========== Statement ========== */
struct Statement : ASTNode {
    std::vector<std::shared_ptr<ASTNode>> statements;
    
    Statement(NodeType t) : ASTNode(t) {}
};

/* ========== Always Block ========== */
struct AlwaysBlock : Statement {
    enum Sensitivity { POSEDGE, NEGEDGE, BOTH, LEVEL };
    Sensitivity sens;
    std::string clockSignal;
    std::vector<std::string> sensitivityList;
    
    AlwaysBlock() : Statement(NodeType::ALWAYS_BLOCK), sens(LEVEL) {}
};

/* ========== Generate Block ========== */
struct GenerateBlock : Statement {
    enum GenerateType { GEN_IF, GEN_FOR, GEN_CASE };
    GenerateType genType;
    std::shared_ptr<ASTNode> condition;
    std::shared_ptr<ASTNode> init;
    std::shared_ptr<ASTNode> update;
    
    GenerateBlock() : Statement(NodeType::GENERATE_BLOCK), genType(GEN_IF) {}
};

/* ========== Class Declaration (SystemVerilog) ========== */
struct ClassDecl : ASTNode {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::shared_ptr<ASTNode>> members;
    std::vector<std::string> extends;
    
    ClassDecl() : ASTNode(NodeType::CLASS_DECL) {}
};

/* ========== Interface Declaration (SystemVerilog) ========== */
struct InterfaceDecl : ASTNode {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::shared_ptr<ASTNode>> items;
    
    InterfaceDecl() : ASTNode(NodeType::INTERFACE_DECL) {}
};

/* ========== Package Declaration (SystemVerilog) ========== */
struct PackageDecl : ASTNode {
    std::string name;
    std::vector<std::shared_ptr<ASTNode>> items;
    
    PackageDecl() : ASTNode(NodeType::PACKAGE_DECL) {}
};

/* ========== Function Declaration ========== */
struct FunctionDecl : ASTNode {
    std::string name;
    std::string returnType;
    std::vector<std::shared_ptr<ASTNode>> parameters;
    std::vector<std::shared_ptr<ASTNode>> statements;
    
    FunctionDecl() : ASTNode(NodeType::FUNCTION_DECL) {}
};

/* ========== Task Declaration ========== */
struct TaskDecl : ASTNode {
    std::string name;
    std::vector<std::shared_ptr<ASTNode>> parameters;
    std::vector<std::shared_ptr<ASTNode>> statements;
    
    TaskDecl() : ASTNode(NodeType::TASK_DECL) {}
};

/* ========== Assertion (SystemVerilog) ========== */
struct Assertion : ASTNode {
    enum AssertionType { ASSERT, ASSUME, COVER };
    AssertionType assertType;
    std::string label;
    std::shared_ptr<ASTNode> property;
    
    Assertion() : ASTNode(NodeType::ASSERTION), assertType(ASSERT) {}
};

/* ========== Parser Error (guard against redefinition) ========== */
#ifndef VERILOG_PARSER_PARSE_ERROR_DEFINED
#define VERILOG_PARSER_PARSE_ERROR_DEFINED
struct ParseError {
    std::string filename;
    int line;
    int column;
    std::string message;
    std::string severity;

    ParseError() : line(0), column(0) {}
    ParseError(const std::string &msg, const std::string &fn = "", int l = 0, int c = 0, const std::string &sev = "error")
        : filename(fn), line(l), column(c), message(msg), severity(sev) {}
    ParseError(const std::string &msg, const SourceLocation &loc, const std::string &sev = "error")
        : filename(loc.filename), line(loc.line), column(loc.column), message(msg), severity(sev) {}
};
#endif // VERILOG_PARSER_PARSE_ERROR_DEFINED

/* ========== Parser Result ========== */
struct ParseResult {
    bool success;
    std::vector<std::shared_ptr<ASTNode>> modules;
    std::vector<ParseError> errors;
    std::vector<ParseError> warnings;
    
    ParseResult() : success(false) {}
};

/* ========== Parser Class ========== */
class Parser {
public:
    Parser();
    ~Parser();
    
    // Parse a file
    ParseResult parseFile(const std::string &filename);
    
    // Parse a string
    ParseResult parseString(const std::string &code, const std::string &name = "<input>");
    
    // Parse multiple files
    ParseResult parseFiles(const std::vector<std::string> &filenames);
    
    // Enable/disable features
    void enableSystemVerilog(bool enable) { systemVerilog_ = enable; }
    void enableDebug(bool enable) { debug_ = enable; }
    
    // Get statistics
    size_t getModuleCount() const { return modules_.size(); }
    size_t getErrorCount() const { return errors_.size(); }
    size_t getWarningCount() const { return warnings_.size(); }
    
private:
    // Lexer (standalone Lexer class from lexer.h)
    std::unique_ptr<VerilogParser::Lexer> lexer_;
    
    // Parser state
    bool systemVerilog_;
    bool debug_;
    int global_safety_counter_;  // Global safety counter to prevent infinite loops
    int global_max_tokens_;
    std::vector<std::shared_ptr<ASTNode>> modules_;
    std::vector<ParseError> errors_;
    std::vector<ParseError> warnings_;
    std::vector<std::shared_ptr<ASTNode>> pending_decls_;  // Extra declarations from comma-separated names

    // Safety check - call at the top of every while(true) loop
    bool should_continue() {
        return ++global_safety_counter_ <= global_max_tokens_;
    }
    
    // Parse methods
    std::shared_ptr<ModuleDecl> parseModule();
    std::shared_ptr<PortDecl> parsePort();
    std::shared_ptr<ASTNode> parseDeclaration();
    std::shared_ptr<ASTNode> parseStatement();
    std::shared_ptr<Expression> parseExpression();
    std::shared_ptr<Expression> parsePrimary();
    std::shared_ptr<ASTNode> parseModuleItem();
    std::shared_ptr<ASTNode> parseWireDecl();
    std::shared_ptr<ASTNode> parseRegDecl();
    std::shared_ptr<ASTNode> parseLogicDecl();
    std::shared_ptr<ASTNode> parseIntegerDecl();
    std::shared_ptr<ASTNode> parseRealDecl();
    std::shared_ptr<ASTNode> parseTimeDecl();
    std::shared_ptr<ASTNode> parseParameter();
    std::shared_ptr<ASTNode> parseImplicitParameter();
    std::shared_ptr<ASTNode> parseGenvarDecl();
    std::shared_ptr<ASTNode> parseAssign();
    std::shared_ptr<ASTNode> parseAlwaysBlock();
    std::shared_ptr<ASTNode> parseInitialBlock();
    std::shared_ptr<ASTNode> parseBlock();
    std::shared_ptr<ASTNode> parseIfStatement();
    std::shared_ptr<ASTNode> parseCaseStatement();
    std::shared_ptr<ASTNode> parseForLoop();
    std::shared_ptr<ASTNode> parseWhileLoop();
    std::shared_ptr<ASTNode> parseRepeatLoop();
    std::shared_ptr<ASTNode> parseForeverLoop();
    std::shared_ptr<ASTNode> parseForkStatement();
    std::shared_ptr<ASTNode> parseDisableStatement();
    std::shared_ptr<ASTNode> parseWaitStatement();
    std::shared_ptr<ASTNode> parseBlockingAssignment();
    std::shared_ptr<ASTNode> parseNonBlockingAssignment();
    std::shared_ptr<ASTNode> parseSVTypeDecl();  // typedef/enum/struct/union
    std::shared_ptr<ASTNode> parseSystemTaskCall();
    std::shared_ptr<ASTNode> parseFunctionCall();
    std::shared_ptr<ASTNode> parseModuleInstance();
    
    // SystemVerilog methods
    std::shared_ptr<ClassDecl> parseClass();
    std::shared_ptr<InterfaceDecl> parseInterface();
    std::shared_ptr<PackageDecl> parsePackage();
    std::shared_ptr<ASTNode> parseFunction();
    std::shared_ptr<ASTNode> parseTask();
    std::shared_ptr<ASTNode> parseAssertion();

    // Generate methods
    std::shared_ptr<ASTNode> parseGenerate();
    std::shared_ptr<ASTNode> parseGenerateItem();
    std::shared_ptr<ASTNode> parseGenerateIf();
    std::shared_ptr<ASTNode> parseGenerateFor();
    std::shared_ptr<ASTNode> parseGenerateCase();
    
    // Utility methods
    void error(const std::string &msg);
    void warning(const std::string &msg);
    void expect(TokenType type);
    bool match(TokenType type);
    bool check(TokenType type);
    
    // Expression parsing with precedence
    std::shared_ptr<Expression> parseExpression1();
    std::shared_ptr<Expression> parseExpression2();
    std::shared_ptr<Expression> parseExpression3();
    std::shared_ptr<Expression> parseExpression4();
    std::shared_ptr<Expression> parseExpression5();
    std::shared_ptr<Expression> parseExpression6();
    std::shared_ptr<Expression> parseExpression7();
    std::shared_ptr<Expression> parseExpression8();
    std::shared_ptr<Expression> parseExpression9();
    std::shared_ptr<Expression> parseExpression10();
    std::shared_ptr<Expression> parseExpression11();
};

} // namespace VerilogParser

#endif /* VERILOG_PARSER_FULL_H */
