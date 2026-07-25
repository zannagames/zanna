//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/internal/OpHelpers.hpp
// Purpose: Provide reusable helpers for decoding VM operands and emitting common
//          traps used by opcode handlers.
// Key invariants: Helpers only operate on operands belonging to the active frame
//                 and never retain references beyond the call site.
// Ownership/Lifetime: Functions access VM state through the existing handler
//                     access layer without introducing new global state.
// Links: docs/il/il-guide.md#reference, src/vm/OpHelpers.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/internal/OpHelpers.hpp
 * @brief Defines typed operand, result, binary-operation, and trap helpers for
 *        VM opcode handlers.
 *
 * @details
 * These helpers translate between the VM's untyped `Slot` representation and
 * handler-selected C++ scalar types, centralize common binary handler flow, and
 * attach instruction/frame context to arithmetic traps. All VM, frame,
 * instruction, block, and string inputs are borrowed for the duration of the
 * call.
 */

#pragma once

#include "vm/OpHandlerAccess.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/Trap.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"

#include <cstddef>
#include <type_traits>

namespace il::vm::internal
{
namespace detail
{
/**
 * @brief Raise a runtime-bridge trap with current execution context.
 * @param kind Trap classification, such as overflow or divide-by-zero.
 * @param message Borrowed, non-null human-readable diagnostic.
 * @param instr Borrowed instruction supplying the source location.
 * @param frame Active frame supplying optional function metadata.
 * @param block Optional active block supplying its label.
 */
void trapWithMessage(TrapKind kind,
                     const char *message,
                     const il::core::Instr &instr,
                     Frame &frame,
                     const il::core::BasicBlock *block);

/**
 * @brief Selects the `Slot` member used for a supported scalar C++ type.
 * @tparam T Scalar type loaded or stored.
 * @tparam Enable SFINAE selector used by partial specializations.
 *
 * The primary template is intentionally undefined. Instantiation is supported
 * only for non-boolean integral, floating-point, and `bool` types.
 */
template <typename T, typename Enable = void> struct SlotTraits;

/// @brief Maps non-boolean integral values through `Slot::i64`.
template <typename T>
struct SlotTraits<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
{
    /**
     * @brief Convert the slot's signed integer payload to `T`.
     * @param slot Slot whose `i64` member is active.
     * @return `static_cast<T>(slot.i64)`.
     */
    [[nodiscard]] static T load(const Slot &slot) noexcept
    {
        return static_cast<T>(slot.i64);
    }

    /**
     * @brief Store an integral value through the slot's signed integer member.
     * @param slot Destination whose `i64` member becomes active.
     * @param value Value converted with `static_cast<int64_t>`.
     */
    static void store(Slot &slot, T value) noexcept
    {
        slot.i64 = static_cast<int64_t>(value);
    }
};

/// @brief Maps floating-point values through `Slot::f64`.
template <typename T> struct SlotTraits<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
    /**
     * @brief Convert the slot's double payload to `T`.
     * @param slot Slot whose `f64` member is active.
     * @return `static_cast<T>(slot.f64)`.
     */
    [[nodiscard]] static T load(const Slot &slot) noexcept
    {
        return static_cast<T>(slot.f64);
    }

    /**
     * @brief Store a floating value through the slot's double member.
     * @param slot Destination whose `f64` member becomes active.
     * @param value Value converted with `static_cast<double>`.
     */
    static void store(Slot &slot, T value) noexcept
    {
        slot.f64 = static_cast<double>(value);
    }
};

/// @brief Maps boolean values through normalized zero-or-one `Slot::i64` data.
template <> struct SlotTraits<bool, void>
{
    /**
     * @brief Interpret any nonzero integer slot payload as true.
     * @param slot Slot whose `i64` member is active.
     * @return `slot.i64 != 0`.
     */
    [[nodiscard]] static bool load(const Slot &slot) noexcept
    {
        return slot.i64 != 0;
    }

    /**
     * @brief Store false as zero or true as one.
     * @param slot Destination whose `i64` member becomes active.
     * @param value Boolean value to normalize.
     */
    static void store(Slot &slot, bool value) noexcept
    {
        slot.i64 = value ? 1 : 0;
    }
};

} // namespace detail

/**
 * @brief Evaluate one instruction operand and decode its slot as `T`.
 * @tparam T Type supported by `detail::SlotTraits`.
 * @param vm Active VM used to resolve constants, temporaries, and globals.
 * @param fr Active frame containing temporary operand slots.
 * @param instr Borrowed instruction whose operand vector is indexed.
 * @param index Zero-based operand index.
 * @return The evaluated slot payload converted to `T`.
 *
 * @pre `index < instr.operands.size()`.
 * @pre The evaluated slot member agrees with the selected `SlotTraits<T>`
 *      representation.
 */
template <typename T>
[[nodiscard]] inline T readOperand(VM &vm, Frame &fr, const il::core::Instr &instr, size_t index)
{
    const Slot slot = il::vm::detail::VMAccess::eval(vm, fr, instr.operands[index]);
    return detail::SlotTraits<T>::load(slot);
}

/**
 * @brief Encode a typed value and store it in an instruction destination.
 * @tparam T Type supported by `detail::SlotTraits`.
 * @param fr Active frame that owns the destination temporary.
 * @param instr Borrowed result-producing instruction.
 * @param value Scalar payload to encode.
 *
 * @pre `instr` has a valid result temporary addressable in `fr`.
 * @note The local `Slot` is intentionally not zero-initialized because the
 *       selected trait fully initializes the active union member.
 */
template <typename T> inline void writeResult(Frame &fr, const il::core::Instr &instr, T value)
{
    Slot slot;
    detail::SlotTraits<T>::store(slot, value);
    il::vm::detail::ops::storeResult(fr, instr, slot);
}

/**
 * @brief Implement a non-control-flow binary opcode over scalar type `T`.
 * @tparam T Operand and result type supported by `SlotTraits`.
 * @tparam Compute Callable type invocable as `compute(T lhs, T rhs)`.
 * @param vm Active VM used to evaluate operands zero and one.
 * @param fr Active frame receiving the result.
 * @param instr Borrowed instruction with two operands and a destination.
 * @param compute Callable forwarded exactly once and invoked left-to-right with
 *                decoded scalar operands.
 * @return Default `VM::ExecResult`, signalling normal fallthrough.
 *
 * @pre `instr` has at least two operands and a valid result temporary.
 * @note The helper does not advance an instruction pointer or modify block
 *       control-flow metadata.
 */
template <typename T, typename Compute>
inline VM::ExecResult binaryOp(VM &vm, Frame &fr, const il::core::Instr &instr, Compute &&compute)
{
    const T lhs = readOperand<T>(vm, fr, instr, 0);
    const T rhs = readOperand<T>(vm, fr, instr, 1);
    writeResult<T>(fr, instr, std::forward<Compute>(compute)(lhs, rhs));
    return {};
}

/**
 * @brief Raise a divide-by-zero trap with instruction context.
 * @param instr Borrowed instruction supplying the source location.
 * @param frame Active frame supplying optional function metadata.
 * @param block Optional current block supplying its label.
 * @param message Borrowed, non-null human-readable diagnostic.
 */
inline void trapDivideByZero(const il::core::Instr &instr,
                             Frame &frame,
                             const il::core::BasicBlock *block,
                             const char *message)
{
    detail::trapWithMessage(TrapKind::DivideByZero, message, instr, frame, block);
}

/**
 * @brief Raise an overflow trap with instruction context.
 * @param instr Borrowed instruction supplying the source location.
 * @param frame Active frame supplying optional function metadata.
 * @param block Optional current block supplying its label.
 * @param message Borrowed, non-null human-readable diagnostic.
 */
inline void trapOverflow(const il::core::Instr &instr,
                         Frame &frame,
                         const il::core::BasicBlock *block,
                         const char *message)
{
    detail::trapWithMessage(TrapKind::Overflow, message, instr, frame, block);
}

/**
 * @brief Guard a division-like operation against a zero divisor.
 * @tparam T Scalar type constructible from integer zero and equality-comparable.
 * @param divisor Value compared with `static_cast<T>(0)`.
 * @param instr Borrowed instruction supplying the trap source location.
 * @param frame Active frame supplying optional function metadata.
 * @param block Optional current block supplying its label.
 * @param message Borrowed, non-null divide-by-zero diagnostic.
 * @return `true` when the divisor is nonzero; `false` after raising the trap.
 *
 * @note For floating-point `T`, both positive and negative zero trap, while NaN
 *       passes the guard.
 */
template <typename T>
[[nodiscard]] inline bool ensureNonZero(T divisor,
                                        const il::core::Instr &instr,
                                        Frame &frame,
                                        const il::core::BasicBlock *block,
                                        const char *message)
{
    if (divisor == static_cast<T>(0))
    {
        trapDivideByZero(instr, frame, block, message);
        return false;
    }
    return true;
}

} // namespace il::vm::internal
