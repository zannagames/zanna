//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPixelsTests.cpp
// Purpose: Tests for Zanna.Graphics.Pixels software image buffer.
// Key invariants:
//   - Raw RGBA storage and all transforms preserve documented channel order.
//   - Mutation generations change only when observable pixel content changes.
//   - Extreme dimensions, coordinates, and encoded input fail without overflow.
// Ownership/Lifetime:
//   - Test-created runtime objects are owned by the runtime object system.
//   - Temporary files are confined to test-scoped platform paths.
// Links: src/runtime/graphics/2d/rt_pixels.c,
//        src/runtime/graphics/2d/rt_pixels_transform.c,
//        src/runtime/graphics/2d/rt_pixels_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"

#include "tests/common/PlatformSkip.h"
#include "tests/common/PosixCompat.h"
#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
static jmp_buf g_trap_jmp;
static bool g_trap_expected = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static uint32_t test_png_read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void release_test_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void test_png_write_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back((uint8_t)(value >> 24));
    out.push_back((uint8_t)(value >> 16));
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}

static uint32_t test_png_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t test_png_adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void test_png_append_chunk(std::vector<uint8_t> &out,
                                  const char type[4],
                                  const uint8_t *payload,
                                  size_t len) {
    test_png_write_u32(out, (uint32_t)len);
    size_t type_offset = out.size();
    out.insert(out.end(), type, type + 4);
    if (payload && len > 0)
        out.insert(out.end(), payload, payload + len);
    test_png_write_u32(out, test_png_crc32(out.data() + type_offset, len + 4));
}

static bool test_read_file(const char *path, std::vector<uint8_t> &data) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    data.resize((size_t)len);
    bool ok = data.empty() || fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

static bool test_write_file(const char *path, const std::vector<uint8_t> &data) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

static void test_png_expect_rejected(const char *path, const std::vector<uint8_t> &png) {
    assert(test_write_file(path, png));
    rt_string runtime_path = rt_string_from_bytes(path, strlen(path));
    assert(runtime_path != nullptr);
    assert(rt_pixels_load_png(runtime_path) == nullptr);
    rt_string_unref(runtime_path);
    unlink(path);
}

// ============================================================================
// Constructor Tests
// ============================================================================

static void test_new() {
    void *p = rt_pixels_new(100, 50);
    assert(p != nullptr);
    assert(rt_pixels_width(p) == 100);
    assert(rt_pixels_height(p) == 50);
    printf("test_new: PASSED\n");
}

static void test_new_zero_dimensions() {
    void *p = rt_pixels_new(0, 0);
    assert(p != nullptr);
    assert(rt_pixels_width(p) == 0);
    assert(rt_pixels_height(p) == 0);
    printf("test_new_zero_dimensions: PASSED\n");
}

static void test_new_negative_dimensions() {
    void *p = rt_pixels_new(-10, -20);
    assert(p == nullptr);
    printf("test_new_negative_dimensions: PASSED\n");
}

static void test_pixels_reject_incomplete_or_inconsistent_inline_layouts() {
    auto *short_data = static_cast<rt_pixels_impl *>(
        rt_obj_new_i64(RT_PIXELS_CLASS_ID, static_cast<int64_t>(sizeof(rt_pixels_impl))));
    assert(short_data != nullptr);
    std::memset(short_data, 0, sizeof(*short_data));
    short_data->width = 1;
    short_data->height = 1;
    short_data->data =
        reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(short_data) + sizeof(*short_data));
    short_data->cache_identity = 1;
    EXPECT_TRAP(rt_pixels_width(short_data));
    release_test_object(short_data);

    auto *wrong_data = static_cast<rt_pixels_impl *>(rt_obj_new_i64(
        RT_PIXELS_CLASS_ID, static_cast<int64_t>(sizeof(rt_pixels_impl) + sizeof(uint32_t))));
    assert(wrong_data != nullptr);
    std::memset(wrong_data, 0, sizeof(*wrong_data) + sizeof(uint32_t));
    uint32_t external_pixel = 0;
    wrong_data->width = 1;
    wrong_data->height = 1;
    wrong_data->data = &external_pixel;
    wrong_data->cache_identity = 1;
    EXPECT_TRAP(rt_pixels_height(wrong_data));
    release_test_object(wrong_data);

    auto *stale_cache = static_cast<rt_pixels_impl *>(rt_obj_new_i64(
        RT_PIXELS_CLASS_ID, static_cast<int64_t>(sizeof(rt_pixels_impl) + sizeof(uint32_t))));
    assert(stale_cache != nullptr);
    std::memset(stale_cache, 0, sizeof(*stale_cache) + sizeof(uint32_t));
    stale_cache->width = 1;
    stale_cache->height = 1;
    stale_cache->data = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(stale_cache) +
                                                     sizeof(*stale_cache));
    stale_cache->cache_identity = 1;
    stale_cache->generation = 2;
    stale_cache->alpha_scan_valid = 1;
    stale_cache->alpha_scan_generation = 1;
    stale_cache->alpha_scan_classification = RT_PIXELS_ALPHA_OPAQUE;
    EXPECT_TRAP(rt_pixels_width(stale_cache));
    release_test_object(stale_cache);

    printf("test_pixels_reject_incomplete_or_inconsistent_inline_layouts: PASSED\n");
}

// ============================================================================
// Pixel Access Tests
// ============================================================================

static void test_get_set() {
    void *p = rt_pixels_new(10, 10);

    // Initially should be 0 (transparent black)
    assert(rt_pixels_get(p, 5, 5) == 0);

    // Set a pixel
    int64_t red = 0xFF0000FF; // Red with full alpha
    rt_pixels_set(p, 5, 5, red);
    assert(rt_pixels_get(p, 5, 5) == red);

    printf("test_get_set: PASSED\n");
}

static void test_color_aware_setters_preserve_raw_rgba() {
    void *p = rt_pixels_new(2, 2);

    rt_pixels_set_color(p, 0, 0, rt_color_rgb(255, 0, 0));
    assert(rt_pixels_get(p, 0, 0) == 0xFF0000FF);

    rt_pixels_set_color(p, 1, 0, rt_color_rgba(0, 0, 255, 128));
    assert(rt_pixels_get(p, 1, 0) == 0x0000FF80);

    rt_pixels_set_rgba(p, 0, 1, 0x00000080);
    assert(rt_pixels_get(p, 0, 1) == 0x00000080);

    rt_pixels_fill_color(p, rt_color_rgba(1, 2, 3, 4));
    for (int64_t y = 0; y < 2; y++)
        for (int64_t x = 0; x < 2; x++)
            assert(rt_pixels_get(p, x, y) == 0x01020304);

    rt_pixels_fill_rgba(p, 0x00000080);
    assert(rt_pixels_get(p, 1, 1) == 0x00000080);

    printf("test_color_aware_setters_preserve_raw_rgba: PASSED\n");
}

static void test_color_getter_returns_color_compatible_value() {
    void *p = rt_pixels_new(2, 1);

    rt_pixels_set_rgba(p, 0, 0, 0x11223344);
    assert(rt_pixels_get(p, 0, 0) == 0x11223344);
    assert(rt_pixels_get_rgba(p, 0, 0) == 0x11223344);

    int64_t color = rt_pixels_get_color(p, 0, 0);
    assert(rt_color_get_r(color) == 0x11);
    assert(rt_color_get_g(color) == 0x22);
    assert(rt_color_get_b(color) == 0x33);
    assert(rt_color_get_a(color) == 0x44);

    int64_t transparent = rt_pixels_get_color(p, 1, 0);
    assert(rt_color_get_r(transparent) == 0);
    assert(rt_color_get_g(transparent) == 0);
    assert(rt_color_get_b(transparent) == 0);
    assert(rt_color_get_a(transparent) == 0);

    printf("test_color_getter_returns_color_compatible_value: PASSED\n");
}

static void test_get_out_of_bounds() {
    void *p = rt_pixels_new(10, 10);

    // Out of bounds should return 0
    assert(rt_pixels_get(p, -1, 0) == 0);
    assert(rt_pixels_get(p, 0, -1) == 0);
    assert(rt_pixels_get(p, 10, 0) == 0);
    assert(rt_pixels_get(p, 0, 10) == 0);
    assert(rt_pixels_get(p, 100, 100) == 0);

    printf("test_get_out_of_bounds: PASSED\n");
}

static void test_set_out_of_bounds() {
    void *p = rt_pixels_new(10, 10);

    // Set out of bounds - should be silently ignored
    rt_pixels_set(p, -1, 0, 0xFFFFFFFF);
    rt_pixels_set(p, 0, -1, 0xFFFFFFFF);
    rt_pixels_set(p, 10, 0, 0xFFFFFFFF);
    rt_pixels_set(p, 0, 10, 0xFFFFFFFF);

    // All pixels should still be 0
    for (int64_t y = 0; y < 10; y++) {
        for (int64_t x = 0; x < 10; x++) {
            assert(rt_pixels_get(p, x, y) == 0);
        }
    }

    printf("test_set_out_of_bounds: PASSED\n");
}

static void test_corners() {
    void *p = rt_pixels_new(5, 5);

    int64_t tl = 0x11111111;
    int64_t tr = 0x22222222;
    int64_t bl = 0x33333333;
    int64_t br = 0x44444444;

    rt_pixels_set(p, 0, 0, tl);
    rt_pixels_set(p, 4, 0, tr);
    rt_pixels_set(p, 0, 4, bl);
    rt_pixels_set(p, 4, 4, br);

    assert(rt_pixels_get(p, 0, 0) == tl);
    assert(rt_pixels_get(p, 4, 0) == tr);
    assert(rt_pixels_get(p, 0, 4) == bl);
    assert(rt_pixels_get(p, 4, 4) == br);

    printf("test_corners: PASSED\n");
}

static void test_integer_helpers_cover_signed_extremes() {
    assert(rt_pixels_abs_diff_sat64(INT64_MIN, INT64_MAX) == INT64_MAX);
    assert(rt_pixels_abs_diff_sat64(INT64_MAX, INT64_MIN) == INT64_MAX);
    assert(rt_pixels_abs_diff_sat64(INT64_MIN, INT64_MIN + 7) == 7);
    assert(rt_pixels_abs_diff_sat64(INT64_MAX - 9, INT64_MAX) == 9);

    assert(isqrt64(-1) == 0);
    assert(isqrt64(0) == 0);
    assert(isqrt64(1) == 1);
    assert(isqrt64(2) == 1);
    assert(isqrt64(4) == 2);
    assert(isqrt64(INT64_MAX) == INT64_C(3037000499));
    printf("test_integer_helpers_cover_signed_extremes: PASSED\n");
}

static void test_generation_changes_only_for_content_changes() {
    void *p = rt_pixels_new(3, 2);
    assert(rt_pixels_generation(p) == 0);

    rt_pixels_set_rgba(p, 1, 1, 0);
    assert(rt_pixels_generation(p) == 0);
    rt_pixels_set_rgba(p, 1, 1, 0x10203040);
    assert(rt_pixels_generation(p) == 1);
    rt_pixels_set_rgba(p, 1, 1, 0x10203040);
    assert(rt_pixels_generation(p) == 1);

    rt_pixels_fill_rgba(p, 0xAABBCCDD);
    assert(rt_pixels_generation(p) == 2);
    rt_pixels_fill_rgba(p, 0xAABBCCDD);
    assert(rt_pixels_generation(p) == 2);

    rt_pixels_clear(p);
    assert(rt_pixels_generation(p) == 3);
    rt_pixels_clear(p);
    assert(rt_pixels_generation(p) == 3);

    rt_pixels_impl *impl = static_cast<rt_pixels_impl *>(p);
    impl->generation = UINT64_MAX;
    rt_pixels_set_rgba(p, 0, 0, 1);
    assert(rt_pixels_generation(p) == 1);
    printf("test_generation_changes_only_for_content_changes: PASSED\n");
}

// ============================================================================
// Fill Operations Tests
// ============================================================================

static void test_fill() {
    void *p = rt_pixels_new(5, 5);
    int64_t color = 0xAABBCCDD;

    rt_pixels_fill(p, color);

    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            assert(rt_pixels_get(p, x, y) == color);
        }
    }

    printf("test_fill: PASSED\n");
}

static void test_clear() {
    void *p = rt_pixels_new(5, 5);

    // Fill with non-zero color
    rt_pixels_fill(p, 0xFFFFFFFF);

    // Clear to transparent black
    rt_pixels_clear(p);

    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            assert(rt_pixels_get(p, x, y) == 0);
        }
    }

    printf("test_clear: PASSED\n");
}

// ============================================================================
// Copy Operations Tests
// ============================================================================

static void test_copy_basic() {
    void *src = rt_pixels_new(10, 10);
    void *dst = rt_pixels_new(10, 10);

    // Create a pattern in source
    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            rt_pixels_set(src, x, y, (int64_t)(y * 5 + x));
        }
    }

    // Copy 5x5 block from (0,0) to (2,2)
    rt_pixels_copy(dst, 2, 2, src, 0, 0, 5, 5);

    // Verify copy
    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            int64_t expected = y * 5 + x;
            assert(rt_pixels_get(dst, x + 2, y + 2) == expected);
        }
    }

    printf("test_copy_basic: PASSED\n");
}

static void test_copy_clipping() {
    void *src = rt_pixels_new(10, 10);
    void *dst = rt_pixels_new(5, 5);

    // Fill source with a value
    rt_pixels_fill(src, 0x12345678);

    // Copy with clipping (requesting more than fits)
    rt_pixels_copy(dst, 0, 0, src, 0, 0, 10, 10);

    // Destination should be filled (clipped to its size)
    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            assert(rt_pixels_get(dst, x, y) == 0x12345678);
        }
    }

    printf("test_copy_clipping: PASSED\n");
}

static void test_copy_negative_dest() {
    void *src = rt_pixels_new(10, 10);
    void *dst = rt_pixels_new(10, 10);

    // Fill source
    rt_pixels_fill(src, 0xABCDEF00);

    // Copy with negative destination offset (should clip from source)
    rt_pixels_copy(dst, -2, -2, src, 0, 0, 5, 5);

    // Only 3x3 pixels should be copied to (0,0)-(2,2)
    for (int64_t y = 0; y < 3; y++) {
        for (int64_t x = 0; x < 3; x++) {
            assert(rt_pixels_get(dst, x, y) == 0xABCDEF00);
        }
    }

    // Rest should be 0
    assert(rt_pixels_get(dst, 5, 5) == 0);

    printf("test_copy_negative_dest: PASSED\n");
}

static void test_copy_overlap_forward() {
    void *p = rt_pixels_new(4, 4);

    for (int64_t y = 0; y < 4; y++)
        for (int64_t x = 0; x < 4; x++)
            rt_pixels_set(p, x, y, (int64_t)(y * 10 + x));

    rt_pixels_copy(p, 1, 1, p, 0, 0, 3, 3);

    for (int64_t y = 0; y < 3; y++)
        for (int64_t x = 0; x < 3; x++)
            assert(rt_pixels_get(p, x + 1, y + 1) == (int64_t)(y * 10 + x));

    printf("test_copy_overlap_forward: PASSED\n");
}

static void test_copy_overlap_backward() {
    void *p = rt_pixels_new(4, 4);

    for (int64_t y = 0; y < 4; y++)
        for (int64_t x = 0; x < 4; x++)
            rt_pixels_set(p, x, y, (int64_t)(y * 10 + x));

    rt_pixels_copy(p, 0, 0, p, 1, 1, 3, 3);

    for (int64_t y = 0; y < 3; y++)
        for (int64_t x = 0; x < 3; x++)
            assert(rt_pixels_get(p, x, y) == (int64_t)((y + 1) * 10 + (x + 1)));

    printf("test_copy_overlap_backward: PASSED\n");
}

static void test_copy_extreme_coordinates_noop() {
    void *src = rt_pixels_new(2, 2);
    void *dst = rt_pixels_new(2, 2);
    rt_pixels_fill(src, 0x11223344);

    rt_pixels_copy(dst, INT64_MAX - 1, 0, src, -1, 0, INT64_MAX, 1);
    rt_pixels_copy(dst, 0, 0, src, INT64_MAX - 1, 0, INT64_MAX, 1);

    assert(rt_pixels_get(dst, 0, 0) == 0);
    assert(rt_pixels_get(dst, 1, 0) == 0);
    assert(rt_pixels_get(dst, 0, 1) == 0);
    assert(rt_pixels_get(dst, 1, 1) == 0);
    printf("test_copy_extreme_coordinates_noop: PASSED\n");
}

static void test_clone() {
    void *p = rt_pixels_new(5, 5);

    // Create a pattern
    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            rt_pixels_set(p, x, y, (int64_t)(y * 5 + x));
        }
    }

    void *clone = rt_pixels_clone(p);

    assert(clone != nullptr);
    assert(clone != p); // Different object
    assert(rt_pixels_width(clone) == rt_pixels_width(p));
    assert(rt_pixels_height(clone) == rt_pixels_height(p));

    // Verify all pixels match
    for (int64_t y = 0; y < 5; y++) {
        for (int64_t x = 0; x < 5; x++) {
            assert(rt_pixels_get(clone, x, y) == rt_pixels_get(p, x, y));
        }
    }

    // Modify original, clone should not change
    rt_pixels_set(p, 0, 0, 0xFFFFFFFF);
    assert(rt_pixels_get(clone, 0, 0) == 0);

    printf("test_clone: PASSED\n");
}

static void test_copy_generation_and_alpha_cache_are_exact() {
    void *src = rt_pixels_new(2, 2);
    void *dst = rt_pixels_new(2, 2);
    rt_pixels_set_rgba(src, 0, 0, 0x112233FF);
    rt_pixels_set_rgba(src, 1, 0, 0x44556680);

    rt_pixels_impl *src_impl = static_cast<rt_pixels_impl *>(src);
    rt_pixels_impl *dst_impl = static_cast<rt_pixels_impl *>(dst);
    assert(rt_pixels_alpha_classification_cached(src_impl) == RT_PIXELS_ALPHA_FRACTIONAL);
    assert(src_impl->alpha_classification_scan_count == 1);

    rt_pixels_copy(dst, 0, 0, src, 0, 0, 2, 2);
    assert(rt_pixels_generation(dst) == 1);
    assert(dst_impl->alpha_scan_valid == 1);
    assert(rt_pixels_alpha_classification_cached(dst_impl) == RT_PIXELS_ALPHA_FRACTIONAL);
    assert(dst_impl->alpha_classification_scan_count == 0);

    rt_pixels_copy(dst, 0, 0, src, 0, 0, 2, 2);
    assert(rt_pixels_generation(dst) == 1);
    rt_pixels_copy(dst, 0, 0, dst, 0, 0, 2, 2);
    assert(rt_pixels_generation(dst) == 1);

    void *clone = rt_pixels_clone(src);
    rt_pixels_impl *clone_impl = static_cast<rt_pixels_impl *>(clone);
    assert(clone_impl->alpha_scan_valid == 1);
    assert(rt_pixels_alpha_classification_cached(clone_impl) == RT_PIXELS_ALPHA_FRACTIONAL);
    assert(clone_impl->alpha_classification_scan_count == 0);
    printf("test_copy_generation_and_alpha_cache_are_exact: PASSED\n");
}

// ============================================================================
// Byte Conversion Tests
// ============================================================================

static void test_to_bytes() {
    void *p = rt_pixels_new(2, 2);

    // Set 4 pixels with distinct RGBA values
    rt_pixels_set(p, 0, 0, 0x11223344);
    rt_pixels_set(p, 1, 0, 0x55667788);
    rt_pixels_set(p, 0, 1, 0x99AABBCC);
    rt_pixels_set(p, 1, 1, 0xDDEEFF00);

    void *bytes = rt_pixels_to_bytes(p);
    assert(bytes != nullptr);
    assert(rt_bytes_len(bytes) == 16); // 2x2 * 4 bytes per pixel
    const uint8_t expected[16] = {0x11,
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
                                  0xFF,
                                  0x00};
    for (int64_t i = 0; i < 16; ++i)
        assert(rt_bytes_get(bytes, i) == expected[i]);

    printf("test_to_bytes: PASSED\n");
}

static void test_from_bytes() {
    // Create bytes for a 2x2 image
    void *bytes = rt_bytes_new(16);

    // Manually set pixel data (canonical RGBA byte order)
    // Pixel (0,0) = 0x11223344
    rt_bytes_set(bytes, 0, 0x11);
    rt_bytes_set(bytes, 1, 0x22);
    rt_bytes_set(bytes, 2, 0x33);
    rt_bytes_set(bytes, 3, 0x44);
    // Pixel (1,0) = 0x55667788
    rt_bytes_set(bytes, 4, 0x55);
    rt_bytes_set(bytes, 5, 0x66);
    rt_bytes_set(bytes, 6, 0x77);
    rt_bytes_set(bytes, 7, 0x88);
    // Pixel (0,1) = 0x99AABBCC
    rt_bytes_set(bytes, 8, 0x99);
    rt_bytes_set(bytes, 9, 0xAA);
    rt_bytes_set(bytes, 10, 0xBB);
    rt_bytes_set(bytes, 11, 0xCC);
    // Pixel (1,1) = 0xDDEEFF00
    rt_bytes_set(bytes, 12, 0xDD);
    rt_bytes_set(bytes, 13, 0xEE);
    rt_bytes_set(bytes, 14, 0xFF);
    rt_bytes_set(bytes, 15, 0x00);

    void *p = rt_pixels_from_bytes(2, 2, bytes);
    assert(p != nullptr);
    assert(rt_pixels_generation(p) == 0);
    assert(rt_pixels_width(p) == 2);
    assert(rt_pixels_height(p) == 2);

    assert(rt_pixels_get(p, 0, 0) == 0x11223344);
    assert(rt_pixels_get(p, 1, 0) == 0x55667788);
    assert(rt_pixels_get(p, 0, 1) == (int64_t)0x99AABBCC);
    assert(rt_pixels_get(p, 1, 1) == (int64_t)0xDDEEFF00);

    printf("test_from_bytes: PASSED\n");
}

static void test_from_bytes_rejects_wrong_object() {
    void *not_bytes = rt_pixels_new(1, 1);
    assert(not_bytes != nullptr);
    EXPECT_TRAP((void)rt_pixels_from_bytes(1, 1, not_bytes));
    printf("test_from_bytes_rejects_wrong_object: PASSED\n");
}

static void test_round_trip() {
    void *original = rt_pixels_new(10, 10);

    // Create a pattern
    for (int64_t y = 0; y < 10; y++) {
        for (int64_t x = 0; x < 10; x++) {
            rt_pixels_set(original, x, y, (int64_t)((y << 24) | (x << 16) | 0xFF));
        }
    }

    // Convert to bytes and back
    void *bytes = rt_pixels_to_bytes(original);
    void *restored = rt_pixels_from_bytes(10, 10, bytes);

    // Verify all pixels match
    for (int64_t y = 0; y < 10; y++) {
        for (int64_t x = 0; x < 10; x++) {
            assert(rt_pixels_get(restored, x, y) == rt_pixels_get(original, x, y));
        }
    }

    printf("test_round_trip: PASSED\n");
}

// ============================================================================
// Edge Case Tests
// ============================================================================

static void test_large_image() {
    // Create a reasonably large image
    void *p = rt_pixels_new(1000, 1000);
    assert(p != nullptr);
    assert(rt_pixels_width(p) == 1000);
    assert(rt_pixels_height(p) == 1000);

    // Set corner pixels
    rt_pixels_set(p, 0, 0, 0x11111111);
    rt_pixels_set(p, 999, 0, 0x22222222);
    rt_pixels_set(p, 0, 999, 0x33333333);
    rt_pixels_set(p, 999, 999, 0x44444444);

    assert(rt_pixels_get(p, 0, 0) == 0x11111111);
    assert(rt_pixels_get(p, 999, 0) == 0x22222222);
    assert(rt_pixels_get(p, 0, 999) == 0x33333333);
    assert(rt_pixels_get(p, 999, 999) == 0x44444444);

    printf("test_large_image: PASSED\n");
}

static void test_single_pixel() {
    void *p = rt_pixels_new(1, 1);
    assert(p != nullptr);
    assert(rt_pixels_width(p) == 1);
    assert(rt_pixels_height(p) == 1);

    rt_pixels_set(p, 0, 0, 0xDEADBEEF);
    assert(rt_pixels_get(p, 0, 0) == (int64_t)0xDEADBEEF);

    printf("test_single_pixel: PASSED\n");
}

// ============================================================================
// BMP Load/Save Tests
// ============================================================================

static void test_bmp_save_load_roundtrip() {
    // Create a test image with known colors
    void *p = rt_pixels_new(10, 10);
    assert(p != nullptr);

    // Fill with a pattern - Red at (0,0), Green at (9,0), Blue at (0,9), White at (9,9)
    // Using 0xRRGGBBAA format
    rt_pixels_set(p, 0, 0, 0xFF0000FF); // Red
    rt_pixels_set(p, 9, 0, 0x00FF00FF); // Green
    rt_pixels_set(p, 0, 9, 0x0000FFFF); // Blue
    rt_pixels_set(p, 9, 9, 0xFFFFFFFF); // White

    // Fill middle with gray
    for (int y = 3; y < 7; y++) {
        for (int x = 3; x < 7; x++) {
            rt_pixels_set(p, x, y, 0x808080FF); // Gray
        }
    }

    // Create temp file path
    char tmpfile[] = "/tmp/zanna_test_bmp_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    close(fd);

    // Add .bmp extension
    char bmppath[256];
    snprintf(bmppath, sizeof(bmppath), "%s.bmp", tmpfile);
    rename(tmpfile, bmppath);

    // Save to BMP
    rt_string path = rt_string_from_bytes(bmppath, strlen(bmppath));
    int64_t result = rt_pixels_save_bmp(p, path);
    assert(result == 1);

    // Load BMP back
    void *loaded = rt_pixels_load_bmp(path);
    assert(loaded != nullptr);
    assert(rt_pixels_width(loaded) == 10);
    assert(rt_pixels_height(loaded) == 10);

    // Verify colors (BMP is 24-bit, so alpha is always 0xFF on load)
    int64_t red_loaded = rt_pixels_get(loaded, 0, 0);
    int64_t green_loaded = rt_pixels_get(loaded, 9, 0);
    int64_t blue_loaded = rt_pixels_get(loaded, 0, 9);
    int64_t white_loaded = rt_pixels_get(loaded, 9, 9);

    // Check RGB components (ignore alpha differences)
    assert((red_loaded & 0xFFFFFF00) == 0xFF000000);   // Red
    assert((green_loaded & 0xFFFFFF00) == 0x00FF0000); // Green
    assert((blue_loaded & 0xFFFFFF00) == 0x0000FF00);  // Blue
    assert((white_loaded & 0xFFFFFF00) == 0xFFFFFF00); // White

    // Verify gray in middle
    int64_t gray = rt_pixels_get(loaded, 5, 5);
    assert((gray & 0xFFFFFF00) == 0x80808000);

    // Cleanup
    unlink(bmppath);

    printf("test_bmp_save_load_roundtrip: PASSED\n");
}

static void test_bmp_load_invalid_path() {
    const char *invalid = "/nonexistent/path/file.bmp";
    rt_string path = rt_string_from_bytes(invalid, strlen(invalid));
    void *p = rt_pixels_load_bmp(path);
    assert(p == nullptr);

    printf("test_bmp_load_invalid_path: PASSED\n");
}

static void test_bmp_save_null_inputs() {
    // Save with null pixels should return 0
    const char *tmp = "/tmp/test.bmp";
    rt_string path = rt_string_from_bytes(tmp, strlen(tmp));
    assert(rt_pixels_save_bmp(nullptr, path) == 0);

    // Save with null path should return 0
    void *p = rt_pixels_new(10, 10);
    assert(rt_pixels_save_bmp(p, nullptr) == 0);

    printf("test_bmp_save_null_inputs: PASSED\n");
}

static void test_bmp_odd_dimensions() {
    // BMP row padding test - use width that requires padding
    void *p = rt_pixels_new(7, 5); // 7 pixels = 21 bytes, needs 3 bytes padding to reach 24
    assert(p != nullptr);

    // Fill with a checkerboard pattern
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 7; x++) {
            if ((x + y) % 2 == 0)
                rt_pixels_set(p, x, y, 0xFF0000FF); // Red
            else
                rt_pixels_set(p, x, y, 0x00FF00FF); // Green
        }
    }

    // Create temp file
    char tmpfile[] = "/tmp/zanna_test_bmp_odd_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    close(fd);

    char bmppath[256];
    snprintf(bmppath, sizeof(bmppath), "%s.bmp", tmpfile);
    rename(tmpfile, bmppath);

    // Save and reload
    rt_string path = rt_string_from_bytes(bmppath, strlen(bmppath));
    assert(rt_pixels_save_bmp(p, path) == 1);

    void *loaded = rt_pixels_load_bmp(path);
    assert(loaded != nullptr);
    assert(rt_pixels_width(loaded) == 7);
    assert(rt_pixels_height(loaded) == 5);

    // Verify checkerboard pattern
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 7; x++) {
            int64_t color = rt_pixels_get(loaded, x, y);
            if ((x + y) % 2 == 0)
                assert((color & 0xFFFFFF00) == 0xFF000000); // Red
            else
                assert((color & 0xFFFFFF00) == 0x00FF0000); // Green
        }
    }

    // Cleanup
    unlink(bmppath);

    printf("test_bmp_odd_dimensions: PASSED\n");
}

static void test_bmp_load_rejects_invalid_header_extents() {
    void *p = rt_pixels_new(1, 1);
    assert(p != nullptr);
    rt_pixels_set(p, 0, 0, 0x123456FF);

    const char *bmppath = "/tmp/zanna_test_bad_bmp_header.bmp";
    rt_string path = rt_string_from_bytes(bmppath, strlen(bmppath));
    assert(rt_pixels_save_bmp(p, path) == 1);

    std::vector<uint8_t> data;
    assert(test_read_file(bmppath, data));
    assert(data.size() >= 54);

    data[10] = data[11] = data[12] = data[13] = 0; // data_offset before headers
    assert(test_write_file(bmppath, data));
    assert(rt_pixels_load_bmp(path) == nullptr);

    assert(rt_pixels_save_bmp(p, path) == 1);
    assert(test_read_file(bmppath, data));
    assert(data.size() >= 54);
    data[2] = 54;
    data[3] = data[4] = data[5] = 0; // declared file_size too small for pixel row
    assert(test_write_file(bmppath, data));
    assert(rt_pixels_load_bmp(path) == nullptr);

    assert(rt_pixels_save_bmp(p, path) == 1);
    assert(test_read_file(bmppath, data));
    assert(data.size() >= 54);
    data[6] = 1; // reserved fields must be zero
    assert(test_write_file(bmppath, data));
    assert(rt_pixels_load_bmp(path) == nullptr);

    assert(rt_pixels_save_bmp(p, path) == 1);
    assert(test_read_file(bmppath, data));
    assert(data.size() >= 54);
    data[26] = 2; // BITMAPINFOHEADER planes must be exactly one
    assert(test_write_file(bmppath, data));
    assert(rt_pixels_load_bmp(path) == nullptr);

    assert(rt_pixels_save_bmp(p, path) == 1);
    assert(test_read_file(bmppath, data));
    assert(data.size() >= 54);
    data[34] = 3; // nonzero image_size must match the padded pixel payload
    assert(test_write_file(bmppath, data));
    assert(rt_pixels_load_bmp(path) == nullptr);

    unlink(bmppath);
    rt_string_unref(path);
    printf("test_bmp_load_rejects_invalid_header_extents: PASSED\n");
}

static void test_image_paths_reject_embedded_nul() {
    static const char prefix[] = "/tmp/zanna_test_embedded_nul.bmp";
    static const char embedded_path[] = "/tmp/zanna_test_embedded_nul.bmp\0ignored";
    rt_string normal = rt_string_from_bytes(prefix, strlen(prefix));
    rt_string embedded = rt_string_from_bytes(embedded_path, sizeof(embedded_path) - 1u);
    assert(normal != nullptr);
    assert(embedded != nullptr);

    void *pixels = rt_pixels_new(1, 1);
    assert(pixels != nullptr);
    rt_pixels_set(pixels, 0, 0, 0x123456FF);
    assert(rt_pixels_save_bmp(pixels, normal) == 1);

    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
    assert(test_read_file(prefix, before));
    assert(rt_pixels_save_bmp(pixels, embedded) == 0);
    assert(rt_pixels_save_png(pixels, embedded) == 0);
    assert(test_read_file(prefix, after));
    assert(after == before);

    EXPECT_TRAP((void)rt_pixels_load(embedded));
    EXPECT_TRAP((void)rt_pixels_load_bmp(embedded));
    EXPECT_TRAP((void)rt_pixels_load_png(embedded));
    EXPECT_TRAP((void)rt_pixels_load_jpeg(embedded));
    EXPECT_TRAP((void)rt_pixels_load_gif(embedded));

    unlink(prefix);
    rt_string_unref(embedded);
    rt_string_unref(normal);
    printf("test_image_paths_reject_embedded_nul: PASSED\n");
}

static void test_png_load_rejects_bad_chunk_crc() {
    void *p = rt_pixels_new(1, 1);
    assert(p != nullptr);
    rt_pixels_set(p, 0, 0, 0xFF0000FF);

    const char *pngpath = "/tmp/zanna_test_bad_crc.png";
    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_save_png(p, path) == 1);

    std::vector<uint8_t> data;
    assert(test_read_file(pngpath, data));
    assert(data.size() > 33);
    data[29] ^= 0x01u;
    assert(test_write_file(pngpath, data));

    void *loaded = rt_pixels_load_png(path);
    assert(loaded == nullptr);
    unlink(pngpath);
    printf("test_png_load_rejects_bad_chunk_crc: PASSED\n");
}

static void test_png_load_rejects_bad_zlib_adler() {
    void *p = rt_pixels_new(1, 1);
    assert(p != nullptr);
    rt_pixels_set(p, 0, 0, 0x00FF00FF);

    const char *pngpath = "/tmp/zanna_test_bad_adler.png";
    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_save_png(p, path) == 1);

    std::vector<uint8_t> data;
    assert(test_read_file(pngpath, data));
    size_t pos = 8;
    bool patched = false;
    while (pos + 12 <= data.size()) {
        uint32_t len = test_png_read_u32(data.data() + pos);
        if (pos + 12 + len > data.size())
            break;
        uint8_t *type = data.data() + pos + 4;
        if (memcmp(type, "IDAT", 4) == 0 && len >= 6) {
            size_t payload = pos + 8;
            data[payload + len - 1] ^= 0x01u;
            uint32_t crc = test_png_crc32(type, len + 4);
            data[pos + 8 + len + 0] = (uint8_t)(crc >> 24);
            data[pos + 8 + len + 1] = (uint8_t)(crc >> 16);
            data[pos + 8 + len + 2] = (uint8_t)(crc >> 8);
            data[pos + 8 + len + 3] = (uint8_t)crc;
            patched = true;
            break;
        }
        pos += 12 + len;
    }
    assert(patched);
    assert(test_write_file(pngpath, data));

    void *loaded = rt_pixels_load_png(path);
    assert(loaded == nullptr);
    unlink(pngpath);
    printf("test_png_load_rejects_bad_zlib_adler: PASSED\n");
}

static void test_png_truecolor_trns_transparency() {
    const char *pngpath = "/tmp/zanna_test_truecolor_trns.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 2;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));

    uint8_t trns[6] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00};
    test_png_append_chunk(png, "tRNS", trns, sizeof(trns));

    uint8_t scanline[4] = {0, 255, 0, 0};
    uint32_t adler = test_png_adler32(scanline, sizeof(scanline));
    std::vector<uint8_t> idat;
    idat.push_back(0x78);
    idat.push_back(0x01);
    idat.push_back(0x01);
    idat.push_back(0x04);
    idat.push_back(0x00);
    idat.push_back(0xFB);
    idat.push_back(0xFF);
    idat.insert(idat.end(), scanline, scanline + sizeof(scanline));
    test_png_write_u32(idat, adler);
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    void *loaded = rt_pixels_load_png(path);
    assert(loaded != nullptr);
    assert(rt_pixels_width(loaded) == 1);
    assert(rt_pixels_height(loaded) == 1);
    assert(rt_pixels_get(loaded, 0, 0) == 0xFF000000);
    unlink(pngpath);
    printf("test_png_truecolor_trns_transparency: PASSED\n");
}

static std::vector<uint8_t> test_png_stored_idat(const uint8_t *scanline, size_t len) {
    std::vector<uint8_t> idat;
    assert(len <= 65535);
    idat.push_back(0x78);
    idat.push_back(0x01);
    idat.push_back(0x01);
    idat.push_back((uint8_t)(len & 0xFFu));
    idat.push_back((uint8_t)(len >> 8));
    uint16_t nlen = (uint16_t)~(uint16_t)len;
    idat.push_back((uint8_t)(nlen & 0xFFu));
    idat.push_back((uint8_t)(nlen >> 8));
    idat.insert(idat.end(), scanline, scanline + len);
    test_png_write_u32(idat, test_png_adler32(scanline, len));
    return idat;
}

static std::vector<uint8_t> test_png_prefix(uint8_t bit_depth, uint8_t color_type) {
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);
    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = bit_depth;
    ihdr[9] = color_type;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    return png;
}

static void test_png_append_stored_scanline(std::vector<uint8_t> &png,
                                            const uint8_t *scanline,
                                            size_t len) {
    std::vector<uint8_t> idat = test_png_stored_idat(scanline, len);
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
}

static void test_png_rejects_invalid_chunk_types() {
    static const uint8_t scanline[5] = {0, 1, 2, 3, 255};

    std::vector<uint8_t> nonletter = test_png_prefix(8, 6);
    test_png_append_chunk(nonletter, "a1CD", nullptr, 0);
    test_png_append_stored_scanline(nonletter, scanline, sizeof(scanline));
    test_png_append_chunk(nonletter, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_nonletter_chunk.png", nonletter);

    std::vector<uint8_t> reserved = test_png_prefix(8, 6);
    test_png_append_chunk(reserved, "abcD", nullptr, 0);
    test_png_append_stored_scanline(reserved, scanline, sizeof(scanline));
    test_png_append_chunk(reserved, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_reserved_chunk_bit.png", reserved);

    printf("test_png_rejects_invalid_chunk_types: PASSED\n");
}

static void test_png_rejects_malformed_end() {
    static const uint8_t scanline[5] = {0, 1, 2, 3, 255};

    std::vector<uint8_t> payload_end = test_png_prefix(8, 6);
    test_png_append_stored_scanline(payload_end, scanline, sizeof(scanline));
    uint8_t payload = 0;
    test_png_append_chunk(payload_end, "IEND", &payload, 1);
    test_png_expect_rejected("/tmp/zanna_test_png_iend_payload.png", payload_end);

    std::vector<uint8_t> trailing = test_png_prefix(8, 6);
    test_png_append_stored_scanline(trailing, scanline, sizeof(scanline));
    test_png_append_chunk(trailing, "IEND", nullptr, 0);
    trailing.push_back(0);
    test_png_expect_rejected("/tmp/zanna_test_png_trailing_data.png", trailing);

    printf("test_png_rejects_malformed_end: PASSED\n");
}

static void test_png_rejects_invalid_transparency_metadata() {
    std::vector<uint8_t> gray = test_png_prefix(1, 0);
    uint8_t gray_trns[2] = {0, 2}; // outside the one-bit sample range
    test_png_append_chunk(gray, "tRNS", gray_trns, sizeof(gray_trns));
    uint8_t gray_scanline[2] = {0, 0};
    test_png_append_stored_scanline(gray, gray_scanline, sizeof(gray_scanline));
    test_png_append_chunk(gray, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_gray_trns_range.png", gray);

    std::vector<uint8_t> rgb = test_png_prefix(8, 2);
    uint8_t rgb_trns[6] = {1, 0, 0, 0, 0, 0}; // red key 256 is not an 8-bit sample
    test_png_append_chunk(rgb, "tRNS", rgb_trns, sizeof(rgb_trns));
    uint8_t rgb_scanline[4] = {0, 0, 0, 0};
    test_png_append_stored_scanline(rgb, rgb_scanline, sizeof(rgb_scanline));
    test_png_append_chunk(rgb, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_rgb_trns_range.png", rgb);

    std::vector<uint8_t> empty_palette_trns = test_png_prefix(8, 3);
    uint8_t palette[3] = {0, 0, 0};
    test_png_append_chunk(empty_palette_trns, "PLTE", palette, sizeof(palette));
    test_png_append_chunk(empty_palette_trns, "tRNS", nullptr, 0);
    uint8_t indexed_scanline[2] = {0, 0};
    test_png_append_stored_scanline(empty_palette_trns, indexed_scanline, sizeof(indexed_scanline));
    test_png_append_chunk(empty_palette_trns, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_empty_trns.png", empty_palette_trns);

    std::vector<uint8_t> wrong_order = test_png_prefix(8, 2);
    uint8_t valid_rgb_trns[6] = {0, 0, 0, 0, 0, 0};
    test_png_append_chunk(wrong_order, "tRNS", valid_rgb_trns, sizeof(valid_rgb_trns));
    test_png_append_chunk(wrong_order, "PLTE", palette, sizeof(palette));
    test_png_append_stored_scanline(wrong_order, rgb_scanline, sizeof(rgb_scanline));
    test_png_append_chunk(wrong_order, "IEND", nullptr, 0);
    test_png_expect_rejected("/tmp/zanna_test_png_plte_after_trns.png", wrong_order);

    printf("test_png_rejects_invalid_transparency_metadata: PASSED\n");
}

static void test_png_rejects_invalid_ihdr_methods() {
    const char *pngpath = "/tmp/zanna_test_invalid_ihdr_methods.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[11] = 1; // invalid PNG filter method
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_rejects_invalid_ihdr_methods: PASSED\n");
}

static void test_png_indexed_requires_palette() {
    const char *pngpath = "/tmp/zanna_test_indexed_no_plte.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 3;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    uint8_t scanline[2] = {0, 0};
    std::vector<uint8_t> idat = test_png_stored_idat(scanline, sizeof(scanline));
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_indexed_requires_palette: PASSED\n");
}

static void test_png_indexed_rejects_palette_index_out_of_range() {
    const char *pngpath = "/tmp/zanna_test_indexed_bad_index.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 3;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));

    uint8_t plte[3] = {255, 0, 0};
    test_png_append_chunk(png, "PLTE", plte, sizeof(plte));
    uint8_t scanline[2] = {0, 1};
    std::vector<uint8_t> idat = test_png_stored_idat(scanline, sizeof(scanline));
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_indexed_rejects_palette_index_out_of_range: PASSED\n");
}

static void test_png_subbyte_grayscale_trns_uses_raw_sample() {
    const char *pngpath = "/tmp/zanna_test_gray1_trns.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 1;
    ihdr[9] = 0;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    uint8_t trns[2] = {0x00, 0x01};
    test_png_append_chunk(png, "tRNS", trns, sizeof(trns));
    uint8_t scanline[2] = {0, 0x80};
    std::vector<uint8_t> idat = test_png_stored_idat(scanline, sizeof(scanline));
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    void *loaded = rt_pixels_load_png(path);
    assert(loaded != nullptr);
    assert(rt_pixels_get(loaded, 0, 0) == 0xFFFFFF00);
    unlink(pngpath);
    printf("test_png_subbyte_grayscale_trns_uses_raw_sample: PASSED\n");
}

static void test_png_rejects_trns_for_alpha_color_type() {
    const char *pngpath = "/tmp/zanna_test_rgba_trns_invalid.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 6;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    uint8_t trns[2] = {0, 0};
    test_png_append_chunk(png, "tRNS", trns, sizeof(trns));
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_rejects_trns_for_alpha_color_type: PASSED\n");
}

static void test_png_rejects_wrong_truecolor_trns_length() {
    const char *pngpath = "/tmp/zanna_test_rgb_trns_bad_len.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 2;
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    uint8_t trns[7] = {0, 0, 0, 0, 0, 0, 0};
    test_png_append_chunk(png, "tRNS", trns, sizeof(trns));
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_rejects_wrong_truecolor_trns_length: PASSED\n");
}

static void test_png_adam7_invalid_filter_rejected() {
    const char *pngpath = "/tmp/zanna_test_adam7_bad_filter.png";
    std::vector<uint8_t> png;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), signature, signature + 8);

    uint8_t ihdr[13] = {0};
    ihdr[3] = 1;
    ihdr[7] = 1;
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[12] = 1; // Adam7
    test_png_append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    uint8_t scanline[4] = {9, 255, 0, 0};
    std::vector<uint8_t> idat = test_png_stored_idat(scanline, sizeof(scanline));
    test_png_append_chunk(png, "IDAT", idat.data(), idat.size());
    test_png_append_chunk(png, "IEND", nullptr, 0);
    assert(test_write_file(pngpath, png));

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));
    assert(rt_pixels_load_png(path) == nullptr);
    unlink(pngpath);
    printf("test_png_adam7_invalid_filter_rejected: PASSED\n");
}

// ============================================================================
// Transform Tests
// ============================================================================

static void test_flip_h() {
    // Create a 3x2 image with distinct colors in each corner
    // [R G B]
    // [C M Y]
    void *p = rt_pixels_new(3, 2);
    rt_pixels_set(p, 0, 0, 0x11111111); // top-left
    rt_pixels_set(p, 1, 0, 0x22222222); // top-middle
    rt_pixels_set(p, 2, 0, 0x33333333); // top-right
    rt_pixels_set(p, 0, 1, 0x44444444); // bottom-left
    rt_pixels_set(p, 1, 1, 0x55555555); // bottom-middle
    rt_pixels_set(p, 2, 1, 0x66666666); // bottom-right

    // FlipH returns a new buffer and leaves the source unchanged
    void *result = rt_pixels_flip_h(p);
    assert(result != nullptr);
    assert(result != p);
    assert(rt_pixels_width(result) == 3);
    assert(rt_pixels_height(result) == 2);

    // Source remains unchanged
    assert(rt_pixels_get(p, 0, 0) == 0x11111111);
    assert(rt_pixels_get(p, 1, 0) == 0x22222222);
    assert(rt_pixels_get(p, 2, 0) == 0x33333333);
    assert(rt_pixels_get(p, 0, 1) == 0x44444444);
    assert(rt_pixels_get(p, 1, 1) == 0x55555555);
    assert(rt_pixels_get(p, 2, 1) == 0x66666666);

    // After horizontal flip:
    // [B G R]
    // [Y M C]
    assert(rt_pixels_get(result, 0, 0) == 0x33333333); // was top-right
    assert(rt_pixels_get(result, 1, 0) == 0x22222222); // middle unchanged
    assert(rt_pixels_get(result, 2, 0) == 0x11111111); // was top-left
    assert(rt_pixels_get(result, 0, 1) == 0x66666666); // was bottom-right
    assert(rt_pixels_get(result, 1, 1) == 0x55555555); // middle unchanged
    assert(rt_pixels_get(result, 2, 1) == 0x44444444); // was bottom-left

    printf("test_flip_h: PASSED\n");
}

static void test_flip_v() {
    // Create a 2x3 image
    void *p = rt_pixels_new(2, 3);
    rt_pixels_set(p, 0, 0, 0x11111111); // row 0
    rt_pixels_set(p, 1, 0, 0x22222222);
    rt_pixels_set(p, 0, 1, 0x33333333); // row 1
    rt_pixels_set(p, 1, 1, 0x44444444);
    rt_pixels_set(p, 0, 2, 0x55555555); // row 2
    rt_pixels_set(p, 1, 2, 0x66666666);

    // FlipV returns a new buffer and leaves the source unchanged
    void *result = rt_pixels_flip_v(p);
    assert(result != nullptr);
    assert(result != p);
    assert(rt_pixels_width(result) == 2);
    assert(rt_pixels_height(result) == 3);

    // Source remains unchanged
    assert(rt_pixels_get(p, 0, 0) == 0x11111111);
    assert(rt_pixels_get(p, 1, 0) == 0x22222222);
    assert(rt_pixels_get(p, 0, 1) == 0x33333333);
    assert(rt_pixels_get(p, 1, 1) == 0x44444444);
    assert(rt_pixels_get(p, 0, 2) == 0x55555555);
    assert(rt_pixels_get(p, 1, 2) == 0x66666666);

    // After vertical flip, row 0 becomes row 2, row 2 becomes row 0
    assert(rt_pixels_get(result, 0, 0) == 0x55555555); // was row 2
    assert(rt_pixels_get(result, 1, 0) == 0x66666666);
    assert(rt_pixels_get(result, 0, 1) == 0x33333333); // row 1 unchanged
    assert(rt_pixels_get(result, 1, 1) == 0x44444444);
    assert(rt_pixels_get(result, 0, 2) == 0x11111111); // was row 0
    assert(rt_pixels_get(result, 1, 2) == 0x22222222);

    printf("test_flip_v: PASSED\n");
}

static void test_rotate_cw() {
    // Create a 3x2 image
    // [A B C]
    // [D E F]
    void *p = rt_pixels_new(3, 2);
    rt_pixels_set(p, 0, 0, 0xAAAAAAAA); // A
    rt_pixels_set(p, 1, 0, 0xBBBBBBBB); // B
    rt_pixels_set(p, 2, 0, 0xCCCCCCCC); // C
    rt_pixels_set(p, 0, 1, 0xDDDDDDDD); // D
    rt_pixels_set(p, 1, 1, 0xEEEEEEEE); // E
    rt_pixels_set(p, 2, 1, 0xFFFFFFFF); // F

    void *rotated = rt_pixels_rotate_cw(p);
    assert(rotated != nullptr);
    // After 90 CW rotation, dimensions swap: 3x2 -> 2x3
    assert(rt_pixels_width(rotated) == 2);
    assert(rt_pixels_height(rotated) == 3);

    // After 90 CW:
    // [D A]
    // [E B]
    // [F C]
    assert(rt_pixels_get(rotated, 0, 0) == 0xDDDDDDDD); // D
    assert(rt_pixels_get(rotated, 1, 0) == 0xAAAAAAAA); // A
    assert(rt_pixels_get(rotated, 0, 1) == 0xEEEEEEEE); // E
    assert(rt_pixels_get(rotated, 1, 1) == 0xBBBBBBBB); // B
    assert(rt_pixels_get(rotated, 0, 2) == 0xFFFFFFFF); // F
    assert(rt_pixels_get(rotated, 1, 2) == 0xCCCCCCCC); // C

    printf("test_rotate_cw: PASSED\n");
}

static void test_rotate_ccw() {
    // Create a 3x2 image
    // [A B C]
    // [D E F]
    void *p = rt_pixels_new(3, 2);
    rt_pixels_set(p, 0, 0, 0xAAAAAAAA); // A
    rt_pixels_set(p, 1, 0, 0xBBBBBBBB); // B
    rt_pixels_set(p, 2, 0, 0xCCCCCCCC); // C
    rt_pixels_set(p, 0, 1, 0xDDDDDDDD); // D
    rt_pixels_set(p, 1, 1, 0xEEEEEEEE); // E
    rt_pixels_set(p, 2, 1, 0xFFFFFFFF); // F

    void *rotated = rt_pixels_rotate_ccw(p);
    assert(rotated != nullptr);
    // After 90 CCW rotation, dimensions swap: 3x2 -> 2x3
    assert(rt_pixels_width(rotated) == 2);
    assert(rt_pixels_height(rotated) == 3);

    // After 90 CCW:
    // [C F]
    // [B E]
    // [A D]
    assert(rt_pixels_get(rotated, 0, 0) == 0xCCCCCCCC); // C
    assert(rt_pixels_get(rotated, 1, 0) == 0xFFFFFFFF); // F
    assert(rt_pixels_get(rotated, 0, 1) == 0xBBBBBBBB); // B
    assert(rt_pixels_get(rotated, 1, 1) == 0xEEEEEEEE); // E
    assert(rt_pixels_get(rotated, 0, 2) == 0xAAAAAAAA); // A
    assert(rt_pixels_get(rotated, 1, 2) == 0xDDDDDDDD); // D

    printf("test_rotate_ccw: PASSED\n");
}

static void test_rotate_180() {
    // Create a 3x2 image
    // [A B C]
    // [D E F]
    void *p = rt_pixels_new(3, 2);
    rt_pixels_set(p, 0, 0, 0xAAAAAAAA); // A
    rt_pixels_set(p, 1, 0, 0xBBBBBBBB); // B
    rt_pixels_set(p, 2, 0, 0xCCCCCCCC); // C
    rt_pixels_set(p, 0, 1, 0xDDDDDDDD); // D
    rt_pixels_set(p, 1, 1, 0xEEEEEEEE); // E
    rt_pixels_set(p, 2, 1, 0xFFFFFFFF); // F

    void *rotated = rt_pixels_rotate_180(p);
    assert(rotated != nullptr);
    // After 180 rotation, dimensions stay same
    assert(rt_pixels_width(rotated) == 3);
    assert(rt_pixels_height(rotated) == 2);

    // After 180:
    // [F E D]
    // [C B A]
    assert(rt_pixels_get(rotated, 0, 0) == 0xFFFFFFFF); // F
    assert(rt_pixels_get(rotated, 1, 0) == 0xEEEEEEEE); // E
    assert(rt_pixels_get(rotated, 2, 0) == 0xDDDDDDDD); // D
    assert(rt_pixels_get(rotated, 0, 1) == 0xCCCCCCCC); // C
    assert(rt_pixels_get(rotated, 1, 1) == 0xBBBBBBBB); // B
    assert(rt_pixels_get(rotated, 2, 1) == 0xAAAAAAAA); // A

    printf("test_rotate_180: PASSED\n");
}

static void test_rotate_positive_90_is_clockwise() {
    void *p = rt_pixels_new(3, 2);
    rt_pixels_set(p, 0, 0, 0xAAAAAAAA);
    rt_pixels_set(p, 1, 0, 0xBBBBBBBB);
    rt_pixels_set(p, 2, 0, 0xCCCCCCCC);
    rt_pixels_set(p, 0, 1, 0xDDDDDDDD);
    rt_pixels_set(p, 1, 1, 0xEEEEEEEE);
    rt_pixels_set(p, 2, 1, 0xFFFFFFFF);

    void *rotated = rt_pixels_rotate(p, 90.0);
    assert(rotated != nullptr);
    assert(rt_pixels_width(rotated) == 2);
    assert(rt_pixels_height(rotated) == 3);
    assert(rt_pixels_get(rotated, 0, 0) == 0xDDDDDDDD);
    assert(rt_pixels_get(rotated, 1, 0) == 0xAAAAAAAA);
    assert(rt_pixels_get(rotated, 0, 2) == 0xFFFFFFFF);
    assert(rt_pixels_get(rotated, 1, 2) == 0xCCCCCCCC);
    printf("test_rotate_positive_90_is_clockwise: PASSED\n");
}

static void test_rotate_single_pixel_keeps_centered_extent() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_set(p, 0, 0, 0xABCDEF80);

    void *rotated = rt_pixels_rotate(p, 45.0);
    assert(rotated != nullptr);
    assert(rt_pixels_width(rotated) == 1);
    assert(rt_pixels_height(rotated) == 1);
    assert(rt_pixels_get(rotated, 0, 0) == 0xABCDEF80);
    printf("test_rotate_single_pixel_keeps_centered_extent: PASSED\n");
}

static void test_zero_dimension_flip_v_returns_zero_dimension_copy() {
    void *p = rt_pixels_new(0, 0);
    assert(p != nullptr);
    void *flipped = rt_pixels_flip_v(p);
    assert(flipped != nullptr);
    assert(rt_pixels_width(flipped) == 0);
    assert(rt_pixels_height(flipped) == 0);
    printf("test_zero_dimension_flip_v_returns_zero_dimension_copy: PASSED\n");
}

static void test_empty_rotation_validates_angle_and_preserves_shape() {
    void *p = rt_pixels_new(0, 5);
    assert(p != nullptr);
    EXPECT_TRAP(rt_pixels_rotate(p, NAN));

    void *same = rt_pixels_rotate(p, 0.0);
    assert(same != nullptr);
    assert(rt_pixels_width(same) == 0);
    assert(rt_pixels_height(same) == 5);

    void *quarter = rt_pixels_rotate(p, 90.0);
    assert(quarter != nullptr);
    assert(rt_pixels_width(quarter) == 5);
    assert(rt_pixels_height(quarter) == 0);

    void *arbitrary = rt_pixels_rotate(p, 45.0);
    assert(arbitrary != nullptr);
    assert(rt_pixels_width(arbitrary) == 0);
    assert(rt_pixels_height(arbitrary) == 5);
    printf("test_empty_rotation_validates_angle_and_preserves_shape: PASSED\n");
}

static void test_near_cardinal_rotation_is_not_aliased() {
    void *p = rt_pixels_new(3, 2);
    rt_pixels_fill_rgba(p, 0x112233FF);
    void *rotated = rt_pixels_rotate(p, 90.0005);
    assert(rotated != nullptr);
    assert(rt_pixels_width(rotated) != 2 || rt_pixels_height(rotated) != 3);
    printf("test_near_cardinal_rotation_is_not_aliased: PASSED\n");
}

static void test_scale_and_resize_reject_nonpositive_target_dimensions() {
    void *p = rt_pixels_new(2, 2);
    assert(p != nullptr);
    assert(rt_pixels_scale(p, 0, 2) == nullptr);
    assert(rt_pixels_scale(p, 2, -1) == nullptr);
    assert(rt_pixels_resize(p, 0, 2) == nullptr);
    assert(rt_pixels_resize(p, 2, -1) == nullptr);
    printf("test_scale_and_resize_reject_nonpositive_target_dimensions: PASSED\n");
}

static void test_scale_up() {
    // Create a 2x2 image and scale to 4x4
    void *p = rt_pixels_new(2, 2);
    rt_pixels_set(p, 0, 0, 0x11111111); // top-left
    rt_pixels_set(p, 1, 0, 0x22222222); // top-right
    rt_pixels_set(p, 0, 1, 0x33333333); // bottom-left
    rt_pixels_set(p, 1, 1, 0x44444444); // bottom-right

    void *scaled = rt_pixels_scale(p, 4, 4);
    assert(scaled != nullptr);
    assert(rt_pixels_width(scaled) == 4);
    assert(rt_pixels_height(scaled) == 4);

    // Each 2x2 block should have the same color (nearest neighbor)
    // Top-left quadrant
    assert(rt_pixels_get(scaled, 0, 0) == 0x11111111);
    assert(rt_pixels_get(scaled, 1, 0) == 0x11111111);
    assert(rt_pixels_get(scaled, 0, 1) == 0x11111111);
    assert(rt_pixels_get(scaled, 1, 1) == 0x11111111);

    // Top-right quadrant
    assert(rt_pixels_get(scaled, 2, 0) == 0x22222222);
    assert(rt_pixels_get(scaled, 3, 0) == 0x22222222);
    assert(rt_pixels_get(scaled, 2, 1) == 0x22222222);
    assert(rt_pixels_get(scaled, 3, 1) == 0x22222222);

    // Bottom-left quadrant
    assert(rt_pixels_get(scaled, 0, 2) == 0x33333333);
    assert(rt_pixels_get(scaled, 1, 2) == 0x33333333);
    assert(rt_pixels_get(scaled, 0, 3) == 0x33333333);
    assert(rt_pixels_get(scaled, 1, 3) == 0x33333333);

    // Bottom-right quadrant
    assert(rt_pixels_get(scaled, 2, 2) == 0x44444444);
    assert(rt_pixels_get(scaled, 3, 2) == 0x44444444);
    assert(rt_pixels_get(scaled, 2, 3) == 0x44444444);
    assert(rt_pixels_get(scaled, 3, 3) == 0x44444444);

    printf("test_scale_up: PASSED\n");
}

static void test_scale_down() {
    // Create a 4x4 image with different colors in each quadrant
    void *p = rt_pixels_new(4, 4);

    // Fill top-left quadrant
    rt_pixels_set(p, 0, 0, 0x11111111);
    rt_pixels_set(p, 1, 0, 0x11111111);
    rt_pixels_set(p, 0, 1, 0x11111111);
    rt_pixels_set(p, 1, 1, 0x11111111);

    // Fill top-right quadrant
    rt_pixels_set(p, 2, 0, 0x22222222);
    rt_pixels_set(p, 3, 0, 0x22222222);
    rt_pixels_set(p, 2, 1, 0x22222222);
    rt_pixels_set(p, 3, 1, 0x22222222);

    // Fill bottom-left quadrant
    rt_pixels_set(p, 0, 2, 0x33333333);
    rt_pixels_set(p, 1, 2, 0x33333333);
    rt_pixels_set(p, 0, 3, 0x33333333);
    rt_pixels_set(p, 1, 3, 0x33333333);

    // Fill bottom-right quadrant
    rt_pixels_set(p, 2, 2, 0x44444444);
    rt_pixels_set(p, 3, 2, 0x44444444);
    rt_pixels_set(p, 2, 3, 0x44444444);
    rt_pixels_set(p, 3, 3, 0x44444444);

    // Scale down to 2x2
    void *scaled = rt_pixels_scale(p, 2, 2);
    assert(scaled != nullptr);
    assert(rt_pixels_width(scaled) == 2);
    assert(rt_pixels_height(scaled) == 2);

    // Each pixel should sample from the corresponding quadrant
    assert(rt_pixels_get(scaled, 0, 0) == 0x11111111);
    assert(rt_pixels_get(scaled, 1, 0) == 0x22222222);
    assert(rt_pixels_get(scaled, 0, 1) == 0x33333333);
    assert(rt_pixels_get(scaled, 1, 1) == 0x44444444);

    printf("test_scale_down: PASSED\n");
}

static void test_scale_preserves_source_endpoints() {
    void *p = rt_pixels_new(4, 1);
    rt_pixels_set(p, 0, 0, 0x111111FF);
    rt_pixels_set(p, 1, 0, 0x222222FF);
    rt_pixels_set(p, 2, 0, 0x333333FF);
    rt_pixels_set(p, 3, 0, 0x444444FF);

    void *scaled = rt_pixels_scale(p, 2, 1);
    assert(scaled != nullptr);
    assert(rt_pixels_get(scaled, 0, 0) == 0x111111FF);
    assert(rt_pixels_get(scaled, 1, 0) == 0x444444FF);
    printf("test_scale_preserves_source_endpoints: PASSED\n");
}

static int64_t pack_rgba(int r, int g, int b, int a) {
    return ((int64_t)(r & 0xFF) << 24) | ((int64_t)(g & 0xFF) << 16) | ((int64_t)(b & 0xFF) << 8) |
           (int64_t)(a & 0xFF);
}

static int channel_r(int64_t rgba) {
    return (int)((rgba >> 24) & 0xFF);
}

static int channel_g(int64_t rgba) {
    return (int)((rgba >> 16) & 0xFF);
}

static int channel_b(int64_t rgba) {
    return (int)((rgba >> 8) & 0xFF);
}

static int channel_a(int64_t rgba) {
    return (int)(rgba & 0xFF);
}

static void test_tint_multiplies_tagged_alpha() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_set(p, 0, 0, pack_rgba(100, 150, 200, 255));

    void *tinted = rt_pixels_tint(p, rt_color_rgba(255, 255, 255, 128));
    assert(tinted != nullptr);
    int64_t color = rt_pixels_get(tinted, 0, 0);
    assert(channel_r(color) == 100);
    assert(channel_g(color) == 150);
    assert(channel_b(color) == 200);
    assert(channel_a(color) >= 127 && channel_a(color) <= 128);
    printf("test_tint_multiplies_tagged_alpha: PASSED\n");
}

static void test_tint_rounds_instead_of_darkening() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_set_rgba(p, 0, 0, pack_rgba(1, 1, 1, 1));

    void *tinted = rt_pixels_tint(p, rt_color_rgba(128, 128, 128, 128));
    assert(tinted != nullptr);
    int64_t color = rt_pixels_get(tinted, 0, 0);
    assert(channel_r(color) == 1);
    assert(channel_g(color) == 1);
    assert(channel_b(color) == 1);
    assert(channel_a(color) == 1);
    printf("test_tint_rounds_instead_of_darkening: PASSED\n");
}

static void test_fused_region_transform_matches_individual_passes() {
    void *source = rt_pixels_new(4, 3);
    for (int64_t y = 0; y < 3; ++y) {
        for (int64_t x = 0; x < 4; ++x) {
            uint32_t value = (uint32_t)((20 + x * 31) << 24) | (uint32_t)((30 + y * 47) << 16) |
                             (uint32_t)((40 + x * 11 + y * 7) << 8) |
                             (uint32_t)(80 + x * 23 + y * 13);
            rt_pixels_set_rgba(source, x, y, value);
        }
    }

    int64_t tint = rt_color_rgba(160, 190, 220, 173);
    void *fused = rt_pixels_transform_region_nearest(source, 1, 0, 3, 2, 5, 4, 1, 1, tint, 129);
    assert(fused != nullptr);

    void *region = rt_pixels_new(3, 2);
    rt_pixels_copy(region, 0, 0, source, 1, 0, 3, 2);
    void *flipped_h = rt_pixels_flip_h(region);
    void *flipped_v = rt_pixels_flip_v(flipped_h);
    void *scaled = rt_pixels_scale(flipped_v, 5, 4);
    void *expected = rt_pixels_tint(scaled, tint);
    auto *expected_impl = static_cast<rt_pixels_impl *>(expected);
    for (int64_t i = 0; i < expected_impl->width * expected_impl->height; ++i) {
        uint32_t pixel = expected_impl->data[i];
        uint32_t alpha = ((pixel & 0xFFu) * 129u + 127u) / 255u;
        expected_impl->data[i] = (pixel & 0xFFFFFF00u) | alpha;
    }
    for (int64_t y = 0; y < 4; ++y) {
        for (int64_t x = 0; x < 5; ++x)
            assert(rt_pixels_get(fused, x, y) == rt_pixels_get(expected, x, y));
    }

    printf("test_fused_region_transform_matches_individual_passes: PASSED\n");
}

static int64_t bilerp_rgba_premul(
    int64_t p00, int64_t p10, int64_t p01, int64_t p11, int frac_x, int frac_y) {
    int inv_frac_x = 256 - frac_x;
    int inv_frac_y = 256 - frac_y;

    int a00 = channel_a(p00);
    int a10 = channel_a(p10);
    int a01 = channel_a(p01);
    int a11 = channel_a(p11);

    int weighted_a = a00 * inv_frac_x * inv_frac_y + a10 * frac_x * inv_frac_y +
                     a01 * inv_frac_x * frac_y + a11 * frac_x * frac_y;
    int a = (weighted_a + 32768) >> 16;
    if (a <= 0)
        return 0;

    int weighted_r = (channel_r(p00) * a00) * inv_frac_x * inv_frac_y +
                     (channel_r(p10) * a10) * frac_x * inv_frac_y +
                     (channel_r(p01) * a01) * inv_frac_x * frac_y +
                     (channel_r(p11) * a11) * frac_x * frac_y;
    int weighted_g = (channel_g(p00) * a00) * inv_frac_x * inv_frac_y +
                     (channel_g(p10) * a10) * frac_x * inv_frac_y +
                     (channel_g(p01) * a01) * inv_frac_x * frac_y +
                     (channel_g(p11) * a11) * frac_x * frac_y;
    int weighted_b = (channel_b(p00) * a00) * inv_frac_x * inv_frac_y +
                     (channel_b(p10) * a10) * frac_x * inv_frac_y +
                     (channel_b(p01) * a01) * inv_frac_x * frac_y +
                     (channel_b(p11) * a11) * frac_x * frac_y;

    int r = (weighted_r + weighted_a / 2) / weighted_a;
    int g = (weighted_g + weighted_a / 2) / weighted_a;
    int b = (weighted_b + weighted_a / 2) / weighted_a;
    return pack_rgba(r, g, b, a);
}

static int64_t average_rgba_premul(int64_t p0, int64_t p1, int64_t p2) {
    int a0 = channel_a(p0);
    int a1 = channel_a(p1);
    int a2 = channel_a(p2);
    int a = (a0 + a1 + a2 + 1) / 3;
    if (a <= 0)
        return 0;

    int sum_a = a0 + a1 + a2;
    int sum_r = channel_r(p0) * a0 + channel_r(p1) * a1 + channel_r(p2) * a2;
    int sum_g = channel_g(p0) * a0 + channel_g(p1) * a1 + channel_g(p2) * a2;
    int sum_b = channel_b(p0) * a0 + channel_b(p1) * a1 + channel_b(p2) * a2;
    int r = (sum_r + sum_a / 2) / sum_a;
    int g = (sum_g + sum_a / 2) / sum_a;
    int b = (sum_b + sum_a / 2) / sum_a;
    return pack_rgba(r, g, b, a);
}

static void test_blur_rgba_channel_order() {
    void *p = rt_pixels_new(3, 1);
    int64_t p0 = 0x10002040;
    int64_t p1 = 0x80FF8040;
    int64_t p2 = 0xF01020C0;
    rt_pixels_set(p, 0, 0, p0);
    rt_pixels_set(p, 1, 0, p1);
    rt_pixels_set(p, 2, 0, p2);

    void *blurred = rt_pixels_blur(p, 1);
    assert(blurred != nullptr);
    assert(rt_pixels_get(blurred, 1, 0) == average_rgba_premul(p0, p1, p2));

    printf("test_blur_rgba_channel_order: PASSED\n");
}

static void test_blur_alpha_aware_preserves_edge_color() {
    void *p = rt_pixels_new(2, 1);
    rt_pixels_set(p, 0, 0, pack_rgba(255, 0, 0, 255));
    rt_pixels_set(p, 1, 0, pack_rgba(0, 0, 0, 0));

    void *blurred = rt_pixels_blur(p, 1);
    assert(blurred != nullptr);

    int64_t rgba = rt_pixels_get(blurred, 0, 0);
    assert(channel_r(rgba) >= 254 && channel_r(rgba) <= 255);
    assert(channel_g(rgba) == 0);
    assert(channel_b(rgba) == 0);
    assert(channel_a(rgba) >= 127 && channel_a(rgba) <= 128);

    printf("test_blur_alpha_aware_preserves_edge_color: PASSED\n");
}

static void test_blur_does_not_quantize_between_separable_passes() {
    void *p = rt_pixels_new(2, 2);
    rt_pixels_set_rgba(p, 1, 1, pack_rgba(0, 0, 0, 5));

    void *blurred = rt_pixels_blur(p, 1);
    assert(blurred != nullptr);
    for (int64_t y = 0; y < 2; ++y) {
        for (int64_t x = 0; x < 2; ++x)
            assert(channel_a(rt_pixels_get(blurred, x, y)) == 1);
    }
    printf("test_blur_does_not_quantize_between_separable_passes: PASSED\n");
}

static uint32_t reference_blur_pixel(
    void *pixels, int64_t width, int64_t height, int64_t x, int64_t y, int64_t radius) {
    if (radius > 10)
        radius = 10;
    int64_t left = x > radius ? x - radius : 0;
    int64_t top = y > radius ? y - radius : 0;
    int64_t right = radius < width - 1 - x ? x + radius : width - 1;
    int64_t bottom = radius < height - 1 - y ? y + radius : height - 1;
    int64_t premul_r = 0;
    int64_t premul_g = 0;
    int64_t premul_b = 0;
    int64_t alpha = 0;
    int64_t count = 0;
    for (int64_t source_y = top; source_y <= bottom; source_y++) {
        for (int64_t source_x = left; source_x <= right; source_x++) {
            uint32_t pixel = static_cast<uint32_t>(rt_pixels_get(pixels, source_x, source_y));
            int64_t sample_alpha = channel_a(pixel);
            premul_r += channel_r(pixel) * sample_alpha;
            premul_g += channel_g(pixel) * sample_alpha;
            premul_b += channel_b(pixel) * sample_alpha;
            alpha += sample_alpha;
            count++;
        }
    }
    int64_t averaged_alpha = (alpha + count / 2) / count;
    if (averaged_alpha <= 0)
        return 0;
    int64_t red = (premul_r + alpha / 2) / alpha;
    int64_t green = (premul_g + alpha / 2) / alpha;
    int64_t blue = (premul_b + alpha / 2) / alpha;
    return static_cast<uint32_t>(pack_rgba(red, green, blue, averaged_alpha));
}

static void test_blur_matches_exact_2d_footprint_across_edges() {
    const int64_t dimensions[][2] = {{1, 1}, {1, 7}, {8, 1}, {2, 2}, {3, 5}, {7, 4}, {11, 12}};
    const int64_t radii[] = {1, 2, 5, 10, 17};
    uint32_t random_state = UINT32_C(0x9e3779b9);

    for (const auto &dimension : dimensions) {
        int64_t width = dimension[0];
        int64_t height = dimension[1];
        void *pixels = rt_pixels_new(width, height);
        assert(pixels != nullptr);
        for (int64_t y = 0; y < height; y++) {
            for (int64_t x = 0; x < width; x++) {
                random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
                rt_pixels_set_rgba(pixels, x, y, random_state);
            }
        }

        for (int64_t radius : radii) {
            void *blurred = rt_pixels_blur(pixels, radius);
            assert(blurred != nullptr);
            for (int64_t y = 0; y < height; y++) {
                for (int64_t x = 0; x < width; x++) {
                    uint32_t expected = reference_blur_pixel(pixels, width, height, x, y, radius);
                    assert(static_cast<uint32_t>(rt_pixels_get(blurred, x, y)) == expected);
                }
            }
        }
    }
    printf("test_blur_matches_exact_2d_footprint_across_edges: PASSED\n");
}

static void test_opaque_blur_reuses_alpha_classification() {
    void *p = rt_pixels_new(2, 2);
    rt_pixels_fill_rgba(p, pack_rgba(10, 20, 30, 255));
    auto *source = static_cast<rt_pixels_impl *>(p);
    assert(rt_pixels_alpha_classification_cached(source) == RT_PIXELS_ALPHA_OPAQUE);

    void *blurred = rt_pixels_blur(p, 1);
    assert(blurred != nullptr);
    auto *impl = static_cast<rt_pixels_impl *>(blurred);
    assert(impl->alpha_scan_valid == 1);
    assert(rt_pixels_alpha_classification_cached(impl) == RT_PIXELS_ALPHA_OPAQUE);
    assert(impl->alpha_classification_scan_count == 0);
    printf("test_opaque_blur_reuses_alpha_classification: PASSED\n");
}

static void test_blur_zero_returns_exact_copy() {
    void *p = rt_pixels_new(2, 2);
    rt_pixels_set(p, 0, 0, 0x11223344);
    rt_pixels_set(p, 1, 0, 0x55667788);
    rt_pixels_set(p, 0, 1, 0x99AABBCC);
    rt_pixels_set(p, 1, 1, 0xDDEEFF00);

    void *blurred = rt_pixels_blur(p, 0);
    assert(blurred != nullptr);
    assert(blurred != p);
    assert(rt_pixels_width(blurred) == 2);
    assert(rt_pixels_height(blurred) == 2);
    assert(rt_pixels_get(blurred, 0, 0) == 0x11223344);
    assert(rt_pixels_get(blurred, 1, 0) == 0x55667788);
    assert(rt_pixels_get(blurred, 0, 1) == (int64_t)0x99AABBCC);
    assert(rt_pixels_get(blurred, 1, 1) == (int64_t)0xDDEEFF00);

    printf("test_blur_zero_returns_exact_copy: PASSED\n");
}

static void test_blur_empty_image_returns_empty_pixels() {
    void *p = rt_pixels_new(0, 0);
    void *blurred = rt_pixels_blur(p, 1);
    assert(blurred != nullptr);
    assert(rt_pixels_width(blurred) == 0);
    assert(rt_pixels_height(blurred) == 0);
    printf("test_blur_empty_image_returns_empty_pixels: PASSED\n");
}

static void test_rotate_nonfinite_traps() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_set(p, 0, 0, 0x12345678);
    EXPECT_TRAP(rt_pixels_rotate(p, INFINITY));
    printf("test_rotate_nonfinite_traps: PASSED\n");
}

static void test_resize_rgba_channel_order() {
    void *p = rt_pixels_new(2, 2);
    int64_t p00 = 0x10305070;
    int64_t p10 = 0x90B0D0F0;
    int64_t p01 = 0x20406080;
    int64_t p11 = 0xA0C0E000;

    rt_pixels_set(p, 0, 0, p00);
    rt_pixels_set(p, 1, 0, p10);
    rt_pixels_set(p, 0, 1, p01);
    rt_pixels_set(p, 1, 1, p11);

    void *resized = rt_pixels_resize(p, 3, 3);
    assert(resized != nullptr);
    assert(rt_pixels_get(resized, 1, 1) == bilerp_rgba_premul(p00, p10, p01, p11, 128, 128));

    printf("test_resize_rgba_channel_order: PASSED\n");
}

static void test_resize_alpha_aware_preserves_edge_color() {
    void *p = rt_pixels_new(2, 1);
    rt_pixels_set(p, 0, 0, pack_rgba(255, 0, 0, 255));
    rt_pixels_set(p, 1, 0, pack_rgba(0, 0, 0, 0));

    void *resized = rt_pixels_resize(p, 3, 1);
    assert(resized != nullptr);

    int64_t rgba = rt_pixels_get(resized, 1, 0);
    assert(channel_r(rgba) == 255);
    assert(channel_g(rgba) == 0);
    assert(channel_b(rgba) == 0);
    assert(channel_a(rgba) >= 127 && channel_a(rgba) <= 128);

    printf("test_resize_alpha_aware_preserves_edge_color: PASSED\n");
}

static void test_resize_preserves_source_endpoints() {
    void *p = rt_pixels_new(4, 1);
    rt_pixels_set(p, 0, 0, pack_rgba(10, 0, 0, 255));
    rt_pixels_set(p, 1, 0, pack_rgba(80, 0, 0, 255));
    rt_pixels_set(p, 2, 0, pack_rgba(160, 0, 0, 255));
    rt_pixels_set(p, 3, 0, pack_rgba(250, 0, 0, 255));

    void *resized = rt_pixels_resize(p, 2, 1);
    assert(resized != nullptr);
    assert(rt_pixels_get(resized, 0, 0) == pack_rgba(10, 0, 0, 255));
    assert(rt_pixels_get(resized, 1, 0) == pack_rgba(250, 0, 0, 255));
    printf("test_resize_preserves_source_endpoints: PASSED\n");
}

static void test_resize_odd_ratio_uses_rounded_area_filter() {
    void *p = rt_pixels_new(5, 1);
    rt_pixels_set_rgba(p, 0, 0, pack_rgba(0, 0, 0, 255));
    rt_pixels_set_rgba(p, 1, 0, pack_rgba(100, 0, 0, 255));
    rt_pixels_set_rgba(p, 2, 0, pack_rgba(0, 0, 0, 255));
    rt_pixels_set_rgba(p, 3, 0, pack_rgba(1, 0, 0, 255));
    rt_pixels_set_rgba(p, 4, 0, pack_rgba(1, 0, 0, 255));

    void *resized = rt_pixels_resize(p, 2, 1);
    assert(resized != nullptr);
    assert(channel_r(rt_pixels_get(resized, 0, 0)) == 50);
    assert(channel_r(rt_pixels_get(resized, 1, 0)) == 1);
    printf("test_resize_odd_ratio_uses_rounded_area_filter: PASSED\n");
}

static void test_resize_hybrid_preserves_upscaled_axis_interpolation() {
    void *p = rt_pixels_new(5, 2);
    for (int64_t x = 0; x < 5; x++) {
        rt_pixels_set_rgba(p, x, 0, pack_rgba(0, 0, 0, 255));
        rt_pixels_set_rgba(p, x, 1, pack_rgba(200, 0, 0, 255));
    }

    void *resized = rt_pixels_resize(p, 2, 3);
    assert(resized != nullptr);
    assert(channel_r(rt_pixels_get(resized, 0, 0)) == 0);
    assert(channel_r(rt_pixels_get(resized, 0, 1)) == 100);
    assert(channel_r(rt_pixels_get(resized, 0, 2)) == 200);
    assert(channel_r(rt_pixels_get(resized, 1, 1)) == 100);
    printf("test_resize_hybrid_preserves_upscaled_axis_interpolation: PASSED\n");
}

static void test_alpha_preserving_transforms_reuse_classification() {
    void *p = rt_pixels_new(2, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x112233FF);
    rt_pixels_set_rgba(p, 1, 0, 0x44556680);
    rt_pixels_impl *source = static_cast<rt_pixels_impl *>(p);
    assert(rt_pixels_alpha_classification_cached(source) == RT_PIXELS_ALPHA_FRACTIONAL);

    void *results[] = {rt_pixels_flip_h(p),
                       rt_pixels_flip_v(p),
                       rt_pixels_rotate_cw(p),
                       rt_pixels_rotate_ccw(p),
                       rt_pixels_rotate_180(p),
                       rt_pixels_scale(p, 4, 1),
                       rt_pixels_invert(p),
                       rt_pixels_grayscale(p),
                       rt_pixels_tint(p, rt_color_rgb(255, 255, 255))};
    for (void *result : results) {
        assert(result != nullptr);
        rt_pixels_impl *impl = static_cast<rt_pixels_impl *>(result);
        assert(impl->alpha_scan_valid == 1);
        assert(rt_pixels_alpha_classification_cached(impl) == RT_PIXELS_ALPHA_FRACTIONAL);
        assert(impl->alpha_classification_scan_count == 0);
    }
    printf("test_alpha_preserving_transforms_reuse_classification: PASSED\n");
}

static void test_tint_luminance_masked_keeps_handle_valid() {
    void *p = rt_pixels_new(2, 1);
    rt_pixels_set_rgba(p, 0, 0, 0xF0F0F0FF); /* bright: takes the tint */
    rt_pixels_set_rgba(p, 1, 0, 0x101010FF); /* dark: below lumLo, untouched */
    rt_pixels_impl *impl = static_cast<rt_pixels_impl *>(p);
    /* Populate the alpha-scan cache first: the handle validator rejects a
     * valid-flagged scan whose generation is stale, so an in-place mutator
     * that bumps generation without invalidating the cache bricks the
     * handle (found via the 2048x2048 team-tint path). */
    assert(rt_pixels_alpha_classification_cached(impl) == RT_PIXELS_ALPHA_OPAQUE);
    assert(impl->alpha_scan_valid == 1);

    rt_pixels_tint_luminance_masked(p, 0x2244AA, 0.5, 132, 168);

    /* The handle must still validate after the in-place tint. */
    assert(rt_pixels_checked_impl_or_null(p) != nullptr);
    assert(rt_pixels_width(p) == 2);
    /* Bright texel moved toward the tint; dark texel untouched; alpha kept. */
    uint32_t bright = (uint32_t)rt_pixels_get_rgba(p, 0, 0);
    assert(bright != 0xF0F0F0FFu);
    assert((bright & 0xFFu) == 0xFFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0x101010FFu);
    printf("test_tint_luminance_masked_keeps_handle_valid: PASSED\n");
}

static void test_recolor_masked_color_class() {
    void *p = rt_pixels_new(4, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x171D31FF); /* navy: exact ref, recolors */
    rt_pixels_set_rgba(p, 1, 0, 0x0A1228FF); /* shaded navy: recolors darker */
    rt_pixels_set_rgba(p, 2, 0, 0x3C2D1EFF); /* brown: inside tolerance but
                                              * chroma points red-ward — the
                                              * class gate must reject it */
    rt_pixels_set_rgba(p, 3, 0, 0xFBFBFBFF); /* white: outside tolerance */

    rt_pixels_recolor_masked(p, 0xC81E1E, 0x171D31, 64);

    uint32_t exact = (uint32_t)rt_pixels_get_rgba(p, 0, 0);
    uint32_t shaded = (uint32_t)rt_pixels_get_rgba(p, 1, 0);
    /* Exact ref recolors to ~target (shade == 1). */
    assert(((exact >> 24) & 0xFFu) > 0x90u);
    assert(((exact >> 8) & 0xFFu) < 0x40u);
    assert((exact & 0xFFu) == 0xFFu);
    /* Shaded navy recolors red-dominant but darker than the exact hit. */
    assert(((shaded >> 24) & 0xFFu) > ((shaded >> 8) & 0xFFu));
    assert(((shaded >> 24) & 0xFFu) < ((exact >> 24) & 0xFFu));
    /* Brown neighbor and white stay untouched. */
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0x3C2D1EFFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0xFBFBFBFFu);
    printf("test_recolor_masked_color_class: PASSED\n");
}

static void test_dilate_masked_gutter_fill() {
    /* 5x1 strip: covered red texel at x=0, uncovered black gutter after. */
    void *p = rt_pixels_new(5, 1);
    void *m = rt_pixels_new(5, 1);
    rt_pixels_set_rgba(p, 0, 0, 0xC80000FF);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);

    rt_pixels_dilate_masked(p, m, 2);

    /* Two passes grow two texels; each copies its lone covered neighbor. */
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0xC80000FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0xC80000FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0u);
    /* The mask records the growth. */
    assert((uint32_t)rt_pixels_get_rgba(m, 2, 0) == 0xFFFFFFFFu);
    assert((uint32_t)rt_pixels_get_rgba(m, 3, 0) == 0u);
    /* Dimension mismatch is a documented no-op. */
    void *wrong = rt_pixels_new(2, 2);
    rt_pixels_dilate_masked(p, wrong, 4);
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0u);
    printf("test_dilate_masked_gutter_fill: PASSED\n");
}

static void test_dilate_masked_id_map_growth() {
    /* Pins the ID-map contract the actor jersey-mask growth relies on:
     * running DilateMasked with an identity image (white = in-band,
     * zero = out-of-band) as the PIXELS argument and the full UV
     * coverage as the MASK argument must (a) never write a covered
     * texel, (b) grow white-adjacent gutters to non-zero, (c) grow
     * gutters reachable only through zero-valued covered texels to
     * zero while still stamping them into the mask. */
    void *p = rt_pixels_new(5, 1);
    void *m = rt_pixels_new(5, 1);
    rt_pixels_set_rgba(p, 1, 0, 0xFFFFFFFF); /* in-band, covered */
    /* x=2 stays zero: out-of-band but covered (pants). */
    rt_pixels_set_rgba(m, 1, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m, 2, 0, 0xFFFFFFFF);

    rt_pixels_dilate_masked(p, m, 2);

    /* Gutter beside the white texel goes white. */
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0xFFFFFFFFu);
    /* Covered zero texel is never overwritten by its white neighbor. */
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0u);
    /* Gutters grown only from the zero texel stay zero yet are stamped
     * covered — the growth front still advances through them. */
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0u);
    assert((uint32_t)rt_pixels_get_rgba(p, 4, 0) == 0u);
    assert((uint32_t)rt_pixels_get_rgba(m, 3, 0) == 0xFFFFFFFFu);
    assert((uint32_t)rt_pixels_get_rgba(m, 4, 0) == 0xFFFFFFFFu);

    /* A gutter contested by white and zero fronts averages non-zero,
     * so contested seam texels end up tint-eligible. */
    void *p2 = rt_pixels_new(3, 1);
    void *m2 = rt_pixels_new(3, 1);
    rt_pixels_set_rgba(p2, 0, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m2, 0, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m2, 2, 0, 0xFFFFFFFF);
    rt_pixels_dilate_masked(p2, m2, 1);
    assert((uint32_t)rt_pixels_get_rgba(p2, 1, 0) == 0x7F7F7F7Fu);
    printf("test_dilate_masked_id_map_growth: PASSED\n");
}

static void test_dilate_owner_exact_copy() {
    /* Owner growth copies EXACT values — a 255-vs-0 label front never
     * decays (the averaging op truncates it within a few rings), and a
     * covered texel is never overwritten. */
    void *p = rt_pixels_new(5, 1);
    void *m = rt_pixels_new(5, 1);
    rt_pixels_set_rgba(p, 1, 0, 0xFFFFFFFF); /* in-region label, covered */
    /* x=2 stays zero: out-of-region but covered. */
    rt_pixels_set_rgba(m, 1, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m, 2, 0, 0xFFFFFFFF);

    rt_pixels_dilate_owner(p, m, 2);

    /* Gutter beside the white label copies EXACT white. */
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0xFFFFFFFFu);
    /* Covered zero texel is never overwritten. */
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0u);
    /* Gutters owned by the zero region copy exact zero, stamped covered. */
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0u);
    assert((uint32_t)rt_pixels_get_rgba(p, 4, 0) == 0u);
    assert((uint32_t)rt_pixels_get_rgba(m, 3, 0) == 0xFFFFFFFFu);
    assert((uint32_t)rt_pixels_get_rgba(m, 4, 0) == 0xFFFFFFFFu);
    printf("test_dilate_owner_exact_copy: PASSED\n");
}

static void test_dilate_owner_tie_determinism() {
    /* A gutter contested by two fronts resolves by the FIXED neighbor scan
     * order — the left (dx=-1) owner wins over the right, exactly, with no
     * averaging. */
    void *p = rt_pixels_new(3, 1);
    void *m = rt_pixels_new(3, 1);
    rt_pixels_set_rgba(p, 0, 0, 0xFFFFFFFF);
    /* x=2 covered, value zero. */
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m, 2, 0, 0xFFFFFFFF);
    rt_pixels_dilate_owner(p, m, 1);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0xFFFFFFFFu);
    printf("test_dilate_owner_tie_determinism: PASSED\n");
}

static void test_dilate_owner_full_fill() {
    /* passes <= 0 runs to convergence: every texel takes its nearest
     * owner's exact color and the whole mask stamps covered. */
    void *p = rt_pixels_new(7, 1);
    void *m = rt_pixels_new(7, 1);
    rt_pixels_set_rgba(p, 0, 0, 0xC80000FF);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_dilate_owner(p, m, 0);
    for (int x = 0; x < 7; x++) {
        assert((uint32_t)rt_pixels_get_rgba(p, x, 0) == 0xC80000FFu);
        assert((uint32_t)rt_pixels_get_rgba(m, x, 0) == 0xFFFFFFFFu);
    }
    printf("test_dilate_owner_full_fill: PASSED\n");
}

static void test_dilate_owner_dim_mismatch_noop() {
    void *p = rt_pixels_new(5, 1);
    void *m = rt_pixels_new(2, 2);
    rt_pixels_set_rgba(p, 0, 0, 0xC80000FF);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_dilate_owner(p, m, 4);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0u);
    printf("test_dilate_owner_dim_mismatch_noop: PASSED\n");
}

static void test_colorize_masked_shade() {
    /* The cap case that motivated the op: authored navy (6,13,34) has
     * lum 13; with ref_lum 13 it must land EXACTLY on the target, and a
     * 2x-brighter texel doubles the target under the explicit clamp
     * (the fixed-ref op crushed the whole cap to ~0.45x target). */
    void *p = rt_pixels_new(3, 1);
    void *m = rt_pixels_new(3, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x060D22FF); /* navy, lum 13 */
    rt_pixels_set_rgba(p, 1, 0, 0x0C1A44FF); /* 2x navy, lum 26 */
    rt_pixels_set_rgba(p, 2, 0, 0x060D22FF); /* outside the mask */
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m, 1, 0, 0xFFFFFFFF);

    rt_pixels_colorize_masked(p, m, 0x1F4A36, 13, 2.8, 1.0);

    /* shade == 1.0 -> exact target (31,74,54). */
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0x1F4A36FFu);
    /* shade == 2.0 -> exactly doubled target. */
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0x3E946CFFu);
    /* Uncovered texel untouched. */
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0x060D22FFu);
    printf("test_colorize_masked_shade: PASSED\n");
}

static void test_colorize_masked_strength_and_alpha() {
    void *p = rt_pixels_new(1, 1);
    void *m = rt_pixels_new(1, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x060D2280); /* navy at alpha 0x80 */
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_colorize_masked(p, m, 0x1F4A36, 13, 2.8, 0.5);
    uint32_t out = (uint32_t)rt_pixels_get_rgba(p, 0, 0);
    /* Half blend: (6+31)/2, (13+74)/2, (34+54)/2 (integer floors). */
    assert(((out >> 24) & 0xFFu) == 18u);
    assert(((out >> 16) & 0xFFu) == 43u);
    assert(((out >> 8) & 0xFFu) == 44u);
    /* Alpha preserved. */
    assert((out & 0xFFu) == 0x80u);
    printf("test_colorize_masked_strength_and_alpha: PASSED\n");
}

static void test_colorize_masked_linear_reference_and_dark_tail() {
    /* ADR 0293. At the reference luma the linear op must land exactly on
     * the target (same contract as the byte-space op). For a texel darker
     * than the reference, the LINEAR result must be brighter than the
     * byte-space result once both are read back as sRGB bytes — the
     * byte-space multiply double-darkens after shader linearization,
     * which is the whole reason the variant exists. */
    void *p1 = rt_pixels_new(2, 1);
    void *p2 = rt_pixels_new(2, 1);
    void *m = rt_pixels_new(2, 1);
    /* A mid gray at the reference (128) and a half-dark texel (64). */
    rt_pixels_set_rgba(p1, 0, 0, 0x808080FF);
    rt_pixels_set_rgba(p1, 1, 0, 0x404040FF);
    rt_pixels_set_rgba(p2, 0, 0, 0x808080FF);
    rt_pixels_set_rgba(p2, 1, 0, 0x404040FF);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_set_rgba(m, 1, 0, 0xFFFFFFFF);

    rt_pixels_colorize_masked(p1, m, 0x1A3A6B, 128, 2.2, 1.0);
    rt_pixels_colorize_masked_linear(p2, m, 0x1A3A6B, 128, 2.2, 1.0);

    /* Reference texel: both ops land on the target (linear may round by
     * one code value per channel through the EOTF round trip). */
    uint32_t refByte = (uint32_t)rt_pixels_get_rgba(p1, 0, 0);
    uint32_t refLin = (uint32_t)rt_pixels_get_rgba(p2, 0, 0);
    assert(refByte == 0x1A3A6BFFu);
    for (int c = 1; c <= 3; c++) {
        int64_t a = (refLin >> (8 * c)) & 0xFF;
        int64_t b = (refByte >> (8 * c)) & 0xFF;
        int64_t d = a - b;
        if (d < 0)
            d = -d;
        assert(d <= 1);
    }
    /* Dark-tail texel: the linear result is strictly brighter per channel
     * than the byte-space result (green channel is the biggest, check
     * it explicitly). */
    uint32_t darkByte = (uint32_t)rt_pixels_get_rgba(p1, 1, 0);
    uint32_t darkLin = (uint32_t)rt_pixels_get_rgba(p2, 1, 0);
    assert(((darkLin >> 16) & 0xFFu) > ((darkByte >> 16) & 0xFFu));
    assert(((darkLin >> 24) & 0xFFu) >= ((darkByte >> 24) & 0xFFu));
    assert(((darkLin >> 8) & 0xFFu) > ((darkByte >> 8) & 0xFFu));
    /* Alpha untouched. */
    assert((darkLin & 0xFFu) == 0xFFu);
    printf("test_colorize_masked_linear_reference_and_dark_tail: PASSED\n");
}

static void test_colorize_masked_linear_mask_and_strength() {
    void *p = rt_pixels_new(2, 1);
    void *m = rt_pixels_new(2, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x80808080); /* alpha 0x80 */
    rt_pixels_set_rgba(p, 1, 0, 0x40404040);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF); /* only texel 0 covered */
    rt_pixels_colorize_masked_linear(p, m, 0x1A3A6B, 128, 2.2, 0.0);
    /* strength 0: covered texel unchanged (up to EOTF round trip). */
    uint32_t s0 = (uint32_t)rt_pixels_get_rgba(p, 0, 0);
    for (int c = 1; c <= 3; c++) {
        int64_t a = (s0 >> (8 * c)) & 0xFF;
        int64_t d = a - 0x80;
        if (d < 0)
            d = -d;
        assert(d <= 1);
    }
    assert((s0 & 0xFFu) == 0x80u);
    rt_pixels_colorize_masked_linear(p, m, 0x1A3A6B, 128, 2.2, 1.0);
    /* Uncovered texel untouched by both passes. */
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0x40404040u);
    printf("test_colorize_masked_linear_mask_and_strength: PASSED\n");
}

static void test_stamp_nonzero_copies_sparse_layer() {
    void *p = rt_pixels_new(3, 1);
    void *layer = rt_pixels_new(3, 1);
    rt_pixels_set_rgba(p, 0, 0, 0x101010FF);
    rt_pixels_set_rgba(p, 1, 0, 0x202020FF);
    rt_pixels_set_rgba(p, 2, 0, 0x303030FF);
    /* Layer patches only the middle texel; zero-RGB texels pass through. */
    rt_pixels_set_rgba(layer, 1, 0, 0xC8C8C8FF);
    rt_pixels_stamp_nonzero(p, layer);
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0x101010FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0xC8C8C8FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0x303030FFu);
    /* Dimension mismatch is a documented no-op. */
    void *wrong = rt_pixels_new(2, 2);
    rt_pixels_set_rgba(wrong, 0, 0, 0xFFFFFFFF);
    rt_pixels_stamp_nonzero(p, wrong);
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0x101010FFu);
    printf("test_stamp_nonzero_copies_sparse_layer: PASSED\n");
}

static void test_sparse_mutators_preserve_generation_on_noop() {
    void *p = rt_pixels_new(3, 1);
    void *m = rt_pixels_new(3, 1);
    void *layer = rt_pixels_new(3, 1);
    assert(p && m && layer);

    for (int64_t x = 0; x < 3; ++x)
        rt_pixels_set_rgba(m, x, 0, 0xFFFFFFFF);
    uint64_t p_generation = rt_pixels_generation(p);
    uint64_t m_generation = rt_pixels_generation(m);

    /* A fully covered mask has no growth frontier, so neither image changed. */
    rt_pixels_dilate_masked(p, m, 4);
    assert(rt_pixels_generation(p) == p_generation);
    assert(rt_pixels_generation(m) == m_generation);
    rt_pixels_dilate_owner(p, m, 4);
    assert(rt_pixels_generation(p) == p_generation);
    assert(rt_pixels_generation(m) == m_generation);

    /* Empty and value-identical sparse layers are also exact no-ops. */
    rt_pixels_stamp_nonzero(p, layer);
    assert(rt_pixels_generation(p) == p_generation);
    rt_pixels_set_rgba(p, 1, 0, 0xA0B0C0FF);
    rt_pixels_set_rgba(layer, 1, 0, 0xA0B0C0FF);
    p_generation = rt_pixels_generation(p);
    rt_pixels_stamp_nonzero(p, layer);
    assert(rt_pixels_generation(p) == p_generation);

    /* A real sparse replacement publishes exactly one mutation. */
    rt_pixels_set_rgba(layer, 1, 0, 0x102030FF);
    rt_pixels_stamp_nonzero(p, layer);
    assert(rt_pixels_generation(p) == p_generation + 1);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0x102030FFu);
    printf("test_sparse_mutators_preserve_generation_on_noop: PASSED\n");
}

static void test_colorize_masked_mask_scale() {
    /* A half-width mask scales proportionally: its covered left half
     * gates the pixels' left half. */
    void *p = rt_pixels_new(4, 1);
    void *m = rt_pixels_new(2, 1);
    for (int x = 0; x < 4; x++)
        rt_pixels_set_rgba(p, x, 0, 0x060D22FF);
    rt_pixels_set_rgba(m, 0, 0, 0xFFFFFFFF);
    rt_pixels_colorize_masked(p, m, 0x1F4A36, 13, 2.8, 1.0);
    assert((uint32_t)rt_pixels_get_rgba(p, 0, 0) == 0x1F4A36FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 1, 0) == 0x1F4A36FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 2, 0) == 0x060D22FFu);
    assert((uint32_t)rt_pixels_get_rgba(p, 3, 0) == 0x060D22FFu);
    printf("test_colorize_masked_mask_scale: PASSED\n");
}

// ============================================================================
// BlendPixel Tests
// ============================================================================

static void test_blend_fully_opaque() {
    void *p = rt_pixels_new(4, 4);
    // Black canvas; blend fully-opaque red (alpha=255) at (1,1)
    rt_pixels_blend_pixel(p, 1, 1, 0x00FF0000, 255);
    // Should be identical to set_rgb (fully opaque fast path)
    int64_t got = rt_pixels_get_rgb(p, 1, 1);
    assert(got == 0x00FF0000);
    printf("test_blend_fully_opaque: PASSED\n");
}

static void test_blend_opaque_normalizes_tagged_color_rgb() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_blend_pixel(p, 0, 0, rt_color_rgba(0x12, 0x34, 0x56, 0), 255);
    assert(rt_pixels_get(p, 0, 0) == 0x123456FF);
    printf("test_blend_opaque_normalizes_tagged_color_rgb: PASSED\n");
}

static void test_blend_transparent() {
    void *p = rt_pixels_new(4, 4);
    rt_pixels_fill(p, (int64_t)0xFF000000); // red opaque background
    // Blend with alpha=0 — no change expected
    rt_pixels_blend_pixel(p, 0, 0, 0x0000FF00, 0);
    int64_t got = rt_pixels_get(p, 0, 0);
    // Background (red RGBA) should be preserved
    assert((got >> 24) == 0xFF && ((got >> 16) & 0xFF) == 0 && ((got >> 8) & 0xFF) == 0);
    printf("test_blend_transparent: PASSED\n");
}

static void test_blend_50_percent() {
    void *p = rt_pixels_new(4, 4);
    // Set background pixel to opaque white (0xFFFFFFFF in RGBA)
    rt_pixels_set(p, 2, 2, (int64_t)0xFFFFFFFF);
    // Blend opaque black (0x000000) at 50% alpha
    rt_pixels_blend_pixel(p, 2, 2, 0x00000000, 128);
    // Result: ~50% grey — channels should be near 127 (within rounding ±2)
    int64_t rgba = rt_pixels_get(p, 2, 2);
    int r = (int)((rgba >> 24) & 0xFF);
    int g = (int)((rgba >> 16) & 0xFF);
    int b = (int)((rgba >> 8) & 0xFF);
    assert(r >= 125 && r <= 130);
    assert(g >= 125 && g <= 130);
    assert(b >= 125 && b <= 130);
    printf("test_blend_50_percent: PASSED\n");
}

static void test_blend_50_percent_over_transparent_keeps_source_color() {
    void *p = rt_pixels_new(1, 1);
    rt_pixels_blend_pixel(p, 0, 0, 0x00FF0000, 128);
    int64_t rgba = rt_pixels_get(p, 0, 0);
    assert(channel_r(rgba) == 255);
    assert(channel_g(rgba) == 0);
    assert(channel_b(rgba) == 0);
    assert(channel_a(rgba) >= 127 && channel_a(rgba) <= 128);
    printf("test_blend_50_percent_over_transparent_keeps_source_color: PASSED\n");
}

static void test_blend_out_of_bounds() {
    // Should silently clip — no crash
    void *p = rt_pixels_new(4, 4);
    rt_pixels_blend_pixel(p, -1, -1, 0x00FF0000, 255);
    rt_pixels_blend_pixel(p, 100, 100, 0x00FF0000, 255);
    printf("test_blend_out_of_bounds: PASSED\n");
}

int main() {
#ifdef _WIN32
    // Skip on Windows: test uses /tmp paths not available on Windows
    ZANNA_PLATFORM_SKIP("POSIX temp paths not available on Windows");
#endif
    printf("=== Zanna.Graphics.Pixels Tests ===\n\n");

    // Constructors
    test_new();
    test_new_zero_dimensions();
    test_new_negative_dimensions();
    test_pixels_reject_incomplete_or_inconsistent_inline_layouts();

    // Pixel access
    test_get_set();
    test_color_aware_setters_preserve_raw_rgba();
    test_color_getter_returns_color_compatible_value();
    test_get_out_of_bounds();
    test_set_out_of_bounds();
    test_corners();
    test_integer_helpers_cover_signed_extremes();
    test_generation_changes_only_for_content_changes();

    // Fill operations
    test_fill();
    test_clear();

    // Copy operations
    test_copy_basic();
    test_copy_clipping();
    test_copy_negative_dest();
    test_copy_overlap_forward();
    test_copy_overlap_backward();
    test_copy_extreme_coordinates_noop();
    test_clone();
    test_copy_generation_and_alpha_cache_are_exact();

    // Byte conversion
    test_to_bytes();
    test_from_bytes();
    test_from_bytes_rejects_wrong_object();
    test_round_trip();

    // Edge cases
    test_large_image();
    test_single_pixel();

    // BMP I/O
    test_bmp_save_load_roundtrip();
    test_bmp_load_invalid_path();
    test_bmp_save_null_inputs();
    test_bmp_odd_dimensions();
    test_bmp_load_rejects_invalid_header_extents();
    test_image_paths_reject_embedded_nul();
    test_png_load_rejects_bad_chunk_crc();
    test_png_load_rejects_bad_zlib_adler();
    test_png_truecolor_trns_transparency();
    test_png_rejects_invalid_ihdr_methods();
    test_png_indexed_requires_palette();
    test_png_indexed_rejects_palette_index_out_of_range();
    test_png_subbyte_grayscale_trns_uses_raw_sample();
    test_png_rejects_trns_for_alpha_color_type();
    test_png_rejects_wrong_truecolor_trns_length();
    test_png_adam7_invalid_filter_rejected();
    test_png_rejects_invalid_chunk_types();
    test_png_rejects_malformed_end();
    test_png_rejects_invalid_transparency_metadata();

    // Transforms
    test_flip_h();
    test_flip_v();
    test_rotate_cw();
    test_rotate_ccw();
    test_rotate_180();
    test_rotate_positive_90_is_clockwise();
    test_rotate_single_pixel_keeps_centered_extent();
    test_rotate_nonfinite_traps();
    test_zero_dimension_flip_v_returns_zero_dimension_copy();
    test_empty_rotation_validates_angle_and_preserves_shape();
    test_near_cardinal_rotation_is_not_aliased();
    test_scale_up();
    test_scale_down();
    test_scale_preserves_source_endpoints();
    test_scale_and_resize_reject_nonpositive_target_dimensions();
    test_tint_multiplies_tagged_alpha();
    test_tint_rounds_instead_of_darkening();
    test_fused_region_transform_matches_individual_passes();
    test_blur_zero_returns_exact_copy();
    test_blur_empty_image_returns_empty_pixels();
    test_blur_rgba_channel_order();
    test_blur_alpha_aware_preserves_edge_color();
    test_blur_does_not_quantize_between_separable_passes();
    test_blur_matches_exact_2d_footprint_across_edges();
    test_opaque_blur_reuses_alpha_classification();
    test_resize_rgba_channel_order();
    test_resize_alpha_aware_preserves_edge_color();
    test_resize_preserves_source_endpoints();
    test_resize_odd_ratio_uses_rounded_area_filter();
    test_resize_hybrid_preserves_upscaled_axis_interpolation();
    test_alpha_preserving_transforms_reuse_classification();

    // BlendPixel
    test_tint_luminance_masked_keeps_handle_valid();
    test_recolor_masked_color_class();
    test_dilate_masked_gutter_fill();
    test_dilate_masked_id_map_growth();
    test_dilate_owner_exact_copy();
    test_dilate_owner_tie_determinism();
    test_dilate_owner_full_fill();
    test_dilate_owner_dim_mismatch_noop();
    test_colorize_masked_shade();
    test_colorize_masked_linear_reference_and_dark_tail();
    test_colorize_masked_linear_mask_and_strength();
    test_colorize_masked_strength_and_alpha();
    test_colorize_masked_mask_scale();
    test_stamp_nonzero_copies_sparse_layer();
    test_sparse_mutators_preserve_generation_on_noop();
    test_blend_fully_opaque();
    test_blend_opaque_normalizes_tagged_color_rgb();
    test_blend_transparent();
    test_blend_50_percent();
    test_blend_50_percent_over_transparent_keeps_source_color();
    test_blend_out_of_bounds();

    printf("\nAll tests passed!\n");
    return 0;
}
