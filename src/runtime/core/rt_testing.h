//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_testing.h
/// @file
/// @brief Declares the GC-managed non-fatal assertion suite and golden checker.
///
// Purpose: A test harness whose assertions RECORD rather than trap. Zanna
// already ships `Zanna.Core.Diagnostics.Assert*`, but every one of those calls
// rt_trap() and aborts on the first failure, which is the opposite of what a
// test program needs: run every check, report all failures, emit one parseable
// verdict line, and set a process exit code a shell runner can branch on.
//
// Key invariants:
//   - No assertion ever traps. A failing check increments a counter, prints a
//     "FAIL: " line, and returns false so the caller may early-out if it wants.
//   - Every Eq*/Check entry point returns the boolean outcome.
//   - Report() emits exactly one line starting "RESULT: " and is the only
//     output a runner needs to grep.
//   - A suite in baseline mode (a golden check ran with no recorded digest)
//     reports "RESULT: baseline" rather than ok or fail, so a first run cannot
//     be mistaken for a pass.
//
// Ownership/Lifetime:
//   - Suite objects are heap-allocated runtime objects managed through Zanna's
//     reference-counting/GC lifetime; callers do not free them explicitly.
//   - The suite name is retained for the object's lifetime and released by the
//     finalizer.
//
// Links: src/runtime/core/rt_testing.c (implementation),
//        src/runtime/core/rt_trap.c (the trapping Diagnostics.Assert* family),
//        docs/adr/0251-zanna-testing-suite.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for Testing.Suite instances.
/// @details Stamped by rt_obj_new_i64 at construction and verified by the
///          receiver guard so a handle of another class traps instead of being
///          reinterpreted as a Suite payload.
#define RT_TESTING_SUITE_CLASS_ID INT64_C(-0x430901)

/// @brief Create a suite that accumulates check results under @p name.
/// @param name Suite label echoed in the verdict line; NULL becomes empty.
/// @return New GC-managed Suite handle, or NULL on allocation failure.
void *rt_testing_suite_new(rt_string name);

/// @brief Record a boolean check.
/// @details Failure prints `FAIL: <msg>` and increments the failure count.
/// @param obj Suite receiver.
/// @param cond Condition under test.
/// @param msg Description printed on failure.
/// @return Non-zero when @p cond held, so callers can early-out.
int8_t rt_testing_check(void *obj, int8_t cond, rt_string msg);

/// @brief Record an integer equality check, printing both values on failure.
/// @param obj Suite receiver.
/// @param actual Observed value.
/// @param expected Required value.
/// @param msg Description printed on failure.
/// @return Non-zero when the values matched.
int8_t rt_testing_eq_int(void *obj, int64_t actual, int64_t expected, rt_string msg);

/// @brief Record a string equality check, printing both values on failure.
/// @param obj Suite receiver.
/// @param actual Observed value.
/// @param expected Required value.
/// @param msg Description printed on failure.
/// @return Non-zero when the values matched.
int8_t rt_testing_eq_str(void *obj, rt_string actual, rt_string expected, rt_string msg);

/// @brief Record a float equality check within an explicit tolerance.
/// @details The tolerance is a required argument rather than a hidden constant:
///          a simulation tolerance and a rendering tolerance are not the same
///          number, and silently picking one for the caller hides real drift.
/// @param obj Suite receiver.
/// @param actual Observed value.
/// @param expected Required value.
/// @param epsilon Maximum permitted absolute difference; negative is treated as
///        zero. A NaN on either side always fails.
/// @param msg Description printed on failure.
/// @return Non-zero when the values matched within @p epsilon.
int8_t rt_testing_eq_num(
    void *obj, double actual, double expected, double epsilon, rt_string msg);

/// @brief Record an inclusive-range check.
/// @details The shape a statistical calibration gate needs: assert that a
///          measured rate lands inside a band rather than on an exact value.
/// @param obj Suite receiver.
/// @param value Observed value.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param msg Description printed on failure.
/// @return Non-zero when @p value fell inside the band.
int8_t rt_testing_in_band(void *obj, double value, double lo, double hi, rt_string msg);

/// @brief Record an unconditional failure.
/// @param obj Suite receiver.
/// @param msg Description printed immediately.
void rt_testing_fail(void *obj, rt_string msg);

/// @brief Print a non-assertion note without affecting the verdict.
/// @param obj Suite receiver.
/// @param msg Text to print, indented to distinguish it from failures.
void rt_testing_note(void *obj, rt_string msg);

/// @brief Number of failed checks so far.
/// @param obj Suite receiver.
/// @return Failure count, or zero for an invalid receiver.
int64_t rt_testing_failures(void *obj);

/// @brief Number of checks recorded so far.
/// @param obj Suite receiver.
/// @return Total check count, or zero for an invalid receiver.
int64_t rt_testing_checks(void *obj);

/// @brief The suite's label.
/// @param obj Suite receiver.
/// @return Borrowed name string, or the empty string.
rt_string rt_testing_name(void *obj);

/// @brief Whether the suite has recorded no failures and is not in baseline mode.
/// @param obj Suite receiver.
/// @return Non-zero when a Report() now would say ok.
int8_t rt_testing_passed(void *obj);

/// @brief Emit the single verdict line and return the pass state.
/// @details Prints exactly one of:
///          - `RESULT: baseline (<name>: ...)` when a golden check ran with no
///            recorded digest — a first run must not read as a pass;
///          - `RESULT: ok (<name>: N checks)`;
///          - `RESULT: fail (<name>: M of N checks failed)`.
/// @param obj Suite receiver.
/// @return Non-zero when the verdict was ok.
int8_t rt_testing_report(void *obj);

/// @brief Process exit code matching the current verdict.
/// @param obj Suite receiver.
/// @return `0` when passing, `1` when failing, `2` in baseline mode.
int64_t rt_testing_exit_code(void *obj);

/// @brief SHA-256 of @p text as lowercase hex — the golden digest form.
/// @param text Content to digest; NULL is treated as empty.
/// @return Owned hex digest string.
rt_string rt_testing_golden_digest(rt_string text);

/// @brief Compare @p actual against a recorded digest, recording the outcome.
/// @details Always notes the computed digest so a moved baseline can be
///          re-pinned by copying the printed value. An empty or `"UNSET"`
///          @p expected puts the suite into baseline mode instead of failing,
///          which is what a first run needs.
/// @param obj Suite receiver.
/// @param label Name of the golden being checked.
/// @param actual Content whose digest is compared.
/// @param expected Recorded digest, `""`, or `"UNSET"`.
/// @return Non-zero when the digest matched or the suite entered baseline mode.
int8_t rt_testing_golden_check(void *obj, rt_string label, rt_string actual, rt_string expected);

#ifdef __cplusplus
}
#endif
