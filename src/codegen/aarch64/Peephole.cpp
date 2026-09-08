//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/Peephole.cpp
// Purpose: Driver for conservative peephole optimizations over AArch64 MIR.
//          Orchestrates the modular sub-passes (layout, loop hoisting,
//          per-block rewrites, CFG-aware DCE, dead spill stores, branches).
// Key invariants:
//   - All rewrites preserve instruction semantics and ordering.
//   - Must be called after register allocation (physical registers required).
//   - Cross-block passes read edges only from MirCfg, never from a private
//     terminator scan, so they agree with the allocator and the verifier.
//   - Block-local folds that reach the block end consult blockExitLive().
// Ownership/Lifetime:
//   - Mutates MFunction in place; borrows fn only during the call.
// Links: src/codegen/aarch64/Peephole.hpp,
//        src/codegen/aarch64/peephole/ (sub-pass implementations)
//
//===----------------------------------------------------------------------===//

#include "Peephole.hpp"

#include "MirCfg.hpp"
#include "PhysLiveness.hpp"
#include "TargetAArch64.hpp"
#include "peephole/BranchOpt.hpp"
#include "peephole/CopyPropDCE.hpp"
#include "peephole/IdentityElim.hpp"
#include "peephole/LoopOpt.hpp"
#include "peephole/MemoryOpt.hpp"
#include "peephole/PeepholeCommon.hpp"
#include "peephole/StrengthReduce.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * @file
 * @brief Implements the AArch64 post-allocation peephole pipeline.
 *
 * Every stage that drops or redirects a definition reaching a block end
 * solves physical liveness first (PhysLiveness.hpp) and reads
 * blockExitLive(); cross-block rewrites publish no metadata of their own.
 */

namespace zanna::codegen::aarch64 {

// Import sub-pass functions into local scope for concise call sites.
namespace ph = peephole;

namespace {

/**
 * @brief Exit-live seed of the block-local DCE on the no-target path.
 *
 * Direct unit tests run the driver without a target and without the
 * CFG-aware DCE; their expectations were written against the block-local
 * pass's conservative seed (argument registers and callee-saved GPRs live at
 * every exit). Keep that seed there, on top of the solved exit-live set.
 *
 * @param exitLive Solved exit-live set of the block.
 * @return @p exitLive plus X0-X7, V0-V7, and X19-X28.
 */
static PhysRegSet conservativeDceSeed(const PhysRegSet &exitLive) {
    PhysRegSet seed = exitLive;
    for (unsigned r = static_cast<unsigned>(PhysReg::X0); r <= static_cast<unsigned>(PhysReg::X7);
         ++r)
        seed.add(static_cast<PhysReg>(r));
    for (unsigned r = static_cast<unsigned>(PhysReg::V0); r <= static_cast<unsigned>(PhysReg::V7);
         ++r)
        seed.add(static_cast<PhysReg>(r));
    for (unsigned r = static_cast<unsigned>(PhysReg::X19); r <= static_cast<unsigned>(PhysReg::X28);
         ++r)
        seed.add(static_cast<PhysReg>(r));
    return seed;
}

/**
 * @brief Runs the ordered local rewrite sequence over every non-empty block.
 *
 * The sequence covers division/remainder strength reduction, constant-aware
 * rewrites, copy propagation, branch/arithmetic/load-store fusion, local
 * memory forwarding, move folding, identity removal, optional local DCE, and
 * dead flag-setter elimination.
 *
 * @param[in,out] fn Physical-register MIR function to rewrite.
 * @param[in,out] stats Transformation counters shared across sub-passes.
 * @param target Non-null when CFG-aware DCE will run separately; `nullptr`
 *        enables the legacy per-block dead-instruction pass here.
 */
static void runPerBlockRewrites(MFunction &fn, PeepholeStats &stats, const TargetInfo *target) {
    // Folds that redirect or drop a definition reaching the block end need
    // the ABI (call/return effects) and the block's exit-live set. Direct
    // unit tests may run without a target; AAPCS64 (the Darwin singleton) is
    // the common denominator.
    const TargetInfo &effectiveTarget = target != nullptr ? *target : darwinTarget();

    // One liveness solve for the stage: the rewrites below never add an
    // upward-exposed read to a block, so a block's exit-live set computed
    // here stays a superset of the truth while earlier blocks are rewritten.
    const PhysLiveness liveness = computePhysLiveness(fn, effectiveTarget);

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        auto &instrs = block.instrs;
        if (instrs.empty())
            continue;

        const PhysRegSet exitLive = blockExitLive(fn, bi, effectiveTarget, liveness);

        // Pass 0.9: Division/remainder strength reduction (multi-instruction patterns).
        // Must run BEFORE Pass 1's single-instruction strength reduction, because
        // Pass 1 converts UDIV->LSR which would break the UDIV+MSUB remainder
        // pattern. Remainder fusion must see the original UDIV/SDIV.
        {
            bool changed = true;
            while (changed) {
                changed = false;
                ph::RegConstMap divConsts;
                for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
                    if (ph::tryRemainderFusion(instrs, i, divConsts, stats, &exitLive)) {
                        changed = true;
                        break;
                    }
                    ph::updateKnownConsts(instrs[i], divConsts);
                }
            }

            changed = true;
            while (changed) {
                changed = false;
                ph::RegConstMap divConsts;
                for (std::size_t i = 0; i < instrs.size(); ++i) {
                    bool localChange = false;
                    if (instrs[i].opc == MOpcode::UDivRRR)
                        localChange =
                            ph::tryUDivStrengthReduction(instrs, i, divConsts, stats, &exitLive);
                    else if (instrs[i].opc == MOpcode::SDivRRR)
                        localChange =
                            ph::trySDivStrengthReduction(instrs, i, divConsts, stats, &exitLive);

                    if (localChange) {
                        changed = true;
                        break;
                    }
                    ph::updateKnownConsts(instrs[i], divConsts);
                }
            }
        }

        // Pass 1: Single-instruction constant-aware rewrites.
        ph::RegConstMap knownConsts;
        for (auto &instr : instrs) {
            ph::updateKnownConsts(instr, knownConsts);
            if (ph::tryCmpZeroToTst(instr, stats))
                continue;
            if (ph::tryArithmeticIdentity(instr, stats))
                continue;
            (void)ph::tryStrengthReduction(instr, knownConsts, stats);
            (void)ph::tryDivStrengthReduction(instr, knownConsts, stats);
            (void)ph::tryImmediateFolding(instr, knownConsts, stats);
        }

        // Pass 1.5: Copy propagation.
        ph::propagateCopies(instrs, stats);

        // Pass 1.6 / 1.65 / 1.7 / 1.8: instruction-level fusions.
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryCbzCbnzFusion(instrs, i, stats) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryTbzTbnzFusion(instrs, i, stats, &exitLive) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::tryCsetBranchFusion(instrs, i, stats, &exitLive) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryMaddFusion(instrs, i, stats, effectiveTarget, exitLive) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryLdpStpMerge(instrs, i, stats) && i > 0)
                --i;
        }

        // Pass 1.85 / 1.9 / 1.97: local store/load shuffling.
        ph::eliminateDeadFpStores(instrs, stats);
        ph::forwardStoreLoads(instrs, stats);
        ph::foldComputeIntoTarget(instrs, stats, effectiveTarget, exitLive);

        // Pass 2: Fold consecutive moves.
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (!ph::tryFoldImmThenMove(instrs, i, stats, &exitLive))
                (void)ph::tryFoldConsecutiveMoves(instrs, i, stats, &exitLive);
        }

        // Pass 3+4: Mark and remove identity moves.
        std::vector<bool> toRemove(instrs.size(), false);
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::isIdentityMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            } else if (ph::isIdentityFMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityFMovesRemoved;
            }
        }
        /// @brief Tests whether an instruction has been marked for removal.
        /// @param v Removal marker.
        /// @return The marker value.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool v) { return v; }))
            ph::removeMarkedInstructions(instrs, toRemove);

        // Pass 4.5: Local DCE. The modular pipeline runs the CFG-aware variant
        // post-block; direct unit tests (no target) keep the legacy per-block path
        // with its conservative seed on top of the solved exit-live set.
        if (target == nullptr) {
            const PhysRegSet seed = conservativeDceSeed(exitLive);
            ph::removeDeadInstructions(instrs, stats, &seed);
        }

        // Pass 4.6: Dead flag-setter elimination AFTER DCE so dead readers of
        // flags are gone before we judge a flag-setter unused.
        ph::removeDeadFlagSetters(instrs, stats);
    }
}

/**
 * @brief Applies final branch inversion and branch-to-next cleanup.
 *
 * A conditional branch to the next layout block followed by an unconditional
 * branch is inverted and collapsed when the condition has a known inverse.
 * Remaining unconditional branches to the next block are removed.
 *
 * @param[in,out] fn Function whose block tails are simplified.
 * @param[in,out] stats Branch inversion/removal counters to update.
 */
static void runBranchInversionAndCleanup(MFunction &fn, PeepholeStats &stats) {
    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        const auto &nextBlock = fn.blocks[bi + 1];

        if (block.instrs.empty())
            continue;

        // Branch inversion: b.cond .Ltarget; b .Lfallthrough
        // when .Ltarget == next block -> b.!cond .Lfallthrough (remove b .Lfallthrough)
        if (block.instrs.size() >= 2) {
            auto &secondLast = block.instrs[block.instrs.size() - 2];
            auto &last = block.instrs[block.instrs.size() - 1];

            if (secondLast.opc == MOpcode::BCond && secondLast.ops.size() == 2 &&
                secondLast.ops[0].kind == MOperand::Kind::Cond &&
                secondLast.ops[1].kind == MOperand::Kind::Label && last.opc == MOpcode::Br &&
                last.ops.size() == 1 && last.ops[0].kind == MOperand::Kind::Label) {
                if (secondLast.ops[1].label == nextBlock.name) {
                    const char *inv = ph::invertCondition(secondLast.ops[0].cond);
                    if (inv) {
                        secondLast.ops[0] = MOperand::condOp(inv);
                        secondLast.ops[1] = last.ops[0];
                        block.instrs.pop_back();
                        ++stats.branchInversions;
                        continue;
                    }
                }
            }
        }

        // Remove branches to the immediately following block.
        if (ph::isBranchTo(block.instrs.back(), nextBlock.name)) {
            block.instrs.pop_back();
            ++stats.branchesToNextRemoved;
        }
    }
}

} // namespace

/**
 * @brief Runs the full ordered peephole pipeline on allocated AArch64 MIR.
 *
 * @param[in,out] fn Function to optimize in place.
 * @param target Optional target metadata enabling CFG-aware dead-code removal.
 * @return Aggregate counters populated by every invoked sub-pass.
 * @pre Register operands have been assigned physical registers.
 */
/// @brief Triage kill switch for one peephole sub-stage (`ZANNA_NO_PH_<NAME>=1`).
/// @details Names: REORDER, LOOPHOIST, PERBLOCK, DCE_CFG, FPSTORES, BRANCH.
///          A bisection aid
///          against a program-level oracle; the stage-level switches live in
///          CodegenPipeline.cpp (backendStageDisabled). Never consulted at -O0.
static bool peepholeStageDisabled(const char *name) {
    std::string key = "ZANNA_NO_PH_";
    key += name;
    return std::getenv(key.c_str()) != nullptr;
}

PeepholeStats runPeephole(MFunction &fn, const TargetInfo *target) {
    PeepholeStats stats;

    // Pass 0: Reorder blocks for better code layout
    if (!peepholeStageDisabled("REORDER"))
        stats.blocksReordered = static_cast<int>(ph::reorderBlocks(fn));

    // Pass 0.5: Hoist loop-invariant MovRI out of loop bodies.
    // LoopOpt now rejects merge-like headers, non-preheader entries, and uses
    // that can observe the value before a dominating definition inside the loop.
    if (!peepholeStageDisabled("LOOPHOIST"))
        stats.loopConstsHoisted = static_cast<int>(ph::hoistLoopConstants(fn));

    // Passes 0.9 through 4.6: local per-block rewrites (division strength reduction,
    // constant-aware rewrites, fusions, identity removal, local DCE/flag DCE).
    if (!peepholeStageDisabled("PERBLOCK"))
        runPerBlockRewrites(fn, stats, target);

    if (target != nullptr && !peepholeStageDisabled("DCE_CFG")) {
        ph::removeDeadInstructionsCFG(fn, stats, *target);
        for (auto &block : fn.blocks)
            ph::removeDeadFlagSetters(block.instrs, stats);
    }

    // Pass 4.7: cross-block dead spill-store elimination.
    if (!peepholeStageDisabled("FPSTORES"))
        ph::eliminateDeadFpStoresCrossBlock(fn, stats);

    // Pass 5: Branch inversion and branch-to-next removal.
    if (!peepholeStageDisabled("BRANCH"))
        runBranchInversionAndCleanup(fn, stats);

    return stats;
}

/**
 * @brief Runs local cleanup and branch simplification after instruction scheduling.
 *
 * @param[in,out] fn Scheduled function to clean without cross-block reshaping.
 * @param target Optional target metadata; when null, local DCE is performed.
 * @return Counters for transformations applied by this reduced pass set.
 */
PeepholeStats runPostSchedulePeephole(MFunction &fn, const TargetInfo *target) {
    PeepholeStats stats;

    // The scheduler may have moved definitions; solve liveness on the
    // scheduled shape before any move fold or block-local DCE reads it.
    const TargetInfo &effectiveTarget = target != nullptr ? *target : darwinTarget();
    const PhysLiveness liveness = computePhysLiveness(fn, effectiveTarget);

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &instrs = fn.blocks[bi].instrs;
        if (instrs.empty())
            continue;

        const PhysRegSet exitLive = blockExitLive(fn, bi, effectiveTarget, liveness);

        ph::propagateCopies(instrs, stats);

        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (!ph::tryFoldImmThenMove(instrs, i, stats, &exitLive))
                (void)ph::tryFoldConsecutiveMoves(instrs, i, stats, &exitLive);
        }

        std::vector<bool> toRemove(instrs.size(), false);
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::isIdentityMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            } else if (ph::isIdentityFMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityFMovesRemoved;
            }
        }
        /// @brief Tests whether an instruction has been marked for removal.
        /// @param v Removal marker.
        /// @return The marker value.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool v) { return v; }))
            ph::removeMarkedInstructions(instrs, toRemove);

        if (target == nullptr) {
            const PhysRegSet seed = conservativeDceSeed(exitLive);
            ph::removeDeadInstructions(instrs, stats, &seed);
        }
        ph::removeDeadFlagSetters(instrs, stats);
    }

    runBranchInversionAndCleanup(fn, stats);

    return stats;
}

/**
 * @brief Prunes saved-register metadata to physical registers still referenced by MIR.
 *
 * @param[in,out] fn Function whose `savedGPRs` and `savedFPRs` vectors are filtered.
 * @post Each retained saved register occurs as a physical register operand.
 */
void pruneUnusedCalleeSaved(MFunction &fn) {
    // Build a set of all physical registers actually referenced in the MIR.
    std::unordered_set<uint16_t> usedRegs;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            for (const auto &op : mi.ops) {
                if (op.kind == MOperand::Kind::Reg && op.reg.isPhys)
                    usedRegs.insert(op.reg.idOrPhys);
            }
        }
    }

    // Prune savedGPRs: remove any callee-saved register not referenced.
    fn.savedGPRs.erase(std::remove_if(fn.savedGPRs.begin(),
                                      fn.savedGPRs.end(),
                                      /// @brief Tests whether a saved GPR is unused.
                                      /// @param r Physical register to inspect.
                                      /// @return `true` when no MIR operand references `r`.
                                      [&](PhysReg r) {
                                          return usedRegs.find(static_cast<uint16_t>(r)) ==
                                                 usedRegs.end();
                                      }),
                       fn.savedGPRs.end());

    // Prune savedFPRs: same logic.
    fn.savedFPRs.erase(std::remove_if(fn.savedFPRs.begin(),
                                      fn.savedFPRs.end(),
                                      /// @brief Tests whether a saved FPR is unused.
                                      /// @param r Physical register to inspect.
                                      /// @return `true` when no MIR operand references `r`.
                                      [&](PhysReg r) {
                                          return usedRegs.find(static_cast<uint16_t>(r)) ==
                                                 usedRegs.end();
                                      }),
                       fn.savedFPRs.end());
}

} // namespace zanna::codegen::aarch64
