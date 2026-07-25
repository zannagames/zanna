//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/Check_Loops.cpp
// Purpose: Validate BASIC loop constructs and maintain the semantic analyzer's
//          loop/label bookkeeping while emitting targeted diagnostics.
// Key invariants:
//   * ControlCheckContext maintains loop stacks and scope guards to mirror the
//     runtime nesting structure; every helper must push/pop correctly.
//   * EXIT/NEXT statements verify they match an active loop, ensuring the
//     resulting control flow remains well-structured.
//   * Condition expressions are validated with shared helpers so diagnostics are
//     consistent across IF and loop constructs.
// References: docs/tutorials/basic-tutorial.md#loops,
//             docs/internals/codemap/basic.md#semantic-analyzer
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Semantic checks for loop constructs (WHILE, DO, FOR, NEXT, EXIT).
/// @details Applies condition validation, loop stack management, and loop
///          variable tracking using the shared control-flow context helpers.
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/sem/Check_Common.hpp"

#include <string>

namespace il::frontends::basic::sem {

namespace {

/// @brief Determine whether a semantic type is valid in a numeric loop role.
/// @details Delegates to the analyzer's shared numeric classification so FOR
///          variables, bounds, and STEP expressions follow the same rules as
///          other numeric expressions. Unknown is accepted by that classifier
///          to avoid duplicating an earlier diagnostic.
/// @param type Semantic type to classify.
/// @return True when @p type is numeric or intentionally unresolved.
bool isNumericLoopType(SemanticAnalyzer::Type type) noexcept {
    return semantic_analyzer_detail::isNumericSemanticType(type);
}

/// @brief Emit the standard type-mismatch diagnostic for a numeric loop field.
/// @details Builds a context-specific message naming the invalid FOR component
///          and its inferred type, then reports diagnostic B2001 at the supplied
///          source span.
/// @param context Control-flow context that owns the diagnostic sink.
/// @param loc Location of the invalid loop variable or expression.
/// @param width Width, in source characters, to highlight.
/// @param what Human-readable name of the loop component being checked.
/// @param type Inferred non-numeric semantic type.
void emitLoopTypeError(ControlCheckContext &context,
                       il::support::SourceLoc loc,
                       uint32_t width,
                       const std::string &what,
                       SemanticAnalyzer::Type type) {
    std::string msg = what;
    msg += " must be numeric, got ";
    msg += semantic_analyzer_detail::semanticTypeName(type);
    context.diagnostics().emit(il::support::Severity::Error, "B2001", loc, width, std::move(msg));
}

/// @brief Map a supported BASIC array type to its element type.
/// @details FOR EACH uses the returned scalar type to infer or validate the
///          iteration variable. Non-array and currently unsupported array kinds
///          have no mapping.
/// @param arrayType Semantic type assigned to the iteration source.
/// @return The scalar element type, or std::nullopt when @p arrayType is not a
///         supported array type.
std::optional<SemanticAnalyzer::Type> arrayElementType(SemanticAnalyzer::Type arrayType) {
    using Type = SemanticAnalyzer::Type;
    switch (arrayType) {
        case Type::ArrayInt:
            return Type::Int;
        case Type::ArrayString:
            return Type::String;
        case Type::ArrayObject:
            return Type::Object;
        default:
            return std::nullopt;
    }
}

} // namespace

/// @brief Validate a WHILE loop and analyse its body.
///
/// The helper ensures the loop condition is type-checked (if present) and then
/// visits each body statement within a new scope.  It acquires a loop guard from
/// @ref ControlCheckContext to record that a WHILE loop is active so nested EXIT
/// statements can target it accurately.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the WHILE statement.
void analyzeWhile(SemanticAnalyzer &analyzer, const WhileStmt &stmt) {
    ControlCheckContext context(analyzer);
    if (stmt.cond)
        checkConditionExpr(context.analyzer(), *stmt.cond);

    [[maybe_unused]] auto loopGuard = context.whileLoopGuard();
    [[maybe_unused]] auto scope = context.pushScope();
    for (const auto &bodyStmt : stmt.body) {
        if (!bodyStmt)
            continue;
        context.visitStmt(*bodyStmt);
    }
}

/// @brief Validate a DO[/LOOP] construct, handling both pre- and post-test forms.
///
/// The helper checks the condition when it appears, sets up loop/scope guards,
/// and walks the loop body.  When the condition occurs after the body the same
/// validation logic runs again to mirror BASIC's post-test semantics.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the DO statement.
void analyzeDo(SemanticAnalyzer &analyzer, const DoStmt &stmt) {
    ControlCheckContext context(analyzer);
    const auto checkCond = [&]() {
        if (stmt.cond)
            checkConditionExpr(context.analyzer(), *stmt.cond);
    };

    if (stmt.testPos == DoStmt::TestPos::Pre)
        checkCond();

    [[maybe_unused]] auto loopGuard = context.doLoopGuard();
    {
        [[maybe_unused]] auto scope = context.pushScope();
        for (const auto &bodyStmt : stmt.body) {
            if (!bodyStmt)
                continue;
            context.visitStmt(*bodyStmt);
        }
    }

    if (stmt.testPos == DoStmt::TestPos::Post)
        checkCond();
}

/// @brief Validate a FOR loop, including loop variable tracking and body analysis.
///
/// The helper resolves the loop variable, evaluates start/end/step expressions,
/// and records the active FOR variable so NEXT statements can be checked for
/// mismatches.  The loop guard scopes the loop on the analyzer's stack to ensure
/// EXIT statements recognise the context.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the FOR statement (non-const because the
///             analyzer records implicit conversions on it).
void analyzeFor(SemanticAnalyzer &analyzer, ForStmt &stmt) {
    ControlCheckContext context(analyzer);

    // BUG-081 fix: Handle expression-based loop variables
    // Extract variable name for tracking, validate the lvalue expression
    std::string varName;
    SemanticAnalyzer::Type loopVarType = SemanticAnalyzer::Type::Unknown;
    if (stmt.varExpr) {
        if (auto *varExpr = as<VarExpr>(*stmt.varExpr)) {
            // Simple variable: FOR i = 1 TO 10
            varName = varExpr->name;
            context.resolveLoopVariable(varName);
            // Update the VarExpr with the scoped name (e.g., "I" -> "I_2")
            varExpr->name = varName;
            if (auto ty = context.varType(varName))
                loopVarType = *ty;
        } else if (as<MemberAccessExpr>(*stmt.varExpr)) {
            // Member access: FOR obj.field = 1 TO 10
            // Validate the base expression exists
            loopVarType = context.evaluateExpr(*stmt.varExpr);
            // For tracking purposes, use a placeholder name (NEXT matching with complex
            // expressions is optional)
            varName = "<complex>";
        } else if (as<ArrayExpr>(*stmt.varExpr)) {
            // Array element: FOR arr(i) = 1 TO 10
            loopVarType = context.evaluateExpr(*stmt.varExpr);
            varName = "<complex>";
        } else {
            // Other expressions are not assignable loop counters.
            context.evaluateExpr(*stmt.varExpr);
            varName = "<complex>";
            context.diagnostics().emit(il::support::Severity::Error,
                                       "B2001",
                                       stmt.varExpr->loc,
                                       1,
                                       "FOR loop variable must be assignable");
        }
    }

    if (loopVarType != SemanticAnalyzer::Type::Unknown && !isNumericLoopType(loopVarType))
        emitLoopTypeError(
            context, stmt.varExpr ? stmt.varExpr->loc : stmt.loc, 1, "FOR variable", loopVarType);

    if (stmt.start) {
        auto type = context.evaluateExpr(*stmt.start);
        if (type != SemanticAnalyzer::Type::Unknown && !isNumericLoopType(type))
            emitLoopTypeError(context, stmt.start->loc, 1, "FOR start expression", type);
    }
    if (stmt.end) {
        auto type = context.evaluateExpr(*stmt.end);
        if (type != SemanticAnalyzer::Type::Unknown && !isNumericLoopType(type))
            emitLoopTypeError(context, stmt.end->loc, 1, "FOR end expression", type);
    }
    if (stmt.step) {
        auto type = context.evaluateExpr(*stmt.step);
        if (type != SemanticAnalyzer::Type::Unknown && !isNumericLoopType(type))
            emitLoopTypeError(context, stmt.step->loc, 1, "FOR STEP expression", type);
    }

    [[maybe_unused]] auto forGuard = context.trackForVariable(varName);
    [[maybe_unused]] auto loopGuard = context.forLoopGuard();
    [[maybe_unused]] auto scope = context.pushScope();
    for (const auto &bodyStmt : stmt.body) {
        if (!bodyStmt)
            continue;
        context.visitStmt(*bodyStmt);
    }
}

/// @brief Validate a FOR EACH loop for array iteration.
///
/// The helper verifies that the array exists and is indeed an array, declares
/// the element variable implicitly if needed, and analyzes the loop body.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the FOR EACH statement.
void analyzeForEach(SemanticAnalyzer &analyzer, ForEachStmt &stmt) {
    ControlCheckContext context(analyzer);

    std::string arrayName = stmt.arrayName;
    context.resolveSymbolRef(arrayName);
    stmt.arrayName = arrayName;

    // Verify the array exists
    if (!context.analyzer().lookupArrayMetadata(stmt.arrayName)) {
        context.diagnostics().emit(il::support::Severity::Error,
                                   "B1020",
                                   stmt.loc,
                                   static_cast<unsigned>(stmt.arrayName.size()),
                                   "array '" + stmt.arrayName + "' not found");
        return;
    }

    SemanticAnalyzer::Type arrayType =
        context.varType(stmt.arrayName).value_or(SemanticAnalyzer::Type::Unknown);
    auto elemType = arrayElementType(arrayType);
    if (!elemType) {
        std::string msg = "FOR EACH source must be an array";
        if (arrayType != SemanticAnalyzer::Type::Unknown) {
            msg += ", got ";
            msg += semantic_analyzer_detail::semanticTypeName(arrayType);
        }
        context.diagnostics().emit(
            il::support::Severity::Error, "B2001", stmt.loc, 1, std::move(msg));
    }

    // Resolve the element variable - this will create it if it doesn't exist
    std::string elemVar = stmt.elementVar;
    context.resolveLoopVariable(elemVar);
    stmt.elementVar = elemVar; // Update with scoped name if changed

    if (elemType) {
        auto currentElemType = context.varType(stmt.elementVar);
        if (!currentElemType || *currentElemType == SemanticAnalyzer::Type::Unknown) {
            context.setVarType(stmt.elementVar, *elemType);
        } else if (*currentElemType != *elemType) {
            std::string msg = "FOR EACH element variable type ";
            msg += semantic_analyzer_detail::semanticTypeName(*currentElemType);
            msg += " does not match array element type ";
            msg += semantic_analyzer_detail::semanticTypeName(*elemType);
            context.diagnostics().emit(
                il::support::Severity::Error, "B2001", stmt.loc, 1, std::move(msg));
        }
    }

    // Track as a FOR loop for EXIT FOR support
    [[maybe_unused]] auto forGuard = context.trackForVariable(stmt.elementVar);
    [[maybe_unused]] auto loopGuard = context.forLoopGuard();
    [[maybe_unused]] auto scope = context.pushScope();
    for (const auto &bodyStmt : stmt.body) {
        if (!bodyStmt)
            continue;
        context.visitStmt(*bodyStmt);
    }
}

/// @brief Validate a NEXT statement, ensuring it matches an active FOR loop.
///
/// NEXT can optionally name the loop variable.  The helper verifies that a FOR
/// loop is active and, when a variable is provided, that it matches the innermost
/// loop.  Diagnostics B1002 surface when mismatches occur.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the NEXT statement.
void analyzeNext(SemanticAnalyzer &analyzer, const NextStmt &stmt) {
    ControlCheckContext context(analyzer);
    if (!context.hasForVariable() ||
        (!stmt.var.empty() && stmt.var != context.currentForVariable())) {
        std::string msg = "mismatched NEXT";
        if (!stmt.var.empty()) {
            msg += " '";
            msg += stmt.var;
            msg += "'";
        }
        if (context.hasForVariable()) {
            msg += ", expected '";
            msg += std::string(context.currentForVariable());
            msg += "'";
        } else {
            msg += ", no active FOR";
        }
        context.diagnostics().emit(
            il::support::Severity::Error, "B1002", stmt.loc, 4, std::move(msg));
        return;
    }

    context.popForVariable();
}

/// @brief Validate an EXIT statement against the currently active loop stack.
///
/// EXIT targets a specific loop kind (DO, WHILE, FOR).  The helper checks that a
/// loop is active and that the requested kind matches the innermost loop on the
/// stack.  It reports diagnostic B1011 when EXIT is misused or mismatched.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param stmt AST node representing the EXIT statement.
void analyzeExit(SemanticAnalyzer &analyzer, const ExitStmt &stmt) {
    ControlCheckContext context(analyzer);
    if (stmt.kind == ExitStmt::LoopKind::Sub || stmt.kind == ExitStmt::LoopKind::Function) {
        const bool wantsFunction = stmt.kind == ExitStmt::LoopKind::Function;
        const char *targetName = wantsFunction ? "FUNCTION" : "SUB";
        if (!context.hasActiveProcScope()) {
            std::string msg = "EXIT ";
            msg += targetName;
            msg += " used outside of a procedure";
            context.diagnostics().emit(
                il::support::Severity::Error, "B1011", stmt.loc, 4, std::move(msg));
            return;
        }

        if (wantsFunction != context.isActiveFunction()) {
            std::string msg = "EXIT ";
            msg += targetName;
            msg += wantsFunction ? " used outside of a FUNCTION" : " used inside a FUNCTION";
            context.diagnostics().emit(
                il::support::Severity::Error, "B1011", stmt.loc, 4, std::move(msg));
            return;
        }
        return;
    }

    const auto targetLoop = context.toLoopKind(stmt.kind);
    const char *targetName = context.loopKindName(targetLoop);

    if (!context.hasActiveLoop()) {
        std::string msg = "EXIT ";
        msg += targetName;
        msg += " used outside of any loop";
        context.diagnostics().emit(
            il::support::Severity::Error, "B1011", stmt.loc, 4, std::move(msg));
        return;
    }

    const auto activeLoop = context.currentLoop();
    if (activeLoop == targetLoop)
        return;

    std::string msg = "EXIT ";
    msg += targetName;
    msg += " does not match innermost loop (";
    msg += context.loopKindName(activeLoop);
    msg += ')';
    context.diagnostics().emit(il::support::Severity::Error, "B1011", stmt.loc, 4, std::move(msg));
}

} // namespace il::frontends::basic::sem
