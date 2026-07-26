//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/analysis/Dominators.cpp
// Purpose: Implement the Cooper–Harvey–Kennedy dominator tree algorithm and
//          expose dominance queries for IL CFGs.
// Links: docs/internals/architecture.md#analysis
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dominator tree construction and query helpers.
/// @details Provides an out-of-line home for the algorithm so the header can
///          remain lightweight while documenting how dominance intersections are
///          computed and cached.

#include "il/analysis/Dominators.hpp"
#include "il/analysis/CFG.hpp"
#include "il/core/Function.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <unordered_set>

namespace zanna::analysis {

/// @brief Return the immediate dominator for a block.
/// @details Performs a map lookup against the cached immediate dominator table.
///          Entry blocks are stored with null dominators and therefore produce
///          `nullptr` results.  The tree must have been previously computed via
///          @ref computeDominatorTree.
/// @param B Block whose immediate dominator is sought.
/// @return Immediate dominator of @p B or `nullptr` if @p B is the entry block.
/// @invariant The dominator tree has been previously computed for the
/// containing function.
il::core::Block *DomTree::immediateDominator(il::core::Block *B) const {
    auto it = idom.find(B);
    return it == idom.end() ? nullptr : it->second;
}

/// @brief Check whether one block dominates another.
/// @details Walks up the dominator chain from @p B until reaching the entry or
///          encountering @p A.  Missing dominator entries terminate the search
///          early, signalling that the tree was not fully populated for the
///          block (such as unreachable regions).
/// @param A Potential dominator.
/// @param B Block being tested for domination.
/// @return `true` if @p A dominates @p B, otherwise `false`.
/// @invariant Both blocks belong to the same function and the dominator tree
/// is fully built.
bool DomTree::dominates(il::core::Block *A, il::core::Block *B) const {
    if (!A || !B)
        return false;
    if (A == B)
        return true;
    while (B) {
        auto it = idom.find(B);
        if (it == idom.end())
            return false;
        B = it->second;
        if (B == A)
            return true;
    }
    return false;
}

/// @brief Construct the dominator tree for a function.
/// @details Implements the Cooper–Harvey–Kennedy algorithm to derive immediate
///          dominators for every reachable block.  Builds a reverse-postorder
///          traversal, iteratively refines the immediate dominator map using the
///          standard intersection routine, and finally populates child lists for
///          convenience.
/// @param ctx CFG context used to access traversal helpers.
/// @param F Function whose dominator relationships are computed.
/// @return A fully populated dominator tree with parent and child links.
/// @invariant The function must have a valid control-flow graph with a
/// single entry block.
DomTree computeDominatorTree(const CFGContext &ctx, il::core::Function &F) {
    DomTree DT;
    auto rpo = reversePostOrder(ctx, F);
    if (rpo.empty())
        return DT;

    std::unordered_map<il::core::Block *, std::size_t> index;
    for (std::size_t i = 0; i < rpo.size(); ++i)
        index[rpo[i]] = i;

    il::core::Block *entry = rpo.front();
    DT.idom[entry] = nullptr;

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < rpo.size(); ++i) {
            il::core::Block *b = rpo[i];
            const auto &preds = predecessors(ctx, *b);

            il::core::Block *newIdom = nullptr;
            for (auto *p : preds) {
                if (DT.idom.contains(p)) {
                    newIdom = p;
                    break;
                }
            }
            if (!newIdom)
                continue;

            // Intersect two dominance paths by advancing along the dominator
            // chain using block visit indexes until the nearest common
            // ancestor is located.
            /// @brief Finds the nearest common node on two dominator chains.
            /// @param b1 First block.
            /// @param b2 Second block.
            /// @return Common dominator, or null when a chain is incomplete.
            auto intersect = [&](il::core::Block *b1,
                                 il::core::Block *b2) -> il::core::Block * {
                /// @brief Looks up a block's reverse-postorder index.
                /// @param block Block to query.
                /// @return Index, or `std::nullopt` when absent.
                auto blockIndex = [&](il::core::Block *block) -> std::optional<std::size_t> {
                    auto it = index.find(block);
                    if (it == index.end())
                        return std::nullopt;
                    return it->second;
                };
                while (b1 != b2) {
                    auto b1Index = blockIndex(b1);
                    auto b2Index = blockIndex(b2);
                    if (!b1Index || !b2Index)
                        return nullptr;
                    while (*b1Index > *b2Index) {
                        auto it = DT.idom.find(b1);
                        if (it == DT.idom.end() || !it->second)
                            return nullptr;
                        b1 = it->second;
                        b1Index = blockIndex(b1);
                        if (!b1Index)
                            return nullptr;
                    }
                    while (*b2Index > *b1Index) {
                        auto it = DT.idom.find(b2);
                        if (it == DT.idom.end() || !it->second)
                            return nullptr;
                        b2 = it->second;
                        b2Index = blockIndex(b2);
                        if (!b2Index)
                            return nullptr;
                    }
                }
                return b1;
            };

            bool complete = true;
            for (auto *p : preds) {
                if (p == newIdom || !DT.idom.contains(p))
                    continue;
                newIdom = intersect(p, newIdom);
                if (!newIdom) {
                    complete = false;
                    break;
                }
            }
            if (!complete)
                continue;

            auto existingIt = DT.idom.find(b);
            if (existingIt == DT.idom.end()) {
                DT.idom.emplace(b, newIdom);
                changed = true;
            } else if (existingIt->second != newIdom) {
                existingIt->second = newIdom;
                changed = true;
            }
        }
    }

    for (auto &[blk, id] : DT.idom) {
        if (id)
            DT.children[id].push_back(blk);
    }

    return DT;
}

/// @brief Return the immediate post-dominator of block @p B.
/// @param B Block whose immediate post-dominator is requested.
/// @return Immediate post-dominator, or nullptr for an exit, unknown, or
///         non-exit-reachable block without a computed parent.
il::core::Block *PostDomTree::immediatePostDominator(il::core::Block *B) const {
    auto it = ipostdom.find(B);
    return it == ipostdom.end() ? nullptr : it->second;
}

/// @brief Check whether block @p A post-dominates block @p B.
/// @details Walks up the post-dominator chain from @p B until reaching the
///          virtual exit (nullptr) or finding @p A.
/// @param A Potential post-dominator.
/// @param B Block whose exit paths are tested.
/// @return True when @p A is @p B or appears on its computed post-dominator chain.
bool PostDomTree::postDominates(il::core::Block *A, il::core::Block *B) const {
    if (!A || !B)
        return false;
    if (A == B)
        return true;
    while (B) {
        auto it = ipostdom.find(B);
        if (it == ipostdom.end())
            return false;
        B = it->second; // nullptr means we've reached the virtual exit
        if (B == A)
            return true;
    }
    return false;
}

/// @brief Compute post-dominator tree for function @p F.
///
/// @details Applies the Cooper–Harvey–Kennedy iterative algorithm on the
/// reversed CFG.  Exit blocks (no CFG successors) are initialised with
/// @c ipostdom = nullptr, representing the virtual exit node.  All other
/// blocks are processed in reverse-post-order of the reversed CFG, which
/// is obtained by reversing the post-order DFS from the exit blocks.
/// @param ctx CFG context providing successor and predecessor caches.
/// @param F Function whose post-dominance relationships are computed.
/// @return Post-dominator tree for blocks connected to a real exit; components
///         with no exit may remain without immediate-post-dominator entries.
PostDomTree computePostDominatorTree(const CFGContext &ctx, il::core::Function &F) {
    PostDomTree PDT;
    if (F.blocks.empty())
        return PDT;

    // -------------------------------------------------------------------------
    // Step 1: Compute post-order of the reversed CFG.
    //
    // A DFS that starts at exit blocks (no successors) and follows predecessors
    // of the original CFG is equivalent to a DFS on the reversed CFG starting
    // from the virtual exit.  Recording blocks in completion order yields the
    // post-order of the reversed CFG; reversing it gives the RPO we need for
    // the CHK iteration.
    // -------------------------------------------------------------------------
    std::vector<il::core::Block *> po_rev;
    po_rev.reserve(F.blocks.size());
    std::unordered_set<il::core::Block *> visited;
    visited.reserve(F.blocks.size());

    /// @brief Performs iterative DFS over predecessor edges from one root.
    /// @param start Exit or otherwise unvisited start block.
    auto dfs = [&](il::core::Block *start) {
        struct Frame {
            il::core::Block *block;
            std::size_t predecessorIndex;
        };
        std::vector<Frame> stack;
        visited.insert(start);
        stack.push_back({start, 0});
        while (!stack.empty()) {
            Frame &frame = stack.back();
            const auto &preds = predecessors(ctx, *frame.block);
            if (frame.predecessorIndex < preds.size()) {
                il::core::Block *pred = preds[frame.predecessorIndex++];
                if (visited.insert(pred).second)
                    stack.push_back({pred, 0});
                continue;
            }
            po_rev.push_back(frame.block);
            stack.pop_back();
        }
    };

    // Start the DFS from all exit blocks (successors of the virtual exit).
    for (auto &bb : F.blocks) {
        if (successors(ctx, bb).empty() && !visited.count(&bb))
            dfs(&bb);
    }
    // Handle blocks not reachable from any exit (e.g., infinite-loop bodies).
    for (auto &bb : F.blocks) {
        if (!visited.count(&bb))
            dfs(&bb);
    }

    // RPO of reversed CFG: reverse the post-order.
    std::vector<il::core::Block *> rpo_rev(po_rev.rbegin(), po_rev.rend());

    // -------------------------------------------------------------------------
    // Step 2: Assign RPO indices.
    //
    // The virtual exit node is conceptually at index 0 (the "entry" of the
    // reversed CFG).  Real block indices start at 1 so that nullptr (virtual
    // exit) naturally has the smallest index and the CHK intersection converges
    // toward it correctly.
    // -------------------------------------------------------------------------
    std::unordered_map<il::core::Block *, std::size_t> index;
    index.reserve(F.blocks.size() + 1);
    // nullptr (virtual exit) = index 0
    for (std::size_t i = 0; i < rpo_rev.size(); ++i)
        index[rpo_rev[i]] = i + 1;

    /// @brief Returns a block's reversed-CFG RPO index.
    /// @param b Block, or null for the virtual exit.
    /// @return RPO index, zero for the virtual exit, or the maximum when absent.
    auto getIdx = [&](il::core::Block *b) -> std::size_t {
        if (!b)
            return 0; // virtual exit
        auto it = index.find(b);
        return it != index.end() ? it->second : std::numeric_limits<std::size_t>::max();
    };

    // -------------------------------------------------------------------------
    // Step 3: Initialise exit blocks.
    //
    // Exit blocks' immediate post-dominator is the virtual exit (nullptr).
    // -------------------------------------------------------------------------
    for (auto &bb : F.blocks) {
        if (successors(ctx, bb).empty())
            PDT.ipostdom[&bb] = nullptr;
    }

    // -------------------------------------------------------------------------
    // Step 4: Iterative CHK algorithm on the reversed CFG.
    //
    // For each block in RPO of the reversed CFG, compute the intersection of
    // its successors' immediate post-dominators (successors in the original CFG
    // = predecessors in the reversed CFG).
    // -------------------------------------------------------------------------
    /// @brief Finds the common node on two post-dominator chains.
    /// @param b1 First block.
    /// @param b2 Second block.
    /// @return Common post-dominator, or `std::nullopt` for an incomplete chain.
    auto intersect = [&](il::core::Block *b1,
                         il::core::Block *b2) -> std::optional<il::core::Block *> {
        while (b1 != b2) {
            while (getIdx(b1) > getIdx(b2)) {
                auto it = PDT.ipostdom.find(b1);
                if (it == PDT.ipostdom.end())
                    return std::nullopt;
                b1 = it->second;
            }
            while (getIdx(b2) > getIdx(b1)) {
                auto it = PDT.ipostdom.find(b2);
                if (it == PDT.ipostdom.end())
                    return std::nullopt;
                b2 = it->second;
            }
        }
        return std::optional<il::core::Block *>{b1};
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto *b : rpo_rev) {
            // Exit blocks are already initialised and never change.
            if (successors(ctx, *b).empty())
                continue;

            const auto &succs = successors(ctx, *b);

            // Find the first already-processed successor as the initial candidate.
            il::core::Block *newIdom = nullptr;
            for (auto *s : succs) {
                if (PDT.ipostdom.count(s)) {
                    newIdom = s;
                    break;
                }
            }
            if (!newIdom)
                continue; // No processed successor yet; defer.

            // Intersect all processed successors.
            bool complete = true;
            for (auto *s : succs) {
                if (s == newIdom || !PDT.ipostdom.count(s))
                    continue;
                auto common = intersect(s, newIdom);
                if (!common) {
                    complete = false;
                    break;
                }
                newIdom = *common;
            }
            if (!complete)
                continue;

            auto existingIt = PDT.ipostdom.find(b);
            if (existingIt == PDT.ipostdom.end()) {
                PDT.ipostdom.emplace(b, newIdom);
                changed = true;
            } else if (existingIt->second != newIdom) {
                existingIt->second = newIdom;
                changed = true;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Step 5: Build child lists.
    // -------------------------------------------------------------------------
    for (auto &[blk, ipd] : PDT.ipostdom) {
        if (ipd)
            PDT.children[ipd].push_back(blk);
    }

    return PDT;
}

} // namespace zanna::analysis
