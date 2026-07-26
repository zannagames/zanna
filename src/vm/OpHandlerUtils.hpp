//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/OpHandlerUtils.hpp
// Purpose: Shared helper routines for VM opcode handlers.
// Key invariants: Helpers operate on VM frames without leaking references.
// Ownership/Lifetime: Functions mutate frame state in-place without storing globals.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares arithmetic, operand-evaluation, result-storage, and control
///        helpers shared by VM opcode handlers.
/// @details The utilities provide cross-platform checked integer operations,
///          cached operand evaluation, consistent register ownership updates,
///          and validation of resume and error tokens.

#pragma once

#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"

#include "il/core/Instr.hpp"
#include "il/core/Value.hpp"

#include <bit>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace il::core {
struct BasicBlock;
}

namespace il::vm::detail {
namespace ops {

#ifdef _MSC_VER
// MSVC doesn't have __builtin_*_overflow, so we implement our own

/// @brief Perform checked signed addition with overflow detection for MSVC.
/// @tparam T Signed integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed sum.
/// @return True when the addition overflowed.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_signed<T>::value, bool>::type checked_add_impl(
    T lhs, T rhs, T *result) {
    // Signed overflow: if signs are the same but result sign differs
    *result = static_cast<T>(static_cast<typename std::make_unsigned<T>::type>(lhs) +
                             static_cast<typename std::make_unsigned<T>::type>(rhs));
    if (rhs >= 0)
        return *result < lhs; // Positive rhs, result should be >= lhs
    else
        return *result > lhs; // Negative rhs, result should be <= lhs
}

/// @brief Perform checked unsigned addition with overflow detection for MSVC.
/// @tparam T Unsigned integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed sum.
/// @return True when the addition wrapped around.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_unsigned<T>::value, bool>::type
checked_add_impl(T lhs, T rhs, T *result) {
    *result = lhs + rhs;
    return *result < lhs; // Unsigned wrap-around
}

/// @brief Perform checked signed subtraction with overflow detection for MSVC.
/// @tparam T Signed integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed difference.
/// @return True when the subtraction overflowed.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_signed<T>::value, bool>::type checked_sub_impl(
    T lhs, T rhs, T *result) {
    *result = static_cast<T>(static_cast<typename std::make_unsigned<T>::type>(lhs) -
                             static_cast<typename std::make_unsigned<T>::type>(rhs));
    if (rhs >= 0)
        return *result > lhs; // Subtracting positive, result should be <= lhs
    else
        return *result < lhs; // Subtracting negative, result should be >= lhs
}

/// @brief Perform checked unsigned subtraction with overflow detection for MSVC.
/// @tparam T Unsigned integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed difference.
/// @return True when the subtraction underflowed.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_unsigned<T>::value, bool>::type
checked_sub_impl(T lhs, T rhs, T *result) {
    *result = lhs - rhs;
    return lhs < rhs; // Unsigned underflow
}

/// @brief Perform checked signed multiplication with overflow detection for MSVC.
/// @tparam T Signed integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed product.
/// @return True when the multiplication overflowed.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_signed<T>::value, bool>::type checked_mul_impl(
    T lhs, T rhs, T *result) {
    using U = typename std::make_unsigned<T>::type;
    // Use double-width multiplication for 32-bit types
    if constexpr (sizeof(T) <= 4) {
        int64_t wide = static_cast<int64_t>(lhs) * static_cast<int64_t>(rhs);
        *result = static_cast<T>(wide);
        return wide < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
               wide > static_cast<int64_t>(std::numeric_limits<T>::max());
    } else {
        // For 64-bit, check for overflow before computing
        if (lhs == 0 || rhs == 0) {
            *result = 0;
            return false;
        }
        if (lhs == std::numeric_limits<T>::min() && rhs == -1) {
            *result = std::numeric_limits<T>::min();
            return true;
        }
        if (rhs == std::numeric_limits<T>::min() && lhs == -1) {
            *result = std::numeric_limits<T>::min();
            return true;
        }
        const T max = std::numeric_limits<T>::max();
        const T min = std::numeric_limits<T>::min();
        bool overflow = false;
        if (lhs > 0) {
            if (rhs > 0) {
                overflow = lhs > max / rhs;
            } else {
                overflow = rhs < min / lhs;
            }
        } else {
            if (rhs > 0) {
                overflow = lhs < min / rhs;
            } else {
                overflow = lhs < max / rhs;
            }
        }
        if (overflow) {
            *result = static_cast<T>(static_cast<U>(lhs) * static_cast<U>(rhs));
            return true;
        }
        *result = lhs * rhs;
        return false;
    }
}

/// @brief Perform checked unsigned multiplication with overflow detection for MSVC.
/// @tparam T Unsigned integer type.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed product.
/// @return True when the multiplication overflowed.
template <typename T>
[[nodiscard]] inline typename std::enable_if<std::is_unsigned<T>::value, bool>::type
checked_mul_impl(T lhs, T rhs, T *result) {
    if (lhs == 0 || rhs == 0) {
        *result = 0;
        return false;
    }
    *result = lhs * rhs;
    return lhs > std::numeric_limits<T>::max() / rhs;
}

#endif // _MSC_VER

/// @brief Perform checked addition using compiler builtins.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @return True when the operation overflowed.
/// @note Force-inlined for optimal performance in hot interpreter loops.
template <typename T> [[nodiscard]] inline bool checked_add(T lhs, T rhs, T *result) {
#ifdef _MSC_VER
    return checked_add_impl(lhs, rhs, result);
#else
    return __builtin_add_overflow(lhs, rhs, result);
#endif
}

/// @brief Perform checked subtraction using compiler builtins.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @return True when the operation overflowed.
/// @note Force-inlined for optimal performance in hot interpreter loops.
template <typename T> [[nodiscard]] inline bool checked_sub(T lhs, T rhs, T *result) {
#ifdef _MSC_VER
    return checked_sub_impl(lhs, rhs, result);
#else
    return __builtin_sub_overflow(lhs, rhs, result);
#endif
}

/// @brief Perform checked multiplication using compiler builtins.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @return True when the operation overflowed.
/// @note Force-inlined for optimal performance in hot interpreter loops.
template <typename T> [[nodiscard]] inline bool checked_mul(T lhs, T rhs, T *result) {
#ifdef _MSC_VER
    return checked_mul_impl(lhs, rhs, result);
#else
    return __builtin_mul_overflow(lhs, rhs, result);
#endif
}

/// @brief Apply two's complement wrapping semantics to addition.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the addition with wrap-around semantics.
template <typename T> inline T wrap_add(T lhs, T rhs) {
    T result{};
    (void)checked_add(lhs, rhs, &result);
    return result;
}

/// @brief Apply two's complement wrapping semantics to subtraction.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the subtraction with wrap-around semantics.
template <typename T> inline T wrap_sub(T lhs, T rhs) {
    T result{};
    (void)checked_sub(lhs, rhs, &result);
    return result;
}

/// @brief Apply two's complement wrapping semantics to multiplication.
/// @tparam T Arithmetic type to operate on.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Result of the multiplication with wrap-around semantics.
template <typename T> inline T wrap_mul(T lhs, T rhs) {
    T result{};
    (void)checked_mul(lhs, rhs, &result);
    return result;
}

/// @brief Perform checked addition and invoke a trap policy on overflow.
/// @tparam T Arithmetic type to operate on.
/// @tparam Trap Callable invoked when overflow occurs.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @param trap Policy invoked when overflow occurs.
/// @return True when the result is valid, false if overflow triggered the trap.
template <typename T, typename Trap> inline bool trap_add(T lhs, T rhs, T *result, Trap &&trap) {
    if (checked_add(lhs, rhs, result)) {
        std::forward<Trap>(trap)();
        return false;
    }
    return true;
}

/// @brief Perform checked subtraction and invoke a trap policy on overflow.
/// @tparam T Arithmetic type to operate on.
/// @tparam Trap Callable invoked when overflow occurs.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @param trap Policy invoked when overflow occurs.
/// @return True when the result is valid, false if overflow triggered the trap.
template <typename T, typename Trap> inline bool trap_sub(T lhs, T rhs, T *result, Trap &&trap) {
    if (checked_sub(lhs, rhs, result)) {
        std::forward<Trap>(trap)();
        return false;
    }
    return true;
}

/// @brief Perform checked multiplication and invoke a trap policy on overflow.
/// @tparam T Arithmetic type to operate on.
/// @tparam Trap Callable invoked when overflow occurs.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param result Pointer receiving the computed value.
/// @param trap Policy invoked when overflow occurs.
/// @return True when the result is valid, false if overflow triggered the trap.
template <typename T, typename Trap> inline bool trap_mul(T lhs, T rhs, T *result, Trap &&trap) {
    if (checked_mul(lhs, rhs, result)) {
        std::forward<Trap>(trap)();
        return false;
    }
    return true;
}

/// @brief Store the result of an instruction if it produces one.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param val Slot to write into the destination register.
void storeResult(Frame &fr, const il::core::Instr &in, const Slot &val);

/// @brief Evaluate a pre-resolved operand in the hot dispatch path.
/// @details Avoids the @c std::vector<Value> heap indirection and the
///          @c Value::kind branch for the three common operand kinds.
///          The @c Cold case falls back to @c VM::eval() which handles
///          ConstStr, GlobalAddr, and NullPtr correctly.
/// @param vm      Active VM (needed for the Cold fallback).
/// @param fr      Current execution frame providing the register file.
/// @param op      Pre-resolved operand from @c BlockExecCache.
/// @param original Original IL value, used only for the @c Cold fallback.
/// @return @c Slot containing the evaluated operand.
[[nodiscard]] inline Slot evalFast(VM &vm,
                                   Frame &fr,
                                   const ResolvedOp &op,
                                   const il::core::Value &original) noexcept {
    switch (op.kind) {
        case ResolvedOp::Kind::Reg:
            // Hot path: register read — bounds check guards rare out-of-range errors
            if (op.regId < fr.regs.size()) [[likely]]
                return fr.regs[op.regId];
            return detail::VMAccess::eval(vm, fr, original); // let VM::eval() report the error
        case ResolvedOp::Kind::ImmI64: {
            Slot s{};
            s.i64 = op.numVal;
            return s;
        }
        case ResolvedOp::Kind::ImmF64: {
            Slot s{};
            s.f64 = std::bit_cast<double>(op.numVal);
            return s;
        }
        case ResolvedOp::Kind::Cold:
        default:
            return detail::VMAccess::eval(vm, fr, original);
    }
}

/// @brief Internal dispatcher that evaluates operands via the VM.
/// @note Optimized for the dispatch hot path: uses uninitialized Slot for output
///       since the compute/compare functor immediately overwrites it.
///       When @c ExecState::blockCache is populated the pre-resolved @c ResolvedOp
///       array is used to avoid the @c std::vector<Value> heap indirection.
struct OperandDispatcher {
    /// @brief Evaluate two instruction operands and apply a computation.
    /// @details Uses the current block's pre-resolved operand cache when
    ///          available, falls back to general VM evaluation otherwise, then
    ///          stores the functor-produced slot through @ref storeResult.
    /// @tparam Compute Callable accepting output, left, and right slots.
    /// @param vm Active VM used for operand evaluation.
    /// @param fr Current frame providing registers.
    /// @param in Binary instruction containing two operands and result metadata.
    /// @param compute Computation that writes the output slot.
    /// @return Empty execution result indicating normal fallthrough.
    template <typename Compute>
    static VM::ExecResult runBinary(VM &vm,
                                    Frame &fr,
                                    const il::core::Instr &in,
                                    Compute &&compute) {
        Slot lhs, rhs;
        // Fast path: use pre-resolved operand cache when available.
        const auto *state = detail::VMAccess::currentExecState(vm);
        const auto *bc = state ? state->blockCache : nullptr;
        if (bc && state->ip < bc->instrOpOffset.size()) [[likely]] {
            const size_t off = bc->instrOpOffset[state->ip];
            lhs = evalFast(vm, fr, bc->resolvedOps[off], in.operands[0]);
            rhs = evalFast(vm, fr, bc->resolvedOps[off + 1], in.operands[1]);
        } else {
            lhs = vm.eval(fr, in.operands[0]);
            rhs = vm.eval(fr, in.operands[1]);
        }
        Slot out;
        std::forward<Compute>(compute)(out, lhs, rhs);
        storeResult(fr, in, out);
        return {};
    }

    /// @brief Evaluate two instruction operands and apply a predicate.
    /// @details Shares the cached evaluation path with @ref runBinary, converts
    ///          the predicate result to canonical integer boolean form, and
    ///          stores it in the instruction destination.
    /// @tparam Compare Callable returning a truth value for two operand slots.
    /// @param vm Active VM used for operand evaluation.
    /// @param fr Current frame providing registers.
    /// @param in Comparison instruction containing operands and result metadata.
    /// @param compare Predicate applied to the evaluated slots.
    /// @return Empty execution result indicating normal fallthrough.
    template <typename Compare>
    static VM::ExecResult runCompare(VM &vm,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     Compare &&compare) {
        Slot lhs, rhs;
        // Fast path: use pre-resolved operand cache when available.
        const auto *state = detail::VMAccess::currentExecState(vm);
        const auto *bc = state ? state->blockCache : nullptr;
        if (bc && state->ip < bc->instrOpOffset.size()) [[likely]] {
            const size_t off = bc->instrOpOffset[state->ip];
            lhs = evalFast(vm, fr, bc->resolvedOps[off], in.operands[0]);
            rhs = evalFast(vm, fr, bc->resolvedOps[off + 1], in.operands[1]);
        } else {
            lhs = vm.eval(fr, in.operands[0]);
            rhs = vm.eval(fr, in.operands[1]);
        }
        Slot out;
        out.i64 = std::forward<Compare>(compare)(lhs, rhs) ? 1 : 0;
        storeResult(fr, in, out);
        return {};
    }
};

/// @brief Evaluate a binary opcode's operands and run a computation functor.
/// @tparam Compute Callable with signature <tt>void(Slot &, const Slot &, const Slot &)</tt>.
/// @param vm Active virtual machine used for operand evaluation.
/// @param fr Current execution frame.
/// @param in Instruction describing operand locations and result slot.
/// @param compute Functor that writes the computed result into the provided output slot.
/// @return Execution result signalling normal fallthrough.
template <typename Compute>
VM::ExecResult applyBinary(VM &vm, Frame &fr, const il::core::Instr &in, Compute &&compute) {
    return OperandDispatcher::runBinary(vm, fr, in, std::forward<Compute>(compute));
}

/// @brief Evaluate a binary opcode's operands and run a comparison functor.
/// @tparam Compare Callable with signature <tt>bool(const Slot &, const Slot &)</tt>.
/// @param vm Active virtual machine used for operand evaluation.
/// @param fr Current execution frame.
/// @param in Instruction describing operand locations and result slot.
/// @param compare Functor returning true when the predicate holds.
/// @return Execution result signalling normal fallthrough.
template <typename Compare>
VM::ExecResult applyCompare(VM &vm, Frame &fr, const il::core::Instr &in, Compare &&compare) {
    return OperandDispatcher::runCompare(vm, fr, in, std::forward<Compare>(compare));
}
} // namespace ops

namespace control {
/// @brief Extract a valid resume token from a slot, or return nullptr if invalid.
/// @param fr Active frame holding the resume state metadata.
/// @param slot Slot expected to contain a resume token pointer.
/// @return Pointer to the frame's resume state if valid, or nullptr.
Frame::ResumeState *expectResumeToken(Frame &fr, const Slot &slot);

/// @brief Emit a trap indicating that a resume instruction received an invalid token.
/// @param fr Active frame.
/// @param in Instruction that attempted the resume.
/// @param bb Current basic block pointer, may be null.
/// @param detail Human-readable diagnostic describing the failure.
void trapInvalidResume(Frame &fr,
                       const il::core::Instr &in,
                       const il::core::BasicBlock *bb,
                       std::string detail);

/// @brief Resolve an error token slot to the corresponding VmError payload.
/// @param fr Active frame holding error state.
/// @param slot Slot expected to contain an error token.
/// @return Explicit error pointer from @p slot, the current trap token when one
///         exists, or the address of @ref Frame::activeError as a final fallback.
const VmError *resolveErrorToken(Frame &fr, const Slot &slot);
} // namespace control
} // namespace il::vm::detail
