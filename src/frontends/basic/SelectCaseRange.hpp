//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file SelectCaseRange.hpp
/// @brief Shared helpers for SELECT CASE range validation.
/// @details Centralises the 32-bit integer bounds used by both semantic analysis
///          and lowering so that CASE label diagnostics remain consistent. The
///          utilities are header-only and carry no mutable state.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace il::frontends::basic {

/// @brief Minimum legal integer value for SELECT CASE labels.
/// @details Matches the signed 32-bit lower bound enforced by the BASIC frontend.
inline constexpr int64_t kCaseLabelMin = static_cast<int64_t>(std::numeric_limits<int32_t>::min());

/// @brief Maximum legal integer value for SELECT CASE labels.
/// @details Matches the signed 32-bit upper bound enforced by the BASIC frontend.
inline constexpr int64_t kCaseLabelMax = static_cast<int64_t>(std::numeric_limits<int32_t>::max());

/// @brief Format the standard out-of-range diagnostic for a CASE label.
/// @details Produces a human-readable message that reports the raw label value
///          and states that it exceeds the 32-bit signed range enforced by the
///          frontend. Both semantic analysis and lowering use this to keep
///          diagnostics consistent.
/// @param raw The original CASE label value before range checking.
/// @return A formatted message suitable for diagnostic emission.
/// @note This helper formats the message only; it does not perform the range
///       check or emit a diagnostic.
inline std::string makeSelectCaseLabelRangeMessage(int64_t raw) {
    std::string msg = "CASE label ";
    msg += std::to_string(raw);
    msg += " is outside 32-bit signed range";
    return msg;
}

} // namespace il::frontends::basic
