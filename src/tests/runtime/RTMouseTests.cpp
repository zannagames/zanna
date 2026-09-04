//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTMouseTests.cpp
// Purpose: Tests for Zanna.Input.Mouse static class.
// Key invariants:
//   - Extreme coordinates and floating-point input never trigger undefined
//     numeric conversions or wraparound.
//   - Per-frame mouse state remains finite and queryable after malformed input.
// Ownership/Lifetime:
//   - Test hooks borrow synthetic canvas pointers only for the duration of each
//     test and are cleared before returning.
// Links: src/runtime/graphics/input/rt_input.c,
//        src/runtime/graphics/input/rt_input.h
//
//===----------------------------------------------------------------------===//

#include "rt_input.h"
#include "rt_internal.h"

#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

extern "C" void rt_input_set_mouse_warp_hook(void (*hook)(void *canvas, int64_t x, int64_t y));
extern "C" void rt_input_reset_test_hooks(void);
extern "C" void rt_mouse_clear_canvas_if_matches(void *canvas);

static int g_mouse_warp_calls = 0;
static void *g_mouse_warp_canvas = nullptr;
static int64_t g_mouse_warp_x = 0;
static int64_t g_mouse_warp_y = 0;

extern "C" void test_mouse_warp_hook(void *canvas, int64_t x, int64_t y) {
    g_mouse_warp_calls++;
    g_mouse_warp_canvas = canvas;
    g_mouse_warp_x = x;
    g_mouse_warp_y = y;
}

// ============================================================================
// Button Constants
// ============================================================================

static void test_button_constants() {
    // Test button constant getters return expected values
    assert(rt_mouse_button_left() == 0);
    assert(rt_mouse_button_right() == 1);
    assert(rt_mouse_button_middle() == 2);
    assert(rt_mouse_button_x1() == 3);
    assert(rt_mouse_button_x2() == 4);
    printf("test_button_constants: PASSED\n");
}

// ============================================================================
// Initial State
// ============================================================================

static void test_initial_state() {
    rt_mouse_init();

    // Position should be at origin initially
    assert(rt_mouse_x() == 0);
    assert(rt_mouse_y() == 0);
    assert(rt_mouse_delta_x() == 0);
    assert(rt_mouse_delta_y() == 0);

    // All buttons should be up initially
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_RIGHT) == 0);
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_MIDDLE) == 0);
    assert(rt_mouse_is_up(ZANNA_MOUSE_BUTTON_LEFT) == 1);
    assert(rt_mouse_left() == 0);
    assert(rt_mouse_right() == 0);
    assert(rt_mouse_middle() == 0);

    // Wheel should be at zero
    assert(rt_mouse_wheel_x() == 0);
    assert(rt_mouse_wheel_y() == 0);

    // Cursor state
    assert(rt_mouse_is_hidden() == 0);
    assert(rt_mouse_is_captured() == 0);

    printf("test_initial_state: PASSED\n");
}

// ============================================================================
// Position Updates
// ============================================================================

static void test_position_updates() {
    rt_mouse_init();
    rt_mouse_begin_frame();

    // Update position during frame
    rt_mouse_update_pos(100, 200);
    assert(rt_mouse_x() == 100);
    assert(rt_mouse_y() == 200);

    // Delta is calculated at start of begin_frame (x - prev_x)
    // After first begin_frame, delta = 0-0 = 0, and we updated to (100, 200)
    // On second begin_frame, delta = 100-0 = 100
    rt_mouse_begin_frame();
    assert(rt_mouse_delta_x() == 100);
    assert(rt_mouse_delta_y() == 200);

    // Now update to new position
    rt_mouse_update_pos(150, 250);

    // On third begin_frame, delta = 150-100 = 50
    rt_mouse_begin_frame();
    assert(rt_mouse_delta_x() == 50);
    assert(rt_mouse_delta_y() == 50);

    // Move back slightly
    rt_mouse_update_pos(140, 240);

    // On fourth begin_frame, delta = 140-150 = -10
    rt_mouse_begin_frame();
    assert(rt_mouse_delta_x() == -10);
    assert(rt_mouse_delta_y() == -10);

    printf("test_position_updates: PASSED\n");
}

// ============================================================================
// Frame Finalization (same-frame deltas)
// ============================================================================

static void test_finalize_frame_same_frame_delta() {
    rt_mouse_init();

    // Establish a known baseline regardless of state left by earlier tests.
    rt_mouse_begin_frame();
    rt_mouse_update_pos(1000, 2000);
    rt_mouse_finalize_frame();

    // A poll that pumps motion must expose that motion in the SAME frame:
    // finalize recomputes the delta after events instead of lagging one poll.
    rt_mouse_begin_frame();
    rt_mouse_update_pos(1030, 2060);
    rt_mouse_finalize_frame();
    assert(rt_mouse_x() == 1030);
    assert(rt_mouse_delta_x() == 30);
    assert(rt_mouse_delta_y() == 60);

    // A poll with no motion yields a zero delta for this frame.
    rt_mouse_begin_frame();
    rt_mouse_finalize_frame();
    assert(rt_mouse_delta_x() == 0);
    assert(rt_mouse_delta_y() == 0);

    printf("test_finalize_frame_same_frame_delta: PASSED\n");
}

static void test_extreme_position_deltas_saturate() {
    rt_mouse_init();

    rt_mouse_update_pos(0, 0);
    rt_mouse_begin_frame();

    rt_mouse_update_pos(INT64_MIN, INT64_MAX);
    rt_mouse_finalize_frame();
    assert(rt_mouse_delta_x() == INT64_MIN);
    assert(rt_mouse_delta_y() == INT64_MAX);

    rt_mouse_update_pos(INT64_MAX, INT64_MIN);
    rt_mouse_finalize_frame();
    assert(rt_mouse_delta_x() == INT64_MAX);
    assert(rt_mouse_delta_y() == INT64_MIN);

    rt_mouse_update_pos(0, 0);
    rt_mouse_finalize_frame();

    printf("test_extreme_position_deltas_saturate: PASSED\n");
}

static void test_nonfinite_and_extreme_forced_deltas_are_safe() {
    rt_mouse_force_delta_f(std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity());
    assert(rt_mouse_delta_x() == 0);
    assert(rt_mouse_delta_y() == 0);
    assert(rt_mouse_delta_xf() == 0.0);
    assert(rt_mouse_delta_yf() == 0.0);

    rt_mouse_force_delta_f(DBL_MAX, -DBL_MAX);
    assert(rt_mouse_delta_x() == INT64_MAX);
    assert(rt_mouse_delta_y() == INT64_MIN);
    assert(rt_mouse_delta_xf() == DBL_MAX);
    assert(rt_mouse_delta_yf() == -DBL_MAX);

    printf("test_nonfinite_and_extreme_forced_deltas_are_safe: PASSED\n");
}

// ============================================================================
// Button State
// ============================================================================

static void test_button_state() {
    rt_mouse_init();
    rt_mouse_begin_frame();

    // Press left button
    rt_mouse_button_down(ZANNA_MOUSE_BUTTON_LEFT);
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT) == 1);
    assert(rt_mouse_is_up(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_left() == 1);
    assert(rt_mouse_was_pressed(ZANNA_MOUSE_BUTTON_LEFT) == 1);

    // Release left button
    rt_mouse_button_up(ZANNA_MOUSE_BUTTON_LEFT);
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_is_up(ZANNA_MOUSE_BUTTON_LEFT) == 1);
    assert(rt_mouse_left() == 0);
    assert(rt_mouse_was_released(ZANNA_MOUSE_BUTTON_LEFT) == 1);

    // New frame - events should be cleared
    rt_mouse_begin_frame();
    assert(rt_mouse_was_pressed(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_was_released(ZANNA_MOUSE_BUTTON_LEFT) == 0);

    printf("test_button_state: PASSED\n");
}

// ============================================================================
// Click Detection
// ============================================================================

static void test_click_detection() {
    rt_mouse_init();
    rt_mouse_begin_frame();

    // Quick press and release should be a click
    rt_mouse_button_down(ZANNA_MOUSE_BUTTON_LEFT);
    rt_mouse_button_up(ZANNA_MOUSE_BUTTON_LEFT);
    assert(rt_mouse_was_clicked(ZANNA_MOUSE_BUTTON_LEFT) == 1);

    // New frame - click should be cleared
    rt_mouse_begin_frame();
    assert(rt_mouse_was_clicked(ZANNA_MOUSE_BUTTON_LEFT) == 0);

    printf("test_click_detection: PASSED\n");
}

static void test_first_click_is_not_a_double_click() {
    rt_mouse_begin_frame();

    // X2 has not been used by an earlier test, so this is its first click since
    // input initialization even when the complete executable runs in one process.
    rt_mouse_button_down(ZANNA_MOUSE_BUTTON_X2);
    rt_mouse_button_up(ZANNA_MOUSE_BUTTON_X2);
    assert(rt_mouse_was_clicked(ZANNA_MOUSE_BUTTON_X2) == 1);
    assert(rt_mouse_was_double_clicked(ZANNA_MOUSE_BUTTON_X2) == 0);

    printf("test_first_click_is_not_a_double_click: PASSED\n");
}

// ============================================================================
// Scroll Wheel
// ============================================================================

static void test_scroll_wheel() {
    rt_mouse_init();
    rt_mouse_begin_frame();

    // Scroll up
    rt_mouse_update_wheel(0, 3);
    assert(rt_mouse_wheel_x() == 0);
    assert(rt_mouse_wheel_y() == 3);

    // Scroll more
    rt_mouse_update_wheel(2, -1);
    assert(rt_mouse_wheel_x() == 2);
    assert(rt_mouse_wheel_y() == 2);

    // New frame - wheel should reset
    rt_mouse_begin_frame();
    assert(rt_mouse_wheel_x() == 0);
    assert(rt_mouse_wheel_y() == 0);

    printf("test_scroll_wheel: PASSED\n");
}

static void test_scroll_wheel_precision() {
    rt_mouse_init();
    rt_mouse_begin_frame();

    rt_mouse_update_wheel(0.25, 1.5);
    assert(std::fabs(rt_mouse_wheel_xf() - 0.25) < 1e-9);
    assert(std::fabs(rt_mouse_wheel_yf() - 1.5) < 1e-9);
    assert(rt_mouse_wheel_x() == 0);
    assert(rt_mouse_wheel_y() == 1);

    rt_mouse_update_wheel(0.75, -0.25);
    assert(std::fabs(rt_mouse_wheel_xf() - 1.0) < 1e-9);
    assert(std::fabs(rt_mouse_wheel_yf() - 1.25) < 1e-9);
    assert(rt_mouse_wheel_x() == 1);
    assert(rt_mouse_wheel_y() == 1);

    rt_mouse_begin_frame();
    assert(std::fabs(rt_mouse_wheel_xf()) < 1e-9);
    assert(std::fabs(rt_mouse_wheel_yf()) < 1e-9);

    printf("test_scroll_wheel_precision: PASSED\n");
}

static void test_scroll_wheel_rejects_nonfinite_and_saturates() {
    rt_mouse_begin_frame();

    rt_mouse_update_wheel(std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity());
    assert(rt_mouse_wheel_xf() == 0.0);
    assert(rt_mouse_wheel_yf() == 0.0);

    rt_mouse_update_wheel(DBL_MAX, -DBL_MAX);
    rt_mouse_update_wheel(DBL_MAX, -DBL_MAX);
    assert(std::isfinite(rt_mouse_wheel_xf()));
    assert(std::isfinite(rt_mouse_wheel_yf()));
    assert(rt_mouse_wheel_x() == INT64_MAX);
    assert(rt_mouse_wheel_y() == INT64_MIN);

    printf("test_scroll_wheel_rejects_nonfinite_and_saturates: PASSED\n");
}

// ============================================================================
// Cursor Control
// ============================================================================

static void test_cursor_control() {
    rt_mouse_init();
    rt_input_reset_test_hooks();

    // Hide cursor
    rt_mouse_hide();
    assert(rt_mouse_is_hidden() == 1);

    // Show cursor
    rt_mouse_show();
    assert(rt_mouse_is_hidden() == 0);

    // Capture mouse
    rt_mouse_capture();
    assert(rt_mouse_is_captured() == 1);

    // Release mouse
    rt_mouse_release();
    assert(rt_mouse_is_captured() == 0);

    g_mouse_warp_calls = 0;
    g_mouse_warp_canvas = nullptr;
    g_mouse_warp_x = 0;
    g_mouse_warp_y = 0;
    void *canvas = reinterpret_cast<void *>(0x5678);
    rt_input_set_mouse_warp_hook(test_mouse_warp_hook);
    rt_mouse_set_canvas(canvas);

    // Set position
    rt_mouse_set_pos(500, 300);
    assert(rt_mouse_x() == 500);
    assert(rt_mouse_y() == 300);
    assert(g_mouse_warp_calls == 1);
    assert(g_mouse_warp_canvas == canvas);
    assert(g_mouse_warp_x == 500);
    assert(g_mouse_warp_y == 300);

    rt_mouse_set_canvas(nullptr);
    rt_input_reset_test_hooks();

    printf("test_cursor_control: PASSED\n");
}

static void test_canvas_detach() {
    rt_mouse_init();
    rt_input_reset_test_hooks();

    g_mouse_warp_calls = 0;
    g_mouse_warp_canvas = nullptr;
    g_mouse_warp_x = 0;
    g_mouse_warp_y = 0;

    void *canvas_a = reinterpret_cast<void *>(0x5678);
    void *canvas_b = reinterpret_cast<void *>(0x9999);
    rt_input_set_mouse_warp_hook(test_mouse_warp_hook);
    rt_mouse_set_canvas(canvas_a);

    rt_mouse_clear_canvas_if_matches(canvas_b);
    rt_mouse_set_pos(50, 60);
    assert(g_mouse_warp_calls == 1);
    assert(g_mouse_warp_canvas == canvas_a);

    rt_mouse_clear_canvas_if_matches(canvas_a);
    rt_mouse_set_pos(70, 80);
    assert(g_mouse_warp_calls == 1);

    rt_input_reset_test_hooks();
    printf("test_canvas_detach: PASSED\n");
}

static void test_focus_loss_cancels_held_input_and_allows_next_click() {
    rt_mouse_begin_frame();
    rt_mouse_update_pos(20, 12);
    rt_mouse_button_down(ZANNA_MOUSE_BUTTON_LEFT);
    rt_keyboard_on_key_down(ZANNA_KEY_A);
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT) == 1);
    assert(rt_keyboard_is_down(ZANNA_KEY_A) == 1);

    rt_input_focus_lost();
    assert(rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_was_pressed(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_was_released(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_was_clicked(ZANNA_MOUSE_BUTTON_LEFT) == 0);
    assert(rt_mouse_delta_x() == 0);
    assert(rt_mouse_delta_y() == 0);
    assert(rt_keyboard_is_down(ZANNA_KEY_A) == 0);
    assert(rt_keyboard_was_pressed(ZANNA_KEY_A) == 0);
    assert(rt_keyboard_was_released(ZANNA_KEY_A) == 0);

    rt_mouse_button_down(ZANNA_MOUSE_BUTTON_LEFT);
    rt_mouse_button_up(ZANNA_MOUSE_BUTTON_LEFT);
    assert(rt_mouse_was_clicked(ZANNA_MOUSE_BUTTON_LEFT) == 1);
    printf("test_focus_loss_cancels_held_input_and_allows_next_click: PASSED\n");
}

// ============================================================================
// Boundary Cases
// ============================================================================

static void test_boundary_cases() {
    rt_mouse_init();

    // Invalid button indices
    assert(rt_mouse_is_down(-1) == 0);
    assert(rt_mouse_is_down(999) == 0);
    assert(rt_mouse_is_up(-1) == 1);
    assert(rt_mouse_is_up(999) == 1);
    assert(rt_mouse_was_pressed(-1) == 0);
    assert(rt_mouse_was_released(-1) == 0);
    assert(rt_mouse_was_clicked(-1) == 0);
    assert(rt_mouse_was_double_clicked(-1) == 0);

    // Invalid button operations should not crash
    rt_mouse_button_down(-1);
    rt_mouse_button_down(999);
    rt_mouse_button_up(-1);
    rt_mouse_button_up(999);

    printf("test_boundary_cases: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Zanna.Input.Mouse Tests ===\n\n");

    test_button_constants();
    test_initial_state();
    test_position_updates();
    test_finalize_frame_same_frame_delta();
    test_extreme_position_deltas_saturate();
    test_nonfinite_and_extreme_forced_deltas_are_safe();
    test_button_state();
    test_click_detection();
    test_first_click_is_not_a_double_click();
    test_scroll_wheel();
    test_scroll_wheel_precision();
    test_scroll_wheel_rejects_nonfinite_and_saturates();
    test_cursor_control();
    test_canvas_detach();
    test_focus_loss_cancels_held_input_and_allows_next_click();
    test_boundary_cases();

    printf("\nAll tests passed!\n");
    return 0;
}
