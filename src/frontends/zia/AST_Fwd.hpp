//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/AST_Fwd.hpp
// Purpose: Break AST header dependency cycles with shared node declarations,
//          owning pointer aliases, and the common source-location alias.
// Key invariants:
//   * AST child pointers use unique ownership.
//   * Forward declarations remain sufficient for cross-category pointer fields.
// Ownership: Pointer aliases transfer ownership; SourceLoc is a value alias.
// References: src/frontends/zia/AST.hpp, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Forward declarations and shared aliases for Zia AST nodes.
///
/// @details This header provides forward declarations for the four core AST
/// node types (Expr, Stmt, TypeNode, Decl) and their corresponding
/// unique_ptr aliases (ExprPtr, StmtPtr, TypePtr, DeclPtr). These
/// forward declarations break circular dependencies between the AST
/// node headers — for example, an expression can contain a block
/// statement, and a statement can contain expressions. By declaring
/// the types here without including their full definitions, each
/// AST_*.hpp header can reference the others through pointers without
/// creating include cycles.
///
/// Also provides a SourceLoc alias imported from the support library,
/// used by all AST nodes for source location tracking.
///
/// @invariant All pointer aliases use std::unique_ptr for single-ownership
///            semantics. AST nodes form a tree, not a graph.
///
/// Ownership/Lifetime: AST nodes are owned by their parent node via
/// unique_ptr. The root ModuleDecl owns the entire tree and is itself
/// owned by the compilation pipeline.
///
/// @see AST.hpp — umbrella header that includes all AST node headers.
/// @see AST_Expr.hpp, AST_Stmt.hpp, AST_Types.hpp, AST_Decl.hpp —
///      full definitions of the forward-declared types.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "support/diagnostics.hpp"
#include <memory>

namespace il::frontends::zia {
//===----------------------------------------------------------------------===//
/// @name Forward Declarations
/// @brief Forward declarations for AST node types and smart pointer aliases.
/// @details These enable circular references between node types (e.g., an
/// expression containing a block that contains statements).
/// @{
//===----------------------------------------------------------------------===//

struct Expr;
struct Stmt;
struct TypeNode;
struct Decl;

/// @brief Unique pointer to an expression node.
/// @details Expressions compute values and can be nested arbitrarily deep.
using ExprPtr = std::unique_ptr<Expr>;

/// @brief Unique pointer to a statement node.
/// @details Statements perform actions and may contain expressions.
using StmtPtr = std::unique_ptr<Stmt>;

/// @brief Unique pointer to a type annotation node.
/// @details Type nodes appear in variable declarations, function signatures,
/// and type casts. They represent syntactic type annotations, not resolved
/// semantic types (see Types.hpp for semantic types).
using TypePtr = std::unique_ptr<TypeNode>;

/// @brief Unique pointer to a declaration node.
/// @details Declarations introduce named entities: types, functions, fields.
using DeclPtr = std::unique_ptr<Decl>;

/// @}

//===----------------------------------------------------------------------===//
/// @name Source Location
/// @brief Type alias for source location tracking.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Source location for error messages and debugging.
/// @details Each AST node stores its source location to enable accurate
/// error reporting and source mapping during lowering.
using SourceLoc = il::support::SourceLoc;

/// @}

} // namespace il::frontends::zia
