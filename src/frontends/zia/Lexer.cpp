//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/Lexer.cpp
// Purpose: Tokenize Zia source while tracking locations, interpolation state,
//          nested comments, literal values, and recoverable lexical errors.
// Key invariants:
//   * Source position never advances beyond the owned buffer.
//   * CR, LF, and CRLF update line/column state consistently.
//   * Interpolation brace state is balanced with interpolation nesting.
//   * Numeric separators and base-specific digits are validated before value
//     conversion.
// Ownership/Lifetime:
//   - Lexer owns its source buffer and token cache.
//   - DiagnosticEngine is borrowed and must outlive the lexer.
// Links: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Implementation of the Zia lexical analyzer.
///
/// @details This file implements the Lexer class which tokenizes Zia
/// source code. Key implementation details:
///
/// ## Keyword Lookup
///
/// Keywords are stored in a sorted array (kKeywordTable) for O(log n) binary
/// search lookup.
///
/// ## String Interpolation
///
/// Interpolated strings like `"Hello ${name}!"` are handled by:
/// 1. Returning StringStart token for `"Hello ${`
/// 2. Tracking interpolation depth and brace nesting
/// 3. Resuming string lexing after `}` to emit StringMid or StringEnd
///
/// ## Number Literals
///
/// Supports decimal, hexadecimal (0x), and binary (0b) integer literals,
/// plus floating-point with optional exponent (1.5e-3).
///
/// @see Lexer.hpp for the class interface
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/ZiaSupport.hpp"
#include "frontends/common/CharUtils.hpp"
#include "frontends/common/EscapeSequences.hpp"
#include "frontends/common/NumberParsing.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace il::frontends::zia {

namespace {

/// @brief Maximum number of source bytes retained for a single string token.
constexpr size_t kMaxStringLength = 16 * 1024 * 1024;

/// @brief Maximum nested block comment depth accepted by the lexer.
constexpr size_t kMaxBlockCommentDepth = 1024;

/// @brief Maximum retained spelling for one numeric token.
constexpr size_t kMaxNumericLiteralLength = 4096;

} // namespace

//===----------------------------------------------------------------------===//
// TokenKind to string conversion
//===----------------------------------------------------------------------===//

/// @brief Convert a Zia token kind to its diagnostic spelling.
/// @param kind Token kind to describe.
/// @return Static null-terminated spelling, or `"?"` for an unhandled value.
const char *tokenKindToString(TokenKind kind) {
    switch (kind) {
        case TokenKind::Eof:
            return "eof";
        case TokenKind::Error:
            return "error";
        case TokenKind::IntegerLiteral:
            return "integer";
        case TokenKind::NumberLiteral:
            return "number";
        case TokenKind::StringLiteral:
            return "string";
        case TokenKind::Identifier:
            return "identifier";
        case TokenKind::StringStart:
            return "string_start";
        case TokenKind::StringMid:
            return "string_mid";
        case TokenKind::StringEnd:
            return "string_end";

        // Keywords
        case TokenKind::KwStruct:
            return "struct";
        case TokenKind::KwClass:
            return "class";
        case TokenKind::KwEnum:
            return "enum";
        case TokenKind::KwInterface:
            return "interface";
        case TokenKind::KwFinal:
            return "final";
        case TokenKind::KwExpose:
            return "expose";
        case TokenKind::KwHide:
            return "hide";
        case TokenKind::KwForeign:
            return "foreign";
        case TokenKind::KwOverride:
            return "override";
        case TokenKind::KwDeinit:
            return "deinit";
        case TokenKind::KwProperty:
            return "property";
        case TokenKind::KwStatic:
            return "static";
        case TokenKind::KwWeak:
            return "weak";
        case TokenKind::KwAsync:
            return "async";
        case TokenKind::KwAwait:
            return "await";
        case TokenKind::KwModule:
            return "module";
        case TokenKind::KwNamespace:
            return "namespace";
        case TokenKind::KwBind:
            return "bind";
        case TokenKind::KwFunc:
            return "func";
        case TokenKind::KwReturn:
            return "return";
        case TokenKind::KwVar:
            return "var";
        case TokenKind::KwNew:
            return "new";
        case TokenKind::KwIf:
            return "if";
        case TokenKind::KwElse:
            return "else";
        case TokenKind::KwLet:
            return "let";
        case TokenKind::KwMatch:
            return "match";
        case TokenKind::KwWhile:
            return "while";
        case TokenKind::KwFor:
            return "for";
        case TokenKind::KwIn:
            return "in";
        case TokenKind::KwIs:
            return "is";
        case TokenKind::KwGuard:
            return "guard";
        case TokenKind::KwBreak:
            return "break";
        case TokenKind::KwContinue:
            return "continue";
        case TokenKind::KwDefer:
            return "defer";
        case TokenKind::KwTry:
            return "try";
        case TokenKind::KwType:
            return "type";
        case TokenKind::KwCatch:
            return "catch";
        case TokenKind::KwFinally:
            return "finally";
        case TokenKind::KwThrow:
            return "throw";
        case TokenKind::KwExtends:
            return "extends";
        case TokenKind::KwImplements:
            return "implements";
        case TokenKind::KwSelf:
            return "self";
        case TokenKind::KwSuper:
            return "super";
        case TokenKind::KwAs:
            return "as";
        case TokenKind::KwTrue:
            return "true";
        case TokenKind::KwFalse:
            return "false";
        case TokenKind::KwNull:
            return "null";
        case TokenKind::KwAnd:
            return "and";
        case TokenKind::KwOr:
            return "or";
        case TokenKind::KwNot:
            return "not";

        // Operators
        case TokenKind::Plus:
            return "+";
        case TokenKind::Minus:
            return "-";
        case TokenKind::Star:
            return "*";
        case TokenKind::Slash:
            return "/";
        case TokenKind::Percent:
            return "%";
        case TokenKind::PlusEqual:
            return "+=";
        case TokenKind::MinusEqual:
            return "-=";
        case TokenKind::StarEqual:
            return "*=";
        case TokenKind::SlashEqual:
            return "/=";
        case TokenKind::PercentEqual:
            return "%=";
        case TokenKind::Ampersand:
            return "&";
        case TokenKind::Pipe:
            return "|";
        case TokenKind::Caret:
            return "^";
        case TokenKind::Tilde:
            return "~";
        case TokenKind::ShiftLeft:
            return "<<";
        case TokenKind::ShiftRight:
            return ">>";
        case TokenKind::ShiftLeftEqual:
            return "<<=";
        case TokenKind::ShiftRightEqual:
            return ">>=";
        case TokenKind::AmpersandEqual:
            return "&=";
        case TokenKind::PipeEqual:
            return "|=";
        case TokenKind::CaretEqual:
            return "^=";
        case TokenKind::Bang:
            return "!";
        case TokenKind::Equal:
            return "=";
        case TokenKind::EqualEqual:
            return "==";
        case TokenKind::NotEqual:
            return "!=";
        case TokenKind::Less:
            return "<";
        case TokenKind::LessEqual:
            return "<=";
        case TokenKind::Greater:
            return ">";
        case TokenKind::GreaterEqual:
            return ">=";
        case TokenKind::AmpAmp:
            return "&&";
        case TokenKind::PipePipe:
            return "||";
        case TokenKind::Arrow:
            return "->";
        case TokenKind::FatArrow:
            return "=>";
        case TokenKind::Question:
            return "?";
        case TokenKind::QuestionQuestion:
            return "??";
        case TokenKind::QuestionDot:
            return "?.";
        case TokenKind::Dot:
            return ".";
        case TokenKind::DotDot:
            return "..";
        case TokenKind::DotDotEqual:
            return "..=";
        case TokenKind::Ellipsis:
            return "...";
        case TokenKind::Colon:
            return ":";
        case TokenKind::Semicolon:
            return ";";
        case TokenKind::Comma:
            return ",";
        case TokenKind::At:
            return "@";

        // Brackets
        case TokenKind::LParen:
            return "(";
        case TokenKind::RParen:
            return ")";
        case TokenKind::LBracket:
            return "[";
        case TokenKind::RBracket:
            return "]";
        case TokenKind::LBrace:
            return "{";
        case TokenKind::RBrace:
            return "}";
    }
    return "?";
}

/// @brief Determine whether this token belongs to the contiguous keyword range.
/// @return True for token kinds from KwStruct through KwNot.
bool Token::isKeyword() const {
    return kind >= TokenKind::KwStruct && kind <= TokenKind::KwNot;
}

//===----------------------------------------------------------------------===//
// Keyword lookup table
//===----------------------------------------------------------------------===//

namespace {

/// @brief One sorted source spelling and its lexer token kind.
struct KeywordEntry {
    /// @brief Case-sensitive keyword spelling.
    std::string_view key;
    /// @brief Token emitted when key matches.
    TokenKind kind{TokenKind::Error};
};

// Sorted for binary search.
constexpr std::array<KeywordEntry, 53> kKeywordTable = {{
    {"and", TokenKind::KwAnd},
    {"as", TokenKind::KwAs},
    {"async", TokenKind::KwAsync},
    {"await", TokenKind::KwAwait},
    {"bind", TokenKind::KwBind},
    {"break", TokenKind::KwBreak},
    {"catch", TokenKind::KwCatch},
    {"class", TokenKind::KwClass},
    {"continue", TokenKind::KwContinue},
    {"defer", TokenKind::KwDefer},
    {"deinit", TokenKind::KwDeinit},
    {"else", TokenKind::KwElse},
    {"enum", TokenKind::KwEnum},
    {"export", TokenKind::KwExpose},
    {"expose", TokenKind::KwExpose},
    {"extends", TokenKind::KwExtends},
    {"false", TokenKind::KwFalse},
    {"final", TokenKind::KwFinal},
    {"finally", TokenKind::KwFinally},
    {"for", TokenKind::KwFor},
    {"foreign", TokenKind::KwForeign},
    {"func", TokenKind::KwFunc},
    {"guard", TokenKind::KwGuard},
    {"hide", TokenKind::KwHide},
    {"if", TokenKind::KwIf},
    {"implements", TokenKind::KwImplements},
    {"in", TokenKind::KwIn},
    {"interface", TokenKind::KwInterface},
    {"is", TokenKind::KwIs},
    {"let", TokenKind::KwLet},
    {"match", TokenKind::KwMatch},
    {"module", TokenKind::KwModule},
    {"namespace", TokenKind::KwNamespace},
    {"new", TokenKind::KwNew},
    {"not", TokenKind::KwNot},
    {"null", TokenKind::KwNull},
    {"or", TokenKind::KwOr},
    {"override", TokenKind::KwOverride},
    {"private", TokenKind::KwHide},
    {"property", TokenKind::KwProperty},
    {"public", TokenKind::KwExpose},
    {"return", TokenKind::KwReturn},
    {"self", TokenKind::KwSelf},
    {"static", TokenKind::KwStatic},
    {"struct", TokenKind::KwStruct},
    {"super", TokenKind::KwSuper},
    {"throw", TokenKind::KwThrow},
    {"true", TokenKind::KwTrue},
    {"try", TokenKind::KwTry},
    {"type", TokenKind::KwType},
    {"var", TokenKind::KwVar},
    {"weak", TokenKind::KwWeak},
    {"while", TokenKind::KwWhile},
}};

// Use common character utilities
using common::char_utils::isDigit;
using common::char_utils::isHexDigit;
using common::char_utils::isLetter;
using common::char_utils::isWhitespace;

/// @brief Check if character can start an identifier (letter or underscore).
/// @param c Source byte to classify.
/// @return True for an ASCII letter or underscore.
inline bool isIdentifierStart(char c) {
    return isLetter(c) || c == '_';
}

/// @brief Check if character can continue an identifier.
/// @param c Source byte to classify.
/// @return True for an ASCII letter, digit, or underscore.
inline bool isIdentifierContinue(char c) {
    return isLetter(c) || isDigit(c) || c == '_';
}

/// @brief True if @p c is the digit-group separator ('_') allowed in numeric
///        literals (e.g. 1_000_000).
/// @param c Source byte to classify.
/// @return True when @p c is underscore.
inline bool isNumericSeparator(char c) {
    return c == '_';
}

/// @brief Validate '_' placement in a decimal literal: each separator must sit
///        between two digits (no leading/trailing/double underscore).
/// @param text Complete token spelling.
/// @param start First byte belonging to the numeric payload.
/// @return true if all separators are well-placed.
bool validateNumericSeparators(std::string_view text, size_t start = 0) {
    for (size_t i = start; i < text.size(); ++i) {
        if (text[i] != '_')
            continue;
        if (i == start || i + 1 >= text.size())
            return false;
        if (!isDigit(text[i - 1]) || !isDigit(text[i + 1]))
            return false;
    }
    return true;
}

/// @brief Return @p text (from @p start) with all '_' digit separators
///        stripped, yielding the bare numeric string for value parsing.
/// @param text Complete token spelling.
/// @param start First byte to copy or filter.
/// @return Owned substring with underscore separators removed.
std::string removeNumericSeparators(std::string_view text, size_t start = 0) {
    std::string result;
    result.reserve(text.size() - start);
    for (size_t i = start; i < text.size(); ++i) {
        if (text[i] != '_')
            result.push_back(text[i]);
    }
    return result;
}

/// @brief Validate '_' placement in a based integer literal (0x.., 0b.., 0o..).
/// @param text Complete token spelling including its base prefix.
/// @param digitStart      Index of the first digit after the base prefix.
/// @param isDigitForBase  Predicate identifying a valid digit for the base.
/// @return true if every separator is between two base-valid digits.
bool validateBasedIntegerSeparators(std::string_view text,
                                    size_t digitStart,
                                    bool (*isDigitForBase)(char)) {
    bool previousWasDigit = false;
    for (size_t i = digitStart; i < text.size(); ++i) {
        char c = text[i];
        if (isDigitForBase(c)) {
            previousWasDigit = true;
            continue;
        }
        if (c != '_' || !previousWasDigit || i + 1 >= text.size() || !isDigitForBase(text[i + 1])) {
            return false;
        }
        previousWasDigit = false;
    }
    return previousWasDigit;
}

/// @brief Parse a prefix-free, non-decimal integer with signed integer bounds.
/// @param digits Digits after separators and base prefix have been removed.
/// @param base Numeric base represented by @p digits.
/// @return The parsed value, or `std::nullopt` when the literal overflows `int64_t`.
///
/// @details Binary and octal literals are value literals, not arbitrary-width bit
///          patterns. This helper keeps their bounds consistent and avoids duplicating
///          overflow checks in each base-specific lexer branch.
std::optional<int64_t> parseSignedBasedInteger(std::string_view digits, uint32_t base) {
    constexpr uint64_t kMaxSignedInteger =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t value = 0;
    for (char digit : digits) {
        uint32_t digitValue = static_cast<uint32_t>(digit - '0');
        if (value > (kMaxSignedInteger - digitValue) / base)
            return std::nullopt;
        value = value * base + digitValue;
    }
    return static_cast<int64_t>(value);
}

/// @brief Select a stable diagnostic code from a lexer error message.
/// @details More specific unterminated, literal, escape, and identifier cases
///          are recognized before falling back to the general lexer code.
/// @param message Human-readable error text.
/// @return Stable Zia lexer diagnostic identifier.
std::string classifyLexError(const std::string &message) {
    if (message.find("unterminated block comment") != std::string::npos)
        return "V-ZIA-LEX-UNTERMINATED-COMMENT";
    if (message.find("unterminated") != std::string::npos &&
        message.find("string") != std::string::npos)
        return "V-ZIA-LEX-UNTERMINATED-STRING";
    if (message.find("literal") != std::string::npos)
        return "V-ZIA-LEX-LITERAL";
    if (message.find("escape") != std::string::npos)
        return "V-ZIA-LEX-ESCAPE";
    if (message.find("identifier too long") != std::string::npos)
        return "V-ZIA-LEX-IDENTIFIER";
    return "V-ZIA-LEX";
}

} // anonymous namespace

/// @copydoc Lexer::lookupKeyword()
std::optional<TokenKind> Lexer::lookupKeyword(std::string_view name) {
    /// @brief Orders a keyword-table entry against a lookup key.
    /// @param entry Keyword table entry.
    /// @param key Requested keyword spelling.
    /// @return `true` when `entry.key` precedes `key`.
    auto it = std::lower_bound(
        kKeywordTable.begin(),
        kKeywordTable.end(),
        name,
        [](const KeywordEntry &entry, std::string_view key) { return entry.key < key; });
    if (it != kKeywordTable.end() && it->key == name)
        return it->kind;
    return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Lexer implementation
//===----------------------------------------------------------------------===//

/// @copydoc Lexer::Lexer()
Lexer::Lexer(std::string source, uint32_t fileId, il::support::DiagnosticEngine &diag)
    : source_(std::move(source)), fileId_(fileId), diag_(diag) {}

/// @copydoc Lexer::peekChar() const
char Lexer::peekChar() const {
    if (pos_ >= source_.size())
        return '\0';
    return source_[pos_];
}

/// @copydoc Lexer::peekChar(size_t) const
char Lexer::peekChar(size_t offset) const {
    if (offset >= source_.size() - pos_)
        return '\0';
    return source_[pos_ + offset];
}

/// @copydoc Lexer::getChar()
char Lexer::getChar() {
    if (pos_ >= source_.size())
        return '\0';
    char c = source_[pos_++];
    if (c == '\r') {
        if (pos_ < source_.size() && source_[pos_] == '\n')
            ++pos_;
        ++line_;
        column_ = 1;
        return '\n';
    }
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

/// @copydoc Lexer::eof()
bool Lexer::eof() const {
    return pos_ >= source_.size();
}

/// @copydoc Lexer::currentLoc()
il::support::SourceLoc Lexer::currentLoc() const {
    return il::support::SourceLoc{fileId_, line_, column_};
}

/// @copydoc Lexer::reportError()
void Lexer::reportError(il::support::SourceLoc loc, const std::string &message) {
    reportErrorRange(loc, il::support::SourceLoc{loc.file_id, loc.line, loc.column + 1}, message);
}

/// @copydoc Lexer::reportErrorRange()
void Lexer::reportErrorRange(il::support::SourceLoc start,
                             il::support::SourceLoc end,
                             const std::string &message) {
    il::support::Diagnostic diag{
        il::support::Severity::Error,
        message,
        start,
        classifyLexError(message),
    };
    if (start.isValid()) {
        if (!end.isValid() || end.file_id != start.file_id ||
            (end.line == start.line && end.column <= start.column) || end.line < start.line) {
            end = il::support::SourceLoc{start.file_id, start.line, start.column + 1};
        }
        diag.range = il::support::SourceRange{start, end};
    }
    diag.stage = "lex";
    diag_.report(std::move(diag));
}

/// @copydoc Lexer::skipLineComment()
void Lexer::skipLineComment() {
    // Skip the //
    getChar();
    getChar();
    // Skip until end of line or EOF
    while (!eof() && peekChar() != '\n' && peekChar() != '\r') {
        getChar();
    }
}

/// @copydoc Lexer::skipBlockComment()
bool Lexer::skipBlockComment() {
    il::support::SourceLoc startLoc = currentLoc();

    // Skip /*
    getChar();
    getChar();

    size_t depth = 1; // Support nested comments
    while (!eof() && depth > 0) {
        char c = getChar();
        if (c == '/' && peekChar() == '*') {
            getChar();
            if (depth == kMaxBlockCommentDepth) {
                reportError(startLoc, "block comment nesting too deep (limit: 1024)");
                return false;
            }
            ++depth;
        } else if (c == '*' && peekChar() == '/') {
            getChar();
            --depth;
        }
    }

    if (depth > 0) {
        reportErrorRange(startLoc, currentLoc(), "unterminated block comment");
        return false;
    }
    return true;
}

/// @copydoc Lexer::skipWhitespaceAndComments()
bool Lexer::skipWhitespaceAndComments() {
    while (!eof()) {
        char c = peekChar();

        if (isWhitespace(c)) {
            getChar();
            continue;
        }

        // Line comment: //
        if (c == '/' && peekChar(1) == '/') {
            skipLineComment();
            continue;
        }

        // Block comment: /* ... */
        if (c == '/' && peekChar(1) == '*') {
            if (!skipBlockComment())
                return false;
            continue;
        }

        break;
    }
    return true;
}

/// @copydoc Lexer::lexIdentifierOrKeyword()
Token Lexer::lexIdentifierOrKeyword() {
    Token tok;
    tok.loc = currentLoc();
    tok.text.reserve(16);

    // Consume identifier characters (capped at 1024 to prevent OOM from malicious input)
    while (!eof() && isIdentifierContinue(peekChar())) {
        if (tok.text.size() >= 1024) {
            auto endLoc = tok.loc;
            endLoc.column += static_cast<uint32_t>(tok.text.size());
            reportErrorRange(tok.loc, endLoc, "identifier too long (limit: 1024 characters)");
            // Skip remaining identifier characters
            while (!eof() && isIdentifierContinue(peekChar()))
                getChar();
            tok.kind = TokenKind::Error;
            return tok;
        }
        tok.text.push_back(getChar());
    }

    // Check if it's a keyword (case-sensitive)
    if (auto kw = lookupKeyword(tok.text)) {
        tok.kind = *kw;
        return tok;
    }

    tok.kind = TokenKind::Identifier;
    return tok;
}

/// @brief Check whether the current source position can lex `.digits` as a number.
/// @details The lexer has no parser context, so it relies on the previous consumed
/// token. Tokens that can end an expression keep `.` available for field access and
/// tuple indexes; all other contexts allow the leading-dot float extension.
/// @return True when the current dot should be lexed as part of a NumberLiteral.
/// @copydoc Lexer::canStartLeadingDotNumber()
bool Lexer::canStartLeadingDotNumber() const {
    if (peekChar() != '.' || !isDigit(peekChar(1)))
        return false;
    if (!lastSignificantTokenKind_)
        return true;

    switch (*lastSignificantTokenKind_) {
        case TokenKind::Identifier:
        case TokenKind::IntegerLiteral:
        case TokenKind::NumberLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::StringEnd:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwNull:
        case TokenKind::KwSelf:
        case TokenKind::KwSuper:
        case TokenKind::RParen:
        case TokenKind::RBracket:
        case TokenKind::RBrace:
            return false;
        default:
            return true;
    }
}

/// @copydoc Lexer::consumeMalformedBasedLiteralTail()
bool Lexer::consumeMalformedBasedLiteralTail(Token &tok) {
    bool withinLimit = true;
    while (!eof() && isIdentifierContinue(peekChar())) {
        const char c = getChar();
        if (tok.text.size() < kMaxNumericLiteralLength)
            tok.text.push_back(c);
        else
            withinLimit = false;
    }
    return withinLimit;
}

/// @copydoc Lexer::lexNumber()
Token Lexer::lexNumber() {
    Token tok;
    tok.loc = currentLoc();
    tok.kind = TokenKind::IntegerLiteral;
    bool withinLengthLimit = true;
    auto appendCurrent = [&]() {
        const char c = getChar();
        if (tok.text.size() < kMaxNumericLiteralLength)
            tok.text.push_back(c);
        else
            withinLengthLimit = false;
    };
    auto rejectOversized = [&]() {
        if (withinLengthLimit)
            return false;
        reportErrorRange(tok.loc, currentLoc(), "numeric literal too long (limit: 4096 bytes)");
        tok.kind = TokenKind::Error;
        return true;
    };

    // Check for hex (0x), binary (0b), or octal (0o)
    if (peekChar() == '0') {
        char next = peekChar(1);
        if (next == 'x' || next == 'X') {
            // Hex literal
            appendCurrent(); // '0'
            appendCurrent(); // 'x'

            if (!isHexDigit(peekChar())) {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok);
                if (rejectOversized())
                    return tok;
                reportError(tok.loc, "invalid hex literal: expected hex digits after 0x");
                tok.kind = TokenKind::Error;
                return tok;
            }

            while (!eof() && (isHexDigit(peekChar()) || isNumericSeparator(peekChar()))) {
                appendCurrent();
            }

            if (!eof() && isIdentifierContinue(peekChar())) {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok) && withinLengthLimit;
                if (rejectOversized())
                    return tok;
                reportErrorRange(tok.loc, currentLoc(), "invalid hex literal");
                tok.kind = TokenKind::Error;
                return tok;
            }
            if (rejectOversized())
                return tok;

            /// @brief Tests whether a hexadecimal literal character is a digit.
            /// @param ch Character to inspect.
            /// @return `true` for an accepted hexadecimal digit.
            if (!validateBasedIntegerSeparators(
                    tok.text, 2, [](char ch) { return isHexDigit(ch); })) {
                reportError(tok.loc, "invalid hex literal: '_' must separate digits");
                tok.kind = TokenKind::Error;
                return tok;
            }

            // Parse hex value
            std::string normalized = removeNumericSeparators(tok.text, 2);
            std::string_view hexDigits(normalized);
            auto parsed = common::number_parsing::parseHexLiteral(hexDigits);
            if (!parsed.valid) {
                if (parsed.overflow)
                    reportError(tok.loc, "hex literal out of range");
                else
                    reportError(tok.loc, "invalid hex literal");
                tok.kind = TokenKind::Error;
            } else {
                tok.intValue = parsed.intValue;
            }
            return tok;
        } else if (next == 'b' || next == 'B') {
            // Binary literal
            appendCurrent(); // '0'
            appendCurrent(); // 'b'

            if (peekChar() != '0' && peekChar() != '1') {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok);
                if (rejectOversized())
                    return tok;
                reportError(tok.loc, "invalid binary literal: expected binary digits after 0b");
                tok.kind = TokenKind::Error;
                return tok;
            }

            while (!eof() &&
                   (peekChar() == '0' || peekChar() == '1' || isNumericSeparator(peekChar()))) {
                appendCurrent();
            }

            if (!eof() && isIdentifierContinue(peekChar())) {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok) && withinLengthLimit;
                if (rejectOversized())
                    return tok;
                reportErrorRange(tok.loc, currentLoc(), "invalid binary literal");
                tok.kind = TokenKind::Error;
                return tok;
            }
            if (rejectOversized())
                return tok;

            /// @brief Tests whether a binary literal character is a digit.
            /// @param ch Character to inspect.
            /// @return `true` for `0` or `1`.
            if (!validateBasedIntegerSeparators(
                    tok.text, 2, [](char ch) { return ch == '0' || ch == '1'; })) {
                reportError(tok.loc, "invalid binary literal: '_' must separate digits");
                tok.kind = TokenKind::Error;
                return tok;
            }

            std::string binaryDigits = removeNumericSeparators(tok.text, 2);
            auto parsed = parseSignedBasedInteger(binaryDigits, 2);
            if (!parsed) {
                reportError(tok.loc, "binary literal out of range");
                tok.kind = TokenKind::Error;
                return tok;
            }
            tok.intValue = *parsed;
            return tok;
        } else if (next == 'o' || next == 'O') {
            // Octal literal
            appendCurrent(); // '0'
            appendCurrent(); // 'o'

            /// @brief Tests whether a character is an octal digit.
            /// @param ch Character to inspect.
            /// @return `true` for `0` through `7`.
            auto isOctalDigit = [](char ch) { return ch >= '0' && ch <= '7'; };
            if (!isOctalDigit(peekChar())) {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok);
                if (rejectOversized())
                    return tok;
                reportError(tok.loc, "invalid octal literal: expected octal digits after 0o");
                tok.kind = TokenKind::Error;
                return tok;
            }

            while (!eof() && (isOctalDigit(peekChar()) || isNumericSeparator(peekChar()))) {
                appendCurrent();
            }

            if (!eof() && isIdentifierContinue(peekChar())) {
                withinLengthLimit = consumeMalformedBasedLiteralTail(tok) && withinLengthLimit;
                if (rejectOversized())
                    return tok;
                reportErrorRange(tok.loc, currentLoc(), "invalid octal literal");
                tok.kind = TokenKind::Error;
                return tok;
            }
            if (rejectOversized())
                return tok;

            if (!validateBasedIntegerSeparators(tok.text, 2, isOctalDigit)) {
                reportError(tok.loc, "invalid octal literal: '_' must separate digits");
                tok.kind = TokenKind::Error;
                return tok;
            }

            std::string octalDigits = removeNumericSeparators(tok.text, 2);
            auto parsed = parseSignedBasedInteger(octalDigits, 8);
            if (!parsed) {
                reportError(tok.loc, "octal literal out of range");
                tok.kind = TokenKind::Error;
                return tok;
            }
            tok.intValue = *parsed;
            return tok;
        }
    }

    // Decimal number
    while (!eof() && (isDigit(peekChar()) || isNumericSeparator(peekChar()))) {
        appendCurrent();
    }

    // Check for decimal point (but not .. range operator)
    if (peekChar() == '.' && peekChar(1) != '.') {
        tok.kind = TokenKind::NumberLiteral;
        appendCurrent(); // consume '.'

        // Consume fractional part
        while (!eof() && (isDigit(peekChar()) || isNumericSeparator(peekChar()))) {
            appendCurrent();
        }
    }

    // Check for exponent
    char e = peekChar();
    if (e == 'e' || e == 'E') {
        tok.kind = TokenKind::NumberLiteral;
        appendCurrent(); // consume 'e' or 'E'

        // Optional sign
        char sign = peekChar();
        if (sign == '+' || sign == '-') {
            appendCurrent();
        }

        // Exponent digits
        if (!isDigit(peekChar())) {
            reportError(tok.loc, "invalid numeric literal: expected exponent digits");
            tok.kind = TokenKind::Error;
            return tok;
        }

        size_t exponentDigits = 0;
        while (!eof() && (isDigit(peekChar()) || isNumericSeparator(peekChar()))) {
            if (isDigit(peekChar()))
                ++exponentDigits;
            appendCurrent();
        }
        // A binary64 exponent never exceeds 3 digits; cap well above that so the
        // value parser is never handed an absurd magnitude.
        if (exponentDigits > 6) {
            reportErrorRange(tok.loc, currentLoc(), "numeric literal exponent is out of range");
            tok.kind = TokenKind::Error;
            return tok;
        }
    }

    if (!eof() && isIdentifierContinue(peekChar())) {
        withinLengthLimit = consumeMalformedBasedLiteralTail(tok) && withinLengthLimit;
        if (rejectOversized())
            return tok;
        reportErrorRange(tok.loc, currentLoc(), "invalid numeric literal");
        tok.kind = TokenKind::Error;
        return tok;
    }
    if (rejectOversized())
        return tok;

    if (!validateNumericSeparators(tok.text)) {
        reportError(tok.loc, "invalid numeric literal: '_' must separate digits");
        tok.kind = TokenKind::Error;
        return tok;
    }

    // Parse the value
    std::string normalized = removeNumericSeparators(tok.text);
    auto parsed = common::number_parsing::parseDecimalLiteral(normalized);
    if (!parsed.valid) {
        if (parsed.overflow)
            reportError(tok.loc, "numeric literal out of range");
        else
            reportError(tok.loc, "invalid numeric literal");
        tok.kind = TokenKind::Error;
    } else if (parsed.isFloat) {
        tok.kind = TokenKind::NumberLiteral;
        tok.floatValue = parsed.floatValue;
    } else {
        tok.kind = TokenKind::IntegerLiteral;
        tok.intValue = parsed.intValue;
        tok.requiresNegation = parsed.requiresNegation;
    }

    return tok;
}

// Escape sequence processing is provided by frontends/common/EscapeSequences.hpp.
// The Lexer itself satisfies the Cursor concept (peekChar/getChar/eof) required
// by the templated processHexEscape and processUnicodeEscape utilities.
// The namespace alias below keeps call sites concise.
namespace esc = common::escape_sequences;

/// @copydoc Lexer::lexStringEscape()
bool Lexer::lexStringEscape(Token &tok) {
    tok.text.push_back(getChar()); // consume '\'
    if (eof()) {
        reportError(tok.loc, "unterminated escape sequence");
        tok.kind = TokenKind::Error;
        return false;
    }

    char escaped = peekChar();
    tok.text.push_back(getChar()); // consume the escape character

    // Handle unicode escape \uXXXX
    if (escaped == 'u') {
        char digits[4];
        bool valid = true;
        for (int i = 0; i < 4 && valid; ++i) {
            if (eof() || !isHexDigit(peekChar())) {
                valid = false;
            } else {
                digits[i] = getChar();
                tok.text.push_back(digits[i]);
            }
        }
        if (valid) {
            if (auto utf8 = esc::processUnicodeEscape(std::string_view(digits, 4))) {
                for (char ch : *utf8)
                    tok.stringValue.push_back(ch);
            } else {
                reportError(tok.loc, "invalid unicode escape sequence: expected \\uXXXX");
                tok.kind = TokenKind::Error;
                return false;
            }
        } else {
            reportError(tok.loc, "invalid unicode escape sequence: expected \\uXXXX");
            tok.kind = TokenKind::Error;
            return false;
        }
        return true;
    }

    // Handle hex escape \xXX
    if (escaped == 'x') {
        if (!eof() && isHexDigit(peekChar())) {
            char high = getChar();
            tok.text.push_back(high);
            if (!eof() && isHexDigit(peekChar())) {
                char low = getChar();
                tok.text.push_back(low);
                if (auto hexChar = esc::processHexEscape(high, low)) {
                    tok.stringValue.push_back(*hexChar);
                } else {
                    reportError(tok.loc, "invalid hex escape sequence: expected \\xXX");
                    tok.kind = TokenKind::Error;
                    return false;
                }
            } else {
                reportError(tok.loc, "invalid hex escape sequence: expected \\xXX");
                tok.kind = TokenKind::Error;
                return false;
            }
        } else {
            reportError(tok.loc, "invalid hex escape sequence: expected \\xXX");
            tok.kind = TokenKind::Error;
            return false;
        }
        return true;
    }

    if (auto escaped_ch = esc::processEscape(escaped)) {
        tok.stringValue.push_back(*escaped_ch);
    } else {
        reportError(tok.loc, std::string("invalid escape sequence: \\") + escaped);
        tok.kind = TokenKind::Error;
        return false;
    }
    return true;
}

/// @copydoc Lexer::lexString()
Token Lexer::lexString() {
    Token tok;
    tok.loc = currentLoc();
    tok.kind = TokenKind::StringLiteral;

    // Check for triple-quoted string
    if (peekChar() == '"' && peekChar(1) == '"' && peekChar(2) == '"') {
        return lexTripleQuotedString();
    }

    tok.text.push_back(getChar()); // consume opening "

    while (!eof()) {
        char c = peekChar();

        // Check for closing quote
        if (c == '"') {
            tok.text.push_back(getChar());
            return tok;
        }

        if (tok.text.size() >= kMaxStringLength) {
            reportErrorRange(tok.loc, currentLoc(), "string literal too long (limit: 16MB)");
            // Skip to closing quote or EOF
            while (!eof() && peekChar() != '"')
                getChar();
            if (!eof())
                getChar(); // consume closing "
            tok.kind = TokenKind::Error;
            return tok;
        }

        // Check for string interpolation: ${
        if (c == '$' && peekChar(1) == '{') {
            if (interpolationDepth_ >= 32) {
                reportError(tok.loc, "string interpolation nesting too deep (limit: 32)");
                interpolationDepth_ = 0;
                braceDepth_.clear();
                while (!eof() && peekChar() != '"')
                    getChar();
                if (!eof())
                    getChar();
                tok.kind = TokenKind::Error;
                return tok;
            }
            // This is an interpolated string - change token kind to StringStart
            tok.kind = TokenKind::StringStart;
            tok.text.push_back(getChar()); // consume '$'
            tok.text.push_back(getChar()); // consume '{'
            // Enter interpolation mode
            interpolationDepth_++;
            braceDepth_.push_back(0);
            ZANNA_ZIA_ASSERT(braceDepth_.size() == static_cast<size_t>(interpolationDepth_),
                             "interpolation brace-depth stack out of sync with depth counter");
            return tok;
        }

        // Check for newline (error in single-quoted string)
        if (c == '\n' || c == '\r') {
            reportError(tok.loc, "newline in string literal");
            tok.kind = TokenKind::Error;
            return tok;
        }

        // Check for escape sequence
        if (c == '\\') {
            if (!lexStringEscape(tok))
                return tok;
            continue;
        }

        tok.text.push_back(getChar());
        tok.stringValue.push_back(c);
    }

    reportErrorRange(tok.loc, currentLoc(), "unterminated string literal");
    tok.kind = TokenKind::Error;
    return tok;
}

/// @copydoc Lexer::lexInterpolatedStringContinuation()
Token Lexer::lexInterpolatedStringContinuation() {
    Token tok;
    tok.loc = currentLoc();

    // We just consumed '}' - now continue reading the string
    while (!eof()) {
        char c = peekChar();

        if (tok.text.size() >= kMaxStringLength) {
            reportErrorRange(tok.loc, currentLoc(), "string literal too long (limit: 16MB)");
            interpolationDepth_ = 0;
            braceDepth_.clear();
            tok.kind = TokenKind::Error;
            return tok;
        }

        // Check for closing quote
        if (c == '"') {
            tok.text.push_back(getChar());
            tok.kind = TokenKind::StringEnd;
            return tok;
        }

        // Check for another interpolation: ${
        if (c == '$' && peekChar(1) == '{') {
            if (interpolationDepth_ >= 32) {
                reportError(tok.loc, "string interpolation nesting too deep (limit: 32)");
                interpolationDepth_ = 0;
                braceDepth_.clear();
                tok.kind = TokenKind::Error;
                return tok;
            }
            tok.kind = TokenKind::StringMid;
            tok.text.push_back(getChar()); // consume '$'
            tok.text.push_back(getChar()); // consume '{'
            // Enter new interpolation - push brace depth and increment depth counter
            braceDepth_.push_back(0);
            interpolationDepth_++;
            return tok;
        }

        // Check for newline (error in string)
        if (c == '\n' || c == '\r') {
            reportError(tok.loc, "newline in string literal");
            interpolationDepth_ = 0;
            braceDepth_.clear();
            tok.kind = TokenKind::Error;
            return tok;
        }

        // Check for escape sequence
        if (c == '\\') {
            if (!lexStringEscape(tok)) {
                interpolationDepth_ = 0;
                braceDepth_.clear();
                return tok;
            }
            continue;
        }

        tok.text.push_back(getChar());
        tok.stringValue.push_back(c);
    }

    reportErrorRange(tok.loc, currentLoc(), "unterminated interpolated string");
    interpolationDepth_ = 0;
    braceDepth_.clear();
    tok.kind = TokenKind::Error;
    return tok;
}

/// @copydoc Lexer::lexTripleQuotedString()
Token Lexer::lexTripleQuotedString() {
    Token tok;
    tok.loc = currentLoc();
    tok.kind = TokenKind::StringLiteral;

    // Consume opening """
    tok.text.push_back(getChar());
    tok.text.push_back(getChar());
    tok.text.push_back(getChar());

    while (!eof()) {
        char c = peekChar();

        if (tok.text.size() >= kMaxStringLength) {
            reportErrorRange(tok.loc, currentLoc(), "string literal too long (limit: 16MB)");
            tok.kind = TokenKind::Error;
            return tok;
        }

        // Check for closing """
        if (c == '"' && peekChar(1) == '"' && peekChar(2) == '"') {
            tok.text.push_back(getChar());
            tok.text.push_back(getChar());
            tok.text.push_back(getChar());
            return tok;
        }

        // Handle escape sequences
        if (c == '\\') {
            if (!lexStringEscape(tok))
                return tok;
            continue;
        }

        tok.text.push_back(getChar());
        tok.stringValue.push_back(c);
    }

    reportErrorRange(tok.loc, currentLoc(), "unterminated triple-quoted string");
    tok.kind = TokenKind::Error;
    return tok;
}

/// @copydoc Lexer::next()
Token Lexer::next() {
    // Return cached token if available
    if (peeked_.has_value()) {
        Token tok = std::move(*peeked_);
        peeked_.reset();
        if (tok.kind != TokenKind::Eof && tok.kind != TokenKind::Error)
            lastSignificantTokenKind_ = tok.kind;
        return tok;
    }

    /// @brief Records the last significant token before returning it.
    /// @param tok Token to remember and return.
    /// @return The supplied token.
    auto remember = [this](Token tok) {
        if (tok.kind != TokenKind::Eof && tok.kind != TokenKind::Error)
            lastSignificantTokenKind_ = tok.kind;
        return tok;
    };

    if (!skipWhitespaceAndComments()) {
        Token tok;
        tok.kind = TokenKind::Error;
        tok.loc = currentLoc();
        return tok;
    }

    if (eof()) {
        Token tok;
        tok.kind = TokenKind::Eof;
        tok.loc = currentLoc();
        return tok;
    }

    char c = peekChar();

    // Identifier or keyword
    if (isIdentifierStart(c)) {
        return remember(lexIdentifierOrKeyword());
    }

    // Number
    if (isDigit(c) || canStartLeadingDotNumber()) {
        return remember(lexNumber());
    }

    // String literal
    if (c == '"') {
        return remember(lexString());
    }

    // Operators and punctuation
    return remember(lexOperatorOrPunctuation(c));
}

/// @copydoc Lexer::lexOperatorOrPunctuation()
Token Lexer::lexOperatorOrPunctuation(char c) {
    Token tok;
    tok.loc = currentLoc();

    switch (c) {
        case '+':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::PlusEqual;
                tok.text = "+=";
            } else {
                tok.kind = TokenKind::Plus;
                tok.text = "+";
            }
            break;

        case '-':
            getChar();
            if (peekChar() == '>') {
                getChar();
                tok.kind = TokenKind::Arrow;
                tok.text = "->";
            } else if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::MinusEqual;
                tok.text = "-=";
            } else {
                tok.kind = TokenKind::Minus;
                tok.text = "-";
            }
            break;

        case '*':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::StarEqual;
                tok.text = "*=";
            } else {
                tok.kind = TokenKind::Star;
                tok.text = "*";
            }
            break;

        case '/':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::SlashEqual;
                tok.text = "/=";
            } else {
                tok.kind = TokenKind::Slash;
                tok.text = "/";
            }
            break;

        case '%':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::PercentEqual;
                tok.text = "%=";
            } else {
                tok.kind = TokenKind::Percent;
                tok.text = "%";
            }
            break;

        case '&':
            getChar();
            if (peekChar() == '&') {
                getChar();
                tok.kind = TokenKind::AmpAmp;
                tok.text = "&&";
            } else if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::AmpersandEqual;
                tok.text = "&=";
            } else {
                tok.kind = TokenKind::Ampersand;
                tok.text = "&";
            }
            break;

        case '|':
            getChar();
            if (peekChar() == '|') {
                getChar();
                tok.kind = TokenKind::PipePipe;
                tok.text = "||";
            } else if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::PipeEqual;
                tok.text = "|=";
            } else {
                tok.kind = TokenKind::Pipe;
                tok.text = "|";
            }
            break;

        case '^':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::CaretEqual;
                tok.text = "^=";
            } else {
                tok.kind = TokenKind::Caret;
                tok.text = "^";
            }
            break;

        case '~':
            tok.kind = TokenKind::Tilde;
            tok.text = "~";
            getChar();
            break;

        case '!':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::NotEqual;
                tok.text = "!=";
            } else {
                tok.kind = TokenKind::Bang;
                tok.text = "!";
            }
            break;

        case '=':
            getChar();
            if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::EqualEqual;
                tok.text = "==";
            } else if (peekChar() == '>') {
                getChar();
                tok.kind = TokenKind::FatArrow;
                tok.text = "=>";
            } else {
                tok.kind = TokenKind::Equal;
                tok.text = "=";
            }
            break;

        case '<':
            getChar();
            if (peekChar() == '<') {
                getChar();
                if (peekChar() == '=') {
                    getChar();
                    tok.kind = TokenKind::ShiftLeftEqual;
                    tok.text = "<<=";
                } else {
                    tok.kind = TokenKind::ShiftLeft;
                    tok.text = "<<";
                }
            } else if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::LessEqual;
                tok.text = "<=";
            } else {
                tok.kind = TokenKind::Less;
                tok.text = "<";
            }
            break;

        case '>':
            getChar();
            if (peekChar() == '>') {
                getChar();
                if (peekChar() == '=') {
                    getChar();
                    tok.kind = TokenKind::ShiftRightEqual;
                    tok.text = ">>=";
                } else {
                    tok.kind = TokenKind::ShiftRight;
                    tok.text = ">>";
                }
            } else if (peekChar() == '=') {
                getChar();
                tok.kind = TokenKind::GreaterEqual;
                tok.text = ">=";
            } else {
                tok.kind = TokenKind::Greater;
                tok.text = ">";
            }
            break;

        case '?':
            getChar();
            if (peekChar() == '?') {
                getChar();
                tok.kind = TokenKind::QuestionQuestion;
                tok.text = "??";
            } else if (peekChar() == '.') {
                getChar();
                tok.kind = TokenKind::QuestionDot;
                tok.text = "?.";
            } else {
                tok.kind = TokenKind::Question;
                tok.text = "?";
            }
            break;

        case '.':
            getChar();
            if (peekChar() == '.') {
                getChar();
                if (peekChar() == '.') {
                    getChar();
                    tok.kind = TokenKind::Ellipsis;
                    tok.text = "...";
                } else if (peekChar() == '=') {
                    getChar();
                    tok.kind = TokenKind::DotDotEqual;
                    tok.text = "..=";
                } else {
                    tok.kind = TokenKind::DotDot;
                    tok.text = "..";
                }
            } else {
                tok.kind = TokenKind::Dot;
                tok.text = ".";
            }
            break;

        case ':':
            tok.kind = TokenKind::Colon;
            tok.text = ":";
            getChar();
            break;

        case ';':
            tok.kind = TokenKind::Semicolon;
            tok.text = ";";
            getChar();
            break;

        case ',':
            tok.kind = TokenKind::Comma;
            tok.text = ",";
            getChar();
            break;

        case '@':
            tok.kind = TokenKind::At;
            tok.text = "@";
            getChar();
            break;

        case '(':
            tok.kind = TokenKind::LParen;
            tok.text = "(";
            getChar();
            break;

        case ')':
            tok.kind = TokenKind::RParen;
            tok.text = ")";
            getChar();
            break;

        case '[':
            tok.kind = TokenKind::LBracket;
            tok.text = "[";
            getChar();
            break;

        case ']':
            tok.kind = TokenKind::RBracket;
            tok.text = "]";
            getChar();
            break;

        case '{':
            tok.kind = TokenKind::LBrace;
            tok.text = "{";
            getChar();
            // Track brace depth in interpolation mode
            if (interpolationDepth_ > 0 && !braceDepth_.empty()) {
                braceDepth_.back()++;
            }
            break;

        case '}':
            getChar();
            // Check if this closes an interpolation
            if (interpolationDepth_ > 0 && !braceDepth_.empty()) {
                if (braceDepth_.back() == 0) {
                    // This closes the interpolation - continue lexing the string
                    braceDepth_.pop_back();
                    interpolationDepth_--;
                    return lexInterpolatedStringContinuation();
                } else {
                    // Just a nested brace
                    braceDepth_.back()--;
                }
            }
            tok.kind = TokenKind::RBrace;
            tok.text = "}";
            break;

        default:
            reportError(tok.loc, std::string("unexpected character '") + c + "'");
            tok.kind = TokenKind::Error;
            tok.text = std::string(1, c);
            getChar();
            break;
    }

    return tok;
}

/// @copydoc Lexer::peek()
const Token &Lexer::peek() {
    if (!peeked_.has_value()) {
        auto savedLastTokenKind = lastSignificantTokenKind_;
        peeked_ = next();
        lastSignificantTokenKind_ = savedLastTokenKind;
    }
    return *peeked_;
}

} // namespace il::frontends::zia
