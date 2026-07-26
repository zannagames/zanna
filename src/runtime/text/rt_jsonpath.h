//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_jsonpath.h
// Purpose: JSONPath-like navigation and scalar conversion over parsed JSON
//          trees or raw JSON source strings.
//
// Key invariants:
//   - Supports dotted access (obj.key), bracket notation (obj['key']), and array indexing (arr[0]).
//   - An optional leading '$' selects the root; empty paths also select it.
//   - Direct/boxed string roots are auto-parsed as JSON source text.
//   - Get returns NULL for both a missing path and stored JSON null; Has and
//     GetOr preserve the distinction through container membership.
//   - Array indices may be negative (counting from the end).
//   - Query supports only the first '*' wildcard; recursive descent, filters,
//     slices, unions, and quoted-key escapes are not implemented.
//   - Numeric-looking segments are interpreted as Seq indices, even when
//     written in quoted bracket notation.
//
// Ownership/Lifetime:
//   - Returned non-null values are retained for the caller; conversion strings
//     and result Seqs are newly allocated.
//   - The JSON tree is borrowed; it must remain valid during the query.
//
// Links: src/runtime/text/rt_jsonpath.c (implementation),
//        src/runtime/text/rt_json.h (raw-source parser),
//        src/runtime/core/rt_string.h (paths and conversions)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_jsonpath.h
 * @brief Declares JSONPath-like lookup, wildcard query, and scalar conversion.
 * @details Paths navigate borrowed parsed trees or raw JSON source Strings by
 *          member, bracket key, and sequence index. The API distinguishes
 *          membership from JSON null, supports defaults and one wildcard
 *          expansion, and returns retained values or caller-owned conversions.
 */

#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Query a JSON object using a path expression.
/// @details Accepts parsed trees or raw/boxed JSON source strings. Empty and
///          `$` paths select the root. Stored JSON null and missing paths both
///          return null through this convenience API.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed expression such as `"user.name"` or `"items[0].id"`.
/// @return Retained caller-owned terminal value, or null.
void *rt_jsonpath_get(void *root, rt_string path);

/// @brief Query a JSON object, returning a default if not found.
/// @details A present JSON-null member returns null rather than the default.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @param def Borrowed default runtime value.
/// @return Retained caller-owned path value or default, or null.
void *rt_jsonpath_get_or(void *root, rt_string path, void *def);

/// @brief Check if a path exists in the JSON object.
/// @details Returns true for a terminal member whose stored value is JSON null.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @return `1` when every segment exists, otherwise `0`.
int8_t rt_jsonpath_has(void *root, rt_string path);

/// @brief Get all values matching a wildcard path.
/// @details Uses only the first `*`; it expands direct Seq elements or Map
///          values and optionally resolves the remaining suffix. Without a
///          wildcard, returns at most one non-null match.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path such as `"users.*.name"`.
/// @return Newly allocated element-owning Seq of retained matches.
void *rt_jsonpath_query(void *root, rt_string path);

/// @brief Get string value at path, or empty string if not found.
/// @details Converts strings, boxed integers/floats, and Booleans to text.
///          Containers and JSON null are not convertible.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @return Caller-owned converted string, or an owned empty failure sentinel.
rt_string rt_jsonpath_get_str(void *root, rt_string path);

/// @brief Get integer value at path, or 0 if not found.
/// @details Converts boxed integer, float, Boolean, and parseable string
///          scalars. Float conversion uses defined saturation.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @return Converted integer, or zero as the absence/conversion-failure sentinel.
int64_t rt_jsonpath_get_int(void *root, rt_string path);

/// @brief Resolve @p path and convert it to an integer, reporting success.
/// @details Returns 1 and writes @p out when the value exists and is convertible
/// (number, boolean, or numeric string); returns 0 for an absent path or a value
/// of an inconvertible type (object/array/null, or non-numeric string), leaving
/// @p out untouched. Lets callers distinguish a genuine 0 from a failure and
/// supply their own default (VDOC-245). Internal helper.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @param out Optional output written only on successful conversion.
/// @return `1` on a convertible value, otherwise `0`.
int8_t rt_jsonpath_try_get_int(void *root, rt_string path, int64_t *out);

/// @brief Resolve @p path and convert it to a string, reporting success.
/// @details Returns 1 and writes a fresh caller-owned @p out when the value exists
/// and is a string or scalar (number/boolean coerced to text); returns 0 for an
/// absent path or an inconvertible type (object/array/null), leaving @p out
/// untouched (VDOC-245). Internal helper.
/// @param root Borrowed parsed value tree or JSON source string.
/// @param path Borrowed path expression.
/// @param out Non-null output receiving a caller-owned string on success.
/// @return `1` on a convertible value, otherwise `0`.
int8_t rt_jsonpath_try_get_str(void *root, rt_string path, rt_string *out);

#ifdef __cplusplus
}
#endif
