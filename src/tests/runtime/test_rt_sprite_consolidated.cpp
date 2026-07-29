//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/test_rt_sprite_consolidated.cpp
// Purpose: Consolidated SpriteBatch, TextureAtlas, Sprite, SpriteSheet, and
//   SpriteAnimator runtime behavior tests.
//
// Key invariants:
//   - Runtime objects are released by each fixture after their assertions.
//   - Animator restart uses the implementation's explicit uninitialized-clock
//     sentinel so a real timestamp of zero remains valid.
//
// Ownership/Lifetime:
//   - Tests own every runtime object and release it before returning.
//
// Links: src/runtime/graphics/2d/rt_sprite.c,
//        src/runtime/graphics/2d/rt_spritebatch.c,
//        src/runtime/graphics/2d/rt_spritesheet.c
//
//===----------------------------------------------------------------------===//
// Consolidated Sprite runtime tests (2 files merged).

#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_seq.h"
#include "rt_sprite.h"
#include "rt_spritebatch.h"
#include "rt_spritesheet.h"
#include "rt_string.h"
#include "rt_texatlas.h"
#include "tests/TestHarness.hpp"
#include "tests/common/PosixCompat.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── RTSpriteBatchTests.cpp ──
extern "C" void rt_abort(const char *msg);

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static void release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

// ============================================================================
// SpriteBatch Creation Tests
// ============================================================================

TEST(RTSprite, SpritebatchNewDefault) {
    void *batch = rt_spritebatch_new(0);
    ASSERT_TRUE(batch != nullptr);

    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);
    ASSERT_TRUE(rt_spritebatch_capacity(batch) > 0);
    ASSERT_TRUE(rt_spritebatch_is_active(batch) == 0);

    printf("test_spritebatch_new_default: PASSED\n");
}

TEST(RTSprite, SpritebatchNewCapacity) {
    void *batch = rt_spritebatch_new(512);
    ASSERT_TRUE(batch != nullptr);
    ASSERT_TRUE(rt_spritebatch_capacity(batch) >= 512);

    printf("test_spritebatch_new_capacity: PASSED\n");
}

// ============================================================================
// SpriteBatch Begin/End Tests
// ============================================================================

TEST(RTSprite, SpritebatchBegin) {
    void *batch = rt_spritebatch_new(0);

    ASSERT_TRUE(rt_spritebatch_is_active(batch) == 0);

    rt_spritebatch_begin(batch);
    ASSERT_TRUE(rt_spritebatch_is_active(batch) == 1);

    printf("test_spritebatch_begin: PASSED\n");
}

TEST(RTSprite, SpritebatchBeginClearsCount) {
    void *batch = rt_spritebatch_new(0);
    void *pixels = rt_pixels_new(1, 1);
    ASSERT_TRUE(pixels != nullptr);

    // First batch
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, pixels, 0, 0);
    rt_spritebatch_draw_pixels(batch, pixels, 10, 10);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 2);

    // Second begin should clear
    rt_spritebatch_begin(batch);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    printf("test_spritebatch_begin_clears_count: PASSED\n");
}

TEST(RTSprite, SpritebatchEndNullCanvasDeactivates) {
    void *batch = rt_spritebatch_new(0);
    void *pixels = rt_pixels_new(4, 4);

    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_pixels(batch, pixels, 0, 0);
    ASSERT_TRUE(rt_spritebatch_is_active(batch) == 1);

    rt_spritebatch_end(batch, nullptr);
    ASSERT_TRUE(rt_spritebatch_is_active(batch) == 0);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    rt_spritebatch_begin(batch);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    printf("test_spritebatch_end_null_canvas_deactivates: PASSED\n");
}

// ============================================================================
// SpriteBatch Draw Tests
// ============================================================================

TEST(RTSprite, SpritebatchDrawIncrementsCount) {
    void *batch = rt_spritebatch_new(0);
    void *pixels = rt_pixels_new(1, 1);
    ASSERT_TRUE(pixels != nullptr);

    rt_spritebatch_begin(batch);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    rt_spritebatch_draw_pixels(batch, pixels, 0, 0);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 1);

    rt_spritebatch_draw_pixels(batch, pixels, 10, 10);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 2);

    rt_spritebatch_draw_pixels(batch, pixels, 20, 20);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 3);

    printf("test_spritebatch_draw_increments_count: PASSED\n");
}

TEST(RTSprite, SpritebatchDrawNotActive) {
    void *batch = rt_spritebatch_new(0);

    // Without begin, draw should not add
    rt_spritebatch_draw_pixels(batch, (void *)1, 0, 0);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    printf("test_spritebatch_draw_not_active: PASSED\n");
}

TEST(RTSprite, SpritebatchDrawNull) {
    void *batch = rt_spritebatch_new(0);

    rt_spritebatch_begin(batch);

    // Drawing null should not add
    rt_spritebatch_draw_pixels(batch, nullptr, 0, 0);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    printf("test_spritebatch_draw_null: PASSED\n");
}

// ============================================================================
// SpriteBatch Settings Tests
// ============================================================================

TEST(RTSprite, SpritebatchSettings) {
    void *batch = rt_spritebatch_new(0);

    // Test sort by depth
    rt_spritebatch_set_sort_by_depth(batch, 1);
    // No getter, but should not crash

    // Test tint
    rt_spritebatch_set_tint(batch, 0xFF0000FF); // Red

    // Test alpha
    rt_spritebatch_set_alpha(batch, 128);

    // Test reset
    rt_spritebatch_reset_settings(batch);

    printf("test_spritebatch_settings: PASSED\n");
}

TEST(RTSprite, SpritebatchAlphaClamp) {
    void *batch = rt_spritebatch_new(0);

    // Test alpha clamping (no direct getter, but should not crash)
    rt_spritebatch_set_alpha(batch, -100);
    rt_spritebatch_set_alpha(batch, 500);
    rt_spritebatch_set_alpha(batch, 0);
    rt_spritebatch_set_alpha(batch, 255);

    printf("test_spritebatch_alpha_clamp: PASSED\n");
}

// ============================================================================
// SpriteBatch Capacity Tests
// ============================================================================

TEST(RTSprite, SpritebatchGrow) {
    void *batch = rt_spritebatch_new(4);
    void *pixels = rt_pixels_new(1, 1);
    ASSERT_TRUE(pixels != nullptr);

    rt_spritebatch_begin(batch);

    // Add more than initial capacity
    for (int i = 0; i < 20; i++) {
        rt_spritebatch_draw_pixels(batch, pixels, i * 10, i * 10);
    }

    ASSERT_TRUE(rt_spritebatch_count(batch) == 20);
    ASSERT_TRUE(rt_spritebatch_capacity(batch) >= 20);

    printf("test_spritebatch_grow: PASSED\n");
}

// ============================================================================
// SpriteBatch Region Draw Tests
// ============================================================================

TEST(RTSprite, SpritebatchDrawRegion) {
    void *batch = rt_spritebatch_new(0);
    void *pixels = rt_pixels_new(64, 64);
    ASSERT_TRUE(pixels != nullptr);

    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_region(batch, pixels, 0, 0, 10, 10, 32, 32);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 1);

    printf("test_spritebatch_draw_region: PASSED\n");
}

TEST(RTSprite, TextureAtlasAddAndLookup) {
    void *pixels = rt_pixels_new(32, 32);
    ASSERT_TRUE(pixels != nullptr);

    void *atlas = rt_texatlas_new(pixels);
    ASSERT_TRUE(atlas != nullptr);

    rt_string name = rt_string_from_bytes("hero_idle_0", 11);
    ASSERT_TRUE(name != nullptr);

    rt_texatlas_add(atlas, name, 4, 8, 12, 16);
    ASSERT_TRUE(rt_texatlas_region_count(atlas) == 1);
    ASSERT_TRUE(rt_texatlas_has(atlas, name) == 1);
    ASSERT_TRUE(rt_texatlas_get_x(atlas, name) == 4);
    ASSERT_TRUE(rt_texatlas_get_y(atlas, name) == 8);
    ASSERT_TRUE(rt_texatlas_get_w(atlas, name) == 12);
    ASSERT_TRUE(rt_texatlas_get_h(atlas, name) == 16);

    rt_string_unref(name);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, TextureAtlasLoadGrid) {
    void *pixels = rt_pixels_new(32, 16);
    ASSERT_TRUE(pixels != nullptr);

    void *atlas = rt_texatlas_load_grid(pixels, 16, 16);
    ASSERT_TRUE(atlas != nullptr);
    ASSERT_TRUE(rt_texatlas_region_count(atlas) == 2);

    rt_string zero = rt_string_from_bytes("0", 1);
    rt_string one = rt_string_from_bytes("1", 1);
    ASSERT_TRUE(rt_texatlas_has(atlas, zero) == 1);
    ASSERT_TRUE(rt_texatlas_has(atlas, one) == 1);
    ASSERT_TRUE(rt_texatlas_get_x(atlas, one) == 16);
    ASSERT_TRUE(rt_texatlas_get_y(atlas, one) == 0);
    ASSERT_TRUE(rt_texatlas_get_w(atlas, one) == 16);
    ASSERT_TRUE(rt_texatlas_get_h(atlas, one) == 16);

    rt_string_unref(zero);
    rt_string_unref(one);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, TextureAtlasEmptyLookupIsAbsent) {
    void *pixels = rt_pixels_new(8, 8);
    ASSERT_TRUE(pixels != nullptr);
    void *atlas = rt_texatlas_new(pixels);
    ASSERT_TRUE(atlas != nullptr);
    rt_string empty = rt_string_from_bytes("", 0);
    ASSERT_TRUE(empty != nullptr);

    ASSERT_TRUE(rt_texatlas_has(atlas, empty) == 0);
    ASSERT_TRUE(rt_texatlas_region_count(atlas) == 0);

    rt_string_unref(empty);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, TextureAtlasLookupRejectsEmbeddedNullAliases) {
    void *pixels = rt_pixels_new(16, 16);
    ASSERT_TRUE(pixels != nullptr);
    void *atlas = rt_texatlas_new(pixels);
    ASSERT_TRUE(atlas != nullptr);

    rt_string prefix = rt_const_cstr("hero");
    const char alias_bytes[] = {'h', 'e', 'r', 'o', '\0', 'a', 'l', 't'};
    rt_string alias = rt_string_from_bytes(alias_bytes, sizeof(alias_bytes));
    rt_texatlas_add(atlas, prefix, 4, 5, 6, 7);

    ASSERT_TRUE(rt_texatlas_has(atlas, prefix) == 1);
    ASSERT_TRUE(rt_texatlas_has(atlas, alias) == 0);
    ASSERT_TRUE(rt_texatlas_get_x(atlas, alias) == 0);
    ASSERT_TRUE(rt_texatlas_get_y(atlas, alias) == 0);
    ASSERT_TRUE(rt_texatlas_get_w(atlas, alias) == 0);
    ASSERT_TRUE(rt_texatlas_get_h(atlas, alias) == 0);

    void *batch = rt_spritebatch_new(0);
    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_atlas(batch, atlas, alias, 0, 0);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 0);

    rt_string_unref(alias);
    rt_string_unref(prefix);
    release_object(batch);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, TextureAtlasAcceptsExactMaximumNameLength) {
    void *pixels = rt_pixels_new(8, 8);
    ASSERT_TRUE(pixels != nullptr);
    void *atlas = rt_texatlas_new(pixels);
    ASSERT_TRUE(atlas != nullptr);

    const char bytes[] = "1234567890123456789012345678901";
    static_assert(sizeof(bytes) - 1 == 31);
    rt_string name = rt_string_from_bytes(bytes, sizeof(bytes) - 1);
    rt_texatlas_add(atlas, name, 0, 0, 8, 8);
    ASSERT_TRUE(rt_texatlas_region_count(atlas) == 1);
    ASSERT_TRUE(rt_texatlas_has(atlas, name) == 1);

    rt_string_unref(name);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, SpritebatchDrawAtlasVariantsIncrementCount) {
    void *pixels = rt_pixels_new(32, 32);
    ASSERT_TRUE(pixels != nullptr);

    void *atlas = rt_texatlas_new(pixels);
    ASSERT_TRUE(atlas != nullptr);

    rt_string name = rt_string_from_bytes("coin", 4);
    ASSERT_TRUE(name != nullptr);
    rt_texatlas_add(atlas, name, 0, 0, 16, 16);

    void *batch = rt_spritebatch_new(0);
    ASSERT_TRUE(batch != nullptr);

    rt_spritebatch_begin(batch);
    rt_spritebatch_draw_atlas(batch, atlas, name, 0, 0);
    rt_spritebatch_draw_atlas_scaled(batch, atlas, name, 32, 32, 150);
    rt_spritebatch_draw_atlas_ex(batch, atlas, name, 64, 64, 200, 45, 7);
    ASSERT_TRUE(rt_spritebatch_count(batch) == 3);

    rt_string_unref(name);
    release_object(batch);
    release_object(atlas);
    release_object(pixels);
}

TEST(RTSprite, SpriteContainsUsesScaledOrigin) {
    void *pixels = rt_pixels_new(10, 10);
    ASSERT_TRUE(pixels != nullptr);

    void *sprite = rt_sprite_new(pixels);
    ASSERT_TRUE(sprite != nullptr);

    rt_sprite_set_x(sprite, 100);
    rt_sprite_set_y(sprite, 100);
    rt_sprite_set_origin(sprite, 5, 5);
    rt_sprite_set_scale_x(sprite, 200);
    rt_sprite_set_scale_y(sprite, 200);

    ASSERT_TRUE(rt_sprite_contains(sprite, 92, 92) == 1);
    ASSERT_TRUE(rt_sprite_contains(sprite, 89, 89) == 0);
}

TEST(RTSprite, SpriteOverlapsUsesScaledOrigin) {
    void *big_pixels = rt_pixels_new(10, 10);
    void *small_pixels = rt_pixels_new(4, 4);
    ASSERT_TRUE(big_pixels != nullptr);
    ASSERT_TRUE(small_pixels != nullptr);

    void *scaled = rt_sprite_new(big_pixels);
    void *probe = rt_sprite_new(small_pixels);
    ASSERT_TRUE(scaled != nullptr);
    ASSERT_TRUE(probe != nullptr);

    rt_sprite_set_x(scaled, 100);
    rt_sprite_set_y(scaled, 100);
    rt_sprite_set_origin(scaled, 5, 5);
    rt_sprite_set_scale_x(scaled, 200);
    rt_sprite_set_scale_y(scaled, 200);

    rt_sprite_set_x(probe, 88);
    rt_sprite_set_y(probe, 88);

    ASSERT_TRUE(rt_sprite_overlaps(scaled, probe) == 1);
}

TEST(RTSprite, SpriteFramesGrowPastLegacyCapAndTrackDelays) {
    void *pixels = rt_pixels_new(1, 1);
    ASSERT_TRUE(pixels != nullptr);

    void *sprite = rt_sprite_new(pixels);
    ASSERT_TRUE(sprite != nullptr);
    rt_sprite_set_frame_delay(sprite, 50);

    for (int i = 1; i < 80; i++)
        rt_sprite_add_frame(sprite, pixels);

    ASSERT_TRUE(rt_sprite_get_frame_count(sprite) == 80);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 0) == 50);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 79) == 50);

    rt_sprite_set_frame_delay_at(sprite, 70, 17);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 70) == 17);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 69) == 50);

    rt_sprite_set_frame_delay_at(sprite, 70, 0);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 70) == 1);

    rt_sprite_set_frame_delay(sprite, 33);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 0) == 33);
    ASSERT_TRUE(rt_sprite_get_frame_delay_at(sprite, 70) == 33);

    release_object(sprite);
    release_object(pixels);
}

// ============================================================================
// Main
// ============================================================================


// ── RTSpriteSheetTests.cpp ──
// (vm_trap, rt_object.h, rt_pixels.h, rt_seq.h, rt_spritesheet.h, rt_string.h
//  already included above)

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

// Helper: create a test atlas with known pixel values
static void *make_test_atlas(int64_t w, int64_t h) {
    void *px = rt_pixels_new(w, h);
    int64_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            // Encode position into color: ARGB with R=x, G=y
            int64_t color = (int64_t)0xFF000000 | ((x & 0xFF) << 16) | ((y & 0xFF) << 8);
            rt_pixels_set(px, x, y, color);
        }
    }
    return px;
}

TEST(RTSprite, NewBasic) {
    void *atlas = make_test_atlas(64, 64);
    void *sheet = rt_spritesheet_new(atlas);
    ASSERT(sheet != NULL, "spritesheet_new should return non-null");
    ASSERT(rt_obj_class_id(sheet) == RT_SPRITESHEET_CLASS_ID, "sheet has class id");
    ASSERT(rt_spritesheet_region_count(sheet) == 0, "new sheet has 0 regions");
    ASSERT(rt_spritesheet_width(sheet) == 64, "width matches atlas");
    ASSERT(rt_spritesheet_height(sheet) == 64, "height matches atlas");
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, NewNullAtlas) {
    void *sheet = rt_spritesheet_new(NULL);
    ASSERT(sheet == NULL, "null atlas returns null sheet");
}

TEST(RTSprite, NewRejectsWrongAtlasHandle) {
    void *not_pixels = rt_obj_new_i64(0, 8);
    void *sheet = rt_spritesheet_new(not_pixels);
    ASSERT(sheet == NULL, "wrong atlas handle returns null sheet");
    release_object(not_pixels);
}

TEST(RTSprite, SetAndGetRegion) {
    void *atlas = make_test_atlas(64, 64);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("walk_0");
    rt_spritesheet_set_region(sheet, name, 0, 0, 32, 32);
    ASSERT(rt_spritesheet_region_count(sheet) == 1, "1 region after set");
    ASSERT(rt_spritesheet_has_region(sheet, name) == 1, "has_region returns 1");

    void *region = rt_spritesheet_get_region(sheet, name);
    ASSERT(region != NULL, "get_region returns non-null");

    // Verify pixel data was correctly copied (pixel at 0,0 should match atlas 0,0)
    int64_t p = rt_pixels_get(region, 0, 0);
    int64_t expected = rt_pixels_get(atlas, 0, 0);
    ASSERT(p == expected, "region pixel 0,0 matches atlas 0,0");

    release_object(region);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, SpriteSheetRejectsEmptyRegionName) {
    void *atlas = make_test_atlas(16, 16);
    void *sheet = rt_spritesheet_new(atlas);
    ASSERT(sheet != NULL, "spritesheet_new should return non-null");

    rt_string empty = rt_const_cstr("");
    rt_spritesheet_set_region(sheet, empty, 0, 0, 8, 8);
    ASSERT(rt_spritesheet_region_count(sheet) == 0, "empty region name is ignored");
    ASSERT(rt_spritesheet_has_region(sheet, empty) == 0, "empty region name is not present");
    rt_string_unref(empty);

    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, SpriteSheetRejectsEmbeddedNullRegionNames) {
    void *atlas = make_test_atlas(16, 16);
    void *sheet = rt_spritesheet_new(atlas);
    ASSERT(sheet != NULL, "spritesheet_new should return non-null");

    const char bytes[] = {'h', 'e', 'r', 'o', '\0', 'a', 'l', 't'};
    rt_string embedded = rt_string_from_bytes(bytes, sizeof(bytes));
    rt_string prefix = rt_const_cstr("hero");
    rt_spritesheet_set_region(sheet, embedded, 0, 0, 8, 8);
    ASSERT(rt_spritesheet_region_count(sheet) == 0, "embedded-NUL region name is ignored");
    ASSERT(rt_spritesheet_has_region(sheet, embedded) == 0, "embedded-NUL name cannot alias");
    ASSERT(rt_spritesheet_has_region(sheet, prefix) == 0, "prefix name remains absent");
    ASSERT(rt_spritesheet_get_region(sheet, embedded) == NULL, "embedded-NUL lookup is rejected");
    ASSERT(rt_spritesheet_remove_region(sheet, embedded) == 0, "embedded-NUL removal is rejected");

    rt_string_unref(prefix);
    rt_string_unref(embedded);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, RegionOffset) {
    void *atlas = make_test_atlas(64, 64);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("frame1");
    rt_spritesheet_set_region(sheet, name, 16, 16, 16, 16);

    void *region = rt_spritesheet_get_region(sheet, name);
    ASSERT(region != NULL, "offset region returned");

    // Pixel at region(0,0) should match atlas(16,16)
    int64_t p = rt_pixels_get(region, 0, 0);
    int64_t expected = rt_pixels_get(atlas, 16, 16);
    ASSERT(p == expected, "offset region pixel matches atlas at correct position");

    release_object(region);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, HasRegionFalse) {
    void *atlas = make_test_atlas(32, 32);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("nonexistent");
    ASSERT(rt_spritesheet_has_region(sheet, name) == 0, "has_region returns 0 for missing");
    ASSERT(rt_spritesheet_get_region(sheet, name) == NULL, "get_region returns null for missing");

    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, UpdateExistingRegion) {
    void *atlas = make_test_atlas(64, 64);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("r");
    rt_spritesheet_set_region(sheet, name, 0, 0, 16, 16);
    ASSERT(rt_spritesheet_region_count(sheet) == 1, "1 region");

    // Update same name with different coords
    rt_spritesheet_set_region(sheet, name, 32, 32, 8, 8);
    ASSERT(rt_spritesheet_region_count(sheet) == 1, "still 1 region after update");

    void *region = rt_spritesheet_get_region(sheet, name);
    ASSERT(region != NULL, "get updated region");

    // Pixel at region(0,0) should now match atlas(32,32)
    int64_t p = rt_pixels_get(region, 0, 0);
    int64_t expected = rt_pixels_get(atlas, 32, 32);
    ASSERT(p == expected, "updated region reads from new atlas position");

    release_object(region);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, InvalidRegionsRejected) {
    void *atlas = make_test_atlas(32, 32);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("bad");
    rt_spritesheet_set_region(sheet, name, -1, 0, 16, 16);
    rt_spritesheet_set_region(sheet, name, 0, 0, 0, 16);
    rt_spritesheet_set_region(sheet, name, 24, 0, 16, 16);
    ASSERT(rt_spritesheet_region_count(sheet) == 0, "invalid regions are not added");
    ASSERT(rt_spritesheet_has_region(sheet, name) == 0, "invalid region name is absent");
    ASSERT(rt_spritesheet_get_region(sheet, name) == NULL, "invalid region has no pixels");

    rt_spritesheet_set_region(sheet, name, 0, 0, 16, 16);
    ASSERT(rt_spritesheet_region_count(sheet) == 1, "valid region is added");
    rt_spritesheet_set_region(sheet, name, 24, 0, 16, 16);
    void *region = rt_spritesheet_get_region(sheet, name);
    ASSERT(region != NULL, "region remains valid after rejected update");
    ASSERT(rt_pixels_width(region) == 16, "rejected update preserves width");

    release_object(region);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, RemoveRegion) {
    void *atlas = make_test_atlas(32, 32);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string name = rt_const_cstr("r");
    rt_spritesheet_set_region(sheet, name, 0, 0, 16, 16);
    ASSERT(rt_spritesheet_region_count(sheet) == 1, "1 region");

    int8_t removed = rt_spritesheet_remove_region(sheet, name);
    ASSERT(removed == 1, "remove returns 1");
    ASSERT(rt_spritesheet_region_count(sheet) == 0, "0 regions after remove");
    ASSERT(rt_spritesheet_has_region(sheet, name) == 0, "has returns 0 after remove");

    // Removing again returns 0
    int8_t removed2 = rt_spritesheet_remove_region(sheet, name);
    ASSERT(removed2 == 0, "remove non-existent returns 0");

    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, MultipleRegions) {
    void *atlas = make_test_atlas(64, 64);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string n0 = rt_const_cstr("a");
    rt_string n1 = rt_const_cstr("b");
    rt_string n2 = rt_const_cstr("c");

    rt_spritesheet_set_region(sheet, n0, 0, 0, 16, 16);
    rt_spritesheet_set_region(sheet, n1, 16, 0, 16, 16);
    rt_spritesheet_set_region(sheet, n2, 32, 0, 16, 16);
    ASSERT(rt_spritesheet_region_count(sheet) == 3, "3 regions");

    ASSERT(rt_spritesheet_has_region(sheet, n0) == 1, "has a");
    ASSERT(rt_spritesheet_has_region(sheet, n1) == 1, "has b");
    ASSERT(rt_spritesheet_has_region(sheet, n2) == 1, "has c");

    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, FromGrid) {
    void *atlas = make_test_atlas(64, 32);
    void *sheet = rt_spritesheet_from_grid(atlas, 32, 32);
    ASSERT(sheet != NULL, "from_grid returns non-null");

    // 64/32=2 cols, 32/32=1 row => 2 regions named "0" and "1"
    ASSERT(rt_spritesheet_region_count(sheet) == 2, "grid produces 2 regions");

    rt_string n0 = rt_const_cstr("0");
    rt_string n1 = rt_const_cstr("1");
    ASSERT(rt_spritesheet_has_region(sheet, n0) == 1, "has region 0");
    ASSERT(rt_spritesheet_has_region(sheet, n1) == 1, "has region 1");

    // Region "1" should start at atlas x=32
    void *r1 = rt_spritesheet_get_region(sheet, n1);
    ASSERT(r1 != NULL, "region 1 not null");
    int64_t p = rt_pixels_get(r1, 0, 0);
    int64_t expected = rt_pixels_get(atlas, 32, 0);
    ASSERT(p == expected, "grid region 1 starts at correct atlas offset");

    release_object(r1);
    release_object(sheet);
    release_object(atlas);
}

TEST(RTSprite, FromGridInvalid) {
    void *atlas = make_test_atlas(32, 32);
    ASSERT(rt_spritesheet_from_grid(NULL, 16, 16) == NULL, "null atlas returns null");
    ASSERT(rt_spritesheet_from_grid(atlas, 0, 16) == NULL, "zero frame_w returns null");
    ASSERT(rt_spritesheet_from_grid(atlas, 16, 0) == NULL, "zero frame_h returns null");
    release_object(atlas);
}

TEST(RTSprite, RegionNames) {
    void *atlas = make_test_atlas(32, 32);
    void *sheet = rt_spritesheet_new(atlas);

    rt_string n0 = rt_const_cstr("alpha");
    rt_string n1 = rt_const_cstr("beta");
    rt_spritesheet_set_region(sheet, n0, 0, 0, 16, 16);
    rt_spritesheet_set_region(sheet, n1, 16, 0, 16, 16);

    void *names = rt_spritesheet_region_names(sheet);
    ASSERT(names != NULL, "region_names returns non-null");
    ASSERT(rt_seq_len(names) == 2, "names seq has 2 entries");
    ASSERT(std::strcmp(rt_string_cstr((rt_string)rt_seq_get(names, 0)), "alpha") == 0,
           "first region name is alpha");
    ASSERT(std::strcmp(rt_string_cstr((rt_string)rt_seq_get(names, 1)), "beta") == 0,
           "second region name is beta");

    release_object(sheet);
    ASSERT(std::strcmp(rt_string_cstr((rt_string)rt_seq_get(names, 0)), "alpha") == 0,
           "region name snapshot survives sheet release");

    release_object(names);
    release_object(atlas);
}

TEST(RTSprite, NullSafety) {
    rt_string name = rt_const_cstr("test");
    // All functions should handle NULL gracefully
    ASSERT(rt_spritesheet_region_count(NULL) == 0, "null count = 0");
    ASSERT(rt_spritesheet_width(NULL) == 0, "null width = 0");
    ASSERT(rt_spritesheet_height(NULL) == 0, "null height = 0");
    ASSERT(rt_spritesheet_has_region(NULL, name) == 0, "null has = 0");
    ASSERT(rt_spritesheet_get_region(NULL, name) == NULL, "null get = null");
    ASSERT(rt_spritesheet_remove_region(NULL, name) == 0, "null remove = 0");
}

TEST(RTSprite, AnimatorDuplicateClipReplacesAndPlayRestarts) {
    rt_sprite_animator_t *anim = rt_sprite_animator_new();
    ASSERT_TRUE(anim != nullptr);

    ASSERT_TRUE(rt_sprite_animator_add_clip(anim, "walk", 0, 4, 100, 1) == 1);
    ASSERT_TRUE(rt_sprite_animator_add_clip(anim, "walk", 5, 2, 40, 0) == 1);
    ASSERT_TRUE(anim->clip_count == 1);
    ASSERT_TRUE(anim->clips[0].start_frame == 5);
    ASSERT_TRUE(anim->clips[0].frame_count == 2);
    ASSERT_TRUE(anim->clips[0].frame_delay_ms == 40);
    ASSERT_TRUE(anim->clips[0].loop == 0);

    ASSERT_TRUE(rt_sprite_animator_play(anim, "walk") == 1);
    anim->clip_frame = 1;
    anim->last_update_ms = 12345;
    ASSERT_TRUE(rt_sprite_animator_play(anim, "walk") == 1);
    ASSERT_TRUE(anim->current_clip == 0);
    ASSERT_TRUE(anim->clip_frame == 0);
    ASSERT_TRUE(anim->last_update_ms == INT64_MIN);
    ASSERT_TRUE(rt_sprite_animator_is_playing(anim) == 1);

    rt_sprite_animator_destroy(anim);
}

TEST(RTSprite, AnimatorRejectsWrongHandleClass) {
    void *pixels = rt_pixels_new(1, 1);
    ASSERT_TRUE(pixels != nullptr);
    rt_sprite_animator_t *wrong = (rt_sprite_animator_t *)pixels;

    ASSERT_TRUE(rt_sprite_animator_add_clip(wrong, "idle", 0, 1, 100, 1) == 0);
    ASSERT_TRUE(rt_sprite_animator_play(wrong, "idle") == 0);
    rt_sprite_animator_stop(wrong);
    rt_sprite_animator_update(wrong, nullptr);
    ASSERT_TRUE(rt_sprite_animator_is_playing(wrong) == 0);
    ASSERT_TRUE(rt_sprite_animator_get_current(wrong) == nullptr);
    rt_sprite_animator_destroy(wrong);

    release_object(pixels);
}

/// @brief Main.
int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
