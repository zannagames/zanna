//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGraphics2DTests.cpp
// Purpose: Tests for Zanna.Graphics 2D rendering, tilemap, UI, and game helpers.
// Key invariants:
//   - Rendering tests compare exact RGBA storage or bounded channel rounding.
//   - Adversarial handles and integer limits must fail safely without OOB access.
// Ownership/Lifetime:
//   - Runtime objects remain test-owned for the process lifetime unless a test
//     explicitly releases them to exercise finalization.
// Links: src/runtime/graphics/2d/rt_graphics2d.c,
//        src/runtime/graphics/2d/rt_graphics2d.h
//
//===----------------------------------------------------------------------===//

#include "rt_camera.h"
#include "rt_graphics.h"
#include "rt_graphics2d.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_sprite.h"
#include "rt_string.h"
#include "rt_tilemap.h"

#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

extern "C" {
#include "rt_graphics2d_internal.h"
}

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static int64_t red_of(int64_t rgba) {
    return ((uint32_t)rgba >> 24) & 255;
}

static int64_t green_of(int64_t rgba) {
    return ((uint32_t)rgba >> 16) & 255;
}

static int64_t blue_of(int64_t rgba) {
    return ((uint32_t)rgba >> 8) & 255;
}

static void test_graphics2d_handles_have_unique_classes_and_reject_wrong_types() {
    void *target = rt_rendertarget2d_new(1, 1);
    void *pixels = rt_pixels_new(1, 1);
    void *texture = rt_texture2d_new(pixels);
    void *surface = rt_surface2d_new(1, 1);
    void *gpu_texture = rt_gputexture2d_new(pixels);
    void *screen_scaler = rt_screenscaler_new(320, 180, 640, 360);
    void *renderer = rt_renderer2d_new(1);
    void *sprite = rt_sprite_new(pixels);
    void *shader = rt_shader2d_new(RT_GRAPHICS2D_EFFECT_NONE);
    void *post = rt_postprocess2d_new();

    assert(target != nullptr);
    assert(texture != nullptr);
    assert(surface != nullptr);
    assert(gpu_texture != nullptr);
    assert(screen_scaler != nullptr);
    assert(renderer != nullptr);
    assert(sprite != nullptr);
    assert(shader != nullptr);
    assert(post != nullptr);

    int64_t target_class = rt_obj_class_id(target);
    int64_t texture_class = rt_obj_class_id(texture);
    int64_t surface_class = rt_obj_class_id(surface);
    int64_t gpu_texture_class = rt_obj_class_id(gpu_texture);
    int64_t screen_scaler_class = rt_obj_class_id(screen_scaler);
    int64_t renderer_class = rt_obj_class_id(renderer);
    int64_t pixels_class = rt_obj_class_id(pixels);
    int64_t sprite_class = rt_obj_class_id(sprite);
    int64_t shader_class = rt_obj_class_id(shader);
    int64_t post_class = rt_obj_class_id(post);

    assert(target_class != 0);
    assert(texture_class != 0);
    assert(renderer_class != 0);
    assert(target_class != texture_class);
    assert(target_class != renderer_class);
    assert(texture_class != renderer_class);
    assert(texture_class != pixels_class);
    assert(texture_class != sprite_class);
    assert(surface_class != target_class);
    assert(gpu_texture_class != texture_class);
    assert(screen_scaler_class != 0);
    assert(shader_class != post_class);
    assert(post_class != texture_class);
    assert(rt_rendertarget2d_width(surface) == 1);
    assert(rt_texture2d_width(gpu_texture) == 1);
    assert(rt_viewport2d_get_scale(screen_scaler) == 2000);

    assert(rt_texture2d_new(sprite) == nullptr);
    assert(rt_gputexture2d_new(sprite) == nullptr);
    assert(rt_tileset2d_new(sprite, 1, 1) == nullptr);
    assert(rt_tileset2d_columns(sprite) == 0);
    assert(rt_tileset2d_get_tile_pixels(sprite, 0) == nullptr);
    assert(rt_nineslice2d_new(sprite, 0, 0, 0, 0) == nullptr);
    assert(rt_animatedsprite2d_new(nullptr) == nullptr);
    assert(rt_animatedsprite2d_new(pixels) == nullptr);
    assert(rt_camerarig2d_new(pixels) == nullptr);
    assert(rt_rendertarget2d_get_pixels(sprite) == nullptr);
    assert(rt_texture2d_get_pixels(sprite) == nullptr);
    assert(rt_tilelayer2d_width(sprite) == 0);
    assert(rt_tilelayer2d_get(sprite, 0, 0) == -1);
    rt_tilelayer2d_set(sprite, 0, 0, 1);
    assert(rt_viewport2d_get_scale(sprite) == 1000);
    assert(rt_viewport2d_world_to_screen_x(sprite, 123) == 123);
    assert(rt_shader2d_apply(post, pixels) == nullptr);
    assert(rt_postprocess2d_apply(shader, pixels) == nullptr);
    assert(rt_color_get_a(rt_color_rgb(1, 2, 3)) == 0);
    assert(rt_color_get_a(rt_color_rgba(1, 2, 3, 0)) == 0);
    printf("test_graphics2d_handles_have_unique_classes_and_reject_wrong_types: PASSED\n");
}

static void test_render_target_alpha_blend() {
    void *target = rt_rendertarget2d_new(4, 4);
    assert(target != nullptr);
    assert(rt_rendertarget2d_width(target) == 4);
    assert(rt_rendertarget2d_height(target) == 4);

    rt_rendertarget2d_clear(target, 0x000000FF);
    void *src = rt_pixels_new(1, 1);
    rt_pixels_set(src, 0, 0, 0xFF000080);
    rt_rendertarget2d_draw_pixels(target, 1, 1, src);

    int64_t blended = rt_pixels_get(rt_rendertarget2d_get_pixels(target), 1, 1);
    assert(red_of(blended) >= 126 && red_of(blended) <= 129);
    assert(green_of(blended) == 0);
    assert(blue_of(blended) == 0);

    rt_rendertarget2d_draw_region(target, INT64_MAX - 1, 0, src, -1, 0, INT64_MAX, 1);
    assert(rt_pixels_get(rt_rendertarget2d_get_pixels(target), 0, 0) == 0x000000FF);

    rt_rendertarget2d_clear(target, 0x00000000);
    rt_rendertarget2d_draw_pixels(target, 2, 2, src);
    int64_t over_transparent = rt_pixels_get(rt_rendertarget2d_get_pixels(target), 2, 2);
    assert(red_of(over_transparent) == 255);
    assert(green_of(over_transparent) == 0);
    assert(blue_of(over_transparent) == 0);
    assert((over_transparent & 255) >= 127 && (over_transparent & 255) <= 128);
    printf("test_render_target_alpha_blend: PASSED\n");
}

static void test_render_target_self_overlap_region_uses_snapshot() {
    void *target = rt_rendertarget2d_new(4, 1);
    assert(target != nullptr);
    void *pixels = rt_rendertarget2d_get_pixels(target);
    assert(pixels != nullptr);

    rt_pixels_set(pixels, 0, 0, 0xFF0000FF);
    rt_pixels_set(pixels, 1, 0, 0x00FF00FF);
    rt_pixels_set(pixels, 2, 0, 0x0000FFFF);
    rt_pixels_set(pixels, 3, 0, 0xFFFFFFFF);

    rt_rendertarget2d_draw_region(target, 1, 0, pixels, 0, 0, 3, 1);

    assert(rt_pixels_get(pixels, 0, 0) == 0xFF0000FF);
    assert(rt_pixels_get(pixels, 1, 0) == 0xFF0000FF);
    assert(rt_pixels_get(pixels, 2, 0) == 0x00FF00FF);
    assert(rt_pixels_get(pixels, 3, 0) == 0x0000FFFF);
    printf("test_render_target_self_overlap_region_uses_snapshot: PASSED\n");
}

static void test_core_arithmetic_payload_and_generation_guards() {
    assert(rt2d_saturating_mul_i64(INT64_MAX - 1, 1) == INT64_MAX - 1);
    assert(rt2d_saturating_mul_i64(INT64_MAX, 2) == INT64_MAX);
    assert(rt2d_saturating_mul_i64(INT64_MIN, -1) == INT64_MAX);
    assert(rt2d_saturating_mul_i64(INT64_MIN, 1) == INT64_MIN);
    assert(rt2d_saturating_mul_i64(-3037000500LL, 3037000500LL) == INT64_MIN);
#if SIZE_MAX < UINT64_MAX
    assert(rt2d_checked_count(1000000, 100, 64, nullptr) == 0);
#endif

    void *path = rt_path2d_new(1);
    assert(path != nullptr);
    void *undersized = rt_obj_new_i64(rt_obj_class_id(path), 1);
    assert(undersized != nullptr);
    void *pixels = rt_pixels_new(1, 1);
    assert(pixels != nullptr);
    rt_path2d_clear(undersized);
    rt_path2d_move_to(undersized, 1, 2);
    rt_path2d_line_to(undersized, 3, 4);
    rt_path2d_draw_to_pixels(undersized, pixels, 0xFFFFFF);
    assert(rt_path2d_count(undersized) == 0);
    assert(rt_path2d_get_x(undersized, 0) == 0);
    assert(rt_path2d_get_y(undersized, 0) == 0);

    void *target = rt_rendertarget2d_new(1, 1);
    void *transparent = rt_pixels_new(1, 1);
    assert(target != nullptr);
    assert(transparent != nullptr);
    void *target_pixels = rt_rendertarget2d_get_pixels(target);
    assert(rt_pixels_generation(target_pixels) == 0);
    rt_rendertarget2d_draw_pixels(target, 0, 0, transparent);
    assert(rt_pixels_generation(target_pixels) == 0);

    void *material_source = rt_pixels_new(2, 2);
    assert(material_source != nullptr);
    rt_pixels_fill(material_source, 0x808080FF);
    void *material = rt_material2d_new();
    assert(material != nullptr);
    rt_material2d_set_tint(material, 0x00FF0000);
    void *processed = rt_material2d_apply(material, material_source);
    assert(processed != nullptr);
    assert(rt_pixels_generation(processed) == 1);
    printf("test_core_arithmetic_payload_and_generation_guards: PASSED\n");
}

static void test_sampled_self_blits_snapshot_and_low_alpha_unpremultiply() {
    constexpr int64_t kA = 0xFF0000FF;
    constexpr int64_t kB = 0x00FF00FF;
    constexpr int64_t kC = 0x0000FFFF;
    constexpr int64_t kD = 0xFFFFFFFF;
    void *target = rt_rendertarget2d_new(4, 1);
    assert(target != nullptr);
    void *pixels = rt_rendertarget2d_get_pixels(target);
    assert(pixels != nullptr);
    void *texture = rt_texture2d_new(pixels);
    void *renderer = rt_renderer2d_new(1);
    assert(texture != nullptr);
    assert(renderer != nullptr);

    rt_pixels_set(pixels, 0, 0, kA);
    rt_pixels_set(pixels, 1, 0, kB);
    rt_pixels_set(pixels, 2, 0, kC);
    rt_pixels_set(pixels, 3, 0, kD);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_scaled(renderer, texture, 1, 0, 4, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    assert(rt_pixels_get(pixels, 0, 0) == kA);
    assert(rt_pixels_get(pixels, 1, 0) == kA);
    assert(rt_pixels_get(pixels, 2, 0) == kB);
    assert(rt_pixels_get(pixels, 3, 0) == kC);
    rt_renderer2d_end(renderer, nullptr);

    rt_pixels_set(pixels, 0, 0, kA);
    rt_pixels_set(pixels, 1, 0, kB);
    rt_pixels_set(pixels, 2, 0, kC);
    rt_pixels_set(pixels, 3, 0, kD);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_rotated_at(renderer, texture, 1, 0, 0, 0, 0.0);
    rt_renderer2d_flush_to_target(renderer, target);
    assert(rt_pixels_get(pixels, 0, 0) == kA);
    assert(rt_pixels_get(pixels, 1, 0) == kA);
    assert(rt_pixels_get(pixels, 2, 0) == kB);
    assert(rt_pixels_get(pixels, 3, 0) == kC);
    rt_renderer2d_end(renderer, nullptr);

    void *low_alpha_source = rt_pixels_new(2, 1);
    assert(low_alpha_source != nullptr);
    rt_pixels_set(low_alpha_source, 0, 0, 0xFF000001);
    void *low_alpha_texture = rt_texture2d_new(low_alpha_source);
    assert(low_alpha_texture != nullptr);
    rt_texture2d_set_filter(low_alpha_texture, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_texture2d_set_wrap(low_alpha_texture, RT_GRAPHICS2D_WRAP_CLAMP);
    rt_rendertarget2d_resize(target, 3, 1);
    rt_rendertarget2d_clear(target, 0);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_set_blend_mode(renderer, RT_GRAPHICS2D_BLEND_OPAQUE);
    rt_renderer2d_draw_texture_scaled(renderer, low_alpha_texture, 0, 0, 3, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    int64_t middle = rt_pixels_get(rt_rendertarget2d_get_pixels(target), 1, 0);
    assert(red_of(middle) == 255);
    assert((middle & 255) == 1);
    rt_renderer2d_end(renderer, nullptr);
    printf("test_sampled_self_blits_snapshot_and_low_alpha_unpremultiply: PASSED\n");
}

static void *undersized_like(void *valid_object) {
    assert(valid_object != nullptr);
    void *undersized = rt_obj_new_i64(rt_obj_class_id(valid_object), 1);
    assert(undersized != nullptr);
    return undersized;
}

static void test_extended_payload_transform_and_bulk_fill_guards() {
    void *pixels = rt_pixels_new(4, 3);
    assert(pixels != nullptr);
    void *target_a = rt_rendertarget2d_new(1, 1);
    void *target_b = rt_rendertarget2d_new(1, 1);
    void *sprite = rt_sprite_new(pixels);
    assert(target_a != nullptr);
    assert(target_b != nullptr);
    assert(sprite != nullptr);

    void *material = rt_material2d_new();
    void *transform = rt_transform2d_new();
    void *sampler = rt_sampler2d_new();
    void *blend = rt_blendstate2d_new();
    void *sprite_renderer = rt_spriterenderer2d_new();
    void *chunk_cache = rt_tilechunkcache2d_new(1, 1);
    void *tile_renderer = rt_tilemaprenderer2d_new();
    void *clip = rt_animationclip2d_new(0, 1, 1, 0);
    void *animated = rt_animatedsprite2d_new(sprite);
    void *layout = rt_textlayout2d_new();
    void *pass = rt_renderpass2d_new(target_a, target_b);
    void *graph = rt_rendergraph2d_new(1);
    void *mask = rt_collisionmask2d_new(1, 1);
    void *hitbox = rt_hitbox2d_new(0, 0, 1, 1);
    void *palette = rt_palette2d_new();
    void *gradient = rt_gradient2d_new(0x000000FF, 0xFFFFFFFF, 2);
    void *rig = rt_camerarig2d_new(nullptr);
    void *packer = rt_texturepackeratlas_new(pixels);
    void *aseprite = rt_asepriteimporter_new();
    void *tiled = rt_tiledmaploader_new();
    assert(material && transform && sampler && blend && sprite_renderer && chunk_cache &&
           tile_renderer && clip && animated && layout && pass && graph && mask && hitbox &&
           palette && gradient && rig && packer && aseprite && tiled);

    void *bad_material = undersized_like(material);
    void *bad_transform = undersized_like(transform);
    void *bad_sampler = undersized_like(sampler);
    void *bad_blend = undersized_like(blend);
    void *bad_sprite_renderer = undersized_like(sprite_renderer);
    void *bad_chunk_cache = undersized_like(chunk_cache);
    void *bad_tile_renderer = undersized_like(tile_renderer);
    void *bad_clip = undersized_like(clip);
    void *bad_animated = undersized_like(animated);
    void *bad_layout = undersized_like(layout);
    void *bad_pass = undersized_like(pass);
    void *bad_graph = undersized_like(graph);
    void *bad_mask = undersized_like(mask);
    void *bad_hitbox = undersized_like(hitbox);
    void *bad_palette = undersized_like(palette);
    void *bad_gradient = undersized_like(gradient);
    void *bad_rig = undersized_like(rig);
    void *bad_packer = undersized_like(packer);
    void *bad_aseprite = undersized_like(aseprite);
    void *bad_tiled = undersized_like(tiled);

    rt_material2d_set_alpha(bad_material, 1);
    assert(rt_material2d_get_alpha(bad_material) == 255);
    rt_transform2d_set_x(bad_transform, 1);
    assert(rt_transform2d_get_x(bad_transform) == 0);
    rt_sampler2d_set_filter(bad_sampler, RT_GRAPHICS2D_FILTER_LINEAR);
    assert(rt_sampler2d_get_filter(bad_sampler) == RT_GRAPHICS2D_FILTER_NEAREST);
    rt_blendstate2d_set_alpha(bad_blend, 1);
    assert(rt_blendstate2d_get_alpha(bad_blend) == 255);
    rt_spriterenderer2d_set_material(bad_sprite_renderer, material);
    assert(rt_tilechunkcache2d_get_chunk_width(bad_chunk_cache) == 0);
    assert(rt_tilemaprenderer2d_get_draw_count(bad_tile_renderer) == 0);
    assert(rt_animationclip2d_get_frame_count(bad_clip) == 0);
    assert(rt_animatedsprite2d_is_playing(bad_animated) == 0);
    assert(rt_textlayout2d_measure_width(bad_layout, rt_str_from_lit("x", 1)) > 0);
    rt_renderpass2d_execute(bad_pass);
    assert(rt_rendergraph2d_get_count(bad_graph) == 0);
    assert(rt_collisionmask2d_get(bad_mask, 0, 0) == 0);
    assert(rt_hitbox2d_contains(bad_hitbox, 0, 0) == 0);
    assert(rt_palette2d_get_count(bad_palette) == 0);
    assert(rt_gradient2d_sample(bad_gradient, 50) == 0);
    assert(rt_camerarig2d_get_render_x(bad_rig) == 0);
    assert(rt_texturepackeratlas_get_atlas(bad_packer) == nullptr);
    assert(rt_asepriteimporter_get_frame_width(bad_aseprite) == 0);
    assert(rt_tiledmaploader_get_tile_width(bad_tiled) == 0);

    rt_transform2d_set_origin(transform, INT64_MAX - 1, 0);
    assert(rt_transform2d_transform_x(transform, INT64_MAX, 0) == INT64_MAX);
    rt_transform2d_set_origin(transform, 0, 0);
    rt_transform2d_set_rotation(transform, INT64_MAX);
    int64_t huge_rotation_x = rt_transform2d_transform_x(transform, 1000, 200);
    int64_t huge_rotation_y = rt_transform2d_transform_y(transform, 1000, 200);
    rt_transform2d_set_rotation(transform, INT64_MAX % 360);
    assert(rt_transform2d_transform_x(transform, 1000, 200) == huge_rotation_x);
    assert(rt_transform2d_transform_y(transform, 1000, 200) == huge_rotation_y);

    assert(rt_pixels_generation(pixels) == 0);
    rt_gradient2d_fill_horizontal(gradient, pixels);
    assert(rt_pixels_generation(pixels) == 1);
    rt_gradient2d_fill_horizontal(gradient, pixels);
    assert(rt_pixels_generation(pixels) == 1);
    rt_gradient2d_fill_vertical(gradient, pixels);
    assert(rt_pixels_generation(pixels) == 2);
    rt_gradient2d_fill_vertical(gradient, pixels);
    assert(rt_pixels_generation(pixels) == 2);

    void *empty_palette = rt_palette2d_new();
    void *palette_clone = rt_palette2d_apply(empty_palette, pixels);
    assert(palette_clone != nullptr);
    assert(rt_pixels_generation(palette_clone) == 0);
    assert(rt_pixels_get(palette_clone, 3, 2) == rt_pixels_get(pixels, 3, 2));

    void *alpha_pixels = rt_pixels_new(2, 1);
    rt_pixels_set(alpha_pixels, 0, 0, 0x00000000);
    rt_pixels_set(alpha_pixels, 1, 0, 0x00000001);
    void *alpha_mask = rt_collisionmask2d_from_pixels(alpha_pixels, 0);
    assert(alpha_mask != nullptr);
    assert(rt_collisionmask2d_get(alpha_mask, 0, 0) == 0);
    assert(rt_collisionmask2d_get(alpha_mask, 1, 0) == 1);
    printf("test_extended_payload_transform_and_bulk_fill_guards: PASSED\n");
}

static void test_texture_renderer_material_and_effects() {
    void *src = rt_pixels_new(2, 2);
    rt_pixels_set(src, 0, 0, 0x808080FF);
    rt_pixels_set(src, 1, 0, 0x112233FF);

    void *texture = rt_texture2d_new(src);
    assert(texture != nullptr);
    assert(rt_texture2d_width(texture) == 2);
    assert(rt_texture2d_height(texture) == 2);
    rt_texture2d_set_filter(texture, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_texture2d_set_wrap(texture, RT_GRAPHICS2D_WRAP_REPEAT);
    assert(rt_texture2d_get_filter(texture) == RT_GRAPHICS2D_FILTER_LINEAR);
    assert(rt_texture2d_get_wrap(texture) == RT_GRAPHICS2D_WRAP_REPEAT);

    void *material = rt_material2d_new();
    rt_material2d_set_tint(material, 0x00FF0000);
    rt_material2d_set_alpha(material, 128);
    void *tinted = rt_material2d_apply(material, src);
    int64_t tinted_pixel = rt_pixels_get(tinted, 0, 0);
    assert(red_of(tinted_pixel) >= 127 && red_of(tinted_pixel) <= 129);
    assert(green_of(tinted_pixel) == 0);
    assert((tinted_pixel & 255) >= 127 && (tinted_pixel & 255) <= 129);

    void *shader = rt_shader2d_new(RT_GRAPHICS2D_EFFECT_INVERT);
    void *inverted = rt_shader2d_apply(shader, src);
    assert(rt_pixels_get(inverted, 1, 0) == 0xEEDDCCFF);

    void *post = rt_postprocess2d_new();
    rt_postprocess2d_set_effect(post, RT_GRAPHICS2D_EFFECT_GRAYSCALE);
    void *gray = rt_postprocess2d_apply(post, src);
    int64_t g = rt_pixels_get(gray, 1, 0);
    assert(red_of(g) == green_of(g));
    assert(green_of(g) == blue_of(g));

    void *target = rt_rendertarget2d_new(4, 4);
    void *renderer = rt_renderer2d_new(1);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture(renderer, texture, 1, 1);
    assert(rt_renderer2d_count(renderer) == 1);
    rt_renderer2d_flush_to_target(renderer, target);
    assert(rt_pixels_get(rt_rendertarget2d_get_pixels(target), 1, 1) == 0x808080FF);
    rt_renderer2d_end(renderer, nullptr);
    assert(rt_renderer2d_count(renderer) == 0);
    rt_renderer2d_end(renderer, nullptr);
    assert(rt_renderer2d_count(renderer) == 0);

    void *sample_src = rt_pixels_new(2, 1);
    rt_pixels_set(sample_src, 0, 0, 0xFF0000FF);
    rt_pixels_set(sample_src, 1, 0, 0x00FF00FF);
    void *sample_texture = rt_texture2d_new(sample_src);
    rt_texture2d_set_wrap(sample_texture, RT_GRAPHICS2D_WRAP_REPEAT);
    rt_rendertarget2d_resize(target, 4, 1);
    rt_rendertarget2d_clear(target, 0x00000000);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_region(renderer, sample_texture, 0, 0, 0, 0, 4, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    void *target_pixels = rt_rendertarget2d_get_pixels(target);
    assert(rt_pixels_get(target_pixels, 0, 0) == 0xFF0000FF);
    assert(rt_pixels_get(target_pixels, 1, 0) == 0x00FF00FF);
    assert(rt_pixels_get(target_pixels, 2, 0) == 0xFF0000FF);
    assert(rt_pixels_get(target_pixels, 3, 0) == 0x00FF00FF);
    rt_renderer2d_end(renderer, nullptr);

    rt_texture2d_set_filter(sample_texture, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_texture2d_set_wrap(sample_texture, RT_GRAPHICS2D_WRAP_CLAMP);
    rt_rendertarget2d_resize(target, 3, 1);
    rt_rendertarget2d_clear(target, 0x00000000);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_scaled(renderer, sample_texture, 0, 0, 3, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    target_pixels = rt_rendertarget2d_get_pixels(target);
    int64_t middle = rt_pixels_get(target_pixels, 1, 0);
    assert(red_of(middle) >= 126 && red_of(middle) <= 129);
    assert(green_of(middle) >= 126 && green_of(middle) <= 129);
    rt_renderer2d_end(renderer, nullptr);

    void *alpha_sample_src = rt_pixels_new(2, 1);
    rt_pixels_set(alpha_sample_src, 0, 0, 0xFF0000FF);
    rt_pixels_set(alpha_sample_src, 1, 0, 0x00000000);
    void *alpha_sample_texture = rt_texture2d_new(alpha_sample_src);
    rt_texture2d_set_filter(alpha_sample_texture, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_texture2d_set_wrap(alpha_sample_texture, RT_GRAPHICS2D_WRAP_CLAMP);
    rt_rendertarget2d_clear(target, 0x00000000);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_scaled(renderer, alpha_sample_texture, 0, 0, 3, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    target_pixels = rt_rendertarget2d_get_pixels(target);
    int64_t alpha_middle = rt_pixels_get(target_pixels, 1, 0);
    assert(red_of(alpha_middle) >= 250);
    assert((alpha_middle & 255) >= 126 && (alpha_middle & 255) <= 129);
    rt_renderer2d_end(renderer, nullptr);

    void *atlas_src = rt_pixels_new(3, 1);
    rt_pixels_set(atlas_src, 0, 0, 0xFF0000FF);
    rt_pixels_set(atlas_src, 1, 0, 0x00FF00FF);
    rt_pixels_set(atlas_src, 2, 0, 0x0000FFFF);
    void *atlas_texture = rt_texture2d_new(atlas_src);
    rt_texture2d_set_filter(atlas_texture, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_texture2d_set_wrap(atlas_texture, RT_GRAPHICS2D_WRAP_CLAMP);
    rt_rendertarget2d_resize(target, 1, 1);
    rt_rendertarget2d_clear(target, 0x00000000);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_region(renderer, atlas_texture, 0, 0, 1, 0, 1, 1);
    rt_renderer2d_flush_to_target(renderer, target);
    target_pixels = rt_rendertarget2d_get_pixels(target);
    assert(rt_pixels_get(target_pixels, 0, 0) == 0x00FF00FF);
    rt_renderer2d_end(renderer, nullptr);

    rt_rendertarget2d_clear(target, 0x101010FF);
    void *add_src = rt_pixels_new(1, 1);
    rt_pixels_set(add_src, 0, 0, 0x202000FF);
    rt_renderer2d_begin(renderer);
    rt_renderer2d_set_blend_mode(renderer, RT_GRAPHICS2D_BLEND_ADD);
    rt_renderer2d_draw_pixels(renderer, add_src, 0, 0);
    rt_renderer2d_flush_to_target(renderer, target);
    assert(rt_pixels_get(rt_rendertarget2d_get_pixels(target), 0, 0) == 0x303010FF);
    rt_renderer2d_end(renderer, nullptr);
    printf("test_texture_renderer_material_and_effects: PASSED\n");
}

static void test_viewport_tiles_and_objects() {
    void *viewport = rt_viewport2d_new(320, 180, 1280, 720);
    assert(rt_viewport2d_get_scale(viewport) == 4000);
    assert(rt_viewport2d_world_to_screen_x(viewport, 10) == 40);
    assert(rt_viewport2d_screen_to_world_y(viewport, 80) == 20);
    rt_viewport2d_set_screen_size(viewport, 1000, 720);
    rt_viewport2d_set_integer_scaling(viewport, 1);
    assert(rt_viewport2d_get_scale(viewport) == 3000);
    assert(rt_viewport2d_get_offset_x(viewport) == 20);
    rt_viewport2d_set_screen_size(viewport, 160, 90);
    assert(rt_viewport2d_get_scale(viewport) == 500);
    assert(rt_viewport2d_get_offset_x(viewport) == 0);
    assert(rt_viewport2d_get_offset_y(viewport) == 0);

    void *tiles = rt_pixels_new(4, 2);
    for (int64_t y = 0; y < 2; y++) {
        for (int64_t x = 0; x < 2; x++)
            rt_pixels_set(tiles, x, y, 0xFF0000FF);
        for (int64_t x = 2; x < 4; x++)
            rt_pixels_set(tiles, x, y, 0x00FF00FF);
    }
    void *tileset = rt_tileset2d_new(tiles, 2, 2);
    assert(rt_tileset2d_columns(tileset) == 2);
    assert(rt_tileset2d_rows(tileset) == 1);
    assert(rt_tileset2d_tile_count(tileset) == 2);
    void *tile = rt_tileset2d_get_tile_pixels(tileset, 1);
    assert(rt_pixels_get(tile, 0, 0) == 0x00FF00FF);

    void *layer = rt_tilelayer2d_new(3, 2);
    rt_tilelayer2d_set(layer, 1, 1, 7);
    assert(rt_tilelayer2d_get(layer, 1, 1) == 7);
    rt_tilelayer2d_set_opacity(layer, 300);
    assert(rt_tilelayer2d_get_opacity(layer) == 100);
    rt_tilelayer2d_set_opacity(layer, -20);
    assert(rt_tilelayer2d_get_opacity(layer) == 0);

    void *objects = rt_objectlayer2d_new(1);
    int64_t index = rt_objectlayer2d_add_rect(objects, 10, 20, 30, 40, 5);
    assert(index == 0);
    assert(rt_objectlayer2d_count(objects) == 1);
    assert(rt_objectlayer2d_get_x(objects, 0) == 10);
    assert(rt_objectlayer2d_get_height(objects, 0) == 40);
    assert(rt_objectlayer2d_get_type(objects, 0) == 5);
    int64_t normalized = rt_objectlayer2d_add_rect(objects, 40, 50, -10, -20, 6);
    assert(normalized == 1);
    assert(rt_objectlayer2d_get_x(objects, 1) == 30);
    assert(rt_objectlayer2d_get_y(objects, 1) == 30);
    assert(rt_objectlayer2d_get_width(objects, 1) == 10);
    assert(rt_objectlayer2d_get_height(objects, 1) == 20);
    assert(rt_objectlayer2d_add_rect(objects, 0, 0, 0, 1, 7) == -1);
    assert(rt_objectlayer2d_add_rect(objects, 0, 0, INT64_MIN, 1, 7) == -1);
    assert(rt_objectlayer2d_add_rect(objects, INT64_MIN, 0, -1, 1, 7) == -1);
    assert(rt_objectlayer2d_count(objects) == 2);

    void *autotile = rt_autotile2d_new();
    rt_autotile2d_set_variant(autotile, 5, 42);
    assert(rt_autotile2d_resolve(autotile, 5) == 42);
    rt_autotile2d_apply(autotile, layer, 2, 0, 5);
    assert(rt_tilelayer2d_get(layer, 2, 0) == 42);
    printf("test_viewport_tiles_and_objects: PASSED\n");
}

static void test_paths_shapes_text_nineslice_and_debugdraw() {
    void *pixels = rt_pixels_new(8, 8);
    void *path = rt_path2d_new(2);
    rt_path2d_move_to(path, 0, 0);
    rt_path2d_line_to(path, 3, 0);
    assert(rt_path2d_count(path) == 2);
    rt_path2d_draw_to_pixels(path, pixels, 0x00FF0000);
    assert(rt_pixels_get(pixels, 3, 0) == 0xFF0000FF);
    rt_path2d_draw_to_pixels(path, pixels, rt_color_rgba(0, 0, 255, 255));
    assert(rt_pixels_get(pixels, 3, 0) == 0x0000FFFF);

    void *shape = rt_shaperenderer2d_new();
    rt_shaperenderer2d_set_stroke(shape, 0x0000FF00);
    rt_shaperenderer2d_line(shape, pixels, 0, 1, 3, 1);
    assert(rt_pixels_get(pixels, 3, 1) == 0x00FF00FF);
    rt_shaperenderer2d_set_stroke(shape, rt_color_rgba(255, 0, 0, 255));
    rt_shaperenderer2d_line(shape, pixels, 0, 3, 3, 3);
    assert(rt_pixels_get(pixels, 3, 3) == 0xFF0000FF);
    rt_shaperenderer2d_set_stroke(shape, 0xFF0000FF);
    rt_shaperenderer2d_line(shape, pixels, 0, 5, 3, 5);
    assert(rt_pixels_get(pixels, 3, 5) == 0xFF0000FF);
    rt_shaperenderer2d_set_stroke(shape, -1);
    rt_shaperenderer2d_line(shape, pixels, 0, 7, 7, 7);
    assert(rt_pixels_get(pixels, 7, 7) == 0);
    void *stroke_disabled_path = rt_path2d_new(2);
    rt_path2d_move_to(stroke_disabled_path, 6, 6);
    rt_path2d_line_to(stroke_disabled_path, 7, 7);
    rt_shaperenderer2d_path(shape, pixels, stroke_disabled_path);
    assert(rt_pixels_get(pixels, 7, 7) == 0);

    rt_string text = rt_str_from_lit("Hi", 2);
    void *text_renderer = rt_textrenderer2d_new();
    rt_textrenderer2d_set_scale(text_renderer, 2);
    assert(rt_textrenderer2d_measure_width(text_renderer, text) == 32);
    assert(rt_textrenderer2d_measure_height(text_renderer, text) == 16);
    rt_textrenderer2d_set_font(text_renderer, pixels);
    assert(rt_textrenderer2d_measure_width(text_renderer, text) == 32);
    void *sdf = rt_sdffont_new(nullptr, 6);
    assert(rt_sdffont_get_bitmap_font(sdf) == nullptr);
    assert(rt_sdffont_get_spread(sdf) == 6);
    assert(rt_sdffont_new(pixels, 6) == nullptr);

    void *source = rt_pixels_new(3, 3);
    rt_pixels_set(source, 0, 0, 0xFF0000FF);
    rt_pixels_set(source, 2, 0, 0x00FF00FF);
    rt_pixels_set(source, 0, 2, 0x0000FFFF);
    rt_pixels_set(source, 2, 2, 0xFFFF00FF);
    void *slice = rt_nineslice2d_new(source, 1, 1, 1, 1);
    void *target = rt_pixels_new(5, 5);
    rt_nineslice2d_draw_to_pixels(slice, target, 0, 0, 5, 5);
    assert(rt_pixels_get(target, 0, 0) == 0xFF0000FF);
    assert(rt_pixels_get(target, 4, 0) == 0x00FF00FF);
    assert(rt_pixels_get(target, 0, 4) == 0x0000FFFF);
    assert(rt_pixels_get(target, 4, 4) == 0xFFFF00FF);

    void *overlap_source = rt_pixels_new(5, 5);
    for (int64_t py = 0; py < 5; ++py) {
        for (int64_t px = 0; px < 5; ++px) {
            uint32_t value =
                (uint32_t)(((px + 1) * 31) << 24) | (uint32_t)(((py + 1) * 37) << 16) | 0x000055FFu;
            rt_pixels_set(overlap_source, px, py, value);
        }
    }
    void *overlap_expected = rt_pixels_clone(overlap_source);
    void *overlap_slice = rt_nineslice2d_new(overlap_source, 1, 1, 1, 1);
    rt_nineslice2d_draw_to_pixels(overlap_slice, overlap_expected, 1, 0, 4, 5);
    rt_nineslice2d_draw_to_pixels(overlap_slice, overlap_source, 1, 0, 4, 5);
    for (int64_t py = 0; py < 5; ++py) {
        for (int64_t px = 0; px < 5; ++px)
            assert(rt_pixels_get(overlap_source, px, py) ==
                   rt_pixels_get(overlap_expected, px, py));
    }

    void *debug = rt_debugdraw2d_new(1);
    rt_debugdraw2d_line(debug, 0, 2, 3, 2, 0x000000FF);
    rt_debugdraw2d_rect(debug, 1, 1, 3, 3, 0x00FFFFFF);
    rt_debugdraw2d_line(debug, 0, 4, 3, 4, rt_color_rgba(0, 255, 0, 255));
    assert(rt_debugdraw2d_count(debug) == 3);
    rt_debugdraw2d_rect(debug, 0, 0, 0, 3, 0x00FFFFFF);
    rt_debugdraw2d_circle(debug, 0, 0, -1, 0x00FFFFFF);
    assert(rt_debugdraw2d_count(debug) == 3);
    rt_debugdraw2d_draw_to_pixels(debug, pixels);
    assert(rt_pixels_get(pixels, 0, 2) == 0x0000FFFF);
    assert(rt_pixels_get(pixels, 3, 4) == 0x00FF00FF);
    rt_debugdraw2d_clear(debug);
    assert(rt_debugdraw2d_count(debug) == 0);
    printf("test_paths_shapes_text_nineslice_and_debugdraw: PASSED\n");
}

static void test_transform_sampler_blend_and_sprite_renderer() {
    void *transform = rt_transform2d_new();
    assert(transform != nullptr);
    rt_transform2d_set_position(transform, 10, 20);
    rt_transform2d_set_scale(transform, 200, 200);
    rt_transform2d_translate(transform, 1, -2);
    assert(rt_transform2d_get_x(transform) == 11);
    assert(rt_transform2d_get_y(transform) == 18);
    assert(rt_transform2d_transform_x(transform, 2, 3) == 15);
    assert(rt_transform2d_transform_y(transform, 2, 3) == 24);
    rt_transform2d_set_position(transform, INT64_MAX, INT64_MAX);
    rt_transform2d_set_origin(transform, 0, 0);
    rt_transform2d_set_scale(transform, 100, 100);
    assert(rt_transform2d_transform_x(transform, 1, 1) == INT64_MAX);
    assert(rt_transform2d_transform_y(transform, 1, 1) == INT64_MAX);

    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x336699FF);
    void *texture = rt_texture2d_new(pixels);
    void *sampler = rt_sampler2d_new();
    rt_sampler2d_set_filter(sampler, RT_GRAPHICS2D_FILTER_LINEAR);
    rt_sampler2d_set_wrap(sampler, RT_GRAPHICS2D_WRAP_REPEAT);
    rt_sampler2d_apply_to_texture(sampler, texture);
    assert(rt_texture2d_get_filter(texture) == RT_GRAPHICS2D_FILTER_LINEAR);
    assert(rt_texture2d_get_wrap(texture) == RT_GRAPHICS2D_WRAP_REPEAT);

    void *blend = rt_blendstate2d_new();
    rt_blendstate2d_set_blend_mode(blend, RT_GRAPHICS2D_BLEND_OPAQUE);
    rt_blendstate2d_set_alpha(blend, 300);
    assert(rt_blendstate2d_get_alpha(blend) == 255);

    void *renderer = rt_renderer2d_new(1);
    void *sprite_renderer = rt_spriterenderer2d_new();
    rt_spriterenderer2d_set_sampler(sprite_renderer, sampler);
    rt_spriterenderer2d_set_blend_state(sprite_renderer, blend);
    rt_renderer2d_begin(renderer);
    rt_spriterenderer2d_draw_texture(sprite_renderer, renderer, texture, 0, 0);
    assert(rt_renderer2d_count(renderer) == 1);

    void *texture_default = rt_texture2d_new(pixels);
    assert(rt_texture2d_get_filter(texture_default) == RT_GRAPHICS2D_FILTER_NEAREST);
    assert(rt_texture2d_get_wrap(texture_default) == RT_GRAPHICS2D_WRAP_CLAMP);
    rt_spriterenderer2d_draw_texture(sprite_renderer, renderer, texture_default, 0, 0);
    assert(rt_texture2d_get_filter(texture_default) == RT_GRAPHICS2D_FILTER_NEAREST);
    assert(rt_texture2d_get_wrap(texture_default) == RT_GRAPHICS2D_WRAP_CLAMP);

    void *white = rt_pixels_new(1, 1);
    rt_pixels_set(white, 0, 0, 0xFFFFFFFF);
    void *state_renderer = rt_renderer2d_new(2);
    void *state_target = rt_rendertarget2d_new(2, 1);
    void *state_material = rt_material2d_new();
    rt_material2d_set_tint(state_material, 0x000000FF);
    void *state_sprite_renderer = rt_spriterenderer2d_new();
    rt_spriterenderer2d_set_material(state_sprite_renderer, state_material);
    rt_renderer2d_begin(state_renderer);
    rt_renderer2d_set_tint(state_renderer, 0x00FF0000);
    rt_spriterenderer2d_draw_pixels(state_sprite_renderer, state_renderer, white, 0, 0);
    rt_renderer2d_draw_pixels(state_renderer, white, 1, 0);
    rt_renderer2d_flush_to_target(state_renderer, state_target);
    void *state_pixels = rt_rendertarget2d_get_pixels(state_target);
    assert(rt_pixels_get(state_pixels, 0, 0) == 0x0000FFFF);
    assert(rt_pixels_get(state_pixels, 1, 0) == 0xFF0000FF);
    rt_renderer2d_end(state_renderer, nullptr);
    printf("test_transform_sampler_blend_and_sprite_renderer: PASSED\n");
}

static void test_animation_collision_palette_gradient_and_rig() {
    void *frame0 = rt_pixels_new(2, 2);
    void *frame1 = rt_pixels_new(2, 2);
    rt_pixels_fill(frame0, 0x000000FF);
    rt_pixels_fill(frame1, 0xFFFFFFFF);
    void *sprite = rt_sprite_new(frame0);
    rt_sprite_add_frame(sprite, frame1);

    void *clip = rt_animationclip2d_new(0, 2, 50, 1);
    void *animated = rt_animatedsprite2d_new(sprite);
    assert(rt_animatedsprite2d_is_playing(animated) == 0);
    rt_animatedsprite2d_play(animated);
    assert(rt_animatedsprite2d_is_playing(animated) == 0);
    rt_animatedsprite2d_set_clip(animated, clip);
    assert(rt_animatedsprite2d_is_playing(animated) == 1);
    rt_animatedsprite2d_update(animated, 50);
    assert(rt_animatedsprite2d_get_frame(animated) == 1);
    assert(rt_sprite_get_frame(sprite) == 1);
    rt_animatedsprite2d_stop(animated);
    assert(rt_animatedsprite2d_is_playing(animated) == 0);
    assert(rt_animatedsprite2d_get_frame(animated) == 0);
    assert(rt_sprite_get_frame(sprite) == 0);
    rt_animatedsprite2d_play(animated);
    assert(rt_animatedsprite2d_is_playing(animated) == 1);
    rt_animatedsprite2d_update(animated, 50);
    assert(rt_animatedsprite2d_get_frame(animated) == 1);
    rt_animatedsprite2d_update(animated, INT64_MAX);
    assert(rt_sprite_get_frame(sprite) >= 0);
    assert(rt_sprite_get_frame(sprite) < rt_sprite_get_frame_count(sprite));

    void *oneshot = rt_animationclip2d_new(0, 2, 50, 0);
    rt_animatedsprite2d_set_clip(animated, oneshot);
    rt_animatedsprite2d_update(animated, 100);
    assert(rt_animatedsprite2d_is_playing(animated) == 0);
    assert(rt_animatedsprite2d_get_frame(animated) == 1);
    rt_animatedsprite2d_play(animated);
    assert(rt_animatedsprite2d_is_playing(animated) == 1);
    assert(rt_animatedsprite2d_get_frame(animated) == 0);
    assert(rt_sprite_get_frame(sprite) == 0);

    void *bad_clip = rt_animationclip2d_new(99, 5, 10, 0);
    rt_animatedsprite2d_set_clip(animated, bad_clip);
    rt_animatedsprite2d_update(animated, 10);
    assert(rt_animatedsprite2d_is_playing(animated) == 0);

    void *mask_a = rt_collisionmask2d_new(2, 2);
    void *mask_b = rt_collisionmask2d_new(2, 2);
    rt_collisionmask2d_set(mask_a, 1, 1, 1);
    rt_collisionmask2d_set(mask_b, 0, 0, 1);
    assert(rt_collisionmask2d_overlaps(mask_a, 0, 0, mask_b, 1, 1) == 1);
    rt_collisionmask2d_set(mask_a, 1, 0, 1);
    rt_collisionmask2d_set(mask_b, 0, 0, 1);
    assert(rt_collisionmask2d_overlaps(mask_a, INT64_MAX - 1, 0, mask_b, INT64_MAX, 0) == 1);
    void *alpha_pixels = rt_pixels_new(2, 1);
    rt_pixels_set(alpha_pixels, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(alpha_pixels, 1, 0, 0xFFFFFF00);
    void *alpha_mask = rt_collisionmask2d_from_pixels(alpha_pixels, 0);
    assert(rt_collisionmask2d_get(alpha_mask, 0, 0) == 1);
    assert(rt_collisionmask2d_get(alpha_mask, 1, 0) == 0);
    assert(rt_collisionmask2d_from_pixels(sprite, 0) == nullptr);

    void *hit_a = rt_hitbox2d_new(0, 0, 10, 10);
    void *hit_b = rt_hitbox2d_new(5, 5, 2, 2);
    assert(rt_hitbox2d_contains(hit_a, 9, 9) == 1);
    assert(rt_hitbox2d_intersects(hit_a, hit_b) == 1);
    void *hit_max = rt_hitbox2d_new(INT64_MAX - 1, 0, 2, 2);
    void *hit_edge = rt_hitbox2d_new(INT64_MAX, 1, 1, 1);
    assert(rt_hitbox2d_contains(hit_max, INT64_MAX, 1) == 1);
    assert(rt_hitbox2d_intersects(hit_max, hit_edge) == 1);

    void *palette = rt_palette2d_new();
    rt_palette2d_set_color(palette, 3, 0xFF0000FF);
    rt_palette2d_set_color(palette, 4, rt_color_rgba(0, 0, 255, 128));
    rt_palette2d_set_color(palette, 128, 0x123456FF);
    assert(rt_palette2d_get_color(palette, 4) == 0x0000FF80);
    int64_t palette_color = rt_palette2d_get_color_value(palette, 4);
    assert(rt_color_get_r(palette_color) == 0);
    assert(rt_color_get_g(palette_color) == 0);
    assert(rt_color_get_b(palette_color) == 255);
    assert(rt_color_get_a(palette_color) == 128);
    void *indexed = rt_pixels_new(1, 1);
    rt_pixels_set(indexed, 0, 0, 0x00000003);
    void *mapped_legacy = rt_palette2d_apply_legacy(palette, indexed);
    assert(rt_pixels_get(mapped_legacy, 0, 0) == 0xFF0000FF);
    rt_pixels_set(indexed, 0, 0, 0x00000004);
    void *mapped_tagged = rt_palette2d_apply_legacy(palette, indexed);
    assert(rt_pixels_get(mapped_tagged, 0, 0) == 0x0000FF80);
    rt_pixels_set(indexed, 0, 0, 0x00000080);
    void *mapped_legacy_alpha = rt_palette2d_apply_legacy(palette, indexed);
    assert(rt_pixels_get(mapped_legacy_alpha, 0, 0) == 0x123456FF);
    void *nearest_src = rt_pixels_new(2, 1);
    rt_pixels_set(nearest_src, 0, 0, 0xF01010FF);
    rt_pixels_set(nearest_src, 1, 0, 0x0010F080);
    void *mapped = rt_palette2d_apply(palette, nearest_src);
    assert(rt_pixels_get(mapped, 0, 0) == 0xFF0000FF);
    assert(rt_pixels_get(mapped, 1, 0) == 0x0000FF80);

    void *gradient = rt_gradient2d_new(0x000000FF, 0xFFFFFFFF, 2);
    assert(rt_gradient2d_sample(gradient, 100) == 0xFFFFFFFF);
    int64_t smooth_mid = rt_gradient2d_sample(gradient, 50);
    assert(red_of(smooth_mid) >= 126 && red_of(smooth_mid) <= 128);
    int64_t normalized_mid = rt_gradient2d_sample_normalized(gradient, 0.5);
    assert(red_of(normalized_mid) >= 126 && red_of(normalized_mid) <= 128);
    assert(rt_gradient2d_sample_normalized(gradient, 50.0) == smooth_mid);
    assert(rt_gradient2d_sample_normalized(gradient, 1.0) == 0xFFFFFFFF);
    void *stepped_gradient = rt_gradient2d_new(0x000000FF, 0xFFFFFFFF, 3);
    assert(rt_gradient2d_sample(stepped_gradient, 20) == 0x000000FF);
    assert(rt_gradient2d_sample(stepped_gradient, 50) == 0x7F7F7FFF);
    assert(rt_gradient2d_sample(stepped_gradient, 80) == 0xFFFFFFFF);
    void *tagged_gradient =
        rt_gradient2d_new(rt_color_rgba(0, 255, 0, 128), rt_color_rgba(255, 0, 0, 255), 2);
    assert(rt_gradient2d_sample(tagged_gradient, 0) == 0x00FF0080);
    int64_t gradient_color = rt_gradient2d_sample_color(tagged_gradient, 0);
    assert(rt_color_get_r(gradient_color) == 0);
    assert(rt_color_get_g(gradient_color) == 255);
    assert(rt_color_get_b(gradient_color) == 0);
    assert(rt_color_get_a(gradient_color) == 128);
    int64_t normalized_color = rt_gradient2d_sample_color_normalized(tagged_gradient, 0.0);
    assert(rt_color_get_r(normalized_color) == 0);
    assert(rt_color_get_g(normalized_color) == 255);
    assert(rt_color_get_b(normalized_color) == 0);
    assert(rt_color_get_a(normalized_color) == 128);
    void *grad_pixels = rt_pixels_new(2, 1);
    rt_gradient2d_fill_horizontal(gradient, grad_pixels);
    assert(rt_pixels_get(grad_pixels, 0, 0) == 0x000000FF);
    assert(rt_pixels_get(grad_pixels, 1, 0) == 0xFFFFFFFF);

    void *camera = rt_camera_new(100, 100);
    void *rig = rt_camerarig2d_new(camera);
    rt_camerarig2d_set_smoothing(rig, 50);
    rt_camerarig2d_set_target(rig, 150, 50);
    rt_camerarig2d_update(rig);
    assert(rt_camera_get_x(camera) == 50);
    rt_camerarig2d_set_smoothing(rig, 200);
    rt_camerarig2d_set_target(rig, 250, 50);
    rt_camerarig2d_update(rig);
    assert(rt_camera_get_x(camera) == 200);
    rt_camerarig2d_add_shake(rig, 3, -2);
    assert(rt_camerarig2d_get_render_x(rig) == rt_camera_get_x(camera) + 3);
    assert(rt_camerarig2d_get_render_y(rig) == rt_camera_get_y(camera) - 2);
    rt_camerarig2d_add_shake(rig, INT64_MAX, INT64_MIN);
    assert(rt_camerarig2d_get_render_x(rig) == INT64_MAX);
    assert(rt_camerarig2d_get_render_y(rig) <= rt_camera_get_y(camera));
    rt_camerarig2d_clear_shake(rig);
    rt_camera_set_x(camera, 42);
    rt_camerarig2d_set_camera(rig, alpha_pixels);
    assert(rt_camerarig2d_get_render_x(rig) == 42);
    rt_camerarig2d_set_camera(rig, nullptr);
    assert(rt_camerarig2d_get_render_x(rig) == 0);
    rt_camerarig2d_set_camera(rig, camera);
    assert(rt_camerarig2d_get_render_x(rig) == 42);
    assert(rt_material2d_get_alpha(alpha_pixels) == 255);
    assert(rt_sampler2d_get_wrap(alpha_pixels) == RT_GRAPHICS2D_WRAP_CLAMP);
    assert(rt_palette2d_apply(alpha_pixels, indexed) == nullptr);
    assert(rt_collisionmask2d_get_width(alpha_pixels) == 0);
    printf("test_animation_collision_palette_gradient_and_rig: PASSED\n");
}

static void test_camera_extreme_arithmetic_saturates() {
    void *camera = rt_camera_new(INT64_MAX, INT64_MAX);
    assert(camera != nullptr);
    rt_camera_follow(camera, INT64_MAX, INT64_MAX);
    rt_camera_move(camera, INT64_MAX, INT64_MAX);
    assert(rt_camera_get_x(camera) == INT64_MAX);
    assert(rt_camera_get_y(camera) == INT64_MAX);

    rt_camera_set_bounds(camera, INT64_MIN, INT64_MIN, INT64_MAX, INT64_MAX);
    rt_camera_smooth_follow(camera, INT64_MIN, INT64_MIN, 500);
    assert(rt_camera_get_x(camera) >= INT64_MIN);
    assert(rt_camera_get_y(camera) >= INT64_MIN);
    printf("test_camera_extreme_arithmetic_saturates: PASSED\n");
}

static void test_layout_rendergraph_tile_helpers_and_importers() {
    rt_string text = rt_str_from_lit("Hello", 5);
    void *layout = rt_textlayout2d_new();
    rt_textlayout2d_set_scale(layout, 2);
    rt_textlayout2d_set_wrap_width(layout, 16);
    assert(rt_textlayout2d_measure_width(layout, text) == 16);
    assert(rt_textlayout2d_measure_height(layout, text) >= 16);
    rt_textlayout2d_set_font(layout, rt_pixels_new(1, 1));
    assert(rt_textlayout2d_measure_width(layout, text) == 16);
    rt_textlayout2d_set_wrap_width(layout, 0);
    rt_string two_lines = rt_str_from_lit("A\nA", 3);
    assert(rt_textlayout2d_measure_width(layout, two_lines) == 16);
    assert(rt_textlayout2d_measure_height(layout, two_lines) == 32);
    rt_textlayout2d_set_scale(layout, 1);
    rt_textlayout2d_set_wrap_width(layout, 24);
    rt_string words = rt_str_from_lit("AA AA", 5);
    assert(rt_textlayout2d_measure_width(layout, words) == 16);
    assert(rt_textlayout2d_measure_height(layout, words) == 16);

    void *src = rt_rendertarget2d_new(1, 1);
    void *dst = rt_rendertarget2d_new(1, 1);
    rt_rendertarget2d_clear(src, 0x112233FF);
    void *shader = rt_shader2d_new(RT_GRAPHICS2D_EFFECT_INVERT);
    void *pass = rt_renderpass2d_new(src, dst);
    rt_renderpass2d_set_shader(pass, shader);
    void *graph = rt_rendergraph2d_new(1);
    rt_rendergraph2d_add_pass(graph, rt_rendertarget2d_get_pixels(src));
    assert(rt_rendergraph2d_get_count(graph) == 0);
    rt_rendergraph2d_add_pass(graph, pass);
    assert(rt_rendergraph2d_get_count(graph) == 1);
    rt_rendergraph2d_execute(graph);
    assert(rt_pixels_get(rt_rendertarget2d_get_pixels(dst), 0, 0) == 0xEEDDCCFF);

    void *wrong_source_pass = rt_renderpass2d_new(rt_rendertarget2d_get_pixels(src), dst);
    assert(wrong_source_pass == nullptr);
    assert(rt_renderpass2d_new(src, rt_rendertarget2d_get_pixels(dst)) == nullptr);
    void *surface_dst = rt_surface2d_new(1, 1);
    void *surface_pass = rt_renderpass2d_new(src, surface_dst);
    assert(surface_pass != nullptr);
    rt_renderpass2d_execute(surface_pass);
    assert(rt_pixels_get(rt_rendertarget2d_get_pixels(surface_dst), 0, 0) == 0x112233FF);
    assert(rt_rendertarget2d_get_pixels(rt_rendertarget2d_get_pixels(src)) == nullptr);
    rt_renderpass2d_execute(rt_rendertarget2d_get_pixels(src));

    void *cache = rt_tilechunkcache2d_new(8, 8);
    rt_tilechunkcache2d_mark_dirty(cache);
    assert(rt_tilechunkcache2d_get_dirty_count(cache) == 1);
    for (int i = 0; i < 3; i++)
        rt_tilechunkcache2d_mark_dirty(cache);
    assert(rt_tilechunkcache2d_get_dirty_count(cache) == 4);
    rt_tilechunkcache2d_clear_dirty(cache);
    assert(rt_tilechunkcache2d_get_dirty_count(cache) == 0);
    void *tile_renderer = rt_tilemaprenderer2d_new();
    rt_tilemaprenderer2d_set_chunk_cache(tile_renderer, cache);
    assert(rt_tilemaprenderer2d_get_draw_count(tile_renderer) == 0);

    void *count_tileset = rt_pixels_new(1, 1);
    rt_pixels_set(count_tileset, 0, 0, 0xFFFFFFFF);
    void *count_tilemap = rt_tilemap_new(3, 2, 1, 1);
    rt_tilemap_set_tileset(count_tilemap, count_tileset);
    rt_tilemap_set_tile(count_tilemap, 0, 0, 1);
    rt_tilemap_set_tile(count_tilemap, 1, 0, 2);
    rt_tilemap_set_tile(count_tilemap, 2, 0, 0);
    int64_t overlay = rt_tilemap_add_layer(count_tilemap, rt_str_from_lit("overlay", 7));
    rt_tilemap_set_tile_layer(count_tilemap, overlay, 0, 1, 1);
    assert(rt_tilemap_count_drawn_region(count_tilemap, 0, 0, 3, 2) == 2);
    rt_tilemap_set_layer_visible(count_tilemap, overlay, 0);
    assert(rt_tilemap_count_drawn_region(count_tilemap, -1, 0, 3, 2) == 1);

    void *atlas_pixels = rt_pixels_new(4, 4);
    void *packer = rt_texturepackeratlas_new(atlas_pixels);
    assert(rt_texturepackeratlas_region_count(atlas_pixels) == 0);
    if (rt_texturepackeratlas_get_atlas(packer)) {
        rt_string hero = rt_str_from_lit("hero", 4);
        rt_texturepackeratlas_add(packer, hero, 0, 0, 2, 2);
        assert(rt_texturepackeratlas_has(packer, hero) == 1);
        assert(rt_texturepackeratlas_region_count(packer) == 1);
    }

    void *ase = rt_asepriteimporter_new();
    rt_asepriteimporter_set_grid(ase, 0, 0);
    assert(rt_asepriteimporter_get_frame_width(ase) == 0);
    assert(rt_asepriteimporter_to_atlas(ase, atlas_pixels) == nullptr);
    rt_asepriteimporter_set_grid(ase, 2, 2);
    assert(rt_asepriteimporter_get_frame_width(ase) == 2);
    assert(rt_asepriteimporter_get_frame_height(ase) == 2);

    void *tiled = rt_tiledmaploader_new();
    rt_tiledmaploader_set_tile_size(tiled, 4, 5);
    assert(rt_tiledmaploader_get_tile_width(tiled) == 4);
    assert(rt_tiledmaploader_get_tile_height(tiled) == 5);
    void *tilemap = rt_tiledmaploader_new_tilemap(tiled, 3, 2);
    assert(tilemap != nullptr);
    assert(rt_tilemap_get_tile_width(tilemap) == 4);
    assert(rt_tilemap_get_tile_height(tilemap) == 5);
    printf("test_layout_rendergraph_tile_helpers_and_importers: PASSED\n");
}

static void test_rotation_and_scaled_tilemap_edge_inputs() {
    void *pixels = rt_pixels_new(2, 2);
    assert(pixels != nullptr);
    void *texture = rt_texture2d_new(pixels);
    assert(texture != nullptr);
    void *renderer = rt_renderer2d_new(4);
    assert(renderer != nullptr);

    rt_renderer2d_begin(renderer);
    rt_renderer2d_draw_texture_rotated(renderer, texture, 0, 0, std::nan(""));
    rt_renderer2d_draw_texture_rotated_at(
        renderer, texture, 0, 0, 1, 1, std::numeric_limits<double>::infinity());
    assert(rt_renderer2d_count(renderer) == 0);

    rt_renderer2d_draw_texture_rotated(renderer, texture, 0, 0, 720.0 + 45.0);
    assert(rt_renderer2d_count(renderer) == 1);

    void *tilemap = rt_tilemap_new(2, 2, 16, 16);
    assert(tilemap != nullptr);
    void *hit =
        rt_tilemap_hit_test_scaled(tilemap, INT64_MIN, INT64_MAX, INT64_MAX, INT64_MIN, INT64_MAX);
    assert(hit != nullptr);

    if (rt_obj_release_check0(hit))
        rt_obj_free(hit);
    if (rt_obj_release_check0(tilemap))
        rt_obj_free(tilemap);
    if (rt_obj_release_check0(renderer))
        rt_obj_free(renderer);
    if (rt_obj_release_check0(texture))
        rt_obj_free(texture);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    printf("test_rotation_and_scaled_tilemap_edge_inputs: PASSED\n");
}

int main() {
    test_graphics2d_handles_have_unique_classes_and_reject_wrong_types();
    test_render_target_alpha_blend();
    test_render_target_self_overlap_region_uses_snapshot();
    test_core_arithmetic_payload_and_generation_guards();
    test_sampled_self_blits_snapshot_and_low_alpha_unpremultiply();
    test_extended_payload_transform_and_bulk_fill_guards();
    test_texture_renderer_material_and_effects();
    test_viewport_tiles_and_objects();
    test_paths_shapes_text_nineslice_and_debugdraw();
    test_transform_sampler_blend_and_sprite_renderer();
    test_animation_collision_palette_gradient_and_rig();
    test_camera_extreme_arithmetic_saturates();
    test_layout_rendergraph_tile_helpers_and_importers();
    test_rotation_and_scaled_tilemap_edge_inputs();
    printf("RTGraphics2DTests: ALL PASSED\n");
    return 0;
}
