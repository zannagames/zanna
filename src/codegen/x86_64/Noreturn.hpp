//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Noreturn.hpp
// Purpose: Classifies a direct x86-64 MIR call to a runtime helper that never
//          returns, using the backend-neutral symbol set.
// Key invariants:
//   - Only direct calls are recognized: the instruction must be MOpcode::CALL
//     with an OpLabel first operand.
// Ownership/Lifetime:
//   - Stateless inline predicates.
// Links: src/codegen/common/NoReturnSymbols.hpp,
//        src/codegen/x86_64/Lowering.Mem.cpp (emits UD2 after such calls)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/NoReturnSymbols.hpp"
#include "codegen/x86_64/MachineIR.hpp"

#include <variant>

/// @file
/// @brief Classifies direct x86-64 MIR calls to non-returning runtime helpers.

namespace zanna::codegen::x64 {

/// @brief Tests whether @p instr directly calls a known no-return helper.
/// @param instr MIR instruction to classify without modification.
/// @return `true` for a `CALL` whose label names a helper accepted by
///         common::isNoReturnRuntimeCallee().
[[nodiscard]] inline bool isNoReturnCall(const MInstr &instr) {
    if (instr.opcode != MOpcode::CALL || instr.operands.empty())
        return false;
    const auto *label = std::get_if<OpLabel>(&instr.operands.front());
    return label != nullptr && common::isNoReturnRuntimeCallee(label->name);
}

} // namespace zanna::codegen::x64
