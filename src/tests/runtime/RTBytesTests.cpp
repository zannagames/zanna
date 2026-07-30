//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTBytesTests.cpp
// Purpose: Comprehensive tests for Zanna.Collections.Bytes byte array.
// Key invariants: Bounds and parser validation stop before memory access even
//                 when the embedder trap hook returns.
// Ownership/Lifetime: Tests release temporary forged-input fixtures and rely
//                     on process teardown for legacy test allocations.
// Links: src/runtime/collections/rt_bytes.c
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
} // namespace

static bool g_trap_return = false; // when set, vm_trap records and RETURNS

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_return)
        return; // simulate an embedder hook that returns (VDOC-177)
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static void test_new_creates_zero_filled_bytes() {
    void *bytes = rt_bytes_new(10);
    assert(bytes != nullptr);
    assert(rt_bytes_len(bytes) == 10);

    // Should be zero-filled
    for (int64_t i = 0; i < 10; ++i) {
        assert(rt_bytes_get(bytes, i) == 0);
    }
}

static void test_new_with_zero_length() {
    void *bytes = rt_bytes_new(0);
    assert(bytes != nullptr);
    assert(rt_bytes_len(bytes) == 0);
}

static void test_new_with_negative_length() {
    EXPECT_TRAP(rt_bytes_new(-5));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.New") != nullptr);
}

static void test_from_str() {
    rt_string str = rt_string_from_bytes("Hello", 5);
    void *bytes = rt_bytes_from_str(str);

    assert(rt_bytes_len(bytes) == 5);
    assert(rt_bytes_get(bytes, 0) == 'H');
    assert(rt_bytes_get(bytes, 1) == 'e');
    assert(rt_bytes_get(bytes, 2) == 'l');
    assert(rt_bytes_get(bytes, 3) == 'l');
    assert(rt_bytes_get(bytes, 4) == 'o');
}

static void test_from_hex() {
    rt_string hex = rt_string_from_bytes("deadbeef", 8);
    void *bytes = rt_bytes_from_hex(hex);

    assert(rt_bytes_len(bytes) == 4);
    assert(rt_bytes_get(bytes, 0) == 0xDE);
    assert(rt_bytes_get(bytes, 1) == 0xAD);
    assert(rt_bytes_get(bytes, 2) == 0xBE);
    assert(rt_bytes_get(bytes, 3) == 0xEF);
}

static void test_from_hex_uppercase() {
    rt_string hex = rt_string_from_bytes("CAFEBABE", 8);
    void *bytes = rt_bytes_from_hex(hex);

    assert(rt_bytes_len(bytes) == 4);
    assert(rt_bytes_get(bytes, 0) == 0xCA);
    assert(rt_bytes_get(bytes, 1) == 0xFE);
    assert(rt_bytes_get(bytes, 2) == 0xBA);
    assert(rt_bytes_get(bytes, 3) == 0xBE);
}

static void test_from_hex_odd_length_traps() {
    rt_string hex = rt_string_from_bytes("abc", 3);
    EXPECT_TRAP(rt_bytes_from_hex(hex));
}

static void test_from_hex_invalid_char_traps() {
    rt_string hex = rt_string_from_bytes("zzzz", 4);
    EXPECT_TRAP(rt_bytes_from_hex(hex));
}

static void test_from_hex_embedded_nul_is_not_truncated() {
    const char hex_bytes[] = {'4', '1', '\0', '4'};
    rt_string hex = rt_string_from_bytes(hex_bytes, sizeof(hex_bytes));
    EXPECT_TRAP(rt_bytes_from_hex(hex));
}

static void test_get_set() {
    void *bytes = rt_bytes_new(4);

    rt_bytes_set(bytes, 0, 0xDE);
    rt_bytes_set(bytes, 1, 0xAD);
    rt_bytes_set(bytes, 2, 0xBE);
    rt_bytes_set(bytes, 3, 0xEF);

    assert(rt_bytes_get(bytes, 0) == 0xDE);
    assert(rt_bytes_get(bytes, 1) == 0xAD);
    assert(rt_bytes_get(bytes, 2) == 0xBE);
    assert(rt_bytes_get(bytes, 3) == 0xEF);
}

static void test_set_clamps_to_byte() {
    void *bytes = rt_bytes_new(2);

    // Values should be clamped to 0-255
    rt_bytes_set(bytes, 0, 0x1234); // Only lower 8 bits (0x34)
    rt_bytes_set(bytes, 1, -1);     // Should become 0xFF

    assert(rt_bytes_get(bytes, 0) == 0x34);
    assert(rt_bytes_get(bytes, 1) == 0xFF);
}

static void test_get_out_of_bounds_traps() {
    void *bytes = rt_bytes_new(5);
    EXPECT_TRAP(rt_bytes_get(bytes, 5));
    EXPECT_TRAP(rt_bytes_get(bytes, -1));
}

static void test_set_out_of_bounds_traps() {
    void *bytes = rt_bytes_new(5);
    EXPECT_TRAP(rt_bytes_set(bytes, 5, 0));
    EXPECT_TRAP(rt_bytes_set(bytes, -1, 0));
}

static void test_slice() {
    void *bytes = rt_bytes_new(5);
    for (int64_t i = 0; i < 5; ++i)
        rt_bytes_set(bytes, i, (int64_t)(i + 10));

    void *slice = rt_bytes_slice(bytes, 1, 4);
    assert(rt_bytes_len(slice) == 3);
    assert(rt_bytes_get(slice, 0) == 11);
    assert(rt_bytes_get(slice, 1) == 12);
    assert(rt_bytes_get(slice, 2) == 13);
}

static void test_slice_clamps_bounds() {
    void *bytes = rt_bytes_new(5);
    for (int64_t i = 0; i < 5; ++i)
        rt_bytes_set(bytes, i, (int64_t)(i + 1));

    // Start clamped to 0
    void *slice1 = rt_bytes_slice(bytes, -5, 3);
    assert(rt_bytes_len(slice1) == 3);
    assert(rt_bytes_get(slice1, 0) == 1);

    // End clamped to len
    void *slice2 = rt_bytes_slice(bytes, 2, 100);
    assert(rt_bytes_len(slice2) == 3);
    assert(rt_bytes_get(slice2, 0) == 3);

    // Empty slice when start >= end
    void *slice3 = rt_bytes_slice(bytes, 3, 2);
    assert(rt_bytes_len(slice3) == 0);
}

static void test_copy() {
    void *src = rt_bytes_new(5);
    for (int64_t i = 0; i < 5; ++i)
        rt_bytes_set(src, i, (int64_t)(i + 1));

    void *dst = rt_bytes_new(10);
    rt_bytes_copy(dst, 3, src, 1, 3); // Copy 3 bytes from src[1..4] to dst[3..6]

    assert(rt_bytes_get(dst, 0) == 0);
    assert(rt_bytes_get(dst, 1) == 0);
    assert(rt_bytes_get(dst, 2) == 0);
    assert(rt_bytes_get(dst, 3) == 2);
    assert(rt_bytes_get(dst, 4) == 3);
    assert(rt_bytes_get(dst, 5) == 4);
    assert(rt_bytes_get(dst, 6) == 0);
}

static void test_copy_overlapping() {
    // Test that copy handles overlapping regions correctly (memmove)
    void *bytes = rt_bytes_new(10);
    for (int64_t i = 0; i < 10; ++i)
        rt_bytes_set(bytes, i, (int64_t)(i + 1));

    // Copy bytes[2..7] to bytes[0..5]
    rt_bytes_copy(bytes, 0, bytes, 2, 5);

    assert(rt_bytes_get(bytes, 0) == 3);
    assert(rt_bytes_get(bytes, 1) == 4);
    assert(rt_bytes_get(bytes, 2) == 5);
    assert(rt_bytes_get(bytes, 3) == 6);
    assert(rt_bytes_get(bytes, 4) == 7);
}

static void test_copy_bounds_check() {
    void *src = rt_bytes_new(5);
    void *dst = rt_bytes_new(5);

    EXPECT_TRAP(rt_bytes_copy(dst, 3, src, 0, 5));             // dst overflow
    EXPECT_TRAP(rt_bytes_copy(dst, 0, src, 3, 5));             // src overflow
    EXPECT_TRAP(rt_bytes_copy(dst, INT64_MAX - 1, src, 0, 4)); // dst index overflow guard
    EXPECT_TRAP(rt_bytes_copy(dst, 0, src, INT64_MAX - 1, 4)); // src index overflow guard
    EXPECT_TRAP(rt_bytes_copy(dst, -1, src, 0, 1));            // negative dst index
    EXPECT_TRAP(rt_bytes_copy(dst, 0, src, -1, 1));            // negative src index
    EXPECT_TRAP(rt_bytes_copy(dst, 0, src, 0, -1));            // negative count
}

static void test_copy_rejects_non_bytes_even_for_zero_count() {
    void *bytes = rt_bytes_new(5);
    void *not_bytes = rt_obj_new_i64(12345, 8);
    assert(not_bytes != nullptr);

    EXPECT_TRAP(rt_bytes_copy(not_bytes, 0, bytes, 0, 0));
    EXPECT_TRAP(rt_bytes_copy(bytes, 0, not_bytes, 0, 0));

    if (rt_obj_release_check0(not_bytes))
        rt_obj_free(not_bytes);
}

static void test_signed_binary_reads_sign_extend() {
    void *bytes = rt_bytes_new(28);

    rt_bytes_write_i16le(bytes, 0, -1);
    rt_bytes_write_i16be(bytes, 2, -2);
    rt_bytes_write_i32le(bytes, 4, -1);
    rt_bytes_write_i32be(bytes, 8, -2);
    rt_bytes_write_i64le(bytes, 12, INT64_MIN);
    rt_bytes_write_i64be(bytes, 20, -1);

    assert(rt_bytes_read_i16le(bytes, 0) == -1);
    assert(rt_bytes_read_i16be(bytes, 2) == -2);
    assert(rt_bytes_read_i32le(bytes, 4) == -1);
    assert(rt_bytes_read_i32be(bytes, 8) == -2);
    assert(rt_bytes_read_i64le(bytes, 12) == INT64_MIN);
    assert(rt_bytes_read_i64be(bytes, 20) == -1);
}

static void test_to_str() {
    void *bytes = rt_bytes_new(5);
    rt_bytes_set(bytes, 0, 'H');
    rt_bytes_set(bytes, 1, 'e');
    rt_bytes_set(bytes, 2, 'l');
    rt_bytes_set(bytes, 3, 'l');
    rt_bytes_set(bytes, 4, 'o');

    rt_string str = rt_bytes_to_str(bytes);
    const char *cstr = rt_string_cstr(str);
    assert(strncmp(cstr, "Hello", 5) == 0);
}

static void test_to_hex() {
    void *bytes = rt_bytes_new(4);
    rt_bytes_set(bytes, 0, 0xDE);
    rt_bytes_set(bytes, 1, 0xAD);
    rt_bytes_set(bytes, 2, 0xBE);
    rt_bytes_set(bytes, 3, 0xEF);

    rt_string hex = rt_bytes_to_hex(bytes);
    const char *cstr = rt_string_cstr(hex);
    assert(strcmp(cstr, "deadbeef") == 0);
}

static void test_to_base64() {
    rt_string input = rt_string_from_bytes("hello", 5);
    void *bytes = rt_bytes_from_str(input);

    rt_string b64 = rt_bytes_to_base64(bytes);
    const char *cstr = rt_string_cstr(b64);
    assert(strcmp(cstr, "aGVsbG8=") == 0);
}

static void test_from_base64_to_str() {
    rt_string b64 = rt_string_from_bytes("aGVsbG8=", 8);
    void *bytes = rt_bytes_from_base64(b64);

    rt_string decoded = rt_bytes_to_str(bytes);
    const char *cstr = rt_string_cstr(decoded);
    assert(strcmp(cstr, "hello") == 0);
}

static void test_base64_empty_roundtrip() {
    void *bytes = rt_bytes_new(0);
    rt_string b64 = rt_bytes_to_base64(bytes);
    assert(rt_string_cstr(b64)[0] == '\0');

    void *decoded = rt_bytes_from_base64(rt_string_from_bytes("", 0));
    assert(rt_bytes_len(decoded) == 0);
}

static void test_from_base64_invalid_chars_traps() {
    rt_string b64 = rt_string_from_bytes("!!!!", 4);
    EXPECT_TRAP(rt_bytes_from_base64(b64));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.FromBase64") != nullptr);
}

static void test_from_base64_embedded_nul_is_not_truncated() {
    const char b64_bytes[] = {'Q', 'Q', '=', '=', '\0', 'A', 'A', 'A', 'A'};
    rt_string b64 = rt_string_from_bytes(b64_bytes, sizeof(b64_bytes));
    EXPECT_TRAP(rt_bytes_from_base64(b64));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.FromBase64") != nullptr);
}

static void test_from_base64_bad_padding_traps() {
    rt_string b64 = rt_string_from_bytes("AA=A", 4);
    EXPECT_TRAP(rt_bytes_from_base64(b64));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.FromBase64") != nullptr);

    rt_string noncanonical = rt_string_from_bytes("AB==", 4);
    EXPECT_TRAP(rt_bytes_from_base64(noncanonical));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.FromBase64") != nullptr);
}

static void test_hex_roundtrip() {
    // Create bytes, convert to hex, convert back
    void *original = rt_bytes_new(8);
    for (int64_t i = 0; i < 8; ++i)
        rt_bytes_set(original, i, (int64_t)(i * 17)); // Various values

    rt_string hex = rt_bytes_to_hex(original);
    void *restored = rt_bytes_from_hex(hex);

    assert(rt_bytes_len(restored) == 8);
    for (int64_t i = 0; i < 8; ++i)
        assert(rt_bytes_get(restored, i) == rt_bytes_get(original, i));
}

static void test_fill() {
    void *bytes = rt_bytes_new(10);
    rt_bytes_fill(bytes, 0xAB);

    for (int64_t i = 0; i < 10; ++i)
        assert(rt_bytes_get(bytes, i) == 0xAB);
}

static void test_fill_clamps_to_byte() {
    void *bytes = rt_bytes_new(3);
    rt_bytes_fill(bytes, 0x12345); // Should use 0x45

    for (int64_t i = 0; i < 3; ++i)
        assert(rt_bytes_get(bytes, i) == 0x45);
}

static void test_find() {
    void *bytes = rt_bytes_new(10);
    for (int64_t i = 0; i < 10; ++i)
        rt_bytes_set(bytes, i, (int64_t)(i + 1));

    assert(rt_bytes_find(bytes, 1) == 0);
    assert(rt_bytes_find(bytes, 5) == 4);
    assert(rt_bytes_find(bytes, 10) == 9);
    assert(rt_bytes_find(bytes, 11) == -1); // Not found
    assert(rt_bytes_find(bytes, 0) == -1);  // Not found
}

static void test_find_with_duplicates() {
    void *bytes = rt_bytes_new(5);
    rt_bytes_set(bytes, 0, 1);
    rt_bytes_set(bytes, 1, 2);
    rt_bytes_set(bytes, 2, 3);
    rt_bytes_set(bytes, 3, 2);
    rt_bytes_set(bytes, 4, 1);

    // Should return first occurrence
    assert(rt_bytes_find(bytes, 2) == 1);
    assert(rt_bytes_find(bytes, 1) == 0);
}

static void test_clone() {
    void *original = rt_bytes_new(5);
    for (int64_t i = 0; i < 5; ++i)
        rt_bytes_set(original, i, (int64_t)(i + 10));

    void *clone = rt_bytes_clone(original);

    // Same contents
    assert(rt_bytes_len(clone) == 5);
    for (int64_t i = 0; i < 5; ++i)
        assert(rt_bytes_get(clone, i) == rt_bytes_get(original, i));

    // Modification is independent
    rt_bytes_set(clone, 0, 99);
    assert(rt_bytes_get(original, 0) == 10); // Original unchanged
    assert(rt_bytes_get(clone, 0) == 99);
}

static void test_null_handling() {
    // These should return safe defaults
    assert(rt_bytes_len(nullptr) == 0);
    assert(rt_bytes_find(nullptr, 0) == -1);

    // Slice and clone on null return empty bytes
    void *slice = rt_bytes_slice(nullptr, 0, 10);
    assert(rt_bytes_len(slice) == 0);

    void *clone = rt_bytes_clone(nullptr);
    assert(rt_bytes_len(clone) == 0);

    // ToStr and ToHex on null return empty string
    rt_string str = rt_bytes_to_str(nullptr);
    assert(rt_string_cstr(str)[0] == '\0');

    rt_string hex = rt_bytes_to_hex(nullptr);
    assert(rt_string_cstr(hex)[0] == '\0');

    rt_string b64 = rt_bytes_to_base64(nullptr);
    assert(rt_string_cstr(b64)[0] == '\0');

    // Fill on null should not crash
    rt_bytes_fill(nullptr, 0);
}

static void test_null_traps() {
    EXPECT_TRAP(rt_bytes_get(nullptr, 0));
    EXPECT_TRAP(rt_bytes_set(nullptr, 0, 0));
    EXPECT_TRAP(rt_bytes_copy(nullptr, 0, nullptr, 0, 1));
}

static void test_from_raw_rejects_size_t_overflow() {
    if (SIZE_MAX > (size_t)INT64_MAX) {
        EXPECT_TRAP(rt_bytes_from_raw(nullptr, (size_t)INT64_MAX + 1u));
        assert(g_last_trap && strstr(g_last_trap, "Bytes.FromRaw") != nullptr);
    }
}

static void test_from_raw_rejects_null_nonempty_data() {
    EXPECT_TRAP(rt_bytes_from_raw(nullptr, 1));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.FromRaw") != nullptr);
}

static void test_extract_raw_requires_length_output() {
    void *bytes = rt_bytes_new(1);
    EXPECT_TRAP(rt_bytes_extract_raw(bytes, nullptr));
    assert(g_last_trap && strstr(g_last_trap, "Bytes.ExtractRaw") != nullptr);
}

static void test_rejects_non_bytes_objects() {
    void *obj = rt_obj_new_i64(12345, 8);
    assert(rt_bytes_is_bytes(obj) == 0);
    EXPECT_TRAP(rt_bytes_len(obj));
    EXPECT_TRAP(rt_bytes_data(obj));
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief With a RETURNING trap hook installed, invalid-Bytes-handle methods
///        must return a type-appropriate sentinel instead of dereferencing the
///        NULL that the checked cast returns (VDOC-177).
static void test_invalid_handle_with_returning_hook() {
    // A runtime string handle is a valid object but NOT a Bytes instance, so
    // rt_bytes_require traps and (with a returning hook) yields NULL.
    void *not_bytes = (void *)rt_string_from_bytes("not bytes", 9);

    g_trap_return = true;
    g_last_trap = nullptr;

    assert(rt_bytes_len(not_bytes) == 0);
    assert(rt_bytes_is_empty(not_bytes) == 1);
    assert(rt_bytes_get(not_bytes, 0) == 0);
    rt_bytes_set(not_bytes, 0, 1); // must not crash
    assert(rt_bytes_find(not_bytes, 42) == -1);
    void *sliced = rt_bytes_slice(not_bytes, 0, 4);
    assert(sliced != nullptr && rt_bytes_len(sliced) == 0);
    void *cloned = rt_bytes_clone(not_bytes);
    assert(cloned != nullptr && rt_bytes_len(cloned) == 0);
    rt_string hex = rt_bytes_to_hex(not_bytes);
    assert(rt_str_len(hex) == 0);
    rt_string str = rt_bytes_to_str(not_bytes);
    assert(rt_str_len(str) == 0);
    rt_bytes_fill(not_bytes, 0); // must not crash
    // Read/write helpers route through bounds check; also must not crash.
    assert(rt_bytes_read_i32le(not_bytes, 0) == 0);
    rt_bytes_write_i32le(not_bytes, 0, 5);

    g_trap_return = false;
    assert(g_last_trap != nullptr); // a trap was in fact raised each time
    printf("test_invalid_handle_with_returning_hook: OK\n");
}

static void test_returning_hook_stops_parser_and_bounds_failures() {
    rt_string odd_hex = rt_string_from_bytes("abc", 3);
    rt_string bad_hex = rt_string_from_bytes("zz", 2);
    rt_string short_b64 = rt_string_from_bytes("AAA", 3);
    rt_string bad_b64 = rt_string_from_bytes("!!!!", 4);
    rt_string forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(0x2468U));
    void *bytes = rt_bytes_new(2);
    rt_bytes_set(bytes, 0, 0x5A);

    g_trap_return = true;
    g_last_trap = nullptr;

    assert(rt_bytes_from_hex(odd_hex) == nullptr);
    assert(rt_bytes_from_hex(bad_hex) == nullptr);
    assert(rt_bytes_from_base64(short_b64) == nullptr);
    assert(rt_bytes_from_base64(bad_b64) == nullptr);
    assert(rt_bytes_from_str(forged) == nullptr);
    assert(rt_bytes_from_hex(forged) == nullptr);
    assert(rt_bytes_from_base64(forged) == nullptr);
    assert(rt_bytes_from_raw(nullptr, 1) == nullptr);

    assert(rt_bytes_get(bytes, 99) == 0);
    rt_bytes_set(bytes, 99, 0xFF);
    assert(rt_bytes_get(bytes, 0) == 0x5A);

    g_trap_return = false;
    assert(g_last_trap != nullptr);

    rt_string_unref(odd_hex);
    rt_string_unref(bad_hex);
    rt_string_unref(short_b64);
    rt_string_unref(bad_b64);
    if (rt_obj_release_check0(bytes))
        rt_obj_free(bytes);
}

int main() {
    test_new_creates_zero_filled_bytes();
    test_new_with_zero_length();
    test_new_with_negative_length();
    test_from_str();
    test_from_hex();
    test_from_hex_uppercase();
    test_from_hex_odd_length_traps();
    test_from_hex_invalid_char_traps();
    test_from_hex_embedded_nul_is_not_truncated();
    test_get_set();
    test_set_clamps_to_byte();
    test_get_out_of_bounds_traps();
    test_set_out_of_bounds_traps();
    test_slice();
    test_slice_clamps_bounds();
    test_copy();
    test_copy_overlapping();
    test_copy_bounds_check();
    test_copy_rejects_non_bytes_even_for_zero_count();
    test_signed_binary_reads_sign_extend();
    test_to_str();
    test_to_hex();
    test_to_base64();
    test_from_base64_to_str();
    test_base64_empty_roundtrip();
    test_from_base64_invalid_chars_traps();
    test_from_base64_embedded_nul_is_not_truncated();
    test_from_base64_bad_padding_traps();
    test_hex_roundtrip();
    test_fill();
    test_fill_clamps_to_byte();
    test_find();
    test_find_with_duplicates();
    test_clone();
    test_null_handling();
    test_null_traps();
    test_from_raw_rejects_size_t_overflow();
    test_from_raw_rejects_null_nonempty_data();
    test_extract_raw_requires_length_output();
    test_rejects_non_bytes_objects();
    test_invalid_handle_with_returning_hook();
    test_returning_hook_stops_parser_and_bounds_failures();

    return 0;
}
