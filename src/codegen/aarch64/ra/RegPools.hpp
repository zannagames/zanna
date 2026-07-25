//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/RegPools.hpp
// Purpose: Physical register pool management for the AArch64 register
//          allocator. Maintains free-lists for GPR and FPR classes and
//          tracks callee-saved register usage.
//
// Key invariants:
//   - build() must be called before any take/release operations.
//   - Callee-saved usage arrays are indexed by PhysReg ordinal.
//   - GPR pool never hands out X9/X16/X17 (global scratch), X18, X29, X30, or SP.
//
// Ownership/Lifetime:
//   - Owned by the LinearAllocator; one RegPools per allocation run.
//
// Links: codegen/aarch64/ra/RegPools.cpp,
//        codegen/aarch64/ra/Allocator.hpp,
//        codegen/aarch64/TargetAArch64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <deque>

#include "codegen/aarch64/TargetAArch64.hpp"

/// @file
/// @brief Declares per-function free pools for AArch64 physical registers.

namespace zanna::codegen::aarch64::ra {

/// @brief Physical register free-lists and callee-saved usage tracking for
///        one function's register allocation run.
///
/// `build()` admits registers in ABI caller-saved then callee-saved order while
/// honoring global reservations and per-function exclusions. Take operations
/// remove one register from the appropriate deque; release operations validate
/// eligibility and reject duplicate returns.
struct RegPools {
    std::deque<PhysReg> gprFree{}; ///< Available allocatable GPRs.
    std::deque<PhysReg> fprFree{}; ///< Available allocatable FPRs.

    /// Which callee-saved GPRs were actually used in this function.
    /// Indexed by static_cast<std::size_t>(PhysReg); AArch64 has 64 PhysReg values.
    std::array<bool, 64> calleeUsed{};
    /// Which callee-saved FPRs were actually used in this function.
    std::array<bool, 64> calleeUsedFPR{};

    /// Pre-computed set of callee-saved GPRs for O(1) lookup in takeGPRPreferCalleeSaved().
    /// Stored as a dense bool array (indexed by PhysReg ID) for cache efficiency.
    std::array<bool, 64> calleeSavedGPRSet{};
    /// Pre-computed set of callee-saved FPRs for O(1) lookup in takeFPRPreferCalleeSaved().
    std::array<bool, 64> calleeSavedFPRSet{};

    /// Registers eligible for this function's pools. Release validation is
    /// membership-based so argument registers (allocatable unless they are
    /// ABI live-ins for the function) round-trip correctly.
    std::array<bool, 64> poolEligible{};

    /// @brief Initialize free lists from target info.
    /// @param ti Target ABI description.
    /// @param excluded Per-function exclusions (e.g. ABI live-in argument
    ///        registers whose incoming values are read before being defined);
    ///        indexed by PhysReg ordinal.
    /// @post Free lists and usage sets describe only @p ti and @p excluded;
    ///       all state from a prior allocation run is discarded.
    void build(const TargetInfo &ti, const std::array<bool, 64> &excluded = {});

    /// @brief Take an available GPR, preferring caller-saved registers.
    /// @return Removed physical GPR.
    /// @throws std::runtime_error if no GPR is free.
    PhysReg takeGPR();

    /// @brief Take a GPR, preferring callee-saved registers.
    /// @param ti Target description retained for API symmetry; callee-saved
    ///           membership was cached by @ref build.
    /// @return Removed physical GPR.
    /// @throws std::runtime_error if no GPR is free.
    PhysReg takeGPRPreferCalleeSaved(const TargetInfo &ti);

    /// @brief Release a GPR back to the free pool.
    /// @param r Physical GPR previously taken from this pool.
    /// @param ti Target description retained for API symmetry.
    /// @throws std::runtime_error if @p r was not eligible for this function or
    ///         is already free.
    void releaseGPR(PhysReg r, const TargetInfo &ti);

    /// @brief Take the next available FPR in pool order.
    /// @return Removed physical FPR.
    /// @throws std::runtime_error if no FPR is free.
    PhysReg takeFPR();

    /// @brief Take an FPR, preferring callee-saved registers.
    /// @param ti Target description retained for API symmetry; callee-saved
    ///           membership was cached by @ref build.
    /// @return Removed physical FPR.
    /// @throws std::runtime_error if no FPR is free.
    PhysReg takeFPRPreferCalleeSaved(const TargetInfo &ti);

    /// @brief Release an FPR back to the free pool.
    /// @param r Physical FPR previously taken from this pool.
    /// @param ti Target description retained for API symmetry.
    /// @throws std::runtime_error if @p r is not an eligible FPR or is already free.
    void releaseFPR(PhysReg r, const TargetInfo &ti);
};

} // namespace zanna::codegen::aarch64::ra
