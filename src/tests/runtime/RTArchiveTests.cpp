//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTArchiveTests.cpp
// Purpose: Validate Zanna.IO.Archive ZIP archive support.
// Key invariants: Round-trip create/read preserves data, ZIP format compatibility.
// Ownership/Lifetime: Each test owns and removes its temporary archive/output
//                     paths and releases all returned managed values.
// Links: docs/zannalib/io.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_archive.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_dir.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "tests/common/PlatformSkip.h"

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if RT_PLATFORM_WINDOWS
#include <direct.h>
#include <io.h>
#include <windows.h>
#define mkdir_p(path) _mkdir(path)
#define rmdir _rmdir
#define unlink _unlink
#else
#include "tests/common/PosixCompat.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
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

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Set or clear one process environment variable portably for resource-limit tests.
/// @details Windows uses `_putenv_s`, where an empty value removes the logical
///          setting for this test. POSIX uses `setenv`/`unsetenv`. The helper
///          asserts success because a failed mutation would make the limit test
///          nondeterministic.
/// @param name Environment variable name.
/// @param value New value, or NULL to clear the variable.
static void set_test_environment(const char *name, const char *value) {
#if RT_PLATFORM_WINDOWS
    assert(_putenv_s(name, value ? value : "") == 0);
#else
    int rc = value ? setenv(name, value, 1) : unsetenv(name);
    assert(rc == 0);
#endif
}

/// @brief Restore an environment variable from a caller-owned optional snapshot.
/// @param name Environment variable name.
/// @param saved NULL when the variable was originally absent, otherwise its copied value.
static void restore_test_environment(const char *name, const char *saved) {
    set_test_environment(name, saved);
}

/// @brief Duplicate an optional C string with portable `malloc` semantics.
/// @param value Source string, or NULL.
/// @return Owned copy, or NULL when @p value is NULL or allocation fails.
static char *copy_optional_cstr(const char *value) {
    if (!value)
        return nullptr;
    size_t length = strlen(value);
    char *copy = static_cast<char *>(malloc(length + 1));
    assert(copy != nullptr);
    memcpy(copy, value, length + 1);
    return copy;
}

static void *make_invalid_bytes_object(int64_t len) {
    struct FakeBytes {
        int64_t len;
        uint8_t *data;
    };

    FakeBytes *bad = static_cast<FakeBytes *>(rt_obj_new_i64(RT_BYTES_CLASS_ID, sizeof(FakeBytes)));
    bad->len = len;
    bad->data = nullptr;
    return bad;
}

/// @brief Compare two byte arrays
static bool bytes_equal(void *a, void *b) {
    int64_t len_a = get_bytes_len(a);
    int64_t len_b = get_bytes_len(b);
    if (len_a != len_b)
        return false;
    return memcmp(get_bytes_data(a), get_bytes_data(b), len_a) == 0;
}

/// @brief Create bytes from string literal
static void *make_bytes_str(const char *str) {
    size_t len = strlen(str);
    void *bytes = rt_bytes_new((int64_t)len);
    memcpy(get_bytes_data(bytes), str, len);
    return bytes;
}

/// @brief Get a temporary file path for testing
/// @note Uses rotating buffers to allow multiple concurrent paths
static const char *get_temp_path(const char *name) {
    static char paths[4][256];
    static int idx = 0;
    char *path = paths[idx];
    idx = (idx + 1) % 4;
#if RT_PLATFORM_WINDOWS
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = ".";
    snprintf(path, 256, "%s\\%s", tmp, name);
#else
    snprintf(path, 256, "/tmp/%s", name);
#endif
    return path;
}

/// @brief Delete a file if it exists
static void delete_file(const char *path) {
    unlink(path);
}

/// @brief Count process file/handle resources without retaining a new one.
/// @details Windows exposes the process handle count directly. POSIX probes a
///          bounded descriptor range with `fcntl(F_GETFD)` and treats any error
///          other than EBADF as an occupied slot. The archive tests allocate
///          descriptors from the low range, so 4096 slots detect leaked roots,
///          parents, sources, and temporary output files deterministically.
/// @return Number of live handles/descriptors visible to the test process.
static uint64_t process_open_resource_count() {
#if RT_PLATFORM_WINDOWS
    DWORD count = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count) != 0);
    return (uint64_t)count;
#else
    uint64_t count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
            ++count;
    }
    return count;
#endif
}

static bool file_equals_text(const char *path, const char *expected) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read] = '\0';
    bool ok = strcmp(buf, expected) == 0;
    free(buf);
    return ok;
}

static std::vector<uint8_t> read_file_bytes(const char *path) {
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    assert(size >= 0);
    fseek(fp, 0, SEEK_SET);

    std::vector<uint8_t> bytes((size_t)size);
    if (size > 0) {
        size_t read = fread(bytes.data(), 1, (size_t)size, fp);
        assert(read == (size_t)size);
    }
    fclose(fp);
    return bytes;
}

static void write_u32_le(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    assert(offset + 4 <= bytes.size());
    bytes[offset + 0] = (uint8_t)(value & 0xFF);
    bytes[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
    bytes[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
    bytes[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

static void write_u16_le(std::vector<uint8_t> &bytes, size_t offset, uint16_t value) {
    assert(offset + 2 <= bytes.size());
    bytes[offset + 0] = (uint8_t)(value & 0xFF);
    bytes[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
}

static void *bytes_from_vector(const std::vector<uint8_t> &bytes) {
    void *result = rt_bytes_new((int64_t)bytes.size());
    if (!bytes.empty())
        memcpy(get_bytes_data(result), bytes.data(), bytes.size());
    return result;
}

static size_t find_zip_signature(const std::vector<uint8_t> &bytes, uint32_t sig) {
    for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
        uint32_t found = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8) |
                         ((uint32_t)bytes[i + 2] << 16) | ((uint32_t)bytes[i + 3] << 24);
        if (found == sig) {
            return i;
        }
    }
    return bytes.size();
}

//=============================================================================
// Basic Archive Tests
//=============================================================================

static void test_create_empty_archive() {
    printf("Testing Create Empty Archive:\n");

    const char *path = get_temp_path("test_empty.zip");
    delete_file(path);

    // Create archive
    void *ar = rt_archive_create(rt_const_cstr(path));
    test_result("Create returns non-null", ar != NULL);

    // Finish immediately (empty archive)
    rt_archive_finish(ar);

    // Verify file exists and is valid ZIP
    test_result("IsZip returns true", rt_archive_is_zip(rt_const_cstr(path)) == 1);

    // Open and verify
    void *ar2 = rt_archive_open(rt_const_cstr(path));
    test_result("Open returns non-null", ar2 != NULL);
    test_result("Count is 0", rt_archive_count(ar2) == 0);

    delete_file(path);
}

static void test_create_single_file() {
    printf("Testing Create Single File:\n");

    const char *path = get_temp_path("test_single.zip");
    delete_file(path);

    // Create archive with one file
    void *ar = rt_archive_create(rt_const_cstr(path));
    void *content = make_bytes_str("Hello, World!");
    rt_archive_add(ar, rt_const_cstr("hello.txt"), content);
    rt_archive_finish(ar);

    // Reopen and verify
    void *ar2 = rt_archive_open(rt_const_cstr(path));
    test_result("Count is 1", rt_archive_count(ar2) == 1);
    test_result("Has entry", rt_archive_has(ar2, rt_const_cstr("hello.txt")) == 1);

    void *read_content = rt_archive_read(ar2, rt_const_cstr("hello.txt"));
    test_result("Content matches", bytes_equal(content, read_content));

    delete_file(path);
}

static void test_create_multiple_files() {
    printf("Testing Create Multiple Files:\n");

    const char *path = get_temp_path("test_multi.zip");
    delete_file(path);

    // Create archive with multiple files
    void *ar = rt_archive_create(rt_const_cstr(path));

    void *content1 = make_bytes_str("File 1 content");
    void *content2 = make_bytes_str("File 2 has different content");
    void *content3 = make_bytes_str("Third file");

    rt_archive_add(ar, rt_const_cstr("file1.txt"), content1);
    rt_archive_add(ar, rt_const_cstr("file2.txt"), content2);
    rt_archive_add(ar, rt_const_cstr("subdir/file3.txt"), content3);

    rt_archive_finish(ar);

    // Reopen and verify
    void *ar2 = rt_archive_open(rt_const_cstr(path));
    test_result("Count is 3", rt_archive_count(ar2) == 3);

    test_result("Has file1", rt_archive_has(ar2, rt_const_cstr("file1.txt")) == 1);
    test_result("Has file2", rt_archive_has(ar2, rt_const_cstr("file2.txt")) == 1);
    test_result("Has subdir/file3", rt_archive_has(ar2, rt_const_cstr("subdir/file3.txt")) == 1);
    test_result("No missing file", rt_archive_has(ar2, rt_const_cstr("missing.txt")) == 0);

    void *r1 = rt_archive_read(ar2, rt_const_cstr("file1.txt"));
    void *r2 = rt_archive_read(ar2, rt_const_cstr("file2.txt"));
    void *r3 = rt_archive_read(ar2, rt_const_cstr("subdir/file3.txt"));

    test_result("Content1 matches", bytes_equal(content1, r1));
    test_result("Content2 matches", bytes_equal(content2, r2));
    test_result("Content3 matches", bytes_equal(content3, r3));

    delete_file(path);
}

static void test_add_string() {
    printf("Testing AddStr:\n");

    const char *path = get_temp_path("test_addstr.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("text.txt"), rt_const_cstr("Hello from string!"));
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    rt_string text = rt_archive_read_str(ar2, rt_const_cstr("text.txt"));
    test_result("ReadStr works", strcmp(rt_string_cstr(text), "Hello from string!") == 0);

    delete_file(path);
}

static void test_add_directory() {
    printf("Testing AddDir:\n");

    const char *path = get_temp_path("test_dir.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_dir(ar, rt_const_cstr("mydir"));
    rt_archive_add_str(ar, rt_const_cstr("mydir/file.txt"), rt_const_cstr("Inside dir"));
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    test_result("Count is 2", rt_archive_count(ar2) == 2);
    test_result("Has directory", rt_archive_has(ar2, rt_const_cstr("mydir/")) == 1);

    // Check info for directory
    void *info = rt_archive_info(ar2, rt_const_cstr("mydir/"));
    void *is_dir = rt_map_get(info, rt_const_cstr("isDirectory"));
    test_result("isDirectory is true", rt_unbox_i1(is_dir) == 1);

    void *dir_bytes = rt_archive_read(ar2, rt_const_cstr("mydir/"));
    test_result("Read directory entry returns empty Bytes", rt_bytes_len(dir_bytes) == 0);

    delete_file(path);
}

static void test_extract_operations() {
    printf("Testing Extract and ExtractAll:\n");

    const char *zip_path = get_temp_path("test_extract.zip");
    const char *single_out = get_temp_path("test_extract_single.txt");
    const char *extract_dir = get_temp_path("test_extract_dir");
    delete_file(zip_path);
    delete_file(single_out);
    rt_dir_remove_all(rt_const_cstr(extract_dir));

    void *ar = rt_archive_create(rt_const_cstr(zip_path));
    rt_archive_add_str(ar, rt_const_cstr("hello.txt"), rt_const_cstr("Hello extract"));
    rt_archive_add_str(ar, rt_const_cstr("nested/world.txt"), rt_const_cstr("Nested extract"));
    rt_archive_finish(ar);

    void *reader = rt_archive_open(rt_const_cstr(zip_path));
    rt_archive_extract(reader, rt_const_cstr("hello.txt"), rt_const_cstr(single_out));
    test_result("Extract writes absolute destination",
                file_equals_text(single_out, "Hello extract"));

    mkdir_p(extract_dir);
    rt_archive_extract_all(reader, rt_const_cstr(extract_dir));

    char nested_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(nested_path, sizeof(nested_path), "%s\\nested\\world.txt", extract_dir);
#else
    snprintf(nested_path, sizeof(nested_path), "%s/nested/world.txt", extract_dir);
#endif
    char hello_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(hello_path, sizeof(hello_path), "%s\\hello.txt", extract_dir);
#else
    snprintf(hello_path, sizeof(hello_path), "%s/hello.txt", extract_dir);
#endif
    test_result("ExtractAll writes hello.txt", file_equals_text(hello_path, "Hello extract"));
    test_result("ExtractAll writes nested/world.txt",
                file_equals_text(nested_path, "Nested extract"));

    delete_file(single_out);
    delete_file(zip_path);
    rt_dir_remove_all(rt_const_cstr(extract_dir));
}

static void test_invalid_entry_names() {
    printf("Testing Invalid Entry Names:\n");

    const char *path = get_temp_path("test_invalid_names.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    void *content = make_bytes_str("payload");

    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("../evil.txt"), content));
    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("..\\evil.txt"), content));
    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("/absolute.txt"), content));
    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("C:\\absolute.txt"), content));

    rt_archive_add(ar, rt_const_cstr("subdir\\file.txt"), content);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    test_result("Normalized name found",
                rt_archive_has(ar2, rt_const_cstr("subdir/file.txt")) == 1);
    EXPECT_TRAP(rt_archive_read(ar2, rt_const_cstr("../missing.txt")));

    delete_file(path);
}

static void test_duplicate_and_null_entries_trap() {
    printf("Testing Duplicate/Null Entry Validation:\n");

    const char *path = get_temp_path("test_duplicate_entries.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    void *content = make_bytes_str("payload");

    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("null.bin"), nullptr));

    rt_archive_add(ar, rt_const_cstr("dup.txt"), content);
    EXPECT_TRAP(rt_archive_add(ar, rt_const_cstr("dup.txt"), content));

    rt_archive_add_dir(ar, rt_const_cstr("dir"));
    EXPECT_TRAP(rt_archive_add_dir(ar, rt_const_cstr("dir/")));

    rt_archive_finish(ar);
    test_result("duplicate/null entry validation traps", true);

    delete_file(path);
}

//=============================================================================
// Compression Tests
//=============================================================================

static void test_compression_stored() {
    printf("Testing Stored Compression:\n");

    const char *path = get_temp_path("test_stored.zip");
    delete_file(path);

    // Small data should be stored uncompressed
    void *ar = rt_archive_create(rt_const_cstr(path));
    void *small = make_bytes_str("Small data");
    rt_archive_add(ar, rt_const_cstr("small.txt"), small);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    void *read_small = rt_archive_read(ar2, rt_const_cstr("small.txt"));
    test_result("Small data round-trip", bytes_equal(small, read_small));

    delete_file(path);
}

static void test_compression_deflate() {
    printf("Testing Deflate Compression:\n");

    const char *path = get_temp_path("test_deflate.zip");
    delete_file(path);

    // Create compressible data (repeated pattern)
    char buffer[2000];
    for (int i = 0; i < 2000; i++) {
        buffer[i] = 'A' + (i % 26);
    }
    void *large = rt_bytes_new(2000);
    memcpy(get_bytes_data(large), buffer, 2000);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add(ar, rt_const_cstr("large.txt"), large);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));

    // Check compression worked
    void *info = rt_archive_info(ar2, rt_const_cstr("large.txt"));
    void *size = rt_map_get(info, rt_const_cstr("size"));
    void *comp_size = rt_map_get(info, rt_const_cstr("compressedSize"));

    int64_t orig_size = rt_unbox_i64(size);
    int64_t compressed_size = rt_unbox_i64(comp_size);

    test_result("Size correct", orig_size == 2000);
    test_result("Compression occurred", compressed_size < orig_size);

    printf("    Original: %lld bytes, Compressed: %lld bytes (%.1f%%)\n",
           (long long)orig_size,
           (long long)compressed_size,
           100.0 * compressed_size / orig_size);

    // Verify content
    void *read_large = rt_archive_read(ar2, rt_const_cstr("large.txt"));
    test_result("Large data round-trip", bytes_equal(large, read_large));

    delete_file(path);
}

//=============================================================================
// Property Tests
//=============================================================================

static void test_properties() {
    printf("Testing Properties:\n");

    const char *path = get_temp_path("test_props.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("a.txt"), rt_const_cstr("A"));
    rt_archive_add_str(ar, rt_const_cstr("b.txt"), rt_const_cstr("B"));
    rt_archive_add_str(ar, rt_const_cstr("c.txt"), rt_const_cstr("C"));
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));

    // Test Path property
    rt_string ar_path = rt_archive_path(ar2);
    test_result("Path not empty", strlen(rt_string_cstr(ar_path)) > 0);
    rt_string_unref(ar_path);

    // Test Count property
    test_result("Count is 3", rt_archive_count(ar2) == 3);

    // Test Names property
    void *names = rt_archive_names(ar2);
    test_result("Names has 3 entries", rt_seq_len(names) == 3);
    release_obj(ar2);
    ar2 = nullptr;
    rt_string first_name = static_cast<rt_string>(rt_seq_get(names, 0));
    test_result("Names survive archive release",
                first_name && rt_str_eq(first_name, rt_const_cstr("a.txt")));
    release_obj(names);
    release_obj(ar);

    delete_file(path);
}

static void test_path_retained_after_caller_release() {
    printf("Testing Path Lifetime:\n");

    const char *c_path = get_temp_path("test_path_lifetime.zip");
    delete_file(c_path);

    rt_string owned_path = rt_string_from_bytes(c_path, strlen(c_path));
    void *ar = rt_archive_create(owned_path);
    rt_string_unref(owned_path);

    for (int i = 0; i < 64; i++) {
        rt_string noise =
            rt_string_from_bytes("path-lifetime-noise", strlen("path-lifetime-noise"));
        rt_string_unref(noise);
    }

    rt_archive_add_str(ar, rt_const_cstr("entry.txt"), rt_const_cstr("payload"));
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(c_path));
    rt_string stored_path = rt_archive_path(ar2);
    test_result("Archive path preserved", rt_str_eq(stored_path, rt_const_cstr(c_path)));
    rt_string_unref(stored_path);

    delete_file(c_path);
}

static void test_create_does_not_truncate_until_finish() {
    printf("Testing Create Defers Destination Replacement:\n");

    const char *path = get_temp_path("test_create_deferred.zip");
    delete_file(path);

    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs("old payload", fp);
    fclose(fp);
#if RT_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA original_attributes{};
    assert(GetFileAttributesExA(path, GetFileExInfoStandard, &original_attributes) != 0);
#else
    assert(chmod(path, 0600) == 0);
#endif

    void *ar = rt_archive_create(rt_const_cstr(path));
    test_result("existing file unchanged after Create", file_equals_text(path, "old payload"));

    rt_archive_add_str(ar, rt_const_cstr("entry.txt"), rt_const_cstr("new payload"));
    rt_archive_finish(ar);
    test_result("Finish writes valid zip", rt_archive_is_zip(rt_const_cstr(path)) == 1);
#if RT_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA replaced_attributes{};
    assert(GetFileAttributesExA(path, GetFileExInfoStandard, &replaced_attributes) != 0);
    test_result("Finish preserves existing file creation time",
                CompareFileTime(&original_attributes.ftCreationTime,
                                &replaced_attributes.ftCreationTime) == 0);
#else
    struct stat replaced;
    assert(stat(path, &replaced) == 0);
    test_result("Finish preserves existing file mode", (replaced.st_mode & 0777) == 0600);
#endif

    delete_file(path);
}

/// @brief Verify a failed atomic Finish rolls back and removes its temporary sidecar.
/// @details Uses an existing directory as the destination so atomic replacement
///          fails after the sidecar has been fully written. Removing that
///          directory and retrying the same Archive must succeed, proving the
///          central-directory append was rolled back. The isolated parent must
///          contain only the final archive, proving failed sidecar cleanup.
static void test_finish_failure_is_retryable_and_cleans_sidecar() {
    printf("Testing Finish Failure Rollback and Cleanup:\n");

    char parent[256];
    snprintf(parent, sizeof(parent), "%s", get_temp_path("test_archive_finish_retry"));
    char destination[320];
#if RT_PLATFORM_WINDOWS
    snprintf(destination, sizeof(destination), "%s\\result.zip", parent);
#else
    snprintf(destination, sizeof(destination), "%s/result.zip", parent);
#endif
    rt_dir_remove_all(rt_const_cstr(parent));
    rt_dir_make_all(rt_const_cstr(parent));
    rt_dir_make(rt_const_cstr(destination));

    void *writer = rt_archive_create(rt_const_cstr(destination));
    rt_archive_add_str(writer, rt_const_cstr("entry.txt"), rt_const_cstr("retry payload"));
    EXPECT_TRAP(rt_archive_finish(writer));

    rt_dir_remove(rt_const_cstr(destination));
    rt_archive_finish(writer);
    assert(rt_archive_is_zip(rt_const_cstr(destination)) == 1);
    void *entries = rt_dir_list(rt_const_cstr(parent));
    test_result("failed Finish is retryable", rt_seq_len(entries) == 1);
    release_obj(entries);
    rt_dir_remove_all(rt_const_cstr(parent));
}

static void test_entry_info() {
    printf("Testing Entry Info:\n");

    const char *path = get_temp_path("test_info.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    void *content = make_bytes_str("Test content for info");
    rt_archive_add(ar, rt_const_cstr("info.txt"), content);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    void *info = rt_archive_info(ar2, rt_const_cstr("info.txt"));

    // Check all expected keys
    test_result("Has size key", rt_map_has(info, rt_const_cstr("size")) == 1);
    test_result("Has compressedSize key", rt_map_has(info, rt_const_cstr("compressedSize")) == 1);
    test_result("Has crc key", rt_map_has(info, rt_const_cstr("crc")) == 1);
    test_result("Has method key", rt_map_has(info, rt_const_cstr("method")) == 1);
    test_result("Has modifiedTime key", rt_map_has(info, rt_const_cstr("modifiedTime")) == 1);
    test_result("Has isDir key", rt_map_has(info, rt_const_cstr("isDir")) == 1);
    test_result("Has isDirectory key", rt_map_has(info, rt_const_cstr("isDirectory")) == 1);

    // Verify values
    void *size = rt_map_get(info, rt_const_cstr("size"));
    test_result("Size correct", rt_unbox_i64(size) == get_bytes_len(content));

    void *method = rt_map_get(info, rt_const_cstr("method"));
    int64_t method_value = rt_unbox_i64(method);
    test_result("method is supported", method_value == 0 || method_value == 8);

    void *crc = rt_map_get(info, rt_const_cstr("crc"));
    test_result("CRC is present", rt_unbox_i64(crc) >= 0);

    void *is_dir_short = rt_map_get(info, rt_const_cstr("isDir"));
    test_result("isDir is false", rt_unbox_i1(is_dir_short) == 0);

    void *is_dir = rt_map_get(info, rt_const_cstr("isDirectory"));
    test_result("isDirectory is false", rt_unbox_i1(is_dir) == 0);

    delete_file(path);
}

//=============================================================================
// FromBytes Tests
//=============================================================================

static void test_from_bytes() {
    printf("Testing FromBytes:\n");

    const char *path = get_temp_path("test_frombytes.zip");
    delete_file(path);

    // Create a ZIP file
    void *ar = rt_archive_create(rt_const_cstr(path));
    void *content = make_bytes_str("Memory test content");
    rt_archive_add(ar, rt_const_cstr("memory.txt"), content);
    rt_archive_finish(ar);

    // Read the ZIP file into bytes
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *zip_bytes = rt_bytes_new(size);
    size_t read_count = fread(get_bytes_data(zip_bytes), 1, size, f);
    fclose(f);
    assert((long)read_count == size);

    // Open from bytes
    void *ar2 = rt_archive_from_bytes(zip_bytes);
    test_result("FromBytes returns non-null", ar2 != NULL);
    test_result("Count is 1", rt_archive_count(ar2) == 1);

    void *read_content = rt_archive_read(ar2, rt_const_cstr("memory.txt"));
    test_result("Content matches", bytes_equal(content, read_content));

    // Path should be empty for FromBytes
    rt_string ar_path = rt_archive_path(ar2);
    test_result("Path is empty", strlen(rt_string_cstr(ar_path)) == 0);

    void *invalid = make_bytes_str("not a zip");
    EXPECT_TRAP(rt_archive_from_bytes(invalid));

    delete_file(path);
}

static void test_stored_size_mismatch_traps() {
    printf("Testing Stored Entry Size Validation:\n");

    const char *path = get_temp_path("test_stored_mismatch.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("bad.txt"), rt_const_cstr("abc"));
    rt_archive_finish(ar);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    bool patched = false;
    for (size_t i = 0; i + 46 <= bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 0x01 &&
            bytes[i + 3] == 0x02) {
            write_u32_le(bytes, i + 24, 4);
            patched = true;
            break;
        }
    }
    assert(patched);

    void *zip_bytes = rt_bytes_new((int64_t)bytes.size());
    memcpy(get_bytes_data(zip_bytes), bytes.data(), bytes.size());

    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("stored size mismatch traps during parse", true);

    delete_file(path);
}

static void test_central_directory_count_mismatch_traps() {
    printf("Testing Central Directory Count Validation:\n");

    const char *path = get_temp_path("test_cd_count.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("one.txt"), rt_const_cstr("one"));
    rt_archive_finish(ar);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    bool patched = false;
    for (size_t i = 0; i + 22 <= bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 0x05 &&
            bytes[i + 3] == 0x06) {
            write_u16_le(bytes, i + 8, 2);
            write_u16_le(bytes, i + 10, 2);
            patched = true;
            break;
        }
    }
    assert(patched);

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("central directory count mismatch traps", true);

    delete_file(path);
}

static void test_central_directory_nul_name_traps() {
    printf("Testing Central Directory NUL Name Validation:\n");

    const char *path = get_temp_path("test_cd_nul.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("nul.txt"), rt_const_cstr("payload"));
    rt_archive_finish(ar);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    bool patched = false;
    for (size_t i = 0; i + 46 <= bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 0x01 &&
            bytes[i + 3] == 0x02) {
            bytes[i + 46] = 0;
            patched = true;
            break;
        }
    }
    assert(patched);

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("central directory NUL name traps", true);

    delete_file(path);
}

static void test_central_directory_duplicate_name_traps() {
    printf("Testing Central Directory Duplicate Name Validation:\n");

    const char *path = get_temp_path("test_cd_duplicate.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("one.txt"), rt_const_cstr("one"));
    rt_archive_add_str(ar, rt_const_cstr("two.txt"), rt_const_cstr("two"));
    rt_archive_finish(ar);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    int patched = 0;
    for (size_t i = 0; i + 46 <= bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 0x01 &&
            bytes[i + 3] == 0x02) {
            uint16_t name_len = (uint16_t)(bytes[i + 28] | (bytes[i + 29] << 8));
            if (name_len == strlen("two.txt") &&
                memcmp(bytes.data() + i + 46, "two.txt", strlen("two.txt")) == 0) {
                memcpy(bytes.data() + i + 46, "one.txt", strlen("one.txt"));
                patched = 1;
                break;
            }
        }
    }
    assert(patched);

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("central directory duplicate name traps", true);

    delete_file(path);
}

static void test_corrupt_local_header_offset_traps_on_open() {
    printf("Testing Local Header Offset Validation:\n");

    const char *path = get_temp_path("test_local_offset.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("offset.txt"), rt_const_cstr("payload"));
    rt_archive_finish(ar);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    bool patched = false;
    for (size_t i = 0; i + 46 <= bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 0x01 &&
            bytes[i + 3] == 0x02) {
            write_u32_le(bytes, i + 42, 0xFFFFFFF0U);
            patched = true;
            break;
        }
    }
    assert(patched);

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("corrupt local header offset traps during parse", true);

    delete_file(path);
}

/// @brief Verify a central entry cannot redirect extraction through a differently named local file.
/// @details Mutates only the local-header filename while leaving the central
///          directory intact. Opening the byte-backed archive must reject the
///          disagreement before any lookup or extraction is attempted.
static void test_local_and_central_names_must_match() {
    printf("Testing Local/Central Name Agreement:\n");

    const char *path = get_temp_path("test_local_name_mismatch.zip");
    delete_file(path);
    void *writer = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(writer, rt_const_cstr("name.txt"), rt_const_cstr("payload"));
    rt_archive_finish(writer);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    size_t local = find_zip_signature(bytes, 0x04034b50U);
    assert(local + 30 + strlen("name.txt") <= bytes.size());
    bytes[local + 30] = (uint8_t)'x';

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("local/central name mismatch traps during parse", true);
    delete_file(path);
}

/// @brief Verify a local payload range cannot consume bytes from the central directory.
/// @details Patches both local and central size metadata to agree on a stored
///          payload whose exclusive end is one byte beyond the central-directory
///          offset. Open-time range validation must reject the overlap even
///          though the two headers are otherwise mutually consistent.
static void test_local_payload_cannot_overlap_central_directory() {
    printf("Testing Local Payload/Central Directory Separation:\n");

    const char *path = get_temp_path("test_local_overlaps_central.zip");
    delete_file(path);
    void *writer = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(writer, rt_const_cstr("range.txt"), rt_const_cstr("abc"));
    rt_archive_finish(writer);

    std::vector<uint8_t> bytes = read_file_bytes(path);
    size_t local = find_zip_signature(bytes, 0x04034b50U);
    size_t central = find_zip_signature(bytes, 0x02014b50U);
    assert(local + 30 <= central && central + 46 <= bytes.size());
    uint16_t name_len = (uint16_t)(bytes[local + 26] | ((uint16_t)bytes[local + 27] << 8));
    uint16_t extra_len = (uint16_t)(bytes[local + 28] | ((uint16_t)bytes[local + 29] << 8));
    size_t data_offset = local + 30 + (size_t)name_len + (size_t)extra_len;
    assert(data_offset <= central && central - data_offset < UINT32_MAX);
    uint32_t overlapping_size = (uint32_t)(central - data_offset + 1);
    write_u32_le(bytes, local + 18, overlapping_size);
    write_u32_le(bytes, local + 22, overlapping_size);
    write_u32_le(bytes, central + 20, overlapping_size);
    write_u32_le(bytes, central + 24, overlapping_size);

    void *zip_bytes = bytes_from_vector(bytes);
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    test_result("local payload overlap traps during parse", true);
    delete_file(path);
}

/// @brief Verify encoded, per-entry, and aggregate archive resource ceilings are enforced.
/// @details Builds a valid two-entry archive, then samples each documented
///          environment override in a fresh `FromBytes` construction. Every
///          deliberately undersized ceiling must trap before decompression or
///          a large allocation, and the caller's original environment is
///          restored afterward.
static void test_configured_archive_resource_limits() {
    printf("Testing Configured Archive Resource Limits:\n");

    const char *path = get_temp_path("test_archive_limits.zip");
    delete_file(path);
    void *writer = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(writer, rt_const_cstr("first.txt"), rt_const_cstr("0123456789abcdef"));
    rt_archive_add_str(writer, rt_const_cstr("second.txt"), rt_const_cstr("fedcba9876543210"));
    rt_archive_finish(writer);
    std::vector<uint8_t> encoded = read_file_bytes(path);
    void *zip_bytes = bytes_from_vector(encoded);

    const char *old_file_raw = getenv("ZANNA_ARCHIVE_MAX_FILE_BYTES");
    const char *old_entry_raw = getenv("ZANNA_ARCHIVE_MAX_ENTRY_BYTES");
    const char *old_total_raw = getenv("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES");
    char *old_file = copy_optional_cstr(old_file_raw);
    char *old_entry = copy_optional_cstr(old_entry_raw);
    char *old_total = copy_optional_cstr(old_total_raw);

    set_test_environment("ZANNA_ARCHIVE_MAX_FILE_BYTES", "16");
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    restore_test_environment("ZANNA_ARCHIVE_MAX_FILE_BYTES", old_file);

    set_test_environment("ZANNA_ARCHIVE_MAX_ENTRY_BYTES", "8");
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    restore_test_environment("ZANNA_ARCHIVE_MAX_ENTRY_BYTES", old_entry);

    set_test_environment("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES", "24");
    EXPECT_TRAP(rt_archive_from_bytes(zip_bytes));
    restore_test_environment("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES", old_total);

    const char *entry_path = get_temp_path("test_archive_writer_entry_limit.zip");
    delete_file(entry_path);
    set_test_environment("ZANNA_ARCHIVE_MAX_ENTRY_BYTES", "8");
    void *entry_writer = rt_archive_create(rt_const_cstr(entry_path));
    EXPECT_TRAP(rt_archive_add_str(
        entry_writer, rt_const_cstr("too-large.txt"), rt_const_cstr("0123456789abcdef")));
    assert(rt_archive_count(entry_writer) == 0);
    release_obj(entry_writer);
    restore_test_environment("ZANNA_ARCHIVE_MAX_ENTRY_BYTES", old_entry);

    const char *total_path = get_temp_path("test_archive_writer_total_limit.zip");
    delete_file(total_path);
    set_test_environment("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES", "24");
    void *total_writer = rt_archive_create(rt_const_cstr(total_path));
    rt_archive_add_str(total_writer, rt_const_cstr("first.txt"), rt_const_cstr("0123456789abcdef"));
    EXPECT_TRAP(rt_archive_add_str(
        total_writer, rt_const_cstr("second.txt"), rt_const_cstr("fedcba9876543210")));
    assert(rt_archive_count(total_writer) == 1);
    release_obj(total_writer);
    restore_test_environment("ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES", old_total);

    const char *encoded_path = get_temp_path("test_archive_writer_encoded_limit.zip");
    delete_file(encoded_path);
    set_test_environment("ZANNA_ARCHIVE_MAX_FILE_BYTES", "64");
    void *encoded_writer = rt_archive_create(rt_const_cstr(encoded_path));
    rt_archive_add_str(encoded_writer, rt_const_cstr("a"), rt_const_cstr("x"));
    EXPECT_TRAP(rt_archive_finish(encoded_writer));
    assert(rt_archive_count(encoded_writer) == 1);
    assert(rt_archive_is_zip(rt_const_cstr(encoded_path)) == 0);
    release_obj(encoded_writer);
    restore_test_environment("ZANNA_ARCHIVE_MAX_FILE_BYTES", old_file);

    free(old_file);
    free(old_entry);
    free(old_total);
    test_result("configured archive resource ceilings trap", true);
    delete_file(entry_path);
    delete_file(total_path);
    delete_file(encoded_path);
    delete_file(path);
}

/// @brief Exercise indexed duplicate checks and lookups with hundreds of entry names.
/// @details Adds 512 distinct stored entries, probes a duplicate near the end,
///          finishes and reopens the archive, then verifies every name through
///          the read-side hash index. This guards both index growth/rebuild and
///          collision probing without relying on internal structure mirrors.
static void test_large_entry_name_indexes() {
    printf("Testing Large Entry Name Indexes:\n");

    const char *path = get_temp_path("test_archive_name_index.zip");
    delete_file(path);
    void *writer = rt_archive_create(rt_const_cstr(path));
    for (int index = 0; index < 512; ++index) {
        char name[64];
        char value[32];
        snprintf(name, sizeof(name), "indexed/entry-%04d.txt", index);
        snprintf(value, sizeof(value), "value-%04d", index);
        rt_archive_add_str(writer, rt_const_cstr(name), rt_const_cstr(value));
    }
    EXPECT_TRAP(rt_archive_add_str(
        writer, rt_const_cstr("indexed/entry-0511.txt"), rt_const_cstr("duplicate")));
    rt_archive_finish(writer);

    void *reader = rt_archive_open(rt_const_cstr(path));
    assert(rt_archive_count(reader) == 512);
    for (int index = 0; index < 512; ++index) {
        char name[64];
        snprintf(name, sizeof(name), "indexed/entry-%04d.txt", index);
        assert(rt_archive_has(reader, rt_const_cstr(name)) == 1);
    }
    test_result("write/read archive name indexes retain all entries", true);
    delete_file(path);
}

/// @brief Verify concurrent writers and read-only property snapshots are serialized safely.
/// @details Four native threads add disjoint entry-name ranges while an observer
///          repeatedly calls Count and Names. After all joins, Finish and reopen
///          must expose the exact union. This stresses writer exclusion, shared
///          snapshots, index growth, and independent name ownership together.
static void test_concurrent_archive_writer_serialization() {
    printf("Testing Concurrent Archive Writer Serialization:\n");

    const char *path = get_temp_path("test_archive_concurrent_writer.zip");
    delete_file(path);
    void *writer = rt_archive_create(rt_const_cstr(path));
    constexpr int kThreads = 4;
    constexpr int kEntriesPerThread = 64;
    std::atomic<int> completed{0};

    std::thread observer([&]() {
        while (completed.load(std::memory_order_acquire) < kThreads) {
            int64_t count = rt_archive_count(writer);
            assert(count >= 0 && count <= kThreads * kEntriesPerThread);
            void *names = rt_archive_names(writer);
            int64_t name_count = rt_seq_len(names);
            assert(name_count >= 0 && name_count <= kThreads * kEntriesPerThread);
            release_obj(names);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index]() {
            for (int item = 0; item < kEntriesPerThread; ++item) {
                char name[80];
                char value[40];
                snprintf(
                    name, sizeof(name), "concurrent/thread-%d-entry-%03d.txt", thread_index, item);
                snprintf(value, sizeof(value), "thread-%d-value-%03d", thread_index, item);
                rt_archive_add_str(writer, rt_const_cstr(name), rt_const_cstr(value));
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::thread &worker : workers)
        worker.join();
    observer.join();

    assert(rt_archive_count(writer) == kThreads * kEntriesPerThread);
    rt_archive_finish(writer);
    void *reader = rt_archive_open(rt_const_cstr(path));
    test_result("concurrent writer retains exact entry union",
                rt_archive_count(reader) == kThreads * kEntriesPerThread);
    delete_file(path);
}

static void test_unsupported_zip_features_trap_on_open() {
    printf("Testing Unsupported ZIP Feature Rejection:\n");

    const char *path = get_temp_path("test_zip_unsupported_features.zip");
    delete_file(path);

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("entry.txt"), rt_const_cstr("payload"));
    rt_archive_finish(ar);

    std::vector<uint8_t> original = read_file_bytes(path);
    size_t cd = find_zip_signature(original, 0x02014b50U);
    assert(cd != original.size());

    std::vector<uint8_t> data_descriptor = original;
    write_u16_le(data_descriptor, cd + 8, 0x0008);
    EXPECT_TRAP(rt_archive_from_bytes(bytes_from_vector(data_descriptor)));

    std::vector<uint8_t> zip64_version = original;
    write_u16_le(zip64_version, cd + 6, 45);
    EXPECT_TRAP(rt_archive_from_bytes(bytes_from_vector(zip64_version)));

    std::vector<uint8_t> zip64_size = original;
    write_u32_le(zip64_size, cd + 20, UINT32_MAX);
    EXPECT_TRAP(rt_archive_from_bytes(bytes_from_vector(zip64_size)));

    test_result("unsupported ZIP features trap", true);
    delete_file(path);
}

#if !RT_PLATFORM_WINDOWS
static void test_extract_all_rejects_symlink_parent() {
    printf("Testing ExtractAll symlink parent rejection:\n");

    const char *zip_path = get_temp_path("test_extract_symlink.zip");
    const char *dest_dir = get_temp_path("test_extract_symlink_dest");
    const char *target_dir = get_temp_path("test_extract_symlink_target");
    delete_file(zip_path);
    rt_dir_remove_all(rt_const_cstr(dest_dir));
    rt_dir_remove_all(rt_const_cstr(target_dir));

    void *ar = rt_archive_create(rt_const_cstr(zip_path));
    rt_archive_add_str(ar, rt_const_cstr("link/evil.txt"), rt_const_cstr("payload"));
    rt_archive_finish(ar);

    mkdir_p(dest_dir);
    mkdir_p(target_dir);
    char link_path[512];
    char target_file[512];
    snprintf(link_path, sizeof(link_path), "%s/link", dest_dir);
    snprintf(target_file, sizeof(target_file), "%s/evil.txt", target_dir);
    (void)unlink(link_path);
    assert(symlink(target_dir, link_path) == 0);

    void *reader = rt_archive_open(rt_const_cstr(zip_path));
    EXPECT_TRAP(rt_archive_extract_all(reader, rt_const_cstr(dest_dir)));
    test_result("target file was not written", !file_equals_text(target_file, "payload"));

    unlink(link_path);
    rt_dir_remove_all(rt_const_cstr(dest_dir));
    rt_dir_remove_all(rt_const_cstr(target_dir));
    delete_file(zip_path);
}
#endif

/// @brief Verify a recovered entry-data trap closes every ExtractAll OS resource.
/// @details Corrupts a stored payload without changing its matching local and
///          central metadata, so open succeeds and CRC validation traps only
///          after ExtractAll owns its root and nested-parent descriptors. The
///          process resource count must return exactly to its baseline.
static void test_extract_all_trap_closes_resources() {
    printf("Testing ExtractAll Trap Resource Cleanup:\n");

    const char *archive_path = get_temp_path("test_archive_extract_cleanup.zip");
    const char *destination = get_temp_path("test_archive_extract_cleanup_dir");
    delete_file(archive_path);
    rt_dir_remove_all(rt_const_cstr(destination));

    void *writer = rt_archive_create(rt_const_cstr(archive_path));
    rt_archive_add_str(writer, rt_const_cstr("nested/file.txt"), rt_const_cstr("payload"));
    rt_archive_finish(writer);
    release_obj(writer);

    std::vector<uint8_t> encoded = read_file_bytes(archive_path);
    size_t local = find_zip_signature(encoded, 0x04034b50U);
    assert(local + 30 <= encoded.size());
    uint16_t name_len = (uint16_t)(encoded[local + 26] | ((uint16_t)encoded[local + 27] << 8));
    uint16_t extra_len = (uint16_t)(encoded[local + 28] | ((uint16_t)encoded[local + 29] << 8));
    size_t payload_offset = local + 30 + (size_t)name_len + (size_t)extra_len;
    assert(payload_offset < encoded.size());
    encoded[payload_offset] ^= 0x5aU;

    void *zip_bytes = bytes_from_vector(encoded);
    void *reader = rt_archive_from_bytes(zip_bytes);
    uint64_t before = process_open_resource_count();
    EXPECT_TRAP(rt_archive_extract_all(reader, rt_const_cstr(destination)));
    uint64_t after = process_open_resource_count();
    test_result("failed ExtractAll closes native resources", after == before);

    release_obj(reader);
    release_obj(zip_bytes);
    char nested_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(nested_path, sizeof(nested_path), "%s\\nested", destination);
#else
    snprintf(nested_path, sizeof(nested_path), "%s/nested", destination);
#endif
    (void)rmdir(nested_path);
    (void)rmdir(destination);
    delete_file(archive_path);
}

//=============================================================================
// Static Methods Tests
//=============================================================================

static void test_is_zip() {
    printf("Testing IsZip:\n");

    const char *zip_path = get_temp_path("test_iszip.zip");
    const char *txt_path = get_temp_path("test_iszip.txt");
    delete_file(zip_path);
    delete_file(txt_path);

    // Create a valid ZIP
    void *ar = rt_archive_create(rt_const_cstr(zip_path));
    rt_archive_add_str(ar, rt_const_cstr("test.txt"), rt_const_cstr("test"));
    rt_archive_finish(ar);

    // Create a non-ZIP file
    FILE *f = fopen(txt_path, "w");
    fprintf(f, "This is not a ZIP file");
    fclose(f);

    // Test IsZip
    test_result("IsZip on ZIP returns true", rt_archive_is_zip(rt_const_cstr(zip_path)) == 1);
    test_result("IsZip on TXT returns false", rt_archive_is_zip(rt_const_cstr(txt_path)) == 0);
    test_result("IsZip on missing returns false",
                rt_archive_is_zip(rt_const_cstr("/nonexistent/file.zip")) == 0);

    delete_file(zip_path);
    delete_file(txt_path);
}

static void test_is_zip_bytes() {
    printf("Testing IsZipBytes:\n");

    const char *path = get_temp_path("test_iszipbytes.zip");
    delete_file(path);

    // Create a valid ZIP
    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add_str(ar, rt_const_cstr("test.txt"), rt_const_cstr("test"));
    rt_archive_finish(ar);

    // Read into bytes
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *zip_bytes = rt_bytes_new(size);
    const size_t zip_read = fread(get_bytes_data(zip_bytes), 1, size, f);
    assert(zip_read == static_cast<size_t>(size));
    fclose(f);

    // Create non-ZIP bytes
    void *txt_bytes = make_bytes_str("Not a ZIP file");

    test_result("IsZipBytes on ZIP returns true", rt_archive_is_zip_bytes(zip_bytes) == 1);
    test_result("IsZipBytes on text returns false", rt_archive_is_zip_bytes(txt_bytes) == 0);
    void *bad = make_invalid_bytes_object(4);
    test_result("IsZipBytes on invalid Bytes returns false", rt_archive_is_zip_bytes(bad) == 0);
    release_obj(bad);

    delete_file(path);
}

static void test_eocd_signature_inside_comment_is_ignored() {
    printf("Testing EOCD Comment Signature Scan:\n");

    std::vector<uint8_t> bytes(26, 0);
    write_u32_le(bytes, 0, 0x06054b50U);
    write_u16_le(bytes, 20, 4);
    write_u32_le(bytes, 22, 0x06054b50U);

    void *zip_bytes = bytes_from_vector(bytes);
    test_result("IsZipBytes ignores comment signature", rt_archive_is_zip_bytes(zip_bytes) == 1);

    void *ar = rt_archive_from_bytes(zip_bytes);
    test_result("FromBytes opens commented empty zip", ar != NULL);
    test_result("Commented empty zip count", rt_archive_count(ar) == 0);
}

//=============================================================================
// Binary Data Tests
//=============================================================================

static void test_binary_data() {
    printf("Testing Binary Data:\n");

    const char *path = get_temp_path("test_binary.zip");
    delete_file(path);

    // Create binary data with all byte values
    void *binary = rt_bytes_new(256);
    uint8_t *data = get_bytes_data(binary);
    for (int i = 0; i < 256; i++) {
        data[i] = (uint8_t)i;
    }

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add(ar, rt_const_cstr("binary.bin"), binary);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    void *read_binary = rt_archive_read(ar2, rt_const_cstr("binary.bin"));
    test_result("Binary data round-trip", bytes_equal(binary, read_binary));

    delete_file(path);
}

static void test_large_file() {
    printf("Testing Large File:\n");

    const char *path = get_temp_path("test_large.zip");
    delete_file(path);

    // 100KB of data
    size_t size = 100 * 1024;
    void *large = rt_bytes_new((int64_t)size);
    uint8_t *data = get_bytes_data(large);
    for (size_t i = 0; i < size; i++) {
        data[i] = (uint8_t)(i & 0xFF);
    }

    void *ar = rt_archive_create(rt_const_cstr(path));
    rt_archive_add(ar, rt_const_cstr("large.bin"), large);
    rt_archive_finish(ar);

    void *ar2 = rt_archive_open(rt_const_cstr(path));
    void *read_large = rt_archive_read(ar2, rt_const_cstr("large.bin"));
    test_result("Large file round-trip", bytes_equal(large, read_large));

    delete_file(path);
}

//=============================================================================
// Entry Point
//=============================================================================

int main() {
    printf("=== RT Archive Tests ===\n\n");

    // Basic tests
    test_create_empty_archive();
    printf("\n");
    test_create_single_file();
    printf("\n");
    test_create_multiple_files();
    printf("\n");
    test_add_string();
    printf("\n");
    test_add_directory();
    test_extract_operations();
    printf("\n");
    test_invalid_entry_names();
    printf("\n");
    test_duplicate_and_null_entries_trap();
    printf("\n");

    // Compression tests
    test_compression_stored();
    printf("\n");
    test_compression_deflate();
    printf("\n");

    // Property tests
    test_properties();
    printf("\n");
    test_path_retained_after_caller_release();
    printf("\n");
    test_create_does_not_truncate_until_finish();
    printf("\n");
    test_finish_failure_is_retryable_and_cleans_sidecar();
    printf("\n");
    test_entry_info();
    printf("\n");

    // FromBytes tests
    test_from_bytes();
    printf("\n");
    test_stored_size_mismatch_traps();
    printf("\n");
    test_central_directory_count_mismatch_traps();
    printf("\n");
    test_central_directory_nul_name_traps();
    printf("\n");
    test_central_directory_duplicate_name_traps();
    printf("\n");
    test_corrupt_local_header_offset_traps_on_open();
    printf("\n");
    test_local_and_central_names_must_match();
    printf("\n");
    test_local_payload_cannot_overlap_central_directory();
    printf("\n");
    test_configured_archive_resource_limits();
    printf("\n");
    test_large_entry_name_indexes();
    printf("\n");
    test_concurrent_archive_writer_serialization();
    printf("\n");
    test_unsupported_zip_features_trap_on_open();
    printf("\n");
#if !RT_PLATFORM_WINDOWS
    test_extract_all_rejects_symlink_parent();
    printf("\n");
#endif
    test_extract_all_trap_closes_resources();
    printf("\n");

    // Static method tests
    test_is_zip();
    printf("\n");
    test_is_zip_bytes();
    printf("\n");
    test_eocd_signature_inside_comment_is_ignored();
    printf("\n");

    // Binary data tests
    test_binary_data();
    printf("\n");
    test_large_file();
    printf("\n");

    printf("All Archive tests passed!\n");
    return 0;
}
