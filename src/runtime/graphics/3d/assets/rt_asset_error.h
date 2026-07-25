//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_asset_error.h
// Purpose: Thread-local diagnostics for recoverable runtime asset load failures.
// Key invariants:
//   - Content failures are reported as last-error state instead of traps.
//   - Partial degradation is reported through a bounded warning list.
// Ownership/Lifetime:
//   - Diagnostic strings live in thread-local storage owned by this module.
//   - Runtime string getters return freshly allocated rt_string handles.
// Links: rt_asset_error.c, docs/zannalib/graphics/rendering3d.md
//
//===----------------------------------------------------------------------===//
/**
 * @file rt_asset_error.h
 * @brief Declares thread-local error, warning, and import-report APIs.
 *
 * Recoverable asset loaders use these functions to preserve the first or last
 * failure, accumulate bounded degradation warnings, and expose structured
 * counters across nested load operations. Internal C-string getters borrow
 * thread-local storage; `rt_assets3d_*` string getters allocate runtime strings
 * owned by the caller.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
/// Attach compiler `printf` argument checking to variadic diagnostic APIs.
#define RT_ASSET_ERROR_PRINTF(fmt_index, first_arg)                                                \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
/// No-op format-checking annotation for compilers without GNU attributes.
#define RT_ASSET_ERROR_PRINTF(fmt_index, first_arg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Classification of recoverable 3D asset-load failures.
typedef enum rt_asset_error_code {
    /// No error has been recorded.
    RT_ASSET_ERROR_NONE = 0,
    /// Requested asset path does not exist.
    RT_ASSET_ERROR_NOT_FOUND = 1,
    /// Asset exists but its bytes cannot be read.
    RT_ASSET_ERROR_UNREADABLE = 2,
    /// File signature does not match the requested or detected format.
    RT_ASSET_ERROR_BAD_MAGIC = 3,
    /// Input structure or payload is malformed.
    RT_ASSET_ERROR_CORRUPT = 4,
    /// Valid content requests a feature or format the loader cannot import.
    RT_ASSET_ERROR_UNSUPPORTED = 5,
    /// Input exceeds a defensive size or count limit.
    RT_ASSET_ERROR_TOO_LARGE = 6
} rt_asset_error_code;

/// @brief Clear the current thread's error, warnings, and import counters.
/// @details Does not change the nested load depth.
void rt_asset_error_clear(void);
/// @brief Clear only the current thread's error code, text, and truncation flag.
void rt_asset_error_clear_error(void);
/// @brief Begin a nested asset-load diagnostic scope.
/// @return Nonzero for the outermost scope, which also clears prior
/// diagnostics; zero for a nested scope.
int rt_asset_error_begin_load(void);
/// @brief End a successful load scope and clear the error at outermost exit.
/// @details Warnings and statistics remain available after success.
void rt_asset_error_end_load_success(void);
/// @brief End a failed load scope without clearing diagnostics.
void rt_asset_error_end_load_failure(void);
/// @brief Replace the last error with copied bounded text.
/// @param[in] code Error classification.
/// @param[in] message Null-terminated text, or `NULL` for empty text.
void rt_asset_error_set(rt_asset_error_code code, const char *message);
/// @brief Replace the last error with bounded formatted text.
/// @param[in] code Error classification.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
void rt_asset_error_setf(rt_asset_error_code code, const char *fmt, ...)
    RT_ASSET_ERROR_PRINTF(2, 3);
/// @brief Store copied error text only when no error is currently recorded.
/// @param[in] code Error classification.
/// @param[in] message Null-terminated text, or `NULL` for empty text.
void rt_asset_error_set_if_empty(rt_asset_error_code code, const char *message);
/// @brief Store formatted error text only when no error is currently recorded.
/// @param[in] code Error classification.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
void rt_asset_error_setf_if_empty(rt_asset_error_code code, const char *fmt, ...)
    RT_ASSET_ERROR_PRINTF(2, 3);
/// @brief Read the current thread's last error code.
/// @return The stored code or `RT_ASSET_ERROR_NONE`.
rt_asset_error_code rt_asset_error_get_code(void);
/// @brief Borrow the current thread's last error text.
/// @return A nonnull thread-local null-terminated buffer.
/// @note The caller must not free the pointer; later diagnostic operations on
/// the same thread may overwrite it.
const char *rt_asset_error_get_message(void);
/// @brief Append copied text to the bounded warning list.
/// @param[in] message Null-terminated text, or `NULL` for an empty warning.
/// @details Warnings beyond the 16 visible slots increment a suppression count
/// and update the last slot with a cumulative summary.
void rt_asset_error_add_warning(const char *message);
/// @brief Append bounded formatted text to the warning list.
/// @param[in] fmt `printf`-style format string, or `NULL` for empty text.
/// @param[in] ... Values referenced by `fmt`.
void rt_asset_error_add_warningf(const char *fmt, ...) RT_ASSET_ERROR_PRINTF(1, 2);
/// @brief Read the number of visible warning slots.
/// @return A count from zero through 16.
int64_t rt_asset_error_get_warning_count(void);
/// @brief Borrow one visible warning.
/// @param[in] index Zero-based warning slot.
/// @return Thread-local text, or a static empty string for an invalid index.
const char *rt_asset_error_get_warning(int64_t index);

/// @brief Structured import-degradation counters recorded alongside load warnings.
/// @details Loaders bump these whenever content is dropped or approximated during
///          import so tools can inspect degradation without parsing warning text.
///          Reset with the warning list at the start of each top-level load.
typedef enum rt_asset_import_stat {
    /// Primitives skipped because their mode is unsupported (glTF POINTS/LINES/...).
    RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES = 0,
    /// Vertices whose >4 meaningful bone influences were folded to the top four.
    RT_ASSET_IMPORT_STAT_TRUNCATED_INFLUENCE_VERTICES,
    /// Vertices whose meaningful joints exceeded the runtime bone limit and were dropped.
    RT_ASSET_IMPORT_STAT_OUT_OF_RANGE_JOINT_VERTICES,
    /// Optional (extensionsUsed) extensions the loader does not interpret.
    RT_ASSET_IMPORT_STAT_IGNORED_EXTENSIONS,
    /// Skeletal CUBICSPLINE channels baked to sampled keys by an importer.
    RT_ASSET_IMPORT_STAT_BAKED_CUBIC_SPLINE_CHANNELS,
    /// Animation keys removed by importer-side compression.
    RT_ASSET_IMPORT_STAT_COMPRESSED_ANIMATION_KEYS_DROPPED,
    /// Sentinel equal to the number of statistic slots.
    RT_ASSET_IMPORT_STAT_COUNT
} rt_asset_import_stat;

/// @brief Add @p amount to one structured import counter (negative/invalid inputs ignored).
/// @param[in] stat Counter selector.
/// @param[in] amount Positive increment; overflow saturates at `INT64_MAX`.
void rt_asset_error_add_import_stat(rt_asset_import_stat stat, int64_t amount);
/// @brief Read one structured import counter for the current thread's last load.
/// @param[in] stat Counter selector.
/// @return The stored value, or zero for an invalid selector.
int64_t rt_asset_error_get_import_stat(rt_asset_import_stat stat);
/// @brief Return whether the current load-error message was truncated to fit storage.
/// @return One when truncated or formatting failed, otherwise zero.
int rt_asset_error_get_message_was_truncated(void);
/// @brief Return whether warning @p index was truncated to fit storage.
/// @param[in] index Zero-based visible warning slot.
/// @return One when truncated, otherwise zero; invalid indices return zero.
int rt_asset_error_get_warning_was_truncated(int64_t index);
/// @brief Return how many warnings were suppressed after the bounded warning list filled.
/// @return The current thread's cumulative suppressed-warning count.
int64_t rt_asset_error_get_warning_suppressed_count(void);

/// @brief Copy the last load-error text into a new runtime string.
/// @return A caller-owned runtime string, possibly empty.
rt_string rt_assets3d_get_last_load_error(void);
/// @brief Read the last load-error code through the runtime API.
/// @return The current code converted to `int64_t`.
int64_t rt_assets3d_get_last_load_error_code(void);
/// @brief Read the visible load-warning count through the runtime API.
/// @return The current thread's visible warning count.
int64_t rt_assets3d_get_load_warning_count(void);
/// @brief Copy one visible load warning into a runtime string.
/// @param[in] index Zero-based warning slot.
/// @return A caller-owned string, empty when the index is invalid.
rt_string rt_assets3d_get_load_warning(int64_t index);
/// @brief Join visible warnings with newlines in a new runtime string.
/// @return A caller-owned string, empty when no warnings are present.
rt_string rt_assets3d_get_load_warnings(void);
/// @brief JSON summary of the last load's degradation: the structured counters plus
///        the warning strings, e.g. `{"skippedPrimitives":1,...,"warnings":["..."]}`.
/// @return A caller-owned bounded JSON report string.
rt_string rt_assets3d_get_import_report(void);

#ifdef __cplusplus
}
#endif

#undef RT_ASSET_ERROR_PRINTF
