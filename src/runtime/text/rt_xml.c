//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_xml.c
// Purpose: Implements XML parsing and formatting for the Zanna.Data.Xml class
//          per XML 1.0. Builds a node tree supporting elements, text content,
//          comments, and CDATA sections. Provides Parse, Format, FormatPretty,
//          and node navigation (Tag, Attr, SetAttr, Children, TextContent).
//
// Key invariants:
//   - Parse returns a document root node; invalid XML returns NULL.
//   - Element nodes carry a tag name, attribute rt_map, and children rt_seq.
//   - Text and CDATA nodes carry a text content string.
//   - Attributes are stored as an rt_map<String, String>.
//   - Format produces minimal XML (no added whitespace).
//   - Parse and diagnostics use thread-local state.
//
// Ownership/Lifetime:
//   - The node tree is heap-allocated; all nodes are owned by their parent.
//   - The caller owns the root node returned by Parse.
//   - Formatted XML strings are fresh rt_string allocations owned by caller.
//
// Links: src/runtime/text/rt_xml.h (public API),
//        src/runtime/text/rt_html.h (tolerant HTML parser, related)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_xml.c
 * @brief Implements XML document parsing, formatting, and node navigation.
 * @details The parser constructs managed element, text, comment, and CDATA
 *          nodes with owned attribute Maps and child sequences, records
 *          thread-local syntax diagnostics, and supports compact or pretty
 *          serialization plus tag, attribute, child, and text operations.
 */

#include "rt_xml.h"

#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// XML Node Structure
//=============================================================================

/// @brief Internal XML node structure.
#include "rt_xml_internal.h"

/// @brief Last parse error message (thread-local to avoid concurrent parse clobbering).
static _Thread_local char xml_last_error[256] = {0};

/// @brief Release a temporary runtime object after another owner has retained it.
/// @param obj Runtime-managed object reference; null is ignored.
static void xml_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

//=============================================================================
// Forward Declarations
//=============================================================================

static void xml_node_finalizer(void *obj);
static void *xml_node_new(rt_xml_node_type_t type);
static void *parse_document(const char *input, size_t len);

//=============================================================================
// Helper Functions
//=============================================================================

#include "rt_trap.h"

static void set_error(const char *msg);

/// @brief Safely expose a runtime XML input string as a byte span.
/// @details Public XML entry points use `size_t` lengths internally. This
///          helper rejects malformed handles that report negative lengths before
///          a cast can turn them into very large spans.
/// @param text Runtime string handle to inspect.
/// @param empty_error Error message to set when the input is NULL or empty.
/// @param out_cstr Receives the borrowed UTF-8 byte pointer.
/// @param out_len Receives the non-negative byte length.
/// @return 1 when a non-empty byte span is available; otherwise 0.
static int xml_string_view_nonempty(rt_string text,
                                    const char *empty_error,
                                    const char **out_cstr,
                                    size_t *out_len) {
    if (!text) {
        set_error(empty_error);
        return 0;
    }
    int64_t raw_len = rt_str_len(text);
    if (raw_len <= 0) {
        set_error(raw_len == 0 ? empty_error : "Invalid XML string length");
        return 0;
    }
    const char *cstr = rt_string_cstr(text);
    if (!cstr) {
        set_error("Invalid XML string");
        return 0;
    }
    *out_cstr = cstr;
    *out_len = (size_t)raw_len;
    return 1;
}

/// @brief Safely expose an optional runtime XML content string as a byte span.
/// @details Unlike `xml_string_view_nonempty`, NULL is accepted and maps to an
///          empty span for constructors such as `Xml.Comment` and `Xml.Cdata`.
/// @param text Optional runtime string handle.
/// @param out_cstr Receives the borrowed byte pointer or NULL for no content.
/// @param out_len Receives the non-negative byte length.
/// @return 1 when the handle is valid; otherwise 0 and `xml_last_error` is set.
static int xml_optional_string_view(rt_string text, const char **out_cstr, size_t *out_len) {
    *out_cstr = NULL;
    *out_len = 0;
    if (!text)
        return 1;
    int64_t raw_len = rt_str_len(text);
    if (raw_len < 0) {
        set_error("Invalid XML string length");
        return 0;
    }
    const char *cstr = rt_string_cstr(text);
    if (!cstr && raw_len > 0) {
        set_error("Invalid XML string");
        return 0;
    }
    *out_cstr = cstr;
    *out_len = (size_t)raw_len;
    return 1;
}

/// @brief Stash a parse-error message in the thread-local error buffer.
///
/// Truncates to fit within the 256-byte buffer with a null terminator.
/// Subsequent calls overwrite. The error survives until `clear_error`
/// or the next failed parse on this thread, and is exposed via
/// `rt_xml_error()`.
/// @param msg Required null-terminated diagnostic text.
static void set_error(const char *msg) {
    strncpy(xml_last_error, msg, sizeof(xml_last_error) - 1);
    xml_last_error[sizeof(xml_last_error) - 1] = '\0';
}

/// @brief Reset the thread-local parse-error buffer to empty.
///
/// Called at the start of every `parse_document` so success returns
/// clear `rt_xml_error()` output.
static void clear_error(void) {
    xml_last_error[0] = '\0';
}

/// @brief Append an XML child to an owned Seq and verify that it was added.
/// @details `rt_seq_push` traps on allocation failure and returns `void`. When
///          a trap recovery handler resumes execution, checking the length
///          change lets XML parsing stop cleanly instead of reporting success
///          with a missing child.
/// @param seq Owned child sequence.
/// @param child Child node to append.
/// @param error_msg Diagnostic stored in `rt_xml_error()` on failure.
/// @return 1 when the child was appended; 0 when parsing should fail.
static int xml_seq_push_checked(void *seq, void *child, const char *error_msg) {
    int64_t before = rt_seq_len(seq);
    rt_seq_push(seq, child);
    if (rt_seq_len(seq) != before + 1) {
        set_error(error_msg ? error_msg : "XML sequence append failed");
        return 0;
    }
    return 1;
}

/// @brief Push a borrowed XML node pointer onto an explicit traversal stack.
/// @details Used by text-content collection to avoid recursive traversal of
///          programmatically-built trees. The stack stores borrowed references;
///          ownership remains with the XML node tree.
/// @param stack_io Stack allocation pointer, grown with `realloc` as needed.
/// @param count_io Number of entries currently stored.
/// @param cap_io Current stack capacity.
/// @param node Borrowed XML node pointer to push.
/// @return 1 on success; 0 on allocation overflow/OOM.
static int xml_traversal_stack_push(void ***stack_io,
                                    size_t *count_io,
                                    size_t *cap_io,
                                    void *node) {
    if (*count_io == *cap_io) {
        size_t new_cap = *cap_io ? *cap_io * 2u : 32u;
        if (new_cap < *cap_io || new_cap > SIZE_MAX / sizeof(void *)) {
            set_error("XML traversal stack overflow");
            return 0;
        }
        void **new_stack = (void **)realloc(*stack_io, new_cap * sizeof(void *));
        if (!new_stack) {
            set_error("XML traversal allocation failed");
            return 0;
        }
        *stack_io = new_stack;
        *cap_io = new_cap;
    }
    (*stack_io)[(*count_io)++] = node;
    return 1;
}

/// @brief Return true if the thread-local error buffer is currently non-empty.
/// @return Whether the current thread has a recorded XML error.
static bool has_error(void) {
    return xml_last_error[0] != '\0';
}

/// @brief Borrowed-node stack used to traverse XML trees without C recursion.
typedef struct xml_node_stack {
    void **items; ///< Borrowed XML node pointers.
    size_t len;   ///< Number of occupied entries.
    size_t cap;   ///< Allocated entry count.
} xml_node_stack;

/// @brief Release storage owned by an XML traversal stack.
/// @param stack Stack to dispose. Safe to call on an empty stack.
static void xml_node_stack_dispose(xml_node_stack *stack) {
    if (!stack)
        return;
    free(stack->items);
    stack->items = NULL;
    stack->len = 0;
    stack->cap = 0;
}

/// @brief Push a borrowed XML node pointer onto a traversal stack.
/// @details The stack does not retain nodes; callers must only use it while the
///          source XML tree is otherwise live. Growth is checked for both
///          `size_t` overflow and allocation failure.
/// @param stack Stack to grow.
/// @param node Borrowed node pointer to push.
/// @param api Diagnostic prefix used if the stack cannot grow.
/// @return true on success, false after recording and trapping an allocation error.
static bool xml_node_stack_push(xml_node_stack *stack, void *node, const char *api) {
    if (!node)
        return true;
    if (stack->len == stack->cap) {
        if (stack->cap > SIZE_MAX / 2u) {
            set_error("XML traversal stack overflow");
            rt_trap(api);
            return false;
        }
        size_t new_cap = stack->cap ? stack->cap * 2u : 64u;
        if (new_cap > SIZE_MAX / sizeof(*stack->items)) {
            set_error("XML traversal stack overflow");
            rt_trap(api);
            return false;
        }
        void **grown = (void **)realloc(stack->items, new_cap * sizeof(*stack->items));
        if (!grown) {
            set_error("XML traversal stack allocation failed");
            rt_trap(api);
            return false;
        }
        stack->items = grown;
        stack->cap = new_cap;
    }
    stack->items[stack->len++] = node;
    return true;
}

//=============================================================================
// Node Management
//=============================================================================

/// @brief GC finalizer for XML nodes — cascades release through the tree.
///
/// Releases tag, content, attribute map, and every child retained by
/// this node. Children are owned by their parent, so dropping the root
/// cleans up the entire document.
/// `parent` is a weak pointer and is intentionally not released.
/// @param obj XML node being finalized; null is ignored.
static void xml_node_finalizer(void *obj) {
    xml_node *node = (xml_node *)obj;
    if (!node)
        return;

    // Release tag
    if (node->tag) {
        if (rt_obj_release_check0((void *)node->tag))
            rt_obj_free((void *)node->tag);
    }

    // Release content
    if (node->content) {
        if (rt_obj_release_check0((void *)node->content))
            rt_obj_free((void *)node->content);
    }

    // Release attributes map
    if (node->attributes) {
        if (rt_obj_release_check0(node->attributes))
            rt_obj_free(node->attributes);
    }

    // Release children (parent owns each child — finalizer must release them)
    if (node->children) {
        int64_t count = rt_seq_len(node->children);
        for (int64_t i = 0; i < count; i++) {
            void *child = rt_seq_get(node->children, i);
            if (child) {
                if (rt_obj_release_check0(child))
                    rt_obj_free(child);
            }
        }
        if (rt_obj_release_check0(node->children))
            rt_obj_free(node->children);
    }

    // Note: parent is a weak reference, don't release
}

/// @brief Allocate a fresh XML node of the given type.
///
/// Hooks `xml_node_finalizer` so the GC takes care of cleanup. Element
/// and document nodes get an empty children seq; element nodes also
/// get an empty attribute map. Returns NULL on any allocation failure
/// (caller decides whether that should trap or be propagated).
///
/// @param type Node kind (element / text / comment / cdata / document).
/// @return Owned node pointer, or NULL on OOM.
static void *xml_node_new(rt_xml_node_type_t type) {
    xml_node *node = (xml_node *)rt_obj_new_i64(RT_XML_NODE_CLASS_ID, sizeof(xml_node));
    if (!node)
        return NULL;

    rt_obj_set_finalizer(node, xml_node_finalizer);

    node->type = type;
    node->tag = NULL;
    node->content = NULL;
    node->attributes = NULL;
    node->children = NULL;
    node->parent = NULL;

    // Create children seq for elements and documents
    if (type == XML_NODE_ELEMENT || type == XML_NODE_DOCUMENT) {
        node->children = rt_seq_new();
        if (!node->children) {
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }
    }

    // Create attributes map for elements
    if (type == XML_NODE_ELEMENT) {
        node->attributes = rt_map_new();
        if (!node->attributes) {
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }
    }

    return node;
}

//=============================================================================
// Parser State
//=============================================================================

/* S-17: Maximum element nesting depth */
#define XML_MAX_DEPTH 200
#define XML_MAX_ENTITY_REF_LEN 32

typedef struct {
    const char *input;
    size_t len;
    size_t pos;
    int line;
    int col;
    int depth; // Current element nesting depth
} xml_parser;

// The parser is a hand-written recursive descent over a byte buffer.
// `pos` tracks the cursor, `line`/`col` are kept current for error
// messages, and `depth` enforces a maximum nesting depth (S-17).

/// @brief Initialize parser state at the beginning of `input`.
/// @param p Parser state to initialize.
/// @param input Borrowed XML byte buffer.
/// @param len Number of input bytes.
static void parser_init(xml_parser *p, const char *input, size_t len) {
    p->input = input;
    p->len = len;
    p->pos = 0;
    p->line = 1;
    p->col = 1;
    p->depth = 0;
}

/// @brief True if the cursor is past the end of the input buffer.
/// @param p Parser state to inspect.
/// @return `true` when the byte cursor reached the input length.
static bool parser_eof(xml_parser *p) {
    return p->pos >= p->len;
}

/// @brief Return the byte at the cursor without consuming it; '\\0' at EOF.
/// @param p Parser state to inspect.
/// @return Current byte, or the null sentinel at end-of-input.
static char parser_peek(xml_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos];
}

/// @brief Consume and return the byte at the cursor, updating line/col.
///
/// Returns '\\0' at EOF without advancing. Used by every consuming
/// helper so the line/column stay accurate for error messages.
/// @param p Parser state to advance.
/// @return Consumed byte, or the null sentinel at end-of-input.
static char parser_advance(xml_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    char c = p->input[p->pos++];
    if (c == '\n') {
        p->line++;
        p->col = 1;
    } else {
        p->col++;
    }
    return c;
}

/// @brief Advance past any whitespace at the cursor.
/// @param p Parser state to advance using C-library whitespace classification.
static void parser_skip_ws(xml_parser *p) {
    while (!parser_eof(p) && isspace((unsigned char)parser_peek(p)))
        parser_advance(p);
}

/// @brief Consume a literal string if it matches at the cursor; else no-op.
///
/// Returns true and advances past `str` on match, false and leaves the
/// cursor untouched otherwise. The piece-by-piece advance keeps the
/// line/col counters in sync.
/// @param p Parser state to inspect and conditionally advance.
/// @param str Required null-terminated literal.
/// @return `true` on a consumed match, otherwise `false`.
static bool parser_match(xml_parser *p, const char *str) {
    size_t len = strlen(str);
    if (p->pos + len > p->len)
        return false;
    if (strncmp(p->input + p->pos, str, len) != 0)
        return false;
    for (size_t i = 0; i < len; i++)
        parser_advance(p);
    return true;
}

/// @brief Like `parser_match` but does not consume on success.
/// @param p Parser state to inspect.
/// @param str Required null-terminated literal.
/// @return `true` when @p str begins at the cursor.
static bool parser_lookahead(xml_parser *p, const char *str) {
    size_t len = strlen(str);
    if (p->pos + len > p->len)
        return false;
    return strncmp(p->input + p->pos, str, len) == 0;
}

//=============================================================================
// Parsing Helpers
//=============================================================================

/// @brief Whether `c` may begin an XML name.
///
/// ASCII names follow XML's letter / underscore / colon rule; non-ASCII bytes
/// are accepted as part of UTF-8 names so documents using Unicode element and
/// attribute names are not rejected by the byte-oriented parser.
/// @param c Candidate first byte.
/// @return Whether @p c is accepted at the start of a name.
static bool is_name_start_char(char c) {
    unsigned char ch = (unsigned char)c;
    return ch >= 0x80 || isalpha(ch) || c == '_' || c == ':';
}

/// @brief Whether `c` may continue an XML name.
/// @param c Candidate continuation byte.
/// @return Whether @p c is accepted after the first name byte.
static bool is_name_char(char c) {
    unsigned char ch = (unsigned char)c;
    return ch >= 0x80 || isalnum(ch) || c == '_' || c == ':' || c == '-' || c == '.';
}

/// @brief Return true if `name` (an `rt_string`) is a valid XML 1.0 element or attribute name.
/// @details Scans the complete runtime byte length, so embedded null bytes are
///          rejected. Non-ASCII UTF-8 bytes are accepted bytewise.
/// @param name Borrowed runtime string to validate.
/// @return Whether @p name satisfies the engine's XML-name rules.
static bool is_valid_xml_name(rt_string name) {
    int64_t len = name ? rt_str_len(name) : 0;
    if (!name || len <= 0)
        return false;
    const char *s = rt_string_cstr(name);
    if (!s)
        return false;
    // Scan the full runtime byte length so an embedded NUL (forbidden in XML
    // names) rejects the name instead of validating only the prefix before it.
    if (!is_name_start_char(s[0]))
        return false;
    for (int64_t i = 1; i < len; ++i) {
        if (!is_name_char(s[i]))
            return false;
    }
    return true;
}

/// @brief Return true if `needle` appears anywhere within the `len`-byte buffer `s`.
/// @param s Haystack byte buffer.
/// @param len Number of haystack bytes.
/// @param needle Required nonempty null-terminated needle.
/// @return Whether the needle occurs in the byte span.
static bool contains_bytes_n(const char *s, size_t len, const char *needle) {
    if (!s || !needle)
        return false;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > len)
        return false;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(s + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

/// @brief Return true if `s` contains any byte that is illegal in XML 1.0 character data.
///        Legal control characters are TAB (0x09), LF (0x0A), and CR (0x0D).
/// @param s Byte buffer, or null only when @p len is zero.
/// @param len Number of bytes to inspect.
/// @return Whether an illegal C0 control byte is present.
bool contains_invalid_xml_chars(const char *s, size_t len) {
    if (!s && len > 0)
        return true;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D)
            return true;
    }
    return false;
}

/// @brief Return true if every byte in `text` is a legal XML 1.0 character.
///        NULL is treated as an empty string (valid).
/// @param text Borrowed runtime string, or null.
/// @return Whether the byte content passes the XML character check.
static bool is_valid_xml_string(rt_string text) {
    if (!text)
        return true;
    int64_t len = rt_str_len(text);
    if (len < 0)
        return false;
    return !contains_invalid_xml_chars(rt_string_cstr(text), (size_t)len);
}

/// @brief Return true if `codepoint` is a legal XML 1.0 character (per XML 1.0 §2.2).
///        Rejects surrogates (0xD800–0xDFFF) and the non-characters 0xFFFE and 0xFFFF.
/// @param codepoint Unicode scalar candidate.
/// @return Whether XML 1.0 permits the codepoint.
static bool is_valid_xml_char(uint32_t codepoint) {
    return codepoint == 0x9 || codepoint == 0xA || codepoint == 0xD ||
           (codepoint >= 0x20 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

/// @brief Parse an XML name (tag or attribute) into a fresh `rt_string`.
///
/// Reads from `pos` up to the first non-name byte. Returns NULL (and
/// leaves the cursor untouched) if the first byte isn't a valid name
/// start. The returned string is caller-owned.
/// @param p Parser state positioned at a potential name.
/// @return Newly allocated name string, or null without consuming when invalid.
static rt_string parse_name(xml_parser *p) {
    size_t start = p->pos;

    if (parser_eof(p) || !is_name_start_char(parser_peek(p)))
        return NULL;

    while (!parser_eof(p) && is_name_char(parser_peek(p)))
        parser_advance(p);

    size_t len = p->pos - start;
    return rt_string_from_bytes(p->input + start, (int64_t)len);
}

/// @brief Decode a `&...;` entity reference into UTF-8 bytes.
///
/// Handles:
///   - Numeric: `&#NN;` (decimal) and `&#xHHHH;` (hex), encoded as 1-4 UTF-8 bytes.
///   - Named: `&lt; &gt; &amp; &quot; &apos;` only (no full HTML entity table).
/// Returns the number of bytes written to `out` (>=1), with `*consumed`
/// set to how many input bytes were eaten (including the leading `&`
/// and trailing `;`). Returns 0 if the input does not form a valid
/// entity reference, leaving the caller to copy the literal `&`.
///
/// @param str      Input cursor, must point at `&`.
/// @param len      Bytes available from `str`.
/// @param out      Destination buffer (must hold at least 4 bytes).
/// @param consumed Out: bytes consumed from `str` on success.
/// @return Bytes written to `out`, or 0 on parse failure.
int decode_entity(const char *str, size_t len, char *out, size_t *consumed) {
    if (len < 2 || str[0] != '&')
        return 0;

    // Find semicolon within a bounded entity reference length.
    size_t end = 1;
    while (end < len && end <= XML_MAX_ENTITY_REF_LEN && str[end] != ';')
        end++;
    if (end >= len || end > XML_MAX_ENTITY_REF_LEN)
        return 0;

    *consumed = end + 1;

    // Character reference
    if (str[1] == '#') {
        uint32_t codepoint = 0;
        if (len > 2 && str[2] == 'x') {
            if (end == 3)
                return 0;
            // Hex
            for (size_t i = 3; i < end; i++) {
                char c = str[i];
                uint32_t digit;
                if (c >= '0' && c <= '9')
                    digit = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    digit = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    digit = (uint32_t)(c - 'A' + 10);
                else
                    return 0;
                if (codepoint > (0x10FFFFu - digit) / 16u)
                    return 0;
                codepoint = codepoint * 16u + digit;
            }
        } else {
            if (end == 2)
                return 0;
            // Decimal
            for (size_t i = 2; i < end; i++) {
                char c = str[i];
                if (c < '0' || c > '9')
                    return 0;
                uint32_t digit = (uint32_t)(c - '0');
                if (codepoint > (0x10FFFFu - digit) / 10u)
                    return 0;
                codepoint = codepoint * 10u + digit;
            }
        }

        if (!is_valid_xml_char(codepoint))
            return 0;

        // Encode as UTF-8
        if (codepoint < 0x80) {
            out[0] = (char)codepoint;
            return 1;
        } else if (codepoint < 0x800) {
            out[0] = (char)(0xC0 | (codepoint >> 6));
            out[1] = (char)(0x80 | (codepoint & 0x3F));
            return 2;
        } else if (codepoint < 0x10000) {
            out[0] = (char)(0xE0 | (codepoint >> 12));
            out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out[2] = (char)(0x80 | (codepoint & 0x3F));
            return 3;
        } else if (codepoint < 0x110000) {
            out[0] = (char)(0xF0 | (codepoint >> 18));
            out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out[3] = (char)(0x80 | (codepoint & 0x3F));
            return 4;
        }
        return 0;
    }

    // Named entities
    size_t name_len = end - 1;
    if (name_len == 2 && strncmp(str + 1, "lt", 2) == 0) {
        out[0] = '<';
        return 1;
    }
    if (name_len == 2 && strncmp(str + 1, "gt", 2) == 0) {
        out[0] = '>';
        return 1;
    }
    if (name_len == 3 && strncmp(str + 1, "amp", 3) == 0) {
        out[0] = '&';
        return 1;
    }
    if (name_len == 4 && strncmp(str + 1, "quot", 4) == 0) {
        out[0] = '"';
        return 1;
    }
    if (name_len == 4 && strncmp(str + 1, "apos", 4) == 0) {
        out[0] = '\'';
        return 1;
    }

    return 0;
}

/// @brief Parse a single quoted attribute value, decoding entities.
///
/// Accepts either single or double quotes (the opening quote determines
/// the closing). Two-pass: first measures the decoded length so we can
/// allocate exactly, then re-reads to fill the buffer. Returns NULL on
/// allocation failure, unquoted input, malformed entities, forbidden
/// characters, or an unterminated value. Parse failures set the thread-local
/// XML error except for allocation failure and a missing opening quote.
/// @param p Parser positioned at the opening quote.
/// @return Newly allocated decoded value, or null on failure.
static rt_string parse_attr_value(xml_parser *p) {
    char quote = parser_peek(p);
    if (quote != '"' && quote != '\'')
        return NULL;
    parser_advance(p);

    // First pass: calculate decoded length
    size_t start = p->pos;
    size_t decoded_len = 0;
    while (!parser_eof(p) && parser_peek(p) != quote) {
        char c = parser_peek(p);
        if (c == '<') {
            set_error("Invalid '<' in attribute value");
            return NULL;
        }
        if (contains_invalid_xml_chars(&c, 1)) {
            set_error("Invalid XML character in attribute value");
            return NULL;
        }
        if (c == '&') {
            char buf[4];
            size_t consumed;
            int n = decode_entity(p->input + p->pos, p->len - p->pos, buf, &consumed);
            if (n > 0) {
                decoded_len += n;
                p->pos += consumed;
                continue;
            }
            set_error("Invalid XML entity in attribute value");
            return NULL;
        }
        decoded_len++;
        parser_advance(p);
    }

    if (parser_eof(p)) {
        set_error("Unterminated attribute value");
        return NULL;
    }

    // Rewind and decode
    p->pos = start;
    char *buf = malloc(decoded_len + 1);
    if (!buf)
        return NULL;

    size_t out_pos = 0;
    while (!parser_eof(p) && parser_peek(p) != quote) {
        char c = parser_peek(p);
        if (c == '<') {
            free(buf);
            set_error("Invalid '<' in attribute value");
            return NULL;
        }
        if (contains_invalid_xml_chars(&c, 1)) {
            free(buf);
            set_error("Invalid XML character in attribute value");
            return NULL;
        }
        if (c == '&') {
            char decoded[4];
            size_t consumed;
            int n = decode_entity(p->input + p->pos, p->len - p->pos, decoded, &consumed);
            if (n > 0) {
                memcpy(buf + out_pos, decoded, n);
                out_pos += n;
                p->pos += consumed;
                continue;
            }
            free(buf);
            set_error("Invalid XML entity in attribute value");
            return NULL;
        }
        buf[out_pos++] = parser_advance(p);
    }
    buf[out_pos] = '\0';

    // Skip closing quote
    if (!parser_eof(p))
        parser_advance(p);

    rt_string result = rt_string_from_bytes(buf, (int64_t)out_pos);
    free(buf);
    return result;
}

/// @brief Parse element text content (everything up to the next `<`).
///
/// Same two-pass approach as `parse_attr_value`. Returns NULL when the
/// segment is empty (the caller treats that as "no text node here").
/// Whitespace-only content is preserved. Predefined and numeric entity
/// references are decoded; malformed entities and forbidden XML control
/// characters set the thread-local error.
/// @param p Parser positioned at the first character-data byte.
/// @return Newly allocated decoded text, or null for an empty segment or failure.
static rt_string parse_text_content(xml_parser *p) {
    size_t start = p->pos;

    // First pass: find end and calculate decoded length
    size_t decoded_len = 0;
    while (!parser_eof(p) && parser_peek(p) != '<') {
        char c = parser_peek(p);
        if (contains_invalid_xml_chars(&c, 1)) {
            set_error("Invalid XML character in text content");
            return NULL;
        }
        if (c == '&') {
            char buf[4];
            size_t consumed;
            int n = decode_entity(p->input + p->pos, p->len - p->pos, buf, &consumed);
            if (n > 0) {
                decoded_len += n;
                p->pos += consumed;
                continue;
            }
            set_error("Invalid XML entity in text content");
            return NULL;
        }
        decoded_len++;
        parser_advance(p);
    }

    if (decoded_len == 0)
        return NULL;

    // Rewind and decode
    p->pos = start;
    char *buf = malloc(decoded_len + 1);
    if (!buf)
        return NULL;

    size_t out_pos = 0;
    while (!parser_eof(p) && parser_peek(p) != '<') {
        char c = parser_peek(p);
        if (contains_invalid_xml_chars(&c, 1)) {
            free(buf);
            set_error("Invalid XML character in text content");
            return NULL;
        }
        if (c == '&') {
            char decoded[4];
            size_t consumed;
            int n = decode_entity(p->input + p->pos, p->len - p->pos, decoded, &consumed);
            if (n > 0) {
                memcpy(buf + out_pos, decoded, n);
                out_pos += n;
                p->pos += consumed;
                continue;
            }
            free(buf);
            set_error("Invalid XML entity in text content");
            return NULL;
        }
        buf[out_pos++] = parser_advance(p);
    }
    buf[out_pos] = '\0';

    rt_string result = rt_string_from_bytes(buf, (int64_t)out_pos);
    free(buf);
    return result;
}

//=============================================================================
// Element Parsing
//=============================================================================

static void *parse_node(xml_parser *p);

/// @brief Parse a comment node `<!-- ... -->`.
///
/// The body is captured verbatim (no entity decoding inside comments
/// per XML 1.0). Rejects a missing terminator, an internal `--`, a body
/// ending in `-`, and forbidden XML control characters.
/// @param p Parser positioned at the opening `<!--`.
/// @return Newly allocated comment node, or null on mismatch or failure.
static void *parse_comment(xml_parser *p) {
    if (!parser_match(p, "<!--"))
        return NULL;

    size_t start = p->pos;
    while (!parser_eof(p) && !parser_lookahead(p, "-->"))
        parser_advance(p);

    size_t len = p->pos - start;

    if (!parser_match(p, "-->")) {
        set_error("Unterminated comment");
        return NULL;
    }
    bool invalid_comment = len > 0 && p->input[start + len - 1] == '-';
    for (size_t i = 0; !invalid_comment && i + 1 < len; i++) {
        if (p->input[start + i] == '-' && p->input[start + i + 1] == '-')
            invalid_comment = true;
    }
    if (invalid_comment) {
        set_error("Invalid XML comment content");
        return NULL;
    }
    if (contains_invalid_xml_chars(p->input + start, len)) {
        set_error("Invalid XML character in comment");
        return NULL;
    }

    void *node = xml_node_new(XML_NODE_COMMENT);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    n->content = rt_string_from_bytes(p->input + start, (int64_t)len);
    return node;
}

/// @brief Parse a CDATA section `<![CDATA[ ... ]]>`.
///
/// CDATA preserves the body byte-for-byte (no entity decoding) — useful
/// for embedding raw markup or scripts. Reports an error if the
/// closing `]]>` is missing or the body contains a forbidden XML control
/// character.
/// @param p Parser positioned at the opening `<![CDATA[`.
/// @return Newly allocated CDATA node, or null on mismatch or failure.
static void *parse_cdata(xml_parser *p) {
    if (!parser_match(p, "<![CDATA["))
        return NULL;

    size_t start = p->pos;
    while (!parser_eof(p) && !parser_lookahead(p, "]]>"))
        parser_advance(p);

    size_t len = p->pos - start;

    if (!parser_match(p, "]]>")) {
        set_error("Unterminated CDATA section");
        return NULL;
    }
    if (contains_invalid_xml_chars(p->input + start, len)) {
        set_error("Invalid XML character in CDATA section");
        return NULL;
    }

    void *node = xml_node_new(XML_NODE_CDATA);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    n->content = rt_string_from_bytes(p->input + start, (int64_t)len);
    return node;
}

/// @brief Skip past a processing instruction `<?target ... ?>`.
///
/// We don't model PIs as nodes — they're typically just `<?xml ... ?>`
/// declarations or stylesheet hints, neither of which the public API
/// exposes. Returns false (after setting the error) on missing `?>`.
/// @param p Parser positioned at the opening `<?`.
/// @return `true` after consuming a complete instruction; otherwise `false`.
static bool skip_processing_instruction(xml_parser *p) {
    if (!parser_match(p, "<?"))
        return false;

    while (!parser_eof(p) && !parser_lookahead(p, "?>"))
        parser_advance(p);

    if (!parser_match(p, "?>")) {
        set_error("Unterminated processing instruction");
        return false;
    }

    return true;
}

/// @brief Skip past a DOCTYPE declaration `<!DOCTYPE ... >`.
///
/// We don't validate against DTDs, so the contents are discarded.
/// Tracks the square-bracket depth and quoted spans so the terminating `>`
/// of declarations with internal subsets is identified correctly. An
/// unterminated declaration sets the thread-local XML error.
/// @param p Parser positioned at a possible `<!DOCTYPE` declaration.
/// @return `true` after consuming a complete declaration; otherwise `false`.
static bool skip_doctype(xml_parser *p) {
    if (!parser_lookahead(p, "<!DOCTYPE"))
        return false;

    parser_match(p, "<!DOCTYPE");

    int bracket_depth = 0;
    char quote = '\0';
    while (!parser_eof(p)) {
        char c = parser_peek(p);
        if (quote) {
            if (c == quote)
                quote = '\0';
            parser_advance(p);
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            parser_advance(p);
            continue;
        }
        if (c == '[') {
            bracket_depth++;
            parser_advance(p);
            continue;
        }
        if (c == ']' && bracket_depth > 0) {
            bracket_depth--;
            parser_advance(p);
            continue;
        }
        if (c == '>' && bracket_depth == 0) {
            parser_advance(p);
            return true;
        }
        parser_advance(p);
    }

    set_error("Unterminated DOCTYPE declaration");
    return false;
}

/// @brief Parse a complete element, including attributes and children.
///
/// Recursive: children are parsed by `parse_node`, which calls back into
/// `parse_element` for nested tags. `depth` is bumped on entry and
/// restored on every exit path to enforce the 200-deep limit (S-17:
/// blocks pathological recursion attacks). Verifies that the closing
/// `</tag>` matches the opening tag and reports a precise mismatch
/// error otherwise. Self-closing `<tag/>` is supported.
/// @param p Parser positioned at the element's opening `<`.
/// @return Newly allocated element subtree, or null with an error on failure.
static void *parse_element(xml_parser *p) {
    /* S-17: Reject excessively nested documents */
    if (p->depth >= XML_MAX_DEPTH) {
        set_error("element nesting depth limit exceeded");
        return NULL;
    }
    p->depth++;

    if (!parser_match(p, "<")) {
        p->depth--;
        return NULL;
    }

    // Parse tag name
    rt_string tag = parse_name(p);
    if (!tag) {
        p->depth--;
        set_error("Expected element name");
        return NULL;
    }

    void *node = xml_node_new(XML_NODE_ELEMENT);
    if (!node) {
        p->depth--;
        if (rt_obj_release_check0((void *)tag))
            rt_obj_free((void *)tag);
        return NULL;
    }

    xml_node *elem = (xml_node *)node;
    elem->tag = tag;

    // Parse attributes
    for (;;) {
        parser_skip_ws(p);

        // Check for end of opening tag
        if (parser_lookahead(p, "/>")) {
            parser_match(p, "/>");
            p->depth--;
            return node; // Self-closing
        }
        if (parser_lookahead(p, ">")) {
            parser_match(p, ">");
            break; // Continue to content
        }

        // Parse attribute
        rt_string attr_name = parse_name(p);
        if (!attr_name) {
            p->depth--;
            set_error("Expected attribute name or tag end");
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }

        parser_skip_ws(p);
        if (!parser_match(p, "=")) {
            p->depth--;
            set_error("Expected '=' in attribute");
            if (rt_obj_release_check0((void *)attr_name))
                rt_obj_free((void *)attr_name);
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }
        parser_skip_ws(p);

        rt_string attr_value = parse_attr_value(p);
        if (!attr_value) {
            p->depth--;
            if (!has_error())
                set_error("Expected attribute value");
            if (rt_obj_release_check0((void *)attr_name))
                rt_obj_free((void *)attr_name);
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }

        if (rt_map_has(elem->attributes, attr_name)) {
            p->depth--;
            set_error("Duplicate XML attribute");
            if (rt_obj_release_check0((void *)attr_name))
                rt_obj_free((void *)attr_name);
            if (rt_obj_release_check0((void *)attr_value))
                rt_obj_free((void *)attr_value);
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }

        rt_map_set(elem->attributes, attr_name, (void *)attr_value);

        // Release our refs (map retains)
        if (rt_obj_release_check0((void *)attr_name))
            rt_obj_free((void *)attr_name);
        if (rt_obj_release_check0((void *)attr_value))
            rt_obj_free((void *)attr_value);
    }

    // Parse content
    bool closed = false;
    while (!parser_eof(p)) {
        // Check for end tag
        if (parser_lookahead(p, "</")) {
            parser_match(p, "</");
            rt_string end_tag = parse_name(p);
            if (!end_tag) {
                set_error("Expected closing tag name");
                p->depth--;
                if (rt_obj_release_check0(node))
                    rt_obj_free(node);
                return NULL;
            }
            parser_skip_ws(p);
            if (!parser_match(p, ">")) {
                set_error("Expected '>' after closing tag");
                p->depth--;
                if (rt_obj_release_check0((void *)end_tag))
                    rt_obj_free((void *)end_tag);
                if (rt_obj_release_check0(node))
                    rt_obj_free(node);
                return NULL;
            }

            // Verify tag match
            const char *start_tag_str = rt_string_cstr(elem->tag);
            const char *end_tag_str = rt_string_cstr(end_tag);
            if (strcmp(start_tag_str, end_tag_str) != 0) {
                char err[128];
                snprintf(
                    err, sizeof(err), "Mismatched tags: <%s> vs </%s>", start_tag_str, end_tag_str);
                set_error(err);
                p->depth--;
                if (rt_obj_release_check0((void *)end_tag))
                    rt_obj_free((void *)end_tag);
                if (rt_obj_release_check0(node))
                    rt_obj_free(node);
                return NULL;
            }

            if (rt_obj_release_check0((void *)end_tag))
                rt_obj_free((void *)end_tag);
            closed = true;
            break;
        }

        // Parse child node
        void *child = parse_node(p);
        if (child) {
            xml_node *child_node = (xml_node *)child;
            child_node->parent = elem;
            if (!xml_seq_push_checked(elem->children, child, "XML child append failed")) {
                child_node->parent = NULL;
                if (rt_obj_release_check0(child))
                    rt_obj_free(child);
                p->depth--;
                if (rt_obj_release_check0(node))
                    rt_obj_free(node);
                return NULL;
            }
            // Ownership transferred to elem; its finalizer will release child
        } else if (xml_last_error[0] != '\0') {
            // Parse error occurred
            p->depth--;
            if (rt_obj_release_check0(node))
                rt_obj_free(node);
            return NULL;
        }
    }

    if (!closed) {
        char err[128];
        snprintf(err, sizeof(err), "Missing closing tag for <%s>", rt_string_cstr(elem->tag));
        set_error(err);
        p->depth--;
        if (rt_obj_release_check0(node))
            rt_obj_free(node);
        return NULL;
    }

    p->depth--;
    return node;
}

/// @brief Dispatch to the appropriate parse routine based on the cursor.
///
/// Peeks at the upcoming bytes to decide between comment, CDATA, processing
/// instruction, DOCTYPE, element, and text. Instructions and declarations are
/// consumed without producing nodes, then dispatch resumes recursively.
/// Whitespace-only text nodes are preserved because they are XML character
/// data. Returns NULL at EOF or on failure; a real parse error is signalled via
/// `xml_last_error[0] != 0`.
/// @param p Parser positioned at the next top-level or child construct.
/// @return Newly allocated node, or null at EOF, after skipped markup, or on error.
static void *parse_node(xml_parser *p) {
    if (parser_eof(p))
        return NULL;

    // Comment
    if (parser_lookahead(p, "<!--"))
        return parse_comment(p);

    // CDATA
    if (parser_lookahead(p, "<![CDATA["))
        return parse_cdata(p);

    // Processing instruction (skip)
    if (parser_lookahead(p, "<?")) {
        skip_processing_instruction(p);
        return parse_node(p);
    }

    // DOCTYPE (skip)
    if (parser_lookahead(p, "<!DOCTYPE")) {
        if (!skip_doctype(p))
            return NULL;
        return parse_node(p);
    }

    if (parser_lookahead(p, "</")) {
        set_error("Unexpected closing tag");
        return NULL;
    }

    // Element
    if (parser_lookahead(p, "<") && !parser_lookahead(p, "</"))
        return parse_element(p);

    if (parser_lookahead(p, "<")) {
        set_error("Invalid XML markup");
        return NULL;
    }

    // Text content
    rt_string text = parse_text_content(p);
    if (text) {
        void *node = xml_node_new(XML_NODE_TEXT);
        if (!node) {
            if (rt_obj_release_check0((void *)text))
                rt_obj_free((void *)text);
            return NULL;
        }
        xml_node *n = (xml_node *)node;
        n->content = text;
        return node;
    }

    return NULL;
}

/// @brief Parse a complete XML document into a tree.
///
/// Builds a `XML_NODE_DOCUMENT` root and pushes every parsed top-level
/// node (typically one element plus optional comments / PIs) onto its
/// children seq. Returns NULL on parse failure, with the cause in
/// `xml_last_error`.
///
/// @param input UTF-8 XML source.
/// @param len   Length of `input` in bytes.
/// @return Owned document node, or NULL on failure.
static void *parse_document(const char *input, size_t len) {
    clear_error();

    xml_parser p;
    parser_init(&p, input, len);

    void *doc = xml_node_new(XML_NODE_DOCUMENT);
    if (!doc)
        return NULL;

    xml_node *doc_node = (xml_node *)doc;
    int root_count = 0;

    // Parse all root-level nodes
    while (!parser_eof(&p)) {
        parser_skip_ws(&p);
        if (parser_eof(&p))
            break;

        void *node = parse_node(&p);
        if (node) {
            xml_node *n = (xml_node *)node;
            if (n->type == XML_NODE_TEXT) {
                set_error("Text is not allowed outside the document element");
                if (rt_obj_release_check0(node))
                    rt_obj_free(node);
                if (rt_obj_release_check0(doc))
                    rt_obj_free(doc);
                return NULL;
            }
            if (n->type == XML_NODE_ELEMENT) {
                root_count++;
                if (root_count > 1) {
                    set_error("XML document must contain exactly one root element");
                    if (rt_obj_release_check0(node))
                        rt_obj_free(node);
                    if (rt_obj_release_check0(doc))
                        rt_obj_free(doc);
                    return NULL;
                }
            }
            n->parent = doc_node;
            rt_seq_push(doc_node->children, node);
            // Ownership transferred to doc; its finalizer will release node
        } else if (xml_last_error[0] != '\0') {
            // Parse error
            if (rt_obj_release_check0(doc))
                rt_obj_free(doc);
            return NULL;
        }
    }

    if (root_count != 1) {
        set_error("XML document must contain exactly one root element");
        if (rt_obj_release_check0(doc))
            rt_obj_free(doc);
        return NULL;
    }

    return doc;
}

//=============================================================================
// Public API - Parsing
//=============================================================================

/// @brief `Xml.Parse(text)` — parse XML source into a document tree.
///
/// Returns NULL on empty input or any parse error; call `Xml.Error()`
/// to retrieve a human-readable reason. Does not validate against any
/// schema or DTD.
///
/// @param text UTF-8 XML source.
/// @return Owned document node, or NULL on failure.
void *rt_xml_parse(rt_string text) {
    clear_error();
    const char *cstr = NULL;
    size_t len = 0;
    if (!xml_string_view_nonempty(text, "Empty XML input", &cstr, &len))
        return NULL;

    return parse_document(cstr, len);
}

/// @brief `Xml.ParseResult(text)` — parse XML source into a Result.
///
/// Successful parses return `Ok(document)`. Malformed or empty input returns
/// `Err(message)` with the same diagnostic text exposed by `Xml.Error()`.
///
/// @param text UTF-8 XML source.
/// @return Owned `Zanna.Result` carrying either the document node or an error string.
void *rt_xml_parse_result(rt_string text) {
    void *doc = rt_xml_parse(text);
    if (!doc) {
        rt_string err = rt_xml_error();
        if (!err || rt_str_len(err) == 0) {
            rt_str_release_maybe(err);
            return rt_result_err_str(rt_const_cstr("XML parse error"));
        }
        void *result = rt_result_err_str(err);
        rt_str_release_maybe(err);
        return result;
    }

    void *result = rt_result_ok(doc);
    xml_release_temp_object(doc);
    return result;
}

/// @brief `Xml.Error()` — return the last parse error message on this thread.
///
/// Empty when the most recent parse succeeded. Errors are thread-local
/// so concurrent parses don't clobber each other's diagnostics.
///
/// @return Owned `rt_string` with the error text, or "" if none.
rt_string rt_xml_error(void) {
    return rt_string_from_bytes(xml_last_error, strlen(xml_last_error));
}

/// @brief Test whether an opaque object is a live XML node handle.
/// @param node Candidate runtime object; null is permitted.
/// @return 1 for a `Zanna.Data.Xml` node of the expected size, otherwise 0.
int8_t rt_xml_is_node(void *node) {
    return rt_obj_is_instance(node, RT_XML_NODE_CLASS_ID, sizeof(xml_node)) ? 1 : 0;
}

/// @brief `Xml.IsValid(text)` — boolean parse-success probe.
///
/// Internally runs `Parse` and discards the result. This checks acceptance by
/// this parser's practical XML subset, NOT full XML 1.0 well-formedness: DTD
/// contents are skipped (declared entities are not expanded), only the five
/// predefined named entities plus numeric references are decoded, and
/// namespaces/UTF-8 byte sequences are not validated.
///
/// @param text Candidate XML.
/// @return 1 if it parses, 0 otherwise.
int8_t rt_xml_is_valid(rt_string text) {
    void *doc = rt_xml_parse(text);
    if (doc) {
        if (rt_obj_release_check0(doc))
            rt_obj_free(doc);
        return 1;
    }
    return 0;
}

//=============================================================================
// Public API - Node Creation
//=============================================================================

/// @brief `Xml.Element(tag)` — create a fresh element node.
///
/// The element starts with no attributes and no children. The tag
/// string is retained, so the caller can release its own reference
/// after the call.
///
/// @param tag Element tag name.
/// @return Owned element node, or NULL on OOM.
void *rt_xml_element(rt_string tag) {
    clear_error();
    if (!is_valid_xml_name(tag)) {
        set_error("Invalid XML element name");
        return NULL;
    }

    void *node = xml_node_new(XML_NODE_ELEMENT);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    rt_obj_retain_maybe((void *)tag);
    n->tag = tag;
    return node;
}

/// @brief `Xml.Text(content)` — create a text node.
///
/// Used for `<elem>foo</elem>` -style content. Use `Xml.Cdata` instead
/// if the body should bypass entity escaping during formatting.
///
/// @param content Text body (NULL produces an empty text node).
/// @return Owned text node.
void *rt_xml_text(rt_string content) {
    clear_error();
    if (!is_valid_xml_string(content)) {
        set_error("Invalid XML character in text content");
        return NULL;
    }

    void *node = xml_node_new(XML_NODE_TEXT);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    if (content)
        rt_obj_retain_maybe((void *)content);
    n->content = content;
    return node;
}

/// @brief `Xml.Comment(content)` — create a comment node.
///
/// Formats as `<!-- content -->`. The body is emitted verbatim;
/// callers must avoid embedding `--` inside (XML 1.0 forbids it).
///
/// @param content Comment text.
/// @return Owned comment node.
void *rt_xml_comment(rt_string content) {
    clear_error();
    const char *s = NULL;
    size_t len = 0;
    if (!xml_optional_string_view(content, &s, &len))
        return NULL;
    if (content) {
        if (contains_bytes_n(s, len, "--") || (len > 0 && s[len - 1] == '-')) {
            set_error("Invalid XML comment content");
            return NULL;
        }
        if (contains_invalid_xml_chars(s, len)) {
            set_error("Invalid XML character in comment");
            return NULL;
        }
    }

    void *node = xml_node_new(XML_NODE_COMMENT);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    if (content)
        rt_obj_retain_maybe((void *)content);
    n->content = content;
    return node;
}

/// @brief `Xml.Cdata(content)` — create a CDATA section node.
///
/// Formats as `<![CDATA[content]]>` with no entity escaping. Useful
/// for embedding markup, code, or other content that would otherwise
/// require heavy escaping.
///
/// @param content Raw CDATA body.
/// @return Owned CDATA node.
void *rt_xml_cdata(rt_string content) {
    clear_error();
    const char *s = NULL;
    size_t len = 0;
    if (!xml_optional_string_view(content, &s, &len))
        return NULL;
    if (content) {
        if (contains_bytes_n(s, len, "]]>")) {
            set_error("Invalid CDATA content");
            return NULL;
        }
        if (contains_invalid_xml_chars(s, len)) {
            set_error("Invalid XML character in CDATA section");
            return NULL;
        }
    }

    void *node = xml_node_new(XML_NODE_CDATA);
    if (!node)
        return NULL;

    xml_node *n = (xml_node *)node;
    if (content)
        rt_obj_retain_maybe((void *)content);
    n->content = content;
    return node;
}

//=============================================================================
// Public API - Node Properties
//=============================================================================

/// @brief `XmlNode.NodeType` — return the kind enum (element/text/etc).
///
/// Values match the `rt_xml_node_type_t` enum (1=element, 2=text,
/// 3=comment, 4=cdata, 5=document). Returns 0 for NULL.
/// @param node XML node to inspect.
/// @return Node-kind value, or 0 when @p node is not an XML node.
int64_t rt_xml_node_type(void *node) {
    if (!rt_xml_is_node(node))
        return 0;
    xml_node *n = (xml_node *)node;
    return (int64_t)n->type;
}

/// @brief `XmlNode.Tag` — return the element tag name.
///
/// Returns "" for non-element nodes (text, comment, CDATA, document).
/// The returned string is retained and owned by the caller.
/// @param node XML node to inspect.
/// @return Owned tag string, or an owned empty string when unavailable.
rt_string rt_xml_tag(void *node) {
    if (!rt_xml_is_node(node))
        return rt_str_empty();
    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->tag)
        return rt_str_empty();
    rt_obj_retain_maybe((void *)n->tag);
    return n->tag;
}

/// @brief `XmlNode.Content` — return the raw `content` string slot.
///
/// Populated for text, comment, and CDATA nodes; "" for elements and
/// documents (use `TextContent` to recursively gather descendant text).
/// @param node XML node to inspect.
/// @return Owned content string, or an owned empty string when unavailable.
rt_string rt_xml_content(void *node) {
    if (!rt_xml_is_node(node))
        return rt_str_empty();
    xml_node *n = (xml_node *)node;
    if (!n->content)
        return rt_str_empty();
    rt_obj_retain_maybe((void *)n->content);
    return n->content;
}

/// @brief Walk the tree and append every descendant text/CDATA into `sb`.
///
/// Optimization O-04: building one growing builder is O(n) total versus
/// the O(n²) you'd get with naive `result = result + child.text` chains.
/// Iteratively traverses element and document nodes in document order;
/// comments and other node kinds contribute no content. Borrowed references
/// are used throughout because children remain owned by their parents.
/// @param node Root of the subtree to traverse.
/// @param sb Initialized destination builder.
/// @return 1 when collection completed, 0 when a builder append failed.
static int collect_text_content(void *node, rt_string_builder *sb) {
    if (!rt_xml_is_node(node))
        return 1;

    void **stack = NULL;
    size_t stack_count = 0;
    size_t stack_cap = 0;
    if (!xml_traversal_stack_push(&stack, &stack_count, &stack_cap, node))
        return 0;

    while (stack_count > 0) {
        void *current = stack[--stack_count];
        if (!rt_xml_is_node(current))
            continue;

        xml_node *n = (xml_node *)current;
        if (n->type == XML_NODE_TEXT || n->type == XML_NODE_CDATA) {
            if (n->content) {
                const char *cstr = rt_string_cstr(n->content);
                if (cstr && rt_sb_append_cstr(sb, cstr) != RT_SB_OK) {
                    free(stack);
                    return 0;
                }
            }
            continue;
        }

        if ((n->type != XML_NODE_ELEMENT && n->type != XML_NODE_DOCUMENT) || !n->children)
            continue;

        int64_t child_count = rt_seq_len(n->children);
        for (int64_t i = child_count; i > 0; i--) {
            void *child = rt_seq_get(n->children, i - 1);
            if (!xml_traversal_stack_push(&stack, &stack_count, &stack_cap, child)) {
                free(stack);
                return 0;
            }
            // rt_seq_get returns a borrowed reference — do not release
        }
    }
    free(stack);
    return 1;
}

/// @brief `XmlNode.TextContent` — concatenated text of this node and descendants.
///
/// For text/CDATA nodes returns the content directly. For element /
/// document nodes, iteratively concatenates every descendant text +
/// CDATA into a single string via the builder helper. Returns "" for
/// nodes with no textual content.
/// @param node XML node whose textual value is requested.
/// @return Owned concatenated string; empty for invalid or nontextual nodes.
rt_string rt_xml_text_content(void *node) {
    if (!rt_xml_is_node(node))
        return rt_str_empty();

    xml_node *n = (xml_node *)node;

    // For text/cdata nodes, return content directly
    if (n->type == XML_NODE_TEXT || n->type == XML_NODE_CDATA) {
        if (n->content) {
            rt_obj_retain_maybe((void *)n->content);
            return n->content;
        }
        return rt_str_empty();
    }

    // For elements/documents, gather all text content using a builder (O(n))
    if (n->type != XML_NODE_ELEMENT && n->type != XML_NODE_DOCUMENT)
        return rt_str_empty();

    if (!n->children)
        return rt_str_empty();

    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!collect_text_content(node, &sb)) {
        rt_sb_free(&sb);
        return rt_str_empty();
    }
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

//=============================================================================
// Public API - Attributes
//=============================================================================

/// @brief `XmlNode.Attr(name)` — get an attribute value, or "" if missing.
///
/// Case-sensitive (XML attributes are). Returns "" for non-element
/// nodes or unknown attributes; the empty-string return is
/// indistinguishable from an explicitly empty attribute, so use
/// `HasAttr` if you need to disambiguate.
/// @param node Element node to query.
/// @param name Case-sensitive attribute name.
/// @return Owned attribute value, or an owned empty string when absent.
rt_string rt_xml_attr(void *node, rt_string name) {
    if (!rt_xml_is_node(node) || !name)
        return rt_str_empty();

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->attributes)
        return rt_str_empty();

    void *value = rt_map_get(n->attributes, name);
    if (!value)
        return rt_str_empty();

    rt_obj_retain_maybe(value);
    return (rt_string)value;
}

/// @brief `XmlNode.HasAttr(name)` — test whether an attribute is present.
///
/// Distinguishes "attribute exists with empty value" from "attribute
/// absent". Returns 0 for non-element nodes.
/// @param node Element node to query.
/// @param name Case-sensitive attribute name.
/// @return 1 when the attribute exists, otherwise 0.
int8_t rt_xml_has_attr(void *node, rt_string name) {
    if (!rt_xml_is_node(node) || !name)
        return 0;

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->attributes)
        return 0;

    return rt_map_has(n->attributes, name);
}

/// @brief `XmlNode.SetAttr(name, value)` — set / overwrite an attribute.
///
/// Silently no-ops on non-element nodes or a null name. A null value is
/// stored as an empty string. Invalid names or forbidden value characters
/// set the thread-local XML error; existing values are replaced.
/// @param node Element node to mutate.
/// @param name Valid XML attribute name.
/// @param value Attribute value, or null to store an empty value.
void rt_xml_set_attr(void *node, rt_string name, rt_string value) {
    clear_error();
    if (!rt_xml_is_node(node) || !name)
        return;
    if (!is_valid_xml_name(name)) {
        set_error("Invalid XML attribute name");
        return;
    }
    if (!is_valid_xml_string(value)) {
        set_error("Invalid XML character in attribute value");
        return;
    }

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->attributes)
        return;

    rt_string stored = value ? value : rt_str_empty();
    rt_map_set(n->attributes, name, (void *)stored);
    if (!value && rt_obj_release_check0((void *)stored))
        rt_obj_free((void *)stored);
}

/// @brief `XmlNode.RemoveAttr(name)` — drop an attribute.
///
/// Non-element nodes, null names, and absent attributes are harmless no-ops.
/// @param node Element node to mutate.
/// @param name Case-sensitive attribute name.
/// @return 1 if the attribute was present and removed, 0 otherwise.
int8_t rt_xml_remove_attr(void *node, rt_string name) {
    if (!rt_xml_is_node(node) || !name)
        return 0;

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->attributes)
        return 0;

    return rt_map_remove(n->attributes, name);
}

/// @brief `XmlNode.AttrNames` — list the attribute names of an element.
///
/// Returns an owned `seq<str>`, always — empty for non-element or
/// attribute-less nodes. Order is the underlying map's iteration
/// order (effectively insertion order).
/// @param node Element node to inspect.
/// @return Owned sequence containing owned attribute-name strings.
void *rt_xml_attr_names(void *node) {
    if (!rt_xml_is_node(node))
        return rt_seq_new();

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT || !n->attributes)
        return rt_seq_new();

    return rt_map_keys(n->attributes);
}

//=============================================================================
// Public API - Children
//=============================================================================

/// @brief `XmlNode.Children` — return a snapshot seq of child nodes.
///
/// Returns a *copy* of the internal seq so the caller can iterate or
/// mutate it without affecting the parent. The result seq retains the
/// children it contains; release the seq when finished.
/// @param node XML node whose direct children are requested.
/// @return Owned child sequence, empty for invalid or childless nodes, or null
///         when the sequence itself cannot be allocated.
void *rt_xml_children(void *node) {
    void *copy = rt_seq_new();
    if (!copy) {
        set_error("XML children allocation failed");
        return NULL;
    }
    rt_seq_set_owns_elements(copy, 1);
    if (!rt_xml_is_node(node))
        return copy;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return copy;

    // Return a copy of the children seq
    int64_t count = rt_seq_len(n->children);
    for (int64_t i = 0; i < count; i++) {
        void *child = rt_seq_get(n->children, i);
        if (!xml_seq_push_checked(copy, child, "XML children copy failed"))
            return copy;
    }
    return copy;
}

/// @brief Return the number of direct children of an XML node.
/// @param node XML node to inspect.
/// @return Direct-child count, or zero for invalid or childless nodes.
int64_t rt_xml_child_count(void *node) {
    if (!rt_xml_is_node(node))
        return 0;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return 0;

    return rt_seq_len(n->children);
}

/// @brief `XmlNode.ChildAt(index)` — borrowed reference to the Nth child.
///
/// Returns NULL for negative or out-of-range indices. The reference
/// is borrowed: the parent retains ownership, do not release.
/// @param node XML node to inspect.
/// @param index Zero-based child index.
/// @return Borrowed child node, or null when the index or receiver is invalid.
void *rt_xml_child_at(void *node, int64_t index) {
    if (!rt_xml_is_node(node) || index < 0)
        return NULL;

    xml_node *n = (xml_node *)node;
    if (!n->children || index >= rt_seq_len(n->children))
        return NULL;

    return rt_seq_get(n->children, index);
}

/// @brief `XmlNode.Child(tag)` — first direct-child element with that tag.
///
/// Linear scan over the children seq, returning the first element
/// whose tag matches exactly. Returns NULL if no match. Borrowed
/// reference (do not release).
/// @param node XML node whose direct children are searched.
/// @param tag Case-sensitive element tag.
/// @return Borrowed first matching element, or null when absent.
void *rt_xml_child(void *node, rt_string tag) {
    if (!rt_xml_is_node(node) || !tag)
        return NULL;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return NULL;

    const char *target = rt_string_cstr(tag);
    int64_t count = rt_seq_len(n->children);

    for (int64_t i = 0; i < count; i++) {
        void *child = rt_seq_get(n->children, i);
        xml_node *cn = (xml_node *)child;

        if (cn->type == XML_NODE_ELEMENT && cn->tag) {
            const char *child_tag = rt_string_cstr(cn->tag);
            if (strcmp(child_tag, target) == 0)
                return child; // Borrowed reference — caller must not release
        }
        // Do not release — children are owned by parent node
    }

    return NULL;
}

/// @brief `XmlNode.ChildrenByTag(tag)` — direct children matching `tag`.
///
/// Like `Child(tag)` but returns *all* matches in document order. Only
/// looks one level deep — for recursive search use `FindAll`. The
/// result seq retains the returned child nodes.
/// @param node XML node whose direct children are searched.
/// @param tag Case-sensitive element tag.
/// @return Owned sequence of matching elements, empty when none match.
void *rt_xml_children_by_tag(void *node, rt_string tag) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);
    if (!rt_xml_is_node(node) || !tag)
        return result;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return result;

    const char *target = rt_string_cstr(tag);
    int64_t count = rt_seq_len(n->children);

    for (int64_t i = 0; i < count; i++) {
        void *child = rt_seq_get(n->children, i);
        xml_node *cn = (xml_node *)child;

        if (cn->type == XML_NODE_ELEMENT && cn->tag) {
            const char *child_tag = rt_string_cstr(cn->tag);
            if (strcmp(child_tag, target) == 0) {
                rt_seq_push(result, child);
            }
        }
        // Do not release — children are owned by parent node
    }

    return result;
}

/// @brief `XmlNode.Append(child)` — add `child` to the end of `node.children`.
///
/// Sets the child's `parent` weak pointer and retains the child for
/// the parent, so callers may release their local reference afterward. A
/// document child, an ancestor that would introduce a cycle, or a child owned
/// by another parent is rejected with an XML error. Re-appending a child
/// already owned by @p node is a no-op.
/// @param node Parent node with a child sequence.
/// @param child Detached non-document XML node to append.
void rt_xml_append(void *node, void *child) {
    clear_error();
    if (!rt_xml_is_node(node) || !rt_xml_is_node(child) || node == child)
        return;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return;

    xml_node *cn = (xml_node *)child;
    if (cn->type == XML_NODE_DOCUMENT) {
        set_error("Cannot append an XML document as a child");
        return;
    }
    for (xml_node *p = n; p; p = p->parent) {
        if (p == cn) {
            set_error("Cannot create a cycle in XML tree");
            return;
        }
    }
    if (cn->parent) {
        if (cn->parent == n)
            return;
        set_error("XML child already has a parent");
        return;
    }
    cn->parent = n;

    rt_obj_retain_maybe(child);
    rt_seq_push(n->children, child);
}

/// @brief `XmlNode.Insert(index, child)` — splice `child` at position `index`.
///
/// Indices past the end clamp to "end of list" (effectively `Append`).
/// Negative indices are rejected as a silent no-op. The same document,
/// cycle, and existing-parent restrictions as @ref rt_xml_append apply.
/// @param node Parent node with a child sequence.
/// @param index Zero-based insertion index.
/// @param child Detached non-document XML node to insert.
void rt_xml_insert(void *node, int64_t index, void *child) {
    clear_error();
    if (!rt_xml_is_node(node) || !rt_xml_is_node(child) || node == child || index < 0)
        return;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return;

    if (index > rt_seq_len(n->children))
        index = rt_seq_len(n->children);

    xml_node *cn = (xml_node *)child;
    if (cn->type == XML_NODE_DOCUMENT) {
        set_error("Cannot insert an XML document as a child");
        return;
    }
    for (xml_node *p = n; p; p = p->parent) {
        if (p == cn) {
            set_error("Cannot create a cycle in XML tree");
            return;
        }
    }
    if (cn->parent) {
        if (cn->parent == n)
            return;
        set_error("XML child already has a parent");
        return;
    }
    cn->parent = n;

    rt_obj_retain_maybe(child);
    rt_seq_insert(n->children, index, child);
}

/// @brief `XmlNode.Remove(child)` — detach `child` by identity.
///
/// Returns 0 if `child` isn't found in `node.children`. On success,
/// clears the child's `parent` pointer and releases the parent's
/// reference (the child is freed if there are no other holders).
/// @param node Parent node to mutate.
/// @param child Direct child to locate by object identity.
/// @return 1 after removal, or 0 when the inputs are invalid or no match exists.
int8_t rt_xml_remove(void *node, void *child) {
    if (!rt_xml_is_node(node) || !rt_xml_is_node(child))
        return 0;

    xml_node *n = (xml_node *)node;
    if (!n->children)
        return 0;

    int64_t idx = rt_seq_find(n->children, child);
    if (idx < 0)
        return 0;

    xml_node *cn = (xml_node *)child;
    cn->parent = NULL;

    void *removed = rt_seq_remove(n->children, idx);
    if (removed) {
        if (rt_obj_release_check0(removed))
            rt_obj_free(removed);
    }
    return 1;
}

/// @brief `XmlNode.RemoveAt(index)` — detach the child at `index`.
///
/// Silent no-op for out-of-range or negative indices. Same ownership
/// transfer as `Remove`: the parent's reference is released after the
/// child has been pulled out of the seq.
/// @param node Parent node to mutate.
/// @param index Zero-based child index.
void rt_xml_remove_at(void *node, int64_t index) {
    if (!rt_xml_is_node(node) || index < 0)
        return;

    xml_node *n = (xml_node *)node;
    if (!n->children || index >= rt_seq_len(n->children))
        return;

    void *child = rt_seq_get(n->children, index);
    if (child) {
        xml_node *cn = (xml_node *)child;
        cn->parent = NULL;
        // Don't release here — ownership stays until seq_remove
    }

    void *removed = rt_seq_remove(n->children, index);
    if (removed) {
        // Parent relinquishes ownership: release the child now
        if (rt_obj_release_check0(removed))
            rt_obj_free(removed);
    }
}

/// @brief `XmlElement.SetText(text)` — replace all children with one text node.
///
/// Drains the element's existing children, then (if `text` is non-empty)
/// appends a single text node. Useful for `<tag>some-value</tag>`-style
/// updates where the element should hold only its text. No-op for
/// non-element receivers. Forbidden XML control characters leave the existing
/// children untouched and set the thread-local XML error.
/// @param node Element whose children are replaced.
/// @param text New textual content; null or empty leaves the element childless.
void rt_xml_set_text(void *node, rt_string text) {
    clear_error();
    if (!rt_xml_is_node(node))
        return;
    if (!is_valid_xml_string(text)) {
        set_error("Invalid XML character in text content");
        return;
    }

    xml_node *n = (xml_node *)node;
    if (n->type != XML_NODE_ELEMENT)
        return;

    // Clear existing children
    if (n->children) {
        while (rt_seq_len(n->children) > 0) {
            void *removed = rt_seq_remove(n->children, 0);
            if (removed) {
                xml_node *rn = (xml_node *)removed;
                rn->parent = NULL;
                if (rt_obj_release_check0(removed))
                    rt_obj_free(removed);
            }
        }
    }

    // Add text node
    if (text && rt_str_len(text) > 0) {
        void *text_node = rt_xml_text(text);
        if (text_node) {
            xml_node *tn = (xml_node *)text_node;
            tn->parent = n;
            rt_seq_push(n->children, text_node);
            // Ownership transferred to n; its finalizer will release text_node
        }
    }
}

//=============================================================================
// Public API - Navigation
//=============================================================================

/// @brief `XmlNode.Parent` — retained reference to the containing node, or NULL.
///
/// Returns NULL for detached nodes and the document root. Bumps the
/// parent's refcount before returning so it survives at least until
/// the caller releases it.
/// @param node XML node whose parent is requested.
/// @return Retained parent node, or null when detached or invalid.
void *rt_xml_parent(void *node) {
    if (!rt_xml_is_node(node))
        return NULL;

    xml_node *n = (xml_node *)node;
    if (!n->parent)
        return NULL;

    rt_obj_retain_maybe(n->parent);
    return n->parent;
}

/// @brief `XmlDocument.Root` — first element child of the document.
///
/// XML 1.0 mandates a single root element; this helper finds it
/// while skipping comment nodes. If passed a descendant, the helper first
/// climbs to its topmost ancestor; a topmost element is returned directly.
/// The result is a borrowed reference owned by its tree.
/// @param doc Document or descendant node whose tree root is requested.
/// @return Borrowed document element, or null if the tree has none.
void *rt_xml_root(void *doc) {
    if (!rt_xml_is_node(doc))
        return NULL;

    xml_node *n = (xml_node *)doc;
    while (n->parent)
        n = n->parent;
    if (n->type == XML_NODE_ELEMENT)
        return n;
    if (n->type != XML_NODE_DOCUMENT || !n->children)
        return NULL;

    // Find first element child
    int64_t count = rt_seq_len(n->children);
    for (int64_t i = 0; i < count; i++) {
        void *child = rt_seq_get(n->children, i);
        xml_node *cn = (xml_node *)child;
        if (cn->type == XML_NODE_ELEMENT)
            return child; // Borrowed reference — parent owns it
        // Do not release — children are owned by parent node
    }

    return NULL;
}

/// @brief Iterative DFS helper for `FindAll` — push every element matching `tag`.
///
/// Each match is retained before being pushed so the result seq holds
/// strong refs (callers can outlive the source tree). Children are pushed in
/// reverse order so the LIFO stack still emits document-order results.
/// @param node Root of the subtree to traverse.
/// @param tag Null-terminated case-sensitive tag to match.
/// @param result Owning result sequence that receives matches.
static void find_all_iterative(void *node, const char *tag, void *result) {
    if (!rt_xml_is_node(node))
        return;
    xml_node_stack stack = {0};
    if (!xml_node_stack_push(&stack, node, "Xml.FindAll: traversal stack allocation failed"))
        return;
    while (stack.len > 0) {
        void *cur = stack.items[--stack.len];
        xml_node *n = (xml_node *)cur;
        if (n->type == XML_NODE_ELEMENT && n->tag) {
            const char *node_tag = rt_string_cstr(n->tag);
            if (strcmp(node_tag, tag) == 0)
                rt_seq_push(result, cur);
        }
        if (n->children) {
            int64_t count = rt_seq_len(n->children);
            for (int64_t i = count; i > 0; i--) {
                if (!xml_node_stack_push(&stack,
                                         rt_seq_get(n->children, i - 1),
                                         "Xml.FindAll: traversal stack allocation failed")) {
                    xml_node_stack_dispose(&stack);
                    return;
                }
            }
        }
    }
    xml_node_stack_dispose(&stack);
}

/// @brief Advance a path pointer past any leading slash separators.
/// @param path Null-terminated path pointer, or null.
/// @return First non-slash position, the terminating null byte, or null.
static const char *skip_path_separators(const char *path) {
    while (path && *path == '/')
        path++;
    return path;
}

/// @brief Locate the end of the current slash-delimited path segment.
/// @param path Nonnull pointer to the segment's first byte.
/// @return Pointer to the following slash or terminating null byte.
static const char *path_segment_end(const char *path) {
    const char *p = path;
    while (*p && *p != '/')
        p++;
    return p;
}

/// @brief Compare an element tag with a non-null-terminated path segment.
/// @param node Candidate XML node.
/// @param seg First byte of the requested segment.
/// @param seg_len Segment length in bytes.
/// @return `true` only for an element with an exactly equal tag.
static bool element_tag_matches_segment(xml_node *node, const char *seg, size_t seg_len) {
    if (!node || node->type != XML_NODE_ELEMENT || !node->tag)
        return false;
    const char *tag = rt_string_cstr(node->tag);
    return strlen(tag) == seg_len && strncmp(tag, seg, seg_len) == 0;
}

/// @brief Recursive helper for slash-path FindAll: walks `path` one segment at a time,
///        pushing matching nodes into `result`. Handles both the "this node is the match"
///        and "descend into children" cases.
/// @details Leading and repeated separators are ignored. Traversal is limited
///          to @c XML_MAX_DEPTH; exceeding the limit sets an XML error and traps.
/// @param node Current subtree node.
/// @param path Remaining null-terminated slash path.
/// @param result Owning result sequence that receives matches.
/// @param depth Number of path levels traversed so far.
static void find_path_all_recursive(void *node, const char *path, void *result, int depth) {
    if (!rt_xml_is_node(node))
        return;
    if (depth >= XML_MAX_DEPTH) {
        set_error("XML path search depth limit exceeded");
        rt_trap("Xml.FindAll: path search depth limit exceeded");
        return;
    }

    path = skip_path_separators(path);
    if (!path || *path == '\0') {
        rt_seq_push(result, node);
        return;
    }

    const char *seg_end = path_segment_end(path);
    size_t seg_len = (size_t)(seg_end - path);
    const char *rest = skip_path_separators(seg_end);

    xml_node *n = (xml_node *)node;
    if (element_tag_matches_segment(n, path, seg_len)) {
        if (!rest || *rest == '\0') {
            rt_seq_push(result, node);
            return;
        }
        if (n->children) {
            int64_t count = rt_seq_len(n->children);
            for (int64_t i = 0; i < count; i++)
                find_path_all_recursive(rt_seq_get(n->children, i), rest, result, depth + 1);
        }
        return;
    }

    if (n->children) {
        int64_t count = rt_seq_len(n->children);
        for (int64_t i = 0; i < count; i++) {
            void *child = rt_seq_get(n->children, i);
            xml_node *cn = (xml_node *)child;
            if (element_tag_matches_segment(cn, path, seg_len)) {
                if (!rest || *rest == '\0')
                    rt_seq_push(result, child);
                else
                    find_path_all_recursive(child, rest, result, depth + 1);
            }
        }
    }
}

/// @brief `XmlNode.FindAll(tag)` — recursive search returning all matches.
///
/// Walks the entire subtree (including the receiver itself) collecting
/// every element whose tag equals `tag`. Returns an owned seq of
/// retained node references. A tag containing `/` is treated as a simple
/// slash-separated element path instead of a single literal tag.
/// @param node Root of the subtree to search.
/// @param tag Case-sensitive tag or slash-separated path.
/// @return Owned sequence of matching nodes, empty for invalid input or no matches.
void *rt_xml_find_all(void *node, rt_string tag) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);
    if (!rt_xml_is_node(node) || !tag)
        return result;

    const char *target = rt_string_cstr(tag);
    if (strchr(target, '/'))
        find_path_all_recursive(node, target, result, 0);
    else
        find_all_iterative(node, target, result);
    return result;
}

/// @brief Iterative DFS helper for `Find` — return first descendant element matching `tag`.
///
/// Pre-order traversal, returns the first hit. Retains the returned node so the
/// caller owns it. Children are pushed in reverse order to preserve document order.
/// @param node Root of the subtree to search.
/// @param tag Null-terminated case-sensitive tag to match.
/// @return Retained first matching element, or null when absent or traversal fails.
static void *find_first_iterative(void *node, const char *tag) {
    if (!rt_xml_is_node(node))
        return NULL;
    xml_node_stack stack = {0};
    if (!xml_node_stack_push(&stack, node, "Xml.Find: traversal stack allocation failed"))
        return NULL;
    while (stack.len > 0) {
        void *cur = stack.items[--stack.len];
        xml_node *n = (xml_node *)cur;
        if (n->type == XML_NODE_ELEMENT && n->tag) {
            const char *node_tag = rt_string_cstr(n->tag);
            if (strcmp(node_tag, tag) == 0) {
                rt_obj_retain_maybe(cur);
                xml_node_stack_dispose(&stack);
                return cur;
            }
        }
        if (n->children) {
            int64_t count = rt_seq_len(n->children);
            for (int64_t i = count; i > 0; i--) {
                if (!xml_node_stack_push(&stack,
                                         rt_seq_get(n->children, i - 1),
                                         "Xml.Find: traversal stack allocation failed")) {
                    xml_node_stack_dispose(&stack);
                    return NULL;
                }
            }
        }
    }
    xml_node_stack_dispose(&stack);
    return NULL;
}

/// @brief `XmlNode.Find(tag)` — first descendant element matching `tag`.
///
/// DFS pre-order; returns the receiver itself if it matches. Returns
/// NULL when no match is found. A tag containing `/` selects the first
/// result of the simple path search. The returned node is retained.
/// @param node Root of the subtree to search.
/// @param tag Case-sensitive tag or slash-separated path.
/// @return Retained first matching node, or null when absent or invalid.
void *rt_xml_find(void *node, rt_string tag) {
    if (!rt_xml_is_node(node) || !tag)
        return NULL;

    const char *target = rt_string_cstr(tag);
    if (strchr(target, '/')) {
        void *matches = rt_xml_find_all(node, tag);
        void *first = rt_seq_len(matches) > 0 ? rt_seq_get(matches, 0) : NULL;
        if (first)
            rt_obj_retain_maybe(first);
        if (rt_obj_release_check0(matches))
            rt_obj_free(matches);
        return first;
    }
    return find_first_iterative(node, target);
}

/// @brief `XmlNode.FindOption(tag)` — first matching descendant as an Option.
/// @details Wraps the retained node returned by @ref rt_xml_find in
///          `Some(node)` and releases the temporary reference after the Option
///          retains it. Absence, invalid receivers, and NULL tags return None.
/// @param node Starting XML node.
/// @param tag Tag name or simple slash-separated path.
/// @return Opaque Zanna.Option containing the first matching node, or None.
void *rt_xml_find_option(void *node, rt_string tag) {
    void *found = rt_xml_find(node, tag);
    if (!found)
        return rt_option_none();
    void *option = rt_option_some(found);
    if (rt_obj_release_check0(found))
        rt_obj_free(found);
    return option;
}

//=============================================================================
// Public API - Formatting
