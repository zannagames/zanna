//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the integer conversion opcode handlers, including range-checked
// casts, floating-point conversions, and canonical boolean normalisation.  The
// helpers rely on vm/IntOpSupport.hpp to share trap reporting and operand
// evaluation logic across opcode families, keeping the VM semantics aligned with
// the IL specification.
//
//===----------------------------------------------------------------------===//

#include "vm/OpHandlers_Int.hpp"

#include "vm/IntOpSupport.hpp"

#include "il/core/Opcode.hpp"

#include <type_traits>

/// @file
/// @brief Integer conversion opcode handlers used by the VM interpreter.
/// @details The functions in this translation unit convert integers between
///          widths, bridge to floating-point types, and normalise boolean
///          results.  They employ trait structures to share range checks across
///          signed and unsigned variants, emitting traps when conversions would
///          violate IL semantics.

namespace il::vm::detail::integer {
namespace {
/// @brief Traits for range-checking signed integer narrowing conversions.
/// @details Provides helper routines that interpret slot values as signed
///          operands, test whether they fit within narrower types, and compute
///          canonical boolean representations.  The interface is consumed by
///          @ref handleCastNarrowChkImpl so signed and unsigned handlers can
///          share the same implementation body.
struct SignedNarrowCastTraits {
    using WideType = int64_t;
    static constexpr const char *kOutOfRangeMessage = "value out of range in cast.si_narrow.chk";
    static constexpr const char *kUnsupportedTypeMessage =
        "unsupported target type in cast.si_narrow.chk";

    /// @brief Convert the raw slot value into the wide representation.
    /// @param raw Slot value read from the VM frame.
    /// @return Operand interpreted as a signed 64-bit quantity.
    static WideType toWide(int64_t raw) {
        return raw;
    }

    /// @brief Translate the wide representation back into storage form.
    /// @param value Narrowed value in the wide representation.
    /// @return Storage value suitable for writing to a slot.
    static int64_t toStorage(WideType value) {
        return value;
    }

    /// @brief Determine whether the operand fits within a narrower signed type.
    /// @tparam NarrowT Signed integer type to check.
    /// @param value Operand expressed as a signed 64-bit number.
    /// @return True when @p value is representable in @p NarrowT.
    template <typename NarrowT> static bool fits(WideType value) {
        return fitsSignedRange<NarrowT>(value);
    }

    /// @brief Narrow the operand to the requested signed type.
    /// @tparam NarrowT Signed integer type to narrow to.
    /// @param value Operand expressed as a signed 64-bit number.
    /// @return Storage value after narrowing to @p NarrowT.
    template <typename NarrowT> static int64_t narrow(WideType value) {
        return static_cast<int64_t>(static_cast<NarrowT>(value));
    }

    /// @brief Check whether the operand encodes a valid boolean value.
    /// @param value Operand expressed as a signed 64-bit number.
    /// @return True when @p value equals 0 or 1.
    static bool checkBoolean(WideType value) {
        return (value == 0) || (value == 1);
    }

    /// @brief Produce the canonical boolean storage value.
    /// @param value Operand expressed as a signed 64-bit number.
    /// @return Least significant bit suitable for @c i1 storage.
    static int64_t booleanValue(WideType value) {
        return value & 1;
    }
};

/// @brief Traits for range-checking unsigned integer narrowing conversions.
/// @details Mirrors @ref SignedNarrowCastTraits but treats operands as unsigned
///          quantities so range checks follow modulo arithmetic semantics.
struct UnsignedNarrowCastTraits {
    using WideType = uint64_t;
    static constexpr const char *kOutOfRangeMessage = "value out of range in cast.ui_narrow.chk";
    static constexpr const char *kUnsupportedTypeMessage =
        "unsupported target type in cast.ui_narrow.chk";

    /// @brief Convert the raw slot value into the unsigned wide representation.
    /// @param raw Slot value read from the VM frame.
    /// @return Operand interpreted as an unsigned 64-bit quantity.
    static WideType toWide(int64_t raw) {
        return static_cast<WideType>(raw);
    }

    /// @brief Translate the unsigned wide representation back into storage form.
    /// @param value Narrowed value in the unsigned wide representation.
    /// @return Storage value suitable for writing to a slot.
    static int64_t toStorage(WideType value) {
        return static_cast<int64_t>(value);
    }

    /// @brief Determine whether the operand fits within a narrower unsigned type.
    /// @tparam NarrowT Unsigned integer type to check.
    /// @param value Operand expressed as an unsigned 64-bit number.
    /// @return True when @p value is representable in @p NarrowT.
    template <typename NarrowT> static bool fits(WideType value) {
        return fitsUnsignedRange<NarrowT>(value);
    }

    /// @brief Narrow the operand to the requested unsigned type.
    /// @tparam NarrowT Unsigned integer type to narrow to.
    /// @param value Operand expressed as an unsigned 64-bit number.
    /// @return Storage value after narrowing to @p NarrowT.
    template <typename NarrowT> static int64_t narrow(WideType value) {
        return static_cast<int64_t>(static_cast<NarrowT>(value));
    }

    /// @brief Check whether the operand encodes a valid boolean value.
    /// @param value Operand expressed as an unsigned 64-bit number.
    /// @return True when @p value equals 0 or 1.
    static bool checkBoolean(WideType value) {
        return value <= 1;
    }

    /// @brief Produce the canonical boolean storage value.
    /// @param value Operand expressed as an unsigned 64-bit number.
    /// @return Least significant bit suitable for @c i1 storage.
    static int64_t booleanValue(WideType value) {
        return static_cast<int64_t>(value & 1);
    }
};

/// @brief Shared implementation for @c cast.*_narrow.chk opcodes.
/// @details Converts the operand through the trait-supplied helpers, performing
///          range checks, boolean validation, and unsupported-type detection.  On
///          failure the function emits the trait-defined diagnostic message and
///          returns immediately so callers propagate the trapped state.
/// @tparam Traits Trait structure providing signed or unsigned semantics.
/// @param value Operand already evaluated from the VM frame.
/// @param fr Active frame receiving the conversion result.
/// @param in Instruction describing the conversion target type.
/// @param bb Pointer to the current basic block for diagnostics.
/// @return Execution result describing whether interpretation should continue.
template <typename Traits>
VM::ExecResult handleCastNarrowChkImpl(const Slot &value,
                                       Frame &fr,
                                       const il::core::Instr &in,
                                       const il::core::BasicBlock *bb) {
    switch (in.type.kind) {
        case il::core::Type::Kind::I1:
        case il::core::Type::Kind::I16:
        case il::core::Type::Kind::I32:
        case il::core::Type::Kind::I64:
            break;
        default:
            emitTrap(TrapKind::InvalidCast, Traits::kUnsupportedTypeMessage, in, fr, bb);
            return {};
    }

    /// @brief Apply the signed or unsigned checked-narrowing semantics selected by `Traits`.
    /// @return Narrowed semantic result with an explicit trap classification.
    const auto result =
        [&]() -> il::semantics::SemanticResult<int64_t> {
        if constexpr (std::is_same_v<Traits, UnsignedNarrowCastTraits>) {
            return il::semantics::unsignedNarrow(static_cast<uint64_t>(value.i64),
                                                 semanticWidthForType(in.type.kind));
        } else {
            return il::semantics::signedNarrow(value.i64, semanticWidthForType(in.type.kind));
        }
    }();

    if (!result.ok()) {
        emitTrap(TrapKind::InvalidCast, Traits::kOutOfRangeMessage, in, fr, bb);
        return {};
    }

    Slot out{};
    out.i64 = result.value;
    ops::storeResult(fr, in, out);
    return {};
}
} // namespace

/// @brief Execute the @c cast.si_narrow.chk opcode.
/// @details Evaluates the operand and forwards it to
///          @ref handleCastNarrowChkImpl using signed traits so overflow traps
///          and boolean checks follow signed semantics.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame providing operand and result storage.
/// @param in Instruction describing the narrowing conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer for diagnostics.
/// @param ip Instruction index within the block (unused).
/// @return Execution result describing whether interpretation should continue.
VM::ExecResult handleCastSiNarrowChk(VM &vm,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     const VM::BlockMap &blocks,
                                     const il::core::BasicBlock *&bb,
                                     size_t &ip) {
    (void)blocks;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    return handleCastNarrowChkImpl<SignedNarrowCastTraits>(value, fr, in, bb);
}

/// @brief Execute the @c cast.ui_narrow.chk opcode.
/// @details Mirrors @ref handleCastSiNarrowChk but interprets operands as
///          unsigned quantities so range checks honour modulo semantics.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame providing operand and result storage.
/// @param in Instruction describing the narrowing conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer for diagnostics.
/// @param ip Instruction index within the block (unused).
/// @return Execution result describing whether interpretation should continue.
VM::ExecResult handleCastUiNarrowChk(VM &vm,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     const VM::BlockMap &blocks,
                                     const il::core::BasicBlock *&bb,
                                     size_t &ip) {
    (void)blocks;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    return handleCastNarrowChkImpl<UnsignedNarrowCastTraits>(value, fr, in, bb);
}

/// @brief Execute the @c cast.si_to_fp opcode.
/// @details Converts a signed integer operand to double precision and stores the
///          resulting floating-point value in the destination slot.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame providing operand and result storage.
/// @param in Instruction describing the conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result describing whether interpretation should continue.
VM::ExecResult handleCastSiToFp(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    Slot out{};
    out.f64 = static_cast<double>(value.i64);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute the @c cast.ui_to_fp opcode.
/// @details Interprets the operand as unsigned before converting to
///          double-precision floating point, preserving modulo semantics for the
///          source range.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame providing operand and result storage.
/// @param in Instruction describing the conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result describing whether interpretation should continue.
VM::ExecResult handleCastUiToFp(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    const Slot value = VMAccess::eval(vm, fr, in.operands[0]);
    const uint64_t operand = static_cast<uint64_t>(value.i64);
    Slot out{};
    out.f64 = static_cast<double>(operand);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Execute either the @c trunc.1 or @c zext.1 opcode.
/// @details Normalises the operand into the canonical boolean domain.  Truncation
///          masks off the least significant bit, while zero-extension maps any
///          non-zero operand to @c 1.  The helper inspects the opcode to select
///          the desired behaviour before writing the result slot.
/// @param vm Virtual machine instance executing the instruction.
/// @param fr Active frame providing operand and result storage.
/// @param in Instruction describing the boolean conversion.
/// @param blocks Map of basic blocks for the current function (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction index within the block (unused).
/// @return Execution result describing whether interpretation should continue.
VM::ExecResult handleTruncOrZext1(VM &vm,
                                  Frame &fr,
                                  const il::core::Instr &in,
                                  const VM::BlockMap &blocks,
                                  const il::core::BasicBlock *&bb,
                                  size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    Slot operand = VMAccess::eval(vm, fr, in.operands[0]);
    const bool truthy = operand.i64 != 0;

    Slot result{};
    switch (in.op) {
        case il::core::Opcode::Trunc1:
            result.i64 = operand.i64 & 1;
            break;
        case il::core::Opcode::Zext1:
            result.i64 = truthy ? 1 : 0;
            break;
        default:
            result = operand;
            break;
    }

    ops::storeResult(fr, in, result);
    return {};
}
} // namespace il::vm::detail::integer
