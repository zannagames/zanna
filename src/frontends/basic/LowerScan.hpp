//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LowerScan.hpp
// Purpose: Declares AST scanning helpers for BASIC lowering.
// Key invariants: Scanning only mutates bookkeeping flags; no IR is emitted.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerScan.hpp
 * @brief Provides the Lowerer class-body fragment for pre-emission scanning.
 *
 * This file is textually included inside Lowerer. It declares the coarse
 * expression categories and private adapters used to discover runtime needs
 * without generating instructions.
 */

#pragma once

public:
/// @brief Coarse result categories produced by the expression scan.
enum class ExprType {
    /// Integer-width scalar represented by the scan as `i64`.
    I64,
    /// Floating-point scalar represented as `f64`.
    F64,
    /// Runtime-managed BASIC string.
    Str,
    /// Boolean expression category.
    Bool,
    /// Object reference.
    Obj,
};

private:
/// @brief Infer the coarse result category of one expression tree.
/// @param e Expression root to inspect.
/// @return Scan category used by subsequent lowering decisions.
ExprType scanExpr(const Expr &e);
/// @brief Infer the coarse result category of a builtin call.
/// @param c Builtin call to inspect.
/// @return Category prescribed by builtin scan rules.
ExprType scanBuiltinCallExpr(const BuiltinCallExpr &c);

private:
/// @brief Record runtime requirements reachable from one statement.
/// @param s Statement subtree to inspect without emitting IL.
void scanStmt(const Stmt &s);
/// @brief Analyze @p prog for runtime usage prior to emission.
/// @details Traverses procedure and main-body statements through the shared
///          runtime-needs scanner.
/// @param prog Complete BASIC program to inspect.
void scanProgram(const Program &prog);
