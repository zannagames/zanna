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
/// @brief Capacity, including the terminator, of the launch command line buffer.
#define STEAM_LAUNCH_COMMAND_LINE_CAPACITY 4096
/// @brief Capacity, including the terminator, of DLC names and branch names read from Steam.
#define STEAM_APP_TEXT_CAPACITY 256

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

    int launch_ready;                                    ///< Launch parameter group bound.
    rt_steam_self_str_cstr_fn launch_query_param;        ///< GetLaunchQueryParam.
    rt_steam_launch_command_line_fn launch_command_line; ///< GetLaunchCommandLine.

    int details_ready;               ///< App details group bound.
    rt_steam_self_int_fn dlc_count;  ///< GetDLCCount.
    rt_steam_dlc_data_fn dlc_data;   ///< BGetDLCDataByIndex.
    rt_steam_self_int_fn build_id;   ///< GetAppBuildId.
    rt_steam_beta_name_fn beta_name; ///< GetCurrentBetaName.
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

    int icons_ready;                           ///< Achievement icon group bound (it also calls
                                               ///< GetAchievementAndUnlockTime).
    rt_steam_self_str_int_fn achievement_icon; ///< GetAchievementIcon.
    rt_steam_image_size_fn image_size;         ///< ISteamUtils::GetImageSize.
    rt_steam_image_rgba_fn image_rgba;         ///< ISteamUtils::GetImageRGBA.

    int percentages_ready;                            ///< Global percentage group bound.
    int percentages_loaded;                           ///< A percentage request succeeded.
    rt_steam_self_call_fn request_global_percentages; ///< RequestGlobalAchievementPercentages.
    rt_steam_achieved_percent_fn achieved_percent;    ///< GetAchievementAchievedPercent.

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

/// @brief ISteamTimeline binding (v004 in SDK 1.61 through 1.65).
typedef struct steam_timeline_api {
    void *self;                                         ///< Interface pointer, or NULL.
    int ready;                                          ///< Every export below resolved.
    rt_steam_timeline_tooltip_fn set_tooltip;           ///< SetTimelineTooltip.
    rt_steam_timeline_clear_tooltip_fn clear_tooltip;   ///< ClearTimelineTooltip.
    rt_steam_timeline_game_mode_fn set_game_mode;       ///< SetTimelineGameMode.
    rt_steam_timeline_add_instant_fn add_instant;       ///< AddInstantaneousTimelineEvent.
    rt_steam_timeline_add_range_fn add_range;           ///< AddRangeTimelineEvent.
    rt_steam_timeline_start_range_fn start_range;       ///< StartRangeTimelineEvent.
    rt_steam_timeline_update_range_fn update_range;     ///< UpdateRangeTimelineEvent.
    rt_steam_timeline_end_range_fn end_range;           ///< EndRangeTimelineEvent.
    rt_steam_self_u64_void_fn remove_event;             ///< RemoveTimelineEvent.
    rt_steam_self_u64_call_fn event_recording;          ///< DoesEventRecordingExist.
    rt_steam_self_void_fn start_phase;                  ///< StartGamePhase.
    rt_steam_self_void_fn end_phase;                    ///< EndGamePhase.
    rt_steam_self_str_void_fn set_phase_id;             ///< SetGamePhaseID.
    rt_steam_self_str_call_fn phase_recording;          ///< DoesGamePhaseRecordingExist.
    rt_steam_timeline_phase_tag_fn add_phase_tag;       ///< AddGamePhaseTag.
    rt_steam_timeline_phase_attribute_fn set_attribute; ///< SetGamePhaseAttribute.
    rt_steam_self_str_void_fn overlay_to_phase;         ///< OpenOverlayToGamePhase.
    rt_steam_self_u64_void_fn overlay_to_event;         ///< OpenOverlayToTimelineEvent.
} steam_timeline_api;

/// @brief ISteamUGC binding (v021 in SDK 1.65, v020 in SDK 1.61).
typedef struct steam_ugc_api {
    void *self;                                  ///< Interface pointer, or NULL.
    int ready;                                   ///< Accessor and every export resolved.
    int version;                                 ///< 21 or 20: selects the subscribed-item forms.
    rt_steam_ugc_query_user_fn query_user;       ///< CreateQueryUserUGCRequest.
    rt_steam_ugc_query_all_fn query_all;         ///< CreateQueryAllUGCRequestPage.
    rt_steam_ugc_query_details_fn query_details; ///< CreateQueryUGCDetailsRequest.
    rt_steam_self_u64_call_fn send_query;        ///< SendQueryUGCRequest.
    rt_steam_ugc_query_result_fn query_result;   ///< GetQueryUGCResult.
    rt_steam_ugc_query_text_fn preview_url;      ///< GetQueryUGCPreviewURL.
    rt_steam_ugc_query_text_fn query_metadata;   ///< GetQueryUGCMetadata.
    rt_steam_self_u64_bool_fn release_query;     ///< ReleaseQueryUGCRequest.
    rt_steam_self_u64_str_bool_fn add_required_tag;              ///< AddRequiredTag.
    rt_steam_self_u64_str_bool_fn set_search_text;               ///< SetSearchText.
    rt_steam_self_u64_flag_bool_fn return_long_description;      ///< SetReturnLongDescription.
    rt_steam_self_u64_flag_bool_fn return_metadata;              ///< SetReturnMetadata.
    rt_steam_ugc_create_item_fn create_item;                     ///< CreateItem.
    rt_steam_ugc_start_update_fn start_update;                   ///< StartItemUpdate.
    rt_steam_self_u64_str_bool_fn set_title;                     ///< SetItemTitle.
    rt_steam_self_u64_str_bool_fn set_description;               ///< SetItemDescription.
    rt_steam_self_u64_str_bool_fn set_metadata;                  ///< SetItemMetadata.
    rt_steam_ugc_set_visibility_fn set_visibility;               ///< SetItemVisibility.
    rt_steam_ugc_set_tags_fn set_tags;                           ///< SetItemTags.
    rt_steam_self_u64_str_bool_fn set_content;                   ///< SetItemContent.
    rt_steam_self_u64_str_bool_fn set_preview;                   ///< SetItemPreview.
    rt_steam_ugc_submit_update_fn submit_update;                 ///< SubmitItemUpdate.
    rt_steam_ugc_update_progress_fn update_progress;             ///< GetItemUpdateProgress.
    rt_steam_self_u64_call_fn subscribe;                         ///< SubscribeItem.
    rt_steam_self_u64_call_fn unsubscribe;                       ///< UnsubscribeItem.
    rt_steam_self_u64_call_fn delete_item;                       ///< DeleteItem.
    rt_steam_ugc_subscribed_count_v020_fn subscribed_count_v020; ///< GetNumSubscribedItems (v020).
    rt_steam_ugc_subscribed_count_v021_fn subscribed_count_v021; ///< GetNumSubscribedItems (v021).
    rt_steam_ugc_subscribed_items_v020_fn subscribed_items_v020; ///< GetSubscribedItems (v020).
    rt_steam_ugc_subscribed_items_v021_fn subscribed_items_v021; ///< GetSubscribedItems (v021).
    rt_steam_self_u64_u32_fn item_state;                         ///< GetItemState.
    rt_steam_ugc_install_info_fn install_info;                   ///< GetItemInstallInfo.
    rt_steam_ugc_download_info_fn download_info;                 ///< GetItemDownloadInfo.
    rt_steam_self_u64_flag_bool_fn download;                     ///< DownloadItem.
} steam_ugc_api;

/// @brief One cached Steam Input handle lookup.
typedef struct steam_input_handle_entry {
    char *name;      ///< Heap copy of the manifest name.
    uint64_t handle; ///< Handle Steam returned, or 0 when it does not know the name.
} steam_input_handle_entry;

/// @brief Growable cache of Steam Input handle lookups (heap, freed at stop).
/// @details Unknown names are cached as 0 too, so a per-frame query does not
///          repeat a failing lookup; the cache is cleared whenever a
///          configuration or controller change could make a name known.
typedef struct steam_input_handle_cache {
    steam_input_handle_entry *entries; ///< Entries, or NULL.
    int count;                         ///< Entries in use.
    int capacity;                      ///< Allocated entries.
} steam_input_handle_cache;

/// @brief ISteamInput binding (v007 in SDK 1.65, v006 in SDK 1.61 through 1.64).
typedef struct steam_input_api {
    void *self;                                      ///< Interface pointer, or NULL.
    int ready;                                       ///< Accessor and every export below resolved.
    int started;                                     ///< ISteamInput::Init succeeded.
    rt_steam_input_init_fn init;                     ///< Init.
    rt_steam_self_bool_fn shutdown;                  ///< Shutdown.
    rt_steam_self_str_bool_fn set_manifest;          ///< SetInputActionManifestFilePath.
    rt_steam_input_run_frame_fn run_frame;           ///< RunFrame.
    rt_steam_input_controllers_fn connected;         ///< GetConnectedControllers.
    rt_steam_self_void_fn device_callbacks;          ///< EnableDeviceCallbacks.
    rt_steam_self_str_u64_fn action_set_handle;      ///< GetActionSetHandle (sets and layers).
    rt_steam_self_u64_u64_void_fn activate_set;      ///< ActivateActionSet.
    rt_steam_self_u64_u64_fn current_set;            ///< GetCurrentActionSet.
    rt_steam_self_u64_u64_void_fn activate_layer;    ///< ActivateActionSetLayer.
    rt_steam_self_u64_u64_void_fn deactivate_layer;  ///< DeactivateActionSetLayer.
    rt_steam_self_u64_void_fn deactivate_all_layers; ///< DeactivateAllActionSetLayers.
    rt_steam_self_str_u64_fn digital_handle;         ///< GetDigitalActionHandle.
    rt_steam_input_digital_data_fn digital_data;     ///< GetDigitalActionData.
    rt_steam_input_origins_fn digital_origins;       ///< GetDigitalActionOrigins.
    rt_steam_self_u64_cstr_fn digital_name;          ///< GetStringForDigitalActionName.
    rt_steam_self_str_u64_fn analog_handle;          ///< GetAnalogActionHandle.
    rt_steam_input_analog_data_fn analog_data;       ///< GetAnalogActionData.
    rt_steam_input_origins_fn analog_origins;        ///< GetAnalogActionOrigins.
    rt_steam_self_u64_cstr_fn analog_name;           ///< GetStringForAnalogActionName.
    rt_steam_input_glyph_png_fn glyph_png;           ///< GetGlyphPNGForActionOrigin.
    rt_steam_self_int_cstr_fn origin_name;           ///< GetStringForActionOrigin.
    rt_steam_input_vibration_fn vibrate;             ///< TriggerVibration.
    rt_steam_input_led_fn led_color;                 ///< SetLEDColor.
    rt_steam_self_u64_bool_fn binding_panel;         ///< ShowBindingPanel.
    rt_steam_self_u64_int_fn input_type;             ///< GetInputTypeForHandle.
    rt_steam_self_u64_int_fn gamepad_index;          ///< GetGamepadIndexForController.
    char *pending_manifest; ///< Manifest Steam has not accepted yet (heap), or NULL.
    uint64_t controllers[RT_STEAM_INPUT_MAX_COUNT]; ///< Connected controllers at the last pump.
    int controller_count;                           ///< Entries in @ref controllers.
    steam_input_handle_cache action_sets;           ///< Action set and layer handles.
    steam_input_handle_cache digital_actions;       ///< Digital action handles.
    steam_input_handle_cache analog_actions;        ///< Analog action handles.
} steam_input_api;

/// @brief Stage of a pending request operation.
typedef enum steam_op_stage {
    STEAM_OP_FREE = 0,                 ///< Slot unused.
    STEAM_OP_PLAYER_COUNT,             ///< Waiting for NumberOfCurrentPlayers_t.
    STEAM_OP_FIND,                     ///< Waiting for LeaderboardFindResult_t (Find/FindOrCreate).
    STEAM_OP_FIND_FOR_UPLOAD,          ///< Waiting for LeaderboardFindResult_t before uploading.
    STEAM_OP_FIND_FOR_DOWNLOAD,        ///< Waiting for LeaderboardFindResult_t before downloading.
    STEAM_OP_UPLOAD,                   ///< Waiting for LeaderboardScoreUploaded_t.
    STEAM_OP_DOWNLOAD,                 ///< Waiting for LeaderboardScoresDownloaded_t.
    STEAM_OP_TEXT_INPUT,               ///< Waiting for GamepadTextInputDismissed_t.
    STEAM_OP_TIMELINE_EVENT_RECORDING, ///< Waiting for SteamTimelineEventRecordingExists_t.
    STEAM_OP_TIMELINE_PHASE_RECORDING, ///< Waiting for SteamTimelineGamePhaseRecordingExists_t.
    STEAM_OP_ACHIEVEMENT_PERCENTAGES,  ///< Waiting for GlobalAchievementPercentagesReady_t.
    STEAM_OP_UGC_QUERY,                ///< Waiting for SteamUGCQueryCompleted_t.
    STEAM_OP_UGC_SUBSCRIBE,            ///< Waiting for RemoteStorageSubscribePublishedFileResult_t.
    STEAM_OP_UGC_UNSUBSCRIBE, ///< Waiting for RemoteStorageUnsubscribePublishedFileResult_t.
    STEAM_OP_UGC_CREATE,      ///< Waiting for CreateItemResult_t.
    STEAM_OP_UGC_SUBMIT,      ///< Waiting for SubmitItemUpdateResult_t.
    STEAM_OP_UGC_DELETE,      ///< Waiting for DeleteItemResult_t.
} steam_op_stage;

/// @brief One request the provider is serving.
typedef struct steam_request_op {
    uint64_t token;         ///< Provider handle given to the services core (nonzero).
    steam_op_stage stage;   ///< What the operation waits for.
    rt_steam_api_call call; ///< Outstanding SteamAPICall_t, or 0 while waiting on a callback.
    char name[RT_STEAM_LEADERBOARD_NAME_CAPACITY]; ///< Leaderboard name, or the id a query names.
    uint64_t leaderboard;                          ///< Resolved SteamLeaderboard_t, or 0.
    int32_t score;                                 ///< LeaderboardUpload score.
    int keep_best;                                 ///< LeaderboardUpload keeps a better score.
    int data_request;                              ///< LeaderboardDownload ELeaderboardDataRequest.
    int range_start;                               ///< LeaderboardDownload range start.
    int range_end;                                 ///< LeaderboardDownload range end.
    uint64_t ugc_query; ///< UGCQueryHandle_t a Workshop query owns, or RT_STEAM_UGC_INVALID_HANDLE.
} steam_request_op;

/// @brief One cached leaderboard name-to-handle mapping.
typedef struct steam_leaderboard_cache_entry {
    char name[RT_STEAM_LEADERBOARD_NAME_CAPACITY]; ///< Leaderboard name, or "" when unused.
    uint64_t leaderboard;                          ///< SteamLeaderboard_t handle.
} steam_leaderboard_cache_entry;

/// @brief An achievement icon variant Steam reported as unset.
/// @details Steam answers every GetAchievementIcon call for an unset icon with
///          another UserAchievementIconFetched_t, so the binding remembers
///          these and neither asks again nor reports them twice.
typedef struct steam_missing_icon {
    char name[RT_STEAM_STAT_NAME_CAPACITY]; ///< Achievement API name.
    int achieved;                           ///< Nonzero for the unlocked variant.
} steam_missing_icon;

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
    steam_timeline_api timeline;             ///< ISteamTimeline binding.
    steam_input_api input;                   ///< ISteamInput binding.
    steam_ugc_api ugc;                       ///< ISteamUGC binding.
    steam_request_op ops[RT_SERVICES_PENDING_REQUEST_CAPACITY]; ///< Pending operations.
    uint64_t next_token;                                        ///< Next provider token.
    steam_leaderboard_cache_entry boards[STEAM_LEADERBOARD_CACHE_CAPACITY]; ///< Board cache.
    int next_board_slot;               ///< Cache slot replaced next once the cache is full.
    steam_missing_icon *missing_icons; ///< Unset icon variants (heap, freed on stop).
    int missing_icon_count;            ///< Entries in @ref missing_icons.
    int missing_icon_capacity;         ///< Allocated entries in @ref missing_icons.
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

/// @brief Open and bind ISteamTimeline.
void rt_services_steam_bind_timeline(void);

/// @brief Open and bind ISteamUGC.
void rt_services_steam_bind_workshop(void);

/// @brief Start a Workshop request.
/// @param args Validated RT_SERVICES_REQUEST_WORKSHOP_* arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_workshop_request(const rt_services_request_args *args,
                                                uint64_t *out_handle,
                                                char *message,
                                                size_t message_capacity);

/// @brief Complete a Workshop request whose Steam call completed.
/// @param op Operation in a Workshop stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_workshop_call_completed(steam_request_op *op,
                                               const rt_steam_api_call_completed *completed);

/// @brief Release the Steam query handle of a Workshop operation that will never complete.
/// @param op Operation being freed.
void rt_services_steam_workshop_op_release(steam_request_op *op);

/// @brief Decode Workshop callbacks (item installed, download result, subscription changes).
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a Workshop callback, otherwise 0.
int rt_services_steam_workshop_callback(const rt_steam_callback_msg *msg);

/// @brief Open and bind ISteamInput; action input itself starts with ActionInput.Start.
void rt_services_steam_bind_input(void);

/// @brief Run a Steam Input frame and refresh the connected controllers.
/// @details Called at the start of each provider pump; does nothing unless
///          action input is started.
void rt_services_steam_input_frame(void);

/// @brief Shut Steam Input down and free its caches; called before SteamAPI_Shutdown.
void rt_services_steam_stop_input(void);

/// @brief Decode Steam Input device and configuration callbacks.
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a Steam Input callback, otherwise 0.
int rt_services_steam_input_callback(const rt_steam_callback_msg *msg);

/// @brief Parse a decimal, nonzero uint64 id (timeline events, SteamID64 values).
/// @param text NUL-terminated candidate; digits only.
/// @param out Receives the id on success.
/// @return 1 when @p text is an integer in 1..18446744073709551615, otherwise 0.
int rt_services_steam_parse_u64(const char *text, uint64_t *out);

/// @brief Start a timeline recording query.
/// @param args Validated TimelineEventRecording or TimelinePhaseRecording arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_timeline_request(const rt_services_request_args *args,
                                                uint64_t *out_handle,
                                                char *message,
                                                size_t message_capacity);

/// @brief Complete a timeline recording query whose Steam call completed.
/// @param op Operation in a timeline stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_timeline_call_completed(steam_request_op *op,
                                               const rt_steam_api_call_completed *completed);

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

/// @brief Start a global achievement percentage request.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_achievement_percentages(uint64_t *out_handle,
                                                       char *message,
                                                       size_t message_capacity);

/// @brief Complete a global achievement percentage request whose Steam call completed.
/// @param op Operation in the achievement percentage stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_achievement_percentages_completed(
    steam_request_op *op, const rt_steam_api_call_completed *completed);

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
/// @brief Timeline operations backed by ISteamTimeline.
extern const rt_services_timeline_ops rt_services_steam_timeline_ops;
/// @brief Action input operations backed by ISteamInput.
extern const rt_services_action_input_ops rt_services_steam_action_input_ops;
/// @brief Workshop operations backed by ISteamUGC.
extern const rt_services_workshop_ops rt_services_steam_workshop_ops;

#ifdef __cplusplus
}
#endif
