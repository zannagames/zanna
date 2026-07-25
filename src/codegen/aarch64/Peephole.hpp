//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/Peephole.hpp
// Purpose: Declare peephole optimizations over AArch64 Machine IR.
//
// Key invariants:
// - Rewrites preserve observable behavior while instructions and blocks may
//   be removed, reordered, fused, or replaced.
// - Must be called after register allocation when physical registers are known.
//
// Ownership/Lifetime:
// - Operates on mutable Machine IR owned by the caller.
// - No dynamic resources are allocated beyond temporary vectors.
//
// Links: src/codegen/aarch64/Peephole.cpp,
//        src/codegen/aarch64/peephole/,
//        docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"

/**
 * @file
 * @brief Declares post-register-allocation AArch64 MIR cleanup pipelines.
 *
 * The full pipeline combines local algebraic rewrites with CFG-aware
 * forwarding, loop cleanup, dead-code removal, and branch layout work. A
 * smaller post-scheduling entry point restricts itself to inexpensive cleanup
 * that is safe after instruction order has been chosen.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Counts transformations performed by one peephole pipeline invocation.
 *
 * Every field starts at zero and is incremented by the pass that owns the
 * corresponding transformation category.
 */
struct PeepholeStats {
    int identityMovesRemoved{0};    ///< Number of `mov r, r` instructions removed.
    int identityFMovesRemoved{0};   ///< Number of `fmov d, d` instructions removed.
    int consecutiveMovsFolded{0};   ///< Number of consecutive moves folded.
    int deadInstructionsRemoved{0}; ///< Number of dead instructions removed.
    int cmpZeroToTst{0};            ///< Number of `cmp r, #0` → `tst r, r` transforms.
    int arithmeticIdentities{0};    ///< Number of identity arithmetic ops removed.
    int strengthReductions{0};      ///< Number of mul→shift strength reductions.
    int branchesToNextRemoved{0};   ///< Number of branches to next block removed.
    int blocksReordered{0};         ///< Number of blocks reordered for layout.
    int copiesPropagated{0};        ///< Number of copy propagations applied.
    int cbzFusions{0};              ///< Number of cmp+b.cond → cbz/cbnz fusions.
    int maddFusions{0};             ///< Number of mul+add → madd fusions.
    int ldpStpMerges{0};            ///< Number of ldr/str pairs merged into ldp/stp.
    int branchInversions{0};        ///< Number of branch inversions applied.
    int immFoldings{0};             ///< Number of RRR→RI immediate foldings.
    int loopConstsHoisted{0};       ///< Number of MovRI instructions hoisted out of loops.

    /**
     * @brief Sums all recorded transformation counts.
     *
     * @return Aggregate number of counted rewrites across every category.
     */
    [[nodiscard]] int total() const noexcept {
        return identityMovesRemoved + identityFMovesRemoved + consecutiveMovsFolded +
               deadInstructionsRemoved + cmpZeroToTst + arithmeticIdentities + strengthReductions +
               branchesToNextRemoved + blocksReordered + copiesPropagated + cbzFusions +
               maddFusions + ldpStpMerges + branchInversions + immFoldings + loopConstsHoisted;
    }
};

/**
 * @brief Runs the complete post-allocation AArch64 MIR peephole pipeline.
 *
 * The pipeline performs block layout, loop-constant work, local folding and
 * fusion, optional CFG-aware DCE, cross-block spill/load forwarding, phi
 * cleanup, and final branch simplification.
 *
 * @param[in,out] fn Physical-register MIR function to optimize in place.
 * @param target Optional target description enabling CFG-aware liveness/DCE;
 *        `nullptr` selects the legacy block-local DCE path.
 * @return Counts for all transformations applied during this invocation.
 * @pre Every register operand in @p fn has been allocated to a physical register.
 */
[[nodiscard]] PeepholeStats runPeephole(MFunction &fn, const TargetInfo *target = nullptr);

/**
 * @brief Runs the inexpensive cleanup subset intended after scheduling.
 *
 * This entry point limits work to local copy propagation/folding, identity
 * removal, dead instructions and flag setters, plus final branch cleanup. It
 * deliberately omits layout, loop, phi, and cross-block memory rewrites.
 *
 * @param[in,out] fn Scheduled physical-register MIR function to clean in place.
 * @param target Optional target description enabling CFG-aware behavior in
 *        shared helpers; `nullptr` retains local cleanup semantics.
 * @return Counts for transformations applied by this reduced pipeline.
 */
[[nodiscard]] PeepholeStats runPostSchedulePeephole(MFunction &fn,
                                                    const TargetInfo *target = nullptr);

/**
 * @brief Removes stale callee-saved entries after MIR rewrites.
 *
 * Peephole folding can eliminate the final operand reference to a register
 * selected by register allocation. This scan retains only saved GPRs/FPRs that
 * still occur as physical register operands.
 *
 * @param[in,out] fn Function whose saved-register lists are pruned in place.
 * @post Every register in `savedGPRs` or `savedFPRs` appears in at least one
 *       MIR operand.
 */
void pruneUnusedCalleeSaved(MFunction &fn);

} // namespace zanna::codegen::aarch64
