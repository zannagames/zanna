//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/ParserUtil.hpp
// Purpose: Declares lexical helpers shared by IL parser components.
// Key invariants: None.
// Ownership/Lifetime: Stateless utility routines operate on caller-provided data.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares shared lexical and diagnostic utilities for IL parsing.
 *
 * @details These stateless helpers normalize whitespace/comments, extract
 *          delimited tokens, validate identifiers, parse numeric and trap-kind
 *          literals, recognize explicit SSA IDs, and construct consistently
 *          line-prefixed diagnostics from caller-owned text and streams.
 */

#pragma once

#include "support/diag_expected.hpp"
#include "support/source_location.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace il::io {

/// @brief Remove leading and trailing whitespace from the supplied text.
/// @param text Input string that may contain surrounding whitespace.
/// @return Substring view with surrounding whitespace stripped.
std::string trim(const std::string &text);

/// @brief Remove inline IL comments while preserving quoted string contents.
/// @details Treats `#` and `//` outside double-quoted strings as comment
///          introducers when they start a line or follow whitespace. Backslash
///          escapes inside strings are honoured.
/// @param text Source line to process.
/// @return Text before the first inline comment marker.
std::string stripInlineComment(const std::string &text);

/// @brief Remove module-declaration trailing comments while preserving strings.
/// @details Handles the same quoted-string and backslash escaping rules as
///          @ref stripInlineComment, and additionally treats semicolons as
///          declaration comments when they begin a line or follow whitespace.
/// @param text Source declaration tail to process.
/// @return Text before the first declaration comment marker.
std::string stripDeclarationComment(const std::string &text);

/// @brief Delimiter consumed after a parser utility token.
/// @details Distinguishes input exhaustion, a comma separator, and ordinary
///          whitespace so opcode parsers can enforce grammar-specific layouts.
enum class TokenDelimiter {
    End,
    Comma,
    Whitespace,
};

/// @brief Extract the next comma or whitespace delimited token from a stream.
/// @param stream Source stream backed by an instruction tail segment.
/// @return Token without any trailing comma delimiter.
std::string readToken(std::istringstream &stream);

/// @brief Extract the next token and report how it was separated from following text.
/// @param stream Source stream backed by an instruction tail segment.
/// @param delimiter Optional output receiving end/comma/whitespace classification.
/// @return Token without its delimiter.
std::string readToken(std::istringstream &stream, TokenDelimiter *delimiter);

/// @brief Validate an IL symbol/label fragment after removing sigils.
/// @param text Candidate identifier text without leading @, %, or ^.
/// @return True when @p text is non-empty and contains no IL delimiters.
bool isValidILIdentifier(std::string_view text);

/// @brief Format a diagnostic string including the line number prefix used by the parser.
/// @param lineNo Line number associated with the diagnostic message.
/// @param message Human-readable diagnostic message body.
/// @return Formatted diagnostic string, e.g. "Line 3: malformed foo".
std::string formatLineDiag(unsigned lineNo, std::string_view message);

/// @brief Attempt to parse an integer literal token.
/// @param token Textual representation of a signed integer.
/// @param value Destination receiving the parsed value on success.
/// @return True if the entire token was consumed as an integer.
bool parseIntegerLiteral(const std::string &token, long long &value);

/// @brief Parse serializer-style temp names such as "t42" into their numeric id.
/// @param name Identifier text without the leading '%' sigil.
/// @return Temp id when @p name is exactly `t` followed by decimal digits.
std::optional<unsigned> parseExplicitTempName(std::string_view name);

/// @brief Attempt to parse a floating-point literal token.
/// @param token Textual representation of a floating value.
/// @param value Destination receiving the parsed value on success.
/// @return True if the entire token was consumed as a floating literal.
bool parseFloatLiteral(const std::string &token, double &value);

/// @brief Attempt to parse a trap kind identifier token.
/// @param token Candidate identifier, e.g. "DivideByZero".
/// @param value Destination receiving the enumerated integral value on success.
/// @return True when @p token matches a known trap kind identifier.
bool parseTrapKindToken(const std::string &token, long long &value);

/// @brief Map a trap kind integral value back to its identifier spelling.
/// @param value Enumerated integral trap kind value.
/// @return Identifier string when known; std::nullopt otherwise.
std::optional<std::string_view> trapKindTokenFromValue(long long value);

/// @brief Construct a Diag with a standard "line N: <message>" prefix.
/// @param loc Optional source location for the diagnostic.
/// @param lineNo Line number to embed in the message prefix.
/// @param message Human-readable message body.
/// @return Populated diagnostic object.
inline il::support::Diag makeLineErrorDiag(il::support::SourceLoc loc,
                                           unsigned lineNo,
                                           std::string_view message) {
    return il::support::makeError(loc, formatLineDiag(lineNo, message));
}

} // namespace il::io
