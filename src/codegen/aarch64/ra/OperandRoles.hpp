//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/OperandRoles.hpp
// Purpose: Determines whether each operand of an MIR instruction is a use,
//          a def, or both, for register allocation purposes.
//
// Key invariants:
//   - Must cover every MOpcode that has register operands.
//   - Returns {isUse, isDef} for the operand at position idx.
//
// Ownership/Lifetime:
//   - Stateless free function; no ownership.
//
// Links: codegen/aarch64/ra/OperandRoles.cpp,
//        codegen/aarch64/ra/Allocator.cpp,
//        codegen/aarch64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <utility>

#include "codegen/aarch64/MachineIR.hpp"

/// @file
/// @brief Declares authoritative explicit operand roles for AArch64 MIR.

namespace zanna::codegen::aarch64::ra {

/// @brief Determine the use/def roles of operand @p idx in instruction @p ins.
/// @details Routes through the opcode-class predicates in `OpcodeClassify.hpp`
///          plus explicit handling for irregular shapes such as pair accesses,
///          jump tables, phi stores, and conditional selects. The allocator and
///          scheduler use this result for liveness and dependency construction.
/// @param ins Machine instruction whose operand is being classified.
/// @param idx Zero-based index into `ins.ops`.
/// @return `{isUse, isDef}` pair indicating whether the operand is read, written, or both.
/// @throws std::logic_error if @p idx names a register operand of an opcode
///         whose role has not been classified. Non-register operands return
///         `{false, false}`.
std::pair<bool, bool> operandRoles(const MInstr &ins, std::size_t idx);

} // namespace zanna::codegen::aarch64::ra
