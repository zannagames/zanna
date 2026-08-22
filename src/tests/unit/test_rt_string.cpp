//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_string.cpp
// Purpose: Verify runtime string helpers including substring operations clamp inputs.
// Key invariants: Substring operations clamp start/length and avoid overflow.
// Ownership/Lifetime: Uses runtime library.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_internal.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <setjmp.h>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

extern "C" void rt_string_register_handle(rt_string s);
extern "C" void rt_string_unregister_handle(rt_string s);

int main() {
    rt_string empty = rt_const_cstr("");
    assert(rt_str_len(empty) == 0);

    rt_string hello = rt_const_cstr("hello");
    assert(rt_str_byte_at(hello, 0) == 'h');
    assert(rt_str_byte_at(hello, 4) == 'o');
    assert(rt_str_byte_at(hello, -1) == -1);
    assert(rt_str_byte_at(hello, 5) == -1);
    assert(rt_str_byte_at(rt_const_cstr("\xC3\xA9"), 0) == 0xC3);
    assert(rt_str_byte_at(rt_const_cstr("\xC3\xA9"), 1) == 0xA9);
    rt_string world = rt_const_cstr("world");
    rt_string hw = rt_str_concat(rt_string_ref(hello), rt_string_ref(world));
    assert(rt_str_len(hw) == 10);
    rt_string helloworld = rt_const_cstr("helloworld");
    assert(rt_str_eq(hw, helloworld));

    rt_string sub0 = rt_str_substr(hw, 0, 5);
    assert(rt_str_eq(sub0, hello));
    rt_string sub1 = rt_str_substr(hw, 5, 5);
    assert(rt_str_eq(sub1, world));
    rt_string subempty = rt_str_substr(hw, 10, 0);
    assert(rt_str_len(subempty) == 0);

    rt_string clamp1 = rt_str_substr(hw, 8, 10);
    rt_string ld = rt_const_cstr("ld");
    assert(rt_str_eq(clamp1, ld));
    rt_string clamp2 = rt_str_substr(hw, -3, 4);
    rt_string hell = rt_const_cstr("hell");
    assert(rt_str_eq(clamp2, hell));
    rt_string clamp3 = rt_str_substr(hw, 2, -5);
    assert(rt_str_len(clamp3) == 0);

    int64_t huge = std::numeric_limits<int64_t>::max();
    rt_string biglen = rt_str_substr(hw, 2, huge);
    rt_string lloworld = rt_const_cstr("lloworld");
    assert(rt_str_eq(biglen, lloworld));
    rt_string bigstart = rt_str_substr(hw, huge, huge);
    assert(rt_str_len(bigstart) == 0);

    assert(!rt_str_eq(hello, world));

    rt_string num = rt_const_cstr("  -42 ");
    assert(rt_to_int(num) == -42);

    rt_string abcde = rt_const_cstr("ABCDE");

    rt_string left = rt_str_left(abcde, 2);
    rt_string ab = rt_const_cstr("AB");
    assert(rt_str_eq(left, ab));

    rt_string right = rt_str_right(abcde, 3);
    rt_string cde = rt_const_cstr("CDE");
    assert(rt_str_eq(right, cde));

    rt_string mid_full = rt_str_mid(abcde, 1);
    assert(rt_str_eq(mid_full, abcde));

    rt_string mid_part = rt_str_mid_len(abcde, 1, 2);
    rt_string mid_ab = rt_const_cstr("AB");
    assert(rt_str_eq(mid_part, mid_ab));

    rt_string full_left = rt_str_left(abcde, 5);
    assert(full_left == abcde);
    rt_string full_right = rt_str_right(abcde, 5);
    assert(full_right == abcde);
    rt_string empty_left = rt_str_left(abcde, 0);
    rt_string empty_mid = rt_str_mid_len(abcde, 2, 0);
    assert(empty_left == empty_mid);

    {
        rt_string left_owned = rt_const_cstr("left");
        rt_string right_owned = rt_const_cstr("right");
        auto *left_impl = (rt_string_impl *)left_owned;
        auto *right_impl = (rt_string_impl *)right_owned;
        // Short strings may be literal (heap==NULL) or embedded (heap==RT_SSO_SENTINEL)
        assert(left_impl->heap == nullptr || left_impl->heap == RT_SSO_SENTINEL);
        assert(right_impl->heap == nullptr || right_impl->heap == RT_SSO_SENTINEL);
        size_t left_before = left_impl->literal_refs;
        size_t right_before = right_impl->literal_refs;
        rt_string joined = rt_str_concat(rt_string_ref(left_owned), rt_string_ref(right_owned));
        assert(left_impl->literal_refs == left_before);
        assert(right_impl->literal_refs == right_before);
        rt_string_unref(joined);
        rt_string_unref(left_owned);
        rt_string_unref(right_owned);
    }

    {
        rt_string base = rt_const_cstr("dup");
        auto *base_impl = (rt_string_impl *)base;
        assert(base_impl->heap == nullptr || base_impl->heap == RT_SSO_SENTINEL);
        size_t before = base_impl->literal_refs;
        rt_string doubled = rt_str_concat(rt_string_ref(base), rt_string_ref(base));
        assert(base_impl->literal_refs == before);
        rt_string_unref(doubled);
        rt_string_unref(base);
    }

    {
        // Create strings long enough to not use SSO (> RT_SSO_MAX_LEN = 32)
        const char *long_left = "heap_string_that_is_longer_than_32_chars";
        const char *long_right = "data_string_that_is_longer_than_32_chars";
        rt_string left_heap = rt_string_from_bytes(long_left, std::strlen(long_left));
        rt_string right_heap = rt_string_from_bytes(long_right, std::strlen(long_right));
        auto *left_impl = (rt_string_impl *)left_heap;
        auto *right_impl = (rt_string_impl *)right_heap;
        // With SSO, short strings use embedded storage (heap == RT_SSO_SENTINEL)
        // Long strings use heap allocation (heap != nullptr && heap != RT_SSO_SENTINEL)
        assert(left_impl->heap != nullptr && left_impl->heap != RT_SSO_SENTINEL);
        assert(right_impl->heap != nullptr && right_impl->heap != RT_SSO_SENTINEL);
        size_t left_before = left_impl->heap->refcnt;
        size_t right_before = right_impl->heap->refcnt;
        rt_string merged = rt_str_concat(rt_string_ref(left_heap), rt_string_ref(right_heap));
        assert(left_impl->heap->refcnt == left_before);
        assert(right_impl->heap->refcnt == right_before);
        // Merged string is long so it also uses heap
        auto *merged_impl = (rt_string_impl *)merged;
        assert(merged_impl->heap != nullptr && merged_impl->heap != RT_SSO_SENTINEL);
        rt_string_unref(merged);
        rt_string_unref(left_heap);
        rt_string_unref(right_heap);
    }

    {
        rt_string original = rt_const_cstr("clone-source");
        auto *original_impl = (rt_string_impl *)original;
        assert(original_impl->heap == nullptr || original_impl->heap == RT_SSO_SENTINEL);
        size_t before = original_impl->literal_refs;

        rt_string cloned = rt_str_clone(original);
        assert(cloned == original);
        assert(original_impl->literal_refs == before + 1);

        rt_string_unref(original);
        rt_string expected = rt_const_cstr("clone-source");
        assert(rt_str_eq(cloned, expected));
        rt_string_unref(expected);
        rt_string_unref(cloned);
    }

    {
        rt_string_impl huge_literal = {
            RT_STRING_MAGIC, nullptr, nullptr, std::numeric_limits<size_t>::max(), 0};
        rt_string_impl small_literal = {RT_STRING_MAGIC, nullptr, nullptr, 16, 0};
        huge_literal.data = const_cast<char *>("x");
        small_literal.data = const_cast<char *>("small");
        rt_string_register_handle(&huge_literal);
        rt_string_register_handle(&small_literal);
        g_last_trap = nullptr;
        g_trap_expected = true;
        if (setjmp(g_trap_jmp) == 0) {
            (void)rt_str_concat(&huge_literal, &small_literal);
            assert(!"rt_str_concat should trap on overflow");
        } else {
            assert(g_last_trap != nullptr);
            assert(std::strcmp(g_last_trap, "rt_str_concat: length overflow") == 0);
        }
        g_trap_expected = false;
        g_last_trap = nullptr;
        rt_string_unregister_handle(&small_literal);
        rt_string_unregister_handle(&huge_literal);
    }

    return 0;
}
