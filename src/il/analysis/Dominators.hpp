//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/Dominators.hpp
// Purpose: Dominator tree analysis for IL functions. Captures dominance
//          relationships (block A dominates block B iff every path from
//          entry to B passes through A). Provides dominates() and
//          immediateDominator() queries, built via the iterative
//          Cooper-Harvey-Kennedy algorithm.
// Key invariants:
//   - A block dominates itself; the entry block dominates all reachable blocks.
//   - immediateDominator() returns nullptr for the entry and unindexed blocks.
//   - DomTree must be recomputed after any CFG mutation.
// Ownership/Lifetime: DomTree owns its idom and children maps by value.
//          Computed from a CFGContext + Function; block pointers must remain
//          stable while the DomTree is in use.
// Links: il/analysis/CFG.hpp, il/core/Function.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares forward and post-dominator tree analyses for IL functions.
 *
 * @details The analysis results map each basic block to its immediate
 *          dominator or post-dominator and expose ancestry queries over those
 *          trees. Forward dominance is rooted at the function entry, while
 *          post-dominance models multiple CFG exits through a virtual exit.
 *          Results borrow block pointers and therefore must be discarded after
 *          control-flow or block-storage mutations.
 */

#pragma once

#include <unordered_map>
#include <vector>

namespace il::core {
struct Function;
struct BasicBlock;
using Block = BasicBlock;
} // namespace il::core

namespace zanna::analysis {
struct CFGContext;

/// @brief Dominator tree for a function.
/// Stores immediate dominator relationships and tree children for each block.
struct DomTree {
    std::unordered_map<il::core::Block *, il::core::Block *>
        idom; ///< Maps each block to its immediate dominator
    std::unordered_map<il::core::Block *, std::vector<il::core::Block *>>
        children; ///< Maps each block to the blocks it immediately dominates

    /// @brief Return true if block @p A dominates block @p B.
    /// @param A Potential dominator.
    /// @param B Block being checked.
    /// @return True if A dominates B.
    bool dominates(il::core::Block *A, il::core::Block *B) const;

    /// @brief Return the immediate dominator of block @p B.
    /// @param B Block whose immediate dominator is requested.
    /// @return Immediate dominator or nullptr for the entry block.
    il::core::Block *immediateDominator(il::core::Block *B) const;
};

/// @brief Compute dominator tree for function @p F.
/// @param ctx CFG context providing traversal utilities.
/// @param F Function to analyze.
/// @return Dominator tree with immediate dominators and child map.
DomTree computeDominatorTree(const CFGContext &ctx, il::core::Function &F);

/// @brief Post-dominator tree for a function.
///
/// @details Stores immediate post-dominator relationships.  A block X
/// post-dominates block Y if every path from Y to any exit passes through X.
/// The tree is rooted at a virtual exit node represented by `nullptr`; all
/// actual exit blocks (blocks with no CFG successors) have their entry in
/// @c ipostdom set to `nullptr`.
///
/// Queries are analogous to forward dominator queries:
///   - @ref postDominates(A, B) — true iff A is an ancestor of B in the tree.
///   - @ref immediatePostDominator(B) — immediate parent of B in the tree.
struct PostDomTree {
    std::unordered_map<il::core::Block *, il::core::Block *>
        ipostdom; ///< Maps each block to its immediate post-dominator
    std::unordered_map<il::core::Block *, std::vector<il::core::Block *>>
        children; ///< Maps each block to the blocks it immediately post-dominates

    /// @brief Return true if block @p A post-dominates block @p B.
    /// @param A Potential post-dominator.
    /// @param B Block being tested.
    /// @return True if every path from @p B to any exit passes through @p A.
    bool postDominates(il::core::Block *A, il::core::Block *B) const;

    /// @brief Return the immediate post-dominator of block @p B.
    /// @param B Block whose immediate post-dominator is requested.
    /// @return Immediate post-dominator, or nullptr for exit blocks
    ///         (whose post-dominator is the virtual exit).
    il::core::Block *immediatePostDominator(il::core::Block *B) const;
};

/// @brief Compute post-dominator tree for function @p F.
///
/// @details Applies the Cooper-Harvey-Kennedy iterative algorithm on the
/// reversed CFG.  Exit blocks (those with no successors) are the roots; a
/// virtual exit node represented by @c nullptr connects them all.  The
/// computation is analogous to @ref computeDominatorTree but uses successor
/// lists instead of predecessor lists.
///
/// @param ctx CFG context providing traversal utilities.
/// @param F Function to analyze.
/// @return Post-dominator tree with immediate post-dominators and child map.
PostDomTree computePostDominatorTree(const CFGContext &ctx, il::core::Function &F);

} // namespace zanna::analysis
