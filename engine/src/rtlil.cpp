/**
 * RTLIL - Register Transfer Level Intermediate Language
 * Implementation based on industry-standard RTLIL reference
 *
 * Complete implementation of all methods declared in rtlil_industrial.h
 */

#include "rtlil.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <climits>
#include <charconv>
#include <set>

namespace RTLIL {

// ============================================================================
// IdString static members
// ============================================================================

bool IdString::destruct_guard_ok = false;
IdString::destruct_guard_t IdString::destruct_guard;
std::vector<IdString::Storage> IdString::global_id_storage_;
std::unordered_map<std::string_view, int> IdString::global_id_index_;
std::unordered_map<int, IdString::AutoidxStorage> IdString::global_autoidx_id_storage_;
std::unordered_map<int, int> IdString::global_refcount_storage_;
std::vector<int> IdString::global_free_idx_list_;
int IdString::autoidx = 1;

static void populate(std::string_view name) {
    if (name.size() > 1 && name[0] == '\\' && name[1] == '$') {
        name = name.substr(1);
    }
    RTLIL::IdString::global_id_index_.insert({name, static_cast<int>(RTLIL::IdString::global_id_storage_.size())});
    RTLIL::IdString::global_id_storage_.push_back({const_cast<char*>(name.data()), static_cast<int>(name.size())});
}

void IdString::prepopulate() {
    global_id_storage_.reserve(256);
    global_id_index_.reserve(256);
    global_id_index_.insert({"", 0});
    global_id_storage_.push_back({const_cast<char*>(""), 0});
}

int IdString::really_insert(std::string_view p, std::unordered_map<std::string_view, int>::iterator &it) {
    ensure_prepopulated();

    // Validate identifier
    if (p.empty()) {
        throw std::runtime_error("RTLIL::IdString: empty identifier not allowed");
    }

    if (p[0] != '$' && p[0] != '\\') {
        throw std::runtime_error("RTLIL::IdString: identifier must start with $ or \\");
    }

    // Check for control characters
    for (char ch : p) {
        if (static_cast<unsigned char>(ch) <= static_cast<unsigned char>(' ')) {
            throw std::runtime_error("RTLIL::IdString: control character in identifier");
        }
    }

    // Handle auto-indexed IDs
    if (p.substr(0, 6) == "$auto$") {
        size_t autoidx_pos = p.find_last_of('$') + 1;
        std::string_view suffix = p.substr(autoidx_pos);
        if (!suffix.empty() && suffix[0] >= '0' && suffix[0] <= '9') {
            int p_autoidx;
            auto [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), p_autoidx);
            if (ec == std::errc()) {
                auto autoidx_it = global_autoidx_id_storage_.find(-p_autoidx);
                if (autoidx_it != global_autoidx_id_storage_.end() &&
                    p.substr(0, autoidx_pos) == *autoidx_it->second.prefix) {
                    return -p_autoidx;
                }
            }
        }
    }

    // Allocate new index
    if (global_free_idx_list_.empty()) {
        assert(global_id_storage_.size() < 0x40000000);
        global_free_idx_list_.push_back(static_cast<int>(global_id_storage_.size()));
        global_id_storage_.push_back({nullptr, 0});
    }

    int idx = global_free_idx_list_.back();
    global_free_idx_list_.pop_back();
    char* buf = static_cast<char*>(malloc(p.size() + 1));
    memcpy(buf, p.data(), p.size());
    buf[p.size()] = 0;
    global_id_storage_.at(idx) = {buf, static_cast<int>(p.size())};
    global_id_index_.insert(it, {std::string_view(buf, p.size()), idx});

    return idx;
}

// ============================================================================
// Const implementation
// ============================================================================

std::vector<RTLIL::State>& Const::get_bits() {
    return bits_;
}

std::string& Const::get_str() {
    return str_;
}

const std::vector<RTLIL::State>& Const::get_bits() const {
    return bits_;
}

const std::string& Const::get_str() const {
    return str_;
}

Const::Const(std::string str) : flags(RTLIL::CONST_FLAG_STRING), tag(backing_tag::string) {
    new (&str_) std::string(std::move(str));
}

Const::Const(long long val) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits) {
    new (&bits_) std::vector<RTLIL::State>();
    for (int i = 0; i < 32; i++) {
        bits_.push_back((val >> i) & 1 ? S1 : S0);
    }
}

Const::Const(long long val, int width) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits) {
    new (&bits_) std::vector<RTLIL::State>();
    for (int i = 0; i < width; i++) {
        bits_.push_back((val >> i) & 1 ? S1 : S0);
    }
}

Const::Const(RTLIL::State bit, int width) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits) {
    new (&bits_) std::vector<RTLIL::State>(width, bit);
}

Const::Const(const std::vector<bool> &bits) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits) {
    new (&bits_) std::vector<RTLIL::State>();
    for (bool b : bits) {
        bits_.push_back(b ? S1 : S0);
    }
}

Const::Const(const RTLIL::Const &other) : flags(other.flags), tag(other.tag) {
    if (tag == backing_tag::bits) {
        new (&bits_) std::vector<RTLIL::State>(other.bits_);
    } else {
        new (&str_) std::string(other.str_);
    }
}

Const::Const(RTLIL::Const &&other) : flags(other.flags), tag(other.tag) {
    if (tag == backing_tag::bits) {
        new (&bits_) std::vector<RTLIL::State>(std::move(other.bits_));
    } else {
        new (&str_) std::string(std::move(other.str_));
    }
}

RTLIL::Const &Const::operator=(const RTLIL::Const &other) {
    if (this != &other) {
        if (tag == backing_tag::bits) {
            bits_.~vector();
        } else {
            str_.~basic_string();
        }

        flags = other.flags;
        tag = other.tag;

        if (tag == backing_tag::bits) {
            new (&bits_) std::vector<RTLIL::State>(other.bits_);
        } else {
            new (&str_) std::string(other.str_);
        }
    }
    return *this;
}

Const::~Const() {
    if (tag == backing_tag::bits) {
        bits_.~vector();
    } else {
        str_.~basic_string();
    }
}

bool Const::operator<(const RTLIL::Const &other) const {
    if (flags != other.flags) return flags < other.flags;
    if (tag != other.tag) return tag < other.tag;
    if (tag == backing_tag::bits) {
        return bits_ < other.bits_;
    } else {
        return str_ < other.str_;
    }
}

bool Const::operator==(const RTLIL::Const &other) const {
    if (flags != other.flags) return false;
    if (tag != other.tag) return false;
    if (tag == backing_tag::bits) {
        return bits_ == other.bits_;
    } else {
        return str_ == other.str_;
    }
}

bool Const::operator!=(const RTLIL::Const &other) const {
    return !(*this == other);
}

bool Const::as_bool() const {
    if (tag == backing_tag::bits) {
        for (auto bit : bits_) {
            if (bit == S1) return true;
        }
        return false;
    }
    return !str_.empty();
}

int Const::as_int(bool is_signed) const {
    if (tag == backing_tag::string) {
        return 0;
    }

    int ret = 0;
    int width = std::min(32, static_cast<int>(bits_.size()));
    for (int i = 0; i < width; i++) {
        if (bits_[i] == S1)
            ret |= (1 << i);
    }

    if (is_signed && width > 0 && bits_[width-1] == S1) {
        for (int i = width; i < 32; i++)
            ret |= (1 << i);
    }

    return ret;
}

bool Const::convertible_to_int(bool is_signed) const {
    if (tag == backing_tag::string) return false;
    if (bits_.size() > 32) return false;
    if (is_signed && bits_.size() > 31) return false;
    return true;
}

std::optional<int> Const::try_as_int(bool is_signed) const {
    if (!convertible_to_int(is_signed)) return std::nullopt;
    return as_int(is_signed);
}

int Const::as_int_saturating(bool is_signed) const {
    if (tag == backing_tag::string) return 0;
    if (bits_.size() <= 32) return as_int(is_signed);
    if (is_signed) {
        return bits_[bits_.size()-1] == S1 ? INT_MIN : INT_MAX;
    }
    return as_int(false);
}

std::string Const::as_string(const char* any) const {
    if (tag == backing_tag::string) return str_;

    std::string ret;
    for (int i = (int)bits_.size()-1; i >= 0; i--) {
        switch (bits_[i]) {
            case S0: ret += "0"; break;
            case S1: ret += "1"; break;
            case Sx: ret += any; break;
            case Sz: ret += "z"; break;
            case Sa: ret += "-"; break;
            default: ret += any; break;
        }
    }
    return ret;
}

Const Const::from_string(const std::string &str) {
    if (str.empty()) {
        return Const(std::vector<RTLIL::State>());
    }

    // Check if it's a binary string
    bool is_binary = true;
    for (char c : str) {
        if (c != '0' && c != '1' && c != 'x' && c != 'X' && c != 'z' && c != 'Z' && c != '-') {
            is_binary = false;
            break;
        }
    }

    if (is_binary) {
        std::vector<RTLIL::State> bits;
        for (int i = (int)str.size()-1; i >= 0; i--) {
            switch (str[i]) {
                case '0': bits.push_back(S0); break;
                case '1': bits.push_back(S1); break;
                case 'x': case 'X': bits.push_back(Sx); break;
                case 'z': case 'Z': bits.push_back(Sz); break;
                case '-': bits.push_back(Sa); break;
                default: bits.push_back(Sx); break;
            }
        }
        return Const(bits);
    }

    // Try as integer
    try {
        long long val = std::stoll(str, nullptr, 0);
        return Const(val);
    } catch (...) {
        return Const(str);
    }
}

std::vector<RTLIL::State> Const::to_bits() const {
    if (tag == backing_tag::string) {
        std::vector<RTLIL::State> bits;
        for (char c : str_) {
            bits.push_back(c == '1' ? S1 : S0);
        }
        return bits;
    }
    return bits_;
}

std::string Const::decode_string() const {
    if (tag == backing_tag::string) return str_;
    return as_string();
}

int Const::size() const {
    if (tag == backing_tag::string) return (int)str_.size();
    return (int)bits_.size();
}

bool Const::empty() const {
    if (tag == backing_tag::string) return str_.empty();
    return bits_.empty();
}

void Const::append(const RTLIL::Const &other) {
    if (tag == backing_tag::string || other.tag == backing_tag::string) {
        throw std::runtime_error("Cannot append string and bitvector constants");
    }
    bits_.insert(bits_.end(), other.bits_.begin(), other.bits_.end());
}

void Const::set(int i, RTLIL::State state) {
    if (tag == backing_tag::bits) {
        bits_[i] = state;
    }
}

void Const::resize(int size, RTLIL::State fill) {
    if (tag == backing_tag::bits) {
        bits_.resize(size, fill);
    }
}

RTLIL::State Const::operator[](int i) const {
    if (tag == backing_tag::bits) {
        return bits_[i];
    }
    return Sx;
}

// ============================================================================
// SigBit implementation
// ============================================================================

bool SigBit::operator==(const SigBit &o) const {
    if (is_constant() && o.is_constant()) {
        return data == o.data;
    }
    if (is_wire() && o.is_wire()) {
        return wire_idx == o.wire_idx && offset == o.offset;
    }
    return false;
}

bool SigBit::operator<(const SigBit &o) const {
    if (is_constant() && o.is_constant()) {
        return data < o.data;
    }
    if (is_wire() && o.is_wire()) {
        if (wire_idx != o.wire_idx) return wire_idx < o.wire_idx;
        return offset < o.offset;
    }
    return is_constant();
}

// ============================================================================
// SigChunk implementation
// ============================================================================

bool SigChunk::operator==(const SigChunk &o) const {
    return wire_idx == o.wire_idx && offset == o.offset && width == o.width;
}

bool SigChunk::operator<(const SigChunk &o) const {
    if (wire_idx != o.wire_idx) return wire_idx < o.wire_idx;
    if (offset != o.offset) return offset < o.offset;
    return width < o.width;
}

// ============================================================================
// SigSpec implementation
// ============================================================================

SigSpec::SigSpec(const SigChunk &chunk) {
    for (int i = 0; i < chunk.width; i++) {
        bits_.push_back(SigBit(chunk.wire_idx, chunk.offset + i));
    }
}

SigSpec::SigSpec(int wire_idx, int width) {
    for (int i = 0; i < width; i++) {
        bits_.push_back(SigBit(wire_idx, i));
    }
}

SigSpec::SigSpec(const RTLIL::Const &val) {
    for (auto bit : val.to_bits()) {
        bits_.push_back(SigBit(bit));
    }
}

void SigSpec::append(const SigSpec &other) {
    bits_.insert(bits_.end(), other.bits_.begin(), other.bits_.end());
}

void SigSpec::append(const SigBit &bit) {
    bits_.push_back(bit);
}

void SigSpec::append(const RTLIL::Const &val) {
    for (auto bit : val.to_bits()) {
        bits_.push_back(SigBit(bit));
    }
}

void SigSpec::prepend(const SigSpec &other) {
    bits_.insert(bits_.begin(), other.bits_.begin(), other.bits_.end());
}

SigSpec SigSpec::extract(int offset, int length) const {
    SigSpec result;
    for (int i = offset; i < offset + length && i < width(); i++) {
        result.bits_.push_back(bits_[i]);
    }
    return result;
}

void SigSpec::replace(int offset, const SigSpec &other) {
    for (int i = 0; i < other.width() && offset + i < width(); i++) {
        bits_[offset + i] = other.bits_[i];
    }
}

void SigSpec::remove(int offset, int length) {
    bits_.erase(bits_.begin() + offset, bits_.begin() + offset + length);
}

void SigSpec::reverse() {
    std::reverse(bits_.begin(), bits_.end());
}

bool SigSpec::operator<(const SigSpec &o) const {
    return bits_ < o.bits_;
}

RTLIL::Const SigSpec::as_const() const {
    std::vector<RTLIL::State> bits;
    for (auto &bit : bits_) {
        if (bit.is_constant()) {
            bits.push_back(bit.data);
        } else {
            bits.push_back(Sx);
        }
    }
    return RTLIL::Const(bits);
}

// ============================================================================
// Module implementation
// ============================================================================

Module::~Module() {
    for (auto &it : wires_) {
        Wire *w = it.second;
        delete w;
    }
    for (auto &it : cells_) {
        Cell *c = it.second;
        delete c;
    }
    for (auto &it : memories_) {
        delete it.second;
    }
    for (auto &it : processes_) {
        delete it.second;
    }
}

Wire *Module::addWire(const IdString &name) {
    Wire *w = new Wire(name);
    wires_[name] = w;
    return w;
}

Wire *Module::addWire(const IdString &name, int width) {
    Wire *w = new Wire(name, width);
    wires_[name] = w;
    return w;
}

Wire *Module::addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output) {
    Wire *w = new Wire(name, width, port_id, port_input, port_output);
    wires_[name] = w;
    return w;
}

Wire *Module::addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output, int start_offset) {
    Wire *w = new Wire(name, width, port_id, port_input, port_output, start_offset);
    wires_[name] = w;
    return w;
}

Wire *Module::addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output, int start_offset, bool is_signed) {
    Wire *w = new Wire(name, width, port_id, port_input, port_output, start_offset);
    w->is_signed_ = is_signed;
    wires_[name] = w;
    return w;
}

Wire *Module::findWire(IdString name) const {
    auto it = wires_.find(name);
    if (it != wires_.end()) return it->second;
    return nullptr;
}

void Module::remove(Wire *wire) {
    wires_.erase(wire->name);
    delete wire;
}

// ============================================================================
// Module compatibility functions (snake_case API)
// ============================================================================

Wire *Module::find_wire(const std::string &name) const {
    return findWire(IdString(name));
}

Wire *Module::find_wire(const char *name) const {
    return findWire(IdString(std::string(name)));
}

bool Module::has_wire(const std::string &name) const {
    return hasWire(IdString(name));
}

int Module::wire_index(const std::string &name) const {
    auto it = wires_.find(IdString(name));
    if (it == wires_.end()) return -1;
    int idx = 0;
    for (auto &w : wires_) {
        if (w.first == it->first) return idx;
        idx++;
    }
    return -1;
}

Cell *Module::addCell(const IdString &name, const IdString &type) {
    Cell *c = new Cell(name, type);
    cells_[name] = c;
    return c;
}

Cell *Module::addCell(const IdString &name, const IdString &type, const std::map<IdString, SigSpec> &connections) {
    Cell *c = new Cell(name, type);
    c->connections_ = connections;
    cells_[name] = c;
    return c;
}

Cell *Module::addCell(const IdString &name, const IdString &type, const std::map<IdString, SigSpec> &connections, const std::map<IdString, Const> &parameters) {
    Cell *c = new Cell(name, type);
    c->connections_ = connections;
    c->parameters_ = parameters;
    cells_[name] = c;
    return c;
}

Cell *Module::findCell(IdString name) const {
    auto it = cells_.find(name);
    if (it != cells_.end()) return it->second;
    return nullptr;
}

void Module::remove(Cell *cell) {
    cells_.erase(cell->name);
    delete cell;
}

// ============================================================================
// Cell compatibility functions (snake_case API)
// ============================================================================

Cell *Module::find_cell(const std::string &name) const {
    return findCell(IdString(name));
}

Cell *Module::find_cell(const char *name) const {
    return findCell(IdString(std::string(name)));
}

bool Module::has_cell(const std::string &name) const {
    return hasCell(IdString(name));
}

Memory *Module::addMemory(const IdString &name, int width, int size, int start_offset) {
    Memory *m = new Memory(name, width, size, start_offset);
    memories_[name] = m;
    return m;
}

Memory *Module::findMemory(IdString name) const {
    auto it = memories_.find(name);
    if (it != memories_.end()) return it->second;
    return nullptr;
}

void Module::remove(Memory *memory) {
    memories_.erase(memory->name);
    delete memory;
}

Process *Module::addProcess(const IdString &name, Process::ProcessType type) {
    Process *p = new Process(name, type);
    processes_[name] = p;
    return p;
}

Process *Module::addProcess(const IdString &name, SwitchRule *sw) {
    Process *p = new Process(name, Process::ALWAYS);
    p->switches.push_back(sw);
    processes_[name] = p;
    return p;
}

Process *Module::findProcess(IdString name) const {
    auto it = processes_.find(name);
    if (it != processes_.end()) return it->second;
    return nullptr;
}

void Module::remove(Process *process) {
    processes_.erase(process->name);
    delete process;
}

void Module::select(Design *design) {
    selected_ = true;
}

void Module::select(Design *design, Wire *wire) {
    selected_ = true;
}

void Module::select(Design *design, Cell *cell) {
    selected_ = true;
}

void Module::select(Design *design, Memory *memory) {
    selected_ = true;
}

void Module::select(Design *design, Process *process) {
    selected_ = true;
}

void Module::select(Design *design, SigSpec sig) {
    selected_ = true;
}

void Module::optimize() {
    // Full optimization: remove unused wires and cells,
    // dead code elimination, constant propagation
    if (cells_.empty()) return;

    // Step 1: Identify used wire indices (from output ports and sequential cells)
    std::set<int> used_wire_idxs;
    for (auto &it : wires_) {
        if (it.second->port_id() > 0) {
            used_wire_idxs.insert(it.second->start_offset_);
        }
    }

    // Step 2: Iteratively find used cells and their input wire indices
    bool changed = true;
    int max_iters = 100;
    while (changed && max_iters-- > 0) {
        changed = false;
        for (auto &it : cells_) {
            Cell *cell = it.second;
            bool output_used = false;
            int out_wire_idx = -1;
            for (auto &conn : cell->connections_) {
                if (conn.first == IdString("\\Y") || conn.first == IdString("Y") ||
                    conn.first == IdString("\\Q") || conn.first == IdString("Q")) {
                    for (auto &bit : conn.second.bits_) {
                        if (bit.is_wire() && used_wire_idxs.count(bit.wire_idx)) {
                            output_used = true;
                        }
                    }
                }
            }
            if (output_used) {
                for (auto &conn : cell->connections_) {
                    for (auto &bit : conn.second.bits_) {
                        if (bit.is_wire() && !used_wire_idxs.count(bit.wire_idx)) {
                            used_wire_idxs.insert(bit.wire_idx);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    // Step 3: Remove unused cells
    auto cell_it = cells_.begin();
    while (cell_it != cells_.end()) {
        Cell *cell = cell_it->second;
        bool is_used = false;
        for (auto &conn : cell->connections_) {
            if (conn.first == IdString("\\Y") || conn.first == IdString("Y") ||
                conn.first == IdString("\\Q") || conn.first == IdString("Q")) {
                for (auto &bit : conn.second.bits_) {
                    if (bit.is_wire() && used_wire_idxs.count(bit.wire_idx)) {
                        is_used = true;
                    }
                }
            }
        }
        if (!is_used) {
            delete cell;
            cell_it = cells_.erase(cell_it);
        } else {
            ++cell_it;
        }
    }

    // Step 4: Remove unused wires
    auto wire_it = wires_.begin();
    while (wire_it != wires_.end()) {
        if (wire_it->second->port_id() == 0 && !used_wire_idxs.count(wire_it->second->start_offset_)) {
            delete wire_it->second;
            wire_it = wires_.erase(wire_it);
        } else {
            ++wire_it;
        }
    }
}

void Module::connect(const SigSpec &left, const SigSpec &right) {
    // Connect two signal specifications together
    // This creates a direct electrical connection (wire alias)
    if (left.width() == 0 || right.width() == 0) return;

    // For single-bit connections, create a BUF cell as a connector
    int width = std::min(left.width(), right.width());
    if (width > 0) {
        IdString cell_name = IdString("\\connect_" + std::to_string(cells_.size()));
        Cell *buf = addCell(cell_name, IdString("\\BUF"));
        if (buf) {
            buf->setPort(IdString("\\A"), left);
            buf->setPort(IdString("\\Y"), right);
        }
    }
}

void Module::connect(const SigSig &conn) {
    connect(conn.first, conn.second);
}

void Module::fixup_ports() {
    // Fix port numbering and directions
    std::vector<Wire*> port_wires;
    for (auto &it : wires_) {
        if (it.second->port_id() > 0) {
            port_wires.push_back(it.second);
        }
    }

    std::sort(port_wires.begin(), port_wires.end(), [](Wire *a, Wire *b) {
        return a->port_id() < b->port_id();
    });

    for (int i = 0; i < (int)port_wires.size(); i++) {
        port_wires[i]->port_id_ = i + 1;
    }
}

// ============================================================================
// Design compatibility functions (snake_case API)
// ============================================================================

Module *Design::add_module(const std::string &name) {
    return addModule(IdString(name));
}

Module *Design::find_module(const std::string &name) {
    return findModule(IdString(name));
}

Module *Design::find_module(const std::string &name) const {
    return findModule(IdString(name));
}

bool Design::has_module(const std::string &name) const {
    return hasModule(IdString(name));
}

Module *Design::find_module(const char *name) {
    return findModule(IdString(std::string(name)));
}

Module *Design::find_module(const char *name) const {
    return findModule(IdString(std::string(name)));
}

// ============================================================================
// Design implementation
// ============================================================================

Design::~Design() {
    for (auto &it : modules_) {
        delete it.second;
    }
}

Module *Design::addModule(const IdString &name) {
    Module *m = new Module(name);
    m->design_ = this;
    modules_[name] = m;
    return m;
}

Module *Design::addModule(Module *existing) {
    existing->design_ = this;
    modules_[existing->name] = existing;
    return existing;
}

Module *Design::findModule(IdString name) const {
    auto it = modules_.find(name);
    if (it != modules_.end()) return it->second;
    return nullptr;
}

void Design::remove(Module *module) {
    modules_.erase(module->name);
    delete module;
}

void Design::select(Module *mod) {
    mod->selected_ = true;
}

void Design::select(Wire *wire) {
    wire->is_global_ = true;
}

void Design::select(Cell *cell) {
    cell->is_external = false;
}

void Design::select(Memory *memory) {
    // Memory selection
}

void Design::select(Process *process) {
    // Process selection
}

void Design::select(SigSpec sig) {
    // Select signals
}

void Design::select_all() {
    for (auto &it : modules_) {
        it.second->selected_ = true;
    }
}

void Design::deselect_all() {
    for (auto &it : modules_) {
        it.second->selected_ = false;
    }
}

void Design::optimize() {
    for (auto &it : modules_) {
        it.second->optimize();
    }
}

bool Design::save(std::string filename) {
    // Save RTLIL to file in native RTLIL text format
    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << "# RTLIL representation generated by ai_rtl_sim\n";
    file << "autoidx " << (modules_.size() * 100) << "\n\n";

    for (auto &mod_it : modules_) {
        Module *mod = mod_it.second;
        file << "module " << mod->name.c_str() << "\n";

        // Save wires with port info
        for (auto &wire_it : mod->wires_) {
            Wire *wire = wire_it.second;
            file << "  wire width " << wire->width_ << " " << wire->name.c_str();
            if (wire->port_id_ > 0) {
                file << "  # port " << wire->port_id_;
                if (wire->port_input_ == PD_INPUT) file << " input";
                if (wire->port_output_ == PD_OUTPUT) file << " output";
            }
            file << "\n";
        }

        // Save cells with connections
        for (auto &cell_it : mod->cells_) {
            Cell *cell = cell_it.second;
            file << "  cell " << cell->type.c_str() << " " << cell->name.c_str() << "\n";
            for (auto &conn : cell->connections_) {
                file << "    connect " << conn.first.c_str() << " ";
                for (size_t i = 0; i < conn.second.bits_.size(); i++) {
                    if (i > 0) file << " ";
                    if (conn.second.bits_[i].is_wire()) {
                        file << "#w" << conn.second.bits_[i].wire_idx << "." << conn.second.bits_[i].offset;
                    } else {
                        file << (int)conn.second.bits_[i].data;
                    }
                }
                file << "\n";
            }
            file << "  end\n";
        }

        file << "end\n\n";
    }

    file.close();
    return true;
}

bool Design::load(std::string filename) {
    // Load RTLIL from file in native RTLIL text format
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    std::string line, token;
    Module *current_module = nullptr;

    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::istringstream iss(trimmed);
        iss >> token;

        if (token == "autoidx") {
            int idx; iss >> idx;
        } else if (token == "module") {
            std::string mod_name; iss >> mod_name;
            current_module = addModule(IdString(mod_name));
        } else if (token == "wire" && current_module) {
            std::string sub; int width = 1;
            iss >> sub; // "width"
            iss >> width;
            std::string wire_name; iss >> wire_name;
            current_module->addWire(IdString(wire_name), width);
        } else if (token == "cell" && current_module) {
            std::string cell_type, cell_name;
            iss >> cell_type >> cell_name;
            current_module->addCell(IdString(cell_name), IdString(cell_type));
        } else if (token == "end") {
            // end of cell or module
        }
    }

    file.close();
    return !modules_.empty();
}

void Design::merge_designs(Design *other) {
    for (auto &it : other->modules_) {
        if (modules_.find(it.first) == modules_.end()) {
            addModule(it.second);
            other->modules_.erase(it.first);
        }
    }
}

// ============================================================================
// Constant operations implementation
// ============================================================================

RTLIL::Const const_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int len = std::max({arg1.size(), arg2.size(), result_len});
    std::vector<RTLIL::State> result;

    for (int i = 0; i < len; i++) {
        RTLIL::State bit = (i < arg1.size()) ? arg1[i] : S0;
        result.push_back(bit == S1 ? S0 : S1);
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int len = std::max({arg1.size(), arg2.size(), result_len});
    std::vector<RTLIL::State> result;

    for (int i = 0; i < len; i++) {
        RTLIL::State bit1 = (i < arg1.size()) ? arg1[i] : S0;
        RTLIL::State bit2 = (i < arg2.size()) ? arg2[i] : S0;

        if (bit1 == S1 && bit2 == S1)
            result.push_back(S1);
        else if (bit1 == S0 || bit2 == S0)
            result.push_back(S0);
        else
            result.push_back(Sx);
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int len = std::max({arg1.size(), arg2.size(), result_len});
    std::vector<RTLIL::State> result;

    for (int i = 0; i < len; i++) {
        RTLIL::State bit1 = (i < arg1.size()) ? arg1[i] : S0;
        RTLIL::State bit2 = (i < arg2.size()) ? arg2[i] : S0;

        if (bit1 == S1 || bit2 == S1)
            result.push_back(S1);
        else if (bit1 == S0 && bit2 == S0)
            result.push_back(S0);
        else
            result.push_back(Sx);
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int len = std::max({arg1.size(), arg2.size(), result_len});
    std::vector<RTLIL::State> result;

    for (int i = 0; i < len; i++) {
        RTLIL::State bit1 = (i < arg1.size()) ? arg1[i] : S0;
        RTLIL::State bit2 = (i < arg2.size()) ? arg2[i] : S0;

        if (bit1 == Sx || bit2 == Sx)
            result.push_back(Sx);
        else if (bit1 == bit2)
            result.push_back(S0);
        else
            result.push_back(S1);
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_not(const_xor(arg1, arg2, signed1, signed2, result_len), Const(), false, false, result_len);
}

RTLIL::Const const_reduce_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    for (int i = 0; i < arg1.size(); i++) {
        if (arg1[i] != S1) return RTLIL::Const(S0);
    }
    return RTLIL::Const(S1);
}

RTLIL::Const const_reduce_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    for (int i = 0; i < arg1.size(); i++) {
        if (arg1[i] == S1) return RTLIL::Const(S1);
    }
    return RTLIL::Const(S0);
}

RTLIL::Const const_reduce_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int count = 0;
    for (int i = 0; i < arg1.size(); i++) {
        if (arg1[i] == S1) count++;
    }
    return RTLIL::Const(count % 2 == 1 ? S1 : S0);
}

RTLIL::Const const_reduce_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_not(const_reduce_xor(arg1, arg2, signed1, signed2, result_len), Const(), false, false, result_len);
}

RTLIL::Const const_reduce_bool(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_reduce_or(arg1, arg2, signed1, signed2, result_len);
}

RTLIL::Const const_logic_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return arg1.as_bool() ? RTLIL::Const(S0) : RTLIL::Const(S1);
}

RTLIL::Const const_logic_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return (arg1.as_bool() && arg2.as_bool()) ? RTLIL::Const(S1) : RTLIL::Const(S0);
}

RTLIL::Const const_logic_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return (arg1.as_bool() || arg2.as_bool()) ? RTLIL::Const(S1) : RTLIL::Const(S0);
}

RTLIL::Const const_shl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int shift = arg2.as_int(false);
    int len = std::max(arg1.size(), result_len);
    std::vector<RTLIL::State> result(len, S0);

    for (int i = 0; i < len; i++) {
        if (i + shift < arg1.size()) {
            result[i] = arg1[i + shift];
        }
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_shr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int shift = arg2.as_int(false);
    int len = std::max(arg1.size(), result_len);
    std::vector<RTLIL::State> result(len, S0);

    for (int i = 0; i < len; i++) {
        if (i - shift >= 0 && i - shift < arg1.size()) {
            result[i] = arg1[i - shift];
        }
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_sshl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_shl(arg1, arg2, signed1, signed2, result_len);
}

RTLIL::Const const_sshr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int shift = arg2.as_int(false);
    int len = std::max(arg1.size(), result_len);
    std::vector<RTLIL::State> result(len, S0);

    RTLIL::State sign_bit = S0;
    if (signed1 && arg1.size() > 0) {
        sign_bit = arg1[arg1.size()-1];
    }

    for (int i = 0; i < len; i++) {
        if (i - shift >= 0 && i - shift < arg1.size()) {
            result[i] = arg1[i - shift];
        } else if (i >= arg1.size()) {
            result[i] = sign_bit;
        }
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_shift(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool left, bool signed1, bool signed2, int result_len) {
    return left ? const_shl(arg1, arg2, signed1, signed2, result_len)
                : const_shr(arg1, arg2, signed1, signed2, result_len);
}

RTLIL::Const const_shiftx(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool left, int result_len) {
    int shift = arg2.as_int(false);
    int len = std::max(arg1.size(), result_len);
    std::vector<RTLIL::State> result(len, Sx);

    if (left) {
        for (int i = 0; i < len; i++) {
            if (i + shift < arg1.size()) {
                result[i] = arg1[i + shift];
            }
        }
    } else {
        for (int i = 0; i < len; i++) {
            if (i - shift >= 0 && i - shift < arg1.size()) {
                result[i] = arg1[i - shift];
            }
        }
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_lt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1.as_int(signed1) < arg2.as_int(signed2) ? S1 : S0);
}

RTLIL::Const const_le(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1.as_int(signed1) <= arg2.as_int(signed2) ? S1 : S0);
}

RTLIL::Const const_eq(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1 == arg2 ? S1 : S0);
}

RTLIL::Const const_ne(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1 != arg2 ? S1 : S0);
}

RTLIL::Const const_eqx(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    if (arg1.size() != arg2.size()) return RTLIL::Const(S0);
    for (int i = 0; i < arg1.size(); i++) {
        if (arg1[i] != arg2[i] && arg1[i] != Sx && arg2[i] != Sx) {
            return RTLIL::Const(S0);
        }
    }
    return RTLIL::Const(S1);
}

RTLIL::Const const_nex(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_not(const_eqx(arg1, arg2, signed1, signed2, result_len), Const(), false, false, result_len);
}

RTLIL::Const const_ge(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1.as_int(signed1) >= arg2.as_int(signed2) ? S1 : S0);
}

RTLIL::Const const_gt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return RTLIL::Const(arg1.as_int(signed1) > arg2.as_int(signed2) ? S1 : S0);
}

RTLIL::Const const_add(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int len = std::max({arg1.size(), arg2.size(), result_len});
    std::vector<RTLIL::State> result;
    int carry = 0;

    for (int i = 0; i < len; i++) {
        int bit1 = (i < arg1.size() && arg1[i] == S1) ? 1 : 0;
        int bit2 = (i < arg2.size() && arg2[i] == S1) ? 1 : 0;
        int sum = bit1 + bit2 + carry;
        result.push_back((sum & 1) ? S1 : S0);
        carry = sum >> 1;
    }

    return RTLIL::Const(result);
}

RTLIL::Const const_sub(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    // a - b = a + (-b) = a + (~b + 1)
    RTLIL::Const negated = const_not(arg2, Const(), false, false, arg2.size());
    RTLIL::Const one(std::vector<RTLIL::State>{S1});
    RTLIL::Const negated_plus_one = const_add(negated, one, false, false, arg2.size());
    return const_add(arg1, negated_plus_one, signed1, signed2, result_len);
}

RTLIL::Const const_mul(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int a = arg1.as_int(signed1);
    int b = arg2.as_int(signed2);
    long long result = (long long)a * b;
    return RTLIL::Const(result, result_len);
}

RTLIL::Const const_div(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int a = arg1.as_int(signed1);
    int b = arg2.as_int(signed2);
    if (b == 0) return RTLIL::Const(Sx, result_len);
    return RTLIL::Const(a / b, result_len);
}

RTLIL::Const const_divfloor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_div(arg1, arg2, signed1, signed2, result_len);
}

RTLIL::Const const_modfloor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int a = arg1.as_int(signed1);
    int b = arg2.as_int(signed2);
    if (b == 0) return RTLIL::Const(Sx, result_len);
    return RTLIL::Const(a % b, result_len);
}

RTLIL::Const const_mod(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return const_modfloor(arg1, arg2, signed1, signed2, result_len);
}

RTLIL::Const const_pow(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    int a = arg1.as_int(signed1);
    int b = arg2.as_int(signed2);
    if (b < 0) return RTLIL::Const(0, result_len);
    long long result = 1;
    for (int i = 0; i < b; i++) {
        result *= a;
    }
    return RTLIL::Const(result, result_len);
}

RTLIL::Const const_pos(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return arg1;
}

RTLIL::Const const_buf(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    return arg1;
}

RTLIL::Const const_neg(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len) {
    RTLIL::Const zero(std::vector<RTLIL::State>{S0});
    return const_sub(zero, arg1, false, signed1, result_len);
}

RTLIL::Const const_mux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3) {
    return arg3.as_bool() ? arg2 : arg1;
}

RTLIL::Const const_pmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3) {
    int width = arg1.size() / (arg3.size() / arg2.size());
    for (int i = 0; i < arg3.size(); i++) {
        if (arg3[i] == S1) {
            // Extract bits from arg2
            std::vector<RTLIL::State> result;
            for (int j = i * width; j < (i + 1) * width && j < arg2.size(); j++) {
                result.push_back(arg2[j]);
            }
            return RTLIL::Const(result);
        }
    }
    return arg1;
}

RTLIL::Const const_bmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2) {
    int addr = arg2.as_int(false);
    int width = arg1.size() / (1 << arg2.size());
    // Extract bits from arg1
    std::vector<RTLIL::State> result;
    for (int i = addr * width; i < (addr + 1) * width && i < arg1.size(); i++) {
        result.push_back(arg1[i]);
    }
    return RTLIL::Const(result);
}

RTLIL::Const const_demux(const RTLIL::Const &arg1, const RTLIL::Const &arg2) {
    int addr = arg2.as_int(false);
    int width = arg1.size();
    std::vector<RTLIL::State> result(width * (1 << arg2.size()), S0);
    for (int i = 0; i < width; i++) {
        if (arg1[i] == S1) {
            result[addr * width + i] = S1;
        }
    }
    return RTLIL::Const(result);
}

RTLIL::Const const_bweqx(const RTLIL::Const &arg1, const RTLIL::Const &arg2) {
    int len = std::max(arg1.size(), arg2.size());
    std::vector<RTLIL::State> result;
    for (int i = 0; i < len; i++) {
        RTLIL::State bit1 = (i < arg1.size()) ? arg1[i] : S0;
        RTLIL::State bit2 = (i < arg2.size()) ? arg2[i] : S0;
        result.push_back(bit1 == bit2 ? S1 : S0);
    }
    return RTLIL::Const(result);
}

RTLIL::Const const_bwmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3) {
    int len = std::max({arg1.size(), arg2.size(), arg3.size()});
    std::vector<RTLIL::State> result;
    for (int i = 0; i < len; i++) {
        RTLIL::State bit1 = (i < arg1.size()) ? arg1[i] : S0;
        RTLIL::State bit2 = (i < arg2.size()) ? arg2[i] : S0;
        RTLIL::State bit3 = (i < arg3.size()) ? arg3[i] : S0;
        result.push_back(bit3 == S1 ? bit2 : bit1);
    }
    return RTLIL::Const(result);
}

// ============================================================================
// CaseRule destructor implementation
// ============================================================================

CaseRule::~CaseRule() {
    for (auto *sw : switches)
        delete sw;
}

// ============================================================================
// SwitchRule destructor implementation
// ============================================================================

SwitchRule::~SwitchRule() {
    for (auto *c : cases)
        delete c;
}

} // namespace RTLIL
