/**
 * RTLIL Compatibility Layer
 *
 * Provides backward-compatible API for the old RTLIL types
 * while using the new native RTLIL implementation.
 */

#ifndef RTLIL_COMPAT_H
#define RTLIL_COMPAT_H

#include "rtlil.h"

namespace RTLIL {

// Compatibility aliases
using ModulePtr = Module*;
using DesignPtr = Design*;
using WirePtr = Wire*;
using CellPtr = Cell*;

// Make new types work with old API patterns
// Old: mod->wires.push_back(wire)
// New: mod->addWire(name, width)

// Helper to convert old style access to new style
inline Wire *add_wire(Module *mod, const std::string &name, int width) {
    return mod->addWire(IdString("$" + name), width);
}

inline Cell *add_cell(Module *mod, const std::string &name, const std::string &type) {
    return mod->addCell(IdString("$" + name), IdString("$" + type));
}

inline Process *add_process(Module *mod, const std::string &name) {
    return mod->addProcess(IdString("$" + name));
}

inline Module *add_module(Design *design, const std::string &name) {
    return design->addModule(IdString("$" + name));
}

inline Module *find_module(Design *design, const std::string &name) {
    return design->findModule(IdString("$" + name));
}

inline Wire *find_wire(Module *mod, const std::string &name) {
    return mod->findWire(IdString("$" + name));
}

inline Cell *find_cell(Module *mod, const std::string &name) {
    return mod->findCell(IdString("$" + name));
}

// Helper to set wire properties
inline void set_wire_input(Wire *w) { w->port_input_ = PD_INPUT; }
inline void set_wire_output(Wire *w) { w->port_output_ = PD_OUTPUT; }
inline void set_wire_port_id(Wire *w, int id) { w->port_id_ = id; }

// Helper to set cell connections
inline void set_cell_port(Cell *c, const std::string &port, const SigSpec &sig) {
    c->setPort(IdString("$" + port), sig);
}

inline void set_cell_param(Cell *c, const std::string &param, const Const &val) {
    c->setParam(IdString("$" + param), val);
}

} // namespace RTLIL

#endif // RTLIL_COMPAT_H
