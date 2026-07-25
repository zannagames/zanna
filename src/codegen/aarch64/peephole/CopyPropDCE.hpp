//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/CopyPropDCE.hpp
// Purpose: Declarations for copy propagation, dead code elimination, dead FP
//          store elimination, dead flag-setter removal, and compute-into-target
//          folding peephole sub-passes.
//
// Key invariants:
//   - Copy origins are not chased through ABI registers; ABI uses are rewritten
//     only from non-ABI origins and can be disabled through the debug override.
//   - DCE conservatively marks callee-saved and ABI registers as live at exit.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "../Peephole.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares copy propagation and dead-code/store cleanup for AArch64 MIR.

namespace zanna::codegen::aarch64::peephole {

/// @brief Propagate same-class physical-register copies within one block.
///
/// Tracks `MovRR` and `FMovRR` origins until a definition or control boundary
/// invalidates them. Uses of ABI registers may be forwarded only from non-ABI
/// origins, and no origin chain is followed through an ABI register.
///
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param[in,out] stats Statistics receiving the number of rewritten operands.
/// @return Number of copy-source operands and ordinary uses rewritten.
std::size_t propagateCopies(std::vector<MInstr> &instrs, PeepholeStats &stats);

/// @brief Remove side-effect-free definitions dead within one basic block.
///
/// Performs a conservative backward physical-register liveness scan seeded
/// with ABI argument registers, callee-saved GPRs, and any allocator-provided
/// live-through registers.
///
/// @param[in,out] instrs Block-local instruction sequence to compact.
/// @param[in,out] stats Statistics receiving the removal count.
/// @param carriedExitRegs Optional sorted list of physical registers carried
///        live across the enclosing block's exit without any in-block use
///        (MBasicBlock::carriedExitRegs); seeded into the live-at-exit set.
/// @return Number of instructions removed.
std::size_t removeDeadInstructions(std::vector<MInstr> &instrs,
                                   PeepholeStats &stats,
                                   const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Perform whole-function, CFG-aware physical-register DCE.
///
/// Builds successor edges from MIR branches and layout fallthrough, solves the
/// standard live-in/live-out equations, and removes non-effectful definitions
/// that are dead at their program point. Calls and returns include their
/// implicit ABI uses and clobbers.
///
/// @param[in,out] fn Function whose block instructions may be removed.
/// @param[in,out] stats Statistics receiving the removal count.
/// @param target Target ABI used for implicit call, return, and function-exit
///               register liveness.
/// @return Number of instructions removed.
std::size_t removeDeadInstructionsCFG(MFunction &fn,
                                      PeepholeStats &stats,
                                      const TargetInfo &target);

/// @brief Remove overwritten stores to identical FP-relative slots in one block.
///
/// Only scalar eight-byte FP-relative stores become removal candidates. Reads,
/// paired accesses, arbitrary-base memory, and control boundaries invalidate
/// affected facts conservatively.
///
/// @param[in,out] instrs Block-local instruction sequence to compact.
/// @param[in,out] stats Statistics receiving the removal count.
/// @return Number of overwritten stores removed.
std::size_t eliminateDeadFpStores(std::vector<MInstr> &instrs, PeepholeStats &stats);

/// @brief Remove dead comparisons whose NZCV value is overwritten before use.
///
/// @param[in,out] instrs Block-local instruction sequence to compact.
/// @param[in,out] stats Statistics receiving the removal count.
/// @return Number of comparison instructions removed.
std::size_t removeDeadFlagSetters(std::vector<MInstr> &instrs, PeepholeStats &stats);

/// @brief Fold compute-then-move patterns where an ALU result is immediately
///        moved to another register and the original destination is dead.
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param[in,out] stats Statistics receiving the erased-move count.
/// @return Number of ALU destinations redirected to their move targets.
std::size_t foldComputeIntoTarget(std::vector<MInstr> &instrs, PeepholeStats &stats);

/// @brief Remove stores to compiler spill slots never loaded in the function.
///
/// Cross-block analysis first collects every directly loaded FP offset, then
/// removes scalar or paired stores only when all covered offsets belong to
/// compiler-created spill slots and none is loaded. User locals are excluded
/// because address-derived base-register loads can observe them indirectly.
///
/// @param[in,out] fn Function whose spill stores may be removed.
/// @param[in,out] stats Statistics receiving the removal count.
/// @return Number of scalar or pair store instructions removed.
std::size_t eliminateDeadFpStoresCrossBlock(MFunction &fn, PeepholeStats &stats);

} // namespace zanna::codegen::aarch64::peephole
