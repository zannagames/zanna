//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LICM.hpp
// Purpose: Loop-Invariant Code Motion -- function pass that hoists
//          loop-invariant, side-effect-free instructions to loop preheaders,
//          reducing redundant computation per iteration. Loads are hoisted
//          only when the loop contains no aliasing memory writes.
// Key invariants:
//   - Only hoists instructions that are pure, non-trapping, and whose operands
//     are defined outside the loop or are themselves loop-invariant.
//   - Never hoists string loads, whose results carry one owned reference per
//     dynamic execution.
//   - Assumes LoopSimplify has provided dedicated preheader/latch blocks.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/transform/analysis/LoopInfo.hpp,
//        il/analysis/BasicAA.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares loop-invariant code motion for IL functions.
 *
 * @details LICM hoists pure, non-trapping computations whose operands are
 *          invariant into canonical loop preheaders. Its optional memory mode
 *          additionally permits loads and readonly operations only when alias
 *          and ModRef facts prove the loop cannot invalidate them; ownership-
 *          bearing string loads remain fixed at their dynamic execution sites.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Perform loop-invariant code motion for trivially safe instructions.
/// @details Hoists instructions whose operands are loop-invariant, whose opcode
///          is side-effect free and non-trapping, and (for loads) only when the
///          loop contains no memory writes (based on BasicAA/modref metadata).
///          String loads are excluded because their results are ownership-bearing.
///          Assumes LoopSimplify has provided a dedicated preheader/latch.
class LICM : public FunctionPass {
  public:
    /// @brief Configure whether memory reads may be hoisted.
    /// @param allowMemoryHoisting Permit proven-safe loads and readonly calls when true.
    explicit LICM(bool allowMemoryHoisting = true);

    /// @brief Identifier used when registering the pass.
    /// @return `"licm"` when memory hoisting is enabled, otherwise `"licm-safe"`.
    std::string_view id() const override;

    /// @brief Run loop-invariant code motion over @p function.
    /// @param function Function whose natural loops are optimized in place.
    /// @param analysis Manager providing loop, CFG, dominance, and alias results.
    /// @return All analyses when unchanged, otherwise no preserved analyses.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;

  private:
    /// @brief Whether the pass may hoist memory-reading operations when proven safe.
    bool allowMemoryHoisting_ = true;
};

/// @brief Register the LICM pass with the provided registry.
/// @param registry Registry that receives the memory-hoisting pass entry.
void registerLICMPass(PassRegistry &registry);

/// @brief Register the memory-safe LICM subset with the provided registry.
/// @param registry Registry that receives the computation-only pass entry.
void registerLICMSafePass(PassRegistry &registry);

} // namespace il::transform
