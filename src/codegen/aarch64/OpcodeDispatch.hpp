//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/OpcodeDispatch.hpp
// Purpose: Instruction lowering dispatch for IL->MIR conversion.
// Key invariants: Returns true when the opcode is handled, false otherwise;
//                 unhandled opcodes must be processed by the caller;
//                 block indices are used instead of retaining references
//                 across helper calls that may mutate function-level state.
// Ownership/Lifetime: Stateless free function; mutable state is accessed
//                     through the LoweringContext reference.
// Links: src/codegen/aarch64/InstrLowering.hpp,
//        src/codegen/aarch64/LoweringContext.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "LoweringContext.hpp"
#include "MachineIR.hpp"
#include "il/core/Instr.hpp"

/**
 * @file
 * @brief Declares the central IL-opcode dispatcher for AArch64 instruction lowering.
 *
 * The dispatcher owns opcodes that can be lowered independently within an
 * output block and forwards specialized cases to `InstrLowering`. A `false`
 * result is a protocol signal to `LowerILToMIR`, not necessarily an error.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Lowers one IL instruction through the AArch64 opcode dispatch table.
 *
 * Terminators and frame-only markers may be accepted without immediately
 * emitting MIR because later lowering stages own them. Unsupported opcodes and
 * malformed instances of conditionally handled opcodes return `false` so the
 * caller can apply its own handling or diagnostic policy.
 *
 * @param ins IL instruction to dispatch.
 * @param bbIn Source IL block containing @p ins; used to resolve producers and
 *        block-local values.
 * @param[in,out] ctx Function-wide lowering state, maps, frame, and output MIR.
 * @param bbOutIdx Index of the destination block in `ctx.mf.blocks`.
 * @return `true` if this layer accepted @p ins, including deferred/no-emission
 *         cases; `false` if the caller must handle it.
 * @pre `bbOutIdx < ctx.mf.blocks.size()`.
 * @throws std::runtime_error If a handled instruction violates a required
 *         operand/type contract or virtual-register allocation fails.
 */
bool lowerInstruction(const il::core::Instr &ins,
                      const il::core::BasicBlock &bbIn,
                      LoweringContext &ctx,
                      std::size_t bbOutIdx);

} // namespace zanna::codegen::aarch64
