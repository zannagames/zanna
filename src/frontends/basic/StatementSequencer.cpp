//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/StatementSequencer.cpp
// Purpose: Implements BASIC statement-list parsing across line breaks, colons,
//          numeric or named labels, included files, and caller-defined block
//          terminators.
// Key invariants:
//   - The most recent consumed separator is classified consistently, with any
//     line break taking precedence over colons in the same skipped run.
//   - A stashed line label is delivered once, then cleared or restashed only
//     when collection must stop before its statement.
//   - Terminator predicates observe line context before the statement parser
//     consumes the pending construct.
// Ownership/Lifetime:
//   - StatementSequencer borrows its Parser and transfers parsed AST nodes into
//     caller-owned vectors or returned smart pointers.
// Links: src/frontends/basic/StatementSequencer.hpp,
//        src/frontends/basic/Parser.hpp,
//        src/frontends/basic/LineUtils.hpp,
//        src/frontends/basic/AST.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/StatementSequencer.hpp"
#include "frontends/basic/AST.hpp"
#include "frontends/basic/LineUtils.hpp"

#include "frontends/basic/Parser.hpp"

/// @file
/// @brief Statement sequencing helper shared by BASIC parser productions.
/// @details Normalises the handling of colon/newline separators and optional
///          line labels so grammar productions can focus solely on recognising
///          statement shapes.  The sequencer also exposes utilities for parsing
///          lists of statements that terminate on custom predicates.

namespace il::frontends::basic {
/// @brief Construct a sequencer bound to a parser instance.
/// @details Stores a reference to the owning parser so token queries and
///          sub-parses can be delegated while this helper focuses on separator
///          bookkeeping. Pending line-label state starts empty.
/// @param parser Parser borrowed for the sequencer's complete lifetime.
StatementSequencer::StatementSequencer(Parser &parser) : parser_(parser) {}

/// @brief Consume any leading statement separators before parsing begins.
/// @details Eats an arbitrary number of colon or end-of-line tokens and updates
///          the cached separator kind to reflect the most recent token
///          consumed. Callers can then determine whether the next statement was
///          preceded by a newline or a colon separator.
/// @post @ref lastSeparator_ is @ref SeparatorKind::LineBreak when any newline
///       was consumed, otherwise @ref SeparatorKind::Colon when any colon was
///       consumed, and @ref SeparatorKind::None when no separator was present.
void StatementSequencer::skipLeadingSeparator() {
    bool consumedColon = false;
    bool consumedLineBreak = false;

    while (parser_.at(TokenKind::Colon) || parser_.at(TokenKind::EndOfLine)) {
        if (parser_.at(TokenKind::Colon)) {
            parser_.consume();
            consumedColon = true;
        } else {
            parser_.consume();
            consumedLineBreak = true;
        }
    }

    if (consumedLineBreak) {
        lastSeparator_ = SeparatorKind::LineBreak;
    } else if (consumedColon) {
        lastSeparator_ = SeparatorKind::Colon;
    } else {
        lastSeparator_ = SeparatorKind::None;
    }
}

/// @brief Consume consecutive end-of-line tokens.
/// @details Used when parsing constructs that should coalesce blank lines. The
///          routine records that the last separator encountered was a line break
///          whenever any tokens are consumed.
/// @return @c true when at least one end-of-line token was removed.
/// @post @ref lastSeparator_ changes only when a token was consumed.
bool StatementSequencer::skipLineBreaks() {
    bool consumed = false;
    while (parser_.at(TokenKind::EndOfLine)) {
        parser_.consume();
        consumed = true;
    }
    if (consumed)
        lastSeparator_ = SeparatorKind::LineBreak;
    return consumed;
}

/// @brief Consume colon or newline separators following a statement.
/// @details Mirrors @ref skipLeadingSeparator but is intended for use once a
///          statement has already been parsed.  The cached separator kind is
///          updated so later logic knows how the last pair of statements were
///          separated.
/// @post @ref lastSeparator_ follows the same precedence rules as
///       @ref skipLeadingSeparator.
void StatementSequencer::skipStatementSeparator() {
    bool consumedColon = false;
    bool consumedLineBreak = false;

    while (parser_.at(TokenKind::Colon) || parser_.at(TokenKind::EndOfLine)) {
        if (parser_.at(TokenKind::Colon)) {
            parser_.consume();
            consumedColon = true;
        } else {
            parser_.consume();
            consumedLineBreak = true;
        }
    }

    if (consumedLineBreak) {
        lastSeparator_ = SeparatorKind::LineBreak;
    } else if (consumedColon) {
        lastSeparator_ = SeparatorKind::Colon;
    } else {
        lastSeparator_ = SeparatorKind::None;
    }
}

/// @brief Execute a callback with the current line-number context.
/// @details BASIC permits optional numeric labels ahead of statements.  This
///          helper checks for such a label—either pending from a previous
///          iteration, an allowed identifier followed by a colon, or a numeric
///          token—and passes the resolved label and location to @p fn. Named
///          labels are mapped through the parser's numeric label registry.
///          When a pending label is replayed while another number is already
///          next, @ref deferredLineOnly_ tells collection to preserve that next
///          label for a later logical line.
/// @param fn Callback invoked with the discovered line number (or zero when
///           absent) and its source location.
/// @param allowIdentifierLabel Whether an identifier-colon pair may be consumed
///                             as a named label in this context.
/// @post @p fn is invoked exactly once.
/// @post A directly consumed numeric label is registered with the parser when
///       it is valid and nonzero; an out-of-range spelling emits B0001.
void StatementSequencer::withOptionalLineNumber(
    const std::function<void(int, il::support::SourceLoc)> &fn, bool allowIdentifierLabel) {
    int line = 0;
    il::support::SourceLoc loc{};
    deferredLineOnly_ = false;
    if (pendingLine_ >= 0) {
        line = pendingLine_;
        loc = pendingLineLoc_;
        pendingLine_ = -1;
        pendingLineLoc_ = {};

        if (parser_.at(TokenKind::Number))
            deferredLineOnly_ = true;
    } else if (allowIdentifierLabel && parser_.at(TokenKind::Identifier) &&
               parser_.peek(1).kind == TokenKind::Colon) {
        const Token &tok = parser_.peek();
        int labelNumber = parser_.ensureLabelNumber(tok.lexeme);
        parser_.noteNamedLabelDefinition(tok, labelNumber);
        parser_.consume();
        parser_.consume();
        line = labelNumber;
        loc = tok.loc;
    } else if (parser_.at(TokenKind::Number)) {
        const Token &tok = parser_.peek();
        if (auto parsed = parseLineNumberLiteral(tok.lexeme)) {
            line = *parsed;
        } else {
            parser_.emitError("B0001", tok, "line number is out of range");
        }
        loc = tok.loc;
        if (line != 0)
            parser_.noteNumericLabelUsage(line);
        parser_.consume();
    }
    fn(line, loc);
}

/// @brief Record a line number token for consumption by the next iteration.
/// @details When parsing falls through due to colon-separated content the
///          following line number needs to be preserved.  This method caches the
///          value so @ref withOptionalLineNumber can present it before reading
///          additional tokens.
/// @param line Parsed line number to replay later.
/// @param loc Source location associated with the numeric token.
/// @post @ref pendingLine_ and @ref pendingLineLoc_ equal the supplied values.
void StatementSequencer::stashPendingLine(int line, il::support::SourceLoc loc) {
    pendingLine_ = line;
    pendingLineLoc_ = loc;
}

/// @brief Retrieve the most recent separator kind consumed by the sequencer.
/// @details Helps callers distinguish between statements separated by newlines,
///          colons, or nothing when reasoning about layout-sensitive constructs
///          such as multi-line IF statements.
/// @return Enum describing the last observed separator.
StatementSequencer::SeparatorKind StatementSequencer::lastSeparator() const {
    return lastSeparator_;
}

/// @brief Decide how statement collection should proceed for the current line.
/// @details Evaluates the supplied @p isTerminator predicate and deferred-line
///          state to determine whether parsing should continue, terminate, or
///          pause until more input arrives.  When terminating, the
///          @p onTerminator callback is invoked so callers can consume any
///          terminator tokens before returning.
/// @param line Numeric label associated with the pending statement (0 when absent).
/// @param lineLoc Source location of the label token, if any.
/// @param isTerminator Predicate consulted to decide whether to stop parsing.
/// @param onTerminator Callback executed when the terminator predicate succeeds.
/// @param state Mutable bookkeeping record that captures separator and deferral information.
/// @return Action describing whether to continue, terminate, or defer parsing.
/// @post Termination populates @p state.info and calls @p onTerminator exactly
///       once. Deferral may consume and stash one following numeric label.
StatementSequencer::LineAction StatementSequencer::evaluateLineAction(
    int line,
    il::support::SourceLoc lineLoc,
    const TerminatorPredicate &isTerminator,
    const TerminatorConsumer &onTerminator,
    CollectionState &state) {
    if (isTerminator(line, lineLoc)) {
        state.info.line = line;
        state.info.loc = parser_.peek().loc;
        onTerminator(line, lineLoc, state.info);
        return LineAction::Terminate;
    }

    if (deferredLineOnly_) {
        if (parser_.at(TokenKind::Number)) {
            const Token &next = parser_.peek();
            if (auto parsed = parseLineNumberLiteral(next.lexeme)) {
                stashPendingLine(*parsed, next.loc);
            } else {
                parser_.emitError("B0001", next, "line number is out of range");
                stashPendingLine(0, next.loc);
            }
            parser_.consume();
            state.hadPendingLine = true;
        }
        return LineAction::Defer;
    }

    return LineAction::Continue;
}

/// @brief Collect consecutive statements until a terminator predicate fires.
/// @details Repeatedly parses statements, forwarding them into @p dst while
///          consulting @p isTerminator before each iteration to decide whether
///          parsing should stop.  When the predicate succeeds the supplied
///          @p onTerminator callback is run to perform any clean-up or token
///          consumption, and the gathered terminator metadata is returned to the
///          caller.
/// @param isTerminator Predicate that inspects the current line context and
///        decides whether collection should stop.
/// @param onTerminator Callback invoked when @p isTerminator succeeds. It can
///        consume tokens or populate the returned info structure.
/// @param dst Destination vector that receives parsed statements.
/// @return Information describing the terminator that halted collection.
/// @post Parsed statements and ADDFILE-spliced statements are appended to
///       @p dst without clearing its prior contents.
/// @note Reaching end of file without a terminator returns default-initialized
///       terminator information.
StatementSequencer::TerminatorInfo StatementSequencer::collectStatements(
    const TerminatorPredicate &isTerminator,
    const TerminatorConsumer &onTerminator,
    std::vector<StmtPtr> &dst) {
    CollectionState state;
    skipLeadingSeparator();
    while (!parser_.at(TokenKind::EndOfFile)) {
        skipLineBreaks();
        if (parser_.at(TokenKind::EndOfFile))
            break;

        state.separatorBefore = lastSeparator_;
        state.hadPendingLine = (pendingLine_ >= 0);

        int line = 0;
        il::support::SourceLoc lineLoc{};
        bool allowIdentifierLabel =
            (state.separatorBefore != SeparatorKind::Colon) && !state.hadPendingLine;
        withOptionalLineNumber(
            /// @brief Captures the current optional line number and source location.
            /// @param currentLine Parsed user line number.
            /// @param currentLoc Source location of the line marker.
            [&line, &lineLoc](int currentLine, il::support::SourceLoc currentLoc) {
                line = currentLine;
                lineLoc = currentLoc;
            },
            allowIdentifierLabel);

        LineAction action = evaluateLineAction(line, lineLoc, isTerminator, onTerminator, state);
        if (action == LineAction::Terminate || action == LineAction::Defer)
            break;

        TokenKind lookaheadKind = parser_.peek().kind;
        if (lookaheadKind == TokenKind::KeywordAddfile) {
            // Handle ADDFILE directive by splicing included content into dst.
            parser_.handleAddFileInto(dst);
        } else if (lookaheadKind != TokenKind::EndOfLine && lookaheadKind != TokenKind::EndOfFile &&
                   lookaheadKind != TokenKind::Colon) {
            auto stmt = parser_.parseStatement(line);
            if (stmt) {
                stmt->line = line;
                dst.push_back(std::move(stmt));
            }
        }

        skipStatementSeparator();
    }
    return state.info;
}

/// @brief Convenience overload collecting until a specific token appears.
/// @details Wraps the general @ref collectStatements logic with a predicate that
///          watches for @p terminator and a consumer that removes exactly one
///          matching token.
/// @param terminator Token kind to treat as the terminator.
/// @param dst Destination vector for parsed statements.
/// @return Terminator metadata populated by the underlying collection routine.
StatementSequencer::TerminatorInfo StatementSequencer::collectStatements(
    TokenKind terminator, std::vector<StmtPtr> &dst) {
    /// @brief Stops collection when the parser is positioned at the requested token.
    /// @return `true` when the requested terminator is current.
    auto predicate = [&](int, il::support::SourceLoc) { return parser_.at(terminator); };

    /// @brief Consumes the requested terminator after its metadata has been captured.
    auto consumer = [&](int, il::support::SourceLoc, TerminatorInfo &) { parser_.consume(); };
    return collectStatements(predicate, consumer, dst);
}

/// @brief Parse a full BASIC line, including optional label and statements.
/// @details Gathers statements separated by colons until a line break, neutral
///          boundary, or different user line is observed. Labels belonging to
///          the following line are restashed. A label-only line becomes a
///          @ref LabelStmt, an empty unlabeled line becomes an empty
///          @ref StmtList, a single statement is returned directly, and
///          multiple statements are wrapped in a @ref StmtList. User line
///          numbers are propagated to all statements on the logical line.
/// @return A statement node representing the parsed line. For empty lines a
///         placeholder list or label is emitted to keep numbering consistent.
StmtPtr StatementSequencer::parseStatementLine() {
    std::vector<StmtPtr> stmts;
    int lineNumber = 0;
    bool haveLine = false;
    il::support::SourceLoc lineLoc{};
    /// @brief Captures the first line context and stops at the next logical line.
    /// @param line Current user line number.
    /// @param loc Current line source location.
    /// @return `true` when collection reached a different logical line.
    auto predicate = [&](int line, il::support::SourceLoc loc) {
        if (!haveLine) {
            haveLine = true;
            lineNumber = line;
            lineLoc = loc;
            return false;
        }

        if (lastSeparator() == SeparatorKind::LineBreak) {
            if (hasUserLine(line))
                stashPendingLine(line, loc);
            return true;
        }

        if (lastSeparator() != SeparatorKind::Colon) {
            if (hasUserLine(line))
                stashPendingLine(line, loc);
            return true;
        }

        if (hasUserLine(line) && line != lineNumber) {
            stashPendingLine(line, loc);
            return true;
        }
        return false;
    };
    /// @brief Preserves a user line label encountered at the stopping boundary.
    /// @param line Boundary user line number.
    /// @param loc Boundary line source location.
    auto consumer = [&](int line, il::support::SourceLoc loc, TerminatorInfo &) {
        if (hasUserLine(line))
            stashPendingLine(line, loc);
    };

    collectStatements(predicate, consumer, stmts);

    il::support::SourceLoc stmtLineLoc = lineLoc;
    if (!stmtLineLoc.isValid() || !stmtLineLoc.hasLine()) {
        stmtLineLoc = parser_.peek().loc;
    }

    if (stmts.empty() && haveLine && hasUserLine(lineNumber)) {
        auto label = std::make_unique<LabelStmt>();
        label->line = lineNumber;
        label->loc = stmtLineLoc;

        if (pendingLine_ == lineNumber) {
            pendingLine_ = -1;
            pendingLineLoc_ = {};
        }

        return label;
    }

    if (stmts.empty()) {
        auto list = std::make_unique<StmtList>();
        list->line = lineNumber;
        list->loc = stmtLineLoc;
        list->stmts = std::move(stmts);
        return list;
    }

    if (!haveLine && !stmts.empty())
        lineNumber = stmts.front()->line;

    if (!isUnlabeledLine(lineNumber)) {
        for (auto &stmt : stmts) {
            if (stmt)
                stmt->line = lineNumber;
        }
    }

    if (stmts.size() == 1)
        return std::move(stmts.front());

    auto list = std::make_unique<StmtList>();
    list->line = lineNumber;

    il::support::SourceLoc firstLoc{};
    for (const auto &stmt : stmts) {
        if (stmt && stmt->loc.hasFile() && stmt->loc.hasLine()) {
            firstLoc = stmt->loc;
            break;
        }
    }
    if (!firstLoc.hasLine()) {
        firstLoc = stmtLineLoc;
    }

    list->loc = firstLoc;
    list->stmts = std::move(stmts);
    return list;
}

} // namespace il::frontends::basic
