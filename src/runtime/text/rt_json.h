//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_json.h
// Purpose: Public C ABI for Zanna.Data.Json parsing, validation, compact and
//          pretty serialization, and runtime-value type classification.
//
// Key invariants:
//   - Parses all standard JSON types: null, boolean, number, string, array, object.
//   - Numbers are stored as f64; integer parsing preserves exact values up to 53 bits.
//   - JSON null is represented by a NULL value; invalid JSON is reported by trap
//     or by the status result from rt_json_try_parse.
//   - rt_json_format produces compact JSON; rt_json_format_pretty produces
//     indented output.
//   - Strings and raw non-ASCII input must be valid UTF-8; \u escapes, including
//     valid surrogate pairs, are decoded by the allocating parser.
//   - Parsing and formatting enforce a shared finite nesting-depth limit.
//
// Ownership/Lifetime:
//   - Returned value trees, diagnostic strings, and formatted/type strings carry
//     references owned by the caller; JSON null has no object reference.
//   - Input strings are borrowed for the duration of parsing.
//
// Links: src/runtime/text/rt_json_parse.c (allocating parser),
//        src/runtime/text/rt_json_validate.c (syntax validator),
//        src/runtime/text/rt_json_format.c (serializer and type classifier),
//        src/runtime/text/rt_json_internal.h (shared cursor and depth limit),
//        src/runtime/core/rt_string.h (runtime strings)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json.h
 * @brief Declares JSON parsing, validation, formatting, and type classification.
 * @details The public C ABI maps JSON documents to managed Maps, sequences,
 *          Strings, and boxed primitives; distinguishes status-returning parse
 *          failures from JSON null; validates without building a tree; and
 *          emits compact or pretty caller-owned JSON text.
 */

#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse a JSON string into a Zanna value.
/// @details Parses exactly one complete JSON value after allowing surrounding
///          JSON whitespace. Objects become Maps, arrays become element-owning
///          Seqs, strings remain runtime strings, numbers become boxed F64,
///          and Booleans become boxed I1. Duplicate object members replace
///          earlier values. Syntax, UTF-8, depth, and trailing-content errors
///          trap with a line/column diagnostic. A null input returns null
///          directly, while an empty string traps.
/// @param text Borrowed JSON text.
/// @return Parsed value: Map (object), Seq (array), String, boxed number/bool, or NULL for JSON
///         null; non-null returned values are owned by the caller.
/// @note Use `rt_json_try_parse` when JSON null must be distinguished from a
///       syntax failure without trapping.
void *rt_json_parse(rt_string text);

/// @brief Parse JSON without trapping on syntax errors.
/// @details All outputs are initialized before parsing. On success, message,
///          line, and column remain null/zero. If @p out_value is null, a
///          successfully parsed non-null tree is released internally. Syntax
///          failures are returned rather than trapped; allocation failures may
///          still use the runtime trap mechanism.
/// @param text Borrowed JSON text; null and empty inputs report `"empty input"`.
/// @param out_value Optional output receiving the caller-owned parsed value.
///                  JSON null is represented by a successful null output.
/// @param out_message Optional output receiving a caller-owned diagnostic
///                    string on failure.
/// @param out_line Optional output receiving the 1-based failure line, or zero
///                 when no location is available.
/// @param out_column Optional output receiving the 1-based failure column, or
///                   zero when no location is available.
/// @return `1` on success, including JSON null, or `0` on invalid input,
///         syntax error, trailing content, or excessive nesting.
int8_t rt_json_try_parse(rt_string text,
                         void **out_value,
                         rt_string *out_message,
                         int64_t *out_line,
                         int64_t *out_column);

/// @brief Parse JSON expecting an object at the root.
/// @details Requires a non-null, non-empty input whose first value is an
///          object and rejects trailing non-whitespace content.
/// @param text Borrowed JSON text.
/// @return Newly allocated Map owning the parsed property tree, or null after
///         a trapped error.
/// @note Traps on invalid input, syntax/depth errors, or a non-object root.
void *rt_json_parse_object(rt_string text);

/// @brief Parse JSON expecting an array at the root.
/// @details Requires a non-null, non-empty input whose first value is an array
///          and rejects trailing non-whitespace content.
/// @param text Borrowed JSON text.
/// @return Newly allocated element-owning Seq containing the parsed value tree,
///         or null after a trapped error.
/// @note Traps on invalid input, syntax/depth errors, or a non-array root.
void *rt_json_parse_array(rt_string text);

/// @brief Format a Zanna value as compact JSON.
/// @details Supports Map, Seq, runtime String, boxed I1, I64, F64, boxed String,
///          and null. Non-finite floating-point values and unknown runtime
///          objects serialize as `null`. Cycles and excessive depth trap.
/// @param obj Borrowed runtime value to format; null denotes JSON null.
/// @return Newly allocated single-line JSON string owned by the caller.
rt_string rt_json_format(void *obj);

/// @brief Format a Zanna value as pretty-printed JSON.
/// @details Uses the same type mapping and error behavior as `rt_json_format`.
///          Non-positive indentation delegates to compact formatting.
/// @param obj Borrowed runtime value to format; null denotes JSON null.
/// @param indent Number of literal spaces per nesting level.
/// @return Newly allocated JSON string with indentation and newlines, owned by
///         the caller.
rt_string rt_json_format_pretty(void *obj, int64_t indent);

/// @brief Check if a string contains valid JSON.
/// @details Validates exactly one complete value, UTF-8 and escape correctness,
///          number grammar, trailing content, and the nesting-depth limit
///          without constructing a value tree.
/// @param text Borrowed JSON text; null and empty strings are invalid.
/// @return `1` for valid JSON, otherwise `0`.
int8_t rt_json_is_valid(rt_string text);

/// @brief Get the JSON type of a parsed value.
/// @details Classifies null, runtime/boxed strings, Seq, Map, boxed I1, and
///          boxed I64/F64. Other runtime objects are reported as `"unknown"`.
/// @param obj Borrowed parsed or format-compatible runtime value.
/// @return Newly allocated string containing `"null"`, `"boolean"`,
///         `"number"`, `"string"`, `"array"`, `"object"`, or `"unknown"`.
rt_string rt_json_type_of(void *obj);

#ifdef __cplusplus
}
#endif
