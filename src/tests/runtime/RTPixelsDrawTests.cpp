//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPixelsDrawTests.cpp
// Purpose: Tests for Pixels drawing primitives (SetRGB/GetRGB and Draw* methods).
// Key invariants:
//   - Primitive coverage is clipped, deterministic, and uses pixel centers.
//   - A drawing call advances generation at most once and only on content change.
//   - Extreme geometry and empty destination shapes complete without overflow.
// Ownership/Lifetime:
//   - Test-created Pixels and strings are managed by the runtime object system.
//   - No borrowed raw storage escapes an individual test.
// Links: src/runtime/graphics/2d/rt_pixels_draw.c,
//        src/runtime/graphics/2d/rt_pixels_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_pixels.h"

#include "tests/common/PosixCompat.h"
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

// ============================================================================
// SetRGB / GetRGB
// ============================================================================

static void test_setrgb_getrgb_roundtrip() {
    void *p = rt_pixels_new(10, 10);
    rt_pixels_set_rgb(p, 5, 5, 0x112233);
    assert(rt_pixels_get_rgb(p, 5, 5) == 0x112233);
    printf("test_setrgb_getrgb_roundtrip: PASSED\n");
}

static void test_setrgb_stores_full_alpha() {
    // SetRGB should store 0xRRGGBBFF (alpha = 255)
    void *p = rt_pixels_new(4, 4);
    rt_pixels_set_rgb(p, 2, 2, 0xFF0000); // red
    int64_t raw = rt_pixels_get(p, 2, 2); // reads 0xRRGGBBAA
    assert((raw & 0xFF) == 0xFF);         // alpha must be 0xFF
    printf("test_setrgb_stores_full_alpha: PASSED\n");
}

static void test_setrgb_masks_color_rgba_without_signed_shift_overflow() {
    void *p = rt_pixels_new(2, 2);
    int64_t transparent = rt_color_rgba(0x11, 0x22, 0x33, 0);
    rt_pixels_set_rgb(p, 0, 0, transparent);
    assert(rt_pixels_get(p, 0, 0) == 0x112233FF);
    assert(rt_pixels_get_rgb(p, 0, 0) == 0x112233);
    printf("test_setrgb_masks_color_rgba_without_signed_shift_overflow: PASSED\n");
}

static void test_getrgb_discards_alpha() {
    // GetRGB should return 0x00RRGGBB regardless of stored alpha
    void *p = rt_pixels_new(4, 4);
    rt_pixels_set(p, 1, 1, 0xABCDEF42); // raw RGBA with alpha 0x42
    int64_t rgb = rt_pixels_get_rgb(p, 1, 1);
    assert(rgb == (int64_t)0xABCDEF); // alpha stripped
    printf("test_getrgb_discards_alpha: PASSED\n");
}

static void test_color_rgba_packing_differs_from_pixels_storage() {
    void *p = rt_pixels_new(1, 1);
    int64_t packed_pixels = 0x12345678;                           // 0xRRGGBBAA
    int64_t packed_color = rt_color_rgba(0x12, 0x34, 0x56, 0x78); // 0xAARRGGBB

    rt_pixels_set(p, 0, 0, packed_pixels);
    assert(rt_pixels_get(p, 0, 0) == packed_pixels);
    assert((packed_color & 0xFFFFFFFFLL) == (int64_t)0x78123456);
    assert((packed_color & (INT64_C(1) << 56)) != 0);
    assert(packed_color != packed_pixels);

    printf("test_color_rgba_packing_differs_from_pixels_storage: PASSED\n");
}

// ============================================================================
// DrawLine
// ============================================================================

static void test_drawline_horizontal() {
    void *p = rt_pixels_new(20, 20);
    rt_pixels_draw_line(p, 0, 10, 19, 10, 0xFF0000); // red horizontal line
    for (int64_t x = 0; x < 20; x++)
        assert(rt_pixels_get_rgb(p, x, 10) == 0xFF0000);
    // Row above and below should be empty
    assert(rt_pixels_get_rgb(p, 0, 9) == 0);
    assert(rt_pixels_get_rgb(p, 0, 11) == 0);
    printf("test_drawline_horizontal: PASSED\n");
}

static void test_drawline_vertical() {
    void *p = rt_pixels_new(20, 20);
    rt_pixels_draw_line(p, 5, 0, 5, 19, 0x00FF00); // green vertical line
    for (int64_t y = 0; y < 20; y++)
        assert(rt_pixels_get_rgb(p, 5, y) == 0x00FF00);
    assert(rt_pixels_get_rgb(p, 4, 10) == 0);
    assert(rt_pixels_get_rgb(p, 6, 10) == 0);
    printf("test_drawline_vertical: PASSED\n");
}

static void test_drawline_endpoints_set() {
    void *p = rt_pixels_new(30, 30);
    rt_pixels_draw_line(p, 2, 3, 27, 18, 0x0000FF);
    // Start and end pixels must be set
    assert(rt_pixels_get_rgb(p, 2, 3) == 0x0000FF);
    assert(rt_pixels_get_rgb(p, 27, 18) == 0x0000FF);
    printf("test_drawline_endpoints_set: PASSED\n");
}

// ============================================================================
// DrawBox
// ============================================================================

static void test_drawbox_fills_all_pixels() {
    void *p = rt_pixels_new(20, 20);
    rt_pixels_draw_box(p, 2, 3, 5, 4, 0xAABBCC); // 5x4 box at (2,3)
    for (int64_t y = 3; y < 7; y++)
        for (int64_t x = 2; x < 7; x++)
            assert(rt_pixels_get_rgb(p, x, y) == 0xAABBCC);
    // Outside should be clear
    assert(rt_pixels_get_rgb(p, 1, 3) == 0);
    assert(rt_pixels_get_rgb(p, 7, 3) == 0);
    assert(rt_pixels_get_rgb(p, 2, 2) == 0);
    assert(rt_pixels_get_rgb(p, 2, 7) == 0);
    printf("test_drawbox_fills_all_pixels: PASSED\n");
}

static void test_drawbox_clipped() {
    // Box extending beyond buffer — no crash, only in-bounds pixels set
    void *p = rt_pixels_new(10, 10);
    rt_pixels_draw_box(p, 8, 8, 100, 100, 0x123456);
    assert(rt_pixels_get_rgb(p, 9, 9) == 0x123456); // corner inside
    assert(rt_pixels_get_rgb(p, 7, 7) == 0);        // outside box
    printf("test_drawbox_clipped: PASSED\n");
}

static void test_drawbox_accepts_tagged_alpha_color() {
    void *p = rt_pixels_new(4, 4);
    rt_pixels_draw_box(p, 1, 1, 2, 2, rt_color_rgba(1, 2, 3, 4));
    assert(rt_pixels_get(p, 1, 1) == 0x01020304);
    assert(rt_pixels_get(p, 2, 2) == 0x01020304);
    assert(rt_pixels_get(p, 0, 0) == 0);
    printf("test_drawbox_accepts_tagged_alpha_color: PASSED\n");
}

static void test_drawbox_extreme_endpoint_noop() {
    void *p = rt_pixels_new(4, 4);
    rt_pixels_draw_box(p, INT64_MAX - 1, 0, 8, 1, 0xFFFFFF);
    for (int64_t y = 0; y < 4; y++)
        for (int64_t x = 0; x < 4; x++)
            assert(rt_pixels_get(p, x, y) == 0);
    printf("test_drawbox_extreme_endpoint_noop: PASSED\n");
}

// ============================================================================
// DrawFrame
// ============================================================================

static void test_drawframe_outline_only() {
    void *p = rt_pixels_new(10, 10);
    rt_pixels_draw_frame(p, 1, 1, 7, 7, 0xFF8800);
    // Corners must be set
    assert(rt_pixels_get_rgb(p, 1, 1) == 0xFF8800);
    assert(rt_pixels_get_rgb(p, 7, 1) == 0xFF8800);
    assert(rt_pixels_get_rgb(p, 1, 7) == 0xFF8800);
    assert(rt_pixels_get_rgb(p, 7, 7) == 0xFF8800);
    // Interior must be clear
    assert(rt_pixels_get_rgb(p, 4, 4) == 0);
    assert(rt_pixels_get_rgb(p, 3, 3) == 0);
    printf("test_drawframe_outline_only: PASSED\n");
}

// ============================================================================
// DrawDisc
// ============================================================================

static void test_drawdisc_center_set() {
    void *p = rt_pixels_new(30, 30);
    rt_pixels_draw_disc(p, 15, 15, 8, 0x00FF00);
    assert(rt_pixels_get_rgb(p, 15, 15) == 0x00FF00); // center
    assert(rt_pixels_get_rgb(p, 15, 16) == 0x00FF00); // just inside
    printf("test_drawdisc_center_set: PASSED\n");
}

static void test_drawdisc_outside_clear() {
    void *p = rt_pixels_new(30, 30);
    rt_pixels_draw_disc(p, 15, 15, 5, 0xFF0000);
    // Point clearly outside the disc (radius 5 from center 15,15)
    assert(rt_pixels_get_rgb(p, 15, 21) == 0); // dy=6 > r=5
    assert(rt_pixels_get_rgb(p, 21, 15) == 0);
    printf("test_drawdisc_outside_clear: PASSED\n");
}

static void test_filled_curves_do_not_round_outside_pixel_centers() {
    void *disc = rt_pixels_new(7, 7);
    rt_pixels_draw_disc(disc, 3, 3, 2, 0xAA5500);
    assert(rt_pixels_get_rgb(disc, 4, 4) == 0xAA5500);
    assert(rt_pixels_get_rgb(disc, 5, 4) == 0);
    assert(rt_pixels_get_rgb(disc, 1, 4) == 0);

    void *ellipse = rt_pixels_new(7, 7);
    rt_pixels_draw_ellipse(ellipse, 3, 3, 2, 2, 0x0055AA);
    assert(rt_pixels_get_rgb(ellipse, 4, 4) == 0x0055AA);
    assert(rt_pixels_get_rgb(ellipse, 5, 4) == 0);
    assert(rt_pixels_get_rgb(ellipse, 1, 4) == 0);
    printf("test_filled_curves_do_not_round_outside_pixel_centers: PASSED\n");
}

static void test_drawdisc_huge_radius_clips_to_buffer() {
    void *p = rt_pixels_new(4, 4);
    rt_pixels_draw_disc(p, 0, 0, INT64_MAX, 0x102030);
    for (int64_t y = 0; y < 4; y++)
        for (int64_t x = 0; x < 4; x++)
            assert(rt_pixels_get_rgb(p, x, y) == 0x102030);
    printf("test_drawdisc_huge_radius_clips_to_buffer: PASSED\n");
}

static void test_shape_generation_only_changes_on_write() {
    void *p = rt_pixels_new(4, 4);
    uint64_t gen = rt_pixels_generation(p);
    rt_pixels_draw_disc(p, 100, 100, 5, 0xFFFFFF);
    rt_pixels_draw_ring(p, 100, 100, 5, 0xFFFFFF);
    rt_pixels_draw_ellipse(p, 100, 100, 5, 3, 0xFFFFFF);
    rt_pixels_draw_ellipse_frame(p, 100, 100, 5, 3, 0xFFFFFF);
    rt_pixels_draw_triangle(p, 100, 100, 110, 100, 100, 110, 0xFFFFFF);
    rt_pixels_draw_bezier(p, 100, 100, 105, 110, 110, 100, 0xFFFFFF);
    assert(rt_pixels_generation(p) == gen);
    rt_pixels_draw_disc(p, 1, 1, 1, 0xFFFFFF);
    assert(rt_pixels_generation(p) == gen + 1);
    printf("test_shape_generation_only_changes_on_write: PASSED\n");
}

static void test_primitives_touch_once_and_idempotent_redraws_do_not_touch() {
    void *box = rt_pixels_new(12, 12);
    rt_pixels_draw_box(box, 1, 1, 8, 8, 0x112233);
    assert(rt_pixels_generation(box) == 1);
    rt_pixels_draw_box(box, 1, 1, 8, 8, 0x112233);
    assert(rt_pixels_generation(box) == 1);

    void *frame = rt_pixels_new(12, 12);
    rt_pixels_draw_frame(frame, 1, 1, 8, 8, 0x445566);
    assert(rt_pixels_generation(frame) == 1);
    rt_pixels_draw_frame(frame, 1, 1, 8, 8, 0x445566);
    assert(rt_pixels_generation(frame) == 1);

    void *thick = rt_pixels_new(20, 20);
    rt_pixels_draw_thick_line(thick, 1, 1, 18, 18, 5, 0x778899);
    assert(rt_pixels_generation(thick) == 1);
    rt_pixels_draw_thick_line(thick, 1, 1, 18, 18, 5, 0x778899);
    assert(rt_pixels_generation(thick) == 1);

    void *blend = rt_pixels_new(1, 1);
    rt_pixels_set_rgba(blend, 0, 0, 0xAABBCCFF);
    uint64_t generation = rt_pixels_generation(blend);
    rt_pixels_blend_pixel(blend, 0, 0, 0xAABBCC, 255);
    rt_pixels_blend_pixel(blend, 0, 0, 0xAABBCC, 128);
    assert(rt_pixels_generation(blend) == generation);

    void *text_pixels = rt_pixels_new(8, 8);
    rt_string text = rt_str_from_lit("A", 1);
    rt_pixels_draw_text(text_pixels, 0, 0, text, 0xFFFFFF);
    assert(rt_pixels_generation(text_pixels) == 1);
    rt_pixels_draw_text(text_pixels, 0, 0, text, 0xFFFFFF);
    assert(rt_pixels_generation(text_pixels) == 1);
    printf("test_primitives_touch_once_and_idempotent_redraws_do_not_touch: PASSED\n");
}

static void test_empty_asymmetric_surface_rejects_all_raster_work() {
    void *p = rt_pixels_new(0, INT64_MAX);
    assert(p != nullptr);
    rt_pixels_draw_box(p, 0, 0, INT64_MAX, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_frame(p, 0, 0, INT64_MAX, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_disc(p, 0, 0, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_ring(p, 0, 0, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_ellipse(p, 0, 0, INT64_MAX, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_ellipse_frame(p, 0, 0, INT64_MAX, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_triangle(p, 0, 0, INT64_MAX, 0, 0, INT64_MAX, 0xFFFFFF);
    rt_pixels_draw_bezier(p, 0, 0, INT64_MAX, INT64_MAX, 0, 0, 0xFFFFFF);
    rt_pixels_draw_thick_line(p, INT64_MIN, INT64_MIN, INT64_MAX, INT64_MAX, INT64_MAX, 0xFFFFFF);
    assert(rt_pixels_generation(p) == 0);
    printf("test_empty_asymmetric_surface_rejects_all_raster_work: PASSED\n");
}

// ============================================================================
// DrawRing
// ============================================================================

static void test_drawring_outline_set_interior_clear() {
    void *p = rt_pixels_new(40, 40);
    rt_pixels_draw_ring(p, 20, 20, 8, 0x8800FF);
    // A point on the ring (approximately at (20, 20-8) = (20, 12))
    assert(rt_pixels_get_rgb(p, 20, 12) == 0x8800FF);
    // Center should be clear
    assert(rt_pixels_get_rgb(p, 20, 20) == 0);
    // Interior (halfway) should be clear
    assert(rt_pixels_get_rgb(p, 20, 15) == 0);
    printf("test_drawring_outline_set_interior_clear: PASSED\n");
}

// ============================================================================
// DrawEllipse
// ============================================================================

static void test_drawellipse_interior_set() {
    void *p = rt_pixels_new(40, 40);
    rt_pixels_draw_ellipse(p, 20, 20, 10, 5, 0x00AAFF);
    assert(rt_pixels_get_rgb(p, 20, 20) == 0x00AAFF); // center
    assert(rt_pixels_get_rgb(p, 20, 23) == 0x00AAFF); // inside ry=5
    // Outside
    assert(rt_pixels_get_rgb(p, 20, 26) == 0); // dy=6 > ry=5
    printf("test_drawellipse_interior_set: PASSED\n");
}

static void test_drawellipse_nonpositive_radii_noop() {
    void *p = rt_pixels_new(5, 5);
    uint64_t gen = rt_pixels_generation(p);
    rt_pixels_draw_ellipse(p, 2, 2, 0, 3, 0xFFFFFF);
    rt_pixels_draw_ellipse_frame(p, 2, 2, 3, -1, 0xFFFFFF);
    assert(rt_pixels_get(p, 2, 2) == 0);
    assert(rt_pixels_generation(p) == gen);
    printf("test_drawellipse_nonpositive_radii_noop: PASSED\n");
}

// ============================================================================
// DrawEllipseFrame
// ============================================================================

static void test_drawellipseframe_outline_set() {
    void *p = rt_pixels_new(40, 40);
    rt_pixels_draw_ellipse_frame(p, 20, 20, 10, 5, 0xFF5500);
    // Top of ellipse (20, 20-5) = (20, 15)
    assert(rt_pixels_get_rgb(p, 20, 15) == 0xFF5500);
    // Interior should be clear
    assert(rt_pixels_get_rgb(p, 20, 20) == 0);
    printf("test_drawellipseframe_outline_set: PASSED\n");
}

// ============================================================================
// FloodFill
// ============================================================================

static void test_floodfill_fills_region() {
    void *p = rt_pixels_new(10, 10);
    // Clear canvas is all black (0x000000FF internally, RGB = 0x000000)
    // Fill with red starting from center
    rt_pixels_flood_fill(p, 5, 5, 0xFF0000);
    // Entire canvas should now be red (all pixels were the same color)
    for (int64_t y = 0; y < 10; y++)
        for (int64_t x = 0; x < 10; x++)
            assert(rt_pixels_get_rgb(p, x, y) == 0xFF0000);
    printf("test_floodfill_fills_region: PASSED\n");
}

static void test_floodfill_respects_boundary() {
    void *p = rt_pixels_new(20, 20);
    // Draw a white box border at (5,5) 10x10
    rt_pixels_draw_frame(p, 5, 5, 10, 10, 0xFFFFFF);
    // Fill inside the box with blue
    rt_pixels_flood_fill(p, 10, 10, 0x0000FF);
    // Interior pixel should be blue
    assert(rt_pixels_get_rgb(p, 10, 10) == 0x0000FF);
    // Border pixel should still be white
    assert(rt_pixels_get_rgb(p, 5, 5) == 0xFFFFFF);
    // Outside the box should still be black
    assert(rt_pixels_get_rgb(p, 1, 1) == 0);
    printf("test_floodfill_respects_boundary: PASSED\n");
}

static void test_floodfill_noop_when_same_color() {
    void *p = rt_pixels_new(5, 5);
    rt_pixels_draw_box(p, 0, 0, 5, 5, 0xFF0000);
    // Fill with same color — should do nothing, no crash
    rt_pixels_flood_fill(p, 2, 2, 0xFF0000);
    assert(rt_pixels_get_rgb(p, 2, 2) == 0xFF0000);
    printf("test_floodfill_noop_when_same_color: PASSED\n");
}

static void test_floodfill_compares_and_writes_alpha() {
    void *p = rt_pixels_new(3, 1);
    rt_pixels_set(p, 0, 0, 0x11223344);
    rt_pixels_set(p, 1, 0, 0x11223355);
    rt_pixels_set(p, 2, 0, 0x11223344);

    rt_pixels_flood_fill(p, 0, 0, rt_color_rgba(9, 8, 7, 6));

    assert(rt_pixels_get(p, 0, 0) == 0x09080706);
    assert(rt_pixels_get(p, 1, 0) == 0x11223355);
    assert(rt_pixels_get(p, 2, 0) == 0x11223344);
    printf("test_floodfill_compares_and_writes_alpha: PASSED\n");
}

// ============================================================================
// DrawThickLine
// ============================================================================

static void test_drawthickline_width() {
    void *p = rt_pixels_new(40, 40);
    // Horizontal thick line with thickness 7 at y=20
    rt_pixels_draw_thick_line(p, 0, 20, 39, 20, 7, 0x804020);
    // Center row
    assert(rt_pixels_get_rgb(p, 20, 20) == 0x804020);
    // Rows within radius (3) of center should be set
    assert(rt_pixels_get_rgb(p, 20, 17) == 0x804020); // 3 rows above center
    // Row beyond radius should be clear
    assert(rt_pixels_get_rgb(p, 20, 24) == 0); // 4 rows below center
    printf("test_drawthickline_width: PASSED\n");
}

// ============================================================================
// DrawTriangle
// ============================================================================

static void test_drawtriangle_interior() {
    void *p = rt_pixels_new(30, 30);
    // Right triangle with vertices at (5,5), (25,5), (5,25)
    rt_pixels_draw_triangle(p, 5, 5, 25, 5, 5, 25, 0x00CC00);
    // Point on the top edge
    assert(rt_pixels_get_rgb(p, 15, 5) == 0x00CC00);
    // Point inside
    assert(rt_pixels_get_rgb(p, 8, 8) == 0x00CC00);
    // Point outside (bottom-right, past hypotenuse)
    assert(rt_pixels_get_rgb(p, 25, 25) == 0);
    printf("test_drawtriangle_interior: PASSED\n");
}

static void test_drawtriangle_degenerate_draws_longest_edge() {
    void *p = rt_pixels_new(8, 3);
    rt_pixels_draw_triangle(p, 1, 1, 6, 1, 3, 1, rt_color_rgba(0xAA, 0xBB, 0xCC, 0xDD));
    for (int64_t x = 1; x <= 6; x++)
        assert(rt_pixels_get(p, x, 1) == 0xAABBCCDD);
    assert(rt_pixels_get(p, 0, 1) == 0);
    assert(rt_pixels_get(p, 7, 1) == 0);
    assert(rt_pixels_get(p, 3, 0) == 0);
    assert(rt_pixels_get(p, 3, 2) == 0);
    printf("test_drawtriangle_degenerate_draws_longest_edge: PASSED\n");
}

static void test_drawtriangle_extreme_collinear_edge_is_exact() {
    void *p = rt_pixels_new(8, 1);
    rt_pixels_draw_triangle(p, INT64_MIN, 0, 0, 0, INT64_MAX, 0, rt_color_rgba(1, 2, 3, 4));
    for (int64_t x = 0; x < 8; x++)
        assert(rt_pixels_get(p, x, 0) == 0x01020304);
    assert(rt_pixels_generation(p) == 1);
    printf("test_drawtriangle_extreme_collinear_edge_is_exact: PASSED\n");
}

static void test_drawtriangle_full_height_uses_unsaturated_edge_ratio() {
    void *p = rt_pixels_new(5, 2);
    rt_pixels_draw_triangle(p, 0, INT64_MIN, 0, 0, 4, INT64_MAX, rt_color_rgba(9, 8, 7, 255));

    assert(rt_pixels_get(p, 2, 0) == static_cast<int64_t>(0x090807FF));
    assert(rt_pixels_get(p, 3, 0) == 0);
    printf("test_drawtriangle_full_height_uses_unsaturated_edge_ratio: PASSED\n");
}

// ============================================================================
// DrawBezier
// ============================================================================

static void test_drawbezier_endpoints() {
    void *p = rt_pixels_new(40, 40);
    // Bezier from (2,2) to (37,2) with control at (20,37)
    rt_pixels_draw_bezier(p, 2, 2, 20, 37, 37, 2, 0xCC0000);
    assert(rt_pixels_get_rgb(p, 2, 2) == 0xCC0000);  // start
    assert(rt_pixels_get_rgb(p, 37, 2) == 0xCC0000); // end
    printf("test_drawbezier_endpoints: PASSED\n");
}

static void test_drawbezier_connects_sparse_samples() {
    void *p = rt_pixels_new(12050, 1);
    assert(p != nullptr);
    rt_pixels_draw_bezier(p, 0, 0, 6025, 0, 12049, 0, 0x00CCFF);
    assert(rt_pixels_get_rgb(p, 0, 0) == 0x00CCFF);
    assert(rt_pixels_get_rgb(p, 12049, 0) == 0x00CCFF);
    assert(rt_pixels_get_rgb(p, 6025, 0) == 0x00CCFF);
    for (int64_t x = 0; x < 12050; x += 137)
        assert(rt_pixels_get_rgb(p, x, 0) == 0x00CCFF);
    printf("test_drawbezier_connects_sparse_samples: PASSED\n");
}

static void test_drawbezier_full_range_control_points_cross_exact_center() {
    void *p = rt_pixels_new(3, 8);
    rt_pixels_draw_bezier(p, INT64_MIN, 0, 0, 10, INT64_MAX, 0, rt_color_rgba(1, 2, 3, 255));
    assert(rt_pixels_get(p, 0, 5) == static_cast<int64_t>(0x010203FF));
    printf("test_drawbezier_full_range_control_points_cross_exact_center: PASSED\n");
}

static void test_drawbezier_does_not_round_intermediate_lerps() {
    void *p = rt_pixels_new(2, 3);
    rt_pixels_draw_bezier(p, 0, 0, 0, 0, 1, 2, 0x123456);

    // At t=1/2 the exact curve is (0.25, 0.5), which rounds to (0, 1).
    // Rounding each de Casteljau interpolation first incorrectly produces (1, 1).
    assert(rt_pixels_get_rgb(p, 0, 1) == 0x123456);
    printf("test_drawbezier_does_not_round_intermediate_lerps: PASSED\n");
}

// ============================================================================
// DrawText
// ============================================================================

static int64_t count_region_pixels(
    void *pixels, int64_t x0, int64_t y0, int64_t w, int64_t h, int64_t rgba) {
    int64_t count = 0;
    for (int64_t y = y0; y < y0 + h; y++)
        for (int64_t x = x0; x < x0 + w; x++)
            if (rt_pixels_get(pixels, x, y) == rgba)
                count++;
    return count;
}

static void test_drawtext_renders_font_metrics_and_alpha_colors() {
    void *p = rt_pixels_new(16, 8);
    rt_string text = rt_str_from_lit("A", 1);
    uint64_t gen = rt_pixels_generation(p);

    rt_pixels_draw_text(p, 0, 0, text, rt_color_rgba(0x12, 0x34, 0x56, 0x78));

    assert(rt_pixels_generation(p) == gen + 1);
    assert(rt_pixels_text_width(text) == 8);
    assert(rt_pixels_text_height() == 8);
    assert(count_region_pixels(p, 0, 0, 8, 8, 0x12345678) > 0);
    assert(rt_pixels_get(p, 8, 0) == 0);

    rt_string utf8 = rt_str_from_lit("h\xc3\xa9", 3);
    assert(rt_pixels_text_width(utf8) == 16);
    assert(rt_pixels_text_scaled_width(utf8, 3) == 48);
    assert(rt_pixels_text_scaled_width(utf8, 0) == 0);
    printf("test_drawtext_renders_font_metrics_and_alpha_colors: PASSED\n");
}

static void test_drawtext_background_scaled_and_alignment() {
    void *bg = rt_pixels_new(8, 8);
    rt_pixels_draw_text_bg(bg, 0, 0, rt_str_from_lit("A", 1), 0xFFFFFF, 0x102030);
    int64_t fg_count = count_region_pixels(bg, 0, 0, 8, 8, 0xFFFFFFFF);
    int64_t bg_count = count_region_pixels(bg, 0, 0, 8, 8, 0x102030FF);
    assert(fg_count > 0);
    assert(bg_count > 0);
    assert(fg_count + bg_count == 64);

    void *scaled = rt_pixels_new(16, 16);
    rt_pixels_draw_text_scaled_bg(
        scaled, 0, 0, rt_str_from_lit("!", 1), 2, rt_color_rgba(1, 2, 3, 4), 0x050607);
    assert(count_region_pixels(scaled, 0, 0, 16, 16, 0x01020304) > 0);
    assert(count_region_pixels(scaled, 0, 0, 16, 16, 0x050607FF) > 0);

    void *centered = rt_pixels_new(24, 8);
    rt_pixels_draw_text_centered(centered, 0, rt_str_from_lit("A", 1), 0xABCDEF);
    assert(count_region_pixels(centered, 0, 0, 8, 8, 0xABCDEFFF) == 0);
    assert(count_region_pixels(centered, 8, 0, 8, 8, 0xABCDEFFF) > 0);
    assert(count_region_pixels(centered, 16, 0, 8, 8, 0xABCDEFFF) == 0);

    void *right = rt_pixels_new(24, 8);
    rt_pixels_draw_text_right(right, 4, 0, rt_str_from_lit("A", 1), 0x445566);
    assert(count_region_pixels(right, 0, 0, 12, 8, 0x445566FF) == 0);
    assert(count_region_pixels(right, 12, 0, 8, 8, 0x445566FF) > 0);
    assert(count_region_pixels(right, 20, 0, 4, 8, 0x445566FF) == 0);

    void *centered_scaled = rt_pixels_new(40, 16);
    rt_pixels_draw_text_centered_scaled(centered_scaled, 0, rt_str_from_lit("A", 1), 0x778899, 2);
    assert(count_region_pixels(centered_scaled, 0, 0, 12, 16, 0x778899FF) == 0);
    assert(count_region_pixels(centered_scaled, 12, 0, 16, 16, 0x778899FF) > 0);
    assert(count_region_pixels(centered_scaled, 28, 0, 12, 16, 0x778899FF) == 0);

    printf("test_drawtext_background_scaled_and_alignment: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

// === Golden pixel-hash regression ===
// The 2D pixels surface is the deterministic software rasterizer. Hashing the
// whole framebuffer after a fixed scene turns it into a golden-image oracle:
// any change to a rasterization primitive shifts the hash. This complements the
// per-primitive spot checks above with a single whole-buffer fingerprint.

/// @brief FNV-1a hash over every pixel of a surface (row-major RGBA).
static uint64_t fnv1a_pixels(void *p) {
    const int64_t w = rt_pixels_width(p);
    const int64_t h = rt_pixels_height(p);
    uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (int64_t y = 0; y < h; ++y) {
        for (int64_t x = 0; x < w; ++x) {
            const uint64_t px = static_cast<uint64_t>(rt_pixels_get(p, x, y));
            for (int b = 0; b < 4; ++b) {
                hash ^= (px >> (b * 8)) & 0xFFu;
                hash *= 1099511628211ULL; // FNV prime
            }
        }
    }
    return hash;
}

/// @brief Render a fixed composition into @p p so the same scene hashes identically.
static void draw_golden_scene(void *p) {
    rt_pixels_fill(p, 0x202030FF);                       // dark slate background
    rt_pixels_draw_box(p, 8, 8, 40, 24, 0x3060A0FF);     // filled panel
    rt_pixels_draw_frame(p, 6, 6, 44, 28, 0xFFFFFFFF);   // panel border
    rt_pixels_draw_disc(p, 24, 40, 12, 0xC04040FF);      // red disc
    rt_pixels_draw_ring(p, 48, 44, 10, 0x40C060FF);      // green ring
    rt_pixels_draw_line(p, 2, 2, 61, 61, 0xF0F000FF);    // yellow diagonal
    rt_pixels_draw_ellipse(p, 40, 18, 8, 5, 0x8040C0FF); // purple ellipse
}

static void test_pixelhash_scene_is_deterministic() {
    void *a = rt_pixels_new(64, 64);
    void *b = rt_pixels_new(64, 64);
    draw_golden_scene(a);
    draw_golden_scene(b);
    // Same scene → identical fingerprint on every run (deterministic SW raster).
    assert(fnv1a_pixels(a) == fnv1a_pixels(b));
    printf("test_pixelhash_scene_is_deterministic: PASSED\n");
}

static void test_pixelhash_golden_value() {
    void *p = rt_pixels_new(64, 64);
    draw_golden_scene(p);
    const uint64_t hash = fnv1a_pixels(p);
    // Golden fingerprint of the fixed scene. If a rasterization primitive changes
    // output, this fails — regenerate after confirming the new pixels are correct.
    // Updated when filled circles/ellipses stopped rounding roots outward:
    // only pixel centers mathematically inside each curve are now covered.
    const uint64_t kGolden = 0x0040F5FA6926F3B3ULL;
    if (hash != kGolden)
        printf("test_pixelhash_golden_value: hash=0x%016llX (golden=0x%016llX)\n",
               static_cast<unsigned long long>(hash),
               static_cast<unsigned long long>(kGolden));
    assert(hash == kGolden);
    // Spot-check anchors so a hash mismatch is debuggable against known pixels.
    assert(rt_pixels_get(p, 0, 0) == static_cast<int64_t>(0x202030FF));   // untouched bg corner
    assert(rt_pixels_get(p, 24, 40) == static_cast<int64_t>(0xC04040FF)); // disc center
    printf("test_pixelhash_golden_value: PASSED\n");
}

int main() {
    // SetRGB / GetRGB
    test_setrgb_getrgb_roundtrip();
    test_setrgb_stores_full_alpha();
    test_setrgb_masks_color_rgba_without_signed_shift_overflow();
    test_getrgb_discards_alpha();
    test_color_rgba_packing_differs_from_pixels_storage();

    // DrawLine
    test_drawline_horizontal();
    test_drawline_vertical();
    test_drawline_endpoints_set();

    // DrawBox
    test_drawbox_fills_all_pixels();
    test_drawbox_clipped();
    test_drawbox_accepts_tagged_alpha_color();
    test_drawbox_extreme_endpoint_noop();

    // DrawFrame
    test_drawframe_outline_only();

    // DrawDisc
    test_drawdisc_center_set();
    test_drawdisc_outside_clear();
    test_filled_curves_do_not_round_outside_pixel_centers();
    test_drawdisc_huge_radius_clips_to_buffer();
    test_shape_generation_only_changes_on_write();
    test_primitives_touch_once_and_idempotent_redraws_do_not_touch();
    test_empty_asymmetric_surface_rejects_all_raster_work();

    // DrawRing
    test_drawring_outline_set_interior_clear();

    // DrawEllipse
    test_drawellipse_interior_set();
    test_drawellipse_nonpositive_radii_noop();

    // DrawEllipseFrame
    test_drawellipseframe_outline_set();

    // FloodFill
    test_floodfill_fills_region();
    test_floodfill_respects_boundary();
    test_floodfill_noop_when_same_color();
    test_floodfill_compares_and_writes_alpha();

    // DrawThickLine
    test_drawthickline_width();

    // DrawTriangle
    test_drawtriangle_interior();
    test_drawtriangle_degenerate_draws_longest_edge();
    test_drawtriangle_extreme_collinear_edge_is_exact();
    test_drawtriangle_full_height_uses_unsaturated_edge_ratio();

    // DrawBezier
    test_drawbezier_endpoints();
    test_drawbezier_connects_sparse_samples();
    test_drawbezier_full_range_control_points_cross_exact_center();
    test_drawbezier_does_not_round_intermediate_lerps();

    // Golden pixel-hash regression (whole-framebuffer fingerprint)
    test_pixelhash_scene_is_deterministic();
    test_pixelhash_golden_value();

    // DrawText
    test_drawtext_renders_font_metrics_and_alpha_colors();
    test_drawtext_background_scaled_and_alignment();

    printf("\nAll tests passed!\n");
    return 0;
}
