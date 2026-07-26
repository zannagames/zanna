//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the floating-point opcode handlers used by the virtual machine.
// Each helper interprets the operands stored in the active frame, performs the
// requested IEEE-754 operation, and writes the result back through the shared
// op-handler utilities while honouring the IL specification's trapping rules.
//
//===----------------------------------------------------------------------===//

#include "vm/OpHandlers_Float.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/FPCast.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/semantics/ScalarOps.hpp"
#include "zanna/vm/internal/OpHelpers.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

/// @file
/// @brief Floating-point opcode handlers and helpers for the VM interpreter.
/// @details The routines here all follow the same pattern: evaluate operand
///          slots via the shared `ops::apply*` helpers, perform the floating
///          point computation using host IEEE-754 semantics, and finally write
///          the result back to the destination slot.  Checked conversions rely
///          on local utilities that raise structured traps through the runtime
///          bridge when the source value cannot be represented exactly.

using namespace il::core;

namespace il::vm::detail::floating {
namespace {
/// @brief Convert an IL integer type to its semantic bit width.
/// @param kind Integer result type requested by a checked conversion.
/// @return 16, 32, or 64 bits; unsupported kinds conservatively select 64.
[[nodiscard]] int integerTypeBits(Type::Kind kind) {
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

/// @brief Convert a shared scalar trap kind to the IL VM floating handler trap kind.
/// @param trap Shared semantic trap category produced by checked FP casts.
/// @return IL VM trap kind used for RuntimeBridge diagnostics.
[[nodiscard]] TrapKind fpSemanticTrapToVmTrap(il::semantics::TrapKind trap) {
    switch (trap) {
        case il::semantics::TrapKind::InvalidCast:
            return TrapKind::InvalidCast;
        case il::semantics::TrapKind::Overflow:
            return TrapKind::Overflow;
        case il::semantics::TrapKind::DivideByZero:
            return TrapKind::DivideByZero;
        case il::semantics::TrapKind::Bounds:
            return TrapKind::Bounds;
        case il::semantics::TrapKind::InvalidOperation:
            return TrapKind::InvalidOperation;
        case il::semantics::TrapKind::None:
        case il::semantics::TrapKind::DomainError:
        default:
            return TrapKind::DomainError;
    }
}

/// @brief Round @p operand to the nearest unsigned 64-bit integer or raise a trap.
/// @details Delegates round-to-nearest, ties-to-even and width checking to the
///          shared scalar semantics layer. Invalid inputs and overflow are
///          translated to VM trap categories and reported with the current
///          instruction, function, and block context.
/// @param operand Floating-point value requested for conversion.
/// @param in Instruction descriptor providing source information for diagnostics.
/// @param fr Active frame identifying the function for diagnostics.
/// @param bb Basic block label used in diagnostic output; may be null.
/// @param resultBits Width of the unsigned integer result.
/// @return Rounded unsigned integer when conversion succeeds without trapping.
[[nodiscard]] uint64_t castFpToUiRoundedOrTrap(
    double operand, const Instr &in, Frame &fr, const BasicBlock *bb, int resultBits) {
    constexpr const char *kInvalidOperandMessage = "invalid fp operand in cast.fp_to_ui.rte.chk";
    constexpr const char *kOverflowMessage = "fp overflow in cast.fp_to_ui.rte.chk";

    /// @brief Record one checked-conversion trap in the current execution context.
    /// @param kind VM trap classification.
    /// @param message Stable diagnostic text.
    auto trap = [&](TrapKind kind, const char *message) {
        RuntimeBridge::trap(kind,
                            message,
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    };

    const auto result =
        il::semantics::fpToUiRte(operand, il::semantics::widthFromBits(resultBits));
    if (!result.ok()) {
        trap(fpSemanticTrapToVmTrap(result.trap),
             result.trap == il::semantics::TrapKind::InvalidCast ? kInvalidOperandMessage
                                                                 : kOverflowMessage);
    }
    return static_cast<uint64_t>(result.value);
}
} // namespace

/// @brief Execute the `fadd` opcode by summing two floating-point operands.
/// @details The handler performs three steps:
///          1. Use @ref ops::applyBinary to evaluate both operand slots,
///             ensuring any lazy values are materialised.
///          2. Add the `double` values using host IEEE-754 semantics so NaNs
///             propagate and infinities behave consistently across platforms.
///          3. Store the sum back to the destination slot via
///             @ref ops::storeResult.
///          No control-flow metadata is adjusted because floating-point addition
///          never branches.
/// @param vm Virtual machine orchestrating execution (unused).
/// @param fr Active frame containing operand and result slots.
/// @param in Instruction describing the addition.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFAdd(VM &vm,
                          Frame &fr,
                          const Instr &in,
                          const VM::BlockMap &blocks,
                          const BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Add two floating-point operands.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    /// @return IEEE-754 sum.
    return il::vm::internal::binaryOp<double>(
        vm, fr, in, [](double lhs, double rhs) { return lhs + rhs; });
}

/// @brief Execute the `fsub` opcode by subtracting two floating-point operands.
/// @details Uses @ref ops::applyBinary to materialise operand slots, then
///          computes the difference with host IEEE-754 subtraction so signed
///          zero behaviour and NaN propagation follow the specification.  The
///          only state change is writing the destination slot.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame containing operand and result storage.
/// @param in Instruction describing the subtraction.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFSub(VM &vm,
                          Frame &fr,
                          const Instr &in,
                          const VM::BlockMap &blocks,
                          const BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Subtract one floating-point slot value from another.
    /// @param[out] out Destination slot.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    return ops::applyBinary(vm, fr, in, [](Slot &out, const Slot &lhsVal, const Slot &rhsVal) {
        out.f64 = lhsVal.f64 - rhsVal.f64;
    });
}

/// @brief Execute the `fmul` opcode by multiplying two floating-point operands.
/// @details Delegates to @ref ops::applyBinary to fetch operands, multiplies the
///          values using host IEEE-754 semantics (preserving NaN and infinity
///          behaviour), and stores the product in the destination slot.
///          Control-flow metadata parameters are unused because the operation is
///          purely arithmetic.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the multiplication.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFMul(VM &vm,
                          Frame &fr,
                          const Instr &in,
                          const VM::BlockMap &blocks,
                          const BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Multiply two floating-point slot values.
    /// @param[out] out Destination slot.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    return ops::applyBinary(vm, fr, in, [](Slot &out, const Slot &lhsVal, const Slot &rhsVal) {
        out.f64 = lhsVal.f64 * rhsVal.f64;
    });
}

/// @brief Execute the `fdiv` opcode by dividing two floating-point operands.
/// @details Fetches operands through @ref ops::applyBinary, divides the values
///          using host IEEE-754 semantics (surfacing infinities and NaNs exactly
///          as the specification requires), and stores the quotient in the
///          destination slot.  Control-flow bookkeeping parameters remain
///          untouched because division cannot branch.
/// @param vm Virtual machine orchestrating execution (unused).
/// @param fr Active frame providing operand and destination storage.
/// @param in Instruction describing the division.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFDiv(VM &vm,
                          Frame &fr,
                          const Instr &in,
                          const VM::BlockMap &blocks,
                          const BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Divide two floating-point slot values.
    /// @param[out] out Destination slot.
    /// @param lhsVal Dividend slot.
    /// @param rhsVal Divisor slot.
    return ops::applyBinary(vm, fr, in, [](Slot &out, const Slot &lhsVal, const Slot &rhsVal) {
        out.f64 = lhsVal.f64 / rhsVal.f64;
    });
}

/// @brief Execute the `fcmp.eq` opcode and record whether operands compare equal.
/// @details Evaluates operands via @ref ops::applyCompare, allowing the shared
///          helper to materialise values and write a boolean result.  Equality
///          uses host IEEE-754 rules: NaN operands force a false result while
///          signed zeros compare equal.  The handler writes `1` for equality and
///          `0` otherwise, leaving control-flow metadata untouched.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpEQ(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare two floating-point slots for equality.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 equality result.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return lhsVal.f64 == rhsVal.f64;
    });
}

/// @brief Execute the `fcmp.ne` opcode and record whether operands differ.
/// @details Delegates to @ref ops::applyCompare so operand evaluation and result
///          storage remain centralised.  IEEE-754 semantics mark comparisons
///          with any NaN operand as unordered, which the helper models by
///          returning true (1).  Otherwise the predicate succeeds when the
///          operands are not equal.  Control-flow metadata is unchanged.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpNE(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare two floating-point slots for inequality.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 inequality result.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return lhsVal.f64 != rhsVal.f64;
    });
}

/// @brief Execute the `fcmp.gt` opcode and record whether lhs > rhs.
/// @details Uses @ref ops::applyCompare so operands are evaluated once before
///          applying a strict greater-than check.  IEEE-754 semantics specify
///          that any NaN operand renders the comparison unordered, which this
///          handler treats as false (0).  Only the destination slot is mutated.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpGT(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare whether the left floating-point slot is greater than the right.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 greater-than result.
    return ops::applyCompare(
        vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) { return lhsVal.f64 > rhsVal.f64; });
}

/// @brief Execute the `fcmp.lt` opcode and record whether lhs < rhs.
/// @details Leverages @ref ops::applyCompare to evaluate operands before
///          applying a strict less-than predicate.  Any NaN operand makes the
///          comparison unordered, which results in false (0).  Only the
///          destination slot is modified.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpLT(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare whether the left floating-point slot is less than the right.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 less-than result.
    return ops::applyCompare(
        vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) { return lhsVal.f64 < rhsVal.f64; });
}

/// @brief Execute the `fcmp.le` opcode and record whether lhs <= rhs.
/// @details Relies on @ref ops::applyCompare to evaluate operands, then applies
///          a non-strict comparison.  Any NaN operand produces false (0) because
///          the comparison becomes unordered.  Signed zeros compare as equal,
///          matching IEEE-754 expectations.  Only the destination slot is
///          mutated.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpLE(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare whether the left floating-point slot is at most the right.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 less-than-or-equal result.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return lhsVal.f64 <= rhsVal.f64;
    });
}

/// @brief Execute the `fcmp.ge` opcode and record whether lhs >= rhs.
/// @details Utilises @ref ops::applyCompare to evaluate operands before applying
///          a non-strict greater-than predicate.  NaN operands produce false (0)
///          because the comparison becomes unordered.  Only the destination slot
///          is modified, leaving control-flow metadata untouched.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpGE(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Compare whether the left floating-point slot is at least the right.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return IEEE-754 greater-than-or-equal result.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return lhsVal.f64 >= rhsVal.f64;
    });
}

/// @brief Execute the `fcmp_ord` opcode to test if both operands are ordered.
/// @details Returns true (1) if neither operand is NaN, false (0) otherwise.
///          Uses std::isnan to check each operand value.  Control-flow metadata
///          remains untouched as this is a pure comparison operation.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpOrd(VM &vm,
                             Frame &fr,
                             const Instr &in,
                             const VM::BlockMap &blocks,
                             const BasicBlock *&bb,
                             size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Test whether both floating-point operands are ordered.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return `true` when neither value is NaN.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return !std::isnan(lhsVal.f64) && !std::isnan(rhsVal.f64);
    });
}

/// @brief Execute the `fcmp_uno` opcode to test if either operand is unordered (NaN).
/// @details Returns true (1) if either operand is NaN, false (0) otherwise.
///          Uses std::isnan to check each operand value.  Control-flow metadata
///          remains untouched as this is a pure comparison operation.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the comparison.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleFCmpUno(VM &vm,
                             Frame &fr,
                             const Instr &in,
                             const VM::BlockMap &blocks,
                             const BasicBlock *&bb,
                             size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Test whether either floating-point operand is unordered.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    /// @return `true` when either value is NaN.
    return ops::applyCompare(vm, fr, in, [](const Slot &lhsVal, const Slot &rhsVal) {
        return std::isnan(lhsVal.f64) || std::isnan(rhsVal.f64);
    });
}

/// @brief Execute the `sitofp` opcode by converting a signed 64-bit integer to `double`.
/// @details Uses @ref ops::applyUnary to evaluate the operand slot and stores the
///          converted `double` value.  Host conversion semantics provide the
///          required IEEE-754 rounding behaviour; no additional traps are
///          raised because the IL spec allows the host to round out-of-range
///          integers.  Only the destination slot is mutated.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue.
VM::ExecResult handleSitofp(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    Slot out{};
    out.f64 = static_cast<double>(value.i64);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute the `fptosi` opcode by converting a floating-point value to a
///        signed 64-bit integer with truncation toward zero.
/// @details Per IL spec, traps on NaN or overflow.  Validates that the operand
///          is finite and within the representable range of int64_t before
///          performing the cast.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer (unused).
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue when no trap fires.
VM::ExecResult handleFptosi(VM &vm,
                            Frame &fr,
                            const Instr &in,
                            const VM::BlockMap &blocks,
                            const BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)ip;
    Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    const double operand = value.f64;

    if (!std::isfinite(operand)) {
        RuntimeBridge::trap(TrapKind::InvalidCast,
                            "invalid fp operand in fptosi",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    constexpr double kMin = static_cast<double>(std::numeric_limits<int64_t>::min());
    constexpr double kUpperExclusive = 0x1p63;
    if (operand < kMin || operand >= kUpperExclusive) {
        RuntimeBridge::trap(TrapKind::Overflow,
                            "fp overflow in fptosi",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
        VM::ExecResult result{};
        result.returned = true;
        return result;
    }

    Slot out{};
    out.i64 = static_cast<int64_t>(operand);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute the `cast.fp_to_si.rte.chk` opcode with checked round-to-nearest conversion.
/// @details The workflow mirrors the IL specification:
///          1. Evaluate the operand via @ref VMAccess::eval.
///          2. Use the shared scalar semantics layer to round ties to even at
///             the destination integer width.
///          3. Trap with @ref TrapKind::InvalidCast for non-finite values or
///             @ref TrapKind::Overflow when the rounded value is out of range.
///          4. Store the checked result in the destination slot.
///          All traps flow through @ref RuntimeBridge so diagnostics include
///          instruction and block context.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the checked conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer used for diagnostics.
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue when no trap fires.
VM::ExecResult handleCastFpToSiRteChk(VM &vm,
                                      Frame &fr,
                                      const Instr &in,
                                      const VM::BlockMap &blocks,
                                      const BasicBlock *&bb,
                                      size_t &ip) {
    (void)blocks;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    const double operand = value.f64;
    const auto result =
        il::semantics::fpToSiRte(operand,
                                 il::semantics::widthFromBits(integerTypeBits(in.type.kind)));
    if (result.trap == il::semantics::TrapKind::InvalidCast) {
        RuntimeBridge::trap(fpSemanticTrapToVmTrap(result.trap),
                            "invalid fp operand in cast.fp_to_si.rte.chk",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    }
    if (result.trap == il::semantics::TrapKind::Overflow) {
        RuntimeBridge::trap(fpSemanticTrapToVmTrap(result.trap),
                            "fp overflow in cast.fp_to_si.rte.chk",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    }

    Slot out{};
    out.i64 = result.ok() ? result.value : 0;
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute the `cast.fp_to_ui.rte.chk` opcode with checked round-to-nearest conversion.
/// @details The helper delegates to @ref castFpToUiRoundedOrTrap to enforce the
///          conversion semantics: reject NaNs or negative inputs with
///          @ref TrapKind::InvalidCast, trap on overflow when the rounded value
///          exceeds the destination width, and otherwise return the rounded
///          integer using ties-to-even. The resulting bits are stored in the
///          VM's 64-bit integer slot representation.
/// @param vm Virtual machine coordinating execution (unused).
/// @param fr Active frame providing operand and destination slots.
/// @param in Instruction describing the checked conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current basic block pointer used for diagnostics.
/// @param ip Instruction index within @p bb (unused).
/// @return Execution result signalling the interpreter should continue when no trap fires.
VM::ExecResult handleCastFpToUiRteChk(VM &vm,
                                      Frame &fr,
                                      const Instr &in,
                                      const VM::BlockMap &blocks,
                                      const BasicBlock *&bb,
                                      size_t &ip) {
    (void)blocks;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    const uint64_t rounded =
        castFpToUiRoundedOrTrap(value.f64, in, fr, bb, integerTypeBits(in.type.kind));

    Slot out{};
    out.i64 = static_cast<int64_t>(rounded);
    ops::storeResult(fr, in, out);
    return {};
}

} // namespace il::vm::detail::floating
