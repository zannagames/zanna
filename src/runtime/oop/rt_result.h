//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/oop/rt_result.h
// Purpose: Result type for error handling representing either Ok(value) or
//          Err(error), with typed extraction and pointer-payload transformation
//          and recovery combinators.
//
// Key invariants:
//   - A Result is always exactly one of Ok or Err.
//   - rt_result_unwrap* requires Ok and rt_result_unwrap_err* requires Err;
//     typed accessors must also match the stored pointer/string/integer/float tag.
//   - Null is neither Ok nor Err.
//   - Callback combinators transform only pointer-valued payloads; typed
//     non-pointer Results pass through unchanged.
//
// Ownership/Lifetime:
//   - Constructors return reference-counted, caller-owned opaque Result objects.
//   - Managed pointer and string payloads are retained while stored; inline
//     numeric payloads own no heap reference.
//   - Extraction and combinator pass-through paths return borrowed references.
//
// Links: src/runtime/oop/rt_result.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_result.h
 * @brief Declares tagged Ok and Err values and Result combinators.
 * @details The API creates typed success and error payloads, inspects variants,
 *          performs strict or defaulted extraction, transforms pointer-backed
 *          values, recovers errors, matches callbacks, and converts successful
 *          Results to Options under explicit managed ownership contracts.
 */

#pragma once

#include "rt_option.h" /* rt_cb_invoke1 callback-invoker strategy types */
#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Result Creation
//=========================================================================

/// @brief Create a pointer-valued `Ok` Result.
/// @details Retains runtime-managed pointer payloads; unmanaged raw pointers
///          are stored borrowed. Null is a valid success payload.
/// @param value Pointer success value; may be @c NULL.
/// @return Caller-owned Result, or @c NULL after a trapped setup failure.
void *rt_result_ok(void *value);

/// @brief Create a string-valued `Ok` Result.
/// @param value Runtime string retained by the Result; may be @c NULL.
/// @return Caller-owned Result, or @c NULL after a trapped setup failure.
void *rt_result_ok_str(rt_string value);

/// @brief Create an integer-valued `Ok` Result.
/// @param value Signed integer stored inline.
/// @return Caller-owned Result, or @c NULL after allocation failure.
void *rt_result_ok_i64(int64_t value);

/// @brief Create a floating-point-valued `Ok` Result.
/// @param value Double-precision value stored inline.
/// @return Caller-owned Result, or @c NULL after allocation failure.
void *rt_result_ok_f64(double value);

/// @brief Create a pointer-valued `Err` Result.
/// @details Retains runtime-managed pointer payloads; unmanaged raw pointers
///          are stored borrowed. Null is a valid error payload.
/// @param error Pointer error value; may be @c NULL.
/// @return Caller-owned Result, or @c NULL after a trapped setup failure.
void *rt_result_err(void *error);

/// @brief Create a string-valued `Err` Result.
/// @param message Runtime string retained by the Result; may be @c NULL.
/// @return Caller-owned Result, or @c NULL after a trapped setup failure.
void *rt_result_err_str(rt_string message);

//=========================================================================
// Result Inspection
//=========================================================================

/// @brief Check whether a Result is `Ok`.
/// @param obj Valid Result; @c NULL is neither variant.
/// @return @c 1 for `Ok`, otherwise @c 0.
int8_t rt_result_is_ok(void *obj);

/// @brief Check whether a Result is `Err`.
/// @param obj Valid Result; @c NULL is neither variant.
/// @return @c 1 for `Err`, otherwise @c 0.
int8_t rt_result_is_err(void *obj);

//=========================================================================
// Value Extraction
//=========================================================================

/// @brief Unwrap a pointer-valued `Ok`.
/// @param obj Valid pointer-valued Result; null, `Err`, and type mismatch trap.
/// @return Borrowed success pointer, which may be @c NULL for `Ok(NULL)`.
void *rt_result_unwrap(void *obj);

/// @brief Unwrap a string-valued `Ok`.
/// @param obj Valid string-valued Result; null, `Err`, and type mismatch trap.
/// @return Borrowed success string, which may be @c NULL.
rt_string rt_result_unwrap_str(void *obj);

/// @brief Unwrap an integer-valued `Ok`.
/// @param obj Valid integer-valued Result; null, `Err`, and type mismatch trap.
/// @return Stored signed integer.
int64_t rt_result_unwrap_i64(void *obj);

/// @brief Unwrap a floating-point-valued `Ok`.
/// @param obj Valid floating-point Result; null, `Err`, and type mismatch trap.
/// @return Stored double-precision value.
double rt_result_unwrap_f64(void *obj);

/// @brief Return an `Ok` pointer payload or a default.
/// @param obj Valid pointer-valued Result; @c NULL is treated as failure.
/// @param def Borrowed fallback for null or `Err`.
/// @return Borrowed success pointer for `Ok`, otherwise @p def.
/// @warning This accessor assumes pointer storage and does not reject typed
///          string/integer/floating `Ok` variants.
void *rt_result_unwrap_or(void *obj, void *def);

/// @brief Return an `Ok` string payload or a default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Borrowed fallback for `Err` or non-string `Ok`.
/// @return Borrowed success string for a matching `Ok`, otherwise @p def.
rt_string rt_result_unwrap_or_str(void *obj, rt_string def);

/// @brief Return an `Ok` integer payload or a default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Fallback for `Err` or non-integer `Ok`.
/// @return Stored integer for a matching `Ok`, otherwise @p def.
int64_t rt_result_unwrap_or_i64(void *obj, int64_t def);

/// @brief Return an `Ok` floating-point payload or a default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Fallback for `Err` or non-floating `Ok`.
/// @return Stored floating-point value for a matching `Ok`, otherwise @p def.
double rt_result_unwrap_or_f64(void *obj, double def);

/// @brief Unwrap a pointer-valued `Err`.
/// @param obj Valid pointer-valued Result; null, `Ok`, and type mismatch trap.
/// @return Borrowed error pointer, which may be @c NULL for `Err(NULL)`.
void *rt_result_unwrap_err(void *obj);

/// @brief Unwrap a string-valued `Err`.
/// @param obj Valid string-valued Result; null, `Ok`, and type mismatch trap.
/// @return Borrowed error string, which may be @c NULL.
rt_string rt_result_unwrap_err_str(void *obj);

/// @brief Probe a pointer-valued `Ok` without trapping.
/// @param obj Valid Result; @c NULL is treated as absent.
/// @return Borrowed success pointer, or @c NULL for null, `Err`, typed
///         non-pointer payloads, or `Ok(NULL)`.
void *rt_result_ok_value(void *obj);

/// @brief Probe a pointer-valued `Err` without trapping.
/// @param obj Valid Result; @c NULL is treated as absent.
/// @return Borrowed error pointer, or @c NULL for null, `Ok`, typed
///         non-pointer payloads, or `Err(NULL)`.
void *rt_result_err_value(void *obj);

//=========================================================================
// Expect (with custom error messages)
//=========================================================================

/// @brief Unwrap a pointer-valued `Ok` or raise an invalid-operation trap.
/// @param obj Valid pointer-valued Result; null, `Err`, and type mismatch trap.
/// @param msg Borrowed diagnostic string; @c NULL selects `"assertion failed"`.
/// @return Borrowed success pointer.
void *rt_result_expect(void *obj, rt_string msg);

/// @brief Unwrap a pointer-valued `Err` or raise an invalid-operation trap.
/// @param obj Valid pointer-valued Result; null, `Ok`, and type mismatch trap.
/// @param msg Borrowed diagnostic string; @c NULL selects `"assertion failed"`.
/// @return Borrowed error pointer.
void *rt_result_expect_err(void *obj, rt_string msg);

//=========================================================================
// Transformation
//=========================================================================

/// @brief Core of @ref rt_result_map with a pluggable callback invoker.
/// @details Single source of truth for the combinator's semantics; used by the
///          native wrapper and VM bridges. A pointer-valued `Ok` is transformed
///          and its callback result retained in a new Result; null, null-callback,
///          `Err`, and typed non-pointer paths pass through without retention.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque transform callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Caller-owned new mapped Result, or borrowed original @p obj.
void *rt_result_map_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Core of @ref rt_result_map_err with a pluggable callback invoker.
/// @details Transforms only a pointer-valued `Err`, retaining the callback
///          result in a new Result. Every other path passes through unchanged.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque error-transform callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Caller-owned new mapped Result, or borrowed original @p obj.
void *rt_result_map_err_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Core of @ref rt_result_and_then with a pluggable callback invoker.
/// @details Invokes only for a pointer-valued `Ok` and returns the callback's
///          Result unchanged. Null, null-callback, `Err`, and typed non-pointer
///          paths pass through.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque Result-returning callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Callback result with strategy-defined ownership, or borrowed
///         original @p obj.
void *rt_result_and_then_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Core of @ref rt_result_or_else with a pluggable callback invoker.
/// @details Invokes only for a pointer-valued `Err` and returns the recovery
///          Result unchanged. Null, null-callback, `Ok`, and typed non-pointer
///          paths pass through.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque recovery callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Callback result with strategy-defined ownership, or borrowed
///         original @p obj.
void *rt_result_or_else_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Transform a pointer-valued `Ok` through a native callback.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Native callback receiving the borrowed success pointer; may be @c NULL.
/// @return Caller-owned new mapped Result, or borrowed original @p obj on
///         null/no-op/pass-through paths.
void *rt_result_map(void *obj, void *(*fn)(void *));

/// @brief Transform a pointer-valued `Err` through a native callback.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Native callback receiving the borrowed error pointer; may be @c NULL.
/// @return Caller-owned new mapped Result, or borrowed original @p obj on
///         null/no-op/pass-through paths.
void *rt_result_map_err(void *obj, void *(*fn)(void *));

/// @brief Chain Result operations (flatMap/andThen).
/// @param obj Valid Result; may be @c NULL.
/// @param fn Native Result-returning callback receiving the borrowed success
///        pointer; may be @c NULL.
/// @return Callback result unchanged for pointer `Ok`, otherwise borrowed
///         original @p obj.
void *rt_result_and_then(void *obj, void *(*fn)(void *));

/// @brief Provide fallback Result if Err (orElse).
/// @param obj Valid Result; may be @c NULL.
/// @param fn Native Result-returning callback receiving the borrowed error
///        pointer; may be @c NULL.
/// @return Callback result unchanged for pointer `Err`, otherwise borrowed
///         original @p obj.
void *rt_result_or_else(void *obj, void *(*fn)(void *));

/// @brief IL-compatible wrapper for @ref rt_result_map accepting an opaque callback pointer.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque native transform callback; may be @c NULL.
/// @return Result of @ref rt_result_map with identical ownership.
void *rt_result_map_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_result_map_err accepting an opaque callback pointer.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque native error-transform callback; may be @c NULL.
/// @return Result of @ref rt_result_map_err with identical ownership.
void *rt_result_map_err_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_result_and_then accepting an opaque callback pointer.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque native Result-returning callback; may be @c NULL.
/// @return Result of @ref rt_result_and_then with identical ownership.
void *rt_result_and_then_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_result_or_else accepting an opaque callback pointer.
/// @param obj Valid Result; may be @c NULL.
/// @param fn Opaque native recovery callback; may be @c NULL.
/// @return Result of @ref rt_result_or_else with identical ownership.
void *rt_result_or_else_wrapper(void *obj, void *fn);

//=========================================================================
// Utility
//=========================================================================

/// @brief Check equality of two Results.
/// @details Requires matching variants and payload tags. Pointer values use
///          identity, strings use content comparison, inline numerics use C
///          equality, and two null Result pointers compare equal.
/// @param a First valid Result or @c NULL.
/// @param b Second valid Result or @c NULL.
/// @return @c 1 when the Results compare equal, otherwise @c 0.
int8_t rt_result_equals(void *a, void *b);

/// @brief Get a string representation of the Result.
/// @param obj Valid Result; @c NULL produces `"Result(null)"`.
/// @return Caller-owned string such as `"Ok(value)"` or `"Err(error)"` for
///         non-null input; null returns an immortal constant string.
/// @note Formatting uses a 256-byte temporary buffer, so long string payloads
///       are truncated to fit.
rt_string rt_result_to_string(void *obj);

#ifdef __cplusplus
}
#endif
