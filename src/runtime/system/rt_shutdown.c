//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_shutdown.c
// Purpose: Implements Zanna.System.Shutdown's poll-based graceful shutdown
//          request bitmask.
// Key invariants: Polling is cooperative and signal-safe work is kept in the
//                 VM/platform layer; this module only stores ordinary atomic
//                 process state.
// Ownership/Lifetime: No heap ownership; state lasts for the process lifetime.
// Links: src/runtime/system/rt_shutdown.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_shutdown.c
 * @brief Implements process-wide cooperative shutdown request state.
 * @details Atomic reason bits accumulate interrupt and termination requests
 *          until polling or explicit clearing consumes them. A one-shot poll
 *          arm flag and optional VM callback synchronize public shutdown state
 *          with the active interpreter interrupt epoch.
 */

#include "rt_shutdown.h"
#include "rt_platform.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

/// @brief Process-wide OR-ed set of pending public shutdown reason bits.
static atomic_llong g_shutdown_pending;
/// @brief One-shot indication that cooperative polling has been armed.
static atomic_bool g_shutdown_polling_enabled;
/// @brief Optional VM hook used to consume its corresponding interrupt epoch.
static _Atomic(rt_shutdown_clear_interrupt_fn) g_clear_interrupt_callback;

/// @brief Arm the one-shot indication that graceful polling is active.
static void mark_polling_enabled(void) {
    atomic_store_explicit(&g_shutdown_polling_enabled, true, memory_order_release);
}

/// @brief Publish the VM callback used to consume an interrupt epoch.
/// @details The callback pointer is stored atomically and may be replaced or
///          cleared. Calls that load it acquire the preceding publication.
/// @param callback Function to invoke after consuming/clearing shutdown state,
///        or NULL to detach the hook.
void rt_shutdown_set_interrupt_clear_callback(rt_shutdown_clear_interrupt_fn callback) {
    atomic_store_explicit(&g_clear_interrupt_callback, callback, memory_order_release);
}

/// @brief Atomically add recognized reasons to the pending shutdown mask.
/// @details Unknown bits are discarded. Multiple requests accumulate until a
///          poll or clear consumes them; requesting no recognized bit is a no-op.
/// @param reason Bitwise combination of RT_SHUTDOWN_REASON_* values.
void rt_shutdown_request(int64_t reason) {
    const int64_t mask = reason & RT_SHUTDOWN_REASON_MASK;
    if (mask == RT_SHUTDOWN_REASON_NONE)
        return;
    atomic_fetch_or_explicit(&g_shutdown_pending, (long long)mask, memory_order_acq_rel);
}

/// @brief Arm polling, atomically consume pending reasons, and sync VM state.
/// @details A nonempty result disarms the polling flag and invokes the installed
///          interrupt-clear callback once after the atomic exchange.
/// @return Bitwise combination of all reasons pending at the exchange, or
///         RT_SHUTDOWN_REASON_NONE.
int64_t rt_shutdown_poll(void) {
    mark_polling_enabled();
    const int64_t mask =
        (int64_t)atomic_exchange_explicit(&g_shutdown_pending, 0, memory_order_acq_rel);
    if (mask != RT_SHUTDOWN_REASON_NONE) {
        atomic_store_explicit(&g_shutdown_polling_enabled, false, memory_order_release);
        rt_shutdown_clear_interrupt_fn callback =
            atomic_load_explicit(&g_clear_interrupt_callback, memory_order_acquire);
        if (callback)
            callback();
    }
    return mask;
}

/// @brief Arm graceful polling and test for any pending reason.
/// @details Unlike rt_shutdown_poll(), this operation does not consume reasons
///          or invoke the VM callback.
/// @return 1 when at least one recognized reason is pending, otherwise 0.
int8_t rt_shutdown_pending(void) {
    mark_polling_enabled();
    return atomic_load_explicit(&g_shutdown_pending, memory_order_acquire) != 0 ? 1 : 0;
}

/// @brief Clear all pending reason bits without changing other state.
/// @details Does not disarm the one-shot polling flag and does not invoke the
///          interrupt-clear callback.
void rt_shutdown_clear_pending_only(void) {
    atomic_store_explicit(&g_shutdown_pending, 0, memory_order_release);
}

/// @brief Clear pending reasons, disarm polling, and sync VM interrupt state.
/// @details Invokes the currently installed interrupt-clear callback even when
///          no public shutdown reason was pending.
void rt_shutdown_clear(void) {
    rt_shutdown_clear_pending_only();
    atomic_store_explicit(&g_shutdown_polling_enabled, false, memory_order_release);
    rt_shutdown_clear_interrupt_fn callback =
        atomic_load_explicit(&g_clear_interrupt_callback, memory_order_acquire);
    if (callback)
        callback();
}

/// @brief Consume the one-shot indication that graceful polling was armed.
/// @return 1 when a prior Poll/Pending operation had armed the flag since its
///         last consume or nonempty poll/clear, otherwise 0.
int8_t rt_shutdown_polling_enabled(void) {
    return atomic_exchange_explicit(&g_shutdown_polling_enabled, false, memory_order_acq_rel) ? 1
                                                                                              : 0;
}

/// @brief Peek at pending reasons without arming or consuming polling state.
/// @return 1 when any recognized reason bit is pending, otherwise 0.
int8_t rt_shutdown_has_pending(void) {
    return atomic_load_explicit(&g_shutdown_pending, memory_order_acquire) != 0 ? 1 : 0;
}

#if RT_PLATFORM_WINDOWS
/// @brief Translate a Windows console-control event into a shutdown reason.
/// @param ctrl_type Console event code supplied by Windows.
/// @return TRUE after recording the event as handled.
static BOOL WINAPI shutdown_win_ctrl_handler(DWORD ctrl_type) {
    int64_t reason = RT_SHUTDOWN_REASON_INTERRUPT;
    if (ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_LOGOFF_EVENT ||
        ctrl_type == CTRL_SHUTDOWN_EVENT) {
        reason = RT_SHUTDOWN_REASON_TERMINATE;
    }
    rt_shutdown_request(reason);
    return TRUE;
}
#else
// rt_shutdown_request only performs a lock-free atomic OR, so it is
// async-signal-safe to call directly from these handlers.
/// @brief Publish an interrupt reason for POSIX SIGINT.
/// @param sig Delivered signal number; ignored.
static void shutdown_posix_sigint(int sig) {
    (void)sig;
    rt_shutdown_request(RT_SHUTDOWN_REASON_INTERRUPT);
}

/// @brief Publish a termination reason for POSIX SIGTERM.
/// @param sig Delivered signal number; ignored.
static void shutdown_posix_sigterm(int sig) {
    (void)sig;
    rt_shutdown_request(RT_SHUTDOWN_REASON_TERMINATE);
}
#endif

/// @brief Install shutdown-producing OS handlers at most once.
/// @details Windows registers a console-control callback. POSIX installs
///          SA_RESTART handlers for SIGINT and SIGTERM. The one-time atomic gate
///          makes concurrent/repeated calls no-ops after the first attempt.
void rt_shutdown_install_signal_handlers(void) {
    static atomic_bool installed;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &installed, &expected, true, memory_order_acq_rel, memory_order_acquire)) {
        return; // already installed
    }
#if RT_PLATFORM_WINDOWS
    SetConsoleCtrlHandler(shutdown_win_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_posix_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // resume interrupted syscalls where possible
    sigaction(SIGINT, &sa, NULL);

    struct sigaction ta;
    memset(&ta, 0, sizeof(ta));
    ta.sa_handler = shutdown_posix_sigterm;
    sigemptyset(&ta.sa_mask);
    ta.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &ta, NULL);
#endif
}

/// @brief Return the public no-reason constant.
/// @return RT_SHUTDOWN_REASON_NONE.
int64_t rt_shutdown_const_none(void) {
    return RT_SHUTDOWN_REASON_NONE;
}

/// @brief Return the public interrupt-reason bit.
/// @return RT_SHUTDOWN_REASON_INTERRUPT.
int64_t rt_shutdown_const_interrupt(void) {
    return RT_SHUTDOWN_REASON_INTERRUPT;
}

/// @brief Return the public termination-reason bit.
/// @return RT_SHUTDOWN_REASON_TERMINATE.
int64_t rt_shutdown_const_terminate(void) {
    return RT_SHUTDOWN_REASON_TERMINATE;
}
