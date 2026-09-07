//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MirCfg.hpp
// Purpose: The one control-flow graph view of an AArch64 MIR function shared
//          by the register allocator, the verifier, and every post-RA
//          peephole that reasons across blocks (successors, predecessors,
//          fallthrough, dominators, back edges, natural loops), plus the
//          block-exit liveness seed post-RA rewrites must respect.
// Key invariants:
//   - Edges come from ra::classifyControlFlow through the shared extractor,
//     so a mid-block Br, a no-return call, a JumpTable, or a trailing
//     conditional branch yields the same edges for every consumer.
//   - Successor and predecessor lists are sorted and deduplicated.
//   - A back edge is an edge whose target dominates its source; a natural
//     loop is the header plus everything that reaches the latch without
//     passing through the header.
//   - blockExitLive() is a superset of what the allocator publishes in
//     MBasicBlock::carriedExitRegs: it adds the frame/stack/link registers,
//     the return registers of a block that leaves the function, and the
//     callee-saved registers of a block that does not (the epilogue restores
//     them at a return, so a value left there is dead).
// Ownership/Lifetime:
//   - MirCfg is a snapshot: it holds no reference to the function and is
//     invalidated by any change to block order or terminators.
// Links: src/codegen/aarch64/MirCfg.cpp, src/codegen/aarch64/ra/Liveness.hpp,
//        src/codegen/common/ra/CfgExtract.hpp,
//        src/codegen/common/ra/Dominators.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.3 / B1)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/common/ra/Dominators.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Declares the shared AArch64 MIR control-flow graph and exit-liveness helpers.

namespace zanna::codegen::aarch64 {

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
 * @brief Snapshot control-flow graph of an AArch64 MIR function.
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

    /// @brief Whether block @p bi ends its exit with exactly one edge to
    ///        @p to, taken either by a trailing `Br to` or by fallthrough.
    /// @details This is the condition under which instructions appended at
    ///          the end of @p bi execute only on the edge to @p to; a
    ///          conditional branch to @p to plus a different fallthrough does
    ///          not qualify.
    [[nodiscard]] bool exitsDirectlyTo(const MFunction &fn, std::size_t bi, std::size_t to) const;

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

/// @brief Physical registers the allocator says are carried across the exit
///        of @p block (MBasicBlock::carriedExitRegs) as a register set.
[[nodiscard]] PhysRegSet carriedExitRegSet(const MBasicBlock &block) noexcept;

/// @brief Physical registers that must be treated as live at the exit of
///        block @p bi of @p fn after register allocation.
/// @details The union of the block's carried exit registers and SP/FP/LR,
///          plus: when the block leaves the function (through `Ret` or by
///          falling off the end) the integer and floating-point return
///          registers, and otherwise the target's callee-saved GPR/FPR sets
///          (which hold pinned frame slots and values the allocator keeps
///          across blocks). At a return the epilogue restores every
///          callee-saved register from its slot, so a definition left there
///          is dead. Block-local rewrites that redirect or drop a definition
///          reaching the block end must consult this set (review item B1).
/// @param fn     Function owning the block.
/// @param bi     Block index in `[0, fn.blocks.size())`.
/// @param target ABI description supplying the register sets.
/// @return The live-at-exit register set.
[[nodiscard]] PhysRegSet blockExitLive(const MFunction &fn,
                                       std::size_t bi,
                                       const TargetInfo &target);

} // namespace zanna::codegen::aarch64
