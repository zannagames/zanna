//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSeqTests.cpp
// Purpose: Comprehensive tests for Zanna.Collections.Seq dynamic sequence.
// Key invariants: Sequence growth and mutation preserve ordering, bounds, and
//                 exactly-one retain/release ownership for managed elements.
// Ownership/Lifetime: Tests release every sequence and any separately owned
//                     managed value after trap recovery is cleared.
// Links: src/runtime/collections/rt_seq.c
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_context.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_random.h"
#include "rt_seq.h"

#include <cassert>
#include <csetjmp>
#include <cstring>

/// @brief Erase constness so borrowed text can travel through an opaque `void *`.
/// @details The runtime payload APIs take `void *`; these tests hand them
///          read-only bytes they own elsewhere and never write through the
///          result, so the cast is safe here.
/// @param text Borrowed NUL-terminated bytes.
/// @return The same address as a mutable `void *`.
static void *borrowedPayload(const char *text) {
    return const_cast<void *>(static_cast<const void *>(text));
}


namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static int g_finalizer_calls = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
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

static void count_finalizer(void *) {
    ++g_finalizer_calls;
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

static int8_t always_true(void *p);
static void *identity(void *p);

static void test_new_and_basic_properties() {
    void *seq = rt_seq_new();
    assert(seq != nullptr);
    assert(rt_seq_len(seq) == 0);
    assert(rt_seq_cap(seq) >= 1);
    assert(rt_seq_is_empty(seq) == 1);
}

static void test_with_capacity() {
    void *seq = rt_seq_with_capacity(100);
    assert(seq != nullptr);
    assert(rt_seq_len(seq) == 0);
    assert(rt_seq_cap(seq) >= 100);
    assert(rt_seq_is_empty(seq) == 1);

    // Minimum capacity is 1
    void *seq2 = rt_seq_with_capacity(0);
    assert(seq2 != nullptr);
    assert(rt_seq_cap(seq2) >= 1);

    void *seq3 = rt_seq_with_capacity(-10);
    assert(seq3 != nullptr);
    assert(rt_seq_cap(seq3) >= 1);
}

static void test_push_and_get() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    assert(rt_seq_len(seq) == 1);
    assert(rt_seq_is_empty(seq) == 0);
    assert(rt_seq_get(seq, 0) == &a);

    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    assert(rt_seq_len(seq) == 3);
    assert(rt_seq_get(seq, 0) == &a);
    assert(rt_seq_get(seq, 1) == &b);
    assert(rt_seq_get(seq, 2) == &c);
}

static void test_push_all_appends() {
    void *a = rt_seq_new();
    void *b = rt_seq_new();

    int v1 = 1;
    int v2 = 2;
    int v3 = 3;

    rt_seq_push(a, &v1);
    rt_seq_push(a, &v2);
    rt_seq_push(b, &v3);

    rt_seq_push_all(a, b);

    assert(rt_seq_len(a) == 3);
    assert(rt_seq_get(a, 0) == &v1);
    assert(rt_seq_get(a, 1) == &v2);
    assert(rt_seq_get(a, 2) == &v3);
}

static void test_push_all_self_doubles() {
    void *seq = rt_seq_new();

    int a = 10;
    int b = 20;

    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);

    rt_seq_push_all(seq, seq);

    assert(rt_seq_len(seq) == 4);
    assert(rt_seq_get(seq, 0) == &a);
    assert(rt_seq_get(seq, 1) == &b);
    assert(rt_seq_get(seq, 2) == &a);
    assert(rt_seq_get(seq, 3) == &b);
}

static void test_owns_elements_mode() {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);

    void *value = new_obj();
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_seq_push(seq, value);
    release_obj(value);
    assert(g_finalizer_calls == 0);

    rt_seq_clear(seq);
    assert(g_finalizer_calls == 1);

    void *non_empty = rt_seq_new();
    int x = 1;
    rt_seq_push(non_empty, &x);
    EXPECT_TRAP(rt_seq_set_owns_elements(non_empty, 1));
}

static void test_owned_seq_slices_and_filters_retain_elements() {
    void *seq = rt_seq_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_seq_set_owns_elements(seq, 1);
    rt_seq_push(seq, value);
    release_obj(value); // Seq now owns the only reference.

    void *clone = rt_seq_clone(seq);
    rt_seq_clear(seq);
    assert(g_finalizer_calls == 0);

    void *taken = rt_seq_take(clone, 1);
    rt_seq_clear(clone);
    assert(g_finalizer_calls == 0);

    void *kept = rt_seq_keep(taken, always_true);
    rt_seq_clear(taken);
    assert(g_finalizer_calls == 0);

    void *applied = rt_seq_apply(kept, identity);
    rt_seq_clear(kept);
    assert(g_finalizer_calls == 0);

    rt_seq_clear(applied);
    assert(g_finalizer_calls == 1);

    release_obj(applied);
    release_obj(kept);
    release_obj(taken);
    release_obj(clone);
    release_obj(seq);
}

static void test_set() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);

    assert(rt_seq_get(seq, 0) == &a);
    rt_seq_set(seq, 0, &c);
    assert(rt_seq_get(seq, 0) == &c);
}

/// @brief Verify raw self-replacement consumes the transferred reference.
/// @details An owning sequence and the caller each hold a reference to the
///          identical object before `SetRaw`. The operation must release the
///          old slot ownership even though pointer identity is unchanged,
///          leaving exactly the transferred reference in the slot.
static void test_owned_set_raw_same_pointer_consumes_transfer() {
    void *seq = rt_seq_new_owned();
    void *value = new_obj();
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_seq_push(seq, value);
    release_obj(value);
    assert(rt_heap_hdr(value)->refcnt == 1);

    rt_obj_retain_maybe(value);
    assert(rt_heap_hdr(value)->refcnt == 2);
    rt_seq_set_raw(seq, 0, value);
    assert(rt_heap_hdr(value)->refcnt == 1);

    rt_seq_clear(seq);
    assert(g_finalizer_calls == 1);
    release_obj(seq);
}

static void test_pop() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    assert(rt_seq_len(seq) == 3);
    void *popped = rt_seq_pop(seq);
    assert(popped == &c);
    assert(rt_seq_len(seq) == 2);

    popped = rt_seq_pop(seq);
    assert(popped == &b);
    assert(rt_seq_len(seq) == 1);

    popped = rt_seq_pop(seq);
    assert(popped == &a);
    assert(rt_seq_len(seq) == 0);
    assert(rt_seq_is_empty(seq) == 1);
}

static void test_owned_pop_transfers_saturated_reference() {
    void *seq = rt_seq_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_seq_set_owns_elements(seq, 1);
    rt_seq_push(seq, value);
    release_obj(value); // Seq now owns the only reference.

    rt_heap_hdr_t *hdr = rt_heap_hdr(value);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    void *popped = rt_seq_pop(seq);
    assert(popped == value);
    assert(rt_seq_len(seq) == 0);

    hdr->refcnt = 1;
    release_obj(popped);
    assert(g_finalizer_calls == 1);
    release_obj(seq);
}

static void test_owned_push_retain_overflow_keeps_capacity_and_length() {
    void *seq = rt_seq_with_capacity(1);
    rt_seq_set_owns_elements(seq, 1);

    int64_t cap_before = rt_seq_cap(seq);
    for (int64_t i = 0; i < cap_before; ++i) {
        void *filler = new_obj();
        rt_seq_push(seq, filler);
        release_obj(filler);
    }

    void *value = new_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(value);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    EXPECT_TRAP(rt_seq_push(seq, value));
    assert(rt_seq_len(seq) == cap_before);
    assert(rt_seq_cap(seq) == cap_before);

    hdr->refcnt = 1;
    release_obj(value);
    rt_seq_clear(seq);
    release_obj(seq);
}

static void test_owned_insert_retain_overflow_keeps_capacity_and_order() {
    void *seq = rt_seq_with_capacity(1);
    rt_seq_set_owns_elements(seq, 1);

    void *existing = new_obj();
    rt_seq_push(seq, existing);
    release_obj(existing);
    int64_t cap_before = rt_seq_cap(seq);

    void *value = new_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(value);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    EXPECT_TRAP(rt_seq_insert(seq, 0, value));
    assert(rt_seq_len(seq) == 1);
    assert(rt_seq_cap(seq) == cap_before);
    assert(rt_seq_get(seq, 0) == existing);

    hdr->refcnt = 1;
    release_obj(value);
    rt_seq_clear(seq);
    release_obj(seq);
}

static void test_peek() {
    void *seq = rt_seq_new();

    int a = 10, b = 20;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);

    assert(rt_seq_peek(seq) == &b);
    assert(rt_seq_len(seq) == 2); // peek doesn't remove

    rt_seq_pop(seq);
    assert(rt_seq_peek(seq) == &a);
}

static void test_first_and_last() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    assert(rt_seq_first(seq) == &a);
    assert(rt_seq_last(seq) == &c);

    // Single element
    void *seq2 = rt_seq_new();
    rt_seq_push(seq2, &b);
    assert(rt_seq_first(seq2) == &b);
    assert(rt_seq_last(seq2) == &b);
}

static void test_insert() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30, d = 40;

    // Insert at beginning of empty
    rt_seq_insert(seq, 0, &a);
    assert(rt_seq_len(seq) == 1);
    assert(rt_seq_get(seq, 0) == &a);

    // Insert at end
    rt_seq_insert(seq, 1, &c);
    assert(rt_seq_len(seq) == 2);
    assert(rt_seq_get(seq, 0) == &a);
    assert(rt_seq_get(seq, 1) == &c);

    // Insert in middle
    rt_seq_insert(seq, 1, &b);
    assert(rt_seq_len(seq) == 3);
    assert(rt_seq_get(seq, 0) == &a);
    assert(rt_seq_get(seq, 1) == &b);
    assert(rt_seq_get(seq, 2) == &c);

    // Insert at beginning
    rt_seq_insert(seq, 0, &d);
    assert(rt_seq_len(seq) == 4);
    assert(rt_seq_get(seq, 0) == &d);
    assert(rt_seq_get(seq, 1) == &a);
    assert(rt_seq_get(seq, 2) == &b);
    assert(rt_seq_get(seq, 3) == &c);
}

static void test_remove() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30, d = 40;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    rt_seq_push(seq, &d);

    // Remove from middle
    void *removed = rt_seq_remove(seq, 1);
    assert(removed == &b);
    assert(rt_seq_len(seq) == 3);
    assert(rt_seq_get(seq, 0) == &a);
    assert(rt_seq_get(seq, 1) == &c);
    assert(rt_seq_get(seq, 2) == &d);

    // Remove from beginning
    removed = rt_seq_remove(seq, 0);
    assert(removed == &a);
    assert(rt_seq_len(seq) == 2);
    assert(rt_seq_get(seq, 0) == &c);
    assert(rt_seq_get(seq, 1) == &d);

    // Remove from end
    removed = rt_seq_remove(seq, 1);
    assert(removed == &d);
    assert(rt_seq_len(seq) == 1);
    assert(rt_seq_get(seq, 0) == &c);
}

static void test_clear() {
    void *seq = rt_seq_new();

    int a = 10, b = 20;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);

    assert(rt_seq_len(seq) == 2);
    rt_seq_clear(seq);
    assert(rt_seq_len(seq) == 0);
    assert(rt_seq_is_empty(seq) == 1);

    // Clear on already empty
    rt_seq_clear(seq);
    assert(rt_seq_len(seq) == 0);
}

static void test_find_and_has() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30, d = 40;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    assert(rt_seq_find(seq, &a) == 0);
    assert(rt_seq_find(seq, &b) == 1);
    assert(rt_seq_find(seq, &c) == 2);
    assert(rt_seq_find(seq, &d) == -1); // not in sequence

    assert(rt_seq_has(seq, &a) == 1);
    assert(rt_seq_has(seq, &b) == 1);
    assert(rt_seq_has(seq, &c) == 1);
    assert(rt_seq_has(seq, &d) == 0);
}

static void test_reverse() {
    // Empty sequence
    void *seq0 = rt_seq_new();
    rt_seq_reverse(seq0); // should not crash
    assert(rt_seq_len(seq0) == 0);

    // Single element
    void *seq1 = rt_seq_new();
    int a = 10;
    rt_seq_push(seq1, &a);
    rt_seq_reverse(seq1);
    assert(rt_seq_get(seq1, 0) == &a);

    // Even number of elements
    void *seq2 = rt_seq_new();
    int b = 20, c = 30, d = 40;
    rt_seq_push(seq2, &a);
    rt_seq_push(seq2, &b);
    rt_seq_push(seq2, &c);
    rt_seq_push(seq2, &d);
    rt_seq_reverse(seq2);
    assert(rt_seq_get(seq2, 0) == &d);
    assert(rt_seq_get(seq2, 1) == &c);
    assert(rt_seq_get(seq2, 2) == &b);
    assert(rt_seq_get(seq2, 3) == &a);

    // Odd number of elements
    void *seq3 = rt_seq_new();
    rt_seq_push(seq3, &a);
    rt_seq_push(seq3, &b);
    rt_seq_push(seq3, &c);
    rt_seq_reverse(seq3);
    assert(rt_seq_get(seq3, 0) == &c);
    assert(rt_seq_get(seq3, 1) == &b);
    assert(rt_seq_get(seq3, 2) == &a);
}

static void test_shuffle_deterministic() {
    RtContext ctx;
    rt_context_init(&ctx);
    rt_set_current_context(&ctx);

    void *seq = rt_seq_new();
    int vals[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; ++i)
        rt_seq_push(seq, &vals[i]);

    rt_randomize_i64(1);
    rt_seq_shuffle(seq);

    // Expected Fisher–Yates result with seed=1 and LCG in rt_rand_int.
    // Original order: [1,2,3,4,5] -> indices [2,4,0,3,1]
    assert(rt_seq_len(seq) == 5);
    assert(rt_seq_get(seq, 0) == &vals[2]);
    assert(rt_seq_get(seq, 1) == &vals[4]);
    assert(rt_seq_get(seq, 2) == &vals[0]);
    assert(rt_seq_get(seq, 3) == &vals[3]);
    assert(rt_seq_get(seq, 4) == &vals[1]);

    // Verify it's a permutation of the original pointers.
    for (int i = 0; i < 5; ++i)
        assert(rt_seq_has(seq, &vals[i]) == 1);

    rt_set_current_context(nullptr);
    rt_context_cleanup(&ctx);
}

static void test_slice() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30, d = 40, e = 50;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    rt_seq_push(seq, &d);
    rt_seq_push(seq, &e);

    // Normal slice
    void *slice1 = rt_seq_slice(seq, 1, 4);
    assert(rt_seq_len(slice1) == 3);
    assert(rt_seq_get(slice1, 0) == &b);
    assert(rt_seq_get(slice1, 1) == &c);
    assert(rt_seq_get(slice1, 2) == &d);

    // Slice from beginning
    void *slice2 = rt_seq_slice(seq, 0, 2);
    assert(rt_seq_len(slice2) == 2);
    assert(rt_seq_get(slice2, 0) == &a);
    assert(rt_seq_get(slice2, 1) == &b);

    // Slice to end
    void *slice3 = rt_seq_slice(seq, 3, 5);
    assert(rt_seq_len(slice3) == 2);
    assert(rt_seq_get(slice3, 0) == &d);
    assert(rt_seq_get(slice3, 1) == &e);

    // Clamped start (negative)
    void *slice4 = rt_seq_slice(seq, -5, 2);
    assert(rt_seq_len(slice4) == 2);
    assert(rt_seq_get(slice4, 0) == &a);
    assert(rt_seq_get(slice4, 1) == &b);

    // Clamped end (beyond length)
    void *slice5 = rt_seq_slice(seq, 3, 100);
    assert(rt_seq_len(slice5) == 2);
    assert(rt_seq_get(slice5, 0) == &d);
    assert(rt_seq_get(slice5, 1) == &e);

    // Empty slice (start >= end)
    void *slice6 = rt_seq_slice(seq, 3, 2);
    assert(rt_seq_len(slice6) == 0);

    void *slice7 = rt_seq_slice(seq, 3, 3);
    assert(rt_seq_len(slice7) == 0);
}

static void test_clone() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    void *cloned = rt_seq_clone(seq);
    assert(cloned != seq);
    assert(rt_seq_len(cloned) == 3);
    assert(rt_seq_get(cloned, 0) == &a);
    assert(rt_seq_get(cloned, 1) == &b);
    assert(rt_seq_get(cloned, 2) == &c);

    // Modifying original doesn't affect clone
    int d = 40;
    rt_seq_push(seq, &d);
    assert(rt_seq_len(seq) == 4);
    assert(rt_seq_len(cloned) == 3);

    // Clone empty
    void *empty = rt_seq_new();
    void *cloned_empty = rt_seq_clone(empty);
    assert(rt_seq_len(cloned_empty) == 0);
}

static void test_capacity_growth() {
    void *seq = rt_seq_with_capacity(2);
    int64_t initial_cap = rt_seq_cap(seq);

    int vals[100];
    for (int i = 0; i < 100; ++i) {
        vals[i] = i;
        rt_seq_push(seq, &vals[i]);
    }

    assert(rt_seq_len(seq) == 100);
    assert(rt_seq_cap(seq) > initial_cap);

    // Verify all elements
    for (int i = 0; i < 100; ++i) {
        assert(rt_seq_get(seq, i) == &vals[i]);
    }
}

static void test_null_handling() {
    // Most operations on null should return safe defaults or 0
    assert(rt_seq_len(nullptr) == 0);
    assert(rt_seq_cap(nullptr) == 0);
    assert(rt_seq_is_empty(nullptr) == 1);
    assert(rt_seq_find(nullptr, nullptr) == -1);
    assert(rt_seq_has(nullptr, nullptr) == 0);

    // Clear on null should not crash
    rt_seq_clear(nullptr);

    // Reverse on null should not crash
    rt_seq_reverse(nullptr);

    // Slice on null returns new empty seq
    void *slice = rt_seq_slice(nullptr, 0, 10);
    assert(slice != nullptr);
    assert(rt_seq_len(slice) == 0);

    // Clone on null returns new empty seq
    void *cloned = rt_seq_clone(nullptr);
    assert(cloned != nullptr);
    assert(rt_seq_len(cloned) == 0);
}

static void test_bounds_errors() {
    void *seq = rt_seq_new();
    int a = 10;
    rt_seq_push(seq, &a);

    // Get out of bounds
    EXPECT_TRAP(rt_seq_get(seq, 1));
    EXPECT_TRAP(rt_seq_get(seq, -1));

    // Set out of bounds
    EXPECT_TRAP(rt_seq_set(seq, 1, &a));
    EXPECT_TRAP(rt_seq_set(seq, -1, &a));

    // Remove out of bounds
    EXPECT_TRAP(rt_seq_remove(seq, 1));
    EXPECT_TRAP(rt_seq_remove(seq, -1));

    // Insert out of bounds
    EXPECT_TRAP(rt_seq_insert(seq, 2, &a));
    EXPECT_TRAP(rt_seq_insert(seq, -1, &a));

    // Pop empty
    rt_seq_pop(seq);
    EXPECT_TRAP(rt_seq_pop(seq));

    // Peek empty
    EXPECT_TRAP(rt_seq_peek(seq));

    // First/Last empty
    EXPECT_TRAP(rt_seq_first(seq));
    EXPECT_TRAP(rt_seq_last(seq));
}

static void test_null_seq_errors() {
    int a = 10;

    EXPECT_TRAP(rt_seq_get(nullptr, 0));
    EXPECT_TRAP(rt_seq_set(nullptr, 0, &a));
    EXPECT_TRAP(rt_seq_push(nullptr, &a));
    EXPECT_TRAP(rt_seq_pop(nullptr));
    EXPECT_TRAP(rt_seq_peek(nullptr));
    EXPECT_TRAP(rt_seq_first(nullptr));
    EXPECT_TRAP(rt_seq_last(nullptr));
    EXPECT_TRAP(rt_seq_insert(nullptr, 0, &a));
    EXPECT_TRAP(rt_seq_remove(nullptr, 0));
}

//=============================================================================
// Sort tests
//=============================================================================

static void test_sort_strings() {
    void *seq = rt_seq_new();

    // Create string handles for testing (using raw pointers as mock strings)
    const char *s1 = "cherry";
    const char *s2 = "apple";
    const char *s3 = "banana";
    const char *s4 = "date";

    rt_seq_push(seq, borrowedPayload(s1));
    rt_seq_push(seq, borrowedPayload(s2));
    rt_seq_push(seq, borrowedPayload(s3));
    rt_seq_push(seq, borrowedPayload(s4));

    // Sort should order by pointer value for non-string objects
    rt_seq_sort(seq);

    // After sorting, elements should be in some consistent order
    assert(rt_seq_len(seq) == 4);
    // All original elements should still be present
    assert(rt_seq_has(seq, borrowedPayload(s1)));
    assert(rt_seq_has(seq, borrowedPayload(s2)));
    assert(rt_seq_has(seq, borrowedPayload(s3)));
    assert(rt_seq_has(seq, borrowedPayload(s4)));
}

static void test_sort_empty() {
    void *seq = rt_seq_new();
    rt_seq_sort(seq); // Should not crash
    assert(rt_seq_len(seq) == 0);
}

static void test_sort_single() {
    void *seq = rt_seq_new();
    int a = 10;
    rt_seq_push(seq, &a);
    rt_seq_sort(seq);
    assert(rt_seq_len(seq) == 1);
    assert(rt_seq_get(seq, 0) == &a);
}

static void test_sort_boxed_values() {
    // VDOC-089: the default sort recognizes boxed strings and integers (the
    // representations produced by the public Push surface), matching List.
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    rt_seq_push(seq, rt_box_str(rt_string_from_bytes("Charlie", 7)));
    rt_seq_push(seq, rt_box_str(rt_string_from_bytes("Alice", 5)));
    rt_seq_push(seq, rt_box_str(rt_string_from_bytes("Bob", 3)));
    rt_seq_sort(seq);

    rt_string first = rt_unbox_str(rt_seq_get(seq, 0));
    rt_string second = rt_unbox_str(rt_seq_get(seq, 1));
    rt_string third = rt_unbox_str(rt_seq_get(seq, 2));
    assert(strcmp(rt_string_cstr(first), "Alice") == 0);
    assert(strcmp(rt_string_cstr(second), "Bob") == 0);
    assert(strcmp(rt_string_cstr(third), "Charlie") == 0);

    void *nums = rt_seq_new();
    rt_seq_set_owns_elements(nums, 1);
    rt_seq_push(nums, rt_box_i64(30));
    rt_seq_push(nums, rt_box_i64(10));
    rt_seq_push(nums, rt_box_i64(20));
    rt_seq_sort(nums);
    assert(rt_unbox_i64(rt_seq_get(nums, 0)) == 10);
    assert(rt_unbox_i64(rt_seq_get(nums, 1)) == 20);
    assert(rt_unbox_i64(rt_seq_get(nums, 2)) == 30);

    // Mixed types rank by class: numeric < string, deterministically.
    void *mixed = rt_seq_new();
    rt_seq_set_owns_elements(mixed, 1);
    rt_seq_push(mixed, rt_box_str(rt_string_from_bytes("zzz", 3)));
    rt_seq_push(mixed, rt_box_i64(5));
    rt_seq_sort(mixed);
    assert(rt_box_type(rt_seq_get(mixed, 0)) == RT_BOX_I64);
    assert(rt_box_type(rt_seq_get(mixed, 1)) == RT_BOX_STR);
}

static void test_sort_null() {
    rt_seq_sort(nullptr);      // Should not crash
    rt_seq_sort_desc(nullptr); // Should not crash
}

static void test_sort_desc() {
    void *seq = rt_seq_new();

    int a = 10, b = 20, c = 30;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    rt_seq_sort_desc(seq);

    // After desc sort, order should be reversed from ascending
    assert(rt_seq_len(seq) == 3);
    assert(rt_seq_has(seq, &a));
    assert(rt_seq_has(seq, &b));
    assert(rt_seq_has(seq, &c));
}

//=============================================================================
// Take and Drop tests
//=============================================================================

static void test_take() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3, d = 4, e = 5;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    rt_seq_push(seq, &d);
    rt_seq_push(seq, &e);

    // Take 3 elements
    void *taken = rt_seq_take(seq, 3);
    assert(rt_seq_len(taken) == 3);
    assert(rt_seq_get(taken, 0) == &a);
    assert(rt_seq_get(taken, 1) == &b);
    assert(rt_seq_get(taken, 2) == &c);

    // Original unchanged
    assert(rt_seq_len(seq) == 5);

    // Take 0 elements
    void *taken0 = rt_seq_take(seq, 0);
    assert(rt_seq_len(taken0) == 0);

    // Take negative
    void *taken_neg = rt_seq_take(seq, -5);
    assert(rt_seq_len(taken_neg) == 0);

    // Take more than length
    void *taken_all = rt_seq_take(seq, 100);
    assert(rt_seq_len(taken_all) == 5);

    // Take from null
    void *taken_null = rt_seq_take(nullptr, 3);
    assert(rt_seq_len(taken_null) == 0);
}

static void test_drop() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3, d = 4, e = 5;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    rt_seq_push(seq, &d);
    rt_seq_push(seq, &e);

    // Drop 2 elements
    void *dropped = rt_seq_drop(seq, 2);
    assert(rt_seq_len(dropped) == 3);
    assert(rt_seq_get(dropped, 0) == &c);
    assert(rt_seq_get(dropped, 1) == &d);
    assert(rt_seq_get(dropped, 2) == &e);

    // Original unchanged
    assert(rt_seq_len(seq) == 5);

    // Drop 0 elements (clone)
    void *dropped0 = rt_seq_drop(seq, 0);
    assert(rt_seq_len(dropped0) == 5);

    // Drop negative (clone)
    void *dropped_neg = rt_seq_drop(seq, -5);
    assert(rt_seq_len(dropped_neg) == 5);

    // Drop more than length
    void *dropped_all = rt_seq_drop(seq, 100);
    assert(rt_seq_len(dropped_all) == 0);

    // Drop from null
    void *dropped_null = rt_seq_drop(nullptr, 3);
    assert(rt_seq_len(dropped_null) == 0);
}

//=============================================================================
// Functional operations tests (C API level)
//=============================================================================

// Predicate: returns true if pointer value is even (for testing)
static int8_t is_even_ptr(void *p) {
    return ((intptr_t)p % 2) == 0 ? 1 : 0;
}

// Predicate: always true
static int8_t always_true(void *p) {
    (void)p;
    return 1;
}

// Predicate: always false
static int8_t always_false(void *p) {
    (void)p;
    return 0;
}

// Transform: returns the same pointer (identity)
static void *identity(void *p) {
    return p;
}

// Reducer: just returns the element (for testing fold)
static void *take_second(void *acc, void *elem) {
    (void)acc;
    return elem;
}

static void test_keep() {
    void *seq = rt_seq_new();
    int a = 2, b = 3, c = 4, d = 5, e = 6;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);
    rt_seq_push(seq, &d);
    rt_seq_push(seq, &e);

    // Keep with always_true returns clone
    void *all = rt_seq_keep(seq, always_true);
    assert(rt_seq_len(all) == 5);

    // Keep with always_false returns empty
    void *none = rt_seq_keep(seq, always_false);
    assert(rt_seq_len(none) == 0);

    // Keep with null pred returns clone
    void *cloned = rt_seq_keep(seq, nullptr);
    assert(rt_seq_len(cloned) == 5);

    // Keep from null returns empty
    void *from_null = rt_seq_keep(nullptr, always_true);
    assert(rt_seq_len(from_null) == 0);
}

static void test_reject() {
    void *seq = rt_seq_new();
    int a = 2, b = 3, c = 4;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    // Reject with always_true returns empty
    void *none = rt_seq_reject(seq, always_true);
    assert(rt_seq_len(none) == 0);

    // Reject with always_false returns clone
    void *all = rt_seq_reject(seq, always_false);
    assert(rt_seq_len(all) == 3);

    // Reject from null returns empty
    void *from_null = rt_seq_reject(nullptr, always_true);
    assert(rt_seq_len(from_null) == 0);
}

static void test_apply() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    // Apply identity returns equivalent seq
    void *applied = rt_seq_apply(seq, identity);
    assert(rt_seq_len(applied) == 3);
    assert(rt_seq_get(applied, 0) == &a);
    assert(rt_seq_get(applied, 1) == &b);
    assert(rt_seq_get(applied, 2) == &c);

    // Apply with null fn returns clone
    void *cloned = rt_seq_apply(seq, nullptr);
    assert(rt_seq_len(cloned) == 3);

    // Apply to null returns empty
    void *from_null = rt_seq_apply(nullptr, identity);
    assert(rt_seq_len(from_null) == 0);
}

static void test_all_any_none() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    // All with always_true
    assert(rt_seq_all(seq, always_true) == 1);

    // All with always_false
    assert(rt_seq_all(seq, always_false) == 0);

    // Any with always_true
    assert(rt_seq_any(seq, always_true) == 1);

    // Any with always_false
    assert(rt_seq_any(seq, always_false) == 0);

    // None with always_true
    assert(rt_seq_none(seq, always_true) == 0);

    // None with always_false
    assert(rt_seq_none(seq, always_false) == 1);

    // Empty sequence
    void *empty = rt_seq_new();
    assert(rt_seq_all(empty, always_true) == 1);  // vacuous truth
    assert(rt_seq_any(empty, always_true) == 0);  // no elements
    assert(rt_seq_none(empty, always_true) == 1); // no elements

    // Null handling
    assert(rt_seq_all(nullptr, always_true) == 1);
    assert(rt_seq_any(nullptr, always_true) == 0);
    assert(rt_seq_none(nullptr, always_true) == 1);
    assert(rt_seq_all(seq, nullptr) == 1);
    assert(rt_seq_any(seq, nullptr) == 0);
    assert(rt_seq_none(seq, nullptr) == 1);
}

static void test_count_where() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    assert(rt_seq_count_where(seq, always_true) == 3);
    assert(rt_seq_count_where(seq, always_false) == 0);
    assert(rt_seq_count_where(seq, nullptr) == 3); // null pred returns len
    assert(rt_seq_count_where(nullptr, always_true) == 0);
}

static void test_find_where() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    assert(rt_seq_find_where(seq, always_true) == &a); // first element
    assert(rt_seq_find_where(seq, always_false) == nullptr);
    assert(rt_seq_find_where(seq, nullptr) == &a); // null pred returns first
    assert(rt_seq_find_where(nullptr, always_true) == nullptr);

    // Empty sequence
    void *empty = rt_seq_new();
    assert(rt_seq_find_where(empty, always_true) == nullptr);
}

static void test_take_while_drop_while() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    // TakeWhile with always_true takes all
    void *all = rt_seq_take_while(seq, always_true);
    assert(rt_seq_len(all) == 3);

    // TakeWhile with always_false takes none
    void *none_tw = rt_seq_take_while(seq, always_false);
    assert(rt_seq_len(none_tw) == 0);

    // DropWhile with always_true drops all
    void *none_dw = rt_seq_drop_while(seq, always_true);
    assert(rt_seq_len(none_dw) == 0);

    // DropWhile with always_false keeps all
    void *all_dw = rt_seq_drop_while(seq, always_false);
    assert(rt_seq_len(all_dw) == 3);

    // Null handling
    assert(rt_seq_len(rt_seq_take_while(nullptr, always_true)) == 0);
    assert(rt_seq_len(rt_seq_drop_while(nullptr, always_true)) == 0);
    assert(rt_seq_len(rt_seq_take_while(seq, nullptr)) == 3); // null pred = clone
    assert(rt_seq_len(rt_seq_drop_while(seq, nullptr)) == 0); // null pred = empty
}

static void test_fold() {
    void *seq = rt_seq_new();
    int a = 1, b = 2, c = 3;
    rt_seq_push(seq, &a);
    rt_seq_push(seq, &b);
    rt_seq_push(seq, &c);

    int init = 0;
    // Fold with take_second returns last element
    void *result = rt_seq_fold(seq, &init, take_second);
    assert(result == &c);

    // Empty seq returns init
    void *empty = rt_seq_new();
    result = rt_seq_fold(empty, &init, take_second);
    assert(result == &init);

    // Null seq returns init
    result = rt_seq_fold(nullptr, &init, take_second);
    assert(result == &init);

    // Null fn returns init
    result = rt_seq_fold(seq, &init, nullptr);
    assert(result == &init);
}

int main() {
    test_new_and_basic_properties();
    test_with_capacity();
    test_push_and_get();
    test_push_all_appends();
    test_push_all_self_doubles();
    test_owns_elements_mode();
    test_owned_seq_slices_and_filters_retain_elements();
    test_set();
    test_owned_set_raw_same_pointer_consumes_transfer();
    test_pop();
    test_owned_pop_transfers_saturated_reference();
    test_owned_push_retain_overflow_keeps_capacity_and_length();
    test_owned_insert_retain_overflow_keeps_capacity_and_order();
    test_peek();
    test_first_and_last();
    test_insert();
    test_remove();
    test_clear();
    test_find_and_has();
    test_reverse();
    test_shuffle_deterministic();
    test_slice();
    test_clone();
    test_capacity_growth();
    test_null_handling();
    test_bounds_errors();
    test_null_seq_errors();

    // Sort tests
    test_sort_strings();
    test_sort_empty();
    test_sort_single();
    test_sort_boxed_values();
    test_sort_null();
    test_sort_desc();

    // Take and Drop tests
    test_take();
    test_drop();

    // Functional operations tests
    test_keep();
    test_reject();
    test_apply();
    test_all_any_none();
    test_count_where();
    test_find_where();
    test_take_while_drop_while();
    test_fold();

    return 0;
}
