//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/frontends/basic/LowerExprLogical.cpp
// Purpose: Implements logical expression lowering helpers for the BASIC Lowerer.
// Key invariants: Logical operators preserve BASIC truthiness semantics using
//                 Lowerer utilities for short-circuiting.
// Ownership/Lifetime: Helpers borrow the Lowerer for the duration of the call.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerExprLogical.cpp
 * @brief Implements eager and short-circuit BASIC logical operators.
 *
 * Short-circuit forms build a boolean diamond and canonicalize its result to
 * BASIC's `i64` -1/0 convention; eager AND and OR perform bitwise operations on
 * fully evaluated logical words.
 */

#include "frontends/basic/LowerExprLogical.hpp"

#include "frontends/basic/DiagnosticEmitter.hpp"

namespace il::frontends::basic {
using namespace il::core;

using IlType = il::core::Type;
using IlKind = IlType::Kind;

namespace {
/// @brief Provide a user-facing display name for logical operators.
///
/// @details Converts the lowering-time enumeration into the BASIC keyword used
///          in diagnostics.  Unknown operators fall back to a sentinel so that
///          callers can include the numeric value when forming error messages.
///
/// @param op Logical operator enumeration from the AST.
/// @return BASIC keyword string or `"<logical>"` for unrecognised values.
std::string_view logicalOperatorDisplayName(BinaryExpr::Op op) noexcept {
    switch (op) {
        case BinaryExpr::Op::LogicalAndShort:
            return "ANDALSO";
        case BinaryExpr::Op::LogicalOrShort:
            return "ORELSE";
        case BinaryExpr::Op::LogicalAnd:
            return "AND";
        case BinaryExpr::Op::LogicalOr:
            return "OR";
        default:
            break;
    }
    return "<logical>";
}

/// Diagnostic code used when this helper receives a non-logical operator.
constexpr std::string_view kDiagUnsupportedLogicalOperator = "B4002";
} // namespace

/// @brief Construct the logical lowering helper around a @ref Lowerer.
///
/// @param lowerer Borrowed lowering context; it must outlive this helper.
LogicalExprLowering::LogicalExprLowering(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

/// @brief Lower a BASIC logical binary expression into IL.
///
/// @details Handles both short-circuit (`ANDALSO`, `ORELSE`) and eager
///          (`AND`, `OR`) operators.  Short-circuit operators delegate to
///          @ref Lowerer::lowerBoolBranchExpr to build explicit control flow
///          while eager ones coerce operands to logical words and emit bitwise
///          operations.  Unsupported operators emit diagnostics and return
///          `FALSE` to keep compilation progressing.
///
/// @param expr Logical binary expression AST node.
/// @return Resulting IL value paired with its logical word type.
Lowerer::RVal LogicalExprLowering::lower(const BinaryExpr &expr) {
    Lowerer &lowerer = *lowerer_;
    Lowerer::RVal lhs = lowerer.lowerExpr(*expr.lhs);
    lowerer.curLoc = expr.loc;

    /// Coerce one lowered operand to the `i1` condition type at this expression.
    auto toBool = [&](Lowerer::RVal val) {
        return lowerer.coerceToBool(std::move(val), expr.loc).value;
    };

    if (expr.op == BinaryExpr::Op::LogicalAndShort) {
        Value cond = toBool(lhs);
        Lowerer::RVal andResult = lowerer.lowerBoolBranchExpr(
            cond,
            expr.loc,
            [&](Value slot) {
                Lowerer::RVal rhs = lowerer.lowerExpr(*expr.rhs);
                Value rhsBool = toBool(std::move(rhs));
                lowerer.curLoc = expr.loc;
                lowerer.emitStore(lowerer.ilBoolTy(), slot, rhsBool);
            },
            [&](Value slot) {
                lowerer.curLoc = expr.loc;
                lowerer.emitStore(lowerer.ilBoolTy(), slot, lowerer.emitBoolConst(false));
            },
            "and_rhs",
            "and_false",
            "and_done");

        lowerer.curLoc = expr.loc;
        Value logical = lowerer.emitBasicLogicalI64(andResult.value);
        return {logical, IlType(IlKind::I64)};
    }

    if (expr.op == BinaryExpr::Op::LogicalOrShort) {
        Value cond = toBool(lhs);
        Lowerer::RVal orResult = lowerer.lowerBoolBranchExpr(
            cond,
            expr.loc,
            [&](Value slot) {
                lowerer.curLoc = expr.loc;
                lowerer.emitStore(lowerer.ilBoolTy(), slot, lowerer.emitBoolConst(true));
            },
            [&](Value slot) {
                Lowerer::RVal rhs = lowerer.lowerExpr(*expr.rhs);
                Value rhsBool = toBool(std::move(rhs));
                lowerer.curLoc = expr.loc;
                lowerer.emitStore(lowerer.ilBoolTy(), slot, rhsBool);
            },
            "or_true",
            "or_rhs",
            "or_done");

        lowerer.curLoc = expr.loc;
        Value logical = lowerer.emitBasicLogicalI64(orResult.value);
        return {logical, IlType(IlKind::I64)};
    }

    if (expr.op == BinaryExpr::Op::LogicalAnd) {
        // Eager BASIC AND operates on logical-word i64 values. Boolean inputs
        // are coerced to -1/0 and integer inputs are preserved as-is.
        if (lhs.type.kind != IlKind::I64)
            lhs = {lowerer.coerceToI64(std::move(lhs), expr.loc).value, IlType(IlKind::I64)};
        Lowerer::RVal rhs = lowerer.lowerExpr(*expr.rhs);
        if (rhs.type.kind != IlKind::I64)
            rhs = {lowerer.coerceToI64(std::move(rhs), expr.loc).value, IlType(IlKind::I64)};
        lowerer.curLoc = expr.loc;
        Value res = lowerer.emitCommon(expr.loc).logical_and(lhs.value, rhs.value);
        return {res, IlType(IlKind::I64)};
    }

    if (expr.op == BinaryExpr::Op::LogicalOr) {
        // Eager BASIC OR operates on logical-word i64 values.
        if (lhs.type.kind != IlKind::I64)
            lhs = {lowerer.coerceToI64(std::move(lhs), expr.loc).value, IlType(IlKind::I64)};
        Lowerer::RVal rhs = lowerer.lowerExpr(*expr.rhs);
        if (rhs.type.kind != IlKind::I64)
            rhs = {lowerer.coerceToI64(std::move(rhs), expr.loc).value, IlType(IlKind::I64)};
        lowerer.curLoc = expr.loc;
        Value res = lowerer.emitCommon(expr.loc).logical_or(lhs.value, rhs.value);
        return {res, IlType(IlKind::I64)};
    }

    if (auto *emitter = lowerer.diagnosticEmitter()) {
        std::string_view opText = logicalOperatorDisplayName(expr.op);
        std::string message = "unsupported logical operator '";
        message.append(opText);
        message.push_back('\'');
        if (opText == std::string_view("<logical>")) {
            message.append(" (enum value ");
            message.append(std::to_string(static_cast<int>(expr.op)));
            message.push_back(')');
        }
        message.append("; assuming FALSE");
        emitter->emit(il::support::Severity::Error,
                      std::string(kDiagUnsupportedLogicalOperator),
                      expr.loc,
                      0,
                      std::move(message));
    }

    lowerer.curLoc = expr.loc;
    Value logicalFalse = lowerer.emitBoolConst(false);
    lowerer.curLoc = expr.loc;
    Value logicalWord = lowerer.emitBasicLogicalI64(logicalFalse);
    return {logicalWord, IlType(IlKind::I64)};
}

/// @brief Member façade that forwards logical lowering to the helper module.
///
/// @param expr Logical binary expression to lower.
/// @return Result of @ref lowerLogicalBinary.
Lowerer::RVal Lowerer::lowerLogicalBinary(const BinaryExpr &expr) {
    return ::il::frontends::basic::lowerLogicalBinary(*this, expr);
}

/// @brief Free-function entry point for callers with an explicit lowerer.
///
/// @param lowerer Active lowering context receiving the emitted IL.
/// @param expr Logical binary expression under translation.
/// @return Lowered IL value representing the BASIC logical result.
Lowerer::RVal lowerLogicalBinary(Lowerer &lowerer, const BinaryExpr &expr) {
    LogicalExprLowering lowering(lowerer);
    return lowering.lower(expr);
}

} // namespace il::frontends::basic
