//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Purpose: Tests for Zanna.Text.CompiledPattern compilation, diagnostic
//          construction, ranges, captures, replacement, and splitting.
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_compiled_pattern.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>

static void test_result(bool cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        assert(false);
    }
}

static bool bytes_eq(rt_string s, const char *expected, size_t len) {
    return rt_str_len(s) == (int64_t)len && memcmp(rt_string_cstr(s), expected, len) == 0;
}

//=============================================================================
// Basic Matching Tests
//=============================================================================

static void test_basic_match() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("hello"));

    test_result(rt_compiled_pattern_is_match(pattern, rt_const_cstr("hello world")),
                "basic_match: should match");

    test_result(!rt_compiled_pattern_is_match(pattern, rt_const_cstr("goodbye world")),
                "basic_match: should not match");

    test_result(rt_compiled_pattern_is_match(pattern, rt_const_cstr("say hello")),
                "basic_match: should match anywhere");
}

static void test_regex_match() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    test_result(rt_compiled_pattern_is_match(pattern, rt_const_cstr("abc123def")),
                "regex_match: should match digits");

    test_result(!rt_compiled_pattern_is_match(pattern, rt_const_cstr("abc")),
                "regex_match: should not match without digits");
}

static void test_get_pattern() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("test\\d+"));
    rt_string pat = rt_compiled_pattern_get_pattern(pattern);
    test_result(strcmp(rt_string_cstr(pat), "test\\d+") == 0,
                "get_pattern: should return original pattern");
}

//=============================================================================
// Find Tests
//=============================================================================

static void test_find() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    rt_string result = rt_compiled_pattern_find(pattern, rt_const_cstr("abc123def456"));
    test_result(strcmp(rt_string_cstr(result), "123") == 0, "find: should find first match");

    result = rt_compiled_pattern_find(pattern, rt_const_cstr("no digits here"));
    test_result(strlen(rt_string_cstr(result)) == 0, "find: should return empty on no match");
}

static void test_find_from() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    rt_string result = rt_compiled_pattern_find_from(pattern, rt_const_cstr("abc123def456"), 6);
    test_result(strcmp(rt_string_cstr(result), "456") == 0, "find_from: should find from position");

    void *dot = rt_compiled_pattern_new(rt_const_cstr("."));
    const char embedded_bytes[] = {'a', '\0', 'b'};
    rt_string embedded = rt_string_from_bytes(embedded_bytes, sizeof(embedded_bytes));
    result = rt_compiled_pattern_find_from(dot, embedded, 1);
    const char nul_only[] = {'\0'};
    test_result(bytes_eq(result, nul_only, sizeof(nul_only)),
                "find_from: should match embedded NUL byte");
}

static void test_find_pos() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("world"));

    int64_t pos = rt_compiled_pattern_find_pos(pattern, rt_const_cstr("hello world"));
    test_result(pos == 6, "find_pos: should return correct position");

    pos = rt_compiled_pattern_find_pos(pattern, rt_const_cstr("hello"));
    test_result(pos == -1, "find_pos: should return -1 on no match");
}

static void test_find_all() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    void *results = rt_compiled_pattern_find_all(pattern, rt_const_cstr("a1b22c333d"));
    test_result(rt_seq_len(results) == 3, "find_all: should find all matches");

    rt_string first = (rt_string)rt_seq_get(results, 0);
    rt_string second = (rt_string)rt_seq_get(results, 1);
    rt_string third = (rt_string)rt_seq_get(results, 2);

    test_result(strcmp(rt_string_cstr(first), "1") == 0, "find_all: first match");
    test_result(strcmp(rt_string_cstr(second), "22") == 0, "find_all: second match");
    test_result(strcmp(rt_string_cstr(third), "333") == 0, "find_all: third match");
}

//=============================================================================
// Capture Group Tests
//=============================================================================

static void test_captures_basic() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("(\\d+)-(\\d+)"));

    void *groups = rt_compiled_pattern_captures(pattern, rt_const_cstr("test 123-456 end"));
    test_result(rt_seq_len(groups) == 3, "captures: should have 3 groups (full + 2 captures)");

    rt_string full = (rt_string)rt_seq_get(groups, 0);
    rt_string g1 = (rt_string)rt_seq_get(groups, 1);
    rt_string g2 = (rt_string)rt_seq_get(groups, 2);

    test_result(strcmp(rt_string_cstr(full), "123-456") == 0, "captures: full match");
    test_result(strcmp(rt_string_cstr(g1), "123") == 0, "captures: group 1");
    test_result(strcmp(rt_string_cstr(g2), "456") == 0, "captures: group 2");
}

static void test_captures_no_match() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("(\\d+)"));

    void *groups = rt_compiled_pattern_captures(pattern, rt_const_cstr("no digits"));
    test_result(rt_seq_len(groups) == 0, "captures: should be empty on no match");
}

static void test_captures_nested() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("((\\w+)@(\\w+))"));

    void *groups = rt_compiled_pattern_captures(pattern, rt_const_cstr("email: user@host.com"));
    // Groups: 0=full, 1=outer group, 2=user, 3=host
    test_result(rt_seq_len(groups) >= 3, "captures_nested: should have multiple groups");
}

//=============================================================================
// Replace Tests
//=============================================================================

static void test_replace() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    rt_string result =
        rt_compiled_pattern_replace(pattern, rt_const_cstr("a1b2c3"), rt_const_cstr("X"));
    test_result(strcmp(rt_string_cstr(result), "aXbXcX") == 0,
                "replace: should replace all matches");
}

static void test_replace_first() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    rt_string result =
        rt_compiled_pattern_replace_first(pattern, rt_const_cstr("a1b2c3"), rt_const_cstr("X"));
    test_result(strcmp(rt_string_cstr(result), "aXb2c3") == 0,
                "replace_first: should replace only first match");
}

static void test_replace_no_match() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\d+"));

    rt_string result =
        rt_compiled_pattern_replace(pattern, rt_const_cstr("no digits"), rt_const_cstr("X"));
    test_result(strcmp(rt_string_cstr(result), "no digits") == 0,
                "replace_no_match: should return original on no match");
}

static void test_null_text_and_replacement() {
    void *digits = rt_compiled_pattern_new(rt_const_cstr("\\d+"));
    rt_string result = rt_compiled_pattern_find(digits, NULL);
    test_result(strcmp(rt_string_cstr(result), "") == 0, "null_text: find returns empty");

    result = rt_compiled_pattern_replace(digits, rt_const_cstr("a1b2"), NULL);
    test_result(strcmp(rt_string_cstr(result), "ab") == 0,
                "null_replacement: replace deletes matches");

    result = rt_compiled_pattern_replace_first(digits, rt_const_cstr("a1b2"), NULL);
    test_result(strcmp(rt_string_cstr(result), "ab2") == 0,
                "null_replacement: replace_first deletes first match");
}

//=============================================================================
// Split Tests
//=============================================================================

static void test_split() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr(","));

    void *parts = rt_compiled_pattern_split(pattern, rt_const_cstr("a,b,c"));
    test_result(rt_seq_len(parts) == 3, "split: should split into 3 parts");

    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 0)), "a") == 0, "split: part 0");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 1)), "b") == 0, "split: part 1");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 2)), "c") == 0, "split: part 2");
}

static void test_split_regex() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("\\s+"));

    void *parts = rt_compiled_pattern_split(pattern, rt_const_cstr("one   two\tthree"));
    test_result(rt_seq_len(parts) == 3, "split_regex: should split by whitespace");
}

static void test_split_limit() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr(","));

    void *parts = rt_compiled_pattern_split_n(pattern, rt_const_cstr("a,b,c,d,e"), 3);
    test_result(rt_seq_len(parts) == 3, "split_limit: should split into 3 parts max");

    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 0)), "a") == 0,
                "split_limit: part 0");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 1)), "b") == 0,
                "split_limit: part 1");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 2)), "c,d,e") == 0,
                "split_limit: part 2 (rest)");
}

//=============================================================================
// Edge Cases
//=============================================================================

static void test_empty_pattern() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr(""));

    // Empty pattern matches at every position
    test_result(rt_compiled_pattern_is_match(pattern, rt_const_cstr("test")),
                "empty_pattern: should match");
}

static void test_empty_text() {
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("a"));

    test_result(!rt_compiled_pattern_is_match(pattern, rt_const_cstr("")),
                "empty_text: should not match non-empty pattern");

    void *empty_pat = rt_compiled_pattern_new(rt_const_cstr(""));
    test_result(rt_compiled_pattern_is_match(empty_pat, rt_const_cstr("")),
                "empty_text: empty pattern should match");
}

static void test_anchors() {
    void *start_pattern = rt_compiled_pattern_new(rt_const_cstr("^hello"));
    void *end_pattern = rt_compiled_pattern_new(rt_const_cstr("world$"));

    test_result(rt_compiled_pattern_is_match(start_pattern, rt_const_cstr("hello world")),
                "anchors: ^ should match at start");
    test_result(!rt_compiled_pattern_is_match(start_pattern, rt_const_cstr("say hello")),
                "anchors: ^ should not match in middle");

    test_result(rt_compiled_pattern_is_match(end_pattern, rt_const_cstr("hello world")),
                "anchors: $ should match at end");
    test_result(!rt_compiled_pattern_is_match(end_pattern, rt_const_cstr("world hello")),
                "anchors: $ should not match in middle");
}

static void test_character_classes() {
    void *digit = rt_compiled_pattern_new(rt_const_cstr("[0-9]+"));
    void *word = rt_compiled_pattern_new(rt_const_cstr("[a-zA-Z]+"));
    void *not_digit = rt_compiled_pattern_new(rt_const_cstr("[^0-9]+"));

    test_result(rt_compiled_pattern_is_match(digit, rt_const_cstr("abc123")),
                "char_class: [0-9] should match digits");
    test_result(rt_compiled_pattern_is_match(word, rt_const_cstr("Hello123")),
                "char_class: [a-zA-Z] should match letters");
    test_result(rt_compiled_pattern_is_match(not_digit, rt_const_cstr("abc")),
                "char_class: [^0-9] should match non-digits");
}

static void test_quantifiers() {
    void *star = rt_compiled_pattern_new(rt_const_cstr("ab*c"));
    void *plus = rt_compiled_pattern_new(rt_const_cstr("ab+c"));
    void *quest = rt_compiled_pattern_new(rt_const_cstr("ab?c"));

    // a*
    test_result(rt_compiled_pattern_is_match(star, rt_const_cstr("ac")),
                "quantifiers: * matches zero");
    test_result(rt_compiled_pattern_is_match(star, rt_const_cstr("abc")),
                "quantifiers: * matches one");
    test_result(rt_compiled_pattern_is_match(star, rt_const_cstr("abbbc")),
                "quantifiers: * matches many");

    // a+
    test_result(!rt_compiled_pattern_is_match(plus, rt_const_cstr("ac")),
                "quantifiers: + requires one");
    test_result(rt_compiled_pattern_is_match(plus, rt_const_cstr("abc")),
                "quantifiers: + matches one");
    test_result(rt_compiled_pattern_is_match(plus, rt_const_cstr("abbbc")),
                "quantifiers: + matches many");

    // a?
    test_result(rt_compiled_pattern_is_match(quest, rt_const_cstr("ac")),
                "quantifiers: ? matches zero");
    test_result(rt_compiled_pattern_is_match(quest, rt_const_cstr("abc")),
                "quantifiers: ? matches one");
}

//=============================================================================
// Main
//=============================================================================

static void test_zero_width_replace_split() {
    // VDOC-054: zero-width matches must not drop source bytes.
    void *pattern = rt_compiled_pattern_new(rt_const_cstr(""));
    rt_string out = rt_compiled_pattern_replace(pattern, rt_const_cstr("abc"), rt_const_cstr("-"));
    test_result(strcmp(rt_string_cstr(out), "-a-b-c-") == 0,
                "replace: empty pattern keeps source bytes");

    void *parts = rt_compiled_pattern_split(pattern, rt_const_cstr("abc"));
    test_result(rt_seq_len(parts) == 3, "split: empty pattern piece count");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 0)), "a") == 0 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 1)), "b") == 0 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 2)), "c") == 0,
                "split: empty pattern pieces");

    void *comma = rt_compiled_pattern_new(rt_const_cstr(","));
    parts = rt_compiled_pattern_split_n(comma, rt_const_cstr("a,b,c"), 2);
    test_result(rt_seq_len(parts) == 2 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(parts, 1)), "b,c") == 0,
                "split_n: limit keeps remainder intact");
}

static void test_captures_backtracking_and_numbering() {
    // VDOC-057: Captures uses the same match language as Find, and group
    // indexes follow lexical (opening-parenthesis) numbering.
    void *pattern = rt_compiled_pattern_new(rt_const_cstr("a*(a)"));
    void *caps = rt_compiled_pattern_captures(pattern, rt_const_cstr("aaa"));
    test_result(rt_seq_len(caps) == 2, "captures: a*(a) succeeds like Find");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 0)), "aaa") == 0,
                "captures: a*(a) full match is 'aaa'");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 1)), "a") == 0,
                "captures: a*(a) group 1 is 'a'");

    // Alternation keeps lexical slots: the untaken branch reports empty.
    pattern = rt_compiled_pattern_new(rt_const_cstr("(a)|(b)"));
    caps = rt_compiled_pattern_captures(pattern, rt_const_cstr("b"));
    test_result(rt_seq_len(caps) == 3, "captures: (a)|(b) reports both slots");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 1)), "") == 0,
                "captures: untaken branch group is empty");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 2)), "b") == 0,
                "captures: taken branch keeps lexical index");

    // Nested groups number by opening parenthesis.
    pattern = rt_compiled_pattern_new(rt_const_cstr("((a)b)"));
    caps = rt_compiled_pattern_captures(pattern, rt_const_cstr("ab"));
    test_result(rt_seq_len(caps) == 3, "captures: nested group count");
    test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 1)), "ab") == 0 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 2)), "a") == 0,
                "captures: outer group first, inner second");

    // VDOC-058: more than 32 groups is fully supported (no OOB, no cap).
    {
        char pat[34 * 3 + 1];
        char txt[34];
        int n = 0;
        for (int i = 0; i < 33; i++) {
            pat[n++] = '(';
            pat[n++] = (char)('a' + (i % 26));
            pat[n++] = ')';
            txt[i] = (char)('a' + (i % 26));
        }
        pat[n] = '\0';
        txt[33] = '\0';
        void *many = rt_compiled_pattern_new(rt_string_from_bytes(pat, (size_t)n));
        void *mc = rt_compiled_pattern_captures(many, rt_string_from_bytes(txt, 33));
        test_result(rt_seq_len(mc) == 34, "captures: 33 groups all reported");
        test_result(strcmp(rt_string_cstr((rt_string)rt_seq_get(mc, 33)), "g") == 0,
                    "captures: 33rd group correct");
    }

    // Quantified group captures the last repetition.
    pattern = rt_compiled_pattern_new(rt_const_cstr("(ab)+"));
    caps = rt_compiled_pattern_captures(pattern, rt_const_cstr("abab"));
    test_result(rt_seq_len(caps) == 2 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 0)), "abab") == 0 &&
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(caps, 1)), "ab") == 0,
                "captures: quantified group keeps last repetition");
}

static void test_nul_pattern_rejected() {
    // VDOC-053: CompiledPattern construction must reject embedded NUL so the
    // Pattern property can always round-trip the constructor input.
    const char pat_bytes[] = {'a', '\0', 'b'};
    rt_string pat = rt_string_from_bytes(pat_bytes, sizeof(pat_bytes));

    jmp_buf env;
    rt_trap_set_recovery(&env);
    bool trapped = true;
    if (setjmp(env) == 0) {
        (void)rt_compiled_pattern_new(pat);
        trapped = false;
    }
    rt_trap_clear_recovery();
    test_result(trapped, "New traps on NUL-containing pattern");
}

static int64_t range_value(void *range, int64_t index) {
    return rt_unbox_i64(rt_seq_get(range, index));
}

static void test_diagnostic_ranges_and_expansion() {
    void *bad = rt_compiled_pattern_try_new(rt_const_cstr("[abc"), 0);
    test_result(bad && rt_result_is_err(bad), "TryNew: malformed syntax returns Err");
    test_result(strstr(rt_string_cstr(rt_result_unwrap_err_str(bad)), "unclosed") != nullptr,
                "TryNew: malformed syntax retains diagnostic");

    void *folded_result = rt_compiled_pattern_try_new(rt_const_cstr("[a-c]+z"), 1);
    test_result(folded_result && rt_result_is_ok(folded_result),
                "TryNew: valid case-insensitive pattern returns Ok");
    void *folded = rt_result_unwrap(folded_result);
    test_result(rt_compiled_pattern_is_match(folded, rt_const_cstr("ABCZ")),
                "TryNew: case option reaches literal and class matching");

    void *word_result = rt_compiled_pattern_try_new(rt_const_cstr("t"), 0);
    void *word = rt_result_unwrap(word_result);
    rt_string unicode = rt_const_cstr("été t—t");
    void *range = rt_compiled_pattern_find_range_from(word, unicode, 0, 1);
    test_result(rt_seq_len(range) == 3 && range_value(range, 0) == 6 &&
                    range_value(range, 1) == 7 && range_value(range, 2) == 7,
                "FindRangeFrom: Unicode letters block an interior word match");
    range = rt_compiled_pattern_find_range_from(word, unicode, 7, 1);
    test_result(rt_seq_len(range) == 3 && range_value(range, 0) == 10 &&
                    range_value(range, 1) == 11,
                "FindRangeFrom: Unicode punctuation forms a word boundary");

    void *anchors = rt_compiled_pattern_new(rt_const_cstr("^|$"));
    range = rt_compiled_pattern_find_range_from(anchors, rt_const_cstr("x"), 0, 0);
    test_result(rt_seq_len(range) == 3 && range_value(range, 0) == 0 &&
                    range_value(range, 1) == 0 && range_value(range, 2) == 1,
                "FindRangeFrom: zero-width start anchor supplies UTF-8 resume");
    range = rt_compiled_pattern_find_range_from(anchors, rt_const_cstr("x"), 1, 0);
    test_result(rt_seq_len(range) == 3 && range_value(range, 0) == 1 &&
                    range_value(range, 1) == 1 && range_value(range, 2) == 2,
                "FindRangeFrom: zero-width end anchor supplies terminal resume");

    void *captures = rt_compiled_pattern_new(rt_const_cstr("(item)-(\\d+)"));
    void *expanded = rt_compiled_pattern_expand_replacement_at(
        captures, rt_const_cstr("item-42"), 0, rt_const_cstr("$2:$1:$$:$&"));
    test_result(expanded && rt_result_is_ok(expanded),
                "ExpandReplacementAt: exact capture match succeeds");
    test_result(strcmp(rt_string_cstr(rt_result_unwrap_str(expanded)), "42:item:$:item-42") == 0,
                "ExpandReplacementAt: capture syntax expands deterministically");
    expanded = rt_compiled_pattern_expand_replacement_at(
        captures, rt_const_cstr("x item-42"), 0, rt_const_cstr("$1"));
    test_result(expanded && rt_result_is_err(expanded),
                "ExpandReplacementAt: later match is not accepted as exact");

    test_result(rt_text_char_is_word(rt_const_cstr("é")),
                "Text.Char.IsWord: Unicode letter is a word scalar");
    test_result(rt_text_char_is_word(rt_const_cstr("́")),
                "Text.Char.IsWord: combining mark is a word scalar");
    test_result(!rt_text_char_is_word(rt_const_cstr("—")),
                "Text.Char.IsWord: Unicode punctuation is a boundary");
}

int main() {
    test_nul_pattern_rejected();
    test_diagnostic_ranges_and_expansion();
    test_zero_width_replace_split();
    test_captures_backtracking_and_numbering();
    // Basic matching
    test_basic_match();
    test_regex_match();
    test_get_pattern();

    // Find operations
    test_find();
    test_find_from();
    test_find_pos();
    test_find_all();

    // Capture groups
    test_captures_basic();
    test_captures_no_match();
    test_captures_nested();

    // Replace operations
    test_replace();
    test_replace_first();
    test_replace_no_match();
    test_null_text_and_replacement();

    // Split operations
    test_split();
    test_split_regex();
    test_split_limit();

    // Edge cases
    test_empty_pattern();
    test_empty_text();
    test_anchors();
    test_character_classes();
    test_quantifiers();

    printf("All CompiledPattern tests passed!\n");
    return 0;
}
