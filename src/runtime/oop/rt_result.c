//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_result.c
// Purpose: Implements the Result<T,E> type (Ok/Err) for the Zanna.Result class.
//          Wraps either a success value or an error value as a heap-allocated
//          object, providing an alternative to exceptions for error propagation.
//
// Key invariants:
//   - Result.Ok(val) selects RESULT_OK; Result.Err(err) selects RESULT_ERR.
//   - IsOk() returns 1 for Ok results; IsErr() returns 1 for Err results.
//   - Unwrap accessors require the expected variant and payload type and trap
//     when used on NULL, the opposite variant, or a mismatched type.
//   - One tagged union stores either pointer, string, integer, or floating data.
//
// Ownership/Lifetime:
//   - Each constructor returns a caller-owned Result reference.
//   - Managed pointer/string payloads are retained until finalization; raw
//     pointer payloads are stored borrowed and numeric payloads are inline.
//   - Extraction and combinator pass-through paths return borrowed references.
//
// Links: src/runtime/oop/rt_result.h (public API),
//        src/runtime/oop/rt_option.h (Option<T>, related present/absent pattern)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_result.c
 * @brief Implements tagged managed Result success and error values.
 * @details Ok and Err variants store pointer, String, integer, or floating
 *          payloads with explicit type tags and retained managed ownership.
 *          Typed extraction, defaults, transformation, recovery, matching, and
 *          Option conversion validate both the variant and stored payload kind.
 */

#include "rt_result.h"
#include "rt_error.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

//=============================================================================
// Internal Structure
//=============================================================================

typedef enum { RESULT_OK = 0, RESULT_ERR = 1 } ResultVariant;

typedef enum { VALUE_PTR = 0, VALUE_STR = 1, VALUE_I64 = 2, VALUE_F64 = 3 } ValueType;

typedef struct {
    ResultVariant variant;
    ValueType value_type;

    union {
        void *ptr;
        rt_string str;
        int64_t i64;
        double f64;
    } value;
} Result;

//=============================================================================
// Result Finalizer
//=============================================================================

/// @brief GC finalizer: release the heap-owned payload (PTR via object refcount, STR via
/// `rt_str_release_maybe`). I64/F64 variants own no heap memory and need no cleanup.
/// @param obj Result payload being finalized; @c NULL requires no cleanup.
static void result_finalizer(void *obj) {
    Result *r = (Result *)obj;
    if (!r)
        return;
    if (r->value_type == VALUE_PTR && r->value.ptr) {
        if (rt_obj_release_check0(r->value.ptr))
            rt_obj_free(r->value.ptr);
        r->value.ptr = NULL;
    } else if (r->value_type == VALUE_STR && r->value.str) {
        rt_str_release_maybe(r->value.str);
        r->value.str = NULL;
    }
}

//=============================================================================
// Result Creation
//=============================================================================

/// @brief Release a provisional Result allocation after constructor setup fails.
/// @param obj Managed Result payload to release and free at zero; @c NULL is ignored.
static void result_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Allocate a zeroed Result payload and report allocation failure.
/// @param what Borrowed operation name used as the diagnostic prefix; @c NULL
///        selects `"Result"`.
/// @return Caller-owned Result payload, or @c NULL after raising an allocation trap.
static Result *result_alloc_trap_safe(const char *what) {
    Result *r = (Result *)rt_obj_new_i64(0, (int64_t)sizeof(Result));
    if (!r) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: allocation failed", what ? what : "Result");
        rt_trap(buf);
        return NULL;
    }
    return r;
}

/// @brief Construct `Ok(ptr)` over a generic pointer payload.
/// @details Retains runtime-managed values after allocating the Result. If the
///          retain traps, the provisional Result is released before rethrowing;
///          unmanaged raw pointers and null are stored without retention.
/// @param value Pointer success payload; may be @c NULL.
/// @return Caller-owned pointer-valued `Ok`, or @c NULL after a trapped setup failure.
void *rt_result_ok(void *value) {
    Result *r = result_alloc_trap_safe("Result.Ok");
    if (!r)
        return NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Result.Ok: retain failed");
        rt_trap_clear_recovery();
        result_release_object(r);
        rt_trap(saved_error);
        return NULL;
    }
    rt_obj_retain_maybe(value);
    rt_trap_clear_recovery();
    r->variant = RESULT_OK;
    r->value_type = VALUE_PTR;
    r->value.ptr = value;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

/// @brief Construct `Ok(string)` and retain the runtime string.
/// @details Heap and literal-pool strings use @ref rt_string_ref; a retain trap
///          releases the provisional Result before being rethrown.
/// @param value Runtime string success payload; may be @c NULL.
/// @return Caller-owned string-valued `Ok`, or @c NULL after a trapped setup failure.
void *rt_result_ok_str(rt_string value) {
    Result *r = result_alloc_trap_safe("Result.OkStr");
    if (!r)
        return NULL;
    rt_string retained = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Result.OkStr: retain failed");
        rt_trap_clear_recovery();
        result_release_object(r);
        rt_trap(saved_error);
        return NULL;
    }
    retained = value ? rt_string_ref(value) : NULL;
    rt_trap_clear_recovery();
    r->variant = RESULT_OK;
    r->value_type = VALUE_STR;
    r->value.str = retained;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

/// @brief Construct `Ok(i64)` with the value stored inline.
/// @param value Signed integer success payload.
/// @return Caller-owned integer-valued `Ok`, or @c NULL after allocation failure.
void *rt_result_ok_i64(int64_t value) {
    Result *r = result_alloc_trap_safe("Result.OkI64");
    if (!r)
        return NULL;
    r->variant = RESULT_OK;
    r->value_type = VALUE_I64;
    r->value.i64 = value;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

/// @brief Construct `Ok(f64)` with the value stored inline.
/// @param value Double-precision success payload.
/// @return Caller-owned floating-point `Ok`, or @c NULL after allocation failure.
void *rt_result_ok_f64(double value) {
    Result *r = result_alloc_trap_safe("Result.OkF64");
    if (!r)
        return NULL;
    r->variant = RESULT_OK;
    r->value_type = VALUE_F64;
    r->value.f64 = value;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

/// @brief Construct `Err(ptr)` carrying an arbitrary heap-managed error value (e.g. an exception
/// object). Retains the error so it survives until the Result is finalized.
/// @details A retain trap releases the provisional Result before being rethrown.
///          Unmanaged raw pointers and null are stored without retention.
/// @param error Pointer error payload; may be @c NULL.
/// @return Caller-owned pointer-valued `Err`, or @c NULL after a trapped setup failure.
void *rt_result_err(void *error) {
    Result *r = result_alloc_trap_safe("Result.Err");
    if (!r)
        return NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Result.Err: retain failed");
        rt_trap_clear_recovery();
        result_release_object(r);
        rt_trap(saved_error);
        return NULL;
    }
    rt_obj_retain_maybe(error);
    rt_trap_clear_recovery();
    r->variant = RESULT_ERR;
    r->value_type = VALUE_PTR;
    r->value.ptr = error;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

/// @brief Construct `Err(message)` with a string error description. The most common Err shape —
/// caller-friendly diagnostic that doesn't require allocating a separate error class.
/// @details Retains heap or literal-pool strings through @ref rt_string_ref.
/// @param message Runtime string error payload; may be @c NULL.
/// @return Caller-owned string-valued `Err`, or @c NULL after a trapped setup failure.
void *rt_result_err_str(rt_string message) {
    Result *r = result_alloc_trap_safe("Result.ErrStr");
    if (!r)
        return NULL;
    rt_string retained = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Result.ErrStr: retain failed");
        rt_trap_clear_recovery();
        result_release_object(r);
        rt_trap(saved_error);
        return NULL;
    }
    retained = message ? rt_string_ref(message) : NULL;
    rt_trap_clear_recovery();
    r->variant = RESULT_ERR;
    r->value_type = VALUE_STR;
    r->value.str = retained;
    rt_obj_set_finalizer(r, result_finalizer);
    return r;
}

//=============================================================================
// Result Inspection
//=============================================================================

/// @brief Check whether the Result is the Ok variant.
/// @param obj Valid Result payload; @c NULL is neither variant.
/// @return @c 1 for `Ok`, otherwise @c 0.
int8_t rt_result_is_ok(void *obj) {
    if (!obj)
        return 0;
    Result *r = (Result *)obj;
    return r->variant == RESULT_OK ? 1 : 0;
}

/// @brief Check whether the Result is the Err variant.
/// @param obj Valid Result payload; @c NULL is neither variant.
/// @return @c 1 for `Err`, otherwise @c 0.
int8_t rt_result_is_err(void *obj) {
    if (!obj)
        return 0;
    Result *r = (Result *)obj;
    return r->variant == RESULT_ERR ? 1 : 0;
}

//=============================================================================
// Value Extraction
//=============================================================================

/// @brief Raise a runtime trap with the supplied unwrap diagnostic.
/// @param msg Borrowed NUL-terminated diagnostic passed directly to @ref rt_trap.
static void trap_with_message(const char *msg) {
    rt_trap(msg);
}

/// @brief Extract the pointer payload from an `Ok` Result.
/// @param obj Valid pointer-valued Result; null, `Err`, and typed non-pointer
///        variants trap.
/// @return Borrowed success pointer, which may be @c NULL for `Ok(NULL)`;
///         trap fallback paths return @c NULL.
void *rt_result_unwrap(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Result");
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK) {
        trap_with_message("Unwrap called on Err Result");
        return NULL;
    }
    if (r->value_type != VALUE_PTR) {
        trap_with_message("Unwrap called on non-object payload; use UnwrapStr/UnwrapI64/UnwrapF64");
        return NULL;
    }
    return r->value.ptr;
}

/// @brief Extract the string payload from an `Ok` Result.
/// @param obj Valid string-valued Result; null, `Err`, and type mismatch trap.
/// @return Borrowed success string, which may be @c NULL; trap fallback paths
///         also return @c NULL.
rt_string rt_result_unwrap_str(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Result");
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK) {
        trap_with_message("Unwrap called on Err Result");
        return NULL;
    }
    if (r->value_type != VALUE_STR) {
        trap_with_message("Unwrap string called on non-string Result");
        return NULL;
    }
    return r->value.str;
}

/// @brief Extract the integer payload from an `Ok` Result.
/// @param obj Valid integer-valued Result; null, `Err`, and type mismatch trap.
/// @return Stored signed integer; trap fallback paths return zero.
int64_t rt_result_unwrap_i64(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Result");
        return 0;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK) {
        trap_with_message("Unwrap called on Err Result");
        return 0;
    }
    if (r->value_type != VALUE_I64) {
        trap_with_message("Unwrap i64 called on non-i64 Result");
        return 0;
    }
    return r->value.i64;
}

/// @brief Extract the floating-point payload from an `Ok` Result.
/// @param obj Valid floating-point Result; null, `Err`, and type mismatch trap.
/// @return Stored double-precision value; trap fallback paths return `0.0`.
double rt_result_unwrap_f64(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Result");
        return 0.0;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK) {
        trap_with_message("Unwrap called on Err Result");
        return 0.0;
    }
    if (r->value_type != VALUE_F64) {
        trap_with_message("Unwrap f64 called on non-f64 Result");
        return 0.0;
    }
    return r->value.f64;
}

/// @brief Return an `Ok` pointer payload or a caller-supplied default.
/// @param obj Valid pointer-valued Result; @c NULL is treated as failure.
/// @param def Borrowed fallback returned for null or `Err`.
/// @return Borrowed success pointer for `Ok`, otherwise @p def.
/// @warning This accessor assumes pointer storage and does not reject typed
///          string/integer/floating `Ok` variants.
void *rt_result_unwrap_or(void *obj, void *def) {
    if (!obj)
        return def;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return def;
    return r->value.ptr;
}

/// @brief Return an `Ok` string payload or a caller-supplied default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Borrowed fallback for `Err` or a non-string `Ok`.
/// @return Borrowed success string for a matching `Ok`, otherwise @p def.
rt_string rt_result_unwrap_or_str(void *obj, rt_string def) {
    if (!obj)
        return def;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return def;
    if (r->value_type != VALUE_STR)
        return def;
    return r->value.str;
}

/// @brief Return an `Ok` integer payload or a caller-supplied default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Fallback for `Err` or a non-integer `Ok`.
/// @return Stored integer for a matching `Ok`, otherwise @p def.
int64_t rt_result_unwrap_or_i64(void *obj, int64_t def) {
    if (!obj)
        return def;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return def;
    if (r->value_type != VALUE_I64)
        return def;
    return r->value.i64;
}

/// @brief Return an `Ok` floating-point payload or a caller-supplied default.
/// @param obj Valid Result; @c NULL is treated as failure.
/// @param def Fallback for `Err` or a non-floating `Ok`.
/// @return Stored floating-point value for a matching `Ok`, otherwise @p def.
double rt_result_unwrap_or_f64(void *obj, double def) {
    if (!obj)
        return def;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return def;
    if (r->value_type != VALUE_F64)
        return def;
    return r->value.f64;
}

/// @brief Extract the Err pointer payload; **traps** if NULL or Ok. Mirror of `unwrap` for the
/// Err side — used to inspect the error after `is_err()` confirms one is present.
/// @param obj Valid pointer-valued Result; null, `Ok`, and typed non-pointer
///        variants trap.
/// @return Borrowed error pointer, which may be @c NULL for `Err(NULL)`;
///         trap fallback paths return @c NULL.
void *rt_result_unwrap_err(void *obj) {
    if (!obj) {
        trap_with_message("UnwrapErr called on NULL Result");
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR) {
        trap_with_message("UnwrapErr called on Ok Result");
        return NULL;
    }
    if (r->value_type != VALUE_PTR) {
        trap_with_message("UnwrapErr called on non-object payload; use UnwrapErrStr");
        return NULL;
    }
    return r->value.ptr;
}

/// @brief Extract the string payload from an `Err` Result.
/// @param obj Valid string-valued Result; null, `Ok`, and type mismatch trap.
/// @return Borrowed error string, which may be @c NULL; trap fallback paths
///         also return @c NULL.
rt_string rt_result_unwrap_err_str(void *obj) {
    if (!obj) {
        trap_with_message("UnwrapErr called on NULL Result");
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR) {
        trap_with_message("UnwrapErr called on Ok Result");
        return NULL;
    }
    if (r->value_type != VALUE_STR) {
        trap_with_message("UnwrapErr string called on non-string Result");
        return NULL;
    }
    return r->value.str;
}

/// @brief Return the Ok pointer if Ok, NULL otherwise. Non-trapping accessor — caller must
/// distinguish "stored NULL" from "this is an Err" via `is_ok` / `is_err`.
/// @param obj Valid Result; @c NULL is treated as absent.
/// @return Borrowed pointer for a pointer-valued `Ok`, or @c NULL for null,
///         `Err`, a typed non-pointer payload, or `Ok(NULL)`.
void *rt_result_ok_value(void *obj) {
    if (!obj)
        return NULL;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return NULL;
    // Generic object accessor: typed payloads (str/i64/f64) must not be
    // reinterpreted as pointers; non-trapping contract returns NULL instead.
    if (r->value_type != VALUE_PTR)
        return NULL;
    return r->value.ptr;
}

/// @brief Return the Err pointer if Err, NULL otherwise.
/// @param obj Valid Result; @c NULL is treated as absent.
/// @return Borrowed pointer for a pointer-valued `Err`, or @c NULL for null,
///         `Ok`, a typed non-pointer payload, or `Err(NULL)`.
void *rt_result_err_value(void *obj) {
    if (!obj)
        return NULL;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR)
        return NULL;
    // See rt_result_ok_value: never expose a typed payload as a pointer.
    if (r->value_type != VALUE_PTR)
        return NULL;
    return r->value.ptr;
}

//=============================================================================
// Expect
//=============================================================================

/// @brief Like `unwrap` with a caller-supplied diagnostic. Traps with INVALID_OPERATION (catchable
/// distinctly from generic unwrap traps) on Err. Use for "this Result must be Ok at this point"
/// invariant checks where the message helps debug failures.
/// @param obj Valid pointer-valued Result; null, `Err`, and type mismatch trap.
/// @param msg Borrowed failure message; @c NULL selects `"assertion failed"`.
/// @return Borrowed pointer success payload on the `Ok` path.
void *rt_result_expect(void *obj, rt_string msg) {
    const char *msg_str = msg ? rt_string_cstr(msg) : "assertion failed";
    char buffer[256];
    if (!obj) {
        snprintf(buffer, sizeof(buffer), "Result expect: %s (NULL Result)", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK) {
        snprintf(buffer, sizeof(buffer), "Result expect: %s", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    if (r->value_type != VALUE_PTR) {
        snprintf(buffer,
                 sizeof(buffer),
                 "Result expect: %s (payload is not an object; use UnwrapStr/UnwrapI64/UnwrapF64)",
                 msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    return r->value.ptr;
}

/// @brief Mirror of `expect` for the Err side: traps with the diagnostic if the Result is Ok.
/// Useful in tests asserting that a fallible operation must have failed with a specific reason.
/// @param obj Valid pointer-valued Result; null, `Ok`, and type mismatch trap.
/// @param msg Borrowed failure message; @c NULL selects `"assertion failed"`.
/// @return Borrowed pointer error payload on the `Err` path.
void *rt_result_expect_err(void *obj, rt_string msg) {
    const char *msg_str = msg ? rt_string_cstr(msg) : "assertion failed";
    char buffer[256];
    if (!obj) {
        snprintf(buffer, sizeof(buffer), "Result expect_err: %s (NULL Result)", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR) {
        snprintf(buffer, sizeof(buffer), "Result expect_err: %s", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    if (r->value_type != VALUE_PTR) {
        snprintf(buffer,
                 sizeof(buffer),
                 "Result expect_err: %s (payload is not an object; use UnwrapErrStr)",
                 msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    return r->value.ptr;
}

//=============================================================================
// Transformation
//=============================================================================

// =============================================================================
// Transformation combinators — all PTR-variant only. Typed-primitive Results
// (STR/I64/F64) pass through unchanged because the function signature
// `void* (*)(void*)` doesn't match the union's other branches. Pair with the
// typed unwrap_*/ok_*_t constructors when you need to transform a typed value.
// =============================================================================

/// @brief Apply a native callback to a pointer-valued `Ok`.
/// @details Null, null-callback, `Err`, and typed non-pointer paths return the
///          original pointer unchanged. A pointer `Ok` wraps and retains the
///          callback result in a newly allocated Result.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Native transform callback receiving the borrowed success pointer;
///        may be @c NULL.
/// @return Caller-owned new Result for a mapped pointer `Ok`; otherwise the
///         borrowed original @p obj.
void *rt_result_map(void *obj, void *(*fn)(void *)) {
    return rt_result_map_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Combinator core shared by the native wrapper and the VM callback bridges.
/// @details The @p invoke strategy abstracts direct native calls and VM
///          interpreter re-entry. It runs only for a pointer-valued `Ok`;
///          the callback result is retained by a newly allocated `Ok`. Every
///          other path returns @p obj unchanged without adding a reference.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque transform callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Caller-owned new mapped Result, or borrowed original @p obj on
///         null/no-op/pass-through paths.
void *rt_result_map_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return obj;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return obj;

    // For pointer values, apply the function
    if (r->value_type == VALUE_PTR) {
        void *new_val = invoke(ctx, fn, r->value.ptr);
        return rt_result_ok(new_val);
    }
    // For other types, return as-is (can't map non-pointer)
    return obj;
}

/// @brief Apply `fn` to the Err value, returning `Err(fn(err))`. Ok passes through unchanged.
/// Useful for translating low-level errors into higher-level error types.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Native transform callback receiving the borrowed error pointer;
///        may be @c NULL.
/// @return Caller-owned new Result for a mapped pointer `Err`; otherwise the
///         borrowed original @p obj.
void *rt_result_map_err(void *obj, void *(*fn)(void *)) {
    return rt_result_map_err_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Apply a pluggable callback to a pointer-valued `Err`.
/// @details Wraps and retains the callback result in a new `Err`. Null,
///          null-callback, `Ok`, and typed non-pointer paths return @p obj
///          unchanged without retaining it.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque error-transform callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Caller-owned new mapped Result, or borrowed original @p obj.
void *rt_result_map_err_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return obj;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR)
        return obj;

    if (r->value_type == VALUE_PTR) {
        void *new_val = invoke(ctx, fn, r->value.ptr);
        return rt_result_err(new_val);
    }
    return obj;
}

/// @brief Monadic bind on the Ok side: apply `fn` (returning a Result) to the Ok value, flattening.
/// Err short-circuits the chain, propagating unchanged. The cornerstone of error pipelines.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Native Result-returning callback receiving the borrowed success
///        pointer; may be @c NULL.
/// @return Callback result unchanged for a pointer `Ok`; otherwise the borrowed
///         original @p obj.
void *rt_result_and_then(void *obj, void *(*fn)(void *)) {
    return rt_result_and_then_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Run a pluggable Result-returning callback for a pointer-valued `Ok`.
/// @details The callback result is returned without retention or wrapping.
///          Null, null-callback, `Err`, and typed non-pointer paths pass through.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque Result-returning callback handle; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Callback result with strategy-defined ownership, or borrowed
///         original @p obj on pass-through paths.
void *rt_result_and_then_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return obj;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_OK)
        return obj;

    if (r->value_type == VALUE_PTR) {
        return invoke(ctx, fn, r->value.ptr);
    }
    return obj;
}

/// @brief Monadic bind on the Err side: apply `fn` (returning a Result) to the Err value to
/// produce a recovery Result. Ok passes through unchanged. Used for "try recovery" patterns.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Native Result-returning callback receiving the borrowed error
///        pointer; may be @c NULL.
/// @return Callback result unchanged for a pointer `Err`; otherwise the
///         borrowed original @p obj.
void *rt_result_or_else(void *obj, void *(*fn)(void *)) {
    return rt_result_or_else_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Run a pluggable recovery callback for a pointer-valued `Err`.
/// @details The callback result is returned without retention or wrapping.
///          Null, null-callback, `Ok`, and typed non-pointer paths pass through.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque Result-returning recovery callback; may be @c NULL.
/// @param invoke Non-NULL strategy whenever callback execution is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Callback result with strategy-defined ownership, or borrowed
///         original @p obj on pass-through paths.
void *rt_result_or_else_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return obj;
    Result *r = (Result *)obj;
    if (r->variant != RESULT_ERR)
        return obj;

    if (r->value_type == VALUE_PTR) {
        return invoke(ctx, fn, r->value.ptr);
    }
    return obj;
}

/// @brief IL trampoline for @ref rt_result_map.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque native transform callback; may be @c NULL.
/// @return Result of @ref rt_result_map with identical ownership.
void *rt_result_map_wrapper(void *obj, void *fn) {
    return rt_result_map(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for @ref rt_result_map_err.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque native error-transform callback; may be @c NULL.
/// @return Result of @ref rt_result_map_err with identical ownership.
void *rt_result_map_err_wrapper(void *obj, void *fn) {
    return rt_result_map_err(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for @ref rt_result_and_then.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque native Result-returning callback; may be @c NULL.
/// @return Result of @ref rt_result_and_then with identical ownership.
void *rt_result_and_then_wrapper(void *obj, void *fn) {
    return rt_result_and_then(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for @ref rt_result_or_else.
/// @param obj Valid Result payload; may be @c NULL.
/// @param fn Opaque native recovery callback; may be @c NULL.
/// @return Result of @ref rt_result_or_else with identical ownership.
void *rt_result_or_else_wrapper(void *obj, void *fn) {
    return rt_result_or_else(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

//=============================================================================
// Utility
//=============================================================================

/// @brief Compare two Result instances for structural equality.
/// @details Variants and payload tags must match. Pointer payloads use identity,
///          strings use content comparison, integers use numeric equality, and
///          floating values use C `==` semantics. Identical pointers, including
///          two null pointers, compare equal.
/// @param a First valid Result payload, or @c NULL.
/// @param b Second valid Result payload, or @c NULL.
/// @return @c 1 when the Result values compare equal; otherwise @c 0.
int8_t rt_result_equals(void *a, void *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;

    Result *ra = (Result *)a;
    Result *rb = (Result *)b;

    if (ra->variant != rb->variant)
        return 0;
    if (ra->value_type != rb->value_type)
        return 0;

    switch (ra->value_type) {
        case VALUE_PTR:
            return ra->value.ptr == rb->value.ptr ? 1 : 0;
        case VALUE_STR:
            return rt_str_cmp(ra->value.str, rb->value.str) == 0 ? 1 : 0;
        case VALUE_I64:
            return ra->value.i64 == rb->value.i64 ? 1 : 0;
        case VALUE_F64:
            // IEEE 754: NaN != NaN, so Result(NaN).Equals(Result(NaN)) returns 0.
            return ra->value.f64 == rb->value.f64 ? 1 : 0;
    }
    return 0;
}

/// @brief Convert a Result to a bounded human-readable representation.
/// @details Formats the active variant and typed payload as `Ok(...)` or
///          `Err(...)`. Pointer values use `%p`; strings are quoted; numeric
///          values use their standard `snprintf` formats.
/// @param obj Valid Result payload; @c NULL produces `"Result(null)"`.
/// @return Caller-owned formatted runtime string for non-null input, or an
///         immortal constant string for null.
/// @note Output is limited to the 255 characters that fit before the temporary
///       buffer's terminator, so long string payloads are truncated.
rt_string rt_result_to_string(void *obj) {
    if (!obj)
        return rt_const_cstr("Result(null)");

    Result *r = (Result *)obj;
    char buf[256];

    if (r->variant == RESULT_OK) {
        switch (r->value_type) {
            case VALUE_PTR:
                snprintf(buf, sizeof(buf), "Ok(%p)", r->value.ptr);
                break;
            case VALUE_STR:
                snprintf(buf, sizeof(buf), "Ok(\"%s\")", rt_string_cstr(r->value.str));
                break;
            case VALUE_I64:
                snprintf(buf, sizeof(buf), "Ok(%lld)", (long long)r->value.i64);
                break;
            case VALUE_F64:
                snprintf(buf, sizeof(buf), "Ok(%g)", r->value.f64);
                break;
        }
    } else {
        switch (r->value_type) {
            case VALUE_PTR:
                snprintf(buf, sizeof(buf), "Err(%p)", r->value.ptr);
                break;
            case VALUE_STR:
                snprintf(buf, sizeof(buf), "Err(\"%s\")", rt_string_cstr(r->value.str));
                break;
            case VALUE_I64:
                snprintf(buf, sizeof(buf), "Err(%lld)", (long long)r->value.i64);
                break;
            case VALUE_F64:
                snprintf(buf, sizeof(buf), "Err(%g)", r->value.f64);
                break;
        }
    }

    return rt_string_from_bytes(buf, strlen(buf));
}
