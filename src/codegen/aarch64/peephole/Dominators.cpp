//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/Dominators.cpp
// Purpose: Bit-vector iterative dominator analysis used by the AArch64 peephole
//          optimizer. Replaces three previously-duplicated copies.
//
// Key invariants:
//   - Pure function — no global state, no allocations beyond the returned value.
//
// Ownership/Lifetime:
//   - Free function returning a value type.
//
// Links: codegen/aarch64/peephole/Dominators.hpp
//
//===----------------------------------------------------------------------===//

#include "Dominators.hpp"

#include <algorithm>
#include <limits>

/// @file
/// @brief Implements iterative bit-vector dominator analysis.

namespace zanna::codegen::aarch64::peephole {

namespace {

/// @brief Set one block-index bit in a pre-sized packed vector.
/// @param[in,out] bits Packed bit vector to modify.
/// @param index Zero-based bit position; the caller guarantees it is in range.
inline void setBit(std::vector<std::uint64_t> &bits, std::size_t index) noexcept {
    bits[index / 64] |= std::uint64_t{1} << (index % 64);
}

} // namespace

/// @copydoc computeDominators
DominatorSets computeDominators(std::size_t blockCount,
                                const std::vector<std::vector<std::size_t>> &preds) {
    DominatorSets result;
    result.blockCount = blockCount;
    if (blockCount == 0)
        return result;

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
            const auto &p = (i < preds.size()) ? preds[i] : preds[0];
            const bool hasPreds = (i < preds.size()) && !preds[i].empty();

            if (!hasPreds) {
                // Unreachable from entry: dominate only itself.
                setBit(next, i);
            } else {
                bool firstPred = true;
                for (std::size_t predIndex : p) {
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

} // namespace zanna::codegen::aarch64::peephole
