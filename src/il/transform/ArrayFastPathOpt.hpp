//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/ArrayFastPathOpt.hpp
// Purpose: Select unchecked numeric array runtime helpers after proven bounds checks.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares numeric array checked-to-fast helper optimization.

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Rewrites array accesses dominated by a matching bounds proof.
/// @details Facts are local to control-flow regions with unique guarded entry
///          and are cleared by potentially mutating or ownership-sensitive calls.
class ArrayFastPathOpt : public FunctionPass {
  public:
    /// @brief Return the pass registry identifier.
    /// @return Stable `array-fastpath` name.
    std::string_view id() const override;
    /// @brief Apply fast-helper selection to one function.
    /// @param function Function updated in place.
    /// @param analysis Analysis manager supplied by the pass pipeline.
    /// @return Analyses preserved by the rewrite.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the array fast-path pass factory.
/// @param registry Pass registry to update.
void registerArrayFastPathOptPass(PassRegistry &registry);

} // namespace il::transform
