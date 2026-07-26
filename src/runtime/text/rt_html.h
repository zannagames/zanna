//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_html.h
// Purpose: Tolerant HTML tree scanning, text/link extraction, tag stripping,
//          and a deliberately small HTML-entity codec.
//
// Key invariants:
//   - Malformed syntax is tolerated; resource exhaustion and size overflow may
//     still raise runtime traps.
//   - Parse nodes are Maps with "tag", "text", "attrs", and "children".
//   - Escaping covers &, <, >, ', and "; unescaping covers those entities,
//     nbsp, and semicolon-terminated non-NUL ASCII numerics.
//   - Entity decoding is separate from parsing, stripping, and extraction.
//
// Ownership/Lifetime:
//   - Returned strings and objects are newly allocated; caller must release.
//   - Input strings are borrowed for the duration of parsing.
//
// Links: src/runtime/text/rt_html.c (implementation),
//        src/runtime/core/rt_string.h (runtime strings),
//        src/runtime/collections/rt_map.h (nodes and attributes),
//        src/runtime/collections/rt_seq.h (children and extraction results)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_html.h
 * @brief Declares tolerant HTML parsing, entity, stripping, and extraction APIs.
 * @details Parsing returns a caller-owned Map tree of elements, attributes,
 *          text, and children without requiring well-formed markup. Utility
 *          routines return fresh Strings or sequences for escaping,
 *          unescaping, plain text, stripped text, links, and text fragments.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse an HTML string into a tree of map nodes.
/// @details Returns a synthetic root with an empty tag. Element nodes preserve
///          tag and attribute spelling; text nodes use an empty tag and raw,
///          undecoded text. Comments, declarations, and whitespace-only text
///          nodes are omitted. The scanner repairs parentage where possible
///          but is not a standards-complete browser parser.
/// @param str Borrowed HTML text; null and empty inputs produce an empty root.
/// @return Newly allocated root Map owning its complete node tree.
void *rt_html_parse(rt_string str);

/// @brief Strip all HTML tags and unescape entities to get plain text.
/// @details Equivalent to `rt_html_unescape(rt_html_strip_tags(str))`, with
///          the intermediate runtime string released internally.
/// @param str Borrowed HTML text; null is treated as empty.
/// @return Newly allocated plain-text string owned by the caller.
rt_string rt_html_to_text(rt_string str);

/// @brief Escape HTML special characters (<, >, &, ", ').
/// @details Replaces the five listed bytes with `&lt;`, `&gt;`, `&amp;`,
///          `&quot;`, and `&#39;`. Other bytes are copied unchanged.
/// @param str Borrowed text to escape; null is treated as empty.
/// @return Newly allocated escaped string owned by the caller.
rt_string rt_html_escape(rt_string str);

/// @brief Decode the runtime's supported HTML entity subset.
/// @details Decodes case-insensitive `lt`, `gt`, `amp`, `quot`, `apos`, and
///          `nbsp`, plus decimal or hexadecimal non-NUL ASCII numeric
///          references. Semicolons are required; unsupported references are
///          copied unchanged.
/// @param str Borrowed string containing entities; null is treated as empty.
/// @return Newly allocated decoded string owned by the caller.
rt_string rt_html_unescape(rt_string str);

/// @brief Remove all HTML tags from a string.
/// @details Removes `<...>` ranges and inserts spaces at recognized block and
///          break tag boundaries. Entities are not decoded.
/// @param str Borrowed HTML text; null is treated as empty.
/// @return Newly allocated tag-stripped string owned by the caller.
rt_string rt_html_strip_tags(rt_string str);

/// @brief Extract all href values from anchor (<a>) tags.
/// @details Accepts quoted, unquoted, and Boolean href attributes. Values are
///          copied verbatim without entity decoding or URL normalization.
/// @param str Borrowed HTML text; null is treated as empty.
/// @return Newly allocated element-owning Seq of copied href strings.
void *rt_html_extract_links(rt_string str);

/// @brief Extract text content of all matching tags.
/// @details Matches tag names case-insensitively, tracks nested elements with
///          the same name, and strips inner markup. Entities remain encoded.
/// @param str Borrowed HTML text; null is treated as empty.
/// @param tag Borrowed non-empty tag name, such as `"p"` or `"h1"`.
/// @return Newly allocated element-owning Seq of extracted text strings.
void *rt_html_extract_text(rt_string str, rt_string tag);

#ifdef __cplusplus
}
#endif
