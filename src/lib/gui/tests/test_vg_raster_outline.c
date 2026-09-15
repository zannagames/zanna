//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/tests/test_vg_raster_outline.c
// Purpose: Glyph rasterizer outline tests — contours made only of off-curve points, runs of
//          consecutive off-curve points, and overlapping contours under TrueType's non-zero
//          winding rule.
// Key invariants:
//   - Glyph data is synthesised in-memory; one font unit maps to one pixel (units_per_em 100,
//     size 100), and each glyph's bitmap origin is (x_min - 1, y_max + 1) in font units.
//   - Every probed pixel lies well inside or outside the expected shape, never on an edge.
// Ownership/Lifetime:
//   - Each test frees the glyph and bitmap vg_rasterize_glyph returned.
// Links: lib/gui/src/font/vg_raster.c, lib/gui/src/font/vg_ttf_internal.h
//
//===----------------------------------------------------------------------===//

#include "vg_font.h"
#include "vg_ttf_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_failed = 0;

#define EXPECT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                 \
            tests_failed++;                                                                        \
        }                                                                                          \
    } while (0)

/// @brief Write a big-endian uint16 to p.
static void put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/// @brief Write a big-endian uint32 to p.
static void put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/// @brief One outline point in font units.
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t on_curve;
} outline_point_t;

/// @brief Serialise a simple glyph with 16-bit coordinate deltas; returns its byte length.
/// @param out Destination buffer (at least 12 + 2*contours + 5*points bytes).
/// @param points Points of every contour, in order.
/// @param contour_ends Inclusive last point index of each contour.
/// @param contour_count Number of contours.
static size_t append_simple_glyph(uint8_t *out,
                                  const outline_point_t *points,
                                  const uint16_t *contour_ends,
                                  uint16_t contour_count) {
    uint16_t point_count = (uint16_t)(contour_ends[contour_count - 1] + 1);
    size_t at = 10;
    int16_t prev_x = 0;
    int16_t prev_y = 0;

    put_u16(out, contour_count);
    memset(out + 2, 0, 8); /* bounding box: the rasterizer derives its own */
    for (uint16_t c = 0; c < contour_count; c++, at += 2)
        put_u16(out + at, contour_ends[c]);
    put_u16(out + at, 0); /* no instructions */
    at += 2;
    for (uint16_t i = 0; i < point_count; i++)
        out[at++] = points[i].on_curve ? 0x01 : 0x00; /* 16-bit x and y deltas follow */
    for (uint16_t i = 0; i < point_count; i++, at += 2) {
        put_u16(out + at, (uint16_t)(int16_t)(points[i].x - prev_x));
        prev_x = points[i].x;
    }
    for (uint16_t i = 0; i < point_count; i++, at += 2) {
        put_u16(out + at, (uint16_t)(int16_t)(points[i].y - prev_y));
        prev_y = points[i].y;
    }
    return at;
}

/// @brief Rasterize one glyph built from @p points at 1 px per font unit.
static vg_glyph_t *rasterize_outline(const outline_point_t *points,
                                     const uint16_t *contour_ends,
                                     uint16_t contour_count) {
    static uint8_t data[512];
    vg_font_t font;
    size_t glyph_len;

    memset(data, 0, sizeof(data));
    glyph_len = append_simple_glyph(data + 8, points, contour_ends, contour_count);
    put_u32(data + 0, 0);                   /* loca[0]: glyph 0 starts at glyf offset 0 */
    put_u32(data + 4, (uint32_t)glyph_len); /* loca[1]: end of glyph 0 */

    memset(&font, 0, sizeof(font));
    font.data = data;
    font.data_size = sizeof(data);
    font.loca_offset = 0;
    font.loca_len = 8;
    font.glyf_offset = 8;
    font.glyf_len = (uint32_t)glyph_len;
    font.head.index_to_loc_format = 1;
    font.head.units_per_em = 100;
    font.maxp.num_glyphs = 1;
    return vg_rasterize_glyph(&font, 0, 100.0f);
}

/// @brief Coverage of the pixel containing font-unit point (fx, fy), or -1 when outside.
static int coverage_at(const vg_glyph_t *glyph, int x_min, int y_max, int fx, int fy) {
    int px = fx - x_min + 1;
    int py = y_max + 1 - fy;
    if (!glyph || !glyph->bitmap || px < 0 || py < 0 || px >= glyph->width || py >= glyph->height)
        return -1;
    return glyph->bitmap[py * glyph->width + px];
}

static void free_glyph(vg_glyph_t *glyph) {
    if (glyph) {
        free(glyph->bitmap);
        free(glyph);
    }
}

/// @brief A contour of four off-curve points is a closed quadratic B-spline (an "o" bowl).
/// @details Implied on-curve points sit midway between neighbours; the shape surrounds (50,50).
static void test_contour_of_only_off_curve_points_is_filled(void) {
    const outline_point_t points[] = {{50, 10, 0}, {90, 50, 0}, {50, 90, 0}, {10, 50, 0}};
    const uint16_t ends[] = {3};
    vg_glyph_t *glyph = rasterize_outline(points, ends, 1);

    EXPECT_TRUE(glyph != NULL);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 50, 50) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 50, 75) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 14, 14) == 0);
    free_glyph(glyph);
}

/// @brief Two consecutive off-curve points form two curves, not a curve and a chord.
/// @details on(10,10) off(10,90) off(90,90) on(90,10): the second curve runs from the implied
///   point (50,90) through control (90,90) to (90,10) and passes (80,70); the chord it used to
///   collapse into crosses y = 70 at x = 60, so (72,70) is inside only when both curves exist.
static void test_consecutive_off_curve_points_emit_every_curve(void) {
    const outline_point_t points[] = {{10, 10, 1}, {10, 90, 0}, {90, 90, 0}, {90, 10, 1}};
    const uint16_t ends[] = {3};
    vg_glyph_t *glyph = rasterize_outline(points, ends, 1);

    EXPECT_TRUE(glyph != NULL);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 72, 70) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 28, 70) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 88, 86) == 0);
    free_glyph(glyph);
}

/// @brief Overlapping contours with the same winding stay solid where they overlap.
/// @details Squares (10..60) and (40..90), both clockwise; even-odd would leave (40..60) empty.
static void test_overlapping_contours_use_nonzero_winding(void) {
    const outline_point_t points[] = {{10, 10, 1},
                                      {10, 60, 1},
                                      {60, 60, 1},
                                      {60, 10, 1},
                                      {40, 40, 1},
                                      {40, 90, 1},
                                      {90, 90, 1},
                                      {90, 40, 1}};
    const uint16_t ends[] = {3, 7};
    vg_glyph_t *glyph = rasterize_outline(points, ends, 2);

    EXPECT_TRUE(glyph != NULL);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 50, 50) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 20, 20) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 80, 80) == 255);
    EXPECT_TRUE(coverage_at(glyph, 10, 90, 80, 20) == 0);
    free_glyph(glyph);
}

/// @brief Run all rasterizer outline tests and print a single PASS line on success.
int main(void) {
    test_contour_of_only_off_curve_points_is_filled();
    test_consecutive_off_curve_points_emit_every_curve();
    test_overlapping_contours_use_nonzero_winding();
    if (tests_failed != 0)
        return 1;
    printf("test_vg_raster_outline: PASS\n");
    return 0;
}
