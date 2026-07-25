//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_stack_safety.c
// Purpose: Installs best-effort platform fatal-fault diagnostics for native
//          stack exhaustion and provides an unconditional stack-overflow
//          termination entry point.
//
// Key invariants:
//   - Process-wide signal/exception handlers are installed transactionally at
//     most once after success; failed registration is silent and retryable.
//   - POSIX alternate signal stacks are per-thread. Every caller preserves an
//     already-enabled stack or attempts to install a distinct thread-local
//     buffer before checking process-wide readiness.
//   - Signal/exception handlers write diagnostic messages using async-signal-
//     safe methods (write/WriteFile) rather than fprintf, which is unsafe in
//     low-stack conditions.
//   - Windows distinguishes EXCEPTION_STACK_OVERFLOW. POSIX cannot reliably
//     distinguish overflow from other SIGSEGV/SIGBUS faults and reports both.
//   - A handled fatal fault terminates immediately through ExitProcess/_exit;
//     the explicit trap uses the same termination policy.
//   - The POSIX fallback alternate stack is a thread-local buffer of size
//     SIGSTKSZ; no heap allocation is used for signal infrastructure.
//   - Unsupported-platform initialization is a no-op, but the explicit trap
//     still prints through stdio and exits with status 1.
//
// Ownership/Lifetime:
//   - Handler state is process-global; POSIX alternate-stack storage belongs to
//     each calling thread and remains valid for that thread's lifetime.
//   - Existing alternate stacks and existing non-default fatal-signal handlers
//     remain owned by the component that installed them.
//   - Installed handlers and Windows error-mode changes are retained until
//     process termination; this module provides no uninstall operation.
//
// Links: src/runtime/core/rt_stack_safety.h (public API),
//        src/runtime/rt_platform.h (platform and thread-local abstractions)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Best-effort native stack-overflow diagnostics and termination.

#include "rt_stack_safety.h"

#include "rt_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum {
    RT_STACK_SAFETY_UNINITIALIZED = 0,
    RT_STACK_SAFETY_INITIALIZING = 1,
    RT_STACK_SAFETY_READY = 2,
};

/// @brief Atomic publication state for the process-wide exception handler.
static int g_stack_safety_state = RT_STACK_SAFETY_UNINITIALIZED;

/// @brief Registration token retained for the lifetime of the process.
static PVOID g_stack_safety_handler = NULL;

/// @brief Dynamically resolved signature for `SetThreadStackGuarantee`.
typedef BOOL(WINAPI *rt_set_thread_stack_guarantee_fn)(PULONG);
/// @brief Dynamically resolved signature for `GetErrorMode`.
typedef UINT(WINAPI *rt_get_error_mode_fn)(void);

/// @brief Reserve enough stack for the current thread's overflow handler.
/// @details SetThreadStackGuarantee is thread-local, so this runs before the
///          process-wide ready-state fast path on every calling thread. The
///          API is resolved dynamically and absence or failure is ignored
///          because initialization has no status-return channel.
static void ensure_thread_stack_guarantee(void) {
    ULONG guarantee = 64U * 1024U;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    rt_set_thread_stack_guarantee_fn set_guarantee =
        kernel32 ? (rt_set_thread_stack_guarantee_fn)(void *)GetProcAddress(
                       kernel32, "SetThreadStackGuarantee")
                 : NULL;
    if (set_guarantee)
        (void)set_guarantee(&guarantee);
}

/// @brief Vectored exception handler for stack overflow detection.
/// @details For `EXCEPTION_STACK_OVERFLOW`, writes a fixed diagnostic directly
///          to the current standard-error OS handle and terminates with status
///          one. Invalid handles and failed writes do not prevent termination.
///          Every other exception continues through the handler chain.
/// @param ep Exception metadata supplied by Windows; may be `NULL`.
/// @return `EXCEPTION_CONTINUE_SEARCH` for non-overflow exceptions. The
///         overflow path does not return.
static LONG WINAPI stack_overflow_handler(EXCEPTION_POINTERS *ep) {
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        // Cannot safely use fprintf here as we're out of stack space.
        // Use WriteFile to stderr directly.
        HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
        static const char msg[] = "Zanna runtime error: stack overflow\n"
                                  "Hint: Reduce recursion depth or use iterative algorithms.\n"
                                  "      Consider using --stack-size=SIZE to increase stack.\n";
        DWORD written = 0;
        if (hStderr && hStderr != INVALID_HANDLE_VALUE)
            (void)WriteFile(hStderr, msg, (DWORD)(sizeof(msg) - 1U), &written, NULL);

        // Terminate immediately - cannot recover from stack overflow
        ExitProcess(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// @brief Install the Windows exception handler after winning initialization.
/// @details Registration occurs before global error-mode changes so a failed
///          `AddVectoredExceptionHandler` attempt leaves those modes untouched.
///          Success retains the registration token, suppresses critical-error
///          and GP-fault dialog boxes while preserving the current mode when
///          `GetErrorMode` is available (otherwise starting from zero), and
///          disables CRT abort messages/report-fault behavior.
/// @return Non-zero when the handler was registered and can be published.
static int install_stack_safety_handler(void) {
    PVOID handler = AddVectoredExceptionHandler(1, stack_overflow_handler);
    if (!handler)
        return 0;

    g_stack_safety_handler = handler;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    rt_get_error_mode_fn get_error_mode =
        kernel32 ? (rt_get_error_mode_fn)(void *)GetProcAddress(kernel32, "GetErrorMode") : NULL;
    UINT current_mode = get_error_mode ? get_error_mode() : 0;
    SetErrorMode(current_mode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    return 1;
}

/// @brief Initialize Windows stack-overflow handling for the calling thread.
/// @details Always attempts the thread-local stack guarantee first. One caller
///          then registers and publishes the process-wide vectored handler;
///          concurrent callers spin on the atomic state. Registration failure
///          resets the state for a later retry and returns silently.
void rt_init_stack_safety(void) {
    ensure_thread_stack_guarantee();
    for (;;) {
        int state = __atomic_load_n(&g_stack_safety_state, __ATOMIC_ACQUIRE);
        if (state == RT_STACK_SAFETY_READY)
            return;
        if (state == RT_STACK_SAFETY_UNINITIALIZED) {
            int expected = RT_STACK_SAFETY_UNINITIALIZED;
            if (__atomic_compare_exchange_n(&g_stack_safety_state,
                                            &expected,
                                            RT_STACK_SAFETY_INITIALIZING,
                                            0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                int ready = install_stack_safety_handler();
                __atomic_store_n(&g_stack_safety_state,
                                 ready ? RT_STACK_SAFETY_READY : RT_STACK_SAFETY_UNINITIALIZED,
                                 __ATOMIC_RELEASE);
                return;
            }
        }
    }
}

/// @brief Emit the explicit Windows stack-overflow trap and terminate.
/// @details Writes a fixed message with `WriteFile` when the standard-error
///          handle is usable, ignores write errors, and calls `ExitProcess(1)`.
/// @note This function does not return.
void rt_trap_stack_overflow(void) {
    // Use WriteFile for safety in low-stack conditions
    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
    static const char msg[] = "Zanna runtime trap: stack overflow\n";
    DWORD written = 0;
    if (hStderr && hStderr != INVALID_HANDLE_VALUE)
        (void)WriteFile(hStderr, msg, (DWORD)(sizeof(msg) - 1U), &written, NULL);
    ExitProcess(1);
}

#elif defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <string.h>
#include <unistd.h>

enum {
    RT_STACK_SAFETY_UNINITIALIZED = 0,
    RT_STACK_SAFETY_INITIALIZING = 1,
    RT_STACK_SAFETY_READY = 2,
};

/// @brief Naturally aligned per-thread storage used only when no stack exists.
/// @details The union remains alive for the entire thread lifetime and is large
///          enough for the platform's `SIGSTKSZ` requirement.
static RT_THREAD_LOCAL union {
    max_align_t alignment;
    unsigned char bytes[SIGSTKSZ];
} g_alt_stack;

/// @brief Atomic publication state for the process-wide signal handlers.
static int g_stack_safety_state = RT_STACK_SAFETY_UNINITIALIZED;

/// @brief Report a handled POSIX fatal memory signal and terminate.
/// @details Zanna cannot distinguish stack exhaustion from an arbitrary
///          segmentation or bus fault at this layer. For SIGSEGV or SIGBUS it
///          writes a fixed message with the async-signal-safe `write` syscall,
///          ignores write errors, and calls `_exit(1)`.
/// @param sig Delivered signal number.
/// @param info Signal metadata; intentionally unused.
/// @param context Machine context; intentionally unused.
static void sigsegv_handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    (void)context;

    if (sig == SIGSEGV || sig == SIGBUS) {
        static const char msg[] = "Zanna runtime error: stack overflow (or segmentation fault)\n"
                                  "Hint: Reduce recursion depth or use iterative algorithms.\n"
                                  "      Consider increasing stack limit with ulimit -s.\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
        _exit(1);
    }
}

/// @brief Ensure the calling POSIX thread has an alternate signal stack.
/// @details `sigaltstack` state is thread-specific. An existing enabled stack
///          is deliberately preserved because runtimes such as ASan own and
///          unmap the stack they install during thread teardown. When disabled,
///          this function installs the caller's distinct thread-local buffer.
/// @return Non-zero when an alternate stack was already enabled or the
///         thread-local replacement was installed; zero on query/install error.
static int ensure_thread_alt_stack(void) {
    stack_t current;
    if (sigaltstack(NULL, &current) != 0)
        return 0;
    if ((current.ss_flags & SS_DISABLE) == 0)
        return 1;

    stack_t replacement;
    memset(&replacement, 0, sizeof(replacement));
    replacement.ss_sp = g_alt_stack.bytes;
    replacement.ss_size = sizeof(g_alt_stack.bytes);
    return sigaltstack(&replacement, NULL) == 0;
}

/// @brief Test whether a fatal-signal disposition belongs to another runtime.
/// @details Zanna does not replace non-default handlers (including ignored
///          dispositions), preserving sanitizer, debugger, embedding-host, and
///          crash-reporter ownership.
/// @param action Non-null signal disposition returned by a query-only
///               `sigaction` call.
/// @return Non-zero when the disposition is not the platform default.
static int has_external_signal_handler(const struct sigaction *action) {
    return action->sa_handler != SIG_DFL;
}

/// @brief Install both process-wide POSIX fatal-signal handlers transactionally.
/// @details Queries both dispositions before mutation. If either signal has a
///          non-default or ignored disposition, Zanna defers completely and
///          reports a successful no-op, leaving the other signal untouched as
///          well. Otherwise it installs one SA_SIGINFO|SA_ONSTACK handler for
///          both. If SIGBUS installation fails after SIGSEGV succeeds, the
///          previous SIGSEGV disposition is restored.
/// @return Non-zero when the desired state is safe to publish as ready.
static int install_stack_safety_handlers(void) {
    struct sigaction previous_segv;
    struct sigaction previous_bus;
    if (sigaction(SIGSEGV, NULL, &previous_segv) != 0 ||
        sigaction(SIGBUS, NULL, &previous_bus) != 0) {
        return 0;
    }
    if (has_external_signal_handler(&previous_segv) || has_external_signal_handler(&previous_bus)) {
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0)
        return 0;
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        (void)sigaction(SIGSEGV, &previous_segv, NULL);
        return 0;
    }
    return 1;
}

/// @brief Initialize POSIX fatal-fault handling for the calling thread.
/// @details Attempts the per-thread alternate stack on every call, even after
///          process-wide readiness. Its failure is not surfaced and does not
///          block handler registration. One caller queries/installs or defers
///          the process signal handlers while concurrent callers spin on the
///          atomic state. Installation failure resets the state for retry.
void rt_init_stack_safety(void) {
    (void)ensure_thread_alt_stack();

    for (;;) {
        int state = __atomic_load_n(&g_stack_safety_state, __ATOMIC_ACQUIRE);
        if (state == RT_STACK_SAFETY_READY)
            return;
        if (state == RT_STACK_SAFETY_UNINITIALIZED) {
            int expected = RT_STACK_SAFETY_UNINITIALIZED;
            if (__atomic_compare_exchange_n(&g_stack_safety_state,
                                            &expected,
                                            RT_STACK_SAFETY_INITIALIZING,
                                            0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                int ready = install_stack_safety_handlers();
                __atomic_store_n(&g_stack_safety_state,
                                 ready ? RT_STACK_SAFETY_READY : RT_STACK_SAFETY_UNINITIALIZED,
                                 __ATOMIC_RELEASE);
                return;
            }
        }
    }
}

/// @brief Emit the explicit POSIX stack-overflow trap and terminate.
/// @details Writes a fixed diagnostic directly to standard error and calls
///          `_exit(1)`, bypassing stdio flushing and process-exit callbacks.
/// @note This function does not return.
void rt_trap_stack_overflow(void) {
    static const char msg[] = "Zanna runtime trap: stack overflow\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
    _exit(1);
}

#else
// Fallback for other platforms - no-op implementation
/// @brief Perform unsupported-platform stack-safety initialization.
/// @details No signal or exception handler is available, so this variant is a
///          no-op.
void rt_init_stack_safety(void) {
    // No-op on unsupported platforms
}

/// @brief Emit the fallback stack-overflow diagnostic and terminate.
/// @details Uses `fprintf` and `fflush` because no supported async fault-handler
///          environment is available, then calls `exit(1)`.
/// @note This function does not return.
void rt_trap_stack_overflow(void) {
    fprintf(stderr, "Zanna runtime trap: stack overflow\n");
    fflush(stderr);
    exit(1);
}
#endif
