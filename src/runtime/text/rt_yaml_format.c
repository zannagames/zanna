//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_yaml_format.c
// Purpose: YAML emission (compact + indented) and type-of for Zanna.Text.Yaml.
//   Parsing lives in rt_yaml.c.
//
// Links: rt_yaml.h, rt_yaml_internal.h, rt_yaml.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief YAML emitter and runtime-value type inspection implementation.
/// @details Supported runtime values are serialized into deterministic
///          block-style collections with round-trip-safe scalar quoting.

#include "rt_yaml.h"
#include "rt_format.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include "rt_yaml_internal.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Formatting Helpers
//=============================================================================

/// @brief Serialize any supported runtime value into a shared YAML buffer.
/// @param obj Runtime value to serialize; null represents YAML null.
/// @param indent Spaces per collection nesting level.
/// @param level Current collection nesting depth.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_value(void *obj, int indent, int level, char **buf, size_t *cap, size_t *len);

/// Maximum block nesting the formatter will emit; matches the parser's
/// YAML_MAX_DEPTH. Exceeding it (deeply nested or CYCLIC containers) aborts
/// the format via this flag instead of recursing without bound.
#define YAML_FORMAT_MAX_DEPTH 200
/// @brief Set when the active format operation exceeds its bounded depth.
static int g_yaml_format_depth_exceeded = 0;

// ---------------------------------------------------------------------------
// Output buffer helpers — append to a growing heap buffer that
// `rt_yaml_dump` returns to the caller. Each helper handles the
// realloc-on-overflow doubling so the inner formatting code stays
// terse.
// ---------------------------------------------------------------------------

/// @brief Ensure a YAML formatting buffer can hold `needed` bytes plus a trailing NUL.
/// @details Grows geometrically using a temporary realloc result so the caller's
///          buffer pointer and capacity remain valid on allocation failure. The
///          helper traps on size_t overflow or OOM and returns `false`; callers
///          should stop appending after a false return.
/// @param buf In/out heap buffer pointer.
/// @param cap In/out byte capacity of `*buf`.
/// @param needed Number of non-NUL payload bytes required.
/// @return `true` when the buffer can hold the requested payload and terminator.
static bool yaml_format_reserve(char **buf, size_t *cap, size_t needed) {
    if (!buf || !cap || needed == SIZE_MAX) {
        rt_trap("rt_yaml: output length overflow");
        return false;
    }
    size_t required = needed + 1u;
    while (required > *cap) {
        size_t new_cap = (*cap == 0) ? 256u : (*cap * 2u);
        if (new_cap <= *cap || new_cap < required)
            new_cap = required;
        char *tmp = (char *)realloc(*buf, new_cap);
        if (!tmp) {
            rt_trap("rt_yaml: memory allocation failed");
            return false;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    return true;
}

/// @brief Append a null-terminated string to a growing YAML buffer.
/// @details Maintains a trailing null byte and stops after the reserve helper
///          traps on size overflow or allocation failure.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Required null-terminated source string.
static void buf_append(char **buf, size_t *cap, size_t *len, const char *str) {
    size_t slen = strlen(str);
    if (slen > SIZE_MAX - *len - 1u || !yaml_format_reserve(buf, cap, *len + slen))
        return;
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

/// @brief Append an exact byte span, including embedded null bytes.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param str Source byte span.
/// @param slen Number of source bytes.
static void buf_append_bytes(char **buf, size_t *cap, size_t *len, const char *str, size_t slen) {
    if (slen > SIZE_MAX - *len - 1u || !yaml_format_reserve(buf, cap, *len + slen))
        return;
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

/// @brief Append one byte while preserving the buffer terminator.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param c Byte to append.
static void buf_append_char(char **buf, size_t *cap, size_t *len, char c) {
    if (*len == SIZE_MAX || !yaml_format_reserve(buf, cap, *len + 1u))
        return;
    (*buf)[*len] = c;
    (*len)++;
    (*buf)[*len] = '\0';
}

/// @brief Append literal spaces for block indentation.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
/// @param spaces Number of spaces to add; nonpositive values add none.
static void buf_append_indent(char **buf, size_t *cap, size_t *len, int spaces) {
    for (int i = 0; i < spaces; i++)
        buf_append_char(buf, cap, len, ' ');
}

/// @brief Decide whether a string needs to be emitted as a quoted YAML scalar.
///
/// Plain scalars are ambiguous when they could be parsed as a
/// number, boolean, null, or special token (e.g. `true`, `123`,
/// `~`), or contain reserved indicator characters (`:`, `#`, `[`,
/// `{`, `&`, `*`, etc.) or leading/trailing whitespace. In any of
/// those cases we wrap them in double quotes for safe round-tripping.
/// @param str Candidate scalar byte span.
/// @param slen Scalar length in bytes.
/// @return `true` when plain emission would be empty, ambiguous, or syntactically reserved.
static bool needs_quoting(const char *str, size_t slen) {
    if (!str || slen == 0)
        return true;

    // Embedded NUL bytes can never be a plain scalar; force quoting so the
    // escape path preserves every byte.
    if (memchr(str, '\0', slen))
        return true;

    if ((slen == 3 && memcmp(str, "---", 3) == 0) || (slen == 3 && memcmp(str, "...", 3) == 0))
        return true;

    void *parsed = parse_scalar(str, slen);
    bool parsed_as_string = parsed && rt_string_is_handle(parsed);
    yaml_release(parsed);
    if (!parsed_as_string)
        return true;

    if (isspace((unsigned char)str[0]) || isspace((unsigned char)str[slen - 1]))
        return true;

    // Check first char
    char c = str[0];
    if (c == '-' || c == ':' || c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '&' || c == '*' || c == '!' || c == '|' || c == '>' || c == '\'' || c == '"' ||
        c == '%' || c == '@' || c == '`')
        return true;

    // Check for special chars
    for (size_t i = 0; i < slen; i++) {
        if (str[i] == '\n' || str[i] == '\r' || str[i] == ':' || str[i] == '#')
            return true;
    }

    return false;
}

/// @brief Emit a string as YAML — plain or double-quoted depending on `needs_quoting`.
///
/// When quoted, escapes `\\`, `\"`, and the standard control
/// characters (`\n`, `\t`, etc.) so the output round-trips through
/// `parse_quoted_string`.
/// @param str Scalar byte span, or null for an empty scalar.
/// @param slen Scalar length in bytes.
/// @param indent Current indentation width, reserved for formatting parity.
/// @param level Current nesting level, reserved for formatting parity.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_string(
    const char *str, size_t slen, int indent, int level, char **buf, size_t *cap, size_t *len) {
    if (!str || slen == 0) {
        buf_append(buf, cap, len, "''");
        return;
    }

    if (!needs_quoting(str, slen)) {
        buf_append_bytes(buf, cap, len, str, slen);
        return;
    }

    // Quote with double quotes
    buf_append_char(buf, cap, len, '"');
    for (size_t bi = 0; bi < slen; bi++) {
        const char *p = str + bi;
        switch (*p) {
            case '"':
                buf_append(buf, cap, len, "\\\"");
                break;
            case '\\':
                buf_append(buf, cap, len, "\\\\");
                break;
            case '\n':
                buf_append(buf, cap, len, "\\n");
                break;
            case '\t':
                buf_append(buf, cap, len, "\\t");
                break;
            case '\r':
                buf_append(buf, cap, len, "\\r");
                break;
            case '\b':
                buf_append(buf, cap, len, "\\b");
                break;
            case '\f':
                buf_append(buf, cap, len, "\\f");
                break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04X", (unsigned char)*p);
                    buf_append(buf, cap, len, esc);
                } else {
                    buf_append_char(buf, cap, len, *p);
                }
                break;
        }
    }
    buf_append_char(buf, cap, len, '"');
}

/// @brief Recursive YAML emitter for any Zanna value.
///
/// Dispatches by runtime type:
///   - NULL                  → `null`
///   - boxed bool/int/float  → `true`/`false`/`123`/`1.5`
///   - rt_string             → plain or quoted scalar
///   - Seq[Any]              → block sequence (one `- elem` per line)
///   - Map[Any,Any]          → block mapping (`key: value` lines)
/// `indent` is the indent step (typically 2); `level` is the
/// current nesting depth. Blocks emit a leading newline so they
/// can sit after a `key:` or `-`. Unknown object kinds are emitted as
/// YAML null, and nesting beyond @c YAML_FORMAT_MAX_DEPTH sets the shared
/// failure flag.
/// @param obj Runtime value to serialize; null represents YAML null.
/// @param indent Spaces per collection nesting level.
/// @param level Current collection nesting depth.
/// @param buf Address of the heap-buffer pointer.
/// @param cap Address of the buffer capacity.
/// @param len Address of the populated byte count.
static void format_value(void *obj, int indent, int level, char **buf, size_t *cap, size_t *len) {
    if (level > YAML_FORMAT_MAX_DEPTH) {
        g_yaml_format_depth_exceeded = 1;
        return;
    }
    if (!obj) {
        buf_append(buf, cap, len, "null");
        return;
    }

    // Check for boxed values.
    if (yaml_is_boxed(obj)) {
        int64_t type_tag = rt_box_type(obj);
        if (type_tag == RT_BOX_I1) {
            buf_append(buf, cap, len, rt_unbox_i1(obj) ? "true" : "false");
            return;
        }
        if (type_tag == RT_BOX_I64) {
            char num[32];
            snprintf(num, sizeof(num), "%lld", (long long)rt_unbox_i64(obj));
            buf_append(buf, cap, len, num);
            return;
        }
        if (type_tag == RT_BOX_F64) {
            double val = rt_unbox_f64(obj);
            if (isinf(val)) {
                buf_append(buf, cap, len, val > 0 ? ".inf" : "-.inf");
            } else if (isnan(val)) {
                buf_append(buf, cap, len, ".nan");
            } else {
                // Locale-independent exact formatting (VDOC-041).
                char num[64];
                rt_format_f64_roundtrip(val, num, sizeof(num));
                buf_append(buf, cap, len, num);
            }
            return;
        }
        if (type_tag == RT_BOX_STR) {
            rt_string s = rt_unbox_str(obj);
            const char *str = rt_string_cstr(s);
            int64_t str_len = rt_str_len(s);
            format_string(str, str_len < 0 ? 0 : (size_t)str_len, indent, level, buf, cap, len);
            if (rt_obj_release_check0((void *)s))
                rt_obj_free((void *)s);
            return;
        }
    }

    // Check for string
    if (rt_string_is_handle(obj)) {
        const char *str = rt_string_cstr((rt_string)obj);
        int64_t str_len = rt_str_len((rt_string)obj);
        format_string(str, str_len < 0 ? 0 : (size_t)str_len, indent, level, buf, cap, len);
        return;
    }

    // Check for sequence
    if (yaml_is_sequence(obj)) {
        int64_t seq_len = rt_seq_len(obj);
        if (seq_len == 0) {
            buf_append(buf, cap, len, "[]");
            return;
        }

        for (int64_t i = 0; i < seq_len; i++) {
            if (i > 0 || level > 0) {
                buf_append_char(buf, cap, len, '\n');
                buf_append_indent(buf, cap, len, indent * level);
            }
            buf_append(buf, cap, len, "- ");

            void *item = rt_seq_get(obj, i);
            format_value(item, indent, level + 1, buf, cap, len);
        }
        return;
    }

    // Check for map
    if (yaml_is_mapping(obj)) {
        void *keys = rt_map_keys(obj);
        int64_t nkeys = rt_seq_len(keys);
        if (nkeys == 0) {
            buf_append(buf, cap, len, "{}");
            yaml_release(keys);
            return;
        }

        for (int64_t i = 0; i < nkeys; i++) {
            if (i > 0 || level > 0) {
                buf_append_char(buf, cap, len, '\n');
                buf_append_indent(buf, cap, len, indent * level);
            }

            void *key = rt_seq_get(keys, i);
            const char *key_str = rt_string_cstr((rt_string)key);
            int64_t raw_key_len = rt_str_len((rt_string)key);
            size_t key_len = raw_key_len < 0 ? 0 : (size_t)raw_key_len;
            if (needs_quoting(key_str, key_len)) {
                format_string(key_str, key_len, indent, level, buf, cap, len);
            } else {
                buf_append_bytes(buf, cap, len, key_str, key_len);
            }
            buf_append(buf, cap, len, ": ");

            void *val = rt_map_get(obj, (rt_string)key);

            // Check if value needs newline (sequences and maps are complex)
            bool complex_value = yaml_is_sequence(val) || yaml_is_mapping(val);

            if (complex_value) {
                format_value(val, indent, level + 1, buf, cap, len);
            } else {
                format_value(val, indent, level, buf, cap, len);
            }
        }

        yaml_release(keys);
        return;
    }

    // Unrecognized runtime object: emit YAML null rather than guessing a scalar.
    buf_append(buf, cap, len, "null");
}

//=============================================================================
// Public API - Formatting
//=============================================================================

/// @brief `Yaml.Format(obj)` — serialize a Zanna value as a YAML string (2-space indent).
///        Delegates to `rt_yaml_format_indent` with `indent=2`.
/// @param obj Map, sequence, string, boxed scalar, null, or unknown runtime object.
/// @return Owned YAML string; empty when the nesting bound is exceeded.
rt_string rt_yaml_format(void *obj) {
    return rt_yaml_format_indent(obj, 2);
}

/// @brief `Yaml.FormatIndent(obj, indent)` — serialize with a custom indent width (1–8).
///        Values outside [1, 8] are clamped to 2 or 8 respectively.
///        Output always uses block-style collections and quotes ambiguous scalars.
/// @param obj Map, sequence, string, boxed scalar, null, or unknown runtime object.
/// @param indent Requested indentation width; values below one use two.
/// @return Owned YAML string; empty when the nesting bound is exceeded.
rt_string rt_yaml_format_indent(void *obj, int64_t indent) {
    if (indent < 1)
        indent = 2;
    if (indent > 8)
        indent = 8;

    char *buf = NULL;
    size_t cap = 0, len = 0;

    g_yaml_format_depth_exceeded = 0;
    format_value(obj, (int)indent, 0, &buf, &cap, &len);
    if (g_yaml_format_depth_exceeded) {
        // Cyclic or absurdly deep input: fail closed with an empty document.
        free(buf);
        return rt_string_from_bytes("", 0);
    }

    rt_string result = rt_string_from_bytes(buf ? buf : "", (int64_t)len);
    free(buf);
    return result;
}

//=============================================================================
// Public API - Type Inspection
//=============================================================================

/// @brief `Yaml.TypeOf(obj)` — return a string describing the YAML type of a parsed value.
///        Returns "null", "bool", "int", "float", "string", "sequence", "mapping", or "unknown".
/// @param obj Runtime value to classify; null is the YAML null category.
/// @return Owned category-name string.
rt_string rt_yaml_type_of(void *obj) {
    if (!obj)
        return rt_string_from_bytes("null", 4);

    // String check (non-boxed)
    if (rt_string_is_handle(obj))
        return rt_string_from_bytes("string", 6);

    // Sequence check
    if (yaml_is_sequence(obj))
        return rt_string_from_bytes("sequence", 8);

    // Map check
    if (yaml_is_mapping(obj))
        return rt_string_from_bytes("mapping", 7);

    // Boxed values
    if (yaml_is_boxed(obj)) {
        int64_t type_tag = rt_box_type(obj);
        if (type_tag == RT_BOX_I1)
            return rt_string_from_bytes("bool", 4);
        if (type_tag == RT_BOX_I64)
            return rt_string_from_bytes("int", 3);
        if (type_tag == RT_BOX_F64)
            return rt_string_from_bytes("float", 5);
        if (type_tag == RT_BOX_STR)
            return rt_string_from_bytes("string", 6);
    }

    return rt_string_from_bytes("unknown", 7);
}
