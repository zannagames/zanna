//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/AstPrint_Expr.cpp
// Purpose: Render every BASIC expression node into the stable AST dump format.
// Key invariants: Traversal is read-only and preserves stored operand order.
// Ownership/Lifetime: The visitor borrows its destination printer.
// Links: src/frontends/basic/AstPrinter.hpp,
//        src/frontends/basic/AstPrint_Stmt.cpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/AstPrinter.hpp"
#include "frontends/basic/BuiltinRegistry.hpp"
#include <array>
#include <sstream>

/// @file
/// @brief Expression-printing visitor used by the BASIC AST printer.
/// @details Implements the concrete visitor that walks expression nodes and
///          serialises them into the `AstPrinter` stream using a stable textual
///          representation.  Formatting choices mirror the statement printer so
///          AST dumps remain both human-readable and deterministic.

namespace il::frontends::basic {

namespace {} // namespace

/// @brief Visitor that renders expression nodes to the printer's stream.
/// @details Each override emits prefix-style textual forms that match the
///          companion statement printer, ensuring dumps remain stable and easy
///          to parse visually.
struct AstPrinter::ExprPrinter final : ExprVisitor {
    /// @brief Construct the visitor with a destination printer.
    /// @details The style parameter is currently unused but preserved so
    ///          expression and statement visitors share a consistent signature.
    ///          Future formatting tweaks can opt into style-specific behaviour
    ///          without changing call sites.
    /// @param printer Borrowed destination receiving expression tokens.
    /// @param style Surrounding print style, currently unused.
    ExprPrinter(Printer &printer, [[maybe_unused]] PrintStyle &style) : printer(printer) {}

    /// @brief Dispatch expression printing through the visitor interface.
    /// @details Delegates to @ref Expr::accept, enabling virtual dispatch across
    ///          the node hierarchy while keeping the call site concise.
    /// @param expr Expression node to render.
    void print(const Expr &expr) {
        expr.accept(*this);
    }

    /// @brief Fallback for unhandled expression types; triggers a build-time failure.
    /// @details Employs a `static_assert` so that adding a new node kind without
    ///          updating the printer yields a compiler error rather than a silent
    ///          omission from dumps.
    /// @tparam NodeT Unhandled concrete expression type.
    template <typename NodeT> void visit([[maybe_unused]] const NodeT &) {
        static_assert(sizeof(NodeT) == 0, "Unhandled expression node in AstPrinter");
    }

    /// @brief Print an integer literal expression.
    /// @details Writes the literal value verbatim, relying on the parser to have
    ///          normalised the token text already.
    /// @param expr Integer literal whose stored value is emitted.
    void visit(const IntExpr &expr) override {
        printer.os << expr.value;
    }

    /// @brief Print a floating-point literal expression preserving precision.
    /// @details Streams the value through an @c ostringstream so the default C++
    ///          formatting rules apply, then forwards the resulting string.
    /// @param expr Floating-point literal whose value is streamed.
    void visit(const FloatExpr &expr) override {
        std::ostringstream os;
        os << expr.value;
        printer.os << os.str();
    }

    /// @brief Print a string literal with surrounding quotes.
    /// @details Emits the value wrapped in double quotes.  Characters are
    ///          written verbatim because the parser already normalises escape
    ///          sequences during AST construction.
    /// @param expr String literal whose value is quoted.
    void visit(const StringExpr &expr) override {
        printer.os << '"' << expr.value << '"';
    }

    /// @brief Print a boolean literal as TRUE/FALSE tokens.
    /// @details Uses uppercase tokens to match the BASIC surface syntax and
    ///          golden tests.
    /// @param expr Boolean literal selecting TRUE or FALSE.
    void visit(const BoolExpr &expr) override {
        printer.os << (expr.value ? "TRUE" : "FALSE");
    }

    /// @brief Print a variable reference by name.
    /// @details Emits the canonical identifier spelling stored on the node.
    /// @param expr Variable reference to render.
    void visit(const VarExpr &expr) override {
        printer.os << expr.name;
    }

    /// @brief Print an array element access with its index expression(s).
    /// @details Emits `name(expr)` or `name(i,j,k)` preserving the syntactic
    ///          order of the original index expressions.
    /// @param expr Array reference whose non-null indices are rendered.
    void visit(const ArrayExpr &expr) override {
        printer.os << expr.name << '(';
        bool first = true;
        for (const auto &indexPtr : expr.indices) {
            if (!indexPtr)
                continue;
            if (!first)
                printer.os << ',';
            first = false;
            indexPtr->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print a unary expression with explicit operator tokens.
    /// @details Uses prefix notation to avoid ambiguity with chained unary
    ///          operators while keeping the textual output compact.
    /// @param expr Unary operation and operand to render.
    void visit(const UnaryExpr &expr) override {
        printer.os << '(';
        switch (expr.op) {
            case UnaryExpr::Op::LogicalNot:
                printer.os << "NOT ";
                break;
            case UnaryExpr::Op::Plus:
                printer.os << "+ ";
                break;
            case UnaryExpr::Op::Negate:
                printer.os << "- ";
                break;
        }
        expr.expr->accept(*this);
        printer.os << ')';
    }

    /// @brief Print a binary expression using prefix notation.
    /// @details The operator table mirrors the enum order so the visitor remains
    ///          data driven; prefix notation keeps evaluation order explicit for
    ///          nested expressions.
    /// @param expr Binary operation whose left and right operands are rendered.
    void visit(const BinaryExpr &expr) override {
        static constexpr std::array<const char *, 17> ops = {"+",
                                                             "-",
                                                             "*",
                                                             "/",
                                                             "^",
                                                             "\\",
                                                             "MOD",
                                                             "=",
                                                             "<>",
                                                             "<",
                                                             "<=",
                                                             ">",
                                                             ">=",
                                                             "ANDALSO",
                                                             "ORELSE",
                                                             "AND",
                                                             "OR"};
        printer.os << '(' << ops[static_cast<size_t>(expr.op)] << ' ';
        expr.lhs->accept(*this);
        printer.os << ' ';
        expr.rhs->accept(*this);
        printer.os << ')';
    }

    /// @brief Print a builtin call including the builtin mnemonic and arguments.
    /// @details Prepends the builtin mnemonic obtained from metadata and prints
    ///          each argument separated by spaces to match the prefix style used
    ///          throughout dumps.
    /// @param expr Builtin invocation to render.
    void visit(const BuiltinCallExpr &expr) override {
        printer.os << '(' << getBuiltinInfo(expr.builtin).name;
        for (const auto &arg : expr.args) {
            printer.os << ' ';
            arg->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print an LBOUND expression with its array operand.
    /// @details Emits `(LBOUND name)` mirroring the parser's canonical form.
    /// @param expr Array name and optional dimension expression.
    void visit(const LBoundExpr &expr) override {
        printer.os << "(LBOUND " << expr.name;
        if (expr.dimension) {
            printer.os << ' ';
            expr.dimension->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print a UBOUND expression with its array operand.
    /// @details Mirrors the `LBOUND` formatting while using the appropriate
    ///          mnemonic.
    /// @param expr Array name and optional dimension expression.
    void visit(const UBoundExpr &expr) override {
        printer.os << "(UBOUND " << expr.name;
        if (expr.dimension) {
            printer.os << ' ';
            expr.dimension->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print a user-defined call expression with its argument list.
    /// @details Emits `(callee arg1 arg2 ...)`, retaining the argument order
    ///          recorded in the AST.
    /// @param expr Call whose qualified or fallback callee and arguments are rendered.
    void visit(const CallExpr &expr) override {
        printer.os << '(';
        if (!expr.calleeQualified.empty()) {
            for (size_t i = 0; i < expr.calleeQualified.size(); ++i) {
                if (i)
                    printer.os << '.';
                printer.os << expr.calleeQualified[i];
            }
        } else {
            printer.os << expr.callee;
        }
        for (const auto &arg : expr.args) {
            printer.os << ' ';
            arg->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print an object construction expression.
    /// @details Serialises `(NEW ClassName args...)`, matching the lowering
    ///          pipeline's expectations when parsing dumps.
    /// @param expr Construction node whose type name and arguments are rendered.
    void visit(const NewExpr &expr) override {
        printer.os << "(NEW ";
        if (!expr.qualifiedType.empty()) {
            for (size_t i = 0; i < expr.qualifiedType.size(); ++i) {
                if (i)
                    printer.os << '.';
                printer.os << expr.qualifiedType[i];
            }
        } else {
            printer.os << expr.className;
        }
        for (const auto &arg : expr.args) {
            printer.os << ' ';
            arg->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print the ME receiver expression.
    /// @details Emits the keyword `ME`, mirroring how the source language refers
    ///          to the current instance inside type members.
    void visit(const MeExpr &) override {
        printer.os << "Me";
    }

    /// @brief Print a member access expression as base.member.
    /// @details Wraps the expression in parentheses to keep nesting unambiguous
    ///          and prints the base expression followed by `.` and the member
    ///          identifier.
    /// @param expr Member access whose base and member name are rendered.
    void visit(const MemberAccessExpr &expr) override {
        printer.os << '(';
        expr.base->accept(*this);
        printer.os << '.' << expr.member << ')';
    }

    /// @brief Print a method invocation on an object instance.
    /// @details Prints the receiver expression, the method name, and each
    ///          argument using the same prefix convention as other calls so
    ///          chained invocations remain easy to read.
    /// @param expr Method invocation to render.
    void visit(const MethodCallExpr &expr) override {
        printer.os << '(';
        expr.base->accept(*this);
        printer.os << '.' << expr.method;
        for (const auto &arg : expr.args) {
            printer.os << ' ';
            arg->accept(*this);
        }
        printer.os << ')';
    }

    /// @brief Print an IS expression as (IS <expr> <dotted-type>).
    /// @param expr Runtime type test to render.
    void visit(const IsExpr &expr) override {
        printer.os << "(IS ";
        expr.value->accept(*this);
        printer.os << ' ';
        for (size_t i = 0; i < expr.typeName.size(); ++i) {
            if (i)
                printer.os << '.';
            printer.os << expr.typeName[i];
        }
        printer.os << ')';
    }

    /// @brief Print an AS expression as (AS <expr> <dotted-type>).
    /// @param expr Runtime cast to render.
    void visit(const AsExpr &expr) override {
        printer.os << "(AS ";
        expr.value->accept(*this);
        printer.os << ' ';
        for (size_t i = 0; i < expr.typeName.size(); ++i) {
            if (i)
                printer.os << '.';
            printer.os << expr.typeName[i];
        }
        printer.os << ')';
    }

    /// @brief Print an ADDRESSOF expression as (ADDRESSOF <name>).
    /// @param expr Procedure-reference expression to render.
    void visit(const AddressOfExpr &expr) override {
        printer.os << "(ADDRESSOF " << expr.targetName << ')';
    }

  private:
    ///< Borrowed token destination shared by the traversal.
    Printer &printer;
};

/// @brief Entry point used by AstPrinter to render an expression node.
/// @details Constructs the visitor and forwards the expression and style, which
///          allows the caller to remain agnostic to the concrete visitor
///          implementation.
/// @param expr Expression to dispatch.
/// @param printer Destination token stream and indentation state.
/// @param style Formatting state retained for interface symmetry.
void AstPrinter::printExpr(const Expr &expr, Printer &printer, PrintStyle &style) {
    ExprPrinter exprPrinter{printer, style};
    exprPrinter.print(expr);
}

} // namespace il::frontends::basic
