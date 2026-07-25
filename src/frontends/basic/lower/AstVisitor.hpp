//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/AstVisitor.hpp
// Purpose: Declares a lightweight visitor interface shared by BASIC lowering
//          components to decouple node traversal from lowering state.
// Key invariants: Each lowering visitor processes one AST family per call and
//                 never mutates ownership of AST nodes.
// Ownership/Lifetime: Visitors borrow AST nodes and Lowerer context; no node
//                     ownership is transferred.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"

/// @file
/// @brief Declares the minimal AST visitor interface used by lowering helpers.

namespace il::frontends::basic::lower {

namespace AST = ::il::frontends::basic;

/// @brief Shared visitor interface for lowering helpers.
/// @details Implementations forward to AST-specific visitors while keeping
///          the Lowerer orchestration decoupled from concrete traversal logic.
struct AstVisitor {
    /// @brief Destroys a lowering visitor through the abstract interface.
    virtual ~AstVisitor() = default;

    /// @brief Visit an expression node and translate it through the bound
    ///        lowering helper.
    /// The expression is borrowed for the duration of the call.
    virtual void visitExpr(const AST::Expr &) = 0;

    /// @brief Visit a statement node and translate it through the bound
    ///        lowering helper.
    /// The statement is borrowed for the duration of the call.
    virtual void visitStmt(const AST::Stmt &) = 0;
};

} // namespace il::frontends::basic::lower
