//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares utilities for printing BASIC AST nodes in a human-readable
// format for debugging, testing, and diagnostic purposes.
//
// The AST printer provides formatted output of the BASIC abstract syntax tree,
// enabling developers to inspect the structure produced by the parser and
// verify that AST construction is correct.
//
// Key Responsibilities:
// - AST visualization: Produces indented, structured text representations of
//   AST nodes showing the tree hierarchy
// - Statement printing: Formats all BASIC statement types (assignments,
//   control flow, I/O, declarations)
// - Expression printing: Displays expression trees with operator precedence
//   and type information
// - Declaration printing: Shows procedure signatures, variable declarations,
//   and array specifications
//
// Output Format:
// The printer uses indentation to show nesting and includes:
// - Node type (e.g., IfStmt, ForStmt, BinaryExpr)
// - Key attributes (variable names, literal values, operators)
// - Child nodes (recursively printed with increased indentation)
//
// Example Output:
//   Program
//     MainStmts:
//       IfStmt
//         Condition: BinaryExpr (>)
//           Left: Identifier(x%)
//           Right: IntLiteral(10)
//         ThenBody:
//           PrintStmt
//             Expr: StringLiteral("Too large")
//
// Integration:
// - Used by: Test infrastructure for AST golden file testing
// - Used by: Debugging tools during frontend development
// - Enables: Visual verification of parser output
//
// Design Notes:
// - Does not take ownership of AST nodes; only traverses for printing
// - Uses internal Printer helper class for indentation management
// - Stateless functions that can be called on any AST node
// - Output is deterministic for reproducible test results
//
//===----------------------------------------------------------------------===//

/**
 * @file AstPrinter.hpp
 * @brief Declares deterministic, human-readable serialization of BASIC ASTs.
 *
 * AstPrinter owns no AST state. A dump call builds temporary stream, indentation,
 * style, and visitor objects, then returns the complete serialized program.
 */

#pragma once

#include "frontends/basic/AST.hpp"
#include <ostream>
#include <string>
#include <string_view>

namespace il::frontends::basic {
namespace print_stmt {
struct Context;
} // namespace print_stmt

/// @brief Emits a textual representation of BASIC programs for debugging.
///
/// AstPrinter walks the Program, Stmt, and Expr nodes to produce a
/// human-readable dump using an internal Printer helper to manage
/// indentation. Each dump call is independent and borrows the AST only for the
/// duration of traversal.
class AstPrinter {
  public:
    /// Allow statement-formatting helpers to reuse private printer facilities.
    friend struct print_stmt::Context;
    /// @brief Produce a formatted dump of the given program.
    /// @details Procedures are emitted first in stored order, followed by main
    ///          statements. Each top-level statement is prefixed with its BASIC
    ///          line number and terminated by Printer::line().
    /// @param prog Program AST to print.
    /// @return String containing the formatted dump.
    std::string dump(const Program &prog);

  private:
    /// @brief Stateful helper that writes lines with indentation.
    struct Printer {
        /// @brief Bind the printer to an output stream.
        /// @param out Stream that receives formatted AST lines.
        /// @warning @p out must outlive this helper and every Indent guard.
        explicit Printer(std::ostream &out) : os(out) {}

        /// Borrowed output stream where text is emitted.
        std::ostream &os;

        /// Current two-space indentation depth.
        int indent = 0;

        /// @brief Write @p text followed by a newline, honoring indentation.
        /// @details Emits two spaces for each current indentation level before
        ///          writing the supplied bytes and a newline.
        /// @param text Line content to write.
        void line(std::string_view text);

        /// @brief RAII guard that decreases indentation on destruction.
        /// @details Created only by Printer::push() after it increments the
        ///          printer's indentation.
        struct Indent {
            /// Borrowed printer whose indentation is managed.
            Printer &p;

            /// @brief Restore previous indentation level when destroyed.
            /// @post Decrements `p.indent` exactly once.
            ~Indent();
        };

        /// @brief Increase indentation for the next scope.
        /// @return Guard that restores previous indentation when destroyed.
        /// @post indent is one greater until the returned guard is destroyed.
        Indent push();
    };

    /// @brief Holds formatting preferences for AST emission.
    struct PrintStyle {
        /// @brief Create a style bound to @p printer.
        /// @details Stores a non-owning pointer used by every formatting method.
        explicit PrintStyle(Printer &printer);

        /// @brief Emit the opening delimiter for a statement body.
        /// @details Writes a leading space followed by `{`.
        void openBody() const;

        /// @brief Emit the closing delimiter for a statement body.
        /// @details Writes `})`, matching the enclosing statement/body format.
        void closeBody() const;

        /// @brief Ensure elements printed with @p first are separated.
        /// @param first In/out flag that is cleared; a space is emitted only
        ///              when the incoming value is false.
        void separate(bool &first) const;

        /// @brief Write a numbered label prefix for a statement.
        /// @param line Numeric line value written before a colon.
        void writeLineNumber(int line) const;

        /// @brief Emit the placeholder used for null optional nodes.
        void writeNull() const;

        /// @brief Emit the prefix used before channel expressions.
        void writeChannelPrefix() const;

        /// @brief Emit the prefix used before argument lists.
        void writeArgsPrefix() const;

        /// @brief Emit the suffix used after argument lists.
        void writeArgsSuffix() const;

        /// @brief Emit the marker indicating trailing newline suppression.
        void writeNoNewlineTag() const;

      private:
        ///< Borrowed destination; valid for the style's entire lifetime.
        Printer *printer;
    };

    /// @brief Expression visitor producing textual representations.
    struct ExprPrinter;

    /// @brief Statement visitor producing textual representations.
    struct StmtPrinter;

    /// @brief Helper invoked by visitors to serialize expressions.
    /// @param expr Expression dispatched through ExprPrinter.
    /// @param p Destination printer.
    /// @param style Formatting state shared with the surrounding traversal.
    static void printExpr(const Expr &expr, Printer &p, PrintStyle &style);

    /// @brief Helper invoked by the dispatcher to serialize statements.
    /// @param stmt Statement dispatched through StmtPrinter.
    /// @param p Destination printer.
    /// @param style Formatting state shared with nested helpers.
    static void printStmt(const Stmt &stmt, Printer &p, PrintStyle &style);

    /// @brief Dump a statement node to the printer.
    /// @param stmt Statement to serialize.
    /// @param p Printer receiving the output.
    void dump(const Stmt &stmt, Printer &p);

    /// @brief Dump an expression node to the printer.
    /// @param expr Expression to serialize.
    /// @param p Printer receiving the output.
    void dump(const Expr &expr, Printer &p);
};

} // namespace il::frontends::basic
