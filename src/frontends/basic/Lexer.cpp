//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lexer.cpp
// Purpose: Tokenize BASIC source into location-aware parser tokens.
// Key invariants:
//   - The source view is borrowed and never modified.
//   - CRLF, lone CR, and LF each produce exactly one logical end-of-line token.
// Ownership/Lifetime:
//   - The caller-owned source buffer outlives the lexer.
//   - Returned token lexemes own their copied text.
// Links: src/frontends/basic/Lexer.hpp, src/frontends/common/LexerBase.hpp
//
//===----------------------------------------------------------------------===//
///
/// @file Lexer.cpp
/// @brief Lexical analyzer for the BASIC frontend.
///
/// @details This file implements the BASIC lexer which converts source text
/// into a stream of tokens for the parser. The lexer handles all BASIC-specific
/// syntax including keywords, operators, literals, and comments.
///
/// Cursor management (peek/get/eof) is inherited from LexerCursor<Lexer> via
/// CRTP, shared with the Zia frontend. This file only contains BASIC-specific
/// tokenization logic.
///
/// ## Tokenization Strategy
///
/// The lexer uses a single-character lookahead approach:
/// 1. Skip whitespace and comments
/// 2. Examine current character to determine token type
/// 3. Consume characters belonging to the token
/// 4. Return token with source location
///
/// ## Keyword Recognition
///
/// BASIC keywords (IF, THEN, WHILE, etc.) are recognized using binary search
/// over a sorted keyword table. Identifiers that match keywords are converted
/// to keyword tokens. Comparison is case-insensitive (BASIC tradition).
///
/// ## Numeric Literals
///
/// Supports:
/// - Decimal integers: `123`
/// - Hexadecimal: `&H1F` or `0x1F`
/// - Binary: `&B1010` or `0b1010`
/// - Floating point: `1.5`, `1.5E-3`
/// - Type suffixes: `%`, `&`, `!`, and `#`
///
/// ## String Literals
///
/// BASIC strings use double quotes with doubling for escapes:
/// - `"Hello, World!"`
/// - `"He said ""Hi"""`
///
/// ## Comments
///
/// Supports both comment styles:
/// - `REM This is a comment`
/// - `' This is also a comment`
///
/// ## Source Location Tracking
///
/// The lexer maintains accurate line and column positions for each token,
/// enabling precise error messages from the parser and semantic analyzer.
/// Position tracking is provided by the LexerCursor CRTP base class.
///
/// @invariant pos_ always points to the next character to be read.
/// @invariant line_ and column_ reflect the position of pos_.
/// @invariant The source buffer is never modified.
///
/// @see Lexer.hpp - Lexer class interface and Token structure
/// @see Parser.hpp - Consumer of the token stream
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/Lexer.hpp"
#include "frontends/common/CharUtils.hpp"
#include "frontends/common/KeywordTable.hpp"
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace il::frontends::basic {

using common::char_utils::isAlphanumeric;
using common::char_utils::isDigit;
using common::char_utils::isLetter;
using common::char_utils::toUpper;

// Use common keyword table utilities
using common::keyword_table::isKeywordTableSorted;
using common::keyword_table::KeywordEntry;
using common::keyword_table::lookupKeywordBinary;

namespace {

/// Sorted canonical keyword spellings and their token kinds.
constexpr std::array<KeywordEntry<TokenKind>, 108> kKeywordTable{{
    {"ABS", TokenKind::KeywordAbs},
    {"ABSTRACT", TokenKind::KeywordAbstract},
    {"ADDFILE", TokenKind::KeywordAddfile},
    {"ADDRESSOF", TokenKind::KeywordAddressOf},
    {"ALTSCREEN", TokenKind::KeywordAltscreen},
    {"AND", TokenKind::KeywordAnd},
    {"ANDALSO", TokenKind::KeywordAndAlso},
    {"APPEND", TokenKind::KeywordAppend},
    {"AS", TokenKind::KeywordAs},
    {"BASE", TokenKind::KeywordBase},
    {"BEEP", TokenKind::KeywordBeep},
    {"BINARY", TokenKind::KeywordBinary},
    {"BOOLEAN", TokenKind::KeywordBoolean},
    {"BYREF", TokenKind::KeywordByRef},
    {"BYVAL", TokenKind::KeywordByVal},
    {"CASE", TokenKind::KeywordCase},
    {"CATCH", TokenKind::KeywordCatch},
    {"CEIL", TokenKind::KeywordCeil},
    {"CLASS", TokenKind::KeywordClass},
    {"CLOSE", TokenKind::KeywordClose},
    {"CLS", TokenKind::KeywordCls},
    {"COLOR", TokenKind::KeywordColor},
    {"CONST", TokenKind::KeywordConst},
    {"COS", TokenKind::KeywordCos},
    {"CURSOR", TokenKind::KeywordCursor},
    {"DECLARE", TokenKind::KeywordDeclare},
    {"DELETE", TokenKind::KeywordDelete},
    {"DESTRUCTOR", TokenKind::KeywordDestructor},
    {"DIM", TokenKind::KeywordDim},
    {"DO", TokenKind::KeywordDo},
    {"EACH", TokenKind::KeywordEach},
    {"ELSE", TokenKind::KeywordElse},
    {"ELSEIF", TokenKind::KeywordElseIf},
    {"END", TokenKind::KeywordEnd},
    {"ENUM", TokenKind::KeywordEnum},
    {"EOF", TokenKind::KeywordEof},
    {"ERROR", TokenKind::KeywordError},
    {"EXIT", TokenKind::KeywordExit},
    {"EXPORT", TokenKind::KeywordExport},
    {"FALSE", TokenKind::KeywordFalse},
    {"FINAL", TokenKind::KeywordFinal},
    {"FINALLY", TokenKind::KeywordFinally},
    {"FLOOR", TokenKind::KeywordFloor},
    {"FOR", TokenKind::KeywordFor},
    {"FOREIGN", TokenKind::KeywordForeign},
    {"FUNCTION", TokenKind::KeywordFunction},
    {"GOSUB", TokenKind::KeywordGosub},
    {"GOTO", TokenKind::KeywordGoto},
    {"IF", TokenKind::KeywordIf},
    {"IMPLEMENTS", TokenKind::KeywordImplements},
    {"IN", TokenKind::KeywordIn},
    {"INPUT", TokenKind::KeywordInput},
    {"INTERFACE", TokenKind::KeywordInterface},
    {"IS", TokenKind::KeywordIs},
    {"LBOUND", TokenKind::KeywordLbound},
    {"LET", TokenKind::KeywordLet},
    // NOTE: 'LINE' is treated as a soft keyword: lex as identifier; parser recognises 'LINE INPUT'.
    {"LOC", TokenKind::KeywordLoc},
    {"LOCATE", TokenKind::KeywordLocate},
    {"LOF", TokenKind::KeywordLof},
    {"LOOP", TokenKind::KeywordLoop},
    {"ME", TokenKind::KeywordMe},
    {"MOD", TokenKind::KeywordMod},
    {"NAMESPACE", TokenKind::KeywordNamespace},
    {"NEW", TokenKind::KeywordNew},
    {"NEXT", TokenKind::KeywordNext},
    {"NOT", TokenKind::KeywordNot},
    {"NOTHING", TokenKind::KeywordNothing},
    {"OFF", TokenKind::KeywordOff},
    {"ON", TokenKind::KeywordOn},
    {"OPEN", TokenKind::KeywordOpen},
    {"OR", TokenKind::KeywordOr},
    {"ORELSE", TokenKind::KeywordOrElse},
    {"OUTPUT", TokenKind::KeywordOutput},
    {"OVERRIDE", TokenKind::KeywordOverride},
    {"POW", TokenKind::KeywordPow},
    {"PRESERVE", TokenKind::KeywordPreserve},
    {"PRINT", TokenKind::KeywordPrint},
    {"PRIVATE", TokenKind::KeywordPrivate},
    {"PROPERTY", TokenKind::KeywordProperty},
    {"PUBLIC", TokenKind::KeywordPublic},
    {"RANDOM", TokenKind::KeywordRandom},
    {"RANDOMIZE", TokenKind::KeywordRandomize},
    {"REDIM", TokenKind::KeywordRedim},
    {"RESUME", TokenKind::KeywordResume},
    {"RETURN", TokenKind::KeywordReturn},
    {"RND", TokenKind::KeywordRnd},
    {"SEEK", TokenKind::KeywordSeek},
    {"SELECT", TokenKind::KeywordSelect},
    {"SHARED", TokenKind::KeywordShared},
    {"SIN", TokenKind::KeywordSin},
    {"SLEEP", TokenKind::KeywordSleep},
    {"SQR", TokenKind::KeywordSqr},
    {"STATIC", TokenKind::KeywordStatic},
    {"STEP", TokenKind::KeywordStep},
    {"SUB", TokenKind::KeywordSub},
    {"SWAP", TokenKind::KeywordSwap},
    {"THEN", TokenKind::KeywordThen},
    {"TO", TokenKind::KeywordTo},
    {"TRUE", TokenKind::KeywordTrue},
    {"TRY", TokenKind::KeywordTry},
    {"TYPE", TokenKind::KeywordType},
    {"UBOUND", TokenKind::KeywordUbound},
    {"UNTIL", TokenKind::KeywordUntil},
    {"USING", TokenKind::KeywordUsing},
    {"VIRTUAL", TokenKind::KeywordVirtual},
    {"WEND", TokenKind::KeywordWend},
    {"WHILE", TokenKind::KeywordWhile},
    {"WRITE", TokenKind::KeywordWrite},
}};

// Verify keyword table is sorted at compile time using common utility
static_assert(isKeywordTableSorted(kKeywordTable),
              "Keyword table must be sorted lexicographically");

/// @brief Lookup a candidate identifier in the keyword table.
///
/// @details Uses the common binary search utility from KeywordTable.hpp.
///          Special-cases "ME" keyword for case-insensitive matching.
/// @param lexeme Uppercased identifier text to classify.
/// @return Keyword kind when recognised; @ref TokenKind::Identifier otherwise.
TokenKind lookupKeyword(std::string_view lexeme) {
    // Special case: "ME" keyword (case-insensitive)
    if (lexeme.size() == 2) {
        if (toUpper(lexeme[0]) == 'M' && toUpper(lexeme[1]) == 'E')
            lexeme = "ME";
    }

    // Use common binary search utility
    auto result = lookupKeywordBinary(kKeywordTable, lexeme);
    return result.value_or(TokenKind::Identifier);
}

/// @brief Check whether a character is valid in a hexadecimal literal.
/// @param c Character to classify.
/// @return True for ASCII decimal digits or A-F/a-f.
bool isHexDigit(char c) {
    const char upper = toUpper(c);
    return isDigit(c) || (upper >= 'A' && upper <= 'F');
}

/// @brief Check whether a character is valid in a binary literal.
/// @param c Character to classify.
/// @return True for `0` or `1`.
bool isBinaryDigit(char c) {
    return c == '0' || c == '1';
}

/// @brief Detect the radix prefixes accepted by the BASIC numeric grammar.
/// @details BASIC accepts both classic `&H`/`&B` prefixes and C-style
///          `0x`/`0b` spellings. The check is intentionally prefix-only; the
///          dedicated lexer validates that at least one digit follows.
/// @param src Full source buffer.
/// @param pos Current cursor position.
/// @return True when @p pos starts a based integer literal prefix.
bool startsBasedLiteral(std::string_view src, size_t pos) {
    if (pos + 1 >= src.size())
        return false;

    const char first = src[pos];
    const char second = toUpper(src[pos + 1]);
    if (first == '&')
        return second == 'H' || second == 'B';
    if (first == '0')
        return second == 'X' || second == 'B';
    return false;
}

} // namespace

/// @brief Construct a lexer over the given source buffer.
///
/// @details Stores lightweight views into @p src and primes the position
///          counters so the first call to @ref next observes the opening
///          character at line 1, column 1.  The caller retains ownership of the
///          underlying buffer for the lexer's lifetime.
/// @param src BASIC program text to scan; must outlive the lexer instance.
/// @param file_id Identifier used when emitting diagnostic locations.
Lexer::Lexer(std::string_view src, uint32_t file_id) : Base(file_id), src_(src) {}

/// @brief Skip spaces and tabs but stop at every supported line ending.
///
/// @details Whitespace between statements is ignored by BASIC except for
///          newline boundaries that influence statement grouping.  This helper
///          advances the cursor past horizontal whitespace while keeping
///          newlines in the stream for later tokenisation.
void Lexer::skipWhitespaceExceptNewline() {
    while (!eof()) {
        char c = peek();
        if (c == ' ' || c == '\t') {
            get();
        } else {
            break;
        }
    }
}

/// @brief Skip whitespace and BASIC comments starting with <tt>'</tt> or REM.
///
/// @details BASIC treats apostrophe-prefixed and "REM" tokens as
///          rest-of-line comments.  The helper repeatedly removes whitespace and
///          comment bodies so the next significant token begins at the current
///          cursor. The LF terminating a comment is preserved so callers can
///          emit @ref TokenKind::EndOfLine; a lone CR inside a comment is
///          consumed as part of that comment.
void Lexer::skipWhitespaceAndComments() {
    while (true) {
        skipWhitespaceExceptNewline();

        if (peek() == '\'') {
            while (!eof() && peek() != '\n')
                get();
            continue;
        }

        if (toUpper(peek()) == 'R' && pos_ + 2 < src_.size() && toUpper(src_[pos_ + 1]) == 'E' &&
            toUpper(src_[pos_ + 2]) == 'M') {
            char after = (pos_ + 3 < src_.size()) ? src_[pos_ + 3] : '\0';
            if (!isAlphanumeric(after) && after != '$' && after != '#' && after != '!' &&
                after != '%' && after != '&') {
                get();
                get();
                get();
                while (!eof() && peek() != '\n')
                    get();
                continue;
            }
        }

        break;
    }
}

/// @brief Lex a numeric literal including optional fraction, exponent, and type
///        suffix (<tt>%</tt>, <tt>&</tt>, <tt>!</tt>, <tt>#</tt>).
///
/// @details Consumes digits, a single decimal point, and an optional exponent
///          section before capturing trailing type designators.  The recognised
///          substring is returned verbatim so later stages can enforce precise
///          numeric semantics.  Location data is captured prior to any
///          consumption for accurate diagnostics.
/// @return Token of kind Number representing the characters consumed.
Token Lexer::lexNumber() {
    il::support::SourceLoc loc{fileId_, line_, column_};
    std::string s;
    bool seenDot = false;
    char suffix = '\0';
    /// Maximum retained decimal literal length before returning Unknown.
    constexpr size_t kMaxNumLen = 1024;
    if (peek() == '.') {
        seenDot = true;
        s.push_back(get());
    }
    while (isDigit(peek()) && s.size() < kMaxNumLen)
        s.push_back(get());
    if (s.size() >= kMaxNumLen) {
        while (isDigit(peek()))
            get();
        return {TokenKind::Unknown, s, loc};
    }
    if (!seenDot && peek() == '.') {
        seenDot = true;
        s.push_back(get());
        while (isDigit(peek()) && s.size() < kMaxNumLen)
            s.push_back(get());
        if (s.size() >= kMaxNumLen) {
            while (isDigit(peek()))
                get();
            return {TokenKind::Unknown, s, loc};
        }
    }
    if ((peek() == 'e' || peek() == 'E')) {
        s.push_back(get());
        if (peek() == '+' || peek() == '-')
            s.push_back(get());
        if (!isDigit(peek()))
            return {TokenKind::Unknown, s, loc};
        while (isDigit(peek()) && s.size() < kMaxNumLen)
            s.push_back(get());
        if (s.size() >= kMaxNumLen) {
            while (isDigit(peek()))
                get();
            return {TokenKind::Unknown, s, loc};
        }
    }
    if (peek() == '#' || peek() == '!' || peek() == '%' || peek() == '&')
        suffix = get();
    (void)seenDot;
    if (suffix != '\0')
        s.push_back(suffix);
    return {TokenKind::Number, s, loc};
}

/// @brief Lex a hexadecimal or binary integer literal.
///
/// @details Handles the four accepted radix prefixes:
///          - `&H` / `&h` for classic BASIC hexadecimal
///          - `&B` / `&b` for classic BASIC binary
///          - `0x` / `0X` for C-style hexadecimal
///          - `0b` / `0B` for C-style binary
///
///          The lexeme is canonicalized to uppercase while preserving the
///          leading `&` or `0`. Only integer suffixes are consumed because
///          based literals denote exact integers rather than decimal floating
///          values. A prefix with no following digit is returned as Unknown so
///          malformed input does not silently become zero during parsing.
/// @return Token of kind Number for valid based literals, Unknown otherwise.
Token Lexer::lexBasedNumber() {
    il::support::SourceLoc loc{fileId_, line_, column_};
    std::string s;
    /// Maximum retained based-literal length before returning Unknown.
    constexpr size_t kMaxNumLen = 1024;

    const char first = get();
    const char marker = toUpper(get());
    s.push_back(first);
    s.push_back(marker);

    const bool binary = marker == 'B';
    bool sawDigit = false;
    while (!eof() && s.size() < kMaxNumLen &&
           (binary ? isBinaryDigit(peek()) : isHexDigit(peek()))) {
        sawDigit = true;
        s.push_back(binary ? get() : toUpper(get()));
    }

    if (s.size() >= kMaxNumLen) {
        while (!eof() && (binary ? isBinaryDigit(peek()) : isHexDigit(peek())))
            get();
        return {TokenKind::Unknown, s, loc};
    }

    if (!sawDigit)
        return {TokenKind::Unknown, s, loc};

    if (peek() == '%' || peek() == '&')
        s.push_back(get());

    return {TokenKind::Number, s, loc};
}

/// @brief Lex an identifier or reserved keyword.
///
/// @details Characters are uppercased while they are consumed so keyword lookup
///          becomes a straightforward table search.  Optional type suffixes are
///          folded into the token text to match the semantics of the BASIC type
///          inference rules applied later in the pipeline.
/// @return Identifier or keyword token; identifiers are uppercased for keyword
///         comparison.
Token Lexer::lexIdentifierOrKeyword() {
    il::support::SourceLoc loc{fileId_, line_, column_};
    std::string s;
    /// Maximum retained identifier length before returning Unknown.
    constexpr size_t kMaxIdentLen = 1024;
    while (isAlphanumeric(peek()) || peek() == '_') {
        if (s.size() >= kMaxIdentLen) {
            // Skip remaining identifier characters to avoid OOM
            while (isAlphanumeric(peek()) || peek() == '_')
                get();
            return {TokenKind::Unknown, s, loc};
        }
        s.push_back(toUpper(get()));
    }
    if (peek() == '$' || peek() == '#' || peek() == '!' || peek() == '%' || peek() == '&')
        s.push_back(toUpper(get()));
    TokenKind kind = lookupKeyword(s);
    return {kind, s, loc};
}

/// @brief Lex a string literal delimited by double quotes.
///
/// @details Copies ordinary characters verbatim, treats backslash as ordinary,
///          and decodes each doubled quote into one quote byte. Newline, EOF
///          before closure, or the size cap produces Unknown for the parser to
///          diagnose.
/// @return String token containing the decoded interior, otherwise Unknown.
Token Lexer::lexString() {
    il::support::SourceLoc loc{fileId_, line_, column_};
    std::string s;
    /// Maximum decoded string payload retained by one token.
    constexpr size_t kMaxStringLen = 16 * 1024 * 1024; // 16MB
    get();                                             // consume opening quote
    bool closed = false;
    while (!eof()) {
        if (s.size() >= kMaxStringLen) {
            // Skip to closing quote or EOF to avoid OOM
            while (!eof() && peek() != '"')
                get();
            if (!eof())
                get(); // consume closing quote
            return {TokenKind::Unknown, s, loc};
        }
        if (peek() == '\n')
            return {TokenKind::Unknown, s, loc};
        if (peek() == '"') {
            get(); // consume the quote
            // Check for "" (double-quote escape convention in BASIC)
            if (peek() == '"') {
                s.push_back('"');
                get(); // consume the second quote
            } else {
                closed = true;
                break; // closing quote
            }
        } else {
            // In BASIC, backslash has no special meaning - it's just a regular character.
            s.push_back(get());
        }
    }
    return {closed ? TokenKind::String : TokenKind::Unknown, s, loc};
}

/// @brief Retrieve the next token from the input stream.
///
/// @details Skips insignificant trivia, returns explicit newline tokens, and
///          dispatches to specialised lexers for numbers, identifiers, and
///          strings.  Punctuation is handled inline via a switch statement to
///          keep hot paths branch-friendly.  Location metadata is captured for
///          every token so diagnostics can point back to the source program.
///          An underscore followed by horizontal whitespace and LF suppresses
///          that line break and restarts scanning; other standalone underscores
///          produce Unknown.
/// @return The next token, which may be EndOfLine or EndOfFile.
Token Lexer::next() {
    for (;;) {
        skipWhitespaceAndComments();

        if (eof())
            return {TokenKind::EndOfFile, "", {fileId_, line_, column_}};

        char c = peek();

        if (c == '\r' || c == '\n') {
            il::support::SourceLoc loc{fileId_, line_, column_};
            get();
            return {TokenKind::EndOfLine, "\n", loc};
        }

        if (startsBasedLiteral(src_, pos_))
            return lexBasedNumber();
        if (isDigit(c) || (c == '.' && pos_ + 1 < src_.size() && isDigit(src_[pos_ + 1])))
            return lexNumber();
        if (isLetter(c))
            return lexIdentifierOrKeyword();
        if (c == '"')
            return lexString();

        il::support::SourceLoc loc{fileId_, line_, column_};
        get();
        switch (c) {
            case '+':
                return {TokenKind::Plus, "+", loc};
            case '-':
                return {TokenKind::Minus, "-", loc};
            case '*':
                return {TokenKind::Star, "*", loc};
            case '/':
                return {TokenKind::Slash, "/", loc};
            case '\\':
                return {TokenKind::Backslash, "\\", loc};
            case '^':
                return {TokenKind::Caret, "^", loc};
            case '&':
                return {TokenKind::Ampersand, "&", loc};
            case '=':
                return {TokenKind::Equal, "=", loc};
            case '<':
                if (peek() == '>') {
                    get();
                    return {TokenKind::NotEqual, "<>", loc};
                }
                if (peek() == '=') {
                    get();
                    return {TokenKind::LessEqual, "<=", loc};
                }
                return {TokenKind::Less, "<", loc};
            case '>':
                if (peek() == '=') {
                    get();
                    return {TokenKind::GreaterEqual, ">=", loc};
                }
                return {TokenKind::Greater, ">", loc};
            case '(':
                return {TokenKind::LParen, "(", loc};
            case ')':
                return {TokenKind::RParen, ")", loc};
            case ',':
                return {TokenKind::Comma, ",", loc};
            case ';':
                return {TokenKind::Semicolon, ";", loc};
            case ':':
                return {TokenKind::Colon, ":", loc};
            case '#':
                return {TokenKind::Hash, "#", loc};
            case '.': {
                // If previous and next chars are digits, this is part of a numeric literal;
                // fallthrough to number logic. Otherwise, return TokenKind::Dot.
                bool prevIsDigit = false;
                if (pos_ >= 2) {
                    prevIsDigit = isDigit(src_[pos_ - 2]);
                }
                bool nextIsDigit = false;
                if (pos_ < src_.size()) {
                    nextIsDigit = isDigit(src_[pos_]);
                }
                if (prevIsDigit && nextIsDigit) {
                    if (column_ > 1)
                        --column_;
                    --pos_;
                    return lexNumber();
                }
                return {TokenKind::Dot, ".", loc};
            }
            case '_': {
                // Line continuation: _ followed by optional whitespace and newline
                // Skip horizontal whitespace after _
                while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\r'))
                    get();

                // Check if followed by newline
                if (!eof() && peek() == '\n') {
                    get(); // consume the newline
                    // Restart token dispatch (iterative, avoids stack overflow)
                    continue;
                }
                // Otherwise, _ is an unknown character
                return {TokenKind::Unknown, "_", loc};
            }
        }
        return {TokenKind::Unknown, std::string(1, c), loc};
    } // for(;;)
}

} // namespace il::frontends::basic
