//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/TypeRules.cpp
// Purpose: Implements table-driven BASIC numeric result selection and a
//          process-wide callback for recoverable operator diagnostics.
// Key invariants:
//   - The operator table is immutable and searched deterministically.
//   - Unknown operators preserve the left operand type after reporting.
//   - Arithmetic promotion preserves surface INTEGER/LONG/SINGLE/DOUBLE ranks.
// Ownership/Lifetime:
//   - The installed error sink is process-wide owned state.
//   - TypeError payloads passed to the sink are temporary and valid only for
//     the callback invocation.
// Links: src/frontends/basic/TypeRules.hpp,
//        src/frontends/basic/SemanticAnalyzer_Exprs.cpp,
//        docs/specs/numerics.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/TypeRules.hpp"

#include <array>
#include <string>
#include <utility>

/// @file
/// @brief Numeric promotion utilities for the BASIC front end.
/// @details Provides helper functions that compute result types for unary and
///          binary numeric operators while surfacing recoverable diagnostics
///          through the currently installed global sink.

namespace il::frontends::basic {
namespace {

/// Numeric type spelling used by the private rule functions.
using NumericType = TypeRules::NumericType;

/// Signature shared by all binary promotion functions in the rule table.
using BinaryFn = NumericType (*)(NumericType, NumericType) noexcept;

/// @brief Access the globally configured type error sink.
/// @details Lazily initialises the sink to an empty callable so callers can
///          install a handler without worrying about static initialisation order.
/// @return Reference to the stored sink function.
/// @warning Access is unsynchronized; callers must serialize concurrent sink
///          replacement and diagnostic emission.
TypeRules::TypeErrorSink &typeErrorSink() noexcept {
    static TypeRules::TypeErrorSink sink;
    return sink;
}

/// @brief Convert a numeric type enumerator into a human-readable string.
/// @param type Numeric type to describe.
/// @return Uppercase name for the BASIC numeric type.
std::string_view numericTypeName(NumericType type) noexcept {
    switch (type) {
        case NumericType::Integer:
            return "INTEGER";
        case NumericType::Long:
            return "LONG";
        case NumericType::Single:
            return "SINGLE";
        case NumericType::Double:
            return "DOUBLE";
    }
    return "UNKNOWN";
}

/// @brief Emit a diagnostic for a type error if a sink is configured.
/// @param code Diagnostic identifier describing the error.
/// @param message Human-readable explanation of the violation.
/// @post Invokes the configured sink once, or performs no work when it is empty.
void emitTypeError(std::string_view code, std::string_view message) {
    if (auto &sink = typeErrorSink()) {
        sink(TypeRules::TypeError{std::string(code), std::string(message)});
    }
}

/// @brief Report an unsupported binary operator and operand combination.
/// @param op Operator spelling.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @post Submits B2101 to the current sink when one is installed.
void reportUnsupportedBinary(std::string_view op, NumericType lhs, NumericType rhs) {
    std::string message = "unsupported numeric operator '";
    message += op;
    message += "' for operands ";
    message += numericTypeName(lhs);
    message += " and ";
    message += numericTypeName(rhs);
    message += '.';
    emitTypeError("B2101", message);
}

/// @brief Report an unsupported unary operator.
/// @param op Operator character (for example '+').
/// @param operand Operand type.
/// @post Submits B2102 to the current sink when one is installed.
void reportUnsupportedUnaryOperator(char op, NumericType operand) {
    std::string message = "unsupported unary operator '";
    message.push_back(op);
    message += "' for operand ";
    message += numericTypeName(operand);
    message += '.';
    emitTypeError("B2102", message);
}

/// @brief Report an unsupported unary operand for a valid operator.
/// @param op Operator character.
/// @param operand Operand type.
/// @post Submits B2103 to the current sink when one is installed.
void reportUnsupportedUnaryOperand(char op, NumericType operand) {
    std::string message = "unsupported operand ";
    message += numericTypeName(operand);
    message += " for unary operator '";
    message.push_back(op);
    message += "'.";
    emitTypeError("B2103", message);
}

/// @brief Check whether the numeric type is an integer category.
/// @param type Numeric type to query.
/// @return @c true if the type is INTEGER or LONG.
constexpr bool isInteger(NumericType type) noexcept {
    return type == NumericType::Integer || type == NumericType::Long;
}

/// @brief Check whether the numeric type is a floating-point category.
/// @param type Numeric type to query.
/// @return @c true if the type is SINGLE or DOUBLE.
constexpr bool isFloat(NumericType type) noexcept {
    return type == NumericType::Single || type == NumericType::Double;
}

/// @brief Promote two integer operands to a common integer type.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return INTEGER when both operands are INTEGER, otherwise LONG.
constexpr NumericType promoteInteger(NumericType lhs, NumericType rhs) noexcept {
    return (lhs == NumericType::Long || rhs == NumericType::Long) ? NumericType::Long
                                                                  : NumericType::Integer;
}

/// @brief Promote two floating-point operands to a common type.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return DOUBLE when either operand is DOUBLE, otherwise SINGLE.
constexpr NumericType promoteFloat(NumericType lhs, NumericType rhs) noexcept {
    return (lhs == NumericType::Double || rhs == NumericType::Double) ? NumericType::Double
                                                                      : NumericType::Single;
}

/// @brief Determine the result type for arithmetic operators (+, -, *).
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Promoted integer type when both operands are integers; otherwise a promoted float type.
constexpr NumericType arithmeticResult(NumericType lhs, NumericType rhs) noexcept {
    if (isInteger(lhs) && isInteger(rhs))
        return promoteInteger(lhs, rhs);
    return promoteFloat(lhs, rhs);
}

/// @brief Determine the result type for division operators (/).
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return DOUBLE when either operand is DOUBLE, SINGLE when either operand is SINGLE, otherwise
/// DOUBLE.
constexpr NumericType divisionResult(NumericType lhs, NumericType rhs) noexcept {
    if (lhs == NumericType::Double || rhs == NumericType::Double)
        return NumericType::Double;
    if (lhs == NumericType::Single || rhs == NumericType::Single)
        return NumericType::Single;
    return NumericType::Double;
}

/// @brief Determine the result type for integer division and modulus.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return LONG when either operand is LONG; otherwise INTEGER.
/// @note This helper selects a result rank only; it does not independently
///       reject floating operands.
constexpr NumericType integerResult(NumericType lhs, NumericType rhs) noexcept {
    return promoteInteger(lhs, rhs);
}

/// @brief Determine the result type for exponentiation.
/// @details Both operand arguments are intentionally ignored.
/// @return DOUBLE regardless of operand types, matching BASIC semantics.
constexpr NumericType powerResult(NumericType, NumericType) noexcept {
    return NumericType::Double;
}

/// @brief Immutable mapping from an operator spelling to its promotion function.
struct BinaryRule {
    /// Case-insensitive operator spelling.
    std::string_view op{};

    /// Non-null result-selection function.
    BinaryFn fn{nullptr};
};

/// Complete supported binary numeric operator table.
constexpr std::array<BinaryRule, 7> Rules = {{{"+", &arithmeticResult},
                                              {"-", &arithmeticResult},
                                              {"*", &arithmeticResult},
                                              {"/", &divisionResult},
                                              {"\\", &integerResult},
                                              {"MOD", &integerResult},
                                              {"^", &powerResult}}};

/// @brief Uppercase an ASCII character.
/// @param c Character to convert.
/// @return Uppercase equivalent when @p c is lowercase; otherwise @p c unchanged.
constexpr char upperChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// @brief Compare two strings ignoring ASCII case.
/// @param lhs Left-hand string.
/// @param rhs Right-hand string.
/// @return @c true if the strings are equal when compared case-insensitively.
constexpr bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (upperChar(lhs[i]) != upperChar(rhs[i]))
            return false;
    }
    return true;
}

} // namespace

/// @brief Install a callback that receives type error diagnostics.
/// @param sink Callable moved into process-wide storage; an empty callable
///             disables reporting.
/// @warning Sink replacement and use are not internally synchronized.
void TypeRules::setTypeErrorSink(TypeErrorSink sink) noexcept {
    typeErrorSink() = std::move(sink);
}

/// @brief Determine the result numeric type for a binary operator.
/// @details Looks up the operator spelling in the @ref Rules table and invokes
///          the associated function to compute the promotion result.  When the
///          operator is unknown the function emits a diagnostic and falls back to
///          the left-hand operand type.
/// @param op Operator spelling (case-insensitive).
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Resulting numeric type.
/// @note A one-byte spelling delegates to the character overload. Multi-byte
///       spellings such as MOD compare with ASCII case folding.
TypeRules::NumericType TypeRules::resultType(std::string_view op,
                                             NumericType lhs,
                                             NumericType rhs) noexcept {
    if (op.size() == 1)
        return resultType(op.front(), lhs, rhs);

    for (const auto &rule : Rules) {
        if (equalsIgnoreCase(rule.op, op))
            return rule.fn(lhs, rhs);
    }
    // Recoverable path: emit diagnostic and fall back to lhs type.
    reportUnsupportedBinary(op, lhs, rhs);
    return lhs;
}

/// @brief Determine the result numeric type for a single-character operator.
/// @details Searches the @ref Rules table for a matching single-character
///          operator.  When the operator is unknown the function emits a
///          diagnostic and falls back to the left-hand operand type.
/// @param op Operator character.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Resulting numeric type.
/// @post An unsupported character reports B2101 when a sink is installed.
TypeRules::NumericType TypeRules::resultType(char op, NumericType lhs, NumericType rhs) noexcept {
    for (const auto &rule : Rules) {
        if (rule.op.size() == 1 && rule.op.front() == op)
            return rule.fn(lhs, rhs);
    }
    // Recoverable path: emit diagnostic and fall back to lhs type.
    std::string opStr(1, op);
    reportUnsupportedBinary(opStr, lhs, rhs);
    return lhs;
}

/// @brief Determine the result numeric type for a unary operator.
/// @details Supports the @c + and @c - operators for numeric operands.  When an
///          unsupported operator or operand type is encountered, a diagnostic is
///          emitted and the operand type is returned unchanged.
/// @param op Operator character.
/// @param operand Operand type.
/// @return Resulting numeric type after applying the operator.
/// @post An unsupported operator reports B2102; an invalid numeric enumerator
///       used with plus or minus reports B2103 when a sink is installed.
TypeRules::NumericType TypeRules::unaryResultType(char op, NumericType operand) noexcept {
    if (op == '-' || op == '+') {
        if (isFloat(operand) || isInteger(operand))
            return operand;
        // Recoverable path: emit diagnostic and preserve operand type.
        reportUnsupportedUnaryOperand(op, operand);
        return operand;
    }
    // Recoverable path: emit diagnostic and preserve operand type.
    reportUnsupportedUnaryOperator(op, operand);
    return operand;
}

} // namespace il::frontends::basic
