//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/TypeCoercionEngine.hpp
// Purpose: Declares centralized BASIC scalar coercion, type queries, widening,
//          and binary numeric promotion during IL lowering.
// Key invariants:
//   - Same-type coercions emit no conversion instruction.
//   - Boolean-to-integer conversion uses BASIC logical words: true is -1 and
//     false is 0.
//   - F64-to-I64 uses checked round-to-even conversion.
//   - I16 and I32 widen to I64 by sign extension.
//   - Unsupported source or target categories are returned unchanged.
// Ownership/Lifetime:
//   - TypeCoercionEngine stores a non-owning Lowerer reference; emitted values
//     and instructions are owned by that lowerer's active IL context.
// Links: src/frontends/basic/TypeCoercionEngine.cpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/EmitCommon.hpp,
//        docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/basic/LowererTypes.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include "support/source_location.hpp"
#include "zanna/il/IRBuilder.hpp"
#include "zanna/il/Module.hpp"
#include <functional>
#include <optional>
#include <string_view>

/// @file
/// @brief Declares BASIC-to-IL scalar coercion and numeric promotion helpers.

namespace il::frontends::basic {

/// @brief Active BASIC lowering context borrowed for IL emission.
class Lowerer;

/// @brief Centralized type coercion engine for BASIC value conversions.
/// @details Provides one lowering-bound interface for converting scalar
///          @ref RVal values, classifying types, constructing canonical IL
///          types, widening narrow integers, and promoting binary operands.
/// @invariant @ref lowerer_ refers to a live lowerer with an active emission
///            context whenever a conversion emits IL.
class TypeCoercionEngine {
  public:
    /// @brief IL SSA value type manipulated by coercion helpers.
    using Value = il::core::Value;

    /// @brief IL type descriptor used for source and result classification.
    using Type = il::core::Type;

    /// @brief IL opcode type used by the unary emission helper.
    using Opcode = il::core::Opcode;

    /// @brief BASIC semantic scalar type.
    using AstType = ::il::frontends::basic::Type;

    /// @brief Explicit alias used by canonical IL type factories.
    using IlType = il::core::Type;

    /// @brief Construct a coercion engine bound to a lowerer.
    /// @param lowerer Lowerer providing IL emission context.
    explicit TypeCoercionEngine(Lowerer &lowerer) noexcept;

    // =========================================================================
    // Primary Coercion Methods
    // =========================================================================

    /// @brief Coerce a value to 64-bit signed integer.
    /// @param v Value/type pair to convert.
    /// @param loc Source location for emitted conversions.
    /// @return Updated i64 value for supported scalar sources; unsupported
    ///         source kinds are returned unchanged.
    [[nodiscard]] RVal toI64(RVal v, il::support::SourceLoc loc);

    /// @brief Coerce a value to 64-bit floating-point.
    /// @param v Value/type pair to convert.
    /// @param loc Source location for emitted conversions.
    /// @return Updated f64 value for f64 or sources accepted by @ref toI64;
    ///         unsupported source kinds are returned unchanged.
    [[nodiscard]] RVal toF64(RVal v, il::support::SourceLoc loc);

    /// @brief Coerce a value to boolean (i1).
    /// @param v Value/type pair to convert.
    /// @param loc Source location for emitted conversions.
    /// @return Existing i1 value or a value emitted through I64 normalization
    ///         and @ref Opcode::Trunc1.
    [[nodiscard]] RVal toBool(RVal v, il::support::SourceLoc loc);

    /// @brief Coerce a value to a target IL type.
    /// @param v Value/type pair to convert.
    /// @param target Target IL type kind.
    /// @param loc Source location for emitted conversions.
    /// @return Updated value with the target type, or unchanged if no conversion needed.
    [[nodiscard]] RVal toType(RVal v, Type::Kind target, il::support::SourceLoc loc);

    /// @brief Coerce a value to match an AST type.
    /// @param v Value/type pair to convert.
    /// @param target Target AST type.
    /// @param loc Source location for emitted conversions.
    /// @return Updated value with appropriate IL type.
    [[nodiscard]] RVal toAstType(RVal v, AstType target, il::support::SourceLoc loc);

    // =========================================================================
    // Type Queries
    // =========================================================================

    /// @brief Checks whether @p v already has i64 IL type.
    /// @param v Value/type pair to inspect.
    /// @return @c true for @ref Type::Kind::I64.
    [[nodiscard]] static bool isI64(const RVal &v) noexcept;

    /// @brief Checks whether @p v already has f64 IL type.
    /// @param v Value/type pair to inspect.
    /// @return @c true for @ref Type::Kind::F64.
    [[nodiscard]] static bool isF64(const RVal &v) noexcept;

    /// @brief Checks whether @p v already has i1 IL type.
    /// @param v Value/type pair to inspect.
    /// @return @c true for @ref Type::Kind::I1.
    [[nodiscard]] static bool isBool(const RVal &v) noexcept;

    /// @brief Checks whether @p v uses the pointer representation for strings.
    /// @param v Value/type pair to inspect.
    /// @return @c true for any @ref Type::Kind::Ptr value.
    /// @note This representation-level predicate cannot distinguish strings
    ///       from other pointer values.
    [[nodiscard]] static bool isString(const RVal &v) noexcept;

    /// @brief Checks whether @p v has pointer IL type.
    /// @param v Value/type pair to inspect.
    /// @return @c true for @ref Type::Kind::Ptr.
    [[nodiscard]] static bool isPointer(const RVal &v) noexcept;

    /// @brief Check if a type is numeric (integer or float).
    /// @param kind IL type kind to classify.
    /// @return @c true for I1, I16, I32, I64, or F64.
    [[nodiscard]] static bool isNumeric(Type::Kind kind) noexcept;

    /// @brief Check if an AST type is numeric.
    /// @param type BASIC semantic type to classify.
    /// @return @c true for I64, F64, or Bool.
    [[nodiscard]] static bool isNumericAst(AstType type) noexcept;

    // =========================================================================
    // IL Type Helpers
    // =========================================================================

    /// @brief Get the IL boolean type (i1).
    /// @return Canonical I1 type descriptor.
    [[nodiscard]] static IlType boolType() noexcept;

    /// @brief Get the IL integer type (i64).
    /// @return Canonical I64 type descriptor.
    [[nodiscard]] static IlType intType() noexcept;

    /// @brief Get the IL float type (f64).
    /// @return Canonical F64 type descriptor.
    [[nodiscard]] static IlType floatType() noexcept;

    /// @brief Get the IL pointer type.
    /// @return Canonical pointer type descriptor.
    [[nodiscard]] static IlType ptrType() noexcept;

    /// @brief Convert an AST type to its corresponding IL type.
    /// @param type BASIC semantic type to map.
    /// @return I64, F64, I1, or pointer for recognized scalar types; all other
    ///         values conservatively map to I64.
    [[nodiscard]] static IlType astToIl(AstType type) noexcept;

    // =========================================================================
    // Widening Helpers
    // =========================================================================

    /// @brief Sign-extend a narrower integer to 64 bits.
    /// @param v Value to widen.
    /// @param fromBits Original bit width (16 or 32).
    /// @param loc Source location.
    /// @return Widened value as i64.
    [[nodiscard]] Value widenToI64(Value v, int fromBits, il::support::SourceLoc loc);

    // =========================================================================
    // Promotion Rules
    // =========================================================================

    /// @brief Determine the common type for binary operations.
    /// @details Implements BASIC numeric promotion rules:
    ///          - If either operand is float, result is float
    ///          - Otherwise, result is integer
    /// @param lhs Left operand type.
    /// @param rhs Right operand type.
    /// @return Common type for the operation.
    [[nodiscard]] static Type::Kind promoteNumeric(Type::Kind lhs, Type::Kind rhs) noexcept;

    /// @brief Coerce both operands to a common type.
    /// @param lhs Left operand (modified in place).
    /// @param rhs Right operand (modified in place).
    /// @param loc Source location for emitted conversions.
    void promoteOperands(RVal &lhs, RVal &rhs, il::support::SourceLoc loc);

  private:
    /// Non-owning lowering context used for every emitted instruction.
    Lowerer &lowerer_;

    /// @brief Emits one unary conversion instruction through the lowerer.
    /// @param op Conversion opcode.
    /// @param resultType Declared result type.
    /// @param operand SSA operand to convert.
    /// @return Newly emitted SSA result.
    [[nodiscard]] Value emitUnary(Opcode op, Type resultType, Value operand);

    /// @brief Maps an i1 value to BASIC's signed logical-word representation.
    /// @param boolVal Boolean SSA value.
    /// @return I64 SSA value equal to -1 for true or 0 for false.
    [[nodiscard]] Value emitBoolToLogicalI64(Value boolVal);
};

} // namespace il::frontends::basic
