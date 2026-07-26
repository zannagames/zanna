//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/TypeParser.hpp
// Purpose: Declares helpers for parsing textual IL type specifiers.
// Key invariants: Type identifiers adhere to docs/il/il-guide.md#reference definitions.
// Ownership/Lifetime: Returned Type objects belong to callers.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares conversion from textual IL type names to core type values.
 *
 * @details The parser recognizes the normative primitive type spellings and
 *          reports recognition through an optional caller-owned status flag,
 *          returning a value object without retaining the input string.
 */

#pragma once

#include "il/core/Type.hpp"

#include <string>

namespace il::io {

/// @brief Parse a textual type token into its IL representation.
/// @param token Lowercase token naming a primitive IL type.
/// @param ok Optional flag receiving true on success and false on failure.
/// @return Parsed il::core::Type value or default constructed on failure.
il::core::Type parseType(const std::string &token, bool *ok = nullptr);

} // namespace il::io
