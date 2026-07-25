//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/IdentityElim.cpp
// Purpose: Identity move elimination and consecutive move folding for
//          the AArch64 peephole optimizer.
//
// Key invariants:
//   - Identity move removal only removes provably redundant mov r,r / fmov d,d.
//   - Consecutive move folding checks liveness before transforming.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "IdentityElim.hpp"

#include "PeepholeCommon.hpp"

#include <algorithm>

/// @file
/// @brief Implements redundant-move recognition and adjacent copy forwarding.

namespace zanna::codegen::aarch64::peephole {

/// @brief Test whether @p instr is a `MOV Xd, Xs` whose destination equals its source.
/// @details Identity moves are unconditional no-ops and are deleted by the peephole.
///          Returns false for any other opcode, malformed operand count, or
///          non-physical operands.
/// @param instr Machine instruction to classify.
/// @return True if @p instr is an identity GPR move.
bool isIdentityMovRR(const MInstr &instr) noexcept {
    if (instr.opc != MOpcode::MovRR)
        return false;
    if (instr.ops.size() != 2)
        return false;
    return samePhysReg(instr.ops[0], instr.ops[1]);
}

/// @brief Test whether @p instr is a `FMOV Dd, Ds` whose destination equals its source.
/// @details The FPR counterpart of @ref isIdentityMovRR; same elimination contract.
/// @param instr Machine instruction to classify.
/// @return True if @p instr is an identity FPR move.
bool isIdentityFMovRR(const MInstr &instr) noexcept {
    if (instr.opc != MOpcode::FMovRR)
        return false;
    if (instr.ops.size() != 2)
        return false;
    return samePhysReg(instr.ops[0], instr.ops[1]);
}

/// @brief Return whether @p reg remains live after an adjacent-move pair.
/// @details An in-block use keeps the value live, while an intervening
///          redefinition kills it. If neither occurs, allocator-provided
///          carried-exit metadata accounts for uses in successor blocks that
///          have no local instruction marking the live-out value.
/// @param instrs Block-local instruction sequence containing the move pair.
/// @param secondMoveIndex Index of the pair's second instruction.
/// @param reg Intermediate physical register whose liveness is queried.
/// @param carriedExitRegs Optional sorted live-through register identifiers.
/// @return `true` when @p reg is used before redefinition or is live through
///         the block exit.
static bool usedAfterMovePairOrCarried(const std::vector<MInstr> &instrs,
                                       std::size_t secondMoveIndex,
                                       const MOperand &reg,
                                       const std::vector<uint16_t> *carriedExitRegs) noexcept {
    for (std::size_t i = secondMoveIndex + 1; i < instrs.size(); ++i) {
        if (usesReg(instrs[i], reg))
            return true;
        if (definesReg(instrs[i], reg))
            return false;
    }
    return carriedExitRegs != nullptr && reg.kind == MOperand::Kind::Reg && reg.reg.isPhys &&
           std::binary_search(carriedExitRegs->begin(), carriedExitRegs->end(), reg.reg.idOrPhys);
}

/// @brief Fold `MOV r1, r0` followed by `MOV r2, r1` into `MOV r2, r0` and a kill of r1.
/// @details The fold is only safe when `r1` is dead after the second move (no subsequent
///          use before redefinition) and when `r1` is not an ABI arg register whose value
///          might be implicitly consumed by an upcoming call. On success the two-move
///          sequence becomes `MOV r2, r0` plus a one-instruction kill (`MOV r1, r1`) that
///          will be removed by the identity-elimination pass on the next iteration.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param idx    Index of the first move to consider.
/// @param stats  Peephole statistics counter (incremented on success).
/// @param carriedExitRegs Sorted physical registers live into successor blocks.
/// @return True if the fold was applied at @p idx.
bool tryFoldConsecutiveMoves(std::vector<MInstr> &instrs,
                             std::size_t idx,
                             PeepholeStats &stats,
                             const std::vector<uint16_t> *carriedExitRegs) {
    if (idx + 1 >= instrs.size())
        return false;

    MInstr &first = instrs[idx];
    MInstr &second = instrs[idx + 1];

    const bool firstIsMovRR = (first.opc == MOpcode::MovRR && first.ops.size() == 2);
    const bool secondIsMovRR = (second.opc == MOpcode::MovRR && second.ops.size() == 2);

    if (!firstIsMovRR || !secondIsMovRR)
        return false;

    if (!samePhysReg(second.ops[1], first.ops[0]))
        return false;

    const MOperand &r1 = first.ops[0];
    if (isArgReg(r1)) {
        for (std::size_t i = idx + 2; i < instrs.size(); ++i) {
            if (instrs[i].opc == MOpcode::Bl || instrs[i].opc == MOpcode::Blr)
                return false;
            if (definesReg(instrs[i], r1))
                break;
        }
    }

    if (usedAfterMovePairOrCarried(instrs, idx + 1, r1, carriedExitRegs))
        return false;

    const MOperand originalSrc = first.ops[1];
    second.ops[1] = originalSrc;
    first.ops[0] = first.ops[1];
    ++stats.consecutiveMovsFolded;
    return true;
}

/// @brief Fold `MOV r1, #imm` + `MOV r2, r1` into `MOV r2, #imm` + identity-kill on r1.
/// @details Same safety constraints as @ref tryFoldConsecutiveMoves: `r1` must be dead
///          after the second move and must not be a live ABI arg register feeding an
///          upcoming call. After the fold, the first instruction becomes an identity
///          kill that's eliminated by the next identity pass; the immediate moves to
///          the user's destination, saving one register live range.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param idx    Index of the first move (the `MOVri`) to consider.
/// @param stats  Peephole statistics counter (incremented on success).
/// @param carriedExitRegs Sorted physical registers live into successor blocks.
/// @return True if the fold was applied at @p idx.
bool tryFoldImmThenMove(std::vector<MInstr> &instrs,
                        std::size_t idx,
                        PeepholeStats &stats,
                        const std::vector<uint16_t> *carriedExitRegs) {
    if (idx + 1 >= instrs.size())
        return false;

    MInstr &first = instrs[idx];
    MInstr &second = instrs[idx + 1];

    if (first.opc != MOpcode::MovRI || first.ops.size() != 2)
        return false;
    if (second.opc != MOpcode::MovRR || second.ops.size() != 2)
        return false;
    if (!samePhysReg(second.ops[1], first.ops[0]))
        return false;

    const MOperand &rd = first.ops[0];
    if (isArgReg(rd)) {
        for (std::size_t i = idx + 2; i < instrs.size(); ++i) {
            if (instrs[i].opc == MOpcode::Bl || instrs[i].opc == MOpcode::Blr)
                return false;
            if (definesReg(instrs[i], rd))
                break;
        }
    }

    if (usedAfterMovePairOrCarried(instrs, idx + 1, rd, carriedExitRegs))
        return false;

    const MOperand imm = first.ops[1];
    first.opc = MOpcode::MovRR;
    first.ops[1] = first.ops[0];
    second.opc = MOpcode::MovRI;
    second.ops[1] = imm;
    ++stats.consecutiveMovsFolded;
    return true;
}

} // namespace zanna::codegen::aarch64::peephole
