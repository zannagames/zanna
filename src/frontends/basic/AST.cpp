//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/AST.cpp
// Purpose: Provide out-of-line accept/dispatch helpers for BASIC AST nodes.
//          The translation unit keeps the node headers lightweight while
//          centralising visitor entry points for every concrete node type.
// Ownership/Lifetime: Dispatch borrows both nodes and visitors for each call.
// Links: src/frontends/basic/AST.hpp,
//        src/frontends/basic/ast/ExprNodes.hpp,
//        src/frontends/basic/ast/StmtNodesAll.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements visitor forwarding shims for the BASIC AST hierarchy.
/// @details Each AST class exposes two overloads of @c accept: one for
///          read-only visitors and one for mutable visitors.  Keeping the
///          definitions out-of-line avoids repeatedly instantiating the same
///          forwarding code across translation units that include @c AST.hpp.
///
///          To add a new expression node:
///            1. Add the node class to the appropriate header (ExprNodes.hpp).
///            2. Add a visit() method to ExprVisitor and MutExprVisitor.
///            3. Add DEFINE_EXPR_ACCEPT(YourNewExpr) below.
///
///          To add a new statement node:
///            1. Add the node class to the appropriate header (StmtNodes*.hpp).
///            2. Add a visit() method to StmtVisitor and MutStmtVisitor.
///            3. Add DEFINE_STMT_ACCEPT(YourNewStmt) below.

#include "frontends/basic/AST.hpp"

namespace il::frontends::basic {

// ============================================================================
// Accept Method Generation Macros
// ============================================================================
//
// These macros generate the mechanical accept() implementations for AST nodes.
// Each node needs two accept overloads: one for const visitors (read-only
// traversal) and one for mutable visitors (rewriting passes).
//
// The pattern is identical for all nodes:
//   void NodeType::accept(VisitorType &v) [const] { v.visit(*this); }

/// @brief Define accept methods for expression nodes.
/// @details Each invocation emits the const ExprVisitor overload and mutable
///          MutExprVisitor overload. Both borrow the visitor and immediately
///          call its matching `visit(NodeType&)` overload with `*this`.
/// @param NodeType The concrete expression class name.
#define DEFINE_EXPR_ACCEPT(NodeType)                                                               \
    void NodeType::accept(ExprVisitor &visitor) const {                                            \
        visitor.visit(*this);                                                                      \
    }                                                                                              \
    void NodeType::accept(MutExprVisitor &visitor) {                                               \
        visitor.visit(*this);                                                                      \
    }

/// @brief Define accept methods for statement nodes.
/// @details Each invocation emits the const StmtVisitor overload and mutable
///          MutStmtVisitor overload. Both borrow the visitor and immediately
///          call its matching `visit(NodeType&)` overload with `*this`.
/// @param NodeType The concrete statement class name.
#define DEFINE_STMT_ACCEPT(NodeType)                                                               \
    void NodeType::accept(StmtVisitor &visitor) const {                                            \
        visitor.visit(*this);                                                                      \
    }                                                                                              \
    void NodeType::accept(MutStmtVisitor &visitor) {                                               \
        visitor.visit(*this);                                                                      \
    }

// ============================================================================
// Free-standing visit() dispatch helpers
// ============================================================================

/// @brief Forward a const expression node to a visitor implementation.
/// @details Wraps the polymorphic @c accept call so clients can invoke
///          `visit(expr, visitor)` without naming the exact derived type.
/// @param expr Expression node to visit.
/// @param visitor Visitor receiving the node.
void visit(const Expr &expr, ExprVisitor &visitor) {
    expr.accept(visitor);
}

/// @brief Forward a mutable expression node to a visitor implementation.
/// @details Enables uniform visitation over mutable AST nodes by deferring to
///          the node's @c accept overload.
/// @param expr Expression node to visit and potentially mutate.
/// @param visitor Visitor receiving the node.
void visit(Expr &expr, MutExprVisitor &visitor) {
    expr.accept(visitor);
}

/// @brief Forward a const statement node to a visitor implementation.
/// @details Used when traversing immutable ASTs; delegates to @c accept to
///          perform the double-dispatch.
/// @param stmt Statement node to visit.
/// @param visitor Visitor receiving the node.
void visit(const Stmt &stmt, StmtVisitor &visitor) {
    stmt.accept(visitor);
}

/// @brief Forward a mutable statement node to a visitor implementation.
/// @details Invokes the node's @c accept overload so visitors can mutate the
///          statement in-place.
/// @param stmt Statement node to visit and potentially mutate.
/// @param visitor Visitor receiving the node.
void visit(Stmt &stmt, MutStmtVisitor &visitor) {
    stmt.accept(visitor);
}

// ============================================================================
// Expression Node Accept Implementations
// ============================================================================

DEFINE_EXPR_ACCEPT(IntExpr)
DEFINE_EXPR_ACCEPT(FloatExpr)
DEFINE_EXPR_ACCEPT(StringExpr)
DEFINE_EXPR_ACCEPT(BoolExpr)
DEFINE_EXPR_ACCEPT(VarExpr)
DEFINE_EXPR_ACCEPT(ArrayExpr)
DEFINE_EXPR_ACCEPT(LBoundExpr)
DEFINE_EXPR_ACCEPT(UBoundExpr)
DEFINE_EXPR_ACCEPT(UnaryExpr)
DEFINE_EXPR_ACCEPT(BinaryExpr)
DEFINE_EXPR_ACCEPT(BuiltinCallExpr)
DEFINE_EXPR_ACCEPT(CallExpr)
DEFINE_EXPR_ACCEPT(NewExpr)
DEFINE_EXPR_ACCEPT(MeExpr)
DEFINE_EXPR_ACCEPT(MemberAccessExpr)
DEFINE_EXPR_ACCEPT(MethodCallExpr)
DEFINE_EXPR_ACCEPT(IsExpr)
DEFINE_EXPR_ACCEPT(AsExpr)
DEFINE_EXPR_ACCEPT(AddressOfExpr)

// ============================================================================
// Statement Node Accept Implementations
// ============================================================================

DEFINE_STMT_ACCEPT(LabelStmt)
DEFINE_STMT_ACCEPT(PrintStmt)
DEFINE_STMT_ACCEPT(PrintChStmt)
DEFINE_STMT_ACCEPT(BeepStmt)
DEFINE_STMT_ACCEPT(CallStmt)
DEFINE_STMT_ACCEPT(ClsStmt)
DEFINE_STMT_ACCEPT(ColorStmt)
DEFINE_STMT_ACCEPT(SleepStmt)
DEFINE_STMT_ACCEPT(LocateStmt)
DEFINE_STMT_ACCEPT(CursorStmt)
DEFINE_STMT_ACCEPT(AltScreenStmt)
DEFINE_STMT_ACCEPT(LetStmt)
DEFINE_STMT_ACCEPT(DimStmt)
DEFINE_STMT_ACCEPT(ConstStmt)
DEFINE_STMT_ACCEPT(StaticStmt)
// SharedStmt accept methods are defined inline in the header.
DEFINE_STMT_ACCEPT(ReDimStmt)
DEFINE_STMT_ACCEPT(SwapStmt)
DEFINE_STMT_ACCEPT(RandomizeStmt)
DEFINE_STMT_ACCEPT(IfStmt)
DEFINE_STMT_ACCEPT(SelectCaseStmt)
DEFINE_STMT_ACCEPT(TryCatchStmt)
DEFINE_STMT_ACCEPT(WhileStmt)
DEFINE_STMT_ACCEPT(DoStmt)
DEFINE_STMT_ACCEPT(ForStmt)
DEFINE_STMT_ACCEPT(ForEachStmt)
DEFINE_STMT_ACCEPT(NextStmt)
DEFINE_STMT_ACCEPT(ExitStmt)
DEFINE_STMT_ACCEPT(GotoStmt)
DEFINE_STMT_ACCEPT(GosubStmt)
DEFINE_STMT_ACCEPT(OpenStmt)
DEFINE_STMT_ACCEPT(CloseStmt)
DEFINE_STMT_ACCEPT(SeekStmt)
DEFINE_STMT_ACCEPT(OnErrorGoto)
DEFINE_STMT_ACCEPT(Resume)
DEFINE_STMT_ACCEPT(EndStmt)
DEFINE_STMT_ACCEPT(InputStmt)
DEFINE_STMT_ACCEPT(InputChStmt)
DEFINE_STMT_ACCEPT(LineInputChStmt)
DEFINE_STMT_ACCEPT(ReturnStmt)
DEFINE_STMT_ACCEPT(FunctionDecl)
DEFINE_STMT_ACCEPT(SubDecl)
DEFINE_STMT_ACCEPT(StmtList)
DEFINE_STMT_ACCEPT(DeleteStmt)
DEFINE_STMT_ACCEPT(ConstructorDecl)
DEFINE_STMT_ACCEPT(DestructorDecl)
DEFINE_STMT_ACCEPT(MethodDecl)
DEFINE_STMT_ACCEPT(ClassDecl)
DEFINE_STMT_ACCEPT(TypeDecl)
DEFINE_STMT_ACCEPT(InterfaceDecl)
DEFINE_STMT_ACCEPT(PropertyDecl)

// Clean up macros to avoid polluting the global namespace
#undef DEFINE_EXPR_ACCEPT
#undef DEFINE_STMT_ACCEPT

} // namespace il::frontends::basic
