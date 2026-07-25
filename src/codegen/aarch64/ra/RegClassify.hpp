//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/RegClassify.hpp
// Purpose: Register classification predicates for the AArch64 register
//          allocator (allocatable GPR test, argument register test).
//
// Key invariants:
//   - isAllocatableGPR excludes X29, X30, SP, X18, and X9/X16/X17 (scratch).
//   - isArgRegister checks both integer and FP argument orders.
//
// Ownership/Lifetime:
//   - Stateless free functions; no ownership.
//
// Links: codegen/aarch64/ra/RegPools.cpp,
//        codegen/aarch64/TargetAArch64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/TargetAArch64.hpp"

/// @file
/// @brief Defines physical-register eligibility and ABI argument queries.

namespace zanna::codegen::aarch64::ra {

/// @brief Test whether a physical GPR may hold an allocated virtual register.
/// @details Excludes the frame pointer, link register, stack pointer,
///          platform-reserved X18, and global scratch registers X9/X16/X17.
///          FPRs and invalid enum values also return false.
/// @param r Physical register to classify.
/// @return `true` only for an unreserved GPR.
inline bool isAllocatableGPR(PhysReg r) {
    switch (r) {
        case PhysReg::X29:
        case PhysReg::X30:
        case PhysReg::SP:
        case PhysReg::X18:
        // Reserve the global scratch registers so the allocator never hands them out.
        case PhysReg::X9:
        case PhysReg::X16:
        case PhysReg::X17:
            return false;
        default:
            return isGPR(r);
    }
}

/// @brief Test whether a physical register belongs to either ABI argument sequence.
/// @param r Physical register to query.
/// @param ti Target description containing ordered integer and FP argument sets.
/// @return `true` when @p r occurs in `intArgOrder` or `f64ArgOrder`.
inline bool isArgRegister(PhysReg r, const TargetInfo &ti) {
    for (auto ar : ti.intArgOrder)
        if (ar == r)
            return true;
    for (auto ar : ti.f64ArgOrder)
        if (ar == r)
            return true;
    return false;
}

} // namespace zanna::codegen::aarch64::ra
