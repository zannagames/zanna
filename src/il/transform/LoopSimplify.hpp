//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LoopSimplify.hpp
// Purpose: Loop Simplification canonicalisation pass -- ensures each natural
//          loop has a unique preheader and, when trivial latches agree, a
//          dedicated latch block. Purely structural; does not change semantics.
// Key invariants:
//   - SSA form is maintained via proper block parameter threading.
//   - Only modifies loops that violate canonical form.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/transform/analysis/LoopInfo.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares structural canonicalization of natural IL loops.
 *
 * @details The pass gives each supported natural loop a dedicated preheader
 *          and can merge multiple equivalent trivial latches into one
 *          forwarding block. Rewrites preserve SSA by cloning header
 *          parameters and threading edge arguments, producing the form
 *          expected by later loop optimizations.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Loop canonicalisation pass that ensures well-structured loop form.
/// @details Transforms each natural loop to have a unique preheader block,
///          and merges multiple trivial latches when they forward identical
///          values. This canonical form is required by downstream loop
///          optimisation passes like IndVarSimplify and loop-invariant code motion.
class LoopSimplify : public FunctionPass {
  public:
    /// @brief Identifier used when registering the pass.
    /// @return The stable pass name `"loop-simplify"`.
    std::string_view id() const override;

    /// @brief Run the loop simplifier over @p function using @p analysis for queries.
    /// @param function Function whose natural loops are canonicalized in place.
    /// @param analysis Manager used to recompute loop information after each edit.
    /// @return All analyses when unchanged; otherwise module analyses only are preserved.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

} // namespace il::transform
