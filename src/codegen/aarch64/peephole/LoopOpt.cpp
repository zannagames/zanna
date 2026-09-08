//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/LoopOpt.cpp
// Purpose: Loop-invariant constant hoisting for the AArch64 peephole optimizer.
//
// Key invariants:
//   - Only hoists MovRI to callee-saved registers (x19-x28).
//   - The register must be defined only by MovRI with the same immediate value
//     throughout the loop body.
//   - Edges, dominators, and natural loops come from the shared MirCfg; the
//     pass keeps no private CFG builder.
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
               opc == MOpcode::AddSpImm || opc == MOpcode::ParallelCopy;
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

            // The body's remaining reads of the register keep it live from
            // the preheader through every loop block; the per-block stage
            // that follows solves liveness on this shape.
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

} // namespace zanna::codegen::aarch64::peephole
