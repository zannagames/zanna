//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Liveness.hpp
// Purpose: CFG-aware liveness analysis for the x86-64 register allocator.
//          Builds control-flow graph from MachineIR terminators and computes
//          gen/kill/liveIn/liveOut sets via backward dataflow iteration.
// Key invariants:
//   - liveOut[B] = union of liveIn[S] for all successors S of B.
//   - liveIn[B] = gen[B] union (liveOut[B] - kill[B]).
//   - Shared dataflow solver bounds fixed-point iteration at 1000 by default.
//   - Gen/kill computed from virtual register operands only; physregs ignored.
// Ownership/Lifetime:
//   - Value-owned containers valid for the lifetime of the LivenessAnalysis instance.
// Links: src/codegen/x86_64/ra/Liveness.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Liveness.hpp
 * @brief Declares CFG-aware virtual-register liveness analysis for x86-64 MIR.
 *
 * The analysis derives block successors and predecessors, records
 * upward-exposed uses and definitions, and solves backward dataflow equations.
 * Query results belong to the analysis object and are replaced by its next
 * invocation of LivenessAnalysis::run().
 */

#pragma once

#include "../MachineIR.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::x64::ra {

/// @brief CFG-aware liveness analysis over Machine IR blocks.
///
/// @details Builds the control-flow graph from JMP/JCC terminators, computes
///          gen/kill sets per block, and solves the standard backward dataflow
///          equations to produce live-in/live-out sets for each block. Only
///          virtual registers participate; physical registers are intentionally
///          excluded. The object retains no reference to the analyzed function.
class LivenessAnalysis {
  public:
    /// @brief Construct an analysis with no current function results.
    /// @post numBlocks() is zero until run() is called.
    LivenessAnalysis() = default;

    /// @brief Analyze virtual-register liveness for one machine function.
    /// @details Replaces all previously computed maps, graph edges, and
    ///          liveness sets before constructing the CFG and solving the
    ///          backward equations to a fixed point.
    /// @param func Machine function whose blocks and operands are inspected.
    /// @post numBlocks() equals `func.blocks.size()`, and every block has
    ///       corresponding predecessor, successor, live-in, and live-out sets.
    /// @throws Internal compiler error if MIR contains an unsplit in-block
    ///         label, the derived CFG is malformed, or dataflow fails to
    ///         converge within the shared solver's iteration bound.
    void run(const MFunction &func);

    /// @brief Return virtual registers live immediately after a block.
    /// @param blockIdx Zero-based index in the most recently analyzed function.
    /// @return Reference to the block's live-out set.
    /// @pre run() has completed and `blockIdx < numBlocks()`.
    /// @warning The reference is invalidated by the next run() call or
    ///          destruction of this object.
    [[nodiscard]] const std::unordered_set<uint16_t> &liveOut(std::size_t blockIdx) const;

    /// @brief Return virtual registers live immediately before a block.
    /// @param blockIdx Zero-based index in the most recently analyzed function.
    /// @return Reference to the block's live-in set.
    /// @pre run() has completed and `blockIdx < numBlocks()`.
    /// @warning The reference is invalidated by the next run() call or
    ///          destruction of this object.
    [[nodiscard]] const std::unordered_set<uint16_t> &liveIn(std::size_t blockIdx) const;

    /// @brief Return the direct CFG successors of a block.
    /// @param blockIdx Zero-based index in the most recently analyzed function.
    /// @return Reference to sorted, deduplicated successor block indices.
    /// @pre run() has completed and `blockIdx < numBlocks()`.
    /// @warning The reference is invalidated by the next run() call or
    ///          destruction of this object.
    [[nodiscard]] const std::vector<std::size_t> &successors(std::size_t blockIdx) const;

    /// @brief Return the direct CFG predecessors of a block.
    /// @param blockIdx Zero-based index in the most recently analyzed function.
    /// @return Reference to predecessor block indices. Their order follows
    ///         source-block traversal during graph reversal.
    /// @pre run() has completed and `blockIdx < numBlocks()`.
    /// @warning The reference is invalidated by the next run() call or
    ///          destruction of this object.
    [[nodiscard]] const std::vector<std::size_t> &predecessors(std::size_t blockIdx) const;

    /// @brief Return the number of blocks represented by the current results.
    /// @return Zero before the first run(), otherwise the block count of the
    ///         most recently analyzed function.
    [[nodiscard]] std::size_t numBlocks() const {
        return succs_.size();
    }

  private:
    /// Legacy declaration mirroring the shared solver's default iteration
    /// bound; run() currently relies directly on that default.
    static constexpr std::size_t kMaxIterations = 1000;

    ///< Maps each block label to its zero-based index in the current function.
    std::unordered_map<std::string, std::size_t> blockIndex_;
    ///< Direct CFG successor indices for each block.
    std::vector<std::vector<std::size_t>> succs_;
    ///< Direct CFG predecessor indices for each block.
    std::vector<std::vector<std::size_t>> preds_;
    ///< Per-block upward-exposed virtual-register uses.
    std::vector<std::unordered_set<uint16_t>> gen_;
    ///< Per-block virtual-register definitions.
    std::vector<std::unordered_set<uint16_t>> kill_;
    ///< Fixed-point virtual-register live-in sets.
    std::vector<std::unordered_set<uint16_t>> liveIn_;
    ///< Fixed-point virtual-register live-out sets.
    std::vector<std::unordered_set<uint16_t>> liveOut_;

    /// @brief Populate the label-to-block-index lookup for @p func.
    /// @param func Function whose block labels are indexed in layout order.
    /// @post A duplicate label, if present in malformed MIR, maps to its last
    ///       occurrence because later assignments replace earlier entries.
    void buildBlockIndex(const MFunction &func);

    /// @brief Build successor relations from classified control-flow operations.
    /// @details Conditional targets and layout fallthroughs are preserved,
    ///          while jumps, jump tables, returns, and traps terminate scanning.
    /// @param func Function whose already-split basic blocks form the CFG.
    /// @throws Internal compiler error if an instruction-level label remains
    ///         inside any basic block.
    void buildCFG(const MFunction &func);

    /// @brief Compute upward-exposed-use and definition sets for every block.
    /// @param func Function corresponding to the allocated gen/kill vectors.
    /// @pre gen_ and kill_ each contain one empty set per block in @p func.
    void computeGenKill(const MFunction &func);

    /// @brief Append an instruction's virtual-register uses and definitions.
    /// @details Operand roles classify register operands. Virtual base and
    ///          index registers in memory operands are always address uses.
    ///          Physical registers do not contribute to either output.
    /// @param instr Instruction whose operands are inspected.
    /// @param uses Destination receiving virtual-register uses.
    /// @param defs Destination receiving virtual-register definitions.
    /// @note Existing entries in @p uses and @p defs are preserved.
    static void collectVregs(const MInstr &instr,
                             std::vector<uint16_t> &uses,
                             std::vector<uint16_t> &defs);
};

} // namespace zanna::codegen::x64::ra
