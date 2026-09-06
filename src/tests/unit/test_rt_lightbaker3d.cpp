//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_lightbaker3d.cpp
// Purpose: Unit tests for baked GI — deterministic direct/bounce behavior,
//   bounded transactional scene/BVH/chart publication, immutable bake inputs,
//   light semantics, probe validation/sampling, and portable strict .vlpg I/O.
// Key invariants:
//   - Same scene + options bake byte-identical atlases (fixed seeds).
//   - Colored walls bleed their tint into neighboring texels via bounces.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/render/rt_lightbaker3d.c, docs/adr/0088
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_object.h"
extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_pixels_internal.h"
}
#include "rt_lightbaker3d.h"
#include "rt_pixels.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cfloat>
#include <cmath>
#include <csetjmp>
#include <cstdint>
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

namespace {

void release_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

template <typename Fn> bool expect_trap(const char *message, Fn &&fn) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && std::strcmp(g_last_trap, message) == 0;
}

bool copy_file(const char *source, const char *destination) {
    FILE *input = std::fopen(source, "rb");
    FILE *output = input ? std::fopen(destination, "wb") : nullptr;
    unsigned char buffer[4096];
    bool ok = input && output;
    while (ok) {
        std::size_t count = std::fread(buffer, 1, sizeof(buffer), input);
        if (count > 0 && std::fwrite(buffer, 1, count, output) != count)
            ok = false;
        if (count < sizeof(buffer)) {
            if (std::ferror(input))
                ok = false;
            break;
        }
    }
    if (input && std::fclose(input) != 0)
        ok = false;
    if (output && std::fclose(output) != 0)
        ok = false;
    return ok;
}

bool overwrite_bytes(const char *path, long offset, const unsigned char *bytes, std::size_t count) {
    FILE *file = std::fopen(path, "r+b");
    bool ok = file && std::fseek(file, offset, SEEK_SET) == 0 &&
              std::fwrite(bytes, 1, count, file) == count;
    if (file && std::fclose(file) != 0)
        ok = false;
    return ok;
}

void *make_quad(double size, double y, double nx, double ny, double nz, int vertical_x) {
    void *mesh = rt_mesh3d_new();
    double h = size * 0.5;
    if (!vertical_x) {
        rt_mesh3d_add_vertex(mesh, -h, y, -h, nx, ny, nz, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, h, y, -h, nx, ny, nz, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, h, y, h, nx, ny, nz, 1.0, 1.0);
        rt_mesh3d_add_vertex(mesh, -h, y, h, nx, ny, nz, 0.0, 1.0);
    } else {
        /* Wall at x = y-param (reuse y as offset), facing -X. */
        rt_mesh3d_add_vertex(mesh, y, 0.0, -h, nx, ny, nz, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, y, 0.0, h, nx, ny, nz, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, y, size, h, nx, ny, nz, 1.0, 1.0);
        rt_mesh3d_add_vertex(mesh, y, size, -h, nx, ny, nz, 0.0, 1.0);
    }
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 0, 2, 3);
    return mesh;
}

void *make_triangle(double size = 0.25) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, size, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, size, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    return mesh;
}

void *make_unique_triangle_strip(int triangle_count) {
    void *mesh = rt_mesh3d_new();
    for (int triangle = 0; triangle < triangle_count; ++triangle) {
        double x = (double)(triangle % 16) * 0.02;
        double z = (double)(triangle / 16) * 0.02;
        int64_t first = (int64_t)triangle * 3;
        rt_mesh3d_add_vertex(mesh, x, 0.0, z, 0.0, 1.0, 0.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x + 0.005, 0.0, z, 0.0, 1.0, 0.0, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x, 0.0, z + 0.005, 0.0, 1.0, 0.0, 0.0, 1.0);
        rt_mesh3d_add_triangle(mesh, first, first + 1, first + 2);
    }
    return mesh;
}

void *add_static_node(void *scene, void *mesh, void *material, const char *name) {
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(node, rt_const_cstr(name));
    rt_scene_node3d_set_mesh(node, mesh);
    if (material)
        rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_set_static(node, 1);
    rt_scene3d_add(scene, node);
    release_object(node);
    return node;
}

void *make_sun() {
    void *dir = rt_vec3_new(0.0, -1.0, 0.0);
    void *sun = rt_light3d_new_directional(dir, 1.0, 1.0, 1.0);
    if (rt_obj_release_check0(dir))
        rt_obj_free(dir);
    return sun;
}

long long atlas_channel_sum(void *atlas, int shift) {
    long long total = 0;
    int64_t w = rt_pixels_width(atlas);
    int64_t h = rt_pixels_height(atlas);
    for (int64_t y = 0; y < h; y += 4)
        for (int64_t x = 0; x < w; x += 4)
            total += (rt_pixels_get(atlas, x, y) >> shift) & 0xFF;
    return total;
}

bool run_bake(void *baker) {
    for (int i = 0; i < 100000; ++i) {
        if (rt_lightbaker3d_bake_step(baker))
            return true;
    }
    return false;
}

long long bake_floor_with_light(void *light, bool add_occluder) {
    void *scene = rt_scene3d_new();
    void *floor_mesh = make_quad(2.0, 0.0, 0.0, 1.0, 0.0, 0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    add_static_node(scene, floor_mesh, material, "lighting_floor");
    void *occluder_mesh = nullptr;
    if (add_occluder) {
        occluder_mesh = make_quad(2.0, 1.0, 0.0, 1.0, 0.0, 0);
        add_static_node(scene, occluder_mesh, material, "lighting_occluder");
    }
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    rt_lightbaker3d_add_light(baker, light);
    bool completed = run_bake(baker);
    void *atlas = completed ? rt_lightbaker3d_get_atlas(baker) : nullptr;
    long long sum = atlas ? atlas_channel_sum(atlas, 24) : -1;
    release_object(atlas);
    release_object(baker);
    release_object(occluder_mesh);
    release_object(material);
    release_object(floor_mesh);
    release_object(scene);
    return sum;
}

//=========================================================================

bool test_indirect_only_lightmaps() {
    TEST("indirect-only lightmaps omit primary direct light but preserve sky and fixture bounces");
    long long energy[4] = {};
    for (int mode = 0; mode < 4; ++mode) {
        void *scene = rt_scene3d_new();
        void *floor = make_quad(6.0, 0.0, 0.0, 1.0, 0.0, 0);
        void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
        add_static_node(scene, floor, material, "indirect_floor");
        void *ceiling = nullptr;
        if (mode == 3) {
            ceiling = make_quad(6.0, 2.0, 0.0, -1.0, 0.0, 0);
            add_static_node(scene, ceiling, material, "bounce_ceiling");
        }
        void *position = rt_vec3_new(0.0, 1.0, 0.0);
        void *light = rt_light3d_new_point(position, 0.7, 0.5, 0.3, 0.01);
        void *baker = rt_lightbaker3d_new(scene);
        EXPECT_TRUE(rt_lightbaker3d_get_include_direct(baker) == 1,
                    "default preserves direct bake");
        rt_lightbaker3d_set_include_direct(baker, -7);
        EXPECT_TRUE(rt_lightbaker3d_get_include_direct(baker) == 1, "C Boolean input normalizes");
        rt_lightbaker3d_set_include_direct(baker, mode == 0);
        rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
        rt_lightbaker3d_set_samples(baker, 16);
        rt_lightbaker3d_set_bounces(baker, mode >= 2 ? 1 : 0);
        if (mode == 2)
            rt_lightbaker3d_set_sky_color(baker, 0.2, 0.2, 0.2);
        rt_lightbaker3d_add_light(baker, light);
        EXPECT_TRUE(run_bake(baker), "configured lightmap completes");
        void *atlas = rt_lightbaker3d_get_atlas(baker);
        EXPECT_TRUE(atlas != nullptr, "configured lightmap publishes atlas");
        energy[mode] = atlas_channel_sum(atlas, 24);
        rt_lightbaker3d_set_include_direct(baker, mode != 0);
        EXPECT_TRUE(rt_lightbaker3d_get_include_direct(baker) == (mode == 0),
                    "gathered option cannot change");
        for (void *object : {atlas, baker, light, position, ceiling, material, floor, scene})
            release_object(object);
    }
    EXPECT_TRUE(energy[0] > 0, "default mode includes direct light");
    EXPECT_TRUE(energy[1] == 0, "zero-bounce indirect atlas is black despite live light");
    EXPECT_TRUE(energy[2] > 0, "indirect-only atlas retains sky");
    EXPECT_TRUE(energy[3] > 0, "indirect-only atlas retains fixture bounce with black sky");
    PASS();
}

bool test_direct_bake_and_apply() {
    TEST("direct-only bake lights the floor atlas, writes UV1, and applies");

    void *scene = rt_scene3d_new();
    void *floor_mesh = make_quad(8.0, 0.0, 0.0, 1.0, 0.0, 0);
    void *floor_mat = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *floor_node = add_static_node(scene, floor_mesh, floor_mat, "bake_floor");

    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_samples(baker, 8);
    rt_lightbaker3d_set_bounces(baker, 0);
    void *sun = make_sun();
    rt_lightbaker3d_add_light(baker, sun);
    EXPECT_TRUE(run_bake(baker), "bake completes");
    EXPECT_TRUE(rt_lightbaker3d_get_progress(baker) == 1.0, "progress reaches 1");

    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas != nullptr, "atlas produced");
    long long lit = atlas_channel_sum(atlas, 24);
    EXPECT_TRUE(lit > 0, "sunlit floor bakes non-black texels");

    rt_lightbaker3d_apply(baker);
    void *applied = rt_scene_node3d_get_material(floor_node);
    EXPECT_TRUE(applied != nullptr && rt_material3d_get_has_lightmap(applied),
                "Apply installs a lightmap material instance on the baked node");

    if (rt_obj_release_check0(atlas))
        rt_obj_free(atlas);
    for (void *o : {sun, baker, floor_mat, floor_mesh, scene}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    PASS();
}

bool test_mirrored_normal_and_directional_snapshot() {
    TEST("mirrored normals and directional snapshots remain finite and correctly oriented");

    void *scene = rt_scene3d_new();
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *node = add_static_node(scene, mesh, material, "mirrored_normal");
    rt_scene_node3d_set_scale(node, -1e-20, 1.0, 1.0);

    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    void *direction = rt_vec3_new(1.0, 0.0, 0.0);
    void *sun = rt_light3d_new_directional(direction, 1.0, 1.0, 1.0);
    static_cast<rt_light3d *>(sun)->position[0] = std::numeric_limits<double>::quiet_NaN();
    rt_lightbaker3d_add_light(baker, sun);
    EXPECT_TRUE(run_bake(baker), "mirrored bake completes");
    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas != nullptr && atlas_channel_sum(atlas, 24) > 0,
                "inverse-transpose orientation lights the mirrored face and ignores sun position");

    for (void *object : {atlas, sun, direction, baker, material, mesh, scene})
        release_object(object);
    PASS();
}

bool test_deep_scene_and_malformed_triangle_recovery() {
    TEST("deep/wide gather retains nodes past 512 and skips only malformed triangles");

    void *scene = rt_scene3d_new();
    void *shared_mesh = make_triangle(0.01);
    void *shared_material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *last_node = nullptr;
    for (int node_index = 0; node_index < 513; ++node_index)
        last_node = add_static_node(scene, shared_mesh, shared_material, "wide_static_node");
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    EXPECT_TRUE(run_bake(baker), "wide scene bake completes");
    void *wide_atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(wide_atlas != nullptr, "wide scene produces a complete atlas");
    release_object(wide_atlas);
    rt_lightbaker3d_apply(baker);
    void *last_material = rt_scene_node3d_get_material(last_node);
    EXPECT_TRUE(last_material && rt_material3d_get_has_lightmap(last_material),
                "node beyond the old traversal ceiling receives the atlas");
    release_object(baker);
    release_object(shared_material);
    release_object(shared_mesh);
    release_object(scene);

    scene = rt_scene3d_new();
    void *mesh = rt_mesh3d_new();
    for (int triangle = 0; triangle < 2; ++triangle) {
        double x = (double)triangle;
        rt_mesh3d_add_vertex(mesh, x, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x + 0.25, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x, 0.0, 0.25, 0.0, 1.0, 0.0, 0.0, 1.0);
        rt_mesh3d_add_triangle(mesh, triangle * 3, triangle * 3 + 1, triangle * 3 + 2);
    }
    auto *mesh_impl = static_cast<rt_mesh3d *>(mesh);
    mesh_impl->indices[0] = UINT32_MAX;
    rt_mesh3d_touch_geometry(mesh_impl);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    add_static_node(scene, mesh, material, "malformed_then_valid");
    baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    EXPECT_TRUE(run_bake(baker), "malformed mesh bake completes");
    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas != nullptr, "valid triangle after malformed input still produces an atlas");
    release_object(atlas);
    release_object(baker);
    release_object(material);
    release_object(mesh);
    release_object(scene);
    PASS();
}

bool test_incremental_snapshot_and_single_publication() {
    TEST("incremental bake bounds sample work, freezes inputs, and publishes private UV meshes");

    void *scene = rt_scene3d_new();
    void *mesh = make_unique_triangle_strip(65);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *node = add_static_node(scene, mesh, material, "incremental_strip");
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 8);
    rt_lightbaker3d_set_bounces(baker, 1);
    rt_lightbaker3d_set_sky_color(baker, 0.25, 0.25, 0.25);
    uint32_t revision_before = static_cast<rt_mesh3d *>(mesh)->geometry_revision;
    EXPECT_TRUE(rt_lightbaker3d_bake_step(baker) == 0,
                "first bounded sample-work slice is incomplete");
    EXPECT_TRUE(static_cast<rt_mesh3d *>(mesh)->geometry_revision == revision_before,
                "partial slice does not publish UV mutations");

    rt_lightbaker3d_set_texels_per_unit(baker, 32.0);
    rt_lightbaker3d_set_samples(baker, 99);
    rt_lightbaker3d_set_bounces(baker, 7);
    rt_lightbaker3d_set_include_direct(baker, 0);
    rt_lightbaker3d_set_sky_color(baker, 1.0, 0.0, 0.0);
    EXPECT_TRUE(rt_lightbaker3d_get_texels_per_unit(baker) == 1.0,
                "density is frozen after the first gather");
    EXPECT_TRUE(rt_lightbaker3d_get_samples(baker) == 8,
                "sample count is frozen after the first gather");
    EXPECT_TRUE(rt_lightbaker3d_get_bounces(baker) == 1,
                "bounce count is frozen after the first gather");
    EXPECT_TRUE(rt_lightbaker3d_get_include_direct(baker) == 1,
                "direct-light option is frozen during a partial bake");
    EXPECT_TRUE(run_bake(baker), "remaining slice completes");
    EXPECT_TRUE(static_cast<rt_mesh3d *>(mesh)->geometry_revision == revision_before,
                "published UVs never mutate the shared source mesh");
    auto *published_mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(node));
    EXPECT_TRUE(published_mesh && published_mesh != mesh,
                "publication installs a private lightmap mesh on the node");
    EXPECT_TRUE(published_mesh->vertex_count ==
                    static_cast<rt_mesh3d *>(mesh)->vertex_count + 65 * 3,
                "each baked triangle receives seam-safe private vertices");
    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas != nullptr, "incremental bake publishes an atlas");
    EXPECT_TRUE(static_cast<rt_pixels_impl *>(atlas)->generation == 1,
                "direct atlas fill advances Pixels generation once");

    rt_lightbaker3d_apply(baker);
    void *first_applied = rt_scene_node3d_get_material(node);
    rt_lightbaker3d_apply(baker);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) == first_applied,
                "repeated Apply is idempotent");
    release_object(atlas);
    release_object(baker);
    release_object(material);
    release_object(mesh);
    release_object(scene);
    PASS();
}

bool test_source_changes_reject_publication_and_apply() {
    TEST("source transform/binding changes reject stale publication and delayed Apply");

    void *scene = rt_scene3d_new();
    void *mesh = make_unique_triangle_strip(65);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *node = add_static_node(scene, mesh, material, "changing_source");
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 8);
    rt_lightbaker3d_set_bounces(baker, 0);
    uint32_t revision_before = static_cast<rt_mesh3d *>(mesh)->geometry_revision;
    EXPECT_TRUE(rt_lightbaker3d_bake_step(baker) == 0, "first source snapshot slice is incomplete");
    rt_scene_node3d_set_scale(node, 2.0, 1.0, 1.0);
    EXPECT_TRUE(run_bake(baker), "changed-source bake terminates");
    EXPECT_TRUE(rt_lightbaker3d_get_atlas(baker) == nullptr,
                "world-transform change rejects the stale atlas");
    EXPECT_TRUE(static_cast<rt_mesh3d *>(mesh)->geometry_revision == revision_before,
                "world-transform change publishes no UVs");
    for (void *object : {baker, material, mesh, scene})
        release_object(object);

    scene = rt_scene3d_new();
    mesh = make_triangle();
    material = rt_material3d_new_color(1.0, 1.0, 1.0);
    node = add_static_node(scene, mesh, material, "delayed_apply_source");
    baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    EXPECT_TRUE(run_bake(baker), "delayed-apply source bake completes");
    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas != nullptr, "delayed-apply source produces an atlas");
    rt_material3d_set_emissive_color(material, 1.0, 0.0, 0.0);
    rt_lightbaker3d_apply(baker);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) == material &&
                    !rt_material3d_get_has_lightmap(material),
                "in-place material change after baking prevents stale Apply");
    void *replacement = rt_material3d_new_color(0.25, 0.5, 0.75);
    rt_scene_node3d_set_material(node, replacement);
    rt_lightbaker3d_apply(baker);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) == replacement &&
                    !rt_material3d_get_has_lightmap(replacement),
                "material change after baking prevents stale Apply");
    for (void *object : {replacement, atlas, baker, material, mesh, scene})
        release_object(object);
    PASS();
}

bool test_lightmap_uv_seams_and_shared_instances() {
    TEST("bake UV publication preserves indexed seams and isolates shared mesh instances");

    void *scene = rt_scene3d_new();
    void *mesh = make_quad(2.0, 0.0, 0.0, 1.0, 0.0, 0);
    auto *source = static_cast<rt_mesh3d *>(mesh);
    uint32_t source_revision = source->geometry_revision;
    float source_uv1[8];
    for (uint32_t i = 0; i < source->vertex_count; ++i) {
        source_uv1[i * 2] = source->vertices[i].uv1[0];
        source_uv1[i * 2 + 1] = source->vertices[i].uv1[1];
    }
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *node_a = add_static_node(scene, mesh, material, "shared_instance_a");
    void *node_b = add_static_node(scene, mesh, material, "shared_instance_b");
    rt_scene_node3d_set_position(node_b, 3.0, 0.0, 0.0);
    void *bent_mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(bent_mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(bent_mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(bent_mesh, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(bent_mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(bent_mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(bent_mesh, 0, 3, 1);
    void *bent_node = add_static_node(scene, bent_mesh, material, "hard_seam");
    rt_scene_node3d_set_position(bent_node, -3.0, 0.0, 0.0);

    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 2.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    EXPECT_TRUE(run_bake(baker), "shared-instance bake completes");

    auto *mesh_a = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(node_a));
    auto *mesh_b = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(node_b));
    EXPECT_TRUE(mesh_a && mesh_b && mesh_a != source && mesh_b != source && mesh_a != mesh_b,
                "each shared source instance receives a distinct published mesh");
    EXPECT_TRUE(source->geometry_revision == source_revision && source->vertex_count == 4,
                "source topology and revision remain unchanged");
    for (uint32_t i = 0; i < source->vertex_count; ++i)
        EXPECT_TRUE(source->vertices[i].uv1[0] == source_uv1[i * 2] &&
                        source->vertices[i].uv1[1] == source_uv1[i * 2 + 1],
                    "source UV1 data remains byte-for-byte equivalent");
    EXPECT_TRUE(mesh_a->vertex_count == 8 && mesh_a->index_count == 6,
                "coplanar quad publication appends one four-vertex UV island");
    EXPECT_TRUE(mesh_a->indices[0] == mesh_a->indices[3] &&
                    mesh_a->indices[2] == mesh_a->indices[4],
                "connected coplanar triangles retain shared island vertices");
    EXPECT_TRUE(mesh_a->vertices[mesh_a->indices[0]].uv1[0] ==
                        mesh_a->vertices[mesh_a->indices[3]].uv1[0] &&
                    mesh_a->vertices[mesh_a->indices[0]].uv1[1] ==
                        mesh_a->vertices[mesh_a->indices[3]].uv1[1],
                "shared island corners publish one stable UV coordinate");
    auto *bent_published = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(bent_node));
    EXPECT_TRUE(bent_published && bent_published != bent_mesh && bent_published->vertex_count == 10,
                "non-coplanar triangles append independent hard-seam vertices");
    EXPECT_TRUE(bent_published->indices[0] != bent_published->indices[3] &&
                    bent_published->indices[1] != bent_published->indices[5],
                "hard-edge source vertices are split across UV islands");

    for (void *object : {baker, material, bent_mesh, mesh, scene})
        release_object(object);
    PASS();
}

bool test_atlas_capacity_failure_is_atomic() {
    TEST("atlas-capacity preflight rejects the whole bake without touching UVs");

    void *scene = rt_scene3d_new();
    void *mesh = rt_mesh3d_new();
    for (int triangle = 0; triangle < 50; ++triangle) {
        int64_t first = (int64_t)triangle * 3;
        rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, 126.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 126.0, 0.0, 1.0, 0.0, 0.0, 1.0);
        rt_mesh3d_add_triangle(mesh, first, first + 1, first + 2);
    }
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    add_static_node(scene, mesh, material, "overfull_atlas");
    uint32_t revision_before = static_cast<rt_mesh3d *>(mesh)->geometry_revision;
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    EXPECT_TRUE(rt_lightbaker3d_bake_step(baker) == 1, "overfull bake terminates");
    EXPECT_TRUE(rt_lightbaker3d_get_atlas(baker) == nullptr,
                "overfull bake never publishes a partial atlas");
    EXPECT_TRUE(static_cast<rt_mesh3d *>(mesh)->geometry_revision == revision_before,
                "overfull bake does not publish any UV write");
    release_object(baker);
    release_object(material);
    release_object(mesh);
    release_object(scene);
    PASS();
}

bool test_punctual_light_semantics() {
    TEST("baked local lights honor decay, range, spot cones, and shadow flags");

    void *position = rt_vec3_new(0.0, 10.0, 0.0);
    void *point_none = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 1.0);
    rt_light3d_set_decay_type(point_none, 0);
    long long no_decay = bake_floor_with_light(point_none, false);
    release_object(point_none);

    void *point_cubic = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 1.0);
    rt_light3d_set_decay_type(point_cubic, 3);
    long long cubic_decay = bake_floor_with_light(point_cubic, false);
    release_object(point_cubic);

    void *away = rt_vec3_new(0.0, 1.0, 0.0);
    void *spot = rt_light3d_new_spot(position, away, 1.0, 1.0, 1.0, 1.0, 10.0, 20.0);
    long long outside_spot = bake_floor_with_light(spot, false);
    release_object(spot);

    void *ranged_point = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 1.0);
    rt_light3d_set_range(ranged_point, 1.0);
    EXPECT_TRUE(bake_floor_with_light(ranged_point, false) == 0,
                "authored point cutoff reaches baker through public property");
    rt_light3d_set_range(ranged_point, 0.0);
    EXPECT_TRUE(bake_floor_with_light(ranged_point, false) > 0,
                "zero point range restores unbounded baked attenuation");
    release_object(ranged_point);

    void *area = rt_light3d_new_area_sphere(position, 0.5, 1.0, 1.0, 1.0, 1.0);
    long long outside_range = bake_floor_with_light(area, false);
    release_object(area);
    release_object(away);
    release_object(position);

    EXPECT_TRUE(no_decay > cubic_decay, "cubic decay is dimmer than no decay at distance");
    EXPECT_TRUE(outside_spot == 0, "spot light contributes nothing outside its outer cone");
    EXPECT_TRUE(outside_range == 0, "finite-range light contributes nothing past its range");

    position = rt_vec3_new(0.0, 2.0, 0.0);
    void *unshadowed_light = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 1.0);
    rt_light3d_set_decay_type(unshadowed_light, 0);
    rt_light3d_set_casts_shadows(unshadowed_light, 0);
    long long unshadowed = bake_floor_with_light(unshadowed_light, true);
    release_object(unshadowed_light);
    void *shadowed_light = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 1.0);
    rt_light3d_set_decay_type(shadowed_light, 0);
    rt_light3d_set_casts_shadows(shadowed_light, 1);
    long long shadowed = bake_floor_with_light(shadowed_light, true);
    release_object(shadowed_light);
    release_object(position);
    EXPECT_TRUE(unshadowed > shadowed, "disabling shadows preserves occluded direct light");
    PASS();
}

bool test_area_volume_and_dynamic_light_storage() {
    TEST("rectangle, sphere, volume, and lights beyond the old sixteen-light cap bake correctly");

    void *position = rt_vec3_new(0.0, 2.0, 0.0);
    void *down = rt_vec3_new(0.0, -1.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *rectangle_down =
        rt_light3d_new_area_rectangle(position, down, 2.0, 2.0, 1.0, 1.0, 1.0, 0.0, 8.0);
    void *rectangle_up =
        rt_light3d_new_area_rectangle(position, up, 2.0, 2.0, 1.0, 1.0, 1.0, 0.0, 8.0);
    void *sphere = rt_light3d_new_area_sphere(position, 0.75, 1.0, 1.0, 1.0, 8.0);
    void *volume = rt_light3d_new_volume(position, 3.0, 1.0, 1.0, 1.0, 8.0);
    long long rectangle_front = bake_floor_with_light(rectangle_down, false);
    long long rectangle_back = bake_floor_with_light(rectangle_up, false);
    long long sphere_sum = bake_floor_with_light(sphere, false);
    long long volume_sum = bake_floor_with_light(volume, false);
    EXPECT_TRUE(rectangle_front > 0 && rectangle_back == 0,
                "rectangle bake respects finite area and one-sided emission");
    EXPECT_TRUE(sphere_sum > 0, "sphere area light contributes finite irradiance");
    EXPECT_TRUE(volume_sum > 0, "points inside a volume light receive isotropic irradiance");
    for (void *object : {volume, sphere, rectangle_up, rectangle_down, up, down, position})
        release_object(object);

    void *scene = rt_scene3d_new();
    void *mesh = make_quad(2.0, 0.0, 0.0, 1.0, 0.0, 0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    add_static_node(scene, mesh, material, "many_lights_floor");
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_texels_per_unit(baker, 1.0);
    rt_lightbaker3d_set_samples(baker, 1);
    rt_lightbaker3d_set_bounces(baker, 0);
    position = rt_vec3_new(0.0, 2.0, 0.0);
    void *lights[17] = {};
    for (int index = 0; index < 17; ++index) {
        lights[index] = rt_light3d_new_point(position, 0.0, index == 16 ? 1.0 : 0.0, 0.0, 0.0);
        rt_light3d_set_decay_type(lights[index], 0);
        rt_lightbaker3d_add_light(baker, lights[index]);
    }
    EXPECT_TRUE(run_bake(baker), "bake with more than sixteen lights completes");
    void *atlas = rt_lightbaker3d_get_atlas(baker);
    EXPECT_TRUE(atlas && atlas_channel_sum(atlas, 16) > 0,
                "the seventeenth light contributes instead of being silently dropped");

    release_object(atlas);
    for (void *light : lights)
        release_object(light);
    for (void *object : {position, baker, material, mesh, scene})
        release_object(object);
    PASS();
}

bool test_bounce_color_bleed_and_determinism() {
    TEST("red wall bleeds into bounced floor light; bakes are deterministic");

    long long red_sum[2] = {0, 0};
    long long green_sum[2] = {0, 0};
    long long first_hash = 0;
    for (int variant = 0; variant < 2; ++variant) {
        void *scene = rt_scene3d_new();
        void *floor_mesh = make_quad(6.0, 0.0, 0.0, 1.0, 0.0, 0);
        void *floor_mat = rt_material3d_new_color(1.0, 1.0, 1.0);
        add_static_node(scene, floor_mesh, floor_mat, "floor");
        void *wall_mesh = make_quad(6.0, 2.9, -1.0, 0.0, 0.0, 1);
        void *wall_mat = variant == 0 ? rt_material3d_new_color(1.0, 0.05, 0.05)
                                      : rt_material3d_new_color(1.0, 1.0, 1.0);
        add_static_node(scene, wall_mesh, wall_mat, "wall");

        void *baker = rt_lightbaker3d_new(scene);
        rt_lightbaker3d_set_samples(baker, 16);
        rt_lightbaker3d_set_bounces(baker, 2);
        void *sun = make_sun();
        rt_lightbaker3d_add_light(baker, sun);
        EXPECT_TRUE(run_bake(baker), "bake completes");
        void *atlas = rt_lightbaker3d_get_atlas(baker);
        EXPECT_TRUE(atlas != nullptr, "atlas produced");
        red_sum[variant] = atlas_channel_sum(atlas, 24);
        green_sum[variant] = atlas_channel_sum(atlas, 16);

        if (variant == 0) {
            /* Determinism: re-bake the identical scene and compare a channel hash. */
            void *baker2 = rt_lightbaker3d_new(scene);
            rt_lightbaker3d_set_samples(baker2, 16);
            rt_lightbaker3d_set_bounces(baker2, 2);
            void *sun2 = make_sun();
            rt_lightbaker3d_add_light(baker2, sun2);
            EXPECT_TRUE(run_bake(baker2), "second bake completes");
            void *atlas2 = rt_lightbaker3d_get_atlas(baker2);
            EXPECT_TRUE(atlas2 != nullptr, "second atlas produced");
            long long h1 = atlas_channel_sum(atlas, 24) * 131 + atlas_channel_sum(atlas, 16);
            long long h2 = atlas_channel_sum(atlas2, 24) * 131 + atlas_channel_sum(atlas2, 16);
            first_hash = h1;
            EXPECT_TRUE(h1 == h2, "identical scene + options bake identically");
            if (rt_obj_release_check0(atlas2))
                rt_obj_free(atlas2);
            for (void *o : {sun2, baker2}) {
                if (rt_obj_release_check0(o))
                    rt_obj_free(o);
            }
        }
        if (rt_obj_release_check0(atlas))
            rt_obj_free(atlas);
        for (void *o : {sun, baker, wall_mat, wall_mesh, floor_mat, floor_mesh, scene}) {
            if (rt_obj_release_check0(o))
                rt_obj_free(o);
        }
    }
    (void)first_hash;
    /* Red wall variant must skew red vs the white wall variant. */
    double ratio_red = (double)red_sum[0] / (double)(green_sum[0] + 1);
    double ratio_white = (double)red_sum[1] / (double)(green_sum[1] + 1);
    EXPECT_TRUE(ratio_red > ratio_white * 1.02, "red wall bounce skews the floor bake toward red");
    PASS();
}

bool test_probe_validation_empty_scene_and_sampling_guards() {
    TEST("probe grids validate bounds/counts and sample empty scenes safely");

    void *minimum = rt_vec3_new(0.0, 0.0, 0.0);
    void *maximum = rt_vec3_new(4.0, 4.0, 4.0);
    void *grid = rt_lightprobegrid3d_new(minimum, maximum, 2.0);
    EXPECT_TRUE(grid != nullptr, "ordered finite grid allocates");
    EXPECT_TRUE(rt_lightprobegrid3d_get_probe_count(grid) == 27,
                "exact-multiple extent uses ceil(intervals)+1 without an extra plane");

    void *position = rt_vec3_new(1.0, 1.0, 1.0);
    void *zero = rt_vec3_new(0.0, 0.0, 0.0);
    void *unbaked = rt_lightprobegrid3d_sample(grid, position, zero);
    EXPECT_TRUE(rt_vec3_x(unbaked) == 0.0 && rt_vec3_y(unbaked) == 0.0 && rt_vec3_z(unbaked) == 0.0,
                "unbaked grid samples black");

    void *scene = rt_scene3d_new();
    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_samples(baker, 8);
    rt_lightbaker3d_set_bounces(baker, 1);
    rt_lightbaker3d_set_sky_color(baker, 0.5, 0.5, 0.5);
    rt_lightprobegrid3d_bake(grid, baker);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *zero_sample = rt_lightprobegrid3d_sample(grid, position, zero);
    void *up_sample = rt_lightprobegrid3d_sample(grid, position, up);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(zero_sample)) && rt_vec3_x(zero_sample) > 0.0,
                "empty scene produces finite nonblack sky probes on the first bake");
    EXPECT_TRUE(rt_vec3_x(zero_sample) == rt_vec3_x(up_sample) &&
                    rt_vec3_y(zero_sample) == rt_vec3_y(up_sample) &&
                    rt_vec3_z(zero_sample) == rt_vec3_z(up_sample),
                "zero normal uses the documented positive-Y fallback");

    void *nan_position = rt_vec3_new(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
    void *nan_sample = rt_lightprobegrid3d_sample(grid, nan_position, up);
    EXPECT_TRUE(rt_vec3_x(nan_sample) == 0.0 && rt_vec3_y(nan_sample) == 0.0 &&
                    rt_vec3_z(nan_sample) == 0.0,
                "nonfinite sample position returns black");
    void *huge_position = rt_vec3_new(DBL_MAX, DBL_MAX, DBL_MAX);
    void *huge_sample = rt_lightprobegrid3d_sample(grid, huge_position, up);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(huge_sample)) && std::isfinite(rt_vec3_y(huge_sample)) &&
                    std::isfinite(rt_vec3_z(huge_sample)),
                "finite extreme position clamps without undefined integer conversion");

    void *inverted_min = rt_vec3_new(2.0, 0.0, 0.0);
    void *inverted_max = rt_vec3_new(1.0, 1.0, 1.0);
    EXPECT_TRUE(
        expect_trap("LightProbeGrid3D.New: bounds must be finite and ordered",
                    [&]() { (void)rt_lightprobegrid3d_new(inverted_min, inverted_max, 1.0); }),
        "inverted bounds trap with the finite/order diagnostic");
    void *nan_bound = rt_vec3_new(std::numeric_limits<double>::quiet_NaN(), 1.0, 1.0);
    EXPECT_TRUE(expect_trap("LightProbeGrid3D.New: bounds must be finite and ordered",
                            [&]() { (void)rt_lightprobegrid3d_new(minimum, nan_bound, 1.0); }),
                "nonfinite bounds trap before any integer narrowing");

    void *fresh_grid = rt_lightprobegrid3d_new(minimum, maximum, 2.0);
    EXPECT_TRUE(
        rt_lightprobegrid3d_save(fresh_grid, rt_const_cstr("zanna_unbaked_probe_grid.vlpg")) == 0,
        "unbaked grids are not serialized");
    for (void *object : {fresh_grid,
                         nan_bound,
                         inverted_min,
                         inverted_max,
                         huge_sample,
                         huge_position,
                         nan_sample,
                         nan_position,
                         zero_sample,
                         up_sample,
                         up,
                         baker,
                         scene,
                         unbaked,
                         zero,
                         position,
                         grid,
                         minimum,
                         maximum})
        release_object(object);
    PASS();
}

bool test_probe_grid_sampling_and_roundtrip() {
    TEST("probe grid samples brighter near light, round-trips via .vlpg");

    const char *canonical_path = "zanna_probe_grid.vlpg";
    const char *truncated_path = "zanna_probe_grid_truncated.vlpg";
    const char *trailing_path = "zanna_probe_grid_trailing.vlpg";
    const char *validity_path = "zanna_probe_grid_bad_validity.vlpg";
    const char *nan_path = "zanna_probe_grid_nan_coefficient.vlpg";

    /* Open scene: a bright sky on one half via an emissive wall at x=+4. */
    void *scene = rt_scene3d_new();
    void *floor_mesh = make_quad(16.0, 0.0, 0.0, 1.0, 0.0, 0);
    void *floor_mat = rt_material3d_new_color(0.8, 0.8, 0.8);
    add_static_node(scene, floor_mesh, floor_mat, "floor");

    void *baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_samples(baker, 32);
    rt_lightbaker3d_set_bounces(baker, 1);
    rt_lightbaker3d_set_sky_color(baker, 0.6, 0.6, 0.6);
    /* Point light near x = +5 lights the positive-X side. */
    void *lp = rt_vec3_new(5.0, 2.0, 0.0);
    void *point = rt_light3d_new_point(lp, 1.0, 0.9, 0.7, 0.05);
    if (rt_obj_release_check0(lp))
        rt_obj_free(lp);
    rt_lightbaker3d_add_light(baker, point);

    void *gmin = rt_vec3_new(-6.0, 0.5, -1.0);
    void *gmax = rt_vec3_new(6.0, 2.5, 1.0);
    void *grid = rt_lightprobegrid3d_new(gmin, gmax, 2.0);
    EXPECT_TRUE(grid != nullptr, "grid allocates");
    EXPECT_TRUE(rt_lightprobegrid3d_get_probe_count(grid) > 0, "grid has probes");
    rt_lightprobegrid3d_bake(grid, baker);

    void *near_pos = rt_vec3_new(5.0, 1.5, 0.0);
    void *far_pos = rt_vec3_new(-5.0, 1.5, 0.0);
    /* Down-facing normal: the probe signal here is floor-bounced light, which
     * carries the point light's attenuation gradient (an up normal would read
     * the uniform sky instead). */
    void *up = rt_vec3_new(0.0, -1.0, 0.0);
    void *near_s = rt_lightprobegrid3d_sample(grid, near_pos, up);
    void *far_s = rt_lightprobegrid3d_sample(grid, far_pos, up);
    double nr = rt_vec3_x(near_s), fr = rt_vec3_x(far_s);
    EXPECT_TRUE(nr > fr, "probe irradiance is brighter near the light");

    // IncludeDirect controls lightmap primaries, never the lit surfaces seen
    // by probe rays. Removing that contribution would erase fixture bounce.
    void *indirect_baker = rt_lightbaker3d_new(scene);
    rt_lightbaker3d_set_samples(indirect_baker, 32);
    rt_lightbaker3d_set_bounces(indirect_baker, 1);
    rt_lightbaker3d_set_sky_color(indirect_baker, 0.6, 0.6, 0.6);
    rt_lightbaker3d_set_include_direct(indirect_baker, 0);
    rt_lightbaker3d_add_light(indirect_baker, point);
    void *indirect_grid = rt_lightprobegrid3d_new(gmin, gmax, 2.0);
    rt_lightprobegrid3d_bake(indirect_grid, indirect_baker);
    void *indirect_near = rt_lightprobegrid3d_sample(indirect_grid, near_pos, up);
    void *indirect_far = rt_lightprobegrid3d_sample(indirect_grid, far_pos, up);
    EXPECT_TRUE(rt_vec3_x(indirect_near) == nr && rt_vec3_x(indirect_far) == fr,
                "lightmap option leaves probe fixture irradiance unchanged");
    for (void *object : {indirect_far, indirect_near, indirect_grid, indirect_baker})
        release_object(object);

    EXPECT_TRUE(rt_lightprobegrid3d_save(grid, rt_const_cstr(canonical_path)) == 1, "grid saves");
    unsigned char canonical_header[52] = {0};
    FILE *canonical = std::fopen(canonical_path, "rb");
    EXPECT_TRUE(canonical != nullptr &&
                    std::fread(canonical_header, 1, sizeof(canonical_header), canonical) ==
                        sizeof(canonical_header),
                "canonical VLPG header reads");
    if (canonical)
        std::fclose(canonical);
    EXPECT_TRUE(canonical_header[40] == 7 && canonical_header[41] == 0 &&
                    canonical_header[44] == 2 && canonical_header[45] == 0 &&
                    canonical_header[48] == 2 && canonical_header[49] == 0,
                "VLPG dimensions are fixed-width little-endian values");
    void *gmin2 = rt_vec3_new(0.0, 0.0, 0.0);
    void *gmax2 = rt_vec3_new(1.0, 1.0, 1.0);
    void *grid2 = rt_lightprobegrid3d_new(gmin2, gmax2, 1.0);
    EXPECT_TRUE(rt_lightprobegrid3d_load(grid2, rt_const_cstr(canonical_path)) == 1, "grid loads");
    void *near_s2 = rt_lightprobegrid3d_sample(grid2, near_pos, up);
    EXPECT_TRUE(rt_vec3_x(near_s2) == nr, "loaded grid samples identically");

    FILE *truncated = std::fopen(truncated_path, "wb");
    EXPECT_TRUE(truncated != nullptr, "truncated-grid fixture opens");
    EXPECT_TRUE(std::fwrite("VLPG0001", 1, 8, truncated) == 8, "truncated-grid header writes");
    std::fclose(truncated);
    EXPECT_TRUE(rt_lightprobegrid3d_load(grid2, rt_const_cstr(truncated_path)) == 0,
                "truncated grid is rejected");

    EXPECT_TRUE(copy_file(canonical_path, trailing_path), "trailing-byte fixture copies");
    FILE *trailing = std::fopen(trailing_path, "ab");
    unsigned char extra = 0xA5;
    EXPECT_TRUE(trailing && std::fwrite(&extra, 1, 1, trailing) == 1, "trailing byte appends");
    if (trailing)
        std::fclose(trailing);
    EXPECT_TRUE(rt_lightprobegrid3d_load(grid2, rt_const_cstr(trailing_path)) == 0,
                "trailing VLPG payload is rejected");

    EXPECT_TRUE(copy_file(canonical_path, validity_path), "validity fixture copies");
    const unsigned char noncanonical_validity = 2;
    EXPECT_TRUE(overwrite_bytes(validity_path, 52, &noncanonical_validity, 1),
                "noncanonical validity byte writes");
    EXPECT_TRUE(rt_lightprobegrid3d_load(grid2, rt_const_cstr(validity_path)) == 0,
                "noncanonical validity byte is rejected");

    EXPECT_TRUE(copy_file(canonical_path, nan_path), "nonfinite coefficient fixture copies");
    const unsigned char little_endian_nan[4] = {0x00, 0x00, 0xC0, 0x7F};
    long coefficient_offset = 52 + (long)rt_lightprobegrid3d_get_probe_count(grid);
    EXPECT_TRUE(overwrite_bytes(nan_path, coefficient_offset, little_endian_nan, 4),
                "nonfinite coefficient writes");
    EXPECT_TRUE(rt_lightprobegrid3d_load(grid2, rt_const_cstr(nan_path)) == 0,
                "nonfinite SH coefficient is rejected");

    void *near_s3 = rt_lightprobegrid3d_sample(grid2, near_pos, up);
    EXPECT_TRUE(rt_vec3_x(near_s3) == nr,
                "all rejected loads preserve the previous grid transactionally");
    std::remove(truncated_path);
    std::remove(trailing_path);
    std::remove(validity_path);
    std::remove(nan_path);
    std::remove(canonical_path);

    for (void *o : {near_s,
                    far_s,
                    near_s2,
                    near_s3,
                    near_pos,
                    far_pos,
                    up,
                    gmin,
                    gmax,
                    gmin2,
                    gmax2,
                    grid,
                    grid2,
                    point,
                    baker,
                    floor_mat,
                    floor_mesh,
                    scene}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    PASS();
}

bool test_probe_bake_rejects_non_baker_argument() {
    TEST("probe bake traps a wrong-class or null baker before touching the grid");

    void *minimum = rt_vec3_new(0.0, 0.0, 0.0);
    void *maximum = rt_vec3_new(4.0, 4.0, 4.0);
    void *grid = rt_lightprobegrid3d_new(minimum, maximum, 2.0);
    EXPECT_TRUE(grid != nullptr, "grid allocates");
    void *scene = rt_scene3d_new();
    EXPECT_TRUE(scene != nullptr, "scene allocates");

    EXPECT_TRUE(expect_trap("LightBaker3D.set_IncludeDirect: invalid baker",
                            [&] { rt_lightbaker3d_set_include_direct(scene, 0); }),
                "direct-light setter rejects wrong-class receivers");
    EXPECT_TRUE(expect_trap("LightBaker3D.get_IncludeDirect: invalid baker",
                            [&] { (void)rt_lightbaker3d_get_include_direct(nullptr); }),
                "direct-light getter rejects null receivers");

    /* The shipped mistake: the scene itself where the baker belongs (the
     * neighboring ReflectionProbe3D.Capture signature does take the scene,
     * and obj<T> parameters are not enforced at Zia call sites). */
    EXPECT_TRUE(expect_trap("LightProbeGrid3D.Bake: invalid baker",
                            [&] { rt_lightprobegrid3d_bake(grid, scene); }),
                "a SceneGraph passed as the baker traps by class id");
    EXPECT_TRUE(expect_trap("LightProbeGrid3D.Bake: invalid baker",
                            [&] { rt_lightprobegrid3d_bake(grid, nullptr); }),
                "a null baker traps");
    EXPECT_TRUE(expect_trap("LightProbeGrid3D.Bake: invalid grid",
                            [&] { rt_lightprobegrid3d_bake(scene, nullptr); }),
                "the receiver grid is validated before the baker");

    /* The rejected calls leave the grid intact: a real baker still bakes. */
    void *baker = rt_lightbaker3d_new(scene);
    EXPECT_TRUE(baker != nullptr, "baker allocates");
    rt_lightbaker3d_set_samples(baker, 8);
    rt_lightbaker3d_set_sky_color(baker, 0.5, 0.5, 0.5);
    rt_lightprobegrid3d_bake(grid, baker);
    void *position = rt_vec3_new(1.0, 1.0, 1.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *sample = rt_lightprobegrid3d_sample(grid, position, up);
    EXPECT_TRUE(rt_vec3_x(sample) > 0.0, "the same grid bakes sky light after the rejected calls");

    for (void *o : {sample, up, position, baker, scene, grid, maximum, minimum})
        release_object(o);
    PASS();
}

} // namespace

int main() {
    std::printf("LightBaker3D + LightProbeGrid3D tests\n");
    bool ok = true;
    ok &= test_indirect_only_lightmaps();
    ok &= test_direct_bake_and_apply();
    ok &= test_mirrored_normal_and_directional_snapshot();
    ok &= test_deep_scene_and_malformed_triangle_recovery();
    ok &= test_incremental_snapshot_and_single_publication();
    ok &= test_source_changes_reject_publication_and_apply();
    ok &= test_lightmap_uv_seams_and_shared_instances();
    ok &= test_atlas_capacity_failure_is_atomic();
    ok &= test_punctual_light_semantics();
    ok &= test_area_volume_and_dynamic_light_storage();
    ok &= test_bounce_color_bleed_and_determinism();
    ok &= test_probe_validation_empty_scene_and_sampling_guards();
    ok &= test_probe_bake_rejects_non_baker_argument();
    ok &= test_probe_grid_sampling_and_roundtrip();
    std::printf("\nBaked GI tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
