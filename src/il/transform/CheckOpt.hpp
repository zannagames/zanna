//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/CheckOpt.hpp
// Purpose: Declares the CheckOpt function pass -- optimises check opcodes
//          (IdxChk, SDivChk0, UDivChk0, etc.) via dominance-based redundancy
//          elimination and loop-invariant check hoisting to preheaders.
// Key invariants:
//   - Checks are removed only when provably dominated by an identical check.
//   - Hoisting occurs only when operands are loop-invariant and the check
//     would execute on every loop entry.
//   - CFG structure is preserved when only removing instructions.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/analysis/Dominators.hpp,
//        il/transform/analysis/LoopInfo.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the IL check optimization function pass.

#pragma once

#include "il/transform/PassRegistry.hpp"

#include <string>
#include <unordered_map>

namespace il::transform {

/// @brief Optimize check opcodes via redundancy elimination and loop hoisting.
///
/// This pass identifies check instructions (IdxChk, SDivChk0, UDivChk0, etc.)
/// that are redundant due to dominating equivalent checks, or that can be
/// safely hoisted out of loops when their operands are loop-invariant.
class CheckOpt : public FunctionPass {
  public:
    /// @brief Identifier used when registering the pass.
    /// @return Stable pass registry name.
    std::string_view id() const override;

    /// @brief Run check optimization over @p function.
    /// @param function Function to optimize.
    /// @param analysis Analysis manager for querying dominators and loop info.
    /// @return PreservedAnalyses indicating which analyses remain valid.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;

  private:
    /// @brief Phase 0.5 of run(): demote overflow-checked ops to plain ops when
    ///        a guarding signed comparison (CBr) proves the operation cannot
    ///        overflow on the taken edge.
    /// @param function Function inspected and rewritten.
    /// @param blockMap Label-to-block lookup for guard successors.
    /// @param predecessorCounts Incoming edge counts for uniqueness checks.
    /// @return True if any instruction was rewritten.
    bool runGuardOverflowElim(core::Function &function,
                              const std::unordered_map<std::string, core::BasicBlock *> &blockMap,
                              const std::unordered_map<std::string, unsigned> &predecessorCounts);
};

/// @brief Register the CheckOpt pass with the provided registry.
/// @param registry PassRegistry to register the pass into.
void registerCheckOptPass(PassRegistry &registry);

} // namespace il::transform
