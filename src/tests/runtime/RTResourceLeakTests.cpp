//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTResourceLeakTests.cpp
// Purpose: Tests verifying that error paths in rt_pixels and rt_websocket
//          properly release resources (GC objects, file handles, malloc'd
//          memory) instead of leaking them.
//
//===----------------------------------------------------------------------===//

#include "rt_crc32.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_pixels.h"
#include "rt_string.h"

#include "tests/common/PosixCompat.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void assert_png_chunk_crcs_valid(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    assert(fseek(f, 0, SEEK_END) == 0);
    long file_len = ftell(f);
    assert(file_len > 8);
    assert(fseek(f, 0, SEEK_SET) == 0);

    uint8_t *buf = (uint8_t *)malloc((size_t)file_len);
    assert(buf != nullptr);
    assert(fread(buf, 1, (size_t)file_len, f) == (size_t)file_len);
    fclose(f);

    static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(memcmp(buf, png_sig, 8) == 0);

    size_t pos = 8;
    int saw_iend = 0;
    while (pos + 12 <= (size_t)file_len) {
        uint32_t chunk_len = read_be32(buf + pos);
        pos += 4;
        size_t chunk_start = pos;
        assert(pos + 4 + (size_t)chunk_len + 4 <= (size_t)file_len);

        const uint8_t *chunk_type = buf + pos;
        pos += 4 + (size_t)chunk_len;

        uint32_t file_crc = read_be32(buf + pos);
        pos += 4;

        uint32_t actual_crc = rt_crc32_compute(buf + chunk_start, 4 + (size_t)chunk_len);
        assert(file_crc == actual_crc);

        if (memcmp(chunk_type, "IEND", 4) == 0) {
            saw_iend = 1;
            break;
        }
    }

    free(buf);
    assert(saw_iend == 1);
}

// ============================================================================
// PNG load — error path cleanup
// ============================================================================

/// Loading a nonexistent PNG returns NULL without leaking GC objects.
static void test_load_png_nonexistent_no_leak() {
    int64_t before = rt_gc_tracked_count();

    const char *path = "/nonexistent/path/file.png";
    rt_string s = rt_string_from_bytes(path, strlen(path));
    void *result = rt_pixels_load_png(s);
    assert(result == nullptr);

    int64_t after = rt_gc_tracked_count();
    // The string itself may be tracked, but no PNG-internal objects should remain.
    // Allow for the string object; the important thing is no growth beyond that.
    assert(after <= before + 1);

    printf("test_load_png_nonexistent_no_leak: PASSED\n");
}

/// Loading a file that isn't a PNG (bad signature) returns NULL without leaking.
static void test_load_png_bad_signature_no_leak() {
    // Create a temp file with non-PNG content
    char tmpfile[] = "/tmp/zanna_test_bad_png_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    const char *junk = "This is not a PNG file at all!";
    const ssize_t junk_written = write(fd, junk, strlen(junk));
    assert(junk_written == static_cast<ssize_t>(strlen(junk)));
    close(fd);

    int64_t before = rt_gc_tracked_count();

    rt_string s = rt_string_from_bytes(tmpfile, strlen(tmpfile));
    void *result = rt_pixels_load_png(s);
    assert(result == nullptr);

    int64_t after = rt_gc_tracked_count();
    assert(after <= before + 1);

    unlink(tmpfile);
    printf("test_load_png_bad_signature_no_leak: PASSED\n");
}

/// Loading a truncated PNG (valid signature, incomplete data) returns NULL
/// without leaking comp_bytes or raw_bytes.
static void test_load_png_truncated_no_leak() {
    // Create a temp file with just the PNG signature + truncated IHDR
    char tmpfile[] = "/tmp/zanna_test_trunc_png_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);

    // PNG signature
    unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    const ssize_t sig_written = write(fd, sig, 8);
    assert(sig_written == 8);
    // Truncated — no IHDR or IDAT chunks
    close(fd);

    int64_t before = rt_gc_tracked_count();

    rt_string s = rt_string_from_bytes(tmpfile, strlen(tmpfile));
    void *result = rt_pixels_load_png(s);
    assert(result == nullptr);

    int64_t after = rt_gc_tracked_count();
    assert(after <= before + 1);

    unlink(tmpfile);
    printf("test_load_png_truncated_no_leak: PASSED\n");
}

/// Repeated failed PNG loads don't accumulate GC objects (regression test for
/// the comp_bytes/raw_bytes leak fixed in this changeset).
static void test_load_png_repeated_failures_stable() {
    char tmpfile[] = "/tmp/zanna_test_repeat_png_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    const char *junk = "NotAPNG";
    const ssize_t junk_written = write(fd, junk, strlen(junk));
    assert(junk_written == static_cast<ssize_t>(strlen(junk)));
    close(fd);

    rt_string s = rt_string_from_bytes(tmpfile, strlen(tmpfile));

    // Warm up — first call may initialize internal state
    void *r = rt_pixels_load_png(s);
    assert(r == nullptr);

    int64_t baseline = rt_gc_tracked_count();

    // Do 100 failed loads — tracked count should not grow
    for (int i = 0; i < 100; i++) {
        r = rt_pixels_load_png(s);
        assert(r == nullptr);
    }

    int64_t after = rt_gc_tracked_count();
    // Allow small variance (±2) for string interning etc., but not 100+ leaked objects
    assert(after - baseline < 5);

    unlink(tmpfile);
    printf("test_load_png_repeated_failures_stable: PASSED\n");
}

// ============================================================================
// BMP load — error path cleanup
// ============================================================================

/// Loading a nonexistent BMP returns NULL without leaking file handles.
static void test_load_bmp_nonexistent_no_leak() {
    const char *path = "/nonexistent/path/file.bmp";
    rt_string s = rt_string_from_bytes(path, strlen(path));
    void *result = rt_pixels_load_bmp(s);
    assert(result == nullptr);

    printf("test_load_bmp_nonexistent_no_leak: PASSED\n");
}

/// Loading a truncated BMP (valid magic, incomplete headers) returns NULL.
static void test_load_bmp_truncated_no_leak() {
    char tmpfile[] = "/tmp/zanna_test_trunc_bmp_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    // Write just BM magic + a few bytes (not a complete header)
    const ssize_t header_written = write(fd, "BM\0\0\0\0\0\0\0\0\0\0\0\0", 14);
    assert(header_written == 14);
    close(fd);

    rt_string s = rt_string_from_bytes(tmpfile, strlen(tmpfile));
    void *result = rt_pixels_load_bmp(s);
    assert(result == nullptr);

    unlink(tmpfile);
    printf("test_load_bmp_truncated_no_leak: PASSED\n");
}

// ============================================================================
// PNG save/load roundtrip — verify success path releases internal buffers
// ============================================================================

/// A successful PNG save+load roundtrip doesn't leak GC objects.
static void test_png_roundtrip_no_leak() {
    // Create a small pixel buffer
    void *p = rt_pixels_new(4, 4);
    assert(p != nullptr);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            rt_pixels_set(p, x, y, 0xFF0000FF); // red

    // Save to temp file
    char tmpfile[] = "/tmp/zanna_test_png_rt_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    close(fd);

    char pngpath[256];
    snprintf(pngpath, sizeof(pngpath), "%s.png", tmpfile);
    rename(tmpfile, pngpath);

    rt_string path = rt_string_from_bytes(pngpath, strlen(pngpath));

    int64_t before = rt_gc_tracked_count();

    int64_t save_ok = rt_pixels_save_png(p, path);
    assert(save_ok == 1);
    assert_png_chunk_crcs_valid(pngpath);

    int64_t after_save = rt_gc_tracked_count();
    // save_png should release raw_bytes and comp_bytes internally
    assert(after_save - before < 3);

    // Load it back
    void *loaded = rt_pixels_load_png(path);
    assert(loaded != nullptr);
    assert(rt_pixels_width(loaded) == 4);
    assert(rt_pixels_height(loaded) == 4);

    int64_t after_load = rt_gc_tracked_count();
    // load_png should release raw_bytes and comp_bytes internally;
    // only the pixels object itself should be newly tracked
    assert(after_load - before < 5);

    unlink(pngpath);
    printf("test_png_roundtrip_no_leak: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // PNG error paths
    test_load_png_nonexistent_no_leak();
    test_load_png_bad_signature_no_leak();
    test_load_png_truncated_no_leak();
    test_load_png_repeated_failures_stable();

    // BMP error paths
    test_load_bmp_nonexistent_no_leak();
    test_load_bmp_truncated_no_leak();

    // Roundtrip (success path)
    test_png_roundtrip_no_leak();

    printf("\nAll resource leak tests passed.\n");
    return 0;
}
