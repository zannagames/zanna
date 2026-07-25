//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/StrengthReduce.cpp
// Purpose: Arithmetic identity elimination, strength reduction (mul/udiv/sdiv
//          to shifts, div-by-constant to multiply-by-magic), cmp-zero-to-tst,
//          and immediate folding for the AArch64 peephole optimizer.
//
// Key invariants:
//   - Rewrites preserve semantic equivalence under the AArch64 ISA.
//   - Strength reduction only applies to provably equivalent transforms.
//   - Division strength reduction covers:
//     * UDIV by power-of-2 -> LSR (logical shift right)
//     * SDIV by power-of-2 -> ASR with sign correction
//     * SDIV by arbitrary constant -> SMULH + shifts (magic number multiply)
//   - Remainder fusion covers:
//     * UDIV+MSUB (UREM) by power-of-2 -> AND mask
//     * SDIV+MSUB (SREM) by positive power-of-2 -> sign-corrected mask sequence
//
// Ownership/Lifetime:
//   - Operates on mutable instructions owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "StrengthReduce.hpp"

#include "../TargetAArch64.hpp"
#include "PeepholeCommon.hpp"
#include "codegen/common/MagicDivision.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_AMD64))
#include <intrin.h>
#endif

/// @file
/// @brief Implements AArch64 arithmetic identities and constant strength reduction.

namespace zanna::codegen::aarch64::peephole {
namespace {

/// @brief Compute the base-two exponent of a positive signed power of two.
/// @param value Candidate signed value.
/// @return Exponent in `[0, 62]`, or `-1` when @p value is non-positive or is
///         not a power of two.
[[nodiscard]] int log2IfPowerOf2(long long value) noexcept {
    if (value <= 0)
        return -1;
    if ((value & (value - 1)) != 0)
        return -1;
    int log = 0;
    while ((1LL << log) < value)
        ++log;
    return log;
}

/// @brief Magic number parameters for signed division by constant.
///
/// For a positive divisor d, we compute multiplier M and post-shift S such that:
///   floor(x / d) = floor((x * M) >> (64 + S)) + sign_correction
///
/// The SMULH instruction computes the upper 64 bits of x * M, giving us
/// floor(x * M / 2^64). Combined with a post-shift of S, this yields the
/// quotient.
using zanna::codegen::MagicNumber;
using zanna::codegen::UnsignedMagicNumber;

/// @brief Select the first candidate register that does not conflict with any operand in @p avoid.
/// @param candidates Ordered list of scratch register candidates to try.
/// @param avoid      Operands whose physical registers must not be reused.
/// @return The first non-conflicting candidate, or nullopt if all conflict.
[[nodiscard]] std::optional<MOperand> pickTempReg(const std::array<PhysReg, 3> &candidates,
                                                  std::initializer_list<MOperand> avoid) {
    for (PhysReg candidate : candidates) {
        const MOperand reg = MOperand::regOp(candidate);
        bool conflicts = false;
        for (const auto &blocked : avoid) {
            if (isPhysReg(blocked) && samePhysReg(blocked, reg)) {
                conflicts = true;
                break;
            }
        }
        if (!conflicts)
            return reg;
    }
    return std::nullopt;
}

/// @brief Return true if @p reg is used by any instruction after @p idx before being redefined.
/// @details Scans forward from idx+1; stops early when a definition is found.
///          When the scan reaches the end of the block without a redefinition,
///          the register may still be live: the allocator can carry values to
///          a single-predecessor successor in registers with no in-block use
///          marking the carry. @p carriedExitRegs (MBasicBlock::carriedExitRegs,
///          sorted) supplies that invisible live-out set.
/// @param instrs Block-local instruction sequence to scan.
/// @param idx Index after which the scan begins.
/// @param reg Physical register whose liveness is queried.
/// @param carriedExitRegs Optional sorted allocator-provided live-through set.
/// @return `true` when @p reg is used before redefinition or remains live at
///         the end of the block.
[[nodiscard]] bool regUsedAfterBeforeRedef(
    const std::vector<MInstr> &instrs,
    std::size_t idx,
    const MOperand &reg,
    const std::vector<uint16_t> *carriedExitRegs = nullptr) noexcept {
    for (std::size_t i = idx + 1; i < instrs.size(); ++i) {
        if (usesReg(instrs[i], reg))
            return true;
        if (definesReg(instrs[i], reg))
            return false;
    }
    if (carriedExitRegs != nullptr && reg.kind == MOperand::Kind::Reg && reg.reg.isPhys) {
        return std::binary_search(
            carriedExitRegs->begin(), carriedExitRegs->end(), reg.reg.idOrPhys);
    }
    return false;
}

/// @brief Compute the magic number for signed division by a constant.
///
/// Algorithm based on Warren's "Hacker's Delight" (2nd edition, §10-4).
/// Given a positive divisor d, find M and S such that for any signed 64-bit x:
///   floor(x / d) = (smulh(x, M) [+ x if needsAdd] >> S) + (x < 0 ? 1 : 0)
///
/// @param d The divisor (must be >= 2).
/// @return Magic-number parameters. An unsuitable divisor is represented by
///         the shared helper's zero multiplier sentinel.
[[maybe_unused]] [[nodiscard]] MagicNumber computeSignedMagic(long long d) noexcept {
    return zanna::codegen::computeSignedMagic(d);
}

/// @brief Compute the magic number for unsigned division by a non-power-of-2 constant.
///
/// Returns the UMULH multiplier and post-shift count for an optimized unsigned divide
/// sequence. The `needsAdd` flag indicates that the libdivide-style correction step
/// (subtract-then-shift) is required when the exact multiplier overflows 64 bits.
///
/// @param d The unsigned divisor; must be > 1 and not a power of 2.
/// @return Magic number parameters, or nullopt if @p d is unsuitable.
[[nodiscard]] std::optional<UnsignedMagicNumber> computeUnsignedMagic(uint64_t d) noexcept {
    return zanna::codegen::computeUnsignedMagic(d);
}

} // namespace

/// @copydoc tryCmpZeroToTst
bool tryCmpZeroToTst(MInstr &instr, PeepholeStats &stats) {
    if (instr.opc != MOpcode::CmpRI)
        return false;
    if (instr.ops.size() != 2)
        return false;
    if (!isPhysReg(instr.ops[0]) || !isImmValue(instr.ops[1], 0))
        return false;

    instr.opc = MOpcode::TstRR;
    instr.ops[1] = instr.ops[0];
    ++stats.cmpZeroToTst;
    return true;
}

/// @copydoc tryArithmeticIdentity
bool tryArithmeticIdentity(MInstr &instr, PeepholeStats &stats) {
    switch (instr.opc) {
        case MOpcode::AddRI:
        case MOpcode::SubRI:
            if (instr.ops.size() == 3 && isImmValue(instr.ops[2], 0)) {
                instr.opc = MOpcode::MovRR;
                instr.ops.pop_back();
                ++stats.arithmeticIdentities;
                return true;
            }
            break;

        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
            if (instr.ops.size() == 3 && isImmValue(instr.ops[2], 0)) {
                instr.opc = MOpcode::MovRR;
                instr.ops.pop_back();
                ++stats.arithmeticIdentities;
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

/// @copydoc tryStrengthReduction
bool tryStrengthReduction(MInstr &instr, const RegConstMap &knownConsts, PeepholeStats &stats) {
    if (instr.opc != MOpcode::MulRRR)
        return false;
    if (instr.ops.size() != 3)
        return false;

    auto lhsConst = getConstValue(instr.ops[1], knownConsts);
    auto rhsConst = getConstValue(instr.ops[2], knownConsts);

    int shiftAmount = -1;
    MOperand otherOperand;

    if (lhsConst) {
        int log = log2IfPowerOf2(*lhsConst);
        if (log >= 0 && log <= 63) {
            shiftAmount = log;
            otherOperand = instr.ops[2];
        }
    }
    if (shiftAmount < 0 && rhsConst) {
        int log = log2IfPowerOf2(*rhsConst);
        if (log >= 0 && log <= 63) {
            shiftAmount = log;
            otherOperand = instr.ops[1];
        }
    }

    if (shiftAmount < 0)
        return false;

    instr.opc = MOpcode::LslRI;
    instr.ops[1] = otherOperand;
    instr.ops[2] = MOperand::immOp(shiftAmount);
    ++stats.strengthReductions;
    return true;
}

/// @copydoc tryDivStrengthReduction
bool tryDivStrengthReduction(MInstr &instr, const RegConstMap &knownConsts, PeepholeStats &stats) {
    if (instr.opc != MOpcode::UDivRRR)
        return false;
    if (instr.ops.size() != 3)
        return false;

    auto rhsConst = getConstValue(instr.ops[2], knownConsts);
    if (!rhsConst || *rhsConst <= 0)
        return false;

    int log = log2IfPowerOf2(*rhsConst);
    if (log < 0 || log > 63)
        return false;

    instr.opc = MOpcode::LsrRI;
    instr.ops[2] = MOperand::immOp(log);
    ++stats.strengthReductions;
    return true;
}

/// @copydoc tryUDivStrengthReduction
bool tryUDivStrengthReduction(std::vector<MInstr> &instrs,
                              std::size_t idx,
                              const RegConstMap &knownConsts,
                              PeepholeStats &stats,
                              const std::vector<uint16_t> *carriedExitRegs) {
    if (idx >= instrs.size())
        return false;

    auto &divInstr = instrs[idx];
    if (divInstr.opc != MOpcode::UDivRRR || divInstr.ops.size() != 3)
        return false;
    if (!isPhysReg(divInstr.ops[0]) || !isPhysReg(divInstr.ops[1]) || !isPhysReg(divInstr.ops[2]))
        return false;

    auto rhsConst = getConstValue(divInstr.ops[2], knownConsts);
    if (!rhsConst || *rhsConst <= 1)
        return false;

    const uint64_t divisor = static_cast<uint64_t>(*rhsConst);
    if ((divisor & (divisor - 1)) == 0)
        return false;

    const auto magic = computeUnsignedMagic(divisor);
    if (!magic.has_value())
        return false;

    const MOperand dst = divInstr.ops[0];
    const MOperand lhs = divInstr.ops[1];
    const MOperand rhsReg = divInstr.ops[2];

    const bool rhsLiveAfter = regUsedAfterBeforeRedef(instrs, idx, rhsReg, carriedExitRegs);
    if (rhsLiveAfter)
        return false;

    const PhysReg rhsPhys = static_cast<PhysReg>(rhsReg.reg.idOrPhys);
    const auto tempReg = pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs});
    if (!tempReg.has_value())
        return false;

    MOperand lhsValue = lhs;
    std::vector<MInstr> expansion;
    if (magic->needsAdd && samePhysReg(dst, lhs)) {
        const auto preservedLhs =
            pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs, *tempReg});
        if (!preservedLhs.has_value())
            return false;
        expansion.push_back(MInstr{MOpcode::MovRR, {*preservedLhs, lhs}});
        lhsValue = *preservedLhs;
    }

    expansion.push_back(MInstr{
        MOpcode::MovRI, {*tempReg, MOperand::immOp(static_cast<long long>(magic->multiplier))}});
    expansion.push_back(MInstr{MOpcode::UmulhRRR, {dst, lhsValue, *tempReg}});

    if (magic->needsAdd) {
        expansion.push_back(MInstr{MOpcode::SubRRR, {*tempReg, lhsValue, dst}});
        expansion.push_back(MInstr{MOpcode::LsrRI, {*tempReg, *tempReg, MOperand::immOp(1)}});
        expansion.push_back(MInstr{MOpcode::AddRRR, {dst, *tempReg, dst}});
    }

    if (magic->shift > 0)
        expansion.push_back(MInstr{
            MOpcode::LsrRI, {dst, dst, MOperand::immOp(static_cast<long long>(magic->shift))}});

    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx));
    instrs.insert(
        instrs.begin() + static_cast<std::ptrdiff_t>(idx), expansion.begin(), expansion.end());

    ++stats.strengthReductions;
    return true;
}

/// @copydoc tryImmediateFolding
bool tryImmediateFolding(MInstr &instr, const RegConstMap &knownConsts, PeepholeStats &stats) {
    if (instr.ops.size() != 3)
        return false;

    MOpcode riOpc;
    switch (instr.opc) {
        case MOpcode::AddRRR:
            riOpc = MOpcode::AddRI;
            break;
        case MOpcode::SubRRR:
            riOpc = MOpcode::SubRI;
            break;
        default:
            return false;
    }

    auto rhsConst = getConstValue(instr.ops[2], knownConsts);
    if (!rhsConst)
        return false;

    long long val = *rhsConst;
    if (val < 0 || val > 4095)
        return false;

    instr.opc = riOpc;
    instr.ops[2] = MOperand::immOp(val);
    ++stats.immFoldings;
    return true;
}

/// @copydoc tryFPArithmeticIdentity
[[maybe_unused]] bool tryFPArithmeticIdentity([[maybe_unused]] MInstr &instr,
                                              [[maybe_unused]] PeepholeStats &stats) {
    return false;
}

/// @copydoc trySDivStrengthReduction
bool trySDivStrengthReduction(std::vector<MInstr> &instrs,
                              std::size_t idx,
                              const RegConstMap &knownConsts,
                              PeepholeStats &stats,
                              const std::vector<uint16_t> *carriedExitRegs) {
    if (idx >= instrs.size())
        return false;

    auto &divInstr = instrs[idx];
    if (divInstr.opc != MOpcode::SDivRRR || divInstr.ops.size() != 3)
        return false;

    auto rhsConst = getConstValue(divInstr.ops[2], knownConsts);
    if (!rhsConst || *rhsConst == 0)
        return false;

    const long long divisor = *rhsConst;
    const MOperand dst = divInstr.ops[0];
    const MOperand lhs = divInstr.ops[1];
    const MOperand rhsReg = divInstr.ops[2]; // register holding the constant divisor
    if (!isPhysReg(dst) || !isPhysReg(lhs) || !isPhysReg(rhsReg))
        return false;

    if (divisor == 1) {
        divInstr.opc = MOpcode::MovRR;
        divInstr.ops = {dst, lhs};
        ++stats.strengthReductions;
        return true;
    }

    if (divisor == -1) {
        const PhysReg rhsPhys = static_cast<PhysReg>(rhsReg.reg.idOrPhys);
        const bool rhsLiveAfter = regUsedAfterBeforeRedef(instrs, idx, rhsReg, carriedExitRegs);
        if (rhsLiveAfter)
            return false;

        const auto zeroReg = pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs});
        if (!zeroReg.has_value())
            return false;

        std::vector<MInstr> expansion;
        expansion.push_back(MInstr{MOpcode::MovRI, {*zeroReg, MOperand::immOp(0)}});
        expansion.push_back(MInstr{MOpcode::SubRRR, {dst, *zeroReg, lhs}});
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx));
        instrs.insert(
            instrs.begin() + static_cast<std::ptrdiff_t>(idx), expansion.begin(), expansion.end());
        ++stats.strengthReductions;
        return true;
    }

    const bool positiveDivisor = divisor > 0;
    const int log = positiveDivisor ? log2IfPowerOf2(divisor) : -1;
    if (log >= 1 && log <= 63) {
        const bool rhsLiveAfter = regUsedAfterBeforeRedef(instrs, idx, rhsReg, carriedExitRegs);
        if (rhsLiveAfter)
            return false;

        // SDIV by power-of-2: Replace with sign-corrected arithmetic shift.
        //
        // For x / 2^k (signed), the standard sequence is:
        //   asr  tmp, x, #63       ; sign extension: -1 if negative, 0 if positive
        //   lsr  tmp, tmp, #(64-k) ; extract (2^k - 1) if negative, 0 if positive
        //   add  tmp, x, tmp       ; bias: add (2^k-1) to round toward zero
        //   asr  dst, tmp, #k      ; arithmetic shift to divide
        //
        const PhysReg rhsPhys = static_cast<PhysReg>(rhsReg.reg.idOrPhys);
        const auto tmpReg = pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs});
        if (!tmpReg.has_value())
            return false;

        std::vector<MInstr> expansion;

        // asr tmp, lhs, #63
        expansion.push_back(MInstr{MOpcode::AsrRI, {*tmpReg, lhs, MOperand::immOp(63)}});

        // lsr tmp, tmp, #(64-k)
        expansion.push_back(MInstr{MOpcode::LsrRI, {*tmpReg, *tmpReg, MOperand::immOp(64 - log)}});

        // add tmp, lhs, tmp
        expansion.push_back(MInstr{MOpcode::AddRRR, {*tmpReg, lhs, *tmpReg}});

        // asr dst, tmp, #k
        expansion.push_back(MInstr{MOpcode::AsrRI, {dst, *tmpReg, MOperand::immOp(log)}});

        // Replace the SDivRRR with the expansion
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx));
        instrs.insert(
            instrs.begin() + static_cast<std::ptrdiff_t>(idx), expansion.begin(), expansion.end());

        ++stats.strengthReductions;
        return true;
    }

    if (divisor == std::numeric_limits<long long>::min())
        return false;

    const long long absDivisor = divisor < 0 ? -divisor : divisor;
    const auto magic = computeSignedMagic(absDivisor);
    if (magic.multiplier == 0)
        return false;

    const bool rhsLiveAfter = regUsedAfterBeforeRedef(instrs, idx, rhsReg, carriedExitRegs);
    if (rhsLiveAfter)
        return false;

    const PhysReg rhsPhys = static_cast<PhysReg>(rhsReg.reg.idOrPhys);
    const auto tempReg = pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs});
    if (!tempReg.has_value())
        return false;

    MOperand lhsValue = lhs;
    std::vector<MInstr> expansion;
    if (samePhysReg(dst, lhs)) {
        const auto preservedLhs =
            pickTempReg({kScratchGPR, kScratchGPR2, rhsPhys}, {dst, lhs, *tempReg});
        if (!preservedLhs.has_value())
            return false;
        expansion.push_back(MInstr{MOpcode::MovRR, {*preservedLhs, lhs}});
        lhsValue = *preservedLhs;
    }

    expansion.push_back(MInstr{
        MOpcode::MovRI, {*tempReg, MOperand::immOp(static_cast<long long>(magic.multiplier))}});
    expansion.push_back(MInstr{MOpcode::SmulhRRR, {dst, lhsValue, *tempReg}});
    if (magic.multiplier < 0)
        expansion.push_back(MInstr{MOpcode::AddRRR, {dst, dst, lhsValue}});
    if (magic.shift > 0)
        expansion.push_back(MInstr{MOpcode::AsrRI, {dst, dst, MOperand::immOp(magic.shift)}});
    expansion.push_back(MInstr{MOpcode::LsrRI, {*tempReg, lhsValue, MOperand::immOp(63)}});
    expansion.push_back(MInstr{MOpcode::AddRRR, {dst, dst, *tempReg}});
    if (divisor < 0) {
        expansion.push_back(MInstr{MOpcode::MovRI, {*tempReg, MOperand::immOp(0)}});
        expansion.push_back(MInstr{MOpcode::SubRRR, {dst, *tempReg, dst}});
    }

    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx));
    instrs.insert(
        instrs.begin() + static_cast<std::ptrdiff_t>(idx), expansion.begin(), expansion.end());

    ++stats.strengthReductions;
    return true;
}

/// @copydoc tryRemainderFusion
bool tryRemainderFusion(std::vector<MInstr> &instrs,
                        std::size_t idx,
                        const RegConstMap &knownConsts,
                        PeepholeStats &stats,
                        const std::vector<uint16_t> *carriedExitRegs) {
    // Match the pattern: [SU]DivRRR tmp, lhs, rhs; MSubRRRR dst, tmp, rhs, lhs
    // where rhs is a known power-of-2 constant.
    //
    // For UREM by power-of-2 N:
    //   Replace with: AndRI dst, lhs, #(N-1)
    //
    // For SREM by power-of-2 N:
    //   The SDIV is handled by trySDivStrengthReduction (which expands it to shifts).
    //   If we get here, SDIV was NOT yet strength-reduced, so we do it inline:
    //   Replace SDIV+MSUB with:
    //     asr  tmp, lhs, #63
    //     and  tmp, tmp, #(N-1)      ; mask = (2^k-1) if negative, 0 if positive
    //     add  tmp, lhs, tmp         ; bias lhs toward zero
    //     and  tmp, tmp, #~(N-1)     ; clear lower k bits (round down to multiple of N)
    //     sub  dst, lhs, tmp         ; remainder = lhs - rounded_down
    //   But this is complex and may not always fit in valid logical immediates.
    //   For now, only handle UREM by power-of-2.

    if (idx + 1 >= instrs.size())
        return false;

    auto &divInstr = instrs[idx];
    auto &msubInstr = instrs[idx + 1];

    // Check for [SU]DivRRR
    const bool isUnsigned = (divInstr.opc == MOpcode::UDivRRR);
    if (divInstr.opc != MOpcode::UDivRRR && divInstr.opc != MOpcode::SDivRRR)
        return false;
    if (divInstr.ops.size() != 3)
        return false;

    // Check for MSubRRRR
    if (msubInstr.opc != MOpcode::MSubRRRR || msubInstr.ops.size() != 4)
        return false;

    // Verify the pattern: msub dst, divDst, rhs, lhs
    // MSubRRRR ops: [dst, mul1, mul2, sub] => dst = sub - mul1*mul2
    const MOperand &divDst = divInstr.ops[0];
    const MOperand &divLhs = divInstr.ops[1];
    const MOperand &divRhs = divInstr.ops[2];

    // msub's mul1 must be the div result
    if (!samePhysReg(msubInstr.ops[1], divDst))
        return false;

    // msub's mul2 must be the divisor
    if (!samePhysReg(msubInstr.ops[2], divRhs))
        return false;

    // msub's sub operand must be the dividend
    if (!samePhysReg(msubInstr.ops[3], divLhs))
        return false;

    // Get the constant divisor
    auto rhsConst = getConstValue(divRhs, knownConsts);
    if (!rhsConst || *rhsConst <= 1)
        return false;

    const long long divisor = *rhsConst;
    const int log = log2IfPowerOf2(divisor);

    if (log < 1 || log > 63)
        return false;

    const long long mask = divisor - 1; // e.g., 8-1 = 7 = 0b111

    // Verify the div result register is not used after the msub
    for (std::size_t i = idx + 2; i < instrs.size(); ++i) {
        if (usesReg(instrs[i], divDst))
            return false;
        if (definesReg(instrs[i], divDst))
            break;
    }

    if (isUnsigned) {
        // UREM by power-of-2: x % N == x & (N-1)
        // Verify mask is a valid AArch64 logical immediate
        if (!isLogicalImmediate(static_cast<uint64_t>(mask)))
            return false;

        const MOperand remDst = msubInstr.ops[0];

        // Replace both instructions with: and dst, lhs, #(N-1)
        instrs[idx] = MInstr{MOpcode::AndRI, {remDst, divLhs, MOperand::immOp(mask)}};
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx + 1));

        ++stats.strengthReductions;
        return true;
    }

    // SREM by power-of-2 for signed values.
    // C/IL semantics: remainder has the same sign as the dividend.
    //
    // Optimized sequence:
    //   negs tmp, lhs             ; negate (but we don't have negs)
    //   Actually, the cleanest approach for SREM by 2^k:
    //
    //   asr  tmp, lhs, #63        ; sign mask: -1 if negative, 0 if positive
    //   lsr  tmp, tmp, #(64-k)    ; (2^k - 1) if negative, 0 if positive
    //   add  tmp, lhs, tmp        ; biased value
    //   and  tmp, tmp, #-(2^k)    ; round down to multiple of 2^k (clear low k bits)
    //   sub  dst, lhs, tmp        ; remainder = lhs - rounded_down
    //
    // #-(2^k) as a logical immediate: this is ~(2^k - 1). For k=1 that's
    // 0xFFFFFFFFFFFFFFFE, which is a valid logical immediate (alternating pattern).

    // Check if both masks are valid logical immediates
    const auto negMask = static_cast<uint64_t>(~mask); // ~(N-1) = -(N)
    if (!isLogicalImmediate(negMask))
        return false;

    // We need a temp register. Use the div's destination register.
    if (!isPhysReg(divDst))
        return false;

    const MOperand tmp = divDst;
    const MOperand remDst = msubInstr.ops[0];

    std::vector<MInstr> expansion;

    // asr tmp, lhs, #63
    expansion.push_back(MInstr{MOpcode::AsrRI, {tmp, divLhs, MOperand::immOp(63)}});

    // lsr tmp, tmp, #(64-k)
    expansion.push_back(MInstr{MOpcode::LsrRI, {tmp, tmp, MOperand::immOp(64 - log)}});

    // add tmp, lhs, tmp
    expansion.push_back(MInstr{MOpcode::AddRRR, {tmp, divLhs, tmp}});

    // and tmp, tmp, #-(2^k)   (clear low k bits to round down)
    expansion.push_back(
        MInstr{MOpcode::AndRI, {tmp, tmp, MOperand::immOp(static_cast<long long>(negMask))}});

    // sub dst, lhs, tmp       (remainder = lhs - rounded_down)
    expansion.push_back(MInstr{MOpcode::SubRRR, {remDst, divLhs, tmp}});

    // Replace both instructions with the expansion
    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(idx),
                 instrs.begin() + static_cast<std::ptrdiff_t>(idx + 2));
    instrs.insert(
        instrs.begin() + static_cast<std::ptrdiff_t>(idx), expansion.begin(), expansion.end());

    ++stats.strengthReductions;
    return true;
}

} // namespace zanna::codegen::aarch64::peephole
