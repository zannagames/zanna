//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Token.hpp
// Purpose: Declares BASIC lexical token classifications, owned lexeme values,
//          source locations, and display-name conversion.
// Key invariants:
//   - TokenKind order is generated from TokenKinds.def and ends with Count.
//   - Count is a sentinel rather than a lexable token.
//   - A Token owns its exact lexeme and carries the location where it begins.
// Ownership/Lifetime:
//   - Token is a copyable value type owning its string.
//   - SourceLoc is stored by value; any referenced source identity remains
//     governed by the source-management layer.
//   - tokenKindToString returns static storage.
// Links: src/frontends/basic/Token.cpp,
//        src/frontends/basic/TokenKinds.def,
//        src/frontends/basic/Lexer.hpp,
//        src/frontends/basic/Parser.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "support/source_location.hpp"
#include <string>

/// @file
/// @brief Declares the token values exchanged by the BASIC lexer and parser.

namespace il::frontends::basic {

/// @brief All token kinds recognized by the BASIC lexer.
/// @details TokenKinds.def provides markers, literals, identifiers, keywords,
///          operators, punctuation, and structural newline tokens in the exact
///          order used by the display-name table.
enum class TokenKind {
#define TOKEN(K, S) K,
#include "frontends/basic/TokenKinds.def"
#undef TOKEN

    Count, ///< Total number of token kinds (sentinel, not a real token).
};

/// @brief A lexical token produced by the BASIC lexer.
/// @details Tokens are value types that own their lexeme and track the
///          starting source location. Identifier lexemes retain any BASIC type
///          suffix so later phases can apply implicit typing rules.
struct Token {
    /// @brief Classification of this token.
    TokenKind kind{TokenKind::Unknown};

    /// @brief Exact character sequence for this token.
    /// @details Owned by this token and preserved without canonicalization.
    std::string lexeme;

    /// @brief Source location where the token begins.
    /// @details Stored by value for diagnostics and AST propagation.
    il::support::SourceLoc loc;
};

/// @brief Returns the generated display spelling for a token kind.
/// @param k Token classification to describe.
/// @return Static null-terminated spelling, or @c "?" for an out-of-range
///         underlying value such as @ref TokenKind::Count.
const char *tokenKindToString(TokenKind k);

} // namespace il::frontends::basic
