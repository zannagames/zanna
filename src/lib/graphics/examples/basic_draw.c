//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/examples/basic_draw.c
// Purpose: ZannaGFX example showing window creation, primitive drawing, and
//          basic event handling.
// Key invariants: Keeps example code minimal and side-effect free beyond the
//                 graphics system; exits cleanly on close/ESC.
// Ownership/Lifetime: Demonstration program; resources owned and released by
//                     the example.
// Links: docs/vgfx.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Interactive ZannaGFX primitive-drawing example.
/// @details Demonstrates parameterized window creation, a fixed set of raster
///          primitives, frame pacing, close/key/resize event handling, display
///          presentation, and explicit window teardown.

/*
 * ZannaGFX Example: Basic Drawing
 * Demonstrates window creation, drawing primitives, and event handling
 */

#include <stdio.h>
#include <vgfx.h>

/// @brief Run the basic drawing example until close, Escape, or update failure.
/// @details Creates one resizable 640-by-480 window, draws a static collection
///          of rectangles, circles, and diagonals into its framebuffer, then
///          presents frames while draining the event queue. The owned window is
///          destroyed before the successful/interactive path returns.
/// @return 1 when window creation fails; otherwise 0 after the loop terminates.
int main(void) {
    printf("ZannaGFX v%d.%d.%d - Basic Drawing Example\n",
           VGFX_VERSION_MAJOR,
           VGFX_VERSION_MINOR,
           VGFX_VERSION_PATCH);

    /* Create window with default parameters */
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 640;
    params.height = 480;
    params.title = "ZannaGFX - Basic Drawing";
    params.resizable = 1;

    vgfx_window_t win = vgfx_create_window(&params);
    if (!win) {
        fprintf(stderr, "Failed to create window: %s\n", vgfx_get_last_error());
        return 1;
    }

    printf("Window created: %dx%d\n", params.width, params.height);

    /* Set FPS limit */
    vgfx_set_fps(win, 60);

    /* Clear screen to dark blue */
    vgfx_cls(win, VGFX_RGB(0, 0, 64));

    /* Draw some shapes */
    vgfx_fill_rect(win, 50, 50, 100, 100, VGFX_RED);
    vgfx_rect(win, 45, 45, 110, 110, VGFX_WHITE);

    vgfx_fill_circle(win, 400, 240, 80, VGFX_GREEN);
    vgfx_circle(win, 400, 240, 85, VGFX_WHITE);

    vgfx_line(win, 0, 0, 639, 479, VGFX_YELLOW);
    vgfx_line(win, 639, 0, 0, 479, VGFX_YELLOW);

    /* Event loop */
    int running = 1;
    while (running) {
        vgfx_event_t event;
        while (vgfx_poll_event(win, &event)) {
            if (event.type == VGFX_EVENT_CLOSE) {
                printf("Close event received\n");
                running = 0;
            } else if (event.type == VGFX_EVENT_KEY_DOWN) {
                if (event.data.key.key == VGFX_KEY_ESCAPE) {
                    printf("ESC pressed, exiting\n");
                    running = 0;
                }
            } else if (event.type == VGFX_EVENT_RESIZE) {
                printf(
                    "Window resized to %dx%d\n", event.data.resize.width, event.data.resize.height);
            }
        }

        /* Update display */
        if (!vgfx_update(win)) {
            fprintf(stderr, "Update failed: %s\n", vgfx_get_last_error());
            break;
        }
    }

    /* Cleanup */
    vgfx_destroy_window(win);
    printf("Window destroyed\n");

    return 0;
}
