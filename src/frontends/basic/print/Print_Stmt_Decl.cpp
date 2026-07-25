//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/print/Print_Stmt_Decl.cpp
// Purpose: Emit BASIC declaration and binding statements for the AST printer.
// Whitespace invariants: Helper functions only emit single spaces where the
//   original printer did, relying on Context utilities for nested spacing.
// Ownership/Lifetime: Context and statement nodes are managed by the caller.
// Notes: Utility lambdas ensure consistent parameter and field rendering.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements canonical serialization of BASIC declarations,
///        assignments, classes, interfaces, properties, and DELETE.

#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/print/Print_Stmt_Common.hpp"

namespace il::frontends::basic::print_stmt {
namespace {
/// @brief Print a space-separated parameter list with array suffix markers.
/// @details Emits each parameter name, inserting `()` for array parameters to
///          mirror BASIC's declaration syntax. Items are separated by single
///          spaces so the resulting `(PARAMS ...)` form stays compact.
/// @param params Parameter descriptors gathered from the AST node.
/// @param ctx Printer context providing the destination stream.
void printParamList(const std::vector<Param> &params, Context &ctx) {
    bool firstParam = true;
    for (const auto &param : params) {
        if (!firstParam) {
            ctx.stream() << ' ';
        }
        firstParam = false;
        ctx.stream() << param.name;
        if (param.is_array) {
            ctx.stream() << "()";
        }
    }
}

/// @brief Emit the `(FIELDS ...)` section for class/type declarations.
/// @details Appends each field as `name:type` after the `(FIELDS` prefix. When
///          the field list is empty the helper emits nothing so callers can
///          elide the section entirely.
/// @tparam FieldT Struct-like type exposing `name` and `type` members.
/// @param fields Sequence of fields to render.
/// @param ctx Printer context with formatting helpers.
template <typename FieldT> void printFields(const std::vector<FieldT> &fields, Context &ctx) {
    if (fields.empty()) {
        return;
    }
    auto &os = ctx.stream();
    os << " (FIELDS";
    for (const auto &field : fields) {
        os << ' ' << field.name << ':' << typeToString(field.type);
    }
    os << ')';
}
} // namespace

/// @brief Print a `LET` assignment statement.
/// @details Produces `(LET <target> <expr>)`, delegating to the context to
///          render both expressions so nested formatting remains consistent.
/// @param stmt Assignment statement describing the target and expression.
/// @param ctx Printer context that owns the stream.
void printLet(const LetStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(LET ";
    ctx.printExpr(*stmt.target);
    os << ' ';
    ctx.printExpr(*stmt.expr);
    os << ')';
}

/// @brief Emit a `CONST` statement describing constant declarations.
/// @details Prints the constant name, initializer expression, and optional type.
/// @param stmt CONST statement to render.
/// @param ctx Printer context responsible for nested expressions.
void printConst(const ConstStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(CONST " << stmt.name << " = ";
    ctx.printExpr(*stmt.initializer);
    if (stmt.type != Type::I64) {
        os << " AS " << typeToString(stmt.type);
    }
    os << ')';
}

/// @brief Emit a `DIM` statement describing array or scalar declarations.
/// @details Handles scalar declarations by appending `AS <type>` and array
///          declarations by optionally printing the size and explicit type when
///          present in the AST. When an array omits its type and defaults to
///          integer the helper skips the redundant `AS` clause.
/// @param stmt DIM statement to render.
/// @param ctx Printer context responsible for nested expressions.
void printDim(const DimStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(DIM " << stmt.name;
    if (stmt.isArray) {
        if (stmt.size) {
            os << ' ';
            ctx.printExpr(*stmt.size);
        }
        if (stmt.type != Type::I64) {
            os << " AS " << typeToString(stmt.type);
        }
    } else {
        os << " AS " << typeToString(stmt.type);
    }
    os << ')';
}

/// @brief Print a `REDIM` statement for resizing arrays.
/// @details Emits the array name and optional size expression, matching the
///          compact s-expression used across BASIC printer output.
/// @param stmt REDIM statement describing the target array.
/// @param ctx Printer context supplying expression formatting.
void printReDim(const ReDimStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(REDIM " << stmt.name;
    if (stmt.size) {
        os << ' ';
        ctx.printExpr(*stmt.size);
    } else {
        for (const auto &dim : stmt.dimensions) {
            if (!dim)
                continue;
            os << ' ';
            ctx.printExpr(*dim);
        }
    }
    os << ')';
}

/// @brief Render a function declaration including signature and body.
/// @details Prints `(FUNCTION <name> RET <type> (<params...>))` and then emits
///          the numbered body using @ref Context::printNumberedBody so nested
///          statements maintain their original line numbers.
/// @param stmt Function declaration node with parameters and body.
/// @param ctx Printer context delegating nested printing tasks.
void printFunction(const FunctionDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(FUNCTION " << stmt.name;
    os << " qualifiedName: ";
    if (!stmt.qualifiedName.empty()) {
        os << stmt.qualifiedName;
    } else {
        os << "<null>";
    }
    os << " RET " << typeToString(stmt.ret) << " (";
    printParamList(stmt.params, ctx);
    os << ")";
    if (stmt.explicitRetType != BasicType::Unknown && stmt.explicitRetType != BasicType::Void) {
        os << " AS " << toString(stmt.explicitRetType);
    }
    ctx.printNumberedBody(stmt.body);
}

/// @brief Render a `SUB` declaration with parameters and body.
/// @details The helper mirrors @ref printFunction but omits the return type to
///          reflect BASIC's subroutine syntax.
/// @param stmt Subroutine declaration to print.
/// @param ctx Printer context responsible for nested formatting.
void printSub(const SubDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(SUB " << stmt.name;
    os << " qualifiedName: ";
    if (!stmt.qualifiedName.empty()) {
        os << stmt.qualifiedName;
    } else {
        os << "<null>";
    }
    os << " (";
    printParamList(stmt.params, ctx);
    os << ")";
    ctx.printNumberedBody(stmt.body);
}

/// @brief Print a class constructor declaration.
/// @details Outputs `(CONSTRUCTOR (<params...>))` followed by the numbered body
///          for consistency with other procedure declarations.
/// @param stmt Constructor declaration holding parameters and body.
/// @param ctx Printer context providing stream access.
void printConstructor(const ConstructorDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(CONSTRUCTOR";
    if (stmt.isStatic)
        os << " STATIC";
    os << " (";
    printParamList(stmt.params, ctx);
    os << ")";
    ctx.printNumberedBody(stmt.body);
}

/// @brief Print a class destructor declaration.
/// @details Emits `(DESTRUCTOR` followed by the numbered body. Destructors have
///          no parameters, so only the body is rendered.
/// @param stmt Destructor declaration to print.
/// @param ctx Printer context managing body formatting.
void printDestructor(const DestructorDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(DESTRUCTOR";
    ctx.printNumberedBody(stmt.body);
}

/// @brief Render a class method declaration, optionally including return type.
/// @details Prints the method name, optional `RET` clause, and parameter list
///          before delegating body emission to the context.
/// @param stmt Method declaration node describing the member.
/// @param ctx Printer context with expression helpers.
void printMethod(const MethodDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(METHOD ";
    if (stmt.isStatic)
        os << "STATIC ";
    os << stmt.name;
    if (stmt.ret) {
        os << " RET " << typeToString(*stmt.ret);
    }
    os << " (";
    printParamList(stmt.params, ctx);
    os << ")";
    ctx.printNumberedBody(stmt.body);
}

/// @brief Emit a class declaration including fields and member body.
/// @details Writes `(CLASS <name> (FIELDS ...))` when fields exist, then prints
///          the numbered member body via the context to cover constructors,
///          methods, and nested declarations.
/// @param stmt Class declaration AST node.
/// @param ctx Printer context handling nested emission.
namespace {
// Specialised print for CLASS fields so we can include STATIC markers.
void printClassFields(const std::vector<ClassDecl::Field> &fields, Context &ctx) {
    if (fields.empty())
        return;
    auto &os = ctx.stream();
    os << " (FIELDS";
    for (const auto &field : fields) {
        os << ' ';
        if (field.isStatic)
            os << "STATIC ";
        os << field.name << ':' << typeToString(field.type);
    }
    os << ')';
}
} // namespace

void printClass(const ClassDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(CLASS " << stmt.name;
    os << " qualifiedName: ";
    if (!stmt.qualifiedName.empty()) {
        os << stmt.qualifiedName;
    } else {
        os << "<null>";
    }
    printClassFields(stmt.fields, ctx);
    // Implements list
    if (!stmt.implementsQualifiedNames.empty()) {
        os << " (IMPLEMENTS";
        for (const auto &qn : stmt.implementsQualifiedNames) {
            os << ' ';
            for (size_t i = 0; i < qn.size(); ++i) {
                if (i)
                    os << '.';
                os << qn[i];
            }
        }
        os << ')';
    }
    ctx.printNumberedBody(stmt.members);
}

/// @brief Render a user-defined TYPE declaration with field list.
/// @details Emits `(TYPE <name> (FIELDS ...))`, reusing @ref printFields to keep
///          the field formatting consistent between classes and types.
/// @param stmt TYPE declaration describing the aggregate.
/// @param ctx Printer context for stream access.
void printType(const TypeDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(TYPE " << stmt.name;
    printFields(stmt.fields, ctx);
    os << ')';
}

/// @brief Emit an INTERFACE declaration including abstract members.
/// @details Prints `(INTERFACE A.B.I)` and then the numbered member body.
/// @param stmt Interface declaration AST node.
/// @param ctx Printer context handling nested emission.
void printInterface(const InterfaceDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(INTERFACE ";
    for (size_t i = 0; i < stmt.qualifiedName.size(); ++i) {
        if (i)
            os << '.';
        os << stmt.qualifiedName[i];
    }
    ctx.printNumberedBody(stmt.members);
}

/// @brief Emit a PROPERTY declaration including optional accessors.
/// @details Prints `(PROPERTY [STATIC] <name> :<type> ...)` and renders GET/SET
///          blocks when present.
/// @param stmt Property declaration AST node.
/// @param ctx Printer context handling nested emission.
void printProperty(const PropertyDecl &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(PROPERTY ";
    if (stmt.isStatic)
        os << "STATIC ";
    os << stmt.name << ':' << typeToString(stmt.type);
    // Getter
    if (stmt.get.present) {
        os << " (GET";
        if (stmt.get.access != stmt.access)
            os << ' ' << (stmt.get.access == Access::Public ? "PUBLIC" : "PRIVATE");
        ctx.printNumberedBody(stmt.get.body);
        os << ')';
    }
    // Setter
    if (stmt.set.present) {
        os << " (SET";
        if (stmt.set.access != stmt.access)
            os << ' ' << (stmt.set.access == Access::Public ? "PUBLIC" : "PRIVATE");
        os << " param:" << stmt.set.paramName;
        ctx.printNumberedBody(stmt.set.body);
        os << ')';
    }
    os << ')';
}

/// @brief Print a DELETE statement targeting a specific expression.
/// @details Produces `(DELETE <expr>)`, delegating to the context to render the
///          expression so array subscripts and field access follow standard
///          formatting rules.
/// @param stmt DELETE statement to output.
/// @param ctx Printer context owning the output stream.
void printDelete(const DeleteStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(DELETE ";
    ctx.printExpr(*stmt.target);
    os << ')';
}

} // namespace il::frontends::basic::print_stmt
