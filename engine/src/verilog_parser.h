/**
 * Verilog Parser - Uses native RTLIL
 */

#ifndef VERILOG_PARSER_H
#define VERILOG_PARSER_H

#include "rtlil.h"
#include <string>
#include <vector>

namespace VerilogParser {

#ifndef VERILOG_PARSER_PARSE_ERROR_DEFINED
#define VERILOG_PARSER_PARSE_ERROR_DEFINED
struct ParseError {
    std::string filename;
    int line;
    int column;
    std::string message;
    std::string severity;
    ParseError() : line(0), column(0) {}
};
#endif // VERILOG_PARSER_PARSE_ERROR_DEFINED

std::vector<ParseError> parse_file(RTLIL::Design *design, const char *filename);
std::vector<ParseError> parse_string(RTLIL::Design *design, const char *code,
                                      size_t len, const char *name);

} // namespace VerilogParser

#endif /* VERILOG_PARSER_H */
