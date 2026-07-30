//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSaveDataTests.cpp
// Purpose: Tests for SaveData key-value persistence system. Validates in-memory
//   operations (set/get/remove/clear/count), JSON save/load round-trip,
//   type safety (int vs string getters with defaults), and null safety.
// Key invariants:
//   - Keys are unique, last-write wins.
//   - GetInt on a string key returns the default (and vice versa).
//   - Save/Load are whole-file operations producing valid JSON.
//   - All functions are null-safe.
// Ownership/Lifetime:
//   - SaveData objects are GC-managed; tests rely on the runtime allocator.
// Links: rt_savedata.c, rt_savedata.h, docs/zannalib/game/persistence.md
//
//===----------------------------------------------------------------------===//

#include "common/PlatformCapabilities.hpp"
#include "rt.hpp"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_savedata.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if ZANNA_HOST_WINDOWS
#include <direct.h>
#include <process.h>
#define GETPID _getpid
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define GETPID getpid
#endif

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static bool g_trap_returns = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_returns)
        return;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            (expr);                                                                                \
            g_trap_expected = false;                                                               \
            assert(!"Expected trap did not fire");                                                 \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

/// @brief Helper: create rt_string from C literal.
static rt_string S(const char *s) {
    return rt_const_cstr(s);
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static bool string_bytes_equal(rt_string s, const char *bytes, size_t len) {
    return s && (size_t)rt_str_len(s) == len && memcmp(rt_string_cstr(s), bytes, len) == 0;
}

static void write_file_exact(const char *path, const char *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    assert(fp != nullptr);
    size_t written = fwrite(bytes, 1, len, fp);
    fclose(fp);
    assert(written == len);
}

static void configure_test_save_root() {
    char root[256];
#if ZANNA_HOST_WINDOWS
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = "C:\\Temp";
    snprintf(root, sizeof(root), "%s\\zanna_savedata_home_%d", tmp, (int)GETPID());
    _mkdir(root);
    _putenv_s("APPDATA", root);
    _putenv_s("USERPROFILE", root);
#else
    snprintf(root, sizeof(root), "/tmp/zanna_savedata_home_%d", (int)GETPID());
    mkdir(root, 0755);
    setenv("HOME", root, 1);
#endif
}

// ============================================================================
// Null Safety Tests
// ============================================================================

static void test_null_safety() {
    printf("--- Null Safety ---\n");

    EXPECT_TRAP(rt_savedata_set_int(nullptr, S("key"), 42));
    EXPECT_TRAP(rt_savedata_set_string(nullptr, S("key"), S("val")));
    assert(rt_savedata_get_int(nullptr, S("key"), -1) == -1);
    assert(rt_savedata_has_key(nullptr, S("key")) == 0);
    assert(rt_savedata_remove(nullptr, S("key")) == 0);
    assert(rt_savedata_count(nullptr) == 0);
    assert(rt_savedata_save(nullptr) == 0);
    assert(rt_savedata_load(nullptr) == 0);
    rt_savedata_clear(nullptr);

    printf("  test_null_safety: PASSED\n");
}

static void test_returning_trap_safety() {
    void *sd = rt_savedata_new(S("returning-traps"));
    void *fake = rt_obj_new_i64(RT_SAVEDATA_CLASS_ID, 1);
    rt_string key = rt_string_from_bytes("score", 5);
    rt_string text_key = rt_string_from_bytes("text", 4);
    rt_string old_value = rt_string_from_bytes("old", 3);
    rt_string replacement = rt_string_from_bytes("replacement", 11);
    rt_string new_key = rt_string_from_bytes("new-key", 7);
    auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
    assert(sd != nullptr);
    assert(fake != nullptr);
    assert(key != nullptr);
    assert(text_key != nullptr);
    assert(old_value != nullptr);
    assert(replacement != nullptr);
    assert(new_key != nullptr);

    rt_savedata_set_int(sd, key, 7);
    rt_savedata_set_string(sd, text_key, old_value);
    assert(rt_savedata_count(sd) == 2);

    g_trap_returns = true;
    g_last_trap = nullptr;
    rt_savedata_set_int(fake, key, 99);
    assert(g_last_trap != nullptr);
    assert(rt_savedata_get_int(fake, key, -1) == -1);
    assert(rt_savedata_has_key(fake, key) == 0);
    assert(rt_savedata_remove(fake, key) == 0);
    assert(rt_savedata_count(fake) == 0);
    assert(rt_savedata_save(fake) == 0);
    assert(rt_savedata_load(fake) == 0);
    rt_savedata_clear(fake);
    rt_string fake_path = rt_savedata_get_path(fake);
    assert(fake_path != nullptr);
    rt_string_unref(fake_path);

    g_last_trap = nullptr;
    rt_savedata_set_int(sd, forged, 99);
    assert(g_last_trap != nullptr);
    assert(rt_savedata_count(sd) == 2);
    assert(rt_savedata_get_int(sd, key, -1) == 7);

    g_last_trap = nullptr;
    rt_savedata_set_string(sd, key, forged);
    assert(g_last_trap != nullptr);
    assert(rt_savedata_get_int(sd, key, -1) == 7);

    assert(replacement->heap == RT_SSO_SENTINEL);
    replacement->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
    g_last_trap = nullptr;
    rt_savedata_set_string(sd, text_key, replacement);
    assert(g_last_trap != nullptr);
    replacement->literal_refs = 1;
    rt_string retained_old = rt_savedata_get_string(sd, text_key, nullptr);
    assert(string_bytes_equal(retained_old, "old", 3));
    rt_string_unref(retained_old);

    assert(new_key->heap == RT_SSO_SENTINEL);
    new_key->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
    g_last_trap = nullptr;
    rt_savedata_set_int(sd, new_key, 1);
    assert(g_last_trap != nullptr);
    new_key->literal_refs = 1;
    assert(rt_savedata_count(sd) == 2);
    g_trap_returns = false;

    rt_string_unref(key);
    rt_string_unref(text_key);
    rt_string_unref(old_value);
    rt_string_unref(replacement);
    rt_string_unref(new_key);
    release_obj(fake);
    release_obj(sd);
    printf("  test_returning_trap_safety: PASSED\n");
}

static void test_empty_name_traps() {
    printf("--- Constructor Validation ---\n");

    EXPECT_TRAP(rt_savedata_new(S("")));
    printf("  test_empty_name_traps: PASSED\n");
}

static void test_embedded_nul_name_traps() {
    char name_bytes[] = {'g', 'a', 'm', 'e', '\0', 'x'};
    rt_string name = rt_string_from_bytes(name_bytes, sizeof(name_bytes));
    EXPECT_TRAP(rt_savedata_new(name));
    rt_string_unref(name);
    printf("  test_embedded_nul_name_traps: PASSED\n");
}

/// @brief VDOC-199: Path.DataDir validates the runtime String's full byte
///        length, so an app name with an embedded NUL (and bytes after it) is
///        rejected rather than silently truncated at the NUL.
static void test_data_dir_embedded_nul_name_traps() {
    char name_bytes[] = {'a', 'p', 'p', '\0', 'x'};
    rt_string name = rt_string_from_bytes(name_bytes, sizeof(name_bytes));
    EXPECT_TRAP(rt_path_data_dir(name));
    rt_string_unref(name);
    printf("  test_data_dir_embedded_nul_name_traps: PASSED\n");
}

// ============================================================================
// In-Memory Key-Value Operations
// ============================================================================

static void test_set_get_int() {
    void *sd = rt_savedata_new(S("test-int"));
    assert(sd != nullptr);

    rt_savedata_set_int(sd, S("score"), 42000);
    assert(rt_savedata_get_int(sd, S("score"), 0) == 42000);

    // Default for missing key
    assert(rt_savedata_get_int(sd, S("missing"), -1) == -1);

    // Overwrite
    rt_savedata_set_int(sd, S("score"), 99999);
    assert(rt_savedata_get_int(sd, S("score"), 0) == 99999);

    // Negative value
    rt_savedata_set_int(sd, S("negative"), -500);
    assert(rt_savedata_get_int(sd, S("negative"), 0) == -500);

    // Zero
    rt_savedata_set_int(sd, S("zero"), 0);
    assert(rt_savedata_get_int(sd, S("zero"), -1) == 0);

    printf("  test_set_get_int: PASSED\n");
}

static void test_set_get_string() {
    void *sd = rt_savedata_new(S("test-str"));
    assert(sd != nullptr);

    rt_savedata_set_string(sd, S("name"), S("ACE"));
    rt_string val = rt_savedata_get_string(sd, S("name"), S("default"));
    assert(strcmp(rt_string_cstr(val), "ACE") == 0);

    // Default for missing key
    rt_string missing = rt_savedata_get_string(sd, S("missing"), S("fallback"));
    assert(strcmp(rt_string_cstr(missing), "fallback") == 0);

    // Overwrite
    rt_savedata_set_string(sd, S("name"), S("PLAYER1"));
    rt_string updated = rt_savedata_get_string(sd, S("name"), S(""));
    assert(strcmp(rt_string_cstr(updated), "PLAYER1") == 0);

    // Empty string value
    rt_savedata_set_string(sd, S("empty_val"), S(""));
    rt_string empty = rt_savedata_get_string(sd, S("empty_val"), S("notfound"));
    assert(strcmp(rt_string_cstr(empty), "") == 0);

    printf("  test_set_get_string: PASSED\n");
}

static void test_type_mismatch_defaults() {
    void *sd = rt_savedata_new(S("test-types"));
    assert(sd != nullptr);

    // Set as int, get as string → returns default
    rt_savedata_set_int(sd, S("score"), 42000);
    rt_string s = rt_savedata_get_string(sd, S("score"), S("nope"));
    assert(strcmp(rt_string_cstr(s), "nope") == 0);

    // Set as string, get as int → returns default
    rt_savedata_set_string(sd, S("name"), S("ACE"));
    assert(rt_savedata_get_int(sd, S("name"), -1) == -1);

    printf("  test_type_mismatch_defaults: PASSED\n");
}

static void test_type_overwrite() {
    void *sd = rt_savedata_new(S("test-overwrite"));
    assert(sd != nullptr);

    // Set as int, then overwrite with string
    rt_savedata_set_int(sd, S("val"), 100);
    assert(rt_savedata_get_int(sd, S("val"), 0) == 100);

    rt_savedata_set_string(sd, S("val"), S("hello"));
    assert(rt_savedata_get_int(sd, S("val"), -1) == -1); // Now a string
    rt_string sv = rt_savedata_get_string(sd, S("val"), S(""));
    assert(strcmp(rt_string_cstr(sv), "hello") == 0);

    printf("  test_type_overwrite: PASSED\n");
}

// ============================================================================
// HasKey, Remove, Clear, Count
// ============================================================================

static void test_has_key() {
    void *sd = rt_savedata_new(S("test-haskey"));
    assert(sd != nullptr);

    assert(rt_savedata_has_key(sd, S("x")) == 0);

    rt_savedata_set_int(sd, S("x"), 10);
    assert(rt_savedata_has_key(sd, S("x")) == 1);
    assert(rt_savedata_has_key(sd, S("y")) == 0);

    printf("  test_has_key: PASSED\n");
}

static void test_remove() {
    void *sd = rt_savedata_new(S("test-remove"));
    assert(sd != nullptr);

    rt_savedata_set_int(sd, S("a"), 1);
    rt_savedata_set_int(sd, S("b"), 2);
    assert(rt_savedata_count(sd) == 2);

    // Remove existing
    assert(rt_savedata_remove(sd, S("a")) == 1);
    assert(rt_savedata_count(sd) == 1);
    assert(rt_savedata_has_key(sd, S("a")) == 0);
    assert(rt_savedata_has_key(sd, S("b")) == 1);

    // Remove non-existent
    assert(rt_savedata_remove(sd, S("a")) == 0);

    printf("  test_remove: PASSED\n");
}

static void test_clear() {
    void *sd = rt_savedata_new(S("test-clear"));
    assert(sd != nullptr);

    rt_savedata_set_int(sd, S("a"), 1);
    rt_savedata_set_string(sd, S("b"), S("two"));
    rt_savedata_set_int(sd, S("c"), 3);
    assert(rt_savedata_count(sd) == 3);

    rt_savedata_clear(sd);
    assert(rt_savedata_count(sd) == 0);
    assert(rt_savedata_has_key(sd, S("a")) == 0);

    printf("  test_clear: PASSED\n");
}

static void test_count() {
    void *sd = rt_savedata_new(S("test-count"));
    assert(sd != nullptr);

    assert(rt_savedata_count(sd) == 0);

    rt_savedata_set_int(sd, S("a"), 1);
    assert(rt_savedata_count(sd) == 1);

    rt_savedata_set_string(sd, S("b"), S("two"));
    assert(rt_savedata_count(sd) == 2);

    // Overwrite doesn't increase count
    rt_savedata_set_int(sd, S("a"), 99);
    assert(rt_savedata_count(sd) == 2);

    printf("  test_count: PASSED\n");
}

/// @brief Exercise count invariants across a larger mixed mutation set.
/// @details Replacements must not change cardinality, successful removals must
///          decrement it exactly once, failed removals must leave it unchanged,
///          and Clear must reset it without depending on list traversal order.
static void test_count_tracks_bulk_mutations() {
    void *sd = rt_savedata_new(S("test-count-bulk"));
    assert(sd != nullptr);

    constexpr int kEntryCount = 2048;
    char key_bytes[32];
    for (int i = 0; i < kEntryCount; ++i) {
        int written = snprintf(key_bytes, sizeof(key_bytes), "key-%04d", i);
        assert(written > 0 && static_cast<size_t>(written) < sizeof(key_bytes));
        rt_string key = rt_string_from_bytes(key_bytes, static_cast<size_t>(written));
        assert(key != nullptr);
        rt_savedata_set_int(sd, key, i);
        rt_string_unref(key);
    }
    assert(rt_savedata_count(sd) == kEntryCount);

    rt_string replacement = S("replacement");
    assert(replacement != nullptr);
    for (int i = 0; i < kEntryCount; i += 2) {
        int written = snprintf(key_bytes, sizeof(key_bytes), "key-%04d", i);
        assert(written > 0 && static_cast<size_t>(written) < sizeof(key_bytes));
        rt_string key = rt_string_from_bytes(key_bytes, static_cast<size_t>(written));
        assert(key != nullptr);
        rt_savedata_set_string(sd, key, replacement);
        rt_string_unref(key);
    }
    rt_string_unref(replacement);
    assert(rt_savedata_count(sd) == kEntryCount);

    int64_t expected = kEntryCount;
    for (int i = 0; i < kEntryCount; i += 3) {
        int written = snprintf(key_bytes, sizeof(key_bytes), "key-%04d", i);
        assert(written > 0 && static_cast<size_t>(written) < sizeof(key_bytes));
        rt_string key = rt_string_from_bytes(key_bytes, static_cast<size_t>(written));
        assert(key != nullptr);
        assert(rt_savedata_remove(sd, key) == 1);
        assert(rt_savedata_remove(sd, key) == 0);
        rt_string_unref(key);
        --expected;
    }
    assert(rt_savedata_count(sd) == expected);

    rt_savedata_clear(sd);
    assert(rt_savedata_count(sd) == 0);
    release_obj(sd);

    printf("  test_count_tracks_bulk_mutations: PASSED\n");
}

// ============================================================================
// Path Computation
// ============================================================================

static void test_path_contains_game_name() {
    void *sd = rt_savedata_new(S("mygame"));
    assert(sd != nullptr);

    rt_string path = rt_savedata_get_path(sd);
    const char *pcstr = rt_string_cstr(path);
    assert(pcstr != nullptr);
    assert(strstr(pcstr, "mygame") != nullptr);
    assert(strstr(pcstr, "save.json") != nullptr);
#if ZANNA_HOST_WINDOWS
    assert((pcstr[0] == '\\' && pcstr[1] == '\\') ||
           ((pcstr[0] >= 'A' && pcstr[0] <= 'Z') || (pcstr[0] >= 'a' && pcstr[0] <= 'z')) &&
               pcstr[1] == ':' && (pcstr[2] == '\\' || pcstr[2] == '/'));
#else
    assert(pcstr[0] == '/');
#endif

    printf("  test_path_contains_game_name: PASSED\n");
}

// ============================================================================
// Save / Load Round-Trip
// ============================================================================

static void test_save_load_round_trip() {
    /* Use a PID-unique game name to avoid collisions in parallel test runs */
    char game[64];
    snprintf(game, sizeof(game), "zanna-test-%d", (int)GETPID());

    void *sd1 = rt_savedata_new(S(game));
    assert(sd1 != nullptr);

    // Populate
    rt_savedata_set_int(sd1, S("high_score"), 42000);
    rt_savedata_set_int(sd1, S("level"), 5);
    rt_savedata_set_string(sd1, S("player"), S("ACE"));
    rt_savedata_set_string(sd1, S("guild"), S("Zanna Knights"));
    rt_savedata_set_int(sd1, S("negative"), -100);

    // Save
    int8_t save_ok = rt_savedata_save(sd1);
    assert(save_ok == 1);

    // Load into a fresh instance
    void *sd2 = rt_savedata_new(S(game));
    assert(sd2 != nullptr);
    assert(rt_savedata_count(sd2) == 0); // Empty before load

    int8_t load_ok = rt_savedata_load(sd2);
    assert(load_ok == 1);

    // Verify all values survived the round-trip
    assert(rt_savedata_get_int(sd2, S("high_score"), 0) == 42000);
    assert(rt_savedata_get_int(sd2, S("level"), 0) == 5);
    assert(rt_savedata_get_int(sd2, S("negative"), 0) == -100);

    rt_string p = rt_savedata_get_string(sd2, S("player"), S(""));
    assert(strcmp(rt_string_cstr(p), "ACE") == 0);

    rt_string g = rt_savedata_get_string(sd2, S("guild"), S(""));
    assert(strcmp(rt_string_cstr(g), "Zanna Knights") == 0);

    assert(rt_savedata_count(sd2) == 5);

    // Cleanup: remove the save file
    rt_string path = rt_savedata_get_path(sd1);
    remove(rt_string_cstr(path));

    printf("  test_save_load_round_trip: PASSED\n");
}

static void test_load_nonexistent_returns_success_and_clears_entries() {
    void *sd = rt_savedata_new(S("no-such-game-ever-9999"));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("stale"), 123);
    assert(rt_savedata_count(sd) == 1);

    int8_t result = rt_savedata_load(sd);
    assert(result == 1); // Missing save file is an empty successful load.
    assert(rt_savedata_count(sd) == 0);

    printf("  test_load_nonexistent_returns_success_and_clears_entries: PASSED\n");
}

static void test_save_overwrite() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-overwrite-%d", (int)GETPID());

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);

    // First save
    rt_savedata_set_int(sd, S("score"), 100);
    assert(rt_savedata_save(sd) == 1);

    // Modify and save again
    rt_savedata_set_int(sd, S("score"), 200);
    rt_savedata_set_string(sd, S("name"), S("BOB"));
    assert(rt_savedata_save(sd) == 1);

    // Load into fresh instance — should see latest data
    void *sd2 = rt_savedata_new(S(game));
    assert(rt_savedata_load(sd2) == 1);
    assert(rt_savedata_get_int(sd2, S("score"), 0) == 200);

    rt_string n = rt_savedata_get_string(sd2, S("name"), S(""));
    assert(strcmp(rt_string_cstr(n), "BOB") == 0);

    // Cleanup
    rt_string path = rt_savedata_get_path(sd);
    remove(rt_string_cstr(path));

    printf("  test_save_overwrite: PASSED\n");
}

// ============================================================================
// Edge Cases
// ============================================================================

static void test_invalid_keys_trap() {
    void *sd = rt_savedata_new(S("test-invalidkey"));
    assert(sd != nullptr);

    EXPECT_TRAP(rt_savedata_set_int(sd, S(""), 42));
    EXPECT_TRAP(rt_savedata_set_string(sd, S(""), S("val")));

    char nul_key_bytes[] = {'b', 'a', 'd', '\0', 'k', 'e', 'y'};
    rt_string nul_key = rt_string_from_bytes(nul_key_bytes, sizeof(nul_key_bytes));
    EXPECT_TRAP(rt_savedata_set_int(sd, nul_key, 1));
    rt_string_unref(nul_key);

    unsigned char bad_utf8_bytes[] = {'b', 'a', 'd', 0xC3u, '('};
    rt_string bad_utf8_key = rt_string_from_bytes(reinterpret_cast<const char *>(bad_utf8_bytes),
                                                  sizeof(bad_utf8_bytes));
    EXPECT_TRAP(rt_savedata_set_string(sd, bad_utf8_key, S("val")));
    rt_string_unref(bad_utf8_key);

    assert(rt_savedata_count(sd) == 0);

    printf("  test_invalid_keys_trap: PASSED\n");
}

static void test_invalid_string_values_trap() {
    void *sd = rt_savedata_new(S("test-invalidvalue"));
    assert(sd != nullptr);

    unsigned char bad_value_bytes[] = {'b', 'a', 'd', 0xC3u, '('};
    rt_string bad_value = rt_string_from_bytes(reinterpret_cast<const char *>(bad_value_bytes),
                                               sizeof(bad_value_bytes));
    EXPECT_TRAP(rt_savedata_set_string(sd, S("bad"), bad_value));
    rt_string_unref(bad_value);

    assert(rt_savedata_count(sd) == 0);
    printf("  test_invalid_string_values_trap: PASSED\n");
}

static void test_special_chars_in_values() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-special-%d", (int)GETPID());

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);

    // Values with JSON-sensitive characters
    rt_savedata_set_string(sd, S("quote"), S("He said \"hello\""));
    rt_savedata_set_string(sd, S("backslash"), S("C:\\Users\\test"));
    rt_savedata_set_string(sd, S("newline"), S("line1\nline2"));

    assert(rt_savedata_save(sd) == 1);

    // Round-trip
    void *sd2 = rt_savedata_new(S(game));
    assert(rt_savedata_load(sd2) == 1);

    rt_string q = rt_savedata_get_string(sd2, S("quote"), S(""));
    assert(strcmp(rt_string_cstr(q), "He said \"hello\"") == 0);

    rt_string b = rt_savedata_get_string(sd2, S("backslash"), S(""));
    assert(strcmp(rt_string_cstr(b), "C:\\Users\\test") == 0);

    rt_string nl = rt_savedata_get_string(sd2, S("newline"), S(""));
    assert(strcmp(rt_string_cstr(nl), "line1\nline2") == 0);

    // Cleanup
    rt_string path = rt_savedata_get_path(sd);
    remove(rt_string_cstr(path));

    printf("  test_special_chars_in_values: PASSED\n");
}

static void test_large_int_values() {
    void *sd = rt_savedata_new(S("test-large"));
    assert(sd != nullptr);

    rt_savedata_set_int(sd, S("max"), INT64_MAX);
    rt_savedata_set_int(sd, S("min"), INT64_MIN);

    assert(rt_savedata_get_int(sd, S("max"), 0) == INT64_MAX);
    assert(rt_savedata_get_int(sd, S("min"), 0) == INT64_MIN);

    printf("  test_large_int_values: PASSED\n");
}

static void test_large_int_round_trip() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-int64-%d", (int)GETPID());

    const int64_t above_double_exact = INT64_C(9007199254740993);

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("above_double_exact"), above_double_exact);
    rt_savedata_set_int(sd, S("max"), INT64_MAX);
    rt_savedata_set_int(sd, S("min"), INT64_MIN);
    assert(rt_savedata_save(sd) == 1);

    void *loaded = rt_savedata_new(S(game));
    assert(loaded != nullptr);
    assert(rt_savedata_load(loaded) == 1);
    assert(rt_savedata_get_int(loaded, S("above_double_exact"), 0) == above_double_exact);
    assert(rt_savedata_get_int(loaded, S("max"), 0) == INT64_MAX);
    assert(rt_savedata_get_int(loaded, S("min"), 0) == INT64_MIN);

    rt_string path = rt_savedata_get_path(sd);
    remove(rt_string_cstr(path));

    printf("  test_large_int_round_trip: PASSED\n");
}

static void test_string_values_preserve_embedded_nul_round_trip() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-nul-value-%d", (int)GETPID());

    char value_bytes[] = {'A', '\0', 'B', 0x01, 'C'};
    rt_string value = rt_string_from_bytes(value_bytes, sizeof(value_bytes));

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);
    rt_savedata_set_string(sd, S("binary_value"), value);
    assert(rt_savedata_save(sd) == 1);

    void *sd2 = rt_savedata_new(S(game));
    assert(rt_savedata_load(sd2) == 1);

    rt_string loaded = rt_savedata_get_string(sd2, S("binary_value"), S(""));
    assert(string_bytes_equal(loaded, value_bytes, sizeof(value_bytes)));

    rt_string path = rt_savedata_get_path(sd);
    remove(rt_string_cstr(path));

    rt_string_unref(value);
    printf("  test_string_values_preserve_embedded_nul_round_trip: PASSED\n");
}

static void test_load_rejects_malformed_json() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-malformed-%d", (int)GETPID());

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("score"), 7);
    assert(rt_savedata_save(sd) == 1);

    rt_string path = rt_savedata_get_path(sd);
    write_file_exact(rt_string_cstr(path), "{\"score\": 1", strlen("{\"score\": 1"));
    assert(rt_savedata_load(sd) == 0);
    assert(rt_savedata_get_int(sd, S("score"), -1) == 7);

    write_file_exact(rt_string_cstr(path), "{} trailing", strlen("{} trailing"));
    assert(rt_savedata_load(sd) == 0);
    assert(rt_savedata_get_int(sd, S("score"), -1) == 7);

    remove(rt_string_cstr(path));
    printf("  test_load_rejects_malformed_json: PASSED\n");
}

static void test_load_rejects_non_int64_numbers() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-number-range-%d", (int)GETPID());

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("score"), 7);
    assert(rt_savedata_save(sd) == 1);

    rt_string path = rt_savedata_get_path(sd);
    write_file_exact(rt_string_cstr(path), "{\"score\": 1.5}", strlen("{\"score\": 1.5}"));
    assert(rt_savedata_load(sd) == 0);
    assert(rt_savedata_get_int(sd, S("score"), -1) == 7);

    write_file_exact(rt_string_cstr(path),
                     "{\"score\": 9223372036854775808}",
                     strlen("{\"score\": 9223372036854775808}"));
    assert(rt_savedata_load(sd) == 0);
    assert(rt_savedata_get_int(sd, S("score"), -1) == 7);

    remove(rt_string_cstr(path));
    printf("  test_load_rejects_non_int64_numbers: PASSED\n");
}

static void test_load_rejects_invalid_decoded_key() {
    char game[64];
    snprintf(game, sizeof(game), "zanna-invalid-load-key-%d", (int)GETPID());

    void *sd = rt_savedata_new(S(game));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("score"), 7);
    assert(rt_savedata_save(sd) == 1);

    rt_string path = rt_savedata_get_path(sd);
    const char bad_key_json[] = "{\"bad\\u0000key\": 1}";
    write_file_exact(rt_string_cstr(path), bad_key_json, strlen(bad_key_json));
    assert(rt_savedata_load(sd) == 0);
    assert(rt_savedata_get_int(sd, S("score"), -1) == 7);

    remove(rt_string_cstr(path));
    printf("  test_load_rejects_invalid_decoded_key: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

// VDOC-246: Save promises a Boolean failure result, so an un-creatable parent
// directory (here: the home root is a regular file, not a directory) must return
// false instead of trapping inside rt_dir_make_all. A trap would abort via the
// non-expecting vm_trap, so reaching the assertion at all proves the soft-fail.
static void test_save_soft_fails_when_parent_dir_uncreatable() {
    printf("--- Save soft-fails on un-creatable parent dir ---\n");

    // Point the per-user data root at a regular file so directory creation fails.
    char fileRoot[256];
    snprintf(fileRoot, sizeof(fileRoot), "/tmp/zanna_savedata_filehome_%d", (int)GETPID());
    std::remove(fileRoot);
    write_file_exact(fileRoot, "x", 1);
#if ZANNA_HOST_WINDOWS
    // Skip on Windows: the APPDATA/USERPROFILE layering differs; the POSIX path
    // exercises the shared trap-to-Boolean recovery.
    std::remove(fileRoot);
    (void)fileRoot;
    return;
#else
    setenv("HOME", fileRoot, 1);

    void *sd = rt_savedata_new(S("dir-fail-game"));
    assert(sd != nullptr);
    rt_savedata_set_int(sd, S("hp"), 100);
    int8_t ok = rt_savedata_save(sd);
    assert(ok == 0 && "Save must return false, not trap, when the parent dir cannot be created");

    // Restore an isolated, valid save root for any later tests.
    configure_test_save_root();
    std::remove(fileRoot);
#endif
}

int main() {
    printf("=== RTSaveDataTests ===\n\n");
    configure_test_save_root();

    test_null_safety();
    test_returning_trap_safety();
    test_empty_name_traps();
    test_embedded_nul_name_traps();
    test_data_dir_embedded_nul_name_traps();

    printf("\n--- Key-Value Operations ---\n");
    test_set_get_int();
    test_set_get_string();
    test_type_mismatch_defaults();
    test_type_overwrite();

    printf("\n--- HasKey / Remove / Clear / Count ---\n");
    test_has_key();
    test_remove();
    test_clear();
    test_count();
    test_count_tracks_bulk_mutations();

    printf("\n--- Path ---\n");
    test_path_contains_game_name();

    printf("\n--- Save / Load ---\n");
    test_save_load_round_trip();
    test_load_nonexistent_returns_success_and_clears_entries();
    test_save_overwrite();
    test_save_soft_fails_when_parent_dir_uncreatable();

    printf("\n--- Edge Cases ---\n");
    test_invalid_keys_trap();
    test_invalid_string_values_trap();
    test_special_chars_in_values();
    test_large_int_values();
    test_large_int_round_trip();
    test_string_values_preserve_embedded_nul_round_trip();
    test_load_rejects_malformed_json();
    test_load_rejects_non_int64_numbers();
    test_load_rejects_invalid_decoded_key();

    printf("\n=== All RTSaveDataTests passed! ===\n");
    return 0;
}
