//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_serialize.h
// Purpose: Unified serialization façade for Zanna.Data.Serialize. Provides
//          format-agnostic parse/format/convert/detect operations over JSON,
//          XML, YAML, TOML, and CSV via a single rt_format_t enum or
//          heuristic auto-detection.
// Key invariants:
//   - Format is selected by rt_format_t enum value or auto-detected from content.
//   - All format operations produce a string; parse operations produce a value.
//   - Parse failures return NULL and record a message via rt_serialize_error().
//   - Ordinary invalid input and unknown formats propagate through NULL/empty
//     returns and thread-local diagnostics; allocation/backend failures may trap.
// Ownership/Lifetime:
//   - Returned values, Results, and strings are caller-owned unless their
//     underlying runtime type documents an immutable sentinel.
//   - Input rt_string arguments are borrowed for the duration of each call.
// Links: src/runtime/text/rt_serialize.c (implementation),
//        src/runtime/text/rt_json.h, rt_xml.h, rt_yaml.h, rt_toml.h, rt_csv.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_serialize.h
 * @brief Declares format-agnostic parsing, formatting, conversion, and detection.
 * @details A shared format enumeration selects JSON, XML, YAML, TOML, or CSV
 *          backends. Operations borrow input values, return caller-owned
 *          managed output or Results, and expose thread-local diagnostics for
 *          ordinary invalid input and unsupported formats.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Format enumeration
//=========================================================================

/// @brief Supported serialization formats.
typedef enum {
    RT_FORMAT_JSON = 0, ///< JSON (RFC 8259)
    RT_FORMAT_XML = 1,  ///< XML (subset)
    RT_FORMAT_YAML = 2, ///< YAML (1.2 subset)
    RT_FORMAT_TOML = 3, ///< TOML (v1.0)
    RT_FORMAT_CSV = 4   ///< CSV (RFC 4180)
} rt_format_t;

//=========================================================================
// Unified Parse / Format
//=========================================================================

/// @brief Parse text in the specified format into a Zanna value.
/// @details Dispatches to JSON, XML, YAML, TOML, or CSV validation and parsing.
///          Because a valid null document may also yield null, inspect
///          `rt_serialize_error()` to distinguish failure.
/// @param text Borrowed serialized input.
/// @param format Source `rt_format_t` value.
/// @return Caller-owned parsed value, or null for valid null or failure.
void *rt_serialize_parse(rt_string text, int64_t format);

/// @brief Parse text in the specified format into a Zanna.Result.
/// @details Returns `Ok(value)` for valid input, including valid null values
///          that map to NULL, and `Err(message)` for invalid input, nil input,
///          or unknown format values.
/// @param text Borrowed serialized input.
/// @param format Source `rt_format_t` value.
/// @return Caller-owned Zanna.Result containing the parsed value or error.
void *rt_serialize_parse_result(rt_string text, int64_t format);

/// @brief Format a Zanna value as text in the specified format.
/// @details Generic values are projected as needed for XML, TOML, and CSV;
///          XML nodes are projected before non-XML output.
/// @param obj Borrowed generic value or XML node.
/// @param format Target `rt_format_t` value.
/// @return Caller-owned formatted string, or empty text with an error.
rt_string rt_serialize_format(void *obj, int64_t format);

/// @brief Format a Zanna value as pretty-printed text.
/// @details Indents less than one default to two. TOML and CSV ignore the
///          indentation request.
/// @param obj Borrowed generic value or XML node.
/// @param format Target `rt_format_t` value.
/// @param indent Spaces per indentation level.
/// @return Caller-owned formatted string, or empty text with an error.
rt_string rt_serialize_format_pretty(void *obj, int64_t format, int64_t indent);

/// @brief Check if text is valid for the specified format.
/// @param text Borrowed text to validate.
/// @param format Candidate `rt_format_t` value.
/// @return `1` if the selected backend accepts the text, otherwise `0`.
int8_t rt_serialize_is_valid(rt_string text, int64_t format);

//=========================================================================
// Format Auto-Detection
//=========================================================================

/// @brief Detect the format of a text string.
/// @details Heuristically detects the format based on content:
///          - Starts with a valid JSON object/array, or JSON-looking '{'/'[' → JSON
///          - Starts with '<' → XML
///          - Starts with a standalone '---' document marker → YAML
///          - Contains a valid TOML '[section]' or 'key = value' first line → TOML
///          - Contains a valid YAML 'key: value' first line → YAML
///          - Contains commas on the first line → CSV
/// @param text Borrowed text to detect.
/// @return Detected `rt_format_t` value, or `-1` if unrecognized.
int64_t rt_serialize_detect(rt_string text);

/// @brief Parse text by auto-detecting the format.
/// @details Detection failure records a thread-local diagnostic before
///          returning null.
/// @param text Borrowed serialized input.
/// @return Caller-owned parsed value, or null for valid null or failure.
void *rt_serialize_auto_parse(rt_string text);

/// @brief Parse text by auto-detecting the format and return a Zanna.Result.
/// @details Returns `Ok(value)` after successful detection and parsing, or
///          `Err(message)` when detection fails or the detected parser rejects
///          the input.
/// @param text Borrowed serialized input.
/// @return Caller-owned Zanna.Result containing the parsed value or error.
void *rt_serialize_auto_parse_result(rt_string text);

//=========================================================================
// Round-Trip Conversion
//=========================================================================

/// @brief Convert between formats.
/// @details Validates and parses the source, projects XML nodes to generic
///          structures when needed, then formats the target. Cross-format
///          conversion may be structurally lossy.
/// @param text Borrowed source text.
/// @param from_format Source `rt_format_t` value.
/// @param to_format Target `rt_format_t` value.
/// @return Caller-owned converted text, or empty text with an error.
rt_string rt_serialize_convert(rt_string text, int64_t from_format, int64_t to_format);

//=========================================================================
// Format Metadata
//=========================================================================

/// @brief Get the name of a format.
/// @param format Candidate `rt_format_t` value.
/// @return Newly allocated lowercase format name, or `"unknown"`.
rt_string rt_serialize_format_name(int64_t format);

/// @brief Get the MIME type for a format.
/// @param format Candidate `rt_format_t` value.
/// @return Newly allocated MIME type, or `"application/octet-stream"`.
rt_string rt_serialize_mime_type(int64_t format);

/// @brief Look up format by name (case-insensitive).
/// @details Accepts `yaml` and `yml`; rejects embedded null bytes.
/// @param name Borrowed case-insensitive short format name.
/// @return Matching `rt_format_t` value, or `-1` if unrecognized.
int64_t rt_serialize_format_from_name(rt_string name);

//=========================================================================
// Error Handling
//=========================================================================

/// @brief Get the last serialization error message.
/// @details Error state is thread-local and cleared at the beginning of public
///          serialization operations.
/// @return Retained caller-owned error string, or an empty string if none.
rt_string rt_serialize_error(void);

#ifdef __cplusplus
}
#endif
