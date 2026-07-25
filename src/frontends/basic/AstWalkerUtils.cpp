//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/frontends/basic/AstWalkerUtils.cpp
// Purpose: Implements helper utilities shared by BASIC AST walker implementations.
// Key invariants: Utilities mirror BasicAstWalker traversal semantics and never
//                 mutate AST nodes.
// Ownership/Lifetime: Functions operate on borrowed nodes.
// Links: src/frontends/basic/AstWalkerUtils.hpp,
//        src/frontends/basic/AstWalker.hpp,
//        docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Convenience predicates for BASIC AST walker implementations.
/// @details The helpers here encode common queries used by the walkers and
///          printers in the BASIC front-end.  Housing the logic out of line
///          keeps headers minimal while providing a single location that
///          documents traversal assumptions shared across the front-end.

#include "frontends/basic/AstWalkerUtils.hpp"

namespace il::frontends::basic::walker {

/// @brief Check whether a PRINT item carries an evaluated expression.
///
/// @details BASIC PRINT statements may interleave literal separators and
///          expressions.  The AST walker needs to know when an item provides a
///          computed value so it can request lowering or formatting.  The
///          helper simply inspects the discriminant and ensures the expression
///          pointer is populated for safety; returning @c false for malformed
///          items allows callers to degrade gracefully without dereferencing
///          null nodes.
///
/// @param item PRINT item under inspection.
/// @return True when the item represents an expression payload.
bool printItemHasExpr(const PrintItem &item) noexcept {
    return item.kind == PrintItem::Kind::Expr && item.expr != nullptr;
}

} // namespace il::frontends::basic::walker
