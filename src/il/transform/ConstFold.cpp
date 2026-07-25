//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/il/transform/ConstFold.cpp
// Purpose: Implement the IL constant-folding pass that reduces literal
//          expressions and recognised runtime calls.
// Key invariants: Folding must never change observable behaviour—operations
//                 that would trap at runtime or depend on non-constant data must
//                 be left untouched.  Integer folding respects overflow flags,
//                 and floating-point folding matches runtime helper semantics.
// Ownership/Lifetime: Operates on caller-owned Module/Function instances and
//                     mutates instructions in place without allocating global
//                     state.
// Links: docs/il/il-guide.md#reference, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Constant folding implementation for IL modules.
/// @details Declares helper routines for extracting literal operands,
///          performing checked arithmetic, and substituting folded values before
///          exposing the public @ref constFold entry point.

#include "il/transform/ConstFold.hpp"
#include "il/core/FPCast.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Value.hpp"
#include "il/transform/OverflowArithmetic.hpp"
#include "il/utils/UseDefInfo.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <limits>
#include <optional>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {

/// @brief Extract a 64-bit integer constant operand.
/// @details Recognises @ref Value::Kind::ConstInt operands and exposes their
///          payload for folding without performing any conversions.  Other
///          operand kinds leave @p out untouched and cause the helper to fail so
///          callers can defer to runtime evaluation.
/// @param v Operand to inspect.
/// @param out Receives the integer payload when recognised.
/// @return True when @p v carries a constant integer value.
static bool isConstInt(const Value &v, long long &out) {
    if (v.kind == Value::Kind::ConstInt) {
        out = v.i64;
        return true;
    }
    return false;
}

/// @brief Extract a floating-point value suitable for math intrinsic folding.
/// @details Accepts floating and integer literals, promoting integers to double
///          precision using the default C++ conversion so runtime semantics are
///          preserved.  Temporaries and references cause the helper to fail so
///          the call remains at runtime.
/// @param v Operand to convert.
/// @param out Receives the double-precision value when available.
/// @return True when @p v provides a foldable floating-point payload.
static bool getConstFloat(const Value &v, double &out) {
    if (v.kind == Value::Kind::ConstFloat) {
        out = v.f64;
        return true;
    }
    if (v.kind == Value::Kind::ConstInt) {
        out = static_cast<double>(v.i64);
        return true;
    }
    return false;
}

/// @brief Return the arithmetic bit width of an IL integer kind.
/// @param kind Declared IL result type.
/// @return 16, 32, or 64; non-integer and unsupported kinds conservatively use 64.
static int integerTypeBits(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I16:
            return 16;
        case Type::Kind::I32:
            return 32;
        case Type::Kind::I64:
            return 64;
        default:
            return 64;
    }
}

/// @brief Sign-extend an integer constant to the instruction result width.
/// @details IL integer constants are stored in a 64-bit payload, but i16/i32
///          arithmetic observes narrower two's-complement wraparound. This helper
///          truncates to the declared width and sign-extends back to the payload.
/// @param value Raw folded integer value.
/// @param kind Declared result type of the instruction being folded.
/// @return Value represented in the declared integer width.
static long long normalizeIntegerForType(long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return value;
    const auto mask = (1ULL << static_cast<unsigned>(bits)) - 1ULL;
    const auto truncated = static_cast<unsigned long long>(value) & mask;
    const auto sign = 1ULL << static_cast<unsigned>(bits - 1);
    if ((truncated & sign) == 0)
        return static_cast<long long>(truncated);
    return static_cast<long long>(truncated | ~mask);
}

/// @brief Clear bits above the declared IL integer width.
/// @param value Full-width unsigned value to narrow.
/// @param kind Declared integer type that supplies the target width.
/// @return Unsigned payload canonicalized to @p kind.
static unsigned long long normalizeUnsignedForType(unsigned long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return value;
    return value & ((1ULL << static_cast<unsigned>(bits)) - 1ULL);
}

/// @brief Return the signed minimum for the declared IL integer width.
/// @param kind Declared IL integer type.
/// @return Minimum two's-complement value representable by @p kind.
static long long signedMinForType(Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return std::numeric_limits<long long>::min();
    return -(1LL << static_cast<unsigned>(bits - 1));
}

/// @brief Test whether a full-width signed result fits the IL result type.
/// @details Checked overflow opcodes trap on overflow in their declared result
///          width, not only on native 64-bit overflow. This range test keeps
///          constant folding from hiding sub-64-bit overflow traps.
/// @param value Full-width signed result.
/// @param kind Target IL integer type.
/// @return True when @p value is representable without truncation.
static bool fitsSignedIntegerType(long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return true;
    const long long min = -(1LL << static_cast<unsigned>(bits - 1));
    const long long max = (1LL << static_cast<unsigned>(bits - 1)) - 1LL;
    return value >= min && value <= max;
}

/// @brief Round a finite double using IEEE ties-to-even semantics.
/// @details `std::nearbyint` depends on the process floating-point rounding mode,
///          while the runtime helper is expected to round halves to the nearest
///          even integer deterministically.
/// @param value Finite value to round.
/// @return Rounded value, preserving signed zero.
static double roundHalfEven(double value) {
    const double lower = std::floor(value);
    const double fraction = value - lower;
    double rounded = lower;
    if (fraction > 0.5) {
        rounded = lower + 1.0;
    } else if (fraction == 0.5) {
        const double evenCandidate = std::fmod(std::fabs(lower), 2.0) == 0.0 ? lower : lower + 1.0;
        rounded = evenCandidate;
    }
    if (rounded == 0.0)
        return std::copysign(0.0, value);
    return rounded;
}

/// @brief Evaluate a small integral power using only sequenced IEEE
/// multiplications, avoiding host-libm variation in optimized IL.
/// @param base Floating-point base.
/// @param exponent Integral exponent limited to a deterministic range.
/// @param out Receives the finite result on success.
/// @return True when exponentiation remains finite and within the supported range.
static bool deterministicIntegralPower(double base, long long exponent, double &out) {
    if (exponent < -308 || exponent > 308)
        return false;
    const bool reciprocal = exponent < 0;
    unsigned long long remaining = reciprocal
                                       ? static_cast<unsigned long long>(-(exponent + 1)) + 1ULL
                                       : static_cast<unsigned long long>(exponent);
    double factor = base;
    double result = 1.0;
    while (remaining != 0) {
        if ((remaining & 1ULL) != 0)
            result *= factor;
        remaining >>= 1ULL;
        if (remaining != 0)
            factor *= factor;
    }
    if (reciprocal)
        result = 1.0 / result;
    if (!std::isfinite(result))
        return false;
    out = result;
    return true;
}

/// @brief Fold square roots only when integer arithmetic proves an exact root.
/// @param value Candidate nonnegative exactly represented integer.
/// @param out Receives the exact floating-point root on success.
/// @return True when @p value is a perfect square within the exact-integer range.
static bool exactIntegerSquareRoot(double value, double &out) {
    constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53
    if (!std::isfinite(value) || value < 0.0 || value > kMaxExactInteger ||
        value != std::trunc(value)) {
        return false;
    }
    const auto integer = static_cast<unsigned long long>(value);
    unsigned long long lo = 0;
    unsigned long long hi = std::min<unsigned long long>(integer, 94906266ULL) + 1ULL;
    while (lo + 1 < hi) {
        const unsigned long long mid = lo + (hi - lo) / 2;
        if (mid != 0 && mid > integer / mid)
            hi = mid;
        else
            lo = mid;
    }
    if (lo * lo != integer)
        return false;
    out = static_cast<double>(lo);
    return true;
}

/// @brief Select a deterministic IEEE-style minimum.
/// @details Returns the non-NaN operand when exactly one is NaN and preserves
///          negative zero when either equal-zero operand is negative.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Deterministic minimum result.
static double deterministicMin(double lhs, double rhs) {
    if (std::isnan(lhs))
        return rhs;
    if (std::isnan(rhs))
        return lhs;
    if (lhs == rhs && lhs == 0.0)
        return std::signbit(lhs) || std::signbit(rhs) ? -0.0 : 0.0;
    return lhs < rhs ? lhs : rhs;
}

/// @brief Select a deterministic IEEE-style maximum.
/// @details Returns the non-NaN operand when exactly one is NaN and returns
///          negative zero only when both equal-zero operands are negative.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Deterministic maximum result.
static double deterministicMax(double lhs, double rhs) {
    if (std::isnan(lhs))
        return rhs;
    if (std::isnan(rhs))
        return lhs;
    if (lhs == rhs && lhs == 0.0)
        return std::signbit(lhs) && std::signbit(rhs) ? -0.0 : 0.0;
    return lhs > rhs ? lhs : rhs;
}

/// @brief Perform checked addition mirroring the `.ovf` opcode semantics.
/// @details Uses compiler builtins to detect overflow; when detected the helper
///          returns @c std::nullopt so folding can be skipped and runtime traps
///          are preserved.
/// @param a Left-hand integer operand.
/// @param b Right-hand integer operand.
/// @return Folded result or empty optional when overflow occurred.
static std::optional<long long> checkedAdd(long long a, long long b) {
    long long result{};
    if (detail::addOverflows(a, b, result))
        return std::nullopt;
    return result;
}

/// @brief Perform checked subtraction mirroring the `.ovf` opcode semantics.
/// @param a Left-hand integer operand.
/// @param b Right-hand integer operand.
/// @return Folded result or empty optional when overflow occurred.
static std::optional<long long> checkedSub(long long a, long long b) {
    long long result{};
    if (detail::subOverflows(a, b, result))
        return std::nullopt;
    return result;
}

/// @brief Perform checked multiplication mirroring the `.ovf` opcode semantics.
/// @param a Left-hand integer operand.
/// @param b Right-hand integer operand.
/// @return Folded result or empty optional when overflow occurred.
static std::optional<long long> checkedMul(long long a, long long b) {
    long long result{};
    if (detail::mulOverflows(a, b, result))
        return std::nullopt;
    return result;
}

/// @brief Perform a portable sign-extending right shift.
/// @param value Signed value represented as a 64-bit two's-complement payload.
/// @param shift Shift count in the range [0, 63].
/// @return Arithmetic right-shift result without relying on implementation-defined
///         signed shifting.
static long long arithmeticShiftRight(long long value, unsigned shift) {
    if (shift == 0)
        return value;

    const auto bits = static_cast<unsigned long long>(value);
    auto shifted = bits >> shift;
    if ((bits & (1ULL << 63U)) != 0)
        shifted |= (~0ULL) << (64U - shift);
    return static_cast<long long>(shifted);
}

/// @brief Fold recognised math runtime calls into constants.
/// @details Handles a curated set of runtime helpers (absolute value, rounding,
///          power, square root, trigonometry, etc.) when all operands are
///          literals that satisfy each helper's domain restrictions.  When a
///          domain precondition fails or an operand is non-constant the helper
///          returns @c false so the call remains in the IR.
/// @param in Instruction describing the runtime call.
/// @param out Receives the folded constant on success.
/// @return True when folding succeeded and @p out contains the result.
static bool foldCall(const Instr &in, Value &out) {
    if (in.op != Opcode::Call)
        return false;
    const std::string &c = in.callee;
    if (c == "rt_val" || c == "rt_val_to_double" || c == "rt_int_to_str" || c == "rt_f64_to_str")
        return false;
    if (c == "rt_abs_i64" && in.operands.size() == 1) {
        long long v;
        if (isConstInt(in.operands[0], v) && v != std::numeric_limits<long long>::min()) {
            out = Value::constInt(v < 0 ? -v : v);
            return true;
        }
        return false;
    }
    double a;
    if (c == "rt_abs_f64" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            out = Value::constFloat(std::fabs(a));
            return true;
        }
        return false;
    }
    if (c == "rt_int_floor" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            out = Value::constFloat(std::floor(a));
            return true;
        }
        return false;
    }
    if (c == "rt_fix_trunc" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            out = Value::constFloat(std::trunc(a));
            return true;
        }
        return false;
    }
    if (c == "rt_floor" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            out = Value::constFloat(std::floor(a));
            return true;
        }
        return false;
    }
    if (c == "rt_ceil" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            out = Value::constFloat(std::ceil(a));
            return true;
        }
        return false;
    }
    if (c == "rt_sqrt" && in.operands.size() == 1) {
        double root = 0.0;
        if (getConstFloat(in.operands[0], a) && exactIntegerSquareRoot(a, root)) {
            out = Value::constFloat(root);
            return true;
        }
        return false;
    }
    if ((c == "rt_pow_f64_chkdom" || c == "rt_pow_f64" || c == "rt_math_pow") &&
        in.operands.size() == 2) {
        double base = 0.0;
        double expd = 0.0;
        if (getConstFloat(in.operands[0], base) && getConstFloat(in.operands[1], expd)) {
            const bool expIntegral = std::isfinite(expd) && (expd == std::trunc(expd));
            if (!(base < 0.0 && !expIntegral) && expIntegral) {
                const double truncated = std::trunc(expd);
                if (std::fabs(truncated) <= 16.0) {
                    const long long exp = static_cast<long long>(truncated);
                    double value = 0.0;
                    if (deterministicIntegralPower(base, exp, value)) {
                        out = Value::constFloat(value);
                        return true;
                    }
                }
            }
        }
        return false;
    }
    if (c == "rt_round_even" && in.operands.size() == 2) {
        double value = 0.0;
        long long digits = 0;
        if (getConstFloat(in.operands[0], value) && isConstInt(in.operands[1], digits)) {
            if (!std::isfinite(value)) {
                out = Value::constFloat(value);
                return true;
            }
            if (digits == 0) {
                out = Value::constFloat(roundHalfEven(value));
                return true;
            }
            const double digitsAsDouble = static_cast<double>(digits);
            if (!std::isfinite(digitsAsDouble) || std::fabs(digitsAsDouble) > 308.0) {
                out = Value::constFloat(value);
                return true;
            }
            double factor = 0.0;
            if (!deterministicIntegralPower(10.0, digits, factor) || factor == 0.0)
                return false;
            const double scaled = value * factor;
            if (!std::isfinite(scaled)) {
                out = Value::constFloat(value);
                return true;
            }
            const double rounded = roundHalfEven(scaled);
            out = Value::constFloat(rounded / factor);
            return true;
        }
        return false;
    }
    if (c == "rt_sin" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    if (c == "rt_cos" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(1.0);
            return true;
        }
        return false;
    }
    // Trigonometric functions with constant folding for specific values
    if (c == "rt_tan" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    if (c == "rt_asin" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    if (c == "rt_acos" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 1.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    if (c == "rt_atan" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    // Log/exp functions
    if (c == "rt_log" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 1.0) {
            out = Value::constFloat(0.0);
            return true;
        }
        return false;
    }
    if (c == "rt_exp" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a) && a == 0.0) {
            out = Value::constFloat(1.0);
            return true;
        }
        return false;
    }
    // Min/max with constant arguments
    double b;
    if (c == "rt_min_f64" && in.operands.size() == 2) {
        if (getConstFloat(in.operands[0], a) && getConstFloat(in.operands[1], b)) {
            out = Value::constFloat(deterministicMin(a, b));
            return true;
        }
        return false;
    }
    if (c == "rt_max_f64" && in.operands.size() == 2) {
        if (getConstFloat(in.operands[0], a) && getConstFloat(in.operands[1], b)) {
            out = Value::constFloat(deterministicMax(a, b));
            return true;
        }
        return false;
    }
    // Integer min/max
    long long ai, bi;
    if (c == "rt_min_i64" && in.operands.size() == 2) {
        if (isConstInt(in.operands[0], ai) && isConstInt(in.operands[1], bi)) {
            out = Value::constInt(ai < bi ? ai : bi);
            return true;
        }
        return false;
    }
    if (c == "rt_max_i64" && in.operands.size() == 2) {
        if (isConstInt(in.operands[0], ai) && isConstInt(in.operands[1], bi)) {
            out = Value::constInt(ai > bi ? ai : bi);
            return true;
        }
        return false;
    }
    // Clamp
    if (c == "rt_clamp_f64" && in.operands.size() == 3) {
        double val, lo, hi;
        if (getConstFloat(in.operands[0], val) && getConstFloat(in.operands[1], lo) &&
            getConstFloat(in.operands[2], hi)) {
            if (lo > hi)
                std::swap(lo, hi);
            if (val < lo)
                out = Value::constFloat(lo);
            else if (val > hi)
                out = Value::constFloat(hi);
            else
                out = Value::constFloat(val);
            return true;
        }
        return false;
    }
    if (c == "rt_clamp_i64" && in.operands.size() == 3) {
        long long val, lo, hi;
        if (isConstInt(in.operands[0], val) && isConstInt(in.operands[1], lo) &&
            isConstInt(in.operands[2], hi)) {
            if (lo > hi)
                std::swap(lo, hi);
            if (val < lo)
                out = Value::constInt(lo);
            else if (val > hi)
                out = Value::constInt(hi);
            else
                out = Value::constInt(val);
            return true;
        }
        return false;
    }
    // Sign function
    if (c == "rt_sgn_i64" && in.operands.size() == 1) {
        if (isConstInt(in.operands[0], ai)) {
            out = Value::constInt(ai > 0 ? 1 : (ai < 0 ? -1 : 0));
            return true;
        }
        return false;
    }
    if (c == "rt_sgn_f64" && in.operands.size() == 1) {
        if (getConstFloat(in.operands[0], a)) {
            if (std::isnan(a)) {
                out = Value::constFloat(a);
                return true;
            }
            out = Value::constFloat(a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0));
            return true;
        }
        return false;
    }
    return false;
}

} // namespace

/// @brief Perform constant folding across a module in-place.
/// @details Visits every instruction, attempting to fold recognised runtime
///          calls, unary casts, and binary arithmetic operations.  Successful
///          folds update all uses of the temporary via UseDefInfo's safe
///          replacement API, then erase the original instruction, shrinking the
///          IR without altering observable behaviour.
/// @param m Module whose functions are to be folded.
void constFold(Module &m) {
    // Constant folding must be observationally equivalent to VM; where traps would
    // occur at runtime, folding must not change behavior.
    for (auto &f : m.functions) {
        // Build initial use-count info once per function.
        zanna::il::UseDefInfo useInfo(f);

        std::unordered_map<BasicBlock *, std::vector<size_t>> eraseByBlock;
        eraseByBlock.reserve(f.blocks.size());

        for (auto &b : f.blocks) {
            auto &toErase = eraseByBlock[&b];

            for (size_t i = 0; i < b.instructions.size(); ++i) {
                Instr &in = b.instructions[i];
                if (!in.result)
                    continue;
                Value repl;
                bool folded = false;
                if (in.op == Opcode::Call) {
                    folded = foldCall(in, repl);
                } else if (in.operands.size() == 1) {
                    if (in.op == Opcode::CastSiToFp || in.op == Opcode::Sitofp) {
                        long long operand;
                        if (isConstInt(in.operands[0], operand)) {
                            repl = Value::constFloat(static_cast<double>(operand));
                            folded = true;
                        }
                    } else if (in.op == Opcode::CastUiToFp) {
                        long long operand;
                        if (isConstInt(in.operands[0], operand)) {
                            repl = Value::constFloat(
                                static_cast<double>(static_cast<unsigned long long>(operand)));
                            folded = true;
                        }
                    } else if (in.op == Opcode::Fptosi) {
                        double operand;
                        if (getConstFloat(in.operands[0], operand) && std::isfinite(operand)) {
                            const double truncated = std::trunc(operand);
                            const double lower =
                                static_cast<double>(std::numeric_limits<long long>::min());
                            const double upperExclusive = 9223372036854775808.0;
                            if (truncated >= lower && truncated < upperExclusive) {
                                repl = Value::constInt(static_cast<long long>(truncated));
                                folded = true;
                            }
                        }
                    } else if (in.op == Opcode::Zext1) {
                        long long operand;
                        if (isConstInt(in.operands[0], operand)) {
                            repl = Value::constInt((operand & 1) != 0 ? 1 : 0);
                            folded = true;
                        }
                    } else if (in.op == Opcode::Trunc1) {
                        long long operand;
                        if (isConstInt(in.operands[0], operand)) {
                            repl = Value::constBool((operand & 1) != 0);
                            folded = true;
                        }
                    } else if (in.op == Opcode::CastFpToSiRteChk) {
                        double operand;
                        if (getConstFloat(in.operands[0], operand)) {
                            const auto result =
                                checkedFpToSiRte(operand, integerTypeBits(in.type.kind));
                            if (result.ok()) {
                                repl = Value::constInt(result.value);
                                folded = true;
                            }
                        }
                    } else if (in.op == Opcode::CastFpToUiRteChk) {
                        double operand;
                        if (getConstFloat(in.operands[0], operand)) {
                            const auto result =
                                checkedFpToUiRte(operand, integerTypeBits(in.type.kind));
                            if (result.ok()) {
                                repl = Value::constInt(result.value);
                                folded = true;
                            }
                        }
                    }
                } else if (in.operands.size() == 2) {
                    long long lhs, rhs, res = 0;
                    if (isConstInt(in.operands[0], lhs) && isConstInt(in.operands[1], rhs)) {
                        folded = true;
                        switch (in.op) {
                            case Opcode::Add:
                            case Opcode::IAddOvf:
                                if (const auto sum = checkedAdd(lhs, rhs)) {
                                    if (fitsSignedIntegerType(*sum, in.type.kind))
                                        res = *sum;
                                    else
                                        folded = false;
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::SDiv:
                            case Opcode::SDivChk0:
                                lhs = normalizeIntegerForType(lhs, in.type.kind);
                                rhs = normalizeIntegerForType(rhs, in.type.kind);
                                if (rhs != 0 &&
                                    !(lhs == signedMinForType(in.type.kind) && rhs == -1)) {
                                    res = normalizeIntegerForType(lhs / rhs, in.type.kind);
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::SRem:
                            case Opcode::SRemChk0:
                                lhs = normalizeIntegerForType(lhs, in.type.kind);
                                rhs = normalizeIntegerForType(rhs, in.type.kind);
                                if (rhs != 0 &&
                                    !(lhs == signedMinForType(in.type.kind) && rhs == -1)) {
                                    res = normalizeIntegerForType(lhs % rhs, in.type.kind);
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::Sub:
                            case Opcode::ISubOvf:
                                if (const auto diff = checkedSub(lhs, rhs)) {
                                    if (fitsSignedIntegerType(*diff, in.type.kind))
                                        res = *diff;
                                    else
                                        folded = false;
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::Mul:
                            case Opcode::IMulOvf:
                                if (const auto prod = checkedMul(lhs, rhs)) {
                                    if (fitsSignedIntegerType(*prod, in.type.kind))
                                        res = *prod;
                                    else
                                        folded = false;
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::And:
                                res = lhs & rhs;
                                break;
                            case Opcode::Or:
                                res = lhs | rhs;
                                break;
                            case Opcode::Xor:
                                res = lhs ^ rhs;
                                break;
                            // Shift operations
                            case Opcode::Shl:
                                lhs = normalizeIntegerForType(lhs, in.type.kind);
                                rhs = normalizeIntegerForType(rhs, in.type.kind);
                                res = static_cast<long long>(
                                    normalizeUnsignedForType(static_cast<unsigned long long>(lhs),
                                                             in.type.kind)
                                    << (static_cast<unsigned long long>(rhs) &
                                        static_cast<unsigned long long>(
                                            integerTypeBits(in.type.kind) - 1)));
                                break;
                            case Opcode::LShr:
                                lhs = normalizeIntegerForType(lhs, in.type.kind);
                                rhs = normalizeIntegerForType(rhs, in.type.kind);
                                res = static_cast<long long>(
                                    normalizeUnsignedForType(static_cast<unsigned long long>(lhs),
                                                             in.type.kind) >>
                                    (static_cast<unsigned long long>(rhs) &
                                     static_cast<unsigned long long>(integerTypeBits(in.type.kind) -
                                                                     1)));
                                break;
                            case Opcode::AShr:
                                lhs = normalizeIntegerForType(lhs, in.type.kind);
                                rhs = normalizeIntegerForType(rhs, in.type.kind);
                                res = arithmeticShiftRight(
                                    lhs,
                                    static_cast<unsigned>(static_cast<unsigned long long>(rhs) &
                                                          static_cast<unsigned long long>(
                                                              integerTypeBits(in.type.kind) - 1)));
                                break;
                            // Integer comparisons - produce boolean results
                            case Opcode::ICmpEq:
                                repl = Value::constBool(lhs == rhs);
                                break;
                            case Opcode::ICmpNe:
                                repl = Value::constBool(lhs != rhs);
                                break;
                            case Opcode::SCmpLT:
                                repl = Value::constBool(lhs < rhs);
                                break;
                            case Opcode::SCmpLE:
                                repl = Value::constBool(lhs <= rhs);
                                break;
                            case Opcode::SCmpGT:
                                repl = Value::constBool(lhs > rhs);
                                break;
                            case Opcode::SCmpGE:
                                repl = Value::constBool(lhs >= rhs);
                                break;
                            // Unsigned comparisons (treat as unsigned) - produce boolean results
                            case Opcode::UCmpLT:
                                repl = Value::constBool(static_cast<unsigned long long>(lhs) <
                                                        static_cast<unsigned long long>(rhs));
                                break;
                            case Opcode::UCmpLE:
                                repl = Value::constBool(static_cast<unsigned long long>(lhs) <=
                                                        static_cast<unsigned long long>(rhs));
                                break;
                            case Opcode::UCmpGT:
                                repl = Value::constBool(static_cast<unsigned long long>(lhs) >
                                                        static_cast<unsigned long long>(rhs));
                                break;
                            case Opcode::UCmpGE:
                                repl = Value::constBool(static_cast<unsigned long long>(lhs) >=
                                                        static_cast<unsigned long long>(rhs));
                                break;
                            // Unsigned division and remainder
                            case Opcode::UDiv:
                            case Opcode::UDivChk0:
                                if (normalizeUnsignedForType(static_cast<unsigned long long>(rhs),
                                                             in.type.kind) != 0) {
                                    const auto ulhs = normalizeUnsignedForType(
                                        static_cast<unsigned long long>(lhs), in.type.kind);
                                    const auto urhs = normalizeUnsignedForType(
                                        static_cast<unsigned long long>(rhs), in.type.kind);
                                    res = normalizeIntegerForType(
                                        static_cast<long long>(ulhs / urhs), in.type.kind);
                                } else {
                                    folded = false;
                                }
                                break;
                            case Opcode::URem:
                            case Opcode::URemChk0:
                                if (normalizeUnsignedForType(static_cast<unsigned long long>(rhs),
                                                             in.type.kind) != 0) {
                                    const auto ulhs = normalizeUnsignedForType(
                                        static_cast<unsigned long long>(lhs), in.type.kind);
                                    const auto urhs = normalizeUnsignedForType(
                                        static_cast<unsigned long long>(rhs), in.type.kind);
                                    res = normalizeIntegerForType(
                                        static_cast<long long>(ulhs % urhs), in.type.kind);
                                } else {
                                    folded = false;
                                }
                                break;
                            default:
                                folded = false;
                                break;
                        }
                        // For non-comparison operations, synthesize the integer result
                        // Comparison operations already set repl directly
                        if (folded && in.op != Opcode::ICmpEq && in.op != Opcode::ICmpNe &&
                            in.op != Opcode::SCmpLT && in.op != Opcode::SCmpLE &&
                            in.op != Opcode::SCmpGT && in.op != Opcode::SCmpGE &&
                            in.op != Opcode::UCmpLT && in.op != Opcode::UCmpLE &&
                            in.op != Opcode::UCmpGT && in.op != Opcode::UCmpGE) {
                            repl = Value::constInt(normalizeIntegerForType(res, in.type.kind));
                        }
                    }
                    // Try floating-point folding
                    if (!folded) {
                        double flhs = 0.0;
                        double frhs = 0.0;
                        if (getConstFloat(in.operands[0], flhs) &&
                            getConstFloat(in.operands[1], frhs)) {
                            double fres = 0.0;
                            folded = true;
                            switch (in.op) {
                                case Opcode::FAdd:
                                    fres = flhs + frhs;
                                    break;
                                case Opcode::FSub:
                                    fres = flhs - frhs;
                                    break;
                                case Opcode::FMul:
                                    fres = flhs * frhs;
                                    break;
                                case Opcode::FDiv:
                                    fres = flhs / frhs;
                                    break;
                                // Float comparisons produce boolean (int) results
                                case Opcode::FCmpEQ:
                                    repl = Value::constBool(flhs == frhs);
                                    break;
                                case Opcode::FCmpNE:
                                    repl = Value::constBool(flhs != frhs);
                                    break;
                                case Opcode::FCmpLT:
                                    repl = Value::constBool(flhs < frhs);
                                    break;
                                case Opcode::FCmpLE:
                                    repl = Value::constBool(flhs <= frhs);
                                    break;
                                case Opcode::FCmpGT:
                                    repl = Value::constBool(flhs > frhs);
                                    break;
                                case Opcode::FCmpGE:
                                    repl = Value::constBool(flhs >= frhs);
                                    break;
                                case Opcode::FCmpOrd:
                                    repl = Value::constBool(!std::isnan(flhs) && !std::isnan(frhs));
                                    break;
                                case Opcode::FCmpUno:
                                    repl = Value::constBool(std::isnan(flhs) || std::isnan(frhs));
                                    break;
                                default:
                                    folded = false;
                                    break;
                            }
                            // For arithmetic operations, produce float result
                            if (folded && in.op != Opcode::FCmpEQ && in.op != Opcode::FCmpNE &&
                                in.op != Opcode::FCmpLT && in.op != Opcode::FCmpLE &&
                                in.op != Opcode::FCmpGT && in.op != Opcode::FCmpGE &&
                                in.op != Opcode::FCmpOrd && in.op != Opcode::FCmpUno) {
                                repl = Value::constFloat(fres);
                            }
                        }
                    }
                }
                if (folded) {
                    useInfo.replaceAllUsesStableStorage(*in.result, repl);
                    toErase.push_back(i);
                }
            }
        }

        // Replacements above use cached operand locations, so instruction
        // storage must remain fixed until every fold has been propagated.
        // Compact each block once after that stable-storage phase.
        for (auto &[block, indices] : eraseByBlock) {
            if (indices.empty())
                continue;
            std::vector<bool> remove(block->instructions.size(), false);
            for (size_t index : indices)
                remove[index] = true;
            std::vector<Instr> compacted;
            compacted.reserve(block->instructions.size() - indices.size());
            for (size_t index = 0; index < block->instructions.size(); ++index) {
                if (!remove[index])
                    compacted.push_back(std::move(block->instructions[index]));
            }
            block->instructions = std::move(compacted);
        }
    }
    m.internOwnedIdentifiers();
}

} // namespace il::transform
