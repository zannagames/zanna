//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_toml.c
// Purpose: Implements a TOML-subset parser and formatter for the Zanna.Data.Toml
//          class. Produces an rt_map tree from a TOML document supporting
//          tables ([table]), arrays of tables ([[array]]), inline tables,
//          basic/literal/multi-line strings, and array values. This is NOT a
//          conforming TOML v1.0 parser: every scalar (integers, floats,
//          booleans, datetimes) is stored as an rt_string and arbitrary
//          unquoted scalar text is accepted (so IsValid is a structural check,
//          not a conformance check). Dotted assignment keys (a.b = ...)
//          construct nested tables like section headers; quoted keys keep
//          their literal spelling. Input must be NUL-free valid UTF-8.
//
// Key invariants:
//   - [Section] key hierarchy maps to nested rt_map trees; scalars are strings.
//   - Arrays are rt_seq of element strings (nested arrays/inline tables recurse).
//   - Duplicate keys within the same table cause a parse error.
//   - Parse returns NULL on invalid input (not a trap).
//   - Format emits [section] headers for nested tables (see the depth limits
//     documented on rt_toml_format).
//
// Ownership/Lifetime:
//   - Returned rt_map trees are fresh allocations owned by the caller.
//   - Formatted TOML strings are fresh rt_string allocations owned by caller.
//
// Links: src/runtime/text/rt_toml.h (public API),
//        src/runtime/rt_map.h, rt_seq.h, rt_box.h (container types)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_toml.c
 * @brief Implements the runtime's documented TOML structural subset.
 * @details The parser validates NUL-free UTF-8 and builds nested Maps,
 *          sequences, arrays of tables, inline tables, dotted keys, and
 *          multiple String forms while retaining scalar spellings as Strings.
 *          Formatting and dotted lookup operate on this managed representation.
 */

#include "rt_toml.h"

#include "rt_string_internal.h"

#include "rt_box.h"
#include "rt_format.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Minimum payload bytes read by Seq/Map public APIs when formatting TOML values.
#define TOML_SEQ_MIN_PAYLOAD (sizeof(int64_t) * 2 + sizeof(void *) + sizeof(int8_t))
#define TOML_MAP_MIN_PAYLOAD (sizeof(void *) * 2 + sizeof(size_t) * 2)

// --- Internal parse error flag (S-14, thread-local to avoid concurrent parse clobbering) ---
static _Thread_local int g_toml_had_error = 0;

// --- Helper: create string from substring ---

/// @brief One-line wrapper that builds an `rt_string` from a (ptr, len) substring.
/// @param s Source byte span.
/// @param len Number of bytes to copy.
/// @return Newly allocated runtime string.
static rt_string make_str(const char *s, int64_t len) {
    return rt_string_from_bytes(s, len);
}

/// @brief Append bytes during TOML parsing and flag parse failure on error.
/// @details TOML parsing is intentionally non-trapping; malformed input and
///          allocation failures both flow through @ref g_toml_had_error so the
///          public parser can release partial containers and return NULL.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the append failed.
static int toml_parse_append_bytes(rt_string_builder *sb, const char *bytes, size_t len) {
    if (rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK)
        return 1;
    g_toml_had_error = 1;
    return 0;
}

// --- Helper: trim whitespace ---

/// @brief Skip horizontal whitespace (`space`, `tab`) — does NOT consume newlines.
/// @details TOML treats newlines as significant (line-oriented format),
///          so we explicitly limit whitespace skipping to in-line spaces.
/// @param p In/out null-terminated parse cursor.
static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

/// @brief Advance the cursor past the rest of the current line, including the `\n`.
/// @details Used to skip comments and malformed lines without breaking
///          the parser. Stops at NUL if no terminating newline is found.
/// @param p In/out null-terminated parse cursor.
static void skip_line(const char **p) {
    while (**p && **p != '\n')
        (*p)++;
    if (**p == '\n')
        (*p)++;
}

/// @brief Skip whitespace, newlines, and comments inside multiline TOML arrays.
/// @details Unlike `skip_ws`, this consumes line boundaries because TOML arrays
///          may span multiple lines and may contain comments between elements.
/// @param p Cursor to advance.
static void skip_array_ws(const char **p) {
    for (;;) {
        while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
            (*p)++;
        if (**p == '#') {
            skip_line(p);
            continue;
        }
        return;
    }
}

// --- Helper: parse a bare key (alphanumeric, dash, underscore) ---

/// @brief Parse a TOML bare key (alphanumerics, `-`, `_`, `.`).
/// @details Dotted keys (e.g. `a.b.c`) are returned as a single
///          string here — the caller splits on `.` to walk nested
///          tables. Returns NULL when the cursor isn't sitting on a
///          legal key character.
/// @param p In/out parse cursor.
/// @return Newly allocated key string, or null when no bare-key byte is present.
static rt_string parse_bare_key(const char **p) {
    const char *start = *p;
    while (isalnum((unsigned char)**p) || **p == '-' || **p == '_' || **p == '.')
        (*p)++;
    if (*p == start)
        return NULL;
    return make_str(start, (int64_t)(*p - start));
}

// --- Helper: parse a quoted string ---

/// @brief Decode one hexadecimal digit used by TOML `\u`/`\U` escapes.
/// @param c Input byte.
/// @return Value in [0, 15], or -1 when @p c is not hexadecimal.
static int toml_hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// @brief Append a Unicode scalar value as UTF-8 to a string builder.
/// @details Rejects surrogate halves and values outside Unicode range by
///          setting the TOML parse-error flag. Valid values append between
///          one and four bytes.
/// @param sb Destination builder.
/// @param codepoint Unicode scalar value.
static void toml_append_utf8(rt_string_builder *sb, uint32_t codepoint) {
    char out[4];
    size_t len = 0;
    if (codepoint <= 0x7Fu) {
        out[len++] = (char)codepoint;
    } else if (codepoint <= 0x7FFu) {
        out[len++] = (char)(0xC0u | (codepoint >> 6));
        out[len++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
        g_toml_had_error = 1;
        return;
    } else if (codepoint <= 0xFFFFu) {
        out[len++] = (char)(0xE0u | (codepoint >> 12));
        out[len++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[len++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0x10FFFFu) {
        out[len++] = (char)(0xF0u | (codepoint >> 18));
        out[len++] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        out[len++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[len++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else {
        g_toml_had_error = 1;
        return;
    }
    (void)toml_parse_append_bytes(sb, out, len);
}

/// @brief Parse a basic/literal TOML string, including multiline variants.
/// @details Basic strings decode TOML escapes (`\n`, `\t`, `\"`, `\\`,
///          `\uXXXX`, and `\UXXXXXXXX`). Literal strings preserve bytes.
///          Triple-quoted strings may span lines and terminate only at the
///          matching triple quote. The opening quote must be at `**p`.
/// @param p In/out cursor positioned at a single or double quote.
/// @return Newly allocated decoded string; malformed input sets the parse-error flag.
static rt_string parse_quoted_string(const char **p) {
    char quote = **p;
    int multiline = ((*p)[1] == quote && (*p)[2] == quote);
    *p += multiline ? 3 : 1;

    rt_string_builder sb;
    rt_sb_init(&sb);
    while (**p) {
        if (multiline && (*p)[0] == quote && (*p)[1] == quote && (*p)[2] == quote) {
            *p += 3;
            rt_string result = rt_string_from_bytes(sb.data, sb.len);
            rt_sb_free(&sb);
            return result;
        }
        if (!multiline && **p == quote) {
            (*p)++;
            rt_string result = rt_string_from_bytes(sb.data, sb.len);
            rt_sb_free(&sb);
            return result;
        }
        if (!multiline && **p == '\n') {
            g_toml_had_error = 1;
            break;
        }
        if (quote == '"' && **p == '\\') {
            (*p)++;
            char esc = **p;
            if (!esc) {
                g_toml_had_error = 1;
                break;
            }
            (*p)++;
            switch (esc) {
                case 'b':
                    (void)toml_parse_append_bytes(&sb, "\b", 1);
                    break;
                case 't':
                    (void)toml_parse_append_bytes(&sb, "\t", 1);
                    break;
                case 'n':
                    (void)toml_parse_append_bytes(&sb, "\n", 1);
                    break;
                case 'f':
                    (void)toml_parse_append_bytes(&sb, "\f", 1);
                    break;
                case 'r':
                    (void)toml_parse_append_bytes(&sb, "\r", 1);
                    break;
                case '"':
                    (void)toml_parse_append_bytes(&sb, "\"", 1);
                    break;
                case '\\':
                    (void)toml_parse_append_bytes(&sb, "\\", 1);
                    break;
                case 'u':
                case 'U': {
                    int digits = esc == 'u' ? 4 : 8;
                    uint32_t cp = 0;
                    for (int i = 0; i < digits; i++) {
                        int hv = toml_hex_value((*p)[i]);
                        if (hv < 0) {
                            g_toml_had_error = 1;
                            break;
                        }
                        cp = (cp << 4) | (uint32_t)hv;
                    }
                    if (g_toml_had_error)
                        break;
                    *p += digits;
                    toml_append_utf8(&sb, cp);
                    break;
                }
                default:
                    g_toml_had_error = 1;
                    break;
            }
            if (g_toml_had_error)
                break;
            continue;
        }
        if (!toml_parse_append_bytes(&sb, *p, 1))
            break;
        (*p)++;
    }
    if (!g_toml_had_error)
        g_toml_had_error = 1;
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

// --- Helper: parse a value ---

/// @brief Parse a TOML scalar value (string, number, bool, date) into an `rt_string`.
/// @details Strings (quoted or unquoted) all come back as raw strings;
///          numeric/boolean type discrimination is left to the
///          consumer (most callers run `rt_parse_try_int` /
///          `rt_parse_try_bool` to coerce after the fact). Stops at
///          newline, `#` (comment), or `,` (array element separator);
///          trailing in-line whitespace is trimmed off.
/// @param p In/out parse cursor.
/// @param stop_bracket Whether a closing bracket terminates a bare scalar.
/// @param stop_brace Whether a closing brace terminates a bare scalar.
/// @return Newly allocated scalar string.
static rt_string parse_value_until(const char **p, int stop_bracket, int stop_brace) {
    skip_ws(p);

    // Quoted string
    if (**p == '"' || **p == '\'')
        return parse_quoted_string(p);

    // Bare value (number, boolean, date, or unquoted string)
    const char *start = *p;
    while (**p && **p != '\n' && **p != '#' && **p != ',' && (!stop_bracket || **p != ']') &&
           (!stop_brace || **p != '}'))
        (*p)++;

    // Trim trailing whitespace
    const char *end = *p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    return make_str(start, (int64_t)(end - start));
}

// --- Helper: parse an inline array ---

/// @brief Release a TOML value after storing it in an owning container.
/// @details `rt_map_set` and `rt_seq_push` retain runtime values. This helper
///          drops the parser's temporary reference for both raw strings and
///          object-backed values.
/// @param obj TOML value object, raw `rt_string`, or NULL.
static void release_obj_maybe(void *obj) {
    if (!obj)
        return;
    if (rt_string_is_handle(obj)) {
        rt_string_unref((rt_string)obj);
        return;
    }
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void *parse_value_object(const char **p, int stop_bracket, int stop_brace);

/// @brief Parse a TOML inline table literal `{ key = value, ... }`.
/// @details Values may be strings, arrays, or nested inline tables. Duplicate
///          keys and malformed separators mark the parse as failed.
/// @param p Cursor positioned at the opening `{`; advanced past `}` on success.
/// @return Fresh rt_map containing the inline table entries.
static void *parse_inline_table(const char **p) {
    (*p)++;
    void *map = rt_map_new();
    if (!map) {
        g_toml_had_error = 1;
        return NULL;
    }

    while (**p) {
        skip_ws(p);
        if (**p == '}') {
            (*p)++;
            return map;
        }
        rt_string key = NULL;
        if (**p == '"' || **p == '\'')
            key = parse_quoted_string(p);
        else
            key = parse_bare_key(p);
        if (!key) {
            g_toml_had_error = 1;
            return map;
        }
        skip_ws(p);
        if (**p != '=') {
            g_toml_had_error = 1;
            rt_string_unref(key);
            return map;
        }
        (*p)++;
        if (rt_map_has(map, key)) {
            g_toml_had_error = 1;
            rt_string_unref(key);
            return map;
        }
        void *value = parse_value_object(p, 0, 1);
        if (value) {
            rt_map_set(map, key, value);
            release_obj_maybe(value);
        }
        rt_string_unref(key);
        skip_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == '}') {
            (*p)++;
            return map;
        }
        g_toml_had_error = 1;
        return map;
    }

    if (!g_toml_had_error)
        g_toml_had_error = 1;
    return map;
}

/// @brief Parse a TOML inline array literal `[a, b, c]` into a Seq of TOML values.
/// @details Walks comma-separated values until the closing `]`. Each
///          element is read via `parse_value_object`, so nested arrays and
///          inline tables are preserved as runtime containers. Tolerates
///          trailing commas and stray whitespace between elements.
/// @param p In/out cursor positioned at the opening bracket.
/// @return Caller-owned Seq containing parsed values; malformed input sets the
///         parse-error flag.
static void *parse_array(const char **p) {
    (*p)++; // skip '['
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);

    while (**p) {
        skip_array_ws(p);
        if (**p == ']') {
            (*p)++;
            return seq;
        }
        if (**p == ',') {
            g_toml_had_error = 1;
            break;
        }
        void *val = parse_value_object(p, 1, 0);
        if (val) {
            rt_seq_push(seq, val);
            release_obj_maybe(val);
        }
        skip_array_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == ']') {
            (*p)++;
            return seq;
        }
        break;
    }
    if (!g_toml_had_error)
        g_toml_had_error = 1;
    return seq;
}

/// @brief Parse one TOML value as the appropriate runtime object.
/// @details Arrays become Seq, inline tables become Map, and scalar values
///          become rt_string. Numeric/bool/datetime coercion remains in the
///          existing typed getters.
/// @param p Cursor at the value start.
/// @param stop_bracket Whether `]` terminates a bare value.
/// @param stop_brace Whether `}` terminates a bare value.
/// @return Fresh value object or NULL on allocation failure.
static void *parse_value_object(const char **p, int stop_bracket, int stop_brace) {
    skip_ws(p);
    if (**p == '[')
        return parse_array(p);
    if (**p == '{')
        return parse_inline_table(p);
    return parse_value_until(p, stop_bracket, stop_brace);
}

/// @brief Maximum nesting depth for TOML sections/tables (consistent with JSON/XML/YAML).
#define TOML_MAX_DEPTH 200

/// @brief Test whether a value has the runtime Map layout.
/// @param obj Borrowed candidate value.
/// @return Nonzero for a Map object, otherwise zero.
static int is_map_obj(void *obj) {
    return obj && !rt_string_is_handle(obj) &&
           rt_obj_is_instance(obj, RT_MAP_CLASS_ID, TOML_MAP_MIN_PAYLOAD);
}

/// @brief Test whether a value has the runtime Seq layout.
/// @param obj Borrowed candidate value.
/// @return Nonzero for a Seq object, otherwise zero.
static int is_seq_obj(void *obj) {
    return obj && !rt_string_is_handle(obj) &&
           rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, TOML_SEQ_MIN_PAYLOAD);
}

/// @brief Resolve or create a dotted path of nested TOML tables.
/// @details Each missing segment becomes a Map. Empty segments and collisions
///          with non-Map values set the parse-error flag.
/// @param root Borrowed root Map.
/// @param name Dotted bare-key bytes.
/// @param len Number of bytes in @p name.
/// @return Borrowed deepest table, or the last reachable value after an error.
static void *ensure_table_path(void *root, const char *name, size_t len) {
    void *current = root;
    size_t pos = 0;

    while (pos < len) {
        if (!is_map_obj(current)) {
            g_toml_had_error = 1;
            return current;
        }

        size_t start = pos;
        while (pos < len && name[pos] != '.')
            pos++;
        if (pos == start) {
            g_toml_had_error = 1;
            return current;
        }

        rt_string key = make_str(name + start, (int64_t)(pos - start));
        void *next = rt_map_get(current, key);
        if (next && !is_map_obj(next)) {
            g_toml_had_error = 1;
            rt_string_unref(key);
            return current;
        }
        if (!next) {
            next = rt_map_new();
            rt_map_set(current, key, next);
            release_obj_maybe(next);
        }
        rt_string_unref(key);
        current = next;

        if (pos < len && name[pos] == '.')
            pos++;
    }

    return current;
}

/// @brief Ensure an array-of-tables path exists and append a fresh table.
/// @details For `[[a.b]]`, prefix segments are regular maps and the final
///          segment is a Seq that owns map entries. The appended map becomes
///          the current section for subsequent key/value lines.
/// @param root Root TOML map.
/// @param name Dotted array-of-tables name.
/// @param len Byte length of @p name.
/// @return Borrowed pointer to the appended table, or @p root after an error.
static void *ensure_array_table_path(void *root, const char *name, size_t len) {
    if (!root || !name || len == 0) {
        g_toml_had_error = 1;
        return root;
    }

    size_t last_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '.')
            last_start = i + 1;
    }
    if (last_start >= len) {
        g_toml_had_error = 1;
        return root;
    }

    void *parent = root;
    if (last_start > 0)
        parent = ensure_table_path(root, name, last_start - 1);
    if (!is_map_obj(parent)) {
        g_toml_had_error = 1;
        return root;
    }

    rt_string key = make_str(name + last_start, (int64_t)(len - last_start));
    void *seq = rt_map_get(parent, key);
    if (seq && !is_seq_obj(seq)) {
        g_toml_had_error = 1;
        rt_string_unref(key);
        return root;
    }
    if (!seq) {
        seq = rt_seq_new();
        if (!seq) {
            g_toml_had_error = 1;
            rt_string_unref(key);
            return root;
        }
        rt_seq_set_owns_elements(seq, 1);
        rt_map_set(parent, key, seq);
        release_obj_maybe(seq);
        seq = rt_map_get(parent, key);
    }

    void *table = rt_map_new();
    if (!table) {
        g_toml_had_error = 1;
        rt_string_unref(key);
        return root;
    }
    rt_seq_push(seq, table);
    release_obj_maybe(table);
    rt_string_unref(key);
    return table;
}

// --- Public API ---

/// @brief Parse a NUL-free UTF-8 TOML-subset document into nested containers.
/// @details Tables become Maps, arrays become Seqs, and every scalar spelling
///          becomes a runtime string. Duplicate keys, malformed structure,
///          invalid UTF-8, and embedded null bytes return null without trapping.
/// @param src Borrowed TOML source string.
/// @return Caller-owned root Map, or null on invalid input or parse allocation failure.
void *rt_toml_parse(rt_string src) {
    if (!src)
        return NULL;

    const char *p = rt_string_cstr(src);
    // TOML documents are text: an embedded NUL byte in the runtime String is
    // invalid input. Reject it explicitly instead of silently parsing only the
    // prefix before the NUL (the C-string walker below cannot see past it).
    int64_t src_len = rt_str_len(src);
    if (!p || src_len < 0 || memchr(p, '\0', (size_t)src_len) != NULL)
        return NULL;
    // TOML requires a valid UTF-8 stream (VDOC-040).
    if (!rt_utf8_span_valid(p, (size_t)src_len))
        return NULL;
    g_toml_had_error = 0;
    void *root = rt_map_new();
    void *current_section = root;

    while (*p) {
        skip_ws(&p);

        // Skip empty lines
        if (*p == '\n') {
            p++;
            continue;
        }

        // Skip comments
        if (*p == '#') {
            skip_line(&p);
            continue;
        }

        // Section header [section] or [section.subsection]
        if (*p == '[') {
            p++;
            int is_array = 0;
            if (*p == '[') {
                p++;
                is_array = 1;
            }

            skip_ws(&p);
            rt_string section_name = parse_bare_key(&p);
            skip_ws(&p);

            if (*p == ']') {
                p++;
            } else {
                g_toml_had_error = 1;
                if (section_name)
                    rt_string_unref(section_name);
                skip_line(&p);
                continue;
            }
            if (is_array) {
                if (*p == ']') {
                    p++;
                } else {
                    g_toml_had_error = 1;
                    if (section_name)
                        rt_string_unref(section_name);
                    skip_line(&p);
                    continue;
                }
            }
            skip_ws(&p);
            if (*p != '\0' && *p != '\n' && *p != '#') {
                g_toml_had_error = 1;
                if (section_name)
                    rt_string_unref(section_name);
                skip_line(&p);
                continue;
            }

            if (section_name) {
                // Create nested map for section
                const char *name_cstr = rt_string_cstr(section_name);
                size_t name_len = (size_t)rt_str_len(section_name);

                // Count nesting depth (dots + 1)
                int depth = 1;
                for (size_t di = 0; di < name_len; di++) {
                    if (name_cstr[di] == '.')
                        depth++;
                }
                if (depth > TOML_MAX_DEPTH) {
                    g_toml_had_error = 1;
                    rt_string_unref(section_name);
                    skip_line(&p);
                    continue;
                }

                current_section = is_array ? ensure_array_table_path(root, name_cstr, name_len)
                                           : ensure_table_path(root, name_cstr, name_len);
                rt_string_unref(section_name);
            }
            skip_line(&p);
            continue;
        }

        // Key = Value
        const int quoted_key = (*p == '"' || *p == '\'');
        rt_string key = NULL;
        if (quoted_key)
            key = parse_quoted_string(&p);
        else
            key = parse_bare_key(&p);

        if (!key) {
            /* S-14: flag malformed line that cannot be parsed as key=value */
            g_toml_had_error = 1;
            skip_line(&p);
            continue;
        }

        skip_ws(&p);
        if (*p != '=') {
            /* S-14: flag missing '=' separator */
            g_toml_had_error = 1;
            rt_string_unref(key);
            skip_line(&p);
            continue;
        }
        p++; // skip '='
        skip_ws(&p);

        // Dotted bare keys construct nested tables per TOML semantics
        // (`a.b = v` stores `v` under key `b` of table `a`). Quoted keys keep
        // their literal spelling, dots included.
        void *target_section = current_section;
        rt_string final_key = key; // borrowed alias unless a split allocates
        rt_string split_key = NULL;
        if (!quoted_key) {
            const char *kc = rt_string_cstr(key);
            const char *last_dot = kc ? strrchr(kc, '.') : NULL;
            if (last_dot) {
                int key_depth = 1;
                for (const char *dp = kc; *dp; dp++) {
                    if (*dp == '.')
                        key_depth++;
                }
                if (last_dot == kc || last_dot[1] == '\0' || key_depth > TOML_MAX_DEPTH) {
                    /* Leading/trailing dot or excessive nesting: malformed key. */
                    g_toml_had_error = 1;
                    rt_string_unref(key);
                    skip_line(&p);
                    continue;
                }
                target_section = ensure_table_path(current_section, kc, (size_t)(last_dot - kc));
                if (!target_section || !is_map_obj(target_section)) {
                    g_toml_had_error = 1;
                    rt_string_unref(key);
                    skip_line(&p);
                    continue;
                }
                split_key = make_str(last_dot + 1, (int64_t)strlen(last_dot + 1));
                final_key = split_key;
            }
        }

        // Parse value
        if (rt_map_has(target_section, final_key)) {
            g_toml_had_error = 1;
            if (split_key)
                rt_string_unref(split_key);
            rt_string_unref(key);
            skip_line(&p);
            continue;
        }
        void *val = parse_value_object(&p, 0, 0);
        if (val) {
            rt_map_set(target_section, final_key, val);
            release_obj_maybe(val);
        }
        if (split_key)
            rt_string_unref(split_key);
        rt_string_unref(key);

        skip_line(&p);
    }

    if (g_toml_had_error) {
        release_obj_maybe(root);
        return NULL;
    }
    return root;
}

/// @brief Check whether a string contains valid TOML syntax.
/// @details Performs a complete parse and releases the temporary result.
/// @param src Borrowed TOML source string.
/// @return `1` when `rt_toml_parse` succeeds, otherwise `0`.
int8_t rt_toml_is_valid(rt_string src) {
    void *result = rt_toml_parse(src);
    if (!result || g_toml_had_error)
        return 0;
    release_obj_maybe(result);
    return 1;
}

/// @brief Test whether bytes can be emitted as an unquoted TOML key.
/// @param s Candidate key bytes.
/// @param len Number of candidate bytes.
/// @return Nonzero for a nonempty alphanumeric/underscore/hyphen key.
static int is_bare_key_bytes(const char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

/// @brief Append bytes during TOML formatting without touching parser error state.
/// @details Formatting failures should abort the current format operation, but
///          must not leak into the thread-local parser validity flag. Formatter
///          helpers therefore use local integer status propagation.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the append failed.
static int toml_format_append_bytes(rt_string_builder *sb, const char *bytes, size_t len) {
    return rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK;
}

/// @brief Append a C string during TOML formatting.
/// @param sb Destination builder.
/// @param text NUL-terminated bytes to append.
/// @return 1 on success, 0 when the append failed.
static int toml_format_append_cstr(rt_string_builder *sb, const char *text) {
    return rt_sb_append_cstr(sb, text) == RT_SB_OK;
}

/// @brief Append TOML basic-string text with required escaping and surrounding quotes.
/// @details Escapes backslash, quote, common control characters, and remaining
///          C0 controls as `\u00XX`. The input is treated as already valid
///          UTF-8 text; non-control bytes are copied through unchanged.
/// @param sb Destination builder.
/// @param s Source bytes; NULL is treated as empty.
/// @param len Number of bytes to read from @p s.
/// @return 1 on success, 0 when escaping or appending failed.
static int append_quoted_toml_bytes(rt_string_builder *sb, const char *s, size_t len) {
    if (!toml_format_append_cstr(sb, "\""))
        return 0;
    if (s) {
        for (size_t i = 0; i < len; ++i) {
            char ch = s[i];
            switch (ch) {
                case '\\':
                    if (!toml_format_append_cstr(sb, "\\\\"))
                        return 0;
                    break;
                case '"':
                    if (!toml_format_append_cstr(sb, "\\\""))
                        return 0;
                    break;
                case '\n':
                    if (!toml_format_append_cstr(sb, "\\n"))
                        return 0;
                    break;
                case '\r':
                    if (!toml_format_append_cstr(sb, "\\r"))
                        return 0;
                    break;
                case '\t':
                    if (!toml_format_append_cstr(sb, "\\t"))
                        return 0;
                    break;
                case '\b':
                    if (!toml_format_append_cstr(sb, "\\b"))
                        return 0;
                    break;
                case '\f':
                    if (!toml_format_append_cstr(sb, "\\f"))
                        return 0;
                    break;
                default:
                    if ((unsigned char)ch < 0x20) {
                        char esc[7];
                        int n = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)ch);
                        if (n < 0 || (size_t)n >= sizeof(esc) || !toml_format_append_cstr(sb, esc))
                            return 0;
                    } else {
                        if (!toml_format_append_bytes(sb, &ch, 1))
                            return 0;
                    }
                    break;
            }
        }
    }
    return toml_format_append_cstr(sb, "\"");
}

/// @brief Append a NUL-terminated string as a quoted TOML basic string.
/// @param sb Destination builder.
/// @param s Source C string; NULL is treated as empty.
/// @return 1 on success, 0 when escaping or appending failed.
static int append_quoted_toml_string(rt_string_builder *sb, const char *s) {
    return append_quoted_toml_bytes(sb, s, s ? strlen(s) : 0);
}

/// @brief Append a TOML key, using bare-key syntax when legal.
/// @details Bare keys are emitted directly; all other keys are emitted as
///          quoted TOML strings with the same escaping rules as values.
/// @param sb Destination builder.
/// @param key Key bytes; NULL emits an empty quoted key.
/// @param key_len Number of bytes in @p key.
/// @return 1 on success, 0 when appending failed.
static int append_toml_key_bytes(rt_string_builder *sb, const char *key, size_t key_len) {
    if (is_bare_key_bytes(key, key_len))
        return toml_format_append_bytes(sb, key, key_len);
    return append_quoted_toml_bytes(sb, key ? key : "", key ? key_len : 0);
}

/// @brief Append one runtime value using TOML value syntax.
/// @details Supports raw runtime strings, boxed booleans/integers/floats/strings,
///          sequence objects, and inline Map values. Unsupported values are
///          serialized as an empty TOML string to preserve the legacy fallback.
/// @param sb Destination builder.
/// @param val Runtime value pointer.
/// @param depth Current recursive container depth.
/// @return 1 on success, 0 when appending or numeric formatting failed.
static int append_toml_value(rt_string_builder *sb, void *val, int depth) {
    // Depth guard bounds recursion for deeply nested (or cyclic) containers;
    // exceeding it fails the format, which surfaces as an empty result.
    if (depth > TOML_MAX_DEPTH)
        return 0;
    if (!val) {
        return append_quoted_toml_string(sb, "");
    }

    if (rt_string_is_handle(val)) {
        rt_string s = (rt_string)val;
        int64_t len = rt_str_len(s);
        if (len < 0)
            return 0;
        return append_quoted_toml_bytes(sb, rt_string_cstr(s), (size_t)len);
    }

    int64_t box_type = rt_box_type(val);
    char buf[96];
    switch (box_type) {
        case RT_BOX_I1:
            return toml_format_append_cstr(sb, rt_unbox_i1(val) ? "true" : "false");
        case RT_BOX_I64: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)rt_unbox_i64(val));
            return n >= 0 && (size_t)n < sizeof(buf) &&
                   toml_format_append_bytes(sb, buf, (size_t)n);
        }
        case RT_BOX_F64: {
            double d = rt_unbox_f64(val);
            int n;
            if (isfinite(d)) {
                // Locale-independent exact formatting (VDOC-041): plain snprintf
                // would inherit the process C numeric locale (comma separators).
                rt_format_f64_roundtrip(d, buf, sizeof(buf));
                n = (int)strlen(buf);
            } else {
                // inf/nan spellings contain no separator and are valid TOML.
                n = snprintf(buf, sizeof(buf), "%.17g", d);
            }
            if (n < 0 || (size_t)n >= sizeof(buf) - 2)
                return 0;
            // TOML floats must carry a fractional/exponent marker: whole-valued
            // doubles like 1.0 print as "1" via %g, which is an integer token.
            // Append ".0" so the value keeps its float type on reparse. Skip for
            // outputs already containing '.', an exponent, or inf/nan spellings.
            int has_float_marker = 0;
            for (int i = 0; i < n; i++) {
                if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E' || buf[i] == 'n' ||
                    buf[i] == 'i') {
                    has_float_marker = 1;
                    break;
                }
            }
            if (!has_float_marker) {
                buf[n++] = '.';
                buf[n++] = '0';
                buf[n] = '\0';
            }
            return toml_format_append_bytes(sb, buf, (size_t)n);
        }
        case RT_BOX_STR: {
            rt_string s = rt_unbox_str(val);
            int ok = 0;
            if (s) {
                int64_t len = rt_str_len(s);
                ok = len >= 0 && append_quoted_toml_bytes(sb, rt_string_cstr(s), (size_t)len);
            } else {
                ok = append_quoted_toml_string(sb, "");
            }
            rt_string_unref(s);
            return ok;
        }
        default:
            break;
    }

    if (is_seq_obj(val)) {
        if (!toml_format_append_cstr(sb, "["))
            return 0;
        int64_t len = rt_seq_len(val);
        for (int64_t i = 0; i < len; i++) {
            if (i > 0 && !toml_format_append_cstr(sb, ", "))
                return 0;
            if (!append_toml_value(sb, rt_seq_get(val, i), depth + 1))
                return 0;
        }
        return toml_format_append_cstr(sb, "]");
    }

    if (is_map_obj(val)) {
        // Maps in value position (e.g. inside arrays) emit as inline tables.
        if (!toml_format_append_cstr(sb, "{"))
            return 0;
        void *keys = rt_map_keys(val);
        if (!keys)
            return 0;
        int ok = 1;
        int64_t n = rt_seq_len(keys);
        for (int64_t i = 0; ok && i < n; i++) {
            rt_string k = (rt_string)rt_seq_get(keys, i);
            int64_t klen = rt_str_len(k);
            if (klen < 0 || (i > 0 && !toml_format_append_cstr(sb, ", ")) ||
                !append_toml_key_bytes(sb, rt_string_cstr(k), (size_t)klen) ||
                !toml_format_append_cstr(sb, " = ") ||
                !append_toml_value(sb, rt_map_get(val, k), depth + 1)) {
                ok = 0;
            }
        }
        release_obj_maybe(keys);
        return ok && toml_format_append_cstr(sb, "}");
    }

    return append_quoted_toml_string(sb, "");
}

/// @brief Recursively emit a table: scalar entries first, then subtables as
///        dotted `[a.b.c]` section headers.
/// @param sb Output builder.
/// @param map Table to emit.
/// @param path Dotted (already-escaped) path of this table; empty at the root.
/// @param depth Current nesting depth; bounded by TOML_MAX_DEPTH so deeply
///        nested or cyclic inputs fail the format instead of recursing forever.
/// @return 1 on success, 0 on failure.
static int toml_format_table(rt_string_builder *sb, void *map, rt_string_builder *path, int depth) {
    if (depth > TOML_MAX_DEPTH)
        return 0;

    void *keys = rt_map_keys(map);
    if (!keys)
        return 0;
    int ok = 1;
    int64_t n = rt_seq_len(keys);

    // Pass 1: scalar/array entries under this table's header.
    for (int64_t i = 0; ok && i < n; i++) {
        rt_string key = (rt_string)rt_seq_get(keys, i);
        void *val = rt_map_get(map, key);
        if (is_map_obj(val))
            continue;
        int64_t klen = rt_str_len(key);
        if (klen < 0 || !append_toml_key_bytes(sb, rt_string_cstr(key), (size_t)klen) ||
            !toml_format_append_cstr(sb, " = ") || !append_toml_value(sb, val, depth) ||
            !toml_format_append_cstr(sb, "\n"))
            ok = 0;
    }

    // Pass 2: subtables as [dotted.section] headers, recursively.
    for (int64_t i = 0; ok && i < n; i++) {
        rt_string key = (rt_string)rt_seq_get(keys, i);
        void *val = rt_map_get(map, key);
        if (!is_map_obj(val))
            continue;
        int64_t klen = rt_str_len(key);
        if (klen < 0) {
            ok = 0;
            break;
        }

        const size_t saved_path_len = path->len;
        if ((path->len > 0 && !toml_format_append_cstr(path, ".")) ||
            !append_toml_key_bytes(path, rt_string_cstr(key), (size_t)klen)) {
            ok = 0;
            break;
        }

        if (!toml_format_append_cstr(sb, "[") ||
            !toml_format_append_bytes(sb, path->data, path->len) ||
            !toml_format_append_cstr(sb, "]\n") || !toml_format_table(sb, val, path, depth + 1) ||
            !toml_format_append_cstr(sb, "\n"))
            ok = 0;

        path->len = saved_path_len;
    }

    release_obj_maybe(keys);
    return ok;
}

/// @brief Format a runtime Map as TOML-subset text.
/// @details Emits non-Map entries before recursively emitted dotted section
///          headers. Invalid roots, allocation failure, and recursion beyond
///          `TOML_MAX_DEPTH` produce an empty string.
/// @param map Borrowed root Map.
/// @return Caller-owned TOML text, or an empty string on failure.
rt_string rt_toml_format(void *map) {
    if (!map)
        return rt_string_from_bytes("", 0);
    if (!is_map_obj(map))
        return rt_string_from_bytes("", 0);

    rt_string_builder sb;
    rt_sb_init(&sb);
    rt_string_builder path;
    rt_sb_init(&path);

    if (!toml_format_table(&sb, map, &path, 0)) {
        rt_sb_free(&path);
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&path);
    rt_sb_free(&sb);
    return result;
}

/// @brief Get a value from a parsed TOML document by dot-separated key path.
/// @details @p root may be a Map, raw TOML runtime string, or boxed string.
///          Empty path segments and non-Map intermediate values fail.
/// @param root Borrowed parsed Map or TOML text value.
/// @param key_path Borrowed dotted byte path.
/// @return Borrowed value at the path, or null when parsing or lookup fails.
void *rt_toml_get(void *root, rt_string key_path) {
    if (!root || !key_path)
        return NULL;

    // Auto-parse: if root is a raw TOML string, parse it first
    if (rt_string_is_handle(root)) {
        root = rt_toml_parse((rt_string)root);
        if (!root)
            return NULL;
    } else if (rt_box_type(root) == RT_BOX_STR) {
        // Boxed string (from Zia str→ptr conversion) — unbox and parse
        rt_string s = rt_unbox_str(root);
        if (s) {
            root = rt_toml_parse(s);
            rt_string_unref(s);
            if (!root)
                return NULL;
        }
    }

    const char *path = rt_string_cstr(key_path);
    size_t path_len = (size_t)rt_str_len(key_path);
    size_t path_pos = 0;
    void *current = root;

    while (path_pos < path_len) {
        if (!is_map_obj(current))
            return NULL;

        size_t start = path_pos;
        while (path_pos < path_len && path[path_pos] != '.')
            path_pos++;
        if (path_pos == start)
            return NULL;

        rt_string key = make_str(path + start, (int64_t)(path_pos - start));
        if (path_pos < path_len && path[path_pos] == '.')
            path_pos++;

        void *next = rt_map_get(current, key);
        rt_string_unref(key);
        if (!next)
            return NULL;
        current = next;
    }

    return current;
}

/// @brief Get a string value from a parsed TOML document by key path.
/// @param root Borrowed parsed Map or TOML text value.
/// @param key_path Borrowed dotted byte path.
/// @return Retained string value, or a newly allocated empty string for
///         missing and non-string values.
rt_string rt_toml_get_str(void *root, rt_string key_path) {
    void *val = rt_toml_get(root, key_path);
    if (!val)
        return rt_string_from_bytes("", 0);
    // Check if value is a raw string
    if (rt_string_is_handle(val))
        return rt_string_ref((rt_string)val);
    // Try as boxed string
    if (rt_box_type(val) == RT_BOX_STR)
        return rt_unbox_str(val);
    return rt_string_from_bytes("", 0);
}
