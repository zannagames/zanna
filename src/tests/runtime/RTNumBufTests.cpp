//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTNumBufTests.cpp
// Purpose: Runtime coverage for packed F64Buffer and I64Buffer collections.
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_list.h"
#include "rt_numbuf.h"
#include "rt_object.h"
#include "rt_seq.h"

#include "rt_trap.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

void *g_trap_a = nullptr;
void *g_trap_b = nullptr;

void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

bool near(double a, double b) {
    return std::fabs(a - b) < 0.0000001;
}

void expect_trap(void (*fn)(), const char *message) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn();
        rt_trap_clear_recovery();
        assert(false && "expected trap");
    }
    std::string text = rt_trap_get_error();
    rt_trap_clear_recovery();
    assert(text.find(message) != std::string::npos);
}

void test_f64_buffer_core_ops() {
    void *buf = rt_f64buf_new(4);
    assert(rt_obj_class_id(buf) == RT_F64BUFFER_CLASS_ID);
    assert(rt_f64buf_len(buf) == 4);

    rt_f64buf_set(buf, 0, 1.5);
    rt_f64buf_set(buf, 1, -2.0);
    rt_f64buf_set(buf, 2, 4.0);
    rt_f64buf_set(buf, 3, 8.5);
    assert(near(rt_f64buf_get(buf, 0), 1.5));
    assert(near(rt_f64buf_get(buf, 3), 8.5));
    assert(near(rt_f64buf_sum(buf), 12.0));
    assert(near(rt_f64buf_min(buf), -2.0));
    assert(near(rt_f64buf_max(buf), 8.5));

    rt_f64buf_add_scalar(buf, 0.5);
    assert(near(rt_f64buf_get(buf, 0), 2.0));
    rt_f64buf_mul_scalar(buf, 2.0);
    assert(near(rt_f64buf_get(buf, 1), -3.0));

    void *other = rt_f64buf_new(4);
    rt_f64buf_fill(other, 1.0);
    rt_f64buf_add_buffer(buf, other);
    assert(near(rt_f64buf_get(buf, 2), 10.0));
    assert(near(rt_f64buf_dot(buf, other), rt_f64buf_sum(buf)));

    void *copy = rt_f64buf_new(0);
    rt_f64buf_copy_from(copy, buf);
    assert(rt_f64buf_len(copy) == 4);
    assert(near(rt_f64buf_get(copy, 3), 19.0));

    release_obj(copy);
    release_obj(other);
    release_obj(buf);
}

void test_i64_buffer_core_ops() {
    void *buf = rt_i64buf_new(5);
    assert(rt_obj_class_id(buf) == RT_I64BUFFER_CLASS_ID);
    assert(rt_i64buf_len(buf) == 5);

    for (int64_t i = 0; i < rt_i64buf_len(buf); i++)
        rt_i64buf_set(buf, i, i + 1);
    assert(rt_i64buf_sum(buf) == 15);
    assert(rt_i64buf_min(buf) == 1);
    assert(rt_i64buf_max(buf) == 5);

    rt_i64buf_add_scalar(buf, 2);
    rt_i64buf_mul_scalar(buf, 3);
    assert(rt_i64buf_get(buf, 0) == 9);
    assert(rt_i64buf_get(buf, 4) == 21);

    void *other = rt_i64buf_new(5);
    rt_i64buf_fill(other, 2);
    rt_i64buf_add_buffer(buf, other);
    assert(rt_i64buf_get(buf, 2) == 17);
    assert(rt_i64buf_dot(buf, other) == rt_i64buf_sum(buf) * 2);

    void *copy = rt_i64buf_new(1);
    rt_i64buf_copy_from(copy, buf);
    assert(rt_i64buf_len(copy) == 5);
    assert(rt_i64buf_get(copy, 4) == 23);

    release_obj(copy);
    release_obj(other);
    release_obj(buf);
}

void test_slice_is_independent_copy() {
    void *buf = rt_f64buf_new(6);
    for (int64_t i = 0; i < rt_f64buf_len(buf); i++)
        rt_f64buf_set(buf, i, (double)i + 0.25);

    void *slice = rt_f64buf_slice(buf, 2, 5);
    assert(rt_f64buf_len(slice) == 3);
    assert(near(rt_f64buf_get(slice, 0), 2.25));
    assert(near(rt_f64buf_get(slice, 2), 4.25));

    rt_f64buf_set(slice, 0, 99.0);
    assert(near(rt_f64buf_get(buf, 2), 2.25));
    assert(near(rt_f64buf_get(slice, 0), 99.0));

    void *empty = rt_f64buf_slice(buf, 10, 2);
    assert(rt_f64buf_len(empty) == 0);

    release_obj(empty);
    release_obj(slice);
    release_obj(buf);
}

void test_to_seq_from_seq_round_trip() {
    void *src = rt_f64buf_new(3);
    rt_f64buf_set(src, 0, 1.25);
    rt_f64buf_set(src, 1, 2.5);
    rt_f64buf_set(src, 2, 3.75);

    void *seq = rt_f64buf_to_seq(src);
    assert(rt_seq_len(seq) == 3);
    assert(near(rt_unbox_f64(rt_seq_get(seq, 1)), 2.5));

    void *round_trip = rt_f64buf_from_seq(seq);
    assert(rt_f64buf_len(round_trip) == 3);
    assert(near(rt_f64buf_get(round_trip, 2), 3.75));

    void *ints = rt_seq_new_owned();
    void *a = rt_box_i64(7);
    void *b = rt_box_i64(11);
    rt_seq_push(ints, a);
    rt_seq_push(ints, b);
    release_obj(a);
    release_obj(b);

    void *ibuf = rt_i64buf_from_seq(ints);
    assert(rt_i64buf_len(ibuf) == 2);
    assert(rt_i64buf_get(ibuf, 0) == 7);
    assert(rt_i64buf_get(ibuf, 1) == 11);

    release_obj(ibuf);
    release_obj(ints);
    release_obj(round_trip);
    release_obj(seq);
    release_obj(src);
}

void test_to_list_preserves_order_and_values() {
    void *src = rt_i64buf_new(3);
    rt_i64buf_set(src, 0, 10);
    rt_i64buf_set(src, 1, 20);
    rt_i64buf_set(src, 2, 30);

    void *list = rt_i64buf_to_list(src);
    assert(rt_list_len(list) == 3);
    void *value = rt_list_get(list, 1);
    assert(rt_unbox_i64(value) == 20);

    release_obj(value);
    release_obj(list);
    release_obj(src);
}

void trap_f64_oob() {
    (void)rt_f64buf_get(g_trap_a, 1);
}

void trap_i64_mismatch() {
    rt_i64buf_add_buffer(g_trap_a, g_trap_b);
}

void trap_f64_dot_mismatch() {
    (void)rt_f64buf_dot(g_trap_a, g_trap_b);
}

void trap_empty_min() {
    (void)rt_i64buf_min(g_trap_a);
}

void test_trap_contracts() {
    g_trap_a = rt_f64buf_new(1);
    expect_trap(trap_f64_oob, "out of bounds");
    release_obj(g_trap_a);

    g_trap_a = rt_i64buf_new(2);
    g_trap_b = rt_i64buf_new(1);
    expect_trap(trap_i64_mismatch, "length mismatch");
    release_obj(g_trap_b);
    release_obj(g_trap_a);

    g_trap_a = rt_f64buf_new(2);
    g_trap_b = rt_f64buf_new(3);
    expect_trap(trap_f64_dot_mismatch, "length mismatch");
    release_obj(g_trap_b);
    release_obj(g_trap_a);

    g_trap_a = rt_i64buf_new(0);
    expect_trap(trap_empty_min, "empty buffer");
    release_obj(g_trap_a);
    g_trap_a = nullptr;
    g_trap_b = nullptr;
}

void test_deterministic_native_reference_loop() {
    void *a = rt_f64buf_new(4);
    void *b = rt_f64buf_new(4);
    double expected_sum = 0.0;
    double expected_dot = 0.0;
    for (int64_t i = 0; i < 4; i++) {
        const double av = (double)(i + 1) * 1.5;
        const double bv = (double)(5 - i) * 0.25;
        rt_f64buf_set(a, i, av);
        rt_f64buf_set(b, i, bv);
        expected_sum += av;
        expected_dot += av * bv;
    }
    assert(near(rt_f64buf_sum(a), expected_sum));
    assert(near(rt_f64buf_dot(a, b), expected_dot));
    release_obj(b);
    release_obj(a);
}

} // namespace

static void test_i64_overflow_traps() {
    // VDOC-101: I64Buffer arithmetic traps on signed overflow instead of
    // invoking undefined behavior.
    void *buf = rt_i64buf_new(1);
    rt_i64buf_set(buf, 0, INT64_MAX);

    {
        jmp_buf env;
        rt_trap_set_recovery(&env);
        bool trapped = true;
        if (setjmp(env) == 0) {
            rt_i64buf_add_scalar(buf, 1);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "AddScalar overflow must trap");
    }
    {
        jmp_buf env;
        rt_trap_set_recovery(&env);
        volatile bool trapped = true;
        if (setjmp(env) == 0) {
            rt_i64buf_mul_scalar(buf, 2);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "MulScalar overflow must trap");
    }
    {
        void *pair = rt_i64buf_new(2);
        rt_i64buf_set(pair, 0, INT64_MAX);
        rt_i64buf_set(pair, 1, INT64_MAX);
        jmp_buf env;
        rt_trap_set_recovery(&env);
        volatile bool trapped = true;
        if (setjmp(env) == 0) {
            (void)rt_i64buf_sum(pair);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "Sum overflow must trap");
    }
    // In-range arithmetic still works.
    void *ok = rt_i64buf_new(2);
    rt_i64buf_set(ok, 0, 3);
    rt_i64buf_set(ok, 1, 4);
    rt_i64buf_add_scalar(ok, 1);
    assert(rt_i64buf_sum(ok) == 9);
    assert(rt_i64buf_dot(ok, ok) == 16 + 25);
}

int main() {
    test_i64_overflow_traps();
    test_f64_buffer_core_ops();
    test_i64_buffer_core_ops();
    test_slice_is_independent_copy();
    test_to_seq_from_seq_round_trip();
    test_to_list_preserves_order_and_values();
    test_trap_contracts();
    test_deterministic_native_reference_loop();

    std::puts("RTNumBufTests: PASS");
    return 0;
}
