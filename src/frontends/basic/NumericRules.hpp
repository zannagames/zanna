//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/NumericRules.hpp
// Purpose: Unified numeric type rules for BASIC semantic analysis and lowering.
//          Centralizes type predicates, promotion rules, and result type inference
//          that were previously duplicated between SemanticAnalyzer and Lowerer.
// Key invariants: All predicates are constexpr and suitable for use in tight loops.
//                 No heap allocations or string operations in hot paths.
// Ownership/Lifetime: Stateless utilities; no retained resources.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#expressions
//
//===----------------------------------------------------------------------===//

/// @file NumericRules.hpp
/// @brief Defines shared constexpr numeric classification and promotion rules.
/// @details These stateless helpers keep semantic-analysis types and IL lowering
///          types on explicit, parallel paths. Unknown semantic types are
///          accepted by selected predicates to suppress cascading diagnostics;
///          the IL helpers operate only on concrete core type kinds.

#pragma once

#include "frontends/basic/SemanticAnalyzer.hpp"
#include "il/core/Type.hpp"

namespace il::frontends::basic::numeric_rules {

// ============================================================================
// Semantic Type Predicates
// ============================================================================
// These operate on SemanticAnalyzer::Type values used during semantic analysis.

using SemType = SemanticAnalyzer::Type;

/// @brief Check if a semantic type is numeric (int or float).
/// @details Unknown types are considered numeric to avoid cascading errors.
/// @param type Semantic type to check.
/// @return True if the type is Int, Float, or Unknown.
[[nodiscard]] constexpr bool isNumeric(SemType type) noexcept {
    return type == SemType::Int || type == SemType::Float || type == SemType::Unknown;
}

/// @brief Check if a semantic type is an integer type.
/// @details Unknown types are accepted to allow continued validation.
/// @param type Semantic type to check.
/// @return True if the type is Int or Unknown.
[[nodiscard]] constexpr bool isInteger(SemType type) noexcept {
    return type == SemType::Int || type == SemType::Unknown;
}

/// @brief Check if a semantic type is a floating-point type.
/// @param type Semantic type to check.
/// @return True if the type is Float.
[[nodiscard]] constexpr bool isFloat(SemType type) noexcept {
    return type == SemType::Float;
}

/// @brief Check if a semantic type is boolean.
/// @details Unknown types are accepted to allow continued validation.
/// @param type Semantic type to check.
/// @return True if the type is Bool or Unknown.
[[nodiscard]] constexpr bool isBoolean(SemType type) noexcept {
    return type == SemType::Bool || type == SemType::Unknown;
}

/// @brief Check if a semantic type is a string.
/// @param type Semantic type to check.
/// @return True if the type is String.
[[nodiscard]] constexpr bool isString(SemType type) noexcept {
    return type == SemType::String;
}

// ============================================================================
// Semantic Type Promotion
// ============================================================================

/// @brief Compute the common numeric type for two operands.
/// @details If either operand is Float, the result is Float; otherwise Int.
///          Unknown operands are treated as Int to avoid cascading errors.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Promoted numeric type.
[[nodiscard]] constexpr SemType promoteNumeric(SemType lhs, SemType rhs) noexcept {
    return (lhs == SemType::Float || rhs == SemType::Float) ? SemType::Float : SemType::Int;
}

// ============================================================================
// Semantic Result Type Rules
// ============================================================================
// These mirror the rules in TypeRules.cpp but operate on SemanticAnalyzer::Type.

/// @brief Compute result type for arithmetic operations (+, -, *).
/// @details Follows BASIC promotion: Float if either operand is Float.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Result type (Int or Float).
[[nodiscard]] constexpr SemType arithmeticResultType(SemType lhs, SemType rhs) noexcept {
    return promoteNumeric(lhs, rhs);
}

/// @brief Compute result type for division (/).
/// @details BASIC division always produces Float.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return Always Float for valid numeric operands; Unknown otherwise.
[[nodiscard]] constexpr SemType divisionResultType(SemType lhs, SemType rhs) noexcept {
    if (!isNumeric(lhs) || !isNumeric(rhs))
        return SemType::Unknown;
    return SemType::Float;
}

/// @brief Compute result type for integer division (\) and modulus (MOD).
/// @details These operations always produce Int; both input values are ignored.
/// @return Always Int.
[[nodiscard]] constexpr SemType integerOnlyResultType(SemType, SemType) noexcept {
    return SemType::Int;
}

/// @brief Compute result type for exponentiation (^).
/// @details BASIC exponentiation always produces Float (Double in runtime);
///          both input values are ignored.
/// @return Always Float.
[[nodiscard]] constexpr SemType powerResultType(SemType, SemType) noexcept {
    return SemType::Float;
}

/// @brief Compute result type for comparison operators.
/// @details All comparisons produce Bool; both input values are ignored.
/// @return Always Bool.
[[nodiscard]] constexpr SemType comparisonResultType(SemType, SemType) noexcept {
    return SemType::Bool;
}

/// @brief Compute result type for addition including string concatenation.
/// @details If either operand is String, result is String; otherwise numeric.
/// @param lhs Left-hand operand type.
/// @param rhs Right-hand operand type.
/// @return String for string concatenation; otherwise numeric promotion.
[[nodiscard]] constexpr SemType addResultType(SemType lhs, SemType rhs) noexcept {
    if (lhs == SemType::String || rhs == SemType::String)
        return SemType::String;
    return promoteNumeric(lhs, rhs);
}

// ============================================================================
// IL Type Predicates
// ============================================================================
// These operate on il::core::Type::Kind values used during lowering.

using IlKind = il::core::Type::Kind;

/// @brief Check if an IL type is an integer type.
/// @param kind IL type kind to check.
/// @return True if the kind is I16, I32, or I64.
[[nodiscard]] constexpr bool isIlInteger(IlKind kind) noexcept {
    return kind == IlKind::I16 || kind == IlKind::I32 || kind == IlKind::I64;
}

/// @brief Check if an IL type is a floating-point type.
/// @param kind IL type kind to check.
/// @return True if the kind is F64.
[[nodiscard]] constexpr bool isIlFloat(IlKind kind) noexcept {
    return kind == IlKind::F64;
}

/// @brief Check if an IL type is numeric (integer or float).
/// @param kind IL type kind to check.
/// @return True if the kind is an integer or float type.
[[nodiscard]] constexpr bool isIlNumeric(IlKind kind) noexcept {
    return isIlInteger(kind) || isIlFloat(kind);
}

// ============================================================================
// IL Type Promotion
// ============================================================================

/// @brief Compute the common IL integer type for arithmetic.
/// @details Prefers narrowest common type; promotes to I64 when mixed.
/// @param lhs Left-hand operand IL type kind.
/// @param rhs Right-hand operand IL type kind.
/// @return Result IL type kind.
[[nodiscard]] constexpr IlKind promoteIlInteger(IlKind lhs, IlKind rhs) noexcept {
    if (lhs == IlKind::I16 && rhs == IlKind::I16)
        return IlKind::I16;
    if (lhs == IlKind::I32 && rhs == IlKind::I32)
        return IlKind::I32;
    return IlKind::I64;
}

/// @brief Compute the common IL numeric type for arithmetic.
/// @details If either operand is F64, result is F64; otherwise integer promotion.
/// @param lhs Left-hand operand IL type kind.
/// @param rhs Right-hand operand IL type kind.
/// @return Result IL type kind.
[[nodiscard]] constexpr IlKind promoteIlNumeric(IlKind lhs, IlKind rhs) noexcept {
    if (lhs == IlKind::F64 || rhs == IlKind::F64)
        return IlKind::F64;
    return promoteIlInteger(lhs, rhs);
}

// ============================================================================
// Operator Classification
// ============================================================================

/// @brief Check if a binary operator requires floating-point operands.
/// @details Power (^) and BASIC real division (/) always operate on floats.
/// @param op Binary operator to check.
/// @return True if the operator requires float operands.
[[nodiscard]] constexpr bool requiresFloatOperands(BinaryExpr::Op op) noexcept {
    return op == BinaryExpr::Op::Pow || op == BinaryExpr::Op::Div;
}

/// @brief Check if a binary operator requires integer-only operands.
/// @details Integer division (\) and modulus (MOD) require integers.
/// @param op Binary operator to check.
/// @return True if the operator requires integer operands.
[[nodiscard]] constexpr bool requiresIntegerOperands(BinaryExpr::Op op) noexcept {
    return op == BinaryExpr::Op::IDiv || op == BinaryExpr::Op::Mod;
}

/// @brief Check if a binary operator is a comparison.
/// @param op Binary operator to check.
/// @return True if the operator is a comparison (produces boolean).
[[nodiscard]] constexpr bool isComparisonOp(BinaryExpr::Op op) noexcept {
    return op == BinaryExpr::Op::Eq || op == BinaryExpr::Op::Ne || op == BinaryExpr::Op::Lt ||
           op == BinaryExpr::Op::Le || op == BinaryExpr::Op::Gt || op == BinaryExpr::Op::Ge;
}

/// @brief Check if a binary operator is a logical operator.
/// @param op Binary operator to check.
/// @return True if the operator is AND, OR, ANDALSO, or ORELSE.
[[nodiscard]] constexpr bool isLogicalOp(BinaryExpr::Op op) noexcept {
    return op == BinaryExpr::Op::LogicalAnd || op == BinaryExpr::Op::LogicalOr ||
           op == BinaryExpr::Op::LogicalAndShort || op == BinaryExpr::Op::LogicalOrShort;
}

/// @brief Check if a binary operator is arithmetic (+, -, *, /).
/// @param op Binary operator to check.
/// @return True if the operator is arithmetic.
[[nodiscard]] constexpr bool isArithmeticOp(BinaryExpr::Op op) noexcept {
    return op == BinaryExpr::Op::Add || op == BinaryExpr::Op::Sub || op == BinaryExpr::Op::Mul ||
           op == BinaryExpr::Op::Div;
}

} // namespace il::frontends::basic::numeric_rules
