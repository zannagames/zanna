//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSpriteBatchContractTests.cpp
// Purpose: Isolated SpriteBatch ordering, transform, clipping, and arithmetic
//   correctness contracts with fake runtime objects and drawing backends.
//
// Key invariants:
//   - Equal-depth commands preserve submission order.
//   - Region clipping is identical before fast and transformed drawing paths.
//   - Full-range scales, rotations, and destination coordinates are handled
//     without floating-point precision loss or signed overflow.
//
// Ownership/Lifetime:
//   - Fake runtime object allocations are process-local test fixtures.
//
// Links: src/runtime/graphics/2d/rt_spritebatch.c,
//        src/runtime/graphics/2d/rt_spritebatch.h
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_spritebatch.h"
}

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef RT_PIXELS_CLASS_ID
#define RT_PIXELS_CLASS_ID INT64_C(-0x600201)
#endif

namespace {

struct ObjHeader {
    int64_t class_id;
};

struct StubPixels {
    int64_t width;
    int64_t height;
    int64_t id;
};

struct DrawCall {
    int64_t pixels_id;
    int64_t x;
    int64_t y;
    int64_t sx;
    int64_t sy;
    int64_t w;
    int64_t h;
};

DrawCall g_alpha_calls[16];
DrawCall g_region_calls[16];
int g_alpha_call_count = 0;
int g_region_call_count = 0;
int g_tint_call_count = 0;
int64_t g_last_tint = -2;
int g_scale_call_count = 0;
int64_t g_last_scale_width = 0;
int64_t g_last_scale_height = 0;
int g_rotate_call_count = 0;
double g_last_rotation = -1.0;
int64_t g_rotate_output_width = 0;
int64_t g_rotate_output_height = 0;

void reset_draw_calls() {
    std::memset(g_alpha_calls, 0, sizeof(g_alpha_calls));
    std::memset(g_region_calls, 0, sizeof(g_region_calls));
    g_alpha_call_count = 0;
    g_region_call_count = 0;
    g_tint_call_count = 0;
    g_last_tint = -2;
    g_scale_call_count = 0;
    g_last_scale_width = 0;
    g_last_scale_height = 0;
    g_rotate_call_count = 0;
    g_last_rotation = -1.0;
    g_rotate_output_width = 0;
    g_rotate_output_height = 0;
}

} // namespace

static ObjHeader *header_from_payload(void *obj) {
    return reinterpret_cast<ObjHeader *>(obj) - 1;
}

extern "C" void *rt_obj_new_i64(int64_t class_id, int64_t byte_size) {
    auto *header = static_cast<ObjHeader *>(
        std::calloc(1, sizeof(ObjHeader) + static_cast<size_t>(byte_size)));
    assert(header != nullptr);
    header->class_id = class_id;
    return header + 1;
}

extern "C" int64_t rt_obj_class_id(void *obj) {
    return obj ? header_from_payload(obj)->class_id : 0;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    if (!obj)
        return 0;
    if (class_id == RT_PIXELS_CLASS_ID)
        return 1;
    return rt_obj_class_id(obj) == class_id;
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" int8_t rt_heap_is_payload(void *) {
    return 0;
}

extern "C" int32_t rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *obj) {
    std::free(header_from_payload(obj));
}

extern "C" void rt_obj_retain_maybe(void *) {}

extern "C" void rt_trap(const char *msg) {
    std::fprintf(stderr, "unexpected rt_trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

extern "C" void *rt_pixels_tint(void *pixels, int64_t tint) {
    g_tint_call_count++;
    g_last_tint = tint;
    return pixels;
}

extern "C" void *rt_pixels_clone(void *pixels) {
    return pixels;
}

extern "C" void *rt_pixels_new(int64_t width, int64_t height) {
    auto *pixels = static_cast<StubPixels *>(std::calloc(1, sizeof(StubPixels)));
    assert(pixels != nullptr);
    pixels->width = width;
    pixels->height = height;
    return pixels;
}

extern "C" void rt_pixels_copy(
    void *, int64_t, int64_t, void *, int64_t, int64_t, int64_t, int64_t) {}

extern "C" void *rt_pixels_scale(void *pixels, int64_t width, int64_t height) {
    g_scale_call_count++;
    g_last_scale_width = width;
    g_last_scale_height = height;
    return pixels;
}

extern "C" void *rt_pixels_rotate(void *pixels, double rotation) {
    g_rotate_call_count++;
    g_last_rotation = rotation;
    if (g_rotate_output_width > 0)
        static_cast<StubPixels *>(pixels)->width = g_rotate_output_width;
    if (g_rotate_output_height > 0)
        static_cast<StubPixels *>(pixels)->height = g_rotate_output_height;
    return pixels;
}

extern "C" int64_t rt_pixels_width(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->width : 0;
}

extern "C" int64_t rt_pixels_height(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->height : 0;
}

extern "C" void rt_canvas_blit_alpha(void *canvas, int64_t x, int64_t y, void *pixels) {
    (void)canvas;
    assert(g_alpha_call_count < (int)(sizeof(g_alpha_calls) / sizeof(g_alpha_calls[0])));
    g_alpha_calls[g_alpha_call_count++] = {static_cast<StubPixels *>(pixels)->id, x, y, 0, 0, 0, 0};
}

extern "C" void rt_canvas_blit_region(
    void *canvas, int64_t x, int64_t y, void *pixels, int64_t, int64_t, int64_t, int64_t) {
    (void)canvas;
    assert(g_region_call_count < (int)(sizeof(g_region_calls) / sizeof(g_region_calls[0])));
    g_region_calls[g_region_call_count++] = {
        static_cast<StubPixels *>(pixels)->id, x, y, 0, 0, 0, 0};
}

extern "C" void rt_canvas_blit_region_alpha(void *canvas,
                                            int64_t x,
                                            int64_t y,
                                            void *pixels,
                                            int64_t sx,
                                            int64_t sy,
                                            int64_t width,
                                            int64_t height) {
    // The SpriteBatch region fast path now blends; record identically to the opaque
    // region blit so existing region-draw assertions still observe the call.
    (void)canvas;
    assert(g_region_call_count < (int)(sizeof(g_region_calls) / sizeof(g_region_calls[0])));
    g_region_calls[g_region_call_count++] = {
        static_cast<StubPixels *>(pixels)->id, x, y, sx, sy, width, height};
}

extern "C" void rt_sprite_draw_transformed(
    void *, void *, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) {}

extern "C" int64_t rt_sprite_get_depth(void *) {
    return 0;
}

static void test_equal_depth_pixels_preserve_submission_order() {
    StubPixels first{4, 4, 1};
    StubPixels second{4, 4, 2};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    assert(rt_obj_class_id(batch) == RT_SPRITEBATCH_CLASS_ID);
    rt_spritebatch_set_sort_by_depth(batch, 1);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, &first, 10, 20);
    rt_spritebatch_draw_pixels(batch, &second, 30, 40);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_alpha_call_count == 2);
    assert(g_alpha_calls[0].pixels_id == 1);
    assert(g_alpha_calls[1].pixels_id == 2);
}

static void test_depth_sort_preserves_submission_order_within_equal_depth() {
    StubPixels back{8, 8, 10};
    StubPixels front_a{8, 8, 20};
    StubPixels front_b{8, 8, 30};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_set_sort_by_depth(batch, 1);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(batch, &front_a, 0, 0, 0, 0, 8, 8, 100, 100, 0, 5);
    rt_spritebatch_draw_region_ex(batch, &back, 0, 0, 0, 0, 8, 8, 100, 100, 0, 1);
    rt_spritebatch_draw_region_ex(batch, &front_b, 0, 0, 0, 0, 8, 8, 100, 100, 0, 5);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_region_call_count == 3);
    assert(g_region_calls[0].pixels_id == 10);
    assert(g_region_calls[1].pixels_id == 20);
    assert(g_region_calls[2].pixels_id == 30);
}

static void test_zero_tint_applies_black_and_negative_tint_disables_tint() {
    StubPixels pixels{4, 4, 50};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);

    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, &pixels, 0, 0);
    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));
    assert(g_alpha_call_count == 1);
    assert(g_tint_call_count == 0);

    rt_spritebatch_set_tint(batch, 0);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, &pixels, 0, 0);
    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));
    assert(g_alpha_call_count == 1);
    assert(g_tint_call_count == 1);
    assert(g_last_tint == 0);

    rt_spritebatch_set_tint(batch, -1);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, &pixels, 0, 0);
    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));
    assert(g_alpha_call_count == 1);
    assert(g_tint_call_count == 0);

    int64_t tagged = (INT64_C(1) << 56) | INT64_C(0x80010203);
    rt_spritebatch_set_tint(batch, tagged);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, &pixels, 0, 0);
    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));
    assert(g_alpha_call_count == 1);
    assert(g_tint_call_count == 1);
    assert(g_last_tint == tagged);
}

static void test_rotated_region_keeps_requested_top_left() {
    StubPixels pixels{16, 16, 60};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(batch, &pixels, 100, 200, 4, 4, 8, 8, 100, 100, 90, 0);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_alpha_call_count == 1);
    assert(g_alpha_calls[0].x == 100);
    assert(g_alpha_calls[0].y == 200);
}

static void test_regions_are_source_clipped_before_queueing() {
    StubPixels pixels{8, 6, 70};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(batch, &pixels, 100, 200, -3, -2, 6, 5, 100, 100, 0, 0);
    rt_spritebatch_draw_region_ex(batch, &pixels, 0, 0, 8, 0, 1, 1, 100, 100, 0, 0);
    rt_spritebatch_draw_region_ex(batch, &pixels, 0, 0, 0, 0, 0, 1, 100, 100, 0, 0);
    rt_spritebatch_draw_region_ex(batch, &pixels, 0, 0, INT64_MIN, 0, INT64_MAX, 1, 100, 100, 0, 0);
    assert(rt_spritebatch_count(batch) == 1);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_region_call_count == 1);
    assert(g_region_calls[0].x == 103);
    assert(g_region_calls[0].y == 202);
    assert(g_region_calls[0].sx == 0);
    assert(g_region_calls[0].sy == 0);
    assert(g_region_calls[0].w == 3);
    assert(g_region_calls[0].h == 3);
}

static void test_full_range_scale_uses_exact_integer_arithmetic() {
    StubPixels pixels{4, 1, 80};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(batch, &pixels, 0, 0, 0, 0, 3, 1, INT64_MAX, 100, 0, 0);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_scale_call_count == 1);
    assert(g_last_scale_width == INT64_C(276701161105643274));
    assert(g_last_scale_height == 1);
}

static void test_full_range_rotation_is_canonicalized_before_double_conversion() {
    StubPixels pixels{8, 8, 90};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(batch, &pixels, 0, 0, 0, 0, 8, 8, 100, 100, INT64_MAX, 0);

    reset_draw_calls();
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_rotate_call_count == 1);
    assert(g_last_rotation == 7.0);
}

static void test_rotation_recentering_saturates_extreme_destinations() {
    StubPixels pixels{8, 8, 100};

    void *batch = rt_spritebatch_new(0);
    assert(batch != nullptr);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region_ex(
        batch, &pixels, INT64_MAX, INT64_MIN, 0, 0, 8, 8, 100, 100, 90, 0);

    reset_draw_calls();
    g_rotate_output_width = 2;
    g_rotate_output_height = 16;
    rt_spritebatch_end(batch, reinterpret_cast<void *>(1));

    assert(g_alpha_call_count == 1);
    assert(g_alpha_calls[0].x == INT64_MAX);
    assert(g_alpha_calls[0].y == INT64_MIN);
}

int main() {
    test_equal_depth_pixels_preserve_submission_order();
    test_depth_sort_preserves_submission_order_within_equal_depth();
    test_zero_tint_applies_black_and_negative_tint_disables_tint();
    test_rotated_region_keeps_requested_top_left();
    test_regions_are_source_clipped_before_queueing();
    test_full_range_scale_uses_exact_integer_arithmetic();
    test_full_range_rotation_is_canonicalized_before_double_conversion();
    test_rotation_recentering_saturates_extreme_destinations();
    std::printf("RTSpriteBatchContractTests passed.\n");
    return 0;
}
