//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/ArithSimplify.hpp
// Purpose: Declarations for arithmetic simplification peephole sub-passes:
//          MOV-zero to XOR, CMP-zero to TEST, arithmetic identities, and
//          strength reduction (multiply by power-of-2 to shift).
// Key invariants:
//   - Rewrites preserve semantic equivalence under the x86-64 ISA.
//   - Flag-clobbering rewrites (XOR, TEST) check for downstream flag readers.
// Ownership/Lifetime:
//   - Operates on mutable instructions owned by the caller.
// Links: src/codegen/x86_64/peephole/ArithSimplify.cpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp,
//        src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "PeepholeCommon.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares local post-allocation arithmetic simplifications.

namespace zanna::codegen::x64::peephole {

/// @brief Rewrite a MOV immediate-to-register into XOR to synthesize zero.
/// @details Replaces the opcode and operand list with 32-bit self-XOR, which
///          zero-extends the physical destination to 64 bits.
/// @param instr Candidate instruction replaced in place.
/// @param regOperand Physical destination operand copied into both source and destination roles.
/// @pre The caller has established that changing EFLAGS is unobservable.
/// @post @p instr is @c XORrr32 with two copies of @p regOperand.
void rewriteToXor(MInstr &instr, Operand regOperand);

/// @brief Convert a compare-against-zero into a register self-test.
/// @param instr Candidate compare replaced in place.
/// @param regOperand Physical register copied into both @c TEST operands.
/// @post @p instr is @c TESTrr with two copies of @p regOperand.
void rewriteToTest(MInstr &instr, Operand regOperand);

/// @brief Rewrite arithmetic identity operations (add #0, or #0, xor #0, and #-1, shift #0).
/// @details Recognizes only the listed two-operand immediate forms and rejects
///          removal when a later instruction observes their EFLAGS definition.
///          The instruction vector itself is not modified.
/// @param instrs Basic-block instruction sequence to inspect.
/// @param idx In-range index of the candidate instruction.
/// @param stats Statistics incremented on a removable identity.
/// @return @c true when the caller should omit @c instrs[idx]; otherwise @c false.
/// @pre @p idx is less than @c instrs.size().
[[nodiscard]] bool tryArithmeticIdentity(const std::vector<MInstr> &instrs,
                                         std::size_t idx,
                                         PeepholeStats &stats);

/// @brief Apply strength reduction: mul by power-of-2 -> shift left.
/// @details Rewrites a two-register @c IMULrr when its source physical
///          register is known to hold a positive power of two and no later
///          instruction observes the multiply's EFLAGS.
/// @param instrs Basic-block instruction sequence mutated in place.
/// @param idx In-range index of the candidate instruction.
/// @param knownConsts Physical-register constant facts valid at @p idx.
/// @param stats Statistics incremented when a rewrite occurs.
/// @return @c true after replacing the multiply with @c SHLri; otherwise @c false.
/// @pre @p idx is less than @c instrs.size().
[[nodiscard]] bool tryStrengthReduction(std::vector<MInstr> &instrs,
                                        std::size_t idx,
                                        const RegConstMap &knownConsts,
                                        PeepholeStats &stats);

} // namespace zanna::codegen::x64::peephole
