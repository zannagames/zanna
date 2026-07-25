//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/Parser_Stmt_Core.cpp
// Purpose: Implement parsing routines for core BASIC statements such as LET and procedure
// declarations. Key invariants: Maintains the parser's registry of known procedures so CALL
// statements without parentheses can still be
//                 resolved and ensures assignment targets honour BASIC's typing conventions.
// Ownership/Lifetime: Parser allocates AST nodes with std::unique_ptr and transfers ownership to
// the caller. Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#statements
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "frontends/basic/constfold/Dispatch.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

/// @file Parser_Stmt_Core.cpp
/// @brief Core BASIC statement parsing entry points.
/// @details Provides the shared helpers that recognise procedure declarations,
///          LET assignments, and CALL statements.  The routines maintain the
///          parser's registry of known procedures so ambiguity between
///          identifier expressions and procedure calls can be resolved without
///          backtracking.

namespace il::frontends::basic {

/// @brief Remember that a procedure declaration introduced @p name.
/// @details The parser keeps a set of procedure identifiers so later
///          parenthesis-free CALL statements can be interpreted correctly.  This
///          helper inserts the identifier into that set, guaranteeing idempotent
///          behaviour across multiple declarations.
/// @param name Procedure spelling; any BASIC type suffix is removed before canonicalization.
void Parser::noteProcedureName(std::string_view name) {
    std::string canon = CanonicalizeIdent(StripTypeSuffix(name));
    knownProcedures_.emplace(canon.empty() ? std::string(name) : std::move(canon));
}

/// @brief Query whether @p name is tracked as a known procedure.
/// @details Procedure references without parentheses rely on this lookup to
///          disambiguate between variable access and an implicit CALL.  The
///          suffix-stripped canonical key is probed in the unordered set.
/// @param name Identifier token spelling to probe.
/// @return True when the parser has previously recorded the name as a
///         procedure.
bool Parser::isKnownProcedureName(const std::string &name) const {
    std::string canon = CanonicalizeIdent(StripTypeSuffix(name));
    return knownProcedures_.find(canon.empty() ? name : canon) != knownProcedures_.end();
}

/// @brief Parse an assignment that omits the optional LET keyword.
/// @details First probes the token sequence without consuming it; when an
///          assignment is recognized, parses the lvalue, `=`, and initializer
///          into a LetStmt.
/// @return Disengaged when the token sequence is not an implicit assignment;
///         otherwise an engaged owned LET node.
Parser::StmtResult Parser::parseImplicitLet() {
    if (!isImplicitAssignmentStart())
        return std::nullopt;

    auto loc = peek().loc;
    auto target = parseLetTarget();
    expect(TokenKind::Equal);
    auto expr = parseExpression();

    auto stmt = std::make_unique<LetStmt>();
    stmt->loc = loc;
    stmt->target = std::move(target);
    stmt->expr = std::move(expr);
    return StmtResult(std::move(stmt));
}

/// @brief Probe whether the current tokens form an implicit assignment target.
/// @details Accepts a soft identifier or `ME`, walks parenthesized subscripts
///          and top-level dotted members, and succeeds only on a top-level `=`.
///          The bounded 512-token lookahead prevents malformed input from
///          growing the token buffer indefinitely.
/// @return `true` when a top-level assignment operator follows a supported lvalue shape.
bool Parser::isImplicitAssignmentStart() const {
    // BUG-OOP-021: Allow soft keywords (COLOR, FLOOR, etc.) as variable names.
    if (!isSoftIdentToken(peek().kind) && !at(TokenKind::KeywordMe))
        return false;

    int depth = 0;
    int offset = 1;
    /// Maximum number of buffered tokens examined by the non-consuming probe.
    constexpr int kMaxImplicitAssignmentProbe = 512;
    while (offset <= kMaxImplicitAssignmentProbe) {
        const Token &tok = peek(offset);
        if (tok.kind == TokenKind::Equal) {
            if (depth == 0)
                return true;
            ++offset;
            continue;
        }
        if (tok.kind == TokenKind::EndOfLine || tok.kind == TokenKind::EndOfFile ||
            tok.kind == TokenKind::Colon)
            return false;
        if (tok.kind == TokenKind::LParen) {
            ++depth;
            ++offset;
            continue;
        }
        if (tok.kind == TokenKind::RParen) {
            if (depth == 0)
                return false;
            --depth;
            ++offset;
            continue;
        }
        if (tok.kind == TokenKind::Dot && depth == 0) {
            ++offset;
            const Token &member = peek(offset);
            // BUG-CARDS-004 + A-071 fix: Accept all keywords as member names after dot
            if (!isMemberIdentToken(member.kind))
                return false;
            ++offset;
            continue;
        }
        if (depth > 0) {
            ++offset;
            continue;
        }
        return false;
    }
    return false;
}

/// @brief Parse a procedure or method call statement when possible.
/// @details BASIC allows both object method invocations (e.g. `obj.method()`)
///          and legacy procedure calls that omit parentheses.  The routine first
///          detects object-style calls by scanning ahead for `identifier.
///          identifier(`.  Failing that, it interprets `identifier(` as a normal
///          call expression or, when the name is known to refer to a procedure,
///          emits a diagnostic if the parentheses are missing.  Any malformed
///          sequence triggers an error report and synchronisation so parsing can
///          continue. The integer argument required by the dispatch-facing
///          signature is intentionally ignored.
/// @return Parsed call statement on success, an empty optional when no call is
///         present, or a null statement pointer when an error was reported.
Parser::StmtResult Parser::parseCall(int) {
    // Allow calls starting with an identifier or OOP receivers like
    // ME.Speak() or BASE.Speak().
    if (!at(TokenKind::Identifier) && !at(TokenKind::KeywordMe) && !at(TokenKind::KeywordBase))
        return std::nullopt;
    const Token identTok = peek();
    const Token nextTok = peek(1);
    if (nextTok.kind == TokenKind::Dot) {
        // Attempt to parse a namespace-qualified call pattern: Ident( . Ident )+ '('
        // Prefer this interpretation in statement position to surface clearer
        // procedure diagnostics, but avoid misclassifying instance calls like
        // `o.F()` by requiring either multiple qualification segments or that
        // the head identifier is a known namespace.
        if (at(TokenKind::Identifier)) {
            // Non-destructive probe for pattern Ident ('.' Ident)+ '('
            size_t i = 0;
            if (peek(i).kind == TokenKind::Identifier && peek(i + 1).kind == TokenKind::Dot) {
                // Advance through segments
                i += 2; // consumed first ident and dot conceptually
                bool ok = true;
                bool sawAdditionalDot = false;
                // BUG-OOP-040 fix: Use isMemberIdentToken() to allow keyword segments
                // in dotted namespaces like Zanna.Random.Seed() and Zanna.IO.File.Delete().
                while (isMemberIdentToken(peek(i).kind) && peek(i + 1).kind == TokenKind::Dot) {
                    sawAdditionalDot = true;
                    i += 2;
                }
                // Require final name segment (identifier or keyword) followed by '('.
                // BUG-OOP-040: Use isMemberIdentToken() to allow keywords in final position.
                if (!(isMemberIdentToken(peek(i).kind) && peek(i + 1).kind == TokenKind::LParen))
                    ok = false;

                if (ok) {
                    // BUG-082 fix: Only treat as qualified procedure call if the first identifier
                    // is a known namespace. Otherwise, let the expression parser handle it, which
                    // will correctly distinguish between namespace-qualified calls (Ns.Ns.Proc)
                    // and method calls on member access (obj.field.Method).
                    //
                    // This fixes cases like game.awayTeam.InitPlayer() which should be parsed
                    // as a MethodCallExpr on MemberAccessExpr, not as a qualified CallExpr.
                    bool treatAsQualified = false;
                    std::string headCanon = CanonicalizeIdent(identTok.lexeme);
                    if (knownNamespaces_.find(headCanon.empty() ? identTok.lexeme : headCanon) !=
                        knownNamespaces_.end()) {
                        treatAsQualified = true;
                    } else {
                        // When runtime namespaces are enabled, accept multi-segment
                        // dotted calls even if the head is not pre-registered as a
                        // namespace (e.g., Zanna.IO.File.*).
                        if (il::frontends::basic::FrontendOptions::enableRuntimeNamespaces()) {
                            if (sawAdditionalDot) {
                                treatAsQualified = true;
                            } else {
                                // Also accept explicit 'Zanna' regardless of registry seeding.
                                if (identTok.lexeme.size() == 5 || identTok.lexeme.size() == 6) {
                                    std::string head = identTok.lexeme;
                                    for (auto &c : head)
                                        c = static_cast<char>(
                                            std::tolower(static_cast<unsigned char>(c)));
                                    if (head == "zanna")
                                        treatAsQualified = true;
                                }
                            }
                        }
                    }

                    if (treatAsQualified) {
                        // Consume the qualified segments for real and parse arguments as a CallExpr
                        auto [segs, startLoc] = parseQualifiedIdentSegments();
                        expect(TokenKind::LParen);
                        std::vector<ExprPtr> args;
                        if (!at(TokenKind::RParen)) {
                            while (true) {
                                args.push_back(parseExpression());
                                if (!at(TokenKind::Comma))
                                    break;
                                consume();
                            }
                        }
                        expect(TokenKind::RParen);

                        auto call = std::make_unique<CallExpr>();
                        call->loc = startLoc;
                        if (segs.size() > 1)
                            call->calleeQualified = segs;
                        call->callee = JoinQualified(segs);
                        call->args = std::move(args);

                        auto stmt = std::make_unique<CallStmt>();
                        stmt->loc = identTok.loc;
                        stmt->call = std::move(call);
                        return StmtResult(std::move(stmt));
                    }
                }
            }
        }
        // Fallback: parse a general expression and accept MethodCallExpr or CallExpr
        auto expr = parseExpression(/*min_prec=*/0);
        if (expr && (is<MethodCallExpr>(*expr) || is<CallExpr>(*expr))) {
            auto stmt = std::make_unique<CallStmt>();
            stmt->loc = identTok.loc;
            stmt->call = std::move(expr);
            return StmtResult(std::move(stmt));
        }
        // Special-case: method SUB calls without parentheses (e.g., obj.Inc)
        if (expr && is<MemberAccessExpr>(*expr)) {
            // Only accept when end-of-statement follows
            const Token &after = peek();
            bool endOfStmt = after.kind == TokenKind::EndOfLine ||
                             after.kind == TokenKind::EndOfFile || after.kind == TokenKind::Colon ||
                             after.kind == TokenKind::Number;
            if (endOfStmt) {
                auto *ma = as<MemberAccessExpr>(*expr);
                // Synthesize a zero-arg MethodCallExpr
                auto call = std::make_unique<MethodCallExpr>();
                call->loc = ma->loc;
                call->Expr::loc = ma->loc;
                call->base = std::move(ma->base);
                call->method = ma->member;

                auto stmt = std::make_unique<CallStmt>();
                stmt->loc = identTok.loc;
                stmt->call = std::move(call);
                return StmtResult(std::move(stmt));
            }
        }
        reportUnknownStatement(identTok);
        resyncAfterError();
        return StmtResult(StmtPtr{});
    }
    if (nextTok.kind != TokenKind::LParen) {
        // Traditional BASIC allows procedure calls without parentheses
        // for zero-argument procedures. Only allow this when followed by
        // end-of-statement markers (EOL, EOF, :, or line number) and the
        // name is known as a procedure.
        bool isEndOfStmt = nextTok.kind == TokenKind::EndOfLine ||
                           nextTok.kind == TokenKind::EndOfFile ||
                           nextTok.kind == TokenKind::Colon || nextTok.kind == TokenKind::Number;

        if (isKnownProcedureName(identTok.lexeme)) {
            if (!isEndOfStmt) {
                // Not end-of-statement: this is likely an attempt to call with arguments
                // without parentheses - report error.
                reportMissingCallParenthesis(identTok, nextTok);
                resyncAfterError();
                return StmtResult(StmtPtr{});
            }

            consume(); // consume the identifier token
            auto call = std::make_unique<CallExpr>();
            call->loc = identTok.loc;
            call->Expr::loc = identTok.loc;
            call->callee = identTok.lexeme;
            auto stmt = std::make_unique<CallStmt>();
            stmt->loc = identTok.loc;
            stmt->call = std::move(call);
            return StmtResult(std::move(stmt));
        }
        return std::nullopt;
    }

    // Parse full expression to allow array-element method calls like arr(i).Init(...)
    auto expr = parseExpression(/*min_prec=*/0);
    if (expr && (is<CallExpr>(*expr) || is<MethodCallExpr>(*expr))) {
        auto stmt = std::make_unique<CallStmt>();
        stmt->loc = identTok.loc;
        stmt->call = std::move(expr);
        return StmtResult(std::move(stmt));
    }
    reportInvalidCallExpression(identTok);
    resyncAfterError();
    return StmtResult(StmtPtr{});
}

/// @brief Emit a diagnostic for procedure calls that omit parentheses.
/// @details When a known procedure name is followed by a non-`(` token the
///          parser expects the legacy CALL syntax and surfaces a diagnostic.
///          This helper routes the message either through the diagnostic
///          emitter or, in tooling-lite builds, directly to stderr.  The caret
///          is positioned at the unexpected token when available.
/// @param identTok Identifier naming the procedure.
/// @param nextTok  Token that violated the call syntax.
void Parser::reportMissingCallParenthesis(const Token &identTok, const Token &nextTok) {
    auto diagLoc = nextTok.loc.hasLine() ? nextTok.loc : identTok.loc;
    std::string message =
        "expected '(' after procedure name '" + identTok.lexeme + "' in procedure call statement";
    emitError("B0001", diagLoc, std::move(message));
}

/// @brief Emit a diagnostic for identifiers that fail to form a valid call.
/// @details If expression parsing fails to yield a @ref CallExpr or
///          @ref MethodCallExpr the parser reports an error explaining the
///          expected construct.  The diagnostic is routed through the configured
///          emitter when available, otherwise it falls back to printing the
///          message directly.
/// @param identTok Identifier token that initiated the failed parse.
void Parser::reportInvalidCallExpression(const Token &identTok) {
    std::string message = "expected procedure call after identifier '" + identTok.lexeme + "'";
    emitError("B0001", identTok, std::move(message));
}

/// @brief Parse a BASIC `LET` assignment statement.
/// @details Consumes the `LET` keyword, parses the left-hand side using
///          @ref parseLetTarget, and then expects an `=` followed by a general
///          expression.  The resulting @ref LetStmt adopts the source location
///          of the keyword so diagnostics can report accurate spans.
/// @return Newly constructed LET statement node.
StmtPtr Parser::parseLetStatement() {
    auto loc = peek().loc;
    consume();
    auto target = parseLetTarget();
    expect(TokenKind::Equal);
    auto e = parseExpression();
    auto stmt = std::make_unique<LetStmt>();
    stmt->loc = loc;
    stmt->target = std::move(target);
    stmt->expr = std::move(e);
    return stmt;
}

/// @brief Parse an assignment target including array and member postfixes.
/// @details Soft identifiers use array-or-variable disambiguation; other
///          primary forms are accepted for recovery. Dotted postfix operations
///          are then attached to the base expression.
/// @return Parsed lvalue-shaped expression, possibly partial after diagnostics.
ExprPtr Parser::parseLetTarget() {
    ExprPtr base;
    // BUG-OOP-021: Allow soft keywords (COLOR, FLOOR, etc.) as assignment targets.
    if (at(TokenKind::Identifier) ||
        (isSoftIdentToken(peek().kind) && peek().kind != TokenKind::Identifier)) {
        base = parseArrayOrVar();
    } else {
        base = parsePrimary();
    }
    return parsePostfix(std::move(base));
}

/// @brief Parse a BASIC `CONST` constant declaration statement.
/// @details Consumes the `CONST` keyword, parses an identifier, expects `=`,
///          and then parses an initializer expression. The type is inferred from
///          the identifier suffix or can be explicitly specified with AS.
///          Literal and foldable integer/string initializers are cached under a
///          canonical name for SELECT CASE parsing.
/// @return Newly constructed CONST node, or a dummy LET recovery node when the
///         required identifier is missing.
StmtPtr Parser::parseConstStatement() {
    auto loc = peek().loc;
    consume(); // CONST keyword

    if (!isSoftIdentToken(peek().kind)) {
        emitError("B0001", peek(), "expected identifier after CONST");
        resyncAfterError();
        return std::make_unique<LetStmt>(); // Return dummy statement
    }

    auto identTok = expectSoftIdentifier();
    std::string name = identTok.lexeme;

    Type type = typeFromSuffix(name);

    // Check for explicit type with AS keyword
    if (at(TokenKind::KeywordAs)) {
        consume();
        type = parseTypeKeyword();
    }

    expect(TokenKind::Equal);

    auto initializer = parseExpression();

    auto stmt = std::make_unique<ConstStmt>();
    stmt->loc = loc;
    stmt->name = std::move(name);
    stmt->type = type;
    stmt->initializer = std::move(initializer);

    // Track simple CONST values to enable constant labels in SELECT CASE.
    // Canonicalize identifier for case-insensitive lookup.
    {
        const std::string canon = CanonicalizeIdent(stmt->name);
        if (stmt->initializer) {
            // First check if the initializer is already a simple literal.
            if (auto *ie = as<IntExpr>(*stmt->initializer)) {
                knownConstInts_[canon] = ie->value;
            } else if (auto *se = as<StringExpr>(*stmt->initializer)) {
                knownConstStrs_[canon] = se->value;
            }
            // Try constant folding for binary expressions.
            else if (auto folded = constfold::fold_expr(*stmt->initializer)) {
                if (auto *ie2 = as<IntExpr>(**folded)) {
                    knownConstInts_[canon] = ie2->value;
                } else if (auto *se2 = as<StringExpr>(**folded)) {
                    knownConstStrs_[canon] = se2->value;
                }
            }
        }
    }
    return stmt;
}

/// @brief Derive the default BASIC type from an identifier suffix.
/// @details BASIC permits suffix characters (such as `$` or `%`) that encode a
///          variable's type.  This helper inspects the final character of the
///          identifier and maps it to the appropriate semantic type, falling
///          back to integer when no suffix is present.
/// @param name Identifier spelling to inspect.
/// @return Semantic type dictated by the suffix.
Type Parser::typeFromSuffix(std::string_view name) {
    if (!name.empty()) {
        char c = name.back();
        switch (c) {
            case '#':
            case '!':
                return Type::F64;
            case '$':
                return Type::Str;
            case '%':
            case '&':
                return Type::I64;
            default:
                break;
        }
    }
    return Type::I64;
}

/// @brief Parse a BASIC type keyword that follows an `AS` clause.
/// @details Recognises both reserved keywords (e.g. `BOOLEAN`) and legacy
///          identifiers such as `INTEGER` or `STRING`. An identifier token is
///          consumed even when its spelling is unrecognized.
/// @return Recognized semantic type, or I64 as the default for absent or
///         unrecognized type spellings.
Type Parser::parseTypeKeyword() {
    if (at(TokenKind::KeywordBoolean)) {
        consume();
        return Type::Bool;
    }
    if (at(TokenKind::Identifier)) {
        std::string name = peek().lexeme;
        consume();
        std::string upperName = string_utils::to_upper(name);
        if (upperName == "INTEGER" || upperName == "INT" || upperName == "LONG")
            return Type::I64;
        if (upperName == "DOUBLE" || upperName == "FLOAT")
            return Type::F64;
        if (upperName == "SINGLE")
            return Type::F64;
        if (upperName == "STRING")
            return Type::Str;
    }
    return Type::I64;
}

/// @brief Parse an optional parenthesised parameter list.
/// @details If the current token is an opening parenthesis the parser
///          repeatedly consumes optional BYREF/BYVAL modes, soft identifiers,
///          empty array markers, AS annotations, and commas until the closing
///          parenthesis. Primitive AS types override suffix inference; qualified
///          object types are canonicalized and stored in `objectClass`.
/// @return Sequence of parameter descriptors discovered in the token stream.
std::vector<Param> Parser::parseParamList() {
    std::vector<Param> params;
    if (!at(TokenKind::LParen))
        return params;
    consume();
    if (at(TokenKind::RParen)) {
        consume();
        return params;
    }
    while (true) {
        bool sawByRef = false;
        if (at(TokenKind::KeywordByRef)) {
            consume();
            sawByRef = true;
        } else if (at(TokenKind::KeywordByVal)) {
            consume();
            // BYVAL is the default, just consume and continue
        }

        Token id;
        id = expectSoftIdentifier();
        Param p;
        p.loc = id.loc;
        p.name = id.lexeme;
        p.type = typeFromSuffix(id.lexeme);
        p.isByRef = sawByRef;
        if (at(TokenKind::LParen)) {
            consume();
            expect(TokenKind::RParen);
            p.is_array = true;
        }
        if (at(TokenKind::KeywordAs)) {
            consume();
            // Support primitive types and qualified class names after AS
            if (at(TokenKind::Identifier)) {
                // Determine if this is a primitive keyword or a class name
                std::string first = peek().lexeme;
                std::string upper = string_utils::to_upper(first);
                const bool isPrimitive =
                    (upper == "INTEGER" || upper == "INT" || upper == "LONG" || upper == "DOUBLE" ||
                     upper == "FLOAT" || upper == "SINGLE" || upper == "STRING" ||
                     upper == "BOOLEAN");
                if (isPrimitive) {
                    p.type = parseTypeKeyword();
                } else {
                    // Parse qualified class name: Ident ('.' Ident)*
                    auto [segs, startLoc] = parseQualifiedIdentSegments();
                    (void)startLoc;
                    // Canonicalize segments; semantic analyzer validates existence
                    for (auto &seg : segs)
                        seg = CanonicalizeIdent(seg);
                    // Join dotted form into objectClass string (lower casing preserved by
                    // Canonicalize)
                    if (!segs.empty()) {
                        std::string cls;
                        for (size_t i = 0; i < segs.size(); ++i) {
                            if (i)
                                cls.push_back('.');
                            cls += segs[i];
                        }
                        p.objectClass = std::move(cls);
                        // Ensure IL param type becomes pointer later
                        p.type = Type::I64;
                    } else {
                        // Fallback: treat as primitive keyword path
                        p.type = parseTypeKeyword();
                    }
                }
            } else if (at(TokenKind::KeywordBoolean)) {
                p.type = parseTypeKeyword();
            } else {
                expect(TokenKind::Identifier);
            }
        }
        params.push_back(std::move(p));
        if (at(TokenKind::Comma)) {
            consume();
            continue;
        }
        break;
    }
    expect(TokenKind::RParen);
    return params;
}

/// @brief Parse a full BASIC `FUNCTION` declaration.
/// @details Delegates to @ref parseFunctionHeader to build the declaration
///          scaffold, infers the return type from either an explicit suffix or
///          the `AS` clause, records the procedure name for later disambiguation
///          of CALL statements, and finally parses the body until the matching
///          `END FUNCTION` terminator is reached. Array parameters are
///          temporarily registered for expression disambiguation and the
///          parser's previous array-name state is restored afterward.
/// @return Newly constructed function declaration statement.
StmtPtr Parser::parseFunctionStatement() {
    auto func = parseFunctionHeader();
    if (!func)
        return nullptr;
    if (func->explicitRetType != BasicType::Unknown) {
        switch (func->explicitRetType) {
            case BasicType::Int:
                func->ret = Type::I64;
                break;
            case BasicType::Float:
                func->ret = Type::F64;
                break;
            case BasicType::String:
                func->ret = Type::Str;
                break;
            case BasicType::Bool:
                func->ret = Type::Bool;
                break;
            case BasicType::Void:
                func->ret = Type::I64;
                break;
            case BasicType::Object:
                // Objects are represented as I64 (pointer-sized) at AST level
                func->ret = Type::I64;
                break;
            case BasicType::Unknown:
                break;
        }
    }
    noteProcedureName(func->name);

    // BUG-086 fix: Register array parameters so the parser can distinguish
    // arr(i) from proc(i) when parsing the procedure body.
    std::vector<std::string> arrayParams;
    std::vector<bool> hadOuterArray;
    for (const auto &param : func->params) {
        if (param.is_array) {
            hadOuterArray.push_back(arrays_.contains(param.name));
            arrays_.insert(param.name);
            arrayParams.push_back(param.name);
        }
    }

    parseProcedureBody(TokenKind::KeywordFunction, func->body);

    // BUG-086 fix: Remove array parameters from the global set after parsing
    // the procedure body to maintain proper scoping.
    for (size_t i = 0; i < arrayParams.size(); ++i) {
        if (!hadOuterArray[i])
            arrays_.erase(arrayParams[i]);
    }

    return func;
}

/// @brief Parse a complete BASIC `SUB` declaration.
/// @details Consumes the `SUB` keyword and identifier, parses the optional
///          parameter list, and rejects any stray `AS <type>` clause (which is
///          illegal for subroutines). Qualified declaration names split into a
///          namespace path and unqualified name. After recording the procedure
///          name, array-parameter classification is scoped around body parsing
///          through the closing `END SUB` token pair.
/// @return Newly constructed subroutine declaration statement.
StmtPtr Parser::parseSubStatement() {
    auto loc = peek().loc;
    consume();
    Token nameTok = expectSoftIdentifier();
    auto sub = std::make_unique<SubDecl>();
    sub->loc = loc;
    // Support qualified procedure names: Ident ('.' Ident)*
    std::vector<std::string> segs;
    if (isSoftIdentToken(nameTok.kind))
        segs.push_back(nameTok.lexeme);
    while (at(TokenKind::Dot) && isMemberIdentToken(peek(1).kind)) {
        consume(); // '.'
        Token seg = consume();
        segs.push_back(seg.lexeme);
    }
    if (segs.size() > 1) {
        sub->name = segs.back();
        sub->namespacePath.assign(segs.begin(), segs.end() - 1);
    } else {
        sub->name = nameTok.lexeme;
    }
    sub->params = parseParamList();
    if (at(TokenKind::KeywordAs)) {
        Token asTok = consume();
        if (!at(TokenKind::EndOfLine) && !at(TokenKind::EndOfFile))
            consume();
        emitError("B4007", asTok, "SUB cannot have 'AS <TYPE>'");
    }
    noteProcedureName(sub->name);

    // BUG-086 fix: Register array parameters so the parser can distinguish
    // arr(i) from proc(i) when parsing the procedure body.
    std::vector<std::string> arrayParams;
    std::vector<bool> hadOuterArray;
    for (const auto &param : sub->params) {
        if (param.is_array) {
            hadOuterArray.push_back(arrays_.contains(param.name));
            arrays_.insert(param.name);
            arrayParams.push_back(param.name);
        }
    }

    parseProcedureBody(TokenKind::KeywordSub, sub->body);

    // BUG-086 fix: Remove array parameters from the global set after parsing
    // the procedure body to maintain proper scoping.
    for (size_t i = 0; i < arrayParams.size(); ++i) {
        if (!hadOuterArray[i])
            arrays_.erase(arrayParams[i]);
    }

    return sub;
}

/// @brief Parse an EXPORT FUNCTION/SUB statement.
/// @details Consumes the EXPORT keyword and delegates to the standard
///          FUNCTION or SUB parser, marking the result as exported.
/// @return Function or sub declaration with export linkage.
StmtPtr Parser::parseExportStatement() {
    consume(); // consume EXPORT

    if (at(TokenKind::KeywordFunction)) {
        auto stmt = parseFunctionStatement();
        if (stmt) {
            auto *fn = static_cast<FunctionDecl *>(stmt.get());
            fn->isExport = true;
        }
        return stmt;
    }
    if (at(TokenKind::KeywordSub)) {
        auto stmt = parseSubStatement();
        if (stmt) {
            auto *sub = static_cast<SubDecl *>(stmt.get());
            sub->isExport = true;
        }
        return stmt;
    }

    emitError("B4999", peek(), "Expected FUNCTION or SUB after EXPORT");
    return nullptr;
}

/// @brief Parse a DECLARE FOREIGN FUNCTION/SUB statement (import, no body).
/// @details Consumes DECLARE FOREIGN, then parses the function/sub header
///          without a body. The resulting declaration has isForeign=true.
/// @return Function or sub declaration with import linkage.
StmtPtr Parser::parseDeclareStatement() {
    consume(); // consume DECLARE

    if (!at(TokenKind::KeywordForeign)) {
        emitError("B4999", peek(), "Expected FOREIGN after DECLARE");
        return nullptr;
    }
    consume(); // consume FOREIGN

    if (at(TokenKind::KeywordFunction)) {
        auto fn = parseFunctionHeader();
        if (fn) {
            fn->isForeign = true;
            // Apply explicit return type (same logic as parseFunctionStatement).
            if (fn->explicitRetType != BasicType::Unknown) {
                switch (fn->explicitRetType) {
                    case BasicType::Int:
                        fn->ret = Type::I64;
                        break;
                    case BasicType::Float:
                        fn->ret = Type::F64;
                        break;
                    case BasicType::String:
                        fn->ret = Type::Str;
                        break;
                    case BasicType::Bool:
                        fn->ret = Type::Bool;
                        break;
                    default:
                        fn->ret = Type::I64;
                        break;
                }
            }
        }
        return fn;
    }
    if (at(TokenKind::KeywordSub)) {
        auto loc = peek().loc;
        consume(); // consume SUB
        Token nameTok = expectSoftIdentifier();
        auto sub = std::make_unique<SubDecl>();
        sub->loc = loc;
        sub->name = nameTok.lexeme;
        sub->params = parseParamList();
        sub->isForeign = true;
        return sub;
    }

    emitError("B4999", peek(), "Expected FUNCTION or SUB after DECLARE FOREIGN");
    return nullptr;
}

} // namespace il::frontends::basic
