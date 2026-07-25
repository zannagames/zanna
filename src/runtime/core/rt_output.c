//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_output.c
// Purpose: Implements process-global runtime output routing, optional capture,
//          POSIX stdout buffering, and reference-counted flush suppression for
//          terminal-rendering batches.
//
// Key invariants:
//   - Initialization (rt_output_init) is idempotent and uses double-checked
//     locking with acquire/release atomics; concurrent callers are safe.
//   - POSIX output uses a static 16 KiB fully buffered stdout stream; native
//     Windows output bypasses CRT stdio and writes to the OS stdout handle.
//   - An installed capture callback receives runtime writes instead of stdout.
//     Hook replacement is synchronized, but callback invocations may overlap.
//   - Batch mode is reference-counted (g_batch_mode_depth); nested begin/end
//     calls work correctly and only the outermost POSIX end flushes.
//   - Batch mode controls conditional flushes; it does not maintain a separate
//     byte buffer or delay capture callbacks.
//
// Ownership/Lifetime:
//   - The internal output buffer (g_output_buffer) is a process-global static
//     array registered with setvbuf; it must remain valid for the process
//     lifetime (guaranteed because it is static).
//   - Capture hooks borrow write buffers only for the callback invocation and
//     must copy data they retain. Hook contexts remain caller-owned.
//   - The module performs no explicit heap allocation.
//
// Links: src/runtime/core/rt_output.h (public API),
//        src/runtime/core/rt_term.c (terminal control, uses rt_output),
//        src/runtime/core/rt_io.c (higher-level PRINT/INPUT primitives)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Process-global runtime stdout routing, capture, and batching.

#include "rt_output.h"

#include "rt_atomic_compat.h"
#include "rt_platform.h"
#include "rt_trap.h"

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Size of the stdout buffer.
/// @details 16KB is sufficient for several full screens of output.
///          Larger buffers reduce flush frequency but increase memory usage.
#define RT_OUTPUT_BUFFER_SIZE 16384

/// @brief Internal stdout buffer for full buffering mode.
static char g_output_buffer[RT_OUTPUT_BUFFER_SIZE];

/// @brief Atomic init state: 0=uninit, 1=initializing, 2=done.
/// @details Uses double-checked locking (same pattern as rt_context.c) so that
///          concurrent calls to rt_output_init are safe without a mutex.
static int g_output_init_state = 0;

/// @brief Reference count for nested batch mode calls (atomic).
/// @details Allows nested begin/end batch calls to work correctly across threads.
static int g_batch_mode_depth = 0;

/// @brief Whether an exit-time stdout flush has been registered.
static int g_output_exit_handler_registered = 0;

/// @brief Process-global runtime stdout capture callback.
/// @details The runtime output layer is already process-global, so capture uses
///          the same scope. The REPL installs this only around one VM execution.
static rt_output_capture_hook g_output_capture_hook = {NULL, NULL};

/// @brief Spinlock protecting capture-hook replacement and snapshot reads.
/// @details The callback pointer and opaque context must be observed as one
///          consistent pair.  A small spinlock is sufficient because hook
///          changes are rare and snapshot reads copy only two machine words.
static int g_output_capture_lock;

/// @brief Yield the current thread while waiting for a rare output lock.
/// @details Uses `SwitchToThread` on Windows and `sched_yield` elsewhere.
static void rt_output_yield_(void) {
#if RT_PLATFORM_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}

/// @brief Acquire the output capture spinlock.
/// @details Spins with cooperative thread yields until it atomically acquires
///          the lock. The lock protects only hook-pair snapshots and
///          replacement, never user callback execution.
static void rt_output_capture_lock_(void) {
    if (__atomic_test_and_set(&g_output_capture_lock, __ATOMIC_ACQUIRE)) {
        do {
            rt_output_yield_();
        } while (__atomic_test_and_set(&g_output_capture_lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release the output capture spinlock.
/// @details Publishes writes to the hook callback/context pair with release
///          ordering.
static void rt_output_capture_unlock_(void) {
    __atomic_clear(&g_output_capture_lock, __ATOMIC_RELEASE);
}

/// @brief Flush buffered stdout during normal process termination.
/// @details Delegates to @ref rt_output_flush. On native Windows this is a
///          harmless no-op because output bypasses CRT buffering.
static void rt_output_flush_at_exit_(void) {
    rt_output_flush();
}

#if RT_PLATFORM_LINUX
/// @brief Register a DSO-aware process-exit callback through the C++ ABI.
/// @param func Callback receiving @p arg at process or DSO teardown.
/// @param arg Opaque callback argument.
/// @param dso_handle Optional DSO identity; `NULL` registers process-wide.
/// @return Zero on successful registration; otherwise a non-zero ABI error.
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);

/// @brief Adapt the output flush callback to `__cxa_atexit`'s signature.
/// @param arg Ignored opaque argument supplied during registration.
static void rt_output_flush_at_exit_adapter_(void *arg) {
    (void)arg;
    rt_output_flush_at_exit_();
}

/// @brief Register the exit-time stdout flush handler.
/// @details Linux routes through libc's __cxa_atexit() (plain atexit() is not
///          reliably available for late-bound native executables).
/// @return Zero on success, or the non-zero status from `__cxa_atexit`.
static int rt_output_register_exit_handler_(void) {
    return __cxa_atexit(rt_output_flush_at_exit_adapter_, NULL, NULL);
}
#else
/// @brief Register the exit-time stdout flush handler with `atexit`.
/// @return Zero on success, or the non-zero status from `atexit`.
static int rt_output_register_exit_handler_(void) {
    return atexit(rt_output_flush_at_exit_);
}
#endif

/// @brief Write @p len raw bytes to the platform's standard-output stream.
/// @details Two implementations live behind the `RT_PLATFORM_WINDOWS` macro: Win32 uses
///          `WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ...)` to avoid CRT translation,
///          POSIX uses `fwrite(stdout)`. The Win32 path chunks at `0xFFFFFFFF` because
///          `WriteFile`'s `nNumberOfBytesToWrite` is `DWORD` so a single `size_t` may
///          exceed it on a 64-bit build. Invalid handles, write errors, and zero-byte
///          progress stop the operation silently, potentially after a partial write.
/// @param s Source byte range; null is a no-op.
/// @param len Number of bytes to write; zero is a no-op.
#if RT_PLATFORM_WINDOWS
/// @brief Write a raw byte range directly to the Windows stdout handle.
static void rt_output_write_bytes(const char *s, size_t len) {
    if (!s || len == 0)
        return;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == NULL || out == INVALID_HANDLE_VALUE)
        return;

    while (len > 0) {
        const DWORD chunk = len > 0xFFFFFFFFu ? 0xFFFFFFFFu : (DWORD)len;
        DWORD written = 0;
        if (!WriteFile(out, s, chunk, &written, NULL) || written == 0)
            return;
        s += written;
        len -= written;
    }
}
#endif

/// @brief Install @p hook as the current runtime output capture target.
/// @details Returns the old hook for scoped restoration. This intentionally does
///          not call @ref rt_output_init because capture should also work before
///          stdout buffering is initialized. Replacement and snapshot reads are
///          synchronized, but a thread that already copied the old hook may
///          invoke it after this function returns.
/// @param fn New callback, or `NULL` to route subsequent snapshots to stdout.
/// @param ctx Opaque context stored with @p fn; it is not owned by the runtime.
/// @return The callback/context pair replaced by this call.
rt_output_capture_hook rt_output_set_capture_hook(rt_output_capture_fn fn, void *ctx) {
    rt_output_capture_lock_();
    rt_output_capture_hook oldHook = g_output_capture_hook;
    g_output_capture_hook.fn = fn;
    g_output_capture_hook.ctx = ctx;
    rt_output_capture_unlock_();
    return oldHook;
}

/// @brief Try to deliver bytes to the active capture hook.
/// @details Returns non-zero when a hook consumed the output. A null byte range
///          or zero-length write is treated as consumed because there is nothing
///          left for stdout to do. The hook pair is copied under the spinlock,
///          then invoked without the lock so callbacks may perform nested output
///          or replace the hook. Concurrent writers may invoke the callback
///          simultaneously.
/// @param s Byte range offered to capture.
/// @param len Number of bytes at @p s.
/// @return Non-zero for an empty write or when a callback was invoked; zero
///         when non-empty data should fall through to stdout.
static int rt_output_try_capture_(const char *s, size_t len) {
    if (!s || len == 0)
        return 1;
    rt_output_capture_lock_();
    rt_output_capture_hook hook = g_output_capture_hook;
    rt_output_capture_unlock_();
    if (!hook.fn)
        return 0;
    hook.fn(s, len, hook.ctx);
    return 1;
}

/// @brief Initialize stdout buffering (idempotent, thread-safe).
/// @details One winning thread configures non-Windows stdout for full buffering
///          with the static 16 KiB buffer and attempts to register a normal-exit
///          flush. Native Windows performs no CRT setup because writes use
///          `WriteFile`. Other callers either observe completion or yield until
///          it is published. The return values from `setvbuf` and failed exit
///          registration are not surfaced or retried.
void rt_output_init(void) {
    if (__atomic_load_n(&g_output_init_state, __ATOMIC_ACQUIRE) == 2)
        return;

    int expected = 0;
    if (__atomic_compare_exchange_n(
            &g_output_init_state, &expected, 1, /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
#if RT_PLATFORM_WINDOWS
        // Native-linked Windows executables enter directly through Zanna's
        // startup shim, bypassing CRT startup. Avoid configuring CRT stdio in
        // that mode; writes below go straight to the OS handle.
#else
        // Configure stdout for full buffering with our internal buffer.
        // _IOFBF = full buffering: output is written when buffer is full or fflush() is called.
        // This is the key change that reduces system calls.
        setvbuf(stdout, g_output_buffer, _IOFBF, RT_OUTPUT_BUFFER_SIZE);
        if (!g_output_exit_handler_registered && rt_output_register_exit_handler_() == 0)
            g_output_exit_handler_registered = 1;
#endif
        __atomic_store_n(&g_output_init_state, 2, __ATOMIC_RELEASE);
        return;
    }

    // Another thread is initializing; spin until done.
    while (__atomic_load_n(&g_output_init_state, __ATOMIC_ACQUIRE) != 2) {
        rt_output_yield_();
    }
}

/// @brief Route a NUL-terminated string to capture or stdout without flushing.
/// @details Null input is ignored. An installed capture hook receives the bytes
///          before output initialization and completely replaces stdout
///          delivery. Otherwise the function initializes output and writes up
///          to the first NUL byte. Stream and OS write failures are not
///          reported.
/// @param s NUL-terminated text to write; may be `NULL`.
void rt_output_str(const char *s) {
    if (!s)
        return;
    if (rt_output_try_capture_(s, strlen(s)))
        return;
    rt_output_init();
#if RT_PLATFORM_WINDOWS
    rt_output_write_bytes(s, strlen(s));
#else
    fputs(s, stdout);
#endif
}

/// @brief Route an explicit byte range to capture or stdout without flushing.
/// @details Null and zero-length ranges are ignored. Embedded NUL bytes are
///          preserved. An installed capture hook completely replaces stdout
///          delivery; otherwise output is initialized and the requested range
///          is passed to the platform writer. Write failures are not reported.
/// @param s Start of the byte range; may be `NULL` only for a no-op.
/// @param len Number of bytes to write.
void rt_output_strn(const char *s, size_t len) {
    if (!s || len == 0)
        return;
    if (rt_output_try_capture_(s, len))
        return;
    rt_output_init();
#if RT_PLATFORM_WINDOWS
    rt_output_write_bytes(s, len);
#else
    fwrite(s, 1, len, stdout);
#endif
}

/// @brief Flush the stdout buffer immediately.
/// @details Calls `fflush(stdout)` on non-Windows platforms and ignores its
///          status. Native Windows writes are already sent directly to the OS,
///          so this function is a no-op. Capture callbacks have no module-owned
///          buffer to flush.
void rt_output_flush(void) {
#if !RT_PLATFORM_WINDOWS
    fflush(stdout);
#endif
}

/// @brief Enter batch mode — defer all flushes until the matching end_batch.
/// @details Atomically increments the process-global reference count. Nested
///          and cross-thread calls share the same depth. An attempt to exceed
///          `INT_MAX` traps and leaves the depth unchanged.
void rt_output_begin_batch(void) {
    int cur = __atomic_load_n(&g_batch_mode_depth, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur == INT_MAX) {
            rt_trap("rt_output_begin_batch: batch depth overflow");
            return;
        }
        int next = cur + 1;
        if (__atomic_compare_exchange_n(&g_batch_mode_depth,
                                        &cur,
                                        next,
                                        /*weak=*/0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;
    }
}

/// @brief Exit batch mode — flush stdout when the outermost batch ends.
/// @details Decrements the reference counter. Only the outermost end triggers
///          a POSIX `fflush(stdout)`; Windows requires no flush. Unbalanced end
///          calls are no-ops. The atomic transition is safe across threads,
///          although batching remains process-global rather than thread-local.
void rt_output_end_batch(void) {
    int cur = __atomic_load_n(&g_batch_mode_depth, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur <= 0)
            return;
        int next = cur - 1;
        if (__atomic_compare_exchange_n(&g_batch_mode_depth,
                                        &cur,
                                        next,
                                        /*weak=*/0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (next == 0) {
#if !RT_PLATFORM_WINDOWS
                fflush(stdout);
#endif
            }
            return;
        }
    }
}

/// @brief Return non-zero if batch mode is currently active.
/// @return One when the atomically observed process-global depth is positive;
///         otherwise zero.
int8_t rt_output_is_batch_mode(void) {
    return __atomic_load_n(&g_batch_mode_depth, __ATOMIC_ACQUIRE) > 0;
}

/// @brief Flush stdout only if not in batch mode.
/// @details Used by PRINT/SAY functions that want immediate output when running
///          interactively but deferred output during canvas rendering loops.
///          The depth test and flush are separate operations, so a concurrent
///          begin may race with the decision. Windows and capture-only output
///          require no module flush.
void rt_output_flush_if_not_batch(void) {
    if (__atomic_load_n(&g_batch_mode_depth, __ATOMIC_ACQUIRE) == 0) {
#if !RT_PLATFORM_WINDOWS
        fflush(stdout);
#endif
    }
}
