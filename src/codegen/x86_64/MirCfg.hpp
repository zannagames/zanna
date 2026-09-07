//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/MirCfg.hpp
// Purpose: The one control-flow graph view of an x86-64 MIR function shared
//          by the register allocator, the verifier, and the block-layout
//          peepholes (successors, predecessors, fallthrough, dominators, back
//          edges, natural loops).
// Key invariants:
//   - Edges come from ra::classifyControlFlow through the shared extractor,
//     so a mid-block JMP, a UD2, a JUMPTABLE, or a trailing JCC yields the
//     same edges for every consumer.
//   - Successor and predecessor lists are sorted and deduplicated.
//   - A back edge is an edge whose target dominates its source; a natural
//     loop is the header plus everything that reaches the latch without
//     passing through the header.
// Ownership/Lifetime:
//   - MirCfg is a snapshot: it holds no reference to the function and is
//     invalidated by any change to block order or terminators.
// Links: src/codegen/x86_64/MirCfg.cpp, src/codegen/x86_64/ra/Liveness.hpp,
//        src/codegen/common/ra/CfgExtract.hpp,
//        src/codegen/common/ra/Dominators.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.3)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ra/Dominators.hpp"
#include "codegen/x86_64/MachineIR.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Declares the shared x86-64 MIR control-flow graph snapshot.

namespace zanna::codegen::x64 {

/// @brief One latch -> header edge whose header dominates the latch.
struct BackEdge {
    std::size_t latch{0};  ///< Source block of the backward edge.
    std::size_t header{0}; ///< Dominating target block (loop header).
};

/// @brief Blocks of one natural loop, sorted by block index.
struct NaturalLoop {
    BackEdge edge{};                 ///< Back edge that induces the loop.
    std::vector<std::size_t> blocks; ///< Sorted member block indices (header included).

    /// @brief Membership test.
    [[nodiscard]] bool contains(std::size_t bi) const noexcept;
};

/**
 * @brief Snapshot control-flow graph of an x86-64 MIR function.
 *
 * Built once from the block list; queries never touch the function again.
 * Dominators are computed lazily on first use because most consumers only
 * need edges.
 */
class MirCfg {
  public:
    /// @brief Build the graph for @p fn in its current layout.
    explicit MirCfg(const MFunction &fn);

    /// @brief Number of blocks in the graph.
    [[nodiscard]] std::size_t size() const noexcept {
        return succs_.size();
    }

    /// @brief Sorted successor indices of block @p bi.
    [[nodiscard]] const std::vector<std::size_t> &succs(std::size_t bi) const {
        return succs_[bi];
    }

    /// @brief Sorted predecessor indices of block @p bi.
    [[nodiscard]] const std::vector<std::size_t> &preds(std::size_t bi) const {
        return preds_[bi];
    }

    /// @brief Whole successor table (index-based CFG for the shared solvers).
    [[nodiscard]] const std::vector<std::vector<std::size_t>> &successors() const noexcept {
        return succs_;
    }

    /// @brief Whole predecessor table.
    [[nodiscard]] const std::vector<std::vector<std::size_t>> &predecessors() const noexcept {
        return preds_;
    }

    /// @brief Label -> block index map.
    [[nodiscard]] const std::unordered_map<std::string, std::size_t> &blockIndex() const noexcept {
        return blockIndex_;
    }

    /// @brief Resolve a block label to its index.
    [[nodiscard]] std::optional<std::size_t> indexOf(const std::string &label) const;

    /// @brief Whether @p from has an edge to @p to.
    [[nodiscard]] bool hasEdge(std::size_t from, std::size_t to) const;

    /// @brief Whether block @p bi reaches block `bi + 1` by layout fallthrough
    ///        (no unconditional transfer ends the block).
    [[nodiscard]] bool fallsThrough(std::size_t bi) const {
        return fallsThrough_[bi] != 0;
    }

    /// @brief Whether any block other than the last relies on fallthrough.
    /// @details Block reordering changes control flow for such a function
    ///          unless the pair stays adjacent; the layout passes use this to
    ///          decline.
    [[nodiscard]] bool anyFallthrough() const noexcept;

    /// @brief Dominator sets (computed on first use).
    [[nodiscard]] const zanna::codegen::ra::DominatorSets &dominators() const;

    /// @brief Whether block @p a dominates block @p b.
    [[nodiscard]] bool dominates(std::size_t a, std::size_t b) const {
        return dominators().dominates(a, b);
    }

    /// @brief Every edge whose target dominates its source, ordered by
    ///        (latch, header).
    [[nodiscard]] std::vector<BackEdge> backEdges() const;

    /// @brief Natural loop induced by @p edge: the header plus every block
    ///        that reaches the latch without passing through the header.
    [[nodiscard]] NaturalLoop naturalLoop(const BackEdge &edge) const;

    /// @brief Per-block natural-loop nesting depth (see ra::computeLoopDepths).
    [[nodiscard]] std::vector<unsigned> loopDepths() const;

  private:
    std::vector<std::vector<std::size_t>> succs_;                         ///< Successor lists.
    std::vector<std::vector<std::size_t>> preds_;                         ///< Predecessor lists.
    std::vector<unsigned char> fallsThrough_;                             ///< Fallthrough flags.
    std::unordered_map<std::string, std::size_t> blockIndex_;             ///< Label -> index.
    mutable std::optional<zanna::codegen::ra::DominatorSets> dominators_; ///< Lazy dominators.
};

} // namespace zanna::codegen::x64
