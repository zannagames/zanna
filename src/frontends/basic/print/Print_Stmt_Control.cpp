//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/print/Print_Stmt_Control.cpp
// Purpose: Emit BASIC control-flow statements for the AST printer.
// Whitespace invariants: Keywords are separated by single spaces and bodies are
//   serialized via Context::printNumberedBody to preserve indentation and
//   spacing guarantees.
// Ownership/Lifetime: Context and statement nodes are owned by the caller.
// Notes: Helpers rely on Context to handle recursive printing.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements canonical serialization of BASIC control-flow statements.

#include "frontends/basic/print/Print_Stmt_Common.hpp"

namespace il::frontends::basic::print_stmt {

/// @brief Render an IF/ELSEIF/ELSE chain to the printer stream.
/// @details The helper emits the s-expression form `(IF <cond> THEN <stmt> ...)`
///          and iterates through the optional ELSEIF arms before appending an
///          ELSE branch when present. Expressions and nested statements are
///          delegated to the shared @ref Context utilities so spacing matches
///          the rest of the printer.
/// @param stmt High-level IF statement containing branches and predicates.
/// @param ctx Printer context supplying the destination stream and helpers.
void printIf(const IfStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(IF ";
    ctx.printExpr(*stmt.cond);
    os << " THEN ";
    ctx.printStmt(*stmt.then_branch);
    for (const auto &elseif : stmt.elseifs) {
        os << " ELSEIF ";
        ctx.printExpr(*elseif.cond);
        os << " THEN ";
        ctx.printStmt(*elseif.then_branch);
    }
    if (stmt.else_branch) {
        os << " ELSE ";
        ctx.printStmt(*stmt.else_branch);
    }
    os << ')';
}

/// @brief Emit the SELECT CASE construct with arms and optional ELSE body.
/// @details The selector expression is printed first, defaulting to the style's
///          null representation when absent (representing `SELECT CASE TRUE`).
///          Each case arm is written as `(CASE <labels...>)` followed by the
///          numbered body; an additional `(CASE ELSE)` section is emitted when an
///          else body exists. Nested statements are printed via
///          @ref Context::printNumberedBody to maintain stable numbering.
/// @param stmt AST node describing the SELECT CASE statement.
/// @param ctx Printer context responsible for formatting child nodes.
void printSelectCase(const SelectCaseStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(SELECT CASE ";
    if (stmt.selector) {
        ctx.printExpr(*stmt.selector);
    } else {
        ctx.style.writeNull();
    }
    for (const auto &arm : stmt.arms) {
        os << " (CASE";
        for (auto label : arm.labels) {
            os << ' ' << label;
        }
        os << ')';
        ctx.printNumberedBody(arm.body);
    }
    if (!stmt.elseBody.empty()) {
        os << " (CASE ELSE)";
        ctx.printNumberedBody(stmt.elseBody);
    }
    os << ')';
}

/// @brief Print a WHILE loop header and numbered body.
/// @details Outputs the guard expression and then defers to
///          @ref Context::printNumberedBody so each BASIC line retains its
///          original numbers during pretty-printing.
/// @param stmt WHILE statement to render.
/// @param ctx Printer context with formatting helpers.
void printWhile(const WhileStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(WHILE ";
    ctx.printExpr(*stmt.cond);
    ctx.printNumberedBody(stmt.body);
}

/// @brief Emit a DO loop with its variant (`WHILE`, `UNTIL`, or unconditional`).
/// @details The printer records whether the test occurs before or after the
///          loop body, writes the condition kind, and prints the predicate when
///          required. Body statements are rendered using
///          @ref Context::printNumberedBody to preserve numbering.
/// @param stmt DO statement describing the iteration form.
/// @param ctx Printer context writing to the destination stream.
void printDo(const DoStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(DO " << (stmt.testPos == DoStmt::TestPos::Pre ? "pre" : "post") << ' ';
    switch (stmt.condKind) {
        case DoStmt::CondKind::None:
            os << "NONE";
            break;
        case DoStmt::CondKind::While:
            os << "WHILE";
            break;
        case DoStmt::CondKind::Until:
            os << "UNTIL";
            break;
    }
    if (stmt.condKind != DoStmt::CondKind::None && stmt.cond) {
        os << ' ';
        ctx.printExpr(*stmt.cond);
    }
    ctx.printNumberedBody(stmt.body);
}

/// @brief Print a FOR loop header with optional STEP expression.
/// @details The helper emits `(FOR <var> = <start> TO <end> [STEP <step>])`
///          before deferring to @ref Context::printNumberedBody for the loop
///          body. Optional step expressions are only printed when provided in
///          the AST to match user input.
/// @param stmt FOR statement carrying bounds and optional step.
/// @param ctx Printer context that formats nested nodes.
void printFor(const ForStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(FOR ";
    if (stmt.varExpr)
        ctx.printExpr(*stmt.varExpr);
    else
        os << "<missing-var>";
    os << " = ";
    ctx.printExpr(*stmt.start);
    os << " TO ";
    ctx.printExpr(*stmt.end);
    if (stmt.step) {
        os << " STEP ";
        ctx.printExpr(*stmt.step);
    }
    ctx.printNumberedBody(stmt.body);
}

/// @brief Emit the `NEXT` statement closing a FOR loop.
/// @details Outputs the loop variable directly, producing `(NEXT <var>)`. The
///          helper intentionally performs no additional formatting because the
///          AST guarantees a single variable name.
/// @param stmt NEXT statement to print.
/// @param ctx Printer context providing the stream.
void printNext(const NextStmt &stmt, Context &ctx) {
    ctx.stream() << "(NEXT " << stmt.var << ')';
}

/// @brief Print an EXIT statement annotated with the loop kind being exited.
/// @details Converts the enum describing the loop category back into its BASIC
///          keyword (FOR/WHILE/DO) and appends it to the `(EXIT ...)` prefix so
///          the output mirrors the original syntax.
/// @param stmt EXIT statement that terminates a surrounding loop.
/// @param ctx Printer context used for streaming.
void printExit(const ExitStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(EXIT ";
    switch (stmt.kind) {
        case ExitStmt::LoopKind::For:
            os << "FOR";
            break;
        case ExitStmt::LoopKind::While:
            os << "WHILE";
            break;
        case ExitStmt::LoopKind::Do:
            os << "DO";
            break;
        case ExitStmt::LoopKind::Sub:
            os << "SUB";
            break;
        case ExitStmt::LoopKind::Function:
            os << "FUNCTION";
            break;
    }
    os << ')';
}

} // namespace il::frontends::basic::print_stmt
