//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the Lexer class, which performs lexical analysis of BASIC
// source code and produces a stream of tokens for the parser.
//
// The lexer is the first stage of the BASIC frontend compilation pipeline:
//   Lexer -> Parser -> AST -> Semantic -> Lowerer -> IL
//
// Key Responsibilities:
// - Tokenizes BASIC source text into lexical tokens (keywords, identifiers,
//   literals, operators, punctuation)
// - Recognizes BASIC-specific constructs including:
//   * Keywords (IF, THEN, FOR, NEXT, DIM, SUB, FUNCTION, etc.)
//   * Type suffixes (%, &, !, #, $) for integer, long, single, double, string
//   * Numeric literals (integer, floating-point, scientific notation)
//   * String literals with doubled-quote escaping
//   * Line numbers and labels
//   * Comment syntax (REM statements and ' single-line comments)
// - Maintains source location information for diagnostic reporting
// - Provides efficient single-pass scanning with minimal lookahead
//
// Design Notes:
// - The lexer does not own the source buffer; callers must ensure the buffer
//   remains valid for the lexer's lifetime
// - Character position tracking enables accurate error reporting during parsing
//   and semantic analysis
// - Whitespace (spaces, tabs) is skipped, but newlines are preserved as tokens
//   since BASIC uses line-oriented syntax
// - The lexer handles both traditional BASIC line numbers and modern label-based
//   control flow
// - Cursor management (peek/get/eof/position tracking) is inherited from
//   LexerCursor<Lexer> via CRTP, shared with the Zia frontend
//
// Usage:
//   Lexer lex(sourceText, fileId);
//   Token tok;
//   while ((tok = lex.next()).kind != TokenKind::EndOfFile) {
//     // Process token
//   }
//
//===----------------------------------------------------------------------===//

/**
 * @file Lexer.hpp
 * @brief Declares single-pass lexical analysis for BASIC source text.
 *
 * The lexer borrows its source view, owns each returned token lexeme, preserves
 * logical line boundaries, and represents malformed or over-limit lexemes as
 * TokenKind::Unknown for the parser to diagnose.
 */

#pragma once

#include "frontends/basic/Token.hpp"
#include "frontends/common/LexerBase.hpp"
#include <string_view>

namespace il::frontends::basic {

/// @brief Tokenizes BASIC source text into a stream of tokens.
/// @details Construct with a source buffer and file identifier, then call
/// next() repeatedly to iterate through tokens until an EOF token is returned.
/// Inherits cursor management (peek/get/eof/position tracking) from
/// LexerCursor<Lexer> via CRTP.
class Lexer : public il::frontends::common::lexer_base::LexerCursor<Lexer> {
    /// CRTP cursor base providing position, lookahead, consumption, and EOF state.
    using Base = il::frontends::common::lexer_base::LexerCursor<Lexer>;

  public:
    /// @brief Create a lexer over the given source buffer.
    /// @param src Source text to tokenize. The lexer does not take ownership.
    /// @param file_id Identifier of the source file for diagnostics.
    Lexer(std::string_view src, uint32_t file_id);

    /// @brief Produce the next token in the source.
    /// @details Repeated calls after input exhaustion continue returning
    ///          EndOfFile at the current location.
    /// @return The next lexical token, or an EOF token when no characters
    /// remain.
    Token next();

    /// @brief Provide the source buffer to the CRTP base class.
    /// @return View of the source text being tokenized.
    /// @warning The view aliases caller-owned storage and is valid only while
    ///          that storage remains alive.
    [[nodiscard]] std::string_view source() const {
        return src_;
    }

  private:
    /// @brief Skip spaces and tabs but leave newlines intact.
    /// @post The cursor is at EOF, a line ending, or the next non-horizontal-space byte.
    void skipWhitespaceExceptNewline();

    /// @brief Skip spaces, tabs, and BASIC comments starting with `'` or REM.
    /// @details REM begins a comment only when not followed by an alphanumeric
    ///          byte or a BASIC type suffix. The terminating LF is preserved.
    /// @post The cursor is positioned at a significant byte, LF, or EOF.
    void skipWhitespaceAndComments();

    /// @brief Lex a based integer literal (`&H`, `&B`, `0x`, or `0b`).
    /// @details Consumes the radix prefix, radix-appropriate digits, and an
    ///          optional integer suffix (`%` or `&`). The returned lexeme is
    ///          canonicalized to uppercase where case is relevant. Prefixes
    ///          without digits and literals reaching the 1024-byte limit return
    ///          Unknown after consuming the remaining radix digits.
    /// @return Number token for a valid literal, otherwise Unknown.
    Token lexBasedNumber();

    /// @brief Lex a numeric literal including optional decimal point and exponent.
    /// @details Consumes digit sequences, optional decimal fraction, optional
    ///          exponent (E/e followed by optional +/- and digits), and optional
    ///          type suffix (#, !, &, %). A missing exponent digit or a numeric
    ///          body reaching the 1024-byte limit yields Unknown.
    /// @return Number token for a valid scanned form, otherwise Unknown.
    Token lexNumber();

    /// @brief Lex an identifier or keyword.
    /// @details Called only after next() observes an ASCII-style letter.
    ///          Consumes alphanumerics/underscores, plus one optional type suffix
    ///          ($, #, !, &, %), and uppercases the lexeme before exact keyword
    ///          lookup. Identifiers reaching 1024 bytes yield Unknown.
    /// @return Identifier token or the corresponding keyword kind.
    Token lexIdentifierOrKeyword();

    /// @brief Lex a string literal enclosed in double quotes.
    /// @details Converts each doubled quote (`""`) to one quote byte. Backslash
    ///          has no special meaning. A newline, EOF before closure, or the
    ///          16 MiB limit yields Unknown; this routine does not emit diagnostics.
    /// @return String token containing the decoded interior, otherwise Unknown.
    Token lexString();

    /// @brief Borrowed source bytes; caller storage must outlive the lexer.
    std::string_view src_; ///< Source code being tokenized.
};

} // namespace il::frontends::basic
