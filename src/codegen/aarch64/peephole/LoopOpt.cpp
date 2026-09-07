//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/LoopOpt.cpp
// Purpose: Loop-invariant constant hoisting and phi-slot spill elimination for
//          the AArch64 peephole optimizer.
//
// Key invariants:
//   - Only hoists MovRI to callee-saved registers (x19-x28).
//   - The register must be defined only by MovRI with the same immediate value
//     throughout the loop body.
//   - Phi-slot rewrites require dominance-proven, call-free natural loops and
//     preserve parallel-copy semantics without a scratch register.
//   - Edges, dominators, and natural loops come from the shared MirCfg; the
//     passes keep no private CFG builder.
//
// Ownership/Lifetime:
//   - Operates on mutable MFunction owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp, codegen/aarch64/MirCfg.hpp
//
//===----------------------------------------------------------------------===//

#include "LoopOpt.hpp"

#include "PeepholeCommon.hpp"
#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MirCfg.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

/// @file
/// @brief Implements post-allocation natural-loop optimizations for AArch64 MIR.

namespace zanna::codegen::aarch64::peephole {
namespace {

/// @brief Return the branch-target label of a terminator instruction, or "" if
///        @p mi is not a (conditional or unconditional) branch.
/// @param mi Machine instruction whose label operand is inspected.
/// @return The direct target label for recognized branch forms, or an empty
///         string for malformed or non-branch instructions.
[[nodiscard]] std::string getBranchTarget(const MInstr &mi) {
    if (mi.opc == MOpcode::Br && !mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Label)
        return mi.ops[0].label;
    if (mi.opc == MOpcode::BCond && mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Label)
        return mi.ops[1].label;
    if ((mi.opc == MOpcode::Cbz || mi.opc == MOpcode::Cbnz || mi.opc == MOpcode::Tbz ||
         mi.opc == MOpcode::Tbnz) &&
        mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Label)
        return mi.ops[1].label;
    return {};
}

/// @brief Test whether a physical-register identifier is a hoistable GPR.
/// @param phys Numeric physical-register identifier.
/// @return `true` for the callee-saved range X19--X28.
[[nodiscard]] bool isCalleeSavedGPR(uint32_t phys) noexcept {
    return phys >= static_cast<uint32_t>(PhysReg::X19) &&
           phys <= static_cast<uint32_t>(PhysReg::X28);
}

/// @brief Record a physical register made live across @p block's exit by a
///        post-RA loop rewrite while preserving the sorted metadata invariant.
/// @param[in,out] block Block whose sorted `carriedExitRegs` vector is updated.
/// @param reg Physical register made live through the block exit. Non-register
///            and virtual-register operands are ignored.
void markCarriedExitReg(MBasicBlock &block, const MOperand &reg) {
    if (!isPhysReg(reg))
        return;
    const uint16_t phys = reg.reg.idOrPhys;
    const auto insertion =
        std::lower_bound(block.carriedExitRegs.begin(), block.carriedExitRegs.end(), phys);
    if (insertion == block.carriedExitRegs.end() || *insertion != phys)
        block.carriedExitRegs.insert(insertion, phys);
}

/// @brief A single register-to-register copy to be inserted on a loop back-edge
///        in place of a removed phi-slot store+load round-trip.
struct EdgeMove {
    std::size_t storeInstrIdx{0}; ///< Index of the phi store instruction being removed.
    MOperand srcReg{};            ///< Source physical register (the store's source).
    MOperand dstReg{};            ///< Destination physical register (the load's destination).
    RegClass cls{RegClass::GPR};  ///< GPR or FPR — selects MovRR vs FMovRR opcode.
};

/// @brief Order a set of parallel register moves so that no destination is overwritten
///        before its value has been read as a source.
/// @details Implements a greedy topological sort: each iteration picks a move whose
///          destination is not used as a source by any remaining move. Returns false
///          if the move set contains a cycle (which would require a scratch register).
/// @param moves   Unordered set of parallel edge moves to sequence.
/// @param ordered Output: moves in safe emission order (cleared on entry).
/// @return true if a safe ordering was found; false if a register cycle was detected.
[[nodiscard]] bool orderEdgeMoves(const std::vector<EdgeMove> &moves,
                                  std::vector<EdgeMove> &ordered) {
    ordered.clear();
    ordered.reserve(moves.size());

    std::vector<EdgeMove> pending;
    pending.reserve(moves.size());
    for (const auto &move : moves) {
        if (move.srcReg.reg.idOrPhys == move.dstReg.reg.idOrPhys)
            continue;
        pending.push_back(move);
    }

    while (!pending.empty()) {
        auto readyIt = pending.end();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            const auto dstPhys = it->dstReg.reg.idOrPhys;
            bool dstUsedAsSource = false;
            for (auto jt = pending.begin(); jt != pending.end(); ++jt) {
                if (it == jt)
                    continue;
                if (jt->cls == it->cls && jt->srcReg.reg.idOrPhys == dstPhys) {
                    dstUsedAsSource = true;
                    break;
                }
            }
            if (!dstUsedAsSource) {
                readyIt = it;
                break;
            }
        }
        if (readyIt == pending.end())
            return false;
        ordered.push_back(*readyIt);
        pending.erase(readyIt);
    }

    return true;
}

} // namespace

/// @copydoc hoistLoopConstants
std::size_t hoistLoopConstants(MFunction &fn) {
    if (fn.blocks.size() < 3)
        return 0;

    /// Test whether operand zero of @p opc is not an explicit GPR definition.
    ///
    /// This conservative classification lets the loop scan distinguish uses
    /// from the conventional destination operand without a full role table.
    auto isNonDefOpc = [](MOpcode opc) -> bool {
        return opc == MOpcode::StrRegFpImm || opc == MOpcode::StrRegBaseImm ||
               opc == MOpcode::Str8RegFpImm || opc == MOpcode::Str8RegBaseImm ||
               opc == MOpcode::Str16RegFpImm || opc == MOpcode::Str16RegBaseImm ||
               opc == MOpcode::Str32RegFpImm || opc == MOpcode::Str32RegBaseImm ||
               opc == MOpcode::StrRegSpImm || opc == MOpcode::StrFprFpImm ||
               opc == MOpcode::StrFprBaseImm || opc == MOpcode::StrFprSpImm ||
               opc == MOpcode::StpRegFpImm || opc == MOpcode::StpFprFpImm ||
               opc == MOpcode::CmpRR || opc == MOpcode::CmpRI || opc == MOpcode::TstRR ||
               opc == MOpcode::FCmpRR || opc == MOpcode::Br || opc == MOpcode::BCond ||
               opc == MOpcode::Cbz || opc == MOpcode::Cbnz || opc == MOpcode::Tbz ||
               opc == MOpcode::Tbnz || opc == MOpcode::JumpTable || opc == MOpcode::Ret ||
               opc == MOpcode::Bl || opc == MOpcode::Blr || opc == MOpcode::SubSpImm ||
               opc == MOpcode::AddSpImm || opc == MOpcode::PhiStoreGPR ||
               opc == MOpcode::PhiStoreFPR;
    };

    // Edges, dominators, and natural loops come from the shared CFG snapshot;
    // the rewrites below insert and erase MovRI instructions only, so it
    // stays valid for the whole pass.
    const MirCfg cfg(fn);

    /// @brief Indexed natural loop considered for constant hoisting.
    struct LoopInfo {
        /// Dominating back-edge target.
        std::size_t header{0};

        /// Back-edge source.
        std::size_t latch{0};

        /// Blocks in the natural loop (sorted).
        std::vector<std::size_t> body;

        /// @brief Membership test on @ref body.
        [[nodiscard]] bool contains(std::size_t bi) const noexcept {
            return std::binary_search(body.begin(), body.end(), bi);
        }
    };

    std::vector<LoopInfo> loops;
    std::unordered_set<std::size_t> seenHeaders;

    // One loop per header, keyed by its lowest-indexed latch. A layout-created
    // backward edge is not necessarily a loop (if/else joins can be placed
    // before one predecessor); MirCfg::backEdges() keeps only edges whose
    // target dominates the source. The preheader convention below (the block
    // laid out just before the header) needs the header ahead of its latch.
    for (const BackEdge &edge : cfg.backEdges()) {
        if (edge.header >= edge.latch)
            continue;
        if (!seenHeaders.insert(edge.header).second)
            continue;
        loops.push_back({edge.header, edge.latch, cfg.naturalLoop(edge).blocks});
    }

    if (loops.empty())
        return 0;

    std::unordered_map<uint32_t, int64_t> globallyHoisted;

    std::size_t hoisted = 0;

    for (const auto &loop : loops) {
        if (loop.header == 0)
            continue;

        // Skip "loops" whose header block contains a Ret instruction.
        // A block with Ret is a function exit, not a real loop header.
        // Back-edges to such blocks are exit paths, not iteration edges.
        {
            bool headerHasRet = false;
            if (loop.header < fn.blocks.size()) {
                for (const auto &mi : fn.blocks[loop.header].instrs) {
                    if (mi.opc == MOpcode::Ret) {
                        headerHasRet = true;
                        break;
                    }
                }
            }
            if (headerHasRet)
                continue;
        }

        // Skip "loops" whose header has multiple predecessors from outside the loop.
        // These are typically if/else merge points misidentified as loop headers.
        // A true loop header has exactly one entry edge from outside the loop (the
        // preheader) plus one back-edge from within the loop (the latch).
        {
            int outsidePreds = 0;
            for (std::size_t p : cfg.preds(loop.header)) {
                if (!loop.contains(p))
                    ++outsidePreds;
            }
            if (outsidePreds > 1)
                continue; // merge point, not a proper loop header
        }

        const std::size_t preIdx = loop.header - 1;

        bool preInLoop = false;
        for (const auto &other : loops) {
            if (&other == &loop)
                continue;
            if (other.contains(preIdx)) {
                preInLoop = true;
                break;
            }
        }
        if (preInLoop)
            continue;
        // Also skip if preIdx is inside THIS loop's own body.
        if (loop.contains(preIdx))
            continue;

        auto &preBlock = fn.blocks[preIdx];
        if (preBlock.instrs.empty())
            continue;

        // The preheader must actually reach the header (by branch or by
        // fallthrough); a block that returns, traps, or jumps elsewhere is
        // not a preheader even though layout puts it just before the loop.
        if (!cfg.hasEdge(preIdx, loop.header))
            continue;

        /// @brief Per-register evidence accumulated across one loop body.
        struct RegInfo {
            /// Number of matching immediate materializations encountered.
            std::size_t movriCount{0};

            /// Number of conflicting immediates or other definitions.
            std::size_t otherDefCount{0};

            /// Number of blocks that use the register without defining it locally.
            std::size_t useWithoutDefBlocks{0}; // blocks that USE but don't DEFINE

            /// Immediate shared by the candidate `MovRI` definitions.
            int64_t immValue{0};
        };

        std::unordered_map<uint32_t, RegInfo> regDefs;

        for (std::size_t bi : loop.body) {
            if (bi >= fn.blocks.size())
                continue;
            const auto &instrs = fn.blocks[bi].instrs;

            // Per-block: track which callee-saved GPRs are defined vs used
            std::unordered_set<uint32_t> definedInBlock;
            std::unordered_set<uint32_t> usedInBlock;
            for (const auto &mi : instrs) {
                if (mi.opc == MOpcode::MovRI && mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                    mi.ops[0].reg.cls == RegClass::GPR && isCalleeSavedGPR(mi.ops[0].reg.idOrPhys))
                    definedInBlock.insert(mi.ops[0].reg.idOrPhys);
                // Check uses (non-def operands)
                std::size_t startOp = isNonDefOpc(mi.opc) ? 0 : 1;
                for (std::size_t oi = startOp; oi < mi.ops.size(); ++oi) {
                    if (mi.ops[oi].kind == MOperand::Kind::Reg && mi.ops[oi].reg.isPhys &&
                        mi.ops[oi].reg.cls == RegClass::GPR &&
                        isCalleeSavedGPR(mi.ops[oi].reg.idOrPhys))
                        usedInBlock.insert(mi.ops[oi].reg.idOrPhys);
                }
            }
            // Count blocks that USE a register without defining it in the same block
            for (uint32_t r : usedInBlock) {
                if (!definedInBlock.count(r))
                    regDefs[r].useWithoutDefBlocks++;
            }

            for (std::size_t ii = 0; ii < instrs.size(); ++ii) {
                const auto &mi = instrs[ii];
                if (mi.opc == MOpcode::MovRI && mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                    mi.ops[0].reg.cls == RegClass::GPR && mi.ops[1].kind == MOperand::Kind::Imm) {
                    const uint32_t phys = mi.ops[0].reg.idOrPhys;
                    auto &info = regDefs[phys];
                    if (info.movriCount == 0)
                        info.immValue = mi.ops[1].imm;
                    else if (mi.ops[1].imm != info.immValue)
                        ++info.otherDefCount;
                    ++info.movriCount;
                } else {
                    if (!mi.ops.empty() && isPhysReg(mi.ops[0]) &&
                        mi.ops[0].reg.cls == RegClass::GPR && !isNonDefOpc(mi.opc)) {
                        ++regDefs[mi.ops[0].reg.idOrPhys].otherDefCount;
                    }
                    if (mi.opc == MOpcode::Bl || mi.opc == MOpcode::Blr) {
                        for (uint32_t r = static_cast<uint32_t>(PhysReg::X0);
                             r <= static_cast<uint32_t>(PhysReg::X17);
                             ++r)
                            ++regDefs[r].otherDefCount;
                    }
                }
            }
        }

        auto &preInstrs = preBlock.instrs;
        std::size_t insertIdx = preInstrs.size();
        while (insertIdx > 0) {
            const auto opc = preInstrs[insertIdx - 1].opc;
            if (opc == MOpcode::Br || opc == MOpcode::BCond || opc == MOpcode::Cbz ||
                opc == MOpcode::Cbnz || opc == MOpcode::Tbz || opc == MOpcode::Tbnz ||
                opc == MOpcode::JumpTable || opc == MOpcode::Ret)
                --insertIdx;
            else
                break;
        }

        for (auto &[phys, info] : regDefs) {
            if (info.movriCount == 0 || info.otherDefCount > 0)
                continue;
            if (!isCalleeSavedGPR(phys))
                continue;
            // If any loop body block uses this register without a local MovRI
            // definition, the hoisted value from the preheader might not reach
            // that block (e.g., mutually exclusive if/else branches where only
            // one side has the MovRI). Refuse to hoist in this case.
            if (info.useWithoutDefBlocks > 0)
                continue;

            auto git = globallyHoisted.find(phys);
            if (git != globallyHoisted.end() && git->second != info.immValue)
                continue;

            bool safeInAllBlocks = true;
            for (std::size_t bi : loop.body) {
                if (bi >= fn.blocks.size())
                    continue;
                const auto &instrs = fn.blocks[bi].instrs;
                for (std::size_t ii = 0; ii < instrs.size(); ++ii) {
                    const auto &mi = instrs[ii];

                    if (mi.opc == MOpcode::MovRI && mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                        mi.ops[0].reg.cls == RegClass::GPR && mi.ops[0].reg.idOrPhys == phys &&
                        mi.ops[1].kind == MOperand::Kind::Imm && mi.ops[1].imm == info.immValue)
                        break;

                    std::size_t startOp = isNonDefOpc(mi.opc) ? 0 : 1;
                    for (std::size_t oi = startOp; oi < mi.ops.size(); ++oi) {
                        if (mi.ops[oi].kind == MOperand::Kind::Reg && mi.ops[oi].reg.isPhys &&
                            mi.ops[oi].reg.cls == RegClass::GPR &&
                            mi.ops[oi].reg.idOrPhys == phys) {
                            safeInAllBlocks = false;
                            break;
                        }
                    }
                    if (!safeInAllBlocks)
                        break;
                }
                if (!safeInAllBlocks)
                    break;
            }
            if (!safeInAllBlocks)
                continue;

            globallyHoisted[phys] = info.immValue;

            MInstr hoistedMov{
                MOpcode::MovRI,
                {MOperand::regOp(static_cast<PhysReg>(phys)), MOperand::immOp(info.immValue)}};
            preInstrs.insert(preInstrs.begin() + static_cast<std::ptrdiff_t>(insertIdx),
                             hoistedMov);
            ++insertIdx;

            const MOperand hoistedReg = MOperand::regOp(static_cast<PhysReg>(phys));
            markCarriedExitReg(preBlock, hoistedReg);
            for (std::size_t blockIndex : loop.body) {
                if (blockIndex < fn.blocks.size())
                    markCarriedExitReg(fn.blocks[blockIndex], hoistedReg);
            }

            for (std::size_t bi : loop.body) {
                if (bi >= fn.blocks.size())
                    continue;

                // Don't remove MovRI from blocks that have predecessors outside
                // the loop body. Such blocks are reachable from paths where the
                // preheader's hoisted MovRI hasn't executed, so removing the
                // local MovRI would leave the register undefined on those paths.
                {
                    bool hasOutsidePred = false;
                    for (std::size_t p : cfg.preds(bi)) {
                        if (!loop.contains(p) && p != preIdx) {
                            hasOutsidePred = true;
                            break;
                        }
                    }
                    if (hasOutsidePred)
                        continue; // preserve MovRI in this block
                }

                auto &instrs = fn.blocks[bi].instrs;
                auto beforeSize = instrs.size();
                instrs.erase(std::remove_if(instrs.begin(),
                                            instrs.end(),
                                            /// Select redundant in-loop materializations
                                            /// of the value inserted in the preheader.
                                            [phys, &info](const MInstr &mi) {
                                                return mi.opc == MOpcode::MovRI &&
                                                       mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                                                       mi.ops[0].reg.cls == RegClass::GPR &&
                                                       mi.ops[0].reg.idOrPhys == phys &&
                                                       mi.ops[1].kind == MOperand::Kind::Imm &&
                                                       mi.ops[1].imm == info.immValue;
                                            }),
                             instrs.end());
                (void)beforeSize;
            }

            // Re-validate insertIdx after erase (defensive: if preIdx were
            // somehow in the loop body, the erase could shrink preInstrs).
            if (insertIdx > preInstrs.size())
                insertIdx = preInstrs.size();

            ++hoisted;
        }
    }

    return hoisted;
}

/// @copydoc eliminateLoopPhiSpills
std::size_t eliminateLoopPhiSpills(MFunction &fn, const TargetInfo &targetInfo) {
    if (fn.blocks.size() < 2)
        return 0;

    // Build block-name -> block-index map.
    std::unordered_map<std::string, std::size_t> nameToIdx;
    for (std::size_t i = 0; i < fn.blocks.size(); ++i)
        nameToIdx[fn.blocks[i].name] = i;

    /// Test whether an opcode ends a schedulable/control-flow region.
    /// @param opc Opcode to classify.
    /// @return `true` for recognized branch, jump-table, and return forms.
    auto isTerminator = [](MOpcode opc) -> bool {
        return opc == MOpcode::Br || opc == MOpcode::BCond || opc == MOpcode::Cbz ||
               opc == MOpcode::Cbnz || opc == MOpcode::Tbz || opc == MOpcode::Tbnz ||
               opc == MOpcode::JumpTable || opc == MOpcode::Ret;
    };

    // Dominators and natural loops come from the shared CFG snapshot so
    // layout-created backward branches to earlier join blocks are not
    // mistaken for loop back-edges. The snapshot is taken before the single
    // block split this pass may perform (it returns right after).
    const MirCfg cfg(fn);

    /// Test whether a call instruction clobbers a physical register under the
    /// AArch64 ABI sets modeled by this post-allocation rewrite.
    ///
    /// @param mi Candidate direct or indirect call.
    /// @param reg Physical register whose survival is queried.
    /// @return `true` when @p mi is a call and @p reg is caller-saved.
    // Call clobbers come from the target's caller-saved sets via the shared
    // effects model rather than a hard-coded AAPCS64 register range.
    const PhysRegSet callClobbers = callClobberSet(targetInfo);
    auto callClobbersReg = [&callClobbers](const MInstr &mi, const MOperand &reg) -> bool {
        if ((mi.opc != MOpcode::Bl && mi.opc != MOpcode::Blr) || !isPhysReg(reg))
            return false;
        return callClobbers.contains(static_cast<PhysReg>(reg.reg.idOrPhys));
    };

    // Find back-edges: a direct or conditional branch in block i whose target
    // block j <= i dominates i. Only explicit branch instructions qualify:
    // the edge-move rewrite below inserts the copies before the latch's
    // terminator suffix, which is where such a branch lives.
    /// @brief Dominance-proven backward branch edge.
    struct PhiBackEdge {
        /// Source block containing the backward branch.
        std::size_t latchIdx;

        /// Dominating target block at the loop header.
        std::size_t headerIdx;
    };

    std::vector<PhiBackEdge> backEdges;

    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        for (const auto &mi : fn.blocks[i].instrs) {
            std::string target = getBranchTarget(mi);
            if (target.empty())
                continue;
            auto it = nameToIdx.find(target);
            if (it != nameToIdx.end() && it->second <= i && cfg.dominates(it->second, i))
                backEdges.push_back({i, it->second});
        }
    }

    if (backEdges.empty())
        return 0;

    /// Test whether a candidate loop contains a call or an invalid block index.
    /// @param loopBlocks Natural-loop block indices to inspect.
    /// @return `true` when register edge moves cannot be proven call-safe.
    auto loopContainsCall = [&fn](const std::vector<std::size_t> &loopBlocks) {
        for (std::size_t blockIdx : loopBlocks) {
            if (blockIdx >= fn.blocks.size())
                return true;
            for (const auto &instr : fn.blocks[blockIdx].instrs) {
                if (instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr)
                    return true;
            }
        }
        return false;
    };

    /// @brief One scalar component of a header phi-slot load.
    struct PhiLoad {
        /// Index of the scalar or pair load instruction in its block.
        std::size_t instrIdx{0};

        /// Frame-pointer-relative byte offset of the loaded component.
        int64_t fpOffset{0};

        /// Physical destination register receiving the phi value.
        MOperand dstReg{};
    };

    /// @brief One scalar component of a back-edge phi-slot store.
    struct PhiStore {
        /// Index of the scalar or pair store instruction in its block.
        std::size_t instrIdx{0};

        /// Frame-pointer-relative byte offset of the stored component.
        int64_t fpOffset{0};

        /// Physical source register carrying the next phi value.
        MOperand srcReg{};
    };

    /// Decompose a recognized scalar or paired FP-relative load into phi-load
    /// components associated with the original instruction index.
    ///
    /// @param mi Candidate load instruction.
    /// @param instrIdx Index of @p mi in its containing sequence.
    /// @param[in,out] out Vector receiving one or two scalar load components.
    /// @return `true` for a well-formed supported load; `false` otherwise.
    auto appendPhiLoads =
        [](const MInstr &mi, std::size_t instrIdx, std::vector<PhiLoad> &out) -> bool {
        /// Append one decomposed load component.
        auto pushLoad = [&](const MOperand &dst, int64_t offset) {
            out.push_back({instrIdx, offset, dst});
        };

        switch (mi.opc) {
            case MOpcode::LdrRegFpImm:
            case MOpcode::LdrFprFpImm:
                if (mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                    mi.ops[1].kind == MOperand::Kind::Imm) {
                    pushLoad(mi.ops[0], mi.ops[1].imm);
                    return true;
                }
                return false;

            case MOpcode::LdpRegFpImm:
            case MOpcode::LdpFprFpImm:
                if (mi.ops.size() >= 3 && isPhysReg(mi.ops[0]) && isPhysReg(mi.ops[1]) &&
                    mi.ops[2].kind == MOperand::Kind::Imm) {
                    pushLoad(mi.ops[0], mi.ops[2].imm);
                    pushLoad(mi.ops[1], mi.ops[2].imm + 8);
                    return true;
                }
                return false;

            default:
                return false;
        }
    };

    /// Decompose a recognized scalar, pseudo, or paired FP-relative store,
    /// retaining only components whose offsets are in @p phiOffsets.
    ///
    /// @param mi Candidate store instruction.
    /// @param instrIdx Index of @p mi in its containing sequence.
    /// @param[in,out] out Vector receiving matching scalar store components.
    /// @param phiOffsets Header phi offsets eligible for matching.
    /// @return `true` for a well-formed supported store even when none of its
    ///         components matches; `false` otherwise.
    auto appendPhiStores = [](const MInstr &mi,
                              std::size_t instrIdx,
                              std::vector<PhiStore> &out,
                              const std::unordered_set<int64_t> &phiOffsets) -> bool {
        /// Append one decomposed store component when its offset is relevant.
        auto pushStore = [&](const MOperand &src, int64_t offset) {
            if (phiOffsets.count(offset))
                out.push_back({instrIdx, offset, src});
        };

        switch (mi.opc) {
            case MOpcode::StrRegFpImm:
            case MOpcode::StrFprFpImm:
            case MOpcode::PhiStoreGPR:
            case MOpcode::PhiStoreFPR:
                if (mi.ops.size() >= 2 && isPhysReg(mi.ops[0]) &&
                    mi.ops[1].kind == MOperand::Kind::Imm) {
                    pushStore(mi.ops[0], mi.ops[1].imm);
                    return true;
                }
                return false;

            case MOpcode::StpRegFpImm:
            case MOpcode::StpFprFpImm:
                if (mi.ops.size() >= 3 && isPhysReg(mi.ops[0]) && isPhysReg(mi.ops[1]) &&
                    mi.ops[2].kind == MOperand::Kind::Imm) {
                    pushStore(mi.ops[0], mi.ops[2].imm);
                    pushStore(mi.ops[1], mi.ops[2].imm + 8);
                    return true;
                }
                return false;

            default:
                return false;
        }
    };

    /// @brief Complete memory-to-register rewrite plan for one back edge.
    struct EdgePlan {
        /// Original store instruction indices erased as a unit.
        std::unordered_set<std::size_t> storeIndicesToRemove;

        /// Acyclic physical copies in safe sequential emission order.
        std::vector<EdgeMove> orderedMoves;

        /// Number of scalar spill/reload pairs represented by the plan.
        std::size_t eliminatedCount{0};
    };

    std::size_t eliminated = 0;

    // Process each back-edge. We process at most one per pass to avoid
    // invalidating indices after block insertion.
    for (const auto &edge : backEdges) {
        const NaturalLoop loop = cfg.naturalLoop(BackEdge{edge.latchIdx, edge.headerIdx});
        // The edge-move rewrite extends physical-register phi values across the
        // hot loop body. Keep it to call-free loops until the pass has a full
        // liveness proof for every rewritten register through complex bodies.
        if (loopContainsCall(loop.blocks))
            continue;

        auto &header = fn.blocks[edge.headerIdx];
        auto &latch = fn.blocks[edge.latchIdx];

        // Step 1: Identify phi loads at the start of the header.
        std::vector<PhiLoad> phiLoads;

        for (std::size_t i = 0; i < header.instrs.size(); ++i) {
            if (!appendPhiLoads(header.instrs[i], i, phiLoads))
                break;
        }

        // Require at least 2 consecutive phi loads. Single-variable loops
        // often use register movs for phi transfer (from single-predecessor
        // optimization), making header splitting unsafe.
        if (phiLoads.size() < 2)
            continue;

        // Step 2: Find matching phi stores in the latch block.
        // These are FP-relative stores that write to the same offsets
        // as the header's phi loads. They may not be strictly at the end
        // (other instructions like cmp can be interspersed).
        // Collect phi load offsets for matching.
        std::unordered_set<int64_t> phiLoadOffsets;
        for (const auto &pl : phiLoads)
            phiLoadOffsets.insert(pl.fpOffset);

        // Scan the entire latch block for stores to phi slot offsets.
        std::vector<PhiStore> phiStores;
        for (std::size_t i = 0; i < latch.instrs.size(); ++i) {
            (void)appendPhiStores(latch.instrs[i], i, phiStores, phiLoadOffsets);
        }

        if (phiStores.empty())
            continue;

        // Step 3: Match phi loads with phi stores by FP offset.
        struct PhiPair {
            PhiLoad load;
            PhiStore store;
        };

        std::vector<PhiPair> pairs;

        for (const auto &load : phiLoads) {
            for (const auto &store : phiStores) {
                if (load.fpOffset == store.fpOffset) {
                    pairs.push_back({load, store});
                    break;
                }
            }
        }

        if (pairs.empty())
            continue;

        // Safety: require a 1:1 match between phi loads and ALL phi-like stores.
        if (pairs.size() != phiLoads.size())
            continue;

        // Count ALL StrRegFpImm stores in the block (not just those matching
        // phi load offsets). If there are stores to FP offsets that DON'T have
        // a matching phi load, some loop-carried values are transferred via
        // different mechanisms (register movs, etc.) and splitting the header
        // would break those transfers.
        {
            std::unordered_set<int64_t> matchedOffsets;
            for (const auto &p : pairs)
                matchedOffsets.insert(p.load.fpOffset);

            // Collect ALL unique FP store offsets in the latch block.
            std::unordered_set<int64_t> allStoreOffsets;
            std::vector<PhiStore> allStores;
            for (std::size_t i = 0; i < latch.instrs.size(); ++i) {
                appendPhiStores(latch.instrs[i], i, allStores, phiLoadOffsets);
            }
            for (const auto &store : allStores)
                allStoreOffsets.insert(store.fpOffset);

            // Check that EVERY phi-slot store is matched by a phi load.
            // Offsets that are stored but not loaded indicate loop-carried
            // values transferred via register moves, not stack loads.
            bool hasUnmatchedStores = false;
            for (int64_t off : allStoreOffsets) {
                if (!matchedOffsets.count(off)) {
                    hasUnmatchedStores = true;
                    break;
                }
            }
            if (hasUnmatchedStores)
                continue;
        }

        // Step 4: Verify the phi slot offsets are NOT loaded anywhere else in the
        // function besides the header. If the exit block loads from the same offset,
        // we must keep the store.
        std::unordered_set<int64_t> phiOffsets;
        for (const auto &p : pairs)
            phiOffsets.insert(p.load.fpOffset);

        bool safeToEliminate = true;
        for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (bi == edge.headerIdx)
                continue; // Skip header (its loads are the ones we're eliminating).
            for (const auto &mi : fn.blocks[bi].instrs) {
                std::vector<PhiLoad> loads;
                appendPhiLoads(mi, 0, loads);
                for (const auto &load : loads) {
                    if (phiOffsets.count(load.fpOffset)) {
                        safeToEliminate = false;
                        break;
                    }
                }
                if (!safeToEliminate)
                    break;
            }
            if (!safeToEliminate)
                break;
        }

        if (!safeToEliminate)
            continue;

        std::size_t firstNonPhiIdx = phiLoads.back().instrIdx + 1;

        /// Prove that a stored source register retains its value until the edge.
        ///
        /// @param instrs Instruction sequence containing the candidate store.
        /// @param store Decomposed store and its original instruction index.
        /// @return `true` when no later definition or call clobbers the source.
        auto sourceRegSurvivesToEdge = [&](const std::vector<MInstr> &instrs,
                                           const PhiStore &store) -> bool {
            if (!isPhysReg(store.srcReg))
                return false;
            if (store.instrIdx >= instrs.size())
                return false;
            for (std::size_t i = store.instrIdx + 1; i < instrs.size(); ++i) {
                const auto &instr = instrs[i];
                if (definesReg(instr, store.srcReg) || callClobbersReg(instr, store.srcReg))
                    return false;
            }
            return true;
        };

        /// Match each header load to the last suitable edge store and construct
        /// an acyclic parallel-copy lowering.
        ///
        /// @param edgeInstrs Sequence containing the stores on this back edge.
        /// @param stores Decomposed candidate stores from @p edgeInstrs.
        /// @param[out] plan Store removals, ordered copies, and elimination count.
        /// @return `true` when every load has a surviving source and the required
        ///         register moves contain no cycle.
        auto planEdgeMoves = [&](const std::vector<MInstr> &edgeInstrs,
                                 const std::vector<PhiStore> &stores,
                                 EdgePlan &plan) {
            plan.storeIndicesToRemove.clear();
            plan.orderedMoves.clear();
            plan.eliminatedCount = 0;

            std::vector<EdgeMove> moves;
            for (const auto &load : phiLoads) {
                bool matched = false;
                for (auto storeIt = stores.rbegin(); storeIt != stores.rend(); ++storeIt) {
                    const auto &store = *storeIt;
                    if (load.fpOffset != store.fpOffset)
                        continue;
                    if (!sourceRegSurvivesToEdge(edgeInstrs, store))
                        return false;
                    matched = true;
                    plan.storeIndicesToRemove.insert(store.instrIdx);
                    ++plan.eliminatedCount;
                    if (store.srcReg.reg.idOrPhys != load.dstReg.reg.idOrPhys) {
                        moves.push_back(EdgeMove{
                            store.instrIdx, store.srcReg, load.dstReg, load.dstReg.reg.cls});
                    }
                    break;
                }
                if (!matched)
                    return false;
            }

            if (!orderEdgeMoves(moves, plan.orderedMoves))
                return false;
            return true;
        };

        EdgePlan edgePlan;
        std::vector<MInstr> selfBodyInstrs;
        if (edge.latchIdx == edge.headerIdx) {
            selfBodyInstrs.assign(header.instrs.begin() +
                                      static_cast<std::ptrdiff_t>(firstNonPhiIdx),
                                  header.instrs.end());
            std::vector<PhiStore> bodyPhiStores;
            for (std::size_t i = 0; i < selfBodyInstrs.size(); ++i) {
                (void)appendPhiStores(selfBodyInstrs[i], i, bodyPhiStores, phiLoadOffsets);
            }
            if (!planEdgeMoves(selfBodyInstrs, bodyPhiStores, edgePlan))
                continue;
        } else if (!planEdgeMoves(latch.instrs, phiStores, edgePlan)) {
            continue;
        }

        // Step 5: Split the header block. Create a new body block that contains
        // everything after the phi loads. The back-edge will target this body block.
        std::string bodyName = header.name + "_body";
        std::string headerName = header.name;
        if (nameToIdx.find(bodyName) != nameToIdx.end())
            continue;

        // Build the body block with instructions after the phi loads.
        MBasicBlock bodyBlock;
        bodyBlock.name = bodyName;
        // The new body owns the original header's outgoing edges, so it also
        // inherits allocator-published live-out metadata for those edges.
        bodyBlock.carriedExitRegs = header.carriedExitRegs;
        if (edge.latchIdx == edge.headerIdx) {
            bodyBlock.instrs = std::move(selfBodyInstrs);
        } else {
            bodyBlock.instrs.assign(header.instrs.begin() +
                                        static_cast<std::ptrdiff_t>(firstNonPhiIdx),
                                    header.instrs.end());
        }

        // Trim the header to just the phi loads + unconditional branch to body.
        header.instrs.resize(firstNonPhiIdx);
        header.instrs.push_back(MInstr{MOpcode::Br, {MOperand::labelOp(bodyName)}});
        for (const auto &load : phiLoads)
            markCarriedExitReg(header, load.dstReg);

        /// Remove complete store instructions selected by original index.
        /// @param[in,out] bb Edge block whose instruction vector is compacted.
        /// @param indices Store instruction indices to erase.
        auto removeStores = [](MBasicBlock &bb, const std::unordered_set<std::size_t> &indices) {
            if (indices.empty())
                return;
            std::vector<MInstr> newInstrs;
            newInstrs.reserve(bb.instrs.size());
            for (std::size_t i = 0; i < bb.instrs.size(); ++i) {
                if (indices.count(i))
                    continue;
                newInstrs.push_back(std::move(bb.instrs[i]));
            }
            bb.instrs = std::move(newInstrs);
        };

        /// Apply one edge plan by deleting stores and inserting ordered register
        /// copies immediately before the block's terminator suffix.
        ///
        /// @param[in,out] bb Back-edge block to rewrite.
        /// @param plan Previously validated store-removal and copy sequence.
        auto applyEdgeMoves = [&](MBasicBlock &bb, const EdgePlan &plan) {
            removeStores(bb, plan.storeIndicesToRemove);
            if (plan.orderedMoves.empty())
                return;
            std::vector<MInstr> movs;
            movs.reserve(plan.orderedMoves.size());
            for (const auto &move : plan.orderedMoves) {
                const MOpcode movOpc = move.cls == RegClass::FPR ? MOpcode::FMovRR : MOpcode::MovRR;
                movs.push_back(MInstr{movOpc, {move.dstReg, move.srcReg}});
            }
            std::size_t insertPos = bb.instrs.size();
            while (insertPos > 0 && isTerminator(bb.instrs[insertPos - 1].opc))
                --insertPos;
            bb.instrs.insert(bb.instrs.begin() + static_cast<std::ptrdiff_t>(insertPos),
                             movs.begin(),
                             movs.end());
        };

        if (edge.latchIdx == edge.headerIdx) {
            // Self-loops carry the phi stores in the split body block itself.
            applyEdgeMoves(bodyBlock, edgePlan);
            for (const auto &load : phiLoads)
                markCarriedExitReg(bodyBlock, load.dstReg);
        } else {
            // Multi-block loops carry phi values in the latch block. If we redirect the
            // latch to the hot body, we must translate the phi-slot stores into register
            // edge moves there instead of relying on the cold reload header.
            applyEdgeMoves(latch, edgePlan);
            for (const auto &load : phiLoads)
                markCarriedExitReg(latch, load.dstReg);
        }
        eliminated += edgePlan.eliminatedCount;

        /// Retarget every reference to the old header label in one back-edge block.
        /// @param[in,out] bb Block whose label operands may be rewritten.
        auto redirectBackedgeTarget = [&](MBasicBlock &bb) {
            for (auto &mi : bb.instrs) {
                for (auto &op : mi.ops) {
                    if (op.kind == MOperand::Kind::Label && op.label == headerName)
                        op.label = bodyName;
                }
            }
        };

        // Redirect back-edge branch targets from the split body and the original
        // latch block. Multi-block loops branch back from the latch; self-loops
        // branch back from the new body block itself.
        redirectBackedgeTarget(bodyBlock);
        if (edge.latchIdx != edge.headerIdx)
            redirectBackedgeTarget(latch);

        // Step 7: Insert body block immediately after header.
        fn.blocks.insert(fn.blocks.begin() + static_cast<std::ptrdiff_t>(edge.headerIdx) + 1,
                         std::move(bodyBlock));

        // Only process one back-edge per pass (indices are invalidated).
        break;
    }

    return eliminated;
}

} // namespace zanna::codegen::aarch64::peephole
