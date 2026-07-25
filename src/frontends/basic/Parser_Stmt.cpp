//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/Parser_Stmt.cpp
// Purpose: Implement statement-level parsing routines for the BASIC front end.
// Key invariants: Relies on the token buffer for deterministic lookahead and
//                 records discovered procedures so forward references can be
//                 recognised.
// Ownership/Lifetime: Parser owns tokens produced by the lexer and returns
//                     statements as std::unique_ptr nodes that the caller
//                     manages.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#statements
//
//===----------------------------------------------------------------------===//

/// @file Parser_Stmt.cpp
/// @brief Implements BASIC statement dispatch and shared body parsing.
/// @details This translation unit builds the core/OOP handler registry, applies
///          statement-form precedence, parses namespace and USING constructs,
///          manages recursion/nesting guards, and supplies shared FUNCTION body
///          and error-recovery helpers.

#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "zanna/diag/BasicDiag.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace il::frontends::basic {

/// @brief Register the minimal statement parsers required for core BASIC.
///
/// @details Installs handlers for assignment statements and procedure
///          declarations.  The registry keeps pointers-to-member functions so
///          dispatch remains table-driven.  Centralising the registration here
///          keeps @ref Parser::parseStatement focused on lookup rather than the
///          list of supported statements.
///
/// @param registry Dispatcher that maps statement-starting tokens to Parser
///        member functions.
void Parser::registerCoreParsers(StatementParserRegistry &registry) {
    registry.registerHandler(TokenKind::KeywordLet, &Parser::parseLetStatement);
    registry.registerHandler(TokenKind::KeywordConst, &Parser::parseConstStatement);
    registry.registerHandler(TokenKind::KeywordFunction, &Parser::parseFunctionStatement);
    registry.registerHandler(TokenKind::KeywordSub, &Parser::parseSubStatement);
    registry.registerHandler(TokenKind::KeywordExport, &Parser::parseExportStatement);
    registry.registerHandler(TokenKind::KeywordDeclare, &Parser::parseDeclareStatement);
    registry.registerHandler(TokenKind::KeywordNamespace, &Parser::parseNamespaceDecl);
    registry.registerHandler(TokenKind::KeywordUsing, &Parser::parseUsingDecl);
}

/// @brief Register object-oriented statement parsers.
///
/// @details Adds handlers for class/type declarations and delete statements so
///          the dispatcher recognizes CLASS, INTERFACE, TYPE, ENUM, and DELETE
///          introducers.
///
/// @param registry Dispatcher that records keyword-to-handler mappings.
void Parser::registerOopParsers(StatementParserRegistry &registry) {
    registry.registerHandler(TokenKind::KeywordClass, &Parser::parseClassDecl);
    registry.registerHandler(TokenKind::KeywordInterface, &Parser::parseInterfaceDecl);
    registry.registerHandler(TokenKind::KeywordType, &Parser::parseTypeDecl);
    registry.registerHandler(TokenKind::KeywordEnum, &Parser::parseEnumDecl);
    registry.registerHandler(TokenKind::KeywordDelete, &Parser::parseDeleteStatement);
}

/// @brief Parse a single BASIC statement based on the current token.
///
/// @details Statement dispatch follows a prioritized order:
///          1. Leading line number errors (early diagnostics)
///          2. Soft keywords (LINE INPUT - identifier-based, not a reserved keyword)
///          3. Registry lookup (all keyword-based statements: IF, SELECT, FOR, etc.)
///          4. Implicit LET (identifier = expression patterns)
///          5. Procedure calls (identifier-based invocations)
///          6. Unknown statement fallback with error recovery
///
///          The registry-first approach ensures keyword dispatch is table-driven
///          and new statements can be added by registering handlers without
///          modifying this function.
///
/// @param line One-based line number attached to the statement from the
///        original source listing.
/// @return Owned AST node on success; @c nullptr when recovery is required.
StmtPtr Parser::parseStatement(int line) {
    if (++stmtDepth_ > kMaxStmtDepth) {
        --stmtDepth_;
        emitError("B0001", peek(), "statement nesting too deep (limit: 512)");
        // Consume tokens to the next line boundary so the collectStatements
        // loop can make progress instead of spinning on the same token.
        while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine))
            consume();
        return nullptr;
    }

    /// @brief Restores the statement-recursion counter on every exit path.
    struct DepthGuard {
        /// Counter incremented by the current parseStatement invocation.
        unsigned &d;

        /// @brief Leave the current statement-recursion level.
        ~DepthGuard() {
            --d;
        }
    } stmtGuard_{stmtDepth_};

    // 1. Diagnose unexpected leading line numbers before statement content.
    if (auto handled = parseLeadingLineNumberError())
        return std::move(*handled);

    // 2. Soft keyword: LINE INPUT (identifier "LINE" followed by INPUT keyword).
    //    Must be checked before registry lookup since LINE is not a reserved keyword.
    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::KeywordInput) {
        if (string_utils::to_upper(peek().lexeme) == "LINE") {
            return parseLineInputStatement();
        }
    }

    // 3. Registry lookup: handles all keyword-based statements (IF, SELECT, FOR,
    //    WHILE, DO, LET, PRINT, DIM, SUB, FUNCTION, CLASS, etc.) via table-driven
    //    dispatch. This is the primary statement parsing path.
    if (auto stmt = parseRegisteredStatement(line))
        return std::move(*stmt);

    // 4. Implicit LET: identifier = expression (assignment without LET keyword).
    //    Must follow registry check since explicit LET is handled by the registry.
    if (auto stmt = parseImplicitLet())
        return std::move(*stmt);

    // 5. Procedure calls: identifier-based invocations (foo(), obj.method()).
    //    Requires lookahead to distinguish from variable references.
    if (auto stmt = parseCall(line))
        return std::move(*stmt);

    // 6. Unknown statement: emit diagnostic and synchronize to statement boundary.
    Token offendingTok = peek();
    reportUnknownStatement(offendingTok);
    resyncAfterError();
    while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine)) {
        consume();
    }
    return nullptr;
}

/// @brief Invoke the callback registered for the current statement token.
/// @details Soft identifier keywords followed by `=` or `(` are deliberately
///          left unmatched so implicit-assignment or call parsing can handle
///          them. Otherwise the no-line callback takes precedence over the
///          line-aware callback.
/// @param line Source line forwarded to a line-aware handler.
/// @return Disengaged when no callback should run; otherwise the handler's
///         statement pointer, which may be null after a diagnosed error.
Parser::StmtResult Parser::parseRegisteredStatement(int line) {
    const Token tok = peek(); // Copy to avoid reference invalidation

    // BUG-OOP-021: Soft keywords (COLOR, FLOOR, RANDOM, etc.) should be treated as
    // identifiers when followed by '=' (assignment) or '(' (array subscript/call).
    // This allows using these keywords as variable names: color = 5
    if (isSoftIdentToken(tok.kind) && tok.kind != TokenKind::Identifier) {
        TokenKind next = peek(1).kind;
        if (next == TokenKind::Equal || next == TokenKind::LParen) {
            // Treat as identifier, fall through to implicit LET or call parsing.
            return std::nullopt;
        }
    }

    const auto [noArg, withLine] = statementRegistry().lookup(tok.kind);
    if (noArg) {
        auto stmt = (this->*noArg)();
        return StmtResult(std::move(stmt));
    }
    if (withLine) {
        auto stmt = (this->*withLine)(line);
        return StmtResult(std::move(stmt));
    }
    return std::nullopt;
}

/// @brief Parse a NAMESPACE declaration with a dotted path and body.
/// @details Consumes the NAMESPACE header, parses a dotted identifier path
///          like `Foo.Bar.Baz`, tolerates an optional end-of-line, and then
///          collects nested statements until `END NAMESPACE`.
/// @return Newly allocated NamespaceDecl node.
StmtPtr Parser::parseNamespaceDecl() {
    auto loc = peek().loc;
    consume(); // NAMESPACE

    std::vector<std::string> path;
    // Require at least one identifier.
    Token first = expect(TokenKind::Identifier);
    if (first.kind == TokenKind::Identifier)
        path.push_back(first.lexeme);
    // Parse optional `.Ident` segments.
    while (at(TokenKind::Dot)) {
        consume();
        Token seg = expect(TokenKind::Identifier);
        if (seg.kind != TokenKind::Identifier)
            break;
        path.push_back(seg.lexeme);
    }

    if (at(TokenKind::EndOfLine))
        consume();

    auto decl = std::make_unique<NamespaceDecl>();
    decl->loc = loc;
    decl->path = std::move(path);
    if (!decl->path.empty()) {
        std::string nsHead = CanonicalizeIdent(decl->path.front());
        knownNamespaces_.insert(nsHead.empty() ? decl->path.front() : std::move(nsHead));
    }
    // Track namespace nesting to enforce USING-at-file-scope rule.
    ++nsDepth_;

    /// @brief Restores namespace nesting depth on every return path.
    struct NamespaceDepthGuard {
        /// Parser depth incremented for the current namespace.
        int &depth;

        /// @brief Restore parser namespace depth when namespace parsing exits.
        ~NamespaceDepthGuard() {
            --depth;
        }
    } guard{nsDepth_};

    parseProcedureBody(TokenKind::KeywordNamespace, decl->body);
    return decl;
}

/// @brief Parse a USING directive or resource statement.
/// @details Supports three forms:
///          - "USING Foo.Bar.Baz" (namespace import)
///          - "USING FB = Foo.Bar" (namespace alias)
///          - "USING x AS Type = expr ... END USING" (resource statement)
///          Recovers from malformed syntax by building a UsingDecl with empty
///          path so semantic analysis can emit precise diagnostics.
/// @return Newly allocated UsingDecl or UsingStmt node.
StmtPtr Parser::parseUsingDecl() {
    auto loc = peek().loc;
    consume(); // USING

    // Check for resource statement form: USING identifier AS ...
    // This is allowed inside procedures (unlike namespace USING).
    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::KeywordAs) {
        return parseUsingStatement(loc);
    }

    // Reject namespace USING inside procedures. Namespace-scoped USING is
    // validated by semantic analysis so runtime-namespace mode can allow it.
    if (procDepth_ > 0) {
        emitError("B0001", loc, "USING namespace imports are only allowed at file scope");
        // Attempt to recover by skipping to end of line.
        while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine))
            consume();
        if (at(TokenKind::EndOfLine))
            consume();
        return nullptr;
    }

    auto decl = std::make_unique<UsingDecl>();
    decl->loc = loc;

    // Check for alias form: "Identifier = ..."
    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Equal) {
        Token aliasTok = consume();
        decl->alias = aliasTok.lexeme;
        consume(); // =
    }

    // Parse dotted namespace path: Identifier ('.' Identifier)*
    if (at(TokenKind::Identifier)) {
        Token first = consume();
        decl->namespacePath.push_back(first.lexeme);

        while (at(TokenKind::Dot)) {
            consume(); // .
            if (at(TokenKind::Identifier)) {
                Token seg = consume();
                decl->namespacePath.push_back(seg.lexeme);
            } else {
                // Trailing dot or malformed path; stop and let semantics report error.
                break;
            }
        }
    }

    // Be permissive: ignore any trailing tokens until the current statement ends.
    while (!(at(TokenKind::EndOfLine) || at(TokenKind::EndOfFile) || at(TokenKind::Colon)))
        consume();

    return decl;
}

/// @brief Parse a USING resource statement (USING x AS Type = expr ... END USING).
/// @param loc Source location of the USING keyword.
/// @return Newly allocated UsingStmt node.
StmtPtr Parser::parseUsingStatement(il::support::SourceLoc loc) {
    auto stmt = std::make_unique<UsingStmt>();
    stmt->loc = loc;

    // Parse variable name
    if (!at(TokenKind::Identifier)) {
        emitError("B0002", loc, "expected variable name after USING");
        syncToStmtBoundary();
        return nullptr;
    }
    Token varTok = consume();
    stmt->varName = varTok.lexeme;

    // Consume AS
    if (!at(TokenKind::KeywordAs)) {
        emitError("B0002", loc, "expected AS after variable name in USING");
        syncToStmtBoundary();
        return nullptr;
    }
    consume(); // AS

    // Parse qualified type name: Identifier ('.' Identifier)*
    if (!at(TokenKind::Identifier)) {
        emitError("B0002", loc, "expected type name after AS in USING");
        syncToStmtBoundary();
        return nullptr;
    }
    Token typeTok = consume();
    stmt->typeQualified.push_back(typeTok.lexeme);

    while (at(TokenKind::Dot)) {
        consume(); // .
        if (at(TokenKind::Identifier)) {
            Token seg = consume();
            stmt->typeQualified.push_back(seg.lexeme);
        } else {
            emitError("B0002", loc, "expected identifier after '.' in type name");
            break;
        }
    }

    // Expect '=' and initializer expression
    if (!at(TokenKind::Equal)) {
        emitError("B0002", loc, "expected '=' after type in USING statement");
        syncToStmtBoundary();
        return nullptr;
    }
    consume(); // =

    // Parse initializer expression (typically NEW ClassName(...))
    stmt->initExpr = parseExpression();
    if (!stmt->initExpr) {
        emitError("B0002", loc, "expected initializer expression in USING statement");
        return nullptr;
    }

    // Parse body until END USING using the standard body parser
    parseProcedureBody(TokenKind::KeywordUsing, stmt->body);

    return stmt;
}

/// @brief Diagnose an unexpected numeric token at statement position.
/// @details Leaves non-number tokens unmatched. For a number, emits the
///          dedicated diagnostic, synchronizes to a statement boundary, and
///          returns an engaged result containing a null statement.
/// @return Disengaged when the current token is not a number; otherwise an
///         engaged null statement indicating handled recovery.
Parser::StmtResult Parser::parseLeadingLineNumberError() {
    if (!at(TokenKind::Number))
        return std::nullopt;

    reportUnexpectedLineNumber(peek());
    resyncAfterError();
    return StmtResult(StmtPtr{});
}

/// @brief Emit the catalogued unexpected-line-number diagnostic.
/// @details Uses the structured emitter when configured and otherwise prints a
///          fallback message to standard error.
/// @param tok Numeric token supplying spelling, location, and range length.
void Parser::reportUnexpectedLineNumber(const Token &tok) {
    auto diagId = diag::BasicDiag::UnexpectedLineNumber;
    if (emitter_) {
        emitter_->emit(diag::getSeverity(diagId),
                       std::string(diag::getCode(diagId)),
                       tok.loc,
                       static_cast<uint32_t>(tok.lexeme.size()),
                       diag::formatMessage(diagId,
                                           std::initializer_list<diag::Replacement>{
                                               diag::Replacement{"token", tok.lexeme}}));
    } else {
        std::fprintf(stderr, "unexpected line number '%s' before statement\n", tok.lexeme.c_str());
    }
}

/// @brief Emit the catalogued unknown-statement diagnostic.
/// @details Uses the structured emitter when configured and otherwise prints a
///          fallback message to standard error.
/// @param tok Unrecognized statement-leading token.
void Parser::reportUnknownStatement(const Token &tok) {
    auto diagId = diag::BasicDiag::UnknownStatement;
    if (emitter_) {
        emitter_->emit(diag::getSeverity(diagId),
                       std::string(diag::getCode(diagId)),
                       tok.loc,
                       static_cast<uint32_t>(tok.lexeme.size()),
                       diag::formatMessage(diagId,
                                           std::initializer_list<diag::Replacement>{
                                               diag::Replacement{"token", tok.lexeme}}));
    } else {
        std::fprintf(stderr,
                     "unknown statement '%s'; expected keyword or procedure call\n",
                     tok.lexeme.c_str());
    }
}

/// @brief Advance error recovery to the next statement boundary.
/// @details Delegates to the token helper shared by statement parsers.
void Parser::resyncAfterError() {
    syncToStmtBoundary();
}

/// @brief Check whether @p kind marks the beginning of a statement.
///
/// @details Some keywords are used both as statement leaders and as infix
///          operators.  The parser explicitly filters out logical operators and
///          recognises structural keywords such as THEN and ELSE so the caller
///          can treat them as implicit statement boundaries when splitting
///          colon-separated sequences.
///
/// @param kind Token to examine.
/// @return True when a handler or structural keyword introduces a new statement.
bool Parser::isStatementStart(TokenKind kind) const {
    switch (kind) {
        case TokenKind::KeywordAnd:
        case TokenKind::KeywordOr:
        case TokenKind::KeywordNot:
        case TokenKind::KeywordAndAlso:
        case TokenKind::KeywordOrElse:
            return false;
        case TokenKind::KeywordThen:
        case TokenKind::KeywordElse:
        case TokenKind::KeywordElseIf:
        case TokenKind::KeywordWend:
        case TokenKind::KeywordTo:
        case TokenKind::KeywordStep:
        case TokenKind::KeywordAs:
            return true;
        default:
            break;
    }
    return statementRegistry().contains(kind);
}

/// @brief Parse a FUNCTION header without consuming its body.
/// @details Captures the identifier, suffix-derived return type, parameter
///          list, and optional `AS` annotation. A recognized primitive stores
///          `explicitRetType`; otherwise an identifier path is canonicalized
///          into `explicitClassRetQname`.
/// @return Owned partial function declaration ready for body collection.
std::unique_ptr<FunctionDecl> Parser::parseFunctionHeader() {
    auto loc = peek().loc;
    consume(); // FUNCTION
    Token nameTok = expect(TokenKind::Identifier);
    auto fn = std::make_unique<FunctionDecl>();
    fn->loc = loc;
    fn->name = nameTok.lexeme;
    fn->ret = typeFromSuffix(nameTok.lexeme);
    fn->params = parseParamList();
    if (at(TokenKind::KeywordAs)) {
        consume();
        fn->explicitRetType = parseBasicType();
        if (fn->explicitRetType == BasicType::Unknown) {
            // Parse a qualified class name: Ident ('.' Ident)*
            if (at(TokenKind::Identifier)) {
                std::vector<std::string> segs;
                // consume first
                segs.push_back(CanonicalizeIdent(peek().lexeme));
                consume();
                while (at(TokenKind::Dot) && peek(1).kind == TokenKind::Identifier) {
                    consume(); // dot
                    segs.push_back(CanonicalizeIdent(peek().lexeme));
                    consume();
                }
                if (!segs.empty())
                    fn->explicitClassRetQname = std::move(segs);
            }
        }
    }
    return fn;
}

/// @brief Parse a sequence of statements terminated by `END <keyword>`.
///
/// @details Delegates to @ref StatementSequencer so nested statements can reuse
///          colon-separated parsing logic.  The callback consumes the terminating
///          `END` token followed by the expected keyword, leaving the parser
///          positioned after the body.  All parsed statements are appended to
///          @p body and the location of the `END` keyword is returned for later
///          diagnostics.
///
/// @param endKind Keyword expected after END to close the body.
/// @param body Destination vector for parsed statements.
/// @return Source location of the terminating END token, or an invalid location
///         when end of file is reached first.
il::support::SourceLoc Parser::parseProcedureBody(TokenKind endKind, std::vector<StmtPtr> &body) {
    auto ctx = statementSequencer();
    const bool isProcBody = (endKind != TokenKind::KeywordNamespace);
    if (isProcBody)
        ++procDepth_;

    /// @brief Restores procedure nesting depth when body collection exits.
    struct ProcedureDepthGuard {
        /// Parser procedure-depth counter.
        int &depth;
        /// Whether this body contributes a procedure nesting level.
        bool active;

        /// @brief Restore parser procedure depth when body parsing exits.
        ~ProcedureDepthGuard() {
            if (active)
                --depth;
        }
    } guard{procDepth_, isProcBody};

    /// Recognize the exact `END <endKind>` body terminator.
    auto info = ctx.collectStatements(
        [&](int, il::support::SourceLoc) {
            return at(TokenKind::KeywordEnd) && peek(1).kind == endKind;
        },
        /// Consume both tokens of the already-recognized terminator.
        [&](int, il::support::SourceLoc, StatementSequencer::TerminatorInfo &) {
            consume();
            consume();
        },
        body);
    if (!info.loc.isValid() && at(TokenKind::EndOfFile)) {
        std::string msg = "missing END ";
        msg += tokenKindToString(endKind);
        emitError("B0001", peek().loc, std::move(msg));
    }
    return info.loc;
}

/// @brief Parse statements comprising a function body.
///
/// @details Invokes @ref parseProcedureBody with the `FUNCTION` terminator so
///          the helper collects statements until `END FUNCTION` is encountered.
///          The resulting end location is stored on the declaration for
///          downstream diagnostics.
///
/// @param fn Function declaration to populate with body statements.
/// @pre @p fn is non-null.
void Parser::parseFunctionBody(FunctionDecl *fn) {
    fn->endLoc = parseProcedureBody(TokenKind::KeywordFunction, fn->body);
}


} // namespace il::frontends::basic
