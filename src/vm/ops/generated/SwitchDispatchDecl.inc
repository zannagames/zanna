//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IMPORTANT: This file is auto-generated. Do not edit manually.
//
// File: vm/ops/generated/SwitchDispatchDecl.inc
// Purpose: Forward declaration for switch-based opcode dispatch function.
//
// Generator: Manually maintained (synchronized with Opcode enum).
// Regenerate: Update when adding/removing opcodes in il/core/Opcode.hpp.
//
// Links: docs/il/il-guide.md#reference, vm/DispatchStrategy.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the portable switch-based opcode dispatcher.
/// @param state Active execution state updated by the selected inline handler.
/// @param instr Current instruction whose opcode selects the switch case.
void dispatchOpcodeSwitch(ExecState &state, const il::core::Instr &instr);
