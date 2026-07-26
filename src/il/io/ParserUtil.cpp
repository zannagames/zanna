//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements lexical helper functions used by the IL parser.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Token-level utilities shared by the textual IL parser.
/// @details Supplies trimming, tokenisation, literal parsing, and trap-kind
///          mapping helpers that keep the main parser logic concise.

#include "il/internal/io/ParserUtil.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>

namespace {
/// @brief Associates a textual trap-kind mnemonic with its stable numeric code.
struct TrapKindSymbol {
    /// Canonical token accepted and emitted by textual IL.
    const char *name;
    /// Runtime trap-kind value represented by @ref name.
    long long value;
};

/// @brief Complete bidirectional trap-kind spelling table.
constexpr std::array<TrapKindSymbol, 12> kTrapKindSymbols = {{
    {"DivideByZero", 0},
    {"Overflow", 1},
    {"InvalidCast", 2},
    {"DomainError", 3},
    {"Bounds", 4},
    {"FileNotFound", 5},
    {"EOF", 6},
    {"IOError", 7},
    {"InvalidOperation", 8},
    {"RuntimeError", 9},
    {"Interrupt", 10},
    {"NetworkError", 11},
}};
} // namespace

namespace il::io {

/// @brief Strip leading and trailing ASCII whitespace from the supplied text.
///
/// Uses std::isspace to scan from both ends of the buffer and returns a
/// substring containing the original content without surrounding whitespace.
/// Interior characters are preserved verbatim, so callers can safely trim
/// instruction tokens or directive fields before further parsing.
///
/// @param text Input text that may contain leading or trailing whitespace.
/// @return Copy of @p text without surrounding whitespace characters.
std::string trim(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

/// @brief Strip comments while preserving quoted text.
/// @details Shared implementation for normal IL source comments and module
///          declaration comments.  The declaration mode additionally treats a
///          semicolon at a token boundary as a comment marker.
/// @param text Input source text.
/// @param allowSemicolon Whether semicolon can introduce a comment.
/// @return Prefix before the first matching comment marker.
std::string stripCommentImpl(const std::string &text, bool allowSemicolon) {
    bool inString = false;
    bool escape = false;
    const auto beginsComment = [&](size_t pos) {
        return pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1]));
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString) {
            if (escape) {
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (ch == '"')
                inString = false;
            continue;
        }

        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '#' && beginsComment(i))
            return text.substr(0, i);
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/' && beginsComment(i))
            return text.substr(0, i);
        if (allowSemicolon && ch == ';' && beginsComment(i))
            return text.substr(0, i);
    }
    return text;
}

/// @brief Strip inline comments from an IL source line.
///
/// `#` and `//` begin comments only when they occur outside a quoted string and
/// are at the beginning of a line or preceded by whitespace. This preserves IL
/// identifiers such as BASIC-style `%name#` and `@F#`.
/// @param text Source line to scan.
/// @return Prefix preceding the first eligible inline comment marker.
std::string stripInlineComment(const std::string &text) {
    return stripCommentImpl(text, false);
}

/// @brief Strip comments from a module declaration while preserving quoted text.
///
/// This routine is used for directive tails such as extern attributes and global
/// initializers where semicolon is also accepted as a comment introducer.  Comment
/// markers inside double-quoted strings are ignored, including escaped quotes.
///
/// @param text Input declaration text.
/// @return Prefix before the first declaration comment marker.
std::string stripDeclarationComment(const std::string &text) {
    return trim(stripCommentImpl(text, true));
}

/// @brief Read the next token from a comma-delimited instruction tail segment.
///
/// Extraction skips leading whitespace and gathers either a quoted string
/// literal (including embedded spaces) or a whitespace-delimited token. A
/// trailing comma is removed so the returned token can be parsed directly.
///
/// @param stream Backing stream positioned at the next token to extract.
/// @param delimiter Optional output describing what terminated the token.
/// @return Token read from @p stream with any trailing comma stripped.
/// @note An unterminated quoted token or missing token sets @p stream's fail bit.
std::string readToken(std::istringstream &stream, TokenDelimiter *delimiter) {
    if (delimiter)
        *delimiter = TokenDelimiter::End;
    stream >> std::ws;
    if (!stream.good()) {
        stream.setstate(std::ios::failbit);
        return {};
    }

    std::string token;
    if (stream.peek() == '"') {
        char c = '\0';
        stream.get(c);
        token.push_back(c);

        bool escape = false;
        bool closed = false;
        while (stream.get(c)) {
            token.push_back(c);
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') {
                closed = true;
                break;
            }
        }

        if (!closed) {
            stream.setstate(std::ios::failbit);
            return token;
        }

        stream >> std::ws;
        if (stream.peek() == ',') {
            stream.get();
            if (delimiter)
                *delimiter = TokenDelimiter::Comma;
        } else if (stream.peek() != std::char_traits<char>::eof()) {
            if (delimiter)
                *delimiter = TokenDelimiter::Whitespace;
        }
        if (stream.eof())
            stream.clear(stream.rdstate() & ~std::ios::failbit);
        return token;
    }

    char c = '\0';
    bool stoppedOnWhitespace = false;
    while (stream.get(c)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            stoppedOnWhitespace = true;
            break;
        }
        if (c == ',') {
            if (delimiter)
                *delimiter = TokenDelimiter::Comma;
            break;
        }
        token.push_back(c);
    }
    if (token.empty()) {
        stream.setstate(std::ios::failbit);
    } else {
        if (stoppedOnWhitespace) {
            stream >> std::ws;
            if (stream.peek() == ',') {
                stream.get();
                if (delimiter)
                    *delimiter = TokenDelimiter::Comma;
            } else if (stream.peek() != std::char_traits<char>::eof()) {
                if (delimiter)
                    *delimiter = TokenDelimiter::Whitespace;
            }
        }
        if (stream.eof())
            stream.clear(stream.rdstate() & ~std::ios::failbit);
    }
    return token;
}

/// @brief Read the next token without reporting its terminating delimiter.
/// @param stream Backing stream positioned at the next token to extract.
/// @return Token read from @p stream, with delimiter handling identical to the
///         two-argument overload.
std::string readToken(std::istringstream &stream) {
    return readToken(stream, nullptr);
}

/// @brief Validate an IL identifier fragment after sigils are stripped.
///
/// The grammar remains intentionally permissive for existing generated names
/// (`.`, `$`, and digits are allowed), but rejects whitespace and characters
/// that would collide with IL delimiters or sigils.
/// @param text Candidate identifier without an IL sigil.
/// @return True when @p text can be emitted as an unquoted IL identifier.
bool isValidILIdentifier(std::string_view text) {
    if (text.empty())
        return false;
    if (text == "." || text == "..")
        return false;

    for (size_t index = 0; index < text.size(); ++index) {
        unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < 0x20 || ch == 0x7f)
            return false;
        if (std::isspace(ch))
            return false;
        switch (static_cast<char>(ch)) {
            case '@':
            case '^':
            case '(':
            case ')':
            case '{':
            case '}':
            case '[':
            case ']':
            case ',':
            case ':':
            case ';':
            case '"':
            case '\\':
                return false;
            default:
                break;
        }
        if (index == 0 && (ch == '%' || ch == '#'))
            return false;
    }

    return true;
}

/// @brief Format a diagnostic string that mirrors the "Line N:" prefix style.
/// @param lineNo Input line number associated with the diagnostic.
/// @param message Human-readable message body.
/// @return Combined diagnostic string.
std::string formatLineDiag(unsigned lineNo, std::string_view message) {
    std::ostringstream oss;
    // Use lowercase "line" to match VM and parser diagnostics uniformly.
    oss << "line " << lineNo << ": " << message;
    return oss.str();
}

/// @brief Parse a token as a signed integer literal.
///
/// Internally forwards to std::stoll and verifies that the entire token is
/// consumed by the conversion. On success @p value receives the parsed result;
/// otherwise the function returns false and leaves @p value unmodified.
///
/// @param token Candidate integer literal, e.g. "-42".
/// @param value Output location for the parsed integer when parsing succeeds.
/// @return True when @p token represents a valid signed integer literal.
bool parseIntegerLiteral(const std::string &token, long long &value) {
    if (token.empty())
        return false;

    size_t pos = 0;
    const bool negative = token[pos] == '-';
    if (token[pos] == '+' || token[pos] == '-')
        ++pos;
    if (pos >= token.size())
        return false;

    int base = 10;
    if (token[pos] == '0') {
        if (pos + 1 < token.size()) {
            const char prefix = token[pos + 1];
            if (prefix == 'b' || prefix == 'B') {
                base = 2;
                pos += 2;
            } else if (prefix == 'x' || prefix == 'X') {
                base = 16;
                pos += 2;
            } else {
                base = 8;
            }
        }
    }

    const auto maxSigned = static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    const unsigned long long limit = negative ? maxSigned + 1ULL : maxSigned;
    unsigned long long acc = 0;
    bool sawDigit = false;
    bool previousWasUnderscore = false;
    /// Convert one ASCII digit to its value in the selected integer base.
    auto digitValue = [base](unsigned char ch) {
        int digit = -1;
        if (ch >= '0' && ch <= '9')
            digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            digit = 10 + (ch - 'a');
        else if (ch >= 'A' && ch <= 'F')
            digit = 10 + (ch - 'A');
        if (digit < 0 || digit >= base)
            return -1;
        return digit;
    };
    for (; pos < token.size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>(token[pos]);
        if (ch == '_') {
            if (!sawDigit || previousWasUnderscore || pos + 1 >= token.size())
                return false;
            const auto next = static_cast<unsigned char>(token[pos + 1]);
            if (digitValue(next) < 0)
                return false;
            previousWasUnderscore = true;
            continue;
        }

        int digit = digitValue(ch);
        if (digit < 0)
            return false;

        sawDigit = true;
        previousWasUnderscore = false;
        const auto unsignedDigit = static_cast<unsigned long long>(digit);
        if (acc > (limit - unsignedDigit) / static_cast<unsigned long long>(base))
            return false;
        acc = acc * static_cast<unsigned long long>(base) + unsignedDigit;
    }
    if (!sawDigit || previousWasUnderscore)
        return false;

    if (negative) {
        if (acc == maxSigned + 1ULL) {
            value = std::numeric_limits<long long>::min();
        } else {
            value = -static_cast<long long>(acc);
        }
    } else {
        value = static_cast<long long>(acc);
    }
    return true;
}

/// @brief Parse a serializer-style temporary identifier name.
/// @param name Identifier without the leading `%` sigil.
/// @return Numeric id for names of the form `tN`, or std::nullopt otherwise.
std::optional<unsigned> parseExplicitTempName(std::string_view name) {
    if (name.size() < 2 || name.front() != 't')
        return std::nullopt;

    unsigned long long value = 0;
    for (size_t index = 1; index < name.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(name[index]);
        if (!std::isdigit(ch))
            return std::nullopt;

        value = value * 10 + static_cast<unsigned long long>(ch - '0');
        if (value > std::numeric_limits<unsigned>::max())
            return std::nullopt;
    }

    return static_cast<unsigned>(value);
}

/// @brief Parse a token as a floating-point literal.
///
/// Uses std::stod to recognise decimal or scientific-notation values and checks
/// that the conversion consumed the full token. Successful parses store the
/// resulting double in @p value; failures report false without mutating it.
///
/// @param token Candidate floating literal, e.g. "3.14" or "1e-3".
/// @param value Output location for the parsed floating-point value.
/// @return True when @p token is a valid floating-point literal.
bool parseFloatLiteral(const std::string &token, double &value) {
    // Handle well-known spellings for special values explicitly so behaviour is
    // consistent across libstdc++/libc++ and locales.
    if (!token.empty()) {
        std::string lower;
        lower.reserve(token.size());
        for (unsigned char ch : token) {
            lower.push_back(static_cast<char>(std::tolower(ch)));
        }
        if (lower == "nan") {
            value = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
        if (lower == "inf" || lower == "+inf") {
            value = std::numeric_limits<double>::infinity();
            return true;
        }
        if (lower == "-inf") {
            value = -std::numeric_limits<double>::infinity();
            return true;
        }
    }

    std::istringstream stream(token);
    stream.imbue(std::locale::classic());
    double parsed = 0.0;
    stream >> parsed;
    if (!stream)
        return false;
    stream >> std::ws;
    if (!stream.eof())
        return false;
    if (!std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

/// @brief Parse a trap-kind mnemonic into its numeric representation.
/// @param token Trap kind name such as "DivideByZero".
/// @param value Output receiving the numeric trap code on success.
/// @return True when @p token matches a known trap name.
bool parseTrapKindToken(const std::string &token, long long &value) {
    for (const auto &entry : kTrapKindSymbols) {
        if (token == entry.name) {
            value = entry.value;
            return true;
        }
    }
    return false;
}

/// @brief Map a numeric trap code back to its mnemonic name.
/// @param value Numeric trap kind identifier.
/// @return Name of the trap kind when recognised; otherwise empty optional.
std::optional<std::string_view> trapKindTokenFromValue(long long value) {
    for (const auto &entry : kTrapKindSymbols) {
        if (entry.value == value)
            return entry.name;
    }
    return std::nullopt;
}

} // namespace il::io
