//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/rt_win32_wait.h
// Purpose: Shared helpers for exact Win32 waits, including finite deadline
//          slicing and checked thread-handle joins.
//
// Key invariants:
//   - A finite timeout never produces INFINITE (0xFFFFFFFF).
//   - Long waits are split into finite slices and retain their absolute deadline.
//   - Deadline addition saturates instead of wrapping GetTickCount64 values.
//   - A thread join publishes success only after an exact signaled wait and close.
//
// Ownership/Lifetime:
//   - Deadline helpers are pure and allocate no state.
//   - A non-null handle passed to the join helper is consumed on every
//     observable success or failure path.
//
// Links: src/runtime/threads/rt_future.c,
//        src/runtime/threads/rt_monitor_win.c,
//        src/runtime/threads/rt_threads_win.c,
//        src/runtime/threads/rt_concqueue.c,
//        src/runtime/io/rt_watcher.c,
//        src/runtime/network/rt_http_server.c,
//        src/runtime/network/rt_https_server.c,
//        src/runtime/network/rt_ws_server.c,
//        src/runtime/network/rt_wss_server.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_platform.h"

#if RT_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits.h>
#include <stdint.h>

#define RT_WIN32_MAX_FINITE_WAIT_MS ((DWORD)(INFINITE - 1u))

typedef enum rt_win32_thread_join_result {
    RT_WIN32_THREAD_JOIN_FAILED = 0,
    RT_WIN32_THREAD_JOINED = 1,
    RT_WIN32_THREAD_JOIN_CURRENT = 2
} rt_win32_thread_join_result;

/// @brief Compute a saturating absolute deadline from an explicit tick value.
/// @param now Current GetTickCount64 value.
/// @param timeout_ms Relative timeout in milliseconds; non-positive means now.
/// @return Absolute deadline, saturated at the ULONGLONG maximum.
static inline ULONGLONG rt_win32_deadline_after_ms(ULONGLONG now, int64_t timeout_ms) {
    ULONGLONG add = timeout_ms > 0 ? (ULONGLONG)timeout_ms : 0;
    return (ULLONG_MAX - now < add) ? ULLONG_MAX : now + add;
}

/// @brief Compute a deadline using the current monotonic Win32 tick counter.
static inline ULONGLONG rt_win32_deadline_from_now_ms(int64_t timeout_ms) {
    return rt_win32_deadline_after_ms(GetTickCount64(), timeout_ms);
}

/// @brief Convert an absolute deadline into one legal finite Win32 wait slice.
/// @details INFINITE is reserved by Win32, so deltas at or above that value are
///          capped one millisecond lower and the caller recomputes after waking.
/// @param now Current GetTickCount64 value.
/// @param deadline Absolute deadline in the same tick domain.
/// @return 0 when expired, otherwise a value in [1, 0xFFFFFFFE].
static inline DWORD rt_win32_wait_slice_at(ULONGLONG now, ULONGLONG deadline) {
    ULONGLONG delta;
    if (deadline <= now)
        return 0;
    delta = deadline - now;
    return delta > (ULONGLONG)RT_WIN32_MAX_FINITE_WAIT_MS ? RT_WIN32_MAX_FINITE_WAIT_MS
                                                          : (DWORD)delta;
}

/// @brief Compute one legal finite wait slice from the current tick value.
static inline DWORD rt_win32_wait_slice_until(ULONGLONG deadline) {
    return rt_win32_wait_slice_at(GetTickCount64(), deadline);
}

/// @brief Wait for and close a real Win32 thread handle with exact status checks.
/// @details A handle for the calling thread cannot be waited here without
///          deadlocking; it is closed and reported distinctly so lifecycle
///          code can follow its documented self-stop policy. Every other
///          handle must identify a thread, signal successfully, and close.
/// @param thread Owned real thread handle.
/// @return A joined/current result on success, or failed with LastError set.
static inline rt_win32_thread_join_result rt_win32_join_thread_handle(HANDLE thread) {
    if (!thread) {
        SetLastError(ERROR_INVALID_HANDLE);
        return RT_WIN32_THREAD_JOIN_FAILED;
    }
    const DWORD thread_id = GetThreadId(thread);
    if (thread_id == 0) {
        const DWORD error = GetLastError();
        (void)CloseHandle(thread);
        SetLastError(error == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : error);
        return RT_WIN32_THREAD_JOIN_FAILED;
    }
    if (thread_id == GetCurrentThreadId()) {
        return CloseHandle(thread) ? RT_WIN32_THREAD_JOIN_CURRENT : RT_WIN32_THREAD_JOIN_FAILED;
    }
    const DWORD wait = WaitForSingleObject(thread, INFINITE);
    if (wait != WAIT_OBJECT_0) {
        DWORD error = GetLastError();
        if (wait != WAIT_FAILED)
            error = ERROR_INVALID_FUNCTION;
        (void)CloseHandle(thread);
        SetLastError(error == ERROR_SUCCESS ? ERROR_INVALID_FUNCTION : error);
        return RT_WIN32_THREAD_JOIN_FAILED;
    }
    return CloseHandle(thread) ? RT_WIN32_THREAD_JOINED : RT_WIN32_THREAD_JOIN_FAILED;
}

#endif // RT_PLATFORM_WINDOWS
