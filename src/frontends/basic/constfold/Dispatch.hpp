//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/constfold/Dispatch.hpp
// Purpose: Declares BASIC literal extraction, domain-specific fold operations,
//          and the central expression-folding dispatcher.
// Key invariants:
//   - A failed fold leaves the caller's AST unchanged.
//   - Successful AST folds return newly allocated literal nodes.
//   - NumericValue and Constant tags agree with their active payloads.
// Ownership/Lifetime:
//   - Expression parameters are borrowed for each call.
//   - Returned ExprPtr values transfer ownership to the caller.
// Links: src/frontends/basic/constfold/Dispatch.cpp,
//        src/frontends/basic/constfold/Value.hpp,
//        src/frontends/basic/constfold/FoldArith.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Constant folding dispatcher API for the BASIC front end.
/// @details Exposes helpers that allow expression visitors to query whether an
///          expression is foldable and to obtain the folded literal when
///          possible.  Domain-specific implementations live in the neighbouring
///          translation units.

#pragma once

#include "frontends/basic/ast/ExprNodes.hpp"

#include <optional>
#include <string>

namespace il::frontends::basic::constfold {

namespace AST {
using ::il::frontends::basic::BinaryExpr;
using ::il::frontends::basic::BoolExpr;
using ::il::frontends::basic::Expr;
using ::il::frontends::basic::ExprPtr;
using ::il::frontends::basic::UnaryExpr;
} // namespace AST

/// @brief Numeric literal representation shared by folding helpers.
struct NumericValue {
    /// True when @ref f is the authoritative payload.
    bool isFloat = false;
    /// Floating-point view of the literal.
    double f = 0.0;
    /// Integer view of the literal.
    long long i = 0;
};

/// @brief Literal categories understood by the dispatcher.
enum class LiteralKind {
    /// Signed integer literal.
    Int,
    /// Double-precision floating-point literal.
    Float,
    /// Boolean literal.
    Bool,
    /// Owned string literal.
    String,
    /// Sentinel for an unavailable or unsupported constant.
    Invalid,
};

/// @brief Result container emitted by folding helpers.
struct Constant {
    /// Tag selecting the active payload.
    LiteralKind kind = LiteralKind::Invalid;
    /// Numeric payload used by integer and floating-point constants.
    NumericValue numeric{};
    /// Boolean payload used when @ref kind is LiteralKind::Bool.
    bool boolValue = false;
    /// String payload used when @ref kind is LiteralKind::String.
    std::string stringValue;
};

/// @brief Domains handled by the constant-fold dispatcher.
enum class FoldKind {
    /// Numeric arithmetic operators.
    Arith,
    /// Boolean and bitwise logical operators.
    Logical,
    /// Equality and ordering operators.
    Compare,
    /// String concatenation and string builtins.
    Strings,
    /// Explicit cast operations.
    Casts,
};

/// @brief Convert an expression into its numeric payload when possible.
/// @param expr Expression to inspect without modifying.
/// @return Numeric literal summary, or std::nullopt for non-numeric nodes.
std::optional<NumericValue> numeric_from_expr(const AST::Expr &expr);

/// @brief Promote @p lhs to match @p rhs according to BASIC rules.
/// @param lhs Left numeric value whose representation is copied and promoted.
/// @param rhs Right numeric value used to select the common representation.
/// @return Copy of @p lhs using floating-point representation when either
///         operand is floating point; otherwise an unchanged integer copy.
NumericValue promote_numeric(const NumericValue &lhs, const NumericValue &rhs);

/// @brief Fold unary arithmetic operators (Plus/Negate) on literals.
/// @param op Unary arithmetic operator to evaluate.
/// @param value Candidate literal operand.
/// @return Newly allocated numeric literal, or nullptr when folding is unsafe
///         or unsupported.
AST::ExprPtr fold_unary_arith(AST::UnaryExpr::Op op, const AST::Expr &value);

/// @brief Fold logical NOT when applied to literal operands.
/// @param operand Candidate boolean or integer literal.
/// @return Newly allocated folded literal, or nullptr for unsupported input.
AST::ExprPtr fold_logical_not(const AST::Expr &operand);

/// @brief Inspect whether a short-circuit logical operator terminates early.
/// @param op Candidate short-circuit logical operator.
/// @param lhs Literal left operand.
/// @return Decisive result when @p lhs makes the right operand unnecessary;
///         otherwise std::nullopt.
std::optional<bool> try_short_circuit(AST::BinaryExpr::Op op, const AST::BoolExpr &lhs);

/// @brief Determine if @p op performs short-circuit evaluation.
/// @param op Binary operator to classify.
/// @return True for the short-circuit AND/OR variants.
bool is_short_circuit(AST::BinaryExpr::Op op);

/// @brief Fold boolean binary expressions with literal operands.
/// @param lhs Candidate literal left operand.
/// @param op Binary logical operator to evaluate.
/// @param rhs Candidate literal right operand.
/// @return Newly allocated boolean literal, or nullptr when no boolean fold
///         applies.
AST::ExprPtr fold_boolean_binary(const AST::Expr &lhs,
                                 AST::BinaryExpr::Op op,
                                 const AST::Expr &rhs);

/// @brief Fold LEN builtin when the argument is a literal.
/// @param arg Candidate string literal.
/// @return Newly allocated length literal, or nullptr for non-string input.
AST::ExprPtr foldLenLiteral(const AST::Expr &arg);

/// @brief Fold MID$ builtin when all operands are literals.
/// @param source Candidate source string literal.
/// @param start Candidate one-based starting-position integer literal.
/// @param length Candidate length integer literal.
/// @return Newly allocated substring literal, or nullptr when an operand is
///         ineligible.
AST::ExprPtr foldMidLiteral(const AST::Expr &source,
                            const AST::Expr &start,
                            const AST::Expr &length);

/// @brief Fold LEFT$ builtin when both operands are literals.
/// @param source Candidate source string literal.
/// @param count Candidate prefix-length integer literal.
/// @return Newly allocated prefix literal, or nullptr when folding is invalid.
AST::ExprPtr foldLeftLiteral(const AST::Expr &source, const AST::Expr &count);

/// @brief Fold RIGHT$ builtin when both operands are literals.
/// @param source Candidate source string literal.
/// @param count Candidate suffix-length integer literal.
/// @return Newly allocated suffix literal, or nullptr when folding is invalid.
AST::ExprPtr foldRightLiteral(const AST::Expr &source, const AST::Expr &count);

/// @brief Fold CHR$ builtin when the argument is a literal integer.
/// @param arg Candidate integer character-code literal.
/// @return Newly allocated one-byte string literal, or nullptr when @p arg is
///         not an integer in the accepted range.
AST::ExprPtr foldChrLiteral(const AST::Expr &arg);

/// @brief Fold VAL builtin when the argument is a literal string.
/// @param arg Candidate numeric string literal.
/// @return Newly allocated numeric literal, or nullptr when parsing fails.
AST::ExprPtr foldValLiteral(const AST::Expr &arg);

/// @brief Fold CINT builtin when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated rounded 16-bit-range integer literal, or nullptr
///         when conversion is invalid or out of range.
AST::ExprPtr foldCIntLiteral(const AST::Expr &arg);

/// @brief Fold CLNG builtin when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated rounded 32-bit-range integer literal, or nullptr
///         when conversion is invalid or out of range.
AST::ExprPtr foldCLngLiteral(const AST::Expr &arg);

/// @brief Fold CSNG builtin when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated single-precision-rounded literal, or nullptr when
///         the result is not finite or representable.
AST::ExprPtr foldCSngLiteral(const AST::Expr &arg);

/// @brief Fold CDBL builtin when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated double-precision literal, or nullptr when folding
///         cannot preserve a finite value.
AST::ExprPtr foldCDblLiteral(const AST::Expr &arg);

/// @brief Fold INT builtin (floor) when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated floored floating literal, or nullptr on failure.
AST::ExprPtr foldIntLiteral(const AST::Expr &arg);

/// @brief Fold FIX builtin (truncate) when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated truncated floating literal, or nullptr on failure.
AST::ExprPtr foldFixLiteral(const AST::Expr &arg);

/// @brief Fold ROUND builtin when arguments are literal.
/// @param value Candidate numeric value literal.
/// @param digits Optional decimal-digit expression; nullptr selects zero
///        fractional digits.
/// @return Newly allocated rounded floating literal, or nullptr when either
///         supplied expression cannot be folded safely.
AST::ExprPtr foldRoundLiteral(const AST::Expr &value, const AST::Expr *digits);

/// @brief Fold STR$ builtin when the argument is a literal numeric.
/// @param arg Candidate numeric literal.
/// @return Newly allocated formatted string literal, or nullptr for
///         unsupported input.
AST::ExprPtr foldStrLiteral(const AST::Expr &arg);

/// @brief Check whether @p expr can be folded to a literal value.
/// @param expr Candidate binary expression with literal operands.
/// @return True when the dispatcher can currently fold the binary operation.
bool can_fold(const AST::Expr &expr);

/// @brief Attempt to fold @p expr into a freshly allocated literal node.
/// @param expr Candidate binary expression with literal operands.
/// @return Engaged optional containing the replacement node when folding
///         succeeds; std::nullopt otherwise.
std::optional<AST::ExprPtr> fold_expr(const AST::Expr &expr);

} // namespace il::frontends::basic::constfold
