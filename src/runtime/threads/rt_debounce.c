//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_debounce.c
// Purpose: Implements signal-and-poll rate-limiting primitives for Zia code:
//          `Zanna.Threads.Debouncer` (becomes ready only after a quiet period
//          since the last `Signal`) and `Zanna.Threads.Throttler` (allows at
//          most one `Try` to succeed per fixed interval). These are passive
//          state objects — they do *not* hold a stored callback or fire any
//          deferred work; the calling Zia loop polls `IsReady`/`Try` and
//          decides what to do with the result.
//
// Key invariants:
//   - `Signal` resets the debouncer's timer to "now"; `IsReady` returns true
//     only after `delay_ms` elapses with no further `Signal` calls.
//   - `Try` atomically claims the throttle slot when the interval has elapsed
//     since the previous successful `Try`, returning 1 to the caller and
//     stamping the new last-allowed time. Otherwise returns 0 immediately.
//   - Time is measured with a monotonic clock (CLOCK_MONOTONIC on POSIX,
//     QueryPerformanceCounter on Win32) to avoid wall-clock skew.
//   - All public entry points lock the per-object monitor to keep state
//     coherent under concurrent reads/writes.
//
// Ownership/Lifetime:
//   - Debouncer and throttler objects are heap-allocated and managed by the GC.
//   - Each instance owns one runtime monitor that is released by the finalizer.
//   - No callback function pointers are stored; lifetime questions about
//     callables don't apply to this module.
//
// Links: src/runtime/threads/rt_debounce.h (public API),
//        src/runtime/threads/rt_scheduler.h (timer-based scheduler, related concept)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Passive, monitor-protected debounce and throttle timing state.
/// @details These objects do not schedule callbacks. Callers signal or attempt
///          an operation and poll readiness using monotonic millisecond samples.

#include "rt_debounce.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_threads.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

// --- Helper: current time in milliseconds ---

#if defined(_WIN32)
static INIT_ONCE g_debounce_freq_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER g_debounce_freq;

/// @brief One-shot initializer for the Win32 performance-counter frequency.
/// @details Run at most once per process via `InitOnceExecuteOnce`.
///          `QueryPerformanceFrequency` is invariant for the lifetime of the system, so we
///          cache it once into `g_debounce_freq` rather than calling it on every timing
///          sample. Always returns TRUE so a failed probe is cached and later samples use
///          the monotonic `GetTickCount64` fallback.
/// @param once Windows one-time initialization state.
/// @param param Unused initialization parameter.
/// @param ctx Unused output-context slot.
/// @return Always @c TRUE to mark initialization complete.
static BOOL CALLBACK debounce_freq_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    if (!QueryPerformanceFrequency(&g_debounce_freq))
        g_debounce_freq.QuadPart = 0;
    return TRUE;
}
#endif

/// @brief Read the current monotonic time in milliseconds.
/// @details Uses `QueryPerformanceCounter` on Win32 (with the cached frequency to convert
///          ticks → ms via a split-modulo to avoid `int64_t` overflow on long-running
///          processes), with `GetTickCount64` as a fallback. POSIX uses `CLOCK_MONOTONIC`,
///          falling back to `CLOCK_REALTIME`; it returns 0 only if both POSIX sources fail.
/// @return Current monotonic or fallback time in whole milliseconds.
static int64_t current_time_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    InitOnceExecuteOnce(&g_debounce_freq_once, debounce_freq_init, NULL, NULL);
    if (g_debounce_freq.QuadPart <= 0 || !QueryPerformanceCounter(&counter)) {
        ULONGLONG ticks = GetTickCount64();
        return ticks > (ULONGLONG)INT64_MAX ? INT64_MAX : (int64_t)ticks;
    }
    return (int64_t)((counter.QuadPart / g_debounce_freq.QuadPart) * 1000LL +
                     ((counter.QuadPart % g_debounce_freq.QuadPart) * 1000LL) /
                         g_debounce_freq.QuadPart);
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    return 0;
#endif
}

/// @brief Return elapsed milliseconds since @p start_ms, clamping backward clocks to zero.
/// @param start_ms Earlier millisecond timestamp.
/// @return Nonnegative elapsed milliseconds.
static int64_t elapsed_since_ms(int64_t start_ms) {
    int64_t now = current_time_ms();
    return now > start_ms ? now - start_ms : 0;
}

// --- Debouncer ---

/// @brief Monitor-protected state of one passive debouncer.
typedef struct {
    void *monitor;
    int64_t delay_ms;
    int64_t last_signal_time;
    int64_t signal_count;
    int8_t has_signal;
} rt_debounce_data;

/// @brief Validate that `debouncer` is a debouncer object, trapping on misuse.
/// @details `trap_on_null` controls the NULL-handling policy: when 1, a NULL pointer raises a
///          trap; when 0 it silently returns NULL so callers that want a no-op on null inputs
///          (e.g. `IsReady(null) → 0`) can keep their hot path simple. A non-null pointer
///          with the wrong class id always traps regardless of the flag — that's a
///          programmer error we want surfaced loudly.
/// @param debouncer Candidate Debouncer handle.
/// @param trap_on_null Nonzero to trap on null.
/// @return Validated payload, or null for an accepted null/trap path.
static rt_debounce_data *debounce_require(void *debouncer, int8_t trap_on_null) {
    if (!debouncer) {
        if (trap_on_null)
            rt_trap("Debouncer: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(debouncer, RT_DEBOUNCER_CLASS_ID, sizeof(rt_debounce_data))) {
        rt_trap("Debouncer: invalid object");
        return NULL;
    }
    return (rt_debounce_data *)debouncer;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime-managed object reference, or null.
static void debounce_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer for a `Debounce` timer — releases the monitor the timer
///        synchronizes on, no-op if already torn down.
/// @param obj Debouncer payload to finalize; null is ignored.
static void debounce_finalizer(void *obj) {
    rt_debounce_data *data = (rt_debounce_data *)obj;
    if (!data || !data->monitor)
        return;
    if (rt_obj_release_check0(data->monitor))
        rt_obj_free(data->monitor);
    data->monitor = NULL;
}

/// @brief Create a new debouncer — signal() must be followed by delay_ms of quiet before is_ready()
/// returns true.
/// @param delay_ms Quiet period in milliseconds; negative values become zero.
/// @return New runtime-managed Debouncer, or null after an allocation trap.
void *rt_debounce_new(int64_t delay_ms) {
    void *obj = rt_obj_new_i64(RT_DEBOUNCER_CLASS_ID, sizeof(rt_debounce_data));
    if (!obj) {
        rt_trap("Debouncer.New: memory allocation failed");
        return NULL;
    }
    rt_debounce_data *data = (rt_debounce_data *)obj;
    data->monitor = rt_obj_new_i64(0, 1);
    if (!data->monitor) {
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Debouncer.New: memory allocation failed");
        return NULL;
    }
    data->delay_ms = delay_ms > 0 ? delay_ms : 0;
    data->last_signal_time = 0;
    data->signal_count = 0;
    data->has_signal = 0;
    rt_obj_set_finalizer(obj, debounce_finalizer);
    return obj;
}

/// @brief Bump the debouncer's "last activity" timestamp to now and increment the signal count.
/// @details Each call resets the quiet-period clock — `IsReady` won't return true again until
///          `delay_ms` has elapsed since *this* call (not the previous one). The signal count
///          saturates at `INT64_MAX` rather than wrapping, which is fine for diagnostics use
///          and avoids a wraparound surprise in long-running daemons.
/// @param debouncer Debouncer handle; null is ignored.
void rt_debounce_signal(void *debouncer) {
    if (!debouncer)
        return;
    rt_debounce_data *data = debounce_require(debouncer, 0);
    if (!data)
        return;
    rt_obj_retain_maybe(debouncer);
    rt_monitor_enter(data->monitor);
    data->last_signal_time = current_time_ms();
    data->has_signal = 1;
    if (data->signal_count < INT64_MAX)
        data->signal_count++;
    rt_monitor_exit(data->monitor);
    debounce_release_object(debouncer);
}

/// @brief Return 1 if `delay_ms` has elapsed since the last `Signal`, else 0.
/// @details Always returns 0 before the first `Signal` call — the explicit `has_signal` flag
///          (set by `Signal`, cleared by `Reset`) distinguishes "never signalled" from a
///          coincidental `last_signal_time == 0` reading on systems whose monotonic clock
///          starts at zero. Elapsed time is computed via `elapsed_since_ms`, which clamps
///          backward-clock readings to zero so a system-time adjustment cannot make a
///          previously-ready debouncer regress to "not ready".
/// @param debouncer Debouncer handle; null is not ready.
/// @return 1 after a signal followed by the configured quiet period, otherwise 0.
int8_t rt_debounce_is_ready(void *debouncer) {
    if (!debouncer)
        return 0;
    rt_debounce_data *data = debounce_require(debouncer, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(debouncer);
    rt_monitor_enter(data->monitor);
    if (!data->has_signal) {
        rt_monitor_exit(data->monitor);
        debounce_release_object(debouncer);
        return 0; // Never signaled
    }
    int64_t elapsed = elapsed_since_ms(data->last_signal_time);
    int8_t ready = elapsed >= data->delay_ms ? 1 : 0;
    rt_monitor_exit(data->monitor);
    debounce_release_object(debouncer);
    return ready;
}

/// @brief Return a debouncer to its never-signalled state.
/// @param debouncer Debouncer handle; null is ignored.
void rt_debounce_reset(void *debouncer) {
    if (!debouncer)
        return;
    rt_debounce_data *data = debounce_require(debouncer, 0);
    if (!data)
        return;
    rt_obj_retain_maybe(debouncer);
    rt_monitor_enter(data->monitor);
    data->last_signal_time = 0;
    data->signal_count = 0;
    data->has_signal = 0;
    rt_monitor_exit(data->monitor);
    debounce_release_object(debouncer);
}

/// @brief Read the configured nonnegative debounce delay.
/// @param debouncer Debouncer handle; null reports zero.
/// @return Delay in milliseconds.
int64_t rt_debounce_get_delay(void *debouncer) {
    if (!debouncer)
        return 0;
    rt_debounce_data *data = debounce_require(debouncer, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(debouncer);
    rt_monitor_enter(data->monitor);
    int64_t delay = data->delay_ms;
    rt_monitor_exit(data->monitor);
    debounce_release_object(debouncer);
    return delay;
}

/// @brief Return the cumulative number of `Signal` calls observed by this debouncer.
/// @details Useful for instrumentation — e.g. logging "coalesced N signals into one event"
///          when the debouncer finally fires. Counter saturates at `INT64_MAX`. Reset to
///          zero by `rt_debounce_reset`.
/// @param debouncer Debouncer handle; null reports zero.
/// @return Saturating signal count since construction or the last reset.
int64_t rt_debounce_get_signal_count(void *debouncer) {
    if (!debouncer)
        return 0;
    rt_debounce_data *data = debounce_require(debouncer, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(debouncer);
    rt_monitor_enter(data->monitor);
    int64_t count = data->signal_count;
    rt_monitor_exit(data->monitor);
    debounce_release_object(debouncer);
    return count;
}

// --- Throttler ---

/// @brief Monitor-protected state of one passive throttler.
typedef struct {
    void *monitor;
    int64_t interval_ms;
    int64_t last_allowed_time;
    int64_t count;
    int8_t has_last_allowed;
} rt_throttle_data;

/// @brief Validate that `throttler` is a throttler object, trapping on misuse.
/// @details Mirror of `debounce_require` for the throttler class id. Same `trap_on_null`
///          contract: 1 to trap on NULL, 0 to silently return NULL so callers can shortcut
///          the no-op case. A wrong class id always traps.
/// @param throttler Candidate Throttler handle.
/// @param trap_on_null Nonzero to trap on null.
/// @return Validated payload, or null for an accepted null/trap path.
static rt_throttle_data *throttle_require(void *throttler, int8_t trap_on_null) {
    if (!throttler) {
        if (trap_on_null)
            rt_trap("Throttler: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(throttler, RT_THROTTLER_CLASS_ID, sizeof(rt_throttle_data))) {
        rt_trap("Throttler: invalid object");
        return NULL;
    }
    return (rt_throttle_data *)throttler;
}

/// @brief GC finalizer for a `Throttle` limiter — mirror of `debounce_finalizer`
///        but on the throttle-data struct.
/// @param obj Throttler payload to finalize; null is ignored.
static void throttle_finalizer(void *obj) {
    rt_throttle_data *data = (rt_throttle_data *)obj;
    if (!data || !data->monitor)
        return;
    if (rt_obj_release_check0(data->monitor))
        rt_obj_free(data->monitor);
    data->monitor = NULL;
}

/// @brief Create a new throttle — try() returns true at most once per interval_ms.
/// @param interval_ms Minimum interval in milliseconds; negative values become zero.
/// @return New runtime-managed Throttler, or null after an allocation trap.
void *rt_throttle_new(int64_t interval_ms) {
    void *obj = rt_obj_new_i64(RT_THROTTLER_CLASS_ID, sizeof(rt_throttle_data));
    if (!obj) {
        rt_trap("Throttler.New: memory allocation failed");
        return NULL;
    }
    rt_throttle_data *data = (rt_throttle_data *)obj;
    data->monitor = rt_obj_new_i64(0, 1);
    if (!data->monitor) {
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Throttler.New: memory allocation failed");
        return NULL;
    }
    data->interval_ms = interval_ms > 0 ? interval_ms : 0;
    data->last_allowed_time = 0;
    data->count = 0;
    data->has_last_allowed = 0;
    rt_obj_set_finalizer(obj, throttle_finalizer);
    return obj;
}

/// @brief Atomically claim the current throttle slot if it is available.
/// @details The first attempt always succeeds. Success stamps the current time
///          and increments a saturating count; rejection changes no state.
/// @param throttler Throttler handle; null is rejected.
/// @return 1 when allowed, otherwise 0.
int8_t rt_throttle_try(void *throttler) {
    if (!throttler)
        return 0;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    int64_t now = current_time_ms();
    int64_t elapsed = data->has_last_allowed ? (now - data->last_allowed_time) : 0;
    if (elapsed < 0)
        elapsed = 0;
    if (!data->has_last_allowed || elapsed >= data->interval_ms) {
        data->last_allowed_time = now;
        data->has_last_allowed = 1;
        if (data->count < INT64_MAX)
            data->count++;
        rt_monitor_exit(data->monitor);
        debounce_release_object(throttler);
        return 1;
    }
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
    return 0;
}

/// @brief Check availability without claiming the throttle slot.
/// @param throttler Throttler handle; null is rejected.
/// @return 1 before the first success or after the interval, otherwise 0.
int8_t rt_throttle_can_proceed(void *throttler) {
    if (!throttler)
        return 0;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    if (!data->has_last_allowed) {
        rt_monitor_exit(data->monitor);
        debounce_release_object(throttler);
        return 1;
    }
    int64_t elapsed = elapsed_since_ms(data->last_allowed_time);
    int8_t ready = elapsed >= data->interval_ms ? 1 : 0;
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
    return ready;
}

/// @brief Clear throttle timing and the successful-operation count.
/// @param throttler Throttler handle; null is ignored.
void rt_throttle_reset(void *throttler) {
    if (!throttler)
        return;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    data->last_allowed_time = 0;
    data->count = 0;
    data->has_last_allowed = 0;
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
}

/// @brief Read the configured nonnegative throttle interval.
/// @param throttler Throttler handle; null reports zero.
/// @return Interval in milliseconds.
int64_t rt_throttle_get_interval(void *throttler) {
    if (!throttler)
        return 0;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    int64_t interval = data->interval_ms;
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
    return interval;
}

/// @brief Return the cumulative number of `Try` calls that succeeded on this throttler.
/// @details Counts successes only — calls that returned 0 (rate-limited) are not included.
///          Useful for telemetry like "events allowed through this rate limiter so far".
///          Counter saturates at `INT64_MAX`. Reset to zero by `rt_throttle_reset`.
/// @param throttler Throttler handle; null reports zero.
/// @return Saturating successful-operation count.
int64_t rt_throttle_get_count(void *throttler) {
    if (!throttler)
        return 0;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    int64_t count = data->count;
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
    return count;
}

/// @brief Compute the nonnegative time until the next claim can succeed.
/// @param throttler Throttler handle; null is treated as ready.
/// @return Milliseconds remaining, or zero before the first success or when ready.
int64_t rt_throttle_remaining_ms(void *throttler) {
    if (!throttler)
        return 0;
    rt_throttle_data *data = throttle_require(throttler, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(throttler);
    rt_monitor_enter(data->monitor);
    if (!data->has_last_allowed) {
        rt_monitor_exit(data->monitor);
        debounce_release_object(throttler);
        return 0;
    }
    int64_t elapsed = elapsed_since_ms(data->last_allowed_time);
    int64_t remaining = data->interval_ms - elapsed;
    int64_t out = remaining > 0 ? remaining : 0;
    rt_monitor_exit(data->monitor);
    debounce_release_object(throttler);
    return out;
}
