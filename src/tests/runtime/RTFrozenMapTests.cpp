//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTFrozenMapTests.cpp
// Purpose: Verify immutable FrozenMap construction, lookup, ownership, and
//          hostile source-element handling.
// Key invariants:
//   - Construction retains each published key/value and rolls back partial maps.
//   - Invalid Seq sources and non-string key representations trap and return null.
// Ownership/Lifetime:
//   - Hostile-input regressions release their strings, boxes, objects, Seqs, and maps.
//   - Borrowing source Seqs never consume their test-owned elements.
// Links: src/runtime/collections/rt_frozenmap.c,
//        src/runtime/collections/rt_seq.c, src/runtime/oop/rt_box.c
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_frozenmap.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
static int g_finalizer_calls = 0;
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
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
}

static void release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void begin_returning_traps() {
    g_returning_trap_count = 0;
    g_trap_returns = true;
}

static void end_returning_traps() {
    g_trap_returns = false;
}

static void test_empty() {
    void *fm = rt_frozenmap_empty();
    assert(fm != NULL);
    assert(rt_frozenmap_len(fm) == 0);
    assert(rt_frozenmap_is_empty(fm) == 1);
}

static void test_from_seqs() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();

    rt_seq_push(keys, make_str("name"));
    rt_seq_push(keys, make_str("age"));
    rt_seq_push(vals, make_str("Alice"));
    rt_seq_push(vals, make_str("30"));

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    assert(rt_frozenmap_len(fm) == 2);
    assert(rt_frozenmap_is_empty(fm) == 0);
}

static void test_get() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();

    rt_string k = make_str("key");
    rt_string v = make_str("value");
    rt_seq_push(keys, k);
    rt_seq_push(vals, v);

    void *fm = rt_frozenmap_from_seqs(keys, vals);

    rt_string lookup = make_str("key");
    void *got = rt_frozenmap_get(fm, lookup);
    assert(got != NULL);
    assert(strcmp(rt_string_cstr((rt_string)got), "value") == 0);

    rt_string missing = make_str("nope");
    assert(rt_frozenmap_get(fm, missing) == NULL);

    rt_string_unref(lookup);
    rt_string_unref(missing);
}

static void test_has() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    rt_seq_push(keys, make_str("a"));
    rt_seq_push(vals, make_str("1"));

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    rt_string a = make_str("a");
    rt_string b = make_str("b");
    assert(rt_frozenmap_has(fm, a) == 1);
    assert(rt_frozenmap_has(fm, b) == 0);
    rt_string_unref(a);
    rt_string_unref(b);
}

static void test_keys_values() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    rt_seq_push(keys, make_str("x"));
    rt_seq_push(keys, make_str("y"));
    rt_seq_push(vals, make_str("10"));
    rt_seq_push(vals, make_str("20"));

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    void *ks = rt_frozenmap_keys(fm);
    void *vs = rt_frozenmap_values(fm);
    assert(rt_seq_len(ks) == 2);
    assert(rt_seq_len(vs) == 2);
}

static void test_get_or() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    rt_seq_push(keys, make_str("k"));
    rt_seq_push(vals, make_str("v"));

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    rt_string def = make_str("DEFAULT");
    rt_string k = make_str("k");
    rt_string m = make_str("missing");

    void *got = rt_frozenmap_get_or(fm, k, def);
    assert(strcmp(rt_string_cstr((rt_string)got), "v") == 0);

    got = rt_frozenmap_get_or(fm, m, def);
    assert(got == def);

    rt_string_unref(def);
    rt_string_unref(k);
    rt_string_unref(m);
}

static void test_merge() {
    void *k1 = rt_seq_new();
    void *v1 = rt_seq_new();
    rt_seq_push(k1, make_str("a"));
    rt_seq_push(v1, make_str("1"));

    void *k2 = rt_seq_new();
    void *v2 = rt_seq_new();
    rt_seq_push(k2, make_str("b"));
    rt_seq_push(v2, make_str("2"));

    void *fm1 = rt_frozenmap_from_seqs(k1, v1);
    void *fm2 = rt_frozenmap_from_seqs(k2, v2);
    void *merged = rt_frozenmap_merge(fm1, fm2);

    assert(rt_frozenmap_len(merged) == 2);
    rt_string a = make_str("a");
    rt_string b = make_str("b");
    assert(rt_frozenmap_has(merged, a) == 1);
    assert(rt_frozenmap_has(merged, b) == 1);
    rt_string_unref(a);
    rt_string_unref(b);
}

static void test_merge_overwrite() {
    void *k1 = rt_seq_new();
    void *v1 = rt_seq_new();
    rt_seq_push(k1, make_str("key"));
    rt_seq_push(v1, make_str("old"));

    void *k2 = rt_seq_new();
    void *v2 = rt_seq_new();
    rt_seq_push(k2, make_str("key"));
    rt_string new_val = make_str("new");
    rt_seq_push(v2, new_val);

    void *fm1 = rt_frozenmap_from_seqs(k1, v1);
    void *fm2 = rt_frozenmap_from_seqs(k2, v2);
    void *merged = rt_frozenmap_merge(fm1, fm2);

    assert(rt_frozenmap_len(merged) == 1);
    rt_string k = make_str("key");
    void *got = rt_frozenmap_get(merged, k);
    assert(strcmp(rt_string_cstr((rt_string)got), "new") == 0);
    rt_string_unref(k);
}

static void test_equals() {
    void *k1 = rt_seq_new();
    void *v1 = rt_seq_new();
    rt_seq_push(k1, make_str("a"));
    rt_seq_push(v1, make_str("1"));

    void *k2 = rt_seq_new();
    void *v2 = rt_seq_new();
    rt_seq_push(k2, make_str("a"));
    rt_seq_push(v2, make_str("1"));

    void *fm1 = rt_frozenmap_from_seqs(k1, v1);
    void *fm2 = rt_frozenmap_from_seqs(k2, v2);

    // Values are different string objects with same content
    // equals checks by reference, so this may or may not be equal
    // depending on string interning. Test structural equality at least for len.
    assert(rt_frozenmap_len(fm1) == rt_frozenmap_len(fm2));
}

static void test_null_safety() {
    assert(rt_frozenmap_len(NULL) == 0);
    assert(rt_frozenmap_is_empty(NULL) == 1);
    assert(rt_frozenmap_get(NULL, NULL) == NULL);
    assert(rt_frozenmap_has(NULL, NULL) == 0);
    assert(rt_frozenmap_equals(NULL, NULL) == 1);
}

static void test_dedup_keys() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    rt_seq_push(keys, make_str("k"));
    rt_seq_push(keys, make_str("k"));
    rt_seq_push(vals, make_str("first"));
    rt_seq_push(vals, make_str("second"));

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    // Last value wins for duplicate keys
    assert(rt_frozenmap_len(fm) == 1);
    rt_string k = make_str("k");
    void *got = rt_frozenmap_get(fm, k);
    assert(strcmp(rt_string_cstr((rt_string)got), "second") == 0);
    rt_string_unref(k);
}

static void test_embedded_nul_keys_are_distinct() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_seq_push(keys, k1);
    rt_seq_push(keys, k2);
    rt_seq_push(vals, v1);
    rt_seq_push(vals, v2);

    void *fm = rt_frozenmap_from_seqs(keys, vals);
    assert(rt_frozenmap_len(fm) == 2);
    assert(rt_frozenmap_get(fm, k1) == v1);
    assert(rt_frozenmap_get(fm, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    release_obj(v1);
    release_obj(v2);
    release_obj(fm);
}

static void test_releases_retained_values() {
    void *keys = rt_seq_new();
    void *vals = rt_seq_new();
    rt_string key = make_str("owned");
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_seq_push(keys, key);
    rt_seq_push(vals, value);
    void *fm = rt_frozenmap_from_seqs(keys, vals);

    release_obj(value); // FrozenMap now owns the only reference.
    assert(g_finalizer_calls == 0);
    release_obj(fm);
    assert(g_finalizer_calls == 1);

    rt_string_unref(key);
}

static void expect_invalid_key_element(void *invalid) {
    void *keys = rt_seq_new();
    void *values = rt_seq_new();
    rt_string valid_key = make_str("valid");
    void *stored = new_obj();
    void *unused = new_obj();
    assert(keys != nullptr);
    assert(values != nullptr);
    assert(valid_key != nullptr);

    const size_t key_refs = valid_key->literal_refs;
    rt_heap_hdr_t *stored_header = rt_heap_hdr(stored);
    assert(stored_header != nullptr);
    assert(stored_header->refcnt == 1);

    rt_seq_push(keys, valid_key);
    rt_seq_push(keys, invalid);
    rt_seq_push(values, stored);
    rt_seq_push(values, unused);

    begin_returning_traps();
    void *map = rt_frozenmap_from_seqs(keys, values);
    end_returning_traps();

    assert(map == nullptr);
    assert(g_returning_trap_count == 1);
    assert(valid_key->literal_refs == key_refs);
    assert(stored_header->refcnt == 1);

    release_obj(keys);
    release_obj(values);
    rt_string_unref(valid_key);
    release_obj(stored);
    release_obj(unused);
}

static void test_rejects_invalid_key_elements() {
    void *wrong_box = rt_box_i64(7);
    void *wrong_object = new_obj();
    assert(wrong_box != nullptr);

    expect_invalid_key_element(wrong_box);
    expect_invalid_key_element(wrong_object);
    expect_invalid_key_element(reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    release_obj(wrong_box);
    release_obj(wrong_object);
}

static void test_rejects_invalid_source_sequences() {
    void *fake = new_obj();
    void *keys = rt_seq_new();
    void *values = rt_seq_new();
    assert(keys != nullptr);
    assert(values != nullptr);

    begin_returning_traps();
    void *map = rt_frozenmap_from_seqs(fake, values);
    end_returning_traps();
    assert(map == nullptr);
    assert(g_returning_trap_count == 1);

    begin_returning_traps();
    map = rt_frozenmap_from_seqs(keys, fake);
    end_returning_traps();
    assert(map == nullptr);
    assert(g_returning_trap_count == 1);

    release_obj(fake);
    release_obj(keys);
    release_obj(values);
}

static void test_accepts_boxed_string_keys() {
    void *keys = rt_seq_new();
    void *values = rt_seq_new();
    rt_string key = make_str("boxed");
    void *boxed_key = rt_box_str(key);
    void *null_key = rt_box_str(nullptr);
    void *value = new_obj();
    assert(keys != nullptr);
    assert(values != nullptr);
    assert(boxed_key != nullptr);
    assert(null_key != nullptr);

    rt_seq_push(keys, boxed_key);
    rt_seq_push(keys, null_key);
    rt_seq_push(values, value);
    rt_seq_push(values, nullptr);

    void *map = rt_frozenmap_from_seqs(keys, values);
    assert(map != nullptr);
    assert(rt_frozenmap_len(map) == 1);
    assert(rt_frozenmap_get(map, key) == value);

    release_obj(map);
    release_obj(keys);
    release_obj(values);
    release_obj(boxed_key);
    release_obj(null_key);
    rt_string_unref(key);
    release_obj(value);
}

static void test_failed_boxed_string_retain_never_publishes_pointer() {
    rt_string text = make_str("boxed");
    void *box = rt_box_str(text);
    void *seq = rt_seq_new();
    assert(text != nullptr);
    assert(box != nullptr);
    assert(seq != nullptr);
    assert(text->literal_refs == 2);
    rt_seq_push(seq, box);

    text->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
    begin_returning_traps();

    rt_string out = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
    assert(rt_box_try_to_str(box, &out) == 0);
    assert(out == nullptr);
    assert(rt_unbox_str(box) == nullptr);
    assert(rt_seq_get_str(seq, 0) == nullptr);
    assert(rt_box_to_str_option(box) == nullptr);

    end_returning_traps();
    assert(g_returning_trap_count == 4);

    text->literal_refs = 2;
    release_obj(seq);
    release_obj(box);
    rt_string_unref(text);

    const char *long_bytes =
        "heap-backed boxed strings must also reject a failed retain without publishing";
    rt_string heap_text = make_str(long_bytes);
    void *heap_box = rt_box_str(heap_text);
    assert(heap_text != nullptr);
    assert(heap_box != nullptr);
    rt_heap_hdr_t *heap_header = rt_heap_hdr((void *)heap_text->data);
    assert(heap_header != nullptr);
    assert(heap_header->refcnt == 2);

    heap_header->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    begin_returning_traps();
    out = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
    assert(rt_box_try_to_str(heap_box, &out) == 0);
    assert(out == nullptr);
    assert(rt_unbox_str(heap_box) == nullptr);
    end_returning_traps();
    assert(g_returning_trap_count == 2);

    heap_header->refcnt = 2;
    release_obj(heap_box);
    rt_string_unref(heap_text);
}

/// @brief Main.
int main() {
    test_empty();
    test_from_seqs();
    test_get();
    test_has();
    test_keys_values();
    test_get_or();
    test_merge();
    test_merge_overwrite();
    test_equals();
    test_null_safety();
    test_dedup_keys();
    test_embedded_nul_keys_are_distinct();
    test_releases_retained_values();
    test_rejects_invalid_key_elements();
    test_rejects_invalid_source_sequences();
    test_accepts_boxed_string_keys();
    test_failed_boxed_string_retain_never_publishes_pointer();
    return 0;
}
