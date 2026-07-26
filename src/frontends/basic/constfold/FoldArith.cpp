//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/constfold/FoldArith.cpp
// Purpose: Implement arithmetic constant-folding helpers for the BASIC front
//          end dispatcher.
// Key invariants: Folding must honour BASIC overflow semantics, return
//                 Value::invalid() when a fold is unsafe, and avoid mutating the
//                 original AST or Constant inputs.
// Ownership/Lifetime: Operates purely on lightweight Value/Constant summaries;
//                     callers retain ownership of AST nodes and runtime data.
// Links: docs/tutorials/basic-tutorial.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements numeric folding utilities shared by the dispatcher.
/// @details The helpers evaluate constant expressions in place, promoting
///          operands as necessary and preserving diagnostic information so the
///          caller can decide whether folding succeeded.
#include "common/IntegerHelpers.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "frontends/basic/constfold/Dispatch.hpp"
#include "frontends/basic/constfold/Value.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

namespace il::frontends::basic::constfold {
namespace {
namespace intops = il::common::integer;

constexpr std::size_t kOpCount = static_cast<std::size_t>(AST::BinaryExpr::Op::LogicalOr) + 1;

using BinOpFn = Value (*)(Value, Value);

/// @brief Determine whether an integer falls within the 16-bit BASIC range.
/// @details BASIC short integers clamp to +/-32768 during folding; when the
///          folded value would leave that range the helper returns
///          @ref Value::invalid so callers can bail out and defer to runtime
///          traps.  The range check operates on promoted 64-bit intermediates to
///          avoid overflow in the comparison itself.
/// @param v Signed integer candidate from the folding inputs.
/// @return @c true when @p v is representable as a signed 16-bit value.
[[nodiscard]] constexpr bool is_i16(long long v) noexcept {
    return v >= static_cast<long long>(std::numeric_limits<std::int16_t>::min()) &&
           v <= static_cast<long long>(std::numeric_limits<std::int16_t>::max());
}

/// @brief Perform wraparound addition that mirrors BASIC integer semantics.
/// @details Promotes operands to an unsigned 64-bit domain before adding so the
///          result naturally wraps at 64 bits.  The cast back to @c long long
///          then preserves the two's-complement interpretation expected by the
///          runtime.
/// @param lhs Left-hand integer operand.
/// @param rhs Right-hand integer operand.
/// @return Sum computed with modulo-2^64 semantics.
[[nodiscard]] long long wrap_add(long long lhs, long long rhs) noexcept {
    const auto promoted = intops::promote_binary(lhs, rhs);
    const auto sum =
        static_cast<std::uint64_t>(promoted.lhs) + static_cast<std::uint64_t>(promoted.rhs);
    return static_cast<long long>(sum);
}

/// @brief Perform wraparound subtraction with BASIC integer semantics.
/// @details Mirrors @ref wrap_add by promoting to unsigned arithmetic so
///          underflow wraps naturally.  The promotion helper keeps the signed
///          interpretation consistent even for mixed-width inputs.
/// @param lhs Left-hand integer operand.
/// @param rhs Right-hand integer operand.
/// @return Difference computed modulo 2^64.
[[nodiscard]] long long wrap_sub(long long lhs, long long rhs) noexcept {
    const auto promoted = intops::promote_binary(lhs, rhs);
    const auto diff =
        static_cast<std::uint64_t>(promoted.lhs) - static_cast<std::uint64_t>(promoted.rhs);
    return static_cast<long long>(diff);
}

/// @brief Perform wraparound multiplication with BASIC integer semantics.
/// @details Promotes operands to the widened representation emitted by
///          @ref intops::promote_binary and multiplies them as unsigned 64-bit
///          integers so overflow wraps without triggering UB.
/// @param lhs Left-hand integer operand.
/// @param rhs Right-hand integer operand.
/// @return Product computed modulo 2^64.
[[nodiscard]] long long wrap_mul(long long lhs, long long rhs) noexcept {
    const auto promoted = intops::promote_binary(lhs, rhs);
    const auto prod =
        static_cast<std::uint64_t>(promoted.lhs) * static_cast<std::uint64_t>(promoted.rhs);
    return static_cast<long long>(prod);
}

/// @brief Fold an addition expression when both operands are constant.
/// @details Promotes to floating point when either operand is already a float;
///          otherwise performs integer addition with wraparound semantics.  When
///          both operands originate from 16-bit literals the helper refuses to
///          fold results that would overflow the 16-bit range so the caller can
///          emit the correct runtime trap.
/// @param lhs Left-hand operand as a folded @ref Value.
/// @param rhs Right-hand operand as a folded @ref Value.
/// @return Folded result or @ref Value::invalid on overflow/domain errors.
Value fold_add(Value lhs, Value rhs) {
    if (lhs.isFloat() || rhs.isFloat())
        return Value::fromFloat(lhs.asDouble() + rhs.asDouble());

    const long long sum = wrap_add(lhs.i, rhs.i);
    if (is_i16(lhs.i) && is_i16(rhs.i)) {
        if (sum < static_cast<long long>(std::numeric_limits<std::int16_t>::min()) ||
            sum > static_cast<long long>(std::numeric_limits<std::int16_t>::max()))
            return Value::invalid();
    }
    return Value::fromInt(sum);
}

/// @brief Fold a subtraction expression when both operands are constant.
/// @details Uses floating subtraction when either operand is a float; otherwise
///          relies on @ref wrap_sub to retain modulo semantics.  The helper does
///          not reject 16-bit overflow because BASIC allows wraparound for
///          subtraction.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Folded result or @ref Value::invalid when folding is not possible.
Value fold_sub(Value lhs, Value rhs) {
    if (lhs.isFloat() || rhs.isFloat())
        return Value::fromFloat(lhs.asDouble() - rhs.asDouble());
    return Value::fromInt(wrap_sub(lhs.i, rhs.i));
}

/// @brief Fold a multiplication expression when both operands are constant.
/// @details Promotes to floating point when needed; otherwise multiplies via
///          @ref wrap_mul so integer wraparound matches runtime behaviour.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Folded value or @ref Value::invalid when folding fails.
Value fold_mul(Value lhs, Value rhs) {
    if (lhs.isFloat() || rhs.isFloat())
        return Value::fromFloat(lhs.asDouble() * rhs.asDouble());
    return Value::fromInt(wrap_mul(lhs.i, rhs.i));
}

/// @brief Fold a floating division expression when both operands are constant.
/// @details Rejects division by zero before performing a double-precision
///          division so that callers can emit a runtime trap instead of folding
///          an invalid value.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Folded floating result or @ref Value::invalid on zero divisor.
Value fold_div(Value lhs, Value rhs) {
    const double divisor = rhs.asDouble();
    if (divisor == 0.0)
        return Value::invalid();
    return Value::fromFloat(lhs.asDouble() / divisor);
}

/// @brief Fold integer division when both operands are constant integers.
/// @details Requires both operands to be integral and rejects zero divisors so
///          runtime traps are preserved.  Successful folds perform truncating
///          integer division just like BASIC's `\` operator.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Folded quotient or @ref Value::invalid on failure.
Value fold_idiv(Value lhs, Value rhs) {
    if (!lhs.isInt() || !rhs.isInt())
        return Value::invalid();
    if (rhs.i == 0)
        return Value::invalid();
    return Value::fromInt(lhs.i / rhs.i);
}

/// @brief Fold integer modulo when both operands are constant integers.
/// @details Rejects non-integer operands and zero divisors so behaviour matches
///          runtime traps.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Folded remainder or @ref Value::invalid on failure.
Value fold_mod(Value lhs, Value rhs) {
    if (!lhs.isInt() || !rhs.isInt())
        return Value::invalid();
    if (rhs.i == 0)
        return Value::invalid();
    return Value::fromInt(lhs.i % rhs.i);
}

/// @brief Build the dispatch table that maps AST arithmetic ops to fold helpers.
/// @details Populates an array indexed by @ref AST::BinaryExpr::Op where entries
///          reference the appropriate folding routine or @c nullptr when an
///          operation cannot be folded by this domain.
/// @return Fully initialised dispatch table.
constexpr std::array<BinOpFn, kOpCount> make_arith_table() {
    std::array<BinOpFn, kOpCount> table{};
    table.fill(nullptr);
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::Add)] = &fold_add;
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::Sub)] = &fold_sub;
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::Mul)] = &fold_mul;
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::Div)] = &fold_div;
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::IDiv)] = &fold_idiv;
    table[static_cast<std::size_t>(AST::BinaryExpr::Op::Mod)] = &fold_mod;
    return table;
}

constexpr auto kArithFold = make_arith_table();

/// @brief Convert a parsed constant into a folding @ref Value when possible.
/// @details Prefers the numeric payload when present, falling back to parsing
///          the literal string to maintain legacy semantics.  Invalid literals
///          yield @c std::nullopt so the caller can refuse the fold.
/// @param constant Constant descriptor extracted from the AST.
/// @return Foldable value or empty optional when conversion fails.
[[nodiscard]] std::optional<Value> makeValueFromConstant(const Constant &constant) {
    if (constant.kind == LiteralKind::Int || constant.kind == LiteralKind::Float) {
        if (constant.stringValue.empty())
            return makeValue(constant.numeric);

        auto parsed = detail::parseNumericLiteral(constant.stringValue);
        if (parsed.ok)
            return parsed.isFloat ? Value::fromFloat(parsed.d) : Value::fromInt(parsed.i);
        return makeValue(constant.numeric);
    }

    if (constant.kind == LiteralKind::Invalid && !constant.stringValue.empty()) {
        auto parsed = detail::parseNumericLiteral(constant.stringValue);
        if (!parsed.ok)
            return std::nullopt;
        return parsed.isFloat ? Value::fromFloat(parsed.d) : Value::fromInt(parsed.i);
    }

    return std::nullopt;
}

/// @brief Attempt to fold a binary arithmetic expression to a constant.
/// @details Looks up the appropriate folding helper, promotes operands to a
///          compatible representation, executes the fold, and converts back to a
///          @ref Value.  Invalid folds propagate as @c std::nullopt so callers
///          can leave the expression untouched.
/// @param op Binary operator kind.
/// @param lhs Left-hand operand after literal extraction.
/// @param rhs Right-hand operand after literal extraction.
/// @return Folded value or empty optional when folding is not possible.
std::optional<Value> tryFold(AST::BinaryExpr::Op op, Value lhs, Value rhs) {
    if (!lhs.valid || !rhs.valid)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(op);
    if (index >= kArithFold.size())
        return std::nullopt;
    auto fn = kArithFold[index];
    if (!fn)
        return std::nullopt;
    auto promoted = promote(lhs, rhs);
    Value result = fn(promoted.first, promoted.second);
    if (!result.valid)
        return std::nullopt;
    return result;
}

} // namespace

/// @copydoc fold_unary_arith(AST::UnaryExpr::Op, const AST::Expr &)
AST::ExprPtr fold_unary_arith(AST::UnaryExpr::Op op, const AST::Expr &value) {
    auto numeric = numeric_from_expr(value);
    if (!numeric)
        return nullptr;

    NumericValue result = *numeric;
    switch (op) {
        case AST::UnaryExpr::Op::Plus:
            break;
        case AST::UnaryExpr::Op::Negate:
            if (result.isFloat) {
                result = NumericValue{true, -result.f, static_cast<long long>(-result.f)};
            } else {
                auto negated = wrap_sub(0, result.i);
                result = NumericValue{false, static_cast<double>(negated), negated};
            }
            break;
        default:
            return nullptr;
    }

    if (result.isFloat) {
        auto out = std::make_unique<::il::frontends::basic::FloatExpr>();
        out->value = result.f;
        return out;
    }
    auto out = std::make_unique<::il::frontends::basic::IntExpr>();
    out->value = result.i;
    return out;
}

/// @brief Folds a binary arithmetic operation over normalized constants.
/// @details Converts both operands to the internal numeric representation,
///          applies BASIC promotion and checked folding rules, and converts a
///          valid result back to the public constant representation.
/// @param op Binary arithmetic operator to evaluate.
/// @param lhs Left constant operand.
/// @param rhs Right constant operand.
/// @return Folded constant, or `std::nullopt` when either operand is
///         non-numeric or the requested operation cannot be folded safely.
std::optional<Constant> fold_arith(AST::BinaryExpr::Op op,
                                   const Constant &lhs,
                                   const Constant &rhs) {
    auto lhsValue = makeValueFromConstant(lhs);
    auto rhsValue = makeValueFromConstant(rhs);
    if (!lhsValue || !rhsValue)
        return std::nullopt;

    auto folded = tryFold(op, *lhsValue, *rhsValue);
    if (!folded)
        return std::nullopt;

    Constant c;
    c.kind = folded->isFloat() ? LiteralKind::Float : LiteralKind::Int;
    c.numeric = toNumericValue(*folded);
    return c;
}

} // namespace il::frontends::basic::constfold
