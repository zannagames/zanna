//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/StrengthReduce.hpp
// Purpose: Declarations for arithmetic identity elimination, strength reduction,
//          division/modulo optimization, and immediate folding peephole sub-passes.
//
// Key invariants:
//   - Rewrites preserve semantic equivalence under the AArch64 ISA.
//   - Strength reduction only applies to provably equivalent transforms.
//   - Division strength reduction covers UDIV, SDIV (power-of-2 and arbitrary
//     constant), and remainder fusion (UDIV+MSUB, SDIV+MSUB by power-of-2).
//
// Ownership/Lifetime:
//   - Operates on mutable instructions owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "../Peephole.hpp"
#include "PeepholeCommon.hpp"

/// @file
/// @brief Declares integer strength-reduction and arithmetic folding rewrites.

namespace zanna::codegen::aarch64::peephole {

/// @brief Rewrite `CmpRI reg, 0` as `TstRR reg, reg`.
/// @param[in,out] instr Candidate instruction, rewritten on success.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @return `true` when @p instr was a well-formed comparison with zero.
[[nodiscard]] bool tryCmpZeroToTst(MInstr &instr, PeepholeStats &stats);

/// @brief Replace integer add, subtract, or shift identities with register moves.
/// @param[in,out] instr Candidate immediate-form instruction.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @return `true` when a supported operation had an immediate operand of zero.
[[nodiscard]] bool tryArithmeticIdentity(MInstr &instr, PeepholeStats &stats);

/// @brief Replace multiplication by a known positive power of two with `LslRI`.
/// @param[in,out] instr Candidate `MulRRR`, rewritten on success.
/// @param knownConsts Physical-GPR constants valid at @p instr.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @return `true` when either multiply input is a known power of two with a
///         shift in the AArch64 64-bit range.
[[nodiscard]] bool tryStrengthReduction(MInstr &instr,
                                        const RegConstMap &knownConsts,
                                        PeepholeStats &stats);

/// @brief Replace unsigned division by a known power of two with `LsrRI`.
/// @param[in,out] instr Candidate `UDivRRR`, rewritten on success.
/// @param knownConsts Physical-GPR constants valid at @p instr.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @return `true` when the divisor is a known positive power of two.
[[nodiscard]] bool tryDivStrengthReduction(MInstr &instr,
                                           const RegConstMap &knownConsts,
                                           PeepholeStats &stats);

/// @brief Apply multi-instruction unsigned division strength reduction.
///
/// Handles unsigned division by arbitrary non-power-of-2 constants using
/// `umulh` plus an optional correction/add sequence. Power-of-2 divisors stay
/// on the single-instruction path above so they can collapse directly to `lsr`.
///
/// @param[in,out] instrs Block-local instruction sequence to expand.
/// @param idx Index of the candidate `UDivRRR`.
/// @param knownConsts Physical-GPR constants valid at @p idx.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @param carriedExitRegs Optional sorted list of physical registers the
///        allocator carries live across the enclosing block's exit without any
///        in-block use (MBasicBlock::carriedExitRegs); clobber analysis treats
///        them as live at block end.
/// @return `true` when the divisor was suitable, the divisor register was dead,
///         scratch registers were available, and an expansion was installed.
[[nodiscard]] bool tryUDivStrengthReduction(std::vector<MInstr> &instrs,
                                            std::size_t idx,
                                            const RegConstMap &knownConsts,
                                            PeepholeStats &stats,
                                            const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Apply strength reduction: signed division by constant.
///
/// Handles division by `1`, `-1`, positive powers of two, and arbitrary signed
/// constants. General divisors use a signed multiply-high magic-number sequence
/// with quotient and divisor-sign corrections, preserving truncation toward
/// zero.
///
/// @param[in,out] instrs Block-local instruction sequence to expand.
/// @param idx Index of the candidate `SDivRRR`.
/// @param knownConsts Physical-GPR constants valid at @p idx.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @param carriedExitRegs Optional sorted physical-register identifiers carried
///        live through the block exit; such a divisor register cannot be reused.
/// @return `true` when the instruction was replaced by an equivalent move,
///         negate, shift/bias, or multiply-high sequence.
[[nodiscard]] bool trySDivStrengthReduction(std::vector<MInstr> &instrs,
                                            std::size_t idx,
                                            const RegConstMap &knownConsts,
                                            PeepholeStats &stats,
                                            const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Fuse [SU]DIV+MSUB remainder pattern into cheaper operations.
///
/// Matches: [SU]DivRRR tmp, lhs, rhs; MSubRRRR dst, tmp, rhs, lhs
/// where rhs is a known power-of-2 constant.
///
/// - UREM by 2^k: Replaced with AND dst, lhs, #(2^k - 1)
/// - SREM by 2^k: Replaced with sign-corrected AND+SUB sequence (5 instructions,
///   each 1 cycle vs ~22 cycles for SDIV + ~4 cycles for MSUB).
///
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param idx Index of the `[SU]DivRRR` candidate.
/// @param knownConsts Physical-GPR constants valid at @p idx.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @param carriedExitRegs Accepted for consistency with the other division
///        rewrites. The current remainder forms reuse the division result and
///        do not need to consult the live-through set.
/// @return `true` when the adjacent divide/remainder sequence was replaced.
[[nodiscard]] bool tryRemainderFusion(std::vector<MInstr> &instrs,
                                      std::size_t idx,
                                      const RegConstMap &knownConsts,
                                      PeepholeStats &stats,
                                      const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Fold a known right-hand operand of `AddRRR` or `SubRRR` into an immediate.
/// @param[in,out] instr Candidate register-form instruction.
/// @param knownConsts Physical-GPR constants valid at @p instr.
/// @param[in,out] stats Statistics updated when the rewrite succeeds.
/// @return `true` when the right operand is known in the unshifted 12-bit
///         immediate range `[0, 4095]`.
[[nodiscard]] bool tryImmediateFolding(MInstr &instr,
                                       const RegConstMap &knownConsts,
                                       PeepholeStats &stats);

/// @brief Placeholder for future floating-point identity rewrites.
/// @param[in,out] instr Candidate instruction; currently never modified.
/// @param[in,out] stats Statistics object; currently never modified.
/// @return Always `false`.
[[maybe_unused]] [[nodiscard]] bool tryFPArithmeticIdentity(MInstr &instr, PeepholeStats &stats);

} // namespace zanna::codegen::aarch64::peephole
