//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/Marshal.hpp
// Purpose: Declares helpers for converting between VM and runtime data types.
// Key invariants: Conversion helpers preserve existing runtime encodings.
// Ownership/Lifetime: Views returned do not extend the lifetime of underlying data.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares value, string, argument, and return marshalling between the
///        VM and runtime bridge.
/// @details The API preserves VM slot encodings, exposes bounded borrowed string
///          views, manages hidden checked-power status arguments, and validates
///          external-call arity before producing the runtime @c void** ABI.

#pragma once

#include "il/core/Value.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "zanna/runtime/rt.h"
#include "vm/Trap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace il::vm {

/// @brief Non-owning character view used by VM string values.
using StringRef = std::string_view;
/// @brief Opaque string handle managed by the C runtime.
using ZannaString = ::rt_string;

/// @brief Indicates whether a string view is guaranteed to be null-terminated.
enum class AssumeNullTerminated : bool {
    No = false, ///< Do not assume storage has a trailing null byte.
    Yes = true, ///< Caller guarantees safe null-terminated backing storage.
};

union Slot;

namespace detail {
/// @brief Check whether a runtime-provided string length fits within @p limit.
/// @param length Length reported by the runtime as a signed 64-bit value.
/// @param limit Maximum representable size for the destination view type.
/// @return @c true when @p length is non-negative and does not exceed @p limit.
/// @note Constexpr for compile-time validation when possible.
[[nodiscard]] constexpr bool lengthWithinLimit(int64_t length, uint64_t limit) noexcept {
    return length >= 0 && static_cast<uint64_t>(length) <= limit;
}
} // namespace detail

/// @brief Maximum number of bytes the VM is willing to expose from a runtime string.
/// @details Strings larger than this limit are treated as invalid to avoid allocating
///          unbounded host buffers when marshalling corrupted runtime handles.
inline constexpr uint64_t kMaxBridgeStringBytes =
    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());

/// @brief Stable storage for every runtime return representation.
/// @details A bridge call selects one member according to its declared return
///          kind and later copies that member into a VM slot.
struct ResultBuffers {
    int64_t i64 = 0; ///< Integer and boolean result storage.
    double f64 = 0.0; ///< Floating-point result storage.
    ZannaString str = nullptr; ///< Runtime string-handle result storage.
    void *ptr = nullptr; ///< Opaque pointer result storage.
};

/// @brief State for the hidden status argument used by checked power helpers.
struct PowStatus {
    bool active{false}; ///< Whether the runtime signature requested status.
    bool ok{true}; ///< Inline success flag initialized before the call.
    bool *ptr{nullptr}; ///< Active flag location, which the runtime may replace.
};

/// @brief Classification produced after inspecting a checked power call.
struct PowTrapOutcome {
    bool triggered{false}; ///< Whether the completed call must trap.
    TrapKind kind{TrapKind::RuntimeError}; ///< Trap category when triggered.
    std::string message; ///< Owning diagnostic text when triggered.
};

/// @brief Convert a host string view into a runtime string handle.
/// @param text Source string view to convert.
/// @param assumeNullTerminated Hint indicating whether @p text is null-terminated,
///        allowing the implementation to skip a copy when possible.
/// @return Runtime string handle wrapping the converted text.
ZannaString toZannaString(StringRef text,
                          AssumeNullTerminated assumeNullTerminated = AssumeNullTerminated::No);

/// @brief Extract a host string view from a runtime string handle.
/// @param str Runtime string handle to read from.
/// @return String view referencing the handle's character data. The view does
///         not extend the lifetime of the underlying buffer.
StringRef fromZannaString(const ZannaString &str);

//===----------------------------------------------------------------------===//
// Constant Scalar Conversion Helpers
//===----------------------------------------------------------------------===//
//
// The following functions convert IL constant values to C++ scalar types.
// They are intentionally restricted to "constant scalar" kinds:
//   - ConstInt:   integer literal
//   - ConstFloat: floating-point literal
//   - NullPtr:    null pointer (treated as zero)
//
// These are NOT general-purpose coercions. Using them on Temp, ConstStr, or
// GlobalAddr values is a programmer error and will abort in debug builds.
// This restriction exists to catch misuse early and ensure new Value::Kind
// variants are handled explicitly rather than silently converting.
//
//===----------------------------------------------------------------------===//

/// @brief Check if a Value::Kind represents a constant that can be converted
///        to a scalar numeric type (i64 or f64).
/// @param kind The value kind to check.
/// @return True for ConstInt, ConstFloat, and NullPtr; false otherwise.
/// @note Constexpr for use in static assertions and compile-time checks.
[[nodiscard]] constexpr bool isConstantScalar(il::core::Value::Kind kind) noexcept {
    using Kind = il::core::Value::Kind;
    return kind == Kind::ConstInt || kind == Kind::ConstFloat || kind == Kind::NullPtr;
}

/// @brief Check if a Value represents a constant that can be converted to a scalar.
/// @param value The value to check.
/// @return True if the value's kind is a constant scalar.
[[nodiscard]] inline bool isConstantScalar(const il::core::Value &value) noexcept {
    return isConstantScalar(value.kind);
}

/// @brief Convert a constant VM value into a 64-bit integer.
/// @details Handles integer, floating, and null pointer constants. Other value
///          kinds are programmer errors and trigger an assertion so that new
///          kinds must update the marshalling layer explicitly.
/// @pre isConstantScalar(value) must be true.
/// @param value Constant VM value to convert.
/// @return 64-bit integer representation of @p value.
/// @note This is NOT a general-purpose coercion. It is intended only for
///       marshalling constant operands to runtime helpers.
int64_t toI64(const il::core::Value &value);

/// @brief Convert a constant VM value into a 64-bit floating point number.
/// @details Mirrors toI64 but produces a double precision result. Integer
///          constants are cast, null pointers yield zero, and unsupported kinds
///          assert during development builds.
/// @pre isConstantScalar(value) must be true.
/// @param value Constant VM value to convert.
/// @return 64-bit floating point representation of @p value.
/// @note This is NOT a general-purpose coercion. It is intended only for
///       marshalling constant operands to runtime helpers.
double toF64(const il::core::Value &value);

/// @brief Obtain a typed pointer into a slot suitable for passing as a runtime argument.
/// @param slot Slot containing the value to marshal.
/// @param kind Type kind determining which union member to address.
/// @return Pointer to the appropriate union member within @p slot.
void *slotToArgPointer(Slot &slot, il::core::Type::Kind kind);

/// @brief Obtain a pointer to the appropriate result buffer for a given type kind.
/// @param kind Type kind of the expected return value.
/// @param buffers Result buffer aggregate that owns the storage.
/// @return Pointer to the buffer member matching @p kind.
void *resultBufferFor(il::core::Type::Kind kind, ResultBuffers &buffers);

/// @brief Write a result buffer value back into a slot based on type kind.
/// @param slot Destination slot receiving the unmarshalled value.
/// @param kind Type kind selecting which buffer member to read.
/// @param buffers Result buffers populated by a prior runtime call.
void assignResult(Slot &slot, il::core::Type::Kind kind, const ResultBuffers &buffers);

//===----------------------------------------------------------------------===//
// Stack-Optimized Argument Marshalling (HIGH-6)
//===----------------------------------------------------------------------===//

/// @brief Maximum arguments for stack-allocated marshalling buffer.
/// @details Runtime calls with <= this many total arguments (params + hidden)
///          use inline storage to avoid heap allocation. Most runtime calls
///          have 0-4 arguments, so this covers the common case.
inline constexpr std::size_t kMaxStackMarshalArgs = 12;

/// @brief Stack-allocated buffer for marshalled argument pointers.
/// @details Uses inline storage for small argument counts to avoid heap
///          allocation on every runtime call. Falls back to heap for large
///          argument lists (rare in practice).
struct MarshalledArgs {
    /// @brief Inline storage for common argument counts.
    std::array<void *, kMaxStackMarshalArgs> inlineBuffer{};

    /// @brief Overflow storage for large argument lists.
    std::vector<void *> heapBuffer;

    /// @brief Number of arguments marshalled.
    std::size_t count{0};

    /// @brief Whether using heap storage.
    bool usingHeap{false};

    /// @brief Get pointer to the argument array.
    /// @return Mutable pointer to the active inline or heap storage.
    [[nodiscard]] void **data() noexcept {
        return usingHeap ? heapBuffer.data() : inlineBuffer.data();
    }

    /// @brief Get const pointer to the argument array.
    /// @return Read-only pointer to the active inline or heap storage.
    [[nodiscard]] void *const *data() const noexcept {
        return usingHeap ? heapBuffer.data() : inlineBuffer.data();
    }

    /// @brief Check if empty.
    /// @return @c true when no argument pointers are stored.
    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }

    /// @brief Get number of arguments.
    /// @return Number of visible and hidden arguments marshalled.
    [[nodiscard]] std::size_t size() const noexcept {
        return count;
    }
};

/// @brief Marshal arguments using stack-allocated buffer when possible.
/// @details Avoids heap allocation for runtime calls with <= kMaxStackMarshalArgs
///          arguments. This covers the vast majority of runtime calls.
/// @param sig Runtime signature describing parameter types.
/// @param args Argument slots to marshal.
/// @param [in,out] powStatus Power function status tracker.
/// @param [out] result Marshalled arguments using inline or heap storage.
void marshalArgumentsInline(const il::runtime::RuntimeSignature &sig,
                            std::span<Slot> args,
                            PowStatus &powStatus,
                            MarshalledArgs &result);

/// @brief Legacy interface returning vector (for compatibility).
/// @deprecated Prefer marshalArgumentsInline for new code.
/// @param sig Runtime signature describing parameter types.
/// @param args Argument slots to marshal.
/// @param [in,out] powStatus Power function status tracker.
/// @return Owning vector of pointers to visible and hidden argument storage.
std::vector<void *> marshalArguments(const il::runtime::RuntimeSignature &sig,
                                     std::span<Slot> args,
                                     PowStatus &powStatus);

/// @brief Inspect a completed power-function call and determine whether a trap occurred.
/// @details After a runtime call to a power function, this examines the status
///          flag written by the callee to decide whether the operation succeeded,
///          produced an overflow, or violated a domain constraint.
/// @param desc Runtime descriptor of the called function.
/// @param powStatus Status tracker populated during argument marshalling.
/// @param args Original argument slots (used for diagnostic context).
/// @param buffers Result buffers written by the runtime call.
/// @return Outcome indicating whether a trap was triggered and its kind/message.
PowTrapOutcome classifyPowTrap(const il::runtime::RuntimeDescriptor &desc,
                               const PowStatus &powStatus,
                               std::span<const Slot> args,
                               const ResultBuffers &buffers);

/// @brief Construct a return-value slot from result buffers using the call's signature.
/// @details Reads the appropriate result buffer member based on the signature's
///          return type and packages it into a Slot for the caller.
/// @param signature Runtime signature describing the return type.
/// @param buffers Result buffers populated by the runtime call.
/// @return Slot containing the return value.
Slot assignCallResult(const il::runtime::RuntimeSignature &signature, const ResultBuffers &buffers);

//===----------------------------------------------------------------------===//
// Marshalling Validation Helpers
//===----------------------------------------------------------------------===//

/// @brief Result of marshalling validation checks.
struct MarshalValidation {
    bool ok{true};            ///< True when all checks pass.
    std::string errorMessage; ///< Diagnostic message when validation fails.
};

/// @brief Validate that argument count matches the expected parameter count.
/// @param desc Runtime descriptor describing the expected signature.
/// @param argCount Number of arguments actually supplied.
/// @return Validation result with error message on mismatch.
[[nodiscard]] MarshalValidation validateMarshalArity(const il::runtime::RuntimeDescriptor &desc,
                                                     std::size_t argCount);

/// @brief Validate that argument count matches the expected parameter count.
/// @param sig Runtime signature describing expected parameters.
/// @param argCount Number of arguments actually supplied.
/// @param calleeName Name used in error messages.
/// @return Validation result with error message on mismatch.
[[nodiscard]] MarshalValidation validateMarshalArity(const il::runtime::RuntimeSignature &sig,
                                                     std::size_t argCount,
                                                     std::string_view calleeName);

/// @brief Validate argument count and optionally check for null pointer args.
/// @param desc Runtime descriptor describing the expected signature.
/// @param args Span of argument slots to validate.
/// @param checkNullPointers When true, validates that pointer-typed args are non-null.
/// @return Validation result with error message on failure.
[[nodiscard]] MarshalValidation validateMarshalArgs(const il::runtime::RuntimeDescriptor &desc,
                                                    std::span<const Slot> args,
                                                    bool checkNullPointers = false);

/// @brief Combined marshal and validate: checks arity then builds void** array.
/// @details This is the recommended entry point for marshalling runtime calls.
///          It validates argument count before marshalling to avoid out-of-bounds
///          access, and can optionally validate pointer arguments.
/// @param desc Runtime descriptor for the callee.
/// @param args Argument slots supplied by the caller.
/// @param [in,out] powStatus Power function trap status tracker.
/// @param [out] validation Validation result; check before using returned vector.
/// @param checkNullPointers When true, validates pointer args are non-null.
/// @return Vector of marshalled argument pointers; empty on validation failure.
[[nodiscard]] std::vector<void *> marshalArgumentsValidated(
    const il::runtime::RuntimeDescriptor &desc,
    std::span<Slot> args,
    PowStatus &powStatus,
    MarshalValidation &validation,
    bool checkNullPointers = false);

} // namespace il::vm
