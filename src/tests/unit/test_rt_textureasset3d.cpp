//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_textureasset3d.cpp
// Purpose: Dedicated unit coverage for the TextureAsset3D KTX2 codec family
//          (previously exercised only by opt-in clang-only fuzzing): container
//          loads across formats, metadata readback, CPU fallback presence,
//          and recoverable rejection of truncated or corrupted payloads.
// Key invariants:
//   - Loaders return NULL (never trap, never crash) for malformed input.
//   - Loaded assets report positive dimensions, at least one mip, and a
//     stable non-empty format token.
// Ownership/Lifetime:
//   - Test-scoped runtime objects live for the process; the harness never
//     frees them explicitly.
// Links: src/runtime/graphics/3d/assets/rt_textureasset3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "rt_textureasset3d.h"
}

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

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

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/// Read checked-in fixtures from the configured source root, including when
/// ZANNA_BUILD_DIR is outside the repository. Retain standalone-run fallbacks.
static bool read_fixture(const char *name, std::vector<uint8_t> &out) {
    const char *prefixes[] = {
#ifdef ZANNA_TEST_SOURCE_DIR
        ZANNA_TEST_SOURCE_DIR "/src/tests/fixtures/runtime/textures/",
#endif
        "src/tests/fixtures/runtime/textures/",
        "../../../src/tests/fixtures/runtime/textures/",
        "../../src/tests/fixtures/runtime/textures/",
    };
    for (const char *prefix : prefixes) {
        std::string path = std::string(prefix) + name;
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            fclose(f);
            return false;
        }
        out.resize((size_t)size);
        size_t got = fread(out.data(), 1, (size_t)size, f);
        fclose(f);
        return got == (size_t)size;
    }
    return false;
}

/// Load one fixture through the permissive in-memory loader and validate the
/// shared metadata contract.
static void expect_fixture_loads(const char *name, bool expect_pixels_fallback) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(read_fixture(name, bytes), "fixture readable");
    void *asset = rt_textureasset3d_load_ktx2_memory(bytes.data(), bytes.size());
    EXPECT_TRUE(asset != nullptr, "container loads");
    EXPECT_TRUE(rt_textureasset3d_get_width(asset) > 0, "width positive");
    EXPECT_TRUE(rt_textureasset3d_get_height(asset) > 0, "height positive");
    EXPECT_TRUE(rt_textureasset3d_get_mip_count(asset) >= 1, "at least one mip");
    rt_string format = rt_textureasset3d_get_format(asset);
    const char *format_cs = format ? rt_string_cstr(format) : "";
    EXPECT_TRUE(format_cs && format_cs[0] != '\0', "format token non-empty");
    EXPECT_TRUE(std::strcmp(format_cs, "unknown") != 0, "format token classified");
    EXPECT_TRUE(rt_textureasset3d_get_degraded(asset) == 0,
                "well-formed fixture is not the degraded checker");
    if (expect_pixels_fallback)
        EXPECT_TRUE(rt_textureasset3d_get_pixels(asset) != nullptr, "CPU pixels available");
    PASS();
}

static void test_rgba8_fixture() {
    TEST("rgba8 container loads with CPU pixels");
    expect_fixture_loads("checker_rgba8.ktx2", true);
}

static void test_bc7_fixture() {
    TEST("BC7 container loads");
    expect_fixture_loads("marker_bc7.ktx2", false);
}

static void test_bc6h_fixture() {
    TEST("BC6H container loads");
    expect_fixture_loads("quad_bc6h.ktx2", false);
}

static void test_etc1s_fixture() {
    TEST("ETC1S/BasisLZ container loads");
    expect_fixture_loads("quad_etc1s.ktx2", false);
}

static void test_uastc_fixture() {
    TEST("UASTC container loads");
    expect_fixture_loads("quad_uastc.ktx2", false);
}

static void test_empty_payload_rejected() {
    TEST("zero-size payload is rejected recoverably");
    /* NULL data is a programmer-error trap by contract; zero size with a
     * valid pointer is the recoverable NULL-return path exercised here. */
    uint8_t byte = 0;
    EXPECT_TRUE(rt_textureasset3d_load_ktx2_memory(&byte, 0) == nullptr, "zero-size rejected");
    PASS();
}

static void test_truncations_rejected() {
    TEST("every fixture rejects truncation at all lengths");
    const char *names[] = {"checker_rgba8.ktx2",
                           "marker_bc7.ktx2",
                           "quad_bc6h.ktx2",
                           "quad_etc1s.ktx2",
                           "quad_uastc.ktx2"};
    for (const char *name : names) {
        std::vector<uint8_t> bytes;
        EXPECT_TRUE(read_fixture(name, bytes), "fixture readable");
        /* The fixtures are tiny (<600 bytes), so sweep every prefix length:
         * a truncated container must load as NULL, never crash or trap. */
        for (size_t len = 1; len < bytes.size(); ++len) {
            void *asset = rt_textureasset3d_load_ktx2_memory(bytes.data(), len);
            if (asset != nullptr) {
                printf("FAIL: %s accepted a %zu-byte truncation\n", name, len);
                return;
            }
        }
    }
    PASS();
}

static void test_corrupt_magic_rejected() {
    TEST("corrupted identifier bytes are rejected");
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(read_fixture("checker_rgba8.ktx2", bytes), "fixture readable");
    std::vector<uint8_t> corrupt = bytes;
    corrupt[0] ^= 0xFF;
    corrupt[4] ^= 0xFF;
    EXPECT_TRUE(rt_textureasset3d_load_ktx2_memory(corrupt.data(), corrupt.size()) == nullptr,
                "flipped magic rejected");
    PASS();
}

static void test_corrupt_body_never_crashes() {
    TEST("single-byte body corruption never crashes the loader");
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(read_fixture("quad_etc1s.ktx2", bytes), "fixture readable");
    /* Flip each byte in turn past the identifier: the loader may accept
     * (payload bits) or reject (structural fields) but must stay memory-safe
     * and return either NULL or a valid asset. */
    for (size_t i = 12; i < bytes.size(); ++i) {
        std::vector<uint8_t> corrupt = bytes;
        corrupt[i] ^= 0xFF;
        void *asset = rt_textureasset3d_load_ktx2_memory(corrupt.data(), corrupt.size());
        if (asset) {
            EXPECT_TRUE(rt_textureasset3d_get_width(asset) > 0,
                        "accepted corruption still yields valid metadata");
        }
    }
    PASS();
}

int main() {
    printf("test_rt_textureasset3d:\n");
    test_rgba8_fixture();
    test_bc7_fixture();
    test_bc6h_fixture();
    test_etc1s_fixture();
    test_uastc_fixture();
    test_empty_payload_rejected();
    test_truncations_rejected();
    test_corrupt_magic_rejected();
    test_corrupt_body_never_crashes();
    printf("%d/%d tests passed\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
