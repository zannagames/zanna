//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/io/FormatUtils.hpp
// Purpose: Declare formatting helpers used by IL-level utilities and frontends.
// Key invariants: Formatting remains locale-independent and matches runtime
//                 conventions for special values.
// Ownership/Lifetime: Returned strings own their storage and are independent
//                     of the helpers.
// License: GPL-3.0-only (see LICENSE).
// Links: docs/il/il-guide.md#reference, src/il/io/FormatUtils.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/il/io/FormatUtils.hpp
 * @brief Declares canonical, locale-independent formatting for IL numbers.
 *
 * @details
 * These helpers centralize the textual representation used when numeric values
 * cross IL and frontend boundaries. Returned strings own their storage.
 * Integral values use signed decimal notation; floating-point values preserve
 * enough precision for a `double` round trip and use stable spellings for NaN
 * and infinities.
 */

#pragma once

#include <cstdint>
#include <string>

namespace zanna::il::io
{

/**
 * @brief Format a signed 64-bit integer in canonical decimal notation.
 *
 * @param value Value to convert to text.
 * @return An owning, locale-independent decimal string with a leading minus
 *         sign only when `value` is negative.
 */
[[nodiscard]] std::string format_integer(std::int64_t value);

/**
 * @brief Format a double-precision value using the canonical IL spelling.
 *
 * Finite values use the classic C locale, `defaultfloat`, and `max_digits10`
 * precision. NaN is rendered as `NaN`; positive and negative infinity are
 * rendered as `Inf` and `-Inf`, respectively.
 *
 * @param value Floating-point value to convert to text.
 * @return An owning, locale-independent representation that preserves finite
 *         round-trip fidelity.
 *
 * @note NaN payload bits and sign are intentionally not preserved in text.
 */
[[nodiscard]] std::string format_float(double value);

} // namespace zanna::il::io
