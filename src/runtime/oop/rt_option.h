//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/oop/rt_option.h
// Purpose: Option type representing either Some(value) or None, with typed extraction,
// transformation (map, flat_map), and conversion to Result.
//
// Key invariants:
//   - An Option is always exactly one of Some or None.
//   - rt_option_unwrap* traps on None/type mismatch; rt_option_unwrap_or*
//     provides non-trapping typed defaults.
//   - Pointer, string, integer, and floating payload tags are distinct and
//     must be accessed through their matching API.
//   - Callback combinators operate only on pointer-valued Some variants unless noted.
//
// Ownership/Lifetime:
//   - Constructors return reference-counted, caller-owned opaque Option objects.
//   - Managed pointer and string values are retained while stored; inline
//     numeric values own no heap reference.
//   - Extraction returns borrowed stored references. Combinator pass-through
//     paths return the original object without adding a reference.
//
// Links: src/runtime/oop/rt_option.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_option.h
 * @brief Declares tagged Some and None values and Option combinators.
 * @details Constructors create pointer, String, integer, Boolean, or floating
 *          Some variants and empty None values. Inspection, typed extraction,
 *          defaults, mapping, flat-mapping, filtering, and Result conversion
 *          preserve the documented managed-reference ownership rules.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

/// @brief Runtime heap class identifier assigned to opaque Option payloads.
#define RT_OPTION_CLASS_ID INT64_C(-0x430401)

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Option Creation
//=========================================================================

/// @brief Create a pointer-valued `Some`.
/// @details Retains runtime-managed pointer values; unmanaged raw pointers are
///          stored borrowed. Null is a valid payload and still produces `Some`.
/// @param value Pointer payload to store; may be @c NULL.
/// @return Caller-owned Option object, or @c NULL after an allocation failure trap.
void *rt_option_some(void *value);

/// @brief Create a string-valued `Some`.
/// @param value Runtime string retained by the Option; may be @c NULL.
/// @return Caller-owned Option object, or @c NULL after an allocation failure trap.
void *rt_option_some_str(rt_string value);

/// @brief Create an integer-valued `Some`.
/// @param value Signed integer stored inline.
/// @return Caller-owned Option object, or @c NULL after an allocation failure trap.
void *rt_option_some_i64(int64_t value);

/// @brief Create a normalized boolean `Some` using the integer payload tag.
/// @param value Boolean-like value; zero stores 0 and nonzero stores 1.
/// @return Caller-owned Option object, or @c NULL after an allocation failure trap.
void *rt_option_some_i1(int8_t value);

/// @brief Create a floating-point-valued `Some`.
/// @param value Double-precision value stored inline.
/// @return Caller-owned Option object, or @c NULL after an allocation failure trap.
void *rt_option_some_f64(double value);

/// @brief Create an empty `None` option.
/// @return Caller-owned Option object.
void *rt_option_none(void);

//=========================================================================
// Option Inspection
//=========================================================================

/// @brief Check whether an option is `Some`.
/// @param obj Valid Option object; @c NULL is treated as absent.
/// @return @c 1 for `Some`, otherwise @c 0.
int8_t rt_option_is_some(void *obj);

/// @brief Check whether an option is `None`.
/// @param obj Valid Option object; @c NULL is treated as `None`.
/// @return @c 1 for `None` or null, otherwise @c 0.
int8_t rt_option_is_none(void *obj);

//=========================================================================
// Value Extraction
//=========================================================================

/// @brief Unwrap a pointer-valued `Some`.
/// @param obj Valid pointer-valued Option; null, `None`, and typed non-pointer
///        variants trap.
/// @return Borrowed contained pointer, which may be @c NULL for `Some(NULL)`.
void *rt_option_unwrap(void *obj);

/// @brief Unwrap a string-valued `Some`.
/// @param obj Valid string-valued Option; null, `None`, and type mismatch trap.
/// @return Borrowed contained runtime string, which may be @c NULL.
rt_string rt_option_unwrap_str(void *obj);

/// @brief Unwrap an integer-valued `Some`.
/// @param obj Valid integer-valued Option; null, `None`, and type mismatch trap.
/// @return Stored signed integer.
int64_t rt_option_unwrap_i64(void *obj);

/// @brief Unwrap and normalize an integer-valued Option as boolean.
/// @param obj Valid integer-valued Option; invalid variants trap.
/// @return @c 0 when the stored integer is zero, otherwise @c 1.
int8_t rt_option_unwrap_i1(void *obj);

/// @brief Unwrap a floating-point-valued `Some`.
/// @param obj Valid floating-point Option; null, `None`, and type mismatch trap.
/// @return Stored double-precision value.
double rt_option_unwrap_f64(void *obj);

/// @brief Return a pointer-valued `Some` payload or a default.
/// @param obj Valid pointer-valued Option; @c NULL is treated as `None`.
/// @param def Borrowed fallback returned for null or `None`.
/// @return Borrowed contained pointer for `Some`, otherwise @p def.
/// @warning This accessor assumes pointer storage and does not reject typed
///          string/integer/floating `Some` variants.
void *rt_option_unwrap_or(void *obj, void *def);

/// @brief Return a string-valued `Some` payload or a default.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param def Borrowed fallback for absent or non-string variants.
/// @return Borrowed contained string for a matching `Some`, otherwise @p def.
rt_string rt_option_unwrap_or_str(void *obj, rt_string def);

/// @brief Return an integer-valued `Some` payload or a default.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param def Fallback for absent or non-integer variants.
/// @return Stored integer for a matching `Some`, otherwise @p def.
int64_t rt_option_unwrap_or_i64(void *obj, int64_t def);

/// @brief Return a normalized boolean payload or normalized default.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param def Boolean-like fallback for absent or non-integer variants.
/// @return @c 0 when the selected integer is zero, otherwise @c 1.
int8_t rt_option_unwrap_or_i1(void *obj, int8_t def);

/// @brief Return a floating-point-valued `Some` payload or a default.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param def Fallback for absent or non-floating variants.
/// @return Stored floating-point value for a matching `Some`, otherwise @p def.
double rt_option_unwrap_or_f64(void *obj, double def);

/// @brief Non-trapping pointer-payload probe.
/// @param obj Valid Option; @c NULL is treated as absent.
/// @return Borrowed pointer for a pointer-valued `Some`, or @c NULL for null,
///         `None`, any typed non-pointer variant, or `Some(NULL)`.
void *rt_option_value(void *obj);

//=========================================================================
// Expect (with custom error messages)
//=========================================================================

/// @brief Unwrap a pointer-valued `Some` or raise an invalid-operation trap.
/// @param obj Valid pointer-valued Option; null, `None`, and type mismatch trap.
/// @param msg Borrowed failure message; @c NULL selects `"assertion failed"`.
/// @return Borrowed contained pointer on success.
void *rt_option_expect(void *obj, rt_string msg);

//=========================================================================
// Transformation
//=========================================================================

/// @brief Strategy for invoking a one-argument, value-returning user callback.
/// @details VM bridges supply an invoker that executes the opaque function
///          handle on the interpreter; native code uses a direct C call. `ctx`
///          is the bridge's context pointer and is passed through unchanged.
/// @param ctx Borrowed invocation context supplied by the bridge.
/// @param fn Opaque user callback handle.
/// @param arg Borrowed callback argument.
/// @return Callback result with ownership defined by the invocation strategy.
typedef void *(*rt_cb_invoke1)(void *ctx, void *fn, void *arg);

/// @brief Strategy for invoking a zero-argument, value-returning user callback.
/// @param ctx Borrowed invocation context supplied by the bridge.
/// @param fn Opaque user callback handle.
/// @return Callback result with ownership defined by the invocation strategy.
typedef void *(*rt_cb_invoke0)(void *ctx, void *fn);

/// @brief Strategy for invoking a one-argument boolean predicate callback.
/// @param ctx Borrowed invocation context supplied by the bridge.
/// @param fn Opaque user predicate handle.
/// @param arg Borrowed predicate argument.
/// @return Predicate result interpreted as false when zero and true otherwise.
typedef int8_t (*rt_cb_invoke_pred)(void *ctx, void *fn, void *arg);

/// @brief Direct-call invoker: treats `fn` as a C function pointer `void*(*)(void*)`.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL native callback pointer.
/// @param arg Borrowed argument passed to the callback.
/// @return Native callback result without ownership adjustment.
void *rt_cb_direct_invoke1(void *ctx, void *fn, void *arg);

/// @brief Direct-call invoker: treats `fn` as a C function pointer `void*(*)(void)`.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL native callback pointer.
/// @return Native callback result without ownership adjustment.
void *rt_cb_direct_invoke0(void *ctx, void *fn);

/// @brief Direct-call invoker: treats `fn` as a C predicate pointer `int8_t(*)(void*)`.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL native predicate pointer.
/// @param arg Borrowed argument passed to the predicate.
/// @return Predicate result exactly as returned by the native callback.
int8_t rt_cb_direct_invoke_pred(void *ctx, void *fn, void *arg);

/// @brief Core of @ref rt_option_map with a pluggable callback invoker.
/// @details Single source of truth for the combinator's semantics; used by the
///          native wrapper and VM bridges. Pointer-valued `Some` invokes the
///          callback and wraps/retains its result in a new Option. Null,
///          null-callback, and `None` allocate `None`; typed non-pointer `Some`
///          values pass through unchanged without invocation.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque transform callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Caller-owned new Option for mapped/absent paths, or borrowed
///         original @p obj for a typed non-pointer `Some`.
void *rt_option_map_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Core of @ref rt_option_and_then with a pluggable callback invoker.
/// @details A pointer-valued `Some` returns the callback's Option unchanged;
///          absent paths allocate `None`, and typed non-pointer `Some` values
///          pass through without invocation.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque Option-returning callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Callback result with strategy-defined ownership, a caller-owned new
///         `None`, or the borrowed original typed Option.
void *rt_option_and_then_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx);

/// @brief Core of @ref rt_option_or_else with a pluggable callback invoker.
/// @details `Some` passes through without callback execution. Null and `None`
///          invoke the fallback when supplied or allocate a new `None`.
/// @param obj Valid Option; @c NULL is treated as absent.
/// @param fn Opaque fallback callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Borrowed original `Some`, callback result with strategy-defined
///         ownership, or caller-owned new `None`.
void *rt_option_or_else_invoke(void *obj, void *fn, rt_cb_invoke0 invoke, void *ctx);

/// @brief Core of @ref rt_option_filter with a pluggable callback invoker.
/// @details Invokes the predicate only for pointer-valued `Some`. Passing
///          returns the original Option; false, absent, null-predicate, and
///          typed non-pointer paths allocate `None`.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque predicate handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever predicate execution is required.
/// @param ctx Borrowed strategy context forwarded to @p invoke.
/// @return Borrowed original pointer Option on success, otherwise caller-owned
///         new `None`.
void *rt_option_filter_invoke(void *obj, void *fn, rt_cb_invoke_pred invoke, void *ctx);

/// @brief Transform a pointer-valued `Some` through a native callback.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Native callback receiving the borrowed pointer payload; may be @c NULL.
/// @return Caller-owned new Option for mapped/absent paths, or borrowed
///         original @p obj for a typed non-pointer `Some`.
void *rt_option_map(void *obj, void *(*fn)(void *));

/// @brief Chain Option operations (flatMap/andThen).
/// @param obj Valid Option; may be @c NULL.
/// @param fn Native Option-returning callback receiving the borrowed pointer
///        payload; may be @c NULL.
/// @return Callback result unchanged for pointer `Some`, caller-owned new
///         `None` for absent paths, or borrowed original typed Option.
void *rt_option_and_then(void *obj, void *(*fn)(void *));

/// @brief Provide fallback Option if None (orElse).
/// @param obj Valid Option; @c NULL is treated as absent.
/// @param fn Native zero-argument fallback callback; may be @c NULL.
/// @return Borrowed original `Some`, callback result unchanged, or caller-owned
///         new `None` when no fallback is supplied.
void *rt_option_or_else(void *obj, void *(*fn)(void));

/// @brief Retain a pointer-valued `Some` only when a predicate passes.
/// @param obj Valid Option; may be @c NULL.
/// @param pred Native predicate receiving the borrowed pointer payload; may be @c NULL.
/// @return Borrowed original Option when the predicate succeeds, otherwise
///         caller-owned new `None`.
void *rt_option_filter(void *obj, int8_t (*pred)(void *));

/// @brief IL-compatible wrapper for @ref rt_option_map accepting an opaque callback pointer.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque native transform callback; may be @c NULL.
/// @return Result of @ref rt_option_map with identical ownership.
void *rt_option_map_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_option_and_then accepting an opaque callback pointer.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque native Option-returning callback; may be @c NULL.
/// @return Result of @ref rt_option_and_then with identical ownership.
void *rt_option_and_then_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_option_or_else accepting an opaque callback pointer.
/// @param obj Valid Option; may be @c NULL.
/// @param fn Opaque native zero-argument fallback; may be @c NULL.
/// @return Result of @ref rt_option_or_else with identical ownership.
void *rt_option_or_else_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_option_filter accepting an opaque callback pointer.
/// @param obj Valid Option; may be @c NULL.
/// @param pred Opaque native predicate callback; may be @c NULL.
/// @return Result of @ref rt_option_filter with identical ownership.
void *rt_option_filter_wrapper(void *obj, void *pred);

//=========================================================================
// Conversion
//=========================================================================

/// @brief Convert Option to Result (Some->Ok, None->Err).
/// @details Preserves the stored pointer/string/integer/floating type and
///          retains the selected managed payload in a new Result.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param err Pointer error payload used for null/`None`.
/// @return Caller-owned typed `Ok` for `Some`, otherwise pointer-valued `Err`.
void *rt_option_ok_or(void *obj, void *err);

/// @brief Convert Option to Result with error string.
/// @details Preserves a `Some` payload's stored type and creates a string-valued
///          `Err` for absence.
/// @param obj Valid Option; @c NULL is treated as `None`.
/// @param err Runtime string retained by the new absent-path Result.
/// @return Caller-owned typed `Ok` for `Some`, otherwise string-valued `Err`.
void *rt_option_ok_or_str(void *obj, rt_string err);

//=========================================================================
// Utility
//=========================================================================

/// @brief Check equality of two Options.
/// @details Null compares like `None`; pointer values use identity, strings use
///          content, and inline numbers use C equality.
/// @param a First valid Option or @c NULL.
/// @param b Second valid Option or @c NULL.
/// @return @c 1 when variants, value tags, and payloads compare equal; otherwise @c 0.
int8_t rt_option_equals(void *a, void *b);

/// @brief Get a string representation of the Option.
/// @param obj Valid Option; @c NULL renders as `None`.
/// @return Runtime string such as `"Some(value)"` or `"None"`; `Some` paths
///         allocate a caller-owned string and `None` returns an immortal constant.
rt_string rt_option_to_string(void *obj);

#ifdef __cplusplus
}
#endif
