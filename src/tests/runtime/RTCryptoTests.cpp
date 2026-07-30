//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTCryptoTests.cpp
// Purpose: Validate low- and high-level hashing, KDF, AEAD, key agreement,
//          password, random, and crypto-policy behavior.
// Key invariants:
//   - Results match published vectors and authenticated decryption fails closed.
//   - Invalid lengths, handles, and policy identifiers cannot mutate output or
//     bypass the selected crypto mode.
// Ownership/Lifetime:
//   - Tests release managed runtime values they create; native test buffers are
//     stack-owned and never escape their test.
// Links: src/runtime/network/rt_crypto.c,
//        docs/zannalib/crypto.md
//
//===----------------------------------------------------------------------===//

#include "rt_aes.h"
#include "rt_bytes.h"
#include "rt_cipher.h"
#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_ecdsa_p256.h"
#include "rt_hash.h"
#include "rt_keyderive.h"
#include "rt_password.h"
#include "rt_rand.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);
extern "C" void rt_abort(const char *msg);

namespace {
static bool g_returning_trap_enabled = false;
static int g_returning_trap_count = 0;
static char g_returning_trap_last[256];
} // namespace

extern "C" void vm_trap(const char *msg) {
    if (!g_returning_trap_enabled)
        rt_abort(msg);
    g_returning_trap_count++;
    snprintf(
        g_returning_trap_last, sizeof(g_returning_trap_last), "%s", msg ? msg : "Unknown trap");
}

static void reset_returning_trap_probe() {
    g_returning_trap_enabled = true;
    g_returning_trap_count = 0;
    g_returning_trap_last[0] = '\0';
}

static void finish_returning_trap_probe() {
    g_returning_trap_enabled = false;
}

static bool returning_trap_seen(const char *message_substr) {
    return g_returning_trap_count == 1 && message_substr &&
           strstr(g_returning_trap_last, message_substr) != nullptr;
}

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

template <typename Fn> static bool expect_trap(Fn fn, const char *message_substr) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn();
        rt_trap_clear_recovery();
        return false;
    }

    const char *message = rt_trap_get_error();
    bool matched = !message_substr || (message && strstr(message, message_substr) != nullptr);
    rt_trap_clear_recovery();
    return matched;
}

/// @brief Create a Bytes object from raw data.
static void *make_bytes(const uint8_t *data, size_t len) {
    void *bytes = rt_bytes_new((int64_t)len);
    for (size_t i = 0; i < len; i++) {
        rt_bytes_set(bytes, (int64_t)i, data[i]);
    }
    return bytes;
}

/// @brief Create a Bytes object from a C string.
static void *make_bytes_str(const char *str) {
    return make_bytes((const uint8_t *)str, strlen(str));
}

static rt_string make_string_raw(const uint8_t *data, size_t len) {
    return rt_string_from_bytes((const char *)data, len);
}

static bool bytes_equal(void *bytes, const uint8_t *expected, size_t len) {
    if (!bytes || rt_bytes_len(bytes) != (int64_t)len)
        return false;
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)rt_bytes_get(bytes, (int64_t)i) != expected[i])
            return false;
    }
    return true;
}

static bool bytes_are_value(const uint8_t *bytes, size_t len, uint8_t value) {
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != value)
            return false;
    }
    return true;
}

//=============================================================================
// HMAC-MD5 Tests (RFC 2202)
//=============================================================================

static void test_hmac_md5() {
    printf("Testing Hash.HmacMD5:\n");

    // Test 1: key = 0x0b repeated 16 times, data = "Hi There"
    // Expected: 9294727a3638bb1c13f48ef8158bfc9d
    {
        uint8_t key_data[16];
        memset(key_data, 0x0b, 16);
        void *key = make_bytes(key_data, 16);
        void *data = make_bytes_str("Hi There");

        rt_string result = rt_hash_hmac_md5_bytes(key, data);
        test_result("HMAC-MD5 Test 1",
                    strcmp(rt_string_cstr(result), "9294727a3638bb1c13f48ef8158bfc9d") == 0);
    }

    // Test 2: key = "Jefe", data = "what do ya want for nothing?"
    // Expected: 750c783e6ab0b503eaa86e310a5db738
    {
        rt_string key = rt_const_cstr("Jefe");
        rt_string data = rt_const_cstr("what do ya want for nothing?");

        rt_string result = rt_hash_hmac_md5(key, data);
        test_result("HMAC-MD5 Test 2",
                    strcmp(rt_string_cstr(result), "750c783e6ab0b503eaa86e310a5db738") == 0);
    }

    // Test 3: key = 0xaa repeated 16 times, data = 0xdd repeated 50 times
    // Expected: 56be34521d144c88dbb8c733f0e8b3f6
    {
        uint8_t key_data[16];
        memset(key_data, 0xaa, 16);
        uint8_t data_bytes[50];
        memset(data_bytes, 0xdd, 50);

        void *key = make_bytes(key_data, 16);
        void *data = make_bytes(data_bytes, 50);

        rt_string result = rt_hash_hmac_md5_bytes(key, data);
        test_result("HMAC-MD5 Test 3",
                    strcmp(rt_string_cstr(result), "56be34521d144c88dbb8c733f0e8b3f6") == 0);
    }

    printf("\n");
}

//=============================================================================
// HMAC-SHA1 Tests (RFC 2202)
//=============================================================================

static void test_hmac_sha1() {
    printf("Testing Hash.HmacSHA1:\n");

    // Test 1: key = 0x0b repeated 20 times, data = "Hi There"
    // Expected: b617318655057264e28bc0b6fb378c8ef146be00
    {
        uint8_t key_data[20];
        memset(key_data, 0x0b, 20);
        void *key = make_bytes(key_data, 20);
        void *data = make_bytes_str("Hi There");

        rt_string result = rt_hash_hmac_sha1_bytes(key, data);
        test_result("HMAC-SHA1 Test 1",
                    strcmp(rt_string_cstr(result), "b617318655057264e28bc0b6fb378c8ef146be00") ==
                        0);
    }

    // Test 2: key = "Jefe", data = "what do ya want for nothing?"
    // Expected: effcdf6ae5eb2fa2d27416d5f184df9c259a7c79
    {
        rt_string key = rt_const_cstr("Jefe");
        rt_string data = rt_const_cstr("what do ya want for nothing?");

        rt_string result = rt_hash_hmac_sha1(key, data);
        test_result("HMAC-SHA1 Test 2",
                    strcmp(rt_string_cstr(result), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79") ==
                        0);
    }

    // Test 3: key = 0xaa repeated 20 times, data = 0xdd repeated 50 times
    // Expected: 125d7342b9ac11cd91a39af48aa17b4f63f175d3
    {
        uint8_t key_data[20];
        memset(key_data, 0xaa, 20);
        uint8_t data_bytes[50];
        memset(data_bytes, 0xdd, 50);

        void *key = make_bytes(key_data, 20);
        void *data = make_bytes(data_bytes, 50);

        rt_string result = rt_hash_hmac_sha1_bytes(key, data);
        test_result("HMAC-SHA1 Test 3",
                    strcmp(rt_string_cstr(result), "125d7342b9ac11cd91a39af48aa17b4f63f175d3") ==
                        0);
    }

    printf("\n");
}

//=============================================================================
// HMAC-SHA256 Tests (RFC 4231)
//=============================================================================

static void test_hmac_sha256() {
    printf("Testing Hash.HmacSHA256:\n");

    // Test 1: key = 0x0b repeated 20 times, data = "Hi There"
    // Expected: b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
    {
        uint8_t key_data[20];
        memset(key_data, 0x0b, 20);
        void *key = make_bytes(key_data, 20);
        void *data = make_bytes_str("Hi There");

        rt_string result = rt_hash_hmac_sha256_bytes(key, data);
        test_result("HMAC-SHA256 Test 1",
                    strcmp(rt_string_cstr(result),
                           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") ==
                        0);
    }

    // Test 2: key = "Jefe", data = "what do ya want for nothing?"
    // Expected: 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
    {
        rt_string key = rt_const_cstr("Jefe");
        rt_string data = rt_const_cstr("what do ya want for nothing?");

        rt_string result = rt_hash_hmac_sha256(key, data);
        test_result("HMAC-SHA256 Test 2",
                    strcmp(rt_string_cstr(result),
                           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") ==
                        0);
    }

    // Test 3: key = 0xaa repeated 20 times, data = 0xdd repeated 50 times
    // Expected: 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe
    {
        uint8_t key_data[20];
        memset(key_data, 0xaa, 20);
        uint8_t data_bytes[50];
        memset(data_bytes, 0xdd, 50);

        void *key = make_bytes(key_data, 20);
        void *data = make_bytes(data_bytes, 50);

        rt_string result = rt_hash_hmac_sha256_bytes(key, data);
        test_result("HMAC-SHA256 Test 3",
                    strcmp(rt_string_cstr(result),
                           "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe") ==
                        0);
    }

    // Test 4: Long key (longer than block size - gets hashed)
    // key = 0xaa repeated 131 times
    // data = "Test Using Larger Than Block-Size Key - Hash Key First"
    // Expected: 60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54
    {
        uint8_t key_data[131];
        memset(key_data, 0xaa, 131);
        void *key = make_bytes(key_data, 131);
        void *data = make_bytes_str("Test Using Larger Than Block-Size Key - Hash Key First");

        rt_string result = rt_hash_hmac_sha256_bytes(key, data);
        test_result("HMAC-SHA256 Test 4 (long key)",
                    strcmp(rt_string_cstr(result),
                           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") ==
                        0);
    }

    printf("\n");
}

static void test_hmac_sha384_sha512() {
    printf("Testing HMAC-SHA384/SHA512:\n");

    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    const char data[] = "Hi There";
    uint8_t mac384[48];
    uint8_t mac512[64];
    static const uint8_t exp384[48] = {0xaf, 0xd0, 0x39, 0x44, 0xd8, 0x48, 0x95, 0x62, 0x6b, 0x08,
                                       0x25, 0xf4, 0xab, 0x46, 0x90, 0x7f, 0x15, 0xf9, 0xda, 0xdb,
                                       0xe4, 0x10, 0x1e, 0xc6, 0x82, 0xaa, 0x03, 0x4c, 0x7c, 0xeb,
                                       0xc5, 0x9c, 0xfa, 0xea, 0x9e, 0xa9, 0x07, 0x6e, 0xde, 0x7f,
                                       0x4a, 0xf1, 0x52, 0xe8, 0xb2, 0xfa, 0x9c, 0xb6};
    static const uint8_t exp512[64] = {
        0x87, 0xaa, 0x7c, 0xde, 0xa5, 0xef, 0x61, 0x9d, 0x4f, 0xf0, 0xb4, 0x24, 0x1a,
        0x1d, 0x6c, 0xb0, 0x23, 0x79, 0xf4, 0xe2, 0xce, 0x4e, 0xc2, 0x78, 0x7a, 0xd0,
        0xb3, 0x05, 0x45, 0xe1, 0x7c, 0xde, 0xda, 0xa8, 0x33, 0xb7, 0xd6, 0xb8, 0xa7,
        0x02, 0x03, 0x8b, 0x27, 0x4e, 0xae, 0xa3, 0xf4, 0xe4, 0xbe, 0x9d, 0x91, 0x4e,
        0xeb, 0x61, 0xf1, 0x70, 0x2e, 0x69, 0x6c, 0x20, 0x3a, 0x12, 0x68, 0x54};

    rt_hmac_sha384(key, sizeof(key), data, strlen(data), mac384);
    rt_hmac_sha512(key, sizeof(key), data, strlen(data), mac512);
    test_result("HMAC-SHA384 RFC 4231 vector", memcmp(mac384, exp384, sizeof(exp384)) == 0);
    test_result("HMAC-SHA512 RFC 4231 vector", memcmp(mac512, exp512, sizeof(exp512)) == 0);
    printf("\n");
}

static void test_sha256_incremental_matches_one_shot() {
    printf("Testing SHA-256 incremental API:\n");

    static const char payload[] =
        "The quick brown fox jumps over the lazy dog while the TLS transcript grows.";
    uint8_t one_shot[32];
    uint8_t incremental[32];

    rt_sha256(payload, sizeof(payload) - 1, one_shot);

    rt_sha256_ctx ctx;
    rt_sha256_init(&ctx);
    rt_sha256_update(&ctx, payload, 13);
    rt_sha256_update(&ctx, payload + 13, 21);
    rt_sha256_update(&ctx, payload + 34, sizeof(payload) - 1 - 34);
    rt_sha256_final(&ctx, incremental);

    test_result("SHA-256 incremental matches one-shot", memcmp(one_shot, incremental, 32) == 0);
    printf("\n");
}

static void test_sha_finalization_boundaries() {
    printf("Testing SHA finalization boundaries:\n");

    rt_sha256_ctx boundary_ctx;
    rt_sha256_init(&boundary_ctx);
    memset(boundary_ctx.buffer, 0, sizeof(boundary_ctx.buffer));
    boundary_ctx.count = std::numeric_limits<uint64_t>::max() - 7u;

    uint8_t boundary_digest[32];
    memset(boundary_digest, 0xa5, sizeof(boundary_digest));
    reset_returning_trap_probe();
    rt_sha256_final(&boundary_ctx, boundary_digest);
    bool boundary_ok = g_returning_trap_count == 0 &&
                       !bytes_are_value(boundary_digest, sizeof(boundary_digest), 0xa5);
    finish_returning_trap_probe();
    test_result("SHA-256 final padding does not overflow the message counter", boundary_ok);

    bool oversized_ok = true;
    if (std::numeric_limits<size_t>::max() > std::numeric_limits<uint64_t>::max() / UINT64_C(8)) {
        uint8_t input = 0;
        uint8_t digest[32];
        memset(digest, 0xa5, sizeof(digest));
        size_t oversized =
            static_cast<size_t>(std::numeric_limits<uint64_t>::max() / UINT64_C(8) + UINT64_C(1));
        reset_returning_trap_probe();
        rt_sha256(&input, oversized, digest);
        oversized_ok =
            returning_trap_seen("input too large") && bytes_are_value(digest, sizeof(digest), 0xa5);
        finish_returning_trap_probe();
    }
    test_result("SHA-256 one-shot stops after an oversized returning trap", oversized_ok);
    printf("\n");
}

static void test_sha_hmac_null_zero_inputs() {
    printf("Testing SHA/HMAC NULL zero-length inputs:\n");

    uint8_t sha_empty[64];
    uint8_t sha_null[64];
    uint8_t mac_empty[64];
    uint8_t mac_null[64];

    rt_sha256("", 0, sha_empty);
    rt_sha256(NULL, 0, sha_null);
    test_result("SHA-256 accepts NULL with zero length", memcmp(sha_empty, sha_null, 32) == 0);

    rt_sha384("", 0, sha_empty);
    rt_sha384(NULL, 0, sha_null);
    test_result("SHA-384 accepts NULL with zero length", memcmp(sha_empty, sha_null, 48) == 0);

    rt_sha512("", 0, sha_empty);
    rt_sha512(NULL, 0, sha_null);
    test_result("SHA-512 accepts NULL with zero length", memcmp(sha_empty, sha_null, 64) == 0);

    rt_hmac_sha256((const uint8_t *)"", 0, (const uint8_t *)"", 0, mac_empty);
    rt_hmac_sha256(NULL, 0, NULL, 0, mac_null);
    test_result("HMAC-SHA256 accepts NULL key/data with zero lengths",
                memcmp(mac_empty, mac_null, 32) == 0);

    rt_hmac_sha512((const uint8_t *)"", 0, (const uint8_t *)"", 0, mac_empty);
    rt_hmac_sha512(NULL, 0, NULL, 0, mac_null);
    test_result("HMAC-SHA512 accepts NULL key/data with zero lengths",
                memcmp(mac_empty, mac_null, 64) == 0);
    printf("\n");
}

static void test_string_inputs_preserve_embedded_nul() {
    printf("Testing string APIs preserve embedded NUL bytes:\n");

    uint8_t msg_data[] = {'a', 0, 'b'};
    uint8_t key_data[] = {'k', 0, 'y'};
    rt_string msg = make_string_raw(msg_data, sizeof(msg_data));
    rt_string key = make_string_raw(key_data, sizeof(key_data));
    void *msg_bytes = make_bytes(msg_data, sizeof(msg_data));
    void *key_bytes = make_bytes(key_data, sizeof(key_data));

    test_result("MD5 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_md5(msg)),
                       rt_string_cstr(rt_hash_md5_bytes(msg_bytes))) == 0);
    test_result("SHA1 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_sha1(msg)),
                       rt_string_cstr(rt_hash_sha1_bytes(msg_bytes))) == 0);
    test_result("SHA256 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_sha256(msg)),
                       rt_string_cstr(rt_hash_sha256_bytes(msg_bytes))) == 0);
    test_result("CRC32 string matches Bytes", rt_hash_crc32(msg) == rt_hash_crc32_bytes(msg_bytes));
    test_result("Fast hash string matches Bytes",
                rt_hash_fast(msg) == rt_hash_fast_bytes(msg_bytes));

    test_result("HMAC-MD5 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_hmac_md5(key, msg)),
                       rt_string_cstr(rt_hash_hmac_md5_bytes(key_bytes, msg_bytes))) == 0);
    test_result("HMAC-SHA1 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_hmac_sha1(key, msg)),
                       rt_string_cstr(rt_hash_hmac_sha1_bytes(key_bytes, msg_bytes))) == 0);
    test_result("HMAC-SHA256 string matches Bytes",
                strcmp(rt_string_cstr(rt_hash_hmac_sha256(key, msg)),
                       rt_string_cstr(rt_hash_hmac_sha256_bytes(key_bytes, msg_bytes))) == 0);

    printf("\n");
}

//=============================================================================
// PBKDF2-SHA256 Tests (RFC 6070 extended)
//=============================================================================

static void test_pbkdf2_sha256() {
    printf("Testing KeyDerive.Pbkdf2SHA256:\n");

    // Test 1: password="password", salt="salt", iterations=100000, dkLen=32
    // Note: RFC 6070 uses SHA1, but we're using SHA256
    // Expected from hashlib.pbkdf2_hmac("sha256", ...).
    {
        rt_string password = rt_const_cstr("password");
        void *salt = make_bytes_str("salt");

        rt_string result = rt_keyderive_pbkdf2_sha256_str(password, salt, 100000, 32);
        test_result("PBKDF2-SHA256 password/salt/100000/32",
                    strcmp(rt_string_cstr(result),
                           "0394a2ede332c9a13eb82e9b24631604c31df978b4e2f0fbd2c549944f9d79a5") ==
                        0);
    }

    // Test 2: password="passwordPASSWORDpassword",
    // salt="saltSALTsaltSALTsaltSALTsaltSALTsalt", iterations=100000, dkLen=40
    {
        rt_string password = rt_const_cstr("passwordPASSWORDpassword");
        void *salt = make_bytes_str("saltSALTsaltSALTsaltSALTsaltSALTsalt");

        rt_string result = rt_keyderive_pbkdf2_sha256_str(password, salt, 100000, 40);
        test_result("PBKDF2-SHA256 long password/salt/100000/40",
                    strcmp(rt_string_cstr(result),
                           "af70dc8ce4ccc6d39e35080f4af755133b266f3a8da78983844e1caf1fb9f76d9d70"
                           "fe5b9bd9ec71") == 0);
    }

    // Test 3: Returns Bytes object
    {
        rt_string password = rt_const_cstr("test");
        void *salt = make_bytes_str("salt");

        void *result = rt_keyderive_pbkdf2_sha256(password, salt, 100000, 16);
        test_result("PBKDF2-SHA256 returns Bytes", result != nullptr);
        test_result("PBKDF2-SHA256 Bytes has correct length", rt_bytes_len(result) == 16);
    }

    // Test 4: Embedded NUL bytes in passwords are significant
    {
        uint8_t full_password[] = {'p', 'a', 's', 's', 0, 'w', 'o', 'r', 'd'};
        rt_string password_full = make_string_raw(full_password, sizeof(full_password));
        rt_string password_prefix = rt_const_cstr("pass");
        void *salt = make_bytes_str("salt");

        rt_string full = rt_keyderive_pbkdf2_sha256_str(password_full, salt, 100000, 16);
        rt_string prefix = rt_keyderive_pbkdf2_sha256_str(password_prefix, salt, 100000, 16);
        test_result("PBKDF2 treats embedded NUL as password data",
                    strcmp(rt_string_cstr(full), rt_string_cstr(prefix)) != 0);
    }

    // Test 5: scrypt-SHA256 matches RFC 7914 test vector
    {
        rt_string password = rt_const_cstr("password");
        void *salt = make_bytes_str("NaCl");

        rt_string result = rt_keyderive_scrypt_sha256_str(password, salt, 1024, 8, 16, 64);
        test_result("scrypt-SHA256 RFC 7914 vector",
                    strcmp(rt_string_cstr(result),
                           "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
                           "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0"
                           "640") == 0);
    }

    printf("\n");
}

static void test_crypto_input_validation() {
    printf("Testing Crypto input validation:\n");

    void *salt = make_bytes_str("salt");
    rt_string empty = rt_const_cstr("");
    rt_string empty_hash = rt_hash_sha256(empty);
    test_result("Hash accepts real empty strings",
                strcmp(rt_string_cstr(empty_hash),
                       "e3b0c44298fc1c149afbf4c8996fb924"
                       "27ae41e4649b934ca495991b7852b855") == 0);
    test_result("PBKDF2 accepts real empty password strings",
                rt_keyderive_pbkdf2_sha256_str(empty, salt, 100000, 16) != nullptr);
    test_result(
        "Hash rejects NULL string input",
        expect_trap([]() { (void)rt_hash_sha256(nullptr); }, "Hash: string must not be null"));
    test_result(
        "KeyDerive rejects NULL password",
        expect_trap([&]() { (void)rt_keyderive_pbkdf2_sha256_str(nullptr, salt, 100000, 16); },
                    "KeyDerive: password must not be null"));
    test_result("Password.Hash rejects NULL password",
                expect_trap([]() { (void)rt_password_hash(nullptr); }, "password is null"));
    test_result("Password.HashIters rejects NULL password",
                expect_trap([]() { (void)rt_password_hash_with_iterations(nullptr, 100000); },
                            "password is null"));
    test_result("Password.HashScryptParams rejects NULL password",
                expect_trap([]() { (void)rt_password_hash_scrypt_params(nullptr, 16384, 8, 1); },
                            "password is null"));

    uint8_t zero_key[16] = {0};
    uint8_t zero_iv[16] = {0};
    void *aes_key = make_bytes(zero_key, sizeof(zero_key));
    void *aes_iv = make_bytes(zero_iv, sizeof(zero_iv));
    test_result("AES CBC rejects NULL plaintext",
                expect_trap([&]() { (void)rt_aes_encrypt(nullptr, aes_key, aes_iv); },
                            "plaintext is null"));
    test_result("AES CBC rejects NULL ciphertext",
                expect_trap([&]() { (void)rt_aes_decrypt(nullptr, aes_key, aes_iv); },
                            "ciphertext is null"));
    test_result("AES auth rejects NULL plaintext",
                expect_trap([&]() { (void)rt_aes_encrypt_auth(nullptr, aes_key, nullptr); },
                            "plaintext is null"));
    test_result("AES auth rejects NULL ciphertext",
                expect_trap([&]() { (void)rt_aes_decrypt_auth(nullptr, aes_key, nullptr); },
                            "ciphertext is null"));
    test_result("AES string encrypt rejects NULL plaintext",
                expect_trap([&]() { (void)rt_aes_encrypt_str(nullptr, rt_const_cstr("pw")); },
                            "plaintext is null"));
    test_result(
        "AES string encrypt rejects NULL password",
        expect_trap([&]() { (void)rt_aes_encrypt_str(empty, nullptr); }, "password is null"));
    test_result("AES string decrypt rejects NULL ciphertext",
                expect_trap([&]() { (void)rt_aes_decrypt_str(nullptr, rt_const_cstr("pw")); },
                            "encrypted data is null"));

    rt_crypto_random_bytes(nullptr, 0);
    test_result("Crypto random allows NULL zero-length output", true);
    test_result(
        "Crypto random rejects NULL positive-length output",
        expect_trap([]() { rt_crypto_random_bytes(nullptr, 1); }, "random output buffer is null"));

    uint8_t prk256[32] = {0};
    uint8_t prk384[48] = {0};
    test_result("HKDF-SHA256 allows NULL zero-length OKM",
                rt_hkdf_expand(prk256, nullptr, 0, nullptr, 0) == 0);
    test_result("HKDF-SHA384 allows NULL zero-length OKM",
                rt_hkdf_expand_sha384(prk384, nullptr, 0, nullptr, 0) == 0);
    test_result("HKDF-Expand-Label allows NULL zero-length OKM",
                rt_hkdf_expand_label(prk256, "empty", nullptr, 0, nullptr, 0) == 0);
    test_result("HKDF-Expand-Label-SHA384 allows NULL zero-length OKM",
                rt_hkdf_expand_label_sha384(prk384, "empty", nullptr, 0, nullptr, 0) == 0);

    printf("\n");
}

static void test_returning_trap_crypto_guards() {
    printf("Testing returning trap crypto guards:\n");

    void *salt = make_bytes_str("salt");
    void *plain = make_bytes_str("plain");
    void *not_bytes = (void *)rt_const_cstr("not bytes");
    uint8_t zero_key_data[16] = {0};
    uint8_t zero_iv_data[16] = {0};
    void *aes_key = make_bytes(zero_key_data, sizeof(zero_key_data));
    void *aes_iv = make_bytes(zero_iv_data, sizeof(zero_iv_data));

    reset_returning_trap_probe();
    rt_string hash = rt_hash_sha256(nullptr);
    bool hash_ok = returning_trap_seen("Hash: string must not be null") &&
                   strcmp(rt_string_cstr(hash), "") == 0;
    finish_returning_trap_probe();
    test_result("Hash NULL string stops after returning trap", hash_ok);

    reset_returning_trap_probe();
    rt_string invalid_string_hash = rt_hash_sha256((rt_string)plain);
    bool invalid_string_ok = returning_trap_seen("invalid string handle") &&
                             strcmp(rt_string_cstr(invalid_string_hash), "") == 0;
    finish_returning_trap_probe();
    test_result("Hash invalid string handle stops after returning trap", invalid_string_ok);

    reset_returning_trap_probe();
    rt_string byte_hash = rt_hash_sha256_bytes(not_bytes);
    bool byte_hash_ok =
        returning_trap_seen("invalid Bytes object") && strcmp(rt_string_cstr(byte_hash), "") == 0;
    finish_returning_trap_probe();
    test_result("Hash invalid Bytes stops after returning trap", byte_hash_ok);

    reset_returning_trap_probe();
    rt_string key = rt_keyderive_pbkdf2_sha256_str(nullptr, salt, 100000, 16);
    bool key_ok =
        returning_trap_seen("password must not be null") && strcmp(rt_string_cstr(key), "") == 0;
    finish_returning_trap_probe();
    test_result("KeyDerive NULL password stops after returning trap", key_ok);

    reset_returning_trap_probe();
    rt_string pwd_hash = rt_password_hash_with_iterations(nullptr, 100000);
    bool pwd_hash_ok =
        returning_trap_seen("password is null") && strcmp(rt_string_cstr(pwd_hash), "") == 0;
    finish_returning_trap_probe();
    test_result("Password.HashIters NULL password stops after returning trap", pwd_hash_ok);

    reset_returning_trap_probe();
    void *cipher = rt_cipher_encrypt_with_key_aad(plain, not_bytes, nullptr);
    bool cipher_ok = returning_trap_seen("key must be a Bytes object") && cipher == nullptr;
    finish_returning_trap_probe();
    test_result("Cipher invalid key Bytes stops after returning trap", cipher_ok);

    reset_returning_trap_probe();
    void *aes_cipher = rt_aes_encrypt(not_bytes, aes_key, aes_iv);
    bool aes_ok = returning_trap_seen("plaintext must be a Bytes object") && aes_cipher == nullptr;
    finish_returning_trap_probe();
    test_result("AES CBC invalid plaintext stops after returning trap", aes_ok);

    reset_returning_trap_probe();
    void *aes_str_cipher = rt_aes_encrypt_str(nullptr, rt_const_cstr("pw"));
    bool aes_str_ok = returning_trap_seen("plaintext is null") && aes_str_cipher == nullptr;
    finish_returning_trap_probe();
    test_result("AES string NULL plaintext stops after returning trap", aes_str_ok);

    reset_returning_trap_probe();
    uint8_t digest[32];
    memset(digest, 0xa5, sizeof(digest));
    rt_sha256(nullptr, 1, digest);
    bool sha_ok = returning_trap_seen("input buffer is null") &&
                  bytes_are_value(digest, sizeof(digest), 0xa5);
    finish_returning_trap_probe();
    test_result("SHA one-shot preserves output after returning trap", sha_ok);

    reset_returning_trap_probe();
    uint8_t ikm[1] = {1};
    uint8_t prk[32];
    memset(prk, 0xa5, sizeof(prk));
    rt_hkdf_extract(nullptr, 1, ikm, sizeof(ikm), prk);
    bool hkdf_ok =
        returning_trap_seen("salt buffer is null") && bytes_are_value(prk, sizeof(prk), 0xa5);
    finish_returning_trap_probe();
    test_result("HKDF extract preserves output after returning trap", hkdf_ok);

    printf("\n");
}

static void test_constant_time_equals_and_passwords() {
    printf("Testing constant-time compare and Password:\n");

    rt_string mac1 = rt_const_cstr("abcdef012345");
    rt_string mac2 = rt_const_cstr("abcdef012345");
    rt_string mac3 = rt_const_cstr("abcdef012346");
    test_result("Hash.ConstantTimeEquals accepts equal strings",
                rt_hash_constant_time_equals(mac1, mac2) == 1);
    test_result("Hash.ConstantTimeEquals rejects different strings",
                rt_hash_constant_time_equals(mac1, mac3) == 0);

    void *b1 = make_bytes_str("tag");
    void *b2 = make_bytes_str("tag");
    void *b3 = make_bytes_str("tags");
    test_result("Hash.ConstantTimeEqualsBytes accepts equal bytes",
                rt_hash_constant_time_equals_bytes(b1, b2) == 1);
    test_result("Hash.ConstantTimeEqualsBytes rejects length mismatch",
                rt_hash_constant_time_equals_bytes(b1, b3) == 0);

    rt_string hash = rt_password_hash_scrypt_params(rt_const_cstr("secret"), 16384, 8, 1);
    test_result("Password.HashScryptParams verifies",
                rt_password_verify(rt_const_cstr("secret"), hash) == 1);
    test_result("Password.Verify rejects wrong password",
                rt_password_verify(rt_const_cstr("wrong"), hash) == 0);
    test_result("Password.NeedsRehash accepts current scrypt params",
                rt_password_needs_rehash(hash) == 0);

    rt_string weak_scrypt = rt_const_cstr("SCRYPT$4$1$1$AAAAAAAAAAAAAAAAAAAAAA==$"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    test_result("Password.NeedsRehash flags weak stored scrypt params",
                rt_password_needs_rehash(weak_scrypt) == 1);

    rt_string pbkdf2 = rt_password_hash_with_iterations(rt_const_cstr("secret"), 100000);
    test_result("Password.Verify accepts legacy PBKDF2",
                rt_password_verify(rt_const_cstr("secret"), pbkdf2) == 1);
    test_result("Password.NeedsRehash flags PBKDF2", rt_password_needs_rehash(pbkdf2) == 1);

    printf("\n");
}

static void test_high_level_aead_wrappers() {
    printf("Testing high-level AEAD wrappers:\n");

    const uint8_t plain_data[] = {'s', 'e', 'c', 'r', 'e', 't'};
    void *plain = make_bytes(plain_data, sizeof(plain_data));
    void *aad = make_bytes_str("context");
    void *wrong_aad = make_bytes_str("other");

    void *cipher = rt_cipher_encrypt_aad(plain, rt_const_cstr("password"), aad);
    test_result("Cipher.EncryptAAD uses versioned format",
                cipher && rt_bytes_len(cipher) > 4 && rt_bytes_get(cipher, 0) == 'V' &&
                    rt_bytes_get(cipher, 1) == 'C' && rt_bytes_get(cipher, 2) == 'P' &&
                    rt_bytes_get(cipher, 3) == '2');
    void *opened = rt_cipher_decrypt_aad(cipher, rt_const_cstr("password"), aad);
    test_result("Cipher.DecryptAAD round-trips",
                bytes_equal(opened, plain_data, sizeof(plain_data)));
    test_result("Cipher.DecryptAAD rejects wrong AAD",
                rt_cipher_decrypt_aad(cipher, rt_const_cstr("password"), wrong_aad) == nullptr);

    void *key = rt_cipher_generate_key();
    void *key_cipher = rt_cipher_encrypt_with_key_aad(plain, key, aad);
    test_result("Cipher.EncryptWithKeyAAD uses versioned format",
                key_cipher && rt_bytes_len(key_cipher) > 4 && rt_bytes_get(key_cipher, 0) == 'V' &&
                    rt_bytes_get(key_cipher, 1) == 'C' && rt_bytes_get(key_cipher, 2) == 'K' &&
                    rt_bytes_get(key_cipher, 3) == '2');
    void *key_opened = rt_cipher_decrypt_with_key_aad(key_cipher, key, aad);
    test_result("Cipher.DecryptWithKeyAAD round-trips",
                bytes_equal(key_opened, plain_data, sizeof(plain_data)));
    test_result("Cipher.DecryptWithKeyAAD rejects wrong AAD",
                rt_cipher_decrypt_with_key_aad(key_cipher, key, wrong_aad) == nullptr);

    uint8_t aes_key_data[16];
    memset(aes_key_data, 0x42, sizeof(aes_key_data));
    void *aes_key = make_bytes(aes_key_data, sizeof(aes_key_data));
    void *aes_cipher = rt_aes_encrypt_auth(plain, aes_key, aad);
    test_result("Aes.EncryptAuth uses versioned format",
                aes_cipher && rt_bytes_len(aes_cipher) > 4 && rt_bytes_get(aes_cipher, 0) == 'V' &&
                    rt_bytes_get(aes_cipher, 1) == 'A' && rt_bytes_get(aes_cipher, 2) == 'K' &&
                    rt_bytes_get(aes_cipher, 3) == '1');
    void *aes_opened = rt_aes_decrypt_auth(aes_cipher, aes_key, aad);
    test_result("Aes.DecryptAuth round-trips",
                bytes_equal(aes_opened, plain_data, sizeof(plain_data)));
    test_result("Aes.DecryptAuth rejects wrong AAD",
                rt_aes_decrypt_auth(aes_cipher, aes_key, wrong_aad) == nullptr);

    uint8_t aes256_key_data[32];
    memset(aes256_key_data, 0x24, sizeof(aes256_key_data));
    void *aes256_key = make_bytes(aes256_key_data, sizeof(aes256_key_data));
    void *aes256_cipher = rt_aes_encrypt_auth(plain, aes256_key, aad);
    void *aes256_opened = rt_aes_decrypt_auth(aes256_cipher, aes256_key, aad);
    test_result("Aes.EncryptAuth accepts AES-256 key",
                bytes_equal(aes256_opened, plain_data, sizeof(plain_data)));

    uint8_t cbc_key_data[16];
    uint8_t cbc_iv_data[16];
    memset(cbc_key_data, 0x31, sizeof(cbc_key_data));
    memset(cbc_iv_data, 0x7c, sizeof(cbc_iv_data));
    void *cbc_key = make_bytes(cbc_key_data, sizeof(cbc_key_data));
    void *cbc_iv = make_bytes(cbc_iv_data, sizeof(cbc_iv_data));
    void *empty_plain = make_bytes(nullptr, 0);
    void *cbc_cipher = rt_aes_encrypt(empty_plain, cbc_key, cbc_iv);
    void *cbc_opened = rt_aes_decrypt(cbc_cipher, cbc_key, cbc_iv);
    test_result("Aes.CBC round-trips empty plaintext", cbc_opened && rt_bytes_len(cbc_opened) == 0);

    printf("\n");
}

//=============================================================================
// Secure Random Tests
//=============================================================================

static void test_crypto_rand() {
    printf("Testing Rand:\n");

    // Test 1: Bytes returns correct length
    {
        void *bytes = rt_crypto_rand_bytes(32);
        test_result("Rand.Bytes returns correct length", rt_bytes_len(bytes) == 32);
    }

    // Test 1b: Bytes(0) returns an empty Bytes object
    {
        void *bytes = rt_crypto_rand_bytes(0);
        test_result("Rand.Bytes(0) returns empty Bytes", rt_bytes_len(bytes) == 0);
    }

    // Test 2: Multiple calls produce different results
    {
        void *bytes1 = rt_crypto_rand_bytes(16);
        void *bytes2 = rt_crypto_rand_bytes(16);

        bool different = false;
        for (int i = 0; i < 16; i++) {
            if (rt_bytes_get(bytes1, i) != rt_bytes_get(bytes2, i)) {
                different = true;
                break;
            }
        }
        test_result("Rand.Bytes produces different results", different);
    }

    // Test 3: Int returns values in range
    {
        bool all_in_range = true;
        for (int i = 0; i < 100; i++) {
            int64_t val = rt_crypto_rand_int(10, 20);
            if (val < 10 || val > 20) {
                all_in_range = false;
                break;
            }
        }
        test_result("Rand.Int returns values in range [10, 20]", all_in_range);
    }

    // Test 4: Int with min == max returns that value
    {
        int64_t val = rt_crypto_rand_int(42, 42);
        test_result("Rand.Int with min==max returns that value", val == 42);
    }

    // Test 5: Int produces variety (not always same value)
    {
        std::set<int64_t> values;
        for (int i = 0; i < 50; i++) {
            values.insert(rt_crypto_rand_int(0, 100));
        }
        // Should have at least 10 different values in 50 tries
        test_result("Rand.Int produces variety", values.size() >= 10);
    }

    // Test 6: Int with negative range
    {
        bool all_in_range = true;
        for (int i = 0; i < 100; i++) {
            int64_t val = rt_crypto_rand_int(-100, -50);
            if (val < -100 || val > -50) {
                all_in_range = false;
                break;
            }
        }
        test_result("Rand.Int with negative range", all_in_range);
    }

    // Test 7: Int with range spanning zero
    {
        bool saw_negative = false;
        bool saw_positive = false;
        for (int i = 0; i < 100; i++) {
            int64_t val = rt_crypto_rand_int(-10, 10);
            if (val < 0)
                saw_negative = true;
            if (val > 0)
                saw_positive = true;
        }
        test_result("Rand.Int spanning zero produces negatives and positives",
                    saw_negative && saw_positive);
    }

    // Test 8: Int handles ranges that used to overflow signed arithmetic
    {
        bool ok = true;
        for (int i = 0; i < 20; i++) {
            int64_t neg = rt_crypto_rand_int(std::numeric_limits<int64_t>::min(), -1);
            int64_t pos = rt_crypto_rand_int(0, std::numeric_limits<int64_t>::max());
            (void)rt_crypto_rand_int(std::numeric_limits<int64_t>::min(),
                                     std::numeric_limits<int64_t>::max());
            if (neg > -1 || pos < 0) {
                ok = false;
                break;
            }
        }
        test_result("Rand.Int handles extreme signed ranges", ok);
    }

    printf("\n");
}

//=============================================================================
// AEAD / Key Exchange Tests
//=============================================================================

static void test_aead_tamper_detection() {
    printf("Testing AEAD tamper detection:\n");

    {
        static const uint8_t key[32] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                                        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
                                        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
                                        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
        static const uint8_t nonce[12] = {
            0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
        static const uint8_t aad[12] = {
            0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
        static const char plaintext[] =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
            "for the future, sunscreen would be it.";
        static const uint8_t expected[] = {
            0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc, 0x53,
            0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe, 0xa9, 0xe2,
            0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67,
            0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b, 0x1a, 0x71, 0xde, 0x0a,
            0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36, 0x92,
            0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c, 0x98, 0x03, 0xae, 0xe3, 0x28, 0x09,
            0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80,
            0x8b, 0x48, 0x31, 0xd7, 0xbc, 0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
            0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16, 0x1a, 0xe1, 0x0b,
            0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
        uint8_t ciphertext[sizeof(expected)];
        uint8_t opened[sizeof(plaintext) - 1];

        size_t cipher_len = rt_chacha20_poly1305_encrypt(
            key, nonce, aad, sizeof(aad), plaintext, sizeof(plaintext) - 1, ciphertext);
        test_result("ChaCha20-Poly1305 RFC 8439 vector encrypt",
                    cipher_len == sizeof(expected) &&
                        memcmp(ciphertext, expected, sizeof(expected)) == 0);
        test_result("ChaCha20-Poly1305 RFC 8439 vector decrypt",
                    rt_chacha20_poly1305_decrypt(
                        key, nonce, aad, sizeof(aad), ciphertext, sizeof(ciphertext), opened) ==
                            (long)sizeof(opened) &&
                        memcmp(opened, plaintext, sizeof(opened)) == 0);
    }

    {
        uint8_t key[32];
        uint8_t nonce[12];
        memset(key, 0x11, sizeof(key));
        memset(nonce, 0x22, sizeof(nonce));
        const char *msg = "ChaCha20-Poly1305";
        uint8_t ciphertext[sizeof("ChaCha20-Poly1305") - 1 + 16];
        uint8_t plaintext[sizeof("ChaCha20-Poly1305") - 1];

        size_t cipher_len =
            rt_chacha20_poly1305_encrypt(key, nonce, nullptr, 0, msg, strlen(msg), ciphertext);
        test_result("ChaCha20 encrypt length matches", cipher_len == strlen(msg) + 16);
        ciphertext[cipher_len - 1] ^= 0x01;
        test_result("ChaCha20 tamper detected",
                    rt_chacha20_poly1305_decrypt(
                        key, nonce, nullptr, 0, ciphertext, cipher_len, plaintext) < 0);
        test_result("ChaCha20 rejects NULL AAD with nonzero length",
                    rt_chacha20_poly1305_encrypt(
                        key, nonce, nullptr, 1, msg, strlen(msg), ciphertext) == 0);
        uint8_t tag_only[16];
        test_result("ChaCha20 handles NULL zero-length plaintext",
                    rt_chacha20_poly1305_encrypt(key, nonce, nullptr, 0, nullptr, 0, tag_only) ==
                        sizeof(tag_only));
        test_result("ChaCha20 decrypts tag-only ciphertext with NULL plaintext",
                    rt_chacha20_poly1305_decrypt(
                        key, nonce, nullptr, 0, tag_only, sizeof(tag_only), nullptr) == 0);
    }

    {
        uint8_t key[16];
        uint8_t nonce[12];
        memset(key, 0x33, sizeof(key));
        memset(nonce, 0x44, sizeof(nonce));
        const char *msg = "AES-GCM";
        uint8_t ciphertext[sizeof("AES-GCM") - 1 + 16];
        uint8_t plaintext[sizeof("AES-GCM") - 1];

        size_t cipher_len =
            rt_aes128_gcm_encrypt(key, nonce, nullptr, 0, msg, strlen(msg), ciphertext);
        test_result("AES-GCM encrypt length matches", cipher_len == strlen(msg) + 16);
        ciphertext[0] ^= 0x80;
        test_result(
            "AES-GCM tamper detected",
            rt_aes128_gcm_decrypt(key, nonce, nullptr, 0, ciphertext, cipher_len, plaintext) < 0);
        test_result("AES-GCM rejects NULL AAD with nonzero length",
                    rt_aes128_gcm_encrypt(key, nonce, nullptr, 1, msg, strlen(msg), ciphertext) ==
                        0);
    }

    {
        uint8_t key[32] = {0};
        uint8_t nonce[12] = {0};
        uint8_t plaintext[16] = {0};
        uint8_t ciphertext[32];
        uint8_t opened[16];
        static const uint8_t expected[32] = {0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
                                             0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18,
                                             0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
                                             0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};
        test_result("AES-256-GCM vector encrypt",
                    rt_aes256_gcm_encrypt(
                        key, nonce, nullptr, 0, plaintext, sizeof(plaintext), ciphertext) ==
                            sizeof(ciphertext) &&
                        memcmp(ciphertext, expected, sizeof(expected)) == 0);
        test_result("AES-256-GCM vector decrypt",
                    rt_aes256_gcm_decrypt(
                        key, nonce, nullptr, 0, ciphertext, sizeof(ciphertext), opened) == 16 &&
                        memcmp(opened, plaintext, sizeof(plaintext)) == 0);
    }

    printf("\n");
}

static void test_aead_rejects_unrepresentable_tagged_lengths() {
    uint8_t key128[16] = {0};
    uint8_t key256[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t byte = 0;
    const size_t impossible = std::numeric_limits<size_t>::max();

    test_result("ChaCha20 rejects plaintext length whose tag would wrap",
                rt_chacha20_poly1305_encrypt(key256, nonce, nullptr, 0, &byte, impossible, &byte) ==
                    0);
    test_result("AES-128-GCM rejects plaintext length whose tag would wrap",
                rt_aes128_gcm_encrypt(key128, nonce, nullptr, 0, &byte, impossible, &byte) == 0);
    test_result("AES-256-GCM rejects plaintext length whose tag would wrap",
                rt_aes256_gcm_encrypt(key256, nonce, nullptr, 0, &byte, impossible, &byte) == 0);
    printf("\n");
}

static void test_x25519_shared_secret_agreement() {
    printf("Testing X25519 shared secret agreement:\n");

    uint8_t alice_secret[32], alice_public[32];
    uint8_t bob_secret[32], bob_public[32];
    uint8_t shared1[32], shared2[32];

    rt_x25519_keygen(alice_secret, alice_public);
    rt_x25519_keygen(bob_secret, bob_public);
    test_result("Alice X25519 succeeds", rt_x25519(alice_secret, bob_public, shared1) == 0);
    test_result("Bob X25519 succeeds", rt_x25519(bob_secret, alice_public, shared2) == 0);

    test_result("Shared secrets match", memcmp(shared1, shared2, sizeof(shared1)) == 0);

    uint8_t zeros[32] = {0};
    test_result("Shared secret is non-zero", memcmp(shared1, zeros, sizeof(shared1)) != 0);
    test_result("All-zero peer public key is rejected",
                rt_x25519(alice_secret, zeros, shared1) != 0);
    printf("\n");
}

static void test_p256_ecdh_shared_secret_agreement() {
    printf("Testing P-256 ECDH shared secret agreement:\n");

    uint8_t alice_priv[32] = {0};
    uint8_t bob_priv[32] = {0};
    alice_priv[31] = 1;
    bob_priv[31] = 2;

    uint8_t alice_x[32], alice_y[32], bob_x[32], bob_y[32];
    uint8_t shared1[32], shared2[32];
    test_result("Alice P-256 public key",
                ecdsa_p256_public_from_private(alice_priv, alice_x, alice_y) == 1);
    test_result("Bob P-256 public key",
                ecdsa_p256_public_from_private(bob_priv, bob_x, bob_y) == 1);
    test_result("Alice P-256 ECDH succeeds",
                ecdsa_p256_ecdh(alice_priv, bob_x, bob_y, shared1) == 1);
    test_result("Bob P-256 ECDH succeeds",
                ecdsa_p256_ecdh(bob_priv, alice_x, alice_y, shared2) == 1);
    test_result("P-256 shared secrets match", memcmp(shared1, shared2, sizeof(shared1)) == 0);

    uint8_t bad_y[32];
    memcpy(bad_y, bob_y, sizeof(bad_y));
    bad_y[31] ^= 1;
    test_result("P-256 ECDH rejects invalid peer point",
                ecdsa_p256_ecdh(alice_priv, bob_x, bad_y, shared1) == 0);

    static const uint8_t p256_p_bytes[32] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
                                             0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t digest[32] = {0};
    uint8_t sig_r[32] = {0};
    uint8_t sig_s[32] = {0};
    digest[31] = 1;
    sig_r[31] = 1;
    sig_s[31] = 1;
    test_result("P-256 ECDH rejects non-canonical peer x",
                ecdsa_p256_ecdh(alice_priv, p256_p_bytes, bob_y, shared1) == 0);
    test_result("P-256 verify rejects non-canonical public x",
                ecdsa_p256_verify(p256_p_bytes, bob_y, digest, sig_r, sig_s) == 0);
    printf("\n");
}

static void test_crypto_module_approved_mode() {
    printf("Testing Crypto.Module approved mode:\n");

    test_result("Module self-test passes", rt_crypto_module_self_test() == 1);
    rt_crypto_module_mode_t original_mode = rt_crypto_module_get_mode();
    test_result("Invalid module mode is rejected",
                rt_crypto_module_set_mode(static_cast<rt_crypto_module_mode_t>(99)) == 0);
    test_result("Invalid module mode preserves prior policy",
                rt_crypto_module_get_mode() == original_mode);
    test_result("Unknown crypto service is denied",
                rt_crypto_module_service_allowed(static_cast<rt_crypto_module_service_t>(99)) == 0);
    test_result("Enable approved mode succeeds", rt_crypto_module_enable_approved_mode() == 1);
    test_result("Approved mode flag is set", rt_crypto_module_is_approved_mode() == 1);
    test_result("AES-GCM is allowed",
                rt_crypto_module_service_allowed(RT_CRYPTO_SERVICE_AES_GCM) == 1);
    test_result("ChaCha20-Poly1305 is rejected",
                rt_crypto_module_service_allowed(RT_CRYPTO_SERVICE_CHACHA20_POLY1305) == 0);
    test_result("scrypt is rejected",
                rt_crypto_module_service_allowed(RT_CRYPTO_SERVICE_SCRYPT) == 0);

    uint8_t random_probe[70000];
    memset(random_probe, 0, sizeof(random_probe));
    rt_crypto_module_random_bytes(random_probe, sizeof(random_probe));
    bool any_random = false;
    for (size_t i = 0; i < sizeof(random_probe); i++) {
        if (random_probe[i] != 0) {
            any_random = true;
            break;
        }
    }
    test_result("Approved DRBG handles chunked large requests", any_random);

    const uint8_t plain_data[] = {'a', 'p', 'p', 'r', 'o', 'v', 'e', 'd'};
    void *plain = make_bytes(plain_data, sizeof(plain_data));
    void *aad = make_bytes_str("approved-context");
    void *cipher = rt_cipher_encrypt_aad(plain, rt_const_cstr("password"), aad);
    test_result("Cipher approved password format uses AES magic",
                cipher && rt_bytes_len(cipher) > 4 && rt_bytes_get(cipher, 0) == 'V' &&
                    rt_bytes_get(cipher, 1) == 'C' && rt_bytes_get(cipher, 2) == 'A' &&
                    rt_bytes_get(cipher, 3) == '1');
    void *opened = rt_cipher_decrypt_aad(cipher, rt_const_cstr("password"), aad);
    test_result("Cipher approved password round-trips",
                bytes_equal(opened, plain_data, sizeof(plain_data)));

    void *key = rt_cipher_generate_key();
    void *key_cipher = rt_cipher_encrypt_with_key_aad(plain, key, aad);
    test_result("Cipher approved key format uses AES magic",
                key_cipher && rt_bytes_len(key_cipher) > 4 && rt_bytes_get(key_cipher, 0) == 'V' &&
                    rt_bytes_get(key_cipher, 1) == 'K' && rt_bytes_get(key_cipher, 2) == 'A' &&
                    rt_bytes_get(key_cipher, 3) == '1');
    void *key_opened = rt_cipher_decrypt_with_key_aad(key_cipher, key, aad);
    test_result("Cipher approved key round-trips",
                bytes_equal(key_opened, plain_data, sizeof(plain_data)));

    rt_string approved_hash = rt_password_hash(rt_const_cstr("secret"));
    test_result("Password.Hash uses PBKDF2 in approved mode",
                strncmp(rt_string_cstr(approved_hash), "PBKDF2$", 7) == 0);
    test_result("Approved PBKDF2 password verifies",
                rt_password_verify(rt_const_cstr("secret"), approved_hash) == 1);
    test_result("Approved PBKDF2 password is current",
                rt_password_needs_rehash(approved_hash) == 0);

    rt_crypto_module_disable_approved_mode();
    test_result("Approved mode flag is cleared", rt_crypto_module_is_approved_mode() == 0);
    printf("\n");
}

//=============================================================================
// Entry Point
//=============================================================================

int main() {
    printf("=== RT Crypto Tests ===\n\n");

    test_hmac_md5();
    test_hmac_sha1();
    test_hmac_sha256();
    test_hmac_sha384_sha512();
    test_sha256_incremental_matches_one_shot();
    test_sha_finalization_boundaries();
    test_sha_hmac_null_zero_inputs();
    test_string_inputs_preserve_embedded_nul();
    test_pbkdf2_sha256();
    test_crypto_input_validation();
    test_returning_trap_crypto_guards();
    test_constant_time_equals_and_passwords();
    test_high_level_aead_wrappers();
    test_crypto_rand();
    test_aead_tamper_detection();
    test_aead_rejects_unrepresentable_tagged_lengths();
    test_x25519_shared_secret_agreement();
    test_p256_ecdh_shared_secret_agreement();
    test_crypto_module_approved_mode();

    printf("All Crypto tests passed!\n");
    return 0;
}
