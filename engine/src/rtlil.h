/**
 * RTLIL - Register Transfer Level Intermediate Language
 * Industrial-grade implementation based on industry-standard RTLIL
 *
 * This is a complete rewrite with full feature support:
 * - IdString with global string cache and reference counting
 * - SigSpec/SigBit/SigChunk for signal representation
 * - Const with bitvector and string backing
 * - Wire, Cell, Memory, Process, Module, Design
 * - AttrObject for attribute metadata
 * - ObjIterator for safe container iteration
 * - Full operator overloading and comparison
 */

#ifndef RTLIL_INDUSTRIAL_H
#define RTLIL_INDUSTRIAL_H

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <optional>
#include <atomic>
#include <functional>
#include <cassert>

namespace RTLIL {

// ============================================================================
// Forward declarations
// ============================================================================

struct Const;
struct AttrObject;
struct NamedObject;
struct Selection;
struct Design;
struct Module;
struct Wire;
struct Memory;
struct Cell;
struct SigChunk;
struct SigBit;
struct SigSpec;
struct CaseRule;
struct SwitchRule;
struct SyncRule;
struct Process;
struct IdString;
struct OwningIdString;

typedef std::pair<SigSpec, SigSpec> SigSig;

// ============================================================================
// State enumeration (from Yosys RTLIL::State)
// ============================================================================

enum State : unsigned char {
    S0 = 0,  // logic 0
    S1 = 1,  // logic 1
    Sx = 2,  // undefined value or conflict
    Sz = 3,  // high-impedance / not-connected
    Sa = 4,  // don't care (used only in cases)
    Sm = 5   // marker (used internally by some passes)
};

// ============================================================================
// SyncType enumeration (from Yosys RTLIL::SyncType)
// ============================================================================

enum SyncType : unsigned char {
    ST0 = 0,  // level sensitive: 0
    ST1 = 1,  // level sensitive: 1
    STp = 2,  // edge sensitive: posedge
    STn = 3,  // edge sensitive: negedge
    STe = 4,  // edge sensitive: both edges
    STa = 5,  // always active
    STg = 6,  // global clock
    STi = 7   // init
};

// ============================================================================
// ConstFlags (from Yosys RTLIL::ConstFlags)
// ============================================================================

enum ConstFlags : unsigned char {
    CONST_FLAG_NONE    = 0,
    CONST_FLAG_STRING  = 1,
    CONST_FLAG_SIGNED  = 2,  // only used for parameters
    CONST_FLAG_REAL    = 4,  // only used for parameters
    CONST_FLAG_UNSIZED = 8,  // only used for parameters
};

// ============================================================================
// PortDir (from Yosys RTLIL::PortDir)
// ============================================================================

enum PortDir : unsigned char {
    PD_UNKNOWN = 0,
    PD_INPUT = 1,
    PD_OUTPUT = 2,
    PD_INOUT = 3
};

// ============================================================================
// IdString - Global string cache with reference counting
// Based on industry-standard RTLIL::IdString
// ============================================================================

struct IdString {
    struct Storage {
        char *buf;
        int size;

        std::string_view str_view() const { return {buf, static_cast<size_t>(size)}; }
    };

    struct AutoidxStorage {
        const std::string *prefix;
        std::atomic<char *> full_str;

        AutoidxStorage(const std::string *prefix) : prefix(prefix), full_str(nullptr) {}
        AutoidxStorage(AutoidxStorage&& other) : prefix(other.prefix),
            full_str(other.full_str.exchange(nullptr, std::memory_order_relaxed)) {}
        ~AutoidxStorage() { delete[] full_str.load(std::memory_order_acquire); }
    };

    // Global id string cache
    static bool destruct_guard_ok;
    static struct destruct_guard_t {
        destruct_guard_t() { destruct_guard_ok = true; }
        ~destruct_guard_t() { destruct_guard_ok = false; }
    } destruct_guard;

    static std::vector<Storage> global_id_storage_;
    static std::unordered_map<std::string_view, int> global_id_index_;
    static std::unordered_map<int, AutoidxStorage> global_autoidx_id_storage_;
    static std::unordered_map<int, int> global_refcount_storage_;
    static std::vector<int> global_free_idx_list_;
    static int autoidx;

    // The actual IdString object is just a single int
    int index_;

    constexpr inline IdString() : index_(0) { }
    inline IdString(const char *str) : index_(insert(std::string_view(str))) { }
    constexpr IdString(const IdString &str) = default;
    IdString(IdString &&str) = default;
    inline IdString(const std::string &str) : index_(insert(std::string_view(str))) { }
    inline IdString(std::string_view str) : index_(insert(str)) { }

    IdString &operator=(const IdString &rhs) = default;

    inline void operator=(const char *rhs) {
        IdString id(rhs);
        *this = id;
    }

    inline void operator=(const std::string &rhs) {
        IdString id(rhs);
        *this = id;
    }

    inline const char *c_str() const {
        if (index_ >= 0)
            return global_id_storage_.at(index_).buf;

        AutoidxStorage &s = global_autoidx_id_storage_.at(index_);
        char *full_str = s.full_str.load(std::memory_order_acquire);
        if (full_str != nullptr)
            return full_str;
        const std::string &prefix = *s.prefix;
        std::string suffix = std::to_string(-index_);
        char *c = new char[prefix.size() + suffix.size() + 1];
        memcpy(c, prefix.data(), prefix.size());
        memcpy(c + prefix.size(), suffix.c_str(), suffix.size() + 1);
        if (s.full_str.compare_exchange_strong(full_str, c, std::memory_order_acq_rel))
            return c;
        delete[] c;
        return full_str;
    }

    inline std::string str() const {
        std::string result;
        append_to(&result);
        return result;
    }

    inline void append_to(std::string *out) const {
        if (index_ >= 0) {
            *out += global_id_storage_.at(index_).str_view();
            return;
        }
        *out += *global_autoidx_id_storage_.at(index_).prefix;
        *out += std::to_string(-index_);
    }

    std::string unescape() const {
        if (index_ < 0) {
            return str();
        }
        std::string_view str = global_id_storage_.at(index_).str_view();
        if (str.size() < 2 || str[0] != '\\' || str[1] == '$' || str[1] == '\\' || (str[1] >= '0' && str[1] <= '9'))
            return std::string(str);
        return std::string(str.substr(1));
    }

    inline bool lt_by_name(IdString rhs) const {
        return index_ < rhs.index_;
    }

    inline bool operator<(IdString rhs) const {
        return index_ < rhs.index_;
    }

    inline bool operator==(IdString rhs) const { return index_ == rhs.index_; }
    inline bool operator!=(IdString rhs) const { return index_ != rhs.index_; }

    bool operator==(const std::string &rhs) const { return str() == rhs; }
    bool operator!=(const std::string &rhs) const { return str() != rhs; }

    bool operator==(const char *rhs) const { return strcmp(c_str(), rhs) == 0; }
    bool operator!=(const char *rhs) const { return strcmp(c_str(), rhs) != 0; }

    bool begins_with(std::string_view prefix) const {
        std::string_view s = str();
        return s.substr(0, prefix.size()) == prefix;
    }

    bool ends_with(std::string_view suffix) const {
        std::string_view s = str();
        if (s.size() < suffix.size()) return false;
        return s.substr(s.size() - suffix.size()) == suffix;
    }

    bool contains(std::string_view s) const {
        if (index_ >= 0)
            return global_id_storage_.at(index_).str_view().find(s) != std::string::npos;
        return str().find(s) != std::string::npos;
    }

    size_t size() const {
        if (index_ >= 0)
            return global_id_storage_.at(index_).size;
        AutoidxStorage &s = global_autoidx_id_storage_.at(index_);
        return s.prefix->size() + std::to_string(-index_).size();
    }

    bool empty() const { return index_ == 0; }
    void clear() { *this = IdString(); }

    bool isPublic() const { return begins_with("\\"); }

    // Static methods
    static void ensure_prepopulated() {
        if (global_id_index_.empty())
            prepopulate();
    }

    static int insert(std::string_view p) {
        ensure_prepopulated();

        auto it = global_id_index_.find(p);
        if (it != global_id_index_.end()) {
            return it->second;
        }
        return really_insert(p, it);
    }

    static IdString from_index(int index) {
        IdString result;
        result.index_ = index;
        return result;
    }

private:
    static void prepopulate();
    static int really_insert(std::string_view p, std::unordered_map<std::string_view, int>::iterator &it);
};

// ============================================================================
// OwningIdString - Reference-counted IdString
// ============================================================================

struct OwningIdString : public IdString {
    inline OwningIdString() { }
    inline OwningIdString(const OwningIdString &str) : IdString(str) { get_reference(); }
    inline OwningIdString(const char *str) : IdString(str) { get_reference(); }
    inline OwningIdString(const IdString &str) : IdString(str) { get_reference(); }
    inline OwningIdString(IdString &&str) : IdString(str) { get_reference(); }
    inline OwningIdString(const std::string &str) : IdString(str) { get_reference(); }
    inline OwningIdString(std::string_view str) : IdString(str) { get_reference(); }
    inline ~OwningIdString() { put_reference(); }

    inline OwningIdString &operator=(const OwningIdString &rhs) {
        put_reference();
        index_ = rhs.index_;
        get_reference();
        return *this;
    }

    inline OwningIdString &operator=(const IdString &rhs) {
        put_reference();
        index_ = rhs.index_;
        get_reference();
        return *this;
    }

    inline OwningIdString &operator=(OwningIdString &&rhs) {
        std::swap(index_, rhs.index_);
        return *this;
    }

    static IdString immortal(const char* str) {
        IdString result(str);
        get_reference(result.index_);
        return result;
    }

private:
    void get_reference() { get_reference(index_); }
    static void get_reference(int idx) {
        if (idx < 0) return;
        auto it = global_refcount_storage_.find(idx);
        if (it == global_refcount_storage_.end())
            global_refcount_storage_.insert(it, {idx, 1});
        else
            ++it->second;
    }

    void put_reference() {
        if (index_ < 0 || !destruct_guard_ok) return;
        auto it = global_refcount_storage_.find(index_);
        if (it != global_refcount_storage_.end() && it->second >= 1) {
            if (--it->second == 0) {
                global_refcount_storage_.erase(it);
            }
        }
    }
};

// ============================================================================
// Const - Constant value (bitvector or string)
// Based on industry-standard RTLIL::Const
// ============================================================================

struct Const {
    short int flags;

private:
    using bitvectype = std::vector<RTLIL::State>;
    enum class backing_tag : bool { bits, string };
    backing_tag tag;
    union {
        bitvectype bits_;
        std::string str_;
    };

    bool is_bits() const { return tag == backing_tag::bits; }
    bool is_str() const { return tag == backing_tag::string; }

    bitvectype& get_bits();
    std::string& get_str();
    const bitvectype& get_bits() const;
    const std::string& get_str() const;

public:
    Const() : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits), bits_(std::vector<RTLIL::State>()) {}
    Const(std::string str);
    Const(long long val);
    Const(long long val, int width);
    Const(RTLIL::State bit, int width = 1);
    Const(std::vector<RTLIL::State> bits) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits), bits_(std::move(bits)) {}
    Const(const std::vector<bool> &bits);
    Const(const RTLIL::Const &other);
    Const(RTLIL::Const &&other);
    RTLIL::Const &operator=(const RTLIL::Const &other);
    ~Const();

    bool operator<(const RTLIL::Const &other) const;
    bool operator==(const RTLIL::Const &other) const;
    bool operator!=(const RTLIL::Const &other) const;

    bool as_bool() const;
    int as_int(bool is_signed = false) const;
    bool convertible_to_int(bool is_signed = false) const;
    std::optional<int> try_as_int(bool is_signed = false) const;
    int as_int_saturating(bool is_signed = false) const;

    std::string as_string(const char* any = "-") const;
    static Const from_string(const std::string &str);
    std::vector<RTLIL::State> to_bits() const;

    std::string decode_string() const;
    int size() const;
    bool empty() const;

    void append(const RTLIL::Const &other);
    void set(int i, RTLIL::State state);
    void resize(int size, RTLIL::State fill);

    RTLIL::State operator[](int i) const;

    // Bitwise operations
    friend RTLIL::Const const_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Reduce operations
    friend RTLIL::Const const_reduce_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_reduce_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_reduce_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_reduce_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_reduce_bool(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Logic operations
    friend RTLIL::Const const_logic_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_logic_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_logic_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Shift operations
    friend RTLIL::Const const_shl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_shr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_sshl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_sshr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Arithmetic operations
    friend RTLIL::Const const_add(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_sub(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_mul(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_div(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_mod(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_pow(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Comparison operations
    friend RTLIL::Const const_lt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_le(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_eq(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_ne(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_ge(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_gt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

    // Mux operations
    friend RTLIL::Const const_mux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
    friend RTLIL::Const const_pmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
    friend RTLIL::Const const_bmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2);
    friend RTLIL::Const const_demux(const RTLIL::Const &arg1, const RTLIL::Const &arg2);

    // Unary operations
    friend RTLIL::Const const_pos(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_buf(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
    friend RTLIL::Const const_neg(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
};

// ============================================================================
// SigBit - Single signal bit
// ============================================================================

struct SigBit {
    union {
        struct {
            int wire_idx;
            int offset;
        };
        RTLIL::State data;
    };

    SigBit() : data(Sx) {}
    SigBit(RTLIL::State s) : data(s) {}
    SigBit(int wire, int off = 0) : wire_idx(wire), offset(off) {}

    bool operator==(const SigBit &o) const;
    bool operator!=(const SigBit &o) const { return !(*this == o); }
    bool operator<(const SigBit &o) const;

    bool is_wire() const { return wire_idx >= 0; }
    bool is_constant() const { return wire_idx < 0; }
};

// ============================================================================
// SigChunk - A contiguous range of bits from a wire
// ============================================================================

struct SigChunk {
    int wire_idx;
    int offset;
    int width;

    SigChunk() : wire_idx(-1), offset(0), width(0) {}
    SigChunk(int wire, int off, int w) : wire_idx(wire), offset(off), width(w) {}

    bool operator==(const SigChunk &o) const;
    bool operator!=(const SigChunk &o) const { return !(*this == o); }
    bool operator<(const SigChunk &o) const;
};

// ============================================================================
// SigSpec - Collection of signal bits
// ============================================================================

struct SigSpec {
    std::vector<SigBit> bits_;

    SigSpec() = default;
    SigSpec(const SigBit &bit) { bits_.push_back(bit); }
    SigSpec(const SigChunk &chunk);
    SigSpec(int wire_idx, int width);
    SigSpec(const RTLIL::Const &val);

    int width() const { return (int)bits_.size(); }
    bool empty() const { return bits_.empty(); }

    void append(const SigSpec &other);
    void append(const SigBit &bit);
    void append(const RTLIL::Const &val);

    void prepend(const SigSpec &other);

    SigSpec extract(int offset, int length) const;
    SigSpec extract(int offset) const { return extract(offset, 1); }

    void replace(int offset, const SigSpec &other);
    void replace(const SigSpec &old, const SigSpec &rep);

    void remove(int offset, int length = 1);

    void reverse();

    bool operator==(const SigSpec &o) const { return bits_ == o.bits_; }
    bool operator!=(const SigSpec &o) const { return !(*this == o); }
    bool operator<(const SigSpec &o) const;

    SigBit operator[](int i) const { return bits_.at(i); }

    RTLIL::Const as_const() const;

    // Iterator support
    auto begin() { return bits_.begin(); }
    auto end() { return bits_.end(); }
    auto begin() const { return bits_.begin(); }
    auto end() const { return bits_.end(); }
};

typedef std::pair<SigSpec, SigSpec> SigSig;

// ============================================================================
// AttrObject - Object with attributes
// ============================================================================

struct AttrObject {
    std::map<IdString, Const> attributes;

    bool has_attribute(IdString name) const {
        return attributes.find(name) != attributes.end();
    }

    Const &get_attribute(IdString name) {
        return attributes.at(name);
    }

    const Const &get_attribute(IdString name) const {
        return attributes.at(name);
    }

    void set_attribute(IdString name, const Const &value) {
        attributes[name] = value;
    }

    void unset_attribute(IdString name) {
        attributes.erase(name);
    }

    void copy_attributes(const AttrObject *other) {
        for (auto &it : other->attributes)
            attributes[it.first] = it.second;
    }
};

// ============================================================================
// NamedObject - Object with a name
// ============================================================================

struct NamedObject : public AttrObject {
    IdString name;

    virtual ~NamedObject() = default;

    virtual std::string describe() const {
        return name.str();
    }
};

// ============================================================================
// Wire - Port or internal signal
// Based on industry-standard RTLIL::Wire
// ============================================================================

struct Wire : public NamedObject {
    int width_;
    int start_offset_;
    int port_id_;
    PortDir port_input_;
    PortDir port_output_;
    bool is_signed_;
    bool is_unsized_;
    bool is_memory_;
    bool is_local_;
    bool is_global_;
    bool is_auto;

    Wire() : width_(1), start_offset_(0), port_id_(0),
             port_input_(PD_UNKNOWN), port_output_(PD_UNKNOWN),
             is_signed_(false), is_unsized_(false), is_memory_(false),
             is_local_(false), is_global_(false), is_auto(false) {}

    Wire(const IdString &name) : width_(1), start_offset_(0), port_id_(0),
         port_input_(PD_UNKNOWN), port_output_(PD_UNKNOWN),
         is_signed_(false), is_unsized_(false), is_memory_(false),
         is_local_(false), is_global_(false), is_auto(false) {
        this->name = name;
    }

    Wire(const IdString &name, int width) : width_(width), start_offset_(0), port_id_(0),
         port_input_(PD_UNKNOWN), port_output_(PD_UNKNOWN),
         is_signed_(false), is_unsized_(false), is_memory_(false),
         is_local_(false), is_global_(false), is_auto(false) {
        this->name = name;
    }

    Wire(const IdString &name, int width, int port_id) : width_(width), start_offset_(0), port_id_(port_id),
         port_input_(PD_UNKNOWN), port_output_(PD_UNKNOWN),
         is_signed_(false), is_unsized_(false), is_memory_(false),
         is_local_(false), is_global_(false), is_auto(false) {
        this->name = name;
    }

    Wire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output)
        : width_(width), start_offset_(0), port_id_(port_id),
          port_input_(port_input), port_output_(port_output),
          is_signed_(false), is_unsized_(false), is_memory_(false),
          is_local_(false), is_global_(false), is_auto(false) {
        this->name = name;
    }

    Wire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output, int start_offset)
        : width_(width), start_offset_(start_offset), port_id_(port_id),
          port_input_(port_input), port_output_(port_output),
          is_signed_(false), is_unsized_(false), is_memory_(false),
          is_local_(false), is_global_(false), is_auto(false) {
        this->name = name;
    }

    int width() const { return width_; }
    int start_offset() const { return start_offset_; }
    int port_id() const { return port_id_; }
    PortDir port_input() const { return port_input_; }
    PortDir port_output() const { return port_output_; }
    bool is_signed() const { return is_signed_; }
    bool is_unsized() const { return is_unsized_; }
    bool is_memory() const { return is_memory_; }
    bool is_local() const { return is_local_; }
    bool is_global() const { return is_global_; }
};

// ============================================================================
// Memory - Memory array
// ============================================================================

struct Memory : public NamedObject {
    int width_;
    int size_;
    int start_offset_;

    Memory() : width_(0), size_(0), start_offset_(0) {}
    Memory(const IdString &name, int width, int size, int start_offset = 0)
        : width_(width), size_(size), start_offset_(start_offset) {
        this->name = name;
    }

    int width() const { return width_; }
    int size() const { return size_; }
    int start_offset() const { return start_offset_; }
};

// ============================================================================
// Cell - Module instance or primitive
// Based on industry-standard RTLIL::Cell
// ============================================================================

struct Cell : public NamedObject {
    IdString type;
    std::map<IdString, SigSpec> connections_;
    std::map<IdString, Const> parameters_;
    std::map<IdString, int> port_connections_;

    bool is_difficult;
    bool is_external;
    bool blackbox;

    Cell() : is_difficult(false), is_external(false), blackbox(false) {}
    Cell(const IdString &name, const IdString &type) : type(type),
        is_difficult(false), is_external(false), blackbox(false) {
        this->name = name;
    }

    const IdString &type_name() const { return type; }

    bool hasPort(IdString port_name) const {
        return connections_.find(port_name) != connections_.end();
    }

    const SigSpec &getPort(IdString port_name) const {
        return connections_.at(port_name);
    }

    SigSpec &getPort(IdString port_name) {
        return connections_.at(port_name);
    }

    void setPort(IdString port_name, const SigSpec &sig) {
        connections_[port_name] = sig;
    }

    void unsetPort(IdString port_name) {
        connections_.erase(port_name);
    }

    bool hasParam(IdString param_name) const {
        return parameters_.find(param_name) != parameters_.end();
    }

    const Const &getParam(IdString param_name) const {
        return parameters_.at(param_name);
    }

    Const &getParam(IdString param_name) {
        return parameters_.at(param_name);
    }

    void setParam(IdString param_name, const Const &value) {
        parameters_[param_name] = value;
    }

    void unsetParam(IdString param_name) {
        parameters_.erase(param_name);
    }

    // Port iteration
    auto begin_port_connections() { return connections_.begin(); }
    auto end_port_connections() { return connections_.end(); }
    auto begin_port_connections() const { return connections_.begin(); }
    auto end_port_connections() const { return connections_.end(); }

    size_t port_connections() const { return connections_.size(); }
    size_t parameters() const { return parameters_.size(); }
};

// ============================================================================
// CaseRule - Case statement
// ============================================================================

struct CaseRule : public AttrObject {
    SigSpec signal;
    std::vector<IdString> compare;
    std::vector<SwitchRule*> switches;
    std::vector<SigSig> actions;

    ~CaseRule();
};

// ============================================================================
// SwitchRule - Switch statement
// ============================================================================

struct SwitchRule : public AttrObject {
    SigSpec signal;
    std::vector<CaseRule*> cases;

    ~SwitchRule();
};

// ============================================================================
// SyncRule - Synchronization rule
// ============================================================================

struct SyncRule : public AttrObject {
    SyncType type;
    SigSpec signal;
    SigSpec actions_sig;
    std::vector<CaseRule*> actions;

    ~SyncRule() {
        for (auto *a : actions)
            delete a;
    }
};

// ============================================================================
// Process - Always block
// ============================================================================

struct Process : public NamedObject {
    enum ProcessType {
        ALWAYS,
        INITIAL,
        FINAL
    };

    ProcessType type;
    std::vector<SwitchRule*> switches;

    Process() : type(ALWAYS) {}
    Process(const IdString &name, ProcessType type = ALWAYS) : type(type) {
        this->name = name;
    }

    ~Process() {
        for (auto *sw : switches)
            delete sw;
    }
};

// ============================================================================
// ObjIterator - Safe container iterator
// ============================================================================

template<typename T>
struct ObjIterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    typename std::map<IdString, T>::iterator it;
    std::map<IdString, T> *list_p;
    int *refcount_p;

    ObjIterator() : list_p(nullptr), refcount_p(nullptr) {}

    ObjIterator(decltype(list_p) list_p, int *refcount_p) : list_p(list_p), refcount_p(refcount_p) {
        if (list_p->empty()) {
            this->list_p = nullptr;
            this->refcount_p = nullptr;
        } else {
            it = list_p->begin();
            (*refcount_p)++;
        }
    }

    ObjIterator(const ObjIterator &other) {
        it = other.it;
        list_p = other.list_p;
        refcount_p = other.refcount_p;
        if (refcount_p)
            (*refcount_p)++;
    }

    ObjIterator &operator=(const ObjIterator &other) {
        if (refcount_p)
            (*refcount_p)--;
        it = other.it;
        list_p = other.list_p;
        refcount_p = other.refcount_p;
        if (refcount_p)
            (*refcount_p)++;
        return *this;
    }

    ~ObjIterator() {
        if (refcount_p)
            (*refcount_p)--;
    }

    inline T operator*() const {
        assert(list_p != nullptr);
        return it->second;
    }

    inline bool operator!=(const ObjIterator &other) const {
        if (list_p == nullptr || other.list_p == nullptr)
            return list_p != other.list_p;
        return it != other.it;
    }

    inline bool operator==(const ObjIterator &other) const {
        return !(*this != other);
    }

    inline ObjIterator& operator++() {
        assert(list_p != nullptr);
        if (++it == list_p->end()) {
            (*refcount_p)--;
            list_p = nullptr;
            refcount_p = nullptr;
        }
        return *this;
    }
};

// ============================================================================
// ObjRange - Iterator range for containers
// ============================================================================

template<typename T>
struct ObjRange {
    std::map<IdString, T> *list_p;
    int *refcount_p;

    ObjRange(decltype(list_p) list_p, int *refcount_p) : list_p(list_p), refcount_p(refcount_p) {}
    ObjIterator<T> begin() { return ObjIterator<T>(list_p, refcount_p); }
    ObjIterator<T> end() { return ObjIterator<T>(); }

    size_t size() const { return list_p->size(); }

    std::vector<T> to_vector() const {
        std::vector<T> result;
        result.reserve(list_p->size());
        for (auto &it : *list_p)
            result.push_back(it.second);
        return result;
    }
};

// ============================================================================
// Module - Hardware module
// Based on industry-standard RTLIL::Module
// ============================================================================

struct Module : public NamedObject {
    // Design parent
    Design *design_;

    // Contents
    std::map<IdString, Wire*> wires_;
    std::map<IdString, Cell*> cells_;
    std::map<IdString, Memory*> memories_;
    std::map<IdString, Process*> processes_;

    // Reference counts for iterators
    int refcount_wires_;
    int refcount_cells_;
    int refcount_memories_;
    int refcount_processes_;

    // Selection state
    bool selected_;

    Module() : design_(nullptr), refcount_wires_(0), refcount_cells_(0),
               refcount_memories_(0), refcount_processes_(0), selected_(true) {}
    Module(const IdString &name) : design_(nullptr), refcount_wires_(0), refcount_cells_(0),
                                    refcount_memories_(0), refcount_processes_(0), selected_(true) {
        this->name = name;
    }

    ~Module();

    // Wire operations
    Wire *addWire(const IdString &name);
    Wire *addWire(const IdString &name, int width);
    Wire *addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output);
    Wire *addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output, int start_offset);
    Wire *addWire(const IdString &name, int width, int port_id, PortDir port_input, PortDir port_output, int start_offset, bool is_signed);

    Wire *findWire(IdString name) const;
    bool hasWire(IdString name) const { return wires_.find(name) != wires_.end(); }
    void remove(Wire *wire);

    ObjRange<Wire*> wires() { return ObjRange<Wire*>(&wires_, &refcount_wires_); }
    size_t wire_count() const { return wires_.size(); }

    // Compatibility with old API (snake_case) - defined in .cpp
    Wire *find_wire(const std::string &name) const;
    Wire *find_wire(const char *name) const;
    bool has_wire(const std::string &name) const;
    int wire_index(const std::string &name) const;

    // Cell operations
    Cell *addCell(const IdString &name, const IdString &type);
    Cell *addCell(const IdString &name, const IdString &type, const std::map<IdString, SigSpec> &connections);
    Cell *addCell(const IdString &name, const IdString &type, const std::map<IdString, SigSpec> &connections, const std::map<IdString, Const> &parameters);

    Cell *findCell(IdString name) const;
    bool hasCell(IdString name) const { return cells_.find(name) != cells_.end(); }
    void remove(Cell *cell);

    ObjRange<Cell*> cells() { return ObjRange<Cell*>(&cells_, &refcount_cells_); }
    size_t cell_count() const { return cells_.size(); }

    // Compatibility with old API (snake_case) - defined in .cpp
    Cell *find_cell(const std::string &name) const;
    Cell *find_cell(const char *name) const;
    bool has_cell(const std::string &name) const;

    // Memory operations
    Memory *addMemory(const IdString &name, int width, int size, int start_offset = 0);
    Memory *findMemory(IdString name) const;
    bool hasMemory(IdString name) const { return memories_.find(name) != memories_.end(); }
    void remove(Memory *memory);

    ObjRange<Memory*> memories() { return ObjRange<Memory*>(&memories_, &refcount_memories_); }
    size_t memory_count() const { return memories_.size(); }

    // Process operations
    Process *addProcess(const IdString &name, Process::ProcessType type = Process::ALWAYS);
    Process *addProcess(const IdString &name, SwitchRule *sw);
    Process *findProcess(IdString name) const;
    bool hasProcess(IdString name) const { return processes_.find(name) != processes_.end(); }
    void remove(Process *process);

    ObjRange<Process*> processes() { return ObjRange<Process*>(&processes_, &refcount_processes_); }
    size_t process_count() const { return processes_.size(); }

    // Selection
    void select(Design *design);
    void select(Design *design, Module *mod);
    void select(Design *design, Wire *wire);
    void select(Design *design, Cell *cell);
    void select(Design *design, Memory *memory);
    void select(Design *design, Process *process);
    void select(Design *design, SigSpec sig);

    void optimize();

    // Statistics
    int numPorts() const;
    int numWires() const { return wires_.size(); }
    int numCells() const { return cells_.size(); }
    int numProcesses() const { return processes_.size(); }

    // Connection operations
    void connect(const SigSpec &left, const SigSpec &right);
    void connect(const SigSig &conn);
    void fixup_ports();
};

// ============================================================================
// Design - Top-level container
// Based on industry-standard RTLIL::Design
// ============================================================================

struct Design : public AttrObject {
    // Modules
    std::map<IdString, Module*> modules_;
    int refcount_modules_;

    // Selection state
    bool selected_;

    Design() : refcount_modules_(0), selected_(true) {}
    ~Design();

    // Module operations
    Module *addModule(const IdString &name);
    Module *addModule(Module *existing);
    Module *findModule(IdString name) const;
    bool hasModule(IdString name) const { return modules_.find(name) != modules_.end(); }
    void remove(Module *module);

    // Compatibility with old API (snake_case) - defined in .cpp
    Module *add_module(const std::string &name);
    Module *find_module(const std::string &name);
    Module *find_module(const std::string &name) const;
    bool has_module(const std::string &name) const;

    // Additional compatibility functions
    Module *find_module(const char *name);
    Module *find_module(const char *name) const;

    ObjRange<Module*> modules() { return ObjRange<Module*>(&modules_, &refcount_modules_); }
    size_t module_count() const { return modules_.size(); }

    // Selection
    void select(Module *mod);
    void select(Wire *wire);
    void select(Cell *cell);
    void select(Memory *memory);
    void select(Process *process);
    void select(SigSpec sig);

    void select_all();
    void deselect_all();

    // Optimization
    void optimize();

    // Statistics
    int numModules() const { return modules_.size(); }

    // Save/Load
    bool save(std::string filename);
    bool load(std::string filename);

    // Merge
    void merge_designs(Design *other);
};

// ============================================================================
// Helper functions
// ============================================================================

// Escape/unescape identifiers
inline std::string escape_id(const std::string &str) {
    if (str.size() > 0 && str[0] != '\\' && str[0] != '$')
        return "\\" + str;
    return str;
}

inline std::string unescape_id(const std::string &str) {
    if (str.size() < 2) return str;
    if (str[0] != '\\') return str;
    if (str[1] == '$' || str[1] == '\\') return str;
    if (str[1] >= '0' && str[1] <= '9') return str;
    return str.substr(1);
}

inline std::string unescape_id(IdString str) {
    return str.unescape();
}

// Format ID for output
inline const char *id2cstr(IdString str) {
    return str.c_str();
}

// Sort helpers
template <typename T>
struct sort_by_name_id {
    bool operator()(T *a, T *b) const {
        return a->name < b->name;
    }
};

template <typename T>
struct sort_by_name_str {
    bool operator()(T *a, T *b) const {
        return a->name.lt_by_name(b->name);
    }
};

// Constant operations (declared as friends in Const)
RTLIL::Const const_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_reduce_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_reduce_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_reduce_xor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_reduce_xnor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_reduce_bool(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_logic_not(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_logic_and(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_logic_or(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_shl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_shr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_sshl(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_sshr(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_shift(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool left, bool signed1, bool signed2, int result_len);
RTLIL::Const const_shiftx(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool left, int result_len);

RTLIL::Const const_lt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_le(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_eq(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_ne(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_eqx(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_nex(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_ge(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_gt(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_add(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_sub(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_mul(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_div(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_divfloor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_modfloor(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_mod(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_pow(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_pos(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_buf(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
RTLIL::Const const_neg(const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

RTLIL::Const const_mux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
RTLIL::Const const_pmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
RTLIL::Const const_bmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2);
RTLIL::Const const_demux(const RTLIL::Const &arg1, const RTLIL::Const &arg2);

RTLIL::Const const_bweqx(const RTLIL::Const &arg1, const RTLIL::Const &arg2);
RTLIL::Const const_bwmux(const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);

} // namespace RTLIL

#endif // RTLIL_INDUSTRIAL_H
