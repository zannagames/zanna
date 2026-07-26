//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_ini.c
// Purpose: Implements INI/config file parsing and formatting for the
//          Zanna.Text.Ini class. Supports [sections], key=value pairs,
//          and line comments starting with ';' or '#'.
//
// Key invariants:
//   - Section names are case-sensitive; keys within a section are case-sensitive.
//   - Keys before any section header belong to the implicit root section "".
//   - Comment lines (starting with ';' or '#') are discarded during parsing.
//   - Leading and trailing whitespace is stripped from keys and values.
//   - Duplicate keys in the same section keep the last value seen.
//   - Formatting follows the implementation-defined order returned by Map.Keys,
//     except that the implicit root section is always emitted first.
//
// Ownership/Lifetime:
//   - The parsed INI object is a reference-counted Map owned by the caller.
//   - Section and key strings stored internally are fresh copies owned by the object.
//
// Links: src/runtime/text/rt_ini.h (public API),
//        src/runtime/rt_map.h (used internally to store section/key/value data)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ini.c
 * @brief Implements case-sensitive INI parsing, formatting, and mutation.
 * @details Lines are split across common newline forms, comments and blanks
 *          are discarded, names and values are trimmed, and section Maps are
 *          assembled beneath a root Map with last-value-wins semantics.
 *          Formatting emits the implicit root first and preserves Map snapshots.
 */

#include "rt_ini.h"

#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <ctype.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// @brief Trim leading and trailing `isspace` bytes from a bounded range.
/// @param start Start of the readable byte range.
/// @param len Number of bytes in the range.
/// @param out_len Non-null output receiving the trimmed byte length.
/// @return Pointer to the first retained byte, possibly the original range end.
static const char *ini_trim(const char *start, size_t len, size_t *out_len) {
    const char *end = start + len;
    while (start < end && isspace((unsigned char)*start))
        ++start;
    while (end > start && isspace((unsigned char)*(end - 1)))
        --end;
    *out_len = (size_t)(end - start);
    return start;
}

/// @brief Release one locally owned runtime-object reference.
/// @details Frees the object if the released reference was its last one. Null
///          pointers are ignored.
/// @param obj Runtime object reference owned by the current scope.
static void release_local_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Compute the byte length of an `rt_string`, NULL-safe.
/// @details Runtime strings are length-prefixed; embedded NUL bytes are
///          preserved when parsing/formatting.
/// @param s Borrowed runtime string.
/// @return Non-negative byte length, or zero for null/empty input.
static size_t str_len(rt_string s) {
    if (!s)
        return 0;
    int64_t len = rt_str_len(s);
    return len > 0 ? (size_t)len : 0;
}

/// @brief Copy a byte range into a runtime string or raise an INI trap.
/// @param bytes Borrowed byte range accepted by `rt_string_from_bytes`.
/// @param len Number of bytes to copy.
/// @return Newly allocated runtime string, or null only if the trap hook returns.
static rt_string ini_string_from_bytes_or_trap(const char *bytes, size_t len) {
    rt_string result = rt_string_from_bytes(bytes, len);
    if (!result)
        rt_trap("Ini: string allocation failed");
    return result;
}

/// @brief Convert a string-builder error into the common INI runtime trap.
/// @details A failed builder is released before trapping. Callers rely on the
///          trap being terminating and must not reuse it after failure.
/// @param sb Builder associated with @p status.
/// @param status Result of the most recent builder operation.
static void ini_check_sb(rt_string_builder *sb, rt_sb_status_t status) {
    if (status == RT_SB_OK)
        return;
    rt_sb_free(sb);
    rt_trap("Ini: string builder allocation failed");
}

/// @brief Append all bytes of a runtime string to a builder.
/// @details Null strings and inaccessible null buffers append nothing.
///          Embedded NUL bytes are preserved through the explicit length.
/// @param sb Initialized destination builder.
/// @param s Borrowed runtime string to append.
static void append_rt_string(rt_string_builder *sb, rt_string s) {
    if (!s)
        return;
    const char *c = rt_string_cstr(s);
    if (c)
        ini_check_sb(sb, rt_sb_append_bytes(sb, c, str_len(s)));
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

/// @brief Parse INI-format text into a nested Map: top-level keys are section names, values are
/// inner Maps of key→value strings. Keys outside any section go under the empty-string section.
/// @details Splits on CR, LF, or CRLF; trims entire lines, section names, keys,
///          and values; and ignores blank lines plus lines whose first
///          non-space byte is `;` or `#`. Values split at the first `=` and
///          remain unquoted and unescaped. Later duplicate keys replace earlier
///          values. Malformed lines and section headers without `]` are skipped.
///          A default-section Map is created lazily only when needed.
/// @param text Borrowed INI bytes; null and empty input produce an empty Map.
/// @return Newly allocated root Map owning all nested section Maps and strings.
void *rt_ini_parse(rt_string text) {
    void *root = rt_map_new();
    if (!text)
        return root;

    const char *src = rt_string_cstr(text);
    if (!src)
        return root;

    size_t src_len = (size_t)rt_str_len(text);
    const char *end = src + src_len;

    // Current section name (starts as "" for the default section). The default
    // section's Map is created lazily on the first out-of-section key so that
    // Parse(NULL) and Parse("") produce the same empty document shape.
    rt_string current_section = ini_string_from_bytes_or_trap("", 0);
    void *current_map = NULL;

    const char *line_start = src;
    while (line_start <= end) {
        // Find end of line
        const char *line_end = line_start;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            ++line_end;

        size_t raw_len = (size_t)(line_end - line_start);

        // Trim the line
        size_t tlen;
        const char *tline = ini_trim(line_start, raw_len, &tlen);

        if (tlen == 0 || tline[0] == ';' || tline[0] == '#') {
            // Empty line or comment — skip
        } else if (tline[0] == '[' && tlen >= 2) {
            // Section header: find closing ]
            const char *close = memchr(tline + 1, ']', tlen - 1);
            if (close) {
                size_t name_len;
                const char *name = ini_trim(tline + 1, (size_t)(close - tline - 1), &name_len);
                rt_string_unref(current_section);
                current_section = ini_string_from_bytes_or_trap(name, name_len);

                // Create section map if it doesn't exist
                if (!rt_map_has(root, current_section)) {
                    void *new_section = rt_map_new();
                    rt_map_set(root, current_section, new_section);
                    current_map = new_section;
                    release_local_obj(new_section);
                } else {
                    current_map = rt_map_get(root, current_section);
                }
            }
        } else {
            // Key=value pair
            const char *eq = memchr(tline, '=', tlen);
            if (eq) {
                size_t key_len;
                const char *key = ini_trim(tline, (size_t)(eq - tline), &key_len);
                size_t val_len;
                const char *val = ini_trim(eq + 1, tlen - (size_t)(eq - tline) - 1, &val_len);

                rt_string k = ini_string_from_bytes_or_trap(key, key_len);
                rt_string v = ini_string_from_bytes_or_trap(val, val_len);
                if (!current_map) {
                    // First key outside any section: materialize the default section.
                    void *default_section = rt_map_new();
                    rt_map_set(root, current_section, default_section);
                    current_map = default_section;
                    release_local_obj(default_section);
                }
                rt_map_set(current_map, k, (void *)v);
                rt_string_unref(k);
                rt_string_unref(v);
            }
        }

        // Advance past end-of-line
        if (line_end < end) {
            line_start = line_end + 1;
            // Handle \r\n
            if (*line_end == '\r' && line_start < end && *line_start == '\n')
                ++line_start;
        } else {
            break;
        }
    }

    rt_string_unref(current_section);
    return root;
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

/// @brief Serialize a parsed INI map back into INI-format text.
/// @details Layout matches what most INI tools accept:
///          1. Default-section keys (under empty-string section name)
///             are written first, no `[section]` header.
///          2. Each named section gets a leading blank line, then
///             `[name]`, then its `key = value` entries.
///          3. Values are written verbatim — no quoting, no escaping.
///          The blank line before each section is what gives the
///          rendered file its conventional INI look. Named sections and
///          their keys follow the implementation-defined snapshots returned
///          by `rt_map_keys`.
/// @param ini_map Borrowed Map of section-name strings to key/value Maps; null
///                is formatted as empty text.
/// @return Newly allocated formatted INI string owned by the caller.
rt_string rt_ini_format(void *ini_map) {
    if (!ini_map)
        return rt_string_from_bytes("", 0);

    rt_string_builder sb;
    rt_sb_init(&sb);

    // Get all section names
    void *sections = rt_map_keys(ini_map);
    int64_t sect_count = rt_seq_len(sections);

    // Write default section (empty key) first if it exists
    rt_string empty = ini_string_from_bytes_or_trap("", 0);
    void *default_sec = rt_map_get(ini_map, empty);
    if (default_sec && rt_map_len(default_sec) > 0) {
        void *keys = rt_map_keys(default_sec);
        int64_t kcount = rt_seq_len(keys);
        for (int64_t i = 0; i < kcount; ++i) {
            rt_string key = (rt_string)rt_seq_get(keys, i);
            rt_string val = (rt_string)rt_map_get(default_sec, key);
            append_rt_string(&sb, key);
            ini_check_sb(&sb, rt_sb_append_cstr(&sb, " = "));
            append_rt_string(&sb, val);
            ini_check_sb(&sb, rt_sb_append_bytes(&sb, "\n", 1));
        }
        release_local_obj(keys);
    }
    rt_string_unref(empty);

    // Write named sections
    for (int64_t s = 0; s < sect_count; ++s) {
        rt_string sect_name = (rt_string)rt_seq_get(sections, s);
        if (!sect_name || str_len(sect_name) == 0)
            continue; // Skip default section (already written)

        void *sect_map = rt_map_get(ini_map, sect_name);
        if (!sect_map)
            continue;

        ini_check_sb(&sb, rt_sb_append_bytes(&sb, "\n[", 2));
        append_rt_string(&sb, sect_name);
        ini_check_sb(&sb, rt_sb_append_bytes(&sb, "]\n", 2));

        void *keys = rt_map_keys(sect_map);
        int64_t kcount = rt_seq_len(keys);
        for (int64_t i = 0; i < kcount; ++i) {
            rt_string key = (rt_string)rt_seq_get(keys, i);
            rt_string val = (rt_string)rt_map_get(sect_map, key);
            append_rt_string(&sb, key);
            ini_check_sb(&sb, rt_sb_append_cstr(&sb, " = "));
            append_rt_string(&sb, val);
            ini_check_sb(&sb, rt_sb_append_bytes(&sb, "\n", 1));
        }
        release_local_obj(keys);
    }
    release_local_obj(sections);

    rt_string result = ini_string_from_bytes_or_trap(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

// ---------------------------------------------------------------------------
// Get / Set / Remove
// ---------------------------------------------------------------------------

/// @brief Get a retained value from a parsed INI Map.
/// @details Missing arguments, sections, and keys all return a newly allocated
///          empty string, making absence indistinguishable from an explicitly
///          empty value. A found string is retained before return.
/// @param ini_map Borrowed root INI Map.
/// @param section Borrowed section name, including `""` for the default section.
/// @param key Borrowed key name.
/// @return Caller-owned reference to the stored value, or a caller-owned empty
///         string when the lookup cannot be completed.
rt_string rt_ini_get(void *ini_map, rt_string section, rt_string key) {
    if (!ini_map || !section || !key)
        return rt_string_from_bytes("", 0);

    void *sect_map = rt_map_get(ini_map, section);
    if (!sect_map)
        return rt_string_from_bytes("", 0);

    rt_string val = (rt_string)rt_map_get(sect_map, key);
    if (!val)
        return rt_string_from_bytes("", 0);

    // Return a retained copy so caller can unref
    rt_obj_retain_maybe(val);
    return val;
}

/// @brief Set a value, creating its section Map when necessary.
/// @details The runtime Map retains the supplied key and value according to its
///          normal ownership rules. A null root, section, or key is ignored.
/// @param ini_map Borrowed mutable root INI Map.
/// @param section Borrowed section name.
/// @param key Borrowed key name.
/// @param value Borrowed runtime string value to store.
void rt_ini_set(void *ini_map, rt_string section, rt_string key, rt_string value) {
    if (!ini_map || !section || !key)
        return;

    void *sect_map = rt_map_get(ini_map, section);
    if (!sect_map) {
        sect_map = rt_map_new();
        rt_map_set(ini_map, section, sect_map);
        release_local_obj(sect_map);
    }
    rt_map_set(sect_map, key, (void *)value);
}

/// @brief Check whether a named section exists in the parsed INI map.
/// @param ini_map Borrowed root INI Map.
/// @param section Borrowed section name.
/// @return `1` when the section key exists, or `0` for absent/null input.
int8_t rt_ini_has_section(void *ini_map, rt_string section) {
    if (!ini_map || !section)
        return 0;
    return rt_map_has(ini_map, section);
}

/// @brief Return a Seq of all section names (top-level keys) in the parsed INI tree.
/// @details Ordering follows `rt_map_keys` and is implementation-defined. Null
///          input returns a newly allocated empty Seq.
/// @param ini_map Borrowed root INI Map.
/// @return Newly allocated Seq containing copies of all section-name strings.
void *rt_ini_sections(void *ini_map) {
    if (!ini_map)
        return rt_seq_new();
    return rt_map_keys(ini_map);
}

/// @brief Remove one key from a named INI section.
/// @details The section itself remains present even when its last key is
///          removed. Null inputs and missing sections report no removal.
/// @param ini_map Borrowed mutable root INI Map.
/// @param section Borrowed section name.
/// @param key Borrowed key to remove.
/// @return `1` when an entry was removed, otherwise `0`.
int8_t rt_ini_remove(void *ini_map, rt_string section, rt_string key) {
    if (!ini_map || !section || !key)
        return 0;

    void *sect_map = rt_map_get(ini_map, section);
    if (!sect_map)
        return 0;

    return rt_map_remove(sect_map, key);
}
