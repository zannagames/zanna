//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements BASIC parser helpers for IO-related statements. The routines live
// in a dedicated translation unit so the main parser remains focused on general
// statement handling while these helpers concentrate on separator and channel
// peculiarities.
//
//===----------------------------------------------------------------------===//
//
/// @file Parser_Stmt_IO.cpp
/// @brief Parsing utilities for BASIC IO statements (PRINT, INPUT, OPEN, etc.).
/// @details The functions consume tokens from @ref Parser, build the appropriate
///          AST nodes, and report diagnostics through the active diagnostic
///          emitter when malformed statements are encountered.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Parser.hpp"

namespace il::frontends::basic {

/// @brief Register parsing functions for IO-related statement keywords.
/// @details Populates the provided registry so the generic parser dispatch can
///          map BASIC keywords (PRINT, OPEN, etc.) to their specialised handler
///          methods on @ref Parser.
/// @param registry Dispatcher that maps tokens to member function pointers.
void Parser::registerIoParsers(StatementParserRegistry &registry) {
    registry.registerHandler(TokenKind::KeywordPrint, &Parser::parsePrintStatement);
    registry.registerHandler(TokenKind::KeywordWrite, &Parser::parseWriteStatement);
    registry.registerHandler(TokenKind::KeywordOpen, &Parser::parseOpenStatement);
    registry.registerHandler(TokenKind::KeywordClose, &Parser::parseCloseStatement);
    registry.registerHandler(TokenKind::KeywordSeek, &Parser::parseSeekStatement);
    registry.registerHandler(TokenKind::KeywordInput, &Parser::parseInputStatement);
}

/// @brief Parse the PRINT statement, supporting both console and channel forms.
/// @details Consumes the `PRINT` token and distinguishes between the standard
///          variant and the `PRINT #` channel form. Expressions, commas, and
///          semicolons are appended to the resulting AST node until a statement
///          terminator is encountered.
/// @return Newly allocated AST node representing the parsed statement.
StmtPtr Parser::parsePrintStatement() {
    auto loc = peek().loc;
    consume(); // PRINT
    if (at(TokenKind::Hash)) {
        consume();
        auto stmt = std::make_unique<PrintChStmt>();
        stmt->loc = loc;
        stmt->mode = PrintChStmt::Mode::Print;
        stmt->channelExpr = parseExpression();
        stmt->trailingNewline = true;
        bool lastWasSemicolon = false;
        if (at(TokenKind::Comma)) {
            consume();
            while (true) {
                if (at(TokenKind::EndOfLine) || at(TokenKind::EndOfFile) || at(TokenKind::Colon))
                    break;
                // BUG-OOP-021: Allow soft keywords as expressions in PRINT#.
                bool isSoftKw =
                    isSoftIdentToken(peek().kind) && peek().kind != TokenKind::Identifier;
                if (isStatementStart(peek().kind) && !isSoftKw)
                    break;
                if (at(TokenKind::Semicolon)) {
                    consume();
                    lastWasSemicolon = true;
                    continue;
                }
                lastWasSemicolon = false;
                stmt->args.push_back(parseExpression());
                if (at(TokenKind::Semicolon)) {
                    consume();
                    lastWasSemicolon = true;
                    continue;
                }
                if (!at(TokenKind::Comma))
                    break;
                consume();
            }
            if (lastWasSemicolon)
                stmt->trailingNewline = false;
        }
        return stmt;
    }
    auto stmt = std::make_unique<PrintStmt>();
    stmt->loc = loc;
    while (!at(TokenKind::EndOfLine) && !at(TokenKind::EndOfFile) && !at(TokenKind::Colon)) {
        TokenKind k = peek().kind;
        // BUG-OOP-021: Allow soft keywords (COLOR, FLOOR, etc.) as expressions in PRINT.
        // This enables: PRINT color   (where 'color' is a variable)
        bool isSoftKw = isSoftIdentToken(k) && k != TokenKind::Identifier;
        if (isStatementStart(k) && !isSoftKw)
            break;
        if (at(TokenKind::Comma)) {
            consume();
            stmt->items.push_back(PrintItem{PrintItem::Kind::Comma, nullptr});
            continue;
        }
        if (at(TokenKind::Semicolon)) {
            consume();
            stmt->items.push_back(PrintItem{PrintItem::Kind::Semicolon, nullptr});
            continue;
        }
        stmt->items.push_back(PrintItem{PrintItem::Kind::Expr, parseExpression()});
    }
    return stmt;
}

/// @brief Parse the WRITE# statement.
/// @details Handles channel-prefixed WRITE statements, requiring a hash marker
///          and comma-separated expression list. Unlike PRINT#, WRITE# does not
///          permit null items, so expressions are parsed greedily until no more
///          commas remain.
/// @return AST node representing the WRITE# statement.
StmtPtr Parser::parseWriteStatement() {
    auto loc = peek().loc;
    consume(); // WRITE
    expect(TokenKind::Hash);
    auto stmt = std::make_unique<PrintChStmt>();
    stmt->loc = loc;
    stmt->mode = PrintChStmt::Mode::Write;
    stmt->trailingNewline = true;
    stmt->channelExpr = parseExpression();
    expect(TokenKind::Comma);
    while (true) {
        stmt->args.push_back(parseExpression());
        if (!at(TokenKind::Comma))
            break;
        consume();
    }
    return stmt;
}

/// @brief Parse the OPEN statement configuring file channels.
/// @details Parses a required path expression, `FOR` mode, and `AS #` channel
///          expression. Supported modes are INPUT, OUTPUT, APPEND, BINARY, and
///          RANDOM; an unrecognized mode token is consumed and diagnosed
///          through the configured emitter when present.
/// @return AST node describing the OPEN statement.
StmtPtr Parser::parseOpenStatement() {
    auto loc = peek().loc;
    consume(); // OPEN
    auto stmt = std::make_unique<OpenStmt>();
    stmt->loc = loc;
    stmt->pathExpr = parseExpression();
    expect(TokenKind::KeywordFor);
    if (at(TokenKind::KeywordInput)) {
        consume();
        stmt->mode = OpenStmt::Mode::Input;
    } else if (at(TokenKind::KeywordOutput)) {
        consume();
        stmt->mode = OpenStmt::Mode::Output;
    } else if (at(TokenKind::KeywordAppend)) {
        consume();
        stmt->mode = OpenStmt::Mode::Append;
    } else if (at(TokenKind::KeywordBinary)) {
        consume();
        stmt->mode = OpenStmt::Mode::Binary;
    } else if (at(TokenKind::KeywordRandom)) {
        consume();
        stmt->mode = OpenStmt::Mode::Random;
    } else {
        Token unexpected = consume();
        if (emitter_) {
            emitter_->emitExpected(unexpected.kind, TokenKind::KeywordInput, unexpected.loc);
        }
    }
    expect(TokenKind::KeywordAs);
    expect(TokenKind::Hash);
    stmt->channelExpr = parseExpression();
    return stmt;
}

/// @brief Parse the CLOSE statement.
/// @details Requires `CLOSE #` followed by an expression naming the channel to
///          close.
/// @return AST node describing the CLOSE statement.
StmtPtr Parser::parseCloseStatement() {
    auto loc = peek().loc;
    consume(); // CLOSE
    auto stmt = std::make_unique<CloseStmt>();
    stmt->loc = loc;
    expect(TokenKind::Hash);
    stmt->channelExpr = parseExpression();
    return stmt;
}

/// @brief Parse the SEEK statement.
/// @details Expects `SEEK #` followed by the channel expression and a comma
///          separating the position expression. Both operands are parsed as
///          general expressions.
/// @return AST node describing the SEEK statement.
StmtPtr Parser::parseSeekStatement() {
    auto loc = peek().loc;
    consume(); // SEEK
    auto stmt = std::make_unique<SeekStmt>();
    stmt->loc = loc;
    expect(TokenKind::Hash);
    stmt->channelExpr = parseExpression();
    expect(TokenKind::Comma);
    stmt->positionExpr = parseExpression();
    return stmt;
}

/// @brief Parse the INPUT statement, supporting prompt and variable lists.
/// @details Console INPUT accepts an optional literal-string prompt followed by
///          one or more soft-identifier destinations. Channel INPUT requires a
///          numeric channel literal after `#` and accepts one or more NameRef
///          targets separated by commas; an invalid channel recovers as zero.
/// @return AST node for the parsed INPUT statement.
StmtPtr Parser::parseInputStatement() {
    auto loc = peek().loc;
    consume(); // INPUT
    if (at(TokenKind::Hash)) {
        consume();
        Token channelTok = expect(TokenKind::Number);
        int channel = 0;
        if (channelTok.kind == TokenKind::Number) {
            if (auto parsed = parseLineNumberLiteral(channelTok.lexeme))
                channel = *parsed;
            else
                emitError("B0001", channelTok, "channel number is out of range");
        }
        expect(TokenKind::Comma);
        auto stmt = std::make_unique<InputChStmt>();
        stmt->loc = loc;
        stmt->channel = channel;
        // Parse one or more comma-separated identifier targets
        while (true) {
            Token targetTok = expectSoftIdentifier();
            NameRef ref;
            ref.name = targetTok.lexeme;
            ref.loc = targetTok.loc;
            stmt->targets.push_back(std::move(ref));
            if (!at(TokenKind::Comma))
                break;
            consume();
        }
        return stmt;
    }
    ExprPtr prompt;
    if (at(TokenKind::String)) {
        prompt = makeStrExpr(peek().lexeme, peek().loc);
        consume();
        expect(TokenKind::Comma);
    }
    auto stmt = std::make_unique<InputStmt>();
    stmt->loc = loc;
    stmt->prompt = std::move(prompt);

    Token nameTok = expectSoftIdentifier();
    stmt->vars.push_back(nameTok.lexeme);

    while (at(TokenKind::Comma)) {
        consume();
        Token nextTok = expectSoftIdentifier();
        stmt->vars.push_back(nextTok.lexeme);
    }

    return stmt;
}

/// @brief Parse the `LINE INPUT` statement that reads an entire line.
/// @details Console LINE INPUT accepts an optional literal prompt and one soft
///          identifier. The channel-prefixed form parses a channel expression
///          and requires its destination to be a scalar variable or array
///          element. An invalid channel target is diagnosed and replaced by an
///          unnamed VarExpr recovery node.
/// @return AST node describing the LINE INPUT statement.
StmtPtr Parser::parseLineInputStatement() {
    auto loc = peek().loc;
    consume(); // LINE
    expect(TokenKind::KeywordInput);

    if (!at(TokenKind::Hash)) {
        ExprPtr prompt;
        if (at(TokenKind::String)) {
            prompt = makeStrExpr(peek().lexeme, peek().loc);
            consume();
            expect(TokenKind::Comma);
        }

        auto stmt = std::make_unique<InputStmt>();
        stmt->loc = loc;
        stmt->prompt = std::move(prompt);
        Token targetTok = expectSoftIdentifier();
        stmt->vars.push_back(targetTok.lexeme);
        return stmt;
    }

    expect(TokenKind::Hash);
    auto stmt = std::make_unique<LineInputChStmt>();
    stmt->loc = loc;
    stmt->channelExpr = parseExpression();
    expect(TokenKind::Comma);
    auto target = parseArrayOrVar();
    Expr *rawTarget = target.get();
    if (rawTarget && !is<VarExpr>(*rawTarget) && !is<ArrayExpr>(*rawTarget)) {
        il::support::SourceLoc diagLoc = rawTarget->loc.hasLine() ? rawTarget->loc : loc;
        emitError("B0001", diagLoc, "expected variable");
        auto fallback = std::make_unique<VarExpr>();
        fallback->loc = diagLoc;
        stmt->targetVar = std::move(fallback);
    } else {
        stmt->targetVar = std::move(target);
    }
    return stmt;
}

} // namespace il::frontends::basic
