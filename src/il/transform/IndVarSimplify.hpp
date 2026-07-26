//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/IndVarSimplify.hpp
// Purpose: Induction Variable Simplification + Loop Strength Reduction --
//          function pass that normalises simple counted loops to canonical
//          form (i < bound, positive step) and rewrites linear expressions
//          of induction variables into incremental loop-carried updates.
// Key invariants:
//   - Targets only single-latch, well-structured loops from LoopInfo.
//   - Hoisted initial values are placed in the loop preheader.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/transform/analysis/LoopInfo.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares induction-variable canonicalization and strength reduction.
 *
 * @details The function pass recognizes well-structured single-latch counted
 *          loops, normalizes supported comparisons and positive steps, and
 *          replaces loop-local linear induction expressions with initialized,
 *          incrementally updated loop-carried values. It relies on pipeline
 *          loop and dominance analysis and rewrites functions in place.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Induction variable simplification and loop strength reduction pass.
/// @details Normalises counted loops to canonical form (i < bound, positive step)
///          and rewrites linear expressions of induction variables into incremental
///          loop-carried updates, reducing computation within loop bodies.
class IndVarSimplify : public FunctionPass {
  public:
    /// @brief Return the pass identifier string ("indvars").
    /// @return A string view identifying this pass in the registry.
    std::string_view id() const override;

    /// @brief Run induction variable simplification on a single function.
    /// @param function The function containing loops to simplify.
    /// @param analysis The analysis manager providing loop info and dominators.
    /// @return A PreservedAnalyses set indicating which analyses remain valid.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the IndVarSimplify pass with the registry under identifier "indvars".
/// @param registry The pass registry to register with.
void registerIndVarSimplifyPass(PassRegistry &registry);

} // namespace il::transform
