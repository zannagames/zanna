//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/RegAllocLinear.hpp
// Purpose: Linear-scan register allocator entry point for AArch64 Machine IR.
//          Maps virtual registers to physical registers, inserting spill/reload
//          code when register pressure exceeds available registers.
//
// Key invariants:
//   - After allocation, all MReg operands have isPhys=true.
//   - Spill slots are allocated in the function's frame layout.
//   - Callee-saved registers are tracked for prologue/epilogue generation.
//
// Ownership/Lifetime:
//   - Modifies the MFunction in place; caller owns the MFunction.
//   - Uses TargetInfo for available registers and calling convention.
//
// Links: src/codegen/aarch64/RegAllocLinear.cpp,
//        src/codegen/aarch64/ra/Allocator.hpp,
//        src/codegen/aarch64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetAArch64.hpp"

/**
 * @file
 * @brief Declares the public AArch64 linear-scan register-allocation entry point.
 *
 * This interface separates the code-generation pipeline from the allocator's
 * internal liveness, interval, assignment, and spill-rewrite machinery under
 * `codegen/aarch64/ra`.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Carries statistics produced by AArch64 register allocation.
 *
 * The current `LinearAllocator` returns the default value; the spill-slot field
 * is retained as the public result shape for allocators that report it.
 */
struct AllocationResult {
    int gprSpillSlots{0}; ///< Reported GPR spill slots; currently left at zero.
};

/**
 * @brief Performs linear-scan register allocation on a machine function.
 *
 * The allocator computes live intervals, assigns target registers, inserts
 * spill/reload MIR as necessary, and records used callee-saved registers and
 * frame spill slots.
 *
 * @param[in,out] fn Pre-allocation MIR function to rewrite in place.
 * @param ti Target register sets and calling-convention information.
 * @return Allocation statistics supplied by `ra::LinearAllocator::run()`.
 * @post Every register operand in @p fn names a physical register.
 * @post `fn.savedGPRs`, `fn.savedFPRs`, and frame spill metadata reflect the
 *       completed assignment.
 */
[[nodiscard]] AllocationResult allocate(MFunction &fn, const TargetInfo &ti);

} // namespace zanna::codegen::aarch64
