//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/Dominators.hpp
// Purpose: Backend-neutral bit-vector dominator analysis over an index-based
//          MIR control-flow graph. Both backends' MirCfg helpers compute
//          dominators through this one implementation.
// Key invariants:
//   - Entry block (index 0) dominates only itself in the result.
//   - Blocks with no predecessors (unreachable from entry) dominate only
//     themselves.
//   - The predecessor relation is index based: preds[i] lists the predecessor
//     block indices of block i; out-of-range indices are ignored.
// Ownership/Lifetime:
//   - Header-only; returns a value type with no shared state.
// Links: codegen/common/ra/CfgExtract.hpp,
//        codegen/aarch64/MirCfg.hpp, codegen/x86_64/MirCfg.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

/// @file
/// @brief Declares the shared packed dominator-set analysis for MIR CFGs.

namespace zanna::codegen::ra {

/// @brief Owns a packed dominator set for every indexed basic block.
///
/// `bits[block][word]` stores the corresponding 64-bit word of the block's
/// dominator bitset. The representation is independent of MIR objects, so
/// callers retain responsibility for keeping their block indices stable.
struct DominatorSets {
    /// Packed dominator bitsets indexed first by dominated block.
    std::vector<std::vector<std::uint64_t>> bits;

    /// Number of blocks represented when the analysis was computed.
    std::size_t blockCount = 0;

    /// @brief Test whether one indexed block dominates another.
    /// @param dominator Candidate dominator block index.
    /// @param block Candidate dominated block index.
    /// @return `true` when the dominator bit is present; `false` for an
    ///         out-of-range index or an absent relationship.
    [[nodiscard]] bool dominates(std::size_t dominator, std::size_t block) const noexcept {
        if (block >= bits.size())
            return false;
        const std::size_t word = dominator / 64;
        if (word >= bits[block].size())
            return false;
        return (bits[block][word] & (std::uint64_t{1} << (dominator % 64))) != 0;
    }
};

/// @brief Compute dominator sets for all blocks using iterative bit-vector dataflow.
/// @details Standard iterative algorithm: the entry block dominates only
///          itself, every other block starts as the universal set and is
///          intersected with each predecessor's dominator set until a fixed
///          point. Blocks without predecessors are unreachable and dominate
///          only themselves.
/// @param blockCount Total number of blocks (entry is index 0).
/// @param preds      preds[i] lists the predecessor block indices of block i.
///                   Missing lists and out-of-range indices are tolerated.
/// @return Dominator sets sized to @p blockCount.
[[nodiscard]] inline DominatorSets computeDominators(
    std::size_t blockCount, const std::vector<std::vector<std::size_t>> &preds) {
    DominatorSets result;
    result.blockCount = blockCount;
    if (blockCount == 0)
        return result;

    /// Set one block-index bit in a pre-sized packed vector.
    const auto setBit = [](std::vector<std::uint64_t> &bits, std::size_t index) noexcept {
        bits[index / 64] |= std::uint64_t{1} << (index % 64);
    };

    const std::size_t wordCount = (blockCount + 63) / 64;
    result.bits.assign(blockCount, std::vector<std::uint64_t>(wordCount, 0));

    const std::uint64_t tailMask = (blockCount % 64 == 0)
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : (std::uint64_t{1} << (blockCount % 64)) - 1;

    // Entry block dominates only itself; every other block starts as the
    // universal set so intersections converge from "everything" downward.
    setBit(result.bits[0], 0);
    for (std::size_t i = 1; i < blockCount; ++i) {
        std::fill(result.bits[i].begin(),
                  result.bits[i].end(),
                  std::numeric_limits<std::uint64_t>::max());
        result.bits[i].back() &= tailMask;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < blockCount; ++i) {
            std::vector<std::uint64_t> next(wordCount, 0);
            const bool hasPreds = (i < preds.size()) && !preds[i].empty();

            if (!hasPreds) {
                // Unreachable from entry: dominate only itself.
                setBit(next, i);
            } else {
                bool firstPred = true;
                for (std::size_t predIndex : preds[i]) {
                    if (predIndex >= blockCount)
                        continue;
                    if (firstPred) {
                        next = result.bits[predIndex];
                        firstPred = false;
                        continue;
                    }
                    for (std::size_t word = 0; word < wordCount; ++word)
                        next[word] &= result.bits[predIndex][word];
                }
                if (firstPred) {
                    // Every predecessor index was out of range.
                    std::fill(next.begin(), next.end(), 0);
                }
                setBit(next, i);
            }

            if (result.bits[i] != next) {
                result.bits[i] = std::move(next);
                changed = true;
            }
        }
    }

    return result;
}

} // namespace zanna::codegen::ra
