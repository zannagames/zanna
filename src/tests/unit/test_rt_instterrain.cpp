//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_instterrain.cpp
// Purpose: Unit tests for InstanceBatch3D and Terrain3D.
// Key invariants:
//   - Instance transforms retain aligned motion history across mutation.
//   - Terrain sampling, scaling, LOD storage, and opaque grid metadata agree.
// Ownership/Lifetime:
//   - Runtime objects created by tests are managed by the runtime test heap.
//   - White-box views are borrowed only for the duration of each assertion.
// Links: rt_instbatch3d.h, rt_terrain3d.h, rt_terrain3d_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_heap.h"
#include "rt_instbatch3d.h"
#include "rt_internal.h"
#include "rt_terrain3d.h"
#include "rt_terrain3d_internal.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_mat4_identity(void);
extern void *rt_mat4_new(double m00,
                         double m01,
                         double m02,
                         double m03,
                         double m10,
                         double m11,
                         double m12,
                         double m13,
                         double m20,
                         double m21,
                         double m22,
                         double m23,
                         double m30,
                         double m31,
                         double m32,
                         double m33);
extern void *rt_mat4_translate(double x, double y, double z);
extern void *rt_mesh3d_new_box(double sx, double sy, double sz);
extern void *rt_material3d_new_color(double r, double g, double b);
extern void *rt_pixels_new(int64_t w, int64_t h);
extern void rt_pixels_set(void *px, int64_t x, int64_t y, int64_t color);
}

typedef struct {
    void *vptr;
    void *mesh;
    void *material;
    float *transforms;
    float *current_snapshot;
    float *prev_transforms;
    int32_t instance_count;
    int32_t instance_capacity;
    int32_t motion_snapshot_count;
    int32_t prev_count;
    int64_t last_motion_frame;
    int8_t has_prev_snapshot;
    double *transforms64;
    float *visible_transforms;
    float *visible_prev_transforms;
    int32_t visible_capacity;
    int32_t visible_prev_capacity;
    float *prev_submit_transforms;
    int32_t prev_submit_capacity;
    int32_t allocation_capacity;
    double *current_snapshot64;
    double *prev_transforms64;
} rt_instbatch3d_view;

typedef struct {
    void *vptr;
    float *heights;
    int32_t width;
    int32_t depth;
    int64_t height_count;
    double scale[3];
    void **chunk_meshes;
    void **chunk_meshes_lod1;
    void **chunk_meshes_lod2;
    float *chunk_aabbs;
    uint8_t *chunk_lod_state;
    int32_t chunks_x;
    int32_t chunks_z;
    int32_t chunk_capacity;
    void *material;
    float lod_dist1;
    float lod_dist2;
    float lod_hysteresis;
    float skirt_depth;
    void *splat_map;
    void *splat_map2;
    void *layer_textures[8];
    double layer_scales[8];
    void *base_texture;
    void *baked_texture;
    int8_t splat_dirty;
} rt_terrain3d_view;

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps)) {                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

/*==========================================================================
 * InstanceBatch3D tests
 *=========================================================================*/

static void test_instbatch_create() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);
    EXPECT_TRUE(batch != nullptr, "InstanceBatch3D created");
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 0, "Batch starts empty");
}

static void test_instbatch_add() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);

    rt_instbatch3d_add(batch, rt_mat4_identity());
    rt_instbatch3d_add(batch, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_instbatch3d_add(batch, rt_mat4_translate(2.0, 0.0, 0.0));
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 3, "Batch count = 3 after 3 adds");
}

static void test_instbatch_remove() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);

    rt_instbatch3d_add(batch, rt_mat4_identity());
    rt_instbatch3d_add(batch, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_instbatch3d_remove(batch, 0);
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 1, "Batch count = 1 after remove");
}

static void test_instbatch_remove_keeps_motion_history_aligned() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);
    rt_instbatch3d_view *view = (rt_instbatch3d_view *)batch;

    rt_instbatch3d_add(batch, rt_mat4_identity());
    rt_instbatch3d_add(batch, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_instbatch3d_add(batch, rt_mat4_translate(2.0, 0.0, 0.0));

    view->motion_snapshot_count = 3;
    view->prev_count = 3;
    view->has_prev_snapshot = 1;
    view->current_snapshot64[3] = 10.0;
    view->current_snapshot64[19] = 20.0;
    view->current_snapshot64[35] = 30.0;
    view->prev_transforms64[3] = 100.0;
    view->prev_transforms64[19] = 200.0;
    view->prev_transforms64[35] = 300.0;

    rt_instbatch3d_remove(batch, 0);

    EXPECT_TRUE(rt_instbatch3d_count(batch) == 2, "Batch remove keeps the expected instance count");
    EXPECT_NEAR(view->transforms[3], 2.0, 0.01, "Batch remove swap-moves the last transform");
    EXPECT_NEAR(view->current_snapshot64[3],
                30.0,
                0.01,
                "Batch remove keeps current motion history aligned with swapped transforms");
    EXPECT_NEAR(view->prev_transforms64[3],
                300.0,
                0.01,
                "Batch remove keeps previous motion history aligned with swapped transforms");
}

static void test_instbatch_clear() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);

    rt_instbatch3d_add(batch, rt_mat4_identity());
    rt_instbatch3d_add(batch, rt_mat4_identity());
    rt_instbatch3d_clear(batch);
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 0, "Batch count = 0 after clear");
}

static void test_instbatch_sanitizes_nonfinite_matrices() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.5, 0.5, 0.5);
    void *batch = rt_instbatch3d_new(mesh, mat);
    rt_instbatch3d_view *view = (rt_instbatch3d_view *)batch;
    void *bad = rt_mat4_new(
        NAN, 0.0, 0.0, INFINITY, 0.0, INFINITY, 0.0, 2.0, 0.0, 0.0, NAN, 3.0, 0.0, 0.0, 0.0, NAN);
    rt_instbatch3d_add(batch, bad);
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 1, "Batch accepts matrix after sanitizing it");
    EXPECT_NEAR(view->transforms[0], 1.0, 0.001, "NaN matrix diagonal falls back to identity");
    EXPECT_NEAR(view->transforms[3], 0.0, 0.001, "Infinite matrix translation falls back to 0");
    EXPECT_NEAR(view->transforms[5], 1.0, 0.001, "Infinite matrix diagonal falls back to identity");
    EXPECT_NEAR(view->transforms[7], 2.0, 0.001, "Finite matrix component is preserved");
    EXPECT_NEAR(view->transforms[10], 1.0, 0.001, "NaN matrix diagonal falls back to identity");
    EXPECT_NEAR(
        view->transforms[15], 1.0, 0.001, "NaN homogeneous component falls back to identity");

    void *huge = rt_mat4_new(
        1.0, 0.0, 0.0, 1.0e20, 0.0, 1.0, 0.0, -1.0e20, 0.0, 0.0, 1.0, 5.0e19, 0.0, 0.0, 0.0, 1.0);
    rt_instbatch3d_add(batch, huge);
    EXPECT_TRUE(rt_instbatch3d_count(batch) == 2,
                "Batch accepts huge finite matrix after clamping");
    EXPECT_NEAR(view->transforms[16 + 3],
                1000000000000.0,
                10000.0,
                "Huge positive translation clamps to world bound");
    EXPECT_NEAR(view->transforms[16 + 7],
                -1000000000000.0,
                10000.0,
                "Huge negative translation clamps to world bound");
    EXPECT_NEAR(view->transforms[16 + 11],
                1000000000000.0,
                10000.0,
                "Huge finite transform component clamps before culling");
}

/*==========================================================================
 * Terrain3D tests
 *=========================================================================*/

static void test_terrain_create() {
    void *terrain = rt_terrain3d_new(64, 64);
    EXPECT_TRUE(terrain != nullptr, "Terrain3D created");
}

static void test_terrain_flat_height() {
    void *terrain = rt_terrain3d_new(32, 32);
    /* Default heights are 0, so any query should return 0 */
    EXPECT_NEAR(
        rt_terrain3d_get_height_at(terrain, 5.0, 5.0), 0.0, 0.01, "Flat terrain: height = 0");
}

static void test_terrain_normal_flat() {
    void *terrain = rt_terrain3d_new(32, 32);
    void *normal = rt_terrain3d_get_normal_at(terrain, 5.0, 5.0);
    EXPECT_NEAR(rt_vec3_x(normal), 0.0, 0.01, "Flat normal X ≈ 0");
    EXPECT_NEAR(rt_vec3_y(normal), 1.0, 0.01, "Flat normal Y ≈ 1");
    EXPECT_NEAR(rt_vec3_z(normal), 0.0, 0.01, "Flat normal Z ≈ 0");
}

static void test_terrain_edge_normals_use_one_sided_spacing() {
    void *terrain = rt_terrain3d_new(2, 2);
    auto *view = static_cast<rt_terrain3d_view *>(terrain);
    EXPECT_TRUE(view != nullptr && view->heights != nullptr,
                "Terrain edge-normal fixture exposes the height buffer");
    if (!view || !view->heights)
        return;

    view->heights[1] = 1.0f;
    view->heights[3] = 1.0f;
    void *normal = rt_terrain3d_get_normal_at(terrain, 0.0, 0.0);
    const double inv_sqrt2 = 0.7071067811865475;
    EXPECT_NEAR(
        rt_vec3_x(normal), -inv_sqrt2, 0.01, "Terrain edge normal uses one-sided X spacing");
    EXPECT_NEAR(
        rt_vec3_y(normal), inv_sqrt2, 0.01, "Terrain edge normal preserves the true edge slope");
    EXPECT_NEAR(rt_vec3_z(normal), 0.0, 0.01, "Terrain edge normal keeps flat Z slope");
}

static void test_terrain_scale() {
    void *terrain = rt_terrain3d_new(32, 32);
    rt_terrain3d_set_scale(terrain, 2.0, 10.0, 2.0);
    /* Still flat (heights=0), so height at any point should be 0 */
    EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 10.0, 10.0),
                0.0,
                0.01,
                "Scaled flat terrain: height = 0");
}

/// @brief Verify the opaque Terrain3D descriptor reports sample intervals rather than samples.
/// @details Non-unit X/Z spacing and height scale must survive the layout-independent boundary;
///          extents use `(count - 1) * spacing`, and rejected handles zero the caller's output.
static void test_terrain_grid_info_uses_sample_intervals() {
    void *terrain = rt_terrain3d_new(3, 4);
    rt_terrain3d_grid_info info = {};
    rt_terrain3d_set_scale(terrain, 2.5, 7.0, 3.0);

    EXPECT_TRUE(rt_terrain3d_get_grid_info_internal(terrain, &info) == 1,
                "Terrain opaque grid descriptor accepts a valid terrain");
    EXPECT_TRUE(info.width == 3 && info.depth == 4,
                "Terrain opaque grid descriptor preserves sample counts");
    EXPECT_NEAR(info.spacing_x, 2.5, 0.001, "Terrain grid descriptor preserves X spacing");
    EXPECT_NEAR(info.height_scale, 7.0, 0.001, "Terrain grid descriptor preserves height scale");
    EXPECT_NEAR(info.spacing_z, 3.0, 0.001, "Terrain grid descriptor preserves Z spacing");
    EXPECT_NEAR(info.extent_x, 5.0, 0.001, "Terrain X extent counts sample intervals");
    EXPECT_NEAR(info.extent_z, 9.0, 0.001, "Terrain Z extent counts sample intervals");

    info.width = 99;
    EXPECT_TRUE(rt_terrain3d_get_grid_info_internal(nullptr, &info) == 0,
                "Terrain opaque grid descriptor rejects null handles");
    EXPECT_TRUE(info.width == 0 && info.extent_x == 0.0,
                "Terrain opaque grid descriptor zeroes rejected output");
}

static void test_terrain_material() {
    void *terrain = rt_terrain3d_new(32, 32);
    void *mat = rt_material3d_new_color(0.3, 0.6, 0.2);
    rt_terrain3d_set_material(terrain, mat);
    /* Just ensure no crash — material is used during draw */
    EXPECT_TRUE(terrain != nullptr, "Terrain with material set");
}

static void test_terrain_heightmap_resample_preserves_source_edges() {
    void *terrain = rt_terrain3d_new(2, 2);
    void *heightmap = rt_pixels_new(4, 4);
    EXPECT_TRUE(terrain != nullptr && heightmap != nullptr, "Terrain heightmap fixture created");
    if (!terrain || !heightmap)
        return;

    rt_pixels_set(heightmap, 2, 2, 0x400000FF);
    rt_pixels_set(heightmap, 3, 3, 0xFFFFFFFF);
    rt_terrain3d_set_heightmap(terrain, heightmap);

    EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 1.0, 1.0),
                1.0,
                0.001,
                "Terrain heightmap resampling preserves the source bottom-right edge");
}

static void test_terrain_corrupt_private_counts_are_safe() {
    void *terrain = rt_terrain3d_new(4, 4);
    auto *view = static_cast<rt_terrain3d_view *>(terrain);
    EXPECT_TRUE(view != nullptr && view->height_count == 16,
                "Terrain test fixture records allocated height capacity");
    if (!view)
        return;

    view->width = std::numeric_limits<int32_t>::max();
    EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 1.0, 1.0),
                0.0,
                0.001,
                "Terrain height query rejects corrupt dimensions");
    void *normal = rt_terrain3d_get_normal_at(terrain, 1.0, 1.0);
    EXPECT_NEAR(rt_vec3_y(normal), 1.0, 0.001, "Terrain normal falls back on corrupt dimensions");
    EXPECT_TRUE(rt_terrain3d_build_heightmap_pixels(terrain) == nullptr,
                "Terrain heightmap export rejects corrupt dimensions");
    EXPECT_TRUE(rt_terrain3d_build_nav_mesh(terrain, 1) == nullptr,
                "Terrain nav mesh build rejects corrupt dimensions");

    view->width = 4;
    view->depth = 4;
    view->scale[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(std::isfinite(rt_terrain3d_get_height_at(terrain, 1.0, 1.0)),
                "Terrain height query repairs corrupt scale values");

    view->chunks_x = std::numeric_limits<int32_t>::max();
    view->chunks_z = std::numeric_limits<int32_t>::max();
    rt_terrain3d_set_scale(terrain, 2.0, 3.0, 4.0);
    EXPECT_TRUE(view->chunk_capacity == 1,
                "Terrain invalidation keeps using allocated chunk capacity");

    view->chunks_x = 0;
    view->chunks_z = 1;
    view->chunk_capacity = 32;
    rt_terrain3d_set_skirt_depth(terrain, 3.0);
    EXPECT_TRUE(view->chunk_capacity == 32,
                "Terrain invalidation ignores corrupt zero-sized chunk grids");
}

static void test_terrain_wrong_class_private_slots_clear_without_release() {
    void *terrain = rt_terrain3d_new(2, 2);
    auto *view = static_cast<rt_terrain3d_view *>(terrain);
    void *valid_material = rt_material3d_new_color(0.1, 0.2, 0.3);
    void *valid_pixels = rt_pixels_new(1, 1);
    void *wrong_material = rt_material3d_new_color(0.4, 0.5, 0.6);
    void *wrong_pixels = rt_pixels_new(1, 1);
    EXPECT_TRUE(view != nullptr && valid_material && valid_pixels && wrong_material && wrong_pixels,
                "Terrain private-slot corruption fixture is created");
    if (!view || !valid_material || !valid_pixels || !wrong_material || !wrong_pixels)
        return;
    rt_pixels_set(valid_pixels, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(wrong_pixels, 0, 0, 0xFFFFFFFF);

    view->material = wrong_pixels;
    size_t wrong_pixels_refcnt = rt_heap_hdr(wrong_pixels)->refcnt;
    rt_terrain3d_set_material(terrain, valid_material);
    EXPECT_TRUE(view->material == valid_material,
                "Terrain SetMaterial replaces wrong-class private material slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_pixels)->refcnt == wrong_pixels_refcnt,
                "Terrain SetMaterial does not release unowned wrong-class material slots");

    view->splat_map = wrong_material;
    size_t wrong_material_refcnt = rt_heap_hdr(wrong_material)->refcnt;
    rt_terrain3d_set_splat_map(terrain, valid_pixels);
    EXPECT_TRUE(view->splat_map == valid_pixels,
                "Terrain SetSplatMap replaces wrong-class private splat-map slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain SetSplatMap does not release unowned wrong-class splat-map slots");

    view->layer_textures[0] = wrong_material;
    rt_terrain3d_set_layer_texture(terrain, 0, valid_pixels);
    EXPECT_TRUE(view->layer_textures[0] == valid_pixels,
                "Terrain SetLayerTexture replaces wrong-class private layer texture slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain SetLayerTexture does not release unowned wrong-class layer texture slots");

    view->base_texture = wrong_material;
    rt_terrain3d_set_splat_map(terrain, nullptr);
    EXPECT_TRUE(view->base_texture == nullptr,
                "Terrain SetSplatMap(NULL) clears wrong-class cached base texture slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain SetSplatMap(NULL) does not release unowned wrong-class base textures");

    view->base_texture = wrong_material;
    view->baked_texture = wrong_material;
    rt_terrain3d_set_material(terrain, rt_material3d_new_color(0.7, 0.8, 0.9));
    EXPECT_TRUE(view->base_texture == nullptr && view->baked_texture == nullptr,
                "Terrain SetMaterial clears wrong-class cached texture slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain SetMaterial does not release unowned wrong-class cached texture slots");

    view->material = wrong_pixels;
    rt_terrain3d_set_splat_map(terrain, nullptr);
    EXPECT_TRUE(view->material == nullptr,
                "Terrain SetSplatMap(NULL) clears wrong-class private material slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_pixels)->refcnt == wrong_pixels_refcnt,
                "Terrain SetSplatMap(NULL) does not release unowned wrong-class materials");

    view->chunk_meshes[0] = wrong_material;
    rt_terrain3d_set_scale(terrain, 2.0, 1.0, 2.0);
    EXPECT_TRUE(view->chunk_meshes[0] == nullptr,
                "Terrain invalidation clears wrong-class cached chunk mesh slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain invalidation does not release unowned wrong-class chunk mesh slots");
}

static void test_terrain_corrupt_height_samples_are_clamped() {
    void *terrain = rt_terrain3d_new(2, 2);
    auto *view = static_cast<rt_terrain3d_view *>(terrain);
    EXPECT_TRUE(view != nullptr && view->heights != nullptr,
                "Terrain corruption fixture exposes the height buffer");
    if (!view || !view->heights)
        return;

    view->heights[3] = std::numeric_limits<float>::infinity();
    EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 1.0, 1.0),
                0.0,
                0.001,
                "Terrain height query repairs infinite private samples");

    view->heights[3] = 1.0e30f;
    double huge_height = rt_terrain3d_get_height_at(terrain, 1.0, 1.0);
    EXPECT_TRUE(std::isfinite(huge_height) && huge_height <= 1000000.0,
                "Terrain height query clamps huge private samples");
    void *normal = rt_terrain3d_get_normal_at(terrain, 1.0, 1.0);
    double normal_len =
        std::sqrt(rt_vec3_x(normal) * rt_vec3_x(normal) + rt_vec3_y(normal) * rt_vec3_y(normal) +
                  rt_vec3_z(normal) * rt_vec3_z(normal));
    EXPECT_TRUE(std::isfinite(normal_len) && normal_len > 0.9 && normal_len < 1.1,
                "Terrain normal remains normalized with huge private samples");
    EXPECT_TRUE(rt_terrain3d_build_nav_mesh(terrain, 1) != nullptr,
                "Terrain nav mesh build survives huge private samples");
    EXPECT_TRUE(rt_terrain3d_build_heightmap_pixels(terrain) != nullptr,
                "Terrain heightmap export survives huge private samples");

    view->heights[3] = -1.0e30f;
    double low_height = rt_terrain3d_get_height_at(terrain, 1.0, 1.0);
    EXPECT_TRUE(std::isfinite(low_height) && low_height >= -1000000.0,
                "Terrain height query clamps huge negative private samples");
}

static void test_terrain_layer_scale_setter_repairs_invalid_values() {
    void *terrain = rt_terrain3d_new(2, 2);
    auto *view = static_cast<rt_terrain3d_view *>(terrain);
    EXPECT_TRUE(view != nullptr, "Terrain layer-scale fixture created");
    if (!view)
        return;

    rt_terrain3d_set_layer_scale(terrain, 0, std::numeric_limits<double>::quiet_NaN());
    EXPECT_NEAR(view->layer_scales[0], 1.0, 0.001, "NaN terrain layer scale falls back to 1");

    rt_terrain3d_set_layer_scale(terrain, 1, -8.0);
    EXPECT_NEAR(view->layer_scales[1], 1.0, 0.001, "Negative terrain layer scale falls back to 1");

    rt_terrain3d_set_layer_scale(terrain, 2, std::numeric_limits<double>::infinity());
    EXPECT_NEAR(view->layer_scales[2], 1.0, 0.001, "Infinite terrain layer scale falls back to 1");

    rt_terrain3d_set_layer_scale(terrain, 3, 1.0e30);
    EXPECT_NEAR(view->layer_scales[3],
                1000000.0,
                0.001,
                "Huge terrain layer scale clamps to renderer-safe max");
}

int main() {
    /* InstanceBatch3D */
    test_instbatch_create();
    test_instbatch_add();
    test_instbatch_remove();
    test_instbatch_remove_keeps_motion_history_aligned();
    test_instbatch_clear();
    test_instbatch_sanitizes_nonfinite_matrices();

    /* Terrain3D */
    test_terrain_create();
    test_terrain_flat_height();
    test_terrain_normal_flat();
    test_terrain_edge_normals_use_one_sided_spacing();
    test_terrain_scale();
    test_terrain_grid_info_uses_sample_intervals();
    test_terrain_material();
    test_terrain_heightmap_resample_preserves_source_edges();
    test_terrain_corrupt_private_counts_are_safe();
    test_terrain_wrong_class_private_slots_clear_without_release();
    test_terrain_corrupt_height_samples_are_clamped();
    test_terrain_layer_scale_setter_repairs_invalid_values();

    printf("InstanceBatch3D+Terrain3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
