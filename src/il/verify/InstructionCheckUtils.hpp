//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares low-level utility functions used throughout instruction
// verification. These helpers provide common operations on IL type metadata and
// literal value validation that are needed by multiple verification strategies.
//
// Instruction verification requires checking that literal operands fit within
// their declared types and that opcode metadata correctly describes instruction
// semantics. Rather than duplicating these checks across verification strategies,
// this file centralizes them as reusable building blocks.
//
// Key Responsibilities:
// - Validate integer literals fit within their declared integer kind
// - Map opcode type categories to concrete type kinds when possible
// - Provide type metadata queries used across verification strategies
//
// Design Notes:
// All functions in this file are stateless pure functions operating on IL type
// metadata. They form a utility layer below the main verification logic, providing
// primitive operations used to implement higher-level verification rules. The
// functions are in the il::verify::detail namespace to indicate they are internal
// helpers not part of the public verification API.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares primitive type and integer-range queries for instruction checks.
/// @details These stateless helpers translate fixed opcode type categories,
///          validate integer literal representability, and classify integer
///          widths shared by arithmetic and cast verification.

#pragma once

#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"

#include <optional>

namespace il::verify::detail {

/// @brief Checks whether an integer literal fits within the specified kind.
/// @param value Literal value to test.
/// @param kind Target integer kind describing the representable range.
/// @return True when @p value can be represented without loss.
bool fitsInIntegerKind(long long value, il::core::Type::Kind kind);

/// @brief Maps a type category to a concrete type kind when available.
/// @param category Opcode metadata category to translate.
/// @return Optional kind describing the category, or std::nullopt when dynamic.
std::optional<il::core::Type::Kind> kindFromCategory(il::core::TypeCategory category);

/// @brief Check if a type kind is a supported integer width for arithmetic operations.
/// @details Returns true for I16, I32, and I64 - the integer types that can be
///          used with arithmetic instructions, index operations, and integer casts.
/// @param kind Type kind to check.
/// @return True when @p kind is one of I16, I32, or I64.
inline bool isSupportedIntegerWidth(il::core::Type::Kind kind) {
    return kind == il::core::Type::Kind::I16 || kind == il::core::Type::Kind::I32 ||
           kind == il::core::Type::Kind::I64;
}

/// @brief Check if a type kind is a supported narrowing target width.
/// @details Returns true for I16 and I32 - the integer types that can be
///          targets of narrowing cast operations. I64 is excluded since it cannot
///          be a narrowing target (it's the widest integer type).
/// @param kind Type kind to check.
/// @return True when @p kind is one of I16 or I32.
inline bool isNarrowingTargetWidth(il::core::Type::Kind kind) {
    return kind == il::core::Type::Kind::I16 || kind == il::core::Type::Kind::I32;
}

} // namespace il::verify::detail
