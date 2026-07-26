//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/lower/detail/OopLoweringHelper.cpp
// Purpose: Implements the internal object-oriented lowering forwarding facade.
// Key invariants: Explicit-context overloads use the caller's OOP state;
//                 convenience overloads use Lowerer's active state.
// Ownership/Lifetime: Borrows Lowerer and OOP context state; owns no AST or IR.
// Links: src/frontends/basic/lower/detail/LowererDetail.hpp,
//        src/frontends/basic/OopLoweringContext.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements internal forwarding for BASIC OOP lowering.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/lower/detail/LowererDetail.hpp"

namespace il::frontends::basic::lower::detail {

/// @brief Creates an OOP helper over an authorized Lowerer facade.
/// @param access Borrowed forwarding facade retained by value.
OopLoweringHelper::OopLoweringHelper(Lowerer::DetailAccess access) noexcept : access_(access) {}

/// @copydoc OopLoweringHelper::lowerNewExpr(const NewExpr &)
RVal OopLoweringHelper::lowerNewExpr(const NewExpr &expr) {
    return access_.lowerNewExpr(expr);
}

/// @copydoc OopLoweringHelper::lowerNewExpr(const NewExpr &, OopLoweringContext &)
RVal OopLoweringHelper::lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx) {
    return access_.lowerNewExpr(expr, ctx);
}

/// @copydoc OopLoweringHelper::lowerMeExpr(const MeExpr &)
RVal OopLoweringHelper::lowerMeExpr(const MeExpr &expr) {
    return access_.lowerMeExpr(expr);
}

/// @copydoc OopLoweringHelper::lowerMeExpr(const MeExpr &, OopLoweringContext &)
RVal OopLoweringHelper::lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx) {
    return access_.lowerMeExpr(expr, ctx);
}

/// @copydoc OopLoweringHelper::lowerMemberAccessExpr(const MemberAccessExpr &)
RVal OopLoweringHelper::lowerMemberAccessExpr(const MemberAccessExpr &expr) {
    return access_.lowerMemberAccessExpr(expr);
}

/// @copydoc OopLoweringHelper::lowerMemberAccessExpr(const MemberAccessExpr &,
///                                                   OopLoweringContext &)
RVal OopLoweringHelper::lowerMemberAccessExpr(const MemberAccessExpr &expr,
                                              OopLoweringContext &ctx) {
    return access_.lowerMemberAccessExpr(expr, ctx);
}

/// @copydoc OopLoweringHelper::lowerMethodCallExpr(const MethodCallExpr &)
RVal OopLoweringHelper::lowerMethodCallExpr(const MethodCallExpr &expr) {
    return access_.lowerMethodCallExpr(expr);
}

/// @copydoc OopLoweringHelper::lowerMethodCallExpr(const MethodCallExpr &,
///                                                 OopLoweringContext &)
RVal OopLoweringHelper::lowerMethodCallExpr(const MethodCallExpr &expr, OopLoweringContext &ctx) {
    return access_.lowerMethodCallExpr(expr, ctx);
}

/// @copydoc OopLoweringHelper::lowerDelete(const DeleteStmt &)
void OopLoweringHelper::lowerDelete(const DeleteStmt &stmt) {
    access_.lowerDelete(stmt);
}

/// @copydoc OopLoweringHelper::lowerDelete(const DeleteStmt &, OopLoweringContext &)
void OopLoweringHelper::lowerDelete(const DeleteStmt &stmt, OopLoweringContext &ctx) {
    access_.lowerDelete(stmt, ctx);
}

/// @copydoc OopLoweringHelper::scanOOP()
void OopLoweringHelper::scanOOP(const Program &prog) {
    access_.scanOOP(prog);
}

/// @copydoc OopLoweringHelper::emitOopDeclsAndBodies()
void OopLoweringHelper::emitOopDeclsAndBodies(const Program &prog) {
    access_.emitOopDeclsAndBodies(prog);
}

} // namespace il::frontends::basic::lower::detail
