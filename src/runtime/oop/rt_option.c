//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_option.c
// Purpose: Implements the Option<T> type (Some/None) for the Zanna.Option class.
//          Wraps an optional value as a heap-allocated object with a presence
//          flag, allowing functions to return "no value" without using NULL.
//
// Key invariants:
//   - Option.None() creates the OPTION_NONE variant with no active payload.
//   - Option.Some(val) creates OPTION_SOME with an explicit payload type tag.
//   - IsSome() returns 1 for Some; IsNone() returns 1 for None and NULL.
//   - Unwrap accessors trap on None or a mismatched stored value type.
//   - Pointer and string payloads are retained on creation and released on finalize.
//   - Integer and floating-point payloads are stored inline and own no heap reference.
//
// Ownership/Lifetime:
//   - Each constructor returns a caller-owned Option reference.
//   - Options retain managed pointer/string payloads until their finalizer runs;
//     raw pointer values are stored borrowed because they have no runtime refcount.
//   - Extraction returns borrowed stored references rather than retaining them.
//
// Links: src/runtime/oop/rt_option.h (public API),
//        src/runtime/oop/rt_result.h (Result<T,E> type, related pattern)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_option.c
 * @brief Implements tagged managed Option values and combinators.
 * @details Some variants store pointer, String, integer, or floating payloads,
 *          retaining managed references until finalization, while None carries
 *          no value. Typed unwrap, defaulting, mapping, flat-mapping, filtering,
 *          and Result conversion enforce variant and payload-tag semantics.
 */

#include "rt_option.h"
#include "rt_error.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
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

typedef enum { OPTION_SOME = 0, OPTION_NONE = 1 } OptionVariant;

typedef enum { VALUE_PTR = 0, VALUE_STR = 1, VALUE_I64 = 2, VALUE_F64 = 3 } ValueType;

typedef struct {
    OptionVariant variant;
    ValueType value_type;

    union {
        void *ptr;
        rt_string str;
        int64_t i64;
        double f64;
    } value;
} Option;

//=============================================================================
// Option Finalizer
//=============================================================================

/// @brief GC finalizer: releases the contained reference for SOME variants. PTR variants release
/// via the generic object path; STR variants use `rt_str_release_maybe` (which handles literal
/// vs heap strings). I64/F64 variants own no heap memory and need no cleanup.
/// @param obj Option payload being finalized; @c NULL and non-Some payloads
///        require no cleanup.
static void option_finalizer(void *obj) {
    Option *o = (Option *)obj;
    if (!o || o->variant != OPTION_SOME)
        return;
    if (o->value_type == VALUE_PTR && o->value.ptr) {
        if (rt_obj_release_check0(o->value.ptr))
            rt_obj_free(o->value.ptr);
        o->value.ptr = NULL;
    } else if (o->value_type == VALUE_STR && o->value.str) {
        rt_str_release_maybe(o->value.str);
        o->value.str = NULL;
    }
}

/// @brief Retain a generic Option pointer payload with an explicit status.
/// @details Preserves `rt_obj_retain_maybe` compatibility for null and
///          unmanaged opaque pointers, but requires registered strings and
///          managed heap payloads to acquire ownership before publication.
/// @param value Candidate pointer payload; may be null or unmanaged.
/// @return One when publication is safe; zero after a returning retain trap.
static int option_retain_ptr_checked(void *value) {
    if (!value)
        return 1;
    if (rt_string_is_handle(value))
        return rt_string_ref((rt_string)value) ? 1 : 0;
    if (!rt_heap_is_payload(value))
        return 1;

    int32_t status = rt_heap_try_retain_live(value);
    if (status == 1 || status == 2)
        return 1;
    rt_trap(status < 0 ? "Option.Some: refcount overflow"
                       : "Option.Some: invalid or released payload");
    return 0;
}

//=============================================================================
// Option Creation
//=============================================================================

/// @brief Construct `Some(value)` over a generic pointer payload. Retains `value` via the heap
/// refcount path (so it survives until the Option is finalized).
/// @details Runtime-managed strings and heap payloads gain a retained reference;
///          null and unmanaged raw pointers are stored without retention. If
///          Option allocation traps, the provisional managed retain is released
///          before the diagnostic is rethrown.
/// @param value Pointer payload to wrap; may be @c NULL.
/// @return Caller-owned `Some` option whose pointer payload is @p value, or
///         @c NULL after an allocation failure trap.
void *rt_option_some(void *value) {
    if (!option_retain_ptr_checked(value))
        return NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Option.Some: allocation failed");
        rt_trap_clear_recovery();
        if (rt_obj_release_check0(value))
            rt_obj_free(value);
        rt_trap(saved_error);
        return NULL;
    }

    Option *o = (Option *)rt_obj_new_i64(RT_OPTION_CLASS_ID, (int64_t)sizeof(Option));
    if (!o) {
        rt_trap_clear_recovery();
        if (rt_obj_release_check0(value))
            rt_obj_free(value);
        return NULL;
    }

    o->variant = OPTION_SOME;
    o->value_type = VALUE_PTR;
    o->value.ptr = value;
    rt_obj_set_finalizer(o, option_finalizer);
    rt_trap_clear_recovery();
    return o;
}

/// @brief Construct `Some(string)`. Retains the string via `rt_string_ref` (handles both heap
/// and literal-pool strings); accepts NULL (stored as NULL).
/// @details The provisional retained string is released if allocating the
///          Option fails.
/// @param value Runtime string to wrap; may be @c NULL while still producing `Some`.
/// @return Caller-owned string-valued `Some` option, or @c NULL after an
///         allocation failure trap.
void *rt_option_some_str(rt_string value) {
    rt_string retained = value ? rt_string_ref(value) : NULL;
    if (value && !retained)
        return NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "Option.SomeStr: allocation failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe(retained);
        rt_trap(saved_error);
        return NULL;
    }

    Option *o = (Option *)rt_obj_new_i64(RT_OPTION_CLASS_ID, (int64_t)sizeof(Option));
    if (!o) {
        rt_trap_clear_recovery();
        rt_str_release_maybe(retained);
        return NULL;
    }

    o->variant = OPTION_SOME;
    o->value_type = VALUE_STR;
    o->value.str = retained;
    rt_obj_set_finalizer(o, option_finalizer);
    rt_trap_clear_recovery();
    return o;
}

/// @brief Construct `Some(i64)` with the value stored inline in the union.
/// @param value Signed integer payload to store; no heap reference is retained.
/// @return Caller-owned integer-valued `Some` option, or @c NULL after an
///         allocation failure trap.
void *rt_option_some_i64(int64_t value) {
    Option *o = (Option *)rt_obj_new_i64(RT_OPTION_CLASS_ID, (int64_t)sizeof(Option));
    if (!o) {
        rt_trap("Option.SomeI64: allocation failed");
        return NULL;
    }

    o->variant = OPTION_SOME;
    o->value_type = VALUE_I64;
    o->value.i64 = value;
    rt_obj_set_finalizer(o, option_finalizer);
    return o;
}

/// @brief Construct `Some(i1)` as a normalized inline integer payload.
/// @param value Boolean-like input; zero becomes 0 and every nonzero value becomes 1.
/// @return Caller-owned integer-backed `Some` option, or @c NULL after an
///         allocation failure trap.
void *rt_option_some_i1(int8_t value) {
    return rt_option_some_i64(value ? 1 : 0);
}

/// @brief Construct `Some(f64)` with the value stored inline.
/// @param value Floating-point payload to store; no heap reference is retained.
/// @return Caller-owned floating-point `Some` option, or @c NULL after an
///         allocation failure trap.
void *rt_option_some_f64(double value) {
    Option *o = (Option *)rt_obj_new_i64(RT_OPTION_CLASS_ID, (int64_t)sizeof(Option));
    if (!o) {
        rt_trap("Option.SomeF64: allocation failed");
        return NULL;
    }

    o->variant = OPTION_SOME;
    o->value_type = VALUE_F64;
    o->value.f64 = value;
    rt_obj_set_finalizer(o, option_finalizer);
    return o;
}

/// @brief Construct the empty Option (`None`).
/// @details Initializes the unused payload slot to a null pointer and installs
///          the common finalizer for a uniform object layout.
/// @return Caller-owned `None` option.
void *rt_option_none(void) {
    Option *o = (Option *)rt_obj_new_i64(RT_OPTION_CLASS_ID, (int64_t)sizeof(Option));
    if (!o) {
        rt_trap("Option.None: allocation failed");
        return NULL;
    }

    o->variant = OPTION_NONE;
    o->value_type = VALUE_PTR;
    o->value.ptr = NULL;
    rt_obj_set_finalizer(o, option_finalizer);
    return o;
}

//=============================================================================
// Option Inspection
//=============================================================================

/// @brief Check whether the Option contains a value (is the Some variant).
/// @param obj Valid Option payload to inspect; @c NULL is treated as absent.
/// @return @c 1 for `Some`, otherwise @c 0.
int8_t rt_option_is_some(void *obj) {
    if (!obj)
        return 0;
    Option *o = (Option *)obj;
    return o->variant == OPTION_SOME ? 1 : 0;
}

/// @brief Check whether the Option is empty (is the None variant).
/// @param obj Valid Option payload to inspect; @c NULL is treated as `None`.
/// @return @c 1 for `None` or null, otherwise @c 0.
int8_t rt_option_is_none(void *obj) {
    if (!obj)
        return 1; // Treat NULL as None
    Option *o = (Option *)obj;
    return o->variant == OPTION_NONE ? 1 : 0;
}

//=============================================================================
// Value Extraction
//=============================================================================

#include "rt_trap.h"

/// @brief Raise a runtime trap with the supplied unwrap diagnostic.
/// @param msg Borrowed NUL-terminated diagnostic passed directly to @ref rt_trap.
static void trap_with_message(const char *msg) {
    rt_trap(msg);
}

/// @brief Extract the pointer payload from a Some option; **traps** if NULL or None. Use this
/// when you've already proven (via `is_some`) that the option holds a value.
/// @param obj Valid pointer-valued Option payload to unwrap.
/// @return Borrowed contained pointer, which may itself be @c NULL for `Some(NULL)`;
///         trap fallback paths return @c NULL.
void *rt_option_unwrap(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Option");
        return NULL;
    }
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME) {
        trap_with_message("Unwrap called on None Option");
        return NULL;
    }
    if (o->value_type != VALUE_PTR) {
        trap_with_message(
            "Unwrap called on non-object payload; use UnwrapStr/UnwrapI64/UnwrapI1/UnwrapF64");
        return NULL;
    }
    return o->value.ptr;
}

/// @brief Extract the string value from a Some option.
/// @param obj Valid string-valued Option payload; null, `None`, and non-string
///        variants trap.
/// @return Borrowed contained string handle, which may be @c NULL; trap fallback
///         paths also return @c NULL.
rt_string rt_option_unwrap_str(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Option");
        return NULL;
    }
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME) {
        trap_with_message("Unwrap called on None Option");
        return NULL;
    }
    if (o->value_type != VALUE_STR) {
        trap_with_message("Unwrap string called on non-string Option");
        return NULL;
    }
    return o->value.str;
}

/// @brief Extract the integer value from a Some option.
/// @param obj Valid integer-valued Option payload; null, `None`, and non-integer
///        variants trap.
/// @return Stored signed integer; trap fallback paths return zero.
int64_t rt_option_unwrap_i64(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Option");
        return 0;
    }
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME) {
        trap_with_message("Unwrap called on None Option");
        return 0;
    }
    if (o->value_type != VALUE_I64) {
        trap_with_message("Unwrap i64 called on non-i64 Option");
        return 0;
    }
    return o->value.i64;
}

/// @brief Extract a normalized boolean value from an integer-valued Some option.
/// @param obj Valid integer-valued Option payload; invalid variants trap through
///        @ref rt_option_unwrap_i64.
/// @return @c 0 when the stored integer is zero, otherwise @c 1.
int8_t rt_option_unwrap_i1(void *obj) {
    return rt_option_unwrap_i64(obj) ? 1 : 0;
}

/// @brief Extract the floating-point value from a Some option.
/// @param obj Valid floating-point Option payload; null, `None`, and non-floating
///        variants trap.
/// @return Stored floating-point value; trap fallback paths return `0.0`.
double rt_option_unwrap_f64(void *obj) {
    if (!obj) {
        trap_with_message("Unwrap called on NULL Option");
        return 0.0;
    }
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME) {
        trap_with_message("Unwrap called on None Option");
        return 0.0;
    }
    if (o->value_type != VALUE_F64) {
        trap_with_message("Unwrap f64 called on non-f64 Option");
        return 0.0;
    }
    return o->value.f64;
}

/// @brief Return the wrapped pointer if present, otherwise `def`. Never traps. NULL handle treated
/// as None.
/// @param obj Valid pointer-valued Option payload; @c NULL is treated as `None`.
/// @param def Borrowed fallback pointer returned for null or `None`.
/// @return Borrowed stored pointer for `Some`, otherwise @p def.
/// @warning This generic accessor assumes a pointer-valued Option and does not
///          reject typed string/integer/floating variants.
void *rt_option_unwrap_or(void *obj, void *def) {
    if (!obj)
        return def;
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return def;
    return o->value.ptr;
}

/// @brief Return the stored string or a caller-supplied default.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param def Borrowed fallback string returned for absent or non-string variants.
/// @return Borrowed contained string for a string-valued `Some`, otherwise @p def.
rt_string rt_option_unwrap_or_str(void *obj, rt_string def) {
    if (!obj)
        return def;
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return def;
    if (o->value_type != VALUE_STR)
        return def;
    return o->value.str;
}

/// @brief Return the stored integer or a caller-supplied default.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param def Fallback integer returned for absent or non-integer variants.
/// @return Contained integer for an integer-valued `Some`, otherwise @p def.
int64_t rt_option_unwrap_or_i64(void *obj, int64_t def) {
    if (!obj)
        return def;
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return def;
    if (o->value_type != VALUE_I64)
        return def;
    return o->value.i64;
}

/// @brief Unwrap a boolean option or return a normalized default.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param def Boolean-like fallback normalized before use.
/// @return @c 0 when the selected stored/default integer is zero, otherwise @c 1.
int8_t rt_option_unwrap_or_i1(void *obj, int8_t def) {
    return rt_option_unwrap_or_i64(obj, def ? 1 : 0) ? 1 : 0;
}

/// @brief Return the stored floating-point value or a caller-supplied default.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param def Fallback value returned for absent or non-floating variants.
/// @return Contained floating-point value for a matching `Some`, otherwise @p def.
double rt_option_unwrap_or_f64(void *obj, double def) {
    if (!obj)
        return def;
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return def;
    if (o->value_type != VALUE_F64)
        return def;
    return o->value.f64;
}

/// @brief Return the wrapped pointer if Some, NULL otherwise. Like `unwrap` but non-trapping —
/// the caller must distinguish "stored NULL" from "no value" via `is_some` / `is_none`.
/// @param obj Valid Option payload; @c NULL is treated as absent.
/// @return Borrowed pointer from a pointer-valued `Some`, or @c NULL for null,
///         `None`, a typed non-pointer variant, or `Some(NULL)`.
void *rt_option_value(void *obj) {
    if (!obj)
        return NULL;
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return NULL;
    // Generic object accessor: typed payloads (str/i64/f64) must not be
    // reinterpreted as pointers; non-trapping contract returns NULL instead.
    if (o->value_type != VALUE_PTR)
        return NULL;
    return o->value.ptr;
}

//=============================================================================
// Expect
//=============================================================================

/// @brief Select the C-string message used for an `expect()` failure.
/// @param msg Borrowed runtime string, or @c NULL to request the default text.
/// @return Borrowed NUL-terminated message bytes, defaulting to
///         `"assertion failed"`.
static const char *rt_option_expect_message(rt_string msg) {
    return msg ? rt_string_cstr(msg) : "assertion failed";
}

/// @brief Like `unwrap` but with a caller-supplied diagnostic message. Traps with kind
/// INVALID_OPERATION (more specific than the generic unwrap trap) so callers can catch
/// expectation violations distinctly. Use when you want a meaningful failure mode.
/// @param obj Valid pointer-valued Option payload to inspect.
/// @param msg Borrowed diagnostic string used for null, `None`, or type mismatch;
///        @c NULL selects `"assertion failed"`.
/// @return Borrowed contained pointer for a pointer-valued `Some`; trap fallback
///         paths return @c NULL.
void *rt_option_expect(void *obj, rt_string msg) {
    const char *msg_str = rt_option_expect_message(msg);
    char buffer[256];
    if (!obj) {
        snprintf(buffer, sizeof(buffer), "Option expect: %s (NULL Option)", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME) {
        snprintf(buffer, sizeof(buffer), "Option expect: %s", msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    if (o->value_type != VALUE_PTR) {
        snprintf(buffer,
                 sizeof(buffer),
                 "Option expect: %s (payload is not an object; use UnwrapStr/UnwrapI64/UnwrapF64)",
                 msg_str);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, buffer);
        return NULL;
    }
    return o->value.ptr;
}

//=============================================================================
// Transformation
//=============================================================================

// =============================================================================
// Transformation combinators — all PTR-variant only. Typed-primitive Options
// (STR/I64/F64) pass through unchanged because the function signature `void*
// (*)(void*)` doesn't match the union; callers must unwrap-transform-rewrap.
// All combinators on NULL/None inputs return a fresh `None`.
// =============================================================================

/// @brief Direct-call invoker used by the native combinator wrappers.
/// @details Reinterprets @p fn as `void *(*)(void *)`; @p ctx is unused.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL opaque C callback pointer.
/// @param arg Borrowed argument forwarded to the callback.
/// @return Callback result without retaining or otherwise transforming it.
void *rt_cb_direct_invoke1(void *ctx, void *fn, void *arg) {
    (void)ctx;
    return (RT_FN_PTR_CAST((void *(*)(void *))fn))(arg);
}

/// @brief Direct-call invoker for zero-argument callbacks.
/// @details Reinterprets @p fn as `void *(*)(void)`; @p ctx is unused.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL opaque C callback pointer.
/// @return Callback result without retaining or otherwise transforming it.
void *rt_cb_direct_invoke0(void *ctx, void *fn) {
    (void)ctx;
    return (RT_FN_PTR_CAST((void *(*)(void))fn))();
}

/// @brief Direct-call invoker for boolean predicates.
/// @details Reinterprets @p fn as `int8_t (*)(void *)`; @p ctx is unused.
/// @param ctx Ignored bridge context.
/// @param fn Non-NULL opaque C predicate pointer.
/// @param arg Borrowed argument forwarded to the predicate.
/// @return Predicate result exactly as returned by the callback.
int8_t rt_cb_direct_invoke_pred(void *ctx, void *fn, void *arg) {
    (void)ctx;
    return (RT_FN_PTR_CAST((int8_t (*)(void *))fn))(arg);
}

/// @brief Apply a native callback to a pointer-valued `Some`.
/// @details Delegates to @ref rt_option_map_invoke with the direct C invoker.
///          Null callbacks, null options, and `None` produce a fresh `None`;
///          typed non-pointer `Some` values pass through unchanged.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Native transform callback receiving the borrowed contained pointer;
///        may be @c NULL.
/// @return New caller-owned Option for mapped/absent paths, or the borrowed
///         original @p obj for a typed non-pointer `Some`.
void *rt_option_map(void *obj, void *(*fn)(void *)) {
    return rt_option_map_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Combinator core shared by the native wrapper and the VM callback bridges.
/// @details The @p invoke strategy abstracts how the user callback runs (direct
///          C call for native code or interpreter re-entry for a VM). A
///          pointer-valued `Some` invokes the callback and wraps its result in a
///          new Option, retaining a managed result. Null, null-callback, and
///          `None` paths allocate `None`; typed non-pointer variants return the
///          original object without invoking the callback.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque user callback handle; @c NULL selects a fresh `None`.
/// @param invoke Non-NULL invocation strategy when @p fn is non-NULL and @p obj
///        is a pointer-valued `Some`.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Caller-owned newly allocated Option for mapped/absent paths, or the
///         borrowed original @p obj for a typed non-pointer `Some`.
void *rt_option_map_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return rt_option_none();
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return rt_option_none();

    if (o->value_type == VALUE_PTR) {
        void *new_val = invoke(ctx, fn, o->value.ptr);
        return rt_option_some(new_val);
    }
    return obj;
}

/// @brief Monadic bind: apply `fn` to the wrapped value where `fn` itself returns an Option,
/// flattening the result. Used to chain fallible operations without nested Options.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Native callback receiving a borrowed pointer and returning an Option;
///        may be @c NULL.
/// @return Callback result unchanged for a pointer-valued `Some`, a fresh
///         caller-owned `None` for absent paths, or the borrowed original
///         Option for a typed non-pointer `Some`.
void *rt_option_and_then(void *obj, void *(*fn)(void *)) {
    return rt_option_and_then_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke1, NULL);
}

/// @brief Run a pluggable Option-returning callback and flatten its result.
/// @details Pointer-valued `Some` invokes @p invoke and returns its result
///          unchanged. Null, null-callback, and `None` paths allocate `None`;
///          typed non-pointer `Some` values pass through without invocation.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque Option-returning callback handle; may be @c NULL.
/// @param invoke Non-NULL invocation strategy when callback execution is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Callback result with its callback-defined ownership, a fresh
///         caller-owned `None`, or the borrowed original typed Option.
void *rt_option_and_then_invoke(void *obj, void *fn, rt_cb_invoke1 invoke, void *ctx) {
    if (!obj || !fn)
        return rt_option_none();
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return rt_option_none();

    if (o->value_type == VALUE_PTR) {
        return invoke(ctx, fn, o->value.ptr);
    }
    return obj;
}

/// @brief If the option is Some, return it unchanged; otherwise call `fn()` to compute a fallback
/// Option. Used for "try this default lookup if the primary failed" patterns.
/// @param obj Valid Option payload; @c NULL is treated as absent.
/// @param fn Native zero-argument fallback callback; may be @c NULL.
/// @return Borrowed original Option for `Some`, callback result unchanged for
///         an available fallback, or a fresh caller-owned `None` when no
///         fallback callback is supplied.
void *rt_option_or_else(void *obj, void *(*fn)(void)) {
    return rt_option_or_else_invoke(obj, RT_FN_PTR_CAST((void *)fn), rt_cb_direct_invoke0, NULL);
}

/// @brief Compute a fallback Option through a pluggable zero-argument invoker.
/// @param obj Valid Option payload; @c NULL is treated as absent.
/// @param fn Opaque fallback callback handle; may be @c NULL.
/// @param invoke Non-NULL invocation strategy when @p fn is non-NULL and the
///        option is absent.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Borrowed original Option for `Some`, callback result with its
///         callback-defined ownership, or a fresh caller-owned `None`.
void *rt_option_or_else_invoke(void *obj, void *fn, rt_cb_invoke0 invoke, void *ctx) {
    if (!obj)
        return fn ? invoke(ctx, fn) : rt_option_none();
    Option *o = (Option *)obj;
    if (o->variant == OPTION_SOME)
        return obj;
    return fn ? invoke(ctx, fn) : rt_option_none();
}

/// @brief Return the option if Some AND `pred(value)` is true; otherwise None. Cheap way to
/// turn unconditional Some values into Some-or-None based on a predicate.
/// @details Only pointer-valued `Some` variants are predicate-compatible;
///          typed string/integer/floating variants produce `None`.
/// @param obj Valid Option payload; may be @c NULL.
/// @param pred Native predicate receiving the borrowed pointer payload; may be @c NULL.
/// @return Borrowed original pointer-valued Option when the predicate succeeds;
///         otherwise a fresh caller-owned `None`.
void *rt_option_filter(void *obj, int8_t (*pred)(void *)) {
    return rt_option_filter_invoke(
        obj, RT_FN_PTR_CAST((void *)pred), rt_cb_direct_invoke_pred, NULL);
}

/// @brief Filter a pointer-valued Option through a pluggable predicate invoker.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque predicate handle; may be @c NULL.
/// @param invoke Non-NULL predicate invocation strategy when evaluation is required.
/// @param ctx Borrowed strategy context forwarded unchanged to @p invoke.
/// @return Borrowed original Option when a pointer-valued `Some` passes;
///         otherwise a fresh caller-owned `None`.
void *rt_option_filter_invoke(void *obj, void *fn, rt_cb_invoke_pred invoke, void *ctx) {
    if (!obj || !fn)
        return rt_option_none();
    Option *o = (Option *)obj;
    if (o->variant != OPTION_SOME)
        return rt_option_none();

    if (o->value_type == VALUE_PTR && invoke(ctx, fn, o->value.ptr)) {
        return obj;
    }
    return rt_option_none();
}

/// @brief IL trampoline for @ref rt_option_map.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque native callback pointer reinterpreted as `void *(*)(void *)`;
///        may be @c NULL.
/// @return Result produced by @ref rt_option_map with the same ownership rules.
void *rt_option_map_wrapper(void *obj, void *fn) {
    return rt_option_map(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for @ref rt_option_and_then.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque native Option-returning callback pointer; may be @c NULL.
/// @return Result produced by @ref rt_option_and_then with the same ownership rules.
void *rt_option_and_then_wrapper(void *obj, void *fn) {
    return rt_option_and_then(obj, RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for @ref rt_option_or_else.
/// @param obj Valid Option payload; may be @c NULL.
/// @param fn Opaque native zero-argument fallback pointer; may be @c NULL.
/// @return Result produced by @ref rt_option_or_else with the same ownership rules.
void *rt_option_or_else_wrapper(void *obj, void *fn) {
    return rt_option_or_else(obj, RT_FN_PTR_CAST((void *(*)(void))fn));
}

/// @brief IL trampoline for @ref rt_option_filter.
/// @param obj Valid Option payload; may be @c NULL.
/// @param pred Opaque native predicate pointer; may be @c NULL.
/// @return Result produced by @ref rt_option_filter with the same ownership rules.
void *rt_option_filter_wrapper(void *obj, void *pred) {
    return rt_option_filter(obj, RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}

//=============================================================================
// Conversion
//=============================================================================

/// @brief Convert Option → Result by supplying an error value: `Some(v) → Ok(v)`, `None →
/// Err(err)`. Preserves the value-type variant so e.g. `Some(i64) → Ok_i64`. Used to bridge
/// missing-data failures into the Result error-handling pipeline.
/// @details The new Result retains the selected managed payload through the
///          matching typed Result constructor; the source Option is unchanged.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param err Pointer error value used only for an absent option.
/// @return Caller-owned typed `Ok` for `Some`, or pointer-valued `Err` for
///         null/`None`.
void *rt_option_ok_or(void *obj, void *err) {
    if (!obj)
        return rt_result_err(err);
    Option *o = (Option *)obj;
    if (o->variant == OPTION_SOME) {
        switch (o->value_type) {
            case VALUE_PTR:
                return rt_result_ok(o->value.ptr);
            case VALUE_STR:
                return rt_result_ok_str(o->value.str);
            case VALUE_I64:
                return rt_result_ok_i64(o->value.i64);
            case VALUE_F64:
                return rt_result_ok_f64(o->value.f64);
        }
    }
    return rt_result_err(err);
}

/// @brief String-error variant of `ok_or`. None becomes `Err_str(err)` so the resulting Result
/// holds an rt_string error message rather than an opaque pointer.
/// @param obj Valid Option payload; @c NULL is treated as `None`.
/// @param err String error value retained by an absent option's new Result.
/// @return Caller-owned typed `Ok` for `Some`, or string-valued `Err` for
///         null/`None`.
void *rt_option_ok_or_str(void *obj, rt_string err) {
    if (!obj)
        return rt_result_err_str(err);
    Option *o = (Option *)obj;
    if (o->variant == OPTION_SOME) {
        switch (o->value_type) {
            case VALUE_PTR:
                return rt_result_ok(o->value.ptr);
            case VALUE_STR:
                return rt_result_ok_str(o->value.str);
            case VALUE_I64:
                return rt_result_ok_i64(o->value.i64);
            case VALUE_F64:
                return rt_result_ok_f64(o->value.f64);
        }
    }
    return rt_result_err_str(err);
}

//=============================================================================
// Utility
//=============================================================================

/// @brief Compare two option instances for structural equality.
/// @details Null is `None`-like. Pointer payloads use identity, strings use
///          content comparison, integers use numeric equality, and floating
///          values use C `==` semantics (so NaN is unequal).
/// @param a First valid Option payload, or @c NULL.
/// @param b Second valid Option payload, or @c NULL.
/// @return @c 1 when the variants, stored types, and values compare equal;
///         otherwise @c 0.
int8_t rt_option_equals(void *a, void *b) {
    // Both NULL = equal (both "None-like")
    if (!a && !b)
        return 1;

    if (!a) {
        Option *ob = (Option *)b;
        return ob->variant == OPTION_NONE ? 1 : 0;
    }
    if (!b) {
        Option *oa = (Option *)a;
        return oa->variant == OPTION_NONE ? 1 : 0;
    }

    if (a == b)
        return 1;

    Option *oa = (Option *)a;
    Option *ob = (Option *)b;

    if (oa->variant != ob->variant)
        return 0;
    if (oa->variant == OPTION_NONE)
        return 1; // Both None

    if (oa->value_type != ob->value_type)
        return 0;

    switch (oa->value_type) {
        case VALUE_PTR:
            return oa->value.ptr == ob->value.ptr ? 1 : 0;
        case VALUE_STR:
            return rt_str_cmp(oa->value.str, ob->value.str) == 0 ? 1 : 0;
        case VALUE_I64:
            return oa->value.i64 == ob->value.i64 ? 1 : 0;
        case VALUE_F64:
            return oa->value.f64 == ob->value.f64 ? 1 : 0;
    }
    return 0;
}

/// @brief Convert the option to a human-readable string representation.
/// @details Null and `None` produce the constant `"None"`. `Some` formats
///          pointers with `%p`, quotes string contents, and formats inline
///          numeric values into a bounded temporary buffer.
/// @param obj Valid Option payload; @c NULL is rendered as `None`.
/// @return Runtime string such as `"None"` or `"Some(value)"`; `Some` paths
///         return a caller-owned allocation, while the `None` result is an
///         immortal constant string.
rt_string rt_option_to_string(void *obj) {
    if (!obj)
        return rt_const_cstr("None");

    Option *o = (Option *)obj;
    char buf[256];

    if (o->variant == OPTION_NONE) {
        return rt_const_cstr("None");
    }

    switch (o->value_type) {
        case VALUE_PTR:
            snprintf(buf, sizeof(buf), "Some(%p)", o->value.ptr);
            break;
        case VALUE_STR:
            snprintf(buf, sizeof(buf), "Some(\"%s\")", rt_string_cstr(o->value.str));
            break;
        case VALUE_I64:
            snprintf(buf, sizeof(buf), "Some(%lld)", (long long)o->value.i64);
            break;
        case VALUE_F64:
            snprintf(buf, sizeof(buf), "Some(%g)", o->value.f64);
            break;
    }

    return rt_string_from_bytes(buf, strlen(buf));
}
