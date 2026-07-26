//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Shared infrastructure for control-flow and expression semantic
///        checkers.
/// @details Provides thin context wrappers and helper routines that expose the
///          mutable state used by control-statement analyzers (loop stacks and
///          label tracking) and expression analyzers (type queries and implicit
///          conversions) while asserting invariants when a checker completes.
///          Individual checkers live in dedicated translation units.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace il::frontends::basic::sem {

/// @brief Shared context for control-statement semantic checks.
/// @details Wraps the analyzer state so helpers can manipulate loop and label
///          tracking consistently. On destruction the context asserts that loop
///          and FOR-variable stacks have been balanced by the checker.
class ControlCheckContext {
  public:
    /// @brief Construct a control-flow check context wrapping the given analyzer.
    /// @details Snapshots the current loop and FOR-variable stack depths so that
    ///          the destructor can assert they were balanced by the checker.
    /// @param analyzer The semantic analyzer whose loop/label state is being wrapped.
    explicit ControlCheckContext(SemanticAnalyzer &analyzer) noexcept
        : analyzer_(&analyzer), stmtContext_(analyzer), loopDepth_(analyzer.loopStack_.size()),
          forDepth_(analyzer.forStack_.size()) {}

    ControlCheckContext(const ControlCheckContext &) = delete;
    ControlCheckContext &operator=(const ControlCheckContext &) = delete;

    /// @brief Restore semantic-control stacks to their entry depths.
    /// @details Recovery truncates any loop or FOR-variable state left behind
    ///          by malformed control flow so diagnostics can be returned
    ///          without leaking checker state into later statements.
    ~ControlCheckContext() {
        // Malformed control flow — a stray NEXT, an unterminated FOR, or a reserved
        // keyword used as an identifier that parses as NEXT/LOOP — can leave the
        // loop/FOR stacks deeper than they were on entry. Recovery must be
        // exception-safe: restore the stacks to the snapshot depth so the checker
        // returns structured diagnostics for the invalid source instead of aborting
        // on a balance assert (VDOC-244). The checker only ever grows these stacks
        // within a context and guards its pops against underflow, so restoring is a
        // truncation back to the entry depth; a shallower stack (which should not
        // occur) is left untouched rather than grown.
        if (analyzer_ != nullptr) {
            if (analyzer_->loopStack_.size() > loopDepth_)
                analyzer_->loopStack_.resize(loopDepth_);
            if (analyzer_->forStack_.size() > forDepth_)
                analyzer_->forStack_.resize(forDepth_);
        }
    }

    /// @brief Access the wrapped semantic analyzer (mutable).
    /// @return Reference to the underlying SemanticAnalyzer instance.
    [[nodiscard]] SemanticAnalyzer &analyzer() noexcept {
        return *analyzer_;
    }

    /// @brief Access the wrapped semantic analyzer (const).
    /// @return Const reference to the underlying SemanticAnalyzer instance.
    [[nodiscard]] const SemanticAnalyzer &analyzer() const noexcept {
        return *analyzer_;
    }

    /// @brief Access the control-statement context for structured statement tracking.
    /// @return Reference to the ControlStmtContext used for loop guard management.
    [[nodiscard]] semantic_analyzer_detail::ControlStmtContext &stmt() noexcept {
        return stmtContext_;
    }

    /// @brief Access the control-statement context (const).
    /// @return Const reference to the ControlStmtContext.
    [[nodiscard]] const semantic_analyzer_detail::ControlStmtContext &stmt() const noexcept {
        return stmtContext_;
    }

    /// @brief Check whether a line-number label has been defined in the program.
    /// @param label The integer line-number label to look up.
    /// @return True if the label is in the defined-labels set.
    [[nodiscard]] bool hasKnownLabel(int label) const noexcept {
        return analyzer_->labels_.count(label) != 0;
    }

    /// @brief Check whether a line-number label has been referenced by GOTO/GOSUB.
    /// @param label The integer line-number label to look up.
    /// @return True if the label has been referenced.
    [[nodiscard]] bool hasReferencedLabel(int label) const noexcept {
        return analyzer_->labelRefs_.count(label) != 0;
    }

    /// @brief Record a forward reference to a line-number label.
    /// @details Inserts the label into the reference set and notifies the active
    ///          procedure scope (if any) so it can track cross-scope label references.
    /// @param label The integer line-number label being referenced.
    /// @return True if this is the first reference to the label (newly inserted).
    bool insertLabelReference(int label) {
        auto insertResult = analyzer_->labelRefs_.insert(label);
        if (insertResult.second && analyzer_->activeProcScope_)
            analyzer_->activeProcScope_->noteLabelRefInserted(label);
        return insertResult.second;
    }

    /// @brief Check whether there is at least one active enclosing loop.
    /// @return True if the loop stack is non-empty.
    [[nodiscard]] bool hasActiveLoop() const noexcept {
        return !analyzer_->loopStack_.empty();
    }

    /// @brief Return the kind of the innermost enclosing loop.
    /// @pre hasActiveLoop() must be true.
    /// @return The LoopKind of the top of the loop stack.
    [[nodiscard]] SemanticAnalyzer::LoopKind currentLoop() const noexcept {
        assert(hasActiveLoop() && "no active loop available");
        return analyzer_->loopStack_.back();
    }

    /// @brief Create a RAII loop guard that pushes While onto the loop stack.
    /// @return A LoopGuard that pops the loop stack on destruction.
    semantic_analyzer_detail::ControlStmtContext::LoopGuard whileLoopGuard() {
        return {*analyzer_, SemanticAnalyzer::LoopKind::While};
    }

    /// @brief Create a RAII loop guard that pushes Do onto the loop stack.
    /// @return A LoopGuard that pops the loop stack on destruction.
    semantic_analyzer_detail::ControlStmtContext::LoopGuard doLoopGuard() {
        return {*analyzer_, SemanticAnalyzer::LoopKind::Do};
    }

    /// @brief Create a RAII loop guard that pushes For onto the loop stack.
    /// @return A LoopGuard that pops the loop stack on destruction.
    semantic_analyzer_detail::ControlStmtContext::LoopGuard forLoopGuard() {
        return {*analyzer_, SemanticAnalyzer::LoopKind::For};
    }

    /// @brief Create a RAII loop guard that pushes Sub onto the loop stack.
    /// @details Used when entering a SUB declaration body so that EXIT SUB
    ///          can be validated against the enclosing scope kind.
    /// @return A LoopGuard that pops the loop stack on destruction.
    semantic_analyzer_detail::ControlStmtContext::LoopGuard subLoopGuard() {
        return {*analyzer_, SemanticAnalyzer::LoopKind::Sub};
    }

    /// @brief Create a RAII loop guard that pushes Function onto the loop stack.
    /// @details Used when entering a FUNCTION declaration body so that
    ///          EXIT FUNCTION can be validated against the enclosing scope kind.
    /// @return A LoopGuard that pops the loop stack on destruction.
    semantic_analyzer_detail::ControlStmtContext::LoopGuard functionLoopGuard() {
        return {*analyzer_, SemanticAnalyzer::LoopKind::Function};
    }

    /// @brief Create a RAII guard that pushes a FOR variable onto the tracking stack.
    /// @details The guard pops the FOR variable on destruction, ensuring NEXT
    ///          validation sees the correct innermost FOR variable.
    /// @param name The name of the FOR loop counter variable.
    /// @return A ForLoopGuard that pops the variable on destruction.
    semantic_analyzer_detail::ControlStmtContext::ForLoopGuard trackForVariable(std::string name) {
        return {*analyzer_, std::move(name)};
    }

    /// @brief Convert an AST ExitStmt::LoopKind to the analyzer's LoopKind.
    /// @param kind The AST-level loop kind from an EXIT statement.
    /// @return The corresponding SemanticAnalyzer::LoopKind value.
    SemanticAnalyzer::LoopKind toLoopKind(ExitStmt::LoopKind kind) const noexcept {
        switch (kind) {
            case ExitStmt::LoopKind::For:
                return SemanticAnalyzer::LoopKind::For;
            case ExitStmt::LoopKind::While:
                return SemanticAnalyzer::LoopKind::While;
            case ExitStmt::LoopKind::Do:
                return SemanticAnalyzer::LoopKind::Do;
            case ExitStmt::LoopKind::Sub:
                return SemanticAnalyzer::LoopKind::Sub;
            case ExitStmt::LoopKind::Function:
                return SemanticAnalyzer::LoopKind::Function;
        }
        return SemanticAnalyzer::LoopKind::While;
    }

    /// @brief Return the BASIC keyword name for a loop kind (e.g. "FOR", "WHILE").
    /// @param kind The loop kind to convert to a human-readable name.
    /// @return A null-terminated C string with the BASIC keyword.
    const char *loopKindName(SemanticAnalyzer::LoopKind kind) const noexcept {
        switch (kind) {
            case SemanticAnalyzer::LoopKind::For:
                return "FOR";
            case SemanticAnalyzer::LoopKind::While:
                return "WHILE";
            case SemanticAnalyzer::LoopKind::Do:
                return "DO";
            case SemanticAnalyzer::LoopKind::Sub:
                return "SUB";
            case SemanticAnalyzer::LoopKind::Function:
                return "FUNCTION";
        }
        return "WHILE";
    }

    /// @brief Push a new lexical scope for block-structured statements.
    /// @return A RAII guard that pops the scope on destruction.
    ScopeTracker::ScopedScope pushScope() {
        return ScopeTracker::ScopedScope(analyzer_->scopes_);
    }

    /// @brief Check whether the FOR variable tracking stack is non-empty.
    /// @return True if at least one FOR variable is being tracked.
    [[nodiscard]] bool hasForVariable() const noexcept {
        return !analyzer_->forStack_.empty();
    }

    /// @brief Return the name of the innermost FOR loop variable.
    /// @return The variable name, or an empty view if no FOR loop is active.
    [[nodiscard]] std::string_view currentForVariable() const noexcept {
        if (analyzer_->forStack_.empty())
            return {};
        return analyzer_->forStack_.back();
    }

    /// @brief Pop the innermost FOR variable from the tracking stack.
    void popForVariable() {
        analyzer_->popForVariable();
    }

    /// @brief Install an error handler targeting the given line label.
    /// @param label The line-number label where ON ERROR GOTO branches.
    void installErrorHandler(int label) {
        analyzer_->installErrorHandler(label);
    }

    /// @brief Remove the currently active error handler.
    void clearErrorHandler() {
        analyzer_->clearErrorHandler();
    }

    /// @brief Check whether an ON ERROR GOTO handler is currently active.
    /// @return True if an error handler has been installed and not yet cleared.
    [[nodiscard]] bool hasActiveErrorHandler() const noexcept {
        return analyzer_->hasActiveErrorHandler();
    }

    /// @brief Check whether the analyzer is currently inside a SUB or FUNCTION body.
    /// @return True if a procedure scope is active.
    [[nodiscard]] bool hasActiveProcScope() const noexcept {
        return analyzer_->activeProcScope_ != nullptr;
    }

    /// @brief Check whether the active procedure scope is a FUNCTION body.
    /// @return True if semantic analysis is currently inside a FUNCTION.
    [[nodiscard]] bool isActiveFunction() const noexcept {
        return analyzer_->activeFunction_ != nullptr ||
               analyzer_->hasLoopOfKind(SemanticAnalyzer::LoopKind::Function);
    }

    /// @brief Check whether the main module contains at least one GOSUB.
    [[nodiscard]] bool mainHasGosub() const noexcept {
        return analyzer_->mainHasGosub();
    }

    /// @brief Check whether a loop of the specified kind exists on the loop stack.
    /// @param kind The loop kind to search for (For, While, Do, Sub, Function).
    /// @return True if the loop stack contains an entry matching kind.
    [[nodiscard]] bool hasLoopOfKind(SemanticAnalyzer::LoopKind kind) const noexcept {
        return analyzer_->hasLoopOfKind(kind);
    }

    /// @brief Access the diagnostic sink for emitting semantic errors and warnings.
    /// @return Reference to the SemanticDiagnostics instance.
    [[nodiscard]] SemanticDiagnostics &diagnostics() noexcept {
        return analyzer_->de;
    }

    /// @brief Resolve a loop variable name and register it as a definition.
    /// @param name The variable name to resolve (may be case-folded in place).
    void resolveLoopVariable(std::string &name) {
        analyzer_->resolveAndTrackSymbol(name, SemanticAnalyzer::SymbolKind::Definition);
    }

    /// @brief Resolve a symbol name as a read/reference.
    /// @param name The variable name to resolve (may be scope-mapped in place).
    void resolveSymbolRef(std::string &name) {
        analyzer_->resolveAndTrackSymbol(name, SemanticAnalyzer::SymbolKind::Reference);
    }

    /// @brief Look up the tracked semantic type of a variable.
    [[nodiscard]] std::optional<SemanticAnalyzer::Type> varType(const std::string &name) const {
        return analyzer_->lookupVarType(name);
    }

    /// @brief Set or refine a variable type while preserving procedure-scope rollback.
    /// @param name Variable name whose semantic type is updated.
    /// @param type New semantic type to record.
    void setVarType(const std::string &name, SemanticAnalyzer::Type type) {
        auto itType = analyzer_->varTypes_.find(name);
        if (analyzer_->activeProcScope_) {
            std::optional<SemanticAnalyzer::Type> previous;
            if (itType != analyzer_->varTypes_.end())
                previous = itType->second;
            analyzer_->activeProcScope_->noteVarTypeMutation(name, previous);
        }
        analyzer_->varTypes_[name] = type;
    }

    /// @brief Evaluate an expression and return its inferred type.
    /// @param expr The expression node to type-check.
    /// @return The inferred semantic type of the expression.
    SemanticAnalyzer::Type evaluateExpr(Expr &expr) {
        return analyzer_->visitExpr(expr);
    }

    /// @brief Recursively visit and type-check a statement.
    /// @param stmt The statement node to check.
    void visitStmt(Stmt &stmt) {
        analyzer_->visitStmt(stmt);
    }

    /// @brief Record that an implicit type conversion is applied to an expression.
    /// @param expr The expression being implicitly converted.
    /// @param target The target type of the implicit conversion.
    void markImplicitConversion(const Expr &expr, SemanticAnalyzer::Type target) {
        analyzer_->markImplicitConversion(expr, target);
    }

  private:
    SemanticAnalyzer *analyzer_{nullptr};
    semantic_analyzer_detail::ControlStmtContext stmtContext_;
    size_t loopDepth_ = 0;
    size_t forDepth_ = 0;
};

/// @brief Context wrapper for expression checkers.
/// @details Provides helpers for evaluating child expressions and recording
///          implicit conversions while exposing diagnostics. visitExpr expects
///          mutable nodes, so evaluate() internally casts away constness when
///          callers only have const handles.
class ExprCheckContext {
  public:
    /// @brief Construct an expression check context wrapping the given analyzer.
    /// @param analyzer The semantic analyzer providing type tables and diagnostics.
    explicit ExprCheckContext(SemanticAnalyzer &analyzer) noexcept : analyzer_(&analyzer) {}

    /// @brief Access the wrapped semantic analyzer (mutable).
    /// @return Reference to the underlying SemanticAnalyzer instance.
    [[nodiscard]] SemanticAnalyzer &analyzer() noexcept {
        return *analyzer_;
    }

    /// @brief Access the wrapped semantic analyzer (const).
    /// @return Const reference to the underlying SemanticAnalyzer instance.
    [[nodiscard]] const SemanticAnalyzer &analyzer() const noexcept {
        return *analyzer_;
    }

    /// @brief Evaluate an expression and return its inferred type.
    /// @param expr The expression node to type-check (mutable for cast insertion).
    /// @return The inferred semantic type of the expression.
    SemanticAnalyzer::Type evaluate(Expr &expr) {
        return analyzer_->visitExpr(expr);
    }

    /// @brief Evaluate a const expression by casting away constness.
    /// @details The analyzer's visitExpr requires a mutable reference because it
    ///          may insert implicit cast wrappers. This overload provides a
    ///          convenient entry point when the caller only has a const handle.
    /// @param expr The expression node to type-check.
    /// @return The inferred semantic type of the expression.
    SemanticAnalyzer::Type evaluate(const Expr &expr) {
        return analyzer_->visitExpr(const_cast<Expr &>(expr));
    }

    /// @brief Record that an implicit type conversion is applied to an expression.
    /// @param expr The expression being implicitly converted.
    /// @param target The target type of the implicit conversion.
    void markImplicitConversion(const Expr &expr, SemanticAnalyzer::Type target) {
        analyzer_->markImplicitConversion(expr, target);
    }

    /// @brief Access the diagnostic sink for emitting semantic errors and warnings.
    /// @return Reference to the SemanticDiagnostics instance.
    [[nodiscard]] SemanticDiagnostics &diagnostics() noexcept {
        return analyzer_->de;
    }

    /// @brief Resolve a callee name to its procedure signature.
    /// @details Looks up the name in the procedure signature table, filtering
    ///          by the specified kind (Sub or Function).
    /// @param expr The call expression whose callee name is resolved.
    /// @param kind Whether to search for Sub or Function signatures.
    /// @return Pointer to the matching ProcSignature, or nullptr if not found.
    const ProcSignature *resolveCallee(const CallExpr &expr, ProcSignature::Kind kind) {
        return analyzer_->resolveCallee(expr, kind);
    }

    /// @brief Validate the argument types of a call expression against a signature.
    /// @details Checks each actual argument type against the corresponding formal
    ///          parameter type and inserts implicit conversions where allowed.
    /// @param expr The call expression with arguments to validate.
    /// @param sig The resolved procedure signature to check against (may be null).
    /// @return Vector of inferred types for each actual argument.
    std::vector<SemanticAnalyzer::Type> checkCallArgs(const CallExpr &expr,
                                                      const ProcSignature *sig) {
        return analyzer_->checkCallArgs(expr, sig);
    }

    /// @brief Infer the return type of a call expression from its resolved signature.
    /// @param expr The call expression to infer a return type for.
    /// @param sig The resolved procedure signature (may be null for unknown callees).
    /// @return The inferred return type of the call.
    SemanticAnalyzer::Type inferCallType(const CallExpr &expr, const ProcSignature *sig) {
        return analyzer_->inferCallType(expr, sig);
    }

    // =========================================================================
    // Variable analysis helpers
    // =========================================================================

    /// @brief Resolve a symbol name and record it with the given definition/reference kind.
    /// @param name The symbol name to resolve (may be case-folded in place).
    /// @param kind Whether this is a definition or a reference.
    void resolveAndTrackSymbol(std::string &name, SemanticAnalyzer::SymbolKind kind) {
        analyzer_->resolveAndTrackSymbol(name, kind);
    }

    /// @brief Resolve a symbol name and record it as a reference (read-use).
    /// @param name The symbol name to resolve (may be case-folded in place).
    void resolveAndTrackSymbolRef(std::string &name) {
        analyzer_->resolveAndTrackSymbol(name, SemanticAnalyzer::SymbolKind::Reference);
    }

    /// @brief Check whether a symbol with the given name has been declared.
    /// @param name The symbol name to look up (case-sensitive after folding).
    /// @return True if the symbol exists in the symbol table.
    [[nodiscard]] bool hasSymbol(const std::string &name) const {
        return analyzer_->symbols_.count(name) != 0;
    }

    /// @brief Check whether the current lexical scope binds a source name.
    [[nodiscard]] bool hasScopedBinding(const std::string &name) const {
        return analyzer_->scopes_.resolve(name).has_value();
    }

    /// @brief Check whether a name refers to a declared enum type.
    /// @param name The name to look up in the OOP index enum table.
    /// @return True if the name matches a registered enum.
    [[nodiscard]] bool isEnumName(const std::string &name) const {
        const auto &enums = analyzer_->oopIndex_.enums();
        return enums.find(name) != enums.end();
    }

    /// @brief Access the full set of declared symbol names.
    /// @return Const reference to the unordered set of all known symbol names.
    [[nodiscard]] const std::unordered_set<std::string> &symbols() const noexcept {
        return analyzer_->symbols_;
    }

    /// @brief Look up the declared type of a variable by name.
    /// @param name The variable name to look up.
    /// @return The variable's type if found, or std::nullopt if undeclared.
    [[nodiscard]] std::optional<SemanticAnalyzer::Type> varType(const std::string &name) const {
        auto it = analyzer_->varTypes_.find(name);
        if (it != analyzer_->varTypes_.end())
            return it->second;
        return std::nullopt;
    }

    // =========================================================================
    // Array analysis helpers
    // =========================================================================

    /// @brief Check whether an array with the given name has been declared.
    /// @param name The array name to look up.
    /// @return True if the array exists in the array metadata table.
    [[nodiscard]] bool hasArray(const std::string &name) const {
        return analyzer_->arrays_.count(name) != 0;
    }

    /// @brief Look up the metadata (dimensions, element type) for a declared array.
    /// @param name The array name to look up.
    /// @return Pointer to the ArrayMetadata if found, or nullptr if undeclared.
    [[nodiscard]] const ArrayMetadata *arrayMetadata(const std::string &name) const {
        auto it = analyzer_->arrays_.find(name);
        if (it != analyzer_->arrays_.end())
            return &it->second;
        return nullptr;
    }

    /// @brief Insert an implicit cast wrapper around an expression.
    /// @details Wraps the expression AST node with a cast to the target type,
    ///          used for automatic widening (e.g. Int to Float).
    /// @param expr The expression to wrap with a cast.
    /// @param target The target type for the implicit cast.
    void insertImplicitCast(Expr &expr, SemanticAnalyzer::Type target) {
        analyzer_->insertImplicitCast(expr, target);
    }

  private:
    SemanticAnalyzer *analyzer_{nullptr};
};

/// @brief Emit a generic type-mismatch error diagnostic.
/// @param diagnostics The diagnostic sink to emit the error into.
/// @param code The diagnostic code (e.g. "B2001") identifying the error category.
/// @param loc Source location of the mismatched expression.
/// @param length Token length for the underline span.
/// @param message Human-readable error message describing the type mismatch.
inline void emitTypeMismatch(SemanticDiagnostics &diagnostics,
                             std::string code,
                             il::support::SourceLoc loc,
                             uint32_t length,
                             std::string message) {
    diagnostics.emit(
        il::support::Severity::Error, std::move(code), loc, length, std::move(message));
}

/// @brief Emit an operand type-mismatch error for a binary expression.
/// @details Convenience wrapper that formats a standard "operand type mismatch"
///          message using the binary expression's source location.
/// @param diagnostics The diagnostic sink to emit the error into.
/// @param expr The binary expression whose operands have mismatched types.
/// @param diagId The diagnostic code to use; if empty, no diagnostic is emitted.
inline void emitOperandTypeMismatch(SemanticDiagnostics &diagnostics,
                                    const BinaryExpr &expr,
                                    std::string_view diagId) {
    if (diagId.empty())
        return;

    emitTypeMismatch(diagnostics, std::string(diagId), expr.loc, 1, "operand type mismatch");
}

/// @brief Emit a divide-by-zero error for a binary expression.
/// @details Used when constant folding detects a zero divisor in integer
///          division or modulo operations.
/// @param diagnostics The diagnostic sink to emit the error into.
/// @param expr The binary expression with the zero divisor.
inline void emitDivideByZero(SemanticDiagnostics &diagnostics, const BinaryExpr &expr) {
    diagnostics.emit(il::support::Severity::Error, "B2002", expr.loc, 1, "divide by zero");
}

// =========================================================================
// Per-construct dispatcher entry points implemented in dedicated translation
// units.  Each function accepts the shared analyzer state and the specific
// AST node, validates its constraints, records diagnostics, and propagates
// type information back through the analyzer tables.
// =========================================================================

/// @brief Validate a condition expression used by IF, WHILE, DO, etc.
/// @details Ensures the expression produces a boolean-compatible type and
///          emits a diagnostic if the condition is non-boolean or unknown.
/// @param analyzer The semantic analyzer providing type context and diagnostics.
/// @param expr The condition expression to validate (may be mutated for casts).
void checkConditionExpr(SemanticAnalyzer &analyzer, Expr &expr);

/// @brief Analyze an IF/ELSEIF/ELSE statement chain.
/// @details Validates each condition expression, type-checks branches, and
///          verifies that the control-flow structure is well-formed.
/// @param analyzer The semantic analyzer providing scoping and type context.
/// @param stmt The IF statement node containing conditions and branch bodies.
void analyzeIf(SemanticAnalyzer &analyzer, const IfStmt &stmt);

/// @brief Analyze a SELECT CASE statement including all CASE arms.
/// @details Classifies the selector expression (numeric vs. string), validates
///          each CASE arm for type consistency, detects duplicate and overlapping
///          labels, and checks for multiple CASE ELSE clauses.
/// @param analyzer The semantic analyzer providing scoping and type context.
/// @param stmt The SELECT CASE statement node.
void analyzeSelectCase(SemanticAnalyzer &analyzer, const SelectCaseStmt &stmt);

/// @brief Analyze the body of a single SELECT CASE arm.
/// @details Opens a new scope for the arm body and visits each child statement.
/// @param analyzer The semantic analyzer providing scoping and type context.
/// @param body The list of statements within the CASE arm.
void analyzeSelectCaseBody(SemanticAnalyzer &analyzer, const std::vector<StmtPtr> &body);

/// @brief Analyze a WHILE loop statement.
/// @details Validates the loop condition, pushes a While loop guard onto the
///          loop stack, and recursively checks the loop body.
/// @param analyzer The semantic analyzer providing loop tracking and type context.
/// @param stmt The WHILE statement node.
void analyzeWhile(SemanticAnalyzer &analyzer, const WhileStmt &stmt);

/// @brief Analyze a DO/LOOP statement with optional WHILE or UNTIL condition.
/// @details Pushes a Do loop guard, validates the pre- or post-condition if
///          present, and recursively checks the loop body.
/// @param analyzer The semantic analyzer providing loop tracking and type context.
/// @param stmt The DO statement node.
void analyzeDo(SemanticAnalyzer &analyzer, const DoStmt &stmt);

/// @brief Analyze a FOR/NEXT loop with start, end, and optional STEP values.
/// @details Validates that the loop variable is numeric, evaluates start/end/step
///          expressions, pushes the FOR variable onto the tracking stack, and
///          recursively checks the loop body.
/// @param analyzer The semantic analyzer providing loop and variable tracking.
/// @param stmt The FOR statement node (may be mutated for implicit casts).
void analyzeFor(SemanticAnalyzer &analyzer, ForStmt &stmt);

/// @brief Analyze a FOR EACH loop over a collection.
/// @details Validates that the iterable expression is a supported collection
///          type and that the element variable has a compatible type.
/// @param analyzer The semantic analyzer providing loop and variable tracking.
/// @param stmt The FOR EACH statement node (may be mutated for implicit casts).
void analyzeForEach(SemanticAnalyzer &analyzer, ForEachStmt &stmt);

/// @brief Analyze an EXIT statement (EXIT FOR, EXIT WHILE, EXIT DO, etc.).
/// @details Verifies that the EXIT kind matches an enclosing loop of the
///          specified type and emits a diagnostic if no matching loop exists.
/// @param analyzer The semantic analyzer providing loop stack context.
/// @param stmt The EXIT statement node.
void analyzeExit(SemanticAnalyzer &analyzer, const ExitStmt &stmt);

/// @brief Analyze a GOTO statement targeting a line-number label.
/// @details Records the label reference for later validation against defined
///          labels and emits a diagnostic if the label is out of scope.
/// @param analyzer The semantic analyzer providing label tracking.
/// @param stmt The GOTO statement node.
void analyzeGoto(SemanticAnalyzer &analyzer, const GotoStmt &stmt);

/// @brief Analyze a GOSUB statement targeting a subroutine label.
/// @details Records the label reference and validates that GOSUB is not used
///          inside a structured SUB/FUNCTION without matching RETURN.
/// @param analyzer The semantic analyzer providing label and scope tracking.
/// @param stmt The GOSUB statement node.
void analyzeGosub(SemanticAnalyzer &analyzer, const GosubStmt &stmt);

/// @brief Analyze an ON ERROR GOTO statement for structured error handling.
/// @details Installs the error handler label in the analyzer state and validates
///          that the target label is defined or is the special label 0 (clear).
/// @param analyzer The semantic analyzer providing error handler tracking.
/// @param stmt The ON ERROR GOTO statement node.
void analyzeOnErrorGoto(SemanticAnalyzer &analyzer, const OnErrorGoto &stmt);

/// @brief Analyze a NEXT statement closing a FOR loop.
/// @details Validates that the NEXT variable matches the innermost FOR variable
///          and pops the FOR tracking stack.
/// @param analyzer The semantic analyzer providing FOR variable tracking.
/// @param stmt The NEXT statement node.
void analyzeNext(SemanticAnalyzer &analyzer, const NextStmt &stmt);

/// @brief Analyze a RESUME statement inside an error handler.
/// @details Validates that RESUME appears inside an active error handler context
///          and checks RESUME NEXT or RESUME <label> targets.
/// @param analyzer The semantic analyzer providing error handler context.
/// @param stmt The RESUME statement node.
void analyzeResume(SemanticAnalyzer &analyzer, const Resume &stmt);

/// @brief Analyze a RETURN statement from a FUNCTION or SUB.
/// @details Validates the return value type against the enclosing procedure's
///          declared return type and inserts implicit conversions if needed.
/// @param analyzer The semantic analyzer providing procedure return context.
/// @param stmt The RETURN statement node (may be mutated for implicit casts).
void analyzeReturn(SemanticAnalyzer &analyzer, ReturnStmt &stmt);

/// @brief Analyze a unary expression (NOT, negation, etc.).
/// @details Validates that the operand type supports the unary operator and
///          determines the result type.
/// @param analyzer The semantic analyzer providing type context.
/// @param expr The unary expression node.
/// @return The inferred result type of the unary operation.
SemanticAnalyzer::Type analyzeUnaryExpr(SemanticAnalyzer &analyzer, const UnaryExpr &expr);

/// @brief Analyze a binary expression (arithmetic, comparison, logical, string).
/// @details Validates operand types, performs implicit widening (Int→Float),
///          checks for divide-by-zero on constant divisors, and determines the
///          result type.
/// @param analyzer The semantic analyzer providing type context.
/// @param expr The binary expression node.
/// @return The inferred result type of the binary operation.
SemanticAnalyzer::Type analyzeBinaryExpr(SemanticAnalyzer &analyzer, const BinaryExpr &expr);

/// @brief Analyze a function or subroutine call expression.
/// @details Resolves the callee by name, validates argument count and types
///          against the procedure signature, and determines the return type.
/// @param analyzer The semantic analyzer providing symbol and signature tables.
/// @param expr The call expression node.
/// @return The inferred return type of the called procedure.
SemanticAnalyzer::Type analyzeCallExpr(SemanticAnalyzer &analyzer, const CallExpr &expr);

/// @brief Analyze a variable reference expression.
/// @details Resolves the variable name in the current scope, records the
///          reference for used-variable tracking, and returns the variable's type.
/// @param analyzer The semantic analyzer providing variable tables.
/// @param expr The variable expression node (may be mutated for resolution).
/// @return The inferred type of the referenced variable.
SemanticAnalyzer::Type analyzeVarExpr(SemanticAnalyzer &analyzer, VarExpr &expr);

/// @brief Analyze an array element access expression.
/// @details Validates that the name refers to a declared array, checks the
///          index count against the array's declared dimensions, and validates
///          index types.
/// @param analyzer The semantic analyzer providing array metadata.
/// @param expr The array access expression node (may be mutated for casts).
/// @return The element type of the array.
SemanticAnalyzer::Type analyzeArrayExpr(SemanticAnalyzer &analyzer, ArrayExpr &expr);

/// @brief Analyze an LBOUND() intrinsic call on an array.
/// @details Validates that the argument names a declared array and that the
///          optional dimension argument is a valid integer constant.
/// @param analyzer The semantic analyzer providing array metadata.
/// @param expr The LBOUND expression node (may be mutated for resolution).
/// @return Type::Int, the return type of the LBOUND intrinsic.
SemanticAnalyzer::Type analyzeLBoundExpr(SemanticAnalyzer &analyzer, LBoundExpr &expr);

/// @brief Analyze a UBOUND() intrinsic call on an array.
/// @details Validates that the argument names a declared array and that the
///          optional dimension argument is a valid integer constant.
/// @param analyzer The semantic analyzer providing array metadata.
/// @param expr The UBOUND expression node (may be mutated for resolution).
/// @return Type::Int, the return type of the UBOUND intrinsic.
SemanticAnalyzer::Type analyzeUBoundExpr(SemanticAnalyzer &analyzer, UBoundExpr &expr);

} // namespace il::frontends::basic::sem
