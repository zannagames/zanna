//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_cubemap3d_ibl.cpp
// Purpose: Unit tests for CubeMap3D image-based lighting — SH-9 irradiance
//   projection, GGX-prefiltered specular mip chain, trilinear IBL sampling,
//   and the ensure/idempotency contract.
//
// Key invariants:
//   - A constant-color environment projects to a constant irradiance (DC-only
//     SH) and constant prefiltered levels.
//   - rt_cubemap3d_ensure_ibl is idempotent and assigns a stable ibl_identity.
//   - rt_cubemap_sample_ibl falls back to the legacy roughness blur when the
//     IBL payload has not been prepared.
//   - Mutable source and generated-payload mirrors are repaired from retained
//     ownership identities before sampling or finalization.
//
// Ownership/Lifetime:
//   - Test-scoped runtime objects are retained only for each case's duration.
// Links: src/runtime/graphics/3d/render/rt_cubemap3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_canvas3d.h"
#include "rt_internal.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "rt_canvas3d_internal.h"
}

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

extern "C" int rt_obj_release_check0(void *obj);
extern "C" void rt_obj_free(void *obj);
extern "C" void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

static int tests_passed = 0;
static int tests_total = 0;

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

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            printf("FAIL: expected %lld, got %lld\n", (long long)(b), (long long)(a));             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                                                     \
    do {                                                                                           \
        const double actual_ = (double)(a);                                                        \
        const double expected_ = (double)(b);                                                      \
        if (!std::isfinite(actual_) || !std::isfinite(expected_) ||                                \
            std::fabs(actual_ - expected_) > (eps)) {                                              \
            printf("FAIL: expected ~%f, got %f\n", expected_, actual_);                            \
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

/// Build a Pixels face filled with one 0xRRGGBBAA color.
static void *make_face(int size, int64_t rgba) {
    void *p = rt_pixels_new(size, size);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            rt_pixels_set_rgba(p, x, y, rgba);
    return p;
}

/// Build a cubemap from six per-face solid colors (order +X,-X,+Y,-Y,+Z,-Z).
static rt_cubemap3d *make_cubemap(int size, const int64_t face_rgba[6]) {
    void *faces[6];
    for (int i = 0; i < 6; i++)
        faces[i] = make_face(size, face_rgba[i]);
    return (rt_cubemap3d *)rt_cubemap3d_new(
        faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
}

static int tracked_pixels_finalizers = 0;

static void tracked_pixels_finalize(void *) {
    tracked_pixels_finalizers++;
}

static void release_runtime_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static std::filesystem::path write_hdr_fixture(const char *suffix,
                                               const std::string &header,
                                               const std::vector<uint8_t> &pixels) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("zanna_cubemap_") + suffix + ".hdr");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(header.data(), (std::streamsize)header.size());
    stream.write((const char *)pixels.data(), (std::streamsize)pixels.size());
    stream.close();
    return path;
}

static rt_cubemap3d *load_hdr_fixture(const std::filesystem::path &path) {
    std::string text = path.string();
    rt_string runtime_path = rt_string_from_bytes(text.data(), text.size());
    rt_cubemap3d *result = (rt_cubemap3d *)rt_cubemap3d_load_hdr_panorama(runtime_path, 1.0);
    rt_string_unref(runtime_path);
    return result;
}

static bool cubemap_faces_equal(const rt_cubemap3d *a, const rt_cubemap3d *b) {
    if (!a || !b || a->face_size != b->face_size)
        return false;
    for (int face = 0; face < 6; face++) {
        for (int y = 0; y < a->face_size; y++) {
            for (int x = 0; x < a->face_size; x++) {
                if (rt_pixels_get(a->faces[face], x, y) != rt_pixels_get(b->faces[face], x, y))
                    return false;
            }
        }
    }
    return true;
}

/// Radiance orientation signs/axis order and CRLF headers must canonicalize identically. The
/// old-RLE fixture also exercises a multi-byte repeat count with a zero low digit.
static void test_hdr_layouts_crlf_and_old_rle() {
    TEST("HDR orientation, CRLF, and old-RLE compatibility");
    const std::vector<uint8_t> canonical_pixels = {
        255,
        0,
        0,
        128,
        0,
        255,
        0,
        128,
        0,
        0,
        255,
        128,
        255,
        255,
        255,
        128,
    };
    const std::vector<uint8_t> reversed_pixels = {
        255,
        255,
        255,
        128,
        0,
        0,
        255,
        128,
        0,
        255,
        0,
        128,
        255,
        0,
        0,
        128,
    };
    const std::vector<uint8_t> axis_swapped_pixels = {
        0,
        0,
        255,
        128,
        255,
        0,
        0,
        128,
        255,
        255,
        255,
        128,
        0,
        255,
        0,
        128,
    };
    auto canonical_path = write_hdr_fixture(
        "canonical", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n", canonical_pixels);
    auto reversed_path = write_hdr_fixture(
        "reversed", "#?RADIANCE\r\nFORMAT=32-bit_rle_rgbe\r\n\r\n+Y 2 -X 2\r\n", reversed_pixels);
    auto swapped_path = write_hdr_fixture(
        "swapped", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+X 2 +Y 2\n", axis_swapped_pixels);
    rt_cubemap3d *canonical = load_hdr_fixture(canonical_path);
    rt_cubemap3d *reversed = load_hdr_fixture(reversed_path);
    rt_cubemap3d *swapped = load_hdr_fixture(swapped_path);
    EXPECT_TRUE(canonical && reversed && swapped, "all valid Radiance layouts load");
    EXPECT_TRUE(cubemap_faces_equal(canonical, reversed), "signed CRLF layout canonicalizes");
    EXPECT_TRUE(cubemap_faces_equal(canonical, swapped), "axis-swapped layout canonicalizes");

    std::vector<uint8_t> old_rle = {64, 32, 16, 128, 1, 1, 1, 0, 1, 1, 1, 1};
    auto old_rle_path = write_hdr_fixture(
        "old_rle", "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 257\n", old_rle);
    rt_cubemap3d *old_rle_map = load_hdr_fixture(old_rle_path);
    EXPECT_TRUE(old_rle_map != nullptr, "bounded multi-byte old-RLE repeat loads safely");

    release_runtime_object(canonical);
    release_runtime_object(reversed);
    release_runtime_object(swapped);
    release_runtime_object(old_rle_map);
    std::filesystem::remove(canonical_path);
    std::filesystem::remove(reversed_path);
    std::filesystem::remove(swapped_path);
    std::filesystem::remove(old_rle_path);
    PASS();
}

/// Constant environment: SH irradiance must be the environment color for every
/// normal, and every prefiltered level must resample to the same color.
static void test_constant_environment() {
    TEST("constant environment -> constant irradiance + prefilter");
    const int64_t gray = 0x808080FF; /* 0.502 linear-ish (stored 8-bit) */
    int64_t faces[6] = {gray, gray, gray, gray, gray, gray};
    rt_cubemap3d *cm = make_cubemap(32, faces);
    EXPECT_TRUE(cm != nullptr, "cubemap created");
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    EXPECT_TRUE(cm->ibl_ready == 1, "ibl ready");

    const float dirs[5][3] = {
        {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {0.577f, 0.577f, 0.577f}};
    const float expect = (float)0x80 / 255.0f;
    for (int i = 0; i < 5; i++) {
        float rgb[3];
        rt_sh9_eval_irradiance(cm->ibl_sh, dirs[i][0], dirs[i][1], dirs[i][2], rgb);
        EXPECT_NEAR(rgb[0], expect, 0.02);
        EXPECT_NEAR(rgb[1], expect, 0.02);
        EXPECT_NEAR(rgb[2], expect, 0.02);
    }
    for (int m = 0; m < cm->ibl_mip_count; m++) {
        float r, g, b;
        float rough = cm->ibl_mip_count > 1 ? (float)m / (float)(cm->ibl_mip_count - 1) : 0.0f;
        rt_cubemap_sample_ibl(cm, 0.3f, 0.8f, -0.5f, rough, &r, &g, &b);
        EXPECT_NEAR(r, expect, 0.02);
        EXPECT_NEAR(g, expect, 0.02);
        EXPECT_NEAR(b, expect, 0.02);
    }
    PASS();
}

/// Top-lit environment: irradiance along +Y must exceed irradiance along -Y,
/// and both must sit inside the physical bounds of the environment.
static void test_gradient_environment() {
    TEST("top-lit environment -> directional irradiance");
    const int64_t white = 0xFFFFFFFFll;
    const int64_t black = 0x000000FFll;
    const int64_t mid = 0x808080FF;
    /* +X, -X, +Y(top)=white, -Y(bottom)=black, +Z, -Z */
    int64_t faces[6] = {mid, mid, white, black, mid, mid};
    rt_cubemap3d *cm = make_cubemap(32, faces);
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);

    float up[3];
    float down[3];
    rt_sh9_eval_irradiance(cm->ibl_sh, 0, 1, 0, up);
    rt_sh9_eval_irradiance(cm->ibl_sh, 0, -1, 0, down);
    EXPECT_TRUE(up[0] > down[0] + 0.2f, "up irradiance dominates down");
    EXPECT_TRUE(up[0] > 0.5f && up[0] <= 1.05f, "up irradiance in bounds");
    EXPECT_TRUE(down[0] >= 0.0f && down[0] < 0.5f, "down irradiance in bounds");
    PASS();
}

/// ensure_ibl must be idempotent: second call keeps the payload and identity.
static void test_ensure_idempotent() {
    TEST("ensure_ibl idempotency + chain shape");
    int64_t faces[6] = {0x102030FF, 0x102030FF, 0x102030FF, 0x102030FF, 0x102030FF, 0x102030FF};
    rt_cubemap3d *cm = make_cubemap(64, faces);
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    uint64_t identity = cm->ibl_identity;
    int32_t mips = cm->ibl_mip_count;
    EXPECT_TRUE(identity != 0, "identity assigned");
    EXPECT_TRUE(mips >= 1 && mips <= RT_CUBEMAP3D_IBL_MAX_MIPS, "mip count bounded");
    EXPECT_EQ(cm->ibl_base_size, 64); /* min(face 64, 128) */
    for (int m = 0; m < mips; m++) {
        for (int f = 0; f < 6; f++) {
            EXPECT_TRUE(cm->ibl_mips[m][f] != nullptr, "mip face allocated");
            EXPECT_EQ(rt_pixels_width(cm->ibl_mips[m][f]), (int64_t)(64 >> m));
        }
    }
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    EXPECT_TRUE(cm->ibl_identity == identity, "identity stable across re-ensure");
    EXPECT_EQ(cm->ibl_mip_count, mips);
    PASS();
}

/// Without ensure_ibl, rt_cubemap_sample_ibl must fall back to the legacy
/// roughness blur and produce identical output.
static void test_sample_fallback() {
    TEST("sample_ibl falls back without prepared payload");
    int64_t faces[6] = {0xFF0000FF, 0x00FF00FF, 0x0000FFFF, 0xFFFF00FF, 0x00FFFFFF, 0xFF00FFFF};
    rt_cubemap3d *cm = make_cubemap(16, faces);
    float ar, ag, ab;
    float br, bg, bb;
    rt_cubemap_sample_ibl(cm, 0.2f, 0.5f, 0.9f, 0.4f, &ar, &ag, &ab);
    rt_cubemap_sample_roughness(cm, 0.2f, 0.5f, 0.9f, 0.4f, &br, &bg, &bb);
    EXPECT_NEAR(ar, br, 1e-6);
    EXPECT_NEAR(ag, bg, 1e-6);
    EXPECT_NEAR(ab, bb, 1e-6);
    PASS();
}

/// A concentrated bright region must blur outward as roughness rises: on-axis
/// energy drops, off-axis energy grows, monotonically across the chain.
static void test_prefilter_blurs_with_roughness() {
    TEST("prefilter spreads a bright region with roughness");
    const int64_t black = 0x000000FF;
    int size = 32;
    void *faces[6];
    for (int i = 0; i < 6; i++)
        faces[i] = make_face(size, black);
    /* Bright 8x8 block in the middle of the +Z face (index 4). */
    for (int y = 12; y < 20; y++)
        for (int x = 12; x < 20; x++)
            rt_pixels_set_rgba(faces[4], x, y, 0xFFFFFFFFll);
    rt_cubemap3d *cm = (rt_cubemap3d *)rt_cubemap3d_new(
        faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);

    float sharp_r, sharp_g, sharp_b;
    float rough_r, rough_g, rough_b;
    /* On-axis: straight at the bright block. */
    rt_cubemap_sample_ibl(cm, 0.0f, 0.0f, 1.0f, 0.0f, &sharp_r, &sharp_g, &sharp_b);
    rt_cubemap_sample_ibl(cm, 0.0f, 0.0f, 1.0f, 1.0f, &rough_r, &rough_g, &rough_b);
    EXPECT_TRUE(sharp_r > 0.9f, "mirror sample sees the bright block");
    EXPECT_TRUE(rough_r < sharp_r - 0.1f, "rough sample diluted on-axis");
    /* Off-axis (40 degrees off): mirror sees black, rough sees bleed. */
    rt_cubemap_sample_ibl(cm, 0.64f, 0.0f, 0.77f, 0.0f, &sharp_r, &sharp_g, &sharp_b);
    rt_cubemap_sample_ibl(cm, 0.64f, 0.0f, 0.77f, 1.0f, &rough_r, &rough_g, &rough_b);
    EXPECT_TRUE(sharp_r < 0.05f, "mirror off-axis stays dark");
    EXPECT_TRUE(rough_r > sharp_r, "rough off-axis gains bled energy");
    PASS();
}

/// Source-face fields are public runtime payload mirrors, not ownership identities.
static void test_source_face_state_repair() {
    TEST("source faces and allocation metadata repair before sampling");
    void *faces[6];
    for (int f = 0; f < 6; f++)
        faces[f] = make_face(8, 0xFF0000FF);
    void *substitute = make_face(8, 0x00FF00FF);
    rt_cubemap3d *cm = (rt_cubemap3d *)rt_cubemap3d_new(
        faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
    EXPECT_TRUE(cm != nullptr && substitute != nullptr, "repair fixtures exist");
    for (int f = 0; f < 6; f++)
        cm->faces[f] = substitute;
    cm->face_size = -8;
    cm->cache_identity = 0;

    EXPECT_EQ(rt_cubemap3d_is_complete(cm), 1);
    EXPECT_EQ(cm->face_size, 8);
    EXPECT_TRUE(cm->cache_identity == cm->allocation_cache_identity,
                "source cache identity restored");
    for (int f = 0; f < 6; f++)
        EXPECT_TRUE(cm->faces[f] == cm->owned_faces[f], "source face mirror restored");

    float r, g, b;
    cm->faces[0] = substitute;
    rt_cubemap_sample(cm, 1.0f, 0.0f, 0.0f, &r, &g, &b);
    EXPECT_TRUE(r > 0.99f && g < 0.01f && b < 0.01f, "sample repairs its source view");
    cm->faces[0] = substitute;
    rt_cubemap_sample_unit(cm, 1.0f, 0.0f, 0.0f, &r, &g, &b);
    EXPECT_TRUE(r > 0.99f && g < 0.01f && b < 0.01f, "unit sample repairs its source view");
    cm->faces[0] = substitute;
    rt_cubemap_sample_roughness(cm, 1.0f, 0.0f, 0.0f, 0.5f, &r, &g, &b);
    EXPECT_TRUE(r > 0.99f && g < 0.01f && b < 0.01f, "roughness sample repairs its source view");

    release_runtime_object(cm);
    for (int f = 0; f < 6; f++)
        release_runtime_object(faces[f]);
    release_runtime_object(substitute);
    PASS();
}

/// Generated IBL metadata must be repaired before any mip indexing or backend use.
static void test_ibl_state_repair_and_rebuild() {
    TEST("IBL mirrors repair and malformed authoritative payloads rebuild");
    const int64_t red[6] = {0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF};
    rt_cubemap3d *cm = make_cubemap(8, red);
    void *substitute = make_face(8, 0x00FF00FF);
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    const int32_t expected_mips = cm->owned_ibl_mip_count;
    const int32_t expected_base = cm->owned_ibl_base_size;
    const uint64_t expected_identity = cm->owned_ibl_identity;
    const float expected_sh0 = cm->owned_ibl_sh[0];

    for (int f = 0; f < 6; f++)
        cm->ibl_mips[0][f] = substitute;
    cm->ibl_ready = 7;
    cm->ibl_mip_count = INT32_MAX;
    cm->ibl_base_size = -1;
    cm->ibl_identity = 0;
    cm->ibl_sh[0] = NAN;
    float r, g, b;
    rt_cubemap_sample_ibl(cm, 1.0f, 0.0f, 0.0f, 0.5f, &r, &g, &b);
    EXPECT_EQ(cm->ibl_ready, 1);
    EXPECT_EQ(cm->ibl_mip_count, expected_mips);
    EXPECT_EQ(cm->ibl_base_size, expected_base);
    EXPECT_TRUE(cm->ibl_identity == expected_identity, "IBL identity restored");
    EXPECT_NEAR(cm->ibl_sh[0], expected_sh0, 1e-6);
    for (int f = 0; f < 6; f++)
        EXPECT_TRUE(cm->ibl_mips[0][f] == cm->owned_ibl_mips[0][f], "IBL face mirror restored");
    EXPECT_TRUE(r > 0.99f && g < 0.01f && b < 0.01f,
                "IBL sample remains bounded after corrupt mip metadata");

    ((rt_pixels_impl *)cm->owned_ibl_mips[0][0])->width = 99;
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    EXPECT_TRUE(cm->ibl_identity != expected_identity,
                "malformed authoritative mip chain receives a fresh identity");
    EXPECT_EQ(rt_pixels_width(cm->ibl_mips[0][0]), expected_base);
    const uint64_t rebuilt_identity = cm->ibl_identity;
    cm->owned_ibl_sh[0] = NAN;
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    EXPECT_TRUE(cm->ibl_identity != rebuilt_identity,
                "non-finite authoritative SH cache is rebuilt transactionally");

    release_runtime_object(cm);
    release_runtime_object(substitute);
    PASS();
}

/// Finalization follows retained owner slots even when every public mirror is substituted.
static void test_cubemap_finalizer_uses_owner_slots() {
    TEST("cubemap finalizer releases source and IBL owner slots");
    void *faces[6];
    for (int f = 0; f < 6; f++) {
        faces[f] = make_face(4, 0x102030FF);
        rt_obj_set_finalizer(faces[f], tracked_pixels_finalize);
    }
    void *substitute = make_face(4, 0xA0B0C0FF);
    rt_cubemap3d *cm = (rt_cubemap3d *)rt_cubemap3d_new(
        faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
    EXPECT_EQ(rt_cubemap3d_ensure_ibl(cm), 1);
    rt_obj_set_finalizer(cm->owned_ibl_mips[0][0], tracked_pixels_finalize);
    for (int f = 0; f < 6; f++) {
        release_runtime_object(faces[f]);
        cm->faces[f] = substitute;
    }
    cm->ibl_mips[0][0] = substitute;
    tracked_pixels_finalizers = 0;
    release_runtime_object(cm);
    EXPECT_EQ(tracked_pixels_finalizers, 7);
    release_runtime_object(substitute);
    PASS();
}

int main() {
    printf("test_rt_cubemap3d_ibl:\n");
    test_constant_environment();
    test_gradient_environment();
    test_ensure_idempotent();
    test_sample_fallback();
    test_prefilter_blurs_with_roughness();
    test_source_face_state_repair();
    test_ibl_state_repair_and_rebuild();
    test_cubemap_finalizer_uses_owner_slots();
    test_hdr_layouts_crlf_and_old_rle();
    printf("%d/%d tests passed\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
