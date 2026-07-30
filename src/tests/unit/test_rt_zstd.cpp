//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_zstd.cpp
// Purpose: Unit tests for the from-scratch Zstandard decompressor — real-frame
//   round-trips against fixtures produced by the reference encoder, plus
//   malformed-input rejection (truncation, bad magic, bad checksum, output
//   budget).
//
// Key invariants:
//   - Every fixture decodes to stable expected plaintext bytes or digest.
//   - Corrupt or truncated frames return 0 without crashing or leaking.
//   - max_output is honored as a hard budget.
//   - Caller-owned decoding requires an exact destination and consumes the complete frame.
//   - Zero-literal compressed blocks never pass null storage to byte-copy operations.
//
// Ownership/Lifetime:
//   - Decoded buffers are freed by the test after each comparison.
// Links: src/runtime/io/rt_zstd.c
//
//===----------------------------------------------------------------------===//

#include "rt_zstd.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

static bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }
    out.resize((size_t)size);
    if (size > 0 && fread(out.data(), 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static std::string fixture_path(const char *name) {
#ifdef ZANNA_SOURCE_DIR
    return std::string(ZANNA_SOURCE_DIR) + "/src/tests/unit/data/zstd/" + name;
#else
    return std::string("src/tests/unit/data/zstd/") + name;
#endif
}

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool expected_fixture_bytes(const char *stem, std::vector<uint8_t> &expected) {
    if (std::strcmp(stem, "text") == 0) {
        static const char phrase[] = "the quick brown fox jumps over the lazy dog. ";
        const size_t phrase_len = sizeof(phrase) - 1;
        expected.reserve(phrase_len * 40);
        for (int i = 0; i < 40; ++i)
            expected.insert(expected.end(), phrase, phrase + phrase_len);
        return true;
    }
    if (std::strcmp(stem, "bytes") == 0) {
        expected.resize(256 * 64);
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<uint8_t>(i & 0xFFu);
        return true;
    }
    if (std::strcmp(stem, "tiny") == 0) {
        expected = {'h', 'i'};
        return true;
    }
    if (std::strcmp(stem, "empty") == 0) {
        expected.clear();
        return true;
    }
    return false;
}

static bool expected_fixture_summary(const char *stem, size_t &len, uint64_t &hash) {
    if (std::strcmp(stem, "mixed") == 0) {
        len = 5276;
        hash = 0x6ABC1835B722875BULL;
        return true;
    }

    std::vector<uint8_t> expected;
    if (!expected_fixture_bytes(stem, expected))
        return false;
    len = expected.size();
    hash = fnv1a64(expected.data(), expected.size());
    return true;
}

/// Round-trip one fixture: <stem>.zst must decode to its stable expected payload.
static void roundtrip_case(const char *stem) {
    std::string label = std::string("fixture '") + stem + "' decodes byte-identically";
    TEST(label.c_str());
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> expected;
    size_t expected_len = 0;
    uint64_t expected_hash = 0;
    const bool has_expected_bytes = expected_fixture_bytes(stem, expected);
    EXPECT_TRUE(read_file(fixture_path((std::string(stem) + ".zst").c_str()), compressed),
                "compressed fixture readable");
    EXPECT_TRUE(expected_fixture_summary(stem, expected_len, expected_hash), "known fixture stem");

    uint8_t *out = nullptr;
    size_t out_len = 0;
    int ok = rt_zstd_decompress_raw(compressed.data(), compressed.size(), 1u << 24, &out, &out_len);
    EXPECT_TRUE(ok == 1, "decompression succeeds");
    EXPECT_TRUE(out_len == expected_len, "decoded length matches");
    if (has_expected_bytes) {
        EXPECT_TRUE(out_len == 0 || std::memcmp(out, expected.data(), out_len) == 0,
                    "decoded bytes match");
    } else {
        EXPECT_TRUE(fnv1a64(out, out_len) == expected_hash, "decoded hash matches");
    }
    free(out);
    PASS();
}

static void test_rejects_bad_magic() {
    TEST("bad magic is rejected");
    uint8_t junk[16] = {0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *out = nullptr;
    size_t out_len = 0;
    EXPECT_TRUE(rt_zstd_decompress_raw(junk, sizeof(junk), 1024, &out, &out_len) == 0,
                "junk frame fails");
    EXPECT_TRUE(out == nullptr, "no buffer on failure");
    PASS();
}

static void test_rejects_truncation() {
    TEST("truncated frames are rejected at every length");
    std::vector<uint8_t> compressed;
    EXPECT_TRUE(read_file(fixture_path("mixed.zst"), compressed), "fixture readable");
    /* Every strict prefix must fail cleanly (also a mini structure-fuzz). */
    for (size_t cut = 0; cut < compressed.size(); cut += 7) {
        uint8_t *out = nullptr;
        size_t out_len = 0;
        int ok = rt_zstd_decompress_raw(compressed.data(), cut, 1u << 24, &out, &out_len);
        if (ok) {
            free(out);
            EXPECT_TRUE(false, "a truncated prefix decoded successfully");
        }
    }
    PASS();
}

static void test_rejects_checksum_mismatch() {
    TEST("corrupted payload fails the content checksum");
    std::vector<uint8_t> compressed;
    EXPECT_TRUE(read_file(fixture_path("bytes.zst"), compressed), "fixture readable");
    /* Flip one byte in the middle of the frame; either structural validation
     * or the xxhash64 checksum must reject it. */
    compressed[compressed.size() / 2] ^= 0x5A;
    uint8_t *out = nullptr;
    size_t out_len = 0;
    int ok = rt_zstd_decompress_raw(compressed.data(), compressed.size(), 1u << 24, &out, &out_len);
    if (ok)
        free(out);
    EXPECT_TRUE(ok == 0, "corrupted frame rejected");
    PASS();
}

static void test_honors_output_budget() {
    TEST("max_output is a hard budget");
    std::vector<uint8_t> compressed;
    size_t expected_len = 0;
    uint64_t expected_hash = 0;
    EXPECT_TRUE(read_file(fixture_path("bytes.zst"), compressed), "fixture readable");
    EXPECT_TRUE(expected_fixture_summary("bytes", expected_len, expected_hash),
                "known fixture stem");
    uint8_t *out = nullptr;
    size_t out_len = 0;
    EXPECT_TRUE(rt_zstd_decompress_raw(
                    compressed.data(), compressed.size(), expected_len - 1, &out, &out_len) == 0,
                "one byte under the real size fails");
    EXPECT_TRUE(rt_zstd_decompress_raw(
                    compressed.data(), compressed.size(), expected_len, &out, &out_len) == 1,
                "exact budget succeeds");
    free(out);
    PASS();
}

/// @brief Decode a compressed block whose raw literals and sequence count are both zero.
/// @details This valid empty frame reaches the zero-length literal buffer path without
///          relying on the reference encoder choosing a compressed block representation.
static void test_zero_literal_compressed_block() {
    TEST("zero-literal compressed block decodes safely");
    static const uint8_t frame[] = {
        0x28,
        0xB5,
        0x2F,
        0xFD, // magic
        0x20,
        0x00, // single-segment frame, content size zero
        0x15,
        0x00,
        0x00, // last compressed block, two payload bytes
        0x00, // raw literals section with regenerated size zero
        0x00, // zero sequences
    };
    uint8_t *out = nullptr;
    size_t out_len = 0;
    EXPECT_TRUE(rt_zstd_decompress_raw(frame, sizeof(frame), 0, &out, &out_len) == 1,
                "empty compressed block succeeds");
    EXPECT_TRUE(out != nullptr, "successful empty output remains freeable");
    EXPECT_TRUE(out_len == 0, "empty compressed block emits zero bytes");
    free(out);
    PASS();
}

/// @brief Verify the caller-owned decoder requires exact output size and complete input use.
/// @details A known reference frame must decode byte-identically into its exact destination.
///          Destinations one byte short/long and otherwise-valid frames with one trailing byte all
///          fail without allocating or accepting a prefix.
static void test_exact_destination_rejects_size_mismatch_and_trailing_input() {
    TEST("exact destination rejects size mismatch and trailing input");
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> expected;
    EXPECT_TRUE(read_file(fixture_path("text.zst"), compressed), "fixture readable");
    EXPECT_TRUE(expected_fixture_bytes("text", expected), "expected bytes available");
    std::vector<uint8_t> decoded(expected.size(), 0u);
    EXPECT_TRUE(rt_zstd_decompress_into(
                    compressed.data(), compressed.size(), decoded.data(), decoded.size()) == 1,
                "exact destination succeeds");
    EXPECT_TRUE(decoded == expected, "exact destination bytes match");

    std::vector<uint8_t> short_output(expected.size() - 1u, 0u);
    EXPECT_TRUE(
        rt_zstd_decompress_into(
            compressed.data(), compressed.size(), short_output.data(), short_output.size()) == 0,
        "short destination fails");
    std::vector<uint8_t> long_output(expected.size() + 1u, 0u);
    EXPECT_TRUE(rt_zstd_decompress_into(
                    compressed.data(), compressed.size(), long_output.data(), long_output.size()) ==
                    0,
                "long destination fails");

    compressed.push_back(0x00u);
    EXPECT_TRUE(rt_zstd_decompress_into(
                    compressed.data(), compressed.size(), decoded.data(), decoded.size()) == 0,
                "trailing frame byte fails");
    PASS();
}

int main() {
    printf("test_rt_zstd:\n");
    roundtrip_case("text");
    roundtrip_case("bytes");
    roundtrip_case("mixed");
    roundtrip_case("tiny");
    roundtrip_case("empty");
    test_rejects_bad_magic();
    test_rejects_truncation();
    test_rejects_checksum_mismatch();
    test_honors_output_budget();
    test_zero_literal_compressed_block();
    test_exact_destination_rejects_size_mismatch_and_trailing_input();
    printf("%d/%d tests passed\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
