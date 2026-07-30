//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTBinaryBufferTests.cpp
// Purpose: Comprehensive tests for the BinaryBuffer runtime type.
//          Covers constructors, write/read round-trips, cursor semantics,
//          capacity growth, to_bytes/from_bytes paths, and reset behaviour.
// Key invariants: Cursor/length updates commit only after validation and full
//                 allocation; returning trap hooks cannot advance bad writes.
// Ownership/Lifetime: Test fixtures are runtime-managed and remain reachable
//                     for the duration of each focused test process.
// Links: src/runtime/collections/rt_binbuf.c
//
//===----------------------------------------------------------------------===//

#include "rt_binbuf.h"
#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>

//=============================================================================
// Trap infrastructure
//=============================================================================

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
// When true, vm_trap RECORDS the message and RETURNS (like an embedder trap
// hook that resumes) instead of longjmp/abort — this is what exercises the
// post-trap fall-through guards (VDOC-189).
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
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            (void)(expr);                                                                          \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

//=============================================================================
// Construction
//=============================================================================

static void test_new_default() {
    void *bb = rt_binbuf_new();
    assert(bb != nullptr);
    assert(rt_binbuf_get_len(bb) == 0);
    assert(rt_binbuf_get_position(bb) == 0);
}

static void test_new_cap() {
    void *bb = rt_binbuf_new_cap(1024);
    assert(bb != nullptr);
    assert(rt_binbuf_get_len(bb) == 0);
    assert(rt_binbuf_get_position(bb) == 0);
}

static void test_new_cap_clamped() {
    void *bb = rt_binbuf_new_cap(0);
    assert(bb != nullptr);
    assert(rt_binbuf_get_len(bb) == 0);
    EXPECT_TRAP(rt_binbuf_new_cap(-5));
    assert(g_last_trap && strstr(g_last_trap, "BinaryBuffer") != nullptr);
}

static void test_from_bytes() {
    // Build a bytes object [10, 20, 30]
    void *src = rt_bytes_new(3);
    rt_bytes_set(src, 0, 10);
    rt_bytes_set(src, 1, 20);
    rt_bytes_set(src, 2, 30);

    void *bb = rt_binbuf_from_bytes(src);
    assert(bb != nullptr);
    assert(rt_binbuf_get_len(bb) == 3);
    assert(rt_binbuf_get_position(bb) == 0);

    // Verify content via read (exercises IO-H-2 fixed path)
    assert(rt_binbuf_read_byte(bb) == 10);
    assert(rt_binbuf_read_byte(bb) == 20);
    assert(rt_binbuf_read_byte(bb) == 30);
}

static void test_from_bytes_null() {
    EXPECT_TRAP(rt_binbuf_from_bytes(nullptr));
}

//=============================================================================
// Write / Read Round-Trips
//=============================================================================

static void test_write_read_byte() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 0xAB);
    assert(rt_binbuf_get_len(bb) == 1);

    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_byte(bb) == 0xAB);
}

static void test_write_byte_range_traps() {
    void *bb = rt_binbuf_new();
    EXPECT_TRAP(rt_binbuf_write_byte(bb, -1));
    EXPECT_TRAP(rt_binbuf_write_byte(bb, 256));
    rt_binbuf_write_byte(bb, 255);
    assert(rt_binbuf_get_len(bb) == 1);
}

static void test_write_read_i16le() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i16le(bb, 0x1234);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i16le(bb) == 0x1234);
}

static void test_write_read_i16be() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i16be(bb, 0x5678);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i16be(bb) == 0x5678);
}

static void test_write_read_i32le() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i32le(bb, 0x12345678);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i32le(bb) == 0x12345678);
}

static void test_write_read_i32be() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i32be(bb, 0x1EADBEEF);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i32be(bb) == 0x1EADBEEF);
}

static void test_write_read_unsigned_widths() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_u16le(bb, 0xFFFF);
    rt_binbuf_write_u16be(bb, 0xBEEF);
    rt_binbuf_write_u32le(bb, 0xFFFFFFFFLL);
    rt_binbuf_write_u32be(bb, 0xDEADBEEFULL);

    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_u16le(bb) == 0xFFFF);
    assert(rt_binbuf_read_u16be(bb) == 0xBEEF);
    assert(rt_binbuf_read_u32le(bb) == (int64_t)UINT32_MAX);
    assert(rt_binbuf_read_u32be(bb) == 0xDEADBEEFULL);
}

static void test_write_read_i64le() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i64le(bb, 0x0123456789ABCDEFLL);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i64le(bb) == 0x0123456789ABCDEFLL);
}

static void test_write_read_i64be() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i64be(bb, 0xFEDCBA9876543210LL);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i64be(bb) == (int64_t)0xFEDCBA9876543210LL);
}

static void test_endian_byte_order_i16() {
    // Verify LE places low byte first, BE places high byte first
    void *le = rt_binbuf_new();
    rt_binbuf_write_i16le(le, 0x0102);
    rt_binbuf_set_position(le, 0);
    assert(rt_binbuf_read_byte(le) == 0x02); // low byte first
    assert(rt_binbuf_read_byte(le) == 0x01); // high byte second

    void *be = rt_binbuf_new();
    rt_binbuf_write_i16be(be, 0x0102);
    rt_binbuf_set_position(be, 0);
    assert(rt_binbuf_read_byte(be) == 0x01); // high byte first
    assert(rt_binbuf_read_byte(be) == 0x02); // low byte second
}

static void test_signed_integer_round_trips() {
    void *bb = rt_binbuf_new();

    rt_binbuf_write_i16le(bb, -1);
    rt_binbuf_write_i16be(bb, -2);
    rt_binbuf_write_i32le(bb, -1);
    rt_binbuf_write_i32be(bb, -2);
    rt_binbuf_write_i64le(bb, INT64_MIN);
    rt_binbuf_write_i64be(bb, -1);

    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i16le(bb) == -1);
    assert(rt_binbuf_read_i16be(bb) == -2);
    assert(rt_binbuf_read_i32le(bb) == -1);
    assert(rt_binbuf_read_i32be(bb) == -2);
    assert(rt_binbuf_read_i64le(bb) == INT64_MIN);
    assert(rt_binbuf_read_i64be(bb) == -1);
}

static void test_narrow_integer_range_traps() {
    void *bb = rt_binbuf_new();
    EXPECT_TRAP(rt_binbuf_write_i16le(bb, -32769));
    EXPECT_TRAP(rt_binbuf_write_i16le(bb, 32768));
    EXPECT_TRAP(rt_binbuf_write_i16be(bb, -32769));
    EXPECT_TRAP(rt_binbuf_write_i16be(bb, 32768));
    EXPECT_TRAP(rt_binbuf_write_i32le(bb, (int64_t)INT32_MIN - 1));
    EXPECT_TRAP(rt_binbuf_write_i32le(bb, (int64_t)INT32_MAX + 1));
    EXPECT_TRAP(rt_binbuf_write_i32be(bb, (int64_t)INT32_MIN - 1));
    EXPECT_TRAP(rt_binbuf_write_i32be(bb, (int64_t)INT32_MAX + 1));
    EXPECT_TRAP(rt_binbuf_write_u16le(bb, -1));
    EXPECT_TRAP(rt_binbuf_write_u16le(bb, (int64_t)UINT16_MAX + 1));
    EXPECT_TRAP(rt_binbuf_write_u16be(bb, -1));
    EXPECT_TRAP(rt_binbuf_write_u16be(bb, (int64_t)UINT16_MAX + 1));
    EXPECT_TRAP(rt_binbuf_write_u32le(bb, -1));
    EXPECT_TRAP(rt_binbuf_write_u32le(bb, (int64_t)UINT32_MAX + 1));
    EXPECT_TRAP(rt_binbuf_write_u32be(bb, -1));
    EXPECT_TRAP(rt_binbuf_write_u32be(bb, (int64_t)UINT32_MAX + 1));
}

static void test_write_read_str() {
    void *bb = rt_binbuf_new();
    rt_string s = rt_const_cstr("hello");
    rt_binbuf_write_str(bb, s);

    // Verify: 4-byte LE length prefix + 5 bytes payload
    assert(rt_binbuf_get_len(bb) == 4 + 5);

    rt_binbuf_set_position(bb, 0);
    rt_string out = rt_binbuf_read_str(bb);
    assert(out != nullptr);
    assert(strcmp(rt_string_cstr(out), "hello") == 0);
}

static void test_write_read_str_preserves_embedded_nul() {
    const char bytes[] = {'a', '\0', 'b'};
    void *bb = rt_binbuf_new();
    rt_string s = rt_string_from_bytes(bytes, sizeof(bytes));
    rt_binbuf_write_str(bb, s);

    assert(rt_binbuf_get_len(bb) == 4 + (int64_t)sizeof(bytes));

    rt_binbuf_set_position(bb, 0);
    rt_string out = rt_binbuf_read_str(bb);
    assert(out != nullptr);
    assert(rt_str_len(out) == (int64_t)sizeof(bytes));
    assert(memcmp(rt_string_cstr(out), bytes, sizeof(bytes)) == 0);
    rt_string_unref(s);
    rt_string_unref(out);
}

static void test_read_truncated_str_preserves_position() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_i32le(bb, 4);
    rt_binbuf_write_byte(bb, 'a');
    rt_binbuf_write_byte(bb, 'b');

    rt_binbuf_set_position(bb, 0);
    EXPECT_TRAP(rt_binbuf_read_str(bb));
    assert(rt_binbuf_get_position(bb) == 0);
}

static void test_write_null_str_writes_empty() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_str(bb, nullptr);
    assert(rt_binbuf_get_len(bb) == 4);

    rt_binbuf_set_position(bb, 0);
    rt_string out = rt_binbuf_read_str(bb);
    assert(out != nullptr);
    assert(rt_str_len(out) == 0);
    rt_string_unref(out);
}

static void test_write_read_bytes() {
    void *src = rt_bytes_new(4);
    rt_bytes_set(src, 0, 0xDE);
    rt_bytes_set(src, 1, 0xAD);
    rt_bytes_set(src, 2, 0xBE);
    rt_bytes_set(src, 3, 0xEF);

    void *bb = rt_binbuf_new();
    rt_binbuf_write_bytes(bb, src);

    // 4-byte LE length prefix + 4 bytes payload
    assert(rt_binbuf_get_len(bb) == 4 + 4);

    rt_binbuf_set_position(bb, 0);
    int64_t prefix = rt_binbuf_read_i32le(bb); // read the length prefix
    assert(prefix == 4);
    void *out = rt_binbuf_read_bytes(bb, 4);
    assert(rt_bytes_get(out, 0) == 0xDE);
    assert(rt_bytes_get(out, 1) == 0xAD);
    assert(rt_bytes_get(out, 2) == 0xBE);
    assert(rt_bytes_get(out, 3) == 0xEF);
}

static void test_write_invalid_payloads_trap() {
    void *bb = rt_binbuf_new();
    EXPECT_TRAP(rt_binbuf_write_bytes(bb, nullptr));
}

static void test_write_bytes_invalid_data_preserves_buffer() {
    struct FakeBytes {
        int64_t len;
        uint8_t *data;
    };

    void *bb = rt_binbuf_new();
    FakeBytes *bad = static_cast<FakeBytes *>(rt_obj_new_i64(RT_BYTES_CLASS_ID, sizeof(FakeBytes)));
    bad->len = 1;
    bad->data = nullptr;

    EXPECT_TRAP(rt_binbuf_write_bytes(bb, bad));
    assert(rt_binbuf_get_position(bb) == 0);
    assert(rt_binbuf_get_len(bb) == 0);

    if (rt_obj_release_check0(bad))
        rt_obj_free(bad);
}

//=============================================================================
// Cursor / Position Semantics
//=============================================================================

static void test_position_advances_on_write() {
    void *bb = rt_binbuf_new();
    assert(rt_binbuf_get_position(bb) == 0);
    rt_binbuf_write_byte(bb, 1);
    assert(rt_binbuf_get_position(bb) == 1);
    rt_binbuf_write_i32le(bb, 0);
    assert(rt_binbuf_get_position(bb) == 5);
}

static void test_position_advances_on_read() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 42);
    rt_binbuf_write_byte(bb, 99);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_get_position(bb) == 0);
    rt_binbuf_read_byte(bb);
    assert(rt_binbuf_get_position(bb) == 1);
}

static void test_set_position_bounds_trap() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 1);
    rt_binbuf_write_byte(bb, 2);

    rt_binbuf_set_position(bb, 2);
    assert(rt_binbuf_get_position(bb) == 2);

    EXPECT_TRAP(rt_binbuf_set_position(bb, 100));
    EXPECT_TRAP(rt_binbuf_set_position(bb, -5));
    assert(rt_binbuf_get_position(bb) == 2);
}

static void test_read_past_end_traps() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 0xFF);
    rt_binbuf_set_position(bb, 0);
    rt_binbuf_read_byte(bb);
    // Position is now at end — next read must trap
    EXPECT_TRAP(rt_binbuf_read_byte(bb));
}

//=============================================================================
// to_bytes / from_bytes Round-Trip
//=============================================================================

static void test_to_bytes_round_trip() {
    void *bb = rt_binbuf_new();
    for (int i = 0; i < 8; i++)
        rt_binbuf_write_byte(bb, (int64_t)(i * 10));

    // to_bytes converts buffer contents to a Bytes object (IO-M-2 fixed path)
    void *bytes = rt_binbuf_to_bytes(bb);
    assert(rt_bytes_len(bytes) == 8);
    for (int i = 0; i < 8; i++)
        assert(rt_bytes_get(bytes, i) == (int64_t)(i * 10));
}

static void test_from_bytes_to_bytes_identity() {
    // Build source bytes
    void *src = rt_bytes_new(5);
    for (int i = 0; i < 5; i++)
        rt_bytes_set(src, i, (int64_t)(100 + i));

    // Round-trip: bytes → binbuf → bytes
    void *bb = rt_binbuf_from_bytes(src);
    void *dst = rt_binbuf_to_bytes(bb);

    assert(rt_bytes_len(dst) == 5);
    for (int i = 0; i < 5; i++)
        assert(rt_bytes_get(dst, i) == (int64_t)(100 + i));
}

//=============================================================================
// Reset
//=============================================================================

static void test_reset() {
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 1);
    rt_binbuf_write_byte(bb, 2);
    assert(rt_binbuf_get_len(bb) == 2);
    assert(rt_binbuf_get_position(bb) == 2);

    rt_binbuf_reset(bb);
    assert(rt_binbuf_get_len(bb) == 0);
    assert(rt_binbuf_get_position(bb) == 0);

    // Buffer can be reused after reset
    rt_binbuf_write_byte(bb, 99);
    assert(rt_binbuf_get_len(bb) == 1);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_byte(bb) == 99);
}

//=============================================================================
// Capacity Growth (exercises IO-H-3 overflow guard in binbuf_ensure)
//=============================================================================

static void test_capacity_growth() {
    // Start with a tiny buffer and write enough to force several doublings
    void *bb = rt_binbuf_new_cap(1);

    const int N = 1024;
    for (int i = 0; i < N; i++)
        rt_binbuf_write_byte(bb, (int64_t)(i & 0xFF));

    assert(rt_binbuf_get_len(bb) == N);

    // Verify all written bytes are correct (no corruption from realloc)
    rt_binbuf_set_position(bb, 0);
    for (int i = 0; i < N; i++)
        assert(rt_binbuf_read_byte(bb) == (int64_t)(i & 0xFF));
}

static void test_large_single_write_grows_capacity() {
    // A single write of 4 MB must grow the buffer past default capacity (256)
    const int64_t SIZE = 4 * 1024 * 1024;
    void *bb = rt_binbuf_new();

    void *src = rt_bytes_new(SIZE);
    for (int64_t i = 0; i < SIZE; i++)
        rt_bytes_set(src, i, (int64_t)(i & 0xFF));

    // Write length-prefixed bytes blob (exercises rt_binbuf_write_bytes memcpy path)
    rt_binbuf_write_bytes(bb, src);

    // Buffer should have grown to accommodate 4 + 4MB of data
    assert(rt_binbuf_get_len(bb) == 4 + SIZE);
}

//=============================================================================
// Multiple Values — Structured Protocol Simulation
//=============================================================================

static void test_structured_protocol_encode_decode() {
    // Simulate a minimal binary frame: [version:byte][count:i32le][value:i64le]
    void *bb = rt_binbuf_new();
    rt_binbuf_write_byte(bb, 1);             // version
    rt_binbuf_write_i32le(bb, 42);           // count
    rt_binbuf_write_i64le(bb, 0xCAFEBABELL); // value

    assert(rt_binbuf_get_len(bb) == 1 + 4 + 8);

    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_byte(bb) == 1);
    assert(rt_binbuf_read_i32le(bb) == 42);
    assert(rt_binbuf_read_i64le(bb) == (int64_t)0xCAFEBABELL);
}

/// VDOC-189: with a RETURNING trap hook (an embedder that resumes after a
/// recoverable trap), operations must not dereference a NULL receiver or write
/// an out-of-range value. Getters on an invalid receiver return safe sentinels,
/// and a fixed-width write of an out-of-range value leaves the buffer untouched.
static void test_returning_trap_hook_is_safe() {
    g_trap_returns = true;

    // Getters on an invalid (non-BinaryBuffer) receiver must not crash and must
    // return safe sentinels rather than dereferencing the trapped NULL.
    int bogus = 0;
    void *not_a_buffer = &bogus;
    g_last_trap = nullptr;
    assert(rt_binbuf_get_position(not_a_buffer) == 0);
    assert(g_last_trap != nullptr);
    assert(rt_binbuf_get_len(not_a_buffer) == 0);
    void *empty = rt_binbuf_to_bytes(not_a_buffer);
    assert(empty != nullptr && rt_bytes_len(empty) == 0);
    rt_binbuf_reset(not_a_buffer); // must not crash

    // An out-of-range fixed-width write must NOT append a truncated value after
    // the range validator traps-and-returns.
    void *bb = rt_binbuf_new();
    int64_t len_before = rt_binbuf_get_len(bb);
    g_last_trap = nullptr;
    rt_binbuf_write_i16le(bb, 70000); // > INT16_MAX
    assert(g_last_trap != nullptr);
    assert(rt_binbuf_get_len(bb) == len_before); // nothing written
    g_last_trap = nullptr;
    rt_binbuf_write_u16be(bb, -1); // < 0 for unsigned
    assert(g_last_trap != nullptr);
    assert(rt_binbuf_get_len(bb) == len_before);

    // A forged string must not be serialized as an empty value after its
    // validation trap returns.
    rt_string forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(0x8642U));
    g_last_trap = nullptr;
    rt_binbuf_write_str(bb, forged);
    assert(g_last_trap != nullptr);
    assert(rt_binbuf_get_len(bb) == len_before);

    // A valid write after the trapped ones still works (buffer not corrupted).
    g_trap_returns = false;
    rt_binbuf_write_i16le(bb, 1234);
    rt_binbuf_set_position(bb, 0);
    assert(rt_binbuf_read_i16le(bb) == 1234);
}

//=============================================================================
// Main
//=============================================================================

int main() {
    // Construction
    test_new_default();
    test_new_cap();
    test_new_cap_clamped();
    test_from_bytes();
    test_from_bytes_null();

    // Write / read round-trips
    test_write_read_byte();
    test_write_byte_range_traps();
    test_write_read_i16le();
    test_write_read_i16be();
    test_write_read_i32le();
    test_write_read_i32be();
    test_write_read_unsigned_widths();
    test_write_read_i64le();
    test_write_read_i64be();
    test_endian_byte_order_i16();
    test_signed_integer_round_trips();
    test_narrow_integer_range_traps();
    test_write_read_str();
    test_write_read_str_preserves_embedded_nul();
    test_read_truncated_str_preserves_position();
    test_write_null_str_writes_empty();
    test_write_read_bytes();
    test_write_invalid_payloads_trap();
    test_write_bytes_invalid_data_preserves_buffer();
    test_returning_trap_hook_is_safe();

    // Cursor semantics
    test_position_advances_on_write();
    test_position_advances_on_read();
    test_set_position_bounds_trap();
    test_read_past_end_traps();

    // to_bytes / from_bytes
    test_to_bytes_round_trip();
    test_from_bytes_to_bytes_identity();

    // Reset
    test_reset();

    // Capacity growth
    test_capacity_growth();
    test_large_single_write_grows_capacity();

    // Structured encoding
    test_structured_protocol_encode_decode();

    return 0;
}
