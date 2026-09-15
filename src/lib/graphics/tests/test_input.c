//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_input.c
// Purpose: Unit tests covering input events (keyboard/mouse) in ZannaGFX.
// Key invariants: Avoid flakiness by simulating inputs when possible; assert
//                 on event sequencing and data integrity.
// Ownership/Lifetime: Test binary; creates/destroys windows as required.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//

/*
 * ZannaGFX - Input Tests (T16-T21)
 * Tests keyboard, mouse, and event queue with mock backend
 */

#include "test_harness.h"
#include "vgfx.h"
#include "vgfx_internal.h"
#include "vgfx_mock.h"
#include <limits.h>
#include <string.h>

/* T16: Keyboard Input (Mock Backend) */
void test_keyboard_input(void) {
    TEST_BEGIN("T16: Keyboard Input (Mock Backend)");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Inject KEY_DOWN for VGFX_KEY_A */
    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 1);
    vgfx_update(win);

    /* Check key is down */
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 1);

    /* Inject KEY_UP for VGFX_KEY_A */
    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 0);
    vgfx_update(win);

    /* Check key is up */
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T17: Mouse Position (Mock Backend) */
void test_mouse_position(void) {
    TEST_BEGIN("T17: Mouse Position (Mock Backend)");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Case 1: Inside bounds */
    vgfx_mock_inject_mouse_move(win, 150, 200);
    vgfx_update(win);

    int32_t x = 0, y = 0;
    int ok = vgfx_mouse_pos(win, &x, &y);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(x, 150);
    ASSERT_EQ(y, 200);

    /* Case 2: Outside bounds */
    vgfx_mock_inject_mouse_move(win, -10, -10);
    vgfx_update(win);

    ok = vgfx_mouse_pos(win, &x, &y);
    ASSERT_EQ(ok, 0);
    ASSERT_EQ(x, -10);
    ASSERT_EQ(y, -10);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T17b: Mouse Position Uses Logical Coordinates When Scaled */
void test_mouse_position_coord_scale_is_logical(void) {
    TEST_BEGIN("T17b: Mouse Position Coord Scale Is Logical");

    vgfx_mock_set_display_scale(2.0f);
    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "HiDPI", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    vgfx_mock_set_display_scale(1.0f);
    ASSERT_NOT_NULL(win);

    vgfx_set_coord_scale(win, vgfx_window_get_scale(win));

    vgfx_mock_inject_mouse_move(win, 300, 200);
    vgfx_update(win);

    int32_t x = 0, y = 0;
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 150);
    ASSERT_EQ(y, 100);

    vgfx_warp_cursor(win, 160, 120);
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 160);
    ASSERT_EQ(y, 120);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T17c: A centered presentation offset (ADR 0367) shifts public coordinates in
 * both directions and shrinks the public extent to the content. */
void test_coord_transform_centers_public_space(void) {
    TEST_BEGIN("T17c: Coord Transform Centers Public Space");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Letterbox", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* 1x scale, 40 px bars left and right, 20 px above and below. */
    vgfx_set_coord_transform(win, 1.0f, 40, 20, 0, 0);

    int32_t w = 0, h = 0;
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 240);
    ASSERT_EQ(h, 200);

    vgfx_mock_inject_mouse_move(win, 100, 60);
    vgfx_update(win);
    int32_t x = 0, y = 0;
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 60);
    ASSERT_EQ(y, 40);

    /* A pointer inside the left bar is off the content: negative, out of bounds. */
    vgfx_mock_inject_mouse_move(win, 10, 60);
    vgfx_update(win);
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 0);
    ASSERT_EQ(x, -30);

    /* Warping round-trips through the same transform. */
    vgfx_warp_cursor(win, 10, 10);
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 10);
    ASSERT_EQ(y, 10);

    /* Public (0,0) lands on physical (40,20). */
    vgfx_cls(win, VGFX_BLACK);
    vgfx_pset(win, 0, 0, 0xFF0000);
    vgfx_framebuffer_t fb;
    ASSERT_EQ(vgfx_get_framebuffer(win, &fb), 1);
    ASSERT_EQ(fb.pixels[(size_t)20 * (size_t)fb.stride + 40u * 4u], 0xFF);
    ASSERT_EQ(fb.pixels[(size_t)20 * (size_t)fb.stride + 40u * 4u + 1u], 0x00);
    ASSERT_EQ(fb.pixels[0], 0x00);
    vgfx_color_t color = 0;
    ASSERT_EQ(vgfx_point(win, 0, 0, &color), 1);
    ASSERT_EQ(color, 0xFF0000);

    /* A 2x scale composes with the offset: physical (140,120) is public (50,50). */
    vgfx_set_coord_transform(win, 2.0f, 40, 20, 0, 0);
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 120);
    ASSERT_EQ(h, 100);

    /* An explicit content extent wins over the mirrored offset: 1920x1080 over a
     * 1344x872 design scales by 1080/872, whose content rounds to 1665 wide, an
     * odd remainder of 255. The public width must still be the design. */
    vgfx_window_params_t monitor = {
        .width = 1920, .height = 1080, .title = "Monitor", .fps = 0, .resizable = 0};
    vgfx_window_t full = vgfx_create_window(&monitor);
    ASSERT_NOT_NULL(full);
    vgfx_set_coord_transform(full, 1080.0f / 872.0f, 127, 0, 1665, 1080);
    ASSERT_EQ(vgfx_get_size(full, &w, &h), 1);
    ASSERT_EQ(w, 1344);
    ASSERT_EQ(h, 872);
    vgfx_destroy_window(full);
    vgfx_mock_inject_mouse_move(win, 140, 120);
    vgfx_update(win);
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 50);
    ASSERT_EQ(y, 50);

    /* Plain vgfx_set_coord_scale clears the offset (a Canvas3D borrower relies on it). */
    vgfx_set_coord_scale(win, 1.0f);
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 320);
    ASSERT_EQ(h, 240);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T18: Mouse Button (Mock Backend) */
void test_mouse_button(void) {
    TEST_BEGIN("T18: Mouse Button (Mock Backend)");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Inject MOUSE_DOWN for left button */
    vgfx_mock_inject_mouse_button(win, VGFX_MOUSE_LEFT, 1);
    vgfx_update(win);

    ASSERT_EQ(vgfx_mouse_button(win, VGFX_MOUSE_LEFT), 1);

    /* Inject MOUSE_UP for left button */
    vgfx_mock_inject_mouse_button(win, VGFX_MOUSE_LEFT, 0);
    vgfx_update(win);

    ASSERT_EQ(vgfx_mouse_button(win, VGFX_MOUSE_LEFT), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T19: Event Queue – Basic */
void test_event_queue_basic(void) {
    TEST_BEGIN("T19: Event Queue - Basic");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Inject three events */
    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 1);
    vgfx_mock_inject_mouse_move(win, 100, 200);
    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 0);

    /* Update to process platform events */
    vgfx_update(win);

    /* Poll events in order */
    vgfx_event_t ev;
    int ok;

    ok = vgfx_poll_event(win, &ev);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_KEY_DOWN);

    ok = vgfx_poll_event(win, &ev);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_MOUSE_MOVE);

    ok = vgfx_poll_event(win, &ev);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_KEY_UP);

    /* Queue should be empty */
    ok = vgfx_poll_event(win, &ev);
    ASSERT_EQ(ok, 0);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T19b: Public authored-event posting */
void test_event_queue_public_post(void) {
    TEST_BEGIN("T19b: Event Queue - Public Post");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_event_t authored = {0};
    authored.type = VGFX_EVENT_KEY_DOWN;
    authored.time_ms = INT64_C(1234567);
    authored.data.key.key = VGFX_KEY_K;
    authored.data.key.is_repeat = 1;
    authored.data.key.modifiers = VGFX_MOD_CTRL | VGFX_MOD_SHIFT;

    ASSERT_EQ(vgfx_post_event(win, &authored), 1);
    vgfx_event_t observed = {0};
    ASSERT_EQ(vgfx_poll_event(win, &observed), 1);
    ASSERT_EQ(observed.type, VGFX_EVENT_KEY_DOWN);
    ASSERT_EQ(observed.time_ms, INT64_C(1234567));
    ASSERT_EQ(observed.data.key.key, VGFX_KEY_K);
    ASSERT_EQ(observed.data.key.is_repeat, 1);
    ASSERT_EQ(observed.data.key.modifiers, VGFX_MOD_CTRL | VGFX_MOD_SHIFT);
    ASSERT_EQ(vgfx_poll_event(win, &observed), 0);

    authored.type = VGFX_EVENT_NONE;
    ASSERT_EQ(vgfx_post_event(win, &authored), 0);
    authored.type = (vgfx_event_type_t)(VGFX_EVENT_FILE_DROP + 1);
    ASSERT_EQ(vgfx_post_event(win, &authored), 0);
    ASSERT_EQ(vgfx_post_event(NULL, &authored), 0);
    ASSERT_EQ(vgfx_post_event(win, NULL), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T20: Event Queue – Overflow */
void test_event_queue_overflow(void) {
    TEST_BEGIN("T20: Event Queue - Overflow");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Inject VGFX_EVENT_QUEUE_SIZE + 44 events */
    int total_events = VGFX_EVENT_QUEUE_SIZE + 44;
    for (int i = 0; i < total_events; i++) {
        /* Alternate between two different keys to track which are dropped */
        vgfx_key_t key = (i < 44) ? VGFX_KEY_A : VGFX_KEY_B;
        vgfx_mock_inject_key_event(win, key, 1);
    }

    vgfx_update(win);

    /* Count delivered events - should be exactly VGFX_EVENT_QUEUE_SIZE */
    int delivered = 0;
    vgfx_event_t ev;
    while (vgfx_poll_event(win, &ev)) {
        delivered++;
    }
    ASSERT_EQ(delivered, VGFX_EVENT_QUEUE_SIZE);

    /* Check overflow count */
    int32_t overflow = vgfx_event_overflow_count(win);
    ASSERT_EQ(overflow, 44);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_event_queue_flush_and_clear_wrappers(void) {
    TEST_BEGIN("T20b: Event Queue Flush and Clear Wrappers");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_mouse_move(win, 10, 20);
    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 0);
    vgfx_update(win);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_peek_event(win, &ev), 1);
    ASSERT_EQ(vgfx_flush_events(win), 1);
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_KEY_UP);
    ASSERT_EQ(vgfx_poll_event(win, &ev), 0);

    vgfx_mock_inject_mouse_move(win, 10, 20);
    vgfx_update(win);
    ASSERT_EQ(vgfx_peek_event(win, &ev), 1);
    vgfx_clear_events(win);
    ASSERT_EQ(vgfx_poll_event(win, &ev), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_first_limited_update_waits_for_deadline(void) {
    TEST_BEGIN("T20c: First Limited Update Waits for Deadline");

    vgfx_mock_set_time_ms(1000);
    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 60, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    ASSERT_EQ(vgfx_update(win), 1);
    ASSERT_TRUE(vgfx_mock_get_time_ms() >= 1016);
    ASSERT_TRUE(vgfx_frame_time_ms(win) >= 16);

    vgfx_destroy_window(win);
    vgfx_mock_set_time_ms(0);
    TEST_END();
}

/* T21: Resize Event */
void test_resize_event(void) {
    TEST_BEGIN("T21: Resize Event");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 1};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Inject resize to 800x600 */
    vgfx_mock_inject_resize(win, 800, 600);
    vgfx_update(win);

    /* Poll resize event */
    vgfx_event_t ev;
    int ok = vgfx_poll_event(win, &ev);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_RESIZE);
    ASSERT_EQ(ev.data.resize.width, 800);
    ASSERT_EQ(ev.data.resize.height, 600);
    ASSERT_EQ(ev.data.resize.logical_width, 800);
    ASSERT_EQ(ev.data.resize.logical_height, 600);

    /* Check window size updated */
    int32_t w = 0, h = 0;
    ok = vgfx_get_size(win, &w, &h);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(w, 800);
    ASSERT_EQ(h, 600);

    /* Check all pixels are black (framebuffer cleared) */
    vgfx_color_t color;
    for (int32_t y = 0; y < 100; y++) {
        for (int32_t x = 0; x < 100; x++) {
            ok = vgfx_point(win, x, y, &color);
            ASSERT_EQ(ok, 1);
            ASSERT_EQ(color, 0x000000);
        }
    }

    vgfx_destroy_window(win);
    TEST_END();
}

/* T26: Resize Event Logical Dimensions with Coord Scale */
void test_resize_event_scaled_logical(void) {
    TEST_BEGIN("T26: Resize Event Logical Dimensions");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 1};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vgfx_set_coord_scale(win, 1.5f);

    vgfx_mock_inject_resize(win, 500, 333);
    ASSERT_EQ(vgfx_pump_events(win), 1);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_RESIZE);
    ASSERT_EQ(ev.data.resize.width, 500);
    ASSERT_EQ(ev.data.resize.height, 333);
    ASSERT_EQ(ev.data.resize.logical_width, 333);
    ASSERT_EQ(ev.data.resize.logical_height, 222);

    int32_t w = 0, h = 0;
    ASSERT_EQ(vgfx_get_size(win, &w, &h), 1);
    ASSERT_EQ(w, 333);
    ASSERT_EQ(h, 222);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T22: Pump Events Without Present */
void test_pump_events_without_present(void) {
    TEST_BEGIN("T22: Pump Events Without Present");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 1);
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 0);

    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 1);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_KEY_DOWN);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T23: Text Input Event */
void test_text_input_event(void) {
    TEST_BEGIN("T23: Text Input Event");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_text_input(win, 'A');
    ASSERT_EQ(vgfx_pump_events(win), 1);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_TEXT_INPUT);
    ASSERT_EQ(ev.data.text.codepoint, 'A');

    vgfx_destroy_window(win);
    TEST_END();
}

/// @brief Verify allocation-free IME lifecycle payloads and update coalescing.
/// @details Drives the mock adapter through start, two consecutive preedit updates, commit, and
///          cancel. The primary queue must preserve lifecycle boundaries, expose the newest
///          preedit only, retain codepoint selection/replacement metadata, and mark oversized
///          inline UTF-8 deterministically rather than overflowing storage.
void test_composition_lifecycle_events(void) {
    TEST_BEGIN("T23b: IME Composition Lifecycle Events");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "IME", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_composition_start(win, 2, 3);
    vgfx_mock_inject_composition_update(win, "e\xCC\x81", 0, 2);
    vgfx_mock_inject_composition_update(win, "\xE6\xBC\xA2\xE5\xAD\x97", 2, 0);
    vgfx_mock_inject_composition_commit(win, "\xE6\xBC\xA2\xE5\xAD\x97");
    ASSERT_EQ(vgfx_pump_events(win), 1);

    vgfx_event_t event;
    ASSERT_EQ(vgfx_poll_event(win, &event), 1);
    ASSERT_EQ(event.type, VGFX_EVENT_COMPOSITION_START);
    ASSERT_EQ(event.data.composition.replacement_start, 2);
    ASSERT_EQ(event.data.composition.replacement_length, 3);

    ASSERT_EQ(vgfx_poll_event(win, &event), 1);
    ASSERT_EQ(event.type, VGFX_EVENT_COMPOSITION_UPDATE);
    ASSERT_EQ(strcmp(event.data.composition.text, "\xE6\xBC\xA2\xE5\xAD\x97"), 0);
    ASSERT_EQ(event.data.composition.text_length, 6);
    ASSERT_EQ(event.data.composition.selection_start, 2);
    ASSERT_EQ(event.data.composition.selection_length, 0);

    ASSERT_EQ(vgfx_poll_event(win, &event), 1);
    ASSERT_EQ(event.type, VGFX_EVENT_COMPOSITION_COMMIT);
    ASSERT_EQ(strcmp(event.data.composition.text, "\xE6\xBC\xA2\xE5\xAD\x97"), 0);
    ASSERT_EQ(vgfx_poll_event(win, &event), 0);

    vgfx_mock_inject_composition_start(win, -1, -1);
    vgfx_mock_inject_composition_cancel(win);
    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_poll_event(win, &event), 1);
    ASSERT_EQ(event.type, VGFX_EVENT_COMPOSITION_START);
    ASSERT_EQ(event.data.composition.replacement_start, -1);
    ASSERT_EQ(vgfx_poll_event(win, &event), 1);
    ASSERT_EQ(event.type, VGFX_EVENT_COMPOSITION_CANCEL);

    char oversized[VGFX_COMPOSITION_TEXT_CAPACITY + 8];
    memset(oversized, 'x', sizeof(oversized));
    ASSERT_EQ(vgfx_internal_init_composition_event(&event,
                                                   VGFX_EVENT_COMPOSITION_UPDATE,
                                                   10,
                                                   oversized,
                                                   sizeof(oversized),
                                                   0,
                                                   0,
                                                   -1,
                                                   -1,
                                                   0),
              1);
    ASSERT_EQ(event.data.composition.truncated, 1);
    ASSERT_EQ(event.data.composition.text_length, VGFX_COMPOSITION_TEXT_CAPACITY - 1);
    ASSERT_EQ(event.data.composition.text[VGFX_COMPOSITION_TEXT_CAPACITY - 1], '\0');

    vgfx_destroy_window(win);
    TEST_END();
}

/* T24: Scroll Event */
void test_scroll_event(void) {
    TEST_BEGIN("T24: Scroll Event");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_scroll(win, 2.0f, -1.0f, 10, 20);
    ASSERT_EQ(vgfx_pump_events(win), 1);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_SCROLL);
    ASSERT_EQ(ev.data.scroll.delta_x, 2.0f);
    ASSERT_EQ(ev.data.scroll.delta_y, -1.0f);
    ASSERT_EQ(ev.data.scroll.x, 10);
    ASSERT_EQ(ev.data.scroll.y, 20);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T27: Scroll Event Updates Mouse Position */
void test_scroll_updates_mouse_position(void) {
    TEST_BEGIN("T27: Scroll Event Updates Mouse Position");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_mock_inject_scroll(win, 0.25f, -1.5f, 123, 45);
    ASSERT_EQ(vgfx_pump_events(win), 1);

    int32_t x = 0, y = 0;
    ASSERT_EQ(vgfx_mouse_pos(win, &x, &y), 1);
    ASSERT_EQ(x, 123);
    ASSERT_EQ(y, 45);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_SCROLL);
    ASSERT_EQ(ev.data.scroll.x, 123);
    ASSERT_EQ(ev.data.scroll.y, 45);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T28: Mock Fullscreen State Is Per Window */
void test_mock_fullscreen_per_window(void) {
    TEST_BEGIN("T28: Mock Fullscreen State Is Per Window");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t a = vgfx_create_window(&params);
    vgfx_window_t b = vgfx_create_window(&params);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    ASSERT_EQ(vgfx_is_fullscreen(a), 0);
    ASSERT_EQ(vgfx_is_fullscreen(b), 0);

    vgfx_set_fullscreen(a, 1);
    ASSERT_EQ(vgfx_is_fullscreen(a), 1);
    ASSERT_EQ(vgfx_is_fullscreen(b), 0);

    vgfx_set_fullscreen(b, 1);
    ASSERT_EQ(vgfx_is_fullscreen(a), 1);
    ASSERT_EQ(vgfx_is_fullscreen(b), 1);

    vgfx_set_fullscreen(a, 0);
    ASSERT_EQ(vgfx_is_fullscreen(a), 0);
    ASSERT_EQ(vgfx_is_fullscreen(b), 1);

    vgfx_destroy_window(a);
    vgfx_destroy_window(b);
    TEST_END();
}

/* T25: Focus State Sync */
void test_focus_state_sync(void) {
    TEST_BEGIN("T25: Focus State Sync");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    ASSERT_EQ(vgfx_is_focused(win), 1);

    vgfx_mock_inject_key_event(win, VGFX_KEY_A, 1);
    vgfx_mock_inject_mouse_button(win, VGFX_MOUSE_LEFT, 1);
    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 1);
    ASSERT_EQ(vgfx_mouse_button(win, VGFX_MOUSE_LEFT), 1);
    vgfx_clear_events(win);

    vgfx_mock_inject_focus(win, 0);
    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_is_focused(win), 0);
    ASSERT_EQ(vgfx_key_down(win, VGFX_KEY_A), 0);
    ASSERT_EQ(vgfx_mouse_button(win, VGFX_MOUSE_LEFT), 0);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_FOCUS_LOST);

    vgfx_mock_inject_focus(win, 1);
    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_is_focused(win), 1);

    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_FOCUS_GAINED);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T25b: Foreground Request Restores Focus */
void test_request_foreground_restores_focus(void) {
    TEST_BEGIN("T25b: Foreground Request Restores Focus");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    ASSERT_EQ(vgfx_is_focused(win), 1);

    vgfx_mock_inject_focus(win, 0);
    ASSERT_EQ(vgfx_pump_events(win), 1);
    ASSERT_EQ(vgfx_is_focused(win), 0);

    vgfx_request_foreground(win);
    ASSERT_EQ(vgfx_is_focused(win), 1);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_invalid_negative_input_queries_are_safe(void) {
    TEST_BEGIN("T26: Invalid Negative Input Queries Are Safe");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    ASSERT_EQ(vgfx_key_down(win, (vgfx_key_t)-1), 0);
    ASSERT_EQ(vgfx_mouse_button(win, (vgfx_mouse_button_t)-1), 0);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_prevent_close_still_emits_close_event(void) {
    TEST_BEGIN("T27: Prevent Close Still Emits Close Event");

    vgfx_window_params_t params = {
        .width = 320, .height = 240, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_set_prevent_close(win, 1);
    vgfx_mock_inject_close(win);
    vgfx_update(win);

    vgfx_event_t ev;
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_CLOSE);
    ASSERT_EQ(vgfx_close_requested(win), 0);

    vgfx_set_prevent_close(win, 0);
    vgfx_mock_inject_close(win);
    vgfx_update(win);
    ASSERT_EQ(vgfx_poll_event(win, &ev), 1);
    ASSERT_EQ(ev.type, VGFX_EVENT_CLOSE);
    ASSERT_EQ(vgfx_close_requested(win), 1);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_event_overflow_counter_saturates(void) {
    TEST_BEGIN("T31: Event Overflow Counter Saturates");

    vgfx_window_params_t params = {
        .width = 64, .height = 64, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t public_win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(public_win);

    struct vgfx_window *win = (struct vgfx_window *)public_win;
    win->event_overflow = INT32_MAX;
    win->event_head = 0;
    win->event_tail = 1;
    win->event_queue[win->event_tail].type = VGFX_EVENT_KEY_DOWN;

    vgfx_event_t event = {.type = VGFX_EVENT_MOUSE_MOVE};
    ASSERT_EQ(vgfx_internal_enqueue_event(win, &event), 1);
    ASSERT_EQ(win->event_overflow, INT32_MAX);

    vgfx_destroy_window(public_win);
    TEST_END();
}

/* Main test runner */
/// What: Entry point for input tests covering key/mouse event handling.
/// Why:  Validate that the input subsystem reports and sequences events
///       correctly under typical usage.
/// How:  Creates a window, simulates or listens for events, and asserts on
///       observed behavior.
/* T22: Relative (raw) mouse mode — mock backend accumulator semantics */
void test_relative_mouse_mode(void) {
    TEST_BEGIN("T22: Relative Mouse Mode (Mock Backend)");

    vgfx_window_params_t params = {
        .width = 640, .height = 480, .title = "Rel", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    /* Off by default: no native mode, drains read zero. */
    ASSERT_EQ(vgfx_relative_mouse_native(win), 0);
    double dx = 99.0, dy = 99.0;
    vgfx_get_relative_deltas(win, &dx, &dy);
    ASSERT_EQ((int)dx, 0);
    ASSERT_EQ((int)dy, 0);

    /* Enable: mock reports native raw support. */
    ASSERT_EQ(vgfx_set_relative_mouse(win, 1), 1);
    ASSERT_EQ(vgfx_relative_mouse_native(win), 1);

    /* Sub-pixel deltas accumulate across pushes and drain-and-clear. */
    vgfx_mock_push_relative_delta(win, 3.5, -2.25);
    vgfx_mock_push_relative_delta(win, 3.5, -2.25);
    vgfx_mock_push_relative_delta(win, 3.5, -2.25);
    vgfx_get_relative_deltas(win, &dx, &dy);
    ASSERT_EQ((int)(dx * 100.0), 1050);
    ASSERT_EQ((int)(dy * 100.0), -675);

    /* Second drain reads zero (read-and-clear). */
    vgfx_get_relative_deltas(win, &dx, &dy);
    ASSERT_EQ((int)dx, 0);
    ASSERT_EQ((int)dy, 0);

    /* Disable clears any pending accumulation and native state. */
    vgfx_mock_push_relative_delta(win, 7.0, 7.0);
    ASSERT_EQ(vgfx_set_relative_mouse(win, 0), 1);
    ASSERT_EQ(vgfx_relative_mouse_native(win), 0);
    vgfx_get_relative_deltas(win, &dx, &dy);
    ASSERT_EQ((int)dx, 0);
    ASSERT_EQ((int)dy, 0);

    vgfx_destroy_window(win);
    TEST_END();
}

int main(void) {
    printf("========================================\n");
    printf("ZannaGFX Input Tests (T16-T21)\n");
    printf("========================================\n");

    test_keyboard_input();
    test_mouse_position();
    test_mouse_position_coord_scale_is_logical();
    test_coord_transform_centers_public_space();
    test_mouse_button();
    test_event_queue_basic();
    test_event_queue_public_post();
    test_event_queue_overflow();
    test_event_queue_flush_and_clear_wrappers();
    test_first_limited_update_waits_for_deadline();
    test_resize_event();
    test_resize_event_scaled_logical();
    test_pump_events_without_present();
    test_text_input_event();
    test_composition_lifecycle_events();
    test_scroll_event();
    test_scroll_updates_mouse_position();
    test_focus_state_sync();
    test_request_foreground_restores_focus();
    test_mock_fullscreen_per_window();
    test_invalid_negative_input_queries_are_safe();
    test_prevent_close_still_emits_close_event();
    test_event_overflow_counter_saturates();
    test_relative_mouse_mode();

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
// File: src/lib/graphics/tests/test_input.c
// Purpose: Unit tests covering input events (keyboard/mouse) in ZannaGFX.
// Key invariants: Avoid flakiness by simulating inputs when possible; assert
//                 on event sequencing and data integrity.
// Ownership/Lifetime: Test binary; creates/destroys windows as required.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//
