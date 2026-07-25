//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/Coalescer.hpp
// Purpose: Pre-register-allocation move coalescer for AArch64 MIR.
//          Eliminates redundant MovRR/FMovRR instructions by merging virtual
//          registers whose live ranges do not interfere.
// Key invariants:
//   - Only operates on virtual register operands (isPhys == false).
//   - Does not modify physical register assignments or call conventions.
//   - Must run before register allocation (all operands still virtual).
// Ownership/Lifetime:
//   - Modifies MFunction in place; caller owns the MFunction.
// Links: src/codegen/aarch64/Coalescer.cpp,
//        src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/aarch64/RegAllocLinear.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares conservative virtual-register copy coalescing for AArch64 MIR.
 *
 * The pass runs before physical register allocation and rewrites only
 * same-class virtual-register handoff copies whose conservative live intervals
 * do not overlap beyond the move point.
 */

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

namespace zanna::codegen::aarch64 {

/// @brief Coalesce MovRR/FMovRR instructions between virtual registers.
///
/// Scans the MIR function for register-to-register moves between virtual
/// registers. When the source and destination do not have overlapping live
/// ranges, the two vregs are merged into one and the move is deleted.
///
/// This reduces register pressure and eliminates unnecessary copies before
/// the linear-scan register allocator runs.
///
/// @param fn The machine function to coalesce (modified in place).
/// @post Safe identity moves created by coalescing are removed.
void coalesce(MFunction &fn);

} // namespace zanna::codegen::aarch64
