//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/Marshal.cpp
// Purpose: Implement conversions between VM value wrappers and runtime bridge types.
// Ownership/Lifetime: Returned views borrow storage from runtime-managed strings.
// Links: docs/runtime-vm.md#marshalling
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides helpers for converting values between VM and runtime layers.
/// @details Collects the string and scalar conversion routines used by opcode
///          handlers so that ownership semantics and error handling remain
///          consistent across the VM.

#include "vm/Marshal.hpp"

#include "zanna/runtime/rt.h"
#include "vm/DiagFormat.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VM.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace il::vm {

namespace {
using il::core::Type;

/// @brief Type-erased access operations for one runtime-supported IL kind.
/// @details Each entry exposes argument storage, result storage, and assignment
///          operations without repeating a type-kind switch at every bridge
///          call.
struct KindAccessors {
    /// @brief Function type returning a pointer to a slot member.
    /// @param slot Slot whose typed member is requested.
    /// @return Erased pointer to the selected member.
    using SlotAccessor = void *(*)(Slot &);
    /// @brief Function type returning a pointer to a result-buffer member.
    /// @param buffers Result buffers whose typed member is requested.
    /// @return Erased pointer to the selected member.
    using ResultAccessor = void *(*)(ResultBuffers &);
    /// @brief Function type copying a result-buffer member into a slot.
    /// @param[out] slot Destination slot.
    /// @param buffers Source result buffers.
    using ResultAssigner = void (*)(Slot &, const ResultBuffers &);

    SlotAccessor slotAccessor = nullptr; ///< Argument-storage accessor.
    ResultAccessor resultAccessor = nullptr; ///< Return-storage accessor.
    ResultAssigner assignResult = nullptr; ///< Return-to-slot assignment.
};

/// @brief Contiguous set of IL kinds represented by @ref kKindAccessors.
constexpr std::array<Type::Kind, 10> kSupportedKinds = {
    Type::Kind::Void,
    Type::Kind::I1,
    Type::Kind::I16,
    Type::Kind::I32,
    Type::Kind::I64,
    Type::Kind::F64,
    Type::Kind::Ptr,
    Type::Kind::Str,
    Type::Kind::Error,
    Type::Kind::ResumeTok,
};

static_assert(kSupportedKinds.size() == 10, "update kind accessors when Type::Kind grows");

/// @brief Represent a void result with no writable return buffer.
/// @details The supplied result-buffer object is intentionally ignored.
/// @return Always @c nullptr.
constexpr void *nullResultBuffer(ResultBuffers &) {
    return nullptr;
}

/// @brief Ignore assignment for kinds that carry no runtime value.
/// @details Both the destination slot and source buffers are intentionally
///          unused.
constexpr void assignNoop(Slot &, const ResultBuffers &) {}

/// @brief Obtain a type-erased pointer to a selected @ref Slot member.
/// @tparam Member Pointer to the slot data member exposed to the runtime.
/// @param slot Slot containing the selected member.
/// @return Address of the selected member inside @p slot.
template <auto Member> constexpr void *slotMemberAccessor(Slot &slot) {
    return static_cast<void *>(&(slot.*Member));
}

/// @brief Obtain a type-erased pointer to a selected result-buffer member.
/// @tparam Member Pointer to the result data member exposed to the runtime.
/// @param buffers Result storage containing the selected member.
/// @return Address of the selected member inside @p buffers.
template <auto Member> constexpr void *bufferMemberAccessor(ResultBuffers &buffers) {
    return static_cast<void *>(&(buffers.*Member));
}

/// @brief Copy a selected result-buffer member into a selected slot member.
/// @tparam SlotMember Pointer to the destination @ref Slot member.
/// @tparam BufferMember Pointer to the source @ref ResultBuffers member.
/// @param slot Destination VM slot.
/// @param buffers Source runtime result storage.
template <auto SlotMember, auto BufferMember>
constexpr void assignFromBuffer(Slot &slot, const ResultBuffers &buffers) {
    slot.*SlotMember = buffers.*BufferMember;
}

/// @brief Build an accessor entry for a non-value-bearing IL kind.
/// @return Accessors with no argument storage, a null result buffer, and
///         no-op assignment.
constexpr KindAccessors makeVoidAccessors() {
    return KindAccessors{nullptr, &nullResultBuffer, &assignNoop};
}

/// @brief Build an accessor entry for corresponding slot and result members.
/// @tparam SlotMember Pointer to the VM slot member.
/// @tparam BufferMember Pointer to the runtime result-buffer member.
/// @return Fully populated type-erased accessor entry.
template <auto SlotMember, auto BufferMember> constexpr KindAccessors makeAccessors() {
    return KindAccessors{
        &slotMemberAccessor<SlotMember>,
        &bufferMemberAccessor<BufferMember>,
        &assignFromBuffer<SlotMember, BufferMember>,
    };
}

/// @brief Accessor table indexed by the underlying value of @ref Type::Kind.
/// @details Its immediately invoked initializer builds one accessor entry for
///          every supported kind and returns the completed table.
constexpr std::array<KindAccessors, kSupportedKinds.size()> kKindAccessors = [] {
    std::array<KindAccessors, kSupportedKinds.size()> table{};
    table[static_cast<size_t>(Type::Kind::Void)] = makeVoidAccessors();
    table[static_cast<size_t>(Type::Kind::I1)] = makeAccessors<&Slot::i64, &ResultBuffers::i64>();
    table[static_cast<size_t>(Type::Kind::I16)] = makeAccessors<&Slot::i64, &ResultBuffers::i64>();
    table[static_cast<size_t>(Type::Kind::I32)] = makeAccessors<&Slot::i64, &ResultBuffers::i64>();
    table[static_cast<size_t>(Type::Kind::I64)] = makeAccessors<&Slot::i64, &ResultBuffers::i64>();
    table[static_cast<size_t>(Type::Kind::F64)] = makeAccessors<&Slot::f64, &ResultBuffers::f64>();
    table[static_cast<size_t>(Type::Kind::Ptr)] = makeAccessors<&Slot::ptr, &ResultBuffers::ptr>();
    table[static_cast<size_t>(Type::Kind::Str)] = makeAccessors<&Slot::str, &ResultBuffers::str>();
    table[static_cast<size_t>(Type::Kind::Error)] = makeVoidAccessors();
    table[static_cast<size_t>(Type::Kind::ResumeTok)] = makeVoidAccessors();
    return table;
}();

/// @brief Select the accessor entry for an IL type kind.
/// @param kind Supported type kind used as the table index.
/// @return Immutable accessor entry associated with @p kind.
/// @pre @p kind must fall within the contiguous supported-kind table.
const KindAccessors &dispatchFor(Type::Kind kind) {
    const auto index = static_cast<size_t>(kind);
    assert(index < kKindAccessors.size() && "invalid type kind");
    return kKindAccessors[index];
}

} // namespace

/// @brief Convert an immutable VM string view into a runtime handle.
/// @details Preserves the `nullptr` sentinel used throughout the VM to mean "no
///          string" and reuses the runtime's constant-string fast path when the
///          input has no embedded NULs.  Otherwise a fresh runtime allocation
///          mirrors the byte sequence so handlers can safely share the returned
///          handle.
///
///          HIGH-7 optimization: When AssumeNullTerminated::Yes, skip the O(n)
///          scan for embedded NULs and use the fast rt_const_cstr path directly.
///          This is safe for module string literals which are guaranteed to be
///          NUL-terminated without embedded NULs.
///
/// @param text Non-owning reference to the source character range.
/// @param assumeNullTerminated When Yes, skip embedded NUL check (fast path).
/// @return Runtime handle suitable for passing to C helpers; may be null when
///         @p text lacks backing storage.
ZannaString toZannaString(StringRef text, AssumeNullTerminated assumeNullTerminated) {
    if (text.data() == nullptr)
        return nullptr;
    if (text.empty())
        return rt_string_from_bytes(text.data(), 0);

    // Fast path: caller guarantees NUL-terminated without embedded NULs
    if (assumeNullTerminated == AssumeNullTerminated::Yes)
        return rt_const_cstr(text.data());

    // Check for embedded NUL using memchr (often optimized with SIMD)
    // This is faster than string_view::find on many platforms
    const void *nulPos = std::memchr(text.data(), '\0', text.size());
    if (nulPos != nullptr) {
        // String contains embedded NUL - must use byte copy
        return rt_string_from_bytes(text.data(), text.size());
    }

    // No embedded NUL found - can use const wrapper if properly terminated
    // However, since we don't know if there's a NUL terminator after text.size(),
    // we must copy to ensure safety
    return rt_string_from_bytes(text.data(), text.size());
}

/// @brief Convert a runtime string handle back into the VM's view type.
/// @details Valid runtime handles expose a contiguous UTF-8 byte sequence and
///          length via the C ABI helpers.  The returned @ref StringRef borrows
///          that storage without taking ownership, so callers must ensure the
///          runtime string outlives the view.  Null or invalid handles produce
///          an empty view and, in the negative-length case, raise a runtime trap.
/// @param str Runtime string handle to translate.
/// @return Non-owning view of the runtime string's contents, or an empty view
///         when the handle is null.
StringRef fromZannaString(const ZannaString &str) {
    if (!str)
        return {};
    const char *data = rt_string_cstr(str);
    if (!data)
        return {};
    const int64_t length = rt_str_len(str);
    if (length < 0) {
        RuntimeBridge::trap(
            TrapKind::DomainError, "rt_string reported negative length", {}, "", "");
        return {};
    }
    if (!detail::lengthWithinLimit(length, kMaxBridgeStringBytes)) {
        if (RuntimeBridge::hasActiveVm()) {
            RuntimeBridge::trap(
                TrapKind::DomainError, "rt_string length exceeds bridge limit", {}, "", "");
        }
        return {};
    }
    if (!detail::lengthWithinLimit(length, std::numeric_limits<size_t>::max()))
        return {};
    return StringRef{data, static_cast<size_t>(length)};
}

//===----------------------------------------------------------------------===//
// Constant Scalar Conversion Helpers
//===----------------------------------------------------------------------===//
//
// These functions are intentionally restricted to constant values (ConstInt,
// ConstFloat, NullPtr). They are NOT general-purpose coercions.
//
// The precondition isConstantScalar(value) is checked via assertion. Callers
// should verify the value kind before calling if the kind is not statically
// known. This keeps the hot path cheap (no branch on success) while catching
// programmer errors during development.
//
//===----------------------------------------------------------------------===//

namespace {
/// @brief Convert Value::Kind to a diagnostic string.
/// @param kind The value kind to describe.
/// @return Human-readable name for error messages.
constexpr const char *valueKindToString(il::core::Value::Kind kind) noexcept {
    using Kind = il::core::Value::Kind;
    switch (kind) {
        case Kind::Temp:
            return "Temp";
        case Kind::ConstInt:
            return "ConstInt";
        case Kind::ConstFloat:
            return "ConstFloat";
        case Kind::ConstStr:
            return "ConstStr";
        case Kind::GlobalAddr:
            return "GlobalAddr";
        case Kind::NullPtr:
            return "NullPtr";
    }
    return "Unknown";
}
} // namespace

/// @brief Convert an IL constant scalar to a signed 64-bit VM value.
/// @details Integer constants are preserved, floating constants use the C++
///          numeric conversion, and null pointers become zero.  Other value
///          kinds indicate a caller error and route through the runtime trap
///          mechanism.
/// @param value Constant integer, floating-point, or null-pointer value.
/// @return Converted signed integer, or zero after reporting an invalid kind.
/// @pre @ref isConstantScalar must hold for @p value.
int64_t toI64(const il::core::Value &value) {
    using Kind = il::core::Value::Kind;

    // Precondition: value must be a constant scalar.
    // This assertion catches programmer errors where toI64 is called on
    // non-constant values like Temp, ConstStr, or GlobalAddr.
    assert(isConstantScalar(value) &&
           "toI64 requires a constant scalar value (ConstInt, ConstFloat, or NullPtr)");

    switch (value.kind) {
        case Kind::ConstInt:
            return static_cast<int64_t>(value.i64);
        case Kind::ConstFloat:
            return static_cast<int64_t>(value.f64);
        case Kind::NullPtr:
            return 0;
        default:
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                std::string("toI64 called with non-constant value kind: ") +
                                    valueKindToString(value.kind),
                                {},
                                "",
                                "");
            return 0;
    }
}

/// @brief Convert an IL constant scalar to a double-precision VM value.
/// @details Floating constants are preserved, integers are converted
///          numerically, and null pointers become zero.  Unsupported value kinds
///          are reported through the runtime trap mechanism.
/// @param value Constant floating-point, integer, or null-pointer value.
/// @return Converted floating-point value, or zero after reporting an invalid
///         kind.
/// @pre @ref isConstantScalar must hold for @p value.
double toF64(const il::core::Value &value) {
    using Kind = il::core::Value::Kind;

    // Precondition: value must be a constant scalar.
    // This assertion catches programmer errors where toF64 is called on
    // non-constant values like Temp, ConstStr, or GlobalAddr.
    assert(isConstantScalar(value) &&
           "toF64 requires a constant scalar value (ConstInt, ConstFloat, or NullPtr)");

    switch (value.kind) {
        case Kind::ConstFloat:
            return value.f64;
        case Kind::ConstInt:
            return static_cast<double>(value.i64);
        case Kind::NullPtr:
            return 0.0;
        default:
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                std::string("toF64 called with non-constant value kind: ") +
                                    valueKindToString(value.kind),
                                {},
                                "",
                                "");
            return 0.0;
    }
}

/// @brief Expose a VM slot member as runtime argument storage.
/// @param slot Slot containing the argument value.
/// @param kind IL type selecting which slot member is exposed.
/// @return Pointer to the selected member, or @c nullptr after reporting an
///         unsupported kind.
void *slotToArgPointer(Slot &slot, il::core::Type::Kind kind) {
    const auto &entry = dispatchFor(kind);
    if (!entry.slotAccessor) {
        RuntimeBridge::trap(
            TrapKind::InvalidOperation, diag::formatUnsupportedKind("argument", kind), {}, "", "");
        return nullptr;
    }
    return entry.slotAccessor(slot);
}

/// @brief Select writable runtime return storage for an IL type kind.
/// @param kind Return type whose backing member is requested.
/// @param buffers Aggregate storage for all supported runtime result forms.
/// @return Pointer to the selected result member; @c nullptr for void-like
///         kinds or after reporting an unsupported kind.
void *resultBufferFor(il::core::Type::Kind kind, ResultBuffers &buffers) {
    const auto &entry = dispatchFor(kind);
    if (!entry.resultAccessor) {
        RuntimeBridge::trap(
            TrapKind::InvalidOperation, diag::formatUnsupportedKind("return", kind), {}, "", "");
        return nullptr;
    }
    return entry.resultAccessor(buffers);
}

/// @brief Copy a runtime result buffer into a VM slot.
/// @param slot Destination slot.
/// @param kind IL return type selecting the source and destination members.
/// @param buffers Runtime result storage populated by the external call.
void assignResult(Slot &slot, il::core::Type::Kind kind, const ResultBuffers &buffers) {
    const auto &entry = dispatchFor(kind);
    if (!entry.assignResult) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            diag::formatUnsupportedKind("assign return", kind),
                            {},
                            "",
                            "");
        return;
    }
    entry.assignResult(slot, buffers);
}

/// @brief Core marshalling logic shared by both inline and vector variants.
/// @tparam OutputArray Type providing operator[] and size/capacity.
/// @param sig Runtime signature describing parameter types.
/// @param args Argument slots to marshal.
/// @param [in,out] powStatus Power-function status tracker initialized when a
///        hidden status pointer is required.
/// @param [out] output Array that receives pointers to argument storage.
/// @param totalArgs Total number of arguments (params + hidden).
template <typename OutputArray>
static void marshalArgumentsCore(const il::runtime::RuntimeSignature &sig,
                                 std::span<Slot> args,
                                 PowStatus &powStatus,
                                 OutputArray &output,
                                 std::size_t totalArgs) {
    (void)totalArgs; // Used for documentation/debugging

    for (size_t i = 0; i < sig.paramTypes.size(); ++i) {
        auto kind = sig.paramTypes[i].kind;
        Slot &slot = args[i];
        output[i] = slotToArgPointer(slot, kind);
    }

    size_t hiddenIndex = sig.paramTypes.size();
    for (const auto &hidden : sig.hiddenParams) {
        switch (hidden.kind) {
            case il::runtime::RuntimeHiddenParamKind::None:
                output[hiddenIndex++] = nullptr;
                break;
            case il::runtime::RuntimeHiddenParamKind::PowStatusPointer:
                powStatus.active = true;
                powStatus.ok = true;
                powStatus.ptr = &powStatus.ok;
                // Pow helpers expect a pointer to the status pointer so they can swap
                // it for a runtime-managed location when traps must propagate.
                output[hiddenIndex++] = &powStatus.ptr;
                break;
        }
    }
}

/// @brief Marshal runtime arguments into inline storage when capacity permits.
/// @details Selects the fixed buffer for ordinary signatures and a vector for
///          argument lists larger than @ref kMaxStackMarshalArgs, recording the
///          selected representation and total count in @p result.
/// @param sig Runtime signature describing visible and hidden parameters.
/// @param args Mutable VM slots backing visible argument pointers.
/// @param [in,out] powStatus Hidden power-status state when requested.
/// @param [out] result Marshalled argument container populated by the function.
void marshalArgumentsInline(const il::runtime::RuntimeSignature &sig,
                            std::span<Slot> args,
                            PowStatus &powStatus,
                            MarshalledArgs &result) {
    const std::size_t totalArgs = sig.paramTypes.size() + sig.hiddenParams.size();
    result.count = totalArgs;

    if (totalArgs <= kMaxStackMarshalArgs) {
        // Fast path: use inline storage (no heap allocation)
        result.usingHeap = false;
        marshalArgumentsCore(sig, args, powStatus, result.inlineBuffer, totalArgs);
    } else {
        // Slow path: fall back to heap allocation for large argument lists
        result.usingHeap = true;
        result.heapBuffer.resize(totalArgs);
        marshalArgumentsCore(sig, args, powStatus, result.heapBuffer, totalArgs);
    }
}

/// @brief Marshal visible and hidden arguments into an owning pointer vector.
/// @param sig Runtime signature describing visible and hidden parameters.
/// @param args Mutable VM slots backing visible argument pointers.
/// @param [in,out] powStatus Hidden power-status state when requested.
/// @return Pointer vector ordered according to the runtime ABI signature.
std::vector<void *> marshalArguments(const il::runtime::RuntimeSignature &sig,
                                     std::span<Slot> args,
                                     PowStatus &powStatus) {
    const std::size_t totalArgs = sig.paramTypes.size() + sig.hiddenParams.size();
    std::vector<void *> rawArgs(totalArgs);
    marshalArgumentsCore(sig, args, powStatus, rawArgs, totalArgs);
    return rawArgs;
}

/// @brief Classify a checked-power runtime failure as domain or overflow.
/// @details Examines the hidden status location first, then treats a non-finite
///          floating result as overflow.  A negative base with a fractional
///          exponent is classified as a domain error.
/// @param desc Runtime descriptor whose trap class and return type are checked.
/// @param powStatus Hidden status state populated by the power helper.
/// @param args Original VM argument slots containing base and exponent.
/// @param buffers Runtime result storage containing the computed value.
/// @return Triggered trap outcome with kind and message, or a default
///         non-triggered outcome when no checked-power failure occurred.
PowTrapOutcome classifyPowTrap(const il::runtime::RuntimeDescriptor &desc,
                               const PowStatus &powStatus,
                               std::span<const Slot> args,
                               const ResultBuffers &buffers) {
    PowTrapOutcome outcome{};
    if (desc.trapClass != il::runtime::RuntimeTrapClass::PowDomainOverflow || !powStatus.active)
        return outcome;

    bool okStatus = powStatus.ok;
    if (powStatus.ptr) {
        if (powStatus.ptr == &powStatus.ok) {
            okStatus = powStatus.ok;
        } else {
            okStatus = *powStatus.ptr;
        }
    }

    if (!okStatus) {
        const double base = !args.empty() ? args[0].f64 : 0.0;
        const double exp = (args.size() > 1) ? args[1].f64 : 0.0;
        const bool expIntegral = std::isfinite(exp) && (exp == std::trunc(exp));
        const bool domainError = (base < 0.0) && !expIntegral;

        outcome.triggered = true;
        if (domainError) {
            outcome.kind = TrapKind::DomainError;
            outcome.message = "rt_pow_f64_chkdom: negative base with fractional exponent";
        } else {
            outcome.kind = TrapKind::Overflow;
            outcome.message = "rt_pow_f64_chkdom: overflow";
        }
        return outcome;
    }

    if (desc.signature.retType.kind == il::core::Type::Kind::F64 && !std::isfinite(buffers.f64)) {
        outcome.triggered = true;
        outcome.kind = TrapKind::Overflow;
        outcome.message = "rt_pow_f64_chkdom: overflow";
    }

    return outcome;
}

/// @brief Materialize a runtime call result as a new VM slot.
/// @param signature Runtime signature whose return kind selects the assignment.
/// @param buffers Result storage populated by the runtime helper.
/// @return Value-initialized slot containing the selected result member.
Slot assignCallResult(const il::runtime::RuntimeSignature &signature,
                      const ResultBuffers &buffers) {
    Slot destination{};
    assignResult(destination, signature.retType.kind, buffers);
    return destination;
}

//===----------------------------------------------------------------------===//
// Marshalling Validation Helpers
//===----------------------------------------------------------------------===//

/// @brief Validate an argument count against a runtime descriptor.
/// @param desc Descriptor providing signature and diagnostic name.
/// @param argCount Number of visible VM arguments supplied.
/// @return Successful validation or a descriptive arity error.
MarshalValidation validateMarshalArity(const il::runtime::RuntimeDescriptor &desc,
                                       std::size_t argCount) {
    return validateMarshalArity(desc.signature, argCount, desc.name);
}

/// @brief Validate an argument count against an explicit signature.
/// @details Produces an owning diagnostic that identifies expected and supplied
///          counts and notes excess operands when present.
/// @param sig Signature whose visible parameter count is authoritative.
/// @param argCount Number of visible VM arguments supplied.
/// @param calleeName Name prefixed to a validation failure.
/// @return Successful validation or a descriptive arity error.
MarshalValidation validateMarshalArity(const il::runtime::RuntimeSignature &sig,
                                       std::size_t argCount,
                                       std::string_view calleeName) {
    MarshalValidation result;
    const auto expected = sig.paramTypes.size();
    if (argCount != expected) {
        result.ok = false;
        result.errorMessage.reserve(calleeName.size() + 64);
        result.errorMessage.append(calleeName);
        result.errorMessage.append(": expected ");
        result.errorMessage.append(std::to_string(expected));
        result.errorMessage.append(" argument(s), got ");
        result.errorMessage.append(std::to_string(argCount));
        if (argCount > expected)
            result.errorMessage.append(" (excess runtime operands)");
    }
    return result;
}

/// @brief Validate runtime-call arity and optionally reject null pointers.
/// @param desc Descriptor defining visible parameter kinds.
/// @param args VM arguments to inspect.
/// @param checkNullPointers Whether pointer-typed arguments must be non-null.
/// @return Successful validation or the first arity/null-pointer failure.
MarshalValidation validateMarshalArgs(const il::runtime::RuntimeDescriptor &desc,
                                      std::span<const Slot> args,
                                      bool checkNullPointers) {
    // First check arity
    MarshalValidation result = validateMarshalArity(desc, args.size());
    if (!result.ok)
        return result;

    // Optionally validate pointer arguments
    if (checkNullPointers) {
        const auto &sig = desc.signature;
        for (std::size_t i = 0; i < sig.paramTypes.size() && i < args.size(); ++i) {
            if (sig.paramTypes[i].kind == il::core::Type::Kind::Ptr) {
                if (args[i].ptr == nullptr) {
                    result.ok = false;
                    result.errorMessage.reserve(desc.name.size() + 48);
                    result.errorMessage.append(desc.name);
                    result.errorMessage.append(": null pointer argument at index ");
                    result.errorMessage.append(std::to_string(i));
                    return result;
                }
            }
        }
    }

    return result;
}

/// @brief Validate and marshal a runtime call in one operation.
/// @details Prevents out-of-bounds marshalling by checking the visible argument
///          count before delegating to @ref marshalArguments.
/// @param desc Runtime descriptor defining signature and validation rules.
/// @param args Mutable VM slots backing runtime argument pointers.
/// @param [in,out] powStatus Hidden power-status state when requested.
/// @param [out] validation Receives success or the validation diagnostic.
/// @param checkNullPointers Whether pointer-typed arguments must be non-null.
/// @return Marshalled pointer vector on success, or an empty vector on
///         validation failure.
std::vector<void *> marshalArgumentsValidated(const il::runtime::RuntimeDescriptor &desc,
                                              std::span<Slot> args,
                                              PowStatus &powStatus,
                                              MarshalValidation &validation,
                                              bool checkNullPointers) {
    // Validate before marshalling to avoid out-of-bounds access
    validation = validateMarshalArgs(
        desc, std::span<const Slot>{args.data(), args.size()}, checkNullPointers);
    if (!validation.ok)
        return {};

    // Delegate to existing marshalling logic
    return marshalArguments(desc.signature, args, powStatus);
}

} // namespace il::vm
