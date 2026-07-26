//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/OwnershipOpt.hpp
// Purpose: Remove provably redundant runtime retain/release traffic.
// Key invariants:
//   - Pairs are removed only within one basic block.
//   - Any intervening use or ownership-affecting operation blocks removal.
// Ownership/Lifetime: Stateless FunctionPass instantiated by the registry.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares local elimination of redundant runtime ownership traffic.
 *
 * @details The function pass removes a balanced result-free retain/release pair
 *          only when both calls act on the same value and every intervening
 *          instruction is proven not to consume, expose, or perturb the
 *          temporary ownership increment.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Eliminate locally balanced, observably redundant ownership calls.
/// @details Searches each block for a result-free retain followed by a matching
///          result-free release of the same value. The pair is removed only when
///          every intervening instruction is proven unable to consume, expose,
///          or perturb the retained reference.
class OwnershipOpt : public FunctionPass {
  public:
    /// @brief Return the stable pipeline identifier.
    /// @return The pass name `"ownership-opt"`.
    std::string_view id() const override;

    /// @brief Remove redundant retain/release pairs from one function.
    /// @param function Function whose instruction lists are rewritten in place.
    /// @param analysis Analysis manager supplied by the pipeline; not queried.
    /// @return All analyses if unchanged; otherwise module, CFG, dominator, and loop
    ///         analyses are preserved because only instructions are erased.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the ownership cleanup function-pass factory.
/// @param registry Registry that receives the enabled-by-default pass entry.
void registerOwnershipOptPass(PassRegistry &registry);

} // namespace il::transform
