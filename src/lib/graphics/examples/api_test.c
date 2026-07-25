//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/examples/api_test.c
// Purpose: ZannaGFX example used to exercise API surface for sanity checks.
// Key invariants: Avoids undefined behavior; reports failures to stderr; exits
//                 non-zero on error.
// Ownership/Lifetime: Demonstration program; owns and releases created
//                     resources within main.
// Links: docs/vgfx.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Sanity-check example covering the core ZannaGFX C API.
/// @details Creates a small window, verifies framebuffer layout and selected
///          stateful operations, exercises primitive drawing and color helpers,
///          samples empty input/event state, presents one frame, and releases
///          the owned window. Failures are reported immediately to stderr.

/*
 * ZannaGFX API Test
 * Tests core API functionality without requiring platform backend
 */

#include <stdio.h>
#include <string.h>
#include <vgfx.h>

/// @brief Exercise representative window, framebuffer, drawing, input, and event APIs.
/// @details The routine intentionally performs explicit value checks after each
///          state-changing call so an ABI mismatch or backend contract failure
///          is localized to the first affected operation. The created window is
///          destroyed on the successful path.
/// @return 0 when every check succeeds; 1 after reporting the first failure.
int main(void) {
    printf("=== ZannaGFX API Test ===\n");
    printf("Version: %s\n\n", vgfx_version_string());

    /* Test 1: Window creation */
    printf("Test 1: Creating window...\n");
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 320;
    params.height = 240;
    params.title = "Test Window";

    vgfx_window_t win = vgfx_create_window(&params);
    if (!win) {
        fprintf(stderr, "FAIL: Window creation failed: %s\n", vgfx_get_last_error());
        return 1;
    }
    printf("PASS: Window created\n\n");

    /* Test 2: Get window size */
    printf("Test 2: Getting window size...\n");
    int32_t w, h;
    vgfx_get_size(win, &w, &h);
    if (w != 320 || h != 240) {
        fprintf(stderr, "FAIL: Expected 320x240, got %dx%d\n", w, h);
        return 1;
    }
    printf("PASS: Size = %dx%d\n\n", w, h);

    /* Test 3: FPS settings */
    printf("Test 3: FPS settings...\n");
    vgfx_set_fps(win, 30);
    int32_t fps = vgfx_get_fps(win);
    if (fps != 30) {
        fprintf(stderr, "FAIL: Expected FPS=30, got %d\n", fps);
        return 1;
    }
    printf("PASS: FPS = %d\n\n", fps);

    /* Test 4: Framebuffer access */
    printf("Test 4: Framebuffer access...\n");
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(win, &fb)) {
        fprintf(stderr, "FAIL: Get framebuffer failed: %s\n", vgfx_get_last_error());
        return 1;
    }
    if (!fb.pixels || fb.width != 320 || fb.height != 240 || fb.stride != 1280) {
        fprintf(stderr,
                "FAIL: Invalid framebuffer (pixels=%p, w=%d, h=%d, stride=%d)\n",
                (void *)fb.pixels,
                fb.width,
                fb.height,
                fb.stride);
        return 1;
    }
    printf("PASS: Framebuffer OK (stride=%d bytes)\n\n", fb.stride);

    /* Test 5: Clear screen and verify */
    printf("Test 5: Clear screen...\n");
    vgfx_cls(win, VGFX_BLACK);
    uint8_t *corner_pixel = fb.pixels;
    if (corner_pixel[0] != 0x00 || corner_pixel[1] != 0x00 || corner_pixel[2] != 0x00) {
        fprintf(stderr, "FAIL: Clear failed, pixel (0,0) not black\n");
        return 1;
    }
    printf("PASS: Screen cleared to black\n\n");

    /* Test 6: Set and verify pixel */
    printf("Test 6: Set and verify pixel...\n");
    vgfx_pset(win, 10, 10, VGFX_WHITE);
    uint8_t *pixel = fb.pixels + (10 * fb.stride) + (10 * 4);
    if (pixel[0] != 0xFF || pixel[1] != 0xFF || pixel[2] != 0xFF || pixel[3] != 0xFF) {
        fprintf(stderr,
                "FAIL: Pixel at (10,10) expected RGBA(255,255,255,255), got (%d,%d,%d,%d)\n",
                pixel[0],
                pixel[1],
                pixel[2],
                pixel[3]);
        return 1;
    }
    printf("PASS: Pixel at (10,10) = white\n\n");

    /* Test 7: Drawing operations */
    printf("Test 7: Drawing operations...\n");
    vgfx_line(win, 0, 0, 100, 100, VGFX_RED);
    vgfx_rect(win, 50, 50, 100, 80, VGFX_GREEN);
    vgfx_fill_rect(win, 200, 150, 50, 50, VGFX_BLUE);
    vgfx_circle(win, 160, 120, 40, VGFX_YELLOW);
    vgfx_fill_circle(win, 260, 120, 30, VGFX_MAGENTA);
    printf("PASS: All drawing functions executed\n\n");

    /* Test 8: Color utilities */
    printf("Test 8: Color utilities...\n");
    vgfx_color_t orange = vgfx_rgb(255, 128, 0);
    uint8_t r, g, b;
    vgfx_color_to_rgb(orange, &r, &g, &b);
    if (r != 255 || g != 128 || b != 0) {
        fprintf(stderr, "FAIL: Color conversion failed, got RGB(%d,%d,%d)\n", r, g, b);
        return 1;
    }
    printf("PASS: Color conversion RGB(255,128,0)\n\n");

    /* Test 9: Input state (should be all clear) */
    printf("Test 9: Input state...\n");
    if (vgfx_key_down(win, VGFX_KEY_ESCAPE)) {
        fprintf(stderr, "FAIL: ESC key should not be down\n");
        return 1;
    }
    int32_t mx, my;
    vgfx_mouse_pos(win, &mx, &my);
    printf("PASS: Input state OK (mouse at %d,%d)\n\n", mx, my);

    /* Test 10: Event queue (should be empty) */
    printf("Test 10: Event queue...\n");
    vgfx_event_t event;
    if (vgfx_poll_event(win, &event)) {
        fprintf(stderr, "FAIL: Event queue should be empty\n");
        return 1;
    }
    printf("PASS: Event queue empty\n\n");

    /* Test 11: Update (should succeed even with stub backend) */
    printf("Test 11: Update window...\n");
    if (!vgfx_update(win)) {
        fprintf(stderr, "FAIL: Update failed: %s\n", vgfx_get_last_error());
        return 1;
    }
    printf("PASS: Update succeeded\n\n");

    /* Cleanup */
    printf("Destroying window...\n");
    vgfx_destroy_window(win);
    printf("PASS: Window destroyed\n\n");

    printf("=== All Tests Passed ===\n");
    return 0;
}
