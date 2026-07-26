//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/io/OperandParse_Const.cpp
// Purpose: Provide the per-kind parser for constant literal operands.
// Key invariants: Preserves OperandParser diagnostics and literal handling
//                 semantics for integers, floats, booleans, null, and strings.
// Ownership/Lifetime: Operates on parser-managed state without owning data and
//                     never allocates process-global resources.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the helper that parses constant literal operands.
/// @details The helper mirrors the legacy literal decoding rules, including
///          support for numeric suffixes and escaped string payloads, producing
///          il::core::Value instances identical to the historical parser.

#include "zanna/il/io/OperandParse.hpp"

#include "il/core/Instr.hpp"
#include "il/core/Value.hpp"
#include "il/io/StringEscape.hpp"

#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace zanna::il::io {
namespace {

/// @brief Compare ASCII strings without considering letter case.
/// @details Literal parsing must recognise canonical spellings like "INF" and
///          "Inf" regardless of how the user wrote them.  This helper lowers
///          characters manually instead of relying on locale-aware facilities so
///          behaviour remains deterministic across hosts.
/// @param value Source token extracted from the IL stream.
/// @param literal Reference literal to compare against.
/// @return @c true when both strings match ignoring ASCII case, otherwise @c false.
bool equalsIgnoreCase(std::string_view value, std::string_view literal) {
    if (value.size() != literal.size())
        return false;
    for (std::size_t index = 0; index < literal.size(); ++index) {
        const unsigned char lhs = static_cast<unsigned char>(value[index]);
        const unsigned char rhs = static_cast<unsigned char>(literal[index]);
        if (std::tolower(lhs) != std::tolower(rhs))
            return false;
    }
    return true;
}

/// @brief Consume the next whitespace-delimited token from the IL cursor.
/// @details The cursor hands back a view of the consumed characters and the
///          caller is responsible for trimming trailing delimiters such as
///          commas.  Returning `std::nullopt` allows the caller to emit a tailored
///          diagnostic when the operand list unexpectedly ends.
/// @param cur Cursor positioned at the beginning of the token.
/// @return View representing the consumed token or `std::nullopt` when no bytes remain.
std::optional<std::string_view> consumeToken(zanna::parse::Cursor &cur) {
    cur.skipWs();
    const std::size_t begin = cur.offset();
    /// @brief Tests whether a character remains part of the current token.
    /// @param ch Character to inspect.
    /// @return `true` until whitespace is reached.
    const std::string_view token =
        cur.consumeWhile([](char ch) { return !std::isspace(static_cast<unsigned char>(ch)); });
    if (token.empty())
        return std::nullopt;
    cur.seek(begin + token.size());
    return token;
}

/// @brief Decode a quoted string literal operand from the cursor.
/// @details Copies characters out of the cursor while tracking escape
///          sequences, then delegates to @ref ::il::io::decodeEscapedString to
///          expand escapes into their runtime form.  The helper updates the
///          cursor position so subsequent parsers resume at the first
///          unconsumed byte following the literal.
/// @param cur Cursor positioned at the opening quote.
/// @param ctx Parser context used for diagnostic emission.
/// @return Parse result whose value contains the decoded string on success.
ParseResult parseStringLiteral(zanna::parse::Cursor &cur, Context &ctx) {
    const std::size_t begin = cur.offset();
    cur.consume('"');
    std::string literal;
    bool escape = false;
    while (!cur.atEnd()) {
        const char ch = cur.peek();
        cur.advance();
        if (escape) {
            literal.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            literal.push_back(ch);
            escape = true;
            continue;
        }
        literal.push_back(ch);
        if (ch == '"')
            break;
    }

    if (literal.empty() || literal.back() != '"')
        return syntaxError(ctx, "unterminated string literal");

    std::string payload = literal.substr(0, literal.size() - 1);
    std::string decoded;
    std::string err;
    if (!::il::io::decodeEscapedString(payload, decoded, &err))
        return syntaxError(ctx, err);

    ParseResult result;
    result.value = ::il::core::Value::constStr(std::move(decoded));
    cur.seek(begin + 1 + literal.size());
    return result;
}

/// @brief Interpret @p token as either an integer or floating-point literal.
/// @details Examines the token for decimal points, exponent markers, or
///          well-known floating spellings (INF/NAN) before dispatching to the
///          shared literal parsing helpers.  Diagnostics match the historical
///          operand parser so tools that diff output remain stable.
/// @param token Literal token stripped of trailing delimiters and whitespace.
/// @param ctx Parser context that receives diagnostics when parsing fails.
/// @return Parse result containing the parsed value or an error status.
ParseResult parseNumericLiteral(const std::string &token, Context &ctx) {
    ParseResult result;

    const bool hasDecimalPoint = token.find('.') != std::string::npos;
    size_t prefixPos = 0;
    if (prefixPos < token.size() && (token[prefixPos] == '+' || token[prefixPos] == '-'))
        ++prefixPos;
    const bool isHexLiteral = token.size() >= prefixPos + 2 && token[prefixPos] == '0' &&
                              (token[prefixPos + 1] == 'x' || token[prefixPos + 1] == 'X');
    const bool hasExponent = (!isHexLiteral) && (token.find('e') != std::string::npos ||
                                                 token.find('E') != std::string::npos);

    /// @brief Parses a floating literal and preserves historical diagnostics.
    /// @param literal Literal spelling.
    /// @return Successful float value or syntax-error result.
    auto handleFloat = [&](const std::string &literal) -> ParseResult {
        double value = 0.0;
        if (::il::io::parseFloatLiteral(literal, value)) {
            result.value = ::il::core::Value::constFloat(value);
            return result;
        }
        std::ostringstream oss;
        oss << "invalid floating literal '" << literal << "'";
        return syntaxError(ctx, oss.str());
    };

    if (hasDecimalPoint || hasExponent || equalsIgnoreCase(token, "nan") ||
        equalsIgnoreCase(token, "inf") || equalsIgnoreCase(token, "+inf") ||
        equalsIgnoreCase(token, "-inf"))
        return handleFloat(token);

    long long intValue = 0;
    if (::il::io::parseIntegerLiteral(token, intValue)) {
        result.value = ::il::core::Value::constInt(intValue);
        return result;
    }

    std::ostringstream oss;
    oss << "invalid integer literal '" << token << "'";
    return syntaxError(ctx, oss.str());
}

} // namespace

/// @brief Parse a literal constant operand from the IL token stream.
/// @details Handles strings, booleans, `null`, and numeric literals while
///          trimming delimiter characters that separate operands.  Each case
///          delegates to a specialist helper to keep the control flow readable
///          and to reuse shared validation routines.  Diagnostics are emitted via
///          @ref syntaxError so the parser maintains consistent formatting.
/// @param cur Cursor describing the remaining operand text.
/// @param ctx Parser context capturing diagnostics and results.
/// @return Parse result containing the decoded literal or an error.
ParseResult parseConstOperand(zanna::parse::Cursor &cur, Context &ctx) {
    cur.skipWs();
    if (cur.atEnd())
        return syntaxError(ctx, "missing operand");

    if (cur.peek() == '"')
        return parseStringLiteral(cur, ctx);

    auto tokenView = consumeToken(cur);
    if (!tokenView)
        return syntaxError(ctx, "missing operand");

    std::string token(tokenView->begin(), tokenView->end());
    if (!token.empty() && (token.back() == ',' || token.back() == ')')) {
        char tail = token.back();
        token.pop_back();
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.pop_back();
        if (token.empty()) {
            std::string msg =
                tail == ',' ? "missing operand before ','" : "missing operand before ')'";
            return syntaxError(ctx, msg);
        }
    }

    if (equalsIgnoreCase(token, "true")) {
        ParseResult result;
        result.value = ::il::core::Value::constBool(true);
        return result;
    }
    if (equalsIgnoreCase(token, "false")) {
        ParseResult result;
        result.value = ::il::core::Value::constBool(false);
        return result;
    }
    if (token == "null") {
        ParseResult result;
        result.value = ::il::core::Value::null();
        return result;
    }

    return parseNumericLiteral(token, ctx);
}

} // namespace zanna::il::io
