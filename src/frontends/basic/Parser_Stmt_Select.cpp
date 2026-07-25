//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser_Stmt_Select.cpp
// Purpose: Parse BASIC SELECT CASE statements and build structured AST nodes.
// Key invariants: CASE arms and CASE ELSE blocks are validated for correct
//                 ordering while tracking selector ranges for diagnostics.
// Ownership/Lifetime: AST nodes are produced as `std::unique_ptr` values that
//                     transfer ownership to the caller.
// Links: docs/internals/codemap.md, docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

/// @file Parser_Stmt_Select.cpp
/// @brief Implements SELECT CASE syntax parsing, lowering, and validation.
/// @details The parser first captures token-oriented CASE syntax, then converts
///          it into numeric, string, range, and relational AST labels before
///          building the normalized SelectModel. Inline and block bodies share
///          StatementSequencer-based collection.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/Parser_Stmt_ControlHelpers.hpp"
#include "frontends/basic/SelectModel.hpp"
#include "frontends/basic/constfold/Dispatch.hpp"
#include "zanna/il/IO.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace il::frontends::basic {
namespace {

/// @brief Parse a CASE numeric label token with full validation.
/// @details Requires every character in @p text to be consumed and rejects
///          overflow. Optional `%`/`&` suffixes, unary signs, and BASIC/modern
///          hexadecimal or binary prefixes are recognized before conversion.
/// @param text Complete CASE token spelling.
/// @return Parsed integer, or @c std::nullopt if the text is invalid.
std::optional<int64_t> parseCaseIntegerLiteral(std::string_view text) noexcept {
    if (!text.empty()) {
        char last = text.back();
        if (last == '%' || last == '&')
            text.remove_suffix(1);
    }
    int sign = 1;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        sign = text.front() == '-' ? -1 : 1;
        text.remove_prefix(1);
    }
    int base = 10;
    if (text.size() > 2 && text[0] == '&' && (text[1] == 'H' || text[1] == 'h')) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.size() > 2 && text[0] == '&' && (text[1] == 'B' || text[1] == 'b')) {
        base = 2;
        text.remove_prefix(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'X' || text[1] == 'x')) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'B' || text[1] == 'b')) {
        base = 2;
        text.remove_prefix(2);
    }
    if (text.empty())
        return std::nullopt;
    int64_t value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    auto result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    return sign < 0 ? -value : value;
}

} // namespace

/// @brief Parse the `SELECT CASE` header and initialise parser state.
///
/// @details Consumes the `SELECT CASE` keywords, parses the selector expression,
///          records the source range, and prepares a diagnostic callback that
///          forwards errors to the configured emitter.  The partially constructed
///          @ref SelectCaseStmt is stored in the returned state.
///
/// @return Parse state capturing selector information and diagnostic plumbing.
Parser::SelectParseState Parser::parseSelectHeader() {
    SelectParseState state;
    state.selectLoc = peek().loc;
    consume(); // SELECT
    expect(TokenKind::KeywordCase);
    auto selector = parseExpression();
    Token headerEnd = expect(TokenKind::EndOfLine);

    state.stmt = std::make_unique<SelectCaseStmt>();
    state.stmt->loc = state.selectLoc;
    state.stmt->selector = std::move(selector);
    state.stmt->range.begin = state.selectLoc;
    state.stmt->range.end = headerEnd.loc;

    /// Forward SELECT diagnostics to the emitter or a stderr fallback.
    state.diagnose = [&](il::support::SourceLoc diagLoc,
                         uint32_t length,
                         std::string_view message,
                         std::string_view code) {
        if (emitter_) {
            emitter_->emit(il::support::Severity::Error,
                           std::string(code),
                           diagLoc,
                           length,
                           std::string(message));
            return;
        }
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    };

    return state;
}

/// @brief Attempt to parse a `CASE ELSE` block for the current SELECT statement.
///
/// @details Delegates to @ref consumeCaseElse, updating parser bookkeeping about
///          whether an `ELSE` arm has been seen.  The helper returns whether the
///          directive was consumed so callers can adjust their parsing loop.
///
/// @param state Current SELECT parse state.
/// @return True when a `CASE ELSE` clause was handled.
bool Parser::parseSelectElse(SelectParseState &state) {
    auto result = consumeCaseElse(*state.stmt, state.sawCaseArm, state.sawCaseElse, state.diagnose);
    return result.handled;
}

/// @brief Handle directives that may terminate or continue SELECT parsing.
///
/// @details Checks for `END SELECT` and `CASE ELSE` directives before the parser
///          attempts to parse a normal `CASE` arm.  The return value informs the
///          caller whether parsing should terminate, skip to the next iteration,
///          or continue with arm parsing.
///
/// @param state Current SELECT parse state.
/// @return Dispatch action describing how the caller should proceed.
Parser::SelectDispatchAction Parser::dispatchSelectDirective(SelectParseState &state) {
    auto endResult =
        handleEndSelect(*state.stmt, state.sawCaseArm, state.expectEndSelect, state.diagnose);
    if (endResult.handled)
        return SelectDispatchAction::Terminate;

    if (parseSelectElse(state))
        return SelectDispatchAction::Continue;

    return SelectDispatchAction::None;
}

/// @brief Parse all CASE arms within a SELECT block.
///
/// @details Iterates until `END SELECT` or end-of-file, dispatching directives
///          and parsing each `CASE` arm encountered.  Diagnostic hooks are used
///          to report unexpected tokens or missing terminators.
///
/// @param state Current SELECT parse state being populated.
void Parser::parseSelectArms(SelectParseState &state) {
    while (!at(TokenKind::EndOfFile)) {
        while (at(TokenKind::EndOfLine))
            consume();

        if (at(TokenKind::EndOfFile))
            return;

        if (at(TokenKind::Number)) {
            TokenKind next = peek(1).kind;
            if (next == TokenKind::KeywordCase ||
                (next == TokenKind::KeywordEnd && peek(2).kind == TokenKind::KeywordSelect)) {
                Token lineTok = consume();
                if (auto parsed = parseLineNumberLiteral(lineTok.lexeme))
                    noteNumericLabelUsage(*parsed);
                else
                    emitError("B0001", lineTok, "line number is out of range");
            }
        }

        const auto action = dispatchSelectDirective(state);
        if (action == SelectDispatchAction::Terminate)
            return;
        if (action == SelectDispatchAction::Continue)
            continue;

        if (!at(TokenKind::KeywordCase)) {
            Token unexpected = consume();
            state.diagnose(unexpected.loc,
                           static_cast<uint32_t>(unexpected.lexeme.size()),
                           "expected CASE or END SELECT in SELECT CASE",
                           "B0001");
            continue;
        }

        Token caseTok = peek();
        CaseArm arm = parseCaseArm();
        arm.range.begin = caseTok.loc;
        state.stmt->arms.push_back(std::move(arm));
        state.stmt->range.end = state.stmt->arms.back().range.end;
        state.sawCaseArm = true;
    }
}

/// @brief Parse an entire `SELECT CASE` statement.
///
/// @details Invokes @ref parseSelectHeader, parses all arms, and emits
///          diagnostics when an `END SELECT` keyword is missing.  The populated
///          statement is then returned for lowering.
///
/// @return Completed @ref SelectCaseStmt node.
StmtPtr Parser::parseSelectCaseStatement() {
    auto state = parseSelectHeader();
    parseSelectArms(state);

    // Finalize the SELECT model and diagnose missing END SELECT in one place.
    /// Build the normalized model and report an unconsumed END SELECT requirement.
    auto finalizeSelectCase = [&](SelectParseState &st) -> void {
        SelectModelBuilder builder(st.diagnose);
        st.stmt->model = builder.build(*st.stmt);
        if (st.expectEndSelect) {
            st.diagnose(st.selectLoc,
                        static_cast<uint32_t>(6),
                        diag::ERR_SelectCase_MissingEndSelect.text,
                        diag::ERR_SelectCase_MissingEndSelect.id);
        }
    };

    finalizeSelectCase(state);
    return std::move(state.stmt);
}

/// @brief Collect the statements that form a CASE arm body.
///
/// @details Uses the statement sequencer to gather statements until another
///          `CASE` or `END SELECT` keyword is encountered.  The function records
///          the terminator token for diagnostics.
///
/// @return Body statements and metadata describing the terminator.
Parser::SelectBodyResult Parser::collectSelectBody() {
    SelectBodyResult result;
    auto bodyCtx = statementSequencer();
    /// Stop a CASE body before the next CASE or END SELECT directive.
    auto predicate = [&](int, il::support::SourceLoc) {
        if (at(TokenKind::KeywordCase))
            return true;
        if (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordSelect)
            return true;
        return false;
    };
    /// Preserve the location of the directive left for the outer parser.
    auto consumer = [&](int, il::support::SourceLoc, StatementSequencer::TerminatorInfo &info) {
        info.loc = peek().loc;
    };
    result.terminator = bodyCtx.collectStatements(predicate, consumer, result.body);
    return result;
}

/// @brief Collect colon-separated CASE body statements on the header line.
/// @details Parses statements until end-of-line or the next CASE/END SELECT
///          directive, preserving optional numeric-line metadata. The closing
///          end-of-line token is consumed and returned for range tracking.
/// @return Inline body statements and the consumed end-of-line token.
Parser::SelectInlineBodyResult Parser::collectInlineSelectBody() {
    SelectInlineBodyResult result;
    auto inlineCtx = statementSequencer();

    while (!at(TokenKind::EndOfFile)) {
        if (at(TokenKind::EndOfLine))
            break;

        if (at(TokenKind::KeywordCase) ||
            (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordSelect)) {
            break;
        }

        int line = 0;
        /// Capture an optional line number supplied by the sequencer.
        inlineCtx.withOptionalLineNumber(
            [&](int currentLine, il::support::SourceLoc) { line = currentLine; });

        if (!at(TokenKind::Colon)) {
            auto stmt = parseStatement(line);
            if (stmt) {
                stmt->line = line;
                result.body.push_back(std::move(stmt));
            }
        }

        if (at(TokenKind::Colon)) {
            consume();
            continue;
        }

        break;
    }

    Token eolTok = expect(TokenKind::EndOfLine);
    result.terminator = eolTok;
    return result;
}

/// @brief Handle the `END SELECT` directive when encountered.
///
/// @details Validates that at least one `CASE` arm was present, updates the
///          statement's source range, and clears the expectation that an `END`
///          still needs to appear.
///
/// @param stmt Statement being populated.
/// @param sawCaseArm Whether any CASE arms were parsed previously.
/// @param expectEndSelect Flag that tracks whether `END SELECT` remains required.
/// @param diagnose Diagnostic callback for reporting errors.
/// @return Result structure indicating whether the directive was handled.
Parser::SelectHandlerResult Parser::handleEndSelect(SelectCaseStmt &stmt,
                                                    bool sawCaseArm,
                                                    bool &expectEndSelect,
                                                    const SelectDiagnoseFn &diagnose) {
    SelectHandlerResult result;
    if (!(at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordSelect))
        return result;

    result.handled = true;
    consume();
    Token selectTok = expect(TokenKind::KeywordSelect);
    stmt.range.end = selectTok.loc;
    if (!sawCaseArm) {
        diagnose(selectTok.loc,
                 static_cast<uint32_t>(selectTok.lexeme.size()),
                 "SELECT CASE requires at least one CASE arm",
                 "B0001");
        result.emittedDiagnostic = true;
    }
    expectEndSelect = false;
    return result;
}

/// @brief Parse a `CASE ELSE` clause if present.
///
/// @details Verifies that the clause is not duplicated and that at least one
///          `CASE` arm preceded it.  The function collects the clause's body and
///          stores it on the statement when appropriate.
///
/// @param stmt Statement being populated.
/// @param sawCaseArm Whether any CASE arms have been parsed.
/// @param sawCaseElse Tracks whether a CASE ELSE was already encountered.
/// @param diagnose Diagnostic callback.
/// @return Result structure describing whether parsing succeeded and if a
///         diagnostic was emitted.
Parser::SelectHandlerResult Parser::consumeCaseElse(SelectCaseStmt &stmt,
                                                    bool sawCaseArm,
                                                    bool &sawCaseElse,
                                                    const SelectDiagnoseFn &diagnose) {
    SelectHandlerResult result;
    if (!at(TokenKind::KeywordCase) || peek(1).kind != TokenKind::KeywordElse)
        return result;

    result.handled = true;
    consume();
    const Token elseTok = expect(TokenKind::KeywordElse);

    if (sawCaseElse) {
        diagnose(elseTok.loc,
                 static_cast<uint32_t>(elseTok.lexeme.size()),
                 diag::ERR_SelectCase_DuplicateElse.text,
                 diag::ERR_SelectCase_DuplicateElse.id);
        result.emittedDiagnostic = true;
    }
    if (!sawCaseArm) {
        diagnose(elseTok.loc,
                 static_cast<uint32_t>(elseTok.lexeme.size()),
                 "CASE ELSE requires a preceding CASE arm",
                 "B0001");
        result.emittedDiagnostic = true;
    }

    std::vector<StmtPtr> inlineBody;
    Token elseTerminator;
    if (at(TokenKind::Colon)) {
        consume();
        auto inlineResult = collectInlineSelectBody();
        inlineBody = std::move(inlineResult.body);
        elseTerminator = inlineResult.terminator;
    } else {
        elseTerminator = expect(TokenKind::EndOfLine);
    }
    auto bodyResult = collectSelectBody();
    result.emittedDiagnostic = result.emittedDiagnostic || bodyResult.emittedDiagnostic;
    if (!sawCaseElse) {
        auto combinedBody = std::move(inlineBody);
        for (auto &stmtPtr : bodyResult.body) {
            combinedBody.push_back(std::move(stmtPtr));
        }
        stmt.elseBody = std::move(combinedBody);
        stmt.range.end = elseTerminator.loc;
    }
    sawCaseElse = true;
    return result;
}

/// @brief Parse the statements belonging to a `CASE ELSE` arm.
///
/// @details Consumes the `CASE ELSE` keywords, captures the end-of-line location
///          for range tracking, and then gathers the body statements until the
///          next directive terminates the block.
///
/// @return Pair of body statements and the location of the terminating EOL.
std::pair<std::vector<StmtPtr>, il::support::SourceLoc> Parser::parseCaseElseBody() {
    expect(TokenKind::KeywordCase);
    expect(TokenKind::KeywordElse);
    Token elseEol = expect(TokenKind::EndOfLine);

    auto bodyResult = collectSelectBody();
    auto body = std::move(bodyResult.body);

    return {std::move(body), elseEol.loc};
}

/// @brief Token-oriented intermediate storage for one CASE arm.
/// @details The cursor owns copied tokens until the syntax is lowered into a
///          CaseArm, allowing validation and diagnostics to retain source ranges.
struct Parser::Cursor {
    /// @brief Relational CASE operator, sign, and right-hand token.
    struct Relation {
        /// Parsed relational operator.
        CaseArm::CaseRel::Op op{CaseArm::CaseRel::Op::EQ};
        /// Unary sign applied to @ref valueTok.
        int sign = 1;
        /// Numeric right-hand-side token.
        Token valueTok;
    };

    /// Opening CASE keyword token.
    Token caseTok;
    /// Header terminator token used for source-range tracking.
    Token caseEol;
    /// Whether a colon introduced statements on the CASE header line.
    bool hasInlineBody = false;
    /// Literal or folded string label tokens.
    std::vector<Token> stringLabels;
    /// Individual numeric label tokens.
    std::vector<Token> numericLabels;
    /// Inclusive numeric range endpoint token pairs.
    std::vector<std::pair<Token, Token>> ranges;
    /// Relational label records.
    std::vector<Relation> relations;
};

/// @brief Successful syntax handle borrowing the caller-owned cursor.
struct Parser::CaseArmSyntax {
    /// Non-owning cursor populated by @ref parseCaseArmSyntax.
    Cursor *cursor = nullptr;
};

/// @brief Parse one CASE header into token-oriented intermediate storage.
/// @details Supports `CASE IS <op>`, numeric and string literals, numeric
///          ranges, tracked integer/string CONST names, and foldable CHR/CHR$
///          labels when enabled. A colon marks an inline body; otherwise the
///          header's end-of-line is consumed. Malformed labels emit diagnostics
///          and the successfully captured prefix is retained.
/// @param cursor Caller-owned storage cleared and populated by this invocation.
/// @return Syntax handle borrowing @p cursor.
/// @pre @p cursor outlives the returned handle and every use of that handle.
il::support::Expected<Parser::CaseArmSyntax> Parser::parseCaseArmSyntax(Cursor &cursor) {
    cursor.stringLabels.clear();
    cursor.numericLabels.clear();
    cursor.ranges.clear();
    cursor.relations.clear();
    cursor.hasInlineBody = false;

    cursor.caseTok = expect(TokenKind::KeywordCase);

    while (true) {
        if ((at(TokenKind::Identifier) && peek().lexeme == "IS") || at(TokenKind::KeywordIs)) {
            consume();
            Cursor::Relation rel;
            Token opTok = peek();
            bool validRelOp = true;
            switch (opTok.kind) {
                case TokenKind::Less:
                    rel.op = CaseArm::CaseRel::Op::LT;
                    break;
                case TokenKind::LessEqual:
                    rel.op = CaseArm::CaseRel::Op::LE;
                    break;
                case TokenKind::Equal:
                    rel.op = CaseArm::CaseRel::Op::EQ;
                    break;
                case TokenKind::GreaterEqual:
                    rel.op = CaseArm::CaseRel::Op::GE;
                    break;
                case TokenKind::Greater:
                    rel.op = CaseArm::CaseRel::Op::GT;
                    break;
                default: {
                    if (opTok.kind != TokenKind::EndOfLine) {
                        emitError("B0001", opTok, "CASE IS requires a relational operator");
                    }
                    validRelOp = false;
                    break;
                }
            }
            if (!validRelOp)
                break;

            consume();
            rel.sign = 1;
            if (at(TokenKind::Plus) || at(TokenKind::Minus)) {
                rel.sign = at(TokenKind::Minus) ? -1 : 1;
                consume();
            }

            if (!at(TokenKind::Number)) {
                Token bad = peek();
                if (bad.kind != TokenKind::EndOfLine) {
                    emitError("B0001", bad, "SELECT CASE labels must be integer literals");
                }
                break;
            }

            rel.valueTok = consume();
            cursor.relations.push_back(std::move(rel));
        } else if (at(TokenKind::String)) {
            cursor.stringLabels.push_back(consume());
        } else if (at(TokenKind::Identifier) && FrontendOptions::enableSelectCaseConstLabels()) {
            // Support CONST identifiers and CHR$() for labels.
            std::string ident = peek().lexeme;
            // Remove type suffix for canonicalization (e.g., CHR$ -> CHR)
            std::string identBase = ident;
            if (!identBase.empty() &&
                (identBase.back() == '$' || identBase.back() == '%' || identBase.back() == '#' ||
                 identBase.back() == '!' || identBase.back() == '&')) {
                identBase.pop_back();
            }
            std::string canon = CanonicalizeIdent(identBase);

            // Handle CHR / CHR$ builtin: fold to a string literal if possible.
            if (canon == "chr" && peek(1).kind == TokenKind::LParen) {
                // Parse the builtin expression and try to fold to a string literal.
                auto expr = parseExpression();
                if (expr) {
                    // Check if this is a BuiltinCallExpr for CHR/CHR$
                    if (auto *bce = as<BuiltinCallExpr>(*expr)) {
                        if (bce->builtin == BuiltinCallExpr::Builtin::Chr && !bce->args.empty()) {
                            // Try to fold CHR$(arg) to a string literal
                            if (auto folded = constfold::foldChrLiteral(*bce->args[0])) {
                                if (auto *se = as<StringExpr>(*folded)) {
                                    Token t;
                                    t.kind = TokenKind::String;
                                    t.loc = cursor.caseTok.loc;
                                    // Store raw string value without quotes (matching lexer
                                    // behavior)
                                    t.lexeme = se->value;
                                    cursor.stringLabels.push_back(std::move(t));
                                    continue;
                                }
                            }
                        }
                    }
                    emitError("B0001",
                              cursor.caseTok,
                              "SELECT CASE string label must be a literal or CHR$ constant");
                }
                continue;
            }

            // CONST integer/string lookup
            if (auto itS = knownConstStrs_.find(canon); itS != knownConstStrs_.end()) {
                Token labelTok = consume();
                Token t;
                t.kind = TokenKind::String;
                t.loc = labelTok.loc;
                // Store raw string value without quotes (matching lexer behavior)
                t.lexeme = itS->second;
                cursor.stringLabels.push_back(std::move(t));
                // BUG-CARDS-003 fix: Check for comma before continuing to allow multiple CONSTs
                if (at(TokenKind::Comma)) {
                    consume();
                    continue;
                }
                break;
            }
            if (auto itI = knownConstInts_.find(canon); itI != knownConstInts_.end()) {
                consume();
                // Support optional "TO <ident|number>" range after a CONST numeric.
                long long loVal = itI->second;
                if (at(TokenKind::KeywordTo)) {
                    consume();
                    // Optional sign
                    int hiSign = 1;
                    if (at(TokenKind::Minus) || at(TokenKind::Plus)) {
                        hiSign = at(TokenKind::Minus) ? -1 : 1;
                        consume();
                    }
                    long long hiVal = 0;
                    if (at(TokenKind::Number)) {
                        Token hiTok = consume();
                        if (auto parsed = parseCaseIntegerLiteral(hiTok.lexeme)) {
                            hiVal = *parsed;
                        } else {
                            emitError("B0001",
                                      hiTok,
                                      "SELECT CASE range end must be an integer literal or CONST");
                            break;
                        }
                    } else if (at(TokenKind::Identifier)) {
                        std::string hiName = CanonicalizeIdent(peek().lexeme);
                        if (auto itHi = knownConstInts_.find(hiName);
                            itHi != knownConstInts_.end()) {
                            hiVal = itHi->second;
                            consume();
                        } else {
                            emitError("B0001",
                                      peek(),
                                      "SELECT CASE range end must be an integer literal or CONST");
                            break;
                        }
                    } else {
                        emitError("B0001",
                                  peek(),
                                  "SELECT CASE range end must be an integer literal or CONST");
                        break;
                    }

                    Token loTok;
                    loTok.kind = TokenKind::Number;
                    loTok.loc = cursor.caseTok.loc;
                    loTok.lexeme = std::to_string(loVal);
                    Token hiTok;
                    hiTok.kind = TokenKind::Number;
                    hiTok.loc = cursor.caseTok.loc;
                    hiTok.lexeme = std::to_string(hiSign < 0 ? -hiVal : hiVal);
                    cursor.ranges.emplace_back(std::move(loTok), std::move(hiTok));
                } else {
                    Token t;
                    t.kind = TokenKind::Number;
                    t.loc = cursor.caseTok.loc;
                    t.lexeme = std::to_string(loVal);
                    cursor.numericLabels.push_back(std::move(t));
                }
                // BUG-CARDS-003 fix: Check for comma before continuing to allow multiple CONSTs
                if (at(TokenKind::Comma)) {
                    consume();
                    continue;
                }
                break;
            }

            // Unknown identifier in CASE label
            Token bad = peek();
            if (bad.kind != TokenKind::EndOfLine) {
                emitError("B0001", bad, "SELECT CASE labels must be literals or CONSTs");
            }
            break;
        } else if (at(TokenKind::Number) || at(TokenKind::Minus) || at(TokenKind::Plus)) {
            // Handle optional unary sign before number
            int sign = 1;
            if (at(TokenKind::Minus) || at(TokenKind::Plus)) {
                sign = at(TokenKind::Minus) ? -1 : 1;
                consume();
            }

            if (!at(TokenKind::Number)) {
                Token bad = peek();
                if (bad.kind != TokenKind::EndOfLine) {
                    emitError("B0001", bad, "SELECT CASE labels must be integer literals");
                }
                break;
            }

            Token loTok = consume();

            // Apply sign to the token's value for later processing
            if (sign == -1) {
                // Prepend minus to lexeme for correct parsing in lowerCaseArm
                loTok.lexeme = "-" + loTok.lexeme;
            }

            if (at(TokenKind::KeywordTo)) {
                consume();

                // Handle optional sign for high end of range
                int hiSign = 1;
                if (at(TokenKind::Minus) || at(TokenKind::Plus)) {
                    hiSign = at(TokenKind::Minus) ? -1 : 1;
                    consume();
                }

                if (!at(TokenKind::Number)) {
                    Token bad = peek();
                    if (bad.kind != TokenKind::EndOfLine) {
                        emitError("B0001", bad, "SELECT CASE labels must be integer literals");
                    }
                    break;
                }

                Token hiTok = consume();
                if (hiSign == -1) {
                    hiTok.lexeme = "-" + hiTok.lexeme;
                }
                cursor.ranges.emplace_back(std::move(loTok), std::move(hiTok));
            } else {
                cursor.numericLabels.push_back(std::move(loTok));
            }
        } else {
            Token bad = peek();
            if (bad.kind != TokenKind::EndOfLine) {
                emitError("B0001", bad, "SELECT CASE labels must be integer literals");
            }
            break;
        }

        if (at(TokenKind::Comma)) {
            consume();
            continue;
        }
        break;
    }

    if (at(TokenKind::Colon)) {
        cursor.caseEol = peek();
        cursor.hasInlineBody = true;
        consume();
    } else {
        cursor.caseEol = expect(TokenKind::EndOfLine);
    }

    CaseArmSyntax syntax;
    syntax.cursor = &cursor;
    return syntax;
}

/// @brief Convert captured CASE tokens into typed AST label collections.
/// @details Fully parses numeric labels and ranges, applies relation signs, and
///          decodes string token contents. Invalid entries are diagnosed and
///          omitted while other captured labels continue lowering.
/// @param syntax Syntax handle produced by @ref parseCaseArmSyntax.
/// @return Lowered CASE arm containing labels and header source range.
/// @pre @p syntax refers to a live non-null Cursor.
il::support::Expected<CaseArm> Parser::lowerCaseArm(const CaseArmSyntax &syntax) {
    const Cursor &cursor = *syntax.cursor;

    CaseArm arm;
    arm.range.begin = cursor.caseTok.loc;
    arm.range.end = cursor.caseEol.loc;
    arm.caseKeywordLength = static_cast<uint32_t>(cursor.caseTok.lexeme.size());

    for (const Token &labelTok : cursor.numericLabels) {
        if (auto value = parseCaseIntegerLiteral(labelTok.lexeme)) {
            arm.labels.push_back(*value);
        } else {
            emitError("B0001", labelTok, "SELECT CASE labels must be integer literals");
        }
    }

    for (const auto &rangeTok : cursor.ranges) {
        auto lo = parseCaseIntegerLiteral(rangeTok.first.lexeme);
        auto hi = parseCaseIntegerLiteral(rangeTok.second.lexeme);
        if (!lo)
            emitError(
                "B0001", rangeTok.first, "SELECT CASE range start must be an integer literal");
        if (!hi)
            emitError("B0001", rangeTok.second, "SELECT CASE range end must be an integer literal");
        if (lo && hi)
            arm.ranges.emplace_back(*lo, *hi);
    }

    for (const Cursor::Relation &relTok : cursor.relations) {
        if (auto value = parseCaseIntegerLiteral(relTok.valueTok.lexeme)) {
            CaseArm::CaseRel rel;
            rel.op = relTok.op;
            rel.rhs = static_cast<int64_t>(relTok.sign * *value);
            arm.rels.push_back(rel);
        } else {
            emitError("B0001", relTok.valueTok, "SELECT CASE labels must be integer literals");
        }
    }

    for (const Token &stringTok : cursor.stringLabels) {
        std::string decoded;
        std::string err;
        if (!il::io::decodeEscapedString(stringTok.lexeme, decoded, &err)) {
            emitError("B0003", stringTok, err);
            decoded = stringTok.lexeme;
        }
        arm.str_labels.push_back(std::move(decoded));
    }

    return arm;
}

/// @brief Require a CASE arm to contain at least one valid label.
/// @details Emits the catalogued empty-label diagnostic through the configured
///          emitter or stderr fallback and returns the same structured error.
/// @param arm Lowered CASE arm to inspect.
/// @return Success when any numeric, string, range, or relational label exists;
///         otherwise a structured empty-label error.
Parser::ErrorOr<void> Parser::validateCaseArm(const CaseArm &arm) {
    if (!arm.labels.empty() || !arm.str_labels.empty() || !arm.ranges.empty() ||
        !arm.rels.empty()) {
        return ErrorOr<void>{};
    }

    if (emitter_) {
        emitter_->emit(il::support::Severity::Error,
                       std::string(diag::ERR_Case_EmptyLabelList.id),
                       arm.range.begin,
                       arm.caseKeywordLength,
                       std::string(diag::ERR_Case_EmptyLabelList.text));
    } else {
        const std::string msg(diag::ERR_Case_EmptyLabelList.text);
        std::fprintf(stderr, "%s\n", msg.c_str());
    }

    return il::support::makeErrorWithCode(arm.range.begin,
                                          std::string(diag::ERR_Case_EmptyLabelList.id),
                                          std::string(diag::ERR_Case_EmptyLabelList.text));
}

/// @brief Parse a single `CASE` arm, including labels and body.
///
/// @details Handles relational forms (`CASE IS`), string literals, numeric
///          literals, and ranges while emitting diagnostics for malformed
///          entries. After successful non-empty-label validation, it merges an
///          optional inline body with the following block body. Syntax/lowering
///          failure synchronizes and returns an empty arm; validation failure
///          returns the partial arm without collecting its body.
///
/// @return Parsed case arm with labels, ranges, and body statements populated.
CaseArm Parser::parseCaseArm() {
    Cursor cursor;
    auto syntax = parseCaseArmSyntax(cursor);
    if (!syntax) {
        syncToStmtBoundary();
        return {};
    }

    std::vector<StmtPtr> inlineBody;
    if (cursor.hasInlineBody) {
        auto inlineResult = collectInlineSelectBody();
        inlineBody = std::move(inlineResult.body);
        cursor.caseEol = inlineResult.terminator;
    }

    auto lowered = lowerCaseArm(syntax.value());
    if (!lowered) {
        syncToStmtBoundary();
        return {};
    }

    CaseArm arm = std::move(lowered.value());
    if (!validateCaseArm(arm))
        return arm;

    auto bodyResult = collectSelectBody();
    if (!inlineBody.empty()) {
        for (auto &stmt : bodyResult.body) {
            inlineBody.push_back(std::move(stmt));
        }
        arm.body = std::move(inlineBody);
    } else {
        arm.body = std::move(bodyResult.body);
    }

    return arm;
}

} // namespace il::frontends::basic
