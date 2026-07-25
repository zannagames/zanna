//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_trap.c
// Purpose: Fatal runtime trap helpers for the Zanna runtime C ABI. Provides
//   narrow convenience wrappers such as rt_trap_div0() and assertion helpers
//   that ultimately route through the structured runtime trap dispatcher.
//   Centralising trap logic here keeps trap kinds, error codes, and messages
//   consistent across the VM, native code, and runtime C shim paths.
//
// Key invariants:
//   - rt_trap(msg) is reserved for unrecoverable conditions such as invariant
//     violations and checked arithmetic faults.
//   - rt_trap_div0() and rt_trap_ovf() preserve the VM-visible trap kind/code
//     expected by diagnostics and trap-recovering tests.
//   - In unit tests, vm_trap()/runtime trap hooks can be overridden so traps
//     are observed without killing the test process.
//
// Ownership/Lifetime:
//   - Helpers may allocate small stack buffers for formatted diagnostics only.
//   - Runtime string messages are borrowed, validated, and escaped by stored
//     byte length so embedded NULs cannot truncate diagnostics.
//
// Links: src/runtime/core/rt_trap.h (public API),
//        src/runtime/core/rt_internal.h (rt_trap macro shim)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime trap adapters and diagnostic assertion implementations.

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rt_error.h"
#include "rt_internal.h"
#include "rt_trap.h"

/// @brief Append @p s to @p dst with C-style escaping for non-ASCII bytes and quotes.
/// @details Used to format runtime-string trap messages for the C-string trap dispatcher
///          without truncating at embedded NULs. Validates the handle first — invalid
///          handles render as the literal `<invalid string>` rather than dereferencing
///          arbitrary memory. Caps shown bytes at 64 with a `...` ellipsis to keep trap
///          messages bounded for log output. Backslash and double-quote are escaped;
///          non-printable bytes render as `\xNN`. The function is append-only: existing
///          contents of @p dst are preserved.
/// @param dst Existing NUL-terminated destination buffer; may be NULL.
/// @param dst_cap Total destination capacity.
/// @param s Borrowed runtime string; NULL renders as no bytes.
static void append_escaped_string(char *dst, size_t dst_cap, rt_string s) {
    if (!dst || dst_cap == 0)
        return;
    size_t pos = strlen(dst);
    if (pos >= dst_cap)
        return;

    if (s && !rt_string_is_handle((const void *)s)) {
        snprintf(dst + pos, dst_cap - pos, "<invalid string>");
        return;
    }

    const char *bytes = s ? rt_string_cstr(s) : "";
    if (s && !bytes) {
        snprintf(dst + pos, dst_cap - pos, "<invalid string>");
        return;
    }
    size_t len = s ? (size_t)rt_str_len(s) : 0;
    size_t shown = len < 64 ? len : 64;
    for (size_t i = 0; i < shown && pos + 5 < dst_cap; ++i) {
        unsigned char ch = (unsigned char)bytes[i];
        if (ch == '\\' || ch == '"') {
            dst[pos++] = '\\';
            dst[pos++] = (char)ch;
        } else if (ch >= 32 && ch < 127) {
            dst[pos++] = (char)ch;
        } else {
            int n = snprintf(dst + pos, dst_cap - pos, "\\x%02X", ch);
            if (n < 0)
                break;
            pos += (size_t)n;
        }
    }
    if (len > shown && pos + 4 < dst_cap) {
        dst[pos++] = '.';
        dst[pos++] = '.';
        dst[pos++] = '.';
    }
    dst[pos] = '\0';
}

/// @brief Return 1 if @p message is a live, non-empty runtime string handle.
/// @details NULL and empty strings return 0 so the caller falls back to a fixed
///          diagnostic. Invalid non-NULL handles trap at the diagnostic boundary
///          instead of being silently replaced by the fallback text.
/// @param message Borrowed optional diagnostic string.
/// @return One only for a valid handle with at least one stored byte.
static int message_has_bytes(rt_string message) {
    if (message && !rt_string_is_handle((const void *)message)) {
        rt_trap("Zanna.Core.Diagnostics: invalid message string handle");
        return 0;
    }
    if (!message || !message->data)
        return 0;
    return rt_str_len(message) > 0 ? 1 : 0;
}

/// @brief Render @p message into @p buf for trap formatting, falling back to @p fallback.
/// @details If @p message is empty/invalid or the supplied buffer is unusable, returns the
///          @p fallback C-string verbatim. Otherwise zeroes @p buf, escapes the message via
///          `append_escaped_string`, and returns @p buf. The escaped form is guaranteed to
///          fit inside @p cap and to be NUL-terminated.
/// @param message Borrowed optional runtime diagnostic.
/// @param fallback Borrowed fallback C string.
/// @param buf Writable escape buffer; may be NULL when @p cap is zero.
/// @param cap Total capacity of @p buf.
/// @return @p buf for a rendered non-empty message; otherwise @p fallback.
static const char *format_message(rt_string message, const char *fallback, char *buf, size_t cap) {
    if (!message_has_bytes(message))
        return fallback;
    if (!buf || cap == 0)
        return fallback;
    buf[0] = '\0';
    append_escaped_string(buf, cap, message);
    return buf;
}

/// @brief Report a division-by-zero trap through the active runtime trap hook.
/// @details The default hook terminates the process; test and embedder hooks may
///          return after recording the trap for diagnostics.
void rt_trap_div0(void) {
    rt_trap_raise_kind(RT_TRAP_KIND_DIVIDE_BY_ZERO, 0, -1, "Zanna runtime trap: division by zero");
}

/// @brief Report an integer-overflow trap through the active runtime trap hook.
/// @details Mirrors the checked-arithmetic trap path used by the VM/native
///          backends so backend lowering can call a no-argument helper.
void rt_trap_ovf(void) {
    rt_trap_raise_kind(
        RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "Zanna runtime trap: integer overflow");
}

/// @brief Trap with a managed runtime string message.
/// @details Null, empty, or unavailable data uses `"trap"`. Invalid handles
///          raise a dedicated diagnostics trap. Valid stored bytes are escaped
///          into a bounded stack buffer before dispatch, preserving embedded
///          NUL and nonprintable bytes.
/// @param msg Borrowed runtime message; may be NULL.
void rt_trap_string(rt_string msg) {
    if (!msg) {
        rt_trap("trap");
        return;
    }
    if (!rt_string_is_handle((const void *)msg)) {
        rt_trap("Zanna.Core.Diagnostics.Trap: invalid string handle");
        return;
    }

    int64_t len = rt_str_len(msg);
    if (len <= 0) {
        rt_trap("trap");
        return;
    }

    const char *bytes = rt_string_cstr(msg);
    if (!bytes) {
        rt_trap("trap");
        return;
    }
    char escaped[512];
    escaped[0] = '\0';
    append_escaped_string(escaped, sizeof(escaped), msg);
    rt_trap(escaped);
}

/// @brief Assert that @p condition holds; otherwise trap with @p message.
/// @details When @p condition is zero, evaluates @p message and raises a runtime
///          trap using @ref rt_trap. Empty or null messages use the default
///          text "Assertion failed" to avoid silent failures.
/// @param condition Non-zero when the assertion succeeded.
/// @param message Optional runtime string describing the failure.
void rt_diag_assert(int8_t condition, rt_string message) {
    if (condition)
        return;

    char msg_buf[160];
    rt_trap(format_message(message, "Assertion failed", msg_buf, sizeof(msg_buf)));
}

/// @brief Assert two integers are equal.
/// @param expected Expected signed value.
/// @param actual Observed signed value.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_eq(int64_t expected, int64_t actual, rt_string message) {
    if (expected == actual)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertEq failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %" PRId64 ", got %" PRId64, msg, expected, actual);
    rt_trap(buf);
}

/// @brief Assert two integers are not equal.
/// @param a First signed value.
/// @param b Second signed value.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_neq(int64_t a, int64_t b, rt_string message) {
    if (a != b)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertNeq failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: values should not be equal (both are %" PRId64 ")", msg, a);
    rt_trap(buf);
}

/// @brief Assert two floating-point numbers are approximately equal.
/// @details Exact equality (including same-signed infinities) succeeds, as do
///          two NaNs. Otherwise uses absolute error below `1e-9` when the
///          maximum magnitude is below one and relative error below `1e-9`
///          for larger magnitudes.
/// @param expected Expected floating-point value.
/// @param actual Observed floating-point value.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_eq_num(double expected, double actual, rt_string message) {
    // Use a relative epsilon for float comparison
    double epsilon = 1e-9;
    double diff = fabs(expected - actual);
    double maxval = fmax(fabs(expected), fabs(actual));

    // Handle special cases
    if (expected == actual)
        return;
    if (isnan(expected) && isnan(actual))
        return;

    // Use relative comparison for large values, absolute for small
    bool equal = (maxval < 1.0) ? (diff < epsilon) : (diff / maxval < epsilon);
    if (equal)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertEqNum failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %g, got %g (diff=%g)", msg, expected, actual, diff);
    rt_trap(buf);
}

/// @brief Assert two runtime strings are byte-equal.
/// @details Two nulls succeed. Valid non-null handles compare through
///          @ref rt_str_eq; invalid handles force failure and are rendered as
///          bounded invalid-string markers.
/// @param expected Borrowed expected string; may be NULL.
/// @param actual Borrowed observed string; may be NULL.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_eq_str(rt_string expected, rt_string actual, rt_string message) {
    int expected_valid = !expected || rt_string_is_handle((const void *)expected);
    int actual_valid = !actual || rt_string_is_handle((const void *)actual);
    if (expected_valid && actual_valid && expected == actual)
        return;
    if (expected_valid && actual_valid && rt_str_eq(expected, actual) != 0)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertEqStr failed", msg_buf, sizeof(msg_buf));
    char buf[512];
    if (!expected_valid || !actual_valid)
        snprintf(buf, sizeof(buf), "%s: invalid string handle; expected \"", msg);
    else
        snprintf(buf, sizeof(buf), "%s: expected \"", msg);
    append_escaped_string(buf, sizeof(buf), expected);
    strncat(buf, "\", got \"", sizeof(buf) - strlen(buf) - 1);
    append_escaped_string(buf, sizeof(buf), actual);
    strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
    rt_trap(buf);
}

/// @brief Assert an object reference is null.
/// @param obj Borrowed opaque pointer expected to be NULL.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_null(void *obj, rt_string message) {
    if (obj == NULL)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertNull failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected null, got non-null object", msg);
    rt_trap(buf);
}

/// @brief Assert an object reference is not null.
/// @param obj Borrowed opaque pointer expected to be non-null.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_not_null(void *obj, rt_string message) {
    if (obj != NULL)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertNotNull failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected non-null, got null", msg);
    rt_trap(buf);
}

/// @brief Unconditionally fail with a message.
/// @param message Optional borrowed failure message.
void rt_diag_assert_fail(rt_string message) {
    char msg_buf[160];
    const char *msg = format_message(message, "AssertFail called", msg_buf, sizeof(msg_buf));
    rt_trap(msg);
}

/// @brief Assert first value is greater than second.
/// @param a Left signed operand.
/// @param b Right signed operand.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_gt(int64_t a, int64_t b, rt_string message) {
    if (a > b)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertGt failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %" PRId64 " > %" PRId64, msg, a, b);
    rt_trap(buf);
}

/// @brief Assert first value is less than second.
/// @param a Left signed operand.
/// @param b Right signed operand.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_lt(int64_t a, int64_t b, rt_string message) {
    if (a < b)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertLt failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %" PRId64 " < %" PRId64, msg, a, b);
    rt_trap(buf);
}

/// @brief Assert first value is greater than or equal to second.
/// @param a Left signed operand.
/// @param b Right signed operand.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_gte(int64_t a, int64_t b, rt_string message) {
    if (a >= b)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertGte failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %" PRId64 " >= %" PRId64, msg, a, b);
    rt_trap(buf);
}

/// @brief Assert first value is less than or equal to second.
/// @param a Left signed operand.
/// @param b Right signed operand.
/// @param message Optional borrowed failure prefix.
void rt_diag_assert_lte(int64_t a, int64_t b, rt_string message) {
    if (a <= b)
        return;

    char msg_buf[160];
    const char *msg = format_message(message, "AssertLte failed", msg_buf, sizeof(msg_buf));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected %" PRId64 " <= %" PRId64, msg, a, b);
    rt_trap(buf);
}
