//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/Liveness.hpp
// Purpose: CFG-aware liveness analysis for the AArch64 register allocator.
//          Builds the control-flow graph from AArch64 MIR branches, jump
//          tables, returns, and no-return calls, then computes per-block
//          gen/kill/liveIn/liveOut sets via backward dataflow iteration.
//
// Key invariants:
//   - liveOut[B] = union of liveIn[S] for all successors S of B
//   - liveIn[B] = gen[B] union (liveOut[B] - kill[B])
//   - GPR and FPR liveness computed independently
//   - Uses shared dataflow solver from common/ra/DataflowLiveness.hpp
//
// Ownership/Lifetime:
//   - Value-owned containers valid for the lifetime of the LivenessAnalysis.
//
// Links: codegen/common/ra/DataflowLiveness.hpp,
//        codegen/aarch64/ra/Allocator.cpp,
//        codegen/aarch64/ra/Liveness.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares CFG-aware virtual-register liveness for AArch64 allocation.

namespace zanna::codegen::aarch64::ra {

/// @brief CFG-aware liveness analysis over AArch64 Machine IR blocks.
///
/// @details Extracts explicit targets and layout fallthrough from MIR, computes
///          per-block virtual-register gen/kill sets split by register class,
///          and delegates fixed-point backward iteration to the common solver.
///          Analysis results are replaced by each call to @ref run.
class LivenessAnalysis {
  public:
    /// @brief Construct an empty analysis with no associated function.
    LivenessAnalysis() = default;

    /// @brief Run the full analysis: build CFG, compute gen/kill, solve dataflow.
    /// @param func Function whose current blocks and virtual operands are analyzed.
    /// @post All previously returned references are invalidated.
    void run(const MFunction &func);

    /// @brief Get the set of GPR vregs live at the exit of block @p blockIdx.
    /// @param blockIdx Analyzed block index in `[0, numBlocks())`.
    /// @return Const reference to the block's live-out GPR identifier set.
    /// @pre @ref run has completed and @p blockIdx is in range.
    [[nodiscard]] const std::unordered_set<uint16_t> &liveOutGPR(std::size_t blockIdx) const;

    /// @brief Get the set of FPR vregs live at the exit of block @p blockIdx.
    /// @param blockIdx Analyzed block index in `[0, numBlocks())`.
    /// @return Const reference to the block's live-out FPR identifier set.
    /// @pre @ref run has completed and @p blockIdx is in range.
    [[nodiscard]] const std::unordered_set<uint16_t> &liveOutFPR(std::size_t blockIdx) const;

    /// @brief Get the successor block indices for block @p blockIdx.
    /// @param blockIdx Analyzed block index in `[0, numBlocks())`.
    /// @return Const reference to the deduplicated successor list.
    /// @pre @ref run has completed and @p blockIdx is in range.
    [[nodiscard]] const std::vector<std::size_t> &successors(std::size_t blockIdx) const;

    /// @brief Get the predecessor block indices for block @p blockIdx.
    /// @param blockIdx Analyzed block index in `[0, numBlocks())`.
    /// @return Const reference to the predecessor list derived from successors.
    /// @pre @ref run has completed and @p blockIdx is in range.
    [[nodiscard]] const std::vector<std::size_t> &predecessors(std::size_t blockIdx) const;

    /// @brief Number of blocks in the analyzed function.
    /// @return Size of the current successor/result vectors, or zero before
    ///         analyzing a nonempty function.
    [[nodiscard]] std::size_t numBlocks() const {
        return succs_.size();
    }

  private:
    /// Maps each block label to its current function-local index.
    std::unordered_map<std::string, std::size_t> blockIndex_;

    /// CFG successor lists indexed by block.
    std::vector<std::vector<std::size_t>> succs_;

    /// Reverse CFG predecessor lists indexed by block.
    std::vector<std::vector<std::size_t>> preds_;

    /// Virtual GPR identifiers live at each block exit.
    std::vector<std::unordered_set<uint16_t>> liveOutGPR_;

    /// Virtual FPR identifiers live at each block exit.
    std::vector<std::unordered_set<uint16_t>> liveOutFPR_;

    /// @brief Build the label -> block index map.
    /// @param func Function supplying block names in stable layout order.
    void buildBlockIndex(const MFunction &func);

    /// @brief Extract successor relations from control-transfer instructions.
    /// @param func Function whose branches and fallthroughs define the CFG.
    void buildCFG(const MFunction &func);

    /// @brief Compute gen/kill sets and solve dataflow for both register classes.
    /// @param func Function whose virtual operand roles generate the dataflow sets.
    void computeLiveOutSets(const MFunction &func);
};

} // namespace zanna::codegen::aarch64::ra
