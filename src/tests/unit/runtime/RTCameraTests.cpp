//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/RTCameraTests.cpp
/// @file
/// @brief Exercises Camera2D transforms, bounds, follow, culling, and parallax.
// Purpose: Validate camera arithmetic and rendering contracts, including
//   deterministic behavior at int64 boundaries and bounded tile traversal.
//
// Key invariants:
//   - Camera math saturates without signed overflow and coordinate transforms
//     remain stable for equivalent large rotations.
//   - Dirty state changes only when the effective transform changes.
//   - Parallax traversal is budgeted and terminates at numeric boundaries.
//
// Ownership/Lifetime:
//   - Runtime camera/pixel handles live for the test process; cameras retain
//     parallax Pixels handles according to the production ownership contract.
//
// Links: rt_camera.c, rt_camera.h, rt_pixels.c
//
//===----------------------------------------------------------------------===//

#include "rt_camera.h"
#include "rt_pixels.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  %s...", #name);                                                                  \
        test_##name();                                                                             \
        printf(" OK\n");                                                                           \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf(" FAILED at line %d: %s\n", __LINE__, #cond);                                   \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

TEST(create) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(cam != NULL);
    ASSERT(rt_camera_get_x(cam) == 0);
    ASSERT(rt_camera_get_y(cam) == 0);
    ASSERT(rt_camera_get_zoom(cam) == 100);
    ASSERT(rt_camera_get_width(cam) == 800);
    ASSERT(rt_camera_get_height(cam) == 600);
    ASSERT(rt_camera_is_dirty(cam) == 1); // newly created camera is always dirty
}

TEST(is_visible_inside) {
    void *cam = rt_camera_new(800, 600);
    // Camera at (0,0), zoom 100 → viewport covers world [0,0,800,600].
    // An object fully inside the viewport must be visible.
    ASSERT(rt_camera_is_visible(cam, 100, 100, 200, 200) == 1);
    // Object touching the right/bottom edge — still overlapping.
    ASSERT(rt_camera_is_visible(cam, 600, 400, 200, 200) == 1);
    // Object at origin.
    ASSERT(rt_camera_is_visible(cam, 0, 0, 1, 1) == 1);
}

TEST(is_visible_outside) {
    void *cam = rt_camera_new(800, 600);
    // Viewport covers world [0,0,800,600].
    // Objects entirely off each edge must be invisible.
    ASSERT(rt_camera_is_visible(cam, 800, 0, 50, 50) == 0);  // off right
    ASSERT(rt_camera_is_visible(cam, 0, 600, 50, 50) == 0);  // off bottom
    ASSERT(rt_camera_is_visible(cam, -100, 0, 50, 50) == 0); // off left
    ASSERT(rt_camera_is_visible(cam, 0, -100, 50, 50) == 0); // off top
}

TEST(is_visible_partial_overlap) {
    void *cam = rt_camera_new(800, 600);
    // Object partially hanging off the right edge — should still be visible.
    ASSERT(rt_camera_is_visible(cam, 780, 100, 100, 100) == 1); // right edge: 780+100=880 > 800
    // Object partially hanging off the bottom.
    ASSERT(rt_camera_is_visible(cam, 100, 580, 100, 100) == 1); // bottom: 580+100=680 > 600
    // Just one pixel inside the right edge.
    ASSERT(rt_camera_is_visible(cam, 799, 0, 10, 10) == 1);
}

TEST(is_visible_rejects_empty_rectangles) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(rt_camera_is_visible(cam, 100, 100, 0, 10) == 0);
    ASSERT(rt_camera_is_visible(cam, 100, 100, 10, 0) == 0);
    ASSERT(rt_camera_is_visible(cam, 100, 100, -10, 10) == 0);
}

TEST(is_visible_null_camera) {
    // NULL camera must conservatively return 1 (visible).
    ASSERT(rt_camera_is_visible(NULL, 0, 0, 9999, 9999) == 1);
    ASSERT(rt_camera_is_visible(NULL, -1000, -1000, 1, 1) == 1);
}

TEST(is_visible_zoom_in) {
    void *cam = rt_camera_new(800, 600);
    // Zoom in to 200%: world-space viewport = [0, 0, 400, 300].
    rt_camera_set_zoom(cam, 200);
    // Object at (450, 100) is inside 800×600 viewport but outside 400×300 — invisible.
    ASSERT(rt_camera_is_visible(cam, 450, 100, 50, 50) == 0);
    // Object at (100, 100) is inside 400×300 — visible.
    ASSERT(rt_camera_is_visible(cam, 100, 100, 50, 50) == 1);
}

TEST(is_visible_zoom_out) {
    void *cam = rt_camera_new(800, 600);
    // Zoom out to 50%: world-space viewport = [0, 0, 1600, 1200].
    rt_camera_set_zoom(cam, 50);
    // Objects up to world coord 1600×1200 are now visible.
    ASSERT(rt_camera_is_visible(cam, 1500, 1100, 50, 50) == 1);
    // But beyond that range is still invisible.
    ASSERT(rt_camera_is_visible(cam, 1601, 0, 50, 50) == 0);
}

TEST(is_visible_with_camera_offset) {
    void *cam = rt_camera_new(800, 600);
    // Move camera to world pos (1000, 500) → viewport covers [1000,500,1800,1100].
    rt_camera_set_x(cam, 1000);
    rt_camera_set_y(cam, 500);
    // Object at (1100, 600) — inside the offset viewport.
    ASSERT(rt_camera_is_visible(cam, 1100, 600, 100, 100) == 1);
    // Object at (0, 0) — far behind the camera, invisible.
    ASSERT(rt_camera_is_visible(cam, 0, 0, 100, 100) == 0);
    // Object just before the viewport left edge.
    ASSERT(rt_camera_is_visible(cam, 900, 500, 99, 100) == 0); // x+w=999 <= cam_x=1000
}

TEST(world_screen_roundtrip_with_rotation_and_zoom) {
    void *cam = rt_camera_new(800, 600);
    rt_camera_set_x(cam, 100);
    rt_camera_set_y(cam, 50);
    rt_camera_set_zoom(cam, 200);
    rt_camera_set_rotation(cam, 90);

    int64_t sx = -1, sy = -1;
    int64_t wx = -1, wy = -1;

    rt_camera_world_to_screen(cam, 300, 200, &sx, &sy);
    ASSERT(sx == 400);
    ASSERT(sy == 300);

    rt_camera_world_to_screen(cam, 350, 260, &sx, &sy);
    rt_camera_screen_to_world(cam, sx, sy, &wx, &wy);
    ASSERT(wx == 350);
    ASSERT(wy == 260);
}

TEST(follow_and_bounds_respect_zoom) {
    void *cam = rt_camera_new(800, 600);
    rt_camera_set_zoom(cam, 200);
    rt_camera_set_bounds(cam, 0, 0, 1000, 1000);

    rt_camera_follow(cam, 900, 900);
    ASSERT(rt_camera_get_x(cam) == 600);
    ASSERT(rt_camera_get_y(cam) == 700);

    rt_camera_smooth_follow(cam, 0, 0, 1000);
    ASSERT(rt_camera_get_x(cam) == 0);
    ASSERT(rt_camera_get_y(cam) == 0);
}

TEST(dirty_flag) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(rt_camera_is_dirty(cam) == 1); // starts dirty
    rt_camera_clear_dirty(cam);
    ASSERT(rt_camera_is_dirty(cam) == 0);
    rt_camera_set_x(cam, 100);
    ASSERT(rt_camera_is_dirty(cam) == 1);
    rt_camera_clear_dirty(cam);
    rt_camera_set_zoom(cam, 200);
    ASSERT(rt_camera_is_dirty(cam) == 1);
    rt_camera_clear_dirty(cam);
    rt_camera_set_rotation(cam, 45);
    ASSERT(rt_camera_is_dirty(cam) == 1);
}

TEST(noop_mutations_keep_dirty_clear) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(cam != NULL);
    rt_camera_clear_dirty(cam);

    rt_camera_set_x(cam, 0);
    rt_camera_set_y(cam, 0);
    rt_camera_set_zoom(cam, 100);
    rt_camera_set_rotation(cam, 0);
    rt_camera_move(cam, 0, 0);
    rt_camera_follow(cam, 400, 300);
    ASSERT(rt_camera_is_dirty(cam) == 0);

    rt_camera_set_bounds(cam, 0, 0, 800, 600);
    rt_camera_clear_dirty(cam);
    rt_camera_set_x(cam, 123);
    rt_camera_set_y(cam, 456);
    ASSERT(rt_camera_get_x(cam) == 0);
    ASSERT(rt_camera_get_y(cam) == 0);
    ASSERT(rt_camera_is_dirty(cam) == 0);
}

TEST(center_accessors_and_set_center) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(cam != NULL);
    ASSERT(rt_camera_get_center_x(cam) == 400);
    ASSERT(rt_camera_get_center_y(cam) == 300);

    rt_camera_clear_dirty(cam);
    rt_camera_set_center(cam, 1000, 900);
    ASSERT(rt_camera_get_x(cam) == 600);
    ASSERT(rt_camera_get_y(cam) == 600);
    ASSERT(rt_camera_get_center_x(cam) == 1000);
    ASSERT(rt_camera_get_center_y(cam) == 900);
    ASSERT(rt_camera_is_dirty(cam) == 1);
}

TEST(smooth_follow_noop_keeps_dirty_clear) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(cam != NULL);
    rt_camera_clear_dirty(cam);
    rt_camera_smooth_follow(cam, 400, 300, 0);
    ASSERT(rt_camera_get_x(cam) == 0);
    ASSERT(rt_camera_get_y(cam) == 0);
    ASSERT(rt_camera_is_dirty(cam) == 0);
}

TEST(smooth_follow_small_fraction_makes_one_pixel_progress) {
    void *cam = rt_camera_new(100, 100);
    ASSERT(cam != NULL);
    rt_camera_clear_dirty(cam);

    rt_camera_smooth_follow(cam, 549, 50, 1);
    ASSERT(rt_camera_get_x(cam) == 1);
    ASSERT(rt_camera_get_y(cam) == 0);
    ASSERT(rt_camera_is_dirty(cam) == 1);
}

TEST(one_pixel_deadzone_only_contains_center) {
    void *cam = rt_camera_new(100, 100);
    ASSERT(cam != NULL);
    rt_camera_set_deadzone(cam, 1, 0);
    rt_camera_clear_dirty(cam);

    rt_camera_smooth_follow(cam, 51, 50, 1000);
    ASSERT(rt_camera_get_x(cam) == 1);
    ASSERT(rt_camera_is_dirty(cam) == 1);
}

TEST(extreme_zoom_math_is_exact_and_saturating) {
    void *cam = rt_camera_new(INT64_MAX, INT64_MAX);
    ASSERT(cam != NULL);
    rt_camera_set_zoom(cam, 1000);
    ASSERT(rt_camera_get_center_x(cam) == INT64_C(461168601842738790));
    ASSERT(rt_camera_get_center_y(cam) == INT64_C(461168601842738790));

    rt_camera_set_center(cam, INT64_MAX, INT64_MIN);
    ASSERT(rt_camera_get_x(cam) == INT64_C(8762203435012037017));
    ASSERT(rt_camera_get_y(cam) == INT64_MIN);
}

TEST(huge_rotation_matches_reduced_rotation) {
    void *large = rt_camera_new(640, 480);
    void *reduced = rt_camera_new(640, 480);
    ASSERT(large != NULL);
    ASSERT(reduced != NULL);

    rt_camera_set_rotation(large, INT64_MAX);
    rt_camera_set_rotation(reduced, INT64_MAX % 360);
    int64_t large_x = 0;
    int64_t large_y = 0;
    int64_t reduced_x = 0;
    int64_t reduced_y = 0;
    rt_camera_world_to_screen(large, 123, -456, &large_x, &large_y);
    rt_camera_world_to_screen(reduced, 123, -456, &reduced_x, &reduced_y);
    ASSERT(large_x == reduced_x);
    ASSERT(large_y == reduced_y);
}

TEST(set_bounds_clamp_marks_dirty) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(cam != NULL);

    rt_camera_set_x(cam, 900);
    rt_camera_set_y(cam, 700);
    rt_camera_clear_dirty(cam);
    ASSERT(rt_camera_is_dirty(cam) == 0);

    rt_camera_set_bounds(cam, 0, 0, 1000, 1000);
    ASSERT(rt_camera_get_x(cam) == 200);
    ASSERT(rt_camera_get_y(cam) == 400);
    ASSERT(rt_camera_is_dirty(cam) == 1);
}

TEST(parallax_add_remove) {
    void *cam = rt_camera_new(800, 600);
    ASSERT(rt_camera_parallax_count(cam) == 0);

    void *fake_pixels_a = rt_pixels_new(8, 8);
    void *fake_pixels_b = rt_pixels_new(8, 8);
    ASSERT(fake_pixels_a != NULL);
    ASSERT(fake_pixels_b != NULL);

    int64_t idx0 = rt_camera_add_parallax(cam, fake_pixels_a, 50, 50);
    ASSERT(idx0 == 0);
    ASSERT(rt_camera_parallax_count(cam) == 1);

    int64_t idx1 = rt_camera_add_parallax(cam, fake_pixels_b, 100, 100);
    ASSERT(idx1 == 1);
    ASSERT(rt_camera_parallax_count(cam) == 2);

    // Remove first layer, count drops
    rt_camera_remove_parallax(cam, 0);
    ASSERT(rt_camera_parallax_count(cam) == 1);

    // Clear all
    rt_camera_clear_parallax(cam);
    ASSERT(rt_camera_parallax_count(cam) == 0);
}

TEST(parallax_max_layers) {
    void *cam = rt_camera_new(800, 600);
    void *pixels[8] = {0};

    // Fill all 8 slots
    for (int i = 0; i < 8; i++) {
        pixels[i] = rt_pixels_new(4, 4);
        ASSERT(pixels[i] != NULL);
        int64_t idx = rt_camera_add_parallax(cam, pixels[i], 50, 50);
        ASSERT(idx == i);
    }
    ASSERT(rt_camera_parallax_count(cam) == 8);

    // 9th layer should fail
    void *overflow_pixels = rt_pixels_new(4, 4);
    ASSERT(overflow_pixels != NULL);
    int64_t overflow = rt_camera_add_parallax(cam, overflow_pixels, 50, 50);
    ASSERT(overflow == -1);
    ASSERT(rt_camera_parallax_count(cam) == 8);
}

TEST(parallax_null_safety) {
    // NULL camera should not crash, returns safe defaults
    ASSERT(rt_camera_parallax_count(NULL) == 0);
    void *pixels = rt_pixels_new(2, 2);
    ASSERT(pixels != NULL);
    ASSERT(rt_camera_add_parallax(NULL, pixels, 50, 50) == -1);
    /// @brief Rt_camera_remove_parallax.
    rt_camera_remove_parallax(NULL, 0); // no crash
                                        /// @brief Rt_camera_clear_parallax.
    rt_camera_clear_parallax(NULL);     // no crash

    // NULL pixels should be rejected
    void *cam = rt_camera_new(800, 600);
    ASSERT(rt_camera_add_parallax(cam, NULL, 50, 50) == -1);
    ASSERT(rt_camera_add_parallax(cam, cam, 50, 50) == -1);
    ASSERT(rt_camera_parallax_count(cam) == 0);

    // draw_parallax with NULL canvas returns 0
    ASSERT(rt_camera_draw_parallax(cam, NULL) == 0);
    ASSERT(rt_camera_draw_parallax(NULL, (void *)0x1) == 0);
}

struct FakeCanvas {
    void *vptr;
    int64_t magic;
    void *gfx_win;
};

static FakeCanvas *make_fake_canvas() {
    FakeCanvas *c = (FakeCanvas *)std::calloc(1, sizeof(FakeCanvas));
    assert(c != NULL);
    c->vptr = NULL;
    c->gfx_win = NULL;
    return c;
}

TEST(parallax_draw_transformed_path) {
    void *cam = rt_camera_new(320, 240);
    ASSERT(cam != NULL);

    void *pixels = rt_pixels_new(16, 12);
    ASSERT(pixels != NULL);
    ASSERT(rt_camera_add_parallax(cam, pixels, 40, 60) == 0);

    rt_camera_set_x(cam, 75);
    rt_camera_set_y(cam, 30);
    rt_camera_set_zoom(cam, 150);
    rt_camera_set_rotation(cam, 25);

    FakeCanvas *canvas = make_fake_canvas();
    ASSERT(rt_camera_draw_parallax(cam, canvas) == 1);

    std::free(canvas);
}

TEST(parallax_skips_pathological_tile_counts) {
    void *cam = rt_camera_new(INT64_MAX / 4, INT64_MAX / 4);
    ASSERT(cam != NULL);

    void *pixels = rt_pixels_new(1, 1);
    ASSERT(pixels != NULL);
    ASSERT(rt_camera_add_parallax(cam, pixels, 100, 100) == 0);

    FakeCanvas *canvas = make_fake_canvas();
    ASSERT(rt_camera_draw_parallax(cam, canvas) == 0);

    std::free(canvas);
}

/// @brief Main.
int main() {
    printf("RTCameraTests:\n");
    RUN_TEST(create);
    RUN_TEST(is_visible_inside);
    RUN_TEST(is_visible_outside);
    RUN_TEST(is_visible_partial_overlap);
    RUN_TEST(is_visible_rejects_empty_rectangles);
    RUN_TEST(is_visible_null_camera);
    RUN_TEST(is_visible_zoom_in);
    RUN_TEST(is_visible_zoom_out);
    RUN_TEST(is_visible_with_camera_offset);
    RUN_TEST(world_screen_roundtrip_with_rotation_and_zoom);
    RUN_TEST(follow_and_bounds_respect_zoom);
    RUN_TEST(dirty_flag);
    RUN_TEST(noop_mutations_keep_dirty_clear);
    RUN_TEST(center_accessors_and_set_center);
    RUN_TEST(smooth_follow_noop_keeps_dirty_clear);
    RUN_TEST(smooth_follow_small_fraction_makes_one_pixel_progress);
    RUN_TEST(one_pixel_deadzone_only_contains_center);
    RUN_TEST(extreme_zoom_math_is_exact_and_saturating);
    RUN_TEST(huge_rotation_matches_reduced_rotation);
    RUN_TEST(set_bounds_clamp_marks_dirty);
    RUN_TEST(parallax_add_remove);
    RUN_TEST(parallax_max_layers);
    RUN_TEST(parallax_null_safety);
    RUN_TEST(parallax_draw_transformed_path);
    RUN_TEST(parallax_skips_pathological_tile_counts);

    printf("\n%d tests passed, %d tests failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
