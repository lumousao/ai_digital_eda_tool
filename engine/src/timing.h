/**
 * Timing Analysis - Industrial-grade implementation based on OpenSTA
 *
 * Features:
 * - Graph-based timing analysis
 * - Setup/Hold analysis
 * - Clock domain crossing
 * - Multi-corner analysis
 * - OCV (On-Chip Variation)
 * - Clock tree analysis
 * - Path analysis with slack calculation
 * - SDC constraint support
 */

#ifndef TIMING_INDUSTRIAL_H
#define TIMING_INDUSTRIAL_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <optional>
#include <functional>

namespace TimingAnalysis {

// ============================================================================
// Forward declarations
// ============================================================================

struct Graph;
struct Vertex;
struct Edge;
struct TimingArc;
struct Path;
struct Clock;
struct ClockEdge;
struct Tag;
struct Search;
struct Network;
struct Sdc;
struct StaState;

// ============================================================================
// Type definitions
// ============================================================================

typedef int VertexId;
typedef int EdgeId;
typedef int TagIndex;
typedef int PathAPIndex;
typedef int DcalcAPIndex;

const VertexId vertex_id_null = -1;
const TagIndex tag_index_null = -1;

// ============================================================================
// Delay types
// ============================================================================

typedef double Delay;
typedef double Slew;
typedef double Slack;
typedef double Arrival;
typedef double Required;

const Delay delay_zero = 0.0;
const Delay delay_max = 1e30;
const Delay delay_min = -1e30;

// ============================================================================
// Transition (Rise/Fall)
// ============================================================================

class RiseFall {
public:
    enum Index { rise = 0, fall = 1 };

    RiseFall() : index_(rise) {}
    explicit RiseFall(Index idx) : index_(idx) {}

    Index index() const { return index_; }
    bool isRise() const { return index_ == rise; }
    bool isFall() const { return index_ == fall; }

    const RiseFall *toggle() const {
        return index_ == rise ? &fall_ : &rise_;
    }

    static const RiseFall &riseInstance() { return rise_; }
    static const RiseFall &fallInstance() { return fall_; }

private:
    Index index_;
    static const RiseFall rise_;
    static const RiseFall fall_;
};

// ============================================================================
// MinMax
// ============================================================================

class MinMax {
public:
    enum Index { min = 0, max = 1 };

    MinMax() : index_(min) {}
    explicit MinMax(Index idx) : index_(idx) {}

    Index index() const { return index_; }
    bool isMin() const { return index_ == min; }
    bool isMax() const { return index_ == max; }

    const MinMax *opposite() const {
        return index_ == min ? &max_ : &min_;
    }

    Delay initValue() const {
        return index_ == min ? delay_max : delay_min;
    }

    bool compare(Delay a, Delay b) const {
        return index_ == min ? a < b : a > b;
    }

    static const MinMax &minInstance() { return min_; }
    static const MinMax &maxInstance() { return max_; }

private:
    Index index_;
    static const MinMax min_;
    static const MinMax max_;
};

// ============================================================================
// Clock
// ============================================================================

struct Clock : public std::enable_shared_from_this<Clock> {
    std::string name;
    Delay period;
    Delay start_time;
    Delay duty_cycle;

    Clock() : period(0.0), start_time(0.0), duty_cycle(0.5) {}
    Clock(const std::string &name, Delay period)
        : name(name), period(period), start_time(0.0), duty_cycle(0.5) {}

    Delay waveform(bool rising) const {
        return rising ? start_time : start_time + period * duty_cycle;
    }
};

// ============================================================================
// ClockEdge
// ============================================================================

struct ClockEdge {
    std::shared_ptr<Clock> clock;
    bool is_rising;
    int edge_index;

    ClockEdge() : is_rising(true), edge_index(0) {}
    ClockEdge(std::shared_ptr<Clock> clk, bool rising, int idx)
        : clock(clk), is_rising(rising), edge_index(idx) {}

    Delay time() const {
        return clock->waveform(is_rising);
    }
};

// ============================================================================
// ClkInfo - Clock information for a path
// ============================================================================

struct ClkInfo {
    std::shared_ptr<Clock> clock;
    ClockEdge edge;
    bool is_generated;
    bool is_falling_clock;

    ClkInfo() : is_generated(false), is_falling_clock(false) {}
    ClkInfo(std::shared_ptr<Clock> clk, const ClockEdge &e)
        : clock(clk), edge(e), is_generated(false), is_falling_clock(false) {}
};

// ============================================================================
// TimingRole - Role of a timing arc
// ============================================================================

class TimingRole {
public:
    enum Index {
        comb_setup = 0,
        comb_hold,
        reg_setup,
        reg_hold,
        latch_setup,
        latch_hold,
        clk_lat,
        width,
        period,
        removal,
        recovery,
        skew,
        nochange,
        undefined
    };

    TimingRole() : index_(undefined) {}
    explicit TimingRole(Index idx) : index_(idx) {}

    Index index() const { return index_; }

    bool isSetup() const { return index_ == reg_setup || index_ == latch_setup; }
    bool isHold() const { return index_ == reg_hold || index_ == latch_hold; }
    bool isSetupOrHold() const { return isSetup() || isHold(); }
    bool isConstraint() const { return isSetupOrHold() || index_ == removal || index_ == recovery; }

    static const TimingRole &setupRole();
    static const TimingRole &holdRole();
    static const TimingRole &widthRole();
    static const TimingRole &periodRole();

private:
    Index index_;
};

// ============================================================================
// TimingArc
// ============================================================================

struct TimingArc {
    EdgeId edge_id;
    int arc_index;
    TimingRole role;
    Delay delay;
    Slew slew;

    TimingArc() : edge_id(-1), arc_index(0), delay(0.0), slew(0.0) {}
    TimingArc(EdgeId eid, int idx, const TimingRole &r)
        : edge_id(eid), arc_index(idx), role(r), delay(0.0), slew(0.0) {}

    int index() const { return arc_index; }
    const TimingRole &timing_role() const { return role; }
};

// ============================================================================
// Edge - Connection between vertices
// ============================================================================

struct Edge {
    EdgeId id;
    VertexId from_vertex;
    VertexId to_vertex;
    std::vector<TimingArc> arcs;

    Edge() : id(-1), from_vertex(-1), to_vertex(-1) {}
    Edge(EdgeId eid, VertexId from, VertexId to)
        : id(eid), from_vertex(from), to_vertex(to) {}

    int arc_count() const { return (int)arcs.size(); }
    TimingArc *arc(int index) { return &arcs[index]; }
    const TimingArc *arc(int index) const { return &arcs[index]; }
};

// ============================================================================
// Vertex - Node in timing graph
// ============================================================================

struct Vertex {
    VertexId id;
    std::string name;
    bool is_clock;
    bool is_primary_input;
    bool is_primary_output;
    bool is_latch;
    bool is_reg_data;
    bool is_reg_clk;
    bool is_port;

    std::vector<EdgeId> fan_in;
    std::vector<EdgeId> fan_out;

    // Timing data
    std::map<TagIndex, Path> paths;

    Vertex() : id(-1), is_clock(false), is_primary_input(false),
               is_primary_output(false), is_latch(false),
               is_reg_data(false), is_reg_clk(false), is_port(false) {}

    Vertex(VertexId vid, const std::string &n)
        : id(vid), name(n), is_clock(false), is_primary_input(false),
          is_primary_output(false), is_latch(false),
          is_reg_data(false), is_reg_clk(false), is_port(false) {}

    int fan_in_count() const { return (int)fan_in.size(); }
    int fan_out_count() const { return (int)fan_out.size(); }

    bool isDriven() const { return !fan_in.empty(); }
    bool drivesLogic() const { return !fan_out.empty(); }
};

// ============================================================================
// Tag - Path tag for timing analysis
// ============================================================================

struct Tag {
    TagIndex index;
    const ClkInfo *clk_info;
    bool is_clock;
    bool is_disabled;
    bool is_false_path;

    Tag() : index(-1), clk_info(nullptr), is_clock(false),
            is_disabled(false), is_false_path(false) {}
    Tag(TagIndex idx, const ClkInfo *clk)
        : index(idx), clk_info(clk), is_clock(false),
          is_disabled(false), is_false_path(false) {}

    TagIndex getIndex() const { return index; }
};

// ============================================================================
// Scene - Analysis point context
// ============================================================================

struct Scene {
    std::string name;
    bool is_default;

    Scene() : is_default(true) {}
    Scene(const std::string &n, bool def = false) : name(n), is_default(def) {}
};

// ============================================================================
// Path - Timing path
// Based on OpenSTA Path.hh
// ============================================================================

struct Path {
    Path *prev_path;
    Arrival arrival;
    Required required;
    VertexId vertex_id;
    TagIndex tag_index;
    EdgeId prev_edge_id;
    int prev_arc_idx;

    Path() : prev_path(nullptr), arrival(0.0), required(0.0),
             vertex_id(vertex_id_null), tag_index(tag_index_null),
             prev_edge_id(-1), prev_arc_idx(0) {}

    Path(const Path *path)
        : prev_path(path ? path->prev_path : nullptr),
          arrival(path ? path->arrival : delay_zero),
          required(path ? path->required : delay_zero),
          vertex_id(path ? path->vertex_id : vertex_id_null),
          tag_index(path ? path->tag_index : tag_index_null),
          prev_edge_id(path ? path->prev_edge_id : -1),
          prev_arc_idx(path ? path->prev_arc_idx : 0) {}

    Path(Vertex *vertex, Tag *tag);

    Path(Vertex *vertex, Tag *tag, const Arrival &arrival,
         Path *prev_path, Edge *prev_edge, TimingArc *prev_arc);

    void init(Vertex *vertex, const Arrival &arrival);
    void init(Vertex *vertex, Tag *tag, const Arrival &arrival,
              Path *prev_path, Edge *prev_edge, TimingArc *prev_arc);
    void init(Vertex *vertex, Tag *tag);

    bool isNull() const { return vertex_id == vertex_id_null; }

    Slack slack() const { return required - arrival; }
    Slew slew() const { return 0.0; }  // Simplified

    Path *prevPath() const { return prev_path; }
    void setPrevPath(Path *path) { prev_path = path; }

    static bool less(const Path *path1, const Path *path2);
    static int cmp(const Path *path1, const Path *path2);
    static bool equal(const Path *path1, const Path *path2);
};

// ============================================================================
// PathLess - Path comparison functor
// ============================================================================

class PathLess {
public:
    PathLess() {}
    bool operator()(const Path *path1, const Path *path2) const {
        return Path::less(path1, path2);
    }
};

// ============================================================================
// VertexPathIterator - Iterator for paths on a vertex
// ============================================================================

class VertexPathIterator {
public:
    VertexPathIterator(Vertex *vertex, const MinMax *min_max, const RiseFall *rf);
    bool hasNext();
    Path *next();

private:
    void findNext();

    Vertex *vertex_;
    const MinMax *min_max_;
    const RiseFall *rf_;
    std::vector<Path*> paths_;
    size_t path_index_;
    Path *next_;
};

// ============================================================================
// Graph - Timing graph
// Based on OpenSTA Graph.hh
// ============================================================================

struct Graph {
    std::vector<Vertex> vertices;
    std::vector<Edge> edges;

    std::unordered_map<std::string, VertexId> vertex_index;
    std::unordered_map<std::string, EdgeId> edge_index;

    Graph() {}

    Vertex *createVertex(const std::string &name);
    Vertex *findVertex(const std::string &name) const;
    Vertex *vertex(VertexId id) { return &vertices[id]; }
    const Vertex *vertex(VertexId id) const { return &vertices[id]; }

    Edge *createEdge(VertexId from, VertexId to);
    Edge *findEdge(VertexId from, VertexId to) const;
    Edge *edge(EdgeId id) { return &edges[id]; }
    const Edge *edge(EdgeId id) const { return &edges[id]; }

    int vertex_count() const { return (int)vertices.size(); }
    int edge_count() const { return (int)edges.size(); }

    void buildFromNetwork(Network *network);
};

// ============================================================================
// Network - Hardware network
// ============================================================================

struct NetworkPin {
    std::string name;
    std::string vertex_name;
    bool is_clock;
    bool is_input;
    bool is_output;

    NetworkPin() : is_clock(false), is_input(false), is_output(false) {}
};

struct NetworkPort {
    std::string name;
    int width;
    bool is_input;
    bool is_output;
    bool is_inout;

    NetworkPort() : width(1), is_input(false), is_output(false), is_inout(false) {}
};

struct NetworkInstance {
    std::string name;
    std::string master_name;
    std::vector<NetworkPin> pins;

    NetworkInstance() {}
};

struct Network {
    std::string top_module_name;
    std::vector<NetworkPort> ports;
    std::vector<NetworkInstance> instances;
    std::vector<NetworkPin> pins;

    Network() {}

    void addPort(const NetworkPort &port) { ports.push_back(port); }
    void addInstance(const NetworkInstance &inst) { instances.push_back(inst); }
    void addPin(const NetworkPin &pin) { pins.push_back(pin); }

    NetworkPort *findPort(const std::string &name);
    NetworkInstance *findInstance(const std::string &name);
    NetworkPin *findPin(const std::string &name);
};

// ============================================================================
// Sdc - Synopsys Design Constraints
// ============================================================================

struct SdcCommand {
    enum CommandType {
        CREATE_CLOCK,
        CREATE_GENERATED_CLOCK,
        SET_INPUT_DELAY,
        SET_OUTPUT_DELAY,
        SET_MAX_DELAY,
        SET_MIN_DELAY,
        SET_MAX_TIME_BOMB,
        SET_MIN_TIME_BOMB,
        SET_MULTICYCLE_PATH,
        SET_FALSE_PATH,
        SET_CLOCK_DOMAIN_CROSSING,
        SET_MAX_CAPACITANCE,
        SET_MAX_FANOUT,
        SET_MAX_TRANSITION,
        GROUP_PATH,
        SET_DISABLE_TIMING,
        SET_TIMING_DERATE
    };

    CommandType type;
    std::string target;
    double value;
    std::string clock_name;
    bool rising;
    bool falling;

    SdcCommand() : type(CREATE_CLOCK), value(0.0), rising(true), falling(true) {}
};

struct Sdc {
    std::vector<SdcCommand> commands;
    std::vector<std::shared_ptr<Clock>> clocks;

    Sdc() {}

    void createClock(const std::string &name, Delay period, Delay start_time = 0.0);
    void createGeneratedClock(const std::string &name, const std::string &source,
                             bool divide_by = true, int divide_value = 1);
    void setInputDelay(Delay delay, const std::string &pin, const std::string &clock);
    void setOutputDelay(Delay delay, const std::string &pin, const std::string &clock);
    void setMaxDelay(Delay delay, const std::string &from, const std::string &to);
    void setMinDelay(Delay delay, const std::string &from, const std::string &to);
    void setMulticyclePath(int cycles, const std::string &from, const std::string &to);
    void setFalsePath(const std::string &from, const std::string &to);
    void setMaxTransition(Delay transition, const std::string &pin);
    void setMaxCapacitance(double cap, const std::string &pin);

    std::shared_ptr<Clock> findClock(const std::string &name) const;
};

// ============================================================================
// Search - Timing search
// ============================================================================

struct Search {
    Graph *graph;
    Sdc *sdc;

    // Timing data
    std::map<VertexId, std::map<TagIndex, Path>> arrival_paths;
    std::map<VertexId, std::map<TagIndex, Path>> required_paths;

    Search() : graph(nullptr), sdc(nullptr) {}
    Search(Graph *g, Sdc *s) : graph(g), sdc(s) {}

    void setup();
    void update();
    void deliverArrivals();
    void deliverRequireds();

    Path * arrivals(VertexId vertex_id, TagIndex tag_index);
    Path * requireds(VertexId vertex_id, TagIndex tag_index);

    void setArrivals(VertexId vertex_id, TagIndex tag_index, const Path &path);
    void setRequireds(VertexId vertex_id, TagIndex tag_index, const Path &path);

    Slack slack(VertexId vertex_id, const MinMax *min_max);
    Slack slack(VertexId vertex_id, const MinMax *min_max, const RiseFall *rf);
    Slack slack(VertexId vertex_id, const MinMax *min_max, const ClkInfo *clk_info);

    Slew slew(VertexId vertex_id, const MinMax *min_max);
    Slew slew(VertexId vertex_id, const MinMax *min_max, const RiseFall *rf);
    Slew slew(VertexId vertex_id, const MinMax *min_max, const ClkInfo *clk_info);

    void findRequireds();

private:
    void setupClocks();
    void findClks();
    void tagClockStarts();
    void bfsArrivals();
    void bfsRequireds();
    void updateVertexArrivals(Vertex *vertex);
    void updateVertexRequireds(Vertex *vertex);
};

// ============================================================================
// StaState - STA state container
// ============================================================================

struct StaState {
    Network *network;
    Graph *graph;
    Search *search;
    Sdc *sdc;

    StaState() : network(nullptr), graph(nullptr), search(nullptr), sdc(nullptr) {}

    void init(Network *net, Graph *g, Search *s, Sdc *sdc) {
        network = net;
        graph = g;
        search = s;
        this->sdc = sdc;
    }
};

// ============================================================================
// TimingAnalyzer - Main timing analysis engine
// ============================================================================

class TimingAnalyzer {
public:
    TimingAnalyzer();
    ~TimingAnalyzer();

    // Setup
    void setNetwork(Network *network);
    void setSdc(Sdc *sdc);

    // Analysis
    void analyzeSetup();
    void analyzeHold();
    void analyzeAll();
    void findWorstPaths(int count);

    // Query
    Slack getSlack(const std::string &pin, bool setup);
    Slew getSlew(const std::string &pin, bool rise);
    Delay getDelay(const std::string &from, const std::string &to);
    Delay getDelay(const std::string &from, const std::string &to, const RiseFall *rf);

    // Path analysis
    std::vector<Path*> getSetupPaths(int count);
    std::vector<Path*> getHoldPaths(int count);
    std::vector<Path*> getWorstPaths(int count);

    // Report
    void reportTiming(const std::string &filename);
    void reportClocks(const std::string &filename);
    void reportSlacks(const std::string &filename);

    // Statistics
    int getVertexCount() const;
    int getEdgeCount() const;
    int getPathCount() const;

private:
    std::unique_ptr<StaState> sta_state_;
    std::unique_ptr<Sdc> sdc_;
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<Network> network_;
    std::unique_ptr<Search> search_;

    // Raw pointers for external objects (not owned)
    Network *network_ptr_;
    Sdc *sdc_ptr_;

    void buildTimingGraph();
    void propagateArrivals();
    void propagateRequireds();
    void calculateSlacks();

    Path *findWorstPath(const MinMax *min_max);
    std::vector<Path*> findAllPaths(const MinMax *min_max, int count);
};

// ============================================================================
// Helper functions
// ============================================================================

// Delay calculations
Delay calculateWireDelay(double resistance, double capacitance);
Delay calculateGateDelay(const std::string &cell_type, double input_cap, double output_cap);
Slew calculateOutputSlew(const std::string &cell_type, double input_slew, double load_cap);

// SDC parsing
Sdc parseSdcFile(const std::string &filename);
void applySdcConstraints(Sdc &sdc, Network *network);

// Timing report generation
void generateTimingReport(const std::string &filename, const std::vector<Path*> &paths);
void generateSlackReport(const std::string &filename, const std::map<std::string, Slack> &slacks);

} // namespace TimingAnalysis

#endif // TIMING_INDUSTRIAL_H
