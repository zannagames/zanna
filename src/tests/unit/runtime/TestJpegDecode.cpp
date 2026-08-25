//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/TestJpegDecode.cpp
// Purpose: Unit tests for the baseline JPEG decoder.
// Key invariants:
//   - Decoder accepts valid baseline JPEG files
//   - Decoder rejects non-JPEG data gracefully (returns NULL)
//   - YCbCr->RGB conversion produces correct output
// Ownership/Lifetime:
//   - Test-scoped
// Links: src/runtime/graphics/2d/rt_pixels_jpeg.c,
//        src/runtime/graphics/2d/rt_pixels.h
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
const char *rt_asset_error_get_message(void);
void *rt_pixels_load_jpeg(void *path);
void *rt_pixels_load_bmp(void *path);
void *rt_pixels_load_png(void *path);
int64_t rt_pixels_width(void *pixels);
int64_t rt_pixels_height(void *pixels);
int64_t rt_pixels_get(void *pixels, int64_t x, int64_t y);
int64_t rt_pixels_save_png(void *pixels_ptr, void *path);
const uint32_t *rt_pixels_raw_buffer(void *pixels);
int rt_jpeg_decode_buffer_rgba32(const uint8_t *data,
                                 size_t len,
                                 uint32_t **out_pixels,
                                 int64_t *out_width,
                                 int64_t *out_height);
int rt_jpeg_decode_buffer_into_rgba32(
    const uint8_t *data, size_t len, uint32_t *dst_pixels, int64_t dst_width, int64_t dst_height);
void *rt_jpeg_decode_buffer(const uint8_t *data, size_t len);
void *rt_const_cstr(const char *s);
int64_t rt_obj_release_check0(void *obj);
void rt_obj_free(void *obj);
}

namespace {
std::jmp_buf g_trap_jmp;
const char *g_last_trap = nullptr;
bool g_expect_trap = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::abort();
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_last_trap = nullptr;                                                                     \
        g_expect_trap = true;                                                                      \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            g_expect_trap = false;                                                                 \
            EXPECT_TRUE(false);                                                                    \
        } else {                                                                                   \
            g_expect_trap = false;                                                                 \
            EXPECT_TRUE(g_last_trap != nullptr);                                                   \
        }                                                                                          \
    } while (0)

// Helper to load JPEG from a C string path
static void *load_jpeg(const char *path) {
    void *rts = rt_const_cstr(path);
    if (!rts)
        return nullptr;
    void *result = rt_pixels_load_jpeg(rts);
    return result;
}

// Helper to load PNG from a C string path
static void *load_png(const char *path) {
    void *rts = rt_const_cstr(path);
    if (!rts)
        return nullptr;
    void *result = rt_pixels_load_png(rts);
    return result;
}

static void free_pixels(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static void jpeg_append_segment(std::vector<uint8_t> &jpeg,
                                uint8_t marker,
                                const std::vector<uint8_t> &payload) {
    if (payload.size() > UINT16_MAX - 2u)
        std::abort();
    uint16_t length = (uint16_t)(payload.size() + 2u);
    jpeg.push_back(0xFF);
    jpeg.push_back(marker);
    jpeg.push_back((uint8_t)(length >> 8));
    jpeg.push_back((uint8_t)length);
    jpeg.insert(jpeg.end(), payload.begin(), payload.end());
}

static std::vector<uint8_t> jpeg_gray_sof(uint8_t sampling, uint8_t table_id) {
    return {8, 0, 1, 0, 1, 1, 1, sampling, table_id};
}

static void jpeg_append_exif_orientation(std::vector<uint8_t> &jpeg, uint16_t orientation) {
    std::vector<uint8_t> exif = {
        'E',  'x',  'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
        0x12, 0x01, 3,   0,   1, 0, 0,   0,   0,  0, 0, 0, 0, 0, 0, 0,
    };
    exif[24] = (uint8_t)orientation;
    exif[25] = (uint8_t)(orientation >> 8);
    jpeg_append_segment(jpeg, 0xE1, exif);
}

static std::vector<uint8_t> make_gray_jpeg(uint8_t table_id = 0,
                                           uint8_t quantizer = 1,
                                           uint8_t sampling = 0x11,
                                           uint8_t entropy = 0x3F,
                                           bool terminal_zrl = false,
                                           bool complete_dc_tree = false,
                                           bool insert_tem = false,
                                           bool insert_header_rst = false,
                                           bool duplicate_sof = false,
                                           bool trailing_orientation = false) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    if (insert_tem) {
        jpeg.push_back(0xFF);
        jpeg.push_back(0x01);
    }
    if (insert_header_rst) {
        jpeg.push_back(0xFF);
        jpeg.push_back(0xD0);
    }

    std::vector<uint8_t> dqt(65, quantizer);
    dqt[0] = table_id;
    jpeg_append_segment(jpeg, 0xDB, dqt);

    std::vector<uint8_t> sof = jpeg_gray_sof(sampling, table_id);
    jpeg_append_segment(jpeg, 0xC0, sof);
    if (duplicate_sof)
        jpeg_append_segment(jpeg, 0xC0, sof);

    std::vector<uint8_t> dht;
    dht.push_back(table_id);
    dht.push_back(complete_dc_tree ? 2 : 1);
    dht.insert(dht.end(), 15, 0);
    dht.push_back(0);
    if (complete_dc_tree)
        dht.push_back(1);

    dht.push_back((uint8_t)(0x10u | table_id));
    dht.push_back(1);
    dht.push_back(terminal_zrl ? 1 : 0);
    dht.insert(dht.end(), 14, 0);
    dht.push_back(terminal_zrl ? 0xF0 : 0x00);
    if (terminal_zrl)
        dht.push_back(0x00);
    jpeg_append_segment(jpeg, 0xC4, dht);

    std::vector<uint8_t> sos = {1, 1, (uint8_t)((table_id << 4) | table_id), 0, 63, 0};
    jpeg_append_segment(jpeg, 0xDA, sos);
    jpeg.push_back(entropy);
    if (trailing_orientation)
        jpeg_append_exif_orientation(jpeg, 6);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

// One-block grayscale JPEG whose single DC coefficient is non-zero
// (quantizer 1): the DC Huffman table's lone 1-bit code maps to the given
// category, and `entropy` carries code + magnitude bits + AC EOB.
// Reconstruction must equal dc/8 + 128 — the spec relation the IDCT
// descale bug (five-bit final shift instead of three) violated by 4x while
// the all-zero fixtures above stayed green (0/32 == 0/8).
static std::vector<uint8_t> make_dc_jpeg(uint8_t dc_category, const std::vector<uint8_t> &entropy) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};

    std::vector<uint8_t> dqt(65, 1);
    dqt[0] = 0;
    jpeg_append_segment(jpeg, 0xDB, dqt);

    std::vector<uint8_t> sof = jpeg_gray_sof(0x11, 0);
    jpeg_append_segment(jpeg, 0xC0, sof);

    std::vector<uint8_t> dht;
    dht.push_back(0); // DC table 0: one 1-bit code -> the category symbol
    dht.push_back(1);
    dht.insert(dht.end(), 15, 0);
    dht.push_back(dc_category);
    dht.push_back(0x10); // AC table 0: one 1-bit code -> EOB
    dht.push_back(1);
    dht.insert(dht.end(), 15, 0);
    dht.push_back(0x00);
    jpeg_append_segment(jpeg, 0xC4, dht);

    std::vector<uint8_t> sos = {1, 1, 0x00, 0, 63, 0};
    jpeg_append_segment(jpeg, 0xDA, sos);
    jpeg.insert(jpeg.end(), entropy.begin(), entropy.end());
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

static std::vector<uint8_t> make_three_component_jpeg(bool rgb_ids = false,
                                                      int adobe_transform = -1) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    if (adobe_transform >= 0) {
        std::vector<uint8_t> app14 = {
            'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, (uint8_t)adobe_transform};
        jpeg_append_segment(jpeg, 0xEE, app14);
    }

    std::vector<uint8_t> dqt(65, 1);
    dqt[0] = 0;
    jpeg_append_segment(jpeg, 0xDB, dqt);

    uint8_t first = rgb_ids ? (uint8_t)'R' : 2;
    uint8_t second = rgb_ids ? (uint8_t)'G' : 3;
    uint8_t third = rgb_ids ? (uint8_t)'B' : 1;
    std::vector<uint8_t> sof = {8, 0, 1, 0, 1, 3, first, 0x11, 0, second, 0x11, 0, third, 0x11, 0};
    jpeg_append_segment(jpeg, 0xC0, sof);

    std::vector<uint8_t> dht;
    dht.push_back(0);
    dht.push_back(1);
    dht.push_back(1);
    dht.insert(dht.end(), 14, 0);
    dht.push_back(0);
    dht.push_back(9);
    dht.push_back(0x10);
    dht.push_back(1);
    dht.insert(dht.end(), 15, 0);
    dht.push_back(0);
    jpeg_append_segment(jpeg, 0xC4, dht);

    std::vector<uint8_t> sos = {3, first, 0, second, 0, third, 0, 0, 63, 0};
    jpeg_append_segment(jpeg, 0xDA, sos);
    jpeg.push_back(0x0A);
    jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

static bool decode_raw(const std::vector<uint8_t> &jpeg,
                       uint32_t **pixels,
                       int64_t *width,
                       int64_t *height) {
    return rt_jpeg_decode_buffer_rgba32(jpeg.data(), jpeg.size(), pixels, width, height) != 0;
}

TEST(JpegDecodeTest, RejectNonJpeg) {
    EXPECT_TRAP((void)rt_pixels_load_jpeg(nullptr));

    // Non-existent file
    void *result = load_jpeg("/tmp/nonexistent_jpeg_test_file.jpg");
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(std::strlen(rt_asset_error_get_message()) > 0);
}

TEST(JpegDecodeTest, RejectInvalidData) {
    // Create a file with random data (not JPEG)
    const char *path = "/tmp/zanna_test_invalid.jpg";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    fwrite(garbage, 1, sizeof(garbage), f);
    fclose(f);

    void *result = load_jpeg(path);
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(std::strlen(rt_asset_error_get_message()) > 0);
    remove(path);
}

TEST(JpegDecodeTest, RoundTripViaPng) {
    // Create a known pixel pattern, save as PNG, load the PNG,
    // then verify we can at least create and load pixels correctly
    // (tests the overall pipeline, not JPEG-specific decoding since
    // we can't encode JPEG)

    // We'll create a 2x2 PNG and verify the pipeline works
    // Verify the JPEG decoder rejects a PNG file (wrong magic bytes).
    // Create a minimal file with PNG signature
    const char *path = "/tmp/zanna_jpeg_test_png_reject.png";
    FILE *fp = fopen(path, "wb");
    ASSERT_TRUE(fp != nullptr);
    // Minimal PNG signature
    const uint8_t png_sig[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0};
    fwrite(png_sig, 1, sizeof(png_sig), fp);
    fclose(fp);

    void *png_as_jpeg = load_jpeg(path);
    EXPECT_EQ(png_as_jpeg, nullptr); // JPEG decoder rejects PNG files
    remove(path);
}

TEST(JpegDecodeTest, IdctDcScaleIsSpecExact) {
    // dc=64 (category 7, bits 0 1000000, EOB 0)  -> 64/8  + 128 = 136.
    // dc=512 (category 10, bits 0 1000000000, 0) -> 512/8 + 128 = 192.
    // Two magnitudes pin the SCALE, not just the offset: any wrong final
    // shift moves both, and moves them by different amounts.
    struct DcCase {
        uint8_t category;
        std::vector<uint8_t> entropy;
        uint32_t expected;
    };

    const DcCase cases[] = {
        {7, {0x40, 0x7F}, UINT32_C(0x888888FF)},
        {10, {0x40, 0x0F}, UINT32_C(0xC0C0C0FF)},
    };
    for (const DcCase &c : cases) {
        std::vector<uint8_t> jpeg = make_dc_jpeg(c.category, c.entropy);
        uint32_t *pixels = nullptr;
        int64_t width = 0;
        int64_t height = 0;
        EXPECT_TRUE(decode_raw(jpeg, &pixels, &width, &height));
        ASSERT_TRUE(pixels != nullptr);
        EXPECT_EQ(width, 1);
        EXPECT_EQ(height, 1);
        EXPECT_EQ(pixels[0], c.expected);
        std::free(pixels);
    }
}

TEST(JpegDecodeTest, DecodesGeneratedBaselineAndAllTableIds) {
    for (uint8_t table_id = 0; table_id < 4; ++table_id) {
        std::vector<uint8_t> jpeg = make_gray_jpeg(table_id);
        uint32_t *pixels = nullptr;
        int64_t width = 0;
        int64_t height = 0;
        EXPECT_TRUE(decode_raw(jpeg, &pixels, &width, &height));
        ASSERT_TRUE(pixels != nullptr);
        EXPECT_EQ(width, 1);
        EXPECT_EQ(height, 1);
        EXPECT_EQ(pixels[0], UINT32_C(0x808080FF));
        std::free(pixels);
    }

    std::vector<uint8_t> with_tem = make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, true);
    uint32_t *pixels = nullptr;
    int64_t width = 0;
    int64_t height = 0;
    EXPECT_TRUE(decode_raw(with_tem, &pixels, &width, &height));
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(pixels[0], UINT32_C(0x808080FF));
    std::free(pixels);
}

TEST(JpegDecodeTest, MapsYcbcrRolesByComponentId) {
    // The fixture's luma block decodes to DC=256 -> 256/8 + 128 = 160
    // (0xA0). The previous expectation (0x88 = 136) had the IDCT descale
    // bug baked in (256/32 + 128) — see IdctDcScaleIsSpecExact.
    std::vector<uint8_t> jpeg = make_three_component_jpeg();
    uint32_t *pixels = nullptr;
    int64_t width = 0;
    int64_t height = 0;
    EXPECT_TRUE(decode_raw(jpeg, &pixels, &width, &height));
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(width, 1);
    EXPECT_EQ(height, 1);
    EXPECT_EQ(pixels[0], UINT32_C(0xA0A0A0FF));
    std::free(pixels);

    jpeg = make_three_component_jpeg(false, 1);
    pixels = nullptr;
    EXPECT_TRUE(decode_raw(jpeg, &pixels, &width, &height));
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(pixels[0], UINT32_C(0xA0A0A0FF));
    std::free(pixels);
}

TEST(JpegDecodeTest, RejectsRgbAndAdobeNonYcbcrTransforms) {
    for (const std::vector<uint8_t> &jpeg :
         {make_three_component_jpeg(true), make_three_component_jpeg(false, 0)}) {
        uint32_t *pixels = nullptr;
        int64_t width = 0;
        int64_t height = 0;
        EXPECT_FALSE(decode_raw(jpeg, &pixels, &width, &height));
        EXPECT_EQ(pixels, nullptr);
        EXPECT_EQ(width, 0);
        EXPECT_EQ(height, 0);
    }
}

TEST(JpegDecodeTest, RejectsMalformedTablesSamplingAndEntropy) {
    const std::vector<std::vector<uint8_t>> malformed = {
        make_gray_jpeg(0, 0),
        make_gray_jpeg(0, 1, 0x11, 0x00),
        make_gray_jpeg(0, 1, 0x11, 0x07, true),
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, true),
        make_gray_jpeg(0, 1, 0x11, 0x0F, false, false, false, false, true),
        make_gray_jpeg(0, 1, 0x21, 0x0F),
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, false, true),
    };

    for (const std::vector<uint8_t> &jpeg : malformed) {
        uint32_t *pixels = reinterpret_cast<uint32_t *>(uintptr_t{1});
        int64_t width = 17;
        int64_t height = 19;
        EXPECT_FALSE(decode_raw(jpeg, &pixels, &width, &height));
        EXPECT_EQ(pixels, nullptr);
        EXPECT_EQ(width, 0);
        EXPECT_EQ(height, 0);
    }
}

TEST(JpegDecodeTest, EarlyFailureClearsRawOutputs) {
    const uint8_t invalid[] = {0, 1, 2};
    uint32_t *pixels = reinterpret_cast<uint32_t *>(uintptr_t{1});
    int64_t width = 17;
    int64_t height = 19;
    EXPECT_EQ(rt_jpeg_decode_buffer_rgba32(invalid, sizeof(invalid), &pixels, &width, &height), 0);
    EXPECT_EQ(pixels, nullptr);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);
}

TEST(JpegDecodeTest, DirectDecodeRejectsTrailingExifOrientation) {
    std::vector<uint8_t> jpeg =
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, false, false, false, true);
    uint32_t direct_pixel = UINT32_C(0xDEADBEEF);
    EXPECT_EQ(rt_jpeg_decode_buffer_into_rgba32(jpeg.data(), jpeg.size(), &direct_pixel, 1, 1), 0);

    uint32_t *pixels = nullptr;
    int64_t width = 0;
    int64_t height = 0;
    EXPECT_TRUE(decode_raw(jpeg, &pixels, &width, &height));
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(width, 1);
    EXPECT_EQ(height, 1);
    EXPECT_EQ(pixels[0], UINT32_C(0x808080FF));
    std::free(pixels);
}

TEST(JpegDecodeTest, ManagedDecodeSupportsDirectAndOrientationFallbackPaths) {
    std::vector<uint8_t> ordinary =
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, false, false, false, false);
    void *pixels = rt_jpeg_decode_buffer(ordinary.data(), ordinary.size());
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(rt_pixels_width(pixels), 1);
    EXPECT_EQ(rt_pixels_height(pixels), 1);
    EXPECT_EQ(rt_pixels_get(pixels, 0, 0), INT64_C(0x808080FF));
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);

    std::vector<uint8_t> oriented =
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, false, false, false, true);
    pixels = rt_jpeg_decode_buffer(oriented.data(), oriented.size());
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(rt_pixels_get(pixels, 0, 0), INT64_C(0x808080FF));
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
}

TEST(JpegDecodeTest, MalformedExifCountCannotApplyOrientation) {
    std::vector<uint8_t> jpeg =
        make_gray_jpeg(0, 1, 0x11, 0x3F, false, false, false, false, false, true);
    static const uint8_t exif_signature[] = {'E', 'x', 'i', 'f'};
    auto exif = std::search(
        jpeg.begin(), jpeg.end(), exif_signature, exif_signature + sizeof(exif_signature));
    ASSERT_TRUE(exif != jpeg.end());
    size_t exif_offset = (size_t)(exif - jpeg.begin());
    ASSERT_TRUE(exif_offset + 16u <= jpeg.size());
    jpeg[exif_offset + 14u] = 0xFF;
    jpeg[exif_offset + 15u] = 0xFF;

    uint32_t direct_pixel = 0;
    EXPECT_EQ(rt_jpeg_decode_buffer_into_rgba32(jpeg.data(), jpeg.size(), &direct_pixel, 1, 1), 1);
    EXPECT_EQ(direct_pixel, UINT32_C(0x808080FF));
}

int main() {
    return zanna_test::run_all_tests();
}
