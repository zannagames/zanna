//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/analysis/Liveness.hpp
// Purpose: Liveness analysis for IL functions -- computes live-in and live-out
//          sets for SSA temporaries at each basic block using backward dataflow
//          fixpoint iteration over dense bitsets indexed by temporary ID.
// Key invariants:
//   - Liveness sets are conservative: a live temporary is guaranteed to be
//     used along some path from the program point.
//   - Bitset size equals the total number of SSA value IDs in the function.
// Ownership/Lifetime: LivenessInfo owns its bitset storage by value. CFGInfo
//          owns its adjacency maps. Both are returned as self-contained values
//          from their respective compute functions.
// Links: il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares CFG summaries and block-level SSA liveness analysis.
 *
 * @details Liveness is represented by dense value-id bitsets and exposed
 *          through lightweight set views. Callers may either let the analysis
 *          construct CFG adjacency or supply a stable precomputed snapshot.
 */

#pragma once

#include "il/core/fwd.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace il::transform {

/// @brief Cached control-flow information for a function.
struct CFGInfo {
    /// @brief Outgoing block edges keyed by source block.
    std::unordered_map<const core::BasicBlock *, std::vector<const core::BasicBlock *>> successors;
    /// @brief Incoming block edges keyed by target block.
    std::unordered_map<const core::BasicBlock *, std::vector<const core::BasicBlock *>>
        predecessors;
};

/// @brief Cached liveness sets (live-in/live-out) for each block.
class LivenessInfo {
  public:
    /// @brief Lightweight view over the live value bitset for a block edge.
    class SetView {
      public:
        /// @brief Construct an unattached empty view.
        SetView() = default;

        /// @brief Query whether the set contains @p valueId.
        /// @param valueId SSA temporary identifier.
        /// @return True when live; false otherwise.
        bool contains(unsigned valueId) const;

        /// @brief Invoke a callback once for every set SSA id.
        /// @tparam Fn Callable accepting an unsigned id.
        /// @param fn Callback forwarded for each live value.
        template <typename Fn> void forEach(Fn &&fn) const {
            if (!bits_)
                return;
            for (unsigned id = 0; id < bits_->size(); ++id) {
                if ((*bits_)[id])
                    fn(id);
            }
        }

        /// @brief Check whether the set is empty.
        /// @return True for an unattached view or one with no set bits.
        bool empty() const;

        /// @brief Access the underlying bitset representation.
        /// @return Reference to a vector<bool> view; may reference an empty sentinel.
        const std::vector<bool> &bits() const;

      private:
        /// @brief Construct a borrowed view over an existing bitset.
        /// @param bits Bitset to observe, or nullptr for an unattached view.
        explicit SetView(const std::vector<bool> *bits);

        /// @brief Borrowed bitset storage, or nullptr for the empty view.
        const std::vector<bool> *bits_ = nullptr;

        friend class LivenessInfo;
    };

    /// @brief Live-in set for @p block.
    /// @param block Block represented in this analysis snapshot.
    /// @return View of values live before the block.
    SetView liveIn(const core::BasicBlock &block) const;
    /// @brief Live-in set for an optional block pointer.
    /// @param block Block pointer, or null.
    /// @return Corresponding view, or an empty view for null/unknown blocks.
    SetView liveIn(const core::BasicBlock *block) const;

    /// @brief Live-out set for @p block.
    /// @param block Block represented in this analysis snapshot.
    /// @return View of values live after the block.
    SetView liveOut(const core::BasicBlock &block) const;
    /// @brief Live-out set for an optional block pointer.
    /// @param block Block pointer, or null.
    /// @return Corresponding view, or an empty view for null/unknown blocks.
    SetView liveOut(const core::BasicBlock *block) const;

    /// @brief Total number of SSA value IDs tracked by the analysis.
    /// @return Dense bitset capacity.
    std::size_t valueCount() const;

  private:
    /// @brief Dense boolean storage used for each live-value set.
    using BitSet = std::vector<bool>;

    /// @brief Number of value identifiers addressable by every stored bitset.
    std::size_t valueCount_{0};
    /// @brief Borrowed blocks retained in function order for this snapshot.
    std::vector<const core::BasicBlock *> blocks_;
    /// @brief Maps each retained block pointer to its bitset-vector index.
    std::unordered_map<const core::BasicBlock *, std::size_t> blockIndex_;
    /// @brief Live-before sets indexed in parallel with @ref blocks_.
    std::vector<BitSet> liveInBits_;
    /// @brief Live-after sets indexed in parallel with @ref blocks_.
    std::vector<BitSet> liveOutBits_;

    friend LivenessInfo computeLiveness(core::Module &module, core::Function &fn);
    friend LivenessInfo computeLiveness(core::Module &module,
                                        core::Function &fn,
                                        const CFGInfo &cfg);
};

/// @brief Build CFG adjacency information for a function.
/// @param module Module used to construct the shared CFG context.
/// @param fn Function whose edges are indexed.
/// @return Self-contained successor and predecessor maps.
CFGInfo buildCFG(core::Module &module, core::Function &fn);

/// @brief Compute liveness sets for @p fn by building a CFG internally.
/// @param module Module containing @p fn.
/// @param fn Function analyzed.
/// @return Live-in and live-out bitsets for every block.
LivenessInfo computeLiveness(core::Module &module, core::Function &fn);

/// @brief Compute liveness sets for @p fn using a precomputed CFG.
/// @param module Module containing @p fn.
/// @param fn Function analyzed.
/// @param cfg Stable adjacency snapshot for @p fn.
/// @return Live-in and live-out bitsets for every block.
LivenessInfo computeLiveness(core::Module &module, core::Function &fn, const CFGInfo &cfg);

} // namespace il::transform
