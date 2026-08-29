//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/tests/runtime/RTIdeWorkspaceTests.cpp
// Purpose: Tests for paged directory I/O plus IDE workspace, asset resolver,
//          manifest, and edit helpers.
// Key invariants:
//   - Runtime workspace helpers return structured maps/sequences with stable keys.
//   - Temporary files are isolated under per-test directories and removed on success.
// Ownership/Lifetime:
//   - Runtime strings created by tests are unref'd by the creating test.
//   - Runtime map/sequence objects are owned by the runtime test process.
// Links: src/runtime/io/rt_dir_page.cpp,
//        src/runtime/io/rt_ide_primitives.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_dir.h"
#include "rt_ide_primitives.h"
#include "rt_watcher.h"

#include "rt_internal.h"
#include "rt_map.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if RT_PLATFORM_LINUX
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#endif

#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
#include <sys/xattr.h>
#endif

#if RT_PLATFORM_MACOS && defined(_POSIX_C_SOURCE)
extern "C" int chflags(const char *, uint32_t);
#endif

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

namespace fs = std::filesystem;

static rt_string s(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

static std::string get_str(void *map, const char *key) {
    rt_string value = rt_map_get_str(map, rt_const_cstr(key));
    std::string out(rt_string_cstr(value), (size_t)rt_str_len(value));
    rt_string_unref(value);
    return out;
}

static void write_file(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    // A silent failure here (Windows refuses to truncate a hidden file through
    // the standard streams) would surface much later as an unrelated edit-range
    // or content mismatch, so fixture writes fail loudly instead.
    assert(out.is_open());
    out << text;
    out.flush();
    assert(out.good());
}

static std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

static fs::path temp_root() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("zanna_ide_workspace_" + std::to_string(now));
    fs::create_directories(root);
    return root;
}

static bool seq_contains_relative(void *seq, const std::string &rel) {
    for (int64_t i = 0; i < rt_seq_len(seq); i++) {
        void *entry = rt_seq_get(seq, i);
        if (get_str(entry, "relativePath") == rel)
            return true;
    }
    return false;
}

static void *seq_find_relative(void *seq, const std::string &rel) {
    for (int64_t i = 0; i < rt_seq_len(seq); i++) {
        void *entry = rt_seq_get(seq, i);
        if (get_str(entry, "relativePath") == rel)
            return entry;
    }
    return nullptr;
}

static void test_directory_page() {
    fs::path root = temp_root();
    fs::create_directories(root / "folder");
    write_file(root / "alpha.zia", "alpha");
    write_file(root / "beta.zia", "beta");
    write_file(root / "gamma.zia", "gamma");
    write_file(root / "delta.zia", "delta");

    rt_string rootString = s(root.string());
    std::set<std::string> names;
    int64_t offset = 0;
    bool done = false;
    int pageCount = 0;
    while (!done) {
        void *page = rt_dir_page(rootString, offset, 2);
        assert(rt_map_get_bool(page, rt_const_cstr("valid")) == 1);
        assert(rt_map_get_int(page, rt_const_cstr("offset")) == offset);
        assert(rt_map_get_int(page, rt_const_cstr("limit")) == 2);
        assert(get_str(page, "path") == fs::absolute(root).lexically_normal().generic_string());
        assert(rt_seq_len(rt_map_get(page, rt_const_cstr("diagnostics"))) == 0);

        void *entries = rt_map_get(page, rt_const_cstr("entries"));
        assert(rt_seq_len(entries) <= 2);
        for (int64_t i = 0; i < rt_seq_len(entries); i++) {
            void *entry = rt_seq_get(entries, i);
            const std::string name = get_str(entry, "name");
            const std::string kind = get_str(entry, "kind");
            assert(!name.empty());
            assert(kind == "file" || kind == "directory" || kind == "other");
            assert(!get_str(entry, "path").empty());
            names.insert(name);
        }
        const int64_t nextOffset = rt_map_get_int(page, rt_const_cstr("nextOffset"));
        assert(nextOffset > offset || rt_map_get_bool(page, rt_const_cstr("done")) == 1);
        offset = nextOffset;
        done = rt_map_get_bool(page, rt_const_cstr("done")) == 1;
        pageCount++;
        assert(pageCount < 10);
    }
    assert(pageCount >= 3);
    assert(names.size() == 5);
    assert(names.count("folder") == 1);
    assert(names.count("delta.zia") == 1);

    void *clamped = rt_dir_page(rootString, 0, 100000);
    assert(rt_map_get_int(clamped, rt_const_cstr("limit")) == 4096);

    rt_string missing = s((root / "missing").string());
    void *invalid = rt_dir_page(missing, 0, 2);
    assert(rt_map_get_bool(invalid, rt_const_cstr("valid")) == 0);
    assert(rt_seq_len(rt_map_get(invalid, rt_const_cstr("diagnostics"))) > 0);

    rt_string_unref(missing);
    rt_string_unref(rootString);
    fs::remove_all(root);
}

static void test_file_index_and_ignore() {
    fs::path root = temp_root();
    write_file(root / ".gitignore", "*.tmp\n!keep.tmp\nignored/\n");
    write_file(root / "src/.gitignore", "*.gen.zia\n!keep.gen.zia\n\\#literal.zia\n");
    write_file(root / "src/main.zia", "module Main;\n");
    write_file(root / "src/generated.gen.zia", "skip");
    write_file(root / "src/keep.gen.zia", "keep");
    write_file(root / "src/#literal.zia", "skip");
    write_file(root / "assets/tiles.png", "png");
    write_file(root / "build/generated.zia", "skip");
    write_file(root / ".cache/generated.zia", "skip");
    write_file(root / ".claude/worktrees/agent/src/copied.zia", "skip");
    write_file(root / ".secret.zia", "skip");
    write_file(root / "ignored/hidden.zia", "skip");
    write_file(root / "keep.tmp", "keep");

    rt_string root_s = s(root.string());
    void *entries = rt_workspace_file_index_enumerate(
        root_s, rt_const_cstr(".zia,.png,.tmp"), rt_const_cstr(""), 1);
    void *status = rt_workspace_file_index_status(
        root_s, rt_const_cstr(".zia,.png,.tmp"), rt_const_cstr(""), 1);
    assert(rt_map_get_bool(status, rt_const_cstr("valid")) == 1);
    assert(rt_map_get_int(status, rt_const_cstr("entryCount")) == rt_seq_len(entries));
    assert(rt_map_get_bool(status, rt_const_cstr("truncated")) == 0);
    assert(rt_map_get_int(status, rt_const_cstr("maxEntries")) > 0);
    assert(rt_seq_len(rt_map_get(status, rt_const_cstr("diagnostics"))) == 0);

    void *first_page = rt_workspace_file_index_page(
        root_s, rt_const_cstr(".zia,.png,.tmp"), rt_const_cstr(""), 1, 0, 2);
    assert(rt_map_get_bool(first_page, rt_const_cstr("valid")) == 1);
    assert(rt_map_get_int(first_page, rt_const_cstr("offset")) == 0);
    assert(rt_map_get_int(first_page, rt_const_cstr("emitted")) == 2);
    assert(rt_map_get_bool(first_page, rt_const_cstr("done")) == 0);
    void *first_entries = rt_map_get(first_page, rt_const_cstr("entries"));
    assert(rt_seq_len(first_entries) == 2);
    int64_t next_offset = rt_map_get_int(first_page, rt_const_cstr("nextOffset"));
    assert(next_offset == 2);

    void *second_page = rt_workspace_file_index_page(
        root_s, rt_const_cstr(".zia,.png,.tmp"), rt_const_cstr(""), 1, next_offset, 64);
    assert(rt_map_get_bool(second_page, rt_const_cstr("valid")) == 1);
    assert(rt_map_get_int(second_page, rt_const_cstr("offset")) == next_offset);
    assert(rt_map_get_bool(second_page, rt_const_cstr("done")) == 1);
    void *second_entries = rt_map_get(second_page, rt_const_cstr("entries"));
    assert(rt_seq_len(first_entries) + rt_seq_len(second_entries) == rt_seq_len(entries));

    void *cursor = rt_workspace_file_index_cursor_new(
        root_s, rt_const_cstr(".zia,.png,.tmp"), rt_const_cstr(""), 1);
    assert(rt_workspace_file_index_cursor_is_valid(cursor) == 1);
    const int64_t cursor_generation = rt_workspace_file_index_cursor_generation(cursor);
    assert(cursor_generation > 0);
    void *cursor_first = rt_workspace_file_index_cursor_next(cursor, 2);
    assert(rt_map_get_bool(cursor_first, rt_const_cstr("valid")) == 1);
    assert(rt_map_get_int(cursor_first, rt_const_cstr("generation")) == cursor_generation);
    assert(rt_map_get_int(cursor_first, rt_const_cstr("emitted")) == 2);
    assert(rt_map_get_int(cursor_first, rt_const_cstr("maxEntries")) == 0);
    assert(rt_map_get_bool(cursor_first, rt_const_cstr("truncated")) == 0);

    // More than the former eight-cache-entry limit cannot evict an explicitly
    // owned traversal. Every independent cursor also receives its own generation.
    std::vector<void *> interleaved;
    std::set<int64_t> generations;
    for (int i = 0; i < 12; ++i) {
        void *other =
            rt_workspace_file_index_cursor_new(root_s, rt_const_cstr(".zia"), rt_const_cstr(""), 0);
        assert(other != nullptr);
        generations.insert(rt_workspace_file_index_cursor_generation(other));
        void *other_page = rt_workspace_file_index_cursor_next(other, 1);
        assert(rt_map_get_bool(other_page, rt_const_cstr("valid")) == 1);
        interleaved.push_back(other);
    }
    assert(generations.size() == interleaved.size());
    void *cursor_rest = rt_workspace_file_index_cursor_next(cursor, 4096);
    assert(rt_map_get_int(cursor_rest, rt_const_cstr("generation")) == cursor_generation);
    assert(rt_map_get_bool(cursor_rest, rt_const_cstr("done")) == 1);
    assert(rt_map_get_int(cursor_first, rt_const_cstr("emitted")) +
               rt_map_get_int(cursor_rest, rt_const_cstr("emitted")) ==
           rt_seq_len(entries));
    for (void *other : interleaved)
        rt_workspace_file_index_cursor_destroy(other);
    rt_workspace_file_index_cursor_destroy(cursor);
    assert(rt_workspace_file_index_cursor_is_valid(cursor) == 0);
    assert(rt_workspace_file_index_cursor_generation(cursor) == 0);
    void *destroyed_page = rt_workspace_file_index_cursor_next(cursor, 1);
    assert(rt_map_get_bool(destroyed_page, rt_const_cstr("valid")) == 0);
    rt_workspace_file_index_cursor_destroy(cursor);

    // Fallback-watcher pages request all extensions plus directories. Those
    // rows include precise metadata and a bounded content sample so a same-size
    // rewrite inside one whole-second timestamp cannot disappear.
    void *fingerprint_page =
        rt_workspace_file_index_page(root_s, rt_const_cstr(""), rt_const_cstr(""), 1, 0, 4096);
    assert(rt_map_get_bool(fingerprint_page, rt_const_cstr("valid")) == 1);
    assert(rt_map_get_bool(fingerprint_page, rt_const_cstr("done")) == 1);
    void *fingerprint_entries = rt_map_get(fingerprint_page, rt_const_cstr("entries"));
    void *main_entry = seq_find_relative(fingerprint_entries, "src/main.zia");
    assert(main_entry != nullptr);
    assert(rt_map_get_int(main_entry, rt_const_cstr("size")) > 0);
    assert(rt_map_get_int(main_entry, rt_const_cstr("modifiedNs")) != -1);
    const int64_t first_sample = rt_map_get_int(main_entry, rt_const_cstr("sampleHash"));
    assert(first_sample >= 0);

    std::error_code rewrite_time_error;
    const auto original_main_mtime = fs::last_write_time(root / "src/main.zia", rewrite_time_error);
    assert(!rewrite_time_error);
    write_file(root / "src/main.zia", "module Mine;\n");
    fs::last_write_time(root / "src/main.zia", original_main_mtime, rewrite_time_error);
    assert(!rewrite_time_error);
    assert(fs::last_write_time(root / "src/main.zia", rewrite_time_error) == original_main_mtime);
    assert(!rewrite_time_error);
    void *rewritten_page =
        rt_workspace_file_index_page(root_s, rt_const_cstr(""), rt_const_cstr(""), 1, 0, 4096);
    void *rewritten_entries = rt_map_get(rewritten_page, rt_const_cstr("entries"));
    void *rewritten_main = seq_find_relative(rewritten_entries, "src/main.zia");
    assert(rewritten_main != nullptr);
    assert(rt_map_get_int(rewritten_main, rt_const_cstr("size")) ==
           rt_map_get_int(main_entry, rt_const_cstr("size")));
    assert(rt_map_get_int(rewritten_main, rt_const_cstr("sampleHash")) != first_sample);

    rt_string invalid_root = s((root / "missing-page").string());
    void *invalid_page = rt_workspace_file_index_page(
        invalid_root, rt_const_cstr(".zia"), rt_const_cstr(""), 0, 0, 10);
    assert(rt_map_get_bool(invalid_page, rt_const_cstr("valid")) == 0);
    assert(rt_seq_len(rt_map_get(invalid_page, rt_const_cstr("diagnostics"))) > 0);
    rt_string_unref(invalid_root);

    rt_string missing_root = s((root / "missing").string());
    void *missing_status =
        rt_workspace_file_index_status(missing_root, rt_const_cstr(".zia"), rt_const_cstr(""), 0);
    assert(rt_map_get_bool(missing_status, rt_const_cstr("valid")) == 0);
    assert(rt_seq_len(rt_map_get(missing_status, rt_const_cstr("diagnostics"))) > 0);
    rt_string_unref(missing_root);

    assert(seq_contains_relative(entries, "src/main.zia"));
    assert(seq_contains_relative(entries, "src/keep.gen.zia"));
    assert(!seq_contains_relative(entries, "src/generated.gen.zia"));
    assert(!seq_contains_relative(entries, "src/#literal.zia"));
    assert(seq_contains_relative(entries, "assets/tiles.png"));
    assert(seq_contains_relative(entries, "keep.tmp"));
    assert(!seq_contains_relative(entries, "build/generated.zia"));
    assert(!seq_contains_relative(entries, ".cache/generated.zia"));
    assert(!seq_contains_relative(entries, ".claude/worktrees/agent/src/copied.zia"));
    assert(!seq_contains_relative(entries, ".secret.zia"));
    assert(!seq_contains_relative(entries, "ignored/hidden.zia"));

    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("tmp/cache.bin"), rt_const_cstr("tmp/")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("keep.tmp"), rt_const_cstr("*.tmp,!keep.tmp")) == 0);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("#literal.zia"), rt_const_cstr("\\#literal.zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("!literal.zia"), rt_const_cstr("\\!literal.zia")) == 1);

    // VDOC-192: `**/` matches whole path components, not a mid-component suffix.
    // A component that merely ENDS with the literal must NOT be ignored.
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("foobar"), rt_const_cstr("**/bar")) == 0);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("foo/xbar"), rt_const_cstr("foo/**/bar")) == 0);
    // But a real component named `bar` at any depth (zero or more components) IS
    // ignored.
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("bar"), rt_const_cstr("**/bar")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("a/b/bar"), rt_const_cstr("**/bar")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("foo/bar"), rt_const_cstr("foo/**/bar")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("foo/x/bar"), rt_const_cstr("foo/**/bar")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("src/main.zia"), rt_const_cstr("src/*.zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("src/nested/main.zia"), rt_const_cstr("src/*.zia")) == 0);
    assert(rt_workspace_file_index_should_ignore(root_s,
                                                 rt_const_cstr("src/nested/main.zia"),
                                                 rt_const_cstr("src/**/main.zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("file7.zia"), rt_const_cstr("file[0-9].zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("filex.zia"), rt_const_cstr("file[!0-9].zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("literal*.zia"), rt_const_cstr("literal\\*.zia")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("literalx.zia"), rt_const_cstr("literal\\*.zia")) == 0);
    rt_string_unref(root_s);
    fs::remove_all(root);
}

/// @brief A result limit must also bound raw traversal work when no files match.
static void test_file_index_cursor_work_budget() {
    fs::path root = temp_root();
    for (int i = 0; i < 200; ++i)
        write_file(root / ("candidate_" + std::to_string(i) + ".txt"), "skip");
    rt_string root_s = s(root.string());
    void *cursor =
        rt_workspace_file_index_cursor_new(root_s, rt_const_cstr(".zia"), rt_const_cstr(""), 0);
    assert(rt_workspace_file_index_cursor_is_valid(cursor) == 1);

    int64_t priorScanned = 0;
    bool done = false;
    int pages = 0;
    while (!done) {
        void *page = rt_workspace_file_index_cursor_next(cursor, 1);
        assert(rt_map_get_bool(page, rt_const_cstr("valid")) == 1);
        const int64_t work = rt_map_get_int(page, rt_const_cstr("work"));
        const int64_t scanned = rt_map_get_int(page, rt_const_cstr("scanned"));
        assert(work > 0 && work <= 64);
        assert(scanned == priorScanned + work);
        assert(rt_map_get_int(page, rt_const_cstr("emitted")) == 0);
        priorScanned = scanned;
        done = rt_map_get_bool(page, rt_const_cstr("done")) == 1;
        pages++;
        assert(pages < 10);
    }
    assert(priorScanned == 200);
    assert(pages >= 4);
    rt_workspace_file_index_cursor_destroy(cursor);
    rt_string_unref(root_s);
    fs::remove_all(root);
}

static void test_asset_resolver_and_manifest() {
    fs::path root = temp_root();
    write_file(root / "scenes/level.json", "{}");
    write_file(root / "scenes/local.png", "local");
    write_file(root / "assets/tiles.png", "tiles");

    rt_string scene = s((root / "scenes/level.json").string());
    rt_string project = s(root.string());
    void *scene_relative = rt_asset_resolver_resolve(
        scene, project, rt_const_cstr("assets"), rt_const_cstr("local.png"));
    assert(rt_map_get_bool(scene_relative, rt_const_cstr("found")) == 1);
    assert(get_str(scene_relative, "source") == "scene");

    void *asset_root = rt_asset_resolver_resolve(
        scene, project, rt_const_cstr("assets"), rt_const_cstr("tiles.png"));
    assert(rt_map_get_bool(asset_root, rt_const_cstr("found")) == 1);
    assert(get_str(asset_root, "source") == "assetRoot");

    void *missing = rt_asset_resolver_resolve(
        scene, project, rt_const_cstr("assets"), rt_const_cstr("missing.png"));
    assert(rt_map_get_bool(missing, rt_const_cstr("found")) == 0);
    assert(get_str(missing, "diagnostic").find("missing.png") != std::string::npos);

    // VDOC-197: an empty asset name must not resolve to the project directory
    // itself — it is rejected as not found with an explicit diagnostic.
    void *empty_asset =
        rt_asset_resolver_resolve(scene, project, rt_const_cstr("assets"), rt_const_cstr(""));
    assert(rt_map_get_bool(empty_asset, rt_const_cstr("found")) == 0);
    assert(rt_map_get_bool(empty_asset, rt_const_cstr("exists")) == 0);
    assert(get_str(empty_asset, "diagnostic").find("empty asset name") != std::string::npos);

    // VDOC-197: a RELATIVE scene path is resolved against the project root, not
    // the process CWD, so scene-relative resolution is CWD-independent. Passing
    // the project-relative scene path resolves the scene-local asset the same
    // way the absolute scene path did above.
    void *scene_relative_project = rt_asset_resolver_resolve(rt_const_cstr("scenes/level.json"),
                                                             project,
                                                             rt_const_cstr("assets"),
                                                             rt_const_cstr("local.png"));
    assert(rt_map_get_bool(scene_relative_project, rt_const_cstr("found")) == 1);
    assert(get_str(scene_relative_project, "source") == "scene");

    rt_string manifest_text = rt_const_cstr("project Demo\n"
                                            "lang zia\n"
                                            "entry src/main.zia\n"
                                            "sources src\n"
                                            "exclude build\n"
                                            "asset-root assets\n"
                                            "default-scene scenes/level.json\n"
                                            "[run.play]\n"
                                            "entry src/main.zia\n"
                                            "args --dev, --scene=one\n");
    void *manifest = rt_project_manifest_parse_text(manifest_text);
    assert(rt_map_get_bool(manifest, rt_const_cstr("valid")) == 1);
    assert(get_str(manifest, "name") == "Demo");
    assert(get_str(manifest, "entry") == "src/main.zia");
    assert(rt_seq_len(rt_map_get(manifest, rt_const_cstr("runConfigs"))) == 1);

    // VDOC-194: directives inside an UNKNOWN section must not leak into the
    // top level. The top-level `entry` stays as set before the unknown section,
    // the manifest is marked invalid, and no stray run/build config is created.
    rt_string hijack_text = rt_const_cstr("project Guarded\n"
                                          "entry real.zia\n"
                                          "[unknown]\n"
                                          "entry hijack.zia\n"
                                          "sources evil\n");
    void *hijacked = rt_project_manifest_parse_text(hijack_text);
    assert(get_str(hijacked, "entry") == "real.zia");
    assert(rt_map_get_bool(hijacked, rt_const_cstr("valid")) == 0);
    assert(rt_seq_len(rt_map_get(hijacked, rt_const_cstr("runConfigs"))) == 0);
    // The unknown section itself plus each of its two body directives are
    // diagnosed (3 diagnostics), and none of `evil` reached the top-level
    // sourceGlobs.
    assert(rt_seq_len(rt_map_get(hijacked, rt_const_cstr("diagnostics"))) == 3);

    rt_string_unref(scene);
    rt_string_unref(project);
    fs::remove_all(root);
}

static void test_workspace_watcher_batch() {
    fs::path root = temp_root();
    rt_string root_s = s(root.string());
    void *watcher = rt_watcher_new(root_s);
    rt_watcher_start(watcher);

    void *empty = rt_workspace_watcher_poll_batch(watcher, 4);
    assert(rt_seq_len(empty) == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_file(root / "created.zia", "module Created;\n");

    void *batch = nullptr;
    for (int attempt = 0; attempt < 30; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        batch = rt_workspace_watcher_poll_batch(watcher, 4);
        if (rt_seq_len(batch) > 0)
            break;
    }
    assert(batch != nullptr);
    assert(rt_seq_len(batch) > 0);
    void *event = rt_seq_get(batch, 0);
    assert(get_str(event, "typeName") != "none");
    assert(!get_str(event, "path").empty());
    assert(get_str(event, "oldPath").empty());
    assert(get_str(event, "newPath").empty());
    assert(rt_map_get_int(event, rt_const_cstr("overflowCount")) == 0);

#if RT_PLATFORM_LINUX || RT_PLATFORM_WINDOWS
    while (rt_seq_len(rt_workspace_watcher_poll_batch(watcher, 64)) > 0) {
    }
    fs::path old_path = root / "created.zia";
    fs::path new_path = root / "renamed.zia";
    fs::rename(old_path, new_path);
    bool saw_complete_rename = false;
    for (int attempt = 0; attempt < 30 && !saw_complete_rename; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        void *rename_batch = rt_workspace_watcher_poll_batch(watcher, 16);
        for (int64_t i = 0; i < rt_seq_len(rename_batch); i++) {
            void *rename_event = rt_seq_get(rename_batch, i);
            if (get_str(rename_event, "typeName") != "renamed")
                continue;
            saw_complete_rename =
                get_str(rename_event, "path") == new_path.string() &&
                get_str(rename_event, "oldPath") == old_path.string() &&
                get_str(rename_event, "newPath") == new_path.string() &&
                rt_map_get_bool(rename_event, rt_const_cstr("requiresRescan")) == 0;
        }
    }
    assert(saw_complete_rename);
#endif

    rt_watcher_stop(watcher);
    rt_string_unref(root_s);
    fs::remove_all(root);
}

static void add_edit(void *seq,
                     const fs::path &file,
                     int64_t sl,
                     int64_t sc,
                     int64_t el,
                     int64_t ec,
                     const std::string &text,
                     int64_t expected_size = -1) {
    void *edit = rt_map_new();
    rt_string path_s = s(file.string());
    rt_string text_s = s(text);
    rt_map_set_str(edit, rt_const_cstr("file"), path_s);
    rt_map_set_int(edit, rt_const_cstr("startLine"), sl);
    rt_map_set_int(edit, rt_const_cstr("startColumn"), sc);
    rt_map_set_int(edit, rt_const_cstr("endLine"), el);
    rt_map_set_int(edit, rt_const_cstr("endColumn"), ec);
    rt_map_set_str(edit, rt_const_cstr("newText"), text_s);
    if (expected_size >= 0)
        rt_map_set_int(edit, rt_const_cstr("expectedSize"), expected_size);
    rt_seq_push(seq, edit);
    rt_string_unref(path_s);
    rt_string_unref(text_s);
}

static void add_workspace_root(void *seq, const fs::path &root) {
    rt_string root_s = s(root.string());
    rt_seq_push(seq, root_s);
    rt_string_unref(root_s);
}

/// @brief Append one complete-file replacement for prepared-save coverage.
static void add_whole_file_edit(void *seq,
                                const fs::path &file,
                                const std::string &text,
                                int64_t expected_size,
                                int64_t max_bytes) {
    void *edit = rt_map_new();
    rt_string path_s = s(file.string());
    rt_string text_s = s(text);
    rt_map_set_str(edit, rt_const_cstr("file"), path_s);
    rt_map_set_bool(edit, rt_const_cstr("wholeFile"), 1);
    rt_map_set_str(edit, rt_const_cstr("newText"), text_s);
    rt_map_set_int(edit, rt_const_cstr("expectedSize"), expected_size);
    rt_map_set_int(edit, rt_const_cstr("maxBytes"), max_bytes);
    rt_seq_push(seq, edit);
    rt_string_unref(path_s);
    rt_string_unref(text_s);
}

static void test_workspace_edits() {
    fs::path root = temp_root();
    fs::path a = root / "a.zia";
    fs::path b = root / "b.zia";
    write_file(a, "one\ntwo\n");
    write_file(b, "alpha\nbeta\n");

#if !RT_PLATFORM_WINDOWS
    bool permission_fixture = false;
    const fs::perms expected_permissions =
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    std::error_code permission_ec;
    fs::permissions(a, expected_permissions, fs::perm_options::replace, permission_ec);
    permission_fixture = !permission_ec;
    struct stat ownership_before{};
    const bool ownership_fixture = stat(a.c_str(), &ownership_before) == 0;
#if RT_PLATFORM_MACOS
    constexpr uint32_t kUserNoDumpFlag = 0x00000001u;
    bool file_flags_fixture = chflags(a.c_str(), ownership_before.st_flags | kUserNoDumpFlag) == 0;
    struct stat flags_before{};
    file_flags_fixture = file_flags_fixture && stat(a.c_str(), &flags_before) == 0;
#elif RT_PLATFORM_LINUX
    bool file_flags_fixture = false;
    int flags_fd = open(a.c_str(), O_RDONLY);
    int flags_before = 0;
    if (flags_fd >= 0 && ioctl(flags_fd, FS_IOC_GETFLAGS, &flags_before) == 0) {
        int desired_flags = flags_before | FS_NODUMP_FL;
        file_flags_fixture = ioctl(flags_fd, FS_IOC_SETFLAGS, &desired_flags) == 0;
        if (file_flags_fixture)
            flags_before = desired_flags;
    }
    if (flags_fd >= 0)
        close(flags_fd);
#else
    const bool file_flags_fixture = false;
#endif
#else
    const std::wstring alternate_stream = a.wstring() + L":zanna-workspace-edit";
    HANDLE stream_handle = CreateFileW(alternate_stream.c_str(),
                                       GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    const char stream_payload[] = "alternate-metadata";
    bool alternate_stream_fixture = false;
    if (stream_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        alternate_stream_fixture = WriteFile(stream_handle,
                                             stream_payload,
                                             static_cast<DWORD>(sizeof(stream_payload)),
                                             &written,
                                             nullptr) != 0 &&
                                   written == sizeof(stream_payload);
        CloseHandle(stream_handle);
    }
    DWORD original_attributes = GetFileAttributesW(a.wstring().c_str());
    bool attribute_fixture =
        original_attributes != INVALID_FILE_ATTRIBUTES &&
        SetFileAttributesW(a.wstring().c_str(), original_attributes | FILE_ATTRIBUTE_HIDDEN) != 0;
#endif

#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
    const char metadata_value[] = "zanna-workspace-edit";
    bool xattr_fixture = false;
#if RT_PLATFORM_LINUX
    const char *metadata_name = "user.zanna.workspace-edit";
    xattr_fixture =
        setxattr(a.c_str(), metadata_name, metadata_value, sizeof(metadata_value), 0) == 0;
#elif RT_PLATFORM_MACOS
    const char *metadata_name = "com.zanna.workspace-edit";
    xattr_fixture =
        setxattr(
            a.c_str(), metadata_name, metadata_value, sizeof(metadata_value), 0, XATTR_NOFOLLOW) ==
        0;
#endif
#endif

    void *edits = rt_seq_new_owned();
    add_edit(edits, a, 2, 1, 2, 4, "TWO");
    add_edit(edits, b, 1, 1, 1, 6, "ALPHA");

    void *valid = rt_workspace_edit_validate(edits);
    assert(rt_map_get_bool(valid, rt_const_cstr("success")) == 1);
    void *applied = rt_workspace_edit_apply(edits);
    assert(rt_map_get_bool(applied, rt_const_cstr("success")) == 1);
    assert(read_file(a) == "one\nTWO\n");
    assert(read_file(b) == "ALPHA\nbeta\n");
#if !RT_PLATFORM_WINDOWS
    if (permission_fixture) {
        std::error_code status_ec;
        const fs::perms actual_permissions = fs::status(a, status_ec).permissions();
        assert(!status_ec);
        assert((actual_permissions & fs::perms::mask) == expected_permissions);
    }
    if (ownership_fixture) {
        struct stat ownership_after{};
        assert(stat(a.c_str(), &ownership_after) == 0);
        assert(ownership_after.st_uid == ownership_before.st_uid);
        assert(ownership_after.st_gid == ownership_before.st_gid);
    }
#if RT_PLATFORM_MACOS
    if (file_flags_fixture) {
        struct stat flags_after{};
        assert(stat(a.c_str(), &flags_after) == 0);
        assert(flags_after.st_flags == flags_before.st_flags);
    }
#elif RT_PLATFORM_LINUX
    if (file_flags_fixture) {
        int flags_after_fd = open(a.c_str(), O_RDONLY);
        int flags_after = 0;
        assert(flags_after_fd >= 0);
        assert(ioctl(flags_after_fd, FS_IOC_GETFLAGS, &flags_after) == 0);
        close(flags_after_fd);
        assert((flags_after & FS_FL_USER_MODIFIABLE) == (flags_before & FS_FL_USER_MODIFIABLE));
    }
#endif
#else
    if (attribute_fixture) {
        DWORD actual_attributes = GetFileAttributesW(a.wstring().c_str());
        assert(actual_attributes != INVALID_FILE_ATTRIBUTES);
        assert((actual_attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    }
    if (alternate_stream_fixture) {
        HANDLE actual_stream = CreateFileW(alternate_stream.c_str(),
                                           GENERIC_READ,
                                           FILE_SHARE_READ,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
        assert(actual_stream != INVALID_HANDLE_VALUE);
        char actual_payload[sizeof(stream_payload)] = {};
        DWORD actual_read = 0;
        assert(ReadFile(actual_stream,
                        actual_payload,
                        static_cast<DWORD>(sizeof(actual_payload)),
                        &actual_read,
                        nullptr) != 0);
        CloseHandle(actual_stream);
        assert(actual_read == sizeof(stream_payload));
        assert(std::string(actual_payload, sizeof(actual_payload)) ==
               std::string(stream_payload, sizeof(stream_payload)));
    }
    // Preservation has been verified; drop the hidden bit so the rest of the case
    // can rewrite `a` through the standard streams, which cannot truncate a
    // hidden file on Windows.
    if (attribute_fixture)
        SetFileAttributesW(a.wstring().c_str(), original_attributes);
#endif
#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
    if (xattr_fixture) {
        char metadata_buffer[64] = {};
#if RT_PLATFORM_LINUX
        const ssize_t metadata_size =
            getxattr(a.c_str(), metadata_name, metadata_buffer, sizeof(metadata_buffer));
#else
        const ssize_t metadata_size = getxattr(
            a.c_str(), metadata_name, metadata_buffer, sizeof(metadata_buffer), 0, XATTR_NOFOLLOW);
#endif
        assert(metadata_size == static_cast<ssize_t>(sizeof(metadata_value)));
        assert(std::string(metadata_buffer, sizeof(metadata_value)) ==
               std::string(metadata_value, sizeof(metadata_value)));
    }
#endif

    // VDOC-196: a successful apply leaves no `.zanna-edit-*` temp/backup sidecars
    // behind (backups are reserved with unpredictable names and cleaned up).
    for (const auto &entry : fs::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        assert(name.find(".zanna-edit-") == std::string::npos);
    }

    // The opaque ABI must reject maps and other object kinds instead of
    // interpreting rt_seq_len == -1 as an empty, successful edit batch.
    void *not_edits = rt_map_new();
    void *wrong_type = rt_workspace_edit_validate(not_edits);
    assert(rt_map_get_bool(wrong_type, rt_const_cstr("success")) == 0);
    assert(rt_map_get_int(wrong_type, rt_const_cstr("editCount")) == 0);
    assert(rt_seq_len(rt_map_get(wrong_type, rt_const_cstr("diagnostics"))) == 1);
    void *wrong_apply = rt_workspace_edit_apply(not_edits);
    assert(rt_map_get_bool(wrong_apply, rt_const_cstr("success")) == 0);
    assert(rt_map_get_int(wrong_apply, rt_const_cstr("appliedFiles")) == 0);

    // Workspace coordinates are one-based UTF-8 byte columns. Replacing text
    // after a multi-byte codepoint must neither split nor shift the edit range.
    write_file(a, "prefix caf\xC3\xA9 suffix\n");
    void *unicode = rt_seq_new_owned();
    add_edit(unicode, a, 1, 8, 1, 13, "tea");
    void *unicode_applied = rt_workspace_edit_apply(unicode);
    assert(rt_map_get_bool(unicode_applied, rt_const_cstr("success")) == 1);
    assert(read_file(a) == "prefix tea suffix\n");

    // A prepared whole-file save owns one immutable validation snapshot. A
    // same-size external rewrite after preparation must abort at commit, while
    // a fresh token applies exactly once and remains explicitly destroyable.
    fs::path prepared_file = root / "prepared.zia";
    write_file(prepared_file, "old-a\n");
    void *stale_edits = rt_seq_new_owned();
    add_whole_file_edit(stale_edits, prepared_file, "saved\n", 6, 1024);
    void *stale_prepared = rt_workspace_edit_prepare(stale_edits);
    assert(rt_workspace_edit_prepared_is_valid(stale_prepared) == 1);
    void *stale_validation = rt_workspace_edit_prepared_result(stale_prepared);
    assert(rt_map_get_bool(stale_validation, rt_const_cstr("success")) == 1);
    write_file(prepared_file, "old-b\n");
    void *stale_apply = rt_workspace_edit_prepared_apply(stale_prepared);
    assert(rt_map_get_bool(stale_apply, rt_const_cstr("success")) == 0);
    assert(read_file(prepared_file) == "old-b\n");
    assert(rt_workspace_edit_prepared_is_valid(stale_prepared) == 0);
    rt_workspace_edit_prepared_destroy(stale_prepared);

    void *fresh_edits = rt_seq_new_owned();
    add_whole_file_edit(fresh_edits, prepared_file, "saved\n", 6, 1024);
    void *fresh_prepared = rt_workspace_edit_prepare(fresh_edits);
    assert(rt_workspace_edit_prepared_is_valid(fresh_prepared) == 1);
    void *fresh_apply = rt_workspace_edit_prepared_apply(fresh_prepared);
    assert(rt_map_get_bool(fresh_apply, rt_const_cstr("success")) == 1);
    assert(rt_map_get_int(fresh_apply, rt_const_cstr("appliedFiles")) == 1);
    assert(read_file(prepared_file) == "saved\n");
    void *second_apply = rt_workspace_edit_prepared_apply(fresh_prepared);
    assert(rt_map_get_bool(second_apply, rt_const_cstr("success")) == 0);
    rt_workspace_edit_prepared_destroy(fresh_prepared);

    void *limited_edits = rt_seq_new_owned();
    add_whole_file_edit(limited_edits, prepared_file, "too large", 6, 4);
    void *limited_prepared = rt_workspace_edit_prepare(limited_edits);
    assert(rt_workspace_edit_prepared_is_valid(limited_prepared) == 0);
    rt_workspace_edit_prepared_destroy(limited_prepared);

    // A sparse target beyond the transaction's per-file budget must be
    // rejected from metadata alone, before the runtime allocates or reads it.
    fs::path oversized = root / "oversized.zia";
    write_file(oversized, "x");
    std::error_code resize_ec;
    fs::resize_file(oversized, 64u * 1024u * 1024u + 1u, resize_ec);
    if (!resize_ec) {
        void *oversized_edits = rt_seq_new_owned();
        add_edit(oversized_edits, oversized, 1, 1, 1, 2, "y");
        void *oversized_result = rt_workspace_edit_validate(oversized_edits);
        assert(rt_map_get_bool(oversized_result, rt_const_cstr("success")) == 0);
    }
    fs::remove(oversized);

    // Two directory entries for one inode/file ID are ambiguous under atomic
    // replacement (which would break the hard-link relationship). Reject the
    // complete batch instead of silently editing only one alias.
    fs::path hardlink_source = root / "hardlink-source.zia";
    fs::path hardlink_alias = root / "hardlink-alias.zia";
    write_file(hardlink_source, "left right\n");
    std::error_code hardlink_ec;
    fs::create_hard_link(hardlink_source, hardlink_alias, hardlink_ec);
    if (!hardlink_ec) {
        void *hardlink_edits = rt_seq_new_owned();
        add_edit(hardlink_edits, hardlink_source, 1, 1, 1, 5, "LEFT");
        add_edit(hardlink_edits, hardlink_alias, 1, 6, 1, 11, "RIGHT");
        void *hardlink_result = rt_workspace_edit_apply(hardlink_edits);
        assert(rt_map_get_bool(hardlink_result, rt_const_cstr("success")) == 0);
        assert(read_file(hardlink_source) == "left right\n");
        assert(read_file(hardlink_alias) == "left right\n");
    }

    write_file(a, "abcdef\n");
    void *overlap = rt_seq_new_owned();
    add_edit(overlap, a, 1, 1, 1, 4, "x");
    add_edit(overlap, a, 1, 3, 1, 6, "y");
    void *rejected = rt_workspace_edit_validate(overlap);
    assert(rt_map_get_bool(rejected, rt_const_cstr("success")) == 0);

    rt_string root_s = s(root.string());
    write_file(a, "first\nsecond\n");
    void *rooted = rt_seq_new_owned();
    add_edit(rooted, fs::path("a.zia"), 1, 1, 1, 6, "FIRST", 13);
    void *rooted_valid = rt_workspace_edit_validate_in_root(rooted, root_s);
    assert(rt_map_get_bool(rooted_valid, rt_const_cstr("success")) == 1);
    void *rooted_applied = rt_workspace_edit_apply_in_root(rooted, root_s);
    assert(rt_map_get_bool(rooted_applied, rt_const_cstr("success")) == 1);
    assert(read_file(a) == "FIRST\nsecond\n");

    // A prepared rooted transaction must not follow a directory component
    // swapped to a symlink between validation and commit. The retained-root,
    // no-follow reopen fails closed and neither the moved original nor the
    // outside lookalike is changed.
    fs::path secure_dir = root / "secure-dir";
    fs::path held_dir = root / "secure-dir-held";
    fs::path secure_file = secure_dir / "guarded.zia";
    fs::path outside_dir = root.parent_path() / (root.filename().string() + "_symlink_target");
    fs::path outside_guarded = outside_dir / "guarded.zia";
    write_file(secure_file, "inside\n");
    write_file(outside_guarded, "outside\n");
    void *guarded_edits = rt_seq_new_owned();
    add_edit(guarded_edits, fs::path("secure-dir/guarded.zia"), 1, 1, 1, 7, "MUTATE");
    void *guarded_prepared = rt_workspace_edit_prepare_in_root(guarded_edits, root_s);
    assert(rt_workspace_edit_prepared_is_valid(guarded_prepared) == 1);
    std::error_code swap_ec;
    fs::rename(secure_dir, held_dir, swap_ec);
    assert(!swap_ec);
    fs::create_directory_symlink(outside_dir, secure_dir, swap_ec);
    if (!swap_ec) {
        void *guarded_result = rt_workspace_edit_prepared_apply(guarded_prepared);
        assert(rt_map_get_bool(guarded_result, rt_const_cstr("success")) == 0);
        assert(read_file(held_dir / "guarded.zia") == "inside\n");
        assert(read_file(outside_guarded) == "outside\n");
        fs::remove(secure_dir, swap_ec);
    }
    rt_workspace_edit_prepared_destroy(guarded_prepared);
    fs::rename(held_dir, secure_dir, swap_ec);
    fs::remove_all(outside_dir);

    void *size_mismatch = rt_seq_new_owned();
    add_edit(size_mismatch, fs::path("a.zia"), 1, 1, 1, 6, "first", 999);
    void *size_rejected = rt_workspace_edit_validate_in_root(size_mismatch, root_s);
    assert(rt_map_get_bool(size_rejected, rt_const_cstr("success")) == 0);

    fs::path outside = root.parent_path() / (root.filename().string() + "_outside.zia");
    write_file(outside, "outside\n");
    void *escaping = rt_seq_new_owned();
    add_edit(escaping, outside, 1, 1, 1, 8, "ESCAPE");
    void *escaping_rejected = rt_workspace_edit_validate_in_root(escaping, root_s);
    assert(rt_map_get_bool(escaping_rejected, rt_const_cstr("success")) == 0);
    assert(read_file(outside) == "outside\n");

    // Multi-root validation keeps one transactional batch even when workspace
    // folders are unrelated. Every target must still belong to an explicit
    // root, and one escaping target prevents all allowed writes as well.
    fs::path second_root = temp_root();
    fs::path second_file = second_root / "second.zia";
    write_file(a, "left\n");
    write_file(second_file, "right\n");
    void *roots = rt_seq_new_owned();
    add_workspace_root(roots, root);
    add_workspace_root(roots, second_root);
    void *multi = rt_seq_new_owned();
    add_edit(multi, a, 1, 1, 1, 5, "LEFT");
    add_edit(multi, second_file, 1, 1, 1, 6, "RIGHT");
    void *multi_valid = rt_workspace_edit_validate_in_roots(multi, roots);
    assert(rt_map_get_bool(multi_valid, rt_const_cstr("success")) == 1);
    void *multi_applied = rt_workspace_edit_apply_in_roots(multi, roots);
    assert(rt_map_get_bool(multi_applied, rt_const_cstr("success")) == 1);
    assert(rt_map_get_int(multi_applied, rt_const_cstr("appliedFiles")) == 2);
    assert(read_file(a) == "LEFT\n");
    assert(read_file(second_file) == "RIGHT\n");

    write_file(a, "allowed\n");
    void *multi_escape = rt_seq_new_owned();
    add_edit(multi_escape, a, 1, 1, 1, 8, "MUTATED");
    add_edit(multi_escape, outside, 1, 1, 1, 8, "ESCAPE");
    void *multi_escape_rejected = rt_workspace_edit_apply_in_roots(multi_escape, roots);
    assert(rt_map_get_bool(multi_escape_rejected, rt_const_cstr("success")) == 0);
    assert(rt_map_get_int(multi_escape_rejected, rt_const_cstr("appliedFiles")) == 0);
    assert(read_file(a) == "allowed\n");
    assert(read_file(outside) == "outside\n");

    rt_string_unref(root_s);
    fs::remove(outside);
    fs::remove_all(second_root);
    fs::remove_all(root);
}

/// @brief VDOC-193: a `.gitignore` rewrite must invalidate the pattern cache
///        even when the modification timestamp is unchanged (a same-second, or
///        coarse-clock, save). The cache identity is content-derived, so the new
///        patterns take effect immediately.
static void test_gitignore_same_second_rewrite() {
    fs::path root = temp_root();
    write_file(root / ".gitignore", "*.tmp\n");
    rt_string root_s = s(root.string());

    // Prime the cache: *.tmp ignored, *.log not.
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("a.tmp"), rt_const_cstr("")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("a.log"), rt_const_cstr("")) == 0);

    // Rewrite with different patterns, then force the ORIGINAL mtime back so the
    // whole-second timestamp is identical to the primed version.
    std::error_code ec;
    auto mtime = fs::last_write_time(root / ".gitignore", ec);
    write_file(root / ".gitignore", "*.log\n");
    fs::last_write_time(root / ".gitignore", mtime, ec);

    // Despite the unchanged mtime, the content-derived cache identity changed,
    // so the new patterns are honored: *.log now ignored, *.tmp no longer.
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("a.log"), rt_const_cstr("")) == 1);
    assert(rt_workspace_file_index_should_ignore(
               root_s, rt_const_cstr("a.tmp"), rt_const_cstr("")) == 0);
    rt_string_unref(root_s);
    fs::remove_all(root);
}

int main() {
    test_directory_page();
    test_file_index_and_ignore();
    test_file_index_cursor_work_budget();
    test_asset_resolver_and_manifest();
    test_workspace_watcher_batch();
    test_workspace_edits();
    test_gitignore_same_second_rewrite();
    return 0;
}
