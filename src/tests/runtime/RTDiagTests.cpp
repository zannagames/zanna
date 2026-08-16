//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTDiagTests.cpp
// Purpose: Tests for Zanna.Diagnostics assert functions.
//
// Note: These tests verify that passing assertions don't trap. The failure
// cases are tested separately since they terminate the process.
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <string>

/// @brief Erase constness so borrowed text can travel through an opaque `void *`.
/// @details The runtime payload APIs take `void *`; these tests hand them
///          read-only bytes they own elsewhere and never write through the
///          result, so the cast is safe here.
/// @param text Borrowed NUL-terminated bytes.
/// @return The same address as a mutable `void *`.
static void *borrowedPayload(const char *text) {
    return const_cast<void *>(static_cast<const void *>(text));
}

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

static bool g_return_traps = false;
static int g_returning_trap_count = 0;
static std::string g_last_returning_trap;

extern "C" void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_returning_trap_count++;
        g_last_returning_trap = msg ? msg : "";
        return;
    }
    rt_abort(msg);
}

// ============================================================================
// Helper
// ============================================================================

static rt_string make_str(const char *s) {
    return rt_const_cstr(s);
}

static void call_assert_eq_str_embedded_nul_failure() {
    const char lhs_bytes[] = {'a', '\0', 'b'};
    const char rhs_bytes[] = {'a', '\0', 'c'};
    rt_string lhs = rt_string_from_bytes(lhs_bytes, sizeof(lhs_bytes));
    rt_string rhs = rt_string_from_bytes(rhs_bytes, sizeof(rhs_bytes));
    rt_diag_assert_eq_str(lhs, rhs, make_str("nul mismatch"));
}

static void call_assert_fail_invalid_message() {
    int local = 42;
    rt_diag_assert_fail((rt_string)&local);
}

static void call_assert_invalid_message() {
    int local = 42;
    rt_diag_assert(0, (rt_string)&local);
}

static void test_invalid_message_returning_trap_stops_assertion() {
    int local = 42;
    g_returning_trap_count = 0;
    g_last_returning_trap.clear();
    g_return_traps = true;
    rt_diag_assert_eq(1, 2, (rt_string)&local);
    g_return_traps = false;

    assert(g_returning_trap_count == 1);
    assert(g_last_returning_trap.find("invalid message string handle") != std::string::npos);
}

static void call_trap_string_escapes_controls() {
    const char bytes[] = {'l', 'i', 'n', 'e', '\n', '"', '\\'};
    rt_string msg = rt_string_from_bytes(bytes, sizeof(bytes));
    rt_trap_string(msg);
}

static void expect_trap(void (*fn)(), const char *message) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn();
        rt_trap_clear_recovery();
        assert(false && "expected diagnostic trap");
    } else {
        std::string text = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(text.find(message) != std::string::npos);
    }
}

// ============================================================================
// AssertEq Tests (passing cases)
// ============================================================================

static void test_assert_eq_passing() {
    // These should all pass (not trap)
    rt_diag_assert_eq(42, 42, make_str("equal integers"));
    rt_diag_assert_eq(0, 0, make_str("zero equals zero"));
    rt_diag_assert_eq(-100, -100, make_str("negative integers"));
    rt_diag_assert_eq(INT64_MAX, INT64_MAX, make_str("max int64"));
    rt_diag_assert_eq(INT64_MIN, INT64_MIN, make_str("min int64"));

    printf("test_assert_eq_passing: PASSED\n");
}

// ============================================================================
// AssertNeq Tests (passing cases)
// ============================================================================

static void test_assert_neq_passing() {
    rt_diag_assert_neq(1, 2, make_str("different integers"));
    rt_diag_assert_neq(0, 1, make_str("zero vs one"));
    rt_diag_assert_neq(-1, 1, make_str("negative vs positive"));
    rt_diag_assert_neq(INT64_MAX, INT64_MIN, make_str("max vs min"));

    printf("test_assert_neq_passing: PASSED\n");
}

// ============================================================================
// AssertEqNum Tests (passing cases)
// ============================================================================

static void test_assert_eq_num_passing() {
    rt_diag_assert_eq_num(3.14, 3.14, make_str("equal doubles"));
    rt_diag_assert_eq_num(0.0, 0.0, make_str("zero equals zero"));
    rt_diag_assert_eq_num(-2.5, -2.5, make_str("negative doubles"));

    // Test with very close values (within epsilon)
    rt_diag_assert_eq_num(1.0, 1.0 + 1e-12, make_str("nearly equal"));

    // Test NaN equality (special case - NaN equals NaN for this assertion)
    rt_diag_assert_eq_num(NAN, NAN, make_str("NaN equals NaN"));

    // Test infinity
    rt_diag_assert_eq_num(INFINITY, INFINITY, make_str("infinity equals infinity"));
    rt_diag_assert_eq_num(-INFINITY, -INFINITY, make_str("neg infinity"));

    printf("test_assert_eq_num_passing: PASSED\n");
}

// ============================================================================
// AssertEqStr Tests (passing cases)
// ============================================================================

static void test_assert_eq_str_passing() {
    rt_diag_assert_eq_str(make_str("hello"), make_str("hello"), make_str("equal strings"));
    rt_diag_assert_eq_str(make_str(""), make_str(""), make_str("empty strings"));
    rt_diag_assert_eq_str(make_str("abc123"), make_str("abc123"), make_str("alphanumeric"));
    rt_diag_assert_eq_str(NULL, NULL, make_str("null strings"));

    const char bytes[] = {'a', '\0', 'b'};
    rt_string lhs = rt_string_from_bytes(bytes, sizeof(bytes));
    rt_string rhs = rt_string_from_bytes(bytes, sizeof(bytes));
    rt_diag_assert_eq_str(lhs, rhs, make_str("embedded nul strings"));
    rt_string_unref(lhs);
    rt_string_unref(rhs);

    printf("test_assert_eq_str_passing: PASSED\n");
}

// ============================================================================
// AssertNull Tests (passing cases)
// ============================================================================

static void test_assert_null_passing() {
    rt_diag_assert_null(nullptr, make_str("null pointer"));

    printf("test_assert_null_passing: PASSED\n");
}

// ============================================================================
// AssertNotNull Tests (passing cases)
// ============================================================================

static void test_assert_not_null_passing() {
    int dummy = 42;
    rt_diag_assert_not_null(&dummy, make_str("non-null pointer"));

    const char *str = "test";
    rt_diag_assert_not_null(borrowedPayload(str), make_str("string pointer"));

    printf("test_assert_not_null_passing: PASSED\n");
}

// ============================================================================
// AssertGt Tests (passing cases)
// ============================================================================

static void test_assert_gt_passing() {
    rt_diag_assert_gt(10, 5, make_str("10 > 5"));
    rt_diag_assert_gt(0, -1, make_str("0 > -1"));
    rt_diag_assert_gt(INT64_MAX, 0, make_str("max > 0"));
    rt_diag_assert_gt(1, INT64_MIN, make_str("1 > min"));

    printf("test_assert_gt_passing: PASSED\n");
}

// ============================================================================
// AssertLt Tests (passing cases)
// ============================================================================

static void test_assert_lt_passing() {
    rt_diag_assert_lt(5, 10, make_str("5 < 10"));
    rt_diag_assert_lt(-1, 0, make_str("-1 < 0"));
    rt_diag_assert_lt(0, INT64_MAX, make_str("0 < max"));
    rt_diag_assert_lt(INT64_MIN, 1, make_str("min < 1"));

    printf("test_assert_lt_passing: PASSED\n");
}

// ============================================================================
// AssertGte Tests (passing cases)
// ============================================================================

static void test_assert_gte_passing() {
    rt_diag_assert_gte(10, 5, make_str("10 >= 5"));
    rt_diag_assert_gte(5, 5, make_str("5 >= 5 (equal)"));
    rt_diag_assert_gte(0, -1, make_str("0 >= -1"));
    rt_diag_assert_gte(0, 0, make_str("0 >= 0"));

    printf("test_assert_gte_passing: PASSED\n");
}

// ============================================================================
// AssertLte Tests (passing cases)
// ============================================================================

static void test_assert_lte_passing() {
    rt_diag_assert_lte(5, 10, make_str("5 <= 10"));
    rt_diag_assert_lte(5, 5, make_str("5 <= 5 (equal)"));
    rt_diag_assert_lte(-1, 0, make_str("-1 <= 0"));
    rt_diag_assert_lte(0, 0, make_str("0 <= 0"));

    printf("test_assert_lte_passing: PASSED\n");
}

// ============================================================================
// Basic Assert Tests (passing cases)
// ============================================================================

static void test_basic_assert_passing() {
    rt_diag_assert(1, make_str("true condition"));
    rt_diag_assert(42, make_str("non-zero is true"));
    rt_diag_assert(-1, make_str("negative non-zero is true"));

    printf("test_basic_assert_passing: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Zanna.Diagnostics Assert Tests ===\n\n");

    // Basic assert
    test_basic_assert_passing();

    // Equality assertions
    test_assert_eq_passing();
    test_assert_neq_passing();
    test_assert_eq_num_passing();
    test_assert_eq_str_passing();

    // Null assertions
    test_assert_null_passing();
    test_assert_not_null_passing();

    // Comparison assertions
    test_assert_gt_passing();
    test_assert_lt_passing();
    test_assert_gte_passing();
    test_assert_lte_passing();

    expect_trap(call_assert_eq_str_embedded_nul_failure, "\\x00");
    expect_trap(call_assert_fail_invalid_message, "invalid message string handle");
    expect_trap(call_assert_invalid_message, "invalid message string handle");
    expect_trap(call_trap_string_escapes_controls, "line\\x0A\\\"\\\\");
    test_invalid_message_returning_trap_stops_assertion();

    printf("\nAll RTDiagTests passed!\n");
    return 0;
}
