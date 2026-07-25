//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file defines the BasicType enumeration, which represents the BASIC
// language's type system at the frontend level.
//
// BASIC Type System:
// The BASIC frontend currently groups numeric surface spellings into two
// semantic categories:
// - Int: INTEGER, INT, and LONG surface spellings (stored as i64)
// - Float: SINGLE, FLOAT, and DOUBLE surface spellings (stored as f64)
// - String: Variable-length character sequences (suffix: $)
//
// Additionally, the Void type represents procedures (SUB) that do not return
// a value, while Unknown is used during type inference and error recovery.
//
// Type Suffixes:
// BASIC allows variables and functions to declare their type via a suffix:
//   counter% = 42         ' Integer variable
//   distance# = 3.14159   ' Double variable
//   name$ = "Alice"       ' String variable
//
// If no suffix is provided and no explicit DIM statement exists, BASIC defaults
// to integer storage.
//
// Integration:
// - Used by: Parser for type annotation during AST construction
// - Used by: SemanticAnalyzer for type checking and validation
// - Used by: Lowerer for mapping BASIC types to IL primitive types
// - Mapped to IL types: Int→i64, Float→f64, Bool→i1, Object→ptr, String→string
//
// Design Notes:
// - This is a frontend-specific type representation; the lowerer translates
//   BasicType to IL primitive types
// - The enumeration is kept minimal to match BASIC's simple type system
// - Header-only implementation for efficiency
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicTypes.hpp
 * @brief Defines compact BASIC semantic-type and declaration-access enums.
 *
 * BasicType bridges source-level annotations and lowering decisions. Access
 * records the visibility metadata attached to supported declarations.
 */

#pragma once

#include <cstdint>

namespace il::frontends::basic {

/// @brief Enumerates BASIC scalar, object, void, and unresolved types.
enum class BasicType {
    /// Type is absent, not yet inferred, or invalid during recovery.
    Unknown,
    /// Integer category lowered to signed i64.
    Int,
    /// Floating-point category lowered to f64.
    Float,
    /// Variable-length BASIC string value.
    String,
    /// Boolean category lowered to i1.
    Bool,
    /// No value, used for SUB and other non-returning signatures.
    Void,
    /// Runtime-managed object-reference category.
    Object ///< Runtime object reference (lowered to ptr in IL)
};

/// @brief Converts a BasicType to its lowercase BASIC surface spelling.
/// @param t The BASIC type to convert.
/// @return Null-terminated static string naming the type, or `"?"` when @p t
///         does not equal a defined enumerator.
inline const char *toString(BasicType t) {
    switch (t) {
        case BasicType::Unknown:
            return "unknown";
        case BasicType::Int:
            return "int";
        case BasicType::Float:
            return "float";
        case BasicType::String:
            return "string";
        case BasicType::Bool:
            return "boolean";
        case BasicType::Void:
            return "void";
        case BasicType::Object:
            return "object";
    }
    return "?";
}

} // namespace il::frontends::basic

namespace il::frontends::basic {

/// @brief Access control for declarations (default Public).
/// @note Applies to CLASS/TYPE fields and class members.
enum class Access : std::uint8_t {
    /// Visible outside the declaring type or module.
    Public = 0,
    /// Visible only within its declaration scope.
    Private = 1,
};

} // namespace il::frontends::basic
