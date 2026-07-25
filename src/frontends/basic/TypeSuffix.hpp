//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/TypeSuffix.hpp
// Purpose: Declares BASIC semantic type inference from trailing identifier
//          sigils, with optional and integer-defaulting forms.
// Key invariants:
//   - $, #/!, and %/& map to Str, F64, and I64 respectively.
//   - Unrecognized or missing sigils yield nullopt in the suffix-only form and
//     I64 in the defaulting form.
// Ownership/Lifetime:
//   - Pure stateless helpers borrow each input string view only for the call.
// Links: src/frontends/basic/TypeSuffix.cpp,
//        src/frontends/basic/SymbolTable.cpp,
//        src/frontends/basic/Semantic_OOP_Builder.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"

#include <optional>
#include <string_view>

/// @file
/// @brief Declares trailing-sigil type inference for BASIC identifiers.

namespace il::frontends::basic {

/// @brief Determine the BASIC AST type implied by an identifier suffix.
/// @param name BASIC identifier, potentially containing a type suffix character.
/// @return Semantic type derived from a recognized final sigil, defaulting to
///         @ref Type::I64 when none matches.
Type inferAstTypeFromName(std::string_view name);

/// @brief Inspect @p name and return the AST type encoded by a recognised suffix.
/// @param name Identifier spelling to analyse.
/// @return Optional semantic type when the final byte is one of @c $, @c #,
///         @c !, @c %, or @c &; otherwise @c std::nullopt.
std::optional<Type> inferAstTypeFromSuffix(std::string_view name);

} // namespace il::frontends::basic
