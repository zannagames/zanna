//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/Parser_Stmt_Loop.cpp
// Purpose: Implement loop-related statement parsing for the BASIC parser.
// Key invariants: Ensures loop headers and terminators are matched and
//                 diagnostics cover invalid configurations.
// Ownership/Lifetime: Parser creates AST nodes managed by caller-owned
//                     std::unique_ptr wrappers.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#loops
//
//===----------------------------------------------------------------------===//

/// @file Parser_Stmt_Loop.cpp
/// @brief Implements BASIC WHILE, DO, FOR, NEXT, and EXIT parsing.
/// @details Loop bodies are collected by StatementSequencer and transferred
///          into owned AST nodes. This parser records syntactic loop forms;
///          nesting and target validity are left to later validation.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/Parser_Stmt_ControlHelpers.hpp"

namespace il::frontends::basic {

/// @brief Parse a `WHILE ... WEND` loop statement.
///
/// @details Consumes the `WHILE` keyword, parses the condition expression, and
///          delegates to the statement sequencer to collect the body until the
///          matching `WEND`.  The resulting AST node owns the body statements
///          and records the loop header location for diagnostics.
///
/// @return Newly allocated @ref WhileStmt representing the loop.
StmtPtr Parser::parseWhileStatement() {
    auto loc = peek().loc;
    consume(); // WHILE
    auto cond = parseExpression();
    auto stmt = std::make_unique<WhileStmt>();
    stmt->loc = loc;
    stmt->cond = std::move(cond);
    auto ctxWhile = statementSequencer();
    ctxWhile.collectStatements(TokenKind::KeywordWend, stmt->body);
    return stmt;
}

/// @brief Parse the flexible `DO` loop family.
///
/// @details Supports pre-test (`DO WHILE`/`DO UNTIL`) and post-test (`LOOP
///          WHILE`/`LOOP UNTIL`) forms, reporting diagnostics when both are
///          specified simultaneously.  The body is gathered until the closing
///          `LOOP`, and optional conditions are stored on the AST node along
///          with their source locations.
///
/// @return Newly allocated @ref DoStmt capturing the loop semantics.
StmtPtr Parser::parseDoStatement() {
    auto loc = peek().loc;
    consume(); // DO
    auto stmt = std::make_unique<DoStmt>();
    stmt->loc = loc;

    bool hasPreTest = false;
    if (at(TokenKind::KeywordWhile) || at(TokenKind::KeywordUntil)) {
        hasPreTest = true;
        Token testTok = consume();
        stmt->testPos = DoStmt::TestPos::Pre;
        stmt->condKind = testTok.kind == TokenKind::KeywordWhile ? DoStmt::CondKind::While
                                                                 : DoStmt::CondKind::Until;
        stmt->cond = parseExpression();
    }

    auto ctxDo = statementSequencer();
    ctxDo.collectStatements(TokenKind::KeywordLoop, stmt->body);

    bool hasPostTest = false;
    Token postTok{};
    DoStmt::CondKind postKind = DoStmt::CondKind::None;
    ExprPtr postCond;
    if (at(TokenKind::KeywordWhile) || at(TokenKind::KeywordUntil)) {
        hasPostTest = true;
        postTok = consume();
        postKind = postTok.kind == TokenKind::KeywordWhile ? DoStmt::CondKind::While
                                                           : DoStmt::CondKind::Until;
        postCond = parseExpression();
    }

    if (hasPreTest && hasPostTest) {
        emitError("B0001", postTok, "DO loop cannot have both pre and post conditions");
    } else if (hasPostTest) {
        stmt->testPos = DoStmt::TestPos::Post;
        stmt->condKind = postKind;
        stmt->cond = std::move(postCond);
    }

    return stmt;
}

/// @brief Parse a `FOR` counting loop or `FOR EACH` iteration loop.
///
/// @details Recognizes two forms:
///          1. FOR var = start TO end [STEP step] ... NEXT
///          2. FOR EACH element IN array ... NEXT
///          Statements are collected until `NEXT`; an optional following soft
///          identifier is consumed but is not matched against the opening
///          variable by this routine.
///
/// @return Newly allocated @ref ForStmt or @ref ForEachStmt describing the loop.
StmtPtr Parser::parseForStatement() {
    auto loc = peek().loc;
    consume(); // FOR

    // Check for FOR EACH syntax
    if (at(TokenKind::KeywordEach)) {
        consume(); // EACH
        auto stmt = std::make_unique<ForEachStmt>();
        stmt->loc = loc;

        // Parse element variable name
        Token elemTok = expectSoftIdentifier();
        stmt->elementVar = elemTok.lexeme;

        // Expect IN keyword
        expect(TokenKind::KeywordIn);

        // Parse array name
        Token arrayTok = expectSoftIdentifier();
        stmt->arrayName = arrayTok.lexeme;

        // Collect body until NEXT
        auto ctxFor = statementSequencer();
        ctxFor.collectStatements(TokenKind::KeywordNext, stmt->body);

        // Optionally consume variable name after NEXT
        if (isSoftIdentToken(peek().kind)) {
            consume();
        }
        return stmt;
    }

    // Standard FOR var = start TO end [STEP step] form
    auto stmt = std::make_unique<ForStmt>();
    stmt->loc = loc;

    stmt->varExpr = parseLetTarget();

    expect(TokenKind::Equal);
    stmt->start = parseExpression();
    expect(TokenKind::KeywordTo);
    stmt->end = parseExpression();
    if (at(TokenKind::KeywordStep)) {
        consume();
        stmt->step = parseExpression();
    }
    auto ctxFor = statementSequencer();
    ctxFor.collectStatements(TokenKind::KeywordNext, stmt->body);
    if (isSoftIdentToken(peek().kind)) {
        consume();
    }
    return stmt;
}

/// @brief Parse a standalone `NEXT` terminator.
///
/// @details Recognises the optional loop variable and records it for semantic
///          checks.  The node is primarily used during validation to ensure
///          `FOR` loops are properly nested.
///
/// @return Newly allocated @ref NextStmt capturing the terminator.
StmtPtr Parser::parseNextStatement() {
    auto loc = peek().loc;
    consume(); // NEXT
    std::string name;
    if (isSoftIdentToken(peek().kind)) {
        name = peek().lexeme;
        consume();
    }
    auto stmt = std::make_unique<NextStmt>();
    stmt->loc = loc;
    stmt->var = std::move(name);
    return stmt;
}

/// @brief Parse an `EXIT` statement for breaking out of loops.
///
/// @details Accepts `FOR`, `WHILE`, `DO`, `SUB`, or `FUNCTION` after EXIT.
///          When no recognized keyword follows, the node defaults to the WHILE
///          kind and leaves the unrecognized token untouched; this routine does
///          not emit a diagnostic for the omission.
///
/// @return Newly allocated @ref ExitStmt describing the exit semantics.
StmtPtr Parser::parseExitStatement() {
    auto loc = peek().loc;
    consume(); // EXIT

    ExitStmt::LoopKind kind = ExitStmt::LoopKind::While;
    if (at(TokenKind::KeywordFor)) {
        consume();
        kind = ExitStmt::LoopKind::For;
    } else if (at(TokenKind::KeywordWhile)) {
        consume();
        kind = ExitStmt::LoopKind::While;
    } else if (at(TokenKind::KeywordDo)) {
        consume();
        kind = ExitStmt::LoopKind::Do;
    } else if (at(TokenKind::KeywordSub)) {
        consume();
        kind = ExitStmt::LoopKind::Sub;
    } else if (at(TokenKind::KeywordFunction)) {
        consume();
        kind = ExitStmt::LoopKind::Function;
    }

    auto stmt = std::make_unique<ExitStmt>();
    stmt->loc = loc;
    stmt->kind = kind;
    return stmt;
}

} // namespace il::frontends::basic
