//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_trap.h
// Purpose: Runtime trap entry points for unrecoverable IL/runtime faults,
// routing diagnostics through the active VM/runtime trap handler with
// structured trap metadata.
//
// Key invariants:
//   - Trap helpers dispatch through the active trap hook rather than hardwiring
//     stderr/exit behaviour.
//   - Tests may override vm_trap()/trap dispatch to observe failures without
//     terminating the process.
//   - rt_diag_assert accepts an int8_t condition; non-zero means the assertion passed.
//   - The ABI of these functions is stable; codegen backends depend on their signatures.
//
// Ownership/Lifetime:
//   - Message inputs are borrowed for the duration of dispatch/formatting.
//   - Recovery nodes and the latest diagnostic are thread-local; installing a
//     legacy recovery allocates a small node that clear/recovery later frees.
//   - Trap hooks may recover or return, while rt_abort always terminates.
//
// Links: src/runtime/core/rt_trap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Trap dispatch, legacy recovery, abort, and diagnostic assertions.
#pragma once

#include <setjmp.h>
#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Trap with the supplied message using the active runtime trap handler.
/// @details Runtime trap hooks may be overridden by tests and embedders, and an override is
///          allowed to return after recording the trap. Callers that cannot safely continue
///          after a trap must immediately return, jump, or otherwise stop their local control
///          flow after invoking this function.
/// @param msg Null-terminated trap message, or NULL for a generic trap.
void rt_trap(const char *msg);

/// @brief Install a legacy setjmp recovery target for the current thread.
/// @details Runtime code that must clean up around recoverable traps uses this
///          to catch the next @ref rt_trap in the current thread. Callers must
///          pair a successful install with @ref rt_trap_clear_recovery before
///          returning normally, and must also clear it from the recovery path
///          before re-raising or handling the trap.
///          Installation allocates a stack node and aborts on allocation
///          failure. Passing NULL clears consecutive topmost legacy frames
///          without removing a native recovery frame.
/// @param buf Borrowed jump buffer that remains live until recovery is cleared,
///        or NULL for legacy-stack cleanup.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Clear the current thread's top legacy trap recovery target.
/// @details Removes the most recent recovery frame installed by
///          @ref rt_trap_set_recovery. Safe cleanup paths call this before
///          releasing resources or re-raising the saved trap message.
void rt_trap_clear_recovery(void);

/// @brief Return the last trap message recorded on the current thread.
/// @details The returned pointer is owned by the runtime's thread-local trap
///          buffer and remains valid until the next trap on the same thread.
///          Callers that need to re-raise after cleanup should copy it first.
/// @return Last trap message, or an empty string if no message is available.
const char *rt_trap_get_error(void);

/// @brief Terminate the runtime immediately with a diagnostic message.
/// @details This is the non-recoverable companion to @ref rt_trap. Use it
///          only after conditions where continuing would create unsafe or
///          invalid runtime state even if an embedder's trap hook returns.
/// @param msg Null-terminated diagnostic message, or NULL for a generic trap.
/// @return This function does not return.
void rt_abort(const char *msg);

/// @brief Trap with a managed runtime string message.
/// @details Validates the string handle before reading it. Control bytes,
///          quotes, backslashes, and embedded NUL bytes are escaped so the
///          C-string trap dispatcher receives bounded, unambiguous diagnostic
///          text instead of a truncated or multi-line raw payload.
/// @param msg Runtime string message, or NULL/empty for a generic trap.
void rt_trap_string(rt_string msg);

/// @brief Traps the runtime on division by zero.
void rt_trap_div0(void);

/// @brief Traps the runtime on checked integer overflow.
void rt_trap_ovf(void);

/// @brief Trap when @p condition is false with the supplied diagnostic.
/// @param condition Non-zero when the assertion passes.
/// @param message Runtime string describing the assertion; falls back to a
///        default when empty.
void rt_diag_assert(int8_t condition, rt_string message);

/// @brief Assert two integers are equal.
/// @param expected The expected value.
/// @param actual The actual value.
/// @param message Description of what was being tested.
void rt_diag_assert_eq(int64_t expected, int64_t actual, rt_string message);

/// @brief Assert two integers are not equal.
/// @param a First value.
/// @param b Second value.
/// @param message Description of what was being tested.
void rt_diag_assert_neq(int64_t a, int64_t b, rt_string message);

/// @brief Assert two numbers are approximately equal.
/// @details Exact equality and two NaNs pass. Other values use absolute error
///          below `1e-9` when both magnitudes are below one and relative error
///          below `1e-9` otherwise.
/// @param expected The expected value.
/// @param actual The actual value.
/// @param message Description of what was being tested.
void rt_diag_assert_eq_num(double expected, double actual, rt_string message);

/// @brief Assert two strings are equal.
/// @details Compares stored bytes for valid handles; two null handles pass.
///          Invalid handles fail with bounded escaped diagnostics.
/// @param expected The expected string.
/// @param actual The actual string.
/// @param message Description of what was being tested.
void rt_diag_assert_eq_str(rt_string expected, rt_string actual, rt_string message);

/// @brief Assert an object reference is null.
/// @param obj The object to check.
/// @param message Description of what was being tested.
void rt_diag_assert_null(void *obj, rt_string message);

/// @brief Assert an object reference is not null.
/// @param obj The object to check.
/// @param message Description of what was being tested.
void rt_diag_assert_not_null(void *obj, rt_string message);

/// @brief Unconditionally fail with a message.
/// @param message Failure description.
void rt_diag_assert_fail(rt_string message);

/// @brief Assert first value is greater than second.
/// @param a First value.
/// @param b Second value.
/// @param message Description of what was being tested.
void rt_diag_assert_gt(int64_t a, int64_t b, rt_string message);

/// @brief Assert first value is less than second.
/// @param a First value.
/// @param b Second value.
/// @param message Description of what was being tested.
void rt_diag_assert_lt(int64_t a, int64_t b, rt_string message);

/// @brief Assert first value is greater than or equal to second.
/// @param a First value.
/// @param b Second value.
/// @param message Description of what was being tested.
void rt_diag_assert_gte(int64_t a, int64_t b, rt_string message);

/// @brief Assert first value is less than or equal to second.
/// @param a First value.
/// @param b Second value.
/// @param message Description of what was being tested.
void rt_diag_assert_lte(int64_t a, int64_t b, rt_string message);

#ifdef __cplusplus
}
#endif
