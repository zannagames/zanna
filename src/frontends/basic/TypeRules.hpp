//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/TypeRules.hpp
// Purpose: Declares surface BASIC numeric ranks, table-driven operator result
//          selection, and recoverable type-error reporting.
// Key invariants:
//   - INTEGER and LONG lower as integral storage; SINGLE and DOUBLE lower as
//     floating storage while retaining distinct semantic ranks.
//   - Unsupported operators return the left operand type after optional
//     diagnostic delivery.
//   - Exponentiation selects DOUBLE and slash division selects a floating rank.
// Ownership/Lifetime:
//   - Result computations retain no per-call state.
//   - setTypeErrorSink replaces one process-wide owned callback; access is not
//     internally synchronized.
// Links: src/frontends/basic/TypeRules.cpp,
//        src/frontends/basic/SemanticAnalyzer_Exprs.cpp,
//        docs/specs/numerics.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <functional>
#include <string>
#include <string_view>

/// @file
/// @brief Declares BASIC numeric operator result rules and error callbacks.

namespace il::frontends::basic {

/// @brief Namespace-like utility class for BASIC numeric result selection.
/// @details INTEGER and LONG are integral ranks, while SINGLE and DOUBLE are
///          floating ranks. The implementation recognizes plus, minus,
///          multiply, slash division, backslash division, MOD, and power.
class TypeRules {
  public:
    /// @brief Available numeric types ordered by promotion lattice.
    enum class NumericType {
        Integer, ///< INTEGER/INT surface rank, stored as i64.
        Long,    ///< LONG surface rank, stored as i64.
        Single,  ///< SINGLE surface rank, stored as f64.
        Double,  ///< DOUBLE surface rank, stored as f64.
    };

    /// @brief Structured information describing a numeric type error.
    struct TypeError {
        std::string code;    ///< Project-defined diagnostic code.

        /// Human-readable explanation of the unsupported operation.
        std::string message; ///< Human-readable explanation.
    };

    /// @brief Callback invoked when recoverable type errors occur.
    using TypeErrorSink = std::function<void(const TypeError &error)>;

    /// @brief Determine the binary operator result type.
    /// @param op Operator token ("+", "-", "*", "/", "\\", "MOD", "^").
    /// @param lhs Left operand numeric type.
    /// @param rhs Right operand numeric type.
    /// @return Resulting numeric type according to BASIC promotion rules.
    static NumericType resultType(std::string_view op, NumericType lhs, NumericType rhs) noexcept;

    /// @brief Determine binary operator result type for single-character operators.
    /// @param op One-byte operator spelling.
    /// @param lhs Left operand numeric rank.
    /// @param rhs Right operand numeric rank.
    /// @return Table-selected result, or @p lhs after reporting an unsupported
    ///         operator.
    static NumericType resultType(char op, NumericType lhs, NumericType rhs) noexcept;

    /// @brief Determine the unary operator result type.
    /// @param op Unary operator ("-" or "+").
    /// @param operand Operand numeric type.
    /// @return @p operand for supported plus/minus and as the recoverable
    ///         fallback after reporting any unsupported case.
    static NumericType unaryResultType(char op, NumericType operand) noexcept;

    /// @brief Install a callback used to report recoverable type errors.
    /// @param sink Callback moved into global storage; empty disables reporting.
    /// @warning Concurrent replacement and result computation require external
    ///          synchronization.
    static void setTypeErrorSink(TypeErrorSink sink) noexcept;
};

} // namespace il::frontends::basic
