//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_bigint.c
/// @file
/// @brief Implements GC-managed arbitrary-precision signed integers.
///
// Purpose: Implements arbitrary-precision integer arithmetic for the Zanna
//          runtime. Uses a base-2^32 little-endian digit array with a separate
//          sign flag. Covers grade-school add/sub/mul, Knuth Algorithm D for
//          division, Euclidean GCD, and conversion to/from int64 and strings.
//
// Key invariants:
//   - Digits are stored in little-endian order (index 0 = least significant).
//   - Zero is always represented as non-negative with zero digits (len == 0).
//   - The sign flag is 0 for non-negative and 1 for negative; -0 is normalised
//     to +0 after every operation.
//   - Digit arrays are heap-allocated via calloc and tracked separately from
//     the GC-managed outer object; the finalizer frees them explicitly.
//   - All arithmetic functions are pure with respect to their output objects;
//     no shared mutable state — safe for concurrent use on distinct objects.
//
// Ownership/Lifetime:
//   - BigInt objects are allocated via rt_obj_new_i64 (GC-managed); the
//     finalizer (bigint_finalizer) frees the digit array via free().
//   - Intermediate bigint_t values used during computation are owned by the
//     function and freed before return or on error paths.
//   - Public operations never consume operands and return new BigInt/runtime
//     objects. Across this ABI, a null BigInt operand denotes numeric zero.
//     Nonnull objects of the wrong runtime class raise a trap.
//
// Links: src/runtime/core/rt_bigint.h (public API),
//        src/runtime/core/rt_object.c (rt_obj_new_i64, rt_obj_set_finalizer),
//        src/runtime/core/rt_string.c (string output for to-string conversion)
//
//===----------------------------------------------------------------------===//

#include "rt_bigint.h"

#include "rt_bytes.h"
#include "rt_object.h"
#include "rt_string.h"

// External trap function
#include "rt_trap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Internal Representation
//=============================================================================

/// @brief Numeric base represented by one 32-bit magnitude limb.
#define BIGINT_BASE ((uint64_t)1 << 32)
/// @brief Runtime class identifier encoded from the text `BigInt`.
#define BIGINT_CLASS_ID 0x424967496E74 // "BigInt"

/// @brief Sign-magnitude payload stored inside each runtime BigInt object.
///
/// Only the first `len` limbs are significant. Limbs are little-endian,
/// `len <= cap`, and normalized zero has `len == 0` and `sign == 0`.
typedef struct {
    uint32_t *digits; // Little-endian digits (least significant first)
    int64_t len;      // Number of digits
    int64_t cap;      // Capacity
    int sign;         // 0 = non-negative, 1 = negative
} bigint_t;

/// @brief Validate a generic object operand as a BigInt (or null).
/// @details Returns 1 when @p obj is null (each caller applies its own null
///          semantics — e.g. Add treats null as zero) OR a genuine BigInt
///          instance. For any other object it traps and returns 0, so the
///          operation stops before casting the payload to `bigint_t` and
///          dereferencing unrelated memory as digits/len/cap/sign (VDOC-204).
static int bigint_check(void *obj) {
    if (!obj)
        return 1;
    if (!rt_obj_is_instance(obj, BIGINT_CLASS_ID, sizeof(bigint_t))) {
        rt_trap("BigInt: invalid BigInt object");
        return 0;
    }
    return 1;
}

//=============================================================================
// Memory Management
//=============================================================================

/// @brief Releases the separately allocated digit array during object finalization.
/// @param obj BigInt payload being finalized.
static void bigint_finalizer(void *obj);

/// @brief Releases an internally owned BigInt runtime reference.
/// @param bi Nullable BigInt reference to release and possibly free.
static void bigint_release_owned(bigint_t *bi);

/// @brief Checked signed 64-bit addition for digit counts.
/// @details BigInt capacity and length values are stored as int64_t.  Any
///          arithmetic that computes a new digit count must fail before signed
///          overflow occurs, otherwise later allocation sizes can wrap or
///          underallocate.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum on success.
/// @return 1 on success, 0 on overflow.
static int bigint_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
    if (!out)
        return 0;
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return 0;
    *out = a + b;
    return 1;
}

/// @brief Checked signed 64-bit multiplication for allocation bounds.
/// @details Used when estimating output string or byte-buffer sizes from the
///          number of base-2^32 limbs.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the product on success.
/// @return 1 on success, 0 on overflow.
static int bigint_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
    if (!out)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 0;
        } else if (b < INT64_MIN / a) {
            return 0;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 0;
        } else if (a < INT64_MAX / b) {
            return 0;
        }
    }
    *out = a * b;
    return 1;
#endif
}

/// @brief Validate a requested digit capacity before allocating.
/// @param capacity Number of uint32_t limbs requested.
/// @return Non-zero when the capacity is positive and fits in size_t bytes.
static int bigint_capacity_is_allocable(int64_t capacity) {
    return capacity > 0 && (uint64_t)capacity <= SIZE_MAX / sizeof(uint32_t);
}

/// @brief Allocate a GC-managed BigInt with the given digit capacity.
/// @details Creates the BigInt as a runtime object via rt_obj_new_i64 so the
///          garbage collector can track it. The digit array is a separate heap
///          allocation (calloc) because it may need to grow via realloc — which
///          is incompatible with the fixed-layout GC object. The finalizer
///          (bigint_finalizer) ensures the digit array is freed when the GC
///          reclaims the outer object.
/// @param capacity Initial number of uint32 digit slots to allocate.
/// @return Initialized GC-managed payload, or `NULL` after an allocation trap.
/// @note Nonpositive requested capacity is normalized to four limbs.
static bigint_t *bigint_alloc(int64_t capacity) {
    void *obj = rt_obj_new_i64(BIGINT_CLASS_ID, sizeof(bigint_t));
    if (!obj)
        return NULL;

    bigint_t *bi = (bigint_t *)obj;
    bi->cap = capacity > 0 ? capacity : 4;
    if (!bigint_capacity_is_allocable(bi->cap)) {
        rt_trap("bigint: capacity overflow");
        bigint_release_owned(bi);
        return NULL;
    }
    bi->digits = calloc((size_t)bi->cap, sizeof(uint32_t));
    if (!bi->digits) {
        rt_trap("bigint: memory allocation failed");
        bigint_release_owned(bi);
        return NULL;
    }
    bi->len = 0;
    bi->sign = 0;

    rt_obj_set_finalizer(obj, bigint_finalizer);
    return bi;
}

/// @brief Free the digit array owned by a BigInt object.
/// @details Called automatically by the GC when the BigInt's refcount drops to
///          zero. The digit array was allocated separately from the GC object
///          (see bigint_alloc), so it must be freed explicitly here. Nulling
///          the pointer prevents double-free if the finalizer runs twice during
///          shutdown sweeps.
/// @param obj Pointer to the bigint_t payload (cast from void* by GC).
static void bigint_finalizer(void *obj) {
    if (!obj)
        return;
    bigint_t *bi = (bigint_t *)obj;
    if (bi->digits) {
        free(bi->digits);
        bi->digits = NULL;
    }
}

/// @brief Drop a temporary owned reference to a BigInt.
///
/// Decrements the GC refcount and frees the object via
/// `rt_obj_free` if the count hits zero. Used by helpers that
/// allocate intermediate BigInts they don't return to the caller
/// (e.g. quotient discarded after a remainder calculation).
/// @param bi Nullable owned reference to release.
static void bigint_release_owned(bigint_t *bi) {
    if (bi && rt_obj_release_check0(bi))
        rt_obj_free(bi);
}

/// @brief Grow the digit array if current capacity is insufficient.
/// @details Doubles the existing capacity (or uses @p cap, whichever is larger)
///          to amortize growth cost over many operations. The new region is
///          zero-filled so arithmetic routines can safely read beyond the current
///          len without encountering uninitialized data. Traps on overflow or
///          allocation failure and returns 0 so trap-recovering tests/embedders
///          do not continue with an undersized digit buffer.
/// @param bi BigInt whose digit array may need expansion.
/// @param cap Minimum number of digit slots required.
/// @return Nonzero when capacity is sufficient; zero after an overflow or
///         allocation trap.
static int bigint_ensure_capacity(bigint_t *bi, int64_t cap) {
    if (cap <= bi->cap)
        return 1;

    if (bi->cap > INT64_MAX / 2) {
        rt_trap("bigint: capacity overflow");
        return 0;
    }
    int64_t new_cap = bi->cap * 2;
    if (new_cap < cap)
        new_cap = cap;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(uint32_t)) {
        rt_trap("bigint: allocation size overflow");
        return 0;
    }

    uint32_t *new_digits = realloc(bi->digits, (size_t)new_cap * sizeof(uint32_t));
    if (!new_digits) {
        rt_trap("bigint: memory allocation failed");
        return 0;
    }
    memset(new_digits + bi->cap, 0, (size_t)(new_cap - bi->cap) * sizeof(uint32_t));
    bi->digits = new_digits;
    bi->cap = new_cap;
    return 1;
}

/// @brief Strip trailing zero digits and normalize the sign (-0 → +0).
/// @details Every arithmetic operation must call this before returning to
///          maintain the invariant that the digit array has no leading zeros
///          (index len-1 is always nonzero, or len is 0 for the value zero).
///          Without normalization, comparison and printing would produce
///          incorrect results because they rely on len to determine magnitude.
///          The -0 → +0 rule prevents sign-related edge cases in division
///          and comparison.
/// @param bi BigInt to normalize in-place.
static void bigint_normalize(bigint_t *bi) {
    while (bi->len > 0 && bi->digits[bi->len - 1] == 0)
        bi->len--;

    // Zero is always non-negative
    if (bi->len == 0)
        bi->sign = 0;
}

/// @brief Deep-copy a BigInt including its digit array.
/// @details Allocates a fresh GC-managed BigInt and copies all digits from @p a.
///          Needed because BigInt arithmetic is non-mutating — every operation
///          returns a new object rather than modifying an operand in place.
///          The clone inherits the sign and length but gets its own digit storage.
/// @param a Source BigInt to copy.
/// @return New BigInt with identical value, or `NULL` after allocation failure.
static bigint_t *bigint_clone(bigint_t *a) {
    bigint_t *result = bigint_alloc(a->len);
    if (!result)
        return NULL;

    result->len = a->len;
    result->sign = a->sign;
    memcpy(result->digits, a->digits, (size_t)a->len * sizeof(uint32_t));
    return result;
}

//=============================================================================
// BigInt Creation
//=============================================================================

/// @brief Create a BigInt from a signed 64-bit integer.
/// @details Decomposes the value into base-2^32 digits stored little-endian.
///          Handles INT64_MIN specially because negating it overflows int64 —
///          we cast to uint64 first and add 1 to INT64_MAX instead. Zero is
///          represented as len=0, sign=0 (no digits needed). Negative values
///          store the magnitude in digits and set sign=1.
/// @param val The 64-bit integer to convert.
/// @return New GC-managed BigInt (refcount=1), or NULL on allocation failure.
void *rt_bigint_from_i64(int64_t val) {
    bigint_t *bi = bigint_alloc(2);
    if (!bi)
        return NULL;

    if (val == 0) {
        bi->len = 0;
        bi->sign = 0;
    } else if (val < 0) {
        bi->sign = 1;
        uint64_t uval;
        if (val == INT64_MIN) {
            uval = (uint64_t)INT64_MAX + 1;
        } else {
            uval = (uint64_t)(-val);
        }

        bi->digits[0] = (uint32_t)(uval & 0xFFFFFFFF);
        bi->digits[1] = (uint32_t)(uval >> 32);
        bi->len = (bi->digits[1] != 0) ? 2 : 1;
    } else {
        bi->sign = 0;
        bi->digits[0] = (uint32_t)(val & 0xFFFFFFFF);
        bi->digits[1] = (uint32_t)((uint64_t)val >> 32);
        bi->len = (bi->digits[1] != 0) ? 2 : 1;
    }

    return bi;
}

/// @brief Parse a BigInt from decimal or 0x/0b/0o-prefixed text.
/// @details Skips leading whitespace, handles an optional sign character, then
///          detects `0x`, `0b`, or `0o` to choose base 16, 2, or 8. Digits are
///          accumulated using grade-school multiply-and-add: for each digit d,
///          result = result * base + d. Returns NULL for empty or non-numeric
///          input rather than trapping, so callers can provide their own
///          diagnostic. Underscores are ignored wherever they occur within the
///          digit scan; trailing space, tab, CR, and LF are accepted. The result
///          is always normalized (no leading zeros).
/// @param str Runtime string containing the number text.
/// @return New BigInt, or NULL if the string is empty, NULL, or invalid.
void *rt_bigint_from_str(rt_string str) {
    if (!str)
        return NULL;

    const char *s = rt_string_cstr(str);
    if (!s)
        return NULL;

    int64_t slen = rt_str_len(str);
    if (slen == 0)
        return NULL;

    int sign = 0;
    int64_t i = 0;

    // Skip leading whitespace
    while (i < slen && (s[i] == ' ' || s[i] == '\t'))
        i++;

    // Check sign
    if (i < slen && s[i] == '-') {
        sign = 1;
        i++;
    } else if (i < slen && s[i] == '+') {
        i++;
    }

    // Check base
    int base = 10;
    if (i + 1 < slen && s[i] == '0') {
        if (s[i + 1] == 'x' || s[i + 1] == 'X') {
            base = 16;
            i += 2;
        } else if (s[i + 1] == 'b' || s[i + 1] == 'B') {
            base = 2;
            i += 2;
        } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
            base = 8;
            i += 2;
        }
    }

    bigint_t *result = bigint_alloc(4);
    if (!result)
        return NULL;

    result->len = 0;
    result->sign = 0;
    int saw_digit = 0;

    while (i < slen) {
        char c = s[i];
        int digit;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = c - 'A' + 10;
        } else if (c == '_') {
            // Allow underscores as separators
            i++;
            continue;
        } else {
            break;
        }

        if (digit >= base)
            break;
        saw_digit = 1;

        // result = result * base + digit
        uint64_t carry = (uint64_t)digit;
        for (int64_t j = 0; j < result->len; j++) {
            uint64_t prod = (uint64_t)result->digits[j] * (uint64_t)base + carry;
            result->digits[j] = (uint32_t)(prod & 0xFFFFFFFF);
            carry = prod >> 32;
        }

        while (carry > 0) {
            if (!bigint_ensure_capacity(result, result->len + 1)) {
                bigint_release_owned(result);
                return NULL;
            }
            result->digits[result->len] = (uint32_t)(carry & 0xFFFFFFFF);
            result->len++;
            carry >>= 32;
        }

        i++;
    }

    while (i < slen && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        i++;
    if (!saw_digit || i != slen) {
        bigint_release_owned(result);
        return NULL;
    }

    result->sign = (result->len > 0) ? sign : 0;
    bigint_normalize(result);
    return result;
}

/// @brief Creates a BigInt from a big-endian two's-complement byte array.
/// @param bytes Runtime Bytes object to decode; null/empty denotes zero.
/// @return New normalized BigInt with the decoded signed value.
/// @note Redundant leading sign-extension bytes are accepted.
void *rt_bigint_from_bytes(void *bytes) {
    if (!bytes)
        return rt_bigint_zero();

    int64_t len = rt_bytes_len(bytes);
    if (len == 0)
        return rt_bigint_zero();

    // Get first byte to determine sign
    uint8_t first = (uint8_t)rt_bytes_get(bytes, 0);
    int sign = (first & 0x80) ? 1 : 0;

    // Find first significant byte
    int64_t start = 0;
    if (sign) {
        while (start < len - 1 && rt_bytes_get(bytes, start) == 0xFF)
            start++;
    } else {
        while (start < len - 1 && rt_bytes_get(bytes, start) == 0)
            start++;
    }

    int64_t significant_len = len - start;
    int64_t rounded_significant_len;
    int64_t num_digits;
    int64_t alloc_digits;
    if (!bigint_checked_add_i64(significant_len, 3, &rounded_significant_len) ||
        (num_digits = rounded_significant_len / 4) < 0 ||
        !bigint_checked_add_i64(num_digits, 1, &alloc_digits)) {
        rt_trap("bigint: byte input size overflow");
        return NULL;
    }

    bigint_t *bi = bigint_alloc(alloc_digits);
    if (!bi)
        return NULL;

    if (sign) {
        // Two's complement negative: invert and add 1
        uint64_t carry = 1;
        for (int64_t i = len - 1, d = 0; i >= 0; i -= 4) {
            uint32_t word = 0;
            for (int j = 0; j < 4 && i - j >= 0; j++) {
                uint8_t b = (uint8_t)rt_bytes_get(bytes, i - j);
                word |= ((uint32_t)(~b & 0xFF)) << (j * 8);
            }
            uint64_t sum = (uint64_t)word + carry;
            if (!bigint_ensure_capacity(bi, d + 1)) {
                bigint_release_owned(bi);
                return NULL;
            }
            bi->digits[d] = (uint32_t)sum;
            carry = sum >> 32;
            d++;
            if (bi->len < d)
                bi->len = d;
        }
        bi->sign = 1;
    } else {
        // Positive: straightforward
        for (int64_t i = len - 1, d = 0; i >= 0; i -= 4) {
            uint32_t word = 0;
            for (int j = 0; j < 4 && i - j >= 0; j++) {
                uint8_t b = (uint8_t)rt_bytes_get(bytes, i - j);
                word |= ((uint32_t)b) << (j * 8);
            }
            if (!bigint_ensure_capacity(bi, d + 1)) {
                bigint_release_owned(bi);
                return NULL;
            }
            bi->digits[d] = word;
            d++;
            if (bi->len < d)
                bi->len = d;
        }
        bi->sign = 0;
    }

    bigint_normalize(bi);
    return bi;
}

/// @brief Return a new BigInt representing zero.
/// @return New GC-managed zero value.
void *rt_bigint_zero(void) {
    return rt_bigint_from_i64(0);
}

/// @brief Return a new BigInt representing one.
/// @return New GC-managed one value.
void *rt_bigint_one(void) {
    return rt_bigint_from_i64(1);
}

//=============================================================================
// Conversion
//=============================================================================

/// @brief Convert BigInt to int64, saturating if the value exceeds 64-bit range.
/// @details Extracts the lower 64 bits of the magnitude and applies the sign.
///          Values larger than INT64_MAX clamp to INT64_MAX, while values below
///          INT64_MIN clamp to INT64_MIN. Callers can use rt_bigint_fits_i64
///          to detect lossy narrowing ahead of time.
/// @param a BigInt to convert.
/// @return int64_t representation, saturated when out of range.
/// @note A null operand converts to zero.
int64_t rt_bigint_to_i64(void *a) {
    if (!bigint_check(a))
        return 0;
    if (!a)
        return 0;

    bigint_t *bi = (bigint_t *)a;

    if (bi->len == 0)
        return 0;

    uint64_t val = 0;
    for (int64_t i = bi->len - 1; i >= 0 && i < 2; i--) {
        val = (val << 32) | bi->digits[i];
    }

    if (bi->len > 2 || val > (uint64_t)INT64_MAX + (bi->sign ? 1ULL : 0ULL)) {
        // Overflow - saturate
        if (bi->sign)
            return INT64_MIN;
        return INT64_MAX;
    }

    if (bi->sign) {
        if (val == (uint64_t)INT64_MAX + 1ULL)
            return INT64_MIN;
        return -(int64_t)val;
    }
    return (int64_t)val;
}

/// @brief Convert BigInt to its decimal string representation.
/// @details Delegates to rt_bigint_to_str_base with base 10.
/// @param a BigInt to convert.
/// @return Newly allocated runtime string with the decimal text.
rt_string rt_bigint_to_str(void *a) {
    return rt_bigint_to_str_base(a, 10);
}

/// @brief Convert BigInt to a string in the specified base (2-36).
/// @details Repeatedly divides the magnitude by the base, collecting remainders
///          as digits. Digits above 9 use lowercase letters (a-z). Prepends '-'
///          for negative values. Traps on invalid base (< 2 or > 36).
/// @param a BigInt to convert.
/// @param base Radix for the output (2-36).
/// @return Newly allocated lowercase runtime string, or `NULL` after a trap.
/// @note A null BigInt formats as `0`.
rt_string rt_bigint_to_str_base(void *a, int64_t base) {
    if (!bigint_check(a))
        return rt_string_from_bytes("0", 1);
    if (!a)
        return rt_string_from_bytes("0", 1);

    if (base < 2 || base > 36) {
        rt_trap("BigInt.ToString: base must be between 2 and 36");
        return NULL;
    }

    bigint_t *bi = (bigint_t *)a;

    if (bi->len == 0)
        return rt_string_from_bytes("0", 1);

    // Work with a copy
    bigint_t *tmp = bigint_clone(bi);
    if (!tmp)
        return NULL;

    // Estimate size: safe upper bound covering all bases (base 2 = 32 bits/limb)
    int64_t limb_chars;
    int64_t max_chars;
    if (!bigint_checked_mul_i64(bi->len, 33, &limb_chars) ||
        !bigint_checked_add_i64(limb_chars, 4, &max_chars) ||
        (uint64_t)max_chars > (uint64_t)SIZE_MAX) {
        rt_trap("bigint: string size overflow");
        bigint_release_owned(tmp);
        return NULL;
    }
    char *buf = malloc((size_t)max_chars);
    if (!buf) {
        rt_trap("bigint: memory allocation failed");
        bigint_release_owned(tmp);
        return NULL;
    }
    int64_t pos = max_chars - 1;
    buf[pos--] = '\0';

    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";

    while (tmp->len > 0) {
        uint64_t remainder = 0;
        for (int64_t i = tmp->len - 1; i >= 0; i--) {
            uint64_t cur = (remainder << 32) | tmp->digits[i];
            tmp->digits[i] = (uint32_t)(cur / (uint64_t)base);
            remainder = cur % (uint64_t)base;
        }
        buf[pos--] = digits[remainder];
        bigint_normalize(tmp);
    }

    if (bi->sign)
        buf[pos--] = '-';

    rt_string result = rt_string_from_bytes(buf + pos + 1, max_chars - pos - 2);
    free(buf);

    bigint_release_owned(tmp);

    return result;
}

/// @brief Convert BigInt to a big-endian two's-complement byte array.
/// @param a BigInt to encode; null denotes zero.
/// @return New runtime Bytes object containing a minimal signed encoding.
/// @details Adds a leading `0x00` or `0xFF` only when required to preserve the
///          sign. The zero encoding is the single byte `0x00`.
void *rt_bigint_to_bytes(void *a) {
    if (!bigint_check(a))
        return NULL;
    if (!a) {
        void *b = rt_bytes_new(1);
        if (!b)
            return NULL;
        rt_bytes_set(b, 0, 0);
        return b;
    }

    bigint_t *bi = (bigint_t *)a;

    if (bi->len == 0) {
        void *b = rt_bytes_new(1);
        if (!b)
            return NULL;
        rt_bytes_set(b, 0, 0);
        return b;
    }

    // Calculate byte length
    int64_t byte_len;
    if (!bigint_checked_mul_i64(bi->len, 4, &byte_len)) {
        rt_trap("bigint: byte size overflow");
        return NULL;
    }
    // Trim leading zeros
    while (byte_len > 1) {
        int64_t idx = byte_len - 1;
        int64_t digit_idx = idx / 4;
        int64_t byte_idx = idx % 4;
        if (digit_idx >= bi->len)
            break;
        uint8_t b = (bi->digits[digit_idx] >> (byte_idx * 8)) & 0xFF;
        if (b != 0)
            break;
        byte_len--;
    }

    // Materialize the big-endian two's-complement bytes of the value in exactly
    // `byte_len` bytes into a temp buffer FIRST, then decide the sign prefix from
    // the RESULT's top bit — not the magnitude's. Deciding from the magnitude's
    // high bit was wrong for negatives (e.g. -129 has magnitude top bit 0x81 set,
    // but its 1-byte two's complement is 0x7F with the top bit clear, so it
    // silently encoded as +127) (VDOC-203).
    uint8_t *tmp = (uint8_t *)malloc((size_t)byte_len);
    if (!tmp) {
        rt_trap("bigint: memory allocation failed");
        return NULL;
    }
    if (bi->sign) {
        uint32_t carry = 1;
        for (int64_t i = 0; i < byte_len; i++) {
            int64_t digit_idx = i / 4;
            int64_t byte_idx = i % 4;
            uint8_t b = 0;
            if (digit_idx < bi->len)
                b = (bi->digits[digit_idx] >> (byte_idx * 8)) & 0xFF;
            uint32_t sum = ((~b) & 0xFF) + carry;
            tmp[byte_len - 1 - i] = (uint8_t)(sum & 0xFF);
            carry = sum >> 8;
        }
    } else {
        for (int64_t i = 0; i < byte_len; i++) {
            int64_t digit_idx = i / 4;
            int64_t byte_idx = i % 4;
            uint8_t b = 0;
            if (digit_idx < bi->len)
                b = (bi->digits[digit_idx] >> (byte_idx * 8)) & 0xFF;
            tmp[byte_len - 1 - i] = b;
        }
    }

    // A negative value needs a leading 0xFF exactly when the encoded top bit is
    // clear (else it reads as positive); a positive value needs a leading 0x00
    // exactly when the encoded top bit is set (else it reads as negative).
    int need_sign = 0;
    if (bi->sign && !(tmp[0] & 0x80))
        need_sign = 1;
    if (!bi->sign && (tmp[0] & 0x80))
        need_sign = 1;

    int64_t result_len;
    if (!bigint_checked_add_i64(byte_len, need_sign, &result_len)) {
        free(tmp);
        rt_trap("bigint: byte size overflow");
        return NULL;
    }
    void *result = rt_bytes_new(result_len);
    if (!result) {
        free(tmp);
        return NULL;
    }
    if (need_sign)
        rt_bytes_set(result, 0, bi->sign ? 0xFF : 0x00);
    for (int64_t i = 0; i < byte_len; i++)
        rt_bytes_set(result, need_sign + i, tmp[i]);
    free(tmp);

    return result;
}

/// @brief Check whether the BigInt value fits in a signed 64-bit integer.
/// @details Compares the magnitude against INT64_MAX (for non-negative) or
///          INT64_MIN (for negative). Used by callers to guard against lossy
///          narrowing before calling rt_bigint_to_i64.
/// @param a BigInt to test.
/// @return 1 if the value is representable as int64, 0 otherwise.
/// @note A null operand denotes zero and therefore fits.
int8_t rt_bigint_fits_i64(void *a) {
    if (!bigint_check(a))
        return 1;
    if (!a)
        return 1;

    bigint_t *bi = (bigint_t *)a;

    if (bi->len == 0)
        return 1;
    if (bi->len > 2)
        return 0;

    uint64_t val = bi->digits[0];
    if (bi->len == 2)
        val |= ((uint64_t)bi->digits[1]) << 32;

    uint64_t max = bi->sign ? ((uint64_t)INT64_MAX + 1) : (uint64_t)INT64_MAX;
    return val <= max ? 1 : 0;
}

//=============================================================================
// Internal Arithmetic Helpers
//=============================================================================

/// @brief Compare absolute values of two BigInts (sign ignored).
///
/// Magnitudes are stored little-endian in `digits`, so the
/// most-significant digit lives at index `len-1`. A length
/// difference is decisive (no leading zeros after `bigint_normalize`),
/// otherwise we walk from the top digit downward and stop at the
/// first inequality.
/// @param a First normalized magnitude.
/// @param b Second normalized magnitude.
/// @return >0 if |a|>|b|, <0 if |a|<|b|, 0 if equal.
static int bigint_cmp_mag(bigint_t *a, bigint_t *b) {
    if (a->len != b->len)
        return (a->len > b->len) ? 1 : -1;

    for (int64_t i = a->len - 1; i >= 0; i--) {
        if (a->digits[i] != b->digits[i])
            return (a->digits[i] > b->digits[i]) ? 1 : -1;
    }
    return 0;
}

/// @brief Add two BigInt magnitudes: returns a freshly-allocated `|a| + |b|`.
///
/// School-book addition over base-2^32 digits: walk both digit
/// arrays from least- to most-significant, propagating a 64-bit
/// carry to absorb the overflow of two 32-bit additions plus
/// carry. Capacity is grown lazily so an addition that produces a
/// new top digit doesn't require a separate pass.
/// @param a First normalized magnitude.
/// @param b Second normalized magnitude.
/// @return New normalized BigInt with `sign = 0`, or NULL on alloc failure.
static bigint_t *bigint_add_mag(bigint_t *a, bigint_t *b) {
    int64_t max_len = (a->len > b->len) ? a->len : b->len;
    bigint_t *result = bigint_alloc(max_len + 1);
    if (!result)
        return NULL;

    uint64_t carry = 0;
    for (int64_t i = 0; i < max_len || carry; i++) {
        if (!bigint_ensure_capacity(result, i + 1)) {
            bigint_release_owned(result);
            return NULL;
        }
        uint64_t sum = carry;
        if (i < a->len)
            sum += a->digits[i];
        if (i < b->len)
            sum += b->digits[i];
        result->digits[i] = (uint32_t)(sum & 0xFFFFFFFF);
        carry = sum >> 32;
        if (result->len <= i)
            result->len = i + 1;
    }

    bigint_normalize(result);
    return result;
}

/// @brief Subtract two BigInt magnitudes: returns `|a| - |b|`.
///
/// Caller must guarantee `|a| >= |b|` (typically via
/// `bigint_cmp_mag`); otherwise the borrow overflows on the top
/// digit and the result is garbage. School-book subtraction with a
/// 1-bit borrow flag, base 2^32. The result is normalized so any
/// leading zero digits introduced by cancellation are trimmed.
/// @param a Magnitude from which @p b is subtracted.
/// @param b Magnitude no larger than @p a.
/// @return New normalized non-negative BigInt, or NULL on alloc failure.
static bigint_t *bigint_sub_mag(bigint_t *a, bigint_t *b) {
    bigint_t *result = bigint_alloc(a->len);
    if (!result)
        return NULL;

    int64_t borrow = 0;
    for (int64_t i = 0; i < a->len; i++) {
        int64_t diff = (int64_t)a->digits[i] - borrow;
        if (i < b->len)
            diff -= b->digits[i];

        if (diff < 0) {
            diff += BIGINT_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }

        if (!bigint_ensure_capacity(result, i + 1)) {
            bigint_release_owned(result);
            return NULL;
        }
        result->digits[i] = (uint32_t)diff;
        if (result->len <= i)
            result->len = i + 1;
    }

    bigint_normalize(result);
    return result;
}

/// @brief Add one to a non-negative BigInt magnitude in place.
/// @details Used by arithmetic right shift of negative values. In sign-magnitude
///          form, `-x >> n` equals `-(floor(x / 2^n) + 1)` when any shifted-out
///          bit is non-zero. This helper performs the `+ 1` on the magnitude
///          with carry propagation and capacity growth.
/// @param bi Non-negative BigInt magnitude to increment.
/// @return 1 on success, 0 if capacity growth fails.
static int bigint_add_one_mag_inplace(bigint_t *bi) {
    if (!bi)
        return 0;
    uint64_t carry = 1;
    int64_t i = 0;
    while (carry) {
        if (!bigint_ensure_capacity(bi, i + 1))
            return 0;
        uint64_t sum = carry;
        if (i < bi->len)
            sum += bi->digits[i];
        bi->digits[i] = (uint32_t)(sum & 0xFFFFFFFFu);
        carry = sum >> 32;
        if (bi->len <= i)
            bi->len = i + 1;
        i++;
    }
    bigint_normalize(bi);
    return 1;
}

/// @brief Compute a fixed limb width for two's-complement bitwise operations.
/// @details Arbitrary-precision negative integers conceptually have infinite
///          leading one bits. For a finite operation, using one extra limb beyond
///          the widest magnitude preserves the sign bit and leaves enough room
///          to convert the result back to sign-magnitude form unambiguously.
/// @param a First operand, or NULL for zero.
/// @param b Second operand, or NULL for zero.
/// @param out Receives the limb width.
/// @return 1 on success, 0 if the width would overflow allocation limits.
static int bigint_twos_width(bigint_t *a, bigint_t *b, int64_t *out) {
    int64_t a_len = a ? a->len : 0;
    int64_t b_len = b ? b->len : 0;
    int64_t max_len = a_len > b_len ? a_len : b_len;
    if (!bigint_checked_add_i64(max_len, 1, out) || !bigint_capacity_is_allocable(*out))
        return 0;
    return 1;
}

/// @brief Encode a sign-magnitude BigInt into fixed-width two's-complement limbs.
/// @details Positive values are copied directly and zero-padded. Negative values
///          are converted by copying their magnitude, inverting every fixed-width
///          limb, and adding one. The output buffer is little-endian like the
///          BigInt digit array.
/// @param bi Source BigInt, or NULL for zero.
/// @param words Output limb buffer.
/// @param width Number of limbs available in @p words.
/// @return 1 on success, 0 if @p bi cannot fit in @p width.
static int bigint_to_twos_words(bigint_t *bi, uint32_t *words, int64_t width) {
    if (!words || width <= 0)
        return 0;
    memset(words, 0, (size_t)width * sizeof(uint32_t));
    if (!bi || bi->len == 0)
        return 1;
    if (bi->len > width)
        return 0;
    memcpy(words, bi->digits, (size_t)bi->len * sizeof(uint32_t));
    if (!bi->sign)
        return 1;

    for (int64_t i = 0; i < width; i++)
        words[i] = ~words[i];
    uint64_t carry = 1;
    for (int64_t i = 0; i < width && carry; i++) {
        uint64_t sum = (uint64_t)words[i] + carry;
        words[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    return 1;
}

/// @brief Decode fixed-width two's-complement limbs into a BigInt.
/// @details The top bit of the highest fixed-width limb determines whether the
///          encoded value is negative. Negative values are converted back to
///          magnitude by inverting all limbs and adding one, then setting the
///          sign flag after normalization.
/// @param words Little-endian two's-complement limbs.
/// @param width Number of limbs in @p words.
/// @return Newly allocated sign-magnitude BigInt, or NULL on allocation failure.
static bigint_t *bigint_from_twos_words(const uint32_t *words, int64_t width) {
    if (!words || width <= 0)
        return (bigint_t *)rt_bigint_zero();

    int negative = (words[width - 1] & 0x80000000u) != 0;
    bigint_t *result = bigint_alloc(width);
    if (!result)
        return NULL;

    result->len = width;
    if (!negative) {
        memcpy(result->digits, words, (size_t)width * sizeof(uint32_t));
        result->sign = 0;
        bigint_normalize(result);
        return result;
    }

    for (int64_t i = 0; i < width; i++)
        result->digits[i] = ~words[i];
    result->sign = 0;
    if (!bigint_add_one_mag_inplace(result)) {
        bigint_release_owned(result);
        return NULL;
    }
    if (result->len > 0)
        result->sign = 1;
    bigint_normalize(result);
    return result;
}

/// @brief Apply a BigInt bitwise operator using arbitrary-width two's-complement semantics.
/// @details Handles positive and negative operands uniformly by converting both
///          operands into a sign-preserving fixed-width two's-complement buffer,
///          applying the requested operation limb-by-limb, then converting the
///          finite result back to the runtime's sign-magnitude representation.
/// @param a First operand, or NULL for zero.
/// @param b Second operand, or NULL for zero.
/// @param op Operation selector: '&', '|', or '^'.
/// @return Newly allocated BigInt result, or NULL on failure.
static bigint_t *bigint_bitwise_twos(bigint_t *a, bigint_t *b, char op) {
    int64_t width;
    if (!bigint_twos_width(a, b, &width)) {
        rt_trap("bigint: bitwise size overflow");
        return NULL;
    }

    uint32_t *aw = (uint32_t *)calloc((size_t)width, sizeof(uint32_t));
    uint32_t *bw = (uint32_t *)calloc((size_t)width, sizeof(uint32_t));
    if (!aw || !bw) {
        free(aw);
        free(bw);
        rt_trap("bigint: memory allocation failed");
        return NULL;
    }

    if (!bigint_to_twos_words(a, aw, width) || !bigint_to_twos_words(b, bw, width)) {
        free(aw);
        free(bw);
        rt_trap("bigint: bitwise conversion failed");
        return NULL;
    }

    for (int64_t i = 0; i < width; i++) {
        if (op == '&')
            aw[i] &= bw[i];
        else if (op == '|')
            aw[i] |= bw[i];
        else
            aw[i] ^= bw[i];
    }

    bigint_t *result = bigint_from_twos_words(aw, width);
    free(aw);
    free(bw);
    return result;
}

/// @brief Test whether any magnitude bits below @p n are set.
/// @details Arithmetic right shift of negative values must know whether the
///          discarded magnitude has a non-zero remainder so it can round toward
///          negative infinity. This scans only the affected low limbs.
/// @param bi Source BigInt magnitude.
/// @param n Number of low bits being discarded.
/// @return 1 if any discarded bit is set, 0 otherwise.
static int bigint_magnitude_has_low_bits(bigint_t *bi, int64_t n) {
    if (!bi || bi->len == 0 || n <= 0)
        return 0;

    int64_t full_words = n / 32;
    int bit_count = (int)(n % 32);
    int64_t limit = full_words < bi->len ? full_words : bi->len;
    for (int64_t i = 0; i < limit; i++) {
        if (bi->digits[i] != 0)
            return 1;
    }
    if (bit_count > 0 && full_words < bi->len) {
        uint32_t mask = (uint32_t)((1ULL << bit_count) - 1ULL);
        if ((bi->digits[full_words] & mask) != 0)
            return 1;
    }
    return 0;
}

/// @brief Logical right shift of a BigInt magnitude.
/// @details Ignores the sign flag and shifts only the absolute-value digit array.
///          The caller chooses the final sign. Used directly for positive values
///          and as the quotient part of arithmetic right shift for negatives.
/// @param bi Source BigInt magnitude.
/// @param n Number of bits to shift right.
/// @return Newly allocated non-negative shifted magnitude.
static bigint_t *bigint_shr_magnitude(bigint_t *bi, int64_t n) {
    if (!bi || bi->len == 0)
        return (bigint_t *)rt_bigint_zero();

    int64_t word_shift = n / 32;
    int bit_shift = (int)(n % 32);
    if (word_shift >= bi->len)
        return (bigint_t *)rt_bigint_zero();

    int64_t new_len = bi->len - word_shift;
    bigint_t *result = bigint_alloc(new_len);
    if (!result)
        return NULL;

    result->len = new_len;
    result->sign = 0;
    uint32_t carry = 0;
    for (int64_t i = new_len - 1; i >= 0; i--) {
        uint64_t val = ((uint64_t)carry << 32) | bi->digits[i + word_shift];
        result->digits[i] = (uint32_t)(val >> bit_shift);
        carry = bit_shift ? (uint32_t)(val & ((1ULL << bit_shift) - 1ULL)) : 0;
    }

    bigint_normalize(result);
    return result;
}

/// @brief Build a non-negative BigInt containing exactly bit @p n.
/// @details Centralizes bounds checks for public bit-manipulation helpers so
///          huge bit indexes trap before `word + 1` or allocation byte counts
///          overflow.
/// @param n Zero-based bit index.
/// @return A BigInt mask with bit @p n set, or NULL on overflow/allocation failure.
static bigint_t *bigint_single_bit_mask(int64_t n) {
    if (n < 0)
        return (bigint_t *)rt_bigint_zero();

    int64_t word = n / 32;
    int bit = (int)(n % 32);
    int64_t new_len;
    if (!bigint_checked_add_i64(word, 1, &new_len)) {
        rt_trap("bigint: bit index overflow");
        return NULL;
    }

    bigint_t *mask = bigint_alloc(new_len);
    if (!mask)
        return NULL;
    mask->len = new_len;
    mask->sign = 0;
    mask->digits[word] = 1U << bit;
    bigint_normalize(mask);
    return mask;
}

//=============================================================================
// Basic Arithmetic
//=============================================================================

/// @brief Add two BigInt operands without receiver validation.
/// @details Internal core shared by the public `rt_bigint_add` (which validates
///          first) and `rt_bigint_sub`, which passes a trusted STACK-allocated
///          negated copy of b. Internal callers pass already-trusted operands
///          (heap instances or internal stack temps), so re-validating here
///          would wrongly reject the stack temp (VDOC-204).
/// @param a Trusted first operand, or null for zero.
/// @param b Trusted second operand, or null for zero.
/// @return New BigInt holding the sum, or `NULL` after allocation failure.
static void *bigint_add_impl(void *a, void *b) {
    if (!a)
        return b ? bigint_clone((bigint_t *)b) : rt_bigint_zero();
    if (!b)
        return bigint_clone((bigint_t *)a);

    bigint_t *bi_a = (bigint_t *)a;
    bigint_t *bi_b = (bigint_t *)b;

    if (bi_a->sign == bi_b->sign) {
        // Same sign: add magnitudes
        bigint_t *result = bigint_add_mag(bi_a, bi_b);
        if (result)
            result->sign = bi_a->sign;
        return result;
    } else {
        // Different signs: subtract magnitudes
        int cmp = bigint_cmp_mag(bi_a, bi_b);
        if (cmp == 0)
            return rt_bigint_zero();

        bigint_t *result;
        if (cmp > 0) {
            result = bigint_sub_mag(bi_a, bi_b);
            if (result)
                result->sign = bi_a->sign;
        } else {
            result = bigint_sub_mag(bi_b, bi_a);
            if (result)
                result->sign = bi_b->sign;
        }
        return result;
    }
}

/// @brief Adds two BigInts without consuming either operand.
/// @details Dispatches to magnitude addition for like signs and magnitude
///          subtraction for unlike signs. Null operands denote zero.
/// @param a First addend.
/// @param b Second addend.
/// @return New normalized BigInt holding `a + b`.
void *rt_bigint_add(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    return bigint_add_impl(a, b);
}

/// @brief Subtract two BigInts (a - b), returning a new result.
/// @details Implemented as a + (-b): negates the sign of b and delegates to
///          rt_bigint_add. This reuses the sign-dispatch logic in add rather
///          than duplicating it. The temporary negation is reverted after the
///          call so the original b is not mutated.
/// @param a Minuend (not consumed).
/// @param b Subtrahend (not consumed).
/// @return New BigInt holding a - b.
/// @note Null operands denote zero.
void *rt_bigint_sub(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    if (!b)
        return a ? bigint_clone((bigint_t *)a) : rt_bigint_zero();

    bigint_t *bi_b = (bigint_t *)b;

    // Negate b and add
    bigint_t neg_b = *bi_b;
    neg_b.sign = bi_b->sign ? 0 : 1;
    if (neg_b.len == 0)
        neg_b.sign = 0;

    // Use the unchecked add core: `a` is validated above and `&neg_b` is a
    // trusted internal stack temp that the public guard would reject (VDOC-204).
    return bigint_add_impl(a, &neg_b);
}

/// @brief Multiply two BigInts using grade-school long multiplication.
/// @details Allocates a result with len = a.len + b.len digits (the maximum
///          possible product width). For each pair of digits (a[i], b[j]),
///          computes the uint64 product and accumulates into result[i+j] with
///          carry propagation into result[i+j+1]. The sign of the result is
///          XOR of the input signs. Handles NULL/zero inputs as zero.
/// @param a First factor (not consumed).
/// @param b Second factor (not consumed).
/// @return New BigInt holding a * b.
void *rt_bigint_mul(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    if (!a || !b)
        return rt_bigint_zero();

    bigint_t *bi_a = (bigint_t *)a;
    bigint_t *bi_b = (bigint_t *)b;

    if (bi_a->len == 0 || bi_b->len == 0)
        return rt_bigint_zero();

    int64_t result_len;
    if (!bigint_checked_add_i64(bi_a->len, bi_b->len, &result_len)) {
        rt_trap("bigint: multiplication size overflow");
        return NULL;
    }

    bigint_t *result = bigint_alloc(result_len);
    if (!result)
        return NULL;

    result->len = result_len;
    memset(result->digits, 0, (size_t)result->len * sizeof(uint32_t));

    for (int64_t i = 0; i < bi_a->len; i++) {
        uint64_t carry = 0;
        for (int64_t j = 0; j < bi_b->len || carry; j++) {
            uint64_t prod = result->digits[i + j] + carry;
            if (j < bi_b->len)
                prod += (uint64_t)bi_a->digits[i] * bi_b->digits[j];
            result->digits[i + j] = (uint32_t)(prod & 0xFFFFFFFF);
            carry = prod >> 32;
        }
    }

    result->sign = (bi_a->sign != bi_b->sign) ? 1 : 0;
    bigint_normalize(result);
    return result;
}

/// @brief Divide two BigInts, producing quotient and optional remainder.
/// @details Three-path implementation:
///          1. Single-digit divisor → fast path using simple loop division.
///          2. Equal-length operands → magnitude comparison shortcut.
///          3. Multi-digit divisor → Knuth's Algorithm D (long division with
///             normalizing shift to ensure the leading divisor digit ≥ base/2,
///             trial quotient estimation via two-digit / one-digit, and
///             correction step for overestimates).
///          Sign of quotient = XOR of input signs. Remainder takes dividend's sign.
///          Traps on zero divisor. NULL inputs are treated as zero.
/// @param a Dividend (not consumed).
/// @param b Divisor (not consumed). Must be non-zero.
/// @param remainder If non-NULL, receives a new BigInt holding the remainder.
/// @return New BigInt holding the quotient (truncated toward zero).
/// @note The remainder has the dividend's sign and satisfies
///       `a == quotient * b + remainder` with `|remainder| < |b|`.
void *rt_bigint_divmod(void *a, void *b, void **remainder) {
    if (!bigint_check(a) || !bigint_check(b))
        return NULL;
    if (!b) {
        rt_trap("BigInt division by zero");
        return NULL;
    }

    bigint_t *bi_b = (bigint_t *)b;
    if (bi_b->len == 0) {
        rt_trap("BigInt division by zero");
        return NULL;
    }

    if (!a) {
        if (remainder)
            *remainder = rt_bigint_zero();
        return rt_bigint_zero();
    }

    bigint_t *bi_a = (bigint_t *)a;
    if (bi_a->len == 0) {
        if (remainder)
            *remainder = rt_bigint_zero();
        return rt_bigint_zero();
    }

    int cmp = bigint_cmp_mag(bi_a, bi_b);
    if (cmp < 0) {
        // |a| < |b|: quotient = 0, remainder = a
        if (remainder)
            *remainder = bigint_clone(bi_a);
        return rt_bigint_zero();
    }

    if (cmp == 0) {
        // |a| == |b|: quotient = +-1, remainder = 0
        if (remainder)
            *remainder = rt_bigint_zero();
        bigint_t *q = (bigint_t *)rt_bigint_one();
        if (q)
            q->sign = (bi_a->sign != bi_b->sign) ? 1 : 0;
        return q;
    }

    // Simple long division for single-digit divisor
    if (bi_b->len == 1) {
        bigint_t *quot = bigint_alloc(bi_a->len);
        if (!quot)
            return NULL;

        uint64_t divisor = bi_b->digits[0];
        uint64_t rem = 0;

        for (int64_t i = bi_a->len - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | bi_a->digits[i];
            quot->digits[i] = (uint32_t)(cur / divisor);
            rem = cur % divisor;
        }
        quot->len = bi_a->len;
        quot->sign = (bi_a->sign != bi_b->sign) ? 1 : 0;
        bigint_normalize(quot);

        if (remainder) {
            bigint_t *r = (bigint_t *)rt_bigint_from_i64((int64_t)rem);
            if (!r) {
                bigint_release_owned(quot);
                return NULL;
            }
            r->sign = bi_a->sign;
            bigint_normalize(r);
            *remainder = r;
        }

        return quot;
    }

    // General case: Knuth Algorithm D (simplified)
    // Make copies to work with
    int64_t n = bi_b->len;
    int64_t m = bi_a->len - n;

    bigint_t *quot = bigint_alloc(m + 1);
    bigint_t *rem = bigint_clone(bi_a);
    if (!quot || !rem) {
        bigint_release_owned(quot);
        bigint_release_owned(rem);
        return NULL;
    }

    // Normalize
    int shift = 0;
    uint32_t high = bi_b->digits[n - 1];
    while ((high & 0x80000000) == 0) {
        high <<= 1;
        shift++;
    }

    // Left shift both numbers
    if (shift > 0) {
        if (!bigint_ensure_capacity(rem, rem->len + 1)) {
            bigint_release_owned(rem);
            bigint_release_owned(quot);
            return NULL;
        }
        uint32_t carry = 0;
        for (int64_t i = 0; i < rem->len; i++) {
            uint64_t val = ((uint64_t)rem->digits[i] << shift) | carry;
            rem->digits[i] = (uint32_t)(val & 0xFFFFFFFF);
            carry = (uint32_t)(val >> 32);
        }
        if (carry) {
            rem->digits[rem->len] = carry;
            rem->len++;
        }

        // Create shifted divisor
        bigint_t *d = bigint_clone(bi_b);
        if (!d) {
            bigint_release_owned(rem);
            bigint_release_owned(quot);
            return NULL;
        }
        carry = 0;
        for (int64_t i = 0; i < d->len; i++) {
            uint64_t val = ((uint64_t)d->digits[i] << shift) | carry;
            d->digits[i] = (uint32_t)(val & 0xFFFFFFFF);
            carry = (uint32_t)(val >> 32);
        }
        if (carry) {
            if (!bigint_ensure_capacity(d, d->len + 1)) {
                bigint_release_owned(d);
                bigint_release_owned(rem);
                bigint_release_owned(quot);
                return NULL;
            }
            d->digits[d->len] = carry;
            d->len++;
        }

        // Division loop
        for (int64_t j = m; j >= 0; j--) {
            // Estimate quotient digit
            uint64_t qhat;
            int64_t idx = j + n;
            uint64_t num = 0;
            if (idx < rem->len)
                num = rem->digits[idx];
            num = (num << 32);
            if (idx - 1 >= 0 && idx - 1 < rem->len)
                num |= rem->digits[idx - 1];

            qhat = num / d->digits[n - 1];
            if (qhat > 0xFFFFFFFF)
                qhat = 0xFFFFFFFF;

            // Multiply and subtract
            int64_t borrow = 0;
            for (int64_t i = 0; i < n; i++) {
                uint64_t prod = qhat * d->digits[i];
                int64_t diff = 0;
                if (j + i < rem->len)
                    diff = rem->digits[j + i];
                diff = diff - (prod & 0xFFFFFFFF) - borrow;
                if (diff < 0) {
                    diff += BIGINT_BASE;
                    borrow = (prod >> 32) + 1;
                } else {
                    borrow = prod >> 32;
                }
                if (j + i < rem->len)
                    rem->digits[j + i] = (uint32_t)diff;
            }

            if (j + n < rem->len) {
                int64_t diff = rem->digits[j + n] - borrow;
                if (diff < 0) {
                    // qhat was too big, add back
                    qhat--;
                    uint64_t addback_carry = 0;
                    for (int64_t i = 0; i < n; i++) {
                        uint64_t sum = (j + i < rem->len ? rem->digits[j + i] : 0) + d->digits[i] +
                                       addback_carry;
                        if (j + i < rem->len)
                            rem->digits[j + i] = (uint32_t)(sum & 0xFFFFFFFF);
                        addback_carry = sum >> 32;
                    }
                    rem->digits[j + n] = 0;
                } else {
                    rem->digits[j + n] = (uint32_t)diff;
                }
            }

            if (!bigint_ensure_capacity(quot, j + 1)) {
                bigint_release_owned(d);
                bigint_release_owned(rem);
                bigint_release_owned(quot);
                return NULL;
            }
            quot->digits[j] = (uint32_t)qhat;
            if (quot->len <= j)
                quot->len = j + 1;
        }

        // Right shift remainder
        if (shift > 0) {
            uint32_t rshift_carry = 0;
            for (int64_t i = rem->len - 1; i >= 0; i--) {
                uint64_t val = ((uint64_t)rshift_carry << 32) | rem->digits[i];
                rem->digits[i] = (uint32_t)(val >> shift);
                rshift_carry = (uint32_t)(val & ((1ULL << shift) - 1ULL));
            }
        }

        bigint_release_owned(d);
    } else {
        // No shift needed - simplified division
        for (int64_t j = m; j >= 0; j--) {
            uint64_t qhat;
            int64_t idx = j + n;
            uint64_t num = 0;
            if (idx < rem->len)
                num = rem->digits[idx];
            num = (num << 32);
            if (idx - 1 >= 0 && idx - 1 < rem->len)
                num |= rem->digits[idx - 1];

            qhat = num / bi_b->digits[n - 1];
            if (qhat > 0xFFFFFFFF)
                qhat = 0xFFFFFFFF;

            int64_t borrow = 0;
            for (int64_t i = 0; i < n; i++) {
                uint64_t prod = qhat * bi_b->digits[i];
                int64_t diff = 0;
                if (j + i < rem->len)
                    diff = rem->digits[j + i];
                diff = diff - (prod & 0xFFFFFFFF) - borrow;
                if (diff < 0) {
                    diff += BIGINT_BASE;
                    borrow = (prod >> 32) + 1;
                } else {
                    borrow = prod >> 32;
                }
                if (j + i < rem->len)
                    rem->digits[j + i] = (uint32_t)diff;
            }

            if (j + n < rem->len) {
                int64_t diff = rem->digits[j + n] - borrow;
                if (diff < 0) {
                    qhat--;
                    uint64_t carry = 0;
                    for (int64_t i = 0; i < n; i++) {
                        uint64_t sum =
                            (j + i < rem->len ? rem->digits[j + i] : 0) + bi_b->digits[i] + carry;
                        if (j + i < rem->len)
                            rem->digits[j + i] = (uint32_t)(sum & 0xFFFFFFFF);
                        carry = sum >> 32;
                    }
                    rem->digits[j + n] = 0;
                } else {
                    rem->digits[j + n] = (uint32_t)diff;
                }
            }

            if (!bigint_ensure_capacity(quot, j + 1)) {
                bigint_release_owned(rem);
                bigint_release_owned(quot);
                return NULL;
            }
            quot->digits[j] = (uint32_t)qhat;
            if (quot->len <= j)
                quot->len = j + 1;
        }
    }

    quot->sign = (bi_a->sign != bi_b->sign) ? 1 : 0;
    rem->sign = bi_a->sign;
    bigint_normalize(quot);
    bigint_normalize(rem);

    if (remainder)
        *remainder = rem;
    else
        bigint_release_owned(rem);

    return quot;
}

/// @brief Divides two BigInts and returns the quotient truncated toward zero.
/// @param a Dividend; null denotes zero.
/// @param b Nonzero divisor.
/// @return New quotient BigInt, or `NULL` after a division-by-zero trap.
void *rt_bigint_div(void *a, void *b) {
    return rt_bigint_divmod(a, b, NULL);
}

/// @brief Return the remainder of BigInt division (a % b).
/// @param a Dividend; null denotes zero.
/// @param b Nonzero divisor.
/// @return New remainder with the dividend's sign, or `NULL` after a trap.
void *rt_bigint_mod(void *a, void *b) {
    void *rem = NULL;
    void *quot = rt_bigint_divmod(a, b, &rem);
    bigint_release_owned((bigint_t *)quot);
    return rem;
}

/// @brief Negate a BigInt, returning a new result.
/// @param a Operand; null denotes zero.
/// @return New BigInt with the opposite sign, with zero remaining nonnegative.
void *rt_bigint_neg(void *a) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a)
        return rt_bigint_zero();

    bigint_t *result = bigint_clone((bigint_t *)a);
    if (!result)
        return NULL;

    if (result->len > 0)
        result->sign = result->sign ? 0 : 1;
    return result;
}

/// @brief Return the absolute value of a BigInt.
/// @param a Operand; null denotes zero.
/// @return New nonnegative BigInt with the same magnitude.
void *rt_bigint_abs(void *a) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a)
        return rt_bigint_zero();

    bigint_t *result = bigint_clone((bigint_t *)a);
    if (!result)
        return NULL;

    result->sign = 0;
    return result;
}

//=============================================================================
// Comparison
//=============================================================================

/// @brief Compare two BigInts: returns -1, 0, or 1.
/// @details Compares signs first (negative < non-negative), then magnitudes
///          digit-by-digit from most significant to least significant.
/// @param a First operand.
/// @param b Second operand.
/// @return -1 if a < b, 0 if equal, 1 if a > b.
int64_t rt_bigint_cmp(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return 0;
    if (!a && !b)
        return 0;
    if (!a)
        return rt_bigint_is_zero(b) ? 0 : (rt_bigint_is_negative(b) ? 1 : -1);
    if (!b)
        return rt_bigint_is_zero(a) ? 0 : (rt_bigint_is_negative(a) ? -1 : 1);

    bigint_t *bi_a = (bigint_t *)a;
    bigint_t *bi_b = (bigint_t *)b;

    // Check signs first
    if (bi_a->sign != bi_b->sign) {
        if (bi_a->len == 0 && bi_b->len == 0)
            return 0;
        return bi_a->sign ? -1 : 1;
    }

    // Same sign
    int mag_cmp = bigint_cmp_mag(bi_a, bi_b);
    return bi_a->sign ? -mag_cmp : mag_cmp;
}

/// @brief Return 1 if two BigInts are equal.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return One when the numeric values are equal, otherwise zero.
int8_t rt_bigint_eq(void *a, void *b) {
    return rt_bigint_cmp(a, b) == 0 ? 1 : 0;
}

/// @brief Check if the BigInt represents zero (len == 0).
/// @param a BigInt to test; NULL is treated as zero.
/// @return 1 if the value is zero, 0 otherwise.
int8_t rt_bigint_is_zero(void *a) {
    if (!bigint_check(a))
        return 1;
    if (!a)
        return 1;
    return ((bigint_t *)a)->len == 0 ? 1 : 0;
}

/// @brief Check if the BigInt is strictly negative (sign == 1 and len > 0).
/// @param a BigInt to test; NULL is treated as non-negative.
/// @return 1 if negative, 0 otherwise.
int8_t rt_bigint_is_negative(void *a) {
    if (!bigint_check(a))
        return 0;
    if (!a)
        return 0;
    bigint_t *bi = (bigint_t *)a;
    return (bi->len > 0 && bi->sign) ? 1 : 0;
}

/// @brief Return the sign of the BigInt: -1, 0, or 1.
/// @details Returns -1 for negative, 0 for zero, 1 for positive. Mirrors the
///          signum function from mathematics. NULL is treated as zero.
/// @param a BigInt to query.
/// @return -1, 0, or 1.
int64_t rt_bigint_sign(void *a) {
    if (!bigint_check(a))
        return 0;
    if (!a)
        return 0;
    bigint_t *bi = (bigint_t *)a;
    if (bi->len == 0)
        return 0;
    return bi->sign ? -1 : 1;
}

//=============================================================================
// Bitwise Operations
//=============================================================================

/// @brief Bitwise AND of two BigInts.
/// @details Applies arbitrary-precision two's-complement semantics. NULL
///          operands are treated as zero.
/// @param a First operand (not consumed).
/// @param b Second operand (not consumed).
/// @return New BigInt holding a AND b.
void *rt_bigint_and(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    return bigint_bitwise_twos((bigint_t *)a, (bigint_t *)b, '&');
}

/// @brief Computes bitwise OR using infinite-width two's-complement semantics.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt holding `a OR b`.
void *rt_bigint_or(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    return bigint_bitwise_twos((bigint_t *)a, (bigint_t *)b, '|');
}

/// @brief Computes bitwise XOR using infinite-width two's-complement semantics.
/// @param a First operand; null denotes zero.
/// @param b Second operand; null denotes zero.
/// @return New BigInt holding `a XOR b`.
void *rt_bigint_xor(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    return bigint_bitwise_twos((bigint_t *)a, (bigint_t *)b, '^');
}

/// @brief Computes infinite-width two's-complement bitwise NOT.
/// @param a Operand; null denotes zero.
/// @return New BigInt equal to `-(a + 1)`.
void *rt_bigint_not(void *a) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    // For arbitrary precision, NOT doesn't have fixed meaning
    // Return -(a + 1) as two's complement would
    void *one = rt_bigint_one();
    if (!one)
        return NULL;
    void *sum = rt_bigint_add(a, one);
    if (!sum) {
        bigint_release_owned((bigint_t *)one);
        return NULL;
    }
    void *result = rt_bigint_neg(sum);
    bigint_release_owned((bigint_t *)one);
    bigint_release_owned((bigint_t *)sum);

    return result;
}

/// @brief Left-shifts a BigInt by @p n bits.
/// @param a Value to shift; null denotes zero.
/// @param n Shift distance; nonpositive values return an unchanged clone.
/// @return New BigInt equal to `a * 2^n` for positive @p n.
void *rt_bigint_shl(void *a, int64_t n) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a || n <= 0)
        return a ? bigint_clone((bigint_t *)a) : rt_bigint_zero();

    bigint_t *bi = (bigint_t *)a;
    if (bi->len == 0)
        return rt_bigint_zero();

    int64_t word_shift = n / 32;
    int bit_shift = (int)(n % 32);

    int64_t shifted_len;
    int64_t new_len;
    if (!bigint_checked_add_i64(bi->len, word_shift, &shifted_len) ||
        !bigint_checked_add_i64(shifted_len, 1, &new_len)) {
        rt_trap("bigint: shift size overflow");
        return NULL;
    }
    bigint_t *result = bigint_alloc(new_len);
    if (!result)
        return NULL;

    result->len = new_len;
    result->sign = bi->sign;

    // Word shift
    for (int64_t i = 0; i < word_shift; i++)
        result->digits[i] = 0;

    // Bit shift
    uint32_t carry = 0;
    for (int64_t i = 0; i < bi->len; i++) {
        uint64_t val = ((uint64_t)bi->digits[i] << bit_shift) | carry;
        result->digits[i + word_shift] = (uint32_t)(val & 0xFFFFFFFF);
        carry = (uint32_t)(val >> 32);
    }
    result->digits[bi->len + word_shift] = carry;

    bigint_normalize(result);
    return result;
}

/// @brief Arithmetic-right-shifts a BigInt by @p n bits.
/// @param a Value to shift; null denotes zero.
/// @param n Shift distance; nonpositive values return an unchanged clone.
/// @return New floor-division result `floor(a / 2^n)` for positive @p n.
/// @note Negative values round toward negative infinity by detecting discarded
///       one bits and incrementing the shifted magnitude.
void *rt_bigint_shr(void *a, int64_t n) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a || n <= 0)
        return a ? bigint_clone((bigint_t *)a) : rt_bigint_zero();

    bigint_t *bi = (bigint_t *)a;
    if (bi->len == 0)
        return rt_bigint_zero();

    bigint_t *result = bigint_shr_magnitude(bi, n);
    if (!result)
        return NULL;
    if (bi->sign) {
        if (bigint_magnitude_has_low_bits(bi, n) && !bigint_add_one_mag_inplace(result)) {
            bigint_release_owned(result);
            return NULL;
        }
        if (result->len > 0)
            result->sign = 1;
    }
    bigint_normalize(result);
    return result;
}

//=============================================================================
// Advanced Operations
//=============================================================================

/// @brief Compute a^n using binary exponentiation (squaring method).
/// @details Runs in O(log n) multiplications by repeatedly squaring the base
///          and multiplying into the accumulator when the current exponent bit
///          is set. This is dramatically faster than n successive multiplications
///          for large exponents. Negative exponents trap because BigInt has no
///          fractional representation — callers must check before calling.
/// @param a Base value (not consumed).
/// @param n Exponent (must be non-negative).
/// @return New BigInt holding a^n. Returns 1 for n=0.
void *rt_bigint_pow(void *a, int64_t n) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (n < 0) {
        rt_trap("BigInt.Pow: negative exponent");
        return NULL;
    }

    if (n == 0)
        return rt_bigint_one();
    if (!a)
        return rt_bigint_zero();

    bigint_t *bi = (bigint_t *)a;
    if (bi->len == 0)
        return rt_bigint_zero();

    // Binary exponentiation
    void *result = rt_bigint_one();
    void *base = bigint_clone(bi);
    if (!result || !base) {
        bigint_release_owned((bigint_t *)result);
        bigint_release_owned((bigint_t *)base);
        return NULL;
    }

    while (n > 0) {
        if (n & 1) {
            void *tmp = rt_bigint_mul(result, base);
            bigint_release_owned((bigint_t *)result);
            if (!tmp) {
                bigint_release_owned((bigint_t *)base);
                return NULL;
            }
            result = tmp;
        }
        n >>= 1;
        if (n > 0) {
            void *tmp = rt_bigint_mul(base, base);
            bigint_release_owned((bigint_t *)base);
            if (!tmp) {
                bigint_release_owned((bigint_t *)result);
                return NULL;
            }
            base = tmp;
        }
    }

    bigint_release_owned((bigint_t *)base);
    return result;
}

/// @brief Modular exponentiation: compute a^n mod m.
/// @details Uses binary exponentiation with intermediate modular reduction after
///          each multiply, keeping intermediate values bounded by m^2 rather
///          than growing exponentially. The per-bit loop follows a fixed
///          three-multiplication schedule (cross product and both squares), but
///          the underlying arbitrary-precision operations are not documented as
///          constant-time. Traps on zero modulus or a negative exponent.
/// @param a Base (not consumed).
/// @param n Exponent (not consumed).
/// @param m Modulus (not consumed; must be non-zero).
/// @return New BigInt holding a^n mod m as the least non-negative residue in [0, |m|).
void *rt_bigint_pow_mod(void *a, void *n, void *m) {
    if (!bigint_check(a) || !bigint_check(n) || !bigint_check(m))
        return NULL;
    if (!m || rt_bigint_is_zero(m)) {
        rt_trap("BigInt.PowMod: zero modulus");
        return NULL;
    }

    if (rt_bigint_is_negative(n)) {
        rt_trap("BigInt.PowMod: negative exponent");
        return NULL;
    }

    if (!n || rt_bigint_is_zero(n)) {
        void *one = rt_bigint_one();
        if (!one)
            return NULL;
        void *result = rt_bigint_mod(one, m);
        bigint_release_owned((bigint_t *)one);
        return result;
    }
    if (!a || rt_bigint_is_zero(a))
        return rt_bigint_zero();

    /* S-23: ladder-style fixed arithmetic schedule — executes one cross product
     * and both candidate squares per exponent bit (MSB to LSB). This avoids
     * skipping a multiply based solely on the exponent bit, but the general
     * BigInt implementation does not promise whole-operation constant time.
     * if bit==1: r0 = r0*r1 mod m;  r1 = r1^2 mod m
     * if bit==0: r1 = r0*r1 mod m;  r0 = r0^2 mod m */
    int64_t nbits = rt_bigint_bit_length(n);

    void *r0 = rt_bigint_one();
    void *r1 = rt_bigint_mod(a, m);
    if (!r0 || !r1) {
        bigint_release_owned((bigint_t *)r0);
        bigint_release_owned((bigint_t *)r1);
        return NULL;
    }

    for (int64_t i = nbits - 1; i >= 0; i--) {
        int8_t bit = rt_bigint_test_bit(n, i);

        void *cross = rt_bigint_mul(r0, r1);
        if (!cross) {
            bigint_release_owned((bigint_t *)r0);
            bigint_release_owned((bigint_t *)r1);
            return NULL;
        }
        void *cross_m = rt_bigint_mod(cross, m);
        bigint_release_owned((bigint_t *)cross);

        void *sq0 = rt_bigint_mul(r0, r0);
        if (!sq0) {
            bigint_release_owned((bigint_t *)cross_m);
            bigint_release_owned((bigint_t *)r0);
            bigint_release_owned((bigint_t *)r1);
            return NULL;
        }
        void *sq0_m = rt_bigint_mod(sq0, m);
        bigint_release_owned((bigint_t *)sq0);

        void *sq1 = rt_bigint_mul(r1, r1);
        if (!sq1) {
            bigint_release_owned((bigint_t *)cross_m);
            bigint_release_owned((bigint_t *)sq0_m);
            bigint_release_owned((bigint_t *)r0);
            bigint_release_owned((bigint_t *)r1);
            return NULL;
        }
        void *sq1_m = rt_bigint_mod(sq1, m);
        bigint_release_owned((bigint_t *)sq1);

        if (!cross_m || !sq0_m || !sq1_m) {
            bigint_release_owned((bigint_t *)cross_m);
            bigint_release_owned((bigint_t *)sq0_m);
            bigint_release_owned((bigint_t *)sq1_m);
            bigint_release_owned((bigint_t *)r0);
            bigint_release_owned((bigint_t *)r1);
            return NULL;
        }

        bigint_release_owned((bigint_t *)r0);
        bigint_release_owned((bigint_t *)r1);

        if (bit) {
            r0 = cross_m;
            r1 = sq1_m;
            bigint_release_owned((bigint_t *)sq0_m);
        } else {
            r1 = cross_m;
            r0 = sq0_m;
            bigint_release_owned((bigint_t *)sq1_m);
        }
    }

    bigint_release_owned((bigint_t *)r1);

    // Normalize to the least non-negative residue in [0, |m|). `Mod` uses
    // truncating division, so a negative base propagates a negative residue
    // through the ladder; PowMod returns the conventional non-negative
    // representative of the congruence class (e.g. PowMod(-2,3,5) == 2, not -3)
    // (VDOC-205). The raw residue is in (-|m|, 0) when negative, so adding |m|
    // lands it in (0, |m|).
    if (r0 && rt_bigint_is_negative(r0)) {
        void *absm = rt_bigint_abs(m);
        void *norm = absm ? bigint_add_impl(r0, absm) : NULL;
        bigint_release_owned((bigint_t *)absm);
        bigint_release_owned((bigint_t *)r0);
        r0 = norm;
    }

    return r0;
}

/// @brief Compute the greatest common divisor using Euclidean algorithm.
/// @details Repeatedly replaces `(x, y)` with `(y, x mod y)` until the
///          remainder is zero. Operates on
///          absolute values so negative inputs are handled correctly. Returns
///          zero when both inputs are zero (mathematical convention).
/// @param a First value (not consumed).
/// @param b Second value (not consumed).
/// @return New BigInt holding gcd(|a|, |b|).
void *rt_bigint_gcd(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    if (!a)
        return b ? rt_bigint_abs(b) : rt_bigint_zero();
    if (!b)
        return rt_bigint_abs(a);

    void *x = rt_bigint_abs(a);
    void *y = rt_bigint_abs(b);
    if (!x || !y) {
        bigint_release_owned((bigint_t *)x);
        bigint_release_owned((bigint_t *)y);
        return NULL;
    }

    // Euclidean algorithm using division remainders.
    while (!rt_bigint_is_zero(y)) {
        void *rem = rt_bigint_mod(x, y);
        bigint_release_owned((bigint_t *)x);
        if (!rem) {
            bigint_release_owned((bigint_t *)y);
            return NULL;
        }
        x = y;
        y = rem;
    }

    bigint_release_owned((bigint_t *)y);
    return x;
}

/// @brief Compute the least common multiple: lcm(a,b) = |a*b| / gcd(a,b).
/// @details Uses the standard identity lcm(a,b) = |a*b| / gcd(a,b) rather than
///          enumerating multiples, which would be impractical for large values.
///          The absolute value ensures the result is always non-negative.
///          Returns zero when either input is zero (by convention).
/// @param a First value (not consumed).
/// @param b Second value (not consumed).
/// @return New BigInt holding lcm(|a|, |b|).
void *rt_bigint_lcm(void *a, void *b) {
    if (!bigint_check(a) || !bigint_check(b))
        return rt_bigint_zero();
    if (!a || !b)
        return rt_bigint_zero();

    void *gcd = rt_bigint_gcd(a, b);
    if (!gcd)
        return NULL;
    if (rt_bigint_is_zero(gcd)) {
        bigint_release_owned((bigint_t *)gcd);
        return rt_bigint_zero();
    }

    void *prod = rt_bigint_mul(a, b);
    if (!prod) {
        bigint_release_owned((bigint_t *)gcd);
        return NULL;
    }
    void *abs_prod = rt_bigint_abs(prod);
    bigint_release_owned((bigint_t *)prod);
    if (!abs_prod) {
        bigint_release_owned((bigint_t *)gcd);
        return NULL;
    }

    void *result = rt_bigint_div(abs_prod, gcd);
    bigint_release_owned((bigint_t *)abs_prod);
    bigint_release_owned((bigint_t *)gcd);

    return result;
}

/// @brief Return the number of bits needed to represent the magnitude.
/// @details Computes floor(log2(|value|)) + 1 by finding the highest set bit
///          in the most significant digit. Returns 0 for a zero value.
/// @param a BigInt to measure.
/// @return Bit count (excludes the sign).
int64_t rt_bigint_bit_length(void *a) {
    if (!bigint_check(a))
        return 0;
    if (!a)
        return 0;

    bigint_t *bi = (bigint_t *)a;
    if (bi->len == 0)
        return 0;

    int64_t base_limbs = bi->len - 1;
    int64_t bits;
    if (!bigint_checked_mul_i64(base_limbs, 32, &bits)) {
        rt_trap("bigint: bit length overflow");
        return INT64_MAX;
    }
    uint32_t high = bi->digits[bi->len - 1];

    while (high > 0) {
        bits++;
        high >>= 1;
    }

    return bits;
}

/// @details Uses infinite-width two's-complement semantics: positions beyond a
///          positive magnitude return 0, while positions beyond a negative
///          magnitude return its sign-extension bit (1).
/// @param a BigInt to test.
/// @param n Zero-based bit index.
/// @brief Test bit n (0 = LSB). Returns 1 if set, 0 if clear.
/// @param a Value to inspect; null denotes zero.
/// @param n Nonnegative zero-based bit index.
/// @return One if the infinite-width two's-complement bit is set; zero for a
///         clear bit or a negative index.
int8_t rt_bigint_test_bit(void *a, int64_t n) {
    if (!bigint_check(a))
        return 0;
    if (!a || n < 0)
        return 0;

    bigint_t *bi = (bigint_t *)a;
    int64_t word = n / 32;
    int bit = (int)(n % 32);

    if (word >= bi->len)
        return bi->sign ? 1 : 0;

    if (bi->sign) {
        int64_t width;
        if (!bigint_checked_add_i64(bi->len, 1, &width) || !bigint_capacity_is_allocable(width)) {
            rt_trap("bigint: bit test size overflow");
            return 0;
        }
        uint32_t *words = (uint32_t *)calloc((size_t)width, sizeof(uint32_t));
        if (!words) {
            rt_trap("bigint: memory allocation failed");
            return 0;
        }
        if (!bigint_to_twos_words(bi, words, width)) {
            free(words);
            rt_trap("bigint: bit test conversion failed");
            return 0;
        }
        int8_t result = (words[word] >> bit) & 1 ? 1 : 0;
        free(words);
        return result;
    }

    return (bi->digits[word] >> bit) & 1 ? 1 : 0;
}

/// @brief Return a new BigInt with bit n set to 1.
/// @param a Value to modify conceptually; null denotes zero.
/// @param n Zero-based bit index; a negative index returns an unchanged clone.
/// @return New value with bit @p n set under infinite-width two's-complement semantics.
void *rt_bigint_set_bit(void *a, int64_t n) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (n < 0)
        return a ? bigint_clone((bigint_t *)a) : rt_bigint_zero();

    bigint_t *mask = bigint_single_bit_mask(n);
    if (!mask)
        return NULL;
    void *result = rt_bigint_or(a, mask);
    bigint_release_owned(mask);
    return result;
}

/// @brief Return a new BigInt with bit n cleared to 0.
/// @param a Value to modify conceptually; null denotes zero.
/// @param n Zero-based bit index; a negative index returns an unchanged clone.
/// @return New value with bit @p n cleared under infinite-width two's-complement semantics.
void *rt_bigint_clear_bit(void *a, int64_t n) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a || n < 0)
        return a ? bigint_clone((bigint_t *)a) : rt_bigint_zero();

    bigint_t *mask = bigint_single_bit_mask(n);
    if (!mask)
        return NULL;
    void *not_mask = rt_bigint_not(mask);
    if (!not_mask) {
        bigint_release_owned(mask);
        return NULL;
    }
    void *result = rt_bigint_and(a, not_mask);
    bigint_release_owned(mask);
    bigint_release_owned((bigint_t *)not_mask);
    return result;
}

/// @brief Integer square root using Newton's method (Heron's algorithm).
/// @details Computes floor(sqrt(a)) by iterating x = (x + a/x) / 2 until the
///          estimate stabilizes (consecutive iterations differ by at most 1).
///          The initial guess uses half the bit-length to start close to the
///          answer, typically converging in O(log(log(a))) iterations. Traps
///          on negative input because square root of a negative integer is
///          undefined in the integer domain.
/// @param a Non-negative BigInt (not consumed).
/// @return New BigInt holding floor(sqrt(a)).
void *rt_bigint_sqrt(void *a) {
    if (!bigint_check(a))
        return rt_bigint_zero();
    if (!a)
        return rt_bigint_zero();

    bigint_t *bi = (bigint_t *)a;
    if (bi->sign) {
        rt_trap("BigInt.Sqrt: negative input");
        return NULL;
    }

    if (bi->len == 0)
        return rt_bigint_zero();

    // Newton's method
    int64_t bits = rt_bigint_bit_length(a);
    void *one = rt_bigint_one();
    void *x = rt_bigint_shl(one, (bits + 1) / 2);
    bigint_release_owned((bigint_t *)one);
    if (!x)
        return NULL;

    while (1) {
        void *q = rt_bigint_div(a, x);
        if (!q) {
            bigint_release_owned((bigint_t *)x);
            return NULL;
        }
        void *sum = rt_bigint_add(x, q);
        bigint_release_owned((bigint_t *)q);
        if (!sum) {
            bigint_release_owned((bigint_t *)x);
            return NULL;
        }
        void *next = rt_bigint_shr(sum, 1);
        bigint_release_owned((bigint_t *)sum);
        if (!next) {
            bigint_release_owned((bigint_t *)x);
            return NULL;
        }

        if (rt_bigint_cmp(next, x) >= 0) {
            bigint_release_owned((bigint_t *)next);
            break;
        }

        bigint_release_owned((bigint_t *)x);
        x = next;
    }

    return x;
}
