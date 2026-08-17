//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTRegexTests.cpp
// Purpose: Validate rt_regex / rt_pattern_* API (Zanna.Text.Regex) and the
//          shared diagnostic regex engine used by runtime and GUI search.
// Key invariants: Public pattern operations, recoverable compilation, option
//                 parity, zero-width anchors, and capture expansion are sound.
// Ownership/Lifetime: Returned rt_string values are released after each test.
//
//===----------------------------------------------------------------------===//

#include "zanna/runtime/rt.h"

#include "rt_regex.h"
#include "rt_regex_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *label, int ok) {
    printf("  %-50s %s\n", label, ok ? "PASS" : "FAIL");
    assert(ok);
}

static rt_string S(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static int str_eq_c(rt_string s, const char *expected) {
    rt_string exp = S(expected);
    int result = rt_str_eq(s, exp);
    rt_string_unref(exp);
    return result;
}

static void test_is_match(void) {
    printf("rt_pattern_is_match:\n");
    rt_string text = S("hello world");
    rt_string pat_hello = S("hello");
    rt_string pat_digit = S("\\d+");
    rt_string pat_word = S("\\w+");
    rt_string pat_anchor = S("^hello");
    rt_string pat_end = S("world$");

    check("match literal", rt_pattern_is_match(text, pat_hello));
    check("no match digit in alpha", !rt_pattern_is_match(text, pat_digit));
    check("match word char class", rt_pattern_is_match(text, pat_word));
    check("match start anchor", rt_pattern_is_match(text, pat_anchor));
    check("match end anchor", rt_pattern_is_match(text, pat_end));

    rt_string_unref(text);
    rt_string_unref(pat_hello);
    rt_string_unref(pat_digit);
    rt_string_unref(pat_word);
    rt_string_unref(pat_anchor);
    rt_string_unref(pat_end);
}

static void test_find(void) {
    printf("rt_pattern_find:\n");
    rt_string text = S("foo123bar456");
    rt_string pat = S("\\d+");

    rt_string found = rt_pattern_find(text, pat);
    check("find first digits", str_eq_c(found, "123"));
    rt_string_unref(found);

    rt_string pat_none = S("xyz");
    rt_string not_found = rt_pattern_find(text, pat_none);
    check("find returns empty on no match", rt_str_len(not_found) == 0);
    rt_string_unref(not_found);

    // Position 6 is past "foo123" (positions 0-5), so scan starts at "bar456"
    rt_string found2 = rt_pattern_find_from(text, pat, 6);
    check("find_from skips first match", str_eq_c(found2, "456"));
    rt_string_unref(found2);

    int64_t pos = rt_pattern_find_pos(text, pat);
    check("find_pos returns correct offset", pos == 3);

    int64_t no_pos = rt_pattern_find_pos(text, pat_none);
    check("find_pos returns -1 on no match", no_pos == -1);

    rt_string_unref(text);
    rt_string_unref(pat);
    rt_string_unref(pat_none);
}

static void test_replace(void) {
    printf("rt_pattern_replace:\n");
    rt_string text = S("aabbcc");
    rt_string pat = S("[ab]");
    rt_string repl = S("X");

    rt_string result = rt_pattern_replace(text, pat, repl);
    check("replace all matches", str_eq_c(result, "XXXXcc"));
    rt_string_unref(result);

    rt_string result2 = rt_pattern_replace_first(text, pat, repl);
    check("replace_first replaces only first", str_eq_c(result2, "Xabbcc"));
    rt_string_unref(result2);

    rt_string_unref(text);
    rt_string_unref(pat);
    rt_string_unref(repl);
}

static void test_find_all(void) {
    printf("rt_pattern_find_all:\n");
    rt_string text = S("one two three");
    rt_string pat = S("\\w+");

    void *seq = rt_pattern_find_all(text, pat);
    check("find_all returns seq", seq != NULL);
    check("find_all matches 3 words", rt_seq_len(seq) == 3);

    rt_string first = (rt_string)rt_seq_get(seq, 0);
    check("first match is 'one'", str_eq_c(first, "one"));

    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);

    rt_string_unref(text);
    rt_string_unref(pat);
}

static void test_split(void) {
    printf("rt_pattern_split:\n");
    rt_string text = S("a,b,,c");
    rt_string pat = S(",");

    void *parts = rt_pattern_split(text, pat);
    check("split returns seq", parts != NULL);
    check("split produces 4 parts", rt_seq_len(parts) == 4);

    if (rt_obj_release_check0(parts))
        rt_obj_free(parts);

    rt_string_unref(text);
    rt_string_unref(pat);
}

static void test_escape(void) {
    printf("rt_pattern_escape:\n");
    rt_string special = S("a.b*c?d");
    rt_string escaped = rt_pattern_escape(special);

    // After escaping, the result should be usable as a literal pattern
    rt_string text = S("a.b*c?d");
    check("escaped pattern matches literal text", rt_pattern_is_match(text, escaped));

    // Escaped pattern must NOT match "axbxcxd" (dot/star/question would if unescaped)
    rt_string variant = S("axbxcxd");
    check("escaped pattern does not match changed text", !rt_pattern_is_match(variant, escaped));

    rt_string_unref(special);
    rt_string_unref(escaped);
    rt_string_unref(text);
    rt_string_unref(variant);
}

static int diagnostic_failure_count = 0;

static void count_engine_failure(const char *message) {
    (void)message;
    diagnostic_failure_count++;
}

static void test_shared_engine(void) {
    printf("shared diagnostic engine:\n");
    diagnostic_failure_count = 0;
    re_set_failure_handler(count_engine_failure);

    char error[128];
    re_compiled_pattern *invalid =
        re_compile_diagnostic("[abc", RE_COMPILE_DEFAULT, error, sizeof(error));
    check("malformed pattern is recoverable", invalid == NULL);
    check("malformed pattern has a useful diagnostic", strstr(error, "unclosed") != NULL);
    check("syntax diagnostic does not invoke fatal callback", diagnostic_failure_count == 0);

    re_compiled_pattern *folded =
        re_compile_diagnostic("[a-c]+z", RE_COMPILE_CASE_INSENSITIVE, error, sizeof(error));
    check("case-insensitive pattern compiles", folded != NULL);
    int match_start = -1;
    int match_end = -1;
    check("case-insensitive literal and class share options",
          re_find_match(folded, "xxABCZ", 6, 0, &match_start, &match_end) && match_start == 2 &&
              match_end == 6);
    re_free(folded);

    re_compiled_pattern *anchors =
        re_compile_diagnostic("^|$", RE_COMPILE_DEFAULT, error, sizeof(error));
    check("zero-width anchor pattern compiles", anchors != NULL);
    check("start anchor returns its zero-width span",
          re_find_match(anchors, "x", 1, 0, &match_start, &match_end) && match_start == 0 &&
              match_end == 0);
    check("end anchor remains discoverable after explicit progress",
          re_find_match(anchors, "x", 1, 1, &match_start, &match_end) && match_start == 1 &&
              match_end == 1);
    re_free(anchors);

    re_compiled_pattern *captures =
        re_compile_diagnostic("(a+)(b)", RE_COMPILE_DEFAULT, error, sizeof(error));
    check("capture pattern compiles", captures != NULL);
    char *expanded = NULL;
    size_t expanded_len = 0;
    const char *replacement = "$2-$1-$$-$&-${1}-$9";
    check("capture replacement expands exact match",
          re_expand_replacement(
              captures, "aaab", 4, 0, replacement, strlen(replacement), &expanded, &expanded_len));
    check("capture replacement syntax is deterministic",
          expanded && expanded_len == strlen("b-aaa-$-aaab-aaa-$9") &&
              strcmp(expanded, "b-aaa-$-aaab-aaa-$9") == 0);
    free(expanded);
    re_free(captures);

    re_set_failure_handler(NULL);
}

int main(void) {
    printf("=== RTRegexTests ===\n");
    test_is_match();
    test_find();
    test_replace();
    test_find_all();
    test_split();
    test_escape();
    test_shared_engine();
    printf("All regex tests passed.\n");
    return 0;
}
