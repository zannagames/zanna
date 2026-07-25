//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser_Stmt_Jump.cpp
// Purpose: Parse BASIC control-transfer statements (GOTO, GOSUB, RETURN).
// Key invariants: Each parser consumes tokens in lock-step with the lexer,
//                 produces heap-allocated AST nodes, and records source
//                 locations for later diagnostics.
// Ownership/Lifetime: Returned AST nodes use std::unique_ptr semantics; the
//                     parser retains no ownership once the node is returned to
//                     the caller.
// Links: docs/tutorials/basic-tutorial.md#statements, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file Parser_Stmt_Jump.cpp
/// @brief Implements jump-oriented BASIC statement parsers.
/// @details Provides the parsing routines for GOTO, GOSUB, and RETURN statements
///          and returns heap-allocated AST nodes describing the parsed constructs.

#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Parser.hpp"

namespace il::frontends::basic {

/// @brief Parse a `GOTO <line>` statement.
/// @details The routine expects the current token stream to be positioned at the
///          `GOTO` keyword.  It consumes the keyword, parses the trailing line
///          label (numeric or named), and materialises a @c GotoStmt containing
///          the resolved destination together with the originating source
///          location. Named targets receive stable synthetic numbers and record
///          a forward reference. An out-of-range numeric target is diagnosed and
///          retained as zero; a missing target synchronizes and returns null.
/// @return Owned AST node describing the jump, or null when no target token exists.
StmtPtr Parser::parseGotoStatement() {
    auto loc = peek().loc;
    consume(); // GOTO
    int target = 0;
    if (at(TokenKind::Identifier)) {
        Token targetTok = consume();
        target = ensureLabelNumber(targetTok.lexeme);
        noteNamedLabelReference(targetTok, target);
    } else if (at(TokenKind::Number)) {
        Token targetTok = consume();
        if (auto parsed = parseLineNumberLiteral(targetTok.lexeme)) {
            target = *parsed;
            noteNumericLabelUsage(target);
        } else {
            emitError("B0001", targetTok, "line number is out of range");
        }
    } else {
        Token unexpected = peek();
        emitError("B0001", unexpected, "expected label or number after GOTO");
        syncToStmtBoundary();
        return nullptr;
    }
    auto stmt = std::make_unique<GotoStmt>();
    stmt->loc = loc;
    stmt->target = target;
    return stmt;
}

/// @brief Parse a `GOSUB <line>` statement.
/// @details After consuming the `GOSUB` keyword the parser requires either a
///          numeric literal or a named label that identifies the subroutine
///          entry line.  The resulting @c GosubStmt records both the call-site
///          location and the resolved target line. Named targets receive stable
///          synthetic numbers and record a forward reference. An out-of-range
///          number is diagnosed and retained as zero; a missing target
///          synchronizes and returns null.
/// @return Owned AST node describing the call target, or null when no target token exists.
StmtPtr Parser::parseGosubStatement() {
    auto loc = peek().loc;
    consume(); // GOSUB
    int target = 0;
    if (at(TokenKind::Identifier)) {
        Token targetTok = consume();
        target = ensureLabelNumber(targetTok.lexeme);
        noteNamedLabelReference(targetTok, target);
    } else if (at(TokenKind::Number)) {
        Token targetTok = consume();
        if (auto parsed = parseLineNumberLiteral(targetTok.lexeme)) {
            target = *parsed;
            noteNumericLabelUsage(target);
        } else {
            emitError("B0001", targetTok, "line number is out of range");
        }
    } else {
        Token unexpected = peek();
        emitError("B0001", unexpected, "expected label or number after GOSUB");
        syncToStmtBoundary();
        return nullptr;
    }
    auto stmt = std::make_unique<GosubStmt>();
    stmt->loc = loc;
    stmt->targetLine = target;
    return stmt;
}

/// @brief Parse a `RETURN [expr]` statement.
/// @details Consumes the `RETURN` keyword, captures the current source
///          location, and optionally parses a trailing expression that supplies
///          a return value when present.  Parsing halts at statement separators
///          (`:`, end-of-line, or end-of-file) so chained statements are left in
///          the token buffer for subsequent parsers.  The resulting @c ReturnStmt
///          carries either a populated expression or a null pointer to indicate
///          a void-style return.
/// @return Owned AST node describing the return statement.
StmtPtr Parser::parseReturnStatement() {
    auto loc = peek().loc;
    consume(); // RETURN
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->loc = loc;
    if (!at(TokenKind::EndOfLine) && !at(TokenKind::EndOfFile) && !at(TokenKind::Colon))
        stmt->value = parseExpression();
    return stmt;
}

} // namespace il::frontends::basic
