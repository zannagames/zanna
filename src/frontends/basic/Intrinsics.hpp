//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares a small legacy registry of selected BASIC string and
// conversion intrinsics, providing static signature metadata.
//
// BASIC Intrinsics:
// This table currently contains LEFT$, RIGHT$, MID$, INSTR, LEN, trimming and
// case functions, CHR$, ASC, VAL, and STR$.
//
// Intrinsic Registry:
// Each intrinsic is described by static metadata:
// - Name: Function identifier (e.g., "SIN", "LEFT$")
// - Parameter types: Expected argument types
// - Return type: Result type of the function
// - Arity: Fixed or variable argument count
//
// This registry provides type information for:
// - Semantic validation during analysis
// - IL generation during lowering
// - Error message generation for invalid calls
//
// Type System:
// Intrinsics use BASIC's type system:
// - Int: Integer values (64-bit in this implementation)
// - Float: Floating-point values (64-bit double precision)
// - String: Variable-length character sequences
//
// Integration:
// - Exposes direct lookup and deterministic name dumping for compatibility
// - The primary frontend builtin pipeline is declared in BuiltinRegistry.hpp
//
// Design Notes:
// - Intrinsic descriptors are immutable static data
// - Table covers only the selected legacy signatures declared in Intrinsics.cpp
// - Callers must not free or modify descriptor entries
// - Kept separate from the primary BuiltinRegistry descriptor system
//
//===----------------------------------------------------------------------===//

/**
 * @file Intrinsics.hpp
 * @brief Declares a static signature registry for selected BASIC intrinsics.
 *
 * Lookup is exact and case-sensitive. Returned descriptors and parameter arrays
 * have static storage duration and must not be modified or released.
 */

#pragma once

#include <cstddef>
#include <ostream>
#include <string_view>

namespace il::frontends::basic::intrinsics {

/// @brief Type of parameter or return value for a BASIC intrinsic.
/// @details Numeric accepts or represents either integer or floating-point,
///          depending on the intrinsic and its arguments.
enum class Type {
    Int,    ///< 64-bit integer.
    Float,  ///< 64-bit floating point.
    String, ///< BASIC string.
    Numeric ///< Either Int or Float.
};

/// @brief Parameter descriptor.
/// @details Describes one position in an Intrinsic::params array.
struct Param {
    Type type{Type::Int};  ///< Parameter type.
    bool optional{false};  ///< True if the parameter is optional.
};

/// @brief Intrinsic function descriptor.
/// @invariant @c params addresses static storage when @c paramCount is nonzero.
struct Intrinsic {
    std::string_view name{};       ///< BASIC name including $ suffix.
    Type returnType{Type::Int};    ///< Return type.
    const Param *params{nullptr};  ///< Pointer to ordered parameter descriptors.
    std::size_t paramCount{0};     ///< Number of parameters in @ref params.
};

/// @brief Lookup intrinsic by BASIC name.
/// @param name Exact, case-sensitive name such as "LEFT$".
/// @return Pointer to a static descriptor, or nullptr if unknown.
const Intrinsic *lookup(std::string_view name);

/// @brief Dump all intrinsic names separated by spaces to @p os.
/// @param os Output stream receiving names.
/// @note Emits no leading/trailing space and no terminating newline.
void dumpNames(std::ostream &os);

} // namespace il::frontends::basic::intrinsics
