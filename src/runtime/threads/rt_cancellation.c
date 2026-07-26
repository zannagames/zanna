//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_cancellation.c
// Purpose: Implements a cooperative cancellation token for the
//          Zanna.Threads.Cancellation class. Tokens can be cancelled, checked,
//          and linked in a parent-child hierarchy so that cancelling a parent
//          propagates to all linked child tokens.
//
// Key invariants:
//   - Cancellation state is stored as an atomic_int (POSIX) or volatile LONG
//     (Win32) to allow lock-free reads from any thread.
//   - Local cancellation state can be reset for reuse via rt_cancellation_reset.
//   - Linked parent tokens propagate cancellation down to children on cancel.
//   - IsCancelled is always safe to call from any thread without locking.
//   - The finalizer releases any retained linked parent token.
//
// Ownership/Lifetime:
//   - Cancellation token objects are heap-allocated and managed by the GC.
//   - Linked child tokens retain their parent token until finalization.
//
// Links: src/runtime/threads/rt_cancellation.h (public API),
//        src/runtime/threads/rt_future.h (futures accept a cancellation token),
//        src/runtime/threads/rt_async.h (async tasks use cancellation tokens)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Atomic cooperative-cancellation tokens with retained parent links.
/// @details Local cancellation reads are lock-free. A monitor protects each
///          token's weak intrusive child list so cancellation and finalization
///          can safely race, while child-to-parent retention keeps ancestry
///          available for recursive checks.

#include "rt_cancellation.h"

#include "rt_error.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_threads.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <stdatomic.h>
#endif

// --- Internal structures ---

/// @brief Runtime payload of a cancellation token and its linkage metadata.
typedef struct rt_cancellation_data {
#if defined(_WIN32)
    volatile LONG cancelled;
#else
    atomic_int cancelled;
#endif
    void *parent;  // linked parent token (NULL if root)
    void *monitor; // protects the weak child list
    struct rt_cancellation_data *first_child;
    struct rt_cancellation_data *next_sibling;
} rt_cancellation_data;

// --- Platform-specific atomic helpers ---

/// @brief Set the initial value of the `cancelled` flag at construction time.
/// @details Called only from `rt_cancellation_new` / `_linked` before the object is published.
///          Uses `atomic_init` (POSIX) or `InterlockedExchange` (Win32 — there is no
///          dedicated init primitive, but `InterlockedExchange` provides the same
///          happens-before guarantee for any subsequent reader).
/// @param data Unpublished token payload to initialize.
/// @param value Initial integer flag value.
static inline void cancel_init(rt_cancellation_data *data, int value) {
#if defined(_WIN32)
    InterlockedExchange(&data->cancelled, (LONG)value);
#else
    atomic_init(&data->cancelled, value);
#endif
}

/// @brief Atomic load of the local `cancelled` flag.
/// @details The Win32 path uses `InterlockedCompareExchange(..., 0, 0)` as a portable
///          atomic-load — no value can satisfy the compare so the flag is left untouched, but
///          the operation carries a sequentially-consistent ordering that older MSVC versions
///          didn't expose via a dedicated load intrinsic.
/// @param data Token payload to read.
/// @return Current local cancellation flag.
static inline int cancel_load(rt_cancellation_data *data) {
#if defined(_WIN32)
    return (int)InterlockedCompareExchange(&data->cancelled, 0, 0);
#else
    return atomic_load(&data->cancelled);
#endif
}

/// @brief Atomic store of the `cancelled` flag.
/// @details Used both to flip a token to cancelled (`value = 1`) and to clear it via
///          `rt_cancellation_reset` (`value = 0`). Sequentially consistent so concurrent
///          `IsCancelled` callers see the new value without further synchronization.
/// @param data Token payload to update.
/// @param value New integer flag value.
static inline void cancel_store(rt_cancellation_data *data, int value) {
#if defined(_WIN32)
    InterlockedExchange(&data->cancelled, (LONG)value);
#else
    atomic_store(&data->cancelled, value);
#endif
}

/// @brief Drop a runtime-managed object reference, freeing it if it was the last one.
/// @details One-line wrapper used pervasively in this file's parent/child unlink paths so
///          each call site doesn't have to repeat the release-then-free dance. Safe on NULL.
/// @param obj Runtime-managed object reference, or null.
static void cancellation_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Validate that `token` is a cancellation token, trapping on type mismatch.
/// @details Guards every public entry point that dereferences `rt_cancellation_data`. Returns
///          NULL on a NULL input (caller treats that as a no-op) but raises a runtime trap
///          when given a non-cancellation object — that's a programming bug we want to
///          surface loudly rather than silently misinterpret as a token.
/// @param token Candidate cancellation-token handle.
/// @return Validated payload, or null for a null or trapped invalid input.
static rt_cancellation_data *cancellation_require(void *token) {
    if (!token)
        return NULL;
    if (!rt_obj_is_instance(token, RT_CANCELLATION_CLASS_ID, sizeof(rt_cancellation_data))) {
        rt_trap("CancelToken: invalid object");
        return NULL;
    }
    return (rt_cancellation_data *)token;
}

/// @brief Cancel `data` and mark its immediate linked children.
/// @details Children retain their parent and `IsCancelled` walks upward through
///          that retained parent chain. Marking immediate children is therefore
///          enough to make cancellation sticky for the full descendant tree
///          without retaining weak child-list entries that may already be
///          finalizing and waiting to unlink from this monitor.
/// @param data Token payload to cancel; null is ignored.
static void cancellation_mark_cancelled(rt_cancellation_data *data) {
    if (!data)
        return;
    cancel_store(data, 1);
    if (!data->monitor)
        return;

    rt_monitor_enter(data->monitor);
    for (rt_cancellation_data *child = data->first_child; child; child = child->next_sibling) {
        cancel_store(child, 1);
    }
    rt_monitor_exit(data->monitor);
}

/// @brief GC finalizer for cancellation tokens.
/// @details Tears down the parent linkage in two steps: (1) unlink this node from the parent's
///          intrusive child list under the parent's monitor so a concurrent `Cancel(parent)`
///          can't dereference a freed sibling pointer, then (2) drop the retain on the parent
///          and release the local monitor. Any retained children are *not* released here —
///          children hold a retain on the parent (us), not the other way around, so by the
///          time we're finalizing, no live child can still reference us.
/// @param obj Cancellation-token object to finalize; null is ignored.
static void cancellation_finalizer(void *obj) {
    rt_cancellation_data *data = (rt_cancellation_data *)obj;
    if (!data)
        return;

    if (data->parent) {
        rt_cancellation_data *parent = (rt_cancellation_data *)data->parent;
        if (parent->monitor) {
            rt_monitor_enter(parent->monitor);
            rt_cancellation_data **link = &parent->first_child;
            while (*link) {
                if (*link == data) {
                    *link = data->next_sibling;
                    break;
                }
                link = &(*link)->next_sibling;
            }
            rt_monitor_exit(parent->monitor);
        }
        cancellation_release_object(data->parent);
        data->parent = NULL;
    }

    data->first_child = NULL;
    data->next_sibling = NULL;

    if (data->monitor) {
        cancellation_release_object(data->monitor);
        data->monitor = NULL;
    }
}

// --- Public API ---

/// @brief Create an independent token in the non-cancelled state.
/// @return New runtime-managed token, or null after an allocation trap.
void *rt_cancellation_new(void) {
    void *obj = rt_obj_new_i64(RT_CANCELLATION_CLASS_ID, sizeof(rt_cancellation_data));
    if (!obj) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CancelToken.New: memory allocation failed");
        return NULL;
    }
    rt_cancellation_data *data = (rt_cancellation_data *)obj;
    cancel_init(data, 0);
    data->parent = NULL;
    data->monitor = rt_obj_new_i64(0, 1);
    if (!data->monitor) {
        cancellation_release_object(obj);
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CancelToken.New: memory allocation failed");
        return NULL;
    }
    data->first_child = NULL;
    data->next_sibling = NULL;
    rt_obj_set_finalizer(obj, cancellation_finalizer);
    return obj;
}

/// @brief Check this token and its retained ancestor chain for cancellation.
/// @param token Cancellation token, or null.
/// @return 1 if the token or any ancestor is cancelled, otherwise 0.
int8_t rt_cancellation_is_cancelled(void *token) {
    if (!token)
        return 0;
    rt_cancellation_data *data = cancellation_require(token);
    if (!data)
        return 0;
    rt_obj_retain_maybe(token);
    if (cancel_load(data)) {
        cancellation_release_object(token);
        return 1;
    }
    void *parent = data->parent;
    rt_obj_retain_maybe(parent);
    cancellation_release_object(token);
    if (parent) {
        int8_t cancelled = rt_cancellation_is_cancelled(parent);
        cancellation_release_object(parent);
        return cancelled;
    }
    return 0;
}

/// @brief Atomically request cancellation and mark immediate linked children.
/// @details Descendants observe cancellation either from their locally marked
///          parent or by walking the retained ancestor chain.
/// @param token Cancellation token, or null for a no-op.
void rt_cancellation_cancel(void *token) {
    if (!token)
        return;
    rt_cancellation_data *data = cancellation_require(token);
    if (!data)
        return;
    rt_obj_retain_maybe(token);
    cancellation_mark_cancelled(data);
    cancellation_release_object(token);
}

/// @brief Clear the local cancelled flag on `token` so it can be reused for a new operation.
/// @details Only the token's *own* flag is cleared. If `token` was a linked child whose parent
///          is still cancelled, `IsCancelled` will continue to report true via the upward
///          parent check on subsequent calls. Reset is intended for tokens whose
///          cancellation request has been honoured and the same object is being recycled
///          for a fresh request — not as a way to undo a parent's cancellation.
/// @param token Cancellation token, or null for a no-op.
void rt_cancellation_reset(void *token) {
    if (!token)
        return;
    rt_cancellation_data *data = cancellation_require(token);
    if (!data)
        return;
    rt_obj_retain_maybe(token);
    cancel_store(data, 0);
    cancellation_release_object(token);
}

/// @brief Create a token linked to a retained optional parent.
/// @details The child is inserted into the parent's weak child list under its
///          monitor. A parent already cancelled, or cancelled during linking,
///          causes the child to start cancelled.
/// @param parent Valid parent cancellation token, or null for an independent token.
/// @return New runtime-managed child token, or null after invalid input or allocation failure.
void *rt_cancellation_linked(void *parent) {
    rt_cancellation_data *parent_data = NULL;
    int8_t parent_cancelled = 0;
    int parent_retained = 0;
    if (parent) {
        parent_data = cancellation_require(parent);
        if (!parent_data)
            return NULL;
        parent_cancelled = rt_cancellation_is_cancelled(parent);
        rt_obj_retain_maybe(parent);
        parent_retained = 1;
    }

    void *obj = rt_obj_new_i64(RT_CANCELLATION_CLASS_ID, sizeof(rt_cancellation_data));
    if (!obj) {
        if (parent_retained)
            cancellation_release_object(parent);
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CancelToken.Linked: memory allocation failed");
        return NULL;
    }
    rt_cancellation_data *data = (rt_cancellation_data *)obj;
    cancel_init(data, 0);
    data->parent = NULL;
    data->monitor = rt_obj_new_i64(0, 1);
    if (!data->monitor) {
        cancellation_release_object(obj);
        if (parent_retained)
            cancellation_release_object(parent);
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CancelToken.Linked: memory allocation failed");
        return NULL;
    }
    data->first_child = NULL;
    data->next_sibling = NULL;
    rt_obj_set_finalizer(obj, cancellation_finalizer);

    if (parent) {
        data->parent = parent;
        if (parent_data->monitor) {
            rt_monitor_enter(parent_data->monitor);
            data->next_sibling = parent_data->first_child;
            parent_data->first_child = data;
            if (parent_cancelled || cancel_load(parent_data))
                cancel_store(data, 1);
            rt_monitor_exit(parent_data->monitor);
        } else if (parent_cancelled) {
            cancel_store(data, 1);
        }
    }
    return obj;
}

/// @brief Public alias for `rt_cancellation_is_cancelled` exposed under a friendlier name.
/// @details Matches the naming convention used by other Zanna.Threads polling primitives
///          (e.g. `rt_future_check`). Identical semantics: returns 1 if this token or any
///          of its ancestors has been cancelled, else 0.
/// @param token Cancellation token, or null.
/// @return 1 if cancellation is observable, otherwise 0.
int8_t rt_cancellation_check(void *token) {
    return rt_cancellation_is_cancelled(token);
}

/// @brief Trap with `OperationCancelledException` if cancellation has been requested.
/// @details Used at well-defined cancellation points inside long-running runtime work so a
///          cooperative caller can bail out promptly. The trap kind is `RT_TRAP_KIND_INTERRUPT`
///          (not `RUNTIME_ERROR`) so `try { ... } catch InterruptedException` blocks in Zia
///          can distinguish cancellation from other faults. No-op if `token` is NULL or not
///          cancelled — callers can sprinkle these freely without performance worry.
/// @param token Cancellation token, or null.
void rt_cancellation_throw_if_cancelled(void *token) {
    if (rt_cancellation_check(token))
        rt_trap_raise_kind(RT_TRAP_KIND_INTERRUPT,
                           0,
                           -1,
                           "OperationCancelledException: cancellation was requested");
}
