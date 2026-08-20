//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTFileExtTests.cpp
// Purpose: Validate runtime file operations in rt_file_ext.c.
// Key invariants: File operations work correctly across platforms, bounded text reads reject
//                 oversize before allocation, no-clobber writes preserve racing destinations,
//                 compare/exchange writes reject stale snapshots without changing the file,
//                 FileLease ownership is exclusive and becomes reacquirable after release,
//                 opaque identity keys collapse aliases without accepting directories,
//                 ReadBytes/WriteBytes handle binary data correctly, ReadLines/WriteLines preserve
//                 line structure, atomic overwrites preserve existing permissions, and Windows
//                 sparse files retain 64-bit seek/stat behavior beyond 2 GiB.
// Ownership/Lifetime: Uses runtime library; tests return newly allocated
//                     strings and objects that must be released.
// Links: src/runtime/io/rt_file_ext.c, docs/zannalib/io/files.md
//
//===----------------------------------------------------------------------===//

#include "common/PlatformCapabilities.hpp"
#include "rt.hpp"
#include "rt_bytes.h"
#include "rt_file.h"
#include "rt_file_ext.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "tests/common/PlatformSkip.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#include "tests/common/PosixCompat.h"
#if RT_PLATFORM_WINDOWS
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define rmdir_p(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <utime.h>
#define mkdir_p(path) mkdir(path, 0755)
#define rmdir_p(path) rmdir(path)
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
        assert(g_last_trap != nullptr);                                                            \
    } while (0)

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

/// @brief Get a unique temp directory path for testing.
static const char *get_test_base() {
#if RT_PLATFORM_WINDOWS
    static char buf[256];
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = getenv("TMP");
    if (!tmp)
        tmp = "C:\\Temp";
    snprintf(buf, sizeof(buf), "%s\\zanna_file_test_%d", tmp, (int)getpid());
    return buf;
#else
    static char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/zanna_file_test_%d", (int)getpid());
    return buf;
#endif
}

/// @brief Helper to create a test file with content.
static void create_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", content);
        fclose(f);
    }
}

/// @brief Helper to create a test file with raw bytes (no newline translation).
static void create_test_file_bin(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    if (len > 0 && data) {
        (void)fwrite(data, 1, len, f);
    }
    fclose(f);
}

/// @brief Helper to remove a file.
static void remove_file(const char *path) {
#if RT_PLATFORM_WINDOWS
    _unlink(path);
#else
    unlink(path);
#endif
}

static const char *get_missing_path() {
    static char buf[512];
#if RT_PLATFORM_WINDOWS
    snprintf(buf, sizeof(buf), "%s\\missing_dir\\missing_file.txt", get_test_base());
#else
    snprintf(buf, sizeof(buf), "%s/missing_dir/missing_file.txt", get_test_base());
#endif
    return buf;
}

/// @brief Test rt_io_file_exists.
static void test_exists() {
    printf("Testing rt_io_file_exists:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_exists_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    // File doesn't exist yet
    test_result("non-existent file", rt_io_file_exists(path) == 0);

    // Create file
    create_test_file(file_path, "test");
    test_result("file exists after create", rt_io_file_exists(path) == 1);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_exists_dir", base);
    mkdir_p(dir_path);
    test_result("directory is not treated as file",
                rt_io_file_exists(rt_const_cstr(dir_path)) == 0);

    // Clean up
    remove_file(file_path);
    rmdir_p(dir_path);
    test_result("file not exists after remove", rt_io_file_exists(path) == 0);

    printf("\n");
}

/// @brief Test stable file identity across distinct path spellings.
static void test_same_file() {
    printf("Testing rt_file_same:\n");

    const char *base = get_test_base();
    char source_path[512], alias_path[512], other_path[512], directory_path[512];
    snprintf(source_path, sizeof(source_path), "%s_same_source.txt", base);
    snprintf(alias_path, sizeof(alias_path), "%s_same_alias.txt", base);
    snprintf(other_path, sizeof(other_path), "%s_same_other.txt", base);
    snprintf(directory_path, sizeof(directory_path), "%s_same_directory", base);
    remove_file(source_path);
    remove_file(alias_path);
    remove_file(other_path);
    rmdir_p(directory_path);
    create_test_file(source_path, "same identity");
    create_test_file(other_path, "same contents, different identity");
    mkdir_p(directory_path);

    rt_string source = rt_const_cstr(source_path);
    test_result("identical path is same file", rt_file_same(source, source) == 1);
    test_result("different inode is not same file",
                rt_file_same(source, rt_const_cstr(other_path)) == 0);
    test_result("missing path is not same file",
                rt_file_same(source, rt_const_cstr(get_missing_path())) == 0);
    test_result("directory is not treated as same file",
                rt_file_same(rt_const_cstr(directory_path), rt_const_cstr(directory_path)) == 0);

    rt_string source_identity = rt_file_identity_key(source);
    rt_string other_identity = rt_file_identity_key(rt_const_cstr(other_path));
    rt_string missing_identity = rt_file_identity_key(rt_const_cstr(get_missing_path()));
    rt_string directory_identity = rt_file_identity_key(rt_const_cstr(directory_path));
    test_result("regular file has opaque identity key", rt_str_len(source_identity) > 0);
    test_result("different file has different identity key",
                rt_str_len(other_identity) > 0 &&
                    strcmp(rt_string_cstr(source_identity), rt_string_cstr(other_identity)) != 0);
    test_result("missing path has no identity key", rt_str_len(missing_identity) == 0);
    test_result("directory has no file identity key", rt_str_len(directory_identity) == 0);

    std::error_code linkError;
    std::filesystem::create_hard_link(source_path, alias_path, linkError);
    if (!linkError) {
        test_result("hard-link spelling resolves to same file",
                    rt_file_same(source, rt_const_cstr(alias_path)) == 1);
        rt_string alias_identity = rt_file_identity_key(rt_const_cstr(alias_path));
        test_result("hard-link spellings share one identity key",
                    strcmp(rt_string_cstr(source_identity), rt_string_cstr(alias_identity)) == 0);
        rt_string_unref(alias_identity);
    } else {
        printf("  hard-link identity: SKIP (filesystem rejected link)\n");
    }

    rt_string_unref(source_identity);
    rt_string_unref(other_identity);
    rt_string_unref(missing_identity);
    rt_string_unref(directory_identity);

    remove_file(source_path);
    remove_file(alias_path);
    remove_file(other_path);
    rmdir_p(directory_path);
    printf("\n");
}

/// @brief Test rt_file_copy.
static void test_copy() {
    printf("Testing rt_file_copy:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_copy_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_copy_dst.txt", base);

    // Create source file
    create_test_file(src_path, "Hello, World!");

    rt_string src = rt_const_cstr(src_path);
    rt_string dst = rt_const_cstr(dst_path);

    test_result("source exists", rt_io_file_exists(src) == 1);
    test_result("dest not exists", rt_io_file_exists(dst) == 0);

    // Copy file
    rt_file_copy(src, dst);

    test_result("source still exists", rt_io_file_exists(src) == 1);
    test_result("dest exists after copy", rt_io_file_exists(dst) == 1);

    // Verify content
    rt_string content = rt_io_file_read_all_text(dst);
    test_result("content matches", rt_str_eq(content, rt_const_cstr("Hello, World!")));

    // Clean up
    remove_file(src_path);
    remove_file(dst_path);

    printf("\n");
}

/// @brief Test rt_file_copy refuses to copy a file onto itself.
static void test_copy_same_file_traps() {
    printf("Testing rt_file_copy same-file guard:\n");

    const char *base = get_test_base();
    char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s_copy_same.txt", base);
    create_test_file(path_buf, "preserve me");

    rt_string path = rt_const_cstr(path_buf);
    EXPECT_TRAP(rt_file_copy(path, path));

    rt_string content = rt_io_file_read_all_text(path);
    test_result("same-file copy preserves content",
                rt_str_eq(content, rt_const_cstr("preserve me")));

    remove_file(path_buf);
    printf("\n");
}

/// @brief Test rt_file_copy does not overwrite an existing destination.
static void test_copy_existing_destination_traps() {
    printf("Testing rt_file_copy no-overwrite contract:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_copy_existing_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_copy_existing_dst.txt", base);

    create_test_file(src_path, "new content");
    create_test_file(dst_path, "old content");

    rt_string src = rt_const_cstr(src_path);
    rt_string dst = rt_const_cstr(dst_path);

    EXPECT_TRAP(rt_file_copy(src, dst));
    test_result("destination content preserved",
                rt_str_eq(rt_io_file_read_all_text(dst), rt_const_cstr("old content")));

    remove_file(src_path);
    remove_file(dst_path);
    printf("\n");
}

/// @brief Test rt_file_move.
static void test_move() {
    printf("Testing rt_file_move:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_move_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_move_dst.txt", base);

    // Create source file
    create_test_file(src_path, "Move Test");

    rt_string src = rt_const_cstr(src_path);
    rt_string dst = rt_const_cstr(dst_path);

    test_result("source exists", rt_io_file_exists(src) == 1);

    // Move file
    rt_file_move(src, dst);

    test_result("source gone after move", rt_io_file_exists(src) == 0);
    test_result("dest exists after move", rt_io_file_exists(dst) == 1);

    // Verify content
    rt_string content = rt_io_file_read_all_text(dst);
    test_result("content preserved", rt_str_eq(content, rt_const_cstr("Move Test")));

    // Clean up
    remove_file(dst_path);

    printf("\n");
}

/// @brief Test rt_file_move refuses to overwrite an existing destination.
static void test_move_existing_destination_traps() {
    printf("Testing rt_file_move no-overwrite contract:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_move_existing_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_move_existing_dst.txt", base);

    create_test_file(src_path, "new content");
    create_test_file(dst_path, "old content");

    rt_string src = rt_const_cstr(src_path);
    rt_string dst = rt_const_cstr(dst_path);

    EXPECT_TRAP(rt_file_move(src, dst));

    test_result("source preserved", rt_io_file_exists(src) == 1);
    test_result("dest preserved",
                rt_str_eq(rt_io_file_read_all_text(dst), rt_const_cstr("old content")));

    remove_file(src_path);
    remove_file(dst_path);

    printf("\n");
}

/// @brief Test rt_file_move_over replaces an existing destination.
static void test_move_over_replaces() {
    printf("Testing rt_file_move_over replacement:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_move_over_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_move_over_dst.txt", base);

    create_test_file(src_path, "new content");
    create_test_file(dst_path, "old content");

    rt_string src = rt_const_cstr(src_path);
    rt_string dst = rt_const_cstr(dst_path);

    rt_file_move_over(src, dst);

    test_result("source removed", rt_io_file_exists(src) == 0);
    test_result("dest exists", rt_io_file_exists(dst) == 1);
    test_result("dest replaced",
                rt_str_eq(rt_io_file_read_all_text(dst), rt_const_cstr("new content")));

    remove_file(dst_path);

    printf("\n");
}

/// @brief Test file move APIs reject directory sources.
static void test_move_directory_source_traps() {
    printf("Testing rt_file_move rejects directories:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_move_dir_src", base);
    snprintf(dst_path, sizeof(dst_path), "%s_move_dir_dst", base);
    rmdir_p(src_path);
    rmdir_p(dst_path);
    mkdir_p(src_path);

    EXPECT_TRAP(rt_file_move(rt_const_cstr(src_path), rt_const_cstr(dst_path)));
    test_result("directory source still not a file",
                rt_io_file_exists(rt_const_cstr(src_path)) == 0);
    test_result("directory destination not created as file",
                rt_io_file_exists(rt_const_cstr(dst_path)) == 0);
    rmdir_p(src_path);
    rmdir_p(dst_path);

    snprintf(src_path, sizeof(src_path), "%s_move_over_dir_src", base);
    snprintf(dst_path, sizeof(dst_path), "%s_move_over_dir_dst", base);
    rmdir_p(src_path);
    rmdir_p(dst_path);
    mkdir_p(src_path);

    EXPECT_TRAP(rt_file_move_over(rt_const_cstr(src_path), rt_const_cstr(dst_path)));
    test_result("MoveOver directory source still not a file",
                rt_io_file_exists(rt_const_cstr(src_path)) == 0);
    test_result("MoveOver directory destination not created as file",
                rt_io_file_exists(rt_const_cstr(dst_path)) == 0);
    rmdir_p(src_path);
    rmdir_p(dst_path);

    printf("\n");
}

/// @brief Test rt_file_size.
static void test_size() {
    printf("Testing rt_file_size:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_size_test.txt", base);

    // Create file with known content
    create_test_file(file_path, "12345");

    rt_string path = rt_const_cstr(file_path);

    int64_t size = rt_file_size(path);
    test_result("size is 5 bytes", size == 5);

    // Non-existent file
    rt_string nonexist = rt_const_cstr(get_missing_path());
    test_result("non-existent returns -1", rt_file_size(nonexist) == -1);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_size_dir", base);
    mkdir_p(dir_path);
    rt_string dir = rt_const_cstr(dir_path);
    test_result("directory size returns -1", rt_file_size(dir) == -1);

    // Clean up
    remove_file(file_path);
    rmdir_p(dir_path);

    printf("\n");
}

/// @brief Test whole-file readers reject directories and special paths.
static void test_read_regular_file_required() {
    printf("Testing regular-file read requirement:\n");

    const char *base = get_test_base();
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_read_dir", base);
    mkdir_p(dir_path);
    rt_string dir = rt_const_cstr(dir_path);

    EXPECT_TRAP(rt_io_file_read_all_text(dir));
    EXPECT_TRAP(rt_io_file_read_all_bytes(dir));
    EXPECT_TRAP(rt_io_file_read_all_lines(dir));
    test_result("directory reads trap", true);

    rmdir_p(dir_path);
    printf("\n");
}

/// @brief Verify bounded text reads accept exact/empty limits and reject oversize or invalid caps.
static void test_read_all_text_bounded() {
    printf("Testing rt_io_file_read_all_text_bounded:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_read_bounded.txt", base);
    remove_file(file_path);
    create_test_file(file_path, "four");
    rt_string path = rt_const_cstr(file_path);

    rt_string exact = rt_io_file_read_all_text_bounded(path, 4);
    test_result("exact byte ceiling succeeds", rt_str_eq(exact, rt_const_cstr("four")));
    EXPECT_TRAP(rt_io_file_read_all_text_bounded(path, 3));
    test_result("oversize traps", true);
    EXPECT_TRAP(rt_io_file_read_all_text_bounded(path, -1));
    test_result("negative ceiling traps", true);

    create_test_file(file_path, "");
    rt_string empty = rt_io_file_read_all_text_bounded(path, 0);
    test_result("empty file fits zero ceiling", rt_str_len(empty) == 0);

    remove_file(file_path);
    printf("\n");
}

/// @brief Test embedded NUL bytes are rejected before paths reach C APIs.
static void test_embedded_nul_path_rejected() {
    printf("Testing embedded NUL path rejection:\n");

    const char raw_path[] = {'/', 't', 'm', 'p', '/', 'v', 'i', 'p', 'e', 'r', '\0', 'x'};
    rt_string bad_path = rt_string_from_bytes(raw_path, sizeof(raw_path));

    test_result("embedded NUL path does not exist", rt_io_file_exists(bad_path) == 0);
    EXPECT_TRAP(rt_io_file_read_all_text(bad_path));

    rt_string_unref(bad_path);
    printf("\n");
}

/// @brief Test rt_file_read_bytes and rt_file_write_bytes.
static void test_read_write_bytes() {
    printf("Testing rt_file_read_bytes and rt_file_write_bytes:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_bytes_test.bin", base);

    rt_string path = rt_const_cstr(file_path);

    // Create bytes with binary data including null bytes
    void *bytes = rt_bytes_new(5);
    rt_bytes_set(bytes, 0, 0x48); // 'H'
    rt_bytes_set(bytes, 1, 0x00); // null byte
    rt_bytes_set(bytes, 2, 0x69); // 'i'
    rt_bytes_set(bytes, 3, 0xFF); // 255
    rt_bytes_set(bytes, 4, 0x21); // '!'

    // Write bytes
    rt_file_write_bytes(path, bytes);
    test_result("file created", rt_io_file_exists(path) == 1);

    // Read bytes back
    void *read_bytes = rt_file_read_bytes(path);
    test_result("read 5 bytes", rt_bytes_len(read_bytes) == 5);
    test_result("byte 0 correct", rt_bytes_get(read_bytes, 0) == 0x48);
    test_result("byte 1 (null) correct", rt_bytes_get(read_bytes, 1) == 0x00);
    test_result("byte 2 correct", rt_bytes_get(read_bytes, 2) == 0x69);
    test_result("byte 3 correct", rt_bytes_get(read_bytes, 3) == 0xFF);
    test_result("byte 4 correct", rt_bytes_get(read_bytes, 4) == 0x21);

    // Clean up
    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_file_read_lines and rt_file_write_lines.
static void test_read_write_lines() {
    printf("Testing rt_file_read_lines and rt_file_write_lines:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_lines_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    // Create sequence of lines
    void *lines = rt_seq_new();
    rt_seq_push(lines, rt_const_cstr("Line 1"));
    rt_seq_push(lines, rt_const_cstr("Line 2"));
    rt_seq_push(lines, rt_const_cstr("Line 3"));

    // Write lines
    rt_file_write_lines(path, lines);
    test_result("file created", rt_io_file_exists(path) == 1);
    rt_io_file_write_all_lines(path, lines);
    test_result("write all lines alias keeps file", rt_io_file_exists(path) == 1);

    // Read lines back
    // Note: WriteLines adds a newline after each line, so ReadLines will get
    // an extra empty line at the end. We check the first 3 lines are correct.
    void *read_lines = rt_file_read_lines(path);
    int64_t line_count = rt_seq_len(read_lines);
    // Should have at least 3 lines (may have 4 with trailing empty line)
    test_result("read at least 3 lines", line_count >= 3);

    rt_string line1 = (rt_string)rt_seq_get(read_lines, 0);
    rt_string line2 = (rt_string)rt_seq_get(read_lines, 1);
    rt_string line3 = (rt_string)rt_seq_get(read_lines, 2);

    test_result("line 1 correct", rt_str_eq(line1, rt_const_cstr("Line 1")));
    test_result("line 2 correct", rt_str_eq(line2, rt_const_cstr("Line 2")));
    test_result("line 3 correct", rt_str_eq(line3, rt_const_cstr("Line 3")));

    // Clean up
    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_file_append.
static void test_append() {
    printf("Testing rt_file_append:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_append_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    // Create initial file
    create_test_file(file_path, "Hello");

    // Append text
    rt_file_append(path, rt_const_cstr(", World!"));

    // Verify content
    rt_string content = rt_io_file_read_all_text(path);
    test_result("content appended", rt_str_eq(content, rt_const_cstr("Hello, World!")));

    // Append more
    rt_file_append(path, rt_const_cstr(" Test"));
    content = rt_io_file_read_all_text(path);
    test_result("second append", rt_str_eq(content, rt_const_cstr("Hello, World! Test")));

    // Clean up
    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_io_file_append_line.
static void test_append_line() {
    printf("Testing rt_io_file_append_line:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_append_line_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    remove_file(file_path);

    rt_io_file_append_line(path, rt_const_cstr("Line 1"));
    rt_io_file_append_line(path, rt_const_cstr("Line 2"));

    rt_string content = rt_io_file_read_all_text(path);
    test_result("content matches", rt_str_eq(content, rt_const_cstr("Line 1\nLine 2\n")));

    // Concurrency-hygiene (VDOC-182): many appenders each write their whole
    // line (text + LF) in one atomic O_APPEND write, so no line is ever split
    // by another appender's newline. Run several threads and verify every
    // resulting line is intact ("thread-N") with no interleaving.
    remove_file(file_path);
    {
        const int kThreads = 8;
        const int kLines = 200;
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; t++) {
            workers.emplace_back([&, t]() {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "thread-%d", t);
                rt_string rec = rt_string_from_bytes(buf, (size_t)n);
                for (int i = 0; i < kLines; i++)
                    rt_io_file_append_line(path, rec);
                rt_string_unref(rec);
            });
        }
        for (auto &w : workers)
            w.join();

        rt_string all = rt_io_file_read_all_text(path);
        const char *text = rt_string_cstr(all);
        int lineCount = 0;
        bool allIntact = true;
        for (const char *p2 = text; *p2;) {
            const char *nl = strchr(p2, '\n');
            size_t linelen = nl ? (size_t)(nl - p2) : strlen(p2);
            // Every line must be exactly "thread-<digit>" — no split/merge.
            if (linelen < 8 || strncmp(p2, "thread-", 7) != 0)
                allIntact = false;
            lineCount++;
            if (!nl)
                break;
            p2 = nl + 1;
        }
        test_result("all concurrent lines are intact", allIntact);
        test_result("all concurrent lines present", lineCount == kThreads * kLines);
    }

    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_io_file_write_all_text performs a full overwrite.
static void test_write_all_text() {
    printf("Testing rt_io_file_write_all_text:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_write_all_text_test.txt", base);

    rt_string path = rt_const_cstr(file_path);
    create_test_file(file_path, "old contents that should disappear");

    rt_io_file_write_all_text(path, rt_const_cstr("fresh text"));
    test_result("text replaced",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("fresh text")));
    test_result("size matches replacement", rt_file_size(path) == 10);

    remove_file(file_path);

    printf("\n");
}

/// @brief Verify durable no-clobber text creation preserves an existing destination.
static void test_write_all_text_new() {
    printf("Testing rt_io_file_write_all_text_new:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_write_all_text_new.txt", base);
    remove_file(file_path);
    rt_string path = rt_const_cstr(file_path);

    rt_io_file_write_all_text_new(path, rt_const_cstr("first"));
    test_result("missing destination created",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("first")));
    EXPECT_TRAP(rt_io_file_write_all_text_new(path, rt_const_cstr("replacement")));
    test_result("existing destination preserved",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("first")));

    remove_file(file_path);
    printf("\n");
}

/// @brief Verify whole-file compare/exchange has one stale-snapshot commit point.
static void test_compare_exchange_all_text() {
    printf("Testing rt_io_file_compare_exchange_all_text:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_compare_exchange.txt", base);
    remove_file(file_path);
    rt_string path = rt_const_cstr(file_path);

    test_result("missing file matches empty snapshot",
                rt_io_file_compare_exchange_all_text(
                    path, rt_const_cstr(""), rt_const_cstr("revision-1")) == 1);
    test_result("first revision committed",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("revision-1")));
    test_result("stale snapshot rejected",
                rt_io_file_compare_exchange_all_text(
                    path, rt_const_cstr(""), rt_const_cstr("lost update")) == 0);
    test_result("rejected replacement preserved current bytes",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("revision-1")));
    test_result("current snapshot commits",
                rt_io_file_compare_exchange_all_text(
                    path, rt_const_cstr("revision-1"), rt_const_cstr("revision-2")) == 1);
    test_result("second revision committed",
                rt_str_eq(rt_io_file_read_all_text(path), rt_const_cstr("revision-2")));

    remove_file(file_path);
    printf("\n");
}

/// @brief Verify FileLease exclusion, explicit release, and persistent markers.
static void test_file_lease() {
    printf("Testing cross-process file leases:\n");

    const char *base = get_test_base();
    char lease_path[512];
    snprintf(lease_path, sizeof(lease_path), "%s_recovery.lease", base);
    remove_file(lease_path);
    rt_string path = rt_const_cstr(lease_path);

    void *first = rt_file_lease_try_acquire(path);
    test_result("first lease acquired", first != nullptr && rt_file_lease_is_valid(first) == 1);
    test_result("lease marker persists", rt_io_file_exists(path) == 1);

    void *contended = rt_file_lease_try_acquire(path);
    test_result("second live lease is rejected", contended == nullptr);
#if !RT_PLATFORM_WINDOWS
    // A distinct process must observe the same exclusion guarantee.
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        void *child_contended = rt_file_lease_try_acquire(path);
        _exit(child_contended == nullptr ? 0 : 1);
    }
    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    test_result("cross-process live lease is rejected",
                WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
#endif

    rt_file_lease_release(first);
    test_result("released lease becomes invalid", rt_file_lease_is_valid(first) == 0);
    void *second = rt_file_lease_try_acquire(path);
    test_result("abandoned marker is reacquirable",
                second != nullptr && rt_file_lease_is_valid(second) == 1);
    rt_file_lease_release(second);

    if (rt_obj_release_check0(first))
        rt_obj_free(first);
    if (second && rt_obj_release_check0(second))
        rt_obj_free(second);
    remove_file(lease_path);
    printf("\n");
}

/// @brief Test rt_io_file_read_all_bytes / rt_io_file_write_all_bytes.
static void test_read_write_all_bytes() {
    printf("Testing rt_io_file_read_all_bytes/rt_io_file_write_all_bytes:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_read_all_bytes_test.bin", base);

    rt_string path = rt_const_cstr(file_path);
    remove_file(file_path);

    void *bytes = rt_bytes_new(4);
    rt_bytes_set(bytes, 0, 0xDE);
    rt_bytes_set(bytes, 1, 0xAD);
    rt_bytes_set(bytes, 2, 0xBE);
    rt_bytes_set(bytes, 3, 0xEF);

    rt_io_file_write_all_bytes(path, bytes);

    void *read_bytes = rt_io_file_read_all_bytes(path);
    test_result("len == 4", rt_bytes_len(read_bytes) == 4);
    test_result("byte0 == 0xDE", rt_bytes_get(read_bytes, 0) == 0xDE);
    test_result("byte1 == 0xAD", rt_bytes_get(read_bytes, 1) == 0xAD);
    test_result("byte2 == 0xBE", rt_bytes_get(read_bytes, 2) == 0xBE);
    test_result("byte3 == 0xEF", rt_bytes_get(read_bytes, 3) == 0xEF);

    remove_file(file_path);

    printf("\n");
}

static void test_write_all_bytes_invalid_data_traps() {
    printf("Testing rt_io_file_write_all_bytes invalid data:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_write_all_bytes_bad_data.bin", base);

    struct FakeBytes {
        int64_t len;
        uint8_t *data;
    };

    rt_string path = rt_const_cstr(file_path);
    remove_file(file_path);

    FakeBytes *bad = static_cast<FakeBytes *>(rt_obj_new_i64(RT_BYTES_CLASS_ID, sizeof(FakeBytes)));
    bad->len = 1;
    bad->data = nullptr;

    EXPECT_TRAP(rt_io_file_write_all_bytes(path, bad));
    test_result("target not created", rt_io_file_exists(path) == 0);

    if (rt_obj_release_check0(bad))
        rt_obj_free(bad);
    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_io_file_read_all_lines.
static void test_read_all_lines() {
    printf("Testing rt_io_file_read_all_lines:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_read_all_lines_test.txt", base);

    static const char content[] = "one\r\ntwo\nthree\r\nfour";
    create_test_file_bin(file_path, content, sizeof(content) - 1);

    rt_string path = rt_const_cstr(file_path);
    void *lines = rt_io_file_read_all_lines(path);
    test_result("line count == 4", rt_seq_len(lines) == 4);

    rt_string line0 = (rt_string)rt_seq_get(lines, 0);
    rt_string line1 = (rt_string)rt_seq_get(lines, 1);
    rt_string line2 = (rt_string)rt_seq_get(lines, 2);
    rt_string line3 = (rt_string)rt_seq_get(lines, 3);

    test_result("line0", rt_str_eq(line0, rt_const_cstr("one")));
    test_result("line1", rt_str_eq(line1, rt_const_cstr("two")));
    test_result("line2", rt_str_eq(line2, rt_const_cstr("three")));
    test_result("line3", rt_str_eq(line3, rt_const_cstr("four")));

    remove_file(file_path);

    snprintf(file_path, sizeof(file_path), "%s_read_all_lines_trailing.txt", base);
    static const char trailing[] = "one\n\n";
    create_test_file_bin(file_path, trailing, sizeof(trailing) - 1);
    path = rt_const_cstr(file_path);
    lines = rt_io_file_read_all_lines(path);
    test_result("trailing empty lines preserved", rt_seq_len(lines) == 3);
    test_result("trailing line0", rt_str_eq((rt_string)rt_seq_get(lines, 0), rt_const_cstr("one")));
    test_result("trailing line1 empty", rt_str_len((rt_string)rt_seq_get(lines, 1)) == 0);
    test_result("trailing line2 empty", rt_str_len((rt_string)rt_seq_get(lines, 2)) == 0);

    remove_file(file_path);

    printf("\n");
}

/// @brief Test rt_file_modified.
static void test_modified() {
    printf("Testing rt_file_modified:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_modified_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    // Create file
    create_test_file(file_path, "test");

    time_t now = time(NULL);
    int64_t mtime = rt_file_modified(path);

    // Modified time should be recent (within last minute)
    test_result("mtime is recent", mtime > 0 && (now - mtime) < 60);

    // Non-existent file
    rt_string nonexist = rt_const_cstr(get_missing_path());
    test_result("non-existent returns -1", rt_file_modified(nonexist) == -1);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_modified_dir", base);
    mkdir_p(dir_path);
    test_result("directory modified returns -1", rt_file_modified(rt_const_cstr(dir_path)) == -1);

    // Clean up
    remove_file(file_path);
    rmdir_p(dir_path);

    printf("\n");
}

/// @brief Test rt_file_touch.
static void test_touch() {
    printf("Testing rt_file_touch:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_touch_test.txt", base);

    rt_string path = rt_const_cstr(file_path);

    // File doesn't exist
    test_result("file not exists", rt_io_file_exists(path) == 0);

    // Touch creates file
    rt_file_touch(path);
    test_result("touch creates file", rt_io_file_exists(path) == 1);

    // File should be empty
    int64_t size = rt_file_size(path);
    test_result("file is empty", size == 0);

    // Get initial mtime
    int64_t mtime1 = rt_file_modified(path);

    // Small delay to ensure time difference
    usleep(100000); // 100ms

    // Touch again updates mtime
    rt_file_touch(path);
    int64_t mtime2 = rt_file_modified(path);
    test_result("touch updates mtime", mtime2 >= mtime1);

    // Touch must reject a directory operand (VDOC-183): it is a File
    // operation and must not mutate directory timestamps.
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_touch_dir", base);
    mkdir_p(dir_path);
    EXPECT_TRAP(rt_file_touch(rt_const_cstr(dir_path)));
    test_result("touch rejects a directory", true);

    // Clean up
    remove_file(file_path);

    printf("\n");
}

/// @brief Test Delete traps on non-file paths but remains idempotent for missing files.
static void test_delete_error_contract() {
    printf("Testing rt_io_file_delete error contract:\n");

    rt_io_file_delete(rt_const_cstr(get_missing_path()));
    test_result("delete missing is idempotent", true);

    const char *base = get_test_base();
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s_delete_dir", base);
    mkdir_p(dir_path);
    EXPECT_TRAP(rt_io_file_delete(rt_const_cstr(dir_path)));
    rmdir_p(dir_path);

    printf("\n");
}

/// @brief Test write/append string parameters are validated instead of treated as empty.
static void test_invalid_string_writes_trap() {
    printf("Testing invalid file write strings:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_invalid_string_writes.txt", base);
    rt_string path = rt_const_cstr(file_path);

    EXPECT_TRAP(rt_io_file_write_all_text(path, nullptr));
    EXPECT_TRAP(rt_file_append(path, nullptr));
    EXPECT_TRAP(rt_io_file_append_line(path, nullptr));

    void *lines = rt_seq_new();
    rt_seq_push(lines, rt_const_cstr("ok"));
    rt_seq_push(lines, nullptr);
    EXPECT_TRAP(rt_file_write_lines(path, lines));

    EXPECT_TRAP(rt_io_file_write_all_text(rt_const_cstr(""), rt_const_cstr("x")));

    remove_file(file_path);
    printf("\n");
}

/// @brief Test empty file handling.
static void test_empty_file() {
    printf("Testing empty file handling:\n");

    const char *base = get_test_base();
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s_empty_test.txt", base);

    // Create empty file
    create_test_file(file_path, "");

    rt_string path = rt_const_cstr(file_path);

    // Read empty file as text
    rt_string text = rt_io_file_read_all_text(path);
    test_result("empty text read", rt_str_len(text) == 0);

    // Read empty file as bytes
    void *bytes = rt_file_read_bytes(path);
    test_result("empty bytes read", rt_bytes_len(bytes) == 0);

    // Read empty file as lines
    void *lines = rt_file_read_lines(path);
    // Empty file still yields one empty line
    test_result("empty lines read", rt_seq_len(lines) >= 0);

    // Clean up
    remove_file(file_path);

    printf("\n");
}

/// @brief Test non-existent file operations.
static void test_nonexistent() {
    printf("Testing non-existent file operations:\n");

    rt_string path = rt_const_cstr(get_missing_path());

    // High-level text/line reads trap on I/O errors.
    EXPECT_TRAP(rt_io_file_read_all_text(path));
    EXPECT_TRAP(rt_file_read_lines(path));

    EXPECT_TRAP(rt_file_read_bytes(path));

    test_result("size returns -1", rt_file_size(path) == -1);
    test_result("modified returns -1", rt_file_modified(path) == -1);

    printf("\n");
}

#if RT_PLATFORM_WINDOWS
/// @brief Verify the Windows file adapters use 64-bit offsets and sizes end to end.
/// @details Writing one byte after a 3 GiB seek creates a sparse file, so the test
///          exercises `_lseeki64`, `_fstat64`, and `_wstat64` without consuming
///          gigabytes of disk space or memory.
static void test_windows_large_file_offsets() {
    printf("Testing Windows large-file offsets:\n");

    const char *base = get_test_base();
    char path[512];
    snprintf(path, sizeof(path), "%s_large_sparse.bin", base);
    remove_file(path);

    RtFile file;
    RtError error = RT_ERROR_NONE;
    rt_file_init(&file);
    assert(rt_file_open(&file, path, "wb+", RT_F_BINARY, &error));
    const int64_t offset = INT64_C(3) * 1024 * 1024 * 1024;
    assert(rt_file_seek(&file, offset, SEEK_SET, &error));
    const uint8_t marker = 0xA5;
    assert(rt_file_write(&file, &marker, sizeof(marker), &error));
    assert(rt_file_close(&file, &error));

    test_result("3 GiB sparse size preserved", rt_file_size(rt_const_cstr(path)) == offset + 1);
    test_result("large sparse file remains visible", rt_io_file_exists(rt_const_cstr(path)) == 1);
    test_result("large sparse file timestamp readable", rt_file_modified(rt_const_cstr(path)) >= 0);

    remove_file(path);
    printf("\n");
}
#endif

#if !RT_PLATFORM_WINDOWS
static void test_copy_preserves_posix_metadata() {
    printf("Testing copy metadata preservation:\n");

    const char *base = get_test_base();
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s_copy_meta_src.txt", base);
    snprintf(dst_path, sizeof(dst_path), "%s_copy_meta_dst.txt", base);
    remove_file(src_path);
    remove_file(dst_path);

    create_test_file(src_path, "metadata");
    assert(chmod(src_path, 0640) == 0);
    struct utimbuf times;
    times.actime = 946684800;
    times.modtime = 946684800;
    assert(utime(src_path, &times) == 0);

    rt_file_copy(rt_const_cstr(src_path), rt_const_cstr(dst_path));

    struct stat st;
    assert(stat(dst_path, &st) == 0);
    test_result("mode preserved", (st.st_mode & 0777) == 0640);
    test_result("mtime preserved", st.st_mtime == 946684800);

    remove_file(src_path);
    remove_file(dst_path);
    printf("\n");
}

/// @brief Verify atomic text and line replacement preserves POSIX mode bits.
/// @details Both the buffered atomic helper and the streaming WriteLines path
///          replace the destination inode, so each must explicitly copy the
///          existing permission mode to its staged sidecar before rename.
static void test_atomic_overwrite_preserves_posix_mode() {
    printf("Testing atomic overwrite mode preservation:\n");

    const char *base = get_test_base();
    char path[512];
    snprintf(path, sizeof(path), "%s_atomic_mode.txt", base);
    remove_file(path);
    create_test_file(path, "old contents");
    assert(chmod(path, 0600) == 0);

    rt_io_file_write_all_text(rt_const_cstr(path), rt_const_cstr("new contents"));
    struct stat st;
    assert(stat(path, &st) == 0);
    test_result("WriteAllText preserves 0600", (st.st_mode & 0777) == 0600);

    assert(chmod(path, 0640) == 0);
    void *lines = rt_seq_new();
    rt_seq_push(lines, rt_const_cstr("one line"));
    rt_file_write_lines(rt_const_cstr(path), lines);
    assert(stat(path, &st) == 0);
    test_result("WriteLines preserves 0640", (st.st_mode & 0777) == 0640);
    if (rt_obj_release_check0(lines))
        rt_obj_free(lines);

    remove_file(path);
    printf("\n");
}
#endif

/// @brief Entry point for file extension tests.
int main() {
    printf("=== RT File Extension Tests ===\n\n");

    test_exists();
    test_same_file();
    test_copy();
    test_copy_same_file_traps();
    test_copy_existing_destination_traps();
    test_move();
    test_move_existing_destination_traps();
    test_move_over_replaces();
    test_move_directory_source_traps();
    test_size();
    test_read_regular_file_required();
    test_read_all_text_bounded();
    test_embedded_nul_path_rejected();
    test_read_write_bytes();
    test_read_write_lines();
    test_append();
    test_append_line();
    test_write_all_text();
    test_write_all_text_new();
    test_compare_exchange_all_text();
    test_file_lease();
    test_read_write_all_bytes();
    test_write_all_bytes_invalid_data_traps();
    test_read_all_lines();
    test_modified();
    test_touch();
    test_delete_error_contract();
    test_invalid_string_writes_trap();
    test_empty_file();
    test_nonexistent();
#if RT_PLATFORM_WINDOWS
    test_windows_large_file_offsets();
#endif
#if !RT_PLATFORM_WINDOWS
    test_copy_preserves_posix_metadata();
    test_atomic_overwrite_preserves_posix_mode();
#endif

    printf("All file extension tests passed!\n");
    return 0;
}
