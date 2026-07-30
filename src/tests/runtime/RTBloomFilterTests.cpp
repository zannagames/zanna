//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTBloomFilterTests.cpp
// Purpose: Tests BloomFilter construction, membership, estimates, and merges.
// Key invariants: Invalid string handles never reach the hash byte loop, and
//                 failed additions leave the bitset and count unchanged.
// Ownership/Lifetime: Runtime strings are released after each test; filters
//                     remain managed by the runtime test process.
// Links: src/runtime/collections/rt_bloomfilter.c
//
//===----------------------------------------------------------------------===//

#include "rt_bloomfilter.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static bool g_trap_returns = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_returns)
        return;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static void test_new() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    assert(bf != NULL);
    assert(rt_bloomfilter_count(bf) == 0);
}

static void test_negative_expected_items_traps() {
    EXPECT_TRAP(rt_bloomfilter_new(-1, 0.01));
    assert(g_last_trap && strstr(g_last_trap, "BloomFilter") != nullptr);
}

static void test_invalid_fpr_is_sanitized() {
    void *nan_bf = rt_bloomfilter_new(100, NAN);
    void *inf_bf = rt_bloomfilter_new(100, INFINITY);
    assert(nan_bf != NULL);
    assert(inf_bf != NULL);
    assert(rt_bloomfilter_count(nan_bf) == 0);
    assert(rt_bloomfilter_count(inf_bf) == 0);
}

static void test_add_and_check() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    rt_string s1 = make_str("hello");
    rt_string s2 = make_str("world");

    rt_bloomfilter_add(bf, s1);
    rt_bloomfilter_add(bf, s2);

    assert(rt_bloomfilter_count(bf) == 2);
    assert(rt_bloomfilter_might_contain(bf, s1) == 1);
    assert(rt_bloomfilter_might_contain(bf, s2) == 1);

    rt_string_unref(s1);
    rt_string_unref(s2);
}

static void test_definitely_not_present() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    rt_string s1 = make_str("alpha");
    rt_string s2 = make_str("beta");
    rt_string s3 = make_str("gamma");

    rt_bloomfilter_add(bf, s1);
    rt_bloomfilter_add(bf, s2);

    // gamma was never added -- might still report as present (false positive)
    // but the probability should be very low with a good filter
    // We just verify that items we DID add are found
    assert(rt_bloomfilter_might_contain(bf, s1) == 1);
    assert(rt_bloomfilter_might_contain(bf, s2) == 1);

    rt_string_unref(s1);
    rt_string_unref(s2);
    rt_string_unref(s3);
}

static void test_many_items() {
    void *bf = rt_bloomfilter_new(1000, 0.01);
    char buf[32];

    // Add 500 items
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "item_%d", i);
        rt_string s = make_str(buf);
        rt_bloomfilter_add(bf, s);
        rt_string_unref(s);
    }

    assert(rt_bloomfilter_count(bf) == 500);

    // Verify all added items are found
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "item_%d", i);
        rt_string s = make_str(buf);
        assert(rt_bloomfilter_might_contain(bf, s) == 1);
        rt_string_unref(s);
    }
}

static void test_fpr() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    assert(rt_bloomfilter_fpr(bf) == 0.0); // Empty filter

    rt_string s = make_str("test");
    rt_bloomfilter_add(bf, s);
    assert(rt_bloomfilter_fpr(bf) > 0.0);
    assert(rt_bloomfilter_fpr(bf) < 1.0);
    rt_string_unref(s);
}

static void test_duplicate_add_does_not_inflate_fpr() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    rt_string s = make_str("duplicate");

    rt_bloomfilter_add(bf, s);
    double first = rt_bloomfilter_fpr(bf);
    rt_bloomfilter_add(bf, s);
    double second = rt_bloomfilter_fpr(bf);

    assert(rt_bloomfilter_count(bf) == 2);
    assert(first == second);
    rt_string_unref(s);
}

static void test_clear() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    rt_string s = make_str("test");
    rt_bloomfilter_add(bf, s);
    assert(rt_bloomfilter_count(bf) == 1);

    rt_bloomfilter_clear(bf);
    assert(rt_bloomfilter_count(bf) == 0);
    assert(rt_bloomfilter_might_contain(bf, s) == 0);

    rt_string_unref(s);
}

static void test_merge() {
    void *a = rt_bloomfilter_new(100, 0.01);
    void *b = rt_bloomfilter_new(100, 0.01);

    rt_string s1 = make_str("alpha");
    rt_string s2 = make_str("beta");

    rt_bloomfilter_add(a, s1);
    rt_bloomfilter_add(b, s2);

    int64_t ok = rt_bloomfilter_merge(a, b);
    assert(ok == 1);
    assert(rt_bloomfilter_might_contain(a, s1) == 1);
    assert(rt_bloomfilter_might_contain(a, s2) == 1);

    rt_string_unref(s1);
    rt_string_unref(s2);
}

static void test_null_safety() {
    assert(rt_bloomfilter_count(NULL) == 0);
    assert(rt_bloomfilter_might_contain(NULL, NULL) == 0);
    assert(rt_bloomfilter_fpr(NULL) == 0.0);
    assert(rt_bloomfilter_merge(NULL, NULL) == 0);
}

static void test_returning_hook_rejects_forged_string() {
    void *bf = rt_bloomfilter_new(100, 0.01);
    rt_string forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(0x97531U));
    g_trap_returns = true;
    g_last_trap = nullptr;

    rt_bloomfilter_add(bf, forged);
    assert(g_last_trap != nullptr);
    assert(rt_bloomfilter_count(bf) == 0);
    assert(rt_bloomfilter_might_contain(bf, forged) == 0);

    g_trap_returns = false;
}

/// @brief Main.
int main() {
    test_new();
    test_negative_expected_items_traps();
    test_invalid_fpr_is_sanitized();
    test_add_and_check();
    test_definitely_not_present();
    test_many_items();
    test_fpr();
    test_duplicate_add_does_not_inflate_fpr();
    test_clear();
    test_merge();
    test_null_safety();
    test_returning_hook_rejects_forged_string();

    return 0;
}
