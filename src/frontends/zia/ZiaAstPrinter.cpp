//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file ZiaAstPrinter.cpp
/// @brief Implements the Zia AST tree-walking printer.
///
/// @details Walks all declaration, statement, expression, and type nodes
/// in the Zia AST and produces a stable, indentation-based textual dump
/// suitable for debugging, golden tests, and diagnostics.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/ZiaAstPrinter.hpp"

#include <sstream>

namespace il::frontends::zia {

namespace {

// ---------------------------------------------------------------------------
// Printer helper -- manages indentation and line output.
// ---------------------------------------------------------------------------

/// @brief Stateful indentation helper for one AST dump stream.
struct Printer {
    /// @brief Bind the helper to the stream that receives printed AST output.
    /// @param out Destination stream used by all subsequent line writes.
    explicit Printer(std::ostream &out) : os(out) {}

    /// Destination stream borrowed for the helper lifetime.
    std::ostream &os;
    /// Current indentation depth in two-space units.
    int indent = 0;

    /// @brief Write @p text on a new line, honoured by current indentation.
    /// @param text Line contents without a trailing newline.
    void line(const std::string &text) {
        for (int i = 0; i < indent; ++i)
            os << "  ";
        os << text << '\n';
    }

    /// @brief Increase indentation by one level.
    void push() {
        ++indent;
    }

    /// @brief Decrease indentation by one level.
    /// @pre A matching @ref push call has established a positive indentation level.
    void pop() {
        --indent;
    }
};

// ---------------------------------------------------------------------------
// Forward declarations for mutually-recursive print helpers.
// ---------------------------------------------------------------------------

static void printDecl(const Decl &decl, Printer &p);
static void printStmt(const Stmt &stmt, Printer &p);
static void printExpr(const Expr &expr, Printer &p);
static void printType(const TypeNode &type, Printer &p);

// printExpr is split into per-category group helpers; each returns true when it
// handled @p expr.kind, false to let the next group try.
static bool printLeafExpr(const Expr &expr, Printer &p);
static bool printOperatorExpr(const Expr &expr, Printer &p);
static bool printConstructionExpr(const Expr &expr, Printer &p);
static bool printControlFlowExpr(const Expr &expr, Printer &p);

// ---------------------------------------------------------------------------
// Location formatting
// ---------------------------------------------------------------------------

/// @brief Format a source location as "(line:col)".
/// @param loc Source location to format; the file identifier is intentionally omitted.
/// @return Parenthesized one-based line and column.
static std::string locStr(const SourceLoc &loc) {
    std::ostringstream s;
    s << "(" << loc.line << ":" << loc.column << ")";
    return s.str();
}

// ---------------------------------------------------------------------------
// Operator name helpers
// ---------------------------------------------------------------------------

/// @brief Source spelling of a binary operator (e.g. Add → "+"); "?" if
///        unknown. Used to render BinaryExpr nodes in the AST dump.
/// @param op Binary operator enumerator.
/// @return Static source spelling.
static const char *binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:
            return "+";
        case BinaryOp::Sub:
            return "-";
        case BinaryOp::Mul:
            return "*";
        case BinaryOp::Div:
            return "/";
        case BinaryOp::Mod:
            return "%";
        case BinaryOp::Eq:
            return "==";
        case BinaryOp::Ne:
            return "!=";
        case BinaryOp::Lt:
            return "<";
        case BinaryOp::Le:
            return "<=";
        case BinaryOp::Gt:
            return ">";
        case BinaryOp::Ge:
            return ">=";
        case BinaryOp::And:
            return "&&";
        case BinaryOp::Or:
            return "||";
        case BinaryOp::BitAnd:
            return "&";
        case BinaryOp::BitOr:
            return "|";
        case BinaryOp::BitXor:
            return "^";
        case BinaryOp::Shl:
            return "<<";
        case BinaryOp::Shr:
            return ">>";
        case BinaryOp::Assign:
            return "=";
    }
    return "?";
}

/// @brief Source spelling of a unary operator (e.g. Neg → "-"); "?" if
///        unknown. Used to render UnaryExpr nodes in the AST dump.
/// @param op Unary operator enumerator.
/// @return Static source spelling.
static const char *unaryOpName(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg:
            return "-";
        case UnaryOp::Not:
            return "!";
        case UnaryOp::BitNot:
            return "~";
        case UnaryOp::AddressOf:
            return "&";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Type printing
// ---------------------------------------------------------------------------

/// @brief Recursively print a type node (named/generic/optional/…) to the
///        AST dump via @p p, one indented line per structural part.
/// @param type Type node to print.
/// @param p Indentation/output helper.
static void printType(const TypeNode &type, Printer &p) {
    switch (type.kind) {
        case TypeKind::Named: {
            const auto &n = static_cast<const NamedType &>(type);
            p.line("NamedType \"" + n.name + "\" " + locStr(n.loc));
            break;
        }
        case TypeKind::Generic: {
            const auto &g = static_cast<const GenericType &>(type);
            p.line("GenericType \"" + g.name + "\" " + locStr(g.loc));
            p.push();
            for (const auto &arg : g.args) {
                if (arg)
                    printType(*arg, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case TypeKind::Optional: {
            const auto &o = static_cast<const OptionalType &>(type);
            p.line("OptionalType " + locStr(o.loc));
            p.push();
            if (o.inner)
                printType(*o.inner, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case TypeKind::Function: {
            const auto &f = static_cast<const FunctionType &>(type);
            p.line("FunctionType " + locStr(f.loc));
            p.push();
            if (!f.params.empty()) {
                p.line("Params:");
                p.push();
                for (const auto &param : f.params) {
                    if (param)
                        printType(*param, p);
                    else
                        p.line("<null>");
                }
                p.pop();
            }
            if (f.returnType) {
                p.line("ReturnType:");
                p.push();
                printType(*f.returnType, p);
                p.pop();
            }
            p.pop();
            break;
        }
        case TypeKind::Tuple: {
            const auto &t = static_cast<const TupleType &>(type);
            p.line("TupleType " + locStr(t.loc));
            p.push();
            for (const auto &elem : t.elements) {
                if (elem)
                    printType(*elem, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case TypeKind::FixedArray: {
            const auto &fa = static_cast<const FixedArrayType &>(type);
            p.line("FixedArrayType [" + std::to_string(fa.count) + "] " + locStr(fa.loc));
            p.push();
            if (fa.elementType)
                printType(*fa.elementType, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Pattern printing (for match arms)
// ---------------------------------------------------------------------------

/// @brief Print a match-arm pattern (literal/binding/tuple/enum/…) to the dump.
/// @param pat Pattern to print.
/// @param p Indentation/output helper.
static void printPattern(const MatchArm::Pattern &pat, Printer &p) {
    switch (pat.kind) {
        case MatchArm::Pattern::Kind::Wildcard:
            p.line("WildcardPattern");
            break;
        case MatchArm::Pattern::Kind::Literal:
            p.line("LiteralPattern");
            p.push();
            if (pat.literal)
                printExpr(*pat.literal, p);
            else
                p.line("<null>");
            p.pop();
            break;
        case MatchArm::Pattern::Kind::Binding:
            p.line("BindingPattern \"" + pat.binding + "\"");
            break;
        case MatchArm::Pattern::Kind::Constructor:
            p.line("ConstructorPattern \"" + pat.binding + "\"");
            if (!pat.subpatterns.empty()) {
                p.push();
                for (const auto &sub : pat.subpatterns)
                    printPattern(sub, p);
                p.pop();
            }
            break;
        case MatchArm::Pattern::Kind::Tuple:
            p.line("TuplePattern");
            if (!pat.subpatterns.empty()) {
                p.push();
                for (const auto &sub : pat.subpatterns)
                    printPattern(sub, p);
                p.pop();
            }
            break;
        case MatchArm::Pattern::Kind::Expression:
            p.line("ExpressionPattern");
            p.push();
            if (pat.literal)
                printExpr(*pat.literal, p);
            else
                p.line("<null>");
            p.pop();
            break;
        case MatchArm::Pattern::Kind::Or:
            p.line("OrPattern");
            if (!pat.subpatterns.empty()) {
                p.push();
                for (const auto &sub : pat.subpatterns)
                    printPattern(sub, p);
                p.pop();
            }
            break;
    }
    if (pat.guard) {
        p.push();
        p.line("Guard:");
        p.push();
        printExpr(*pat.guard, p);
        p.pop();
        p.pop();
    }
}

/// @brief Print all arms of a `match` (pattern, optional guard, body) to the dump.
/// @param arms Match arms in source order.
/// @param p Indentation/output helper.
static void printMatchArms(const std::vector<MatchArm> &arms, Printer &p) {
    p.line("Arms:");
    p.push();
    for (const auto &arm : arms) {
        p.line("MatchArm");
        p.push();
        p.line("Pattern:");
        p.push();
        printPattern(arm.pattern, p);
        p.pop();
        p.line("Body:");
        p.push();
        if (arm.body)
            printExpr(*arm.body, p);
        else
            p.line("<null>");
        p.pop();
        p.pop();
    }
    p.pop();
}

// ---------------------------------------------------------------------------
// Expression printing
// ---------------------------------------------------------------------------

/// @brief Recursively print an expression subtree to the AST dump.
/// @param expr Expression root.
/// @param p Indentation/output helper.
/// @details Dispatches through leaf, operator, construction, and control-flow category printers.
static void printExpr(const Expr &expr, Printer &p) {
    if (printLeafExpr(expr, p))
        return;
    if (printOperatorExpr(expr, p))
        return;
    if (printConstructionExpr(expr, p))
        return;
    printControlFlowExpr(expr, p);
}

/// @brief Print literal and name expressions (leaves with no child exprs).
/// @param expr Candidate expression.
/// @param p Indentation/output helper.
/// @return True when this category handled @p expr.
static bool printLeafExpr(const Expr &expr, Printer &p) {
    switch (expr.kind) {
        // -- Literals -------------------------------------------------------
        case ExprKind::IntLiteral: {
            const auto &e = static_cast<const IntLiteralExpr &>(expr);
            p.line("IntLiteral " + std::to_string(e.value) + " " + locStr(e.loc));
            break;
        }
        case ExprKind::NumberLiteral: {
            const auto &e = static_cast<const NumberLiteralExpr &>(expr);
            std::ostringstream val;
            val << e.value;
            p.line("NumberLiteral " + val.str() + " " + locStr(e.loc));
            break;
        }
        case ExprKind::StringLiteral: {
            const auto &e = static_cast<const StringLiteralExpr &>(expr);
            p.line("StringLiteral \"" + e.value + "\" " + locStr(e.loc));
            break;
        }
        case ExprKind::BoolLiteral: {
            const auto &e = static_cast<const BoolLiteralExpr &>(expr);
            p.line(std::string("BoolLiteral ") + (e.value ? "true" : "false") + " " +
                   locStr(e.loc));
            break;
        }
        case ExprKind::NullLiteral: {
            p.line("NullLiteral " + locStr(expr.loc));
            break;
        }
        case ExprKind::UnitLiteral: {
            p.line("UnitLiteral " + locStr(expr.loc));
            break;
        }

        // -- Names ----------------------------------------------------------
        case ExprKind::Ident: {
            const auto &e = static_cast<const IdentExpr &>(expr);
            p.line("IdentExpr \"" + e.name + "\" " + locStr(e.loc));
            break;
        }
        case ExprKind::SelfExpr: {
            p.line("SelfExpr " + locStr(expr.loc));
            break;
        }
        case ExprKind::SuperExpr: {
            p.line("SuperExpr " + locStr(expr.loc));
            break;
        }
        default:
            return false;
    }
    return true;
}

/// @brief Print operator / access / cast expressions.
/// @brief Print operator and member-access expression families.
/// @param expr Candidate expression.
/// @param p Indentation/output helper.
/// @return True when this category handled @p expr.
static bool printOperatorExpr(const Expr &expr, Printer &p) {
    switch (expr.kind) {
        // -- Operators ------------------------------------------------------
        case ExprKind::Binary: {
            const auto &e = static_cast<const BinaryExpr &>(expr);
            p.line(std::string("BinaryExpr (") + binaryOpName(e.op) + ") " + locStr(e.loc));
            p.push();
            if (e.left)
                printExpr(*e.left, p);
            else
                p.line("<null>");
            if (e.right)
                printExpr(*e.right, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::Unary: {
            const auto &e = static_cast<const UnaryExpr &>(expr);
            p.line(std::string("UnaryExpr (") + unaryOpName(e.op) + ") " + locStr(e.loc));
            p.push();
            if (e.operand)
                printExpr(*e.operand, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::Ternary: {
            const auto &e = static_cast<const TernaryExpr &>(expr);
            p.line("TernaryExpr " + locStr(e.loc));
            p.push();
            p.line("Condition:");
            p.push();
            if (e.condition)
                printExpr(*e.condition, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Then:");
            p.push();
            if (e.thenExpr)
                printExpr(*e.thenExpr, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Else:");
            p.push();
            if (e.elseExpr)
                printExpr(*e.elseExpr, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::Call: {
            const auto &e = static_cast<const CallExpr &>(expr);
            p.line("CallExpr " + locStr(e.loc));
            p.push();
            p.line("Callee:");
            p.push();
            if (e.callee)
                printExpr(*e.callee, p);
            else
                p.line("<null>");
            p.pop();
            if (!e.args.empty()) {
                p.line("Args:");
                p.push();
                for (const auto &arg : e.args) {
                    if (arg.name)
                        p.line("NamedArg \"" + *arg.name + "\":");
                    else
                        p.line("Arg:");
                    p.push();
                    if (arg.value)
                        printExpr(*arg.value, p);
                    else
                        p.line("<null>");
                    p.pop();
                }
                p.pop();
            }
            p.pop();
            break;
        }
        case ExprKind::Index: {
            const auto &e = static_cast<const IndexExpr &>(expr);
            p.line("IndexExpr " + locStr(e.loc));
            p.push();
            p.line("Base:");
            p.push();
            if (e.base)
                printExpr(*e.base, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Index:");
            p.push();
            if (e.index)
                printExpr(*e.index, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::Field: {
            const auto &e = static_cast<const FieldExpr &>(expr);
            p.line("FieldExpr \"" + e.field + "\" " + locStr(e.loc));
            p.push();
            if (e.base)
                printExpr(*e.base, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::OptionalChain: {
            const auto &e = static_cast<const OptionalChainExpr &>(expr);
            p.line("OptionalChainExpr \"" + e.field + "\" " + locStr(e.loc));
            p.push();
            if (e.base)
                printExpr(*e.base, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::Coalesce: {
            const auto &e = static_cast<const CoalesceExpr &>(expr);
            p.line("CoalesceExpr " + locStr(e.loc));
            p.push();
            if (e.left)
                printExpr(*e.left, p);
            else
                p.line("<null>");
            if (e.right)
                printExpr(*e.right, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::Is: {
            const auto &e = static_cast<const IsExpr &>(expr);
            p.line("IsExpr " + locStr(e.loc));
            p.push();
            p.line("Value:");
            p.push();
            if (e.value)
                printExpr(*e.value, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Type:");
            p.push();
            if (e.type)
                printType(*e.type, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::As: {
            const auto &e = static_cast<const AsExpr &>(expr);
            p.line("AsExpr " + locStr(e.loc));
            p.push();
            p.line("Value:");
            p.push();
            if (e.value)
                printExpr(*e.value, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Type:");
            p.push();
            if (e.type)
                printType(*e.type, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::Range: {
            const auto &e = static_cast<const RangeExpr &>(expr);
            p.line(std::string("RangeExpr ") + (e.inclusive ? "..=" : "..") + " " + locStr(e.loc));
            p.push();
            p.line("Start:");
            p.push();
            if (e.start)
                printExpr(*e.start, p);
            else
                p.line("<null>");
            p.pop();
            p.line("End:");
            p.push();
            if (e.end)
                printExpr(*e.end, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::Try: {
            const auto &e = static_cast<const TryExpr &>(expr);
            p.line("TryExpr " + locStr(e.loc));
            p.push();
            if (e.operand)
                printExpr(*e.operand, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::ForceUnwrap: {
            const auto &e = static_cast<const ForceUnwrapExpr &>(expr);
            p.line("ForceUnwrapExpr " + locStr(e.loc));
            p.push();
            if (e.operand)
                printExpr(*e.operand, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case ExprKind::Await: {
            const auto &e = static_cast<const AwaitExpr &>(expr);
            p.line("AwaitExpr " + locStr(e.loc));
            p.push();
            if (e.operand)
                printExpr(*e.operand, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        default:
            return false;
    }
    return true;
}

/// @brief Print object/collection construction expressions.
/// @brief Print call, allocation, lambda, and aggregate-construction expressions.
/// @param expr Candidate expression.
/// @param p Indentation/output helper.
/// @return True when this category handled @p expr.
static bool printConstructionExpr(const Expr &expr, Printer &p) {
    switch (expr.kind) {
        // -- Construction ---------------------------------------------------
        case ExprKind::New: {
            const auto &e = static_cast<const NewExpr &>(expr);
            p.line("NewExpr " + locStr(e.loc));
            p.push();
            p.line("Type:");
            p.push();
            if (e.type)
                printType(*e.type, p);
            else
                p.line("<null>");
            p.pop();
            if (!e.args.empty()) {
                p.line("Args:");
                p.push();
                for (const auto &arg : e.args) {
                    if (arg.name)
                        p.line("NamedArg \"" + *arg.name + "\":");
                    else
                        p.line("Arg:");
                    p.push();
                    if (arg.value)
                        printExpr(*arg.value, p);
                    else
                        p.line("<null>");
                    p.pop();
                }
                p.pop();
            }
            p.pop();
            break;
        }
        case ExprKind::StructLiteral: {
            const auto &e = static_cast<const StructLiteralExpr &>(expr);
            p.line("StructLiteralExpr \"" + e.typeName + "\" " + locStr(e.loc));
            p.push();
            for (const auto &field : e.fields) {
                p.line("Field \"" + field.name + "\" " + locStr(field.loc) + ":");
                p.push();
                if (field.value)
                    printExpr(*field.value, p);
                else
                    p.line("<null>");
                p.pop();
            }
            p.pop();
            break;
        }
        case ExprKind::Lambda: {
            const auto &e = static_cast<const LambdaExpr &>(expr);
            p.line("LambdaExpr " + locStr(e.loc));
            p.push();
            if (!e.params.empty()) {
                p.line("Params:");
                p.push();
                for (const auto &param : e.params) {
                    std::string paramStr = "LambdaParam \"" + param.name + "\"";
                    if (param.type) {
                        p.line(paramStr);
                        p.push();
                        p.line("Type:");
                        p.push();
                        printType(*param.type, p);
                        p.pop();
                        p.pop();
                    } else {
                        p.line(paramStr);
                    }
                }
                p.pop();
            }
            if (e.returnType) {
                p.line("ReturnType:");
                p.push();
                printType(*e.returnType, p);
                p.pop();
            }
            if (!e.captures.empty()) {
                p.line("Captures:");
                p.push();
                for (const auto &cap : e.captures) {
                    p.line("Capture \"" + cap.name + "\"");
                }
                p.pop();
            }
            p.line("Body:");
            p.push();
            if (e.body)
                printExpr(*e.body, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::ListLiteral: {
            const auto &e = static_cast<const ListLiteralExpr &>(expr);
            p.line("ListLiteralExpr " + locStr(e.loc));
            p.push();
            for (const auto &elem : e.elements) {
                if (elem)
                    printExpr(*elem, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case ExprKind::MapLiteral: {
            const auto &e = static_cast<const MapLiteralExpr &>(expr);
            p.line("MapLiteralExpr " + locStr(e.loc));
            p.push();
            for (const auto &entry : e.entries) {
                p.line("Entry:");
                p.push();
                p.line("Key:");
                p.push();
                if (entry.key)
                    printExpr(*entry.key, p);
                else
                    p.line("<null>");
                p.pop();
                p.line("Value:");
                p.push();
                if (entry.value)
                    printExpr(*entry.value, p);
                else
                    p.line("<null>");
                p.pop();
                p.pop();
            }
            p.pop();
            break;
        }
        case ExprKind::SetLiteral: {
            const auto &e = static_cast<const SetLiteralExpr &>(expr);
            p.line("SetLiteralExpr " + locStr(e.loc));
            p.push();
            for (const auto &elem : e.elements) {
                if (elem)
                    printExpr(*elem, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case ExprKind::Tuple: {
            const auto &e = static_cast<const TupleExpr &>(expr);
            p.line("TupleExpr " + locStr(e.loc));
            p.push();
            for (const auto &elem : e.elements) {
                if (elem)
                    printExpr(*elem, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case ExprKind::TupleIndex: {
            const auto &e = static_cast<const TupleIndexExpr &>(expr);
            p.line("TupleIndexExpr ." + std::to_string(e.index) + " " + locStr(e.loc));
            p.push();
            if (e.tuple)
                printExpr(*e.tuple, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        default:
            return false;
    }
    return true;
}

/// @brief Print control-flow expressions (if/match/block).
/// @brief Print conditional, match, range, and other control-flow-oriented expressions.
/// @param expr Candidate expression.
/// @param p Indentation/output helper.
/// @return True when this category handled @p expr.
static bool printControlFlowExpr(const Expr &expr, Printer &p) {
    switch (expr.kind) {
        // -- Control flow expressions ---------------------------------------
        case ExprKind::If: {
            const auto &e = static_cast<const IfExpr &>(expr);
            p.line("IfExpr " + locStr(e.loc));
            p.push();
            p.line("Condition:");
            p.push();
            if (e.condition)
                printExpr(*e.condition, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Then:");
            p.push();
            if (e.thenBranch)
                printExpr(*e.thenBranch, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Else:");
            p.push();
            if (e.elseBranch)
                printExpr(*e.elseBranch, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case ExprKind::Match: {
            const auto &e = static_cast<const MatchExpr &>(expr);
            p.line("MatchExpr " + locStr(e.loc));
            p.push();
            p.line("Scrutinee:");
            p.push();
            if (e.scrutinee)
                printExpr(*e.scrutinee, p);
            else
                p.line("<null>");
            p.pop();
            printMatchArms(e.arms, p);
            p.pop();
            break;
        }
        case ExprKind::Block: {
            const auto &e = static_cast<const BlockExpr &>(expr);
            p.line("BlockExpr " + locStr(e.loc));
            p.push();
            if (!e.statements.empty()) {
                p.line("Statements:");
                p.push();
                for (const auto &stmt : e.statements) {
                    if (stmt)
                        printStmt(*stmt, p);
                    else
                        p.line("<null>");
                }
                p.pop();
            }
            p.line("Value:");
            p.push();
            if (e.value)
                printExpr(*e.value, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        default:
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Statement printing
// ---------------------------------------------------------------------------

/// @brief Recursively print a statement subtree to the AST dump.
/// @brief Recursively print a statement and its child statements/expressions.
/// @param stmt Statement to print.
/// @param p Indentation/output helper.
static void printStmt(const Stmt &stmt, Printer &p) {
    switch (stmt.kind) {
        case StmtKind::Block: {
            const auto &s = static_cast<const BlockStmt &>(stmt);
            p.line("BlockStmt " + locStr(s.loc));
            p.push();
            for (const auto &child : s.statements) {
                if (child)
                    printStmt(*child, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case StmtKind::Expr: {
            const auto &s = static_cast<const ExprStmt &>(stmt);
            p.line("ExprStmt " + locStr(s.loc));
            p.push();
            if (s.expr)
                printExpr(*s.expr, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case StmtKind::Var: {
            const auto &s = static_cast<const VarStmt &>(stmt);
            std::string header = (s.isFinal ? "FinalStmt" : "VarStmt");
            if (s.isTupleDestructure && !s.tupleNames.empty()) {
                header += " (";
                for (size_t i = 0; i < s.tupleNames.size(); ++i) {
                    if (i)
                        header += ", ";
                    header += s.tupleNames[i];
                }
                header += ") " + locStr(s.loc);
            } else {
                header += " \"" + s.name + "\" " + locStr(s.loc);
            }
            p.line(header);
            p.push();
            if (s.isTupleDestructure && !s.tupleTypes.empty()) {
                for (size_t i = 0; i < s.tupleTypes.size(); ++i) {
                    if (!s.tupleTypes[i])
                        continue;
                    p.line("Type " + std::to_string(i) + ":");
                    p.push();
                    printType(*s.tupleTypes[i], p);
                    p.pop();
                }
            } else if (s.type) {
                p.line("Type:");
                p.push();
                printType(*s.type, p);
                p.pop();
            }
            if (s.initializer) {
                p.line("Initializer:");
                p.push();
                printExpr(*s.initializer, p);
                p.pop();
            }
            p.pop();
            break;
        }
        case StmtKind::If: {
            const auto &s = static_cast<const IfStmt &>(stmt);
            p.line("IfStmt " + locStr(s.loc));
            p.push();
            p.line("Condition:");
            p.push();
            if (s.condition)
                printExpr(*s.condition, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Then:");
            p.push();
            if (s.thenBranch)
                printStmt(*s.thenBranch, p);
            else
                p.line("<null>");
            p.pop();
            if (s.elseBranch) {
                p.line("Else:");
                p.push();
                printStmt(*s.elseBranch, p);
                p.pop();
            }
            p.pop();
            break;
        }
        case StmtKind::While: {
            const auto &s = static_cast<const WhileStmt &>(stmt);
            p.line("WhileStmt " + locStr(s.loc));
            p.push();
            p.line("Condition:");
            p.push();
            if (s.condition)
                printExpr(*s.condition, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Body:");
            p.push();
            if (s.body)
                printStmt(*s.body, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case StmtKind::For: {
            const auto &s = static_cast<const ForStmt &>(stmt);
            p.line("ForStmt " + locStr(s.loc));
            p.push();
            if (s.init) {
                p.line("Init:");
                p.push();
                printStmt(*s.init, p);
                p.pop();
            }
            if (s.condition) {
                p.line("Condition:");
                p.push();
                printExpr(*s.condition, p);
                p.pop();
            }
            if (s.update) {
                p.line("Update:");
                p.push();
                printExpr(*s.update, p);
                p.pop();
            }
            p.line("Body:");
            p.push();
            if (s.body)
                printStmt(*s.body, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case StmtKind::ForIn: {
            const auto &s = static_cast<const ForInStmt &>(stmt);
            std::string header = "ForInStmt \"" + s.variable + "\"";
            if (s.isTuple)
                header += ", \"" + s.secondVariable + "\"";
            header += " " + locStr(s.loc);
            p.line(header);
            p.push();
            if (s.variableType) {
                p.line("VariableType:");
                p.push();
                printType(*s.variableType, p);
                p.pop();
            }
            if (s.isTuple && s.secondVariableType) {
                p.line("SecondVariableType:");
                p.push();
                printType(*s.secondVariableType, p);
                p.pop();
            }
            p.line("Iterable:");
            p.push();
            if (s.iterable)
                printExpr(*s.iterable, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Body:");
            p.push();
            if (s.body)
                printStmt(*s.body, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case StmtKind::Return: {
            const auto &s = static_cast<const ReturnStmt &>(stmt);
            p.line("ReturnStmt " + locStr(s.loc));
            if (s.value) {
                p.push();
                printExpr(*s.value, p);
                p.pop();
            }
            break;
        }
        case StmtKind::Break: {
            p.line("BreakStmt " + locStr(stmt.loc));
            break;
        }
        case StmtKind::Continue: {
            p.line("ContinueStmt " + locStr(stmt.loc));
            break;
        }
        case StmtKind::Defer: {
            const auto &s = static_cast<const DeferStmt &>(stmt);
            p.line("DeferStmt " + locStr(s.loc));
            p.push();
            if (s.action)
                printStmt(*s.action, p);
            else
                p.line("<null>");
            p.pop();
            break;
        }
        case StmtKind::Guard: {
            const auto &s = static_cast<const GuardStmt &>(stmt);
            p.line("GuardStmt " + locStr(s.loc));
            p.push();
            p.line("Condition:");
            p.push();
            if (s.condition)
                printExpr(*s.condition, p);
            else
                p.line("<null>");
            p.pop();
            p.line("Else:");
            p.push();
            if (s.elseBlock)
                printStmt(*s.elseBlock, p);
            else
                p.line("<null>");
            p.pop();
            p.pop();
            break;
        }
        case StmtKind::Match: {
            const auto &s = static_cast<const MatchStmt &>(stmt);
            p.line("MatchStmt " + locStr(s.loc));
            p.push();
            p.line("Scrutinee:");
            p.push();
            if (s.scrutinee)
                printExpr(*s.scrutinee, p);
            else
                p.line("<null>");
            p.pop();
            printMatchArms(s.arms, p);
            p.pop();
            break;
        }
        case StmtKind::Try: {
            const auto &s = static_cast<const TryStmt &>(stmt);
            std::string desc = "TryStmt " + locStr(s.loc);
            if (!s.catchTypeName.empty())
                desc += " catchType=" + s.catchTypeName;
            p.line(desc);
            break;
        }
        case StmtKind::Throw: {
            const auto &s = static_cast<const ThrowStmt &>(stmt);
            p.line("ThrowStmt " + locStr(s.loc));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers for printing common declaration parts
// ---------------------------------------------------------------------------

/// @brief Print a function/method parameter list (name, type, default) to the dump.
/// @param params Parameters in source order.
/// @param p Indentation/output helper.
static void printParams(const std::vector<Param> &params, Printer &p) {
    if (params.empty())
        return;
    p.line("Params:");
    p.push();
    for (const auto &param : params) {
        std::string header = "Param \"" + param.name + "\"";
        p.line(header);
        p.push();
        if (param.type) {
            p.line("Type:");
            p.push();
            printType(*param.type, p);
            p.pop();
        }
        if (param.defaultValue) {
            p.line("Default:");
            p.push();
            printExpr(*param.defaultValue, p);
            p.pop();
        }
        p.pop();
    }
    p.pop();
}

/// @brief Print generic type parameters with their `: Constraint` bounds to
///        the dump.
/// @param genericParams Generic parameter names.
/// @param constraints Positional interface constraints; empty entries are omitted.
/// @param p Indentation/output helper.
static void printGenericParamsWithConstraints(const std::vector<std::string> &genericParams,
                                              const std::vector<std::string> &constraints,
                                              Printer &p) {
    if (genericParams.empty())
        return;
    std::string gp = "GenericParams: [";
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (i > 0)
            gp += ", ";
        gp += genericParams[i];
        if (i < constraints.size() && !constraints[i].empty())
            gp += ": " + constraints[i];
    }
    gp += "]";
    p.line(gp);
}

/// @brief Print a type's implemented-interface list to the dump.
/// @param interfaces Interface names in declaration order.
/// @param p Indentation/output helper.
static void printInterfaces(const std::vector<std::string> &interfaces, Printer &p) {
    if (interfaces.empty())
        return;
    std::string ifaces = "Implements: [";
    for (size_t i = 0; i < interfaces.size(); ++i) {
        if (i > 0)
            ifaces += ", ";
        ifaces += interfaces[i];
    }
    ifaces += "]";
    p.line(ifaces);
}

/// @brief Print a declaration's public/private visibility to the dump.
/// @param vis Visibility enumerator.
/// @param p Indentation/output helper.
static void printVisibility(Visibility vis, Printer &p) {
    p.line(vis == Visibility::Public ? "Visibility: public" : "Visibility: private");
}

/// @brief Print a function/method body block (if present) to the dump.
/// @param body Optional owned statement body.
/// @param p Indentation/output helper.
static void printBody(const StmtPtr &body, Printer &p) {
    if (body) {
        p.line("Body:");
        p.push();
        printStmt(*body, p);
        p.pop();
    }
}

/// @brief Print a function/method declared return type (if present) to the dump.
/// @param retType Optional declared result type.
/// @param p Indentation/output helper.
static void printReturnType(const TypePtr &retType, Printer &p) {
    if (retType) {
        p.line("ReturnType:");
        p.push();
        printType(*retType, p);
        p.pop();
    }
}

/// @brief Print a type's member declarations (fields/methods) to the dump.
/// @param members Owned member declarations in source order.
/// @param p Indentation/output helper.
static void printMembers(const std::vector<DeclPtr> &members, Printer &p) {
    if (members.empty())
        return;
    p.line("Members:");
    p.push();
    for (const auto &member : members) {
        if (member)
            printDecl(*member, p);
        else
            p.line("<null>");
    }
    p.pop();
}

// ---------------------------------------------------------------------------
// Declaration printing
// ---------------------------------------------------------------------------

/// @brief Recursively print a declaration (function/type/global/…) and its
///        children to the AST dump.
/// @param decl Declaration to print.
/// @param p Indentation/output helper.
static void printDecl(const Decl &decl, Printer &p) {
    switch (decl.kind) {
        case DeclKind::Module: {
            // Module is handled by the top-level dump() -- but support it here
            // for completeness.
            const auto &d = static_cast<const ModuleDecl &>(decl);
            p.line("ModuleDecl \"" + d.name + "\" " + locStr(d.loc));
            p.push();
            for (const auto &bind : d.binds)
                printDecl(bind, p);
            for (const auto &child : d.declarations) {
                if (child)
                    printDecl(*child, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case DeclKind::Bind: {
            const auto &d = static_cast<const BindDecl &>(decl);
            std::string header = "BindDecl \"" + d.path + "\"";
            if (!d.alias.empty())
                header += " as \"" + d.alias + "\"";
            if (d.isNamespaceBind)
                header += " (namespace)";
            header += " " + locStr(d.loc);
            p.line(header);
            if (!d.specificItems.empty()) {
                p.push();
                std::string items = "Items: [";
                for (size_t i = 0; i < d.specificItems.size(); ++i) {
                    if (i > 0)
                        items += ", ";
                    items += d.specificItems[i];
                }
                items += "]";
                p.line(items);
                p.pop();
            }
            break;
        }
        case DeclKind::Struct: {
            const auto &d = static_cast<const StructDecl &>(decl);
            p.line("StructDecl \"" + d.name + "\" " + locStr(d.loc));
            p.push();
            printGenericParamsWithConstraints(d.genericParams, d.genericParamConstraints, p);
            printInterfaces(d.interfaces, p);
            printMembers(d.members, p);
            p.pop();
            break;
        }
        case DeclKind::Class: {
            const auto &d = static_cast<const ClassDecl &>(decl);
            std::string header = "ClassDecl \"" + d.name + "\"";
            if (!d.baseClass.empty())
                header += " extends \"" + d.baseClass + "\"";
            header += " " + locStr(d.loc);
            p.line(header);
            p.push();
            printGenericParamsWithConstraints(d.genericParams, d.genericParamConstraints, p);
            printInterfaces(d.interfaces, p);
            printMembers(d.members, p);
            p.pop();
            break;
        }
        case DeclKind::Interface: {
            const auto &d = static_cast<const InterfaceDecl &>(decl);
            p.line("InterfaceDecl \"" + d.name + "\" " + locStr(d.loc));
            p.push();
            printGenericParamsWithConstraints(d.genericParams, d.genericParamConstraints, p);
            printMembers(d.members, p);
            p.pop();
            break;
        }
        case DeclKind::Function: {
            const auto &d = static_cast<const FunctionDecl &>(decl);
            std::string header = "FunctionDecl \"" + d.name + "\"";
            if (d.isAsync)
                header += " (async)";
            if (d.isOverride)
                header += " (override)";
            header += " " + locStr(d.loc);
            p.line(header);
            p.push();
            printVisibility(d.visibility, p);
            printGenericParamsWithConstraints(d.genericParams, d.genericParamConstraints, p);
            printParams(d.params, p);
            printReturnType(d.returnType, p);
            printBody(d.body, p);
            p.pop();
            break;
        }
        case DeclKind::Field: {
            const auto &d = static_cast<const FieldDecl &>(decl);
            std::string header = "FieldDecl \"" + d.name + "\"";
            if (d.isFinal)
                header += " (final)";
            if (d.isWeak)
                header += " (weak)";
            header += " " + locStr(d.loc);
            p.line(header);
            p.push();
            printVisibility(d.visibility, p);
            if (d.type) {
                p.line("Type:");
                p.push();
                printType(*d.type, p);
                p.pop();
            }
            if (d.initializer) {
                p.line("Initializer:");
                p.push();
                printExpr(*d.initializer, p);
                p.pop();
            }
            p.pop();
            break;
        }
        case DeclKind::Method: {
            const auto &d = static_cast<const MethodDecl &>(decl);
            std::string header = "MethodDecl \"" + d.name + "\"";
            if (d.isOverride)
                header += " (override)";
            header += " " + locStr(d.loc);
            p.line(header);
            p.push();
            printVisibility(d.visibility, p);
            printGenericParamsWithConstraints(d.genericParams, d.genericParamConstraints, p);
            printParams(d.params, p);
            printReturnType(d.returnType, p);
            printBody(d.body, p);
            p.pop();
            break;
        }
        case DeclKind::Constructor: {
            const auto &d = static_cast<const ConstructorDecl &>(decl);
            p.line("ConstructorDecl " + locStr(d.loc));
            p.push();
            printVisibility(d.visibility, p);
            printParams(d.params, p);
            printBody(d.body, p);
            p.pop();
            break;
        }
        case DeclKind::GlobalVar: {
            const auto &d = static_cast<const GlobalVarDecl &>(decl);
            std::string header = (d.isFinal ? "GlobalFinalDecl" : "GlobalVarDecl");
            header += " \"" + d.name + "\" " + locStr(d.loc);
            p.line(header);
            p.push();
            if (d.type) {
                p.line("Type:");
                p.push();
                printType(*d.type, p);
                p.pop();
            }
            if (d.initializer) {
                p.line("Initializer:");
                p.push();
                printExpr(*d.initializer, p);
                p.pop();
            }
            p.pop();
            break;
        }
        case DeclKind::Namespace: {
            const auto &d = static_cast<const NamespaceDecl &>(decl);
            p.line("NamespaceDecl \"" + d.name + "\" " + locStr(d.loc));
            p.push();
            for (const auto &child : d.declarations) {
                if (child)
                    printDecl(*child, p);
                else
                    p.line("<null>");
            }
            p.pop();
            break;
        }
        case DeclKind::Property: {
            const auto &d = static_cast<const PropertyDecl &>(decl);
            p.line("PropertyDecl \"" + d.name + "\" " + locStr(d.loc));
            break;
        }
        case DeclKind::Destructor: {
            p.line("DestructorDecl " + locStr(decl.loc));
            break;
        }
        case DeclKind::Enum: {
            const auto &d = static_cast<const EnumDecl &>(decl);
            p.line("EnumDecl \"" + d.name + "\" " + locStr(d.loc));
            if (!d.variants.empty()) {
                p.push();
                for (const auto &v : d.variants) {
                    std::string vstr = "Variant \"" + v.name + "\"";
                    if (v.explicitValue.has_value())
                        vstr += " = " + std::to_string(v.explicitValue.value());
                    vstr += " " + locStr(v.loc);
                    p.line(vstr);
                }
                p.pop();
            }
            break;
        }
        case DeclKind::TypeAlias: {
            const auto &d = static_cast<const TypeAliasDecl &>(decl);
            p.line("TypeAliasDecl \"" + d.name + "\" " + locStr(d.loc));
            break;
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

/// @brief Serialize a complete Zia module as an indentation-based AST tree.
/// @param module Root module declaration.
/// @return Deterministic textual dump ending in a newline.
std::string ZiaAstPrinter::dump(const ModuleDecl &module) {
    std::ostringstream os;
    Printer p{os};
    printDecl(module, p);
    return os.str();
}

} // namespace il::frontends::zia
