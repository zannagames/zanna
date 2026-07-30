//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTFrozenSetTests.cpp
// Purpose: Verify immutable FrozenSet construction, set operations, ownership,
//          and hostile source-element handling.
// Key invariants:
//   - Construction deduplicates valid strings and rolls back partial sets.
//   - Invalid Seq sources and non-string element representations trap and return null.
// Ownership/Lifetime:
//   - Hostile-input regressions release their strings, boxes, objects, Seqs, and sets.
//   - Borrowing source Seqs never consume their test-owned elements.
// Links: src/runtime/collections/rt_frozenset.c,
//        src/runtime/collections/rt_seq.c, src/runtime/oop/rt_box.c
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_frozenset.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
static bool g_trap_returns = false;
static int g_returning_trap_count = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    if (g_trap_returns) {
        (void)msg;
        ++g_returning_trap_count;
        return;
    }
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *s, size_t len) {
    return rt_string_from_bytes(s, len);
}

static void *new_obj() {
    void *obj = rt_obj_new_i64(0, 8);
    assert(obj != nullptr);
    return obj;
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void begin_returning_traps() {
    g_returning_trap_count = 0;
    g_trap_returns = true;
}

static void end_returning_traps() {
    g_trap_returns = false;
}

static void test_empty() {
    void *fs = rt_frozenset_empty();
    assert(fs != NULL);
    assert(rt_frozenset_len(fs) == 0);
    assert(rt_frozenset_is_empty(fs) == 1);
}

static void test_from_seq() {
    void *seq = rt_seq_new();
    rt_seq_push(seq, make_str("apple"));
    rt_seq_push(seq, make_str("banana"));
    rt_seq_push(seq, make_str("cherry"));

    void *fs = rt_frozenset_from_seq(seq);
    assert(rt_frozenset_len(fs) == 3);
    assert(rt_frozenset_is_empty(fs) == 0);
}

static void test_has() {
    void *seq = rt_seq_new();
    rt_seq_push(seq, make_str("alpha"));
    rt_seq_push(seq, make_str("beta"));

    void *fs = rt_frozenset_from_seq(seq);
    rt_string a = make_str("alpha");
    rt_string b = make_str("beta");
    rt_string c = make_str("gamma");

    assert(rt_frozenset_has(fs, a) == 1);
    assert(rt_frozenset_has(fs, b) == 1);
    assert(rt_frozenset_has(fs, c) == 0);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_dedup() {
    void *seq = rt_seq_new();
    rt_seq_push(seq, make_str("dup"));
    rt_seq_push(seq, make_str("dup"));
    rt_seq_push(seq, make_str("dup"));
    rt_seq_push(seq, make_str("unique"));

    void *fs = rt_frozenset_from_seq(seq);
    assert(rt_frozenset_len(fs) == 2);
}

static void test_embedded_nul_items_are_distinct() {
    void *seq = rt_seq_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");

    rt_seq_push(seq, k1);
    rt_seq_push(seq, k2);

    void *fs = rt_frozenset_from_seq(seq);
    assert(rt_frozenset_len(fs) == 2);
    assert(rt_frozenset_has(fs, k1) == 1);
    assert(rt_frozenset_has(fs, k2) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_items() {
    void *seq = rt_seq_new();
    rt_seq_push(seq, make_str("x"));
    rt_seq_push(seq, make_str("y"));

    void *fs = rt_frozenset_from_seq(seq);
    void *items = rt_frozenset_items(fs);
    assert(rt_seq_len(items) == 2);
}

static void test_union() {
    void *s1 = rt_seq_new();
    rt_seq_push(s1, make_str("a"));
    rt_seq_push(s1, make_str("b"));

    void *s2 = rt_seq_new();
    rt_seq_push(s2, make_str("b"));
    rt_seq_push(s2, make_str("c"));

    void *fs1 = rt_frozenset_from_seq(s1);
    void *fs2 = rt_frozenset_from_seq(s2);

    void *u = rt_frozenset_union(fs1, fs2);
    assert(rt_frozenset_len(u) == 3);

    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_string c = make_str("c");
    assert(rt_frozenset_has(u, a) == 1);
    assert(rt_frozenset_has(u, b) == 1);
    assert(rt_frozenset_has(u, c) == 1);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_intersect() {
    void *s1 = rt_seq_new();
    rt_seq_push(s1, make_str("a"));
    rt_seq_push(s1, make_str("b"));
    rt_seq_push(s1, make_str("c"));

    void *s2 = rt_seq_new();
    rt_seq_push(s2, make_str("b"));
    rt_seq_push(s2, make_str("c"));
    rt_seq_push(s2, make_str("d"));

    void *fs1 = rt_frozenset_from_seq(s1);
    void *fs2 = rt_frozenset_from_seq(s2);

    void *inter = rt_frozenset_intersect(fs1, fs2);
    assert(rt_frozenset_len(inter) == 2);

    rt_string b = make_str("b");
    rt_string c = make_str("c");
    assert(rt_frozenset_has(inter, b) == 1);
    assert(rt_frozenset_has(inter, c) == 1);

    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_diff() {
    void *s1 = rt_seq_new();
    rt_seq_push(s1, make_str("a"));
    rt_seq_push(s1, make_str("b"));
    rt_seq_push(s1, make_str("c"));

    void *s2 = rt_seq_new();
    rt_seq_push(s2, make_str("b"));

    void *fs1 = rt_frozenset_from_seq(s1);
    void *fs2 = rt_frozenset_from_seq(s2);

    void *d = rt_frozenset_diff(fs1, fs2);
    assert(rt_frozenset_len(d) == 2);

    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_string c = make_str("c");
    assert(rt_frozenset_has(d, a) == 1);
    assert(rt_frozenset_has(d, b) == 0);
    assert(rt_frozenset_has(d, c) == 1);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
}

static void test_is_subset() {
    void *s1 = rt_seq_new();
    rt_seq_push(s1, make_str("a"));
    rt_seq_push(s1, make_str("b"));

    void *s2 = rt_seq_new();
    rt_seq_push(s2, make_str("a"));
    rt_seq_push(s2, make_str("b"));
    rt_seq_push(s2, make_str("c"));

    void *fs1 = rt_frozenset_from_seq(s1);
    void *fs2 = rt_frozenset_from_seq(s2);

    assert(rt_frozenset_is_subset(fs1, fs2) == 1);
    assert(rt_frozenset_is_subset(fs2, fs1) == 0);
}

static void test_equals() {
    void *s1 = rt_seq_new();
    rt_seq_push(s1, make_str("x"));
    rt_seq_push(s1, make_str("y"));

    void *s2 = rt_seq_new();
    rt_seq_push(s2, make_str("y"));
    rt_seq_push(s2, make_str("x"));

    void *fs1 = rt_frozenset_from_seq(s1);
    void *fs2 = rt_frozenset_from_seq(s2);

    assert(rt_frozenset_equals(fs1, fs2) == 1);

    void *s3 = rt_seq_new();
    rt_seq_push(s3, make_str("x"));
    void *fs3 = rt_frozenset_from_seq(s3);
    assert(rt_frozenset_equals(fs1, fs3) == 0);
}

static void test_null_safety() {
    assert(rt_frozenset_len(NULL) == 0);
    assert(rt_frozenset_is_empty(NULL) == 1);
    assert(rt_frozenset_has(NULL, NULL) == 0);
    assert(rt_frozenset_is_subset(NULL, NULL) == 1);
    assert(rt_frozenset_equals(NULL, NULL) == 1);
}

static void expect_invalid_element(void *invalid) {
    void *items = rt_seq_new();
    rt_string valid = make_str("valid");
    assert(items != nullptr);
    assert(valid != nullptr);
    const size_t refs = valid->literal_refs;

    rt_seq_push(items, valid);
    rt_seq_push(items, invalid);

    begin_returning_traps();
    void *set = rt_frozenset_from_seq(items);
    end_returning_traps();

    assert(set == nullptr);
    assert(g_returning_trap_count == 1);
    assert(valid->literal_refs == refs);

    release_obj(items);
    rt_string_unref(valid);
}

static void test_rejects_invalid_elements() {
    void *wrong_box = rt_box_i64(7);
    void *wrong_object = new_obj();
    assert(wrong_box != nullptr);

    expect_invalid_element(wrong_box);
    expect_invalid_element(wrong_object);
    expect_invalid_element(reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    release_obj(wrong_box);
    release_obj(wrong_object);
}

static void test_rejects_invalid_source_sequence() {
    void *fake = new_obj();

    begin_returning_traps();
    void *set = rt_frozenset_from_seq(fake);
    end_returning_traps();

    assert(set == nullptr);
    assert(g_returning_trap_count == 1);
    release_obj(fake);
}

static void test_accepts_boxed_string_elements() {
    void *items = rt_seq_new();
    rt_string value = make_str("boxed");
    void *boxed_value = rt_box_str(value);
    void *boxed_null = rt_box_str(nullptr);
    assert(items != nullptr);
    assert(boxed_value != nullptr);
    assert(boxed_null != nullptr);

    rt_seq_push(items, boxed_value);
    rt_seq_push(items, boxed_null);

    void *set = rt_frozenset_from_seq(items);
    assert(set != nullptr);
    assert(rt_frozenset_len(set) == 1);
    assert(rt_frozenset_has(set, value) == 1);

    release_obj(set);
    release_obj(items);
    release_obj(boxed_value);
    release_obj(boxed_null);
    rt_string_unref(value);
}

/// @brief Main.
int main() {
    test_empty();
    test_from_seq();
    test_has();
    test_dedup();
    test_embedded_nul_items_are_distinct();
    test_items();
    test_union();
    test_intersect();
    test_diff();
    test_is_subset();
    test_equals();
    test_null_safety();
    test_rejects_invalid_elements();
    test_rejects_invalid_source_sequence();
    test_accepts_boxed_string_elements();
    return 0;
}
