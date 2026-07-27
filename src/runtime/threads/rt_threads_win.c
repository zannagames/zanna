//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_threads_win.c
// Purpose: Windows (Win32) implementation of the thread runtime: native handles,
//   trampoline, join/timed-join, id/alive queries, sleep/yield. Compiled only
//   on _WIN32; POSIX uses rt_threads_posix.c. Shared model in rt_threads_internal.h.
//
// Key invariants:
//   - `_beginthreadex` initializes CRT thread state and each returned HANDLE is
//     closed exactly once after a successful join or detach claim.
//   - A child adopts its reserved runtime-context binding before invoking the
//     typed callback and unbinds it before publishing completion.
// Ownership/Lifetime:
//   - The thread inner object owns the HANDLE and retained construction state
//     until terminal cleanup. Ordinary callback arguments are borrowed; Owned
//     start variants retain managed arguments until callback exit.
//
// Links: rt_threads_internal.h, rt_threads_posix.c, rt_threads_common.c, rt_threads.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements native Thread operations with Win32 synchronization.
/// @details Thread records use a critical section and condition variable for
///          repeatable completion observation, inherit the caller's reserved
///          runtime-context binding, and close each `_beginthreadex` handle
///          during finalization. Legacy opaque callbacks are decoded only at
///          the public ABI boundary.

#include "rt_threads_internal.h"

#include "rt_stack_safety.h"

#if defined(_WIN32)

//===----------------------------------------------------------------------===//
// Windows Threading Implementation
//===----------------------------------------------------------------------===//
//
// Uses Windows synchronization primitives:
// - CRITICAL_SECTION for mutex (faster than SRWLOCK for this use case)
// - CONDITION_VARIABLE for signaling between threads
// - _beginthreadex/CloseHandle for CRT-compatible thread management
//
// Thread lifecycle on Windows:
// 1. _beginthreadex initializes CRT thread state and spawns the OS thread
// 2. Thread runs entry function via trampoline
// 3. On completion, signals waiters via CONDITION_VARIABLE
// 4. CloseHandle is called when thread object is finalized
//
//===----------------------------------------------------------------------===//

#define WIN32_LEAN_AND_MEAN
#include "rt_win32_wait.h"
#include <process.h>
#include <windows.h>

/// @brief Internal representation of a Zanna thread (Windows).
///
/// Uses Windows CRITICAL_SECTION and CONDITION_VARIABLE for synchronization.
/// The thread handle is stored for potential future use (e.g., priority changes)
/// but is not required for join operations since we use condition variables.
typedef struct RtThread {
    uint32_t magic;           ///< Runtime tag for validating Thread handles.
    CRITICAL_SECTION cs;      ///< Critical section protecting state access.
    CONDITION_VARIABLE cv;    ///< Condition var for Join() signaling.
    HANDLE hThread;           ///< OS thread handle.
    unsigned threadId;        ///< OS thread ID for self-join detection.
    int finished;             ///< 1 when thread has completed.
    int joined;               ///< Reserved for ABI/debug compatibility; joins are repeatable.
    int64_t id;               ///< Unique Zanna thread identifier.
    RtContext *inherited_ctx; ///< Parent's runtime context.
    rt_thread_entry_fn entry; ///< User's entry function.
    void *arg;                ///< Argument to entry function.
    int8_t owns_arg;          ///< 1 when arg is a retained runtime object.
} RtThread;

/// @brief True if @p obj is a live regular Thread handle (correct class id,
///        size, and RT_THREAD_MAGIC guard) — distinguishes it from SafeThread
///        and stale/foreign pointers before any RtThread deref.
/// @note Defined once per platform backend branch; both copies are identical.
/// @param obj Candidate runtime object.
/// @return Non-zero only for a valid live regular Thread handle.
int is_regular_thread_handle(void *obj) {
    return rt_obj_is_instance(obj, RT_THREAD_CLASS_ID, sizeof(RtThread)) &&
           thread_handle_magic(obj) == RT_THREAD_MAGIC;
}

/// @brief Global counter for assigning unique thread IDs.
static volatile LONG64 g_next_thread_id_win = 1;

/// @brief Atomically generates the next unique thread ID.
/// @return Monotonically increasing process-local runtime thread ID.
static int64_t next_thread_id_win(void) {
    return (int64_t)InterlockedIncrement64(&g_next_thread_id_win) - 1;
}

/// @brief Release and clear a Windows Thread's retained callback argument.
/// @param t Thread record whose owned argument is released.
static void rt_thread_release_owned_arg_win(RtThread *t) {
    if (!t || !t->owns_arg || !t->arg)
        return;
    if (rt_obj_release_check0(t->arg))
        rt_obj_free(t->arg);
    t->arg = NULL;
    t->owns_arg = 0;
}

/// @brief Finalize a Windows Thread record and its native resources.
/// @details Clears validation state, releases any retained argument, closes
///          the native thread handle, and destroys the critical section.
/// @param obj Thread object being finalized.
static void rt_thread_finalize_win(void *obj) {
    if (!obj)
        return;
    RtThread *t = (RtThread *)obj;
    t->magic = 0;
    rt_thread_release_owned_arg_win(t);
    if (t->hThread)
        CloseHandle(t->hThread);
    DeleteCriticalSection(&t->cs);
    // CONDITION_VARIABLE doesn't need explicit cleanup on Windows
}

/// @brief Adopt inherited context, run the callback, and publish completion.
/// @details The trampoline initializes stack-safety state, releases any owned
///          argument after callback exit, unbinds or cancels the reserved
///          context binding, wakes all joiners, and drops the Thread self-reference.
/// @param p Self-retained `RtThread` record passed to `_beginthreadex`.
/// @return Zero as the native thread exit status.
static unsigned __stdcall rt_thread_trampoline_win(void *p) {
    RtThread *t = (RtThread *)p;
    rt_init_stack_safety();
    int context_adopted =
        t && t->inherited_ctx && rt_context_adopt_reserved_thread_binding(t->inherited_ctx);
    if (context_adopted && t->entry)
        t->entry(t->arg);
    if (t)
        rt_thread_release_owned_arg_win(t);
    if (context_adopted)
        rt_set_current_context(NULL);
    else if (t && t->inherited_ctx)
        (void)rt_context_cancel_reserved_thread_binding(t->inherited_ctx);

    if (t) {
        EnterCriticalSection(&t->cs);
        t->finished = 1;
        WakeAllConditionVariable(&t->cv);
        LeaveCriticalSection(&t->cs);

        if (rt_obj_release_check0(t))
            rt_obj_free(t);
    }
    return 0;
}

/// @brief Validate and cast a regular Windows Thread handle.
/// @param thread Candidate runtime Thread object.
/// @param what Diagnostic used for NULL input.
/// @return Valid Thread payload, or NULL after a returning trap hook.
static RtThread *require_thread_win(void *thread, const char *what) {
    if (!thread) {
        rt_trap(what ? what : "Thread: null thread");
        return NULL;
    }
    if (!is_regular_thread_handle(thread)) {
        rt_trap("Thread: invalid thread handle");
        return NULL;
    }
    return (RtThread *)thread;
}

/// @brief Windows backend for `rt_thread_start*`.
///
/// Allocates an `RtThread` object, captures the active runtime
/// context (so the new thread inherits it), starts the OS thread
/// via `_beginthreadex`, and stows the handle for `join`. If
/// `retain_arg` is non-zero we GC-retain `arg` so the thread can
/// access it past the caller's lifetime.
/// Traps on null entry or `_beginthreadex` failure.
/// @param entry Typed callback executed once by the native thread.
/// @param arg Callback argument, or NULL.
/// @param retain_arg Whether a managed @p arg is retained through callback exit.
/// @return New caller-owned Thread handle, or NULL after a reported failure.
static void *rt_thread_start_impl_win(rt_thread_entry_fn entry, void *arg, int8_t retain_arg) {
    if (!entry)
        rt_trap("Thread.Start: null entry");
    if (!entry)
        return NULL;

    RtContext *ctx = rt_get_current_context();
    if (!ctx)
        ctx = rt_legacy_context();
    if (!ctx)
        return NULL;

    RtThread *t = (RtThread *)rt_obj_new_i64(RT_THREAD_CLASS_ID, (int64_t)sizeof(RtThread));
    if (!t)
        rt_trap("Thread.Start: failed to create thread");
    if (!t)
        return NULL;

    if (!InitializeCriticalSectionEx(&t->cs, 0, 0)) {
        thread_release_object(t);
        rt_trap("Thread.Start: synchronization initialization failed");
        return NULL;
    }
    InitializeConditionVariable(&t->cv);

    t->hThread = NULL;
    t->threadId = 0;
    t->finished = 0;
    t->joined = 0;
    t->magic = RT_THREAD_MAGIC;
    t->id = next_thread_id_win();
    t->inherited_ctx = ctx;
    t->entry = entry;
    t->arg = arg;
    t->owns_arg = 0;
    rt_obj_set_finalizer(t, rt_thread_finalize_win);
    if (retain_arg && arg) {
        if (!thread_try_retain_owned_value(arg, "Thread.StartOwned: arg retain failed")) {
            thread_release_object(t);
            return NULL;
        }
        t->owns_arg = 1;
    }

    // Hold a self-reference until the thread exits.
    if (!thread_try_retain_self(t, "Thread.Start: self retain failed")) {
        thread_release_object(t);
        return NULL;
    }

    /* Keep caller-owned context storage alive across the _beginthreadex-to-
       trampoline gap. The child consumes this reservation without a second
       binding-count increment. */
    if (!rt_context_reserve_thread_binding(ctx)) {
        thread_release_object(t);
        thread_release_object(t);
        return NULL;
    }

    {
        const uintptr_t native_handle =
            _beginthreadex(NULL, 0, rt_thread_trampoline_win, t, 0, &t->threadId);
        t->hThread = native_handle == 0 ? NULL : (HANDLE)native_handle;
    }
    if (!t->hThread) {
        // Drop thread self-reference and the caller-visible reference, then trap.
        (void)rt_context_cancel_reserved_thread_binding(ctx);
        thread_release_object(t);
        thread_release_object(t);
        rt_trap("Thread.Start: failed to create thread");
        return NULL;
    }

    return t;
}

/// @brief Start a Windows Thread with a typed callback and borrowed argument.
/// @param entry Typed callback executed once on the new thread.
/// @param arg Borrowed callback argument, or NULL.
/// @return New caller-owned Thread handle, or NULL after a reported failure.
void *rt_thread_start_fn(rt_thread_entry_fn entry, void *arg) {
    return rt_thread_start_impl_win(entry, arg, 0);
}

/// @brief Start a Windows Thread while retaining its managed argument.
/// @param entry Typed callback executed once on the new thread.
/// @param arg Managed value retained through callback exit, borrowed unmanaged pointer, or NULL.
/// @return New caller-owned Thread handle, or NULL after a reported failure.
void *rt_thread_start_owned_fn(rt_thread_entry_fn entry, void *arg) {
    return rt_thread_start_impl_win(entry, arg, 1);
}

/// @brief Public: `Thread.Start(entry, arg)` — start a thread without retaining `arg`.
/// The caller is responsible for keeping `arg` alive until the thread reads it.
/// @param entry Opaque representation of `void (*)(void *)`.
/// @param arg Borrowed callback argument, or NULL.
/// @return New caller-owned Thread handle, or NULL after a reported failure.
void *rt_thread_start(void *entry, void *arg) {
    return rt_thread_start_fn(thread_entry_from_opaque(entry), arg);
}

/// @brief Public: start a thread that owns its argument (GC-retains it for the thread's lifetime).
/// Use this when the caller's reference to `arg` may go out of scope before the thread runs.
/// @param entry Opaque representation of `void (*)(void *)`.
/// @param arg Managed value retained through callback exit, borrowed unmanaged pointer, or NULL.
/// @return New caller-owned Thread handle, or NULL after a reported failure.
void *rt_thread_start_owned(void *entry, void *arg) {
    return rt_thread_start_owned_fn(thread_entry_from_opaque(entry), arg);
}

/// @brief Block until a Thread or SafeThread finishes executing.
/// @details Completion is repeatably observable. NULL, wrong-class, self-join,
///          and native condition-wait failures trap. SafeThread handles
///          delegate to their inner Thread without rethrowing captured callback errors.
/// @param thread Thread or SafeThread handle to join.
void rt_thread_join(void *thread) {
    if (is_safe_thread_handle(thread)) {
        rt_thread_safe_join(thread);
        return;
    }
    RtThread *t = require_thread_win(thread, "Thread.Join: null thread");
    if (!t)
        return;
    rt_obj_retain_maybe(thread);

    EnterCriticalSection(&t->cs);
    if (!t->finished && GetCurrentThreadId() == t->threadId) {
        LeaveCriticalSection(&t->cs);
        thread_release_object(thread);
        rt_trap("Thread.Join: cannot join self");
        return;
    }
    while (!t->finished) {
        if (!SleepConditionVariableCS(&t->cv, &t->cs, INFINITE)) {
            LeaveCriticalSection(&t->cs);
            thread_release_object(thread);
            rt_trap("Thread.Join: condition wait failed");
            return;
        }
    }
    LeaveCriticalSection(&t->cs);
    thread_release_object(thread);
}

/// @brief Poll a Thread or SafeThread for completion without blocking.
/// @param thread Thread or SafeThread handle to inspect.
/// @return One when finished, otherwise zero.
int8_t rt_thread_try_join(void *thread) {
    if (is_safe_thread_handle(thread)) {
        rt_obj_retain_maybe(thread);
        SafeThreadCtx *ctx = (SafeThreadCtx *)thread;
        void *inner = safe_thread_copy_inner_thread(ctx);
        thread_release_object(thread);
        return thread_try_join_inner_or_release(inner);
    }
    RtThread *t = require_thread_win(thread, "Thread.TryJoin: null thread");
    if (!t)
        return 0;
    rt_obj_retain_maybe(thread);

    EnterCriticalSection(&t->cs);
    if (!t->finished && GetCurrentThreadId() == t->threadId) {
        LeaveCriticalSection(&t->cs);
        thread_release_object(thread);
        rt_trap("Thread.Join: cannot join self");
        return 0;
    }
    if (!t->finished) {
        LeaveCriticalSection(&t->cs);
        thread_release_object(thread);
        return 0;
    }
    LeaveCriticalSection(&t->cs);
    thread_release_object(thread);
    return 1;
}

/// @brief Bounded join — wait at most `ms` milliseconds. Returns 1 on join, 0 on timeout.
/// `ms < 0` waits forever (delegates to `rt_thread_join`); `ms == 0` is a try-join.
/// @param thread Thread or SafeThread handle to join.
/// @param ms Maximum wait in milliseconds; negative means indefinite.
/// @return One when finished, otherwise zero on timeout or a returning trap path.
int8_t rt_thread_join_for(void *thread, int64_t ms) {
    if (is_safe_thread_handle(thread)) {
        if (ms < 0) {
            rt_thread_safe_join(thread);
            return 1;
        }
        rt_obj_retain_maybe(thread);
        SafeThreadCtx *ctx = (SafeThreadCtx *)thread;
        void *inner = safe_thread_copy_inner_thread(ctx);
        thread_release_object(thread);
        return thread_join_for_inner_or_release(inner, ms);
    }
    RtThread *t = require_thread_win(thread, "Thread.JoinFor: null thread");
    if (!t)
        return 0;

    if (ms < 0) {
        rt_thread_join(thread);
        return 1;
    }

    rt_obj_retain_maybe(thread);

    EnterCriticalSection(&t->cs);
    if (!t->finished && GetCurrentThreadId() == t->threadId) {
        LeaveCriticalSection(&t->cs);
        thread_release_object(thread);
        rt_trap("Thread.Join: cannot join self");
        return 0;
    }
    if (ms == 0) {
        if (!t->finished) {
            LeaveCriticalSection(&t->cs);
            thread_release_object(thread);
            return 0;
        }
        LeaveCriticalSection(&t->cs);
        thread_release_object(thread);
        return 1;
    }

    // Use an absolute deadline so spurious wakes and long finite waits retain
    // the caller's requested duration without ever passing INFINITE (VDOC-129).
    ULONGLONG deadline = rt_win32_deadline_from_now_ms(ms);

    while (!t->finished) {
        DWORD remaining = rt_win32_wait_slice_until(deadline);
        if (remaining == 0) {
            LeaveCriticalSection(&t->cs);
            thread_release_object(thread);
            return 0;
        }
        BOOL ok = SleepConditionVariableCS(&t->cv, &t->cs, remaining);
        if (!ok && !t->finished) {
            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT)
                continue;
            LeaveCriticalSection(&t->cs);
            thread_release_object(thread);
            rt_trap("Thread.JoinFor: condition wait failed");
            return 0;
        }
    }

    LeaveCriticalSection(&t->cs);
    thread_release_object(thread);
    return 1;
}

/// @brief Get a Thread or SafeThread's process-local runtime ID.
/// @param thread Thread or SafeThread handle to inspect.
/// @return Stable positive runtime ID, or zero after a returning trap hook.
int64_t rt_thread_get_id(void *thread) {
    if (is_safe_thread_handle(thread))
        return rt_thread_safe_get_id(thread);
    RtThread *t = require_thread_win(thread, "Thread.get_Id: null thread");
    if (!t)
        return 0;
    rt_obj_retain_maybe(thread);
    EnterCriticalSection(&t->cs);
    int64_t id = t->id;
    LeaveCriticalSection(&t->cs);
    thread_release_object(thread);
    return id;
}

/// @brief Check whether a Thread or SafeThread callback is still running.
/// @param thread Thread or SafeThread handle to inspect.
/// @return One before callback completion, otherwise zero.
int8_t rt_thread_get_is_alive(void *thread) {
    if (is_safe_thread_handle(thread))
        return rt_thread_safe_is_alive(thread);
    RtThread *t = require_thread_win(thread, "Thread.get_IsAlive: null thread");
    if (!t)
        return 0;
    rt_obj_retain_maybe(thread);
    EnterCriticalSection(&t->cs);
    const int alive = t->finished ? 0 : 1;
    LeaveCriticalSection(&t->cs);
    thread_release_object(thread);
    return (int8_t)alive;
}

/// @brief Sleep the calling thread for a bounded millisecond duration.
/// @param ms Requested duration, clamped to `[0, INT32_MAX]`.
void rt_thread_sleep(int64_t ms) {
    if (ms < 0)
        ms = 0;
    if (ms > INT32_MAX)
        ms = INT32_MAX;
    rt_sleep_ms((int32_t)ms);
}

/// @brief Hint to the OS that we're willing to yield the CPU (Win32 `SwitchToThread`).
void rt_thread_yield(void) {
    if (!SwitchToThread())
        Sleep(0);
}
#endif // defined(_WIN32)
