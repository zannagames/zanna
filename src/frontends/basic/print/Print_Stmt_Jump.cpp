//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/print/Print_Stmt_Jump.cpp
// Purpose: Emit BASIC jump and error-handling statements for the AST printer.
// Whitespace invariants: Output preserves original spacing contracts, only
//   writing spaces when required by the BASIC syntax tokens themselves.
// Ownership/Lifetime: Context and statement nodes remain owned by the caller.
// Notes: Helpers rely on Context to delegate nested printing.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements canonical serialization of BASIC jumps, returns, and
///        legacy error-handling statements.

#include "frontends/basic/print/Print_Stmt_Common.hpp"

namespace il::frontends::basic::print_stmt {

/// @brief Print a @c GOTO statement in the s-expression printer format.
/// @details Writes the literal token followed by the target label while relying
///          on the caller-provided context stream.  No additional spacing is
///          introduced beyond what the BASIC syntax requires.
/// @param stmt AST node describing the @c GOTO statement.
/// @param ctx Printer context providing the output stream and helpers.
void printGoto(const GotoStmt &stmt, Context &ctx) {
    ctx.stream() << "(GOTO " << stmt.target << ')';
}

/// @brief Print a @c GOSUB statement for the BASIC AST printer.
/// @details Emits the keyword followed by the numeric target line, matching the
///          canonical s-expression format used in golden tests.
/// @param stmt AST node describing the @c GOSUB statement.
/// @param ctx Printer context writing to the destination stream.
void printGosub(const GosubStmt &stmt, Context &ctx) {
    ctx.stream() << "(GOSUB " << stmt.targetLine << ')';
}

/// @brief Print a BASIC @c RETURN statement, including optional payloads.
/// @details Handles both plain returns and `RETURN GOSUB` as well as optional
///          return values by appending the appropriate suffixes before closing
///          the s-expression.
/// @param stmt AST node describing the @c RETURN statement.
/// @param ctx Printer context used for expression printing.
void printReturn(const ReturnStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(RETURN";
    if (stmt.isGosubReturn) {
        os << " GOSUB";
    }
    if (stmt.value) {
        os << ' ';
        ctx.printExpr(*stmt.value);
    }
    os << ')';
}

/// @brief Print an `ON ERROR GOTO` statement in s-expression form.
/// @details Emits either the literal zero for @c ON ERROR GOTO 0 or the provided
///          target label, encapsulated within parentheses for the printer
///          output.
/// @param stmt AST node describing the handler redirection.
/// @param ctx Printer context with the destination stream.
void printOnErrorGoto(const OnErrorGoto &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(ON-ERROR GOTO ";
    if (stmt.toZero) {
        os << '0';
    } else {
        os << stmt.target;
    }
    os << ')';
}

/// @brief Print a @c RESUME statement, capturing the resume mode.
/// @details Writes the base keyword and appends "NEXT" or a label when the
///          statement resumes execution at a different location.  The helper
///          preserves the absence of a suffix for @c RESUME SAME.
/// @param stmt AST node describing the resume behaviour.
/// @param ctx Printer context with streaming utilities.
void printResume(const Resume &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(RESUME";
    switch (stmt.mode) {
        case Resume::Mode::Same:
            break;
        case Resume::Mode::Next:
            os << " NEXT";
            break;
        case Resume::Mode::Label:
            os << ' ' << stmt.target;
            break;
    }
    os << ')';
}

} // namespace il::frontends::basic::print_stmt
