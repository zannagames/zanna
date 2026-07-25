//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/TypeSuffix.cpp
// Purpose: Provide the BASIC identifier suffix parser that converts sigils into
//          semantic types used by the lowering pipeline.
// Key invariants:
//   - Only the final identifier byte participates in suffix inference.
//   - $, #/!, and %/& map to string, floating, and integer types respectively.
//   - The optional helper distinguishes no suffix from an explicit integer
//     suffix; the convenience helper defaults either case to I64.
// Ownership/Lifetime:
//   - Helpers are stateless and borrow identifier views only for each call.
// Links: src/frontends/basic/TypeSuffix.hpp,
//        src/frontends/basic/SymbolTable.cpp,
//        src/frontends/basic/Semantic_OOP_Builder.cpp,
//        docs/tutorials/basic-tutorial.md#types
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the mapping between BASIC type suffixes and semantic types.
/// @details Consolidating the logic here avoids exposing parsing helpers in the
///          public header while clearly documenting the defaulting rules applied
///          when a suffix is absent.

#include "frontends/basic/TypeSuffix.hpp"

namespace il::frontends::basic {

/// @brief Deduce the BASIC scalar type represented by an identifier suffix.
/// @details BASIC allows variable names to end in a sigil that encodes the
///          variable's type (for example @c A$ for strings and @c B% for
///          integers). This helper inspects only the final byte and preserves
///          the distinction between an explicit suffix and no suffix.
/// @param name Identifier to inspect; the view is not stored.
/// @return String for @c $, F64 for @c # or @c !, I64 for @c % or @c &, and
///         @c std::nullopt when no recognized suffix is present.
std::optional<Type> inferAstTypeFromSuffix(std::string_view name) {
    if (!name.empty()) {
        switch (name.back()) {
            case '$':
                return Type::Str;
            case '#':
            case '!':
                return Type::F64;
            case '%':
            case '&':
                return Type::I64;
            default:
                break;
        }
    }
    return std::nullopt;
}

/// @brief Infer an identifier's BASIC type from its suffix, defaulting to integer.
/// @param name Identifier to inspect.
/// @return The suffix-implied type, or @ref Type::I64 when the name has no
///         recognized type sigil.
Type inferAstTypeFromName(std::string_view name) {
    if (auto suffixType = inferAstTypeFromSuffix(name))
        return *suffixType;
    return Type::I64;
}

} // namespace il::frontends::basic
