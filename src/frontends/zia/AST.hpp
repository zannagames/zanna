//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/AST.hpp
// Purpose: Provide the umbrella include for the complete Zia syntax tree model.
// Key invariants:
//   * Concrete node definitions remain split by declaration, expression,
//     statement, and type categories.
//   * Shared forward declarations and owning pointer aliases come from AST_Fwd.
// Ownership: This aggregate declares no state; AST nodes own child nodes through
//            the policies documented in the category headers.
// References: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Abstract Syntax Tree types for the Zia programming language.
///
/// @details This file defines the complete Abstract Syntax Tree (AST) node
/// hierarchy for Zia, a modern object-oriented language with value and
/// reference semantics. The AST is produced by the Parser and consumed by
/// the semantic analyzer (Sema) and IL lowerer (Lowerer).
///
/// ## Design Overview
///
/// The AST is organized into four main categories:
///
/// **1. Type Nodes (TypeNode hierarchy)**
/// Represent type annotations in the source code, such as:
/// - Named types: `Integer`, `String`, `MyClass`
/// - Generic types: `List[T]`, `Map[K, V]`
/// - Optional types: `T?`
/// - Function types: `(A, B) -> C`
/// - Tuple types: `(A, B)`
///
/// **2. Expression Nodes (Expr hierarchy)**
/// Represent expressions that compute values:
/// - Literals: integers, floats, strings, booleans, null
/// - Operations: binary, unary, ternary, range
/// - Access: identifiers, field access, indexing
/// - Calls: function/method invocation, constructor calls
/// - Control flow expressions: if-else, match, block expressions
///
/// **3. Statement Nodes (Stmt hierarchy)**
/// Represent statements that perform actions:
/// - Control flow: if, while, for, for-in, guard, match
/// - Declarations: var, final
/// - Jumps: return, break, continue
/// - Expression statements
///
/// **4. Declaration Nodes (Decl hierarchy)**
/// Represent top-level and type member declarations:
/// - Types: struct, class, interface
/// - Functions: global functions, methods, constructors
/// - Members: fields
/// - Modules: module declaration, imports
///
/// ## Ownership Model
///
/// All AST nodes own their children via `std::unique_ptr`. When a node is
/// destroyed, all its children are automatically cleaned up. The parser
/// owns the root ModuleDecl, which transitively owns the entire tree.
///
/// ## Memory Layout
///
/// Each node contains:
/// - A `kind` field identifying the specific node type (for safe downcasting)
/// - A `loc` field with source location information for error messages
/// - Type-specific data fields
///
/// ## Type Aliases
///
/// For convenience, smart pointer aliases are provided:
/// - `ExprPtr` = `std::unique_ptr<Expr>`
/// - `StmtPtr` = `std::unique_ptr<Stmt>`
/// - `TypePtr` = `std::unique_ptr<TypeNode>`
/// - `DeclPtr` = `std::unique_ptr<Decl>`
///
/// ## Usage Example
///
/// ```cpp
/// // Creating an integer literal expression
/// auto intExpr = std::make_unique<IntLiteralExpr>(loc, 42);
///
/// // Creating a binary addition expression
/// auto addExpr = std::make_unique<BinaryExpr>(
///     loc, BinaryOp::Add,
///     std::move(leftExpr),
///     std::move(rightExpr)
/// );
///
/// // Downcasting based on kind
/// if (expr->kind == ExprKind::Binary) {
///     auto *binary = static_cast<BinaryExpr*>(expr);
///     // ... process binary expression
/// }
/// ```
///
/// @invariant All AST nodes own their children via unique_ptr.
/// @invariant Every node has a valid source location for error reporting.
/// @invariant Node kind field matches the actual derived type.
///
/// @see Parser.hpp - Creates AST from tokens
/// @see Sema.hpp - Performs semantic analysis on AST
/// @see Lowerer.hpp - Converts AST to intermediate language
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST_Decl.hpp"
#include "frontends/zia/AST_Expr.hpp"
#include "frontends/zia/AST_Fwd.hpp"
#include "frontends/zia/AST_Stmt.hpp"
#include "frontends/zia/AST_Types.hpp"
