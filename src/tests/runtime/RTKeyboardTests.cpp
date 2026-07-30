//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTKeyboardTests.cpp
// Purpose: Validate Zanna.Input.Key constants and frame-coherent keyboard APIs.
// Key invariants:
//   - Public and backend key vocabularies map without collisions.
//   - Pressed/released snapshots contain each key at most once per frame.
//   - Text input retains valid UTF-8 and rejects controls/private-use scalars.
// Ownership/Lifetime:
//   - Legacy short-lived runtime handles are process-scoped in this executable;
//     the high-churn regression explicitly releases its snapshot sequences.
//   - Platform query hooks borrow synthetic canvas pointers for one test scope.
// Links: src/runtime/graphics/input/rt_input.c,
//        src/runtime/graphics/input/rt_input.h,
//        docs/adr/0169-super-modifier-keys-and-studio-viewport-picking.md
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstdio>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

extern "C" void rt_input_set_caps_lock_query_hook(int32_t (*hook)(void *canvas));
extern "C" void rt_input_reset_test_hooks(void);
extern "C" void rt_keyboard_clear_canvas_if_matches(void *canvas);

static int g_caps_lock_query_calls = 0;
static void *g_caps_lock_canvas = nullptr;
static int32_t g_caps_lock_value = 0;

extern "C" int32_t test_caps_lock_query_hook(void *canvas) {
    g_caps_lock_query_calls++;
    g_caps_lock_canvas = canvas;
    return g_caps_lock_value;
}

// ============================================================================
// Key Code Constants
// ============================================================================

static void test_key_constants() {
    // Test that key code getters return expected GLFW-compatible values
    assert(rt_keyboard_key_a() == 65);
    assert(rt_keyboard_key_z() == 90);
    assert(rt_keyboard_key_0() == 48);
    assert(rt_keyboard_key_9() == 57);
    assert(rt_keyboard_key_space() == 32);
    assert(rt_keyboard_key_enter() == 257);
    assert(rt_keyboard_key_escape() == 256);
    assert(rt_keyboard_key_up() == 265);
    assert(rt_keyboard_key_down() == 264);
    assert(rt_keyboard_key_left() == 263);
    assert(rt_keyboard_key_right() == 262);
    assert(rt_keyboard_key_f1() == 290);
    assert(rt_keyboard_key_f12() == 301);
    assert(rt_keyboard_key_lshift() == 340);
    assert(rt_keyboard_key_lctrl() == 341);
    assert(rt_keyboard_key_lalt() == 342);
    assert(rt_keyboard_key_lsuper() == 343);
    assert(rt_keyboard_key_rshift() == 344);
    assert(rt_keyboard_key_rctrl() == 345);
    assert(rt_keyboard_key_ralt() == 346);
    assert(rt_keyboard_key_rsuper() == 347);
    printf("test_key_constants: PASSED\n");
}

static void test_vgfx_special_key_translation() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();

    // vgfx special-key values differ from the public Zanna.Input key constants.
    rt_keyboard_on_vgfx_key_down(264); // VGFX_KEY_TAB
    assert(rt_keyboard_was_pressed(rt_keyboard_key_tab()) == 1);
    assert(rt_keyboard_was_pressed(rt_keyboard_key_down()) == 0);

    rt_keyboard_on_vgfx_key_down(262); // VGFX_KEY_BACKSPACE
    assert(rt_keyboard_was_pressed(rt_keyboard_key_backspace()) == 1);
    assert(rt_keyboard_was_pressed(rt_keyboard_key_right()) == 0);

    rt_keyboard_on_vgfx_key_down(263); // VGFX_KEY_DELETE
    assert(rt_keyboard_was_pressed(rt_keyboard_key_delete()) == 1);
    assert(rt_keyboard_was_pressed(rt_keyboard_key_left()) == 0);

    rt_keyboard_on_vgfx_key_up(264);
    assert(rt_keyboard_was_released(rt_keyboard_key_tab()) == 1);
    rt_keyboard_on_vgfx_key_up(262);
    rt_keyboard_on_vgfx_key_up(263);

    rt_keyboard_on_vgfx_key_down(343); // VG_KEY_LEFT_SUPER
    assert(rt_keyboard_was_pressed(rt_keyboard_key_lsuper()) == 1);
    assert(rt_keyboard_is_down(rt_keyboard_key_lsuper()) == 1);
    rt_keyboard_on_vgfx_key_up(343);
    assert(rt_keyboard_was_released(rt_keyboard_key_lsuper()) == 1);

    rt_keyboard_on_vgfx_key_down(347); // VG_KEY_RIGHT_SUPER
    assert(rt_keyboard_was_pressed(rt_keyboard_key_rsuper()) == 1);
    assert(rt_keyboard_is_down(rt_keyboard_key_rsuper()) == 1);
    rt_keyboard_on_vgfx_key_up(347);
    assert(rt_keyboard_was_released(rt_keyboard_key_rsuper()) == 1);

    printf("test_vgfx_special_key_translation: PASSED\n");
}

// ============================================================================
// Keyboard State - Initial State
// ============================================================================

static void test_initial_state() {
    // Initialize keyboard system
    rt_keyboard_init();

    // All keys should be up initially
    assert(rt_keyboard_is_down(rt_keyboard_key_a()) == 0);
    assert(rt_keyboard_is_up(rt_keyboard_key_a()) == 1);
    assert(rt_keyboard_any_down() == 0);
    assert(rt_keyboard_get_down() == 0);
    printf("test_initial_state: PASSED\n");
}

// ============================================================================
// Key Press/Release Events
// ============================================================================

static void test_key_press_release() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();

    // Simulate key down event (using GLFW key code directly for testing)
    int64_t key_a = rt_keyboard_key_a();

    // Initially up
    assert(rt_keyboard_is_down(key_a) == 0);
    assert(rt_keyboard_is_up(key_a) == 1);

    // Simulate press - the on_key_down function expects vgfx codes, but we can
    // test the state tracking functions directly by accessing internal state
    // For now, we'll test the GetPressed/GetReleased functionality

    printf("test_key_press_release: PASSED\n");
}

// ============================================================================
// Per-Frame Event Tracking
// ============================================================================

static void test_frame_events() {
    rt_keyboard_init();

    // Begin a new frame - should reset pressed/released lists
    rt_keyboard_begin_frame();

    // GetPressed and GetReleased should return empty sequences
    void *pressed = rt_keyboard_get_pressed();
    void *released = rt_keyboard_get_released();

    assert(pressed != nullptr);
    assert(released != nullptr);
    assert(rt_seq_len(pressed) == 0);
    assert(rt_seq_len(released) == 0);

    printf("test_frame_events: PASSED\n");
}

static void test_frame_edges_are_unique_and_bounded() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();

    const int64_t key_a = rt_keyboard_key_a();
    const int64_t key_z = rt_keyboard_key_z();
    for (int iteration = 0; iteration < 10000; ++iteration) {
        rt_keyboard_on_key_down(key_a);
        rt_keyboard_on_key_up(key_a);
    }
    rt_keyboard_on_key_down(key_z);
    rt_keyboard_on_key_up(key_z);

    void *pressed = rt_keyboard_get_pressed();
    void *released = rt_keyboard_get_released();
    assert(pressed != nullptr);
    assert(released != nullptr);
    assert(rt_seq_len(pressed) == 2);
    assert(rt_seq_len(released) == 2);
    assert(rt_unbox_i64(rt_seq_get(pressed, 0)) == key_a);
    assert(rt_unbox_i64(rt_seq_get(pressed, 1)) == key_z);
    assert(rt_unbox_i64(rt_seq_get(released, 0)) == key_a);
    assert(rt_unbox_i64(rt_seq_get(released, 1)) == key_z);
    assert(rt_keyboard_was_pressed(key_a) == 1);
    assert(rt_keyboard_was_released(key_a) == 1);
    assert(rt_keyboard_is_up(key_a) == 1);

    if (rt_obj_release_check0(pressed))
        rt_obj_free(pressed);
    if (rt_obj_release_check0(released))
        rt_obj_free(released);

    printf("test_frame_edges_are_unique_and_bounded: PASSED\n");
}

// ============================================================================
// Key Name Helper
// ============================================================================

static void test_key_name() {
    // Test key name lookup
    rt_string name_a = rt_keyboard_key_name(rt_keyboard_key_a());
    assert(name_a != nullptr);
    // Should return "A"
    assert(rt_str_len(name_a) == 1);

    rt_string name_space = rt_keyboard_key_name(rt_keyboard_key_space());
    assert(name_space != nullptr);
    // Should return "Space"
    assert(rt_str_len(name_space) == 5);

    rt_string name_enter = rt_keyboard_key_name(rt_keyboard_key_enter());
    assert(name_enter != nullptr);
    // Should return "Enter"
    assert(rt_str_len(name_enter) == 5);

    rt_string name_f1 = rt_keyboard_key_name(rt_keyboard_key_f1());
    assert(name_f1 != nullptr);
    // Should return "F1"
    assert(rt_str_len(name_f1) == 2);

    rt_string name_left_super = rt_keyboard_key_name(rt_keyboard_key_lsuper());
    assert(name_left_super != nullptr);
    assert(std::strcmp(rt_string_cstr(name_left_super), "Left Super") == 0);

    rt_string name_right_super = rt_keyboard_key_name(rt_keyboard_key_rsuper());
    assert(name_right_super != nullptr);
    assert(std::strcmp(rt_string_cstr(name_right_super), "Right Super") == 0);

    // Unknown key should return "Unknown"
    rt_string name_unknown = rt_keyboard_key_name(-999);
    assert(name_unknown != nullptr);

    printf("test_key_name: PASSED\n");
}

// ============================================================================
// Modifier State
// ============================================================================

static void test_modifier_state() {
    rt_keyboard_init();

    // Initially all modifiers should be off
    assert(rt_keyboard_shift() == 0);
    assert(rt_keyboard_ctrl() == 0);
    assert(rt_keyboard_alt() == 0);
    // CapsLock state is platform-dependent, skip testing its initial value

    printf("test_modifier_state: PASSED\n");
}

static void test_caps_lock_query() {
    rt_input_reset_test_hooks();
    rt_keyboard_init();

    g_caps_lock_query_calls = 0;
    g_caps_lock_canvas = nullptr;
    g_caps_lock_value = 1;

    void *canvas = reinterpret_cast<void *>(0x1234);
    rt_input_set_caps_lock_query_hook(test_caps_lock_query_hook);
    rt_keyboard_set_canvas(canvas);

    assert(g_caps_lock_query_calls == 1);
    assert(g_caps_lock_canvas == canvas);
    assert(rt_keyboard_caps_lock() == 1);
    assert(g_caps_lock_query_calls == 2);

    g_caps_lock_value = 0;
    assert(rt_keyboard_caps_lock() == 0);
    assert(g_caps_lock_query_calls == 3);
    assert(g_caps_lock_canvas == canvas);

    rt_keyboard_set_canvas(nullptr);
    rt_input_reset_test_hooks();

    printf("test_caps_lock_query: PASSED\n");
}

static void test_canvas_detach() {
    rt_input_reset_test_hooks();
    rt_keyboard_init();

    g_caps_lock_query_calls = 0;
    g_caps_lock_canvas = nullptr;
    g_caps_lock_value = 1;

    void *canvas_a = reinterpret_cast<void *>(0x1111);
    void *canvas_b = reinterpret_cast<void *>(0x2222);
    rt_input_set_caps_lock_query_hook(test_caps_lock_query_hook);
    rt_keyboard_set_canvas(canvas_a);

    rt_keyboard_clear_canvas_if_matches(canvas_b);
    assert(rt_keyboard_caps_lock() == 1);
    assert(g_caps_lock_canvas == canvas_a);

    rt_keyboard_clear_canvas_if_matches(canvas_a);
    g_caps_lock_canvas = canvas_b;
    assert(rt_keyboard_caps_lock() == 1);
    assert(g_caps_lock_canvas == nullptr);

    rt_input_reset_test_hooks();
    printf("test_canvas_detach: PASSED\n");
}

// ============================================================================
// Text Input
// ============================================================================

static void test_text_input() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();

    // Initially text input is disabled, so GetText should return empty
    rt_string text = rt_keyboard_get_text();
    assert(text != nullptr);
    assert(rt_str_len(text) == 0);

    // Enable text input
    rt_keyboard_enable_text_input();

    // Disable text input
    rt_keyboard_disable_text_input();

    printf("test_text_input: PASSED\n");
}

static void test_text_input_utf8() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();
    rt_keyboard_enable_text_input();

    rt_keyboard_text_input(0x00E9);  // e acute
    rt_keyboard_text_input(0x1F642); // slightly smiling face

    rt_string text = rt_keyboard_get_text();
    assert(text != nullptr);
    assert(rt_str_len(text) == 6);
    assert(std::strcmp(rt_string_cstr(text), "\xC3\xA9\xF0\x9F\x99\x82") == 0);

    rt_keyboard_disable_text_input();

    printf("test_text_input_utf8: PASSED\n");
}

static void test_text_input_rejects_private_use_function_keys() {
    rt_keyboard_init();
    rt_keyboard_begin_frame();
    rt_keyboard_enable_text_input();

    rt_keyboard_text_input(0xF700); // macOS NSUpArrowFunctionKey
    rt_keyboard_text_input(0xF703); // macOS NSRightArrowFunctionKey
    rt_keyboard_text_input(0xE000); // Unicode private-use area
    rt_keyboard_text_input(0x0085); // C1 control

    rt_string text = rt_keyboard_get_text();
    assert(text != nullptr);
    assert(rt_str_len(text) == 0);

    rt_keyboard_text_input('x');
    text = rt_keyboard_get_text();
    assert(text != nullptr);
    assert(rt_str_len(text) == 1);
    assert(std::strcmp(rt_string_cstr(text), "x") == 0);

    rt_keyboard_disable_text_input();

    printf("test_text_input_rejects_private_use_function_keys: PASSED\n");
}

// ============================================================================
// Boundary Cases
// ============================================================================

static void test_boundary_cases() {
    rt_keyboard_init();

    // Test invalid key codes
    assert(rt_keyboard_is_down(-1) == 0);
    assert(rt_keyboard_is_down(9999) == 0);
    assert(rt_keyboard_is_up(-1) == 1);
    assert(rt_keyboard_is_up(9999) == 1);
    assert(rt_keyboard_was_pressed(-1) == 0);
    assert(rt_keyboard_was_released(-1) == 0);

    printf("test_boundary_cases: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Zanna.Input.Keyboard Tests ===\n\n");

    test_key_constants();
    test_vgfx_special_key_translation();
    test_initial_state();
    test_key_press_release();
    test_frame_events();
    test_frame_edges_are_unique_and_bounded();
    test_key_name();
    test_modifier_state();
    test_caps_lock_query();
    test_canvas_detach();
    test_text_input();
    test_text_input_utf8();
    test_text_input_rejects_private_use_function_keys();
    test_boundary_cases();

    printf("\nAll tests passed!\n");
    return 0;
}
