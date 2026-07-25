//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/RuntimeSignatures_Handlers.hpp
// Purpose: Handler templates and adapter function declarations for runtime
//          descriptor marshalling. These templates bridge the VM's generic
//          void** argument arrays to typed C runtime function calls.
// Key invariants: Handler templates must match the RuntimeHandler signature.
// Ownership/Lifetime: Templates are instantiated at compile time; adapters
//                     have static duration.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares typed VM-to-runtime marshalling adapters.
/// @details Generic templates cover direct C ABI calls, while named adapters
///          handle array pointers, ownership, narrowing, and hidden parameters.

#pragma once

#include "zanna/runtime/rt.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace il::runtime {
namespace detail {

/// @brief Abort execution after reporting a runtime-handler marshalling invariant failure.
/// @details Runtime handlers are called from the VM through a C ABI function pointer and cannot
///          return structured diagnostics. A missing argument slot or result buffer means the VM
///          bridge and descriptor metadata disagree; continuing would reinterpret invalid storage,
///          so the bridge fails closed with a fatal diagnostic.
/// @param message Human-readable reason for the marshalling failure.
[[noreturn]] inline void runtimeHandlerFatal(const char *message) {
    std::fprintf(stderr, "[FATAL] Runtime handler marshalling failure: %s\n", message);
    std::abort();
}

/// @brief Return a typed pointer to a required VM argument slot.
/// @details The VM passes one pointer to storage for each argument. This helper validates both
///          the outer argument array and the selected slot before applying the typed cast used by
///          runtime handler templates.
/// @tparam T Argument storage type expected by the bound runtime function.
/// @param args VM-provided argument slot array.
/// @param index Zero-based argument index.
/// @return Pointer to typed argument storage.
template <typename T> T *requiredArgSlot(void **args, std::size_t index) {
    if (!args)
        runtimeHandlerFatal("argument array is null");
    if (!args[index])
        runtimeHandlerFatal("argument slot is null");
    return reinterpret_cast<T *>(args[index]);
}

/// @brief Store a non-void runtime return value into required VM result storage.
/// @details Non-void runtime descriptors must provide result storage. This helper checks that
///          contract and performs the same typed storage write that the VM bridge expects.
/// @tparam T Return value type.
/// @param result VM-provided result storage.
/// @param value Runtime helper result to store.
template <typename T> void storeRequiredResult(void *result, T value) {
    if (!result)
        runtimeHandlerFatal("non-void runtime call missing result storage");
    using StoredT = std::remove_cv_t<T>;
    *reinterpret_cast<StoredT *>(result) = static_cast<StoredT>(value);
}

} // namespace detail

/// @brief Adapter that invokes a concrete runtime function from VM call stubs.
///
/// @details Each instantiation binds a runtime C function pointer and translates
///          the generic `void **` argument array provided by the VM into typed
///          parameters.  The @ref invoke entry point matches the signature
///          expected by @ref RuntimeDescriptor.
/// @tparam Fn Bound runtime function pointer.
/// @tparam Ret Runtime function return type.
/// @tparam Args Runtime function parameter types in ABI order.
template <auto Fn, typename Ret, typename... Args> struct DirectHandler {
    /// @brief Dispatch the runtime call using the supplied argument array.
    ///
    /// @details Expands the argument pack through @ref call using an index
    ///          sequence so each operand is reinterpreted to the function's
    ///          declared type.  Results are written into the provided buffer when
    ///          applicable.
    ///
    /// @param args Pointer to the marshalled argument array.
    /// @param result Optional pointer to storage for the return value.
    static void invoke(void **args, void *result) {
        call(args, result, std::index_sequence_for<Args...>{});
    }

  private:
    /// @brief Helper that expands the argument pack and forwards to the target.
    ///
    /// @details Uses `reinterpret_cast` to view each `void *` slot as the
    ///          appropriate type.  When the function returns a value the helper
    ///          stores it through @p result.
    /// @tparam I Compile-time argument indices.
    /// @param args Pointer to marshalled argument storage.
    /// @param result Optional result storage for non-void targets.
    /// @param Unnamed index sequence selecting each argument slot.
    template <std::size_t... I>
    static void call(void **args, void *result, std::index_sequence<I...>) {
        if constexpr (std::is_void_v<Ret>) {
            Fn(*detail::requiredArgSlot<Args>(args, I)...);
        } else {
            detail::storeRequiredResult<Ret>(
                result, Fn(*detail::requiredArgSlot<Args>(args, I)...));
        }
    }
};

/// @brief Handler that retains string arguments before invoking the runtime.
///
/// @details Some runtime entry points consume string handles without
///          retaining them, so the VM must increment reference counts before
///          the call to keep values alive.  The wrapper first retains any
///          string arguments and then delegates to @ref DirectHandler to
///          perform the actual call/return marshalling.
/// @tparam Fn Bound runtime function pointer.
/// @tparam Ret Runtime function return type.
/// @tparam Args Runtime function parameter types in ABI order.
template <auto Fn, typename Ret, typename... Args> struct ConsumingStringHandler {
    /// @brief Invoke a runtime helper after retaining string arguments.
    /// @param args VM-supplied argument array.
    /// @param result Optional pointer to storage for the return value.
    static void invoke(void **args, void *result) {
        retainStrings(args, std::index_sequence_for<Args...>{});
        DirectHandler<Fn, Ret, Args...>::invoke(args, result);
    }

  private:
    /// @brief Retain every string argument present in the parameter pack.
    /// @tparam I Compile-time argument indices.
    /// @param args VM-supplied argument array.
    /// @param Unnamed index sequence selecting argument slots.
    template <std::size_t... I> static void retainStrings(void **args, std::index_sequence<I...>) {
        (retainArg<Args>(args, I), ...);
    }

    /// @brief Retain a single argument when it is of type @c rt_string.
    /// @tparam T Declared argument type.
    /// @param args VM-supplied argument array.
    /// @param index Zero-based slot index.
    template <typename T> static void retainArg(void **args, std::size_t index) {
        if constexpr (std::is_same_v<std::remove_cv_t<T>, rt_string>) {
            if (!args)
                return;
            void *slot = args[index];
            if (!slot)
                return;
            rt_string value = *reinterpret_cast<rt_string *>(slot);
            if (value)
                rt_string_ref(value);
        }
    }
};

// ============================================================================
// Adapter function declarations
// These bridge VM calling conventions to specific runtime helpers that require
// custom argument/result marshalling beyond what DirectHandler provides.
// ============================================================================

/// @brief Bridge runtime string trap requests into the VM trap mechanism.
void trapFromRuntimeString(void **args, void *result);

/// @brief Mutate a string handle in place for debugger bridge tests.
void testMutateStringNoStack(void **args, void *result);

/// @brief Adapter that narrows @ref rt_lof_ch results to 32 bits.
int32_t rt_lof_ch_i32(int32_t channel);

/// @brief Adapter that narrows @ref rt_loc_ch results to 32 bits.
int32_t rt_loc_ch_i32(int32_t channel);

// --- Integer (i32) array helpers ---
// These adapters marshal VM void** arrays for 32-bit integer array operations.

/// @brief Allocate a new i32 array with the specified capacity.
void invokeRtArrI32New(void **args, void *result);
/// @brief Return the length of an i32 array.
void invokeRtArrI32Len(void **args, void *result);
/// @brief Read an element from an i32 array by index.
void invokeRtArrI32Get(void **args, void *result);
/// @brief Write an element into an i32 array at the given index.
void invokeRtArrI32Set(void **args, void *result);
/// @brief Read an element from an i32 array after a dominating bounds check.
void invokeRtArrI32GetFast(void **args, void *result);
/// @brief Write an element into an i32 array after a dominating bounds check.
void invokeRtArrI32SetFast(void **args, void *result);
/// @brief Resize an i32 array to a new capacity.
void invokeRtArrI32Resize(void **args, void *result);

// --- LONG (i64) array helpers ---
// These adapters marshal VM void** arrays for 64-bit integer array operations.

/// @brief Allocate a new i64 array with the specified capacity.
void invokeRtArrI64New(void **args, void *result);
/// @brief Return the length of an i64 array.
void invokeRtArrI64Len(void **args, void *result);
/// @brief Read an element from an i64 array by index.
void invokeRtArrI64Get(void **args, void *result);
/// @brief Write an element into an i64 array at the given index.
void invokeRtArrI64Set(void **args, void *result);
/// @brief Read an element from an i64 array after a dominating bounds check.
void invokeRtArrI64GetFast(void **args, void *result);
/// @brief Write an element into an i64 array after a dominating bounds check.
void invokeRtArrI64SetFast(void **args, void *result);
/// @brief Resize an i64 array to a new capacity.
void invokeRtArrI64Resize(void **args, void *result);

// --- SINGLE/DOUBLE (f64) array helpers ---
// These adapters marshal VM void** arrays for 64-bit floating-point array operations.

/// @brief Allocate a new f64 array with the specified capacity.
void invokeRtArrF64New(void **args, void *result);
/// @brief Return the length of an f64 array.
void invokeRtArrF64Len(void **args, void *result);
/// @brief Read an element from an f64 array by index.
void invokeRtArrF64Get(void **args, void *result);
/// @brief Write an element into an f64 array at the given index.
void invokeRtArrF64Set(void **args, void *result);
/// @brief Read an element from an f64 array after a dominating bounds check.
void invokeRtArrF64GetFast(void **args, void *result);
/// @brief Write an element into an f64 array after a dominating bounds check.
void invokeRtArrF64SetFast(void **args, void *result);
/// @brief Resize an f64 array to a new capacity.
void invokeRtArrF64Resize(void **args, void *result);

// --- Object array helpers ---
// These adapters marshal VM void** arrays for object pointer array operations.

/// @brief Allocate a new object array with the specified capacity.
void invokeRtArrObjNew(void **args, void *result);
/// @brief Return the length of an object array.
void invokeRtArrObjLen(void **args, void *result);
/// @brief Read an element from an object array by index.
void invokeRtArrObjGet(void **args, void *result);
/// @brief Write an element into an object array at the given index.
void invokeRtArrObjPut(void **args, void *result);
/// @brief Resize an object array to a new capacity.
void invokeRtArrObjResize(void **args, void *result);

// --- String array helpers ---
// These adapters marshal VM void** arrays for reference-counted string array operations.

/// @brief Allocate a new string array with the specified capacity.
void invokeRtArrStrAlloc(void **args, void *result);
/// @brief Release a string array and decrement element reference counts.
void invokeRtArrStrRelease(void **args, void *result);
/// @brief Read a string element from the array, retaining the reference.
void invokeRtArrStrGet(void **args, void *result);
/// @brief Write a string element into the array at the given index.
void invokeRtArrStrPut(void **args, void *result);
/// @brief Return the length of a string array.
void invokeRtArrStrLen(void **args, void *result);

// --- Bounds checking ---

/// @brief Trigger a bounds-check panic when an array index is out of range.
void invokeRtArrOobPanic(void **args, void *result);

// --- Conversion helpers ---
// These adapters handle numeric conversion with appropriate range checking.

/// @brief Convert a double-precision float to a 16-bit integer (CINT).
void invokeRtCintFromDouble(void **args, void *result);
/// @brief Convert a double-precision float to a 64-bit integer (CLNG).
void invokeRtClngFromDouble(void **args, void *result);
/// @brief Convert a double-precision float to a 32-bit float (CSNG).
void invokeRtCsngFromDouble(void **args, void *result);
/// @brief Allocate a formatted string from a double-precision float.
void invokeRtStrFAlloc(void **args, void *result);
/// @brief Round a double-precision float to the nearest even integer.
void invokeRtRoundEven(void **args, void *result);
/// @brief Compute power with domain/overflow checking (f64 ^ f64).
void invokeRtPowF64Chkdom(void **args, void *result);

/// @brief VM adapter for Pow that manages the hidden bool* parameter internally.
/// @details The C function rt_pow_f64_chkdom takes a hidden bool* status pointer
///          that native codegen passes via a hidden ABI parameter.  The VM only
///          passes the two visible f64 arguments, so this adapter allocates a
///          local bool and traps on domain errors.
void vmInvokeRtPow(void **args, void *result);

} // namespace il::runtime
