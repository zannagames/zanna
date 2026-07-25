//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/GVN.hpp
// Purpose: Global Value Numbering + Redundant Load Elimination -- function
//          pass that eliminates redundant pure computations across basic
//          blocks using dominator-tree preorder traversal, and removes
//          redundant loads via BasicAA memory disambiguation.
// Key invariants:
//   - Only eliminates side-effect-free, non-trapping computations.
//   - Available loads are invalidated by intervening stores/calls that may
//     clobber memory per BasicAA.
//   - Ownership-bearing string loads are never treated as redundant.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/analysis/BasicAA.hpp,
//        il/analysis/Dominators.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares dominator-scoped global value numbering and load reuse.

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Global Value Numbering pass that eliminates redundant computations.
/// @details Traverses the dominator tree in preorder, assigning value numbers
///          to pure instructions. Instructions with duplicate value numbers are
///          replaced with the dominating equivalent. Also performs redundant load
///          elimination using BasicAA memory disambiguation to track available
///          memory values, excluding ownership-bearing string loads.
class GVN : public FunctionPass {
  public:
    /// @brief Return the pass identifier string ("gvn").
    /// @return A string view identifying this pass in the registry.
    std::string_view id() const override;

    /// @brief Run GVN on a single function.
    /// @param function The function to optimize by eliminating redundant values.
    /// @param analysis The analysis manager providing dominator tree and alias info.
    /// @return A PreservedAnalyses set indicating which analyses remain valid.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the GVN pass with the pass registry under identifier "gvn".
/// @param registry The pass registry to register with.
void registerGVNPass(PassRegistry &registry);

} // namespace il::transform
