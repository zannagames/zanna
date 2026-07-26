//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_shutdown.h
// Purpose: Poll-based graceful shutdown requests for Zanna.System.Shutdown.
// Key invariants: Requests are process-wide bitmasks and are safe to publish
//                 from cooperative host/runtime code.
// Ownership/Lifetime: The module owns only atomic process state; no heap data.
// Links: docs/zannalib/system.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_shutdown.h
 * @brief Declares cooperative process-wide graceful-shutdown polling.
 * @details Callers atomically publish recognized reason bits, inspect or
 *          consume accumulated requests, clear public and VM interrupt state,
 *          and coordinate a one-shot polling indication without allocating
 *          heap storage.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Empty shutdown-reason mask.
#define RT_SHUTDOWN_REASON_NONE INT64_C(0)
/// @brief Cooperative interruption bit, such as Ctrl-C or SIGINT.
#define RT_SHUTDOWN_REASON_INTERRUPT INT64_C(1)
/// @brief Cooperative termination bit, such as SIGTERM or console close.
#define RT_SHUTDOWN_REASON_TERMINATE INT64_C(2)
/// @brief Mask of all recognized public shutdown-reason bits.
#define RT_SHUTDOWN_REASON_MASK (RT_SHUTDOWN_REASON_INTERRUPT | RT_SHUTDOWN_REASON_TERMINATE)

/// @brief Callback that consumes the VM's matching pending interrupt epoch.
typedef void (*rt_shutdown_clear_interrupt_fn)(void);

/// @brief Install the VM callback used to consume a pending interrupt epoch.
/// @details Atomically replaces the prior callback. rt_shutdown_poll() invokes
///          it after consuming a nonempty reason mask; rt_shutdown_clear()
///          invokes it whenever explicitly clearing state.
/// @param callback Callback to publish, or NULL to detach the current hook.
void rt_shutdown_set_interrupt_clear_callback(rt_shutdown_clear_interrupt_fn callback);

/// @brief Request cooperative graceful shutdown for one or more reason bits.
/// @details Atomically ORs recognized bits into process-wide state. Unknown bits
///          are ignored and concurrent requests accumulate until consumed.
/// @param reason Bitwise combination of RT_SHUTDOWN_REASON_* values.
void rt_shutdown_request(int64_t reason);

/// @brief Return and clear the current shutdown reason bitmask.
/// @details Arms the graceful-polling flag before consuming reasons. A nonempty
///          result disarms that flag and invokes the interrupt-clear callback.
/// @return All reasons pending at the atomic exchange, or
///         RT_SHUTDOWN_REASON_NONE.
int64_t rt_shutdown_poll(void);

/// @brief Return non-zero when any shutdown reason is pending.
/// @details Arms graceful polling but neither consumes reasons nor invokes the
///          interrupt-clear callback.
/// @return 1 when any reason is pending, otherwise 0.
int8_t rt_shutdown_pending(void);

/// @brief Clear pending shutdown reasons and the active VM interrupt epoch.
/// @details Also disarms graceful polling and invokes the installed callback
///          even when no public reason was pending.
void rt_shutdown_clear(void);

/// @brief Clear pending shutdown reasons without invoking the VM callback.
/// @details Leaves the one-shot graceful-polling flag unchanged.
void rt_shutdown_clear_pending_only(void);

/// @brief Consume and return the one-shot graceful-polling arm flag.
/// @return 1 when Poll/Pending armed the flag since its last consume or explicit
///         disarm, otherwise 0.
int8_t rt_shutdown_polling_enabled(void);

/// @brief Non-enabling internal peek used by the VM interrupt path.
/// @return 1 when any reason is pending without modifying state, otherwise 0.
int8_t rt_shutdown_has_pending(void);

/// @brief Install OS signal/console handlers that publish shutdown requests.
/// @details Wires SIGINT/SIGTERM (POSIX) or the console control handler
///          (Windows) so an OS interrupt/termination becomes a
///          `rt_shutdown_request`, letting `Shutdown.Poll` observe Ctrl-C and
///          termination. Idempotent. The VM installs its own richer handler
///          automatically; native/AOT programs call this to opt in to the same
///          OS integration (they do not get it for free, since the compiler and
///          tools also link the runtime and must keep default signal behavior)
///          (VDOC-210). Exposed as `Zanna.System.Shutdown.InstallSignalHandlers`.
///          Registration is best-effort and the API exposes no failure result.
void rt_shutdown_install_signal_handlers(void);

/// @brief Get the runtime class property's no-reason value.
/// @return RT_SHUTDOWN_REASON_NONE.
int64_t rt_shutdown_const_none(void);

/// @brief Get the runtime class property's interrupt-reason value.
/// @return RT_SHUTDOWN_REASON_INTERRUPT.
int64_t rt_shutdown_const_interrupt(void);

/// @brief Get the runtime class property's termination-reason value.
/// @return RT_SHUTDOWN_REASON_TERMINATE.
int64_t rt_shutdown_const_terminate(void);

#ifdef __cplusplus
}
#endif
