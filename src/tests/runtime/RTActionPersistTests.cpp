//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTActionPersistTests.cpp
/// @file
/// @brief Exercises action binding JSON save/load and rollback behavior.
// Purpose: Validate deterministic action persistence, strict schema handling,
//   resource-safe parsing, and transactional rejection of malformed documents.
//
// Key invariants:
//   - A successful save/load round trip preserves action and binding order.
//   - Invalid or ambiguous documents leave the live registry byte-for-byte
//     equivalent to its pre-load state.
//   - Forward-compatible unknown binding tags are skipped, not materialized.
//
// Ownership/Lifetime:
//   - Runtime strings are process-local test values. Registry-owned action and
//     binding copies are cleared between cases.
//
// Links: rt_action_io.c, rt_action.c, RTActionMappingTests.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_const_cstr(s);
}

static rt_string make_bytes(const std::string &s) {
    return rt_string_from_bytes(s.data(), s.size());
}

static void assert_same_string(rt_string lhs, rt_string rhs) {
    assert(lhs != nullptr);
    assert(rhs != nullptr);
    assert(rt_str_len(lhs) == rt_str_len(rhs));
    assert(memcmp(rt_string_cstr(lhs), rt_string_cstr(rhs), (size_t)rt_str_len(lhs)) == 0);
}

// ============================================================================
// Basic save/load roundtrip
// ============================================================================

static void test_save_empty() {
    rt_action_init();
    rt_action_clear();

    rt_string json = rt_action_save();
    const char *cstr = rt_string_cstr(json);
    assert(cstr != nullptr);
    assert(strlen(cstr) > 0);
    /* Should produce valid JSON with empty actions array */
    assert(strstr(cstr, "\"actions\":[]") != nullptr);

    printf("test_save_empty: PASSED\n");
}

static void test_save_load_button_action() {
    rt_action_init();
    rt_action_clear();

    /* Define a button action with key binding */
    rt_action_define(make_str("jump"));
    rt_action_bind_key(make_str("jump"), ZANNA_KEY_SPACE);

    /* Save */
    rt_string json = rt_action_save();
    const char *cstr = rt_string_cstr(json);
    assert(strstr(cstr, "\"name\":\"jump\"") != nullptr);
    assert(strstr(cstr, "\"type\":\"button\"") != nullptr);
    assert(strstr(cstr, "\"type\":\"key\"") != nullptr);

    /* Clear and reload */
    rt_action_clear();
    assert(rt_action_exists(make_str("jump")) == 0);

    int8_t ok = rt_action_load(json);
    assert(ok == 1);
    assert(rt_action_exists(make_str("jump")) == 1);
    assert(rt_action_is_axis(make_str("jump")) == 0);
    assert(rt_action_binding_count(make_str("jump")) == 1);

    printf("test_save_load_button_action: PASSED\n");
}

static void test_save_load_axis_action() {
    rt_action_init();
    rt_action_clear();

    /* Define an axis action with two key bindings */
    rt_action_define_axis(make_str("move_x"));
    rt_action_bind_key_axis(make_str("move_x"), ZANNA_KEY_LEFT, -1.0);
    rt_action_bind_key_axis(make_str("move_x"), ZANNA_KEY_RIGHT, 1.0);

    /* Save */
    rt_string json = rt_action_save();

    /* Clear and reload */
    rt_action_clear();
    int8_t ok = rt_action_load(json);
    assert(ok == 1);

    assert(rt_action_exists(make_str("move_x")) == 1);
    assert(rt_action_is_axis(make_str("move_x")) == 1);
    assert(rt_action_binding_count(make_str("move_x")) == 2);

    printf("test_save_load_axis_action: PASSED\n");
}

static void test_save_load_multiple_actions() {
    rt_action_init();
    rt_action_clear();

    rt_action_define(make_str("fire"));
    rt_action_bind_key(make_str("fire"), ZANNA_KEY_Z);
    rt_action_bind_mouse(make_str("fire"), ZANNA_MOUSE_BUTTON_LEFT);

    rt_action_define(make_str("dodge"));
    rt_action_bind_key(make_str("dodge"), ZANNA_KEY_X);

    rt_action_define_axis(make_str("look_x"));
    rt_action_bind_mouse_x(make_str("look_x"), 0.5);

    /* Save */
    rt_string json = rt_action_save();

    /* Clear and reload */
    rt_action_clear();
    int8_t ok = rt_action_load(json);
    assert(ok == 1);

    assert(rt_action_exists(make_str("fire")) == 1);
    assert(rt_action_exists(make_str("dodge")) == 1);
    assert(rt_action_exists(make_str("look_x")) == 1);
    assert(rt_action_binding_count(make_str("fire")) == 2);
    assert(rt_action_binding_count(make_str("dodge")) == 1);
    assert(rt_action_is_axis(make_str("look_x")) == 1);

    printf("test_save_load_multiple_actions: PASSED\n");
}

static void test_save_load_pad_bindings() {
    rt_action_init();
    rt_action_clear();

    rt_action_define(make_str("jump"));
    rt_action_bind_pad_button(make_str("jump"), 0, ZANNA_PAD_A);

    rt_action_define_axis(make_str("move_x"));
    rt_action_bind_pad_axis(make_str("move_x"), -1, ZANNA_AXIS_LEFT_X, 1.0);

    rt_string json = rt_action_save();

    rt_action_clear();
    int8_t ok = rt_action_load(json);
    assert(ok == 1);

    assert(rt_action_exists(make_str("jump")) == 1);
    assert(rt_action_binding_count(make_str("jump")) == 1);
    assert(rt_action_exists(make_str("move_x")) == 1);
    assert(rt_action_binding_count(make_str("move_x")) == 1);

    printf("test_save_load_pad_bindings: PASSED\n");
}

// ============================================================================
// Edge cases
// ============================================================================

static void test_load_clears_existing() {
    rt_action_init();
    rt_action_clear();

    rt_action_define(make_str("existing"));
    rt_action_define(make_str("other"));

    /* Save only "other" */
    rt_action_remove(make_str("existing"));
    rt_string json = rt_action_save();

    /* Restore with "existing" present */
    rt_action_define(make_str("existing"));
    assert(rt_action_exists(make_str("existing")) == 1);

    rt_action_load(json);
    /* "existing" should be gone (load clears first) */
    assert(rt_action_exists(make_str("existing")) == 0);
    assert(rt_action_exists(make_str("other")) == 1);

    printf("test_load_clears_existing: PASSED\n");
}

static void test_load_null_returns_zero() {
    int8_t ok = rt_action_load(NULL);
    assert(ok == 0);

    printf("test_load_null_returns_zero: PASSED\n");
}

static void test_save_json_is_valid() {
    rt_action_init();
    rt_action_clear();

    rt_action_define(make_str("test_action"));
    rt_action_bind_key(make_str("test_action"), ZANNA_KEY_A);

    rt_string json = rt_action_save();
    const char *cstr = rt_string_cstr(json);

    /* Basic JSON structure validation */
    assert(cstr[0] == '{');
    assert(cstr[strlen(cstr) - 1] == '}');
    assert(strstr(cstr, "\"actions\"") != nullptr);

    printf("test_save_json_is_valid: PASSED\n");
}

static void test_roundtrip_preserves_exact_order() {
    rt_action_init();
    rt_action_clear();

    assert(rt_action_define(make_str("first")) == 1);
    assert(rt_action_bind_key(make_str("first"), ZANNA_KEY_A) == 1);
    assert(rt_action_bind_key(make_str("first"), ZANNA_KEY_B) == 1);
    assert(rt_action_define_axis(make_str("second")) == 1);
    assert(rt_action_bind_key_axis(make_str("second"), ZANNA_KEY_LEFT, -1.0) == 1);
    assert(rt_action_bind_key_axis(make_str("second"), ZANNA_KEY_RIGHT, 1.0) == 1);

    rt_string before = rt_action_save();
    assert(rt_action_load(before) == 1);
    rt_string after = rt_action_save();
    assert_same_string(before, after);

    printf("test_roundtrip_preserves_exact_order: PASSED\n");
}

static void assert_rejected_document_preserves_registry(const char *document) {
    rt_action_clear();
    assert(rt_action_define(make_str("sentinel")) == 1);
    assert(rt_action_bind_key(make_str("sentinel"), ZANNA_KEY_SPACE) == 1);
    rt_string before = rt_action_save();
    assert(rt_action_load(make_str(document)) == 0);
    rt_string after = rt_action_save();
    assert_same_string(before, after);
}

static void test_malformed_schema_is_transactional() {
    static const char *documents[] = {
        "{\"actions\":[{\"type\":\"button\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"\",\"type\":\"button\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"bad\\u0000name\",\"type\":\"button\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"a\",\"name\":\"b\",\"type\":\"button\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"type\":\"axis\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"other\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":[],\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"same\",\"type\":\"button\",\"bindings\":[]},"
        "{\"name\":\"same\",\"type\":\"button\",\"bindings\":[]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"key\",\"type\":\"key\",\"code\":65,\"pad\":0,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"key\",\"code\":65.5,\"pad\":0,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"key\",\"code\":512,\"pad\":0,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"mouse_x\",\"code\":0,\"pad\":0,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"axis\",\"bindings\":["
        "{\"type\":\"mouse\",\"code\":0,\"pad\":0,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"axis\",\"bindings\":["
        "{\"type\":\"pad_axis\",\"code\":0,\"pad\":4,\"value\":1}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"chord\",\"code\":0,\"pad\":0,\"value\":1,\"keys\":[341]}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"chord\",\"code\":0,\"pad\":0,\"value\":1,\"keys\":[341,341]}]}]}",
        "{\"actions\":[{\"name\":\"a\",\"type\":\"button\",\"bindings\":["
        "{\"type\":\"key\",\"code\":65,\"pad\":0}]}]}",
    };

    rt_action_init();
    for (const char *document : documents)
        assert_rejected_document_preserves_registry(document);

    printf("test_malformed_schema_is_transactional: PASSED\n");
}

static void test_unknown_binding_tags_are_forward_compatible() {
    rt_action_init();
    rt_action_clear();
    const char *document = "{\"actions\":[{\"name\":\"future\",\"type\":\"button\",\"bindings\":["
                           "{\"type\":\"future_device\",\"vendor\":7}]}]}";
    assert(rt_action_load(make_str(document)) == 1);
    assert(rt_action_exists(make_str("future")) == 1);
    assert(rt_action_binding_count(make_str("future")) == 0);

    printf("test_unknown_binding_tags_are_forward_compatible: PASSED\n");
}

static void test_names_longer_than_legacy_stack_buffer_roundtrip() {
    rt_action_init();
    rt_action_clear();
    std::string long_name(600, 'n');
    std::string document =
        "{\"actions\":[{\"name\":\"" + long_name + "\",\"type\":\"button\",\"bindings\":[]}]}";
    assert(rt_action_load(make_bytes(document)) == 1);
    assert(rt_action_exists(make_bytes(long_name)) == 1);
    rt_string saved = rt_action_save();
    assert(rt_str_len(saved) > (int64_t)long_name.size());

    printf("test_names_longer_than_legacy_stack_buffer_roundtrip: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Action Persistence Tests ===\n\n");

    test_save_empty();
    test_save_load_button_action();
    test_save_load_axis_action();
    test_save_load_multiple_actions();
    test_save_load_pad_bindings();
    test_load_clears_existing();
    test_load_null_returns_zero();
    test_save_json_is_valid();
    test_roundtrip_preserves_exact_order();
    test_malformed_schema_is_transactional();
    test_unknown_binding_tags_are_forward_compatible();
    test_names_longer_than_legacy_stack_buffer_roundtrip();

    printf("\nAll Action Persistence tests passed!\n");
    return 0;
}
