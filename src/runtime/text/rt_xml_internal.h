//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_xml_internal.h
// Purpose: Shared internal XML node representation for the XML runtime, used by
//   the parser/DOM (rt_xml.c) and the serializer (rt_xml_format.c).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private XML node layout and parser/formatter helper declarations.
/// @details This header is shared only by the XML parser and serializer. Public
///          callers must treat node pointers as opaque handles and use
///          `rt_xml.h`.

#pragma once

#include "rt_string.h"
#include "rt_xml.h" // rt_xml_node_type_t

#include <stdbool.h>
#include <stddef.h>

/// @brief Detect forbidden XML 1.0 C0 control bytes in a span.
/// @details Horizontal tab, line feed, and carriage return are permitted. This
///          byte-level helper does not validate complete UTF-8 sequences.
/// @param s Byte span, or null only when @p len is zero.
/// @param len Number of bytes to inspect.
/// @return `true` when the span is invalid or contains a forbidden control byte.
bool contains_invalid_xml_chars(const char *s, size_t len);

/// @brief Decode one XML entity or numeric character reference.
/// @details Recognizes the five predefined named entities and decimal or
///          hexadecimal numeric references. Numeric values must be legal XML
///          characters and are encoded as one to four UTF-8 bytes.
/// @param str Input span beginning with `&`.
/// @param len Number of available input bytes.
/// @param out Destination with room for at least four bytes.
/// @param consumed Receives the input bytes consumed, including delimiters,
///                 on success.
/// @return Number of bytes written to @p out, or zero for an invalid or
///         unsupported reference.
int decode_entity(const char *str, size_t len, char *out, size_t *consumed);

/// @brief Internal reference-counted representation of every XML node kind.
/// @details Element nodes own attribute maps and child sequences. The child
///          sequence retains each child, while @ref xml_node::parent is a weak
///          back-reference that prevents ownership cycles.
typedef struct xml_node {
    rt_xml_node_type_t type; ///< Discriminator controlling which fields apply.
    rt_string tag;           ///< Owned tag name for element nodes, otherwise null.
    rt_string content;       ///< Owned body for text, comment, and CDATA nodes.
    void *attributes;        ///< Owned string map for element nodes.
    void *children;          ///< Owned sequence of retained child nodes.
    struct xml_node *parent; ///< Borrowed weak parent pointer, or null.
} xml_node;
