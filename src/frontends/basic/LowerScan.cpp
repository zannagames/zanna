//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/frontends/basic/LowerScan.cpp
// Purpose: Thin orchestrator delegating BASIC scan passes to specialized helpers.
// Key invariants: Scan sets flags only; no IR emission.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerScan.cpp
 * @brief Implements Lowerer adapters for pre-emission AST scanning.
 *
 * The substantive visitors live under the `lower` namespace; this unit exposes
 * them through Lowerer without emitting IL itself.
 */

#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {
namespace lower {
/// @brief Infer the coarse result category of one expression tree.
/// @param lowerer Lowerer whose symbol and runtime state supports the scan.
/// @param expr Expression root to classify.
/// @return Coarse expression result category.
Lowerer::ExprType scanExprTypes(Lowerer &lowerer, const Expr &expr);
/// @brief Infer the result category of one builtin invocation.
/// @param lowerer Lowerer whose builtin metadata supports the scan.
/// @param expr Builtin invocation to classify.
/// @return Coarse builtin result category.
Lowerer::ExprType scanBuiltinExprTypes(Lowerer &lowerer, const BuiltinCallExpr &expr);
/// @brief Record runtime requirements reachable from one statement.
/// @param lowerer Lowerer receiving requirement flags.
/// @param stmt Statement root to scan.
void scanStmtRuntimeNeeds(Lowerer &lowerer, const Stmt &stmt);
/// @brief Record runtime requirements reachable from an entire program.
/// @param lowerer Lowerer receiving requirement flags.
/// @param prog Procedure and main-body roots to scan.
void scanProgramRuntimeNeeds(Lowerer &lowerer, const Program &prog);
} // namespace lower

/// @brief Determine the IL type produced by a BASIC expression.
///
/// @details The lowering pipeline performs a lightweight pre-pass that infers
///          result types without emitting instructions.  This shim delegates to
///          the shared `lower::scanExprTypes` helper so other translation units
///          can reuse the same logic without introducing additional headers.
///
/// @param expr BASIC expression to analyse.
/// @return Inferred IL type classification for the expression.
Lowerer::ExprType Lowerer::scanExpr(const Expr &expr) {
    return lower::scanExprTypes(*this, expr);
}

/// @brief Analyse the return type of a BASIC intrinsic call.
///
/// @details Built-in procedures funnel through a dedicated visitor that
///          understands intrinsic signatures.  The wrapper keeps the public
///          `Lowerer` API minimal while ensuring the `lower::` namespace owns the
///          implementation.
///
/// @param expr Builtin call expression subject to type classification.
/// @return Resulting IL type emitted by the intrinsic.
Lowerer::ExprType Lowerer::scanBuiltinCallExpr(const BuiltinCallExpr &expr) {
    return lower::scanBuiltinExprTypes(*this, expr);
}

/// @brief Inspect a statement to record runtime service requirements.
///
/// @details During the scanning phase the lowering context marks which VM
///          services (file I/O, runtime library helpers, etc.) must be linked
///          for the final program.  The wrapper simply forwards to the shared
///          `lower::scanStmtRuntimeNeeds` implementation.
///
/// @param stmt Statement to analyse for runtime dependencies.
void Lowerer::scanStmt(const Stmt &stmt) {
    lower::scanStmtRuntimeNeeds(*this, stmt);
}

/// @brief Perform a scan pass across the entire BASIC program.
///
/// @details Orchestrates the scanning helpers so that expression types and
///          runtime requirements are discovered before emission begins.  The
///          work is delegated to `lower::scanProgramRuntimeNeeds`, keeping the
///          translation unit focused on API wiring.
///
/// @param prog Parsed BASIC program.
void Lowerer::scanProgram(const Program &prog) {
    lower::scanProgramRuntimeNeeds(*this, prog);
}

} // namespace il::frontends::basic
