//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/LoopDepths.hpp
// Purpose: Per-block natural-loop nesting depth over an index-based CFG, the
//          spill-weight input of both function-wide register allocators and
//          the MirCfg::loopDepths() of both backends.
// Key invariants:
//   - Back edges are found by DFS from block 0; each contributes the natural
//     loop of its (latch, header) pair; depth is capped at 4.
// Ownership/Lifetime: Pure function; no global state.
// Links: src/codegen/aarch64/MirCfg.cpp, src/codegen/x86_64/MirCfg.cpp,
//        src/codegen/common/ra/IntervalAssign.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

/// @file
/// @brief Natural-loop nesting depth per block.

namespace zanna::codegen::ra {

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

} // namespace zanna::codegen::ra
