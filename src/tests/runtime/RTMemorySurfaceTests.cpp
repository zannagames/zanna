//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTMemorySurfaceTests.cpp
// Purpose: Validate the public Zanna.Memory retain/release surface and heap
//          correctness traps that protect it.
//
//===----------------------------------------------------------------------===//

#include "common/ProcessIsolation.hpp"
#include "rt_array_obj.h"
#include "rt_box.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_string.h"
#include "rt_trap.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <csetjmp>
#include <cstring>
#include <stdint.h>
#include <string>

/// @brief Erase constness so borrowed text can travel through an opaque `void *`.
/// @details The runtime payload APIs take `void *`; these tests hand them
///          read-only bytes they own elsewhere and never write through the
///          result, so the cast is safe here.
/// @param text Borrowed NUL-terminated bytes.
/// @return The same address as a mutable `void *`.
static void *borrowedPayload(const char *text) {
    return const_cast<void *>(static_cast<const void *>(text));
}


extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

namespace {

int g_finalizer_count = 0;
int g_trapping_finalizer_count = 0;
int g_second_finalizer_count = 0;
void *g_resurrected = nullptr;

void count_finalizer(void *obj) {
    (void)obj;
    g_finalizer_count++;
}

void resurrect_finalizer(void *obj) {
    g_resurrected = obj;
    rt_obj_resurrect(obj);
}

void trapping_finalizer(void *obj) {
    (void)obj;
    g_trapping_finalizer_count++;
    rt_trap("finalizer boom");
}

void second_count_finalizer(void *obj) {
    (void)obj;
    g_second_finalizer_count++;
}

void call_memory_retain_invalid() {
    int local = 1;
    rt_memory_retain(&local);
}

void call_memory_release_invalid() {
    int local = 1;
    rt_memory_release(&local);
}

void call_memory_retain_str_object() {
    void *obj = rt_obj_new_i64(13, 8);
    rt_memory_retain_str((rt_string)obj);
}

void call_memory_retain_string_payload() {
    const char *raw = "heap-backed string payload must not be retained directly";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    rt_memory_retain(borrowedPayload(rt_string_cstr(text)));
}

void call_memory_release_str_object() {
    void *obj = rt_obj_new_i64(14, 8);
    rt_memory_release_str((rt_string)obj);
}

void call_object_negative_size() {
    rt_obj_new_i64(7, -1);
}

void call_heap_double_release_deferred() {
    void *obj = rt_obj_new_i64(8, 8);
    assert(rt_heap_release_deferred(obj) == 0);
    rt_heap_release_deferred(obj);
}

void call_heap_retain_overflow() {
    void *obj = rt_obj_new_i64(9, 8);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    rt_heap_retain(obj);
}

void call_sso_string_retain_overflow() {
    rt_string text = rt_string_from_bytes("sso", 3);
    assert(text != nullptr);
    text->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_string_ref(text);
}

void call_heap_release_immortal() {
    void *obj = rt_obj_new_i64(12, 8);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_IMMORTAL_REFCNT;
    rt_heap_release(obj);
}

void call_heap_set_len_past_capacity() {
    void *payload = rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_U8, 1, 0, 4);
    assert(payload != nullptr);
    rt_heap_set_len(payload, 5);
}

void call_memory_release_unknown_heap_kind() {
    void *obj = rt_obj_new_i64(15, 8);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->kind = 999;
    rt_memory_release(obj);
}

void call_memory_retain_unknown_heap_kind() {
    void *obj = rt_obj_new_i64(19, 8);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->kind = 999;
    rt_memory_retain(obj);
}

void call_memory_release_unknown_array_element_kind() {
    void *payload = rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_U8, 1, 1, 1);
    assert(payload != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->elem_kind = 999;
    rt_memory_release(payload);
}

void call_memory_release_array_len_past_cap() {
    void **payload = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_OBJ, sizeof(void *), 1, 1);
    assert(payload != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->len = 2;
    hdr->cap = 1;
    rt_memory_release(payload);
}

void call_box_str_retain_overflow() {
    const char *raw = "heap-backed box string retain overflow";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(payload) == 1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_box_str(text);
}

void call_value_type_add_field_retain_overflow() {
    void *value_type = rt_box_value_type((int64_t)sizeof(void *));
    assert(value_type != nullptr);
    const char *raw = "heap-backed value type string retain overflow";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(payload) == 1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    *(rt_string *)value_type = text;
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    rt_box_value_type_add_field(value_type, 0, RT_VALUE_FIELD_STR, 1);
}

void call_option_some_retain_overflow() {
    void *obj = rt_obj_new_i64(16, 8);
    assert(obj != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_option_some(obj);
}

void call_option_some_str_retain_overflow() {
    const char *raw = "heap-backed option string retain overflow";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(payload) == 1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_option_some_str(text);
}

void call_result_ok_retain_overflow() {
    void *obj = rt_obj_new_i64(17, 8);
    assert(obj != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_result_ok(obj);
}

void call_result_err_retain_overflow() {
    void *obj = rt_obj_new_i64(18, 8);
    assert(obj != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_result_err(obj);
}

void call_result_ok_str_retain_overflow() {
    const char *raw = "heap-backed result ok string retain overflow";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(payload) == 1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_result_ok_str(text);
}

void call_result_err_str_retain_overflow() {
    const char *raw = "heap-backed result err string retain overflow";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(payload) == 1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    (void)rt_result_err_str(text);
}

void call_resurrect_live_object() {
    void *obj = rt_obj_new_i64(10, 8);
    rt_obj_resurrect(obj);
}

void call_obj_free_live_object() {
    void *obj = rt_obj_new_i64(11, 8);
    rt_obj_free(obj);
}

void expect_trap(void (*fn)(), const char *message) {
    auto result = zanna::tests::runIsolated(fn);
    assert(result.trapped());
    assert(result.stderrText.find(message) != std::string::npos);
}

void test_memory_release_array_validation_preserves_refcount() {
    void *bad_kind = rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_U8, 1, 1, 1);
    assert(bad_kind != nullptr);
    rt_heap_hdr_t *bad_kind_hdr = rt_heap_hdr(bad_kind);
    bad_kind_hdr->elem_kind = 999;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_memory_release(bad_kind);
        rt_trap_clear_recovery();
        assert(false && "unsupported array element kind should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("unsupported array element kind") != std::string::npos);
    }
    assert(bad_kind_hdr->refcnt == 1);
    bad_kind_hdr->elem_kind = RT_ELEM_U8;
    assert(rt_memory_release(bad_kind) == 0);

    void *bad_len = rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_U8, 1, 1, 1);
    assert(bad_len != nullptr);
    rt_heap_hdr_t *bad_len_hdr = rt_heap_hdr(bad_len);
    bad_len_hdr->len = 2;
    bad_len_hdr->cap = 1;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_memory_release(bad_len);
        rt_trap_clear_recovery();
        assert(false && "array length past capacity should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("array length exceeds capacity") != std::string::npos);
    }
    assert(bad_len_hdr->refcnt == 1);
    bad_len_hdr->len = 1;
    bad_len_hdr->cap = 1;
    assert(rt_memory_release(bad_len) == 0);
}

void test_memory_release_runs_finalizer() {
    g_finalizer_count = 0;
    void *obj = rt_obj_new_i64(0xCAFE, 16);
    assert(obj != nullptr);
    rt_obj_set_finalizer(obj, count_finalizer);

    rt_memory_retain(obj);
    assert(rt_memory_release(obj) == 1);
    assert(g_finalizer_count == 0);
    assert(rt_heap_is_payload(obj) == 1);

    assert(rt_memory_release(obj) == 0);
    assert(g_finalizer_count == 1);
    assert(rt_heap_is_payload(obj) == 0);
}

void test_memory_release_reports_string_refcount() {
    rt_string s = rt_string_from_bytes("refcounted string", 17);
    assert(s != nullptr);
    rt_memory_retain(s);
    assert(rt_memory_release(s) == 1);
    assert(rt_memory_release(s) == 0);

    rt_string small = rt_string_from_bytes("sso", 3);
    assert(small != nullptr);
    rt_memory_retain(small);
    assert(rt_memory_release(small) == 1);
    assert(rt_memory_release(small) == 0);
}

void test_memory_release_reports_resurrection_refcount() {
    g_resurrected = nullptr;
    void *obj = rt_obj_new_i64(0x515, 16);
    assert(obj != nullptr);
    rt_obj_set_finalizer(obj, resurrect_finalizer);

    assert(rt_memory_release(obj) == 1);
    assert(g_resurrected == obj);
    assert(rt_heap_is_payload(obj) == 1);
    assert(rt_memory_release(obj) == 0);
    assert(rt_heap_is_payload(obj) == 0);
}

void test_memory_release_array_drops_elements() {
    g_finalizer_count = 0;
    void *obj = rt_obj_new_i64(0xBEEF, 8);
    rt_obj_set_finalizer(obj, count_finalizer);

    void **arr = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_OBJ, sizeof(void *), 1, 1);
    assert(arr != nullptr);
    arr[0] = obj;
    rt_obj_retain_maybe(obj);

    assert(rt_obj_release_check0(obj) == 0);
    assert(g_finalizer_count == 0);

    assert(rt_memory_release(arr) == 0);
    assert(g_finalizer_count == 1);
    assert(rt_heap_is_payload(arr) == 0);
}

void test_memory_release_trapping_finalizer_frees_object() {
    g_trapping_finalizer_count = 0;
    void *obj = rt_obj_new_i64(0xBAD, 16);
    assert(obj != nullptr);
    rt_obj_set_finalizer(obj, trapping_finalizer);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_memory_release(obj);
        rt_trap_clear_recovery();
        assert(false && "trapping finalizer should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("finalizer boom") != std::string::npos);
    }

    assert(g_trapping_finalizer_count == 1);
    assert(rt_heap_is_payload(obj) == 0);
}

void test_memory_release_array_trap_drains_and_frees() {
    g_trapping_finalizer_count = 0;
    g_second_finalizer_count = 0;

    void *first = rt_obj_new_i64(0xA11, 8);
    void *second = rt_obj_new_i64(0xA12, 8);
    assert(first != nullptr);
    assert(second != nullptr);
    rt_obj_set_finalizer(first, trapping_finalizer);
    rt_obj_set_finalizer(second, second_count_finalizer);

    void **arr = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_OBJ, sizeof(void *), 2, 2);
    assert(arr != nullptr);
    arr[0] = first;
    arr[1] = second;
    rt_obj_retain_maybe(first);
    rt_obj_retain_maybe(second);
    assert(rt_obj_release_check0(first) == 0);
    assert(rt_obj_release_check0(second) == 0);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_memory_release(arr);
        rt_trap_clear_recovery();
        assert(false && "trapping array element finalizer should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("finalizer boom") != std::string::npos);
    }

    assert(g_trapping_finalizer_count == 1);
    assert(g_second_finalizer_count == 1);
    assert(rt_heap_is_payload(first) == 0);
    assert(rt_heap_is_payload(second) == 0);
    assert(rt_heap_is_payload(arr) == 0);
}

void test_value_type_trapping_previous_finalizer_releases_fields() {
    g_trapping_finalizer_count = 0;
    void *value_type = rt_box_value_type((int64_t)sizeof(rt_string));
    assert(value_type != nullptr);
    rt_obj_set_finalizer(value_type, trapping_finalizer);

    const char *raw = "value type field survives until chained finalizer trap cleanup";
    rt_string text = rt_string_from_bytes(raw, std::strlen(raw));
    assert(text != nullptr);
    void *text_payload = borrowedPayload(rt_string_cstr(text));
    assert(rt_heap_is_payload(text_payload) == 1);
    *(rt_string *)value_type = text;
    rt_box_value_type_add_field(value_type, 0, RT_VALUE_FIELD_STR, 1);
    rt_string_unref(text);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_memory_release(value_type);
        rt_trap_clear_recovery();
        assert(false && "trapping chained finalizer should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("finalizer boom") != std::string::npos);
    }

    assert(g_trapping_finalizer_count == 1);
    assert(rt_heap_is_payload(text_payload) == 0);
    assert(rt_heap_is_payload(value_type) == 0);
}

void test_value_type_field_trap_drains_remaining_fields() {
    g_trapping_finalizer_count = 0;
    g_second_finalizer_count = 0;

    void *first = rt_obj_new_i64(0xC01, 8);
    void *second = rt_obj_new_i64(0xC02, 8);
    assert(first != nullptr);
    assert(second != nullptr);
    rt_obj_set_finalizer(first, trapping_finalizer);
    rt_obj_set_finalizer(second, second_count_finalizer);

    void *value_type = rt_box_value_type((int64_t)(sizeof(void *) * 2));
    assert(value_type != nullptr);
    void **slots = (void **)value_type;
    slots[0] = first;
    slots[1] = second;
    rt_box_value_type_add_field(value_type, 0, RT_VALUE_FIELD_OBJ, 1);
    rt_box_value_type_add_field(value_type, (int64_t)sizeof(void *), RT_VALUE_FIELD_OBJ, 1);
    assert(rt_obj_release_check0(first) == 0);
    assert(rt_obj_release_check0(second) == 0);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_memory_release(value_type);
        rt_trap_clear_recovery();
        assert(false && "trapping value-type field finalizer should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("finalizer boom") != std::string::npos);
    }

    assert(g_trapping_finalizer_count == 1);
    assert(g_second_finalizer_count == 1);
    assert(rt_heap_is_payload(first) == 0);
    assert(rt_heap_is_payload(second) == 0);
    assert(rt_heap_is_payload(value_type) == 0);
}

void test_object_array_uses_object_element_kind() {
    g_finalizer_count = 0;
    void *obj = rt_obj_new_i64(0x0B1, 8);
    rt_obj_set_finalizer(obj, count_finalizer);

    void **arr = rt_arr_obj_new(1);
    assert(arr != nullptr);
    rt_heap_hdr_t *hdr = rt_heap_hdr(arr);
    assert(hdr->elem_kind == RT_ELEM_OBJ);
    rt_arr_obj_put(arr, 0, obj);

    assert(rt_obj_release_check0(obj) == 0);
    rt_arr_obj_release(arr);
    assert(g_finalizer_count == 1);
}

void test_memory_release_array_drops_box_elements() {
    rt_string text = rt_string_from_bytes("boxed", 5);
    void *box = rt_box_str(text);
    assert(box != nullptr);

    void **arr = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_BOX, sizeof(void *), 1, 1);
    assert(arr != nullptr);
    arr[0] = box;
    rt_obj_retain_maybe(box);

    assert(rt_obj_release_check0(box) == 0);
    assert(rt_memory_release(arr) == 0);
    assert(rt_heap_is_payload(arr) == 0);
    assert(rt_heap_is_payload(box) == 0);
    assert(rt_memory_release(text) == 0);
}

void test_heap_mark_disposed_return_contract() {
    void *obj = rt_obj_new_i64(0xD15, 8);
    assert(rt_heap_mark_disposed(obj) == 1);
    assert(rt_heap_mark_disposed(obj) == 0);
    assert(rt_obj_release_check0(obj) == 1);
    rt_obj_free(obj);
}

} // namespace

int main(int argc, char *argv[]) {
    zanna::tests::registerChildFunction(call_memory_retain_invalid);
    zanna::tests::registerChildFunction(call_memory_release_invalid);
    zanna::tests::registerChildFunction(call_memory_retain_str_object);
    zanna::tests::registerChildFunction(call_memory_retain_string_payload);
    zanna::tests::registerChildFunction(call_memory_release_str_object);
    zanna::tests::registerChildFunction(call_object_negative_size);
    zanna::tests::registerChildFunction(call_heap_double_release_deferred);
    zanna::tests::registerChildFunction(call_heap_retain_overflow);
    zanna::tests::registerChildFunction(call_sso_string_retain_overflow);
    zanna::tests::registerChildFunction(call_heap_release_immortal);
    zanna::tests::registerChildFunction(call_heap_set_len_past_capacity);
    zanna::tests::registerChildFunction(call_memory_release_unknown_heap_kind);
    zanna::tests::registerChildFunction(call_memory_retain_unknown_heap_kind);
    zanna::tests::registerChildFunction(call_memory_release_unknown_array_element_kind);
    zanna::tests::registerChildFunction(call_memory_release_array_len_past_cap);
    zanna::tests::registerChildFunction(call_box_str_retain_overflow);
    zanna::tests::registerChildFunction(call_value_type_add_field_retain_overflow);
    zanna::tests::registerChildFunction(call_option_some_retain_overflow);
    zanna::tests::registerChildFunction(call_option_some_str_retain_overflow);
    zanna::tests::registerChildFunction(call_result_ok_retain_overflow);
    zanna::tests::registerChildFunction(call_result_err_retain_overflow);
    zanna::tests::registerChildFunction(call_result_ok_str_retain_overflow);
    zanna::tests::registerChildFunction(call_result_err_str_retain_overflow);
    zanna::tests::registerChildFunction(call_resurrect_live_object);
    zanna::tests::registerChildFunction(call_obj_free_live_object);
    if (zanna::tests::dispatchChild(argc, argv))
        return 0;

    test_memory_release_runs_finalizer();
    test_memory_release_reports_string_refcount();
    test_memory_release_reports_resurrection_refcount();
    test_memory_release_array_drops_elements();
    test_memory_release_trapping_finalizer_frees_object();
    test_memory_release_array_trap_drains_and_frees();
    test_value_type_trapping_previous_finalizer_releases_fields();
    test_value_type_field_trap_drains_remaining_fields();
    test_memory_release_array_validation_preserves_refcount();
    test_object_array_uses_object_element_kind();
    test_memory_release_array_drops_box_elements();
    test_heap_mark_disposed_return_contract();
    expect_trap(call_memory_retain_invalid, "Zanna.Memory.Retain");
    expect_trap(call_memory_release_invalid, "Zanna.Memory.Release");
    expect_trap(call_memory_retain_str_object, "Zanna.Memory.RetainStr");
    expect_trap(call_memory_retain_string_payload, "invalid string payload");
    expect_trap(call_memory_release_str_object, "Zanna.Memory.ReleaseStr");
    expect_trap(call_object_negative_size, "negative object size");
    expect_trap(call_heap_double_release_deferred, "double release");
    expect_trap(call_heap_retain_overflow, "refcount overflow");
    expect_trap(call_sso_string_retain_overflow, "refcount overflow");
    expect_trap(call_heap_release_immortal, "cannot release immortal refcount");
    expect_trap(call_heap_set_len_past_capacity, "length exceeds capacity");
    expect_trap(call_memory_release_unknown_heap_kind, "unsupported heap payload kind");
    expect_trap(call_memory_retain_unknown_heap_kind, "unsupported heap payload kind");
    expect_trap(call_memory_release_unknown_array_element_kind, "unsupported array element kind");
    expect_trap(call_memory_release_array_len_past_cap, "array length exceeds capacity");
    expect_trap(call_box_str_retain_overflow, "refcount overflow");
    expect_trap(call_value_type_add_field_retain_overflow, "refcount overflow");
    expect_trap(call_option_some_retain_overflow, "refcount overflow");
    expect_trap(call_option_some_str_retain_overflow, "refcount overflow");
    expect_trap(call_result_ok_retain_overflow, "refcount overflow");
    expect_trap(call_result_err_retain_overflow, "refcount overflow");
    expect_trap(call_result_ok_str_retain_overflow, "refcount overflow");
    expect_trap(call_result_err_str_retain_overflow, "refcount overflow");
    expect_trap(call_resurrect_live_object, "refcount is not zero");
    expect_trap(call_obj_free_live_object, "refcount is not zero");
    return 0;
}
