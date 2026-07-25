//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/RuntimeSignatures_Handlers.cpp
// Purpose: Adapter function implementations for runtime descriptor marshalling.
//          These functions bridge the VM's void** argument arrays to typed C
//          runtime function calls for helpers requiring custom marshalling.
// Key invariants: Each adapter matches the RuntimeHandler signature.
// Ownership/Lifetime: Functions have static duration; called via function ptr.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements custom VM-to-runtime call marshalling adapters.
/// @details Each adapter validates typed argument slots, translates pointer or
///          numeric representations as required by the native helper, and writes
///          non-void results through mandatory VM result storage.

#include "RuntimeSignatures_Handlers.hpp"

#include "rt.hpp"
#include "rt_array.h"
#include "rt_array_f64.h"
#include "rt_array_i64.h"
#include "rt_array_obj.h"
#include "rt_fp.h"
#include "rt_internal.h"
#include "rt_math.h"
#include "zanna/runtime/rt.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace il::runtime {
namespace {

constexpr const char kTestBridgeMutatedText[] = "bridge-mutated";

/// @brief Return a typed pointer to a required VM argument slot.
/// @details Runtime bridge handlers receive a `void**` where each element points
///          to storage for the corresponding IL argument. Missing slots indicate
///          descriptor/VM bridge disagreement and are fatal because continuing
///          would fabricate arguments that the IL program did not supply.
/// @tparam T Stored argument type expected in the slot.
/// @param args VM-provided argument slot array.
/// @param index Zero-based argument index to read.
/// @return Pointer to typed slot storage.
template <typename T> T *argSlot(void **args, size_t index) {
    return detail::requiredArgSlot<T>(args, index);
}

/// @brief Clamp a 64-bit runtime file position/length to a 32-bit IL value.
///
/// @details Runtime helpers such as @ref rt_lof_ch and @ref rt_loc_ch expose
///          64-bit offsets so large files can be supported.  BASIC however
///          models these builtins as returning 32-bit signed integers.  The
///          bridge narrows the runtime result using saturation so overflow
///          produces INT32_MAX/INT32_MIN instead of wrapping.
/// @param value Runtime offset or length to narrow.
/// @return Saturated signed 32-bit representation.
int32_t clampRuntimeOffset(int64_t value) {
    if (value > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    if (value < std::numeric_limits<int32_t>::min())
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

} // namespace

// ============================================================================
// File I/O adapters
// ============================================================================

/// @brief Return a channel's file length in saturated I32 form.
/// @param channel Runtime file-channel identifier.
/// @return File length clamped to the signed 32-bit range.
int32_t rt_lof_ch_i32(int32_t channel) {
    return clampRuntimeOffset(rt_lof_ch(channel));
}

/// @brief Return a channel's current position in saturated I32 form.
/// @param channel Runtime file-channel identifier.
/// @return Current offset clamped to the signed 32-bit range.
int32_t rt_loc_ch_i32(int32_t channel) {
    return clampRuntimeOffset(rt_loc_ch(channel));
}

// ============================================================================
// Debug/test adapters
// ============================================================================

/// @brief Forward a runtime string argument to the runtime trap facility.
/// @param args Slot zero contains the runtime string handle.
/// @param result Unused because trapping has no return value.
void trapFromRuntimeString(void **args, void * /*result*/) {
    auto *slot = argSlot<rt_string>(args, 0);
    rt_string str = *slot;
    rt_trap_string(str);
}

/// @brief Replace a debugger-test string slot without stack mediation.
/// @param args Slot zero receives a newly allocated test string handle.
/// @param result Unused because mutation occurs through the argument slot.
void testMutateStringNoStack(void **args, void * /*result*/) {
    auto *slot = argSlot<rt_string>(args, 0);
    rt_string updated =
        rt_string_from_bytes(kTestBridgeMutatedText, sizeof(kTestBridgeMutatedText) - 1);
    *slot = updated;
}

// ============================================================================
// Integer array adapters
// ============================================================================

/// @brief Allocate an I32 array from an I64 length operand.
/// @param args Slot zero contains the requested element count.
/// @param result Receives the new array payload pointer.
void invokeRtArrI32New(void **args, void *result) {
    const auto lenPtr = argSlot<const int64_t>(args, 0);
    const size_t len = static_cast<size_t>(*lenPtr);
    int32_t *arr = rt_arr_i32_new(len);
    detail::storeRequiredResult<void *>(result, arr);
}

/// @brief Read an I32 array length as an I64 IL value.
/// @param args Slot zero contains the array payload pointer.
/// @param result Receives the element count.
void invokeRtArrI32Len(void **args, void *result) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    int32_t *arr = *arrPtr;
    const size_t len = rt_arr_i32_len(arr);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(len));
}

/// @brief Perform a checked I32 array read.
/// @param args Slots contain the array pointer and I64 index.
/// @param result Receives the sign-preserving I64 element value.
void invokeRtArrI32Get(void **args, void *result) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    int32_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int32_t value = rt_arr_i32_get(arr, idx);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(value));
}

/// @brief Perform a checked I32 array write.
/// @param args Slots contain the array pointer, I64 index, and I64 value.
/// @param result Unused for this void operation.
void invokeRtArrI32Set(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const int64_t>(args, 2);
    int32_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int32_t value = static_cast<int32_t>(*valPtr);
    rt_arr_i32_set(arr, idx, value);
}

/// @brief Read an I32 element after a dominating bounds check.
/// @param args Slots contain the array pointer and I64 index.
/// @param result Receives the sign-preserving I64 element value.
void invokeRtArrI32GetFast(void **args, void *result) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    int32_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int32_t value = rt_arr_i32_get_fast(arr, idx);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(value));
}

/// @brief Write an I32 element after a dominating bounds check.
/// @param args Slots contain the array pointer, I64 index, and I64 value.
/// @param result Unused for this void operation.
void invokeRtArrI32SetFast(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const int64_t>(args, 2);
    int32_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int32_t value = static_cast<int32_t>(*valPtr);
    rt_arr_i32_set_fast(arr, idx, value);
}

/// @brief Resize an I32 array through its mutable payload handle.
/// @param args Slots contain the array handle and new I64 length.
/// @param result Receives the resized pointer or null on failure.
void invokeRtArrI32Resize(void **args, void *result) {
    const auto arrPtr = argSlot<int32_t *>(args, 0);
    const auto newLenPtr = argSlot<const int64_t>(args, 1);
    const size_t newLen = static_cast<size_t>(*newLenPtr);
    int32_t **handle = arrPtr;
    int rc = rt_arr_i32_resize(handle, newLen);
    int32_t *resized = (rc == 0) ? *handle : nullptr;
    if (rc == 0)
        *arrPtr = resized;
    detail::storeRequiredResult<void *>(result, resized);
}

// ============================================================================
// I64 (LONG) array adapters
// ============================================================================

/// @brief Allocate an I64 array.
/// @param args Slot zero contains the requested element count.
/// @param result Receives the new array payload pointer.
void invokeRtArrI64New(void **args, void *result) {
    const auto lenPtr = argSlot<const int64_t>(args, 0);
    const size_t len = static_cast<size_t>(*lenPtr);
    int64_t *arr = rt_arr_i64_new(len);
    detail::storeRequiredResult<void *>(result, arr);
}

/// @brief Read an I64 array length.
/// @param args Slot zero contains the array payload pointer.
/// @param result Receives the I64 element count.
void invokeRtArrI64Len(void **args, void *result) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    int64_t *arr = *arrPtr;
    const size_t len = rt_arr_i64_len(arr);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(len));
}

/// @brief Perform a checked I64 array read.
/// @param args Slots contain the array pointer and I64 index.
/// @param result Receives the selected I64 element.
void invokeRtArrI64Get(void **args, void *result) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    int64_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int64_t value = rt_arr_i64_get(arr, idx);
    detail::storeRequiredResult<int64_t>(result, value);
}

/// @brief Perform a checked I64 array write.
/// @param args Slots contain the array pointer, index, and value.
/// @param result Unused for this void operation.
void invokeRtArrI64Set(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const int64_t>(args, 2);
    int64_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int64_t value = *valPtr;
    rt_arr_i64_set(arr, idx, value);
}

/// @brief Read an I64 element after a dominating bounds check.
/// @param args Slots contain the array pointer and index.
/// @param result Receives the selected element.
void invokeRtArrI64GetFast(void **args, void *result) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    int64_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int64_t value = rt_arr_i64_get_fast(arr, idx);
    detail::storeRequiredResult<int64_t>(result, value);
}

/// @brief Write an I64 element after a dominating bounds check.
/// @param args Slots contain the array pointer, index, and value.
/// @param result Unused for this void operation.
void invokeRtArrI64SetFast(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const int64_t>(args, 2);
    int64_t *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const int64_t value = *valPtr;
    rt_arr_i64_set_fast(arr, idx, value);
}

/// @brief Resize an I64 array through its mutable payload handle.
/// @param args Slots contain the array handle and new length.
/// @param result Receives the resized pointer or null on failure.
void invokeRtArrI64Resize(void **args, void *result) {
    const auto arrPtr = argSlot<int64_t *>(args, 0);
    const auto newLenPtr = argSlot<const int64_t>(args, 1);
    const size_t newLen = static_cast<size_t>(*newLenPtr);
    int64_t **handle = arrPtr;
    int rc = rt_arr_i64_resize(handle, newLen);
    int64_t *resized = (rc == 0) ? *handle : nullptr;
    if (rc == 0)
        *arrPtr = resized;
    detail::storeRequiredResult<void *>(result, resized);
}

// ============================================================================
// F64 (SINGLE/DOUBLE) array adapters
// ============================================================================

/// @brief Allocate an F64 array.
/// @param args Slot zero contains the requested element count.
/// @param result Receives the new array payload pointer.
void invokeRtArrF64New(void **args, void *result) {
    const auto lenPtr = argSlot<const int64_t>(args, 0);
    const size_t len = static_cast<size_t>(*lenPtr);
    double *arr = rt_arr_f64_new(len);
    detail::storeRequiredResult<void *>(result, arr);
}

/// @brief Read an F64 array length as an I64 value.
/// @param args Slot zero contains the array payload pointer.
/// @param result Receives the element count.
void invokeRtArrF64Len(void **args, void *result) {
    const auto arrPtr = argSlot<double *>(args, 0);
    double *arr = *arrPtr;
    const size_t len = rt_arr_f64_len(arr);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(len));
}

/// @brief Perform a checked F64 array read.
/// @param args Slots contain the array pointer and I64 index.
/// @param result Receives the selected F64 element.
void invokeRtArrF64Get(void **args, void *result) {
    const auto arrPtr = argSlot<double *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    double *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const double value = rt_arr_f64_get(arr, idx);
    detail::storeRequiredResult<double>(result, value);
}

/// @brief Perform a checked F64 array write.
/// @param args Slots contain the array pointer, index, and F64 value.
/// @param result Unused for this void operation.
void invokeRtArrF64Set(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<double *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const double>(args, 2);
    double *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const double value = *valPtr;
    rt_arr_f64_set(arr, idx, value);
}

/// @brief Read an F64 element after a dominating bounds check.
/// @param args Slots contain the array pointer and index.
/// @param result Receives the selected element.
void invokeRtArrF64GetFast(void **args, void *result) {
    const auto arrPtr = argSlot<double *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    double *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const double value = rt_arr_f64_get_fast(arr, idx);
    detail::storeRequiredResult<double>(result, value);
}

/// @brief Write an F64 element after a dominating bounds check.
/// @param args Slots contain the array pointer, index, and value.
/// @param result Unused for this void operation.
void invokeRtArrF64SetFast(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<double *>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<const double>(args, 2);
    double *arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    const double value = *valPtr;
    rt_arr_f64_set_fast(arr, idx, value);
}

/// @brief Resize an F64 array through its mutable payload handle.
/// @param args Slots contain the array handle and new length.
/// @param result Receives the resized pointer or null on failure.
void invokeRtArrF64Resize(void **args, void *result) {
    const auto arrPtr = argSlot<double *>(args, 0);
    const auto newLenPtr = argSlot<const int64_t>(args, 1);
    const size_t newLen = static_cast<size_t>(*newLenPtr);
    double **handle = arrPtr;
    int rc = rt_arr_f64_resize(handle, newLen);
    double *resized = (rc == 0) ? *handle : nullptr;
    if (rc == 0)
        *arrPtr = resized;
    detail::storeRequiredResult<void *>(result, resized);
}

// ============================================================================
// Object array adapters
// ============================================================================

/// @brief Allocate an object-reference array.
/// @param args Slot zero contains the requested element count.
/// @param result Receives the new array payload pointer.
void invokeRtArrObjNew(void **args, void *result) {
    const auto lenPtr = argSlot<const int64_t>(args, 0);
    const size_t len = static_cast<size_t>(*lenPtr);
    void **arr = rt_arr_obj_new(len);
    detail::storeRequiredResult<void *>(result, arr);
}

/// @brief Read an object array length.
/// @param args Slot zero contains the array payload pointer.
/// @param result Receives the I64 element count.
void invokeRtArrObjLen(void **args, void *result) {
    const auto arrPtr = argSlot<void **>(args, 0);
    void **arr = *arrPtr;
    const size_t len = rt_arr_obj_len(arr);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(len));
}

/// @brief Perform a checked object array read.
/// @param args Slots contain the array pointer and I64 index.
/// @param result Receives the selected object pointer.
void invokeRtArrObjGet(void **args, void *result) {
    const auto arrPtr = argSlot<void **>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    void **arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    void *ptr = rt_arr_obj_get(arr, idx);
    detail::storeRequiredResult<void *>(result, ptr);
}

/// @brief Perform a checked retaining object array write.
/// @param args Slots contain the array pointer, index, and object pointer.
/// @param result Unused for this void operation.
void invokeRtArrObjPut(void **args, void * /*result*/) {
    const auto arrPtr = argSlot<void **>(args, 0);
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const auto valPtr = argSlot<void *>(args, 2);
    void **arr = *arrPtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    void *val = *valPtr;
    rt_arr_obj_put(arr, idx, val);
}

/// @brief Resize an object-reference array.
/// @param args Slots contain the array pointer and new length.
/// @param result Receives the resized array pointer.
void invokeRtArrObjResize(void **args, void *result) {
    const auto arrPtr = argSlot<void **>(args, 0);
    const auto lenPtr = argSlot<const int64_t>(args, 1);
    void **arr = *arrPtr;
    const size_t len = static_cast<size_t>(*lenPtr);
    void **res = rt_arr_obj_resize(arr, len);
    detail::storeRequiredResult<void *>(result, res);
}

// ============================================================================
// String array adapters
// ============================================================================

/// @brief Allocate a reference-counted string array.
/// @param args Slot zero contains the requested element count.
/// @param result Receives the new array payload pointer.
void invokeRtArrStrAlloc(void **args, void *result) {
    const auto lenPtr = argSlot<const int64_t>(args, 0);
    const size_t len = static_cast<size_t>(*lenPtr);
    rt_string *arr = rt_arr_str_alloc(len);
    detail::storeRequiredResult<void *>(result, arr);
}

/// @brief Release a string array and its element references.
/// @param args Slots contain the array payload and its element count.
/// @param result Unused for this void operation.
void invokeRtArrStrRelease(void **args, void * /*result*/) {
    // Param 0 is a pointer-typed IL value; args[0] points to storage containing the pointer.
    const auto arrPtr = argSlot<rt_string *>(args, 0);
    rt_string *arr = *arrPtr;
    const auto sizePtr = argSlot<const int64_t>(args, 1);
    const size_t size = static_cast<size_t>(*sizePtr);
    rt_arr_str_release(arr, size);
}

/// @brief Read and retain a string array element.
/// @param args Slots contain the array payload and index.
/// @param result Receives the retained runtime string handle.
void invokeRtArrStrGet(void **args, void *result) {
    // Param 0 (ptr): args[0] -> storage holding the array payload pointer
    const auto arrPtr = argSlot<rt_string *>(args, 0);
    rt_string *arr = *arrPtr;
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    const size_t idx = static_cast<size_t>(*idxPtr);
    rt_string value = rt_arr_str_get(arr, idx);
    detail::storeRequiredResult<rt_string>(result, value);
}

/// @brief Store a runtime string handle into an array.
/// @param args Slots contain the array payload, index, and string handle.
/// @param result Unused for this void operation.
void invokeRtArrStrPut(void **args, void * /*result*/) {
    // Param 0 (ptr): args[0] -> storage holding the array payload pointer
    const auto arrPtr = argSlot<rt_string *>(args, 0);
    rt_string *arr = *arrPtr;
    const auto idxPtr = argSlot<const int64_t>(args, 1);
    // Param 2 (string): args[2] -> storage holding the string handle directly
    const auto valuePtr = argSlot<rt_string>(args, 2);
    rt_string value = *valuePtr;
    const size_t idx = static_cast<size_t>(*idxPtr);
    rt_arr_str_put(arr, idx, value);
}

/// @brief Read a string array length.
/// @param args Slot zero contains the array payload pointer.
/// @param result Receives the I64 element count.
void invokeRtArrStrLen(void **args, void *result) {
    // Param 0 (ptr): args[0] -> storage holding the array payload pointer
    const auto arrPtr = argSlot<rt_string *>(args, 0);
    rt_string *arr = *arrPtr;
    const size_t len = rt_arr_str_len(arr);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(len));
}

// ============================================================================
// Bounds checking
// ============================================================================

/// @brief Invoke the runtime's array-bounds panic helper.
/// @param args Slots contain the invalid index and current length.
/// @param result Unused because the helper traps.
void invokeRtArrOobPanic(void **args, void * /*result*/) {
    const auto idxPtr = argSlot<const int64_t>(args, 0);
    const auto lenPtr = argSlot<const int64_t>(args, 1);
    const size_t idx = static_cast<size_t>(*idxPtr);
    const size_t len = static_cast<size_t>(*lenPtr);
    rt_arr_oob_panic(idx, len);
}

// ============================================================================
// Conversion adapters
// ============================================================================

/// @brief Marshal checked F64-to-I16 conversion as an I64 IL result.
/// @param args Slots contain the input value and writable success pointer.
/// @param result Receives the widened converted integer.
void invokeRtCintFromDouble(void **args, void *result) {
    const auto xPtr = argSlot<const double>(args, 0);
    const auto okPtr = argSlot<bool *>(args, 1);
    const double x = *xPtr;
    bool *ok = *okPtr;
    const int16_t value = rt_cint_from_double(x, ok);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(value));
}

/// @brief Marshal checked F64-to-I32 conversion as an I64 IL result.
/// @param args Slots contain the input value and writable success pointer.
/// @param result Receives the widened converted integer.
void invokeRtClngFromDouble(void **args, void *result) {
    const auto xPtr = argSlot<const double>(args, 0);
    const auto okPtr = argSlot<bool *>(args, 1);
    const double x = *xPtr;
    bool *ok = *okPtr;
    const int32_t value = rt_clng_from_double(x, ok);
    detail::storeRequiredResult<int64_t>(result, static_cast<int64_t>(value));
}

/// @brief Marshal checked F64-to-F32 conversion as an F64 IL result.
/// @param args Slots contain the input value and writable success pointer.
/// @param result Receives the widened single-precision result.
void invokeRtCsngFromDouble(void **args, void *result) {
    const auto xPtr = argSlot<const double>(args, 0);
    const auto okPtr = argSlot<bool *>(args, 1);
    const double x = *xPtr;
    bool *ok = *okPtr;
    const float value = rt_csng_from_double(x, ok);
    detail::storeRequiredResult<double>(result, static_cast<double>(value));
}

/// @brief Format an F64 operand through the runtime's single-precision path.
/// @param args Slot zero contains the floating-point value.
/// @param result Receives the newly allocated runtime string.
void invokeRtStrFAlloc(void **args, void *result) {
    const auto xPtr = argSlot<const double>(args, 0);
    rt_string str = rt_str_f_alloc(*xPtr); // narrows to single precision internally
    detail::storeRequiredResult<rt_string>(result, str);
}

/// @brief Round an F64 operand to an even result at a requested precision.
/// @param args Slots contain the value and decimal digit count.
/// @param result Receives the rounded F64 value.
void invokeRtRoundEven(void **args, void *result) {
    const auto xPtr = argSlot<const double>(args, 0);
    const auto digitsPtr = argSlot<const int64_t>(args, 1);
    const double x = *xPtr;
    const int ndigits = static_cast<int>(*digitsPtr);
    const double rounded = rt_round_even(x, ndigits);
    detail::storeRequiredResult<double>(result, rounded);
}

/// @brief Invoke checked power with an explicit status pointer.
/// @param args Slots contain base, exponent, and writable status pointer.
/// @param result Receives the computed F64 value.
void invokeRtPowF64Chkdom(void **args, void *result) {
    const auto basePtr = argSlot<const double>(args, 0);
    const auto expPtr = argSlot<const double>(args, 1);
    const auto okPtrPtr = argSlot<bool *>(args, 2);
    const double base = *basePtr;
    const double exponent = *expPtr;
    bool *ok = *okPtrPtr;
    const double value = rt_pow_f64_chkdom(base, exponent, ok);
    detail::storeRequiredResult<double>(result, value);
}

/// @brief Invoke checked power while supplying the hidden status locally.
/// @param args Slots contain the visible base and exponent operands.
/// @param result Receives the computed value when no domain error occurs.
void vmInvokeRtPow(void **args, void *result) {
    const auto basePtr = argSlot<const double>(args, 0);
    const auto expPtr = argSlot<const double>(args, 1);
    const double base = *basePtr;
    const double exponent = *expPtr;
    bool ok = true;
    const double value = rt_pow_f64_chkdom(base, exponent, &ok);
    if (!ok) {
        rt_trap("Pow: domain error or overflow");
        return;
    }
    detail::storeRequiredResult<double>(result, value);
}

} // namespace il::runtime
