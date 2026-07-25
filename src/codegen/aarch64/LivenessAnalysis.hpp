//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/LivenessAnalysis.hpp
// Purpose: Cross-block liveness analysis for IL->MIR lowering.
// Key invariants:
//   - Temps used in a different block than their definition are cross-block.
//   - Cross-block temps are assigned dedicated spill slots via FrameBuilder.
//   - Alloca temps are excluded from cross-block analysis.
// Ownership/Lifetime:
//   - Returns a value-type LivenessInfo; borrows Function and FrameBuilder
//     only for the duration of the call.
// Links: src/codegen/aarch64/LivenessAnalysis.cpp,
//        src/codegen/aarch64/LoweringContext.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares conservative IL temporary liveness across basic-block boundaries.
 *
 * The analysis records each temporary's defining block, finds uses in other
 * blocks, excludes recomputable alloca addresses, and reserves one persistent
 * frame spill for every cross-block value.
 */

#pragma once

#include "FrameBuilder.hpp"
#include "il/core/Function.hpp"

#include <unordered_map>
#include <unordered_set>

namespace zanna::codegen::aarch64 {

/// @brief Result of cross-block liveness analysis.
/// @invariant Every key in @ref crossBlockSpillOffset also appears in
///            @ref crossBlockTemps.
struct LivenessInfo {
    /// @brief Map of temp ID to the block index where it's defined.
    std::unordered_map<unsigned, std::size_t> tempDefBlock;

    /// @brief Set of temp IDs that are used in a different block than where defined.
    std::unordered_set<unsigned> crossBlockTemps;

    /// @brief Map of cross-block temp ID to its spill slot offset.
    std::unordered_map<unsigned, int> crossBlockSpillOffset;
};

/// @brief Analyze which temps are used across block boundaries.
/// @details Temps that are defined in one block and used in another must be
///          spilled at definition and reloaded at use, since the register
///          allocator processes blocks independently.
/// @param fn The IL function to analyze.
/// @param allocaTemps Set of temp IDs that are allocas (excluded from analysis).
/// @param fb Frame builder for allocating spill slots.
/// @return LivenessInfo containing the analysis results.
/// @post @p fb contains a spill slot for every returned cross-block temp.
LivenessInfo analyzeCrossBlockLiveness(const il::core::Function &fn,
                                       const std::unordered_set<unsigned> &allocaTemps,
                                       FrameBuilder &fb);

} // namespace zanna::codegen::aarch64
