//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/Coalescer.cpp
// Purpose: Pre-register-allocation move coalescer for AArch64 MIR.
//          Eliminates redundant MovRR/FMovRR between virtual registers by
//          merging vregs whose live ranges do not interfere.
// Key invariants:
//   - Only coalesces moves where both operands are virtual registers.
//   - After coalescing, all references to the replaced vreg are rewritten.
//   - The coalesced move instruction is replaced with a no-op (identity move)
//     which is later cleaned up by the peephole pass.
// Ownership/Lifetime:
//   - Modifies MFunction in place; caller owns the MFunction.
// Links: src/codegen/aarch64/Coalescer.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements conservative pre-allocation copy coalescing for AArch64 MIR.
 *
 * Instructions are linearized in block layout order to build whole-function
 * virtual-register intervals. A move is merged only for a source-to-destination
 * handoff at the same instruction position; intervals and candidates are
 * recomputed after every rewrite, then resulting identity moves are erased.
 */

#include "codegen/aarch64/Coalescer.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::aarch64 {
namespace {

/// @brief Represents a half-open live interval [start, end) for a virtual register.
struct LiveInterval {
    unsigned start{0};
    unsigned end{0};
};

/// @brief Candidate move instruction eligible for coalescing.
struct CoalesceCandidate {
    std::size_t blockIdx{0};      ///< Block containing the move.
    std::size_t instrIdx{0};      ///< Index within the block's instruction list.
    uint16_t dstVReg{0};          ///< Destination virtual register ID.
    uint16_t srcVReg{0};          ///< Source virtual register ID.
    RegClass cls{RegClass::GPR};  ///< Register class (GPR or FPR).
};

/// @brief Check if an opcode is a virtual-to-virtual register move.
/// @param opc The machine opcode.
/// @return True for MovRR or FMovRR.
static bool isMoveRR(MOpcode opc) {
    return opc == MOpcode::MovRR || opc == MOpcode::FMovRR;
}

/// @brief Assign a globally unique instruction index to each instruction.
///
/// Walks all blocks in layout order, assigning sequential indices. This
/// produces a linearized view of the function for live interval computation.
///
/// @param fn         The machine function.
/// @param instrIndex Out-param: resized to fn.blocks and filled so that
///                   instrIndex[blockIdx][instrIdx] is the global index of
///                   that instruction. Any prior contents are cleared.
/// @return Total number of instructions assigned (the global index count).
static unsigned linearizeFunction(const MFunction &fn,
                                  std::vector<std::vector<unsigned>> &instrIndex) {
    instrIndex.clear();
    instrIndex.resize(fn.blocks.size());
    unsigned idx = 0;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        instrIndex[bi].resize(fn.blocks[bi].instrs.size());
        for (std::size_t ii = 0; ii < fn.blocks[bi].instrs.size(); ++ii) {
            instrIndex[bi][ii] = idx++;
        }
    }
    return idx;
}

/// @brief Compute conservative live intervals for all virtual registers.
///
/// For each vreg, the interval spans from its first definition to its last use
/// across the entire function (linearized). This is conservative because it does
/// not account for control flow -- it treats the function as a straight-line
/// sequence of instructions.
///
/// @param fn The machine function.
/// @param instrIndex Linearized instruction indices from linearizeFunction.
/// @param totalInstrs Total instruction count.
/// @param cls Filter: only compute intervals for vregs of this register class.
/// @return Map from vreg ID to its live interval.
static std::unordered_map<uint16_t, LiveInterval> computeLiveIntervals(
    const MFunction &fn,
    const std::vector<std::vector<unsigned>> &instrIndex,
    unsigned totalInstrs,
    RegClass cls) {
    (void)totalInstrs;
    std::unordered_map<uint16_t, LiveInterval> intervals;

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        for (std::size_t ii = 0; ii < fn.blocks[bi].instrs.size(); ++ii) {
            const auto &mi = fn.blocks[bi].instrs[ii];
            const unsigned pos = instrIndex[bi][ii];

            for (const auto &op : mi.ops) {
                if (op.kind != MOperand::Kind::Reg)
                    continue;
                if (op.reg.isPhys)
                    continue;
                if (op.reg.cls != cls)
                    continue;

                const uint16_t vreg = op.reg.idOrPhys;
                auto it = intervals.find(vreg);
                if (it == intervals.end()) {
                    intervals[vreg] = LiveInterval{pos, pos + 1};
                } else {
                    if (pos < it->second.start)
                        it->second.start = pos;
                    if (pos + 1 > it->second.end)
                        it->second.end = pos + 1;
                }
            }
        }
    }

    return intervals;
}

/// @brief Test whether two live intervals overlap.
/// @details Uses the standard half-open interval interference test: `[a.start, a.end)`
///          overlaps `[b.start, b.end)` iff `a.start < b.end && b.start < a.end`.
///          Touching endpoints (`a.end == b.start`) do *not* interfere, which is
///          the property that allows a back-to-back def-then-use move to be coalesced.
/// @param a First live interval.
/// @param b Second live interval.
/// @return True if @p a and @p b have any overlap.
static bool interferes(const LiveInterval &a, const LiveInterval &b) {
    return a.start < b.end && b.start < a.end;
}

/// @brief Rewrite every reference to @p oldVReg as @p newVReg throughout @p fn.
/// @details Walks every operand of every instruction in every block. Filters
///          on register class so a GPR rewrite cannot accidentally rename an
///          FPR that happens to share the same numeric id. Used after a coalesce
///          decision to merge the two virtual registers into one.
/// @param fn       Machine function to mutate.
/// @param cls      Register class whose ids are being merged (GPR or FPR).
/// @param oldVReg  Virtual register id being replaced.
/// @param newVReg  Virtual register id to use in place of @p oldVReg.
static void rewriteVReg(MFunction &fn, RegClass cls, uint16_t oldVReg, uint16_t newVReg) {
    for (auto &bb : fn.blocks) {
        for (auto &mi : bb.instrs) {
            for (auto &op : mi.ops) {
                if (op.kind == MOperand::Kind::Reg && !op.reg.isPhys && op.reg.cls == cls &&
                    op.reg.idOrPhys == oldVReg) {
                    op.reg.idOrPhys = newVReg;
                }
            }
        }
    }
}

/// @brief Collect candidate move instructions for coalescing.
/// @param fn Machine function to scan.
/// @return Virtual-to-virtual, same-class, non-identity move locations.
static std::vector<CoalesceCandidate> collectCandidates(const MFunction &fn) {
    std::vector<CoalesceCandidate> candidates;

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        for (std::size_t ii = 0; ii < fn.blocks[bi].instrs.size(); ++ii) {
            const auto &mi = fn.blocks[bi].instrs[ii];
            if (!isMoveRR(mi.opc))
                continue;
            if (mi.ops.size() < 2)
                continue;

            const auto &dst = mi.ops[0];
            const auto &src = mi.ops[1];

            // Both must be virtual registers of the same class.
            if (dst.kind != MOperand::Kind::Reg || src.kind != MOperand::Kind::Reg)
                continue;
            if (dst.reg.isPhys || src.reg.isPhys)
                continue;
            if (dst.reg.cls != src.reg.cls)
                continue;
            // Skip identity moves.
            if (dst.reg.idOrPhys == src.reg.idOrPhys)
                continue;

            candidates.push_back(
                CoalesceCandidate{bi, ii, dst.reg.idOrPhys, src.reg.idOrPhys, dst.reg.cls});
        }
    }

    return candidates;
}

/// @brief Run one round of coalescing for a given register class.
/// @param fn Machine function modified in place.
/// @param cls Register class processed independently.
/// @return The number of moves coalesced.
static unsigned coalesceClass(MFunction &fn, RegClass cls) {
    unsigned coalesced = 0;

    // Recompute intervals and candidates each iteration since coalescing
    // changes live ranges.
    bool changed = true;
    while (changed) {
        changed = false;

        std::vector<std::vector<unsigned>> instrIndex;
        const unsigned totalInstrs = linearizeFunction(fn, instrIndex);
        auto intervals = computeLiveIntervals(fn, instrIndex, totalInstrs, cls);
        auto candidates = collectCandidates(fn);

        for (const auto &cand : candidates) {
            if (cand.cls != cls)
                continue;

            auto dstIt = intervals.find(cand.dstVReg);
            auto srcIt = intervals.find(cand.srcVReg);
            if (dstIt == intervals.end() || srcIt == intervals.end())
                continue;

            // The move defines dstVReg from srcVReg. For coalescing, we need
            // to check whether the *merged* interval would conflict. Since we
            // are replacing dstVReg with srcVReg, the srcVReg's interval
            // extends to cover dstVReg's range. We check: does dstVReg's range
            // (excluding the move point itself) overlap with srcVReg's range?
            //
            // Conservative check: the src interval, extended by the dst
            // interval, must not interfere with any *other* vreg that already
            // interferes with only one of them. But a simpler, still-sound
            // check: if the src and dst intervals only overlap at the move
            // point itself, coalescing is safe.
            //
            // We use the simple non-interference check: the src live range
            // (excluding the definition of dst at the move point) must not
            // overlap with dst's live range.

            const unsigned movePos = instrIndex[cand.blockIdx][cand.instrIdx];

            // Build the effective src interval excluding the move point.
            // The src is used at movePos (as a read), and the dst is defined
            // at movePos. After coalescing, the merged vreg would span
            // min(src.start, dst.start) to max(src.end, dst.end).
            // The coalescing is safe if the only overlap point between src and
            // dst intervals is the move instruction itself.

            LiveInterval srcRange = srcIt->second;
            LiveInterval dstRange = dstIt->second;

            // Only coalesce the defensible copy-elimination shape:
            // the source's last touch is the move, and the destination's first
            // touch is the same move.  This preserves the optimization for
            // SSA-style copy handoff while avoiding whole-function rewrites for
            // values that are live on unrelated CFG paths.
            if (srcRange.end != movePos + 1 || dstRange.start != movePos)
                continue;
            if (interferes(LiveInterval{srcRange.start, movePos}, LiveInterval{movePos + 1, dstRange.end}))
                continue;

            // Coalesce: rewrite all dstVReg references to srcVReg.
            rewriteVReg(fn, cls, cand.dstVReg, cand.srcVReg);

            // The move is now an identity move (srcVReg -> srcVReg).
            // Mark it for deletion by converting to identity.
            // The peephole pass or regalloc will clean these up.
            // We leave it as-is since rewriteVReg already made both
            // operands the same vreg.

            ++coalesced;
            changed = true;
            break; // Restart with fresh intervals after each coalesce.
        }
    }

    return coalesced;
}

/// @brief Remove identity moves (MovRR/FMovRR where src == dst vreg).
/// @param fn Machine function whose blocks are compacted in place.
static void removeIdentityMoves(MFunction &fn) {
    for (auto &bb : fn.blocks) {
        std::vector<MInstr> filtered;
        filtered.reserve(bb.instrs.size());
        for (auto &mi : bb.instrs) {
            if (isMoveRR(mi.opc) && mi.ops.size() >= 2 && mi.ops[0].kind == MOperand::Kind::Reg &&
                mi.ops[1].kind == MOperand::Kind::Reg && !mi.ops[0].reg.isPhys &&
                !mi.ops[1].reg.isPhys && mi.ops[0].reg.idOrPhys == mi.ops[1].reg.idOrPhys) {
                continue; // Skip identity move.
            }
            filtered.push_back(std::move(mi));
        }
        bb.instrs = std::move(filtered);
    }
}

} // namespace

/// @brief Coalesce safe GPR and FPR virtual-register copies in @p fn.
/// @param fn Machine function modified in place.
/// @post Coalesced identity moves are removed from their blocks.
void coalesce(MFunction &fn) {
    // Coalesce GPR and FPR classes independently.
    coalesceClass(fn, RegClass::GPR);
    coalesceClass(fn, RegClass::FPR);

    // Clean up identity moves left behind by coalescing.
    removeIdentityMoves(fn);
}

} // namespace zanna::codegen::aarch64
