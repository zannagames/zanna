//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser_Stmt_Control.cpp
// Purpose: Registers the control-flow statement parsers used by the BASIC front
//          end. The parser owns a registry that maps keywords to member-function
//          callbacks; this file installs the callbacks that recognise branching
//          and looping constructs so the parser can remain data-driven instead of
//          hard-coding dispatch logic in a large conditional.
// Key invariants: Registration order mirrors keyword definitions to keep error
//                 messages deterministic.
// Ownership/Lifetime: Registry entries borrow parser member functions; no
//                     additional resources are owned.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file Parser_Stmt_Control.cpp
/// @brief Implements control-flow registration and TRY/CATCH parsing.
/// @details BASIC supports a diverse set of control-flow keywords.  Rather than
///          emit a switch statement for each token, the parser keeps a registry
///          that associates keywords with the member function responsible for
///          parsing that statement. This translation unit also owns the
///          multi-phase TRY/CATCH/FINALLY collector.

#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/Parser_Stmt_ControlHelpers.hpp"

namespace il::frontends::basic {

/// @brief Install the control-flow statement handlers required by BASIC.
///
/// @details The parser populates a `StatementParserRegistry` with mappings from
///          control-flow keywords to the member functions that understand their
///          syntax.  When the front end encounters a keyword token it consults
///          this registry to discover the appropriate parsing routine, keeping
///          dispatch table-driven and easy to extend.  Each registration stores a
///          pointer-to-member; ownership of the actual parsing implementations
///          remains with `Parser`.
///
/// @param registry Mutable registry that records keyword-to-parser mappings.
void Parser::registerControlFlowParsers(StatementParserRegistry &registry) {
    registry.registerHandler(TokenKind::KeywordIf, &Parser::parseIfStatement);
    registry.registerHandler(TokenKind::KeywordSelect, &Parser::parseSelectCaseStatement);
    registry.registerHandler(TokenKind::KeywordWhile, &Parser::parseWhileStatement);
    registry.registerHandler(TokenKind::KeywordDo, &Parser::parseDoStatement);
    registry.registerHandler(TokenKind::KeywordFor, &Parser::parseForStatement);
    registry.registerHandler(TokenKind::KeywordNext, &Parser::parseNextStatement);
    registry.registerHandler(TokenKind::KeywordExit, &Parser::parseExitStatement);
    registry.registerHandler(TokenKind::KeywordGoto, &Parser::parseGotoStatement);
    registry.registerHandler(TokenKind::KeywordGosub, &Parser::parseGosubStatement);
    registry.registerHandler(TokenKind::KeywordReturn, &Parser::parseReturnStatement);
    registry.registerHandler(TokenKind::KeywordTry, &Parser::parseTryCatchStatement);
}

/// @brief Parse a TRY statement with CATCH and/or FINALLY clauses.
/// @details Collects the TRY body until `CATCH`, `FINALLY`, or `END TRY`.
///          CATCH may bind one identifier, and FINALLY may follow either the
///          TRY body or a CATCH body. A TRY containing neither clause emits
///          `B3201`; a clause missing its closing `END TRY` emits `B3202`.
///          Diagnosed partial constructs still return their accumulated AST so
///          parsing can continue.
/// @return Owned TRY/CATCH node containing every body collected before recovery.
StmtPtr Parser::parseTryCatchStatement() {
    auto locTry = peek().loc;
    consume(); // TRY

    auto node = std::make_unique<TryCatchStmt>();
    node->loc = locTry;
    node->header.begin = locTry;

    // Collect TRY body until CATCH, FINALLY, or END TRY
    auto ctx = statementSequencer();
    /// Clause that terminated the initial TRY-body collection.
    enum class Term { None, Catch, Finally, EndTry } term = Term::None;
    auto termInfo = ctx.collectStatements(
        /// Stop the TRY body at the first clause or closing terminator.
        [&](int, il::support::SourceLoc) {
            if (at(TokenKind::KeywordCatch))
                return true;
            if (at(TokenKind::KeywordFinally))
                return true;
            if (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordTry)
                return true;
            return false;
        },
        /// Record and consume the clause that stopped TRY-body collection.
        [&](int, il::support::SourceLoc, StatementSequencer::TerminatorInfo &) {
            if (at(TokenKind::KeywordCatch)) {
                term = Term::Catch;
                node->header.end = peek().loc;
                consume(); // CATCH
            } else if (at(TokenKind::KeywordFinally)) {
                term = Term::Finally;
                node->header.end = peek().loc;
                consume(); // FINALLY
            } else if (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordTry) {
                term = Term::EndTry;
                node->header.end = peek().loc;
                consume(); // END
                consume(); // TRY
            }
        },
        node->tryBody);

    // Must have at least CATCH or FINALLY
    if (term == Term::EndTry || term == Term::None) {
        emitError("B3201", termInfo.loc, "TRY requires CATCH or FINALLY block");
        // We saw END TRY or EOF — return the node with just TRY body to keep progress.
        return node;
    }

    // If we have CATCH, parse it
    if (term == Term::Catch) {
        // Optional catch variable
        if (at(TokenKind::Identifier)) {
            node->catchVar = CanonicalizeIdent(peek().lexeme);
            consume();
        }

        // Optional line breaks before CATCH body
        ctx.skipLineBreaks();

        // Collect CATCH body until FINALLY or END TRY
        bool sawFinally = false;
        bool sawEndTry = false;
        ctx.collectStatements(
            /// Stop the CATCH body at FINALLY or END TRY.
            [&](int, il::support::SourceLoc) {
                if (at(TokenKind::KeywordFinally))
                    return true;
                if (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordTry)
                    return true;
                return false;
            },
            /// Record and consume the CATCH-body terminator.
            [&](int, il::support::SourceLoc, StatementSequencer::TerminatorInfo &) {
                if (at(TokenKind::KeywordFinally)) {
                    sawFinally = true;
                    consume(); // FINALLY
                } else if (at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordTry) {
                    sawEndTry = true;
                    consume(); // END
                    consume(); // TRY
                }
            },
            node->catchBody);

        if (sawFinally) {
            term = Term::Finally;
        } else if (sawEndTry) {
            // Done - CATCH only, no FINALLY
            return node;
        } else {
            emitError("B3202", locTry, "missing END TRY to terminate TRY/CATCH");
            return node;
        }
    }

    // If we have FINALLY (either directly after TRY or after CATCH), parse it
    if (term == Term::Finally) {
        // Optional line breaks before FINALLY body
        ctx.skipLineBreaks();

        // Collect FINALLY body until END TRY
        bool sawEndTry = false;
        ctx.collectStatements(
            /// Stop the FINALLY body only at END TRY.
            [&](int, il::support::SourceLoc) {
                return at(TokenKind::KeywordEnd) && peek(1).kind == TokenKind::KeywordTry;
            },
            /// Record and consume the closing END TRY pair.
            [&](int, il::support::SourceLoc, StatementSequencer::TerminatorInfo &) {
                sawEndTry = true;
                consume(); // END
                consume(); // TRY
            },
            node->finallyBody);

        if (!sawEndTry) {
            emitError("B3202", locTry, "missing END TRY to terminate TRY/FINALLY");
        }
    }

    return node;
}

} // namespace il::frontends::basic
