//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LoopRotate.hpp
// Purpose: Loop rotation pass -- converts while-style loops (header tests
//          condition, then branches to body or exit) into do-while form
//          (header falls through to body, latch tests condition). This
//          eliminates one branch per iteration and exposes the loop body
//          to better optimization by LICM and IndVarSimplify.
// Key invariants:
//   - Only rotates loops with a single latch and single exit.
//   - SSA form maintained via block parameter threading.
//   - Header must contain only a conditional branch (no side-effecting ops).
//   - Range-demoted signed arithmetic is restored to checked form before the
//     CFG rewrite can invalidate its verifier proof.
// Ownership/Lifetime: Stateless FunctionPass instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/transform/analysis/LoopInfo.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares guarded rotation of while-style IL loops into latch-tested form.
 *
 * @details Eligible single-latch, single-exit loops receive a pre-loop guard
 *          and move their repeated condition test to the latch, removing one
 *          branch from each taken iteration. The pass threads SSA block
 *          parameters across the new edges and restores checked arithmetic
 *          where the original CFG supplied range proofs that rotation removes.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Loop rotation pass converting while-loops to do-while form.
/// @details For loops where the header only tests a condition and branches,
///          this pass moves the condition test to the latch block, allowing
///          the loop body to execute without an initial branch. A guard
///          check is inserted before the loop to skip it entirely when the
///          condition is initially false.
class LoopRotate : public FunctionPass {
  public:
    /// @brief Return the stable pass-pipeline identifier.
    /// @return The pass name `"loop-rotate"`.
    std::string_view id() const override;

    /// @brief Rotate eligible outermost loops in a function.
    /// @param function Function whose loop CFG is rewritten in place.
    /// @param analysis Analysis manager used to recompute loop information after each rewrite.
    /// @return All analyses when unchanged; otherwise module analyses only are preserved.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

} // namespace il::transform
