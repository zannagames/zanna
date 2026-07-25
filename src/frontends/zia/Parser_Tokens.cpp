//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Tokens.cpp
/// @brief Token buffering and error handling for the Zia parser.
///
/// @details Supplies on-demand multi-token lookahead, bounded speculative
///          rollback, token consumption and compaction, contextual identifier
///          classification, syntax disambiguation lookahead, error recovery,
///          and stable parse diagnostic classification/ranges.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace il::frontends::zia {
namespace {

/// @brief True if @p text begins with @p prefix.
/// @param text Text to inspect.
/// @param prefix Required prefix.
/// @return True when @p prefix occurs at offset zero.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

/// @brief True if @p needle occurs anywhere in @p text.
/// @param text Text to inspect.
/// @param needle Substring to locate.
/// @return True when @p needle occurs in @p text.
bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

/// @brief Select a stable parse diagnostic code from its message category.
/// @param message Parser diagnostic text.
/// @return Specific `V-ZIA-PARSE-*` code, or the general parse code.
std::string classifyParserDiagnostic(std::string_view message) {
    if (contains(message, "nesting too deep"))
        return "V-ZIA-PARSE-DEPTH";
    if (contains(message, "literal") || contains(message, "out of range"))
        return "V-ZIA-PARSE-LITERAL";
    if (contains(message, "type parameter") || contains(message, "expected type") ||
        contains(message, "fixed-size array"))
        return "V-ZIA-PARSE-TYPE";
    if (contains(message, "lambda parameters"))
        return "V-ZIA-PARSE-LAMBDA";
    if (contains(message, "pattern"))
        return "V-ZIA-PARSE-PATTERN";
    if (contains(message, "string interpolation") || contains(message, "interpolated string"))
        return "V-ZIA-PARSE-STRING";
    if (contains(message, "declaration") || contains(message, "module level") ||
        contains(message, "function body") || contains(message, "field") ||
        contains(message, "method") || contains(message, "property") ||
        contains(message, "namespace"))
        return "V-ZIA-PARSE-DECL";
    if (contains(message, "assignment target") || contains(message, "lvalue"))
        return "V-ZIA-PARSE-ASSIGNMENT";
    if (contains(message, "expected expression"))
        return "V-ZIA-PARSE-EXPR";
    if (startsWith(message, "expected "))
        return "V-ZIA-PARSE-EXPECTED";
    if (startsWith(message, "unexpected "))
        return "V-ZIA-PARSE-UNEXPECTED";
    return "V-ZIA-PARSE";
}

/// @brief Construct the highlighted source range for a parser diagnostic.
/// @param token Current token used to determine matched text length.
/// @param loc Requested diagnostic start location.
/// @return One-character or token-width range, or an invalid range when
///         @p loc is invalid.
il::support::SourceRange tokenRange(const Token &token, SourceLoc loc) {
    if (!loc.isValid())
        return {};
    uint32_t length = 1;
    if (token.loc.file_id == loc.file_id && token.loc.line == loc.line &&
        token.loc.column == loc.column) {
        length = static_cast<uint32_t>(std::max<std::size_t>(1, token.text.size()));
    }
    return il::support::SourceRange{
        loc,
        il::support::SourceLoc{loc.file_id, loc.line, loc.column + length},
    };
}

} // namespace

/// @brief Construct a parser and seed its lookahead buffer.
/// @param lexer Borrowed token source.
/// @param diag Borrowed diagnostic destination.
Parser::Parser(Lexer &lexer, il::support::DiagnosticEngine &diag) : lexer_(lexer), diag_(diag) {
    tokens_.push_back(lexer_.next());
}

//===----------------------------------------------------------------------===//
// Token Handling
//===----------------------------------------------------------------------===//

/// @brief Begin a diagnostic-suppressed speculative parse.
/// @param parser Parser whose position and error state are snapshotted.
Parser::Speculation::Speculation(Parser &parser)
    : parser_(parser), savedPos_(parser.tokenPos_), savedHasError_(parser.hasError_) {
    ++parser_.suppressionDepth_;
}

/// @brief End speculation, rolling back unless commit() accepted the parse.
Parser::Speculation::~Speculation() {
    --parser_.suppressionDepth_;
    if (!committed_) {
        parser_.tokenPos_ = savedPos_;
        parser_.hasError_ = savedHasError_;
    }
}

/// @brief Inspect a token without consuming it.
/// @param offset Lookahead distance from the current token.
/// @return Stable reference into the on-demand token buffer.
const Token &Parser::peek(size_t offset) {
    while (tokens_.size() <= tokenPos_ + offset) {
        tokens_.push_back(lexer_.next());
    }
    return tokens_[tokenPos_ + offset];
}

/// @brief Consume and return the current token.
/// @return Token at the old parser position.
Token Parser::advance() {
    Token cur = peek();
    ++tokenPos_;
    compactBufferedTokens();
    return cur;
}

/// @brief Test a buffered token kind without consuming it.
/// @param kind Expected token kind.
/// @param offset Lookahead distance.
/// @return True when peek(@p offset) has @p kind.
bool Parser::check(TokenKind kind, size_t offset) {
    return peek(offset).kind == kind;
}

/// @brief Test whether the current token may serve as an identifier.
/// @return True for identifiers and the supported contextual keywords.
bool Parser::checkIdentifierLike() {
    // Allow identifiers and certain contextual keywords that can be used as names
    if (peek().kind == TokenKind::Identifier)
        return true;

    // These keywords can be used as identifiers in parameter/variable contexts
    switch (peek().kind) {
        case TokenKind::KwStruct: // Common parameter name (e.g., setValue(Integer value))
        case TokenKind::KwMatch:  // Common variable name (e.g., var match = false)
            return true;
        default:
            return false;
    }
}

/// @brief Classify tokens that can begin a Zia expression.
/// @param kind Token kind to classify.
/// @return True when expression parsing can start with @p kind.
bool Parser::isExpressionStart(TokenKind kind) const {
    switch (kind) {
        case TokenKind::Identifier:
        case TokenKind::IntegerLiteral:
        case TokenKind::NumberLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::StringStart:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwNull:
        case TokenKind::KwSelf:
        case TokenKind::KwSuper:
        case TokenKind::KwNew:
        case TokenKind::KwMatch:
        case TokenKind::KwStruct:
        case TokenKind::KwIf:
        case TokenKind::KwAwait:
        case TokenKind::LParen:
        case TokenKind::LBracket:
        case TokenKind::LBrace:
        case TokenKind::Minus:
        case TokenKind::Bang:
        case TokenKind::Tilde:
        case TokenKind::Ampersand:
        case TokenKind::KwNot:
            return true;
        default:
            return false;
    }
}

/// @brief Speculatively distinguish match syntax from an identifier named
///        `match`.
/// @return True when a scrutinee followed by `{` parses after the keyword.
bool Parser::isMatchExpressionAhead() {
    if (!check(TokenKind::KwMatch))
        return false;

    Speculation speculation(*this);
    advance(); // consume `match`

    ExprPtr scrutinee = parseExpression();
    if (!scrutinee)
        return false;

    return check(TokenKind::LBrace);
}

/// @brief Distinguish a value-producing block from a map/set literal.
/// @return True when bounded lookahead finds statement syntax or a top-level
///         semicolon before the closing delimiter.
bool Parser::looksLikeBlockExpression() {
    if (!check(TokenKind::LBrace))
        return false;

    switch (peek(1).kind) {
        case TokenKind::KwVar:
        case TokenKind::KwFinal:
        case TokenKind::KwLet:
        case TokenKind::KwReturn:
        case TokenKind::KwBreak:
        case TokenKind::KwContinue:
        case TokenKind::KwDefer:
        case TokenKind::KwThrow:
        case TokenKind::KwGuard:
        case TokenKind::KwWhile:
        case TokenKind::KwFor:
        case TokenKind::KwTry:
            return true;
        case TokenKind::KwMatch: {
            Speculation speculation(*this);
            advance(); // consume the outer `{` so the `match` token is current
            return isMatchExpressionAhead();
        }
        default:
            break;
    }

    int nestedParen = 0;
    int nestedBracket = 0;
    int nestedBrace = 0;
    constexpr size_t kMaxBlockExpressionLookahead = 512;
    for (size_t offset = 1; offset <= kMaxBlockExpressionLookahead; ++offset) {
        TokenKind kind = peek(offset).kind;
        if (kind == TokenKind::Eof)
            return false;

        if (nestedParen == 0 && nestedBracket == 0 && nestedBrace == 0) {
            if (kind == TokenKind::Semicolon)
                return true;
            if (kind == TokenKind::RBrace || kind == TokenKind::Comma || kind == TokenKind::Colon ||
                kind == TokenKind::FatArrow) {
                return false;
            }
        }

        switch (kind) {
            case TokenKind::LParen:
                ++nestedParen;
                break;
            case TokenKind::RParen:
                if (nestedParen > 0)
                    --nestedParen;
                break;
            case TokenKind::LBracket:
                ++nestedBracket;
                break;
            case TokenKind::RBracket:
                if (nestedBracket > 0)
                    --nestedBracket;
                break;
            case TokenKind::LBrace:
                ++nestedBrace;
                break;
            case TokenKind::RBrace:
                if (nestedBrace > 0)
                    --nestedBrace;
                break;
            default:
                break;
        }
    }
    return false;
}

/// @brief Consume the current token when it has a requested kind.
/// @param kind Token kind to match.
/// @param[out] out Optional destination for the consumed token.
/// @return True when a token was consumed.
bool Parser::match(TokenKind kind, Token *out) {
    if (check(kind)) {
        Token tok = advance();
        if (out)
            *out = tok;
        return true;
    }
    return false;
}

/// @brief Require and consume a token kind.
/// @param kind Required token kind.
/// @param what Human-readable expectation for diagnostics.
/// @param[out] out Optional destination for the consumed token.
/// @return True on match; false after reporting an error.
bool Parser::expect(TokenKind kind, const char *what, Token *out) {
    if (check(kind)) {
        Token tok = advance();
        if (out)
            *out = tok;
        return true;
    }
    error(std::string("expected ") + what + ", got " + tokenKindToString(peek().kind));
    return false;
}

/// @brief Discard consumed lookahead tokens when rollback is impossible.
/// @details Compaction is suppressed during speculation and occurs only after
///          the consumed-prefix threshold is reached.
void Parser::compactBufferedTokens() {
    // Do not compact during speculative parsing; saved token positions are
    // relative to the current buffer.
    if (suppressionDepth_ > 0)
        return;

    constexpr size_t kCompactThreshold = 256;
    if (tokenPos_ < kCompactThreshold)
        return;

    tokens_.erase(tokens_.begin(), tokens_.begin() + static_cast<std::ptrdiff_t>(tokenPos_));
    tokenPos_ = 0;
}

/// @brief Advance to a likely declaration or statement boundary after error.
/// @details Stops at semicolon, closing brace, declaration keyword, EOF, or
///          the bounded 10,000-token recovery limit.
void Parser::resyncAfterError() {
    // Bounded token consumption prevents compiler hang on pathological input
    // lacking statement boundaries.
    constexpr unsigned kMaxResyncTokens = 10000;
    unsigned consumed = 0;

    while (!check(TokenKind::Eof) && consumed < kMaxResyncTokens) {
        if (check(TokenKind::Semicolon)) {
            advance();
            return;
        }
        if (check(TokenKind::RBrace) || check(TokenKind::KwFunc) || check(TokenKind::KwStruct) ||
            check(TokenKind::KwClass) || check(TokenKind::KwInterface)) {
            return;
        }
        advance();
        ++consumed;
    }
    if (consumed == kMaxResyncTokens && !check(TokenKind::Eof))
        errorAt(peek().loc, "stopped error recovery after 10000 tokens");
}

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

/// @brief Report a parse error at the current token.
/// @param message Human-readable diagnostic text.
void Parser::error(const std::string &message) {
    errorAt(peek().loc, message);
}

/// @brief Report a classified parse error at a specific location.
/// @param loc Diagnostic start location.
/// @param message Human-readable diagnostic text.
/// @details Speculative parses suppress reporting and do not set hasError_.
void Parser::errorAt(SourceLoc loc, const std::string &message) {
    if (suppressionDepth_ > 0)
        return;
    hasError_ = true;
    const Token &token = peek();
    il::support::Diagnostic diag{
        il::support::Severity::Error,
        message,
        loc,
        classifyParserDiagnostic(message),
    };
    diag.range = tokenRange(token, loc);
    diag.stage = "parse";
    diag_.report(std::move(diag));
}

} // namespace il::frontends::zia
