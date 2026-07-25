//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file Parser_Stmt_ControlHelpers.hpp
/// @brief Shared helper utilities for BASIC control-flow parsing.
/// @details Defines small inline helpers used by the parser to handle control
///          flow constructs (IF/ELSEIF, SELECT CASE, etc.). The helpers maintain
///          consistency of the statement sequencer state, normalize optional
///          line labels between branches, and build statement lists with correct
///          source locations.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Parser.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

namespace il::frontends::basic {

/// @brief Consume an optional line label after a line break.
/// @details If a line break is present, advances the sequencer past it and then
///          checks for an optional numeric label or named label (identifier +
///          colon). Labels are only consumed when the following token matches
///          one of the @p followerKinds, preserving labels that belong to other
///          constructs. Any consumed label is recorded for later diagnostics.
/// @param ctx Statement sequencer to advance past line breaks.
/// @param followerKinds Token kinds that may legally follow the optional label.
inline void Parser::skipOptionalLineLabelAfterBreak(
    StatementSequencer &ctx, std::initializer_list<TokenKind> followerKinds) {
    if (!at(TokenKind::EndOfLine))
        return;

    ctx.skipLineBreaks();

    /// Test whether the token after a candidate label permits consuming it.
    auto matchesFollowers = [&](TokenKind candidate) {
        if (followerKinds.size() == 0)
            return true;
        for (auto follower : followerKinds) {
            if (candidate == follower)
                return true;
        }
        return false;
    };

    if (at(TokenKind::Number)) {
        Token numberTok = peek();
        if (matchesFollowers(peek(1).kind)) {
            if (auto parsed = parseLineNumberLiteral(numberTok.lexeme))
                noteNumericLabelUsage(*parsed);
            else
                emitError("B0001", numberTok, "line number is out of range");
            consume();
        }
        return;
    }

    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
        Token labelTok = peek();
        TokenKind afterColon = peek(2).kind;
        if (matchesFollowers(afterColon)) {
            consume();
            consume();
            int labelNumber = ensureLabelNumber(labelTok.lexeme);
            noteNamedLabelDefinition(labelTok, labelNumber);
        }
    }
}

/// @brief Parse a single IF/ELSEIF branch body statement.
/// @details Skips optional labels after a line break, parses the following
///          statement, and normalizes the statement's line metadata to the
///          branch header line for consistency.
/// @param line Source line number associated with the IF/ELSEIF header.
/// @param ctx Statement sequencer used for line-break handling.
/// @return Parsed statement node, or null when no statement is present.
inline StmtPtr Parser::parseIfBranchBody(int line, StatementSequencer &ctx) {
    skipOptionalLineLabelAfterBreak(ctx);
    auto stmt = parseStatement(line);
    if (stmt)
        stmt->line = line;
    return stmt;
}

namespace parser_helpers {

/// @brief Build a StmtList node from a sequence of branch statements.
/// @details Returns null for empty branches; otherwise constructs a StmtList,
///          assigns its line, and chooses a source location based on the first
///          non-null statement (falling back to @p defaultLoc when needed).
/// @param line Line number used for the list node.
/// @param defaultLoc Fallback source location when no child has one.
/// @param stmts Statement nodes collected for the branch.
/// @return StmtList node or null if the branch is empty.
inline StmtPtr buildBranchList(int line,
                               il::support::SourceLoc defaultLoc,
                               std::vector<StmtPtr> &&stmts) {
    if (stmts.empty())
        return nullptr;
    auto list = std::make_unique<StmtList>();
    list->line = line;
    il::support::SourceLoc listLoc = defaultLoc;
    for (const auto &bodyStmt : stmts) {
        if (bodyStmt) {
            listLoc = bodyStmt->loc;
            break;
        }
    }
    list->loc = listLoc;
    list->stmts = std::move(stmts);
    return list;
}

/// @brief Collect branch statements using a terminator predicate.
/// @details Delegates to @ref StatementSequencer::collectStatements to gather
///          statements until a terminator is encountered, then returns the
///          collected vector for further processing by the parser.
/// @param ctx Statement sequencer driving statement collection.
/// @param predicate Terminator predicate that decides when to stop.
/// @param consumer Callback invoked when a terminator is consumed.
/// @return Vector of collected statements for the branch.
inline std::vector<StmtPtr> collectBranchStatements(
    StatementSequencer &ctx,
    const StatementSequencer::TerminatorPredicate &predicate,
    const StatementSequencer::TerminatorConsumer &consumer) {
    std::vector<StmtPtr> stmts;
    ctx.collectStatements(predicate, consumer, stmts);
    return stmts;
}

} // namespace parser_helpers

} // namespace il::frontends::basic
