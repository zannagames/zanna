//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the numeric lowering helpers used by the BASIC front end.  The
// routines in this file perform operand coercions, select appropriate IL
// opcodes, and emit runtime helper calls for numerically sensitive operations
// such as exponentiation or string concatenation.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Numeric expression lowering utilities for BASIC.
/// @details Provides the implementation behind `Lowerer` helpers that handle
///          arithmetic, relational comparisons, and mixed-type operations,
///          including specialised handling for string concatenation and
///          constant folding patterns.

#include "frontends/basic/LowerExprNumeric.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/NumericRules.hpp"
#include "frontends/basic/RuntimeNames.hpp"
#include "frontends/basic/TypeSuffix.hpp"

#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace il::frontends::basic {
using namespace il::core;
using namespace il::frontends::basic::runtime;

using IlType = il::core::Type;
using IlKind = IlType::Kind;

// Import shared numeric rules
namespace nr = numeric_rules;

namespace {

/// @brief Test whether an IL kind participates in BASIC integer arithmetic.
/// @param kind IL type kind to classify.
/// @return True for an integer kind recognized by the shared numeric rules.
bool isIntegerKind(IlKind kind) {
    return nr::isIlInteger(kind);
}

/// @brief Select the shared integer promotion kind for two operands.
/// @param lhsKind IL kind of the left operand.
/// @param rhsKind IL kind of the right operand.
/// @return IL type wrapping the promoted integer kind.
IlType integerArithmeticType(IlKind lhsKind, IlKind rhsKind) {
    return IlType(nr::promoteIlInteger(lhsKind, rhsKind));
}

} // namespace

/// @brief Bind the numeric lowering helper to a concrete lowering engine.
///
/// @param lowerer Borrowed lowering driver; it must outlive this helper.
NumericExprLowering::NumericExprLowering(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

/// @brief Lower integer division or modulus with divide-by-zero checking.
///
/// @details Eagerly lowers both operands, coerces each to `i64`, and emits
///          SDivChk0 for integer division or SRemChk0 for MOD. No narrow
///          integer variant is selected because both checked opcodes require
///          `i64` operands.
///
/// @param expr Binary expression node representing IDIV or MOD.
/// @return Lowered r-value carrying the operation result.
Lowerer::RVal NumericExprLowering::lowerDivOrMod(const BinaryExpr &expr) {
    Lowerer &lowerer = *lowerer_;
    Lowerer::RVal lhs = lowerer.lowerExpr(*expr.lhs);
    Lowerer::RVal rhs = lowerer.lowerExpr(*expr.rhs);

    // Note: sdiv.chk0 and srem.chk0 IL instructions only support i64 operands.
    // Always coerce to i64 rather than trying to narrow to i16/i32.
    lhs = lowerer.coerceToI64(std::move(lhs), expr.loc);
    rhs = lowerer.coerceToI64(std::move(rhs), expr.loc);

    Opcode op = (expr.op == BinaryExpr::Op::IDiv) ? Opcode::SDivChk0 : Opcode::SRemChk0;
    IlType resultTy(IlKind::I64);

    lowerer.curLoc = expr.loc;
    Value res = lowerer.emitBinary(op, resultTy, lhs.value, rhs.value);
    return {res, resultTy};
}

/// @brief Normalise operands for a numeric binary operation.
///
/// @details Applies BASIC's promotion and coercion rules to ensure both operands
///          use compatible types, capturing the chosen arithmetic/result type in
///          a configuration structure for later use.
///
/// @param expr Binary expression being lowered.
/// @param lhs Left-hand side value to normalise (updated in place).
/// @param rhs Right-hand side value to normalise (updated in place).
/// @return Configuration describing operand category and result type.
NumericExprLowering::NumericOpConfig NumericExprLowering::normalizeNumericOperands(
    const BinaryExpr &expr, Lowerer::RVal &lhs, Lowerer::RVal &rhs) {
    Lowerer &lowerer = *lowerer_;
    NumericOpConfig config;

    // Use shared rule to determine if operator requires float operands
    const bool requiresFloat = nr::requiresFloatOperands(expr.op);
    if (requiresFloat) {
        /// Promote one operand to `f64`, using its node location when available.
        auto promoteToF64 = [&](Lowerer::RVal &value, const Expr *node) {
            if (value.type.kind == IlKind::F64)
                return;
            il::support::SourceLoc loc = node ? node->loc : expr.loc;
            // Route all float promotions through the TypeCoercionEngine
            // (toI64 normalisation followed by int-to-float conversion).
            value = lowerer.ensureF64(std::move(value), loc);
        };

        promoteToF64(lhs, expr.lhs.get());
        promoteToF64(rhs, expr.rhs.get());
        config.isFloat = true;
        config.arithmeticType = IlType(IlKind::F64);
        config.resultType = IlType(IlKind::F64);
        return config;
    }

    // Use shared rule to check if either operand is float
    if (nr::isIlFloat(lhs.type.kind) || nr::isIlFloat(rhs.type.kind)) {
        lhs = lowerer.coerceToF64(std::move(lhs), expr.loc);
        rhs = lowerer.coerceToF64(std::move(rhs), expr.loc);
        config.isFloat = true;
        config.arithmeticType = IlType(IlKind::F64);
        config.resultType = IlType(IlKind::F64);
        return config;
    }

    config.isFloat = false;
    config.arithmeticType = integerArithmeticType(lhs.type.kind, rhs.type.kind);
    config.resultType = config.arithmeticType;

    const auto *lhsInt = as<const IntExpr>(*expr.lhs);
    const auto *rhsInt = as<const IntExpr>(*expr.rhs);
    if (lhsInt && rhsInt) {
        /// Test whether an integer literal fits the signed 16-bit range.
        const auto fits16 = [](long long v) {
            return v >= std::numeric_limits<int16_t>::min() &&
                   v <= std::numeric_limits<int16_t>::max();
        };
        /// Test whether an integer literal fits the signed 32-bit range.
        const auto fits32 = [](long long v) {
            return v >= std::numeric_limits<int32_t>::min() &&
                   v <= std::numeric_limits<int32_t>::max();
        };
        if (fits16(lhsInt->value) && fits16(rhsInt->value)) {
            config.arithmeticType = IlType(IlKind::I16);
            config.resultType = config.arithmeticType;
        } else if (fits32(lhsInt->value) && fits32(rhsInt->value)) {
            config.arithmeticType = IlType(IlKind::I32);
            config.resultType = config.arithmeticType;
        }
    }

    // Coerce operands to match the chosen arithmetic type (fixes BUG-012: boolean
    // variables are i16, but TRUE/FALSE constants are i64, requiring promotion)
    if (lhs.type.kind != config.arithmeticType.kind) {
        if (config.arithmeticType.kind == IlKind::I64)
            lhs = lowerer.coerceToI64(std::move(lhs), expr.loc);
    }
    if (rhs.type.kind != config.arithmeticType.kind) {
        if (config.arithmeticType.kind == IlKind::I64)
            rhs = lowerer.coerceToI64(std::move(rhs), expr.loc);
    }

    return config;
}

/// @brief Detect and lower bespoke constant-folding opportunities.
///
/// @details Handles `0 - X` only when the left AST node is literal zero and the
///          already normalized right operand has `i16` or `i32` type. Other
///          integer widths, floating-point operands, and operators fall through.
///
/// @param expr Binary expression under consideration.
/// @param lhs Normalised left-hand operand.
/// @param rhs Normalised right-hand operand.
/// @param config Operand configuration returned by normalisation.
/// @return Lowered value when a special case applies; `std::nullopt` otherwise.
std::optional<Lowerer::RVal> NumericExprLowering::applySpecialConstantPatterns(
    const BinaryExpr &expr, Lowerer::RVal &lhs, Lowerer::RVal &rhs, const NumericOpConfig &config) {
    (void)lhs;
    if (expr.op != BinaryExpr::Op::Sub || config.isFloat)
        return std::nullopt;

    const auto *lhsInt = as<const IntExpr>(*expr.lhs);
    if (!lhsInt || lhsInt->value != 0)
        return std::nullopt;

    if (!isIntegerKind(rhs.type.kind))
        return std::nullopt;

    if (rhs.type.kind != IlKind::I16 && rhs.type.kind != IlKind::I32)
        return std::nullopt;

    Lowerer &lowerer = *lowerer_;
    lowerer.curLoc = expr.loc;
    Value neg = lowerer.emitCheckedNeg(rhs.type, rhs.value);
    return Lowerer::RVal{neg, rhs.type};
}

/// @brief Choose the IL opcode used to implement a numeric binary operation.
///
/// @details Takes into account whether the operands are floating-point or
///          integer and whether the operation yields a boolean result, in which
///          case the helper also records whether the boolean must be promoted to
///          BASIC's -1/0 logical representation.
///
/// @param op Binary operator being lowered.
/// @param config Operand configuration describing operand categories.
/// @return Structure containing the opcode, result type, and promotion flags.
NumericExprLowering::OpcodeSelection NumericExprLowering::selectNumericOpcode(
    BinaryExpr::Op op, const NumericOpConfig &config) {
    Lowerer &lowerer = *lowerer_;
    OpcodeSelection selection;
    selection.resultType = config.arithmeticType;

    switch (op) {
        case BinaryExpr::Op::Add:
            selection.opcode = config.isFloat ? Opcode::FAdd : Opcode::IAddOvf;
            break;
        case BinaryExpr::Op::Sub:
            selection.opcode = config.isFloat ? Opcode::FSub : Opcode::ISubOvf;
            break;
        case BinaryExpr::Op::Mul:
            selection.opcode = config.isFloat ? Opcode::FMul : Opcode::IMulOvf;
            break;
        case BinaryExpr::Op::Div:
            if (config.isFloat) {
                selection.opcode = Opcode::FDiv;
                selection.resultType = config.arithmeticType;
            } else {
                selection.opcode = Opcode::SDivChk0;
                selection.resultType = IlType(IlKind::I64);
            }
            break;
        case BinaryExpr::Op::Eq:
            selection.opcode = config.isFloat ? Opcode::FCmpEQ : Opcode::ICmpEq;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        case BinaryExpr::Op::Ne:
            selection.opcode = config.isFloat ? Opcode::FCmpNE : Opcode::ICmpNe;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        case BinaryExpr::Op::Lt:
            selection.opcode = config.isFloat ? Opcode::FCmpLT : Opcode::SCmpLT;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        case BinaryExpr::Op::Le:
            selection.opcode = config.isFloat ? Opcode::FCmpLE : Opcode::SCmpLE;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        case BinaryExpr::Op::Gt:
            selection.opcode = config.isFloat ? Opcode::FCmpGT : Opcode::SCmpGT;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        case BinaryExpr::Op::Ge:
            selection.opcode = config.isFloat ? Opcode::FCmpGE : Opcode::SCmpGE;
            selection.resultType = lowerer.ilBoolTy();
            selection.promoteBoolToI64 = true;
            break;
        default:
            break;
    }

    return selection;
}

/// @brief Lower the BASIC exponentiation operator.
///
/// @details Normalises operands to floating point and calls the runtime helper
///          that performs domain-checked exponentiation, recording that the
///          helper must be linked in.
///
/// @param expr AST node for the POW operation.
/// @param lhs Left-hand operand (moved into the helper).
/// @param rhs Right-hand operand (moved into the helper).
/// @return Lowered value representing the power result.
Lowerer::RVal NumericExprLowering::lowerPowBinary(const BinaryExpr &expr,
                                                  Lowerer::RVal lhs,
                                                  Lowerer::RVal rhs) {
    Lowerer &lowerer = *lowerer_;
    NumericOpConfig config = normalizeNumericOperands(expr, lhs, rhs);
    lowerer.trackRuntime(Lowerer::RuntimeFeature::Pow);
    lowerer.curLoc = expr.loc;
    Value res = lowerer.emitCallRet(IlType(IlKind::F64), "rt_pow_f64", {lhs.value, rhs.value});
    IlType resultType =
        (config.resultType.kind == IlKind::Void) ? IlType(IlKind::F64) : config.resultType;
    return {res, resultType};
}

/// @brief Lower binary operations when operands are strings.
///
/// @details Addition calls `kStringConcat`, defers release of the returned
///          string, and reports a string result. Equality, inequality, and all
///          four ordering relations call their runtime comparators, widen an
///          `i1` equality result when necessary, and convert the result to
///          BASIC's `i64` -1/0 logical convention.
///
/// @param expr AST node for the string operation.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value storing the operation result.
Lowerer::RVal NumericExprLowering::lowerStringBinary(const BinaryExpr &expr,
                                                     Lowerer::RVal lhs,
                                                     Lowerer::RVal rhs) {
    Lowerer &lowerer = *lowerer_;
    lowerer.curLoc = expr.loc;
    if (expr.op == BinaryExpr::Op::Add) {
        // Ensure runtime signature is linked for string concatenation.
        lowerer.trackRuntime(Lowerer::RuntimeFeature::Concat);
        // BUG-110: Avoid per-iteration alloca spills; pass operands directly.
        Value res = lowerer.emitCallRet(IlType(IlKind::Str), kStringConcat, {lhs.value, rhs.value});
        lowerer.deferReleaseStr(res);
        return {res, IlType(IlKind::Str)};
    }

    // String comparison operators - select appropriate runtime function
    const char *rtFunc = nullptr;
    bool needsNegation = false;

    switch (expr.op) {
        case BinaryExpr::Op::Eq:
            rtFunc = "rt_str_eq";
            break;
        case BinaryExpr::Op::Ne:
            rtFunc = "rt_str_eq";
            needsNegation = true;
            break;
        case BinaryExpr::Op::Lt:
            rtFunc = "rt_str_lt";
            break;
        case BinaryExpr::Op::Le:
            rtFunc = "rt_str_le";
            break;
        case BinaryExpr::Op::Gt:
            rtFunc = "rt_str_gt";
            break;
        case BinaryExpr::Op::Ge:
            rtFunc = "rt_str_ge";
            break;
        default:
            // Should not reach here - validated by semantic analysis
            rtFunc = "rt_str_eq";
            break;
    }

    // rt_str_eq returns i1 (boolean), other string comparisons return i64
    bool isStrEq = (std::strcmp(rtFunc, "rt_str_eq") == 0);
    IlType callRetTy = isStrEq ? IlType(IlKind::I1) : IlType(IlKind::I64);
    Value cmp = lowerer.emitCallRet(callRetTy, rtFunc, {lhs.value, rhs.value});
    // Widen i1 to i64 if needed
    if (isStrEq)
        cmp = lowerer.emitZext1ToI64(cmp);
    // Convert to BASIC logical form: 0 stays 0, 1 becomes -1 (negate: 0-cmp)
    Value zero = lowerer.emitConstI64(0);
    Value cmpLogical = lowerer.emitISub(zero, cmp);

    if (needsNegation) {
        Value res = lowerer.emitCommon(expr.loc).logical_xor(cmpLogical, lowerer.emitConstI64(-1));
        return {res, IlType(IlKind::I64)};
    }

    return {cmpLogical, IlType(IlKind::I64)};
}

/// @brief Lower arithmetic or comparison operators on numeric operands.
///
/// @details Normalises operands, applies special constant folding, selects the
///          appropriate opcode, and emits the final IL.  Comparison results are
///          expanded to BASIC's logical -1/0 representation when required.
///
/// @param expr AST node for the binary operation.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered r-value carrying the operation result.
Lowerer::RVal NumericExprLowering::lowerNumericBinary(const BinaryExpr &expr,
                                                      Lowerer::RVal lhs,
                                                      Lowerer::RVal rhs) {
    Lowerer &lowerer = *lowerer_;
    NumericOpConfig config = normalizeNumericOperands(expr, lhs, rhs);

    if (auto special = applySpecialConstantPatterns(expr, lhs, rhs, config))
        return *special;

    OpcodeSelection selection = selectNumericOpcode(expr.op, config);
    lowerer.curLoc = expr.loc;
    Value res = lowerer.emitBinary(selection.opcode, selection.resultType, lhs.value, rhs.value);
    if (selection.promoteBoolToI64) {
        lowerer.curLoc = expr.loc;
        Value logical = lowerer.emitBasicLogicalI64(res);
        return {logical, IlType(IlKind::I64)};
    }
    return {res, selection.resultType};
}

/// @brief Entry point on `Lowerer` for lowering division or modulus.
///
/// @param expr Binary expression node representing IDIV or MOD.
/// @return Lowered value after delegating to `NumericExprLowering`.
Lowerer::RVal Lowerer::lowerDivOrMod(const BinaryExpr &expr) {
    NumericExprLowering lowering(*this);
    return lowering.lowerDivOrMod(expr);
}

/// @brief Entry point for lowering exponentiation with explicit operands.
///
/// @param expr Binary expression node.
/// @param lhs Left-hand operand already partially lowered.
/// @param rhs Right-hand operand already partially lowered.
/// @return Lowered value computed by the numeric helper.
Lowerer::RVal Lowerer::lowerPowBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    NumericExprLowering lowering(*this);
    return lowering.lowerPowBinary(expr, std::move(lhs), std::move(rhs));
}

/// @brief Entry point for lowering string-aware binary operations.
///
/// @param expr Binary expression node describing the operation.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value generated by the string helper.
Lowerer::RVal Lowerer::lowerStringBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    NumericExprLowering lowering(*this);
    return lowering.lowerStringBinary(expr, std::move(lhs), std::move(rhs));
}

/// @brief Entry point for lowering generic numeric binary operations.
///
/// @param expr Binary expression node.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value after delegating to `NumericExprLowering`.
Lowerer::RVal Lowerer::lowerNumericBinary(const BinaryExpr &expr, RVal lhs, RVal rhs) {
    NumericExprLowering lowering(*this);
    return lowering.lowerNumericBinary(expr, std::move(lhs), std::move(rhs));
}

/// @brief Free-function wrapper for lowering division or modulus.
///
/// @param lowerer Lowering engine to use.
/// @param expr Binary expression to lower.
/// @return Lowered value provided by the helper.
Lowerer::RVal lowerDivOrMod(Lowerer &lowerer, const BinaryExpr &expr) {
    NumericExprLowering lowering(lowerer);
    return lowering.lowerDivOrMod(expr);
}

/// @brief Free-function wrapper that lowers exponentiation expressions.
///
/// @param lowerer Lowering engine to use.
/// @param expr Binary exponentiation node.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value from the numeric helper.
Lowerer::RVal lowerPowBinary(Lowerer &lowerer,
                             const BinaryExpr &expr,
                             Lowerer::RVal lhs,
                             Lowerer::RVal rhs) {
    NumericExprLowering lowering(lowerer);
    return lowering.lowerPowBinary(expr, std::move(lhs), std::move(rhs));
}

/// @brief Free-function wrapper that lowers string binary operations.
///
/// @param lowerer Lowering engine to use.
/// @param expr Binary expression node.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value emitted by the helper.
Lowerer::RVal lowerStringBinary(Lowerer &lowerer,
                                const BinaryExpr &expr,
                                Lowerer::RVal lhs,
                                Lowerer::RVal rhs) {
    NumericExprLowering lowering(lowerer);
    return lowering.lowerStringBinary(expr, std::move(lhs), std::move(rhs));
}

/// @brief Free-function wrapper that lowers generic numeric binary operations.
///
/// @param lowerer Lowering engine to use.
/// @param expr Binary expression node.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Lowered value provided by the helper.
Lowerer::RVal lowerNumericBinary(Lowerer &lowerer,
                                 const BinaryExpr &expr,
                                 Lowerer::RVal lhs,
                                 Lowerer::RVal rhs) {
    NumericExprLowering lowering(lowerer);
    return lowering.lowerNumericBinary(expr, std::move(lhs), std::move(rhs));
}

} // namespace il::frontends::basic
