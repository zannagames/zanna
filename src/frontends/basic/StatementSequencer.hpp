//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/StatementSequencer.hpp
// Purpose: Declares the parser helper that groups BASIC statements across
//          newline, colon, label, include, and structured-block boundaries.
// Key invariants:
//   - Separator state describes the complete most recently skipped run.
//   - Pending numeric labels are consumed or restashed exactly once.
//   - Caller-defined terminators are tested before ordinary statement parsing.
// Ownership/Lifetime:
//   - The sequencer stores a non-owning Parser reference.
//   - Parsed statements are returned through unique ownership or moved into a
//     caller-owned destination vector.
// Links: src/frontends/basic/StatementSequencer.cpp,
//        src/frontends/basic/Parser.hpp,
//        src/frontends/basic/LineUtils.hpp,
//        src/frontends/basic/AST.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/basic/Token.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include "support/source_location.hpp"
#include <functional>
#include <vector>

/// @file
/// @brief Declares BASIC parser statement sequencing and terminator collection.

namespace il::frontends::basic {

/// @brief Token and grammar owner borrowed by the statement sequencer.
class Parser;

/// @brief Helper that manages separators, labels, and statement iteration.
/// @details Centralizes line-oriented and colon-separated parsing so individual
///          statement productions do not duplicate separator, label, ADDFILE,
///          and structured-block termination rules.
/// @invariant @ref parser_ remains valid for this object's lifetime.
class StatementSequencer {
  public:
    /// @brief Aggregated information about a terminating keyword.
    struct TerminatorInfo {
        int line = 0;                 ///< Optional line number preceding terminator.

        /// Location of the terminator token captured before its consumer runs.
        il::support::SourceLoc loc{}; ///< Location where terminator keyword appeared.
    };

    /// @brief Classification of the most recently consumed separator.
    enum class SeparatorKind {
        None,      ///< No separator observed since last statement.
        Colon,     ///< Colon separated adjacent statements.
        LineBreak, ///< Line break separated adjacent statements.
    };

    /// Predicate receiving the pending line number and label location.
    using TerminatorPredicate =
        std::function<bool(int, il::support::SourceLoc)>; ///< Identifies terminator tokens.

    /// Callback that consumes a recognized terminator and may enrich its metadata.
    using TerminatorConsumer = std::function<void(int, il::support::SourceLoc, TerminatorInfo &)>;

    /// @brief Construct a sequencer bound to a parser instance.
    /// @param parser Parser borrowed for the sequencer's complete lifetime.
    explicit StatementSequencer(Parser &parser);

    /// @brief Consumes all leading colon and newline separators.
    /// @post @ref lastSeparator reflects the consumed run, with line breaks
    ///       taking precedence over colons.
    void skipLeadingSeparator();

    /// @brief Consume consecutive newline tokens.
    /// @return @c true when at least one newline token was consumed.
    bool skipLineBreaks();

    /// @brief Consumes all colon and newline separators after a statement.
    /// @post @ref lastSeparator reflects the consumed run.
    void skipStatementSeparator();

    /// @brief Invoke @p fn with an optional numeric line label.
    /// @details Consumes a stashed label, an allowed identifier-colon label, or
    ///          a numeric line token; otherwise supplies zero and an invalid
    ///          location.
    /// @param fn Callback invoked exactly once with resolved line context.
    /// @param allowIdentifierLabel Whether an identifier-colon pair may define
    ///                             a named label here.
    void withOptionalLineNumber(const std::function<void(int, il::support::SourceLoc)> &fn,
                                bool allowIdentifierLabel = false);

    /// @brief Remember a pending line label for the next statement.
    /// @param line Numeric label value to replay.
    /// @param loc Source location associated with the label.
    void stashPendingLine(int line, il::support::SourceLoc loc);

    /// @brief Inspect the last consumed separator classification.
    /// @return Separator state retained by the most recent skip operation.
    SeparatorKind lastSeparator() const;

    /// @brief Populate @p dst with statements until @p isTerminator fires.
    /// @param isTerminator Predicate tested before parsing each statement.
    /// @param onTerminator Consumer invoked once for a recognized terminator.
    /// @param dst Destination receiving newly parsed or included statements.
    /// @return Captured terminator metadata, or default metadata at end of file.
    TerminatorInfo collectStatements(const TerminatorPredicate &isTerminator,
                                     const TerminatorConsumer &onTerminator,
                                     std::vector<StmtPtr> &dst);

    /// @brief Populate @p dst until the specified terminator token is encountered.
    /// @param terminator Token whose first occurrence stops collection and is
    ///                   consumed.
    /// @param dst Destination receiving parsed statements.
    /// @return Captured metadata for the consumed terminator.
    TerminatorInfo collectStatements(TokenKind terminator, std::vector<StmtPtr> &dst);

    /// @brief Parse and coalesce statements that share a logical source line.
    /// @return A label node for a label-only line, one statement directly, or a
    ///         @ref StmtList representing zero or multiple statements.
    StmtPtr parseStatementLine();

  private:
    /// @brief Status returned when deciding how to handle the current line context.
    enum class LineAction {
        Continue,  ///< Proceed with parsing the statement body.
        Defer,     ///< Defer parsing until additional input arrives.
        Terminate, ///< Stop collection because a terminator was encountered.
    };

    /// @brief Local state threaded through statement collection iterations.
    struct CollectionState {
        SeparatorKind separatorBefore =
            SeparatorKind::None;     ///< Separator preceding the current line.
        bool hadPendingLine = false; ///< True when a pending line label exists before processing.
        TerminatorInfo info;         ///< Populated once a terminator is observed.
    };

    /// @brief Classifies the pending line as a terminator, deferral, or statement.
    /// @param line Optional numeric label preceding the pending construct.
    /// @param lineLoc Source location of that label.
    /// @param isTerminator Caller-defined stop predicate.
    /// @param onTerminator Callback that consumes a recognized terminator.
    /// @param state Mutable iteration state receiving terminator metadata.
    /// @return Action selected for the collection loop.
    LineAction evaluateLineAction(int line,
                                  il::support::SourceLoc lineLoc,
                                  const TerminatorPredicate &isTerminator,
                                  const TerminatorConsumer &onTerminator,
                                  CollectionState &state);

    /// Non-owning parser providing token access and statement sub-parses.
    Parser &parser_;                          ///< Underlying parser providing token access.

    /// Deferred numeric line label, or @c -1 when none is pending.
    int pendingLine_ = -1;                    ///< Deferred numeric line label for next statement.

    /// Source location paired with @ref pendingLine_.
    il::support::SourceLoc pendingLineLoc_{}; ///< Location of the deferred line label.

    /// Whether a replayed label must defer a following numeric label token.
    bool deferredLineOnly_ =
        false; ///< True when pending line should emit label before reading new tokens.

    /// Classification of the most recently consumed separator run.
    SeparatorKind lastSeparator_ =
        SeparatorKind::None; ///< Treat start-of-file as neutral separator state.
};

} // namespace il::frontends::basic
