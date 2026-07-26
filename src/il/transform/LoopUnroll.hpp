//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LoopUnroll.hpp
// Purpose: Loop Unrolling function pass -- replicates loop bodies to reduce
//          iteration overhead. Supports full unrolling of small constant-trip
//          loops, configurable via LoopUnrollConfig thresholds.
// Key invariants:
//   - Only unrolls innermost, single-latch, single-exit loops.
//   - Full unrolling limited by fullUnrollThreshold to prevent code bloat.
//   - Block parameters (SSA phi equivalents) are threaded correctly across
//     unrolled iterations.
// Ownership/Lifetime: FunctionPass holding a LoopUnrollConfig by value;
//          instantiated by the registry.
// Links: il/transform/PassRegistry.hpp, il/transform/analysis/LoopInfo.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares bounded full unrolling of simple counted IL loops.
 *
 * @details The pass recognizes innermost single-latch, single-exit loops with
 *          constant initial value, bound, and step, then clones safe body
 *          instructions for each exact iteration. Configuration caps both the
 *          accepted trip count and original body size to control code growth.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Configuration parameters for loop unrolling.
struct LoopUnrollConfig {
    /// @brief Maximum trip count for full unrolling, which eliminates the loop.
    unsigned fullUnrollThreshold = 8;

    /// @brief Maximum loop-body instruction count considered for unrolling.
    unsigned maxLoopSize = 50;
};

/// @brief Loop unrolling optimization pass.
/// @details Unrolls small constant-bound loops to reduce iteration overhead
///          and expose optimization opportunities. The pass identifies loops
///          with known trip counts and replicates their bodies, updating
///          SSA values appropriately.
class LoopUnroll : public FunctionPass {
  public:
    /// @brief Construct the pass with explicit size and trip-count limits.
    /// @param config Configuration copied into the pass instance.
    explicit LoopUnroll(LoopUnrollConfig config = {}) : config_(config) {}

    /// @brief Identifier used when registering the pass.
    /// @return The stable pass name `"loop-unroll"`.
    std::string_view id() const override;

    /// @brief Run loop unrolling over @p function.
    /// @param function Function whose eligible loops are expanded in place.
    /// @param analysis Manager used to recompute loop and CFG snapshots after each rewrite.
    /// @return All analyses when unchanged; otherwise module analyses only are preserved.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;

  private:
    /// @brief Size and trip-count limits retained by value.
    LoopUnrollConfig config_;
};

/// @brief Register the loop unrolling pass with the provided registry.
/// @param registry Registry that receives the sequential function-pass factory.
void registerLoopUnrollPass(PassRegistry &registry);

} // namespace il::transform
