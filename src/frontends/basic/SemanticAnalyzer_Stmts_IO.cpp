//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements IO and screen-manipulation statement checks for the BASIC semantic
// analyser, covering PRINT/INPUT, channel management, and terminal control
// commands.  The routines live in a dedicated translation unit so the main
// analyzer can remain focused on expression semantics.
//
//===----------------------------------------------------------------------===//
//
/// @file SemanticAnalyzer_Stmts_IO.cpp
/// @brief Semantic checks for BASIC IO statements (PRINT, INPUT, OPEN, etc.).
/// @details The routines visit payload expressions, apply explicit path/channel
///          type rules where implemented, track literal open channels for
///          reopen warnings and procedure rollback, resolve INPUT targets, and
///          reject mutation of active FOR control variables.

#include "frontends/basic/SemanticAnalyzer_Stmts_IO.hpp"
#include "frontends/basic/ASTUtils.hpp"

#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Constructs an IO statement context over shared analyzer state.
/// @details Wraps the common @ref StmtShared base so helpers can access
///          diagnostics and loop-tracking facilities without duplicating
///          plumbing code.
/// @param analyzer Analyzer borrowed by the base context.
/// @post No loop or lexical stack is modified.
IOStmtContext::IOStmtContext(SemanticAnalyzer &analyzer) noexcept : StmtShared(analyzer) {}

} // namespace il::frontends::basic::semantic_analyzer_detail

namespace il::frontends::basic {

using semantic_analyzer_detail::IOStmtContext;
using semantic_analyzer_detail::semanticTypeName;

/// @brief Validate the CLS statement. No semantic checks are required.
/// @details CLS performs a screen clear and accepts no operands, so the visitor
///          intentionally performs no validation beyond acknowledging the node.
void SemanticAnalyzer::visit(const ClsStmt &) {
    // nothing to validate
}

/// @brief Validate the COLOR statement operands.
/// @details Requires the foreground and optional background expressions to be
///          integer or float. Unknown recovery types are tolerated by
///          @ref requireNumeric.
/// @param s Statement with required foreground and optional background.
void SemanticAnalyzer::visit(const ColorStmt &s) {
    requireNumeric(*s.fg, "COLOR foreground must be numeric");
    if (s.bg)
        requireNumeric(*s.bg, "COLOR background must be numeric");
}

/// @brief Validate SLEEP statement operand.
/// @details Requires the duration expression to be integer or float while
///          tolerating unknown recovery type.
/// @param s Statement whose required duration node is dereferenced.
void SemanticAnalyzer::visit(const SleepStmt &s) {
    requireNumeric(*s.ms, "SLEEP duration must be numeric");
}

/// @brief Validate LOCATE statement operands.
/// @details Requires the row and optional column to be integer or float while
///          tolerating unknown recovery types. No coordinate range check occurs
///          here.
/// @param s Statement whose row is required and column is optional.
void SemanticAnalyzer::visit(const LocateStmt &s) {
    requireNumeric(*s.row, "LOCATE row must be numeric");
    if (s.col)
        requireNumeric(*s.col, "LOCATE column must be numeric");
}

/// @brief Validate the CURSOR statement. No semantic checks are required.
/// @details CURSOR accepts only ON/OFF keywords which are validated during
///          parsing, so no expression validation is needed here.
void SemanticAnalyzer::visit(const CursorStmt &) {
    // nothing to validate - ON/OFF is parsed as a boolean flag
}

/// @brief Analyze a PRINT statement for semantic correctness.
/// @details Traverses each printed expression (ignoring pure separators) so any
///          nested semantic issues are diagnosed before code generation.
/// @param p Statement whose expression items are visited in order.
void SemanticAnalyzer::analyzePrint(const PrintStmt &p) {
    for (const auto &it : p.items)
        if (it.kind == PrintItem::Kind::Expr && it.expr)
            visitExpr(*it.expr);
}

/// @brief Analyze a PRINT# or WRITE# statement.
/// @details Visits the optional channel and each non-null payload in order. This
///          routine does not independently require the channel to be integer.
/// @param p Channel-output statement to traverse.
void SemanticAnalyzer::analyzePrintCh(const PrintChStmt &p) {
    if (p.channelExpr)
        visitExpr(*p.channelExpr);
    for (const auto &arg : p.args)
        if (arg)
            visitExpr(*arg);
}

/// @brief Analyze an OPEN statement including type checks and channel tracking.
/// @details Accepts Input, Output, Append, Binary, and Random modes; other enum
///          values emit `B4001`. The optional path must be string and channel
///          integer, with unknown tolerated for recovery. A literal channel
///          already in @ref openChannels_ emits warning `B3002`; a newly opened
///          literal is inserted and logged for procedure rollback. Dynamic
///          channels cannot be tracked.
/// @param stmt Mutable OPEN statement whose expressions may be annotated.
void SemanticAnalyzer::analyzeOpen(OpenStmt &stmt) {
    const bool modeValid =
        stmt.mode == OpenStmt::Mode::Input || stmt.mode == OpenStmt::Mode::Output ||
        stmt.mode == OpenStmt::Mode::Append || stmt.mode == OpenStmt::Mode::Binary ||
        stmt.mode == OpenStmt::Mode::Random;
    if (!modeValid) {
        std::string msg = "invalid OPEN mode";
        de.emit(il::support::Severity::Error, "B4001", stmt.loc, 4, std::move(msg));
    }

    if (stmt.pathExpr) {
        Type pathTy = visitExpr(*stmt.pathExpr);
        if (pathTy != Type::Unknown && pathTy != Type::String) {
            std::string msg = "OPEN path expression must be STRING, got ";
            msg += semanticTypeName(pathTy);
            msg += '.';
            de.emit(il::support::Severity::Error, "B2001", stmt.pathExpr->loc, 1, std::move(msg));
        }
    }

    if (stmt.channelExpr) {
        Type channelTy = visitExpr(*stmt.channelExpr);
        if (channelTy != Type::Unknown && channelTy != Type::Int) {
            std::string msg = "OPEN channel expression must be INTEGER, got ";
            msg += semanticTypeName(channelTy);
            msg += '.';
            de.emit(
                il::support::Severity::Error, "B2001", stmt.channelExpr->loc, 1, std::move(msg));
        } else if (auto *intExpr = as<IntExpr>(*stmt.channelExpr)) {
            long long channel = intExpr->value;
            bool wasOpen = openChannels_.contains(channel);
            if (wasOpen) {
                std::string msg = "channel #";
                msg += std::to_string(channel);
                msg += " is already open";
                de.emit(il::support::Severity::Warning,
                        "B3002",
                        stmt.channelExpr->loc,
                        1,
                        std::move(msg));
            } else {
                if (activeProcScope_)
                    activeProcScope_->noteChannelMutation(channel, false);
                openChannels_.insert(channel);
            }
        }
    }
}

/// @brief Analyze a CLOSE statement and update channel bookkeeping.
/// @details A missing channel is accepted. A present channel must be integer or
///          unknown. A literal currently modeled open is erased and its prior
///          open state logged for procedure rollback; closing an untracked
///          literal emits no warning.
/// @param stmt Mutable CLOSE statement.
void SemanticAnalyzer::analyzeClose(CloseStmt &stmt) {
    if (!stmt.channelExpr)
        return;

    Type channelTy = visitExpr(*stmt.channelExpr);
    if (channelTy != Type::Unknown && channelTy != Type::Int) {
        std::string msg = "CLOSE channel expression must be INTEGER, got ";
        msg += semanticTypeName(channelTy);
        msg += '.';
        de.emit(il::support::Severity::Error, "B2001", stmt.channelExpr->loc, 1, std::move(msg));
        return;
    }

    if (auto *intExpr = as<IntExpr>(*stmt.channelExpr)) {
        long long channel = intExpr->value;
        if (openChannels_.contains(channel)) {
            if (activeProcScope_)
                activeProcScope_->noteChannelMutation(channel, true);
            openChannels_.erase(channel);
        }
    }
}

/// @brief Analyze a SEEK statement for channel and position correctness.
/// @details Visits each optional operand independently and requires integer
///          when its type is known. No open-channel or position-range check is
///          performed.
/// @param stmt Mutable SEEK statement.
void SemanticAnalyzer::analyzeSeek(SeekStmt &stmt) {
    if (stmt.channelExpr) {
        Type channelTy = visitExpr(*stmt.channelExpr);
        if (channelTy != Type::Unknown && channelTy != Type::Int) {
            std::string msg = "SEEK channel expression must be INTEGER, got ";
            msg += semanticTypeName(channelTy);
            msg += '.';
            de.emit(
                il::support::Severity::Error, "B2001", stmt.channelExpr->loc, 1, std::move(msg));
        }
    }

    if (stmt.positionExpr) {
        Type posTy = visitExpr(*stmt.positionExpr);
        if (posTy != Type::Unknown && posTy != Type::Int) {
            std::string msg = "SEEK position expression must be INTEGER, got ";
            msg += semanticTypeName(posTy);
            msg += '.';
            de.emit(
                il::support::Severity::Error, "B2001", stmt.positionExpr->loc, 1, std::move(msg));
        }
    }
}

/// @brief Analyze an INPUT statement targeting variables in the current scope.
/// @details Visits the optional prompt without imposing a type, skips empty
///          target names, resolves/tracks each remaining target as an input
///          definition, and emits the shared loop-variable mutation diagnostic
///          for exact names active in a FOR loop.
/// @param inp Mutable INPUT statement; target strings may be replaced by scoped
///            resolved names.
void SemanticAnalyzer::analyzeInput(InputStmt &inp) {
    IOStmtContext ctx(*this);
    if (inp.prompt)
        visitExpr(*inp.prompt);
    for (auto &name : inp.vars) {
        if (name.empty())
            continue;
        resolveAndTrackSymbol(name, SymbolKind::InputTarget);
        if (ctx.isLoopVariable(name))
            ctx.reportLoopVariableMutation(name, inp.loc, static_cast<uint32_t>(name.size()));
    }
}

/// @brief Analyze an INPUT# statement targeting a specific channel.
/// @details Resolves each non-empty target name as an input definition and
///          checks active FOR-variable mutation. This routine does not visit or
///          type-check a channel expression.
/// @param inp Mutable channel-input statement whose target names may be resolved.
void SemanticAnalyzer::analyzeInputCh(InputChStmt &inp) {
    IOStmtContext ctx(*this);
    for (auto &ref : inp.targets) {
        auto &name = ref.name;
        if (name.empty())
            continue;
        resolveAndTrackSymbol(name, SymbolKind::InputTarget);
        if (ctx.isLoopVariable(name))
            ctx.reportLoopVariableMutation(name, inp.loc, static_cast<uint32_t>(name.size()));
    }
}

/// @brief Analyze a LINE INPUT# statement.
/// @details Visits the optional channel expression and destination expression to
///          ensure nested semantics are validated. It imposes no channel,
///          destination, or string-specific type requirement here.
/// @param inp Mutable line-input statement.
void SemanticAnalyzer::analyzeLineInputCh(LineInputChStmt &inp) {
    if (inp.channelExpr)
        visitExpr(*inp.channelExpr);
    if (inp.targetVar)
        visitExpr(*inp.targetVar);
}

} // namespace il::frontends::basic
