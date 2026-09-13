//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_provider.h
// Purpose: Internal contract between the provider-neutral Zanna.Services core
//          and the platform providers compiled into it (Steam today; other
//          stores later).
// Key invariants:
//   - A provider is a static, immutable rt_services_provider table. The core
//     calls its callbacks only on the main thread and only in this order:
//     start, then any number of pump/query/begin_request calls, then stop.
//   - No callback other than start is invoked while the provider is stopped.
//   - Query callbacks and feature operation tables may be NULL; the core then
//     reports neutral values. A table may also leave individual operations
//     NULL.
//   - The core validates provider-independent argument rules (empty
//     identifiers, constant values, ranges) before calling a provider, so
//     providers see non-NULL strings and in-range constants.
//   - Providers report back exclusively through the rt_services_provider_*
//     functions declared here, which are main-thread only.
// Ownership/Lifetime:
//   - Provider tables have static storage duration.
//   - String query results are new caller-owned references (or NULL for empty).
//   - Text and entries passed to the report functions are borrowed and copied
//     immediately.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/steam/rt_steam_provider.c,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_provider.h
 * @brief Declares the platform services provider interface and core callbacks.
 * @details Adding a store means implementing one @ref rt_services_provider
 *          table, filling the feature operation tables the store supports,
 *          listing the provider in the core's registry (rt_services.c), and
 *          adding optional Zanna.Services.<Provider> extension classes. The
 *          neutral Zanna.Services classes do not change.
 */

#pragma once

#include "rt_platform.h"
#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Capacity, in bytes including the terminator, of a provider message buffer.
#define RT_SERVICES_MESSAGE_CAPACITY 2048
/// @brief Capacity, in bytes including the terminator, of a leaderboard entry user id.
#define RT_SERVICES_USER_ID_CAPACITY 32
/// @brief Capacity, in bytes including the terminator, of a leaderboard entry user name.
#define RT_SERVICES_USER_NAME_CAPACITY 256

/// @brief Achievement attribute selector for rt_services_achievement_ops.attribute.
typedef enum rt_services_achievement_attribute {
    RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_NAME = 1,        ///< Localized display name.
    RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_DESCRIPTION = 2, ///< Localized description.
    RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_HIDDEN = 3,      ///< "1" when hidden, "0" otherwise.
} rt_services_achievement_attribute;

/// @brief Arguments of one asynchronous request, selected by @ref kind.
/// @details Fields not used by @ref kind are zero or the empty string. Strings
///          are borrowed for the duration of begin_request and never NULL.
typedef struct rt_services_request_args {
    int64_t kind;        ///< RT_SERVICES_REQUEST_* value.
    const char *name;    ///< Leaderboard name (leaderboard kinds) or prompt (TextInput).
    const char *text;    ///< Initial text (TextInput).
    int8_t create;       ///< LeaderboardFind: create the board when it is missing.
    int64_t sort;        ///< LeaderboardFind with create: RT_SERVICES_LEADERBOARD_SORT_*.
    int64_t display;     ///< LeaderboardFind with create: RT_SERVICES_LEADERBOARD_DISPLAY_*.
    int64_t score;       ///< LeaderboardUpload: score to upload.
    int8_t keep_best;    ///< LeaderboardUpload: keep a better existing score.
    int64_t scope;       ///< LeaderboardDownload: RT_SERVICES_LEADERBOARD_SCOPE_*.
    int64_t range_start; ///< LeaderboardDownload: first rank or offset.
    int64_t range_end;   ///< LeaderboardDownload: last rank or offset.
    int64_t max_length;  ///< TextInput: maximum length in bytes.
    int64_t text_mode;   ///< TextInput: RT_SERVICES_TEXT_INPUT_MODE_*.
} rt_services_request_args;

/// @brief One downloaded leaderboard entry reported by a provider.
typedef struct rt_services_leaderboard_entry {
    int64_t rank;                                   ///< Global rank, starting at 1.
    int64_t score;                                  ///< Stored score.
    char user_id[RT_SERVICES_USER_ID_CAPACITY];     ///< Provider-defined user id text.
    char user_name[RT_SERVICES_USER_NAME_CAPACITY]; ///< Display name known at completion, or "".
} rt_services_leaderboard_entry;

/// @brief Completion record passed to @ref rt_services_provider_finish_request.
typedef struct rt_services_request_result {
    int8_t succeeded;                             ///< Nonzero when the request succeeded.
    int64_t result_code;                          ///< Provider-defined result code.
    int64_t value;                                ///< Kind-specific integer result.
    int8_t flag;                                  ///< Kind-specific boolean result.
    const char *text;                             ///< Kind-specific text, or NULL.
    const char *error;                            ///< Failure message, or NULL for success.
    const rt_services_leaderboard_entry *entries; ///< Leaderboard entries, or NULL.
    int64_t entry_count;                          ///< Number of @ref entries.
} rt_services_request_result;

/// @brief Zanna.Services.Achievements operations. Every string is a non-empty achievement id.
typedef struct rt_services_achievement_ops {
    /// @brief Unlock locally. @return 1 when accepted.
    int8_t (*unlock)(const char *id);
    /// @brief Lock again. @return 1 when accepted.
    int8_t (*clear)(const char *id);
    /// @brief Read unlock state. @return 1 when the achievement exists and was read.
    int8_t (*get)(const char *id, int8_t *out_unlocked, int64_t *out_unlock_time);
    /// @brief Show a progress notification (0 <= current <= maximum, maximum > 0). @return 1 when
    /// shown.
    int8_t (*indicate_progress)(const char *id, int64_t current, int64_t maximum);
    /// @brief Count defined achievements. @return Count.
    int64_t (*count)(void);
    /// @brief Read the id at a non-negative index. @return Owned id, or NULL when out of range.
    rt_string (*id_at)(int64_t index);
    /// @brief Read a display attribute. @return Owned text, or NULL when unavailable.
    rt_string (*attribute)(const char *id, rt_services_achievement_attribute attribute);
} rt_services_achievement_ops;

/// @brief Zanna.Services.Stats operations. Every string is a non-empty stat name.
typedef struct rt_services_stat_ops {
    /// @brief Read an integer stat. @return 1 when read.
    int8_t (*get_int)(const char *name, int64_t *out_value);
    /// @brief Set an integer stat. @return 1 when accepted.
    int8_t (*set_int)(const char *name, int64_t value);
    /// @brief Read a floating-point stat. @return 1 when read.
    int8_t (*get_float)(const char *name, double *out_value);
    /// @brief Set a finite floating-point stat. @return 1 when accepted.
    int8_t (*set_float)(const char *name, double value);
    /// @brief Update an average-rate stat (finite count, positive finite seconds). @return 1 when
    /// accepted.
    int8_t (*update_average_rate)(const char *name, double count, double seconds);
    /// @brief Commit stats and achievements. @return 1 when the commit started.
    int8_t (*store)(void);
    /// @brief Reset stats and optionally achievements. @return 1 when accepted.
    int8_t (*reset_all)(int8_t include_achievements);
} rt_services_stat_ops;

/// @brief Zanna.Services.Leaderboards operations beyond begin_request.
typedef struct rt_services_leaderboard_ops {
    /// @brief Look up a user's display name live, for Request.EntryUserName.
    /// @param user_id Provider-defined user id from a leaderboard entry.
    /// @return Owned name, or NULL while the name is unknown.
    rt_string (*user_name)(const char *user_id);
} rt_services_leaderboard_ops;

/// @brief Zanna.Services.Presence operations.
typedef struct rt_services_presence_ops {
    /// @brief Set one key (non-empty) to a value (empty removes it). @return 1 when accepted.
    int8_t (*set)(const char *key, const char *value);
    /// @brief Remove every key.
    void (*clear)(void);
} rt_services_presence_ops;

/// @brief Zanna.Services.Overlay operations. Constants arrive already validated.
typedef struct rt_services_overlay_ops {
    /// @brief Report whether the overlay is usable now. @return 1 when enabled.
    int8_t (*is_enabled)(void);
    /// @brief Open an RT_SERVICES_OVERLAY_PAGE_* page. @return 1 when passed to the platform.
    int8_t (*open_page)(int64_t page);
    /// @brief Open a non-empty URL. @return 1 when passed to the platform.
    int8_t (*open_web_page)(const char *url, int8_t modal);
    /// @brief Open a store page; must trap on a malformed non-empty id. @return 1 when passed on.
    int8_t (*open_store)(const char *product_id, int8_t add_to_cart);
    /// @brief Set the RT_SERVICES_NOTIFICATION_POSITION_* corner. @return 1 when passed on.
    int8_t (*set_notification_position)(int64_t position);
    /// @brief Set the notification inset in pixels. @return 1 when passed on.
    int8_t (*set_notification_inset)(int64_t horizontal, int64_t vertical);
} rt_services_overlay_ops;

/// @brief Zanna.Services.OnScreenKeyboard operations beyond begin_request.
typedef struct rt_services_text_input_ops {
    /// @brief Show the floating keyboard (validated mode, non-negative size). @return 1 when shown.
    int8_t (*show_floating)(int64_t mode, int64_t x, int64_t y, int64_t width, int64_t height);
    /// @brief Close the floating keyboard. @return 1 when accepted.
    int8_t (*dismiss_floating)(void);
} rt_services_text_input_ops;

/// @brief Zanna.Services.Cloud operations. Every name is a non-empty file name.
typedef struct rt_services_cloud_ops {
    /// @brief Report whether cloud storage is enabled for the account and app. @return 1 when
    /// enabled.
    int8_t (*is_enabled)(void);
    /// @brief Write a file from borrowed bytes (@p data may be NULL when @p size is 0). @return 1
    /// when written.
    int8_t (*write)(const char *name, const uint8_t *data, int64_t size);
    /// @brief Read a file into a new Bytes object.
    /// @param name File name.
    /// @param out_bytes Receives a caller-owned Zanna.Collections.Bytes on success.
    /// @param message Receives the failure message.
    /// @param message_capacity Size of @p message in bytes.
    /// @return 1 on success, otherwise 0.
    int8_t (*read)(const char *name, void **out_bytes, char *message, size_t message_capacity);
    /// @brief Report whether a file exists. @return 1 when it exists.
    int8_t (*exists)(const char *name);
    /// @brief Delete a file. @return 1 when deleted.
    int8_t (*remove)(const char *name);
    /// @brief Read a file size. @return Bytes, or 0 when missing.
    int64_t (*size)(const char *name);
    /// @brief Read a file's write time. @return Unix seconds, or 0 when unknown.
    int64_t (*timestamp)(const char *name);
    /// @brief Count files. @return File count.
    int64_t (*file_count)(void);
    /// @brief Read the file name at a non-negative index. @return Owned name, or NULL when out of
    /// range.
    rt_string (*file_name_at)(int64_t index);
    /// @brief Read the quota. @return 1 when both outputs were written.
    int8_t (*quota)(int64_t *out_total, int64_t *out_available);
    /// @brief Begin a write batch. @return 1 when started.
    int8_t (*begin_batch)(void);
    /// @brief End the write batch. @return 1 when ended.
    int8_t (*end_batch)(void);
} rt_services_cloud_ops;

/// @brief Static callback table describing one platform services provider.
/// @details All callbacks run on the main thread. @ref start, @ref pump and
///          @ref stop are required; every other callback and feature table may
///          be NULL, in which case the core reports the neutral value for it.
typedef struct rt_services_provider {
    /// @brief Canonical lowercase provider id accepted by Platform.Init (for example "steam").
    const char *id;
    /// @brief Human-readable provider name used in documentation and diagnostics.
    const char *display_name;

    /// @brief Start the provider for a provider-defined application id.
    /// @details Must validate @p app_id first (trapping on a malformed id) and
    ///          leave no provider state behind on failure. On failure the
    ///          provider writes one diagnostic sentence into @p message.
    /// @param app_id Borrowed, non-NULL application id.
    /// @param message Buffer receiving the failure message; untouched on success.
    /// @param message_capacity Size of @p message in bytes.
    /// @return RT_SERVICES_STATUS_OK or a failure RT_SERVICES_STATUS_* value.
    int64_t (*start)(rt_string app_id, char *message, size_t message_capacity);

    /// @brief Deliver pending platform callbacks as events and request completions.
    void (*pump)(void);

    /// @brief Stop the provider. Pending requests are cancelled by the core afterwards.
    void (*stop)(void);

    /// @brief Report whether a started provider supports a feature now.
    /// @param feature RT_SERVICES_FEATURE_* value.
    /// @return 1 when supported, otherwise 0.
    int8_t (*has_feature)(int64_t feature);

    /// @brief Read the provider's application id. @return Owned string or NULL.
    rt_string (*app_id)(void);
    /// @brief Read the signed-in user's id. @return Owned string or NULL.
    rt_string (*user_id)(void);
    /// @brief Read the signed-in user's display name. @return Owned string or NULL.
    rt_string (*user_name)(void);
    /// @brief Read the user's game language code. @return Owned string or NULL.
    rt_string (*language)(void);
    /// @brief Report whether the running application is licensed. @return 1 or 0.
    int8_t (*is_licensed)(void);
    /// @brief Report whether the client is connected online. @return 1 or 0.
    int8_t (*is_online)(void);

    /// @brief Report whether a DLC is owned and installed.
    /// @details Must trap on a malformed provider-defined id.
    /// @param dlc_id Borrowed, non-NULL DLC id.
    /// @return 1 when installed, otherwise 0.
    int8_t (*is_dlc_installed)(rt_string dlc_id);

    /// @brief Begin an asynchronous request.
    /// @details On success stores a nonzero provider handle that the provider
    ///          later passes to @ref rt_services_provider_finish_request. The
    ///          handle is the provider's own token: it must stay stable for the
    ///          life of the request even when the request spans several
    ///          platform calls. On failure writes one sentence into @p message.
    /// @param args Request kind and arguments, already validated by the core.
    /// @param out_handle Receives the nonzero provider handle on success.
    /// @param message Buffer receiving the failure message.
    /// @param message_capacity Size of @p message in bytes.
    /// @return 1 when the request started, otherwise 0.
    int8_t (*begin_request)(const rt_services_request_args *args,
                            uint64_t *out_handle,
                            char *message,
                            size_t message_capacity);

    /// @brief Achievements operations, or NULL.
    const rt_services_achievement_ops *achievements;
    /// @brief Stats operations, or NULL.
    const rt_services_stat_ops *stats;
    /// @brief Leaderboard operations beyond begin_request, or NULL.
    const rt_services_leaderboard_ops *leaderboards;
    /// @brief Rich presence operations, or NULL.
    const rt_services_presence_ops *presence;
    /// @brief Overlay operations, or NULL.
    const rt_services_overlay_ops *overlay;
    /// @brief Text input operations beyond begin_request, or NULL.
    const rt_services_text_input_ops *text_input;
    /// @brief Cloud storage operations, or NULL.
    const rt_services_cloud_ops *cloud;
} rt_services_provider;

//===----------------------------------------------------------------------===//
// Built-in providers
//===----------------------------------------------------------------------===//

/// @brief Steamworks provider backed by the runtime-loaded steam_api redistributable.
extern const rt_services_provider rt_services_steam_provider;

//===----------------------------------------------------------------------===//
// Core functions available to providers (main thread only)
//===----------------------------------------------------------------------===//

/// @brief Trap unless the caller runs on the main thread.
/// @details Provider extension classes (for example Zanna.Services.Steam) use
///          this so every services entry point reports the same diagnostic:
///          "Services: <member> must be called on the main thread".
/// @param member Class-qualified member name, for example "Steam.SteamId".
/// @return 1 on the main thread, 0 after reporting the trap.
int rt_services_provider_require_main_thread(const char *member);

/// @brief Queue a platform event for Platform.PollEvent.
/// @details When the queue is full the oldest event is discarded and the
///          dropped-event counter increments.
/// @param kind RT_SERVICES_EVENT_* value; RT_SERVICES_EVENT_NONE is ignored.
/// @param result_code Provider-defined result code, or 0.
/// @param value Event-specific integer payload.
/// @param flag Event-specific boolean payload (nonzero means true).
/// @param text Borrowed event text, truncated to the event text capacity; NULL is empty.
void rt_services_provider_emit_event(
    int64_t kind, int64_t result_code, int64_t value, int8_t flag, const char *text);

/// @brief Queue a progress event that reports a value out of a total.
/// @details Identical to @ref rt_services_provider_emit_event, plus the total
///          read through Platform.EventTotal.
/// @param kind RT_SERVICES_EVENT_* value; RT_SERVICES_EVENT_NONE is ignored.
/// @param result_code Provider-defined result code, or 0.
/// @param value Progress so far.
/// @param total Progress maximum.
/// @param flag Event-specific boolean payload.
/// @param text Borrowed event text; NULL is empty.
void rt_services_provider_emit_progress_event(
    int64_t kind, int64_t result_code, int64_t value, int64_t total, int8_t flag, const char *text);

/// @brief Record a non-fatal diagnostic readable through Platform.Diagnostics.
/// @details Formats with printf semantics and truncates to the diagnostic
///          capacity. The oldest message is discarded when the ring is full,
///          and a message identical to the newest retained one is not repeated.
/// @param format printf-style format string.
void rt_services_provider_add_diagnostic(const char *format, ...) RT_PRINTF_FORMAT(1, 2);

/// @brief Look up the kind of a pending request by provider handle.
/// @param handle Provider handle from begin_request.
/// @return RT_SERVICES_REQUEST_* value, or 0 when no request is pending for @p handle.
int64_t rt_services_provider_pending_request_kind(uint64_t handle);

/// @brief Complete a pending request with a full result and release the core's reference.
/// @details Unknown handles are ignored, which makes late or duplicate
///          completions harmless. Entries beyond
///          RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY are discarded.
/// @param handle Provider handle from begin_request.
/// @param result Borrowed completion record; its strings and entries are copied.
void rt_services_provider_finish_request(uint64_t handle, const rt_services_request_result *result);

/// @brief Complete a pending request with only a status, value, and error.
/// @details Convenience wrapper around @ref rt_services_provider_finish_request.
/// @param handle Provider handle from begin_request.
/// @param succeeded Nonzero when the request succeeded.
/// @param result_code Provider-defined result code.
/// @param value Kind-specific integer result.
/// @param error Borrowed error text for failures; NULL or empty for success.
void rt_services_provider_complete_request(
    uint64_t handle, int8_t succeeded, int64_t result_code, int64_t value, const char *error);

#ifdef __cplusplus
}
#endif
