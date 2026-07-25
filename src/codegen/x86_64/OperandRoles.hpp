//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/OperandRoles.hpp
// Purpose: Shared x86-64 Machine IR operand role classification.
// Key invariants:
//   - operandRoles() covers all defined MIR opcodes deterministically.
//   - EFlags queries are consistent with the x86-64 instruction set.
// Ownership/Lifetime:
//   - Stateless free functions; no dynamic allocation or persistent state.
// Links: src/codegen/x86_64/OperandRoles.cpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/MachineIR.hpp"

#include <cstddef>
#include <utility>

/**
 * @file
 * @brief Declares the canonical implicit and explicit role model for x86-64 MIR.
 *
 * Dataflow passes query explicit operand use/definition roles through one
 * opcode-complete classifier. Separate predicates model EFLAGS reads/writes and
 * conservatively identify instructions that dead-code elimination must retain.
 */

namespace zanna::codegen::x64 {

/// @brief Return `{isUse, isDef}` for an operand of an x86-64 MIR instruction.
/// @details Used by liveness, DCE, and the register allocator to classify every
///          operand of every MIR opcode without re-encoding the rules per pass.
/// @param instr Machine instruction whose operand is being classified.
/// @param idx   Zero-based index into @c instr.operands.
/// @return Pair indicating whether the operand is read, written, or both.
[[nodiscard]] std::pair<bool, bool> operandRoles(const MInstr &instr, std::size_t idx) noexcept;

/// @brief Test whether @p opcode reads the x86 EFLAGS register.
/// @details The current MIR readers are JCC, SETcc, and CMOVNErr. Fold-safety
///          checks use this result to retain an observable flag producer.
/// @param opcode Opcode to classify.
/// @return True if @p opcode reads EFLAGS.
[[nodiscard]] bool usesEFlags(MOpcode opcode) noexcept;

/// @brief Test whether @p opcode writes the x86 EFLAGS register.
/// @details Flag-writing instructions include all arithmetic/logical forms
///          (`ADD`, `SUB`, `AND`, etc.), the compare family, and the test family.
///          Used by liveness to model flags as an implicit def.
/// @param opcode Opcode to classify.
/// @return True if @p opcode writes EFLAGS.
[[nodiscard]] bool definesEFlags(MOpcode opcode) noexcept;

/// @brief Test whether @p opcode has observable side effects (memory or otherwise).
/// @details Returns true for stores, calls, traps, and any opcode that escapes the
///          virtual-register model. DCE consults this predicate to refuse to delete
///          such instructions even when their result is unused.
/// @param opcode Opcode to classify.
/// @return True if @p opcode has observable side effects.
[[nodiscard]] bool hasObservableSideEffects(MOpcode opcode) noexcept;

} // namespace zanna::codegen::x64
