//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/RegAllocLinear.hpp
// Purpose: Declare the linear-scan register allocator facade for x86-64.
// Key invariants:
//   - Live intervals are computed before allocation.
//   - Spill slots are assigned monotonically per register class.
//   - Assigned virtual registers are rewritten; spills use class-specific slots.
// Ownership/Lifetime:
//   - Functions operate directly on the supplied MFunction; return results by value.
//   - No persistent allocator state between calls.
// Links: src/codegen/x86_64/RegAllocLinear.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetX64.hpp"

#include <unordered_map>

/**
 * @file
 * @brief Declares the x86-64 facade over live intervals and linear-scan allocation.
 */

namespace zanna::codegen::x64 {

/**
 * @brief Summary of physical assignments and spill storage from allocation.
 *
 * The slot counts are expressed in backend eight-byte spill units and are
 * consumed by frame layout.
 */
struct AllocationResult {
    std::unordered_map<uint16_t, PhysReg> vregToPhys; ///< Final vreg → phys mapping.
    int spillSlotsGPR{0};                             ///< Number of 8-byte GPR spill slots.
    int spillSlotsXMM{0};                             ///< Number of 8-byte XMM spill slots.
};

/// @brief Allocate registers for @p func using the x86-64 linear-scan pipeline.
///
/// @details Computes live intervals for every virtual register, then walks
///          them in start-point order assigning physical registers from the
///          available pool. When no register is free the interval with the
///          longest remaining range is spilled. After allocation, all virtual
///          register references in @p func are rewritten to their assigned
///          physical registers.
///
/// @param func   The machine function whose virtual registers are allocated.
///               Modified in-place with spill/reload instructions as needed.
/// @param target ABI metadata providing allocatable register sets and
///               callee-saved information.
/// @return An AllocationResult containing the vreg-to-phys mapping and the
///         number of spill slots consumed per register class.
/// @throws std::exception Propagates liveness, allocation, or rewrite failures.
[[nodiscard]] AllocationResult allocate(MFunction &func, const TargetInfo &target);

} // namespace zanna::codegen::x64
