//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Stmt.cpp
/// @brief Statement parsing implementation for the Zia parser.
///
/// @details Dispatches lexical blocks, bindings, conditionals, C-style and
///          iterable loops, returns, defers, guards, matches, exception
///          handling, jumps, throws, and expression statements. Ambiguous
///          `for` and match-arm shapes use bounded lookahead, while malformed
///          blocks resynchronize at statement boundaries.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
// Statement Parsing
//===----------------------------------------------------------------------===//

/// @brief Parse a statement, dispatching to the appropriate parser based on the leading token.
/// @details Handles blocks, var/final declarations, if, while, for, return, guard, match,
///          break, continue, print/println, and expression statements.
/// @return The parsed statement, or nullptr on error.
StmtPtr Parser::parseStatement() {
    if (++stmtDepth_ > kMaxStmtDepth) {
        --stmtDepth_;
        error("statement nesting too deep (limit: 512)");
        return nullptr;
    }

    SourceLoc loc = peek().loc;
    StmtPtr result;

    // Block
    if (check(TokenKind::LBrace)) {
        result = parseBlock();
        --stmtDepth_;
        return result;
    }

    // var/final/let variable declaration: var x = 5; final y: Integer = 10; let z = 3;
    if (check(TokenKind::KwVar) || check(TokenKind::KwFinal) || check(TokenKind::KwLet)) {
        result = parseVarDecl();
        --stmtDepth_;
        return result;
    }

    // If statement
    if (check(TokenKind::KwIf)) {
        result = parseIfStmt();
        --stmtDepth_;
        return result;
    }

    // While statement
    if (check(TokenKind::KwWhile)) {
        result = parseWhileStmt();
        --stmtDepth_;
        return result;
    }

    // For statement
    if (check(TokenKind::KwFor)) {
        result = parseForStmt();
        --stmtDepth_;
        return result;
    }

    // Return statement
    if (check(TokenKind::KwReturn)) {
        result = parseReturnStmt();
        --stmtDepth_;
        return result;
    }

    // Defer statement
    if (check(TokenKind::KwDefer)) {
        result = parseDeferStmt();
        --stmtDepth_;
        return result;
    }

    // Guard statement
    if (check(TokenKind::KwGuard)) {
        result = parseGuardStmt();
        --stmtDepth_;
        return result;
    }

    // Match statement (only when followed by a scrutinee, not when used as identifier)
    if (check(TokenKind::KwMatch)) {
        if (isMatchExpressionAhead()) {
            result = parseMatchStmt();
            --stmtDepth_;
            return result;
        }
        // else: fall through to expression statement parsing (e.g., match = 10;)
    }

    // Break
    if (match(TokenKind::KwBreak)) {
        if (!expect(TokenKind::Semicolon, ";")) {
            --stmtDepth_;
            return nullptr;
        }
        --stmtDepth_;
        return std::make_unique<BreakStmt>(loc);
    }

    // Continue
    if (match(TokenKind::KwContinue)) {
        if (!expect(TokenKind::Semicolon, ";")) {
            --stmtDepth_;
            return nullptr;
        }
        --stmtDepth_;
        return std::make_unique<ContinueStmt>(loc);
    }

    // Try/catch/finally
    if (check(TokenKind::KwTry)) {
        result = parseTryStmt();
        --stmtDepth_;
        return result;
    }

    // Throw statement
    if (check(TokenKind::KwThrow)) {
        result = parseThrowStmt();
        --stmtDepth_;
        return result;
    }

    // Expression statement
    ExprPtr expr = parseExpression();
    if (!expr) {
        --stmtDepth_;
        return nullptr;
    }

    if (!expect(TokenKind::Semicolon, ";")) {
        --stmtDepth_;
        return nullptr;
    }

    --stmtDepth_;
    return std::make_unique<ExprStmt>(loc, std::move(expr));
}

/// @brief Parse a braced statement block ({ stmt; stmt; ... }).
/// @details Includes error recovery: on parse failure, skips to the next semicolon or brace
///          to continue parsing subsequent statements.
/// @return The parsed BlockStmt, or nullptr on error.
StmtPtr Parser::parseBlock() {
    Token lbraceTok;
    if (!expect(TokenKind::LBrace, "{", &lbraceTok))
        return nullptr;
    SourceLoc loc = lbraceTok.loc;

    std::vector<StmtPtr> statements;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        // Check for declaration keywords that shouldn't appear inside a block.
        // If we see these, the block was likely not properly closed.
        // Note: 'value' is not included because it can be used as an identifier
        // (e.g., "Integer value = 0;"). We only check for unambiguous declaration starters.
        // 'func' is always a declaration keyword and cannot be used as an identifier.
        if (check(TokenKind::KwFunc) ||
            (check(TokenKind::KwExpose) && check(TokenKind::KwFunc, 1)) ||
            (check(TokenKind::KwHide) && check(TokenKind::KwFunc, 1)) ||
            check(TokenKind::KwClass) || check(TokenKind::KwInterface)) {
            error("unexpected declaration keyword in block - possible missing '}'");
            break;
        }

        StmtPtr stmt = parseStatement();
        if (!stmt) {
            resyncAfterError();
            continue;
        }
        statements.push_back(std::move(stmt));
    }

    Token rbraceTok;
    if (!expect(TokenKind::RBrace, "}", &rbraceTok))
        return nullptr;

    return std::make_unique<BlockStmt>(loc, rbraceTok.loc, std::move(statements));
}

/// @brief Parse a local variable declaration (var x: Type = expr; or final/let x = expr;).
/// @return The parsed VarStmt, or nullptr on error.
StmtPtr Parser::parseVarDecl() {
    Token kwTok = advance(); // consume var/final/let
    SourceLoc loc = kwTok.loc;
    bool isFinal = kwTok.kind == TokenKind::KwFinal || kwTok.kind == TokenKind::KwLet;

    if (match(TokenKind::LParen)) {
        std::vector<std::string> names;
        std::vector<TypePtr> types;

        if (check(TokenKind::RParen)) {
            error("tuple binding requires at least two variables");
            return nullptr;
        }

        while (true) {
            if (!checkIdentifierLike()) {
                error("expected variable name in tuple binding");
                return nullptr;
            }
            Token nameTok = advance();
            names.push_back(nameTok.text);

            TypePtr elemType;
            if (match(TokenKind::Colon)) {
                elemType = parseType();
                if (!elemType)
                    return nullptr;
            }
            types.push_back(std::move(elemType));

            if (!match(TokenKind::Comma))
                break;
            if (check(TokenKind::RParen))
                break;
        }

        if (names.size() < 2) {
            error("tuple binding requires at least two variables");
            return nullptr;
        }

        if (!expect(TokenKind::RParen, ")"))
            return nullptr;

        if (!expect(TokenKind::Equal, "'='"))
            return nullptr;

        ExprPtr init = parseExpressionAllowingStructLiterals();
        if (!init)
            return nullptr;

        if (!expect(TokenKind::Semicolon, ";"))
            return nullptr;

        return std::make_unique<VarStmt>(
            loc, std::move(names), std::move(types), std::move(init), isFinal);
    }

    if (!checkIdentifierLike()) {
        error("expected variable name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    // Optional type annotation
    TypePtr type;
    if (match(TokenKind::Colon)) {
        type = parseType();
        if (!type)
            return nullptr;
    }

    // Optional initializer — allow struct literals in this position
    ExprPtr init;
    if (match(TokenKind::Equal)) {
        init = parseExpressionAllowingStructLiterals();
        if (!init)
            return nullptr;
    }

    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return std::make_unique<VarStmt>(
        loc, std::move(name), std::move(type), std::move(init), isFinal);
}

/// @brief Parse an if statement with optional else clause (if cond { body } else { body }).
/// @return The parsed IfStmt, or nullptr on error.
StmtPtr Parser::parseIfStmt() {
    Token ifTok = advance(); // consume 'if'
    SourceLoc loc = ifTok.loc;

    // Zia uses "if condition {" without parentheses
    ExprPtr condition = parseExpression();
    if (!condition)
        return nullptr;

    StmtPtr thenBranch = parseStatement();
    if (!thenBranch)
        return nullptr;

    StmtPtr elseBranch;
    if (match(TokenKind::KwElse)) {
        elseBranch = parseStatement();
        if (!elseBranch)
            return nullptr;
    }

    return std::make_unique<IfStmt>(
        loc, std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

/// @brief Parse a while loop (while condition { body }).
/// @return The parsed WhileStmt, or nullptr on error.
StmtPtr Parser::parseWhileStmt() {
    Token whileTok = advance(); // consume 'while'
    SourceLoc loc = whileTok.loc;

    // Zia uses "while condition {" without parentheses
    ExprPtr condition = parseExpression();
    if (!condition)
        return nullptr;

    StmtPtr body = parseStatement();
    if (!body)
        return nullptr;

    return std::make_unique<WhileStmt>(loc, std::move(condition), std::move(body));
}

/// @brief Parse a for statement, supporting both C-style and for-in forms.
/// @details C-style: for (init; cond; update) { body }
///          For-in: for x in collection { body }
///          For-in tuple: for (k, v) in map { body }
/// @return The parsed ForStmt or ForInStmt, or nullptr on error.
StmtPtr Parser::parseForStmt() {
    Token forTok = advance(); // consume 'for'
    SourceLoc loc = forTok.loc;

    bool hasParen = match(TokenKind::LParen);

    /// @brief Distinguishes C-style `for` syntax from `for-in` by lookahead.
    /// @return `true` when a top-level semicolon appears before the loop body.
    auto isCStyleFor = [&]() -> bool {
        constexpr int kMaxForLookahead = 512;
        int parenDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        for (int i = 0; i < kMaxForLookahead; ++i) {
            TokenKind kind = peek(i).kind;
            if (kind == TokenKind::Eof)
                break;
            if (!hasParen && kind == TokenKind::LBrace)
                break;
            if (kind == TokenKind::LParen) {
                parenDepth++;
                continue;
            }
            if (kind == TokenKind::RParen) {
                if (parenDepth == 0)
                    break;
                parenDepth--;
                continue;
            }
            if (kind == TokenKind::LBracket) {
                bracketDepth++;
                continue;
            }
            if (kind == TokenKind::RBracket) {
                if (bracketDepth > 0)
                    bracketDepth--;
                continue;
            }
            if (kind == TokenKind::LBrace) {
                braceDepth++;
                continue;
            }
            if (kind == TokenKind::RBrace) {
                if (braceDepth > 0)
                    braceDepth--;
                continue;
            }
            if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                if (kind == TokenKind::Semicolon)
                    return true;
                if (kind == TokenKind::KwIn)
                    return false;
            }
        }
        return false;
    };

    if (isCStyleFor()) {
        if (!hasParen) {
            error("expected '(' in C-style for loop");
            return nullptr;
        }

        StmtPtr init;
        if (!check(TokenKind::Semicolon)) {
            if (check(TokenKind::KwVar) || check(TokenKind::KwFinal) || check(TokenKind::KwLet)) {
                init = parseVarDecl();
                if (!init)
                    return nullptr;
            } else {
                ExprPtr initExpr = parseExpression();
                if (!initExpr)
                    return nullptr;
                if (!expect(TokenKind::Semicolon, ";"))
                    return nullptr;
                init = std::make_unique<ExprStmt>(initExpr->loc, std::move(initExpr));
            }
        } else if (!expect(TokenKind::Semicolon, ";")) {
            return nullptr;
        }

        ExprPtr condition;
        if (!check(TokenKind::Semicolon)) {
            condition = parseExpression();
            if (!condition)
                return nullptr;
        }
        if (!expect(TokenKind::Semicolon, ";"))
            return nullptr;

        ExprPtr update;
        if (!check(TokenKind::RParen)) {
            update = parseExpression();
            if (!update)
                return nullptr;
        }

        if (!expect(TokenKind::RParen, ")"))
            return nullptr;

        StmtPtr body = parseStatement();
        if (!body)
            return nullptr;

        return std::make_unique<ForStmt>(
            loc, std::move(init), std::move(condition), std::move(update), std::move(body));
    }

    // Optional extra parentheses for tuple binding: for ((a, b) in ...)
    bool hasTupleParen = false;
    if (hasParen && check(TokenKind::LParen)) {
        hasTupleParen = true;
        advance();
    }

    if (!checkIdentifierLike()) {
        error("expected variable name in for loop");
        return nullptr;
    }

    Token varTok = advance();
    std::string varName = varTok.text;

    TypePtr varType;
    if (match(TokenKind::Colon)) {
        varType = parseType();
        if (!varType)
            return nullptr;
    }

    bool isTuple = false;
    std::string secondVar;
    TypePtr secondType;

    if (match(TokenKind::Comma)) {
        isTuple = true;
        if (!checkIdentifierLike()) {
            error("expected variable name in tuple binding");
            return nullptr;
        }
        Token secondTok = advance();
        secondVar = secondTok.text;

        if (match(TokenKind::Colon)) {
            secondType = parseType();
            if (!secondType)
                return nullptr;
        }
    }

    if (hasTupleParen) {
        if (!expect(TokenKind::RParen, ")"))
            return nullptr;
    }

    if (!expect(TokenKind::KwIn, "in"))
        return nullptr;

    ExprPtr iterable = parseExpression();
    if (!iterable)
        return nullptr;

    if (hasParen) {
        if (!expect(TokenKind::RParen, ")"))
            return nullptr;
    }

    StmtPtr body = parseStatement();
    if (!body)
        return nullptr;

    std::unique_ptr<ForInStmt> stmt;
    if (isTuple) {
        stmt = std::make_unique<ForInStmt>(
            loc, std::move(varName), std::move(secondVar), std::move(iterable), std::move(body));
        stmt->secondVariableType = std::move(secondType);
    } else {
        stmt = std::make_unique<ForInStmt>(
            loc, std::move(varName), std::move(iterable), std::move(body));
    }
    stmt->variableType = std::move(varType);
    return stmt;
}

/// @brief Parse a return statement (return [expr];).
/// @return The parsed ReturnStmt, or nullptr on error.
StmtPtr Parser::parseReturnStmt() {
    Token returnTok = advance(); // consume 'return'
    SourceLoc loc = returnTok.loc;

    ExprPtr value;
    if (!check(TokenKind::Semicolon)) {
        value = parseExpressionAllowingStructLiterals();
        if (!value)
            return nullptr;
    }

    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return std::make_unique<ReturnStmt>(loc, std::move(value));
}

/// @brief Parse a defer statement.
/// @details Supports `defer { ... }` and `defer expr;`.
/// @return The parsed DeferStmt, or nullptr on error.
StmtPtr Parser::parseDeferStmt() {
    Token deferTok = advance(); // consume 'defer'
    SourceLoc loc = deferTok.loc;

    if (check(TokenKind::LBrace)) {
        StmtPtr action = parseBlock();
        if (!action)
            return nullptr;
        return std::make_unique<DeferStmt>(loc, std::move(action));
    }

    ExprPtr expr = parseExpressionAllowingStructLiterals();
    if (!expr)
        return nullptr;

    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return std::make_unique<DeferStmt>(loc, std::make_unique<ExprStmt>(loc, std::move(expr)));
}

/// @brief Parse a guard statement (guard condition else { body }).
/// @details The else block must contain a control flow exit (return, break, continue).
/// @return The parsed GuardStmt, or nullptr on error.
StmtPtr Parser::parseGuardStmt() {
    Token guardTok = advance(); // consume 'guard'
    SourceLoc loc = guardTok.loc;

    // Parentheses are optional around the condition (Swift-style)
    bool hasParens = match(TokenKind::LParen);

    ExprPtr condition = parseExpression();
    if (!condition)
        return nullptr;

    if (hasParens && !expect(TokenKind::RParen, ")"))
        return nullptr;

    if (!expect(TokenKind::KwElse, "else"))
        return nullptr;

    StmtPtr elseBlock = parseStatement();
    if (!elseBlock)
        return nullptr;

    return std::make_unique<GuardStmt>(loc, std::move(condition), std::move(elseBlock));
}

/// @brief Parse a match statement (match expr { pattern => body; ... }).
/// @details Each arm consists of a pattern, optional guard (if condition), and a body
///          that is either a block or a single expression followed by a semicolon.
/// @return The parsed MatchStmt, or nullptr on error.
StmtPtr Parser::parseMatchStmt() {
    Token matchTok = advance(); // consume 'match'
    SourceLoc loc = matchTok.loc;

    // Parse the scrutinee expression
    ExprPtr scrutinee = parseExpression();
    if (!scrutinee)
        return nullptr;

    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    std::vector<MatchArm> arms;

    // Parse match arms
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        MatchArm arm;

        arm.pattern = parseMatchPattern();
        if (match(TokenKind::KwIf)) {
            arm.pattern.guard = parseExpression();
            if (!arm.pattern.guard)
                return nullptr;
        }

        // Expect =>
        if (!expect(TokenKind::FatArrow, "=>"))
            return nullptr;

        /// @brief Tests whether the next match arm body begins with a statement keyword.
        /// @return `true` when statement parsing should be selected.
        auto startsStatementArm = [&]() {
            switch (peek().kind) {
                case TokenKind::KwReturn:
                case TokenKind::KwBreak:
                case TokenKind::KwContinue:
                case TokenKind::KwDefer:
                case TokenKind::KwThrow:
                case TokenKind::KwVar:
                case TokenKind::KwFinal:
                case TokenKind::KwLet:
                case TokenKind::KwIf:
                case TokenKind::KwWhile:
                case TokenKind::KwFor:
                case TokenKind::KwGuard:
                case TokenKind::KwTry:
                case TokenKind::KwMatch:
                    return true;
                default:
                    return false;
            }
        };

        /// @brief Speculatively parses a delimiter-terminated match-arm expression.
        /// @return Parsed expression on committed success; otherwise null.
        auto tryParseExpressionArm = [&]() -> ExprPtr {
            Speculation speculation(*this);
            ExprPtr expr = parseExpressionAllowingStructLiterals();
            if (!expr)
                return nullptr;
            if (check(TokenKind::Semicolon) || check(TokenKind::Comma) ||
                check(TokenKind::RBrace)) {
                speculation.commit();
                if (!match(TokenKind::Semicolon))
                    match(TokenKind::Comma);
                return expr;
            }
            return nullptr;
        };

        // Parse arm body (statement, expression, or block)
        if (check(TokenKind::LBrace)) {
            arm.body = parseBlockExpression();
            if (!arm.body)
                return nullptr;
        } else if (check(TokenKind::KwIf) || check(TokenKind::KwMatch)) {
            arm.body = tryParseExpressionArm();
            if (!arm.body) {
                StmtPtr stmt = parseStatement();
                if (!stmt)
                    return nullptr;
                std::vector<StmtPtr> statements;
                statements.push_back(std::move(stmt));
                arm.body = std::make_unique<BlockExpr>(
                    arm.pattern.literal ? arm.pattern.literal->loc : loc,
                    std::move(statements),
                    nullptr);
            }
        } else if (startsStatementArm()) {
            StmtPtr stmt = parseStatement();
            if (!stmt)
                return nullptr;
            std::vector<StmtPtr> statements;
            statements.push_back(std::move(stmt));
            arm.body =
                std::make_unique<BlockExpr>(arm.pattern.literal ? arm.pattern.literal->loc : loc,
                                            std::move(statements),
                                            nullptr);
        } else {
            // Expression body
            arm.body = parseExpressionAllowingStructLiterals();
            if (!arm.body)
                return nullptr;

            while (match(TokenKind::Semicolon) || match(TokenKind::Comma)) {
            }
        }

        arms.push_back(std::move(arm));
    }

    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    return std::make_unique<MatchStmt>(loc, std::move(scrutinee), std::move(arms));
}

/// @brief Parse a try/catch/finally statement.
/// @details Syntax:
///   try { body }
///   [catch(varName) { catchBody }]*
///   [finally { finallyBody }]
/// At least one of catch or finally must be present.
/// @return The parsed TryStmt, or nullptr on error.
StmtPtr Parser::parseTryStmt() {
    Token tryTok = advance(); // consume 'try'
    SourceLoc loc = tryTok.loc;

    auto stmt = std::make_unique<TryStmt>(loc);

    // Parse try body
    stmt->tryBody = parseBlock();
    if (!stmt->tryBody)
        return nullptr;

    // Parse zero or more catch clauses
    while (check(TokenKind::KwCatch)) {
        Token catchTok = advance();
        TryStmt::CatchClause clause;
        clause.loc = catchTok.loc;

        // Optional catch variable: catch(e), catch(e: ErrorType), or catch
        if (match(TokenKind::LParen)) {
            if (checkIdentifierLike()) {
                clause.var = advance().text;

                // Optional typed catch: catch(e: ErrorType)
                if (match(TokenKind::Colon)) {
                    clause.typeName = parseQualifiedIdentifierString("error type name");
                    if (clause.typeName.empty())
                        return nullptr;
                }
            }
            if (!expect(TokenKind::RParen, ")"))
                return nullptr;
        }

        clause.body = parseBlock();
        if (!clause.body)
            return nullptr;

        if (stmt->catches.empty()) {
            stmt->catchVar = clause.var;
            stmt->catchTypeName = clause.typeName;
        }
        stmt->catches.push_back(std::move(clause));
    }

    // Parse optional finally clause
    if (match(TokenKind::KwFinally)) {
        stmt->finallyBody = parseBlock();
        if (!stmt->finallyBody)
            return nullptr;
    }

    // At least one of catch or finally must be present
    if (stmt->catches.empty() && !stmt->finallyBody) {
        error("try statement requires at least a catch or finally clause");
        return nullptr;
    }

    return stmt;
}

/// @brief Parse a throw statement: throw expr;
/// @return The parsed ThrowStmt, or nullptr on error.
StmtPtr Parser::parseThrowStmt() {
    Token throwTok = advance(); // consume 'throw'
    SourceLoc loc = throwTok.loc;

    ExprPtr value;
    if (!check(TokenKind::Semicolon)) {
        value = parseExpressionAllowingStructLiterals();
        if (!value)
            return nullptr;
    }

    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return std::make_unique<ThrowStmt>(loc, std::move(value));
}

} // namespace il::frontends::zia
