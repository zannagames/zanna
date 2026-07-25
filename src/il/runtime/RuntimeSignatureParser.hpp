//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares parsing utilities for runtime helper signature specifications.
// Runtime helpers (memory allocation, string operations, math functions) have
// signatures encoded as compact string specifications that must be parsed into
// structured type information for IL generation and optimization.
//
// The runtime signature system uses a domain-specific notation to describe function
// signatures compactly: return type and parameter types.
// These specifications are stored in data tables and parsed during compiler initialization
// to build the runtime helper registry. This file provides the parsing infrastructure
// that interprets these specifications.
//
// Specification Format:
// Signatures use a simple text format: "RetType(ParamType1, ParamType2, ...)"
// with type names matching IL core types (i32, f64, ptr, etc.).
//
// The parser handles whitespace, validates type names, splits parameter lists,
// and constructs RuntimeSignature objects that the compiler uses to generate
// correct IL for runtime helper calls.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares parsing utilities for compact runtime ABI signatures.
/// @details Returned token views borrow the caller's specification storage;
///          structured signatures own their translated IL types.

#pragma once

#include "il/runtime/RuntimeSignatures.hpp"

#include <string_view>
#include <vector>

namespace il::runtime {

/// @brief Trim ASCII whitespace from both ends of the provided view.
/// @details Strips leading and trailing spaces, tabs, and newlines from the
///          input string view. The returned view shares storage with @p text
///          and is valid as long as the underlying buffer is alive.
/// @param text Input string view to trim.
/// @return Sub-view of @p text with leading and trailing whitespace removed.
std::string_view trim(std::string_view text);

/// @brief Split a delimited type list into individual type tokens.
/// @details Splits a comma-separated parameter list from a signature
///          specification into individual type token strings. Each returned
///          token is trimmed of surrounding whitespace. For example,
///          splitting `"i64, f64, ptr"` yields `{"i64", "f64", "ptr"}`.
/// @param text Comma-separated parameter list (typically the contents
///             inside the parentheses of a signature specification).
/// @return Vector of trimmed individual type token views.
std::vector<std::string_view> splitTypeList(std::string_view text);

/// @brief Parse a runtime signature specification string into structured form.
/// @details Interprets a compact specification string such as `"f64(i64,ptr)"`
///          and constructs a RuntimeSignature with the parsed return type,
///          parameter type list, and managed-object parameter mask. The format
///          is `ReturnType(Param1, Param2, ...)` where type names correspond to
///          IL core types (i32, i64, f64, ptr, str, i1, void). Invalid or
///          unrecognized specifications have `valid` cleared.
/// @param spec Signature specification string from the runtime data tables.
/// @return Fully populated RuntimeSignature with parsed type information.
RuntimeSignature parseSignatureSpec(std::string_view spec);

} // namespace il::runtime
