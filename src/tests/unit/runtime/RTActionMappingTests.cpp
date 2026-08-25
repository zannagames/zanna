//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/runtime/RTActionMappingTests.cpp
/// @file
/// @brief Exercises action registry, binding, preset, and cached-input behavior.
// Purpose: Validate the action mapping system's public input-abstraction
//   contract and defensive handling of malformed names/source identifiers.
//
// Key invariants:
//   - Names and physical-source identifiers are validated before mutation.
//   - Axis state remains finite even when valid finite contributions overflow.
//   - Preset application is idempotent and transactional.
//
// Ownership/Lifetime:
//   - Runtime strings and sequences created by the test live for the short
//     process lifetime; registry-owned copies are cleared between cases.
//
// Links: rt_action.c, rt_action_presets.c, RTActionPersistTests.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_action_internal.h"
#include "rt_box.h"
#include "rt_input.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

// Helper to create an rt_string from a C string
static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static void *make_key_seq(const int64_t *keys, int count) {
    void *seq = rt_seq_new();
    for (int i = 0; i < count; i++) {
        rt_seq_push(seq, rt_box_i64(keys[i]));
    }
    return seq;
}

static void sim_key_frame(const int64_t *press_keys, const int64_t *release_keys) {
    rt_keyboard_begin_frame();
    if (press_keys) {
        for (int i = 0; press_keys[i] >= 0; i++)
            rt_keyboard_on_key_down(press_keys[i]);
    }
    if (release_keys) {
        for (int i = 0; release_keys[i] >= 0; i++)
            rt_keyboard_on_key_up(release_keys[i]);
    }
}

// Test: Define button action and check existence
static void test_define_button_action() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);
    assert(rt_action_exists(jump) == 1);
    assert(rt_action_is_axis(jump) == 0);

    // Defining same action again should fail
    rt_string jump2 = make_str("jump");
    assert(rt_action_define(jump2) == 0);

    rt_action_clear();
}

// Test: Define axis action and check existence
static void test_define_axis_action() {
    rt_action_init();
    rt_action_clear();

    rt_string move_x = make_str("move_x");
    assert(rt_action_define_axis(move_x) == 1);
    assert(rt_action_exists(move_x) == 1);
    assert(rt_action_is_axis(move_x) == 1);

    rt_action_clear();
}

// Test: Remove action
static void test_remove_action() {
    rt_action_init();
    rt_action_clear();

    rt_string fire = make_str("fire");
    assert(rt_action_define(fire) == 1);
    assert(rt_action_exists(fire) == 1);

    rt_string fire2 = make_str("fire");
    assert(rt_action_remove(fire2) == 1);
    assert(rt_action_exists(fire) == 0);

    // Removing non-existent action should fail
    rt_string fire3 = make_str("fire");
    assert(rt_action_remove(fire3) == 0);

    rt_action_clear();
}

// Test: Bind keyboard key to button action
static void test_bind_key() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    rt_string jump2 = make_str("jump");
    assert(rt_action_bind_key(jump2, ZANNA_KEY_SPACE) == 1);

    rt_string jump3 = make_str("jump");
    assert(rt_action_binding_count(jump3) == 1);

    // Binding to axis action with wrong function should fail
    rt_string move_x = make_str("move_x");
    assert(rt_action_define_axis(move_x) == 1);

    rt_string move_x2 = make_str("move_x");
    assert(rt_action_bind_key(move_x2, ZANNA_KEY_LEFT) == 0); // Wrong - should use bind_key_axis

    rt_action_clear();
}

// Test: Bind key to axis action
static void test_bind_key_axis() {
    rt_action_init();
    rt_action_clear();

    rt_string move_x = make_str("move_x");
    assert(rt_action_define_axis(move_x) == 1);

    rt_string move_x2 = make_str("move_x");
    assert(rt_action_bind_key_axis(move_x2, ZANNA_KEY_LEFT, -1.0) == 1);

    rt_string move_x3 = make_str("move_x");
    assert(rt_action_bind_key_axis(move_x3, ZANNA_KEY_RIGHT, 1.0) == 1);

    rt_string move_x4 = make_str("move_x");
    assert(rt_action_binding_count(move_x4) == 2);

    rt_action_clear();
}

// Test: Unbind key
static void test_unbind_key() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    rt_string jump2 = make_str("jump");
    assert(rt_action_bind_key(jump2, ZANNA_KEY_SPACE) == 1);

    rt_string jump3 = make_str("jump");
    assert(rt_action_binding_count(jump3) == 1);

    rt_string jump4 = make_str("jump");
    assert(rt_action_unbind_key(jump4, ZANNA_KEY_SPACE) == 1);

    rt_string jump5 = make_str("jump");
    assert(rt_action_binding_count(jump5) == 0);

    // Unbinding non-existent binding should fail
    rt_string jump6 = make_str("jump");
    assert(rt_action_unbind_key(jump6, ZANNA_KEY_SPACE) == 0);

    rt_action_clear();
}

// Test: Bind mouse button
static void test_bind_mouse() {
    rt_action_init();
    rt_action_clear();

    rt_string fire = make_str("fire");
    assert(rt_action_define(fire) == 1);

    rt_string fire2 = make_str("fire");
    assert(rt_action_bind_mouse(fire2, ZANNA_MOUSE_BUTTON_LEFT) == 1);

    rt_string fire3 = make_str("fire");
    assert(rt_action_binding_count(fire3) == 1);

    rt_action_clear();
}

// Test: Bind gamepad button
static void test_bind_pad_button() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    // Bind to any controller (-1)
    rt_string jump2 = make_str("jump");
    assert(rt_action_bind_pad_button(jump2, -1, ZANNA_PAD_A) == 1);

    rt_string jump3 = make_str("jump");
    assert(rt_action_binding_count(jump3) == 1);

    rt_action_clear();
}

// Test: Bind gamepad axis
static void test_bind_pad_axis() {
    rt_action_init();
    rt_action_clear();

    rt_string move_x = make_str("move_x");
    assert(rt_action_define_axis(move_x) == 1);

    rt_string move_x2 = make_str("move_x");
    assert(rt_action_bind_pad_axis(move_x2, -1, ZANNA_AXIS_LEFT_X, 1.0) == 1);

    rt_string move_x3 = make_str("move_x");
    assert(rt_action_binding_count(move_x3) == 1);

    rt_action_clear();
}

// Test: Multiple bindings for one action
static void test_multiple_bindings() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    rt_string jump2 = make_str("jump");
    rt_action_bind_key(jump2, ZANNA_KEY_SPACE);

    rt_string jump3 = make_str("jump");
    rt_action_bind_key(jump3, ZANNA_KEY_W);

    rt_string jump4 = make_str("jump");
    rt_action_bind_pad_button(jump4, -1, ZANNA_PAD_A);

    rt_string jump5 = make_str("jump");
    assert(rt_action_binding_count(jump5) == 3);

    rt_action_clear();
}

// Test: Action bindings string
static void test_bindings_str() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    rt_string jump2 = make_str("jump");
    rt_action_bind_key(jump2, ZANNA_KEY_SPACE);

    rt_string jump3 = make_str("jump");
    rt_action_bind_pad_button(jump3, -1, ZANNA_PAD_A);

    rt_string jump4 = make_str("jump");
    rt_string bindings = rt_action_bindings_str(jump4);
    assert(rt_str_len(bindings) > 0);

    rt_action_clear();
}

static void test_bindings_str_not_truncated() {
    rt_action_init();
    rt_action_clear();

    rt_string action = make_str("long_prompt");
    assert(rt_action_define(action) == 1);
    for (int i = 0; i < 220; i++)
        assert(rt_action_bind_key(make_str("long_prompt"), ZANNA_KEY_SPACE) == 1);

    rt_string bindings = rt_action_bindings_str(make_str("long_prompt"));
    assert(rt_str_len(bindings) > 1024);
    assert(rt_action_binding_count(make_str("long_prompt")) == 220);

    rt_action_clear();
}

// Test: Key bound to detection
static void test_key_bound_to() {
    rt_action_init();
    rt_action_clear();

    rt_string jump = make_str("jump");
    assert(rt_action_define(jump) == 1);

    rt_string jump2 = make_str("jump");
    rt_action_bind_key(jump2, ZANNA_KEY_SPACE);

    rt_string bound = rt_action_key_bound_to(ZANNA_KEY_SPACE);
    assert(rt_str_len(bound) > 0);

    // Unbound key should return empty
    rt_string unbound = rt_action_key_bound_to(ZANNA_KEY_A);
    assert(rt_str_len(unbound) == 0);

    rt_action_clear();
}

static void test_action_edge_state_requires_update() {
    rt_action_init();
    rt_action_clear();

    rt_string confirm = make_str("confirm");
    assert(rt_action_define(confirm) == 1);
    assert(rt_action_bind_key(make_str("confirm"), ZANNA_KEY_ENTER) == 1);

    int64_t press_enter[] = {ZANNA_KEY_ENTER, -1};
    sim_key_frame(press_enter, NULL);
    rt_action_update();
    assert(rt_action_pressed(make_str("confirm")) == 1);
    assert(rt_action_held(make_str("confirm")) == 1);
    assert(rt_action_released(make_str("confirm")) == 0);

    sim_key_frame(NULL, NULL);
    rt_action_update();
    assert(rt_action_pressed(make_str("confirm")) == 0);
    assert(rt_action_held(make_str("confirm")) == 1);

    int64_t release_enter[] = {ZANNA_KEY_ENTER, -1};
    sim_key_frame(NULL, release_enter);
    rt_action_update();
    assert(rt_action_pressed(make_str("confirm")) == 0);
    assert(rt_action_held(make_str("confirm")) == 0);
    assert(rt_action_released(make_str("confirm")) == 1);

    rt_action_clear();
}

static void test_chord_release_edge_state() {
    rt_keyboard_init();
    rt_action_init();
    rt_action_clear();

    rt_string save = make_str("save");
    assert(rt_action_define(save) == 1);

    int64_t chord_keys[] = {ZANNA_KEY_LCTRL, ZANNA_KEY_S};
    assert(rt_action_bind_chord(make_str("save"), make_key_seq(chord_keys, 2)) == 1);
    assert(rt_action_chord_count(make_str("save")) == 1);

    int64_t press_ctrl[] = {ZANNA_KEY_LCTRL, -1};
    sim_key_frame(press_ctrl, NULL);
    rt_action_update();
    assert(rt_action_pressed(make_str("save")) == 0);
    assert(rt_action_held(make_str("save")) == 0);
    assert(rt_action_released(make_str("save")) == 0);

    int64_t press_s[] = {ZANNA_KEY_S, -1};
    sim_key_frame(press_s, NULL);
    rt_action_update();
    assert(rt_action_pressed(make_str("save")) == 1);
    assert(rt_action_held(make_str("save")) == 1);
    assert(rt_action_released(make_str("save")) == 0);

    sim_key_frame(NULL, NULL);
    rt_action_update();
    assert(rt_action_pressed(make_str("save")) == 0);
    assert(rt_action_held(make_str("save")) == 1);
    assert(rt_action_released(make_str("save")) == 0);

    int64_t release_s[] = {ZANNA_KEY_S, -1};
    sim_key_frame(NULL, release_s);
    rt_action_update();
    assert(rt_action_pressed(make_str("save")) == 0);
    assert(rt_action_held(make_str("save")) == 0);
    assert(rt_action_released(make_str("save")) == 1);

    int64_t release_ctrl[] = {ZANNA_KEY_LCTRL, -1};
    sim_key_frame(NULL, release_ctrl);
    rt_action_update();
    assert(rt_action_released(make_str("save")) == 0);

    rt_action_clear();
}

// Test: Axis constant getters
static void test_axis_constants() {
    assert(rt_action_axis_left_x() == ZANNA_AXIS_LEFT_X);
    assert(rt_action_axis_left_y() == ZANNA_AXIS_LEFT_Y);
    assert(rt_action_axis_right_x() == ZANNA_AXIS_RIGHT_X);
    assert(rt_action_axis_right_y() == ZANNA_AXIS_RIGHT_Y);
    assert(rt_action_axis_left_trigger() == ZANNA_AXIS_LEFT_TRIGGER);
    assert(rt_action_axis_right_trigger() == ZANNA_AXIS_RIGHT_TRIGGER);
}

// Test: Action system lifecycle
static void test_lifecycle() {
    rt_action_init();

    rt_string test = make_str("test");
    assert(rt_action_define(test) == 1);

    rt_action_shutdown();

    // After shutdown, init should work again
    rt_action_init();

    rt_string test2 = make_str("test");
    assert(rt_action_exists(test2) == 0); // Should be cleared

    rt_action_shutdown();
}

// Test: Empty/null action name handling
static void test_invalid_names() {
    rt_action_init();
    rt_action_clear();

    // NULL name should fail
    assert(rt_action_define(NULL) == 0);
    assert(rt_action_define_axis(NULL) == 0);
    assert(rt_action_exists(NULL) == 0);
    assert(rt_action_remove(NULL) == 0);

    // Empty string should fail
    rt_string empty = rt_str_empty();
    assert(rt_action_define(empty) == 0);
    assert(rt_action_define_axis(empty) == 0);

    rt_action_clear();
}

static void test_invalid_name_encoding_and_embedded_nul() {
    rt_action_init();
    rt_action_clear();

    const char embedded_nul[] = {'b', 'a', 'd', '\0', 'n', 'a', 'm', 'e'};
    rt_string nul_name = rt_string_from_bytes(embedded_nul, sizeof(embedded_nul));
    assert(rt_action_define(nul_name) == 0);
    assert(rt_action_define_axis(nul_name) == 0);

    const unsigned char overlong_utf8[] = {0xC0u, 0xAFu};
    rt_string invalid_utf8 =
        rt_string_from_bytes(reinterpret_cast<const char *>(overlong_utf8), sizeof(overlong_utf8));
    assert(rt_action_define(invalid_utf8) == 0);

    const char valid_utf8[] = "déplacement";
    rt_string valid_name = rt_string_from_bytes(valid_utf8, strlen(valid_utf8));
    assert(rt_action_define_axis(valid_name) == 1);
    assert(rt_action_exists(valid_name) == 1);

    rt_action_clear();
}

static void test_binding_argument_validation() {
    rt_action_init();
    rt_action_clear();
    assert(rt_action_define(make_str("button")) == 1);
    assert(rt_action_define_axis(make_str("axis")) == 1);

    assert(rt_action_bind_key(make_str("button"), ZANNA_KEY_UNKNOWN) == 0);
    assert(rt_action_bind_key(make_str("button"), ZANNA_KEY_MAX) == 0);
    assert(rt_action_bind_key_axis(make_str("axis"), -1, 1.0) == 0);
    assert(rt_action_bind_key_axis(make_str("axis"), ZANNA_KEY_A, NAN) == 0);
    assert(rt_action_bind_key_axis(make_str("axis"), ZANNA_KEY_A, INFINITY) == 0);

    assert(rt_action_bind_mouse(make_str("button"), -1) == 0);
    assert(rt_action_bind_mouse(make_str("button"), ZANNA_MOUSE_BUTTON_MAX) == 0);
    assert(rt_action_bind_mouse_x(make_str("axis"), NAN) == 0);
    assert(rt_action_bind_mouse_y(make_str("axis"), INFINITY) == 0);
    assert(rt_action_bind_scroll_x(make_str("axis"), -INFINITY) == 0);
    assert(rt_action_bind_scroll_y(make_str("axis"), NAN) == 0);

    assert(rt_action_bind_pad_button(make_str("button"), -2, ZANNA_PAD_A) == 0);
    assert(rt_action_bind_pad_button(make_str("button"), ZANNA_PAD_MAX, ZANNA_PAD_A) == 0);
    assert(rt_action_bind_pad_button(make_str("button"), 0, ZANNA_PAD_BUTTON_MAX) == 0);
    assert(rt_action_bind_pad_axis(make_str("axis"), -2, ZANNA_AXIS_LEFT_X, 1.0) == 0);
    assert(rt_action_bind_pad_axis(make_str("axis"), ZANNA_PAD_MAX, ZANNA_AXIS_LEFT_X, 1.0) == 0);
    assert(rt_action_bind_pad_axis(make_str("axis"), 0, -1, 1.0) == 0);
    assert(rt_action_bind_pad_axis(make_str("axis"), 0, ZANNA_AXIS_RIGHT_TRIGGER + 1, 1.0) == 0);
    assert(rt_action_bind_pad_axis(make_str("axis"), 0, ZANNA_AXIS_LEFT_X, NAN) == 0);
    assert(rt_action_bind_pad_button_axis(make_str("axis"), -2, ZANNA_PAD_A, 1.0) == 0);
    assert(rt_action_bind_pad_button_axis(make_str("axis"), 0, ZANNA_PAD_BUTTON_MAX, 1.0) == 0);
    assert(rt_action_bind_pad_button_axis(make_str("axis"), 0, ZANNA_PAD_A, INFINITY) == 0);

    assert(rt_action_binding_count(make_str("button")) == 0);
    assert(rt_action_binding_count(make_str("axis")) == 0);
    assert(rt_str_len(rt_action_key_bound_to(ZANNA_KEY_MAX)) == 0);
    assert(rt_str_len(rt_action_mouse_bound_to(ZANNA_MOUSE_BUTTON_MAX)) == 0);
    assert(rt_str_len(rt_action_pad_button_bound_to(-2, ZANNA_PAD_A)) == 0);

    rt_action_clear();
}

static void test_chord_key_validation() {
    rt_action_init();
    rt_action_clear();
    assert(rt_action_define(make_str("shortcut")) == 1);

    int64_t invalid_keys[] = {ZANNA_KEY_LCTRL, ZANNA_KEY_MAX};
    assert(rt_action_bind_chord(make_str("shortcut"), make_key_seq(invalid_keys, 2)) == 0);

    int64_t duplicate_keys[] = {ZANNA_KEY_LCTRL, ZANNA_KEY_LCTRL};
    assert(rt_action_bind_chord(make_str("shortcut"), make_key_seq(duplicate_keys, 2)) == 0);
    assert(rt_action_chord_count(make_str("shortcut")) == 0);

    int64_t valid_keys[] = {ZANNA_KEY_LCTRL, ZANNA_KEY_S};
    assert(rt_action_bind_chord(make_str("shortcut"), make_key_seq(valid_keys, 2)) == 1);
    assert(rt_action_unbind_chord(make_str("shortcut"), make_key_seq(duplicate_keys, 2)) == 0);
    assert(rt_action_chord_count(make_str("shortcut")) == 1);

    rt_action_clear();
}

static void test_action_rejects_forged_handles_and_corrupt_private_nodes() {
    rt_action_init();
    rt_action_clear();
    void *wrong = rt_box_i64(7);
    rt_string wrong_string = reinterpret_cast<rt_string>(wrong);
    assert(rt_action_define(wrong_string) == 0);
    assert(rt_action_exists(wrong_string) == 0);
    assert(rt_action_remove(wrong_string) == 0);
    assert(rt_action_load_preset(wrong_string) == 0);

    rt_string shortcut = make_str("guarded");
    assert(rt_action_define(shortcut) == 1);
    assert(rt_action_bind_chord(shortcut, wrong) == 0);
    assert(rt_action_unbind_chord(shortcut, wrong) == 0);
    void *wrong_elements = rt_seq_new();
    rt_seq_push(wrong_elements, rt_box_i64(ZANNA_KEY_LCTRL));
    rt_seq_push(wrong_elements, rt_box_f64(1.0));
    assert(rt_action_bind_chord(shortcut, wrong_elements) == 0);
    assert(rt_action_unbind_chord(shortcut, wrong_elements) == 0);
    assert(g_actions != nullptr);

    g_actions->pressed = 2;
    assert(rt_action_exists(shortcut) == 0);
    g_actions->pressed = 0;
    assert(rt_action_bind_key(shortcut, ZANNA_KEY_A) == 1);
    assert(g_actions->bindings != nullptr);
    Binding *binding = g_actions->bindings;
    binding->next = binding;
    rt_action_update();
    assert(rt_action_pressed(shortcut) == 0);
    binding->next = nullptr;

    g_actions->binding_count = 2;
    assert(rt_action_exists(shortcut) == 0);
    assert(rt_action_binding_count(shortcut) == 0);
    rt_action_update();
    g_actions->binding_count = 1;

    uint64_t binding_magic = binding->state_magic;
    binding->state_magic = 0;
    rt_action_update();
    assert(rt_action_pressed(shortcut) == 0);
    binding->state_magic = binding_magic;
    rt_action_clear();
}

static void test_axis_accumulation_saturates_finitely() {
    rt_keyboard_init();
    rt_action_init();
    rt_action_clear();
    assert(rt_action_define_axis(make_str("throttle")) == 1);
    assert(rt_action_bind_key_axis(make_str("throttle"), ZANNA_KEY_A, DBL_MAX) == 1);
    assert(rt_action_bind_key_axis(make_str("throttle"), ZANNA_KEY_A, DBL_MAX) == 1);

    int64_t press_a[] = {ZANNA_KEY_A, -1};
    sim_key_frame(press_a, NULL);
    rt_action_update();
    assert(std::isfinite(rt_action_axis_raw(make_str("throttle"))));
    assert(rt_action_axis_raw(make_str("throttle")) == DBL_MAX);
    assert(rt_action_axis(make_str("throttle")) == 1.0);

    int64_t release_a[] = {ZANNA_KEY_A, -1};
    sim_key_frame(NULL, release_a);
    rt_action_clear();
}

static void test_presets_are_idempotent_and_transactional() {
    rt_action_init();
    rt_action_clear();

    assert(rt_action_load_preset(make_str("platformer")) == 1);
    rt_string first_save = rt_action_save();
    assert(rt_action_load_preset(make_str("platformer")) == 1);
    rt_string second_save = rt_action_save();
    assert(rt_str_len(first_save) == rt_str_len(second_save));
    assert(memcmp(rt_string_cstr(first_save),
                  rt_string_cstr(second_save),
                  (size_t)rt_str_len(first_save)) == 0);

    rt_action_clear();
    assert(rt_action_define(make_str("move_x")) == 1);
    assert(rt_action_define(make_str("sentinel")) == 1);
    rt_string before_conflict = rt_action_save();
    assert(rt_action_load_preset(make_str("platformer")) == 0);
    rt_string after_conflict = rt_action_save();
    assert(rt_str_len(before_conflict) == rt_str_len(after_conflict));
    assert(memcmp(rt_string_cstr(before_conflict),
                  rt_string_cstr(after_conflict),
                  (size_t)rt_str_len(before_conflict)) == 0);

    rt_action_clear();
}

static void test_registry_hash_index_and_exact_list_count() {
    rt_action_init();
    rt_action_clear();

    char name[32];
    for (int i = 0; i < 256; ++i) {
        snprintf(name, sizeof(name), "indexed_action_%d", i);
        assert(rt_action_define(make_str(name)) == 1);
    }
    assert(g_action_count == 256);
    for (int i = 0; i < 256; ++i) {
        snprintf(name, sizeof(name), "indexed_action_%d", i);
        assert(rt_action_exists(make_str(name)) == 1);
    }

    void *names = rt_action_list();
    assert(names != nullptr);
    assert(rt_seq_len(names) == 256);
    assert(rt_action_remove(make_str("indexed_action_127")) == 1);
    assert(g_action_count == 255);
    assert(rt_action_exists(make_str("indexed_action_127")) == 0);
    assert(rt_action_exists(make_str("indexed_action_128")) == 1);

    rt_action_clear();
}

int main() {
    // Initialize keyboard for key name lookups
    rt_keyboard_init();

    test_define_button_action();
    test_define_axis_action();
    test_remove_action();
    test_bind_key();
    test_bind_key_axis();
    test_unbind_key();
    test_bind_mouse();
    test_bind_pad_button();
    test_bind_pad_axis();
    test_multiple_bindings();
    test_bindings_str();
    test_bindings_str_not_truncated();
    test_key_bound_to();
    test_action_edge_state_requires_update();
    test_chord_release_edge_state();
    test_axis_constants();
    test_lifecycle();
    test_invalid_names();
    test_invalid_name_encoding_and_embedded_nul();
    test_binding_argument_validation();
    test_chord_key_validation();
    test_action_rejects_forged_handles_and_corrupt_private_nodes();
    test_axis_accumulation_saturates_finitely();
    test_presets_are_idempotent_and_transactional();
    test_registry_hash_index_and_exact_list_count();

    return 0;
}
