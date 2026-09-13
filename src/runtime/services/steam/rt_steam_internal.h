//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_internal.h
// Purpose: State and helpers shared by the Steam provider's translation units:
//          the lifecycle and dispatch core (rt_steam_provider.c) and the
//          feature bindings for ISteamUserStats (rt_steam_user_stats.c),
//          ISteamFriends/ISteamUtils UI features (rt_steam_social.c), and
//          ISteamRemoteStorage (rt_steam_cloud.c).
// Key invariants:
//   - All state is main-thread only and valid between a successful start and
//     stop; stop zeroes every binding, pending operation, and cache.
//   - A feature group's function pointers are either all bound or all NULL;
//     its `ready` flag records which.
//   - Every request the provider starts owns one slot in the operation table,
//     keyed by a provider token that never changes while the request spans
//     several Steam calls.
// Ownership/Lifetime:
//   - Interface pointers are borrowed from the Steam client.
//   - The state object has static storage duration.
// Links: src/runtime/services/steam/rt_steam_provider.c,
//        src/runtime/services/steam/rt_steam_abi.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_internal.h
 * @brief Declares the Steam provider's shared state and cross-file helpers.
 */

#pragma once

#include "rt_services.h"
#include "rt_services_provider.h"
#include "rt_steam_abi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Capacity, including the terminator, of a resolved library path.
#define STEAM_PATH_CAPACITY 4096
/// @brief Number of leaderboard name-to-handle mappings kept per session.
#define STEAM_LEADERBOARD_CACHE_CAPACITY 32

/// @brief Core flat-API exports, resolved all-or-nothing.
typedef struct steam_core_api {
    rt_steam_init_flat_fn init_flat;              ///< SteamAPI_InitFlat.
    rt_steam_void_fn shutdown;                    ///< SteamAPI_Shutdown.
    rt_steam_restart_app_fn restart_app;          ///< SteamAPI_RestartAppIfNecessary.
    rt_steam_bool_fn is_steam_running;            ///< SteamAPI_IsSteamRunning.
    rt_steam_get_pipe_fn get_pipe;                ///< SteamAPI_GetHSteamPipe.
    rt_steam_void_fn dispatch_init;               ///< SteamAPI_ManualDispatch_Init.
    rt_steam_pipe_fn dispatch_run_frame;          ///< SteamAPI_ManualDispatch_RunFrame.
    rt_steam_next_callback_fn dispatch_next;      ///< SteamAPI_ManualDispatch_GetNextCallback.
    rt_steam_pipe_fn dispatch_free;               ///< SteamAPI_ManualDispatch_FreeLastCallback.
    rt_steam_call_result_fn dispatch_call_result; ///< SteamAPI_ManualDispatch_GetAPICallResult.
} steam_core_api;

/// @brief ISteamUser binding.
typedef struct steam_user_api {
    void *self;                        ///< Interface pointer, or NULL when unavailable.
    rt_steam_self_bool_fn logged_on;   ///< BLoggedOn.
    rt_steam_self_u64_fn get_steam_id; ///< GetSteamID.
} steam_user_api;

/// @brief ISteamFriends binding.
typedef struct steam_friends_api {
    void *self;                         ///< Interface pointer, or NULL when unavailable.
    rt_steam_self_cstr_fn persona_name; ///< GetPersonaName.

    int presence_ready;                              ///< Presence group bound.
    rt_steam_self_str_str_bool_fn set_rich_presence; ///< SetRichPresence.
    rt_steam_self_void_fn clear_rich_presence;       ///< ClearRichPresence.

    int overlay_ready;                               ///< Overlay activation group bound.
    rt_steam_activate_overlay_fn activate_overlay;   ///< ActivateGameOverlay.
    rt_steam_activate_web_page_fn activate_web_page; ///< ActivateGameOverlayToWebPage.
    rt_steam_activate_store_fn activate_store;       ///< ActivateGameOverlayToStore.

    int names_ready;                                               ///< Friend name group bound.
    rt_steam_friend_persona_name_fn friend_persona_name;           ///< GetFriendPersonaName.
    rt_steam_request_user_information_fn request_user_information; ///< RequestUserInformation.
} steam_friends_api;

/// @brief ISteamUtils binding (v011 in SDK 1.65, v010 in 1.61-1.64).
typedef struct steam_utils_api {
    void *self;                           ///< Interface pointer, or NULL when unavailable.
    int version;                          ///< 11 or 10 once bound.
    rt_steam_self_u32_fn get_app_id;      ///< GetAppID.
    rt_steam_self_bool_fn big_picture;    ///< IsSteamInBigPictureMode.
    rt_steam_self_enum_fn steam_hardware; ///< IsRunningOnSteamHardware (v011).
    rt_steam_self_bool_fn under_proton;   ///< IsRunningUnderProton (v011).
    rt_steam_self_bool_fn on_steam_deck;  ///< IsSteamRunningOnSteamDeck (v010).

    int overlay_ready;                                       ///< Overlay utility group bound.
    rt_steam_self_bool_fn overlay_enabled;                   ///< IsOverlayEnabled.
    rt_steam_notification_position_fn notification_position; ///< SetOverlayNotificationPosition.
    rt_steam_notification_inset_fn notification_inset;       ///< SetOverlayNotificationInset.

    int text_input_ready;                              ///< Text input group bound.
    rt_steam_show_floating_input_fn show_floating;     ///< ShowFloatingGamepadTextInput.
    rt_steam_self_bool_fn dismiss_floating;            ///< DismissFloatingGamepadTextInput.
    rt_steam_show_gamepad_input_fn show_gamepad_input; ///< ShowGamepadTextInput.
    rt_steam_self_u32_fn entered_text_length;          ///< GetEnteredGamepadTextLength.
    rt_steam_entered_gamepad_text_fn entered_text;     ///< GetEnteredGamepadTextInput.
} steam_utils_api;

/// @brief ISteamApps binding.
typedef struct steam_apps_api {
    void *self;                                 ///< Interface pointer, or NULL when unavailable.
    rt_steam_self_bool_fn is_subscribed;        ///< BIsSubscribed.
    rt_steam_self_app_bool_fn is_dlc_installed; ///< BIsDlcInstalled.
    rt_steam_self_cstr_fn game_language;        ///< GetCurrentGameLanguage.
} steam_apps_api;

/// @brief ISteamUserStats binding.
typedef struct steam_user_stats_api {
    void *self;                         ///< Interface pointer, or NULL when unavailable.
    rt_steam_self_call_fn player_count; ///< GetNumberOfCurrentPlayers.

    int achievements_ready;                                  ///< Achievement group bound.
    rt_steam_self_str_bool_fn set_achievement;               ///< SetAchievement.
    rt_steam_self_str_bool_fn clear_achievement;             ///< ClearAchievement.
    rt_steam_get_achievement_fn get_achievement;             ///< GetAchievementAndUnlockTime.
    rt_steam_indicate_progress_fn indicate_progress;         ///< IndicateAchievementProgress.
    rt_steam_self_u32_fn num_achievements;                   ///< GetNumAchievements.
    rt_steam_achievement_name_fn achievement_name;           ///< GetAchievementName.
    rt_steam_achievement_attribute_fn achievement_attribute; ///< GetAchievementDisplayAttribute.

    int stats_ready;                             ///< Stat group bound.
    rt_steam_get_stat_int_fn get_stat_int;       ///< GetStatInt32.
    rt_steam_set_stat_int_fn set_stat_int;       ///< SetStatInt32.
    rt_steam_get_stat_float_fn get_stat_float;   ///< GetStatFloat.
    rt_steam_set_stat_float_fn set_stat_float;   ///< SetStatFloat.
    rt_steam_update_avg_rate_fn update_avg_rate; ///< UpdateAvgRateStat.
    rt_steam_self_bool_fn store_stats;           ///< StoreStats.
    rt_steam_reset_all_stats_fn reset_all_stats; ///< ResetAllStats.

    int leaderboards_ready;                                      ///< Leaderboard group bound.
    rt_steam_find_leaderboard_fn find_leaderboard;               ///< FindLeaderboard.
    rt_steam_find_or_create_leaderboard_fn find_or_create;       ///< FindOrCreateLeaderboard.
    rt_steam_leaderboard_name_fn leaderboard_name;               ///< GetLeaderboardName.
    rt_steam_leaderboard_entry_count_fn leaderboard_entry_count; ///< GetLeaderboardEntryCount.
    rt_steam_download_entries_fn download_entries;               ///< DownloadLeaderboardEntries.
    rt_steam_downloaded_entry_fn downloaded_entry;               ///< GetDownloadedLeaderboardEntry.
    rt_steam_upload_score_fn upload_score;                       ///< UploadLeaderboardScore.
} steam_user_stats_api;

/// @brief ISteamRemoteStorage binding.
typedef struct steam_remote_storage_api {
    void *self;                                        ///< Interface pointer, or NULL.
    rt_steam_file_write_fn file_write;                 ///< FileWrite.
    rt_steam_file_read_fn file_read;                   ///< FileRead.
    rt_steam_self_str_bool_fn file_exists;             ///< FileExists.
    rt_steam_self_str_bool_fn file_delete;             ///< FileDelete.
    rt_steam_file_size_fn file_size;                   ///< GetFileSize.
    rt_steam_file_timestamp_fn file_timestamp;         ///< GetFileTimestamp.
    rt_steam_file_count_fn file_count;                 ///< GetFileCount.
    rt_steam_file_name_and_size_fn file_name_and_size; ///< GetFileNameAndSize.
    rt_steam_quota_fn quota;                           ///< GetQuota.
    rt_steam_self_bool_fn enabled_for_account;         ///< IsCloudEnabledForAccount.
    rt_steam_self_bool_fn enabled_for_app;             ///< IsCloudEnabledForApp.
    rt_steam_self_bool_fn begin_batch;                 ///< BeginFileWriteBatch.
    rt_steam_self_bool_fn end_batch;                   ///< EndFileWriteBatch.
} steam_remote_storage_api;

/// @brief Stage of a pending request operation.
typedef enum steam_op_stage {
    STEAM_OP_FREE = 0,          ///< Slot unused.
    STEAM_OP_PLAYER_COUNT,      ///< Waiting for NumberOfCurrentPlayers_t.
    STEAM_OP_FIND,              ///< Waiting for LeaderboardFindResult_t (Find/FindOrCreate).
    STEAM_OP_FIND_FOR_UPLOAD,   ///< Waiting for LeaderboardFindResult_t before uploading.
    STEAM_OP_FIND_FOR_DOWNLOAD, ///< Waiting for LeaderboardFindResult_t before downloading.
    STEAM_OP_UPLOAD,            ///< Waiting for LeaderboardScoreUploaded_t.
    STEAM_OP_DOWNLOAD,          ///< Waiting for LeaderboardScoresDownloaded_t.
    STEAM_OP_TEXT_INPUT,        ///< Waiting for GamepadTextInputDismissed_t.
} steam_op_stage;

/// @brief One request the provider is serving.
typedef struct steam_request_op {
    uint64_t token;         ///< Provider handle given to the services core (nonzero).
    steam_op_stage stage;   ///< What the operation waits for.
    rt_steam_api_call call; ///< Outstanding SteamAPICall_t, or 0 while waiting on a callback.
    char board[RT_STEAM_LEADERBOARD_NAME_CAPACITY]; ///< Leaderboard name for leaderboard kinds.
    uint64_t leaderboard;                           ///< Resolved SteamLeaderboard_t, or 0.
    int32_t score;                                  ///< LeaderboardUpload score.
    int keep_best;                                  ///< LeaderboardUpload keeps a better score.
    int data_request; ///< LeaderboardDownload ELeaderboardDataRequest.
    int range_start;  ///< LeaderboardDownload range start.
    int range_end;    ///< LeaderboardDownload range end.
} steam_request_op;

/// @brief One cached leaderboard name-to-handle mapping.
typedef struct steam_leaderboard_cache_entry {
    char name[RT_STEAM_LEADERBOARD_NAME_CAPACITY]; ///< Leaderboard name, or "" when unused.
    uint64_t leaderboard;                          ///< SteamLeaderboard_t handle.
} steam_leaderboard_cache_entry;

/// @brief Process-global Steam provider state (main thread only).
typedef struct steam_state {
    void *library;                           ///< Loaded redistributable, or NULL.
    char library_path[STEAM_PATH_CAPACITY];  ///< Path of @ref library.
    steam_core_api core;                     ///< Core exports of @ref library.
    int started;                             ///< Nonzero between start and stop.
    rt_steam_pipe pipe;                      ///< Client pipe while started.
    steam_user_api user;                     ///< ISteamUser binding.
    steam_friends_api friends;               ///< ISteamFriends binding.
    steam_utils_api utils;                   ///< ISteamUtils binding.
    steam_apps_api apps;                     ///< ISteamApps binding.
    steam_user_stats_api user_stats;         ///< ISteamUserStats binding.
    steam_remote_storage_api remote_storage; ///< ISteamRemoteStorage binding.
    steam_request_op ops[RT_SERVICES_PENDING_REQUEST_CAPACITY]; ///< Pending operations.
    uint64_t next_token;                                        ///< Next provider token.
    steam_leaderboard_cache_entry boards[STEAM_LEADERBOARD_CACHE_CAPACITY]; ///< Board cache.
    int next_board_slot; ///< Cache slot replaced next once the cache is full.
} steam_state;

/// @brief The Steam provider state, defined in rt_steam_provider.c.
extern steam_state rt_services_steam_state;

/// @brief Short alias used throughout the Steam provider sources.
#define g_steam rt_services_steam_state

//===----------------------------------------------------------------------===//
// Shared helpers (rt_steam_provider.c)
//===----------------------------------------------------------------------===//

/// @brief Parse a decimal Steam app or DLC id.
/// @param text NUL-terminated candidate; digits only.
/// @param out Receives the id on success.
/// @return 1 when @p text is an integer in 1..4294967295, otherwise 0.
int rt_services_steam_parse_id(const char *text, uint32_t *out);

/// @brief Resolve an export from the loaded redistributable.
/// @param name Exported symbol name.
/// @return Symbol address, or NULL.
void *rt_services_steam_symbol(const char *name);

/// @brief Record that an interface or export could not be bound.
/// @param what Accessor or export name(s).
/// @param features Human-readable list of disabled features.
void rt_services_steam_report_missing(const char *what, const char *features);

/// @brief Check a callback payload against its declared layout size.
/// @details Records "Steam: callback <id> payload is <n> bytes; binding expects <m>"
///          when the sizes differ.
/// @param msg Dispatched callback.
/// @param expected Declared payload size in bytes.
/// @return 1 when the payload may be decoded, otherwise 0.
int rt_services_steam_payload_matches(const rt_steam_callback_msg *msg, size_t expected);

/// @brief Fetch a call result after validating its identifier and size.
/// @details On failure writes a sentence into @p error, also records a
///          diagnostic for a layout mismatch, and returns 0.
/// @param completed Decoded SteamAPICallCompleted_t.
/// @param callback_id Expected result identifier.
/// @param buffer Destination for the result structure.
/// @param size Declared size of the result structure.
/// @param method Steam method name used in failure messages.
/// @param error Receives the failure message.
/// @param error_capacity Size of @p error in bytes.
/// @return 1 when the result was copied into @p buffer, otherwise 0.
int rt_services_steam_fetch_call_result(const rt_steam_api_call_completed *completed,
                                        int callback_id,
                                        void *buffer,
                                        size_t size,
                                        const char *method,
                                        char *error,
                                        size_t error_capacity);

/// @brief Allocate an operation slot and a fresh provider token.
/// @param stage Initial stage.
/// @return Operation slot, or NULL when the table is full.
steam_request_op *rt_services_steam_op_alloc(steam_op_stage stage);

/// @brief Find the operation waiting on a Steam call.
/// @param call SteamAPICall_t from a SteamAPICallCompleted_t.
/// @return Operation slot, or NULL.
steam_request_op *rt_services_steam_op_find_call(rt_steam_api_call call);

/// @brief Find the first operation in a stage.
/// @param stage Stage to look for.
/// @return Operation slot, or NULL.
steam_request_op *rt_services_steam_op_find_stage(steam_op_stage stage);

/// @brief Release an operation slot.
/// @param op Slot to clear; NULL is ignored.
void rt_services_steam_op_free(steam_request_op *op);

/// @brief Fail a pending operation's request and release its slot.
/// @param op Operation to fail.
/// @param error Failure message.
void rt_services_steam_op_fail(steam_request_op *op, const char *error);

//===----------------------------------------------------------------------===//
// Feature bindings (rt_steam_user_stats.c, rt_steam_social.c, rt_steam_cloud.c)
//===----------------------------------------------------------------------===//

/// @brief Bind the achievement, stat, and leaderboard groups of an open ISteamUserStats.
void rt_services_steam_bind_user_stats(void);

/// @brief Bind the presence, overlay, and text input groups of open ISteamFriends/ISteamUtils.
void rt_services_steam_bind_social(void);

/// @brief Open and bind ISteamRemoteStorage.
void rt_services_steam_bind_cloud(void);

/// @brief Forget per-session feature state (leaderboard cache) at stop.
void rt_services_steam_reset_user_stats(void);

/// @brief Decode stats callbacks (UserStatsStored_t, UserAchievementStored_t).
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a stats callback, otherwise 0.
int rt_services_steam_user_stats_callback(const rt_steam_callback_msg *msg);

/// @brief Decode text input callbacks (gamepad and floating keyboard dismissal).
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a text input callback, otherwise 0.
int rt_services_steam_social_callback(const rt_steam_callback_msg *msg);

/// @brief Advance a leaderboard operation whose Steam call completed.
/// @param op Operation in a leaderboard stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_leaderboard_call_completed(steam_request_op *op,
                                                  const rt_steam_api_call_completed *completed);

/// @brief Start a leaderboard request.
/// @param args Validated leaderboard request arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_leaderboard(const rt_services_request_args *args,
                                           uint64_t *out_handle,
                                           char *message,
                                           size_t message_capacity);

/// @brief Start a full-screen text input request.
/// @param args Validated TextInput request arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_text_input(const rt_services_request_args *args,
                                          uint64_t *out_handle,
                                          char *message,
                                          size_t message_capacity);

/// @brief Achievements operations backed by ISteamUserStats.
extern const rt_services_achievement_ops rt_services_steam_achievement_ops;
/// @brief Stats operations backed by ISteamUserStats.
extern const rt_services_stat_ops rt_services_steam_stat_ops;
/// @brief Leaderboard operations backed by ISteamUserStats and ISteamFriends.
extern const rt_services_leaderboard_ops rt_services_steam_leaderboard_ops;
/// @brief Presence operations backed by ISteamFriends.
extern const rt_services_presence_ops rt_services_steam_presence_ops;
/// @brief Overlay operations backed by ISteamFriends and ISteamUtils.
extern const rt_services_overlay_ops rt_services_steam_overlay_ops;
/// @brief Text input operations backed by ISteamUtils.
extern const rt_services_text_input_ops rt_services_steam_text_input_ops;
/// @brief Cloud operations backed by ISteamRemoteStorage.
extern const rt_services_cloud_ops rt_services_steam_cloud_ops;

#ifdef __cplusplus
}
#endif
