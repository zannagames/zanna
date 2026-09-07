//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Liveness.cpp
// Purpose: Implementation of CFG-aware liveness analysis for x86-64 regalloc.
//          Computes per-block liveIn/liveOut sets using standard backward
//          dataflow equations over the machine IR control-flow graph.
// Key invariants:
//   - gen[B] = vregs used in B before being defined.
//   - kill[B] = vregs defined in B.
//   - liveOut[B] = union of liveIn[S] for all successors S.
//   - liveIn[B] = gen[B] union (liveOut[B] - kill[B]).
//   - Iteration terminates when no set changes (monotone lattice).
// Ownership/Lifetime:
//   - Operates on const MIR reference; results stored on the analysis instance.
// Links: src/codegen/x86_64/ra/Liveness.hpp,
//        src/codegen/x86_64/OperandRoles.hpp,
//        src/codegen/common/ra/DataflowLiveness.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Liveness.cpp
 * @brief Implements x86-64 MIR CFG extraction and virtual-register liveness.
 */

#include "Liveness.hpp"

#include "codegen/common/ICE.hpp"
#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/common/ra/DataflowLiveness.hpp"
#include "codegen/x86_64/OperandRoles.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Inspect an operand for a label value.
/// @param op Operand variant to inspect.
/// @return Pointer to the label's owned name when @p op is an OpLabel;
///         otherwise `nullptr`.
/// @warning The returned pointer is valid only while @p op and its contained
///          label remain alive and unmoved.
const std::string *getLabel(const Operand &op) {
    if (const auto *lbl = std::get_if<OpLabel>(&op))
        return &lbl->name;
    return nullptr;
}

/// @brief Return the first label operand of @p instr, or nullptr.
/// @param instr Instruction whose operands are searched from left to right.
/// @return Pointer to the first label name, or `nullptr` if none is present.
/// @warning The returned pointer refers to storage owned by @p instr.
const std::string *firstLabelOperand(const MInstr &instr) {
    for (const auto &op : instr.operands) {
        if (const auto *lbl = getLabel(op))
            return lbl;
    }
    return nullptr;
}

} // namespace

/// @copydoc classifyControlFlow
zanna::codegen::ra::BranchDesc classifyControlFlow(const MInstr &instr) {
    using Desc = zanna::codegen::ra::BranchDesc;
    switch (instr.opcode) {
        case MOpcode::JCC:
            return Desc{Desc::Kind::Cond, firstLabelOperand(instr)};
        case MOpcode::JMP:
            return Desc{Desc::Kind::Uncond, firstLabelOperand(instr)};
        case MOpcode::JUMPTABLE: {
            // Case labels start at operand 2 ([0]=index, [1]=table name).
            Desc desc{Desc::Kind::Multi, nullptr};
            for (std::size_t i = 2; i < instr.operands.size(); ++i) {
                if (const auto *label = std::get_if<OpLabel>(&instr.operands[i]))
                    desc.multiTargets.push_back(&label->name);
            }
            return desc;
        }
        case MOpcode::RET:
            return Desc{Desc::Kind::Return, nullptr};
        case MOpcode::UD2:
            return Desc{Desc::Kind::NoReturn, nullptr};
        default:
            return Desc{Desc::Kind::None, nullptr};
    }
}

/// @brief Top-level: build CFG, gen/kill, and solve backward dataflow.
/// @details Initialises per-block data containers, populates the label
///          index, builds successor/predecessor relations, computes
///          gen/kill from the operand role tables, then delegates to the
///          shared backward dataflow solver. The final liveIn/liveOut
///          vectors are stored on this instance for later queries.
/// @param func Machine function to analyse (consumed read-only).
/// @post Any prior analysis results are replaced.
/// @throws Internal compiler error for unsplit labels, malformed CFG data, or
///         failure of the shared dataflow solver to converge.
void LivenessAnalysis::run(const MFunction &func) {
    const std::size_t n = func.blocks.size();
    succs_.assign(n, {});
    gen_.assign(n, {});
    kill_.assign(n, {});
    blockIndex_.clear();

    buildBlockIndex(func);
    buildCFG(func);
    preds_ = zanna::codegen::ra::buildPredecessors(succs_);
    computeGenKill(func);

    // Delegate to the shared backward dataflow solver.
    auto result = zanna::codegen::ra::solveBackwardDataflow(succs_, gen_, kill_);
    liveIn_ = std::move(result.liveIn);
    liveOut_ = std::move(result.liveOut);
}

/// @brief Build the label -> block-index map used by CFG construction.
/// @details A simple linear scan; later passes consult this map to resolve
///          a JMP/JCC's label operand to the destination block index.
/// @param func Function whose block labels are indexed.
/// @post Each distinct label maps to its block's layout index; duplicate
///       labels map to the last matching block.
void LivenessAnalysis::buildBlockIndex(const MFunction &func) {
    for (std::size_t i = 0; i < func.blocks.size(); ++i)
        blockIndex_[func.blocks[i].label] = i;
}

/// @brief Compute the successor list for every block in @p func.
/// @details Delegates to the shared forward-scanning extractor so that every
///          conditional branch in a block contributes an edge (the previous
///          backward scan honored only the JCC nearest the final JMP, which
///          silently dropped switch-cascade successors). RET/UD2 produce no
///          successors; blocks without an unconditional terminator fall
///          through to the next block in layout order.
///
///          The allocator's per-block state machine assumes straight-line
///          execution between block boundaries, so in-block LABEL pseudos
///          must have been promoted to real blocks (splitInternalLabelBlocks)
///          before liveness runs. Enforce that here — a leaked LABEL would
///          mean the CFG below is wrong in ways that miscompile silently.
/// @param func Function whose instruction stream is classified.
/// @post succs_ contains one sorted, deduplicated list per input block.
/// @throws Internal compiler error if any block contains an MOpcode::LABEL.
void LivenessAnalysis::buildCFG(const MFunction &func) {
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::LABEL) {
                ZANNA_ICE("x86-64 liveness: in-block LABEL '" + toString(instr) + "' in block '" +
                          block.label +
                          "' reached register allocation; splitInternalLabelBlocks must run first");
            }
        }
    }

    /// @brief Exposes a block's instruction sequence to the CFG extractor.
    /// @param block Machine basic block.
    /// @return Block instruction vector.
    succs_ = zanna::codegen::ra::extractSuccessors(
        func.blocks,
        blockIndex_,
        [](const MBasicBlock &block) -> const std::vector<MInstr> & { return block.instructions; },
        /// @brief Classifies one instruction's control-flow behavior.
        /// @param instr Machine instruction to classify.
        /// @return Control-flow classification used by CFG extraction.
        [](const MInstr &instr) { return classifyControlFlow(instr); });
}

/// @brief Decompose an instruction's operands into use/def vreg lists.
/// @details Iterates operands and consults the operand-role table to
///          decide whether each register reference is a use, a def, or
///          both. Memory operands contribute base/index as uses since
///          they are read while computing the effective address.
///          Physical registers are skipped — only virtual-register
///          liveness is tracked at this stage.
/// @param instr Instruction whose operands are decomposed.
/// @param uses Output: virtual registers read by @p instr.
/// @param defs Output: virtual registers written by @p instr.
/// @note This function appends and does not clear either output vector.
void LivenessAnalysis::collectVregs(const MInstr &instr,
                                    std::vector<uint16_t> &uses,
                                    std::vector<uint16_t> &defs) {
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto &op = instr.operands[idx];

        // Handle register operands.
        if (const auto *reg = std::get_if<OpReg>(&op)) {
            if (reg->isPhys)
                continue; // Physical registers don't participate in vreg liveness.

            const auto [isUse, isDef] = operandRoles(instr, idx);
            if (isUse)
                uses.push_back(reg->idOrPhys);
            if (isDef)
                defs.push_back(reg->idOrPhys);
        }
        // Handle memory operand base/index registers as uses.
        else if (const auto *mem = std::get_if<OpMem>(&op)) {
            if (!mem->base.isPhys)
                uses.push_back(mem->base.idOrPhys);
            if (mem->hasIndex && !mem->index.isPhys)
                uses.push_back(mem->index.idOrPhys);
        }
    }
}

/// @brief Compute @c gen and @c kill sets for every block.
/// @details For each block, an instruction's uses contribute to @c gen
///          only if they are not already killed earlier in the block
///          (i.e., upward-exposed uses). Defs are added to @c kill
///          unconditionally. This formulation matches the textbook
///          backward dataflow equations for liveness.
/// @param func Function whose virtual-register operands are scanned.
/// @pre gen_ and kill_ have at least `func.blocks.size()` entries.
/// @post gen_[B] and kill_[B] contain the block-local dataflow inputs for
///       every block B.
void LivenessAnalysis::computeGenKill(const MFunction &func) {
    for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
        auto &gen = gen_[bi];
        auto &kill = kill_[bi];
        const auto &block = func.blocks[bi];

        for (const auto &instr : block.instructions) {
            std::vector<uint16_t> uses;
            std::vector<uint16_t> defs;
            collectVregs(instr, uses, defs);

            // gen: vregs used before being defined in this block.
            for (uint16_t u : uses) {
                if (kill.count(u) == 0)
                    gen.insert(u);
            }
            // kill: vregs defined in this block.
            for (uint16_t d : defs) {
                kill.insert(d);
            }
        }
    }
}

/// @copydoc LivenessAnalysis::liveOut()
const std::unordered_set<uint16_t> &LivenessAnalysis::liveOut(std::size_t blockIdx) const {
    return liveOut_[blockIdx];
}

/// @copydoc LivenessAnalysis::liveIn()
const std::unordered_set<uint16_t> &LivenessAnalysis::liveIn(std::size_t blockIdx) const {
    return liveIn_[blockIdx];
}

/// @copydoc LivenessAnalysis::successors()
const std::vector<std::size_t> &LivenessAnalysis::successors(std::size_t blockIdx) const {
    return succs_[blockIdx];
}

/// @copydoc LivenessAnalysis::predecessors()
const std::vector<std::size_t> &LivenessAnalysis::predecessors(std::size_t blockIdx) const {
    return preds_[blockIdx];
}

} // namespace zanna::codegen::x64::ra
