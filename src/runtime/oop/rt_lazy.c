//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_lazy.c
// Purpose: Implements the Lazy<T> deferred initialization wrapper for the
//          Zanna.Functional.Lazy class. The wrapped factory function is called at most
//          on first Value access; subsequent accesses return the published
//          cached value.
//
// Key invariants:
//   - Pure suppliers are required because racing first accesses may evaluate twice.
//   - After the first call, the cached value is returned without locking.
//   - Thread-safety: initialization uses a double-checked flag; on platforms
//     with weaker memory models an atomic/barrier is used.
//   - If the factory returns NULL, NULL is cached and returned on all subsequent
//     accesses.
//   - IsInitialized returns 1 after the first successful Value call.
//
// Ownership/Lifetime:
//   - The Lazy object retains a reference to the cached value once computed.
//   - The GC finalizer releases the cached value reference.
//   - The factory function pointer is not retained; callers own its lifetime.
//
// Links: src/runtime/oop/rt_lazy.h (public API),
//        src/runtime/oop/rt_option.h (Option<T> for present/absent, related)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_lazy.c
 * @brief Implements deferred, once-published managed Lazy values.
 * @details Lazy instances represent direct native suppliers, VM-owned supplier
 *          handles, or already evaluated object, String, and integer values.
 *          Acquire/release publication caches the first completed result, and
 *          finalization releases any managed payload transferred to the Lazy.
 */

#include "rt_lazy.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdlib.h>

//=============================================================================
// Internal Structure
//=============================================================================

typedef enum { VALUE_PTR = 0, VALUE_STR = 1, VALUE_I64 = 2 } ValueType;

/// How the pending computation is represented. SUPPLIER_FN is a directly
/// callable C function pointer (native codegen). SUPPLIER_HANDLE is an opaque
/// managed-function handle owned by a VM bridge, which must complete the Lazy
/// via rt_lazy_complete_obj() before any C-side evaluation is attempted.
typedef enum { SUPPLIER_NONE = 0, SUPPLIER_FN = 1, SUPPLIER_HANDLE = 2 } SupplierKind;

typedef struct {
    int evaluated; /* widened from int8_t for MSVC _Generic atomic compat */
    ValueType value_type;
    SupplierKind supplier_kind;
    /// @brief Direct native supplier invoked once to compute the lazy pointer value.
    /// @return Runtime value cached by the Lazy object.
    void *(*supplier)(void);
    void *supplier_handle;

    union {
        void *ptr;
        rt_string str;
        int64_t i64;
    } value;
} Lazy;

//=============================================================================
// Lazy Finalizer
//=============================================================================

/// @brief GC finalizer: release the cached payload (PTR via heap, STR via `rt_str_release_maybe`).
/// I64 holds no heap memory. Skip if the Lazy was never evaluated (no payload to release).
/// @param[in,out] obj Lazy payload being finalized.
static void lazy_finalizer(void *obj) {
    Lazy *l = (Lazy *)obj;
    if (!l || !l->evaluated)
        return;
    if (l->value_type == VALUE_PTR && l->value.ptr) {
        if (rt_obj_release_check0(l->value.ptr))
            rt_obj_free(l->value.ptr);
        l->value.ptr = NULL;
    } else if (l->value_type == VALUE_STR && l->value.str) {
        rt_str_release_maybe(l->value.str);
        l->value.str = NULL;
    }
}

//=============================================================================
// Lazy Creation
//=============================================================================

/// @brief Construct a Lazy<Ptr> whose value will be computed by `supplier()` on first access.
/// `supplier` is borrowed (not retained); caller owns its lifetime. Its return
/// value is published with release ordering and reused after evaluation.
/// @param[in] supplier Borrowed native callback whose returned object ownership transfers to Lazy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_new(void *(*supplier)(void)) {
    Lazy *l = (Lazy *)rt_obj_new_i64(0, (int64_t)sizeof(Lazy));

    l->evaluated = 0;
    l->value_type = VALUE_PTR;
    l->supplier_kind = SUPPLIER_FN;
    l->supplier = supplier;
    l->supplier_handle = NULL;
    l->value.ptr = NULL;
    rt_obj_set_finalizer(l, lazy_finalizer);
    return l;
}

/// @brief Construct a deferred Lazy whose supplier is an opaque managed-function handle.
/// Used by VM bridges: the handle is not C-callable, so the owning bridge must intercept
/// accessors, execute the handle itself, and publish the result via `rt_lazy_complete_obj`.
/// @param[in] handle Borrowed opaque function handle.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_new_handle(void *handle) {
    Lazy *l = (Lazy *)rt_obj_new_i64(0, (int64_t)sizeof(Lazy));

    l->evaluated = 0;
    l->value_type = VALUE_PTR;
    l->supplier_kind = SUPPLIER_HANDLE;
    l->supplier = NULL;
    l->supplier_handle = handle;
    l->value.ptr = NULL;
    rt_obj_set_finalizer(l, lazy_finalizer);
    return l;
}

/// @brief Return the pending managed-function handle, or NULL when the Lazy is already
/// evaluated or was not created via `rt_lazy_new_handle`.
/// @param[in] obj Lazy payload, or NULL.
/// @return Borrowed pending handle, or NULL.
void *rt_lazy_pending_handle(void *obj) {
    if (!obj)
        return NULL;
    Lazy *l = (Lazy *)obj;
    if (l->supplier_kind != SUPPLIER_HANDLE)
        return NULL;
    if (__atomic_load_n(&l->evaluated, __ATOMIC_ACQUIRE))
        return NULL;
    return l->supplier_handle;
}

/// @brief Publish the computed object payload for a handle-kind Lazy and mark it evaluated.
/// Mirrors `evaluate()`'s release-store so readers on other threads observe the cached value.
/// @param[in,out] obj Lazy payload, or NULL.
/// @param[in] value Computed object whose ownership transfers to the Lazy.
void rt_lazy_complete_obj(void *obj, void *value) {
    if (!obj)
        return;
    Lazy *l = (Lazy *)obj;
    if (__atomic_load_n(&l->evaluated, __ATOMIC_ACQUIRE))
        return;
    l->value_type = VALUE_PTR;
    l->value.ptr = value;
    __atomic_store_n(&l->evaluated, 1, __ATOMIC_RELEASE);
}

/// @brief Construct a pre-evaluated Lazy<Ptr> wrapping `value`. Useful when interop expects a
/// Lazy<T> handle but you already have the value (e.g., test fixtures, eager-but-typed APIs).
/// @param[in] value Object whose ownership transfers to the Lazy, or NULL.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of(void *value) {
    Lazy *l = (Lazy *)rt_obj_new_i64(0, (int64_t)sizeof(Lazy));

    l->evaluated = 1;
    l->value_type = VALUE_PTR;
    l->supplier_kind = SUPPLIER_NONE;
    l->supplier = NULL;
    l->supplier_handle = NULL;
    l->value.ptr = value;
    rt_obj_set_finalizer(l, lazy_finalizer);
    return l;
}

/// @brief Pre-evaluated Lazy<String> variant. Stores the rt_string directly; finalizer releases.
/// @param[in] value Managed String whose ownership transfers to the Lazy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of_str(rt_string value) {
    Lazy *l = (Lazy *)rt_obj_new_i64(0, (int64_t)sizeof(Lazy));

    l->evaluated = 1;
    l->value_type = VALUE_STR;
    l->supplier_kind = SUPPLIER_NONE;
    l->supplier = NULL;
    l->supplier_handle = NULL;
    l->value.str = value;
    rt_obj_set_finalizer(l, lazy_finalizer);
    return l;
}

/// @brief Pre-evaluated Lazy<I64> variant. Stores the value inline (no heap retention needed).
/// @param[in] value Integer value to copy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of_i64(int64_t value) {
    Lazy *l = (Lazy *)rt_obj_new_i64(0, (int64_t)sizeof(Lazy));

    l->evaluated = 1;
    l->value_type = VALUE_I64;
    l->supplier_kind = SUPPLIER_NONE;
    l->supplier = NULL;
    l->supplier_handle = NULL;
    l->value.i64 = value;
    rt_obj_set_finalizer(l, lazy_finalizer);
    return l;
}

//=============================================================================
// Lazy Access
//=============================================================================

/// @brief Thread-safe evaluation: double-checked atomic load on `evaluated` (acquire) lets
/// already-initialized Lazies skip the supplier call without a barrier. On the first call,
/// invoke the supplier, then atomic-store `evaluated=1` (release) so other threads see the
/// completed cache. **Race contract:** double evaluation is possible (no exclusion lock) but
/// benign — the Lazy contract requires pure suppliers, so re-entrancy returns the same value.
/// @param[in,out] l Lazy payload to evaluate.
static void evaluate(Lazy *l) {
    // Use atomic load/store for thread safety on ARM64 and other weak-memory platforms.
    // Double evaluation is possible but benign (Lazy contract assumes pure suppliers).
    if (__atomic_load_n(&l->evaluated, __ATOMIC_ACQUIRE))
        return;

    if (l->supplier_kind == SUPPLIER_HANDLE) {
        // A managed-function handle is not C-callable; the owning VM bridge must have
        // completed this Lazy via rt_lazy_complete_obj() before C-side evaluation runs.
        rt_trap("Lazy: deferred supplier requires the VM bridge; value accessed outside it");
        return;
    }

    if (l->supplier) {
        l->value.ptr = l->supplier();
    }
    __atomic_store_n(&l->evaluated, 1, __ATOMIC_RELEASE);
}

/// @brief Force evaluation if needed, return the pointer payload. NULL handle returns NULL.
/// @param[in,out] obj Lazy payload, or NULL.
/// @return Borrowed cached object pointer, or NULL.
void *rt_lazy_get(void *obj) {
    if (!obj)
        return NULL;
    Lazy *l = (Lazy *)obj;

    evaluate(l);
    return l->value.ptr;
}

/// @brief Force evaluation if needed and return the result as a string.
/// @param[in,out] obj Lazy payload, or NULL.
/// @return Borrowed cached String for VALUE_STR, otherwise a constant empty String.
rt_string rt_lazy_get_str(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    Lazy *l = (Lazy *)obj;

    evaluate(l);

    if (l->value_type == VALUE_STR) {
        return l->value.str;
    }
    return rt_const_cstr("");
}

/// @brief Force evaluation if needed and return the result as an i64.
/// @param[in,out] obj Lazy payload, or NULL.
/// @return Cached integer for VALUE_I64, otherwise zero.
int64_t rt_lazy_get_i64(void *obj) {
    if (!obj)
        return 0;
    Lazy *l = (Lazy *)obj;

    evaluate(l);

    if (l->value_type == VALUE_I64) {
        return l->value.i64;
    }
    return 0;
}

//=============================================================================
// Lazy State
//=============================================================================

/// @brief Check whether the lazy value has already been evaluated (computed).
/// @param[in] obj Lazy payload, or NULL.
/// @return 1 when evaluated or @p obj is NULL; otherwise 0.
int8_t rt_lazy_is_evaluated(void *obj) {
    if (!obj)
        return 1;
    Lazy *l = (Lazy *)obj;
    return (int8_t)(l->evaluated ? 1 : 0);
}

/// @brief Force evaluation of the lazy value, even if already evaluated.
/// @param[in,out] obj Lazy payload, or NULL.
void rt_lazy_force(void *obj) {
    if (!obj)
        return;
    Lazy *l = (Lazy *)obj;
    evaluate(l);
}

//=============================================================================
// Transformation
//=============================================================================

/// @brief Apply `fn` to the lazy's value, returning a fresh pre-evaluated Lazy with the result.
/// **Caveat:** despite the name, this is NOT a deferred-map — it forces evaluation of the source
/// (no closure support in C), then wraps the transformed value. Truly lazy chaining requires
/// `LazySeq` instead. Useful when you want a Lazy<T> handle for an already-known transformation.
/// @param[in,out] obj Source Lazy; returned unchanged when NULL or @p fn is NULL.
/// @param[in] fn Borrowed transform whose returned object transfers to the new Lazy.
/// @return Caller-owned transformed Lazy, or the original borrowed @p obj for invalid arguments.
void *rt_lazy_map(void *obj, void *(*fn)(void *)) {
    if (!obj || !fn)
        return obj;

    Lazy *l = (Lazy *)obj;

    // If already evaluated, apply fn immediately
    if (l->evaluated) {
        void *new_value = fn(l->value.ptr);
        return rt_lazy_of(new_value);
    }

    // Create a new lazy that will apply fn when evaluated
    // Note: In a real implementation, we'd need proper closure support
    // For now, we'll force evaluation and apply
    void *value = rt_lazy_get(obj);
    void *new_value = fn(value);
    return rt_lazy_of(new_value);
}

/// @brief Apply `fn` (returning a Lazy) to the source's value, returning the resulting Lazy
/// directly (no double-wrapping). Forces the source eagerly for the same closure-support reason
/// as `_map`. Returned Lazy is whatever `fn` produced — its evaluation timing is `fn`'s choice.
/// @param[in,out] obj Source Lazy; returned unchanged when NULL or @p fn is NULL.
/// @param[in] fn Borrowed transform returning a Lazy.
/// @return Callback result, or the original borrowed @p obj for invalid arguments.
void *rt_lazy_flat_map(void *obj, void *(*fn)(void *)) {
    if (!obj || !fn)
        return obj;

    // Force evaluation of the source lazy
    void *value = rt_lazy_get(obj);

    // Call fn to get a new lazy
    void *new_lazy = fn(value);

    // Return the new lazy (which will be evaluated when needed)
    return new_lazy;
}

/// @brief IL trampoline for `rt_lazy_new`. Registered as `Zanna.Functional.Lazy.New`, the only
/// public factory that defers evaluation until first access instead of at
/// construction time (the `Of*` factories all wrap already-computed values).
/// @param[in] supplier Opaque native supplier function pointer.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_new_wrapper(void *supplier) {
    return rt_lazy_new((void *(*)(void))supplier);
}

/// @brief IL trampoline for `rt_lazy_map`.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Opaque native transform pointer.
/// @return Result from rt_lazy_map().
void *rt_lazy_map_wrapper(void *obj, void *fn) {
    return rt_lazy_map(obj, (void *(*)(void *))fn);
}

/// @brief IL trampoline for `rt_lazy_flat_map`.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Opaque native transform pointer.
/// @return Result from rt_lazy_flat_map().
void *rt_lazy_flat_map_wrapper(void *obj, void *fn) {
    return rt_lazy_flat_map(obj, (void *(*)(void *))fn);
}
