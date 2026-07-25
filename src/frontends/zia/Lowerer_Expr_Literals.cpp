//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Literals.cpp
/// @brief Literal expression lowering for the Zia IL lowerer.
///
/// @details Scalar literals become immediate IL constants, while string
///          literals materialize owned handles from interned module globals.
///          Range expressions are eagerly materialized as boxed-integer lists
///          with optional reverse and positive-step modifiers, overflow-safe
///          termination, and explicit scratch-slot lifetime management.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include <limits>

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Literal Expression Lowering
//=============================================================================

/// @brief Lower an integer literal to a constant I64 value.
/// @param expr Integer literal carrying the parsed signed value.
/// @return Constant `i64` result.
LowerResult Lowerer::lowerIntLiteral(IntLiteralExpr *expr) {
    return {Value::constInt(expr->value), Type(Type::Kind::I64)};
}

/// @brief Lower a floating-point literal to a constant F64 value.
/// @param expr Number literal carrying the parsed double value.
/// @return Constant `f64` result.
LowerResult Lowerer::lowerNumberLiteral(NumberLiteralExpr *expr) {
    return {Value::constFloat(expr->value), Type(Type::Kind::F64)};
}

/// @brief Lower a string literal to a constant referencing an interned string global.
/// @param expr String literal carrying decoded contents.
/// @return Owned `str` handle scheduled for deferred release.
LowerResult Lowerer::lowerStringLiteral(StringLiteralExpr *expr) {
    std::string globalName = getStringGlobal(expr->value);
    Value val = emitConstStr(globalName);
    return {val, Type(Type::Kind::Str)};
}

/// @brief Lower a boolean literal to a constant I1 value.
/// @param expr Boolean literal carrying the parsed truth value.
/// @return Constant `i1` result.
LowerResult Lowerer::lowerBoolLiteral(BoolLiteralExpr *expr) {
    return {Value::constBool(expr->value), Type(Type::Kind::I1)};
}

/// @brief Lower a `null` literal to a null pointer value.
/// @return Null pointer result.
LowerResult Lowerer::lowerNullLiteral(NullLiteralExpr * /*expr*/) {
    return {Value::null(), Type(Type::Kind::Ptr)};
}

/// @brief Lower a `unit` literal to the pointer-sized unit singleton (null).
/// @return Null pointer used as the IL Unit representation.
LowerResult Lowerer::lowerUnitLiteral(UnitLiteralExpr * /*expr*/) {
    return {Value::null(), Type(Type::Kind::Ptr)};
}

/// @brief Unwrap a `start..end` range and any trailing `.rev()`/`.step(n)` modifiers.
/// @param expr Candidate expression (a RangeExpr or a chain of modifier calls on one).
/// @param out Receives the base range plus accumulated reversed flag and step argument.
/// @return True if @p expr is a range (optionally modified); false otherwise.
/// @details Recurses through the call chain so `(a..b).rev().step(n)` collapses to a single
///          RangeModifierInfo. Each `.rev()` toggles direction; `.step(n)` records the stride.
bool Lowerer::collectRangeModifierChain(Expr *expr, RangeModifierInfo &out) const {
    if (!expr)
        return false;
    if (expr->kind == ExprKind::Range) {
        out.range = static_cast<RangeExpr *>(expr);
        return true;
    }
    if (expr->kind != ExprKind::Call)
        return false;

    auto *call = static_cast<CallExpr *>(expr);
    if (!call->callee || call->callee->kind != ExprKind::Field)
        return false;

    auto *field = static_cast<FieldExpr *>(call->callee.get());
    if (field->field == "rev" && call->args.empty()) {
        if (!collectRangeModifierChain(field->base.get(), out))
            return false;
        out.reversed = !out.reversed;
        return true;
    }
    if (field->field == "step" && call->args.size() == 1) {
        if (!collectRangeModifierChain(field->base.get(), out))
            return false;
        out.stepArg = call->args[0].value.get();
        return true;
    }
    return false;
}

/// @brief Lower a bare range expression (no `.rev()`/`.step()` modifiers).
/// @param expr Range expression to materialize.
/// @return A pointer to a freshly built list of the range's elements.
LowerResult Lowerer::lowerRange(RangeExpr *expr) {
    return lowerRangeWithModifiers(expr, false, nullptr);
}

/// @brief Materialize a range (with optional reverse/step) into a heap list of I64 elements.
/// @param expr Range expression providing the start/end bounds and inclusivity.
/// @param reversed When true, iterate from the high bound down to the low bound.
/// @param stepArg Optional positive stride expression (defaults to 1; validated at runtime).
/// @return A pointer to the populated list.
/// @details Emits a counted loop over scratch cur/end/step slots. Boundary handling is
///          overflow-safe: the loop exits before stepping past INT64_MIN/MAX, and inclusive
///          ranges include the final bound. Scratch slots are released at the exit block.
LowerResult Lowerer::lowerRangeWithModifiers(RangeExpr *expr, bool reversed, Expr *stepArg) {
    Value list = emitCallRet(Type(Type::Kind::Ptr), kListNew, {});

    auto start = lowerExpr(expr->start.get());
    auto end = lowerExpr(expr->end.get());
    Value startValue = widenIntegralToI64(start.value, start.type);
    Value endValue = widenIntegralToI64(end.value, end.type);

    Value stepValue = Value::constInt(1);
    if (stepArg) {
        auto step = lowerExpr(stepArg);
        stepValue = widenIntegralToI64(step.value, step.type);
        stepValue = emitPositiveStepCheck(stepValue);
    }

    std::string curVar = "__range_cur_" + std::to_string(nextTempId());
    std::string endVar = "__range_end_" + std::to_string(nextTempId());
    std::string stepVar = "__range_step_" + std::to_string(nextTempId());
    createSlot(curVar, Type(Type::Kind::I64));
    createSlot(endVar, Type(Type::Kind::I64));
    createSlot(stepVar, Type(Type::Kind::I64));
    storeToSlot(curVar, reversed ? endValue : startValue, Type(Type::Kind::I64));
    storeToSlot(endVar, reversed ? startValue : endValue, Type(Type::Kind::I64));
    storeToSlot(stepVar, stepValue, Type(Type::Kind::I64));

    size_t condIdx = createBlock("range_cond");
    size_t bodyIdx = createBlock("range_body");
    size_t updateIdx = createBlock("range_update");
    size_t updateDoIdx = createBlock("range_update_do");
    size_t endIdx = createBlock("range_end");

    emitBr(condIdx);

    setBlock(condIdx);
    Value cur = loadFromSlot(curVar, Type(Type::Kind::I64));
    Value bound = loadFromSlot(endVar, Type(Type::Kind::I64));
    Opcode condOp;
    if (reversed)
        condOp = expr->inclusive ? Opcode::SCmpGE : Opcode::SCmpGT;
    else
        condOp = expr->inclusive ? Opcode::SCmpLE : Opcode::SCmpLT;
    Value cond = emitBinary(condOp, Type(Type::Kind::I1), cur, bound);
    emitCBr(cond, bodyIdx, endIdx);

    setBlock(bodyIdx);
    Value elem = loadFromSlot(curVar, Type(Type::Kind::I64));
    if (reversed && !expr->inclusive)
        elem = emitBinary(Opcode::ISubOvf, Type(Type::Kind::I64), elem, Value::constInt(1));
    Value boxed = emitBox(elem, Type(Type::Kind::I64));
    emitCall(kListAdd, {list, boxed});
    emitBr(updateIdx);

    setBlock(updateIdx);
    Value updateCur = loadFromSlot(curVar, Type(Type::Kind::I64));
    Value updateBound = loadFromSlot(endVar, Type(Type::Kind::I64));
    Value updateStep = loadFromSlot(stepVar, Type(Type::Kind::I64));
    Value terminal = expr->inclusive
                         ? emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), updateCur, updateBound)
                         : Value::constBool(false);
    Value overflowRisk;
    if (reversed) {
        Value minPlusStep = emitBinary(Opcode::IAddOvf,
                                       Type(Type::Kind::I64),
                                       Value::constInt(std::numeric_limits<int64_t>::min()),
                                       updateStep);
        overflowRisk = emitBinary(Opcode::SCmpLT, Type(Type::Kind::I1), updateCur, minPlusStep);
    } else {
        Value maxMinusStep = emitBinary(Opcode::ISubOvf,
                                        Type(Type::Kind::I64),
                                        Value::constInt(std::numeric_limits<int64_t>::max()),
                                        updateStep);
        overflowRisk = emitBinary(Opcode::SCmpGT, Type(Type::Kind::I1), updateCur, maxMinusStep);
    }
    Value terminalWide = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), terminal);
    Value overflowWide = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), overflowRisk);
    Value exitWide = emitBinary(Opcode::Or, Type(Type::Kind::I64), terminalWide, overflowWide);
    Value exitNow = emitUnary(Opcode::Trunc1, Type(Type::Kind::I1), exitWide);
    emitCBr(exitNow, endIdx, updateDoIdx);

    setBlock(updateDoIdx);
    Value nextCur = loadFromSlot(curVar, Type(Type::Kind::I64));
    Value nextStep = loadFromSlot(stepVar, Type(Type::Kind::I64));
    Value next = reversed ? emitBinary(Opcode::ISubOvf, Type(Type::Kind::I64), nextCur, nextStep)
                          : emitBinary(Opcode::IAddOvf, Type(Type::Kind::I64), nextCur, nextStep);
    storeToSlot(curVar, next, Type(Type::Kind::I64));
    emitBr(condIdx);

    setBlock(endIdx);
    removeSlot(curVar);
    removeSlot(endVar);
    removeSlot(stepVar);
    return {list, Type(Type::Kind::Ptr)};
}

} // namespace il::frontends::zia
