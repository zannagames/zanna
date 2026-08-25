//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/Noreturn.hpp
// Purpose: Shared predicates that classify a direct call to a runtime helper
//          that traps or terminates (never returns to its caller). Passes use
//          this to model such calls as no-return CFG exits so they can prune
//          dead instruction tails and avoid emitting unreachable epilogues.
// Key invariants:
//   - Only DIRECT calls are recognized: the instruction must be MOpcode::Bl
//     with a Label first operand. Indirect/register branches are never
//     classified as no-return here.
//   - The label is resolved through il::runtime::mapCanonicalRuntimeName
//     first, so platform-mangled or aliased names match the canonical set.
//   - The recognized set (rt_trap*, rt_arr_oob_panic, rt_trap_div0/ovf, ...)
//     must stay in sync with the runtime helpers that genuinely do not
//     return; adding a returning symbol here would prune live code.
// Ownership/Lifetime:
//   - Stateless inline predicates; no allocation, no retained state. The
//     MInstr / MBasicBlock arguments are caller-owned and read-only.
// Links: src/codegen/aarch64/MachineIR.hpp,
//        src/il/runtime/RuntimeNameMap.hpp,
//        src/codegen/aarch64/passes/LegalizePass.cpp (dead-tail pruning caller)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <string_view>

/**
 * @file
 * @brief Classifies direct MIR calls to non-returning runtime helpers.
 *
 * These predicates provide one conservative definition of a no-return call
 * for AArch64 passes that trim dead block tails or suppress unreachable
 * epilogues. Only a `Bl` whose first operand is a label can match. Runtime
 * aliases are canonicalized before the fixed symbol set is consulted.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Tests a canonical runtime symbol against the no-return helper set.
 *
 * @param symbol Runtime symbol spelling after any desired canonicalization.
 * @return `true` only for a helper known to trap or terminate execution.
 * @note This function performs an exact, case-sensitive comparison and does
 *       not canonicalize @p symbol itself.
 */
[[nodiscard]] inline bool isNoReturnRuntimeSymbol(std::string_view symbol) noexcept {
    return symbol == "rt_trap_ovf" || symbol == "rt_trap_div0" || symbol == "rt_trap_null" ||
           symbol == "rt_trap_raise_error" || symbol == "rt_trap_string" ||
           symbol == "rt_arr_oob_panic" || symbol == "rt_trap";
}

/**
 * @brief Tests whether an instruction directly calls a known no-return helper.
 *
 * The target label is first offered to `mapCanonicalRuntimeName`; if it has no
 * mapping, the raw spelling is checked. Indirect calls and malformed `Bl`
 * instructions are conservatively treated as returning.
 *
 * @param instr MIR instruction to classify without modification.
 * @return `true` when @p instr is a direct labeled call to a symbol accepted
 *         by `isNoReturnRuntimeSymbol()`.
 */
[[nodiscard]] inline bool isNoReturnCall(const MInstr &instr) {
    if (instr.opc != MOpcode::Bl || instr.ops.empty() || instr.ops[0].kind != MOperand::Kind::Label)
        return false;

    const std::string &raw = instr.ops[0].label;
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(raw))
        return isNoReturnRuntimeSymbol(*mapped);
    return isNoReturnRuntimeSymbol(raw);
}

/**
 * @brief Tests whether a block's final instruction is a no-return direct call.
 *
 * @param block MIR block to inspect.
 * @return `false` for an empty block; otherwise the classification of its last
 *         instruction from `isNoReturnCall()`.
 */
[[maybe_unused]] [[nodiscard]] inline bool endsInNoReturnCall(const MBasicBlock &block) {
    return !block.instrs.empty() && isNoReturnCall(block.instrs.back());
}

} // namespace zanna::codegen::aarch64
