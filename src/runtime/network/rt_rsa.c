//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_rsa.c
// Purpose: Native RSA key parsing, Montgomery modular exponentiation, and
//          PKCS#1 v1.5 / RSA-PSS signature verification for TLS 1.3 and X.509.
//          Implements arbitrary-precision big-integer arithmetic using 64-bit
//          limbs (big-endian), Montgomery multiplication, and Barrett-style
//          conditional subtraction for constant-time modular reduction.
// Key invariants:
//   - All big integers are stored big-endian (limb[0] = most-significant word).
//   - Modular exponentiation uses Montgomery form; no division in the hot loop.
//   - Signature verification avoids ordinary memcmp for digest/tag checks so
//     verification timing does not reveal the first mismatching byte.
//   - Maximum modulus: RT_RSA_MAX_MOD_BYTES (512) bytes = 4096-bit RSA.
// Ownership/Lifetime:
//   - rt_rsa_key_t members (modulus, exponents) are heap-allocated; caller
//     must call rt_rsa_key_free() to release.
// Links: rt_rsa.h (public API), rt_tls_verify.c (caller), rt_crypto.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_rsa.c
 * @brief Implements native RSA parsing and signature operations for TLS.
 * @details The implementation provides bounded big-integer arithmetic,
 *          Montgomery modular exponentiation, strict DER key parsing, and
 *          PKCS#1 v1.5 and PSS encoding verification. Sensitive temporary and
 *          private-key storage is scrubbed before release, while digest and
 *          padding comparisons avoid early mismatch exits.
 */

#include "rt_rsa.h"

#include "rt_crypto.h"

#include <stdlib.h>
#include <string.h>

#define RT_RSA_MIN_MOD_BYTES 128
#define RT_RSA_MAX_MOD_BYTES 512
#define RT_RSA_MAX_WORDS (RT_RSA_MAX_MOD_BYTES / sizeof(uint64_t))

/// @brief Volatile-pointer zeroing that the compiler cannot elide.
/// @details Used to scrub Montgomery scratch, decrypted intermediate
///          blocks, and private key material before they fall out of
///          scope. The volatile cast defeats dead-store elimination.
/// @param ptr Writable storage to scrub; valid for @p len bytes.
/// @param len Number of bytes to overwrite.
static void rsa_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Zero @p len bytes at @p ptr then free the buffer.
/// @details The companion `free`-with-scrub for heap allocations that
///          held sensitive material (decrypted ciphertext, padded
///          plaintext, key schedule extracts).
/// @param ptr Heap allocation to scrub and release, or NULL.
/// @param len Allocation length in bytes.
static void rsa_secure_free(uint8_t *ptr, size_t len) {
    if (ptr) {
        rsa_secure_zero(ptr, len);
        free(ptr);
    }
}

/// @brief Predicate: are all @p len bytes at @p data zero?
/// @details Constant-time bit-OR accumulator so an attacker cannot time
///          the function to infer where the first non-zero byte sits.
///          NULL pointer or zero length is treated as "all zero" so the
///          caller's "empty field rejected" guard fires consistently.
/// @param data Big-endian byte span to inspect.
/// @param len Number of bytes in @p data.
/// @return One when the span is absent, empty, or entirely zero; otherwise
///         zero.
static int rsa_be_is_zero(const uint8_t *data, size_t len) {
    uint8_t acc = 0;
    if (!data || len == 0)
        return 1;
    for (size_t i = 0; i < len; i++)
        acc |= data[i];
    return acc == 0;
}

/// @brief Constant-time equality check for equal-length byte strings.
/// @details XOR-accumulates every byte pair and returns only after scanning
///          the full range, preventing timing from revealing the first
///          mismatching byte. NULL inputs are treated as unequal unless
///          @p len is zero.
/// @param a First byte string.
/// @param b Second byte string.
/// @param len Number of bytes to compare.
/// @return 1 when all @p len bytes are equal, 0 otherwise.
static int rsa_ct_mem_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t acc = 0;
    if ((!a || !b) && len > 0)
        return 0;
    for (size_t i = 0; i < len; i++)
        acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

/// @brief Compare two unsigned big-endian byte arrays after trimming leading zeroes.
///
/// DER INTEGER encodings can include a leading 0x00 sign-protection byte.
/// Public component validation is about numeric magnitude, not raw encoded
/// length, so this helper strips those leading zero bytes before comparing
/// lengths and then byte values.
///
/// @param a     First big-endian integer byte array.
/// @param a_len Length of @p a in bytes.
/// @param b     Second big-endian integer byte array.
/// @param b_len Length of @p b in bytes.
/// @return -1 when a < b, 0 when equal, 1 when a > b.
static int rsa_be_cmp_trimmed(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    while (a_len > 0 && a && *a == 0) {
        a++;
        a_len--;
    }
    while (b_len > 0 && b && *b == 0) {
        b++;
        b_len--;
    }
    if (a_len < b_len)
        return -1;
    if (a_len > b_len)
        return 1;
    if (a_len == 0)
        return 0;
    int cmp = memcmp(a, b, a_len);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

/// @brief Validate that a parsed key has plausible public components.
/// @details Sanity checks the @c modulus + @c public_exponent pair before
///          we hand them to the modular-exponentiation core:
///          - Modulus length is in [128, 512] bytes (1024–4096-bit RSA).
///          - Modulus is non-zero and odd.
///          - Public exponent has length 1–8 bytes and ≤ modulus length.
///          - Public exponent is non-zero, odd, and >= 3.
/// @param key Parsed key whose public components are inspected.
/// @return 1 when every check passes, 0 otherwise.
static int rsa_validate_public_components(const rt_rsa_key_t *key) {
    if (!key || !key->modulus || !key->public_exponent)
        return 0;
    if (key->modulus_len < RT_RSA_MIN_MOD_BYTES || key->modulus_len > RT_RSA_MAX_MOD_BYTES)
        return 0;
    if (rsa_be_is_zero(key->modulus, key->modulus_len) ||
        (key->modulus[key->modulus_len - 1] & 1u) == 0)
        return 0;
    if (key->public_exponent_len == 0 || key->public_exponent_len > 8)
        return 0;
    if (rsa_be_is_zero(key->public_exponent, key->public_exponent_len) ||
        (key->public_exponent[key->public_exponent_len - 1] & 1u) == 0)
        return 0;
    if (rsa_be_cmp_trimmed(
            key->public_exponent, key->public_exponent_len, key->modulus, key->modulus_len) >= 0)
        return 0;
    if (key->public_exponent_len == 1 && key->public_exponent[0] < 3)
        return 0;
    return 1;
}

/// @brief Validate the private-component extension of @ref rsa_validate_public_components.
/// @details Requires the public checks to pass, then additionally
///          confirms @c private_exponent is present, non-zero, and
///          within the modulus byte length. Does not verify that
///          `d * e ≡ 1 (mod φ(n))` — that would require the CRT
///          factors, which the PKCS#1 / PKCS#8 parsers discard.
/// @param key Parsed key whose public and private components are inspected.
/// @return 1 when every check passes, 0 otherwise.
static int rsa_validate_private_components(const rt_rsa_key_t *key) {
    return rsa_validate_public_components(key) && key->private_exponent &&
           key->private_exponent_len > 0 && key->private_exponent_len <= key->modulus_len &&
           !rsa_be_is_zero(key->private_exponent, key->private_exponent_len);
}

/// @brief Compute (a * b + addend + carry_in) into a 128-bit pair (low, high).
/// @details The fundamental Montgomery-multiply micro-op: takes two 64-bit
///          limbs plus an additive carry chain and writes the 128-bit
///          product. Uses `unsigned __int128` on compilers that support
///          it (GCC/Clang) for a single hardware multiply; falls back to
///          four 32x32→64 partials on MSVC and other compilers without
///          128-bit integer support.
/// @param a First 64-bit multiplicand.
/// @param b Second 64-bit multiplicand.
/// @param addend Additive term, typically the previous accumulator limb.
/// @param carry_in Carry from the previous column.
/// @param low_out Destination for the low 64 result bits.
/// @param high_out Destination for the high 64 result bits.
static void rsa_mul_add_u64(uint64_t a,
                            uint64_t b,
                            uint64_t addend,
                            uint64_t carry_in,
                            uint64_t *low_out,
                            uint64_t *high_out) {
#if defined(__SIZEOF_INT128__)
    __extension__ unsigned __int128 acc =
        (unsigned __int128)a * (unsigned __int128)b + addend + carry_in;
    *low_out = (uint64_t)acc;
    *high_out = (uint64_t)(acc >> 64);
#else
    uint64_t p0 = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    uint64_t p1 = (uint64_t)(uint32_t)a * (b >> 32);
    uint64_t p2 = (a >> 32) * (uint64_t)(uint32_t)b;
    uint64_t p3 = (a >> 32) * (b >> 32);

    uint64_t acc0 = (p0 & 0xFFFFFFFFu) + (addend & 0xFFFFFFFFu) + (carry_in & 0xFFFFFFFFu);
    uint64_t word0 = acc0 & 0xFFFFFFFFu;
    uint64_t carry0 = acc0 >> 32;

    uint64_t acc1 = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu) + (addend >> 32) +
                    (carry_in >> 32) + carry0;
    uint64_t word1 = acc1 & 0xFFFFFFFFu;
    uint64_t carry1 = acc1 >> 32;

    uint64_t acc2 = (p1 >> 32) + (p2 >> 32) + (p3 & 0xFFFFFFFFu) + carry1;
    uint64_t word2 = acc2 & 0xFFFFFFFFu;
    uint64_t carry2 = acc2 >> 32;

    uint64_t acc3 = (p3 >> 32) + carry2;

    *low_out = word0 | (word1 << 32);
    *high_out = word2 | (acc3 << 32);
#endif
}

/// @brief Decode one DER tag-length-value header from the start of `buf`.
/// @details DER (Distinguished Encoding Rules) is the wire format
///          ASN.1 structures use for X.509 certificates and PKCS#1
///          RSA key blobs. Each TLV begins with a 1-byte tag, then
///          a length encoded as either:
///          - **Short form** (`0x00`–`0x7F`): the length byte itself.
///          - **Long form** (`0x80 | n`): `n` length bytes follow,
///            big-endian. We cap `n` at 4 (1 GB max length, more than
///            adequate for any TLS payload).
///          Returns 1 on success with `tag`, `val_len`, and `hdr_len`
///          filled. Returns 0 for malformed or truncated input.
///          Used by every higher-level DER walker in this file.
/// @param buf DER bytes beginning at a TLV header.
/// @param buf_len Available bytes at @p buf.
/// @param tag Destination for the decoded one-byte tag.
/// @param val_len Destination for the decoded content length.
/// @param hdr_len Destination for the tag-plus-length header size.
/// @return One for a canonical, fully contained DER TLV; otherwise zero.
static int rsa_der_read_tlv(
    const uint8_t *buf, size_t buf_len, uint8_t *tag, size_t *val_len, size_t *hdr_len) {
    if (!buf || buf_len < 2 || !tag || !val_len || !hdr_len)
        return 0;

    *tag = buf[0];
    if (buf[1] < 0x80) {
        *val_len = buf[1];
        *hdr_len = 2;
    } else {
        size_t num_len_bytes = (size_t)(buf[1] & 0x7F);
        size_t value = 0;
        if (num_len_bytes == 0 || num_len_bytes > sizeof(size_t) || 2 + num_len_bytes > buf_len ||
            buf[2] == 0x00)
            return 0;
        for (size_t i = 0; i < num_len_bytes; i++)
            value = (value << 8) | buf[2 + i];
        if (value < 128)
            return 0;
        *val_len = value;
        *hdr_len = 2 + num_len_bytes;
    }

    if (*hdr_len > buf_len || *val_len > buf_len - *hdr_len)
        return 0;
    return 1;
}

/// @brief Heap-duplicate a big-endian integer value, stripping any leading zero padding.
/// @details DER-encoded INTEGER values include a leading `0x00` byte
///          when the most-significant bit of the first content byte
///          is 1 — this prevents the value from being misread as
///          negative under DER's two's-complement convention. RSA
///          modulus and exponent values are unsigned, so we strip
///          that leading zero before storing them. Caller owns the
///          returned buffer (must `free`).
/// @param data Unsigned big-endian value bytes.
/// @param len Number of input bytes.
/// @param out_len Destination receiving the trimmed allocation length.
/// @return Caller-owned nonempty byte allocation, or NULL for invalid input or
///         allocation failure.
static uint8_t *rsa_dup_be_trimmed(const uint8_t *data, size_t len, size_t *out_len) {
    size_t start = 0;
    uint8_t *copy = NULL;

    if (!data || len == 0 || !out_len)
        return NULL;
    while (start + 1 < len && data[start] == 0x00)
        start++;
    len -= start;
    data += start;

    copy = (uint8_t *)malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, data, len);
    *out_len = len;
    return copy;
}

/// @brief Parse one DER INTEGER (`tag = 0x02`) from `der[*pos]` into a fresh buffer.
/// @details Wrapper that combines `rsa_der_read_tlv` (header decode +
///          tag check) with `rsa_dup_be_trimmed` (strip the
///          DER-mandated leading-zero padding). Advances `*pos` past
///          the consumed TLV on success. Returns 0 if the next field
///          isn't an INTEGER or if the buffer is truncated.
/// @param der Complete DER byte buffer.
/// @param der_len Number of bytes in @p der.
/// @param pos In/out offset of the next TLV.
/// @param out Destination receiving a caller-owned trimmed integer buffer.
/// @param out_len Destination receiving the integer buffer length.
/// @return One on canonical nonnegative INTEGER success; otherwise zero.
static int rsa_parse_der_integer(
    const uint8_t *der, size_t der_len, size_t *pos, uint8_t **out, size_t *out_len) {
    uint8_t tag;
    size_t value_len, hdr_len;

    if (!der || !pos || !out || !out_len || *pos >= der_len)
        return 0;
    if (!rsa_der_read_tlv(der + *pos, der_len - *pos, &tag, &value_len, &hdr_len) || tag != 0x02)
        return 0;
    if (value_len == 0)
        return 0;
    const uint8_t *value = der + *pos + hdr_len;
    if ((value[0] & 0x80u) != 0)
        return 0;
    if (value_len > 1 && value[0] == 0x00 && (value[1] & 0x80u) == 0)
        return 0;
    *out = rsa_dup_be_trimmed(der + *pos + hdr_len, value_len, out_len);
    if (!*out)
        return 0;
    *pos += hdr_len + value_len;
    return 1;
}

/// @brief Return the digest length for a supported RSA hash identifier.
/// @param hash_id SHA-2 algorithm selector.
/// @return 32, 48, or 64 bytes for SHA-256, SHA-384, or SHA-512;
///         otherwise zero.
static size_t rsa_hash_len(rt_rsa_hash_t hash_id) {
    switch (hash_id) {
        case RT_RSA_HASH_SHA256:
            return 32;
        case RT_RSA_HASH_SHA384:
            return 48;
        case RT_RSA_HASH_SHA512:
            return 64;
        default:
            return 0;
    }
}

/// @brief Dispatch a SHA-2 family hash by `hash_id` over the input buffer.
/// @details Routes to `rt_sha256` / `rt_sha384` / `rt_sha512` based on
///          the algorithm tag. Returns 0 for unknown identifiers
///          (caller treats as signature verification failure).
/// @param hash_id SHA-2 algorithm selector.
/// @param data Input byte span.
/// @param len Number of input bytes.
/// @param out Caller-provided digest buffer sized for @p hash_id.
/// @return One when a supported hash was computed; otherwise zero.
static int rsa_hash_buffer(rt_rsa_hash_t hash_id, const void *data, size_t len, uint8_t *out) {
    switch (hash_id) {
        case RT_RSA_HASH_SHA256:
            rt_sha256(data, len, out);
            return 1;
        case RT_RSA_HASH_SHA384:
            rt_sha384(data, len, out);
            return 1;
        case RT_RSA_HASH_SHA512:
            rt_sha512(data, len, out);
            return 1;
        default:
            return 0;
    }
}

/// @brief Return the bit-length of the RSA modulus (e.g. 2048, 3072, 4096).
/// @details Multiplies `modulus_len * 8`, then walks down by counting
///          leading zero bits in the most-significant byte. Used by
///          PSS encode/verify to compute `emLen` (the PSS-encoded
///          message length, which is `ceil((modBits - 1) / 8)`).
/// @param key Key containing the unsigned big-endian modulus.
/// @return Significant modulus width in bits, or zero for missing storage.
static size_t rsa_modulus_bits(const rt_rsa_key_t *key) {
    size_t bits = 0;
    uint8_t first = 0;

    if (!key || !key->modulus || key->modulus_len == 0)
        return 0;
    first = key->modulus[0];
    bits = key->modulus_len * 8;
    while ((first & 0x80u) == 0) {
        first <<= 1;
        bits--;
    }
    return bits;
}

/// @brief MGF1 mask generation function (RFC 8017 §B.2.1) — produces an arbitrary-length mask.
/// @details Stretches a fixed-size hash output to any desired mask
///          length by hashing `seed || INT32_BE(counter)` for
///          `counter = 0, 1, 2, ...` and concatenating the digests
///          until the requested length is filled. Used by RSA-PSS
///          to produce the salt mask and by OAEP to produce the
///          payload mask. The hash function is parameterized so
///          PSS/OAEP can pick a different MGF hash than the message
///          hash if needed (in practice they almost always match).
/// @param hash_id SHA-2 function used for mask expansion.
/// @param seed Seed byte span hashed with successive counters.
/// @param seed_len Number of seed bytes.
/// @param out Destination mask buffer.
/// @param out_len Number of mask bytes to produce.
/// @return One on success; zero for invalid inputs or an unsupported hash.
static int rsa_mgf1(
    rt_rsa_hash_t hash_id, const uint8_t *seed, size_t seed_len, uint8_t *out, size_t out_len) {
    uint8_t hash[64];
    uint8_t block[68];
    uint32_t counter = 0;
    size_t h_len = rsa_hash_len(hash_id);
    size_t pos = 0;

    if (!seed || !out || h_len == 0 || seed_len > sizeof(block) - 4)
        return 0;

    memcpy(block, seed, seed_len);
    while (pos < out_len) {
        block[seed_len + 0] = (uint8_t)(counter >> 24);
        block[seed_len + 1] = (uint8_t)(counter >> 16);
        block[seed_len + 2] = (uint8_t)(counter >> 8);
        block[seed_len + 3] = (uint8_t)counter;
        if (!rsa_hash_buffer(hash_id, block, seed_len + 4, hash))
            return 0;
        {
            size_t copy = out_len - pos;
            if (copy > h_len)
                copy = h_len;
            memcpy(out + pos, hash, copy);
            pos += copy;
        }
        counter++;
    }
    return 1;
}

/// @brief Round a byte length up to the required 64-bit limb count.
/// @param len Byte length to represent.
/// @return Number of 64-bit limbs required.
static size_t rsa_word_count_from_len(size_t len) {
    return (len + sizeof(uint64_t) - 1) / sizeof(uint64_t);
}

/// @brief Zero-fill a multi-word integer.
/// @param words Writable little-endian limb array.
/// @param count Number of 64-bit limbs to clear.
static void rsa_words_zero(uint64_t *words, size_t count) {
    memset(words, 0, count * sizeof(*words));
}

/// @brief Convert a big-endian byte buffer into the little-endian-limb internal representation.
/// @details All RSA values cross the wire as big-endian byte arrays
///          (network order), but our arithmetic uses little-endian
///          64-bit limbs (`out[0]` = least significant 8 bytes).
///          The walk reverses byte order while packing 8 bytes per
///          limb. Higher limbs are zero-filled if the input is
///          shorter than the internal representation needs.
/// @param data Unsigned big-endian input bytes.
/// @param len Number of input bytes.
/// @param out Destination little-endian limb array.
/// @param out_words Capacity of @p out in limbs.
static void rsa_words_from_be(const uint8_t *data, size_t len, uint64_t *out, size_t out_words) {
    rsa_words_zero(out, out_words);
    for (size_t i = 0; i < len; i++)
        out[i / 8] |= (uint64_t)data[len - 1 - i] << ((i % 8) * 8);
}

/// @brief Inverse of `rsa_words_from_be`: emit limbs as a big-endian byte buffer.
/// @details Pads / truncates to the requested output length (the
///          PSS/PKCS#1 padding routines need a specific output size).
///          Stops early once the limb supply is exhausted, leaving
///          the high bytes of `out` zero-padded.
/// @param words Little-endian source limbs.
/// @param word_count Number of available source limbs.
/// @param out Destination big-endian byte buffer.
/// @param out_len Exact output size in bytes.
static void rsa_words_to_be(const uint64_t *words,
                            size_t word_count,
                            uint8_t *out,
                            size_t out_len) {
    memset(out, 0, out_len);
    for (size_t i = 0; i < out_len; i++) {
        size_t word_index = i / 8;
        if (word_index >= word_count)
            break;
        out[out_len - 1 - i] = (uint8_t)(words[word_index] >> ((i % 8) * 8));
    }
}

/// @brief Compare two multi-word integers — returns -1 / 0 / 1 like `memcmp`.
/// @details Walks limbs from most-significant down (the `i--` loop is
///          deliberately ordered that way), short-circuiting at the
///          first inequality. Used by the modular-reduction step in
///          `rsa_mod_double_inplace` to decide whether subtraction
///          is needed.
/// @param a First little-endian limb array.
/// @param b Second little-endian limb array.
/// @param count Common limb count.
/// @return -1, zero, or 1 when @p a is less than, equal to, or greater
///         than @p b.
static int rsa_words_cmp(const uint64_t *a, const uint64_t *b, size_t count) {
    for (size_t i = count; i-- > 0;) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}

/// @brief Big-integer subtraction: `a -= b` with borrow propagation across limbs.
/// @details Uses `__int128` arithmetic to capture the borrow bit in a
///          way the optimizer can fold cleanly into `sbb` instructions
///          on x86_64 / `subs/sbcs` on AArch64. Caller is responsible
///          for ensuring `a >= b` (we don't return underflow status —
///          this is used only in the modular-reduction step where
///          underflow can't happen by construction).
/// @param a In/out minuend limb array.
/// @param b Subtrahend limb array.
/// @param count Common limb count.
static void rsa_words_sub_inplace(uint64_t *a, const uint64_t *b, size_t count) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t rhs = b[i] + borrow;
        uint64_t next_borrow = (rhs < b[i]) || (a[i] < rhs);
        a[i] -= rhs;
        borrow = next_borrow;
    }
}

/// @brief Constant-time conditional swap of two big integers.
/// @details `mask` must be all-ones (`UINT64_MAX`) to swap or all-zeros
///          to leave alone — caller derives that from a boolean
///          condition without branching. The XOR-three-times pattern
///          (`a ^= diff; b ^= diff` after `diff = mask & (a ^ b)`)
///          swaps when `diff` is the difference and no-ops when
///          `diff` is zero. Used to make modular-exponentiation
///          timing-attack resistant: the same instruction sequence
///          runs whether or not the swap happens.
/// @param a First in/out limb array.
/// @param b Second in/out limb array.
/// @param count Common limb count.
/// @param mask All ones to swap or all zeros to retain the arrays.
static void rsa_words_cswap(uint64_t *a, uint64_t *b, size_t count, uint64_t mask) {
    for (size_t i = 0; i < count; i++) {
        uint64_t diff = mask & (a[i] ^ b[i]);
        a[i] ^= diff;
        b[i] ^= diff;
    }
}

/// @brief Add `value` into `words[index]`, propagating carry up through higher limbs.
/// @details Used inside the Montgomery multiplication inner loop to
///          fold partial products into the running accumulator. Stops
///          early once the carry chain reaches a limb where it
///          doesn't propagate further, which keeps the typical case
///          fast even for large modulus sizes.
/// @param words In/out accumulator limb array.
/// @param word_count Capacity of @p words in limbs.
/// @param index Initial limb receiving @p value.
/// @param value Value to add with carry propagation.
static void rsa_words_accumulate(uint64_t *words, size_t word_count, size_t index, uint64_t value) {
    while (value != 0 && index < word_count) {
        uint64_t sum = words[index] + value;
        value = sum < words[index] ? 1 : 0;
        words[index] = sum;
        index++;
    }
}

/// @brief Double a big integer modulo `modulus` (i.e. `value = (value * 2) % modulus`).
/// @details Implements `value <<= 1` across limbs, capturing the
///          out-of-range high bit, then conditionally subtracts
///          `modulus` if the result either overflowed (carry set)
///          or simply became `>= modulus`. Used by
///          `rsa_compute_r_squared` as a primitive building block —
///          repeatedly doubling 1 modulo N produces R² mod N, which
///          is the constant the Montgomery multiplication needs to
///          convert values into Montgomery form.
/// @param value In/out reduced little-endian integer.
/// @param modulus Odd modulus in the same limb representation.
/// @param word_count Common limb count.
static void rsa_mod_double_inplace(uint64_t *value, const uint64_t *modulus, size_t word_count) {
    uint64_t carry = 0;
    for (size_t i = 0; i < word_count; i++) {
        uint64_t next = value[i] >> 63;
        value[i] = (value[i] << 1) | carry;
        carry = next;
    }
    if (carry || rsa_words_cmp(value, modulus, word_count) >= 0)
        rsa_words_sub_inplace(value, modulus, word_count);
}

/// @brief Compute `R² mod N` where R = 2^(64*word_count) — the Montgomery domain conversion
/// constant.
/// @details Montgomery arithmetic represents values in the form
///          `aR mod N` so multiplications can avoid the cost of
///          division-based modular reduction. Going from a normal
///          value `a` to Montgomery form requires multiplying by
///          R² and then doing one Montgomery reduction, which
///          gives `aR mod N`. This helper precomputes that R².
///          Implementation: start with 1 and double `2 * word_count
///          * 64` times, each time reducing modulo N. That gives
///          `2^(2*word_count*64) mod N = R² mod N`. Slow (linear
///          in modulus bits) but only done once per modular-exp call.
/// @param out Destination limb array receiving R-squared modulo N.
/// @param modulus Odd modulus limb array.
/// @param word_count Common limb count.
static void rsa_compute_r_squared(uint64_t *out, const uint64_t *modulus, size_t word_count) {
    rsa_words_zero(out, word_count);
    out[0] = 1;
    for (size_t i = 0; i < word_count * 128; i++)
        rsa_mod_double_inplace(out, modulus, word_count);
}

/// @brief Compute `-N⁻¹ mod 2⁶⁴` (the modular inverse of the modulus's lowest limb, negated).
/// @details Montgomery reduction needs `-N⁻¹ mod 2⁶⁴` precomputed
///          (just the bottom 64 bits of the inverse, since the
///          reduction step only uses that value). Uses 6 rounds of
///          Newton-style iteration: starting from `x = 1`, the
///          recurrence `x ← x * (2 - N₀*x)` doubles the number of
///          correct bits each round. 6 rounds is enough to converge
///          to a full 64-bit inverse for any odd `N₀` (and N is
///          odd by RSA construction — it's the product of two
///          odd primes). Returns the *negated* inverse (`0 - x`)
///          because that's the form Montgomery reduction wants.
/// @param n0 Least-significant odd modulus limb.
/// @return Negated multiplicative inverse of @p n0 modulo 2^64.
static uint64_t rsa_montgomery_n0_inv(uint64_t n0) {
    uint64_t x = 1;
    for (int i = 0; i < 6; i++)
        x *= 2 - n0 * x;
    return (uint64_t)(0 - x);
}

/// @brief Montgomery modular multiplication: `out = a * b * R⁻¹ mod modulus`.
/// @details The core primitive of Montgomery exponentiation. Implements
///          the CIOS (Coarsely Integrated Operand Scanning) algorithm:
///          for each limb `a[i]`, multiply-and-add `a[i] * b` into a
///          running accumulator, then perform one Montgomery reduction
///          step that adds a multiple of the modulus chosen to zero
///          out the lowest accumulator limb. After `word_count`
///          iterations, the accumulator holds `(a * b * R⁻¹) mod N`
///          plus possibly one extra `N` (handled by a final
///          conditional subtract).
///          The `n0_inv` parameter (precomputed by
///          `rsa_montgomery_n0_inv`) is what makes the per-limb
///          reduction work without division.
///          Time complexity is `O(word_count²)`. For 2048-bit RSA
///          (32 limbs), each Mont-mul is ~1024 limb-multiplies,
///          which the wide-multiply intrinsic compiles to single
///          `mulx` / `umulh` instructions.
/// @param out Destination limb array; may alias neither scratch input.
/// @param a First Montgomery-domain operand.
/// @param b Second Montgomery-domain operand.
/// @param modulus Odd modulus limb array.
/// @param word_count Common operand and modulus limb count.
/// @param n0_inv Negated inverse of the modulus's lowest limb.
static void rsa_mont_mul(uint64_t *out,
                         const uint64_t *a,
                         const uint64_t *b,
                         const uint64_t *modulus,
                         size_t word_count,
                         uint64_t n0_inv) {
    uint64_t t[RT_RSA_MAX_WORDS + 3];

    rsa_words_zero(t, RT_RSA_MAX_WORDS + 3);
    for (size_t i = 0; i < word_count; i++) {
        uint64_t carry = 0;
        uint64_t m;

        for (size_t j = 0; j < word_count; j++) {
            uint64_t low = 0;
            uint64_t high = 0;
            rsa_mul_add_u64(a[j], b[i], t[j], carry, &low, &high);
            t[j] = low;
            carry = high;
        }
        rsa_words_accumulate(t, word_count + 3, word_count, (uint64_t)carry);

        m = t[0] * n0_inv;
        carry = 0;
        for (size_t j = 0; j < word_count; j++) {
            uint64_t low = 0;
            uint64_t high = 0;
            rsa_mul_add_u64(m, modulus[j], t[j], carry, &low, &high);
            t[j] = low;
            carry = high;
        }
        rsa_words_accumulate(t, word_count + 3, word_count, (uint64_t)carry);

        for (size_t j = 0; j < word_count + 2; j++)
            t[j] = t[j + 1];
        t[word_count + 2] = 0;
    }

    memcpy(out, t, word_count * sizeof(*out));
    if (rsa_words_cmp(out, modulus, word_count) >= 0)
        rsa_words_sub_inplace(out, modulus, word_count);
}

/// @brief Count the bit-length of a big-endian integer (highest set bit + 1, or 0 if all-zero).
/// @details Walks from the most-significant byte forward to find the
///          first non-zero byte, then counts the bits within it. Used
///          by `rsa_modexp_bytes` to know how many iterations of the
///          Montgomery ladder are needed (one per exponent bit).
/// @param data Unsigned big-endian integer bytes.
/// @param len Number of bytes in @p data.
/// @return Significant bit length, or zero for absent or all-zero input.
static size_t rsa_bit_length_be(const uint8_t *data, size_t len) {
    if (!data || len == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        if (byte != 0) {
            size_t bits = (len - i - 1) * 8;
            while (byte != 0) {
                bits++;
                byte >>= 1;
            }
            return bits;
        }
    }
    return 0;
}

/// @brief Test a bit in a big-endian integer, numbered from the least
///        significant bit.
/// @param data Big-endian integer bytes.
/// @param len Number of bytes in @p data.
/// @param bit_index Zero-based bit index from the least-significant end.
/// @return One when the selected bit is set; otherwise zero.
static int rsa_bit_test_be(const uint8_t *data, size_t len, size_t bit_index) {
    size_t byte_index = len - 1 - (bit_index / 8);
    return (data[byte_index] >> (bit_index % 8)) & 1;
}

/// @brief Compute `base^exp mod modulus` using the constant-time Montgomery ladder.
/// @details The complete RSA primitive — every signature verify and
///          every public-key operation in this file ultimately calls
///          here. Algorithm:
///          1. Convert `modulus`, `base` to limb arrays.
///          2. Precompute `R² mod N` and `-N⁻¹ mod 2⁶⁴` for the
///             Montgomery layer.
///          3. Convert `1` and `base` into Montgomery form
///             (multiply by R²).
///          4. Walk exponent bits from most-significant down using
///             the **Montgomery ladder**: at each step, the pair
///             `(r0, r1)` is conditionally swapped based on the
///             current exponent bit, then both are squared/multiplied
///             together. The conditional swap (via `rsa_words_cswap`)
///             keeps the operation count identical regardless of
///             exponent bit values, which is the timing-attack
///             defense.
///          5. Convert the result back out of Montgomery form
///             (multiply by 1).
///          6. Emit as big-endian bytes.
///          Validates that `base < modulus` (per RSA primitive
///          definition) and that `modulus` is odd (Montgomery
///          arithmetic requires odd modulus). Returns 0 on validation
///          failure, 1 on success.
/// @param base Unsigned big-endian base.
/// @param base_len Number of base bytes.
/// @param exp Unsigned big-endian exponent.
/// @param exp_len Number of exponent bytes.
/// @param mod Unsigned big-endian odd modulus.
/// @param mod_len Number of modulus bytes.
/// @param out Destination for the fixed-width big-endian result.
/// @param out_len Destination capacity, at least @p mod_len.
/// @return One on success; zero for invalid sizes, operands, or modulus.
static int rsa_modexp_bytes(const uint8_t *base,
                            size_t base_len,
                            const uint8_t *exp,
                            size_t exp_len,
                            const uint8_t *mod,
                            size_t mod_len,
                            uint8_t *out,
                            size_t out_len) {
    uint64_t modulus_words[RT_RSA_MAX_WORDS];
    uint64_t base_words[RT_RSA_MAX_WORDS];
    uint64_t r0[RT_RSA_MAX_WORDS];
    uint64_t r1[RT_RSA_MAX_WORDS];
    uint64_t tmp0[RT_RSA_MAX_WORDS];
    uint64_t tmp1[RT_RSA_MAX_WORDS];
    uint64_t rr[RT_RSA_MAX_WORDS];
    uint64_t one_words[RT_RSA_MAX_WORDS];
    uint64_t n0_inv;
    size_t word_count;
    size_t bit_count;

    if (!base || !exp || !mod || !out || mod_len == 0 || out_len < mod_len ||
        mod_len > RT_RSA_MAX_MOD_BYTES)
        return 0;
    if ((mod[mod_len - 1] & 1u) == 0)
        return 0;

    word_count = rsa_word_count_from_len(mod_len);
    if (word_count == 0 || word_count > RT_RSA_MAX_WORDS)
        return 0;

    rsa_words_from_be(mod, mod_len, modulus_words, word_count);
    rsa_words_from_be(base, base_len, base_words, word_count);
    if (rsa_words_cmp(base_words, modulus_words, word_count) >= 0)
        return 0;

    rsa_words_zero(one_words, word_count);
    one_words[0] = 1;
    rsa_compute_r_squared(rr, modulus_words, word_count);
    n0_inv = rsa_montgomery_n0_inv(modulus_words[0]);

    rsa_mont_mul(r0, one_words, rr, modulus_words, word_count, n0_inv);
    rsa_mont_mul(r1, base_words, rr, modulus_words, word_count, n0_inv);

    bit_count = rsa_bit_length_be(exp, exp_len);
    for (size_t bit_index = bit_count; bit_index-- > 0;) {
        uint64_t swap_mask = 0u - (uint64_t)rsa_bit_test_be(exp, exp_len, bit_index);
        rsa_words_cswap(r0, r1, word_count, swap_mask);
        rsa_mont_mul(tmp1, r0, r1, modulus_words, word_count, n0_inv);
        rsa_mont_mul(tmp0, r0, r0, modulus_words, word_count, n0_inv);
        memcpy(r0, tmp0, word_count * sizeof(*r0));
        memcpy(r1, tmp1, word_count * sizeof(*r1));
        rsa_words_cswap(r0, r1, word_count, swap_mask);
    }

    rsa_mont_mul(base_words, r0, one_words, modulus_words, word_count, n0_inv);
    rsa_words_to_be(base_words, word_count, out, mod_len);
    return 1;
}

/// @brief Encode a message digest into RSASSA-PSS padded form (RFC 8017 §9.1.1).
/// @details PSS = Probabilistic Signature Scheme. Adds a random salt
///          per signature so the same message signed twice produces
///          different signatures, plugging a class of weakness in
///          textbook RSA. Layout of the encoded message `EM`:
///          ```
///          EM = maskedDB || H || 0xBC
///          ```
///          where:
///          - `H = Hash(0x0000000000000000 || mHash || salt)` —
///             the "M' prime" construction binds salt to message.
///          - `DB = PS || 0x01 || salt` — padding string + sentinel
///            + salt, then XORed with `MGF1(H, |DB|)` to produce
///            `maskedDB`.
///          - The high bits of `EM[0]` are masked off to guarantee
///            `EM < N` (the RSA modulus) so the modular exponentiation
///            won't reject it.
///          Salt length = hash length here (the conventional choice).
///          Used only by signing operations, which Zanna doesn't do
///          today — kept for symmetry with `rsa_pss_verify_encoded`.
/// @param key Valid private key supplying the modulus width.
/// @param hash_id SHA-2 algorithm used for the digest, salt, and MGF1.
/// @param digest Precomputed message digest.
/// @param digest_len Digest length, required to match @p hash_id.
/// @param em Destination encoded-message buffer.
/// @param em_len Exact encoded-message length derived from the modulus.
/// @return One on success; zero after validation, entropy, or encoding
///         failure. Failure scrubs @p em.
static int rsa_pss_encode(const rt_rsa_key_t *key,
                          rt_rsa_hash_t hash_id,
                          const uint8_t *digest,
                          size_t digest_len,
                          uint8_t *em,
                          size_t em_len) {
    uint8_t salt[64];
    uint8_t m_prime[136];
    uint8_t h[64];
    uint8_t db[512];
    uint8_t db_mask[512];
    size_t h_len = rsa_hash_len(hash_id);
    size_t mod_bits = rsa_modulus_bits(key);
    size_t em_bits = mod_bits > 0 ? mod_bits - 1 : 0;
    size_t masked_db_len = 0;
    size_t ps_len = 0;
    uint8_t top_mask = 0xFF;
    int ok = 0;

    if (!key || !digest || !em || h_len == 0 || digest_len != h_len)
        goto done;
    if (em_len < h_len * 2 + 2 || h_len > sizeof(salt))
        goto done;

    masked_db_len = em_len - h_len - 1;
    if (masked_db_len > sizeof(db))
        goto done;
    ps_len = masked_db_len - h_len - 1;
    if (8 * em_len > em_bits)
        top_mask = (uint8_t)(0xFFu >> (8 * em_len - em_bits));

    rt_crypto_random_bytes(salt, h_len);
    memset(m_prime, 0, 8);
    memcpy(m_prime + 8, digest, h_len);
    memcpy(m_prime + 8 + h_len, salt, h_len);
    if (!rsa_hash_buffer(hash_id, m_prime, 8 + h_len + h_len, h))
        goto done;

    memset(db, 0, ps_len);
    db[ps_len] = 0x01;
    memcpy(db + ps_len + 1, salt, h_len);
    if (!rsa_mgf1(hash_id, h, h_len, db_mask, masked_db_len))
        goto done;
    for (size_t i = 0; i < masked_db_len; i++)
        em[i] = db[i] ^ db_mask[i];
    em[0] &= top_mask;
    memcpy(em + masked_db_len, h, h_len);
    em[em_len - 1] = 0xBC;
    ok = 1;

done:
    rsa_secure_zero(salt, sizeof(salt));
    rsa_secure_zero(m_prime, sizeof(m_prime));
    rsa_secure_zero(h, sizeof(h));
    rsa_secure_zero(db, sizeof(db));
    rsa_secure_zero(db_mask, sizeof(db_mask));
    if (!ok && em && em_len > 0)
        rsa_secure_zero(em, em_len);
    return ok;
}

/// @brief Verify a PSS-encoded message against an expected digest (RFC 8017 §9.1.2).
/// @details Inverse of `rsa_pss_encode`. Steps:
///          1. Validate the trailing `0xBC` sentinel and length floor.
///          2. Recover `H` from the right side of `EM`.
///          3. Recompute `dbMask = MGF1(H, |DB|)` and XOR with the
///             left side of `EM` to recover `DB = PS || 0x01 || salt`.
///          4. Mask the high bits of `DB[0]` to match what encoding
///             would have done.
///          5. Validate `PS` is all-zero and that the `0x01` separator
///             is present at the expected position.
///          6. Reconstruct `M' = 0x0000000000000000 || mHash || salt`,
///             hash it, and compare against `H` from step 2.
///          Returns 1 only if every check passes — any tampering
///          with the signature or the message digest causes one of
///          the checks to fail. This is the function the TLS
///          certificate validator and the TLS-server CertificateVerify
///          path call into for RSA-PSS-SHA256 signatures.
/// @param key Valid public key supplying the modulus width.
/// @param hash_id SHA-2 algorithm used for the digest and MGF1.
/// @param digest Expected precomputed message digest.
/// @param digest_len Digest length, required to match @p hash_id.
/// @param em Recovered PSS encoded-message bytes.
/// @param em_len Number of encoded-message bytes.
/// @return One only when the complete PSS structure and digest match;
///         otherwise zero.
static int rsa_pss_verify_encoded(const rt_rsa_key_t *key,
                                  rt_rsa_hash_t hash_id,
                                  const uint8_t *digest,
                                  size_t digest_len,
                                  const uint8_t *em,
                                  size_t em_len) {
    uint8_t h[64];
    uint8_t db[512];
    uint8_t db_mask[512];
    uint8_t m_prime[136];
    size_t h_len = rsa_hash_len(hash_id);
    size_t mod_bits = rsa_modulus_bits(key);
    size_t em_bits = mod_bits > 0 ? mod_bits - 1 : 0;
    size_t masked_db_len = 0;
    size_t ps_len = 0;
    uint8_t top_mask = 0xFF;

    if (!key || !digest || !em || h_len == 0 || digest_len != h_len)
        return 0;
    if (em_len < h_len * 2 + 2 || em[em_len - 1] != 0xBC)
        return 0;

    masked_db_len = em_len - h_len - 1;
    if (masked_db_len > sizeof(db))
        return 0;
    if (8 * em_len > em_bits)
        top_mask = (uint8_t)(0xFFu >> (8 * em_len - em_bits));
    if ((em[0] & (uint8_t)~top_mask) != 0)
        return 0;

    memcpy(h, em + masked_db_len, h_len);
    if (!rsa_mgf1(hash_id, h, h_len, db_mask, masked_db_len))
        return 0;
    for (size_t i = 0; i < masked_db_len; i++)
        db[i] = em[i] ^ db_mask[i];
    db[0] &= top_mask;

    if (masked_db_len < h_len + 1)
        return 0;
    ps_len = masked_db_len - h_len - 1;
    uint8_t padding_bad = 0;
    for (size_t i = 0; i < ps_len; i++) {
        padding_bad |= db[i];
    }
    if (padding_bad != 0 || db[ps_len] != 0x01)
        return 0;

    memset(m_prime, 0, 8);
    memcpy(m_prime + 8, digest, h_len);
    memcpy(m_prime + 8 + h_len, db + ps_len + 1, h_len);
    if (!rsa_hash_buffer(hash_id, m_prime, 8 + h_len + h_len, db_mask))
        return 0;
    return rsa_ct_mem_equal(db_mask, h, h_len);
}

/// @brief Return the canonical DigestInfo DER prefix for PKCS#1 v1.5 RSA signatures.
/// @details PKCS#1 v1.5 (the older-style RSA signature scheme, still
///          widely used for cert-chain RSA signatures even when the
///          leaf cert uses PSS) wraps the message digest in a
///          fixed DER `DigestInfo` ASN.1 structure that names which
///          hash function produced it. These three byte arrays are
///          the precomputed DER for `SEQUENCE { algorithmIdentifier(SHA-N), OCTET STRING(digest) }`
///          with the digest portion left to be appended by the caller.
///          Returns NULL for unknown hash IDs.
/// @param hash_id SHA-2 algorithm named by the DigestInfo.
/// @param len_out Destination receiving the static prefix length.
/// @return Borrowed immutable prefix bytes, or NULL for invalid arguments or
///         an unsupported hash.
static const uint8_t *rsa_pkcs1_digest_info_prefix(rt_rsa_hash_t hash_id, size_t *len_out) {
    static const uint8_t SHA256_PREFIX[] = {0x30,
                                            0x31,
                                            0x30,
                                            0x0d,
                                            0x06,
                                            0x09,
                                            0x60,
                                            0x86,
                                            0x48,
                                            0x01,
                                            0x65,
                                            0x03,
                                            0x04,
                                            0x02,
                                            0x01,
                                            0x05,
                                            0x00,
                                            0x04,
                                            0x20};
    static const uint8_t SHA384_PREFIX[] = {0x30,
                                            0x41,
                                            0x30,
                                            0x0d,
                                            0x06,
                                            0x09,
                                            0x60,
                                            0x86,
                                            0x48,
                                            0x01,
                                            0x65,
                                            0x03,
                                            0x04,
                                            0x02,
                                            0x02,
                                            0x05,
                                            0x00,
                                            0x04,
                                            0x30};
    static const uint8_t SHA512_PREFIX[] = {0x30,
                                            0x51,
                                            0x30,
                                            0x0d,
                                            0x06,
                                            0x09,
                                            0x60,
                                            0x86,
                                            0x48,
                                            0x01,
                                            0x65,
                                            0x03,
                                            0x04,
                                            0x02,
                                            0x03,
                                            0x05,
                                            0x00,
                                            0x04,
                                            0x40};

    if (!len_out)
        return NULL;
    switch (hash_id) {
        case RT_RSA_HASH_SHA256:
            *len_out = sizeof(SHA256_PREFIX);
            return SHA256_PREFIX;
        case RT_RSA_HASH_SHA384:
            *len_out = sizeof(SHA384_PREFIX);
            return SHA384_PREFIX;
        case RT_RSA_HASH_SHA512:
            *len_out = sizeof(SHA512_PREFIX);
            return SHA512_PREFIX;
        default:
            *len_out = 0;
            return NULL;
    }
}

/// @brief Zero-initialize an RSA key struct (sets all pointers and lengths to 0).
/// @details Idempotent. Safe to call on uninitialized memory before
///          first use. Pair with `rt_rsa_key_free` for proper
///          lifetime management.
/// @param key Key structure to initialize; NULL is a no-op.
void rt_rsa_key_init(rt_rsa_key_t *key) {
    if (!key)
        return;
    memset(key, 0, sizeof(*key));
}

/// @brief Free heap-allocated key components and zero out the struct.
/// @details Frees the modulus, public exponent, and private exponent
///          buffers (each was malloc'd by the parsers via
///          `rsa_dup_be_trimmed`). Final memset ensures any
///          subsequent accidental read sees zeros, not stale
///          pointers — a small defense against use-after-free
///          patterns. Safe on null input.
/// @param key Initialized key whose owned components are consumed; NULL is a
///        no-op.
void rt_rsa_key_free(rt_rsa_key_t *key) {
    if (!key)
        return;
    rsa_secure_free(key->modulus, key->modulus_len);
    rsa_secure_free(key->public_exponent, key->public_exponent_len);
    rsa_secure_free(key->private_exponent, key->private_exponent_len);
    memset(key, 0, sizeof(*key));
}

/// @brief Parse an RSA public key from PKCS#1 RSAPublicKey DER bytes.
/// @details PKCS#1 RSAPublicKey is the simplest RSA public-key
///          encoding: a SEQUENCE of two INTEGERs — modulus first,
///          public exponent second. This is the format used inside
///          the SubjectPublicKeyInfo BIT STRING of an X.509
///          certificate (X.509 wraps it in another layer that the
///          caller has to peel off first). Returns 0 on malformed
///          input; on success, fills `out` and the caller owns the
///          modulus/exponent buffers via `rt_rsa_key_free`.
/// @param der Complete PKCS#1 RSAPublicKey DER bytes.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key replaced only after complete parse and
///        validation success.
/// @return One on success; zero for malformed input, unsupported key sizes, or
///         allocation failure.
int rt_rsa_parse_public_key_pkcs1(const uint8_t *der, size_t der_len, rt_rsa_key_t *out) {
    uint8_t tag;
    size_t value_len, hdr_len, pos = 0;
    size_t end = 0;
    rt_rsa_key_t key;

    if (!der || der_len == 0 || !out)
        return 0;
    rt_rsa_key_init(&key);
    if (!rsa_der_read_tlv(der, der_len, &tag, &value_len, &hdr_len) || tag != 0x30)
        return 0;
    end = hdr_len + value_len;
    if (end != der_len)
        return 0;
    pos = hdr_len;
    if (!rsa_parse_der_integer(der, end, &pos, &key.modulus, &key.modulus_len) ||
        !rsa_parse_der_integer(der, end, &pos, &key.public_exponent, &key.public_exponent_len) ||
        pos != end || !rsa_validate_public_components(&key)) {
        rt_rsa_key_free(&key);
        return 0;
    }
    rt_rsa_key_free(out);
    *out = key;
    return 1;
}

/// @brief Parse an RSA private key from PKCS#1 RSAPrivateKey DER bytes.
/// @details PKCS#1 RSAPrivateKey is a longer SEQUENCE: version,
///          modulus, public exponent, private exponent, primes p
///          and q, exponents dP and dQ, coefficient qInv, and an
///          optional otherPrimeInfos. We parse and validate the full
///          mandatory two-prime structure, retain n/e/d for the runtime's
///          direct mod-exp path, and reject multi-prime version 1 keys
///          until CRT/multi-prime support exists.
/// @param der Complete PKCS#1 RSAPrivateKey DER bytes.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key replaced only after complete parse and
///        validation success.
/// @return One on success; zero for malformed, unsupported, invalid, or
///         unallocatable input.
int rt_rsa_parse_private_key_pkcs1(const uint8_t *der, size_t der_len, rt_rsa_key_t *out) {
    uint8_t tag;
    size_t value_len, hdr_len, pos = 0;
    size_t end = 0;
    uint8_t *version = NULL;
    size_t version_len = 0;
    uint8_t *prime1 = NULL;
    uint8_t *prime2 = NULL;
    uint8_t *exponent1 = NULL;
    uint8_t *exponent2 = NULL;
    uint8_t *coefficient = NULL;
    size_t prime1_len = 0;
    size_t prime2_len = 0;
    size_t exponent1_len = 0;
    size_t exponent2_len = 0;
    size_t coefficient_len = 0;
    rt_rsa_key_t key;
    int ok = 0;

    if (!der || der_len == 0 || !out)
        return 0;
    rt_rsa_key_init(&key);
    if (!rsa_der_read_tlv(der, der_len, &tag, &value_len, &hdr_len) || tag != 0x30)
        return 0;
    end = hdr_len + value_len;
    if (end != der_len)
        return 0;
    pos = hdr_len;
    if (!rsa_parse_der_integer(der, end, &pos, &version, &version_len) || version_len != 1 ||
        version[0] != 0x00 ||
        !rsa_parse_der_integer(der, end, &pos, &key.modulus, &key.modulus_len) ||
        !rsa_parse_der_integer(der, end, &pos, &key.public_exponent, &key.public_exponent_len) ||
        !rsa_parse_der_integer(der, end, &pos, &key.private_exponent, &key.private_exponent_len) ||
        !rsa_parse_der_integer(der, end, &pos, &prime1, &prime1_len) ||
        !rsa_parse_der_integer(der, end, &pos, &prime2, &prime2_len) ||
        !rsa_parse_der_integer(der, end, &pos, &exponent1, &exponent1_len) ||
        !rsa_parse_der_integer(der, end, &pos, &exponent2, &exponent2_len) ||
        !rsa_parse_der_integer(der, end, &pos, &coefficient, &coefficient_len) || pos != end ||
        !rsa_validate_private_components(&key) || rsa_be_is_zero(prime1, prime1_len) ||
        rsa_be_is_zero(prime2, prime2_len) || rsa_be_is_zero(exponent1, exponent1_len) ||
        rsa_be_is_zero(exponent2, exponent2_len) || rsa_be_is_zero(coefficient, coefficient_len)) {
        goto done;
    }

    rt_rsa_key_free(out);
    *out = key;
    rt_rsa_key_init(&key);
    ok = 1;

done:
    rsa_secure_free(version, version_len);
    rsa_secure_free(prime1, prime1_len);
    rsa_secure_free(prime2, prime2_len);
    rsa_secure_free(exponent1, exponent1_len);
    rsa_secure_free(exponent2, exponent2_len);
    rsa_secure_free(coefficient, coefficient_len);
    rt_rsa_key_free(&key);
    return ok;
}

/// @brief Validate that AlgorithmIdentifier parameters are absent or DER NULL.
/// @param params Remaining parameter bytes after the algorithm OID.
/// @param params_len Number of bytes in @p params.
/// @return One for no bytes or one canonical empty NULL TLV; otherwise zero.
static int rsa_der_params_absent_or_null(const uint8_t *params, size_t params_len) {
    uint8_t tag;
    size_t value_len;
    size_t hdr_len;

    if (params_len == 0)
        return 1;
    if (!rsa_der_read_tlv(params, params_len, &tag, &value_len, &hdr_len) || tag != 0x05)
        return 0;
    return value_len == 0 && hdr_len + value_len == params_len;
}

/// @brief Parse an RSA private key from PKCS#8 PrivateKeyInfo DER bytes.
/// @details PKCS#8 is the modern wrapper that most contemporary tools
///          (OpenSSL, Go, etc.) emit by default for `.key` files.
///          We require exact DER consumption, version 0, rsaEncryption
///          AlgorithmIdentifier with absent/NULL parameters, and an
///          OCTET STRING containing a PKCS#1 RSAPrivateKey.
/// @param der Complete PKCS#8 PrivateKeyInfo DER bytes.
/// @param der_len Number of bytes in @p der.
/// @param out Initialized key replaced only after the nested PKCS#1 parse
///        succeeds.
/// @return One on success; zero for malformed input, an unrecognized
///         algorithm, or invalid nested key material.
int rt_rsa_parse_private_key_pkcs8(const uint8_t *der, size_t der_len, rt_rsa_key_t *out) {
    static const uint8_t OID_RSA_ENCRYPTION[] = {
        0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
    uint8_t tag;
    size_t value_len, hdr_len, pos = 0;
    size_t outer_end = 0;
    size_t alg_len = 0, alg_hdr = 0;
    uint8_t *version = NULL;
    size_t version_len = 0;
    int ok = 0;

    if (!der || der_len == 0 || !out)
        return 0;
    if (!rsa_der_read_tlv(der, der_len, &tag, &value_len, &hdr_len) || tag != 0x30)
        return 0;
    outer_end = hdr_len + value_len;
    if (outer_end != der_len)
        return 0;
    pos = hdr_len;
    if (!rsa_parse_der_integer(der, outer_end, &pos, &version, &version_len) || version_len != 1 ||
        version[0] != 0x00)
        goto done;

    if (!rsa_der_read_tlv(der + pos, outer_end - pos, &tag, &alg_len, &alg_hdr) || tag != 0x30) {
        goto done;
    }
    {
        const uint8_t *alg = der + pos + alg_hdr;
        size_t alg_pos = 0;
        size_t oid_len = 0;
        size_t oid_hdr = 0;
        pos += alg_hdr + alg_len;
        if (!rsa_der_read_tlv(alg, alg_len, &tag, &oid_len, &oid_hdr) || tag != 0x06)
            goto done;
        if (oid_hdr > alg_len || oid_len > alg_len - oid_hdr ||
            oid_len != sizeof(OID_RSA_ENCRYPTION) ||
            memcmp(alg + oid_hdr, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION)) != 0)
            goto done;
        alg_pos = oid_hdr + oid_len;
        if (!rsa_der_params_absent_or_null(alg + alg_pos, alg_len - alg_pos))
            goto done;
    }
    if (!rsa_der_read_tlv(der + pos, outer_end - pos, &tag, &value_len, &hdr_len) || tag != 0x04) {
        goto done;
    }
    if (pos > outer_end || hdr_len > outer_end - pos || value_len != outer_end - pos - hdr_len)
        goto done;
    ok = rt_rsa_parse_private_key_pkcs1(der + pos + hdr_len, value_len, out);

done:
    rsa_secure_free(version, version_len);
    return ok;
}

/// @brief Compare two RSA public keys for byte-for-byte equality of modulus and public exponent.
/// @details Used by the TLS server to check whether a leaf cert's
///          public key matches a previously-loaded private key
///          (mismatched leaf-cert / private-key pairs are a common
///          configuration mistake; better to detect it at load time
///          than during the first failed handshake). Returns 0 if
///          either key is missing required fields.
/// @param lhs First parsed RSA key.
/// @param rhs Second parsed RSA key.
/// @return One when modulus and public exponent lengths and bytes match;
///         otherwise zero.
int rt_rsa_public_equals(const rt_rsa_key_t *lhs, const rt_rsa_key_t *rhs) {
    if (!lhs || !rhs || !lhs->modulus || !rhs->modulus || !lhs->public_exponent ||
        !rhs->public_exponent) {
        return 0;
    }
    return lhs->modulus_len == rhs->modulus_len &&
                   lhs->public_exponent_len == rhs->public_exponent_len &&
                   memcmp(lhs->modulus, rhs->modulus, lhs->modulus_len) == 0 &&
                   memcmp(lhs->public_exponent, rhs->public_exponent, lhs->public_exponent_len) == 0
               ? 1
               : 0;
}

/// @brief Produce an RSA-PSS signature over `digest` using the private key in `key`.
/// @details Two-step process:
///          1. PSS-encode `digest` into a `modulus_len`-byte block
///             (`rsa_pss_encode` adds the salt + MGF1 mask + sentinel).
///          2. Modular-exponentiate the encoded block by the private
///             exponent to produce the signature
///             (`signature = encoded^d mod N`).
///          On success writes the signature to `sig_out` and the
///          actual length to `*sig_len_out` (always equals
///          `modulus_len`). Returns 0 on any encoding or arithmetic
///          failure. Used by the TLS server's CertificateVerify
///          path when the server's key is RSA.
/// @param key Valid RSA private key containing n, e, and d.
/// @param hash_id SHA-2 algorithm matching @p digest.
/// @param digest Precomputed message digest.
/// @param digest_len Digest length, required to match @p hash_id.
/// @param sig_out Caller buffer receiving a modulus-width signature.
/// @param sig_len_out In/out capacity and produced signature length.
/// @return One on success; zero for invalid key/input, insufficient output
///         capacity, or encoding/arithmetic failure.
int rt_rsa_pss_sign(const rt_rsa_key_t *key,
                    rt_rsa_hash_t hash_id,
                    const uint8_t *digest,
                    size_t digest_len,
                    uint8_t *sig_out,
                    size_t *sig_len_out) {
    uint8_t em[RT_RSA_MAX_MOD_BYTES];
    size_t em_len = 0;
    int ok = 0;

    if (!key || !sig_out || !sig_len_out || !rsa_validate_private_components(key))
        return 0;
    em_len = key->modulus_len;
    if (em_len == 0 || em_len > sizeof(em) || *sig_len_out < em_len)
        return 0;
    if (!rsa_pss_encode(key, hash_id, digest, digest_len, em, em_len))
        goto done;
    if (!rsa_modexp_bytes(em,
                          em_len,
                          key->private_exponent,
                          key->private_exponent_len,
                          key->modulus,
                          key->modulus_len,
                          sig_out,
                          em_len)) {
        goto done;
    }
    *sig_len_out = em_len;
    ok = 1;

done:
    rsa_secure_zero(em, sizeof(em));
    return ok;
}

/// @brief Verify an RSA-PSS signature against an expected digest using the public key in `key`.
/// @details Two-step process (inverse of sign):
///          1. Modular-exponentiate the signature bytes by the public
///             exponent to recover the encoded message
///             (`encoded = signature^e mod N`).
///          2. PSS-verify the recovered encoding against `digest`
///             (`rsa_pss_verify_encoded` checks the salt-binding hash).
///          Returns 1 only if the recovered encoding is well-formed
///          AND its digest matches what the caller provided.
///          This is the function the TLS validator and TLS-server
///          handshake call into for RSA-PSS-SHA256 cert / handshake
///          signature verification, which is the most common
///          modern-TLS signature path.
/// @param key Valid RSA public key containing n and e.
/// @param hash_id SHA-2 algorithm matching @p digest.
/// @param digest Expected precomputed message digest.
/// @param digest_len Digest length, required to match @p hash_id.
/// @param sig Modulus-width signature bytes.
/// @param sig_len Number of bytes in @p sig.
/// @return One only for a structurally valid signature matching the digest;
///         otherwise zero.
int rt_rsa_pss_verify(const rt_rsa_key_t *key,
                      rt_rsa_hash_t hash_id,
                      const uint8_t *digest,
                      size_t digest_len,
                      const uint8_t *sig,
                      size_t sig_len) {
    uint8_t em[RT_RSA_MAX_MOD_BYTES];
    int ok = 0;

    if (!sig || !rsa_validate_public_components(key))
        return 0;
    if (key->modulus_len == 0 || key->modulus_len > sizeof(em) || sig_len != key->modulus_len)
        return 0;
    if (!rsa_modexp_bytes(sig,
                          sig_len,
                          key->public_exponent,
                          key->public_exponent_len,
                          key->modulus,
                          key->modulus_len,
                          em,
                          key->modulus_len)) {
        goto done;
    }
    ok = rsa_pss_verify_encoded(key, hash_id, digest, digest_len, em, key->modulus_len);

done:
    rsa_secure_zero(em, sizeof(em));
    return ok;
}

/// @brief Verify an RSA PKCS#1 v1.5 signature against an expected digest using the public key.
/// @details The older RSA signature scheme, still widely used for
///          X.509 certificate-chain signatures even when the leaf
///          uses PSS. Verification:
///          1. Recover the encoded message via `signature^e mod N`.
///          2. Validate the deterministic v1.5 padding layout:
///             `0x00 0x01 0xFF...0xFF 0x00 DigestInfo digest`.
///          3. Match `DigestInfo` against the expected hash-algorithm
///             prefix (from `rsa_pkcs1_digest_info_prefix`).
///          4. Compare the trailing digest bytes against the
///             provided digest.
///          The 8+ `0xFF` minimum padding length prevents the
///          Bleichenbacher-style attacks that targeted weak v1.5
///          implementations (the original 1998 attack required
///          variable-length padding to leak information; modern
///          deterministic encoding closes that). Returns 1 only on
///          full structural and digest match. Used by the X.509
///          chain validator for cert-chain RSA signatures.
/// @param key Valid RSA public key containing n and e.
/// @param hash_id SHA-2 algorithm expected in DigestInfo.
/// @param digest Expected precomputed message digest.
/// @param digest_len Digest length, required to match @p hash_id.
/// @param sig Modulus-width signature bytes.
/// @param sig_len Number of bytes in @p sig.
/// @return One only for exact padding, DigestInfo, and digest agreement;
///         otherwise zero.
int rt_rsa_pkcs1_v15_verify(const rt_rsa_key_t *key,
                            rt_rsa_hash_t hash_id,
                            const uint8_t *digest,
                            size_t digest_len,
                            const uint8_t *sig,
                            size_t sig_len) {
    uint8_t em[RT_RSA_MAX_MOD_BYTES];
    const uint8_t *prefix = NULL;
    size_t prefix_len = 0;
    size_t i = 0;
    int ok = 0;

    if (!digest || !sig || !rsa_validate_public_components(key))
        return 0;
    if (key->modulus_len == 0 || key->modulus_len > sizeof(em) || sig_len != key->modulus_len)
        return 0;

    prefix = rsa_pkcs1_digest_info_prefix(hash_id, &prefix_len);
    if (!prefix || digest_len != rsa_hash_len(hash_id))
        return 0;

    if (!rsa_modexp_bytes(sig,
                          sig_len,
                          key->public_exponent,
                          key->public_exponent_len,
                          key->modulus,
                          key->modulus_len,
                          em,
                          key->modulus_len)) {
        goto done;
    }
    if (em[0] != 0x00 || em[1] != 0x01)
        goto done;
    for (i = 2; i < key->modulus_len && em[i] == 0xFF; i++) {
    }
    if (i < 10 || i >= key->modulus_len || em[i] != 0x00)
        goto done;
    i++;
    if (key->modulus_len - i != prefix_len + digest_len)
        goto done;
    if (!rsa_ct_mem_equal(em + i, prefix, prefix_len))
        goto done;
    ok = rsa_ct_mem_equal(em + i + prefix_len, digest, digest_len);

done:
    rsa_secure_zero(em, sizeof(em));
    return ok;
}
