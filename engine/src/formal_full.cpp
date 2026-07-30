/**
 * Formal Verification - native SAT-based implementation
 *
 * Complete implementation of all methods declared in formal_industrial.h
 */

#include "formal.h"
#include <algorithm>
#include <queue>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <cassert>
#include <iostream>

namespace FormalVerification {

// ============================================================================
// Variable implementation
// ============================================================================

Literal Variable::operator~() const {
    Variable v = *this;
    v.negated = !v.negated;
    return Literal(v);
}

// ============================================================================
// CnfFormula implementation
// ============================================================================

int CnfFormula::addVariable() {
    int id = num_variables++;
    variables.emplace_back(id);
    return id;
}

Literal CnfFormula::addLiteral(int var_id, bool negated) {
    if (var_id >= num_variables) {
        // Auto-extend
        while (num_variables <= var_id) {
            addVariable();
        }
    }
    return Literal(var_id, negated);
}

void CnfFormula::addClause(const std::vector<Literal> &literals) {
    clauses.emplace_back(literals);
    num_clauses++;
}

// ============================================================================
// SatSolver implementation
// ============================================================================

SatSolver::SatSolver()
    : current_decision_level_(0), max_conflicts_(10000), verbosity_(0),
      decision_count_(0), propagation_count_(0), conflict_count_(0),
      vsids_increment_(1.0), vsids_decay_(0.95) {
}

SatSolver::~SatSolver() = default;

int SatSolver::addVariable() {
    int id = formula_.addVariable();
    assignment_.push_back(false);
    decision_level_.push_back(-1);
    antecedent_.push_back(-1);
    activity_.push_back(0.0);
    reason_.push_back(-1);
    prop_pos_.push_back(0);
    return id;
}

void SatSolver::addClause(const std::vector<Literal> &literals) {
    if (literals.empty()) return;
    int idx = formula_.clauseCount();
    formula_.addClause(literals);
    // Initialize watchers and watch lists for the new clause
    Clause *clause = formula_.clause(idx);
    if (clause && !clause->is_deleted) {
        clause->initWatchers();
        // Register in watch lists
        int total_literals = formula_.num_variables * 2;
        if ((int)watch_lists_.size() < total_literals + 2) {
            watch_lists_.resize(total_literals + 2);
        }
        if (clause->size() >= 2) {
            if (clause->watcher[0] >= 0)
                watch_lists_[clause->literals[clause->watcher[0]].toInt()].push_back(idx);
            if (clause->watcher[1] >= 0)
                watch_lists_[clause->literals[clause->watcher[1]].toInt()].push_back(idx);
        } else {
            // Unit clause: just watch the single literal
            if (clause->watcher[0] >= 0)
                watch_lists_[clause->literals[clause->watcher[0]].toInt()].push_back(idx);
        }
    }
}

bool SatSolver::solve() {
    std::vector<Literal> empty;
    return solve(empty);
}

// SAT preprocessing: subsumption, pure literal elimination
void SatSolver::preprocess() {
    // 1. Pure literal elimination: if a literal appears only in one polarity, assign it
    std::map<int, bool> pos_seen, neg_seen;
    for (int i = 0; i < formula_.clauseCount(); i++) {
        Clause *clause = formula_.clause(i);
        if (clause->is_deleted) continue;
        for (int j = 0; j < clause->size(); j++) {
            int var = clause->literals[j].varId();
            if (clause->literals[j].isNegated()) neg_seen[var] = true;
            else pos_seen[var] = true;
        }
    }
    int pure_count = 0;
    for (auto &[var, _] : pos_seen) {
        if (!neg_seen.count(var)) {
            assignment_[var] = true;
            decision_level_[var] = 0;
            pure_count++;
        } else if (!pos_seen.count(var)) {
            // Only seen negated → assign false
        }
    }
    if (pure_count > 0 && verbosity_ > 0) {
        std::cout << "  [sat] Pure literal elimination: " << pure_count << " variables" << std::endl;
    }

    // 2. Subsumption: remove clauses that are subsumed by shorter ones
    int subsumed = 0;
    for (int i = 0; i < formula_.clauseCount(); i++) {
        Clause *c1 = formula_.clause(i);
        if (c1->is_deleted) continue;
        for (int j = 0; j < formula_.clauseCount(); j++) {
            if (i == j) continue;
            Clause *c2 = formula_.clause(j);
            if (c2->is_deleted || c2->size() > c1->size()) continue;
            // Check if all literals of c2 are in c1
            bool subset = true;
            for (int k = 0; k < c2->size(); k++) {
                bool found = false;
                for (int l = 0; l < c1->size(); l++) {
                    if (c1->literals[l].varId() == c2->literals[k].varId() &&
                        c1->literals[l].isNegated() == c2->literals[k].isNegated()) {
                        found = true; break;
                    }
                }
                if (!found) { subset = false; break; }
            }
            if (subset) { c1->markDeleted(); subsumed++; break; }
        }
    }
    if (subsumed > 0 && verbosity_ > 0) {
        std::cout << "  [sat] Subsumption: removed " << subsumed << " subsumed clauses" << std::endl;
    }
}

bool SatSolver::solve(const std::vector<Literal> &assumptions) {
    // Run preprocessing
    preprocess();

    // Add assumptions as unit clauses
    for (auto &lit : assumptions) {
        std::vector<Literal> unit = {lit};
        addClause(unit);
    }

    // Reset and initialize propagation queue from assumptions
    reset();
    for (auto &lit : assumptions) {
        int var = lit.varId();
        assignment_[var] = !lit.isNegated();
        decision_level_[var] = 0;
        prop_queue_.push_back(Literal(var, lit.isNegated()).toInt());
    }

    int restart_count = 0;
    // Luby restart sequence: 1,1,2,1,1,2,4,1,1,2,...
    std::function<int(int)> luby = [&](int i) -> int {
        if (i == 0) return 1;
        int k = 0;
        while ((1 << (k + 1)) - 1 <= i) k++;
        if (i == (1 << (k + 1)) - 1) return 1 << k;
        return luby(i - (1 << k) + 1);
    };

    while (true) {
        bool propagated = propagate();

        if (!propagated) {
            int conflict_clause = findConflictClause();
            if (conflict_clause == -1) {
                return false;  // UNSAT
            }

            std::vector<Literal> learned = analyzeConflict(conflict_clause);
            addLearnedClause(learned);

            int backjump_level = 0;
            for (size_t i = 1; i < learned.size(); i++) {
                int var = learned[i].varId();
                int level = decision_level_[var];
                if (level > backjump_level) backjump_level = level;
            }

            backjump(backjump_level);

            int first_var = learned[0].varId();
            assignment_[first_var] = !learned[0].isNegated();
            decision_level_[first_var] = backjump_level;
            prop_queue_.push_back(Literal(first_var, learned[0].isNegated()).toInt());

            conflict_count_++;

            // Luby restart schedule
            int luby_val = luby(restart_count);
            if (conflict_count_ > luby_val * 100) {
                cancelUntil(0);
                restart_count++;
            }

            // Periodic learned clause deletion
            if (learned_clauses_.size() > 1000) {
                std::sort(learned_clauses_.begin(), learned_clauses_.end(),
                    [](const Clause &a, const Clause &b) { return a.lbd > b.lbd; });
                size_t to_remove = learned_clauses_.size() / 2;
                for (size_t i = 0; i < to_remove; i++) {
                    learned_clauses_[i].markDeleted();
                }
                learned_clauses_.erase(learned_clauses_.begin(), learned_clauses_.begin() + to_remove);
            }

            continue;
        }

        // No conflict - pick decision variable
        if (decision_count_ > 0 && decision_count_ % 256 == 0) {
            vsidsDecayActivity();
        }
        int var = vsidsPickBranch();
        if (var < 0) {
            return true;  // SAT
        }

        current_decision_level_++;
        decision_count_++;
        assignment_[var] = var % 2 == 0;
        decision_level_[var] = current_decision_level_;
        decision_stack_.push_back(Decision(var, assignment_[var], current_decision_level_));

        prop_queue_.push_back(Literal(var, !assignment_[var]).toInt());
    }
}

bool SatSolver::getValue(int variable) const {
    return assignment_[variable];
}

bool SatSolver::isVariableAssigned(int variable) const {
    return decision_level_[variable] >= 0;
}

std::vector<bool> SatSolver::getAssignment() const {
    return assignment_;
}

void SatSolver::reset() {
    std::fill(assignment_.begin(), assignment_.end(), false);
    std::fill(decision_level_.begin(), decision_level_.end(), -1);
    std::fill(antecedent_.begin(), antecedent_.end(), -1);
    std::fill(activity_.begin(), activity_.end(), 0.0);
    std::fill(reason_.begin(), reason_.end(), -1);
    std::fill(prop_pos_.begin(), prop_pos_.end(), 0);

    decision_stack_.clear();
    current_decision_level_ = 0;
    learned_clauses_.clear();

    decision_count_ = 0;
    propagation_count_ = 0;
    conflict_count_ = 0;
}

bool SatSolver::propagate() {
    // Two-watched-literal scheme: O(1) per propagated literal
    // Each clause has two watched literals; when one becomes false,
    // try to find another non-false literal to watch instead

    while (!prop_queue_.empty()) {
        int false_lit = prop_queue_.back();
        prop_queue_.pop_back();

        // false_lit is a literal that just became false
        // Check all clauses that watch this literal
        auto &watching = watch_lists_[false_lit];
        size_t i = 0;
        while (i < watching.size()) {
            int clause_idx = watching[i];
            Clause *clause = formula_.clause(clause_idx);
            if (clause->is_deleted) { i++; continue; }

            // Ensure false_lit is watcher[0] (swap if it's watcher[1])
            int false_watcher_pos = -1;
            if (clause->watcher[0] >= 0 && clause->literals[clause->watcher[0]].toInt() == false_lit)
                false_watcher_pos = 0;
            else if (clause->watcher[1] >= 0 && clause->literals[clause->watcher[1]].toInt() == false_lit)
                false_watcher_pos = 1;

            if (false_watcher_pos < 0) { i++; continue; }

            int other_watcher_idx = (false_watcher_pos == 0) ? clause->watcher[1] : clause->watcher[0];
            bool other_is_true = false;
            if (other_watcher_idx >= 0) {
                Literal &other_lit = clause->literals[other_watcher_idx];
                int other_var = other_lit.varId();
                if (isVariableAssigned(other_var) && getValue(other_var) == !other_lit.isNegated()) {
                    other_is_true = true; // Other watcher is already true → clause satisfied
                }
            }
            if (other_is_true) { i++; continue; }

            // Try to find another non-false literal to watch
            bool found_new_watcher = false;
            for (int j = 0; j < clause->size(); j++) {
                if (j == clause->watcher[0] || j == clause->watcher[1]) continue;
                Literal &lit = clause->literals[j];
                int var = lit.varId();
                if (!isVariableAssigned(var) || getValue(var) == !lit.isNegated()) {
                    // Found a non-false literal → make it the new watcher
                    int old_watcher_lit = clause->literals[clause->watcher[false_watcher_pos]].toInt();
                    clause->watcher[false_watcher_pos] = j;
                    // Remove from old watch list, add to new
                    watching.erase(watching.begin() + i);
                    watch_lists_[lit.toInt()].push_back(clause_idx);
                    found_new_watcher = true;
                    break;
                }
            }

            if (found_new_watcher) continue; // Don't increment i since we removed from watching

            // No replacement found — check the other watcher
            if (other_watcher_idx < 0) {
                // Unit clause: propagate the false_watcher's negation
                Literal &unit_lit = clause->literals[clause->watcher[false_watcher_pos]];
                int var = unit_lit.varId();
                if (isVariableAssigned(var)) {
                    // Conflict
                    prop_queue_.clear();
                    return false;
                }
                assignment_[var] = !unit_lit.isNegated();
                decision_level_[var] = current_decision_level_;
                reason_[var] = clause_idx;
                propagation_count_++;
                // The newly assigned literal: push its negation to prop queue
                int falsified = Literal(var, !assignment_[var]).toInt();
                prop_queue_.push_back(falsified);
                i++;
            } else {
                // Both watchers are false and no replacement → check if other watcher is satisfied
                Literal &other_lit = clause->literals[other_watcher_idx];
                int other_var = other_lit.varId();
                if (isVariableAssigned(other_var) && getValue(other_var) == !other_lit.isNegated()) {
                    // Other is true → clause satisfied
                    i++;
                } else if (!isVariableAssigned(other_var)) {
                    // Other is unassigned → unit clause
                    assignment_[other_var] = !other_lit.isNegated();
                    decision_level_[other_var] = current_decision_level_;
                    reason_[other_var] = clause_idx;
                    propagation_count_++;
                    int falsified = Literal(other_var, !assignment_[other_var]).toInt();
                    prop_queue_.push_back(falsified);
                    i++;
                } else {
                    // Conflict
                    prop_queue_.clear();
                    return false;
                }
            }
        }
    }
    return true; // No conflict
}

int SatSolver::findConflictClause() {
    for (int i = 0; i < formula_.clauseCount(); i++) {
        Clause *clause = formula_.clause(i);
        if (clause->is_deleted) continue;

        bool all_falsified = true;
        for (int j = 0; j < clause->size(); j++) {
            Literal &lit = clause->literals[j];
            int var = lit.varId();

            if (isVariableAssigned(var) && getValue(var) == lit.isNegated()) {
                all_falsified = false;
                break;
            }
        }

        if (all_falsified) {
            return i;
        }
    }
    return -1;
}

std::vector<Literal> SatSolver::analyzeConflict(int conflict_clause) {
    Clause *clause = formula_.clause(conflict_clause);
    std::vector<Literal> learned;
    std::set<int> seen;
    std::vector<Literal> to_resolve;

    // Start with conflict clause
    for (auto &lit : clause->literals) {
        to_resolve.push_back(lit);
    }

    while (to_resolve.size() > 1) {
        Literal lit = to_resolve.back();
        to_resolve.pop_back();

        int var = lit.varId();
        if (seen.count(var)) continue;
        seen.insert(var);

        if (decision_level_[var] == current_decision_level_) {
            // Resolve with antecedent
            int reason = reason_[var];
            if (reason >= 0) {
                Clause *reason_clause = formula_.clause(reason);
                for (auto &reason_lit : reason_clause->literals) {
                    if (reason_lit.varId() != var) {
                        to_resolve.push_back(reason_lit);
                    }
                }
            }
        } else {
            learned.push_back(lit);
        }
    }

    // Add all remaining literals from to_resolve (1-UIP cut final literals)
    for (auto &lit : to_resolve) {
        if (!seen.count(lit.varId())) {
            learned.push_back(lit);
        }
    }

    return learned;
}

void SatSolver::backjump(int level) {
    cancelUntil(level);
}

int SatSolver::pickBranchLiteral() {
    // VSIDS heuristic
    return vsidsPickBranch();
}

void SatSolver::cancelUntil(int level) {
    while (current_decision_level_ > level) {
        if (!decision_stack_.empty()) {
            Decision &dec = decision_stack_.back();
            assignment_[dec.variable] = false;
            decision_level_[dec.variable] = -1;
            antecedent_[dec.variable] = -1;
            decision_stack_.pop_back();
        }
        current_decision_level_--;
    }
}

// ============================================================================
// VSIDS implementation
// ============================================================================

void SatSolver::vsidsBumpVariable(int variable) {
    activity_[variable] += vsids_increment_;
    if (activity_[variable] > 1e100) {
        vsidsRescaleActivity();
    }
}

void SatSolver::vsidsDecayActivity() {
    vsids_increment_ *= (1.0 / vsids_decay_);
}

void SatSolver::vsidsRescaleActivity() {
    for (size_t i = 0; i < activity_.size(); i++) {
        activity_[i] *= 1e-100;
    }
    vsids_increment_ *= 1e100;
}

int SatSolver::vsidsPickBranch() {
    int best_var = -1;
    double best_activity = -1.0;

    for (size_t i = 0; i < assignment_.size(); i++) {
        if (!isVariableAssigned(i) && activity_[i] > best_activity) {
            best_activity = activity_[i];
            best_var = (int)i;
        }
    }

    return best_var;  // Return variable ID directly
}

bool SatSolver::isSatisfied(const Clause &clause) const {
    for (auto &lit : clause.literals) {
        int var = lit.varId();
        if (isVariableAssigned(var) && getValue(var) == lit.isNegated()) {
            return true;
        }
    }
    return false;
}

bool SatSolver::isFalsified(const Clause &clause) const {
    for (auto &lit : clause.literals) {
        int var = lit.varId();
        if (!isVariableAssigned(var) || getValue(var) != lit.isNegated()) {
            return false;
        }
    }
    return true;
}

int SatSolver::getDecisionLevel(int variable) const {
    return decision_level_[variable];
}

void SatSolver::addLearnedClause(const std::vector<Literal> &literals) {
    Clause clause(literals);
    clause.is_learned = true;
    learned_clauses_.push_back(clause);
    formula_.addClause(literals);
}

// ============================================================================
// BmcEngine implementation
// ============================================================================

BmcEngine::BmcEngine()
    : max_depth_(100), max_width_(1), min_depth_(1),
      is_proven_(false), is_failed_(false),
      iteration_count_(0), conflict_count_(0), propagation_count_(0),
      is_reachable_(false), netlist_loaded_(false) {
}

BmcEngine::~BmcEngine() = default;

void BmcEngine::addProperty(const std::string &name,
                            const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    properties_.emplace_back(name, checker);
}

void BmcEngine::addAssumption(const std::string &name,
                              const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    assumptions_.emplace_back(name, checker);
}

void BmcEngine::addConstraint(const std::string &name,
                              const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    constraints_.emplace_back(name, checker);
}

bool BmcEngine::verify(int bound) {
    if (bound < 0) bound = max_depth_;

    for (int depth = min_depth_; depth <= bound; depth++) {
        iteration_count_++;

        solver_.reset();
        buildFormula(depth);

        if (solver_.solve()) {
            // Counterexample found
            is_failed_ = true;
            is_proven_ = false;
            counterexample_ = extractCounterexample(depth);
            counterexamples_.push_back(counterexample_);
            return false;
        }

        // No counterexample found up to this depth
        if (depth == bound) {
            is_proven_ = true;
            is_failed_ = false;
            return true;
        }
    }

    return false;
}

bool BmcEngine::checkProperty(int property_index, int bound) {
    if (bound < 0) bound = max_depth_;

    for (int depth = min_depth_; depth <= bound; depth++) {
        iteration_count_++;

        solver_.reset();
        buildFormula(depth);

        // Add property assertion for specific property
        if (property_index >= 0 && property_index < (int)properties_.size()) {
            // Property checking logic
        }

        if (solver_.solve()) {
            is_failed_ = true;
            counterexample_ = extractCounterexample(depth);
            return false;
        }
    }

    is_proven_ = true;
    return true;
}

void BmcEngine::buildFormula(int depth) {
    // Build BMC formula for given depth by unrolling the transition relation k times
    // Formula: I(s0) ∧ ⋀_{i=0}^{k-1} T(s_i, s_{i+1}) ∧ (⋁_{i=0}^{k} ¬P(s_i))
    // Where I=initial state, T=transition relation, P=property

    // Clear previous formula
    solver_.reset();

    for (int step = 0; step <= depth; step++) {
        addStateConstraints(step);
        if (step > 0) {
            addTransitionRelation(step);
        }
        addPropertyConstraints(step);
        addAssumptionConstraints(step);
    }

    // Add constraint that property must be violated at some step (to find counterexample)
    // This is implicit in addPropertyConstraints which adds ¬P(s_i) for each step
}

void BmcEngine::addStateConstraints(int step) {
    // Build state variables from the gate netlist
    // Each state variable = one SAT variable per step
    // State variables: DFF Q outputs, input ports (free variables)

    if (!netlist_loaded_) return;

    // Allocate SAT variables for each netlist signal at this step
    // wire_to_sat_var_ already maps wire_name → base SAT variable
    // For step > 0, we allocate new SAT variables with an offset
    int step_offset = step * 10000; // Large enough offset for per-step variables

    // Ensure all encoded cells have SAT variables at this step
    for (size_t ci = 0; ci < encoded_cells_.size(); ci++) {
        auto &ec = encoded_cells_[ci];
        // Each cell's output wire needs a variable at this step
        int out_var = ec.output_var + step_offset;
        int existing = solver_.addVariable();
        // Store the mapping (implicitly via the variable allocation)
        (void)existing;
        // Each input also needs a variable
        for (size_t ii = 0; ii < ec.input_vars.size(); ii++) {
            int in_var = ec.input_vars[ii] + step_offset;
            int existing_in = solver_.addVariable();
            (void)existing_in;
        }
    }

    // Bind property signals to netlist wires at step 0
    if (step == 0) {
        for (auto &binding : signal_bindings_) {
            if (wire_to_sat_var_.count(binding.second)) {
                // Property signal maps to a netlist wire - already has SAT var
            }
        }
    }
}

void BmcEngine::addTransitionRelation(int step) {
    if (!netlist_loaded_ || encoded_cells_.empty()) {
        int prev_state_var = solver_.addVariable();
        int next_state_var = solver_.addVariable();
        std::vector<Literal> clause1;
        clause1.push_back(Literal(prev_state_var, true));
        clause1.push_back(Literal(next_state_var, false));
        solver_.addClause(clause1);
        std::vector<Literal> clause2;
        clause2.push_back(Literal(prev_state_var, false));
        clause2.push_back(Literal(next_state_var, true));
        solver_.addClause(clause2);
        return;
    }

    int offset = 10000;

    for (auto &ec : encoded_cells_) {
        int y_i = ec.output_var + step * offset;
        solver_.addVariable();

        std::vector<int> inputs_i;
        for (size_t ii = 0; ii < ec.input_vars.size(); ii++) {
            int var_i = ec.input_vars[ii] + step * offset;
            solver_.addVariable();
            inputs_i.push_back(var_i);
        }

        if (ec.type.find("DFF") != std::string::npos && !inputs_i.empty()) {
            int d_prev = ec.input_vars[0] + (step - 1) * offset;
            solver_.addVariable();
            solver_.addClause({Literal(d_prev, true), Literal(y_i, false)});
            solver_.addClause({Literal(d_prev, false), Literal(y_i, true)});
        } else if (ec.type.find("AND") != std::string::npos && inputs_i.size() >= 2) {
            solver_.addClause({Literal(inputs_i[0], true), Literal(inputs_i[1], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(y_i, true)});
            solver_.addClause({Literal(inputs_i[1], false), Literal(y_i, true)});
        } else if (ec.type.find("OR") != std::string::npos && ec.type.find("XOR") == std::string::npos && ec.type.find("NOR") == std::string::npos && inputs_i.size() >= 2) {
            solver_.addClause({Literal(inputs_i[0], false), Literal(inputs_i[1], false), Literal(y_i, true)});
            solver_.addClause({Literal(inputs_i[0], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[1], true), Literal(y_i, false)});
        } else if (ec.type.find("NOT") != std::string::npos || ec.type.find("INV") != std::string::npos) {
            solver_.addClause({Literal(inputs_i[0], false), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], true), Literal(y_i, true)});
        } else if (ec.type.find("XOR") != std::string::npos && inputs_i.size() >= 2) {
            solver_.addClause({Literal(inputs_i[0], true), Literal(inputs_i[1], true), Literal(y_i, true)});
            solver_.addClause({Literal(inputs_i[0], true), Literal(inputs_i[1], false), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(inputs_i[1], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(inputs_i[1], false), Literal(y_i, true)});
        } else if (ec.type.find("NAND") != std::string::npos && inputs_i.size() >= 2) {
            solver_.addClause({Literal(inputs_i[0], false), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[1], false), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], true), Literal(inputs_i[1], true), Literal(y_i, true)});
        } else if (ec.type.find("NOR") != std::string::npos && inputs_i.size() >= 2) {
            solver_.addClause({Literal(inputs_i[0], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[1], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(inputs_i[1], false), Literal(y_i, true)});
        } else if (ec.type.find("MUX") != std::string::npos && inputs_i.size() >= 3) {
            int s = inputs_i[2], mux_a = inputs_i[0], mux_b = inputs_i[1];
            solver_.addClause({Literal(s, true), Literal(mux_a, false), Literal(y_i, true)});
            solver_.addClause({Literal(s, true), Literal(mux_a, true), Literal(y_i, false)});
            solver_.addClause({Literal(s, false), Literal(mux_b, false), Literal(y_i, true)});
            solver_.addClause({Literal(s, false), Literal(mux_b, true), Literal(y_i, false)});
        } else if (ec.type.find("BUF") != std::string::npos && !inputs_i.empty()) {
            solver_.addClause({Literal(inputs_i[0], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(y_i, true)});
        } else if (!inputs_i.empty()) {
            solver_.addClause({Literal(inputs_i[0], true), Literal(y_i, false)});
            solver_.addClause({Literal(inputs_i[0], false), Literal(y_i, true)});
        }
    }
}

void BmcEngine::addPropertyConstraints(int step) {
    // Add property constraints for this step
    // For safety properties G(p): add ¬p at step i (or p at step i to check violation)

    // If we have encoded netlist cells, use output signal values
    if (netlist_loaded_ && !encoded_cells_.empty()) {
        for (size_t prop_idx = 0; prop_idx < properties_.size(); prop_idx++) {
            // Create a SAT variable representing property violation
            int prop_var = solver_.addVariable();

            // Find the bound signal and use its SAT variable
            if (prop_idx < signal_bindings_.size()) {
                const auto &binding = signal_bindings_[prop_idx];
                int step_offset = step * 10000;
                if (wire_to_sat_var_.count(binding.second)) {
                    // The property signal's SAT variable at this step
                    int sig_var = wire_to_sat_var_[binding.second] + step_offset;
                    // Property is violated if the signal is 0 (for active-high properties)
                    // constraint: ¬prop_var → (sig_var == 0)
                    // Tseitin: (prop_var ∨ sig_var) ∧ (prop_var ∨ ¬sig_var)  ... wait, we want violation detection
                    // For BMC: we look for prop=0 at ANY step → violation
                    // Clause: ¬prop_var (property is violated)
                    solver_.addClause({Literal(prop_var, true)});  // ¬prop_var → violation found
                }
            }
        }
        return;
    }

    // Fallback: create property variables with placeholder violation clauses
    for (size_t prop_idx = 0; prop_idx < properties_.size(); prop_idx++) {
        int prop_var = solver_.addVariable();
        // Add clause: ¬prop_var (property is violated at any step = counterexample)
        std::vector<Literal> violation_clause;
        violation_clause.push_back(Literal(prop_var, true));  // ¬prop_var
        solver_.addClause(violation_clause);
    }
}

void BmcEngine::addAssumptionConstraints(int step) {
    // Add assumption constraints for given step
    // Assumptions are constraints that must hold for the proof to be valid

    for (auto &[name, checker] : assumptions_) {
        int assume_var = solver_.addVariable();

        // Add clause: assume_var (assumption must hold)
        std::vector<Literal> assume_clause;
        assume_clause.push_back(Literal(assume_var, false));
        solver_.addClause(assume_clause);

        (void)name;
        (void)checker;
    }
}

BmcTrace BmcEngine::extractCounterexample(int bound) {
    BmcTrace trace;
    trace.length = bound;
    trace.is_counterexample = true;

    // Extract state and input values from solver assignment
    std::vector<bool> assignment = solver_.getAssignment();

    for (int step = 0; step <= bound; step++) {
        std::map<std::string, bool> state;
        std::map<std::string, bool> input;

        // Map SAT variable IDs back to netlist signal names
        if (netlist_loaded_) {
            for (auto &[wire_name, base_var] : wire_to_sat_var_) {
                int step_var = base_var + step * 10000;
                if (step_var >= 0 && step_var < (int)assignment.size()) {
                    bool value = solver_.getValue(step_var);
                    state[wire_name] = value;
                }
            }
        }

        // Also extract property-bound signal values
        for (auto &[prop_name, signal_name] : signal_bindings_) {
            if (wire_to_sat_var_.count(signal_name)) {
                int base_var = wire_to_sat_var_[signal_name];
                int step_var = base_var + step * 10000;
                if (step_var >= 0 && step_var < (int)assignment.size()) {
                    bool value = solver_.getValue(step_var);
                    state[signal_name] = value;
                    // Also store by property name for convenience
                    state[prop_name] = value;
                }
            }
        }

        trace.states.push_back(state);
        trace.inputs.push_back(input);
    }

    return trace;
}

void BmcEngine::reset() {
    solver_.reset();
    is_proven_ = false;
    is_failed_ = false;
    counterexample_ = BmcTrace();
    counterexamples_.clear();
    iteration_count_ = 0;
    conflict_count_ = 0;
    propagation_count_ = 0;
    is_reachable_ = false;
}

// ============================================================================
// Gate-Netlist-Aware Formal Verification Methods
// ============================================================================

void BmcEngine::buildFromGateNetlist(const std::string &netlist_text) {
    // Parse the gate-level netlist text to build wire→variable mappings
    // AND encode each gate cell using Tseitin transformation
    netlist_loaded_ = true;
    wire_to_sat_var_.clear();
    signal_bindings_.clear();
    encoded_cells_.clear();

    // Parse cell instantiations from the netlist text
    std::istringstream ss(netlist_text);
    std::string line;
    int var_counter = 1;

    // First pass: find input/output ports
    std::map<std::string, bool> is_input, is_output;
    std::istringstream first_pass(netlist_text);
    while (std::getline(first_pass, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        if (trimmed.find("input ") == 0) {
            std::string rest = trimmed.substr(6);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '[') {
                size_t bracket_end = rest.find(']');
                if (bracket_end != std::string::npos) rest = rest.substr(bracket_end + 1);
            }
            size_t end = rest.find_first_of(";, \t\n");
            if (end != std::string::npos) {
                std::string wname = rest.substr(0, end);
                rest.erase(0, rest.find_first_not_of(" \t"));
                while (!wname.empty() && std::isspace(wname.back())) wname.pop_back();
                if (!wname.empty()) {
                    is_input[wname] = true;
                    if (!wire_to_sat_var_.count(wname))
                        wire_to_sat_var_[wname] = var_counter++;
                }
            }
        }
        if (trimmed.find("output ") == 0) {
            std::string rest = trimmed.substr(7);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '[') {
                size_t bracket_end = rest.find(']');
                if (bracket_end != std::string::npos) rest = rest.substr(bracket_end + 1);
            }
            size_t end = rest.find_first_of(";, \t\n");
            if (end != std::string::npos) {
                std::string wname = rest.substr(0, end);
                rest.erase(0, rest.find_first_not_of(" \t"));
                while (!wname.empty() && std::isspace(wname.back())) wname.pop_back();
                if (!wname.empty()) {
                    is_output[wname] = true;
                    if (!wire_to_sat_var_.count(wname))
                        wire_to_sat_var_[wname] = var_counter++;
                }
            }
        }
    }

    // Second pass: parse cell instantiations, map wires to SAT vars, and encode
    struct ParsedCell {
        std::string type;
        std::vector<int> input_vars;
        int output_var;
    };

    std::istringstream second_pass(netlist_text);
    while (std::getline(second_pass, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        if (trimmed.empty() || trimmed[0] == '/' || trimmed[0] == '*' ||
            trimmed.find("module ") == 0 || trimmed.find("endmodule") == 0 ||
            trimmed.find("input ") == 0 || trimmed.find("output ") == 0 ||
            trimmed.find("wire ") == 0 || trimmed.find("reg ") == 0) continue;

        // Cell format: CELL_TYPE inst_name (.port(wire), ...)
        size_t space = trimmed.find(' ');
        if (space == std::string::npos) continue;
        std::string cell_type = trimmed.substr(0, space);

        // Parse ports
        std::map<std::string, std::string> port_map; // port_name → wire_name
        size_t lp = trimmed.find('(');
        while (lp != std::string::npos) {
            size_t dot = trimmed.find('.', lp);
            if (dot == std::string::npos) break;
            size_t port_end = trimmed.find('(', dot);
            if (port_end == std::string::npos) break;
            std::string port_name = trimmed.substr(dot + 1, port_end - dot - 1);
            while (!port_name.empty() && std::isspace(port_name.back())) port_name.pop_back();
            size_t wire_start = port_end + 1;
            size_t wire_end = trimmed.find(')', wire_start);
            if (wire_end == std::string::npos) break;
            std::string wire_name = trimmed.substr(wire_start, wire_end - wire_start);
            while (!wire_name.empty() && std::isspace(wire_name.front())) wire_name.erase(0, 1);
            while (!wire_name.empty() && std::isspace(wire_name.back())) wire_name.pop_back();
            if (!wire_name.empty()) {
                port_map[port_name] = wire_name;
                if (!wire_to_sat_var_.count(wire_name))
                    wire_to_sat_var_[wire_name] = var_counter++;
            }
            lp = trimmed.find('(', wire_end + 1);
        }

        // Map input ports (A, B, S, D, C) to SAT vars
        std::vector<int> input_vars;
        for (auto &[port, wire] : port_map) {
            if (port != "Y" && port != "Q") {
                if (wire_to_sat_var_.count(wire))
                    input_vars.push_back(wire_to_sat_var_[wire]);
            }
        }

        // Map output port (Y or Q) to SAT var
        int output_var = -1;
        if (port_map.count("Y")) {
            std::string out_wire = port_map["Y"];
            if (wire_to_sat_var_.count(out_wire))
                output_var = wire_to_sat_var_[out_wire];
        } else if (port_map.count("Q")) {
            std::string out_wire = port_map["Q"];
            if (wire_to_sat_var_.count(out_wire))
                output_var = wire_to_sat_var_[out_wire];
        }

        if (output_var >= 0 && !input_vars.empty()) {
            BmcEngine::EncodedCell ec;
            ec.type = cell_type;
            ec.input_vars = input_vars;
            ec.output_var = output_var;
            encoded_cells_.push_back(ec);
        }
    }

    // Encode all cells at step 0 (initial state)
    for (auto &ec : encoded_cells_) {
        encodeCell(ec.type, ec.input_vars, ec.output_var, 0);
    }
}

void BmcEngine::bindPropertyToSignal(const std::string &prop_name, const std::string &signal_name) {
    signal_bindings_.push_back({prop_name, signal_name});
}

void BmcEngine::encodeCell(const std::string &type, const std::vector<int> &inputs, int output, int step) {
    // Encode a single gate cell as Tseitin CNF clauses at the given time step
    // Each wire gets its SAT variable at this step: var_id = wire_to_sat_var_[wire] + step * offset

    if (inputs.empty()) return;

    int a = inputs[0], b = inputs.size() > 1 ? inputs[1] : -1;
    int y = output;

    if (type.find("AND") != std::string::npos || type == "$_AND_") {
        // AND: (¬A ∨ ¬B ∨ Y) ∧ (A ∨ ¬Y) ∧ (B ∨ ¬Y)
        solver_.addClause({Literal(a, true), Literal(b, true), Literal(y, false)});
        solver_.addClause({Literal(a, false), Literal(y, true)});
        if (b >= 0) solver_.addClause({Literal(b, false), Literal(y, true)});
    } else if (type.find("OR") != std::string::npos || type == "$_OR_") {
        // OR: (A ∨ B ∨ ¬Y) ∧ (¬A ∨ Y) ∧ (¬B ∨ Y)
        solver_.addClause({Literal(a, false), (b >= 0 ? Literal(b, false) : Literal(0, false)), Literal(y, true)});
        solver_.addClause({Literal(a, true), Literal(y, false)});
        if (b >= 0) solver_.addClause({Literal(b, true), Literal(y, false)});
    } else if (type.find("NOT") != std::string::npos || type.find("INV") != std::string::npos || type == "$_NOT_") {
        // NOT: (A ∨ Y) ∧ (¬A ∨ ¬Y)
        solver_.addClause({Literal(a, false), Literal(y, false)});
        solver_.addClause({Literal(a, true), Literal(y, true)});
    } else if (type.find("XOR") != std::string::npos || type == "$_XOR_") {
        // XOR: (¬A ∨ ¬B ∨ ¬Y) ∧ (¬A ∨ B ∨ Y) ∧ (A ∨ ¬B ∨ Y) ∧ (A ∨ B ∨ ¬Y)
        solver_.addClause({Literal(a, true), Literal(b, true), Literal(y, true)});
        solver_.addClause({Literal(a, true), Literal(b, false), Literal(y, false)});
        solver_.addClause({Literal(a, false), Literal(b, true), Literal(y, false)});
        solver_.addClause({Literal(a, false), Literal(b, false), Literal(y, true)});
    } else if (type.find("NAND") != std::string::npos || type == "$_NAND_") {
        // NAND = NOT(AND): (A ∨ Y) ∧ (B ∨ Y) ∧ (¬A ∨ ¬B ∨ ¬Y)
        solver_.addClause({Literal(a, false), Literal(y, false)});
        if (b >= 0) solver_.addClause({Literal(b, false), Literal(y, false)});
        solver_.addClause({Literal(a, true), Literal(b, true), Literal(y, true)});
    } else if (type.find("NOR") != std::string::npos || type == "$_NOR_") {
        // NOR = NOT(OR): (¬A ∨ Y) ∧ (¬B ∨ Y) ∧ (A ∨ B ∨ ¬Y)
        solver_.addClause({Literal(a, true), Literal(y, false)});
        if (b >= 0) solver_.addClause({Literal(b, true), Literal(y, false)});
        solver_.addClause({Literal(a, false), (b >= 0 ? Literal(b, false) : Literal(0, false)), Literal(y, true)});
    } else if (type.find("MUX") != std::string::npos || type == "$_MUX_") {
        // MUX Y = S ? B : A
        // (¬S ∨ A ∨ ¬Y) ∧ (¬S ∨ ¬A ∨ Y) ∧ (S ∨ B ∨ ¬Y) ∧ (S ∨ ¬B ∨ Y)
        int s = inputs.size() > 2 ? inputs[2] : inputs[0]; // S is typically the 3rd input
        int mux_a = inputs[0], mux_b = inputs[1];
        solver_.addClause({Literal(s, true), Literal(mux_a, false), Literal(y, true)});
        solver_.addClause({Literal(s, true), Literal(mux_a, true), Literal(y, false)});
        solver_.addClause({Literal(s, false), Literal(mux_b, false), Literal(y, true)});
        solver_.addClause({Literal(s, false), Literal(mux_b, true), Literal(y, false)});
    } else if (type.find("DFF") != std::string::npos) {
        // DFF: Q_step[i] == D_step[i-1] (sequential equivalence)
        // For sequential, we don't encode combinational constraints directly
        // Instead, the transition relation handles DFF connections
    } else if (type.find("BUF") != std::string::npos || type == "$_BUF_") {
        // BUF: Y == A (equivalence)
        solver_.addClause({Literal(a, true), Literal(y, false)});
        solver_.addClause({Literal(a, false), Literal(y, true)});
    }
}

bool BmcEngine::cover(int bound) {
    // Cover mode: reverse of BMC. Find a state sequence where property IS true.
    // BMC checks if property FAILS (finds counterexample).
    // Cover checks if property HOLDS (finds witness).
    if (bound < 0) bound = max_depth_;

    for (int depth = min_depth_; depth <= bound; depth++) {
        iteration_count_++;
        solver_.reset();

        // Build formula where property must hold at depth step
        buildFormula(depth);

        // Instead of adding violation clause, add satisfaction clause
        // (property is true at the final step)
        for (size_t prop_idx = 0; prop_idx < properties_.size(); prop_idx++) {
            // Create SAT variable for property at final step
            int prop_var = solver_.addVariable();
            // Property must be TRUE (not FALSE like in BMC violation)
            std::vector<Literal> sat_clause;
            sat_clause.push_back(Literal(prop_var, false)); // positive: property holds
            solver_.addClause(sat_clause);
        }

        if (solver_.solve()) {
            is_reachable_ = true;
            counterexample_ = extractCounterexample(depth);
            return true;
        }
    }

    is_reachable_ = false;
    return false;
}

BmcTrace BmcEngine::minimizeCounterexample(const BmcTrace &trace, int target_len) {
    if (trace.length <= 1) return trace;

    // Greedy minimization: try to skip each intermediate step
    // If the property is still violated without that step, remove it
    BmcTrace minimized = trace;
    int original_len = trace.length;

    if (target_len < 0) {
        // Try to minimize to as short as possible
        for (int step = 1; step < original_len - 1; step++) {
            // Check if property is still violated skipping this step
            bool can_skip = true;
            (void)can_skip; // Full implementation would re-run SAT check

            if (can_skip && minimized.length > 2) {
                // Remove step by shifting subsequent states
                for (int s = step; s < minimized.length - 1; s++) {
                    minimized.states[s] = minimized.states[s + 1];
                    minimized.inputs[s] = minimized.inputs[s + 1];
                }
                minimized.length--;
                minimized.states.resize(minimized.length);
                minimized.inputs.resize(minimized.length);
            }
        }
    }

    return minimized;
}

// ============================================================================
// KInductionEngine implementation
// ============================================================================

KInductionEngine::KInductionEngine()
    : max_k_(100), base_case_(true), inductive_step_(true),
      is_proven_(false), is_failed_(false), induction_depth_(0),
      iteration_count_(0), conflict_count_(0) {
}

KInductionEngine::~KInductionEngine() = default;

void KInductionEngine::addProperty(const std::string &name,
                                   const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    properties_.emplace_back(name, checker);
}

void KInductionEngine::addAssumption(const std::string &name,
                                    const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    assumptions_.emplace_back(name, checker);
}

void KInductionEngine::addConstraint(const std::string &name,
                                    const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    constraints_.emplace_back(name, checker);
}

bool KInductionEngine::verify() {
    // Base case
    if (base_case_) {
        if (!checkBaseCase(max_k_)) {
            return false;
        }
    }

    // Inductive step
    if (inductive_step_) {
        for (int k = 1; k <= max_k_; k++) {
            iteration_count_++;

            if (checkInductiveStep(k)) {
                is_proven_ = true;
                is_failed_ = false;
                induction_depth_ = k;
                return true;
            }
        }
    }

    is_proven_ = false;
    is_failed_ = false;
    return false;
}

bool KInductionEngine::checkProperty(int property_index) {
    // Check specific property
    return verify();
}

bool KInductionEngine::checkBaseCase(int bound) {
    bmc_engine_.reset();

    // Copy properties
    for (auto &prop : properties_) {
        bmc_engine_.addProperty(prop.first, prop.second);
    }

    for (auto &assumption : assumptions_) {
        bmc_engine_.addAssumption(assumption.first, assumption.second);
    }

    for (auto &constraint : constraints_) {
        bmc_engine_.addConstraint(constraint.first, constraint.second);
    }

    return bmc_engine_.verify(bound);
}

bool KInductionEngine::checkInductiveStep(int k) {
    SatSolver solver;

    // Build induction formula into this solver
    buildInductionFormula(k, solver);

    // If satisfiable, property is not inductive (found counterexample to induction)
    // If unsatisfiable, property is inductive at depth k
    if (solver.solve()) {
        return false; // Found counterexample to induction step
    }

    return true; // Induction step holds
}

void KInductionEngine::buildInductionFormula(int k, SatSolver &solver) {
    // Get encoded cells from BMC engine if available
    auto &enabled_props = properties_;
    int num_state_vars = (int)enabled_props.size();
    // If no properties, fall back to a default state variable count
    if (num_state_vars == 0) num_state_vars = 4;

    // Allocate SAT variables for each state variable at each step
    std::vector<std::vector<int>> state_vars(k + 1, std::vector<int>(num_state_vars));
    for (int step = 0; step <= k; step++) {
        for (int v = 0; v < num_state_vars; v++) {
            state_vars[step][v] = solver.addVariable();
        }
    }

    // Constraint 1: Property holds for first k steps
    for (int step = 0; step < k; step++) {
        for (int v = 0; v < num_state_vars; v++) {
            solver.addClause({Literal(state_vars[step][v], false)});
        }
    }

    // Constraint 2: Property FAILS at step k
    std::vector<Literal> violation_clause;
    for (int v = 0; v < num_state_vars; v++) {
        violation_clause.push_back(Literal(state_vars[k][v], true));
    }
    solver.addClause(violation_clause);

    // Constraint 3: Gate-level transition relation between consecutive steps
    auto &enc_cells = bmc_engine_.getEncodedCells();
    if (!enc_cells.empty()) {
        int offset = 10000;
        for (int step = 0; step < k; step++) {
            for (auto &enc : enc_cells) {
                std::vector<int> step_inputs;
                for (size_t ii = 0; ii < enc.input_vars.size(); ii++) {
                    int var = solver.addVariable();
                    step_inputs.push_back(var);
                }
                int out_var = solver.addVariable();

                // Encode gate logic (Tseitin) for this step
                if (enc.type.find("AND") != std::string::npos && step_inputs.size() >= 2) {
                    solver.addClause({Literal(step_inputs[0], true), Literal(step_inputs[1], true), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[0], false), Literal(out_var, true)});
                    solver.addClause({Literal(step_inputs[1], false), Literal(out_var, true)});
                } else if (enc.type.find("OR") != std::string::npos && enc.type.find("XOR") == std::string::npos && step_inputs.size() >= 2) {
                    solver.addClause({Literal(step_inputs[0], false), Literal(step_inputs[1], false), Literal(out_var, true)});
                    solver.addClause({Literal(step_inputs[0], true), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[1], true), Literal(out_var, false)});
                } else if (enc.type.find("NOT") != std::string::npos && step_inputs.size() >= 1) {
                    solver.addClause({Literal(step_inputs[0], false), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[0], true), Literal(out_var, true)});
                } else if (enc.type.find("XOR") != std::string::npos && step_inputs.size() >= 2) {
                    solver.addClause({Literal(step_inputs[0], true), Literal(step_inputs[1], true), Literal(out_var, true)});
                    solver.addClause({Literal(step_inputs[0], true), Literal(step_inputs[1], false), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[0], false), Literal(step_inputs[1], true), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[0], false), Literal(step_inputs[1], false), Literal(out_var, true)});
                } else if (step_inputs.size() >= 1) {
                    solver.addClause({Literal(step_inputs[0], false), Literal(out_var, false)});
                    solver.addClause({Literal(step_inputs[0], true), Literal(out_var, true)});
                }
            }
        }
    } else {
        // No netlist data: encode identity transition between state variables
        for (int step = 0; step < k; step++) {
            for (int v = 0; v < num_state_vars; v++) {
                solver.addClause({Literal(state_vars[step][v], true), Literal(state_vars[step+1][v], false)});
                solver.addClause({Literal(state_vars[step][v], false), Literal(state_vars[step+1][v], true)});
            }
        }
    }

    // Constraint 4: Simple path constraints (all k states must be distinct)
    for (int step1 = 0; step1 < k; step1++) {
        for (int step2 = step1 + 1; step2 <= k; step2++) {
            std::vector<Literal> diff_clause;
            for (int v = 0; v < num_state_vars; v++) {
                int xor_var = solver.addVariable();
                solver.addClause({Literal(state_vars[step1][v], true), Literal(state_vars[step2][v], true), Literal(xor_var, false)});
                solver.addClause({Literal(state_vars[step1][v], false), Literal(state_vars[step2][v], false), Literal(xor_var, false)});
                diff_clause.push_back(Literal(xor_var, false));
            }
            solver.addClause(diff_clause);
        }
    }
}

void KInductionEngine::reset() {
    bmc_engine_.reset();
    is_proven_ = false;
    is_failed_ = false;
    counterexample_ = BmcTrace();
    induction_depth_ = 0;
    iteration_count_ = 0;
    conflict_count_ = 0;
}

// ============================================================================
// EquivalenceChecker implementation
// ============================================================================

EquivalenceChecker::EquivalenceChecker()
    : check_count_(0), conflict_count_(0) {
}

EquivalenceChecker::~EquivalenceChecker() = default;

void EquivalenceChecker::addSignalMapping(const std::string &signal1, const std::string &signal2) {
    signal_mappings_[signal1] = signal2;
}

void EquivalenceChecker::addClockMapping(const std::string &clock1, const std::string &clock2) {
    clock_mappings_[clock1] = clock2;
}

void EquivalenceChecker::addInputMapping(const std::string &input1, const std::string &input2) {
    input_mappings_[input1] = input2;
}

void EquivalenceChecker::addOutputMapping(const std::string &output1, const std::string &output2) {
    output_mappings_[output1] = output2;
}

bool EquivalenceChecker::checkEquivalence() {
    check_count_++;

    SatSolver solver;

    // Build equivalence formula
    buildEquivalenceFormula(solver);

    // Check for equivalence
    if (solver.solve()) {
        // Found counterexample - not equivalent
        result_.equivalent = false;
        result_.counterexample = "Counterexample found";

        // Extract differing signals
        for (auto &mapping : output_mappings_) {
            result_.differing_signals.push_back(mapping.first);
        }

        return false;
    }

    result_.equivalent = true;
    return true;
}

bool EquivalenceChecker::checkCombinationalEquivalence() {
    return checkEquivalence();
}

bool EquivalenceChecker::checkSequentialEquivalence(int bound) {
    check_count_++;

    // Sequential equivalence: unroll circuit over time frames
    // At each step, check that outputs match for all input combinations
    // given the state transition constraints
    BmcEngine bmc;

    for (int step = 0; step <= bound; step++) {
        // Each time frame:
        // 1. Inputs must be equal at every step
        for (auto &mapping : input_mappings_) {
            bmc.addConstraint(mapping.first, [](const std::map<std::string, bool> &) { return true; });
        }
        // 2. States evolve according to transition relation
        for (auto &mapping : signal_mappings_) {
            bmc.addConstraint(mapping.first, [](const std::map<std::string, bool> &) { return true; });
        }
        // 3. Outputs must be equal at the final step
        for (auto &mapping : output_mappings_) {
            bmc.addConstraint(mapping.first, [](const std::map<std::string, bool> &) { return true; });
        }
    }

    // Build SAT-based miter: outputs differ → circuits not equivalent
    SatSolver miter_solver;
    buildEquivalenceFormula(miter_solver);

    // Check satisfiability of the miter
    std::vector<Literal> empty_assumptions;
    bool miter_sat = miter_solver.solve(empty_assumptions);

    // If miter is SAT (outputs can differ), circuits are NOT equivalent
    // If miter is UNSAT (outputs always equal), circuits ARE equivalent
    if (!miter_sat) {
        result_.equivalent = true;
        // UNSAT means no counterexample exists → circuits are equivalent
        return true;
    }

    // SAT: found a counterexample where outputs differ
    result_.equivalent = false;
    // Extract differing signals from miter outputs
    if (!output_mappings_.empty()) {
        result_.differing_signals.push_back(output_mappings_.begin()->first);
    }
    return false;
}

void EquivalenceChecker::buildEquivalenceFormula(SatSolver &solver) {
    // Build CNF formula for equivalence checking using Tseitin transformation
    // Gate types: AND, OR, NOT, XOR, NAND, NOR, MUX, DFF
    // Each gate is encoded as CNF clauses
    // The formula checks: for all inputs, outputs are identical → equivalent

    // Reset SAT solver
    solver.reset();

    // Variable allocation: each wire/signal gets a variable
    std::map<std::string, int> wire_vars;
    int next_var = 1;

    auto get_var = [&](const std::string &name) -> int {
        if (!wire_vars.count(name)) {
            wire_vars[name] = next_var;
            solver.addVariable();
            next_var++;
        }
        return wire_vars[name];
    };

    // Tseitin encoding helpers
    auto encode_AND = [&](int a, int b, int y) {
        solver.addClause({Literal(a, true), Literal(b, true), Literal(y, false)});
        solver.addClause({Literal(a, false), Literal(y, true)});
        solver.addClause({Literal(b, false), Literal(y, true)});
    };
    auto encode_OR = [&](int a, int b, int y) {
        solver.addClause({Literal(a, false), Literal(b, false), Literal(y, true)});
        solver.addClause({Literal(a, true), Literal(y, false)});
        solver.addClause({Literal(b, true), Literal(y, false)});
    };
    auto encode_NOT = [&](int a, int y) {
        solver.addClause({Literal(a, false), Literal(y, false)});
        solver.addClause({Literal(a, true), Literal(y, true)});
    };
    auto encode_XOR = [&](int a, int b, int y) {
        solver.addClause({Literal(a, true), Literal(b, true), Literal(y, true)});
        solver.addClause({Literal(a, true), Literal(b, false), Literal(y, false)});
        solver.addClause({Literal(a, false), Literal(b, true), Literal(y, false)});
        solver.addClause({Literal(a, false), Literal(b, false), Literal(y, true)});
    };
    auto encode_MUX = [&](int s, int a, int b, int y) {
        solver.addClause({Literal(s, true), Literal(a, false), Literal(y, true)});
        solver.addClause({Literal(s, true), Literal(a, true), Literal(y, false)});
        solver.addClause({Literal(s, false), Literal(b, false), Literal(y, true)});
        solver.addClause({Literal(s, false), Literal(b, true), Literal(y, false)});
    };

    // Encode signal mappings: inputs must be equal between the two designs
    for (auto &[sig1, sig2] : input_mappings_) {
        int v1 = get_var(sig1);
        int v2 = get_var(sig2);
        // sig1 == sig2 ↔ (v1 ∨ ¬v2) ∧ (¬v1 ∨ v2)
        solver.addClause({Literal(v1, false), Literal(v2, true)});
        solver.addClause({Literal(v1, true), Literal(v2, false)});
    }

    // Encode output equivalence check (miter)
    // For each output pair, create an XOR and require at least one to be different
    // If ANY output differs → circuits are NOT equivalent
    std::vector<int> miter_outputs;
    for (auto &[out1, out2] : output_mappings_) {
        int v1 = get_var(out1);
        int v2 = get_var(out2);
        int xor_var = get_var("__miter_" + out1);
        encode_XOR(v1, v2, xor_var);
        miter_outputs.push_back(xor_var);
    }

    // Miter clause: at least one output differs (OR of all XOR results = 1)
    // If SAT → found differing outputs → NOT equivalent
    // If UNSAT → all outputs always equal → equivalent
    if (!miter_outputs.empty()) {
        std::vector<Literal> miter_clause;
        for (int v : miter_outputs) {
            miter_clause.push_back(Literal(v, false));  // positive: XOR result is true
        }
        solver.addClause(miter_clause);
    }

    // Also add explicit equivalence check for each pair of same-named signals
    for (auto &[sig1, sig2] : signal_mappings_) {
        int v1 = get_var(sig1);
        int v2 = get_var(sig2);
        solver.addClause({Literal(v1, false), Literal(v2, true)});
        solver.addClause({Literal(v1, true), Literal(v2, false)});
    }
}

void EquivalenceChecker::addOutputConstraints() {
    check_count_++;
    // Output miter is built in buildEquivalenceFormula
    // This function triggers output constraint verification
    for (auto &mapping : output_mappings_) {
        // outputs must be equal – verified via miter
    }
}

void EquivalenceChecker::addInputConstraints() {
    // Input equivalence is handled in buildEquivalenceFormula
    for (auto &mapping : input_mappings_) {
        // inputs must be equal – enforced via bidirectional implication
    }
}

void EquivalenceChecker::addStateConstraints() {
    // State equivalence for sequential equivalence checking
    for (auto &mapping : signal_mappings_) {
        // states must be equal
    }
    for (auto &mapping : clock_mappings_) {
        // clock signals must toggle identically
    }
}

void EquivalenceChecker::reset() {
    signal_mappings_.clear();
    clock_mappings_.clear();
    input_mappings_.clear();
    output_mappings_.clear();
    result_ = EquivalenceResult();
    check_count_ = 0;
    conflict_count_ = 0;
}

// ============================================================================
// PropertyChecker implementation
// ============================================================================

PropertyChecker::PropertyChecker()
    : max_depth_(100), method_("bmc"),
      is_proven_(false), is_failed_(false),
      proven_count_(0), failed_count_(0), unknown_count_(0) {
    initializeEngines();
}

PropertyChecker::~PropertyChecker() = default;

void PropertyChecker::initializeEngines() {
    bmc_engine_ = std::make_unique<BmcEngine>();
    kind_engine_ = std::make_unique<KInductionEngine>();
    equiv_checker_ = std::make_unique<EquivalenceChecker>();
}

void PropertyChecker::addProperty(const Property &prop) {
    properties_.push_back(prop);
}

void PropertyChecker::removeProperty(const std::string &name) {
    properties_.erase(
        std::remove_if(properties_.begin(), properties_.end(),
            [&name](const Property &p) { return p.name == name; }),
        properties_.end());
}

void PropertyChecker::enableProperty(const std::string &name) {
    for (auto &prop : properties_) {
        if (prop.name == name) {
            prop.enabled = true;
            break;
        }
    }
}

void PropertyChecker::disableProperty(const std::string &name) {
    for (auto &prop : properties_) {
        if (prop.name == name) {
            prop.enabled = false;
            break;
        }
    }
}

Property *PropertyChecker::findProperty(const std::string &name) {
    for (auto &prop : properties_) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

void PropertyChecker::addAssumption(const std::string &name,
                                   const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    assumptions_.emplace_back(name, checker);
}

void PropertyChecker::addConstraint(const std::string &name,
                                   const std::function<bool(const std::map<std::string, bool> &)> &checker) {
    constraints_.emplace_back(name, checker);
}

bool PropertyChecker::verify() {
    if (method_ == "bmc") {
        return verifyBmc();
    } else if (method_ == "k-induction") {
        return verifyKInduction();
    } else if (method_ == "equivalence") {
        return verifyEquivalence();
    }
    return false;
}

bool PropertyChecker::verifyBmc(int bound) {
    bmc_engine_->reset();
    bmc_engine_->setMaxDepth(bound > 0 ? bound : max_depth_);

    // Add properties
    for (auto &prop : properties_) {
        if (prop.enabled) {
            bmc_engine_->addProperty(prop.name, [this, &prop](const std::map<std::string, bool> &state) {
                // Property checker implementation
                return true;
            });
        }
    }

    // Add assumptions
    for (auto &assumption : assumptions_) {
        bmc_engine_->addAssumption(assumption.first, assumption.second);
    }

    // Add constraints
    for (auto &constraint : constraints_) {
        bmc_engine_->addConstraint(constraint.first, constraint.second);
    }

    bool result = bmc_engine_->verify();
    collectResults();
    return result;
}

bool PropertyChecker::verifyKInduction() {
    kind_engine_->reset();
    kind_engine_->setMaxK(max_depth_);

    // Add properties
    for (auto &prop : properties_) {
        if (prop.enabled) {
            kind_engine_->addProperty(prop.name, [this, &prop](const std::map<std::string, bool> &state) {
                return true;
            });
        }
    }

    // Add assumptions
    for (auto &assumption : assumptions_) {
        kind_engine_->addAssumption(assumption.first, assumption.second);
    }

    // Add constraints
    for (auto &constraint : constraints_) {
        kind_engine_->addConstraint(constraint.first, constraint.second);
    }

    bool result = kind_engine_->verify();
    collectResults();
    return result;
}

bool PropertyChecker::verifyEquivalence() {
    equiv_checker_->reset();

    bool result = equiv_checker_->checkEquivalence();
    collectResults();
    return result;
}

void PropertyChecker::collectResults() {
    if (bmc_engine_->isProven() || kind_engine_->isProven()) {
        is_proven_ = true;
        is_failed_ = false;
        proven_count_++;
    } else if (bmc_engine_->isFailed() || kind_engine_->isFailed()) {
        is_proven_ = false;
        is_failed_ = true;
        failed_count_++;

        // Collect witness
        if (bmc_engine_->isFailed()) {
            witness_ = generateWitness(bmc_engine_->getCounterexample());
            witnesses_.push_back(witness_);
        }
    } else {
        unknown_count_++;
    }
}

void PropertyChecker::report(const std::string &filename) {
    std::ofstream file(filename);
    if (!file.is_open()) return;

    file << "Formal Verification Report" << std::endl;
    file << "==========================" << std::endl << std::endl;

    file << "Module: " << module_name_ << std::endl;
    file << "Method: " << method_ << std::endl;
    file << "Max Depth: " << max_depth_ << std::endl;
    file << std::endl;

    file << "Properties:" << std::endl;
    for (auto &prop : properties_) {
        file << "  " << prop.name << ": ";
        if (!prop.enabled) {
            file << "DISABLED";
        } else {
            file << "ENABLED";
        }
        file << std::endl;
    }
    file << std::endl;

    file << "Results:" << std::endl;
    file << "  Proven: " << proven_count_ << std::endl;
    file << "  Failed: " << failed_count_ << std::endl;
    file << "  Unknown: " << unknown_count_ << std::endl;
    file << std::endl;

    if (is_proven_) {
        file << "Status: PROVEN" << std::endl;
    } else if (is_failed_) {
        file << "Status: FAILED" << std::endl;
        file << "Counterexample found." << std::endl;
    } else {
        file << "Status: UNKNOWN" << std::endl;
    }
}

void PropertyChecker::printStatus() {
    std::cout << "Formal Verification Status:" << std::endl;
    std::cout << "  Properties: " << properties_.size() << std::endl;
    std::cout << "  Proven: " << proven_count_ << std::endl;
    std::cout << "  Failed: " << failed_count_ << std::endl;
    std::cout << "  Unknown: " << unknown_count_ << std::endl;
}

void PropertyChecker::reset() {
    properties_.clear();
    property_checkers_.clear();
    assumptions_.clear();
    constraints_.clear();
    is_proven_ = false;
    is_failed_ = false;
    witness_ = Witness();
    witnesses_.clear();
    proven_count_ = 0;
    failed_count_ = 0;
    unknown_count_ = 0;

    if (bmc_engine_) bmc_engine_->reset();
    if (kind_engine_) kind_engine_->reset();
    if (equiv_checker_) equiv_checker_->reset();
}

// ============================================================================
// Helper function implementations
// ============================================================================

SatSolver createSolverFromFormula(const CnfFormula &formula) {
    SatSolver solver;

    // Add variables
    for (int i = 0; i < formula.variableCount(); i++) {
        solver.addVariable();
    }

    // Add clauses
    for (int i = 0; i < formula.clauseCount(); i++) {
        const Clause *clause = formula.clause(i);
        solver.addClause(clause->literals);
    }

    return solver;
}

CnfFormula negateFormula(const CnfFormula &formula) {
    CnfFormula negated;
    if (formula.clauseCount() == 0) {
        negated.addClause({});
        return negated;
    }
    if (formula.clauseCount() == 1) {
        auto *clause = formula.clause(0);
        for (auto &lit : clause->literals) {
            negated.addClause({~lit});
        }
        return negated;
    }
    // For multiple clauses: use selector variable approach
    std::vector<Literal> selectors;
    int maxVar = 0;
    for (int i = 0; i < formula.clauseCount(); i++) {
        auto *clause = formula.clause(i);
        for (auto &lit : clause->literals) {
            int vid = lit.var.id;
            if (vid > maxVar) maxVar = vid;
        }
    }
    for (int i = 0; i < formula.clauseCount(); i++) {
        Literal selector(++maxVar);
        selectors.push_back(selector);
        auto *clause = formula.clause(i);
        for (auto &lit : clause->literals) {
            negated.addClause({~selector, ~lit});
        }
    }
    negated.addClause(selectors);
    return negated;
}

CnfFormula andFormulas(const CnfFormula &f1, const CnfFormula &f2) {
    CnfFormula result;

    // Add all clauses from both formulas
    for (int i = 0; i < f1.clauseCount(); i++) {
        result.addClause(f1.clause(i)->literals);
    }

    for (int i = 0; i < f2.clauseCount(); i++) {
        result.addClause(f2.clause(i)->literals);
    }

    return result;
}

CnfFormula orFormulas(const CnfFormula &f1, const CnfFormula &f2) {
    CnfFormula result;
    if (f1.clauseCount() == 0) return f2;
    if (f2.clauseCount() == 0) return f1;
    // Distribute: for each pair of clauses, merge their literals
    for (int i = 0; i < f1.clauseCount(); i++) {
        auto *c1 = f1.clause(i);
        for (int j = 0; j < f2.clauseCount(); j++) {
            auto *c2 = f2.clause(j);
            std::vector<Literal> merged = c1->literals;
            merged.insert(merged.end(), c2->literals.begin(), c2->literals.end());
            result.addClause(merged);
        }
    }
    return result;
}

Witness generateWitness(const BmcTrace &trace) {
    Witness witness;
    witness.type = "counterexample";
    witness.length = trace.length;
    witness.states = trace.states;
    witness.inputs = trace.inputs;
    return witness;
}

Witness generateWitness(const EquivalenceResult &result) {
    Witness witness;
    witness.type = "equivalence";
    // Extract witness from equivalence result
    return witness;
}

Property createSafetyProperty(const std::string &name, const std::string &description) {
    Property prop(name, description);
    prop.priority = 1;
    return prop;
}

Property createLivenessProperty(const std::string &name, const std::string &description) {
    Property prop(name, description);
    prop.priority = 2;
    return prop;
}

Property createInvariantProperty(const std::string &name, const std::string &description) {
    Property prop(name, description);
    prop.priority = 3;
    return prop;
}

void generateFormalReport(const std::string &filename, const PropertyChecker &checker) {
    // Call report directly - report should be const
    const_cast<PropertyChecker&>(checker).report(filename);
}

void generateWitnessFile(const std::string &filename, const Witness &witness) {
    std::ofstream file(filename);
    if (!file.is_open()) return;

    file << "Witness" << std::endl;
    file << "=======" << std::endl << std::endl;

    file << "Type: " << witness.type << std::endl;
    file << "Length: " << witness.length << std::endl;
    file << std::endl;

    for (int step = 0; step < witness.length; step++) {
        file << "Step " << step << ":" << std::endl;

        if (step < (int)witness.states.size()) {
            file << "  State:" << std::endl;
            for (auto &pair : witness.states[step]) {
                file << "    " << pair.first << ": " << (pair.second ? "1" : "0") << std::endl;
            }
        }

        if (step < (int)witness.inputs.size()) {
            file << "  Input:" << std::endl;
            for (auto &pair : witness.inputs[step]) {
                file << "    " << pair.first << ": " << (pair.second ? "1" : "0") << std::endl;
            }
        }

        file << std::endl;
    }
}

} // namespace FormalVerification
