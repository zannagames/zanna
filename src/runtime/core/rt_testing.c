//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_testing.c
/// @file
/// @brief Implements the non-fatal assertion suite and golden-digest checker.
///
// Purpose: See rt_testing.h. The short version: `Zanna.Core.Diagnostics.Assert*`
// traps on first failure, so a program using it reports one problem and dies.
// A test program needs the opposite — run everything, print every failure, then
// emit one machine-readable verdict.
//
// Key invariants:
//   - No entry point traps on a failed assertion; only an invalid receiver
//     traps, and that is a programmer error, not a test outcome.
//   - Output goes through rt_term_say so it lands on stdout alongside the
//     program's own output, in call order.
//   - Report() prints exactly one "RESULT: " line.
//
// Ownership/Lifetime:
//   - The suite name is copied into a fixed inline buffer at construction, so
//     the object owns no heap references and needs no finalizer.
//
// Links: src/runtime/core/rt_testing.h, docs/adr/0251-zanna-testing-suite.md
//
//===----------------------------------------------------------------------===//

#include "rt_testing.h"

#include "rt.hpp"
#include "rt_hash.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_trap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/// @brief Longest suite label retained, including the terminator.
#define RT_TESTING_NAME_MAX 96u

/// @brief Scratch size for one formatted output line before it is emitted.
#define RT_TESTING_LINE_MAX 1024u

/// @brief Mutable state owned by a Testing.Suite runtime object.
struct rt_testing_impl {
    int64_t checks;              ///< Total checks recorded.
    int64_t failures;            ///< Checks that failed.
    int64_t baselines;           ///< Golden checks that had no recorded digest.
    char name[RT_TESTING_NAME_MAX]; ///< Suite label, NUL-terminated.
};

/// @brief Safe-cast a handle to the Suite impl, trapping on a class mismatch.
/// @param obj Borrowed candidate Suite handle.
/// @param api Trap message identifying the calling API.
/// @return Typed pointer, or NULL after trapping.
static struct rt_testing_impl *suite_checked(void *obj, const char *api) {
    if (!rt_obj_is_instance(obj, RT_TESTING_SUITE_CLASS_ID, sizeof(struct rt_testing_impl))) {
        rt_trap(api);
        return NULL;
    }
    return (struct rt_testing_impl *)obj;
}

/// @brief Borrow a runtime string's bytes, mapping NULL to the empty string.
/// @param s Borrowed runtime string, or NULL.
/// @return Always a valid NUL-terminated pointer.
static const char *cstr_or_empty(rt_string s) {
    const char *p = s ? rt_string_cstr(s) : NULL;
    return p ? p : "";
}

/// @brief Emit one already-formatted line through the terminal writer.
/// @param text NUL-terminated line without its newline.
static void say_line(const char *text) {
    rt_string s = rt_string_from_bytes(text, strlen(text));
    rt_term_say(s);
    rt_string_unref(s);
}

/// @brief Record one outcome, printing `FAIL: <msg>` when it did not hold.
/// @param st Validated suite.
/// @param ok Non-zero when the check held.
/// @param msg Description printed on failure.
/// @param detail Optional extra text appended in parentheses; may be NULL.
/// @return @p ok, so callers can return it directly.
static int8_t record(struct rt_testing_impl *st, int ok, rt_string msg, const char *detail) {
    st->checks++;
    if (ok)
        return 1;
    st->failures++;
    char line[RT_TESTING_LINE_MAX];
    if (detail && detail[0])
        snprintf(line, sizeof(line), "FAIL: %s (%s)", cstr_or_empty(msg), detail);
    else
        snprintf(line, sizeof(line), "FAIL: %s", cstr_or_empty(msg));
    say_line(line);
    return 0;
}

void *rt_testing_suite_new(rt_string name) {
    struct rt_testing_impl *st = (struct rt_testing_impl *)rt_obj_new_i64(
        RT_TESTING_SUITE_CLASS_ID, (int64_t)sizeof(struct rt_testing_impl));
    if (!st) {
        rt_trap("Testing.Suite.New: memory allocation failed");
        return NULL;
    }
    st->checks = 0;
    st->failures = 0;
    st->baselines = 0;
    const char *src = cstr_or_empty(name);
    size_t len = strlen(src);
    if (len >= RT_TESTING_NAME_MAX)
        len = RT_TESTING_NAME_MAX - 1u;
    memcpy(st->name, src, len);
    st->name[len] = '\0';
    return st;
}

int8_t rt_testing_check(void *obj, int8_t cond, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Check: expected a Suite");
    if (!st)
        return 0;
    return record(st, cond != 0, msg, NULL);
}

int8_t rt_testing_eq_int(void *obj, int64_t actual, int64_t expected, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.EqInt: expected a Suite");
    if (!st)
        return 0;
    if (actual == expected)
        return record(st, 1, msg, NULL);
    char detail[128];
    snprintf(detail, sizeof(detail), "expected %lld, got %lld", (long long)expected,
             (long long)actual);
    return record(st, 0, msg, detail);
}

int8_t rt_testing_eq_str(void *obj, rt_string actual, rt_string expected, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.EqStr: expected a Suite");
    if (!st)
        return 0;
    const char *a = cstr_or_empty(actual);
    const char *e = cstr_or_empty(expected);
    if (strcmp(a, e) == 0)
        return record(st, 1, msg, NULL);
    char detail[RT_TESTING_LINE_MAX];
    snprintf(detail, sizeof(detail), "expected '%s', got '%s'", e, a);
    return record(st, 0, msg, detail);
}

int8_t rt_testing_eq_num(
    void *obj, double actual, double expected, double epsilon, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.EqNum: expected a Suite");
    if (!st)
        return 0;
    if (epsilon < 0.0)
        epsilon = 0.0;
    // A NaN on either side must fail rather than silently compare equal-ish.
    int ok = isfinite(actual) && isfinite(expected) && fabs(actual - expected) <= epsilon;
    if (ok)
        return record(st, 1, msg, NULL);
    char detail[192];
    snprintf(detail, sizeof(detail), "expected %.17g +/- %.17g, got %.17g", expected, epsilon,
             actual);
    return record(st, 0, msg, detail);
}

int8_t rt_testing_in_band(void *obj, double value, double lo, double hi, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.InBand: expected a Suite");
    if (!st)
        return 0;
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    int ok = isfinite(value) && value >= lo && value <= hi;
    if (ok)
        return record(st, 1, msg, NULL);
    char detail[192];
    snprintf(detail, sizeof(detail), "expected %.17g..%.17g, got %.17g", lo, hi, value);
    return record(st, 0, msg, detail);
}

void rt_testing_fail(void *obj, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Fail: expected a Suite");
    if (!st)
        return;
    (void)record(st, 0, msg, NULL);
}

void rt_testing_note(void *obj, rt_string msg) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Note: expected a Suite");
    if (!st)
        return;
    char line[RT_TESTING_LINE_MAX];
    snprintf(line, sizeof(line), "    %s", cstr_or_empty(msg));
    say_line(line);
}

int64_t rt_testing_failures(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Failures: expected a Suite");
    return st ? st->failures : 0;
}

int64_t rt_testing_checks(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Checks: expected a Suite");
    return st ? st->checks : 0;
}

rt_string rt_testing_name(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Name: expected a Suite");
    if (!st)
        return rt_empty_string();
    return rt_string_from_bytes(st->name, strlen(st->name));
}

int8_t rt_testing_passed(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Passed: expected a Suite");
    if (!st)
        return 0;
    return (st->failures == 0 && st->baselines == 0) ? 1 : 0;
}

int8_t rt_testing_report(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.Report: expected a Suite");
    if (!st)
        return 0;
    char line[RT_TESTING_LINE_MAX];
    if (st->baselines > 0) {
        // A first run must not read as a pass: there was nothing to compare
        // against, so the operator has to record the printed digests.
        snprintf(line, sizeof(line),
                 "RESULT: baseline (%s: %lld golden%s unrecorded — pin the digests above)",
                 st->name, (long long)st->baselines, st->baselines == 1 ? "" : "s");
        say_line(line);
        return 0;
    }
    if (st->failures == 0) {
        snprintf(line, sizeof(line), "RESULT: ok (%s: %lld check%s)", st->name,
                 (long long)st->checks, st->checks == 1 ? "" : "s");
        say_line(line);
        return 1;
    }
    snprintf(line, sizeof(line), "RESULT: fail (%s: %lld of %lld check%s failed)", st->name,
             (long long)st->failures, (long long)st->checks, st->checks == 1 ? "" : "s");
    say_line(line);
    return 0;
}

int64_t rt_testing_exit_code(void *obj) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Suite.ExitCode: expected a Suite");
    if (!st)
        return 1;
    if (st->baselines > 0)
        return 2;
    return st->failures == 0 ? 0 : 1;
}

rt_string rt_testing_golden_digest(rt_string text) {
    return rt_hash_sha256(text);
}

/// @brief True when @p expected carries no recorded digest.
/// @details Both the empty string and the literal `"UNSET"` are accepted so a
///          fixture constant can be written either way.
/// @param expected Candidate digest text.
/// @return Non-zero when the golden has never been pinned.
static int golden_unrecorded(const char *expected) {
    return expected[0] == '\0' || strcmp(expected, "UNSET") == 0;
}

int8_t rt_testing_golden_check(void *obj, rt_string label, rt_string actual, rt_string expected) {
    struct rt_testing_impl *st = suite_checked(obj, "Testing.Golden.Check: expected a Suite");
    if (!st)
        return 0;

    rt_string digest = rt_hash_sha256(actual);
    const char *dg = cstr_or_empty(digest);
    const char *lb = cstr_or_empty(label);
    const char *ex = cstr_or_empty(expected);

    // Always print the computed digest: re-pinning a moved golden is a
    // copy-paste, not a re-derivation.
    char line[RT_TESTING_LINE_MAX];
    snprintf(line, sizeof(line), "    %s sha256: %s", lb, dg);
    say_line(line);

    int8_t result;
    if (golden_unrecorded(ex)) {
        st->baselines++;
        result = 1;
    } else if (strcmp(dg, ex) == 0) {
        result = record(st, 1, label, NULL);
    } else {
        char detail[RT_TESTING_LINE_MAX];
        snprintf(detail, sizeof(detail), "digest moved: expected %s", ex);
        result = record(st, 0, label, detail);
    }
    rt_string_unref(digest);
    return result;
}
