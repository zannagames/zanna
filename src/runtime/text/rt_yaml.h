//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_yaml.h
// Purpose: YAML 1.2 subset parser and formatter for Zanna.Data.Yaml.
//          Supports scalars (string/int/float/bool/null), block and flow
//          sequences and mappings, and multi-document streams.
// Key invariants:
//   - Parses the YAML 1.2 core schema; unrecognised constructs produce an error.
//   - rt_yaml_parse returns NULL both for YAML null and for invalid syntax;
//     invalid syntax records an error retrievable via rt_yaml_error().
//   - Multi-document streams (separated by '---') return a Seq of documents.
//   - Formatter always emits block-style collections and quotes ambiguous
//     scalars so output is guaranteed to round-trip through the parser.
// Ownership/Lifetime:
//   - All returned objects are newly heap-allocated; caller must release.
//   - Input rt_string values are borrowed for the duration of the call.
// Links: src/runtime/text/rt_yaml.c (implementation),
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for parsing, formatting, and inspecting the runtime's
///        practical YAML 1.2 value model.
/// @details YAML null maps to a null object pointer; other scalars map to
///          strings or boxed primitives, while collections map to owning
///          sequences and string-keyed maps. Parse failures are distinguished
///          from valid null values through @ref rt_yaml_error or
///          @ref rt_yaml_parse_result.
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// YAML Parsing
//=========================================================================

/// @brief Parse UTF-8 YAML source into runtime values.
/// @details Supports plain and quoted scalars, simple literal/folded blocks,
///          block and flow collections, comments, and explicitly separated
///          document streams. Anchors, aliases, tags, and general YAML object
///          construction are outside this subset.
/// @param text YAML source text.
/// @return Owned map, sequence, string, or boxed primitive; null represents
///         YAML null, an empty document, or a parse failure.
/// @note A failure records a thread-local diagnostic; valid null and empty
///       documents leave @ref rt_yaml_error empty.
void *rt_yaml_parse(rt_string text);

/// @brief Parse YAML string into a Zanna.Result.
/// @details Returns `Ok(value)` for valid YAML, including YAML null and empty
///          documents as `Ok(NULL)`, and `Err(message)` for invalid YAML. This
///          avoids the ambiguity of rt_yaml_parse() returning NULL for both
///          valid null values and failures.
/// @param text UTF-8 YAML source text.
/// @return Owned opaque `Zanna.Result` containing the value or diagnostic.
void *rt_yaml_parse_result(rt_string text);

/// @brief Copy the current thread's most recent YAML parse diagnostic.
/// @return Owned error-message string, or an owned empty string if none exists.
rt_string rt_yaml_error(void);

/// @brief Test whether text is accepted by the runtime YAML subset.
/// @details Empty input and YAML null are valid even though their parsed value
///          is a null pointer. The temporary parsed object is released.
/// @param text Candidate UTF-8 YAML source.
/// @return 1 when accepted, otherwise 0.
int8_t rt_yaml_is_valid(rt_string text);

//=========================================================================
// YAML Formatting
//=========================================================================

/// @brief Serialize a supported runtime value as block-style YAML.
/// @details Uses a two-space indentation step, quotes ambiguous strings for
///          round trips, emits unknown object kinds as `null`, and limits
///          collection depth to 200.
/// @param obj Map, sequence, string, boxed scalar, or null value.
/// @return Owned YAML string; an empty string reports an exceeded depth limit.
rt_string rt_yaml_format(void *obj);

/// @brief Serialize a supported value with a custom indentation width.
/// @details Widths below one use the two-space default; widths above eight are
///          clamped to eight. Collections are emitted in block style and empty
///          collections use `[]` or `{}`.
/// @param obj Map, sequence, string, boxed scalar, or null value.
/// @param indent Requested spaces per collection nesting level.
/// @return Owned YAML string; an empty string reports an exceeded depth limit.
rt_string rt_yaml_format_indent(void *obj, int64_t indent);

//=========================================================================
// Type Inspection
//=========================================================================

/// @brief Describe a runtime value using its YAML data-model category.
/// @param obj Parsed or otherwise compatible runtime value.
/// @return Owned string equal to `null`, `bool`, `int`, `float`, `string`,
///         `sequence`, `mapping`, or `unknown`.
rt_string rt_yaml_type_of(void *obj);

#ifdef __cplusplus
}
#endif
