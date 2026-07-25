//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/Check_Jumps.cpp
// Purpose: Validate BASIC non-local control-flow statements and keep label,
//          procedure, and error-handler state synchronized during semantic
//          analysis.
// Key invariants:
//   * Every referenced line label is recorded even when it is not yet known, so
//     the analyzer retains complete cross-scope reference information.
//   * ON ERROR and RESUME update or query the active handler state through the
//     shared control-check context.
//   * RETURN is distinguished between procedure return and legacy GOSUB return
//     before later lowering consumes the statement.
// References: docs/tutorials/basic-tutorial.md#error-handling,
//             docs/internals/codemap/basic.md#semantic-analyzer
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Semantic checks for jump-oriented constructs (GOTO, GOSUB, ON ERROR,
///        RESUME, RETURN).
/// @details Ensures label references resolve, manages error-handler state, and
///          enforces RETURN usage constraints.
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/Check_Common.hpp"

#include <string>

namespace il::frontends::basic::sem {

namespace {
/// @brief Emit an error diagnostic for a reference to an unknown label.
///
/// @details Constructs a canonical "unknown line" message that mirrors the
///          BASIC runtime wording, then forwards the text to the diagnostic
///          engine owned by @p context.  Centralising the logic keeps the error
///          codes and formatting consistent across all jump checks.
///
/// @param context Control-flow analysis context that exposes diagnostics.
/// @param label   Numeric label identifier referenced by the statement.
/// @param loc     Source location used to underline the offending token.
/// @param width   Width of the highlighted span in characters.
void emitUnknownLabel(ControlCheckContext &context,
                      int label,
                      const il::support::SourceLoc &loc,
                      uint32_t width) {
    const std::string labelText = std::to_string(label);
    context.diagnostics().emit(
        diag::BasicDiag::UnknownLineLabel,
        loc,
        width,
        std::initializer_list<diag::Replacement>{diag::Replacement{"label", labelText}});
}
} // namespace

/// @brief Validate a @c GOTO statement's control-flow constraints.
///
/// @details Registers the referenced label with the active control-flow context
///          so later resolution passes can detect unreferenced labels.  If the
///          label has not been defined yet, emits a diagnostic describing the
///          unresolved target but still records the reference to allow deferred
///          resolution during analysis of subsequent statements.
///
/// @param analyzer Semantic analyzer currently traversing the program.
/// @param stmt     Parsed @c GOTO statement to analyse.
void analyzeGoto(SemanticAnalyzer &analyzer, const GotoStmt &stmt) {
    ControlCheckContext context(analyzer);
    context.insertLabelReference(stmt.target);
    if (!context.hasKnownLabel(stmt.target))
        emitUnknownLabel(context, stmt.target, stmt.loc, 4);
}

/// @brief Validate a @c GOSUB statement and its label reference.
///
/// @details Records the referenced label to enable post-pass reachability checks
///          and emits an "unknown line" diagnostic when the label is not present
///          in the current procedure scope.  The helper mirrors the @c GOTO
///          logic but adjusts the diagnostic width to match the keyword length.
///
/// @param analyzer Semantic analyzer currently traversing the program.
/// @param stmt     Parsed @c GOSUB statement to analyse.
void analyzeGosub(SemanticAnalyzer &analyzer, const GosubStmt &stmt) {
    ControlCheckContext context(analyzer);
    context.insertLabelReference(stmt.targetLine);
    if (!context.hasKnownLabel(stmt.targetLine))
        emitUnknownLabel(context, stmt.targetLine, stmt.loc, 5);
}

/// @brief Analyse an @c ON ERROR GOTO statement and update handler state.
///
/// @details When the statement clears the handler (`GOTO 0`) the active handler
///          state is reset.  Otherwise the referenced label is recorded and
///          validated just like a @c GOTO before installing it as the active
///          error handler inside the control-flow context.
///
/// @param analyzer Semantic analyzer providing procedure state.
/// @param stmt     Parsed @c ON ERROR GOTO statement to analyse.
void analyzeOnErrorGoto(SemanticAnalyzer &analyzer, const OnErrorGoto &stmt) {
    ControlCheckContext context(analyzer);
    if (stmt.toZero) {
        context.clearErrorHandler();
        return;
    }

    context.insertLabelReference(stmt.target);
    if (!context.hasKnownLabel(stmt.target))
        emitUnknownLabel(context, stmt.target, stmt.loc, 4);

    context.installErrorHandler(stmt.target);
}

/// @brief Verify usage of a @c RESUME statement within an error handler.
///
/// @details Ensures a handler is currently active before allowing the resume;
///          otherwise emits diagnostic B1012 describing the misuse.  When the
///          statement resumes to a specific label, the label reference is
///          recorded and validated using the shared helper so unresolved targets
///          surface consistent diagnostics.
///
/// @param analyzer Semantic analyzer providing the execution context.
/// @param stmt     Parsed @c RESUME statement to analyse.
void analyzeResume(SemanticAnalyzer &analyzer, const Resume &stmt) {
    ControlCheckContext context(analyzer);
    if (!context.hasActiveErrorHandler()) {
        std::string msg = "RESUME requires an active error handler";
        context.diagnostics().emit(
            il::support::Severity::Error, "B1012", stmt.loc, 6, std::move(msg));
    }

    if (stmt.mode != Resume::Mode::Label)
        return;

    context.insertLabelReference(stmt.target);
    if (!context.hasKnownLabel(stmt.target))
        emitUnknownLabel(context, stmt.target, stmt.loc, 4);
}

/// @brief Validate a @c RETURN statement in both procedure and GOSUB contexts.
///
/// @details Distinguishes between procedure returns and legacy GOSUB returns.
///          When used outside a procedure, @c RETURN is only legal without a
///          value, in which case it is converted into a GOSUB return.  Otherwise
///          diagnostic B1008 is emitted.  The helper also clears any active error
///          handler to match BASIC's unwinding semantics.
///
/// @param analyzer Semantic analyzer providing access to control-flow state.
/// @param stmt     Parsed @c RETURN statement to analyse; may be rewritten to
///                 mark GOSUB return behaviour.
void analyzeReturn(SemanticAnalyzer &analyzer, ReturnStmt &stmt) {
    ControlCheckContext context(analyzer);
    if (!context.hasActiveProcScope()) {
        if (stmt.value) {
            std::string msg = "RETURN with value not allowed at top level";
            context.diagnostics().emit(
                il::support::Severity::Error, "B1008", stmt.loc, 6, std::move(msg));
        } else if (context.mainHasGosub()) {
            stmt.isGosubReturn = true;
        } else {
            std::string msg = "RETURN without GOSUB";
            context.diagnostics().emit(
                il::support::Severity::Error, "B1008", stmt.loc, 6, std::move(msg));
        }
    }

    if (context.hasActiveErrorHandler())
        context.clearErrorHandler();
}

} // namespace il::frontends::basic::sem
