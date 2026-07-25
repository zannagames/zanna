//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/InstrBuilders.hpp
// Purpose: Convenience factories for common MIR instructions emitted by
//          the AArch64 register allocator (MovRR, LdrFp, StrFp).
//
// Key invariants:
//   - Each builder returns a fully formed MInstr with correct opcode/operands.
//
// Ownership/Lifetime:
//   - Stateless free functions; returned MInstr is owned by caller.
//
// Links: codegen/aarch64/ra/Allocator.cpp,
//        codegen/aarch64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

/// @file
/// @brief Provides small MIR factories used while inserting allocator code.

namespace zanna::codegen::aarch64::ra {

/// @brief Build a GPR-to-GPR register move: `mov dst, src`.
/// @param dst Physical GPR written by the move.
/// @param src Physical GPR read by the move.
/// @return A `MovRR` with destination and source register operands.
[[maybe_unused]] inline MInstr makeMovRR(PhysReg dst, PhysReg src) {
    return MInstr{MOpcode::MovRR, {MOperand::regOp(dst), MOperand::regOp(src)}};
}

/// @brief Build a GPR load from a frame-pointer-relative slot: `ldr dst, [fp, #offset]`.
/// @param dst Physical GPR receiving the loaded value.
/// @param offset Signed byte displacement from the frame pointer.
/// @return An `LdrRegFpImm` with register and immediate operands.
inline MInstr makeLdrFp(PhysReg dst, int offset) {
    return MInstr{MOpcode::LdrRegFpImm, {MOperand::regOp(dst), MOperand::immOp(offset)}};
}

/// @brief Build a GPR store to a frame-pointer-relative slot: `str src, [fp, #offset]`.
/// @param src Physical GPR whose value is stored.
/// @param offset Signed byte displacement from the frame pointer.
/// @return A `StrRegFpImm` with register and immediate operands.
inline MInstr makeStrFp(PhysReg src, int offset) {
    return MInstr{MOpcode::StrRegFpImm, {MOperand::regOp(src), MOperand::immOp(offset)}};
}

} // namespace zanna::codegen::aarch64::ra
