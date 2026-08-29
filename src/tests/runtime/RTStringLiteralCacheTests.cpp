//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTStringLiteralCacheTests.cpp
// Purpose: Regression tests for the `rt_str_from_lit` literal cache: one
//          immortal string per literal address, stable across evaluations,
//          unaffected by release, distinct per site, and never mutated by
//          concat's in-place fast path.
// Key invariants:
//   - The same (address, length) always yields the same registered handle.
//   - Release on the result is a no-op; the handle stays valid.
//   - A different literal address yields a different handle.
// Ownership/Lifetime:
//   - Cached literal strings are immortal and owned by the runtime.
//   - Test-created mortal strings are released before exit.
// Links: src/runtime/core/rt_string_ops.c
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_string.h"

#include <cassert>
#include <cstdio>
#include <cstring>

static const char kShort[] = "K";
static const char kLong[] = "flies out to the center fielder on a routine play";
static const char kOther[] = "K";

int main() {
    // Same site, repeated evaluation → the same immortal handle.
    rt_string a = rt_str_from_lit(kShort, 1);
    rt_string b = rt_str_from_lit(kShort, 1);
    assert(a != NULL);
    assert(a == b);
    assert(rt_string_is_handle(a) == 1);
    assert(rt_str_len(a) == 1);
    assert(strcmp(rt_string_cstr(a), "K") == 0);

    // Release is a no-op on the cached literal; it stays valid.
    assert(rt_string_unref_count(a) == SIZE_MAX);
    rt_string_unref(a);
    rt_string_unref(a);
    assert(rt_string_is_handle(a) == 1);
    assert(rt_str_from_lit(kShort, 1) == a);

    // Retain is a no-op too.
    assert(rt_string_ref(a) == a);

    // A heap-backed (non-SSO) literal behaves identically.
    const size_t longLen = sizeof(kLong) - 1;
    rt_string l1 = rt_str_from_lit(kLong, longLen);
    rt_string l2 = rt_str_from_lit(kLong, longLen);
    assert(l1 != NULL && l1 == l2);
    assert(rt_str_len(l1) == (int64_t)longLen);
    assert(memcmp(rt_string_cstr(l1), kLong, longLen) == 0);
    rt_string_unref(l1);
    assert(rt_string_is_handle(l1) == 1);

    // Distinct sites with identical text are distinct handles (address-keyed).
    rt_string c = rt_str_from_lit(kOther, 1);
    assert(c != a);
    assert(rt_str_eq(a, c) != 0);

    // Concat never mutates the cached literal in place.
    rt_string suffix = rt_string_from_bytes("!", 1);
    rt_string joined = rt_str_concat(rt_string_ref(a), suffix);
    assert(joined != a);
    assert(strcmp(rt_string_cstr(joined), "K!") == 0);
    assert(strcmp(rt_string_cstr(a), "K") == 0);
    rt_string_unref(joined);

    // Empty literals share the empty singleton.
    assert(rt_str_from_lit("", 0) == rt_str_empty());
    assert(rt_str_from_lit(NULL, 0) == rt_str_empty());

    // Many distinct sites: the table grows without losing earlier entries.
    static char sites[600][4];
    rt_string first[600];
    for (int i = 0; i < 600; ++i) {
        snprintf(sites[i], sizeof(sites[i]), "%03d", i);
        first[i] = rt_str_from_lit(sites[i], 3);
        assert(first[i] != NULL);
    }
    for (int i = 0; i < 600; ++i) {
        assert(rt_str_from_lit(sites[i], 3) == first[i]);
        assert(memcmp(rt_string_cstr(first[i]), sites[i], 3) == 0);
    }

    printf("RTStringLiteralCacheTests: ok\n");
    return 0;
}
