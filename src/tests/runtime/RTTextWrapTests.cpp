//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTTextWrapTests.cpp
// Purpose: Validate TextWrapper layout behavior, handle checks, UTF-8
//          boundaries, and recoverable size-overflow handling.
// Key invariants:
//   - Layout operations never split a UTF-8 continuation sequence.
//   - Invalid handles and unrepresentable output sizes trap before data access.
//   - A returning trap cannot turn failed size arithmetic into malloc(0)
//     followed by an out-of-bounds write.
// Ownership/Lifetime:
//   - Registered stack-backed forged handles are unregistered before return.
//   - Results created by the overflow regression are released by the test.
// Links: src/runtime/text/rt_textwrap.c, src/runtime/text/rt_textwrap.h,
//        src/runtime/core/rt_string_ops.c
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_textwrap.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
#include "rt_string_internal.h"
}

static bool g_return_traps = false;
static const char *g_last_returning_trap = nullptr;

extern "C" void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_last_returning_trap = msg;
        return;
    }
    rt_abort(msg);
}

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

static rt_string invalid_string_handle() {
    return (rt_string)(uintptr_t)1;
}

static void expect_trap(void (*fn)(), const char *snippet) {
    jmp_buf env;
    rt_trap_set_recovery(&env);
    if (setjmp(env) == 0) {
        fn();
        rt_trap_clear_recovery();
        assert(false && "expected trap");
    }

    const char *message = rt_trap_get_error();
    test_result("Trap message matches", message != nullptr && strstr(message, snippet) != nullptr);
    rt_trap_clear_recovery();
}

//=============================================================================
// TextWrapper Tests
//=============================================================================

static void test_wrap() {
    printf("Testing TextWrapper Wrap:\n");

    // Test 1: Short text (no wrapping needed)
    {
        rt_string text = rt_const_cstr("Hello");
        rt_string result = rt_textwrap_wrap(text, 20);
        test_result("Short text unchanged", strcmp(rt_string_cstr(result), "Hello") == 0);
    }

    // Test 2: Wrap at word boundary
    {
        rt_string text = rt_const_cstr("Hello world test");
        rt_string result = rt_textwrap_wrap(text, 12);
        test_result("Wrapped at word", strstr(rt_string_cstr(result), "\n") != NULL);
    }

    // Test 3: Preserve existing newlines
    {
        rt_string text = rt_const_cstr("Line1\nLine2");
        rt_string result = rt_textwrap_wrap(text, 80);
        test_result("Preserves newlines", strcmp(rt_string_cstr(result), "Line1\nLine2") == 0);
    }

    // Test 4: WrapLines returns individual strings
    {
        void *lines = rt_textwrap_wrap_lines(rt_const_cstr("one two three"), 7);
        test_result("WrapLines count", rt_seq_len(lines) == 2);
        test_result("WrapLines first line",
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(lines, 0)), "one two") == 0);
    }

    // Test 5: Null input is treated as empty text
    {
        rt_string result = rt_textwrap_wrap(NULL, 20);
        test_result("Null wraps to empty", strcmp(rt_string_cstr(result), "") == 0);
        void *lines = rt_textwrap_wrap_lines(NULL, 20);
        test_result("Null WrapLines has one empty line", rt_seq_len(lines) == 1);
        test_result("Null WrapLines item empty",
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(lines, 0)), "") == 0);
    }

    // Test 6: Width zero or negative disables wrapping
    {
        rt_string text = rt_const_cstr("one two three");
        rt_string zero = rt_textwrap_wrap(text, 0);
        test_result("Width zero leaves text unchanged",
                    strcmp(rt_string_cstr(zero), "one two three") == 0);

        rt_string negative = rt_textwrap_wrap(text, -4);
        test_result("Negative width leaves text unchanged",
                    strcmp(rt_string_cstr(negative), "one two three") == 0);

        void *lines = rt_textwrap_wrap_lines(rt_const_cstr("one two\nthree"), 0);
        test_result("Disabled WrapLines preserves line count", rt_seq_len(lines) == 2);
        test_result("Disabled WrapLines first line",
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(lines, 0)), "one two") == 0);
        test_result("Disabled WrapLines second line",
                    strcmp(rt_string_cstr((rt_string)rt_seq_get(lines, 1)), "three") == 0);
    }

    printf("\n");
}

static void test_indent() {
    printf("Testing TextWrapper Indent:\n");

    // Test 1: Indent single line
    {
        rt_string text = rt_const_cstr("Hello");
        rt_string result = rt_textwrap_indent(text, rt_const_cstr("  "));
        test_result("Indent single line", strcmp(rt_string_cstr(result), "  Hello") == 0);
    }

    // Test 2: Indent multiple lines
    {
        rt_string text = rt_const_cstr("Line1\nLine2");
        rt_string result = rt_textwrap_indent(text, rt_const_cstr("> "));
        test_result("Indent multiple lines",
                    strcmp(rt_string_cstr(result), "> Line1\n> Line2") == 0);
    }

    // Test 3: NULL text and prefix are treated as empty strings
    {
        rt_string text = rt_const_cstr("Line");
        rt_string result = rt_textwrap_indent(NULL, rt_const_cstr("> "));
        test_result("Indent NULL text returns empty", strcmp(rt_string_cstr(result), "") == 0);

        result = rt_textwrap_indent(text, NULL);
        test_result("Indent NULL prefix leaves text unchanged",
                    strcmp(rt_string_cstr(result), "Line") == 0);
    }

    // Test 4: Empty input stays empty instead of becoming a bare prefix
    {
        rt_string empty = rt_const_cstr("");
        rt_string result = rt_textwrap_indent(empty, rt_const_cstr("> "));
        test_result("Indent empty text returns empty", strcmp(rt_string_cstr(result), "") == 0);
    }

    printf("\n");
}

static void test_dedent() {
    printf("Testing TextWrapper Dedent:\n");

    // Test 1: Remove common indent
    {
        rt_string text = rt_const_cstr("    Line1\n    Line2");
        rt_string result = rt_textwrap_dedent(text);
        test_result("Removes common indent", strcmp(rt_string_cstr(result), "Line1\nLine2") == 0);
    }

    // Test 2: Mixed indent (uses minimum)
    {
        rt_string text = rt_const_cstr("  Line1\n    Line2");
        rt_string result = rt_textwrap_dedent(text);
        test_result("Uses minimum indent", strncmp(rt_string_cstr(result), "Line1\n", 6) == 0);
    }

    // Test 3: Tabs and spaces are not treated as interchangeable bytes
    {
        rt_string text = rt_const_cstr("\tLine1\n  Line2");
        rt_string result = rt_textwrap_dedent(text);
        test_result("Does not over-remove partial tab indent",
                    strcmp(rt_string_cstr(result), "\tLine1\n  Line2") == 0);
    }

    // Test 4: Blank lines are preserved while common indent is removed
    {
        rt_string text = rt_const_cstr("\n  Line1\n\n  Line2");
        rt_string result = rt_textwrap_dedent(text);
        test_result("Preserves blank lines during dedent",
                    strcmp(rt_string_cstr(result), "\nLine1\n\nLine2") == 0);
    }

    // Test 5: NULL input is safe
    {
        rt_string result = rt_textwrap_dedent(NULL);
        test_result("Dedent NULL returns empty", strcmp(rt_string_cstr(result), "") == 0);
    }

    printf("\n");
}

static void test_truncate() {
    printf("Testing TextWrapper Truncate:\n");

    // Test 1: Truncate with ellipsis
    {
        rt_string text = rt_const_cstr("Hello World");
        rt_string result = rt_textwrap_truncate(text, 8);
        test_result("Truncate with ellipsis", strcmp(rt_string_cstr(result), "Hello...") == 0);
    }

    // Test 2: No truncation needed
    {
        rt_string text = rt_const_cstr("Hello");
        rt_string result = rt_textwrap_truncate(text, 10);
        test_result("No truncation if short", strcmp(rt_string_cstr(result), "Hello") == 0);
    }

    // Test 3: Custom suffix
    {
        rt_string text = rt_const_cstr("Hello World");
        rt_string result = rt_textwrap_truncate_with(text, 9, rt_const_cstr(">>"));
        test_result("Custom suffix", strcmp(rt_string_cstr(result), "Hello W>>") == 0);
    }

    // Test 4: Long suffix is clipped to the requested width
    {
        rt_string text = rt_const_cstr("Hello World");
        rt_string result = rt_textwrap_truncate_with(text, 2, rt_const_cstr("..."));
        test_result("Suffix clipped to width", strcmp(rt_string_cstr(result), "..") == 0);
    }

    // Test 5: NULL text and NULL suffix are safe
    {
        rt_string text = rt_const_cstr("Hello World");
        rt_string result = rt_textwrap_truncate_with(NULL, 5, rt_const_cstr("..."));
        test_result("Truncate NULL text returns empty", strcmp(rt_string_cstr(result), "") == 0);

        result = rt_textwrap_truncate_with(text, 5, NULL);
        test_result("Truncate NULL suffix keeps prefix",
                    strcmp(rt_string_cstr(result), "Hello") == 0);
    }

    printf("\n");
}

static void test_shorten() {
    printf("Testing TextWrapper Shorten:\n");

    // Test 1: Shorten in middle
    {
        rt_string text = rt_const_cstr("Hello World Test");
        rt_string result = rt_textwrap_shorten(text, 11);
        // Should be something like "Hell...Test"
        test_result("Shorten has ellipsis", strstr(rt_string_cstr(result), "...") != NULL);
        test_result("Shorten starts with H", rt_string_cstr(result)[0] == 'H');
    }

    // Test 2: NULL input is safe
    {
        rt_string result = rt_textwrap_shorten(NULL, 8);
        test_result("Shorten NULL returns empty", strcmp(rt_string_cstr(result), "") == 0);
    }

    printf("\n");
}

static void test_alignment() {
    printf("Testing TextWrapper Alignment:\n");

    // Test 1: Left align
    {
        rt_string text = rt_const_cstr("Hi");
        rt_string result = rt_textwrap_left(text, 5);
        test_result("Left align", strcmp(rt_string_cstr(result), "Hi   ") == 0);
    }

    // Test 2: Right align
    {
        rt_string text = rt_const_cstr("Hi");
        rt_string result = rt_textwrap_right(text, 5);
        test_result("Right align", strcmp(rt_string_cstr(result), "   Hi") == 0);
    }

    // Test 3: Center align
    {
        rt_string text = rt_const_cstr("Hi");
        rt_string result = rt_textwrap_center(text, 6);
        test_result("Center align", strcmp(rt_string_cstr(result), "  Hi  ") == 0);
    }

    // Test 4: Odd width center
    {
        rt_string text = rt_const_cstr("Hi");
        rt_string result = rt_textwrap_center(text, 5);
        test_result("Center odd width", strcmp(rt_string_cstr(result), " Hi  ") == 0);
    }

    // Test 5: NULL text pads as an empty string
    {
        rt_string result = rt_textwrap_left(NULL, 3);
        test_result("Left NULL pads to width", strcmp(rt_string_cstr(result), "   ") == 0);

        result = rt_textwrap_right(NULL, 2);
        test_result("Right NULL pads to width", strcmp(rt_string_cstr(result), "  ") == 0);

        result = rt_textwrap_center(NULL, 4);
        test_result("Center NULL pads to width", strcmp(rt_string_cstr(result), "    ") == 0);
    }

    printf("\n");
}

static void trap_wrap_invalid_text() {
    (void)rt_textwrap_wrap(invalid_string_handle(), 10);
}

static void trap_indent_invalid_prefix() {
    (void)rt_textwrap_indent(rt_const_cstr("Line"), invalid_string_handle());
}

static void trap_center_invalid_text() {
    (void)rt_textwrap_center(invalid_string_handle(), 5);
}

static void test_invalid_handles_trap() {
    printf("Testing TextWrapper invalid handles:\n");

    expect_trap(trap_wrap_invalid_text, "TextWrapper.Wrap: invalid string");
    expect_trap(trap_indent_invalid_prefix, "TextWrapper.Indent: invalid prefix");
    expect_trap(trap_center_invalid_text, "TextWrapper.Center: invalid string");

    printf("\n");
}

static void test_returning_trap_stops_size_overflow() {
    printf("Testing TextWrapper recoverable size overflow:\n");

    char prefix_bytes[] = "x";
    rt_string_impl oversized_prefix = {
        RT_STRING_MAGIC,
        prefix_bytes,
        nullptr,
        std::numeric_limits<size_t>::max() > (size_t)std::numeric_limits<int64_t>::max()
            ? (size_t)std::numeric_limits<int64_t>::max()
            : std::numeric_limits<size_t>::max(),
        1,
    };
    rt_string_register_handle(&oversized_prefix);

    rt_string text = rt_const_cstr("a\n");
    g_last_returning_trap = nullptr;
    g_return_traps = true;
    rt_string result = rt_textwrap_indent(text, &oversized_prefix);
    g_return_traps = false;

    test_result("Oversized indent reports a trap", g_last_returning_trap != nullptr);
    test_result("Oversized indent returns a safe empty string",
                result != nullptr && rt_str_len(result) == 0);

    rt_string_unref(result);
    rt_string_unref(text);
    rt_string_unregister_handle(&oversized_prefix);

    printf("\n");
}

static void test_utility() {
    printf("Testing TextWrapper Utility:\n");

    // Test 1: Line count
    {
        rt_string text = rt_const_cstr("Line1\nLine2\nLine3");
        test_result("Line count", rt_textwrap_line_count(text) == 3);
    }

    // Test 2: Single line count
    {
        rt_string text = rt_const_cstr("No newlines");
        test_result("Single line count", rt_textwrap_line_count(text) == 1);
    }

    // Test 3: Trailing newline does not add an empty trailing line
    {
        rt_string text = rt_const_cstr("Line1\nLine2\n");
        test_result("Trailing newline ignored", rt_textwrap_line_count(text) == 2);
    }

    // Test 4: Max line length
    {
        rt_string text = rt_const_cstr("Hi\nHello\nHi");
        test_result("Max line length", rt_textwrap_max_line_len(text) == 5);
    }

    // Test 5: NULL metrics are safe and deterministic
    {
        test_result("NULL line count is one empty line", rt_textwrap_line_count(NULL) == 1);
        test_result("NULL max line length is zero", rt_textwrap_max_line_len(NULL) == 0);
    }

    printf("\n");
}

static void test_hang() {
    printf("Testing TextWrapper Hang:\n");

    // Test: Hanging indent
    {
        rt_string text = rt_const_cstr("First\nSecond\nThird");
        rt_string result = rt_textwrap_hang(text, rt_const_cstr("    "));
        // First line should not have indent
        test_result("Hang first line no indent", strncmp(rt_string_cstr(result), "First", 5) == 0);
        // Subsequent lines should have indent
        test_result("Hang has indented lines",
                    strstr(rt_string_cstr(result), "    Second") != NULL);
    }

    // Test: NULL input and prefix are safe
    {
        rt_string result = rt_textwrap_hang(NULL, rt_const_cstr("    "));
        test_result("Hang NULL text returns empty", strcmp(rt_string_cstr(result), "") == 0);

        rt_string text = rt_const_cstr("First\nSecond");
        result = rt_textwrap_hang(text, NULL);
        test_result("Hang NULL prefix leaves continuation unprefixed",
                    strcmp(rt_string_cstr(result), "First\nSecond") == 0);
    }

    printf("\n");
}

//=============================================================================
// Entry Point
//=============================================================================

static void test_utf8_boundaries_preserved() {
    printf("Testing TextWrapper UTF-8 boundaries (VDOC-046):\n");

    // Force-breaking must not split a multi-byte sequence: "\xC3\xA9" is é.
    {
        rt_string text = rt_const_cstr("\xC3\xA9\xC3\xA9");
        rt_string result = rt_textwrap_wrap(text, 1);
        test_result("Wrap keeps codepoints whole",
                    strcmp(rt_string_cstr(result), "\xC3\xA9\n\xC3\xA9") == 0);
    }

    // Truncation backs the kept prefix up to a codepoint boundary.
    {
        rt_string text = rt_const_cstr("a\xC3\xA9");
        rt_string suffix = rt_const_cstr("");
        rt_string result = rt_textwrap_truncate_with(text, 2, suffix);
        test_result("Truncate ends on boundary", strcmp(rt_string_cstr(result), "a") == 0);
    }

    // Shorten never starts the tail slice inside a codepoint.
    {
        rt_string text = rt_const_cstr("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
        rt_string result = rt_textwrap_shorten(text, 7);
        test_result("Shorten keeps whole codepoints",
                    strcmp(rt_string_cstr(result), "\xC3\xA9...\xC3\xA9") == 0);
    }
}

int main() {
    printf("=== RT TextWrapper Tests ===\n\n");

    test_wrap();
    test_utf8_boundaries_preserved();
    test_indent();
    test_dedent();
    test_truncate();
    test_shorten();
    test_alignment();
    test_invalid_handles_trap();
    test_returning_trap_stops_size_overflow();
    test_utility();
    test_hang();

    printf("All TextWrapper tests passed!\n");
    return 0;
}
