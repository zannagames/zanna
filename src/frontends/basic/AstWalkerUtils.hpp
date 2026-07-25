//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/AstWalkerUtils.hpp
// Purpose: Provides helper utilities shared by BASIC AST walker implementations.
// Key invariants: Utilities preserve traversal semantics defined by BasicAstWalker.
// Ownership/Lifetime: Helpers operate on borrowed AST nodes without taking ownership.
// Links: src/frontends/basic/AstWalker.hpp,
//        src/frontends/basic/AstWalkerUtils.cpp,
//        docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file AstWalkerUtils.hpp
 * @brief Defines child-ordering and hook helpers for BasicAstWalker.
 *
 * These templates centralize null handling, CRTP recovery, parent/child hook
 * bracketing, ordered range traversal, and PRINT-item filtering.
 */

#pragma once

#include "frontends/basic/ast/DeclNodes.hpp"
#include "frontends/basic/ast/StmtExpr.hpp"

namespace il::frontends::basic {
template <typename Derived> class BasicAstWalker;
}

namespace il::frontends::basic::walker {
/// @brief Determine whether a PRINT item carries an expression to visit.
/// @param item PRINT item whose discriminator and payload pointer are checked.
/// @return True only for an expression-kind item with a non-null expression.
[[nodiscard]] bool printItemHasExpr(const PrintItem &item) noexcept;

namespace detail {
/// @brief Recover the concrete CRTP walker from its BasicAstWalker base.
/// @details The traversal helpers operate on the base reference but must
///          dispatch @c accept through the most-derived type so the derived
///          visitor overrides run. This is the single point performing that
///          static downcast.
/// @tparam Derived Concrete walker subclass.
/// @param walker Walker viewed through its CRTP base.
/// @return Reference to the concrete @p Derived walker.
/// @pre @p walker is the BasicAstWalker base subobject of a Derived instance.
template <typename Derived>
[[nodiscard]] inline Derived &asDerived(BasicAstWalker<Derived> &walker) noexcept {
    return *static_cast<Derived *>(&walker);
}

/// @brief Fire the parent/child observation hooks for @p child *without*
///        recursing into it.
/// @details Used for child nodes the walker wants the derived class to observe
///          (e.g. parameter declarations) but that carry no nested AST to
///          traverse. Contrast with @ref visitOptionalChild, which additionally
///          recurses via @c accept. The before/after hooks always bracket the
///          (here empty) child processing.
/// @param walker Active walker.
/// @param parent Node currently being visited.
/// @param child Child being announced to the walker.
/// @note This helper does not consult shouldVisitChildren().
template <typename Derived, typename Parent, typename Child>
inline void notifyChild(BasicAstWalker<Derived> &walker, const Parent &parent, const Child &child) {
    walker.callBeforeChild(parent, child);
    walker.callAfterChild(parent, child);
}

/// @brief Apply @ref notifyChild to every element of @p range in order.
/// @param walker Active walker.
/// @param parent Node owning the range.
/// @param range Iterable of child nodes to announce (not recursed into).
/// @note Range elements are values or references, not optional pointers.
template <typename Derived, typename Parent, typename Range>
inline void notifyChildRange(BasicAstWalker<Derived> &walker,
                             const Parent &parent,
                             const Range &range) {
    for (const auto &child : range) {
        notifyChild(walker, parent, child);
    }
}

/// @brief Recurse into an optional child pointer, bracketed by child hooks.
/// @details No-ops on a null pointer so callers can pass optional fields
///          unconditionally. When present, the before-child hook fires, the
///          child is traversed through the derived walker's @c accept, then the
///          after-child hook fires — preserving the legacy visitor ordering.
/// @param walker Active walker.
/// @param parent Node owning @p childPtr.
/// @param childPtr Smart/raw pointer to an optional child (may be null).
/// @note This helper assumes the caller has already honored the parent node's
///       shouldVisitChildren() decision.
template <typename Derived, typename Parent, typename Ptr>
inline void visitOptionalChild(BasicAstWalker<Derived> &walker,
                               const Parent &parent,
                               const Ptr &childPtr) {
    if (!childPtr)
        return;
    walker.callBeforeChild(parent, *childPtr);
    childPtr->accept(asDerived(walker));
    walker.callAfterChild(parent, *childPtr);
}

/// @brief Recurse into every non-null child of @p range in order.
/// @details Null entries are skipped so sparse child vectors traverse cleanly;
///          each surviving child is handled by @ref visitOptionalChild.
/// @param walker Active walker.
/// @param parent Node owning the range.
/// @param range Iterable of optional child pointers.
/// @note Traversal order is exactly the range's iteration order.
template <typename Derived, typename Parent, typename Range>
inline void visitChildRange(BasicAstWalker<Derived> &walker,
                            const Parent &parent,
                            const Range &range) {
    for (const auto &child : range) {
        if (!child)
            continue;
        visitOptionalChild(walker, parent, child);
    }
}

/// @brief Traverse the expression carried by a single PRINT item, if any.
/// @details PRINT items may be pure separators (`;`/`,`) with no expression;
///          @ref printItemHasExpr filters those out before recursion.
/// @param walker Active walker.
/// @param stmt PRINT statement owning @p item (used as the parent context).
/// @param item PRINT item potentially holding an expression.
/// @note Separator items and malformed expression items with null payloads are skipped.
template <typename Derived>
inline void visitPrintItem(BasicAstWalker<Derived> &walker,
                           const PrintStmt &stmt,
                           const PrintItem &item) {
    if (!printItemHasExpr(item))
        return;
    visitOptionalChild(walker, stmt, item.expr);
}

/// @brief Traverse every expression-bearing item of a PRINT statement in order.
/// @param walker Active walker.
/// @param stmt PRINT statement whose item list is traversed.
template <typename Derived>
inline void visitPrintItems(BasicAstWalker<Derived> &walker, const PrintStmt &stmt) {
    for (const auto &item : stmt.items) {
        visitPrintItem(walker, stmt, item);
    }
}
} // namespace detail

} // namespace il::frontends::basic::walker
