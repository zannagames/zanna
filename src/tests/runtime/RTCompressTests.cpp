//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTCompressTests.cpp
// Purpose: Validate Zanna.IO.Compress DEFLATE/GZIP and strict zlib decoding.
// Key invariants:
//   - Round-trip compression/decompression preserves data.
//   - Strict zlib rejects malformed framing, bounds, dictionary flags, and checksums.
// Ownership/Lifetime:
//   - Every test owns and releases its runtime Bytes/String handles.
//   - Trap state is process-local test scaffolding and retains no runtime payload.
// Links: docs/zannalib/io/advanced.md,
//        docs/adr/0229-bounded-native-gzip-decoding.md
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static int g_alloc_fail_countdown = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

/// @brief Fail one selected managed allocation in decoder cleanup tests.
/// @param bytes Requested allocation size.
/// @param next Default allocator to invoke when the countdown does not fire.
/// @return Allocated storage, or NULL at the selected boundary.
static void *compress_fail_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    if (g_alloc_fail_countdown > 0 && --g_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
        assert(g_last_trap != nullptr);                                                            \
    } while (0)

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

/// @brief Get bytes data pointer
static uint8_t *get_bytes_data(void *bytes) {
    return rt_bytes_data(bytes);
}

/// @brief Get bytes length
static int64_t get_bytes_len(void *bytes) {
    return rt_bytes_len(bytes);
}

/// @brief Compare two byte arrays
static bool bytes_equal(void *a, void *b) {
    int64_t len_a = get_bytes_len(a);
    int64_t len_b = get_bytes_len(b);
    if (len_a != len_b)
        return false;
    return memcmp(get_bytes_data(a), get_bytes_data(b), len_a) == 0;
}

/// @brief Create bytes from raw data
static void *make_bytes(const uint8_t *data, size_t len) {
    void *bytes = rt_bytes_new(len);
    memcpy(get_bytes_data(bytes), data, len);
    return bytes;
}

/// @brief Create bytes from string literal
static void *make_bytes_str(const char *str) {
    size_t len = strlen(str);
    return make_bytes((const uint8_t *)str, len);
}

static void *make_invalid_bytes_object() {
    struct FakeBytes {
        int64_t len;
        uint8_t *data;
    };

    FakeBytes *bad = static_cast<FakeBytes *>(rt_obj_new_i64(RT_BYTES_CLASS_ID, sizeof(FakeBytes)));
    bad->len = 1;
    bad->data = nullptr;
    return bad;
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void *bytes_with_extra_byte(void *bytes, uint8_t extra) {
    int64_t len = get_bytes_len(bytes);
    void *out = rt_bytes_new(len + 1);
    if (len > 0)
        memcpy(get_bytes_data(out), get_bytes_data(bytes), (size_t)len);
    get_bytes_data(out)[len] = extra;
    return out;
}

//=============================================================================
// DEFLATE Tests
//=============================================================================

static void test_deflate_literals_only() {
    printf("Testing DEFLATE Literals Only (Fixed Huffman):\n");

    // Create 100 sequential bytes - no matches possible since each 3-byte
    // sequence is unique. This tests literal encoding only.
    uint8_t buffer[100];
    for (int i = 0; i < 100; i++) {
        buffer[i] = (uint8_t)i;
    }

    void *original = make_bytes(buffer, 100);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Literals-only round-trip", bytes_equal(original, decompressed));
    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_deflate_simple_match() {
    printf("Testing DEFLATE Simple Match (Fixed Huffman):\n");

    // Create data with one simple match: "ABCABC" repeated
    // This has exactly one match opportunity: at position 3, match position 0, length 3
    const char *text =
        "ABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABCABC";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Simple match round-trip", bytes_equal(original, decompressed));
    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_deflate_distance_with_extra_bits() {
    printf("Testing DEFLATE Distance with Extra Bits:\n");

    // Distance 5-6 require 1 extra bit (dist code 4-5)
    // Distance 7-8 require 1 extra bit (dist code 5)
    // Distance 9-12 require 2 extra bits (dist code 6-7)
    // Distance 25-32 require 4 extra bits (dist code 9)

    // Test distance 10 (requires 2 extra bits): 10 unique bytes then repeat
    const char *text = "0123456789"
                       "0123456789"
                       "0123456789"
                       "0123456789"
                       "0123456789"
                       "0123456789"
                       "0123456789"
                       "0123456789";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Distance with extra bits round-trip", bytes_equal(original, decompressed));
    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_deflate_distance_26() {
    printf("Testing DEFLATE Distance 26:\n");

    // Distance 26 requires 3 extra bits (dist code 9, base 25, extra 1)
    // 26 unique bytes then repeat
    const char *text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Distance 26 round-trip", bytes_equal(original, decompressed));
    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_deflate_longer_data() {
    printf("Testing DEFLATE Longer Data:\n");

    // Test sizes around length code boundaries (414 uses code 280, 415 uses code 281)
    int sizes[] = {300, 414, 415, 500, 1000};
    for (int s = 0; s < 5; s++) {
        int size = sizes[s];
        char *buffer = (char *)malloc(size);
        for (int i = 0; i < size; i++) {
            buffer[i] = 'A' + (i % 26);
        }

        void *original = make_bytes((const uint8_t *)buffer, size);
        free(buffer);
        void *compressed = rt_compress_deflate(original);
        void *decompressed = rt_compress_inflate(compressed);

        char test_name[32];
        snprintf(test_name, sizeof(test_name), "%d bytes round-trip", size);
        test_result(test_name, bytes_equal(original, decompressed));
    }
}

static void test_deflate_inflate_empty() {
    printf("Testing DEFLATE Empty:\n");

    void *empty = rt_bytes_new(0);
    void *compressed = rt_compress_deflate(empty);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Empty data round-trip", bytes_equal(empty, decompressed));
}

static void test_deflate_inflate_small() {
    printf("Testing DEFLATE Small Data:\n");

    // Small data uses stored blocks
    const char *text = "Hello, World!";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Small data round-trip", bytes_equal(original, decompressed));
    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_deflate_inflate_repeated() {
    printf("Testing DEFLATE Repeated Data:\n");

    // Data with lots of repetition - verify round-trip works
    char buffer[1000];
    for (int i = 0; i < 1000; i++) {
        buffer[i] = 'A' + (i % 26);
    }

    void *original = make_bytes((const uint8_t *)buffer, 1000);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Repeated data round-trip", bytes_equal(original, decompressed));

    printf("  Original: %lld bytes, Compressed: %lld bytes (%.1f%% ratio)\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed),
           100.0 * get_bytes_len(compressed) / get_bytes_len(original));
}

static void test_deflate_levels() {
    printf("Testing DEFLATE Levels:\n");

    // Create compressible data
    char buffer[2000];
    for (int i = 0; i < 2000; i++) {
        buffer[i] = 'A' + (i % 10);
    }
    void *original = make_bytes((const uint8_t *)buffer, 2000);

    // Test different levels
    for (int level = 1; level <= 9; level++) {
        void *compressed = rt_compress_deflate_lvl(original, level);
        void *decompressed = rt_compress_inflate(compressed);

        char test_name[32];
        snprintf(test_name, sizeof(test_name), "Level %d round-trip", level);
        test_result(test_name, bytes_equal(original, decompressed));
    }
}

static void test_deflate_binary() {
    printf("Testing DEFLATE Binary Data:\n");

    // Binary data with all byte values
    uint8_t buffer[512];
    for (int i = 0; i < 512; i++) {
        buffer[i] = (uint8_t)(i & 0xFF);
    }

    void *original = make_bytes(buffer, 512);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Binary data round-trip", bytes_equal(original, decompressed));
}

//=============================================================================
// GZIP Tests
//=============================================================================

static void test_gzip_gunzip_basic() {
    printf("Testing GZIP Basic:\n");

    const char *text = "Hello, GZIP World!";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_gzip(original);
    void *decompressed = rt_compress_gunzip(compressed);

    test_result("Basic round-trip", bytes_equal(original, decompressed));

    // Check GZIP magic number
    uint8_t *data = get_bytes_data(compressed);
    test_result("GZIP magic number", data[0] == 0x1F && data[1] == 0x8B);
    test_result("GZIP method = deflate", data[2] == 0x08);
}

static void test_gzip_levels() {
    printf("Testing GZIP Levels:\n");

    char buffer[1000];
    for (int i = 0; i < 1000; i++) {
        buffer[i] = 'X';
    }
    void *original = make_bytes((const uint8_t *)buffer, 1000);

    for (int level = 1; level <= 9; level++) {
        void *compressed = rt_compress_gzip_lvl(original, level);
        void *decompressed = rt_compress_gunzip(compressed);

        char test_name[32];
        snprintf(test_name, sizeof(test_name), "Level %d round-trip", level);
        test_result(test_name, bytes_equal(original, decompressed));
    }
}

static void test_gzip_crc() {
    printf("Testing GZIP CRC:\n");

    // Create data and compress
    void *original = make_bytes_str("Test data for CRC verification");
    void *compressed = rt_compress_gzip(original);
    void *decompressed = rt_compress_gunzip(compressed);

    test_result("CRC verification passed", bytes_equal(original, decompressed));
}

static void test_gzip_concatenated_members() {
    printf("Testing GZIP concatenated members:\n");

    void *first = make_bytes_str("first:");
    void *second = make_bytes_str("second");
    void *gz_first = rt_compress_gzip(first);
    void *gz_second = rt_compress_gzip(second);
    int64_t first_len = get_bytes_len(gz_first);
    int64_t second_len = get_bytes_len(gz_second);
    void *joined = rt_bytes_new(first_len + second_len);
    memcpy(get_bytes_data(joined), get_bytes_data(gz_first), (size_t)first_len);
    memcpy(get_bytes_data(joined) + first_len, get_bytes_data(gz_second), (size_t)second_len);

    void *decompressed = rt_compress_gunzip(joined);
    const char expected[] = "first:second";
    test_result("Concatenated members inflate in order",
                get_bytes_len(decompressed) == (int64_t)strlen(expected) &&
                    memcmp(get_bytes_data(decompressed), expected, strlen(expected)) == 0);
}

/// @brief Verify native GZIP decoding enforces limits before publishing output.
/// @details Covers exact success, absolute and ratio rejection, configured
///          ratio slack, and aggregate limits across concatenated members.
static void test_gzip_native_output_limits() {
    printf("Testing native GZIP output and expansion limits:\n");

    constexpr size_t plain_len = 8192;
    uint8_t plain[plain_len];
    memset(plain, 'X', sizeof(plain));
    void *original = make_bytes(plain, sizeof(plain));
    void *compressed = rt_compress_gzip(original);
    const size_t compressed_len = static_cast<size_t>(get_bytes_len(compressed));
    uint8_t *decoded = nullptr;
    size_t decoded_len = 0;

    int ok = rt_compress_gunzip_raw(
        get_bytes_data(compressed), compressed_len, plain_len, 0, 0, &decoded, &decoded_len);
    test_result("Native exact-limit decode succeeds",
                ok == 1 && decoded_len == plain_len && memcmp(decoded, plain, sizeof(plain)) == 0);
    free(decoded);

    decoded = reinterpret_cast<uint8_t *>(1);
    decoded_len = 1;
    ok = rt_compress_gunzip_raw(
        get_bytes_data(compressed), compressed_len, plain_len - 1, 0, 0, &decoded, &decoded_len);
    test_result("Native absolute limit rejects before publication",
                ok == 0 && decoded == nullptr && decoded_len == 0);

    decoded = reinterpret_cast<uint8_t *>(1);
    decoded_len = 1;
    ok = rt_compress_gunzip_raw(
        get_bytes_data(compressed), compressed_len, plain_len, 1, 0, &decoded, &decoded_len);
    test_result("Native expansion ratio rejects compressed amplification",
                compressed_len < plain_len && ok == 0 && decoded == nullptr && decoded_len == 0);

    ok = rt_compress_gunzip_raw(get_bytes_data(compressed),
                                compressed_len,
                                plain_len,
                                1,
                                plain_len,
                                &decoded,
                                &decoded_len);
    test_result("Native expansion slack is caller configurable",
                ok == 1 && decoded_len == plain_len);
    free(decoded);

    void *joined = rt_bytes_new(static_cast<int64_t>(compressed_len * 2));
    memcpy(get_bytes_data(joined), get_bytes_data(compressed), compressed_len);
    memcpy(get_bytes_data(joined) + compressed_len, get_bytes_data(compressed), compressed_len);
    decoded = reinterpret_cast<uint8_t *>(1);
    decoded_len = 1;
    ok = rt_compress_gunzip_raw(get_bytes_data(joined),
                                compressed_len * 2,
                                plain_len * 2 - 1,
                                0,
                                0,
                                &decoded,
                                &decoded_len);
    test_result("Concatenated members share one aggregate output limit",
                ok == 0 && decoded == nullptr && decoded_len == 0);

    g_alloc_fail_countdown = 1;
    rt_set_alloc_hook(compress_fail_countdown_alloc);
    EXPECT_TRAP(rt_compress_gunzip(compressed));
    rt_set_alloc_hook(nullptr);
    void *recovered = rt_compress_gunzip(compressed);
    test_result("Managed allocation trap leaves gunzip reusable",
                recovered && get_bytes_len(recovered) == (int64_t)plain_len);

    release_obj(recovered);
    release_obj(joined);
    release_obj(compressed);
    release_obj(original);
}

static void test_gzip_member_boundary_ignores_payload_magic() {
    printf("Testing GZIP member boundary detection ignores payload bytes:\n");

    const uint8_t first_payload[] = {'A', 0x1F, 0x8B, 0x08, 'B'};
    void *first = make_bytes(first_payload, sizeof(first_payload));
    void *second = make_bytes_str(":tail");
    void *gz_first = rt_compress_gzip(first);
    void *gz_second = rt_compress_gzip(second);
    int64_t first_len = get_bytes_len(gz_first);
    int64_t second_len = get_bytes_len(gz_second);
    void *joined = rt_bytes_new(first_len + second_len);
    memcpy(get_bytes_data(joined), get_bytes_data(gz_first), (size_t)first_len);
    memcpy(get_bytes_data(joined) + first_len, get_bytes_data(gz_second), (size_t)second_len);

    void *decompressed = rt_compress_gunzip(joined);
    const uint8_t expected[] = {'A', 0x1F, 0x8B, 0x08, 'B', ':', 't', 'a', 'i', 'l'};
    test_result("Payload GZIP magic does not split member",
                get_bytes_len(decompressed) == (int64_t)sizeof(expected) &&
                    memcmp(get_bytes_data(decompressed), expected, sizeof(expected)) == 0);
}

static void test_inflate_limit_traps() {
    printf("Testing Inflate explicit limit:\n");

    void *original = make_bytes_str("limited payload");
    void *compressed = rt_compress_deflate(original);
    EXPECT_TRAP(rt_compress_inflate_limit(compressed, 4));
    test_result("InflateLimit traps when output exceeds limit", true);
}

static void test_gzip_rejects_reserved_flags() {
    printf("Testing GZIP reserved flags rejection:\n");

    void *original = make_bytes_str("reserved flag payload");
    void *compressed = rt_compress_gzip(original);
    uint8_t *data = get_bytes_data(compressed);
    data[3] |= 0x20;

    EXPECT_TRAP(rt_compress_gunzip(compressed));
    test_result("reserved flags trap", true);
}

static void test_gzip_rejects_truncated_optional_filename() {
    printf("Testing GZIP optional filename bounds:\n");

    uint8_t truncated[] = {
        0x1F, 0x8B, 0x08, 0x08, 0, 0, 0, 0, 0, 0xFF, 'n', 'a', 'm', 'e', 'x', 'x', 'x', 'x'};
    void *compressed = make_bytes(truncated, sizeof(truncated));

    EXPECT_TRAP(rt_compress_gunzip(compressed));
    test_result("truncated filename traps", true);
}

//=============================================================================
// String Convenience Tests
//=============================================================================

static void test_deflate_string() {
    printf("Testing DEFLATE String:\n");

    rt_string text = rt_const_cstr("Hello, String Compression!");
    void *compressed = rt_compress_deflate_str(text);
    rt_string decompressed = rt_compress_inflate_str(compressed);

    const char *orig_str = rt_string_cstr(text);
    const char *dec_str = rt_string_cstr(decompressed);

    test_result("String round-trip", strcmp(orig_str, dec_str) == 0);
}

static void test_gzip_string() {
    printf("Testing GZIP String:\n");

    rt_string text = rt_const_cstr("Hello, GZIP String!");
    void *compressed = rt_compress_gzip_str(text);
    rt_string decompressed = rt_compress_gunzip_str(compressed);

    const char *orig_str = rt_string_cstr(text);
    const char *dec_str = rt_string_cstr(decompressed);

    test_result("String round-trip", strcmp(orig_str, dec_str) == 0);
}

//=============================================================================
// Known Compressed Data Tests
//=============================================================================

static void test_inflate_known_data() {
    printf("Testing Inflate Known Data:\n");

    // This is "Hello" compressed with deflate (stored block)
    // Created by: echo -n "Hello" | python3 -c "import zlib,sys;
    // sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(), 0)[2:-4])" However, since we
    // use stored blocks for small data, let's just verify round-trip

    const char *text = "Hello";
    void *original = make_bytes_str(text);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Known data round-trip", bytes_equal(original, decompressed));
}

static void test_inflate_truncated_data_traps() {
    printf("Testing Inflate Truncated Data:\n");

    uint8_t invalid[] = {0x01, 0x00};
    void *compressed = make_bytes(invalid, sizeof(invalid));
    EXPECT_TRAP(rt_compress_inflate(compressed));

    uint8_t truncated_fixed_huffman[] = {0x03};
    void *fixed = make_bytes(truncated_fixed_huffman, sizeof(truncated_fixed_huffman));
    EXPECT_TRAP(rt_compress_inflate(fixed));

    test_result("Truncated input traps", true);
}

/// @brief Verify strict RFC 1950 framing around the shared exact-destination decoder.
/// @details Uses a dependency-free stored-block stream for `Hello`, then independently corrupts
///          compression method/FCHECK, FDICT, DEFLATE block type, stream bounds, declared output
///          size, exact DEFLATE consumption, and Adler-32. This is the low-level contract consumed
///          by KTX2 zlib supercompression and other container decoders.
static void test_zlib_exact_destination_validation() {
    printf("Testing strict zlib exact-destination decode:\n");

    const uint8_t valid[] = {
        0x78,
        0x01, /* CMF/FLG: DEFLATE, 32 KiB, valid FCHECK. */
        0x01,
        0x05,
        0x00,
        0xFA,
        0xFF, /* Final stored block, LEN=5, NLEN=~5. */
        'H',
        'e',
        'l',
        'l',
        'o',
        0x05,
        0x8C,
        0x01,
        0xF5, /* Adler-32("Hello"). */
    };
    uint8_t output[6] = {0};
    uint8_t corrupt[sizeof(valid) + 1];

    test_result("valid zlib stream decodes",
                rt_compress_inflate_zlib_into(valid, sizeof(valid), output, 5) == 1 &&
                    memcmp(output, "Hello", 5) == 0);

    memcpy(corrupt, valid, sizeof(valid));
    corrupt[0] = 0x79; /* Non-DEFLATE method and invalid FCHECK. */
    test_result("invalid CMF/FCHECK rejected",
                rt_compress_inflate_zlib_into(corrupt, sizeof(valid), output, 5) == 0);

    memcpy(corrupt, valid, sizeof(valid));
    corrupt[1] = 0x20; /* 0x7820 has valid FCHECK but requests a preset dictionary. */
    test_result("preset dictionary rejected",
                rt_compress_inflate_zlib_into(corrupt, sizeof(valid), output, 5) == 0);

    memcpy(corrupt, valid, sizeof(valid));
    corrupt[2] = 0x07; /* BFINAL=1, reserved BTYPE=3. */
    test_result("invalid DEFLATE block rejected",
                rt_compress_inflate_zlib_into(corrupt, sizeof(valid), output, 5) == 0);

    test_result("truncated DEFLATE/checksum bounds rejected",
                rt_compress_inflate_zlib_into(valid, sizeof(valid) - 1, output, 5) == 0);
    test_result("short declared destination rejected",
                rt_compress_inflate_zlib_into(valid, sizeof(valid), output, 4) == 0);
    test_result("long declared destination rejected",
                rt_compress_inflate_zlib_into(valid, sizeof(valid), output, 6) == 0);

    memcpy(corrupt, valid, sizeof(valid));
    corrupt[sizeof(valid) - 1] ^= 0x01;
    test_result("Adler-32 corruption rejected",
                rt_compress_inflate_zlib_into(corrupt, sizeof(valid), output, 5) == 0);

    memcpy(corrupt, valid, sizeof(valid) - 4);
    corrupt[sizeof(valid) - 4] = 0x00; /* Hidden byte after the final raw block. */
    memcpy(corrupt + sizeof(valid) - 3, valid + sizeof(valid) - 4, 4);
    test_result("trailing raw-DEFLATE byte rejected",
                rt_compress_inflate_zlib_into(corrupt, sizeof(corrupt), output, 5) == 0);
}

//=============================================================================
// Large Data Test
//=============================================================================

static void test_large_data() {
    printf("Testing Large Data:\n");

    // 100KB of compressible data
    size_t size = 100 * 1024;
    uint8_t *buffer = (uint8_t *)malloc(size);
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)('A' + (i % 26));
    }

    void *original = make_bytes(buffer, size);
    free(buffer);

    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Large data round-trip", bytes_equal(original, decompressed));

    printf("  Original: %lld bytes, Compressed: %lld bytes (%.1f%% ratio)\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed),
           100.0 * get_bytes_len(compressed) / get_bytes_len(original));
}

//=============================================================================
// Random Data Test
//=============================================================================

static void test_random_data() {
    printf("Testing Random Data:\n");

    // Random data (hard to compress)
    uint8_t buffer[1000];
    unsigned int seed = 12345;
    for (int i = 0; i < 1000; i++) {
        seed = seed * 1103515245 + 12345;
        buffer[i] = (uint8_t)(seed >> 16);
    }

    void *original = make_bytes(buffer, 1000);
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Random data round-trip", bytes_equal(original, decompressed));

    printf("  Original: %lld bytes, Compressed: %lld bytes\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed));
}

static void test_inflate_rejects_trailing_data() {
    printf("Testing Inflate Trailing Data Rejection:\n");

    void *original = make_bytes_str("payload payload payload payload");
    void *compressed = rt_compress_deflate(original);
    void *with_trailing_zero = bytes_with_extra_byte(compressed, 0x00);
    void *with_trailing_nonzero = bytes_with_extra_byte(compressed, 0x80);

    EXPECT_TRAP(rt_compress_inflate(with_trailing_zero));
    EXPECT_TRAP(rt_compress_inflate(with_trailing_nonzero));
    test_result("Trailing bytes rejected", true);
}

static void test_invalid_bytes_data_traps_at_api_boundary() {
    printf("Testing invalid Bytes data traps at Compress API boundary:\n");

    void *bad = make_invalid_bytes_object();

    EXPECT_TRAP(rt_compress_deflate(bad));
    EXPECT_TRAP(rt_compress_deflate_lvl(bad, 1));
    EXPECT_TRAP(rt_compress_inflate(bad));
    EXPECT_TRAP(rt_compress_inflate_limit(bad, 16));
    EXPECT_TRAP((void)rt_compress_inflate_str(bad));
    EXPECT_TRAP(rt_compress_gzip(bad));
    EXPECT_TRAP(rt_compress_gzip_lvl(bad, 1));
    EXPECT_TRAP(rt_compress_gunzip(bad));
    EXPECT_TRAP((void)rt_compress_gunzip_str(bad));

    release_obj(bad);
    test_result("Invalid Bytes data rejected", true);
}

//=============================================================================
// Entry Point
//=============================================================================

// Verify the encoder actually emits a dynamic-Huffman (BTYPE=2) block when it
// is smaller, and that the decoder reads it back. A ~4 KB payload drawn from a
// skewed alphabet with few long matches gives dynamic code lengths a clear edge
// over the fixed assignment, so the "keep the smaller block" driver picks type 2.
static void test_deflate_dynamic_huffman() {
    printf("Testing DEFLATE Dynamic Huffman (BTYPE=2):\n");

    static const char alpha[] = "eeeeeeeeetttttttaaaaaaooooiiiinnnsssrrhhddl  .,";
    const size_t alen = sizeof(alpha) - 1;
    uint8_t buf[4096];
    uint32_t s = 0x12345u;
    for (size_t i = 0; i < sizeof(buf); i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)alpha[(s >> 16) % alen];
    }

    void *original = make_bytes(buf, sizeof(buf));
    void *compressed = rt_compress_deflate(original);
    void *decompressed = rt_compress_inflate(compressed);

    test_result("Dynamic-Huffman round-trip", bytes_equal(original, decompressed));

    // Block header low 3 bits = BFINAL(1) | (BTYPE << 1). Dynamic => 1 | (2<<1) = 5.
    int header = get_bytes_len(compressed) > 0 ? (get_bytes_data(compressed)[0] & 7) : -1;
    test_result("Dynamic-Huffman block selected (BTYPE=2)", header == 5);
    printf("  Original: %lld bytes, Compressed: %lld bytes (header&7=%d)\n",
           (long long)get_bytes_len(original),
           (long long)get_bytes_len(compressed),
           header);
}

int main() {
    printf("=== RT Compress Tests ===\n\n");

    // DEFLATE tests
    test_deflate_literals_only();
    printf("\n");
    test_deflate_simple_match();
    printf("\n");
    test_deflate_distance_with_extra_bits();
    printf("\n");
    test_deflate_distance_26();
    printf("\n");
    test_deflate_longer_data();
    printf("\n");
    test_deflate_inflate_empty();
    printf("\n");
    test_deflate_inflate_small();
    printf("\n");
    test_deflate_inflate_repeated();
    printf("\n");
    test_deflate_levels();
    printf("\n");
    test_deflate_binary();
    printf("\n");
    test_deflate_dynamic_huffman();
    printf("\n");

    // GZIP tests
    test_gzip_gunzip_basic();
    printf("\n");
    test_gzip_levels();
    printf("\n");
    test_gzip_crc();
    printf("\n");
    test_gzip_concatenated_members();
    printf("\n");
    test_gzip_native_output_limits();
    printf("\n");
    test_gzip_member_boundary_ignores_payload_magic();
    printf("\n");
    test_gzip_rejects_reserved_flags();
    printf("\n");
    test_gzip_rejects_truncated_optional_filename();
    printf("\n");

    // String tests
    test_deflate_string();
    printf("\n");
    test_gzip_string();
    printf("\n");

    // Known data
    test_inflate_known_data();
    printf("\n");
    test_inflate_truncated_data_traps();
    printf("\n");
    test_zlib_exact_destination_validation();
    printf("\n");
    test_inflate_rejects_trailing_data();
    printf("\n");
    test_inflate_limit_traps();
    printf("\n");
    test_invalid_bytes_data_traps_at_api_boundary();
    printf("\n");

    // Large data
    test_large_data();
    printf("\n");

    // Random data
    test_random_data();
    printf("\n");

    printf("All Compress tests passed!\n");
    return 0;
}
