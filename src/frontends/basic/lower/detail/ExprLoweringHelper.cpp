//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/lower/detail/ExprLoweringHelper.cpp
// Purpose: Implements the internal expression-lowering forwarding facade.
// Key invariants: Operand r-values passed by value are consumed exactly once
//                 by their selected domain lowerer.
// Ownership/Lifetime: Borrows Lowerer state; AST nodes remain caller-owned.
// Links: src/frontends/basic/lower/detail/LowererDetail.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements internal forwarding for BASIC expression lowering.

#include "frontends/basic/LowerExprBuiltin.hpp"
#include "frontends/basic/LowerExprLogical.hpp"
#include "frontends/basic/LowerExprNumeric.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/detail/LowererDetail.hpp"

namespace il::frontends::basic::lower::detail {

/// @brief Creates an expression helper over an authorized Lowerer facade.
/// @param access Borrowed forwarding facade retained by value.
ExprLoweringHelper::ExprLoweringHelper(Lowerer::DetailAccess access) noexcept : access_(access) {}

/// @copydoc ExprLoweringHelper::lowerVarExpr()
RVal ExprLoweringHelper::lowerVarExpr(const VarExpr &expr) {
    return access_.lowerVarExpr(expr);
}

/// @copydoc ExprLoweringHelper::lowerUnaryExpr()
RVal ExprLoweringHelper::lowerUnaryExpr(const UnaryExpr &expr) {
    return access_.lowerUnaryExpr(expr);
}

/// @copydoc ExprLoweringHelper::lowerBinaryExpr()
RVal ExprLoweringHelper::lowerBinaryExpr(const BinaryExpr &expr) {
    return access_.lowerBinaryExpr(expr);
}

/// @copydoc ExprLoweringHelper::lowerBuiltinCall()
RVal ExprLoweringHelper::lowerBuiltinCall(const BuiltinCallExpr &expr) {
    return ::il::frontends::basic::lowerBuiltinCall(access_.lowerer(), expr);
}

/// @copydoc ExprLoweringHelper::lowerUBoundExpr()
RVal ExprLoweringHelper::lowerUBoundExpr(const UBoundExpr &expr) {
    return access_.lowerUBoundExpr(expr);
}

/// @copydoc ExprLoweringHelper::lowerLogicalBinary()
RVal ExprLoweringHelper::lowerLogicalBinary(const BinaryExpr &expr) {
    return ::il::frontends::basic::lowerLogicalBinary(access_.lowerer(), expr);
}

/// @copydoc ExprLoweringHelper::lowerDivOrMod()
RVal ExprLoweringHelper::lowerDivOrMod(const BinaryExpr &expr) {
    return ::il::frontends::basic::lowerDivOrMod(access_.lowerer(), expr);
}

/// @copydoc ExprLoweringHelper::lowerStringBinary()
RVal ExprLoweringHelper::lowerStringBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    return ::il::frontends::basic::lowerStringBinary(
        access_.lowerer(), expr, std::move(lhs), std::move(rhs));
}

/// @copydoc ExprLoweringHelper::lowerNumericBinary()
RVal ExprLoweringHelper::lowerNumericBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    return ::il::frontends::basic::lowerNumericBinary(
        access_.lowerer(), expr, std::move(lhs), std::move(rhs));
}

/// @copydoc ExprLoweringHelper::lowerPowBinary()
RVal ExprLoweringHelper::lowerPowBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    return ::il::frontends::basic::lowerPowBinary(
        access_.lowerer(), expr, std::move(lhs), std::move(rhs));
}

} // namespace il::frontends::basic::lower::detail
