//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/AstPrinter.cpp
// Purpose: Orchestrate the BASIC AST printer facade, indentation, punctuation,
//          and recursive dispatch into expression and statement visitors.
// Ownership/Lifetime: All streams and formatting helpers are local to a dump;
//                     AST nodes are borrowed read-only.
// Links: src/frontends/basic/AstPrinter.hpp,
//        src/frontends/basic/AstPrint_Expr.cpp,
//        src/frontends/basic/AstPrint_Stmt.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file AstPrinter.cpp
 * @brief Implements BASIC AST printer orchestration and shared formatting.
 */

#include "frontends/basic/AstPrinter.hpp"
#include "frontends/basic/BasicTypes.hpp"
#include <sstream>

namespace il::frontends::basic {

/// @brief Create a print style that forwards delimiters to a printer stream.
///
/// The style stores a pointer to the owning @ref Printer so helper methods can
/// emit structural characters directly into the canonical output stream without
/// copying state.
///
/// @param printer Printer that ultimately receives formatted AST text.
AstPrinter::PrintStyle::PrintStyle(Printer &printer) : printer(&printer) {}

/// @brief Emit the opening delimiter used when printing composite constructs.
///
/// Multi-part statements such as PRINT# wrap their payload inside parentheses
/// for readability.  The helper injects the space before the brace so callers do
/// not have to manage spacing rules themselves.
void AstPrinter::PrintStyle::openBody() const {
    printer->os << " {";
}

/// @brief Emit the closing delimiter paired with @ref openBody.
///
/// Keeping the implementation centralised prevents mismatched delimiters when
/// future formatting tweaks are required.
void AstPrinter::PrintStyle::closeBody() const {
    printer->os << "})";
}

/// @brief Insert a space between list elements on every call after the first.
///
/// The @p first flag is toggled by the helper, letting callers express
/// comma/space separated lists without manual bookkeeping.
///
/// @param first Indicates whether an element has already been emitted.
/// @post @p first is false.
void AstPrinter::PrintStyle::separate(bool &first) const {
    if (!first) {
        printer->os << ' ';
    }
    first = false;
}

/// @brief Write a ``<line>:`` prefix to the output stream.
///
/// @param line One-based source line number to render.
void AstPrinter::PrintStyle::writeLineNumber(int line) const {
    printer->os << line << ':';
}

/// @brief Emit the canonical ``<null>`` marker for missing optional values.
void AstPrinter::PrintStyle::writeNull() const {
    printer->os << "<null>";
}

/// @brief Emit the `` channel=#`` prefix for PRINT# invocations.
void AstPrinter::PrintStyle::writeChannelPrefix() const {
    printer->os << " channel=#";
}

/// @brief Emit the opening bracket that precedes argument lists.
void AstPrinter::PrintStyle::writeArgsPrefix() const {
    printer->os << " args=[";
}

/// @brief Emit the closing bracket that terminates argument lists.
void AstPrinter::PrintStyle::writeArgsSuffix() const {
    printer->os << ']';
}

/// @brief Append the `` no-newline`` suffix for PRINT# statements without EOL.
void AstPrinter::PrintStyle::writeNoNewlineTag() const {
    printer->os << " no-newline";
}

/// @brief Write a line of text to the underlying stream with current indentation.
/// @param text Line content to emit.
/// @note Appends a newline character and resets column position.
void AstPrinter::Printer::line(std::string_view text) {
    for (int i = 0; i < indent; ++i)
        os << "  ";
    os << text << '\n';
}

/// @brief Increase indentation level and return RAII guard.
/// @return Indent object whose destruction restores previous indentation.
/// @post The indentation depth is incremented by one.
AstPrinter::Printer::Indent AstPrinter::Printer::push() {
    ++indent;
    return Indent{*this};
}

/// @brief Restore the indentation level saved at construction time.
/// @post The associated printer's indentation is decremented by one.
AstPrinter::Printer::Indent::~Indent() {
    --p.indent;
}

/// @brief Serialize an entire BASIC program to a printable string.
/// @details Emits procedure statements first and main-body statements second,
///          retaining order within both containers. A fresh nested printer
///          formats each statement after its stored line-number prefix.
/// @param prog Program whose procedures and main body are dumped.
/// @return Concatenated text representation of @p prog.
std::string AstPrinter::dump(const Program &prog) {
    std::ostringstream os;
    Printer p{os};
    for (auto &stmt : prog.procs) {
        std::ostringstream line_os;
        line_os << stmt->line << ": ";
        Printer line_p{line_os};
        dump(*stmt, line_p);
        p.line(line_os.str());
    }
    for (auto &stmt : prog.main) {
        std::ostringstream line_os;
        line_os << stmt->line << ": ";
        Printer line_p{line_os};
        dump(*stmt, line_p);
        p.line(line_os.str());
    }
    return os.str();
}

/// @brief Recursively print a statement node and its children.
/// @param stmt Statement to dump.
/// @param p Printer receiving the textual form.
/// @note Creates a fresh style that borrows @p p for this dispatch.
void AstPrinter::dump(const Stmt &stmt, Printer &p) {
    PrintStyle style{p};
    printStmt(stmt, p, style);
}

/// @brief Print an expression node to the printer.
/// @param expr Expression to dump.
/// @param p Printer receiving output.
/// @note Creates a fresh style that borrows @p p for this dispatch.
void AstPrinter::dump(const Expr &expr, Printer &p) {
    PrintStyle style{p};
    printExpr(expr, p, style);
}

} // namespace il::frontends::basic
