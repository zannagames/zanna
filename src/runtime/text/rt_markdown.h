//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_markdown.h
// Purpose: Line-oriented Markdown-subset rendering, plain-text conversion,
//          and extraction of inline link destinations and ATX headings.
//
// Key invariants:
//   - Supports ATX-style headers (#, ##, ..., ######), bold (**), italic (*), inline code (`).
//   - HTML output supports fenced code blocks (```), horizontal rules,
//     unordered lists (-/*), links, and one-line paragraphs.
//   - Ordered lists, images, reference links, nesting, and most CommonMark
//     delimiter rules are not implemented.
//   - Not a full CommonMark implementation; edge cases may differ.
//   - Link extraction returns URL strings only and rewrites unsafe schemes to "#".
//   - User content is HTML-escaped before rendering.
//
// Ownership/Lifetime:
//   - Returned strings and element-owning Seqs are newly allocated.
//   - Input strings are borrowed for the duration of conversion.
//
// Links: src/runtime/text/rt_markdown.c (implementation),
//        src/runtime/core/rt_string.h (runtime strings),
//        src/runtime/collections/rt_seq.h (extraction results)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_markdown.h
 * @brief Declares Markdown-subset rendering, stripping, and extraction.
 * @details Borrowed source text can be converted to escaped HTML or plain text
 *          and scanned for simple link destinations or ATX headings. The API
 *          intentionally implements a documented subset rather than full
 *          CommonMark and returns caller-owned Strings or sequences.
 */

#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Convert Markdown to HTML.
/// @details Renders the supported block/inline subset, escapes literal HTML
///          metacharacters, and rewrites `javascript:`, `data:`, and
///          `vbscript:` link schemes to `#`. Blank lines close open lists.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated HTML string owned by the caller.
rt_string rt_markdown_to_html(rt_string md);

/// @brief Strip Markdown to plain text.
/// @details Removes recognized heading prefixes, matched emphasis/code
///          delimiters, and link destinations. Preserves line boundaries,
///          unmatched markers, intraword underscores, and list/fence markers.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated plain-text string owned by the caller.
rt_string rt_markdown_to_text(rt_string md);

/// @brief Extract all links from Markdown.
/// @details Recognizes simple `[label](url)` patterns without nesting or
///          escapes. Unsafe schemes are returned as `"#"`.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated element-owning Seq of copied URL strings.
void *rt_markdown_extract_links(rt_string md);

/// @brief Extract all headings from Markdown.
/// @details Recognizes one through six leading `#` bytes followed by a literal
///          space at column zero. Inline markers in heading text are retained.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated element-owning Seq of copied heading strings.
void *rt_markdown_extract_headings(rt_string md);

#ifdef __cplusplus
}
#endif
