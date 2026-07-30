//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTLocaleManagerTests.cpp
// Purpose: Validate Zanna.Localization.LocaleManager bootstrap, current/system
//          queries, registry surface, JSON loading, search-path resolution,
//          and unload/reset behavior.
// Key invariants:
//   - Loaded locale data remains alive while any Locale or formatter retains it.
//   - Refcount overflow and schema/path failures do not publish corrupt state.
// Ownership/Lifetime:
//   - Tests release runtime handles and strings they acquire.
//   - Temporary JSON files live only under the process-specific test directory.
// Links: src/runtime/localization/rt_locale_manager.c,
//        src/runtime/localization/rt_locale_manager.h
//
//===----------------------------------------------------------------------===//

#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_object.h"
#include "rt_string.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <setjmp.h>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_GETPID() getpid()
#endif

static jmp_buf g_trap_env;
static int g_expect_trap = 0;
static int g_return_trap = 0;
static int g_return_trap_count = 0;

extern "C" void vm_trap(const char *msg) {
    if (g_expect_trap)
        longjmp(g_trap_env, 1);
    if (g_return_trap) {
        g_return_trap_count++;
        return;
    }
    fprintf(stderr, "unexpected trap: %s\n", msg ? msg : "(null)");
    abort();
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_expect_trap = 1;                                                                         \
        if (setjmp(g_trap_env) == 0) {                                                             \
            (void)(expr);                                                                          \
            g_expect_trap = 0;                                                                     \
            assert(!"expected runtime trap");                                                      \
        } else {                                                                                   \
            g_expect_trap = 0;                                                                     \
        }                                                                                          \
    } while (0)

static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

static rt_string S(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static bool tag_eq(void *locale, const char *expected) {
    rt_string t = rt_locale_tag(locale);
    const char *cs = rt_string_cstr(t);
    bool ok = cs && strcmp(cs, expected) == 0;
    rt_string_unref(t);
    return ok;
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static std::string temp_dir(const char *name) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/zanna_locale_mgr_%ld_%s", base, (long)TEST_GETPID(), name);
    TEST_MKDIR(buf);
    return std::string(buf);
}

static void write_text_file(const std::string &path, const char *text) {
    FILE *f = fopen(path.c_str(), "wb");
    assert(f && "failed to create temp locale JSON");
    size_t len = strlen(text);
    assert(fwrite(text, 1, len, f) == len);
    fclose(f);
}

static const char *FR_JSON = R"json({
  "tag": "fr-FR",
  "names": { "language": "francais", "region": "France", "display": "francais (France)" },
  "text_direction": "ltr",
  "first_day_of_week": 1,
  "measurement": "metric",
  "numbers": {
    "decimal_sep": ",",
    "group_sep": " ",
    "group_size": 3,
    "minus": "-",
    "plus": "+",
    "percent": "%",
    "infinity": "Infinity",
    "nan": "NaN",
    "exponent": "e",
    "digits": "0123456789"
  },
  "currency": {
    "default_code": "EUR",
    "symbol": "EUR",
    "pattern_positive": "{n} {s}",
    "pattern_negative": "-{n} {s}",
    "fraction_digits": 2
  }
})json";

//=============================================================================
// Bootstrap + Current/System
//=============================================================================

static void test_bootstrap() {
    printf("Testing LocaleManager bootstrap:\n");

    // First call should register en-US and pick a sane current.
    void *cur = rt_locale_manager_current();
    test_result("Current() non-null after bootstrap", cur != nullptr);
    // Current is either the system locale (if loaded) or en-US fallback.
    // We can't assert a specific tag because the host env varies; instead,
    // verify IsLoaded(Current) is true — bootstrap must ensure this.
    test_result("IsLoaded(Current) == true", rt_locale_manager_is_loaded(cur) == 1);

    void *sys = rt_locale_manager_system();
    test_result("System() non-null", sys != nullptr);
}

//=============================================================================
// Available registry
//=============================================================================

static void test_available_includes_en_us() {
    printf("Testing LocaleManager.Available:\n");

    void *list = rt_locale_manager_available();
    test_result("Available returns non-null list", list != nullptr);

    extern int64_t rt_list_len(void *);
    extern void *rt_list_get(void *, int64_t);
    int64_t n = rt_list_len(list);
    test_result("Available length >= 1", n >= 1);

    // Scan the list for en-US presence.
    bool has_en_us = false;
    for (int64_t i = 0; i < n; ++i) {
        rt_string entry = (rt_string)rt_list_get(list, i);
        const char *cs = rt_string_cstr(entry);
        if (cs && strcmp(cs, "en-US") == 0)
            has_en_us = true;
    }
    test_result("Available includes en-US", has_en_us);
}

//=============================================================================
// IsLoaded
//=============================================================================

static void test_is_loaded() {
    printf("Testing LocaleManager.IsLoaded:\n");

    rt_string en_us = S("en-US");
    void *en = rt_locale_parse(en_us);
    rt_string_unref(en_us);
    test_result("IsLoaded(en-US) == true", rt_locale_manager_is_loaded(en) == 1);

    rt_string fr = S("fr-FR");
    void *loc_fr = rt_locale_try_parse(fr);
    rt_string_unref(fr);
    test_result("IsLoaded(fr-FR, unloaded) == false", rt_locale_manager_is_loaded(loc_fr) == 0);
}

//=============================================================================
// SetCurrent
//=============================================================================

static void test_set_current_to_loaded() {
    printf("Testing LocaleManager.SetCurrent:\n");

    rt_string en_us = S("en-US");
    void *en = rt_locale_parse(en_us);
    rt_string_unref(en_us);

    rt_locale_manager_set_current(en);
    void *cur = rt_locale_manager_current();
    test_result("SetCurrent(en-US) then Current() == en-US", tag_eq(cur, "en-US"));
}

static void test_set_current_to_unloaded_traps() {
    printf("Testing LocaleManager.SetCurrent trap path:\n");

    rt_string fr = S("fr-FR");
    void *loc_fr = rt_locale_try_parse(fr);
    rt_string_unref(fr);

    EXPECT_TRAP(rt_locale_manager_set_current(loc_fr));
    test_result("SetCurrent(unloaded) traps", true);
}

static void test_set_current_null_traps() {
    printf("Testing LocaleManager.SetCurrent(null) trap:\n");
    EXPECT_TRAP(rt_locale_manager_set_current(nullptr));
    test_result("SetCurrent(null) traps", true);
}

//=============================================================================
// LoadBuiltin
//=============================================================================

static void test_load_builtin_idempotent() {
    printf("Testing LocaleManager.LoadBuiltin:\n");

    rt_string tag = S("en-US");
    rt_locale_manager_load_builtin(tag);
    rt_string_unref(tag);

    // Calling again must not explode.
    rt_string tag2 = S("en-US");
    rt_locale_manager_load_builtin(tag2);
    rt_string_unref(tag2);

    test_result("LoadBuiltin(\"en-US\") idempotent", true);
}

static void test_load_builtin_unknown_traps() {
    printf("Testing LocaleManager.LoadBuiltin unknown:\n");

    rt_string tag = S("fr-FR");
    EXPECT_TRAP(rt_locale_manager_load_builtin(tag));
    rt_string_unref(tag);
    test_result("LoadBuiltin(\"fr-FR\") traps (not baked)", true);
}

//=============================================================================
// LoadFromJson / LoadFromAsset
//=============================================================================

static void test_load_from_json_registers_locale() {
    printf("Testing LocaleManager.LoadFromJson:\n");

    rt_locale_manager_reset();
    std::string dir = temp_dir("json");
    std::string file = dir + "/fr-FR.json";
    write_text_file(file, FR_JSON);

    rt_string path = S(file.c_str());
    rt_locale_manager_load_from_json(path);
    rt_string_unref(path);

    rt_string fr_tag = S("fr-FR");
    void *fr = rt_locale_parse(fr_tag);
    rt_string_unref(fr_tag);
    test_result("LoadFromJson registers fr-FR", rt_locale_manager_is_loaded(fr) == 1);
    release_obj(fr);

    rt_string path2 = S(file.c_str());
    int8_t ok = rt_locale_manager_try_load_from_json(path2);
    rt_string_unref(path2);
    test_result("TryLoadFromJson existing file returns 1", ok == 1);

    rt_string missing = S("nonexistent.json");
    int8_t missing_ok = rt_locale_manager_try_load_from_json(missing);
    rt_string_unref(missing);
    test_result("TryLoadFromJson missing file returns 0", missing_ok == 0);
}

static void test_load_from_json_schema_rejects_invalid() {
    printf("Testing LocaleManager.LoadFromJson schema rejection:\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("bad_schema");
    std::string file = dir + "/bad.json";
    write_text_file(file, R"json({
      "tag": "zz-ZZ",
      "numbers": {
        "decimal_sep": ".",
        "group_sep": ",",
        "group_size": 0,
        "digits": "0123456789"
      }
    })json");

    rt_string path = S(file.c_str());
    int8_t ok = rt_locale_manager_try_load_from_json(path);
    test_result("TryLoadFromJson rejects invalid group_size", ok == 0);
    EXPECT_TRAP(rt_locale_manager_load_from_json(path));
    rt_string_unref(path);
    test_result("LoadFromJson invalid schema traps", true);
}

static void test_load_from_json_rejects_embedded_nul() {
    printf("Testing LocaleManager.LoadFromJson NUL-escape rejection:\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("nul_escape");

    // VDOC-068: an escaped U+0000 must not truncate validation; the hidden
    // suffix would otherwise evade the tag and digit checks entirely.
    std::string file = dir + "/nul_tag.json";
    write_text_file(file, R"json({
      "tag": "zz\u0000-garbage",
      "numbers": {
        "decimal_sep": ".",
        "group_sep": ",",
        "group_size": 3,
        "digits": "0123456789"
      }
    })json");
    rt_string path = S(file.c_str());
    test_result("TryLoadFromJson rejects NUL in tag",
                rt_locale_manager_try_load_from_json(path) == 0);
    rt_string_unref(path);

    std::string file2 = dir + "/nul_digits.json";
    write_text_file(file2, R"json({
      "tag": "zz",
      "numbers": {
        "decimal_sep": ".",
        "group_sep": ",",
        "group_size": 3,
        "digits": "0123456789\u0000junk"
      }
    })json");
    rt_string path2 = S(file2.c_str());
    test_result("TryLoadFromJson rejects NUL in digits",
                rt_locale_manager_try_load_from_json(path2) == 0);
    rt_string_unref(path2);
}

static void test_load_from_json_rejects_malformed_digit_utf8() {
    printf("Testing LocaleManager digit UTF-8 strictness:\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("bad_digit_utf8");

    // VDOC-069: an overlong encoding (C0 80) must not count as a digit glyph.
    std::string file = dir + "/overlong.json";
    {
        std::string json = R"json({
      "tag": "zy",
      "numbers": {
        "decimal_sep": ".",
        "group_sep": ",",
        "group_size": 3,
        "digits": "012345678)json";
        json += '\xC0';
        json += '\x80';
        json += R"json("
      }
    })json";
        FILE *f = fopen(file.c_str(), "wb");
        assert(f);
        assert(fwrite(json.data(), 1, json.size(), f) == json.size());
        fclose(f);
    }
    rt_string path = S(file.c_str());
    test_result("TryLoadFromJson rejects overlong digit encoding",
                rt_locale_manager_try_load_from_json(path) == 0);
    rt_string_unref(path);

    // A valid multibyte digit set still loads (Arabic-Indic digits).
    std::string file2 = dir + "/arabic.json";
    write_text_file(
        file2,
        "{\n  \"tag\": \"zx\",\n  \"numbers\": {\n"
        "    \"decimal_sep\": \".\",\n    \"group_sep\": \",\",\n"
        "    \"group_size\": 3,\n"
        "    \"digits\": \"\u0660\u0661\u0662\u0663\u0664\u0665\u0666\u0667\u0668\u0669\"\n"
        "  }\n}\n");
    rt_string path2 = S(file2.c_str());
    test_result("TryLoadFromJson accepts valid multibyte digits",
                rt_locale_manager_try_load_from_json(path2) == 1);
    rt_string_unref(path2);
}

static void test_load_time_schema_validation() {
    printf("Testing load-time formatter validation (VDOC-070):\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("schema_strict");

    // Unsupported date pattern letter 'Q' must fail at load, not at format.
    std::string f1 = dir + "/bad_date.json";
    write_text_file(f1, R"json({
      "tag": "za",
      "dates": { "patterns": { "short": "Q" } }
    })json");
    rt_string p1 = S(f1.c_str());
    test_result("rejects unsupported date pattern letter",
                rt_locale_manager_try_load_from_json(p1) == 0);
    rt_string_unref(p1);

    // Duplicate currency placeholders cannot round-trip.
    std::string f2 = dir + "/bad_currency.json";
    write_text_file(f2, R"json({
      "tag": "zb",
      "currency": {
        "default_code": "USD",
        "pattern_positive": "{s}{n}{n}",
        "pattern_negative": "-{s}{n}"
      }
    })json");
    rt_string p2 = S(f2.c_str());
    test_result("rejects duplicate currency placeholder",
                rt_locale_manager_try_load_from_json(p2) == 0);
    rt_string_unref(p2);

    // Equal decimal/group separators are ambiguous.
    std::string f3 = dir + "/bad_seps.json";
    write_text_file(f3, R"json({
      "tag": "zc",
      "numbers": {
        "decimal_sep": ",",
        "group_sep": ",",
        "group_size": 3,
        "digits": "0123456789"
      }
    })json");
    rt_string p3 = S(f3.c_str());
    test_result("rejects equal decimal/group separators",
                rt_locale_manager_try_load_from_json(p3) == 0);
    rt_string_unref(p3);

    // Relative-time templates must carry {n}.
    std::string f4 = dir + "/bad_reltime.json";
    write_text_file(f4, R"json({
      "tag": "zd",
      "relative_time": { "past": "ago" }
    })json");
    rt_string p4 = S(f4.c_str());
    test_result("rejects relative-time template without {n}",
                rt_locale_manager_try_load_from_json(p4) == 0);
    rt_string_unref(p4);

    // List templates must carry {0} and {1}.
    std::string f5 = dir + "/bad_list.json";
    write_text_file(f5, R"json({
      "tag": "ze",
      "list_format": { "and": { "pair": "{0} and" } }
    })json");
    rt_string p5 = S(f5.c_str());
    test_result("rejects list template without {1}", rt_locale_manager_try_load_from_json(p5) == 0);
    rt_string_unref(p5);
}

static void test_collation_strength_range() {
    printf("Testing collation.strength load range (VDOC-082):\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("collation_strength");
    std::string file = dir + "/s4.json";
    write_text_file(file, R"json({
      "tag": "zv",
      "collation": { "strength": 4 }
    })json");
    rt_string path = S(file.c_str());
    test_result("strength 4 rejected (collator supports 1..3)",
                rt_locale_manager_try_load_from_json(path) == 0);
    rt_string_unref(path);

    std::string file2 = dir + "/s3.json";
    write_text_file(file2, R"json({
      "tag": "zu",
      "collation": { "strength": 3 }
    })json");
    rt_string path2 = S(file2.c_str());
    test_result("strength 3 accepted", rt_locale_manager_try_load_from_json(path2) == 1);
    rt_string_unref(path2);
}

static void test_load_from_json_missing_traps() {
    printf("Testing LocaleManager.LoadFromJson missing path:\n");

    rt_string path = S("nonexistent.json");
    EXPECT_TRAP(rt_locale_manager_load_from_json(path));
    rt_string_unref(path);
    test_result("LoadFromJson(\"nonexistent.json\") traps", true);
}

static void test_load_from_asset_missing() {
    printf("Testing LocaleManager.LoadFromAsset missing asset:\n");

    rt_string name = S("locales/fr-FR.json");
    EXPECT_TRAP(rt_locale_manager_load_from_asset(name));
    rt_string_unref(name);
    test_result("LoadFromAsset missing asset traps", true);

    rt_string name2 = S("locales/fr-FR.json");
    int8_t ok = rt_locale_manager_try_load_from_asset(name2);
    rt_string_unref(name2);
    test_result("TryLoadFromAsset missing asset returns 0", ok == 0);
}

//=============================================================================
// Search path
//=============================================================================

static void test_search_path_roundtrip() {
    printf("Testing LocaleManager search paths:\n");

    // Before adding, search path is empty.
    {
        rt_locale_manager_reset();
        rt_string sp = rt_locale_manager_search_path();
        const char *cs = rt_string_cstr(sp);
        test_result("SearchPath empty after Reset", cs && *cs == '\0');
        rt_string_unref(sp);
    }
    // Add one path.
    {
        rt_string p = S("/opt/zanna/locales");
        rt_locale_manager_add_search_path(p);
        rt_string_unref(p);
        rt_string sp = rt_locale_manager_search_path();
        test_result("SearchPath contains added path",
                    strcmp(rt_string_cstr(sp), "/opt/zanna/locales") == 0);
        rt_string_unref(sp);
    }
    // Add a second; separator should appear.
    {
        rt_string p = S("/home/user/.zanna/locales");
        rt_locale_manager_add_search_path(p);
        rt_string_unref(p);
        rt_string sp = rt_locale_manager_search_path();
        const char *cs = rt_string_cstr(sp);
        // Don't hard-code the separator; just check both sub-paths appear.
        test_result("SearchPath contains first path", strstr(cs, "/opt/zanna/locales") != nullptr);
        test_result("SearchPath contains second path",
                    strstr(cs, "/home/user/.zanna/locales") != nullptr);
        rt_string_unref(sp);
    }
    // Reset clears.
    {
        rt_locale_manager_reset();
        rt_string sp = rt_locale_manager_search_path();
        test_result("Reset clears SearchPath", strcmp(rt_string_cstr(sp), "") == 0);
        rt_string_unref(sp);
    }
}

//=============================================================================
// Load high-level
//=============================================================================

static void test_load_high_level() {
    printf("Testing LocaleManager.Load:\n");

    rt_locale_manager_reset();
    rt_string en_tag = S("en-US");
    void *en = rt_locale_manager_load(en_tag);
    rt_string_unref(en_tag);
    test_result("Load(\"en-US\") returns a Locale", en != nullptr);
    test_result("Loaded en-US has canonical tag", tag_eq(en, "en-US"));

    // Unregistered locale returns NULL.
    rt_string fr_tag = S("fr-FR");
    void *fr = rt_locale_manager_load(fr_tag);
    rt_string_unref(fr_tag);
    test_result("Load(\"fr-FR\") returns NULL (not registered)", fr == nullptr);

    std::string dir = temp_dir("search");
    write_text_file(dir + "/fr-FR.json", FR_JSON);
    rt_string path = S(dir.c_str());
    rt_locale_manager_add_search_path(path);
    rt_string_unref(path);

    rt_string mixed_tag = S("FR_fr");
    void *loaded_fr = rt_locale_manager_load(mixed_tag);
    rt_string_unref(mixed_tag);
    test_result("Load canonicalizes and searches for fr-FR", loaded_fr != nullptr);
    test_result("Loaded searched locale has canonical tag", tag_eq(loaded_fr, "fr-FR"));
    release_obj(loaded_fr);
    release_obj(en);
}

//=============================================================================
// Unload refusal
//=============================================================================

static void test_unload_current_refused() {
    printf("Testing LocaleManager.Unload refusal:\n");

    void *cur = rt_locale_manager_current();
    int8_t res = rt_locale_manager_unload(cur);
    test_result("Unload(current) returns 0", res == 0);

    // Unload a baked entry (en-US) should also return 0.
    rt_string en_tag = S("en-US");
    void *en = rt_locale_parse(en_tag);
    rt_string_unref(en_tag);
    int8_t res2 = rt_locale_manager_unload(en);
    test_result("Unload(baked en-US) returns 0", res2 == 0);
}

static void test_loaded_locale_handle_blocks_unload_and_replace() {
    printf("Testing loaded locale data lifetime:\n");
    rt_locale_manager_reset();
    std::string dir = temp_dir("lifetime");
    std::string file = dir + "/fr-FR.json";
    write_text_file(file, FR_JSON);

    rt_string path = S(file.c_str());
    rt_locale_manager_load_from_json(path);

    rt_string fr_tag = S("fr-FR");
    void *fr = rt_locale_parse(fr_tag);
    rt_string_unref(fr_tag);
    rt_string fr_tag2 = S("fr-FR");
    void *fr2 = rt_locale_parse(fr_tag2);
    rt_string_unref(fr_tag2);

    test_result("Unload(fr-FR) refused while another Locale handle lives",
                rt_locale_manager_unload(fr) == 0);
    release_obj(fr2);
    test_result("TryLoadFromJson returns 0 while replacing live locale",
                rt_locale_manager_try_load_from_json(path) == 0);
    EXPECT_TRAP(rt_locale_manager_load_from_json(path));
    test_result("LoadFromJson replacing live locale traps", true);

    test_result("Unload(fr-FR) succeeds when passed sole retained handle",
                rt_locale_manager_unload(fr) == 1);
    release_obj(fr);
    rt_string_unref(path);
}

static void test_locale_data_refcount_overflow_is_nonwrapping() {
    printf("Testing locale-data refcount saturation:\n");

    rt_locale_data_t data{};
    data.arena = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
    data.formatter_refs = std::numeric_limits<int64_t>::max();

    g_return_trap = 1;
    g_return_trap_count = 0;
    rt_locale_manager_retain_data(&data);
    g_return_trap = 0;

    test_result("Retain at INT64_MAX traps once", g_return_trap_count == 1);
    test_result("Retain at INT64_MAX leaves the counter pinned",
                data.formatter_refs == std::numeric_limits<int64_t>::max());

    g_return_trap = 1;
    g_return_trap_count = 0;
    rt_locale_manager_release_data(&data);
    g_return_trap = 0;

    test_result("Release of a saturated counter traps once", g_return_trap_count == 1);
    test_result("Release leaves a saturated counter pinned",
                data.formatter_refs == std::numeric_limits<int64_t>::max());
}

//=============================================================================
// Main
//=============================================================================

int main() {
    printf("=== RT LocaleManager Tests ===\n\n");
    test_bootstrap();
    test_available_includes_en_us();
    test_is_loaded();
    test_set_current_to_loaded();
    test_set_current_to_unloaded_traps();
    test_set_current_null_traps();
    test_load_builtin_idempotent();
    test_load_builtin_unknown_traps();
    test_load_from_json_registers_locale();
    test_load_from_json_schema_rejects_invalid();
    test_load_from_json_missing_traps();
    test_load_from_json_rejects_embedded_nul();
    test_load_from_json_rejects_malformed_digit_utf8();
    test_load_time_schema_validation();
    test_collation_strength_range();
    test_load_from_asset_missing();
    test_search_path_roundtrip();
    test_load_high_level();
    test_unload_current_refused();
    test_loaded_locale_handle_blocks_unload_and_replace();
    test_locale_data_refcount_overflow_is_nonwrapping();
    printf("\nAll LocaleManager tests passed!\n");
    return 0;
}
