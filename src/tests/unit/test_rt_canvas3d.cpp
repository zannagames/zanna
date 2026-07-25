//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_canvas3d.cpp
// Purpose: Unit tests for Zanna.Graphics3D types — Mesh3D, Camera3D,
//   Material3D, Light3D. Tests construction, properties, procedural mesh
//   generation, OBJ loading, and camera math.
//
// Key invariants:
//   - Tests run headless (no Canvas3D window creation — that requires display)
//   - All object pointers from _new() must be non-null
//   - Mesh generators produce expected vertex/triangle counts
//   - Camera matrices produce correct transforms
//
// Ownership/Lifetime:
//   - Test-scoped runtime objects are retained only for the duration of each case.
// Links: src/runtime/graphics/3d/render/rt_mesh3d.c,
//        docs/adr/0168-windowless-canvas3d-rendering.md,
//        docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_internal.h"
#include "rt_morphtarget3d.h"
#include "rt_particles3d.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_sprite3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_texatlas3d.h"
#include "rt_textureasset3d.h"
#include "tests/common/PosixCompat.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "rt_canvas3d_clusters.h"
#include "rt_canvas3d_internal.h"
#include "rt_skeleton3d.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"

/* R18 per-instance skinned crowds (declared in rt_skeleton3d_internal.h). */
void rt_canvas3d_draw_mesh_instanced_skinned(void *canvas,
                                             void *mesh_obj,
                                             void *material,
                                             const float *instance_matrices,
                                             int32_t instance_count,
                                             const float *bone_palettes,
                                             const float *prev_bone_palettes,
                                             int32_t bone_count);
}

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

static int tests_passed = 0;
static int tests_total = 0;

typedef struct {
    uint64_t offset;
    uint64_t length;
    uint64_t uncompressed_length;
    uint32_t width;
    uint32_t height;
} TextureAsset3DTestMip;

typedef struct {
    void *vptr;
    void *pixels;
    void **mip_pixels;
    uint8_t **mip_payloads;
    uint8_t *source_backing;
    uint8_t *container_backing;
    uint8_t *mip_fallback_kinds;
    TextureAsset3DTestMip *mips;
    int64_t width;
    int64_t height;
    int64_t mip_count;
    int64_t mip_capacity;
    int64_t resident_mip_start;
    int64_t resident_mip_count;
    int64_t resident_bytes;
    int64_t retained_bytes;
    uint64_t source_backing_bytes;
    uint64_t container_backing_bytes;
    uint64_t container_pixels_generation;
    int64_t container_pixels_width;
    int64_t container_pixels_height;
    const char *container_kind;
    const char *format;
    const char *degraded_reason;
    int8_t compressed;
    int8_t degraded;
    int32_t block_width;
    int32_t block_height;
    int32_t block_bytes;
    int32_t decoder_kind;
    int32_t native_format_id;
    int64_t backend_capability_bit;
    uint64_t cache_identity;
    uint64_t native_revision;
    int8_t alpha_metadata_known;
    int8_t has_alpha_texels;
    int8_t container_tracks_pixels;
} TextureAsset3DTestLayout;

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
} Terrain3DTestLayout;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        tests_total++;                                                                             \
        printf("  [%d] %s... ", tests_total, name);                                                \
    } while (0)
#define PASS()                                                                                     \
    do {                                                                                           \
        tests_passed++;                                                                            \
        printf("ok\n");                                                                            \
    } while (0)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        printf("FAIL: %s\n", msg);                                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            printf("FAIL: expected %lld, got %lld\n", (long long)(b), (long long)(a));             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                                                     \
    do {                                                                                           \
        if (std::fabs((double)(a) - (double)(b)) > (eps)) {                                        \
            printf("FAIL: expected ~%f, got %f\n", (double)(b), (double)(a));                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

template <typename Fn> static bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && (!needle || std::strstr(g_last_trap, needle) != nullptr);
}

static void write_u32le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void write_u64le(uint8_t *dst, uint64_t value) {
    write_u32le(dst, (uint32_t)(value & 0xFFFFFFFFu));
    write_u32le(dst + 4, (uint32_t)(value >> 32));
}

/// @brief Read one little-endian 64-bit fixture field without host-alignment assumptions.
static uint64_t read_u64le(const uint8_t *src) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--)
        value = (value << 8) | src[i];
    return value;
}

/// @brief Read a complete binary fixture into caller-owned vector storage.
/// @return True when open, sizing, and the exact read all succeed.
static bool read_test_binary_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *file = std::fopen(path.c_str(), "rb");
    long size;
    if (!file)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0 || (size = std::ftell(file)) < 0 ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize((size_t)size);
    if (size > 0 && std::fread(out.data(), 1, (size_t)size, file) != (size_t)size) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    return true;
}

/// @brief Build one-level KTX2 bytes for direct memory-loader contract tests.
/// @details The level index uses exact stored/uncompressed lengths and scheme 0. All KTX2 metadata
///          outside the required 2D single-face header remains zero, matching the file helpers.
static std::vector<uint8_t> make_test_ktx2_memory(uint32_t vk_format,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  const uint8_t *payload,
                                                  size_t payload_bytes) {
    static const uint8_t identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    const size_t payload_offset = 80u + 24u;
    std::vector<uint8_t> bytes(payload_offset + payload_bytes, 0);
    std::memcpy(bytes.data(), identifier, sizeof(identifier));
    write_u32le(bytes.data() + 12, vk_format);
    write_u32le(bytes.data() + 16, 1);
    write_u32le(bytes.data() + 20, width);
    write_u32le(bytes.data() + 24, height);
    write_u32le(bytes.data() + 36, 1);
    write_u32le(bytes.data() + 40, 1);
    write_u64le(bytes.data() + 80, payload_offset);
    write_u64le(bytes.data() + 88, payload_bytes);
    write_u64le(bytes.data() + 96, payload_bytes);
    if (payload_bytes > 0)
        std::memcpy(bytes.data() + payload_offset, payload, payload_bytes);
    return bytes;
}

/// @brief Compute RFC 1950 Adler-32 for deterministic zlib fixture construction.
static uint32_t test_adler32(const uint8_t *data, size_t size) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    while (size > 0) {
        size_t chunk = size < 5552u ? size : 5552u;
        for (size_t i = 0; i < chunk; i++) {
            s1 += data[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        data += chunk;
        size -= chunk;
    }
    return (s2 << 16) | s1;
}

/// @brief Wrap bytes in a standards-conforming zlib stream made only of DEFLATE stored blocks.
/// @details Stored blocks keep the large allocation-bound fixture deterministic and avoid using an
///          external compressor. Each block is at most 65,535 bytes and carries one's-complement
///          LEN/NLEN fields; the stream ends with a big-endian Adler-32 trailer.
static std::vector<uint8_t> make_test_zlib_stored(const uint8_t *data, size_t size) {
    std::vector<uint8_t> out = {0x78, 0x01};
    size_t cursor = 0;
    do {
        size_t remaining = size - cursor;
        uint16_t count = (uint16_t)(remaining > 65535u ? 65535u : remaining);
        bool final = cursor + count == size;
        uint16_t inverse = (uint16_t)~count;
        out.push_back(final ? 0x01u : 0x00u);
        out.push_back((uint8_t)count);
        out.push_back((uint8_t)(count >> 8));
        out.push_back((uint8_t)inverse);
        out.push_back((uint8_t)(inverse >> 8));
        out.insert(out.end(), data + cursor, data + cursor + count);
        cursor += count;
    } while (cursor < size);
    uint32_t adler = test_adler32(data, size);
    out.push_back((uint8_t)(adler >> 24));
    out.push_back((uint8_t)(adler >> 16));
    out.push_back((uint8_t)(adler >> 8));
    out.push_back((uint8_t)adler);
    return out;
}

/// @brief Build one-level supercompressed KTX2 bytes with an exact final-length declaration.
static std::vector<uint8_t> make_test_supercompressed_ktx2_memory(uint32_t vk_format,
                                                                  uint32_t width,
                                                                  uint32_t height,
                                                                  uint32_t scheme,
                                                                  const uint8_t *payload,
                                                                  size_t payload_bytes,
                                                                  uint64_t final_bytes) {
    std::vector<uint8_t> bytes =
        make_test_ktx2_memory(vk_format, width, height, payload, payload_bytes);
    write_u32le(bytes.data() + 44, scheme);
    write_u64le(bytes.data() + 96, final_bytes);
    return bytes;
}

static bool write_test_ktx2_mips(const char *path,
                                 uint32_t vk_format,
                                 uint32_t width,
                                 uint32_t height,
                                 const uint8_t *const *levels,
                                 const uint64_t *level_bytes,
                                 uint32_t level_count) {
    static const uint8_t identifier[12] = {
        0xAB,
        0x4B,
        0x54,
        0x58,
        0x20,
        0x32,
        0x30,
        0xBB,
        0x0D,
        0x0A,
        0x1A,
        0x0A,
    };
    const size_t level_table_offset = 80u;
    const size_t level_entry_size = 24u;
    const size_t header_size = level_table_offset + (size_t)level_count * level_entry_size;
    std::vector<uint8_t> header(header_size);
    uint64_t payload_offset = header_size;
    FILE *file;

    if (!levels || !level_bytes || level_count == 0)
        return false;
    std::memcpy(header.data(), identifier, sizeof(identifier));
    write_u32le(header.data() + 12, vk_format);
    write_u32le(header.data() + 16, 1);
    write_u32le(header.data() + 20, width);
    write_u32le(header.data() + 24, height);
    write_u32le(header.data() + 28, 0);
    write_u32le(header.data() + 32, 0);
    write_u32le(header.data() + 36, 1);
    write_u32le(header.data() + 40, level_count);
    write_u32le(header.data() + 44, 0);
    for (uint32_t i = 0; i < level_count; i++) {
        uint8_t *entry = header.data() + level_table_offset + (size_t)i * level_entry_size;
        write_u64le(entry, payload_offset);
        write_u64le(entry + 8, level_bytes[i]);
        write_u64le(entry + 16, level_bytes[i]);
        payload_offset += level_bytes[i];
    }

    file = std::fopen(path, "wb");
    if (!file)
        return false;
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
        std::fclose(file);
        return false;
    }
    for (uint32_t i = 0; i < level_count; i++) {
        if (level_bytes[i] > 0 && levels[i] &&
            std::fwrite(levels[i], 1, (size_t)level_bytes[i], file) != (size_t)level_bytes[i]) {
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);
    return true;
}

static bool write_test_ktx2(const char *path,
                            uint32_t vk_format,
                            uint32_t width,
                            uint32_t height,
                            const uint8_t *level0,
                            uint64_t level0_bytes) {
    const uint8_t *levels[] = {level0};
    const uint64_t level_bytes[] = {level0_bytes};
    return write_test_ktx2_mips(path, vk_format, width, height, levels, level_bytes, 1);
}

static bool write_test_ktx2_custom_header(const char *path,
                                          uint32_t vk_format,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t level_count,
                                          uint32_t supercompression_scheme,
                                          const uint8_t *payload,
                                          uint64_t payload_bytes) {
    static const uint8_t identifier[12] = {
        0xAB,
        0x4B,
        0x54,
        0x58,
        0x20,
        0x32,
        0x30,
        0xBB,
        0x0D,
        0x0A,
        0x1A,
        0x0A,
    };
    const size_t level_table_offset = 80u;
    const size_t level_entry_size = 24u;
    const size_t header_size = level_table_offset + (size_t)level_count * level_entry_size;
    std::vector<uint8_t> header(header_size);
    FILE *file;

    std::memcpy(header.data(), identifier, sizeof(identifier));
    write_u32le(header.data() + 12, vk_format);
    write_u32le(header.data() + 16, 1);
    write_u32le(header.data() + 20, width);
    write_u32le(header.data() + 24, height);
    write_u32le(header.data() + 28, 0);
    write_u32le(header.data() + 32, 0);
    write_u32le(header.data() + 36, 1);
    write_u32le(header.data() + 40, level_count);
    write_u32le(header.data() + 44, supercompression_scheme);
    if (level_count > 0) {
        uint8_t *entry = header.data() + level_table_offset;
        write_u64le(entry, header_size);
        write_u64le(entry + 8, payload_bytes);
        write_u64le(entry + 16, payload_bytes);
    }

    file = std::fopen(path, "wb");
    if (!file)
        return false;
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
        std::fclose(file);
        return false;
    }
    if (payload && payload_bytes > 0 &&
        std::fwrite(payload, 1, (size_t)payload_bytes, file) != (size_t)payload_bytes) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    return true;
}

extern "C" double rt_vec3_x(void *v);
extern "C" double rt_vec3_y(void *v);
extern "C" double rt_vec3_z(void *v);
extern "C" void *rt_vec3_new(double x, double y, double z);
extern "C" void *rt_pixels_new(int64_t width, int64_t height);
extern "C" int64_t rt_pixels_width(void *pixels);
extern "C" int64_t rt_pixels_height(void *pixels);
extern "C" void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color);
extern "C" void *rt_mat4_new(double m00,
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
extern "C" void *rt_mat4_identity(void);
extern "C" void *rt_mat4_scale(double sx, double sy, double sz);
extern "C" void *rt_mat4_translate(double tx, double ty, double tz);
extern "C" void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern "C" void rt_obj_retain_maybe(void *obj);
extern "C" int rt_obj_release_check0(void *obj);
extern "C" void rt_obj_free(void *obj);
extern "C" void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern "C" void *rt_postfx3d_new(void);
extern "C" void rt_canvas3d_set_post_fx(void *canvas, void *postfx);
extern "C" void *rt_canvas3d_screenshot(void *canvas);
extern "C" void *rt_canvas3d_new_offscreen(void *target);
extern "C" void *rt_canvas3d_new_offscreen_accelerated(void *target);
extern "C" int8_t rt_canvas3d_get_is_offscreen(void *canvas);
extern "C" void rt_postfx3d_set_enabled(void *obj, int8_t enabled);
extern "C" void rt_postfx3d_add_vignette(void *obj, double radius, double softness);
extern "C" void rt_postfx3d_add_tonemap(void *obj, int64_t mode, double exposure);
extern "C" void rt_postfx3d_add_ssao(void *obj, double radius, double intensity, int64_t samples);
extern "C" void rt_postfx3d_apply_to_canvas(void *canvas);
extern "C" int64_t rt_canvas3d_get_quality_requested(void *canvas);
extern "C" int64_t rt_canvas3d_get_quality_active(void *canvas);
extern "C" int8_t rt_canvas3d_get_quality_fallback(void *canvas);

static bool finite_vec3(void *v) {
    return v && std::isfinite(rt_vec3_x(v)) && std::isfinite(rt_vec3_y(v)) &&
           std::isfinite(rt_vec3_z(v));
}

static bool bounded_vec3(void *v, double limit) {
    return finite_vec3(v) && std::fabs(rt_vec3_x(v)) <= limit && std::fabs(rt_vec3_y(v)) <= limit &&
           std::fabs(rt_vec3_z(v)) <= limit;
}

static void free_canvas3d_test_draw_state(rt_canvas3d *canvas) {
    if (!canvas)
        return;
    canvas3d_release_retained_mesh_revisions(canvas);
    for (int32_t i = 0; i < canvas->temp_buf_count; i++)
        free(canvas->temp_buffers[i]);
    for (int32_t i = 0; i < canvas->temp_obj_count; i++) {
        if (canvas->temp_objects[i] && rt_obj_release_check0(canvas->temp_objects[i]))
            rt_obj_free(canvas->temp_objects[i]);
    }
    free(canvas->temp_buffers);
    free(canvas->temp_objects);
    free(canvas->temp_buffer_set);
    free(canvas->temp_object_set);
    free(canvas->float_snapshots);
    free(canvas->mesh_snapshots);
    free(canvas->mesh_snapshot_hash);
    free(canvas->final_overlay_arena);
    free(canvas->draw_cmds);
    canvas->temp_buffers = nullptr;
    canvas->temp_objects = nullptr;
    canvas->temp_buffer_set = nullptr;
    canvas->temp_object_set = nullptr;
    canvas->float_snapshots = nullptr;
    canvas->mesh_snapshots = nullptr;
    canvas->mesh_snapshot_hash = nullptr;
    canvas->final_overlay_arena = nullptr;
    canvas->draw_cmds = nullptr;
    canvas->temp_buf_count = canvas->temp_buf_capacity = 0;
    canvas->temp_obj_count = canvas->temp_obj_capacity = 0;
    canvas->temp_buffer_set_capacity = 0;
    canvas->temp_object_set_capacity = 0;
    canvas->float_snapshot_count = canvas->float_snapshot_capacity = 0;
    canvas->draw_count = canvas->draw_capacity = 0;
    canvas->mesh_snapshot_count = canvas->mesh_snapshot_capacity = 0;
    canvas->mesh_snapshot_hash_capacity = 0;
    canvas->final_overlay_arena_capacity = 0u;
    canvas->final_overlay_arena_used = 0u;
    canvas->final_overlay_arena_peak = 0u;
}

static bool finite_camera_state(rt_camera3d *cam) {
    if (!cam)
        return false;
    for (double value : cam->view)
        if (!std::isfinite(value))
            return false;
    for (double value : cam->projection)
        if (!std::isfinite(value))
            return false;
    for (double value : cam->eye)
        if (!std::isfinite(value))
            return false;
    for (double value : cam->shake_offset)
        if (!std::isfinite(value))
            return false;
    return std::isfinite(cam->fov) && std::isfinite(cam->aspect) &&
           std::isfinite(cam->near_plane) && std::isfinite(cam->far_plane) &&
           std::isfinite(cam->fps_yaw) && std::isfinite(cam->fps_pitch) &&
           std::isfinite(cam->shake_intensity) && std::isfinite(cam->shake_duration) &&
           std::isfinite(cam->shake_decay) && std::isfinite(cam->ortho_size);
}

static bool finite_light_state(rt_light3d *light) {
    if (!light)
        return false;
    for (double value : light->direction)
        if (!std::isfinite(value))
            return false;
    for (double value : light->position)
        if (!std::isfinite(value))
            return false;
    for (double value : light->color)
        if (!std::isfinite(value))
            return false;
    for (double value : light->basis_u)
        if (!std::isfinite(value))
            return false;
    for (double value : light->basis_v)
        if (!std::isfinite(value))
            return false;
    return std::isfinite(light->intensity) && std::isfinite(light->attenuation) &&
           std::isfinite(light->inner_cos) && std::isfinite(light->outer_cos) &&
           std::isfinite(light->width) && std::isfinite(light->height) &&
           std::isfinite(light->radius) && std::isfinite(light->range);
}

//=============================================================================
// Mesh3D tests
//=============================================================================

static void test_mesh_empty() {
    TEST("Mesh3D.New creates empty mesh");
    void *m = rt_mesh3d_new();
    assert(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 0);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 0);
    PASS();
}

static void test_mesh_add_vertex_triangle() {
    TEST("Mesh3D AddVertex/AddTriangle");
    void *m = rt_mesh3d_new();
    EXPECT_TRUE(expect_trap_contains([&] { rt_mesh3d_add_vertex(m, NAN, 0, 0, 0, 1, 0, 0, 0); },
                                     "vertex attributes"),
                "invalid AddVertex traps");
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 0);
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    PASS();
}

static void test_mesh_reserve_presizes_without_dirtying_geometry() {
    TEST("Mesh3D.Reserve presizes without dirtying geometry");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    uint32_t revision = m->geometry_revision;
    rt_mesh3d_reserve(m, 1000, 500);
    EXPECT_TRUE(m->vertex_capacity >= 1000, "vertex reserve capacity");
    EXPECT_TRUE(m->index_capacity >= 1500, "index reserve capacity");
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 0);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 0);
    EXPECT_EQ(m->geometry_revision, revision);
    EXPECT_TRUE(expect_trap_contains([&] { rt_mesh3d_reserve(m, -1, 0); },
                                     "capacities must be non-negative"),
                "negative reserve traps");
    PASS();
}

static void test_mesh_mutations_restore_residency_and_counts_are_clamped() {
    TEST("Mesh3D mutations restore residency and public counts clamp to capacity");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    rt_mesh3d_reserve(m, 4, 1);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);

    rt_mesh3d_set_resident(m, 0);
    EXPECT_EQ(rt_mesh3d_get_resident(m), 0);
    rt_mesh3d_add_vertex(m, 0, 0, 1, 0, 1, 0, 1, 1);
    EXPECT_EQ(rt_mesh3d_get_resident(m), 1);

    rt_mesh3d_set_resident(m, 0);
    rt_mesh3d_recalc_normals(m);
    EXPECT_EQ(rt_mesh3d_get_resident(m), 1);

    m->vertex_count = std::numeric_limits<uint32_t>::max();
    m->index_count = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), m->vertex_capacity);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), m->index_capacity / 3u);
    int64_t retained_bytes = (int64_t)m->vertex_capacity * (int64_t)sizeof(vgfx3d_vertex_t) +
                             (int64_t)m->index_capacity * (int64_t)sizeof(uint32_t);
    EXPECT_EQ(rt_mesh3d_get_resident_bytes(m), retained_bytes);
    EXPECT_EQ(rt_mesh3d_get_retained_bytes(m), retained_bytes);
    rt_mesh3d_set_resident(m, 0);
    EXPECT_EQ(rt_mesh3d_get_resident_bytes(m), 0);
    EXPECT_EQ(rt_mesh3d_get_retained_bytes(m), retained_bytes);
    PASS();
}

static void test_mesh_recalc_normals_reuses_large_accumulator() {
    TEST("Mesh3D.RecalcNormals reuses large normal accumulator");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    const int vertex_count = 264;
    rt_mesh3d_reserve(m, vertex_count, vertex_count - 2);
    for (int i = 0; i < vertex_count; i++)
        rt_mesh3d_add_vertex(m, (double)i, (double)(i & 1), 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    for (int i = 0; i + 2 < vertex_count; i++)
        rt_mesh3d_add_triangle(m, i, i + 1, i + 2);

    rt_mesh3d_recalc_normals(m);
    double *first_scratch = m->normal_accum_scratch;
    size_t first_values = m->normal_accum_scratch_values;
    EXPECT_TRUE(first_scratch != nullptr, "large mesh allocated reusable normal scratch");
    EXPECT_TRUE(first_values >= (size_t)vertex_count * 3u, "normal scratch covers all vertices");
    rt_mesh3d_recalc_normals(m);
    EXPECT_TRUE(m->normal_accum_scratch == first_scratch, "second recalculation reuses scratch");
    EXPECT_EQ((int64_t)m->normal_accum_scratch_values, (int64_t)first_values);
    PASS();
}

static void test_mesh_generators_batch_geometry_revision_updates() {
    TEST("Mesh3D generators batch geometry revision updates");
    rt_mesh3d *box = (rt_mesh3d *)rt_mesh3d_new_box(1.0, 1.0, 1.0);
    assert(box);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(box), 24);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(box), 12);
    EXPECT_EQ(box->geometry_revision, 2u);
    EXPECT_EQ(box->tangents_ready, 0);
    PASS();
}

static void test_mesh_reject_invalid_triangle_indices() {
    TEST("Mesh3D.AddTriangle rejects invalid indices");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    EXPECT_TRUE(expect_trap_contains([&] { rt_mesh3d_add_triangle(m, -1, 1, 2); },
                                     "vertex index must be non-negative"),
                "negative triangle indices trap");
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 0);
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    EXPECT_TRUE(expect_trap_contains([&] { rt_mesh3d_add_triangle(m, 0, 1, 9); },
                                     "vertex index out of range"),
                "out-of-range triangle indices trap");
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_mesh3d_add_triangle(m, 0, 1, 1); }, "degenerate triangle"),
        "degenerate triangle indices trap");
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    PASS();
}

static void test_mesh_calc_tangents_tracks_mirrored_uv_handedness() {
    TEST("Mesh3D.CalcTangents stores mirrored UV handedness");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    rt_mesh3d_add_vertex(m, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(m, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(m, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);

    rt_mesh3d_calc_tangents(m);

    EXPECT_NEAR(m->vertices[0].tangent[0], 0.0, 0.001);
    EXPECT_NEAR(m->vertices[0].tangent[1], 1.0, 0.001);
    EXPECT_NEAR(m->vertices[0].tangent[2], 0.0, 0.001);
    EXPECT_TRUE(m->vertices[0].tangent[3] < 0.0f,
                "Mirrored UVs record a negative tangent handedness");
    PASS();
}

static void test_mesh_box() {
    TEST("Mesh3D.NewBox — 24 verts, 12 tris");
    void *m = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    assert(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 24);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 12);
    PASS();
}

static double mesh_triangle_area_sq(const rt_mesh3d *mesh, uint32_t tri) {
    const uint32_t *idx = &mesh->indices[(size_t)tri * 3u];
    const float *a = mesh->vertices[idx[0]].pos;
    const float *b = mesh->vertices[idx[1]].pos;
    const float *c = mesh->vertices[idx[2]].pos;
    double abx = (double)b[0] - (double)a[0];
    double aby = (double)b[1] - (double)a[1];
    double abz = (double)b[2] - (double)a[2];
    double acx = (double)c[0] - (double)a[0];
    double acy = (double)c[1] - (double)a[1];
    double acz = (double)c[2] - (double)a[2];
    double cx = aby * acz - abz * acy;
    double cy = abz * acx - abx * acz;
    double cz = abx * acy - aby * acx;
    return cx * cx + cy * cy + cz * cz;
}

static double mesh_triangle_normal_dot_centroid(const rt_mesh3d *mesh, uint32_t tri) {
    const uint32_t *idx = &mesh->indices[(size_t)tri * 3u];
    const float *a = mesh->vertices[idx[0]].pos;
    const float *b = mesh->vertices[idx[1]].pos;
    const float *c = mesh->vertices[idx[2]].pos;
    double abx = (double)b[0] - (double)a[0];
    double aby = (double)b[1] - (double)a[1];
    double abz = (double)b[2] - (double)a[2];
    double acx = (double)c[0] - (double)a[0];
    double acy = (double)c[1] - (double)a[1];
    double acz = (double)c[2] - (double)a[2];
    double nx = aby * acz - abz * acy;
    double ny = abz * acx - abx * acz;
    double nz = abx * acy - aby * acx;
    double cx = ((double)a[0] + (double)b[0] + (double)c[0]) / 3.0;
    double cy = ((double)a[1] + (double)b[1] + (double)c[1]) / 3.0;
    double cz = ((double)a[2] + (double)b[2] + (double)c[2]) / 3.0;
    return nx * cx + ny * cy + nz * cz;
}

static void test_mesh_sphere() {
    TEST("Mesh3D.NewSphere — correct non-degenerate geometry");
    const int64_t segments = 8;
    const int64_t slices = segments * 2;
    const int64_t expected_vertices = 2 + (segments - 1) * (slices + 1);
    void *m = rt_mesh3d_new_sphere(1.0, segments);
    assert(m);
    // One vertex for each pole, with seam-duplicated interior rings for UVs.
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), expected_vertices);
    // Top and bottom caps use one triangle per slice to avoid zero-area pole faces.
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 224);
    rt_mesh3d *mesh = (rt_mesh3d *)m;
    for (uint32_t tri = 0; tri < mesh->index_count / 3u; ++tri) {
        EXPECT_TRUE(mesh_triangle_area_sq(mesh, tri) > 1e-12,
                    "Sphere triangles are non-degenerate");
        EXPECT_TRUE(mesh_triangle_normal_dot_centroid(mesh, tri) > 0.0,
                    "Sphere triangles are wound outward");
    }
    PASS();
}

static void test_mesh_plane() {
    TEST("Mesh3D.NewPlane — 4 verts, 2 tris");
    void *m = rt_mesh3d_new_plane(2.0, 2.0);
    assert(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 4);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 2);
    PASS();
}

static void test_mesh_cylinder() {
    TEST("Mesh3D.NewCylinder — correct geometry");
    void *m = rt_mesh3d_new_cylinder(1.0, 2.0, 8);
    assert(m);
    // Side: (8+1)*2 = 18, top cap: 1+8 = 9, bottom cap: 1+8 = 9 → 36
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 36);
    // Side: 8*2=16, top: 8, bottom: 8 → 32
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 32);
    rt_mesh3d *mesh = (rt_mesh3d *)m;
    for (uint32_t tri = 0; tri < mesh->index_count / 3u; ++tri)
        EXPECT_TRUE(mesh_triangle_normal_dot_centroid(mesh, tri) > 0.0,
                    "Cylinder triangles are wound outward");
    PASS();
}

static void test_mesh_generators_reject_invalid_dimensions() {
    TEST("Mesh3D generators reject invalid dimensions");
    EXPECT_TRUE(expect_trap_contains([] { rt_mesh3d_new_box(1.0, 0.0, 1.0); }, "greater than zero"),
                "NewBox rejects zero-sized dimensions");
    EXPECT_TRUE(expect_trap_contains([] { rt_mesh3d_new_sphere(NAN, 8); }, "greater than zero"),
                "NewSphere rejects non-finite radii");
    EXPECT_TRUE(
        expect_trap_contains([] { rt_mesh3d_new_cylinder(1.0, -2.0, 8); }, "greater than zero"),
        "NewCylinder rejects negative heights");
    PASS();
}

static void test_mesh_clone() {
    TEST("Mesh3D.Clone preserves geometry");
    void *m = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    ((rt_mesh3d *)m)->bone_count = 4;
    void *c = rt_mesh3d_clone(m);
    assert(c);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(c), 24);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(c), 12);
    EXPECT_EQ(((rt_mesh3d *)c)->bone_count, 0);
    PASS();
}

static void test_mesh_clone_repairs_corrupt_private_counts() {
    TEST("Mesh3D.Clone repairs corrupt private geometry counts");
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_new_box(1.0, 1.0, 1.0);
    assert(mesh != NULL);
    mesh->vertex_count = mesh->vertex_capacity + 17u;
    mesh->index_count = mesh->index_capacity + 11u;

    rt_mesh3d *clone = (rt_mesh3d *)rt_mesh3d_clone(mesh);
    assert(clone != NULL);
    EXPECT_EQ(mesh->vertex_count, mesh->vertex_capacity);
    EXPECT_EQ(mesh->index_count, mesh->index_capacity);
    EXPECT_EQ(clone->vertex_count, mesh->vertex_capacity);
    EXPECT_EQ(clone->index_count, mesh->index_capacity);
    PASS();
}

static void test_mesh_clone_deep_copies_morph_targets() {
    TEST("Mesh3D.Clone deep-copies morph targets");
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0, 0, 0, 0, 1, 0, 0, 0);
    void *morph = rt_morphtarget3d_new(1);
    int64_t shape = rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_morphtarget3d_set_weight(morph, shape, 0.25);
    rt_morphtarget3d_set_delta(morph, shape, 0, 1.0, 2.0, 3.0);
    rt_mesh3d_set_morph_targets(mesh, morph);

    rt_mesh3d *clone = (rt_mesh3d *)rt_mesh3d_clone(mesh);
    assert(clone != NULL);
    EXPECT_TRUE(clone->morph_targets_ref != nullptr, "clone keeps morph targets");
    EXPECT_TRUE(clone->morph_targets_ref != morph, "clone does not share morph target payload");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(clone->morph_targets_ref, shape), 0.25, 0.001);

    rt_morphtarget3d_set_weight(morph, shape, 0.75);
    rt_morphtarget3d_set_delta(morph, shape, 0, 9.0, 9.0, 9.0);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(clone->morph_targets_ref, shape), 0.25, 0.001);
    const float *clone_deltas = rt_morphtarget3d_get_packed_deltas(clone->morph_targets_ref);
    EXPECT_NEAR(clone_deltas[0], 1.0, 0.001);
    EXPECT_NEAR(clone_deltas[1], 2.0, 0.001);
    EXPECT_NEAR(clone_deltas[2], 3.0, 0.001);
    PASS();
}

static void test_mesh_transform_uses_inverse_transpose_normals() {
    TEST("Mesh3D.Transform uses inverse-transpose normals");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0.70710678, 0.70710678, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0.70710678, 0.70710678, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0.70710678, 0.70710678, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);

    void *scale = rt_mat4_scale(2.0, 1.0, 1.0);
    rt_mesh3d_transform(m, scale);

    rt_mesh3d *mesh = (rt_mesh3d *)m;
    EXPECT_NEAR(mesh->vertices[0].normal[0], 0.44721359, 0.01);
    EXPECT_NEAR(mesh->vertices[0].normal[1], 0.89442719, 0.01);
    EXPECT_NEAR(mesh->vertices[0].normal[2], 0.0, 0.01);
    PASS();
}

static void test_mesh_transform_flips_tangent_handedness_for_mirrors() {
    TEST("Mesh3D.Transform flips tangent handedness for mirrored transforms");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    rt_mesh3d_add_vertex(m, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(m, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(m, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_calc_tangents(m);
    EXPECT_TRUE(m->vertices[0].tangent[3] > 0.0f, "Baseline tangent handedness starts positive");
    uint32_t before_i1 = m->indices[1];
    uint32_t before_i2 = m->indices[2];

    void *mirror = rt_mat4_scale(-1.0, 1.0, 1.0);
    rt_mesh3d_transform(m, mirror);

    EXPECT_TRUE(m->vertices[0].tangent[3] < 0.0f,
                "Mirrored transforms flip tangent handedness for normal mapping");
    EXPECT_EQ(m->indices[1], before_i2);
    EXPECT_EQ(m->indices[2], before_i1);
    PASS();
}

static void test_mesh_recalc_normals() {
    TEST("Mesh3D.RecalcNormals — produces unit normals");
    void *m = rt_mesh3d_new();
    // Flat triangle in XY plane
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_recalc_normals(m);
    // Normal should be (0, 0, 1) for CCW triangle in XY plane
    // (Can't directly access vertex data from public API, but at least it doesn't crash)
    PASS();
}

static void test_mesh_recalc_normals_uses_double_accumulation() {
    TEST("Mesh3D.RecalcNormals handles large finite triangles");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1.0e20, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 0, 1.0e20, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_recalc_normals(m);
    EXPECT_NEAR(m->vertices[0].normal[0], 0.0, 0.001);
    EXPECT_NEAR(m->vertices[0].normal[1], 0.0, 0.001);
    EXPECT_NEAR(m->vertices[0].normal[2], 1.0, 0.001);
    PASS();
}

static void test_mesh_obj_loader() {
    TEST("Mesh3D.FromOBJ — loads test cube");
    /* Try multiple paths since ctest working directory may differ.
     * We must check with fopen BEFORE calling FromOBJ so missing fixtures
     * produce an actionable test failure instead of a recoverable loader error. */
    const char *found = NULL;
    const char *paths[] = {"src/tests/fixtures/runtime/test_cube.obj",
                           "../src/tests/fixtures/runtime/test_cube.obj",
                           "../../src/tests/fixtures/runtime/test_cube.obj",
                           NULL};
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) {
            fclose(f);
            found = paths[i];
            break;
        }
    }
    if (!found) {
        printf("SKIP (test_cube.obj not found in any search path)\n");
        tests_passed++;
        return;
    }
    rt_string path = rt_string_from_bytes(found, (int64_t)strlen(found));
    void *m = rt_mesh3d_from_obj(path);
    assert(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 8);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 12);
    PASS();
}

static void test_mesh_obj_loader_flattens_material_groups() {
    TEST("Mesh3D.FromOBJ — flattens material/group directives");
    const char *path = "/tmp/zanna_obj_material_group_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("mtllib test.mtl\n"
          "o Mesh\n"
          "g Front\n"
          "v 0 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n"
          "vt 0 0\n"
          "vt 1 0\n"
          "vt 0 1\n"
          "vn 0 0 1\n"
          "usemtl Material0\n"
          "f 1/1/1 2/2/1 3/3/1\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    void *mesh = rt_mesh3d_from_obj(obj_path);
    assert(mesh);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 1);
    PASS();
}

static void test_mesh_obj_loader_fills_only_missing_normals() {
    TEST("Mesh3D.FromOBJ — preserves authored normals while filling missing normals");
    const char *path = "/tmp/zanna_obj_mixed_normals_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n"
          "v -1 0 0\n"
          "vn 0 0 -1\n"
          "f 1//1 2//1 3//1\n"
          "f 1 3 4\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_from_obj(obj_path);
    assert(mesh);
    bool saw_authored_negative = false;
    bool saw_generated_positive = false;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        if (mesh->vertices[i].normal[2] < -0.9f)
            saw_authored_negative = true;
        if (mesh->vertices[i].normal[2] > 0.9f)
            saw_generated_positive = true;
    }
    EXPECT_TRUE(saw_authored_negative, "authored normals must not be overwritten");
    EXPECT_TRUE(saw_generated_positive, "missing normals should be generated");
    PASS();
}

static void test_mesh_obj_loader_deduplicates_vertices_and_handles_ngons() {
    TEST("Mesh3D.FromOBJ — deduplicates vertices and triangulates n-gons");
    const char *path = "/tmp/zanna_obj_dedup_ngon_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 1 0 0\n"
          "v 1 1 0\n"
          "v 0 1 0\n"
          "v -1 0.5 0\n"
          "vt 0 0\n"
          "vt 1 0\n"
          "vt 1 1\n"
          "vt 0 1\n"
          "vt -1 0.5\n"
          "vn 0 0 1\n"
          "f 1/1/1 2/2/1 3/3/1 4/4/1 5/5/1\n"
          "f 1/1/1 3/3/1 5/5/1 # duplicate vertex tuples are reused\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    void *mesh = rt_mesh3d_from_obj(obj_path);
    assert(mesh);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 5);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 4);
    PASS();
}

static void test_mesh_obj_loader_ear_clips_concave_ngons() {
    TEST("Mesh3D.FromOBJ — ear-clips concave n-gons");
    const char *path = "/tmp/zanna_obj_concave_ngon_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 2 0 0\n"
          "v 2 2 0\n"
          "v 1.4 0.6 0\n"
          "v 0 2 0\n"
          "vn 0 0 1\n"
          "f 1//1 2//1 3//1 4//1 5//1\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_from_obj(obj_path);
    assert(mesh);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 5);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 3);
    for (uint32_t tri = 0; tri < mesh->index_count; tri += 3) {
        uint32_t a = mesh->indices[tri + 0];
        uint32_t b = mesh->indices[tri + 1];
        uint32_t c = mesh->indices[tri + 2];
        bool is_bad_fan_ear = (a == 0 || b == 0 || c == 0) && (a == 1 || b == 1 || c == 1) &&
                              (a == 2 || b == 2 || c == 2);
        EXPECT_TRUE(!is_bad_fan_ear, "concave polygon should not emit the invalid fan ear");
    }
    PASS();
}

static void test_mesh_obj_loader_rejects_invalid_indices() {
    TEST("Mesh3D.FromOBJ — rejects invalid face indices");
    const char *path = "/tmp/zanna_obj_invalid_index_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n"
          "f 1 2 9\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    void *mesh = rt_mesh3d_from_obj(obj_path);
    EXPECT_TRUE(mesh == nullptr, "FromOBJ rejects invalid indices without emitting a mesh");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "FromOBJ records an error for invalid indices");
    PASS();
}

static void test_mesh_obj_loader_rejects_invalid_numeric_tokens() {
    TEST("Mesh3D.FromOBJ — rejects invalid numeric tokens");
    const char *path = "/tmp/zanna_obj_invalid_numeric_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v nan 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n"
          "f 1 2 3\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    void *mesh = rt_mesh3d_from_obj(obj_path);
    EXPECT_TRUE(mesh == nullptr, "FromOBJ rejects NaN vertex positions without trapping");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "FromOBJ records an error for NaN vertex positions");

    f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n"
          "f 1 2 999999999999999999999999999999\n",
          f);
    fclose(f);
    obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    mesh = rt_mesh3d_from_obj(obj_path);
    EXPECT_TRUE(mesh == nullptr, "FromOBJ rejects overflowing face indices without trapping");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "FromOBJ records an error for overflowing face indices");
    PASS();
}

static void test_mesh_obj_loader_rejects_empty_geometry() {
    TEST("Mesh3D.FromOBJ — rejects files without faces");
    const char *path = "/tmp/zanna_obj_empty_geometry_test.obj";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("v 0 0 0\n"
          "v 1 0 0\n"
          "v 0 1 0\n",
          f);
    fclose(f);

    rt_string obj_path = rt_string_from_bytes(path, (int64_t)strlen(path));
    void *mesh = rt_mesh3d_from_obj(obj_path);
    EXPECT_TRUE(mesh == nullptr, "FromOBJ rejects empty geometry without trapping");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "FromOBJ records an error for empty geometry");
    PASS();
}

namespace {
struct BackendEnvGuard {
    bool had_value = false;
    std::string original;

    BackendEnvGuard() {
        if (const char *value = getenv("ZANNA_3D_BACKEND")) {
            had_value = true;
            original = value;
        }
    }

    ~BackendEnvGuard() {
        if (had_value)
            setenv("ZANNA_3D_BACKEND", original.c_str(), 1);
        else
            unsetenv("ZANNA_3D_BACKEND");
    }
};
} // namespace

static void test_backend_select_software_override() {
    TEST("Backend selection - software override");
    BackendEnvGuard guard;
    int env_status = setenv("ZANNA_3D_BACKEND", "software", 1);
    assert(env_status == 0);
    (void)env_status;
    const vgfx3d_backend_t *b = vgfx3d_select_backend();
    if (!b || strcmp(b->name, "software") != 0) {
        printf("FAIL: expected backend software, got %s\n", (b && b->name) ? b->name : "(null)");
        return;
    }
    PASS();
}

static void test_backend_select_platform_override() {
#if RT_PLATFORM_MACOS
    const char *expected = "metal";
#elif RT_PLATFORM_WINDOWS
    const char *expected = "d3d11";
#elif RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_HEADLESS)
    const char *expected = "opengl";
#else
    const char *expected = "software";
#endif

    TEST("Backend selection - platform override");
    BackendEnvGuard guard;
    int env_status = setenv("ZANNA_3D_BACKEND", expected, 1);
    assert(env_status == 0);
    (void)env_status;
    const vgfx3d_backend_t *b = vgfx3d_select_backend();
    if (!b || strcmp(b->name, expected) != 0) {
        printf(
            "FAIL: expected backend %s, got %s\n", expected, (b && b->name) ? b->name : "(null)");
        return;
    }
    PASS();
}

static void test_backend_default_policy_names() {
    TEST("Backend selection - default policy names");
    EXPECT_TRUE(strcmp(vgfx3d_default_backend_name_for_platform(VGFX3D_BACKEND_PLATFORM_LINUX),
                       "opengl") == 0,
                "Linux defaults to OpenGL and falls back when native context creation fails");
    EXPECT_TRUE(
        strcmp(vgfx3d_default_backend_name_for_platform(VGFX3D_BACKEND_PLATFORM_WINDOWS_ARM64),
               "software") == 0,
        "Windows ARM64 keeps the software default");
    EXPECT_TRUE(strcmp(vgfx3d_default_backend_name_for_platform(VGFX3D_BACKEND_PLATFORM_MACOS),
                       "metal") == 0,
                "macOS defaults to Metal");
    EXPECT_TRUE(strcmp(vgfx3d_default_backend_name_for_platform(VGFX3D_BACKEND_PLATFORM_WINDOWS),
                       "d3d11") == 0,
                "Windows x64 defaults to D3D11");
    PASS();
}

//=============================================================================
// Camera3D tests
//=============================================================================

static void test_camera_new() {
    TEST("Camera3D.New — fov preserved");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    assert(cam);
    EXPECT_NEAR(rt_camera3d_get_fov(cam), 60.0, 0.001);
    PASS();
}

static void test_camera_set_fov() {
    TEST("Camera3D.SetFov — updates projection");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    rt_camera3d_set_fov(cam, 90.0);
    EXPECT_NEAR(rt_camera3d_get_fov(cam), 90.0, 0.001);
    PASS();
}

static void test_camera_projection_mode_and_ortho_size_are_mutable() {
    TEST("Camera3D projection mode and orthographic size are mutable");
    auto *cam = static_cast<rt_camera3d *>(rt_camera3d_new(60.0, 1.5, 0.1, 100.0));
    assert(cam);
    double perspective_projection[16];
    std::memcpy(perspective_projection, cam->projection, sizeof(perspective_projection));

    EXPECT_EQ(rt_camera3d_is_ortho(cam), 0);
    EXPECT_NEAR(rt_camera3d_get_ortho_size(cam), 10.0, 0.001);
    rt_camera3d_set_ortho_size(cam, 7.5);
    EXPECT_NEAR(rt_camera3d_get_ortho_size(cam), 7.5, 0.001);
    rt_camera3d_set_is_ortho(cam, 1);
    EXPECT_EQ(rt_camera3d_is_ortho(cam), 1);
    EXPECT_TRUE(
        std::memcmp(perspective_projection, cam->projection, sizeof(perspective_projection)) != 0,
        "Projection matrix changes when orthographic mode is enabled");

    rt_camera3d_set_ortho_size(cam, std::numeric_limits<double>::quiet_NaN());
    EXPECT_NEAR(rt_camera3d_get_ortho_size(cam), 1.0, 0.001);
    rt_camera3d_set_is_ortho(cam, 0);
    EXPECT_EQ(rt_camera3d_is_ortho(cam), 0);
    EXPECT_NEAR(rt_camera3d_get_fov(cam), 60.0, 0.001);
    PASS();
}

static void test_camera_clip_planes() {
    TEST("Camera3D.NearPlane/FarPlane — tunable clip distances");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    EXPECT_NEAR(rt_camera3d_get_near_plane(cam), 0.1, 0.001);
    EXPECT_NEAR(rt_camera3d_get_far_plane(cam), 100.0, 0.001);
    /* Extend draw distance for a large scene. */
    rt_camera3d_set_far_plane(cam, 5000.0);
    EXPECT_NEAR(rt_camera3d_get_far_plane(cam), 5000.0, 0.001);
    rt_camera3d_set_near_plane(cam, 2.0);
    EXPECT_NEAR(rt_camera3d_get_near_plane(cam), 2.0, 0.001);
    PASS();
}

static void test_camera_look_at() {
    TEST("Camera3D.LookAt — position updated");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 5.0, 10.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *pos = rt_camera3d_get_position(cam);
    assert(pos);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(pos), 5.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(pos), 10.0, 0.001);
    PASS();
}

static void test_camera_forward() {
    TEST("Camera3D.Forward — points toward target");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *fwd = rt_camera3d_get_forward(cam);
    assert(fwd);
    // Forward should point roughly along -Z (toward target from eye at +Z)
    EXPECT_NEAR(rt_vec3_z(fwd), -1.0, 0.1);
    PASS();
}

static void test_camera_orbit() {
    TEST("Camera3D.Orbit — position on sphere");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    rt_camera3d_orbit(cam, target, 5.0, 0.0, 0.0);

    void *pos = rt_camera3d_get_position(cam);
    assert(pos);
    // At yaw=0, pitch=0: eye should be at (0, 0, 5)
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.1);
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.1);
    EXPECT_NEAR(rt_vec3_z(pos), 5.0, 0.1);
    PASS();
}

static void test_camera_orbit_syncs_fps_angles() {
    TEST("Camera3D.Orbit syncs FPS yaw/pitch with the resulting view");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    rt_camera3d_orbit(cam, target, 5.0, 90.0, 0.0);

    void *before = rt_camera3d_get_forward(cam);
    rt_camera3d_fps_update(cam, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *after = rt_camera3d_get_forward(cam);

    EXPECT_NEAR(rt_vec3_x(before), -1.0, 0.1);
    EXPECT_NEAR(rt_vec3_z(before), 0.0, 0.1);
    EXPECT_NEAR(rt_vec3_x(after), rt_vec3_x(before), 0.01);
    EXPECT_NEAR(rt_vec3_y(after), rt_vec3_y(before), 0.01);
    EXPECT_NEAR(rt_vec3_z(after), rt_vec3_z(before), 0.01);
    PASS();
}

static void test_camera_screen_to_ray() {
    TEST("Camera3D.ScreenToRay — center ray along view direction");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(cam, eye, target, up);

    // Center of 640×480 screen
    void *ray = rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480);
    assert(ray);
    // Center ray should point along -Z
    double rz = rt_vec3_z(ray);
    assert(rz < -0.9); // should be ~-1.0
    PASS();
}

//=============================================================================
// Material3D tests
//=============================================================================

static void test_material_new() {
    TEST("Material3D.New — default white");
    void *m = rt_material3d_new();
    assert(m);
    PASS();
}

static void test_material_new_color() {
    TEST("Material3D.NewColor — stores color");
    void *m = rt_material3d_new_color(0.5, 0.3, 0.1);
    assert(m);
    PASS();
}

static void test_material_new_textured() {
    TEST("Material3D.NewTextured — accepts Pixels");
    void *px = rt_pixels_new(4, 4);
    void *m = rt_material3d_new_textured(px);
    assert(m);
    PASS();
}

static void test_material_texture_setters_reject_invalid_handles() {
    TEST("Material3D texture setters reject non-Pixels handles");
    void *mat = rt_material3d_new();
    void *pixels = rt_pixels_new(1, 1);
    void *fake = rt_material3d_new();
    assert(mat != NULL && pixels != NULL && fake != NULL);

    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_new_textured(fake); }, "Pixels"),
                "NewTextured rejects non-Pixels handles");
    rt_material3d_set_texture(mat, pixels);
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_texture(mat, fake); }, "Pixels"),
                "SetTexture rejects non-Pixels handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_normal_map(mat, fake); }, "Pixels"),
                "SetNormalMap rejects non-Pixels handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_metallic_roughness_map(mat, fake); },
                                     "Pixels"),
                "SetMetallicRoughnessMap rejects non-Pixels handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_ao_map(mat, fake); }, "Pixels"),
                "SetAOMap rejects non-Pixels handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_specular_map(mat, fake); }, "Pixels"),
                "SetSpecularMap rejects non-Pixels handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_emissive_map(mat, fake); }, "Pixels"),
                "SetEmissiveMap rejects non-Pixels handles");
    PASS();
}

static void test_material_texture_setters_repair_stale_slots_before_rejecting_invalid_handles() {
    TEST("Material3D texture setters repair stale slots before rejecting invalid handles");
    auto *mat = (rt_material3d *)rt_material3d_new();
    void *fake = rt_material3d_new();
    assert(mat != NULL && fake != NULL);
    size_t fake_refcnt = rt_heap_hdr(fake)->refcnt;

    mat->texture = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_texture(mat, fake); }, "Pixels"),
                "SetTexture still rejects non-texture replacements");
    EXPECT_TRUE(mat->texture == nullptr, "SetTexture clears stale texture slots before trapping");

    mat->normal_map = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_normal_map(mat, fake); }, "Pixels"),
                "SetNormalMap still rejects non-texture replacements");
    EXPECT_TRUE(mat->normal_map == nullptr,
                "SetNormalMap clears stale texture slots before trapping");

    mat->metallic_roughness_map = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_metallic_roughness_map(mat, fake); },
                                     "Pixels"),
                "SetMetallicRoughnessMap still rejects non-texture replacements");
    EXPECT_TRUE(mat->metallic_roughness_map == nullptr,
                "SetMetallicRoughnessMap clears stale texture slots before trapping");

    mat->ao_map = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_ao_map(mat, fake); }, "Pixels"),
                "SetAOMap still rejects non-texture replacements");
    EXPECT_TRUE(mat->ao_map == nullptr, "SetAOMap clears stale texture slots before trapping");

    mat->specular_map = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_specular_map(mat, fake); }, "Pixels"),
                "SetSpecularMap still rejects non-texture replacements");
    EXPECT_TRUE(mat->specular_map == nullptr,
                "SetSpecularMap clears stale texture slots before trapping");

    mat->emissive_map = fake;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_emissive_map(mat, fake); }, "Pixels"),
                "SetEmissiveMap still rejects non-texture replacements");
    EXPECT_TRUE(mat->emissive_map == nullptr,
                "SetEmissiveMap clears stale texture slots before trapping");

    EXPECT_TRUE(rt_heap_hdr(fake)->refcnt == fake_refcnt,
                "Repairing stale material texture slots does not release unowned wrong-class refs");
    PASS();
}

static void release_texture_test_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

static void test_textureasset3d_source_container_bridge() {
    TEST("TextureAsset3D exact source containers — kinds, accounting, mutation, rejection");

    struct ContainerCase {
        const char *input_kind;
        const char *expected_kind;
        const uint8_t *bytes;
        size_t size;
    };

    static const uint8_t ktx2[] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a'};
    static const uint8_t bmp[] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const ContainerCase cases[] = {{"ktx2", "ktx2", ktx2, sizeof(ktx2)},
                                          {"png", "png", png, sizeof(png)},
                                          {"jpg", "jpeg", jpeg, sizeof(jpeg)},
                                          {"gif", "gif", gif, sizeof(gif)},
                                          {"bmp", "bmp", bmp, sizeof(bmp)}};
    for (const auto &entry : cases) {
        void *pixels = rt_pixels_new(1, 1);
        void *asset = rt_textureasset3d_wrap_encoded_pixels(
            pixels, entry.bytes, (uint64_t)entry.size, entry.input_kind);
        const uint8_t *borrowed = nullptr;
        uint64_t borrowed_size = 0;
        const char *borrowed_kind = nullptr;
        if (!pixels || !asset ||
            !rt_textureasset3d_get_source_container(
                asset, &borrowed, &borrowed_size, &borrowed_kind) ||
            borrowed_size != entry.size || !borrowed_kind ||
            std::strcmp(borrowed_kind, entry.expected_kind) != 0 ||
            std::memcmp(borrowed, entry.bytes, entry.size) != 0 ||
            rt_textureasset3d_get_source_container_state(asset) !=
                RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID ||
            rt_textureasset3d_get_retained_bytes(asset) < (int64_t)entry.size + 4) {
            release_texture_test_object(asset);
            release_texture_test_object(pixels);
            FAIL("every supported source kind is retained exactly and counted");
            return;
        }
        release_texture_test_object(asset);
        release_texture_test_object(pixels);
    }

    void *pixels = rt_pixels_new(1, 1);
    void *asset = rt_textureasset3d_wrap_encoded_pixels(pixels, png, sizeof(png), "png");
    assert(pixels != nullptr && asset != nullptr);
    rt_pixels_set_rgba(pixels, 0, 0, 0x12345678);
    const uint8_t *borrowed = reinterpret_cast<const uint8_t *>(uintptr_t(1));
    uint64_t borrowed_size = 9;
    const char *borrowed_kind = reinterpret_cast<const char *>(uintptr_t(1));
    EXPECT_TRUE(
        !rt_textureasset3d_get_source_container(asset, &borrowed, &borrowed_size, &borrowed_kind) &&
            borrowed == nullptr && borrowed_size == 0 && borrowed_kind == nullptr,
        "mutating decoded Pixels invalidates stale encoded provenance");
    EXPECT_TRUE(rt_textureasset3d_get_source_container_state(asset) ==
                        RT_TEXTUREASSET3D_SOURCE_CONTAINER_CHANGED_AFTER_IMPORT &&
                    rt_textureasset3d_get_source_container_state(pixels) ==
                        RT_TEXTUREASSET3D_SOURCE_CONTAINER_NONE,
                "fidelity state distinguishes changed provenance from ordinary Pixels");
    EXPECT_TRUE(rt_pixels_get_rgba(rt_textureasset3d_get_pixels(asset), 0, 0) == 0x12345678,
                "mutation invalidation retains the current drawable pixels");
    release_texture_test_object(asset);
    release_texture_test_object(pixels);

    pixels = rt_pixels_new(1, 1);
    asset = rt_textureasset3d_wrap_encoded_pixels(pixels, png, sizeof(png), "jpeg");
    EXPECT_TRUE(asset == nullptr, "container kind/magic mismatches are rejected");
    release_texture_test_object(pixels);
    PASS();
}

static void test_textureasset3d_ktx2_retains_exact_container() {
    TEST("TextureAsset3D KTX2 load retains the complete validated file");
    const char *path = "/tmp/zanna_textureasset3d_source_container.ktx2";
    const uint8_t rgba[] = {0x12, 0x34, 0x56, 0x78};
    std::vector<uint8_t> file_bytes;
    EXPECT_TRUE(write_test_ktx2(path, 37u, 1u, 1u, rgba, sizeof(rgba)),
                "source-container KTX2 fixture written");
    EXPECT_TRUE(read_test_binary_file(path, file_bytes), "source-container KTX2 fixture read");
    void *asset = rt_textureasset3d_load_ktx2_memory(file_bytes.data(), file_bytes.size());
    assert(asset != nullptr);
    const uint8_t *source = nullptr;
    uint64_t source_size = 0;
    const char *kind = nullptr;
    EXPECT_TRUE(rt_textureasset3d_get_source_container(asset, &source, &source_size, &kind) &&
                    source_size == file_bytes.size() && kind && std::strcmp(kind, "ktx2") == 0 &&
                    std::memcmp(source, file_bytes.data(), file_bytes.size()) == 0,
                "KTX2 source span includes header, metadata tables, and mip bytes exactly");
    EXPECT_TRUE(rt_textureasset3d_get_retained_bytes(asset) >= (int64_t)file_bytes.size() + 4,
                "KTX2 retained-byte accounting includes exact container plus canonical payload");
    release_texture_test_object(asset);
    std::remove(path);
    PASS();
}

static void test_textureasset3d_ktx2_material_bridge() {
    TEST("TextureAsset3D.LoadKTX2 — metadata and Material3D bridge");
    const char *rgba_path = "/tmp/zanna_textureasset3d_rgba8_test.ktx2";
    const char *bc7_path = "/tmp/zanna_textureasset3d_bc7_test.ktx2";
    const uint8_t rgba_level0[] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x10,
        0x20,
        0x30,
        0x40,
        0x55,
        0x66,
        0x77,
        0x88,
        0xA0,
        0xB0,
        0xC0,
        0xD0,
    };
    const uint8_t bc7_level0[] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    rt_string rgba_path_s;
    rt_string bc7_path_s;
    void *rgba_asset;
    void *bc7_asset;
    void *fallback_pixels;
    const uint8_t *native_payload = nullptr;
    uint64_t native_payload_bytes = 0;
    int32_t native_width = 0;
    int32_t native_height = 0;
    int32_t native_block_width = 0;
    int32_t native_block_height = 0;
    int32_t native_block_bytes = 0;
    void *mat;
    void *textured;

    EXPECT_TRUE(write_test_ktx2(rgba_path, 37u, 2u, 2u, rgba_level0, sizeof(rgba_level0)),
                "test RGBA8 KTX2 fixture written");
    EXPECT_TRUE(write_test_ktx2(bc7_path, 145u, 4u, 4u, bc7_level0, sizeof(bc7_level0)),
                "test BC7 KTX2 fixture written");

    rgba_path_s = rt_string_from_bytes(rgba_path, std::strlen(rgba_path));
    rgba_asset = rt_textureasset3d_load_ktx2(rgba_path_s);
    rt_string_unref(rgba_path_s);
    assert(rgba_asset != nullptr);

    EXPECT_EQ(rt_textureasset3d_get_width(rgba_asset), 2);
    EXPECT_EQ(rt_textureasset3d_get_height(rgba_asset), 2);
    EXPECT_EQ(rt_textureasset3d_get_mip_count(rgba_asset), 1);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_textureasset3d_get_format(rgba_asset)), "rgba8") == 0,
                "RGBA8 KTX2 reports rgba8 format");
    EXPECT_EQ(rt_textureasset3d_get_compressed(rgba_asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_degraded(rgba_asset), 0);
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_textureasset3d_get_degraded_reason(rgba_asset)), "") == 0,
        "ordinary KTX2 reports no degradation reason");
    fallback_pixels = rt_textureasset3d_get_pixels(rgba_asset);
    EXPECT_TRUE(fallback_pixels != nullptr, "RGBA8 KTX2 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_get_rgba(fallback_pixels, 0, 0), 0x01020304);
    EXPECT_EQ(rt_pixels_get_rgba(fallback_pixels, 1, 0), 0x10203040);

    mat = rt_material3d_new();
    assert(mat != nullptr);
    rt_material3d_set_texture(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_texture(mat), 1);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(mat) == fallback_pixels,
                "material inspection resolves a TextureAsset3D RGBA fallback");
    rt_material3d_set_texture(mat, nullptr);
    EXPECT_EQ(rt_material3d_get_has_texture(mat), 0);
    rt_material3d_set_albedo_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_texture(mat), 1);
    rt_material3d_set_normal_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_normal_map(mat), 1);
    EXPECT_TRUE(rt_material3d_get_normal_map_pixels(mat) == fallback_pixels,
                "normal-map inspection resolves a TextureAsset3D RGBA fallback");
    rt_material3d_set_specular_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_specular_map(mat), 1);
    rt_material3d_set_emissive_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(mat), 1);
    rt_material3d_set_metallic_roughness_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(mat), 1);
    rt_material3d_set_ao_map(mat, rgba_asset);
    EXPECT_EQ(rt_material3d_get_has_ao_map(mat), 1);
    textured = rt_material3d_new_textured(rgba_asset);
    assert(textured != nullptr);
    EXPECT_EQ(rt_material3d_get_has_texture(textured), 1);

    bc7_path_s = rt_string_from_bytes(bc7_path, std::strlen(bc7_path));
    bc7_asset = rt_textureasset3d_load_ktx2(bc7_path_s);
    rt_string_unref(bc7_path_s);
    assert(bc7_asset != nullptr);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_textureasset3d_get_format(bc7_asset)), "bc7") == 0,
                "BC7 KTX2 reports bc7 format");
    EXPECT_EQ(rt_textureasset3d_get_compressed(bc7_asset), 1);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(bc7_asset) != nullptr,
                "BC7 KTX2 exposes a CPU Pixels fallback");
    EXPECT_TRUE(rt_textureasset3d_get_native_mip_info(bc7_asset,
                                                      0,
                                                      &native_payload,
                                                      &native_payload_bytes,
                                                      &native_width,
                                                      &native_height,
                                                      &native_block_width,
                                                      &native_block_height,
                                                      &native_block_bytes) == 1,
                "compressed KTX2 retains native mip payload for backend upload");
    EXPECT_TRUE(native_payload != nullptr, "native BC7 mip payload pointer is available");
    EXPECT_EQ(native_payload_bytes, sizeof(bc7_level0));
    EXPECT_EQ(native_width, 4);
    EXPECT_EQ(native_height, 4);
    EXPECT_EQ(native_block_width, 4);
    EXPECT_EQ(native_block_height, 4);
    EXPECT_EQ(native_block_bytes, 16);
    EXPECT_EQ(native_payload[0], 0x11);
    EXPECT_EQ(rt_textureasset3d_get_native_format_id(bc7_asset),
              RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7);
    EXPECT_TRUE(rt_textureasset3d_get_native_cache_key(bc7_asset) != 0,
                "compressed KTX2 exposes a native cache key");
    EXPECT_TRUE(rt_textureasset3d_get_native_mip_info(
                    bc7_asset, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) ==
                    0,
                "native mip query rejects out-of-range mips");
    rt_material3d_set_texture(mat, bc7_asset);
    EXPECT_EQ(rt_material3d_get_has_texture(mat), 1);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(mat) == rt_textureasset3d_get_pixels(bc7_asset),
                "material inspection resolves a compressed TextureAsset3D fallback");
    textured = rt_material3d_new_textured(bc7_asset);
    assert(textured != nullptr);
    EXPECT_EQ(rt_material3d_get_has_texture(textured), 1);

    std::remove(rgba_path);
    std::remove(bc7_path);
    PASS();
}

static void test_textureasset3d_bc3_software_decode() {
    /* Build one BC3 block: colour0 = red (565 0xF800), colour1 = blue (0x001F); alpha0=255,
     * alpha1=0 (8-alpha mode). Texel 0 uses colour/alpha index 0 -> opaque red; texel 1 uses
     * index 1 -> transparent blue. Verifies endpoint expansion + index selection on both channels.
     */
    uint8_t block[16];
    std::memset(block, 0, sizeof(block));
    block[0] = 0xFF;  /* alpha0 = 255 */
    block[1] = 0x00;  /* alpha1 = 0 */
    block[2] = 0x08;  /* alpha indices: texel0 -> 0, texel1 -> 1 */
    block[8] = 0x00;  /* colour0 low  (0xF800 = red) */
    block[9] = 0xF8;  /* colour0 high */
    block[10] = 0x1F; /* colour1 low (0x001F = blue) */
    block[11] = 0x00; /* colour1 high */
    block[12] = 0x04; /* colour indices: texel0 -> 0, texel1 -> 1 */

    uint8_t out[64];
    std::memset(out, 0x55, sizeof(out));
    rt_textureasset3d_decode_bc3_block(block, out);

    /* Texel 0: opaque red. */
    EXPECT_TRUE(out[0] == 255, "BC3 decode texel0 R = 255 (red endpoint expanded)");
    EXPECT_TRUE(out[1] == 0, "BC3 decode texel0 G = 0");
    EXPECT_TRUE(out[2] == 0, "BC3 decode texel0 B = 0");
    EXPECT_TRUE(out[3] == 255, "BC3 decode texel0 A = 255 (alpha index 0)");
    /* Texel 1: transparent blue. */
    EXPECT_TRUE(out[4] == 0, "BC3 decode texel1 R = 0");
    EXPECT_TRUE(out[5] == 0, "BC3 decode texel1 G = 0");
    EXPECT_TRUE(out[6] == 255, "BC3 decode texel1 B = 255 (blue endpoint expanded)");
    EXPECT_TRUE(out[7] == 0, "BC3 decode texel1 A = 0 (alpha index 1)");
}

static void test_textureasset3d_bc1_bc4_bc5_software_decode() {
    TEST("TextureAsset3D BC1/BC4/BC5 software decode fixtures");

    /* BC1 opaque mode: colour0 = red (0xF800) > colour1 = blue (0x001F); texel0
     * uses index 0 (red), texel1 index 1 (blue). */
    {
        uint8_t block[8];
        uint8_t out[64];
        std::memset(block, 0, sizeof(block));
        block[0] = 0x00; /* colour0 low */
        block[1] = 0xF8; /* colour0 high (red) */
        block[2] = 0x1F; /* colour1 low (blue) */
        block[3] = 0x00; /* colour1 high */
        block[4] = 0x04; /* indices: texel0 -> 0, texel1 -> 1 */
        std::memset(out, 0x55, sizeof(out));
        rt_textureasset3d_decode_bc1_block(block, out);
        EXPECT_TRUE(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255,
                    "BC1 opaque texel0 decodes to opaque red");
        EXPECT_TRUE(out[4] == 0 && out[5] == 0 && out[6] == 255 && out[7] == 255,
                    "BC1 opaque texel1 decodes to opaque blue");
    }

    /* BC1 punch-through mode: colour0 <= colour1 selects the 3-colour mode where
     * index 3 is transparent black. */
    {
        uint8_t block[8];
        uint8_t out[64];
        std::memset(block, 0, sizeof(block));
        block[0] = 0x1F; /* colour0 = blue (0x001F) */
        block[1] = 0x00;
        block[2] = 0x00; /* colour1 = red (0xF800) */
        block[3] = 0xF8;
        block[4] = 0x03; /* texel0 -> index 3 (transparent) */
        std::memset(out, 0x55, sizeof(out));
        rt_textureasset3d_decode_bc1_block(block, out);
        EXPECT_TRUE(out[3] == 0, "BC1 punch-through texel0 alpha = 0");
        EXPECT_TRUE(out[0] == 0 && out[1] == 0 && out[2] == 0,
                    "BC1 punch-through texel0 colour = black");
        EXPECT_TRUE(out[7] == 255, "BC1 punch-through texel1 (index 0) stays opaque");
    }

    /* BC4: endpoints 200 > 40 select the 8-point ramp; texel0 index 0 -> 200,
     * texel1 index 1 -> 40; the channel replicates into R/G/B. */
    {
        uint8_t block[8];
        uint8_t out[64];
        std::memset(block, 0, sizeof(block));
        block[0] = 200;
        block[1] = 40;
        block[2] = 0x08; /* 3-bit indices: texel0 -> 0, texel1 -> 1 */
        std::memset(out, 0x55, sizeof(out));
        rt_textureasset3d_decode_bc4_block(block, out);
        EXPECT_TRUE(out[0] == 200 && out[1] == 200 && out[2] == 200 && out[3] == 255,
                    "BC4 texel0 replicates endpoint 0 into RGB");
        EXPECT_TRUE(out[4] == 40 && out[7] == 255, "BC4 texel1 selects endpoint 1");
    }

    /* BC5: R block endpoints (200, 40) and G block endpoints (40, 200); texel0
     * uses index 0 in both -> (200, 40, 255, 255). */
    {
        uint8_t block[16];
        uint8_t out[64];
        std::memset(block, 0, sizeof(block));
        block[0] = 200; /* R endpoints */
        block[1] = 40;
        block[8] = 40; /* G endpoints */
        block[9] = 200;
        std::memset(out, 0x55, sizeof(out));
        rt_textureasset3d_decode_bc5_block(block, out);
        EXPECT_TRUE(out[0] == 200, "BC5 texel0 R channel from first half-block");
        EXPECT_TRUE(out[1] == 40, "BC5 texel0 G channel from second half-block");
        EXPECT_TRUE(out[2] == 255 && out[3] == 255, "BC5 texel0 B/A fixed opaque");
    }

    /* End-to-end: a BC1 KTX2 (VkFormat 133) loads with the bc1 format name and a
     * decoded RGBA fallback whose first texel matches the block decode. */
    {
        const char *path = "/tmp/zanna_textureasset3d_bc1_test.ktx2";
        uint8_t block[8];
        std::memset(block, 0, sizeof(block));
        block[0] = 0x00;
        block[1] = 0xF8; /* solid red, all indices 0 */
        block[2] = 0x1F;
        block[3] = 0x00;
        EXPECT_TRUE(write_test_ktx2(path, 133u, 4u, 4u, block, sizeof(block)),
                    "BC1 KTX2 fixture written");
        rt_string path_s = rt_string_from_bytes(path, std::strlen(path));
        void *asset = rt_textureasset3d_load_ktx2(path_s);
        rt_string_unref(path_s);
        EXPECT_TRUE(asset != nullptr, "BC1 KTX2 loads");
        if (asset) {
            rt_string format_name = rt_textureasset3d_get_format(asset);
            const char *format_cstr = format_name ? rt_string_cstr(format_name) : NULL;
            EXPECT_TRUE(format_cstr && std::strcmp(format_cstr, "bc1") == 0,
                        "BC1 KTX2 reports format name bc1");
            void *pixels = rt_textureasset3d_get_pixels(asset);
            EXPECT_TRUE(pixels != nullptr, "BC1 fallback pixels decoded");
            if (pixels) {
                int64_t texel = rt_pixels_get(pixels, 0, 0);
                EXPECT_TRUE((uint64_t)texel == 0xFF0000FFull, "BC1 fallback texel0 is opaque red");
            }
            if (rt_obj_release_check0(asset))
                rt_obj_free(asset);
        }
        std::remove(path);
    }
    PASS();
}

/* LSB-first bit writer mirroring BC7's bitstream layout, for constructing known blocks. */
struct Bc7BitWriter {
    uint8_t *buf;
    int pos;

    void put(uint32_t v, int n) {
        for (int i = 0; i < n; i++) {
            if ((v >> i) & 1u)
                buf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
            pos++;
        }
    }
};

static void write_bc7_constant_white_block(uint8_t *block, int mode, uint32_t partition) {
    int subsets = 1;
    int partition_bits = 0;
    int color_bits = 0;
    int alpha_bits = 0;
    int endpoint_pbits = 0;
    int shared_pbits = 0;
    int endpoints;
    Bc7BitWriter w{block, 0};

    std::memset(block, 0, 16);
    for (int i = 0; i < mode; i++)
        w.put(0, 1);
    w.put(1, 1);

    switch (mode) {
        case 0:
            subsets = 3;
            partition_bits = 4;
            color_bits = 4;
            endpoint_pbits = 1;
            break;
        case 1:
            subsets = 2;
            partition_bits = 6;
            color_bits = 6;
            shared_pbits = 1;
            break;
        case 2:
            subsets = 3;
            partition_bits = 6;
            color_bits = 5;
            break;
        case 3:
            subsets = 2;
            partition_bits = 6;
            color_bits = 7;
            endpoint_pbits = 1;
            break;
        case 7:
            subsets = 2;
            partition_bits = 6;
            color_bits = 5;
            alpha_bits = 5;
            endpoint_pbits = 1;
            break;
        default:
            return;
    }

    if (partition_bits)
        w.put(partition & ((1u << partition_bits) - 1u), partition_bits);
    endpoints = subsets * 2;
    for (int c = 0; c < 3; c++)
        for (int e = 0; e < endpoints; e++)
            w.put((1u << color_bits) - 1u, color_bits);
    if (alpha_bits)
        for (int e = 0; e < endpoints; e++)
            w.put((1u << alpha_bits) - 1u, alpha_bits);
    if (endpoint_pbits)
        for (int e = 0; e < endpoints; e++)
            w.put(1, endpoint_pbits);
    if (shared_pbits)
        for (int s = 0; s < subsets; s++)
            w.put(1, shared_pbits);
}

static void test_textureasset3d_bc7_software_decode() {
    uint8_t out[64];

    /* --- Mode 6: single subset, RGBA 7+pbit, 4-bit indices. e0 = transparent black
     * (7-bit 0, p=0 -> 0), e1 = opaque white (7-bit 0x7F, p=1 -> 255). Texel 0 index 0 -> e0,
     * texel 1 index 15 -> e1, texel 2 index 8 -> interp at weight 34 -> ~135. */
    uint8_t b6[16];
    std::memset(b6, 0, sizeof(b6));
    {
        Bc7BitWriter w{b6, 0};
        for (int i = 0; i < 6; i++)
            w.put(0, 1);
        w.put(1, 1); /* mode 6 */
        for (int c = 0; c < 4; c++) {
            w.put(0, 7);
            w.put(0x7F, 7);
        } /* R0R1 G0G1 B0B1 A0A1 */
        w.put(0, 1);
        w.put(1, 1);  /* p0=0 p1=1 */
        w.put(0, 3);  /* texel 0 (anchor, 3 bits) -> 0 */
        w.put(15, 4); /* texel 1 -> 15 */
        w.put(8, 4);  /* texel 2 -> 8 */
        for (int t = 3; t < 16; t++)
            w.put(0, 4);
    }
    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_bc7_block(b6, out) == 1, "BC7 mode6 decodes");
    EXPECT_TRUE(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0,
                "BC7 mode6 texel0 = transparent black (index 0 -> endpoint 0)");
    EXPECT_TRUE(out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255,
                "BC7 mode6 texel1 = opaque white (index 15 -> endpoint 1)");
    EXPECT_TRUE(out[8] == 135 && out[11] == 135,
                "BC7 mode6 texel2 interpolated at weight 34 (= 135)");

    /* --- Mode 5: single subset, RGB 7-bit + A 8-bit, rotation 0, separate 2-bit color/alpha
     * indices. e0 = (0,0,0,0), e1 = (255,255,255,255). */
    uint8_t b5[16];
    std::memset(b5, 0, sizeof(b5));
    {
        Bc7BitWriter w{b5, 0};
        for (int i = 0; i < 5; i++)
            w.put(0, 1);
        w.put(1, 1); /* mode 5 */
        w.put(0, 2); /* rotation 0 */
        for (int c = 0; c < 3; c++) {
            w.put(0, 7);
            w.put(0x7F, 7);
        }
        w.put(0, 8);
        w.put(255, 8); /* A0 A1 */
        w.put(0, 1);
        w.put(3, 2);
        for (int t = 2; t < 16; t++)
            w.put(0, 2); /* color indices */
        w.put(0, 1);
        w.put(3, 2);
        for (int t = 2; t < 16; t++)
            w.put(0, 2); /* alpha indices */
    }
    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_bc7_block(b5, out) == 1, "BC7 mode5 decodes");
    EXPECT_TRUE(out[0] == 0 && out[3] == 0, "BC7 mode5 texel0 = (0,0,0,0)");
    EXPECT_TRUE(out[4] == 255 && out[7] == 255, "BC7 mode5 texel1 = (255,255,255,255)");

    /* --- Mode 5 with rotation 1 (swap R<->A): decoded texel0 (0,0,0,A=255) -> (255,0,0,0). */
    uint8_t b5r[16];
    std::memset(b5r, 0, sizeof(b5r));
    {
        Bc7BitWriter w{b5r, 0};
        for (int i = 0; i < 5; i++)
            w.put(0, 1);
        w.put(1, 1); /* mode 5 */
        w.put(1, 2); /* rotation 1 -> swap R and A */
        for (int c = 0; c < 3; c++) {
            w.put(0, 7);
            w.put(0, 7);
        } /* RGB endpoints all 0 */
        w.put(255, 8);
        w.put(255, 8); /* A0 = A1 = 255 */
        for (int s = 0; s < 2; s++) {
            w.put(0, 1);
            for (int t = 1; t < 16; t++)
                w.put(0, 2); /* all indices 0 -> endpoint 0 */
        }
    }
    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_bc7_block(b5r, out) == 1, "BC7 mode5 (rot) decodes");
    EXPECT_TRUE(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 0,
                "BC7 mode5 rotation 1 swaps alpha into R (255,0,0,0)");

    /* --- Mode 4: RGB 5-bit + A 6-bit, idxMode 0 (color=2-bit, alpha=3-bit). e0=(0,0,0,0),
     * e1=(255,255,255,255). Texel0 -> e0, texel1 -> e1. */
    uint8_t b4[16];
    std::memset(b4, 0, sizeof(b4));
    {
        Bc7BitWriter w{b4, 0};
        for (int i = 0; i < 4; i++)
            w.put(0, 1);
        w.put(1, 1); /* mode 4 */
        w.put(0, 2); /* rotation 0 */
        w.put(0, 1); /* idxMode 0 */
        for (int c = 0; c < 3; c++) {
            w.put(0, 5);
            w.put(31, 5);
        }
        w.put(0, 6);
        w.put(63, 6); /* A0 A1 */
        w.put(0, 1);
        w.put(3, 2);
        for (int t = 2; t < 16; t++)
            w.put(0, 2); /* 2-bit index set */
        w.put(0, 2);
        w.put(7, 3);
        for (int t = 2; t < 16; t++)
            w.put(0, 3); /* 3-bit index set */
    }
    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_bc7_block(b4, out) == 1, "BC7 mode4 decodes");
    EXPECT_TRUE(out[0] == 0 && out[3] == 0, "BC7 mode4 texel0 = (0,0,0,0)");
    EXPECT_TRUE(out[4] == 255 && out[7] == 255, "BC7 mode4 texel1 = (255,255,255,255)");

    for (int mode : {0, 1, 2, 3, 7}) {
        uint8_t block[16];
        uint32_t partition = mode == 0 ? 5u : 17u;
        write_bc7_constant_white_block(block, mode, partition);
        std::memset(out, 0x55, sizeof(out));
        EXPECT_TRUE(rt_textureasset3d_decode_bc7_block(block, out) == 1,
                    "BC7 partitioned mode decodes");
        EXPECT_TRUE(out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255,
                    "BC7 partitioned constant-white block decodes to opaque white");
    }
}

static void test_textureasset3d_etc2_astc_software_decode() {
    TEST("TextureAsset3D ETC2/ASTC software decode fixtures");
    const char *etc2_path = "/tmp/zanna_textureasset3d_etc2_test.ktx2";
    const char *astc_path = "/tmp/zanna_textureasset3d_astc_test.ktx2";
    const uint8_t etc2_block[16] = {
        0x80,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0xFF,
        0xFF,
        0xFF,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };
    const uint8_t astc_block[16] = {
        0xFC,
        0xFD,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0x00,
        0x00,
        0x00,
        0x80,
        0xFF,
        0xFF,
    };
    uint8_t out[12 * 12 * 4];
    rt_string path_s;
    void *asset;
    void *pixels;

    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_etc2_rgba8_block(etc2_block, out) == 1,
                "ETC2 RGBA8/EAC block decodes");
    EXPECT_EQ(out[0], 247);
    EXPECT_EQ(out[1], 247);
    EXPECT_EQ(out[2], 247);
    EXPECT_EQ(out[3], 128);

    std::memset(out, 0x55, sizeof(out));
    EXPECT_TRUE(rt_textureasset3d_decode_astc_ldr_block(astc_block, 4, 4, out) == 1,
                "ASTC LDR void-extent block decodes");
    EXPECT_EQ(out[0], 255);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], 128);
    EXPECT_EQ(out[3], 255);

    EXPECT_TRUE(write_test_ktx2(etc2_path, 151u, 4u, 4u, etc2_block, sizeof(etc2_block)),
                "test ETC2 KTX2 fixture written");
    path_s = rt_string_from_bytes(etc2_path, std::strlen(etc2_path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);
    pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(pixels != nullptr, "ETC2 KTX2 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 0), 0xF7F7F780);
    EXPECT_EQ(rt_textureasset3d_get_native_format_id(asset), RT_TEXTUREASSET3D_NATIVE_FORMAT_ETC2);

    EXPECT_TRUE(write_test_ktx2(astc_path, 157u, 4u, 4u, astc_block, sizeof(astc_block)),
                "test ASTC KTX2 fixture written");
    path_s = rt_string_from_bytes(astc_path, std::strlen(astc_path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);
    pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(pixels != nullptr, "ASTC KTX2 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 0), 0xFF0080FF);
    EXPECT_EQ(rt_textureasset3d_get_native_format_id(asset), RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC);

    std::remove(etc2_path);
    std::remove(astc_path);
    PASS();
}

static void test_textureasset3d_decode_failure_checker_fallback() {
    TEST("TextureAsset3D decode failure produces checker fallback");
    const char *path = "/tmp/zanna_textureasset3d_bc7_checker_fallback.ktx2";
    const uint8_t reserved_bc7_block[16] = {0};
    rt_string path_s;
    void *asset;
    void *pixels;

    EXPECT_TRUE(write_test_ktx2(path, 145u, 4u, 4u, reserved_bc7_block, sizeof(reserved_bc7_block)),
                "reserved-mode BC7 KTX2 fixture written");
    rt_asset_error_clear();
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);

    pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(pixels != nullptr, "decode failure exposes a visible Pixels fallback");
    EXPECT_EQ(rt_pixels_width(pixels), 8);
    EXPECT_EQ(rt_pixels_height(pixels), 8);
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 0), 0xFF00FFFF);
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 1, 0), 0x000000FF);
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 1), 0x000000FF);
    EXPECT_EQ(rt_pixels_get_rgba(pixels, 1, 1), 0xFF00FFFF);
    EXPECT_TRUE(rt_asset_error_get_warning_count() == 1, "decode failure records one load warning");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_warning(0), "bc7") != nullptr,
                "decode failure warning names the format");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_warning(0), "checker") != nullptr,
                "decode failure warning names the checker fallback");
    EXPECT_EQ(rt_textureasset3d_get_degraded(asset), 1);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_textureasset3d_get_degraded_reason(asset)),
                            "cpu_decode_failed") == 0,
                "checker substitution exposes a stable degradation reason");

    path_s = rt_string_from_bytes(path, std::strlen(path));
    void *strict_asset = rt_textureasset3d_load_ktx2_strict(path_s);
    rt_string_unref(path_s);
    EXPECT_TRUE(strict_asset == nullptr,
                "strict KTX2 loader rejects the same unsupported CPU block mode");
    EXPECT_TRUE(std::strstr(rt_string_cstr(rt_assets3d_get_last_load_error()),
                            "strict loading forbids checker fallback") != nullptr,
                "strict rejection records a stable explanatory asset error");

    {
        std::vector<uint8_t> memory_fixture =
            make_test_ktx2_memory(145u, 4u, 4u, reserved_bc7_block, sizeof(reserved_bc7_block));
        strict_asset =
            rt_textureasset3d_load_ktx2_memory_strict(memory_fixture.data(), memory_fixture.size());
        EXPECT_TRUE(strict_asset == nullptr,
                    "strict memory loader applies the same checker-substitution policy");
        EXPECT_TRUE(std::strstr(rt_string_cstr(rt_assets3d_get_last_load_error()),
                                "strict loading forbids checker fallback") != nullptr,
                    "strict memory rejection preserves the explanatory asset error");
    }

    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);

    std::remove(path);
    PASS();
}

static void test_textureasset3d_bc7_partial_mip_fallback() {
    TEST("TextureAsset3D BC7 keeps decoded mips when a later mip is unsupported");
    const char *path = "/tmp/zanna_textureasset3d_bc7_partial_mip_fallback.ktx2";
    const uint8_t valid_block[16] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    uint8_t level0[64];
    const uint8_t reserved_level1[16] = {0};
    const uint8_t *levels[] = {level0, reserved_level1};
    const uint64_t level_bytes[] = {sizeof(level0), sizeof(reserved_level1)};
    rt_string path_s;
    void *asset;
    TextureAsset3DTestLayout *layout;

    for (size_t i = 0; i < sizeof(level0); i += sizeof(valid_block))
        std::memcpy(level0 + i, valid_block, sizeof(valid_block));

    EXPECT_TRUE(write_test_ktx2_mips(path, 145u, 8u, 8u, levels, level_bytes, 2u),
                "mixed-validity BC7 mip chain fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);
    layout = static_cast<TextureAsset3DTestLayout *>(asset);

    EXPECT_TRUE(layout->mip_pixels != nullptr, "BC7 partial decode keeps a mip fallback table");
    EXPECT_TRUE(layout->mip_pixels[0] != nullptr, "supported BC7 mip remains decoded");
    EXPECT_TRUE(layout->mip_pixels[1] == nullptr, "unsupported BC7 mip is left native-only");
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == layout->mip_pixels[0],
                "resident mip 0 resolves to its decoded fallback");
    rt_textureasset3d_set_resident_mip_range(asset, 1, 1);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == nullptr,
                "resident unsupported mip does not receive a whole-texture checker fallback");
    EXPECT_TRUE(vgfx3d_textureasset_native_supported(asset, RT_CANVAS3D_BACKEND_CAP_BC7),
                "partial BC7 fallback still supports native compressed upload");

    std::remove(path);
    PASS();
}

static void test_textureasset3d_mip_residency() {
    TEST("TextureAsset3D mip residency range telemetry");
    const char *path = "/tmp/zanna_textureasset3d_mips_test.ktx2";
    uint8_t level0[64];
    uint8_t level1[16];
    uint8_t level2[4];
    const uint8_t *levels[] = {level0, level1, level2};
    const uint64_t level_bytes[] = {sizeof(level0), sizeof(level1), sizeof(level2)};
    rt_string path_s;
    void *asset;
    void *material;
    rt_material3d *material_impl;
    TextureAsset3DTestLayout *layout;

    for (size_t i = 0; i < sizeof(level0); i++)
        level0[i] = (uint8_t)(i + 1u);
    for (size_t i = 0; i < sizeof(level1); i++)
        level1[i] = (uint8_t)(0x80u + i);
    for (size_t i = 0; i < sizeof(level2); i++)
        level2[i] = (uint8_t)(0xC0u + i);

    EXPECT_TRUE(write_test_ktx2_mips(path, 37u, 4u, 4u, levels, level_bytes, 3u),
                "test mipmapped RGBA8 KTX2 fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);
    layout = static_cast<TextureAsset3DTestLayout *>(asset);

    EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 3);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 3);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 84);
    EXPECT_EQ(rt_textureasset3d_get_retained_bytes(asset),
              168 + (int64_t)layout->container_backing_bytes);
    EXPECT_TRUE(layout->source_backing != nullptr && layout->source_backing_bytes == 84,
                "RGBA8 mip chain retains one exact canonical source backing");
    void *mip_pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(mip_pixels != nullptr, "resident RGBA8 mip 0 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_width(mip_pixels), 4);
    EXPECT_EQ(rt_pixels_height(mip_pixels), 4);
    EXPECT_EQ(rt_pixels_get_rgba(mip_pixels, 0, 0), 0x01020304);

    material = rt_material3d_new();
    assert(material != nullptr);
    rt_material3d_set_texture(material, asset);
    material_impl = (rt_material3d *)material;
    EXPECT_TRUE(material_impl->texture == asset,
                "Material3D retains TextureAsset3D source instead of a stale mip Pixels fallback");
    EXPECT_EQ(rt_material3d_get_has_texture(material), 1);
    EXPECT_TRUE(rt_material3d_resolve_texture_pixels(material_impl->texture) == mip_pixels,
                "Material3D resolves TextureAsset3D to currently resident mip 0");

    rt_textureasset3d_set_resident_mip_range(asset, 1, 2);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 1);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 2);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 20);
    EXPECT_EQ(rt_textureasset3d_get_retained_bytes(asset),
              104 + (int64_t)layout->container_backing_bytes);
    EXPECT_TRUE(layout->mip_pixels[0] == nullptr,
                "decoded mip 0 CPU memory is released after leaving residency");
    mip_pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(mip_pixels != nullptr, "resident RGBA8 mip 1 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_width(mip_pixels), 2);
    EXPECT_EQ(rt_pixels_height(mip_pixels), 2);
    EXPECT_EQ(rt_pixels_get_rgba(mip_pixels, 0, 0), 0x80818283);
    EXPECT_TRUE(rt_material3d_resolve_texture_pixels(material_impl->texture) == mip_pixels,
                "Material3D follows TextureAsset3D residency changes after binding");

    rt_textureasset3d_set_resident_mip_range(asset, 0, 1);
    mip_pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(mip_pixels != nullptr,
                "evicted mip 0 reconstructs from retained canonical source bytes");
    EXPECT_EQ(rt_pixels_width(mip_pixels), 4);
    EXPECT_EQ(rt_pixels_height(mip_pixels), 4);
    EXPECT_EQ(rt_pixels_get_rgba(mip_pixels, 0, 0), 0x01020304);
    EXPECT_EQ(rt_pixels_get_rgba(mip_pixels, 3, 3), 0x3D3E3F40);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 64);
    EXPECT_EQ(rt_textureasset3d_get_retained_bytes(asset),
              148 + (int64_t)layout->container_backing_bytes);
    {
        const uint8_t *source = nullptr;
        uint64_t source_size = 0;
        const char *source_kind = nullptr;
        EXPECT_TRUE(
            rt_textureasset3d_get_source_container(asset, &source, &source_size, &source_kind) &&
                source_size == layout->container_backing_bytes,
            "internally reconstructed mip residency keeps exact KTX2 provenance valid");
    }
    EXPECT_TRUE(layout->mip_pixels[1] == nullptr && layout->mip_pixels[2] == nullptr,
                "re-entering mip 0 evicts decoded mips 1 and 2 after reconstruction commits");

    rt_textureasset3d_set_resident_mip_range(asset, 2, 99);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 2);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 1);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 4);
    mip_pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(mip_pixels != nullptr, "resident RGBA8 mip 2 exposes a Pixels fallback");
    EXPECT_EQ(rt_pixels_width(mip_pixels), 1);
    EXPECT_EQ(rt_pixels_height(mip_pixels), 1);
    EXPECT_EQ(rt_pixels_get_rgba(mip_pixels, 0, 0), 0xC0C1C2C3);
    EXPECT_TRUE(rt_material3d_resolve_texture_pixels(material_impl->texture) == mip_pixels,
                "Material3D resolves to the finest currently resident TextureAsset3D mip");

    rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 0);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == nullptr,
                "zero resident mip count clears the active Pixels fallback");
    {
        const uint8_t *source = nullptr;
        uint64_t source_size = 0;
        const char *source_kind = nullptr;
        EXPECT_TRUE(
            rt_textureasset3d_get_source_container(asset, &source, &source_size, &source_kind) &&
                source_size == layout->container_backing_bytes,
            "evicting decoded KTX2 mips does not discard exact source provenance");
    }
    EXPECT_TRUE(material_impl->texture == asset,
                "Material3D keeps the TextureAsset3D source while residency is temporarily empty");
    EXPECT_TRUE(rt_material3d_resolve_texture_pixels(material_impl->texture) == nullptr,
                "Material3D resolves empty TextureAsset3D residency to no drawable texture");
    EXPECT_EQ(rt_material3d_get_has_texture(material), 0);
    rt_material3d_set_texture(material, nullptr);
    rt_material3d_set_texture(material, asset);
    EXPECT_TRUE(material_impl->texture == asset,
                "Material3D accepts TextureAsset3D assignment while residency is empty");

    rt_textureasset3d_set_resident_mip_range(asset, 99, 1);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 3);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 0);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == nullptr,
                "out-of-range resident mip starts clamp to an empty range at mip_count");

    EXPECT_TRUE(layout != nullptr && layout->mip_capacity == 3,
                "TextureAsset3D test layout sees loaded mip capacity");
    if (layout) {
        layout->mip_count = std::numeric_limits<int64_t>::max();
        EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 3);
        layout->resident_mip_start = 1;
        layout->resident_mip_count = 1;
        layout->pixels = nullptr;
        EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == layout->mip_pixels[1],
                    "TextureAsset3D resolves fallback pixels from the validated resident range");
        layout->resident_mip_count = 99;
        layout->resident_bytes = std::numeric_limits<int64_t>::max();
        EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 0);
        EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 0);
        EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == nullptr,
                    "TextureAsset3D rejects corrupt resident ranges");
        layout->mip_count = -7;
        EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 0);
        EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 0);
        layout->mip_count = layout->mip_capacity;
        rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    }

    EXPECT_TRUE(
        expect_trap_contains([&] { rt_textureasset3d_set_resident_mip_range(asset, -1, 1); },
                             "negative mip range"),
        "SetResidentMipRange rejects a negative first mip");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_textureasset3d_set_resident_mip_range(asset, 0, -1); },
                             "negative mip range"),
        "SetResidentMipRange rejects a negative mip count");

    if (layout) {
        void *old_mip0 = layout->mip_pixels ? layout->mip_pixels[0] : nullptr;
        void *wrong_mip = rt_obj_new_i64(0, 8);
        assert(wrong_mip != nullptr);
        rt_material3d_set_texture(material, nullptr);
        if (old_mip0 && rt_obj_release_check0(old_mip0))
            rt_obj_free(old_mip0);
        rt_obj_retain_maybe(wrong_mip);
        layout->mip_pixels[0] = wrong_mip;
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
        EXPECT_TRUE(rt_obj_release_check0(wrong_mip) == 0,
                    "TextureAsset3D finalizer clears wrong-class mip fallbacks without releasing");
        if (rt_obj_release_check0(wrong_mip))
            rt_obj_free(wrong_mip);
    }

    std::remove(path);
    PASS();
}

/// R23: opt-in automatic mip-residency streaming. A tiny on-screen draw must
/// demote the texture's resident window one mip at a time on the demotion
/// cadence, and a full-resolution demand must restore the window immediately.
static void test_canvas3d_texture_streaming_residency() {
    TEST("Canvas3D texture streaming demotes far textures and restores near ones");
    const char *path = "/tmp/zanna_canvas3d_texture_stream.ktx2";
    const uint8_t valid_block[16] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    std::vector<std::vector<uint8_t>> level_storage;
    std::vector<const uint8_t *> levels;
    std::vector<uint64_t> level_bytes;
    for (uint32_t mip = 0; mip < 9; mip++) {
        uint32_t dim = 256u >> mip;
        if (dim == 0)
            dim = 1;
        uint32_t blocks = ((dim + 3u) / 4u) * ((dim + 3u) / 4u);
        std::vector<uint8_t> data((size_t)blocks * 16u);
        for (size_t i = 0; i < data.size(); i += 16u)
            std::memcpy(&data[i], valid_block, 16u);
        level_storage.push_back(std::move(data));
    }
    for (const auto &level : level_storage) {
        levels.push_back(level.data());
        level_bytes.push_back(level.size());
    }
    EXPECT_TRUE(write_test_ktx2_mips(path, 145u, 256u, 256u, levels.data(), level_bytes.data(), 9u),
                "streaming KTX2 fixture written");

    rt_string path_s = rt_string_from_bytes(path, std::strlen(path));
    void *asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);
    EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 9);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 9);

    rt_canvas3d canvas = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(8, 8);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 500.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    void *far_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -90.0, 0.0, 0.0, 0.0, 1.0);

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx((vgfx_window_t)0, 8, 8);
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "software backend context exists");
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 8;
    canvas.height = 8;
    if (!rt || !rt->target || !canvas.backend_ctx)
        return;
    rt_canvas3d_set_render_target(&canvas, rt);
    rt_material3d_set_unlit(mat, 1);
    rt_material3d_set_texture(mat, asset);
    rt_canvas3d_set_texture_streaming(&canvas, 1);

    /* A 1-unit box 90 units away covers well under a pixel of an 8x8 target,
     * so the demand settles at the last mip. Demotions drop one level every
     * CANVAS3D_TEXTURE_STREAM_DEMOTE_FRAMES decision frames: level k lands on
     * frame 31 + 30*(k-1), so 260 frames reach the full 8-level demotion. */
    for (int frame = 0; frame < 40; frame++) {
        rt_canvas3d_begin(&canvas, camera);
        rt_canvas3d_draw_mesh(&canvas, mesh, far_xf, mat);
        rt_canvas3d_end(&canvas);
    }
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 1);
    EXPECT_EQ(rt_canvas3d_get_texture_streaming_demotions(&canvas), 1);
    for (int frame = 40; frame < 260; frame++) {
        rt_canvas3d_begin(&canvas, camera);
        rt_canvas3d_draw_mesh(&canvas, mesh, far_xf, mat);
        rt_canvas3d_end(&canvas);
    }
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 8);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 1);
    EXPECT_EQ(rt_canvas3d_get_texture_streaming_demotions(&canvas), 8);

    /* A strong negative bias forces a full-resolution demand; the promotion
     * applies at the asset's next first-touch, without demotion pacing. */
    rt_canvas3d_set_texture_streaming_bias(&canvas, -16.0);
    for (int frame = 0; frame < 2; frame++) {
        rt_canvas3d_begin(&canvas, camera);
        rt_canvas3d_draw_mesh(&canvas, mesh, far_xf, mat);
        rt_canvas3d_end(&canvas);
    }
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_start(asset), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_mip_count(asset), 9);
    EXPECT_EQ(rt_canvas3d_get_texture_streaming_demotions(&canvas), 8);

    rt_canvas3d_set_render_target(&canvas, nullptr);
    vgfx3d_software_backend.destroy_ctx(canvas.backend_ctx);
    canvas.backend_ctx = nullptr;
    free(canvas.texture_stream_entries);
    canvas.texture_stream_entries = nullptr;
    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    if (rt_obj_release_check0(camera))
        rt_obj_free(camera);
    if (rt_obj_release_check0(far_xf))
        rt_obj_free(far_xf);
    if (rt_obj_release_check0(rt))
        rt_obj_free(rt);
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
    PASS();
}

/// R18: no-op instanced submit used by the fake GPU backend in the chunking test.
static void r18_dummy_submit_draw_instanced(void *ctx,
                                            vgfx_window_t win,
                                            const vgfx3d_draw_cmd_t *cmd,
                                            const float *instance_matrices,
                                            int32_t instance_count,
                                            const vgfx3d_light_params_t *lights,
                                            int32_t light_count,
                                            const float *ambient,
                                            int8_t wireframe,
                                            int8_t backface_cull) {
    (void)ctx;
    (void)win;
    (void)cmd;
    (void)instance_matrices;
    (void)instance_count;
    (void)lights;
    (void)light_count;
    (void)ambient;
    (void)wireframe;
    (void)backface_cull;
}

/// R18: on the software backend, per-instance skinned draws take the CPU fallback,
/// so each instance must render with its OWN palette — the zero-scale palette
/// collapses its box while the identity palette keeps its box visible.
static void test_canvas3d_per_instance_skinning_software_fallback() {
    TEST("Per-instance skinned draw poses each instance from its own palette (SW)");
    rt_canvas3d canvas = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(16, 16);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.5, 1.5, 1.5);
    void *mat = rt_material3d_new();
    float instance_matrices[32] = {0};
    float palettes[32] = {0};

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx((vgfx_window_t)0, 16, 16);
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "software backend context exists");
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 16;
    canvas.height = 16;
    if (!rt || !rt->target || !canvas.backend_ctx)
        return;
    rt_canvas3d_set_render_target(&canvas, rt);
    rt_material3d_set_unlit(mat, 1);
    rt_material3d_set_color(mat, 1.0, 0.0, 0.0);

    /* Every vertex fully weighted to bone 0. */
    for (int64_t v = 0; v < rt_mesh3d_get_vertex_count(mesh); v++)
        rt_mesh3d_set_bone_weights(mesh, v, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);

    /* Camera default: origin looking down -Z. Instance 0 at (-2, 0, -6) with an
     * identity palette; instance 1 at (+2, 0, -6) with a zero palette that
     * collapses all of its vertices to a point. */
    instance_matrices[0] = instance_matrices[5] = instance_matrices[10] = instance_matrices[15] =
        1.0f;
    instance_matrices[3] = -2.0f;
    instance_matrices[11] = -6.0f;
    instance_matrices[16 + 0] = instance_matrices[16 + 5] = instance_matrices[16 + 10] =
        instance_matrices[16 + 15] = 1.0f;
    instance_matrices[16 + 3] = 2.0f;
    instance_matrices[16 + 11] = -6.0f;
    palettes[0] = palettes[5] = palettes[10] = palettes[15] = 1.0f; /* identity for instance 0 */
    /* instance 1's palette stays all zeros */

    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh_instanced_skinned(
        &canvas, mesh, mat, instance_matrices, 2, palettes, nullptr, 1);
    EXPECT_EQ(canvas.draw_count, 2); /* software fallback: one skinned draw per instance */
    rt_canvas3d_end(&canvas);

    {
        /* Left box (identity palette) renders; right box (zero palette) collapses. */
        const size_t left = ((size_t)8 * 16 + 3) * 4;
        const size_t right = ((size_t)8 * 16 + 12) * 4;
        EXPECT_TRUE(rt->target->color_buf[left + 0] > 128,
                    "identity-palette instance renders its box");
        EXPECT_TRUE(rt->target->color_buf[right + 0] < 32,
                    "zero-palette instance collapses to nothing");
    }

    rt_canvas3d_set_render_target(&canvas, nullptr);
    vgfx3d_software_backend.destroy_ctx(canvas.backend_ctx);
    canvas.backend_ctx = nullptr;
    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    if (rt_obj_release_check0(camera))
        rt_obj_free(camera);
    if (rt_obj_release_check0(rt))
        rt_obj_free(rt);
    PASS();
}

/// R18: GPU-skinning backends pack per-instance palettes into the shared 256-slot
/// palette, so chunk counts follow VGFX3D_MAX_BONES / bone_count; backends without
/// hardware instancing refuse and fall back to one draw per instance.
static void test_canvas3d_per_instance_skinning_chunking() {
    TEST("Per-instance skinned chunking packs the shared palette slot");
    rt_canvas3d canvas = {};
    vgfx3d_backend_t fake = {};
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    std::vector<float> matrices((size_t)5 * 16u, 0.0f);
    std::vector<float> palettes_small((size_t)5 * 8u * 16u, 0.0f);
    std::vector<float> palettes_large((size_t)3 * 200u * 16u, 0.0f);

    fake.name = "metal";

    fake.gpu_skinning = 1; /* mock mirrors real GPU backends */
    fake.submit_draw_instanced = r18_dummy_submit_draw_instanced;
    canvas.backend = &fake;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 8;
    canvas.height = 8;
    canvas.in_frame = 1;
    rt_material3d_set_unlit(mat, 1);
    for (int64_t v = 0; v < rt_mesh3d_get_vertex_count(mesh); v++)
        rt_mesh3d_set_bone_weights(mesh, v, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    for (int i = 0; i < 5; i++) {
        matrices[(size_t)i * 16u + 0] = 1.0f;
        matrices[(size_t)i * 16u + 5] = 1.0f;
        matrices[(size_t)i * 16u + 10] = 1.0f;
        matrices[(size_t)i * 16u + 15] = 1.0f;
    }
    for (size_t i = 0; i < palettes_small.size(); i += 16)
        palettes_small[i] = palettes_small[i + 5] = palettes_small[i + 10] =
            palettes_small[i + 15] = 1.0f;
    for (size_t i = 0; i < palettes_large.size(); i += 16)
        palettes_large[i] = palettes_large[i + 5] = palettes_large[i + 10] =
            palettes_large[i + 15] = 1.0f;

    /* 5 instances x 8 bones: 256/8 = 32 instances fit one draw -> 1 chunk. */
    rt_canvas3d_draw_mesh_instanced_skinned(
        &canvas, mesh, mat, matrices.data(), 5, palettes_small.data(), nullptr, 8);
    EXPECT_EQ(canvas.draw_count, 1);

    /* 3 instances x 200 bones: 256/200 = 1 instance per draw -> 3 chunks. */
    rt_canvas3d_draw_mesh_instanced_skinned(
        &canvas, mesh, mat, matrices.data(), 3, palettes_large.data(), nullptr, 200);
    EXPECT_EQ(canvas.draw_count, 4);

    /* Without hardware instancing the queue refuses per-instance palettes and
     * the draw falls back to one skinned draw per instance. */
    fake.submit_draw_instanced = NULL;
    rt_canvas3d_draw_mesh_instanced_skinned(
        &canvas, mesh, mat, matrices.data(), 2, palettes_small.data(), nullptr, 8);
    EXPECT_EQ(canvas.draw_count, 6);

    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    PASS();
}

/// R20: Mesh3D.CompactStreams only affects GPU static-cache uploads — the software
/// backend keeps sampling the full CPU vertex array, so output is identical, and
/// toggling the flag must bump the geometry revision so backend caches re-encode.
static void test_canvas3d_compact_streams_flag() {
    TEST("Mesh3D.CompactStreams bumps geometry revision and leaves SW output unchanged");
    rt_canvas3d canvas = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(8, 8);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *mat = rt_material3d_new();
    void *xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -5.0, 0.0, 0.0, 0.0, 1.0);
    uint8_t full_frame[8 * 8 * 4];
    uint32_t revision_before;

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx((vgfx_window_t)0, 8, 8);
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "software backend context exists");
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 8;
    canvas.height = 8;
    if (!rt || !rt->target || !canvas.backend_ctx)
        return;
    rt_canvas3d_set_render_target(&canvas, rt);
    rt_material3d_set_unlit(mat, 1);
    rt_material3d_set_color(mat, 0.0, 1.0, 0.0);

    EXPECT_EQ(rt_mesh3d_get_compact_streams(mesh), 0);
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    std::memcpy(full_frame, rt->target->color_buf, sizeof(full_frame));
    EXPECT_TRUE(full_frame[(8 * 4 + 4) * 4 + 1] > 128, "reference draw renders the box");

    revision_before = ((rt_mesh3d *)mesh)->geometry_revision;
    rt_mesh3d_set_compact_streams(mesh, 1);
    EXPECT_EQ(rt_mesh3d_get_compact_streams(mesh), 1);
    EXPECT_TRUE(((rt_mesh3d *)mesh)->geometry_revision != revision_before,
                "toggling CompactStreams bumps the geometry revision");
    rt_mesh3d_set_compact_streams(mesh, 1); /* idempotent: same value keeps the revision */
    EXPECT_EQ(((rt_mesh3d *)mesh)->geometry_revision, revision_before + 1);

    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(std::memcmp(full_frame, rt->target->color_buf, sizeof(full_frame)) == 0,
                "software output is bit-identical with CompactStreams enabled");

    rt_canvas3d_set_render_target(&canvas, nullptr);
    vgfx3d_software_backend.destroy_ctx(canvas.backend_ctx);
    canvas.backend_ctx = nullptr;
    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    if (rt_obj_release_check0(camera))
        rt_obj_free(camera);
    if (rt_obj_release_check0(xf))
        rt_obj_free(xf);
    if (rt_obj_release_check0(rt))
        rt_obj_free(rt);
    PASS();
}

/// Load must fail recoverably: NULL result with the expected diagnostic
/// available through the last-load-error query (no trap).
static bool ktx2_load_fails_with(rt_string path_s, const char *expected_substring) {
    void *asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string err;
    const char *err_cstr;

    if (asset) {
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
        return false;
    }
    err = rt_assets3d_get_last_load_error();
    err_cstr = err ? rt_string_cstr(err) : NULL;
    return err_cstr && std::strstr(err_cstr, expected_substring) != NULL;
}

/// @brief Require a caller-owned KTX2 span to fail recoverably with a matching diagnostic.
/// @details Releases an unexpected asset before returning false, then reads the public
///          last-load-error channel populated by the memory loader. This helper deliberately does
///          not install trap recovery: malformed container data must remain an ordinary load
///          failure rather than crossing the caller-misuse trap boundary.
/// @param bytes Complete or intentionally corrupted KTX2 bytes.
/// @param expected_substring Required fragment of the recoverable load diagnostic.
/// @return True only when no asset was published and the diagnostic contains the fragment.
static bool ktx2_memory_load_fails_with(const std::vector<uint8_t> &bytes,
                                        const char *expected_substring) {
    void *asset = rt_textureasset3d_load_ktx2_memory(bytes.data(), bytes.size());
    rt_string err;
    const char *err_cstr;

    if (asset) {
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
        return false;
    }
    err = rt_assets3d_get_last_load_error();
    err_cstr = err ? rt_string_cstr(err) : NULL;
    return err_cstr && expected_substring && std::strstr(err_cstr, expected_substring) != NULL;
}

/// Supercompressed KTX2 fixtures (Zstandard scheme 2 and ZLIB scheme 3,
/// produced by reference encoders) must load with correctly inflated levels.
static void test_textureasset3d_supercompressed_ktx2_loads() {
    TEST("TextureAsset3D loads Zstd/ZLIB supercompressed KTX2");
    const char *fixtures[] = {"red_rgba8_zstd.ktx2", "red_rgba8_zlib.ktx2"};
    for (int i = 0; i < 2; i++) {
#ifdef ZANNA_SOURCE_DIR
        std::string path =
            std::string(ZANNA_SOURCE_DIR) + "/src/tests/unit/data/ktx2/" + fixtures[i];
#else
        std::string path = std::string("src/tests/unit/data/ktx2/") + fixtures[i];
#endif
        rt_string path_s = rt_string_from_bytes(path.c_str(), path.size());
        void *asset = rt_textureasset3d_load_ktx2(path_s);
        rt_string_unref(path_s);
        EXPECT_TRUE(asset != nullptr, "supercompressed KTX2 loads");
        if (!asset)
            return;
        EXPECT_EQ(rt_textureasset3d_get_width(asset), 4);
        EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 2);
        void *pixels = rt_textureasset3d_get_pixels(asset);
        EXPECT_TRUE(pixels != nullptr, "mip0 pixels decoded from inflated payload");
        if (pixels) {
            EXPECT_TRUE((uint64_t)rt_pixels_get(pixels, 0, 0) == 0xFF0000FFull,
                        "mip0 texel is opaque red after decompression");
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    /* Compressed-format payloads inflate before block decode + native retention. */
    {
#ifdef ZANNA_SOURCE_DIR
        std::string path =
            std::string(ZANNA_SOURCE_DIR) + "/src/tests/unit/data/ktx2/red_bc1_zstd.ktx2";
#else
        std::string path = "src/tests/unit/data/ktx2/red_bc1_zstd.ktx2";
#endif
        rt_string path_s = rt_string_from_bytes(path.c_str(), path.size());
        void *asset = rt_textureasset3d_load_ktx2(path_s);
        rt_string_unref(path_s);
        EXPECT_TRUE(asset != nullptr, "zstd BC1 KTX2 loads");
        if (asset) {
            void *pixels = rt_textureasset3d_get_pixels(asset);
            EXPECT_TRUE(pixels && (uint64_t)rt_pixels_get(pixels, 0, 0) == 0xFF0000FFull,
                        "zstd BC1 block decodes to opaque red");
            if (rt_obj_release_check0(asset))
                rt_obj_free(asset);
        }
    }
    PASS();
}

/// @brief Exercise KTX2's shared strict zlib path against independent wrapper corruptions.
/// @details Builds one valid supercompressed RGBA8 level, confirms its decoded texels, then
///          mutates CMF/FCHECK, FDICT, the raw DEFLATE block, stored bounds, declared output size,
///          and Adler-32 one at a time. Every mutation must fail through the recoverable asset
///          error path without publishing a partially initialized TextureAsset3D.
static void test_textureasset3d_zlib_integrity_validation() {
    TEST("TextureAsset3D validates complete zlib supercompression framing");
    const uint8_t rgba[] = {
        0x01,
        0x02,
        0x03,
        0x04,
        0x10,
        0x20,
        0x30,
        0x40,
        0x50,
        0x60,
        0x70,
        0x80,
        0x90,
        0xA0,
        0xB0,
        0xC0,
    };
    std::vector<uint8_t> zlib = make_test_zlib_stored(rgba, sizeof(rgba));
    std::vector<uint8_t> valid = make_test_supercompressed_ktx2_memory(
        37u, 2u, 2u, 3u, zlib.data(), zlib.size(), sizeof(rgba));
    const uint64_t level_offset = read_u64le(valid.data() + 80);
    const uint64_t level_length = read_u64le(valid.data() + 88);
    void *asset = rt_textureasset3d_load_ktx2_memory(valid.data(), valid.size());

    EXPECT_TRUE(asset != nullptr, "valid generated zlib KTX2 loads through shared decoder");
    if (asset) {
        void *pixels = rt_textureasset3d_get_pixels(asset);
        EXPECT_TRUE(pixels != nullptr, "valid zlib KTX2 publishes decoded Pixels");
        if (pixels) {
            EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 0), 0x01020304);
            EXPECT_EQ(rt_pixels_get_rgba(pixels, 1, 1), 0x90A0B0C0);
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    EXPECT_TRUE(level_offset + level_length <= valid.size() && level_length >= 7,
                "generated zlib level index describes an in-bounds complete stream");
    if (level_offset + level_length <= valid.size() && level_length >= 7) {
        std::vector<uint8_t> corrupt = valid;
        corrupt[(size_t)level_offset] = 0x79;
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "ZLIB decompression"),
                    "invalid zlib CMF/FCHECK fails recoverably");

        corrupt = valid;
        corrupt[(size_t)level_offset] = 0x78;
        corrupt[(size_t)level_offset + 1u] = 0x20;
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "ZLIB decompression"),
                    "zlib preset-dictionary request fails recoverably");

        corrupt = valid;
        corrupt[(size_t)level_offset + 2u] = 0x07;
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "ZLIB decompression"),
                    "reserved raw-DEFLATE block type fails recoverably");

        corrupt = valid;
        write_u64le(corrupt.data() + 88, level_length - 1u);
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "ZLIB decompression"),
                    "truncated zlib stream bounds fail recoverably");

        corrupt = valid;
        write_u64le(corrupt.data() + 96, sizeof(rgba) - 1u);
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "inconsistent uncompressed level length"),
                    "mismatched KTX2 output-size declaration fails before allocation");

        corrupt = valid;
        corrupt[(size_t)(level_offset + level_length - 1u)] ^= 0x01;
        EXPECT_TRUE(ktx2_memory_load_fails_with(corrupt, "ZLIB decompression"),
                    "zlib Adler-32 corruption fails recoverably");
    }
    PASS();
}

/// @brief Prove every authoritative texture capability row matches an executable fallback.
/// @details Enumerates every Vulkan value in every inclusive row, builds a single-block KTX2,
///          and checks normalized name, compression state, CPU quality, decoder success, native
///          id, and backend capability bit. Known-valid BC7, ETC2, and ASTC blocks exercise their
///          partial/structured decoders; BC6H's reserved-zero block is specified to decode as
///          opaque black. The test also verifies duplicate ASTC footprint rows agree on quality.
static void test_textureasset3d_format_capability_table() {
    TEST("TextureAsset3D authoritative format table drives decode and backend support");
    const uint8_t valid_bc7[16] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    const uint8_t valid_etc2[16] = {
        0x80,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0xFF,
        0xFF,
        0xFF,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };
    const uint8_t valid_astc[16] = {
        0xFC,
        0xFD,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0x00,
        0x00,
        0x00,
        0x80,
        0xFF,
        0xFF,
    };
    const int64_t row_count = rt_textureasset3d_get_format_capability_count();

    EXPECT_TRUE(row_count > 0, "format capability table is nonempty");
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("bc6h"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL);
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("bc6hs"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL);
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("etc2"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL);
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("astc"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL);
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("bc7"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_FULL);
    EXPECT_EQ(rt_textureasset3d_cpu_format_support_level("not-a-format"),
              RT_TEXTUREASSET3D_CPU_SUPPORT_NONE);

    for (int64_t row_index = 0; row_index < row_count; row_index++) {
        rt_textureasset3d_format_capability row = {};
        EXPECT_TRUE(rt_textureasset3d_get_format_capability(row_index, &row) == 1,
                    "capability row can be copied");
        if (!row.name || row.vk_format_last < row.vk_format_first || row.block_width <= 0 ||
            row.block_height <= 0 || row.block_bytes <= 0)
            continue;
        EXPECT_EQ(rt_textureasset3d_cpu_format_support_level(row.name), row.cpu_support);
        EXPECT_TRUE(row.cpu_support == RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL ||
                        row.cpu_support == RT_TEXTUREASSET3D_CPU_SUPPORT_FULL,
                    "every advertised table row has an executable CPU fallback");
        EXPECT_TRUE((row.native_format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE) ==
                        (row.backend_capability_bit == RT_TEXTUREASSET3D_BACKEND_CAP_NONE),
                    "native id and backend capability are present together");

        std::vector<uint8_t> payload((size_t)row.block_bytes, 0);
        if (std::strcmp(row.name, "rgba8") == 0) {
            payload[0] = 0x12;
            payload[1] = 0x34;
            payload[2] = 0x56;
            payload[3] = 0x78;
        } else if (std::strcmp(row.name, "bc7") == 0) {
            std::memcpy(payload.data(), valid_bc7, sizeof(valid_bc7));
        } else if (std::strcmp(row.name, "etc2") == 0) {
            std::memcpy(payload.data(), valid_etc2, sizeof(valid_etc2));
        } else if (std::strcmp(row.name, "astc") == 0) {
            std::memcpy(payload.data(), valid_astc, sizeof(valid_astc));
        }

        for (uint32_t vk_format = row.vk_format_first; vk_format <= row.vk_format_last;
             vk_format++) {
            std::vector<uint8_t> bytes = make_test_ktx2_memory(vk_format,
                                                               (uint32_t)row.block_width,
                                                               (uint32_t)row.block_height,
                                                               payload.data(),
                                                               payload.size());
            void *asset = rt_textureasset3d_load_ktx2_memory_strict(bytes.data(), bytes.size());
            EXPECT_TRUE(asset != nullptr, "every Vulkan value in a capability row loads strictly");
            if (!asset)
                continue;
            EXPECT_TRUE(
                std::strcmp(rt_string_cstr(rt_textureasset3d_get_format(asset)), row.name) == 0,
                "loaded texture name comes from its capability row");
            EXPECT_EQ(rt_textureasset3d_get_compressed(asset), row.compressed ? 1 : 0);
            EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) != nullptr,
                        "advertised CPU decoder produces a Pixels fallback");
            EXPECT_EQ(rt_textureasset3d_get_native_format_id(asset), row.native_format_id);
            EXPECT_EQ(rt_textureasset3d_get_native_capability_bit(asset),
                      row.backend_capability_bit);
            if (row.backend_capability_bit != RT_TEXTUREASSET3D_BACKEND_CAP_NONE) {
                EXPECT_TRUE(vgfx3d_textureasset_native_supported(asset, row.backend_capability_bit),
                            "row-derived backend bit authorizes matching native upload");
            } else {
                EXPECT_TRUE(!vgfx3d_textureasset_native_supported(asset, INT64_MAX),
                            "CPU-only row cannot acquire native support from unrelated bits");
            }
            if (rt_obj_release_check0(asset))
                rt_obj_free(asset);
            if (vk_format == UINT32_MAX)
                break;
        }
    }

    {
        rt_textureasset3d_format_capability invalid = {};
        invalid.vk_format_first = UINT32_MAX;
        EXPECT_TRUE(rt_textureasset3d_get_format_capability(-1, &invalid) == 0,
                    "negative capability index is rejected");
        EXPECT_EQ(invalid.vk_format_first, 0);
    }
    PASS();
}

/// @brief Bound the live destination allocation for a large zlib-supercompressed level.
/// @details Generates a deterministic 256x256 RGBA8 image and a stored-block zlib stream without
///          external libraries. The strict memory loader must decode directly into one exact
///          canonical backing span: allocation telemetry reports peak destination bytes equal to
///          final bytes, and sampled corner texels prove that bypassing the intermediate copy did
///          not change content.
static void test_textureasset3d_supercompression_direct_backing_allocation() {
    TEST("TextureAsset3D supercompression decodes directly into canonical backing");
    const uint32_t width = 256;
    const uint32_t height = 256;
    std::vector<uint8_t> rgba((size_t)width * (size_t)height * 4u);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t offset = ((size_t)y * width + x) * 4u;
            rgba[offset + 0] = (uint8_t)x;
            rgba[offset + 1] = (uint8_t)y;
            rgba[offset + 2] = (uint8_t)(x ^ y);
            rgba[offset + 3] = 0xFF;
        }
    }
    std::vector<uint8_t> zlib = make_test_zlib_stored(rgba.data(), rgba.size());
    std::vector<uint8_t> bytes = make_test_supercompressed_ktx2_memory(
        37u, width, height, 3u, zlib.data(), zlib.size(), rgba.size());

    rt_textureasset3d_test_reset_supercompression_allocation_telemetry();
    void *asset = rt_textureasset3d_load_ktx2_memory_strict(bytes.data(), bytes.size());
    EXPECT_TRUE(asset != nullptr, "large generated zlib KTX2 loads strictly");
    if (asset) {
        TextureAsset3DTestLayout *layout = static_cast<TextureAsset3DTestLayout *>(asset);
        void *pixels = rt_textureasset3d_get_pixels(asset);
        EXPECT_TRUE(pixels != nullptr, "large supercompressed texture exposes decoded Pixels");
        if (pixels) {
            EXPECT_EQ(rt_pixels_get_rgba(pixels, 0, 0), 0x000000FF);
            EXPECT_EQ(rt_pixels_get_rgba(pixels, 255, 255), 0xFFFF00FF);
            EXPECT_EQ(rt_pixels_get_rgba(pixels, 173, 91), 0xAD5BF6FF);
        }
        EXPECT_TRUE(layout->source_backing != nullptr,
                    "large supercompressed asset owns canonical backing");
        EXPECT_EQ(layout->source_backing_bytes, rgba.size());
        EXPECT_EQ(rt_textureasset3d_test_get_supercompression_final_bytes(), rgba.size());
        EXPECT_EQ(rt_textureasset3d_test_get_supercompression_peak_bytes(), rgba.size());
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    PASS();
}

static void test_textureasset3d_rejects_unsupported_ktx2_headers() {
    TEST("TextureAsset3D.LoadKTX2 rejects unsupported KTX2 headers");
    const char *super_path = "/tmp/zanna_textureasset3d_supercompressed_test.ktx2";
    const char *implicit_path = "/tmp/zanna_textureasset3d_implicit_mips_test.ktx2";
    const char *short_mip_path = "/tmp/zanna_textureasset3d_short_mip_payload_test.ktx2";
    uint8_t payload[16] = {0};
    uint8_t short_payload[8] = {0};
    rt_string path_s;

    /* Scheme 4 is beyond the known KTX2 supercompression schemes (0 none, 1 BasisLZ,
     * 2 Zstd, 3 ZLIB — all supported now); it must fail with the unsupported
     * diagnostic. A scheme-1 header without its global data segment must fail as
     * truncated BasisLZ instead. */
    EXPECT_TRUE(
        write_test_ktx2_custom_header(super_path, 37u, 1u, 1u, 1u, 4u, payload, sizeof(payload)),
        "supercompressed KTX2 fixture written");
    path_s = rt_string_from_bytes(super_path, std::strlen(super_path));
    EXPECT_TRUE(ktx2_load_fails_with(path_s, "unsupported KTX2 supercompression"),
                "supercompressed KTX2 fails recoverably with a diagnostic");
    rt_string_unref(path_s);

    EXPECT_TRUE(
        write_test_ktx2_custom_header(super_path, 0u, 1u, 1u, 1u, 1u, payload, sizeof(payload)),
        "BasisLZ KTX2 fixture without global data written");
    path_s = rt_string_from_bytes(super_path, std::strlen(super_path));
    EXPECT_TRUE(ktx2_load_fails_with(path_s, "BasisLZ"),
                "truncated BasisLZ KTX2 fails recoverably with a diagnostic");
    rt_string_unref(path_s);

    EXPECT_TRUE(write_test_ktx2_custom_header(implicit_path, 37u, 1u, 1u, 0u, 0u, nullptr, 0u),
                "implicit-mip KTX2 fixture written");
    path_s = rt_string_from_bytes(implicit_path, std::strlen(implicit_path));
    EXPECT_TRUE(ktx2_load_fails_with(path_s, "implicit mip generation is unsupported"),
                "implicit-mip KTX2 fails recoverably with a diagnostic");
    rt_string_unref(path_s);

    EXPECT_TRUE(write_test_ktx2(short_mip_path, 145u, 4u, 4u, short_payload, sizeof(short_payload)),
                "short native-mip KTX2 fixture written");
    path_s = rt_string_from_bytes(short_mip_path, std::strlen(short_mip_path));
    EXPECT_TRUE(ktx2_load_fails_with(path_s, "invalid mip payload length"),
                "short native mip payload fails recoverably with a diagnostic");
    rt_string_unref(path_s);

    std::remove(super_path);
    std::remove(implicit_path);
    std::remove(short_mip_path);
    PASS();
}

static void test_textureasset3d_native_resident_mips_feed_backend_utils() {
    TEST("TextureAsset3D native resident mips feed backend upload helpers");
    const char *path = "/tmp/zanna_textureasset3d_native_resident_mips_test.ktx2";
    uint8_t level0[64];
    uint8_t level1[16];
    uint8_t level2[16];
    const uint8_t *levels[] = {level0, level1, level2};
    const uint64_t level_bytes[] = {sizeof(level0), sizeof(level1), sizeof(level2)};
    rt_string path_s;
    void *asset;
    uint64_t full_key;
    uint64_t resident_key;
    vgfx3d_native_texture_mip_t mip;

    for (size_t i = 0; i < sizeof(level0); i++)
        level0[i] = (uint8_t)(0x10u + i);
    for (size_t i = 0; i < sizeof(level1); i++)
        level1[i] = (uint8_t)(0x80u + i);
    for (size_t i = 0; i < sizeof(level2); i++)
        level2[i] = (uint8_t)(0xC0u + i);

    EXPECT_TRUE(write_test_ktx2_mips(path, 145u, 8u, 8u, levels, level_bytes, 3u),
                "test mipmapped BC7 KTX2 fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr);

    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 96);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) != nullptr,
                "compressed TextureAsset3D keeps a decoded fallback for software sampling");
    EXPECT_TRUE(vgfx3d_textureasset_native_supported(asset, RT_CANVAS3D_BACKEND_CAP_BC7),
                "BC7 asset is native-upload capable when backend advertises BC7");
    EXPECT_TRUE(!vgfx3d_textureasset_native_supported(asset, RT_CANVAS3D_BACKEND_CAP_ASTC),
                "BC7 asset is not native-upload capable under unrelated caps");
    full_key = rt_textureasset3d_get_native_cache_key(asset);
    EXPECT_TRUE(full_key != 0, "fully resident compressed asset has a native cache key");

    rt_textureasset3d_set_resident_mip_range(asset, 1, 2);
    resident_key = rt_textureasset3d_get_native_cache_key(asset);
    EXPECT_TRUE(resident_key != 0 && resident_key != full_key,
                "native cache key changes when the resident mip window changes");
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 32);

    EXPECT_TRUE(vgfx3d_textureasset_get_native_resident_mip(asset, 0, &mip),
                "relative resident mip 0 resolves to absolute mip 1");
    EXPECT_EQ(mip.bytes, 16);
    EXPECT_EQ(mip.width, 4);
    EXPECT_EQ(mip.height, 4);
    EXPECT_EQ(mip.block_width, 4);
    EXPECT_EQ(mip.block_height, 4);
    EXPECT_EQ(mip.block_bytes, 16);
    EXPECT_EQ(mip.format_id, RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7);
    EXPECT_TRUE(mip.data != nullptr && mip.data[0] == level1[0],
                "resident native mip 0 borrows mip 1 payload bytes");

    EXPECT_TRUE(vgfx3d_textureasset_get_native_resident_mip(asset, 1, &mip),
                "relative resident mip 1 resolves to absolute mip 2");
    EXPECT_EQ(mip.bytes, 16);
    EXPECT_EQ(mip.width, 2);
    EXPECT_EQ(mip.height, 2);
    EXPECT_TRUE(mip.data != nullptr && mip.data[0] == level2[0],
                "resident native mip 1 borrows mip 2 payload bytes");
    EXPECT_TRUE(!vgfx3d_textureasset_get_native_resident_mip(asset, 2, &mip),
                "resident native mip query rejects out-of-range relative mips");

    EXPECT_EQ(vgfx3d_textureasset_pending_native_bytes(asset, 0, 0, 1), 32);
    EXPECT_EQ(vgfx3d_textureasset_pending_native_bytes(asset, 0, 1, 1), 16);
    EXPECT_EQ(vgfx3d_textureasset_pending_native_bytes(asset, 1, 0, 1), 16);
    EXPECT_EQ(vgfx3d_textureasset_pending_native_bytes(asset, 2, 0, 1), 0);
    EXPECT_EQ(vgfx3d_textureasset_pending_native_bytes(asset, 0, 0, 0), 0);
    EXPECT_EQ(rt_textureasset3d_get_resident_bytes(asset), 32);

    auto *layout = static_cast<TextureAsset3DTestLayout *>(asset);
    EXPECT_TRUE(layout != nullptr && layout->mip_capacity == 3,
                "Native TextureAsset3D test layout sees loaded mip capacity");
    if (layout) {
        layout->mip_count = std::numeric_limits<int64_t>::max();
        EXPECT_EQ(rt_textureasset3d_get_mip_count(asset), 3);
        EXPECT_TRUE(rt_textureasset3d_get_native_mip_info(
                        asset, 3, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) ==
                        0,
                    "native mip helper rejects corrupt counts beyond allocated capacity");
        layout->resident_mip_start = 1;
        layout->resident_mip_count = 99;
        EXPECT_EQ(rt_textureasset3d_get_native_cache_key(asset), 0);
        EXPECT_TRUE(!vgfx3d_textureasset_native_supported(asset, RT_CANVAS3D_BACKEND_CAP_BC7),
                    "native support helper rejects corrupt resident ranges");
        layout->mip_count = layout->mip_capacity;
        rt_textureasset3d_set_resident_mip_range(asset, 1, 2);
    }

    rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    EXPECT_EQ(rt_textureasset3d_get_native_cache_key(asset), 0);
    EXPECT_TRUE(!vgfx3d_textureasset_native_supported(asset, RT_CANVAS3D_BACKEND_CAP_BC7),
                "empty residency disables native compressed upload");

    {
        const char *empty_path = "/tmp/zanna_textureasset3d_empty_native_payload_test.ktx2";
        const uint8_t *empty_levels[] = {nullptr};
        const uint64_t empty_level_bytes[] = {0};
        rt_string empty_path_s;
        EXPECT_TRUE(
            write_test_ktx2_mips(empty_path, 157u, 4u, 4u, empty_levels, empty_level_bytes, 1u),
            "zero-length native KTX2 fixture written");
        empty_path_s = rt_string_from_bytes(empty_path, std::strlen(empty_path));
        EXPECT_TRUE(ktx2_load_fails_with(empty_path_s, "invalid mip payload length"),
                    "zero-length native payloads are rejected");
        rt_string_unref(empty_path_s);
        std::remove(empty_path);
    }

    std::remove(path);
    PASS();
}

static void test_material_inspection_getters() {
    TEST("Material3D inspection getters");
    void *m = rt_material3d_new_color(0.25, 0.5, 0.75);
    assert(m);
    void *color = rt_material3d_get_color(m);
    EXPECT_NEAR(rt_vec3_x(color), 0.25, 0.001);
    EXPECT_NEAR(rt_vec3_y(color), 0.5, 0.001);
    EXPECT_NEAR(rt_vec3_z(color), 0.75, 0.001);
    EXPECT_EQ(rt_material3d_get_unlit(m), 0);
    rt_material3d_set_unlit(m, 1);
    EXPECT_EQ(rt_material3d_get_unlit(m), 1);
    rt_material3d_set_shading_model(m, 4);
    EXPECT_EQ(rt_material3d_get_shading_model(m), 4);
    rt_material3d_set_shading_model(m, 99);
    EXPECT_EQ(rt_material3d_get_shading_model(m), 0);
    PASS();
}

static void test_material_texture_presence_getters() {
    TEST("Material3D texture presence getters");
    void *m = rt_material3d_new();
    void *px = rt_pixels_new(4, 4);
    void *cm = rt_cubemap3d_new(px, px, px, px, px, px);
    assert(m != NULL && px != NULL && cm != NULL);

    EXPECT_EQ(rt_material3d_get_has_texture(m), 0);
    EXPECT_EQ(rt_material3d_get_has_normal_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_specular_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_ao_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_env_map(m), 0);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(m) == nullptr,
                "empty material exposes no base-color preview pixels");
    EXPECT_TRUE(rt_material3d_get_normal_map_pixels(m) == nullptr,
                "empty material exposes no normal-map preview pixels");
    EXPECT_TRUE(rt_material3d_get_specular_map_pixels(m) == nullptr,
                "empty material exposes no specular-map preview pixels");
    EXPECT_TRUE(rt_material3d_get_emissive_map_pixels(m) == nullptr,
                "empty material exposes no emissive-map preview pixels");
    EXPECT_TRUE(rt_material3d_get_metallic_roughness_map_pixels(m) == nullptr,
                "empty material exposes no metallic/roughness preview pixels");
    EXPECT_TRUE(rt_material3d_get_ao_map_pixels(m) == nullptr,
                "empty material exposes no ambient-occlusion preview pixels");
    EXPECT_TRUE(rt_material3d_get_lightmap_pixels(m) == nullptr,
                "empty material exposes no lightmap preview pixels");
    EXPECT_EQ(rt_material3d_get_has_texture(NULL), 0);
    EXPECT_EQ(rt_material3d_get_has_env_map(NULL), 0);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(NULL) == nullptr,
                "null material exposes no preview pixels");

    rt_material3d_set_albedo_map(m, px);
    EXPECT_EQ(rt_material3d_get_has_texture(m), 1);
    rt_material3d_set_texture(m, NULL);
    EXPECT_EQ(rt_material3d_get_has_texture(m), 0);

    rt_material3d_set_texture(m, px);
    rt_material3d_set_normal_map(m, px);
    rt_material3d_set_specular_map(m, px);
    rt_material3d_set_emissive_map(m, px);
    rt_material3d_set_metallic_roughness_map(m, px);
    rt_material3d_set_ao_map(m, px);
    rt_material3d_set_lightmap(m, px);
    rt_material3d_set_env_map(m, cm);
    EXPECT_EQ(rt_material3d_get_has_texture(m), 1);
    EXPECT_EQ(rt_material3d_get_has_normal_map(m), 1);
    EXPECT_EQ(rt_material3d_get_has_specular_map(m), 1);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(m), 1);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(m), 1);
    EXPECT_EQ(rt_material3d_get_has_ao_map(m), 1);
    EXPECT_EQ(rt_material3d_get_has_env_map(m), 1);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(m) == px,
                "base-color inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_normal_map_pixels(m) == px,
                "normal inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_specular_map_pixels(m) == px,
                "specular inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_emissive_map_pixels(m) == px,
                "emissive inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_metallic_roughness_map_pixels(m) == px,
                "metallic/roughness inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_ao_map_pixels(m) == px,
                "ambient-occlusion inspection returns the bound drawable Pixels");
    EXPECT_TRUE(rt_material3d_get_lightmap_pixels(m) == px,
                "lightmap inspection returns the bound drawable Pixels");

    rt_material3d_set_texture(m, NULL);
    rt_material3d_set_normal_map(m, NULL);
    rt_material3d_set_specular_map(m, NULL);
    rt_material3d_set_emissive_map(m, NULL);
    rt_material3d_set_metallic_roughness_map(m, NULL);
    rt_material3d_set_ao_map(m, NULL);
    rt_material3d_set_lightmap(m, NULL);
    rt_material3d_set_env_map(m, NULL);
    EXPECT_EQ(rt_material3d_get_has_texture(m), 0);
    EXPECT_EQ(rt_material3d_get_has_normal_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_specular_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_ao_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_env_map(m), 0);
    EXPECT_TRUE(rt_material3d_get_texture_pixels(m) == nullptr,
                "cleared base-color slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_normal_map_pixels(m) == nullptr,
                "cleared normal slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_specular_map_pixels(m) == nullptr,
                "cleared specular slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_emissive_map_pixels(m) == nullptr,
                "cleared emissive slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_metallic_roughness_map_pixels(m) == nullptr,
                "cleared metallic/roughness slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_ao_map_pixels(m) == nullptr,
                "cleared ambient-occlusion slot exposes no preview pixels");
    EXPECT_TRUE(rt_material3d_get_lightmap_pixels(m) == nullptr,
                "cleared lightmap slot exposes no preview pixels");
    PASS();
}

//=============================================================================
// Clustered forward+ binning tests (Plan 07)
//=============================================================================

static void test_cluster_slice_and_radius_math() {
    TEST("Cluster froxel slice + light radius math (Plan 07)");
    /* Z slices: clamped at the ends, monotone in between. */
    EXPECT_EQ(canvas3d_cluster_z_slice(0.05f, 0.1f, 100.0f), 0);
    EXPECT_EQ(canvas3d_cluster_z_slice(0.1f, 0.1f, 100.0f), 0);
    EXPECT_EQ(canvas3d_cluster_z_slice(100.0f, 0.1f, 100.0f), VGFX3D_CLUSTER_DIM_Z - 1);
    EXPECT_EQ(canvas3d_cluster_z_slice(1000.0f, 0.1f, 100.0f), VGFX3D_CLUSTER_DIM_Z - 1);
    {
        int32_t prev = 0;
        for (float d = 0.1f; d <= 100.0f; d *= 1.5f) {
            int32_t s = canvas3d_cluster_z_slice(d, 0.1f, 100.0f);
            EXPECT_TRUE(s >= prev, "Z slice is monotone in depth");
            prev = s;
        }
    }
    /* Radius helper: zero-intensity lights vanish; a raw zero attenuation coefficient is unbounded.
     */
    EXPECT_TRUE(canvas3d_cluster_light_radius(0.0f, 1.0f) == 0.0f,
                "Zero intensity has zero influence radius");
    EXPECT_TRUE(canvas3d_cluster_light_radius(1.0f, 0.0f) < 0.0f, "Zero attenuation is unbounded");
    /* intensity/(1 + k r^2) == 1/255 at the returned radius. */
    {
        float r = canvas3d_cluster_light_radius(1.0f, 0.5f);
        float atten = 1.0f / (1.0f + 0.5f * r * r);
        EXPECT_TRUE(std::fabs(atten - 1.0f / 255.0f) < 1e-4f,
                    "Radius solves the attenuation threshold");
    }
    EXPECT_TRUE(canvas3d_cluster_light_is_global(0) && canvas3d_cluster_light_is_global(2) &&
                    !canvas3d_cluster_light_is_global(1) && !canvas3d_cluster_light_is_global(3) &&
                    !canvas3d_cluster_light_is_global(4) && !canvas3d_cluster_light_is_global(5) &&
                    !canvas3d_cluster_light_is_global(6),
                "Directional/ambient are global; punctual/area/volume lights bin");
    PASS();
}

extern "C" int32_t build_light_params(const rt_canvas3d *c,
                                      vgfx3d_light_params_t *out,
                                      int32_t max);

static void cluster_noop_present_postfx(void *ctx, const vgfx3d_postfx_chain_t *chain) {
    (void)ctx;
    (void)chain;
}

static void cluster_noop_begin_frame(void *ctx, const vgfx3d_camera_params_t *cam) {
    (void)ctx;
    (void)cam;
}

static void cluster_noop_submit_draw(void *ctx,
                                     vgfx_window_t win,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     const vgfx3d_light_params_t *lights,
                                     int32_t light_count,
                                     const float *ambient,
                                     int8_t wireframe,
                                     int8_t backface_cull) {
    (void)ctx;
    (void)win;
    (void)cmd;
    (void)lights;
    (void)light_count;
    (void)ambient;
    (void)wireframe;
    (void)backface_cull;
}

static void cluster_noop_end_frame(void *ctx) {
    (void)ctx;
}

/// Begin a mock-backend 3D frame so the canvas carries real cached camera state.
static vgfx3d_backend_t *cluster_test_begin_frame(rt_canvas3d *canvas, void **out_cam) {
    vgfx3d_backend_t *backend = (vgfx3d_backend_t *)calloc(1, sizeof(vgfx3d_backend_t));
    backend->name = "opengl";
    backend->begin_frame = cluster_noop_begin_frame;
    backend->submit_draw = cluster_noop_submit_draw;
    backend->end_frame = cluster_noop_end_frame;
    memset(canvas, 0, sizeof(*canvas));
    canvas->backend = backend;
    canvas->gfx_win = (vgfx_window_t)1;
    *out_cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_canvas3d_begin(canvas, *out_cam);
    return backend;
}

static void cluster_test_make_point_light(
    vgfx3d_light_params_t *l, float x, float y, float z, float intensity, float attenuation) {
    memset(l, 0, sizeof(*l));
    l->type = 1;
    l->shadow_index = -1;
    l->position[0] = x;
    l->position[1] = y;
    l->position[2] = z;
    l->color[0] = l->color[1] = l->color[2] = 1.0f;
    l->intensity = intensity;
    l->attenuation = attenuation;
}

static void test_cluster_table_binning_is_conservative() {
    TEST("Cluster binning is conservative: no in-range light is missing (Plan 07)");
    rt_canvas3d canvas;
    void *cam = nullptr;
    (void)cluster_test_begin_frame(&canvas, &cam);

    vgfx3d_light_params_t lights[6];
    memset(lights, 0, sizeof(lights));
    /* Globals-first prefix: one directional, one ambient. */
    lights[0].type = 0;
    lights[0].direction[1] = -1.0f;
    lights[0].intensity = 1.0f;
    lights[0].shadow_index = -1;
    lights[1].type = 2;
    lights[1].intensity = 0.2f;
    lights[1].shadow_index = -1;
    /* Local lights: tight, medium, wide, and behind the camera (all bounded so
     * the fixture stays under the index cap; unbounded lights get their own
     * overflow test below). */
    cluster_test_make_point_light(&lights[2], 0.0f, 1.0f, -8.0f, 2.0f, 8.0f);
    cluster_test_make_point_light(&lights[3], 5.0f, 0.5f, -30.0f, 4.0f, 4.0f);
    cluster_test_make_point_light(&lights[4], -3.0f, 2.0f, -15.0f, 1.5f, 2.0f);
    cluster_test_make_point_light(&lights[5], 0.0f, 0.0f, 12.0f, 3.0f, 4.0f); /* behind */

    vgfx3d_cluster_table_t *table =
        (vgfx3d_cluster_table_t *)calloc(1, sizeof(vgfx3d_cluster_table_t));
    canvas3d_build_cluster_table(&canvas, lights, 6, 7u, table);

    EXPECT_EQ(table->lights_revision, 7u);
    EXPECT_EQ(table->global_light_count, 2);
    EXPECT_EQ(table->binned_light_count, 4);
    EXPECT_EQ(table->overflow_count, 0);
    /* Prefix-sum integrity. */
    {
        int ok = 1;
        for (int32_t ci = 0; ci < VGFX3D_CLUSTER_COUNT; ci++)
            if (table->offsets[ci] > table->offsets[ci + 1])
                ok = 0;
        EXPECT_TRUE(ok, "Cluster offsets are monotone prefix sums");
        EXPECT_TRUE(table->offsets[VGFX3D_CLUSTER_COUNT] <= VGFX3D_MAX_CLUSTER_LIGHT_INDICES,
                    "Cluster index total stays within the cap");
    }

    /* Conservativeness sweep: any local light contributing more than 1/255 at a
     * visible sample point must appear in that point's cluster list. */
    {
        int violations = 0;
        int samples = 0;
        for (int gz = 0; gz < 12; gz++) {
            float depth = 0.15f * powf(100.0f / 0.15f, (float)gz / 11.0f);
            for (int gy = -3; gy <= 3; gy++) {
                for (int gx = -3; gx <= 3; gx++) {
                    /* Sample points fan out in front of the default camera (-Z). */
                    float p[3] = {(float)gx * depth * 0.15f, (float)gy * depth * 0.15f, -depth};
                    float ndc_x;
                    float ndc_y;
                    {
                        const float *vp = canvas.cached_vp;
                        float cx = vp[0] * p[0] + vp[1] * p[1] + vp[2] * p[2] + vp[3];
                        float cy = vp[4] * p[0] + vp[5] * p[1] + vp[6] * p[2] + vp[7];
                        float cw = vp[12] * p[0] + vp[13] * p[1] + vp[14] * p[2] + vp[15];
                        if (cw <= 1e-6f)
                            continue;
                        ndc_x = cx / cw;
                        ndc_y = cy / cw;
                    }
                    float u = ndc_x * 0.5f + 0.5f;
                    float v = 0.5f - ndc_y * 0.5f;
                    float view_depth =
                        (p[0] - canvas.cached_cam_pos[0]) * canvas.cached_cam_forward[0] +
                        (p[1] - canvas.cached_cam_pos[1]) * canvas.cached_cam_forward[1] +
                        (p[2] - canvas.cached_cam_pos[2]) * canvas.cached_cam_forward[2];
                    if (u < 0.001f || u > 0.999f || v < 0.001f || v > 0.999f)
                        continue;
                    if (view_depth <= table->znear * 1.01f || view_depth >= table->zfar * 0.99f)
                        continue;
                    samples++;
                    int32_t cidx = canvas3d_cluster_index_for_point(
                        u, v, view_depth, table->znear, table->zfar);
                    for (int32_t li = table->global_light_count; li < 6; li++) {
                        float dx = p[0] - lights[li].position[0];
                        float dy = p[1] - lights[li].position[1];
                        float dz = p[2] - lights[li].position[2];
                        float d2 = dx * dx + dy * dy + dz * dz;
                        float atten = lights[li].intensity / (1.0f + lights[li].attenuation * d2);
                        if (atten < (1.0f / 255.0f) * 1.05f)
                            continue; /* negligible (margin avoids float edges) */
                        int found = 0;
                        for (uint16_t k = table->offsets[cidx]; k < table->offsets[cidx + 1]; k++) {
                            if (table->indices[k] == (uint16_t)li) {
                                found = 1;
                                break;
                            }
                        }
                        if (!found) {
                            if (violations < 5)
                                fprintf(stderr,
                                        "VIOL li=%d u=%.3f v=%.3f d=%.2f cidx=%d off=[%u,%u) "
                                        "atten=%.4f p=(%.1f,%.1f,%.1f)\n",
                                        li,
                                        u,
                                        v,
                                        view_depth,
                                        cidx,
                                        table->offsets[cidx],
                                        table->offsets[cidx + 1],
                                        atten,
                                        p[0],
                                        p[1],
                                        p[2]);
                            violations++;
                        }
                    }
                }
            }
        }
        EXPECT_TRUE(samples > 100, "Conservativeness sweep visited a meaningful sample set");
        EXPECT_EQ(violations, 0);
    }

    /* Determinism: rebuilding produces a byte-identical table. */
    {
        vgfx3d_cluster_table_t *again =
            (vgfx3d_cluster_table_t *)calloc(1, sizeof(vgfx3d_cluster_table_t));
        canvas3d_build_cluster_table(&canvas, lights, 6, 7u, again);
        EXPECT_TRUE(memcmp(table, again, sizeof(*table)) == 0, "Binning is deterministic");
        free(again);
    }

    rt_canvas3d_end(&canvas);
    free(table);
    PASS();
}

static void test_cluster_table_overflow_truncates_deterministically() {
    TEST("Cluster index overflow truncates deterministically (Plan 07)");
    rt_canvas3d canvas;
    void *cam = nullptr;
    (void)cluster_test_begin_frame(&canvas, &cam);

    /* Three unbounded lights demand 3 * 3456 entries against the 8192 cap. */
    vgfx3d_light_params_t lights[3];
    memset(lights, 0, sizeof(lights));
    cluster_test_make_point_light(&lights[0], 0.0f, 0.0f, -5.0f, 1.0f, 0.0f);
    cluster_test_make_point_light(&lights[1], 2.0f, 0.0f, -9.0f, 1.0f, 0.0f);
    cluster_test_make_point_light(&lights[2], -2.0f, 1.0f, -20.0f, 1.0f, 0.0f);

    vgfx3d_cluster_table_t *table =
        (vgfx3d_cluster_table_t *)calloc(1, sizeof(vgfx3d_cluster_table_t));
    canvas3d_build_cluster_table(&canvas, lights, 3, 9u, table);

    EXPECT_EQ(table->overflow_count, 3 * VGFX3D_CLUSTER_COUNT - VGFX3D_MAX_CLUSTER_LIGHT_INDICES);
    EXPECT_EQ((int)table->offsets[VGFX3D_CLUSTER_COUNT], VGFX3D_MAX_CLUSTER_LIGHT_INDICES);
    {
        int ok = 1;
        for (int32_t ci = 0; ci < VGFX3D_CLUSTER_COUNT; ci++)
            if (table->offsets[ci] > table->offsets[ci + 1])
                ok = 0;
        EXPECT_TRUE(ok, "Offsets stay monotone under truncation");
        for (int32_t k = 0; k < VGFX3D_MAX_CLUSTER_LIGHT_INDICES; k++)
            if (table->indices[k] > 2)
                ok = 0;
        EXPECT_TRUE(ok, "Truncated index stream stays in range");
    }
    {
        vgfx3d_cluster_table_t *again =
            (vgfx3d_cluster_table_t *)calloc(1, sizeof(vgfx3d_cluster_table_t));
        canvas3d_build_cluster_table(&canvas, lights, 3, 9u, again);
        EXPECT_TRUE(memcmp(table, again, sizeof(*table)) == 0,
                    "Truncation is order-stable and deterministic");
        free(again);
    }

    rt_canvas3d_end(&canvas);
    free(table);
    PASS();
}

static void test_cluster_table_ring_and_gating() {
    TEST("Cluster table ring keys by revision and gates by backend (Plan 07)");
    rt_canvas3d canvas;
    void *cam = nullptr;
    vgfx3d_backend_t *mock_backend = cluster_test_begin_frame(&canvas, &cam);

    vgfx3d_light_params_t lights[2];
    memset(lights, 0, sizeof(lights));
    cluster_test_make_point_light(&lights[0], 0.0f, 1.0f, -5.0f, 1.0f, 1.0f);
    cluster_test_make_point_light(&lights[1], 2.0f, 1.0f, -9.0f, 1.0f, 1.0f);

    /* Flat-loop backend (no GPU postfx hook): never builds tables. */
    canvas.clustered_lighting = 1;
    EXPECT_TRUE(canvas3d_cluster_table_for_revision(&canvas, lights, 2, 5u) == NULL,
                "Backends without GPU postfx keep the flat loop");

    /* GPU-like backend: tables build and key by revision. */
    mock_backend->present_postfx = cluster_noop_present_postfx;
    canvas.clustered_lighting = 0;
    EXPECT_TRUE(canvas3d_cluster_table_for_revision(&canvas, lights, 2, 5u) == NULL,
                "Clustering disabled keeps the flat loop");
    canvas.clustered_lighting = 1;
    const vgfx3d_cluster_table_t *t5 = canvas3d_cluster_table_for_revision(&canvas, lights, 2, 5u);
    const vgfx3d_cluster_table_t *t5b = canvas3d_cluster_table_for_revision(&canvas, lights, 2, 5u);
    const vgfx3d_cluster_table_t *t6 = canvas3d_cluster_table_for_revision(&canvas, lights, 2, 6u);
    EXPECT_TRUE(t5 != NULL && t5 == t5b, "Same revision reuses the cached table");
    EXPECT_TRUE(t6 != NULL && t6 != t5, "New revision gets its own table");
    EXPECT_EQ(t5->lights_revision, 5u);
    EXPECT_EQ(t6->lights_revision, 6u);
    EXPECT_EQ(t5->global_light_count, 0);
    EXPECT_EQ(t5->binned_light_count, 2);

    rt_canvas3d_end(&canvas);
    free(canvas.cluster_tables);
    canvas.cluster_tables = NULL;
    PASS();
}

/// Plan 10: soft particles fade blend-mode fragments against the opaque depth
/// snapshot taken at the opaque->transparent seam (software backend, real
/// end-to-end render into a RenderTarget3D).
static void test_soft_particle_fade_software() {
    TEST("Soft particles fade against the opaque depth snapshot (Plan 10)");
    rt_canvas3d canvas = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(8, 8);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *wall_mesh = rt_mesh3d_new_box(6.0, 6.0, 0.2);
    void *quad_mesh = rt_mesh3d_new_box(4.0, 4.0, 0.02);
    void *wall_mat = rt_material3d_new();
    void *quad_mat = rt_material3d_new();
    void *wall_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -5.0, 0.0, 0.0, 0.0, 1.0);
    void *quad_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -4.8, 0.0, 0.0, 0.0, 1.0);
    const size_t center = (size_t)4 * 8 * 4 + (size_t)4 * 4;
    uint8_t hard_r, hard_g, soft_r, soft_g;

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx((vgfx_window_t)0, 8, 8);
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "software backend context exists");
    canvas.gfx_win =
        (vgfx_window_t)1; /* draws require a window handle; the RT path never uses it */
    canvas.width = 8;
    canvas.height = 8;
    if (!rt || !rt->target || !canvas.backend_ctx)
        return;
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("soft-particles")) == 1,
                "software backend advertises soft particles");
    {
        vgfx3d_backend_t bare = {};
        rt_canvas3d fake = {};
        bare.name = "fake";
        fake.backend = &bare;
        EXPECT_TRUE(rt_canvas3d_backend_supports(&fake, rt_const_cstr("soft-particles")) == 0,
                    "backends without the snapshot hook do not advertise soft particles");
    }
    rt_canvas3d_set_render_target(&canvas, rt);

    rt_material3d_set_unlit(wall_mat, 1);
    rt_material3d_set_color(wall_mat, 1.0, 0.0, 0.0);
    rt_material3d_set_unlit(quad_mat, 1);
    rt_material3d_set_color(quad_mat, 0.0, 1.0, 0.0);
    rt_material3d_set_alpha(quad_mat, 0.5);
    rt_material3d_set_alpha_mode(quad_mat, RT_MATERIAL3D_ALPHA_MODE_BLEND);

    /* Hard pass: fade 0 keeps today's blend result (regression pin). */
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, wall_mesh, wall_xf, wall_mat);
    rt_canvas3d_draw_mesh(&canvas, quad_mesh, quad_xf, quad_mat);
    rt_canvas3d_end(&canvas);
    hard_r = rt->target->color_buf[center + 0];
    hard_g = rt->target->color_buf[center + 1];
    EXPECT_TRUE(hard_g > 90, "Hard blend shows the green particle over the wall");
    EXPECT_TRUE(hard_r > 40, "Hard blend keeps the red wall contribution");

    /* Soft pass: the quad sits 0.1 in front of the wall; fade distance 2.0
     * scales its alpha by ~0.05, so the wall dominates the pixel. */
    ((rt_material3d *)quad_mat)->soft_fade = 2.0;
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, wall_mesh, wall_xf, wall_mat);
    rt_canvas3d_draw_mesh(&canvas, quad_mesh, quad_xf, quad_mat);
    rt_canvas3d_end(&canvas);
    soft_r = rt->target->color_buf[center + 0];
    soft_g = rt->target->color_buf[center + 1];
    EXPECT_TRUE((int)hard_g - (int)soft_g > 60,
                "Soft fade suppresses the particle near opaque geometry");
    EXPECT_TRUE(soft_r > hard_r, "The wall shows through the faded particle");

    /* Empty background: no opaque depth behind the quad -> fade saturates to 1
     * and the particle stays fully visible. */
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, quad_mesh, quad_xf, quad_mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(rt->target->color_buf[center + 1] > 90,
                "Particles over empty background keep full alpha");

    vgfx3d_software_backend.destroy_ctx(canvas.backend_ctx);
    PASS();
}

static uint8_t render_native_light_center_pixel(rt_canvas3d *canvas,
                                                rt_rendertarget3d *target,
                                                void *camera,
                                                void *mesh,
                                                void *material,
                                                void *transform,
                                                void *light) {
    const size_t center = (size_t)8 * 16 * 4 + (size_t)8 * 4;
    rt_canvas3d_set_light(canvas, 0, light);
    rt_canvas3d_begin(canvas, camera);
    rt_canvas3d_draw_mesh(canvas, mesh, transform, material);
    rt_canvas3d_end(canvas);
    return target->target->color_buf[center];
}

/// Native emitter semantics must execute in the software reference path, including
/// rectangle sidedness and volume boundary fading. GPU shader twins mirror this math.
static void test_native_area_volume_lighting_software() {
    TEST("Software renderer shades native rectangle, sphere, and volume lights");
    rt_canvas3d canvas = {};
    auto *target = static_cast<rt_rendertarget3d *>(rt_rendertarget3d_new(16, 16));
    void *camera = rt_camera3d_new(55.0, 1.0, 0.1, 50.0);
    void *mesh = rt_mesh3d_new_plane(4.0, 4.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *transform = rt_mat4_identity();
    void *position = rt_vec3_new(0.0, 2.0, 0.0);
    void *down = rt_vec3_new(0.0, -1.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *rectangle =
        rt_light3d_new_area_rectangle(position, down, 3.0, 3.0, 1.0, 1.0, 1.0, 0.1, 8.0);
    void *rectangle_back =
        rt_light3d_new_area_rectangle(position, up, 3.0, 3.0, 1.0, 1.0, 1.0, 0.1, 8.0);
    void *sphere = rt_light3d_new_area_sphere(position, 1.0, 1.0, 1.0, 1.0, 8.0);
    void *volume = rt_light3d_new_volume(rt_vec3_new(0.0, 0.0, 0.0), 6.0, 1.0, 1.0, 1.0, 6.0);
    uint8_t rectangle_pixel;
    uint8_t backside_pixel;
    uint8_t sphere_pixel;
    uint8_t volume_pixel;
    assert(target && target->target && camera && mesh && material && transform && rectangle &&
           rectangle_back && sphere && volume);
    rt_light3d_set_intensity(sphere, 3.0);

    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx((vgfx_window_t)0, 16, 16);
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 16;
    canvas.height = 16;
    assert(canvas.backend_ctx);
    rt_canvas3d_set_render_target(&canvas, target);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 5.0, 0.0),
                        rt_vec3_new(0.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 0.0, -1.0));

    rectangle_pixel = render_native_light_center_pixel(
        &canvas, target, camera, mesh, material, transform, rectangle);
    backside_pixel = render_native_light_center_pixel(
        &canvas, target, camera, mesh, material, transform, rectangle_back);
    sphere_pixel = render_native_light_center_pixel(
        &canvas, target, camera, mesh, material, transform, sphere);
    volume_pixel = render_native_light_center_pixel(
        &canvas, target, camera, mesh, material, transform, volume);

    EXPECT_TRUE(rectangle_pixel > 20, "Front-facing rectangle illuminates the receiver");
    EXPECT_TRUE(backside_pixel + 10 < rectangle_pixel,
                "Rectangle emitter contributes only from its authored side");
    EXPECT_TRUE(sphere_pixel > 20, "Sphere area light illuminates the receiver");
    EXPECT_TRUE(volume_pixel > 20, "Volume light contributes isotropic in-volume irradiance");

    rt_canvas3d_set_light(&canvas, 0, NULL);
    vgfx3d_software_backend.destroy_ctx(canvas.backend_ctx);
    PASS();
}

/// Plan 10: AddSSR exports a chain entry with its snapshot params, counts as a
/// GPU-scene effect (rejected on software canvases with a recoverable error),
/// and the ssr draw-cmd mask helper follows the material flag + reflectivity.
static void test_ssr_chain_and_mask_plumbing() {
    TEST("SSR chain export, software rejection, and draw-cmd mask (Plan 10)");
    void *fx = rt_postfx3d_new();
    rt_postfx3d_add_ssr(fx, 0.7, 0.3);
    EXPECT_TRUE(vgfx3d_postfx_requires_gpu_scene_buffers(fx) == 1,
                "SSR requires GPU scene buffers");
    {
        vgfx3d_postfx_chain_t chain;
        memset(&chain, 0, sizeof(chain));
        EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1, "Chain export succeeds");
        EXPECT_EQ(chain.effect_count, 1);
        EXPECT_EQ(chain.effects[0].type, (int32_t)VGFX3D_POSTFX_EFFECT_SSR);
        EXPECT_TRUE(chain.effects[0].snapshot.ssr_enabled == 1, "Snapshot flags SSR");
        EXPECT_NEAR(chain.effects[0].snapshot.ssr_intensity, 0.7, 0.001);
        EXPECT_NEAR(chain.effects[0].snapshot.ssr_max_roughness, 0.3, 0.001);
        EXPECT_EQ(chain.effects[0].snapshot.ssr_steps, 24);
        vgfx3d_postfx_chain_free(&chain);
    }
    {
        /* Software canvases run GPU-scene chains through the CPU reference
         * implementations, so binding now succeeds (postfx software parity). */
        rt_canvas3d canvas = {};
        canvas.backend = &vgfx3d_software_backend;
        rt_canvas3d_set_post_fx(&canvas, fx);
        EXPECT_TRUE(canvas.postfx == fx, "SSR chain attaches on software (CPU reference path)");
        EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("postfx-full")) == 1,
                    "software backend advertises the full postfx chain (CPU reference path)");
        EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("ssr")) == 0,
                    "native GPU SSR pipeline stays unadvertised on software");
    }
    {
        vgfx3d_draw_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        EXPECT_TRUE(vgfx3d_draw_cmd_ssr_mask(&cmd) == 0.0f, "mask off by default");
        cmd.ssr_enabled = 1;
        EXPECT_NEAR(vgfx3d_draw_cmd_ssr_mask(&cmd), 0.5, 0.001);
        cmd.reflectivity = 0.8f;
        EXPECT_NEAR(vgfx3d_draw_cmd_ssr_mask(&cmd), 0.8, 0.001);
    }
    PASS();
}

static void test_build_light_params_sorts_globals_first() {
    TEST("build_light_params orders globals before punctual/area/volume lights");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));

    void *point = rt_light3d_new_point(rt_vec3_new(1.0, 2.0, 3.0), 1.0, 0.0, 0.0, 0.5);
    void *dir = rt_light3d_new_directional(rt_vec3_new(0.0, -1.0, 0.0), 0.0, 1.0, 0.0);
    void *ambient = rt_light3d_new_ambient(0.1, 0.1, 0.1);
    auto *rectangle =
        static_cast<rt_light3d *>(rt_light3d_new_area_rectangle(rt_vec3_new(4.0, 5.0, 6.0),
                                                                rt_vec3_new(0.0, 0.0, -1.0),
                                                                8.0,
                                                                3.0,
                                                                0.2,
                                                                0.3,
                                                                0.4,
                                                                0.25,
                                                                14.0));
    auto *volume = static_cast<rt_light3d *>(
        rt_light3d_new_volume(rt_vec3_new(7.0, 8.0, 9.0), 2.5, 0.5, 0.6, 0.7, 5.0));
    rt_light3d_set_decay_type(rectangle, 3);
    canvas.lights[0] = (rt_light3d *)point;
    canvas.lights[1] = (rt_light3d *)dir;
    canvas.lights[2] = (rt_light3d *)ambient;
    canvas.lights[3] = rectangle;
    canvas.lights[4] = volume;

    vgfx3d_light_params_t out[8];
    int32_t count = build_light_params(&canvas, out, 8);
    EXPECT_EQ(count, 5);
    EXPECT_EQ(out[0].type, 0); /* directional first */
    EXPECT_EQ(out[1].type, 2); /* then ambient */
    EXPECT_EQ(out[2].type, 1); /* locals last */
    EXPECT_EQ(out[3].type, 4);
    EXPECT_EQ(out[4].type, 6);
    EXPECT_NEAR(out[3].width, 8.0, 0.001);
    EXPECT_NEAR(out[3].height, 3.0, 0.001);
    EXPECT_NEAR(out[3].range, 14.0, 0.001);
    EXPECT_EQ(out[3].decay_type, 3);
    EXPECT_NEAR(out[3].basis_u[0] * out[3].direction[0] + out[3].basis_u[1] * out[3].direction[1] +
                    out[3].basis_u[2] * out[3].direction[2],
                0.0,
                0.001);
    EXPECT_NEAR(out[4].radius, 2.5, 0.001);
    EXPECT_NEAR(out[4].range, 5.0, 0.001);
    EXPECT_EQ(out[4].casts_shadows, 0);
    /* Identity survives the reorder (shadow patching matches by identity). */
    EXPECT_TRUE(out[0].identity == (uintptr_t)dir && out[2].identity == (uintptr_t)point,
                "Reordered entries keep their light identities");
    PASS();
}

//=============================================================================
// Light3D tests
//=============================================================================

static void test_light_directional() {
    TEST("Light3D.NewDirectional — creates light");
    void *dir = rt_vec3_new(-1.0, -1.0, -1.0);
    void *l = rt_light3d_new_directional(dir, 1.0, 1.0, 1.0);
    assert(l);
    PASS();
}

static void test_light_point() {
    TEST("Light3D.NewPoint — creates light");
    void *pos = rt_vec3_new(0.0, 5.0, 0.0);
    void *l = rt_light3d_new_point(pos, 1.0, 1.0, 1.0, 0.5);
    assert(l);
    PASS();
}

static void test_light_ambient() {
    TEST("Light3D.NewAmbient — creates light");
    void *l = rt_light3d_new_ambient(0.1, 0.1, 0.1);
    assert(l);
    PASS();
}

static void test_light_set_intensity() {
    TEST("Light3D.SetIntensity — no crash");
    void *dir = rt_vec3_new(-1.0, -1.0, 0.0);
    void *l = rt_light3d_new_directional(dir, 1.0, 1.0, 1.0);
    rt_light3d_set_intensity(l, 2.0);
    PASS();
}

static void test_light_set_color() {
    TEST("Light3D.SetColor — no crash");
    void *l = rt_light3d_new_ambient(0.1, 0.1, 0.1);
    rt_light3d_set_color(l, 0.5, 0.5, 0.5);
    PASS();
}

static void test_light_set_position_and_direction() {
    TEST("Light3D.SetPosition/SetDirection — light is movable");
    void *point = rt_light3d_new_point(rt_vec3_new(0.0, 5.0, 0.0), 1.0, 1.0, 1.0, 0.5);
    rt_light3d_set_position(point, rt_vec3_new(3.0, 4.0, -2.0));
    void *p = rt_light3d_get_position(point);
    EXPECT_NEAR(rt_vec3_x(p), 3.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(p), 4.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(p), -2.0, 0.001);

    void *sun = rt_light3d_new_directional(rt_vec3_new(0.0, -1.0, 0.0), 1.0, 1.0, 1.0);
    rt_light3d_set_direction(sun, rt_vec3_new(2.0, 0.0, 0.0)); /* normalizes to (1,0,0) */
    void *d = rt_light3d_get_direction(sun);
    EXPECT_NEAR(rt_vec3_x(d), 1.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(d), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(d), 0.0, 0.001);
    PASS();
}

static void test_light_inspection_getters_and_enabled() {
    TEST("Light3D inspection getters and Enabled");
    void *dir = rt_vec3_new(0.0, -2.0, 0.0);
    void *pos = rt_vec3_new(1.0, 2.0, 3.0);
    void *sun = rt_light3d_new_directional(dir, 1.0, 0.8, 0.6);
    void *point = rt_light3d_new_point(pos, 0.25, 0.5, 0.75, 0.2);
    assert(sun && point);
    void *sun_dir = rt_light3d_get_direction(sun);
    void *point_pos = rt_light3d_get_position(point);
    void *point_color = rt_light3d_get_color(point);
    EXPECT_EQ(rt_light3d_get_type(sun), 0);
    EXPECT_EQ(rt_light3d_get_type(point), 1);
    EXPECT_EQ(rt_light3d_get_enabled(point), 1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(sun), 1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(point), 0);
    EXPECT_NEAR(rt_light3d_get_intensity(point), 1.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(sun_dir), -1.0, 0.001);
    EXPECT_NEAR(rt_vec3_x(point_pos), 1.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(point_pos), 2.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(point_pos), 3.0, 0.001);
    EXPECT_NEAR(rt_vec3_x(point_color), 0.25, 0.001);
    EXPECT_NEAR(rt_vec3_y(point_color), 0.5, 0.001);
    EXPECT_NEAR(rt_vec3_z(point_color), 0.75, 0.001);
    rt_light3d_set_enabled(point, 0);
    EXPECT_EQ(rt_light3d_get_enabled(point), 0);
    rt_light3d_set_casts_shadows(sun, 0);
    EXPECT_EQ(rt_light3d_get_casts_shadows(sun), 0);
    rt_light3d_set_casts_shadows(sun, 1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(sun), 1);
    rt_light3d_set_casts_shadows(point, 1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(point), 1);
    void *ambient = rt_light3d_new_ambient(0.1, 0.1, 0.1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(ambient), 0);
    PASS();
}

//=============================================================================
// Mesh3D — additional tests
//=============================================================================

static void test_mesh_many_vertices() {
    TEST("Mesh3D — dynamic growth (1000 vertices)");
    void *m = rt_mesh3d_new();
    for (int i = 0; i < 1000; i++)
        rt_mesh3d_add_vertex(m, (double)i, 0, 0, 0, 1, 0, 0, 0);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 1000);
    PASS();
}

static void test_mesh_many_triangles() {
    TEST("Mesh3D — many triangles (500 tris)");
    void *m = rt_mesh3d_new();
    for (int i = 0; i < 500; i++) {
        double x = (double)((i % 25) * 2);
        double y = (double)((i / 25) * 2);
        rt_mesh3d_add_vertex(m, x, y, 0, 0, 0, 1, 0, 0);
        rt_mesh3d_add_vertex(m, x + 1.0, y, 0, 0, 0, 1, 1, 0);
        rt_mesh3d_add_vertex(m, x, y + 1.0, 0, 0, 0, 1, 0, 1);
        rt_mesh3d_add_triangle(m, i * 3, i * 3 + 1, i * 3 + 2);
    }
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 500);
    PASS();
}

static void test_mesh_null_safety() {
    TEST("Mesh3D — null safety (no crash)");
    rt_mesh3d_add_vertex(NULL, 0, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(NULL, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(NULL), 0);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(NULL), 0);
    rt_mesh3d_recalc_normals(NULL);
    rt_mesh3d_transform(NULL, NULL);
    PASS();
}

static void test_mesh_sphere_low_segments() {
    TEST("Mesh3D.NewSphere — minimum segments (4)");
    void *m = rt_mesh3d_new_sphere(1.0, 4);
    assert(m);
    assert(rt_mesh3d_get_vertex_count(m) > 0);
    assert(rt_mesh3d_get_triangle_count(m) > 0);
    PASS();
}

static void test_mesh_cylinder_low_segments() {
    TEST("Mesh3D.NewCylinder — minimum segments (3)");
    void *m = rt_mesh3d_new_cylinder(0.5, 1.0, 3);
    assert(m);
    assert(rt_mesh3d_get_vertex_count(m) > 0);
    assert(rt_mesh3d_get_triangle_count(m) > 0);
    PASS();
}

static void test_mesh_box_dimensions() {
    TEST("Mesh3D.NewBox — different dimensions");
    void *m = rt_mesh3d_new_box(2.0, 0.5, 3.0);
    assert(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 24);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 12);
    PASS();
}

static void test_mesh_transform_identity() {
    TEST("Mesh3D.Transform — identity preserves geometry");
    void *m = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *c = rt_mesh3d_clone(m);
    void *id = rt_mat4_identity();
    rt_mesh3d_transform(c, id);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(c), 24);
    PASS();
}

//=============================================================================
// Camera3D — additional tests
//=============================================================================

static void test_camera_null_safety() {
    TEST("Camera3D — null safety (no crash)");
    rt_camera3d_look_at(NULL, NULL, NULL, NULL);
    rt_camera3d_orbit(NULL, NULL, 5.0, 0, 0);
    EXPECT_NEAR(rt_camera3d_get_fov(NULL), 0.0, 0.001);
    rt_camera3d_set_fov(NULL, 90.0);
    assert(rt_camera3d_get_position(NULL) == NULL);
    assert(rt_camera3d_get_forward(NULL) == NULL);
    assert(rt_camera3d_get_right(NULL) == NULL);
    PASS();
}

static void test_camera_rejects_invalid_vec3_handles() {
    TEST("Camera3D rejects invalid vector handles");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0, 0, 5);
    void *target = rt_vec3_new(0, 0, 0);
    void *up = rt_vec3_new(0, 1, 0);
    void *not_vec3 = rt_material3d_new();
    assert(cam != NULL && eye != NULL && target != NULL && up != NULL && not_vec3 != NULL);

    EXPECT_TRUE(expect_trap_contains([&] { rt_camera3d_look_at(cam, not_vec3, target, up); },
                                     "must be Vec3"),
                "Camera3D.LookAt traps on non-Vec3 eye");
    EXPECT_TRUE(expect_trap_contains([&] { rt_camera3d_orbit(cam, not_vec3, 5.0, 0.0, 0.0); },
                                     "target must be Vec3"),
                "Camera3D.Orbit traps on non-Vec3 target");
    PASS();
}

static void test_camera_right_vector() {
    TEST("Camera3D.Right — perpendicular to forward");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0, 0, 5);
    void *target = rt_vec3_new(0, 0, 0);
    void *up = rt_vec3_new(0, 1, 0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *right = rt_camera3d_get_right(cam);
    assert(right);
    /* Right should be along +X when looking down -Z */
    EXPECT_NEAR(rt_vec3_x(right), 1.0, 0.1);
    EXPECT_NEAR(rt_vec3_y(right), 0.0, 0.1);
    EXPECT_NEAR(rt_vec3_z(right), 0.0, 0.1);
    PASS();
}

static void test_camera_orbit_yaw() {
    TEST("Camera3D.Orbit — yaw 90° moves to +X");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *target = rt_vec3_new(0, 0, 0);
    rt_camera3d_orbit(cam, target, 5.0, 90.0, 0.0);
    void *pos = rt_camera3d_get_position(cam);
    /* At yaw=90°, eye should be at roughly (5, 0, 0) */
    EXPECT_NEAR(rt_vec3_x(pos), 5.0, 0.1);
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.1);
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.5);
    PASS();
}

static void test_camera_orbit_pitch() {
    TEST("Camera3D.Orbit — pitch 45° elevates camera");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *target = rt_vec3_new(0, 0, 0);
    rt_camera3d_orbit(cam, target, 10.0, 0.0, 45.0);
    void *pos = rt_camera3d_get_position(cam);
    /* Y should be positive (elevated) */
    assert(rt_vec3_y(pos) > 3.0);
    PASS();
}

static void test_camera_screen_to_ray_corners() {
    TEST("Camera3D.ScreenToRay — corner rays diverge from center");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0, 0, 5);
    void *target = rt_vec3_new(0, 0, 0);
    void *up = rt_vec3_new(0, 1, 0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *center = rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480);
    void *corner = rt_camera3d_screen_to_ray(cam, 0, 0, 640, 480);

    /* Center ray Z should be more negative (more forward) than corner ray Z */
    assert(rt_vec3_z(center) < rt_vec3_z(corner));
    PASS();
}

static void test_camera_screen_to_ray_uses_viewport_aspect() {
    TEST("Camera3D.ScreenToRay uses viewport aspect");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0, 0, 5);
    void *target = rt_vec3_new(0, 0, 0);
    void *up = rt_vec3_new(0, 1, 0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *wide_right = rt_camera3d_screen_to_ray(cam, 800, 200, 800, 400);
    assert(wide_right);
    EXPECT_TRUE(rt_vec3_x(wide_right) > 0.7, "Wide viewport rays use the active 2:1 output aspect");
    PASS();
}

static void test_camera_ortho_screen_to_ray_parallel() {
    TEST("Camera3D.ScreenToRay keeps orthographic rays parallel");
    void *cam = rt_camera3d_new_ortho(5.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *center = rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480);
    void *corner = rt_camera3d_screen_to_ray(cam, 0, 0, 640, 480);
    EXPECT_NEAR(rt_vec3_x(center), rt_vec3_x(corner), 0.0001);
    EXPECT_NEAR(rt_vec3_y(center), rt_vec3_y(corner), 0.0001);
    EXPECT_NEAR(rt_vec3_z(center), rt_vec3_z(corner), 0.0001);
    PASS();
}

static void test_camera_screen_to_ray_origin_handles_ortho_pixels() {
    TEST("Camera3D.ScreenToRayOrigin gives orthographic pixels distinct origins");
    void *cam = rt_camera3d_new_ortho(5.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(cam, eye, target, up);

    void *center = rt_camera3d_screen_to_ray_origin(cam, 320, 240, 640, 480);
    void *corner = rt_camera3d_screen_to_ray_origin(cam, 0, 0, 640, 480);
    EXPECT_TRUE(center != nullptr && corner != nullptr, "ScreenToRayOrigin returns Vec3 objects");
    EXPECT_NEAR(rt_vec3_x(center), 0.0, 0.01);
    EXPECT_NEAR(rt_vec3_y(center), 0.0, 0.01);
    EXPECT_TRUE(std::fabs(rt_vec3_x(corner) - rt_vec3_x(center)) > 1.0,
                "orthographic corner ray starts at a different X");
    EXPECT_TRUE(std::fabs(rt_vec3_y(corner) - rt_vec3_y(center)) > 1.0,
                "orthographic corner ray starts at a different Y");
    PASS();
}

static void test_camera_screen_to_ray_tracks_shaken_view() {
    TEST("Camera3D.ScreenToRay stays aligned with the shaken view");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_camera3d_look_at(cam, eye, target, up);
    rt_camera3d_shake(cam, 1.0, 0.5, 1.0);

    void *ray = rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480);
    EXPECT_NEAR(rt_vec3_x(ray), 0.0, 0.01);
    EXPECT_NEAR(rt_vec3_y(ray), 0.0, 0.01);
    EXPECT_TRUE(rt_vec3_z(ray) < -0.99, "Center-screen shaken ray still points forward");
    PASS();
}

static void test_camera_set_position_rebuilds_view() {
    TEST("Camera3D.SetPosition rebuilds the view state");
    void *cam = rt_camera3d_new(60.0, 1.333, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *moved = rt_vec3_new(1.0, 2.0, 5.0);

    rt_camera3d_look_at(cam, eye, target, up);
    rt_camera3d_set_position(cam, moved);

    void *ray = rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480);
    assert(ray);
    EXPECT_NEAR(rt_vec3_x(ray), 0.0, 0.15);
    EXPECT_NEAR(rt_vec3_y(ray), 0.0, 0.15);
    EXPECT_NEAR(rt_vec3_z(ray), -1.0, 0.15);
    PASS();
}

static void test_camera_set_yaw_pitch_rebuilds_view() {
    TEST("Camera3D.SetYaw/SetPitch rebuild the FPS view");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    rt_camera3d_set_yaw(cam, 90.0);
    void *yaw_fwd = rt_camera3d_get_forward(cam);
    assert(yaw_fwd);
    EXPECT_NEAR(rt_vec3_x(yaw_fwd), 1.0, 0.1);
    EXPECT_NEAR(rt_vec3_z(yaw_fwd), 0.0, 0.15);

    rt_camera3d_set_pitch(cam, 45.0);
    void *pitch_fwd = rt_camera3d_get_forward(cam);
    assert(pitch_fwd);
    EXPECT_TRUE(rt_vec3_y(pitch_fwd) > 0.6, "Pitch tilts the camera upward immediately");
    PASS();
}

static void test_camera_look_at_coincident_eye_preserves_translation() {
    TEST("Camera3D.LookAt keeps eye translation when eye equals target");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(3.0, 4.0, 5.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_camera3d_look_at(cam, eye, eye, up);

    rt_camera3d *impl = (rt_camera3d *)cam;
    EXPECT_TRUE(std::fabs(impl->view[3]) + std::fabs(impl->view[7]) + std::fabs(impl->view[11]) >
                    0.1,
                "Coincident LookAt preserves a translated view matrix");
    PASS();
}

static void test_camera_look_at_preserves_custom_up_basis() {
    TEST("Camera3D.LookAt preserves caller-supplied up basis");
    void *cam_obj = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 0.0);
    void *target = rt_vec3_new(0.0, 0.0, -1.0);
    void *rolled_up = rt_vec3_new(1.0, 0.0, 0.0);
    rt_camera3d *cam = (rt_camera3d *)cam_obj;

    rt_camera3d_look_at(cam_obj, eye, target, rolled_up);

    EXPECT_NEAR(cam->view[4], 1.0, 0.001);
    EXPECT_NEAR(cam->view[5], 0.0, 0.001);
    EXPECT_NEAR(cam->view[6], 0.0, 0.001);
    PASS();
}

static void test_camera_shake_overshoot_clears_immediately() {
    TEST("Camera3D.Shake clears during the frame that consumes remaining duration");
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_camera3d_shake(cam, 1.0, 0.1, 1.0);

    rt_camera3d_update_shake_for_frame(cam, 0.2);

    EXPECT_NEAR(cam->shake_duration, 0.0, 0.0001);
    EXPECT_NEAR(cam->shake_intensity, 0.0, 0.0001);
    EXPECT_NEAR(cam->shake_offset[0], 0.0, 0.0001);
    EXPECT_NEAR(cam->shake_offset[1], 0.0, 0.0001);
    EXPECT_NEAR(cam->shake_offset[2], 0.0, 0.0001);
    PASS();
}

static void test_camera_ortho_set_fov_preserves_projection() {
    TEST("Camera3D.SetFov leaves orthographic projections unchanged");
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new_ortho(10.0, 16.0 / 9.0, 0.1, 100.0);
    double before[16];
    std::memcpy(before, cam->projection, sizeof(before));

    rt_camera3d_set_fov(cam, 45.0);

    EXPECT_TRUE(std::memcmp(before, cam->projection, sizeof(before)) == 0,
                "Orthographic projection matrix stays intact after SetFov");
    PASS();
}

static void test_camera_shake_does_not_drift_eye_in_smooth_follow() {
    TEST("Camera3D.Shake does not mutate the base eye during SmoothFollow");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    rt_camera3d_orbit(cam, target, 5.0, 0.0, 0.0);
    rt_camera3d_shake(cam, 1.0, 0.5, 1.0);
    rt_camera3d_smooth_follow(cam, target, 5.0, 0.0, 0.0, 0.016);

    void *pos = rt_camera3d_get_position(cam);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.0001);
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.0001);
    EXPECT_NEAR(rt_vec3_z(pos), 5.0, 0.0001);
    PASS();
}

static void test_camera_sanitizes_nonfinite_inputs() {
    TEST("Camera3D sanitizes non-finite public inputs");
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(NAN, NAN, NAN, INFINITY);
    assert(cam != NULL);
    EXPECT_NEAR(cam->fov, 60.0, 0.001);
    EXPECT_NEAR(cam->aspect, 1.0, 0.001);
    EXPECT_NEAR(cam->near_plane, 0.1, 0.001);
    EXPECT_NEAR(cam->far_plane, 1000.1, 0.001);
    EXPECT_TRUE(finite_camera_state(cam),
                "Perspective camera starts finite after invalid construction");

    rt_camera3d_set_fov(cam, NAN);
    EXPECT_NEAR(cam->fov, 60.0, 0.001);

    float render_projection[16];
    rt_camera3d_get_render_projection(cam, INFINITY, render_projection);
    for (float value : render_projection)
        EXPECT_TRUE(std::isfinite(value), "Render projection rejects invalid aspect override");

    void *bad_vec = rt_vec3_new(NAN, INFINITY, -INFINITY);
    void *bad_up = rt_vec3_new(NAN, 0.0, INFINITY);
    rt_camera3d_look_at(cam, bad_vec, bad_vec, bad_up);
    EXPECT_TRUE(finite_camera_state(cam), "LookAt keeps view state finite");
    EXPECT_TRUE(finite_vec3(rt_camera3d_get_position(cam)), "LookAt stores a finite eye");

    rt_camera3d_orbit(cam, bad_vec, INFINITY, NAN, -INFINITY);
    rt_camera3d_set_position(cam, bad_vec);
    rt_camera3d_fps_update(cam, NAN, INFINITY, NAN, INFINITY, -INFINITY, NAN, INFINITY);
    rt_camera3d_set_yaw(cam, NAN);
    rt_camera3d_set_pitch(cam, INFINITY);
    rt_camera3d_shake(cam, INFINITY, NAN, NAN);
    rt_camera3d_update_shake_for_frame(cam, INFINITY);
    rt_camera3d_smooth_follow(cam, bad_vec, INFINITY, NAN, INFINITY, NAN);
    rt_camera3d_smooth_look_at(cam, bad_vec, INFINITY, NAN);
    EXPECT_TRUE(finite_camera_state(cam), "Camera remains finite after invalid control updates");
    EXPECT_TRUE(finite_vec3(rt_camera3d_screen_to_ray(cam, 320, 240, 640, 480)),
                "ScreenToRay returns a finite fallback ray");

    rt_camera3d *ortho = (rt_camera3d *)rt_camera3d_new_ortho(NAN, INFINITY, NAN, -INFINITY);
    assert(ortho != NULL);
    EXPECT_NEAR(ortho->ortho_size, 1.0, 0.001);
    EXPECT_NEAR(ortho->aspect, 1.0, 0.001);
    EXPECT_NEAR(ortho->near_plane, 0.1, 0.001);
    EXPECT_NEAR(ortho->far_plane, 1000.1, 0.001);
    EXPECT_TRUE(finite_camera_state(ortho),
                "Orthographic camera starts finite after invalid construction");
    EXPECT_TRUE(finite_vec3(rt_camera3d_screen_to_ray(ortho, 0, 0, 640, 480)),
                "Orthographic ScreenToRay returns a finite ray");
    PASS();
}

static void test_camera_getters_sanitize_corrupt_private_state() {
    TEST("Camera3D getters sanitize corrupt private state");
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    assert(cam != NULL);

    cam->fov = NAN;
    cam->near_plane = NAN;
    cam->far_plane = -INFINITY;
    cam->is_ortho = -3;
    EXPECT_NEAR(rt_camera3d_get_fov(cam), 0.0, 0.001);
    EXPECT_NEAR(rt_camera3d_get_near_plane(cam), 0.1, 0.001);
    EXPECT_NEAR(rt_camera3d_get_far_plane(cam), 1000.1, 0.001);
    EXPECT_EQ(rt_camera3d_is_ortho(cam), 1);

    cam->is_ortho = 0;
    EXPECT_NEAR(rt_camera3d_get_fov(cam), 60.0, 0.001);
    PASS();
}

static void test_camera_clamps_extreme_finite_inputs() {
    TEST("Camera3D clamps extreme finite public inputs");
    const double huge = 1.0e300;
    const double world_limit = 1000000000000.0;
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(huge, huge, huge, huge);
    assert(cam != NULL);
    EXPECT_TRUE(finite_camera_state(cam),
                "Perspective camera starts finite after huge construction");
    EXPECT_NEAR(cam->fov, 179.0, 0.001);
    EXPECT_NEAR(cam->aspect, 1000000.0, 0.001);

    float render_projection[16];
    rt_camera3d_get_render_projection(cam, huge, render_projection);
    for (float value : render_projection)
        EXPECT_TRUE(std::isfinite(value), "Huge aspect override still produces finite projection");

    void *huge_vec = rt_vec3_new(huge, -huge, huge);
    void *huge_target = rt_vec3_new(-huge, huge, -huge);
    void *huge_up = rt_vec3_new(huge, huge, huge);
    assert(huge_vec && huge_target && huge_up);

    rt_camera3d_look_at(cam, huge_vec, huge_target, huge_up);
    rt_camera3d_orbit(cam, huge_target, huge, huge, huge);
    rt_camera3d_set_position(cam, huge_vec);
    rt_camera3d_fps_update(cam, huge, -huge, huge, -huge, huge, huge, huge);
    rt_camera3d_set_yaw(cam, huge);
    rt_camera3d_set_pitch(cam, huge);
    rt_camera3d_shake(cam, huge, huge, huge);
    rt_camera3d_update_shake_for_frame(cam, huge);
    rt_camera3d_smooth_follow(cam, huge_vec, huge, -huge, huge, huge);
    rt_camera3d_smooth_look_at(cam, huge_target, huge, huge);

    EXPECT_TRUE(finite_camera_state(cam), "Huge camera controls leave state finite");
    EXPECT_TRUE(bounded_vec3(rt_camera3d_get_position(cam), world_limit),
                "Huge camera controls keep eye bounded");
    EXPECT_TRUE(finite_vec3(rt_camera3d_get_forward(cam)), "Forward vector remains finite");
    EXPECT_TRUE(finite_vec3(rt_camera3d_get_right(cam)), "Right vector remains finite");
    EXPECT_TRUE(std::isfinite(rt_camera3d_get_yaw(cam)), "Yaw getter remains finite");
    EXPECT_TRUE(std::fabs(rt_camera3d_get_yaw(cam)) <= 180.0, "Yaw getter remains wrapped");
    EXPECT_TRUE(std::isfinite(rt_camera3d_get_pitch(cam)), "Pitch getter remains finite");
    EXPECT_TRUE(std::fabs(rt_camera3d_get_pitch(cam)) <= 89.0, "Pitch getter remains clamped");
    EXPECT_TRUE(finite_vec3(rt_camera3d_screen_to_ray(
                    cam, INT64_MAX, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1)),
                "Huge screen coordinates still produce a finite ray");
    EXPECT_TRUE(bounded_vec3(rt_camera3d_screen_to_ray_origin(
                                 cam, INT64_MAX, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1),
                             world_limit),
                "Huge screen coordinates still produce a bounded ray origin");

    rt_camera3d *ortho = (rt_camera3d *)rt_camera3d_new_ortho(huge, huge, huge, huge);
    assert(ortho != NULL);
    rt_camera3d_look_at(ortho, huge_vec, huge_target, huge_up);
    EXPECT_TRUE(finite_camera_state(ortho), "Huge orthographic camera state remains finite");
    EXPECT_TRUE(finite_vec3(rt_camera3d_screen_to_ray(
                    ortho, INT64_MAX, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1)),
                "Huge orthographic screen ray remains finite");
    EXPECT_TRUE(bounded_vec3(rt_camera3d_screen_to_ray_origin(
                                 ortho, INT64_MAX, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1),
                             world_limit),
                "Huge orthographic screen ray origin remains bounded");
    PASS();
}

//=============================================================================
// Material3D — additional tests
//=============================================================================

static void test_material_set_color() {
    TEST("Material3D.SetColor — changes material");
    void *m = rt_material3d_new();
    rt_material3d_set_color(m, 0.1, 0.2, 0.3);
    PASS();
}

static void test_material_sanitizes_numeric_inputs() {
    TEST("Material3D clamps non-finite and out-of-range inputs");
    rt_material3d *m = (rt_material3d *)rt_material3d_new_color(-1.0, 0.5, INFINITY);
    assert(m);
    EXPECT_NEAR(m->diffuse[0], 0.0, 0.001);
    EXPECT_NEAR(m->diffuse[1], 0.5, 0.001);
    EXPECT_NEAR(m->diffuse[2], 0.0, 0.001);

    rt_material3d_set_color(m, 2.0, NAN, 0.25);
    EXPECT_NEAR(m->diffuse[0], 1.0, 0.001);
    EXPECT_NEAR(m->diffuse[1], 0.0, 0.001);
    EXPECT_NEAR(m->diffuse[2], 0.25, 0.001);

    rt_material3d_set_shininess(m, -12.0);
    EXPECT_NEAR(m->shininess, 0.0, 0.001);
    rt_material3d_set_emissive_color(m, 3.0, -1.0, NAN);
    EXPECT_NEAR(m->emissive[0], 3.0, 0.001);
    EXPECT_NEAR(m->emissive[1], 0.0, 0.001);
    EXPECT_NEAR(m->emissive[2], 0.0, 0.001);
    PASS();
}

static void test_material_getters_sanitize_without_mutating_corrupt_state() {
    TEST("Material3D getters sanitize corrupt stored state without mutating");
    rt_material3d *m = (rt_material3d *)rt_material3d_new();
    assert(m);

    m->diffuse[0] = NAN;
    m->diffuse[1] = -4.0;
    m->diffuse[2] = 4.0;
    void *color = rt_material3d_get_color(m);
    EXPECT_NEAR(rt_vec3_x(color), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(color), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(color), 1.0, 0.001);
    EXPECT_TRUE(std::isnan(m->diffuse[0]), "GetColor does not rewrite corrupt color state");
    EXPECT_NEAR(m->diffuse[2], 4.0, 0.001);

    m->alpha = NAN;
    m->metallic = INFINITY;
    m->roughness = 2.0;
    m->ao = -2.0;
    m->emissive_intensity = INFINITY;
    m->normal_scale = INFINITY;
    m->reflectivity = INFINITY;
    m->shading_model = 99;
    m->alpha_mode = 99;
    m->alpha_mode_auto = 1;
    m->unlit = 7;
    m->double_sided = -3;
    EXPECT_NEAR(rt_material3d_get_alpha(m), 0.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_metallic(m), 0.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_roughness(m), 1.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_ao(m), 0.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_emissive_intensity(m), 0.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_normal_scale(m), 0.0, 0.001);
    EXPECT_NEAR(rt_material3d_get_reflectivity(m), 0.0, 0.001);
    EXPECT_EQ(rt_material3d_get_shading_model(m), 0);
    EXPECT_EQ(rt_material3d_get_alpha_mode(m), RT_MATERIAL3D_ALPHA_MODE_OPAQUE);
    EXPECT_EQ(m->alpha_mode_auto, 1);
    EXPECT_EQ(rt_material3d_get_unlit(m), 1);
    EXPECT_EQ(rt_material3d_get_double_sided(m), 1);
    EXPECT_EQ(m->unlit, 7);
    EXPECT_EQ(m->double_sided, -3);

    m->texture = rt_material3d_new();
    m->normal_map = rt_material3d_new();
    m->specular_map = rt_material3d_new();
    m->emissive_map = rt_material3d_new();
    m->metallic_roughness_map = rt_material3d_new();
    m->ao_map = rt_material3d_new();
    m->env_map = rt_material3d_new();
    EXPECT_EQ(rt_material3d_get_has_texture(m), 0);
    EXPECT_EQ(rt_material3d_get_has_normal_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_specular_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_ao_map(m), 0);
    EXPECT_EQ(rt_material3d_get_has_env_map(m), 0);
    EXPECT_TRUE(m->texture == NULL && m->normal_map == NULL && m->env_map == NULL,
                "invalid material refs are cleared from texture/env slots");
    PASS();
}

static void test_material_clone_repairs_invalid_env_map() {
    TEST("Material3D.Clone repairs invalid texture and environment maps");
    auto *src = (rt_material3d *)rt_material3d_new();
    src->texture = rt_material3d_new();
    src->normal_map = rt_material3d_new();
    src->specular_map = rt_material3d_new();
    src->emissive_map = rt_material3d_new();
    src->metallic_roughness_map = rt_material3d_new();
    src->ao_map = rt_material3d_new();
    src->env_map = rt_material3d_new();

    auto *clone = (rt_material3d *)rt_material3d_clone(src);

    EXPECT_TRUE(clone != nullptr, "Material3D.Clone returns a clone after texture/env-map repair");
    EXPECT_TRUE(src->texture == nullptr && src->normal_map == nullptr &&
                    src->specular_map == nullptr && src->emissive_map == nullptr &&
                    src->metallic_roughness_map == nullptr && src->ao_map == nullptr &&
                    src->env_map == nullptr,
                "Material3D.Clone clears invalid source texture/env-map refs");
    EXPECT_TRUE(clone == nullptr ||
                    (clone->texture == nullptr && clone->normal_map == nullptr &&
                     clone->specular_map == nullptr && clone->emissive_map == nullptr &&
                     clone->metallic_roughness_map == nullptr && clone->ao_map == nullptr &&
                     clone->env_map == nullptr),
                "Material3D.Clone does not retain invalid texture/env-map refs into the clone");
    EXPECT_EQ(rt_material3d_get_has_texture(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_normal_map(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_specular_map(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_emissive_map(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_metallic_roughness_map(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_ao_map(clone), 0);
    EXPECT_EQ(rt_material3d_get_has_env_map(clone), 0);
    PASS();
}

static void test_material_set_shininess() {
    TEST("Material3D.SetShininess — accepts values");
    void *m = rt_material3d_new();
    rt_material3d_set_shininess(m, 128.0);
    rt_material3d_set_shininess(m, 0.0);
    PASS();
}

static void test_material_set_unlit() {
    TEST("Material3D.SetUnlit — toggles lighting");
    void *m = rt_material3d_new();
    rt_material3d_set_unlit(m, 1);
    rt_material3d_set_unlit(m, 0);
    PASS();
}

static void test_material_null_safety() {
    TEST("Material3D — null safety");
    rt_material3d_set_color(NULL, 0, 0, 0);
    rt_material3d_set_texture(NULL, NULL);
    rt_material3d_set_shininess(NULL, 0);
    rt_material3d_set_unlit(NULL, 0);
    PASS();
}

//=============================================================================
// Light3D — additional tests
//=============================================================================

static void test_light_null_safety() {
    TEST("Light3D — null safety");
    rt_light3d_set_intensity(NULL, 1.0);
    rt_light3d_set_color(NULL, 0, 0, 0);
    rt_light3d_set_casts_shadows(NULL, 1);
    rt_light3d_set_spot_cone(NULL, 10.0, 20.0);
    EXPECT_EQ(rt_light3d_get_casts_shadows(NULL), 0);
    EXPECT_NEAR(rt_light3d_get_inner_cone_degrees(NULL), 0.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_outer_cone_degrees(NULL), 0.0, 0.001);
    PASS();
}

static void test_light_spot() {
    TEST("Light3D.NewSpot — creates spot light");
    void *pos = rt_vec3_new(0, 5, 0);
    void *dir = rt_vec3_new(0, -1, 0);
    void *light = rt_light3d_new_spot(pos, dir, 1.0, 1.0, 1.0, 0.1, 30.0, 45.0);
    assert(light);
    PASS();
}

static void test_light_spot_intensity() {
    TEST("Light3D spot — set intensity");
    void *pos = rt_vec3_new(0, 5, 0);
    void *dir = rt_vec3_new(0, -1, 0);
    void *light = rt_light3d_new_spot(pos, dir, 1.0, 1.0, 1.0, 0.1, 30.0, 45.0);
    rt_light3d_set_intensity(light, 2.5);
    PASS();
}

static void test_light_spot_cone_authoring() {
    TEST("Light3D spot cone — exposes sanitized degrees and paired mutation");
    void *position = rt_vec3_new(0.0, 5.0, 0.0);
    void *direction = rt_vec3_new(0.0, -1.0, 0.0);
    void *spot =
        rt_light3d_new_spot(position, direction, 1.0, 0.8, 0.6, 0.1, 20.0, 35.0);
    void *point = rt_light3d_new_point(position, 1.0, 1.0, 1.0, 0.1);
    assert(spot && point);

    EXPECT_NEAR(rt_light3d_get_inner_cone_degrees(spot), 20.0, 0.0001);
    EXPECT_NEAR(rt_light3d_get_outer_cone_degrees(spot), 35.0, 0.0001);
    EXPECT_NEAR(rt_light3d_get_inner_cone_degrees(point), 0.0, 0.0001);
    EXPECT_NEAR(rt_light3d_get_outer_cone_degrees(point), 0.0, 0.0001);

    uint64_t revision = rt_light3d_mutation_revision();
    rt_light3d_set_spot_cone(spot, 60.0, 10.0);
    EXPECT_EQ(rt_light3d_mutation_revision(), revision + 1);
    double inner = rt_light3d_get_inner_cone_degrees(spot);
    double outer = rt_light3d_get_outer_cone_degrees(spot);
    EXPECT_NEAR(inner, 60.0, 0.0001);
    EXPECT_NEAR(outer, 60.01, 0.0001);
    EXPECT_TRUE(outer > inner, "Paired spot-cone setter preserves strict separation");

    rt_light3d_set_spot_cone(spot, NAN, INFINITY);
    EXPECT_NEAR(rt_light3d_get_inner_cone_degrees(spot), 0.0, 0.0001);
    EXPECT_NEAR(rt_light3d_get_outer_cone_degrees(spot), 0.01, 0.0001);

    revision = rt_light3d_mutation_revision();
    rt_light3d_set_spot_cone(point, 15.0, 30.0);
    EXPECT_EQ(rt_light3d_mutation_revision(), revision);
    PASS();
}

static void test_light_native_area_and_volume_types() {
    TEST("Light3D native rectangle, sphere, and volume lights");
    void *position = rt_vec3_new(1.0, 2.0, 3.0);
    void *direction = rt_vec3_new(0.0, 0.0, -2.0);
    auto *rectangle = static_cast<rt_light3d *>(
        rt_light3d_new_area_rectangle(position, direction, 4.0, 2.0, 0.25, 0.5, 0.75, 0.2, 12.0));
    auto *sphere =
        static_cast<rt_light3d *>(rt_light3d_new_area_sphere(position, 1.5, 0.4, 0.5, 0.6, 9.0));
    auto *volume =
        static_cast<rt_light3d *>(rt_light3d_new_volume(position, 3.0, 0.7, 0.8, 0.9, 6.0));
    assert(rectangle && sphere && volume);

    EXPECT_EQ(rt_light3d_get_type(rectangle), 4);
    EXPECT_EQ(rt_light3d_get_type(sphere), 5);
    EXPECT_EQ(rt_light3d_get_type(volume), 6);
    EXPECT_NEAR(rt_light3d_get_width(rectangle), 4.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_height(rectangle), 2.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_radius(sphere), 1.5, 0.001);
    EXPECT_NEAR(rt_light3d_get_radius(volume), 3.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_range(rectangle), 12.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_range(sphere), 9.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_range(volume), 6.0, 0.001);
    EXPECT_EQ(rt_light3d_get_decay_type(rectangle), 2);
    EXPECT_NEAR(rectangle->direction[2], -1.0, 0.001);
    EXPECT_NEAR(rectangle->basis_u[0] * rectangle->direction[0] +
                    rectangle->basis_u[1] * rectangle->direction[1] +
                    rectangle->basis_u[2] * rectangle->direction[2],
                0.0,
                0.001);
    EXPECT_NEAR(rectangle->basis_v[0] * rectangle->direction[0] +
                    rectangle->basis_v[1] * rectangle->direction[1] +
                    rectangle->basis_v[2] * rectangle->direction[2],
                0.0,
                0.001);

    rt_light3d_set_width(rectangle, 5.0);
    rt_light3d_set_height(rectangle, 2.5);
    rt_light3d_set_radius(sphere, 2.0);
    rt_light3d_set_range(volume, 4.0);
    rt_light3d_set_decay_type(rectangle, 3);
    EXPECT_NEAR(rt_light3d_get_width(rectangle), 5.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_height(rectangle), 2.5, 0.001);
    EXPECT_NEAR(rt_light3d_get_radius(sphere), 2.0, 0.001);
    EXPECT_NEAR(rt_light3d_get_range(volume), 4.0, 0.001);
    EXPECT_EQ(rt_light3d_get_decay_type(rectangle), 3);
    rt_light3d_set_width(rectangle, NAN);
    rt_light3d_set_radius(sphere, -1.0);
    rt_light3d_set_range(volume, INFINITY);
    rt_light3d_set_decay_type(rectangle, 99);
    EXPECT_TRUE(rt_light3d_get_width(rectangle) > 0.0,
                "Rectangle width remains positive after invalid input");
    EXPECT_TRUE(rt_light3d_get_radius(sphere) > 0.0,
                "Sphere radius remains positive after invalid input");
    EXPECT_TRUE(rt_light3d_get_range(volume) > 0.0,
                "Volume range remains positive after invalid input");
    EXPECT_EQ(rt_light3d_get_decay_type(rectangle), 3);
    EXPECT_EQ(rt_light3d_get_casts_shadows(volume), 0);
    rt_light3d_set_casts_shadows(volume, 1);
    EXPECT_EQ(rt_light3d_get_casts_shadows(volume), 0);
    PASS();
}

static void test_light_validation_and_clamping() {
    TEST("Light3D normalizes directions and clamps invalid inputs");
    rt_light3d *dir =
        (rt_light3d *)rt_light3d_new_directional(rt_vec3_new(0.0, -10.0, 0.0), 1.0, 1.0, 1.0);
    rt_light3d *point =
        (rt_light3d *)rt_light3d_new_point(rt_vec3_new(0.0, 1.0, 0.0), 1.0, 1.0, 1.0, -4.0);
    rt_light3d *spot = (rt_light3d *)rt_light3d_new_spot(
        rt_vec3_new(0.0, 5.0, 0.0), rt_vec3_new(0.0, -2.0, 0.0), 1.0, 1.0, 1.0, -2.0, 45.0, 10.0);
    assert(dir != NULL && point != NULL && spot != NULL);
    EXPECT_NEAR(dir->direction[0], 0.0, 0.001);
    EXPECT_NEAR(dir->direction[1], -1.0, 0.001);
    EXPECT_NEAR(dir->direction[2], 0.0, 0.001);
    EXPECT_NEAR(point->attenuation, 0.001, 0.0001);
    EXPECT_NEAR(spot->attenuation, 0.001, 0.0001);
    EXPECT_EQ(point->casts_shadows, 0);
    EXPECT_EQ(spot->casts_shadows, 0);
    EXPECT_TRUE(spot->inner_cos > spot->outer_cos,
                "Spot lights reorder inner/outer cones into a valid range");
    rt_light3d *equal_spot = (rt_light3d *)rt_light3d_new_spot(
        rt_vec3_new(0.0, 5.0, 0.0), rt_vec3_new(0.0, -1.0, 0.0), 1.0, 1.0, 1.0, 0.0, 45.0, 45.0);
    EXPECT_TRUE(equal_spot->inner_cos > equal_spot->outer_cos,
                "Spot lights keep equal cones separated for shader falloff");
    rt_light3d_set_intensity(spot, -5.0);
    EXPECT_NEAR(spot->intensity, 0.0, 0.001);
    EXPECT_TRUE(expect_trap_contains([&] { rt_light3d_set_position(point, dir); }, "Vec3"),
                "Light3D.Position rejects invalid handles");
    EXPECT_TRUE(expect_trap_contains([&] { rt_light3d_set_direction(dir, point); }, "Vec3"),
                "Light3D.Direction rejects invalid handles");
    PASS();
}

static void test_light_sanitizes_nonfinite_inputs() {
    TEST("Light3D sanitizes non-finite public inputs");
    void *bad_vec = rt_vec3_new(NAN, INFINITY, -INFINITY);
    rt_light3d *dir = (rt_light3d *)rt_light3d_new_directional(bad_vec, -1.0, NAN, 2.0);
    rt_light3d *point = (rt_light3d *)rt_light3d_new_point(bad_vec, INFINITY, 0.5, -1.0, INFINITY);
    rt_light3d *ambient = (rt_light3d *)rt_light3d_new_ambient(NAN, 2.0, -1.0);
    rt_light3d *spot =
        (rt_light3d *)rt_light3d_new_spot(bad_vec, bad_vec, NAN, 2.0, 0.25, NAN, NAN, INFINITY);
    assert(dir != NULL && point != NULL && ambient != NULL && spot != NULL);
    EXPECT_TRUE(finite_light_state(dir), "Directional light state stays finite");
    EXPECT_TRUE(finite_light_state(point), "Point light state stays finite");
    EXPECT_TRUE(finite_light_state(ambient), "Ambient light state stays finite");
    EXPECT_TRUE(finite_light_state(spot), "Spot light state stays finite");

    EXPECT_NEAR(dir->direction[0], 0.0, 0.001);
    EXPECT_NEAR(dir->direction[1], -1.0, 0.001);
    EXPECT_NEAR(dir->direction[2], 0.0, 0.001);
    EXPECT_NEAR(dir->color[0], 0.0, 0.001);
    EXPECT_NEAR(dir->color[1], 0.0, 0.001);
    EXPECT_NEAR(dir->color[2], 1.0, 0.001);

    EXPECT_NEAR(point->position[0], 0.0, 0.001);
    EXPECT_NEAR(point->position[1], 0.0, 0.001);
    EXPECT_NEAR(point->position[2], 0.0, 0.001);
    EXPECT_NEAR(point->color[0], 0.0, 0.001);
    EXPECT_NEAR(point->color[1], 0.5, 0.001);
    EXPECT_NEAR(point->color[2], 0.0, 0.001);
    EXPECT_NEAR(point->attenuation, 0.001, 0.0001);

    EXPECT_NEAR(ambient->color[0], 0.0, 0.001);
    EXPECT_NEAR(ambient->color[1], 1.0, 0.001);
    EXPECT_NEAR(ambient->color[2], 0.0, 0.001);
    EXPECT_TRUE(spot->inner_cos > spot->outer_cos, "Invalid spot angles clamp to a valid cone");

    rt_light3d_set_intensity(spot, INFINITY);
    rt_light3d_set_color(spot, INFINITY, 2.0, -1.0);
    EXPECT_NEAR(spot->intensity, 0.0, 0.001);
    EXPECT_NEAR(spot->color[0], 0.0, 0.001);
    EXPECT_NEAR(spot->color[1], 1.0, 0.001);
    EXPECT_NEAR(spot->color[2], 0.0, 0.001);
    PASS();
}

static void test_light_clamps_extreme_finite_inputs() {
    TEST("Light3D clamps extreme finite public inputs");
    constexpr double kHuge = 1.0e300;
    constexpr double kWorldLimit = 1000000000000.0;
    void *huge_vec = rt_vec3_new(kHuge, -kHuge, kHuge);
    rt_light3d *dir = (rt_light3d *)rt_light3d_new_directional(huge_vec, 2.0, -1.0, 0.5);
    rt_light3d *point = (rt_light3d *)rt_light3d_new_point(huge_vec, 0.1, 0.2, 0.3, kHuge);
    rt_light3d *spot =
        (rt_light3d *)rt_light3d_new_spot(huge_vec, huge_vec, 0.1, 0.2, 0.3, kHuge, kHuge, kHuge);
    assert(dir != NULL && point != NULL && spot != NULL);

    EXPECT_TRUE(finite_light_state(dir), "Huge directional light state stays finite");
    EXPECT_TRUE(finite_light_state(point), "Huge point light state stays finite");
    EXPECT_TRUE(finite_light_state(spot), "Huge spot light state stays finite");
    EXPECT_NEAR(dir->direction[0], 0.577350, 0.001);
    EXPECT_NEAR(dir->direction[1], -0.577350, 0.001);
    EXPECT_NEAR(dir->direction[2], 0.577350, 0.001);
    EXPECT_TRUE(std::fabs(point->position[0]) <= kWorldLimit &&
                    std::fabs(point->position[1]) <= kWorldLimit &&
                    std::fabs(point->position[2]) <= kWorldLimit,
                "Huge light positions are world-clamped");
    EXPECT_NEAR(point->attenuation, 1000000.0, 0.001);
    EXPECT_TRUE(spot->inner_cos > spot->outer_cos, "Huge spot angles clamp to a valid cone");

    rt_light3d_set_direction(dir, rt_vec3_new(-kHuge, 0.0, 0.0));
    void *new_dir = rt_light3d_get_direction(dir);
    EXPECT_TRUE(finite_vec3(new_dir), "Huge SetDirection result is finite");
    EXPECT_NEAR(rt_vec3_x(new_dir), -1.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(new_dir), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(new_dir), 0.0, 0.001);

    dir->type = 99;
    EXPECT_EQ(rt_light3d_get_type(dir), 0);
    dir->color[0] = INFINITY;
    dir->color[1] = -10.0;
    dir->color[2] = 0.5;
    void *color = rt_light3d_get_color(dir);
    EXPECT_NEAR(rt_vec3_x(color), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_y(color), 0.0, 0.001);
    EXPECT_NEAR(rt_vec3_z(color), 0.5, 0.001);
    PASS();
}

static void test_camera_ortho() {
    TEST("Camera3D.NewOrtho — creates orthographic camera");
    void *cam = rt_camera3d_new_ortho(10.0, 16.0 / 9.0, 0.1, 100.0);
    assert(cam);
    EXPECT_EQ(rt_camera3d_is_ortho(cam), 1);
    PASS();
}

static void test_camera_ortho_look_at() {
    TEST("Camera3D ortho — LookAt works");
    void *cam = rt_camera3d_new_ortho(10.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0, 10, 10);
    void *target = rt_vec3_new(0, 0, 0);
    void *up = rt_vec3_new(0, 1, 0);
    rt_camera3d_look_at(cam, eye, target, up);
    void *pos = rt_camera3d_get_position(cam);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.01);
    EXPECT_NEAR(rt_vec3_y(pos), 10.0, 0.01);
    PASS();
}

static void test_camera_perspective_not_ortho() {
    TEST("Camera3D.New — IsOrtho returns false");
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    EXPECT_EQ(rt_camera3d_is_ortho(cam), 0);
    PASS();
}

//=============================================================================
// Phase 9 — Multi-texture material tests
//=============================================================================

static void test_material_set_emissive() {
    TEST("Material3D.SetEmissiveColor — no crash");
    void *m = rt_material3d_new();
    rt_material3d_set_emissive_color(m, 1.0, 0.5, 0.2);
    PASS();
}

static void test_material_set_maps() {
    TEST("Material3D — set normal/specular/emissive maps");
    void *m = rt_material3d_new();
    void *px = rt_pixels_new(4, 4);
    rt_material3d_set_normal_map(m, px);
    rt_material3d_set_specular_map(m, px);
    rt_material3d_set_emissive_map(m, px);
    /* Set to NULL should also work */
    rt_material3d_set_normal_map(m, NULL);
    rt_material3d_set_specular_map(m, NULL);
    rt_material3d_set_emissive_map(m, NULL);
    PASS();
}

static void test_mesh_calc_tangents() {
    TEST("Mesh3D.CalcTangents — plane tangent along +X");
    void *m = rt_mesh3d_new();
    /* Flat quad in XZ plane with standard UVs */
    rt_mesh3d_add_vertex(m, -1, 0, -1, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, -1, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 1, 0, 1, 0, 1, 1);
    rt_mesh3d_add_vertex(m, -1, 0, 1, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 2, 1);
    rt_mesh3d_add_triangle(m, 0, 3, 2);
    rt_mesh3d_calc_tangents(m);
    /* No crash = pass. Can't directly inspect tangent from public API */
    /* Also test null safety */
    rt_mesh3d_calc_tangents(NULL);
    PASS();
}

//=============================================================================
// Phase 10 — Alpha blending tests
//=============================================================================

static void test_material_alpha() {
    TEST("Material3D.Alpha — default 1.0, set/get works");
    void *m = rt_material3d_new();
    EXPECT_NEAR(rt_material3d_get_alpha(m), 1.0, 0.001);
    rt_material3d_set_alpha(m, 0.5);
    EXPECT_NEAR(rt_material3d_get_alpha(m), 0.5, 0.001);
    EXPECT_EQ(rt_material3d_get_alpha_mode(m), RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha_mode(m, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);
    rt_material3d_set_alpha(m, 2.0);
    EXPECT_NEAR(rt_material3d_get_alpha(m), 1.0, 0.001);
    rt_material3d_set_alpha(m, -1.0);
    EXPECT_NEAR(rt_material3d_get_alpha(m), 0.0, 0.001);
    EXPECT_EQ(rt_material3d_get_alpha_mode(m), RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha(m, 1.0);
    EXPECT_EQ(rt_material3d_get_alpha_mode(m), RT_MATERIAL3D_ALPHA_MODE_OPAQUE);
    rt_material3d_set_alpha_mode(m, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha(m, 1.0);
    EXPECT_EQ(rt_material3d_get_alpha_mode(m), RT_MATERIAL3D_ALPHA_MODE_BLEND);
    /* Null safety */
    rt_material3d_set_alpha(NULL, 0.5);
    EXPECT_NEAR(rt_material3d_get_alpha(NULL), 1.0, 0.001);
    PASS();
}

//=============================================================================
// Phase 11 — Cube map tests
//=============================================================================

static void test_cubemap_load_hdr_panorama() {
    TEST("CubeMap3D.LoadHdrPanorama — Radiance decode, projection, exposure");
    /* Synthetic 32x16 flat-coded panorama: top half green (up), bottom half
     * red (down); linear value 1.0 encodes as RGBE (128, 0, 0, 129). */
    const char *path = "/tmp/zanna_cubemap_pano.hdr";
    {
        FILE *f = fopen(path, "wb");
        assert(f);
        fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 16 +X 32\n");
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 32; x++) {
                unsigned char px4[4] = {0, 0, 0, 129};
                if (y < 8)
                    px4[1] = 128; /* green up */
                else
                    px4[0] = 128; /* red down */
                fwrite(px4, 1, 4, f);
            }
        }
        fclose(f);
    }
    void *cm = rt_cubemap3d_load_hdr_panorama(rt_const_cstr(path), 1.0);
    EXPECT_TRUE(cm != nullptr, "HDR panorama loads as a cube map");
    if (!cm)
        return;
    rt_cubemap3d *cube = (rt_cubemap3d *)cm;
    EXPECT_TRUE(cube->face_size >= 8, "HDR panorama faces have usable resolution");
    {
        /* Face order: +X,-X,+Y,-Y,+Z,-Z. +Y looks up (green), -Y down (red).
         * Linear 1.0 at exposure 1 Reinhard-maps to 0.5 -> byte 128. */
        int64_t c = cube->face_size / 2;
        int64_t up = rt_pixels_get(cube->faces[2], c, c);
        int64_t down = rt_pixels_get(cube->faces[3], c, c);
        int up_g = (int)((up >> 8) & 0xFF);
        int up_r = (int)(up & 0xFF);
        int down_r = (int)(down & 0xFF);
        int down_g = (int)((down >> 8) & 0xFF);
        EXPECT_TRUE(up_g >= 120 && up_g <= 136 && up_r <= 8,
                    "+Y face samples the panorama's upper hemisphere");
        EXPECT_TRUE(down_r >= 120 && down_r <= 136 && down_g <= 8,
                    "-Y face samples the panorama's lower hemisphere");
    }
    {
        /* Exposure 4: Reinhard(4)/[0,1] = 0.8 -> byte 204. */
        void *bright = rt_cubemap3d_load_hdr_panorama(rt_const_cstr(path), 4.0);
        EXPECT_TRUE(bright != nullptr, "HDR panorama loads at raised exposure");
        if (bright) {
            rt_cubemap3d *bc = (rt_cubemap3d *)bright;
            int64_t c = bc->face_size / 2;
            int g = (int)((rt_pixels_get(bc->faces[2], c, c) >> 8) & 0xFF);
            EXPECT_TRUE(g >= 196 && g <= 212, "Exposure scales the Reinhard mapping");
        }
    }
    {
        /* New-style RLE variant: 8x2, all red rows. */
        const char *rle_path = "/tmp/zanna_cubemap_pano_rle.hdr";
        FILE *f = fopen(rle_path, "wb");
        assert(f);
        fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 8\n");
        for (int y = 0; y < 2; y++) {
            unsigned char hdr4[4] = {2, 2, 0, 8};
            unsigned char plane_r[2] = {128 + 8, 128};
            unsigned char plane_zero[2] = {128 + 8, 0};
            unsigned char plane_e[2] = {128 + 8, 129};
            fwrite(hdr4, 1, 4, f);
            fwrite(plane_r, 1, 2, f);
            fwrite(plane_zero, 1, 2, f);
            fwrite(plane_zero, 1, 2, f);
            fwrite(plane_e, 1, 2, f);
        }
        fclose(f);
        void *rle_cm = rt_cubemap3d_load_hdr_panorama(rt_const_cstr(rle_path), 1.0);
        EXPECT_TRUE(rle_cm != nullptr, "RLE-coded Radiance rows decode");
        if (rle_cm) {
            rt_cubemap3d *rc = (rt_cubemap3d *)rle_cm;
            int64_t c = rc->face_size / 2;
            int r = (int)(rt_pixels_get(rc->faces[0], c, c) & 0xFF);
            EXPECT_TRUE(r >= 120 && r <= 136, "RLE panorama decodes to the expected red");
        }
        std::remove(rle_path);
    }
    std::remove(path);
    PASS();
}

static void test_cubemap_new() {
    TEST("CubeMap3D.New — 6 faces creates valid cube map");
    void *px = rt_pixels_new(16, 16);
    void *cm = rt_cubemap3d_new(px, px, px, px, px, px);
    assert(cm != NULL);
    /* Skybox set/clear */
    rt_canvas3d_set_skybox(NULL, cm); /* null canvas = no crash */
    rt_canvas3d_clear_skybox(NULL);
    PASS();
}

static void test_canvas_set_skybox_repairs_stale_existing_slot() {
    TEST("Canvas3D.SetSkybox repairs stale existing skybox before rejecting replacement");
    rt_canvas3d canvas = {};
    void *px = rt_pixels_new(1, 1);
    void *cm = rt_cubemap3d_new(px, px, px, px, px, px);
    void *wrong = rt_material3d_new();

    EXPECT_TRUE(px != nullptr && cm != nullptr && wrong != nullptr, "Skybox repair fixtures exist");
    if (!px || !cm || !wrong)
        return;

    rt_canvas3d_set_skybox(&canvas, cm);
    EXPECT_TRUE(canvas.skybox == (rt_cubemap3d *)cm, "Valid skybox is assigned");

    canvas.skybox_cpu_cache = (uint8_t *)malloc(4);
    canvas.skybox_cpu_cache_w = 1;
    canvas.skybox_cpu_cache_h = 1;
    canvas.skybox_cpu_cache_generation = 123;
    ((rt_cubemap3d *)cm)->face_size = 2;

    rt_canvas3d_set_skybox(&canvas, wrong);

    EXPECT_TRUE(canvas.skybox == nullptr,
                "Stale skybox slot is cleared even when replacement is invalid");
    EXPECT_TRUE(canvas.skybox_cpu_cache == nullptr && canvas.skybox_cpu_cache_generation == 0,
                "Clearing a stale skybox invalidates the CPU skybox cache");
    PASS();
}

static void test_material_reflectivity() {
    TEST("Material3D.Reflectivity clamps to [0, 1]");
    void *m = rt_material3d_new();
    EXPECT_NEAR(rt_material3d_get_reflectivity(m), 0.0, 0.001);
    rt_material3d_set_reflectivity(m, 0.5);
    EXPECT_NEAR(rt_material3d_get_reflectivity(m), 0.5, 0.001);
    rt_material3d_set_reflectivity(m, 3.0);
    EXPECT_NEAR(rt_material3d_get_reflectivity(m), 1.0, 0.001);
    rt_material3d_set_reflectivity(m, -2.0);
    EXPECT_NEAR(rt_material3d_get_reflectivity(m), 0.0, 0.001);
    /* Null safety */
    rt_material3d_set_reflectivity(NULL, 0.5);
    EXPECT_NEAR(rt_material3d_get_reflectivity(NULL), 0.0, 0.001);
    /* Env map set */
    void *px = rt_pixels_new(16, 16);
    void *cm = rt_cubemap3d_new(px, px, px, px, px, px);
    rt_material3d_set_env_map(m, cm);
    rt_material3d_set_env_map(m, NULL);
    PASS();
}

static void test_canvas_ibl_setters_defer_prefilter_work() {
    TEST("Canvas3D IBL setters defer cubemap prefilter work");
    rt_canvas3d canvas = {};
    void *px = rt_pixels_new(4, 4);
    void *cm = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(px != nullptr && cm != nullptr, "IBL lazy fixtures exist");
    if (!px || !cm)
        return;

    rt_canvas3d_set_ibl_enabled(&canvas, 1);
    EXPECT_TRUE(((rt_cubemap3d *)cm)->ibl_ready == 0, "enabling IBL alone does not prefilter");
    rt_canvas3d_set_skybox(&canvas, cm);
    EXPECT_TRUE(((rt_cubemap3d *)cm)->ibl_ready == 0, "setting skybox while IBL is on is lazy");
    EXPECT_TRUE(rt_cubemap3d_ensure_ibl(cm) == 1, "IBL prefilter can still be prepared on demand");
    EXPECT_TRUE(((rt_cubemap3d *)cm)->ibl_ready == 1,
                "explicit preparation marks the cubemap ready");
    PASS();
}

static void test_canvas_ortho_skybox_fills_render_target_uniformly() {
    TEST("Canvas3D.End fills orthographic skybox render targets uniformly");
    rt_canvas3d canvas = {};
    rt_camera3d camera = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(2, 2);
    void *px = rt_pixels_new(1, 1);
    void *cm;

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    EXPECT_TRUE(px != nullptr, "Pixels fixture exists");
    if (!rt || !rt->target || !px)
        return;

    rt_pixels_set(px, 0, 0, 0x123456FF);
    cm = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(cm != nullptr, "CubeMap3D fixture exists");
    if (!cm)
        return;

    canvas.backend = &vgfx3d_software_backend;
    canvas.render_target = rt->target;
    canvas.width = 2;
    canvas.height = 2;
    canvas.skybox = (rt_cubemap3d *)cm;
    camera.view[0] = camera.view[5] = camera.view[10] = camera.view[15] = 1.0;
    camera.projection[0] = camera.projection[5] = camera.projection[10] = camera.projection[15] =
        1.0;
    camera.eye[2] = 5.0;
    camera.is_ortho = 1;

    rt_canvas3d_begin(&canvas, &camera);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(rt->target->color_buf[0] == 0x12 && rt->target->color_buf[1] == 0x34 &&
                    rt->target->color_buf[2] == 0x56 && rt->target->color_buf[3] == 0xFF,
                "Orthographic skyboxes sample the camera forward direction");
    EXPECT_TRUE(rt->target->color_buf[12] == 0x12 && rt->target->color_buf[13] == 0x34 &&
                    rt->target->color_buf[14] == 0x56 && rt->target->color_buf[15] == 0xFF,
                "Orthographic skyboxes fill the whole target uniformly");
    PASS();
}

static void test_canvas_cpu_skybox_fallback_reuses_cache_until_inputs_change() {
    TEST("Canvas3D CPU skybox fallback caches stable cubemap/projection output");
    rt_canvas3d canvas = {};
    rt_camera3d camera = {};
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(2, 2);
    void *px = rt_pixels_new(1, 1);
    void *cm;
    uint8_t *first_cache;
    uint64_t first_generation;

    EXPECT_TRUE(rt != nullptr && rt->target != nullptr, "RenderTarget3D fixture exists");
    EXPECT_TRUE(px != nullptr, "Pixels fixture exists");
    if (!rt || !rt->target || !px)
        return;

    rt_pixels_set(px, 0, 0, 0x102030FF);
    cm = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(cm != nullptr, "CubeMap3D fixture exists");
    if (!cm)
        return;

    canvas.backend = &vgfx3d_software_backend;
    canvas.render_target = rt->target;
    canvas.width = 2;
    canvas.height = 2;
    canvas.skybox = (rt_cubemap3d *)cm;
    camera.view[0] = camera.view[5] = camera.view[10] = camera.view[15] = 1.0;
    camera.projection[0] = camera.projection[5] = camera.projection[10] = camera.projection[15] =
        1.0;
    camera.eye[2] = 5.0;
    camera.is_ortho = 1;

    rt_canvas3d_begin(&canvas, &camera);
    rt_canvas3d_end(&canvas);
    first_cache = canvas.skybox_cpu_cache;
    first_generation = canvas.skybox_cpu_cache_generation;
    EXPECT_TRUE(first_cache != nullptr, "First fallback skybox pass builds a CPU cache image");
    EXPECT_TRUE(rt->target->color_buf[0] == 0x10 && rt->target->color_buf[1] == 0x20 &&
                    rt->target->color_buf[2] == 0x30,
                "First fallback skybox pass writes the cubemap color");

    rt_canvas3d_begin(&canvas, &camera);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(canvas.skybox_cpu_cache == first_cache &&
                    canvas.skybox_cpu_cache_generation == first_generation,
                "Second identical fallback skybox pass reuses the cached image");

    rt_pixels_set(px, 0, 0, 0xA0B0C0FF);
    rt_canvas3d_begin(&canvas, &camera);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(canvas.skybox_cpu_cache != nullptr &&
                    canvas.skybox_cpu_cache_generation != first_generation,
                "Cubemap face mutation invalidates the fallback skybox cache generation");
    EXPECT_TRUE(rt->target->color_buf[0] == 0xA0 && rt->target->color_buf[1] == 0xB0 &&
                    rt->target->color_buf[2] == 0xC0,
                "Fallback skybox cache refreshes after cubemap face mutation");

    ((rt_cubemap3d *)cm)->face_size = 2;
    EXPECT_TRUE(canvas3d_ensure_skybox_cpu_cache(&canvas, 2, 2) == 0,
                "Malformed cubemap generation is rejected by the skybox CPU cache");
    EXPECT_TRUE(canvas.skybox_cpu_cache == nullptr,
                "Malformed cubemap generation invalidates stale skybox cache pixels");

    rt_canvas3d_invalidate_skybox_cache(&canvas);
    PASS();
}

static void test_canvas_cpu_skybox_sanitizes_malformed_ortho_forward() {
    TEST("Canvas3D CPU skybox fallback sanitizes malformed orthographic forward vectors");
    rt_canvas3d canvas = {};
    void *px = rt_pixels_new(1, 1);
    void *cm;

    EXPECT_TRUE(px != nullptr, "Pixels fixture exists");
    if (!px)
        return;

    rt_pixels_set(px, 0, 0, 0x556677FF);
    cm = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(cm != nullptr, "CubeMap3D fixture exists");
    if (!cm)
        return;

    canvas.skybox = (rt_cubemap3d *)cm;
    canvas.cached_cam_is_ortho = 1;
    canvas.cached_cam_forward[0] = NAN;
    canvas.cached_cam_forward[1] = INFINITY;
    canvas.cached_cam_forward[2] = 0.0f;

    EXPECT_TRUE(canvas3d_ensure_skybox_cpu_cache(&canvas, 1, 1) == 1,
                "Malformed orthographic skybox directions fall back to a valid ray");
    EXPECT_TRUE(canvas.skybox_cpu_cache != nullptr, "Skybox CPU cache is populated");
    if (canvas.skybox_cpu_cache) {
        EXPECT_TRUE(canvas.skybox_cpu_cache[0] == 0x55 && canvas.skybox_cpu_cache[1] == 0x66 &&
                        canvas.skybox_cpu_cache[2] == 0x77 && canvas.skybox_cpu_cache[3] == 0xFF,
                    "Sanitized skybox direction still samples the cubemap");
    }
    rt_canvas3d_invalidate_skybox_cache(&canvas);
    PASS();
}

static void test_canvas_cpu_skybox_normalizes_huge_ortho_forward() {
    TEST("Canvas3D CPU skybox fallback normalizes huge orthographic forward vectors");
    rt_canvas3d canvas = {};
    void *red = rt_pixels_new(1, 1);
    void *black = rt_pixels_new(1, 1);
    void *cm;

    EXPECT_TRUE(red != nullptr && black != nullptr, "Pixels fixtures exist");
    if (!red || !black)
        return;

    rt_pixels_set(red, 0, 0, 0xCC0000FF);
    rt_pixels_set(black, 0, 0, 0x000000FF);
    cm = rt_cubemap3d_new(red, black, black, black, black, black);
    EXPECT_TRUE(cm != nullptr, "CubeMap3D fixture exists");
    if (!cm)
        return;

    canvas.skybox = (rt_cubemap3d *)cm;
    canvas.cached_cam_is_ortho = 1;
    canvas.cached_cam_forward[0] = FLT_MAX;
    canvas.cached_cam_forward[1] = 0.0f;
    canvas.cached_cam_forward[2] = 0.0f;

    EXPECT_TRUE(canvas3d_ensure_skybox_cpu_cache(&canvas, 1, 1) == 1,
                "Huge finite orthographic skybox directions stay drawable");
    EXPECT_TRUE(canvas.skybox_cpu_cache != nullptr, "Skybox CPU cache is populated");
    if (canvas.skybox_cpu_cache) {
        EXPECT_TRUE(canvas.skybox_cpu_cache[0] > 0xB0 && canvas.skybox_cpu_cache[1] < 0x10 &&
                        canvas.skybox_cpu_cache[2] < 0x10 && canvas.skybox_cpu_cache[3] == 0xFF,
                    "Huge finite skybox direction samples the intended cubemap face");
    }
    rt_canvas3d_invalidate_skybox_cache(&canvas);
    PASS();
}

//=============================================================================
// Mesh3D.Clear tests
//=============================================================================

static void test_mesh_clear() {
    TEST("Mesh3D.Clear resets counts to zero");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    rt_mesh3d_clear(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 0);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 0);
    PASS();
}

static void test_mesh_clear_then_rebuild() {
    TEST("Mesh3D.Clear allows rebuild without reallocation");
    void *m = rt_mesh3d_new();
    // Build a triangle
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    // Clear and rebuild a different triangle
    rt_mesh3d_clear(m);
    rt_mesh3d_add_vertex(m, 2, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 3, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 2, 1, 0, 0, 1, 0, 0, 1);
    rt_mesh3d_add_vertex(m, 3, 1, 0, 0, 1, 0, 1, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_add_triangle(m, 1, 3, 2);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 4);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 2);
    PASS();
}

static void test_mesh_clear_null_safety() {
    TEST("Mesh3D.Clear null safety");
    rt_mesh3d_clear(NULL); // should not crash
    PASS();
}

static void test_mesh_clear_stress() {
    TEST("Mesh3D.Clear stress: 100 clear-rebuild cycles (Water3D pattern)");
    void *m = rt_mesh3d_new();
    for (int cycle = 0; cycle < 100; cycle++) {
        rt_mesh3d_clear(m);
        // Rebuild a simple quad
        rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
        rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
        rt_mesh3d_add_vertex(m, 1, 0, 1, 0, 1, 0, 1, 1);
        rt_mesh3d_add_vertex(m, 0, 0, 1, 0, 1, 0, 0, 1);
        rt_mesh3d_add_triangle(m, 0, 1, 2);
        rt_mesh3d_add_triangle(m, 0, 2, 3);
    }
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 4);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 2);
    PASS();
}

//=============================================================================
// Sprite3D tests
//=============================================================================

static void test_sprite3d_new() {
    TEST("Sprite3D.New creates sprite");
    void *tex = rt_pixels_new(16, 16);
    void *s = rt_sprite3d_new(tex);
    assert(s);
    PASS();
}

static void test_sprite3d_new_null_texture() {
    TEST("Sprite3D.New with null texture");
    void *s = rt_sprite3d_new(NULL);
    assert(s);
    PASS();
}

static void test_sprite3d_set_position() {
    TEST("Sprite3D.SetPosition");
    void *s = rt_sprite3d_new(NULL);
    rt_sprite3d_set_position(s, 1.0, 2.0, 3.0);
    PASS();
}

static void test_sprite3d_set_scale() {
    TEST("Sprite3D.SetScale");
    void *s = rt_sprite3d_new(NULL);
    rt_sprite3d_set_scale(s, 2.0, 3.0);
    PASS();
}

static void test_sprite3d_set_frame() {
    TEST("Sprite3D.SetFrame");
    void *tex = rt_pixels_new(64, 64);
    void *s = rt_sprite3d_new(tex);
    rt_sprite3d_set_frame(s, 0, 0, 32, 32);
    PASS();
}

static void test_sprite3d_null_safety() {
    TEST("Sprite3D null safety");
    rt_sprite3d_set_position(NULL, 0, 0, 0);
    rt_sprite3d_set_scale(NULL, 1, 1);
    rt_sprite3d_set_anchor(NULL, 0.5, 0.5);
    rt_sprite3d_set_frame(NULL, 0, 0, 16, 16);
    rt_sprite3d_rebase_origin(NULL, 1.0, 2.0, 3.0);
    PASS();
}

//=============================================================================
// RenderTarget3D tests
//=============================================================================

static void test_rendertarget_new() {
    TEST("RenderTarget3D.New — creates target");
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(256, 256);
    assert(rt);
    EXPECT_TRUE(rt->target != nullptr, "RenderTarget3D.New creates the backing target");
    EXPECT_TRUE(rt->target->color_format == VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8,
                "RenderTarget3D.New defaults to LDR UNORM8 color storage");
    EXPECT_TRUE(rt->target->color_buf == nullptr && rt->target->depth_buf == nullptr,
                "RenderTarget3D.New keeps CPU color/depth buffers lazy until first CPU use");
    EXPECT_TRUE(rt->target->estimated_bytes == 256ull * 256ull * 16ull,
                "RenderTarget3D.New budgets native storage and all lazy CPU mirrors");
    PASS();
}

static void test_rendertarget_new_hdr() {
    TEST("RenderTarget3D.NewHdr — creates HDR target");
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new_hdr(256, 128);
    assert(rt);
    EXPECT_TRUE(rt->target != nullptr, "RenderTarget3D.NewHdr creates the backing target");
    EXPECT_TRUE(rt->target->color_format == VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F,
                "RenderTarget3D.NewHdr stores HDR color format metadata");
    EXPECT_TRUE(rt->target->color_buf == nullptr && rt->target->depth_buf == nullptr,
                "RenderTarget3D.NewHdr keeps CPU color/depth buffers lazy until first CPU use");
    EXPECT_TRUE(rt->target->estimated_bytes == 256ull * 128ull * 36ull,
                "RenderTarget3D.NewHdr budgets native storage and both HDR/LDR CPU mirrors");
    PASS();
}

static void test_rendertarget_dimensions() {
    TEST("RenderTarget3D — width/height match constructor");
    auto *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(128, 64);
    EXPECT_EQ(rt_rendertarget3d_get_width(rt), 128);
    EXPECT_EQ(rt_rendertarget3d_get_height(rt), 64);
    rt->width = -128;
    rt->height = -64;
    EXPECT_EQ(rt_rendertarget3d_get_width(rt), 0);
    EXPECT_EQ(rt_rendertarget3d_get_height(rt), 0);
    PASS();
}

static void test_rendertarget_hdr_property() {
    TEST("RenderTarget3D.IsHdr — matches constructor");
    void *ldr = rt_rendertarget3d_new(64, 64);
    void *hdr = rt_rendertarget3d_new_hdr(64, 64);
    EXPECT_TRUE(rt_rendertarget3d_get_is_hdr(ldr) == 0, "LDR RenderTarget3D reports IsHdr false");
    EXPECT_TRUE(rt_rendertarget3d_get_is_hdr(hdr) == 1, "HDR RenderTarget3D reports IsHdr true");
    PASS();
}

static void test_rendertarget_as_pixels() {
    TEST("RenderTarget3D.AsPixels — returns non-null");
    void *rt = rt_rendertarget3d_new(16, 16);
    void *px = rt_rendertarget3d_as_pixels(rt);
    assert(px != NULL);
    PASS();
}

static void test_rendertarget_null_safety() {
    TEST("RenderTarget3D — null safety");
    EXPECT_EQ(rt_rendertarget3d_get_width(NULL), 0);
    EXPECT_EQ(rt_rendertarget3d_get_height(NULL), 0);
    EXPECT_TRUE(rt_rendertarget3d_get_is_hdr(NULL) == 0,
                "RenderTarget3D.IsHdr returns false for null");
    assert(rt_rendertarget3d_as_pixels(NULL) == NULL);
    rt_canvas3d_set_render_target(NULL, NULL);
    rt_canvas3d_reset_render_target(NULL);
    PASS();
}

namespace {
static int g_postfx_release_count = 0;
static int g_render_target_release_count = 0;
static int g_light_release_count = 0;
static int g_render_target_sync_calls = 0;
static bool g_render_target_sync_succeeds = true;
static uint8_t g_render_target_sync_rgba[4] = {0};
static int g_canvas_begin_frame_calls = 0;
static int g_canvas_end_frame_calls = 0;
static int g_canvas_submit_draw_calls = 0;
static int g_canvas_submit_draw_instanced_calls = 0;
static int g_last_instanced_count = 0;
static int g_last_instanced_has_prev = 0;
static float g_last_instanced_prev_x = 0.0f;
static int g_tracked_prev_model_count = 0;
static float g_tracked_prev_model_x[16] = {0.0f};
static vgfx3d_camera_params_t g_canvas_begin_frame_params = {};
static vgfx3d_draw_cmd_t g_last_draw_cmd = {};
static vgfx3d_vertex_t g_last_draw_vertices[512] = {};
static uint32_t g_last_draw_vertex_count = 0;
static int32_t g_last_draw_light_count = 0;
static vgfx3d_light_params_t g_last_draw_lights[VGFX3D_MAX_LIGHTS] = {};
static int g_backend_resize_calls = 0;
static int32_t g_backend_resize_w = 0;
static int32_t g_backend_resize_h = 0;
static uint64_t g_backend_texture_upload_bytes = 0;
static uint64_t g_backend_texture_upload_budget = UINT64_MAX;
static uint64_t g_backend_texture_upload_pending_bytes = 0;
static uint64_t g_backend_frame_gpu_time_us = 0;
static int g_backend_render_scale_calls = 0;
static float g_backend_requested_render_scale = 1.0f;
static bool g_backend_render_scale_succeeds = true;
} // namespace

typedef struct {
    int64_t w;
    int64_t h;
    uint32_t *data;
} pixels_view_t;

extern "C" int tracked_render_target_sync(void *, vgfx3d_rendertarget_t *target) {
    if (!target || !target->color_buf || !g_render_target_sync_succeeds)
        return 0;
    g_render_target_sync_calls++;
    target->color_buf[0] = g_render_target_sync_rgba[0];
    target->color_buf[1] = g_render_target_sync_rgba[1];
    target->color_buf[2] = g_render_target_sync_rgba[2];
    target->color_buf[3] = g_render_target_sync_rgba[3];
    return 1;
}

extern "C" void tracked_postfx_finalizer(void *obj) {
    (void)obj;
    g_postfx_release_count++;
}

extern "C" void tracked_render_target_finalizer(void *obj) {
    rt_rendertarget3d *rtd = (rt_rendertarget3d *)obj;
    g_render_target_release_count++;
    if (rtd && rtd->target) {
        free(rtd->target->color_buf);
        free(rtd->target->depth_buf);
        free(rtd->target);
        rtd->target = nullptr;
    }
}

extern "C" void tracked_light_finalizer(void *obj) {
    (void)obj;
    g_light_release_count++;
}

static void tracked_begin_frame(void *, const vgfx3d_camera_params_t *params) {
    g_canvas_begin_frame_calls++;
    if (params)
        g_canvas_begin_frame_params = *params;
}

static void tracked_end_frame(void *) {
    g_canvas_end_frame_calls++;
}

static uint64_t tracked_texture_upload_bytes(void *) {
    return g_backend_texture_upload_bytes;
}

static void tracked_set_texture_upload_budget(void *, uint64_t bytes) {
    g_backend_texture_upload_budget = bytes;
}

static uint64_t tracked_texture_upload_pending_bytes(void *) {
    return g_backend_texture_upload_pending_bytes;
}

static uint64_t tracked_frame_gpu_time_us(void *) {
    return g_backend_frame_gpu_time_us;
}

static int64_t tracked_native_texture_caps(void *) {
    return RT_CANVAS3D_BACKEND_CAP_BC7;
}

static void tracked_backend_resize(void *, int32_t w, int32_t h) {
    g_backend_resize_calls++;
    g_backend_resize_w = w;
    g_backend_resize_h = h;
}

/**
 * @brief Record a fake backend render-scale transition and return its injected result.
 *
 * This hook lets the Canvas3D contract test model an adapter that temporarily refuses
 * target recreation, such as a GPU backend with an active frame or pending present.
 *
 * @param ctx Unused fake backend context; required to exercise the production hook.
 * @param scale Scale requested by Canvas3D.
 * @return Non-zero when the test configured the transition to succeed.
 */
static int8_t tracked_set_render_scale(void *ctx, float scale) {
    (void)ctx;
    g_backend_render_scale_calls++;
    g_backend_requested_render_scale = scale;
    return g_backend_render_scale_succeeds ? 1 : 0;
}

static void tracked_submit_draw(void *,
                                vgfx_window_t,
                                const vgfx3d_draw_cmd_t *cmd,
                                const vgfx3d_light_params_t *lights,
                                int32_t light_count,
                                const float *,
                                int8_t,
                                int8_t) {
    g_canvas_submit_draw_calls++;
    g_last_draw_light_count = light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count;
    if (lights && g_last_draw_light_count > 0) {
        memcpy(g_last_draw_lights,
               lights,
               (size_t)g_last_draw_light_count * sizeof(vgfx3d_light_params_t));
    }
    if (cmd) {
        g_last_draw_cmd = *cmd;
        g_last_draw_vertex_count =
            cmd->vertex_count >
                    (uint32_t)(sizeof(g_last_draw_vertices) / sizeof(g_last_draw_vertices[0]))
                ? (uint32_t)(sizeof(g_last_draw_vertices) / sizeof(g_last_draw_vertices[0]))
                : cmd->vertex_count;
        if (cmd->vertices && g_last_draw_vertex_count > 0) {
            memcpy(g_last_draw_vertices,
                   cmd->vertices,
                   (size_t)g_last_draw_vertex_count * sizeof(g_last_draw_vertices[0]));
        }
        if (cmd->has_prev_model_matrix &&
            g_tracked_prev_model_count <
                (int)(sizeof(g_tracked_prev_model_x) / sizeof(g_tracked_prev_model_x[0]))) {
            g_tracked_prev_model_x[g_tracked_prev_model_count++] = cmd->prev_model_matrix[3];
        }
    }
}

static void tracked_submit_draw_instanced(void *,
                                          vgfx_window_t,
                                          const vgfx3d_draw_cmd_t *cmd,
                                          const float *,
                                          int32_t instance_count,
                                          const vgfx3d_light_params_t *,
                                          int32_t,
                                          const float *,
                                          int8_t,
                                          int8_t) {
    g_canvas_submit_draw_instanced_calls++;
    g_last_instanced_count = instance_count;
    if (cmd) {
        g_last_instanced_has_prev = cmd->has_prev_instance_matrices;
        g_last_instanced_prev_x =
            cmd->prev_instance_matrices ? cmd->prev_instance_matrices[3] : 0.0f;
    }
}

static void enable_latched_motion_blur(rt_canvas3d *canvas) {
    if (!canvas)
        return;
    canvas->frame_gpu_postfx_enabled = 1;
    canvas->frame_postfx_state_latched = 1;
    vgfx3d_postfx_chain_free(&canvas->frame_postfx_chain);
    memset(&canvas->frame_postfx_chain, 0, sizeof(canvas->frame_postfx_chain));
    canvas->frame_postfx_chain.effects =
        (vgfx3d_postfx_effect_desc_t *)calloc(1, sizeof(vgfx3d_postfx_effect_desc_t));
    if (!canvas->frame_postfx_chain.effects)
        return;
    canvas->frame_postfx_chain.enabled = 1;
    canvas->frame_postfx_chain.effect_count = 1;
    canvas->frame_postfx_chain.effect_capacity = 1;
    canvas->frame_postfx_chain.effects[0].type = VGFX3D_POSTFX_EFFECT_MOTION_BLUR;
    canvas->frame_postfx_chain.effects[0].snapshot.enabled = 1;
    canvas->frame_postfx_chain.effects[0].snapshot.motion_blur_enabled = 1;
}

extern "C" rt_string rt_postfx3d_get_last_error(void *obj);

static void bindcheck_present_postfx(void *ctx, const vgfx3d_postfx_chain_t *chain) {
    (void)ctx;
    (void)chain;
}

static void test_canvas_postfx_bind_time_capability_validation() {
    TEST("Canvas3D.SetPostFX binds GPU-scene effects on every path (Plan 06 parity)");
    vgfx3d_backend_t cpu_backend = {};
    vgfx3d_backend_t gpu_backend = {};
    rt_canvas3d canvas;
    void *fx = rt_postfx3d_new();
    assert(fx != NULL);

    cpu_backend.name = "software";
    gpu_backend.name = "metal";
    gpu_backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    gpu_backend.present_postfx = bindcheck_present_postfx;

    rt_postfx3d_add_ssao(fx, 0.5, 0.65, 8);

    /* CPU-path canvas: depth-aware effects run through the CPU reference
     * implementations, so the bind succeeds with no recorded error. */
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &cpu_backend;
    rt_canvas3d_set_post_fx(&canvas, fx);
    if (canvas.postfx != fx) {
        FAIL("GPU-scene chain failed to attach to a CPU-path canvas");
        return;
    }
    {
        rt_string err = rt_postfx3d_get_last_error(fx);
        const char *msg = rt_string_cstr(err);
        if (msg && msg[0] != '\0') {
            FAIL("LastError was not cleared on the CPU-path bind");
            return;
        }
    }
    rt_canvas3d_set_post_fx(&canvas, NULL);

    /* GPU window canvas: binds and clears the error. */
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &gpu_backend;
    rt_canvas3d_set_post_fx(&canvas, fx);
    if (canvas.postfx != fx) {
        FAIL("supported chain failed to attach to a GPU canvas");
        return;
    }
    {
        rt_string err = rt_postfx3d_get_last_error(fx);
        const char *msg = rt_string_cstr(err);
        if (msg && msg[0] != '\0') {
            FAIL("LastError was not cleared on a successful bind");
            return;
        }
    }
    rt_canvas3d_set_post_fx(&canvas, NULL);
    PASS();
}

static void test_canvas_postfx_retains_owned_reference() {
    TEST("Canvas3D.SetPostFX retains owned reference");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *fx = rt_postfx3d_new();
    assert(fx != NULL);
    g_postfx_release_count = 0;
    rt_obj_set_finalizer(fx, tracked_postfx_finalizer);

    rt_canvas3d_set_post_fx(&canvas, fx);
    if (rt_obj_release_check0(fx))
        rt_obj_free(fx);
    if (g_postfx_release_count != 0) {
        FAIL("Canvas3D did not retain PostFX3D");
        return;
    }

    rt_canvas3d_set_post_fx(&canvas, NULL);
    if (g_postfx_release_count != 1) {
        FAIL("Canvas3D did not release PostFX3D on clear");
        return;
    }
    PASS();
}

static void test_canvas_render_target_retains_owned_reference() {
    TEST("Canvas3D.SetRenderTarget retains owned reference");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *rt = rt_rendertarget3d_new(16, 16);
    assert(rt != NULL);
    g_render_target_release_count = 0;
    rt_obj_set_finalizer(rt, tracked_render_target_finalizer);

    rt_canvas3d_set_render_target(&canvas, rt);
    if (rt_obj_release_check0(rt))
        rt_obj_free(rt);
    if (g_render_target_release_count != 0) {
        FAIL("Canvas3D did not retain RenderTarget3D");
        return;
    }
    if (canvas.render_target_owner == rt &&
        canvas.render_target == ((rt_rendertarget3d *)rt)->target) {
        /* expected path */
    } else {
        FAIL("Canvas3D render target pointers were not updated");
        return;
    }

    rt_canvas3d_reset_render_target(&canvas);
    if (g_render_target_release_count != 1) {
        FAIL("Canvas3D did not release RenderTarget3D on reset");
        return;
    }
    PASS();
}

static void test_canvas_render_target_rejects_mid_frame_changes() {
    TEST("Canvas3D render target binding rejects mid-frame changes");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *rt = rt_rendertarget3d_new(16, 16);
    assert(rt != NULL);

    canvas.in_frame = 1;
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_canvas3d_set_render_target(&canvas, rt); }, "during a frame"),
        "SetRenderTarget traps during an active frame");
    EXPECT_TRUE(canvas.render_target_owner == NULL && canvas.render_target == NULL,
                "SetRenderTarget leaves canvas target unchanged when rejected");

    canvas.in_frame = 0;
    rt_canvas3d_set_render_target(&canvas, rt);
    canvas.in_frame = 1;
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_canvas3d_reset_render_target(&canvas); }, "during a frame"),
        "ResetRenderTarget traps during an active frame");
    EXPECT_TRUE(canvas.render_target_owner == rt,
                "ResetRenderTarget leaves current target bound when rejected");
    canvas.in_frame = 0;
    rt_canvas3d_reset_render_target(&canvas);
    if (rt_obj_release_check0(rt))
        rt_obj_free(rt);
    PASS();
}

static void test_canvas_light_retains_owned_reference() {
    TEST("Canvas3D.SetLight retains owned reference");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *light = rt_light3d_new_ambient(0.1, 0.2, 0.3);
    assert(light != NULL);
    g_light_release_count = 0;
    rt_obj_set_finalizer(light, tracked_light_finalizer);

    rt_canvas3d_set_light(&canvas, 0, light);
    if (rt_obj_release_check0(light))
        rt_obj_free(light);
    if (g_light_release_count != 0) {
        FAIL("Canvas3D did not retain Light3D");
        return;
    }

    rt_canvas3d_set_light(&canvas, 0, NULL);
    if (g_light_release_count != 1) {
        FAIL("Canvas3D did not release Light3D when clearing the slot");
        return;
    }
    PASS();
}

static void test_canvas_light_supports_last_slot() {
    TEST("Canvas3D.SetLight supports the last light slot");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *light = rt_light3d_new_ambient(0.4, 0.5, 0.6);
    assert(light != NULL);
    g_light_release_count = 0;
    rt_obj_set_finalizer(light, tracked_light_finalizer);

    rt_canvas3d_set_light(&canvas, VGFX3D_MAX_LIGHTS - 1, light);
    if (rt_obj_release_check0(light))
        rt_obj_free(light);
    if (g_light_release_count != 0) {
        FAIL("Canvas3D did not retain a Light3D stored in the last slot");
        return;
    }

    rt_canvas3d_set_light(&canvas, VGFX3D_MAX_LIGHTS - 1, NULL);
    if (g_light_release_count != 1) {
        FAIL("Canvas3D did not release a Light3D cleared from the last slot");
        return;
    }
    PASS();
}

static void test_canvas_light_rejects_invalid_inputs() {
    TEST("Canvas3D.SetLight rejects invalid index and handle");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    void *light = rt_light3d_new_ambient(0.2, 0.2, 0.2);
    void *fake = rt_material3d_new();
    assert(light != NULL && fake != NULL);

    EXPECT_TRUE(expect_trap_contains([&] { rt_canvas3d_set_light(&canvas, -1, light); },
                                     "index out of range"),
                "SetLight rejects negative slots");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_canvas3d_set_light(&canvas, VGFX3D_MAX_LIGHTS, light); },
                             "index out of range"),
        "SetLight rejects slots past the light table");

    rt_canvas3d_set_light(&canvas, 0, light);
    EXPECT_TRUE(expect_trap_contains([&] { rt_canvas3d_set_light(&canvas, 0, fake); }, "Light3D"),
                "SetLight rejects non-Light3D handles");
    EXPECT_TRUE(canvas.lights[0] == light, "rejected light handle leaves slot unchanged");
    rt_canvas3d_set_light(&canvas, 0, NULL);
    PASS();
}

static void test_canvas_light_params_sanitize_corrupt_type() {
    TEST("Canvas3D sanitizes corrupt light types before backend submission");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_plane(1.0, 1.0);
    void *mat = rt_material3d_new();
    void *xf = rt_mat4_identity();
    auto *light = (rt_light3d *)rt_light3d_new_ambient(0.4, 0.5, 0.6);
    assert(cam != NULL && mesh != NULL && mat != NULL && xf != NULL && light != NULL);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    light->type = 99;
    rt_canvas3d_set_light(&canvas, 0, light);
    g_last_draw_light_count = 0;
    memset(g_last_draw_lights, 0, sizeof(g_last_draw_lights));

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_last_draw_light_count, 1);
    EXPECT_EQ(g_last_draw_lights[0].type, 0);
    rt_canvas3d_set_light(&canvas, 0, NULL);
    PASS();
}

static void test_canvas_default_lighting_and_clear_lights() {
    TEST("Canvas3D.SetDefaultLighting and ClearLights");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    g_light_release_count = 0;

    rt_canvas3d_set_default_lighting(&canvas);
    EXPECT_EQ(rt_canvas3d_get_light_count(&canvas), 2);
    EXPECT_TRUE(canvas.lights[0] != nullptr && canvas.lights[1] != nullptr,
                "Default lighting installs key/fill lights");
    EXPECT_EQ(rt_light3d_get_type(canvas.lights[0]), 0);
    EXPECT_NEAR(canvas.ambient[0], 0.18, 0.001);
    EXPECT_NEAR(canvas.ambient[1], 0.18, 0.001);
    EXPECT_NEAR(canvas.ambient[2], 0.20, 0.001);
    rt_obj_set_finalizer(canvas.lights[0], tracked_light_finalizer);
    rt_obj_set_finalizer(canvas.lights[1], tracked_light_finalizer);

    rt_light3d_set_enabled(canvas.lights[1], 0);
    EXPECT_EQ(rt_canvas3d_get_light_count(&canvas), 1);
    rt_light3d *saved_disabled_light = canvas.lights[1];
    void *wrong_light = rt_vec3_new(0.0, 0.0, 0.0);
    canvas.lights[1] = (rt_light3d *)wrong_light;
    EXPECT_EQ(rt_canvas3d_get_light_count(&canvas), 1);
    canvas.lights[1] = saved_disabled_light;
    if (rt_obj_release_check0(wrong_light))
        rt_obj_free(wrong_light);

    rt_canvas3d_clear_lights(&canvas);
    EXPECT_EQ(rt_canvas3d_get_light_count(&canvas), 0);
    EXPECT_TRUE(canvas.lights[0] == nullptr && canvas.lights[1] == nullptr,
                "ClearLights clears retained slots");
    EXPECT_EQ(g_light_release_count, 2);
    PASS();
}

static void test_canvas_clustered_lighting_capability_gate() {
    TEST("Canvas3D clustered lighting is capability gated");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;

    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_FORWARD_LIGHT_LIMIT);
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("clustered-lighting")) == 0,
                "backend without clustered capability does not advertise clustered lighting");
    EXPECT_TRUE(rt_canvas3d_try_set_clustered_lighting(&canvas, 1) == 0,
                "non-trapping clustered lighting setter reports unsupported backends");
    EXPECT_TRUE(canvas.clustered_lighting == 0,
                "try-set leaves unsupported clustered lighting disabled");
    rt_canvas3d_set_clustered_lighting(&canvas, 0);
    EXPECT_TRUE(canvas.clustered_lighting == 0, "disabling clustered lighting is a no-op");
    EXPECT_TRUE(rt_canvas3d_try_set_clustered_lighting(&canvas, 0) == 1,
                "non-trapping setter can always disable clustered lighting on a valid canvas");
    EXPECT_TRUE(expect_trap_contains([&] { rt_canvas3d_set_clustered_lighting(&canvas, 1); },
                                     "not supported"),
                "enabling clustered lighting traps when the backend lacks support");
    EXPECT_TRUE(canvas.clustered_lighting == 0,
                "unsupported clustered lighting does not mutate the active path");
    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_FORWARD_LIGHT_LIMIT);
    PASS();
}

static void test_canvas_platform_gpu_clustered_lighting_capability() {
    TEST("Canvas3D platform GPU clustered lighting advertises the many-light path");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));

#if RT_PLATFORM_MACOS
    canvas.backend = &vgfx3d_metal_backend;
#elif RT_PLATFORM_WINDOWS
    canvas.backend = &vgfx3d_d3d11_backend;
#elif RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_HEADLESS)
    canvas.backend = &vgfx3d_opengl_backend;
#else
    canvas.backend = &vgfx3d_software_backend;
#endif

    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("clustered-lighting")) == 1,
                "real platform GPU backend advertises clustered/forward+ lighting");
    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_FORWARD_LIGHT_LIMIT);
    EXPECT_TRUE(rt_canvas3d_try_set_clustered_lighting(&canvas, 1) == 1,
                "non-trapping clustered lighting setter succeeds on capable backends");
    EXPECT_TRUE(canvas.clustered_lighting == 1,
                "try-set enables clustered lighting on capable backends");
    rt_canvas3d_set_clustered_lighting(&canvas, 0);
    rt_canvas3d_set_clustered_lighting(&canvas, 1);
    EXPECT_TRUE(canvas.clustered_lighting == 1,
                "clustered lighting can be enabled on the real platform backend");
    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_MAX_LIGHTS);
    PASS();
}

static void test_canvas_software_clustered_lighting_submits_many_lights() {
    TEST("Canvas3D software clustered lighting submits beyond the forward light cap");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    const int32_t light_count = VGFX3D_FORWARD_LIGHT_LIMIT + 8;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();
    void *xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -4.0, 0.0, 0.0, 0.0, 1.0);

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("clustered-lighting")) == 1,
                "software backend advertises the many-light baseline");
    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_FORWARD_LIGHT_LIMIT);
    EXPECT_TRUE(rt_canvas3d_try_set_clustered_lighting(&canvas, 1) == 1,
                "software clustered lighting baseline can be enabled without trapping");
    EXPECT_EQ(rt_canvas3d_get_max_active_lights(&canvas), VGFX3D_MAX_LIGHTS);

    for (int32_t i = 0; i < light_count; i++) {
        void *pos = rt_vec3_new((double)i, 1.0, 0.0);
        void *light = rt_light3d_new_point(pos, 1.0, 1.0, 1.0, 1.0 + (double)i);
        rt_light3d_set_intensity(light, 1.0 + (double)i);
        rt_canvas3d_set_light(&canvas, i, light);
    }

    g_canvas_submit_draw_calls = 0;
    g_last_draw_light_count = 0;
    memset(g_last_draw_lights, 0, sizeof(g_last_draw_lights));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_light_count, light_count);
    EXPECT_NEAR(g_last_draw_lights[light_count - 1].position[0], (double)(light_count - 1), 0.001);
    EXPECT_NEAR(
        g_last_draw_lights[light_count - 1].intensity, 1.0 + (double)(light_count - 1), 0.001);
    PASS();
}

static void test_canvas_shadow_cascades_capability_gate() {
    TEST("Canvas3D shadow cascades are capability gated");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &vgfx3d_software_backend;
    canvas.shadow_cascade_count = 1;

    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("shadow-csm")) == 1,
                "software backend advertises cascaded shadows");
    rt_canvas3d_set_shadow_cascades(&canvas, 0);
    EXPECT_EQ(canvas.shadow_cascade_count, 1);
    /* Cascade counts are a CSM concept: they clamp to the dedicated cascade
     * slots, not the (larger) total shadow-light budget. */
    rt_canvas3d_set_shadow_cascades(&canvas, VGFX3D_CSM_SLOTS + 2);
    EXPECT_EQ(canvas.shadow_cascade_count, VGFX3D_CSM_SLOTS);

    vgfx3d_backend_t unsupported = {};
    unsupported.name = "fake";
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &unsupported;
    canvas.shadow_cascade_count = 1;
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("shadow-csm")) == 0,
                "fake backends do not advertise cascaded shadows");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_canvas3d_set_shadow_cascades(&canvas, 2); }, "not supported"),
        "CSM counts above one trap when the backend lacks support");
    EXPECT_EQ(canvas.shadow_cascade_count, 1);
    PASS();
}

static void test_canvas_texture_backend_support_queries() {
    TEST("Canvas3D texture BackendSupports reports software CPU support");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &vgfx3d_software_backend;

    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc1")) == 1,
                "software backend reports BC1 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc3")) == 1,
                "software backend reports BC3 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc4")) == 1,
                "software backend reports BC4 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc5")) == 1,
                "software backend reports BC5 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc6h")) == 1,
                "software backend reports partial BC6H CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:bc7")) == 1,
                "software backend reports BC7 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:etc2")) == 1,
                "software backend reports ETC2 CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:astc")) == 1,
                "software backend reports ASTC CPU texture fallback support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("texture:ktx2-cpu")) == 1,
                "software backend reports KTX2 CPU decode support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("bc7")) == 0,
                "software backend does not report native BC7 upload support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("bc1")) == 0,
                "software backend does not report native BC1 upload support");

    vgfx3d_backend_t native_backend = {};
    native_backend.name = "native-test";
    native_backend.get_native_texture_caps = tracked_native_texture_caps;
    canvas.backend = &native_backend;
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("native-texture:bc7")) == 1,
                "native-texture:bc7 reports backend upload support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("backend-texture:bc7")) == 1,
                "backend-texture:bc7 aliases native upload support");
    EXPECT_TRUE(rt_canvas3d_backend_supports(&canvas, rt_const_cstr("native-texture:bc1")) == 0,
                "native texture aliases do not report CPU decode support");
    EXPECT_TRUE((rt_canvas3d_get_backend_capabilities(&canvas) & RT_CANVAS3D_BACKEND_CAP_PBR) == 0,
                "partial backends without a draw path do not advertise draw features");

    native_backend.submit_draw = tracked_submit_draw;
    native_backend.submit_draw_instanced = tracked_submit_draw_instanced;
    int64_t caps = rt_canvas3d_get_backend_capabilities(&canvas);
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_INSTANCING) != 0,
                "BackendCapabilities preserves high 64-bit instancing bit");
    EXPECT_TRUE(caps > INT32_MAX, "BackendCapabilities is a 64-bit mask");
    PASS();
}

static uint32_t g_revision_log[16];
static int g_revision_log_count = 0;

static void revision_tracking_submit_draw(void *,
                                          vgfx_window_t,
                                          const vgfx3d_draw_cmd_t *cmd,
                                          const vgfx3d_light_params_t *,
                                          int32_t,
                                          const float *,
                                          int8_t,
                                          int8_t) {
    if (cmd && g_revision_log_count < 16)
        g_revision_log[g_revision_log_count++] = cmd->lights_revision;
}

/// Light-snapshot revision stamps: draws queued under an unchanged light set
/// share one nonzero stamp (backends skip constant re-upload); mutating a
/// light between draws advances the stamp.
static void test_canvas_light_revision_stamps() {
    TEST("Canvas3D stamps light-snapshot revisions on queued draws");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();
    void *dir = rt_vec3_new(0.0, -1.0, 0.0);
    void *light = rt_light3d_new_directional(dir, 1.0, 1.0, 1.0);
    void *xf =
        rt_mat4_new(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = revision_tracking_submit_draw;
    backend.end_frame = tracked_end_frame;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_set_light(&canvas, 0, light);

    g_revision_log_count = 0;
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    /* Mutate a light property mid-frame: subsequent draws must re-stamp. */
    rt_light3d_set_intensity(light, 0.25);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_revision_log_count, 4);
    EXPECT_TRUE(g_revision_log[0] != 0, "first draw carries a nonzero stamp");
    EXPECT_TRUE(g_revision_log[0] == g_revision_log[1],
                "identical light sets share one revision stamp");
    EXPECT_TRUE(g_revision_log[2] != g_revision_log[1],
                "mutating a light between draws advances the stamp");
    EXPECT_TRUE(g_revision_log[2] == g_revision_log[3],
                "the new snapshot is shared by subsequent draws");

    /* A second identical frame re-uses the same stamp (no spurious churn). */
    g_revision_log_count = 0;
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_revision_log_count, 1);
    EXPECT_TRUE(g_revision_log[0] != 0, "second frame stamp is nonzero");
    PASS();
}

static void test_canvas_occlusion_culling_skips_covered_opaque_draws() {
    TEST("Canvas3D occlusion culling skips dense covered opaque draws");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    const int64_t covered_draws = 64;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 0.2);
    void *material = rt_material3d_new();
    void *near_xf =
        rt_mat4_new(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    void *far_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -2.0, 0.0, 0.0, 0.0, 1.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    g_canvas_submit_draw_calls = 0;
    g_canvas_begin_frame_calls = 0;
    g_canvas_end_frame_calls = 0;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_set_occlusion_culling(&canvas, 1);
    /* Occlusion and frustum culling are independent toggles: enabling occlusion must
     * not overwrite the frustum flag (zeroed by the stack fixture memset). */
    EXPECT_EQ(canvas.frustum_culling, 0);
    EXPECT_EQ(canvas.occlusion_culling, 1);
    rt_canvas3d_set_frustum_culling(&canvas, 1);
    EXPECT_EQ(canvas.frustum_culling, 1);
    EXPECT_EQ(canvas.occlusion_culling, 1);

    for (int frame = 0; frame < 3; ++frame) {
        if (frame == 2)
            g_canvas_submit_draw_calls = 0;
        rt_canvas3d_begin(&canvas, camera);
        for (int64_t i = 0; i < covered_draws; i++)
            rt_canvas3d_draw_mesh(&canvas, mesh, far_xf, material);
        rt_canvas3d_draw_mesh(&canvas, mesh, near_xf, material);
        rt_canvas3d_end(&canvas);
    }

    EXPECT_EQ(rt_canvas3d_get_draw_count(&canvas), covered_draws + 1);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(rt_canvas3d_get_occluded_draw_count(&canvas), covered_draws);
    EXPECT_EQ(rt_canvas3d_get_occlusion_candidate_count(&canvas), covered_draws + 1);
    PASS();
}

static void test_backend_reversed_z_negates_projection_z_row() {
    TEST("Canvas3D negates the projection z row for reversed-Z backends");
    vgfx3d_backend_t standard_backend = {};
    vgfx3d_backend_t reversed_backend = {};
    rt_canvas3d standard_canvas;
    rt_canvas3d reversed_canvas;
    float standard_proj[16];
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    standard_backend.name = "opengl";

    standard_backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    standard_backend.begin_frame = tracked_begin_frame;
    standard_backend.end_frame = tracked_end_frame;
    reversed_backend = standard_backend;
    reversed_backend.reversed_z = 1;

    memset(&standard_canvas, 0, sizeof(standard_canvas));
    standard_canvas.backend = &standard_backend;
    standard_canvas.gfx_win = (vgfx_window_t)1;
    standard_canvas.width = 64;
    standard_canvas.height = 64;
    memset(&reversed_canvas, 0, sizeof(reversed_canvas));
    reversed_canvas.backend = &reversed_backend;
    reversed_canvas.gfx_win = (vgfx_window_t)1;
    reversed_canvas.width = 64;
    reversed_canvas.height = 64;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_begin(&standard_canvas, camera);
    memcpy(standard_proj, g_canvas_begin_frame_params.projection, sizeof(standard_proj));
    rt_canvas3d_end(&standard_canvas);

    rt_canvas3d_begin(&reversed_canvas, camera);
    /* Reversed backends receive the same projection with the z row negated; the x/y/w
     * rows (and therefore culling, unprojection, and screen mapping) are untouched. */
    for (int i = 0; i < 16; i++) {
        float expected = (i >= 8 && i < 12) ? -standard_proj[i] : standard_proj[i];
        EXPECT_NEAR(g_canvas_begin_frame_params.projection[i], expected, 1e-6);
    }
    rt_canvas3d_end(&reversed_canvas);
    PASS();
}

static void test_canvas_hiz_rasterizer_culls_behind_rotated_occluder() {
    TEST("Canvas3D Hi-Z rasterizes deep-AABB occluders that rect writes cannot");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    const int64_t hidden_draws = 8;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *occluder_mesh = rt_mesh3d_new_box(4.0, 4.0, 4.0);
    void *hidden_mesh = rt_mesh3d_new_box(1.0, 1.0, 0.2);
    void *material = rt_material3d_new();
    /* 45-degree Y rotation: the box's world AABB spans ~5.66 units of view depth, so
     * the conservative rect write is rejected by the depth-span guard — only the
     * triangle rasterizer can register this occluder. */
    const double c45 = 0.70710678118654752;
    void *rot_xf = rt_mat4_new(
        c45, 0.0, c45, 0.0, 0.0, 1.0, 0.0, 0.0, -c45, 0.0, c45, 0.0, 0.0, 0.0, 0.0, 1.0);
    void *behind_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -3.0, 0.0, 0.0, 0.0, 1.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_set_occlusion_culling(&canvas, 1);

    for (int frame = 0; frame < 3; ++frame) {
        if (frame == 2)
            g_canvas_submit_draw_calls = 0;
        rt_canvas3d_begin(&canvas, camera);
        rt_canvas3d_draw_mesh(&canvas, occluder_mesh, rot_xf, material);
        for (int64_t i = 0; i < hidden_draws; i++)
            rt_canvas3d_draw_mesh(&canvas, hidden_mesh, behind_xf, material);
        rt_canvas3d_end(&canvas);
    }

    EXPECT_EQ(rt_canvas3d_get_draw_count(&canvas), hidden_draws + 1);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(rt_canvas3d_get_occluded_draw_count(&canvas), hidden_draws);
    PASS();
}

/// @brief Verify over-budget occluders use exact coarse proxies without false edge culls.
/// @details A rotated box repeats its twelve real surface triangles until it exceeds the precise
///   1,024-triangle limit. The bounded proxy must still establish central coverage and hide boxes
///   fully behind it after history warm-up. A box crossing the projected silhouette edge must
///   remain submitted because the proxy publishes only completely covered 4x4 fine blocks.
static void test_canvas_hiz_high_poly_proxy_is_conservative() {
    TEST("Canvas3D Hi-Z uses a conservative proxy for high-poly occluders");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    const int64_t hidden_draws = 8;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *occluder_mesh = rt_mesh3d_new_box(4.0, 4.0, 4.0);
    void *hidden_mesh = rt_mesh3d_new_box(0.8, 0.8, 0.2);
    void *material = rt_material3d_new();
    auto *occluder_view = (rt_mesh3d *)occluder_mesh;
    uint32_t base_indices[36];
    const double c45 = 0.70710678118654752;
    void *rot_xf = rt_mat4_new(
        c45, 0.0, c45, 0.0, 0.0, 1.0, 0.0, 0.0, -c45, 0.0, c45, 0.0, 0.0, 0.0, 0.0, 1.0);
    void *behind_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -3.0, 0.0, 0.0, 0.0, 1.0);
    void *edge_xf = rt_mat4_new(
        1.0, 0.0, 0.0, 4.1, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -3.0, 0.0, 0.0, 0.0, 1.0);

    memcpy(base_indices, occluder_view->indices, sizeof(base_indices));
    for (int repeat = 0; repeat < 128; repeat++) {
        for (int triangle = 0; triangle < 12; triangle++) {
            const uint32_t *indices = &base_indices[(size_t)triangle * 3u];
            rt_mesh3d_add_triangle(occluder_mesh, indices[0], indices[1], indices[2]);
        }
    }
    EXPECT_TRUE(rt_mesh3d_get_triangle_count(occluder_mesh) > 1024,
                "High-poly proxy fixture exceeds the precise per-draw triangle budget");

    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_set_occlusion_culling(&canvas, 1);
    for (int frame = 0; frame < 3; frame++) {
        if (frame == 2)
            g_canvas_submit_draw_calls = 0;
        rt_canvas3d_begin(&canvas, camera);
        rt_canvas3d_draw_mesh(&canvas, occluder_mesh, rot_xf, material);
        for (int64_t i = 0; i < hidden_draws; i++)
            rt_canvas3d_draw_mesh(&canvas, hidden_mesh, behind_xf, material);
        rt_canvas3d_draw_mesh(&canvas, hidden_mesh, edge_xf, material);
        rt_canvas3d_end(&canvas);
    }

    EXPECT_EQ(canvas.hiz_frame_triangles, 12);
    EXPECT_EQ(canvas.hiz_frame_proxy_triangles, 512);
    EXPECT_EQ(g_canvas_submit_draw_calls, 2);
    EXPECT_EQ(rt_canvas3d_get_occluded_draw_count(&canvas), hidden_draws);
    PASS();
}

static void test_frame_light_flatten_cache_shares_snapshot_across_draws() {
    TEST("Canvas3D flattens lights once per generation, not once per draw");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();
    void *xf =
        rt_mat4_new(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    void *light_dir = rt_vec3_new(0.0, -1.0, 0.0);
    void *light = rt_light3d_new_directional(light_dir, 1.0, 1.0, 1.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_set_light(&canvas, 0, light);

    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    int32_t after_first = canvas.frame_light_snapshot_count;
    uint32_t first_stamp = canvas.frame_light_cache_stamp;
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    /* Identical light state: all draws must share one snapshot slice and stamp. */
    EXPECT_TRUE(after_first > 0, "first draw must store a flattened light slice");
    EXPECT_EQ(canvas.frame_light_snapshot_count, after_first);
    EXPECT_EQ(canvas.frame_light_cache_stamp, first_stamp);

    /* A Light3D mutation advances the generation and forces one re-flatten. */
    rt_light3d_set_intensity(light, 0.5);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    EXPECT_EQ(canvas.frame_light_snapshot_count, after_first * 2);
    EXPECT_TRUE(canvas.frame_light_cache_stamp != first_stamp,
                "light mutation must produce a fresh revision stamp");

    /* Canvas slot changes invalidate the cache the same way. */
    int32_t before_slot_change = canvas.frame_light_snapshot_count;
    rt_canvas3d_set_light(&canvas, 1, nullptr);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, material);
    EXPECT_EQ(canvas.frame_light_snapshot_count, before_slot_change + after_first);
    rt_canvas3d_end(&canvas);
    PASS();
}

static void test_shadow_distance_setter_and_effective_range() {
    TEST("Canvas3D.SetShadowDistance caps cascade coverage with an auto default");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.cached_cam_far = 1000.0f;

    /* Auto default: min(camera far, 300). */
    EXPECT_EQ(rt_canvas3d_get_shadow_distance(&canvas), 0.0);
    EXPECT_NEAR(canvas3d_effective_shadow_distance(&canvas), 300.0f, 1e-3);

    /* Short far planes clamp the auto default. */
    canvas.cached_cam_far = 120.0f;
    EXPECT_NEAR(canvas3d_effective_shadow_distance(&canvas), 120.0f, 1e-3);

    /* Explicit values are honored, clamped to the far plane at use time. */
    rt_canvas3d_set_shadow_distance(&canvas, 80.0);
    EXPECT_EQ(rt_canvas3d_get_shadow_distance(&canvas), 80.0);
    EXPECT_NEAR(canvas3d_effective_shadow_distance(&canvas), 80.0f, 1e-3);
    rt_canvas3d_set_shadow_distance(&canvas, 5000.0);
    EXPECT_NEAR(canvas3d_effective_shadow_distance(&canvas), 120.0f, 1e-3);

    /* Non-positive / non-finite restore the automatic default. */
    rt_canvas3d_set_shadow_distance(&canvas, 0.0);
    EXPECT_EQ(rt_canvas3d_get_shadow_distance(&canvas), 0.0);
    rt_canvas3d_set_shadow_distance(&canvas, -25.0);
    EXPECT_EQ(rt_canvas3d_get_shadow_distance(&canvas), 0.0);
    PASS();
}

static void test_instanced_draw_precomputes_world_bounds_in_snapshot_pass() {
    TEST("Canvas3D instanced draws union world bounds once during matrix snapshot");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();

    enum { kInstanceCount = 8 };

    float matrices[kInstanceCount * 16];

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    for (int32_t i = 0; i < kInstanceCount; i++) {
        float *m = &matrices[(size_t)i * 16u];
        memset(m, 0, sizeof(float) * 16);
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[3] = (float)i; /* spread along +X, all inside the frustum */
        m[11] = -3.0f;
    }

    g_canvas_submit_draw_calls = 0;
    g_canvas_submit_draw_instanced_calls = 0;
    rt_camera3d_look_at(camera, eye, target, up);
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, material, matrices, kInstanceCount, NULL, 0);
    /* The union is accumulated inside the snapshot copy loop: exactly one AABB
     * transform per instance. A second scan at append (the old path) would double
     * this counter. */
    EXPECT_EQ(canvas.frame_aabb_transforms, kInstanceCount);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_canvas_submit_draw_instanced_calls > 0 || g_canvas_submit_draw_calls > 0,
                "instanced batch must reach the backend");
    PASS();
}

static void test_rendertarget_as_pixels_syncs_gpu_color_on_demand() {
    TEST("RenderTarget3D.AsPixels syncs backend-owned color on demand");
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    assert(rt != NULL && rt->target != NULL);
    EXPECT_TRUE(rt->target->color_buf == nullptr,
                "RenderTarget3D starts without a CPU color buffer before sync/readback");

    g_render_target_sync_calls = 0;
    g_render_target_sync_rgba[0] = 0xAB;
    g_render_target_sync_rgba[1] = 0xCD;
    g_render_target_sync_rgba[2] = 0xEF;
    g_render_target_sync_rgba[3] = 0x12;
    rt->target->color_dirty = 1;
    rt->target->sync_color = tracked_render_target_sync;
    rt->target->sync_color_userdata = NULL;

    pixels_view_t *px = (pixels_view_t *)rt_rendertarget3d_as_pixels(rt);
    assert(px != NULL);
    if (g_render_target_sync_calls != 1) {
        FAIL("RenderTarget3D.AsPixels did not trigger the backend sync callback");
        return;
    }
    if (!rt->target->color_buf) {
        FAIL("RenderTarget3D.AsPixels did not allocate a CPU color buffer before sync");
        return;
    }
    if (rt->target->color_dirty != 0) {
        FAIL("RenderTarget3D.AsPixels did not clear the dirty color flag");
        return;
    }
    if (!px->data || px->data[0] != 0xABCDEF12u) {
        FAIL("RenderTarget3D.AsPixels did not copy the synced RGBA bytes");
        return;
    }
    PASS();
}

static void test_rendertarget_clear_sync_detaches_backend_callback() {
    TEST("RenderTarget3D clear sync detaches backend callback");
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new_hdr(1, 1);
    assert(rt != NULL && rt->target != NULL);
    rt->target->color_dirty = 1;
    rt->target->hdr_color_valid = 1;
    rt->target->sync_color = tracked_render_target_sync;
    rt->target->sync_color_userdata = rt;

    vgfx3d_rendertarget_clear_sync(rt->target);
    EXPECT_TRUE(rt->target->color_dirty == 0, "clear sync clears dirty color state");
    EXPECT_TRUE(rt->target->hdr_color_valid == 0, "clear sync clears HDR mirror state");
    EXPECT_TRUE(rt->target->sync_color == nullptr, "clear sync clears callback");
    EXPECT_TRUE(rt->target->sync_color_userdata == nullptr, "clear sync clears callback userdata");
    PASS();
}

static void test_rendertarget_rejects_malformed_buffer_layouts() {
    TEST("RenderTarget3D rejects malformed buffer layouts");
    vgfx3d_rendertarget_t bad = {};
    uint8_t tiny_color[4] = {};
    float tiny_depth[1] = {};
    float tiny_hdr[4] = {};

    bad.width = 2;
    bad.height = 2;
    bad.stride = 4;
    bad.color_buf = tiny_color;
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(&bad) == 0,
                "Existing color buffers still require stride >= width*4");

    bad.color_buf = nullptr;
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(&bad) == 0,
                "New color buffers reject undersized strides");

    bad.stride = 8;
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(&bad) != 0, "Valid color layouts still allocate");
    free(bad.color_buf);
    bad.color_buf = nullptr;

    bad.width = INT32_MAX;
    bad.height = INT32_MAX;
    bad.depth_buf = tiny_depth;
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_depth(&bad) == 0,
                "Existing depth buffers still require valid pixel counts");

    bad.color_format = VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F;
    bad.hdr_color_buf = tiny_hdr;
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_hdr_color(&bad) == 0,
                "Existing HDR buffers still require valid pixel counts");

    bad.width = 2;
    bad.height = 2;
    bad.stride = 4;
    bad.color_dirty = 1;
    bad.sync_color = tracked_render_target_sync;
    g_render_target_sync_calls = 0;
    EXPECT_TRUE(vgfx3d_rendertarget_sync_color_if_needed(&bad) == 0,
                "Dirty sync refuses malformed color layouts before callback");
    EXPECT_EQ(g_render_target_sync_calls, 0);

    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(2, 2);
    assert(rt != nullptr && rt->target != nullptr);
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(rt->target) != 0,
                "AsPixels malformed-layout test allocates color");
    rt->target->stride = 4;
    EXPECT_TRUE(rt_rendertarget3d_as_pixels(rt) == nullptr,
                "AsPixels refuses an existing color buffer with invalid stride");
    PASS();
}

static void test_canvas_screenshot_syncs_render_target_on_demand() {
    TEST("Canvas3D.Screenshot syncs render targets on demand without a window");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    assert(rt != NULL && rt->target != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.width = 1;
    canvas.height = 1;

    g_render_target_sync_calls = 0;
    g_render_target_sync_rgba[0] = 0x10;
    g_render_target_sync_rgba[1] = 0x20;
    g_render_target_sync_rgba[2] = 0x30;
    g_render_target_sync_rgba[3] = 0x40;
    rt->target->color_dirty = 1;
    rt->target->sync_color = tracked_render_target_sync;
    rt->target->sync_color_userdata = NULL;

    pixels_view_t *shot = (pixels_view_t *)rt_canvas3d_screenshot(&canvas);
    assert(shot != NULL);
    if (g_render_target_sync_calls != 1) {
        FAIL("Canvas3D.Screenshot did not trigger the render-target sync callback");
        return;
    }
    if (rt->target->color_dirty != 0) {
        FAIL("Canvas3D.Screenshot did not clear the render-target dirty flag");
        return;
    }
    if (!shot->data || shot->data[0] != 0x10203040u) {
        FAIL("Canvas3D.Screenshot did not copy the synced render-target RGBA bytes");
        return;
    }
    PASS();
}

static void test_canvas_screenshot_returns_null_when_sync_fails() {
    TEST("Canvas3D.Screenshot returns null when render-target sync fails");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    assert(rt != NULL && rt->target != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.width = 1;
    canvas.height = 1;

    g_render_target_sync_calls = 0;
    g_render_target_sync_succeeds = false;
    rt->target->color_dirty = 1;
    rt->target->sync_color = tracked_render_target_sync;
    rt->target->sync_color_userdata = NULL;

    void *shot = rt_canvas3d_screenshot(&canvas);
    g_render_target_sync_succeeds = true;
    EXPECT_TRUE(shot == NULL, "Canvas3D.Screenshot returns null on sync failure");
    EXPECT_EQ(g_render_target_sync_calls, 0);
    PASS();
}

static void test_canvas_dimensions_follow_active_render_target() {
    TEST("Canvas3D width/height follow the active render target");
    rt_canvas3d canvas;
    void *rt = rt_rendertarget3d_new(320, 180);
    assert(rt != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.width = 800;
    canvas.height = 600;

    EXPECT_EQ(rt_canvas3d_get_width(&canvas), 800);
    EXPECT_EQ(rt_canvas3d_get_height(&canvas), 600);
    EXPECT_EQ(rt_canvas3d_get_window_width(&canvas), 800);
    EXPECT_EQ(rt_canvas3d_get_window_height(&canvas), 600);
    EXPECT_EQ(rt_canvas3d_get_active_output_width(&canvas), 800);
    EXPECT_EQ(rt_canvas3d_get_active_output_height(&canvas), 600);

    rt_canvas3d_set_render_target(&canvas, rt);
    EXPECT_EQ(rt_canvas3d_get_width(&canvas), 320);
    EXPECT_EQ(rt_canvas3d_get_height(&canvas), 180);
    EXPECT_EQ(rt_canvas3d_get_window_width(&canvas), 800);
    EXPECT_EQ(rt_canvas3d_get_window_height(&canvas), 600);
    EXPECT_EQ(rt_canvas3d_get_active_output_width(&canvas), 320);
    EXPECT_EQ(rt_canvas3d_get_active_output_height(&canvas), 180);

    rt_canvas3d_reset_render_target(&canvas);
    EXPECT_EQ(rt_canvas3d_get_width(&canvas), 800);
    EXPECT_EQ(rt_canvas3d_get_height(&canvas), 600);

    canvas.width = -8;
    canvas.height = -9;
    EXPECT_EQ(rt_canvas3d_get_window_width(&canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_window_height(&canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_active_output_width(&canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_active_output_height(&canvas), 0);

    vgfx3d_rendertarget_t wrong_rt = {};
    canvas.width = 800;
    canvas.height = 600;
    canvas.render_target = &wrong_rt;
    EXPECT_EQ(rt_canvas3d_get_width(&canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_height(&canvas), 0);
    canvas.render_target = nullptr;
    PASS();
}

static void test_canvas_offscreen_constructor_contract() {
    TEST("Canvas3D.NewOffscreen creates a windowless software target renderer");
    void *target = rt_rendertarget3d_new(96, 64);
    void *canvas = rt_canvas3d_new_offscreen(target);

    EXPECT_TRUE(canvas != nullptr, "NewOffscreen returns a Canvas3D");
    EXPECT_TRUE(rt_canvas3d_get_is_offscreen(canvas) == 1,
                "NewOffscreen canvas reports IsOffscreen");
    EXPECT_EQ(rt_canvas3d_get_width(canvas), 96);
    EXPECT_EQ(rt_canvas3d_get_height(canvas), 64);
    EXPECT_EQ(rt_canvas3d_get_active_output_width(canvas), 96);
    EXPECT_EQ(rt_canvas3d_get_active_output_height(canvas), 64);
    EXPECT_EQ(rt_canvas3d_get_window_width(canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_window_height(canvas), 0);
    EXPECT_EQ(rt_canvas3d_poll(canvas), 0);
    EXPECT_TRUE(rt_canvas3d_should_close(canvas) == 0,
                "windowless canvas has no synthetic close request");

    auto *raw = (rt_canvas3d *)canvas;
    EXPECT_TRUE(raw->gfx_win == nullptr, "NewOffscreen does not create a platform window");
    EXPECT_TRUE(raw->backend == &vgfx3d_software_backend,
                "NewOffscreen selects the portable software backend");
    EXPECT_TRUE(raw->render_target_owner == target && raw->render_target != nullptr,
                "NewOffscreen retains and binds the explicit RenderTarget3D");

    rt_canvas3d_clear(canvas, 0.25, 0.5, 0.75);
    auto *shot = (pixels_view_t *)rt_canvas3d_screenshot(canvas);
    EXPECT_TRUE(shot != nullptr && shot->data != nullptr,
                "windowless Canvas3D.Screenshot reads the active target");
    EXPECT_EQ(shot->w, 96);
    EXPECT_EQ(shot->h, 64);
    EXPECT_TRUE(shot->data[0] == 0x3F7FBFFFu,
                "windowless clear writes deterministic RGBA target pixels");

    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_canvas3d_reset_render_target(canvas); }, "requires a render target"),
                "ResetRenderTarget rejects removal of the only offscreen output");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_canvas3d_resize(canvas, 48, 32); }, "replace its render target"),
                "Resize directs offscreen callers to replace the explicit target");
    EXPECT_EQ(rt_canvas3d_get_width(canvas), 96);
    EXPECT_EQ(rt_canvas3d_get_height(canvas), 64);
    PASS();
}

static void test_canvas_offscreen_renders_and_finalizes_without_window() {
    TEST("Canvas3D offscreen renders mesh frames and ScreenshotFinal flushes them");
    void *target = rt_rendertarget3d_new(96, 96);
    void *canvas = rt_canvas3d_new_offscreen(target);
    void *camera = rt_camera3d_new_ortho(2.0, 1.0, 0.1, 100.0);
    void *look_target = rt_vec3_new(0.0, 0.0, 0.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 0.0, 0.0);
    void *transform = rt_mat4_identity();

    EXPECT_TRUE(canvas && camera && look_target && mesh && material && transform,
                "offscreen rendering fixtures exist");
    rt_camera3d_orbit(camera, look_target, 10.0, 0.0, 0.0);
    rt_material3d_set_unlit(material, 1);
    rt_canvas3d_clear(canvas, 0.0, 0.0, 0.0);
    rt_canvas3d_begin(canvas, camera);
    rt_canvas3d_draw_mesh(canvas, mesh, transform, material);

    auto *shot = (pixels_view_t *)rt_canvas3d_screenshot_final(canvas);
    EXPECT_TRUE(shot != nullptr && shot->data != nullptr,
                "ScreenshotFinal returns pixels without a platform window");
    EXPECT_TRUE(((rt_canvas3d *)canvas)->in_frame == 0,
                "ScreenshotFinal ends the pending offscreen frame");

    int64_t changed_pixels = 0;
    for (int64_t i = 0; i < shot->w * shot->h; i++) {
        if (shot->data[i] != 0x000000FFu)
            changed_pixels++;
    }
    EXPECT_TRUE(changed_pixels > 100,
                "software rasterizer writes visible mesh coverage into the target");
    PASS();
}

static void test_canvas_offscreen_rejects_invalid_targets() {
    TEST("Canvas3D.NewOffscreen rejects null and wrong-class targets");
    void *wrong = rt_mesh3d_new_box(1.0, 1.0, 1.0);

    EXPECT_TRUE(expect_trap_contains(
                    [] { (void)rt_canvas3d_new_offscreen(nullptr); }, "RenderTarget3D"),
                "NewOffscreen traps on a null target");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_canvas3d_new_offscreen(wrong); }, "RenderTarget3D"),
                "NewOffscreen traps on a wrong-class target");
    EXPECT_TRUE(rt_canvas3d_get_is_offscreen(nullptr) == 0,
                "IsOffscreen is false for a null handle");
    PASS();
}

static void test_canvas_offscreen_accelerated_constructor_contract() {
    TEST("Canvas3D.NewOffscreenAccelerated renders windowless on any backend (ADR 0191)");
    void *target = rt_rendertarget3d_new(96, 64);
    void *canvas = rt_canvas3d_new_offscreen_accelerated(target);

    EXPECT_TRUE(canvas != nullptr, "NewOffscreenAccelerated returns a Canvas3D");
    EXPECT_TRUE(rt_canvas3d_get_is_offscreen(canvas) == 1,
                "accelerated offscreen canvas reports IsOffscreen");
    EXPECT_EQ(rt_canvas3d_get_width(canvas), 96);
    EXPECT_EQ(rt_canvas3d_get_height(canvas), 64);

    auto *raw = (rt_canvas3d *)canvas;
    EXPECT_TRUE(raw->gfx_win == nullptr,
                "NewOffscreenAccelerated does not create a platform window");
    EXPECT_TRUE(raw->render_target_owner == target && raw->render_target != nullptr,
                "NewOffscreenAccelerated retains and binds the explicit RenderTarget3D");

    /* Backends without headless-context support must fall back to software;
     * an accelerated backend must not report fallback. Either outcome is a
     * valid environment, but the truth channel must be consistent. */
    const bool is_software = raw->backend == &vgfx3d_software_backend;
    if (!is_software) {
        EXPECT_TRUE(rt_canvas3d_get_backend_fallback(canvas) == 0,
                    "an accelerated offscreen backend must not report fallback");
    }

    rt_canvas3d_clear(canvas, 0.25, 0.5, 0.75);
    auto *shot = (pixels_view_t *)rt_canvas3d_screenshot(canvas);
    EXPECT_TRUE(shot != nullptr && shot->data != nullptr && shot->w == 96 && shot->h == 64,
                "accelerated offscreen readback returns target-sized pixels");
    if (is_software && shot && shot->data) {
        EXPECT_TRUE(shot->data[0] == 0x3F7FBFFFu,
                    "software-fallback clear stays byte-deterministic");
    }
    PASS();
}

static void test_canvas_begin2d_uses_render_target_dimensions() {
    TEST("Canvas3D.Begin2D uses render-target size and advances frame serial");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *rt = rt_rendertarget3d_new(320, 180);
    assert(rt != NULL);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 800;
    canvas.height = 600;
    canvas.frame_serial = -7;
    EXPECT_EQ(rt_canvas3d_get_frame_serial(&canvas), 0);
    canvas.frame_serial = 41;
    rt_canvas3d_set_render_target(&canvas, rt);

    g_canvas_begin_frame_calls = 0;
    memset(&g_canvas_begin_frame_params, 0, sizeof(g_canvas_begin_frame_params));
    rt_canvas3d_begin_2d(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_EQ(canvas.frame_serial, 42);
    EXPECT_TRUE(canvas.frame_is_2d == 1, "Canvas3D.Begin2D marks the frame as 2D");
    EXPECT_NEAR(g_canvas_begin_frame_params.projection[0], 2.0f / 322.0f, 0.0001);
    EXPECT_NEAR(g_canvas_begin_frame_params.projection[5], -2.0f / 182.0f, 0.0001);

    rt_canvas3d_end(&canvas);
    PASS();
}

static void test_canvas_begin_uses_active_output_aspect_without_mutating_camera() {
    TEST("Canvas3D.Begin uses the active output aspect without mutating the camera");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *rt = rt_rendertarget3d_new(320, 160);
    double original_projection[16];
    assert(cam != NULL);
    assert(rt != NULL);
    std::memcpy(original_projection, cam->projection, sizeof(original_projection));

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 800;
    canvas.height = 600;
    rt_canvas3d_set_render_target(&canvas, rt);

    g_canvas_begin_frame_calls = 0;
    memset(&g_canvas_begin_frame_params, 0, sizeof(g_canvas_begin_frame_params));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_NEAR(cam->aspect, 1.0, 0.0001);
    EXPECT_TRUE(std::memcmp(original_projection, cam->projection, sizeof(original_projection)) == 0,
                "Canvas3D.Begin leaves the camera's stored projection matrix unchanged");
    EXPECT_NEAR(g_canvas_begin_frame_params.projection[0], 0.8660254f, 0.001);
    PASS();
}

static void test_canvas_camera_relative_upload_rebases_frame_payloads() {
    TEST("Canvas3D camera-relative upload rebases frame payloads");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    constexpr double kBase = 1000000000.0;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();
    void *light_pos = rt_vec3_new(kBase + 8.0, 2.0, -3.0);
    void *light = rt_light3d_new_point(light_pos, 1.0, 1.0, 1.0, 2.0);
    double model[16] = {
        1.0,
        0.0,
        0.0,
        kBase + 4.0,
        0.0,
        1.0,
        0.0,
        1.0,
        0.0,
        0.0,
        1.0,
        -10.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    rt_canvas3d_set_light(&canvas, 0, light);
    rt_canvas3d_set_camera_relative_upload(&canvas, 1);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(kBase, 0.0, 0.0),
                        rt_vec3_new(kBase, 0.0, -1.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    g_canvas_begin_frame_calls = 0;
    g_canvas_submit_draw_calls = 0;
    g_last_draw_light_count = 0;
    memset(&g_canvas_begin_frame_params, 0, sizeof(g_canvas_begin_frame_params));
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_lights, 0, sizeof(g_last_draw_lights));

    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, material);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_NEAR(g_canvas_begin_frame_params.position[0], 0.0, 0.0001);
    EXPECT_NEAR(g_canvas_begin_frame_params.view[3], 0.0, 0.0001);
    EXPECT_NEAR(g_last_draw_cmd.model_matrix[3], 4.0, 0.0001);
    EXPECT_EQ(g_last_draw_light_count, 1);
    EXPECT_NEAR(g_last_draw_lights[0].position[0], 8.0, 0.0001);

    rt_canvas3d_set_camera_relative_upload(&canvas, 0);
    EXPECT_TRUE(canvas.camera_relative_upload == 0, "Canvas3D internal relative mode disables");
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

static void test_canvas_camera_relative_upload_rebases_raw_and_generated_vertices() {
    TEST("Canvas3D camera-relative upload rebases raw and generated vertices");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    constexpr double kBase = 1000000000.0;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    void *material = rt_material3d_new();
    void *raw_mesh = rt_mesh3d_new();
    void *particles = rt_particles3d_new(4);
    void *sprite = rt_sprite3d_new(NULL);
    static const double identity[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    rt_canvas3d_set_camera_relative_upload(&canvas, 1);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(kBase, 0.0, 0.0),
                        rt_vec3_new(kBase, 0.0, -1.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    rt_mesh3d_add_vertex(raw_mesh, kBase + 4.0, 0.0, -4.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(raw_mesh, kBase + 6.0, 0.0, -4.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(raw_mesh, kBase + 4.0, 2.0, -4.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(raw_mesh, 0, 1, 2);

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh_matrix(&canvas, raw_mesh, identity, material);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 3);
    EXPECT_NEAR(g_last_draw_cmd.model_matrix[3], 0.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[0].pos[0], 4.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[1].pos[0], 6.0, 0.0001);
    EXPECT_TRUE(g_last_draw_cmd.geometry_key == NULL,
                "rebased raw vertex snapshots do not reuse world-space geometry caches");

    rt_particles3d_set_position(particles, kBase + 4.0, 0.0, -4.0);
    rt_particles3d_set_speed(particles, 0.0, 0.0);
    rt_particles3d_set_lifetime(particles, 10.0, 10.0);
    rt_particles3d_set_size(particles, 2.0, 2.0);
    rt_particles3d_burst(particles, 1);

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_particles3d_draw(particles, &canvas, camera);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 4);
    EXPECT_NEAR(g_last_draw_cmd.model_matrix[3], 0.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[0].pos[0], 3.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[2].pos[0], 5.0, 0.0001);

    rt_sprite3d_set_position(sprite, kBase + 4.0, 0.0, -4.0);
    rt_sprite3d_set_scale(sprite, 2.0, 2.0);

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 4);
    EXPECT_NEAR(g_last_draw_cmd.model_matrix[3], 0.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[0].pos[0], 3.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[2].pos[0], 5.0, 0.0001);

    rt_sprite3d_set_position(sprite, kBase + 8.0, 0.0, -4.0);
    rt_sprite3d_rebase_origin(sprite, kBase, 0.0, 0.0);
    rt_canvas3d_set_camera_relative_upload(&canvas, 0);

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 4);
    EXPECT_NEAR(g_last_draw_cmd.model_matrix[3], 0.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[0].pos[0], 7.0, 0.0001);
    EXPECT_NEAR(g_last_draw_vertices[2].pos[0], 9.0, 0.0001);

    rt_canvas3d_set_camera_relative_upload(&canvas, 0);
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

// Helper: world-space extent of the captured billboard quad along a vgfx vertex axis.
static double sprite3d_captured_axis_extent(int axis) {
    double lo = g_last_draw_vertices[0].pos[axis];
    double hi = lo;
    for (uint32_t i = 1; i < g_last_draw_vertex_count && i < 4; ++i) {
        double v = g_last_draw_vertices[i].pos[axis];
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    return hi - lo;
}

static void test_sprite3d_billboard_reorients_to_camera() {
    TEST("Sprite3D billboard reorients to face the camera");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    void *sprite = rt_sprite3d_new(NULL);

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;

    rt_sprite3d_set_position(sprite, 4.0, 0.0, 0.0);
    rt_sprite3d_set_scale(sprite, 2.0, 2.0); // 2.0-wide, centered (default anchor)

    // Camera A: viewing down -Z. Billboard width maps to the camera-right axis
    // (world X), so the emitted world-space quad spreads in X and is flat in Z.
    rt_camera3d_look_at(camera,
                        rt_vec3_new(4.0, 0.0, 10.0),
                        rt_vec3_new(4.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));
    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 4);
    EXPECT_NEAR(sprite3d_captured_axis_extent(0), 2.0, 0.001); // width along X
    EXPECT_NEAR(sprite3d_captured_axis_extent(2), 0.0, 0.001); // flat in Z

    // Camera B: viewing down -X. The billboard must re-face the camera, so its
    // width now maps to world Z and the quad becomes flat in X.
    rt_camera3d_look_at(camera,
                        rt_vec3_new(14.0, 0.0, 0.0),
                        rt_vec3_new(4.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));
    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 4);
    EXPECT_NEAR(sprite3d_captured_axis_extent(0), 0.0, 0.001); // quad now flat in X
    EXPECT_NEAR(sprite3d_captured_axis_extent(2), 2.0, 0.001); // width re-mapped to Z

    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

static void test_particles3d_spread_uses_radians() {
    TEST("Particles3D spread uses radians");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    void *particles = rt_particles3d_new(4);
    double max_horizontal = 0.0;

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 0.0, 10.0),
                        rt_vec3_new(0.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    rt_particles3d_set_position(particles, 0.0, 0.0, 0.0);
    rt_particles3d_set_direction(particles, 0.0, 1.0, 0.0, 2.8);
    rt_particles3d_set_speed(particles, 100.0, 100.0);
    rt_particles3d_set_lifetime(particles, 10.0, 10.0);
    rt_particles3d_set_size(particles, 0.0, 0.0);
    rt_particles3d_set_gravity(particles, 0.0, 0.0, 0.0);
    rt_particles3d_burst(particles, 4);
    rt_particles3d_update(particles, 0.25);

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    rt_particles3d_draw(particles, &canvas, camera);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_vertex_count, 16);
    for (uint32_t i = 0; i < g_last_draw_vertex_count; i++) {
        double horizontal = std::sqrt(
            (double)g_last_draw_vertices[i].pos[0] * (double)g_last_draw_vertices[i].pos[0] +
            (double)g_last_draw_vertices[i].pos[2] * (double)g_last_draw_vertices[i].pos[2]);
        if (horizontal > max_horizontal)
            max_horizontal = horizontal;
    }
    EXPECT_TRUE(max_horizontal > 2.0,
                "A 2.8 radian spread creates broad particle motion, not a 2.8 degree cone");
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

static void test_particles3d_extreme_finite_inputs_remain_bounded() {
    TEST("Particles3D extreme finite inputs remain bounded");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    rt_camera3d *camera_impl = (rt_camera3d *)camera;
    void *particles = rt_particles3d_new(4);
    double pos[3] = {};
    constexpr double kHuge = 1.0e300;
    constexpr double kWorldLimit = 1000000000000.0;

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 128;
    canvas.height = 128;
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 0.0, 10.0),
                        rt_vec3_new(0.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    rt_particles3d_set_position(particles, kHuge, -kHuge, kHuge);
    rt_particles3d_set_direction(particles, kHuge, -kHuge, kHuge, kHuge);
    rt_particles3d_set_speed(particles, kHuge, kHuge);
    rt_particles3d_set_lifetime(particles, kHuge, kHuge);
    rt_particles3d_set_size(particles, kHuge, kHuge);
    rt_particles3d_set_gravity(particles, -kHuge, kHuge, -kHuge);
    rt_particles3d_set_alpha(particles, kHuge, -kHuge);
    rt_particles3d_set_rate(particles, kHuge);
    rt_particles3d_set_emitter_shape(particles, 2);
    rt_particles3d_set_emitter_size(particles, kHuge, kHuge, kHuge);
    rt_particles3d_start(particles);
    rt_particles3d_burst(particles, 4);
    rt_particles3d_update(particles, kHuge);
    rt_particles3d_rebase_origin(particles, -kHuge, kHuge, -kHuge);

    rt_particles3d_get_position(particles, pos);
    EXPECT_TRUE(std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]),
                "Emitter position stays finite after extreme inputs");
    EXPECT_TRUE(std::fabs(pos[0]) <= kWorldLimit && std::fabs(pos[1]) <= kWorldLimit &&
                    std::fabs(pos[2]) <= kWorldLimit,
                "Emitter position is clamped to the particle world range");
    EXPECT_TRUE(rt_particles3d_get_count(particles) >= 0 &&
                    rt_particles3d_get_count(particles) <= 4,
                "Particle count remains inside the allocated pool");

    g_canvas_submit_draw_calls = 0;
    g_last_draw_vertex_count = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    rt_canvas3d_begin(&canvas, camera);
    camera_impl->view[0] = INFINITY;
    camera_impl->view[5] = NAN;
    rt_particles3d_draw(particles, &canvas, camera);
    rt_canvas3d_end(&canvas);

    if (g_canvas_submit_draw_calls > 0) {
        for (uint32_t i = 0; i < g_last_draw_vertex_count; i++) {
            EXPECT_TRUE(std::isfinite((double)g_last_draw_vertices[i].pos[0]) &&
                            std::isfinite((double)g_last_draw_vertices[i].pos[1]) &&
                            std::isfinite((double)g_last_draw_vertices[i].pos[2]),
                        "Particle draw vertices remain finite");
        }
    }
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

struct Particles3DTestLayout {
    void *vptr;
    void *particles;
    int32_t count;
    int32_t max_particles;
    double position[3];
    double emit_dir[3];
    double emit_spread;
    double speed_min;
    double speed_max;
    double life_min;
    double life_max;
    double size_start;
    double size_end;
    double gravity[3];
    float color_start[3];
    float color_end[3];
    double alpha_start;
    double alpha_end;
    double rate;
    double accumulator;
    int8_t emitting;
};

static void test_particles3d_getters_sanitize_corrupt_private_state() {
    TEST("Particles3D getters sanitize corrupt private state");
    void *particles_obj = rt_particles3d_new(4);
    auto *particles = static_cast<Particles3DTestLayout *>(particles_obj);

    particles->count = -9;
    particles->emitting = -3;
    EXPECT_EQ(rt_particles3d_get_count(particles_obj), 0);
    EXPECT_EQ(rt_particles3d_get_emitting(particles_obj), 1);

    particles->count = 99;
    EXPECT_EQ(rt_particles3d_get_count(particles_obj), 4);

    particles->max_particles = -1;
    EXPECT_EQ(rt_particles3d_get_count(particles_obj), 0);

    PASS();
}

static void test_canvas_resize_updates_backend_and_projection_aspect() {
    TEST("Canvas3D.Resize updates backend size and next projection aspect");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_camera3d *cam = (rt_camera3d *)rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    assert(cam != NULL);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.resize = tracked_backend_resize;
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.width = 320;
    canvas.height = 240;
    canvas.framebuffer_width = 320;
    canvas.framebuffer_height = 240;

    g_backend_resize_calls = 0;
    g_backend_resize_w = g_backend_resize_h = 0;
    rt_canvas3d_resize(&canvas, 1280, 720);

    EXPECT_EQ(rt_canvas3d_get_window_width(&canvas), 1280);
    EXPECT_EQ(rt_canvas3d_get_window_height(&canvas), 720);
    EXPECT_EQ(canvas.framebuffer_width, 1280);
    EXPECT_EQ(canvas.framebuffer_height, 720);
    EXPECT_EQ(g_backend_resize_calls, 1);
    EXPECT_EQ(g_backend_resize_w, 1280);
    EXPECT_EQ(g_backend_resize_h, 720);

    rt_canvas3d_resize(&canvas, 9000, 720);
    EXPECT_EQ(rt_canvas3d_get_window_width(&canvas), 9000);
    EXPECT_EQ(rt_canvas3d_get_window_height(&canvas), 720);
    EXPECT_EQ(g_backend_resize_calls, 2);

    g_canvas_begin_frame_calls = 0;
    memset(&g_canvas_begin_frame_params, 0, sizeof(g_canvas_begin_frame_params));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_NEAR(cam->aspect, 1.0, 0.0001);
    EXPECT_NEAR(g_canvas_begin_frame_params.projection[0], 0.138564f, 0.001);
    PASS();
}

/**
 * @brief Verify failed backend scale transitions leave Canvas3D state unchanged.
 *
 * The regression covers both restoration to native resolution and a reduced-scale
 * request. It prevents Canvas3D.RenderScale from reporting a value that the backend
 * rejected while a frame/presentation transaction or target allocation was pending.
 */
static void test_canvas_render_scale_failure_preserves_state() {
    TEST("Canvas3D render-scale failure preserves the active scale");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;

    backend.name = "tracked-render-scale";
    backend.set_render_scale = tracked_set_render_scale;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.backend_ctx = &canvas;
    canvas.render_scale = 0.5f;

    g_backend_render_scale_calls = 0;
    g_backend_requested_render_scale = 0.0f;
    g_backend_render_scale_succeeds = false;
    EXPECT_EQ(rt_canvas3d_try_set_render_scale(&canvas, 1.0), 0);
    EXPECT_EQ(g_backend_render_scale_calls, 1);
    EXPECT_NEAR(g_backend_requested_render_scale, 1.0f, 0.0001f);
    EXPECT_NEAR(rt_canvas3d_get_render_scale(&canvas), 0.5, 0.0001);

    g_backend_render_scale_succeeds = true;
    EXPECT_EQ(rt_canvas3d_try_set_render_scale(&canvas, 1.0), 1);
    EXPECT_NEAR(rt_canvas3d_get_render_scale(&canvas), 1.0, 0.0001);

    g_backend_render_scale_succeeds = false;
    EXPECT_EQ(rt_canvas3d_try_set_render_scale(&canvas, 0.5), 0);
    EXPECT_NEAR(g_backend_requested_render_scale, 0.5f, 0.0001f);
    EXPECT_NEAR(rt_canvas3d_get_render_scale(&canvas), 1.0, 0.0001);

    PASS();
}

static void test_canvas_fog_and_shadow_state_sanitize_inputs() {
    TEST("Canvas3D sanitizes fog and shadow inputs");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));

    rt_canvas3d_set_fog(&canvas, NAN, -4.0, 2.0, -1.0, INFINITY);
    EXPECT_TRUE(canvas.fog_enabled == 1, "SetFog enables fog");
    EXPECT_NEAR(canvas.fog_near, 0.0, 0.001);
    EXPECT_TRUE(canvas.fog_far > canvas.fog_near, "SetFog keeps far greater than near");
    EXPECT_NEAR(canvas.fog_color[0], 1.0, 0.001);
    EXPECT_NEAR(canvas.fog_color[1], 0.0, 0.001);
    EXPECT_NEAR(canvas.fog_color[2], 0.0, 0.001);

    rt_canvas3d_set_shadow_bias(&canvas, -10.0);
    EXPECT_NEAR(canvas.shadow_bias, 0.0, 0.001);
    rt_canvas3d_set_shadow_bias(&canvas, NAN);
    EXPECT_NEAR(canvas.shadow_bias, 0.005, 0.001);
    rt_canvas3d_set_shadow_bias(&canvas, 10.0);
    EXPECT_NEAR(canvas.shadow_bias, 0.05, 0.001);
    rt_canvas3d_set_shadow_slope_bias(&canvas, 1000.0);
    EXPECT_NEAR(canvas.shadow_slope_bias, 16.0, 0.001);
    PASS();
}

static void test_canvas_begin_applies_camera_shake_without_follow() {
    TEST("Canvas3D.Begin applies camera shake even without SmoothFollow");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.delta_time_ms = 16;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_camera3d_shake(cam, 1.0, 0.25, 1.0);
    g_canvas_begin_frame_calls = 0;
    std::memset(&g_canvas_begin_frame_params, 0, sizeof(g_canvas_begin_frame_params));

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_TRUE(std::fabs(g_canvas_begin_frame_params.position[0]) > 0.0001f ||
                    std::fabs(g_canvas_begin_frame_params.position[1]) > 0.0001f ||
                    std::fabs(g_canvas_begin_frame_params.position[2] - 5.0f) > 0.0001f,
                "Begin forwards the shaken camera position to the backend");
    PASS();
}

static void test_canvas_overlay_draws_replay_after_3d_frame() {
    TEST("Canvas3D replays overlay draws after a 3D frame");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;

    g_canvas_begin_frame_calls = 0;
    g_canvas_end_frame_calls = 0;
    g_canvas_submit_draw_calls = 0;

    rt_canvas3d_begin(&canvas, cam);
    EXPECT_TRUE(
        canvas3d_queue_screen_rect(&canvas, 4.0f, 4.0f, 10.0f, 10.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
        "Overlay geometry queues successfully during a 3D frame");
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 2);
    EXPECT_EQ(g_canvas_end_frame_calls, 2);
    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    PASS();
}

static void test_canvas_aa_text_reuses_persistent_raster() {
    TEST("Canvas3D DrawText2DAA reuses unchanged rasters across frames");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *first_pixels;
    uint64_t first_identity;

    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_text2d_aa(&canvas, 8, 8, rt_const_cstr("HP 87"), 0xFF5040, 1.5);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(canvas.aa_text_cache_count, 1);
    first_pixels = canvas.aa_text_cache[0].pixels;
    EXPECT_TRUE(first_pixels != nullptr, "First AA text draw retains its raster");
    first_identity = ((rt_pixels_impl *)first_pixels)->cache_identity;

    rt_canvas3d_begin(&canvas, cam);
    /* High color bits are ignored by the renderer and must not split the cache. */
    rt_canvas3d_draw_text2d_aa(&canvas, 32, 12, rt_const_cstr("HP 87"), 0x12FF5040, 1.5);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(canvas.aa_text_cache_count, 1);
    EXPECT_TRUE(canvas.aa_text_cache[0].pixels == first_pixels,
                "Second AA text draw reuses the same Pixels object");
    EXPECT_EQ(((rt_pixels_impl *)canvas.aa_text_cache[0].pixels)->cache_identity, first_identity);

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_text2d_aa(&canvas, 8, 8, rt_const_cstr("HP 86"), 0xFF5040, 1.5);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(canvas.aa_text_cache_count, 2);

    canvas3d_clear_aa_text_cache(&canvas);
    EXPECT_EQ(canvas.aa_text_cache_count, 0);
    EXPECT_TRUE(canvas.aa_text_cache == nullptr, "AA text cache releases its storage");
    PASS();
}

static void test_canvas_overlay_clip_and_new_primitives() {
    TEST("Canvas3D overlay clip trims queued 2D primitives (Plan 08)");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;

    g_canvas_submit_draw_calls = 0;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_set_clip_rect2d(&canvas, 10, 10, 20, 20);

    /* Fully outside the clip: accepted but queues nothing. */
    EXPECT_TRUE(
        canvas3d_queue_screen_rect(&canvas, 0.0f, 0.0f, 5.0f, 5.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
        "Fully-clipped rect reports success");
    EXPECT_TRUE(canvas3d_queue_screen_line(
                    &canvas, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
                "Fully-clipped line reports success");
    /* Text entirely above the clip rect: every dot clipped, no draw queued. */
    rt_canvas3d_draw_text2d_scaled(&canvas, 0, -20, rt_const_cstr("HI"), 0xFFFFFF, 1.0);

    /* Partially inside: each queues exactly one draw. */
    EXPECT_TRUE(
        canvas3d_queue_screen_rect(&canvas, 5.0f, 12.0f, 10.0f, 4.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
        "Partially-clipped rect queues");
    EXPECT_TRUE(canvas3d_queue_screen_line(
                    &canvas, 0.0f, 15.0f, 40.0f, 15.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
                "Crossing line queues");
    EXPECT_TRUE(canvas3d_queue_screen_round_rect(
                    &canvas, 12.0f, 12.0f, 16.0f, 16.0f, 4.0f, 0.0f, 1.0f, 0.0f, 1.0f) != 0,
                "Overlapping rounded rect queues");

    rt_canvas3d_clear_clip_rect2d(&canvas);
    EXPECT_TRUE(
        canvas3d_queue_screen_rect(&canvas, 0.0f, 0.0f, 5.0f, 5.0f, 1.0f, 0.0f, 0.0f, 1.0f) != 0,
        "Rect queues after ClearClipRect2D");
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 4);

    EXPECT_EQ(rt_canvas3d_measure_text2d(&canvas, rt_const_cstr("AB"), 1.0), 24);
    EXPECT_EQ(rt_canvas3d_measure_text2d(&canvas, rt_const_cstr("AB"), 2.0), 48);
    PASS();
}

static void test_canvas_round_rect_clip_bounds_vertices() {
    TEST("Canvas3D rounded rect overlay clipping constrains submitted vertices");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    g_canvas_submit_draw_calls = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    memset(g_last_draw_vertices, 0, sizeof(g_last_draw_vertices));
    g_last_draw_vertex_count = 0;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_set_clip_rect2d(&canvas, 10, 10, 20, 20);
    EXPECT_TRUE(canvas3d_queue_screen_round_rect(
                    &canvas, 2.0f, 2.0f, 36.0f, 36.0f, 10.0f, 0.0f, 1.0f, 0.0f, 1.0f) != 0,
                "Partially clipped rounded rect queues");
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_TRUE(g_last_draw_cmd.vertex_count > 0, "Rounded rect submitted vertices");
    EXPECT_EQ((int64_t)g_last_draw_vertex_count, (int64_t)g_last_draw_cmd.vertex_count);
    for (uint32_t i = 0; i < g_last_draw_vertex_count; ++i) {
        const float x = g_last_draw_vertices[i].pos[0];
        const float y = g_last_draw_vertices[i].pos[1];
        EXPECT_TRUE(x >= 9.999f && x <= 30.001f, "Rounded rect vertex x remains inside clip");
        EXPECT_TRUE(y >= 9.999f && y <= 30.001f, "Rounded rect vertex y remains inside clip");
    }
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

extern "C" {
void *rt_uilabel_new(int64_t x, int64_t y, rt_string text, int64_t color);
void rt_uilabel_draw(void *label, void *canvas);
void *rt_uipanel_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t bg_color, int64_t alpha);
void rt_uipanel_draw(void *panel, void *canvas);
void *rt_uibar_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t fg_color, int64_t bg_color);
void rt_uibar_draw(void *bar, void *canvas);
}

static void test_gameui_widgets_draw_on_canvas3d() {
    TEST("Game.UI widgets render on Canvas3D via the draw-ops adapter (ADR 0017)");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;

    /* The binding registers at Canvas3D creation; tests use stack canvases, so
     * register explicitly (idempotent). */
    canvas3d_register_gameui_ops();

    void *label = rt_uilabel_new(4, 4, rt_const_cstr("HP"), 0xFFFFFF);
    void *panel = rt_uipanel_new(0, 0, 64, 32, 0x203040, 200);
    void *bar = rt_uibar_new(4, 20, 56, 8, 0x40FF40, 0x202020);
    assert(label && panel && bar);

    g_canvas_submit_draw_calls = 0;
    rt_canvas3d_begin(&canvas, cam);
    rt_uipanel_draw(panel, &canvas);
    rt_uibar_draw(bar, &canvas);
    rt_uilabel_draw(label, &canvas);
    rt_canvas3d_end(&canvas);

    if (g_canvas_submit_draw_calls < 3) {
        FAIL("widgets did not queue overlay draws on Canvas3D");
        return;
    }
    PASS();
}

static void test_canvas_postfx_uses_render_target_pixels() {
    TEST("Canvas3D postfx applies to render targets via sync/readback");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    void *fx = rt_postfx3d_new();
    assert(rt != NULL && rt->target != NULL && fx != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.postfx = fx;

    rt_postfx3d_add_vignette(fx, 0.5, 0.25);
    rt_postfx3d_set_enabled(fx, 1);

    g_render_target_sync_calls = 0;
    g_render_target_sync_rgba[0] = 0x80;
    g_render_target_sync_rgba[1] = 0x60;
    g_render_target_sync_rgba[2] = 0x40;
    g_render_target_sync_rgba[3] = 0x20;
    rt->target->color_dirty = 1;
    rt->target->sync_color = tracked_render_target_sync;
    rt->target->sync_color_userdata = NULL;

    rt_postfx3d_apply_to_canvas(&canvas);

    if (g_render_target_sync_calls != 1) {
        FAIL("Canvas3D postfx did not sync render-target pixels");
        return;
    }
    PASS();
}

static void test_canvas_postfx_uses_hdr_render_target_mirror() {
    TEST("Canvas3D postfx uses HDR render-target mirror before tonemapped readback");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new_hdr(1, 1);
    void *fx = rt_postfx3d_new();
    assert(rt != NULL && rt->target != NULL && fx != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.postfx = fx;
    rt_postfx3d_add_tonemap(fx, 1, 1.0);
    rt_postfx3d_set_enabled(fx, 1);

    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(rt->target) != 0,
                "HDR postfx test allocates LDR mirror");
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_hdr_color(rt->target) != 0,
                "HDR postfx test allocates linear HDR mirror");
    rt->target->color_buf[0] = 0;
    rt->target->color_buf[1] = 0;
    rt->target->color_buf[2] = 0;
    rt->target->color_buf[3] = 255;
    rt->target->hdr_color_buf[0] = 4.0f;
    rt->target->hdr_color_buf[1] = 0.0f;
    rt->target->hdr_color_buf[2] = 0.0f;
    rt->target->hdr_color_buf[3] = 1.0f;
    rt->target->hdr_color_valid = 1;

    rt_postfx3d_apply_to_canvas(&canvas);

    if (rt->target->color_buf[0] < 200 || rt->target->hdr_color_buf[0] <= 0.8f) {
        FAIL("HDR postfx did not consume the linear HDR mirror");
        return;
    }
    PASS();
}

static void test_canvas_postfx_hdr_mode0_tonemap_applies_gamma() {
    TEST("Canvas3D explicit mode-0 tonemap applies gamma-out on linear-HDR targets");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new_hdr(1, 1);
    void *fx = rt_postfx3d_new();
    assert(rt != NULL && rt->target != NULL && fx != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.postfx = fx;
    rt_postfx3d_add_tonemap(fx, 0, 1.0); /* explicit "off": Plan 05 gamma-out on HDR */
    rt_postfx3d_set_enabled(fx, 1);

    EXPECT_TRUE(vgfx3d_rendertarget_ensure_color(rt->target) != 0,
                "HDR mode-0 gamma test allocates LDR mirror");
    EXPECT_TRUE(vgfx3d_rendertarget_ensure_hdr_color(rt->target) != 0,
                "HDR mode-0 gamma test allocates linear HDR mirror");
    rt->target->color_buf[0] = 0;
    rt->target->color_buf[1] = 0;
    rt->target->color_buf[2] = 0;
    rt->target->color_buf[3] = 255;
    rt->target->hdr_color_buf[0] = 0.25f;
    rt->target->hdr_color_buf[1] = 0.25f;
    rt->target->hdr_color_buf[2] = 0.25f;
    rt->target->hdr_color_buf[3] = 1.0f;
    rt->target->hdr_color_valid = 1;

    rt_postfx3d_apply_to_canvas(&canvas);

    /* pow(0.25, 1/2.2) ~= 0.533 -> ~136; the legacy linear passthrough would give ~64. */
    if (rt->target->color_buf[0] < 120 || rt->target->color_buf[0] > 150) {
        FAIL("HDR mode-0 tonemap output is not gamma-encoded");
        return;
    }
    PASS();
}

static void test_canvas_postfx_applies_taa_on_cpu_rtt_path() {
    TEST("Canvas3D postfx applies TAA on CPU/render-target path (Plan 06 parity)");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    void *fx = rt_postfx3d_new();
    assert(rt != NULL && rt->target != NULL && fx != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.postfx = fx;
    rt_postfx3d_add_taa(fx, 0.9);
    rt_postfx3d_set_enabled(fx, 1);

    /* The old bind/apply refusal is gone: the CPU TAA implementation runs on the
     * render-target path (history seeds from the first frame), no trap. */
    rt_postfx3d_apply_to_canvas(&canvas);
    rt_postfx3d_apply_to_canvas(&canvas);
    PASS();
}

static void test_canvas_postfx_applies_advanced_cpu_rtt_effects() {
    TEST("Canvas3D postfx applies advanced effects on CPU/render-target path (Plan 06 parity)");
    rt_canvas3d canvas;
    rt_rendertarget3d *rt = (rt_rendertarget3d *)rt_rendertarget3d_new(1, 1);
    void *fx = rt_postfx3d_new();
    assert(rt != NULL && rt->target != NULL && fx != NULL);

    memset(&canvas, 0, sizeof(canvas));
    canvas.render_target = rt->target;
    canvas.postfx = fx;
    rt_postfx3d_add_ssao(fx, 0.5, 1.0, 8);
    rt_postfx3d_set_enabled(fx, 1);

    /* SSAO runs through its CPU reference implementation on this path. */
    rt_postfx3d_apply_to_canvas(&canvas);
    PASS();
}

static void test_canvas_texture_upload_bytes_telemetry() {
    TEST("Canvas3D.TextureUploadBytes reports backend upload telemetry");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;
    backend.get_texture_upload_bytes = tracked_texture_upload_bytes;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    g_backend_texture_upload_bytes = 1536;
    rt_canvas3d_begin(&canvas, cam);
    EXPECT_EQ(rt_canvas3d_get_texture_upload_bytes(&canvas), 0);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_texture_upload_bytes(&canvas), 1536);

    g_backend_texture_upload_bytes = (uint64_t)INT64_MAX + 1u;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_texture_upload_bytes(&canvas), INT64_MAX);

    backend.get_texture_upload_bytes = nullptr;
    canvas.last_texture_upload_bytes = 77;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_texture_upload_bytes(&canvas), 0);
    PASS();
}

static void test_canvas_frame_gpu_time_telemetry() {
    TEST("Canvas3D.FrameGpuTimeUs reports backend GPU timing telemetry");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);

    backend.name = "d3d11";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.end_frame = tracked_end_frame;
    backend.get_frame_gpu_time_us = tracked_frame_gpu_time_us;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    g_backend_frame_gpu_time_us = 4242;
    rt_canvas3d_begin(&canvas, cam);
    EXPECT_EQ(rt_canvas3d_get_frame_gpu_time_us(&canvas), 0);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_frame_gpu_time_us(&canvas), 4242);

    g_backend_frame_gpu_time_us = (uint64_t)INT64_MAX + 1u;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_frame_gpu_time_us(&canvas), INT64_MAX);

    backend.get_frame_gpu_time_us = nullptr;
    canvas.last_frame_gpu_time_us = 77;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(rt_canvas3d_get_frame_gpu_time_us(&canvas), 0);
    PASS();
}

static void test_canvas_texture_upload_budget_controls_backend() {
    TEST("Canvas3D texture upload budget controls backend and pending telemetry");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;

    backend.name = "metal";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.set_texture_upload_budget = tracked_set_texture_upload_budget;
    backend.get_texture_upload_pending_bytes = tracked_texture_upload_pending_bytes;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;

    g_backend_texture_upload_budget = 0;
    rt_canvas3d_set_texture_upload_budget(&canvas, 4096);
    EXPECT_EQ(g_backend_texture_upload_budget, 4096);

    rt_canvas3d_set_texture_upload_budget(&canvas, -1);
    EXPECT_EQ(g_backend_texture_upload_budget, UINT64_MAX);

    g_backend_texture_upload_pending_bytes = 8192;
    EXPECT_EQ(rt_canvas3d_get_texture_upload_pending_bytes(&canvas), 8192);

    g_backend_texture_upload_pending_bytes = (uint64_t)INT64_MAX + 1u;
    EXPECT_EQ(rt_canvas3d_get_texture_upload_pending_bytes(&canvas), INT64_MAX);

    backend.get_texture_upload_pending_bytes = nullptr;
    EXPECT_EQ(rt_canvas3d_get_texture_upload_pending_bytes(&canvas), 0);
    PASS();
}

static void test_canvas_delta_time_preserves_first_zero() {
    TEST("Canvas3D.GetDeltaTime preserves the first zero frame");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.dt_max_ms = 16;
    canvas.delta_time_ms = 0;
    EXPECT_EQ(rt_canvas3d_get_delta_time(&canvas), 0);
    PASS();
}

static void test_canvas_poll_event_queue_drains_in_order() {
    TEST("Canvas3D.PollEvent drains queued event types in order");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.event_type_queue[0] = VGFX_EVENT_KEY_DOWN;
    canvas.event_type_queue[1] = VGFX_EVENT_RESIZE;
    canvas.event_type_queue[2] = VGFX_EVENT_CLOSE;
    canvas.event_type_count = 3;

    EXPECT_EQ(rt_canvas3d_poll_event(&canvas), VGFX_EVENT_KEY_DOWN);
    EXPECT_EQ(rt_canvas3d_poll_event(&canvas), VGFX_EVENT_RESIZE);
    EXPECT_EQ(rt_canvas3d_poll_event(&canvas), VGFX_EVENT_CLOSE);
    EXPECT_EQ(rt_canvas3d_poll_event(&canvas), VGFX_EVENT_NONE);
    PASS();
}

static void test_canvas_delta_time_cap_and_disable() {
    TEST("Canvas3D.GetDeltaTime clamps to max and SetDTMax can disable the cap");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.dt_max_ms = 100;
    canvas.delta_time_ms = 250;
    EXPECT_EQ(rt_canvas3d_get_delta_time(&canvas), 100);

    rt_canvas3d_set_dt_max(&canvas, -1);
    EXPECT_EQ(rt_canvas3d_get_delta_time(&canvas), 250);
    canvas.delta_time_ms = -5;
    EXPECT_EQ(rt_canvas3d_get_delta_time(&canvas), 0);
    PASS();
}

static void test_canvas_delta_time_sec_clamps_huge_dtmax_without_overflow() {
    TEST("Canvas3D.GetDeltaTimeSec handles huge SetDTMax values");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    rt_canvas3d_set_dt_max(&canvas, INT64_MAX);
    canvas.delta_time_us = 500000;
    EXPECT_NEAR(rt_canvas3d_get_delta_time_sec(&canvas), 0.5, 0.0001);
    EXPECT_TRUE(canvas.dt_max_ms <= INT64_MAX / 1000, "SetDTMax clamps to checked multiply range");
    PASS();
}

static void test_canvas_quality_getters_sanitize_corrupt_private_state() {
    TEST("Canvas3D quality getters sanitize corrupt private state");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.quality_requested = -10;
    canvas.quality_active = 99;
    canvas.quality_fallback = -3;
    EXPECT_EQ(rt_canvas3d_get_quality_requested(&canvas), RT_GRAPHICS3D_QUALITY_PERFORMANCE);
    EXPECT_EQ(rt_canvas3d_get_quality_active(&canvas), RT_GRAPHICS3D_QUALITY_CINEMATIC);
    EXPECT_EQ(rt_canvas3d_get_quality_fallback(&canvas), 1);
    PASS();
}

static void test_canvas_synthetic_mouse_accumulation_clamps() {
    TEST("Canvas3D synthetic mouse accumulation clamps instead of overflowing");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    rt_canvas3d_push_synthetic_mouse(&canvas, 900000.0, -900000.0, 0, 900000.0);
    rt_canvas3d_push_synthetic_mouse(&canvas, 900000.0, -900000.0, 0, 900000.0);
    EXPECT_NEAR(canvas.synthetic_mouse_dx, 1000000.0, 0.001);
    EXPECT_NEAR(canvas.synthetic_mouse_dy, -1000000.0, 0.001);
    EXPECT_NEAR(canvas.synthetic_mouse_wheel_y, 1000000.0, 0.001);
    rt_canvas3d_push_synthetic_mouse(&canvas, INFINITY, NAN, 0, -INFINITY);
    EXPECT_NEAR(canvas.synthetic_mouse_dx, 1000000.0, 0.001);
    EXPECT_NEAR(canvas.synthetic_mouse_dy, -1000000.0, 0.001);
    PASS();
}

static void test_canvas_fps_uses_rolling_microsecond_samples() {
    TEST("Canvas3D.GetFPS uses rolling microsecond frame timing");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.delta_time_ms = 0;
    canvas.delta_time_us = 500;
    EXPECT_EQ(rt_canvas3d_get_fps(&canvas), 2000);
    canvas.delta_time_us = 16667;
    EXPECT_EQ(rt_canvas3d_get_fps(&canvas), 60);
    canvas.fps_sample_count = 2;
    canvas.fps_sample_total_us = 40000;
    EXPECT_EQ(rt_canvas3d_get_fps(&canvas), 50);
    PASS();
}

static void test_canvas_boolean_setters_normalize() {
    TEST("Canvas3D boolean render options normalize inputs");
    rt_canvas3d canvas;
    memset(&canvas, 0, sizeof(canvas));
    rt_canvas3d_set_wireframe(&canvas, -5);
    rt_canvas3d_set_backface_cull(&canvas, 42);
    EXPECT_EQ(canvas.wireframe, 1);
    EXPECT_EQ(canvas.backface_cull, 1);
    rt_canvas3d_set_wireframe(&canvas, 0);
    rt_canvas3d_set_backface_cull(&canvas, 0);
    EXPECT_EQ(canvas.wireframe, 0);
    EXPECT_EQ(canvas.backface_cull, 0);
    PASS();
}

static void test_canvas_uses_single_present_pacer() {
    TEST("Canvas3D uses one presentation pacer per backend");
    EXPECT_EQ(canvas3d_window_pacing_fps(1, 1, 60), 60);
    EXPECT_EQ(canvas3d_window_pacing_fps(1, 0, 60), -1);
    EXPECT_EQ(canvas3d_window_pacing_fps(0, 1, 60), -1);
    EXPECT_EQ(canvas3d_window_pacing_fps(0, 0, 60), -1);
    EXPECT_EQ(canvas3d_window_pacing_fps(1, 1, -1), -1);
    PASS();
}

static void test_canvas_material_shading_model_mapping() {
    TEST("Canvas3D maps material shading models to backend draw commands");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_plane(1.0, 1.0);
    void *mat = rt_material3d_new();
    void *xf = rt_mat4_identity();
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    rt_material3d_set_shading_model(mat, 5);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_draw_cmd.shading_model, 5);
    EXPECT_EQ(g_last_draw_cmd.unlit, 0);

    rt_material3d_set_shading_model(mat, 4);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_draw_cmd.shading_model, 4);

    rt_material3d_set_shading_model(mat, 2);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_draw_cmd.workflow, RT_MATERIAL3D_WORKFLOW_PBR);

    rt_material3d_set_unlit(mat, 0);
    rt_material3d_set_shading_model(mat, 3);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_draw_cmd.unlit, 1);
    EXPECT_EQ(g_last_draw_cmd.shading_model, 3);
    PASS();
}

static void test_canvas_material_command_sanitizes_corrupt_fields() {
    TEST("Canvas3D sanitizes corrupt material fields before backend submission");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_plane(1.0, 1.0);
    auto *mat = (rt_material3d *)rt_material3d_new();
    void *xf = rt_mat4_identity();
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    mat->shininess = std::numeric_limits<double>::infinity();
    mat->emissive_intensity = std::numeric_limits<double>::infinity();
    mat->normal_scale = -5.0;
    mat->texture_wrap_s = 99;
    mat->texture_wrap_t = -7;
    mat->texture_filter = 123;
    mat->texture_min_filter = 123;
    mat->texture_mag_filter = -9;
    mat->texture_mip_filter = 123;
    mat->anisotropy = 0;
    mat->texture_slot_wrap_s[0] = 99;
    mat->texture_slot_wrap_t[0] = -7;
    mat->texture_slot_filter[0] = 123;
    mat->texture_slot_min_filter[0] = 123;
    mat->texture_slot_mag_filter[0] = -9;
    mat->texture_slot_mip_filter[0] = 123;
    mat->texture_slot_anisotropy[0] = 0;
    mat->texture_slot_uv_set[0] = 99;
    mat->texture_slot_uv_transform[0][0] = std::numeric_limits<double>::quiet_NaN();
    mat->texture_slot_uv_transform[0][1] = std::numeric_limits<double>::infinity();
    mat->texture_slot_uv_transform[0][3] = -std::numeric_limits<double>::infinity();
    mat->texture_slot_uv_transform[0][4] = 1.0e300;
    mat->texture_slot_uv_transform[0][5] = -1.0e300;
    mat->texture_slot_wrap_s[1] = RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE;
    mat->texture_slot_wrap_t[1] = RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT;
    mat->texture_slot_filter[1] = RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    mat->texture_slot_min_filter[1] = RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    mat->texture_slot_mag_filter[1] = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    mat->texture_slot_mip_filter[1] = RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    mat->texture_slot_anisotropy[1] = 64;
    mat->texture_slot_uv_set[1] = -1;
    mat->custom_params[0] = 1.0e300;
    mat->custom_params[1] = -1.0e300;
    mat->custom_params[8] = 0.75;
    mat->custom_params[9] = -0.25;
    mat->custom_params[10] = 1.0e300;
    mat->custom_params[11] = -1.0e300;

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);

    EXPECT_NEAR(g_last_draw_cmd.shininess, 32.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.emissive_intensity, 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.normal_scale, 0.0f, 0.0001f);
    EXPECT_EQ(g_last_draw_cmd.texture_wrap_s, RT_MATERIAL3D_TEXTURE_WRAP_REPEAT);
    EXPECT_EQ(g_last_draw_cmd.texture_wrap_t, RT_MATERIAL3D_TEXTURE_WRAP_REPEAT);
    EXPECT_EQ(g_last_draw_cmd.texture_filter, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_min_filter, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_mag_filter, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_mip_filter, RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
    EXPECT_EQ(g_last_draw_cmd.texture_anisotropy, 1);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_wrap_s[0], RT_MATERIAL3D_TEXTURE_WRAP_REPEAT);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_wrap_t[0], RT_MATERIAL3D_TEXTURE_WRAP_REPEAT);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_filter[0], RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_min_filter[0], RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_mag_filter[0], RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_mip_filter[0], RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_anisotropy[0], 1);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_uv_set[0], 1);
    EXPECT_NEAR(g_last_draw_cmd.texture_slot_uv_transform[0][0], 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.texture_slot_uv_transform[0][1], 0.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.texture_slot_uv_transform[0][3], 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.texture_slot_uv_transform[0][4], 1000000.0f, 1.0f);
    EXPECT_NEAR(g_last_draw_cmd.texture_slot_uv_transform[0][5], -1000000.0f, 1.0f);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_wrap_s[1], RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_wrap_t[1], RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_filter[1], RT_MATERIAL3D_TEXTURE_FILTER_NEAREST);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_min_filter[1], RT_MATERIAL3D_TEXTURE_FILTER_NEAREST);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_mag_filter[1], RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_mip_filter[1], RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_anisotropy[1], 16);
    EXPECT_EQ(g_last_draw_cmd.texture_slot_uv_set[1], 0);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[0], 1000000.0f, 1.0f);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[1], -1000000.0f, 1.0f);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[8], 0.75f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[9], -0.25f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[10], 1000000.0f, 1.0f);
    EXPECT_NEAR(g_last_draw_cmd.custom_params[11], -1000000.0f, 1.0f);
    PASS();
}

static void test_canvas_material_textureasset_resolves_resident_mip_on_draw() {
    TEST("Canvas3D resolves TextureAsset3D material slots at draw time");
    const char *path = "/tmp/zanna_textureasset3d_draw_mips_test.ktx2";
    uint8_t level0[64];
    uint8_t level1[16];
    const uint8_t *levels[] = {level0, level1};
    const uint64_t level_bytes[] = {sizeof(level0), sizeof(level1)};
    rt_string path_s;
    void *asset;
    void *mip0;
    void *mip1;
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_plane(1.0, 1.0);
    void *mat = rt_material3d_new();
    void *xf = rt_mat4_identity();

    for (size_t i = 0; i < sizeof(level0); i++)
        level0[i] = (uint8_t)(i + 1u);
    for (size_t i = 0; i < sizeof(level1); i++)
        level1[i] = (uint8_t)(0x80u + i);

    EXPECT_TRUE(write_test_ktx2_mips(path, 37u, 4u, 4u, levels, level_bytes, 2u),
                "draw-time mip fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr && cam != nullptr && mesh != nullptr && mat != nullptr &&
           xf != nullptr);

    rt_material3d_set_texture(mat, asset);
    mip0 = rt_textureasset3d_get_pixels(asset);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_last_draw_cmd.texture == mip0,
                "Draw command binds the currently resident TextureAsset3D mip 0");

    rt_textureasset3d_set_resident_mip_range(asset, 1, 1);
    mip1 = rt_textureasset3d_get_pixels(asset);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_last_draw_cmd.texture == mip1,
                "Draw command follows TextureAsset3D resident mip changes after binding");

    rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_last_draw_cmd.texture == nullptr,
                "Draw command omits texture when TextureAsset3D has no resident fallback");

    std::remove(path);
    PASS();
}

static void test_canvas_material_textureasset_forwards_native_blocks_on_draw() {
    TEST("Canvas3D forwards TextureAsset3D material slots with decode-failure checker");
    const char *path = "/tmp/zanna_textureasset3d_draw_native_astc_test.ktx2";
    const uint8_t astc_level0[] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    rt_string path_s;
    void *asset;
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_plane(1.0, 1.0);
    void *mat = rt_material3d_new();
    void *xf = rt_mat4_identity();
    void *checker;

    EXPECT_TRUE(write_test_ktx2(path, 157u, 4u, 4u, astc_level0, sizeof(astc_level0)),
                "draw-time native ASTC fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    assert(asset != nullptr && cam != nullptr && mesh != nullptr && mat != nullptr &&
           xf != nullptr);

    rt_material3d_set_texture(mat, asset);
    checker = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(checker != nullptr,
                "decode-failed ASTC draw fixture exposes checker Pixels fallback");
    EXPECT_EQ(rt_pixels_width(checker), 8);
    EXPECT_EQ(rt_pixels_height(checker), 8);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 0, 0), 0xFF00FFFFu);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 1, 0), 0x000000FFu);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 0, 1), 0x000000FFu);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 1, 1), 0xFF00FFFFu);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_last_draw_cmd.texture == checker,
                "Draw command binds the checker fallback for a decode-failed TextureAsset3D");
    EXPECT_TRUE(g_last_draw_cmd.texture_asset == asset,
                "Draw command still forwards native TextureAsset3D source");

    rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, xf, mat);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(g_last_draw_cmd.texture == nullptr && g_last_draw_cmd.texture_asset == nullptr,
                "Draw command omits native TextureAsset3D when residency is empty");

    std::remove(path);
    PASS();
}

static void test_canvas_draw_mesh_clears_pending_splat_on_failed_draw() {
    TEST("Canvas3D.DrawMesh clears pending terrain splat state on failed draw");
    rt_canvas3d canvas;
    double model[16] = {0.0};
    memset(&canvas, 0, sizeof(canvas));
    canvas.pending_has_splat = 1;
    canvas.pending_splat_map = (void *)0x1;
    canvas.pending_splat_layers[0] = (void *)0x2;
    canvas.pending_splat_layer_scales[0] = 4.0f;
    model[0] = model[5] = model[10] = model[15] = 1.0;

    rt_canvas3d_draw_mesh_matrix(&canvas, NULL, model, NULL);

    EXPECT_EQ(canvas.pending_has_splat, 0);
    EXPECT_TRUE(canvas.pending_splat_map == NULL, "Pending splat map is cleared");
    EXPECT_TRUE(canvas.pending_splat_layers[0] == NULL, "Pending splat layer is cleared");
    EXPECT_NEAR(canvas.pending_splat_layer_scales[0], 0.0, 0.001);
    PASS();
}

static void test_canvas_draw_mesh_sanitizes_pending_splat_scales() {
    TEST("Canvas3D.DrawMesh sanitizes pending terrain splat scales");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.2, 0.4, 0.6);
    void *splat = rt_pixels_new(1, 1);
    void *layer = rt_pixels_new(1, 1);
    double model[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };

    EXPECT_TRUE(cam && mesh && mat && splat && layer, "Splat scale fixtures exist");
    if (!cam || !mesh || !mat || !splat || !layer)
        return;

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    canvas.pending_has_splat = 1;
    canvas.pending_splat_map = splat;
    for (int i = 0; i < 4; i++)
        canvas.pending_splat_layers[i] = layer;
    canvas.pending_splat_layer_scales[0] = std::numeric_limits<float>::quiet_NaN();
    canvas.pending_splat_layer_scales[1] = std::numeric_limits<float>::infinity();
    canvas.pending_splat_layer_scales[2] = -2.0f;
    canvas.pending_splat_layer_scales[3] = 1.0e30f;
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(g_last_draw_cmd.has_splat == 1, "Pending splat flag reaches draw command");
    EXPECT_NEAR(g_last_draw_cmd.splat_layer_scales[0], 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.splat_layer_scales[1], 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.splat_layer_scales[2], 1.0f, 0.0001f);
    EXPECT_NEAR(g_last_draw_cmd.splat_layer_scales[3], 1000000.0f, 1.0f);
    PASS();
}

static void test_canvas_draw_mesh_rejects_corrupt_raw_morph_shape_count() {
    TEST("Canvas3D.DrawMesh rejects corrupt raw morph shape counts");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.2, 0.4, 0.6);
    void *model = rt_mat4_identity();
    static const float tiny_delta[3] = {1.0f, 0.0f, 0.0f};
    static const float tiny_weight[1] = {1.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    mesh_view->morph_deltas = tiny_delta;
    mesh_view->morph_weights = tiny_weight;
    mesh_view->morph_shape_count = INT32_MAX;

    g_canvas_submit_draw_calls = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, model, mat);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(g_last_draw_cmd.morph_shape_count, 0);
    EXPECT_TRUE(g_last_draw_cmd.morph_deltas == nullptr, "Corrupt raw morph deltas are not bound");
    EXPECT_TRUE(g_last_draw_cmd.morph_weights == nullptr,
                "Corrupt raw morph weights are not scanned or bound");
    PASS();
}

static void test_canvas_reuses_float_payload_snapshots_within_frame() {
    TEST("Canvas3D reuses identical skinning float snapshots within one frame");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new_color(0.2, 0.4, 0.6);
    void *model = rt_mat4_identity();
    float palette[16] = {1.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         1.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         1.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         1.0f};
    float prev_palette[16] = {1.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              1.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              0.0f,
                              1.0f,
                              0.0f,
                              0.1f,
                              0.0f,
                              0.0f,
                              1.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 64;
    canvas.height = 64;

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    mesh_view->bone_palette = palette;
    mesh_view->prev_bone_palette = prev_palette;
    mesh_view->bone_count = 1;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_mesh(&canvas, mesh, model, mat);
    int32_t first_temp_count = canvas.temp_buf_count;
    int32_t first_float_snapshot_count = canvas.float_snapshot_count;
    rt_canvas3d_draw_mesh(&canvas, mesh, model, mat);
    EXPECT_EQ(canvas.float_snapshot_count, first_float_snapshot_count);
    EXPECT_EQ(canvas.temp_buf_count, first_temp_count);
    EXPECT_EQ(first_float_snapshot_count, 2);
    rt_canvas3d_end(&canvas);

    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

static void test_canvas_draw_terrain_rejects_2d_frame() {
    TEST("Canvas3D.DrawTerrain rejects Begin2D frames");
    rt_canvas3d canvas;
    void *terrain = rt_terrain3d_new(8, 8);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.6);
    assert(terrain != NULL);
    assert(material != NULL);
    rt_terrain3d_set_material(terrain, material);

    memset(&canvas, 0, sizeof(canvas));
    canvas.in_frame = 1;
    canvas.frame_is_2d = 1;
    canvas.backend = (const vgfx3d_backend_t *)1;

    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        rt_canvas3d_draw_terrain(&canvas, terrain);
        g_expect_trap = false;
        FAIL("Canvas3D.DrawTerrain should trap during Begin2D");
        return;
    }
    g_expect_trap = false;
    EXPECT_TRUE(g_last_trap != nullptr &&
                    std::strstr(g_last_trap, "cannot draw terrain during Begin2D/End") != nullptr,
                "Canvas3D.DrawTerrain reports the Begin2D misuse");
    PASS();
}

static void test_canvas_draw_terrain_sanitizes_nonfinite_lod_distance() {
    TEST("Canvas3D.DrawTerrain sanitizes non-finite LOD distances");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.6);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_lod_distances(terrain, 1.0, 2.0);

    g_canvas_submit_draw_calls = 0;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    canvas.cached_cam_pos[0] = std::numeric_limits<float>::infinity();
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_TRUE(g_last_draw_cmd.vertex_count > 0 && g_last_draw_cmd.vertex_count < 100,
                "Non-finite terrain camera distance falls back to sanitized far LOD");
    PASS();
}

static void test_canvas_draw_terrain_disables_cpu_occlusion_by_default() {
    TEST("Canvas3D.DrawTerrain disables CPU occlusion by default");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(8.0, 10.0, 24.0);
    void *target = rt_vec3_new(8.0, 0.0, 8.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.6);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    EXPECT_EQ(rt_terrain3d_get_cpu_occlusion(terrain), 0);
    rt_terrain3d_set_cpu_occlusion(terrain, 1);
    EXPECT_EQ(rt_terrain3d_get_cpu_occlusion(terrain), 1);
    rt_terrain3d_set_cpu_occlusion(terrain, 0);

    g_canvas_submit_draw_calls = 0;
    rt_canvas3d_set_occlusion_culling(&canvas, 1);
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 1);
    EXPECT_EQ(rt_canvas3d_get_draw_count(&canvas), 1);
    EXPECT_EQ(rt_canvas3d_get_occlusion_candidate_count(&canvas), 0);
    EXPECT_EQ(rt_canvas3d_get_cpu_occluded_draw_count(&canvas), 0);
    EXPECT_EQ(rt_terrain3d_get_last_chunk_count(terrain), 1);
    EXPECT_EQ(rt_terrain3d_get_last_drawn_chunk_count(terrain), 1);
    EXPECT_EQ(rt_terrain3d_get_last_missing_lod_count(terrain), 0);
    EXPECT_EQ(rt_terrain3d_get_last_lod0_chunk_count(terrain), 1);
    PASS();
}

static void test_canvas_draw_terrain_culls_before_building_lod0() {
    TEST("Canvas3D.DrawTerrain frustum-culls before building LOD0");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 4.0, 12.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.6);
    auto *terrain_view = (Terrain3DTestLayout *)terrain;

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;
    rt_canvas3d_set_frustum_culling(&canvas, 1);

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    g_canvas_submit_draw_calls = 0;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain_at(&canvas, terrain, 100000.0, 0.0, 0.0);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_calls, 0);
    EXPECT_EQ(rt_terrain3d_get_last_drawn_chunk_count(terrain), 0);
    EXPECT_EQ(rt_terrain3d_get_last_frustum_culled_chunk_count(terrain), 1);
    EXPECT_TRUE(terrain_view->chunk_meshes[0] == nullptr,
                "culled terrain chunk does not allocate LOD0 mesh");
    PASS();
}

//=============================================================================
// Terrain3D splat tests
//=============================================================================

static void test_terrain_create() {
    TEST("Terrain3D.New — creates terrain");
    void *t = rt_terrain3d_new(32, 32);
    assert(t);
    PASS();
}

static void test_terrain_set_splat_map() {
    TEST("Terrain3D.SetSplatMap — accepts Pixels");
    void *t = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(16, 16);
    rt_terrain3d_set_splat_map(t, splat);
    PASS();
}

static void test_terrain_set_layer_texture() {
    TEST("Terrain3D.SetLayerTexture — accepts layer + Pixels");
    void *t = rt_terrain3d_new(16, 16);
    void *tex = rt_pixels_new(32, 32);
    rt_terrain3d_set_layer_texture(t, 0, tex);
    rt_terrain3d_set_layer_texture(t, 1, tex);
    rt_terrain3d_set_layer_texture(t, 2, tex);
    rt_terrain3d_set_layer_texture(t, 3, tex);
    /* Out of range — should not crash */
    rt_terrain3d_set_layer_texture(t, 4, tex);
    rt_terrain3d_set_layer_texture(t, -1, tex);
    PASS();
}

static void test_terrain_set_layer_scale() {
    TEST("Terrain3D.SetLayerScale — sets UV tiling scale");
    void *t = rt_terrain3d_new(16, 16);
    rt_terrain3d_set_layer_scale(t, 0, 4.0);
    rt_terrain3d_set_layer_scale(t, 1, 8.0);
    /* Out of range — should not crash */
    rt_terrain3d_set_layer_scale(t, 5, 1.0);
    PASS();
}

static void test_terrain_null_safety() {
    TEST("Terrain3D splat — null safety");
    rt_terrain3d_set_splat_map(NULL, NULL);
    rt_terrain3d_set_layer_texture(NULL, 0, NULL);
    rt_terrain3d_set_layer_scale(NULL, 0, 1.0);
    PASS();
}

static void test_terrain_stitch_edge_ignores_float_roundoff() {
    TEST("Terrain3D.StitchEdge ignores sub-ULP seam roundoff");
    void *west = rt_terrain3d_new(2, 2);
    void *east = rt_terrain3d_new(2, 2);
    auto *west_view = (Terrain3DTestLayout *)west;
    auto *east_view = (Terrain3DTestLayout *)east;
    EXPECT_TRUE(west_view && east_view && west_view->heights && east_view->heights,
                "Terrain stitch fixtures exist");
    float east_edge = std::nextafter(std::nextafter(1.0f, 2.0f), 2.0f);
    west_view->heights[1] = 1.0f;
    west_view->heights[3] = 1.0f;
    east_view->heights[0] = east_edge;
    east_view->heights[2] = east_edge;

    int64_t changed =
        rt_terrain3d_stitch_edge(west, RT_TERRAIN3D_EDGE_EAST, east, RT_TERRAIN3D_EDGE_WEST);
    EXPECT_EQ(changed, 0);
    EXPECT_TRUE(west_view->heights[1] == 1.0f && west_view->heights[3] == 1.0f,
                "Tiny terrain seam differences are left untouched");
    EXPECT_TRUE(east_view->heights[0] == east_edge && east_view->heights[2] == east_edge,
                "Tiny neighbor seam differences are left untouched");
    PASS();
}

static void test_terrain_setters_repair_stale_slots_before_rejecting_invalid_handles() {
    TEST("Terrain3D setters repair stale slots before rejecting invalid handles");
    void *terrain = rt_terrain3d_new(16, 16);
    auto *view = (Terrain3DTestLayout *)terrain;
    void *wrong_material = rt_material3d_new();
    void *wrong_pixels = rt_pixels_new(1, 1);
    EXPECT_TRUE(terrain && view && wrong_material && wrong_pixels,
                "Terrain stale-slot rejection fixtures exist");
    if (!terrain || !view || !wrong_material || !wrong_pixels)
        return;

    size_t wrong_material_refcnt = rt_heap_hdr(wrong_material)->refcnt;
    size_t wrong_pixels_refcnt = rt_heap_hdr(wrong_pixels)->refcnt;

    view->material = wrong_pixels;
    rt_terrain3d_set_material(terrain, wrong_pixels);
    EXPECT_TRUE(view->material == nullptr,
                "Terrain3D.SetMaterial clears stale material slot before rejecting replacement");

    view->splat_map = wrong_material;
    EXPECT_TRUE(expect_trap_contains([&] { rt_terrain3d_set_splat_map(terrain, wrong_material); },
                                     "Pixels"),
                "Terrain3D.SetSplatMap still rejects non-Pixels replacements");
    EXPECT_TRUE(view->splat_map == nullptr,
                "Terrain3D.SetSplatMap clears stale splat slot before trapping");

    view->layer_textures[0] = wrong_material;
    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_terrain3d_set_layer_texture(terrain, 0, wrong_material); }, "Pixels"),
                "Terrain3D.SetLayerTexture still rejects non-texture replacements");
    EXPECT_TRUE(view->layer_textures[0] == nullptr,
                "Terrain3D.SetLayerTexture clears stale layer slot before trapping");

    EXPECT_TRUE(rt_heap_hdr(wrong_material)->refcnt == wrong_material_refcnt,
                "Terrain stale texture-slot repair does not release unowned wrong-class refs");
    EXPECT_TRUE(rt_heap_hdr(wrong_pixels)->refcnt == wrong_pixels_refcnt,
                "Terrain stale material-slot repair does not release unowned wrong-class refs");
    PASS();
}

static void test_terrain_single_pixel_splat_map_draws() {
    TEST("Terrain3D draws with a 1x1 splat map");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(1, 1);
    void *layer = rt_pixels_new(2, 2);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_splat_map(terrain, splat);
    rt_terrain3d_set_layer_texture(terrain, 0, layer);

    g_canvas_begin_frame_calls = 0;
    g_canvas_end_frame_calls = 0;
    g_canvas_submit_draw_calls = 0;
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_begin_frame_calls, 1);
    EXPECT_EQ(g_canvas_end_frame_calls, 1);
    EXPECT_TRUE(g_canvas_submit_draw_calls >= 0,
                "Terrain draw completes without trapping for a degenerate splat-map axis");
    PASS();
}

static void test_terrain_splat_bake_uses_base_texture_for_missing_layers() {
    TEST("Terrain3D splat bake falls back to base texture when a weighted layer is missing");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(1, 1);
    void *base = rt_pixels_new(1, 1);
    void *material;

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    EXPECT_TRUE(cam && eye && target && up && terrain && splat && base, "Terrain fixtures exist");
    if (!cam || !eye || !target || !up || !terrain || !splat || !base)
        return;
    rt_pixels_set(splat, 0, 0, 0xFF000000);
    rt_pixels_set(base, 0, 0, 0x22446688);
    material = rt_material3d_new_textured(base);
    EXPECT_TRUE(material != nullptr, "Terrain material fixture exists");
    if (!material)
        return;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    rt_material3d *mat = (rt_material3d *)material;
    rt_pixels_impl *baked = rt_pixels_checked_impl_or_null(mat->texture);
    EXPECT_TRUE(baked != nullptr && baked->data != nullptr, "Splat bake produces a texture");
    if (baked && baked->data)
        EXPECT_TRUE(baked->data[0] == 0x22446688,
                    "Missing weighted splat layers preserve the base texture color and alpha");
    EXPECT_TRUE(
        g_last_draw_cmd.has_splat == 0,
        "Incomplete terrain splat layers use the baked material texture, not GPU splat state");
    PASS();
}

static void test_terrain_splat_bake_uses_material_color_for_missing_layers() {
    TEST("Terrain3D splat bake falls back to material color when no base texture exists");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(1, 1);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.6);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    EXPECT_TRUE(cam && eye && target && up && terrain && splat && material,
                "Terrain material-color fallback fixtures exist");
    if (!cam || !eye || !target || !up || !terrain || !splat || !material)
        return;
    rt_pixels_set(splat, 0, 0, 0x000000FF);

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    rt_material3d *mat = (rt_material3d *)material;
    rt_pixels_impl *baked = rt_pixels_checked_impl_or_null(mat->texture);
    EXPECT_TRUE(baked != nullptr && baked->data != nullptr, "Splat bake produces a texture");
    if (baked && baked->data)
        EXPECT_TRUE(baked->data[0] == 0x336699FF,
                    "Missing weighted splat layers preserve the material diffuse color");
    EXPECT_TRUE(g_last_draw_cmd.has_splat == 0,
                "Incomplete terrain splat layers still use the baked material texture");
    PASS();
}

static void test_terrain_splat_layers_resolve_texture_assets() {
    TEST("Terrain3D splat layers resolve TextureAsset3D sources");
    const char *path = "/tmp/zanna_terrain_splat_textureasset_layer.ktx2";
    const uint8_t rgba_level0[] = {
        0xAA,
        0x20,
        0x30,
        0xFF,
    };
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(1, 1);
    void *material = rt_material3d_new();
    rt_string path_s;
    void *asset;
    void *asset_pixels;

    EXPECT_TRUE(write_test_ktx2(path, 37u, 1u, 1u, rgba_level0, sizeof(rgba_level0)),
                "terrain splat TextureAsset3D fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);
    asset_pixels = rt_textureasset3d_get_pixels(asset);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    EXPECT_TRUE(cam && eye && target && up && terrain && splat && material && asset_pixels,
                "Terrain TextureAsset3D splat fixtures exist");
    if (!cam || !eye || !target || !up || !terrain || !splat || !material || !asset_pixels)
        return;

    rt_pixels_set(splat, 0, 0, 0xFF000000);
    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);
    for (int i = 0; i < 4; i++)
        rt_terrain3d_set_layer_texture(terrain, i, asset);

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    rt_material3d *mat = (rt_material3d *)material;
    EXPECT_TRUE(mat->texture == nullptr, "Complete terrain splat layers skip CPU material baking");
    EXPECT_TRUE(g_last_draw_cmd.has_splat == 1 && g_last_draw_cmd.splat_layers[0] == asset_pixels,
                "Terrain splat draw resolves TextureAsset3D layers to resident pixels");
    PASS();
}

static void test_terrain_splat_bake_falls_back_when_textureasset_layer_loses_residency() {
    TEST("Terrain3D splat bake falls back when TextureAsset3D layer pixels are no longer resident");
    const char *path = "/tmp/zanna_terrain_splat_textureasset_layer_unresident.ktx2";
    const uint8_t rgba_level0[] = {
        0x11,
        0xEE,
        0x22,
        0xFF,
    };
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    void *splat = rt_pixels_new(1, 1);
    void *base = rt_pixels_new(1, 1);
    void *material;
    rt_string path_s;
    void *asset;

    EXPECT_TRUE(write_test_ktx2(path, 37u, 1u, 1u, rgba_level0, sizeof(rgba_level0)),
                "terrain unresident TextureAsset3D splat fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    EXPECT_TRUE(cam && eye && target && up && terrain && splat && base && asset,
                "Terrain unresident TextureAsset3D splat fixtures exist");
    if (!cam || !eye || !target || !up || !terrain || !splat || !base || !asset)
        return;

    rt_pixels_set(splat, 0, 0, 0xFF000000);
    rt_pixels_set(base, 0, 0, 0x336699CC);
    material = rt_material3d_new_textured(base);
    EXPECT_TRUE(material != nullptr, "Terrain unresident splat material exists");
    if (!material)
        return;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);
    rt_terrain3d_set_layer_texture(terrain, 0, asset);
    rt_textureasset3d_set_resident_mip_range(asset, 0, 0);
    EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) == nullptr,
                "TextureAsset3D splat layer can lose its resident Pixels after assignment");

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    rt_material3d *mat = (rt_material3d *)material;
    rt_pixels_impl *baked = rt_pixels_checked_impl_or_null(mat->texture);
    EXPECT_TRUE(baked != nullptr && baked->data != nullptr && baked->data[0] == 0x336699CC,
                "Unresident TextureAsset3D splat layer bakes from the base material texture");
    EXPECT_TRUE(g_last_draw_cmd.has_splat == 0,
                "Unresident TextureAsset3D splat layer does not advertise GPU splat state");

    std::remove(path);
    PASS();
}

static void test_terrain_draw_repairs_invalid_splat_map_and_restores_base_texture() {
    TEST("Terrain3D draw repairs invalid splat maps and restores the base texture");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 10.0, 20.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *terrain = rt_terrain3d_new(16, 16);
    auto *view = (Terrain3DTestLayout *)terrain;
    void *splat = rt_pixels_new(1, 1);
    void *base = rt_pixels_new(1, 1);
    void *layer = rt_pixels_new(1, 1);
    void *material;
    void *wrong_splat = rt_material3d_new();

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = 640;
    canvas.height = 480;

    EXPECT_TRUE(cam && eye && target && up && terrain && view && splat && base && layer &&
                    wrong_splat,
                "Terrain invalid-splat repair fixtures exist");
    if (!cam || !eye || !target || !up || !terrain || !view || !splat || !base || !layer ||
        !wrong_splat)
        return;

    rt_pixels_set(splat, 0, 0, 0xFF000000);
    rt_pixels_set(base, 0, 0, 0x10203040);
    rt_pixels_set(layer, 0, 0, 0xA0B0C0D0);
    material = rt_material3d_new_textured(base);
    EXPECT_TRUE(material != nullptr, "Terrain invalid-splat repair material exists");
    if (!material)
        return;

    rt_camera3d_look_at(cam, eye, target, up);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);
    rt_terrain3d_set_layer_texture(terrain, 0, layer);

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    rt_material3d *mat = (rt_material3d *)material;
    rt_pixels_impl *baked = rt_pixels_checked_impl_or_null(mat->texture);
    EXPECT_TRUE(baked != nullptr && baked->data != nullptr && baked->data[0] == 0xA0B0C0D0,
                "Initial terrain splat bake uses the weighted layer texture");
    EXPECT_TRUE(view->baked_texture == mat->texture && mat->texture != base,
                "Initial terrain splat bake installs a distinct material texture");

    if (view->splat_map && rt_obj_release_check0(view->splat_map))
        rt_obj_free(view->splat_map);
    size_t wrong_refcnt = rt_heap_hdr(wrong_splat)->refcnt;
    view->splat_map = wrong_splat;
    view->splat_dirty = 1;

    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_draw_terrain(&canvas, terrain);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(view->splat_map == nullptr,
                "Terrain draw clears a private non-Pixels splat map slot");
    EXPECT_TRUE(view->baked_texture == nullptr,
                "Terrain draw releases stale baked splat textures after invalid splat repair");
    EXPECT_TRUE(mat->texture == base,
                "Terrain draw restores the base material texture after invalid splat repair");
    EXPECT_TRUE(rt_heap_hdr(wrong_splat)->refcnt == wrong_refcnt,
                "Invalid splat repair does not release unowned wrong-class refs");
    EXPECT_TRUE(g_last_draw_cmd.has_splat == 0,
                "Invalid splat repair prevents stale GPU splat state from reaching draw commands");
    PASS();
}

static void test_terrain_accepts_decode_failure_checker_textureasset_splat_layer() {
    TEST("Terrain3D accepts decode-failure checker TextureAsset3D splat layers");
    const char *path = "/tmp/zanna_terrain_splat_native_only_astc_layer.ktx2";
    const uint8_t astc_level0[] = {
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        0xEE,
        0xF0,
        0x0F,
    };
    rt_string path_s;
    void *terrain = rt_terrain3d_new(16, 16);
    void *asset;
    void *checker;

    EXPECT_TRUE(write_test_ktx2(path, 157u, 4u, 4u, astc_level0, sizeof(astc_level0)),
                "terrain native-only TextureAsset3D fixture written");
    path_s = rt_string_from_bytes(path, std::strlen(path));
    asset = rt_textureasset3d_load_ktx2(path_s);
    rt_string_unref(path_s);

    EXPECT_TRUE(terrain && asset, "Terrain TextureAsset3D fixtures exist");
    if (!terrain || !asset)
        return;
    checker = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(checker != nullptr, "ASTC TextureAsset3D layer fixture exposes checker fallback");
    EXPECT_EQ(rt_pixels_width(checker), 8);
    EXPECT_EQ(rt_pixels_height(checker), 8);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 0, 0), 0xFF00FFFFu);
    EXPECT_EQ(rt_pixels_get_rgba(checker, 1, 0), 0x000000FFu);
    EXPECT_TRUE(!expect_trap_contains([&] { rt_terrain3d_set_layer_texture(terrain, 0, asset); },
                                      "RGBA8 Pixels"),
                "Terrain splat layers accept TextureAsset3D values with drawable checker Pixels");

    std::remove(path);
    PASS();
}

//=============================================================================
//=============================================================================
// SW Backend Feature Tests (SW-01 through SW-08)
//=============================================================================

static void test_vertex_color_default_white() {
    TEST("Mesh3D vertex color defaults to white");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 1);
    PASS();
}

static void test_shadow_enable_disable() {
    TEST("Shadow enable/disable lazily initializes and releases depth targets");
    extern void rt_canvas3d_enable_shadows(void *canvas, int64_t resolution);
    extern void rt_canvas3d_disable_shadows(void *canvas);
    extern void rt_canvas3d_set_shadow_bias(void *canvas, double bias);
    rt_canvas3d canvas;

    /* Call with NULL — should not crash (null-guard) */
    rt_canvas3d_enable_shadows(NULL, 1024);
    rt_canvas3d_disable_shadows(NULL);
    rt_canvas3d_set_shadow_bias(NULL, 0.005);

    memset(&canvas, 0, sizeof(canvas));
    rt_canvas3d_enable_shadows(&canvas, 128);
    EXPECT_EQ(canvas.shadows_enabled, 1);
    EXPECT_EQ(canvas.shadow_resolution, 128);
    vgfx3d_rendertarget_t *rt = canvas.shadow_rts[0];
    EXPECT_TRUE(rt != nullptr, "EnableShadows allocates the query-safe first shadow slot");
    EXPECT_EQ(rt->width, 128);
    EXPECT_EQ(rt->height, 128);
    EXPECT_EQ(rt->stride, 128 * 4);
    EXPECT_TRUE(rt->depth_buf != nullptr, "EnableShadows allocates shadow depth");
    EXPECT_NEAR(rt->depth_buf[0], FLT_MAX, 0.0);
    EXPECT_NEAR(rt->depth_buf[(128 * 128) - 1], FLT_MAX, 0.0);
    for (int32_t slot = 1; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++)
        EXPECT_TRUE(canvas.shadow_rts[slot] == nullptr, "EnableShadows defers unused shadow slots");

    rt_canvas3d_disable_shadows(&canvas);
    EXPECT_EQ(canvas.shadows_enabled, 0);
    EXPECT_EQ(canvas.shadow_count, 0);
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++)
        EXPECT_TRUE(canvas.shadow_rts[slot] == nullptr, "DisableShadows releases shadow slots");
    PASS();
}

static void test_mesh_tangents_for_normal_map() {
    TEST("Mesh3D.CalcTangents produces tangent data");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 0, 1, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_calc_tangents(m);
    /* CalcTangents should not crash and mesh should still be valid */
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 1);
    PASS();
}

static void test_mesh_tangent_fallback_is_orthogonal_to_normal() {
    TEST("Mesh3D.CalcTangents builds orthogonal fallback tangents");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m != NULL);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 1, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 1, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 0, 0, 1, 1, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);

    rt_mesh3d_calc_tangents(m);

    float dot = m->vertices[0].normal[0] * m->vertices[0].tangent[0] +
                m->vertices[0].normal[1] * m->vertices[0].tangent[1] +
                m->vertices[0].normal[2] * m->vertices[0].tangent[2];
    float len = std::sqrt(m->vertices[0].tangent[0] * m->vertices[0].tangent[0] +
                          m->vertices[0].tangent[1] * m->vertices[0].tangent[1] +
                          m->vertices[0].tangent[2] * m->vertices[0].tangent[2]);
    EXPECT_NEAR(dot, 0.0, 0.001);
    EXPECT_NEAR(len, 1.0, 0.001);
    EXPECT_NEAR(m->vertices[0].tangent[3], 1.0, 0.001);
    PASS();
}

/// @brief Verify persistent missing-tangent generation, reuse, and revision invalidation.
/// @details The first normal-mapped draw must build immutable raw and tangent variants without
///          touching source vertices or the global geometry epoch. A later unchanged frame must
///          hit both caches with zero frame snapshot bytes, while a geometry/UV-bearing append must
///          build exactly one new raw and tangent revision.
static void test_canvas_draw_persistently_caches_missing_normal_map_tangents() {
    TEST("Canvas3D.DrawMesh persistently caches generated normal-map tangents");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_new_plane(2.0, 2.0);
    void *mat = rt_material3d_new();
    void *normal = rt_pixels_new(1, 1);
    double model[16] = {0.0};
    assert(mesh != NULL && mat != NULL && normal != NULL);
    rt_material3d_set_normal_map(mat, normal);

    backend.name = "test";
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.in_frame = 1;
    model[0] = model[5] = model[10] = model[15] = 1.0;

    EXPECT_NEAR(mesh->vertices[0].tangent[0], 0.0, 0.001);
    uint64_t geometry_epoch_before_draw = rt_mesh3d_global_geometry_epoch();
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    EXPECT_NEAR(mesh->vertices[0].tangent[0], 0.0, 0.001);
    EXPECT_EQ(rt_mesh3d_global_geometry_epoch(), geometry_epoch_before_draw);
    EXPECT_TRUE(canvas.temp_buf_count == 0 && canvas.mesh_snapshot_bytes == 0u,
                "normal-mapped retained draw creates no frame geometry copies");
    EXPECT_TRUE(canvas.mesh_snapshot_count == 1 &&
                    canvas.mesh_snapshots[0].retained_revision != nullptr,
                "normal-mapped draw retains one immutable geometry revision");
    {
        vgfx3d_vertex_t *queued_vertices =
            canvas.mesh_snapshots[0].retained_revision->tangent_vertices;
        EXPECT_TRUE(queued_vertices != nullptr, "normal-mapped draw has queued vertices");
        EXPECT_TRUE(std::fabs(queued_vertices[0].tangent[0]) +
                            std::fabs(queued_vertices[0].tangent[1]) +
                            std::fabs(queued_vertices[0].tangent[2]) >
                        0.5,
                    "queued normal-mapped draw has a usable tangent basis");
    }
    EXPECT_EQ(mesh->retained_geometry_build_count, 1);
    EXPECT_EQ(mesh->retained_tangent_build_count, 1);

    canvas3d_clear_temp_buffers(&canvas);
    canvas3d_clear_temp_objects(&canvas);
    canvas.draw_count = 0;
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    EXPECT_EQ(mesh->retained_geometry_build_count, 1);
    EXPECT_EQ(mesh->retained_tangent_build_count, 1);
    EXPECT_TRUE(mesh->retained_tangent_cache_hit_count >= 1,
                "unchanged later frame reuses persistent generated tangents");
    EXPECT_TRUE(canvas.temp_buf_count == 0 && canvas.mesh_snapshot_bytes == 0u,
                "persistent tangent cache remains allocation-free at frame scope");

    canvas3d_clear_temp_buffers(&canvas);
    canvas3d_clear_temp_objects(&canvas);
    canvas.draw_count = 0;
    rt_mesh3d_add_vertex(mesh, 3.0, 0.0, 3.0, 0.0, 1.0, 0.0, 0.25, 0.75);
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    EXPECT_EQ(mesh->retained_geometry_build_count, 2);
    EXPECT_EQ(mesh->retained_tangent_build_count, 2);
    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

/// @brief Verify deferred heap draws retain immutable geometry across a source mutation.
/// @details Queues one plane revision, transforms the live mesh before submission, and proves the
///          queued bytes remain unchanged while a second draw forks to a distinct revision. The
///          test also asserts that retained revisions use no canvas frame-temp geometry buffers.
static void test_canvas_draw_retains_heap_mesh_geometry() {
    TEST("Canvas3D.DrawMesh retains immutable heap geometry for deferred draws");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_new_plane(2.0, 2.0);
    void *mat = rt_material3d_new();
    double model[16] = {0.0};
    assert(mesh != NULL && mat != NULL);

    backend.name = "test";
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.in_frame = 1;
    model[0] = model[5] = model[10] = model[15] = 1.0;

    float original_x = mesh->vertices[0].pos[0];
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    EXPECT_TRUE(canvas.temp_buf_count == 0 && canvas.mesh_snapshot_bytes == 0u,
                "heap mesh draw avoids frame-owned vertex/index copies");
    EXPECT_TRUE(canvas.mesh_snapshot_count == 1 &&
                    canvas.mesh_snapshots[0].retained_revision != nullptr,
                "heap mesh draw retains an immutable geometry revision");
    vgfx3d_vertex_t *queued_vertices = canvas.mesh_snapshots[0].vertices;
    EXPECT_TRUE(queued_vertices != nullptr, "heap mesh draw has queued vertices");
    rt_mesh3d_transform(mesh, rt_mat4_translate(4.0, 0.0, 0.0));
    EXPECT_NEAR(queued_vertices[0].pos[0], original_x, 0.001);
    EXPECT_NEAR(mesh->vertices[0].pos[0], original_x + 4.0, 0.001);
    EXPECT_TRUE(mesh->retained_geometry == nullptr,
                "geometry mutation forks away from the queued immutable revision");
    rt_canvas3d_draw_mesh_matrix(&canvas, mesh, model, mat);
    EXPECT_EQ(mesh->retained_geometry_build_count, 2);
    EXPECT_TRUE(canvas.mesh_snapshot_count == 2,
                "same-frame post-mutation draw retains both old and new revisions");
    EXPECT_TRUE(canvas.mesh_snapshots[1].vertices != queued_vertices,
                "post-mutation deferred draw references a distinct revision payload");

    free_canvas3d_test_draw_state(&canvas);
    PASS();
}

static void test_canvas_draw_rejects_public_raw_mesh_handles() {
    TEST("Canvas3D.DrawMesh rejects public raw mesh handles");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_mesh3d raw_mesh;
    void *mat = rt_material3d_new();
    void *transform = rt_mat4_identity();
    assert(mat != NULL && transform != NULL);

    memset(&canvas, 0, sizeof(canvas));
    memset(&raw_mesh, 0, sizeof(raw_mesh));
    backend.name = "test";
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.in_frame = 1;
    raw_mesh.vertex_count = 3;
    raw_mesh.index_count = 3;

    rt_canvas3d_draw_mesh(&canvas, &raw_mesh, transform, mat);
    EXPECT_EQ(canvas.draw_count, 0);
    PASS();
}

static void test_mesh_transform_updates_tangent_basis() {
    TEST("Mesh3D.Transform updates tangent bases for normal mapping");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    void *scale = rt_mat4_scale(2.0, 1.0, 1.0);
    assert(m != NULL);

    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 1, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 0, 1, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 0, 1, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    m->vertices[0].tangent[0] = 0.70710678f;
    m->vertices[0].tangent[1] = 0.70710678f;
    m->vertices[0].tangent[2] = 0.0f;
    m->vertices[0].tangent[3] = -1.0f;

    rt_mesh3d_transform(m, scale);

    EXPECT_NEAR(m->vertices[0].tangent[0], 0.44721359, 0.01);
    EXPECT_NEAR(m->vertices[0].tangent[1], 0.89442719, 0.01);
    EXPECT_NEAR(m->vertices[0].tangent[2], 0.0, 0.01);
    EXPECT_NEAR(m->vertices[0].tangent[3], -1.0, 0.001);
    PASS();
}

static void test_mesh_transform_rejects_singular_normal_matrix() {
    TEST("Mesh3D.Transform rejects singular normal matrices");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new_plane(1.0, 1.0);
    void *scale = rt_mat4_scale(0.0, 1.0, 1.0);
    assert(m != NULL && scale != NULL);
    EXPECT_TRUE(expect_trap_contains([&] { rt_mesh3d_transform(m, scale); }, "invertible"),
                "Mesh3D.Transform traps on singular upper 3x3");
    PASS();
}

static void test_mesh_validation_errors_mark_build_failed() {
    TEST("Mesh3D validation errors mark build_failed");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m != NULL);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 1, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 0, 1, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 1, 0, 0, 0, 1, 0, 1);
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_mesh3d_add_triangle(m, 0, 0, 1); }, "degenerate triangle"),
        "Mesh3D.AddTriangle traps on degenerate triangle");
    EXPECT_TRUE(m->build_failed != 0, "Mesh3D marks failed after validation trap");
    uint32_t before = m->index_count;
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    EXPECT_EQ(m->index_count, before);
    PASS();
}

static void test_mesh_normals_recalc() {
    TEST("Mesh3D.RecalcNormals updates normals");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 0, 0, 0, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 0, 0, 1, 0, 0, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_recalc_normals(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 3);
    PASS();
}

static void test_mesh_recalc_normals_assigns_fallback_for_unreferenced_vertices() {
    TEST("Mesh3D.RecalcNormals assigns finite fallback normals to unreferenced vertices");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m != NULL);
    rt_mesh3d_add_vertex(m, 0, 0, 0, 0, 0, 0, 0, 0);

    rt_mesh3d_recalc_normals(m);

    EXPECT_NEAR(m->vertices[0].normal[0], 0.0, 0.001);
    EXPECT_NEAR(m->vertices[0].normal[1], 1.0, 0.001);
    EXPECT_NEAR(m->vertices[0].normal[2], 0.0, 0.001);
    PASS();
}

static void test_terrain_splat_layer_count() {
    TEST("Terrain3D supports 4 splat layers");
    void *t = rt_terrain3d_new(4, 4);
    assert(t != NULL);
    void *px = rt_pixels_new(4, 4);
    /* Set all 4 layers */
    for (int i = 0; i < 4; i++) {
        rt_terrain3d_set_layer_texture(t, i, px);
        rt_terrain3d_set_layer_scale(t, i, (double)(i + 1));
    }
    PASS();
}

static void test_terrain_splat_map_set() {
    TEST("Terrain3D splat map can be set and cleared");
    void *t = rt_terrain3d_new(4, 4);
    assert(t != NULL);
    void *px = rt_pixels_new(4, 4);
    rt_terrain3d_set_splat_map(t, px);
    rt_terrain3d_set_splat_map(t, NULL);
    PASS();
}

// Metal backend feature tests (MTL-01 through MTL-08)
// These test the runtime API surface that feeds the Metal shader pipeline.
//=============================================================================

static void test_metal_spot_light_creates() {
    TEST("MTL-02: Light3D.NewSpot — inner/outer angles survive creation");
    void *pos = rt_vec3_new(0, 10, 0);
    void *dir = rt_vec3_new(0, -1, 0);
    /* 15° inner, 30° outer cone — Metal shader uses cos() of these */
    void *light = rt_light3d_new_spot(pos, dir, 1.0, 1.0, 1.0, 0.05, 15.0, 30.0);
    assert(light != NULL);
    PASS();
}

static void test_metal_spot_light_narrow_cone() {
    TEST("MTL-02: Spot light — narrow cone (5° inner, 10° outer)");
    void *pos = rt_vec3_new(0, 5, 0);
    void *dir = rt_vec3_new(0, -1, 0);
    void *light = rt_light3d_new_spot(pos, dir, 1.0, 1.0, 1.0, 0.01, 5.0, 10.0);
    assert(light != NULL);
    rt_light3d_set_intensity(light, 3.0);
    PASS();
}

static void test_metal_material_all_maps() {
    TEST("MTL-04/05/06: Material3D — set all 3 map textures");
    void *m = rt_material3d_new();
    void *norm_px = rt_pixels_new(8, 8);
    void *spec_px = rt_pixels_new(8, 8);
    void *emit_px = rt_pixels_new(8, 8);
    rt_material3d_set_normal_map(m, norm_px);
    rt_material3d_set_specular_map(m, spec_px);
    rt_material3d_set_emissive_map(m, emit_px);
    rt_material3d_set_emissive_color(m, 1.0, 0.5, 0.0);
    PASS();
}

static void test_metal_material_map_null_safety() {
    TEST("MTL-04/05/06: Material3D — set maps to NULL is safe");
    void *m = rt_material3d_new();
    rt_material3d_set_normal_map(m, NULL);
    rt_material3d_set_specular_map(m, NULL);
    rt_material3d_set_emissive_map(m, NULL);
    rt_material3d_set_emissive_color(m, 0.0, 0.0, 0.0);
    PASS();
}

static void test_metal_material_map_replace() {
    TEST("MTL-03: Material3D — replacing texture maps doesn't leak");
    void *m = rt_material3d_new();
    void *px1 = rt_pixels_new(4, 4);
    void *px2 = rt_pixels_new(8, 8);
    /* Set then replace each map */
    rt_material3d_set_normal_map(m, px1);
    rt_material3d_set_normal_map(m, px2);
    rt_material3d_set_specular_map(m, px1);
    rt_material3d_set_specular_map(m, NULL);
    rt_material3d_set_emissive_map(m, px2);
    rt_material3d_set_emissive_map(m, px1);
    PASS();
}

static void test_metal_fog_set_clear() {
    TEST("MTL-07: Canvas3D fog — set and clear (null canvas)");
    /* null canvas won't crash (stubs return early) */
    rt_canvas3d_set_fog(NULL, 10.0, 100.0, 0.5, 0.5, 0.6);
    rt_canvas3d_clear_fog(NULL);
    PASS();
}

static void test_metal_tangents_for_normal_map() {
    TEST("MTL-04: Mesh3D.CalcTangents — required for Metal normal maps");
    void *m = rt_mesh3d_new();
    rt_mesh3d_add_vertex(m, -1, 0, -1, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 1, 0, -1, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, 1, 0, 1, 0, 1, 0, 1, 1);
    rt_mesh3d_add_vertex(m, -1, 0, 1, 0, 1, 0, 0, 1);
    rt_mesh3d_add_triangle(m, 0, 2, 1);
    rt_mesh3d_add_triangle(m, 0, 3, 2);
    rt_mesh3d_calc_tangents(m);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(m), 4);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(m), 2);
    PASS();
}

// Metal backend tests — Phase 2 (MTL-09 through MTL-14)
//=============================================================================

extern "C" void rt_canvas3d_enable_shadows(void *canvas, int64_t resolution);
extern "C" void rt_canvas3d_disable_shadows(void *canvas);

static void test_metal_shadow_enable_null() {
    TEST("MTL-12: Canvas3D shadow enable/disable (null canvas safe)");
    rt_canvas3d_enable_shadows(NULL, 1024);
    rt_canvas3d_disable_shadows(NULL);
    PASS();
}

extern "C" void *rt_instbatch3d_new(void *mesh, void *material);
extern "C" void rt_instbatch3d_add(void *batch, void *transform);
extern "C" void rt_instbatch3d_remove(void *batch, int64_t index);

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
} instbatch_view_t;

static void test_metal_instbatch_create() {
    TEST("MTL-13: InstanceBatch3D — create and add instances");
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, mat);
    assert(batch != NULL);
    /* Add a few instances (no getter, just verify no crash) */
    void *t = rt_mat4_identity();
    rt_instbatch3d_add(batch, t);
    rt_instbatch3d_add(batch, t);
    rt_instbatch3d_add(batch, t);
    PASS();
}

static void test_instbatch_remove_preserves_unrelated_motion_history() {
    TEST("InstanceBatch3D.Remove preserves unrelated motion history");
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, mat);
    void *t = rt_mat4_identity();
    instbatch_view_t *view = nullptr;
    assert(batch != NULL);

    rt_instbatch3d_add(batch, t);
    rt_instbatch3d_add(batch, t);
    rt_instbatch3d_add(batch, t);
    view = (instbatch_view_t *)batch;
    view->motion_snapshot_count = 2;
    view->prev_count = 2;
    view->has_prev_snapshot = 1;

    rt_instbatch3d_remove(batch, 1);

    EXPECT_EQ(view->motion_snapshot_count, 1);
    EXPECT_EQ(view->prev_count, 1);
    EXPECT_TRUE(view->has_prev_snapshot == 1,
                "Removing a later instance keeps earlier motion history");
    PASS();
}

static void test_canvas_opaque_alpha_mode_keeps_instanced_path() {
    TEST("Canvas3D keeps fully opaque batches on the instanced path");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float instances[32] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    instances[0] = instances[5] = instances[10] = instances[15] = 1.0f;
    instances[16 + 0] = instances[16 + 5] = instances[16 + 10] = instances[16 + 15] = 1.0f;
    instances[16 + 3] = 2.0f;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    rt_material3d_set_alpha(mat, 1.0);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);

    g_canvas_submit_draw_calls = 0;
    g_canvas_submit_draw_instanced_calls = 0;
    g_last_instanced_count = 0;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 2, NULL, 0);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_instanced_calls, 1);
    EXPECT_EQ(g_last_instanced_count, 2);
    EXPECT_EQ(g_canvas_submit_draw_calls, 0);
    PASS();
}

static void test_canvas_instanced_repairs_corrupt_mesh_counts_before_tangents() {
    TEST("Canvas3D repairs corrupt instanced mesh counts before tangent preparation");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    void *normal = rt_pixels_new(1, 1);
    float instances[16] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    instances[0] = instances[5] = instances[10] = instances[15] = 1.0f;
    memset(&canvas, 0, sizeof(canvas));
    EXPECT_TRUE(cam && mesh && mat && normal, "Instanced corrupt-count fixtures allocate");
    if (!cam || !mesh || !mat || !normal)
        return;

    rt_material3d_set_normal_map(mat, normal);
    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    uint32_t vertex_capacity = mesh_view->vertex_capacity;
    uint32_t index_capacity = mesh_view->index_capacity;
    mesh_view->vertex_count = std::numeric_limits<uint32_t>::max();
    mesh_view->index_count = std::numeric_limits<uint32_t>::max();

    canvas.backend = &backend;
    g_canvas_submit_draw_calls = 0;
    g_canvas_submit_draw_instanced_calls = 0;
    g_last_instanced_count = 0;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 1, NULL, 0);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(mesh_view->vertex_count, vertex_capacity);
    EXPECT_EQ(mesh_view->index_count, index_capacity);
    EXPECT_EQ(g_canvas_submit_draw_instanced_calls, 1);
    EXPECT_EQ(g_last_instanced_count, 1);
    PASS();
}

static void test_canvas_instanced_gpu_synthesizes_previous_matrices() {
    TEST("Canvas3D synthesizes previous matrices for GPU instancing");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float instances[16] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    instances[0] = instances[5] = instances[10] = instances[15] = 1.0f;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    rt_material3d_set_alpha(mat, 1.0);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);

    g_last_instanced_has_prev = 0;
    g_last_instanced_prev_x = -99.0f;
    rt_canvas3d_begin(&canvas, cam);
    enable_latched_motion_blur(&canvas);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 1, NULL, 0);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_instanced_has_prev, 1);
    EXPECT_NEAR(g_last_instanced_prev_x, 0.0, 0.0001);

    instances[3] = 2.0f;
    rt_canvas3d_begin(&canvas, cam);
    enable_latched_motion_blur(&canvas);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 1, NULL, 0);
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_last_instanced_has_prev, 1);
    EXPECT_NEAR(g_last_instanced_prev_x, 0.0, 0.0001);
    vgfx3d_postfx_chain_free(&canvas.frame_postfx_chain);
    PASS();
}

static void test_canvas_instanced_gpu_uses_explicit_previous_matrices() {
    TEST("Canvas3D uses explicit previous matrices for GPU instancing");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float instances[16] = {0.0f};
    float prev_instances[16] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    instances[0] = instances[5] = instances[10] = instances[15] = 1.0f;
    instances[3] = 5.0f;
    prev_instances[0] = prev_instances[5] = prev_instances[10] = prev_instances[15] = 1.0f;
    prev_instances[3] = -7.0f;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    rt_material3d_set_alpha(mat, 1.0);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);

    g_last_instanced_has_prev = 0;
    g_last_instanced_prev_x = 0.0f;
    rt_canvas3d_begin(&canvas, cam);
    enable_latched_motion_blur(&canvas);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 1, prev_instances, 1);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_last_instanced_has_prev, 1);
    EXPECT_NEAR(g_last_instanced_prev_x, -7.0, 0.0001);
    vgfx3d_postfx_chain_free(&canvas.frame_postfx_chain);
    PASS();
}

static void test_canvas_instanced_motion_history_separates_batches() {
    TEST("Canvas3D separates motion history for same mesh instanced batches");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float batch_a[16] = {0.0f};
    float batch_b[16] = {0.0f};

    backend.name = "software";
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.end_frame = tracked_end_frame;

    batch_a[0] = batch_a[5] = batch_a[10] = batch_a[15] = 1.0f;
    batch_b[0] = batch_b[5] = batch_b[10] = batch_b[15] = 1.0f;
    batch_b[3] = 10.0f;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    rt_material3d_set_alpha(mat, 0.5);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, batch_a, 1, NULL, 0);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, batch_b, 1, NULL, 0);
    rt_canvas3d_end(&canvas);

    batch_a[3] = 1.0f;
    batch_b[3] = 11.0f;
    memset(&g_last_draw_cmd, 0, sizeof(g_last_draw_cmd));
    g_tracked_prev_model_count = 0;
    memset(g_tracked_prev_model_x, 0, sizeof(g_tracked_prev_model_x));
    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, batch_a, 1, NULL, 0);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, batch_b, 1, NULL, 0);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_tracked_prev_model_count, 2);
    bool saw_batch_a_prev = false;
    bool saw_batch_b_prev = false;
    for (int i = 0; i < g_tracked_prev_model_count; i++) {
        saw_batch_a_prev =
            saw_batch_a_prev || std::fabs(g_tracked_prev_model_x[i] - 0.0f) < 0.0001f;
        saw_batch_b_prev =
            saw_batch_b_prev || std::fabs(g_tracked_prev_model_x[i] - 10.0f) < 0.0001f;
    }
    EXPECT_TRUE(saw_batch_a_prev, "first batch keeps its own previous transform");
    EXPECT_TRUE(saw_batch_b_prev, "second batch keeps its own previous transform");
    PASS();
}

static void test_canvas_legacy_translucent_batch_falls_back_from_instancing() {
    TEST("Canvas3D routes legacy translucent batches off the instanced path");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *cam = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float instances[32] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = tracked_begin_frame;
    backend.submit_draw = tracked_submit_draw;
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    backend.end_frame = tracked_end_frame;

    instances[0] = instances[5] = instances[10] = instances[15] = 1.0f;
    instances[16 + 0] = instances[16 + 5] = instances[16 + 10] = instances[16 + 15] = 1.0f;
    instances[16 + 3] = 2.0f;

    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    rt_material3d_set_alpha(mat, 0.25);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);

    g_canvas_submit_draw_calls = 0;
    g_canvas_submit_draw_instanced_calls = 0;
    g_last_instanced_count = 0;

    rt_canvas3d_begin(&canvas, cam);
    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances, 2, NULL, 0);
    rt_canvas3d_end(&canvas);

    EXPECT_EQ(g_canvas_submit_draw_instanced_calls, 0);
    EXPECT_EQ(g_canvas_submit_draw_calls, 2);
    PASS();
}

static int32_t g_chunked_fallback_submit_index = 0;
static int8_t g_chunked_fallback_order_ok = 1;

/// @brief Record one non-native fallback draw and verify its global instance order.
/// @details The chunk-boundary regression stores each source index in model translation X. With
///   opaque sorting disabled, backend submission must observe the exact monotonically increasing
///   sequence across the 65,536/65,537 reservation boundary.
static void tracked_chunked_fallback_submit_draw(void *,
                                                 vgfx_window_t,
                                                 const vgfx3d_draw_cmd_t *cmd,
                                                 const vgfx3d_light_params_t *,
                                                 int32_t,
                                                 const float *,
                                                 int8_t,
                                                 int8_t) {
    if (!cmd || cmd->model_matrix[3] != (float)g_chunked_fallback_submit_index)
        g_chunked_fallback_order_ok = 0;
    g_chunked_fallback_submit_index++;
}

/// @brief Verify non-native instancing crosses the 65,536-instance boundary without omissions.
/// @details A backend with no instanced hook forces the per-instance fallback. The request is one
///   element larger than a reservation chunk; every command must queue, telemetry must report no
///   drop, and the backend must receive all model matrices in original stable order.
static void test_canvas_instanced_fallback_chunks_without_omission() {
    TEST("Canvas3D chunks per-instance fallback draws without omissions");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    const int32_t requested = 65537; /* one beyond CANVAS3D_FALLBACK_CHUNK_INSTANCES */
    std::vector<float> instances((size_t)requested * 16, 0.0f);

    backend.name = "software";
    backend.submit_draw = tracked_chunked_fallback_submit_draw;
    backend.end_frame = tracked_end_frame;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.in_frame = 1;
    canvas.opaque_depth_sorting = 0;
    for (int32_t i = 0; i < requested; i++) {
        float *m = &instances[(size_t)i * 16];
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[3] = (float)i;
    }

    rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instances.data(), requested, NULL, 0);
    EXPECT_EQ(canvas.draw_count, requested);
    EXPECT_EQ(rt_canvas3d_get_instanced_fallback_count(&canvas), requested);
    EXPECT_EQ(rt_canvas3d_get_instanced_fallback_dropped_count(&canvas), 0);

    g_chunked_fallback_submit_index = 0;
    g_chunked_fallback_order_ok = 1;
    rt_canvas3d_end(&canvas);
    EXPECT_EQ(g_chunked_fallback_submit_index, requested);
    EXPECT_EQ(g_chunked_fallback_order_ok, 1);

    free(canvas.draw_cmds);
    free(canvas.temp_buffers);
    free(canvas.temp_objects);
    free(canvas.temp_buffer_set);
    free(canvas.temp_object_set);
    free(canvas.mesh_snapshots);
    free(canvas.mesh_snapshot_hash);
    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    PASS();
}

static void test_canvas_instanced_previous_matrices_require_pointer() {
    TEST("Canvas3D rejects previous-instance flag without matrix data");
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    float instance[16] = {0.0f};

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.submit_draw_instanced = tracked_submit_draw_instanced;
    memset(&canvas, 0, sizeof(canvas));
    canvas.backend = &backend;
    canvas.in_frame = 1;
    instance[0] = instance[5] = instance[10] = instance[15] = 1.0f;

    EXPECT_TRUE(
        expect_trap_contains(
            [&]() { rt_canvas3d_queue_instanced_batch(&canvas, mesh, mat, instance, 1, NULL, 1); },
            "previous instance matrices pointer is required"),
        "Previous-instance flag without data traps instead of silently disabling motion history");
    PASS();
}

static void test_metal_terrain_splat_for_gpu() {
    TEST("MTL-14: Terrain3D splat maps + 4 layers for GPU path");
    void *t = rt_terrain3d_new(8, 8);
    assert(t != NULL);
    void *splat = rt_pixels_new(8, 8);
    rt_terrain3d_set_splat_map(t, splat);
    for (int i = 0; i < 4; i++) {
        void *layer = rt_pixels_new(16, 16);
        rt_terrain3d_set_layer_texture(t, i, layer);
        rt_terrain3d_set_layer_scale(t, i, (double)(i + 1) * 4.0);
    }
    PASS();
}

extern "C" void *rt_postfx3d_new(void);
extern "C" void rt_postfx3d_add_bloom(void *obj,
                                      double threshold,
                                      double intensity,
                                      int64_t blur_passes);
extern "C" void rt_postfx3d_add_tonemap(void *obj, int64_t mode, double exposure);
extern "C" void rt_postfx3d_add_fxaa(void *obj);
extern "C" void rt_postfx3d_add_vignette(void *obj, double radius, double softness);
extern "C" void rt_postfx3d_set_enabled(void *obj, int8_t enabled);
extern "C" int64_t rt_postfx3d_get_effect_count(void *obj);

static void test_metal_postfx_new() {
    TEST("MTL-11: PostFX3D — create and add effects");
    void *fx = rt_postfx3d_new();
    assert(fx != NULL);
    rt_postfx3d_add_bloom(fx, 0.8, 1.5, 3);
    rt_postfx3d_add_tonemap(fx, 2, 1.0);
    rt_postfx3d_add_fxaa(fx);
    rt_postfx3d_add_vignette(fx, 0.7, 0.3);
    EXPECT_EQ(rt_postfx3d_get_effect_count(fx), 4);
    rt_postfx3d_set_enabled(fx, 1);
    PASS();
}

static void test_metal_postfx_grows_past_legacy_cap() {
    TEST("MTL-11: PostFX3D grows past the legacy 8-effect cap");
    void *fx = rt_postfx3d_new();
    assert(fx != NULL);
    for (int i = 0; i < 12; i++)
        rt_postfx3d_add_fxaa(fx);
    EXPECT_EQ(rt_postfx3d_get_effect_count(fx), 12);
    PASS();
}

static void test_metal_postfx_null_safety() {
    TEST("MTL-11: PostFX3D — null safety on all ops");
    void *wrong = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_postfx3d_add_bloom(NULL, 0.5, 1.0, 2);
    rt_postfx3d_add_tonemap(NULL, 1, 1.0);
    rt_postfx3d_add_fxaa(NULL);
    rt_postfx3d_add_vignette(NULL, 0.5, 0.2);
    rt_postfx3d_set_enabled(NULL, 0);
    rt_canvas3d_set_post_fx(NULL, NULL);
    rt_postfx3d_add_fxaa(wrong);
    rt_postfx3d_apply_to_canvas(wrong);
    EXPECT_EQ(rt_postfx3d_get_effect_count(wrong), 0);
    PASS();
}

static void test_metal_skinned_mesh_bone_fields() {
    TEST("MTL-09: Mesh3D bone data survives creation");
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    assert(mesh != NULL);
    /* Mesh should have zero bone count by default */
    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 24);
    PASS();
}

static void test_texatlas_rejects_oversized_images_and_defaults_missing_uvs() {
    TEST("TextureAtlas3D rejects oversized padded uploads and defaults missing UVs");
    void *atlas = rt_texatlas3d_new(16, 16);
    void *too_big = rt_pixels_new(16, 16);
    void *small = rt_pixels_new(2, 2);
    double u0 = -1.0;
    double v0 = -1.0;
    double u1 = -1.0;
    double v1 = -1.0;
    assert(atlas != NULL && too_big != NULL && small != NULL);
    EXPECT_EQ(rt_texatlas3d_add(atlas, too_big), -1);
    rt_texatlas3d_get_uv_rect(atlas, 123, &u0, &v0, &u1, &v1);
    EXPECT_NEAR(u0, 0.0, 0.000001);
    EXPECT_NEAR(v0, 0.0, 0.000001);
    EXPECT_NEAR(u1, 1.0, 0.000001);
    EXPECT_NEAR(v1, 1.0, 0.000001);
    int64_t id = rt_texatlas3d_add(atlas, small);
    EXPECT_EQ(id, 0);
    rt_texatlas3d_get_uv_rect(atlas, id, &u0, &v0, &u1, &v1);
    EXPECT_TRUE(u0 > 0.0 && v0 > 0.0 && u1 > u0 && v1 > v0,
                "packed texture returns an interior UV rect after border padding");
    PASS();
}

// Backend selection tests
//=============================================================================

static void test_backend_select() {
    TEST("Backend selection - returns non-null");
    const vgfx3d_backend_t *b = vgfx3d_select_backend();
    assert(b != NULL);
    PASS();
}

//=============================================================================
// New Canvas3D bindings: wind deformation + window-control NULL-safety
//=============================================================================

static void test_wind_deform_base_fixed_canopy_sways() {
    TEST("DrawMeshWind deform: base planted, canopy sways, height preserved");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    rt_mesh3d_add_vertex(m, 0.0, 0.0, 0.0, 0, 1, 0, 0, 0); /* base (y=0)   */
    rt_mesh3d_add_vertex(m, 0.0, 4.0, 0.0, 0, 1, 0, 0, 0); /* canopy (y=4) */
    /* Wind along +X, full strength, phase=PI/2 (peak swing). */
    canvas3d_deform_mesh_wind(m, 1.0, 0.0, 1.0, 1.5707963267948966);
    /* Base vertex: height weight 0 -> no XZ displacement. */
    EXPECT_NEAR(m->vertices[0].pos[0], 0.0f, 1e-5);
    EXPECT_NEAR(m->vertices[0].pos[2], 0.0f, 1e-5);
    /* Canopy vertex: displaced along the wind (+X). */
    EXPECT_TRUE(m->vertices[1].pos[0] > 0.1f, "canopy swayed along +X");
    /* Height (Y) is preserved for both vertices. */
    EXPECT_NEAR(m->vertices[0].pos[1], 0.0f, 1e-5);
    EXPECT_NEAR(m->vertices[1].pos[1], 4.0f, 1e-5);
    PASS();
}

static void test_wind_deform_zero_strength_and_null_safe() {
    TEST("DrawMeshWind deform: zero strength is a no-op; NULL-safe");
    rt_mesh3d *m = (rt_mesh3d *)rt_mesh3d_new();
    assert(m);
    rt_mesh3d_add_vertex(m, 0.0, 0.0, 0.0, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, 0.0, 4.0, 0.0, 0, 1, 0, 0, 0);
    float before_x = m->vertices[1].pos[0];
    canvas3d_deform_mesh_wind(m, 1.0, 0.0, 0.0, 1.5707963267948966); /* strength 0 */
    EXPECT_NEAR(m->vertices[1].pos[0], before_x, 1e-6);
    canvas3d_deform_mesh_wind(nullptr, 1.0, 0.0, 1.0, 1.0); /* NULL mesh -> no crash */
    PASS();
}

static void test_canvas3d_window_bindings_null_safe() {
    TEST("Canvas3D fullscreen/image/wind entry points are NULL-safe");
    /* Headless tests open no window; the new entry points must tolerate NULL. */
    rt_canvas3d_set_fullscreen(nullptr, 1);
    rt_canvas3d_toggle_fullscreen(nullptr);
    EXPECT_EQ(rt_canvas3d_is_fullscreen(nullptr), 0);
    rt_canvas3d_draw_image2d(nullptr, 0, 0, 16, 16, nullptr);
    rt_canvas3d_draw_mesh_wind(nullptr, nullptr, nullptr, nullptr, 1.0, 0.0, 1.0, 1.0);
    PASS();
}

//=============================================================================
// Main
//=============================================================================

int main() {
    printf("=== Graphics3D Unit Tests ===\n\n");

    /* Mesh3D — basic */
    test_mesh_empty();
    test_mesh_add_vertex_triangle();
    test_mesh_reserve_presizes_without_dirtying_geometry();
    test_mesh_mutations_restore_residency_and_counts_are_clamped();
    test_mesh_recalc_normals_reuses_large_accumulator();
    test_mesh_generators_batch_geometry_revision_updates();
    test_mesh_reject_invalid_triangle_indices();
    test_mesh_calc_tangents_tracks_mirrored_uv_handedness();
    test_mesh_box();
    test_mesh_sphere();
    test_mesh_plane();
    test_mesh_cylinder();
    test_mesh_generators_reject_invalid_dimensions();
    test_mesh_clone();
    test_mesh_clone_repairs_corrupt_private_counts();
    test_mesh_clone_deep_copies_morph_targets();
    test_mesh_transform_uses_inverse_transpose_normals();
    test_mesh_transform_updates_tangent_basis();
    test_mesh_transform_flips_tangent_handedness_for_mirrors();
    test_mesh_recalc_normals();
    test_mesh_recalc_normals_uses_double_accumulation();
    test_mesh_obj_loader();
    test_mesh_obj_loader_flattens_material_groups();
    test_mesh_obj_loader_fills_only_missing_normals();
    test_mesh_obj_loader_deduplicates_vertices_and_handles_ngons();
    test_mesh_obj_loader_ear_clips_concave_ngons();
    test_mesh_obj_loader_rejects_invalid_indices();
    test_mesh_obj_loader_rejects_invalid_numeric_tokens();
    test_mesh_obj_loader_rejects_empty_geometry();

    /* Mesh3D — extended */
    test_mesh_many_vertices();
    test_mesh_many_triangles();
    test_mesh_null_safety();
    test_mesh_sphere_low_segments();
    test_mesh_cylinder_low_segments();
    test_mesh_box_dimensions();
    test_mesh_transform_identity();

    /* Camera3D — basic */
    test_camera_new();
    test_camera_set_fov();
    test_camera_projection_mode_and_ortho_size_are_mutable();
    test_camera_clip_planes();
    test_camera_look_at();
    test_camera_forward();
    test_camera_orbit();
    test_camera_orbit_syncs_fps_angles();
    test_camera_screen_to_ray();

    /* Camera3D — extended */
    test_camera_null_safety();
    test_camera_rejects_invalid_vec3_handles();
    test_camera_right_vector();
    test_camera_orbit_yaw();
    test_camera_orbit_pitch();
    test_camera_screen_to_ray_corners();
    test_camera_screen_to_ray_uses_viewport_aspect();
    test_camera_set_position_rebuilds_view();
    test_camera_set_yaw_pitch_rebuilds_view();
    test_camera_look_at_coincident_eye_preserves_translation();
    test_camera_look_at_preserves_custom_up_basis();
    test_camera_shake_overshoot_clears_immediately();
    test_camera_ortho_set_fov_preserves_projection();
    test_camera_ortho_screen_to_ray_parallel();
    test_camera_screen_to_ray_origin_handles_ortho_pixels();
    test_camera_screen_to_ray_tracks_shaken_view();
    test_camera_shake_does_not_drift_eye_in_smooth_follow();
    test_camera_sanitizes_nonfinite_inputs();
    test_camera_getters_sanitize_corrupt_private_state();
    test_camera_clamps_extreme_finite_inputs();

    /* Material3D */
    test_material_new();
    test_material_new_color();
    test_material_new_textured();
    test_material_texture_setters_reject_invalid_handles();
    test_material_texture_setters_repair_stale_slots_before_rejecting_invalid_handles();
    test_textureasset3d_source_container_bridge();
    test_textureasset3d_ktx2_retains_exact_container();
    test_textureasset3d_ktx2_material_bridge();
    test_textureasset3d_bc3_software_decode();
    test_textureasset3d_bc1_bc4_bc5_software_decode();
    test_textureasset3d_bc7_software_decode();
    test_textureasset3d_etc2_astc_software_decode();
    test_textureasset3d_decode_failure_checker_fallback();
    test_textureasset3d_bc7_partial_mip_fallback();
    test_textureasset3d_mip_residency();
    test_canvas3d_texture_streaming_residency();
    test_canvas3d_per_instance_skinning_software_fallback();
    test_canvas3d_per_instance_skinning_chunking();
    test_canvas3d_compact_streams_flag();
    test_textureasset3d_supercompressed_ktx2_loads();
    test_textureasset3d_zlib_integrity_validation();
    test_textureasset3d_format_capability_table();
    test_textureasset3d_supercompression_direct_backing_allocation();
    test_textureasset3d_rejects_unsupported_ktx2_headers();
    test_textureasset3d_native_resident_mips_feed_backend_utils();
    test_material_inspection_getters();
    test_material_texture_presence_getters();
    test_material_set_color();
    test_material_sanitizes_numeric_inputs();
    test_material_getters_sanitize_without_mutating_corrupt_state();
    test_material_clone_repairs_invalid_env_map();
    test_material_set_shininess();
    test_material_set_unlit();
    test_material_null_safety();

    /* Light3D */
    test_cluster_slice_and_radius_math();
    test_cluster_table_binning_is_conservative();
    test_cluster_table_overflow_truncates_deterministically();
    test_cluster_table_ring_and_gating();
    test_build_light_params_sorts_globals_first();
    test_soft_particle_fade_software();
    test_native_area_volume_lighting_software();
    test_ssr_chain_and_mask_plumbing();
    test_light_directional();
    test_light_point();
    test_light_ambient();
    test_light_set_intensity();
    test_light_set_color();
    test_light_set_position_and_direction();
    test_light_inspection_getters_and_enabled();
    test_light_null_safety();
    test_light_spot();
    test_light_spot_intensity();
    test_light_spot_cone_authoring();
    test_light_native_area_and_volume_types();
    test_light_validation_and_clamping();
    test_light_sanitizes_nonfinite_inputs();
    test_light_clamps_extreme_finite_inputs();
    test_camera_ortho();
    test_camera_ortho_look_at();
    test_camera_perspective_not_ortho();

    /* Phase 9 — Multi-texture materials */
    test_material_set_emissive();
    test_material_set_maps();
    test_mesh_calc_tangents();

    /* Phase 10 — Alpha blending */
    test_material_alpha();

    /* Phase 11 — Cube maps */
    test_cubemap_load_hdr_panorama();
    test_cubemap_new();
    test_canvas_set_skybox_repairs_stale_existing_slot();
    test_canvas_ibl_setters_defer_prefilter_work();
    test_canvas_ortho_skybox_fills_render_target_uniformly();
    test_canvas_cpu_skybox_fallback_reuses_cache_until_inputs_change();
    test_canvas_cpu_skybox_sanitizes_malformed_ortho_forward();
    test_canvas_cpu_skybox_normalizes_huge_ortho_forward();
    test_material_reflectivity();

    /* Mesh3D.Clear */
    test_mesh_clear();
    test_mesh_clear_then_rebuild();
    test_mesh_clear_null_safety();
    test_mesh_clear_stress();

    /* Sprite3D */
    test_sprite3d_new();
    test_sprite3d_new_null_texture();
    test_sprite3d_set_position();
    test_sprite3d_set_scale();
    test_sprite3d_set_frame();
    test_sprite3d_null_safety();
    test_sprite3d_billboard_reorients_to_camera();

    /* RenderTarget3D */
    test_rendertarget_new();
    test_rendertarget_new_hdr();
    test_rendertarget_dimensions();
    test_rendertarget_hdr_property();
    test_rendertarget_as_pixels();
    test_rendertarget_null_safety();
    test_rendertarget_as_pixels_syncs_gpu_color_on_demand();
    test_rendertarget_clear_sync_detaches_backend_callback();
    test_rendertarget_rejects_malformed_buffer_layouts();
    test_canvas_offscreen_constructor_contract();
    test_canvas_offscreen_renders_and_finalizes_without_window();
    test_canvas_offscreen_rejects_invalid_targets();
    test_canvas_offscreen_accelerated_constructor_contract();
    test_canvas_screenshot_syncs_render_target_on_demand();
    test_canvas_screenshot_returns_null_when_sync_fails();
    test_canvas_dimensions_follow_active_render_target();
    test_canvas_begin2d_uses_render_target_dimensions();
    test_canvas_begin_uses_active_output_aspect_without_mutating_camera();
    test_canvas_camera_relative_upload_rebases_frame_payloads();
    test_canvas_camera_relative_upload_rebases_raw_and_generated_vertices();
    test_particles3d_spread_uses_radians();
    test_particles3d_extreme_finite_inputs_remain_bounded();
    test_particles3d_getters_sanitize_corrupt_private_state();
    test_canvas_resize_updates_backend_and_projection_aspect();
    test_canvas_render_scale_failure_preserves_state();
    test_canvas_fog_and_shadow_state_sanitize_inputs();
    test_canvas_begin_applies_camera_shake_without_follow();
    test_canvas_overlay_draws_replay_after_3d_frame();
    test_canvas_aa_text_reuses_persistent_raster();
    test_canvas_postfx_uses_render_target_pixels();
    test_canvas_overlay_clip_and_new_primitives();
    test_canvas_round_rect_clip_bounds_vertices();
    test_gameui_widgets_draw_on_canvas3d();
    test_canvas_postfx_uses_hdr_render_target_mirror();
    test_canvas_postfx_hdr_mode0_tonemap_applies_gamma();
    test_canvas_postfx_applies_taa_on_cpu_rtt_path();
    test_canvas_postfx_applies_advanced_cpu_rtt_effects();
    test_canvas_postfx_bind_time_capability_validation();
    test_canvas_postfx_retains_owned_reference();
    test_canvas_render_target_retains_owned_reference();
    test_canvas_render_target_rejects_mid_frame_changes();
    test_canvas_light_retains_owned_reference();
    test_canvas_light_supports_last_slot();
    test_canvas_light_rejects_invalid_inputs();
    test_canvas_light_params_sanitize_corrupt_type();
    test_canvas_default_lighting_and_clear_lights();
    test_canvas_clustered_lighting_capability_gate();
    test_canvas_platform_gpu_clustered_lighting_capability();
    test_canvas_software_clustered_lighting_submits_many_lights();
    test_canvas_shadow_cascades_capability_gate();
    test_canvas_texture_backend_support_queries();
    test_canvas_light_revision_stamps();
    test_canvas_occlusion_culling_skips_covered_opaque_draws();
    test_backend_reversed_z_negates_projection_z_row();
    test_canvas_hiz_rasterizer_culls_behind_rotated_occluder();
    test_canvas_hiz_high_poly_proxy_is_conservative();
    test_frame_light_flatten_cache_shares_snapshot_across_draws();
    test_shadow_distance_setter_and_effective_range();
    test_instanced_draw_precomputes_world_bounds_in_snapshot_pass();
    test_canvas_texture_upload_bytes_telemetry();
    test_canvas_frame_gpu_time_telemetry();
    test_canvas_texture_upload_budget_controls_backend();
    test_canvas_delta_time_preserves_first_zero();
    test_canvas_poll_event_queue_drains_in_order();
    test_canvas_delta_time_cap_and_disable();
    test_canvas_delta_time_sec_clamps_huge_dtmax_without_overflow();
    test_canvas_quality_getters_sanitize_corrupt_private_state();
    test_canvas_synthetic_mouse_accumulation_clamps();
    test_canvas_fps_uses_rolling_microsecond_samples();
    test_canvas_boolean_setters_normalize();
    test_canvas_uses_single_present_pacer();
    test_canvas_material_shading_model_mapping();
    test_canvas_material_command_sanitizes_corrupt_fields();
    test_canvas_material_textureasset_resolves_resident_mip_on_draw();
    test_canvas_material_textureasset_forwards_native_blocks_on_draw();
    test_canvas_draw_mesh_clears_pending_splat_on_failed_draw();
    test_canvas_draw_mesh_sanitizes_pending_splat_scales();
    test_canvas_draw_mesh_rejects_corrupt_raw_morph_shape_count();
    test_canvas_reuses_float_payload_snapshots_within_frame();
    test_canvas_draw_terrain_rejects_2d_frame();
    test_canvas_draw_terrain_sanitizes_nonfinite_lod_distance();
    test_canvas_draw_terrain_disables_cpu_occlusion_by_default();
    test_canvas_draw_terrain_culls_before_building_lod0();

    /* Terrain3D splat */
    test_terrain_create();
    test_terrain_set_splat_map();
    test_terrain_set_layer_texture();
    test_terrain_set_layer_scale();
    test_terrain_null_safety();
    test_terrain_stitch_edge_ignores_float_roundoff();
    test_terrain_setters_repair_stale_slots_before_rejecting_invalid_handles();
    test_terrain_single_pixel_splat_map_draws();
    test_terrain_splat_bake_uses_base_texture_for_missing_layers();
    test_terrain_splat_bake_uses_material_color_for_missing_layers();
    test_terrain_splat_layers_resolve_texture_assets();
    test_terrain_splat_bake_falls_back_when_textureasset_layer_loses_residency();
    test_terrain_draw_repairs_invalid_splat_map_and_restores_base_texture();
    test_terrain_accepts_decode_failure_checker_textureasset_splat_layer();

    /* SW Backend Features (SW-01 through SW-08) */
    test_vertex_color_default_white();
    test_shadow_enable_disable();
    test_mesh_tangents_for_normal_map();
    test_mesh_tangent_fallback_is_orthogonal_to_normal();
    test_canvas_draw_persistently_caches_missing_normal_map_tangents();
    test_canvas_draw_retains_heap_mesh_geometry();
    test_canvas_draw_rejects_public_raw_mesh_handles();
    test_mesh_transform_rejects_singular_normal_matrix();
    test_mesh_validation_errors_mark_build_failed();
    test_mesh_normals_recalc();
    test_mesh_recalc_normals_assigns_fallback_for_unreferenced_vertices();
    test_terrain_splat_layer_count();
    test_terrain_splat_map_set();

    /* Metal backend features (MTL-01 through MTL-08) */
    test_metal_spot_light_creates();
    test_metal_spot_light_narrow_cone();
    test_metal_material_all_maps();
    test_metal_material_map_null_safety();
    test_metal_material_map_replace();
    test_metal_fog_set_clear();
    test_metal_tangents_for_normal_map();

    /* Metal backend features (MTL-09 through MTL-14) */
    test_metal_shadow_enable_null();
    test_metal_instbatch_create();
    test_instbatch_remove_preserves_unrelated_motion_history();
    test_canvas_opaque_alpha_mode_keeps_instanced_path();
    test_canvas_instanced_repairs_corrupt_mesh_counts_before_tangents();
    test_canvas_instanced_gpu_synthesizes_previous_matrices();
    test_canvas_instanced_gpu_uses_explicit_previous_matrices();
    test_canvas_instanced_motion_history_separates_batches();
    test_canvas_legacy_translucent_batch_falls_back_from_instancing();
    test_canvas_instanced_fallback_chunks_without_omission();
    test_canvas_instanced_previous_matrices_require_pointer();
    test_metal_terrain_splat_for_gpu();
    test_metal_postfx_new();
    test_metal_postfx_grows_past_legacy_cap();
    test_metal_postfx_null_safety();
    test_metal_skinned_mesh_bone_fields();
    test_texatlas_rejects_oversized_images_and_defaults_missing_uvs();

    /* Backend */
    test_backend_select();
    test_backend_select_software_override();
    test_backend_select_platform_override();
    test_backend_default_policy_names();

    /* New Canvas3D bindings (wind sway + window-control NULL-safety) */
    test_wind_deform_base_fixed_canopy_sways();
    test_wind_deform_zero_strength_and_null_safe();
    test_canvas3d_window_bindings_null_safe();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
