//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/constfold/FoldBuiltins.cpp
// Purpose: Provides constant folding for numeric builtin functions (VAL, INT,
//          FIX, ROUND, STR$) that convert between types or perform rounding.
// Key invariants: All helpers return nullptr when folding cannot proceed,
//                 ensuring callers can safely check the result. Numeric results
//                 preserve BASIC's floating-point semantics.
// Ownership/Lifetime: Returns newly allocated AST nodes via unique_ptr.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#builtins
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements numeric builtin folding utilities.
/// @details Covers VAL (string→number), INT (floor), FIX (truncate), ROUND,
///          and STR$ (number→string) for literal arguments.

#include "frontends/basic/constfold/Dispatch.hpp"

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "zanna/il/io/FormatUtils.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace il::frontends::basic::constfold {
namespace {

/// @brief Create a floating-point literal node.
/// @param value Numeric value to embed.
/// @return Unique pointer owning the new float literal.
AST::ExprPtr make_float(double value) {
    auto out = std::make_unique<::il::frontends::basic::FloatExpr>();
    out->value = value;
    return out;
}

/// @brief Create an integer literal node.
/// @param value Numeric value to embed.
/// @return Unique pointer owning the new integer literal.
AST::ExprPtr make_int(long long value) {
    auto out = std::make_unique<::il::frontends::basic::IntExpr>();
    out->value = value;
    return out;
}

/// @brief Create a string literal node.
/// @param value Text to embed.
/// @return Unique pointer owning the new string literal.
AST::ExprPtr make_string(std::string value) {
    auto out = std::make_unique<::il::frontends::basic::StringExpr>();
    out->value = std::move(value);
    return out;
}

/// @brief Extract a finite double from an expression if possible.
/// @param expr Expression to inspect.
/// @return Finite double value or empty optional.
std::optional<double> get_finite_double(const AST::Expr &expr) {
    auto numeric = numeric_from_expr(expr);
    if (!numeric)
        return std::nullopt;
    double value = numeric->isFloat ? numeric->f : static_cast<double>(numeric->i);
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

/// @brief Round a finite double using BASIC's banker-rounding rule.
/// @param expr Expression to inspect.
/// @return Rounded value or empty optional when folding cannot proceed.
std::optional<double> get_rounded_even_double(const AST::Expr &expr) {
    auto value = get_finite_double(expr);
    if (!value)
        return std::nullopt;
    double rounded = std::nearbyint(*value);
    if (!std::isfinite(rounded))
        return std::nullopt;
    return rounded;
}

/// @brief Round a value to the specified decimal digits.
/// @param value Value to round.
/// @param digits Positive for fractional, negative for integral multiples.
/// @return Rounded result or empty optional on overflow.
std::optional<double> round_to_digits(double value, int digits) {
    if (!std::isfinite(value))
        return std::nullopt;

    if (digits == 0) {
        double rounded = std::nearbyint(value);
        if (!std::isfinite(rounded))
            return std::nullopt;
        return rounded;
    }

    double scaleExponent = static_cast<double>(std::abs(digits));
    double scale = std::pow(10.0, scaleExponent);
    if (!std::isfinite(scale) || scale == 0.0)
        return std::nullopt;

    double scaled = digits > 0 ? value * scale : value / scale;
    if (!std::isfinite(scaled))
        return std::nullopt;

    double rounded = std::nearbyint(scaled);
    if (!std::isfinite(rounded))
        return std::nullopt;

    double result = digits > 0 ? rounded / scale : rounded * scale;
    if (!std::isfinite(result))
        return std::nullopt;
    return result;
}

/// @brief Parse a string using BASIC's VAL semantics.
/// @param s String to parse.
/// @return Parsed double or empty optional when invalid.
std::optional<double> parse_val_string(const std::string &s) {
    const char *raw = s.c_str();
    while (*raw && std::isspace(static_cast<unsigned char>(*raw)))
        ++raw;

    if (*raw == '\0')
        return 0.0;

    /// @brief Tests whether a character is an ASCII decimal digit.
    /// @param ch Character to inspect.
    /// @return `true` for `0` through `9`.
    auto isDigit = [](char ch) { return ch >= '0' && ch <= '9'; };

    if (*raw == '+' || *raw == '-') {
        char next = raw[1];
        if (next == '.') {
            if (!isDigit(raw[2]))
                return 0.0;
        } else if (!isDigit(next)) {
            return 0.0;
        }
    } else if (*raw == '.') {
        if (!isDigit(raw[1]))
            return 0.0;
    } else if (!isDigit(*raw)) {
        return 0.0;
    }

    char *endp = nullptr;
    double parsed = std::strtod(raw, &endp);
    if (endp == raw)
        return 0.0;
    if (!std::isfinite(parsed))
        return std::nullopt;
    return parsed;
}

} // namespace

/// @brief Fold VAL builtin when the argument is a literal string.
/// @param arg Expression passed to VAL().
/// @return Float literal or nullptr when folding cannot proceed.
AST::ExprPtr foldValLiteral(const AST::Expr &arg) {
    const auto *str = as<const StringExpr>(arg);
    if (!str)
        return nullptr;
    auto parsed = parse_val_string(str->value);
    if (!parsed)
        return nullptr;
    return make_float(*parsed);
}

/// @brief Fold CINT builtin using banker rounding and int16 range checks.
/// @param arg Expression passed to CINT().
/// @return Integer literal or nullptr when folding cannot proceed.
AST::ExprPtr foldCIntLiteral(const AST::Expr &arg) {
    auto value = get_rounded_even_double(arg);
    if (!value)
        return nullptr;
    if (*value < static_cast<double>(std::numeric_limits<int16_t>::min()) ||
        *value > static_cast<double>(std::numeric_limits<int16_t>::max()))
        return nullptr;
    return make_int(static_cast<long long>(static_cast<int16_t>(*value)));
}

/// @brief Fold CLNG builtin using banker rounding and int32 range checks.
/// @param arg Expression passed to CLNG().
/// @return Integer literal or nullptr when folding cannot proceed.
AST::ExprPtr foldCLngLiteral(const AST::Expr &arg) {
    auto value = get_rounded_even_double(arg);
    if (!value)
        return nullptr;
    if (*value < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        *value > static_cast<double>(std::numeric_limits<int32_t>::max()))
        return nullptr;
    return make_int(static_cast<long long>(static_cast<int32_t>(*value)));
}

/// @brief Fold CSNG builtin by casting to float when the result remains finite.
/// @param arg Expression passed to CSNG().
/// @return Float literal or nullptr when folding cannot proceed.
AST::ExprPtr foldCSngLiteral(const AST::Expr &arg) {
    auto value = get_finite_double(arg);
    if (!value)
        return nullptr;
    float converted = static_cast<float>(*value);
    if (!std::isfinite(converted))
        return nullptr;
    return make_float(static_cast<double>(converted));
}

/// @brief Fold CDBL builtin by materialising a floating literal.
/// @param arg Expression passed to CDBL().
/// @return Float literal or nullptr when folding cannot proceed.
AST::ExprPtr foldCDblLiteral(const AST::Expr &arg) {
    auto value = get_finite_double(arg);
    if (!value)
        return nullptr;
    return make_float(*value);
}

/// @brief Fold INT builtin (floor) when the argument is a literal numeric.
/// @param arg Expression passed to INT().
/// @return Float literal with floor value or nullptr.
AST::ExprPtr foldIntLiteral(const AST::Expr &arg) {
    auto value = get_finite_double(arg);
    if (!value)
        return nullptr;
    double floored = std::floor(*value);
    if (!std::isfinite(floored))
        return nullptr;
    return make_float(floored);
}

/// @brief Fold FIX builtin (truncate) when the argument is a literal numeric.
/// @param arg Expression passed to FIX().
/// @return Float literal with truncated value or nullptr.
AST::ExprPtr foldFixLiteral(const AST::Expr &arg) {
    auto value = get_finite_double(arg);
    if (!value)
        return nullptr;
    double truncated = std::trunc(*value);
    if (!std::isfinite(truncated))
        return nullptr;
    return make_float(truncated);
}

/// @brief Fold ROUND builtin when arguments are literal.
/// @param value Expression for the value to round.
/// @param digits Optional expression for decimal digits (may be nullptr).
/// @return Float literal with rounded value or nullptr.
AST::ExprPtr foldRoundLiteral(const AST::Expr &value, const AST::Expr *digits) {
    auto val = get_finite_double(value);
    if (!val)
        return nullptr;

    int digitCount = 0;
    if (digits) {
        auto digVal = get_finite_double(*digits);
        if (!digVal)
            return nullptr;
        double rounded = std::nearbyint(*digVal);
        if (!std::isfinite(rounded))
            return nullptr;
        if (rounded < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            rounded > static_cast<double>(std::numeric_limits<int32_t>::max()))
            return nullptr;
        digitCount = static_cast<int>(rounded);
    }

    auto result = round_to_digits(*val, digitCount);
    if (!result)
        return nullptr;
    return make_float(*result);
}

/// @brief Fold STR$ builtin when the argument is a literal numeric.
/// @param arg Expression passed to STR$().
/// @return String literal or nullptr.
AST::ExprPtr foldStrLiteral(const AST::Expr &arg) {
    auto numeric = numeric_from_expr(arg);
    if (!numeric)
        return nullptr;

    std::string formatted = numeric->isFloat ? zanna::il::io::format_float(numeric->f)
                                             : zanna::il::io::format_integer(numeric->i);
    return make_string(std::move(formatted));
}

} // namespace il::frontends::basic::constfold
