//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_xml_format.c
// Purpose: XML serialization (compact + pretty) and entity escape/unescape for
//   the Zanna.Text.Xml class. Parsing and the DOM API live in rt_xml.c.
//
// Links: rt_xml.h (public API), rt_xml_internal.h (node struct), rt_xml.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief XML tree serialization and entity conversion for the runtime XML API.
/// @details Compact and pretty formatting share a growable byte buffer. Text
///          and attributes are escaped according to their XML context, while
///          validated comment and CDATA bodies are reproduced verbatim.

#include "rt_xml.h"

#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include "rt_xml_internal.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================

/// @brief Serialize one node into a shared growable output buffer.
/// @param node Valid XML node to emit.
/// @param indent Spaces per block indentation level, or zero for compact output.
/// @param level Current tree depth.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_node(void *node, int indent, int level, char **buf, size_t *cap, size_t *len);

// Format-side scratch helpers — a tiny growable c-string used during
// `Format` / `FormatPretty`. `*cap` doubles on overflow; OOM traps.

/// @brief Append a null-terminated string to the formatting buffer.
/// @details Doubles the allocation from an initial 256 bytes until the content
///          fits. Allocation and size-overflow failures raise a runtime trap.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Required null-terminated source string.
static void buf_append(char **buf, size_t *cap, size_t *len, const char *str) {
    size_t slen = strlen(str);
    while (*len + slen + 1 < *len || *len + slen + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : (*cap * 2);
        if (new_cap <= *cap) {
            rt_trap("XML format: output length overflow");
            return;
        }
        char *tmp = (char *)realloc(*buf, new_cap);
        if (!tmp) {
            rt_trap("XML format: memory allocation failed");
            return;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

/// @brief Append an exact byte span to the formatting buffer.
/// @details Embedded null bytes are copied as data; a trailing null terminator
///          is maintained after the span. Allocation and overflow failures trap.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Source byte span.
/// @param slen Number of source bytes.
static void buf_append_bytes(char **buf, size_t *cap, size_t *len, const char *str, size_t slen) {
    while (*len + slen + 1 < *len || *len + slen + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : (*cap * 2);
        if (new_cap <= *cap) {
            rt_trap("XML format: output length overflow");
            return;
        }
        char *tmp = (char *)realloc(*buf, new_cap);
        if (!tmp) {
            rt_trap("XML format: memory allocation failed");
            return;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

/// @brief Append one byte to the formatting buffer.
/// @details Preserves a trailing null terminator and traps on allocation or
///          output-length overflow.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param c Byte to append.
static void buf_append_char(char **buf, size_t *cap, size_t *len, char c) {
    if (*len > SIZE_MAX - 2) {
        rt_trap("XML format: output length overflow");
        return;
    }
    if (*len + 2 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : (*cap * 2);
        if (new_cap <= *cap) {
            rt_trap("XML format: output length overflow");
            return;
        }
        char *tmp = (char *)realloc(*buf, new_cap);
        if (!tmp) {
            rt_trap("XML format: memory allocation failed");
            return;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    (*buf)[*len] = c;
    (*len)++;
    (*buf)[*len] = '\0';
}

/// @brief Append indentation spaces to the formatting buffer.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param spaces Number of ASCII space bytes to append; nonpositive values add none.
static void buf_append_indent(char **buf, size_t *cap, size_t *len, int spaces) {
    for (int i = 0; i < spaces; i++)
        buf_append_char(buf, cap, len, ' ');
}

/// @brief Append `str` with XML special-character escaping.
///
/// Always escapes `&`, `<`, `>`. Quotes (`"` and `'`) are escaped only
/// when `for_attr` is set — text content can carry literal quotes
/// freely. Forbidden C0 control bytes are rendered as `&#xFFFD;`.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Source byte span.
/// @param slen Number of source bytes.
/// @param for_attr Nonzero to apply quoted-attribute escaping.
static void buf_append_escaped_bytes(
    char **buf, size_t *cap, size_t *len, const char *str, size_t slen, int for_attr) {
    for (size_t i = 0; i < slen; i++) {
        switch (str[i]) {
            case '&':
                buf_append(buf, cap, len, "&amp;");
                break;
            case '<':
                buf_append(buf, cap, len, "&lt;");
                break;
            case '>':
                buf_append(buf, cap, len, "&gt;");
                break;
            case '"':
                if (for_attr)
                    buf_append(buf, cap, len, "&quot;");
                else
                    buf_append_char(buf, cap, len, '"');
                break;
            case '\'':
                if (for_attr)
                    buf_append(buf, cap, len, "&apos;");
                else
                    buf_append_char(buf, cap, len, '\'');
                break;
            default:
                if (contains_invalid_xml_chars(str + i, 1))
                    buf_append(buf, cap, len, "&#xFFFD;");
                else
                    buf_append_char(buf, cap, len, str[i]);
                break;
        }
    }
}

/// @brief Append a runtime string with context-appropriate XML escaping.
/// @details Null strings add no bytes; nonnull strings use their runtime byte
///          length rather than relying on a terminator.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Runtime string to append, or null.
/// @param for_attr Nonzero to apply quoted-attribute escaping.
static void buf_append_escaped(char **buf, size_t *cap, size_t *len, rt_string str, int for_attr) {
    if (!str)
        return;
    buf_append_escaped_bytes(buf, cap, len, rt_string_cstr(str), (size_t)rt_str_len(str), for_attr);
}

/// @brief Emit a single element (opening tag, attrs, children, closing tag).
///
/// Self-closes (`<tag/>`) when there are no children. Switches between
/// inline and indented output: an element with only text/CDATA children
/// or mixed content is emitted inline so formatting cannot change character
/// data; an element containing only non-text children gets newlines and
/// indentation when `indent > 0`.
/// @param elem Valid element node to emit.
/// @param indent Spaces per block indentation level.
/// @param level Current element depth.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_element(
    xml_node *elem, int indent, int level, char **buf, size_t *cap, size_t *len) {
    // Indentation
    if (indent > 0 && level > 0)
        buf_append_indent(buf, cap, len, indent * level);

    // Opening tag
    buf_append_char(buf, cap, len, '<');
    buf_append(buf, cap, len, rt_string_cstr(elem->tag));

    // Attributes
    if (elem->attributes) {
        void *keys = rt_map_keys(elem->attributes);
        int64_t nkeys = rt_seq_len(keys);
        for (int64_t i = 0; i < nkeys; i++) {
            void *key = rt_seq_get(keys, i);
            void *val = rt_map_get(elem->attributes, (rt_string)key);

            buf_append_char(buf, cap, len, ' ');
            buf_append(buf, cap, len, rt_string_cstr((rt_string)key));
            buf_append(buf, cap, len, "=\"");
            buf_append_escaped(buf, cap, len, (rt_string)val, 1);
            buf_append_char(buf, cap, len, '"');
        }
        // Release the keys Seq (its owns_elements finalizer releases the strings)
        if (rt_obj_release_check0(keys))
            rt_obj_free(keys);
    }

    // Check for children
    int64_t nchildren = elem->children ? rt_seq_len(elem->children) : 0;
    if (nchildren == 0) {
        buf_append(buf, cap, len, "/>");
        if (indent > 0)
            buf_append_char(buf, cap, len, '\n');
        return;
    }

    buf_append_char(buf, cap, len, '>');

    bool has_text_like = false;
    bool has_non_text_like = false;
    for (int64_t i = 0; i < nchildren; i++) {
        void *child = rt_seq_get(elem->children, i);
        xml_node *cn = (xml_node *)child;
        if (cn->type == XML_NODE_TEXT || cn->type == XML_NODE_CDATA)
            has_text_like = true;
        else
            has_non_text_like = true;
        // Borrowed reference — parent owns child, do not release
    }
    bool text_only = has_text_like && !has_non_text_like;
    bool mixed_content = has_text_like && has_non_text_like;
    bool pretty_block = !text_only && !mixed_content && indent > 0;

    if (pretty_block)
        buf_append_char(buf, cap, len, '\n');

    // Children
    for (int64_t i = 0; i < nchildren; i++) {
        void *child = rt_seq_get(elem->children, i);
        format_node(child, (text_only || mixed_content) ? 0 : indent, level + 1, buf, cap, len);
        // Borrowed reference — parent owns child, do not release
    }

    // Closing tag
    if (pretty_block && level > 0)
        buf_append_indent(buf, cap, len, indent * level);
    buf_append(buf, cap, len, "</");
    buf_append(buf, cap, len, rt_string_cstr(elem->tag));
    buf_append_char(buf, cap, len, '>');
    if (indent > 0)
        buf_append_char(buf, cap, len, '\n');
}

/// @brief Dispatch formatter — emits any node by kind.
///
/// Element nodes go to `format_element`; text is escaped; comments
/// and CDATA are emitted with their delimiters; documents iterate
/// their children at level 0.
/// @param node Valid XML node to emit.
/// @param indent Spaces per block indentation level, or zero for compact output.
/// @param level Current tree depth.
/// @param buf Address of the heap buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_node(void *node, int indent, int level, char **buf, size_t *cap, size_t *len) {
    xml_node *n = (xml_node *)node;

    switch (n->type) {
        case XML_NODE_ELEMENT:
            format_element(n, indent, level, buf, cap, len);
            break;

        case XML_NODE_TEXT:
            if (n->content)
                buf_append_escaped(buf, cap, len, n->content, 0);
            break;

        case XML_NODE_COMMENT:
            if (indent > 0 && level > 0)
                buf_append_indent(buf, cap, len, indent * level);
            buf_append(buf, cap, len, "<!--");
            if (n->content)
                buf_append_bytes(
                    buf, cap, len, rt_string_cstr(n->content), (size_t)rt_str_len(n->content));
            buf_append(buf, cap, len, "-->");
            if (indent > 0)
                buf_append_char(buf, cap, len, '\n');
            break;

        case XML_NODE_CDATA:
            buf_append(buf, cap, len, "<![CDATA[");
            if (n->content)
                buf_append_bytes(
                    buf, cap, len, rt_string_cstr(n->content), (size_t)rt_str_len(n->content));
            buf_append(buf, cap, len, "]]>");
            break;

        case XML_NODE_DOCUMENT:
            if (n->children) {
                int64_t count = rt_seq_len(n->children);
                for (int64_t i = 0; i < count; i++) {
                    void *child = rt_seq_get(n->children, i);
                    format_node(child, indent, 0, buf, cap, len);
                    // Borrowed reference — parent owns child, do not release
                }
            }
            break;
    }
}

/// @brief `Xml.Format(node)` — serialize a node tree to compact XML text.
///
/// No added whitespace, no XML declaration. For human-readable output
/// use `FormatPretty`. Returns "" for NULL.
///
/// @param node Node to serialize.
/// @return Owned `rt_string` containing the XML.
rt_string rt_xml_format(void *node) {
    if (!rt_xml_is_node(node))
        return rt_str_empty();

    char *buf = NULL;
    size_t cap = 0, len = 0;

    format_node(node, 0, 0, &buf, &cap, &len);

    rt_string result = rt_string_from_bytes(buf ? buf : "", (int64_t)len);
    free(buf);
    return result;
}

/// @brief `Xml.FormatPretty(node, indent)` — serialize with indentation.
///
/// `indent` is the number of spaces per nesting level (clamped to
/// 0..8). A trailing newline is trimmed for consistency with most
/// pretty-printers. Mixed-content elements (interleaved text and
/// element children) are kept inline since reflowing them can change
/// semantics — see `format_element` for details.
///
/// @param node   Node to serialize.
/// @param indent Spaces per indent level (0..8).
/// @return Owned `rt_string` containing the indented XML.
rt_string rt_xml_format_pretty(void *node, int64_t indent) {
    if (!rt_xml_is_node(node))
        return rt_str_empty();

    if (indent < 0)
        indent = 0;
    if (indent > 8)
        indent = 8;

    char *buf = NULL;
    size_t cap = 0, len = 0;

    format_node(node, (int)indent, 0, &buf, &cap, &len);

    // Remove trailing newline for consistency
    if (len > 0 && buf[len - 1] == '\n')
        len--;

    rt_string result = rt_string_from_bytes(buf ? buf : "", (int64_t)len);
    free(buf);
    return result;
}

//=============================================================================
// Public API - Utility
//=============================================================================

/// @brief `Xml.Escape(text)` — apply XML text and attribute escaping.
///
/// Escapes `&`, `<`, `>`, `"`, and `'` so the result is safe in either
/// element text or a quoted attribute value. Forbidden C0 control bytes are
/// replaced by the numeric reference `&#xFFFD;`.
/// @param text Runtime string to escape; null is treated as empty.
/// @return Owned escaped string.
rt_string rt_xml_escape(rt_string text) {
    if (!text)
        return rt_str_empty();

    const char *src = rt_string_cstr(text);
    char *buf = NULL;
    size_t cap = 0, len = 0;

    buf_append_escaped_bytes(&buf, &cap, &len, src, (size_t)rt_str_len(text), 1);

    rt_string result = rt_string_from_bytes(buf ? buf : "", (int64_t)len);
    free(buf);
    return result;
}

/// @brief `Xml.Unescape(text)` — decode XML entity references back to characters.
///
/// Inverse of `Escape`. Recognises numeric (`&#NN;` / `&#xHH;`) and the
/// five named XML entities. Unknown entities are left in place
/// (consistent with `decode_entity`'s 0-return policy).
/// @param text Runtime string to decode; null is treated as empty.
/// @return Owned decoded string; allocation failure produces an empty string.
rt_string rt_xml_unescape(rt_string text) {
    if (!text)
        return rt_str_empty();

    const char *src = rt_string_cstr(text);
    size_t src_len = (size_t)rt_str_len(text);

    char *buf = malloc(src_len + 1);
    if (!buf)
        return rt_str_empty();

    size_t out = 0;
    for (size_t i = 0; i < src_len;) {
        if (src[i] == '&') {
            char decoded[4];
            size_t consumed;
            int n = decode_entity(src + i, src_len - i, decoded, &consumed);
            if (n > 0) {
                memcpy(buf + out, decoded, n);
                out += n;
                i += consumed;
                continue;
            }
        }
        buf[out++] = src[i++];
    }
    buf[out] = '\0';

    rt_string result = rt_string_from_bytes(buf, (int64_t)out);
    free(buf);
    return result;
}
