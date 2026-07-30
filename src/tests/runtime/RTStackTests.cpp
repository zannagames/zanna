//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTStackTests.cpp
// Purpose: Comprehensive tests for Zanna.Collections.Stack LIFO collection.
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_stack.h"

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
    void *stack = rt_stack_new();
    assert(stack != nullptr);
    assert(rt_stack_len(stack) == 0);
    assert(rt_stack_is_empty(stack) == 1);
}

static void test_push_increases_length() {
    void *stack = rt_stack_new();

    int a = 10, b = 20, c = 30;
    rt_stack_push(stack, &a);
    assert(rt_stack_len(stack) == 1);
    assert(rt_stack_is_empty(stack) == 0);

    rt_stack_push(stack, &b);
    assert(rt_stack_len(stack) == 2);

    rt_stack_push(stack, &c);
    assert(rt_stack_len(stack) == 3);
}

static void test_lifo_order() {
    void *stack = rt_stack_new();

    int a = 10, b = 20, c = 30;
    rt_stack_push(stack, &a);
    rt_stack_push(stack, &b);
    rt_stack_push(stack, &c);

    // LIFO: last pushed should be popped first
    void *popped = rt_stack_pop(stack);
    assert(popped == &c);
    assert(rt_stack_len(stack) == 2);

    popped = rt_stack_pop(stack);
    assert(popped == &b);
    assert(rt_stack_len(stack) == 1);

    popped = rt_stack_pop(stack);
    assert(popped == &a);
    assert(rt_stack_len(stack) == 0);
    assert(rt_stack_is_empty(stack) == 1);
}

static void test_peek_returns_top_without_removing() {
    void *stack = rt_stack_new();

    int a = 10, b = 20;
    rt_stack_push(stack, &a);
    rt_stack_push(stack, &b);

    // Peek should return top element
    assert(rt_stack_peek(stack) == &b);
    // Length should be unchanged
    assert(rt_stack_len(stack) == 2);

    // Multiple peeks should return same value
    assert(rt_stack_peek(stack) == &b);
    assert(rt_stack_peek(stack) == &b);
    assert(rt_stack_len(stack) == 2);

    // Pop and peek again
    rt_stack_pop(stack);
    assert(rt_stack_peek(stack) == &a);
    assert(rt_stack_len(stack) == 1);
}

static void test_clear_empties_stack() {
    void *stack = rt_stack_new();

    int a = 10, b = 20, c = 30;
    rt_stack_push(stack, &a);
    rt_stack_push(stack, &b);
    rt_stack_push(stack, &c);

    assert(rt_stack_len(stack) == 3);
    assert(rt_stack_is_empty(stack) == 0);

    rt_stack_clear(stack);

    assert(rt_stack_len(stack) == 0);
    assert(rt_stack_is_empty(stack) == 1);

    // Clear on already empty should be safe
    rt_stack_clear(stack);
    assert(rt_stack_len(stack) == 0);
}

static void test_push_after_clear() {
    void *stack = rt_stack_new();

    int a = 10, b = 20;
    rt_stack_push(stack, &a);
    rt_stack_push(stack, &b);
    rt_stack_clear(stack);

    int c = 30;
    rt_stack_push(stack, &c);
    assert(rt_stack_len(stack) == 1);
    assert(rt_stack_peek(stack) == &c);
}

static void test_capacity_growth() {
    void *stack = rt_stack_new();

    // Push many elements to trigger capacity growth
    int vals[100];
    for (int i = 0; i < 100; ++i) {
        vals[i] = i;
        rt_stack_push(stack, &vals[i]);
    }

    assert(rt_stack_len(stack) == 100);

    // Verify LIFO order by popping all
    for (int i = 99; i >= 0; --i) {
        void *popped = rt_stack_pop(stack);
        assert(popped == &vals[i]);
    }

    assert(rt_stack_is_empty(stack) == 1);
}

static void test_null_handling() {
    // Operations on null should return safe defaults
    assert(rt_stack_len(nullptr) == 0);
    assert(rt_stack_is_empty(nullptr) == 1);

    // Clear on null should not crash
    rt_stack_clear(nullptr);
}

static void test_pop_empty_traps() {
    void *stack = rt_stack_new();
    EXPECT_TRAP(rt_stack_pop(stack));

    // Also test after pushing and popping
    int a = 10;
    rt_stack_push(stack, &a);
    rt_stack_pop(stack);
    EXPECT_TRAP(rt_stack_pop(stack));
}

static void test_peek_empty_traps() {
    void *stack = rt_stack_new();
    EXPECT_TRAP(rt_stack_peek(stack));

    // Also test after clear
    int a = 10;
    rt_stack_push(stack, &a);
    rt_stack_clear(stack);
    EXPECT_TRAP(rt_stack_peek(stack));
}

static void test_null_stack_traps() {
    int a = 10;

    EXPECT_TRAP(rt_stack_push(nullptr, &a));
    EXPECT_TRAP(rt_stack_pop(nullptr));
    EXPECT_TRAP(rt_stack_peek(nullptr));
}

static void test_push_null_value() {
    void *stack = rt_stack_new();

    // Pushing null value should be allowed
    rt_stack_push(stack, nullptr);
    assert(rt_stack_len(stack) == 1);
    assert(rt_stack_peek(stack) == nullptr);
    assert(rt_stack_pop(stack) == nullptr);
    assert(rt_stack_is_empty(stack) == 1);
}

static void test_interleaved_operations() {
    void *stack = rt_stack_new();

    int a = 1, b = 2, c = 3, d = 4;

    rt_stack_push(stack, &a);
    rt_stack_push(stack, &b);
    assert(rt_stack_pop(stack) == &b);

    rt_stack_push(stack, &c);
    rt_stack_push(stack, &d);
    assert(rt_stack_peek(stack) == &d);
    assert(rt_stack_len(stack) == 3);

    assert(rt_stack_pop(stack) == &d);
    assert(rt_stack_pop(stack) == &c);
    assert(rt_stack_pop(stack) == &a);
    assert(rt_stack_is_empty(stack) == 1);
}

static void test_owns_elements_mode_releases_on_clear() {
    void *stack = rt_stack_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_stack_set_owns_elements(stack, 1);
    assert(rt_stack_owns_elements(stack) == 1);
    rt_stack_push(stack, value);
    release_obj(value); // Stack now owns the only reference.
    assert(g_finalizer_calls == 0);

    rt_stack_clear(stack);
    assert(g_finalizer_calls == 1);
    release_obj(stack);
}

static void test_owns_elements_pop_transfers_reference() {
    void *stack = rt_stack_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_stack_set_owns_elements(stack, 1);
    rt_stack_push(stack, value);
    release_obj(value); // Stack now owns the only reference.

    void *popped = rt_stack_pop(stack);
    assert(popped == value);
    assert(g_finalizer_calls == 0);
    release_obj(popped);
    assert(g_finalizer_calls == 1);
    release_obj(stack);
}

static void test_owns_elements_pops_transfer_saturated_references() {
    void *stack = rt_stack_new();
    void *bottom = new_obj();
    void *top = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(bottom, count_finalizer);
    rt_obj_set_finalizer(top, count_finalizer);

    rt_stack_set_owns_elements(stack, 1);
    rt_stack_push(stack, bottom);
    rt_stack_push(stack, top);
    release_obj(bottom);
    release_obj(top); // Stack now owns the only references.

    rt_heap_hdr_t *bottom_hdr = rt_heap_hdr(bottom);
    rt_heap_hdr_t *top_hdr = rt_heap_hdr(top);
    bottom_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    top_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    void *popped = rt_stack_pop(stack);
    assert(popped == top);
    assert(rt_stack_len(stack) == 1);

    void *tried = rt_stack_try_pop(stack);
    assert(tried == bottom);
    assert(rt_stack_len(stack) == 0);

    bottom_hdr->refcnt = 1;
    top_hdr->refcnt = 1;
    release_obj(popped);
    release_obj(tried);
    assert(g_finalizer_calls == 2);
    release_obj(stack);
}

static void test_owns_elements_clone_retains_values() {
    void *stack = rt_stack_new();
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_stack_set_owns_elements(stack, 1);
    rt_stack_push(stack, value);
    release_obj(value); // Stack now owns the only reference.

    void *clone = rt_stack_clone(stack);
    assert(rt_stack_owns_elements(clone) == 1);

    rt_stack_clear(stack);
    assert(g_finalizer_calls == 0);
    rt_stack_clear(clone);
    assert(g_finalizer_calls == 1);
    release_obj(clone);
    release_obj(stack);
}

static void test_owns_elements_mode_change_non_empty_traps() {
    void *stack = rt_stack_new();
    int value = 42;
    rt_stack_push(stack, &value);
    EXPECT_TRAP(rt_stack_set_owns_elements(stack, 1));
    release_obj(stack);
}

int main() {
    test_new_and_basic_properties();
    test_push_increases_length();
    test_lifo_order();
    test_peek_returns_top_without_removing();
    test_clear_empties_stack();
    test_push_after_clear();
    test_capacity_growth();
    test_null_handling();
    test_pop_empty_traps();
    test_peek_empty_traps();
    test_null_stack_traps();
    test_push_null_value();
    test_interleaved_operations();
    test_owns_elements_mode_releases_on_clear();
    test_owns_elements_pop_transfers_reference();
    test_owns_elements_pops_transfer_saturated_references();
    test_owns_elements_clone_retains_values();
    test_owns_elements_mode_change_non_empty_traps();

    return 0;
}
