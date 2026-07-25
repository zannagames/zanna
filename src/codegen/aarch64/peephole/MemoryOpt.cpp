//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/MemoryOpt.cpp
// Purpose: Memory optimizations for the AArch64 peephole optimizer: LDP/STP
//          merging, store-load forwarding, and MADD fusion.
//
// Key invariants:
//   - LDP/STP merge only pairs adjacent FP-relative accesses with matching
//     offset alignment.
//   - Store-load forwarding only applies within a basic block scope.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "MemoryOpt.hpp"

#include "PeepholeCommon.hpp"

#include <limits>

/// @file
/// @brief Implements local AArch64 memory forwarding, pairing, and MADD fusion.

namespace zanna::codegen::aarch64::peephole {

namespace {

/// @brief Add two signed byte offsets while rejecting int64 overflow.
/// @param lhs Base offset, usually FP-relative.
/// @param rhs Width or adjacency delta to add.
/// @param out Receives the sum when representable.
/// @return True if the addition was representable.
[[nodiscard]] bool checkedOffsetAdd(int64_t lhs, int64_t rhs, int64_t &out) noexcept {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs))
        return false;
    out = lhs + rhs;
    return true;
}

/// @brief Return true when @p opcode may access memory through a non-FP base.
/// @details Store-load forwarding for FP-relative slots cannot prove these
///          accesses do not alias an address-taken stack object, so they act as
///          conservative scan barriers.
/// @param opcode Opcode to classify.
/// @return `true` for recognized scalar loads and stores through an arbitrary GPR.
[[nodiscard]] bool isBaseRelativeMemory(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
            return true;
        default:
            return false;
    }
}

} // namespace

/// @brief Merge two adjacent `LDR`/`STR` instructions into a single `LDP`/`STP`.
/// @details Recognises four candidate pairs: GPR FP-relative load, GPR FP-relative store,
///          FPR FP-relative load, FPR FP-relative store. To be mergeable, the two accesses
///          must have 8-byte-adjacent FP offsets whose lower address is pair-aligned and
///          encodable. Load destinations must differ. Successful merges normalize
///          register order by ascending address, replace the pair with one instruction,
///          and advance @p stats.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param idx    Index of the first instruction in the candidate pair.
/// @param stats  Peephole statistics counter (incremented on success).
/// @return True if the merge was applied at @p idx.
bool tryLdpStpMerge(std::vector<MInstr> &instrs, std::size_t idx, PeepholeStats &stats) {
    if (idx + 1 >= instrs.size())
        return false;

    auto &first = instrs[idx];
    auto &second = instrs[idx + 1];

    MOpcode pairOpc;
    bool isLoad = false;
    bool fprPair = false;

    if (first.opc == MOpcode::LdrRegFpImm && second.opc == MOpcode::LdrRegFpImm) {
        pairOpc = MOpcode::LdpRegFpImm;
        isLoad = true;
    } else if (first.opc == MOpcode::StrRegFpImm && second.opc == MOpcode::StrRegFpImm) {
        pairOpc = MOpcode::StpRegFpImm;
    } else if (first.opc == MOpcode::LdrFprFpImm && second.opc == MOpcode::LdrFprFpImm) {
        pairOpc = MOpcode::LdpFprFpImm;
        isLoad = true;
        fprPair = true;
    } else if (first.opc == MOpcode::StrFprFpImm && second.opc == MOpcode::StrFprFpImm) {
        pairOpc = MOpcode::StpFprFpImm;
        fprPair = true;
    } else {
        return false;
    }

    if (first.ops.size() != 2 || second.ops.size() != 2)
        return false;
    if (!isPhysReg(first.ops[0]) || !isPhysReg(second.ops[0]))
        return false;
    if (first.ops[1].kind != MOperand::Kind::Imm || second.ops[1].kind != MOperand::Kind::Imm)
        return false;

    long long off1 = first.ops[1].imm;
    long long off2 = second.ops[1].imm;

    const bool ascending = off2 == off1 + 8;
    const bool descending = off1 == off2 + 8;
    if (!ascending && !descending)
        return false;

    const long long pairOffset = ascending ? off1 : off2;
    if (pairOffset < -512 || pairOffset > 504)
        return false;
    if ((pairOffset % 8) != 0)
        return false;

    if (isLoad) {
        if (samePhysReg(first.ops[0], second.ops[0]))
            return false;
    }

    (void)fprPair;

    first.opc = pairOpc;
    MOperand reg1 = ascending ? first.ops[0] : second.ops[0];
    MOperand reg2 = ascending ? second.ops[0] : first.ops[0];
    MOperand offset = MOperand::immOp(pairOffset);
    first.ops.clear();
    first.ops.push_back(reg1);
    first.ops.push_back(reg2);
    first.ops.push_back(offset);

    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx + 1));
    ++stats.ldpStpMerges;
    return true;
}

/// @brief Compute the FP-relative byte range written by an FP-relative store.
/// @details Recognizes single (Str*FpImm, 8 bytes) and pair (Stp*FpImm, 16
///          bytes) GPR/FPR stores. Used by store→load forwarding and dead-store
///          elimination to detect aliasing between memory accesses.
/// @param ins   Candidate store instruction.
/// @param start Out: inclusive start offset (the store's FP-relative immediate).
/// @param end   Out: exclusive end offset (start + access width).
/// @return True if @p ins is a recognized FP-relative store; false otherwise.
static bool fpStoreRange(const MInstr &ins, int64_t &start, int64_t &end) {
    int64_t width = 0;
    switch (ins.opc) {
        case MOpcode::StrRegFpImm:
        case MOpcode::StrFprFpImm:
            width = 8;
            break;
        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
            width = 16;
            break;
        default:
            return false;
    }
    const std::size_t offIndex = width == 8 ? 1u : 2u;
    if (ins.ops.size() <= offIndex || ins.ops[offIndex].kind != MOperand::Kind::Imm)
        return false;
    start = ins.ops[offIndex].imm;
    if (!checkedOffsetAdd(start, width, end))
        return false;
    return true;
}

/// @brief Half-open interval overlap test: true iff [lhsStart,lhsEnd) and
///        [rhsStart,rhsEnd) intersect. Used to decide store/load aliasing.
/// @param lhsStart Inclusive beginning of the first interval.
/// @param lhsEnd Exclusive end of the first interval.
/// @param rhsStart Inclusive beginning of the second interval.
/// @param rhsEnd Exclusive end of the second interval.
/// @return `true` when the intervals share at least one byte.
static bool rangesOverlap(int64_t lhsStart, int64_t lhsEnd, int64_t rhsStart, int64_t rhsEnd) {
    return lhsStart < rhsEnd && rhsStart < lhsEnd;
}

/// @copydoc forwardStoreLoads
std::size_t forwardStoreLoads(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    std::size_t forwarded = 0;
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const bool gprStore = instrs[i].opc == MOpcode::StrRegFpImm;
        const bool isFprStore = instrs[i].opc == MOpcode::StrFprFpImm;
        if (!gprStore && !isFprStore)
            continue;
        if (instrs[i].ops.size() < 2)
            continue;
        if (!isPhysReg(instrs[i].ops[0]))
            continue;
        if (instrs[i].ops[1].kind != MOperand::Kind::Imm)
            continue;

        const int64_t storeOff = instrs[i].ops[1].imm;
        int64_t storeEnd = 0;
        if (!checkedOffsetAdd(storeOff, 8, storeEnd))
            continue;
        const MOperand storeReg = instrs[i].ops[0];
        const MOpcode matchLoad = gprStore ? MOpcode::LdrRegFpImm : MOpcode::LdrFprFpImm;
        const MOpcode matchStore = gprStore ? MOpcode::StrRegFpImm : MOpcode::StrFprFpImm;
        const MOpcode movOpc = gprStore ? MOpcode::MovRR : MOpcode::FMovRR;

        for (std::size_t j = i + 1; j < instrs.size(); ++j) {
            const auto &next = instrs[j];

            int64_t nextStoreStart = 0;
            int64_t nextStoreEnd = 0;
            if (fpStoreRange(next, nextStoreStart, nextStoreEnd) &&
                rangesOverlap(storeOff, storeEnd, nextStoreStart, nextStoreEnd))
                break;

            if (next.opc == matchStore && next.ops.size() >= 2 &&
                next.ops[1].kind == MOperand::Kind::Imm && next.ops[1].imm == storeOff)
                break;

            if (next.opc == matchLoad && next.ops.size() >= 2 &&
                next.ops[1].kind == MOperand::Kind::Imm && next.ops[1].imm == storeOff &&
                isPhysReg(next.ops[0])) {
                instrs[j] = MInstr{movOpc, {next.ops[0], storeReg}};
                ++forwarded;
                ++stats.deadInstructionsRemoved;
                continue;
            }

            if (definesReg(next, storeReg))
                break;

            if (next.opc == MOpcode::Bl || next.opc == MOpcode::Blr)
                break;

            if (next.opc == MOpcode::Br || next.opc == MOpcode::BCond || next.opc == MOpcode::Ret ||
                next.opc == MOpcode::Cbz || next.opc == MOpcode::Cbnz ||
                next.opc == MOpcode::Tbz || next.opc == MOpcode::Tbnz ||
                next.opc == MOpcode::JumpTable)
                break;

            if (isBaseRelativeMemory(next.opc))
                break;
        }
    }
    return forwarded;
}

/// @brief Fuse `MUL Xd, Xn, Xm` + `ADD Xacc, Xacc, Xd` into a single `MADD Xacc, Xn, Xm, Xacc`.
/// @details `MADD Xd, Xn, Xm, Xa` computes `Xd = Xa + (Xn * Xm)` in a single cycle on most
///          AArch64 implementations. Fusion requires that the `MUL`'s destination is dead
///          after the `ADD` (its only use is the `ADD`) and that neither input has been
///          overwritten between the two instructions. The reverse direction (`Xacc + Xd`)
///          is also recognised.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param idx    Index of the `MUL` to consider as the multiply half of the fusion.
/// @param stats  Peephole statistics counter (incremented on success).
/// @return True if the fusion was applied.
bool tryMaddFusion(std::vector<MInstr> &instrs, std::size_t idx, PeepholeStats &stats) {
    if (idx + 1 >= instrs.size())
        return false;

    auto &mulInstr = instrs[idx];
    auto &addInstr = instrs[idx + 1];

    if (mulInstr.opc != MOpcode::MulRRR || mulInstr.ops.size() != 3)
        return false;
    if (addInstr.opc != MOpcode::AddRRR || addInstr.ops.size() != 3)
        return false;

    if (!isPhysReg(mulInstr.ops[0]))
        return false;

    const MOperand &mulDst = mulInstr.ops[0];
    const MOperand &mulA = mulInstr.ops[1];
    const MOperand &mulB = mulInstr.ops[2];

    MOperand addend;
    bool mulDstInLhs = samePhysReg(addInstr.ops[1], mulDst);
    bool mulDstInRhs = samePhysReg(addInstr.ops[2], mulDst);

    if (mulDstInLhs && !mulDstInRhs) {
        addend = addInstr.ops[2];
    } else if (mulDstInRhs && !mulDstInLhs) {
        addend = addInstr.ops[1];
    } else {
        return false;
    }

    for (std::size_t i = idx + 2; i < instrs.size(); ++i) {
        if (usesReg(instrs[i], mulDst))
            return false;
        if (definesReg(instrs[i], mulDst))
            break;
    }

    const MOperand addDst = addInstr.ops[0];
    mulInstr.opc = MOpcode::MAddRRRR;
    mulInstr.ops.clear();
    mulInstr.ops.push_back(addDst);
    mulInstr.ops.push_back(mulA);
    mulInstr.ops.push_back(mulB);
    mulInstr.ops.push_back(addend);

    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx + 1));
    ++stats.maddFusions;
    return true;
}

} // namespace zanna::codegen::aarch64::peephole
