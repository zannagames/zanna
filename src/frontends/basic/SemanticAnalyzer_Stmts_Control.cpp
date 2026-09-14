//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Control.cpp
// Purpose: Dispatch entry points for control-flow statement analysis in the
//          BASIC semantic analyzer.
// Key invariants:
//   - Each helper delegates to sem::check_* modules that maintain loop/label
//     stacks via ControlCheckContext and assert balance on exit.
//   - CATCH and USING variables are renamed to their scoped unique names, the
//     same names references in their bodies receive.
// Ownership/Lifetime:
//   - Borrowed SemanticAnalyzer state only.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Stmts_Control.cpp
/// @brief Implements control-flow statement entry points and scoped TRY/USING checks.
/// @details Most routines adapt the main analyzer to modular `sem::` checkers.
///          TRY/CATCH and resource USING manage lexical bindings directly;
///          RETURN adds declared-result compatibility warnings; error-handler
///          accessors maintain procedure-local handler state.

#include "frontends/basic/SemanticAnalyzer_Stmts_Control.hpp"

#include "frontends/basic/sem/Check_Common.hpp"
#include "frontends/basic/sem/Check_SelectDetail.hpp"

namespace il::frontends::basic {

/// @brief Delegates boolean-condition validation to the control checker.
/// @param expr Mutable condition that may receive conversion metadata.
void SemanticAnalyzer::checkConditionExpr(Expr &expr) {
    sem::checkConditionExpr(*this, expr);
}

/// @brief Delegates IF condition and branch analysis.
/// @param stmt IF/ELSEIF/ELSE tree borrowed during traversal.
void SemanticAnalyzer::analyzeIf(const IfStmt &stmt) {
    sem::analyzeIf(*this, stmt);
}

/// @brief Delegates full SELECT CASE validation and body traversal.
/// @details The checker classifies the selector, validates arms cumulatively,
///          stops at the first invalid arm, and analyzes each accepted arm in a
///          fresh lexical scope. A non-empty else body is analyzed last.
/// @param stmt SELECT CASE statement to inspect.
void SemanticAnalyzer::analyzeSelectCase(const SelectCaseStmt &stmt) {
    sem::analyzeSelectCase(*this, stmt);
}

/// @brief Analyzes a CASE body in a fresh lexical scope.
/// @param body Ordered statement pointers; null entries are skipped.
void SemanticAnalyzer::analyzeSelectCaseBody(const std::vector<StmtPtr> &body) {
    sem::analyzeSelectCaseBody(*this, body);
}

SemanticAnalyzer::SelectCaseSelectorInfo
/// @brief Classifies a SELECT CASE selector as integer, string, or invalid.
/// @details A missing or unknown selector produces an all-false nonfatal
///          descriptor. Integer records an integer conversion; string records
///          string mode. Every other known type emits the SELECT selector
///          diagnostic and marks the result fatal.
/// @param stmt Statement whose selector is evaluated.
/// @return Selector mode and fatal-error flags.
SemanticAnalyzer::classifySelectCaseSelector(const SelectCaseStmt &stmt) {
    sem::ControlCheckContext context(*this);
    return sem::detail::classifySelectCaseSelector(context, stmt);
}

/// @brief Validates one CASE arm against cumulative SELECT state.
/// @details Handles CASE ELSE counting, mixed label kinds, selector/label
///          compatibility, duplicate labels, range validity, and interval
///          collisions while updating @p ctx.
/// @param arm Arm to validate.
/// @param ctx Mutable cumulative state shared by earlier/later arms.
/// @return @c true when no arm diagnostic was emitted.
bool SemanticAnalyzer::validateSelectCaseArm(const CaseArm &arm, SelectCaseArmContext &ctx) {
    return sem::detail::validateSelectCaseArm(arm, ctx);
}

/// @brief Validates and records the string labels in one CASE arm.
/// @details Rejects string labels for numeric selectors, checks cross-arm label
///          kind consistency, and detects duplicate strings using the context's
///          exact string set.
/// @param arm Arm whose string labels are inspected.
/// @param ctx Mutable cumulative SELECT state.
/// @return @c true when all string checks pass.
bool SemanticAnalyzer::validateSelectCaseStringArm(const CaseArm &arm, SelectCaseArmContext &ctx) {
    return sem::detail::validateSelectCaseStringArm(arm, ctx);
}

/// @brief Validates and records numeric CASE labels, ranges, and relations.
/// @details Rejects numeric labels for string selectors, enforces signed 32-bit
///          bounds and range ordering, and detects collisions among exact
///          labels, inclusive ranges, and relational intervals.
/// @param arm Arm whose numeric predicates are inspected.
/// @param ctx Mutable cumulative SELECT state.
/// @return @c true when all numeric checks pass.
bool SemanticAnalyzer::validateSelectCaseNumericArm(const CaseArm &arm, SelectCaseArmContext &ctx) {
    return sem::detail::validateSelectCaseNumericArm(arm, ctx);
}

/// @brief Analyzes TRY, CATCH, and FINALLY bodies with catch binding state.
/// @details Visits TRY in the surrounding lexical scope and warns with `B3203`
///          only when all three bodies are empty. It then pushes one scope,
///          optionally declares a unique integer catch variable while logging
///          procedure rollback state, and visits CATCH followed by FINALLY.
///          Under the current lifetime of the scope guard, FINALLY shares the
///          catch scope and can resolve the catch binding. Lowering performs
///          catch-variable initialization.
/// @param stmt Structured handler statement to analyze.
void SemanticAnalyzer::visit(TryCatchStmt &stmt) {
    // Analyze TRY body under the existing scope.
    for (const auto &st : stmt.tryBody)
        if (st)
            visitStmt(*st);

    // Warn on a completely empty TRY statement (policy: allow with warning).
    if (stmt.tryBody.empty() && stmt.catchBody.empty() && stmt.finallyBody.empty()) {
        de.emit(il::support::Severity::Warning,
                "B3203",
                stmt.header.begin,
                1,
                std::string{"empty TRY, CATCH, and FINALLY bodies"});
    }

    // Begin a new scope for the CATCH body; the optional catch variable is local to it.
    ScopeTracker::ScopedScope catchScope(scopes_);

    if (stmt.catchVar && !stmt.catchVar->empty()) {
        const std::string name = *stmt.catchVar;

        // Forbid duplicate declaration in the same (catch) scope.
        if (scopes_.isDeclaredInCurrentScope(name)) {
            std::string msg = "duplicate local '" + name + "'";
            de.emit(il::support::Severity::Error,
                    "B1013",
                    stmt.header.end.isValid() ? stmt.header.end : stmt.header.begin,
                    static_cast<uint32_t>(name.size()),
                    std::move(msg));
        } else {
            // Declare a local binding for the catch variable in this scope. Lowering
            // binds the error code to the statement's name, which references share.
            std::string unique = scopes_.declareLocal(name);
            stmt.catchVar = unique;

            // Track symbol and force INTEGER (i64) type; initialized during lowering.
            auto insertResult = symbols_.insert(unique);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteSymbolInserted(unique);

            auto itType = varTypes_.find(unique);
            if (activeProcScope_) {
                std::optional<Type> previous;
                if (itType != varTypes_.end())
                    previous = itType->second;
                activeProcScope_->noteVarTypeMutation(unique, previous);
            }
            varTypes_[unique] = Type::Int; // INTEGER (i64)
        }
    }

    // Analyze CATCH body within the new scope.
    for (const auto &st : stmt.catchBody)
        if (st)
            visitStmt(*st);

    // Analyze FINALLY body under the surrounding scope. It runs regardless of
    // whether the TRY or CATCH path completed normally.
    for (const auto &st : stmt.finallyBody)
        if (st)
            visitStmt(*st);
}

/// @brief Validates a scoped resource USING statement.
/// @details Pushes a lexical scope, declares a non-empty resource name as an
///          object local while logging procedure rollback state, then analyzes
///          the initializer and body inside that same scope. A known
///          non-object initializer emits `B3204`; unknown is tolerated for
///          recovery. Same-scope duplicate names emit `B1006`.
/// @param stmt Resource declaration, initializer, and body to analyze.
void SemanticAnalyzer::visit(UsingStmt &stmt) {
    // Begin a new scope for the USING body; the resource variable is local to it.
    ScopeTracker::ScopedScope usingScope(scopes_);

    if (!stmt.varName.empty()) {
        const std::string name = stmt.varName;

        // Forbid duplicate declaration in the same scope.
        if (scopes_.isDeclaredInCurrentScope(name)) {
            std::string msg = "duplicate local '" + name + "'";
            de.emit(il::support::Severity::Error,
                    "B1013",
                    stmt.loc,
                    static_cast<uint32_t>(name.size()),
                    std::move(msg));
        } else {
            // Declare a local binding for the resource variable in this scope. Lowering
            // stores the resource under the statement's name, which references share.
            std::string unique = scopes_.declareLocal(name);
            stmt.varName = unique;

            // Track symbol and set Object type for the resource.
            auto insertResult = symbols_.insert(unique);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteSymbolInserted(unique);

            auto itType = varTypes_.find(unique);
            if (activeProcScope_) {
                std::optional<Type> previous;
                if (itType != varTypes_.end())
                    previous = itType->second;
                activeProcScope_->noteVarTypeMutation(unique, previous);
            }
            varTypes_[unique] = Type::Object; // Object pointer type
        }
    }

    // Analyze the initializer expression if present. USING manages disposable
    // resources, so scalar initializers are rejected instead of lowered as
    // object cleanup.
    if (stmt.initExpr) {
        Type initType = visitExpr(*stmt.initExpr);
        if (initType != Type::Unknown && initType != Type::Object) {
            de.emit(il::support::Severity::Error,
                    "B3204",
                    stmt.initExpr->loc,
                    1,
                    std::string{"USING initializer must produce an object/resource"});
        }
    }

    // Analyze the USING body within the new scope.
    for (const auto &st : stmt.body)
        if (st)
            visitStmt(*st);
}

/// @brief Delegates WHILE condition/body and loop-stack validation.
/// @param stmt WHILE statement to analyse.
void SemanticAnalyzer::analyzeWhile(const WhileStmt &stmt) {
    sem::analyzeWhile(*this, stmt);
}

/// @brief Delegates DO/LOOP condition/body and loop-stack validation.
/// @param stmt DO statement node.
void SemanticAnalyzer::analyzeDo(const DoStmt &stmt) {
    sem::analyzeDo(*this, stmt);
}

/// @brief Delegates numeric FOR typing, binding, and body validation.
/// @param stmt FOR statement being analysed.
void SemanticAnalyzer::analyzeFor(ForStmt &stmt) {
    sem::analyzeFor(*this, stmt);
}

/// @brief Delegates FOR EACH iterator/container and body validation.
/// @param stmt FOR EACH statement being analysed.
void SemanticAnalyzer::analyzeForEach(ForEachStmt &stmt) {
    sem::analyzeForEach(*this, stmt);
}

/// @brief Delegates GOTO target recording/validation.
/// @param stmt GOTO statement node.
void SemanticAnalyzer::analyzeGoto(const GotoStmt &stmt) {
    sem::analyzeGoto(*this, stmt);
}

/// @brief Delegates GOSUB target and protocol validation.
/// @param stmt GOSUB statement node.
void SemanticAnalyzer::analyzeGosub(const GosubStmt &stmt) {
    sem::analyzeGosub(*this, stmt);
}

/// @brief Delegates ON ERROR target/handler-state validation.
/// @param stmt ON ERROR GOTO statement to analyse.
void SemanticAnalyzer::analyzeOnErrorGoto(const OnErrorGoto &stmt) {
    sem::analyzeOnErrorGoto(*this, stmt);
}

/// @brief Delegates NEXT/FOR pairing validation.
/// @param stmt NEXT statement to process.
void SemanticAnalyzer::analyzeNext(const NextStmt &stmt) {
    sem::analyzeNext(*this, stmt);
}

/// @brief Delegates EXIT target validation against active construct kinds.
/// @param stmt EXIT statement to validate.
void SemanticAnalyzer::analyzeExit(const ExitStmt &stmt) {
    sem::analyzeExit(*this, stmt);
}

/// @brief Delegates RESUME validation against active handler state.
/// @param stmt RESUME statement node.
void SemanticAnalyzer::analyzeResume(const Resume &stmt) {
    sem::analyzeResume(*this, stmt);
}

/// @brief Validates RETURN control context and the FUNCTION's scalar result type.
/// @details The shared checker handles procedure versus top-level GOSUB return
///          semantics and may mutate @c isGosubReturn or clear an active error
///          handler. A present value is always visited. Inside a FUNCTION with a
///          scalar result (declared by AS, by name suffix, or the INTEGER default),
///          a string returned from a numeric or boolean result, anything but a string
///          from a string result, or a FLOAT from a BOOLEAN result is error `B4010`.
///          Other numeric and boolean values convert to the result type when the
///          RETURN is lowered.
/// @param stmt Mutable RETURN statement and optional value.
void SemanticAnalyzer::analyzeReturn(ReturnStmt &stmt) {
    sem::analyzeReturn(*this, stmt);

    if (!stmt.value)
        return;

    // Always visit the return expression to resolve variable names, even for
    // object-returning functions where we can't check type compatibility.
    auto valueType = visitExpr(*stmt.value);

    auto expected = activeFunctionResultType();
    if (!expected || valueType == Type::Unknown)
        return;

    if (!semantic_analyzer_detail::functionResultAccepts(*expected, valueType)) {
        std::string msg = "RETURN expression type ";
        msg += semantic_analyzer_detail::semanticTypeName(valueType);
        msg += " does not match FUNCTION result type ";
        msg += semantic_analyzer_detail::semanticTypeName(*expected);
        de.emit(il::support::Severity::Error, "B4010", stmt.value->loc, 1, std::move(msg));
    }
}

/// @brief Accepts END without additional semantic state changes.
/// @details Program termination semantics are handled by later stages.
void SemanticAnalyzer::analyzeEnd(const EndStmt &) {
    // nothing
}

/// @brief Reports whether the body being analyzed contains ON ERROR GOTO <label>.
/// @return @c true when the procedure or module body selects a handler anywhere.
bool SemanticAnalyzer::procedureHasOnError() const noexcept {
    return procedureHasOnError_;
}

} // namespace il::frontends::basic
