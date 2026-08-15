//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf_json.h
// Purpose: Minimal in-place JSON scanner used by the glTF importer. Operates
//   directly on a (json, len) buffer with byte-offset cursors — it never builds
//   a DOM, so the loader can walk the document with zero intermediate
//   allocations except for the explicit string-extracting helpers.
// Key invariants:
//   - All scanners take a (json, len) pair and 0-based byte offsets; callers
//     must keep len in sync with the buffer.
//   - Object/array ranges are [start, end) byte offsets into the same buffer.
//   - Only RFC 8259 whitespace and well-formed UTF-8 Unicode scalar values are
//     accepted; C-string extraction rejects decoded U+0000.
//   - String extractors return malloc'd C strings the caller owns and must
//     free(); numeric extraction uses stack storage first and may allocate for
//     unusually long tokens. Navigation and validation helpers allocate nothing.
// Ownership/Lifetime:
//   - No global state; every call is pure over its argument buffer.
// Links: rt_gltf_json.c, rt_gltf.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_json.h
 * @brief Declares the length-bounded raw JSON scanner used by glTF preload and import.
 * @details Every range is a half-open byte span into caller-owned immutable storage. Functions
 *          that return offsets use `SIZE_MAX` for malformed input; functions ending in `_alloc`
 *          return heap strings owned by the caller. The scanner keeps no global state and builds
 *          no intermediate JSON object graph.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Skip RFC 8259 JSON whitespace beginning at a byte offset.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Starting offset.
/// @return First non-whitespace offset at or after @p pos, or @p len for null/out-of-range input.
size_t gltf_json_skip_ws(const char *json, size_t len, size_t pos);

/// @brief Skip a quoted string token (pos must be at the opening quote); return
///   the offset just past the closing quote.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Offset of the opening quote.
/// @return Offset after the closing quote, or `SIZE_MAX` for invalid UTF-8, invalid escapes,
/// unpaired surrogates, or unterminated input.
size_t gltf_json_skip_string_raw(const char *json, size_t len, size_t pos);

/// @brief Read a quoted string (with escape handling) into a fresh malloc'd
///   buffer; *out_next receives the offset past the string. Returns NULL on error.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Offset of the opening quote.
/// @param out_next Optional destination for the offset after the closing quote.
/// @return Heap-allocated UTF-8 string, or NULL for malformed input or allocation failure.
char *gltf_json_read_string_alloc(const char *json, size_t len, size_t pos, size_t *out_next);

/// @brief Test whether the quoted JSON string at @p pos equals a decoded key.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Offset of the candidate string's opening quote.
/// @param key NUL-terminated decoded key to compare.
/// @param out_next Optional destination for the offset after the closing quote.
/// @return Non-zero when the decoded string exactly equals @p key.
int gltf_json_key_matches(
    const char *json, size_t len, size_t pos, const char *key, size_t *out_next);

/// @brief Skip a complete JSON value (object/array/string/number/literal) and
///   return the offset just past it.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Offset at or before the value.
/// @return Offset immediately following the complete value, or `SIZE_MAX` for malformed input.
size_t gltf_json_skip_value(const char *json, size_t len, size_t pos);

/**
 * @brief Validate one complete JSON document without allocating a DOM.
 * @param json Exact document bytes; embedded NUL/control bytes are invalid JSON.
 * @param len Exact byte length of @p json.
 * @return 1 when one value consumes the whole document after optional whitespace,
 * otherwise 0.
 */
int gltf_json_validate_document(const char *json, size_t len);

/**
 * @brief Validate the lexical form of glTF fields whose schema requires integral tokens.
 * @details Requires an object root and traverses core glTF objects plus supported extension
 *          objects without allocating a DOM. Recognized fields also enforce their object, array,
 *          numeric, or boolean JSON type where the scanner has schema context.
 *          Numeric values used by indices, counts, enum fields, integer arrays, attribute maps,
 *          and boolean-like fields must be complete base-10 integers: fractions, exponents,
 *          leading zeroes, suffixes, sign-only forms, and signed-64 overflow are rejected. Unknown
 *          optional extension objects and application-defined `extras` remain opaque.
 * @param json Exact structurally valid JSON document bytes.
 * @param len Exact byte length of @p json.
 * @return 1 when every recognized integral token is exact, otherwise 0.
 */
int gltf_json_validate_gltf_integral_tokens(const char *json, size_t len);

/// @brief From an opening bracket/brace at pos, return the offset just past its
///   matching close character.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param pos Offset of the opening delimiter.
/// @param open_ch Opening delimiter to count.
/// @param close_ch Corresponding closing delimiter.
/// @return Offset after the matching close, or `SIZE_MAX` for a non-JSON pair or any malformed
/// nested value.
size_t gltf_json_find_matching(
    const char *json, size_t len, size_t pos, char open_ch, char close_ch);

/// @brief Locate a top-level array property and return its full bracketed value span.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param key Root-object property name.
/// @param out_start Optional destination for the opening-bracket offset.
/// @param out_end Optional destination for the offset after the closing bracket.
/// @return Non-zero when the complete root document has one direct property containing a valid
/// array; duplicate queried keys and malformed suffixes fail.
int gltf_json_find_top_level_array(
    const char *json, size_t len, const char *key, size_t *out_start, size_t *out_end);

/// @brief Return a malloc'd copy of the string property `key` within the object
///   range, or NULL if absent/not a string.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset after the object's closing brace.
/// @param key Direct property name.
/// @return Heap-allocated decoded string, or NULL when absent, duplicated, malformed, outside the
/// exact object range, or not a string.
char *gltf_json_object_get_string(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key);

/// @brief Read a direct object property as an exact non-negative `size_t`.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset after the object's closing brace.
/// @param key Direct property name.
/// @param fallback Value returned for absence, invalid syntax, or overflow.
/// @return Parsed property value or @p fallback.
size_t gltf_json_object_get_size(const char *json,
                                 size_t len,
                                 size_t obj_start,
                                 size_t obj_end,
                                 const char *key,
                                 size_t fallback);

/// @brief Read a direct object property as an exact signed `int`.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset after the object's closing brace.
/// @param key Direct property name.
/// @param fallback Value returned for absence, invalid syntax, or overflow.
/// @return Parsed property value or @p fallback.
int gltf_json_object_get_int(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key, int fallback);

/// @brief Find a unique direct property within one exact object range.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset after the object's closing brace.
/// @param key Direct property name.
/// @param out_start Optional destination for the value's first-byte offset.
/// @param out_end Optional destination for the offset following the complete value.
/// @return Non-zero only after the entire object is valid and exactly one queried key is found;
/// outputs remain `SIZE_MAX` on failure.
int gltf_json_object_find_value(const char *json,
                                size_t len,
                                size_t obj_start,
                                size_t obj_end,
                                const char *key,
                                size_t *out_start,
                                size_t *out_end);

/// @brief Fill [*out_start,*out_end) with one element span after validating the exact full array.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @param out_start Optional destination for the element's first-byte offset.
/// @param out_end Optional destination for the offset following the element.
/// @return Non-zero when the validated array contains the requested element; outputs remain
/// `SIZE_MAX` on failure.
int gltf_json_array_item_range(const char *json,
                               size_t len,
                               size_t array_start,
                               size_t array_end,
                               int item_index,
                               size_t *out_start,
                               size_t *out_end);

/// @brief Read an array element as a finite C-locale double.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @param fallback Value returned for absence, invalid syntax, or non-finite output.
/// @return Parsed finite value or @p fallback.
double gltf_json_array_get_number(const char *json,
                                  size_t len,
                                  size_t array_start,
                                  size_t array_end,
                                  int item_index,
                                  double fallback);

/// @brief Return a malloc'd copy of string element `item_index` within the array
///   range, or NULL if absent/not a string.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @return Heap-allocated decoded string, or NULL when absent, malformed, or not a string.
char *gltf_json_array_get_string_alloc(
    const char *json, size_t len, size_t array_start, size_t array_end, int item_index);

/// @brief Return the boolean property `key` within the object range as 0/1, or
///   `fallback` if absent.
/// @param json Source JSON bytes.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset after the object's closing brace.
/// @param key Direct property name.
/// @param fallback Value returned for an absent or invalid property.
/// @return One for true/nonzero, zero for false/zero, or @p fallback.
int gltf_json_object_get_boolish(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key, int fallback);

#ifdef __cplusplus
}
#endif
