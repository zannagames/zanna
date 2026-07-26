//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_multipart.c
// Purpose: Multipart form-data builder/parser for HTTP file uploads.
// Key invariants:
//   - Generates random boundaries and conventional CRLF multipart formatting.
//   - Builder is append-only; parts are concatenated on Build().
//   - Every receiver is validated by stable class identity before payload access.
//   - Parsing is bounded, strict, atomic, and releases partial state on traps.
// Ownership/Lifetime:
//   - Multipart objects are GC-managed and own native copies of every part.
//   - Returned Bytes, Strings, Multipart values, and Results are caller-owned.
// Links: rt_multipart.h (API)
//
//===----------------------------------------------------------------------===//

#include "rt_multipart.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Internal Structures
//=============================================================================

#define MULTIPART_INITIAL_PART_CAPACITY 16
#define MULTIPART_MAX_PARSED_BODY_BYTES (64u * 1024u * 1024u)
#define MULTIPART_MAX_HEADER_BYTES (64u * 1024u)

typedef struct {
    char *name;
    char *filename; // NULL for text fields
    uint8_t *data;
    size_t data_len;
    int is_file;
} multipart_part_t;

typedef struct {
    char boundary[64];
    multipart_part_t *parts;
    int part_count;
    int part_capacity;
} rt_multipart_impl;

//=============================================================================
// Internal Helpers
//=============================================================================

/// @brief Fill @p buf with an unbiased random multipart boundary.
/// @details Generates at most 40 alphanumeric bytes plus a NUL terminator. Random bytes greater
///          than or equal to 248 are rejected so modulo reduction into the 62-character alphabet
///          does not bias boundary character distribution.
/// @param buf Destination buffer for the boundary string.
/// @param buf_len Size of @p buf, including space for the NUL terminator.
static void generate_boundary(char *buf, size_t buf_len) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    if (!buf || buf_len == 0)
        return;

    size_t len = buf_len - 1;
    if (len > 40)
        len = 40;
    for (size_t i = 0; i < len;) {
        uint8_t random[32];
        rt_crypto_random_bytes(random, sizeof(random));
        for (size_t j = 0; j < sizeof(random) && i < len; j++) {
            if (random[j] >= 248)
                continue;
            buf[i++] = chars[random[j] % (sizeof(chars) - 1)];
        }
    }
    buf[len] = '\0';
}

/// @brief Borrow the mutable payload pointer from a managed Bytes object.
/// @param obj Bytes handle.
/// @return Borrowed payload pointer, or null for invalid/empty storage as defined by Bytes.
static inline uint8_t *bytes_data(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Read a managed Bytes object's logical length.
/// @param obj Bytes handle.
/// @return Byte length, or the Bytes layer's invalid-handle sentinel.
static inline int64_t bytes_len_impl(void *obj) {
    return rt_bytes_len(obj);
}

/// @brief Release one caller-owned managed multipart staging reference.
/// @details Strings are released by the common dynamic release helper; heap
///          objects additionally run their finalizer/free path at zero. NULL is
///          accepted so recovery cleanup can remain unconditional.
/// @param value Managed reference, or NULL.
static void multipart_release_owned(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Snapshot the active trap diagnostic before clearing a recovery frame.
/// @details Trap diagnostics use thread-local storage that cleanup may replace.
///          Copying into caller stack storage preserves the original failure
///          when it is re-raised after native and managed resources are freed.
/// @param output Destination buffer.
/// @param capacity Destination capacity in bytes.
/// @param fallback Message used when the active diagnostic is empty.
static void multipart_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Validate and cast a managed Multipart receiver.
/// @details Stable class identity and the complete payload size are checked
///          before any field is read, preventing arbitrary managed objects from
///          being reinterpreted as native pointer/count storage.
/// @param obj Candidate receiver.
/// @param context Diagnostic emitted for an invalid receiver.
/// @return Multipart payload, or NULL after trapping.
static rt_multipart_impl *multipart_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_MULTIPART_CLASS_ID, sizeof(rt_multipart_impl))) {
        rt_trap(context);
        return NULL;
    }
    return (rt_multipart_impl *)obj;
}

/// @brief Test Multipart identity without raising a trap.
/// @param obj Candidate managed object.
/// @return Nonzero only for a live, fully sized Multipart payload.
static int multipart_is_handle(void *obj) {
    return rt_obj_is_instance(obj, RT_MULTIPART_CLASS_ID, sizeof(rt_multipart_impl));
}

/// @brief Add a serialized multipart size component with overflow checking.
/// @param total Running size to update.
/// @param value Number of bytes to add.
/// @return 1 on success; 0 for a null accumulator or `size_t` overflow.
static int multipart_size_add(size_t *total, size_t value) {
    if (!total || *total > SIZE_MAX - value)
        return 0;
    *total += value;
    return 1;
}

/// @brief Detect embedded null bytes across a runtime String's exact length.
/// @param value Runtime String to inspect; null is treated as empty.
/// @return Nonzero when a null occurs before the logical end; zero otherwise.
static int multipart_string_has_embedded_nul(rt_string value) {
    if (!value)
        return 0;
    const char *cstr = rt_string_cstr(value);
    int64_t len64 = rt_str_len(value);
    if (!cstr || len64 <= 0)
        return 0;
    return memchr(cstr, '\0', (size_t)len64) != NULL;
}

/// @brief Ensure a multipart object can hold at least @p needed parts.
/// @details Builder and parser objects use the same append-only part storage.
///          The array grows geometrically with overflow checks so ordinary
///          forms avoid repeated reallocations while large forms are no longer
///          capped by a compile-time 128-part table.
/// @param mp Multipart implementation object.
/// @param needed Minimum required part count.
/// @return 1 when capacity is available; 0 on overflow or allocation failure.
static int multipart_reserve_parts(rt_multipart_impl *mp, int needed) {
    if (!mp || needed < 0)
        return 0;
    if (needed <= mp->part_capacity)
        return 1;

    int new_capacity = mp->part_capacity > 0 ? mp->part_capacity : MULTIPART_INITIAL_PART_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > (uint64_t)SIZE_MAX / sizeof(*mp->parts))
        return 0;

    multipart_part_t *grown =
        (multipart_part_t *)realloc(mp->parts, (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    memset(
        grown + mp->part_capacity, 0, (size_t)(new_capacity - mp->part_capacity) * sizeof(*grown));
    mp->parts = grown;
    mp->part_capacity = new_capacity;
    return 1;
}

/// @brief Validate a parsed multipart Content-Disposition parameter value.
/// @details Rejects controls and DEL so parsed inbound names and filenames cannot inject extra
///          headers if later reserialized. Quotes, backslashes, and semicolons are allowed after
///          parsing because they are data bytes from a quoted parameter; the builder escapes them
///          at serialization time.
/// @param value NUL-terminated parameter value.
/// @return 1 when the value is safe to serialize or accept from parsed input; otherwise 0.
static int multipart_header_param_is_valid(const char *value) {
    if (!value)
        return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    }
    return 1;
}

/// @brief Validate a parsed multipart boundary token.
/// @details Enforces the runtime boundary buffer limit and RFC-compatible bcharsnospace-style
///          characters before parser state is allocated for a multipart body.
/// @param boundary NUL-terminated boundary parameter value.
/// @return 1 when the boundary is syntactically valid and fits in rt_multipart_impl; otherwise 0.
static int multipart_boundary_is_valid(const char *boundary) {
    size_t len = boundary ? strlen(boundary) : 0;
    if (len == 0 || len >= sizeof(((rt_multipart_impl *)0)->boundary))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)boundary[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
            c != '\'' && c != '(' && c != ')' && c != '+' && c != '_' && c != ',' && c != '-' &&
            c != '.' && c != '/' && c != ':' && c != '=' && c != '?') {
            return 0;
        }
    }
    return 1;
}

/// @brief Validate a managed header parameter and borrow its C-string bytes.
/// @details Rejects invalid handles and embedded NUL bytes before native
///          serialization. NULL may map to a fixed fallback for optional
///          filename values. Required names can additionally reject emptiness,
///          ensuring builder output is accepted by the strict parser.
/// @param value Candidate managed String.
/// @param fallback Borrowed fallback used only when NULL is allowed.
/// @param context Diagnostic emitted for invalid input.
/// @param allow_null Nonzero to accept NULL as @p fallback.
/// @param require_nonempty Nonzero to reject an empty non-NULL String.
/// @param output Receives borrowed NUL-terminated bytes.
/// @return One on success; zero after trapping.
static int multipart_header_param_view(rt_string value,
                                       const char *fallback,
                                       const char *context,
                                       int allow_null,
                                       int require_nonempty,
                                       const char **output) {
    if (output)
        *output = NULL;
    if (!output) {
        rt_trap(context);
        return 0;
    }
    if (!value) {
        if (allow_null) {
            *output = fallback;
            return 1;
        }
        rt_trap(context);
        return 0;
    }
    if (!rt_string_is_handle(value)) {
        rt_trap(context);
        return 0;
    }
    const char *cstr = rt_string_cstr(value);
    int64_t len = rt_str_len(value);
    if (!cstr || len < 0 || (require_nonempty && len == 0) ||
        multipart_string_has_embedded_nul(value)) {
        rt_trap(context);
        return 0;
    }
    *output = cstr;
    return 1;
}

/// @brief Append an exact byte span to a pre-sized serialization buffer.
/// @details Performs subtraction-based bounds checks before `memcpy`, avoiding
///          pointer or size overflow even when a corrupted internal size reaches
///          the serializer. A zero-length append succeeds with a NULL source.
/// @param buffer Destination buffer.
/// @param capacity Exact writable capacity.
/// @param position In/out write offset.
/// @param bytes Source bytes.
/// @param length Number of bytes to append.
/// @return One on success, otherwise zero without modifying @p position.
static int multipart_append_bytes(
    uint8_t *buffer, size_t capacity, size_t *position, const void *bytes, size_t length) {
    if (!buffer || !position || *position > capacity || length > capacity - *position ||
        (length > 0 && !bytes)) {
        return 0;
    }
    if (length > 0)
        memcpy(buffer + *position, bytes, length);
    *position += length;
    return 1;
}

/// @brief Find the first exact byte-subsequence occurrence in a bounded range.
/// @param haystack Bytes to search.
/// @param haystack_len Number of searchable bytes.
/// @param needle Nonempty byte sequence to find.
/// @param needle_len Number of bytes in @p needle.
/// @return Borrowed pointer to the first match, or null when absent or invalid.
static const uint8_t *find_bytes(const uint8_t *haystack,
                                 size_t haystack_len,
                                 const uint8_t *needle,
                                 size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len)
        return NULL;

    size_t limit = haystack_len - needle_len;
    for (size_t i = 0; i <= limit; i++) {
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/// @brief Find a syntactically complete multipart boundary delimiter line.
/// @details A candidate must begin at the body start or immediately after CRLF,
///          and the boundary token must be followed by CRLF (ordinary part) or
///          `--` (closing delimiter). Prefix matches such as `--boundaryExtra`
///          are skipped so body data cannot be truncated by a shorter token.
/// @param haystack Remaining body bytes.
/// @param haystack_len Number of available bytes.
/// @param token Boundary token including its leading `--`.
/// @param token_len Exact token length.
/// @return Pointer to the token start, or NULL.
static const uint8_t *multipart_find_boundary_line(const uint8_t *haystack,
                                                   size_t haystack_len,
                                                   const uint8_t *token,
                                                   size_t token_len) {
    if (!haystack || !token || token_len == 0)
        return NULL;
    size_t offset = 0;
    while (offset <= haystack_len) {
        const uint8_t *candidate =
            find_bytes(haystack + offset, haystack_len - offset, token, token_len);
        if (!candidate)
            return NULL;
        size_t index = (size_t)(candidate - haystack);
        size_t remaining = haystack_len - index;
        bool line_start =
            index == 0 || (index >= 2 && candidate[-2] == '\r' && candidate[-1] == '\n');
        bool valid_suffix = remaining >= token_len + 2 &&
                            ((candidate[token_len] == '\r' && candidate[token_len + 1] == '\n') ||
                             (candidate[token_len] == '-' && candidate[token_len + 1] == '-'));
        if (line_start && valid_suffix)
            return candidate;
        offset = index + 1;
    }
    return NULL;
}

/// @brief Compare two equal-length ASCII spans case-insensitively.
/// @param a First span.
/// @param b Second span.
/// @param len Number of bytes to compare.
/// @return 1 when all bytes match after ASCII case folding; 0 otherwise.
static int multipart_ascii_ieq_n(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
    }
    return 1;
}

/// @brief Compute the serialized length of a quoted multipart parameter value.
/// @details Quotes and backslashes each require one additional escape byte.
/// @param value Null-terminated parameter value; null has length zero.
/// @param len_out Optional receiver for the escaped length.
/// @return 1 on success; 0 on `size_t` overflow.
static int multipart_escaped_quoted_length(const char *value, size_t *len_out) {
    size_t len = 0;
    if (len_out)
        *len_out = 0;
    if (!value)
        return 1;
    while (*value) {
        size_t add = (*value == '"' || *value == '\\') ? 2u : 1u;
        if (len > SIZE_MAX - add)
            return 0;
        if (*value == '"' || *value == '\\')
            len += 2;
        else
            len += 1;
        value++;
    }
    if (len_out)
        *len_out = len;
    return 1;
}

/// @brief Append a safely escaped quoted header parameter without allocation.
/// @details Quotes and backslashes receive a backslash prefix; control bytes
///          and DEL become spaces, preserving the builder's injection-safe
///          historical behavior. The caller's sizing pass must have included
///          @ref multipart_escaped_quoted_length for the same value.
/// @param buffer Destination serialization buffer.
/// @param capacity Exact writable capacity.
/// @param position In/out write offset.
/// @param value NUL-terminated parameter value.
/// @return One on success, otherwise zero.
static int multipart_append_escaped_quoted(uint8_t *buffer,
                                           size_t capacity,
                                           size_t *position,
                                           const char *value) {
    if (!value)
        return 1;
    while (*value) {
        char ch = *value++;
        if ((unsigned char)ch < 0x20u || (unsigned char)ch == 0x7fu)
            ch = ' ';
        if (ch == '"' || ch == '\\') {
            const char slash = '\\';
            if (!multipart_append_bytes(buffer, capacity, position, &slash, 1))
                return 0;
        }
        if (!multipart_append_bytes(buffer, capacity, position, &ch, 1))
            return 0;
    }
    return 1;
}

/// @brief Advance a parser cursor past optional spaces and horizontal tabs.
/// @param cursor Address of the input cursor; null cursors are no-ops.
static void multipart_skip_ows(const char **cursor) {
    while (cursor && *cursor && (**cursor == ' ' || **cursor == '\t'))
        (*cursor)++;
}

/// @brief Parse a quoted parameter value while removing backslash escapes.
/// @param cursor In/out cursor initially pointing at the opening quote.
/// @param out Optional destination for a truncated, null-terminated copy.
/// @param out_cap Capacity of @p out including its terminator.
/// @param closed_out Optional receiver set when a closing quote is consumed.
/// @return Full unescaped value length, even when the destination is smaller.
static size_t multipart_copy_unescaped_quoted(const char **cursor,
                                              char *out,
                                              size_t out_cap,
                                              int *closed_out) {
    size_t len = 0;
    const char *p = cursor ? *cursor : NULL;
    if (closed_out)
        *closed_out = 0;
    if (!p || *p != '"')
        return 0;
    p++;
    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\' && *p)
            ch = *p++;
        if (out && len + 1 < out_cap)
            out[len] = ch;
        len++;
    }
    if (*p == '"') {
        p++;
        if (closed_out)
            *closed_out = 1;
    }
    if (out && out_cap > 0) {
        size_t write_len = len < out_cap - 1 ? len : out_cap - 1;
        out[write_len] = '\0';
    }
    if (cursor)
        *cursor = p;
    return len;
}

/// @brief Parse an unquoted parameter token up to whitespace, semicolon, or line ending.
/// @param cursor In/out parser cursor.
/// @param out Optional destination for a truncated, null-terminated copy.
/// @param out_cap Capacity of @p out including its terminator.
/// @return Full token length, even when the destination is smaller.
static size_t multipart_copy_token_value(const char **cursor, char *out, size_t out_cap) {
    size_t len = 0;
    const char *p = cursor ? *cursor : NULL;
    if (!p)
        return 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n') {
        if (*p == ' ' || *p == '\t')
            break;
        if (out && len + 1 < out_cap)
            out[len] = *p;
        len++;
        p++;
    }
    if (out && out_cap > 0) {
        size_t write_len = len < out_cap - 1 ? len : out_cap - 1;
        out[write_len] = '\0';
    }
    if (cursor)
        *cursor = p;
    return len;
}

/// @brief Extract one case-insensitive semicolon parameter from header text.
/// @details Supports quoted values with backslash escapes and unquoted tokens,
///          rejecting unterminated quotes or values that do not fit the caller's
///          output buffer.
/// @param text Parameter-list text.
/// @param target_name Parameter name to locate.
/// @param out Optional output buffer for the decoded value.
/// @param out_cap Capacity of @p out including its terminator.
/// @return 1 when a complete fitting value is found; 0 otherwise.
static int multipart_extract_param_value(const char *text,
                                         const char *target_name,
                                         char *out,
                                         size_t out_cap) {
    const char *p = text;
    size_t target_len = target_name ? strlen(target_name) : 0;
    if (out && out_cap > 0)
        out[0] = '\0';
    if (!text || !target_name || target_len == 0)
        return 0;

    while (*p) {
        const char *name_start;
        const char *name_end;
        multipart_skip_ows(&p);
        if (*p == ';')
            p++;
        multipart_skip_ows(&p);
        if (*p == '\0' || *p == '\r' || *p == '\n')
            break;

        name_start = p;
        while (*p && *p != '=' && *p != ';' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        name_end = p;
        multipart_skip_ows(&p);
        if (*p != '=') {
            while (*p && *p != ';' && *p != '\r' && *p != '\n')
                p++;
            continue;
        }
        p++;
        multipart_skip_ows(&p);

        if ((size_t)(name_end - name_start) == target_len &&
            multipart_ascii_ieq_n(name_start, target_name, target_len)) {
            size_t value_len = 0;
            int closed = 1;
            if (*p == '"')
                value_len = multipart_copy_unescaped_quoted(&p, out, out_cap, &closed);
            else
                value_len = multipart_copy_token_value(&p, out, out_cap);
            return closed && (out_cap == 0 || value_len < out_cap);
        }

        if (*p == '"')
            multipart_copy_unescaped_quoted(&p, NULL, 0, NULL);
        else
            multipart_copy_token_value(&p, NULL, 0);

        while (*p && *p != ';' && *p != '\r' && *p != '\n')
            p++;
    }

    return 0;
}

/// @brief Extract one case-insensitive field value from CRLF-delimited part headers.
/// @param headers Null-terminated part-header block.
/// @param header_name Field name to locate.
/// @param out Optional output buffer.
/// @param out_cap Capacity of @p out including its terminator.
/// @return 1 when a complete fitting value is found; 0 otherwise.
static int multipart_extract_header_value(const char *headers,
                                          const char *header_name,
                                          char *out,
                                          size_t out_cap) {
    const char *line = headers;
    size_t target_len = header_name ? strlen(header_name) : 0;
    if (out && out_cap > 0)
        out[0] = '\0';
    if (!headers || !header_name || target_len == 0)
        return 0;

    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        const char *colon = strchr(line, ':');
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if (colon && (size_t)(colon - line) == target_len &&
            multipart_ascii_ieq_n(line, header_name, target_len)) {
            const char *value = colon + 1;
            size_t value_len;
            while (*value == ' ' || *value == '\t')
                value++;
            value_len = line_len > (size_t)(value - line) ? line_len - (size_t)(value - line) : 0;
            if (out && out_cap > 0) {
                size_t write_len = value_len < out_cap - 1 ? value_len : out_cap - 1;
                memcpy(out, value, write_len);
                out[write_len] = '\0';
            }
            return out_cap == 0 || value_len < out_cap;
        }
        if (!line_end)
            break;
        line = line_end + 2;
    }

    return 0;
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief Finalize a Multipart by releasing all native part copies and storage.
/// @param obj Multipart payload being finalized; may be null.
static void rt_multipart_finalize(void *obj) {
    if (!obj)
        return;
    rt_multipart_impl *mp = (rt_multipart_impl *)obj;
    for (int i = 0; i < mp->part_count; i++) {
        free(mp->parts[i].name);
        free(mp->parts[i].filename);
        free(mp->parts[i].data);
    }
    free(mp->parts);
    mp->parts = NULL;
    mp->part_count = 0;
    mp->part_capacity = 0;
}

//=============================================================================
// Public API — Builder
//=============================================================================

/// @brief Construct a multipart/form-data builder with a random boundary.
/// @details A 40-character unbiased alphanumeric boundary provides roughly 238
///          bits of entropy. A recovery frame covers both managed allocation
///          and entropy generation so a failed constructor cannot leave a
///          partially initialized object registered with the heap.
/// @return Caller-owned Multipart object, or NULL after a returning trap hook.
void *rt_multipart_new(void) {
    rt_multipart_impl *volatile mp = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(
            saved_error, sizeof(saved_error), "Multipart: constructor initialization failed");
        rt_trap_clear_recovery();
        multipart_release_owned((void *)mp);
        rt_trap(saved_error);
        return NULL;
    }

    mp = (rt_multipart_impl *)rt_obj_new_i64(RT_MULTIPART_CLASS_ID,
                                             (int64_t)sizeof(rt_multipart_impl));
    if (!mp) {
        rt_trap("Multipart: memory allocation failed");
        rt_trap_clear_recovery();
        return NULL;
    }
    memset((void *)mp, 0, sizeof(*mp));
    rt_obj_set_finalizer((void *)mp, rt_multipart_finalize);
    generate_boundary((char *)mp->boundary, sizeof(mp->boundary));
    rt_trap_clear_recovery();
    return (void *)mp;
}

/// @brief Append a text field (`Content-Disposition: form-data; name="..."`).
/// @details Returns the builder for fluent chaining. Part storage grows on demand and traps on
///          null input, invalid names, integer overflow, or allocation failure.
/// @param obj Multipart receiver.
/// @param name Nonempty field-name String without embedded nulls.
/// @param value Field bytes copied by exact runtime String length.
/// @return The original Multipart for chaining, or null for an invalid receiver.
void *rt_multipart_add_field(void *obj, rt_string name, rt_string value) {
    rt_multipart_impl *mp = multipart_require(obj, "Multipart.AddField: invalid receiver");
    if (!mp)
        return NULL;

    const char *n = NULL;
    if (!multipart_header_param_view(name, NULL, "Multipart: invalid field name", 0, 1, &n)) {
        return obj;
    }
    if (!value || !rt_string_is_handle(value)) {
        rt_trap("Multipart: invalid field value");
        return obj;
    }
    const char *v = rt_string_cstr(value);
    int64_t v_len64 = rt_str_len(value);
    if (!v || v_len64 < 0 || (uint64_t)v_len64 > (uint64_t)SIZE_MAX) {
        rt_trap("Multipart: invalid field value");
        return obj;
    }
    size_t v_len = (size_t)v_len64;
    if (mp->part_count == INT_MAX || !multipart_reserve_parts(mp, mp->part_count + 1)) {
        rt_trap("Multipart: part storage allocation failed");
        return obj;
    }

    char *name_copy = strdup(n);
    uint8_t *data_copy = (uint8_t *)malloc(v_len > 0 ? v_len : 1);
    if (!name_copy || !data_copy) {
        free(name_copy);
        free(data_copy);
        rt_trap("Multipart: memory allocation failed");
        return obj;
    }
    if (v_len > 0)
        memcpy(data_copy, v, v_len);

    multipart_part_t *part = &mp->parts[mp->part_count++];
    part->name = name_copy;
    part->filename = NULL;
    part->data = data_copy;
    part->data_len = v_len;
    part->is_file = 0;

    return obj;
}

/// @brief Append a file part (`Content-Disposition: form-data; name="..."; filename="..."`,
/// `Content-Type: application/octet-stream`). `data` is a Bytes object (the raw file contents).
/// @details Returns the builder for chaining. A null filename defaults to `file`
///          and null data represents an empty file.
/// @param obj Multipart receiver.
/// @param name Nonempty file field name without embedded nulls.
/// @param filename Optional filename String.
/// @param data Optional Bytes payload.
/// @return The original Multipart for chaining, or null for an invalid receiver.
void *rt_multipart_add_file(void *obj, rt_string name, rt_string filename, void *data) {
    rt_multipart_impl *mp = multipart_require(obj, "Multipart.AddFile: invalid receiver");
    if (!mp)
        return NULL;

    const char *n = NULL;
    const char *fn = NULL;
    if (!multipart_header_param_view(name, NULL, "Multipart: invalid file field name", 0, 1, &n) ||
        !multipart_header_param_view(filename, "file", "Multipart: invalid filename", 1, 0, &fn)) {
        return obj;
    }
    if (data && !rt_bytes_is_bytes(data)) {
        rt_trap("Multipart: invalid file data");
        return obj;
    }
    int64_t len = data ? bytes_len_impl(data) : 0;
    uint8_t *ptr = data ? bytes_data(data) : NULL;
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX || (len > 0 && !ptr)) {
        rt_trap("Multipart: invalid file data length");
        return obj;
    }
    size_t data_len = (size_t)len;
    if (mp->part_count == INT_MAX || !multipart_reserve_parts(mp, mp->part_count + 1)) {
        rt_trap("Multipart: part storage allocation failed");
        return obj;
    }
    char *name_copy = strdup(n);
    char *filename_copy = strdup(fn);
    uint8_t *data_copy = (uint8_t *)malloc(data_len > 0 ? data_len : 1);
    if (!name_copy || !filename_copy || !data_copy) {
        free(name_copy);
        free(filename_copy);
        free(data_copy);
        rt_trap("Multipart: memory allocation failed");
        return obj;
    }
    if (data_len > 0 && ptr)
        memcpy(data_copy, ptr, data_len);

    multipart_part_t *part = &mp->parts[mp->part_count++];
    part->name = name_copy;
    part->filename = filename_copy;
    part->data = data_copy;
    part->data_len = data_len;
    part->is_file = 1;

    return obj;
}

/// @brief Return the full Content-Type value including the random boundary.
/// @param obj Multipart receiver; NULL preserves the historical empty result.
/// @return Caller-owned header String.
rt_string rt_multipart_content_type(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_multipart_impl *mp = multipart_require(obj, "Multipart.ContentType: invalid receiver");
    if (!mp)
        return rt_str_empty();

    char buf[128];
    snprintf(buf, sizeof(buf), "multipart/form-data; boundary=%s", mp->boundary);
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Serialize all parts to a single Bytes object suitable as an HTTP body.
/// @details Computes the exact size with overflow checks, allocates one native
///          staging buffer, and writes escaped parameters directly without a
///          per-part temporary allocation. A recovery frame around the final
///          managed Bytes construction releases native staging before any trap
///          propagates. Layout follows RFC 2046 CRLF framing and always includes
///          the closing `--boundary--` delimiter.
/// @param obj Valid Multipart receiver.
/// @return Caller-owned Bytes, or NULL after a returning trap hook.
void *rt_multipart_build(void *obj) {
    rt_multipart_impl *mp = multipart_require(obj, "Multipart.Build: invalid receiver");
    if (!mp)
        return NULL;

    size_t total = 0;
    size_t blen = strlen(mp->boundary);
    for (int i = 0; i < mp->part_count; i++) {
        multipart_part_t *part = &mp->parts[i];
        size_t escaped_name_len = 0;
        size_t escaped_filename_len = 0;
        if (!multipart_escaped_quoted_length(part->name, &escaped_name_len) ||
            !multipart_escaped_quoted_length(part->filename, &escaped_filename_len)) {
            rt_trap("Multipart: message too large");
            return NULL;
        }
        if (!multipart_size_add(&total, 2 + blen + 2)) {
            rt_trap("Multipart: message too large");
            return NULL;
        }
        if (part->is_file) {
            if (!multipart_size_add(
                    &total,
                    strlen("Content-Disposition: form-data; name=\"\"; filename=\"\"\r\n"
                           "Content-Type: application/octet-stream\r\n\r\n")) ||
                !multipart_size_add(&total, escaped_name_len) ||
                !multipart_size_add(&total, escaped_filename_len)) {
                rt_trap("Multipart: message too large");
                return NULL;
            }
        } else {
            if (!multipart_size_add(&total,
                                    strlen("Content-Disposition: form-data; name=\"\"\r\n\r\n")) ||
                !multipart_size_add(&total, escaped_name_len)) {
                rt_trap("Multipart: message too large");
                return NULL;
            }
        }
        if (!multipart_size_add(&total, part->data_len) || !multipart_size_add(&total, 2)) {
            rt_trap("Multipart: message too large");
            return NULL;
        }
    }
    if (!multipart_size_add(&total, 2 + blen + 4) || total > (size_t)INT64_MAX) {
        rt_trap("Multipart: message too large");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        rt_trap("Multipart: memory allocation failed");
        return NULL;
    }

    size_t pos = 0;
    for (int i = 0; i < mp->part_count; i++) {
        multipart_part_t *part = &mp->parts[i];
        const char boundary_prefix[] = "--";
        const char disposition_prefix[] = "Content-Disposition: form-data; name=\"";
        const char file_name_prefix[] = "\"; filename=\"";
        const char file_suffix[] = "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
        const char field_suffix[] = "\"\r\n\r\n";
        if (!multipart_append_bytes(
                buf, total, &pos, boundary_prefix, sizeof(boundary_prefix) - 1) ||
            !multipart_append_bytes(buf, total, &pos, mp->boundary, blen) ||
            !multipart_append_bytes(buf, total, &pos, "\r\n", 2) ||
            !multipart_append_bytes(
                buf, total, &pos, disposition_prefix, sizeof(disposition_prefix) - 1) ||
            !multipart_append_escaped_quoted(buf, total, &pos, part->name)) {
            free(buf);
            rt_trap("Multipart: serialization failed");
            return NULL;
        }
        if (part->is_file) {
            if (!multipart_append_bytes(
                    buf, total, &pos, file_name_prefix, sizeof(file_name_prefix) - 1) ||
                !multipart_append_escaped_quoted(buf, total, &pos, part->filename) ||
                !multipart_append_bytes(buf, total, &pos, file_suffix, sizeof(file_suffix) - 1)) {
                free(buf);
                rt_trap("Multipart: serialization failed");
                return NULL;
            }
        } else {
            if (!multipart_append_bytes(buf, total, &pos, field_suffix, sizeof(field_suffix) - 1)) {
                free(buf);
                rt_trap("Multipart: serialization failed");
                return NULL;
            }
        }
        if (!multipart_append_bytes(buf, total, &pos, part->data, part->data_len) ||
            !multipart_append_bytes(buf, total, &pos, "\r\n", 2)) {
            free(buf);
            rt_trap("Multipart: serialization failed");
            return NULL;
        }
    }

    if (!multipart_append_bytes(buf, total, &pos, "--", 2) ||
        !multipart_append_bytes(buf, total, &pos, mp->boundary, blen) ||
        !multipart_append_bytes(buf, total, &pos, "--\r\n", 4) || pos != total) {
        free(buf);
        rt_trap("Multipart: serialization failed");
        return NULL;
    }

    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(
            saved_error, sizeof(saved_error), "Multipart: result allocation failed");
        rt_trap_clear_recovery();
        free(buf);
        multipart_release_owned((void *)result);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_bytes_new((int64_t)pos);
    if (!result)
        rt_trap("Multipart: result allocation failed");
    uint8_t *result_data = bytes_data((void *)result);
    if (pos > 0 && !result_data)
        rt_trap("Multipart: result storage unavailable");
    if (pos > 0)
        memcpy(result_data, buf, pos);
    rt_trap_clear_recovery();
    free(buf);
    return (void *)result;
}

/// @brief Return the count of elements in a validated Multipart.
/// @param obj Multipart receiver; null reports zero.
/// @return Nonnegative appended or parsed part count.
int64_t rt_multipart_count(void *obj) {
    if (!obj)
        return 0;
    rt_multipart_impl *mp = multipart_require(obj, "Multipart.Count: invalid receiver");
    return mp ? mp->part_count : 0;
}

//=============================================================================
// Public API — Parser
//=============================================================================

/// @brief Parse a received multipart body into a navigable Multipart object.
/// @details Extracts the boundary from `content_type` (handles both quoted `"..."` and bare
///          forms), then walks the body finding `--boundary` delimiters and per-part
///          `Content-Disposition` headers. Captures `name=` and optional `filename=` attributes;
///          presence of the latter flags the part as a file. The parser rejects bodies larger
///          than @c MULTIPART_MAX_PARSED_BODY_BYTES to bound memory amplification.
///
///          Parsing is STRICT and ATOMIC (VDOC-146): invalid content types or
///          boundaries, oversized bodies, missing delimiters, malformed part
///          headers, truncated bodies (no closing boundary), and allocation
///          failures all trap instead of returning an empty or partial object,
///          so a returned Multipart always represents the complete input. Use
///          `ParseResult` for a non-trapping `Result`-returning variant.
/// @param content_type Content-Type String containing a valid boundary parameter.
/// @param body Bytes containing the complete bounded multipart body.
/// @return Caller-owned complete Multipart, or null after a returning parse trap.
void *rt_multipart_parse(rt_string content_type, void *body) {
    if (!body || !rt_bytes_is_bytes(body)) {
        rt_trap("Multipart: invalid body");
        return NULL;
    }

    const char *ct =
        content_type && rt_string_is_handle(content_type) ? rt_string_cstr(content_type) : NULL;
    if (!ct || multipart_string_has_embedded_nul(content_type)) {
        rt_trap("Multipart: invalid content type");
        return NULL;
    }

    // Extract boundary from content-type
    char boundary[128] = {0};
    if (!multipart_extract_param_value(ct, "boundary", boundary, sizeof(boundary)) ||
        !boundary[0] || !multipart_boundary_is_valid(boundary)) {
        rt_trap("Multipart: invalid or missing boundary");
        return NULL;
    }

    int64_t body_len = bytes_len_impl(body);
    const uint8_t *data = bytes_data(body);
    if (!data || body_len <= 0) {
        rt_trap("Multipart: missing boundary delimiter");
        return NULL;
    }
    if ((uint64_t)body_len > (uint64_t)SIZE_MAX ||
        (uint64_t)body_len > (uint64_t)MULTIPART_MAX_PARSED_BODY_BYTES) {
        rt_trap("Multipart: body too large");
        return NULL;
    }

    char delim[140];
    int dlen = snprintf(delim, sizeof(delim), "--%s", boundary);
    if (dlen <= 0 || (size_t)dlen >= sizeof(delim)) {
        rt_trap("Multipart: invalid or missing boundary");
        return NULL;
    }

    const uint8_t *s = data;
    const uint8_t *s_end = s + (size_t)body_len;
    const uint8_t *p =
        multipart_find_boundary_line(s, (size_t)body_len, (const uint8_t *)delim, (size_t)dlen);
    if (!p) {
        rt_trap("Multipart: missing boundary delimiter");
        return NULL;
    }

    rt_multipart_impl *volatile mp = NULL;
    char *volatile header_text = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(saved_error, sizeof(saved_error), "Multipart: parse failed");
        rt_trap_clear_recovery();
        free((void *)header_text);
        multipart_release_owned((void *)mp);
        rt_trap(saved_error);
        return NULL;
    }

    mp = (rt_multipart_impl *)rt_obj_new_i64(RT_MULTIPART_CLASS_ID,
                                             (int64_t)sizeof(rt_multipart_impl));
    if (!mp)
        rt_trap("Multipart: memory allocation failed");
    memset((void *)mp, 0, sizeof(*mp));
    memcpy((char *)mp->boundary, boundary, sizeof(mp->boundary) - 1);
    rt_obj_set_finalizer((void *)mp, rt_multipart_finalize);

    int saw_closing_boundary = 0;
    while (p && p < s_end) {
        p += dlen;
        if (p + 1 < s_end && p[0] == '-' && p[1] == '-') {
            p += 2;
            if (p != s_end && (p + 1 >= s_end || p[0] != '\r' || p[1] != '\n')) {
                rt_trap("Multipart: malformed closing boundary");
                goto returned_trap;
            }
            saw_closing_boundary = 1;
            break;
        }
        if (p + 1 >= s_end || p[0] != '\r' || p[1] != '\n') {
            rt_trap("Multipart: malformed boundary delimiter");
            goto returned_trap;
        }
        p += 2;

        const uint8_t header_sep[] = {'\r', '\n', '\r', '\n'};
        const uint8_t *headers_end =
            find_bytes(p, (size_t)(s_end - p), header_sep, sizeof(header_sep));
        if (!headers_end) {
            rt_trap("Multipart: truncated body (incomplete part headers)");
            goto returned_trap;
        }

        char part_name[256] = {0};
        char part_filename[256] = {0};
        int has_name = 0;
        int is_file = 0;
        size_t header_len = (size_t)(headers_end - p);
        if (header_len > MULTIPART_MAX_HEADER_BYTES) {
            rt_trap("Multipart: part headers too large");
            goto returned_trap;
        }
        header_text = (char *)malloc(header_len + 1);
        if (!header_text) {
            rt_trap("Multipart: memory allocation failed");
            goto returned_trap;
        }
        if (memchr(p, '\0', header_len)) {
            rt_trap("Multipart: invalid part headers");
            goto returned_trap;
        }
        memcpy((void *)header_text, p, header_len);
        header_text[header_len] = '\0';

        char disposition[512] = {0};
        if (multipart_extract_header_value((const char *)header_text,
                                           "Content-Disposition",
                                           disposition,
                                           sizeof(disposition))) {
            has_name =
                multipart_extract_param_value(disposition, "name", part_name, sizeof(part_name));
            if (multipart_extract_param_value(
                    disposition, "filename", part_filename, sizeof(part_filename))) {
                is_file = 1;
            }
        }
        free((void *)header_text);
        header_text = NULL;

        const uint8_t *data_start = headers_end + 4;
        const uint8_t *next = multipart_find_boundary_line(
            data_start, (size_t)(s_end - data_start), (const uint8_t *)delim, (size_t)dlen);
        if (!next) {
            rt_trap("Multipart: truncated body (missing closing boundary)");
            goto returned_trap;
        }
        if (next < data_start + 2 || next[-2] != '\r' || next[-1] != '\n') {
            rt_trap("Multipart: malformed part framing");
            goto returned_trap;
        }

        size_t data_size = (size_t)((next - 2) - data_start);
        if (!has_name || !part_name[0] || !multipart_header_param_is_valid(part_name) ||
            (is_file && !multipart_header_param_is_valid(part_filename))) {
            rt_trap("Multipart: malformed part header");
            goto returned_trap;
        }
        if (mp->part_count == INT_MAX ||
            !multipart_reserve_parts((rt_multipart_impl *)mp, mp->part_count + 1)) {
            rt_trap("Multipart: memory allocation failed");
            goto returned_trap;
        }

        char *name_copy = strdup(part_name);
        char *filename_copy = is_file ? strdup(part_filename) : NULL;
        uint8_t *data_copy = (uint8_t *)malloc(data_size > 0 ? data_size : 1);
        if (!name_copy || (is_file && !filename_copy) || !data_copy) {
            free(name_copy);
            free(filename_copy);
            free(data_copy);
            rt_trap("Multipart: memory allocation failed");
            goto returned_trap;
        }
        if (data_size > 0)
            memcpy(data_copy, data_start, data_size);

        multipart_part_t *part = &mp->parts[mp->part_count++];
        part->name = name_copy;
        part->filename = filename_copy;
        part->data = data_copy;
        part->data_len = data_size;
        part->is_file = is_file;

        p = next;
    }

    if (!saw_closing_boundary) {
        rt_trap("Multipart: truncated body (missing closing boundary)");
        goto returned_trap;
    }

    rt_trap_clear_recovery();
    return (void *)mp;

returned_trap:
    rt_trap_clear_recovery();
    free((void *)header_text);
    multipart_release_owned((void *)mp);
    return NULL;
}

/// @brief Build `Result.ErrStr` from stable native diagnostic bytes.
/// @details A recovery frame covers both the diagnostic String and Result
///          allocations. It releases either partial value before propagating
///          allocation failure, and drops the String's initial reference after
///          the Result has retained it.
/// @param message NUL-terminated diagnostic bytes.
/// @return Caller-owned error Result.
static void *multipart_error_result(const char *message) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(
            saved_error, sizeof(saved_error), "Multipart: error Result allocation failed");
        rt_trap_clear_recovery();
        multipart_release_owned((void *)result);
        multipart_release_owned((void *)error_string);
        rt_trap(saved_error);
        return NULL;
    }
    const char *stable = message && message[0] ? message : "Multipart: parse failed";
    error_string = rt_string_from_bytes(stable, strlen(stable));
    if (!error_string)
        rt_trap("Multipart: error message allocation failed");
    result = rt_result_err_str((rt_string)error_string);
    if (!result)
        rt_trap("Multipart: error Result allocation failed");
    rt_trap_clear_recovery();
    multipart_release_owned((void *)error_string);
    return (void *)result;
}

/// @brief Wrap and consume one caller-owned Multipart in `Result.Ok`.
/// @details `rt_result_ok` retains the payload. This helper drops the initial
///          parse reference after success and also releases it if Result
///          allocation or retain validation traps.
/// @param multipart Owned Multipart reference to consume.
/// @return Caller-owned success Result.
static void *multipart_success_result_owned(void *multipart) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(
            saved_error, sizeof(saved_error), "Multipart: success Result allocation failed");
        rt_trap_clear_recovery();
        multipart_release_owned((void *)result);
        multipart_release_owned(multipart);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_result_ok(multipart);
    if (!result)
        rt_trap("Multipart: success Result allocation failed");
    rt_trap_clear_recovery();
    multipart_release_owned(multipart);
    return (void *)result;
}

/// @brief Non-trapping companion to @ref rt_multipart_parse.
/// @details Converts parser traps into `Result.ErrStr`, copying the diagnostic
///          before clearing recovery. Result construction occurs under a fresh
///          recovery frame, avoiding the historical loop that retried a failed
///          diagnostic allocation against the same `setjmp` target.
/// @param content_type Candidate Content-Type String.
/// @param body Candidate Bytes body.
/// @return Caller-owned success or error Result.
void *rt_multipart_parse_result(rt_string content_type, void *body) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        multipart_save_trap(saved_error, sizeof(saved_error), "Multipart: parse failed");
        rt_trap_clear_recovery();
        return multipart_error_result(saved_error);
    }
    void *mp = rt_multipart_parse(content_type, body);
    rt_trap_clear_recovery();
    if (!mp)
        return multipart_error_result("Multipart: parse returned no value");
    return multipart_success_result_owned(mp);
}

/// @brief True when a non-file field with @p name exists (distinguishes a
///        present-but-empty field from a missing one, VDOC-146).
/// @param obj Candidate Multipart receiver.
/// @param name Exact case-sensitive field name.
/// @return One when a matching non-file part exists; zero otherwise.
int8_t rt_multipart_has_field(void *obj, rt_string name) {
    if (!multipart_is_handle(obj) || !name || !rt_string_is_handle(name))
        return 0;
    rt_multipart_impl *mp = (rt_multipart_impl *)obj;
    const char *n = rt_string_cstr(name);
    if (!n || multipart_string_has_embedded_nul(name))
        return 0;
    for (int i = 0; i < mp->part_count; i++) {
        if (!mp->parts[i].is_file && mp->parts[i].name && strcmp(mp->parts[i].name, n) == 0)
            return 1;
    }
    return 0;
}

/// @brief True when a file part with @p name exists (distinguishes a present
///        zero-byte file from a missing one, VDOC-146).
/// @param obj Candidate Multipart receiver.
/// @param name Exact case-sensitive file field name.
/// @return One when a matching file part exists; zero otherwise.
int8_t rt_multipart_has_file(void *obj, rt_string name) {
    if (!multipart_is_handle(obj) || !name || !rt_string_is_handle(name))
        return 0;
    rt_multipart_impl *mp = (rt_multipart_impl *)obj;
    const char *n = rt_string_cstr(name);
    if (!n || multipart_string_has_embedded_nul(name))
        return 0;
    for (int i = 0; i < mp->part_count; i++) {
        if (mp->parts[i].is_file && mp->parts[i].name && strcmp(mp->parts[i].name, n) == 0)
            return 1;
    }
    return 0;
}

/// @brief Look up a non-file field by name and return its value, or empty if not found.
/// @details Use `HasField` to distinguish a missing field from a present empty one.
/// @param obj Candidate Multipart receiver.
/// @param name Exact case-sensitive field name.
/// @return Caller-owned exact-value String, or an owned empty String when absent or invalid.
rt_string rt_multipart_get_field(void *obj, rt_string name) {
    if (!multipart_is_handle(obj) || !name || !rt_string_is_handle(name))
        return rt_str_empty();
    rt_multipart_impl *mp = (rt_multipart_impl *)obj;
    const char *n = rt_string_cstr(name);
    if (!n || multipart_string_has_embedded_nul(name))
        return rt_str_empty();

    for (int i = 0; i < mp->part_count; i++) {
        if (!mp->parts[i].is_file && mp->parts[i].name && strcmp(mp->parts[i].name, n) == 0) {
            return rt_string_from_bytes((const char *)mp->parts[i].data, mp->parts[i].data_len);
        }
    }
    return rt_str_empty();
}

/// @brief Look up a file part by name and return its raw contents as Bytes. Returns empty Bytes
/// if the name doesn't match any file part (use `_get_field` for non-file parts).
/// @param obj Candidate Multipart receiver.
/// @param name Exact case-sensitive file field name.
/// @return Caller-owned Bytes copy, empty when absent or invalid.
void *rt_multipart_get_file(void *obj, rt_string name) {
    if (!multipart_is_handle(obj) || !name || !rt_string_is_handle(name))
        return rt_bytes_new(0);
    rt_multipart_impl *mp = (rt_multipart_impl *)obj;
    const char *n = rt_string_cstr(name);
    if (!n || multipart_string_has_embedded_nul(name))
        return rt_bytes_new(0);

    for (int i = 0; i < mp->part_count; i++) {
        if (mp->parts[i].is_file && mp->parts[i].name && strcmp(mp->parts[i].name, n) == 0) {
            void *result = rt_bytes_new((int64_t)mp->parts[i].data_len);
            if (mp->parts[i].data_len > 0)
                memcpy(bytes_data(result), mp->parts[i].data, mp->parts[i].data_len);
            return result;
        }
    }
    return rt_bytes_new(0);
}
