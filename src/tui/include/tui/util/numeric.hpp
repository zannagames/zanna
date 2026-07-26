//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares saturating arithmetic for safe TUI index calculations.
/// @details Provides stateless inline operations whose results clamp at type
///          limits rather than wrapping.
// Key invariants: Arithmetic never overflows; results saturate at type limits.
// Ownership/Lifetime: Stateless inline utilities with no dynamic resources.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <limits>

namespace zanna::tui::util {

/// @brief Saturating addition for size_t values.
/// @details Returns the sum of @p base and @p delta when representable,
///          otherwise clamps to the maximum value for std::size_t.  Useful
///          for computing buffer offsets without risking integer overflow.
/// @param base Starting value.
/// @param delta Amount to add to @p base.
/// @return @p base + @p delta when representable, otherwise the saturated limit.
[[nodiscard]] inline std::size_t clampAdd(std::size_t base, std::size_t delta) {
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (max - base < delta)
        return max;
    return base + delta;
}

} // namespace zanna::tui::util
