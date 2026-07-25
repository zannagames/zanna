//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser_Token.cpp
// Purpose: Implement token buffer management utilities for the BASIC parser.
// Links: docs/tutorials/basic-tutorial.md#parser
//
//===----------------------------------------------------------------------===//

/// @file Parser_Token.cpp
/// @brief Provides lookahead, consumption, and error recovery helpers for the BASIC parser.
/// @details Keeping the token-buffer mechanics here keeps the main parser
///          translation units focused on grammar productions while centralising
///          boundary synchronisation policies.

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Parser.hpp"
#include <cstdio>

namespace il::frontends::basic {
// -----------------------------------------------------------------------------
// Token buffer navigation
// -----------------------------------------------------------------------------

/// @brief Check if the next buffered token matches the expected kind.
/// @details Uses @ref peek to ensure the buffer contains at least one token and
///          then compares its kind against @p k without consuming it.  Provides a
///          lightweight predicate used throughout the parser to guard optional
///          productions.
/// @param k Token kind to test against the next token.
/// @return True when the buffered token is of kind @p k; false otherwise.
bool Parser::at(TokenKind k) const {
    return peek().kind == k;
}

/// @brief Provide lookahead into the token stream without consuming tokens.
/// @details Extends the buffered window by repeatedly querying the lexer until
///          the requested lookahead index exists. Distances above 512 return a
///          static synthetic EOF token without growing the buffer. Returned
///          references may be invalidated by later buffer growth or compaction.
/// @param n Lookahead distance, where 0 refers to the current token.
/// @return Reference to the token at position @p n.
/// @pre @p n is non-negative.
const Token &Parser::peek(int n) const {
    /// Sentinel returned when a probe exceeds the bounded lookahead window.
    static const Token kLookaheadLimitToken{
        TokenKind::EndOfFile, "<lookahead-limit>", il::support::SourceLoc{}};
    /// Largest lookahead distance that may cause lexer/token-buffer growth.
    static const int kMaxPeekDistance = 512;
    if (n > kMaxPeekDistance)
        return kLookaheadLimitToken;
    const size_t wantIndex = tokenStart_ + static_cast<size_t>(n);
    while (tokens_.size() <= wantIndex) {
        tokens_.push_back(lexer_.next());
    }
    return tokens_[wantIndex];
}

/// @brief Remove and return the current token.
/// @details Fetches the token via @ref peek to ensure the buffer contains a
///          value, advances the logical start index, and periodically compacts
///          the consumed prefix.
/// @return The token currently at the front of the buffer.
Token Parser::consume() {
    Token t = peek();
    ++tokenStart_;
    compactConsumedTokens();
    return t;
}

/// @brief Erase a sufficiently large consumed prefix from the token buffer.
/// @details Compaction begins only after at least 64 tokens have been consumed
///          and that prefix accounts for at least half the buffer. The logical
///          start index is reset after erasure.
void Parser::compactConsumedTokens() {
    /// Minimum consumed prefix considered for physical vector erasure.
    constexpr size_t kCompactThreshold = 64;
    if (tokenStart_ < kCompactThreshold || tokenStart_ * 2 < tokens_.size())
        return;

    tokens_.erase(tokens_.begin(), tokens_.begin() + static_cast<std::ptrdiff_t>(tokenStart_));
    tokenStart_ = 0;
}

/// @brief Consume the next token when its kind matches the expected value.
/// @details When the lookahead token does not match @p k, the helper emits a
///          diagnostic (or logs a fallback message) and then calls
///          @ref syncToStmtBoundary to recover. The original offending token is
///          returned without any guarantee that it was consumed: boundary
///          tokens remain current.
/// @param k Expected token kind.
/// @return The matched token on success; otherwise the offending token.
Token Parser::expect(TokenKind k) {
    if (!at(k)) {
        Token t = peek();
        if (emitter_) {
            emitter_->emitExpected(t.kind, k, t.loc);
        } else {
            std::fprintf(
                stderr, "expected %s, got %s\n", tokenKindToString(k), tokenKindToString(t.kind));
        }
        syncToStmtBoundary();
        return t;
    }
    return consume();
}

/// @brief Consume a token that may stand in for an identifier in contextual positions.
/// @details BASIC reserves a handful of keywords only in statement/operator
///          positions.  In declaration and lvalue contexts those tokens remain
///          valid names, so callers use this helper instead of duplicating the
///          soft-keyword list.
/// @return The consumed identifier-like token on success, or the original
///         offending token after diagnostic emission and boundary recovery.
Token Parser::expectSoftIdentifier() {
    if (!isSoftIdentToken(peek().kind)) {
        Token t = peek();
        if (emitter_) {
            emitter_->emitExpected(t.kind, TokenKind::Identifier, t.loc);
        } else {
            std::fprintf(stderr,
                         "expected %s, got %s\n",
                         tokenKindToString(TokenKind::Identifier),
                         tokenKindToString(t.kind));
        }
        syncToStmtBoundary();
        return t;
    }
    return consume();
}

/// @brief Discard buffered tokens until a statement boundary is found.
/// @details Used during error recovery, the method consumes tokens until it
///          encounters an end-of-line, colon, or end-of-file token.  It avoids
///          emitting additional diagnostics so callers remain in control of
///          messaging while ensuring the parser resumes at a stable location.
void Parser::syncToStmtBoundary() {
    // Bounded token consumption prevents compiler hang on pathological input.
    /// Token count after which recovery emits an additional limit diagnostic.
    constexpr unsigned kMaxResyncTokens = 10000;
    unsigned consumed = 0;

    while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine) && !at(TokenKind::Colon) &&
           consumed < kMaxResyncTokens) {
        consume();
        ++consumed;
    }
    if (consumed == kMaxResyncTokens && !at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine) &&
        !at(TokenKind::Colon)) {
        emitError("B0004", peek(), "parser recovery exceeded statement resynchronization limit");
        while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine) && !at(TokenKind::Colon))
            consume();
    }
}

} // namespace il::frontends::basic
