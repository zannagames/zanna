//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_terrain3d_upgrades.cpp
// Purpose: Unit tests for Terrain3D holes (render/nav/collision carve-through),
//   dual splat maps with 8 layers, slope/height auto-blend weight generation,
//   and streamed-tile manifest holes.
// Key invariants:
//   - Render, nav, and collision consume one hole rasterization: the terrain's
//     cell bitmask, handed to the collider via the raw mask getter.
//   - Rule-generated weights normalize to 255 (+-1) across both splat maps.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/world/rt_terrain3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_collider3d.h"
#include "rt_game3d.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_time.h"
#include "rt_vec3.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
static int g_tests_passed = 0;
static int g_tests_total = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

#define TEST(name)                                                                                 \
    do {                                                                                           \
        ++g_tests_total;                                                                           \
        std::printf("  [%d] %s... ", g_tests_total, name);                                         \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        ++g_tests_passed;                                                                          \
        std::printf("ok\n");                                                                       \
        return true;                                                                               \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::printf("FAIL: %s\n", msg);                                                            \
        return false;                                                                              \
    } while (0)

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond))                                                                               \
            FAIL(msg);                                                                             \
    } while (0)

#define EXPECT_EQ_INT(actual, expected, msg)                                                       \
    do {                                                                                           \
        const long long got_ = (long long)(actual);                                                \
        const long long want_ = (long long)(expected);                                             \
        if (got_ != want_) {                                                                       \
            std::printf("FAIL: %s (got %lld, expected %lld)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

bool write_text_file(const char *path, const char *text) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const size_t len = std::strlen(text);
    const bool ok = len == 0 || std::fwrite(text, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

/// Two-level heightmap text: left half low (0.0), right half high (1.0).
bool write_step_heightmap(const char *path, int width, int depth) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "zanna-heightmap-v1 %d %d\n", width, depth);
    for (int z = 0; z < depth; ++z) {
        for (int x = 0; x < width; ++x)
            std::fprintf(f, "%s ", x < width / 2 ? "0.0" : "1.0");
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

//=========================================================================
// Holes: render + nav + collision from one rasterization
//=========================================================================

bool test_holes_carve_render_nav_collision() {
    TEST("holes carve nav meshes and heightfield collision from one bitmask");

    void *terrain = rt_terrain3d_new(17, 17);
    EXPECT_TRUE(terrain != nullptr, "terrain allocates");
    rt_terrain3d_set_scale(terrain, 1.0, 1.0, 1.0);

    void *nav_before = rt_terrain3d_build_nav_mesh(terrain, 1);
    EXPECT_TRUE(nav_before != nullptr, "nav mesh builds before holes");
    long long tris_before = (long long)rt_mesh3d_get_triangle_count(nav_before);
    EXPECT_TRUE(tris_before == 2LL * 16 * 16, "full nav mesh covers every cell");

    /* Carve a 4x4-cell hole in the middle. */
    long long hole = rt_terrain3d_set_hole(terrain, 6.0, 6.0, 4.0, 4.0);
    EXPECT_TRUE(hole == 0, "first hole gets index 0");
    EXPECT_EQ_INT(rt_terrain3d_get_hole_count(terrain), 1, "hole count tracks");

    void *nav_after = rt_terrain3d_build_nav_mesh(terrain, 1);
    EXPECT_TRUE(nav_after != nullptr, "nav mesh builds after holes");
    long long tris_after = (long long)rt_mesh3d_get_triangle_count(nav_after);
    EXPECT_EQ_INT(tris_after, tris_before - 2LL * 16, "hole removes 16 cells of nav triangles");

    /* Coarse nav (step 4) hides the same footprint conservatively. */
    void *nav_coarse = rt_terrain3d_build_nav_mesh(terrain, 4);
    EXPECT_TRUE(nav_coarse != nullptr, "coarse nav builds");
    EXPECT_TRUE(rt_mesh3d_get_triangle_count(nav_coarse) < 2LL * 4 * 4,
                "coarse nav conservatively drops quads covering the hole");

    /* Collider: same mask, sampled through the shared bitmask handoff. */
    void *heightmap = rt_terrain3d_build_heightmap_pixels(terrain);
    EXPECT_TRUE(heightmap != nullptr, "heightmap pixels build");
    void *collider = rt_collider3d_new_heightfield(heightmap, 1.0, 1.0, 1.0);
    EXPECT_TRUE(collider != nullptr, "heightfield collider builds");
    int32_t cells_x = 0;
    int32_t cells_z = 0;
    const uint8_t *mask = rt_terrain3d_get_hole_mask_raw(terrain, &cells_x, &cells_z);
    EXPECT_TRUE(mask != nullptr && cells_x == 16 && cells_z == 16, "hole mask exposes cell grid");
    EXPECT_EQ_INT(rt_collider3d_heightfield_set_holes_raw(collider, mask, cells_x, cells_z),
                  1,
                  "collider accepts the hole mask");

    /* Heightfield local space is centered: cell (8,8) sits at local (0.5, 0.5). */
    double height = 0.0;
    EXPECT_EQ_INT(rt_collider3d_sample_heightfield_raw(collider, 0.5, 0.5, &height, nullptr),
                  0,
                  "sampling inside the hole reports no surface (fall-through)");
    EXPECT_EQ_INT(rt_collider3d_sample_heightfield_raw(collider, -7.5, -7.5, &height, nullptr),
                  1,
                  "sampling solid terrain still reports a surface");

    /* Removing the hole restores full coverage. */
    EXPECT_EQ_INT(rt_terrain3d_remove_hole(terrain, 0), 1, "hole removes");
    void *nav_restored = rt_terrain3d_build_nav_mesh(terrain, 1);
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(nav_restored),
                  tris_before,
                  "removing the hole restores nav coverage");
    EXPECT_TRUE(rt_terrain3d_get_hole_mask_raw(terrain, &cells_x, &cells_z) == nullptr,
                "empty hole list drops the mask");

    for (void *o :
         {nav_before, nav_after, nav_coarse, nav_restored, heightmap, collider, terrain}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    PASS();
}

bool test_holes_handle_extreme_coordinates_and_growth() {
    TEST("holes clamp extreme coordinates and grow without arithmetic overflow");

    void *terrain = rt_terrain3d_new(9, 9);
    EXPECT_TRUE(terrain != nullptr, "terrain allocates");
    rt_terrain3d_set_scale(terrain, 1.0, 1.0, 1.0);

    const double largest = std::numeric_limits<double>::max();
    const double half_largest = largest / 2.0;
    EXPECT_EQ_INT(rt_terrain3d_set_hole(terrain, -half_largest, -half_largest, largest, largest),
                  0,
                  "extreme finite rectangle is accepted without unsafe integer casts");
    int32_t cells_x = 0;
    int32_t cells_z = 0;
    const uint8_t *mask = rt_terrain3d_get_hole_mask_raw(terrain, &cells_x, &cells_z);
    EXPECT_TRUE(mask != nullptr && cells_x == 8 && cells_z == 8,
                "extreme covering rectangle produces the bounded cell mask");
    EXPECT_TRUE((mask[0] & 1u) != 0 && (mask[7] & 0x80u) != 0,
                "extreme rectangle covers both first and last terrain cells");
    rt_terrain3d_clear_holes(terrain);

    for (int i = 0; i < 20; ++i)
        EXPECT_EQ_INT(rt_terrain3d_set_hole(terrain, 100.0 + i, 100.0, 0.5, 0.5),
                      i,
                      "hole array grows geometrically");
    EXPECT_EQ_INT(rt_terrain3d_get_hole_count(terrain), 20, "grown hole count remains exact");

    if (rt_obj_release_check0(terrain))
        rt_obj_free(terrain);
    PASS();
}

//=========================================================================
// Slope/height rules generate normalized 8-layer weights
//=========================================================================

bool test_rules_generate_normalized_weights() {
    TEST("slope/height rules generate normalized weights across both splat maps");

    /* 17x17 terrain, left half at Y=0, right half at Y=10 (scale y = 10). */
    void *terrain = rt_terrain3d_new(17, 17);
    rt_terrain3d_set_scale(terrain, 1.0, 10.0, 1.0);
    void *heights = rt_pixels_new(17, 17);
    for (int z = 0; z < 17; ++z)
        for (int x = 0; x < 17; ++x)
            rt_pixels_set(heights, x, z, x < 8 ? 0x000000FF : (int64_t)0xFFFFFFFF);
    rt_terrain3d_set_heightmap(terrain, heights);

    /* Layer 0: low ground (map 0 channel R). Layer 4: highlands (map 1 channel R). */
    rt_terrain3d_set_height_layer(terrain, 0, -1.0, 4.0, 0.5);
    rt_terrain3d_set_height_layer(terrain, 4, 4.0, 20.0, 0.5);
    rt_terrain3d_rebuild_splat_weights(terrain);

    void *map0 = rt_terrain3d_get_splat_map_raw(terrain, 0);
    void *map1 = rt_terrain3d_get_splat_map_raw(terrain, 1);
    EXPECT_TRUE(map0 != nullptr && map1 != nullptr, "both splat maps generate");

    /* Low region texel: layer 0 dominant. */
    int64_t low0 = rt_pixels_get(map0, 2, 8);
    int64_t low1 = rt_pixels_get(map1, 2, 8);
    EXPECT_TRUE(((low0 >> 24) & 0xFF) > 200, "low ground weights layer 0");
    EXPECT_TRUE(((low1 >> 24) & 0xFF) < 30, "low ground leaves layer 4 empty");

    /* High region texel: layer 4 dominant. */
    int64_t high0 = rt_pixels_get(map0, 14, 8);
    int64_t high1 = rt_pixels_get(map1, 14, 8);
    EXPECT_TRUE(((high1 >> 24) & 0xFF) > 200, "highlands weight layer 4");
    EXPECT_TRUE(((high0 >> 24) & 0xFF) < 30, "highlands leave layer 0 empty");

    /* Weights across both maps sum to 255 (+-1) per texel. */
    for (int x : {2, 8, 14}) {
        int64_t p0 = rt_pixels_get(map0, x, 8);
        int64_t p1 = rt_pixels_get(map1, x, 8);
        long long sum = 0;
        for (int shift = 0; shift <= 24; shift += 8) {
            sum += (p0 >> shift) & 0xFF;
            sum += (p1 >> shift) & 0xFF;
        }
        EXPECT_TRUE(sum >= 254 && sum <= 256, "texel weights normalize to 255 (+-1)");
    }

    if (rt_obj_release_check0(heights))
        rt_obj_free(heights);
    if (rt_obj_release_check0(terrain))
        rt_obj_free(terrain);
    PASS();
}

//=========================================================================
// Streamed manifest holes
//=========================================================================

bool test_streamed_tile_manifest_holes() {
    TEST("streamed terrain tiles apply manifest holes at instantiation");

    const char *heightmap_path = "/tmp/zanna_terrain_holes_tile.height";
    const char *manifest_path = "/tmp/zanna_terrain_holes_manifest.json";
    EXPECT_TRUE(write_step_heightmap(heightmap_path, 17, 17), "tile heightmap writes");

    char manifest[768];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"tiles\":[{\"name\":\"holed\",\"path\":\"tile.bin\","
                  "\"heightmap\":\"%s\",\"center\":[0,0,0],\"radius\":64,"
                  "\"bytes\":65536,\"width\":17,\"depth\":17,\"scale\":[1,1,1],"
                  "\"holes\":[[6,6,4,4]]}]}",
                  heightmap_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Terrain Holes Stream"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream, 0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    rt_game3d_world_stream_set_center(stream, center);
    if (rt_obj_release_check0(center))
        rt_obj_free(center);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);

    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_terrain_tile_count(stream), 1, "tile becomes resident");
    void *terrain = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    EXPECT_TRUE(terrain != nullptr, "resident terrain payload accessible");
    EXPECT_EQ_INT(
        rt_terrain3d_get_hole_count(terrain), 1, "manifest hole applied at instantiation");
    int32_t cells_x = 0;
    int32_t cells_z = 0;
    EXPECT_TRUE(rt_terrain3d_get_hole_mask_raw(terrain, &cells_x, &cells_z) != nullptr,
                "resident tile carries the rasterized hole mask");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// 8-layer compat: 4-layer configs keep the realtime path available
//=========================================================================

bool test_extended_layers_survive_setters() {
    TEST("8-layer setters accept indices 4-7 and dual splat maps");

    void *terrain = rt_terrain3d_new(9, 9);
    void *tex = rt_pixels_new(2, 2);
    for (int layer = 0; layer < 8; ++layer)
        rt_terrain3d_set_layer_texture(terrain, layer, tex);
    rt_terrain3d_set_layer_scale(terrain, 7, 4.0);

    void *splat0 = rt_pixels_new(4, 4);
    void *splat1 = rt_pixels_new(4, 4);
    rt_terrain3d_set_splat_map_at(terrain, 0, splat0);
    rt_terrain3d_set_splat_map_at(terrain, 1, splat1);
    EXPECT_TRUE(rt_terrain3d_get_splat_map_raw(terrain, 0) == splat0, "map 0 binds");
    EXPECT_TRUE(rt_terrain3d_get_splat_map_raw(terrain, 1) == splat1, "map 1 binds");
    rt_terrain3d_set_splat_map_at(terrain, 1, nullptr);
    EXPECT_TRUE(rt_terrain3d_get_splat_map_raw(terrain, 1) == nullptr, "map 1 clears");

    for (void *o : {tex, splat0, splat1, terrain}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    PASS();
}

} // namespace

int main() {
    std::printf("Terrain3D upgrade tests\n");
    bool ok = true;
    ok &= test_holes_carve_render_nav_collision();
    ok &= test_holes_handle_extreme_coordinates_and_growth();
    ok &= test_rules_generate_normalized_weights();
    ok &= test_streamed_tile_manifest_holes();
    ok &= test_extended_layers_survive_setters();
    std::printf("\nTerrain upgrade tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
