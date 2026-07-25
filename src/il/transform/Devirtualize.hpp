//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/Devirtualize.hpp
// Purpose: Convert statically-known indirect calls to direct calls.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares constant function-address devirtualization.

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Rewrites resolvable `call.indirect` instructions as direct calls.
class Devirtualize : public FunctionPass {
  public:
    /// @brief Return the pass registry identifier.
    /// @return Stable pass name.
    std::string_view id() const override;
    /// @brief Run devirtualization on one function.
    /// @param function Function updated in place.
    /// @param analysis Analysis manager supplied by the pipeline.
    /// @return Preserved analysis summary.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the devirtualization pass factory.
/// @param registry Pass registry to update.
void registerDevirtualizePass(PassRegistry &registry);

} // namespace il::transform
