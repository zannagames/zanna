//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_string.h
// Purpose: Declares the primary reference-counted runtime byte-string API,
// including ownership, slicing, UTF-8-aware operations, search, transformation,
// numeric conversion, distance metrics, and LIKE matching.
//
// Key invariants:
//   - Every live string has a stored byte length and trailing NUL, but payloads
//     may contain embedded NULs or arbitrary non-UTF-8 bytes.
//   - Handles may use embedded small-string storage, a separate heap payload, or
//     immortal shared storage. Retain/release counters are atomic.
//   - A process-global synchronized registry validates opaque string handles.
//   - NULL is empty for selected queries and extended APIs, while strict BASIC
//     intrinsics trap; each declaration documents its own rule.
//
// Encoding & indexing:
//   - rt_str_len() returns a byte count clamped to INT64_MAX.
//   - Mid$, MidLen$: operate on CODEPOINT offsets (1-based). These are the
//     primary user-facing slicing functions and reject malformed UTF-8.
//   - rt_str_substr(), Left$, Right$: operate on byte offsets. These are internal
//     or BASIC-compatible and may split UTF-8 sequences.
//   - rt_str_flip() and LIKE wildcard stepping validate strict UTF-8 sequences.
//   - Case conversion (UCase$/LCase$) is ASCII-only: bytes 0x41-0x5A / 0x61-0x7A
//     are toggled; multi-byte UTF-8 sequences pass through unchanged.
//
// Ownership/Lifetime:
//   - String-returning functions transfer one owned reference, which may be a
//     new allocation, a retained input, or an immortal singleton.
//   - rt_string_ref increments refcount; rt_string_unref decrements and frees at zero.
//   - Inputs are borrowed unless a declaration explicitly says they are consumed.
//   - Borrowed C-string views remain valid only while their handle remains alive.
//
// Links: src/runtime/core/rt_string.c (implementation map),
//        src/runtime/core/rt_string_ops.c (core and ownership operations),
//        src/runtime/core/rt_string_advanced.c (extended operations),
//        src/runtime/core/rt_string_specialized.c (specialized transforms),
//        src/runtime/core/rt_string_encode.c (byte/C-string bridging),
//        src/runtime/core/rt_string_format.c (numeric conversion)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Public runtime byte-string ownership and manipulation API.
#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief Stable heap class ID used for runtime string objects.
#define RT_STRING_CLASS_ID INT64_C(-0x530701)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque implementation record for one runtime string handle.
struct rt_string_impl;

/// @brief Exposes the runtime string object layout for host interop wrappers.
typedef struct rt_string_impl ZannaString;

/// @brief Nullable pointer to a registered runtime string implementation.
typedef struct rt_string_impl *rt_string;

/// @brief Increment the reference count of @p s when non-null.
/// @details Mortal embedded and heap-backed counts are updated atomically.
///          Immortal handles are unchanged. Invalid, released, or overflowing
///          handles trap and return `NULL` if the trap hook returns.
/// @param s String to retain; may be NULL.
/// @return The same pointer passed via @p s.
rt_string rt_string_ref(rt_string s);

/// @brief Decrement the reference count of @p s and free when zero.
/// @details Final release clears weak references, unregisters the handle, and
///          frees its embedded or heap-backed storage. Immortal and null handles
///          are no-ops.
/// @param s String to release; may be NULL.
void rt_string_unref(rt_string s);

/// @brief Decrement the reference count of @p s and return the post-release count.
/// @details Performs the same final-release cleanup as @ref rt_string_unref.
///          Immortal strings return `SIZE_MAX`; null returns zero.
/// @param s String to release; may be NULL.
/// @return Reference count immediately after release, zero after final release,
///         or `SIZE_MAX` for immortal storage.
size_t rt_string_unref_count(rt_string s);

/// @brief Release @p s when non-null.
/// @details Exact void-returning alias of @ref rt_string_unref.
/// @param s String handle that may be NULL.
void rt_str_release_maybe(rt_string s);

/// @brief Retain @p s when non-null.
/// @details Discards the chaining result from @ref rt_string_ref.
/// @param s String handle that may be NULL.
void rt_str_retain_maybe(rt_string s);

/// @brief Return non-zero when @p p points at a runtime string handle.
/// @details Used by generic object helpers to distinguish string handles from
///          other pointers. Performs a synchronized live-registry lookup and
///          checks string magic while holding the registry lock.
/// @param p Pointer to test; may be NULL.
/// @return One only for a currently registered handle with valid magic.
int8_t rt_string_is_handle(const void *p);

/// @brief Return the lazily initialized immortal empty-string singleton.
/// @return Shared non-null zero-length handle, or `NULL` after an allocation
///         trap if its hook returns during first initialization.
rt_string rt_str_empty(void);

/// @brief Allocate a runtime string by copying @p len bytes from @p bytes.
/// @details Copies arbitrary bytes, including embedded NUL and malformed UTF-8,
///          and appends a separate terminator. Small results may use inline
///          storage. A null source with nonzero length traps.
/// @param bytes Source buffer; may be NULL only when @p len is zero.
/// @param len Number of bytes to copy into the new runtime string.
/// @return Owned runtime string containing the copied bytes, or `NULL` after
///         overflow/allocation failure.
rt_string rt_string_from_bytes(const char *bytes, size_t len);

/// @brief Create a runtime string from a string literal.
/// @details Copies through @ref rt_string_from_bytes; the result is an ordinary
///          mortal owned string rather than a borrowed or immortal literal view.
/// @param bytes Pointer to the literal data.
/// @param len Number of bytes in the literal.
/// @return Owned copied runtime string.
rt_string rt_str_from_lit(const char *bytes, size_t len);

/// @brief Return the number of bytes stored in @p s (excluding the terminator).
/// @param s String to measure; `NULL` is treated as empty.
/// @return Stored byte length clamped to `INT64_MAX`.
int64_t rt_str_len(rt_string s);

/// @brief Test whether @p s has zero stored bytes.
/// @param s String to inspect; `NULL` is treated as empty.
/// @return One for null or zero-length strings; otherwise zero.
int64_t rt_str_is_empty(rt_string s);

/// @brief Concatenate two owned operands and normally consume both claims.
/// @details May reuse a uniquely owned heap-backed @p a with spare capacity.
///          Equal handles require two ownership claims and are copied safely.
///          Allocation failure after length validation releases both inputs;
///          a length-overflow trap occurs before either is consumed.
/// @param a Owned left operand; null contributes zero bytes.
/// @param b Owned right operand; null contributes zero bytes.
/// @return Owned concatenation, possibly the reused @p a, or `NULL` on failure.
rt_string rt_str_concat(rt_string a, rt_string b);

/// @brief Slice substring of @p s starting at @p start with length @p len.
/// @details Uses byte indexing. Negative values become zero and the slice is
///          clamped to the source. Full slices retain @p s; empty slices return
///          the immortal singleton. A null source traps and falls back to empty.
/// @param s Borrowed source string.
/// @param start Zero-based byte offset.
/// @param len Requested byte count.
/// @return Owned slice handle.
rt_string rt_str_substr(rt_string s, int64_t start, int64_t len);

/// @brief Retaining constructor for strings; used by Zanna.String.FromStr.
/// @param s Borrowed string, or `NULL`.
/// @return @p s with one additional owned reference, or `NULL`.
rt_string rt_str_clone(rt_string s);

/// @brief Return leftmost @p n bytes of @p s.
/// @details A negative count traps; zero returns the empty singleton; a count
///          at least the byte length retains the original.
/// @param s Borrowed source; null traps.
/// @param n Byte count.
/// @return Owned prefix, retained source, or empty singleton.
rt_string rt_str_left(rt_string s, int64_t n);

/// @brief Return rightmost @p n bytes of @p s.
/// @details Uses the same validation and ownership rules as
///          @ref rt_str_left, with byte slicing measured from the end.
/// @param s Borrowed source; null traps.
/// @param n Byte count.
/// @return Owned suffix, retained source, or empty singleton.
rt_string rt_str_right(rt_string s, int64_t n);

/// @brief Return substring starting at @p start to the end of @p s.
/// @details Strictly decodes UTF-8 while locating the codepoint. A start below
///          one or malformed sequence traps. Start one retains the source;
///          a position beyond the codepoint count returns empty.
/// @param s Borrowed source; null traps.
/// @param start One-based UTF-8 codepoint position.
/// @return Owned trailing slice.
rt_string rt_str_mid(rt_string s, int64_t start);

/// @brief Return substring starting at @p start with length @p len.
/// @details Strictly decodes UTF-8, traps for start below one or negative
///          length, and clamps the ending position without signed overflow.
///          A full slice retains the source and a zero/out-of-range slice is
///          empty.
/// @param s Borrowed source; null traps.
/// @param start One-based UTF-8 codepoint position.
/// @param len Number of codepoints requested.
/// @return Owned substring, retained source, or empty singleton.
rt_string rt_str_mid_len(rt_string s, int64_t start, int64_t len);

/// @brief Find @p needle within @p hay starting at 1-based index @p start.
/// @details Search and result indices are byte-based. Starts at or below one
///          become one and positions beyond the end clamp to `Length+1`.
///          An empty needle matches at the clamped position. Null operands miss.
/// @param start One-based starting byte position.
/// @param hay Borrowed haystack.
/// @param needle Borrowed needle.
/// @return One-based byte position or zero when absent.
int64_t rt_str_instr3(int64_t start, rt_string hay, rt_string needle);

/// @brief Zanna.String.IndexOfFrom wrapper with receiver-first argument order.
/// @details Exact argument-order wrapper around @ref rt_str_instr3.
/// @param hay Borrowed haystack.
/// @param start One-based starting byte position.
/// @param needle Borrowed needle.
/// @return One-based byte position or zero when absent.
int64_t rt_str_index_of_from(rt_string hay, int64_t start, rt_string needle);

/// @brief Find @p needle within @p hay starting at index 1.
/// @details Uses byte comparison. Empty needles return one; null operands miss.
/// @param hay Borrowed haystack.
/// @param needle Borrowed needle.
/// @return One-based byte position or zero when absent.
int64_t rt_str_index_of(rt_string hay, rt_string needle);

/// @brief Remove leading ASCII whitespace.
/// @details Trims space, tab, CR, LF, vertical tab, and form feed.
/// @param s Borrowed source; null traps.
/// @return Owned byte slice.
rt_string rt_str_ltrim(rt_string s);

/// @brief Remove trailing ASCII whitespace.
/// @details Uses the same six-byte whitespace set as @ref rt_str_ltrim.
/// @param s Borrowed source; null traps.
/// @return Owned byte slice.
rt_string rt_str_rtrim(rt_string s);

/// @brief Remove leading and trailing ASCII whitespace.
/// @param s Borrowed source; null traps.
/// @return Owned byte slice using the same whitespace set as
///         @ref rt_str_ltrim.
rt_string rt_str_trim(rt_string s);

/// @brief Convert ASCII characters to uppercase.
/// @details Allocates a byte-for-byte copy, mapping only ASCII `a-z`; all
///          high-bit and other bytes are unchanged.
/// @param s Borrowed source; null traps.
/// @return Newly allocated owned string.
rt_string rt_str_ucase(rt_string s);

/// @brief Convert ASCII characters to lowercase.
/// @details Allocates a byte-for-byte copy, mapping only ASCII `A-Z`; all
///          high-bit and other bytes are unchanged.
/// @param s Borrowed source; null traps.
/// @return Newly allocated owned string.
rt_string rt_str_lcase(rt_string s);

/// @brief Create a one-byte string from @p code.
/// @details Values outside 0–255 trap and fall back to the empty singleton.
///          Code zero creates a length-one string containing an embedded NUL.
/// @param code Unsigned byte value represented as int64.
/// @return Owned one-byte string or empty fallback.
rt_string rt_str_chr(int64_t code);

/// @brief Return the unsigned value of the first stored byte.
/// @param s Borrowed valid source; null/invalid handles trap.
/// @return Value 0–255, or zero for empty/null-data and returning-trap cases.
int64_t rt_str_asc(rt_string s);

/// @brief Compare @p a and @p b for equality.
/// @details Compares stored lengths and bytes. Any null operand returns zero,
///          including two null operands.
/// @param a Borrowed first operand.
/// @param b Borrowed second operand.
/// @return One for equal non-null byte strings; otherwise zero.
int8_t rt_str_eq(rt_string a, rt_string b);

/// @brief Compare @p a < @p b lexicographically.
/// @details Uses unsigned-byte lexicographic ordering. Any null operand returns
///          false.
/// @param a Borrowed first operand.
/// @param b Borrowed second operand.
/// @return One when both are non-null and @p a sorts before @p b.
int64_t rt_str_lt(rt_string a, rt_string b);

/// @brief Compare @p a <= @p b lexicographically.
/// @details Uses unsigned-byte lexicographic ordering. Null operands compare
///          true only when both pointers are null.
/// @param a Borrowed first operand.
/// @param b Borrowed second operand.
/// @return Boolean comparison result.
int64_t rt_str_le(rt_string a, rt_string b);

/// @brief Compare @p a > @p b lexicographically.
/// @details Uses unsigned-byte lexicographic ordering. Any null operand returns
///          false.
/// @param a Borrowed first operand.
/// @param b Borrowed second operand.
/// @return Boolean comparison result.
int64_t rt_str_gt(rt_string a, rt_string b);

/// @brief Compare @p a >= @p b lexicographically.
/// @details Uses unsigned-byte lexicographic ordering. Null operands compare
///          true only when both pointers are null.
/// @param a Borrowed first operand.
/// @param b Borrowed second operand.
/// @return Boolean comparison result.
int64_t rt_str_ge(rt_string a, rt_string b);

/// @brief Strictly parse @p s as a signed decimal 64-bit integer.
/// @details Trims ASCII whitespace, rejects empty input and embedded NULs,
///          copies to scratch storage, and requires `strtoll` base-10 parsing
///          to consume everything. Invalid input, allocation failure, and
///          overflow trap and return zero only if the hook returns.
/// @param s Borrowed valid runtime string.
/// @return Parsed integer, or zero after a returning trap.
int64_t rt_to_int(rt_string s);

/// @brief Strictly parse @p s as a floating-point value.
/// @details Rejects embedded NUL, then uses the C-locale strict parser accepting
///          trimmed decimal syntax and signed `nan`/`inf` tokens. Invalid input
///          and overflow trap with INPUT diagnostics.
/// @param s Borrowed valid runtime string.
/// @return Parsed value, or `0.0` after a returning trap.
double rt_to_double(rt_string s);

/// @brief Convert integer @p v to decimal string.
/// @details Uses the checked string builder and traps on formatting,
///          allocation, argument, or size failure before returning an owned
///          empty fallback if the hook returns.
/// @param v Value to format.
/// @return Owned decimal representation.
rt_string rt_int_to_str(int64_t v);

/// @brief Convert @p v using locale-stable double round-trip formatting.
/// @param v Value to format.
/// @return Newly allocated owned representation, including supported
///         non-finite spellings.
rt_string rt_f64_to_str(double v);

/// @brief Legacy alias for double round-trip string allocation.
/// @param v Double-precision value passed through round-trip formatting.
/// @return Newly allocated owned runtime string.
rt_string rt_str_d_alloc(double v);

/// @brief Format @p v after narrowing it to single precision.
/// @details The C ABI accepts double, casts to float, promotes back to double,
///          and applies the runtime's ordinary finite/non-finite float format.
/// @param v Value whose Float32 representation is formatted.
/// @return Newly allocated owned runtime string.
rt_string rt_str_f_alloc(double v);

/// @brief Allocate minimal signed-decimal text for a 32-bit integer.
/// @param v 32-bit integer value to convert.
/// @return Newly allocated owned runtime string.
rt_string rt_str_i32_alloc(int32_t v);

/// @brief Allocate minimal signed-decimal text for a 16-bit integer.
/// @param v 16-bit integer value to convert.
/// @return Newly allocated owned runtime string.
rt_string rt_str_i16_alloc(int16_t v);

/// @brief Parse leading numeric prefix from @p s as double.
/// @details Skips leading ASCII whitespace and calls process-locale `strtod`,
///          ignoring trailing bytes after the longest accepted prefix. Embedded
///          NUL therefore terminates parsing. Null traps; no prefix returns zero;
///          ERANGE that produces infinity traps but returns that value if the
///          hook returns.
/// @param s Borrowed runtime string.
/// @return Parsed prefix value or zero when no prefix exists.
double rt_val(rt_string s);

/// @brief Historic STR$ wrapper for double round-trip formatting.
/// @param v Value to format.
/// @return Result of @ref rt_f64_to_str.
rt_string rt_str(double v);

/// @brief Obtain a null-terminated C string view of @p s.
/// @details Validates the live handle and data pointer. Embedded NUL bytes
///          remain present and will shorten ordinary C-string consumers.
/// @param s Borrowed non-null runtime string.
/// @return Borrowed internal byte buffer, or the static empty C string after a
///         returning validation trap.
const char *rt_string_cstr(rt_string s);

/// @brief Copy a null-terminated C string into a runtime string.
/// @details Copies through the first NUL. Empty input returns the immortal
///          empty singleton; null input returns `NULL` without trapping.
/// @param str Borrowed NUL-terminated source pointer.
/// @return Owned copied handle, immortal empty singleton, or `NULL`.
rt_string rt_const_cstr(const char *str);

//===----------------------------------------------------------------------===//
// Extended String Functions (Zanna.String expansion)
//===----------------------------------------------------------------------===//

/// @brief Replace all occurrences of needle with replacement.
/// @details Matches non-overlapping byte spans. Null haystack returns the empty
///          singleton; null/empty needle, null replacement, or no match retains
///          the haystack. Builder failure traps and returns `NULL`.
/// @param haystack Borrowed source string.
/// @param needle Borrowed byte sequence to find.
/// @param replacement Borrowed replacement bytes.
/// @return Owned transformed or retained handle.
rt_string rt_str_replace(rt_string haystack, rt_string needle, rt_string replacement);

/// @brief Check if string starts with prefix.
/// @details Compares bytes; an empty prefix matches, while either null operand
///          returns false.
/// @param str Borrowed source string.
/// @param prefix Borrowed prefix.
/// @return One on match; otherwise zero.
int64_t rt_str_starts_with(rt_string str, rt_string prefix);

/// @brief Check if string ends with suffix.
/// @details Compares bytes; an empty suffix matches, while either null operand
///          returns false.
/// @param str Borrowed source string.
/// @param suffix Borrowed suffix.
/// @return One on match; otherwise zero.
int64_t rt_str_ends_with(rt_string str, rt_string suffix);

/// @brief Check if string contains needle.
/// @details Uses a byte substring search. Empty needles match and null operands
///          do not.
/// @param str Borrowed source string.
/// @param needle Borrowed byte sequence.
/// @return One on match; otherwise zero.
int64_t rt_str_has(rt_string str, rt_string needle);

/// @brief Count non-overlapping occurrences of needle in str.
/// @details Null operands and an empty needle return zero. A count that would
///          exceed `INT64_MAX` traps and returns the saturated count if the hook
///          returns.
/// @param str Borrowed source string.
/// @param needle Borrowed byte sequence.
/// @return Number of non-overlapping byte matches.
int64_t rt_str_count(rt_string str, rt_string needle);

/// @brief Pad string on the left to reach specified width.
/// @details Width is measured in bytes and padding must contain exactly one
///          byte; a longer padding string traps. Non-positive/already-satisfied
///          width or null/empty padding retains the source. Null source returns
///          empty. Oversized width traps.
/// @param str Borrowed source string.
/// @param width Target byte width.
/// @param pad_str Borrowed one-byte padding string.
/// @return Owned padded, retained, or empty handle; `NULL` on failure.
rt_string rt_str_pad_left(rt_string str, int64_t width, rt_string pad_str);

/// @brief Pad string on the right to reach specified width.
/// @details Uses the byte-width, validation, and ownership rules of
///          @ref rt_str_pad_left, appending rather than prepending padding.
/// @param str Borrowed source string.
/// @param width Target byte width.
/// @param pad_str Borrowed one-byte padding string.
/// @return Owned padded, retained, or empty handle; `NULL` on failure.
rt_string rt_str_pad_right(rt_string str, int64_t width, rt_string pad_str);

/// @brief Split string by delimiter into a sequence.
/// @details Matches non-overlapping delimiter byte spans and preserves leading,
///          adjacent, and trailing empty segments. Null input becomes one empty
///          segment. A null/empty delimiter or one longer than the source yields
///          one retained source element.
/// @param str Borrowed source string.
/// @param delim Borrowed delimiter.
/// @return New owned-element Seq whose string elements are retained by it, or
///         `NULL` after size/allocation failure.
void *rt_str_split(rt_string str, rt_string delim);

/// @brief Split a string into logical lines, normalizing CRLF to LF.
/// @details Splits at every LF, preserves empty segments including a trailing
///          one, and removes at most one CR immediately before each separator
///          or end. Null input yields one empty line.
/// @param str Borrowed source string.
/// @return New owned-element Seq of line strings, or `NULL` on failure.
void *rt_str_lines(rt_string str);

/// @brief 1 if the first character of @p s may start an identifier (ASCII letter or '_').
/// @param s Borrowed string; null/empty returns false.
/// @return One when the first byte is an ASCII letter or underscore.
int8_t rt_text_char_is_identifier_start(rt_string s);
/// @brief 1 if the first character of @p s may continue an identifier (ASCII letter, digit, '_').
/// @param s Borrowed string; null/empty returns false.
/// @return One when the first byte is an ASCII letter, digit, or underscore.
int8_t rt_text_char_is_identifier_part(rt_string s);
/// @brief 1 if the first character of @p s is ASCII alphanumeric (letter or digit).
/// @param s Borrowed string; null/empty returns false.
/// @return One when the first byte is an ASCII letter or digit.
int8_t rt_text_char_is_alnum(rt_string s);
/// @brief Classify the first UTF-8 scalar using deterministic Unicode word rules.
/// @details Letters, marks, numbers, connector punctuation, and join controls
///          from Unicode 16.0 continue words. Malformed/empty input is false.
/// @param s Borrowed string; only its first scalar is inspected.
/// @return One for a Unicode word scalar, otherwise zero.
int8_t rt_text_char_is_word(rt_string s);

/// @brief Join sequence of strings with separator.
/// @details Borrows rather than consumes the sequence and its elements. Null
///          sequence or an empty sequence returns the empty singleton. Null
///          separator and null elements contribute zero bytes. Length overflow
///          traps.
/// @param sep Borrowed separator; null means empty.
/// @param seq Borrowed Seq of string handles.
/// @return Owned joined string or `NULL` on failure.
rt_string rt_str_join(rt_string sep, void *seq);

/// @brief Repeat string count times.
/// @details Null/empty input and non-positive counts return the empty singleton.
///          Product overflow traps.
/// @param str Borrowed source string.
/// @param count Number of byte-sequence repetitions.
/// @return Owned repeated string or `NULL` on failure.
rt_string rt_str_repeat(rt_string str, int64_t count);

/// @brief Reverse string by UTF-8 codepoints (multi-byte aware).
/// @details Strictly validates every sequence, records codepoint boundaries in
///          temporary storage, and copies codepoints in reverse order. Null or
///          empty input returns the empty singleton; malformed UTF-8 and
///          allocation/size failures trap.
/// @param str Borrowed source string.
/// @return Owned reversed string or `NULL` on failure.
rt_string rt_str_flip(rt_string str);

/// @brief Compare two strings, returning -1, 0, or 1.
/// @details Applies unsigned-byte lexicographic ordering with null ordered
///          before non-null.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return -1, 0, or 1 according to the total ordering.
int64_t rt_str_cmp(rt_string a, rt_string b);

/// @brief Case-insensitive string comparison, returning -1, 0, or 1.
/// @details Uses the same null ordering as @ref rt_str_cmp and folds ASCII
///          `A-Z` only; all other bytes compare unchanged.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return -1, 0, or 1 according to ASCII-folded byte ordering.
int64_t rt_str_cmp_nocase(rt_string a, rt_string b);

/// @brief Capitalize the first character of a string.
/// @details Copies the bytes and uppercases only the first byte when it is
///          ASCII `a-z`; the remainder is unchanged. Null/empty input produces
///          a newly allocated empty string.
/// @param str Borrowed source.
/// @return Newly allocated owned string.
rt_string rt_str_capitalize(rt_string str);

/// @brief Title-case a string (capitalize first letter of each word).
/// @details Uppercases the first ASCII byte after the start or ASCII whitespace
///          and leaves every other byte, including existing case, unchanged.
///          Null/empty input produces a newly allocated empty string.
/// @param str Borrowed source.
/// @return Newly allocated owned string.
rt_string rt_str_title(rt_string str);

/// @brief Remove a prefix from a string if present.
/// @details Performs byte matching and always returns a new copy. Null source
///          becomes a newly allocated empty string; null/empty/nonmatching
///          prefix copies the complete source.
/// @param str Borrowed source.
/// @param prefix Borrowed prefix.
/// @return Newly allocated owned copy with one matching prefix removed.
rt_string rt_str_remove_prefix(rt_string str, rt_string prefix);

/// @brief Remove a suffix from a string if present.
/// @details Performs byte matching and otherwise follows the allocation/null
///          rules of @ref rt_str_remove_prefix.
/// @param str Borrowed source.
/// @param suffix Borrowed suffix.
/// @return Newly allocated owned copy with one matching suffix removed.
rt_string rt_str_remove_suffix(rt_string str, rt_string suffix);

/// @brief Find the last occurrence of a needle in a string.
/// @details Searches bytes backward. An empty needle matches at
///          `Length+1`; null operands miss.
/// @param haystack Borrowed source.
/// @param needle Borrowed byte sequence.
/// @return One-based byte index of the last occurrence, or zero.
int64_t rt_str_last_index_of(rt_string haystack, rt_string needle);

/// @brief Trim a specific byte from both ends of a string.
/// @details Uses only the first stored byte of @p ch, even when it begins a
///          multibyte sequence. Null source becomes an allocated empty string;
///          null/empty @p ch returns a new complete copy.
/// @param str Borrowed source.
/// @param ch Borrowed byte string naming the trim byte.
/// @return Newly allocated owned trimmed copy.
rt_string rt_str_trim_char(rt_string str, rt_string ch);

/// @brief Convert a byte string to an ASCII slug.
/// @details Retains lowercase ASCII alphanumerics, folds uppercase ASCII, and
///          collapses every run of other bytes—including non-ASCII bytes—to one
///          hyphen, with leading/trailing separators removed. Null/empty input
///          yields an allocated empty string.
/// @param str Borrowed source.
/// @return Newly allocated owned slug, or `NULL` after allocation failure.
rt_string rt_str_slug(rt_string str);

/// @brief Compute byte-wise Levenshtein edit distance.
/// @details Null is treated as empty. Uses O(min(m,n)) temporary space and
///          byte insertions, deletions, and substitutions.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return Edit distance, or -1 when a length exceeds `INT64_MAX`, temporary
///         size arithmetic overflows, or allocation fails.
int64_t rt_str_levenshtein(rt_string a, rt_string b);

/// @brief Compute byte-wise Jaro similarity.
/// @details Null is treated as empty; two empty values score one. Temporary
///          match-array allocation failure and unsupported huge lengths return
///          zero, indistinguishable from no similarity.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return Similarity in `[0.0, 1.0]`.
double rt_str_jaro(rt_string a, rt_string b);

/// @brief Compute byte-wise Jaro-Winkler similarity.
/// @details Adds a 0.1 bonus for each equal leading byte, up to four, to
///          @ref rt_str_jaro.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return Similarity in `[0.0, 1.0]`.
double rt_str_jaro_winkler(rt_string a, rt_string b);

/// @brief Compute Hamming distance between equal-length byte strings.
/// @details Null is treated as empty.
/// @param a Borrowed first string.
/// @param b Borrowed second string.
/// @return Number of differing byte positions, or -1 for unequal lengths.
int64_t rt_str_hamming(rt_string a, rt_string b);

/// @brief Convert string to camelCase.
/// @details Splits on space, tab, underscore, hyphen, ASCII lower-to-upper
///          transitions, and acronym boundaries. Drops separators, ASCII-folds
///          all words to lowercase, then uppercases the first byte of every
///          word after the first. Null/empty yields an allocated empty string.
/// @param str Borrowed source.
/// @return Newly allocated owned result, or `NULL` after trapped size,
///         allocation, or builder failure.
rt_string rt_str_camel_case(rt_string str);

/// @brief Convert string to PascalCase.
/// @details Uses @ref rt_str_camel_case word splitting/folding but uppercases
///          the first byte of every word.
/// @param str Borrowed source.
/// @return Newly allocated owned result or `NULL` on failure.
rt_string rt_str_pascal_case(rt_string str);

/// @brief Convert string to snake_case.
/// @details Uses the shared word splitter, lowercases ASCII bytes, and joins
///          words with underscores.
/// @param str Borrowed source.
/// @return Newly allocated owned result or `NULL` on failure.
rt_string rt_str_snake_case(rt_string str);

/// @brief Convert string to kebab-case.
/// @details Uses the shared word splitter, lowercases ASCII bytes, and joins
///          words with hyphens.
/// @param str Borrowed source.
/// @return Newly allocated owned result or `NULL` on failure.
rt_string rt_str_kebab_case(rt_string str);

/// @brief Convert string to SCREAMING_SNAKE_CASE.
/// @details Uses the shared word splitter, uppercases ASCII bytes, and joins
///          words with underscores.
/// @param str Borrowed source.
/// @return Newly allocated owned result or `NULL` on failure.
rt_string rt_str_screaming_snake(rt_string str);

/// @brief SQL LIKE pattern matching (case-sensitive).
/// @details Null strings are empty. `%` matches any sequence and `_` consumes
///          one strictly validated UTF-8 codepoint; backslash quotes the next
///          pattern byte. Literal comparisons are byte-wise. Backtracking after
///          `%` also validates codepoint boundaries, and malformed text on such
///          a path traps and returns false if the hook returns.
/// @param text Borrowed text.
/// @param pattern Borrowed pattern.
/// @return One for a complete match; otherwise zero.
int8_t rt_str_like(rt_string text, rt_string pattern);

/// @brief SQL ILIKE pattern matching (case-insensitive).
/// @details Uses @ref rt_str_like wildcard/escape semantics and folds only
///          ASCII letters during literal-byte comparison.
/// @param text Borrowed text.
/// @param pattern Borrowed pattern.
/// @return One for a complete ASCII-case-insensitive match; otherwise zero.
int8_t rt_str_like_ci(rt_string text, rt_string pattern);

#ifdef __cplusplus
}
#endif
