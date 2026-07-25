//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/GlobalPinning.hpp
// Purpose: Target-independent core of the two-tier "pinned global" register
//          allocation scheme. Virtual registers that are live across
//          non-linear CFG boundaries (loop back edges, joins) are candidates
//          for a whole-lifetime physical register; the greedy assignment here
//          pins each candidate to one register for its entire lifetime so the
//          per-block local allocators never spill or reload it.
// Key invariants:
//   - Interference is tested at block granularity: two candidates may share a
//     physical register only when their live-block sets are disjoint. This is
//     conservative inside a block but never wrong.
//   - A pinned register belongs to its candidates for the whole function; the
//     consuming allocator must remove it from its free pools.
// Ownership/Lifetime: Value-semantics helpers; no global state.
// Links: src/codegen/x86_64/ra/Allocator.cpp, src/codegen/aarch64/ra/Allocator.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file GlobalPinning.hpp
 * @brief Defines target-independent whole-lifetime register pinning helpers.
 *
 * Copy-connected candidates may first be coalesced when their precise
 * intra-block segments do not overlap. A weighted greedy allocator then shares
 * physical registers only between candidates with disjoint live-block sets.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::ra {

/// @brief Live segment of a candidate within one block, in half-positions.
/// @details Position 2*i is instruction i's reads, 2*i+1 its writes; a value
///          that is live-in starts at 0 and one that is live-out ends at
///          UINT32_MAX. Two values may share a register inside a block iff
///          their segments do not overlap — a copy `t <- x` at instruction i
///          gives x an end of 2*i and t a start of 2*i+1, which is exactly
///          disjoint, while any real simultaneous liveness overlaps.
struct BlockSegment {
    ///< Inclusive half-position at which the value becomes live.
    uint32_t start{0};
    ///< Inclusive half-position at which the value ceases to be live.
    uint32_t end{0};
};

/// @brief One cross-block virtual register considered for whole-lifetime pinning.
struct GlobalPinCandidate {
    ///< Virtual register represented by this candidate or coalesced root.
    uint16_t vreg{0};
    /// Block-granularity live set: liveBlocks[b] != 0 when the vreg is live
    /// into, out of, or within block b.
    std::vector<char> liveBlocks{};
    /// Per-block live segments (keyed by block index) for copy coalescing.
    std::unordered_map<std::size_t, BlockSegment> segments{};
    /// Spill weight: sum over live blocks of (uses in block) x 10^loopDepth.
    /// Higher weight is assigned first.
    double weight{0.0};
};

/// @brief Merge copy-connected candidates whose lifetimes never overlap.
/// @details Loop-carried values typically form chains `param -> temp -> param`
///          connected by copies whose members are live in the same BLOCKS but
///          never at the same INSTRUCTION. Assigning each chain one register
///          removes the whole chain's memory traffic and turns the connecting
///          copies into identity moves. Union-find over @p copyPairs with a
///          per-shared-block segment-disjointness check keeps merges sound.
/// @param candidates Candidate set; merged in place (roots keep the union of
///        liveBlocks/segments and the summed weight; absorbed members are
///        removed).
/// @param copyPairs (dst, src) vreg pairs connected by register copies.
/// @return Map from absorbed member vreg to its surviving root vreg.
inline std::unordered_map<uint16_t, uint16_t>
coalescePinChains(std::vector<GlobalPinCandidate> &candidates,
                  const std::vector<std::pair<uint16_t, uint16_t>> &copyPairs) {
    std::unordered_map<uint16_t, std::size_t> index;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        index.emplace(candidates[i].vreg, i);

    // Union-find over candidate indices.
    std::vector<std::size_t> parent(candidates.size());
    for (std::size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;

    /// Find a union-find root while applying path compression.
    auto find = [&](std::size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    /// Test whether two inclusive half-position segments do not overlap.
    auto disjoint = [](const BlockSegment &a, const BlockSegment &b) {
        return a.end < b.start || b.end < a.start;
    };

    for (const auto &[dst, src] : copyPairs) {
        auto dstIt = index.find(dst);
        auto srcIt = index.find(src);
        if (dstIt == index.end() || srcIt == index.end())
            continue;
        const std::size_t rootA = find(dstIt->second);
        const std::size_t rootB = find(srcIt->second);
        if (rootA == rootB)
            continue;
        GlobalPinCandidate &a = candidates[rootA];
        GlobalPinCandidate &b = candidates[rootB];

        // Classes may merge only when every shared block's segments are
        // disjoint; otherwise the values are simultaneously live somewhere.
        bool mergeable = true;
        for (const auto &[blockIdx, segA] : a.segments) {
            auto segBIt = b.segments.find(blockIdx);
            if (segBIt != b.segments.end() && !disjoint(segA, segBIt->second)) {
                mergeable = false;
                break;
            }
        }
        if (!mergeable)
            continue;

        // Merge B into A: union live sets and segments, sum weights.
        for (std::size_t bi = 0; bi < a.liveBlocks.size() && bi < b.liveBlocks.size(); ++bi)
            a.liveBlocks[bi] = a.liveBlocks[bi] || b.liveBlocks[bi];
        for (const auto &[blockIdx, segB] : b.segments) {
            auto [it, inserted] = a.segments.emplace(blockIdx, segB);
            if (!inserted) {
                it->second.start = std::min(it->second.start, segB.start);
                it->second.end = std::max(it->second.end, segB.end);
            }
        }
        a.weight += b.weight;
        parent[rootB] = rootA;
    }

    // Compact: keep roots, report member -> root aliasing.
    std::unordered_map<uint16_t, uint16_t> alias;
    std::vector<GlobalPinCandidate> roots;
    roots.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::size_t root = find(i);
        if (root == i)
            continue;
        alias.emplace(candidates[i].vreg, candidates[root].vreg);
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (find(i) == i)
            roots.push_back(std::move(candidates[i]));
    }
    candidates = std::move(roots);
    return alias;
}

/// @brief Result of the greedy pin assignment.
template <typename PhysRegT> struct GlobalPinAssignment {
    /// vreg -> pinned physical register for its whole lifetime.
    std::unordered_map<uint16_t, PhysRegT> pinned{};
    /// Registers that hold at least one pinned candidate (for pool pruning).
    std::vector<PhysRegT> usedRegs{};
};

/// @brief Compute per-block natural-loop nesting depth from successor lists.
/// @details Back edges are found by DFS from block 0; each back edge (u -> h)
///          contributes the natural loop {h} plus every node that reaches u
///          without passing through h (reverse walk over predecessors). The
///          depth of a block is the number of such loops containing it,
///          capped at 4 so weights stay finite.
/// @param succs Per-block successor indices (index-based CFG).
/// @return Per-block loop depth; 0 for blocks outside any loop.
inline std::vector<unsigned> computeLoopDepths(const std::vector<std::vector<std::size_t>> &succs) {
    const std::size_t n = succs.size();
    std::vector<unsigned> depth(n, 0);
    if (n == 0)
        return depth;

    // Predecessor lists for the natural-loop reverse walk.
    std::vector<std::vector<std::size_t>> preds(n);
    for (std::size_t b = 0; b < n; ++b)
        for (std::size_t s : succs[b])
            if (s < n)
                preds[s].push_back(b);

    // Iterative DFS from block 0 collecting back edges (target on stack).
    std::vector<char> seen(n, 0);
    std::vector<char> onStack(n, 0);
    std::vector<std::pair<std::size_t, std::size_t>> backEdges; // (latch, header)
    std::vector<std::pair<std::size_t, std::size_t>> stack;     // (block, next succ)
    stack.emplace_back(0, 0);
    seen[0] = 1;
    onStack[0] = 1;
    while (!stack.empty()) {
        auto &[block, next] = stack.back();
        if (next < succs[block].size()) {
            const std::size_t succ = succs[block][next];
            ++next;
            if (succ >= n)
                continue;
            if (onStack[succ])
                backEdges.emplace_back(block, succ);
            if (!seen[succ]) {
                seen[succ] = 1;
                onStack[succ] = 1;
                stack.emplace_back(succ, 0);
            }
            continue;
        }
        onStack[block] = 0;
        stack.pop_back();
    }

    constexpr unsigned kMaxDepth = 4;
    for (const auto &[latch, header] : backEdges) {
        // Natural loop of (latch -> header): header plus reverse-reachable
        // nodes from latch that do not pass through header.
        std::vector<char> inLoop(n, 0);
        inLoop[header] = 1;
        std::vector<std::size_t> work;
        if (!inLoop[latch]) {
            inLoop[latch] = 1;
            work.push_back(latch);
        }
        while (!work.empty()) {
            const std::size_t node = work.back();
            work.pop_back();
            for (std::size_t pred : preds[node]) {
                if (!inLoop[pred]) {
                    inLoop[pred] = 1;
                    work.push_back(pred);
                }
            }
        }
        for (std::size_t b = 0; b < n; ++b)
            if (inLoop[b] && depth[b] < kMaxDepth)
                ++depth[b];
    }
    return depth;
}

/// @brief Greedily pin candidates to registers from @p pool.
/// @details Candidates are processed in descending weight order. Each is
///          assigned the first pool register whose already-pinned occupancy
///          (union of live-block sets) does not intersect the candidate's
///          live blocks; candidates that fit nowhere are left unpinned and
///          fall back to the consumer's existing spill-home path.
/// @param candidates Candidate set (consumed; sorted internally).
/// @param pool Physical registers available for pinning, in preference order.
/// @param blockCount Number of blocks (length of every live set).
/// @return Pin map plus the list of pool registers actually used.
template <typename PhysRegT>
GlobalPinAssignment<PhysRegT> assignGlobalPins(std::vector<GlobalPinCandidate> candidates,
                                               const std::vector<PhysRegT> &pool,
                                               std::size_t blockCount) {
    GlobalPinAssignment<PhysRegT> result;
    if (pool.empty() || candidates.empty() || blockCount == 0)
        return result;

    /// Order candidates by descending spill benefit with a deterministic vreg tie-break.
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return a.vreg < b.vreg; // deterministic tie-break
    });

    // occupancy[i][b] != 0 when pool[i] already carries a candidate live in b.
    std::vector<std::vector<char>> occupancy(pool.size(), std::vector<char>(blockCount, 0));
    std::vector<char> used(pool.size(), 0);

    for (const auto &candidate : candidates) {
        if (candidate.liveBlocks.size() != blockCount)
            continue;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            bool conflict = false;
            for (std::size_t b = 0; b < blockCount; ++b) {
                if (candidate.liveBlocks[b] && occupancy[i][b]) {
                    conflict = true;
                    break;
                }
            }
            if (conflict)
                continue;
            for (std::size_t b = 0; b < blockCount; ++b)
                if (candidate.liveBlocks[b])
                    occupancy[i][b] = 1;
            result.pinned.emplace(candidate.vreg, pool[i]);
            used[i] = 1;
            break;
        }
    }

    for (std::size_t i = 0; i < pool.size(); ++i)
        if (used[i])
            result.usedRegs.push_back(pool[i]);
    return result;
}

} // namespace zanna::codegen::ra
