//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_asset_error.c
// Purpose: Thread-local diagnostics for recoverable runtime asset load failures.
// Key invariants:
//   - The last error describes the most recent top-level failed content load.
//   - Warnings survive a successful top-level load so callers can inspect degradation.
// Ownership/Lifetime:
//   - All buffers are fixed-size thread-local storage owned by this module.
//   - Getter functions allocate rt_string values for script-facing access.
// Links: rt_asset_error.h, docs/zannalib/graphics/rendering3d.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_asset_error.c
 * @brief Implements thread-local diagnostics and statistics for 3D asset imports.
 *
 * Each thread maintains one nested-load depth, last error, bounded warning
 * list, suppression count, and set of saturating import counters. Beginning a
 * top-level load clears all prior diagnostics; a successful top-level end
 * clears only the error, while failure leaves the complete report available
 * to callers. Script-facing getters copy data into newly allocated runtime
 * strings, whereas internal C getters borrow thread-local buffers.
 */
#include "rt_asset_error.h"

#include "rt_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/// Bytes reserved for the last error, including its null terminator.
#define RT_ASSET_ERROR_MESSAGE_CAP 512
/// Maximum number of visible warning slots retained per thread.
#define RT_ASSET_WARNING_CAP 16
/// Bytes reserved for each warning, including its null terminator.
#define RT_ASSET_WARNING_MESSAGE_CAP 256
/// Capacity of the newline-joined warning and base report storage.
#define RT_ASSET_WARNING_JOINED_CAP                                                                \
    ((RT_ASSET_WARNING_CAP * (RT_ASSET_WARNING_MESSAGE_CAP + 1)) + 64)
/// Capacity proven sufficient for the fixed report plus worst-case `\u00xx` warning escaping.
#define RT_ASSET_IMPORT_REPORT_CAP                                                                 \
    ((RT_ASSET_WARNING_CAP * (((RT_ASSET_WARNING_MESSAGE_CAP - 1) * 6) + 3)) + 2048)

/// Last recoverable asset error code for the current thread.
static RT_THREAD_LOCAL rt_asset_error_code g_asset_error_code = RT_ASSET_ERROR_NONE;
/// Last recoverable asset error text for the current thread.
static RT_THREAD_LOCAL char g_asset_error_message[RT_ASSET_ERROR_MESSAGE_CAP];
/// Whether the last error text exceeded its fixed buffer.
static RT_THREAD_LOCAL int g_asset_error_message_truncated = 0;
/// Bounded visible warning messages for the current thread.
static RT_THREAD_LOCAL char g_asset_warnings[RT_ASSET_WARNING_CAP][RT_ASSET_WARNING_MESSAGE_CAP];
/// Per-visible-slot warning truncation flags.
static RT_THREAD_LOCAL int g_asset_warning_truncated[RT_ASSET_WARNING_CAP];
/// Number of currently visible warning slots.
static RT_THREAD_LOCAL int64_t g_asset_warning_count = 0;
/// Number of warnings hidden by the summary slot, including the displaced final visible warning.
static RT_THREAD_LOCAL int64_t g_asset_warning_suppressed = 0;
/// Nesting depth of asset loaders participating in the current transaction.
static RT_THREAD_LOCAL uint64_t g_asset_load_depth = 0;
/// Saturating import-quality counters indexed by `rt_asset_import_stat`.
static RT_THREAD_LOCAL int64_t g_asset_import_stats[RT_ASSET_IMPORT_STAT_COUNT];

/// @brief Format a diagnostic into bounded storage.
/// @param[out] dst Destination character buffer.
/// @param[in] dst_cap Buffer capacity including the null terminator.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ap Arguments corresponding to `fmt`.
/// @return One when formatting fails or truncates, otherwise zero.
static int asset_error_vformat(char *dst, size_t dst_cap, const char *fmt, va_list ap)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 0)))
#endif
    ;

/// @brief Format an asset diagnostic and report whether storage truncated it.
/// @details Keeps every diagnostic buffer NUL-terminated even when `vsnprintf`
///          fails or the formatted output exceeds @p dst_cap.
/// @param[out] dst Destination buffer.
/// @param[in] dst_cap Destination byte capacity including NUL.
/// @param[in] fmt printf-style format string; NULL is treated as an empty string.
/// @param[in] ap Format argument list.
/// @return 1 when formatted text was truncated; otherwise 0.
static int asset_error_vformat(char *dst, size_t dst_cap, const char *fmt, va_list ap) {
    int written;
    if (!dst || dst_cap == 0)
        return 0;
    if (!fmt)
        fmt = "";
    written = vsnprintf(dst, dst_cap, fmt, ap);
    dst[dst_cap - 1] = '\0';
    return written < 0 || (size_t)written >= dst_cap;
}

/// @brief Map arbitrary integer-backed enum values to the public diagnostic code domain.
/// @param code Caller-supplied error classification.
/// @return @p code when canonical, `NONE` unchanged, or `CORRUPT` for an invalid value.
static rt_asset_error_code asset_error_normalize_code(rt_asset_error_code code) {
    if (code == RT_ASSET_ERROR_NONE)
        return RT_ASSET_ERROR_NONE;
    if (code < RT_ASSET_ERROR_NOT_FOUND || code > RT_ASSET_ERROR_TOO_LARGE)
        return RT_ASSET_ERROR_CORRUPT;
    return code;
}

/// @brief Clear only the current thread's last error.
/// @details Resets the code, message, and message-truncation flag without
/// affecting warnings, import statistics, suppression count, or load depth.
void rt_asset_error_clear_error(void) {
    g_asset_error_code = RT_ASSET_ERROR_NONE;
    g_asset_error_message[0] = '\0';
    g_asset_error_message_truncated = 0;
}

/// @brief Clear all current-thread diagnostics and import statistics.
/// @details Clears the last error, every visible warning slot, the warning
/// count, suppression count, and all import counters. The nesting depth is not
/// modified.
void rt_asset_error_clear(void) {
    rt_asset_error_clear_error();
    g_asset_warning_count = 0;
    g_asset_warning_suppressed = 0;
    for (int64_t i = 0; i < RT_ASSET_WARNING_CAP; i++) {
        g_asset_warnings[i][0] = '\0';
        g_asset_warning_truncated[i] = 0;
    }
    for (int i = 0; i < RT_ASSET_IMPORT_STAT_COUNT; i++)
        g_asset_import_stats[i] = 0;
}

/// @brief Add a positive amount to one saturating import statistic.
/// @param[in] stat Counter selector.
/// @param[in] amount Positive increment.
/// @details Invalid selectors and nonpositive increments are ignored. Overflow
/// saturates the selected counter at `INT64_MAX`.
void rt_asset_error_add_import_stat(rt_asset_import_stat stat, int64_t amount) {
    if (stat < 0 || stat >= RT_ASSET_IMPORT_STAT_COUNT || amount <= 0)
        return;
    if (g_asset_import_stats[stat] > INT64_MAX - amount)
        g_asset_import_stats[stat] = INT64_MAX;
    else
        g_asset_import_stats[stat] += amount;
}

/// @brief Read one current-thread import statistic.
/// @param[in] stat Counter selector.
/// @return The stored value, or zero for an invalid selector.
int64_t rt_asset_error_get_import_stat(rt_asset_import_stat stat) {
    if (stat < 0 || stat >= RT_ASSET_IMPORT_STAT_COUNT)
        return 0;
    return g_asset_import_stats[stat];
}

/// @brief Enter a possibly nested asset-load diagnostic scope.
/// @return Nonzero when this call began the outermost load, otherwise zero.
/// @details Entering at depth zero clears all diagnostics and counters. Nested
/// loaders share the outer transaction's thread-local state.
int rt_asset_error_begin_load(void) {
    if (g_asset_load_depth == 0)
        rt_asset_error_clear();
    if (g_asset_load_depth != UINT64_MAX)
        g_asset_load_depth++;
    return g_asset_load_depth == 1;
}

/// @brief Leave a successful asset-load scope.
/// @details Decrements a positive nesting depth. Once depth reaches zero, the
/// last error is cleared while warnings and import statistics remain available
/// for inspection.
void rt_asset_error_end_load_success(void) {
    if (g_asset_load_depth == 0)
        return;
    g_asset_load_depth--;
    if (g_asset_load_depth == 0)
        rt_asset_error_clear_error();
}

/// @brief Leave a failed asset-load scope while preserving diagnostics.
/// @details Decrements a positive nesting depth but never clears the last
/// error, warnings, or import statistics.
void rt_asset_error_end_load_failure(void) {
    if (g_asset_load_depth > 0)
        g_asset_load_depth--;
}

/// @brief Replace the current thread's last asset error.
/// @param[in] code Error classification to store.
/// @param[in] message Null-terminated message, or `NULL` for empty text.
/// @details The message is copied into bounded thread-local storage and the
/// truncation flag records formatting failure or insufficient capacity.
void rt_asset_error_set(rt_asset_error_code code, const char *message) {
    int written;
    code = asset_error_normalize_code(code);
    if (code == RT_ASSET_ERROR_NONE) {
        rt_asset_error_clear_error();
        return;
    }
    g_asset_error_code = code;
    written = snprintf(
        g_asset_error_message, sizeof(g_asset_error_message), "%s", message ? message : "");
    g_asset_error_message[sizeof(g_asset_error_message) - 1] = '\0';
    g_asset_error_message_truncated =
        written < 0 || (size_t)written >= sizeof(g_asset_error_message);
}

/// @brief Replace the current thread's last asset error using formatted text.
/// @param[in] code Error classification to store.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
/// @details Output is always null-terminated and its truncation status is
/// retained alongside the code.
void rt_asset_error_setf(rt_asset_error_code code, const char *fmt, ...) {
    va_list ap;
    code = asset_error_normalize_code(code);
    if (code == RT_ASSET_ERROR_NONE) {
        rt_asset_error_clear_error();
        return;
    }
    g_asset_error_code = code;
    va_start(ap, fmt);
    g_asset_error_message_truncated =
        asset_error_vformat(g_asset_error_message, sizeof(g_asset_error_message), fmt, ap);
    va_end(ap);
}

/// @brief Store an error only when no earlier error is present.
/// @param[in] code Error classification to store.
/// @param[in] message Null-terminated message, or `NULL` for empty text.
/// @details Preserves the first recorded failure in a nested import chain.
void rt_asset_error_set_if_empty(rt_asset_error_code code, const char *message) {
    if (g_asset_error_code == RT_ASSET_ERROR_NONE)
        rt_asset_error_set(code, message);
}

/// @brief Store a formatted error only when no earlier error is present.
/// @param[in] code Error classification to store.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
/// @details Preserves the first recorded failure in a nested import chain.
void rt_asset_error_setf_if_empty(rt_asset_error_code code, const char *fmt, ...) {
    va_list ap;
    if (g_asset_error_code != RT_ASSET_ERROR_NONE)
        return;
    code = asset_error_normalize_code(code);
    if (code == RT_ASSET_ERROR_NONE) {
        rt_asset_error_clear_error();
        return;
    }
    g_asset_error_code = code;
    va_start(ap, fmt);
    g_asset_error_message_truncated =
        asset_error_vformat(g_asset_error_message, sizeof(g_asset_error_message), fmt, ap);
    va_end(ap);
}

/// @brief Read the current thread's last asset error code.
/// @return The stored code, or `RT_ASSET_ERROR_NONE` after a clear.
rt_asset_error_code rt_asset_error_get_code(void) {
    return g_asset_error_code;
}

/// @brief Borrow the current thread's last asset error message.
/// @return A nonnull pointer to thread-local null-terminated storage.
/// @note The pointer is overwritten by later error operations on the same
/// thread and must not be freed.
const char *rt_asset_error_get_message(void) {
    return g_asset_error_message;
}

/// @brief Append one warning while preserving any truncation from an earlier formatting stage.
/// @param[in] message Null-terminated warning text, or `NULL` for empty text.
/// @param[in] source_truncated Nonzero when @p message was already truncated before this copy.
static void asset_error_add_warning_impl(const char *message, int source_truncated) {
    if (g_asset_warning_count < RT_ASSET_WARNING_CAP) {
        int written = snprintf(g_asset_warnings[g_asset_warning_count],
                               RT_ASSET_WARNING_MESSAGE_CAP,
                               "%s",
                               message ? message : "");
        g_asset_warnings[g_asset_warning_count][RT_ASSET_WARNING_MESSAGE_CAP - 1] = '\0';
        g_asset_warning_truncated[g_asset_warning_count] =
            source_truncated || written < 0 || (size_t)written >= RT_ASSET_WARNING_MESSAGE_CAP;
        g_asset_warning_count++;
        return;
    }
    if (g_asset_warning_suppressed == 0)
        g_asset_warning_suppressed = 2;
    else if (g_asset_warning_suppressed < INT64_MAX)
        g_asset_warning_suppressed++;
    int written = snprintf(g_asset_warnings[RT_ASSET_WARNING_CAP - 1],
                           RT_ASSET_WARNING_MESSAGE_CAP,
                           "%lld more suppressed",
                           (long long)g_asset_warning_suppressed);
    g_asset_warnings[RT_ASSET_WARNING_CAP - 1][RT_ASSET_WARNING_MESSAGE_CAP - 1] = '\0';
    g_asset_warning_truncated[RT_ASSET_WARNING_CAP - 1] =
        written < 0 || (size_t)written >= RT_ASSET_WARNING_MESSAGE_CAP;
}

/// @brief Append one warning to the current thread's bounded warning list.
/// @param[in] message Null-terminated warning text, or `NULL` for empty text.
/// @details The first 16 warnings occupy visible slots. On overflow, the last actual warning is
///          displaced by a cumulative summary and counted together with each incoming warning.
void rt_asset_error_add_warning(const char *message) {
    asset_error_add_warning_impl(message, 0);
}

/// @brief Format and append one warning.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
/// @details Formatting first uses one warning-sized temporary buffer, after
/// which the bounded result follows the ordinary warning insertion policy.
void rt_asset_error_add_warningf(const char *fmt, ...) {
    char message[RT_ASSET_WARNING_MESSAGE_CAP];
    int truncated;
    va_list ap;
    va_start(ap, fmt);
    truncated = asset_error_vformat(message, sizeof(message), fmt, ap);
    va_end(ap);
    asset_error_add_warning_impl(message, truncated);
}

/// @brief Read the number of visible current-thread warning slots.
/// @return A value from zero through `RT_ASSET_WARNING_CAP`.
int64_t rt_asset_error_get_warning_count(void) {
    return g_asset_warning_count;
}

/// @brief Borrow one visible warning message.
/// @param[in] index Zero-based warning slot.
/// @return Thread-local warning text, or a static empty string for an invalid
/// index.
/// @note The returned pointer must not be freed and is invalidated or
/// overwritten by later diagnostic operations on the same thread.
const char *rt_asset_error_get_warning(int64_t index) {
    if (index < 0 || index >= g_asset_warning_count)
        return "";
    return g_asset_warnings[index];
}

/// @brief Test whether the stored last-error message was truncated.
/// @return One when truncated or formatting failed, otherwise zero.
int rt_asset_error_get_message_was_truncated(void) {
    return g_asset_error_message_truncated ? 1 : 0;
}

/// @brief Test whether one visible warning slot was truncated.
/// @param[in] index Zero-based warning slot.
/// @return One when that slot was truncated, otherwise zero; invalid indices
/// also return zero.
int rt_asset_error_get_warning_was_truncated(int64_t index) {
    if (index < 0 || index >= g_asset_warning_count)
        return 0;
    return g_asset_warning_truncated[index] ? 1 : 0;
}

/// @brief Read how many warnings were suppressed after the table filled.
/// @return The current thread's cumulative suppression count.
int64_t rt_asset_error_get_warning_suppressed_count(void) {
    return g_asset_warning_suppressed;
}

/// @brief Copy the last asset-load error into a runtime string.
/// @return A newly allocated string containing the current error message,
/// including an empty string when no message is present.
rt_string rt_assets3d_get_last_load_error(void) {
    const char *message = rt_asset_error_get_message();
    if (!message)
        message = "";
    return rt_string_from_bytes(message, strlen(message));
}

/// @brief Expose the last asset-load error code to the runtime API.
/// @return The current `rt_asset_error_code` converted to `int64_t`.
int64_t rt_assets3d_get_last_load_error_code(void) {
    return (int64_t)rt_asset_error_get_code();
}

/// @brief Expose the number of visible load warnings to the runtime API.
/// @return The current thread's visible warning count.
int64_t rt_assets3d_get_load_warning_count(void) {
    return rt_asset_error_get_warning_count();
}

/// @brief Copy one visible load warning into a runtime string.
/// @param[in] index Zero-based warning slot.
/// @return A newly allocated warning string, or an empty string for an invalid
/// index.
rt_string rt_assets3d_get_load_warning(int64_t index) {
    const char *warning = rt_asset_error_get_warning(index);
    return rt_string_from_bytes(warning, strlen(warning));
}

/// @brief Join all visible load warnings into one runtime string.
/// @return A newly allocated string containing warnings separated by newline
/// characters, or an empty string when none exist.
rt_string rt_assets3d_get_load_warnings(void) {
    char joined[RT_ASSET_WARNING_JOINED_CAP];
    size_t used = 0;
    int64_t count = rt_asset_error_get_warning_count();
    joined[0] = '\0';
    for (int64_t i = 0; i < count; i++) {
        const char *warning = rt_asset_error_get_warning(i);
        size_t len = warning ? strlen(warning) : 0;
        if (i > 0 && used + 1 < sizeof(joined))
            joined[used++] = '\n';
        if (len > sizeof(joined) - used - 1)
            len = sizeof(joined) - used - 1;
        if (len > 0) {
            memcpy(joined + used, warning, len);
            used += len;
        }
        joined[used] = '\0';
        if (used + 1 >= sizeof(joined))
            break;
    }
    return rt_string_from_bytes(joined, used);
}

/// @brief Append @p text to a bounded report buffer, tracking the used length.
/// @param[in,out] dst Null-terminated destination buffer.
/// @param[in] cap Total destination capacity including the null terminator.
/// @param[in,out] used Number of bytes currently written, updated on return.
/// @param[in] text Null-terminated text, or `NULL` for no bytes.
/// @details Text beyond the remaining capacity is silently truncated while the
/// destination remains null-terminated.
static void asset_report_append_bytes(
    char *dst, size_t cap, size_t *used, const char *text, size_t len) {
    if (!dst || !used || cap == 0 || *used >= cap || !text || len == 0)
        return;
    if (len > cap - 1 - *used)
        len = cap - 1 - *used;
    memcpy(dst + *used, text, len);
    *used += len;
    dst[*used] = '\0';
}

/// @brief Append null-terminated text to a bounded report buffer.
/// @param[in,out] dst Null-terminated destination buffer.
/// @param[in] cap Total destination capacity including the null terminator.
/// @param[in,out] used Number of bytes currently written, updated on return.
/// @param[in] text Null-terminated text, or `NULL` for no bytes.
static void asset_report_append(char *dst, size_t cap, size_t *used, const char *text) {
    asset_report_append_bytes(dst, cap, used, text, text ? strlen(text) : 0u);
}

/// @brief Validate one UTF-8 scalar in a null-terminated warning byte span.
/// @param text Source warning bytes.
/// @param len Exact remaining source byte count.
/// @return Encoded byte length from two through four, or zero for malformed UTF-8.
static size_t asset_report_utf8_sequence_length(const unsigned char *text, size_t len) {
    unsigned char b0;
    unsigned char b1;
    if (!text || len == 0)
        return 0u;
    b0 = text[0];
    if (b0 >= 0xC2u && b0 <= 0xDFu)
        return len >= 2u && text[1] >= 0x80u && text[1] <= 0xBFu ? 2u : 0u;
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (len < 3u || text[2] < 0x80u || text[2] > 0xBFu)
            return 0u;
        b1 = text[1];
        if (b0 == 0xE0u)
            return b1 >= 0xA0u && b1 <= 0xBFu ? 3u : 0u;
        if (b0 == 0xEDu)
            return b1 >= 0x80u && b1 <= 0x9Fu ? 3u : 0u;
        return b1 >= 0x80u && b1 <= 0xBFu ? 3u : 0u;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (len < 4u || text[2] < 0x80u || text[2] > 0xBFu || text[3] < 0x80u || text[3] > 0xBFu)
            return 0u;
        b1 = text[1];
        if (b0 == 0xF0u)
            return b1 >= 0x90u && b1 <= 0xBFu ? 4u : 0u;
        if (b0 == 0xF4u)
            return b1 >= 0x80u && b1 <= 0x8Fu ? 4u : 0u;
        return b1 >= 0x80u && b1 <= 0xBFu ? 4u : 0u;
    }
    return 0u;
}

/// @brief Append @p text as a JSON string literal (quoted, minimally escaped).
/// @param[in,out] dst Null-terminated destination buffer.
/// @param[in] cap Total destination capacity including the null terminator.
/// @param[in,out] used Number of bytes currently written, updated on return.
/// @param[in] text Text to quote, or `NULL` for an empty string.
/// @details Quotes, backslashes, common whitespace controls, and remaining
/// bytes below `0x20` are escaped before bounded append.
static void asset_report_append_json_string(char *dst, size_t cap, size_t *used, const char *text) {
    size_t text_len = text ? strlen(text) : 0u;
    size_t pos = 0u;
    asset_report_append(dst, cap, used, "\"");
    while (pos < text_len) {
        char escaped[8];
        unsigned char ch = (unsigned char)text[pos];
        if (ch == '"' || ch == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)ch;
            escaped[2] = '\0';
            pos++;
        } else if (ch == '\n') {
            memcpy(escaped, "\\n", 3);
            pos++;
        } else if (ch == '\r') {
            memcpy(escaped, "\\r", 3);
            pos++;
        } else if (ch == '\t') {
            memcpy(escaped, "\\t", 3);
            pos++;
        } else if (ch < 0x20u) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)ch);
            pos++;
        } else if (ch >= 0x80u) {
            size_t sequence_length = asset_report_utf8_sequence_length(
                (const unsigned char *)text + pos, text_len - pos);
            if (sequence_length != 0u) {
                asset_report_append_bytes(dst, cap, used, text + pos, sequence_length);
                pos += sequence_length;
                continue;
            }
            snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)ch);
            pos++;
        } else {
            escaped[0] = (char)ch;
            escaped[1] = '\0';
            pos++;
        }
        asset_report_append(dst, cap, used, escaped);
    }
    asset_report_append(dst, cap, used, "\"");
}

/// @brief Append one `"key":value` counter field (with optional leading comma).
/// @param[in,out] dst Null-terminated destination buffer.
/// @param[in] cap Total destination capacity including the null terminator.
/// @param[in,out] used Number of bytes currently written, updated on return.
/// @param[in] key Trusted internal JSON field name.
/// @param[in] value Signed counter value.
/// @param[in] leading_comma Nonzero to prefix the field with a comma.
static void asset_report_append_counter(
    char *dst, size_t cap, size_t *used, const char *key, int64_t value, int leading_comma) {
    char field[96];
    snprintf(
        field, sizeof(field), "%s\"%s\":%lld", leading_comma ? "," : "", key, (long long)value);
    asset_report_append(dst, cap, used, field);
}

/// @brief Build the current thread's machine-readable asset import report.
/// @return A newly allocated runtime string containing a complete JSON object
/// with import counters, suppression count, and visible warnings.
/// @details The fixed buffer covers the exact worst case where every retained warning byte needs
/// a six-byte JSON escape. Malformed UTF-8 is escaped bytewise so output remains strict JSON.
rt_string rt_assets3d_get_import_report(void) {
    char report[RT_ASSET_IMPORT_REPORT_CAP];
    size_t used = 0;
    int64_t count = rt_asset_error_get_warning_count();
    report[0] = '\0';
    asset_report_append(report, sizeof(report), &used, "{");
    asset_report_append_counter(report,
                                sizeof(report),
                                &used,
                                "skippedPrimitives",
                                g_asset_import_stats[RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES],
                                0);
    asset_report_append_counter(
        report,
        sizeof(report),
        &used,
        "truncatedInfluenceVertices",
        g_asset_import_stats[RT_ASSET_IMPORT_STAT_TRUNCATED_INFLUENCE_VERTICES],
        1);
    asset_report_append_counter(
        report,
        sizeof(report),
        &used,
        "outOfRangeJointVertices",
        g_asset_import_stats[RT_ASSET_IMPORT_STAT_OUT_OF_RANGE_JOINT_VERTICES],
        1);
    asset_report_append_counter(report,
                                sizeof(report),
                                &used,
                                "ignoredExtensions",
                                g_asset_import_stats[RT_ASSET_IMPORT_STAT_IGNORED_EXTENSIONS],
                                1);
    asset_report_append_counter(
        report,
        sizeof(report),
        &used,
        "bakedCubicSplineChannels",
        g_asset_import_stats[RT_ASSET_IMPORT_STAT_BAKED_CUBIC_SPLINE_CHANNELS],
        1);
    asset_report_append_counter(
        report,
        sizeof(report),
        &used,
        "compressedAnimationKeysDropped",
        g_asset_import_stats[RT_ASSET_IMPORT_STAT_COMPRESSED_ANIMATION_KEYS_DROPPED],
        1);
    asset_report_append_counter(
        report, sizeof(report), &used, "suppressedWarnings", g_asset_warning_suppressed, 1);
    asset_report_append(report, sizeof(report), &used, ",\"warnings\":[");
    for (int64_t i = 0; i < count; i++) {
        if (i > 0)
            asset_report_append(report, sizeof(report), &used, ",");
        asset_report_append_json_string(
            report, sizeof(report), &used, rt_asset_error_get_warning(i));
    }
    asset_report_append(report, sizeof(report), &used, "]}");
    return rt_string_from_bytes(report, used);
}
