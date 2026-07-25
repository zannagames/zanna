//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/VState.hpp
// Purpose: Virtual register state tracking for the AArch64 linear-scan
//          register allocator.
//
// Key invariants:
//   - hasPhys is true iff the vreg currently occupies a physical register.
//   - dirty is true iff the register value is newer than the spill slot.
//
// Ownership/Lifetime:
//   - Owned by the allocator's state maps; one VState per active vreg.
//
// Links: codegen/aarch64/ra/Allocator.hpp,
//        codegen/aarch64/TargetAArch64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <climits>
#include <cstdint>

#include "codegen/aarch64/TargetAArch64.hpp"

/// @file
/// @brief Defines the mutable residency state of an allocated virtual register.

namespace zanna::codegen::aarch64::ra {

/// @brief Tracks the allocation state of a virtual register.
///
/// Maintains physical residency, backing spill-slot validity, and instruction
/// positions used by the allocator's LRU and furthest-next-use eviction
/// heuristics. `phys` is meaningful only while `hasPhys` is true; `fpOffset`
/// is meaningful only after a spill slot has been assigned.
struct VState {
    bool hasPhys{false};        ///< True if currently in a physical register.
    PhysReg phys{PhysReg::X0};  ///< Physical register (valid when hasPhys).
    bool spilled{false};        ///< True if value is on the stack.
    bool dirty{false};          ///< True if register value is newer than spill slot.
    int fpOffset{0};            ///< FP-relative offset of spill slot.
    unsigned lastUse{0};        ///< Instruction index of last use (for LRU).
    unsigned nextUse{UINT_MAX}; ///< Instruction index of next use (for furthest-end-point).
};

} // namespace zanna::codegen::aarch64::ra
