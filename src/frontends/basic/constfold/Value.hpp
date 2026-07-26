//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/constfold/Value.hpp
// Purpose: Defines tagged numeric values, literal parsing, and promotion
//          helpers shared by BASIC constant-folding domains.
// Key invariants:
//   - valid == false denotes a folding failure rather than a numeric value.
//   - ValueKind selects the authoritative numeric payload.
//   - Promotion never mutates caller-owned values.
// Ownership/Lifetime: All values own their scalar payloads and are returned by
//                     value; parsing temporarily owns normalized text.
// Links: src/frontends/basic/constfold/Dispatch.hpp,
//        src/frontends/basic/constfold/FoldArith.cpp,
//        src/frontends/basic/constfold/FoldCompare.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the lightweight value container used by folding helpers.
/// @details BASIC folding routines operate on small tagged scalars that model
///          integer and floating-point literals.  The helpers in this header
///          provide a consistent representation alongside promotion utilities
///          that obey the language's suffix rules.  Keeping the primitives in a
///          single translation unit avoids subtle drift between arithmetic and
///          comparison folders.

#pragma once

#include "frontends/basic/constfold/Dispatch.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace il::frontends::basic::constfold {

namespace detail {

/// @brief Outcome of parsing a numeric literal string.
struct ParsedNumber {
    bool ok = false;      ///< True when @p sv parsed as a complete number.
    bool isFloat = false; ///< True when the literal is floating-point.
    long long i = 0;      ///< Integer value (clamped view when isFloat).
    double d = 0.0;       ///< Floating value (defined when isFloat).
};

/// @brief Parse a BASIC numeric literal, honoring type suffixes and exponents.
/// @details Recognises BASIC's trailing type-suffix markers and lets them force
///          the representation: `!`/`#` force float, `%`/`&` force integer. A
///          Fortran-style `D`/`d` exponent is normalised to `e` so `strtod`
///          accepts double-precision literals. Whether float or integer parsing
///          is attempted first is decided by the forced suffix or, absent one,
///          by the presence of float markers (`.eEpP`); the other form is tried
///          as a fallback. Float magnitudes outside the `long long` range are
///          clamped into @ref ParsedNumber::i so callers always get a usable
///          integer view. Surrounding whitespace is trimmed; an unparsable or
///          out-of-range input yields `ok == false`.
/// @param sv Raw literal text (may include a suffix and surrounding spaces).
/// @return Parsed result; check @ref ParsedNumber::ok before use.
inline ParsedNumber parseNumericLiteral(std::string_view sv) noexcept {
    ParsedNumber result{};

    /// @brief Removes surrounding locale whitespace from a string view.
    /// @param[in,out] view View adjusted to the trimmed subrange.
    auto trim = [](std::string_view &view) noexcept {
        /// @brief Tests whether a byte is classified as whitespace.
        /// @param ch Unsigned byte to inspect.
        /// @return `true` when the active C locale classifies it as whitespace.
        auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        while (!view.empty() && is_space(static_cast<unsigned char>(view.front())))
            view.remove_prefix(1);
        while (!view.empty() && is_space(static_cast<unsigned char>(view.back())))
            view.remove_suffix(1);
    };

    trim(sv);
    if (sv.empty())
        return result;

    bool forceFloat = false;
    bool forceInt = false;
    switch (sv.back()) {
        case '!':
        case '#':
            forceFloat = true;
            sv.remove_suffix(1);
            break;
        case '%':
        case '&':
            forceInt = true;
            sv.remove_suffix(1);
            break;
        default:
            break;
    }

    trim(sv);
    if (sv.empty())
        return result;

    std::string normalisedStorage;
    bool hasFloatMarkers = sv.find_first_of(".eEpP") != std::string_view::npos;

    if (sv.find_first_of("dD") != std::string_view::npos) {
        normalisedStorage.assign(sv.begin(), sv.end());
        for (char &ch : normalisedStorage) {
            if (ch == 'd' || ch == 'D')
                ch = 'e';
        }
        sv = normalisedStorage;
        hasFloatMarkers = true;
    }

    const bool tryFloatFirst = forceFloat || (!forceInt && hasFloatMarkers);

    /// @brief Parses a complete finite floating-point token.
    /// @param view Normalized numeric token.
    /// @return `true` when the entire token parses without range errors.
    auto parseFloat = [&](std::string_view view) -> bool {
        // Use strtod instead of from_chars since Apple Clang doesn't support
        // from_chars for floating-point in C++20
        std::string temp(view.data(), view.size());
        char *end = nullptr;
        errno = 0;
        double value = std::strtod(temp.c_str(), &end);

        // Check if entire string was consumed and no error occurred
        if (end == temp.c_str() + temp.size() && errno == 0) {
            if (!std::isfinite(value))
                return false;
            result.ok = true;
            result.isFloat = true;
            result.d = value;
            if (value >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                value <= static_cast<double>(std::numeric_limits<long long>::max())) {
                result.i = static_cast<long long>(value);
            } else {
                result.i = value < 0 ? std::numeric_limits<long long>::min()
                                     : std::numeric_limits<long long>::max();
            }
            return true;
        }
        return false;
    };

    if (tryFloatFirst) {
        if (parseFloat(sv))
            return result;
    }

    long long intValue = 0;
    auto ic = std::from_chars(sv.data(), sv.data() + sv.size(), intValue, 10);
    if (ic.ec == std::errc{} && ic.ptr == sv.data() + sv.size()) {
        result.ok = true;
        result.isFloat = false;
        result.i = intValue;
        result.d = static_cast<double>(intValue);
        return result;
    }
    if (ic.ec == std::errc::result_out_of_range)
        return result;

    if (!tryFloatFirst && !forceInt) {
        if (parseFloat(sv))
            return result;
    }

    return result;
}

} // namespace detail

/// @brief Kind tags understood by the constant-folding helpers.
enum class ValueKind {
    /// Signed integer representation.
    Int,
    /// Double-precision floating-point representation.
    Float,
};

/// @brief Lightweight tagged scalar used by arithmetic and comparison folders.
struct Value {
    ValueKind kind = ValueKind::Int; ///< Representation tag of the payload.
    double f = 0.0;                  ///< Floating payload (always finite).
    long long i = 0;                 ///< Integer payload using two's-complement.
    bool valid = false;              ///< Indicates whether the value is usable.

    /// @brief Factory for invalid values used to signal folding failures.
    /// @return Invalid sentinel with a deterministic zero payload.
    static constexpr Value invalid() noexcept {
        return Value{ValueKind::Int, 0.0, 0, false};
    }

    /// @brief Construct an integer literal.
    /// @param v Signed integer payload.
    /// @return Valid integer-tagged folding value.
    static constexpr Value fromInt(long long v) noexcept {
        return Value{ValueKind::Int, static_cast<double>(v), v, true};
    }

    /// @brief Construct a floating-point literal.
    /// @param v Floating-point payload; callers ensure conversion to the
    ///        integer view is defined.
    /// @return Valid floating-point-tagged folding value.
    static constexpr Value fromFloat(double v) noexcept {
        return Value{ValueKind::Float, v, static_cast<long long>(v), true};
    }

    /// @brief Query whether the payload models a float.
    /// @return True when the value is valid and floating-point tagged.
    [[nodiscard]] constexpr bool isFloat() const noexcept {
        return valid && kind == ValueKind::Float;
    }

    /// @brief Query whether the payload models an integer.
    /// @return True when the value is valid and integer tagged.
    [[nodiscard]] constexpr bool isInt() const noexcept {
        return valid && kind == ValueKind::Int;
    }

    /// @brief Obtain the value as a double regardless of representation.
    /// @return Floating payload, or the integer payload converted to double.
    [[nodiscard]] constexpr double asDouble() const noexcept {
        return kind == ValueKind::Float ? f : static_cast<double>(i);
    }
};

/// @brief Convert @p numeric into a folding value.
/// @param numeric Dispatcher numeric representation to copy.
/// @return Valid folding value preserving @p numeric's representation tag.
[[nodiscard]] inline Value makeValue(const NumericValue &numeric) noexcept {
    return numeric.isFloat ? Value::fromFloat(numeric.f) : Value::fromInt(numeric.i);
}

/// @brief Convert @p value back into the dispatcher representation.
/// @param value Folding value to copy.
/// @return NumericValue containing coherent integer and floating-point views.
[[nodiscard]] inline NumericValue toNumericValue(const Value &value) noexcept {
    NumericValue numeric;
    numeric.isFloat = value.isFloat();
    numeric.f = value.isFloat() ? value.f : static_cast<double>(value.i);
    numeric.i = value.i;
    return numeric;
}

/// @brief Promote @p lhs and @p rhs following BASIC's suffix rules.
/// @param lhs Left operand copied into the result pair.
/// @param rhs Right operand copied into the result pair.
/// @return Invalid pair when either operand is invalid; otherwise operands
///         promoted to floating point when either input is floating point.
[[nodiscard]] inline std::pair<Value, Value> promote(Value lhs, Value rhs) noexcept {
    if (!lhs.valid || !rhs.valid)
        return {Value::invalid(), Value::invalid()};
    if (lhs.isFloat() || rhs.isFloat()) {
        if (!lhs.isFloat())
            lhs = Value::fromFloat(static_cast<double>(lhs.i));
        if (!rhs.isFloat())
            rhs = Value::fromFloat(static_cast<double>(rhs.i));
    }
    return {lhs, rhs};
}

} // namespace il::frontends::basic::constfold
