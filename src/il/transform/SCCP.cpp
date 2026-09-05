//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/SCCP.cpp
// Purpose: Propagate constants along executable IL edges and fold known branches.
// Key invariants:
//   - Block parameters merge monotonically over executable incoming edges.
//   - A provisional arithmetic trap must be reconsidered as its operands change.
// Ownership/Lifetime: Solver borrows one function; instruction pointers remain
// stable until solving and constant substitution finish, before CFG cleanup.
// Links: SCCP.hpp, docs/il/il-guide.md#reference, test_SCCP.cpp.
//
//===----------------------------------------------------------------------===//
//
// File Structure:
// ---------------
// This file is organized into the following sections:
//
// 1. Lattice and Value Utilities
//    - ValueLattice struct and value comparison helpers
//    - Constant extraction helpers (getConstInt, getConstFloat, etc.)
//    - Overflow-checked arithmetic helpers
//
// 2. Constant Folding by Opcode Family
//    - foldIntegerArithmetic: Add, Sub, Mul, And, Or, Xor, Shl, LShr, AShr
//    - foldOverflowArithmetic: IAddOvf, ISubOvf, IMulOvf
//    - foldDivisionRemainder: SDivChk0, SRemChk0, UDivChk0, URemChk0
//    - foldFloatArithmetic: FAdd, FSub, FMul, FDiv
//    - foldIntegerComparisons: ICmpEq, ICmpNe, SCmpLT, SCmpLE, SCmpGT, SCmpGE
//    - foldUnsignedComparisons: UCmpLT, UCmpLE, UCmpGT, UCmpGE
//    - foldFloatComparisons: FCmpEQ, FCmpNE, FCmpLT, FCmpLE, FCmpGT, FCmpGE
//    - foldTypeConversions: CastSiToFp, CastUiToFp, CastFpToSiRteChk, etc.
//    - foldBooleanOps: Zext1, Trunc1
//    - foldConstantMaterialization: ConstNull, ConstStr, AddrOf
//
// 3. SCCPSolver Class
//    - Lattice state management
//    - Worklist processing
//    - Terminator handling (CBr, SwitchI32)
//    - Rewriting phase
//
// 4. Public API
//    - sccp(Module&) entry point
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Sparse conditional constant propagation for IL functions.
/// @details Provides a lattice-based solver that runs per function.  The solver
///          propagates constants only along executable edges, merges block
///          parameter values using the classic three-point lattice, and rewrites
///          instructions and terminators once fixed points are reached.

#include "il/transform/SCCP.hpp"

#include "il/core/FPCast.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include "il/transform/OverflowArithmetic.hpp"
#include "il/transform/SimplifyCFG.hpp"
#include "il/transform/SimplifyCFG/ReachabilityCleanup.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>

#include <iostream>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

//===----------------------------------------------------------------------===//
// Section 1: Lattice and Value Utilities
//===----------------------------------------------------------------------===//

/// @brief Compare two IL constants for lattice merging.
/// @param lhs First value.
/// @param rhs Second value.
/// @return `true` when their kind and payload are semantically equal.
bool valuesEqual(const Value &lhs, const Value &rhs);

/// @brief Three-point lattice for SCCP analysis.
/// @details Unknown < Constant < Overdefined.  Trap-like instructions are
///          modelled separately during folding so we never turn a known trap
///          into an executable edge.
struct ValueLattice {
    /// @brief Information states ordered from unknown through constant to overdefined.
    enum class Kind { Unknown, Constant, Overdefined };

    /// @brief Construct the top (⊤) lattice element — value not yet seen.
    /// @return Lattice state containing no value information.
    static ValueLattice unknown() {
        return ValueLattice{Kind::Unknown, {}};
    }

    /// @brief Construct a constant lattice element carrying the given value.
    /// @param v Constant moved into the lattice payload.
    /// @return Lattice state representing exactly @p v.
    static ValueLattice fromConstant(Value v) {
        return ValueLattice{Kind::Constant, v};
    }

    /// @brief Construct the bottom (⊥) lattice element — value not constant.
    /// @return Lattice state representing a varying or externally defined value.
    static ValueLattice overdefined() {
        return ValueLattice{Kind::Overdefined, {}};
    }

    /// @brief Return @c true when the element is ⊤ (no information yet).
    /// @return Whether @ref kind is @ref Kind::Unknown.
    bool isUnknown() const {
        return kind == Kind::Unknown;
    }

    /// @brief Return @c true when the element holds a specific constant.
    /// @return Whether @ref value is a valid constant payload.
    bool isConstant() const {
        return kind == Kind::Constant;
    }

    /// @brief Return @c true when the element is ⊥ (value cannot be constant).
    /// @return Whether the state is permanently overdefined.
    bool isOverdefined() const {
        return kind == Kind::Overdefined;
    }

    /// @brief Merge a constant into the lattice state.
    /// @param v Incoming constant from an executable definition or edge.
    /// @return True if the state changed.
    bool mergeConstant(const Value &v) {
        if (kind == Kind::Unknown) {
            kind = Kind::Constant;
            value = v;
            return true;
        }
        if (kind == Kind::Constant && !valuesEqual(value, v)) {
            kind = Kind::Overdefined;
            value = {};
            return true;
        }
        return false;
    }

    /// @brief Raise the lattice element to Overdefined.
    /// @return True if the state changed.
    bool markOverdefined() {
        if (kind == Kind::Overdefined)
            return false;
        kind = Kind::Overdefined;
        value = {};
        return true;
    }

    /// Current lattice discriminator.
    Kind kind = Kind::Unknown;
    /// Constant payload, meaningful only for @ref Kind::Constant.
    Value value{};
};

/// @brief Folding outcome classification used during evaluation.
struct FoldResult {
    /// @brief Outcomes distinguish insufficient information, a constant, and a known trap.
    enum class Kind { Unknown, Constant, Trap };

    /// @brief Construct an outcome with insufficient information to fold.
    /// @return Unknown folding result.
    static FoldResult unknown() {
        return FoldResult{Kind::Unknown, {}};
    }

    /// @brief Construct an outcome proving that evaluating the instruction traps.
    /// @return Trap folding result with no value payload.
    static FoldResult trap() {
        return FoldResult{Kind::Trap, {}};
    }

    /// @brief Construct a successful constant-folding outcome.
    /// @param v Constant moved into the result.
    /// @return Constant folding result.
    static FoldResult constant(Value v) {
        return FoldResult{Kind::Constant, v};
    }

    /// @brief Test whether evaluation is proven to trap.
    /// @return Whether @ref kind is @ref Kind::Trap.
    bool isTrap() const {
        return kind == Kind::Trap;
    }

    /// @brief Test whether folding produced a constant.
    /// @return Whether @ref value contains the replacement constant.
    bool isConstant() const {
        return kind == Kind::Constant;
    }

    /// Outcome discriminator.
    Kind kind{Kind::Unknown};
    /// Constant payload when @ref kind is @ref Kind::Constant.
    Value value;
};

/// @brief Compare two IL values for equality.
/// @param lhs First value.
/// @param rhs Second value.
/// @return `true` when kind and payload are semantically equal.
bool valuesEqual(const Value &lhs, const Value &rhs) {
    return valueEquals(lhs, rhs);
}

/// @brief Produce a human-readable description of an IL value for debug output.
/// @param value Value to render.
/// @return Compact textual representation used only by tracing.
std::string describeValue(const Value &value) {
    std::ostringstream oss;
    switch (value.kind) {
        case Value::Kind::ConstInt:
            oss << value.i64;
            if (value.isBool)
                oss << " (bool)";
            break;
        case Value::Kind::ConstFloat:
            oss << value.f64;
            break;
        case Value::Kind::ConstStr:
            oss << "str(" << value.str << ")";
            break;
        case Value::Kind::GlobalAddr:
            oss << "addr(" << value.str << ")";
            break;
        case Value::Kind::NullPtr:
            oss << "null";
            break;
        case Value::Kind::Temp:
            oss << "%" << value.id;
            break;
    }
    return oss.str();
}

//===----------------------------------------------------------------------===//
// Constant Extraction Helpers
//===----------------------------------------------------------------------===//

/// @brief Extract a signed integer constant from a value.
/// @param value Candidate IL value.
/// @param out Receives the signed payload on success.
/// @return `true` when @p value is an integer constant.
bool getConstInt(const Value &value, long long &out) {
    if (value.kind != Value::Kind::ConstInt)
        return false;
    out = value.i64;
    return true;
}

/// @brief Extract an unsigned integer constant from a value.
/// @param value Candidate IL integer constant.
/// @param out Receives the same bits interpreted as unsigned.
/// @return `true` when @p value is an integer constant.
bool getConstUInt(const Value &value, unsigned long long &out) {
    if (value.kind != Value::Kind::ConstInt)
        return false;
    out = static_cast<unsigned long long>(value.i64);
    return true;
}

/// @brief Extract a floating-point constant from a value.
/// @details Also handles ConstInt by converting to double.
/// @param value Candidate numeric literal.
/// @param out Receives its double representation.
/// @return `true` for integer or floating-point constants.
bool getConstFloat(const Value &value, double &out) {
    if (value.kind == Value::Kind::ConstFloat) {
        out = value.f64;
        return true;
    }
    if (value.kind == Value::Kind::ConstInt) {
        out = static_cast<double>(value.i64);
        return true;
    }
    return false;
}

/// @brief Extract a branch boolean from a constant value.
/// @details Conditional branches are verified as i1, so SCCP only treats
///          boolean-tagged integer literals as compile-time branch conditions.
/// @param value Candidate boolean literal.
/// @param out Receives its truth value.
/// @return `true` only for an integer constant tagged as boolean.
bool getConstBool(const Value &value, bool &out) {
    if (value.kind != Value::Kind::ConstInt || !value.isBool)
        return false;
    out = value.i64 != 0;
    return true;
}

//===----------------------------------------------------------------------===//
// Overflow-Checked Arithmetic Helpers
//===----------------------------------------------------------------------===//

/// @brief Checked signed addition that returns nullopt on overflow.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return Mathematical sum when representable, otherwise `std::nullopt`.
std::optional<long long> checkedAdd(long long lhs, long long rhs) {
    long long result{};
    if (detail::addOverflows(lhs, rhs, result))
        return std::nullopt;
    return result;
}

/// @brief Checked signed subtraction that returns nullopt on overflow.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return Mathematical difference when representable, otherwise `std::nullopt`.
std::optional<long long> checkedSub(long long lhs, long long rhs) {
    long long result{};
    if (detail::subOverflows(lhs, rhs, result))
        return std::nullopt;
    return result;
}

/// @brief Checked signed multiplication that returns nullopt on overflow.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return Mathematical product when representable, otherwise `std::nullopt`.
std::optional<long long> checkedMul(long long lhs, long long rhs) {
    long long result{};
    if (detail::mulOverflows(lhs, rhs, result))
        return std::nullopt;
    return result;
}

/// @brief Compute deterministic two's-complement wrapping addition.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return Low `long long` bits of the mathematical sum.
long long wrappingAdd(long long lhs, long long rhs) {
    long long result{};
    (void)detail::addOverflows(lhs, rhs, result);
    return result;
}

/// @brief Compute deterministic two's-complement wrapping subtraction.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return Low `long long` bits of the mathematical difference.
long long wrappingSub(long long lhs, long long rhs) {
    long long result{};
    (void)detail::subOverflows(lhs, rhs, result);
    return result;
}

/// @brief Compute deterministic two's-complement wrapping multiplication.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return Low `long long` bits of the mathematical product.
long long wrappingMul(long long lhs, long long rhs) {
    long long result{};
    (void)detail::mulOverflows(lhs, rhs, result);
    return result;
}

/// @brief Perform a portable sign-extending right shift.
/// @param value Signed bit pattern to shift.
/// @param shift Shift amount in the range zero through 63.
/// @return Shifted value with high bits filled from the original sign.
long long arithmeticShiftRight(long long value, unsigned shift) {
    if (shift == 0)
        return value;

    const auto bits = static_cast<unsigned long long>(value);
    auto shifted = bits >> shift;
    if ((bits & (1ULL << 63U)) != 0)
        shifted |= (~0ULL) << (64U - shift);
    return static_cast<long long>(shifted);
}

/// @brief Return the integer bit width encoded by an IL integer type.
/// @details Non-integer callers conservatively receive 64 bits, matching the
///          historical SCCP folding behavior for opcodes without a narrower
///          result type.
/// @param kind IL result type.
/// @return 16, 32, or 64 bits; unsupported kinds conservatively map to 64.
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

/// @brief Truncate an unsigned integer constant to the IL result width.
/// @param value Full-width unsigned value produced by a fold.
/// @param kind Result type that determines the target width.
/// @return Unsigned value with all bits above the target width cleared.
static unsigned long long normalizeUnsignedForType(unsigned long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return value;
    const auto mask = (1ULL << bits) - 1ULL;
    return value & mask;
}

/// @brief Truncate and sign-extend an integer constant to the IL result width.
/// @param value Full-width signed value produced by a fold.
/// @param kind Result type that determines truncation and sign extension.
/// @return A canonical signed integer payload for @p kind.
static long long normalizeIntegerForType(long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return value;

    const auto mask = (1ULL << bits) - 1ULL;
    auto truncated = static_cast<unsigned long long>(value) & mask;
    const auto signBit = 1ULL << (bits - 1);
    if ((truncated & signBit) != 0)
        truncated |= ~mask;
    return static_cast<long long>(truncated);
}

/// @brief Return the minimum signed integer representable by an IL integer type.
/// @param kind Integer type whose lower bound is requested.
/// @return Minimum signed value at that width.
static long long signedMinForType(Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return std::numeric_limits<long long>::min();
    return -(1LL << (bits - 1));
}

/// @brief Return true if @p value fits in the signed IL integer type @p kind.
/// @param value Candidate signed constant.
/// @param kind Destination integer type.
/// @return Whether @p value lies within the type's signed range.
static bool fitsSignedIntegerType(long long value, Type::Kind kind) {
    const int bits = integerTypeBits(kind);
    if (bits >= 64)
        return true;
    const long long min = -(1LL << (bits - 1));
    const long long max = (1LL << (bits - 1)) - 1LL;
    return value >= min && value <= max;
}

//===----------------------------------------------------------------------===//
// Section 2: Constant Folding by Opcode Family
//===----------------------------------------------------------------------===//
//
// Each fold function takes the instruction and a resolver for operand values,
// returning an optional constant if the operation can be folded.
//

/// @brief Context for resolving instruction operands during folding.
struct FoldContext {
    /// @brief Create a folding context for one instruction.
    /// @details The context borrows the instruction being evaluated and owns
    ///          the resolver callable used to translate operand indices into
    ///          lattice values. The instruction reference must outlive the
    ///          folding operation.
    /// @param source Instruction currently being folded.
    /// @param resolver Callable that resolves operand indices into values.
    FoldContext(const Instr &source, std::function<bool(size_t, Value &)> resolver)
        : instr(source), resolveOperand(std::move(resolver)) {}

    /// Borrowed instruction being evaluated.
    const Instr &instr;
    /// Resolver returning a known constant for an operand index.
    std::function<bool(size_t, Value &)> resolveOperand;

    /// @brief Resolve operand @p index and extract its signed integer value.
    /// @param index Operand position in @ref instr.
    /// @param out Receives the signed payload.
    /// @return @c true when the operand is a known integer constant.
    bool getConstIntOperand(size_t index, long long &out) const {
        if (index >= instr.operands.size())
            return false;
        Value resolved;
        if (!resolveOperand(index, resolved))
            return false;
        return getConstInt(resolved, out);
    }

    /// @brief Resolve operand @p index and extract its unsigned integer value.
    /// @param index Operand position in @ref instr.
    /// @param out Receives the unsigned interpretation of its bits.
    /// @return @c true when the operand is a known integer constant.
    bool getConstUIntOperand(size_t index, unsigned long long &out) const {
        if (index >= instr.operands.size())
            return false;
        Value resolved;
        if (!resolveOperand(index, resolved))
            return false;
        return getConstUInt(resolved, out);
    }

    /// @brief Resolve operand @p index and extract its floating-point value.
    /// @details Integer constants are widened to double.
    /// @param index Operand position in @ref instr.
    /// @param out Receives the numeric value as double.
    /// @return @c true when the operand has a known numeric value.
    bool getConstFloatOperand(size_t index, double &out) const {
        if (index >= instr.operands.size())
            return false;
        Value resolved;
        if (!resolveOperand(index, resolved))
            return false;
        return getConstFloat(resolved, out);
    }
};

//===----------------------------------------------------------------------===//
// Integer Arithmetic: Add, Sub, Mul, And, Or, Xor, Shl, LShr, AShr
//===----------------------------------------------------------------------===//

/// @brief Fold basic integer arithmetic operations.
/// @details Handles non-overflow-checked integer operations.
/// @param op Arithmetic, bitwise, shift, divide, or remainder opcode.
/// @param ctx Operand resolver and result-type context.
/// @return Constant, trap, or unknown folding outcome.
static FoldResult foldIntegerArithmetic(Opcode op, const FoldContext &ctx) {
    long long lhs{}, rhs{};
    if (!ctx.getConstIntOperand(0, lhs) || !ctx.getConstIntOperand(1, rhs))
        return FoldResult::unknown();

    const Type::Kind resultKind = ctx.instr.type.kind;
    /// Normalize a folded payload to the instruction's declared integer width.
    const auto foldedInt = [resultKind](long long value) {
        return FoldResult::constant(Value::constInt(normalizeIntegerForType(value, resultKind)));
    };
    const int bits = integerTypeBits(resultKind);
    const auto shiftMask = static_cast<unsigned long long>(bits - 1);
    lhs = normalizeIntegerForType(lhs, resultKind);
    rhs = normalizeIntegerForType(rhs, resultKind);

    switch (op) {
        case Opcode::Add:
            return foldedInt(wrappingAdd(lhs, rhs));
        case Opcode::Sub:
            return foldedInt(wrappingSub(lhs, rhs));
        case Opcode::Mul:
            return foldedInt(wrappingMul(lhs, rhs));
        case Opcode::And:
            return foldedInt(lhs & rhs);
        case Opcode::Or:
            return foldedInt(lhs | rhs);
        case Opcode::Xor:
            return foldedInt(lhs ^ rhs);
        case Opcode::SDiv:
            if (rhs == 0 || (lhs == signedMinForType(resultKind) && rhs == -1))
                return FoldResult::trap();
            return foldedInt(lhs / rhs);
        case Opcode::UDiv: {
            auto ulhs = normalizeUnsignedForType(static_cast<unsigned long long>(lhs), resultKind);
            auto urhs = normalizeUnsignedForType(static_cast<unsigned long long>(rhs), resultKind);
            if (urhs == 0)
                return FoldResult::trap();
            return foldedInt(static_cast<long long>(ulhs / urhs));
        }
        case Opcode::SRem:
            if (rhs == 0 || (lhs == signedMinForType(resultKind) && rhs == -1))
                return FoldResult::trap();
            return foldedInt(lhs % rhs);
        case Opcode::URem: {
            auto ulhs = normalizeUnsignedForType(static_cast<unsigned long long>(lhs), resultKind);
            auto urhs = normalizeUnsignedForType(static_cast<unsigned long long>(rhs), resultKind);
            if (urhs == 0)
                return FoldResult::trap();
            return foldedInt(static_cast<long long>(ulhs % urhs));
        }
        case Opcode::Shl:
            return foldedInt(static_cast<long long>(
                normalizeUnsignedForType(static_cast<unsigned long long>(lhs), resultKind)
                << (static_cast<unsigned long long>(rhs) & shiftMask)));
        case Opcode::LShr:
            return foldedInt(static_cast<long long>(
                normalizeUnsignedForType(static_cast<unsigned long long>(lhs), resultKind) >>
                (static_cast<unsigned long long>(rhs) & shiftMask)));
        case Opcode::AShr:
            return foldedInt(arithmeticShiftRight(
                lhs, static_cast<unsigned>(static_cast<unsigned long long>(rhs) & shiftMask)));
        default:
            return FoldResult::unknown();
    }
}

//===----------------------------------------------------------------------===//
// Overflow-Checked Arithmetic: IAddOvf, ISubOvf, IMulOvf
//===----------------------------------------------------------------------===//

/// @brief Fold overflow-checked arithmetic operations.
/// @details Returns nullopt if the operation would overflow at runtime.
/// @param op Checked addition, subtraction, or multiplication opcode.
/// @param ctx Operand resolver and result-type context.
/// @return Constant when representable, trap on overflow, otherwise unknown.
static FoldResult foldOverflowArithmetic(Opcode op, const FoldContext &ctx) {
    long long lhs{}, rhs{};
    if (!ctx.getConstIntOperand(0, lhs) || !ctx.getConstIntOperand(1, rhs))
        return FoldResult::unknown();

    lhs = normalizeIntegerForType(lhs, ctx.instr.type.kind);
    rhs = normalizeIntegerForType(rhs, ctx.instr.type.kind);
    /// Normalize a successful checked result to the instruction's integer width.
    const auto foldedInt = [&ctx](long long value) {
        return FoldResult::constant(
            Value::constInt(normalizeIntegerForType(value, ctx.instr.type.kind)));
    };

    switch (op) {
        case Opcode::IAddOvf:
            if (auto sum = checkedAdd(lhs, rhs)) {
                if (!fitsSignedIntegerType(*sum, ctx.instr.type.kind))
                    return FoldResult::trap();
                return foldedInt(*sum);
            }
            return FoldResult::trap();
        case Opcode::ISubOvf:
            if (auto diff = checkedSub(lhs, rhs)) {
                if (!fitsSignedIntegerType(*diff, ctx.instr.type.kind))
                    return FoldResult::trap();
                return foldedInt(*diff);
            }
            return FoldResult::trap();
        case Opcode::IMulOvf:
            if (auto prod = checkedMul(lhs, rhs)) {
                if (!fitsSignedIntegerType(*prod, ctx.instr.type.kind))
                    return FoldResult::trap();
                return foldedInt(*prod);
            }
            return FoldResult::trap();
        default:
            break;
    }
    return FoldResult::unknown();
}

//===----------------------------------------------------------------------===//
// Division and Remainder: SDivChk0, SRemChk0, UDivChk0, URemChk0
//===----------------------------------------------------------------------===//

/// @brief Fold signed division/remainder with zero-check.
/// @details Returns nullopt for divide-by-zero or MIN/-1 overflow.
/// @param op Checked signed division or remainder opcode.
/// @param ctx Operand resolver and integer result type.
/// @return Constant quotient/remainder, trap for invalid arithmetic, or unknown.
static FoldResult foldSignedDivRem(Opcode op, const FoldContext &ctx) {
    long long lhs{}, rhs{};
    if (!ctx.getConstIntOperand(0, lhs) || !ctx.getConstIntOperand(1, rhs))
        return FoldResult::unknown();

    lhs = normalizeIntegerForType(lhs, ctx.instr.type.kind);
    rhs = normalizeIntegerForType(rhs, ctx.instr.type.kind);

    // Check for divide-by-zero and signed overflow (MIN / -1)
    if (rhs == 0 || (lhs == signedMinForType(ctx.instr.type.kind) && rhs == -1))
        return FoldResult::trap();

    if (op == Opcode::SDivChk0)
        return FoldResult::constant(
            Value::constInt(normalizeIntegerForType(lhs / rhs, ctx.instr.type.kind)));
    return FoldResult::constant(
        Value::constInt(normalizeIntegerForType(lhs % rhs, ctx.instr.type.kind)));
}

/// @brief Fold unsigned division/remainder with zero-check.
/// @param op Checked unsigned division or remainder opcode.
/// @param ctx Operand resolver and integer result type.
/// @return Constant quotient/remainder, trap for zero divisor, or unknown.
static FoldResult foldUnsignedDivRem(Opcode op, const FoldContext &ctx) {
    unsigned long long lhs{}, rhs{};
    if (!ctx.getConstUIntOperand(0, lhs) || !ctx.getConstUIntOperand(1, rhs))
        return FoldResult::unknown();

    lhs = normalizeUnsignedForType(lhs, ctx.instr.type.kind);
    rhs = normalizeUnsignedForType(rhs, ctx.instr.type.kind);

    if (rhs == 0)
        return FoldResult::trap();

    if (op == Opcode::UDivChk0)
        return FoldResult::constant(Value::constInt(
            normalizeIntegerForType(static_cast<long long>(lhs / rhs), ctx.instr.type.kind)));
    return FoldResult::constant(Value::constInt(
        normalizeIntegerForType(static_cast<long long>(lhs % rhs), ctx.instr.type.kind)));
}

//===----------------------------------------------------------------------===//
// Floating-Point Arithmetic: FAdd, FSub, FMul, FDiv
//===----------------------------------------------------------------------===//

/// @brief Fold floating-point arithmetic operations.
/// @param op Floating add, subtract, multiply, or divide opcode.
/// @param ctx Operand resolver for numeric constants.
/// @return IEEE arithmetic result as a constant, or unknown.
static FoldResult foldFloatArithmetic(Opcode op, const FoldContext &ctx) {
    double lhs{}, rhs{};
    if (!ctx.getConstFloatOperand(0, lhs) || !ctx.getConstFloatOperand(1, rhs))
        return FoldResult::unknown();

    double result{};
    switch (op) {
        case Opcode::FAdd:
            result = lhs + rhs;
            break;
        case Opcode::FSub:
            result = lhs - rhs;
            break;
        case Opcode::FMul:
            result = lhs * rhs;
            break;
        case Opcode::FDiv:
            result = lhs / rhs;
            break;
        default:
            return FoldResult::unknown();
    }
    return FoldResult::constant(Value::constFloat(result));
}

//===----------------------------------------------------------------------===//
// Integer Comparisons: ICmpEq, ICmpNe, SCmpLT, SCmpLE, SCmpGT, SCmpGE
//===----------------------------------------------------------------------===//

/// @brief Fold signed integer comparison operations.
/// @param op Equality or signed relational comparison.
/// @param ctx Operand resolver for integer constants.
/// @return Boolean constant for supported comparisons, otherwise unknown.
static FoldResult foldIntegerComparisons(Opcode op, const FoldContext &ctx) {
    long long lhs{}, rhs{};
    if (!ctx.getConstIntOperand(0, lhs) || !ctx.getConstIntOperand(1, rhs))
        return FoldResult::unknown();

    bool result = false;
    switch (op) {
        case Opcode::ICmpEq:
            result = lhs == rhs;
            break;
        case Opcode::ICmpNe:
            result = lhs != rhs;
            break;
        case Opcode::SCmpLT:
            result = lhs < rhs;
            break;
        case Opcode::SCmpLE:
            result = lhs <= rhs;
            break;
        case Opcode::SCmpGT:
            result = lhs > rhs;
            break;
        case Opcode::SCmpGE:
            result = lhs >= rhs;
            break;
        default:
            return FoldResult::unknown();
    }
    return FoldResult::constant(Value::constBool(result));
}

//===----------------------------------------------------------------------===//
// Unsigned Comparisons: UCmpLT, UCmpLE, UCmpGT, UCmpGE
//===----------------------------------------------------------------------===//

/// @brief Fold unsigned integer comparison operations.
/// @param op Unsigned relational comparison opcode.
/// @param ctx Operand resolver for integer constants.
/// @return Boolean constant for supported comparisons, otherwise unknown.
static FoldResult foldUnsignedComparisons(Opcode op, const FoldContext &ctx) {
    unsigned long long lhs{}, rhs{};
    if (!ctx.getConstUIntOperand(0, lhs) || !ctx.getConstUIntOperand(1, rhs))
        return FoldResult::unknown();

    bool result = false;
    switch (op) {
        case Opcode::UCmpLT:
            result = lhs < rhs;
            break;
        case Opcode::UCmpLE:
            result = lhs <= rhs;
            break;
        case Opcode::UCmpGT:
            result = lhs > rhs;
            break;
        case Opcode::UCmpGE:
            result = lhs >= rhs;
            break;
        default:
            return FoldResult::unknown();
    }
    return FoldResult::constant(Value::constBool(result));
}

//===----------------------------------------------------------------------===//
// Floating-Point Comparisons: FCmpEQ, FCmpNE, FCmpLT, FCmpLE, FCmpGT, FCmpGE
//===----------------------------------------------------------------------===//

/// @brief Fold floating-point comparison operations.
/// @param op Ordered floating equality or relational comparison.
/// @param ctx Operand resolver for numeric constants.
/// @return Boolean constant under host IEEE semantics, otherwise unknown.
static FoldResult foldFloatComparisons(Opcode op, const FoldContext &ctx) {
    double lhs{}, rhs{};
    if (!ctx.getConstFloatOperand(0, lhs) || !ctx.getConstFloatOperand(1, rhs))
        return FoldResult::unknown();

    bool result = false;
    switch (op) {
        case Opcode::FCmpEQ:
            result = lhs == rhs;
            break;
        case Opcode::FCmpNE:
            result = lhs != rhs;
            break;
        case Opcode::FCmpLT:
            result = lhs < rhs;
            break;
        case Opcode::FCmpLE:
            result = lhs <= rhs;
            break;
        case Opcode::FCmpGT:
            result = lhs > rhs;
            break;
        case Opcode::FCmpGE:
            result = lhs >= rhs;
            break;
        default:
            return FoldResult::unknown();
    }
    return FoldResult::constant(Value::constBool(result));
}

//===----------------------------------------------------------------------===//
// Type Conversions: CastSiToFp, CastUiToFp, CastFpToSiRteChk, CastFpToUiRteChk
//===----------------------------------------------------------------------===//

/// @brief Fold signed integer to floating-point conversion.
/// @param ctx Operand resolver containing the source instruction.
/// @return Floating constant when the operand is known, otherwise unknown.
static FoldResult foldCastSiToFp(const FoldContext &ctx) {
    long long operand{};
    if (!ctx.getConstIntOperand(0, operand))
        return FoldResult::unknown();
    return FoldResult::constant(Value::constFloat(static_cast<double>(operand)));
}

/// @brief Fold unsigned integer to floating-point conversion.
/// @param ctx Operand resolver containing the source instruction.
/// @return Floating constant when the operand is known, otherwise unknown.
static FoldResult foldCastUiToFp(const FoldContext &ctx) {
    unsigned long long operand{};
    if (!ctx.getConstUIntOperand(0, operand))
        return FoldResult::unknown();
    return FoldResult::constant(Value::constFloat(static_cast<double>(operand)));
}

/// @brief Fold floating-point to signed integer conversion with range check.
/// @param ctx Operand resolver and destination integer type.
/// @return Rounded constant, trap on overflow, or unknown for invalid/nonconstant input.
static FoldResult foldCastFpToSi(const FoldContext &ctx) {
    double operand{};
    if (!ctx.getConstFloatOperand(0, operand))
        return FoldResult::unknown();

    const auto result = checkedFpToSiRte(operand, integerTypeBits(ctx.instr.type.kind));
    if (result.failure == CheckedFPCastFailure::Invalid)
        return FoldResult::unknown();
    if (result.failure == CheckedFPCastFailure::Overflow)
        return FoldResult::trap();

    return FoldResult::constant(Value::constInt(result.value));
}

/// @brief Fold floating-point to unsigned integer conversion with range check.
/// @param ctx Operand resolver and destination integer type.
/// @return Rounded constant, trap on overflow, or unknown for invalid/nonconstant input.
static FoldResult foldCastFpToUi(const FoldContext &ctx) {
    double operand{};
    if (!ctx.getConstFloatOperand(0, operand))
        return FoldResult::unknown();

    const auto result = checkedFpToUiRte(operand, integerTypeBits(ctx.instr.type.kind));
    if (result.failure == CheckedFPCastFailure::Invalid)
        return FoldResult::unknown();
    if (result.failure == CheckedFPCastFailure::Overflow)
        return FoldResult::trap();

    return FoldResult::constant(Value::constInt(result.value));
}

//===----------------------------------------------------------------------===//
// Legacy Conversions: Sitofp, Fptosi
//===----------------------------------------------------------------------===//

/// @brief Fold legacy signed integer to float (truncation, not round-to-even).
/// @param ctx Operand resolver for the source floating value.
/// @return Truncated integer constant, trap when out of range, or unknown.
static FoldResult foldFptosi(const FoldContext &ctx) {
    double operand{};
    if (!ctx.getConstFloatOperand(0, operand) || !std::isfinite(operand))
        return FoldResult::unknown();

    // Truncation semantics (towards zero), unlike CastFpToSiRteChk which rounds.
    double truncated = std::trunc(operand);
    if (!std::isfinite(truncated))
        return FoldResult::trap();

    constexpr double kMin = static_cast<double>(std::numeric_limits<long long>::min());
    constexpr double kMax = static_cast<double>(std::numeric_limits<long long>::max());
    if (truncated < kMin || truncated > kMax)
        return FoldResult::trap();

    return FoldResult::constant(Value::constInt(static_cast<long long>(truncated)));
}

//===----------------------------------------------------------------------===//
// Ordered/Unordered Float Comparisons: FCmpOrd, FCmpUno
//===----------------------------------------------------------------------===//

/// @brief Fold ordered/unordered float comparisons.
/// @param op `fcmp.ord` or `fcmp.uno`.
/// @param ctx Operand resolver for both floating inputs.
/// @return Boolean constant describing the presence or absence of NaNs.
static FoldResult foldFCmpOrdUno(Opcode op, const FoldContext &ctx) {
    double lhs{}, rhs{};
    if (!ctx.getConstFloatOperand(0, lhs) || !ctx.getConstFloatOperand(1, rhs))
        return FoldResult::unknown();

    if (op == Opcode::FCmpOrd)
        return FoldResult::constant(Value::constBool(!std::isnan(lhs) && !std::isnan(rhs)));
    // FCmpUno
    return FoldResult::constant(Value::constBool(std::isnan(lhs) || std::isnan(rhs)));
}

//===----------------------------------------------------------------------===//
// Boolean Operations: Zext1, Trunc1
//===----------------------------------------------------------------------===//

/// @brief Fold zero-extend from 1 bit (boolean to integer).
/// @param ctx Operand resolver for the source bit value.
/// @return Integer zero or one when known, otherwise unknown.
static FoldResult foldZext1(const FoldContext &ctx) {
    long long operand{};
    if (!ctx.getConstIntOperand(0, operand))
        return FoldResult::unknown();
    return FoldResult::constant(Value::constInt((operand & 1) != 0 ? 1 : 0));
}

/// @brief Fold truncate to 1 bit (integer to boolean).
/// @param ctx Operand resolver for the source integer.
/// @return Boolean constant from the low bit, otherwise unknown.
static FoldResult foldTrunc1(const FoldContext &ctx) {
    long long operand{};
    if (!ctx.getConstIntOperand(0, operand))
        return FoldResult::unknown();
    return FoldResult::constant(Value::constBool((operand & 1) != 0));
}

//===----------------------------------------------------------------------===//
// Constant Materialization: ConstNull, ConstStr, AddrOf, ConstF64
//===----------------------------------------------------------------------===//

/// @brief Fold constant materialization instructions.
/// @param instr Candidate constant-producing instruction.
/// @return Materialized constant when its representation is safe to propagate.
static FoldResult foldConstantMaterialization(const Instr &instr) {
    switch (instr.op) {
        case Opcode::ConstNull:
            return FoldResult::constant(Value::null());
        case Opcode::ConstStr:
            // ConstStr is a runtime operation (rt_const_cstr(ptr) → str).
            // Cannot constant-fold: the operand is GlobalAddr (ptr type) but
            // the result is str type. Propagating the operand causes type
            // mismatches at use sites.
            return FoldResult::unknown();
        case Opcode::AddrOf:
            if (!instr.operands.empty())
                return FoldResult::constant(instr.operands[0]);
            return FoldResult::unknown();
        case Opcode::ConstF64:
            if (!instr.operands.empty() && instr.operands[0].kind == Value::Kind::ConstFloat)
                return FoldResult::constant(instr.operands[0]);
            return FoldResult::unknown();
        default:
            return FoldResult::unknown();
    }
}

//===----------------------------------------------------------------------===//
// Section 3: SCCPSolver Class
//===----------------------------------------------------------------------===//

/// @brief Per-function sparse conditional constant-propagation solver.
/// @details Owns lattice, use-index, executability, trap, and worklist state
///          while borrowing the function it eventually rewrites. Instruction
///          pointers remain stable during solving; CFG deletion occurs only
///          after the worklists and constant substitution finish.
class SCCPSolver {
  public:
    /// @brief Initialize lattice and use state for a function snapshot.
    /// @param function Function analyzed and rewritten in place.
    explicit SCCPSolver(Function &function)
        : function_(function), debug_(std::getenv("ZANNA_SCCP_DEBUG") != nullptr) {
        initialiseStates();
    }

    /// @brief Solve to a fixpoint, rewrite constants/terminators, and drop dead blocks.
    /// @return `true` when any instruction, terminator, or block changes.
    bool run() {
        if (function_.blocks.empty())
            return false;

        markBlockExecutable(0);
        process();
        rewriteConstants();
        foldTerminators();
        SimplifyCFG::Stats stats;
        SimplifyCFG::SimplifyCFGPassContext cleanupContext(function_, nullptr, stats);
        if (simplify_cfg::removeUnreachableBlocks(cleanupContext))
            changed_ = true;
        return changed_;
    }

  private:
    /// Function borrowed for the solver lifetime.
    Function &function_;
    /// Lattice state indexed by SSA id.
    std::unordered_map<unsigned, ValueLattice> values_;
    /// Instructions using each SSA id.
    std::unordered_map<unsigned, std::vector<Instr *>> uses_;
    /// Owning block index for every instruction.
    std::unordered_map<Instr *, size_t> instrBlock_;
    /// Lexical index within each block's StableList (pointer order is unrelated).
    std::unordered_map<Instr *, size_t> instrOrder_;
    /// Function block index keyed by label.
    std::unordered_map<std::string, size_t> blockIndex_;
    /// Blocks proven reachable by executable edges.
    std::vector<bool> blockExecutable_;
    /// Earliest currently trapping instruction in each block, or null. Arithmetic
    /// traps are provisional until the operand lattice reaches its fixed point.
    std::vector<Instr *> blockTraps_;
    /// Whether diagnostic lattice tracing is enabled.
    bool debug_ = false;
    /// Newly executable blocks awaiting instruction scheduling.
    std::queue<size_t> blockWorklist_;
    /// Instructions awaiting re-evaluation.
    std::queue<Instr *> instrWorklist_;
    /// Duplicate-suppression set for @ref instrWorklist_.
    std::unordered_set<Instr *> inInstrWorklist_;
    /// Accumulated rewrite flag returned by @ref run.
    bool changed_ = false;

    //===------------------------------------------------------------------===//
    // Initialization
    //===------------------------------------------------------------------===//

    /// @brief Index blocks, definitions, uses, and initial parameter lattice states.
    void initialiseStates() {
        blockExecutable_.assign(function_.blocks.size(), false);
        blockTraps_.assign(function_.blocks.size(), nullptr);
        for (size_t bi = 0; bi < function_.blocks.size(); ++bi) {
            blockIndex_[function_.blocks[bi].label] = bi;
        }

        /// Create a lattice entry and optionally mark an ABI-provided value overdefined.
        auto registerValue = [&](unsigned id, bool overdefined) {
            auto &entry = values_[id];
            if (overdefined && !entry.isOverdefined())
                entry = ValueLattice::overdefined();
        };

        for (auto &param : function_.params)
            registerValue(param.id, true);

        for (size_t bi = 0; bi < function_.blocks.size(); ++bi) {
            BasicBlock &block = function_.blocks[bi];
            for (auto &param : block.params)
                // Entry block params are ABI inputs even when their SSA ids differ
                // from Function::params after frontend lowering or serialization.
                registerValue(param.id, bi == 0);

            size_t ordinal = 0;
            for (auto &instr : block.instructions) {
                instrBlock_[&instr] = bi;
                instrOrder_[&instr] = ordinal++;
                if (instr.result)
                    registerValue(*instr.result, false);

                for (auto &operand : instr.operands) {
                    if (operand.kind == Value::Kind::Temp)
                        uses_[operand.id].push_back(&instr);
                }
                for (auto &args : instr.brArgs) {
                    for (auto &arg : args)
                        if (arg.kind == Value::Kind::Temp)
                            uses_[arg.id].push_back(&instr);
                }
            }
        }
    }

    //===------------------------------------------------------------------===//
    // Lattice State Management
    //===------------------------------------------------------------------===//

    /// @brief Get or create mutable lattice state for an SSA id.
    /// @param id SSA id to query.
    /// @return Mutable map entry.
    ValueLattice &valueState(unsigned id) {
        return values_[id];
    }

    /// @brief Get existing read-only lattice state for an SSA id.
    /// @param id SSA id known to have been registered.
    /// @return Const map entry.
    const ValueLattice &valueState(unsigned id) const {
        auto it = values_.find(id);
        assert(it != values_.end());
        return it->second;
    }

    /// @brief Mark a basic block as reachable and push it onto the block worklist.
    /// @details No-op when the block is already known executable; otherwise records
    ///          the block and schedules its instructions for re-evaluation.
    /// @param index Function block index to mark.
    void markBlockExecutable(size_t index) {
        if (blockExecutable_[index])
            return;
        blockExecutable_[index] = true;
        if (debug_)
            std::cerr << "[sccp] executable block " << function_.blocks[index].label << "\n";
        blockWorklist_.push(index);
    }

    /// @brief Record the earliest currently trapping instruction in a block.
    /// @details Earlier instructions and the trap itself must still be revisited;
    ///          a constant operand can become overdefined as another edge activates.
    /// @param index Function block index to mark.
    /// @param instr Trapping instruction, borrowed from this block's StableList.
    void markBlockTrap(size_t index, Instr &instr) {
        if (blockTraps_[index] && instrOrder_.at(blockTraps_[index]) <= instrOrder_.at(&instr))
            return;
        blockTraps_[index] = &instr;
        if (debug_)
            std::cerr << "[sccp] block " << function_.blocks[index].label << " known to trap\n";
    }

    /// @brief Emit a debug trace line for a lattice value change.
    /// @param id    Virtual register whose state changed.
    /// @param action Short description of the transition (e.g. "const").
    /// @param v Optional new constant value for the log line.
    void traceValueChange(unsigned id, std::string_view action, const Value *v = nullptr) {
        if (!debug_)
            return;
        std::cerr << "[sccp] " << action << " %" << id;
        if (v)
            std::cerr << " -> " << describeValue(*v);
        std::cerr << "\n";
    }

    /// @brief Add an instruction to the instruction worklist if not already present.
    /// @param instr Instruction to schedule.
    void enqueueInstr(Instr &instr) {
        if (inInstrWorklist_.insert(&instr).second)
            instrWorklist_.push(&instr);
    }

    /// @brief Schedule all instructions that use virtual register @p id for re-evaluation.
    /// @details Only enqueues instructions that live in currently executable blocks so
    ///          we never visit dead code.
    /// @param id SSA id whose lattice state changed.
    void enqueueUsers(unsigned id) {
        auto it = uses_.find(id);
        if (it == uses_.end())
            return;
        for (Instr *user : it->second) {
            auto bit = instrBlock_.find(user);
            if (bit == instrBlock_.end())
                continue;
            if (blockExecutable_[bit->second])
                enqueueInstr(*user);
        }
    }

    /// @brief Raise the lattice state for @p id to @p v, propagating to users.
    /// @param id SSA id receiving the constant.
    /// @param v Incoming constant value.
    /// @return @c true when the state actually changed (i.e. was previously Unknown).
    bool mergeConstant(unsigned id, const Value &v) {
        ValueLattice &state = valueState(id);
        if (state.mergeConstant(v)) {
            traceValueChange(id, "const", &v);
            enqueueUsers(id);
            return true;
        }
        return false;
    }

    /// @brief Raise the lattice state for @p id to Overdefined, propagating to users.
    /// @param id SSA id whose value can no longer be constant.
    /// @return @c true when the state actually changed.
    bool markOverdefined(unsigned id) {
        ValueLattice &state = valueState(id);
        if (state.markOverdefined()) {
            traceValueChange(id, "overdefined");
            enqueueUsers(id);
            return true;
        }
        return false;
    }

    //===------------------------------------------------------------------===//
    // Value Resolution
    //===------------------------------------------------------------------===//

    /// @brief Resolve a literal or constant-lattice temporary to a concrete value.
    /// @param operand Candidate literal or temporary.
    /// @param out Receives the resolved constant on success.
    /// @return `true` when @p operand has a concrete constant value.
    bool resolveValue(const Value &operand, Value &out) const {
        switch (operand.kind) {
            case Value::Kind::ConstInt:
            case Value::Kind::ConstFloat:
            case Value::Kind::ConstStr:
            case Value::Kind::GlobalAddr:
            case Value::Kind::NullPtr:
                out = operand;
                return true;
            case Value::Kind::Temp: {
                auto it = values_.find(operand.id);
                if (it == values_.end())
                    return false;
                const ValueLattice &state = it->second;
                if (state.isConstant()) {
                    out = state.value;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    /// @brief Test whether a temporary operand's lattice state is overdefined.
    /// @param operand Value to inspect.
    /// @return `true` only for a registered overdefined temporary.
    bool operandOverdefined(const Value &operand) const {
        if (operand.kind != Value::Kind::Temp)
            return false;
        auto it = values_.find(operand.id);
        if (it == values_.end())
            return false;
        return it->second.isOverdefined();
    }

    //===------------------------------------------------------------------===//
    // Worklist Processing
    //===------------------------------------------------------------------===//

    /// @brief Drain block and instruction worklists until the lattice reaches a fixpoint.
    void process() {
        while (!blockWorklist_.empty() || !instrWorklist_.empty()) {
            if (!blockWorklist_.empty()) {
                size_t blockIndex = blockWorklist_.front();
                blockWorklist_.pop();
                BasicBlock &block = function_.blocks[blockIndex];
                for (auto &instr : block.instructions)
                    enqueueInstr(instr);
                continue;
            }

            Instr *instr = instrWorklist_.front();
            instrWorklist_.pop();
            inInstrWorklist_.erase(instr);
            auto bit = instrBlock_.find(instr);
            if (bit == instrBlock_.end())
                continue;
            if (!blockExecutable_[bit->second])
                continue;
            visitInstruction(*instr, bit->second);
        }
    }

    //===------------------------------------------------------------------===//
    // Edge Propagation
    //===------------------------------------------------------------------===//

    /// @brief Mark one successor executable and merge its block-parameter arguments.
    /// @param fromBlockIndex Source block used to suppress edges after known traps.
    /// @param terminator Source terminator containing labels and argument bundles.
    /// @param succSlot Successor slot to activate.
    void propagateEdge(size_t fromBlockIndex, Instr &terminator, size_t succSlot) {
        if (blockTraps_[fromBlockIndex])
            return;
        if (succSlot >= terminator.labels.size())
            return;
        const std::string &targetLabel = terminator.labels[succSlot];
        auto it = blockIndex_.find(targetLabel);
        if (it == blockIndex_.end())
            return;
        size_t succIndex = it->second;
        markBlockExecutable(succIndex);
        BasicBlock &succ = function_.blocks[succIndex];
        if (succSlot >= terminator.brArgs.size())
            return;
        const auto &args = terminator.brArgs[succSlot];
        for (size_t pi = 0; pi < succ.params.size() && pi < args.size(); ++pi) {
            const Value &arg = args[pi];
            Value resolved;
            if (resolveValue(arg, resolved)) {
                mergeConstant(succ.params[pi].id, resolved);
            } else if (operandOverdefined(arg)) {
                markOverdefined(succ.params[pi].id);
            }
        }
    }

    //===------------------------------------------------------------------===//
    // Instruction Visitors
    //===------------------------------------------------------------------===//

    /// @brief Dispatch a single instruction to the appropriate visitor.
    /// @details Terminators are handled specially: unconditional branches propagate
    ///          along their single edge; trapping terminators mark the block; all
    ///          other instructions are forwarded to @ref visitComputational.
    /// @param instr Instruction to evaluate.
    /// @param blockIndex Owning function block index.
    void visitInstruction(Instr &instr, size_t blockIndex) {
        // StableList guarantees identity, not address ordering. Do not freeze
        // definitions on which a provisional trap depends; only instructions
        // lexically beyond it are presently unreachable.
        if (blockTraps_[blockIndex] &&
            instrOrder_.at(&instr) > instrOrder_.at(blockTraps_[blockIndex]))
            return;

        switch (instr.op) {
            case Opcode::Br:
                propagateEdge(blockIndex, instr, 0);
                break;
            case Opcode::CBr:
                visitCBr(blockIndex, instr);
                break;
            case Opcode::SwitchI32:
                visitSwitch(blockIndex, instr);
                break;
            case Opcode::Trap:
            case Opcode::TrapFromErr:
            case Opcode::TrapErr:
            case Opcode::ResumeSame:
            case Opcode::ResumeNext:
            case Opcode::ResumeLabel:
                markBlockTrap(blockIndex, instr);
                break;
            default:
                visitComputational(instr, blockIndex);
                break;
        }
    }

    /// @brief Evaluate a conditional branch and propagate along the reachable edge(s).
    /// @details When the condition is a known constant, only the taken edge is activated.
    ///          When the condition is overdefined, both edges are marked executable.
    /// @param blockIndex Owning block index.
    /// @param instr Conditional branch to evaluate.
    void visitCBr(size_t blockIndex, Instr &instr) {
        if (instr.operands.empty())
            return;
        Value cond;
        if (resolveValue(instr.operands[0], cond)) {
            bool truth = false;
            if (!getConstBool(cond, truth))
                return;
            if (truth)
                propagateEdge(blockIndex, instr, 0);
            else
                propagateEdge(blockIndex, instr, 1);
        } else if (operandOverdefined(instr.operands[0])) {
            propagateEdge(blockIndex, instr, 0);
            propagateEdge(blockIndex, instr, 1);
        }
    }

    /// @brief Evaluate a switch and propagate along matching case edge(s).
    /// @details When the scrutinee is a known constant, only the matching arm is
    ///          activated (or the default if no case matches).  When overdefined,
    ///          all edges are propagated.
    /// @param blockIndex Owning block index.
    /// @param instr Integer switch to evaluate.
    void visitSwitch(size_t blockIndex, Instr &instr) {
        if (instr.operands.empty())
            return;
        Value scrut;
        if (resolveValue(instr.operands[0], scrut) && scrut.kind == Value::Kind::ConstInt) {
            bool matched = false;
            for (size_t ci = 0; ci < switchCaseCount(instr); ++ci) {
                const Value &caseVal = switchCaseValue(instr, ci);
                if (caseVal.kind == Value::Kind::ConstInt && caseVal.i64 == scrut.i64) {
                    propagateEdge(blockIndex, instr, ci + 1);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                propagateEdge(blockIndex, instr, 0);
        } else if (operandOverdefined(instr.operands[0])) {
            for (size_t li = 0; li < instr.labels.size(); ++li)
                propagateEdge(blockIndex, instr, li);
        }
    }

    /// @brief Evaluate a non-terminator instruction and update its result lattice.
    /// @details Attempts constant folding.  If all operands are constants and the
    ///          fold succeeds the result is raised to Constant; if any operand is
    ///          overdefined the result is raised to Overdefined.
    /// @param instr Instruction whose result state is updated.
    /// @param blockIndex Owning block index used to record known traps.
    void visitComputational(Instr &instr, size_t blockIndex) {
        FoldResult folded = foldInstruction(instr);
        if (folded.isTrap()) {
            markBlockTrap(blockIndex, instr);
            return;
        }

        if (blockTraps_[blockIndex] == &instr) {
            blockTraps_[blockIndex] = nullptr;
            // The formerly suppressed suffix, including its terminator, needs
            // another visit even when it does not directly use the changed input.
            blockWorklist_.push(blockIndex);
            if (debug_)
                std::cerr << "[sccp] reconsider block " << function_.blocks[blockIndex].label
                          << " after provisional trap changed\n";
        }

        if (!instr.result)
            return;

        bool anyOverdefined = false;
        bool allConstants = !instr.operands.empty();
        for (auto &operand : instr.operands) {
            Value resolved;
            if (!resolveValue(operand, resolved)) {
                allConstants = false;
                if (operandOverdefined(operand))
                    anyOverdefined = true;
            }
        }

        if (folded.isConstant()) {
            mergeConstant(*instr.result, folded.value);
            return;
        }

        if (isAlwaysOverdefined(instr.op) || anyOverdefined || allConstants)
            markOverdefined(*instr.result);
    }

    //===------------------------------------------------------------------===//
    // Overdefined Classification
    //===------------------------------------------------------------------===//

    /// @brief Check if an opcode always produces overdefined results.
    /// @details Side-effecting operations and operations with external
    ///          dependencies cannot be constant-folded.
    /// @param op Opcode to classify.
    /// @return `true` when SCCP must never model the result as a constant.
    bool isAlwaysOverdefined(Opcode op) const {
        switch (op) {
            // Memory operations
            case Opcode::Load:
            case Opcode::Alloca:
            case Opcode::GEP:
            case Opcode::Store:
            // Calls
            case Opcode::Call:
            // Exception handling
            case Opcode::ResumeSame:
            case Opcode::ResumeNext:
            case Opcode::ResumeLabel:
            case Opcode::EhPush:
            case Opcode::EhPop:
            case Opcode::Trap:
            case Opcode::TrapFromErr:
            case Opcode::TrapErr:
            case Opcode::ErrGetKind:
            case Opcode::ErrGetCode:
            case Opcode::ErrGetIp:
            case Opcode::ErrGetLine:
            // Runtime checks
            case Opcode::IdxChk:
                return true;
            default:
                return false;
        }
    }

    //===------------------------------------------------------------------===//
    // Main Folding Dispatch
    //===------------------------------------------------------------------===//

    /// @brief Attempt to fold an instruction to a constant value.
    /// @details Dispatches to family-specific fold functions based on opcode.
    /// @param instr Instruction evaluated against current lattice state.
    /// @return Constant, trap, or unknown folding result.
    FoldResult foldInstruction(const Instr &instr) const {
        // Create fold context with operand resolver
        /// Resolve one source operand through the solver's current lattice.
        FoldContext ctx{instr, [this, &instr](size_t index, Value &out) -> bool {
                            if (index >= instr.operands.size())
                                return false;
                            return resolveValue(instr.operands[index], out);
                        }};

        switch (instr.op) {
            //===--------------------------------------------------------------===//
            // Integer Arithmetic
            //===--------------------------------------------------------------===//
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::SDiv:
            case Opcode::UDiv:
            case Opcode::SRem:
            case Opcode::URem:
            case Opcode::Shl:
            case Opcode::LShr:
            case Opcode::AShr:
                return foldIntegerArithmetic(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Overflow-Checked Arithmetic
            //===--------------------------------------------------------------===//
            case Opcode::IAddOvf:
            case Opcode::ISubOvf:
            case Opcode::IMulOvf:
                return foldOverflowArithmetic(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Division and Remainder
            //===--------------------------------------------------------------===//
            case Opcode::SDivChk0:
            case Opcode::SRemChk0:
                return foldSignedDivRem(instr.op, ctx);

            case Opcode::UDivChk0:
            case Opcode::URemChk0:
                return foldUnsignedDivRem(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Floating-Point Arithmetic
            //===--------------------------------------------------------------===//
            case Opcode::FAdd:
            case Opcode::FSub:
            case Opcode::FMul:
            case Opcode::FDiv:
                return foldFloatArithmetic(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Integer Comparisons
            //===--------------------------------------------------------------===//
            case Opcode::ICmpEq:
            case Opcode::ICmpNe:
            case Opcode::SCmpLT:
            case Opcode::SCmpLE:
            case Opcode::SCmpGT:
            case Opcode::SCmpGE:
                return foldIntegerComparisons(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Unsigned Comparisons
            //===--------------------------------------------------------------===//
            case Opcode::UCmpLT:
            case Opcode::UCmpLE:
            case Opcode::UCmpGT:
            case Opcode::UCmpGE:
                return foldUnsignedComparisons(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Floating-Point Comparisons
            //===--------------------------------------------------------------===//
            case Opcode::FCmpEQ:
            case Opcode::FCmpNE:
            case Opcode::FCmpLT:
            case Opcode::FCmpLE:
            case Opcode::FCmpGT:
            case Opcode::FCmpGE:
                return foldFloatComparisons(instr.op, ctx);
            case Opcode::FCmpOrd:
            case Opcode::FCmpUno:
                return foldFCmpOrdUno(instr.op, ctx);

            //===--------------------------------------------------------------===//
            // Type Conversions
            //===--------------------------------------------------------------===//
            case Opcode::CastSiToFp:
            case Opcode::Sitofp:
                return foldCastSiToFp(ctx);
            case Opcode::CastUiToFp:
                return foldCastUiToFp(ctx);
            case Opcode::CastFpToSiRteChk:
                return foldCastFpToSi(ctx);
            case Opcode::CastFpToUiRteChk:
                return foldCastFpToUi(ctx);
            case Opcode::Fptosi:
                return foldFptosi(ctx);

            //===--------------------------------------------------------------===//
            // Boolean Operations
            //===--------------------------------------------------------------===//
            case Opcode::Zext1:
                return foldZext1(ctx);
            case Opcode::Trunc1:
                return foldTrunc1(ctx);

            //===--------------------------------------------------------------===//
            // Constant Materialization
            //===--------------------------------------------------------------===//
            case Opcode::ConstNull:
            case Opcode::ConstStr:
            case Opcode::AddrOf:
            case Opcode::ConstF64:
                return foldConstantMaterialization(instr);

            default:
                return FoldResult::unknown();
        }
    }

    //===------------------------------------------------------------------===//
    // Rewriting Phase
    //===------------------------------------------------------------------===//

    /// @brief Substitute all SSA values whose lattice state is Constant.
    /// @details Iterates the value map and calls @ref replaceAllUses for every
    ///          entry that holds a definite constant.  Non-constant values are
    ///          left unchanged.
    void rewriteConstants() {
        for (auto &[id, state] : values_) {
            if (!state.isConstant())
                continue;
            replaceAllUses(id, state.value);
        }
    }

    /// @brief Replace all uses of a value with a constant using the pre-built use map.
    /// @details Uses the `uses_` map built during initialization for O(uses) replacement
    ///          instead of O(blocks × instructions) full traversal.
    /// @param id SSA id being eliminated from uses.
    /// @param replacement Constant propagated into every indexed use.
    void replaceAllUses(unsigned id, const Value &replacement) {
        auto usesIt = uses_.find(id);
        if (usesIt == uses_.end())
            return;

        for (Instr *instr : usesIt->second) {
            for (auto &operand : instr->operands)
                if (operand.kind == Value::Kind::Temp && operand.id == id) {
                    changed_ = true;
                    operand = replacement;
                }
            for (auto &args : instr->brArgs)
                for (auto &arg : args)
                    if (arg.kind == Value::Kind::Temp && arg.id == id) {
                        changed_ = true;
                        arg = replacement;
                    }
        }
    }

    //===------------------------------------------------------------------===//
    // Terminator Folding
    //===------------------------------------------------------------------===//

    /// @brief Lower constant conditional branches and switches to unconditional branches.
    /// @details Walks all executable, non-trapping blocks and converts any CBr or
    ///          SwitchI32 whose scrutinee is now a known constant into a plain Br.
    void foldTerminators() {
        for (size_t bi = 0; bi < function_.blocks.size(); ++bi) {
            if (!blockExecutable_[bi] || blockTraps_[bi])
                continue;
            BasicBlock &block = function_.blocks[bi];
            if (block.instructions.empty())
                continue;
            Instr &term = block.instructions.back();
            if (term.op == Opcode::CBr)
                rewriteConditional(term);
            else if (term.op == Opcode::SwitchI32)
                rewriteSwitch(term);
        }
    }

    /// @brief Rewrite a constant CBr into an unconditional branch along the taken edge.
    /// @param instr Conditional branch whose condition is resolved from the lattice.
    void rewriteConditional(Instr &instr) {
        if (instr.operands.empty())
            return;
        Value cond;
        if (!resolveValue(instr.operands[0], cond))
            return;
        bool truth = false;
        if (!getConstBool(cond, truth))
            return;
        if (truth)
            convertToBranch(instr, 0);
        else
            convertToBranch(instr, 1);
    }

    /// @brief Mutate a conditional or switch terminator into an unconditional branch.
    /// @param instr    Terminator instruction to rewrite in place.
    /// @param succSlot Index into @c instr.labels selecting the target successor.
    void convertToBranch(Instr &instr, size_t succSlot) {
        if (succSlot >= instr.labels.size())
            return;
        std::string label = instr.labels[succSlot];
        std::vector<std::vector<Value>> args;
        if (succSlot < instr.brArgs.size())
            args.push_back(instr.brArgs[succSlot]);
        else
            args.emplace_back();
        changed_ = true;
        instr.op = Opcode::Br;
        instr.operands.clear();
        instr.setBranchTargets({std::move(label)}, std::move(args));
        instr.type = Type(Type::Kind::Void);
    }

    /// @brief Rewrite a constant SwitchI32 into an unconditional branch to the matching arm.
    /// @param instr Switch terminator whose scrutinee is resolved from the lattice.
    void rewriteSwitch(Instr &instr) {
        if (instr.operands.empty())
            return;
        Value scrut;
        if (!resolveValue(instr.operands[0], scrut) || scrut.kind != Value::Kind::ConstInt)
            return;
        for (size_t ci = 0; ci < switchCaseCount(instr); ++ci) {
            const Value &caseVal = switchCaseValue(instr, ci);
            if (caseVal.kind == Value::Kind::ConstInt && caseVal.i64 == scrut.i64) {
                convertToBranch(instr, ci + 1);
                return;
            }
        }
        convertToBranch(instr, 0);
    }
};

} // namespace

//===----------------------------------------------------------------------===//
// Section 4: Public API
//===----------------------------------------------------------------------===//

/// @copydoc sccp(core::Function &)
bool sccp(Function &function) {
    SCCPSolver solver(function);
    return solver.run();
}

/// @copydoc sccp(core::Module &)
void sccp(Module &module) {
    for (auto &function : module.functions)
        sccp(function);
    module.internOwnedIdentifiers();
}

} // namespace il::transform
