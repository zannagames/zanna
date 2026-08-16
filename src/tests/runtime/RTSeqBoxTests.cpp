//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTSeqBoxTests.cpp
// Purpose: Validate Seq.Find/Has content-aware equality for boxed values.
// Key invariants: Boxed values are compared by content, not pointer identity.
// Ownership/Lifetime: Each sequence owns its retained boxed values until the
//                     test releases the sequence and any independent aliases.
// Links: src/runtime/collections/rt_seq.c, src/runtime/oop/rt_box.c
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

struct managed_value_payload {
    void *obj;
    rt_string str;
};

static int g_managed_value_child_finalized = 0;
static int g_value_type_previous_finalized = 0;

static void managed_value_child_finalizer(void *obj) {
    (void)obj;
    g_managed_value_child_finalized++;
}

static void value_type_previous_finalizer(void *obj) {
    (void)obj;
    g_value_type_previous_finalized++;
}

static void release_object(void *obj) {
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void expect_trap(void (*fn)(), const char *message) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn();
        rt_trap_clear_recovery();
        assert(false && "expected trap");
    } else {
        std::string text = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(text.find(message) != std::string::npos);
    }
}

//=============================================================================
// Seq.Find / Seq.Has with boxed strings
//=============================================================================

static void test_seq_find_boxed_strings() {
    printf("Testing Seq.Find/Has with boxed strings:\n");

    void *seq = rt_seq_new();

    void *apple1 = rt_box_str(rt_const_cstr("apple"));
    void *banana = rt_box_str(rt_const_cstr("banana"));
    void *cherry = rt_box_str(rt_const_cstr("cherry"));

    rt_seq_push(seq, apple1);
    rt_seq_push(seq, banana);
    rt_seq_push(seq, cherry);

    // Create DIFFERENT boxed strings with same content
    void *apple2 = rt_box_str(rt_const_cstr("apple"));
    void *banana2 = rt_box_str(rt_const_cstr("banana"));

    test_result("apple1 != apple2 (different pointers)", apple1 != apple2);
    test_result("Find apple2 returns 0", rt_seq_find(seq, apple2) == 0);
    test_result("Find banana2 returns 1", rt_seq_find(seq, banana2) == 1);
    test_result("Has apple2", rt_seq_has(seq, apple2) == 1);
    test_result("Has banana2", rt_seq_has(seq, banana2) == 1);

    // Non-existent
    void *grape = rt_box_str(rt_const_cstr("grape"));
    test_result("Find grape returns -1", rt_seq_find(seq, grape) == -1);
    test_result("Has grape is false", rt_seq_has(seq, grape) == 0);

    printf("\n");
}

//=============================================================================
// Seq.Find / Seq.Has with boxed integers
//=============================================================================

static void test_seq_find_boxed_integers() {
    printf("Testing Seq.Find/Has with boxed integers:\n");

    void *seq = rt_seq_new();

    void *i42a = rt_box_i64(42);
    void *i99 = rt_box_i64(99);
    void *i0 = rt_box_i64(0);

    rt_seq_push(seq, i42a);
    rt_seq_push(seq, i99);
    rt_seq_push(seq, i0);

    void *i42b = rt_box_i64(42);
    void *i99b = rt_box_i64(99);
    void *i0b = rt_box_i64(0);

    test_result("i42a != i42b (different pointers)", i42a != i42b);
    test_result("Find i42b returns 0", rt_seq_find(seq, i42b) == 0);
    test_result("Find i99b returns 1", rt_seq_find(seq, i99b) == 1);
    test_result("Find i0b returns 2", rt_seq_find(seq, i0b) == 2);
    test_result("Has i42b", rt_seq_has(seq, i42b) == 1);

    void *i77 = rt_box_i64(77);
    test_result("Find i77 returns -1", rt_seq_find(seq, i77) == -1);
    test_result("Has i77 is false", rt_seq_has(seq, i77) == 0);

    printf("\n");
}

//=============================================================================
// Seq.Find / Seq.Has with boxed floats
//=============================================================================

static void test_seq_find_boxed_floats() {
    printf("Testing Seq.Find/Has with boxed floats:\n");

    void *seq = rt_seq_new();

    void *f1a = rt_box_f64(3.14);
    void *f2 = rt_box_f64(2.718);
    rt_seq_push(seq, f1a);
    rt_seq_push(seq, f2);

    void *f1b = rt_box_f64(3.14);
    test_result("f1a != f1b (different pointers)", f1a != f1b);
    test_result("Find f1b returns 0", rt_seq_find(seq, f1b) == 0);
    test_result("Has f1b", rt_seq_has(seq, f1b) == 1);

    void *f3 = rt_box_f64(1.0);
    test_result("Find f3 returns -1", rt_seq_find(seq, f3) == -1);

    printf("\n");
}

static void test_mixed_numeric_sort_preserves_i64_precision() {
    printf("Testing mixed numeric sort precision:\n");

    void *integer = rt_box_i64(INT64_C(9007199254740993));
    void *number = rt_box_f64(9007199254740992.0);
    test_result("i64 above 2^53 sorts after rounded f64",
                rt_box_default_sort_compare(integer, number) > 0);
    test_result("mixed numeric comparison is antisymmetric",
                rt_box_default_sort_compare(number, integer) < 0);

    release_object(integer);
    release_object(number);
    printf("\n");
}

//=============================================================================
// Seq.Find / Seq.Has with boxed booleans
//=============================================================================

static void test_seq_find_boxed_booleans() {
    printf("Testing Seq.Find/Has with boxed booleans:\n");

    void *seq = rt_seq_new();

    void *btrue1 = rt_box_i1(1);
    rt_seq_push(seq, btrue1);

    void *btrue2 = rt_box_i1(1);
    void *bfalse = rt_box_i1(0);

    test_result("btrue1 != btrue2 (different pointers)", btrue1 != btrue2);
    test_result("Has btrue2", rt_seq_has(seq, btrue2) == 1);
    test_result("Has bfalse is false", rt_seq_has(seq, bfalse) == 0);

    printf("\n");
}

//=============================================================================
// Pointer identity still works for non-boxed objects
//=============================================================================

static void test_seq_pointer_identity() {
    printf("Testing Seq.Find/Has with pointer identity (non-boxed):\n");

    void *seq = rt_seq_new();

    // Use the seq itself as a non-boxed element
    rt_seq_push(seq, seq);
    test_result("Has self (same pointer)", rt_seq_has(seq, seq) == 1);
    test_result("Find self returns 0", rt_seq_find(seq, seq) == 0);

    printf("\n");
}

static void test_box_type_rejects_box_element_arrays() {
    printf("Testing Box helpers reject box-element arrays:\n");

    void *box = rt_box_i64(42);
    void **arr = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_BOX, sizeof(void *), 1, 1);
    arr[0] = box;

    test_result("Box.Type ignores RT_ELEM_BOX arrays", rt_box_type(arr) == -1);
    int64_t out = 0;
    test_result("rt_box_try_to_i64 ignores RT_ELEM_BOX arrays",
                rt_box_try_to_i64(arr, &out) == 0 && out == 0);

    release_object(arr);
    printf("\n");
}

static void test_null_string_boxes_compare_equal() {
    printf("Testing null string box equality:\n");

    void *a = rt_box_str(NULL);
    void *b = rt_box_str(NULL);

    test_result("Box.EqStr accepts null string", rt_box_eq_str(a, NULL) == 1);
    test_result("Two null string boxes compare equal", rt_box_equal(a, b) == 1);

    release_object(a);
    release_object(b);
    printf("\n");
}

static void test_boxed_nan_hash_is_canonical() {
    printf("Testing Box.F64 NaN hash canonicalization:\n");

    void *nan_a = rt_box_f64(std::numeric_limits<double>::quiet_NaN());

    union {
        std::uint64_t bits;
        double value;
    } payload_nan{};

    payload_nan.bits = UINT64_C(0x7ff8000000000001);
    void *nan_b = rt_box_f64(payload_nan.value);

    test_result("Distinct NaN boxes are value-equal", rt_box_equal(nan_a, nan_b) == 1);
    test_result("NaN box hashes are canonical", rt_box_hash(nan_a) == rt_box_hash(nan_b));

    release_object(nan_a);
    release_object(nan_b);
    printf("\n");
}

static void call_box_str_invalid_string() {
    int local = 42;
    (void)rt_box_str((rt_string)&local);
}

static void *g_box_eq_invalid_string_box = nullptr;

static void call_box_eq_str_invalid_string() {
    int local = 42;
    (void)rt_box_eq_str(g_box_eq_invalid_string_box, (rt_string)&local);
}

static void test_box_string_helpers_validate_string_handles() {
    printf("Testing Box string helper string-handle validation:\n");

    expect_trap(call_box_str_invalid_string, "invalid string handle");
    g_box_eq_invalid_string_box = rt_box_str(rt_const_cstr("valid"));
    expect_trap(call_box_eq_str_invalid_string, "invalid string handle");
    release_object(g_box_eq_invalid_string_box);
    g_box_eq_invalid_string_box = nullptr;
    printf("\n");
}

static void test_value_type_managed_fields() {
    printf("Testing Box.ValueType managed field registration:\n");

    g_managed_value_child_finalized = 0;
    void *child = rt_obj_new_i64(0xB0A, 8);
    rt_obj_set_finalizer(child, managed_value_child_finalizer);
    rt_string text = rt_string_from_bytes("managed", 7);

    managed_value_payload *boxed =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    test_result("ValueType class id", rt_obj_class_id(boxed) == RT_VALUE_TYPE_CLASS_ID);
    boxed->obj = child;
    boxed->str = text;
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 1);
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, str), RT_VALUE_FIELD_STR, 1);

    release_object(child);
    test_result("Child retained by ValueType", g_managed_value_child_finalized == 0);
    rt_string_unref(text);

    release_object(boxed);
    test_result("ValueType finalizer releases object field", g_managed_value_child_finalized == 1);
    printf("\n");
}

static void test_value_type_chains_existing_finalizer() {
    printf("Testing Box.ValueType finalizer chaining:\n");

    g_managed_value_child_finalized = 0;
    g_value_type_previous_finalized = 0;
    void *child = rt_obj_new_i64(0xB0C, 8);
    rt_obj_set_finalizer(child, managed_value_child_finalizer);
    managed_value_payload *boxed =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    rt_obj_set_finalizer(boxed, value_type_previous_finalizer);
    boxed->obj = child;
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 1);

    release_object(child);
    release_object(boxed);
    test_result("Existing ValueType finalizer still runs", g_value_type_previous_finalized == 1);
    test_result("Chained ValueType finalizer releases object field",
                g_managed_value_child_finalized == 1);
    printf("\n");
}

static managed_value_payload *g_conflict_value = nullptr;
static managed_value_payload *g_invalid_field_value = nullptr;
static void *g_misaligned_field_value = nullptr;

static void call_value_type_conflicting_field() {
    rt_box_value_type_add_field(
        g_conflict_value, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_STR, 0);
}

static void call_value_type_invalid_retain_field() {
    rt_box_value_type_add_field(g_invalid_field_value,
                                (int64_t)offsetof(managed_value_payload, str),
                                RT_VALUE_FIELD_STR,
                                1);
}

static void call_value_type_misaligned_field() {
    rt_box_value_type_add_field(g_misaligned_field_value, 1, RT_VALUE_FIELD_OBJ, 0);
}

static void test_value_type_zero_size_and_duplicate_fields() {
    printf("Testing Box.ValueType zero-size and duplicate field validation:\n");

    void *empty = rt_box_value_type(0);
    test_result("Zero-size ValueType allocated", empty != nullptr);
    test_result("Zero-size ValueType class id", rt_obj_class_id(empty) == RT_VALUE_TYPE_CLASS_ID);
    release_object(empty);

    managed_value_payload *boxed =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    g_conflict_value = boxed;
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 0);
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 0);
    expect_trap(call_value_type_conflicting_field, "field offset already registered");
    g_conflict_value = nullptr;
    release_object(boxed);

    g_managed_value_child_finalized = 0;
    void *child = rt_obj_new_i64(0xD0B, 8);
    rt_obj_set_finalizer(child, managed_value_child_finalizer);
    managed_value_payload *duplicate_retained =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    duplicate_retained->obj = child;
    rt_box_value_type_add_field(
        duplicate_retained, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 1);
    rt_box_value_type_add_field(
        duplicate_retained, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 1);
    release_object(child);
    test_result("Duplicate same-kind field does not retain twice",
                g_managed_value_child_finalized == 0);
    release_object(duplicate_retained);
    test_result("Duplicate same-kind field releases once", g_managed_value_child_finalized == 1);

    managed_value_payload *invalid =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    invalid->str = (rt_string)(uintptr_t)0x1234;
    g_invalid_field_value = invalid;
    expect_trap(call_value_type_invalid_retain_field, "invalid managed field value");
    g_invalid_field_value = nullptr;
    release_object(invalid);

    void *misaligned = rt_box_value_type((int64_t)(sizeof(void *) + 1));
    g_misaligned_field_value = misaligned;
    expect_trap(call_value_type_misaligned_field, "field offset is not pointer-aligned");
    g_misaligned_field_value = nullptr;
    release_object(misaligned);
    printf("\n");
}

/// @brief Verify registered object fields make boxed value types cycle-collectable.
/// @details The value type retains itself through its first object slot. Once the caller drops
///          the construction reference, the registered traversal edge is the only remaining
///          reference and the collector must reclaim the payload without a finalizer underflow.
static void test_value_type_self_cycle_is_collected() {
    printf("Testing Box.ValueType cycle traversal:\n");

    managed_value_payload *boxed =
        (managed_value_payload *)rt_box_value_type((int64_t)sizeof(managed_value_payload));
    test_result("Self-cycle ValueType allocated", boxed != nullptr);
    rt_weakref *weak = rt_weakref_new(boxed);
    boxed->obj = boxed;
    rt_box_value_type_add_field(
        boxed, (int64_t)offsetof(managed_value_payload, obj), RT_VALUE_FIELD_OBJ, 1);

    release_object(boxed);
    test_result("Self-cycle ValueType reclaimed", rt_gc_collect() == 1);
    test_result("Self-cycle ValueType weakref cleared", rt_weakref_alive(weak) == 0);
    rt_weakref_free(weak);
    printf("\n");
}

//=============================================================================
// Main
//=============================================================================

int main() {
    printf("=== Seq Box Content Equality Tests ===\n\n");

    test_seq_find_boxed_strings();
    test_seq_find_boxed_integers();
    test_seq_find_boxed_floats();
    test_mixed_numeric_sort_preserves_i64_precision();
    test_seq_find_boxed_booleans();
    test_seq_pointer_identity();
    test_box_type_rejects_box_element_arrays();
    test_null_string_boxes_compare_equal();
    test_boxed_nan_hash_is_canonical();
    test_box_string_helpers_validate_string_handles();
    test_value_type_managed_fields();
    test_value_type_chains_existing_finalizer();
    test_value_type_zero_size_and_duplicate_fields();
    test_value_type_self_cycle_is_collected();

    printf("All Seq box equality tests passed!\n");
    return 0;
}
