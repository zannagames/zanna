//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/IfConvert.hpp
// Purpose: Convert small branch diamonds and triangles into `select`
//          instructions so backends can emit conditional moves instead of
//          branches.
// Key invariants:
//   - Only side-effect-free, non-trapping arm instructions are speculated.
//   - Arm blocks must be single-predecessor and joined through block params.
//   - Identical values on both incoming edges are forwarded without a select.
// Ownership/Lifetime:
//   - Stateless function pass owned by the pass registry.
// Links: docs/adr/0063-il-select-and-if-conversion.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Function pass folding branch diamonds/triangles into `select`.
/// @invariant Speculated instructions are pure and non-trapping.
/// @ownership Owned by the pass registry factory; no per-run state.
class IfConvert : public FunctionPass {
  public:
    /// @brief Return the stable pipeline identifier for this pass.
    /// @return The pass name `"if-conv"`.
    std::string_view id() const override;

    /// @brief Replace eligible conditional control flow with `select` instructions.
    /// @param function Function whose diamonds, triangles, and collapsed branches are examined.
    /// @param analysis Analysis manager supplied by the pass pipeline; this pass does not query it.
    /// @return All analyses when no rewrite occurs, otherwise no preserved analyses.
    /// @details The pass repeatedly rescans after a successful rewrite because removing arm
    ///          blocks invalidates block indices and predecessor counts. Conversion may be
    ///          disabled by setting `ZANNA_NO_IF_CONVERT` in the process environment.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the `if-conv` function-pass factory.
/// @param registry Registry that receives the enabled-by-default pass entry.
void registerIfConvertPass(PassRegistry &registry);

} // namespace il::transform
