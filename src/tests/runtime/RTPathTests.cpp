//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPathTests.cpp
// Purpose: Validate runtime path manipulation functions in rt_path.c.
// Key invariants: Path operations handle both Unix and Windows separators,
//                 normalize removes redundant components, and absolute detection
//                 considers platform conventions. Link inspection does not
//                 follow the final filesystem component, while SameEntry follows
//                 aliases and compares stable file or directory identity.
// Ownership/Lifetime: Uses runtime library; tests release newly allocated strings
//                     and remove their temporary filesystem fixtures.
// Links: src/runtime/io/rt_path.c, docs/zannalib/io/files.md,
//        docs/adr/0259-stable-filesystem-entry-identity.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_path.h"
#include "tests/common/PlatformSkip.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

/// @brief Test rt_path_join.
static void test_join() {
    printf("Testing rt_path_join:\n");

    // Basic join
    rt_string a = rt_const_cstr("/foo");
    rt_string b = rt_const_cstr("bar");
    rt_string r = rt_path_join(a, b);
    test_result("basic join", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

    // Empty first component
    a = rt_const_cstr("");
    b = rt_const_cstr("bar");
    r = rt_path_join(a, b);
    test_result("empty first", rt_str_eq(r, rt_const_cstr("bar")));
    rt_string_unref(r);

    // Empty second component
    a = rt_const_cstr("/foo");
    b = rt_const_cstr("");
    r = rt_path_join(a, b);
    test_result("empty second", rt_str_eq(r, rt_const_cstr("/foo")));
    rt_string_unref(r);

    // Second component is absolute (Unix)
    a = rt_const_cstr("/foo");
    b = rt_const_cstr("/bar");
    r = rt_path_join(a, b);
    test_result("second absolute", rt_str_eq(r, rt_const_cstr("/bar")));
    rt_string_unref(r);

    // Trailing separator in first
    a = rt_const_cstr("/foo/");
    b = rt_const_cstr("bar");
    r = rt_path_join(a, b);
    test_result("trailing sep", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

#ifndef _WIN32
    // A leading backslash is a valid relative filename byte on POSIX.
    a = rt_const_cstr("/foo");
    b = rt_const_cstr("\\bar");
    r = rt_path_join(a, b);
    test_result("posix leading backslash stays relative",
                rt_str_eq(r, rt_const_cstr("/foo/\\bar")));
    rt_string_unref(r);
#endif

    printf("\n");
}

/// @brief Test rt_path_dir.
static void test_dir() {
    printf("Testing rt_path_dir:\n");

    rt_string p = rt_const_cstr("/foo/bar/baz.txt");
    rt_string r = rt_path_dir(p);
    test_result("nested path", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

    p = rt_const_cstr("baz.txt");
    r = rt_path_dir(p);
    test_result("no directory", rt_str_eq(r, rt_const_cstr(".")));
    rt_string_unref(r);

    p = rt_const_cstr("/baz.txt");
    r = rt_path_dir(p);
    test_result("root file", rt_str_eq(r, rt_const_cstr("/")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/bar/");
    r = rt_path_dir(p);
    test_result("trailing separator", rt_str_eq(r, rt_const_cstr("/foo")));
    rt_string_unref(r);

    p = rt_const_cstr("/");
    r = rt_path_dir(p);
    test_result("root directory", rt_str_eq(r, rt_const_cstr("/")));
    rt_string_unref(r);

    p = rt_const_cstr("");
    r = rt_path_dir(p);
    test_result("empty path", rt_str_eq(r, rt_const_cstr("")));
    rt_string_unref(r);

#ifndef _WIN32
    p = rt_const_cstr("folder\\file.txt");
    r = rt_path_dir(p);
    test_result("posix backslash is not a directory separator", rt_str_eq(r, rt_const_cstr(".")));
    rt_string_unref(r);
#endif

    printf("\n");
}

/// @brief Test rt_path_name.
static void test_name() {
    printf("Testing rt_path_name:\n");

    rt_string p = rt_const_cstr("/foo/bar/baz.txt");
    rt_string r = rt_path_name(p);
    test_result("full path", rt_str_eq(r, rt_const_cstr("baz.txt")));
    rt_string_unref(r);

    p = rt_const_cstr("baz.txt");
    r = rt_path_name(p);
    test_result("filename only", rt_str_eq(r, rt_const_cstr("baz.txt")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/bar/");
    r = rt_path_name(p);
    test_result("trailing slash", rt_str_eq(r, rt_const_cstr("bar")));
    rt_string_unref(r);

    p = rt_const_cstr("");
    r = rt_path_name(p);
    test_result("empty path", rt_str_eq(r, rt_const_cstr("")));
    rt_string_unref(r);

#ifndef _WIN32
    p = rt_const_cstr("folder\\file.txt");
    r = rt_path_name(p);
    test_result("posix backslash remains in filename",
                rt_str_eq(r, rt_const_cstr("folder\\file.txt")));
    rt_string_unref(r);
#endif

    printf("\n");
}

/// @brief Test rt_path_stem.
static void test_stem() {
    printf("Testing rt_path_stem:\n");

    rt_string p = rt_const_cstr("/foo/bar/baz.txt");
    rt_string r = rt_path_stem(p);
    test_result("full path", rt_str_eq(r, rt_const_cstr("baz")));
    rt_string_unref(r);

    p = rt_const_cstr("file.tar.gz");
    r = rt_path_stem(p);
    test_result("multiple dots", rt_str_eq(r, rt_const_cstr("file.tar")));
    rt_string_unref(r);

    p = rt_const_cstr(".hidden");
    r = rt_path_stem(p);
    test_result("hidden file", rt_str_eq(r, rt_const_cstr(".hidden")));
    rt_string_unref(r);

    p = rt_const_cstr("noext");
    r = rt_path_stem(p);
    test_result("no extension", rt_str_eq(r, rt_const_cstr("noext")));
    rt_string_unref(r);

    printf("\n");
}

/// @brief Test rt_path_ext.
static void test_ext() {
    printf("Testing rt_path_ext:\n");

    rt_string p = rt_const_cstr("/foo/bar/baz.txt");
    rt_string r = rt_path_ext(p);
    test_result("full path", rt_str_eq(r, rt_const_cstr(".txt")));
    rt_string_unref(r);

    p = rt_const_cstr("file.tar.gz");
    r = rt_path_ext(p);
    test_result("multiple dots", rt_str_eq(r, rt_const_cstr(".gz")));
    rt_string_unref(r);

    p = rt_const_cstr(".hidden");
    r = rt_path_ext(p);
    test_result("hidden file", rt_str_eq(r, rt_const_cstr("")));
    rt_string_unref(r);

    p = rt_const_cstr("noext");
    r = rt_path_ext(p);
    test_result("no extension", rt_str_eq(r, rt_const_cstr("")));
    rt_string_unref(r);

    printf("\n");
}

/// @brief Test rt_path_with_ext.
static void test_with_ext() {
    printf("Testing rt_path_with_ext:\n");

    rt_string p = rt_const_cstr("/foo/bar.txt");
    rt_string e = rt_const_cstr(".md");
    rt_string r = rt_path_with_ext(p, e);
    test_result("replace ext with dot", rt_str_eq(r, rt_const_cstr("/foo/bar.md")));
    rt_string_unref(r);

    e = rt_const_cstr("md");
    r = rt_path_with_ext(p, e);
    test_result("replace ext without dot", rt_str_eq(r, rt_const_cstr("/foo/bar.md")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/bar");
    e = rt_const_cstr(".txt");
    r = rt_path_with_ext(p, e);
    test_result("add ext to no ext", rt_str_eq(r, rt_const_cstr("/foo/bar.txt")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/bar.txt");
    e = rt_const_cstr("");
    r = rt_path_with_ext(p, e);
    test_result("remove ext", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

    p = rt_const_cstr("");
    e = rt_const_cstr("txt");
    r = rt_path_with_ext(p, e);
    test_result("empty path adds leading dot", rt_str_eq(r, rt_const_cstr(".txt")));
    rt_string_unref(r);

    e = rt_const_cstr(".dat");
    r = rt_path_with_ext(p, e);
    test_result("empty path preserves leading dot", rt_str_eq(r, rt_const_cstr(".dat")));
    rt_string_unref(r);

    printf("\n");
}

/// @brief Test rt_path_is_abs.
static void test_is_abs() {
    printf("Testing rt_path_is_abs:\n");

    rt_string p = rt_const_cstr("/foo/bar");
    test_result("unix absolute", rt_path_is_abs(p) == 1);

    p = rt_const_cstr("foo/bar");
    test_result("relative", rt_path_is_abs(p) == 0);

    p = rt_const_cstr("");
    test_result("empty", rt_path_is_abs(p) == 0);

    printf("\n");
}

/// @brief Test rt_path_norm.
static void test_norm() {
    printf("Testing rt_path_norm:\n");

    rt_string p = rt_const_cstr("/foo//bar");
    rt_string r = rt_path_norm(p);
    test_result("double slash", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/./bar");
    r = rt_path_norm(p);
    test_result("dot component", rt_str_eq(r, rt_const_cstr("/foo/bar")));
    rt_string_unref(r);

    p = rt_const_cstr("/foo/bar/../baz");
    r = rt_path_norm(p);
    test_result("dotdot component", rt_str_eq(r, rt_const_cstr("/foo/baz")));
    rt_string_unref(r);

    p = rt_const_cstr("foo/../bar");
    r = rt_path_norm(p);
    test_result("relative dotdot", rt_str_eq(r, rt_const_cstr("bar")));
    rt_string_unref(r);

    p = rt_const_cstr("../foo");
    r = rt_path_norm(p);
    test_result("leading dotdot", rt_str_eq(r, rt_const_cstr("../foo")));
    rt_string_unref(r);

    p = rt_const_cstr("");
    r = rt_path_norm(p);
    test_result("empty path", rt_str_eq(r, rt_const_cstr(".")));
    rt_string_unref(r);

    p = rt_const_cstr("/");
    r = rt_path_norm(p);
    test_result("root only", rt_str_eq(r, rt_const_cstr("/")));
    rt_string_unref(r);

    printf("\n");
}

/// @brief Test rt_path_sep.
static void test_sep() {
    printf("Testing rt_path_sep:\n");

    rt_string r = rt_path_sep();
#ifdef _WIN32
    test_result("platform sep", rt_str_eq(r, rt_const_cstr("\\")));
#else
    test_result("platform sep", rt_str_eq(r, rt_const_cstr("/")));
#endif
    rt_string_unref(r);

    printf("\n");
}

/// @brief Test non-trapping link inspection and stable filesystem-entry identity.
static void test_is_link() {
    printf("Testing rt_path_is_link:\n");
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path(ec) / ("zanna_path_link_" + std::to_string(nonce));
    test_result("temporary path available", !ec && fs::create_directories(root, ec));
    fs::path target = root / "target.txt";
    fs::path other = root / "other.txt";
    fs::path link = root / "target-link";
    std::ofstream(target.string()) << "target";
    std::ofstream(other.string()) << "other";
    const std::string targetText = target.string();
    const std::string otherText = other.string();
    const std::string linkText = link.string();
    const std::string missingText = (root / "missing").string();
    const std::string rootText = root.string();

    test_result("ordinary file is not link",
                rt_path_is_link(rt_const_cstr(targetText.c_str())) == 0);
    test_result("missing path is not link",
                rt_path_is_link(rt_const_cstr(missingText.c_str())) == 0);
    test_result("identical file path is same entry",
                rt_path_same_entry(rt_const_cstr(targetText.c_str()),
                                   rt_const_cstr(targetText.c_str())) == 1);
    test_result("different files are different entries",
                rt_path_same_entry(rt_const_cstr(targetText.c_str()),
                                   rt_const_cstr(otherText.c_str())) == 0);
    test_result(
        "directory identity is supported",
        rt_path_same_entry(rt_const_cstr(rootText.c_str()), rt_const_cstr(rootText.c_str())) == 1);
    test_result("missing entry compares unequal",
                rt_path_same_entry(rt_const_cstr(targetText.c_str()),
                                   rt_const_cstr(missingText.c_str())) == 0);
    fs::create_symlink(target, link, ec);
    if (!ec) {
        test_result("symbolic link detected",
                    rt_path_is_link(rt_const_cstr(linkText.c_str())) == 1);
        test_result("same-entry identity follows symbolic links",
                    rt_path_same_entry(rt_const_cstr(targetText.c_str()),
                                       rt_const_cstr(linkText.c_str())) == 1);
    } else {
        printf("  symbolic link detected: SKIP (%s)\n", ec.message().c_str());
    }
    ec.clear();
    fs::remove_all(root, ec);
    printf("\n");
}

/// @brief Entry point for path tests.
/// @brief Path.Absolute makes a relative path absolute and normalized (POSIX
///        path — the Windows GetFullPathNameW resolution is covered by a
///        Windows build). Guards the VDOC-184 refactor's POSIX branch.
static void test_abs() {
    printf("Testing rt_path_abs:\n");
    rt_string rel = rt_const_cstr("a/b/../c/./file.txt");
    rt_string abs = rt_path_abs(rel);
    const char *result = rt_string_cstr(abs);
    // Result is absolute (starts with '/') and the ./.. are normalized away.
    test_result("relative becomes absolute", result[0] == '/');
    test_result("relative is normalized",
                strstr(result, "/a/c/file.txt") != nullptr && strstr(result, "..") == nullptr);
    // An already-absolute path round-trips (normalized).
    rt_string already = rt_const_cstr("/tmp/x/../y");
    rt_string abs2 = rt_path_abs(already);
    test_result("absolute normalizes in place", strcmp(rt_string_cstr(abs2), "/tmp/y") == 0);
    rt_string_unref(abs);
    rt_string_unref(abs2);
}

int main() {
#ifdef _WIN32
    // Skip on Windows: tests use Unix-style paths (/foo/bar) that have different
    // semantics on Windows (not considered absolute paths)
    ZANNA_PLATFORM_SKIP("Unix path conventions not applicable on Windows");
#endif
    printf("=== RT Path Tests ===\n\n");

    test_join();
    test_dir();
    test_name();
    test_stem();
    test_ext();
    test_with_ext();
    test_is_abs();
    test_abs();
    test_norm();
    test_sep();
    test_is_link();

    printf("All path tests passed!\n");
    return 0;
}
