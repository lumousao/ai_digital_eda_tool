/**
 * Optimization Passes - local RTL optimization implementation
 *
 * Features:
 * - Constant propagation
 * - Dead code elimination
 * - Logic sharing
 * - Boolean optimization
 * - MUX optimization
 * - FSM extraction and optimization
 * - Resource sharing
 * - Technology mapping
 */

#ifndef OPTIMIZATION_INDUSTRIAL_H
#define OPTIMIZATION_INDUSTRIAL_H

#include "rtlil.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <iostream>
#include <ostream>

namespace Optimization {

// ============================================================================
// Forward declarations
// ============================================================================

struct Pass;
struct PassManager;

// ============================================================================
// Pass - Base class for optimization passes
// ============================================================================

struct Pass {
    std::string name;
    std::string short_help;
    std::string long_help;

    Pass() = default;
    Pass(const std::string &name, const std::string &short_help)
        : name(name), short_help(short_help) {}
    virtual ~Pass() = default;

    virtual void execute(RTLIL::Design *design, const std::vector<std::string> &args) = 0;
    virtual void help() {
        std::cout << short_help << std::endl;
    }
};

// ============================================================================
// PassManager - Manages and executes optimization passes
// ============================================================================

struct PassManager {
    std::vector<Pass*> passes;
    std::map<std::string, Pass*> pass_map;

    PassManager() = default;
    ~PassManager();

    void registerPass(Pass *pass);
    Pass *findPass(const std::string &name) const;
    void executePass(RTLIL::Design *design, const std::string &pass_name,
                     const std::vector<std::string> &args = {});
    void executePasses(RTLIL::Design *design, const std::vector<std::string> &pass_names);
    void help() const;
};

// ============================================================================
// OptPass - Optimization pass pipeline
// ============================================================================

struct OptPass : public Pass {
    OptPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;
};

// ============================================================================
// OptExprPass - Expression optimization
// ============================================================================

struct OptExprPass : public Pass {
    OptExprPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void optimizeExpr(RTLIL::Module *mod);
    void propagateConstants(RTLIL::Module *mod);
    void eliminateDeadCode(RTLIL::Module *mod);
    void shareLogic(RTLIL::Module *mod);
};

// ============================================================================
// ConstPropPass - Constant propagation
// ============================================================================

struct ConstPropPass : public Pass {
    ConstPropPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void propagateConstants(RTLIL::Module *mod);
    void foldConstants(RTLIL::Module *mod);
    void propagateWire(RTLIL::Module *mod, RTLIL::Wire *wire);
};

// ============================================================================
// DeadCodeElimPass - Dead code elimination
// ============================================================================

struct DeadCodeElimPass : public Pass {
    DeadCodeElimPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void eliminateDeadWires(RTLIL::Module *mod);
    void eliminateDeadCells(RTLIL::Module *mod);
    void eliminateDeadProcesses(RTLIL::Module *mod);
    bool isWireUsed(RTLIL::Module *mod, RTLIL::Wire *wire);
    bool isCellUsed(RTLIL::Module *mod, RTLIL::Cell *cell);
};

// ============================================================================
// OptMuxPass - MUX optimization
// ============================================================================

struct OptMuxPass : public Pass {
    OptMuxPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void optimizeMux(RTLIL::Module *mod);
    void mergeMux(RTLIL::Module *mod);
    void reduceMux(RTLIL::Module *mod);
};

// ============================================================================
// ShareLogicPass - Logic sharing
// ============================================================================

struct ShareLogicPass : public Pass {
    ShareLogicPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void findShareableLogic(RTLIL::Module *mod);
    void shareCommonSubexpressions(RTLIL::Module *mod);
    void mergeIdenticalCells(RTLIL::Module *mod);
};

// ============================================================================
// OptReducePass - Logic reduction
// ============================================================================

struct OptReducePass : public Pass {
    OptReducePass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void reduceAnd(RTLIL::Module *mod);
    void reduceOr(RTLIL::Module *mod);
    void reduceXor(RTLIL::Module *mod);
    void reduceMux(RTLIL::Module *mod);
};

// ============================================================================
// TechMapPass - Technology mapping
// ============================================================================

struct TechMapPass : public Pass {
    TechMapPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void mapToLibrary(RTLIL::Module *mod);
    void mapArith(RTLIL::Module *mod);
    void mapMemory(RTLIL::Module *mod);
    void mapFF(RTLIL::Module *mod);
    void mapLUT(RTLIL::Module *mod);
};

// ============================================================================
// FsmExtractPass - FSM extraction
// ============================================================================

struct FsmExtractPass : public Pass {
    FsmExtractPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void extractFSM(RTLIL::Module *mod);
    void detectFSM(RTLIL::Module *mod);
    void encodeFSM(RTLIL::Module *mod);
};

// ============================================================================
// FsmOptPass - FSM optimization
// ============================================================================

struct FsmOptPass : public Pass {
    FsmOptPass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void optimizeFSM(RTLIL::Module *mod);
    void minimizeStates(RTLIL::Module *mod);
    void optimizeEncoding(RTLIL::Module *mod);
};

// ============================================================================
// ResourceSharePass - Resource sharing
// ============================================================================

struct ResourceSharePass : public Pass {
    ResourceSharePass();
    void execute(RTLIL::Design *design, const std::vector<std::string> &args) override;

private:
    void shareResources(RTLIL::Module *mod);
    void mergeMultiplexers(RTLIL::Module *mod);
    void shareArithmetic(RTLIL::Module *mod);
};

// ============================================================================
// Helper functions
// ============================================================================

// Register all optimization passes
void registerOptPasses(PassManager &pm);

// Create default optimization pipeline
std::vector<std::string> createOptPipeline();

// Run full optimization
void optimizeDesign(RTLIL::Design *design);

// Run specific optimization level
void optimizeDesign(RTLIL::Design *design, int level);

} // namespace Optimization

#endif // OPTIMIZATION_INDUSTRIAL_H
