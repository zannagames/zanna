//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTQueueTests.cpp
// Purpose: Comprehensive tests for Zanna.Collections.Queue FIFO collection.
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_queue.h"

#include <cassert>
#include <csetjmp>
#include <cstring>

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

static void test_new_and_basic_properties() {
    void *queue = rt_queue_new();
    assert(queue != nullptr);
    assert(rt_queue_len(queue) == 0);
    assert(rt_queue_is_empty(queue) == 1);
}

static void test_add_increases_length() {
    void *queue = rt_queue_new();

    int a = 10, b = 20, c = 30;
    rt_queue_push(queue, &a);
    assert(rt_queue_len(queue) == 1);
    assert(rt_queue_is_empty(queue) == 0);

    rt_queue_push(queue, &b);
    assert(rt_queue_len(queue) == 2);

    rt_queue_push(queue, &c);
    assert(rt_queue_len(queue) == 3);
}

static void test_fifo_order() {
    void *queue = rt_queue_new();

    int a = 10, b = 20, c = 30;
    rt_queue_push(queue, &a);
    rt_queue_push(queue, &b);
    rt_queue_push(queue, &c);

    // FIFO: first added should be taken first
    void *taken = rt_queue_pop(queue);
    assert(taken == &a);
    assert(rt_queue_len(queue) == 2);

    taken = rt_queue_pop(queue);
    assert(taken == &b);
    assert(rt_queue_len(queue) == 1);

    taken = rt_queue_pop(queue);
    assert(taken == &c);
    assert(rt_queue_len(queue) == 0);
    assert(rt_queue_is_empty(queue) == 1);
}

static void test_peek_returns_front_without_removing() {
    void *queue = rt_queue_new();

    int a = 10, b = 20;
    rt_queue_push(queue, &a);
    rt_queue_push(queue, &b);

    // Peek should return front element (first added)
    assert(rt_queue_peek(queue) == &a);
    // Length should be unchanged
    assert(rt_queue_len(queue) == 2);

    // Multiple peeks should return same value
    assert(rt_queue_peek(queue) == &a);
    assert(rt_queue_peek(queue) == &a);
    assert(rt_queue_len(queue) == 2);

    // Take and peek again
    rt_queue_pop(queue);
    assert(rt_queue_peek(queue) == &b);
    assert(rt_queue_len(queue) == 1);
}

static void test_clear_empties_queue() {
    void *queue = rt_queue_new();

    int a = 10, b = 20, c = 30;
    rt_queue_push(queue, &a);
    rt_queue_push(queue, &b);
    rt_queue_push(queue, &c);

    assert(rt_queue_len(queue) == 3);
    assert(rt_queue_is_empty(queue) == 0);

    rt_queue_clear(queue);

    assert(rt_queue_len(queue) == 0);
    assert(rt_queue_is_empty(queue) == 1);

    // Clear on already empty should be safe
    rt_queue_clear(queue);
    assert(rt_queue_len(queue) == 0);
}

static void test_add_after_clear() {
    void *queue = rt_queue_new();

    int a = 10, b = 20;
    rt_queue_push(queue, &a);
    rt_queue_push(queue, &b);
    rt_queue_clear(queue);

    int c = 30;
    rt_queue_push(queue, &c);
    assert(rt_queue_len(queue) == 1);
    assert(rt_queue_peek(queue) == &c);
}

static void test_wrap_around() {
    void *queue = rt_queue_new();

    // Add and take to move head/tail indices
    int vals[10];
    for (int i = 0; i < 10; ++i) {
        vals[i] = i;
        rt_queue_push(queue, &vals[i]);
    }
    for (int i = 0; i < 8; ++i) {
        void *taken = rt_queue_pop(queue);
        assert(taken == &vals[i]);
    }

    // Now head is at index 8, tail is at index 10
    // Add more to trigger wrap-around
    int more[10];
    for (int i = 0; i < 10; ++i) {
        more[i] = 100 + i;
        rt_queue_push(queue, &more[i]);
    }

    // Take remaining and verify FIFO order
    assert(rt_queue_pop(queue) == &vals[8]);
    assert(rt_queue_pop(queue) == &vals[9]);
    for (int i = 0; i < 10; ++i) {
        void *taken = rt_queue_pop(queue);
        assert(taken == &more[i]);
    }
    assert(rt_queue_is_empty(queue) == 1);
}

static void test_capacity_growth() {
    void *queue = rt_queue_new();

    // Add many elements to trigger capacity growth
    int vals[100];
    for (int i = 0; i < 100; ++i) {
        vals[i] = i;
        rt_queue_push(queue, &vals[i]);
    }

    assert(rt_queue_len(queue) == 100);

    // Verify FIFO order by taking all
    for (int i = 0; i < 100; ++i) {
        void *taken = rt_queue_pop(queue);
        assert(taken == &vals[i]);
    }

    assert(rt_queue_is_empty(queue) == 1);
}

static void test_growth_with_wrap_around() {
    void *queue = rt_queue_new();

    // Fill half, take half to move head
    int first[8];
    for (int i = 0; i < 8; ++i) {
        first[i] = i;
        rt_queue_push(queue, &first[i]);
    }
    for (int i = 0; i < 6; ++i) {
        rt_queue_pop(queue);
    }

    // Now add enough to trigger growth with wrapped data
    int second[20];
    for (int i = 0; i < 20; ++i) {
        second[i] = 100 + i;
        rt_queue_push(queue, &second[i]);
    }

    // Verify remaining first elements
    assert(rt_queue_pop(queue) == &first[6]);
    assert(rt_queue_pop(queue) == &first[7]);

    // Verify second elements
    for (int i = 0; i < 20; ++i) {
        void *taken = rt_queue_pop(queue);
        assert(taken == &second[i]);
    }

    assert(rt_queue_is_empty(queue) == 1);
}

static void test_null_handling() {
    // Operations on null should return safe defaults
    assert(rt_queue_len(nullptr) == 0);
    assert(rt_queue_is_empty(nullptr) == 1);

    // Clear on null should not crash
    rt_queue_clear(nullptr);
}

static void test_take_empty_traps() {
    void *queue = rt_queue_new();
    EXPECT_TRAP(rt_queue_pop(queue));

    // Also test after adding and taking
    int a = 10;
    rt_queue_push(queue, &a);
    rt_queue_pop(queue);
    EXPECT_TRAP(rt_queue_pop(queue));
}

static void test_peek_empty_traps() {
    void *queue = rt_queue_new();
    EXPECT_TRAP(rt_queue_peek(queue));

    // Also test after clear
    int a = 10;
    rt_queue_push(queue, &a);
    rt_queue_clear(queue);
    EXPECT_TRAP(rt_queue_peek(queue));
}

static void test_null_queue_traps() {
    int a = 10;

    EXPECT_TRAP(rt_queue_push(nullptr, &a));
    EXPECT_TRAP(rt_queue_pop(nullptr));
    EXPECT_TRAP(rt_queue_peek(nullptr));
}

static void test_add_null_value() {
    void *queue = rt_queue_new();

    // Adding null value should be allowed
    rt_queue_push(queue, nullptr);
    assert(rt_queue_len(queue) == 1);
    assert(rt_queue_peek(queue) == nullptr);
    assert(rt_queue_pop(queue) == nullptr);
    assert(rt_queue_is_empty(queue) == 1);
}

static void test_interleaved_operations() {
    void *queue = rt_queue_new();

    int a = 1, b = 2, c = 3, d = 4;

    rt_queue_push(queue, &a);
    rt_queue_push(queue, &b);
    assert(rt_queue_pop(queue) == &a);

    rt_queue_push(queue, &c);
    rt_queue_push(queue, &d);
    assert(rt_queue_peek(queue) == &b);
    assert(rt_queue_len(queue) == 3);

    assert(rt_queue_pop(queue) == &b);
    assert(rt_queue_pop(queue) == &c);
    assert(rt_queue_pop(queue) == &d);
    assert(rt_queue_is_empty(queue) == 1);
}

static void test_owns_elements_mode_releases_on_clear() {
    void *queue = rt_queue_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_queue_set_owns_elements(queue, 1);
    assert(rt_queue_owns_elements(queue) == 1);
    rt_queue_push(queue, value);
    release_obj(value); // Queue now owns the only reference.
    assert(g_finalizer_calls == 0);

    rt_queue_clear(queue);
    assert(g_finalizer_calls == 1);
    release_obj(queue);
}

static void test_owns_elements_pop_transfers_reference() {
    void *queue = rt_queue_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_queue_set_owns_elements(queue, 1);
    rt_queue_push(queue, value);
    release_obj(value); // Queue now owns the only reference.

    void *popped = rt_queue_pop(queue);
    assert(popped == value);
    assert(g_finalizer_calls == 0);
    release_obj(popped);
    assert(g_finalizer_calls == 1);
    release_obj(queue);
}

static void test_owns_elements_pops_transfer_saturated_references() {
    void *queue = rt_queue_new();
    void *first = new_obj();
    void *second = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(first, count_finalizer);
    rt_obj_set_finalizer(second, count_finalizer);

    rt_queue_set_owns_elements(queue, 1);
    rt_queue_push(queue, first);
    rt_queue_push(queue, second);
    release_obj(first);
    release_obj(second); // Queue now owns the only references.

    rt_heap_hdr_t *first_hdr = rt_heap_hdr(first);
    rt_heap_hdr_t *second_hdr = rt_heap_hdr(second);
    first_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    second_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    void *popped = rt_queue_pop(queue);
    assert(popped == first);
    assert(rt_queue_len(queue) == 1);

    void *tried = rt_queue_try_pop(queue);
    assert(tried == second);
    assert(rt_queue_len(queue) == 0);

    first_hdr->refcnt = 1;
    second_hdr->refcnt = 1;
    release_obj(popped);
    release_obj(tried);
    assert(g_finalizer_calls == 2);
    release_obj(queue);
}

static void test_owns_elements_clone_retains_values() {
    void *queue = rt_queue_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_queue_set_owns_elements(queue, 1);
    rt_queue_push(queue, value);
    release_obj(value); // Queue now owns the only reference.

    void *clone = rt_queue_clone(queue);
    assert(rt_queue_owns_elements(clone) == 1);

    rt_queue_clear(queue);
    assert(g_finalizer_calls == 0);
    rt_queue_clear(clone);
    assert(g_finalizer_calls == 1);
    release_obj(clone);
    release_obj(queue);
}

static void test_owns_elements_mode_change_non_empty_traps() {
    void *queue = rt_queue_new();
    int value = 42;
    rt_queue_push(queue, &value);
    EXPECT_TRAP(rt_queue_set_owns_elements(queue, 1));
    release_obj(queue);
}

static void test_has_value_equality() {
    // VDOC-086: Has uses the same boxed-value equality as List/Seq/Set, so
    // two separately boxed copies of a value are members of each other.
    void *q = rt_queue_new();
    void *a = rt_box_i64(42);
    rt_queue_push(q, a);
    void *b = rt_box_i64(42);
    assert(rt_queue_has(q, b) == 1);

    rt_string s1 = rt_string_from_bytes("same", 4);
    rt_string s2 = rt_string_from_bytes("same", 4);
    void *bs1 = rt_box_str(s1);
    void *bs2 = rt_box_str(s2);
    rt_queue_push(q, bs1);
    assert(rt_queue_has(q, bs2) == 1);

    void *c = rt_box_i64(43);
    assert(rt_queue_has(q, c) == 0);
}

int main() {
    test_has_value_equality();
    test_new_and_basic_properties();
    test_add_increases_length();
    test_fifo_order();
    test_peek_returns_front_without_removing();
    test_clear_empties_queue();
    test_add_after_clear();
    test_wrap_around();
    test_capacity_growth();
    test_growth_with_wrap_around();
    test_null_handling();
    test_take_empty_traps();
    test_peek_empty_traps();
    test_null_queue_traps();
    test_add_null_value();
    test_interleaved_operations();
    test_owns_elements_mode_releases_on_clear();
    test_owns_elements_pop_transfers_reference();
    test_owns_elements_pops_transfer_saturated_references();
    test_owns_elements_clone_retains_values();
    test_owns_elements_mode_change_non_empty_traps();

    return 0;
}
