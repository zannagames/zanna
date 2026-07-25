//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares constant folding utilities for BASIC expressions,
// performing compile-time evaluation of constant expressions to improve
// generated IL quality.
//
// Constant Folding:
// Constant folding is an optimization pass that evaluates expressions with
// literal operands at compile time, replacing them with their computed results:
//   3 + 5      →  8
//   10 * 2 + 1 →  21
//   "Hello" + " " + "World" → "Hello World"
//
// This optimization:
// - Reduces IL instruction count
// - Enables better dead code elimination
// - Simplifies expressions for easier semantic analysis
// - Improves runtime performance by eliminating redundant computations
//
// Folding Rules:
// Only pure expressions with constant operands are folded:
// - Binary arithmetic: +, -, *, /, \, MOD, ^
// - Unary arithmetic: -, NOT
// - Comparison: =, <, >, <=, >=, <>
// - Logical: AND, OR, NOT
// - String operations supported by the constfold dispatch tables
//
// Expressions involving:
// - Variables
// - Function calls (except certain pure intrinsics)
// - I/O operations
// are NOT folded since they may have side effects or depend on runtime state.
//
// AST Transformation:
// The folder mutates the AST in place, replacing:
//   BinaryExpr(IntLiteral(3), +, IntLiteral(5))
// with:
//   IntLiteral(8)
//
// Ownership:
// - Functions mutate AST in place
// - AST nodes remain owned by caller
// - Replaced nodes are properly deallocated
//
// Integration:
// - Called by: BasicCompiler after parsing and before semantic analysis
// - Operates on: Program procedure entries and main statements, with traversal
//   determined by the concrete visitor handlers
// - Preserves: Source locations on materialized replacement literals
//
// Design Notes:
// - Only folds pure expressions to preserve program semantics
// - Handles all BASIC literal types (integer, float, string, boolean)
// - Propagates type information through folded expressions
// - Safe for use before semantic analysis
//
//===----------------------------------------------------------------------===//

/**
 * @file ConstFolder.hpp
 * @brief Declares in-place literal-expression folding for BASIC AST programs.
 *
 * The pass runs before semantic analysis and rewrites only patterns recognized
 * by its expression, statement, and pure-builtin dispatch handlers.
 */

#pragma once

#include "frontends/basic/Token.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"

namespace il::frontends::basic {

/// \brief Fold constant expressions within a BASIC program AST.
/// \details Visits entries in `prog.procs` followed by `prog.main`. Concrete
///          statement visitors decide which nested expression slots and bodies
///          are traversed; unsupported or nonliteral expressions remain intact.
/// \param prog Program to transform in place.
/// \post Successfully folded subtrees are replaced by owning literal nodes
///       carrying the original outer expression's source location.
void foldConstants(Program &prog);

} // namespace il::frontends::basic
