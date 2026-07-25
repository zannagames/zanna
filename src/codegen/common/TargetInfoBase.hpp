//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/TargetInfoBase.hpp
// Purpose: Target-independent base for ABI target information structs.
// Key invariants: Common fields shared by all backend TargetInfo types; each
//                 backend extends this with target-specific extras.
// Ownership/Lifetime: Value type; each backend owns its own TargetInfo instance
//                     which is typically constructed once at pipeline startup.
// Links: codegen/x86_64/TargetInfo.hpp, codegen/aarch64/TargetInfo.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstddef>
#include <vector>

/// @file
/// @brief Defines ABI register sets common to all native target descriptions.

namespace zanna::codegen::common {

/// @brief Target-independent base struct for backend ABI descriptions.
/// @tparam PhysRegT   Backend-specific physical register enum type.
/// @tparam MaxIntArgs Maximum number of integer arguments passed in registers.
/// @tparam MaxFPArgs  Maximum number of floating-point arguments passed in registers.
template <typename PhysRegT, std::size_t MaxIntArgs, std::size_t MaxFPArgs> struct TargetInfoBase {
    /// @brief Caller-saved general purpose registers.
    std::vector<PhysRegT> callerSavedGPR{};
    /// @brief Callee-saved general purpose registers.
    std::vector<PhysRegT> calleeSavedGPR{};
    /// @brief Caller-saved floating-point registers.
    std::vector<PhysRegT> callerSavedFPR{};
    /// @brief Callee-saved floating-point registers.
    std::vector<PhysRegT> calleeSavedFPR{};
    /// @brief ABI argument order for integer and pointer values.
    std::array<PhysRegT, MaxIntArgs> intArgOrder{};
    /// @brief ABI argument order for 64-bit floating-point values.
    std::array<PhysRegT, MaxFPArgs> f64ArgOrder{};
    /// @brief Register used to return integer and pointer values.
    PhysRegT intReturnReg{};
    /// @brief Register used to return 64-bit floating-point values.
    PhysRegT f64ReturnReg{};
    /// @brief Required stack alignment at call boundaries (bytes).
    unsigned stackAlignment{16U};
};

} // namespace zanna::codegen::common
