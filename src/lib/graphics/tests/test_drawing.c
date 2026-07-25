//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_drawing.c
// Purpose: Unit tests for ZannaGFX drawing primitives and rendering pipeline.
// Key invariants: Tests are deterministic; validate pixel output where
//                 applicable; avoid environment-specific assumptions.
// Ownership/Lifetime: Test binary; creates and destroys windows/textures as
//                     needed.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//

/*
 * ZannaGFX - Drawing Tests (T7-T13)
 * Tests drawing primitives (lines, rectangles, circles)
 */

#include "test_harness.h"
#include "vgfx.h"
#include <limits.h>
#include <math.h>

/* Helper: Count pixels of a given color in window */
static int count_pixels(vgfx_window_t win, int32_t w, int32_t h, vgfx_color_t target) {
    int count = 0;
    vgfx_color_t color;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            if (vgfx_point(win, x, y, &color) && color == target) {
                count++;
            }
        }
    }
    return count;
}

/* T7: Line Drawing – Horizontal */
void test_line_horizontal(void) {
    TEST_BEGIN("T7: Line Drawing - Horizontal");

    vgfx_window_params_t params = {
        .width = 200, .height = 200, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_line(win, 10, 10, 50, 10, 0xFFFFFF);

    /* Check pixels on line are white */
    vgfx_color_t color;
    for (int32_t x = 10; x <= 50; x++) {
        int ok = vgfx_point(win, x, 10, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFFFFFF);
    }

    /* Check pixels outside line are black */
    int ok = vgfx_point(win, 9, 10, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    ok = vgfx_point(win, 51, 10, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T8: Line Drawing – Vertical */
void test_line_vertical(void) {
    TEST_BEGIN("T8: Line Drawing - Vertical");

    vgfx_window_params_t params = {
        .width = 200, .height = 200, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_line(win, 20, 10, 20, 30, 0xFF0000);

    /* Check all pixels on line are red */
    vgfx_color_t color;
    for (int32_t y = 10; y <= 30; y++) {
        int ok = vgfx_point(win, 20, y, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFF0000);
    }

    vgfx_destroy_window(win);
    TEST_END();
}

/* T9: Line Drawing – Diagonal */
void test_line_diagonal(void) {
    TEST_BEGIN("T9: Line Drawing - Diagonal");

    vgfx_window_params_t params = {
        .width = 200, .height = 200, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_line(win, 0, 0, 10, 10, 0x00FF00);

    /* Check endpoints and midpoint */
    vgfx_color_t color;
    int ok;

    ok = vgfx_point(win, 0, 0, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    ok = vgfx_point(win, 5, 5, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    ok = vgfx_point(win, 10, 10, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    /* Count green pixels - should be at least 8 */
    int green_count = count_pixels(win, 200, 200, 0x00FF00);
    ASSERT_TRUE(green_count >= 8);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T10: Rectangle Outline */
void test_rectangle_outline(void) {
    TEST_BEGIN("T10: Rectangle Outline");

    vgfx_window_params_t params = {
        .width = 100, .height = 100, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_rect(win, 10, 10, 20, 15, 0xFFFFFF);

    vgfx_color_t color;
    int ok;

    /* Check top edge: x in [10, 30), y = 10 */
    for (int32_t x = 10; x < 30; x++) {
        ok = vgfx_point(win, x, 10, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFFFFFF);
    }

    /* Check bottom edge: x in [10, 30), y = 24 */
    for (int32_t x = 10; x < 30; x++) {
        ok = vgfx_point(win, x, 24, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFFFFFF);
    }

    /* Check left edge: y in [10, 25), x = 10 */
    for (int32_t y = 10; y < 25; y++) {
        ok = vgfx_point(win, 10, y, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFFFFFF);
    }

    /* Check right edge: y in [10, 25), x = 29 */
    for (int32_t y = 10; y < 25; y++) {
        ok = vgfx_point(win, 29, y, &color);
        ASSERT_EQ(ok, 1);
        ASSERT_EQ(color, 0xFFFFFF);
    }

    /* Check interior is not filled */
    ok = vgfx_point(win, 15, 15, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T11: Filled Rectangle */
void test_filled_rectangle(void) {
    TEST_BEGIN("T11: Filled Rectangle");

    vgfx_window_params_t params = {
        .width = 100, .height = 100, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_fill_rect(win, 5, 5, 10, 10, 0xFF0000);

    vgfx_color_t color;
    int ok;

    /* Check all pixels in [5, 15) × [5, 15) are red */
    for (int32_t y = 5; y < 15; y++) {
        for (int32_t x = 5; x < 15; x++) {
            ok = vgfx_point(win, x, y, &color);
            ASSERT_EQ(ok, 1);
            ASSERT_EQ(color, 0xFF0000);
        }
    }

    /* Check pixels outside are black */
    ok = vgfx_point(win, 4, 5, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    ok = vgfx_point(win, 15, 5, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T12: Circle Outline – Sanity */
void test_circle_outline(void) {
    TEST_BEGIN("T12: Circle Outline - Sanity");

    vgfx_window_params_t params = {
        .width = 200, .height = 200, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_circle(win, 100, 100, 50, 0xFF0000);

    vgfx_color_t color;
    int ok;

    /* Check cardinal points are red */
    ok = vgfx_point(win, 150, 100, &color); /* East */
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0xFF0000);

    ok = vgfx_point(win, 50, 100, &color); /* West */
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0xFF0000);

    ok = vgfx_point(win, 100, 150, &color); /* South */
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0xFF0000);

    ok = vgfx_point(win, 100, 50, &color); /* North */
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0xFF0000);

    /* Check center is black (outline only) */
    ok = vgfx_point(win, 100, 100, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    /* Count red pixels - should be in approximate perimeter range */
    int red_count = count_pixels(win, 200, 200, 0xFF0000);
    ASSERT_TRUE(red_count >= 200 && red_count <= 400);

    vgfx_destroy_window(win);
    TEST_END();
}

/* T13: Filled Circle – Sanity */
void test_filled_circle(void) {
    TEST_BEGIN("T13: Filled Circle - Sanity");

    vgfx_window_params_t params = {
        .width = 200, .height = 200, .title = "Test", .fps = 0, .resizable = 0};

    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_fill_circle(win, 100, 100, 30, 0x00FF00);

    vgfx_color_t color;
    int ok;

    /* Check center is green */
    ok = vgfx_point(win, 100, 100, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    /* Check cardinal points at radius 30 are green */
    ok = vgfx_point(win, 130, 100, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    ok = vgfx_point(win, 70, 100, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    ok = vgfx_point(win, 100, 130, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    ok = vgfx_point(win, 100, 70, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x00FF00);

    /* Count green pixels - should be approximately π × 30² ≈ 2827 (within ±10%) */
    int green_count = count_pixels(win, 200, 200, 0x00FF00);
    int expected = (int)(3.14159 * 30 * 30); /* ~2827 */
    int tolerance = expected / 10;           /* 10% */
    ASSERT_TRUE(green_count >= expected - tolerance && green_count <= expected + tolerance);

    /* Check pixel outside radius is black */
    ok = vgfx_point(win, 131, 100, &color);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(color, 0x000000);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_extreme_line_is_clipped(void) {
    TEST_BEGIN("Audit: Extreme Line Coordinates Are Clipped");

    vgfx_window_params_t params = {
        .width = 32, .height = 32, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_line(win, INT32_MIN, 16, INT32_MAX, 16, 0x123456);

    vgfx_color_t color;
    ASSERT_EQ(vgfx_point(win, 0, 16, &color), 1);
    ASSERT_EQ(color, 0x123456);
    ASSERT_EQ(vgfx_point(win, 31, 16, &color), 1);
    ASSERT_EQ(color, 0x123456);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_extreme_fill_rect_is_clipped(void) {
    TEST_BEGIN("Audit: Extreme Filled Rectangle Is Clipped");

    vgfx_window_params_t params = {
        .width = 16, .height = 16, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_fill_rect(win, -5, -5, INT32_MAX, INT32_MAX, 0xABCDEF);

    vgfx_color_t color;
    ASSERT_EQ(vgfx_point(win, 0, 0, &color), 1);
    ASSERT_EQ(color, 0xABCDEF);
    ASSERT_EQ(vgfx_point(win, 15, 15, &color), 1);
    ASSERT_EQ(color, 0xABCDEF);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_extreme_clip_rect_is_canonicalized(void) {
    TEST_BEGIN("Audit: Extreme Clip Rectangle Is Canonicalized");

    vgfx_window_params_t params = {
        .width = 12, .height = 12, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_set_clip(win, -100, -100, INT32_MAX, INT32_MAX);
    vgfx_fill_rect(win, 0, 0, 12, 12, 0x00AAFF);

    vgfx_color_t color;
    ASSERT_EQ(vgfx_point(win, 11, 11, &color), 1);
    ASSERT_EQ(color, 0x00AAFF);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_empty_clip_rect_suppresses_drawing(void) {
    TEST_BEGIN("Audit: Empty Clip Rectangle Suppresses Drawing");

    vgfx_window_params_t params = {
        .width = 8, .height = 8, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_set_clip(win, 1, 1, 0, 4);
    vgfx_fill_rect(win, 0, 0, 8, 8, 0xFFFFFF);

    vgfx_color_t color;
    ASSERT_EQ(vgfx_point(win, 4, 4, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK);

    vgfx_destroy_window(win);
    TEST_END();
}

/// @brief Verify that widget-style set/clear clip calls cannot escape a compositor limit.
/// @details The test also checks nested-scope intersection, per-scope restoration, and exact
///          restoration of the clip that was active before the outermost scope began.
void test_clip_limit_contains_nested_clips_and_restores_state(void) {
    TEST_BEGIN("Retained compositor clip limit contains nested clips");

    vgfx_window_params_t params = {
        .width = 12, .height = 12, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_set_clip(win, 1, 1, 10, 10);
    ASSERT_EQ(vgfx_push_clip_limit(win, 3, 3, 4, 4), 1);

    vgfx_set_clip(win, 0, 0, 12, 12);
    vgfx_fill_rect(win, 0, 0, 12, 12, 0xCC2200);
    vgfx_clear_clip(win);
    vgfx_fill_rect(win, 0, 0, 12, 12, 0x00AA44);

    vgfx_color_t color = 0;
    ASSERT_EQ(vgfx_point(win, 2, 2, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK);
    ASSERT_EQ(vgfx_point(win, 3, 3, &color), 1);
    ASSERT_EQ(color, 0x00AA44);
    ASSERT_EQ(vgfx_point(win, 7, 7, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK);

    // Nested scope: the inner ceiling intersects the outer one, and drawing
    // after a clear-clip stays inside the innermost ceiling.
    ASSERT_EQ(vgfx_push_clip_limit(win, 4, 4, 6, 6), 1);
    vgfx_clear_clip(win);
    vgfx_fill_rect(win, 0, 0, 12, 12, 0x2244FF);
    ASSERT_EQ(vgfx_point(win, 3, 3, &color), 1);
    ASSERT_EQ(color, 0x00AA44); // outside the nested 4..6 ceiling
    ASSERT_EQ(vgfx_point(win, 5, 5, &color), 1);
    ASSERT_EQ(color, 0x2244FF);
    ASSERT_EQ(vgfx_point(win, 7, 7, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK); // still bounded by the outer 3..6 ceiling

    // Popping the nested scope resumes the outer ceiling.
    vgfx_pop_clip_limit(win);
    vgfx_clear_clip(win);
    vgfx_fill_rect(win, 0, 0, 12, 12, 0xDD8800);
    ASSERT_EQ(vgfx_point(win, 3, 3, &color), 1);
    ASSERT_EQ(color, 0xDD8800);
    ASSERT_EQ(vgfx_point(win, 7, 7, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK);

    vgfx_pop_clip_limit(win);
    int32_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    ASSERT_EQ(vgfx_get_clip(win, &clip_x, &clip_y, &clip_w, &clip_h), 1);
    ASSERT_EQ(clip_x, 1);
    ASSERT_EQ(clip_y, 1);
    ASSERT_EQ(clip_w, 10);
    ASSERT_EQ(clip_h, 10);

    vgfx_destroy_window(win);
    TEST_END();
}

void test_extreme_circle_coordinates_do_not_overflow(void) {
    TEST_BEGIN("Audit: Extreme Circle Coordinates Do Not Overflow");

    vgfx_window_params_t params = {
        .width = 8, .height = 8, .title = "Test", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vgfx_cls(win, VGFX_BLACK);
    vgfx_circle(win, INT32_MAX - 1, INT32_MAX - 1, 4, 0xFFFFFF);
    vgfx_fill_circle(win, INT32_MIN + 1, INT32_MIN + 1, 4, 0xFFFFFF);

    vgfx_color_t color;
    ASSERT_EQ(vgfx_point(win, 0, 0, &color), 1);
    ASSERT_EQ(color, VGFX_BLACK);

    vgfx_destroy_window(win);
    TEST_END();
}

/* Main test runner */
/// What: Entry point for drawing tests covering primitive rendering.
/// Why:  Ensure that core drawing operations work end-to-end.
/// How:  Creates a surface/window, issues draw calls, then validates output.
int main(void) {
    printf("========================================\n");
    printf("ZannaGFX Drawing Tests (T7-T13)\n");
    printf("========================================\n");

    test_line_horizontal();
    test_line_vertical();
    test_line_diagonal();
    test_rectangle_outline();
    test_filled_rectangle();
    test_circle_outline();
    test_filled_circle();
    test_extreme_line_is_clipped();
    test_extreme_fill_rect_is_clipped();
    test_extreme_clip_rect_is_canonicalized();
    test_empty_clip_rect_suppresses_drawing();
    test_clip_limit_contains_nested_clips_and_restores_state();
    test_extreme_circle_coordinates_do_not_overflow();

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
// File: src/lib/graphics/tests/test_drawing.c
// Purpose: Unit tests for ZannaGFX drawing primitives and rendering pipeline.
// Key invariants: Tests are deterministic; validate pixel output where
//                 applicable; avoid environment-specific assumptions.
// Ownership/Lifetime: Test binary; creates and destroys windows/textures as
//                     needed.
// Links: docs/vgfx-testing.md
//
//===----------------------------------------------------------------------===//
