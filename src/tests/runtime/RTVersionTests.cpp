//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTVersionTests.cpp
// Purpose: Validate SemVer parsing, formatting, comparison, constraints, and
//          allocation-failure rollback.
// Key invariants:
//   - Valid components and metadata round-trip without changing precedence.
//   - Malformed or unrepresentable versions are rejected.
//   - A returning trap hook never permits parsing to publish partial metadata.
// Ownership/Lifetime:
//   - Test-created strings and injected native allocations are released by the
//     runtime paths under test.
// Links: src/runtime/text/rt_version.c, src/runtime/text/rt_version.h
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_string.h"
#include "rt_version.h"

#include <cassert>
#include <cstring>
#include <string>

static bool g_return_traps = false;
static const char *g_last_trap = nullptr;
static int g_alloc_fail_countdown = 0;

extern "C" void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_last_trap = msg;
        return;
    }
    rt_abort(msg);
}

static void *fail_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    if (g_alloc_fail_countdown > 0 && --g_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

// ---------------------------------------------------------------------------
// Parse tests
// ---------------------------------------------------------------------------

static void test_parse_basic() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    assert(v != NULL);
    assert(rt_version_major(v) == 1);
    assert(rt_version_minor(v) == 2);
    assert(rt_version_patch(v) == 3);
    rt_string_unref(s);
}

static void test_parse_with_v_prefix() {
    rt_string s = make_str("v2.0.1");
    void *v = rt_version_parse(s);
    assert(v != NULL);
    assert(rt_version_major(v) == 2);
    assert(rt_version_minor(v) == 0);
    assert(rt_version_patch(v) == 1);
    rt_string_unref(s);
}

static void test_parse_prerelease() {
    rt_string s = make_str("1.0.0-alpha.1");
    void *v = rt_version_parse(s);
    assert(v != NULL);
    rt_string pr = rt_version_prerelease(v);
    assert(str_eq(pr, "alpha.1"));
    rt_string_unref(pr);
    rt_string_unref(s);
}

static void test_parse_build() {
    rt_string s = make_str("1.0.0+build.42");
    void *v = rt_version_parse(s);
    assert(v != NULL);
    rt_string b = rt_version_build(v);
    assert(str_eq(b, "build.42"));
    rt_string_unref(b);
    rt_string_unref(s);
}

static void test_parse_full() {
    rt_string s = make_str("1.2.3-beta.1+linux.amd64");
    void *v = rt_version_parse(s);
    assert(v != NULL);
    assert(rt_version_major(v) == 1);
    assert(rt_version_minor(v) == 2);
    assert(rt_version_patch(v) == 3);

    rt_string pr = rt_version_prerelease(v);
    assert(str_eq(pr, "beta.1"));
    rt_string_unref(pr);

    rt_string b = rt_version_build(v);
    assert(str_eq(b, "linux.amd64"));
    rt_string_unref(b);

    rt_string_unref(s);
}

static void test_parse_rejects_missing_patch() {
    rt_string s = make_str("1.0");
    void *v = rt_version_parse(s);
    assert(v == NULL);
    rt_string_unref(s);
}

static void test_parse_rejects_invalid_semver_identifiers() {
    const char *invalid[] = {"1.2.3-",
                             "1.2.3+",
                             "1.2.3-alpha..1",
                             "1.2.3-01",
                             "1.2.3-alpha_beta",
                             "1.2.3+build..1",
                             "1.2.3+build+again"};
    for (const char *text : invalid) {
        rt_string s = make_str(text);
        assert(rt_version_parse(s) == NULL);
        rt_string_unref(s);
    }
}

static void test_parse_rejects_numeric_overflow() {
    rt_string s = make_str("9223372036854775808.0.0");
    assert(rt_version_parse(s) == NULL);
    rt_string_unref(s);
}

static void test_is_valid() {
    rt_string valid = make_str("1.2.3");
    assert(rt_version_is_valid(valid) == 1);
    rt_string_unref(valid);

    rt_string invalid = make_str("not-a-version");
    assert(rt_version_is_valid(invalid) == 0);
    rt_string_unref(invalid);
}

static void test_parse_allocation_failures_stop_cleanly() {
    rt_string prerelease = make_str("1.2.3-alpha");
    g_return_traps = true;
    g_last_trap = nullptr;
    g_alloc_fail_countdown = 1;
    rt_set_alloc_hook(fail_countdown_alloc);
    void *failed_prerelease = rt_version_parse(prerelease);
    rt_set_alloc_hook(nullptr);
    assert(failed_prerelease == nullptr);
    assert(g_last_trap != nullptr);
    rt_string_unref(prerelease);

    rt_string full = make_str("1.2.3-alpha+build");
    g_last_trap = nullptr;
    g_alloc_fail_countdown = 2;
    rt_set_alloc_hook(fail_countdown_alloc);
    void *failed_build = rt_version_parse(full);
    rt_set_alloc_hook(nullptr);
    assert(failed_build == nullptr);
    assert(g_last_trap != nullptr);
    rt_string_unref(full);

    g_alloc_fail_countdown = 0;
    g_return_traps = false;
}

// ---------------------------------------------------------------------------
// ToString tests
// ---------------------------------------------------------------------------

static void test_to_string() {
    rt_string s = make_str("1.2.3-beta.1+build.42");
    void *v = rt_version_parse(s);
    rt_string str = rt_version_to_string(v);
    assert(str_eq(str, "1.2.3-beta.1+build.42"));
    rt_string_unref(str);
    rt_string_unref(s);
}

static void test_to_string_long_metadata() {
    std::string input = "1.2.3-";
    input.append(260, 'a');
    input += "+";
    input.append(260, 'b');

    rt_string s = make_str(input.c_str());
    void *v = rt_version_parse(s);
    assert(v != NULL);
    rt_string str = rt_version_to_string(v);
    assert(rt_str_len(str) == (int64_t)input.size());
    assert(strcmp(rt_string_cstr(str), input.c_str()) == 0);
    rt_string_unref(str);
    rt_string_unref(s);
}

// ---------------------------------------------------------------------------
// Comparison tests
// ---------------------------------------------------------------------------

static void test_cmp_equal() {
    rt_string s1 = make_str("1.2.3");
    rt_string s2 = make_str("1.2.3");
    void *a = rt_version_parse(s1);
    void *b = rt_version_parse(s2);
    assert(rt_version_cmp(a, b) == 0);
    rt_string_unref(s1);
    rt_string_unref(s2);
}

static void test_cmp_major() {
    rt_string s1 = make_str("1.0.0");
    rt_string s2 = make_str("2.0.0");
    void *a = rt_version_parse(s1);
    void *b = rt_version_parse(s2);
    assert(rt_version_cmp(a, b) == -1);
    assert(rt_version_cmp(b, a) == 1);
    rt_string_unref(s1);
    rt_string_unref(s2);
}

static void test_cmp_prerelease() {
    // Pre-release < release
    rt_string s1 = make_str("1.0.0-alpha");
    rt_string s2 = make_str("1.0.0");
    void *a = rt_version_parse(s1);
    void *b = rt_version_parse(s2);
    assert(rt_version_cmp(a, b) < 0);
    rt_string_unref(s1);
    rt_string_unref(s2);
}

static void test_cmp_prerelease_order() {
    rt_string s1 = make_str("1.0.0-alpha");
    rt_string s2 = make_str("1.0.0-beta");
    void *a = rt_version_parse(s1);
    void *b = rt_version_parse(s2);
    assert(rt_version_cmp(a, b) < 0);
    rt_string_unref(s1);
    rt_string_unref(s2);
}

// ---------------------------------------------------------------------------
// Constraint tests
// ---------------------------------------------------------------------------

static void test_satisfies_gte() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    rt_string c = make_str(">=1.0.0");
    assert(rt_version_satisfies(v, c) == 1);
    rt_string_unref(c);

    c = make_str(">=2.0.0");
    assert(rt_version_satisfies(v, c) == 0);
    rt_string_unref(c);

    rt_string_unref(s);
}

static void test_satisfies_caret() {
    rt_string s = make_str("1.5.3");
    void *v = rt_version_parse(s);

    rt_string c1 = make_str("^1.2.0");
    assert(rt_version_satisfies(v, c1) == 1);
    rt_string_unref(c1);

    rt_string c2 = make_str("^2.0.0");
    assert(rt_version_satisfies(v, c2) == 0);
    rt_string_unref(c2);

    rt_string_unref(s);
}

static void test_satisfies_tilde() {
    rt_string s = make_str("1.2.9");
    void *v = rt_version_parse(s);

    rt_string c1 = make_str("~1.2.0");
    assert(rt_version_satisfies(v, c1) == 1);
    rt_string_unref(c1);

    rt_string c2 = make_str("~1.3.0");
    assert(rt_version_satisfies(v, c2) == 0);
    rt_string_unref(c2);

    rt_string_unref(s);
}

static void test_satisfies_trims_trailing_whitespace() {
    rt_string s = make_str("1.2.9");
    void *v = rt_version_parse(s);

    rt_string c1 = make_str("  >= 1.2.3 \t");
    assert(rt_version_satisfies(v, c1) == 1);
    rt_string_unref(c1);

    rt_string c2 = make_str("~ 1.2.0\n");
    assert(rt_version_satisfies(v, c2) == 1);
    rt_string_unref(c2);

    rt_string_unref(s);
}

static void test_satisfies_embedded_nul_not_truncated() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    const char constraint_bytes[] = {'>', '=', '1', '.', '0', '.', '0', '\0'};
    rt_string c = rt_string_from_bytes(constraint_bytes, sizeof(constraint_bytes));
    assert(rt_version_satisfies(v, c) == 0);
    rt_string_unref(c);
    rt_string_unref(s);
}

// ---------------------------------------------------------------------------
// Bump tests
// ---------------------------------------------------------------------------

static void test_bump_major() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    rt_string r = rt_version_bump_major(v);
    assert(str_eq(r, "2.0.0"));
    rt_string_unref(r);
    rt_string_unref(s);
}

static void test_bump_minor() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    rt_string r = rt_version_bump_minor(v);
    assert(str_eq(r, "1.3.0"));
    rt_string_unref(r);
    rt_string_unref(s);
}

static void test_bump_patch() {
    rt_string s = make_str("1.2.3");
    void *v = rt_version_parse(s);
    rt_string r = rt_version_bump_patch(v);
    assert(str_eq(r, "1.2.4"));
    rt_string_unref(r);
    rt_string_unref(s);
}

static void test_null_safety() {
    assert(rt_version_parse(NULL) == NULL);
    assert(rt_version_major(NULL) == 0);
    assert(rt_version_minor(NULL) == 0);
    assert(rt_version_patch(NULL) == 0);
    assert(rt_version_cmp(NULL, NULL) == 0);
    assert(rt_version_satisfies(NULL, NULL) == 0);
}

/// @brief Main.
int main() {
    // Parse
    test_parse_basic();
    test_parse_with_v_prefix();
    test_parse_prerelease();
    test_parse_build();
    test_parse_full();
    test_parse_rejects_missing_patch();
    test_parse_rejects_invalid_semver_identifiers();
    test_parse_rejects_numeric_overflow();
    test_is_valid();
    test_parse_allocation_failures_stop_cleanly();

    // ToString
    test_to_string();
    test_to_string_long_metadata();

    // Compare
    test_cmp_equal();
    test_cmp_major();
    test_cmp_prerelease();
    test_cmp_prerelease_order();

    // Constraints
    test_satisfies_gte();
    test_satisfies_caret();
    test_satisfies_tilde();
    test_satisfies_trims_trailing_whitespace();
    test_satisfies_embedded_nul_not_truncated();

    // Bump
    test_bump_major();
    test_bump_minor();
    test_bump_patch();

    // Null safety
    test_null_safety();

    return 0;
}
