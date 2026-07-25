//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Shared.cpp
// Purpose: Implements numeric-expression validation and the shared statement
//          analyzer state/RAII helpers used by BASIC semantic passes.
// Key invariants:
//   - Loop-kind and FOR-control-variable stacks remain balanced across every
//     statement visitor, including diagnostic-driven early exits.
//   - Moved-from guards become inert and never pop transferred state.
// Ownership/Lifetime:
//   - StmtShared and its guards borrow a SemanticAnalyzer; the analyzer must
//     outlive every helper and live guard bound to it.
// Links: src/frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp,
//        src/frontends/basic/SemanticAnalyzer_Internal.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Shared semantic-analysis utilities for BASIC statements.
/// @details Provides loop tracking helpers, type assertions, and RAII guards
///          that ensure @ref SemanticAnalyzer invariants are maintained even when
///          control flow unwinds due to diagnostics.

#include "frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp"

#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

#include <algorithm>
#include <utility>

namespace il::frontends::basic {

/// @brief Ensure an expression has a numeric type.
/// @details Evaluates @p expr and emits diagnostic B2001 when the result is not
///          numeric, appending the inferred semantic type to @p message.
///          Unknown expressions are accepted because their originating
///          diagnostic has already explained why a concrete type is
///          unavailable.
/// @param expr Mutable expression to analyze and validate.
/// @param message Context prefix used to construct the diagnostic text.
/// @post @p expr may contain annotations produced by @ref visitExpr.
/// @post Emits at most one B2001 diagnostic for this check.
void SemanticAnalyzer::requireNumeric(Expr &expr, std::string_view message) {
    Type exprType = visitExpr(expr);
    if (exprType == Type::Unknown || exprType == Type::Int || exprType == Type::Float)
        return;

    std::string msg(message);
    msg += ", got ";
    msg += semantic_analyzer_detail::semanticTypeName(exprType);
    msg += '.';

    de.emit(il::support::Severity::Error, "B2001", expr.loc, 1, std::move(msg));
}

/// @brief Record entry into a loop of the specified kind.
/// @details Pushes @p kind onto the loop stack so nested constructs can validate
///          statements such as EXIT and determine the active procedure kind.
/// @param kind Loop type being entered.
/// @post @p kind is the innermost entry in @ref loopStack_.
void SemanticAnalyzer::pushLoop(LoopKind kind) {
    loopStack_.push_back(kind);
}

/// @brief Mark exit from the innermost loop.
/// @details Pops the loop stack when present. An empty stack is tolerated so
///          guard cleanup remains defensive rather than invoking undefined
///          behavior after an earlier imbalance.
/// @post A non-empty stack has lost exactly its former last entry.
void SemanticAnalyzer::popLoop() {
    if (!loopStack_.empty())
        loopStack_.pop_back();
}

/// @brief Track a FOR-loop control variable by name.
/// @details Stores the variable so assignments can be flagged while the loop is
///          active. Nested FOR loops retain independent stack entries.
/// @param name Identifier of the FOR-loop control variable; copied into
///             analyzer-owned storage.
/// @post @p name is the innermost entry in @ref forStack_.
void SemanticAnalyzer::pushForVariable(std::string_view name) {
    forStack_.emplace_back(name);
}

/// @brief Remove the most recently tracked FOR-loop variable.
/// @details Pops the stack when non-empty, mirroring the lifetime of the
///          corresponding
///          @ref semantic_analyzer_detail::StmtShared::ForLoopGuard.
/// @post A non-empty stack has lost exactly its former last entry.
void SemanticAnalyzer::popForVariable() {
    if (!forStack_.empty())
        forStack_.pop_back();
}

/// @brief Test whether a variable is currently registered as a FOR-loop control variable.
/// @param name Identifier to search for.
/// @return @c true when @p name appears in the active FOR-loop stack.
/// @note Matching is byte-exact; callers supply the analyzer's canonical name.
bool SemanticAnalyzer::isLoopVariableActive(std::string_view name) const noexcept {
    return std::find(forStack_.begin(), forStack_.end(), name) != forStack_.end();
}

/// @brief Check if a specific loop kind exists anywhere in the loop stack.
/// @param kind Loop or procedure kind to search for.
/// @return @c true when @p kind appears anywhere in the active stack.
bool SemanticAnalyzer::hasLoopOfKind(LoopKind kind) const noexcept {
    return std::find(loopStack_.begin(), loopStack_.end(), kind) != loopStack_.end();
}

} // namespace il::frontends::basic

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Construct shared helpers bound to the owning analyser.
/// @param analyzer Semantic analyzer that maintains loop stacks and diagnostics;
///                 it must outlive this helper and all guards created from it.
StmtShared::StmtShared(SemanticAnalyzer &analyzer) noexcept : analyzer_(analyzer) {}

/// @brief RAII guard that records loop entry and exit.
/// @details Pushes @p kind onto the loop stack during construction and pops it
///          during destruction.
/// @param analyzer Analyzer whose loop stack is updated; it must outlive the
///                 guard or any guard receiving its state by move.
/// @param kind Loop or procedure kind to make active.
/// @post The analyzer's loop stack contains one new innermost entry.
StmtShared::LoopGuard::LoopGuard(SemanticAnalyzer &analyzer,
                                 SemanticAnalyzer::LoopKind kind) noexcept
    : analyzer_(&analyzer) {
    analyzer_->pushLoop(kind);
}

/// @brief Move-construct the guard, transferring ownership of the stack entry.
/// @param other Guard whose pending pop responsibility is transferred.
/// @post @p other is inert and destruction of @p other has no analyzer effect.
StmtShared::LoopGuard::LoopGuard(LoopGuard &&other) noexcept : analyzer_(other.analyzer_) {
    other.analyzer_ = nullptr;
}

/// @brief Move-assign the guard, releasing any existing loop tracking.
/// @details Pops this guard's current stack entry before adopting @p other's
///          pending entry. Self-assignment is an exact no-op.
/// @param other Guard whose pending pop responsibility is transferred.
/// @return Reference to this guard.
/// @post @p other is inert unless @p other and this object are identical.
StmtShared::LoopGuard &StmtShared::LoopGuard::operator=(LoopGuard &&other) noexcept {
    if (this == &other)
        return *this;
    if (analyzer_)
        analyzer_->popLoop();
    analyzer_ = other.analyzer_;
    other.analyzer_ = nullptr;
    return *this;
}

/// @brief Pop the loop stack when the guard goes out of scope.
/// @details A moved-from guard has a null analyzer pointer and performs no work.
StmtShared::LoopGuard::~LoopGuard() noexcept {
    if (analyzer_)
        analyzer_->popLoop();
}

/// @brief RAII guard that tracks FOR-loop variables.
/// @details Registers @p variable on construction and removes it when destroyed.
/// @param analyzer Analyzer whose FOR-variable stack is updated; it must outlive
///                 the guard or any guard receiving its state by move.
/// @param variable Canonical control-variable name transferred to analyzer
///                 storage.
/// @post The analyzer tracks @p variable as its innermost active FOR variable.
StmtShared::ForLoopGuard::ForLoopGuard(SemanticAnalyzer &analyzer, std::string variable)
    : analyzer_(&analyzer) {
    analyzer_->pushForVariable(std::move(variable));
}

/// @brief Move-construct a guard, adopting the tracked variable.
/// @param other Guard whose pending deregistration responsibility is transferred.
/// @post @p other is inert and destruction of @p other has no analyzer effect.
StmtShared::ForLoopGuard::ForLoopGuard(ForLoopGuard &&other) noexcept : analyzer_(other.analyzer_) {
    other.analyzer_ = nullptr;
}

/// @brief Move-assign a guard, releasing any currently tracked variable first.
/// @details Pops this guard's current variable before adopting @p other's
///          pending variable. Self-assignment is an exact no-op.
/// @param other Guard whose pending deregistration responsibility is transferred.
/// @return Reference to this guard.
/// @post @p other is inert unless @p other and this object are identical.
StmtShared::ForLoopGuard &StmtShared::ForLoopGuard::operator=(ForLoopGuard &&other) noexcept {
    if (this == &other)
        return *this;
    if (analyzer_)
        analyzer_->popForVariable();
    analyzer_ = other.analyzer_;
    other.analyzer_ = nullptr;
    return *this;
}

/// @brief Deregister the loop variable when the guard is destroyed.
/// @details A moved-from guard has a null analyzer pointer and performs no work.
StmtShared::ForLoopGuard::~ForLoopGuard() noexcept {
    if (analyzer_)
        analyzer_->popForVariable();
}

/// @brief Determine whether a name refers to an active FOR-loop variable.
/// @param name Identifier to test.
/// @return @c true when @p name is currently tracked by an active guard.
bool StmtShared::isLoopVariable(std::string_view name) const noexcept {
    return analyzer_.isLoopVariableActive(name);
}

/// @brief Emit a diagnostic when a FOR-loop variable is mutated inside the loop body.
/// @details Reports B1010 through the analyzer's diagnostic emitter. The
///          helper does not independently verify that @p name is active.
/// @param name Canonical name interpolated into the diagnostic message.
/// @param loc Source location of the offending assignment.
/// @param width Number of source columns highlighted by the diagnostic.
void StmtShared::reportLoopVariableMutation(const std::string &name,
                                            const il::support::SourceLoc &loc,
                                            uint32_t width) {
    std::string msg = "cannot assign to loop variable '" + name + "' inside FOR";
    analyzer_.de.emit(il::support::Severity::Error, "B1010", loc, width, std::move(msg));
}

} // namespace il::frontends::basic::semantic_analyzer_detail
