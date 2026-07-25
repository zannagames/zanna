//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf_json.c
// Purpose: Allocation-free in-place JSON scanner for the glTF importer. Walks a
//   (json, len) buffer with byte-offset cursors to locate object/array members
//   and extract scalars/strings, without constructing a DOM.
// Key invariants:
//   - Scanners are pure over their (json, len) argument buffer; no global state.
//   - Object/array ranges are [start, end) byte offsets into that buffer.
// Ownership/Lifetime:
//   - `*_alloc` / `*_get_string` helpers return malloc'd strings owned by the
//     caller; every other helper allocates nothing.
// Links: rt_gltf_json.h, rt_gltf.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_json.c
 * @brief Implements an allocation-light, length-bounded JSON scanner for glTF preload work.
 * @details The scanner validates and navigates raw `(pointer, length)` JSON without building a
 *          runtime object graph. It recognizes complete JSON grammar, decodes strings on demand,
 *          returns half-open byte spans for nested values, and applies glTF-specific exact-integer
 *          checks to fields whose schema forbids fractional or exponential spellings. All offsets
 *          are relative to the caller-owned immutable buffer.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_gltf_json.h"
#include "rt_numeric.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// Maximum nested object/array depth accepted by recursive JSON validation.
#define GLTF_JSON_MAX_DEPTH 512

/// @brief Decode one hexadecimal JSON escape digit.
/// @param c ASCII character to decode.
/// @return Value in [0, 15], or -1 if @p c is not a hex digit.
static int gltf_json_hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/// @brief Decode a four-hex-digit JSON `\\uXXXX` escape.
/// @param json Source JSON buffer.
/// @param len Source buffer byte length.
/// @param pos Offset of the first hex digit after `\\u`.
/// @param out_cp Receives the decoded UTF-16 code unit.
/// @return 1 on success, 0 on truncation or invalid hex.
static int gltf_json_read_hex4(const char *json, size_t len, size_t pos, uint32_t *out_cp) {
    uint32_t value = 0;
    if (!json || !out_cp || pos > len || len - pos < 4u)
        return 0;
    for (int i = 0; i < 4; i++) {
        int digit = gltf_json_hex_value(json[pos + (size_t)i]);
        if (digit < 0)
            return 0;
        value = (value << 4) | (uint32_t)digit;
    }
    *out_cp = value;
    return 1;
}

/// @brief Append one Unicode scalar value as UTF-8 to an output string buffer.
/// @param out Destination buffer allocated by the caller.
/// @param count In/out byte count already written to @p out.
/// @param cap Capacity of @p out in bytes.
/// @param cp Unicode scalar value to append.
/// @return 1 on success, 0 if @p cp is invalid or there is not enough capacity.
static int gltf_json_append_utf8(char *out, size_t *count, size_t cap, uint32_t cp) {
    if (!out || !count || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        return 0;
    if (cp <= 0x7Fu) {
        if (*count + 1u > cap)
            return 0;
        out[(*count)++] = (char)cp;
    } else if (cp <= 0x7FFu) {
        if (*count + 2u > cap)
            return 0;
        out[(*count)++] = (char)(0xC0u | (cp >> 6));
        out[(*count)++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        if (*count + 3u > cap)
            return 0;
        out[(*count)++] = (char)(0xE0u | (cp >> 12));
        out[(*count)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[(*count)++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        if (*count + 4u > cap)
            return 0;
        out[(*count)++] = (char)(0xF0u | (cp >> 18));
        out[(*count)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[(*count)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[(*count)++] = (char)(0x80u | (cp & 0x3Fu));
    }
    return 1;
}

/// @brief Check that a JSON literal consumes the entire raw value span after whitespace.
/// @param json Source JSON buffer.
/// @param token_end Offset just after the candidate literal.
/// @param value_end End offset of the raw value span.
/// @return 1 when only JSON whitespace follows the literal before @p value_end.
static int gltf_json_literal_ends_cleanly(const char *json, size_t token_end, size_t value_end) {
    size_t pos = token_end;
    while (pos < value_end && isspace((unsigned char)json[pos]))
        pos++;
    return pos == value_end;
}

/// @brief Advance @p pos past JSON whitespace (space/tab/CR/LF) in the raw byte scanner.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Starting byte offset.
/// @return First offset at or after @p pos that is not JSON whitespace, or @p len.
size_t gltf_json_skip_ws(const char *json, size_t len, size_t pos) {
    while (pos < len) {
        char c = json[pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        pos++;
    }
    return pos;
}

/// @brief Skip a quoted JSON string (honoring backslash escapes) starting at @p pos.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Offset of the opening quote.
/// @return Index just past the closing quote, or SIZE_MAX if @p pos isn't a string or it's
/// unterminated.
size_t gltf_json_skip_string_raw(const char *json, size_t len, size_t pos) {
    if (!json || pos >= len || json[pos] != '"')
        return SIZE_MAX;
    pos++;
    while (pos < len) {
        unsigned char c = (unsigned char)json[pos++];
        if (c < 0x20u)
            return SIZE_MAX;
        if (c == '"')
            return pos;
        if (c == '\\') {
            char esc;
            if (pos >= len)
                return SIZE_MAX;
            esc = json[pos++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    break;
                case 'u': {
                    uint32_t ignored_cp;
                    if (!gltf_json_read_hex4(json, len, pos, &ignored_cp))
                        return SIZE_MAX;
                    pos += 4u;
                    break;
                }
                default:
                    return SIZE_MAX;
            }
        }
    }
    return SIZE_MAX;
}

/// @brief Read and unescape a quoted JSON string at @p pos into a malloc'd C string (caller frees).
/// @details Decodes the standard JSON escape set (\" \\ \/ \b \f \n \r \t); rejects any other
///          escape. Sets @p out_next past the closing quote on success.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Offset of the opening quote.
/// @param out_next Optional destination for the offset following the closing quote.
/// @return The decoded string, or NULL on a malformed/unterminated string or allocation failure.
char *gltf_json_read_string_alloc(const char *json, size_t len, size_t pos, size_t *out_next) {
    char *out;
    size_t cap;
    size_t count = 0;
    if (out_next)
        *out_next = SIZE_MAX;
    if (!json || pos >= len || json[pos] != '"')
        return NULL;
    cap = len - pos + 1u;
    out = (char *)malloc(cap);
    if (!out)
        return NULL;
    pos++;
    while (pos < len) {
        char c = json[pos++];
        if (c == '"') {
            out[count] = '\0';
            if (out_next)
                *out_next = pos;
            return out;
        }
        if (c == '\\') {
            if (pos >= len)
                break;
            c = json[pos++];
            switch (c) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'u': {
                    uint32_t cp;
                    if (!gltf_json_read_hex4(json, len, pos, &cp)) {
                        free(out);
                        return NULL;
                    }
                    pos += 4u;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        uint32_t low;
                        if (pos + 6u > len || json[pos] != '\\' || json[pos + 1u] != 'u' ||
                            !gltf_json_read_hex4(json, len, pos + 2u, &low) || low < 0xDC00u ||
                            low > 0xDFFFu) {
                            free(out);
                            return NULL;
                        }
                        pos += 6u;
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        free(out);
                        return NULL;
                    }
                    if (cp == 0u) {
                        free(out);
                        return NULL;
                    }
                    if (!gltf_json_append_utf8(out, &count, cap, cp)) {
                        free(out);
                        return NULL;
                    }
                    continue;
                }
                default:
                    free(out);
                    return NULL;
            }
        } else if ((unsigned char)c < 0x20u || c == '\0') {
            free(out);
            return NULL;
        }
        out[count++] = c;
    }
    free(out);
    return NULL;
}

/// @brief Whether the JSON string at @p pos equals @p key, advancing @p out_next past it either
/// way.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Offset of the candidate string's opening quote.
/// @param key NUL-terminated decoded key to compare.
/// @param out_next Optional destination for the offset after the candidate string.
/// @return Non-zero when the decoded JSON string exactly equals @p key.
int gltf_json_key_matches(
    const char *json, size_t len, size_t pos, const char *key, size_t *out_next) {
    size_t key_len;
    size_t start;
    size_t cursor;
    if (out_next)
        *out_next = SIZE_MAX;
    if (!json || !key || pos >= len || json[pos] != '"')
        return 0;
    key_len = strlen(key);
    start = pos + 1u;
    cursor = start;
    while (cursor < len) {
        unsigned char c = (unsigned char)json[cursor];
        if (c < 0x20u)
            return 0;
        if (c == '\\')
            break;
        if (c == '"') {
            if (out_next)
                *out_next = cursor + 1u;
            return cursor - start == key_len && memcmp(json + start, key, key_len) == 0;
        }
        cursor++;
    }
    char *decoded = gltf_json_read_string_alloc(json, len, pos, out_next);
    int matches = decoded && strcmp(decoded, key) == 0;
    free(decoded);
    return matches;
}

/// @brief Skip and validate a JSON primitive token.
/// @details Accepts the JSON literals `true`, `false`, `null`, and numbers matching the JSON number
///          grammar. Whitespace may trail the primitive before the delimiter, but arbitrary tokens
///          are rejected so malformed glTF cannot be silently skipped.
/// @param json Source JSON buffer.
/// @param len Source byte length.
/// @param pos Primitive start offset.
/// @return Offset of the first delimiter after the primitive, or SIZE_MAX on malformed input.
static size_t gltf_json_skip_primitive(const char *json, size_t len, size_t pos) {
    size_t start = pos;
    size_t end;
    size_t p;
    if (!json || pos >= len)
        return SIZE_MAX;
    while (pos < len && json[pos] != ',' && json[pos] != '}' && json[pos] != ']')
        pos++;
    end = pos;
    while (end > start && isspace((unsigned char)json[end - 1u]))
        end--;
    if (end == start)
        return SIZE_MAX;
    if ((end - start == 4u && memcmp(json + start, "true", 4u) == 0) ||
        (end - start == 4u && memcmp(json + start, "null", 4u) == 0) ||
        (end - start == 5u && memcmp(json + start, "false", 5u) == 0))
        return pos;
    p = start;
    if (json[p] == '-')
        p++;
    if (p >= end)
        return SIZE_MAX;
    if (json[p] == '0') {
        p++;
    } else if (json[p] >= '1' && json[p] <= '9') {
        while (p < end && json[p] >= '0' && json[p] <= '9')
            p++;
    } else {
        return SIZE_MAX;
    }
    if (p < end && json[p] == '.') {
        p++;
        if (p >= end || json[p] < '0' || json[p] > '9')
            return SIZE_MAX;
        while (p < end && json[p] >= '0' && json[p] <= '9')
            p++;
    }
    if (p < end && (json[p] == 'e' || json[p] == 'E')) {
        p++;
        if (p < end && (json[p] == '+' || json[p] == '-'))
            p++;
        if (p >= end || json[p] < '0' || json[p] > '9')
            return SIZE_MAX;
        while (p < end && json[p] >= '0' && json[p] <= '9')
            p++;
    }
    return p == end ? pos : SIZE_MAX;
}

/// @brief Skip a complete JSON value (string, number, object, or array) starting at @p pos.
/// @details Strings are skipped whole; objects/arrays are skipped by tracking nesting depth so the
///          scanner lands on the value's first sibling/terminator. Primitives run to the next
///          delimiter. Returns SIZE_MAX on malformed/unbalanced input.
/**
 * @brief Recursively skip one JSON value while enforcing container separator grammar.
 * @param json Exact JSON bytes.
 * @param len Exact byte length of @p json.
 * @param pos Offset at or before the value.
 * @param depth Current object/array nesting depth.
 * @return Offset after the value, or `SIZE_MAX` for malformed input/depth overflow.
 */
static size_t gltf_json_skip_value_depth(const char *json, size_t len, size_t pos, int depth) {
    pos = gltf_json_skip_ws(json, len, pos);
    if (pos >= len || depth > GLTF_JSON_MAX_DEPTH)
        return SIZE_MAX;
    if (json[pos] == '"')
        return gltf_json_skip_string_raw(json, len, pos);
    if (json[pos] != '{' && json[pos] != '[')
        return gltf_json_skip_primitive(json, len, pos);

    if (json[pos] == '[') {
        pos = gltf_json_skip_ws(json, len, pos + 1u);
        if (pos < len && json[pos] == ']')
            return pos + 1u;
        for (;;) {
            pos = gltf_json_skip_value_depth(json, len, pos, depth + 1);
            if (pos == SIZE_MAX)
                return SIZE_MAX;
            pos = gltf_json_skip_ws(json, len, pos);
            if (pos >= len)
                return SIZE_MAX;
            if (json[pos] == ']')
                return pos + 1u;
            if (json[pos] != ',')
                return SIZE_MAX;
            pos = gltf_json_skip_ws(json, len, pos + 1u);
            if (pos >= len || json[pos] == ',' || json[pos] == ']')
                return SIZE_MAX;
        }
    }

    pos = gltf_json_skip_ws(json, len, pos + 1u);
    if (pos < len && json[pos] == '}')
        return pos + 1u;
    for (;;) {
        if (pos >= len || json[pos] != '"')
            return SIZE_MAX;
        pos = gltf_json_skip_string_raw(json, len, pos);
        if (pos == SIZE_MAX)
            return SIZE_MAX;
        pos = gltf_json_skip_ws(json, len, pos);
        if (pos >= len || json[pos] != ':')
            return SIZE_MAX;
        pos = gltf_json_skip_value_depth(json, len, pos + 1u, depth + 1);
        if (pos == SIZE_MAX)
            return SIZE_MAX;
        pos = gltf_json_skip_ws(json, len, pos);
        if (pos >= len)
            return SIZE_MAX;
        if (json[pos] == '}')
            return pos + 1u;
        if (json[pos] != ',')
            return SIZE_MAX;
        pos = gltf_json_skip_ws(json, len, pos + 1u);
        if (pos >= len || json[pos] == ',' || json[pos] == '}')
            return SIZE_MAX;
    }
}

/// @brief Skip one complete, structurally valid JSON value.
/// @details Objects and arrays enforce their full comma/colon grammar recursively; callers never
///   receive a valid prefix from a container whose later element or separator is malformed.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Offset at or before the value's first non-whitespace byte.
/// @return Offset immediately after the value, or `SIZE_MAX` on malformed input/depth overflow.
size_t gltf_json_skip_value(const char *json, size_t len, size_t pos) {
    return gltf_json_skip_value_depth(json, len, pos, 0);
}

/**
 * @brief Validate that one JSON value consumes an entire byte buffer.
 * @param json Exact JSON bytes.
 * @param len Exact byte length of @p json.
 * @return 1 for a structurally valid complete document, otherwise 0.
 */
int gltf_json_validate_document(const char *json, size_t len) {
    size_t end;
    if (!json || len == 0)
        return 0;
    end = gltf_json_skip_value(json, len, 0);
    if (end == SIZE_MAX)
        return 0;
    return gltf_json_skip_ws(json, len, end) == len;
}

/// @brief Schema context controlling which raw JSON numeric tokens must be exact integers.
typedef enum gltf_json_integral_walk_mode {
    /// Ordinary recursive glTF object/value validation.
    GLTF_JSON_INTEGRAL_WALK_NORMAL = 0,
    /// Array whose numeric elements are required to be integers.
    GLTF_JSON_INTEGRAL_WALK_INTEGER_ARRAY = 1,
    /// Attribute semantic map whose numeric property values are accessor indices.
    GLTF_JSON_INTEGRAL_WALK_ATTRIBUTE_OBJECT = 2,
    /// Morph-target array whose entries are attribute semantic maps.
    GLTF_JSON_INTEGRAL_WALK_TARGET_ARRAY = 3,
    /// Extension object where only explicitly supported extension payloads are interpreted.
    GLTF_JSON_INTEGRAL_WALK_EXTENSION_MAP = 4
} gltf_json_integral_walk_mode;

/**
 * @brief Test a source object key against a NULL-terminated ASCII name table.
 * @param json Source JSON bytes.
 * @param len Source byte length.
 * @param key_pos Offset of the key's opening quote.
 * @param names NULL-terminated candidate table.
 * @return 1 when the decoded key equals one candidate, otherwise 0.
 */
static int gltf_json_integral_key_in(const char *json,
                                     size_t len,
                                     size_t key_pos,
                                     const char *const *names) {
    if (!names)
        return 0;
    for (size_t i = 0; names[i]; i++) {
        if (gltf_json_key_matches(json, len, key_pos, names[i], NULL))
            return 1;
    }
    return 0;
}

/**
 * @brief Parse one whitespace-bounded JSON token as an exact signed 64-bit decimal integer.
 * @details This lexical parser rejects fractions, exponents, plus signs, leading zeroes, suffixes,
 *          sign-only forms, and overflow while preserving the full `INT64_MIN` range.
 * @param json Source JSON bytes.
 * @param start Inclusive token-span start.
 * @param end Exclusive token-span end.
 * @param out Optional destination for the decoded integer.
 * @return 1 only when the entire trimmed span is one valid signed integer token.
 */
static int gltf_json_parse_i64_span(const char *json, size_t start, size_t end, int64_t *out) {
    uint64_t magnitude = 0u;
    uint64_t limit;
    size_t pos = start;
    int negative = 0;
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos < end && json[pos] == '-') {
        negative = 1;
        pos++;
    }
    if (pos >= end || json[pos] < '0' || json[pos] > '9')
        return 0;
    if (json[pos] == '0' && pos + 1u < end && json[pos + 1u] >= '0' && json[pos + 1u] <= '9')
        return 0;
    limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        uint64_t digit = (uint64_t)(json[pos] - '0');
        if (magnitude > (limit - digit) / 10u)
            return 0;
        magnitude = magnitude * 10u + digit;
        pos++;
    }
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos != end)
        return 0;
    if (out) {
        if (negative && magnitude == (uint64_t)INT64_MAX + 1u)
            *out = INT64_MIN;
        else
            *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    }
    return 1;
}

/**
 * @brief Return whether a raw value span begins with a JSON numeric token.
 * @param json Source JSON bytes.
 * @param start Inclusive value-span start.
 * @param end Exclusive value-span end.
 * @return 1 for `-` or a decimal digit after whitespace, otherwise 0.
 */
static int gltf_json_span_is_numeric(const char *json, size_t start, size_t end) {
    start = gltf_json_skip_ws(json, end, start);
    return start < end && (json[start] == '-' || (json[start] >= '0' && json[start] <= '9'));
}

/**
 * @brief Recursively validate recognized glTF integral token positions.
 * @param json Exact structurally valid JSON bytes.
 * @param len Source byte length.
 * @param start Inclusive value start.
 * @param end Exclusive value end.
 * @param mode Schema context inherited from the owning key.
 * @param depth Current recursion depth, bounded by `GLTF_JSON_MAX_DEPTH`.
 * @return 1 when this subtree contains no malformed recognized integral token.
 */
static int gltf_json_validate_integral_value(const char *json,
                                             size_t len,
                                             size_t start,
                                             size_t end,
                                             gltf_json_integral_walk_mode mode,
                                             int depth) {
    static const char *const scalar_integer_keys[] = {
        "buffer",        "bufferView", "byteLength", "byteOffset", "byteStride", "camera",
        "componentType", "count",      "index",      "indices",    "input",      "light",
        "magFilter",     "material",   "mesh",       "minFilter",  "mode",       "node",
        "output",        "sampler",    "scene",      "skin",       "skeleton",   "source",
        "target",        "texCoord",   "wrapS",      "wrapT",      NULL};
    static const char *const boolean_keys[] = {"doubleSided", "normalized", NULL};
    static const char *const integer_array_keys[] = {
        "children", "joints", "nodes", "variants", NULL};
    static const char *const supported_extensions[] = {"EXT_meshopt_compression",
                                                       "KHR_draco_mesh_compression",
                                                       "KHR_lights_punctual",
                                                       "KHR_materials_anisotropy",
                                                       "KHR_materials_clearcoat",
                                                       "KHR_materials_emissive_strength",
                                                       "KHR_materials_ior",
                                                       "KHR_materials_pbrSpecularGlossiness",
                                                       "KHR_materials_sheen",
                                                       "KHR_materials_specular",
                                                       "KHR_materials_transmission",
                                                       "KHR_materials_unlit",
                                                       "KHR_materials_variants",
                                                       "KHR_materials_volume",
                                                       "KHR_mesh_quantization",
                                                       "KHR_texture_basisu",
                                                       "KHR_texture_transform",
                                                       NULL};
    size_t pos;
    if (!json || start >= end || end > len || depth > GLTF_JSON_MAX_DEPTH)
        return 0;
    pos = gltf_json_skip_ws(json, len, start);
    if (pos >= end)
        return 0;
    if (json[pos] == '[') {
        pos = gltf_json_skip_ws(json, len, pos + 1u);
        while (pos < end && json[pos] != ']') {
            size_t item_end = gltf_json_skip_value(json, len, pos);
            gltf_json_integral_walk_mode item_mode = GLTF_JSON_INTEGRAL_WALK_NORMAL;
            if (item_end == SIZE_MAX || item_end > end)
                return 0;
            if (mode == GLTF_JSON_INTEGRAL_WALK_INTEGER_ARRAY &&
                gltf_json_span_is_numeric(json, pos, item_end) &&
                !gltf_json_parse_i64_span(json, pos, item_end, NULL))
                return 0;
            if (mode == GLTF_JSON_INTEGRAL_WALK_TARGET_ARRAY)
                item_mode = GLTF_JSON_INTEGRAL_WALK_ATTRIBUTE_OBJECT;
            if (!gltf_json_validate_integral_value(json, len, pos, item_end, item_mode, depth + 1))
                return 0;
            pos = gltf_json_skip_ws(json, len, item_end);
            if (pos < end && json[pos] == ',')
                pos = gltf_json_skip_ws(json, len, pos + 1u);
        }
        return pos < end && json[pos] == ']';
    }
    if (json[pos] != '{')
        return 1;
    pos = gltf_json_skip_ws(json, len, pos + 1u);
    while (pos < end && json[pos] != '}') {
        size_t key_pos = pos;
        size_t key_end;
        size_t value_start;
        size_t value_end;
        gltf_json_integral_walk_mode child_mode = GLTF_JSON_INTEGRAL_WALK_NORMAL;
        int recurse = 1;
        int64_t boolean_integer;
        if (json[pos] != '"')
            return 0;
        key_end = gltf_json_skip_string_raw(json, len, key_pos);
        if (key_end == SIZE_MAX)
            return 0;
        pos = gltf_json_skip_ws(json, len, key_end);
        if (pos >= end || json[pos] != ':')
            return 0;
        value_start = gltf_json_skip_ws(json, len, pos + 1u);
        value_end = gltf_json_skip_value(json, len, value_start);
        if (value_end == SIZE_MAX || value_end > end)
            return 0;

        if (mode == GLTF_JSON_INTEGRAL_WALK_EXTENSION_MAP) {
            recurse = gltf_json_integral_key_in(json, len, key_pos, supported_extensions);
        } else if (gltf_json_key_matches(json, len, key_pos, "extras", NULL)) {
            recurse = 0;
        } else {
            if ((mode == GLTF_JSON_INTEGRAL_WALK_ATTRIBUTE_OBJECT ||
                 gltf_json_integral_key_in(json, len, key_pos, scalar_integer_keys)) &&
                gltf_json_span_is_numeric(json, value_start, value_end) &&
                !gltf_json_parse_i64_span(json, value_start, value_end, NULL))
                return 0;
            if (gltf_json_integral_key_in(json, len, key_pos, boolean_keys) &&
                gltf_json_span_is_numeric(json, value_start, value_end)) {
                if (!gltf_json_parse_i64_span(json, value_start, value_end, &boolean_integer) ||
                    (boolean_integer != 0 && boolean_integer != 1))
                    return 0;
            }
            if (gltf_json_integral_key_in(json, len, key_pos, integer_array_keys))
                child_mode = GLTF_JSON_INTEGRAL_WALK_INTEGER_ARRAY;
            else if (gltf_json_key_matches(json, len, key_pos, "attributes", NULL))
                child_mode = GLTF_JSON_INTEGRAL_WALK_ATTRIBUTE_OBJECT;
            else if (gltf_json_key_matches(json, len, key_pos, "targets", NULL))
                child_mode = GLTF_JSON_INTEGRAL_WALK_TARGET_ARRAY;
            else if (gltf_json_key_matches(json, len, key_pos, "extensions", NULL))
                child_mode = GLTF_JSON_INTEGRAL_WALK_EXTENSION_MAP;
        }
        if (recurse && !gltf_json_validate_integral_value(
                           json, len, value_start, value_end, child_mode, depth + 1))
            return 0;
        pos = gltf_json_skip_ws(json, len, value_end);
        if (pos < end && json[pos] == ',')
            pos = gltf_json_skip_ws(json, len, pos + 1u);
    }
    return pos < end && json[pos] == '}';
}

/**
 * @brief Validate every schema-recognized integral and boolean token in a glTF JSON document.
 * @details First validates complete JSON structure, then walks supported glTF fields and
 *          extensions. Numeric tokens in integer positions must be exact decimal integers;
 *          numeric boolean spellings must be exactly zero or one. Opaque `extras` and unsupported
 *          extension payloads remain syntactically validated but are not schema-interpreted.
 * @param json Exact glTF JSON bytes.
 * @param len Exact byte length of @p json.
 * @return Non-zero when the document is structurally valid and every recognized token conforms.
 */
int gltf_json_validate_gltf_integral_tokens(const char *json, size_t len) {
    size_t start;
    size_t end;
    if (!gltf_json_validate_document(json, len))
        return 0;
    start = gltf_json_skip_ws(json, len, 0);
    end = gltf_json_skip_value(json, len, start);
    if (end == SIZE_MAX)
        return 0;
    return gltf_json_validate_integral_value(
        json, len, start, end, GLTF_JSON_INTEGRAL_WALK_NORMAL, 0);
}

/// @brief Find the index just past the bracket that closes the @p open_ch at @p pos.
/// @details Tracks nesting and skips quoted strings so brackets inside strings don't miscount.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param pos Offset of the opening delimiter.
/// @param open_ch Opening object or array delimiter to count.
/// @param close_ch Matching closing delimiter.
/// @return Index after the matching @p close_ch, or SIZE_MAX if unbalanced.
size_t gltf_json_find_matching(
    const char *json, size_t len, size_t pos, char open_ch, char close_ch) {
    int depth = 0;
    if (!json || pos >= len || json[pos] != open_ch)
        return SIZE_MAX;
    while (pos < len) {
        char c = json[pos];
        if (c == '"') {
            pos = gltf_json_skip_string_raw(json, len, pos);
            if (pos == SIZE_MAX)
                return SIZE_MAX;
            continue;
        }
        if (c == open_ch)
            depth++;
        else if (c == close_ch) {
            depth--;
            if (depth == 0)
                return pos + 1u;
        }
        if (depth < 0 || depth > GLTF_JSON_MAX_DEPTH)
            return SIZE_MAX;
        pos++;
    }
    return SIZE_MAX;
}

/// @brief Locate a top-level array property @p key in the root object, by raw byte scan.
/// @details Only matches keys at object depth 1 (true top level), then returns the byte range
///          of the '['..']' array value via @p out_start / @p out_end.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param key Root property name to locate.
/// @param out_start Optional destination for the array's opening-bracket offset.
/// @param out_end Optional destination for the offset following its closing bracket.
/// @return 1 if found (range set), 0 otherwise.
int gltf_json_find_top_level_array(
    const char *json, size_t len, const char *key, size_t *out_start, size_t *out_end) {
    int object_depth = 0;
    int array_depth = 0;
    size_t pos = 0;
    if (out_start)
        *out_start = SIZE_MAX;
    if (out_end)
        *out_end = SIZE_MAX;
    while (pos < len) {
        char c = json[pos];
        if (c == '"') {
            size_t next = gltf_json_skip_string_raw(json, len, pos);
            int at_top_key = 0;
            if (next == SIZE_MAX)
                return 0;
            if (object_depth == 1 && array_depth == 0)
                at_top_key = gltf_json_key_matches(json, len, pos, key, NULL);
            if (at_top_key) {
                size_t colon = gltf_json_skip_ws(json, len, next);
                size_t start;
                size_t end;
                if (colon < len && json[colon] == ':') {
                    start = gltf_json_skip_ws(json, len, colon + 1u);
                    if (start < len && json[start] == '[') {
                        end = gltf_json_skip_value(json, len, start);
                        if (end != SIZE_MAX && end > start && json[end - 1u] == ']') {
                            if (out_start)
                                *out_start = start;
                            if (out_end)
                                *out_end = end;
                            return 1;
                        }
                    }
                }
            }
            pos = next;
            continue;
        }
        if (c == '{')
            object_depth++;
        else if (c == '}')
            object_depth--;
        else if (c == '[')
            array_depth++;
        else if (c == ']')
            array_depth--;
        if (object_depth < 0 || array_depth < 0 || object_depth + array_depth > GLTF_JSON_MAX_DEPTH)
            return 0;
        pos++;
    }
    return 0;
}

/// @brief Read a direct string property @p key from the object spanning [obj_start, obj_end).
/// @details Only the object's own (depth-1) keys are considered; nested objects are skipped.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset immediately after the object's closing brace.
/// @param key Direct property name to locate.
/// @return The unescaped value (caller frees), or NULL if absent or not a string.
char *gltf_json_object_get_string(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key) {
    int depth = 0;
    size_t pos = obj_start;
    while (pos < obj_end && pos < len) {
        char c = json[pos];
        if (c == '"') {
            size_t next = gltf_json_skip_string_raw(json, len, pos);
            int at_key = 0;
            if (next == SIZE_MAX)
                return NULL;
            if (depth == 1)
                at_key = gltf_json_key_matches(json, len, pos, key, NULL);
            if (at_key) {
                size_t colon = gltf_json_skip_ws(json, len, next);
                size_t value;
                if (colon < obj_end && json[colon] == ':') {
                    value = gltf_json_skip_ws(json, len, colon + 1u);
                    if (value < obj_end && json[value] == '"')
                        return gltf_json_read_string_alloc(json, len, value, NULL);
                }
            }
            pos = next;
            continue;
        }
        if (c == '{')
            depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0)
                break;
        } else if (depth == 1 && c == ':') {
            pos = gltf_json_skip_value(json, len, pos + 1u);
            if (pos == SIZE_MAX)
                return NULL;
            continue;
        }
        pos++;
    }
    return NULL;
}

/// @brief Read a direct non-negative integer property @p key as size_t (overflow-checked).
/// @return The parsed value, or @p fallback if absent, non-numeric, or it would overflow.
/**
 * @brief Parse an exact non-negative JSON integer token into `size_t`.
 * @details Rejects signs, fractions, exponents, suffixes, leading zeroes, and overflow; optional
 * surrounding JSON whitespace is accepted.
 * @param json Source JSON bytes.
 * @param start Inclusive token-span start.
 * @param end Exclusive token-span end.
 * @param out Receives the parsed value on success.
 * @return 1 on exact successful conversion, otherwise 0 without modifying @p out.
 */
static int gltf_json_parse_unsigned_integer_span(const char *json,
                                                 size_t start,
                                                 size_t end,
                                                 size_t *out) {
    size_t result = 0;
    size_t pos = start;
    if (!json || !out)
        return 0;
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos >= end || json[pos] < '0' || json[pos] > '9')
        return 0;
    if (json[pos] == '0' && pos + 1u < end && json[pos + 1u] >= '0' && json[pos + 1u] <= '9')
        return 0;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        size_t digit = (size_t)(json[pos] - '0');
        if (result > (SIZE_MAX - digit) / 10u)
            return 0;
        result = result * 10u + digit;
        pos++;
    }
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos != end)
        return 0;
    *out = result;
    return 1;
}

/**
 * @brief Read a direct object property as an exact non-negative `size_t`.
 * @param json Source JSON byte buffer.
 * @param len Exact source byte length.
 * @param obj_start Offset of the object's opening brace.
 * @param obj_end Offset immediately after the object's closing brace.
 * @param key Direct property name to locate.
 * @param fallback Value returned for absence, invalid syntax, or overflow.
 * @return Parsed non-negative integer or @p fallback.
 */
size_t gltf_json_object_get_size(const char *json,
                                 size_t len,
                                 size_t obj_start,
                                 size_t obj_end,
                                 const char *key,
                                 size_t fallback) {
    size_t value_start;
    size_t value_end;
    size_t result;
    if (!gltf_json_object_find_value(
            json, len, obj_start, obj_end, key, &value_start, &value_end) ||
        !gltf_json_parse_unsigned_integer_span(json, value_start, value_end, &result))
        return fallback;
    return result;
}

/// @brief Read a direct signed integer property @p key as int (overflow-checked, allows leading
/// '-').
/// @return The parsed value, or @p fallback if absent, non-numeric, or it would overflow.
/**
 * @brief Parse an exact signed JSON integer token into `int`.
 * @details Rejects fractions, exponents, suffixes, sign-only forms, leading zeroes, and overflow;
 * both `INT_MIN` and `INT_MAX` remain representable without signed-overflow intermediates.
 * @param json Source JSON bytes.
 * @param start Inclusive token-span start.
 * @param end Exclusive token-span end.
 * @param out Receives the parsed value on success.
 * @return 1 on exact successful conversion, otherwise 0 without modifying @p out.
 */
static int gltf_json_parse_int_span(const char *json, size_t start, size_t end, int *out) {
    uint64_t magnitude = 0;
    uint64_t limit;
    size_t pos = start;
    int negative = 0;
    if (!json || !out)
        return 0;
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos < end && json[pos] == '-') {
        negative = 1;
        pos++;
    }
    if (pos >= end || json[pos] < '0' || json[pos] > '9')
        return 0;
    if (json[pos] == '0' && pos + 1u < end && json[pos + 1u] >= '0' && json[pos + 1u] <= '9')
        return 0;
    limit = negative ? (uint64_t)INT_MAX + 1u : (uint64_t)INT_MAX;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        uint64_t digit = (uint64_t)(json[pos] - '0');
        if (magnitude > (limit - digit) / 10u)
            return 0;
        magnitude = magnitude * 10u + digit;
        pos++;
    }
    while (pos < end && isspace((unsigned char)json[pos]))
        pos++;
    if (pos != end)
        return 0;
    if (negative && magnitude == (uint64_t)INT_MAX + 1u)
        *out = INT_MIN;
    else
        *out = negative ? -(int)magnitude : (int)magnitude;
    return 1;
}

/**
 * @brief Read a direct object property as an exact signed `int`.
 * @param json Source JSON byte buffer.
 * @param len Exact source byte length.
 * @param obj_start Offset of the object's opening brace.
 * @param obj_end Offset immediately after the object's closing brace.
 * @param key Direct property name to locate.
 * @param fallback Value returned for absence, invalid syntax, or overflow.
 * @return Parsed signed integer or @p fallback.
 */
int gltf_json_object_get_int(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key, int fallback) {
    size_t value_start;
    size_t value_end;
    int result;
    if (!gltf_json_object_find_value(
            json, len, obj_start, obj_end, key, &value_start, &value_end) ||
        !gltf_json_parse_int_span(json, value_start, value_end, &result))
        return fallback;
    return result;
}

/// @brief Find the byte range of a direct property @p key's value within an object.
/// @details Reports the [out_start, out_end) span of the raw value (any JSON type) for the
///          caller to parse further. Only depth-1 keys match.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset immediately after the object's closing brace.
/// @param key Direct property name to locate.
/// @param out_start Optional destination for the value's first byte.
/// @param out_end Optional destination for the offset immediately following the value.
/// @return 1 if found (range set), 0 otherwise.
int gltf_json_object_find_value(const char *json,
                                size_t len,
                                size_t obj_start,
                                size_t obj_end,
                                const char *key,
                                size_t *out_start,
                                size_t *out_end) {
    int depth = 0;
    size_t pos = obj_start;
    if (out_start)
        *out_start = SIZE_MAX;
    if (out_end)
        *out_end = SIZE_MAX;
    while (pos < obj_end && pos < len) {
        char c = json[pos];
        if (c == '"') {
            size_t next = gltf_json_skip_string_raw(json, len, pos);
            int at_key = 0;
            if (next == SIZE_MAX)
                return 0;
            if (depth == 1)
                at_key = gltf_json_key_matches(json, len, pos, key, NULL);
            if (at_key) {
                size_t colon = gltf_json_skip_ws(json, len, next);
                if (colon < obj_end && json[colon] == ':') {
                    size_t value = gltf_json_skip_ws(json, len, colon + 1u);
                    size_t end = gltf_json_skip_value(json, len, value);
                    if (end != SIZE_MAX && end <= obj_end) {
                        if (out_start)
                            *out_start = value;
                        if (out_end)
                            *out_end = end;
                        return 1;
                    }
                }
                return 0;
            }
            pos = next;
            continue;
        }
        if (c == '{')
            depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0)
                break;
        } else if (depth == 1 && c == ':') {
            pos = gltf_json_skip_value(json, len, pos + 1u);
            if (pos == SIZE_MAX)
                return 0;
            continue;
        }
        pos++;
    }
    return 0;
}

/// @brief Find the byte range of the @p item_index-th element of a JSON array.
/// @details Walks comma-separated values (skipping each whole) until it reaches the index.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset immediately after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @param out_start Optional destination for the element's first byte.
/// @param out_end Optional destination for the offset immediately following the element.
/// @return 1 with [out_start, out_end) set, or 0 if the index is out of range or input malformed.
int gltf_json_array_item_range(const char *json,
                               size_t len,
                               size_t array_start,
                               size_t array_end,
                               int item_index,
                               size_t *out_start,
                               size_t *out_end) {
    size_t pos;
    int index = 0;
    if (out_start)
        *out_start = SIZE_MAX;
    if (out_end)
        *out_end = SIZE_MAX;
    if (!json || item_index < 0 || array_start >= array_end || array_end > len ||
        json[array_start] != '[')
        return 0;
    {
        size_t validated_end = gltf_json_skip_value(json, len, array_start);
        if (validated_end == SIZE_MAX || validated_end != array_end)
            return 0;
    }
    pos = array_start + 1u;
    while (pos < array_end) {
        size_t value_start;
        size_t value_end;
        pos = gltf_json_skip_ws(json, len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        value_start = pos;
        value_end = gltf_json_skip_value(json, len, value_start);
        if (value_end == SIZE_MAX || value_end > array_end)
            return 0;
        if (index == item_index) {
            if (out_start)
                *out_start = value_start;
            if (out_end)
                *out_end = value_end;
            return 1;
        }
        index++;
        pos = gltf_json_skip_ws(json, len, value_end);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    return 0;
}

/// @brief Read the @p item_index-th array element as a C-locale double (@p fallback otherwise).
/// @details Rejects string/object/array elements and non-finite results, returning @p fallback.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset immediately after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @param fallback Value returned for absence, invalid syntax, or non-finite output.
/// @return Parsed finite number or @p fallback.
double gltf_json_array_get_number(const char *json,
                                  size_t len,
                                  size_t array_start,
                                  size_t array_end,
                                  int item_index,
                                  double fallback) {
    size_t value_start;
    size_t value_end;
    size_t text_len;
    char *text;
    double value;
    if (!gltf_json_array_item_range(
            json, len, array_start, array_end, item_index, &value_start, &value_end))
        return fallback;
    value_start = gltf_json_skip_ws(json, len, value_start);
    if (value_start >= value_end || json[value_start] == '"' || json[value_start] == '{' ||
        json[value_start] == '[')
        return fallback;
    text_len = value_end - value_start;
    if (text_len > SIZE_MAX - 1u)
        return fallback;
    text = (char *)malloc(text_len + 1u);
    if (!text)
        return fallback;
    memcpy(text, json + value_start, text_len);
    text[text_len] = '\0';
    if (rt_parse_double(text, &value) != (int32_t)Err_None || !isfinite(value))
        value = fallback;
    free(text);
    return value;
}

/// @brief Read the @p item_index-th array element as an unescaped string (caller frees; NULL if not
/// a string).
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param array_start Offset of the array's opening bracket.
/// @param array_end Offset immediately after the array's closing bracket.
/// @param item_index Zero-based element index.
/// @return Heap-allocated decoded string, or NULL when the element is absent, malformed, or not a
///         string.
char *gltf_json_array_get_string_alloc(
    const char *json, size_t len, size_t array_start, size_t array_end, int item_index) {
    size_t value_start;
    size_t value_end;
    if (!gltf_json_array_item_range(
            json, len, array_start, array_end, item_index, &value_start, &value_end))
        return NULL;
    value_start = gltf_json_skip_ws(json, len, value_start);
    if (value_start >= value_end || json[value_start] != '"')
        return NULL;
    return gltf_json_read_string_alloc(json, len, value_start, NULL);
}

/// @brief Read a property @p key as a boolean, accepting literal true/false or a nonzero number.
/// @param json Source JSON byte buffer.
/// @param len Exact source byte length.
/// @param obj_start Offset of the object's opening brace.
/// @param obj_end Offset immediately after the object's closing brace.
/// @param key Direct property name to locate.
/// @param fallback Value returned when the property is absent or invalid.
/// @return 1 for true/nonzero, 0 for false/zero, or @p fallback if the key is absent.
int gltf_json_object_get_boolish(
    const char *json, size_t len, size_t obj_start, size_t obj_end, const char *key, int fallback) {
    size_t value_start;
    size_t value_end;
    size_t value;
    if (!gltf_json_object_find_value(json, len, obj_start, obj_end, key, &value_start, &value_end))
        return fallback;
    value = gltf_json_skip_ws(json, len, value_start);
    if (value + 4u <= value_end && strncmp(json + value, "true", 4u) == 0 &&
        gltf_json_literal_ends_cleanly(json, value + 4u, value_end))
        return 1;
    if (value + 5u <= value_end && strncmp(json + value, "false", 5u) == 0 &&
        gltf_json_literal_ends_cleanly(json, value + 5u, value_end))
        return 0;
    return gltf_json_object_get_int(json, len, obj_start, obj_end, key, fallback) ? 1 : 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
