//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/Dominators.hpp
// Purpose: Shared bit-vector dominator analysis for the AArch64 peephole
//          optimizer. Replaces three previously-duplicated implementations
//          (one bitset in Peephole.cpp, two set-based in LoopOpt.cpp).
//
// Key invariants:
//   - Entry block (index 0) dominates only itself in the result.
//   - Unreachable blocks (no predecessors after index 0) dominate only themselves.
//   - Predecessor list is indexed: preds[i] is the set of predecessor block
//     indices for block i. Callers must pre-build this list; the analysis
//     itself does not inspect terminators.
//
// Ownership/Lifetime:
//   - Free function returning a value type; no shared state.
//
// Links: codegen/aarch64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// @file
/// @brief Declares compact dominator-set analysis for AArch64 peephole passes.

namespace zanna::codegen::aarch64::peephole {

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
/// @details Standard iterative algorithm: entry block dominates only itself, all
///          others start as the universal set and are intersected with each
///          predecessor's dominator set until fixpoint. Blocks without valid
///          predecessors are treated as unreachable and dominate only themselves.
/// @param blockCount Total number of blocks (entry is index 0).
/// @param preds      preds[i] is the list of predecessor block indices for block i.
///                   Missing lists and out-of-range predecessor indices are
///                   handled conservatively.
/// @return Dominator sets sized to @p blockCount.
[[nodiscard]] DominatorSets computeDominators(std::size_t blockCount,
                                              const std::vector<std::vector<std::size_t>> &preds);

} // namespace zanna::codegen::aarch64::peephole
