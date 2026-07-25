//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Peephole.cpp
// Purpose: Driver for conservative peephole optimizations over Machine IR for
//          the x86-64 backend. Delegates to modular sub-passes under
//          peephole/ for each optimization category.
// Key invariants:
//   - Rewrites preserve instruction ordering and semantics.
//   - Must be called after register allocation when physical registers are known.
//   - Block rewrites iterate to a fixed point bounded by kMaxIterations.
// Ownership/Lifetime:
//   - Mutates MIR owned by the caller; no references to transient operands retained.
// Links: src/codegen/x86_64/Peephole.hpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp,
//        src/codegen/x86_64/peephole/ArithSimplify.hpp,
//        src/codegen/x86_64/peephole/BranchOpt.hpp,
//        src/codegen/x86_64/peephole/DCE.hpp,
//        src/codegen/x86_64/peephole/MemoryOpt.hpp,
//        src/codegen/x86_64/peephole/MovFolding.hpp
//
//===----------------------------------------------------------------------===//

#include "Peephole.hpp"

#include "peephole/ArithSimplify.hpp"
#include "peephole/BranchOpt.hpp"
#include "peephole/DCE.hpp"
#include "peephole/MemoryOpt.hpp"
#include "peephole/MovFolding.hpp"
#include "peephole/PeepholeCommon.hpp"

#include <algorithm>

/**
 * @file
 * @brief Implements the fixed-point driver for modular x86-64 peephole passes.
 *
 * Block-local rewriting tracks constants, simplifies arithmetic, folds moves,
 * forwards frame accesses, and removes dead operations. Function-level branch
 * cleanup reaches a bounded fixed point before and after one-shot layout and
 * cold-block placement.
 */

namespace zanna::codegen::x64 {

// Import sub-pass functions into local scope for concise call sites.
namespace ph = peephole;

/// Maximum number of rewrite iterations before giving up on convergence.
/// Typical functions converge in 1-2 iterations; the bound guards against
/// pathological cases where rewrites keep enabling each other.
static constexpr std::size_t kMaxIterations = 100;

/// @brief Determine if @p blockIndex can pass control to another block.
/// @details Scans backward from the block's terminator: RET or UD2 are
///          absorbing (no successor), JMP/JCC explicitly transfer, and a
///          fall-through case is detected when the block has a subsequent
///          sibling. Used by DCE to know whether instructions in this block
///          can be safely deleted (their effects need not be observable on
///          paths that abort here).
/// @param fn Function being inspected.
/// @param blockIndex Index of the block in question.
/// @return True when control may exit this block to another block.
static bool blockMayTransferControl(const MFunction &fn, std::size_t blockIndex) {
    if (blockIndex >= fn.blocks.size())
        return false;

    const auto &instrs = fn.blocks[blockIndex].instructions;
    for (auto it = instrs.rbegin(); it != instrs.rend(); ++it) {
        switch (it->opcode) {
            case MOpcode::RET:
            case MOpcode::UD2:
                return false;
            case MOpcode::JMP:
            case MOpcode::JCC:
                return true;
            default:
                break;
        }
    }
    return blockIndex + 1 < fn.blocks.size();
}

/// Run per-block rewrite passes (strength reduction, identity elimination,
/// move folding, DCE). Returns the number of transformations applied.
/// @param fn Function whose blocks are rewritten in place.
/// @param stats Cumulative transformation counters.
/// @param target ABI metadata used by target-sensitive DCE.
/// @return Number of new transformations recorded during this invocation.
static std::size_t runBlockRewrites(MFunction &fn,
                                    ph::PeepholeStats &stats,
                                    const TargetInfo &target) {
    std::size_t before = stats.total();

    for (std::size_t blockIndex = 0; blockIndex < fn.blocks.size(); ++blockIndex) {
        auto &block = fn.blocks[blockIndex];
        auto &instrs = block.instructions;
        if (instrs.empty())
            continue;

        // Pass 1: Build register constant map and apply rewrites
        ph::RegConstMap knownConsts;
        std::vector<bool> toRemove(instrs.size(), false);

        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];

            // Track constants loaded via MOVri
            ph::updateKnownConsts(instr, knownConsts);

            switch (instr.opcode) {
                case MOpcode::MOVri: {
                    if (instr.operands.size() != 2)
                        break;

                    if (!ph::isGprReg(instr.operands[0]) || !ph::isZeroImm(instr.operands[1]))
                        break;

                    // XOR clobbers EFLAGS; skip rewrite when a subsequent
                    // instruction reads flags before they are overwritten.
                    if (ph::nextInstrReadsFlags(instrs, i))
                        break;

                    ph::rewriteToXor(instr, instr.operands[0]);
                    ++stats.movZeroToXor;
                    break;
                }
                case MOpcode::CMPri: {
                    if (instr.operands.size() != 2)
                        break;

                    if (!ph::isGprReg(instr.operands[0]) || !ph::isZeroImm(instr.operands[1]))
                        break;

                    ph::rewriteToTest(instr, instr.operands[0]);
                    ++stats.cmpZeroToTest;
                    break;
                }
                default:
                    break;
            }

            // Try arithmetic identity elimination (add #0, shift #0)
            if (ph::tryArithmeticIdentity(instrs, i, stats)) {
                toRemove[i] = true;
                continue;
            }

            // Try strength reduction (mul power-of-2 -> shift)
            (void)ph::tryStrengthReduction(instrs, i, knownConsts, stats);
        }

        // Pass 2: Try to fold consecutive moves.
        (void)ph::foldConsecutiveMoves(instrs, stats);

        // Pass 3: Mark identity moves for removal
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::isIdentityMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            } else if (ph::isIdentityMovSDRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            }
        }

        // Pass 4: Remove marked instructions
        /// Detect whether the removal bitmap contains any marked instruction.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool v) { return v; })) {
            ph::removeMarkedInstructions(instrs, toRemove);
        }

        // Pass 5: Dead code elimination
        ph::forwardFrameStoreLoads(instrs, stats);
        ph::eliminateDeadFrameStores(instrs, stats);
        ph::runBlockDCE(instrs, stats, target, blockMayTransferControl(fn, blockIndex));
    }

    return stats.total() - before;
}

/// @copydoc runPeepholes
std::size_t runPeepholes(MFunction &fn, const TargetInfo &target) {
    ph::PeepholeStats stats;

    // Iterate block rewrites to a fixed point. One rewrite can expose
    // further opportunities (e.g., strength reduction → identity move →
    // DCE), so iterate until no new transformations are found.
    for (std::size_t iter = 0; iter < kMaxIterations; ++iter) {
        if (runBlockRewrites(fn, stats, target) == 0)
            break;
    }

    // Branch cleanup can expose itself; layout can also expose final fallthrough
    // jumps. Keep layout single-shot so cold-block movement and trace layout
    // cannot churn on large functions.
    for (std::size_t iter = 0; iter < kMaxIterations; ++iter) {
        const std::size_t before = stats.total();
        ph::eliminateBranchChains(fn, stats);
        ph::invertConditionalBranches(fn, stats);
        ph::removeFallthroughJumps(fn, stats);
        if (stats.total() == before)
            break;
    }

    ph::traceBlockLayout(fn, stats);
    ph::moveColdBlocks(fn, stats);
    ph::eliminateBranchChains(fn, stats);
    ph::invertConditionalBranches(fn, stats);
    ph::removeFallthroughJumps(fn, stats);

    return stats.total();
}

} // namespace zanna::codegen::x64
