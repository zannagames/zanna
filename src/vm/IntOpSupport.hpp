//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/IntOpSupport.hpp
// Purpose: Shared helpers for integer opcode handlers, covering trap dispatch,
//          width selection, checked arithmetic, division, shifts, and bounds.
// Key invariants: Helpers operate on canonicalised Slot values and honour IL trap
//                 semantics for exceptional arithmetic.
// Ownership/Lifetime: Stateless inline helpers; no heap allocation or ownership transfer.
// Links: docs/il/il-guide.md#reference §Integer Arithmetic, §Bitwise and Shifts,
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared integer-operation kernels and dispatch helpers for VM
///        opcode handlers.
/// @details The stateless helpers select IL integer widths, execute checked and
///          unchecked arithmetic, normalize bounds results, and route semantic
///          failures through the VM's contextual trap machinery.

#pragma once

#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/Trap.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/semantics/ScalarOps.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

// Note: MSVC overflow checking is handled by OpHandlerUtils.hpp
// which provides ops::checked_add/sub/mul that work on all platforms.

namespace il::vm::detail::integer {
/// @brief Emit a trap with context from the current instruction and frame.
/// @details Thin wrapper that formats trap metadata for RuntimeBridge, ensuring
///          all instruction traps include function, block, and source location
///          information for better diagnostics.
/// @param kind Classification of the trap (e.g., overflow, divide-by-zero).
/// @param message Human-readable description of the failure.
/// @param in Instruction that triggered the trap.
/// @param fr Active frame containing function metadata.
/// @param bb Current basic block pointer, may be null.
inline void emitTrap(TrapKind kind,
                     const char *message,
                     const il::core::Instr &in,
                     Frame &fr,
                     const il::core::BasicBlock *bb) {
    RuntimeBridge::trap(kind,
                        message,
                        in.loc,
                        fr.func ? fr.func->name : std::string(),
                        bb ? bb->label : std::string());
}

/// @brief Convert an IL type kind to the shared scalar integer width tag.
/// @details Non-integer or unsupported kinds conservatively select I64 so the
///          semantic fallback matches the historical VM dispatch behavior.
/// @param kind Instruction result type kind.
/// @return Shared integer width used by the scalar semantics kernel.
[[nodiscard]] constexpr il::semantics::IntWidth semanticWidthForType(
    il::core::Type::Kind kind) noexcept {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return il::semantics::IntWidth::I1;
        case il::core::Type::Kind::I16:
            return il::semantics::IntWidth::I16;
        case il::core::Type::Kind::I32:
            return il::semantics::IntWidth::I32;
        case il::core::Type::Kind::I64:
        default:
            return il::semantics::IntWidth::I64;
    }
}

/// @brief Convert a shared semantic trap kind to the IL VM trap kind.
/// @param trap Shared semantic trap category.
/// @return IL VM trap kind used by RuntimeBridge diagnostics.
[[nodiscard]] constexpr TrapKind toVmTrapKind(il::semantics::TrapKind trap) noexcept {
    switch (trap) {
        case il::semantics::TrapKind::None:
            return TrapKind::DomainError;
        case il::semantics::TrapKind::DivideByZero:
            return TrapKind::DivideByZero;
        case il::semantics::TrapKind::Overflow:
            return TrapKind::Overflow;
        case il::semantics::TrapKind::InvalidCast:
            return TrapKind::InvalidCast;
        case il::semantics::TrapKind::Bounds:
            return TrapKind::Bounds;
        case il::semantics::TrapKind::InvalidOperation:
            return TrapKind::InvalidOperation;
        case il::semantics::TrapKind::DomainError:
        default:
            return TrapKind::DomainError;
    }
}

/// @brief Store a semantic integer result or emit the corresponding trap.
/// @details The operand dispatcher stores the output slot after a compute functor
///          returns.  On trap this helper writes a deterministic zero placeholder
///          after raising the trap so no uninitialized slot data is observed while
///          the VM unwinds or dispatches a handler.
/// @param result Shared semantic result to apply.
/// @param out Output slot written on success, or zeroed after a trap.
/// @param in Instruction that triggered the operation.
/// @param fr Active frame used for trap diagnostics.
/// @param bb Current basic block pointer, may be null.
/// @param trapMessage Human-readable message emitted when @p result traps.
inline void storeSemanticResultOrTrap(const il::semantics::SemanticResult<int64_t> &result,
                                      Slot &out,
                                      const il::core::Instr &in,
                                      Frame &fr,
                                      const il::core::BasicBlock *bb,
                                      const char *trapMessage) {
    if (!result.ok()) {
        out.i64 = 0;
        emitTrap(toVmTrapKind(result.trap), trapMessage, in, fr, bb);
        return;
    }
    out.i64 = result.value;
}

/// @brief Apply an overflow-checking binary operation for a specific integer type.
/// @tparam T Signed integer type (e.g., int16_t, int32_t, int64_t).
/// @tparam OverflowOp Callable with signature <tt>bool(T, T, T*)</tt> returning true on overflow.
/// @param in Instruction being executed.
/// @param fr Active frame containing function metadata.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot that receives the result on success.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
/// @param trapMessage Human-readable message emitted when overflow is detected.
/// @param op Overflow-checking operation invoked with the decoded operands.
template <typename T, typename OverflowOp>
void applyOverflowingBinary(const il::core::Instr &in,
                            Frame &fr,
                            const il::core::BasicBlock *bb,
                            Slot &out,
                            const Slot &lhsVal,
                            const Slot &rhsVal,
                            const char *trapMessage,
                            OverflowOp &&op) {
    T lhs = static_cast<T>(lhsVal.i64);
    T rhs = static_cast<T>(rhsVal.i64);
    T result{};
    if (op(lhs, rhs, &result)) {
        emitTrap(TrapKind::Overflow, trapMessage, in, fr, bb);
        return;
    }
    out.i64 = static_cast<int64_t>(result);
}

/// @brief Dispatch an overflow-checking binary operation based on the instruction's type kind.
/// @tparam OverflowOp Callable with signature <tt>bool(T, T, T*)</tt> returning true on overflow.
/// @param in Instruction whose type kind selects the integer width.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot that receives the result on success.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
/// @param trapMessage Human-readable message emitted when overflow is detected.
/// @param overflowOp Overflow-checking operation forwarded to applyOverflowingBinary.
template <typename OverflowOp>
void dispatchOverflowingBinary(const il::core::Instr &in,
                               Frame &fr,
                               const il::core::BasicBlock *bb,
                               Slot &out,
                               const Slot &lhsVal,
                               const Slot &rhsVal,
                               const char *trapMessage,
                               OverflowOp overflowOp) {
    switch (in.type.kind) {
        case il::core::Type::Kind::I16:
            applyOverflowingBinary<int16_t>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage, overflowOp);
            break;
        case il::core::Type::Kind::I32:
            applyOverflowingBinary<int32_t>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage, overflowOp);
            break;
        case il::core::Type::Kind::I64:
        default:
            applyOverflowingBinary<int64_t>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage, overflowOp);
            break;
    }
}

/// @brief Perform signed integer division with zero-divisor and MIN/-1 overflow checks.
/// @tparam T Signed integer type determining the arithmetic width.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot receiving the quotient on success.
/// @param lhsVal Left operand (dividend) slot.
/// @param rhsVal Right operand (divisor) slot.
template <typename T>
void applySignedDiv(const il::core::Instr &in,
                    Frame &fr,
                    const il::core::BasicBlock *bb,
                    Slot &out,
                    const Slot &lhsVal,
                    const Slot &rhsVal) {
    const T lhs = static_cast<T>(lhsVal.i64);
    const T rhs = static_cast<T>(rhsVal.i64);
    if (rhs == 0) {
        emitTrap(TrapKind::DivideByZero, "divide by zero in sdiv", in, fr, bb);
        return;
    }
    if (lhs == std::numeric_limits<T>::min() && rhs == static_cast<T>(-1)) {
        emitTrap(TrapKind::Overflow, "integer overflow in sdiv", in, fr, bb);
        return;
    }
    out.i64 = static_cast<int64_t>(static_cast<T>(lhs / rhs));
}

/// @brief Perform signed integer remainder with zero-divisor and MIN/-1 checks.
/// @details When the dividend is the type minimum and the divisor is -1, the
///          remainder is defined as zero (no trap).
/// @tparam T Signed integer type determining the arithmetic width.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot receiving the remainder on success.
/// @param lhsVal Left operand (dividend) slot.
/// @param rhsVal Right operand (divisor) slot.
template <typename T>
void applySignedRem(const il::core::Instr &in,
                    Frame &fr,
                    const il::core::BasicBlock *bb,
                    Slot &out,
                    const Slot &lhsVal,
                    const Slot &rhsVal) {
    const T lhs = static_cast<T>(lhsVal.i64);
    const T rhs = static_cast<T>(rhsVal.i64);
    if (rhs == 0) {
        emitTrap(TrapKind::DivideByZero, "divide by zero in srem", in, fr, bb);
        return;
    }
    if (lhs == std::numeric_limits<T>::min() && rhs == static_cast<T>(-1)) {
        out.i64 = 0;
        return;
    }

    const int64_t wideLhs = static_cast<int64_t>(lhs);
    const int64_t wideRhs = static_cast<int64_t>(rhs);
    const int64_t quotient = wideLhs / wideRhs;
    const int64_t remainder = wideLhs - quotient * wideRhs;
    out.i64 = static_cast<int64_t>(static_cast<T>(remainder));
}

/// @brief Perform checked signed division (sdiv.chk0) with zero-divisor and overflow traps.
/// @tparam T Signed integer type determining the arithmetic width.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot receiving the quotient on success.
/// @param lhsVal Left operand (dividend) slot.
/// @param rhsVal Right operand (divisor) slot.
template <typename T>
void applyCheckedDiv(const il::core::Instr &in,
                     Frame &fr,
                     const il::core::BasicBlock *bb,
                     Slot &out,
                     const Slot &lhsVal,
                     const Slot &rhsVal) {
    T lhs = static_cast<T>(lhsVal.i64);
    T rhs = static_cast<T>(rhsVal.i64);
    if (rhs == 0) {
        emitTrap(TrapKind::DivideByZero, "divide by zero in sdiv.chk0", in, fr, bb);
        return;
    }
    if (lhs == std::numeric_limits<T>::min() && rhs == static_cast<T>(-1)) {
        emitTrap(TrapKind::Overflow, "integer overflow in sdiv.chk0", in, fr, bb);
        return;
    }
    out.i64 = static_cast<int64_t>(static_cast<T>(lhs / rhs));
}

/// @brief Perform checked signed remainder (srem.chk0) with zero-divisor trap.
/// @details When the dividend is the type minimum and the divisor is -1, the
///          remainder is defined as zero (no trap).
/// @tparam T Signed integer type determining the arithmetic width.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot receiving the remainder on success.
/// @param lhsVal Left operand (dividend) slot.
/// @param rhsVal Right operand (divisor) slot.
template <typename T>
void applyCheckedRem(const il::core::Instr &in,
                     Frame &fr,
                     const il::core::BasicBlock *bb,
                     Slot &out,
                     const Slot &lhsVal,
                     const Slot &rhsVal) {
    T lhs = static_cast<T>(lhsVal.i64);
    T rhs = static_cast<T>(rhsVal.i64);
    if (rhs == 0) {
        emitTrap(TrapKind::DivideByZero, "divide by zero in srem.chk0", in, fr, bb);
        return;
    }
    if (lhs == std::numeric_limits<T>::min() && rhs == static_cast<T>(-1)) {
        out.i64 = 0;
        return;
    }
    const int64_t wideLhs = static_cast<int64_t>(lhs);
    const int64_t wideRhs = static_cast<int64_t>(rhs);
    const int64_t quotient = wideLhs / wideRhs;
    const int64_t remainder = wideLhs - quotient * wideRhs;
    out.i64 = static_cast<int64_t>(static_cast<T>(remainder));
}

/// @brief Function pointer type for checked signed binary operations.
/// @param in Instruction providing type and source metadata.
/// @param fr Active execution frame.
/// @param bb Current basic block, or null.
/// @param[out] out Destination slot.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
using CheckedSignedBinaryFn = void (*)(const il::core::Instr &,
                                       Frame &,
                                       const il::core::BasicBlock *,
                                       Slot &,
                                       const Slot &,
                                       const Slot &);

/// @brief Dispatch a checked signed binary operation by type kind using function pointer templates.
/// @tparam ApplyI16 Handler for 16-bit signed integers.
/// @tparam ApplyI32 Handler for 32-bit signed integers.
/// @tparam ApplyI64 Handler for 64-bit signed integers.
/// @param in Instruction whose type kind selects the integer width.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot that receives the result on success.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
template <CheckedSignedBinaryFn ApplyI16,
          CheckedSignedBinaryFn ApplyI32,
          CheckedSignedBinaryFn ApplyI64>
void dispatchCheckedSignedBinary(const il::core::Instr &in,
                                 Frame &fr,
                                 const il::core::BasicBlock *bb,
                                 Slot &out,
                                 const Slot &lhsVal,
                                 const Slot &rhsVal) {
    switch (in.type.kind) {
        case il::core::Type::Kind::I16:
            ApplyI16(in, fr, bb, out, lhsVal, rhsVal);
            break;
        case il::core::Type::Kind::I32:
            ApplyI32(in, fr, bb, out, lhsVal, rhsVal);
            break;
        case il::core::Type::Kind::I64:
        default:
            ApplyI64(in, fr, bb, out, lhsVal, rhsVal);
            break;
    }
}

/// @brief Check whether an index falls within a half-open [lo, hi) range.
/// @tparam T Integer type used for comparison (determines sign semantics).
/// @param idxSlot Slot containing the index value to check.
/// @param loSlot Slot containing the inclusive lower bound.
/// @param hiSlot Slot containing the exclusive upper bound.
/// @return A pair: (true, normalized-offset) on success, or (false, raw-index) on failure.
template <typename T>
[[nodiscard]] std::pair<bool, int64_t> performBoundsCheck(const Slot &idxSlot,
                                                          const Slot &loSlot,
                                                          const Slot &hiSlot) {
    const auto idx = static_cast<T>(idxSlot.i64);
    const auto lo = static_cast<T>(loSlot.i64);
    const auto hi = static_cast<T>(hiSlot.i64);
    if (idx < lo || idx >= hi) {
        return {false, static_cast<int64_t>(idx)};
    }
    using UnsignedT = std::make_unsigned_t<T>;
    const auto normalized =
        static_cast<UnsignedT>(idx) - static_cast<UnsignedT>(lo);
    if (normalized > static_cast<UnsignedT>(std::numeric_limits<int64_t>::max()))
        return {false, static_cast<int64_t>(idx)};
    return {true, static_cast<int64_t>(normalized)};
}

/// @brief Check whether a signed 64-bit value fits in a narrower signed type.
/// @tparam NarrowT Target narrow signed integer type.
/// @param value Value to check.
/// @return @c true if @p value fits within @c NarrowT's range.
template <typename NarrowT> [[nodiscard]] constexpr bool fitsSignedRange(int64_t value) noexcept {
    return value >= static_cast<int64_t>(std::numeric_limits<NarrowT>::min()) &&
           value <= static_cast<int64_t>(std::numeric_limits<NarrowT>::max());
}

/// @brief Apply unsigned division or remainder with a zero-divisor trap.
/// @tparam ComputeOp Callable with signature <tt>uint64_t(uint64_t, uint64_t)</tt>.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block pointer, may be null.
/// @param out Output slot receiving the result on success.
/// @param lhsVal Left operand (dividend) slot.
/// @param rhsVal Right operand (divisor) slot.
/// @param trapMessage Human-readable message emitted on divide-by-zero.
/// @param compute Functor computing the unsigned result from dividend and divisor.
template <typename ComputeOp>
void applyUnsignedDivOrRem(const il::core::Instr &in,
                           Frame &fr,
                           const il::core::BasicBlock *bb,
                           Slot &out,
                           const Slot &lhsVal,
                           const Slot &rhsVal,
                           const char *trapMessage,
                           ComputeOp compute) {
    const auto divisor = static_cast<uint64_t>(rhsVal.i64);
    if (divisor == 0) {
        emitTrap(TrapKind::DivideByZero, trapMessage, in, fr, bb);
        return;
    }

    const auto dividend = static_cast<uint64_t>(lhsVal.i64);
    out.i64 = static_cast<int64_t>(compute(dividend, divisor));
}

/// @brief Check whether an unsigned 64-bit value fits in a narrower unsigned type.
/// @tparam NarrowT Target narrow unsigned integer type.
/// @param value Value to check.
/// @return @c true if @p value fits within @c NarrowT's range.
template <typename NarrowT>
[[nodiscard]] constexpr bool fitsUnsignedRange(uint64_t value) noexcept {
    return value <= static_cast<uint64_t>(std::numeric_limits<NarrowT>::max());
}

// ============================================================================
// Optimized Integer Operation Helpers (CRITICAL-4 / HIGH-4 Optimization)
// ============================================================================
// These helpers eliminate lambda captures and reduce type dispatch overhead
// by using function pointers and explicit type parameters.

/// @brief Function pointer type for overflow-checking binary operations.
/// @tparam T Integer type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param[out] out Receives the result when representable.
/// @return `true` on overflow.
template <typename T> using OverflowCheckFn = bool (*)(T lhs, T rhs, T *out);

/// @brief Stateless overflow-checking add function for use as function pointer.
/// @details Delegates to ops::checked_add which handles MSVC portability.
/// @tparam T Integer operand and result type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Destination for the sum when representable.
/// @return @c true when addition overflows; otherwise @c false.
template <typename T> inline bool overflowAdd(T lhs, T rhs, T *out) {
    return ops::checked_add(lhs, rhs, out);
}

/// @brief Stateless overflow-checking sub function for use as function pointer.
/// @details Delegates to ops::checked_sub which handles MSVC portability.
/// @tparam T Integer operand and result type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Destination for the difference when representable.
/// @return @c true when subtraction overflows; otherwise @c false.
template <typename T> inline bool overflowSub(T lhs, T rhs, T *out) {
    return ops::checked_sub(lhs, rhs, out);
}

/// @brief Stateless overflow-checking mul function for use as function pointer.
/// @details Delegates to ops::checked_mul which handles MSVC portability.
/// @tparam T Integer operand and result type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Destination for the product when representable.
/// @return @c true when multiplication overflows; otherwise @c false.
template <typename T> inline bool overflowMul(T lhs, T rhs, T *out) {
    return ops::checked_mul(lhs, rhs, out);
}

/// @brief Apply an overflow-checking binary operation for a specific type.
/// @tparam T Integer type (int16_t, int32_t, int64_t).
/// @tparam OverflowFn Function pointer for the overflow-checking operation.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block.
/// @param out Output slot.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
/// @param trapMessage Message for overflow trap.
/// @details Uses function pointer instead of lambda to avoid closure allocation.
template <typename T, OverflowCheckFn<T> OverflowFn>
inline void applyOverflowingBinaryDirect(const il::core::Instr &in,
                                         Frame &fr,
                                         const il::core::BasicBlock *bb,
                                         Slot &out,
                                         const Slot &lhsVal,
                                         const Slot &rhsVal,
                                         const char *trapMessage) {
    T lhs = static_cast<T>(lhsVal.i64);
    T rhs = static_cast<T>(rhsVal.i64);
    T result{};
    if (OverflowFn(lhs, rhs, &result)) {
        emitTrap(TrapKind::Overflow, trapMessage, in, fr, bb);
        return;
    }
    out.i64 = static_cast<int64_t>(result);
}

/// @brief Dispatch an overflow-checking binary operation based on type kind.
/// @tparam OverflowFn16 Function pointer for int16_t overflow check.
/// @tparam OverflowFn32 Function pointer for int32_t overflow check.
/// @tparam OverflowFn64 Function pointer for int64_t overflow check.
/// @details Uses template function pointers instead of lambdas for efficiency.
/// @param in Instruction whose result type selects the integer width.
/// @param fr Active frame used for trap diagnostics.
/// @param bb Current basic block pointer, which may be @c nullptr.
/// @param out Output slot written when the operation succeeds.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
/// @param trapMessage Diagnostic emitted if the selected operation overflows.
template <OverflowCheckFn<int16_t> OverflowFn16,
          OverflowCheckFn<int32_t> OverflowFn32,
          OverflowCheckFn<int64_t> OverflowFn64>
inline void dispatchOverflowingBinaryDirect(const il::core::Instr &in,
                                            Frame &fr,
                                            const il::core::BasicBlock *bb,
                                            Slot &out,
                                            const Slot &lhsVal,
                                            const Slot &rhsVal,
                                            const char *trapMessage) {
    switch (in.type.kind) {
        case il::core::Type::Kind::I16:
            applyOverflowingBinaryDirect<int16_t, OverflowFn16>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage);
            break;
        case il::core::Type::Kind::I32:
            applyOverflowingBinaryDirect<int32_t, OverflowFn32>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage);
            break;
        case il::core::Type::Kind::I64:
        default:
            applyOverflowingBinaryDirect<int64_t, OverflowFn64>(
                in, fr, bb, out, lhsVal, rhsVal, trapMessage);
            break;
    }
}

/// @brief Compute functor for overflow-checking addition (CRITICAL-4 optimization).
/// @details Stateless functor that can be passed without creating a closure.
struct OverflowAddOp {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.
    const char *trapMessage;        ///< Trap message on overflow.

    /// @brief Dispatch overflow-checking addition across integer widths.
    /// @param out Output slot receiving the result.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::checkedAdd(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind)),
            out,
            in,
            fr,
            bb,
            trapMessage);
    }
};

/// @brief Compute functor for overflow-checking subtraction (CRITICAL-4 optimization).
struct OverflowSubOp {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.
    const char *trapMessage;        ///< Trap message on overflow.

    /// @brief Dispatch overflow-checking subtraction across integer widths.
    /// @param out Output slot receiving the result.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::checkedSub(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind)),
            out,
            in,
            fr,
            bb,
            trapMessage);
    }
};

/// @brief Compute functor for overflow-checking multiplication (CRITICAL-4 optimization).
struct OverflowMulOp {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.
    const char *trapMessage;        ///< Trap message on overflow.

    /// @brief Dispatch overflow-checking multiplication across integer widths.
    /// @param out Output slot receiving the result.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::checkedMul(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind)),
            out,
            in,
            fr,
            bb,
            trapMessage);
    }
};

/// @brief Stateless bitwise AND functor for use with applyBinary.
struct BitwiseAndOp {
    /// @brief Compute bitwise AND of two slots.
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot.
    /// @param rhs Right operand slot.
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        out.i64 = lhs.i64 & rhs.i64;
    }
};

/// @brief Stateless bitwise OR functor for use with applyBinary.
struct BitwiseOrOp {
    /// @brief Compute bitwise OR of two slots.
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot.
    /// @param rhs Right operand slot.
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        out.i64 = lhs.i64 | rhs.i64;
    }
};

/// @brief Stateless bitwise XOR functor for use with applyBinary.
struct BitwiseXorOp {
    /// @brief Compute bitwise XOR of two slots.
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot.
    /// @param rhs Right operand slot.
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        out.i64 = lhs.i64 ^ rhs.i64;
    }
};

/// @brief Stateless left-shift functor for use with applyBinary.
struct ShiftLeftOp {
    /// @brief Compute left shift, masking the shift amount to [0, 63].
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot (value to shift).
    /// @param rhs Right operand slot (shift amount).
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        const uint64_t shift = static_cast<uint64_t>(rhs.i64) & 63U;
        const uint64_t value = static_cast<uint64_t>(lhs.i64);
        out.i64 = static_cast<int64_t>(value << shift);
    }
};

/// @brief Stateless logical right-shift functor for use with applyBinary.
struct LogicalShiftRightOp {
    /// @brief Compute logical (unsigned) right shift, masking the shift amount to [0, 63].
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot (value to shift).
    /// @param rhs Right operand slot (shift amount).
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        const uint64_t shift = static_cast<uint64_t>(rhs.i64) & 63U;
        const uint64_t value = static_cast<uint64_t>(lhs.i64);
        out.i64 = static_cast<int64_t>(value >> shift);
    }
};

/// @brief Stateless arithmetic right-shift functor for use with applyBinary.
struct ArithmeticShiftRightOp {
    /// @brief Compute arithmetic (sign-preserving) right shift, masking the shift amount to [0,
    /// 63].
    /// @param out Output slot receiving the result.
    /// @param lhs Left operand slot (value to shift).
    /// @param rhs Right operand slot (shift amount).
    void operator()(Slot &out, const Slot &lhs, const Slot &rhs) const {
        const uint64_t shift = static_cast<uint64_t>(rhs.i64) & 63U;
        if (shift == 0) {
            out.i64 = lhs.i64;
            return;
        }
        const uint64_t value = static_cast<uint64_t>(lhs.i64);
        const bool isNegative = (value & (uint64_t{1} << 63U)) != 0;
        uint64_t shifted = value >> shift;
        if (isNegative) {
            const uint64_t mask = (~uint64_t{0}) << (64U - shift);
            shifted |= mask;
        }
        out.i64 = static_cast<int64_t>(shifted);
    }
};

/// @brief Stateless unsigned division compute functor.
struct UnsignedDivOp {
    /// @brief Compute unsigned quotient.
    /// @param dividend Dividend value.
    /// @param divisor Divisor value (must be non-zero).
    /// @param result Receives the quotient.
    void operator()(uint64_t dividend, uint64_t divisor, uint64_t &result) const {
        result = dividend / divisor;
    }
};

/// @brief Stateless unsigned remainder compute functor.
struct UnsignedRemOp {
    /// @brief Compute unsigned remainder.
    /// @param dividend Dividend value.
    /// @param divisor Divisor value (must be non-zero).
    /// @param result Receives the remainder.
    void operator()(uint64_t dividend, uint64_t divisor, uint64_t &result) const {
        result = dividend % divisor;
    }
};

/// @brief Apply unsigned division or remainder with zero-check.
/// @tparam ComputeOp Stateless functor type for the operation.
/// @param in Instruction being executed.
/// @param fr Active frame.
/// @param bb Current basic block.
/// @param out Output slot.
/// @param lhsVal Left operand slot.
/// @param rhsVal Right operand slot.
/// @param trapMessage Message for divide-by-zero trap.
template <typename ComputeOp>
inline void applyUnsignedDivOrRemDirect(const il::core::Instr &in,
                                        Frame &fr,
                                        const il::core::BasicBlock *bb,
                                        Slot &out,
                                        const Slot &lhsVal,
                                        const Slot &rhsVal,
                                        const char *trapMessage) {
    const auto divisor = static_cast<uint64_t>(rhsVal.i64);
    if (divisor == 0) {
        emitTrap(TrapKind::DivideByZero, trapMessage, in, fr, bb);
        return;
    }
    const auto dividend = static_cast<uint64_t>(lhsVal.i64);
    uint64_t result{};
    ComputeOp{}(dividend, divisor, result);
    out.i64 = static_cast<int64_t>(result);
}

/// @brief Compute functor for unsigned division with zero-check (CRITICAL-4 optimization).
struct UnsignedDivWithCheck {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.
    const char *trapMessage;        ///< Trap message on divide-by-zero.

    /// @brief Perform unsigned division with a zero-divisor trap.
    /// @param out Output slot receiving the quotient.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::unsignedDiv(lhsVal.i64, rhsVal.i64), out, in, fr, bb, trapMessage);
    }
};

/// @brief Compute functor for unsigned remainder with zero-check (CRITICAL-4 optimization).
struct UnsignedRemWithCheck {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.
    const char *trapMessage;        ///< Trap message on divide-by-zero.

    /// @brief Perform unsigned remainder with a zero-divisor trap.
    /// @param out Output slot receiving the remainder.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::unsignedRem(lhsVal.i64, rhsVal.i64), out, in, fr, bb, trapMessage);
    }
};

/// @brief Compute functor for signed division with type dispatch (CRITICAL-4 optimization).
struct SignedDivWithDispatch {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.

    /// @brief Dispatch signed division across integer widths with trap checks.
    /// @param out Output slot receiving the quotient.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        const auto result =
            il::semantics::signedDiv(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind));
        storeSemanticResultOrTrap(result,
                                  out,
                                  in,
                                  fr,
                                  bb,
                                  result.trap == il::semantics::TrapKind::Overflow
                                      ? "integer overflow in sdiv"
                                      : "divide by zero in sdiv");
    }
};

/// @brief Compute functor for signed remainder with type dispatch (CRITICAL-4 optimization).
struct SignedRemWithDispatch {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.

    /// @brief Dispatch signed remainder across integer widths with trap checks.
    /// @param out Output slot receiving the remainder.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::signedRem(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind)),
            out,
            in,
            fr,
            bb,
            "divide by zero in srem");
    }
};

/// @brief Compute functor for checked signed division (CRITICAL-4 optimization).
struct CheckedSignedDivWithDispatch {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.

    /// @brief Dispatch checked signed division (sdiv.chk0) across integer widths.
    /// @param out Output slot receiving the quotient.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        const auto result =
            il::semantics::signedDiv(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind));
        storeSemanticResultOrTrap(result,
                                  out,
                                  in,
                                  fr,
                                  bb,
                                  result.trap == il::semantics::TrapKind::Overflow
                                      ? "integer overflow in sdiv.chk0"
                                      : "divide by zero in sdiv.chk0");
    }
};

/// @brief Compute functor for checked signed remainder (CRITICAL-4 optimization).
struct CheckedSignedRemWithDispatch {
    const il::core::Instr &in;      ///< Instruction being executed.
    Frame &fr;                      ///< Active frame.
    const il::core::BasicBlock *bb; ///< Current basic block.

    /// @brief Dispatch checked signed remainder (srem.chk0) across integer widths.
    /// @param out Output slot receiving the remainder.
    /// @param lhsVal Left operand (dividend) slot.
    /// @param rhsVal Right operand (divisor) slot.
    void operator()(Slot &out, const Slot &lhsVal, const Slot &rhsVal) const {
        storeSemanticResultOrTrap(
            il::semantics::signedRem(lhsVal.i64, rhsVal.i64, semanticWidthForType(in.type.kind)),
            out,
            in,
            fr,
            bb,
            "divide by zero in srem.chk0");
    }
};

} // namespace il::vm::detail::integer
