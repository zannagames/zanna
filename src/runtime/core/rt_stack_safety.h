//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_stack_safety.h
// Purpose: Declares best-effort native stack/fatal-fault handler setup and an
// unconditional diagnostic termination entry point.
//
// Key invariants:
//   - Successful process-handler initialization is idempotent and concurrent;
//     failed setup is silent and may be retried.
//   - On Unix, every calling thread preserves an existing alternate stack or
//     attempts to install its own SIGSTKSZ-byte fallback stack.
//   - POSIX setup defers entirely if either SIGSEGV or SIGBUS already has a
//     non-default owner; otherwise both signals use one alternate-stack handler.
//   - Windows uses vectored exception handling for EXCEPTION_STACK_OVERFLOW and
//     requests a per-thread emergency stack guarantee when the API is available.
//   - rt_trap_stack_overflow always emits a best-effort diagnostic and
//     terminates with status 1.
//
// Ownership/Lifetime:
//   - Handler registration has process-wide effect and is managed internally.
//   - Fallback alternate-stack storage is thread-local; callers never free it.
//   - Pre-existing alternate stacks and handlers retain their original ownership.
//   - No unregister/reset operation is exposed; successful setup lasts until
//     process or thread teardown as appropriate.
//
// Links: src/runtime/core/rt_stack_safety.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Native stack-overflow diagnostic initialization and termination API.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initialize stack safety handlers.
/// @details On Windows, every call best-effort requests a current-thread stack
///          guarantee and successful first use registers a process-wide
///          vectored exception handler. On POSIX, every caller preserves or
///          best-effort installs its thread-local alternate stack; successful
///          first use installs both SIGSEGV and SIGBUS handlers only when
///          neither disposition has another owner. Unsupported-platform setup
///          is a no-op. Setup errors are not returned and remain retryable.
/// @note POSIX reports all handled SIGSEGV/SIGBUS faults as possible stack
///       overflow because this layer cannot reliably classify the fault.
void rt_init_stack_safety(void);

/// @brief Report a stack overflow trap and terminate the process.
/// @details Performs a best-effort low-stack-safe standard-error write and
///          terminates immediately with status 1 on Windows/POSIX. The generic
///          fallback uses stdio followed by `exit(1)`. The function never
///          dispatches a recoverable runtime trap and never returns.
void rt_trap_stack_overflow(void);

#ifdef __cplusplus
}
#endif
