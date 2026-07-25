//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/LiveIntervals.hpp
// Purpose: Declare function-wide first/last-touch interval construction used
//          by the linear-scan allocator.
// Key invariants:
//   - Intervals are half-open instruction index ranges relative to per-function numbering.
//   - Analysis is deterministic and does not mutate the input MIR.
// Ownership/Lifetime:
//   - Interval data is stored in value-owned containers valid for the lifetime of the instance.
// Links: src/codegen/x86_64/ra/LiveIntervals.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

/// @file
/// @brief Declares function-wide virtual-register touch intervals.

namespace zanna::codegen::x64::ra {

/// @brief Sentinel value indicating an uninitialised virtual register slot.
inline constexpr uint16_t kInvalidVReg = UINT16_MAX;

/// @brief Half-open interval describing the lifetime of a virtual register.
/// @invariant `start` <= `end` and both are measured in instruction indices.
struct LiveInterval {
    /// @brief Virtual-register id, or @ref kInvalidVReg before initialization.
    uint16_t vreg{kInvalidVReg}; ///< Virtual register identifier (kInvalidVReg if uninitialised).
    /// @brief Register bank captured from the first occurrence.
    RegClass cls{RegClass::GPR}; ///< Register class constraining allocation.
    /// @brief Global function instruction index of the first occurrence.
    std::size_t start{0U};       ///< Index of the first instruction touching the vreg.
    /// @brief One past the global function instruction index of the last occurrence.
    std::size_t end{0U};         ///< Index just past the last instruction touching the vreg.
};

/// @brief Owns first/last-touch intervals for one analyzed machine function.
/// @details This lightweight analysis ignores CFG paths and operand roles; it
///          spans every explicit virtual register occurrence, including memory
///          base and index registers, in block layout order.
class LiveIntervals {
  public:
    /// @brief Constructs an empty analysis with no observed intervals.
    LiveIntervals() = default;

    /// @brief Computes first/last-touch intervals for @p func.
    /// @details Clears previous results and assigns one monotonically increasing
    ///          instruction index across all blocks in layout order.
    /// @param func Immutable machine function to scan.
    void run(const MFunction &func);

    /// @brief Retrieve the interval for a virtual register if it was observed.
    /// @param vreg Virtual-register identifier to query.
    /// @return Borrowed interval pointer, or @c nullptr when absent.
    /// @note The pointer is invalidated by the next @ref run or destruction.
    [[nodiscard]] const LiveInterval *lookup(uint16_t vreg) const noexcept;

  private:
    /// @brief Owned intervals keyed by virtual-register id.
    std::unordered_map<uint16_t, LiveInterval> intervals_{};
};

} // namespace zanna::codegen::x64::ra
