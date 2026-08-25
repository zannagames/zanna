//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/tests/runtime/RTTilemapRenderContractTests.cpp
// Purpose: Pixel-placement, ownership, projection, source-frame, traversal,
//   scaled rendering, and imported animation contract tests for Tilemap.
//
// Key invariants:
//   - Ordinary orthogonal rendering retains its historical fast-path behavior.
//   - Imported layouts project, scale, and round deterministically without losing
//     source frames or authored traversal order.
//
// Ownership/Lifetime:
//   - Stub runtime objects expose reference transitions to the test process.
//   - Test-owned Tilemaps are destroyed when finalizer behavior is under test.
//
// Links: src/runtime/graphics/2d/rt_tilemap.c,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_graphics_internal.h"
#include "rt_physics2d_internal.h"
#include "rt_tilemap.h"
}

#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#ifndef RT_PIXELS_CLASS_ID
#define RT_PIXELS_CLASS_ID INT64_C(-0x600201)
#endif

namespace {

struct ObjHeader {
    int64_t class_id;
    void (*finalizer)(void *);
};

struct StubPixels {
    int64_t width;
    int64_t height;
    uint32_t *data;
    uint64_t generation;
    uint64_t cache_identity;
    uint64_t alpha_scan_generation;
    uint64_t alpha_classification_scan_count;
    int8_t alpha_scan_valid;
    int8_t alpha_scan_classification;
    int64_t id;
    int refcount;
    bool freed;

    StubPixels(int64_t width_value, int64_t height_value, int64_t id_value, int refs, bool is_freed)
        : width(width_value), height(height_value), data(nullptr), generation(1),
          cache_identity(static_cast<uint64_t>(id_value)), alpha_scan_generation(0),
          alpha_classification_scan_count(0), alpha_scan_valid(0), alpha_scan_classification(0),
          id(id_value), refcount(refs), freed(is_freed) {}
};

struct StubCanvas {
    int64_t width;
    int64_t height;
};

struct StubBody {
    void *vptr;
    uint64_t state_magic;
    double x;
    double y;
    double prev_x;
    double prev_y;
    double w;
    double h;
    double vx;
    double vy;
    double fx;
    double fy;
    double mass;
    double inv_mass;
    double restitution;
    double friction;
    int64_t collision_layer;
    int64_t collision_mask;
    double radius;
    int8_t is_circle;
    double sleep_time;
    int8_t is_sleeping;
    void *owner_world;
    int64_t world_index;

    StubBody(double x_value,
             double y_value,
             double previous_y,
             double width,
             double height,
             double velocity_x,
             double velocity_y)
        : vptr(nullptr), state_magic(RT_PHYSICS2D_BODY_STATE_MAGIC), x(x_value), y(y_value),
          prev_x(x_value), prev_y(previous_y), w(width), h(height), vx(velocity_x), vy(velocity_y),
          fx(0.0), fy(0.0), mass(1.0), inv_mass(1.0), restitution(0.0), friction(0.0),
          collision_layer(1), collision_mask(-1), radius(0.0), is_circle(0), sleep_time(0.0),
          is_sleeping(0), owner_world(nullptr), world_index(-1) {}
};

struct BlitCall {
    int64_t dx;
    int64_t dy;
    int64_t sx;
    int64_t sy;
    int64_t w;
    int64_t h;
    int64_t pixels_id;
};

BlitCall g_blits[16];
int g_blit_count = 0;
int g_alpha_blit_count = 0;
StubPixels *g_clones[16];
int g_clone_count = 0;
int g_pixels_freed = 0;
int g_scale_count = 0;
bool g_fail_map_new = false;

void reset_blits() {
    std::memset(g_blits, 0, sizeof(g_blits));
    g_blit_count = 0;
    g_alpha_blit_count = 0;
}

void reset_pixels_tracking() {
    std::memset(g_clones, 0, sizeof(g_clones));
    g_clone_count = 0;
    g_pixels_freed = 0;
    g_scale_count = 0;
}

ObjHeader *header_from_payload(void *obj) {
    return reinterpret_cast<ObjHeader *>(obj) - 1;
}

void destroy_rt_object(void *obj) {
    if (!obj)
        return;
    ObjHeader *header = header_from_payload(obj);
    if (header->finalizer)
        header->finalizer(obj);
    std::free(header);
}

} // namespace

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
    if (class_id == RT_PIXELS_CLASS_ID || class_id == RT_PHYSICS2D_BODY_CLASS_ID)
        return 1;
    return rt_obj_class_id(obj) == class_id;
}

extern "C" void rt_obj_set_finalizer(void *obj, void (*finalizer)(void *)) {
    if (!obj)
        return;
    header_from_payload(obj)->finalizer = finalizer;
}

extern "C" void rt_trap(const char *msg) {
    std::fprintf(stderr, "unexpected rt_trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

extern "C" void rt_heap_retain(void *obj) {
    if (!obj)
        return;
    auto *pixels = static_cast<StubPixels *>(obj);
    pixels->refcount++;
}

extern "C" void rt_heap_release(void *obj) {
    if (!obj)
        return;
    auto *pixels = static_cast<StubPixels *>(obj);
    assert(pixels->refcount > 0);
    pixels->refcount--;
    if (pixels->refcount == 0 && !pixels->freed) {
        pixels->freed = true;
        g_pixels_freed++;
    }
}

extern "C" void *rt_pixels_clone(void *pixels) {
    if (!pixels)
        return nullptr;
    auto *src = static_cast<StubPixels *>(pixels);
    auto *copy = static_cast<StubPixels *>(std::malloc(sizeof(StubPixels)));
    assert(copy != nullptr);
    new (copy) StubPixels(*src);
    copy->refcount = 1;
    copy->freed = false;
    assert(g_clone_count < (int)(sizeof(g_clones) / sizeof(g_clones[0])));
    g_clones[g_clone_count++] = copy;
    return copy;
}

extern "C" int64_t rt_pixels_width(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->width : 0;
}

extern "C" int64_t rt_pixels_height(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->height : 0;
}

extern "C" const char *rt_string_cstr(rt_string) {
    return "";
}

extern "C" int64_t rt_str_len(rt_string) {
    return 0;
}

extern "C" int64_t rt_canvas_width(void *canvas) {
    return canvas ? static_cast<StubCanvas *>(canvas)->width : 0;
}

extern "C" int64_t rt_canvas_height(void *canvas) {
    return canvas ? static_cast<StubCanvas *>(canvas)->height : 0;
}

extern "C" void rt_canvas_blit_region(void *canvas,
                                      int64_t dx,
                                      int64_t dy,
                                      void *pixels,
                                      int64_t sx,
                                      int64_t sy,
                                      int64_t w,
                                      int64_t h) {
    (void)canvas;
    assert(g_blit_count < (int)(sizeof(g_blits) / sizeof(g_blits[0])));
    g_blits[g_blit_count++] = {dx, dy, sx, sy, w, h, static_cast<StubPixels *>(pixels)->id};
}

extern "C" void rt_canvas_blit(void *canvas, int64_t x, int64_t y, void *pixels) {
    (void)canvas;
    assert(g_blit_count < (int)(sizeof(g_blits) / sizeof(g_blits[0])));
    auto *stub = static_cast<StubPixels *>(pixels);
    g_blits[g_blit_count++] = {
        x, y, 0, 0, stub ? stub->width : 0, stub ? stub->height : 0, stub ? stub->id : 0};
}

extern "C" void rt_canvas_blit_region_alpha(void *canvas,
                                            int64_t dx,
                                            int64_t dy,
                                            void *pixels,
                                            int64_t sx,
                                            int64_t sy,
                                            int64_t w,
                                            int64_t h) {
    g_alpha_blit_count++;
    rt_canvas_blit_region(canvas, dx, dy, pixels, sx, sy, w, h);
}

extern "C" void rt_canvas_blit_regions_alpha(void *canvas,
                                             void *pixels,
                                             const rt_canvas_alpha_region *regions,
                                             size_t region_count) {
    for (size_t i = 0; i < region_count; ++i) {
        const rt_canvas_alpha_region &region = regions[i];
        rt_canvas_blit_region_alpha(
            canvas, region.dx, region.dy, pixels, region.sx, region.sy, region.w, region.h);
    }
}

extern "C" void rt_canvas_blit_alpha(void *canvas, int64_t x, int64_t y, void *pixels) {
    g_alpha_blit_count++;
    rt_canvas_blit(canvas, x, y, pixels);
}

extern "C" void *rt_pixels_new(int64_t width, int64_t height) {
    auto *pixels = static_cast<StubPixels *>(std::malloc(sizeof(StubPixels)));
    assert(pixels != nullptr);
    new (pixels) StubPixels(width, height, 900, 1, false);
    return pixels;
}

extern "C" void rt_pixels_copy(
    void *, int64_t, int64_t, void *, int64_t, int64_t, int64_t, int64_t) {}

extern "C" void *rt_pixels_scale(void *pixels, int64_t new_width, int64_t new_height) {
    g_scale_count++;
    auto *src = static_cast<StubPixels *>(pixels);
    auto *scaled = static_cast<StubPixels *>(std::malloc(sizeof(StubPixels)));
    assert(scaled != nullptr);
    new (scaled) StubPixels(new_width, new_height, src ? src->id : 0, 1, false);
    return scaled;
}

extern "C" int32_t rt_obj_release_check0(void *obj) {
    if (!obj)
        return 0;
    auto *pixels = static_cast<StubPixels *>(obj);
    if (pixels->refcount > 0)
        pixels->refcount--;
    return pixels->refcount == 0 ? 1 : 0;
}

extern "C" void rt_obj_free(void *obj) {
    std::free(obj);
}

extern "C" void *rt_map_new(void) {
    if (g_fail_map_new)
        return nullptr;
    return rt_obj_new_i64(0, 1);
}

extern "C" void rt_map_set_int(void *, rt_string, int64_t) {}

extern "C" void rt_map_set_bool(void *, rt_string, int8_t) {}

extern "C" rt_string rt_const_cstr(const char *) {
    return nullptr;
}

extern "C" double rt_physics2d_body_x(void *body) {
    return body ? static_cast<StubBody *>(body)->x : 0.0;
}

extern "C" double rt_physics2d_body_y(void *body) {
    return body ? static_cast<StubBody *>(body)->y : 0.0;
}

extern "C" double rt_physics2d_body_w(void *body) {
    return body ? static_cast<StubBody *>(body)->w : 0.0;
}

extern "C" double rt_physics2d_body_h(void *body) {
    return body ? static_cast<StubBody *>(body)->h : 0.0;
}

extern "C" double rt_physics2d_body_vx(void *body) {
    return body ? static_cast<StubBody *>(body)->vx : 0.0;
}

extern "C" double rt_physics2d_body_vy(void *body) {
    return body ? static_cast<StubBody *>(body)->vy : 0.0;
}

extern "C" double rt_physics2d_body_prev_y(void *body) {
    return body ? static_cast<StubBody *>(body)->prev_y : 0.0;
}

extern "C" void rt_physics2d_body_set_pos(void *body, double x, double y) {
    if (!body)
        return;
    static_cast<StubBody *>(body)->x = x;
    static_cast<StubBody *>(body)->y = y;
}

extern "C" void rt_physics2d_body_set_vel(void *body, double vx, double vy) {
    if (!body)
        return;
    static_cast<StubBody *>(body)->vx = vx;
    static_cast<StubBody *>(body)->vy = vy;
}

static void test_draw_uses_all_visible_layers_in_order() {
    reset_pixels_tracking();
    StubPixels base_tileset{32, 16, 100, 0, false};
    StubPixels overlay_tileset{16, 16, 200, 0, false};
    StubCanvas canvas{16, 16};

    void *tm = rt_tilemap_new(2, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tileset(tm, &base_tileset);
    assert(rt_tilemap_add_layer(tm, nullptr) == 1);
    rt_tilemap_set_tile_layer(tm, 0, 0, 0, 1);
    rt_tilemap_set_tile_layer(tm, 1, 0, 0, 1);
    rt_tilemap_set_layer_tileset(tm, 1, &overlay_tileset);

    reset_blits();
    rt_tilemap_draw(tm, &canvas, 0, 0);

    assert(g_blit_count == 2);
    assert(g_alpha_blit_count == 2);
    assert(g_blits[0].pixels_id == 100);
    assert(g_blits[1].pixels_id == 200);
    assert(g_blits[0].dx == 0 && g_blits[0].dy == 0);
    assert(g_blits[1].dx == 0 && g_blits[1].dy == 0);
    assert(g_blits[0].sx == 0 && g_blits[1].sx == 0);
}

static void test_draw_region_can_render_layer_only_tilesets() {
    reset_pixels_tracking();
    StubPixels overlay_tileset{16, 16, 300, 0, false};
    StubCanvas canvas{16, 16};

    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(tm != nullptr);
    assert(rt_tilemap_add_layer(tm, nullptr) == 1);
    rt_tilemap_set_tile_layer(tm, 1, 0, 0, 1);
    rt_tilemap_set_layer_tileset(tm, 1, &overlay_tileset);
    rt_tilemap_set_layer_visible(tm, 0, 0);

    reset_blits();
    rt_tilemap_draw_region(tm, &canvas, 0, 0, 0, 0, 1, 1);

    assert(g_blit_count == 1);
    assert(g_alpha_blit_count == 1);
    assert(g_blits[0].pixels_id == 300);
    assert(g_blits[0].dx == 0 && g_blits[0].dy == 0);
}

static void test_tileset_clone_keeps_single_owned_reference() {
    reset_pixels_tracking();
    StubPixels base_tileset{32, 16, 400, 0, false};

    void *tm = rt_tilemap_new(2, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tileset(tm, &base_tileset);

    assert(g_clone_count == 1);
    assert(g_clones[0]->refcount == 1);
}

static void test_layer_tileset_clone_keeps_single_owned_reference() {
    reset_pixels_tracking();
    StubPixels overlay_tileset{16, 16, 500, 0, false};

    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(tm != nullptr);
    assert(rt_tilemap_add_layer(tm, nullptr) == 1);
    rt_tilemap_set_layer_tileset(tm, 1, &overlay_tileset);

    assert(g_clone_count == 1);
    assert(g_clones[0]->refcount == 1);
}

static void test_positive_offset_culls_to_visible_tiles() {
    reset_pixels_tracking();
    StubPixels base_tileset{64, 16, 550, 0, false};
    StubCanvas canvas{32, 16};

    void *tm = rt_tilemap_new(4, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tileset(tm, &base_tileset);
    for (int64_t x = 0; x < 4; x++)
        rt_tilemap_set_tile(tm, x, 0, 1);

    reset_blits();
    rt_tilemap_draw(tm, &canvas, 8, 0);

    assert(g_blit_count == 2);
    assert(g_blits[0].dx == 8);
    assert(g_blits[1].dx == 24);
}

static void test_import_source_frame_and_common_draw_offset() {
    reset_pixels_tracking();
    StubPixels atlas{48, 32, 560, 0, false};
    StubCanvas canvas{64, 64};
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(tm != nullptr);
    assert(rt_tilemap_configure_import_layout(tm,
                                              RT_TILEMAP_IMPORT_ORTHOGONAL,
                                              0,
                                              0,
                                              24,
                                              32,
                                              -4,
                                              -16,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              1) == 1);
    rt_tilemap_set_tileset(tm, &atlas);
    rt_tilemap_set_tile(tm, 0, 0, 2);

    reset_blits();
    rt_tilemap_draw(tm, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == -4 && g_blits[0].dy == -16);
    assert(g_blits[0].sx == 24 && g_blits[0].sy == 0);
    assert(g_blits[0].w == 24 && g_blits[0].h == 32);
    assert(rt_tilemap_get_tile_count(tm) == 2);
}

static void test_import_projection_and_fractional_layer_offset() {
    reset_pixels_tracking();
    StubPixels atlas{16, 8, 570, 0, false};
    StubCanvas canvas{64, 64};
    void *tm = rt_tilemap_new(2, 2, 16, 8);
    assert(tm != nullptr);
    assert(rt_tilemap_configure_import_layout(tm,
                                              RT_TILEMAP_IMPORT_ISOMETRIC,
                                              0,
                                              0,
                                              16,
                                              8,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    assert(rt_tilemap_configure_import_layer(tm, 0, 1.5, -1.5, 1.0, 1.0) == 1);
    rt_tilemap_set_tileset(tm, &atlas);
    rt_tilemap_set_tile(tm, 1, 0, 1);

    reset_blits();
    rt_tilemap_draw(tm, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 26);
    assert(g_blits[0].dy == 3);
}

static void test_import_staggered_hex_and_oblique_projection() {
    reset_pixels_tracking();
    StubPixels stagger_atlas{16, 8, 580, 0, false};
    StubPixels hex_atlas{16, 12, 581, 0, false};
    StubCanvas canvas{128, 128};

    void *staggered = rt_tilemap_new(2, 2, 16, 8);
    assert(rt_tilemap_configure_import_layout(staggered,
                                              RT_TILEMAP_IMPORT_STAGGERED,
                                              0,
                                              0,
                                              16,
                                              8,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    rt_tilemap_set_tileset(staggered, &stagger_atlas);
    rt_tilemap_set_tile(staggered, 1, 1, 1);
    reset_blits();
    rt_tilemap_draw(staggered, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 24 && g_blits[0].dy == 4);

    void *hexagonal = rt_tilemap_new(1, 1, 16, 12);
    assert(rt_tilemap_configure_import_layout(hexagonal,
                                              RT_TILEMAP_IMPORT_HEXAGONAL,
                                              0,
                                              0,
                                              16,
                                              12,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              0,
                                              1,
                                              4,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              1) == 1);
    rt_tilemap_set_tileset(hexagonal, &hex_atlas);
    rt_tilemap_set_tile(hexagonal, 0, 0, 1);
    reset_blits();
    rt_tilemap_draw(hexagonal, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 0 && g_blits[0].dy == 6);

    void *oblique = rt_tilemap_new(2, 2, 16, 8);
    assert(rt_tilemap_configure_import_layout(oblique,
                                              RT_TILEMAP_IMPORT_OBLIQUE,
                                              0,
                                              0,
                                              16,
                                              8,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              1.0,
                                              -0.5,
                                              0.0,
                                              0.0,
                                              2) == 1);
    rt_tilemap_set_tileset(oblique, &stagger_atlas);
    rt_tilemap_set_tile(oblique, 1, 1, 1);
    reset_blits();
    rt_tilemap_draw(oblique, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 17 && g_blits[0].dy == 8);

    StubPixels odd_hex_atlas{5, 7, 582, 0, false};
    void *odd_hex = rt_tilemap_new(1, 2, 5, 7);
    assert(rt_tilemap_configure_import_layout(odd_hex,
                                              RT_TILEMAP_IMPORT_HEXAGONAL,
                                              0,
                                              0,
                                              5,
                                              7,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              3,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    rt_tilemap_set_tileset(odd_hex, &odd_hex_atlas);
    rt_tilemap_set_tile(odd_hex, 0, 1, 1);
    reset_blits();
    rt_tilemap_draw(odd_hex, &canvas, 0, 0);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 2 && g_blits[0].dy == 5);
}

static void test_import_parallax_and_variable_animation_durations() {
    reset_pixels_tracking();
    StubPixels atlas{48, 16, 590, 0, false};
    StubCanvas canvas{64, 64};
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(rt_tilemap_configure_import_layout(tm,
                                              RT_TILEMAP_IMPORT_ORTHOGONAL,
                                              0,
                                              0,
                                              16,
                                              16,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              4.0,
                                              4.0,
                                              1) == 1);
    assert(rt_tilemap_configure_import_layer(tm, 0, 1.5, -1.5, 0.5, 0.5) == 1);
    rt_tilemap_set_tileset(tm, &atlas);
    rt_tilemap_set_tile(tm, 0, 0, 1);
    reset_blits();
    rt_tilemap_draw(tm, &canvas, 10, 10);
    assert(g_blit_count == 1);
    assert(g_blits[0].dx == 9 && g_blits[0].dy == 6);

    const int64_t frames[] = {1, 2, 3};
    const int64_t durations[] = {100, 200, 50};
    assert(rt_tilemap_set_import_tile_anim(tm, 1, 3, frames, durations) == 1);
    assert(rt_tilemap_resolve_anim_tile(tm, 1) == 1);
    rt_tilemap_update_anims(tm, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, 1) == 2);
    rt_tilemap_update_anims(tm, 199);
    assert(rt_tilemap_resolve_anim_tile(tm, 1) == 2);
    rt_tilemap_update_anims(tm, 1);
    assert(rt_tilemap_resolve_anim_tile(tm, 1) == 3);
    rt_tilemap_update_anims(tm, 50);
    assert(rt_tilemap_resolve_anim_tile(tm, 1) == 1);
}

static void test_import_render_order_and_isometric_depth_order() {
    reset_pixels_tracking();
    StubPixels atlas{64, 16, 595, 0, false};
    StubCanvas canvas{128, 128};

    void *orthogonal = rt_tilemap_new(2, 2, 16, 16);
    assert(rt_tilemap_configure_import_layout(orthogonal,
                                              RT_TILEMAP_IMPORT_ORTHOGONAL,
                                              0,
                                              0,
                                              16,
                                              16,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_UP,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    rt_tilemap_set_tileset(orthogonal, &atlas);
    rt_tilemap_set_tile(orthogonal, 0, 0, 1);
    rt_tilemap_set_tile(orthogonal, 1, 0, 2);
    rt_tilemap_set_tile(orthogonal, 0, 1, 3);
    rt_tilemap_set_tile(orthogonal, 1, 1, 4);
    reset_blits();
    rt_tilemap_draw(orthogonal, &canvas, 0, 0);
    assert(g_blit_count == 4);
    assert(g_blits[0].sx == 32 && g_blits[1].sx == 48);
    assert(g_blits[2].sx == 0 && g_blits[3].sx == 16);

    void *isometric = rt_tilemap_new(2, 2, 16, 8);
    assert(rt_tilemap_configure_import_layout(isometric,
                                              RT_TILEMAP_IMPORT_ISOMETRIC,
                                              0,
                                              0,
                                              16,
                                              16,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    rt_tilemap_set_tileset(isometric, &atlas);
    rt_tilemap_set_tile(isometric, 0, 0, 1);
    rt_tilemap_set_tile(isometric, 1, 0, 2);
    rt_tilemap_set_tile(isometric, 0, 1, 3);
    rt_tilemap_set_tile(isometric, 1, 1, 4);
    reset_blits();
    rt_tilemap_draw(isometric, &canvas, 0, 0);
    assert(g_blit_count == 4);
    assert(g_blits[0].sx == 0 && g_blits[1].sx == 32);
    assert(g_blits[2].sx == 16 && g_blits[3].sx == 48);
}

static void test_scaled_import_preserves_projection_and_source_frame() {
    reset_pixels_tracking();
    StubPixels atlas{48, 16, 596, 0, false};
    StubCanvas canvas{128, 128};
    void *tm = rt_tilemap_new(2, 2, 16, 8);
    assert(rt_tilemap_configure_import_layout(tm,
                                              RT_TILEMAP_IMPORT_ISOMETRIC,
                                              0,
                                              0,
                                              24,
                                              16,
                                              -4,
                                              -8,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    assert(rt_tilemap_configure_import_layer(tm, 0, 1.5, -1.5, 1.0, 1.0) == 1);
    rt_tilemap_set_tileset(tm, &atlas);
    rt_tilemap_set_tile(tm, 1, 0, 2);

    reset_blits();
    rt_tilemap_draw_scaled(tm, &canvas, 10, 20, 200);
    assert(g_blit_count == 1);
    assert(g_scale_count == 1);
    assert(g_blits[0].dx == 53 && g_blits[0].dy == 9);
    assert(g_blits[0].w == 48 && g_blits[0].h == 32);
    reset_blits();
    rt_tilemap_draw_scaled(tm, &canvas, 10, 20, 200);
    assert(g_blit_count == 1);
    assert(g_scale_count == 1);
    g_clones[0]->generation++;
    rt_tilemap_draw_scaled(tm, &canvas, 10, 20, 200);
    assert(g_scale_count == 2);

    StubPixels count_atlas{24, 16, 597, 0, false};
    StubCanvas small_canvas{32, 32};
    void *count_map = rt_tilemap_new(4, 4, 16, 8);
    assert(rt_tilemap_configure_import_layout(count_map,
                                              RT_TILEMAP_IMPORT_ISOMETRIC,
                                              0,
                                              0,
                                              24,
                                              16,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              4) == 1);
    rt_tilemap_set_tileset(count_map, &count_atlas);
    rt_tilemap_set_tile(count_map, 0, 3, 1);
    assert(rt_tilemap_count_drawn_visible_scaled(count_map, &small_canvas, 0, 0, 200) == 1);
}

static void test_full_range_scale_divides_before_saturating() {
    reset_pixels_tracking();
    StubPixels atlas{3, 1, 598, 0, false};
    StubCanvas canvas{1, 1};
    void *tm = rt_tilemap_new(1, 1, 3, 1);
    rt_tilemap_set_tileset(tm, &atlas);
    rt_tilemap_set_tile(tm, 0, 0, 1);

    reset_blits();
    rt_tilemap_draw_scaled(tm, &canvas, 0, 0, INT64_MAX);

    assert(g_blit_count == 1);
    assert(g_alpha_blit_count == 1);
    assert(g_blits[0].w == INT64_C(276701161105643274));
    assert(g_blits[0].h == INT64_C(92233720368547758));
}

static void test_tileset_grid_overflow_preserves_existing_binding() {
    reset_pixels_tracking();
    StubPixels valid{16, 16, 599, 0, false};
    StubPixels impossible{INT64_MAX, INT64_MAX, 600, 0, false};
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    rt_tilemap_set_tileset(tm, &valid);
    assert(rt_tilemap_get_tile_count(tm) == 1);
    assert(g_clone_count == 1);
    assert(!g_clones[0]->freed);

    rt_tilemap_set_tileset(tm, &impossible);
    assert(rt_tilemap_get_tile_count(tm) == 1);
    assert(g_clone_count == 2);
    assert(!g_clones[0]->freed);
    assert(g_clones[1]->freed);
}

static void test_one_way_platform_tolerance_rejects_starting_inside_surface() {
    void *tm = rt_tilemap_new(1, 2, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tile(tm, 0, 1, 1);
    rt_tilemap_set_collision(tm, 1, RT_TILE_COLLISION_ONE_WAY_UP);

    StubBody body{0.0, 13.0, 12.5, 8.0, 4.0, 0.0, 1.0};
    assert(rt_tilemap_collide_body(tm, &body) == 0);
    assert(body.y == 13.0);
    assert(body.vy == 1.0);
}

static void test_fractional_aabb_checks_every_overlapped_tile() {
    void *tm = rt_tilemap_new(2, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tile(tm, 1, 0, 1);
    rt_tilemap_set_collision(tm, 1, RT_TILE_COLLISION_SOLID);

    StubBody body{15.5, 0.0, 0.0, 1.0, 8.0, 2.0, 0.0};
    assert(rt_tilemap_collide_body(tm, &body) == 1);
    assert(body.x == 15.0);
    assert(body.vx == 0.0);
}

static void test_collision_rejects_nonfinite_body_geometry() {
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tile(tm, 0, 0, 1);
    rt_tilemap_set_collision(tm, 1, RT_TILE_COLLISION_SOLID);

    StubBody body{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 8.0, 8.0, 1.0, 1.0};
    assert(rt_tilemap_collide_body(tm, &body) == 0);
    assert(std::isnan(body.x));
    assert(body.vx == 1.0 && body.vy == 1.0);
}

static void test_hit_test_propagates_result_allocation_failure() {
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(tm != nullptr);
    g_fail_map_new = true;
    assert(rt_tilemap_hit_test_scaled(tm, 0, 0, 0, 0, 100) == nullptr);
    g_fail_map_new = false;
}

static void test_finalizer_releases_owned_tilesets() {
    reset_pixels_tracking();
    StubPixels base_tileset{32, 16, 600, 0, false};
    StubPixels overlay_tileset{16, 16, 700, 0, false};

    void *tm = rt_tilemap_new(2, 1, 16, 16);
    assert(tm != nullptr);
    rt_tilemap_set_tileset(tm, &base_tileset);
    assert(rt_tilemap_add_layer(tm, nullptr) == 1);
    rt_tilemap_set_layer_tileset(tm, 1, &overlay_tileset);

    destroy_rt_object(tm);

    assert(g_clone_count == 2);
    assert(g_pixels_freed == 2);
    assert(g_clones[0]->freed);
    assert(g_clones[1]->freed);
}

int main() {
    test_draw_uses_all_visible_layers_in_order();
    test_draw_region_can_render_layer_only_tilesets();
    test_tileset_clone_keeps_single_owned_reference();
    test_layer_tileset_clone_keeps_single_owned_reference();
    test_positive_offset_culls_to_visible_tiles();
    test_import_source_frame_and_common_draw_offset();
    test_import_projection_and_fractional_layer_offset();
    test_import_staggered_hex_and_oblique_projection();
    test_import_parallax_and_variable_animation_durations();
    test_import_render_order_and_isometric_depth_order();
    test_scaled_import_preserves_projection_and_source_frame();
    test_full_range_scale_divides_before_saturating();
    test_tileset_grid_overflow_preserves_existing_binding();
    test_one_way_platform_tolerance_rejects_starting_inside_surface();
    test_fractional_aabb_checks_every_overlapped_tile();
    test_collision_rejects_nonfinite_body_geometry();
    test_hit_test_propagates_result_allocation_failure();
    test_finalizer_releases_owned_tilesets();
    std::printf("RTTilemapRenderContractTests passed.\n");
    return 0;
}
