//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTBiMapTests.cpp
// Purpose: Tests for BiMap (bidirectional string map).
//
//===----------------------------------------------------------------------===//

#include "rt_bimap.h"
#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstdio>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *bytes, size_t len) {
    return rt_string_from_bytes(bytes, len);
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

static bool str_bytes_eq(rt_string s, const char *expected, size_t len) {
    const char *data = rt_string_cstr(s);
    return data && rt_str_len(s) == (int64_t)len && memcmp(data, expected, len) == 0;
}

static void test_new_empty() {
    void *bm = rt_bimap_new();
    assert(bm != NULL);
    assert(rt_bimap_len(bm) == 0);
    assert(rt_bimap_is_empty(bm) == 1);
}

static void test_put_and_get() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("en");
    rt_string v = make_str("English");
    rt_bimap_put(bm, k, v);

    assert(rt_bimap_len(bm) == 1);
    assert(rt_bimap_is_empty(bm) == 0);

    rt_string got = rt_bimap_get_by_key(bm, k);
    assert(str_eq(got, "English"));
    rt_string_unref(got);

    rt_string inv = rt_bimap_get_by_value(bm, v);
    assert(str_eq(inv, "en"));
    rt_string_unref(inv);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_has_key_value() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("us");
    rt_string v = make_str("United States");
    rt_bimap_put(bm, k, v);

    assert(rt_bimap_has_key(bm, k) == 1);
    assert(rt_bimap_has_value(bm, v) == 1);

    rt_string other = make_str("Canada");
    assert(rt_bimap_has_value(bm, other) == 0);
    rt_string_unref(other);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_overwrite_key() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("a");
    rt_string v1 = make_str("alpha");
    rt_string v2 = make_str("apple");

    rt_bimap_put(bm, k, v1);
    assert(rt_bimap_len(bm) == 1);

    // Overwrite with new value
    rt_bimap_put(bm, k, v2);
    assert(rt_bimap_len(bm) == 1);

    rt_string got = rt_bimap_get_by_key(bm, k);
    assert(str_eq(got, "apple"));
    rt_string_unref(got);

    // Old value should no longer be in inverse
    assert(rt_bimap_has_value(bm, v1) == 0);
    assert(rt_bimap_has_value(bm, v2) == 1);

    rt_string_unref(k);
    rt_string_unref(v1);
    rt_string_unref(v2);
}

static void test_overwrite_value() {
    void *bm = rt_bimap_new();
    rt_string k1 = make_str("k1");
    rt_string k2 = make_str("k2");
    rt_string v = make_str("shared");

    rt_bimap_put(bm, k1, v);
    assert(rt_bimap_len(bm) == 1);

    // Put with same value under different key - should evict k1
    rt_bimap_put(bm, k2, v);
    assert(rt_bimap_len(bm) == 1);

    assert(rt_bimap_has_key(bm, k1) == 0);
    assert(rt_bimap_has_key(bm, k2) == 1);

    rt_string inv = rt_bimap_get_by_value(bm, v);
    assert(str_eq(inv, "k2"));
    rt_string_unref(inv);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(v);
}

static void test_idempotent_and_two_sided_conflicts() {
    void *bm = rt_bimap_new();
    rt_string k1 = make_str("k1");
    rt_string k2 = make_str("k2");
    rt_string v1 = make_str("v1");
    rt_string v2 = make_str("v2");

    rt_bimap_put(bm, k1, v1);
    rt_bimap_put(bm, k1, v1);
    assert(rt_bimap_len(bm) == 1);
    assert(rt_bimap_has_key(bm, k1) == 1);
    assert(rt_bimap_has_value(bm, v1) == 1);

    rt_bimap_put(bm, k2, v2);
    assert(rt_bimap_len(bm) == 2);
    rt_bimap_put(bm, k1, v2);
    assert(rt_bimap_len(bm) == 1);
    assert(rt_bimap_has_key(bm, k1) == 1);
    assert(rt_bimap_has_key(bm, k2) == 0);
    assert(rt_bimap_has_value(bm, v1) == 0);
    assert(rt_bimap_has_value(bm, v2) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(v1);
    rt_string_unref(v2);
}

static void test_remove_by_key() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("x");
    rt_string v = make_str("y");
    rt_bimap_put(bm, k, v);

    assert(rt_bimap_remove_by_key(bm, k) == 1);
    assert(rt_bimap_len(bm) == 0);
    assert(rt_bimap_has_key(bm, k) == 0);
    assert(rt_bimap_has_value(bm, v) == 0);

    // Remove nonexistent
    assert(rt_bimap_remove_by_key(bm, k) == 0);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_remove_by_value() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("x");
    rt_string v = make_str("y");
    rt_bimap_put(bm, k, v);

    assert(rt_bimap_remove_by_value(bm, v) == 1);
    assert(rt_bimap_len(bm) == 0);
    assert(rt_bimap_has_key(bm, k) == 0);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_keys_values() {
    void *bm = rt_bimap_new();
    rt_string k1 = make_str("a");
    rt_string v1 = make_str("1");
    rt_string k2 = make_str("b");
    rt_string v2 = make_str("2");

    rt_bimap_put(bm, k1, v1);
    rt_bimap_put(bm, k2, v2);

    void *keys = rt_bimap_keys(bm);
    assert(rt_seq_len(keys) == 2);
    assert(rt_seq_cap(keys) == 2);

    void *vals = rt_bimap_values(bm);
    assert(rt_seq_len(vals) == 2);
    assert(rt_seq_cap(vals) == 2);

    rt_string_unref(k1);
    rt_string_unref(v1);
    rt_string_unref(k2);
    rt_string_unref(v2);
}

static void test_clear() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("a");
    rt_string v = make_str("b");
    rt_bimap_put(bm, k, v);

    rt_bimap_clear(bm);
    assert(rt_bimap_len(bm) == 0);
    assert(rt_bimap_is_empty(bm) == 1);
    assert(rt_bimap_has_key(bm, k) == 0);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_many_entries() {
    void *bm = rt_bimap_new();

    // Insert 100 entries to test resize
    char kbuf[16], vbuf[16];
    for (int i = 0; i < 100; ++i) {
        snprintf(kbuf, sizeof(kbuf), "key%d", i);
        snprintf(vbuf, sizeof(vbuf), "val%d", i);
        rt_string k = rt_string_from_bytes(kbuf, strlen(kbuf));
        rt_string v = rt_string_from_bytes(vbuf, strlen(vbuf));
        rt_bimap_put(bm, k, v);
        rt_string_unref(k);
        rt_string_unref(v);
    }

    assert(rt_bimap_len(bm) == 100);

    // Verify some lookups
    rt_string k50 = make_str("key50");
    rt_string v50 = make_str("val50");

    rt_string got = rt_bimap_get_by_key(bm, k50);
    assert(str_eq(got, "val50"));
    rt_string_unref(got);

    rt_string inv = rt_bimap_get_by_value(bm, v50);
    assert(str_eq(inv, "key50"));
    rt_string_unref(inv);

    rt_string_unref(k50);
    rt_string_unref(v50);
}

static void test_get_missing() {
    void *bm = rt_bimap_new();
    rt_string k = make_str("missing");
    rt_string got = rt_bimap_get_by_key(bm, k);
    assert(str_eq(got, ""));
    rt_string_unref(got);

    rt_string inv = rt_bimap_get_by_value(bm, k);
    assert(str_eq(inv, ""));
    rt_string_unref(inv);

    rt_string_unref(k);
}

static void test_embedded_nul_key_and_value() {
    void *bm = rt_bimap_new();
    const char key_bytes[] = {'k', '\0', '1'};
    const char value_bytes[] = {'v', '\0', '2'};
    rt_string k = make_bytes(key_bytes, sizeof(key_bytes));
    rt_string v = make_bytes(value_bytes, sizeof(value_bytes));

    rt_bimap_put(bm, k, v);
    assert(rt_bimap_len(bm) == 1);
    assert(rt_bimap_has_key(bm, k) == 1);
    assert(rt_bimap_has_value(bm, v) == 1);

    rt_string got = rt_bimap_get_by_key(bm, k);
    assert(str_bytes_eq(got, value_bytes, sizeof(value_bytes)));
    rt_string_unref(got);

    rt_string inv = rt_bimap_get_by_value(bm, v);
    assert(str_bytes_eq(inv, key_bytes, sizeof(key_bytes)));
    rt_string_unref(inv);

    assert(rt_bimap_remove_by_key(bm, k) == 1);
    assert(rt_bimap_len(bm) == 0);

    rt_string_unref(k);
    rt_string_unref(v);
}

static void test_null_safety() {
    assert(rt_bimap_len(NULL) == 0);
    assert(rt_bimap_is_empty(NULL) == 1);
    assert(rt_bimap_has_key(NULL, NULL) == 0);
    assert(rt_bimap_has_value(NULL, NULL) == 0);
    assert(rt_bimap_remove_by_key(NULL, NULL) == 0);
    assert(rt_bimap_remove_by_value(NULL, NULL) == 0);
}

int main() {
    test_new_empty();
    test_put_and_get();
    test_has_key_value();
    test_overwrite_key();
    test_overwrite_value();
    test_idempotent_and_two_sided_conflicts();
    test_remove_by_key();
    test_remove_by_value();
    test_keys_values();
    test_clear();
    test_many_entries();
    test_get_missing();
    test_embedded_nul_key_and_value();
    test_null_safety();

    return 0;
}
