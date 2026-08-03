//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_hlod.cpp
// Purpose: Unit tests for cell-level HLOD — proxy bake (merge/simplify/atlas +
//   VSCN round-trip), the proxy residency ring (no-gap swaps in both travel
//   directions, blocking and async), telemetry split, and multi-frame impostor
//   install/automation.
// Key invariants:
//   - A cell with a proxy is never absent from the scene inside the proxy ring:
//     either the full subtree or the proxy subtree is attached every update.
//   - Proxy bake output respects the triangle budget and loads back cleanly.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/rt_game3d_streaming.inc (HLOD proxy rings)
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_game3d.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_vec3.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

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

/// Build a dense grid mesh (`segments`^2 * 2 triangles) so the bake must simplify.
void *make_grid_mesh(int segments, double size, double height) {
    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return nullptr;
    const int verts_per_side = segments + 1;
    for (int z = 0; z < verts_per_side; ++z) {
        for (int x = 0; x < verts_per_side; ++x) {
            double fx = ((double)x / segments - 0.5) * size;
            double fz = ((double)z / segments - 0.5) * size;
            rt_mesh3d_add_vertex(
                mesh, fx, height, fz, 0.0, 1.0, 0.0, (double)x / segments, (double)z / segments);
        }
    }
    for (int z = 0; z < segments; ++z) {
        for (int x = 0; x < segments; ++x) {
            int64_t a = (int64_t)z * verts_per_side + x;
            int64_t b = a + 1;
            int64_t c = a + verts_per_side;
            int64_t d = c + 1;
            rt_mesh3d_add_triangle(mesh, a, b, c);
            rt_mesh3d_add_triangle(mesh, b, d, c);
        }
    }
    return mesh;
}

bool write_dense_cell_scene(const char *path, const char *marker_name) {
    void *scene = rt_scene3d_new();
    void *node_a = rt_scene_node3d_new();
    void *node_b = rt_scene_node3d_new();
    void *mesh_a = make_grid_mesh(40, 8.0, 0.0); /* 3200 tris */
    void *mesh_b = make_grid_mesh(30, 6.0, 2.0); /* 1800 tris */
    void *mat_a = rt_material3d_new_color(0.8, 0.2, 0.2);
    void *mat_b = rt_material3d_new_color(0.2, 0.2, 0.8);
    if (!scene || !node_a || !node_b || !mesh_a || !mesh_b || !mat_a || !mat_b)
        return false;
    rt_scene_node3d_set_name(node_a, rt_const_cstr(marker_name));
    rt_scene_node3d_set_mesh(node_a, mesh_a);
    rt_scene_node3d_set_material(node_a, mat_a);
    rt_scene_node3d_set_mesh(node_b, mesh_b);
    rt_scene_node3d_set_material(node_b, mat_b);
    rt_scene3d_add(scene, node_a);
    rt_scene3d_add(scene, node_b);
    bool ok = rt_scene3d_save(scene, rt_const_cstr(path)) == 1;
    for (void *o : {node_a, node_b, mesh_a, mesh_b, mat_a, mat_b, scene}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    return ok;
}

void set_center(void *stream, double x, double z) {
    void *v = rt_vec3_new(x, 0.0, z);
    rt_game3d_world_stream_set_center(stream, v);
    if (rt_obj_release_check0(v))
        rt_obj_free(v);
}

//=========================================================================
// Proxy bake
//=========================================================================

bool test_bake_cell_proxy() {
    TEST("bakeCellProxy merges, simplifies to budget, and round-trips");

    const char *cell_path = "/tmp/zanna_g3d_hlod_cell.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_hlod_bake_cells.json";
    std::remove("/tmp/zanna_g3d_hlod_cell_proxy.vscn");
    EXPECT_TRUE(write_dense_cell_scene(cell_path, "hlod_cell_marker"), "dense fixture saves");

    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"town\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("HLOD Bake"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream, 0);
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 1, "cell loads for baking");

    EXPECT_EQ_INT(rt_game3d_world_stream_bake_cell_proxy(stream, 0), 1, "bake succeeds");
    rt_string proxy_path = rt_game3d_world_stream_get_cell_proxy(stream, 0);
    EXPECT_TRUE(proxy_path && rt_str_len(proxy_path) > 0, "bake records the proxy path");

    void *proxy_scene = rt_scene3d_load(proxy_path);
    EXPECT_TRUE(proxy_scene != nullptr, "baked proxy .vscn loads back");
    void *root = rt_scene3d_get_root(proxy_scene);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(root), 1, "proxy scene holds one merged node");
    void *proxy_node = rt_scene_node3d_get_child(root, 0);
    void *proxy_mesh = rt_scene_node3d_get_mesh(proxy_node);
    EXPECT_TRUE(proxy_mesh != nullptr, "proxy node carries the merged mesh");
    long long tris = (long long)rt_mesh3d_get_triangle_count(proxy_mesh);
    EXPECT_TRUE(tris > 0 && tris <= 800, "proxy triangle count respects the 800 budget");
    EXPECT_TRUE(rt_scene_node3d_get_material(proxy_node) != nullptr,
                "proxy node carries the atlas material");

    rt_string_unref(proxy_path);
    if (rt_obj_release_check0(proxy_scene))
        rt_obj_free(proxy_scene);
    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Proxy residency ring
//=========================================================================

bool test_proxy_ring_blocking() {
    TEST("proxy ring: Unloaded -> ProxyResident -> Resident with no gap (blocking)");

    const char *cell_path = "/tmp/zanna_g3d_hlod_ring_cell.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_hlod_ring_cells.json";
    std::remove("/tmp/zanna_g3d_hlod_ring_cell_proxy.vscn");
    EXPECT_TRUE(write_dense_cell_scene(cell_path, "ring_cell_marker"), "fixture saves");

    /* Bake a proxy first (separate world), then mount a manifest that references it. */
    {
        char bake_manifest[512];
        std::snprintf(bake_manifest,
                      sizeof(bake_manifest),
                      "{\"cells\":[{\"name\":\"ring\",\"path\":\"%s\",\"center\":[0,0,0],"
                      "\"radius\":8,\"bytes\":65536}]}",
                      cell_path);
        EXPECT_TRUE(write_text_file(manifest_path, bake_manifest), "bake manifest writes");
        void *bake_world = rt_game3d_world_new(rt_const_cstr("HLOD Ring Bake"), 80, 60);
        void *bake_stream = rt_game3d_world_get_stream(bake_world);
        rt_game3d_world_stream_set_async_streaming(bake_stream, 0);
        set_center(bake_stream, 0.0, 0.0);
        rt_game3d_world_stream_set_radii(bake_stream, 64.0, 96.0);
        rt_game3d_world_stream_mount_cells(bake_stream, rt_const_cstr(manifest_path));
        rt_game3d_world_stream_update(bake_stream, 1.0 / 60.0);
        EXPECT_EQ_INT(rt_game3d_world_stream_bake_cell_proxy(bake_stream, 0), 1, "ring bake ok");
        rt_game3d_world_destroy(bake_world);
        if (rt_obj_release_check0(bake_world))
            rt_obj_free(bake_world);
    }

    char manifest[768];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"ring\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536,"
                  "\"proxy\":\"/tmp/zanna_g3d_hlod_ring_cell_proxy.vscn\","
                  "\"proxyBytes\":16384}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "proxy manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("HLOD Ring"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream, 0);
    /* Load radius 64 -> proxy ring defaults to 4x = 256. */
    set_center(stream, 1000.0, 0.0); /* far outside everything */
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_proxy_resident_count(stream),
                  0,
                  "outside the proxy ring nothing is attached");

    set_center(stream, 200.0, 0.0); /* inside proxy ring, outside load radius */
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_proxy_resident_count(stream), 1, "proxy ring loads the proxy");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "cell itself stays unloaded in the proxy ring");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ring_proxy")) != nullptr,
                "proxy subtree is attached");
    EXPECT_TRUE(rt_game3d_world_stream_get_proxy_resident_bytes(stream) > 0,
                "proxy bytes are measured");

    /* Approach: full residency takes over; proxy releases after the commit pass. */
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 1, "full cell becomes resident");
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_proxy_resident_count(stream),
                  0,
                  "proxy releases once the full cell is committed");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ring_cell_marker")) != nullptr,
                "full subtree attached after the swap");

    /* Recede: full -> proxy with no gap; every update must have one of the two. */
    set_center(stream, 200.0, 0.0);
    for (int i = 0; i < 4; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        bool full = rt_game3d_world_stream_get_resident_cell_count(stream) == 1;
        bool proxy = rt_game3d_world_stream_get_proxy_resident_count(stream) == 1;
        EXPECT_TRUE(full || proxy, "receding swap never leaves a gap frame");
    }
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "full cell unloads after receding");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_proxy_resident_count(stream),
                  1,
                  "proxy holds the ring after receding");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

bool test_proxy_ring_async() {
    TEST("proxy ring stages and swaps under worker-backed streaming");

    const char *cell_path = "/tmp/zanna_g3d_hlod_ring_cell.vscn"; /* reuse prior fixtures */
    const char *manifest_path = "/tmp/zanna_g3d_hlod_ring_cells.json";
    char manifest[768];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"ringa\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536,"
                  "\"proxy\":\"/tmp/zanna_g3d_hlod_ring_cell_proxy.vscn\","
                  "\"proxyBytes\":16384}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("HLOD Ring Async"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    set_center(stream, 200.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    for (int i = 0; i < 400; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        if (rt_game3d_world_stream_get_proxy_resident_count(stream) == 1)
            break;
        rt_sleep_ms(1);
    }
    EXPECT_EQ_INT(rt_game3d_world_stream_get_proxy_resident_count(stream),
                  1,
                  "async proxy ring stages and commits the proxy");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 0, "full cell stays unloaded");

    set_center(stream, 0.0, 0.0);
    for (int i = 0; i < 400; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        bool full = rt_game3d_world_stream_get_resident_cell_count(stream) == 1;
        bool proxy = rt_game3d_world_stream_get_proxy_resident_count(stream) == 1;
        EXPECT_TRUE(full || proxy, "async approach never leaves a gap frame");
        if (full)
            break;
        rt_sleep_ms(1);
    }
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 1, "full cell commits async");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Multi-frame impostors
//=========================================================================

bool test_impostor_frames_install() {
    TEST("SetImpostorFrames installs a yaw strip and clears cleanly");

    void *node = rt_scene_node3d_new();
    void *mesh = make_grid_mesh(2, 2.0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    void *strip = rt_pixels_new(8 * 32, 32);
    EXPECT_TRUE(strip != nullptr, "strip pixels allocate");

    rt_scene_node3d_set_impostor_frames(node, 50.0, strip, 8);
    EXPECT_EQ_INT(rt_scene_node3d_get_impostor_frame_index(node), 0, "initial frame index is 0");

    /* Frames < 2 falls back to single-frame semantics. */
    rt_scene_node3d_set_impostor_frames(node, 50.0, strip, 1);
    EXPECT_EQ_INT(
        rt_scene_node3d_get_impostor_frame_index(node), 0, "single-frame fallback resets index");

    /* Clear via the single-frame path. */
    rt_scene_node3d_set_impostor(node, 0.0, nullptr);
    EXPECT_EQ_INT(rt_scene_node3d_get_impostor_frame_index(node), 0, "clear resets frame state");

    for (void *o : {strip, mesh, node}) {
        if (rt_obj_release_check0(o))
            rt_obj_free(o);
    }
    PASS();
}

bool test_generate_impostors() {
    TEST("generateImpostors captures yaw strips for proxy-resident cells");

    const char *cell_path = "/tmp/zanna_g3d_hlod_ring_cell.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_hlod_ring_cells.json";
    char manifest[768];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"impo\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536,"
                  "\"proxy\":\"/tmp/zanna_g3d_hlod_ring_cell_proxy.vscn\","
                  "\"proxyBytes\":16384}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("HLOD Impostors"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream, 0);
    set_center(stream, 200.0, 0.0); /* proxy ring only */
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_proxy_resident_count(stream), 1, "proxy becomes resident");

    EXPECT_EQ_INT(rt_game3d_world_stream_generate_impostors(stream, 400.0),
                  1,
                  "one proxy cell receives an impostor strip");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D HLOD proxy + impostor tests\n");
    bool ok = true;
    ok &= test_bake_cell_proxy();
    ok &= test_proxy_ring_blocking();
    ok &= test_proxy_ring_async();
    ok &= test_impostor_frames_install();
    ok &= test_generate_impostors();
    std::printf("\nHLOD tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
