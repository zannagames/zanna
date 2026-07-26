//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_toml.h
// Purpose: Declares a TOML-subset parser, structural validator, formatter, and
//          dotted-path lookup helpers for Zanna.Data.Toml.
//
// Key invariants:
//   - Parses key-value pairs, tables, arrays of tables, arrays, inline tables,
//     dotted bare keys, and basic/literal single- or multiline strings.
//   - Returns a Map where section names are keys and values are Maps.
//   - Top-level key-value pairs appear in the root Map.
//   - Returns NULL on invalid TOML syntax.
//   - This is not a conforming TOML 1.0 parser: scalar spellings are stored as
//     strings and arbitrary unquoted scalar text is accepted.
//   - Source text must be NUL-free valid UTF-8.
//
// Ownership/Lifetime:
//   - Returned Map objects are newly allocated; caller must release.
//   - Input strings are borrowed for the duration of parsing.
//
// Links: src/runtime/text/rt_toml.c (implementation)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_toml.h
 * @brief Declares parsing, validation, formatting, and lookup for a TOML subset.
 * @details Borrowed UTF-8 source text maps tables and inline tables to Maps,
 *          arrays to sequences, and all scalar spellings to Strings. The API
 *          returns caller-owned parsed or formatted values and supports dotted
 *          lookup over parsed or automatically parsed roots.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse a TOML string into a Map of Maps.
/// @details Tables become nested Maps, arrays become Seqs, and all scalar
///          values become runtime strings. Duplicate keys and malformed
///          structure fail without returning a partial root.
/// @param src Borrowed NUL-free UTF-8 TOML-subset text.
/// @return Caller-owned root Map, or null on invalid input or allocation failure.
void *rt_toml_parse(rt_string src);

/// @brief Check if a string is valid TOML.
/// @details Validity means accepted by this structural subset, not full TOML
///          v1.0 conformance.
/// @param src Borrowed TOML-subset text.
/// @return `1` if a complete parse succeeds, otherwise `0`.
int8_t rt_toml_is_valid(rt_string src);

/// @brief Format a Map as a TOML string.
/// @details Supports string and boxed scalar values, arrays, inline Map values,
///          and nested Maps emitted as dotted section headers. Formatting is
///          bounded to the implementation's maximum nesting depth.
/// @param map Borrowed root Map.
/// @return Caller-owned TOML-subset text, or an empty string on failure.
rt_string rt_toml_format(void *map);

/// @brief Get a value from parsed TOML using dotted key path.
/// @details Raw or boxed TOML strings are parsed automatically before lookup.
/// @param root Borrowed root Map, raw TOML string, or boxed TOML string.
/// @param key_path Borrowed dotted byte path such as `"server.host"`.
/// @return Borrowed value at the path, or null if parsing or lookup fails.
void *rt_toml_get(void *root, rt_string key_path);

/// @brief Get a string value from TOML using dotted key path.
/// @param root Borrowed root Map, raw TOML string, or boxed TOML string.
/// @param key_path Borrowed dotted byte path.
/// @return Retained string value, or a new empty string when missing or non-string.
rt_string rt_toml_get_str(void *root, rt_string key_path);

#ifdef __cplusplus
}
#endif
