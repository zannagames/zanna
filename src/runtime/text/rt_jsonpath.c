//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_jsonpath.c
// Purpose: Implements a JSONPath-like path resolver for the Zanna.Data.JsonPath
//          class. Evaluates dot-separated paths against a parsed rt_map/rt_seq
//          JSON tree (from rt_json_parse.c). The supported syntax is: dot segments
//          (`a.b`), bracket indices (`[0]`, negative from the end) and quoted
//          bracket keys (`["k"]`/`['k']`), an optional leading `$`, and — in
//          `Query` only — a single wildcard `*` segment. Recursive descent
//          (`..`), slices, filters, and unions are NOT implemented.
//
// Key invariants:
//   - Inputs may be pre-parsed trees or raw/boxed JSON source strings, which
//     are auto-parsed through rt_json_parse.
//   - Query returns a Seq of all matched nodes; empty Seq if no match.
//   - '$' is the root selector; '.' separates child segments.
//   - Array indices in brackets are zero-based; negative indices from the end.
//   - In Query, the single wildcard '*' matches all children of the current node.
//   - Path resolution emits no dedicated syntax diagnostics; allocation and
//     raw-JSON parse failures may still trap.
//
// Ownership/Lifetime:
//   - The input JSON tree is borrowed unless a raw JSON string root is auto-parsed.
//   - Returned values and query result elements are retained for the caller.
//   - The returned Seq itself is a fresh allocation owned by the caller.
//
// Links: src/runtime/text/rt_jsonpath.h (public API),
//        src/runtime/text/rt_json.h (JSON parser producing the input tree)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_jsonpath.c
 * @brief Implements JSONPath-like navigation over managed JSON values.
 * @details The resolver accepts parsed trees or auto-parsed String roots and
 *          handles optional root markers, dotted members, quoted bracket keys,
 *          positive or negative sequence indices, and a single Query wildcard.
 *          Lookup, membership, defaulting, scalar conversion, and query results
 *          retain values for their callers.
 */

#include "rt_jsonpath.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_format.h"
#include "rt_numeric.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Minimum payload size required before treating an object as a Seq.
#define JSONPATH_SEQ_MIN_PAYLOAD (sizeof(int64_t) * 2 + sizeof(void *) + sizeof(int8_t))
/// Minimum payload size required before treating an object as a Map.
#define JSONPATH_MAP_MIN_PAYLOAD (sizeof(void *) * 2 + sizeof(size_t) * 2)

/// @brief Test whether a runtime pointer is a sufficiently large Seq object.
/// @param obj Candidate borrowed runtime object.
/// @return Non-zero only for a valid Seq instance.
static int is_seq_obj(void *obj) {
    return obj && !rt_string_is_handle(obj) &&
           rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, JSONPATH_SEQ_MIN_PAYLOAD);
}

/// @brief Test whether a runtime pointer is a sufficiently large Map object.
/// @param obj Candidate borrowed runtime object.
/// @return Non-zero only for a valid Map instance.
static int is_map_obj(void *obj) {
    return obj && !rt_string_is_handle(obj) &&
           rt_obj_is_instance(obj, RT_MAP_CLASS_ID, JSONPATH_MAP_MIN_PAYLOAD);
}

/// @brief Release one locally owned runtime-object reference.
/// @details Frees the object when dropping the reference reaches zero. Null
///          pointers, including JSON null, are ignored.
/// @param obj Runtime object reference owned by the current scope.
static void release_local_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Parse an exact signed 64-bit integer from a non-NUL-terminated span.
/// @details JSONPath array indices may be long or negative. This helper consumes
///          every byte in the span, rejects non-digits, and checks overflow so
///          an overlong selector cannot be truncated into a different index.
/// @param text Span start.
/// @param len Span length in bytes.
/// @param out_index Receives the parsed integer on success.
/// @return 1 on success; 0 if the span is not exactly a valid i64 literal.
static int jsonpath_parse_i64_span(const char *text, int64_t len, int64_t *out_index) {
    if (out_index)
        *out_index = 0;
    if (!text || !out_index || len <= 0)
        return 0;

    int negative = text[0] == '-';
    int64_t pos = negative ? 1 : 0;
    if (pos >= len)
        return 0;

    uint64_t limit = negative ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    uint64_t value = 0;
    for (; pos < len; pos++) {
        unsigned char c = (unsigned char)text[pos];
        if (c < '0' || c > '9')
            return 0;
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (limit - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }

    if (negative) {
        *out_index = value == limit ? INT64_MIN : -(int64_t)value;
    } else {
        *out_index = (int64_t)value;
    }
    return 1;
}

// --- Helper: navigate one segment ---

/// @brief Navigate one path segment, reporting whether the container held it.
/// @details `*found` distinguishes a stored JSON null (C NULL value, found)
///          from an absent key/index (not found) — the value tree uses C NULL
///          for both, so the container must be consulted directly. Segments
///          beginning with a digit or `-` are always interpreted as Seq
///          indices; other segments are case-sensitive Map keys.
/// @param current Borrowed current container.
/// @param seg Segment byte range.
/// @param len Number of bytes in @p seg.
/// @param found Non-null output receiving container-level existence.
/// @return Borrowed child value, including null for stored JSON null or absence.
static void *navigate_segment_ex(void *current, const char *seg, int64_t len, int *found) {
    *found = 0;
    if (!current || len == 0)
        return NULL;

    // Array index: numeric segment
    if (isdigit((unsigned char)seg[0]) || (seg[0] == '-' && len > 1)) {
        int64_t idx = 0;
        if (!jsonpath_parse_i64_span(seg, len, &idx))
            return NULL;

        if (is_seq_obj(current)) {
            int64_t slen = rt_seq_len(current);
            if (idx < 0)
                idx += slen;
            if (idx >= 0 && idx < slen) {
                *found = 1;
                return rt_seq_get(current, idx);
            }
        }
        return NULL;
    }

    // Map key lookup
    if (!is_map_obj(current))
        return NULL;
    rt_string key = rt_string_from_bytes(seg, len);
    void *val = NULL;
    if (rt_map_has(current, key)) {
        *found = 1;
        val = rt_map_get(current, key);
    }
    rt_string_unref(key);
    return val;
}


// --- Helper: resolve path ---

/// @brief Resolve a path while reporting whether the terminal segment exists.
/// @details `*out_found` is 1 when every segment was present in its container —
///          including a terminal segment whose stored value is JSON null — and 0
///          when any segment was absent. This lets `Has`/`GetOr` distinguish a
///          present null member from a missing path. Empty paths and `$`
///          select the root. Dots are optional separators; bracket quotes are
///          stripped but do not implement escapes.
/// @param root Borrowed parsed JSON root.
/// @param path NUL-terminated path expression.
/// @param out_found Non-null output receiving terminal existence.
/// @return Borrowed resolved value, or null for stored JSON null/absence.
static void *resolve_path_ex(void *root, const char *path, int *out_found) {
    *out_found = 0;
    if (!root)
        return NULL;
    if (!path || !*path) {
        *out_found = 1;
        return root;
    }

    void *current = root;
    const char *p = path;
    int found = 1; // The root itself exists.

    // Skip leading '$.' if present
    if (p[0] == '$' && p[1] == '.')
        p += 2;
    else if (p[0] == '$')
        p += 1;

    while (*p && current) {
        // Skip dots
        if (*p == '.') {
            p++;
            continue;
        }

        // Bracket notation: [index] or ["key"]
        if (*p == '[') {
            p++;
            if (*p == '"' || *p == '\'') {
                char quote = *p;
                p++;
                const char *start = p;
                while (*p && *p != quote)
                    p++;
                current = navigate_segment_ex(current, start, (int64_t)(p - start), &found);
                if (*p == quote)
                    p++;
                if (*p == ']')
                    p++;
            } else {
                const char *start = p;
                while (*p && *p != ']')
                    p++;
                current = navigate_segment_ex(current, start, (int64_t)(p - start), &found);
                if (*p == ']')
                    p++;
            }
            continue;
        }

        // Dot notation: key
        const char *start = p;
        while (*p && *p != '.' && *p != '[')
            p++;
        current = navigate_segment_ex(current, start, (int64_t)(p - start), &found);
    }

    // Remaining path segments after a null/leaf value mean the path is missing
    // (a JSON null has no children). Skip trailing dots before deciding.
    while (*p == '.')
        p++;
    if (*p && !current)
        found = 0;

    *out_found = found;
    return current;
}

/// @brief Resolve a path without preserving null-versus-missing existence.
/// @param root Borrowed parsed JSON root.
/// @param path NUL-terminated path expression.
/// @return Borrowed terminal value, or null for absence/JSON null.
static void *resolve_path(void *root, const char *path) {
    int found = 0;
    return resolve_path_ex(root, path, &found);
}

// --- Helper: wildcard query ---

/// @brief Walk a wildcard `*` step: collect every child of `current`, then continue with
/// `remaining`.
/// @details Two cases:
///          1. **Sequence** (`rt_seq_len > 0`) → for each element,
///             either push it into results (if no remaining path) or
///             resolve `remaining` against it and push if found.
///          2. **Map** → enumerate the keys, then iterate values
///             with the same push-or-resolve logic.
///          Container types are checked explicitly before using their public
///          APIs. Resolved terminal JSON-null values are omitted when a
///          remaining path exists because the convenience resolver cannot
///          distinguish them from absence.
/// @param current Borrowed wildcard parent container.
/// @param remaining NUL-terminated suffix after `*`, or empty for direct children.
/// @param results Borrowed mutable result Seq that owns pushed elements.
static void collect_wildcard(void *current, const char *remaining, void *results) {
    if (!current)
        return;

    if (is_seq_obj(current)) {
        int64_t slen = rt_seq_len(current);
        for (int64_t i = 0; i < slen; i++) {
            void *val = rt_seq_get(current, i);
            if (!remaining || !*remaining) {
                rt_seq_push(results, val);
            } else {
                void *sub = resolve_path(val, remaining);
                if (sub)
                    rt_seq_push(results, sub);
            }
        }
        return;
    }

    // Try as map - iterate all values
    if (!is_map_obj(current))
        return;
    void *keys = rt_map_keys(current);
    if (keys) {
        int64_t n = rt_seq_len(keys);
        for (int64_t i = 0; i < n; i++) {
            void *val = rt_map_get(current, (rt_string)rt_seq_get(keys, i));
            if (!remaining || !*remaining) {
                rt_seq_push(results, val);
            } else {
                void *sub = resolve_path(val, remaining);
                if (sub)
                    rt_seq_push(results, sub);
            }
        }
        release_local_obj(keys);
    }
}

// --- Public API ---

/// @brief Auto-detect if root is a raw JSON string and parse it.
/// @details Direct and boxed runtime strings are interpreted as complete JSON
///          source text and passed to `rt_json_parse`; every other runtime
///          value is returned unchanged. Parsed JSON null and parse failure
///          both return null.
/// @param root Borrowed candidate tree or JSON source value.
/// @param owned Optional output set to one when a non-null parsed tree must be
///              released by the caller.
/// @return Borrowed original tree, newly allocated parsed tree, or null.
static void *auto_parse_root(void *root, int *owned) {
    if (owned)
        *owned = 0;
    if (!root)
        return NULL;
    if (rt_string_is_handle(root)) {
        // root is a raw string — try to parse it as JSON
        void *parsed = rt_json_parse((rt_string)root);
        if (parsed && owned)
            *owned = 1;
        return parsed;
    }
    if (rt_box_type(root) == RT_BOX_STR) {
        // root is a boxed string — unbox and parse
        rt_string s = rt_unbox_str(root);
        if (s) {
            void *parsed = rt_json_parse(s);
            rt_string_unref(s);
            if (parsed && owned)
                *owned = 1;
            return parsed;
        }
    }
    return root;
}

/// @brief Retain a resolved runtime value for transfer to an API caller.
/// @param value Borrowed resolved object, or null for JSON null.
/// @return The same pointer with one added reference when non-null.
static void *retain_jsonpath_value(void *value) {
    if (value)
        rt_obj_retain_maybe(value);
    return value;
}

/// @brief Convert a runtime path String to a C string, rejecting embedded NULs.
/// @details Paths are text: the resolver walks C strings, so a path whose runtime
///          byte length extends past a NUL would silently address a different
///          (shorter) path. Such paths match nothing instead (VDOC-038).
/// @param path Borrowed runtime path string.
/// @return NUL-terminated path text, or NULL when the path contains a NUL byte.
static const char *jsonpath_path_cstr(rt_string path) {
    if (!path)
        return NULL;
    const char *p = rt_string_cstr(path);
    int64_t len = rt_str_len(path);
    if (!p || len < 0 || memchr(p, '\0', (size_t)len) != NULL)
        return NULL;
    return p;
}

/// @brief Navigate a parsed JSON tree by a JSONPath-like expression.
/// @details The root may instead be direct or boxed JSON source text, which is
///          parsed temporarily. Empty paths and `$` return the root. Stored
///          JSON null and missing paths both return null through this API.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string without embedded NUL bytes.
/// @return Retained caller-owned terminal value, or null for missing/JSON null.
void *rt_jsonpath_get(void *root, rt_string path) {
    if (!root || !path)
        return NULL;
    int owned_root = 0;
    root = auto_parse_root(root, &owned_root);
    if (!root)
        return NULL;
    const char *path_text = jsonpath_path_cstr(path);
    if (!path_text) {
        if (owned_root)
            release_local_obj(root);
        return NULL;
    }
    void *result = retain_jsonpath_value(resolve_path(root, path_text));
    if (owned_root)
        release_local_obj(root);
    return result;
}

/// @brief Resolve a path with existence tracking, sharing the auto-parse root logic.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @param out_found Receives 1 when the terminal segment exists (even as JSON null).
/// @return Retained value at the path, or NULL when missing or stored null.
static void *jsonpath_get_ex(void *root, rt_string path, int *out_found) {
    *out_found = 0;
    if (!root || !path)
        return NULL;
    int owned_root = 0;
    root = auto_parse_root(root, &owned_root);
    if (!root)
        return NULL;
    const char *path_text = jsonpath_path_cstr(path);
    if (!path_text) {
        if (owned_root)
            release_local_obj(root);
        return NULL;
    }
    void *result = retain_jsonpath_value(resolve_path_ex(root, path_text, out_found));
    if (owned_root)
        release_local_obj(root);
    return result;
}

/// @brief Resolve a path and substitute a default only when it is missing.
/// @details A present JSON-null member returns null and does not select the
///          default. Whichever non-null value is returned is retained for the
///          caller.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @param def Borrowed default runtime value.
/// @return Caller-owned retained resolved/default value, or null.
void *rt_jsonpath_get_or(void *root, rt_string path, void *def) {
    // The default applies only when the path is MISSING: a present JSON null
    // member is a real value and is returned as NULL rather than replaced.
    int found = 0;
    void *result = jsonpath_get_ex(root, path, &found);
    if (found)
        return result;
    return retain_jsonpath_value(def);
}

/// @brief Check whether a path exists in a parsed JSON tree.
/// @details Container membership is tracked separately from the value, so a
///          present JSON-null terminal reports true.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @return `1` when every segment exists, otherwise `0`.
int8_t rt_jsonpath_has(void *root, rt_string path) {
    // Existence is judged by the containers along the path, so a present JSON
    // null member reports true while a missing key/index reports false.
    int found = 0;
    void *result = jsonpath_get_ex(root, path, &found);
    if (result)
        release_local_obj(result);
    return found ? 1 : 0;
}

/// @brief Query a JSON tree with wildcard path support (returns a sequence of matching values).
/// @details Supports the first `*` found in the path. Without a wildcard, at
///          most one non-null result is appended. With one, every direct child
///          of the resolved parent is optionally followed through the suffix.
///          Result ordering follows Seq order or implementation-defined Map-key
///          snapshot order.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string without embedded NUL bytes.
/// @return Newly allocated element-owning Seq of retained matches.
void *rt_jsonpath_query(void *root, rt_string path) {
    void *results = rt_seq_new();
    rt_seq_set_owns_elements(results, 1);
    if (!root || !path)
        return results;

    int owned_root = 0;
    root = auto_parse_root(root, &owned_root);
    if (!root)
        return results;

    const char *p = jsonpath_path_cstr(path);
    if (!p) {
        if (owned_root)
            release_local_obj(root);
        return results;
    }

    // Skip leading '$.'
    if (p[0] == '$' && p[1] == '.')
        p += 2;
    else if (p[0] == '$')
        p += 1;

    // Find wildcard '*'
    const char *star = strchr(p, '*');
    if (!star) {
        // No wildcard - just get single result
        void *val = resolve_path(root, p);
        if (val)
            rt_seq_push(results, val);
        if (owned_root)
            release_local_obj(root);
        return results;
    }

    // Navigate to the parent of the wildcard
    void *parent = root;
    if (star > p) {
        // Extract path before wildcard
        int64_t pre_len = (int64_t)(star - p);
        if (pre_len > 0 && p[pre_len - 1] == '.')
            pre_len--;
        char *pre = (char *)malloc((size_t)(pre_len + 1));
        if (!pre) {
            rt_trap("rt_jsonpath: memory allocation failed");
            if (owned_root)
                release_local_obj(root);
            return results;
        }
        memcpy(pre, p, (size_t)pre_len);
        pre[pre_len] = '\0';
        parent = resolve_path(root, pre);
        free(pre);
    }

    // Get remaining path after wildcard
    const char *remaining = star + 1;
    if (*remaining == '.')
        remaining++;

    collect_wildcard(parent, remaining, results);
    if (owned_root)
        release_local_obj(root);
    return results;
}

/// @brief Resolve and convert a scalar path value to a runtime string.
/// @details Direct/boxed strings are copied or retained; boxed I64, finite or
///          non-finite F64, and I1 values are formatted as text. Containers,
///          JSON null, and missing paths are not convertible. @p out is
///          required for a successful result and is left untouched on failure.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @param out Non-null output receiving a caller-owned string on success.
/// @return `1` when a scalar conversion was stored, otherwise `0`.
int8_t rt_jsonpath_try_get_str(void *root, rt_string path, rt_string *out) {
    void *val = rt_jsonpath_get(root, path);
    if (!val)
        return 0;
    rt_string result = NULL;
    // If it's already a string, return it directly
    if (rt_string_is_handle(val))
        result = rt_string_ref((rt_string)val);
    // If it's a boxed value, try to extract string or convert
    int64_t tag = result ? -1 : rt_box_type(val);
    if (tag == RT_BOX_STR) {
        result = rt_unbox_str(val);
    } else if (tag == RT_BOX_I64) {
        int64_t n = rt_unbox_i64(val);
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)n);
        result = rt_string_from_bytes(buf, strlen(buf));
    } else if (tag == RT_BOX_F64) {
        double d = rt_unbox_f64(val);
        char buf[64];
        // Locale-independent exact formatting (VDOC-041).
        rt_format_f64_roundtrip(d, buf, sizeof(buf));
        result = rt_string_from_bytes(buf, strlen(buf));
    } else if (tag == RT_BOX_I1) {
        int64_t b = rt_unbox_i1(val);
        result = b ? rt_string_from_bytes("true", 4) : rt_string_from_bytes("false", 5);
    }
    // Otherwise (object/array/null) the value is not string-convertible.
    release_local_obj(val);
    if (result && out) {
        *out = result;
        return 1;
    }
    if (result)
        rt_string_unref(result);
    return 0;
}

/// @brief Resolve and stringify a scalar, using empty text as failure sentinel.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @return Caller-owned converted string, or an owned empty string when absent
///         or not convertible.
rt_string rt_jsonpath_get_str(void *root, rt_string path) {
    rt_string result = NULL;
    if (rt_jsonpath_try_get_str(root, path, &result))
        return result;
    return rt_string_from_bytes("", 0); // empty on absence or non-convertible value
}

/// @brief Resolve and convert a scalar path value to `int64_t`.
/// @details I64 is preserved, F64 uses the runtime's defined saturating
///          conversion, I1 becomes zero/one, and direct or boxed strings use
///          exact integer parsing. Containers, JSON null, missing paths, and
///          non-numeric strings fail. @p out is written only on success.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @param out Optional output receiving the converted value.
/// @return `1` for a convertible scalar, otherwise `0`.
int8_t rt_jsonpath_try_get_int(void *root, rt_string path, int64_t *out) {
    void *val = rt_jsonpath_get(root, path);
    if (!val)
        return 0;
    int8_t ok = 0;
    int64_t result = 0;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_I64) {
        result = rt_unbox_i64(val);
        ok = 1;
    } else if (tag == RT_BOX_F64) {
        // Defined saturating conversion; raw casts are UB out of range (VDOC-037).
        result = (int64_t)rt_f64_to_i64(rt_unbox_f64(val));
        ok = 1;
    } else if (tag == RT_BOX_I1) {
        result = rt_unbox_i1(val);
        ok = 1;
    } else if (tag == RT_BOX_STR) {
        rt_string s = rt_unbox_str(val);
        ok = (rt_parse_int64_str(s, &result) == (int32_t)Err_None) ? 1 : 0;
        rt_string_unref(s);
    } else if (rt_string_is_handle(val)) {
        // Raw string handle: parseable numeric text converts, other text does not.
        ok = (rt_parse_int64_str((rt_string)val, &result) == (int32_t)Err_None) ? 1 : 0;
    }
    // Otherwise (object/array/null) the value is not int-convertible: ok stays 0.
    release_local_obj(val);
    if (ok && out)
        *out = result;
    return ok;
}

/// @brief Resolve and convert an integer, using zero as the legacy failure sentinel.
/// @param root Borrowed parsed tree or raw/boxed JSON source string.
/// @param path Borrowed path string.
/// @return Converted integer, or zero when absent or not convertible.
int64_t rt_jsonpath_get_int(void *root, rt_string path) {
    int64_t result = 0;
    rt_jsonpath_try_get_int(root, path, &result);
    return result; // 0 on absence or non-convertible value (legacy sentinel)
}
