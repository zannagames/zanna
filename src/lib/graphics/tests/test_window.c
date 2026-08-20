//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_window.c
// Purpose: Unit tests covering window creation, resize, and teardown flows.
// Key invariants: Windows are destroyed on all paths; events are processed
//                 without deadlocks; resources do not leak.
// Ownership/Lifetime: Test binary; owns windows created during tests.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//

/*
 * ZannaGFX - Window Tests (T1-T3)
 * Tests window creation with various parameters
 */

#include "test_harness.h"
#include "vgfx.h"
#include "vgfx_mock.h"
#include <stdlib.h>
#include <string.h>

/* T1: Window Creation – Valid Parameters */
void test_window_valid_params(void) {
    TEST_BEGIN("T1: Window Creation - Valid Parameters");

    vgfx_window_params_t params = {
        .width = 800, .height = 600, .title = "Test", .fps = 60, .resizable = 1};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    int32_t w = 0, h = 0;
    int ok = vgfx_get_size(win, &w, &h);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(w, 800);
    ASSERT_EQ(h, 600);

    ok = vgfx_update(win);
    ASSERT_EQ(ok, 1);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T2: Window Creation – Dimensions Exceed Max */
void test_window_exceed_max(void) {
    TEST_BEGIN("T2: Window Creation - Dimensions Exceed Max");

    vgfx_window_params_t params = {
        .width = 5000, .height = 5000, .title = "Test", .fps = 60, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NULL(win);

    const char *error = vgfx_get_last_error();
    ASSERT_NOT_NULL(error);
    ASSERT_TRUE(strstr(error, "exceed maximum") != NULL);

    TEST_END();
}

/* T3: Window Creation – Invalid Dimensions Use Defaults */
void test_window_invalid_dimensions_use_defaults(void) {
    TEST_BEGIN("T3: Window Creation - Invalid Dimensions Use Defaults");

    vgfx_window_params_t params = {
        .width = 0, .height = -10, .title = "Test", .fps = 60, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    int32_t w = 0, h = 0;
    int ok = vgfx_get_size(win, &w, &h);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(w, VGFX_DEFAULT_WIDTH);
    ASSERT_EQ(h, VGFX_DEFAULT_HEIGHT);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_hidpi_canvas_logical_size_contract(void) {
    TEST_BEGIN("Audit: HiDPI Canvas Logical Size Contract");

    vgfx_mock_set_display_scale(2.0f);
    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "HiDPI", .fps = -1, .resizable = 1};

    vgfx_window_t win = vgfx_create_window(&params);
    vgfx_mock_set_display_scale(1.0f);
    ASSERT_NOT_NULL(win);

    vgfx_framebuffer_t fb;
    ASSERT_EQ(vgfx_get_framebuffer(win, &fb), 1);
    ASSERT_EQ(fb.width, 640);
    ASSERT_EQ(fb.height, 480);

    int32_t w = 0;
    int32_t h = 0;
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 640);
    ASSERT_EQ(h, 480);

    vgfx_set_coord_scale(win, vgfx_window_get_scale(win));
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 320);
    ASSERT_EQ(h, 240);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_hidpi_resize_reports_physical_and_logical_size(void) {
    TEST_BEGIN("Audit: HiDPI Resize Physical and Logical Size");

    vgfx_mock_set_display_scale(2.0f);
    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "HiDPI", .fps = -1, .resizable = 1};

    vgfx_window_t win = vgfx_create_window(&params);
    vgfx_mock_set_display_scale(1.0f);
    ASSERT_NOT_NULL(win);
    vgfx_set_coord_scale(win, vgfx_window_get_scale(win));

    vgfx_mock_inject_resize(win, 1000, 750);
    ASSERT_EQ(vgfx_pump_events(win), 1);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_RESIZE);
    ASSERT_EQ(ev.data.resize.width, 1000);
    ASSERT_EQ(ev.data.resize.height, 750);
    ASSERT_EQ(ev.data.resize.logical_width, 500);
    ASSERT_EQ(ev.data.resize.logical_height, 375);

    int32_t w = 0;
    int32_t h = 0;
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 500);
    ASSERT_EQ(h, 375);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_monitor_size_allows_null_window(void) {
    TEST_BEGIN("Audit: Monitor Size Allows Null Window");

    int32_t w = -1;
    int32_t h = -1;
    vgfx_get_monitor_size(NULL, &w, &h);
    ASSERT_EQ(w, 1920);
    ASSERT_EQ(h, 1080);

    TEST_END();
}

void test_headless_clipboard_round_trip(void) {
    TEST_BEGIN("Audit: Headless Clipboard Round Trip");

    vgfx_clipboard_clear();
    ASSERT_EQ(vgfx_clipboard_has_format(VGFX_CLIPBOARD_TEXT), 0);
    vgfx_clipboard_set_text("Zanna headless clipboard");
    ASSERT_EQ(vgfx_clipboard_has_format(VGFX_CLIPBOARD_TEXT), 1);

    char *text = vgfx_clipboard_get_text();
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strcmp(text, "Zanna headless clipboard") == 0);
    free(text);

    vgfx_clipboard_clear();
    ASSERT_EQ(vgfx_clipboard_has_format(VGFX_CLIPBOARD_TEXT), 0);
    TEST_END();
}

void test_event_wait_explicit_wake(void) {
    TEST_BEGIN("Event Wait Returns For Explicit Wake");

    vgfx_window_params_t params = vgfx_window_params_default();
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vgfx_mock_set_time_ms(1000);
    ASSERT_EQ(vgfx_wake_events(win), 1);
    ASSERT_EQ(vgfx_wait_events(win, 5000), 1);
    ASSERT_EQ(vgfx_mock_get_time_ms(), 1000);
    ASSERT_EQ(vgfx_wait_events(win, 0), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_headless_native_handles_are_typed_none(void) {
    TEST_BEGIN("Audit: Headless Native Handles Are Typed None");

    vgfx_native_handles_t handles = {.backend = VGFX_NATIVE_BACKEND_WAYLAND,
                                     .display = (void *)(uintptr_t)1,
                                     .surface = (void *)(uintptr_t)1,
                                     .window = 1};
    ASSERT_EQ(vgfx_get_native_handles(NULL, &handles), 0);
    ASSERT_EQ(vgfx_get_native_handles(NULL, NULL), 0);

    vgfx_window_params_t params = vgfx_window_params_default();
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    ASSERT_EQ(vgfx_get_native_handles(win, NULL), 0);
    ASSERT_EQ(vgfx_get_native_handles(win, &handles), 1);
    ASSERT_EQ(handles.backend, VGFX_NATIVE_BACKEND_NONE);
    ASSERT_NULL(handles.display);
    ASSERT_NULL(handles.surface);
    ASSERT_EQ(handles.window, 0);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_text_input_state_contract(void) {
    TEST_BEGIN("Text Input State Validates Offsets And Geometry");
    vgfx_window_params_t params = vgfx_window_params_default();
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    ASSERT_EQ(vgfx_set_text_input_enabled(win, 1), 1);
    ASSERT_EQ(vgfx_set_text_input_enabled(win, 2), 0);
    vgfx_text_input_state_t state = {.surrounding_text = "caf\xC3\xA9",
                                     .cursor_byte = 5,
                                     .anchor_byte = 3,
                                     .cursor_x = 20,
                                     .cursor_y = 10,
                                     .cursor_width = 2,
                                     .cursor_height = 18,
                                     .purpose = VGFX_TEXT_INPUT_NORMAL};
    ASSERT_EQ(vgfx_set_text_input_state(win, &state), 1);
    state.cursor_byte = 6;
    ASSERT_EQ(vgfx_set_text_input_state(win, &state), 0);
    state.cursor_byte = 5;
    state.cursor_height = -1;
    ASSERT_EQ(vgfx_set_text_input_state(win, &state), 0);
    ASSERT_EQ(vgfx_set_text_input_enabled(win, 0), 1);
    vgfx_destroy_window(win);
    TEST_END();
}

/* Main test runner */
/// What: Entry point for window lifecycle tests.
/// Why:  Validate that window create/resize/teardown flows are robust.
/// How:  Creates a window, triggers resizes/events, then cleans up deterministically.
int main(void) {
    printf("========================================\n");
    printf("ZannaGFX Window Tests (T1-T3)\n");
    printf("========================================\n");

    test_window_valid_params();
    test_window_exceed_max();
    test_window_invalid_dimensions_use_defaults();
    test_hidpi_canvas_logical_size_contract();
    test_hidpi_resize_reports_physical_and_logical_size();
    test_monitor_size_allows_null_window();
    test_headless_clipboard_round_trip();
    test_event_wait_explicit_wake();
    test_headless_native_handles_are_typed_none();
    test_text_input_state_contract();

    TEST_SUMMARY();
    return TEST_RETURN_CODE();
}

//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_window.c
// Purpose: Unit tests covering window creation, resize, and teardown flows.
// Key invariants: Windows are destroyed on all paths; events are processed
//                 without deadlocks; resources do not leak.
// Ownership/Lifetime: Test binary; owns windows created during tests.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//
