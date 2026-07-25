//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/ArithSimplify.cpp
// Purpose: Arithmetic simplification peephole sub-passes for the x86-64 backend.
//          Implements MOV-zero to XOR, CMP-zero to TEST, arithmetic identity
//          elimination, and multiply strength reduction.
// Key invariants:
//   - All flag-clobbering rewrites check nextInstrReadsFlags before applying.
//   - Strength reduction only applies when EFLAGS semantics are preserved.
// Ownership/Lifetime:
//   - Stateless; all state is owned by the caller.
// Links: src/codegen/x86_64/peephole/ArithSimplify.hpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#include "ArithSimplify.hpp"

/// @file
/// @brief Implements flag-aware arithmetic identity and strength reductions.

namespace zanna::codegen::x64::peephole {

/// @brief Rewrite @p instr to @c "XOR reg, reg" (the canonical zero idiom).
/// @details Used by the @c MOVri-zero rewrite to take advantage of the fact
///          that XORing a register with itself is shorter to encode and
///          breaks the dependency chain on the previous value of @p regOperand.
/// @param instr Instruction rewritten in place.
/// @param regOperand The destination/source register operand (used twice).
/// @pre The caller has proved that the new EFLAGS definition is unobservable.
void rewriteToXor(MInstr &instr, Operand regOperand) {
    instr.opcode = MOpcode::XORrr32;
    instr.operands.clear();
    instr.operands.push_back(regOperand);
    instr.operands.push_back(regOperand);
}

/// @brief Rewrite @p instr to @c "TEST reg, reg" — the canonical zero-test.
/// @details Converts a @c CMP reg, #0 into a self-test that produces the
///          same condition flags consumed by backend branches (including
///          ZF=1 iff the register is zero) with a smaller encoding.
/// @param instr Instruction rewritten in place.
/// @param regOperand Register being compared (used twice in the new TEST).
/// @post Source-location metadata and other non-opcode fields remain unchanged.
void rewriteToTest(MInstr &instr, Operand regOperand) {
    instr.opcode = MOpcode::TESTrr;
    instr.operands.clear();
    instr.operands.push_back(regOperand);
    instr.operands.push_back(regOperand);
}

/// @brief Detect identity arithmetic instructions that can be removed.
/// @details Looks at @p instrs[idx] for one of:
///          - @c "ADD/OR/XOR reg, #0"
///          - @c "AND reg, #-1"
///          - @c "SHL/SHR/SAR reg, #0"
///          and reports it as removable when EFLAGS aren't observed downstream.
///          The instruction stays in the vector until the caller compacts it
///          out — this routine only marks the candidate by returning true and
///          bumping @p stats.
/// @param instrs Instruction stream being scanned.
/// @param idx Index of the candidate.
/// @param stats Pass-wide statistics accumulator (updated on hit).
/// @return @c true when the instruction is a removable identity.
/// @pre @p idx is less than @c instrs.size().
bool tryArithmeticIdentity(const std::vector<MInstr> &instrs,
                           std::size_t idx,
                           PeepholeStats &stats) {
    const auto &instr = instrs[idx];
    switch (instr.opcode) {
        case MOpcode::ADDri:
            // add reg, #0 -> no-op (but sets flags: ZF=1 iff reg==0, CF=0, OF=0)
            if (instr.operands.size() == 2 && isZeroImm(instr.operands[1])) {
                if (nextInstrReadsFlags(instrs, idx))
                    return false;
                ++stats.arithmeticIdentities;
                return true;
            }
            break;

        case MOpcode::ORri:
        case MOpcode::XORri:
            // or reg, #0 -> no-op | xor reg, #0 -> no-op (but sets flags)
            if (instr.operands.size() == 2 && isZeroImm(instr.operands[1])) {
                if (nextInstrReadsFlags(instrs, idx))
                    return false;
                ++stats.arithmeticIdentities;
                return true;
            }
            break;

        case MOpcode::ANDri:
            // and reg, #-1 -> no-op (AND with all-ones is identity, but sets flags)
            if (instr.operands.size() == 2) {
                const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
                if (imm && imm->val == -1) {
                    if (nextInstrReadsFlags(instrs, idx))
                        return false;
                    ++stats.arithmeticIdentities;
                    return true;
                }
            }
            break;

        case MOpcode::SHLri:
        case MOpcode::SHRri:
        case MOpcode::SARri:
            // shift by 0 -> no-op (but sets flags)
            if (instr.operands.size() == 2 && isZeroImm(instr.operands[1])) {
                if (nextInstrReadsFlags(instrs, idx))
                    return false;
                ++stats.arithmeticIdentities;
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

/// @brief Convert @c "IMUL dst, src" with a power-of-2 source into a left-shift.
/// @details Walks the register-constant map @p knownConsts to recover the
///          immediate that flowed into @c src. If it is a power of two and
///          downstream code does not consume EFLAGS (IMUL and SHL set CF/OF
///          differently), the instruction is rewritten to @c SHLri with the
///          appropriate shift count.
/// @param instrs Instruction stream (mutated in place).
/// @param idx Index of the candidate IMUL.
/// @param knownConsts Per-block constant tracking map.
/// @param stats Pass statistics accumulator.
/// @return @c true when the rewrite was applied.
/// @pre @p idx is less than @c instrs.size().
bool tryStrengthReduction(std::vector<MInstr> &instrs,
                          std::size_t idx,
                          const RegConstMap &knownConsts,
                          PeepholeStats &stats) {
    auto &instr = instrs[idx];
    if (instr.opcode != MOpcode::IMULrr)
        return false;
    if (instr.operands.size() != 2)
        return false;

    // imul dst, src - check if src is a known power-of-2 constant
    auto srcConst = getConstValue(instr.operands[1], knownConsts);
    if (!srcConst)
        return false;

    int shiftAmount = log2IfPowerOf2(*srcConst);
    if (shiftAmount < 0 || shiftAmount > 63)
        return false;

    // Check if any following instruction reads flags set by IMUL.
    // SHL sets CF/OF differently than IMUL, so the transformation is unsafe
    // when flags are consumed.
    if (nextInstrReadsFlags(instrs, idx))
        return false;

    // Rewrite: imul dst, src -> shl dst, #shift
    instr.opcode = MOpcode::SHLri;
    instr.operands[1] = OpImm{shiftAmount};
    ++stats.strengthReductions;
    return true;
}

} // namespace zanna::codegen::x64::peephole
