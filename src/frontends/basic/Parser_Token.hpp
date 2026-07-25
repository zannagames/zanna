//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser_Token.hpp
// Purpose: Token navigation helpers for BASIC parser.
// Key invariants: Buffer always holds current token.
// Ownership/Lifetime: Parser owns lexer and token buffer.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file Parser_Token.hpp
/// @brief Declares the token-navigation fragment included inside Parser.
/// @details This header is intentionally textually included in the private
///          section of `Parser`; its unqualified declarations and inline static
///          helpers are Parser members, not namespace-level APIs.

#pragma once

#include <cctype>

/// @brief Test whether the current token matches a given kind.
/// @param k Token kind to compare against the current token.
/// @return True if the current token is of kind @p k; otherwise false.
/// @note Does not modify parser state.
bool at(TokenKind k) const;

/// @brief Look ahead without consuming tokens.
/// @param n Number of tokens ahead to inspect; 0 refers to the current token.
/// @return Reference to the token at the specified lookahead position.
/// @pre @p n is non-negative.
/// @note Does not advance the logical position, but may lex and append buffered
///       tokens. Distances above 512 return a synthetic EOF sentinel.
const Token &peek(int n = 0) const;

/// @brief Consume the current token and advance.
/// @return The token that was at the current position before advancing.
/// @note Advances the parser by one token.
Token consume();

/// @brief Consume a token of the expected kind or report a mismatch.
/// @param k Required token kind.
/// @return Consumed matching token, or the original offending token after recovery.
/// @note A mismatch may consume intervening recovery tokens but leaves a
///       statement-boundary offender unconsumed.
Token expect(TokenKind k);

/// @brief Consume an identifier or contextual keyword accepted as an identifier.
/// @return Consumed matching token, or the original offending token after recovery.
Token expectSoftIdentifier();

/// @brief Skip tokens until reaching a statement boundary.
/// @note Consumes tokens until a boundary token (e.g., newline or EOF) is encountered.
void syncToStmtBoundary();

/// @brief Check if a token kind is a "soft identifier" (identifier or contextual keyword).
/// @details Keywords like COLOR, FLOOR, RANDOM, NEXT, BASE, COS, SIN, POW, and APPEND can be
///          used as identifiers in contexts like qualified names (Zanna.Terminal.Color,
///          Zanna.Random.Next) or as variable/field names (DIM base AS INTEGER).
///          This allows them to be treated as identifiers when not in keyword context.
/// @param k Token kind to test.
/// @return True if the token can be treated as an identifier in appropriate contexts.
static bool isSoftIdentToken(TokenKind k) {
    switch (k) {
        case TokenKind::Identifier:
        case TokenKind::KeywordColor:
        case TokenKind::KeywordFloor:
        case TokenKind::KeywordRandom:
        case TokenKind::KeywordNext: // BUG-OOP-040: For Zanna.Random.Next()
        case TokenKind::KeywordBase: // BUG-OOP-042: Allow "base" as variable/field name
        case TokenKind::KeywordCos:
        case TokenKind::KeywordSin:
        case TokenKind::KeywordPow:
        case TokenKind::KeywordAppend:
            return true;
        default:
            return false;
    }
}

/// @brief Check if a token kind can appear as a member/qualified name segment.
/// @details This accepts identifiers and keywords after a dot to support runtime
///          namespaces like Zanna.IO.File.Delete or Zanna.Threads.Thread.Sleep.
/// @param k Token kind to test.
/// @return True if the token can be treated as an identifier after '.'.
static bool isMemberIdentToken(TokenKind k) {
    if (k == TokenKind::Identifier)
        return true;
    const char *spelling = tokenKindToString(k);
    if (!spelling || spelling[0] == '\0')
        return false;
    unsigned char first = static_cast<unsigned char>(spelling[0]);
    return std::isalpha(first) && std::isupper(first);
}
