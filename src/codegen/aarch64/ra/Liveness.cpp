//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/Liveness.cpp
// Purpose: Implementation of CFG-aware liveness analysis for AArch64 regalloc.
//          Computes per-block liveIn/liveOut sets using the shared backward
//          dataflow solver, with gen/kill split by register class (GPR/FPR).
//
// Key invariants:
//   - gen[B] = vregs used in B before being defined
//   - kill[B] = vregs defined in B
//   - GPR and FPR solved independently via shared solver
//   - CFG edges include all modeled branches, jump tables, fallthrough, returns,
//     and no-return calls
//
// Ownership/Lifetime:
//   - State owned by the LivenessAnalysis object; valid until run() is called again.
//
// Links: codegen/common/ra/DataflowLiveness.hpp,
//        codegen/aarch64/ra/Liveness.hpp,
//        codegen/aarch64/ra/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "Liveness.hpp"

#include "OperandRoles.hpp"
#include "codegen/aarch64/Noreturn.hpp"
#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/common/ra/DataflowLiveness.hpp"

#include <algorithm>

/// @file
/// @brief Implements AArch64 CFG extraction and backward virtual-register liveness.

namespace zanna::codegen::aarch64::ra {

/// @copydoc LivenessAnalysis::run
void LivenessAnalysis::run(const MFunction &func) {
    const std::size_t n = func.blocks.size();
    succs_.assign(n, {});
    liveOutGPR_.assign(n, {});
    liveOutFPR_.assign(n, {});
    blockIndex_.clear();

    buildBlockIndex(func);
    buildCFG(func);
    preds_ = zanna::codegen::ra::buildPredecessors(succs_);
    computeLiveOutSets(func);
}

/// @copydoc LivenessAnalysis::buildBlockIndex
void LivenessAnalysis::buildBlockIndex(const MFunction &func) {
    for (std::size_t i = 0; i < func.blocks.size(); ++i)
        blockIndex_[func.blocks[i].name] = i;
}

/// @copydoc LivenessAnalysis::buildCFG
void LivenessAnalysis::buildCFG(const MFunction &func) {
    /// Return the label operand used by register-test conditional branches.
    const auto condTarget = [](const MInstr &mi) -> const std::string * {
        if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Label)
            return &mi.ops[1].label;
        return nullptr;
    };

    succs_ = zanna::codegen::ra::extractSuccessors(
        func.blocks,
        blockIndex_,
        /// Expose the instruction vector expected by the generic CFG extractor.
        [](const MBasicBlock &bb) -> const std::vector<MInstr> & { return bb.instrs; },
        /// Translate an AArch64 instruction into the generic branch descriptor.
        [&](const MInstr &mi) {
            using Desc = zanna::codegen::ra::BranchDesc;
            switch (mi.opc) {
                case MOpcode::Br:
                    if (!mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Label)
                        return Desc{Desc::Kind::Uncond, &mi.ops[0].label};
                    return Desc{Desc::Kind::Uncond, nullptr};
                case MOpcode::BCond:
                case MOpcode::Cbz:
                case MOpcode::Cbnz:
                case MOpcode::Tbz:
                case MOpcode::Tbnz:
                    return Desc{Desc::Kind::Cond, condTarget(mi)};
                case MOpcode::JumpTable: {
                    // Case labels start at operand 2 ([0]=index, [1]=name).
                    Desc desc{Desc::Kind::Multi, nullptr};
                    for (std::size_t k = 2; k < mi.ops.size(); ++k) {
                        if (mi.ops[k].kind == MOperand::Kind::Label)
                            desc.multiTargets.push_back(&mi.ops[k].label);
                    }
                    return desc;
                }
                case MOpcode::Ret:
                    return Desc{Desc::Kind::Return, nullptr};
                default:
                    if (isNoReturnCall(mi))
                        return Desc{Desc::Kind::NoReturn, nullptr};
                    return Desc{Desc::Kind::None, nullptr};
            }
        });
}

/// @copydoc LivenessAnalysis::computeLiveOutSets
void LivenessAnalysis::computeLiveOutSets(const MFunction &func) {
    const std::size_t N = func.blocks.size();

    // Step 1: Compute gen/kill per block, split by register class.
    std::vector<std::unordered_set<uint16_t>> genGPR(N), killGPR(N);
    std::vector<std::unordered_set<uint16_t>> genFPR(N), killFPR(N);

    for (std::size_t i = 0; i < N; ++i) {
        for (const auto &mi : func.blocks[i].instrs) {
            for (std::size_t k = 0; k < mi.ops.size(); ++k) {
                const auto &op = mi.ops[k];
                if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                    continue;

                const uint16_t vid = op.reg.idOrPhys;
                const bool fprClass = (op.reg.cls == RegClass::FPR);
                auto &genSet = fprClass ? genFPR[i] : genGPR[i];
                auto &killSet = fprClass ? killFPR[i] : killGPR[i];

                auto [isUse, isDef] = operandRoles(mi, k);

                if (isUse && killSet.find(vid) == killSet.end())
                    genSet.insert(vid);

                if (isDef)
                    killSet.insert(vid);
            }
        }
    }

    // Step 2: Delegate to the shared dataflow solver.
    auto gprResult = zanna::codegen::ra::solveBackwardDataflow(succs_, genGPR, killGPR);
    auto fprResult = zanna::codegen::ra::solveBackwardDataflow(succs_, genFPR, killFPR);

    liveOutGPR_ = std::move(gprResult.liveOut);
    liveOutFPR_ = std::move(fprResult.liveOut);
}

/// @copydoc LivenessAnalysis::liveOutGPR
const std::unordered_set<uint16_t> &LivenessAnalysis::liveOutGPR(std::size_t blockIdx) const {
    return liveOutGPR_[blockIdx];
}

/// @copydoc LivenessAnalysis::liveOutFPR
const std::unordered_set<uint16_t> &LivenessAnalysis::liveOutFPR(std::size_t blockIdx) const {
    return liveOutFPR_[blockIdx];
}

/// @copydoc LivenessAnalysis::successors
const std::vector<std::size_t> &LivenessAnalysis::successors(std::size_t blockIdx) const {
    return succs_[blockIdx];
}

/// @copydoc LivenessAnalysis::predecessors
const std::vector<std::size_t> &LivenessAnalysis::predecessors(std::size_t blockIdx) const {
    return preds_[blockIdx];
}

} // namespace zanna::codegen::aarch64::ra
