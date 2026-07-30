//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTCountMapTests.cpp
// Purpose: Tests for CountMap (frequency counting map).
//
//===----------------------------------------------------------------------===//

#include "rt_countmap.h"
#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>

static jmp_buf g_trap_jmp;
static bool g_trap_expected = false;

extern "C" void vm_trap(const char *msg) {
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *s, size_t len) {
    return rt_string_from_bytes(s, len);
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

static void test_new_empty() {
    void *cm = rt_countmap_new();
    assert(cm != NULL);
    assert(rt_countmap_len(cm) == 0);
    assert(rt_countmap_is_empty(cm) == 1);
    assert(rt_countmap_total(cm) == 0);
}

static void test_inc() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("apple");

    assert(rt_countmap_inc(cm, k) == 1);
    assert(rt_countmap_inc(cm, k) == 2);
    assert(rt_countmap_inc(cm, k) == 3);

    assert(rt_countmap_get(cm, k) == 3);
    assert(rt_countmap_len(cm) == 1);
    assert(rt_countmap_total(cm) == 3);

    rt_string_unref(k);
}

static void test_inc_by() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("banana");

    assert(rt_countmap_inc_by(cm, k, 5) == 5);
    assert(rt_countmap_inc_by(cm, k, 3) == 8);
    assert(rt_countmap_total(cm) == 8);

    // VDOC-099: the return is the post-operation count for every accepted
    // amount — zero is a lookup no-op, and negatives trap.
    assert(rt_countmap_inc_by(cm, k, 0) == 8);
    assert(rt_countmap_total(cm) == 8);
    rt_string missing = make_str("missing");
    assert(rt_countmap_inc_by(cm, missing, 0) == 0);
    rt_string_unref(missing);
    EXPECT_TRAP(rt_countmap_inc_by(cm, k, -2));
    assert(rt_countmap_total(cm) == 8);

    rt_string_unref(k);
}

static void test_dec() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("cherry");

    rt_countmap_inc_by(cm, k, 3);
    assert(rt_countmap_dec(cm, k) == 2);
    assert(rt_countmap_dec(cm, k) == 1);
    assert(rt_countmap_dec(cm, k) == 0);

    // Entry removed when count hits 0
    assert(rt_countmap_has(cm, k) == 0);
    assert(rt_countmap_len(cm) == 0);

    // Decrement nonexistent
    assert(rt_countmap_dec(cm, k) == 0);

    rt_string_unref(k);
}

static void test_set() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("date");

    rt_countmap_set(cm, k, 10);
    assert(rt_countmap_get(cm, k) == 10);
    assert(rt_countmap_total(cm) == 10);

    rt_countmap_set(cm, k, 5);
    assert(rt_countmap_get(cm, k) == 5);
    assert(rt_countmap_total(cm) == 5);

    // Set to 0 removes
    rt_countmap_set(cm, k, 0);
    assert(rt_countmap_has(cm, k) == 0);
    assert(rt_countmap_len(cm) == 0);
    assert(rt_countmap_total(cm) == 0);

    rt_string_unref(k);
}

static void test_has() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("elderberry");

    assert(rt_countmap_has(cm, k) == 0);
    rt_countmap_inc(cm, k);
    assert(rt_countmap_has(cm, k) == 1);

    rt_string_unref(k);
}

static void test_multiple_keys() {
    void *cm = rt_countmap_new();
    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_string c = make_str("c");

    rt_countmap_inc_by(cm, a, 10);
    rt_countmap_inc_by(cm, b, 5);
    rt_countmap_inc_by(cm, c, 20);

    assert(rt_countmap_len(cm) == 3);
    assert(rt_countmap_total(cm) == 35);
    assert(rt_countmap_get(cm, a) == 10);
    assert(rt_countmap_get(cm, b) == 5);
    assert(rt_countmap_get(cm, c) == 20);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_embedded_nul_keys_are_distinct() {
    void *cm = rt_countmap_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");

    rt_countmap_inc_by(cm, k1, 2);
    rt_countmap_inc_by(cm, k2, 5);

    assert(rt_countmap_len(cm) == 2);
    assert(rt_countmap_get(cm, k1) == 2);
    assert(rt_countmap_get(cm, k2) == 5);
    assert(rt_countmap_total(cm) == 7);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_counter_overflow_traps() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("overflow");

    rt_countmap_set(cm, k, INT64_MAX);
    EXPECT_TRAP(rt_countmap_inc(cm, k));

    rt_string_unref(k);
}

static void test_keys() {
    void *cm = rt_countmap_new();
    rt_string a = make_str("x");
    rt_string b = make_str("y");

    rt_countmap_inc(cm, a);
    rt_countmap_inc(cm, b);

    void *keys = rt_countmap_keys(cm);
    assert(rt_seq_len(keys) == 2);
    assert(rt_seq_cap(keys) == 2);

    rt_string_unref(a);
    rt_string_unref(b);
}

static void test_most_common() {
    void *cm = rt_countmap_new();
    rt_string a = make_str("rare");
    rt_string b = make_str("common");
    rt_string c = make_str("very_common");

    rt_countmap_inc_by(cm, a, 1);
    rt_countmap_inc_by(cm, b, 10);
    rt_countmap_inc_by(cm, c, 100);

    void *top = rt_countmap_most_common(cm, 2);
    assert(rt_seq_len(top) == 2);
    assert(rt_seq_cap(top) == 2);

    // First should be "very_common" (100)
    rt_string first = (rt_string)rt_seq_get(top, 0);
    assert(str_eq(first, "very_common"));

    // Second should be "common" (10)
    rt_string second = (rt_string)rt_seq_get(top, 1);
    assert(str_eq(second, "common"));

    // On 32-bit hosts this value is one past SIZE_MAX. It still means "all"
    // rather than truncating to a zero-sized request.
    void *all = rt_countmap_most_common(cm, INT64_C(4294967296));
    assert(rt_seq_len(all) == 3);
    void *none = rt_countmap_most_common(cm, 0);
    assert(rt_seq_len(none) == 0);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_remove() {
    void *cm = rt_countmap_new();
    rt_string k = make_str("fig");

    rt_countmap_inc_by(cm, k, 7);
    assert(rt_countmap_remove(cm, k) == 1);
    assert(rt_countmap_len(cm) == 0);
    assert(rt_countmap_total(cm) == 0);

    // Remove nonexistent
    assert(rt_countmap_remove(cm, k) == 0);

    rt_string_unref(k);
}

static void test_clear() {
    void *cm = rt_countmap_new();
    rt_string a = make_str("g1");
    rt_string b = make_str("g2");

    rt_countmap_inc_by(cm, a, 5);
    rt_countmap_inc_by(cm, b, 3);

    rt_countmap_clear(cm);
    assert(rt_countmap_len(cm) == 0);
    assert(rt_countmap_total(cm) == 0);
    assert(rt_countmap_is_empty(cm) == 1);

    rt_string_unref(a);
    rt_string_unref(b);
}

static void test_null_safety() {
    assert(rt_countmap_len(NULL) == 0);
    assert(rt_countmap_is_empty(NULL) == 1);
    assert(rt_countmap_total(NULL) == 0);
    assert(rt_countmap_get(NULL, NULL) == 0);
    assert(rt_countmap_has(NULL, NULL) == 0);
    assert(rt_countmap_remove(NULL, NULL) == 0);
}

int main() {
    test_new_empty();
    test_inc();
    test_inc_by();
    test_dec();
    test_set();
    test_has();
    test_multiple_keys();
    test_embedded_nul_keys_are_distinct();
    test_counter_overflow_traps();
    test_keys();
    test_most_common();
    test_remove();
    test_clear();
    test_null_safety();

    return 0;
}
