//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/print/Print_Stmt_Common.hpp
// Purpose: Shared helpers for BASIC statement printing.
// Whitespace invariants: Helpers never emit trailing whitespace and only insert
//   spaces or commas that mirror canonical BASIC formatting.
// Ownership/Lifetime: Context references are non-owning and must outlive the
//   helper calls.
// Notes: Used internally by AstPrinter statement visitors.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/AstPrinter.hpp"

#include <memory>
#include <ostream>
#include <vector>

/// @file
/// @brief Defines shared context, formatting, and dispatch helpers for BASIC
///        statement serialization.

namespace il::frontends::basic {
class AstPrinter;
} // namespace il::frontends::basic

namespace il::frontends::basic::print_stmt {
/// @brief Shared utilities that wire AstPrinter helpers together while emitting statements.
/// @details The context bundles the owning @ref AstPrinter instance with a printer style and
///          dispatcher reference.  Helper routines use the aggregated references to format
///          subexpressions, delegate nested statements, and access the output stream without
///          copying heavy printing state.
struct Context {
    /// @brief Bind a statement-printing context to active printer collaborators.
    /// @param p Printer that owns indentation and stream output.
    /// @param s Style helper used for expression and argument formatting.
    /// @param d Dispatcher visitor used for nested statements.
    Context(AstPrinter::Printer &p, AstPrinter::PrintStyle &s, AstPrinter::StmtPrinter &d)
        : printer(p), style(s), dispatcher(d) {}

    AstPrinter::Printer &printer;
    AstPrinter::PrintStyle &style;
    AstPrinter::StmtPrinter &dispatcher;

    /// @brief Retrieve the underlying output stream that receives formatted BASIC text.
    /// @details Returns the stream stored on the owning @ref AstPrinter so helpers can
    ///          write output directly without re-threading references through each call.
    std::ostream &stream() const {
        return printer.os;
    }

    /// @brief Emit an expression using the statement printer's expression visitor.
    /// @details Forwards @p expr to the owning printer so all expression formatting flows
    ///          through the centralised visitor logic.  This ensures expressions embedded in
    ///          statements inherit consistent indentation and keyword casing rules.
    void printExpr(const Expr &expr) const;

    /// @brief Conditionally emit an expression when the pointer is non-null.
    /// @details Guards against optional operands by checking @p expr before delegating to
    ///          @ref printExpr.  When the pointer is null the helper emits nothing, letting
    ///          statement-specific logic decide how to handle the omission.
    void printOptionalExpr(const Expr *expr) const;

    /// @brief Dispatch a nested statement to the printer's statement visitor.
    /// @details Invokes the registered dispatcher so that nested statement sequences (for
    ///          example, the body of a DO loop) render using the same indentation policy as
    ///          top-level statements.
    void printStmt(const Stmt &stmt) const;

    /// @brief Print a numbered BASIC statement body with canonical indentation.
    /// @details Iterates the statement array, prefixes each line with the associated line
    ///          number, and delegates to @ref printStmt for the actual statement rendering.
    ///          The helper preserves the spacing conventions documented for the BASIC
    ///          printer so reformatting remains stable across runs.
    void printNumberedBody(const std::vector<std::unique_ptr<Stmt>> &body) const;
};

/// @brief Translate an AstPrinter type tag into the corresponding keyword string.
/// @details Converts the enum value used by the printer into the uppercase token expected by
///          canonical BASIC output.  The helper provides a default branch so unexpected enum
///          values still produce a sensible fallback during debugging.
/// @param ty BASIC scalar type tag to render.
/// @return Static uppercase type keyword.
inline const char *typeToString(Type ty) {
    switch (ty) {
        case Type::I64:
            return "I64";
        case Type::F64:
            return "F64";
        case Type::Str:
            return "STR";
        case Type::Bool:
            return "BOOLEAN";
    }
    return "I64";
}

/// @brief Convert an OPEN statement mode into its textual representation.
/// @details Maps the strongly-typed mode enum to the uppercase keyword that appears in BASIC
///          source.  The helper uses a defensive default return so unhandled enumeration
///          values degrade gracefully when new modes are introduced.
/// @param mode File-open mode to render.
/// @return Static uppercase OPEN mode keyword.
inline const char *openModeToString(OpenStmt::Mode mode) {
    switch (mode) {
        case OpenStmt::Mode::Input:
            return "INPUT";
        case OpenStmt::Mode::Output:
            return "OUTPUT";
        case OpenStmt::Mode::Append:
            return "APPEND";
        case OpenStmt::Mode::Binary:
            return "BINARY";
        case OpenStmt::Mode::Random:
            return "RANDOM";
    }
    return "INPUT";
}

/// @brief Emit a PRINT statement, respecting separators and trailing semicolons.
/// @details Iterates the expression list, reproduces the original comma/semicolon layout, and
///          dispatches each expression via @ref Context::printExpr so formatting logic is
///          centralised.
void printPrint(const PrintStmt &stmt, Context &ctx);

/// @brief Emit a PRINT# channel statement with explicit channel decoration.
/// @details Writes the channel prefix (including the leading #), mirrors the delimiter between
///          expressions, and delegates payload formatting to @ref Context utilities.
void printPrintChannel(const PrintChStmt &stmt, Context &ctx);

/// @brief Emit an OPEN statement with canonical quoting and mode ordering.
/// @details Prints `OPEN` with the path, access mode, and optional keywords such as `AS` and
///          `LEN`.  The helper pulls textual mode strings from @ref openModeToString.
void printOpen(const OpenStmt &stmt, Context &ctx);

/// @brief Emit a CLOSE statement targeting the specified channels.
/// @details Prints either `CLOSE` alone or a comma-separated list of channel designators,
///          mirroring the AST layout to keep round-tripping stable.
void printClose(const CloseStmt &stmt, Context &ctx);

/// @brief Emit a SEEK statement that repositions a file channel.
/// @details Prints the channel designator, comma, and new position expression while delegating
///          expression formatting to the context utilities.
void printSeek(const SeekStmt &stmt, Context &ctx);

/// @brief Emit an INPUT statement that reads from stdin.
/// @details Reconstructs the prompt (if any) and variable list, inserting separators exactly as
///          the parser recorded them so reprints match canonical BASIC output.
void printInput(const InputStmt &stmt, Context &ctx);

/// @brief Emit an INPUT# statement that reads from a channel.
/// @details Prepends the channel designator, preserves delimiters, and prints the assignment
///          targets through the expression helpers.
void printInputChannel(const InputChStmt &stmt, Context &ctx);

/// @brief Emit a LINE INPUT# statement for reading entire lines from a channel.
/// @details Prints the channel, target variable, and optional prompt while enforcing the
///          spacing conventions expected by BASIC's LINE INPUT syntax.
void printLineInputChannel(const LineInputChStmt &stmt, Context &ctx);

/// @brief Emit a multi-branch IF statement, handling single-line and block forms.
/// @details Prints the conditional expression, determines whether the AST represents a
///          single-line or multiline IF, and delegates nested statement printing accordingly.
void printIf(const IfStmt &stmt, Context &ctx);

/// @brief Emit a SELECT CASE statement using the normalised select model.
/// @details Prints the selector expression and iterates each case arm, invoking the context to
///          emit arm bodies with correct indentation.
void printSelectCase(const SelectCaseStmt &stmt, Context &ctx);

/// @brief Emit a WHILE/WEND loop with consistent indentation.
/// @details Prints the controlling expression, opens the loop body with increased indentation,
///          and ensures the terminal `WEND` keyword appears on its own line.
void printWhile(const WhileStmt &stmt, Context &ctx);

/// @brief Emit a DO loop with the correct variant keyword (`WHILE`/`UNTIL`).
/// @details Prints the entry or exit condition depending on the AST flavour and emits the loop
///          body using the context dispatcher before writing the closing keyword.
void printDo(const DoStmt &stmt, Context &ctx);

/// @brief Emit a FOR/NEXT loop, including optional step expressions.
/// @details Prints the loop variable assignment, terminating value, and optional STEP clause,
///          then emits the loop body before printing the matching NEXT statement.
void printFor(const ForStmt &stmt, Context &ctx);

/// @brief Emit a NEXT statement that closes a FOR loop.
/// @details Prints either the implicit NEXT or enumerates the explicit iterator list captured
///          by the AST node.
void printNext(const NextStmt &stmt, Context &ctx);

/// @brief Emit EXIT statements for the various BASIC constructs.
/// @details Prints the EXIT keyword followed by the construct (FOR, DO, FUNCTION, etc.) encoded
///          in the AST so the output mirrors the original control-flow intent.
void printExit(const ExitStmt &stmt, Context &ctx);

/// @brief Emit an unconditional GOTO statement.
/// @details Prints the keyword and numeric target while allowing the context to manage spacing
///          and trailing newlines.
void printGoto(const GotoStmt &stmt, Context &ctx);

/// @brief Emit a GOSUB statement targeting a line label.
/// @details Prints the keyword and destination label; used for subroutine invocations in
///          legacy BASIC code.
void printGosub(const GosubStmt &stmt, Context &ctx);

/// @brief Emit a RETURN statement.
/// @details Prints `RETURN` with optional line-number targets, matching the semantics encoded
///          in the AST.
void printReturn(const ReturnStmt &stmt, Context &ctx);

/// @brief Emit ON ERROR GOTO statements.
/// @details Prints the structured error-handling clause including the destination line label or
///          `0` to disable handlers.
void printOnErrorGoto(const OnErrorGoto &stmt, Context &ctx);

/// @brief Emit RESUME statements that continue exception handling.
/// @details Prints the variant of RESUME requested by the AST (plain, NEXT, or line-number) so
///          the textual output mirrors the user's intent.
void printResume(const Resume &stmt, Context &ctx);

/// @brief Emit LET assignments, eliding the keyword when the AST captured implicit forms.
/// @details Reconstructs the assignment expression and prints an explicit `LET` only when the
///          source program contained it, keeping round-tripping faithful.
void printLet(const LetStmt &stmt, Context &ctx);

/// @brief Emit CONST declarations with initializer expressions.
/// @details Prints the constant name, type, and initializer.
void printConst(const ConstStmt &stmt, Context &ctx);

/// @brief Emit DIM declarations, including array bounds lists.
/// @details Iterates the declaration list and prints each variable with its dimension ranges
///          using canonical comma-separated formatting.
void printDim(const DimStmt &stmt, Context &ctx);

/// @brief Emit REDIM statements that resize existing arrays.
/// @details Prints the target variables and new dimension bounds, mirroring the structure of
///          the AST to retain formatting invariants.
void printReDim(const ReDimStmt &stmt, Context &ctx);

/// @brief Emit FUNCTION declarations with their parameter lists and bodies.
/// @details Prints the function signature (including return type suffixes), delegates the body
///          to @ref Context::printNumberedBody when line numbers are present, and writes the
///          terminating `END FUNCTION` marker.
void printFunction(const FunctionDecl &stmt, Context &ctx);

/// @brief Emit SUB declarations, mirroring BASIC's canonical layout.
/// @details Prints the procedure signature, indents the body, and emits the `END SUB` trailer.
void printSub(const SubDecl &stmt, Context &ctx);

/// @brief Emit CLASS constructors.
/// @details Prints the constructor header with parameter list, the body statements, and
///          terminates with `END CONSTRUCTOR`.
void printConstructor(const ConstructorDecl &stmt, Context &ctx);

/// @brief Emit CLASS destructors.
/// @details Prints the destructor header and body while ensuring the `END DESTRUCTOR` keyword
///          appears alone on the final line.
void printDestructor(const DestructorDecl &stmt, Context &ctx);

/// @brief Emit METHOD declarations within CLASS blocks.
/// @details Prints the method signature, including any STATIC or SHARED modifiers encoded in
///          the AST, then delegates the body rendering to the context utilities.
void printMethod(const MethodDecl &stmt, Context &ctx);
/// @brief Emit PROPERTY declarations with optional GET/SET blocks.
/// @details Prints the property header, optional STATIC marker, type, and
///          accessor bodies when present. Access modifiers on accessors are
///          included when they differ from the property headline.
void printProperty(const PropertyDecl &stmt, Context &ctx);

/// @brief Emit CLASS declarations with nested members.
/// @details Prints the class header, iterates member declarations, and emits closing `END CLASS`
///          ensuring indentation and blank-line spacing follow the canonical style.
void printClass(const ClassDecl &stmt, Context &ctx);

/// @brief Emit TYPE declarations that describe user-defined types.
/// @details Prints the TYPE header, member field list, and `END TYPE` terminator matching the
///          parser's stored ordering.
void printType(const TypeDecl &stmt, Context &ctx);

/// @brief Emit DELETE statements that release object references.
/// @details Prints the `DELETE` keyword followed by the target expression, delegating expression
///          formatting to the context helpers so spacing stays consistent.
void printDelete(const DeleteStmt &stmt, Context &ctx);
/// @brief Emit INTERFACE declarations with abstract members.
/// @details Prints the interface qualified name and numbered member body.
void printInterface(const InterfaceDecl &stmt, Context &ctx);

} // namespace il::frontends::basic::print_stmt
