//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererBinaryOperatorLowerer.cpp
/// @brief BinaryOperatorLowerer implementation.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererBinaryOperatorLowerer.hpp"

#include "frontends/zia/RuntimeNames.hpp"

namespace il::frontends::zia {

using namespace runtime;

/// @brief Lower a binary expression to IL, selecting the right opcode/runtime helper per type.
/// @param expr Binary expression.
/// @return The result value and its IL type.
/// @details Assignment and short-circuit `and`/`or` are delegated to the Lowerer up front.
///          Otherwise both operands are lowered, integers are promoted to F64 when mixed with
///          a float, and the operator is mapped: arithmetic uses checked integer
///          (`IAddOvf`/`SDivChk0`/...) or floating opcodes; `+` on a string operand concatenates
///          (stringifying the other side); comparisons use float/signed-int opcodes or string
///          runtime helpers (`kStringEquals`, `rt_str_lt`, ...) and yield `i1`; bitwise and
///          shift ops map to their IL opcodes. Non-short-circuit `And`/`Or` here operate
///          bitwise on zero-extended booleans.
LowerResult BinaryOperatorLowerer::lowerBinary(BinaryExpr *expr) {
    if (expr->op == BinaryOp::Assign) {
        return lowerer_.lowerAssignment(expr);
    }

    // Handle short-circuit evaluation before evaluating the right operand.
    if (expr->op == BinaryOp::And || expr->op == BinaryOp::Or) {
        return lowerer_.lowerShortCircuit(expr);
    }

    auto left = lowerer_.lowerExpr(expr->left.get());
    auto right = lowerer_.lowerExpr(expr->right.get());

    TypeRef leftType = lowerer_.sema_.typeOf(expr->left.get());
    TypeRef rightType = lowerer_.sema_.typeOf(expr->right.get());

    bool leftIsFloat = leftType && leftType->kind == TypeKindSem::Number;
    bool rightIsFloat = rightType && rightType->kind == TypeKindSem::Number;
    bool isFloat = leftIsFloat || rightIsFloat;

    if (isFloat && !leftIsFloat && leftType && leftType->isIntegral()) {
        unsigned convId = lowerer_.nextTempId();
        il::core::Instr convInstr;
        convInstr.result = convId;
        convInstr.op = Opcode::Sitofp;
        convInstr.type = Type(Type::Kind::F64);
        convInstr.operands = {left.value};
        convInstr.loc = lowerer_.curLoc_;
        lowerer_.blockMgr_.currentBlock()->instructions.push_back(convInstr);
        left.value = Value::temp(convId);
        left.type = Type(Type::Kind::F64);
    } else if (isFloat && !rightIsFloat && rightType && rightType->isIntegral()) {
        unsigned convId = lowerer_.nextTempId();
        il::core::Instr convInstr;
        convInstr.result = convId;
        convInstr.op = Opcode::Sitofp;
        convInstr.type = Type(Type::Kind::F64);
        convInstr.operands = {right.value};
        convInstr.loc = lowerer_.curLoc_;
        lowerer_.blockMgr_.currentBlock()->instructions.push_back(convInstr);
        right.value = Value::temp(convId);
        right.type = Type(Type::Kind::F64);
    }

    Opcode op = Opcode::IAddOvf;
    Type resultType = isFloat ? Type(Type::Kind::F64) : left.type;

    switch (expr->op) {
        case BinaryOp::Add:
            if (leftType && leftType->kind == TypeKindSem::String) {
                // A string handle (String or String?) concatenates as is; any
                // other operand (Integer, Byte, enum, Number, Boolean) formats
                // the way `toString` does.
                Value rightStr = right.type.kind == Type::Kind::Str
                                     ? right.value
                                     : lowerer_.emitToString(right.value, rightType);

                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::Str), kStringConcat, {left.value, rightStr});
                lowerer_.consumeDeferred(left.value);
                lowerer_.consumeDeferred(rightStr);
                return {result, Type(Type::Kind::Str)};
            }
            if (rightType && rightType->kind == TypeKindSem::String) {
                Value leftStr = left.type.kind == Type::Kind::Str
                                    ? left.value
                                    : lowerer_.emitToString(left.value, leftType);

                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::Str), kStringConcat, {leftStr, right.value});
                lowerer_.consumeDeferred(leftStr);
                lowerer_.consumeDeferred(right.value);
                return {result, Type(Type::Kind::Str)};
            }
            op = isFloat ? Opcode::FAdd : Opcode::IAddOvf;
            break;

        case BinaryOp::Sub:
            op = isFloat ? Opcode::FSub : Opcode::ISubOvf;
            break;

        case BinaryOp::Mul:
            op = isFloat ? Opcode::FMul : Opcode::IMulOvf;
            break;

        case BinaryOp::Div:
            op = isFloat ? Opcode::FDiv : Opcode::SDivChk0;
            break;

        case BinaryOp::Mod:
            op = Opcode::SRemChk0;
            break;

        case BinaryOp::Eq:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::I1), kStringEquals, {left.value, right.value});
                return {result, Type(Type::Kind::I1)};
            }
            if (isFloat) {
                op = Opcode::FCmpEQ;
                resultType = Type(Type::Kind::I1);
            } else {
                Value lhsExt = lowerer_.extendOperandForComparison(left.value, left.type);
                Value rhsExt = lowerer_.extendOperandForComparison(right.value, right.type);
                Value result =
                    lowerer_.emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), lhsExt, rhsExt);
                return {result, Type(Type::Kind::I1)};
            }
            break;

        case BinaryOp::Ne:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value eqResult = lowerer_.emitCallRet(
                    Type(Type::Kind::I1), kStringEquals, {left.value, right.value});
                Value eqI64 = lowerer_.emitUnary(Opcode::Zext1, Type(Type::Kind::I64), eqResult);
                Value result = lowerer_.emitBinary(
                    Opcode::ICmpEq, Type(Type::Kind::I1), eqI64, Value::constInt(0));
                return {result, Type(Type::Kind::I1)};
            }
            if (isFloat) {
                op = Opcode::FCmpNE;
                resultType = Type(Type::Kind::I1);
            } else {
                Value lhsExt = lowerer_.extendOperandForComparison(left.value, left.type);
                Value rhsExt = lowerer_.extendOperandForComparison(right.value, right.type);
                Value result =
                    lowerer_.emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), lhsExt, rhsExt);
                return {result, Type(Type::Kind::I1)};
            }
            break;

        case BinaryOp::Lt:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::I64), "rt_str_lt", {left.value, right.value});
                Value boolResult = lowerer_.emitBinary(
                    Opcode::ICmpNe, Type(Type::Kind::I1), result, Value::constInt(0));
                return {boolResult, Type(Type::Kind::I1)};
            }
            op = isFloat ? Opcode::FCmpLT : Opcode::SCmpLT;
            resultType = Type(Type::Kind::I1);
            break;

        case BinaryOp::Le:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::I64), "rt_str_le", {left.value, right.value});
                Value boolResult = lowerer_.emitBinary(
                    Opcode::ICmpNe, Type(Type::Kind::I1), result, Value::constInt(0));
                return {boolResult, Type(Type::Kind::I1)};
            }
            op = isFloat ? Opcode::FCmpLE : Opcode::SCmpLE;
            resultType = Type(Type::Kind::I1);
            break;

        case BinaryOp::Gt:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::I64), "rt_str_gt", {left.value, right.value});
                Value boolResult = lowerer_.emitBinary(
                    Opcode::ICmpNe, Type(Type::Kind::I1), result, Value::constInt(0));
                return {boolResult, Type(Type::Kind::I1)};
            }
            op = isFloat ? Opcode::FCmpGT : Opcode::SCmpGT;
            resultType = Type(Type::Kind::I1);
            break;

        case BinaryOp::Ge:
            if (leftType && leftType->kind == TypeKindSem::String) {
                Value result = lowerer_.emitCallRet(
                    Type(Type::Kind::I64), "rt_str_ge", {left.value, right.value});
                Value boolResult = lowerer_.emitBinary(
                    Opcode::ICmpNe, Type(Type::Kind::I1), result, Value::constInt(0));
                return {boolResult, Type(Type::Kind::I1)};
            }
            op = isFloat ? Opcode::FCmpGE : Opcode::SCmpGE;
            resultType = Type(Type::Kind::I1);
            break;

        case BinaryOp::And: {
            Value lhsExt =
                (left.type.kind == Type::Kind::I1)
                    ? lowerer_.emitUnary(Opcode::Zext1, Type(Type::Kind::I64), left.value)
                    : left.value;
            Value rhsExt =
                (right.type.kind == Type::Kind::I1)
                    ? lowerer_.emitUnary(Opcode::Zext1, Type(Type::Kind::I64), right.value)
                    : right.value;
            Value andResult =
                lowerer_.emitBinary(Opcode::And, Type(Type::Kind::I64), lhsExt, rhsExt);
            Value truncResult = lowerer_.emitUnary(Opcode::Trunc1, Type(Type::Kind::I1), andResult);
            return {truncResult, Type(Type::Kind::I1)};
        }

        case BinaryOp::Or: {
            Value lhsExt =
                (left.type.kind == Type::Kind::I1)
                    ? lowerer_.emitUnary(Opcode::Zext1, Type(Type::Kind::I64), left.value)
                    : left.value;
            Value rhsExt =
                (right.type.kind == Type::Kind::I1)
                    ? lowerer_.emitUnary(Opcode::Zext1, Type(Type::Kind::I64), right.value)
                    : right.value;
            Value orResult = lowerer_.emitBinary(Opcode::Or, Type(Type::Kind::I64), lhsExt, rhsExt);
            Value truncResult = lowerer_.emitUnary(Opcode::Trunc1, Type(Type::Kind::I1), orResult);
            return {truncResult, Type(Type::Kind::I1)};
        }

        case BinaryOp::BitAnd:
            op = Opcode::And;
            break;

        case BinaryOp::BitOr:
            op = Opcode::Or;
            break;

        case BinaryOp::BitXor:
            op = Opcode::Xor;
            break;

        case BinaryOp::Shl:
            op = Opcode::Shl;
            break;

        case BinaryOp::Shr:
            op = Opcode::AShr;
            break;

        case BinaryOp::Assign:
            break;
    }

    Value result = lowerer_.emitBinary(op, resultType, left.value, right.value);
    return {result, resultType};
}

} // namespace il::frontends::zia
