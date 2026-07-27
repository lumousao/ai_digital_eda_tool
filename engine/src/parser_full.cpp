/**
 * Complete Verilog-2001/2005/SystemVerilog Parser
 * 
 * References:
 * - industry-standard grammar
 * - industry-standard Verilog parser
 * - IEEE 1364-2005 (Verilog-2005)
 * - IEEE 1800-2012 (SystemVerilog)
 */

#include "verilog_parser_full.h"
#include "lexer.h"
#include "preprocessor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace VerilogParser {


// Helper to convert TokenType to string
std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::TOK_MODULE: return "module";
        case TokenType::TOK_ENDMODULE: return "endmodule";
        case TokenType::TOK_INPUT: return "input";
        case TokenType::TOK_OUTPUT: return "output";
        case TokenType::TOK_INOUT: return "inout";
        case TokenType::TOK_WIRE: return "wire";
        case TokenType::TOK_REG: return "reg";
        case TokenType::TOK_LOGIC: return "logic";
        case TokenType::TOK_INTEGER: return "integer";
        case TokenType::TOK_REAL: return "real";
        case TokenType::TOK_TIME: return "time";
        case TokenType::TOK_PARAMETER: return "parameter";
        case TokenType::TOK_LOCALPARAM: return "localparam";
        case TokenType::TOK_ASSIGN: return "assign";
        case TokenType::TOK_ALWAYS: return "always";
        case TokenType::TOK_INITIAL: return "initial";
        case TokenType::TOK_BEGIN: return "begin";
        case TokenType::TOK_END: return "end";
        case TokenType::TOK_IF: return "if";
        case TokenType::TOK_ELSE: return "else";
        case TokenType::TOK_CASE: return "case";
        case TokenType::TOK_ENDCASE: return "endcase";
        case TokenType::TOK_DEFAULT: return "default";
        case TokenType::TOK_FOR: return "for";
        case TokenType::TOK_WHILE: return "while";
        case TokenType::TOK_REPEAT: return "repeat";
        case TokenType::TOK_FOREVER: return "forever";
        case TokenType::TOK_POSEDGE: return "posedge";
        case TokenType::TOK_NEGEDGE: return "negedge";
        case TokenType::TOK_FUNCTION: return "function";
        case TokenType::TOK_ENDFUNCTION: return "endfunction";
        case TokenType::TOK_TASK: return "task";
        case TokenType::TOK_ENDTASK: return "endtask";
        case TokenType::TOK_GENERATE: return "generate";
        case TokenType::TOK_ENDGENERATE: return "endgenerate";
        case TokenType::TOK_GENVAR: return "genvar";
        case TokenType::TOK_SIGNED: return "signed";
        case TokenType::TOK_UNSIGNED: return "unsigned";
        case TokenType::TOK_IDENTIFIER: return "identifier";
        case TokenType::TOK_SYS_TASK: return "system_task";
        case TokenType::TOK_INTEGER_KW: return "integer_literal";
        case TokenType::TOK_STRING: return "string_literal";
        case TokenType::TOK_EOF: return "EOF";
        default: return "unknown";
    }
}

Parser::Parser() : systemVerilog_(true), debug_(false) {
}

Parser::~Parser() {
}

ParseResult Parser::parseFile(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        ParseResult result;
        result.success = false;
        result.errors.push_back(ParseError("Cannot open file: " + filename,
            SourceLocation(filename, 0, 0)));
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string code = buffer.str();

    // Run preprocessor
    Preprocessor pp;
    // Add parent directory of source file as include path
    size_t last_slash = filename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        pp.addIncludePath(filename.substr(0, last_slash));
    }
    // Also add current directory
    pp.addIncludePath(".");
    code = pp.process(code, filename);

    return parseString(code, filename);
}

ParseResult Parser::parseString(const std::string &code, const std::string &name) {
    ParseResult result;

    // Run preprocessor
    Preprocessor pp;
    std::string processed = pp.process(code, name);

    // Create lexer (standalone Lexer class from lexer.h)
    lexer_ = std::make_unique<VerilogParser::Lexer>(processed.c_str(), processed.size(), name);
    lexer_->enableSystemVerilog(systemVerilog_);
    lexer_->enableDebug(debug_);

    // Global safety counter for all loops
    global_safety_counter_ = 0;
    global_max_tokens_ = 500000; // Safety limit for entire parse

    // Parse
    try {
        while (should_continue()) {
            Token tok = lexer_->peek();

            if (tok.type == TokenType::TOK_EOF) {
                break;
            }
            
            if (tok.type == TokenType::TOK_MODULE) {
                auto module = parseModule();
                if (module) {
                    result.modules.push_back(module);
                }
            } else if (systemVerilog_ && tok.type == TokenType::TOK_INTERFACE) {
                auto iface = parseInterface();
                if (iface) {
                    result.modules.push_back(iface);
                }
            } else if (systemVerilog_ && tok.type == TokenType::TOK_PACKAGE) {
                auto pkg = parsePackage();
                if (pkg) {
                    result.modules.push_back(pkg);
                }
            } else if (systemVerilog_ && tok.type == TokenType::TOK_CLASS) {
                auto cls = parseClass();
                if (cls) {
                    result.modules.push_back(cls);
                }
            } else if (systemVerilog_ && tok.type == TokenType::TOK_COVERGROUP) {
                // Parse covergroup: parse until endgroup as a module-level item
                auto cg = std::make_shared<ASTNode>(NodeType::COVERGROUP);
                lexer_->next(); // consume 'covergroup'
                Token tok = lexer_->next(); // name
                if (tok.type == TokenType::TOK_IDENTIFIER) {
                    cg->setAttribute("name", tok.value);
                }
                // Skip until 'endgroup' keyword
                while (should_continue()) {
                    Token t = lexer_->peek();
                    if (t.type == TokenType::TOK_ENDGROUP) { lexer_->next(); break; }
                    if (t.type == TokenType::TOK_EOF) break;
                    lexer_->next();
                }
                result.modules.push_back(cg);
            } else {
                // Skip unknown token
                lexer_->next();
            }
        }
        
        // Check if safety limit was hit
        if (global_safety_counter_ > global_max_tokens_) {
            errors_.push_back(ParseError("Parser safety limit exceeded - possible infinite loop in source code", name, 0, 0, "error"));
        }

        result.success = errors_.empty();
        result.errors = errors_;
        result.warnings = warnings_;
        
    } catch (const std::exception &e) {
        result.success = false;
        result.errors.push_back(ParseError(e.what(), 
            SourceLocation(name, lexer_->getLine(), lexer_->getColumn())));
    }
    
    return result;
}

std::shared_ptr<ModuleDecl> Parser::parseModule() {
    auto module = std::make_shared<ModuleDecl>();

    // Consume 'module' keyword
    Token tok = lexer_->next();
    int module_line = tok.line;
    
    if (tok.type != TokenType::TOK_MODULE) {
        error("Expected 'module'");
        return nullptr;
    }
    
    // Parse module name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected module name");
        return nullptr;
    }
    module->name = tok.value;

    // Parse optional parameters
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_HASH) {
        lexer_->next(); // consume '#'
        tok = lexer_->next();
        if (tok.type == TokenType::TOK_LPAREN) {
            // Parse parameter list
            bool saw_parameter = false;
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }
                if (tok.type == TokenType::TOK_PARAMETER || tok.type == TokenType::TOK_LOCALPARAM) {
                    auto param = parseParameter();
                    if (param) {
                        module->parameters.push_back(param);
                        saw_parameter = true;
                    }
                } else if (saw_parameter && tok.type == TokenType::TOK_IDENTIFIER) {
                    // Implicit parameter: `parameter W=8, S=4` — second param `S` is implied
                    auto param = parseImplicitParameter();
                    if (param) {
                        module->parameters.push_back(param);
                    }
                }
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
        }
    }
    
    // Parse port list
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LPAREN) {
        lexer_->next(); // consume '('
        
        // Check for ANSI-style port declaration
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_INPUT || tok.type == TokenType::TOK_OUTPUT ||
            tok.type == TokenType::TOK_INOUT) {
            // ANSI-style ports - may have comma-separated names: input [3:0] a, b, c
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }

                auto port = parsePort();
                if (port) {
                    module->ports.push_back(port);
                    // Handle comma-separated names: input [3:0] a, b
                    // After parsePort consumed 'a', peek for ',' then parse additional names
                    PortDecl::Direction last_dir = port->dir;
                    int last_width = port->width;
                    tok = lexer_->peek();
                    while (tok.type == TokenType::TOK_COMMA) {
                        lexer_->next(); // consume ','
                        tok = lexer_->peek();
                        if (tok.type == TokenType::TOK_RPAREN) break;
                        if (tok.type == TokenType::TOK_INPUT || tok.type == TokenType::TOK_OUTPUT || tok.type == TokenType::TOK_INOUT) break;
                        if (tok.type == TokenType::TOK_IDENTIFIER) {
                            lexer_->next(); // consume name
                            auto extra_port = std::make_shared<PortDecl>();
                            extra_port->name = tok.value;
                            extra_port->dir = last_dir;
                            extra_port->width = last_width;
                            module->ports.push_back(extra_port);
                        }
                        tok = lexer_->peek();
                    }
                }
            }
        } else {
            // Non-ANSI style - just parse port names
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }
                if (tok.type == TokenType::TOK_IDENTIFIER) {
                    lexer_->next();
                    // Port will be declared later
                }
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse module items

    while (should_continue()) {
        tok = lexer_->peek();
        if (debug_) std::cerr << "parseModule: peek type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;

        if (tok.type == TokenType::TOK_ENDMODULE) {
            lexer_->next();
            break;
        }

        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in module");
            break;
        }

        auto item = parseModuleItem();
        if (item) {
            module->items.push_back(item);
        }
        // Flush any pending declarations from comma-separated names
        for (auto &extra : pending_decls_) {
            module->items.push_back(extra);
        }
        pending_decls_.clear();
        // Note: parseModuleItem already consumes the token on failure (line 832),
        // so we must NOT call lexer_->next() here.
    }
    
    return module;
}

std::shared_ptr<PortDecl> Parser::parsePort() {
    auto port = std::make_shared<PortDecl>();
    
    Token tok = lexer_->peek();
    
    // Parse direction
    if (tok.type == TokenType::TOK_INPUT) {
        port->dir = PortDecl::INPUT;
        lexer_->next();
    } else if (tok.type == TokenType::TOK_OUTPUT) {
        port->dir = PortDecl::OUTPUT;
        lexer_->next();
    } else if (tok.type == TokenType::TOK_INOUT) {
        port->dir = PortDecl::INOUT;
        lexer_->next();
    } else {
        error("Expected port direction");
        return nullptr;
    }
    
    // Parse optional qualifiers (reg/wire/logic/signed/unsigned in any order)
    for (int qi = 0; qi < 10; qi++) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_SIGNED) {
            port->isSigned = true;
            lexer_->next();
            continue;
        } else if (tok.type == TokenType::TOK_UNSIGNED) {
            port->isSigned = false;
            lexer_->next();
            continue;
        } else if (tok.type == TokenType::TOK_REG) {
            port->isReg = true;
            lexer_->next();
            continue;
        } else if (tok.type == TokenType::TOK_LOGIC) {
            port->isLogic = true;
            lexer_->next();
            continue;
        } else if (tok.type == TokenType::TOK_WIRE) {
            lexer_->next();
            continue;
        }
        break;
    }
    
    // Parse optional range [MSB:LSB]
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto msb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto lsb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
        // Calculate width
        int msb = 0, lsb = 0;
        bool has_range = false;
        if (msb_expr && msb_expr->attributes.count("value")) {
            try { msb = std::stoi(msb_expr->attributes["value"]); has_range = true; } catch (...) {}
        }
        if (lsb_expr && lsb_expr->attributes.count("value")) {
            try { lsb = std::stoi(lsb_expr->attributes["value"]); } catch (...) {}
        }
        if (has_range) {
            port->width = std::abs(msb - lsb) + 1;
        } else {
            // Parameterized range (e.g., [WIDTH-1:0]) - default to 32
            port->width = 32;
        }
    }
    
    // Parse port name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected port name");
        return nullptr;
    }
    port->name = tok.value;
    port->loc = SourceLocation("<input>", tok.line, tok.column);
    
    return port;
}

std::shared_ptr<ASTNode> Parser::parseDeclaration() {
    Token tok = lexer_->peek();
    
    if (tok.type == TokenType::TOK_WIRE) {
        return parseWireDecl();
    } else if (tok.type == TokenType::TOK_REG) {
        return parseRegDecl();
    } else if (tok.type == TokenType::TOK_LOGIC) {
        return parseLogicDecl();
    } else if (tok.type == TokenType::TOK_INTEGER) {
        return parseIntegerDecl();
    } else if (tok.type == TokenType::TOK_REAL) {
        return parseRealDecl();
    } else if (tok.type == TokenType::TOK_TIME) {
        return parseTimeDecl();
    } else if (tok.type == TokenType::TOK_PARAMETER || tok.type == TokenType::TOK_LOCALPARAM) {
        return parseParameter();
    } else if (tok.type == TokenType::TOK_GENVAR) {
        return parseGenvarDecl();
    }
    
    return nullptr;
}

std::shared_ptr<ASTNode> Parser::parseWireDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::WIRE_DECL);

    lexer_->next(); // consume 'wire'

    // Parse optional signed/unsigned
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SIGNED || tok.type == TokenType::TOK_UNSIGNED) {
        lexer_->next();
    }

    // Parse optional range [MSB:LSB]
    int width = 1;
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto lsb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto msb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
        // Compute width
        int msb = 0, lsb = 0;
        if (msb_expr && msb_expr->attributes.count("value"))
            msb = std::stoi(msb_expr->attributes["value"]);
        if (lsb_expr && lsb_expr->attributes.count("value"))
            lsb = std::stoi(lsb_expr->attributes["value"]);
        width = std::abs(msb - lsb) + 1;
    }
    decl->setAttribute("width", std::to_string(width));

    // Parse wire name(s) - may be comma-separated: wire a, b, c;
    bool first_name = true;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected wire name");
            break;
        }

        if (first_name) {
            decl->setAttribute("name", tok.value);
            first_name = false;
        } else {
            auto extra = std::make_shared<ASTNode>(NodeType::WIRE_DECL);
            extra->setAttribute("name", tok.value);
            extra->setAttribute("width", std::to_string(width));
            pending_decls_.push_back(extra);
        }

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseRegDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::REG_DECL);
    
    lexer_->next(); // consume 'reg'
    
    // Parse optional signed/unsigned
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SIGNED || tok.type == TokenType::TOK_UNSIGNED) {
        lexer_->next();
    }
    
    // Parse optional range [MSB:LSB]
    int width = 1;
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto lsb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto msb_expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
        // Compute width from MSB and LSB
        int msb = 0, lsb = 0;
        if (msb_expr && msb_expr->attributes.count("value"))
            msb = std::stoi(msb_expr->attributes["value"]);
        if (lsb_expr && lsb_expr->attributes.count("value"))
            lsb = std::stoi(lsb_expr->attributes["value"]);
        width = std::abs(msb - lsb) + 1;
    }
    decl->setAttribute("width", std::to_string(width));

    // Parse reg name(s) - may be comma-separated: reg a, b, c;
    // Store first name in current decl, return additional decls via shared vector
    bool first_name = true;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected reg name");
            break;
        }

        if (first_name) {
            decl->setAttribute("name", tok.value);
            first_name = false;
        } else {
            // Create a new REG_DECL for each additional name
            auto extra = std::make_shared<ASTNode>(NodeType::REG_DECL);
            extra->setAttribute("name", tok.value);
            extra->setAttribute("width", std::to_string(width));
            if (decl->attributes.count("memory_dim"))
                extra->setAttribute("memory_dim", decl->attributes.at("memory_dim"));
            // We need to return these - store in a pending list
            pending_decls_.push_back(extra);
        }

        // Check for memory array declaration: reg [width] name [size]
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_LBRACKET) {
            // Memory array - consume dimensions and store them
            lexer_->next(); // consume '['
            std::string dim_str;
            while (should_continue()) {
                tok = lexer_->next();
                if (tok.type == TokenType::TOK_RBRACKET) break;
                if (tok.type == TokenType::TOK_EOF) break;
                dim_str += tok.value;
            }
            decl->setAttribute("memory_dim", dim_str);
        }

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }

    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }

    return decl;
}

std::shared_ptr<ASTNode> Parser::parseLogicDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::LOGIC_DECL);
    
    lexer_->next(); // consume 'logic'
    
    // Parse optional signed/unsigned
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SIGNED || tok.type == TokenType::TOK_UNSIGNED) {
        lexer_->next();
    }
    
    // Parse optional range
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto lsb = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto msb = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
    }
    
    // Parse logic name(s) - may be comma-separated
    int width = 1;
    // (width already computed above from range if present)
    bool first_name = true;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected logic name");
            break;
        }

        if (first_name) {
            decl->setAttribute("name", tok.value);
            first_name = false;
        } else {
            auto extra = std::make_shared<ASTNode>(NodeType::LOGIC_DECL);
            extra->setAttribute("name", tok.value);
            extra->setAttribute("width", std::to_string(width));
            pending_decls_.push_back(extra);
        }

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseIntegerDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::INTEGER_DECL);
    
    lexer_->next(); // consume 'integer'
    
    // Parse optional signed/unsigned
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SIGNED || tok.type == TokenType::TOK_UNSIGNED) {
        lexer_->next();
    }
    
    // Parse integer name(s) - may be comma-separated: integer a, b;
    bool first_name = true;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected integer name");
            break;
        }

        if (first_name) {
            decl->setAttribute("name", tok.value);
            first_name = false;
        } else {
            auto extra = std::make_shared<ASTNode>(NodeType::INTEGER_DECL);
            extra->setAttribute("name", tok.value);
            pending_decls_.push_back(extra);
        }

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseRealDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::REAL_DECL);
    
    lexer_->next(); // consume 'real'
    
    // Parse real name(s)
    Token tok;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected real name");
            break;
        }
        
        decl->setAttribute("name", tok.value);
        
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseTimeDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::TIME_DECL);
    
    lexer_->next(); // consume 'time'
    
    // Parse time name(s)
    Token tok;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected time name");
            break;
        }
        
        decl->setAttribute("name", tok.value);
        
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseParameter() {
    auto param = std::make_shared<ASTNode>(NodeType::PARAM_DECL);
    
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_PARAMETER && tok.type != TokenType::TOK_LOCALPARAM) {
        error("Expected 'parameter' or 'localparam'");
        return nullptr;
    }
    
    // Parse optional type
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_INTEGER || tok.type == TokenType::TOK_REAL || 
        tok.type == TokenType::TOK_TIME) {
        lexer_->next();
    }
    
    // Parse parameter name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected parameter name");
        return nullptr;
    }
    param->setAttribute("name", tok.value);
    
    // Consume '='
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_ASSIGN_OP && tok.type != TokenType::TOK_EQ) {
        error("Expected '='");
    }
    
    // Parse parameter value
    auto value = parseExpression();
    if (value) {
        param->addChild(value);
    }
    
    return param;
}

// Parse parameter without leading 'parameter' keyword (implicit in comma-separated list)
std::shared_ptr<ASTNode> Parser::parseImplicitParameter() {
    auto param = std::make_shared<ASTNode>(NodeType::PARAM_DECL);

    // Parse optional type (integer, real, time)
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_INTEGER || tok.type == TokenType::TOK_REAL ||
        tok.type == TokenType::TOK_TIME) {
        lexer_->next();
        tok = lexer_->peek();
    }

    // Parse parameter name (already peeked as IDENTIFIER by caller)
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected parameter name");
        return nullptr;
    }
    param->setAttribute("name", tok.value);

    // Consume '='
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_ASSIGN_OP && tok.type != TokenType::TOK_EQ) {
        // No default value — parameter without '='
        return param;
    }

    // Parse parameter value
    auto value = parseExpression();
    if (value) {
        param->children.push_back(value);
    }

    return param;
}

std::shared_ptr<ASTNode> Parser::parseGenvarDecl() {
    auto decl = std::make_shared<ASTNode>(NodeType::WIRE_DECL);
    
    lexer_->next(); // consume 'genvar'
    
    // Parse genvar name(s)
    Token tok;
    while (should_continue()) {
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_IDENTIFIER) {
            error("Expected genvar name");
            break;
        }
        
        decl->setAttribute("name", tok.value);
        
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        } else {
            break;
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseModuleItem() {
    Token tok = lexer_->peek();
    if (debug_) std::cerr << "parseModuleItem: type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;
    
    // Declarations
    if (tok.type == TokenType::TOK_WIRE || tok.type == TokenType::TOK_REG ||
        tok.type == TokenType::TOK_LOGIC || tok.type == TokenType::TOK_INTEGER ||
        tok.type == TokenType::TOK_REAL || tok.type == TokenType::TOK_TIME ||
        tok.type == TokenType::TOK_PARAMETER || tok.type == TokenType::TOK_LOCALPARAM ||
        tok.type == TokenType::TOK_GENVAR) {
        return parseDeclaration();
    }

    // SystemVerilog type declarations
    if (systemVerilog_ && (tok.type == TokenType::TOK_INT || tok.type == TokenType::TOK_BYTE ||
        tok.type == TokenType::TOK_BIT_T || tok.type == TokenType::TOK_SHORTINT ||
        tok.type == TokenType::TOK_LONGINT || tok.type == TokenType::TOK_STRING_T)) {
        return parseDeclaration();
    }

    // SystemVerilog typedef/enum/struct/union
    if (systemVerilog_ && (tok.type == TokenType::TOK_TYPEDEF || tok.type == TokenType::TOK_ENUM ||
        tok.type == TokenType::TOK_STRUCT || tok.type == TokenType::TOK_UNION)) {
        return parseSVTypeDecl();
    }
    
    // Port declarations (non-ANSI style)
    if (tok.type == TokenType::TOK_INPUT || tok.type == TokenType::TOK_OUTPUT || 
        tok.type == TokenType::TOK_INOUT) {
        return parsePort();
    }
    
    // Continuous assignment
    if (tok.type == TokenType::TOK_ASSIGN) {
        return parseAssign();
    }
    
    // Always block
    if (tok.type == TokenType::TOK_ALWAYS) {
        
        auto result = parseAlwaysBlock();
        
        return result;
    }
    
    // Initial block
    if (tok.type == TokenType::TOK_INITIAL) {
        return parseInitialBlock();
    }
    
    // Generate block
    if (tok.type == TokenType::TOK_GENERATE) {
        return parseGenerate();
    }
    
    // Module instantiation
    if (tok.type == TokenType::TOK_IDENTIFIER || tok.type == TokenType::TOK_SYS_TASK) {
        // Check if this is a module instantiation
        // Pattern: module_name instance_name (...) or module_name #(params) instance_name (...)
        Token next = lexer_->peekN(1);
        if (next.type == TokenType::TOK_IDENTIFIER) {
            return parseModuleInstance();
        }
        // Also check for module_name #(params) instance_name (...)
        if (next.type == TokenType::TOK_HASH) {
            return parseModuleInstance();
        }
    }
    
    // Function declaration
    if (tok.type == TokenType::TOK_FUNCTION) {
        return parseFunction();
    }
    
    // Task declaration
    if (tok.type == TokenType::TOK_TASK) {
        return parseTask();
    }
    
    // SystemVerilog constructs
    if (systemVerilog_) {
        if (tok.type == TokenType::TOK_ALWAYS_FF || tok.type == TokenType::TOK_ALWAYS_COMB ||
            tok.type == TokenType::TOK_ALWAYS_LATCH) {
            return parseAlwaysBlock();
        }

        if (tok.type == TokenType::TOK_ASSERT_T || tok.type == TokenType::TOK_ASSUME_T ||
            tok.type == TokenType::TOK_COVER_T) {
            return parseAssertion();
        }
    }

    // Compiler directives: timeunit, timeprecision, default_nettype, celldefine, endcelldefine
    // These are ignored during parsing (handled by preprocessor or accepted as no-ops)
    if (tok.type == TokenType::TOK_TIMEUNIT || tok.type == TokenType::TOK_TIMEPRECISION ||
        tok.type == TokenType::TOK_DEFAULT_NETTYPE) {
        // Skip: timeunit 1ns / 10ps; or timeprecision 1ps; or default_nettype wire;
        // These affect elaboration timing/precision settings, not synthesis
        lexer_->next(); // consume directive keyword
        // Parse and discard the value and semicolon
        Token t = lexer_->peek();
        while (t.type != TokenType::TOK_SEMICOLON && t.type != TokenType::TOK_EOF) {
            lexer_->next();
            t = lexer_->peek();
        }
        if (t.type == TokenType::TOK_SEMICOLON) lexer_->next();
        // Return a no-op statement
        return std::make_shared<ASTNode>(NodeType::BEGIN_STATEMENT);
    }

    if (tok.type == TokenType::TOK_CELLDEFINE) {
        lexer_->next(); return std::make_shared<ASTNode>(NodeType::BEGIN_STATEMENT);
    }
    if (tok.type == TokenType::TOK_ENDCELLDEFINE) {
        lexer_->next(); return std::make_shared<ASTNode>(NodeType::BEGIN_STATEMENT);
    }

    // Skip unknown item
    lexer_->next();
    return nullptr;
}

std::shared_ptr<ASTNode> Parser::parseAssign() {
    auto assign = std::make_shared<ASTNode>(NodeType::ASSIGN);
    
    lexer_->next(); // consume 'assign'
    
    // Parse LHS
    auto lhs = parseExpression();
    if (lhs) {
        assign->addChild(lhs);
    }
    
    // Consume '='
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_ASSIGN_OP && tok.type != TokenType::TOK_EQ) {
        error("Expected '='");
    }
    
    // Parse RHS
    auto rhs = parseExpression();
    if (rhs) {
        assign->addChild(rhs);
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return assign;
}

std::shared_ptr<ASTNode> Parser::parseAlwaysBlock() {

    auto block = std::make_shared<AlwaysBlock>();
    bool is_always_comb = false;
    bool is_always_latch = false;

    Token tok = lexer_->next();

    if (tok.type == TokenType::TOK_ALWAYS || tok.type == TokenType::TOK_ALWAYS_FF ||
        tok.type == TokenType::TOK_ALWAYS_COMB || tok.type == TokenType::TOK_ALWAYS_LATCH) {
        // Record which variant
        if (tok.type == TokenType::TOK_ALWAYS_COMB) { is_always_comb = true; block->setAttribute("sv_type", "always_comb"); }
        if (tok.type == TokenType::TOK_ALWAYS_LATCH) { is_always_latch = true; block->setAttribute("sv_type", "always_latch"); }
        if (tok.type == TokenType::TOK_ALWAYS_FF) { block->setAttribute("sv_type", "always_ff"); }
    } else {
        error("Expected 'always' or similar");
        return nullptr;
    }

    // Parse sensitivity list: always @( ... ) or always @(...)
    // Also handle: always #5 ..., always_comb, always_latch (no sensitivity list)
    tok = lexer_->peek();
    if (debug_) std::cerr << "  alwaysBlock: after 'always', peek type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;

    // always_comb and always_latch have no sensitivity list - directly parse body
    if (is_always_comb || is_always_latch) {
        block->sens = AlwaysBlock::LEVEL;
        auto stmt = parseStatement();
        if (stmt) block->addChild(stmt);
        return block;
    }

    if (tok.type == TokenType::TOK_AT) {
        lexer_->next(); // consume '@'
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_LPAREN) {
            error("Expected '(' after '@'");
            return nullptr;
        }
    } else if (tok.type == TokenType::TOK_HASH) {
        // always #5 ... — no sensitivity list, just parse the body statement
        block->sens = AlwaysBlock::LEVEL;
        auto stmt = parseStatement();
        if (stmt) block->addChild(stmt);
        return block;
    } else if (tok.type == TokenType::TOK_LPAREN) {
        lexer_->next(); // consume '('
    } else {
        error("Expected '@(' or '(' after 'always'");
        return nullptr;
    }

    // Check for '*' (level sensitive)
    tok = lexer_->peek();
    if (debug_) std::cerr << "  sensList: first token type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;
    if (tok.type == TokenType::TOK_STAR) {
        lexer_->next(); // consume '*'
        block->sens = AlwaysBlock::LEVEL;
        // Consume closing ')'
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RPAREN) {
            error("Expected ')' after '*'");
        }
    } else {
        // Parse sensitivity list
        while (should_continue()) {
            tok = lexer_->peek();

            if (tok.type == TokenType::TOK_RPAREN) {
                lexer_->next();
                break;
            }

            // Parse edge
            if (tok.type == TokenType::TOK_POSEDGE) {
                if (block->sens == AlwaysBlock::NEGEDGE) block->sens = AlwaysBlock::BOTH;
                else block->sens = AlwaysBlock::POSEDGE;
                lexer_->next();
            } else if (tok.type == TokenType::TOK_NEGEDGE) {
                if (block->sens == AlwaysBlock::POSEDGE) block->sens = AlwaysBlock::BOTH;
                else block->sens = AlwaysBlock::NEGEDGE;
                lexer_->next();
            }

            // Parse signal name
            tok = lexer_->next();
            if (debug_) std::cerr << "  sensList: signal token type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;

            if (tok.type == TokenType::TOK_IDENTIFIER) {
                block->sensitivityList.push_back(tok.value);
                if (block->clockSignal.empty()) block->clockSignal = tok.value;
            }

            tok = lexer_->peek();
            if (debug_) std::cerr << "  sensList: after signal, peek type=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;

            if (tok.type == TokenType::TOK_COMMA ||
                tok.type == TokenType::TOK_OR_T ||
                (tok.type == TokenType::TOK_IDENTIFIER && (tok.value == "or" || tok.value == "OR"))) {
                lexer_->next();
            }
        }
    }

    

    // Parse statement
    auto stmt = parseStatement();
    
    if (stmt) {
        block->addChild(stmt);
    }

    return block;
}

std::shared_ptr<ASTNode> Parser::parseInitialBlock() {
    auto block = std::make_shared<Statement>(NodeType::INITIAL_BLOCK);
    
    lexer_->next(); // consume 'initial'
    
    // Parse statement
    auto stmt = parseStatement();
    if (stmt) {
        block->addChild(stmt);
    }
    
    return block;
}

std::shared_ptr<ASTNode> Parser::parseStatement() {
    Token tok = lexer_->peek();
    if (debug_) std::cerr << "    parseStmt: peek=" << (int)tok.type << " '" << tok.value << "'" << std::endl;

    // Empty statement: just a semicolon
    if (tok.type == TokenType::TOK_SEMICOLON) {
        lexer_->next(); // consume ';'
        return std::make_shared<ASTNode>(VerilogParser::NodeType::ASSIGN); // empty statement
    }

    // Delay statement: #N or #N assignment
    if (tok.type == TokenType::TOK_HASH) {
        lexer_->next(); // consume '#'
        Token num = lexer_->next();
        // Check if followed by assignment (e.g., #5 clk=~clk; or #5 count<=count+1;)
        Token next = lexer_->peek();
        if (next.type == TokenType::TOK_IDENTIFIER) {
            // Peek ahead to determine blocking vs non-blocking
            Token after = lexer_->peekN(1);
            std::shared_ptr<ASTNode> assign;
            if (after.type == TokenType::TOK_LEQ) {
                assign = parseNonBlockingAssignment();
            } else {
                assign = parseBlockingAssignment();
            }
            if (assign) {
                assign->setAttribute("delay", num.value);
                return assign;
            }
        }
        // Standalone delay: #N;
        Token semi = lexer_->peek();
        if (semi.type == TokenType::TOK_SEMICOLON) lexer_->next();
        auto delay = std::make_shared<ASTNode>(VerilogParser::NodeType::ASSIGN);
        delay->setAttribute("delay", num.value);
        return delay;
    }

    // Block statement
    if (tok.type == TokenType::TOK_BEGIN) {
        return parseBlock();
    }
    
    // If statement
    if (tok.type == TokenType::TOK_IF) {
        return parseIfStatement();
    }
    
    // SystemVerilog unique/priority case
    if (systemVerilog_ && (tok.type == TokenType::TOK_UNIQUE || tok.type == TokenType::TOK_PRIORITY)) {
        lexer_->next(); // consume 'unique' or 'priority'
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_CASE || tok.type == TokenType::TOK_CASEX ||
            tok.type == TokenType::TOK_CASEZ) {
            auto stmt = parseCaseStatement();
            if (stmt) {
                // Mark as unique/priority case
                // The 'unique'/'priority' keyword was already consumed, store as attribute
                stmt->setAttribute("sv_case_qualifier", "unique");
                return stmt;
            }
        }
        error("Expected 'case' after 'unique'/'priority'");
        return nullptr;
    }

    // Case statement
    if (tok.type == TokenType::TOK_CASE || tok.type == TokenType::TOK_CASEX ||
        tok.type == TokenType::TOK_CASEZ) {
        return parseCaseStatement();
    }
    
    // For loop
    if (tok.type == TokenType::TOK_FOR) {
        return parseForLoop();
    }

    // Forever loop
    if (tok.type == TokenType::TOK_FOREVER) {
        return parseForeverLoop();
    }
    
    // While loop
    if (tok.type == TokenType::TOK_WHILE) {
        return parseWhileLoop();
    }
    
    // Repeat loop
    if (tok.type == TokenType::TOK_REPEAT) {
        return parseRepeatLoop();
    }

    // Forever loop
    if (tok.type == TokenType::TOK_FOREVER) {
        return parseForeverLoop();
    }

    // Fork statement
    if (tok.type == TokenType::TOK_FORK) {
        return parseForkStatement();
    }

    // Disable statement
    if (tok.type == TokenType::TOK_DISABLE) {
        return parseDisableStatement();
    }

    // Wait statement
    if (tok.type == TokenType::TOK_WAIT_T) {
        return parseWaitStatement();
    }

    // Event control: @(posedge signal) or @(negedge signal) or @(*)
    if (tok.type == TokenType::TOK_AT) {
        lexer_->next(); // consume '@'
        Token next = lexer_->next();
        if (next.type == TokenType::TOK_LPAREN) {
            // Parse edge type and signal
            Token edge = lexer_->peek();
            std::string edge_type;
            if (edge.type == TokenType::TOK_POSEDGE) {
                edge_type = "posedge";
                lexer_->next();
            } else if (edge.type == TokenType::TOK_NEGEDGE) {
                edge_type = "negedge";
                lexer_->next();
            } else if (edge.type == TokenType::TOK_STAR) {
                edge_type = "*";
                lexer_->next();
            }
            // Parse signal name
            Token sig = lexer_->next();
            std::string signal_name;
            if (sig.type == TokenType::TOK_IDENTIFIER) {
                signal_name = sig.value;
            }
            // Consume ')'
            Token rparen = lexer_->next();
            if (rparen.type != TokenType::TOK_RPAREN) {
                error("Expected ')' after event control");
            }
            // Create WAIT_FOR_EDGE node
            auto wait_node = std::make_shared<ASTNode>(VerilogParser::NodeType::WAIT_FOR_EDGE);
            wait_node->setAttribute("edge_type", edge_type);
            wait_node->setAttribute("signal", signal_name);
            return wait_node;
        }
    }

    // Non-blocking assignment: identifier <= expr or identifier[bit] <= expr
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        Token next = lexer_->peekN(1);
        if (next.type == TokenType::TOK_LEQ) {
            auto result = parseNonBlockingAssignment();
            if (debug_) { Token t = lexer_->peek(); std::cerr << "  afterNB: type=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
            return result;
        } else if (next.type == TokenType::TOK_ASSIGN_OP) {
            auto result = parseBlockingAssignment();
            if (debug_) { Token t = lexer_->peek(); std::cerr << "  afterBLK: type=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
            return result;
        } else if (next.type == TokenType::TOK_LBRACKET) {
            // identifier[bit] <= expr — peek past [...] to find <=
            Token peek2 = lexer_->peekN(2);
            // Skip past the bracket contents to find <= or =
            int bracket_depth = 1;
            int offset = 2;
            while (bracket_depth > 0 && offset < 20) {
                Token t = lexer_->peekN(offset);
                if (t.type == TokenType::TOK_LBRACKET) bracket_depth++;
                else if (t.type == TokenType::TOK_RBRACKET) bracket_depth--;
                if (bracket_depth == 0) {
                    Token after = lexer_->peekN(offset + 1);
                    if (after.type == TokenType::TOK_LEQ) {
                        auto result = parseNonBlockingAssignment();
                        return result;
                    } else if (after.type == TokenType::TOK_ASSIGN_OP) {
                        auto result = parseBlockingAssignment();
                        return result;
                    }
                }
                offset++;
            }
        }
    }

    // System task call
    if (tok.type == TokenType::TOK_SYS_TASK) {
        return parseSystemTaskCall();
    }
    
    // Function/task call
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        Token next = lexer_->peekN(1);
        if (next.type == TokenType::TOK_LPAREN) {
            return parseFunctionCall();
        }
    }
    
    // Unknown statement - do NOT consume tokens, just return nullptr
    if (debug_) std::cerr << "    parseStmt: UNKNOWN type=" << (int)tok.type << " '" << tok.value << "'" << std::endl;
    return nullptr;
}

std::shared_ptr<ASTNode> Parser::parseBlock() {
    auto block = std::make_shared<Statement>(NodeType::BEGIN_STATEMENT);
    
    lexer_->next(); // consume 'begin'
    
    // Parse optional block identifier
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        // Check if this is a block identifier or a statement
        // This is simplified - a real parser would need more context
    }
    
    // Parse statements
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_END) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in block");
            break;
        }
        
        auto stmt = parseStatement();
        if (stmt) {
            block->addChild(stmt);
        } else {
            // parseStatement returned nullptr without consuming - skip this token
            lexer_->next();
        }
    }
    
    return block;
}

std::shared_ptr<ASTNode> Parser::parseIfStatement() {
    auto ifStmt = std::make_shared<Statement>(NodeType::IF_STATEMENT);

    lexer_->next(); // consume 'if'

    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
        return ifStmt;
    }

    // Parse condition - handle unary operators like !, ~
    tok = lexer_->peek();
    std::shared_ptr<ASTNode> cond;
    if (tok.type == TokenType::TOK_EXCLAIM) {
        // Logical NOT: !expr
        lexer_->next(); // consume '!'
        auto operand = parseExpression();
        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::UNARY_OP;
        expr->op = Expression::ULNOT;
        expr->left = operand;
        cond = expr;
    } else if (tok.type == TokenType::TOK_TILDE) {
        // Bitwise NOT: ~expr
        lexer_->next();
        auto operand = parseExpression();
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::UNOT;
        expr->left = operand;
        cond = expr;
    } else {
        cond = parseExpression();
    }
    if (cond) {
        ifStmt->addChild(cond);
    }

    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }

    // Parse then statement
    auto thenStmt = parseStatement();
    if (thenStmt) {
        ifStmt->addChild(thenStmt);
    }

    // Parse optional else
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_ELSE) {
        lexer_->next(); // consume 'else'
        auto elseStmt = parseStatement();
        if (elseStmt) {
            ifStmt->addChild(elseStmt);
        }
    }

    return ifStmt;
}

std::shared_ptr<ASTNode> Parser::parseCaseStatement() {
    auto caseStmt = std::make_shared<Statement>(NodeType::CASE_STATEMENT);
    
    lexer_->next(); // consume 'case', 'casex', or 'casez'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse expression
    auto expr = parseExpression();
    if (expr) {
        caseStmt->addChild(expr);
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse case items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDCASE) {
            lexer_->next();
            break;
        }

        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in case");
            break;
        }

        // Parse case item
        auto item = std::make_shared<Statement>(NodeType::CASE_ITEM);

        // Handle 'default' keyword
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_DEFAULT) {
            lexer_->next(); // consume 'default'
            tok = lexer_->next();
            if (tok.type != TokenType::TOK_COLON) {
                error("Expected ':' after 'default'");
            }
        } else {
            // Parse expression list
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COLON) {
                    lexer_->next();
                    break;
                }
                if (tok.type == TokenType::TOK_DEFAULT || tok.type == TokenType::TOK_ENDCASE) {
                    break;
                }

                auto itemExpr = parseExpression();
                if (itemExpr) {
                    item->addChild(itemExpr);
                } else {
                    // If parseExpression fails, skip token to avoid infinite loop
                    lexer_->next();
                }

                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
        }
        
        // Parse statement
        auto itemStmt = parseStatement();
        if (itemStmt) {
            item->addChild(itemStmt);
        }
        
        caseStmt->addChild(item);
    }
    
    return caseStmt;
}

std::shared_ptr<ASTNode> Parser::parseForLoop() {
    auto forLoop = std::make_shared<Statement>(NodeType::FOR_LOOP);

    lexer_->next(); // consume 'for'

    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }

    // Parse initialization (parseStatement may or may not consume the trailing ';')
    auto init = parseStatement();
    if (init) {
        forLoop->addChild(init);
    }
    // Consume ';' if not already consumed by parseStatement
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SEMICOLON) {
        lexer_->next();
    }

    // Parse condition
    auto cond = parseExpression();
    if (cond) {
        forLoop->addChild(cond);
    }

    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }

    // The update clause is inside the for-header and therefore has no
    // terminating semicolon. Parsing it as a normal statement makes
    // parseBlockingAssignment consume the closing ')' as if it were ';',
    // which in turn loses the loop body and following statements.
    auto update = std::make_shared<ASTNode>(NodeType::ASSIGN);
    auto update_lhs = parseExpression();
    if (update_lhs) update->addChild(update_lhs);
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_ASSIGN_OP) {
        error("Expected '=' in for-loop update");
    }
    auto update_rhs = parseExpression();
    if (update_rhs) update->addChild(update_rhs);
    if (!update->children.empty()) {
        forLoop->addChild(update);
    }

    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }

    // Parse body
    auto body = parseStatement();
    if (body) {
        forLoop->addChild(body);
    }

    return forLoop;
}

std::shared_ptr<ASTNode> Parser::parseWhileLoop() {
    auto whileLoop = std::make_shared<Statement>(NodeType::WHILE_LOOP);
    
    lexer_->next(); // consume 'while'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse condition
    auto cond = parseExpression();
    if (cond) {
        whileLoop->addChild(cond);
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse body
    auto body = parseStatement();
    if (body) {
        whileLoop->addChild(body);
    }
    
    return whileLoop;
}

std::shared_ptr<ASTNode> Parser::parseRepeatLoop() {
    auto repeatLoop = std::make_shared<Statement>(NodeType::REPEAT_LOOP);
    
    lexer_->next(); // consume 'repeat'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse count
    auto count = parseExpression();
    if (count) {
        repeatLoop->addChild(count);
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse body
    auto body = parseStatement();
    if (body) {
        repeatLoop->addChild(body);
    }
    
    return repeatLoop;
}

std::shared_ptr<ASTNode> Parser::parseForeverLoop() {
    auto foreverLoop = std::make_shared<Statement>(NodeType::FOREVER_LOOP);
    lexer_->next(); // consume 'forever'
    auto body = parseStatement();
    if (body) foreverLoop->addChild(body);
    return foreverLoop;
}

std::shared_ptr<ASTNode> Parser::parseForkStatement() {
    auto forkStmt = std::make_shared<ASTNode>(NodeType::FORKJOIN);
    lexer_->next(); // consume 'fork'
    // Parse statements until 'join'/'join_any'/'join_none'
    while (should_continue()) {
        Token tok = lexer_->peek();
        if (tok.type == TokenType::TOK_JOIN || tok.type == TokenType::TOK_JOIN_ANY ||
            tok.type == TokenType::TOK_JOIN_NONE) {
            lexer_->next(); // consume join keyword
            break;
        }
        if (tok.type == TokenType::TOK_EOF) break;
        auto stmt = parseStatement();
        if (stmt) forkStmt->addChild(stmt);
    }
    return forkStmt;
}

std::shared_ptr<ASTNode> Parser::parseDisableStatement() {
    auto disableStmt = std::make_shared<ASTNode>(NodeType::FORKJOIN);
    lexer_->next(); // consume 'disable'
    Token tok = lexer_->next();
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        disableStmt->setAttribute("block_name", tok.value);
    }
    // consume ';'
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();
    return disableStmt;
}

std::shared_ptr<ASTNode> Parser::parseWaitStatement() {
    auto waitStmt = std::make_shared<ASTNode>(NodeType::WAIT_FOR_EDGE);
    lexer_->next(); // consume 'wait'
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) return waitStmt;
    auto expr = parseExpression();
    if (expr) waitStmt->addChild(expr);
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) return waitStmt;
    // Parse body statement
    auto body = parseStatement();
    if (body) waitStmt->addChild(body);
    return waitStmt;
}

std::shared_ptr<ASTNode> Parser::parseBlockingAssignment() {
    auto assign = std::make_shared<ASTNode>(NodeType::ASSIGN);

    // Parse LHS
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  BLK: before LHS peek=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
    auto lhs = parseExpression();
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  BLK: after LHS peek=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
    if (lhs) {
        assign->addChild(lhs);
    }
    
    // Consume '='
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_ASSIGN_OP) {
        error("Expected '='");
    }

    // Parse RHS
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  BLK: before RHS peek=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
    auto rhs = parseExpression();
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  BLK: after RHS peek=" << (int)t.type << " val='" << t.value << "'" << std::endl; }
    if (rhs) {
        assign->addChild(rhs);
    }

    // Consume ';'
    tok = lexer_->next();
    if (debug_) std::cerr << "  BLK: consumed semi=" << (int)tok.type << " val='" << tok.value << "'" << std::endl;
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }

    return assign;
}

std::shared_ptr<ASTNode> Parser::parseNonBlockingAssignment() {

    auto assign = std::make_shared<ASTNode>(NodeType::ASSIGN);
    assign->setAttribute("nonblocking", "1");

    // Parse LHS - must be a simple lvalue (identifier with optional bit/part select)
    // NOT a full expression, because <= would be parsed as comparison
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected identifier for assignment LHS");
        return assign;
    }
    auto lhs = std::make_shared<Expression>();
    lhs->type = NodeType::IDENTIFIER;
    lhs->setAttribute("name", tok.value);

    // Check for bit select [N] or part select [N:M]
    Token next = lexer_->peek();
    if (next.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto index = parseExpression();
        next = lexer_->next();
        if (next.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
        lhs->addChild(index);
    }

    assign->addChild(lhs);

    // Consume '<='
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_LEQ) {
        error("Expected '<='");
    }
    
    // Parse RHS
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  NBA: before RHS=" << (int)t.type << " '" << t.value << "'" << std::endl; }
    auto rhs = parseExpression();
    if (debug_) { Token t = lexer_->peek(); std::cerr << "  NBA: after RHS=" << (int)t.type << " '" << t.value << "'" << std::endl; }
    if (rhs) {
        assign->addChild(rhs);
    }

    // Consume ';'
    tok = lexer_->next();
    if (debug_) std::cerr << "  NBA: semi=" << (int)tok.type << " '" << tok.value << "'" << std::endl;
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }

    return assign;
}

std::shared_ptr<ASTNode> Parser::parseSystemTaskCall() {
    auto call = std::make_shared<ASTNode>(NodeType::FUNCTION_CALL);
    
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_SYS_TASK) {
        error("Expected system task");
        return nullptr;
    }
    call->setAttribute("name", tok.value);
    
    // Consume '('
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LPAREN) {
        lexer_->next();
        
        // Parse arguments
        while (should_continue()) {
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_RPAREN) {
                lexer_->next();
                break;
            }
            
            auto arg = parseExpression();
            if (arg) {
                call->addChild(arg);
            }
            
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_COMMA) {
                lexer_->next();
            }
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return call;
}

std::shared_ptr<ASTNode> Parser::parseFunctionCall() {
    auto call = std::make_shared<ASTNode>(NodeType::FUNCTION_CALL);
    
    // Parse function name
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected function name");
        return nullptr;
    }
    call->setAttribute("name", tok.value);
    
    // Consume '('
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse arguments
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_RPAREN) {
            lexer_->next();
            break;
        }
        
        auto arg = parseExpression();
        if (arg) {
            call->addChild(arg);
        }
        
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        }
    }
    
    return call;
}

std::shared_ptr<ASTNode> Parser::parseModuleInstance() {
    auto inst = std::make_shared<ASTNode>(NodeType::MODULE_INSTANCE);

    // Parse module type (can be identifier or system task like $_BUF_)
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER && tok.type != TokenType::TOK_SYS_TASK) {
        error("Expected module type");
        return nullptr;
    }
    inst->setAttribute("type", tok.value);

    // Check for optional #(params) before instance name
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_HASH) {
        lexer_->next(); // consume '#'
        // Skip parameter list: find matching ')'
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_LPAREN) {
            error("Expected '(' after '#'");
        }
        int depth = 1;
        while (depth > 0) {
            tok = lexer_->next();
            if (tok.type == TokenType::TOK_LPAREN) depth++;
            if (tok.type == TokenType::TOK_RPAREN) depth--;
            if (tok.type == TokenType::TOK_EOF) break;
        }
    }

    // Parse instance name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected instance name");
        return nullptr;
    }
    inst->setAttribute("name", tok.value);

    // Consume '('
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse port connections
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_RPAREN) {
            lexer_->next();
            break;
        }
        
        // Parse port connection
        auto conn = std::make_shared<ASTNode>(NodeType::MODULE_PORT);
        
        // Check for named connection
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_DOT) {
            lexer_->next(); // consume '.'
            
            // Parse port name
            tok = lexer_->next();
            if (tok.type != TokenType::TOK_IDENTIFIER) {
                error("Expected port name");
            }
            conn->setAttribute("port", tok.value);
            
            // Consume '('
            tok = lexer_->next();
            if (tok.type != TokenType::TOK_LPAREN) {
                error("Expected '('");
            }
            
            // Parse expression
            auto expr = parseExpression();
            if (expr) {
                conn->addChild(expr);
            }
            
            // Consume ')'
            tok = lexer_->next();
            if (tok.type != TokenType::TOK_RPAREN) {
                error("Expected ')'");
            }
        } else {
            // Positional connection
            auto expr = parseExpression();
            if (expr) {
                conn->addChild(expr);
            }
        }
        
        inst->addChild(conn);
        
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_COMMA) {
            lexer_->next();
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return inst;
}

std::shared_ptr<ASTNode> Parser::parseGenerate() {
    auto genBlock = std::make_shared<GenerateBlock>();
    
    lexer_->next(); // consume 'generate'
    
    // Parse generate items
    while (should_continue()) {
        Token tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDGENERATE) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in generate block");
            break;
        }
        
        auto item = parseGenerateItem();
        if (item) {
            genBlock->addChild(item);
        }
    }
    
    return genBlock;
}

std::shared_ptr<ASTNode> Parser::parseGenerateItem() {
    Token tok = lexer_->peek();

    if (tok.type == TokenType::TOK_IF) {
        return parseGenerateIf();
    } else if (tok.type == TokenType::TOK_FOR) {
        return parseGenerateFor();
    } else if (tok.type == TokenType::TOK_CASE) {
        return parseGenerateCase();
    } else if (tok.type == TokenType::TOK_GENERATE) {
        // Nested generate block
        return parseGenerate();
    } else {
        return parseModuleItem();
    }
}

std::shared_ptr<ASTNode> Parser::parseGenerateIf() {
    auto genIf = std::make_shared<GenerateBlock>();
    genIf->genType = GenerateBlock::GEN_IF;
    
    lexer_->next(); // consume 'if'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse condition
    auto cond = parseExpression();
    if (cond) {
        genIf->condition = cond;
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse then block
    auto thenBlock = parseGenerateItem();
    if (thenBlock) {
        genIf->addChild(thenBlock);
    }
    
    // Parse optional else
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_ELSE) {
        lexer_->next();
        auto elseBlock = parseGenerateItem();
        if (elseBlock) {
            genIf->addChild(elseBlock);
        }
    }
    
    return genIf;
}

std::shared_ptr<ASTNode> Parser::parseGenerateFor() {
    auto genFor = std::make_shared<GenerateBlock>();
    genFor->genType = GenerateBlock::GEN_FOR;
    
    lexer_->next(); // consume 'for'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse initialization
    auto init = parseStatement();
    if (init) {
        genFor->init = init;
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse condition
    auto cond = parseExpression();
    if (cond) {
        genFor->condition = cond;
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse update
    auto update = parseStatement();
    if (update) {
        genFor->update = update;
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse body
    auto body = parseGenerateItem();
    if (body) {
        genFor->addChild(body);
    }
    
    return genFor;
}

std::shared_ptr<ASTNode> Parser::parseGenerateCase() {
    auto genCase = std::make_shared<GenerateBlock>();
    genCase->genType = GenerateBlock::GEN_CASE;
    
    lexer_->next(); // consume 'case'
    
    // Consume '('
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_LPAREN) {
        error("Expected '('");
    }
    
    // Parse expression
    auto expr = parseExpression();
    if (expr) {
        genCase->condition = expr;
    }
    
    // Consume ')'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_RPAREN) {
        error("Expected ')'");
    }
    
    // Parse case items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDCASE) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in generate case");
            break;
        }
        
        // Parse case item
        auto item = std::make_shared<Statement>(NodeType::CASE_ITEM);
        
        // Parse expression list
        while (should_continue()) {
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_COLON) {
                lexer_->next();
                break;
            }
            
            auto itemExpr = parseExpression();
            if (itemExpr) {
                item->addChild(itemExpr);
            }
            
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_COMMA) {
                lexer_->next();
            }
        }
        
        // Parse statement
        auto itemStmt = parseGenerateItem();
        if (itemStmt) {
            item->addChild(itemStmt);
        }
        
        genCase->addChild(item);
    }
    
    return genCase;
}

std::shared_ptr<ASTNode> Parser::parseFunction() {
    auto func = std::make_shared<FunctionDecl>();
    
    lexer_->next(); // consume 'function'
    
    // Parse optional return type
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_REG || tok.type == TokenType::TOK_LOGIC || 
        tok.type == TokenType::TOK_INTEGER || tok.type == TokenType::TOK_REAL) {
        lexer_->next();
        func->returnType = tok.value;
    }
    
    // Parse function name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected function name");
        return nullptr;
    }
    func->name = tok.value;
    
    // Parse optional range
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LBRACKET) {
        lexer_->next(); // consume '['
        auto lsb = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto msb = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RBRACKET) {
            error("Expected ']'");
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse function items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDFUNCTION) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in function");
            break;
        }
        
        // Parse declaration or statement
        if (tok.type == TokenType::TOK_INPUT) {
            lexer_->next(); // consume 'input'
            auto param = parseDeclaration();
            if (param) {
                func->parameters.push_back(param);
            }
        } else {
            auto stmt = parseStatement();
            if (stmt) {
                func->statements.push_back(stmt);
            }
        }
    }
    
    return func;
}

std::shared_ptr<ASTNode> Parser::parseTask() {
    auto task = std::make_shared<TaskDecl>();
    
    lexer_->next(); // consume 'task'
    
    // Parse task name
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected task name");
        return nullptr;
    }
    task->name = tok.value;
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse task items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDTASK) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in task");
            break;
        }
        
        // Parse declaration or statement
        if (tok.type == TokenType::TOK_INPUT || tok.type == TokenType::TOK_OUTPUT || 
            tok.type == TokenType::TOK_INOUT) {
            auto param = parsePort();
            if (param) {
                task->parameters.push_back(param);
            }
        } else {
            auto stmt = parseStatement();
            if (stmt) {
                task->statements.push_back(stmt);
            }
        }
    }
    
    return task;
}

std::shared_ptr<ASTNode> Parser::parseAssertion() {
    auto assert = std::make_shared<Assertion>();
    
    Token tok = lexer_->next();
    if (tok.type == TokenType::TOK_ASSERT_T) {
        assert->assertType = Assertion::ASSERT;
    } else if (tok.type == TokenType::TOK_ASSUME_T) {
        assert->assertType = Assertion::ASSUME;
    } else if (tok.type == TokenType::TOK_COVER_T) {
        assert->assertType = Assertion::COVER;
    } else {
        error("Expected assertion keyword");
        return nullptr;
    }
    
    // Parse optional label
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        // Check if this is a label or property name
        Token next = lexer_->peekN(1);
        if (next.type == TokenType::TOK_COLON) {
            assert->label = tok.value;
            lexer_->next(); // consume label
            lexer_->next(); // consume ':'
        }
    }
    
    // Parse property
    auto prop = parseExpression();
    if (prop) {
        assert->property = prop;
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    return assert;
}

std::shared_ptr<ASTNode> Parser::parseSVTypeDecl() {
    Token tok = lexer_->peek();

    if (tok.type == TokenType::TOK_TYPEDEF) {
        lexer_->next(); // consume 'typedef'
        auto decl = std::make_shared<ASTNode>(NodeType::TYPEDEF_DECL);

        // Parse: typedef <existing_type> <new_type_name> [range];
        // e.g.: typedef logic [7:0] byte_t;
        //       typedef enum { RED, GREEN } color_t;
        std::string base_type, new_name;
        int width = 1;
        bool is_signed = false;

        // Check if next is enum/struct
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENUM) {
            base_type = "enum";
            // Parse enum body
            lexer_->next(); // consume 'enum'
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_LBRACE) {
                lexer_->next(); // consume '{'
                int enum_val = 0;
                while (should_continue()) {
                    tok = lexer_->peek();
                    if (tok.type == TokenType::TOK_RBRACE) { lexer_->next(); break; }
                    if (tok.type == TokenType::TOK_IDENTIFIER) {
                        lexer_->next();
                        auto member = std::make_shared<ASTNode>(NodeType::WIRE_DECL);
                        member->setAttribute("name", tok.value);
                        member->setAttribute("value", std::to_string(enum_val++));
                        decl->children.push_back(member);
                    }
                    tok = lexer_->peek();
                    if (tok.type == TokenType::TOK_COMMA) lexer_->next();
                    if (tok.type == TokenType::TOK_EQ) {
                        lexer_->next(); // consume '='
                        tok = lexer_->next(); // value
                        if (tok.type == TokenType::TOK_INTEGER_KW || tok.type == TokenType::TOK_INTEGER) {
                            try { enum_val = std::stoi(tok.value); } catch(...) {}
                        }
                    }
                }
            }
            // Get the typedef name
            tok = lexer_->next(); // should be the new type name
            if (tok.type == TokenType::TOK_IDENTIFIER) {
                new_name = tok.value;
            }
        } else if (tok.type == TokenType::TOK_STRUCT || tok.type == TokenType::TOK_UNION) {
            base_type = (tok.type == TokenType::TOK_STRUCT) ? "struct" : "union";
            lexer_->next(); // consume struct/union
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_PACKED) { lexer_->next(); }
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_LBRACE) {
                lexer_->next(); // consume '{'
                while (should_continue()) {
                    tok = lexer_->peek();
                    if (tok.type == TokenType::TOK_RBRACE) { lexer_->next(); break; }
                    if (tok.type == TokenType::TOK_IDENTIFIER || tok.type == TokenType::TOK_REG || tok.type == TokenType::TOK_WIRE || tok.type == TokenType::TOK_LOGIC) {
                        auto field = parseModuleItem();
                        if (field) decl->children.push_back(field);
                    } else {
                        lexer_->next(); // skip
                    }
                    tok = lexer_->peek();
                    if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();
                }
            }
            tok = lexer_->next(); // should be the type name
            if (tok.type == TokenType::TOK_IDENTIFIER) {
                new_name = tok.value;
            }
        } else {
            // Simple typedef: typedef <type> <name>;
            // Parse base type
            if (tok.type == TokenType::TOK_IDENTIFIER || tok.type == TokenType::TOK_REG ||
                tok.type == TokenType::TOK_WIRE || tok.type == TokenType::TOK_LOGIC) {
                base_type = tok.value;
                lexer_->next();
            }
            // Check for range [MSB:LSB]
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_LBRACKET) {
                lexer_->next();
                tok = lexer_->next(); // MSB
                if (tok.type == TokenType::TOK_INTEGER_KW) {
                    try {
                        int msb = std::stoi(tok.value);
                        tok = lexer_->next(); // ':'
                        tok = lexer_->next(); // LSB
                        int lsb = std::stoi(tok.value);
                        width = std::abs(msb - lsb) + 1;
                    } catch(...) { width = 8; }
                }
                lexer_->next(); // consume ']'
            }
            tok = lexer_->next(); // new type name
            if (tok.type == TokenType::TOK_IDENTIFIER) {
                new_name = tok.value;
            }
        }

        decl->setAttribute("base_type", base_type);
        decl->setAttribute("name", new_name);
        decl->setAttribute("width", std::to_string(width));
        decl->setAttribute("signed", is_signed ? "1" : "0");

        // Consume trailing semicolon
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();

        return decl;
    }

    if (tok.type == TokenType::TOK_ENUM) {
        lexer_->next(); // consume 'enum'
        auto decl = std::make_shared<ASTNode>(NodeType::ENUM_DECL);
        int enum_val = 0;

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_LBRACE) {
            lexer_->next(); // consume '{'
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RBRACE) { lexer_->next(); break; }
                if (tok.type == TokenType::TOK_IDENTIFIER) {
                    lexer_->next();
                    auto member = std::make_shared<ASTNode>(NodeType::WIRE_DECL);
                    member->setAttribute("name", tok.value);
                    member->setAttribute("value", std::to_string(enum_val++));
                    decl->children.push_back(member);
                }
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) lexer_->next();
                if (tok.type == TokenType::TOK_EQ) {
                    lexer_->next();
                    tok = lexer_->next();
                    if (tok.type == TokenType::TOK_INTEGER_KW || tok.type == TokenType::TOK_INTEGER) {
                        try { enum_val = std::stoi(tok.value); } catch(...) {}
                    }
                }
            }
        }
        // Get optional variable name after '}'
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_IDENTIFIER) {
            lexer_->next();
            decl->setAttribute("name", tok.value);
        }
        // Consume trailing semicolon
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();
        return decl;
    }

    if (tok.type == TokenType::TOK_STRUCT || tok.type == TokenType::TOK_UNION) {
        lexer_->next(); // consume 'struct' or 'union'
        auto decl = std::make_shared<ASTNode>(
            tok.type == TokenType::TOK_STRUCT ? NodeType::STRUCT_DECL : NodeType::UNION_DECL);

        // Check for 'packed' keyword
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_PACKED || tok.type == TokenType::TOK_UNPACKED) {
            decl->setAttribute("packing", tok.value);
            lexer_->next();
        }

        // Parse struct body
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_LBRACE) {
            lexer_->next(); // consume '{'
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RBRACE) { lexer_->next(); break; }
                if (tok.type == TokenType::TOK_IDENTIFIER || tok.type == TokenType::TOK_REG ||
                    tok.type == TokenType::TOK_WIRE || tok.type == TokenType::TOK_LOGIC) {
                    auto field = parseModuleItem();
                    if (field) {
                        // Extract field name and dimensions
                        std::string fname = field->attributes.count("name") ? field->attributes["name"] : "";
                        int fwidth = field->attributes.count("width") ? std::stoi(field->attributes["width"]) : 1;
                        field->setAttribute("struct_field", "1");
                        field->setAttribute("field_name", fname);
                        field->setAttribute("field_width", std::to_string(fwidth));
                        decl->children.push_back(field);
                    }
                } else {
                    lexer_->next(); // skip unknown
                }
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();
            }
        }

        // Get struct variable name
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_IDENTIFIER) {
            lexer_->next();
            decl->setAttribute("name", tok.value);
        }

        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_SEMICOLON) lexer_->next();
        return decl;
    }

    return nullptr;
}

std::shared_ptr<InterfaceDecl> Parser::parseInterface() {
    auto iface = std::make_shared<InterfaceDecl>();
    
    lexer_->next(); // consume 'interface'
    
    // Parse interface name
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected interface name");
        return nullptr;
    }
    iface->name = tok.value;
    iface->loc = SourceLocation("<input>", tok.line, tok.column);
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse interface items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDINTERFACE) {
            lexer_->next();
            break;
        }

        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in interface");
            break;
        }

        // Parse modport: modport name (input sig1, sig2, output sig3, ...);
        if (tok.type == TokenType::TOK_MODPORT) {
            lexer_->next(); // consume 'modport'
            auto mp = std::make_shared<ASTNode>(NodeType::MODPORT_DECL);
            tok = lexer_->next();
            if (tok.type == TokenType::TOK_IDENTIFIER) {
                mp->setAttribute("name", tok.value);
            }
            tok = lexer_->next(); // consume '('
            if (tok.type == TokenType::TOK_LPAREN) {
                while (should_continue()) {
                    tok = lexer_->peek();
                    if (tok.type == TokenType::TOK_RPAREN) { lexer_->next(); break; }
                    if (tok.type == TokenType::TOK_COMMA) { lexer_->next(); continue; }
                    // Parse direction: input/output/inout
                    std::string dir = "input";
                    if (tok.type == TokenType::TOK_INPUT || tok.type == TokenType::TOK_OUTPUT || tok.type == TokenType::TOK_INOUT) {
                        dir = tok.value;
                        lexer_->next();
                    }
                    // Parse signal name(s) until ',' or ')'
                    tok = lexer_->peek();
                    while (tok.type == TokenType::TOK_IDENTIFIER) {
                        lexer_->next();
                        auto sig = std::make_shared<ASTNode>(NodeType::IDENTIFIER);
                        sig->setAttribute("name", tok.value);
                        sig->setAttribute("direction", dir);
                        mp->addChild(sig);
                        tok = lexer_->peek();
                        if (tok.type == TokenType::TOK_COMMA) { lexer_->next(); tok = lexer_->peek(); }
                        else break;
                    }
                }
                tok = lexer_->next(); // consume ')'
                if (tok.type == TokenType::TOK_SEMICOLON) {} else {
                    // semicolon might be skipped, try to consume if peek is semicolon
                    if (lexer_->peek().type == TokenType::TOK_SEMICOLON) lexer_->next();
                }
            }
            iface->items.push_back(mp);
            continue;
        }

        auto item = parseModuleItem();
        if (item) {
            iface->items.push_back(item);
        }
    }
    
    return iface;
}

std::shared_ptr<PackageDecl> Parser::parsePackage() {
    auto pkg = std::make_shared<PackageDecl>();
    
    lexer_->next(); // consume 'package'
    
    // Parse package name
    Token tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected package name");
        return nullptr;
    }
    pkg->name = tok.value;
    pkg->loc = SourceLocation("<input>", tok.line, tok.column);
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse package items
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDPACKAGE) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in package");
            break;
        }
        
        auto item = parseModuleItem();
        if (item) {
            pkg->items.push_back(item);
        }
    }
    
    return pkg;
}

std::shared_ptr<ClassDecl> Parser::parseClass() {
    auto cls = std::make_shared<ClassDecl>();
    
    lexer_->next(); // consume 'class'
    
    // Parse optional lifetime
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_STATIC || tok.type == TokenType::TOK_AUTOMATIC) {
        lexer_->next();
    }
    
    // Parse class name
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_IDENTIFIER) {
        error("Expected class name");
        return nullptr;
    }
    cls->name = tok.value;
    cls->loc = SourceLocation("<input>", tok.line, tok.column);
    
    // Parse optional parameters
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_HASH) {
        lexer_->next(); // consume '#'
        tok = lexer_->next();
        if (tok.type == TokenType::TOK_LPAREN) {
            // Parse parameter list
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }
                // Parse parameter
                tok = lexer_->next();
                if (tok.type == TokenType::TOK_IDENTIFIER) {
                    cls->parameters.push_back(tok.value);
                }
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
        }
    }
    
    // Parse optional extends
    tok = lexer_->peek();
    if (tok.type == TokenType::TOK_EXTENDS) {
        lexer_->next();
        tok = lexer_->next();
        if (tok.type == TokenType::TOK_IDENTIFIER) {
            cls->extends.push_back(tok.value);
        }
    }
    
    // Consume ';'
    tok = lexer_->next();
    if (tok.type != TokenType::TOK_SEMICOLON) {
        error("Expected ';'");
    }
    
    // Parse class members
    while (should_continue()) {
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_ENDCLASS) {
            lexer_->next();
            break;
        }
        
        if (tok.type == TokenType::TOK_EOF) {
            error("Unexpected end of file in class");
            break;
        }
        
        auto item = parseModuleItem();
        if (item) {
            cls->members.push_back(item);
        }
    }
    
    return cls;
}

// Expression parsing with precedence climbing
std::shared_ptr<Expression> Parser::parseExpression() {
    return parseExpression1();
}

std::shared_ptr<Expression> Parser::parseExpression1() {
    auto left = parseExpression2();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_QUESTION) {
        lexer_->next(); // consume '?'
        auto middle = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_COLON) {
            error("Expected ':'");
        }
        auto right = parseExpression();

        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::TERNARY_OP;
        expr->op = Expression::TERNARY;
        expr->left = left;
        expr->right = middle;
        expr->third = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression2() {
    auto left = parseExpression3();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_PIPEPIPE) {
        lexer_->next(); // consume '||'
        auto right = parseExpression2();
        
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::LOR;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression3() {
    auto left = parseExpression4();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_AMPAMP) {
        lexer_->next(); // consume '&&'
        auto right = parseExpression3();
        
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::LAND;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression4() {
    auto left = parseExpression5();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_PIPE) {
        lexer_->next(); // consume '|'
        auto right = parseExpression4();
        
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::OR;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression5() {
    auto left = parseExpression6();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_CARET) {
        lexer_->next(); // consume '^'
        auto right = parseExpression5();
        
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::XOR;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression6() {
    auto left = parseExpression7();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_AMP) {
        lexer_->next(); // consume '&'
        auto right = parseExpression6();
        
        auto expr = std::make_shared<Expression>();
        expr->op = Expression::AND;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression7() {
    auto left = parseExpression8();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_EQ || tok.type == TokenType::TOK_NEQ ||
        tok.type == TokenType::TOK_EQL || tok.type == TokenType::TOK_NEL ||
        tok.type == TokenType::TOK_CASE_EQ || tok.type == TokenType::TOK_CASE_NEQ) {
        lexer_->next();

        Expression::Op op;
        switch (tok.type) {
            case TokenType::TOK_EQ: op = Expression::EQ; break;
            case TokenType::TOK_NEQ: op = Expression::NE; break;
            case TokenType::TOK_EQL: op = Expression::EQX; break;
            case TokenType::TOK_NEL: op = Expression::NEX; break;
            case TokenType::TOK_CASE_EQ: op = Expression::CASE_EQ; break;
            case TokenType::TOK_CASE_NEQ: op = Expression::CASE_NE; break;
            default: op = Expression::EQ; break;
        }

        auto right = parseExpression7();
        
        auto expr = std::make_shared<Expression>();
        expr->op = op;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression8() {
    auto left = parseExpression9();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LANGLE || tok.type == TokenType::TOK_RANGLE ||
        tok.type == TokenType::TOK_LEQ || tok.type == TokenType::TOK_GEQ) {
        lexer_->next();
        
        Expression::Op op;
        switch (tok.type) {
            case TokenType::TOK_LANGLE: op = Expression::LT; break;
            case TokenType::TOK_RANGLE: op = Expression::GT; break;
            case TokenType::TOK_LEQ: op = Expression::LE; break;
            case TokenType::TOK_GEQ: op = Expression::GE; break;
            default: op = Expression::LT; break;
        }
        
        auto right = parseExpression8();
        
        auto expr = std::make_shared<Expression>();
        expr->op = op;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression9() {
    auto left = parseExpression10();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_LTLT || tok.type == TokenType::TOK_GTGT || tok.type == TokenType::TOK_GTGTGT) {
        lexer_->next();

        Expression::Op op;
        if (tok.type == TokenType::TOK_LTLT) op = Expression::SHL;
        else if (tok.type == TokenType::TOK_GTGTGT) op = Expression::SSA; // arithmetic right shift
        else op = Expression::SHR;
        auto right = parseExpression9();
        
        auto expr = std::make_shared<Expression>();
        expr->op = op;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression10() {
    auto left = parseExpression11();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_PLUS || tok.type == TokenType::TOK_MINUS) {
        lexer_->next();
        
        Expression::Op op = (tok.type == TokenType::TOK_PLUS) ? Expression::ADD : Expression::SUB;
        auto right = parseExpression10();
        
        auto expr = std::make_shared<Expression>();
        expr->op = op;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parseExpression11() {
    auto left = parsePrimary();
    
    Token tok = lexer_->peek();
    if (tok.type == TokenType::TOK_STAR || tok.type == TokenType::TOK_SLASH || 
        tok.type == TokenType::TOK_PERCENT) {
        lexer_->next();
        
        Expression::Op op;
        switch (tok.type) {
            case TokenType::TOK_STAR: op = Expression::MUL; break;
            case TokenType::TOK_SLASH: op = Expression::DIV; break;
            case TokenType::TOK_PERCENT: op = Expression::MOD; break;
            default: op = Expression::MUL; break;
        }
        
        auto right = parseExpression11();
        
        auto expr = std::make_shared<Expression>();
        expr->op = op;
        expr->left = left;
        expr->right = right;
        return expr;
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::parsePrimary() {
    Token tok = lexer_->peek();
    
    // Unary operators
    if (tok.type == TokenType::TOK_MINUS || tok.type == TokenType::TOK_PLUS ||
        tok.type == TokenType::TOK_TILDE || tok.type == TokenType::TOK_AMP ||
        tok.type == TokenType::TOK_PIPE || tok.type == TokenType::TOK_CARET ||
        tok.type == TokenType::TOK_EXCLAIM) {
        lexer_->next();
        
        Expression::Op op;
        switch (tok.type) {
            case TokenType::TOK_MINUS: op = Expression::UMINUS; break;
            case TokenType::TOK_PLUS: op = Expression::UPLUS; break;
            case TokenType::TOK_TILDE: op = Expression::UNOT; break;
            case TokenType::TOK_AMP: op = Expression::UAND; break;
            case TokenType::TOK_PIPE: op = Expression::UOR; break;
            case TokenType::TOK_CARET: op = Expression::UXOR; break;
            case TokenType::TOK_EXCLAIM: op = Expression::ULNOT; break;
            default: op = Expression::UMINUS; break;
        }
        
        auto operand = parsePrimary();

        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::UNARY_OP;
        expr->op = op;
        expr->left = operand;
        return expr;
    }

    // Number literal
    if (tok.type == TokenType::TOK_INTEGER || tok.type == TokenType::TOK_REAL ||
        tok.type == TokenType::TOK_BINARY || tok.type == TokenType::TOK_OCTAL ||
        tok.type == TokenType::TOK_HEX) {
        lexer_->next();
        
        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::NUMBER;
        expr->setAttribute("value", tok.value);
        return expr;
    }
    
    // String literal
    if (tok.type == TokenType::TOK_STRING) {
        lexer_->next();
        
        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::STRING;
        expr->setAttribute("value", tok.value);
        return expr;
    }
    
    // Identifier
    if (tok.type == TokenType::TOK_IDENTIFIER) {
        lexer_->next();
        
        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::IDENTIFIER;
        expr->setAttribute("name", tok.value);
        
        // Check for bit select or part select: expr[index] or expr[msb:lsb]
        Token next = lexer_->peek();
        if (next.type == TokenType::TOK_LBRACKET) {
            lexer_->next(); // consume '['
            auto index = parseExpression();

            // Peek to check if this is a part select (has ':') or bit select
            Token colon_or_bracket = lexer_->peek();
            if (colon_or_bracket.type == TokenType::TOK_COLON) {
                // Part select: expr[msb:lsb]
                lexer_->next(); // consume ':'
                auto lsb = parseExpression();
                Token rbr = lexer_->next();
                if (rbr.type != TokenType::TOK_RBRACKET) {
                    error("Expected ']'");
                }
                auto partExpr = std::make_shared<Expression>();
                partExpr->op = Expression::PART_SELECT;
                partExpr->left = expr;
                partExpr->right = index;
                partExpr->third = lsb;
                return partExpr;
            } else {
                // Bit select: expr[index]
                Token rbr = lexer_->next();
                if (rbr.type != TokenType::TOK_RBRACKET) {
                    error("Expected ']'");
                }
                auto bitExpr = std::make_shared<Expression>();
                bitExpr->op = Expression::BIT_SELECT;
                bitExpr->left = expr;
                bitExpr->right = index;
                return bitExpr;
            }
        }
        
        // Check for function call
        next = lexer_->peek();
        if (next.type == TokenType::TOK_LPAREN) {
            lexer_->next(); // consume '('
            
            auto callExpr = std::make_shared<Expression>();
            callExpr->op = Expression::FUNCTION_CALL;
            callExpr->left = expr;
            
            // Parse arguments
            while (should_continue()) {
                next = lexer_->peek();
                if (next.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }
                
                auto arg = parseExpression();
                if (arg) {
                    callExpr->children.push_back(arg);
                }
                
                next = lexer_->peek();
                if (next.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
            
            return callExpr;
        }
        
        return expr;
    }
    
    // System task
    if (tok.type == TokenType::TOK_SYS_TASK) {
        lexer_->next();
        
        auto expr = std::make_shared<Expression>();
        expr->type = NodeType::FUNCTION_CALL;
        expr->setAttribute("name", tok.value);
        
        // Parse arguments if present
        tok = lexer_->peek();
        if (tok.type == TokenType::TOK_LPAREN) {
            lexer_->next(); // consume '('
            
            while (should_continue()) {
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_RPAREN) {
                    lexer_->next();
                    break;
                }
                
                auto arg = parseExpression();
                if (arg) {
                    expr->children.push_back(arg);
                }
                
                tok = lexer_->peek();
                if (tok.type == TokenType::TOK_COMMA) {
                    lexer_->next();
                }
            }
        }
        
        return expr;
    }
    
    // Parenthesized expression
    if (tok.type == TokenType::TOK_LPAREN) {
        lexer_->next(); // consume '('
        auto expr = parseExpression();
        tok = lexer_->next();
        if (tok.type != TokenType::TOK_RPAREN) {
            error("Expected ')'");
        }
        return expr;
    }
    
    // Concatenation
    if (tok.type == TokenType::TOK_LBRACE) {
        lexer_->next(); // consume '{'
        
        auto concat = std::make_shared<Expression>();
        concat->op = Expression::CONCAT;
        
        while (should_continue()) {
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_RBRACE) {
                lexer_->next();
                break;
            }
            
            auto elem = parseExpression();
            if (elem) {
                concat->children.push_back(elem);
            }
            
            tok = lexer_->peek();
            if (tok.type == TokenType::TOK_COMMA) {
                lexer_->next();
            }
        }
        
        return concat;
    }
    
    // Skip unknown token
    lexer_->next();
    return nullptr;
}

void Parser::error(const std::string &msg) {
    SourceLocation loc(lexer_->getFilename(), lexer_->getLine(), lexer_->getColumn());
    errors_.push_back(ParseError(msg, loc, "error"));
}

void Parser::warning(const std::string &msg) {
    SourceLocation loc(lexer_->getFilename(), lexer_->getLine(), lexer_->getColumn());
    warnings_.push_back(ParseError(msg, loc, "warning"));
}

void Parser::expect(TokenType type) {
    Token tok = lexer_->next();
    if (tok.type != type) {
        error("Expected " + tokenTypeToString(type) + ", got " + tokenTypeToString(tok.type));
    }
}

bool Parser::match(TokenType type) {
    Token tok = lexer_->peek();
    if (tok.type == type) {
        lexer_->next();
        return true;
    }
    return false;
}

bool Parser::check(TokenType type) {
    return lexer_->peek().type == type;
}

} // namespace VerilogParser
