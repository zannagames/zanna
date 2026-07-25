//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/NumberParsing.hpp
// Purpose: Common number parsing utilities for language frontends.
// Key invariants:
//   * Successful parses consume the complete input.
//   * Decimal floating-point results must be finite.
//   * The magnitude 2^63 is represented specially so a parser can accept
//     `-9223372036854775808` without overflowing its positive token.
// Ownership: Header-only stateless utilities; results own scalar values only.
// References: src/frontends/common/ConstantFolding.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares complete-input numeric literal parsers for frontends.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace il::frontends::common::number_parsing {

/// @brief Result of parsing a numeric literal.
struct ParsedNumber {
    bool isFloat = false;    ///< True if number has decimal point or exponent
    int64_t intValue = 0;    ///< Integer value (valid when !isFloat)
    double floatValue = 0.0; ///< Float value (valid when isFloat)
    bool overflow = false;   ///< True if value overflowed during parsing
    bool valid = true;       ///< True if parsing succeeded
    /// True if value is exactly 9223372036854775808 (requires negation to be valid).
    /// This enables parsing `-9223372036854775808` as INT64_MIN.
    bool requiresNegation = false;
};

/// @brief Parse a decimal numeric literal from text.
/// @param text The numeric literal text (digits, optional decimal, optional exponent)
/// @return ParsedNumber with the parsed value
/// @details Handles formats like: 123, 123.45, 1.23e10, 1E-5
[[nodiscard]] inline ParsedNumber parseDecimalLiteral(std::string_view text) {
    ParsedNumber result;

    if (text.empty()) {
        result.valid = false;
        return result;
    }

    // Check if it's a float (has decimal point or exponent)
    bool hasDecimal = text.find('.') != std::string_view::npos;
    bool hasExponent =
        text.find('e') != std::string_view::npos || text.find('E') != std::string_view::npos;

    result.isFloat = hasDecimal || hasExponent;

    if (result.isFloat) {
        auto parseResult =
            std::from_chars(text.data(), text.data() + text.size(), result.floatValue,
                            std::chars_format::general);
        if (parseResult.ec == std::errc::result_out_of_range ||
            !std::isfinite(result.floatValue)) {
            result.overflow = true;
            result.valid = false;
        } else if (parseResult.ec != std::errc{} ||
                   parseResult.ptr != text.data() + text.size()) {
            result.valid = false;
        }
    } else {
        // Parse as unsigned first to handle the INT64_MIN edge case.
        // The value 9223372036854775808 overflows int64_t but is exactly -INT64_MIN,
        // which is valid when negated (e.g., "-9223372036854775808").
        uint64_t unsignedValue = 0;
        auto parseResult = std::from_chars(text.data(), text.data() + text.size(), unsignedValue);

        if (parseResult.ec == std::errc::result_out_of_range) {
            result.overflow = true;
            result.valid = false;
        } else if (parseResult.ec != std::errc{} ||
                   parseResult.ptr != text.data() + text.size()) {
            result.valid = false;
        } else {
            // Check if value fits in signed int64_t
            constexpr uint64_t INT64_MAX_AS_UINT = static_cast<uint64_t>(INT64_MAX);
            constexpr uint64_t INT64_MIN_ABS = static_cast<uint64_t>(INT64_MAX) + 1;

            if (unsignedValue <= INT64_MAX_AS_UINT) {
                result.intValue = static_cast<int64_t>(unsignedValue);
            } else if (unsignedValue == INT64_MIN_ABS) {
                // Special case: 9223372036854775808 requires negation to be valid
                // Store as 0 for now, parser will handle the negation
                result.intValue = 0;
                result.requiresNegation = true;
            } else {
                result.overflow = true;
                result.valid = false;
            }
        }
    }

    return result;
}

/// @brief Parse a hexadecimal integer literal from text.
/// @param text The hex literal text (hex digits only, no prefix)
/// @return ParsedNumber with the parsed value (always integer)
/// @details Expects text without prefix (e.g., "DEADBEEF" not "$DEADBEEF" or "0xDEADBEEF").
///          Hex literals are treated as bit patterns, so 0x8000000000000000 becomes INT64_MIN.
[[nodiscard]] inline ParsedNumber parseHexLiteral(std::string_view text) {
    ParsedNumber result;
    result.isFloat = false;

    if (text.empty()) {
        result.valid = false;
        return result;
    }

    // Parse as unsigned to allow full 64-bit range including 0x8000000000000000.
    // Hex literals are treated as bit patterns, so values like 0x8000000000000000
    // become negative when interpreted as signed (INT64_MIN).
    uint64_t unsignedValue = 0;
    auto parseResult = std::from_chars(text.data(), text.data() + text.size(), unsignedValue, 16);

    if (parseResult.ec == std::errc::result_out_of_range) {
        result.overflow = true;
        result.valid = false;
    } else if (parseResult.ec != std::errc{} || parseResult.ptr != text.data() + text.size()) {
        result.valid = false;
    } else {
        // Reinterpret unsigned bits as signed - this makes 0x8000000000000000 = INT64_MIN
        result.intValue = std::bit_cast<int64_t>(unsignedValue);
    }

    return result;
}

/// @brief Parse a binary integer literal from text.
/// @param text The binary literal text (0s and 1s only, no prefix)
/// @return ParsedNumber with the parsed value (always integer)
[[nodiscard]] inline ParsedNumber parseBinaryLiteral(std::string_view text) {
    ParsedNumber result;
    result.isFloat = false;

    if (text.empty()) {
        result.valid = false;
        return result;
    }

    auto parseResult = std::from_chars(text.data(), text.data() + text.size(), result.intValue, 2);

    if (parseResult.ec == std::errc::result_out_of_range) {
        result.overflow = true;
        result.valid = false;
    } else if (parseResult.ec != std::errc{} || parseResult.ptr != text.data() + text.size()) {
        result.valid = false;
    }

    return result;
}

/// @brief Parse an octal integer literal from text.
/// @param text The octal literal text (0-7 digits only, no prefix)
/// @return ParsedNumber with the parsed value (always integer)
[[nodiscard]] inline ParsedNumber parseOctalLiteral(std::string_view text) {
    ParsedNumber result;
    result.isFloat = false;

    if (text.empty()) {
        result.valid = false;
        return result;
    }

    auto parseResult = std::from_chars(text.data(), text.data() + text.size(), result.intValue, 8);

    if (parseResult.ec == std::errc::result_out_of_range) {
        result.overflow = true;
        result.valid = false;
    } else if (parseResult.ec != std::errc{} || parseResult.ptr != text.data() + text.size()) {
        result.valid = false;
    }

    return result;
}

/// @brief Check if a character could start a numeric literal.
/// @param c The character to check
/// @return True if c is a digit
[[nodiscard]] constexpr bool isNumberStart(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// @brief Check if a character is a valid exponent indicator.
/// @param c The character to check
/// @return True if c is 'e' or 'E'
[[nodiscard]] constexpr bool isExponentChar(char c) noexcept {
    return c == 'e' || c == 'E';
}

/// @brief Check if a character is a sign for exponent.
/// @param c The character to check
/// @return True if c is '+' or '-'
[[nodiscard]] constexpr bool isSignChar(char c) noexcept {
    return c == '+' || c == '-';
}

} // namespace il::frontends::common::number_parsing
