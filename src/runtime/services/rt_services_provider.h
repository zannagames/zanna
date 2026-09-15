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
    int64_t max_length;  ///< TextInput: maximum number of characters the player may enter.
    int64_t text_mode;   ///< TextInput: RT_SERVICES_TEXT_INPUT_MODE_*.
    const char *item_id; ///< Workshop kinds: the item id (Subscribe, Unsubscribe, Delete) or the
                         ///< update id (Submit); "" otherwise.
    const char *items;   ///< WorkshopQuery by ids: comma-separated item ids; "" otherwise.
    const char *tags;    ///< WorkshopQuery by order: required tags, comma-separated, possibly "".
    const char *search;  ///< WorkshopQuery by order: search text, possibly "".
    int64_t query;       ///< WorkshopQuery: RT_SERVICES_WORKSHOP_QUERY_* order, or 0.
    int64_t list;        ///< WorkshopQuery: RT_SERVICES_WORKSHOP_LIST_* list, or 0.
    int64_t page;        ///< WorkshopQuery by order or list: page from 1.
} rt_services_request_args;

/// @brief One downloaded leaderboard entry reported by a provider.
typedef struct rt_services_leaderboard_entry {
    int64_t rank;                                   ///< Global rank, starting at 1.
    int64_t score;                                  ///< Stored score.
    char user_id[RT_SERVICES_USER_ID_CAPACITY];     ///< Provider-defined user id text.
    char user_name[RT_SERVICES_USER_NAME_CAPACITY]; ///< Display name known at completion, or "".
} rt_services_leaderboard_entry;

/// @brief One Workshop item reported by a provider when a query completes.
/// @details Strings are borrowed until rt_services_provider_finish_request
///          returns and are never NULL.
typedef struct rt_services_workshop_item {
    const char *id;          ///< Provider-defined item id.
    const char *title;       ///< Title.
    const char *description; ///< Description.
    const char *owner_id;    ///< Provider-defined user id of the author.
    const char *tags;        ///< Comma-separated tags.
    const char *preview_url; ///< URL of the preview image, possibly empty.
    const char *metadata;    ///< Developer metadata, possibly empty.
    int64_t created;         ///< Creation time in Unix seconds.
    int64_t updated;         ///< Last update time in Unix seconds.
    int64_t visibility;      ///< RT_SERVICES_WORKSHOP_VISIBILITY_* value.
    int64_t votes_up;        ///< Up votes.
    int64_t votes_down;      ///< Down votes.
    int64_t size;            ///< Content size in bytes.
    double score;            ///< Vote score in 0..1.
} rt_services_workshop_item;

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
    const int64_t *details;                       ///< Kind-specific integer details, or NULL.
    int64_t detail_count; ///< Number of @ref details (at most RT_SERVICES_REQUEST_DETAIL_CAPACITY).
    const rt_services_workshop_item *items; ///< Workshop items (WorkshopQuery), or NULL.
    int64_t item_count; ///< Number of @ref items (at most RT_SERVICES_REQUEST_ITEM_CAPACITY).
} rt_services_request_result;

/// @brief One timeline event description passed to rt_services_timeline_ops.
/// @details Strings are borrowed for the duration of the call and never NULL;
///          @ref title is non-empty. Constants and priorities arrive validated.
typedef struct rt_services_timeline_event {
    const char *title;       ///< Event title.
    const char *description; ///< Event description, possibly empty.
    const char *icon;        ///< Provider icon name, possibly empty.
    int64_t priority;      ///< 0..RT_SERVICES_TIMELINE_MAX_PRIORITY, or KEEP_PRIORITY for updates.
    double offset_seconds; ///< Finite start offset relative to now (negative is the past).
    double duration_seconds; ///< Finite, non-negative duration for range events; 0 otherwise.
    int64_t clip;            ///< RT_SERVICES_TIMELINE_CLIP_* value.
} rt_services_timeline_event;

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
    /// @brief Read the icon for an achievement's current state.
    /// @details When the platform is still loading the icon the operation
    ///          returns 0 and later emits RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY.
    /// @param id Achievement id.
    /// @param out_width Receives the width in pixels.
    /// @param out_height Receives the height in pixels.
    /// @param out_rgba Receives caller-owned Zanna.Collections.Bytes holding
    ///        width*height*4 RGBA bytes, or is NULL to read only the size.
    /// @return 1 when the icon is loaded and was read, otherwise 0.
    int8_t (*icon)(const char *id, int64_t *out_width, int64_t *out_height, void **out_rgba);
    /// @brief Read the share of players who unlocked an achievement.
    /// @param id Achievement id.
    /// @param out_percent Receives 0..100.
    /// @return 1 when known (after an AchievementPercentages request succeeded), otherwise 0.
    int8_t (*global_percent)(const char *id, double *out_percent);
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

/// @brief Zanna.Services.Timeline operations (game recording timeline markers and phases).
/// @details Event ids are provider-defined text; a provider traps on a
///          malformed id while it is active. Operations that pass data to the
///          platform return 1 when the call reached it.
typedef struct rt_services_timeline_ops {
    /// @brief Set the RT_SERVICES_TIMELINE_MODE_* mode. @return 1 when passed on.
    int8_t (*set_game_mode)(int64_t mode);
    /// @brief Describe the current game state. @return 1 when passed on.
    int8_t (*set_tooltip)(const char *text, double offset_seconds);
    /// @brief Remove the current state description. @return 1 when passed on.
    int8_t (*clear_tooltip)(double offset_seconds);
    /// @brief Add an instantaneous event, or a range event when @p range is nonzero.
    /// @param event Validated event description.
    /// @param range Nonzero to add a closed range of event->duration_seconds.
    /// @param out_id Receives the event id on success.
    /// @param id_capacity Size of @p out_id in bytes.
    /// @return 1 when the event was added.
    int8_t (*add_event)(const rt_services_timeline_event *event,
                        int8_t range,
                        char *out_id,
                        size_t id_capacity);
    /// @brief Start an open range event. @return 1 and the id in @p out_id when started.
    int8_t (*start_range_event)(const rt_services_timeline_event *event,
                                char *out_id,
                                size_t id_capacity);
    /// @brief Update an open range event. @return 1 when passed on.
    int8_t (*update_range_event)(const char *event_id, const rt_services_timeline_event *event);
    /// @brief End an open range event. @return 1 when passed on.
    int8_t (*end_range_event)(const char *event_id, double offset_seconds);
    /// @brief Remove an event. @return 1 when passed on.
    int8_t (*remove_event)(const char *event_id);
    /// @brief Start a game phase. @return 1 when passed on.
    int8_t (*start_phase)(void);
    /// @brief End the current game phase. @return 1 when passed on.
    int8_t (*end_phase)(void);
    /// @brief Give the current phase a persistent id. @return 1 when passed on.
    int8_t (*set_phase_id)(const char *phase_id);
    /// @brief Tag the current phase. @return 1 when passed on.
    int8_t (*add_phase_tag)(const char *name,
                            const char *icon,
                            const char *group,
                            int64_t priority);
    /// @brief Set a text attribute of the current phase. @return 1 when passed on.
    int8_t (*set_phase_attribute)(const char *group, const char *value, int64_t priority);
    /// @brief Open the overlay at a phase. @return 1 when passed on.
    int8_t (*open_overlay_to_phase)(const char *phase_id);
    /// @brief Open the overlay at an event. @return 1 when passed on.
    int8_t (*open_overlay_to_event)(const char *event_id);
} rt_services_timeline_ops;

/// @brief Zanna.Services.ActionInput operations (platform action-based controller input).
/// @details Controller ids are provider-defined text. The core passes a
///          non-empty controller id to an operation only after
///          @ref check_controller_id accepted it. An empty id reaches only the
///          activation operations, where it means every controller, including
///          controllers connected later. Action, action set, and layer names
///          are non-empty; constants and ranges arrive validated.
typedef struct rt_services_action_input_ops {
    /// @brief Start action input.
    /// @details A platform that refuses the manifest still starts action input
    ///          and may apply the manifest later; the operation then returns 0
    ///          while is_started reports 1. Starting again replaces the manifest.
    /// @param manifest_path Absolute path of the action manifest, or "" to use the
    ///        configuration published with the platform.
    /// @return 1 when started with the manifest accepted, otherwise 0.
    int8_t (*start)(const char *manifest_path);
    /// @brief Stop action input. @return 1 when it was started.
    int8_t (*stop)(void);
    /// @brief Report whether action input is started. @return 1 when started.
    int8_t (*is_started)(void);
    /// @brief Trap, naming @p member, unless @p controller_id is well formed.
    /// @return 1 when well formed, 0 after reporting the trap.
    int8_t (*check_controller_id)(const char *member, const char *controller_id);
    /// @brief Count controllers connected as of the last pump. @return Count.
    int64_t (*controller_count)(void);
    /// @brief Write the id of the connected controller at a non-negative index.
    /// @return 1 when @p index names a controller.
    int8_t (*controller_id_at)(int64_t index, char *out_id, size_t id_capacity);
    /// @brief Read a controller's RT_SERVICES_CONTROLLER_TYPE_* value. @return Type.
    int64_t (*controller_type)(const char *controller_id);
    /// @brief Read the gamepad slot a controller emulates. @return Slot, or -1.
    int64_t (*gamepad_index)(const char *controller_id);
    /// @brief Activate an action set ("" for every controller). @return 1 when passed on.
    int8_t (*activate_action_set)(const char *controller_id, const char *action_set);
    /// @brief Activate or deactivate a layer ("" for every controller). @return 1 when passed on.
    int8_t (*set_layer_active)(const char *controller_id, const char *layer, int8_t active);
    /// @brief Deactivate every layer ("" for every controller). @return 1 when passed on.
    int8_t (*deactivate_all_layers)(const char *controller_id);
    /// @brief Read a digital action.
    /// @param out_pressed Receives 1 while the action is pressed.
    /// @param out_active Receives 1 when the action is available in the active set.
    /// @return 1 when the action is defined.
    int8_t (*digital_action)(const char *controller_id,
                             const char *action,
                             int8_t *out_pressed,
                             int8_t *out_active);
    /// @brief Read an analog action.
    /// @param out_x Receives the horizontal value.
    /// @param out_y Receives the vertical value.
    /// @param out_active Receives 1 when the action is available in the active set.
    /// @return 1 when the action is defined.
    int8_t (*analog_action)(const char *controller_id,
                            const char *action,
                            double *out_x,
                            double *out_y,
                            int8_t *out_active);
    /// @brief Read an action's localized name. @return Owned text, or NULL when unknown.
    rt_string (*action_label)(const char *action);
    /// @brief Read the physical inputs bound to an action.
    /// @param action_set Action set name, or "" for the controller's current set.
    /// @param out_origins Receives provider-defined origin ids.
    /// @param capacity Entries available in @p out_origins.
    /// @return Number of origins written.
    int64_t (*action_origins)(const char *controller_id,
                              const char *action_set,
                              const char *action,
                              int64_t *out_origins,
                              int64_t capacity);
    /// @brief Read a non-negative origin's localized name. @return Owned text, or NULL.
    rt_string (*origin_label)(int64_t origin);
    /// @brief Read the image file of an origin's glyph at an RT_SERVICES_GLYPH_SIZE_* size.
    /// @return Owned path, or NULL.
    rt_string (*origin_glyph_path)(int64_t origin, int64_t size);
    /// @brief Run a controller's motors at strengths in 0..1. @return 1 when passed on.
    int8_t (*vibrate)(const char *controller_id, double left, double right);
    /// @brief Set a controller's light (components 0..255), or restore the user's color.
    /// @return 1 when passed on.
    int8_t (*set_led_color)(
        const char *controller_id, int64_t red, int64_t green, int64_t blue, int8_t restore);
    /// @brief Open the platform's binding panel for a controller. @return 1 when opened.
    int8_t (*show_binding_panel)(const char *controller_id);
} rt_services_action_input_ops;

/// @brief Text field of a pending Workshop item update.
typedef enum rt_services_workshop_field {
    RT_SERVICES_WORKSHOP_FIELD_TITLE = 1,       ///< Workshop.SetTitle.
    RT_SERVICES_WORKSHOP_FIELD_DESCRIPTION = 2, ///< Workshop.SetDescription.
    RT_SERVICES_WORKSHOP_FIELD_METADATA = 3,    ///< Workshop.SetMetadata.
    RT_SERVICES_WORKSHOP_FIELD_TAGS = 4,        ///< Workshop.SetTags (comma-separated).
    RT_SERVICES_WORKSHOP_FIELD_CONTENT = 5,     ///< Workshop.SetContent (absolute folder).
    RT_SERVICES_WORKSHOP_FIELD_PREVIEW = 6,     ///< Workshop.SetPreview (absolute file).
} rt_services_workshop_field;

/// @brief Workshop item state flag: the player is subscribed.
#define RT_SERVICES_WORKSHOP_STATE_SUBSCRIBED INT64_C(1)
/// @brief Workshop item state flag: the item is installed (possibly out of date).
#define RT_SERVICES_WORKSHOP_STATE_INSTALLED INT64_C(4)
/// @brief Workshop item state flag: the item needs an update or is not installed yet.
#define RT_SERVICES_WORKSHOP_STATE_NEEDS_UPDATE INT64_C(8)
/// @brief Workshop item state flag: the item is downloading.
#define RT_SERVICES_WORKSHOP_STATE_DOWNLOADING INT64_C(16)
/// @brief Workshop item state flag: a download was requested and has not started.
#define RT_SERVICES_WORKSHOP_STATE_DOWNLOAD_PENDING INT64_C(32)

/// @brief Zanna.Services.Workshop operations beyond begin_request.
/// @details Item and update ids are provider-defined text. The core passes a
///          non-empty id only after the matching check operation accepted it.
///          Paths arrive absolute and constants validated.
typedef struct rt_services_workshop_ops {
    /// @brief Trap, naming @p member, unless @p item_id is well formed. @return 1 when well formed.
    int8_t (*check_item_id)(const char *member, const char *item_id);
    /// @brief Trap, naming @p member, unless @p update_id is well formed. @return 1 when well
    /// formed.
    int8_t (*check_update_id)(const char *member, const char *update_id);
    /// @brief Count the player's subscribed items. @return Count.
    int64_t (*subscribed_count)(void);
    /// @brief Write the id of the subscribed item at a non-negative index.
    /// @return 1 when @p index names an item.
    int8_t (*subscribed_id_at)(int64_t index, char *out_id, size_t id_capacity);
    /// @brief Read an item's RT_SERVICES_WORKSHOP_STATE_* flags. @return Flags, or 0.
    int64_t (*item_state)(const char *item_id);
    /// @brief Read where an installed item lives.
    /// @param out_folder Receives the absolute install folder.
    /// @param folder_capacity Size of @p out_folder in bytes.
    /// @param out_size Receives the size on disk in bytes.
    /// @param out_time Receives the install time in Unix seconds.
    /// @return 1 when the item is installed.
    int8_t (*install_info)(const char *item_id,
                           char *out_folder,
                           size_t folder_capacity,
                           int64_t *out_size,
                           int64_t *out_time);
    /// @brief Read download progress. @return 1 when the platform reports progress.
    int8_t (*download_info)(const char *item_id, int64_t *out_downloaded, int64_t *out_total);
    /// @brief Download or update an item. @return 1 when the download was queued.
    int8_t (*download)(const char *item_id, int8_t high_priority);
    /// @brief Begin an update of an item the player owns.
    /// @return 1 and the update id in @p out_update_id when started.
    int8_t (*start_update)(const char *item_id, char *out_update_id, size_t id_capacity);
    /// @brief Set a text field of a pending update. @return 1 when accepted.
    int8_t (*set_update_text)(const char *update_id,
                              rt_services_workshop_field field,
                              const char *text);
    /// @brief Set the visibility of a pending update. @return 1 when accepted.
    int8_t (*set_update_visibility)(const char *update_id, int64_t visibility);
    /// @brief Read the progress of a submitted update.
    /// @param out_status Receives an RT_SERVICES_WORKSHOP_UPDATE_STATUS_* value.
    /// @param out_processed Receives the bytes processed.
    /// @param out_total Receives the bytes to process.
    /// @return 1 when written.
    int8_t (*update_progress)(const char *update_id,
                              int64_t *out_status,
                              int64_t *out_processed,
                              int64_t *out_total);
} rt_services_workshop_ops;

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

    /// @brief Read the command line the platform passed through a launch URL.
    /// @return Owned string, or NULL when there is none.
    rt_string (*launch_command_line)(void);
    /// @brief Read one launch parameter the platform passed through a launch URL.
    /// @param key Borrowed, non-empty parameter name.
    /// @return Owned value, or NULL when the parameter is absent.
    rt_string (*launch_parameter)(const char *key);

    /// @brief Count the DLC the application defines. @return Count, or 0.
    int64_t (*dlc_count)(void);
    /// @brief Read one DLC of the application.
    /// @param index Non-negative index.
    /// @param out_id Receives the provider-defined DLC id.
    /// @param id_capacity Size of @p out_id in bytes.
    /// @param out_name Receives the DLC display name.
    /// @param name_capacity Size of @p out_name in bytes.
    /// @param out_available Receives 1 when the DLC can be bought now.
    /// @return 1 when @p index names a DLC, otherwise 0.
    int8_t (*dlc_at)(int64_t index,
                     char *out_id,
                     size_t id_capacity,
                     char *out_name,
                     size_t name_capacity,
                     int8_t *out_available);
    /// @brief Read the installed build's id. @return Build id, or 0 when unknown.
    int64_t (*build_id)(void);
    /// @brief Read the branch the installed build comes from.
    /// @return Owned branch name, or NULL on the default branch.
    rt_string (*branch_name)(void);

    /// @brief Begin an asynchronous request.
    /// @details On success stores a nonzero provider handle that the provider
    ///          later passes to @ref rt_services_provider_finish_request. The
    ///          handle is the provider's own token: it must stay stable for the
    ///          life of the request even when the request spans several
    ///          platform calls. On failure writes one sentence into @p message.
    ///          The core registers the request only after this callback
    ///          returns, so a provider must never complete the request it is
    ///          starting from inside this callback; to fail synchronously it
    ///          returns 0 instead.
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
    /// @brief Recording timeline operations, or NULL.
    const rt_services_timeline_ops *timeline;
    /// @brief Action input operations, or NULL.
    const rt_services_action_input_ops *action_input;
    /// @brief Workshop operations beyond begin_request, or NULL.
    const rt_services_workshop_ops *workshop;
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
