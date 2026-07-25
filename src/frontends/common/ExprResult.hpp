//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/ExprResult.hpp
// Purpose: Common expression result type for all language frontends.
//
// This type represents the result of lowering an expression: the IL value
// produced and its IL type. All language frontends use this unified
// abstraction for expression lowering results.
//
// Key invariants:
//   * value identifies the lowered IL value when isValid() is true.
//   * type describes the IL representation of value.
// Ownership: Stores IL Value and Type objects by value.
// References: src/il/core/Value.hpp, src/il/core/Type.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the value/type pair returned by frontend expression lowering.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

namespace il::frontends::common {

/// @brief Result of lowering an expression to a value and type pair.
/// @details This is the common abstraction shared by all frontends for
///          representing the output of expression lowering.
struct ExprResult {
    il::core::Value value; ///< The lowered value (temp, const, or global).
    il::core::Type type;   ///< The IL type of the value.

    /// @brief Default constructor creates an invalid result.
    /// @post isValid() is false for the default-constructed type.
    ExprResult() = default;

    /// @brief Construct with a value and type.
    /// @param v Lowered IL value.
    /// @param t IL type associated with @p v.
    ExprResult(il::core::Value v, il::core::Type t) : value(v), type(t) {}

    /// @brief Check if this result has a usable, non-error IL type.
    /// @return True unless the type is Void or Error.
    [[nodiscard]] bool isValid() const noexcept {
        return type.kind != il::core::Type::Kind::Void &&
               type.kind != il::core::Type::Kind::Error;
    }

    /// @brief Check if this is an integer type.
    /// @return True for I1, I16, I32, or I64.
    [[nodiscard]] bool isInteger() const noexcept {
        return type.kind == il::core::Type::Kind::I64 || type.kind == il::core::Type::Kind::I32 ||
               type.kind == il::core::Type::Kind::I16 || type.kind == il::core::Type::Kind::I1;
    }

    /// @brief Check if this is a floating-point type.
    /// @return True only for F64.
    [[nodiscard]] bool isFloat() const noexcept {
        return type.kind == il::core::Type::Kind::F64;
    }

    /// @brief Check if this is a string type.
    /// @return True only for Str.
    [[nodiscard]] bool isString() const noexcept {
        return type.kind == il::core::Type::Kind::Str;
    }

    /// @brief Check if this is a boolean type.
    /// @return True only for I1.
    [[nodiscard]] bool isBool() const noexcept {
        return type.kind == il::core::Type::Kind::I1;
    }

    /// @brief Check if this is a pointer type.
    /// @return True only for Ptr.
    [[nodiscard]] bool isPointer() const noexcept {
        return type.kind == il::core::Type::Kind::Ptr;
    }
};

} // namespace il::frontends::common
