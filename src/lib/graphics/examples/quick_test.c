//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/examples/quick_test.c
// Purpose: Minimal ZannaGFX smoke test for CI and manual verification.
// Key invariants: Keeps runtime short; prints clear diagnostics; exits cleanly.
// Ownership/Lifetime: Demonstration code; owns and releases any created
//                     resources.
// Links: docs/vgfx.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Short-lived ZannaGFX window and presentation example.
/// @details Draws a deterministic primitive pattern, presents it, services at
///          most thirty frames of close and keyboard events, and reports each
///          lifecycle stage for manual backend verification.

/*
 * ZannaGFX Quick Test - Creates window, draws, and auto-exits
 * Used for automated testing of the macOS backend
 */

#include <stdio.h>
#include <vgfx.h>

/// @brief Create, render, briefly display, and destroy a fixed graphics window.
/// @details The bounded event loop makes this example suitable for unattended
///          smoke use while still allowing a user to close the window or press
///          Escape early. Every failed create or presentation call is reported.
/// @return 0 after successful cleanup; 1 on window creation or update failure.
int main(void) {
    printf("ZannaGFX macOS Backend Test\n");
    printf("============================\n\n");

    /* Create window */
    printf("1. Creating window...\n");
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 400;
    params.height = 300;
    params.title = "ZannaGFX - Quick Test";
    params.resizable = 0;

    vgfx_window_t win = vgfx_create_window(&params);
    if (!win) {
        fprintf(stderr, "FAIL: %s\n", vgfx_get_last_error());
        return 1;
    }
    printf("   ✓ Window created\n\n");

    /* Draw test pattern */
    printf("2. Drawing test pattern...\n");
    vgfx_cls(win, VGFX_RGB(0, 0, 64));

    /* Red square */
    vgfx_fill_rect(win, 20, 20, 80, 80, VGFX_RED);
    vgfx_rect(win, 18, 18, 84, 84, VGFX_WHITE);

    /* Green circle */
    vgfx_fill_circle(win, 200, 60, 40, VGFX_GREEN);
    vgfx_circle(win, 200, 60, 42, VGFX_WHITE);

    /* Blue filled rectangle */
    vgfx_fill_rect(win, 280, 20, 100, 80, VGFX_BLUE);

    /* Yellow diagonal lines */
    vgfx_line(win, 0, 0, 399, 299, VGFX_YELLOW);
    vgfx_line(win, 399, 0, 0, 299, VGFX_YELLOW);

    /* Magenta circle */
    vgfx_fill_circle(win, 200, 200, 60, VGFX_MAGENTA);

    printf("   ✓ Test pattern drawn\n\n");

    /* Update display */
    printf("3. Presenting to screen...\n");
    if (!vgfx_update(win)) {
        fprintf(stderr, "FAIL: %s\n", vgfx_get_last_error());
        return 1;
    }
    printf("   ✓ Display updated\n\n");

    /* Run for a few frames to ensure window is visible */
    printf("4. Running event loop (30 frames)...\n");
    int frames = 0;
    int running = 1;

    while (running && frames < 30) {
        vgfx_event_t event;
        while (vgfx_poll_event(win, &event)) {
            if (event.type == VGFX_EVENT_CLOSE) {
                printf("   ✓ User closed window\n");
                running = 0;
                break;
            } else if (event.type == VGFX_EVENT_KEY_DOWN) {
                printf("   ✓ Key pressed: %d\n", event.data.key.key);
                if (event.data.key.key == VGFX_KEY_ESCAPE) {
                    running = 0;
                    break;
                }
            }
        }

        if (!vgfx_update(win)) {
            fprintf(stderr, "FAIL: %s\n", vgfx_get_last_error());
            return 1;
        }

        frames++;
    }

    printf("   ✓ Ran %d frames\n\n", frames);

    /* Cleanup */
    printf("5. Cleaning up...\n");
    vgfx_destroy_window(win);
    printf("   ✓ Window destroyed\n\n");

    printf("============================\n");
    printf("SUCCESS: All tests passed!\n");
    printf("Window displayed with graphics for ~0.5 seconds\n");

    return 0;
}
