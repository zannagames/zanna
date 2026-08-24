//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTCodecTests.cpp
// Purpose: Validate Zanna.Text.Codec URL, Base64, Hex, and strict UTF-8 helpers.
// Key invariants: Encodings are reversible; invalid encoded input traps; UTF-8
// validation rejects non-scalar and noncanonical byte sequences without trapping.
// Links: docs/zannalib.md

#include "rt_codec.h"
#include "rt_string.h"

#include <cassert>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

static bool bytes_eq(rt_string s, const char *expected, size_t expected_len) {
    return rt_str_len(s) == (int64_t)expected_len &&
           memcmp(rt_string_cstr(s), expected, expected_len) == 0;
}

//=============================================================================
// Strict UTF-8 Validation Tests
//=============================================================================

static void test_utf8_validation() {
    printf("Testing Codec.IsValidUtf8:\n");

    test_result("Empty string is valid", rt_codec_is_valid_utf8(rt_const_cstr("")) == 1);
    test_result("ASCII is valid", rt_codec_is_valid_utf8(rt_const_cstr("plain text")) == 1);
    test_result("Multibyte scalars are valid",
                rt_codec_is_valid_utf8(rt_const_cstr("caf\xC3\xA9 \xF0\x9F\x8C\x8D")) == 1);

    const char embedded_nul[] = {'a', '\0', 'b'};
    test_result("Embedded U+0000 is valid UTF-8",
                rt_codec_is_valid_utf8(rt_string_from_bytes(embedded_nul, sizeof(embedded_nul))) ==
                    1);

    const char bare_continuation[] = {static_cast<char>(0x80)};
    const char overlong[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    const char surrogate[] = {
        static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80)};
    const char truncated[] = {
        static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x8C)};
    const char too_large[] = {static_cast<char>(0xF4),
                              static_cast<char>(0x90),
                              static_cast<char>(0x80),
                              static_cast<char>(0x80)};
    test_result("Bare continuation is rejected",
                rt_codec_is_valid_utf8(
                    rt_string_from_bytes(bare_continuation, sizeof(bare_continuation))) == 0);
    test_result("Overlong sequence is rejected",
                rt_codec_is_valid_utf8(rt_string_from_bytes(overlong, sizeof(overlong))) == 0);
    test_result("UTF-16 surrogate is rejected",
                rt_codec_is_valid_utf8(rt_string_from_bytes(surrogate, sizeof(surrogate))) == 0);
    test_result("Truncated sequence is rejected",
                rt_codec_is_valid_utf8(rt_string_from_bytes(truncated, sizeof(truncated))) == 0);
    test_result("Code point above U+10FFFF is rejected",
                rt_codec_is_valid_utf8(rt_string_from_bytes(too_large, sizeof(too_large))) == 0);
    test_result("Null runtime handle is rejected", rt_codec_is_valid_utf8(nullptr) == 0);

    printf("\n");
}

//=============================================================================
// URL Encoding Tests
//=============================================================================

static void test_url_encode_basic() {
    printf("Testing Codec.UrlEncode basic:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string enc_empty = rt_codec_url_encode(empty);
    test_result("Empty string encodes to empty", strcmp(rt_string_cstr(enc_empty), "") == 0);

    // Unreserved characters pass through unchanged
    rt_string unreserved =
        rt_const_cstr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~");
    rt_string enc_unreserved = rt_codec_url_encode(unreserved);
    test_result("Unreserved chars unchanged",
                strcmp(rt_string_cstr(enc_unreserved), rt_string_cstr(unreserved)) == 0);

    // Space encodes to %20
    rt_string space = rt_const_cstr("hello world");
    rt_string enc_space = rt_codec_url_encode(space);
    test_result("Space encodes to %20", strcmp(rt_string_cstr(enc_space), "hello%20world") == 0);

    // Special characters encode correctly (lowercase hex)
    rt_string special = rt_const_cstr("key=value&other=test");
    rt_string enc_special = rt_codec_url_encode(special);
    test_result("Special chars encoded",
                strcmp(rt_string_cstr(enc_special), "key%3dvalue%26other%3dtest") == 0);

    // Unicode/extended ASCII (lowercase hex)
    rt_string utf8 = rt_const_cstr("caf\xC3\xA9"); // cafe with accent
    rt_string enc_utf8 = rt_codec_url_encode(utf8);
    test_result("UTF-8 bytes encoded", strcmp(rt_string_cstr(enc_utf8), "caf%c3%a9") == 0);

    const char nul_bytes[] = {'a', '\0', ' ', 'b'};
    rt_string with_nul = rt_string_from_bytes(nul_bytes, sizeof(nul_bytes));
    rt_string enc_nul = rt_codec_url_encode(with_nul);
    test_result("Embedded NUL byte is encoded", strcmp(rt_string_cstr(enc_nul), "a%00%20b") == 0);

    printf("\n");
}

static void test_url_decode_basic() {
    printf("Testing Codec.UrlDecode basic:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string dec_empty = rt_codec_url_decode(empty);
    test_result("Empty string decodes to empty", strcmp(rt_string_cstr(dec_empty), "") == 0);

    // No encoding passes through
    rt_string plain = rt_const_cstr("hello");
    rt_string dec_plain = rt_codec_url_decode(plain);
    test_result("Plain text unchanged", strcmp(rt_string_cstr(dec_plain), "hello") == 0);

    // Percent decoding
    rt_string encoded = rt_const_cstr("hello%20world");
    rt_string dec_encoded = rt_codec_url_decode(encoded);
    test_result("%20 decodes to space", strcmp(rt_string_cstr(dec_encoded), "hello world") == 0);

    // Plus as space
    rt_string plus = rt_const_cstr("hello+world");
    rt_string dec_plus = rt_codec_url_decode(plus);
    test_result("+ decodes to space", strcmp(rt_string_cstr(dec_plus), "hello world") == 0);

    // Multiple encodings (uppercase and lowercase both work)
    rt_string multi = rt_const_cstr("key%3dvalue%26other%3dtest");
    rt_string dec_multi = rt_codec_url_decode(multi);
    test_result("Multiple encodings decoded",
                strcmp(rt_string_cstr(dec_multi), "key=value&other=test") == 0);

    // Case insensitive hex
    rt_string upper = rt_const_cstr("hello%2Fworld");
    rt_string lower = rt_const_cstr("hello%2fworld");
    rt_string dec_upper = rt_codec_url_decode(upper);
    rt_string dec_lower = rt_codec_url_decode(lower);
    test_result("Uppercase hex decoded", strcmp(rt_string_cstr(dec_upper), "hello/world") == 0);
    test_result("Lowercase hex decoded", strcmp(rt_string_cstr(dec_lower), "hello/world") == 0);

    // Invalid percent sequence passes through
    rt_string invalid = rt_const_cstr("100%");
    rt_string dec_invalid = rt_codec_url_decode(invalid);
    test_result("Trailing % passes through", strcmp(rt_string_cstr(dec_invalid), "100%") == 0);

    rt_string invalid2 = rt_const_cstr("100%2");
    rt_string dec_invalid2 = rt_codec_url_decode(invalid2);
    test_result("Incomplete %X passes through", strcmp(rt_string_cstr(dec_invalid2), "100%2") == 0);

    rt_string invalid3 = rt_const_cstr("100%GH");
    rt_string dec_invalid3 = rt_codec_url_decode(invalid3);
    test_result("Invalid hex %GH passes through",
                strcmp(rt_string_cstr(dec_invalid3), "100%GH") == 0);

    rt_string nul_encoded = rt_const_cstr("a%00%20b");
    rt_string dec_nul = rt_codec_url_decode(nul_encoded);
    const char nul_bytes[] = {'a', '\0', ' ', 'b'};
    test_result("Embedded NUL byte is decoded", bytes_eq(dec_nul, nul_bytes, sizeof(nul_bytes)));

    printf("\n");
}

static void test_url_roundtrip() {
    printf("Testing URL encode/decode roundtrip:\n");

    const char *test_strings[] = {"",
                                  "hello",
                                  "hello world",
                                  "key=value&other=test",
                                  "http://example.com/path?query=value#anchor",
                                  "!@#$%^&*()_+{}|:\"<>?",
                                  "\x01\x02\x03\x7F\x80\xFF",
                                  NULL};

    bool all_passed = true;
    for (int i = 0; test_strings[i] != NULL; i++) {
        rt_string original = rt_const_cstr(test_strings[i]);
        rt_string encoded = rt_codec_url_encode(original);
        rt_string decoded = rt_codec_url_decode(encoded);
        if (strcmp(rt_string_cstr(original), rt_string_cstr(decoded)) != 0) {
            all_passed = false;
            break;
        }
    }
    test_result("All roundtrips preserve original", all_passed);

    printf("\n");
}

static void test_url_encode_ascii_unreserved_only() {
    printf("Testing Codec.UrlEncode ASCII-only unreserved set:\n");

    const unsigned char high_byte_raw[] = {0xE9u};
    const char *high_byte = reinterpret_cast<const char *>(high_byte_raw);
    rt_string high = rt_string_from_bytes(high_byte, sizeof(high_byte_raw));
    test_result("High byte is percent-encoded",
                strcmp(rt_string_cstr(rt_codec_url_encode(high)), "%e9") == 0);

    const char *saved_locale = setlocale(LC_CTYPE, NULL);
    std::string saved = saved_locale ? saved_locale : "C";
    const char *candidates[] = {
        "fr_FR.UTF-8",
        "de_DE.UTF-8",
        "en_US.UTF-8",
        "French_France.1252",
        "German_Germany.1252",
        NULL,
    };

    bool changed_locale = false;
    for (int i = 0; candidates[i] != NULL; ++i) {
        if (setlocale(LC_CTYPE, candidates[i]) != NULL) {
            changed_locale = true;
            break;
        }
    }

    if (changed_locale) {
        test_result("High byte remains encoded outside C locale",
                    strcmp(rt_string_cstr(rt_codec_url_encode(high)), "%e9") == 0);
        setlocale(LC_CTYPE, saved.c_str());
    } else {
        test_result("Non-C character locale unavailable", true);
    }

    printf("\n");
}

//=============================================================================
// Base64 Encoding Tests
//=============================================================================

static void test_base64_encode() {
    printf("Testing Codec.Base64Enc:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string enc_empty = rt_codec_base64_enc(empty);
    test_result("Empty string encodes to empty", strcmp(rt_string_cstr(enc_empty), "") == 0);

    // Standard test vectors from RFC 4648
    rt_string f = rt_const_cstr("f");
    test_result("'f' -> 'Zg=='", strcmp(rt_string_cstr(rt_codec_base64_enc(f)), "Zg==") == 0);

    rt_string fo = rt_const_cstr("fo");
    test_result("'fo' -> 'Zm8='", strcmp(rt_string_cstr(rt_codec_base64_enc(fo)), "Zm8=") == 0);

    rt_string foo = rt_const_cstr("foo");
    test_result("'foo' -> 'Zm9v'", strcmp(rt_string_cstr(rt_codec_base64_enc(foo)), "Zm9v") == 0);

    rt_string foob = rt_const_cstr("foob");
    test_result("'foob' -> 'Zm9vYg=='",
                strcmp(rt_string_cstr(rt_codec_base64_enc(foob)), "Zm9vYg==") == 0);

    rt_string fooba = rt_const_cstr("fooba");
    test_result("'fooba' -> 'Zm9vYmE='",
                strcmp(rt_string_cstr(rt_codec_base64_enc(fooba)), "Zm9vYmE=") == 0);

    rt_string foobar = rt_const_cstr("foobar");
    test_result("'foobar' -> 'Zm9vYmFy'",
                strcmp(rt_string_cstr(rt_codec_base64_enc(foobar)), "Zm9vYmFy") == 0);

    // "Hello" test
    rt_string hello = rt_const_cstr("Hello");
    test_result("'Hello' -> 'SGVsbG8='",
                strcmp(rt_string_cstr(rt_codec_base64_enc(hello)), "SGVsbG8=") == 0);

    const char nul_bytes[] = {'a', '\0', 'b'};
    rt_string with_nul = rt_string_from_bytes(nul_bytes, sizeof(nul_bytes));
    test_result("'a\\0b' -> 'YQBi'",
                strcmp(rt_string_cstr(rt_codec_base64_enc(with_nul)), "YQBi") == 0);

    printf("\n");
}

static void test_base64_decode() {
    printf("Testing Codec.Base64Dec:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string dec_empty = rt_codec_base64_dec(empty);
    test_result("Empty string decodes to empty", strcmp(rt_string_cstr(dec_empty), "") == 0);

    // Standard test vectors from RFC 4648
    rt_string zg = rt_const_cstr("Zg==");
    test_result("'Zg==' -> 'f'", strcmp(rt_string_cstr(rt_codec_base64_dec(zg)), "f") == 0);

    rt_string zm8 = rt_const_cstr("Zm8=");
    test_result("'Zm8=' -> 'fo'", strcmp(rt_string_cstr(rt_codec_base64_dec(zm8)), "fo") == 0);

    rt_string zm9v = rt_const_cstr("Zm9v");
    test_result("'Zm9v' -> 'foo'", strcmp(rt_string_cstr(rt_codec_base64_dec(zm9v)), "foo") == 0);

    rt_string zm9vyg = rt_const_cstr("Zm9vYg==");
    test_result("'Zm9vYg==' -> 'foob'",
                strcmp(rt_string_cstr(rt_codec_base64_dec(zm9vyg)), "foob") == 0);

    rt_string zm9vyme = rt_const_cstr("Zm9vYmE=");
    test_result("'Zm9vYmE=' -> 'fooba'",
                strcmp(rt_string_cstr(rt_codec_base64_dec(zm9vyme)), "fooba") == 0);

    rt_string zm9vymy = rt_const_cstr("Zm9vYmFy");
    test_result("'Zm9vYmFy' -> 'foobar'",
                strcmp(rt_string_cstr(rt_codec_base64_dec(zm9vymy)), "foobar") == 0);

    // "Hello" test
    rt_string hello_b64 = rt_const_cstr("SGVsbG8=");
    test_result("'SGVsbG8=' -> 'Hello'",
                strcmp(rt_string_cstr(rt_codec_base64_dec(hello_b64)), "Hello") == 0);

    rt_string nul_b64 = rt_const_cstr("YQBi");
    const char nul_bytes[] = {'a', '\0', 'b'};
    test_result("'YQBi' -> 'a\\0b'",
                bytes_eq(rt_codec_base64_dec(nul_b64), nul_bytes, sizeof(nul_bytes)));

    printf("\n");
}

static void test_base64_roundtrip() {
    printf("Testing Base64 encode/decode roundtrip:\n");

    const char *test_strings[] = {"",
                                  "a",
                                  "ab",
                                  "abc",
                                  "abcd",
                                  "Hello, World!",
                                  "The quick brown fox jumps over the lazy dog.",
                                  "\x01\x02\x03\x04",
                                  NULL};

    bool all_passed = true;
    for (int i = 0; test_strings[i] != NULL; i++) {
        rt_string original = rt_string_from_bytes(test_strings[i], strlen(test_strings[i]));
        rt_string encoded = rt_codec_base64_enc(original);
        rt_string decoded = rt_codec_base64_dec(encoded);
        if (strcmp(rt_string_cstr(original), rt_string_cstr(decoded)) != 0) {
            all_passed = false;
            break;
        }
    }
    test_result("All roundtrips preserve original", all_passed);

    const char nul_bytes[] = {'a', '\0', 'b'};
    rt_string original = rt_string_from_bytes(nul_bytes, sizeof(nul_bytes));
    rt_string encoded = rt_codec_base64_enc(original);
    rt_string decoded = rt_codec_base64_dec(encoded);
    test_result("Embedded NUL roundtrip preserves all bytes",
                bytes_eq(decoded, nul_bytes, sizeof(nul_bytes)));

    printf("\n");
}

//=============================================================================
// Hex Encoding Tests
//=============================================================================

static void test_hex_encode() {
    printf("Testing Codec.HexEnc:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string enc_empty = rt_codec_hex_enc(empty);
    test_result("Empty string encodes to empty", strcmp(rt_string_cstr(enc_empty), "") == 0);

    // Simple tests
    rt_string a = rt_const_cstr("a");
    test_result("'a' -> '61'", strcmp(rt_string_cstr(rt_codec_hex_enc(a)), "61") == 0);

    rt_string hello = rt_const_cstr("Hello");
    test_result("'Hello' -> '48656c6c6f'",
                strcmp(rt_string_cstr(rt_codec_hex_enc(hello)), "48656c6c6f") == 0);

    rt_string binary = rt_const_cstr("\xFF\x10\x20");
    test_result("High-byte chars -> 'ff1020'",
                strcmp(rt_string_cstr(rt_codec_hex_enc(binary)), "ff1020") == 0);

    const char nul_bytes[] = {'a', '\0', 'b'};
    rt_string with_nul = rt_string_from_bytes(nul_bytes, sizeof(nul_bytes));
    test_result("'a\\0b' -> '610062'",
                strcmp(rt_string_cstr(rt_codec_hex_enc(with_nul)), "610062") == 0);

    printf("\n");
}

static void test_hex_decode() {
    printf("Testing Codec.HexDec:\n");

    // Empty string
    rt_string empty = rt_const_cstr("");
    rt_string dec_empty = rt_codec_hex_dec(empty);
    test_result("Empty string decodes to empty", strcmp(rt_string_cstr(dec_empty), "") == 0);

    // Simple tests
    rt_string hex_a = rt_const_cstr("61");
    test_result("'61' -> 'a'", strcmp(rt_string_cstr(rt_codec_hex_dec(hex_a)), "a") == 0);

    rt_string hex_hello = rt_const_cstr("48656c6c6f");
    test_result("'48656c6c6f' -> 'Hello'",
                strcmp(rt_string_cstr(rt_codec_hex_dec(hex_hello)), "Hello") == 0);

    // Uppercase hex
    rt_string hex_upper = rt_const_cstr("48656C6C6F");
    test_result("Uppercase hex decodes",
                strcmp(rt_string_cstr(rt_codec_hex_dec(hex_upper)), "Hello") == 0);

    // Mixed case
    rt_string hex_mixed = rt_const_cstr("48656c6C6f");
    test_result("Mixed case hex decodes",
                strcmp(rt_string_cstr(rt_codec_hex_dec(hex_mixed)), "Hello") == 0);

    rt_string hex_nul = rt_const_cstr("610062");
    const char nul_bytes[] = {'a', '\0', 'b'};
    test_result("'610062' -> 'a\\0b'",
                bytes_eq(rt_codec_hex_dec(hex_nul), nul_bytes, sizeof(nul_bytes)));

    printf("\n");
}

static void test_hex_roundtrip() {
    printf("Testing Hex encode/decode roundtrip:\n");

    const char *test_strings[] = {"", "a", "ab", "Hello, World!", "\x01\x02\xFF", NULL};

    bool all_passed = true;
    for (int i = 0; test_strings[i] != NULL; i++) {
        rt_string original = rt_string_from_bytes(test_strings[i], strlen(test_strings[i]));
        rt_string encoded = rt_codec_hex_enc(original);
        rt_string decoded = rt_codec_hex_dec(encoded);
        if (strcmp(rt_string_cstr(original), rt_string_cstr(decoded)) != 0) {
            all_passed = false;
            break;
        }
    }
    test_result("All roundtrips preserve original", all_passed);

    const char nul_bytes[] = {'a', '\0', 'b'};
    rt_string original = rt_string_from_bytes(nul_bytes, sizeof(nul_bytes));
    rt_string encoded = rt_codec_hex_enc(original);
    rt_string decoded = rt_codec_hex_dec(encoded);
    test_result("Embedded NUL hex roundtrip preserves all bytes",
                bytes_eq(decoded, nul_bytes, sizeof(nul_bytes)));

    printf("\n");
}

//=============================================================================
// Entry Point
//=============================================================================

int main() {
    printf("=== RT Codec Tests ===\n\n");

    test_utf8_validation();

    // URL encoding tests
    test_url_encode_basic();
    test_url_decode_basic();
    test_url_roundtrip();
    test_url_encode_ascii_unreserved_only();

    // Base64 encoding tests
    test_base64_encode();
    test_base64_decode();
    test_base64_roundtrip();

    // Hex encoding tests
    test_hex_encode();
    test_hex_decode();
    test_hex_roundtrip();

    printf("All Codec tests passed!\n");
    return 0;
}
