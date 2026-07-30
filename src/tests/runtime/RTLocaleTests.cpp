//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTLocaleTests.cpp
// Purpose: Validate Zanna.Localization.Locale parsing, canonicalization, and
//          fallback-chain behavior, including returning-trap allocation paths.
// Key invariants:
//   - Canonical BCP-47 casing and field classification remain consistent.
//   - Invalid tags trap only in strict APIs.
//   - Allocation failure cannot fall through into a null Locale payload.
// Ownership/Lifetime:
//   - Test-created runtime strings and Locale handles follow runtime reference
//     ownership; allocator hooks are restored before each test returns.
// Links: src/runtime/localization/rt_locale.c, src/runtime/localization/rt_locale.h
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_manager.h"
#include "rt_locale_platform.h"
#include "rt_locale_posix_tag.h"
#include "rt_option.h"
#include "rt_string.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>

static jmp_buf g_trap_env;
static int g_expect_trap = 0;
static int g_return_traps = 0;
static int g_returning_trap_count = 0;

extern "C" void vm_trap(const char *msg) {
    if (g_return_traps) {
        (void)msg;
        ++g_returning_trap_count;
        return;
    }
    if (g_expect_trap)
        longjmp(g_trap_env, 1);
    fprintf(stderr, "unexpected trap: %s\n", msg ? msg : "(null)");
    abort();
}

static void *fail_locale_alloc(int64_t bytes, void *(*next)(int64_t)) {
    (void)bytes;
    (void)next;
    return nullptr;
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

static bool field_eq(rt_string s, const char *expected) {
    const char *cs = rt_string_cstr(s);
    bool ok = cs && strcmp(cs, expected) == 0;
    rt_string_unref(s);
    return ok;
}

static void test_platform_tag_normalization() {
    char tag[32];

    assert(rt_locale_posix_value_is_invariant("C") == 1);
    assert(rt_locale_posix_value_is_invariant("c.UTF-8") == 1);
    assert(rt_locale_posix_value_is_invariant("PoSiX@legacy") == 1);
    assert(rt_locale_posix_value_is_invariant("en_US.UTF-8") == 0);

    assert(rt_locale_clean_posix_tag("fr_FR.UTF-8", tag, sizeof(tag)) == 0);
    assert(strcmp(tag, "fr-FR") == 0);
    assert(rt_locale_clean_posix_tag("zh_Hans_CN@pinyin", tag, sizeof(tag)) == 0);
    assert(strcmp(tag, "zh-Hans-CN") == 0);

    const char *invalid[] = {
        "-en", "en-", "en__US", "en/US", "1n_US", "toolongtag_US", "\xc3\xa9_US", nullptr};
    for (int i = 0; invalid[i]; i++) {
        strcpy(tag, "stale");
        assert(rt_locale_clean_posix_tag(invalid[i], tag, sizeof(tag)) == -1);
        assert(tag[0] == '\0');
    }

    char tiny[1] = {'x'};
    assert(rt_locale_platform_detect_system(tiny, sizeof(tiny)) == -1);
    assert(tiny[0] == '\0');
}

//=============================================================================
// Parse — happy path
//=============================================================================

static void test_parse_basic_tags() {
    printf("Testing Locale.Parse basic tags:\n");

    {
        rt_string in = S("en");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"en\") -> en", tag_eq(loc, "en"));
    }
    {
        rt_string in = S("en-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"en-US\") -> en-US", tag_eq(loc, "en-US"));
    }
    {
        rt_string in = S("fr-FR");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"fr-FR\") -> fr-FR", tag_eq(loc, "fr-FR"));
    }
    {
        rt_string in = S("en-Latn-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"en-Latn-US\") -> en-Latn-US", tag_eq(loc, "en-Latn-US"));
    }
    {
        rt_string in = S("zh-Hans-CN");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"zh-Hans-CN\") -> zh-Hans-CN", tag_eq(loc, "zh-Hans-CN"));
    }
}

//=============================================================================
// Parse — canonicalization
//=============================================================================

static void test_parse_canonicalization() {
    printf("Testing Locale.Parse canonicalization:\n");

    // Mixed case input → language lowercased, region uppercased.
    {
        rt_string in = S("EN_us");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"EN_us\") canonical", tag_eq(loc, "en-US"));
    }
    // Underscore separator is accepted and normalized to dash.
    {
        rt_string in = S("de_DE");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"de_DE\") canonical", tag_eq(loc, "de-DE"));
    }
    // Script case is normalized to Title-case.
    {
        rt_string in = S("ZH-hans-cn");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"ZH-hans-cn\") canonical", tag_eq(loc, "zh-Hans-CN"));
    }
    // 3-digit region (UN M.49) survives as-is.
    {
        rt_string in = S("es-419");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"es-419\") canonical", tag_eq(loc, "es-419"));
    }
    // "root" maps to invariant.
    {
        rt_string in = S("root");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"root\") -> root", tag_eq(loc, "root"));
    }
    // Variants/extensions/private-use are preserved and canonicalized.
    {
        rt_string in = S("EN_us-u-CA-gregory-x-PRIVATE");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse extension/private-use canonical",
                    tag_eq(loc, "en-US-u-ca-gregory-x-private"));
    }
    // Multiple distinct extension singleton groups are valid and canonicalized.
    {
        rt_string in = S("en-US-a-foo-b-bar-u-ca-gregory");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse multiple extension groups",
                    tag_eq(loc, "en-US-a-foo-b-bar-u-ca-gregory"));
    }
}

//=============================================================================
// Property accessors
//=============================================================================

static void test_property_accessors() {
    printf("Testing Locale property accessors:\n");

    {
        rt_string in = S("en-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("en-US.Language == \"en\"", field_eq(rt_locale_language(loc), "en"));
        test_result("en-US.Script == \"\"", field_eq(rt_locale_script(loc), ""));
        test_result("en-US.Region == \"US\"", field_eq(rt_locale_region(loc), "US"));
        test_result("en-US.Tag == \"en-US\"", field_eq(rt_locale_tag(loc), "en-US"));
    }
    {
        rt_string in = S("en-Latn-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("en-Latn-US.Script == \"Latn\"", field_eq(rt_locale_script(loc), "Latn"));
    }
    {
        void *root = rt_locale_invariant();
        test_result("Invariant().Tag == \"root\"", field_eq(rt_locale_tag(root), "root"));
        test_result("Invariant().Language == \"\"", field_eq(rt_locale_language(root), ""));
    }
}

//=============================================================================
// FromParts
//=============================================================================

static void test_from_parts() {
    printf("Testing Locale.FromParts:\n");

    {
        rt_string lang = S("en");
        rt_string script = S("");
        rt_string region = S("US");
        void *loc = rt_locale_from_parts(lang, script, region);
        rt_string_unref(lang);
        rt_string_unref(script);
        rt_string_unref(region);
        test_result("FromParts(en,\"\",US) -> en-US", tag_eq(loc, "en-US"));
    }
    {
        rt_string lang = S("zh");
        rt_string script = S("Hans");
        rt_string region = S("CN");
        void *loc = rt_locale_from_parts(lang, script, region);
        rt_string_unref(lang);
        rt_string_unref(script);
        rt_string_unref(region);
        test_result("FromParts(zh,Hans,CN) -> zh-Hans-CN", tag_eq(loc, "zh-Hans-CN"));
    }
}

//=============================================================================
// Equals
//=============================================================================

static void test_equals() {
    printf("Testing Locale.Equals:\n");

    rt_string a_str = S("en-US");
    rt_string b_str = S("EN_us");
    rt_string c_str = S("fr-FR");
    void *a = rt_locale_parse(a_str);
    void *b = rt_locale_parse(b_str); // canonicalizes to en-US
    void *c = rt_locale_parse(c_str);
    rt_string_unref(a_str);
    rt_string_unref(b_str);
    rt_string_unref(c_str);

    test_result("Equals(en-US, en-US) = 1", rt_locale_equals(a, a) == 1);
    test_result("Equals(en-US, EN_us canonical)", rt_locale_equals(a, b) == 1);
    test_result("Equals(en-US, fr-FR) = 0", rt_locale_equals(a, c) == 0);
    test_result("Equals(null, null) = 1", rt_locale_equals(nullptr, nullptr) == 1);
    test_result("Equals(a, null) = 0", rt_locale_equals(a, nullptr) == 0);
}

//=============================================================================
// Fallbacks
//=============================================================================

static int64_t list_len(void *list) {
    extern int64_t rt_list_len(void *);
    return rt_list_len(list);
}

static bool list_tag_at(void *list, int64_t idx, const char *expected) {
    extern void *rt_list_get(void *, int64_t);
    void *loc = rt_list_get(list, idx);
    return tag_eq(loc, expected);
}

static void test_fallbacks() {
    printf("Testing Locale.Fallbacks:\n");

    // en-Latn-US -> [en-Latn-US, en-US, en, root]
    {
        rt_string in = S("en-Latn-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        void *chain = rt_locale_fallbacks(loc);
        test_result("en-Latn-US chain length 4", list_len(chain) == 4);
        test_result("chain[0] = en-Latn-US", list_tag_at(chain, 0, "en-Latn-US"));
        test_result("chain[1] = en-US", list_tag_at(chain, 1, "en-US"));
        test_result("chain[2] = en", list_tag_at(chain, 2, "en"));
        test_result("chain[3] = root", list_tag_at(chain, 3, "root"));
    }
    // en-US -> [en-US, en, root]
    {
        rt_string in = S("en-US");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        void *chain = rt_locale_fallbacks(loc);
        test_result("en-US chain length 3", list_len(chain) == 3);
        test_result("chain[0] = en-US", list_tag_at(chain, 0, "en-US"));
        test_result("chain[1] = en", list_tag_at(chain, 1, "en"));
        test_result("chain[2] = root", list_tag_at(chain, 2, "root"));
    }
    // en -> [en, root]
    {
        rt_string in = S("en");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        void *chain = rt_locale_fallbacks(loc);
        test_result("en chain length 2", list_len(chain) == 2);
        test_result("chain[0] = en", list_tag_at(chain, 0, "en"));
        test_result("chain[1] = root", list_tag_at(chain, 1, "root"));
    }
    // root -> [root] (special case: invariant-only chain)
    {
        void *root = rt_locale_invariant();
        void *chain = rt_locale_fallbacks(root);
        test_result("root chain length 1", list_len(chain) == 1);
        test_result("chain[0] = root", list_tag_at(chain, 0, "root"));
    }
    // Extensions/variants fall back through their base tag.
    {
        rt_string in = S("en-US-u-ca-gregory-x-private");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        void *chain = rt_locale_fallbacks(loc);
        test_result("extension chain length 4", list_len(chain) == 4);
        test_result("extension chain[0] full tag",
                    list_tag_at(chain, 0, "en-US-u-ca-gregory-x-private"));
        test_result("extension chain[1] = en-US", list_tag_at(chain, 1, "en-US"));
        test_result("extension chain[2] = en", list_tag_at(chain, 2, "en"));
        test_result("extension chain[3] = root", list_tag_at(chain, 3, "root"));
    }
    {
        rt_string in = S("sl-rozaj-biske");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        void *chain = rt_locale_fallbacks(loc);
        test_result("variant chain length 3", list_len(chain) == 3);
        test_result("variant chain[0] full tag", list_tag_at(chain, 0, "sl-rozaj-biske"));
        test_result("variant chain[1] = sl", list_tag_at(chain, 1, "sl"));
        test_result("variant chain[2] = root", list_tag_at(chain, 2, "root"));
    }
}

//=============================================================================
// Traps
//=============================================================================

static void test_trap_empty() {
    printf("Testing Locale.Parse trap paths:\n");

    rt_string empty = S("");
    EXPECT_TRAP(rt_locale_parse(empty));
    rt_string_unref(empty);
    test_result("Parse(\"\") traps", true);

    // Single-character "tag" is invalid (language min 2 chars).
    rt_string one = S("x");
    EXPECT_TRAP(rt_locale_parse(one));
    rt_string_unref(one);
    test_result("Parse(\"x\") traps", true);

    // Non-alpha input.
    rt_string bogus = S("123");
    EXPECT_TRAP(rt_locale_parse(bogus));
    rt_string_unref(bogus);
    test_result("Parse(\"123\") traps", true);

    // Subtag exceeding 8 chars.
    rt_string toolong = S("englishlang");
    EXPECT_TRAP(rt_locale_parse(toolong));
    rt_string_unref(toolong);
    test_result("Parse(\"englishlang\") traps", true);

    rt_string dup_sep = S("en--US");
    EXPECT_TRAP(rt_locale_parse(dup_sep));
    rt_string_unref(dup_sep);
    test_result("Parse(\"en--US\") traps", true);

    rt_string leading_sep = S("-en-US");
    EXPECT_TRAP(rt_locale_parse(leading_sep));
    rt_string_unref(leading_sep);
    test_result("Parse(\"-en-US\") traps", true);

    rt_string trailing_sep = S("en-US-");
    EXPECT_TRAP(rt_locale_parse(trailing_sep));
    rt_string_unref(trailing_sep);
    test_result("Parse(\"en-US-\") traps", true);

    rt_string dangling_ext = S("en-US-u");
    EXPECT_TRAP(rt_locale_parse(dangling_ext));
    rt_string_unref(dangling_ext);
    test_result("Parse(\"en-US-u\") traps", true);

    rt_string dup_ext = S("en-US-u-ca-gregory-u-nu-latn");
    EXPECT_TRAP(rt_locale_parse(dup_ext));
    rt_string_unref(dup_ext);
    test_result("Parse duplicate extension singleton traps", true);
}

static void test_try_parse_returns_null() {
    printf("Testing Locale.TryParse soft failure:\n");

    rt_string bogus = S("!@#");
    void *loc = rt_locale_try_parse(bogus);
    rt_string_unref(bogus);
    test_result("TryParse(\"!@#\") returns NULL", loc == nullptr);

    rt_string empty = S("");
    void *loc2 = rt_locale_try_parse(empty);
    rt_string_unref(empty);
    test_result("TryParse(\"\") returns NULL", loc2 == nullptr);

    rt_string invalid_option_tag = S("!@#");
    void *invalid_option = rt_locale_try_parse_option(invalid_option_tag);
    rt_string_unref(invalid_option_tag);
    test_result("TryParseOption(\"!@#\") returns None", rt_option_is_none(invalid_option) == 1);

    rt_string valid_option_tag = S("en-US");
    void *valid_option = rt_locale_try_parse_option(valid_option_tag);
    rt_string_unref(valid_option_tag);
    test_result("TryParseOption(\"en-US\") returns Some", rt_option_is_some(valid_option) == 1);
}

//=============================================================================
// Main
//=============================================================================

static void test_bcp47_conformance() {
    printf("Testing BCP-47 conformance (VDOC-065):\n");

    // Valid forms previously rejected.
    {
        rt_string in = S("x-private");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"x-private\") accepted", tag_eq(loc, "x-private"));
    }
    {
        rt_string in = S("zh-cmn-Hans-CN");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse(\"zh-cmn-Hans-CN\") extlang accepted", tag_eq(loc, "zh-cmn-Hans-CN"));
    }
    {
        // Tags longer than the old 39-byte canonical cap.
        rt_string in = S("en-US-u-ca-gregory-nu-latn-x-one-two-three-four");
        void *loc = rt_locale_parse(in);
        rt_string_unref(in);
        test_result("Parse of >39-byte tag accepted",
                    tag_eq(loc, "en-US-u-ca-gregory-nu-latn-x-one-two-three-four"));
    }

    // Malformed forms previously accepted.
    const char *bad[] = {
        "en-a-b-foo",     // empty 'a' extension
        "en-a-x-foo",     // extension with no subtag before private-use
        "en-abcde-Latn",  // script after variant
        "en-abcde-US",    // region after variant
        "sl-rozaj-rozaj", // duplicate variant
    };
    for (int i = 0; i < 5; i++) {
        rt_string in = S(bad[i]);
        EXPECT_TRAP(rt_locale_parse(in));
        rt_string_unref(in);
        test_result(bad[i], true);
    }
    printf("\n");
}

static void test_from_parts_single_subtags() {
    printf("Testing FromParts single-subtag enforcement (VDOC-066):\n");

    {
        rt_string lang = S("en");
        rt_string script = S("Latn");
        rt_string region = S("US");
        void *loc = rt_locale_from_parts(lang, script, region);
        rt_string_unref(lang);
        rt_string_unref(script);
        rt_string_unref(region);
        test_result("FromParts(en, Latn, US) -> en-Latn-US", tag_eq(loc, "en-Latn-US"));
    }
    {
        // Multi-subtag values in any field are rejected.
        rt_string lang = S("en-US");
        rt_string empty = S("");
        EXPECT_TRAP(rt_locale_from_parts(lang, empty, empty));
        rt_string_unref(lang);
        rt_string_unref(empty);
        test_result("FromParts(\"en-US\", , ) traps", true);
    }
    {
        rt_string lang = S("en");
        rt_string script = S("Latn-US");
        rt_string empty = S("");
        EXPECT_TRAP(rt_locale_from_parts(lang, script, empty));
        rt_string_unref(lang);
        rt_string_unref(script);
        rt_string_unref(empty);
        test_result("FromParts(en, \"Latn-US\", ) traps", true);
    }
    {
        // A region shape passed as script must not silently remap.
        rt_string lang = S("en");
        rt_string script = S("US");
        rt_string empty = S("");
        EXPECT_TRAP(rt_locale_from_parts(lang, script, empty));
        rt_string_unref(lang);
        rt_string_unref(script);
        rt_string_unref(empty);
        test_result("FromParts(en, \"US\", ) traps", true);
    }
    printf("\n");
}

static void test_null_equals_invariant() {
    printf("Testing null-handle equality (VDOC-067):\n");
    void *inv = rt_locale_invariant();
    test_result("Equals(null, Invariant()) is true", rt_locale_equals(NULL, inv) == 1);
    test_result("Equals(Invariant(), null) is true", rt_locale_equals(inv, NULL) == 1);
    test_result("Equals(null, null) is true", rt_locale_equals(NULL, NULL) == 1);
    rt_string in = S("en-US");
    void *en = rt_locale_parse(in);
    rt_string_unref(in);
    test_result("Equals(null, en-US) is false", rt_locale_equals(NULL, en) == 0);
    printf("\n");
}

static void test_returning_allocation_traps_stop_before_payload_access() {
    printf("Testing returning allocation traps:\n");

    g_return_traps = 1;
    g_returning_trap_count = 0;
    rt_set_alloc_hook(fail_locale_alloc);
    void *invariant = rt_locale_new();
    rt_set_alloc_hook(nullptr);
    test_result("Locale.New returns null after resumed OOM trap",
                invariant == nullptr && g_returning_trap_count > 0);

    rt_string tag = S("en-US");
    g_returning_trap_count = 0;
    rt_set_alloc_hook(fail_locale_alloc);
    void *parsed = rt_locale_parse(tag);
    rt_set_alloc_hook(nullptr);
    test_result("Locale.Parse returns null after resumed OOM trap",
                parsed == nullptr && g_returning_trap_count > 0);
    rt_string_unref(tag);

    rt_string language = S("en");
    rt_string script = S("Latn");
    rt_string region = S("US");
    g_returning_trap_count = 0;
    rt_set_alloc_hook(fail_locale_alloc);
    void *from_parts = rt_locale_from_parts(language, script, region);
    rt_set_alloc_hook(nullptr);
    test_result("Locale.FromParts returns null after resumed OOM trap",
                from_parts == nullptr && g_returning_trap_count > 0);
    rt_string_unref(language);
    rt_string_unref(script);
    rt_string_unref(region);

    g_return_traps = 0;
    printf("\n");
}

int main() {
    test_platform_tag_normalization();
    test_bcp47_conformance();
    test_from_parts_single_subtags();
    test_null_equals_invariant();
    test_returning_allocation_traps_stop_before_payload_access();
    printf("=== RT Locale Tests ===\n\n");
    test_parse_basic_tags();
    test_parse_canonicalization();
    test_property_accessors();
    test_from_parts();
    test_equals();
    test_fallbacks();
    test_trap_empty();
    test_try_parse_returns_null();
    printf("\nAll Locale tests passed!\n");
    return 0;
}
