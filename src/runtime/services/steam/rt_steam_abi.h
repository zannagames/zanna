//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_abi.h
// Purpose: Authored declarations of the subset of the Steamworks flat C API
//          and callback payload layouts used by the Zanna Steam provider.
// Key invariants:
//   - Only facts required for binary interoperability are declared here:
//     exported symbol names, C function types, callback identifiers, and
//     payload layouts. No Steamworks SDK file is vendored or included.
//   - Callback payloads use the redistributables' packing: 8-byte packing on
//     Windows and 4-byte packing on macOS and Linux. The Steam Input action
//     data returned by value uses one-byte packing on every platform. Sizes
//     and offsets are pinned by compile-time assertions in
//     rt_steam_provider.c.
//   - Every method type listed here has an identical signature in all
//     supported redistributables (Steamworks SDK 1.61 through 1.65), except
//     ISteamUGC's GetNumSubscribedItems and GetSubscribedItems, which gained a
//     bool parameter with SteamUGC_v021; both forms are declared.
//   - C `bool` maps to the one-byte C++ bool used by the flat API on every
//     supported ABI; enums are 32-bit integers. Payload fields that are C++
//     bool are declared uint8_t so arbitrary payload bytes are never read
//     through a C _Bool.
// Ownership/Lifetime:
//   - Interface pointers are borrowed from the Steam client and stay valid
//     between SteamAPI_InitFlat and SteamAPI_Shutdown.
//   - Callback payload pointers are valid only until the matching
//     SteamAPI_ManualDispatch_FreeLastCallback call.
// Links: src/runtime/services/steam/rt_steam_provider.c,
//        src/runtime/services/steam/rt_steam_user_stats.c,
//        src/runtime/services/steam/rt_steam_social.c,
//        src/runtime/services/steam/rt_steam_cloud.c,
//        src/runtime/services/steam/rt_steam_input.c,
//        src/runtime/services/steam/rt_steam_workshop.c,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_abi.h
 * @brief Declares the Steamworks flat-API types and layouts Zanna binds at run time.
 * @details The Steam provider resolves these functions from a developer-shipped
 *          steam_api redistributable with rt_services_dynlib_symbol. Names in
 *          the RT_STEAM_SYMBOL_* macros are the exact exported symbols. The
 *          structures mirror the documented callback payloads under the
 *          platform packing rule so a payload can be copied into them after
 *          its size has been checked.
 */

#pragma once

#include "rt_platform.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Basic types
//===----------------------------------------------------------------------===//

/// @brief Client pipe handle returned by SteamAPI_GetHSteamPipe (HSteamPipe).
typedef int32_t rt_steam_pipe;

/// @brief Asynchronous call handle (SteamAPICall_t); 0 means invalid.
typedef uint64_t rt_steam_api_call;

/// @brief Capacity of the SteamErrMsg buffer written by SteamAPI_InitFlat.
#define RT_STEAM_ERR_MSG_CAPACITY 1024

/// @brief ESteamAPIInitResult: initialization succeeded.
#define RT_STEAM_INIT_RESULT_OK 0
/// @brief ESteamAPIInitResult: initialization failed for an unspecified reason.
#define RT_STEAM_INIT_RESULT_FAILED_GENERIC 1
/// @brief ESteamAPIInitResult: the Steam client is not running.
#define RT_STEAM_INIT_RESULT_NO_STEAM_CLIENT 2
/// @brief ESteamAPIInitResult: the Steam client is older than the redistributable.
#define RT_STEAM_INIT_RESULT_VERSION_MISMATCH 3

/// @brief EResult: success.
#define RT_STEAM_RESULT_OK 1
/// @brief EResult: generic failure.
#define RT_STEAM_RESULT_FAIL 2

/// @brief EResult: a parameter was rejected (StoreStats: stats reverted by the server).
#define RT_STEAM_RESULT_INVALID_PARAM 8

/// @brief ESteamHardwareType values reported by ISteamUtils::IsRunningOnSteamHardware (1.65+).
#define RT_STEAM_HARDWARE_NONE 0
/// @brief ESteamHardwareType: Steam Deck.
#define RT_STEAM_HARDWARE_STEAM_DECK 1
/// @brief ESteamHardwareType: Steam Machine.
#define RT_STEAM_HARDWARE_STEAM_MACHINE 2
/// @brief ESteamHardwareType: Steam Frame.
#define RT_STEAM_HARDWARE_STEAM_FRAME 3

/// @brief k_cchStatNameMax: bytes, including the terminator, of stat and achievement names.
#define RT_STEAM_STAT_NAME_CAPACITY 128
/// @brief k_cchLeaderboardNameMax: bytes, including the terminator, of a leaderboard name.
#define RT_STEAM_LEADERBOARD_NAME_CAPACITY 128
/// @brief k_unMaxCloudFileChunkSize: largest file ISteamRemoteStorage::FileWrite accepts.
#define RT_STEAM_CLOUD_FILE_MAX_BYTES (100 * 1024 * 1024)

/// @brief ELeaderboardDataRequest: absolute global ranks.
#define RT_STEAM_LEADERBOARD_REQUEST_GLOBAL 0
/// @brief ELeaderboardDataRequest: ranks relative to the user.
#define RT_STEAM_LEADERBOARD_REQUEST_GLOBAL_AROUND_USER 1
/// @brief ELeaderboardDataRequest: the user and their friends.
#define RT_STEAM_LEADERBOARD_REQUEST_FRIENDS 2

/// @brief ELeaderboardSortMethod: ascending (lowest score first).
#define RT_STEAM_LEADERBOARD_SORT_ASCENDING 1
/// @brief ELeaderboardSortMethod: descending (highest score first).
#define RT_STEAM_LEADERBOARD_SORT_DESCENDING 2

/// @brief ELeaderboardDisplayType: numeric.
#define RT_STEAM_LEADERBOARD_DISPLAY_NUMERIC 1
/// @brief ELeaderboardDisplayType: time in seconds.
#define RT_STEAM_LEADERBOARD_DISPLAY_TIME_SECONDS 2
/// @brief ELeaderboardDisplayType: time in milliseconds.
#define RT_STEAM_LEADERBOARD_DISPLAY_TIME_MILLISECONDS 3

/// @brief ELeaderboardUploadScoreMethod: keep the user's best score.
#define RT_STEAM_LEADERBOARD_UPLOAD_KEEP_BEST 1
/// @brief ELeaderboardUploadScoreMethod: always replace the user's score.
#define RT_STEAM_LEADERBOARD_UPLOAD_FORCE_UPDATE 2

/// @brief EActivateGameOverlayToWebPageMode: browser beside other overlay windows.
#define RT_STEAM_WEB_PAGE_MODE_DEFAULT 0
/// @brief EActivateGameOverlayToWebPageMode: browser alone in a modal overlay.
#define RT_STEAM_WEB_PAGE_MODE_MODAL 1

/// @brief EOverlayToStoreFlag: show the store page only.
#define RT_STEAM_STORE_FLAG_NONE 0
/// @brief EOverlayToStoreFlag: add to the cart and show the store page.
#define RT_STEAM_STORE_FLAG_ADD_TO_CART_AND_SHOW 2

/// @brief EGamepadTextInputMode: normal text.
#define RT_STEAM_GAMEPAD_INPUT_NORMAL 0
/// @brief EGamepadTextInputMode: masked text.
#define RT_STEAM_GAMEPAD_INPUT_PASSWORD 1
/// @brief EGamepadTextInputLineMode: one line.
#define RT_STEAM_GAMEPAD_LINE_SINGLE 0
/// @brief EGamepadTextInputLineMode: several lines.
#define RT_STEAM_GAMEPAD_LINE_MULTIPLE 1

/// @brief EFloatingGamepadTextInputMode: one line; Enter dismisses.
#define RT_STEAM_FLOATING_INPUT_SINGLE_LINE 0
/// @brief EFloatingGamepadTextInputMode: several lines; the user dismisses explicitly.
#define RT_STEAM_FLOATING_INPUT_MULTIPLE_LINES 1
/// @brief EFloatingGamepadTextInputMode: email layout.
#define RT_STEAM_FLOATING_INPUT_EMAIL 2
/// @brief EFloatingGamepadTextInputMode: numeric layout.
#define RT_STEAM_FLOATING_INPUT_NUMERIC 3

//===----------------------------------------------------------------------===//
// Callback identifiers (k_iSteam<Interface>Callbacks base + offset)
//===----------------------------------------------------------------------===//

/// @brief SteamServersConnected_t (ISteamUser base 100 + 1); empty payload.
#define RT_STEAM_CB_SERVERS_CONNECTED 101
/// @brief SteamServerConnectFailure_t (100 + 2).
#define RT_STEAM_CB_SERVER_CONNECT_FAILURE 102
/// @brief SteamServersDisconnected_t (100 + 3).
#define RT_STEAM_CB_SERVERS_DISCONNECTED 103
/// @brief GameOverlayActivated_t (ISteamFriends base 300 + 31).
#define RT_STEAM_CB_GAME_OVERLAY_ACTIVATED 331
/// @brief SteamAPICallCompleted_t (ISteamUtils base 700 + 3).
#define RT_STEAM_CB_API_CALL_COMPLETED 703
/// @brief SteamShutdown_t (700 + 4); empty payload.
#define RT_STEAM_CB_STEAM_SHUTDOWN 704
/// @brief GamepadTextInputDismissed_t (700 + 14).
#define RT_STEAM_CB_GAMEPAD_TEXT_INPUT_DISMISSED 714
/// @brief FloatingGamepadTextInputDismissed_t (700 + 38); empty payload.
#define RT_STEAM_CB_FLOATING_GAMEPAD_TEXT_INPUT_DISMISSED 738
/// @brief DlcInstalled_t (ISteamApps base 1000 + 5).
#define RT_STEAM_CB_DLC_INSTALLED 1005
/// @brief NewUrlLaunchParameters_t (1000 + 14); empty payload.
#define RT_STEAM_CB_NEW_URL_LAUNCH_PARAMETERS 1014
/// @brief UserStatsStored_t (ISteamUserStats base 1100 + 2).
#define RT_STEAM_CB_USER_STATS_STORED 1102
/// @brief UserAchievementStored_t (1100 + 3).
#define RT_STEAM_CB_USER_ACHIEVEMENT_STORED 1103
/// @brief LeaderboardFindResult_t call result (1100 + 4).
#define RT_STEAM_CB_LEADERBOARD_FIND_RESULT 1104
/// @brief LeaderboardScoresDownloaded_t call result (1100 + 5).
#define RT_STEAM_CB_LEADERBOARD_SCORES_DOWNLOADED 1105
/// @brief LeaderboardScoreUploaded_t call result (1100 + 6).
#define RT_STEAM_CB_LEADERBOARD_SCORE_UPLOADED 1106
/// @brief NumberOfCurrentPlayers_t call result (ISteamUserStats base 1100 + 7).
#define RT_STEAM_CB_NUMBER_OF_CURRENT_PLAYERS 1107
/// @brief UserAchievementIconFetched_t (1100 + 9).
#define RT_STEAM_CB_USER_ACHIEVEMENT_ICON_FETCHED 1109
/// @brief GlobalAchievementPercentagesReady_t call result (1100 + 10).
#define RT_STEAM_CB_GLOBAL_ACHIEVEMENT_PERCENTAGES_READY 1110
/// @brief RemoteStorageSubscribePublishedFileResult_t call result (ISteamRemoteStorage base 1300 +
/// 13).
#define RT_STEAM_CB_UGC_SUBSCRIBE_RESULT 1313
/// @brief RemoteStorageUnsubscribePublishedFileResult_t call result (1300 + 15).
#define RT_STEAM_CB_UGC_UNSUBSCRIBE_RESULT 1315
/// @brief RemoteStoragePublishedFileSubscribed_t (1300 + 21).
#define RT_STEAM_CB_UGC_FILE_SUBSCRIBED 1321
/// @brief RemoteStoragePublishedFileUnsubscribed_t (1300 + 22).
#define RT_STEAM_CB_UGC_FILE_UNSUBSCRIBED 1322
/// @brief SteamUGCQueryCompleted_t call result (ISteamUGC base 3400 + 1).
#define RT_STEAM_CB_UGC_QUERY_COMPLETED 3401
/// @brief CreateItemResult_t call result (3400 + 3).
#define RT_STEAM_CB_UGC_CREATE_ITEM_RESULT 3403
/// @brief SubmitItemUpdateResult_t call result (3400 + 4).
#define RT_STEAM_CB_UGC_SUBMIT_ITEM_UPDATE_RESULT 3404
/// @brief ItemInstalled_t (3400 + 5).
#define RT_STEAM_CB_UGC_ITEM_INSTALLED 3405
/// @brief DownloadItemResult_t (3400 + 6).
#define RT_STEAM_CB_UGC_DOWNLOAD_ITEM_RESULT 3406
/// @brief DeleteItemResult_t call result (3400 + 17).
#define RT_STEAM_CB_UGC_DELETE_ITEM_RESULT 3417
/// @brief SteamInputDeviceConnected_t (ISteamInput base 2800 + 1).
#define RT_STEAM_CB_INPUT_DEVICE_CONNECTED 2801
/// @brief SteamInputDeviceDisconnected_t (2800 + 2).
#define RT_STEAM_CB_INPUT_DEVICE_DISCONNECTED 2802
/// @brief SteamInputConfigurationLoaded_t (2800 + 3).
#define RT_STEAM_CB_INPUT_CONFIGURATION_LOADED 2803
/// @brief SteamTimelineGamePhaseRecordingExists_t call result (ISteamTimeline base 6000 + 1).
#define RT_STEAM_CB_TIMELINE_PHASE_RECORDING_EXISTS 6001
/// @brief SteamTimelineEventRecordingExists_t call result (6000 + 2).
#define RT_STEAM_CB_TIMELINE_EVENT_RECORDING_EXISTS 6002

/// @brief k_cchMaxPhaseIDLength: bytes, including the terminator, of a timeline phase id.
#define RT_STEAM_TIMELINE_PHASE_ID_CAPACITY 64
/// @brief k_unTimelinePriority_KeepCurrentValue: UpdateRangeTimelineEvent keeps the priority.
#define RT_STEAM_TIMELINE_PRIORITY_KEEP_CURRENT UINT32_C(1000000)
/// @brief k_flMaxTimelineEventDuration: longest timeline range event in seconds.
#define RT_STEAM_TIMELINE_MAX_EVENT_DURATION 600.0

/// @brief STEAM_INPUT_MAX_COUNT: handles GetConnectedControllers writes at most.
#define RT_STEAM_INPUT_MAX_COUNT 16
/// @brief STEAM_INPUT_MAX_ORIGINS: origins Get*ActionOrigins writes at most.
#define RT_STEAM_INPUT_MAX_ORIGINS 8
/// @brief STEAM_INPUT_HANDLE_ALL_CONTROLLERS: addresses every controller.
#define RT_STEAM_INPUT_HANDLE_ALL_CONTROLLERS UINT64_MAX
/// @brief k_EInputActionOrigin_MaximumPossibleValue: origins fit in 16 bits.
#define RT_STEAM_INPUT_MAX_ORIGIN 32767
/// @brief ESteamInputLEDFlag: set the color passed to SetLEDColor.
#define RT_STEAM_INPUT_LED_SET_COLOR 0u
/// @brief ESteamInputLEDFlag: restore the color the player chose.
#define RT_STEAM_INPUT_LED_RESTORE_USER_DEFAULT 1u

/// @brief k_cchPublishedDocumentTitleMax: bytes of a Workshop title, terminator included.
#define RT_STEAM_UGC_TITLE_CAPACITY 129
/// @brief k_cchPublishedDocumentDescriptionMax: bytes of a Workshop description.
#define RT_STEAM_UGC_DESCRIPTION_CAPACITY 8000
/// @brief k_cchTagListMax: bytes of a comma-separated Workshop tag list.
#define RT_STEAM_UGC_TAGS_CAPACITY 1025
/// @brief k_cchFilenameMax: bytes of a Workshop file name.
#define RT_STEAM_UGC_FILE_NAME_CAPACITY 260
/// @brief k_cchPublishedFileURLMax: bytes of a Workshop URL or paging cursor.
#define RT_STEAM_UGC_URL_CAPACITY 256
/// @brief k_cchDeveloperMetadataMax (SDK 1.61 onward): bytes of item metadata read back.
#define RT_STEAM_UGC_METADATA_CAPACITY 10000
/// @brief kNumUGCResultsPerPage: most results one query page returns.
#define RT_STEAM_UGC_RESULTS_PER_PAGE 50
/// @brief k_UGCQueryHandleInvalid and k_UGCUpdateHandleInvalid.
#define RT_STEAM_UGC_INVALID_HANDLE UINT64_MAX
/// @brief EUGCMatchingUGCType: ready-to-use items.
#define RT_STEAM_UGC_MATCHING_ITEMS_READY_TO_USE 2
/// @brief EWorkshopFileType: community items.
#define RT_STEAM_UGC_FILE_TYPE_COMMUNITY 0
/// @brief EUserUGCListSortOrder: newest first.
#define RT_STEAM_UGC_SORT_CREATION_DESC 0
/// @brief EUserUGCListSortOrder: most recently subscribed first.
#define RT_STEAM_UGC_SORT_SUBSCRIPTION_DATE_DESC 4

//===----------------------------------------------------------------------===//
// Callback payload layouts (redistributable packing)
//===----------------------------------------------------------------------===//

#if RT_PLATFORM_WINDOWS
/// @brief Packing used by the Windows redistributables for callback structures.
#define RT_STEAM_CALLBACK_PACK 8
#pragma pack(push, 8)
#else
/// @brief Packing used by the macOS and Linux redistributables for callback structures.
#define RT_STEAM_CALLBACK_PACK 4
#pragma pack(push, 4)
#endif

/// @brief Packing sentinel (ValvePackingSentinel_t): 32 bytes under pack 8, 24 under pack 4.
typedef struct rt_steam_packing_sentinel {
    uint32_t u32; ///< Leading 32-bit field.
    uint64_t u64; ///< Field whose alignment depends on the pack.
    uint16_t u16; ///< Trailing short field.
    double d;     ///< Field whose alignment depends on the pack.
} rt_steam_packing_sentinel;

/// @brief One manually dispatched callback (CallbackMsg_t).
typedef struct rt_steam_callback_msg {
    int32_t steam_user;  ///< HSteamUser the callback targets.
    int32_t callback_id; ///< Callback identifier (RT_STEAM_CB_*).
    uint8_t *param;      ///< Payload bytes, valid until FreeLastCallback.
    int32_t param_size;  ///< Payload size in bytes.
} rt_steam_callback_msg;

/// @brief SteamServerConnectFailure_t payload (8 bytes).
typedef struct rt_steam_server_connect_failure {
    int32_t result;         ///< EResult describing the failure.
    uint8_t still_retrying; ///< C++ bool: nonzero while the client keeps retrying.
} rt_steam_server_connect_failure;

/// @brief SteamServersDisconnected_t payload (4 bytes).
typedef struct rt_steam_servers_disconnected {
    int32_t result; ///< EResult describing the disconnect.
} rt_steam_servers_disconnected;

/// @brief GameOverlayActivated_t payload (12 bytes).
typedef struct rt_steam_game_overlay_activated {
    uint8_t active;         ///< Nonzero when the overlay just opened.
    uint8_t user_initiated; ///< C++ bool: nonzero when the user opened or closed it.
    uint32_t app_id;        ///< Application id of the game.
    uint32_t overlay_pid;   ///< Overlay process id (Steam-internal).
} rt_steam_game_overlay_activated;

/// @brief SteamAPICallCompleted_t payload (16 bytes).
typedef struct rt_steam_api_call_completed {
    rt_steam_api_call async_call; ///< Handle of the completed call.
    int32_t callback_id;          ///< Identifier of the call's result structure.
    uint32_t param_size;          ///< Size of the call's result structure.
} rt_steam_api_call_completed;

/// @brief DlcInstalled_t payload (4 bytes).
typedef struct rt_steam_dlc_installed {
    uint32_t app_id; ///< Application id of the installed DLC.
} rt_steam_dlc_installed;

/// @brief NumberOfCurrentPlayers_t call result (8 bytes).
typedef struct rt_steam_number_of_current_players {
    uint8_t success; ///< 1 when the call succeeded.
    int32_t players; ///< Number of players currently playing.
} rt_steam_number_of_current_players;

/// @brief GamepadTextInputDismissed_t payload (12 bytes).
typedef struct rt_steam_gamepad_text_input_dismissed {
    uint8_t submitted;       ///< C++ bool: nonzero when the user submitted text.
    uint32_t submitted_size; ///< Length of the submitted text in bytes.
    uint32_t app_id;         ///< Application id of the game.
} rt_steam_gamepad_text_input_dismissed;

/// @brief UserStatsStored_t payload (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_user_stats_stored {
    uint64_t game_id; ///< Game the stats belong to.
    int32_t result;   ///< EResult of the store.
} rt_steam_user_stats_stored;

/// @brief UserAchievementStored_t payload (152 bytes under pack 8, 148 under pack 4).
typedef struct rt_steam_user_achievement_stored {
    uint64_t game_id;                                   ///< Game the achievement belongs to.
    uint8_t group_achievement;                          ///< C++ bool; unused by Steam.
    char achievement_name[RT_STEAM_STAT_NAME_CAPACITY]; ///< Achievement API name.
    uint32_t current_progress; ///< Progress so far; 0 together with max_progress means unlocked.
    uint32_t max_progress; ///< Progress maximum; 0 together with current_progress means unlocked.
} rt_steam_user_achievement_stored;

/// @brief LeaderboardFindResult_t call result (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_leaderboard_find_result {
    uint64_t leaderboard; ///< SteamLeaderboard_t handle, 0 when not found.
    uint8_t found;        ///< Nonzero when the board was found.
} rt_steam_leaderboard_find_result;

/// @brief LeaderboardScoresDownloaded_t call result (24 bytes under pack 8, 20 under pack 4).
typedef struct rt_steam_leaderboard_scores_downloaded {
    uint64_t leaderboard; ///< SteamLeaderboard_t handle.
    uint64_t entries;     ///< SteamLeaderboardEntries_t handle for GetDownloadedLeaderboardEntry.
    int32_t entry_count;  ///< Number of downloaded entries.
} rt_steam_leaderboard_scores_downloaded;

/// @brief LeaderboardScoreUploaded_t call result (32 bytes under pack 8, 28 under pack 4).
typedef struct rt_steam_leaderboard_score_uploaded {
    uint8_t success;              ///< 1 when the upload succeeded.
    uint64_t leaderboard;         ///< SteamLeaderboard_t handle.
    int32_t score;                ///< Score the upload attempted to set.
    uint8_t score_changed;        ///< Nonzero when the stored score changed.
    int32_t global_rank_new;      ///< The user's global rank after the upload.
    int32_t global_rank_previous; ///< The user's previous global rank, or 0.
} rt_steam_leaderboard_score_uploaded;

/// @brief LeaderboardEntry_t (32 bytes under pack 8, 28 under pack 4).
/// @details The SDK declares the user as a one-byte-aligned CSteamID. It is the
///          first member, so a uint64_t yields the same offsets and size.
typedef struct rt_steam_leaderboard_entry {
    uint64_t steam_id;    ///< SteamID64 of the entry's user.
    int32_t global_rank;  ///< Global rank, starting at 1.
    int32_t score;        ///< Stored score.
    int32_t detail_count; ///< Number of detail values stored with the entry.
    uint64_t ugc;         ///< UGCHandle_t attached to the entry.
} rt_steam_leaderboard_entry;

/// @brief UserAchievementIconFetched_t payload (144 bytes under both packings).
typedef struct rt_steam_user_achievement_icon_fetched {
    uint64_t game_id;                                   ///< Game the achievement belongs to.
    char achievement_name[RT_STEAM_STAT_NAME_CAPACITY]; ///< Achievement API name.
    uint8_t achieved;                                   ///< C++ bool: the unlocked variant loaded.
    int32_t icon_handle;                                ///< Image handle, 0 when there is no icon.
} rt_steam_user_achievement_icon_fetched;

/// @brief GlobalAchievementPercentagesReady_t call result (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_global_achievement_percentages_ready {
    uint64_t game_id; ///< Game the percentages belong to.
    int32_t result;   ///< EResult of the request.
} rt_steam_global_achievement_percentages_ready;

/// @brief SteamParamStringArray_t (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_param_string_array {
    const char **strings; ///< Borrowed strings.
    int32_t count;        ///< Entries in @ref strings.
} rt_steam_param_string_array;

/// @brief SteamUGCDetails_t (9784 bytes under pack 8, 9772 under pack 4).
typedef struct rt_steam_ugc_details {
    uint64_t file_id;                                    ///< PublishedFileId_t.
    int32_t result;                                      ///< EResult for this item.
    int32_t file_type;                                   ///< EWorkshopFileType.
    uint32_t creator_app;                                ///< App that created the item.
    uint32_t consumer_app;                               ///< App that uses the item.
    char title[RT_STEAM_UGC_TITLE_CAPACITY];             ///< Title.
    char description[RT_STEAM_UGC_DESCRIPTION_CAPACITY]; ///< Description.
    uint64_t owner;                                      ///< SteamID64 of the author.
    uint32_t created;                                    ///< Creation time.
    uint32_t updated;                                    ///< Last update time.
    uint32_t added_to_user_list;                         ///< Time added to a user list.
    int32_t visibility;                                  ///< ERemoteStoragePublishedFileVisibility.
    uint8_t banned;                                      ///< C++ bool: banned.
    uint8_t accepted_for_use;                            ///< C++ bool: accepted by the developer.
    uint8_t tags_truncated;                              ///< C++ bool: @ref tags was cut.
    char tags[RT_STEAM_UGC_TAGS_CAPACITY];               ///< Comma-separated tags.
    uint64_t file_handle;                                ///< UGCHandle_t of the primary file.
    uint64_t preview_handle;                             ///< UGCHandle_t of the preview.
    char file_name[RT_STEAM_UGC_FILE_NAME_CAPACITY];     ///< Cloud file name (legacy items).
    int32_t file_size;                                   ///< Primary file size (legacy items).
    int32_t preview_file_size;                           ///< Preview size.
    char url[RT_STEAM_UGC_URL_CAPACITY];                 ///< Video or website URL.
    uint32_t votes_up;                                   ///< Up votes.
    uint32_t votes_down;                                 ///< Down votes.
    float score;                                         ///< Vote score.
    uint32_t children;                                   ///< Children of a collection.
    uint64_t total_files_size;                           ///< Size of all files but the preview.
} rt_steam_ugc_details;

/// @brief SteamUGCQueryCompleted_t call result (280 bytes under both packings).
typedef struct rt_steam_ugc_query_completed {
    uint64_t handle;                             ///< UGCQueryHandle_t.
    int32_t result;                              ///< EResult.
    uint32_t returned;                           ///< Results on this page.
    uint32_t total;                              ///< Matching results on every page.
    uint8_t cached;                              ///< C++ bool: answered from the local cache.
    char next_cursor[RT_STEAM_UGC_URL_CAPACITY]; ///< Cursor of the next page (cursor queries).
} rt_steam_ugc_query_completed;

/// @brief CreateItemResult_t call result (24 bytes under pack 8, 16 under pack 4).
typedef struct rt_steam_ugc_create_item_result {
    int32_t result;          ///< EResult.
    uint64_t file_id;        ///< PublishedFileId_t of the new item.
    uint8_t needs_agreement; ///< C++ bool: the user must accept the Workshop agreement.
} rt_steam_ugc_create_item_result;

/// @brief SubmitItemUpdateResult_t call result (16 bytes under both packings).
typedef struct rt_steam_ugc_submit_item_update_result {
    int32_t result;          ///< EResult.
    uint8_t needs_agreement; ///< C++ bool: the user must accept the Workshop agreement.
    uint64_t file_id;        ///< PublishedFileId_t of the item.
} rt_steam_ugc_submit_item_update_result;

/// @brief ItemInstalled_t payload (32 bytes under pack 8, 28 under pack 4).
typedef struct rt_steam_ugc_item_installed {
    uint32_t app_id;         ///< AppId_t.
    uint64_t file_id;        ///< PublishedFileId_t.
    uint64_t legacy_content; ///< UGCHandle_t of legacy content.
    uint64_t manifest_id;    ///< Manifest of the installed content.
} rt_steam_ugc_item_installed;

/// @brief DownloadItemResult_t payload (24 bytes under pack 8, 16 under pack 4).
typedef struct rt_steam_ugc_download_item_result {
    uint32_t app_id;  ///< AppId_t.
    uint64_t file_id; ///< PublishedFileId_t.
    int32_t result;   ///< EResult.
} rt_steam_ugc_download_item_result;

/// @brief DeleteItemResult_t and RemoteStorage[Un]subscribePublishedFileResult_t call results
///        (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_ugc_file_result {
    int32_t result;   ///< EResult.
    uint64_t file_id; ///< PublishedFileId_t.
} rt_steam_ugc_file_result;

/// @brief RemoteStoragePublishedFile[Un]subscribed_t payload (16 bytes under pack 8, 12 under
///        pack 4).
typedef struct rt_steam_ugc_file_subscription {
    uint64_t file_id; ///< PublishedFileId_t.
    uint32_t app_id;  ///< AppId_t.
} rt_steam_ugc_file_subscription;

/// @brief SteamInputDeviceConnected_t and SteamInputDeviceDisconnected_t payload (8 bytes).
typedef struct rt_steam_input_device {
    uint64_t device; ///< InputHandle_t of the controller.
} rt_steam_input_device;

/// @brief SteamInputConfigurationLoaded_t payload (40 bytes under pack 8, 32 under pack 4).
/// @details The SDK declares the mapping creator as a one-byte-aligned CSteamID
///          that directly follows the device handle, so a uint64_t yields the
///          same offsets and size.
typedef struct rt_steam_input_configuration_loaded {
    uint32_t app_id;          ///< AppId_t the configuration belongs to.
    uint64_t device;          ///< InputHandle_t of the controller.
    uint64_t mapping_creator; ///< SteamID64 of the configuration's author.
    uint32_t major_revision;  ///< Binding revision from the action manifest.
    uint32_t minor_revision;  ///< Minor binding revision.
    uint8_t uses_input_api;   ///< C++ bool: the configuration binds actions.
    uint8_t uses_gamepad_api; ///< C++ bool: the configuration binds gamepad (XInput) inputs.
} rt_steam_input_configuration_loaded;

/// @brief SteamTimelineGamePhaseRecordingExists_t call result (88 bytes under both packings).
typedef struct rt_steam_timeline_phase_recording_exists {
    char phase_id[RT_STEAM_TIMELINE_PHASE_ID_CAPACITY]; ///< Phase id the query named.
    uint64_t recording_ms;                              ///< Milliseconds of recorded video.
    uint64_t longest_clip_ms;                           ///< Longest saved clip in milliseconds.
    uint32_t clip_count;                                ///< Saved clips.
    uint32_t screenshot_count;                          ///< Saved screenshots.
} rt_steam_timeline_phase_recording_exists;

/// @brief SteamTimelineEventRecordingExists_t call result (16 bytes under pack 8, 12 under pack 4).
typedef struct rt_steam_timeline_event_recording_exists {
    uint64_t event_id;        ///< TimelineEventHandle_t the query named.
    uint8_t recording_exists; ///< C++ bool: nonzero when a recording covers the event.
} rt_steam_timeline_event_recording_exists;

#pragma pack(pop)

#pragma pack(push, 1)

/// @brief InputDigitalActionData_t (2 bytes, one-byte packing on every platform).
/// @details Returned by value from GetDigitalActionData.
typedef struct rt_steam_input_digital_data {
    uint8_t state;  ///< C++ bool: the action is pressed.
    uint8_t active; ///< C++ bool: the action is available in the active action set.
} rt_steam_input_digital_data;

/// @brief InputAnalogActionData_t (13 bytes, one-byte packing on every platform).
/// @details Returned by value from GetAnalogActionData. Every field sits at an
///          offset that is a multiple of its own alignment, so the by-value
///          return convention matches the SDK's declaration.
typedef struct rt_steam_input_analog_data {
    int32_t mode;   ///< EInputSourceMode of the bound input.
    float x;        ///< Horizontal value; deltas for mouse-like inputs.
    float y;        ///< Vertical value.
    uint8_t active; ///< C++ bool: the action is available in the active action set.
} rt_steam_input_analog_data;

#pragma pack(pop)

/// @brief Expected sizeof(rt_steam_packing_sentinel) under the platform pack.
#define RT_STEAM_PACKING_SENTINEL_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 32u : 24u)
/// @brief Expected sizeof(rt_steam_callback_msg) under the platform pack.
#define RT_STEAM_CALLBACK_MSG_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 20u)
/// @brief Expected sizeof(rt_steam_user_stats_stored) under the platform pack.
#define RT_STEAM_USER_STATS_STORED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_user_achievement_stored) under the platform pack.
#define RT_STEAM_USER_ACHIEVEMENT_STORED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 152u : 148u)
/// @brief Expected sizeof(rt_steam_leaderboard_find_result) under the platform pack.
#define RT_STEAM_LEADERBOARD_FIND_RESULT_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_leaderboard_scores_downloaded) under the platform pack.
#define RT_STEAM_LEADERBOARD_SCORES_DOWNLOADED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 20u)
/// @brief Expected sizeof(rt_steam_leaderboard_score_uploaded) under the platform pack.
#define RT_STEAM_LEADERBOARD_SCORE_UPLOADED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 32u : 28u)
/// @brief Expected sizeof(rt_steam_leaderboard_entry) under the platform pack.
#define RT_STEAM_LEADERBOARD_ENTRY_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 32u : 28u)
/// @brief Expected sizeof(rt_steam_user_achievement_icon_fetched) under either pack.
#define RT_STEAM_USER_ACHIEVEMENT_ICON_FETCHED_SIZE 144u
/// @brief Expected sizeof(rt_steam_global_achievement_percentages_ready) under the platform pack.
#define RT_STEAM_GLOBAL_ACHIEVEMENT_PERCENTAGES_READY_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_param_string_array) under the platform pack.
#define RT_STEAM_PARAM_STRING_ARRAY_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_ugc_details) under the platform pack.
#define RT_STEAM_UGC_DETAILS_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 9784u : 9772u)
/// @brief Expected sizeof(rt_steam_ugc_query_completed) under either pack.
#define RT_STEAM_UGC_QUERY_COMPLETED_SIZE 280u
/// @brief Expected sizeof(rt_steam_ugc_create_item_result) under the platform pack.
#define RT_STEAM_UGC_CREATE_ITEM_RESULT_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 16u)
/// @brief Expected sizeof(rt_steam_ugc_submit_item_update_result) under either pack.
#define RT_STEAM_UGC_SUBMIT_ITEM_UPDATE_RESULT_SIZE 16u
/// @brief Expected sizeof(rt_steam_ugc_item_installed) under the platform pack.
#define RT_STEAM_UGC_ITEM_INSTALLED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 32u : 28u)
/// @brief Expected sizeof(rt_steam_ugc_download_item_result) under the platform pack.
#define RT_STEAM_UGC_DOWNLOAD_ITEM_RESULT_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 16u)
/// @brief Expected sizeof(rt_steam_ugc_file_result) under the platform pack.
#define RT_STEAM_UGC_FILE_RESULT_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_ugc_file_subscription) under the platform pack.
#define RT_STEAM_UGC_FILE_SUBSCRIPTION_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)
/// @brief Expected sizeof(rt_steam_input_configuration_loaded) under the platform pack.
#define RT_STEAM_INPUT_CONFIGURATION_LOADED_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 40u : 32u)
/// @brief Expected sizeof(rt_steam_timeline_phase_recording_exists) under either pack.
#define RT_STEAM_TIMELINE_PHASE_RECORDING_EXISTS_SIZE 88u
/// @brief Expected sizeof(rt_steam_timeline_event_recording_exists) under the platform pack.
#define RT_STEAM_TIMELINE_EVENT_RECORDING_EXISTS_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 16u : 12u)

//===----------------------------------------------------------------------===//
// Flat API function types
//===----------------------------------------------------------------------===//

/// @brief SteamAPI_InitFlat(SteamErrMsg *) -> ESteamAPIInitResult.
typedef int (*rt_steam_init_flat_fn)(char *err_msg);
/// @brief SteamAPI_Shutdown().
typedef void (*rt_steam_void_fn)(void);
/// @brief SteamAPI_RestartAppIfNecessary(uint32 appId) -> bool.
typedef bool (*rt_steam_restart_app_fn)(uint32_t app_id);
/// @brief SteamAPI_IsSteamRunning() -> bool.
typedef bool (*rt_steam_bool_fn)(void);
/// @brief SteamAPI_GetHSteamPipe() -> HSteamPipe.
typedef rt_steam_pipe (*rt_steam_get_pipe_fn)(void);
/// @brief SteamAPI_ManualDispatch_RunFrame / FreeLastCallback (HSteamPipe).
typedef void (*rt_steam_pipe_fn)(rt_steam_pipe pipe);
/// @brief SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe, CallbackMsg_t *) -> bool.
typedef bool (*rt_steam_next_callback_fn)(rt_steam_pipe pipe, rt_steam_callback_msg *msg);
/// @brief SteamAPI_ManualDispatch_GetAPICallResult(pipe, call, buffer, size, expected id, failed*)
/// -> bool.
typedef bool (*rt_steam_call_result_fn)(rt_steam_pipe pipe,
                                        rt_steam_api_call call,
                                        void *buffer,
                                        int size,
                                        int expected_callback_id,
                                        bool *failed);
/// @brief Versioned interface accessor (SteamAPI_Steam<Interface>_vNNN) -> interface pointer.
typedef void *(*rt_steam_accessor_fn)(void);
/// @brief Interface method returning bool (for example ISteamUser::BLoggedOn).
typedef bool (*rt_steam_self_bool_fn)(void *self);
/// @brief Interface method returning uint64 (ISteamUser::GetSteamID).
typedef uint64_t (*rt_steam_self_u64_fn)(void *self);
/// @brief Interface method returning uint32 (ISteamUtils::GetAppID).
typedef uint32_t (*rt_steam_self_u32_fn)(void *self);
/// @brief Interface method returning a 32-bit enum (ISteamUtils::IsRunningOnSteamHardware).
typedef int (*rt_steam_self_enum_fn)(void *self);
/// @brief Interface method returning a borrowed C string (persona name, language).
typedef const char *(*rt_steam_self_cstr_fn)(void *self);
/// @brief Interface method taking an AppId_t and returning bool (ISteamApps::BIsDlcInstalled).
typedef bool (*rt_steam_self_app_bool_fn)(void *self, uint32_t app_id);
/// @brief Interface method taking one string and returning a borrowed C string
///        (ISteamApps::GetLaunchQueryParam).
typedef const char *(*rt_steam_self_str_cstr_fn)(void *self, const char *key);
/// @brief ISteamApps::GetLaunchCommandLine; returns the number of bytes copied into @p buffer.
typedef int (*rt_steam_launch_command_line_fn)(void *self, char *buffer, int capacity);
/// @brief Interface method returning int (ISteamApps::GetDLCCount, GetAppBuildId).
typedef int (*rt_steam_self_int_fn)(void *self);
/// @brief ISteamApps::BGetDLCDataByIndex.
typedef bool (*rt_steam_dlc_data_fn)(
    void *self, int index, uint32_t *app_id, bool *available, char *name, int name_capacity);
/// @brief ISteamApps::GetCurrentBetaName; returns true on a beta branch.
typedef bool (*rt_steam_beta_name_fn)(void *self, char *name, int name_capacity);
/// @brief Interface method returning a SteamAPICall_t (ISteamUserStats::GetNumberOfCurrentPlayers).
typedef rt_steam_api_call (*rt_steam_self_call_fn)(void *self);
/// @brief Interface method returning void (ISteamFriends::ClearRichPresence).
typedef void (*rt_steam_self_void_fn)(void *self);
/// @brief Interface method taking one string and returning bool (SetAchievement, FileExists).
typedef bool (*rt_steam_self_str_bool_fn)(void *self, const char *name);
/// @brief Interface method taking two strings and returning bool (ISteamFriends::SetRichPresence).
typedef bool (*rt_steam_self_str_str_bool_fn)(void *self, const char *key, const char *value);
/// @brief ISteamUserStats::GetAchievementAndUnlockTime.
typedef bool (*rt_steam_get_achievement_fn)(void *self,
                                            const char *name,
                                            bool *achieved,
                                            uint32_t *unlock_time);
/// @brief ISteamUserStats::IndicateAchievementProgress.
typedef bool (*rt_steam_indicate_progress_fn)(void *self,
                                              const char *name,
                                              uint32_t current,
                                              uint32_t maximum);
/// @brief ISteamUserStats::GetAchievementName.
typedef const char *(*rt_steam_achievement_name_fn)(void *self, uint32_t index);
/// @brief ISteamUserStats::GetAchievementDisplayAttribute.
typedef const char *(*rt_steam_achievement_attribute_fn)(void *self,
                                                         const char *name,
                                                         const char *key);
/// @brief ISteamUserStats::GetStatInt32.
typedef bool (*rt_steam_get_stat_int_fn)(void *self, const char *name, int32_t *value);
/// @brief ISteamUserStats::SetStatInt32.
typedef bool (*rt_steam_set_stat_int_fn)(void *self, const char *name, int32_t value);
/// @brief ISteamUserStats::GetStatFloat.
typedef bool (*rt_steam_get_stat_float_fn)(void *self, const char *name, float *value);
/// @brief ISteamUserStats::SetStatFloat.
typedef bool (*rt_steam_set_stat_float_fn)(void *self, const char *name, float value);
/// @brief ISteamUserStats::UpdateAvgRateStat.
typedef bool (*rt_steam_update_avg_rate_fn)(void *self,
                                            const char *name,
                                            float count_this_session,
                                            double session_length);
/// @brief ISteamUserStats::ResetAllStats.
typedef bool (*rt_steam_reset_all_stats_fn)(void *self, bool achievements_too);
/// @brief ISteamUserStats::FindLeaderboard.
typedef rt_steam_api_call (*rt_steam_find_leaderboard_fn)(void *self, const char *name);
/// @brief ISteamUserStats::FindOrCreateLeaderboard.
typedef rt_steam_api_call (*rt_steam_find_or_create_leaderboard_fn)(void *self,
                                                                    const char *name,
                                                                    int sort_method,
                                                                    int display_type);
/// @brief ISteamUserStats::GetLeaderboardName.
typedef const char *(*rt_steam_leaderboard_name_fn)(void *self, uint64_t leaderboard);
/// @brief ISteamUserStats::GetLeaderboardEntryCount.
typedef int (*rt_steam_leaderboard_entry_count_fn)(void *self, uint64_t leaderboard);
/// @brief ISteamUserStats::DownloadLeaderboardEntries.
typedef rt_steam_api_call (*rt_steam_download_entries_fn)(
    void *self, uint64_t leaderboard, int data_request, int range_start, int range_end);
/// @brief ISteamUserStats::GetDownloadedLeaderboardEntry.
typedef bool (*rt_steam_downloaded_entry_fn)(void *self,
                                             uint64_t entries,
                                             int index,
                                             rt_steam_leaderboard_entry *entry,
                                             int32_t *details,
                                             int details_max);
/// @brief ISteamUserStats::UploadLeaderboardScore.
typedef rt_steam_api_call (*rt_steam_upload_score_fn)(void *self,
                                                      uint64_t leaderboard,
                                                      int upload_method,
                                                      int32_t score,
                                                      const int32_t *details,
                                                      int detail_count);
/// @brief ISteamFriends::ActivateGameOverlay.
typedef void (*rt_steam_activate_overlay_fn)(void *self, const char *dialog);
/// @brief ISteamFriends::ActivateGameOverlayToWebPage.
typedef void (*rt_steam_activate_web_page_fn)(void *self, const char *url, int mode);
/// @brief ISteamFriends::ActivateGameOverlayToStore.
typedef void (*rt_steam_activate_store_fn)(void *self, uint32_t app_id, int flag);
/// @brief ISteamFriends::GetFriendPersonaName.
typedef const char *(*rt_steam_friend_persona_name_fn)(void *self, uint64_t steam_id);
/// @brief ISteamFriends::RequestUserInformation.
typedef bool (*rt_steam_request_user_information_fn)(void *self,
                                                     uint64_t steam_id,
                                                     bool require_name_only);
/// @brief ISteamUtils::SetOverlayNotificationPosition.
typedef void (*rt_steam_notification_position_fn)(void *self, int position);
/// @brief ISteamUtils::SetOverlayNotificationInset.
typedef void (*rt_steam_notification_inset_fn)(void *self, int horizontal, int vertical);
/// @brief ISteamUtils::ShowFloatingGamepadTextInput.
typedef bool (*rt_steam_show_floating_input_fn)(
    void *self, int keyboard_mode, int x, int y, int width, int height);
/// @brief ISteamUtils::ShowGamepadTextInput.
typedef bool (*rt_steam_show_gamepad_input_fn)(void *self,
                                               int input_mode,
                                               int line_mode,
                                               const char *description,
                                               uint32_t char_max,
                                               const char *existing_text);
/// @brief ISteamUtils::GetEnteredGamepadTextInput.
typedef bool (*rt_steam_entered_gamepad_text_fn)(void *self, char *text, uint32_t capacity);
/// @brief ISteamRemoteStorage::FileWrite.
typedef bool (*rt_steam_file_write_fn)(void *self,
                                       const char *name,
                                       const void *data,
                                       int32_t size);
/// @brief ISteamRemoteStorage::FileRead.
typedef int32_t (*rt_steam_file_read_fn)(void *self, const char *name, void *data, int32_t size);
/// @brief ISteamRemoteStorage::GetFileSize.
typedef int32_t (*rt_steam_file_size_fn)(void *self, const char *name);
/// @brief ISteamRemoteStorage::GetFileTimestamp.
typedef int64_t (*rt_steam_file_timestamp_fn)(void *self, const char *name);
/// @brief ISteamRemoteStorage::GetFileCount.
typedef int32_t (*rt_steam_file_count_fn)(void *self);
/// @brief ISteamRemoteStorage::GetFileNameAndSize.
typedef const char *(*rt_steam_file_name_and_size_fn)(void *self, int index, int32_t *size);
/// @brief ISteamRemoteStorage::GetQuota.
typedef bool (*rt_steam_quota_fn)(void *self, uint64_t *total_bytes, uint64_t *available_bytes);
/// @brief Interface method taking one string and returning int
/// (ISteamUserStats::GetAchievementIcon).
typedef int (*rt_steam_self_str_int_fn)(void *self, const char *name);
/// @brief ISteamUtils::GetImageSize.
typedef bool (*rt_steam_image_size_fn)(void *self, int image, uint32_t *width, uint32_t *height);
/// @brief ISteamUtils::GetImageRGBA.
typedef bool (*rt_steam_image_rgba_fn)(void *self, int image, uint8_t *dest, int dest_size);
/// @brief ISteamUserStats::GetAchievementAchievedPercent.
typedef bool (*rt_steam_achieved_percent_fn)(void *self, const char *name, float *percent);
/// @brief Interface method taking one string and returning void (SetGamePhaseID,
/// OpenOverlayToGamePhase).
typedef void (*rt_steam_self_str_void_fn)(void *self, const char *text);
/// @brief Interface method taking one uint64 and returning void (RemoveTimelineEvent).
typedef void (*rt_steam_self_u64_void_fn)(void *self, uint64_t value);
/// @brief Interface method taking one uint64 and returning a SteamAPICall_t
/// (DoesEventRecordingExist).
typedef rt_steam_api_call (*rt_steam_self_u64_call_fn)(void *self, uint64_t value);
/// @brief Interface method taking one string and returning a SteamAPICall_t
///        (DoesGamePhaseRecordingExist).
typedef rt_steam_api_call (*rt_steam_self_str_call_fn)(void *self, const char *text);
/// @brief ISteamTimeline::SetTimelineTooltip.
typedef void (*rt_steam_timeline_tooltip_fn)(void *self, const char *description, float time_delta);
/// @brief ISteamTimeline::ClearTimelineTooltip.
typedef void (*rt_steam_timeline_clear_tooltip_fn)(void *self, float time_delta);
/// @brief ISteamTimeline::SetTimelineGameMode (ETimelineGameMode).
typedef void (*rt_steam_timeline_game_mode_fn)(void *self, int mode);
/// @brief ISteamTimeline::AddInstantaneousTimelineEvent.
typedef uint64_t (*rt_steam_timeline_add_instant_fn)(void *self,
                                                     const char *title,
                                                     const char *description,
                                                     const char *icon,
                                                     uint32_t priority,
                                                     float start_offset_seconds,
                                                     int possible_clip);
/// @brief ISteamTimeline::AddRangeTimelineEvent.
typedef uint64_t (*rt_steam_timeline_add_range_fn)(void *self,
                                                   const char *title,
                                                   const char *description,
                                                   const char *icon,
                                                   uint32_t priority,
                                                   float start_offset_seconds,
                                                   float duration_seconds,
                                                   int possible_clip);
/// @brief ISteamTimeline::StartRangeTimelineEvent.
typedef uint64_t (*rt_steam_timeline_start_range_fn)(void *self,
                                                     const char *title,
                                                     const char *description,
                                                     const char *icon,
                                                     uint32_t priority,
                                                     float start_offset_seconds,
                                                     int possible_clip);
/// @brief ISteamTimeline::UpdateRangeTimelineEvent.
typedef void (*rt_steam_timeline_update_range_fn)(void *self,
                                                  uint64_t event,
                                                  const char *title,
                                                  const char *description,
                                                  const char *icon,
                                                  uint32_t priority,
                                                  int possible_clip);
/// @brief ISteamTimeline::EndRangeTimelineEvent.
typedef void (*rt_steam_timeline_end_range_fn)(void *self,
                                               uint64_t event,
                                               float end_offset_seconds);
/// @brief ISteamTimeline::AddGamePhaseTag.
typedef void (*rt_steam_timeline_phase_tag_fn)(void *self,
                                               const char *tag_name,
                                               const char *tag_icon,
                                               const char *tag_group,
                                               uint32_t priority);
/// @brief ISteamTimeline::SetGamePhaseAttribute.
typedef void (*rt_steam_timeline_phase_attribute_fn)(void *self,
                                                     const char *attribute_group,
                                                     const char *attribute_value,
                                                     uint32_t priority);
/// @brief ISteamInput::Init.
typedef bool (*rt_steam_input_init_fn)(void *self, bool explicitly_call_run_frame);
/// @brief ISteamInput::RunFrame.
typedef void (*rt_steam_input_run_frame_fn)(void *self, bool reserved);
/// @brief ISteamInput::GetConnectedControllers; writes up to RT_STEAM_INPUT_MAX_COUNT handles.
typedef int (*rt_steam_input_controllers_fn)(void *self, uint64_t *handles);
/// @brief Interface method taking one string and returning a uint64 handle
///        (GetActionSetHandle, GetDigitalActionHandle, GetAnalogActionHandle).
typedef uint64_t (*rt_steam_self_str_u64_fn)(void *self, const char *name);
/// @brief Interface method taking two uint64 values and returning void
///        (ActivateActionSet, ActivateActionSetLayer, DeactivateActionSetLayer).
typedef void (*rt_steam_self_u64_u64_void_fn)(void *self, uint64_t first, uint64_t second);
/// @brief Interface method taking one uint64 and returning a uint64 (GetCurrentActionSet).
typedef uint64_t (*rt_steam_self_u64_u64_fn)(void *self, uint64_t value);
/// @brief ISteamInput::GetDigitalActionData.
typedef rt_steam_input_digital_data (*rt_steam_input_digital_data_fn)(void *self,
                                                                      uint64_t controller,
                                                                      uint64_t action);
/// @brief ISteamInput::GetAnalogActionData.
typedef rt_steam_input_analog_data (*rt_steam_input_analog_data_fn)(void *self,
                                                                    uint64_t controller,
                                                                    uint64_t action);
/// @brief ISteamInput::GetDigitalActionOrigins and GetAnalogActionOrigins; writes up to
///        RT_STEAM_INPUT_MAX_ORIGINS EInputActionOrigin values.
typedef int (*rt_steam_input_origins_fn)(
    void *self, uint64_t controller, uint64_t action_set, uint64_t action, int32_t *origins);
/// @brief Interface method taking one uint64 and returning a borrowed C string
///        (GetStringForDigitalActionName, GetStringForAnalogActionName).
typedef const char *(*rt_steam_self_u64_cstr_fn)(void *self, uint64_t value);
/// @brief ISteamInput::GetGlyphPNGForActionOrigin.
typedef const char *(*rt_steam_input_glyph_png_fn)(void *self,
                                                   int origin,
                                                   int size,
                                                   uint32_t flags);
/// @brief Interface method taking an int and returning a borrowed C string
///        (GetStringForActionOrigin).
typedef const char *(*rt_steam_self_int_cstr_fn)(void *self, int value);
/// @brief ISteamInput::TriggerVibration.
typedef void (*rt_steam_input_vibration_fn)(void *self,
                                            uint64_t controller,
                                            uint16_t left_speed,
                                            uint16_t right_speed);
/// @brief ISteamInput::SetLEDColor.
typedef void (*rt_steam_input_led_fn)(
    void *self, uint64_t controller, uint8_t red, uint8_t green, uint8_t blue, unsigned flags);
/// @brief Interface method taking one uint64 and returning bool (ShowBindingPanel).
typedef bool (*rt_steam_self_u64_bool_fn)(void *self, uint64_t value);
/// @brief Interface method taking one uint64 and returning int
///        (GetInputTypeForHandle, GetGamepadIndexForController).
typedef int (*rt_steam_self_u64_int_fn)(void *self, uint64_t value);
/// @brief ISteamUGC::CreateQueryUserUGCRequest.
typedef uint64_t (*rt_steam_ugc_query_user_fn)(void *self,
                                               uint32_t account_id,
                                               int list,
                                               int matching_type,
                                               int sort_order,
                                               uint32_t creator_app,
                                               uint32_t consumer_app,
                                               uint32_t page);
/// @brief ISteamUGC::CreateQueryAllUGCRequestPage.
typedef uint64_t (*rt_steam_ugc_query_all_fn)(void *self,
                                              int query_type,
                                              int matching_type,
                                              uint32_t creator_app,
                                              uint32_t consumer_app,
                                              uint32_t page);
/// @brief ISteamUGC::CreateQueryUGCDetailsRequest.
typedef uint64_t (*rt_steam_ugc_query_details_fn)(void *self, uint64_t *file_ids, uint32_t count);
/// @brief ISteamUGC::GetQueryUGCResult.
typedef bool (*rt_steam_ugc_query_result_fn)(void *self,
                                             uint64_t handle,
                                             uint32_t index,
                                             rt_steam_ugc_details *details);
/// @brief ISteamUGC::GetQueryUGCPreviewURL and GetQueryUGCMetadata.
typedef bool (*rt_steam_ugc_query_text_fn)(
    void *self, uint64_t handle, uint32_t index, char *text, uint32_t capacity);
/// @brief Interface method taking a uint64 and a string and returning bool
///        (AddRequiredTag, SetSearchText, SetItemTitle, SetItemContent, ...).
typedef bool (*rt_steam_self_u64_str_bool_fn)(void *self, uint64_t handle, const char *text);
/// @brief Interface method taking a uint64 and a bool and returning bool
///        (SetReturnLongDescription, SetReturnMetadata).
typedef bool (*rt_steam_self_u64_flag_bool_fn)(void *self, uint64_t handle, bool flag);
/// @brief ISteamUGC::CreateItem.
typedef rt_steam_api_call (*rt_steam_ugc_create_item_fn)(void *self,
                                                         uint32_t consumer_app,
                                                         int file_type);
/// @brief ISteamUGC::StartItemUpdate.
typedef uint64_t (*rt_steam_ugc_start_update_fn)(void *self,
                                                 uint32_t consumer_app,
                                                 uint64_t file_id);
/// @brief ISteamUGC::SetItemVisibility.
typedef bool (*rt_steam_ugc_set_visibility_fn)(void *self, uint64_t handle, int visibility);
/// @brief ISteamUGC::SetItemTags.
typedef bool (*rt_steam_ugc_set_tags_fn)(void *self,
                                         uint64_t handle,
                                         const rt_steam_param_string_array *tags,
                                         bool allow_admin_tags);
/// @brief ISteamUGC::SubmitItemUpdate.
typedef rt_steam_api_call (*rt_steam_ugc_submit_update_fn)(void *self,
                                                           uint64_t handle,
                                                           const char *change_note);
/// @brief ISteamUGC::GetItemUpdateProgress; returns EItemUpdateStatus.
typedef int (*rt_steam_ugc_update_progress_fn)(void *self,
                                               uint64_t handle,
                                               uint64_t *bytes_processed,
                                               uint64_t *bytes_total);
/// @brief ISteamUGC::GetNumSubscribedItems as declared through SDK 1.61 (SteamUGC_v020).
typedef uint32_t (*rt_steam_ugc_subscribed_count_v020_fn)(void *self);
/// @brief ISteamUGC::GetNumSubscribedItems from SDK 1.62 (SteamUGC_v021).
typedef uint32_t (*rt_steam_ugc_subscribed_count_v021_fn)(void *self,
                                                          bool include_locally_disabled);
/// @brief ISteamUGC::GetSubscribedItems as declared through SDK 1.61 (SteamUGC_v020).
typedef uint32_t (*rt_steam_ugc_subscribed_items_v020_fn)(void *self,
                                                          uint64_t *file_ids,
                                                          uint32_t max_entries);
/// @brief ISteamUGC::GetSubscribedItems from SDK 1.62 (SteamUGC_v021).
typedef uint32_t (*rt_steam_ugc_subscribed_items_v021_fn)(void *self,
                                                          uint64_t *file_ids,
                                                          uint32_t max_entries,
                                                          bool include_locally_disabled);
/// @brief ISteamUGC::GetItemState; returns EItemState flags.
typedef uint32_t (*rt_steam_self_u64_u32_fn)(void *self, uint64_t value);
/// @brief ISteamUGC::GetItemInstallInfo.
typedef bool (*rt_steam_ugc_install_info_fn)(void *self,
                                             uint64_t file_id,
                                             uint64_t *size_on_disk,
                                             char *folder,
                                             uint32_t folder_capacity,
                                             uint32_t *timestamp);
/// @brief ISteamUGC::GetItemDownloadInfo.
typedef bool (*rt_steam_ugc_download_info_fn)(void *self,
                                              uint64_t file_id,
                                              uint64_t *bytes_downloaded,
                                              uint64_t *bytes_total);

//===----------------------------------------------------------------------===//
// Exported symbol names
//===----------------------------------------------------------------------===//

#define RT_STEAM_SYMBOL_INIT_FLAT "SteamAPI_InitFlat"
#define RT_STEAM_SYMBOL_SHUTDOWN "SteamAPI_Shutdown"
#define RT_STEAM_SYMBOL_RESTART_APP "SteamAPI_RestartAppIfNecessary"
#define RT_STEAM_SYMBOL_IS_STEAM_RUNNING "SteamAPI_IsSteamRunning"
#define RT_STEAM_SYMBOL_GET_PIPE "SteamAPI_GetHSteamPipe"
#define RT_STEAM_SYMBOL_DISPATCH_INIT "SteamAPI_ManualDispatch_Init"
#define RT_STEAM_SYMBOL_DISPATCH_RUN_FRAME "SteamAPI_ManualDispatch_RunFrame"
#define RT_STEAM_SYMBOL_DISPATCH_NEXT "SteamAPI_ManualDispatch_GetNextCallback"
#define RT_STEAM_SYMBOL_DISPATCH_FREE "SteamAPI_ManualDispatch_FreeLastCallback"
#define RT_STEAM_SYMBOL_DISPATCH_CALL_RESULT "SteamAPI_ManualDispatch_GetAPICallResult"

#define RT_STEAM_SYMBOL_USER_V023 "SteamAPI_SteamUser_v023"
#define RT_STEAM_SYMBOL_USER_LOGGED_ON "SteamAPI_ISteamUser_BLoggedOn"
#define RT_STEAM_SYMBOL_USER_GET_STEAM_ID "SteamAPI_ISteamUser_GetSteamID"

#define RT_STEAM_SYMBOL_FRIENDS_V018 "SteamAPI_SteamFriends_v018"
#define RT_STEAM_SYMBOL_FRIENDS_V017 "SteamAPI_SteamFriends_v017"
#define RT_STEAM_SYMBOL_FRIENDS_PERSONA_NAME "SteamAPI_ISteamFriends_GetPersonaName"

#define RT_STEAM_SYMBOL_UTILS_V011 "SteamAPI_SteamUtils_v011"
#define RT_STEAM_SYMBOL_UTILS_V010 "SteamAPI_SteamUtils_v010"
#define RT_STEAM_SYMBOL_UTILS_GET_APP_ID "SteamAPI_ISteamUtils_GetAppID"
#define RT_STEAM_SYMBOL_UTILS_BIG_PICTURE "SteamAPI_ISteamUtils_IsSteamInBigPictureMode"
#define RT_STEAM_SYMBOL_UTILS_STEAM_HARDWARE "SteamAPI_ISteamUtils_IsRunningOnSteamHardware"
#define RT_STEAM_SYMBOL_UTILS_UNDER_PROTON "SteamAPI_ISteamUtils_IsRunningUnderProton"
#define RT_STEAM_SYMBOL_UTILS_ON_STEAM_DECK "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck"

#define RT_STEAM_SYMBOL_APPS_V009 "SteamAPI_SteamApps_v009"
#define RT_STEAM_SYMBOL_APPS_V008 "SteamAPI_SteamApps_v008"
#define RT_STEAM_SYMBOL_APPS_IS_SUBSCRIBED "SteamAPI_ISteamApps_BIsSubscribed"
#define RT_STEAM_SYMBOL_APPS_IS_DLC_INSTALLED "SteamAPI_ISteamApps_BIsDlcInstalled"
#define RT_STEAM_SYMBOL_APPS_GAME_LANGUAGE "SteamAPI_ISteamApps_GetCurrentGameLanguage"
#define RT_STEAM_SYMBOL_APPS_LAUNCH_QUERY_PARAM "SteamAPI_ISteamApps_GetLaunchQueryParam"
#define RT_STEAM_SYMBOL_APPS_LAUNCH_COMMAND_LINE "SteamAPI_ISteamApps_GetLaunchCommandLine"
#define RT_STEAM_SYMBOL_APPS_DLC_COUNT "SteamAPI_ISteamApps_GetDLCCount"
#define RT_STEAM_SYMBOL_APPS_DLC_DATA "SteamAPI_ISteamApps_BGetDLCDataByIndex"
#define RT_STEAM_SYMBOL_APPS_BUILD_ID "SteamAPI_ISteamApps_GetAppBuildId"
#define RT_STEAM_SYMBOL_APPS_BETA_NAME "SteamAPI_ISteamApps_GetCurrentBetaName"

#define RT_STEAM_SYMBOL_FRIENDS_SET_RICH_PRESENCE "SteamAPI_ISteamFriends_SetRichPresence"
#define RT_STEAM_SYMBOL_FRIENDS_CLEAR_RICH_PRESENCE "SteamAPI_ISteamFriends_ClearRichPresence"
#define RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_OVERLAY "SteamAPI_ISteamFriends_ActivateGameOverlay"
#define RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_WEB_PAGE                                                  \
    "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage"
#define RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_STORE "SteamAPI_ISteamFriends_ActivateGameOverlayToStore"
#define RT_STEAM_SYMBOL_FRIENDS_FRIEND_PERSONA_NAME "SteamAPI_ISteamFriends_GetFriendPersonaName"
#define RT_STEAM_SYMBOL_FRIENDS_REQUEST_USER_INFO "SteamAPI_ISteamFriends_RequestUserInformation"

#define RT_STEAM_SYMBOL_UTILS_OVERLAY_ENABLED "SteamAPI_ISteamUtils_IsOverlayEnabled"
#define RT_STEAM_SYMBOL_UTILS_NOTIFICATION_POSITION                                                \
    "SteamAPI_ISteamUtils_SetOverlayNotificationPosition"
#define RT_STEAM_SYMBOL_UTILS_NOTIFICATION_INSET "SteamAPI_ISteamUtils_SetOverlayNotificationInset"
#define RT_STEAM_SYMBOL_UTILS_SHOW_FLOATING_INPUT                                                  \
    "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput"
#define RT_STEAM_SYMBOL_UTILS_DISMISS_FLOATING_INPUT                                               \
    "SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput"
#define RT_STEAM_SYMBOL_UTILS_SHOW_GAMEPAD_INPUT "SteamAPI_ISteamUtils_ShowGamepadTextInput"
#define RT_STEAM_SYMBOL_UTILS_ENTERED_TEXT_LENGTH "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength"
#define RT_STEAM_SYMBOL_UTILS_ENTERED_TEXT "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput"

#define RT_STEAM_SYMBOL_USER_STATS_V013 "SteamAPI_SteamUserStats_v013"
#define RT_STEAM_SYMBOL_USER_STATS_PLAYER_COUNT "SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers"
#define RT_STEAM_SYMBOL_USER_STATS_SET_ACHIEVEMENT "SteamAPI_ISteamUserStats_SetAchievement"
#define RT_STEAM_SYMBOL_USER_STATS_CLEAR_ACHIEVEMENT "SteamAPI_ISteamUserStats_ClearAchievement"
#define RT_STEAM_SYMBOL_USER_STATS_GET_ACHIEVEMENT_TIME                                            \
    "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime"
#define RT_STEAM_SYMBOL_USER_STATS_INDICATE_PROGRESS                                               \
    "SteamAPI_ISteamUserStats_IndicateAchievementProgress"
#define RT_STEAM_SYMBOL_USER_STATS_NUM_ACHIEVEMENTS "SteamAPI_ISteamUserStats_GetNumAchievements"
#define RT_STEAM_SYMBOL_USER_STATS_ACHIEVEMENT_NAME "SteamAPI_ISteamUserStats_GetAchievementName"
#define RT_STEAM_SYMBOL_USER_STATS_ACHIEVEMENT_ATTRIBUTE                                           \
    "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute"
#define RT_STEAM_SYMBOL_USER_STATS_GET_STAT_INT "SteamAPI_ISteamUserStats_GetStatInt32"
#define RT_STEAM_SYMBOL_USER_STATS_SET_STAT_INT "SteamAPI_ISteamUserStats_SetStatInt32"
#define RT_STEAM_SYMBOL_USER_STATS_GET_STAT_FLOAT "SteamAPI_ISteamUserStats_GetStatFloat"
#define RT_STEAM_SYMBOL_USER_STATS_SET_STAT_FLOAT "SteamAPI_ISteamUserStats_SetStatFloat"
#define RT_STEAM_SYMBOL_USER_STATS_UPDATE_AVG_RATE "SteamAPI_ISteamUserStats_UpdateAvgRateStat"
#define RT_STEAM_SYMBOL_USER_STATS_STORE "SteamAPI_ISteamUserStats_StoreStats"
#define RT_STEAM_SYMBOL_USER_STATS_RESET_ALL "SteamAPI_ISteamUserStats_ResetAllStats"
#define RT_STEAM_SYMBOL_USER_STATS_FIND_LEADERBOARD "SteamAPI_ISteamUserStats_FindLeaderboard"
#define RT_STEAM_SYMBOL_USER_STATS_FIND_OR_CREATE_LEADERBOARD                                      \
    "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard"
#define RT_STEAM_SYMBOL_USER_STATS_LEADERBOARD_NAME "SteamAPI_ISteamUserStats_GetLeaderboardName"
#define RT_STEAM_SYMBOL_USER_STATS_LEADERBOARD_ENTRY_COUNT                                         \
    "SteamAPI_ISteamUserStats_GetLeaderboardEntryCount"
#define RT_STEAM_SYMBOL_USER_STATS_DOWNLOAD_ENTRIES                                                \
    "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries"
#define RT_STEAM_SYMBOL_USER_STATS_DOWNLOADED_ENTRY                                                \
    "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry"
#define RT_STEAM_SYMBOL_USER_STATS_UPLOAD_SCORE "SteamAPI_ISteamUserStats_UploadLeaderboardScore"
#define RT_STEAM_SYMBOL_USER_STATS_ACHIEVEMENT_ICON "SteamAPI_ISteamUserStats_GetAchievementIcon"
#define RT_STEAM_SYMBOL_USER_STATS_REQUEST_GLOBAL_PERCENTAGES                                      \
    "SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages"
#define RT_STEAM_SYMBOL_USER_STATS_ACHIEVED_PERCENT                                                \
    "SteamAPI_ISteamUserStats_GetAchievementAchievedPercent"
#define RT_STEAM_SYMBOL_UTILS_IMAGE_SIZE "SteamAPI_ISteamUtils_GetImageSize"
#define RT_STEAM_SYMBOL_UTILS_IMAGE_RGBA "SteamAPI_ISteamUtils_GetImageRGBA"

#define RT_STEAM_SYMBOL_REMOTE_STORAGE_V016 "SteamAPI_SteamRemoteStorage_v016"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_WRITE "SteamAPI_ISteamRemoteStorage_FileWrite"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_READ "SteamAPI_ISteamRemoteStorage_FileRead"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_EXISTS "SteamAPI_ISteamRemoteStorage_FileExists"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_DELETE "SteamAPI_ISteamRemoteStorage_FileDelete"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_SIZE "SteamAPI_ISteamRemoteStorage_GetFileSize"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_TIMESTAMP                                              \
    "SteamAPI_ISteamRemoteStorage_GetFileTimestamp"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_COUNT "SteamAPI_ISteamRemoteStorage_GetFileCount"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_NAME_AND_SIZE                                          \
    "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_QUOTA "SteamAPI_ISteamRemoteStorage_GetQuota"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_ENABLED_FOR_ACCOUNT                                         \
    "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_ENABLED_FOR_APP                                             \
    "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_BEGIN_BATCH                                                 \
    "SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch"
#define RT_STEAM_SYMBOL_REMOTE_STORAGE_END_BATCH "SteamAPI_ISteamRemoteStorage_EndFileWriteBatch"

#define RT_STEAM_SYMBOL_TIMELINE_V004 "SteamAPI_SteamTimeline_v004"
#define RT_STEAM_SYMBOL_TIMELINE_SET_TOOLTIP "SteamAPI_ISteamTimeline_SetTimelineTooltip"
#define RT_STEAM_SYMBOL_TIMELINE_CLEAR_TOOLTIP "SteamAPI_ISteamTimeline_ClearTimelineTooltip"
#define RT_STEAM_SYMBOL_TIMELINE_SET_GAME_MODE "SteamAPI_ISteamTimeline_SetTimelineGameMode"
#define RT_STEAM_SYMBOL_TIMELINE_ADD_INSTANT "SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_ADD_RANGE "SteamAPI_ISteamTimeline_AddRangeTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_START_RANGE "SteamAPI_ISteamTimeline_StartRangeTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_UPDATE_RANGE "SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_END_RANGE "SteamAPI_ISteamTimeline_EndRangeTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_REMOVE_EVENT "SteamAPI_ISteamTimeline_RemoveTimelineEvent"
#define RT_STEAM_SYMBOL_TIMELINE_EVENT_RECORDING "SteamAPI_ISteamTimeline_DoesEventRecordingExist"
#define RT_STEAM_SYMBOL_TIMELINE_START_PHASE "SteamAPI_ISteamTimeline_StartGamePhase"
#define RT_STEAM_SYMBOL_TIMELINE_END_PHASE "SteamAPI_ISteamTimeline_EndGamePhase"
#define RT_STEAM_SYMBOL_TIMELINE_SET_PHASE_ID "SteamAPI_ISteamTimeline_SetGamePhaseID"
#define RT_STEAM_SYMBOL_TIMELINE_PHASE_RECORDING                                                   \
    "SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist"
#define RT_STEAM_SYMBOL_TIMELINE_ADD_PHASE_TAG "SteamAPI_ISteamTimeline_AddGamePhaseTag"
#define RT_STEAM_SYMBOL_TIMELINE_SET_PHASE_ATTRIBUTE "SteamAPI_ISteamTimeline_SetGamePhaseAttribute"
#define RT_STEAM_SYMBOL_TIMELINE_OVERLAY_TO_PHASE "SteamAPI_ISteamTimeline_OpenOverlayToGamePhase"
#define RT_STEAM_SYMBOL_TIMELINE_OVERLAY_TO_EVENT                                                  \
    "SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent"

#define RT_STEAM_SYMBOL_UGC_V021 "SteamAPI_SteamUGC_v021"
#define RT_STEAM_SYMBOL_UGC_V020 "SteamAPI_SteamUGC_v020"
#define RT_STEAM_SYMBOL_UGC_QUERY_USER "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest"
#define RT_STEAM_SYMBOL_UGC_QUERY_ALL "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage"
#define RT_STEAM_SYMBOL_UGC_QUERY_DETAILS "SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest"
#define RT_STEAM_SYMBOL_UGC_SEND_QUERY "SteamAPI_ISteamUGC_SendQueryUGCRequest"
#define RT_STEAM_SYMBOL_UGC_QUERY_RESULT "SteamAPI_ISteamUGC_GetQueryUGCResult"
#define RT_STEAM_SYMBOL_UGC_QUERY_PREVIEW_URL "SteamAPI_ISteamUGC_GetQueryUGCPreviewURL"
#define RT_STEAM_SYMBOL_UGC_QUERY_METADATA "SteamAPI_ISteamUGC_GetQueryUGCMetadata"
#define RT_STEAM_SYMBOL_UGC_RELEASE_QUERY "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest"
#define RT_STEAM_SYMBOL_UGC_ADD_REQUIRED_TAG "SteamAPI_ISteamUGC_AddRequiredTag"
#define RT_STEAM_SYMBOL_UGC_SET_SEARCH_TEXT "SteamAPI_ISteamUGC_SetSearchText"
#define RT_STEAM_SYMBOL_UGC_RETURN_LONG_DESCRIPTION "SteamAPI_ISteamUGC_SetReturnLongDescription"
#define RT_STEAM_SYMBOL_UGC_RETURN_METADATA "SteamAPI_ISteamUGC_SetReturnMetadata"
#define RT_STEAM_SYMBOL_UGC_CREATE_ITEM "SteamAPI_ISteamUGC_CreateItem"
#define RT_STEAM_SYMBOL_UGC_START_UPDATE "SteamAPI_ISteamUGC_StartItemUpdate"
#define RT_STEAM_SYMBOL_UGC_SET_TITLE "SteamAPI_ISteamUGC_SetItemTitle"
#define RT_STEAM_SYMBOL_UGC_SET_DESCRIPTION "SteamAPI_ISteamUGC_SetItemDescription"
#define RT_STEAM_SYMBOL_UGC_SET_METADATA "SteamAPI_ISteamUGC_SetItemMetadata"
#define RT_STEAM_SYMBOL_UGC_SET_VISIBILITY "SteamAPI_ISteamUGC_SetItemVisibility"
#define RT_STEAM_SYMBOL_UGC_SET_TAGS "SteamAPI_ISteamUGC_SetItemTags"
#define RT_STEAM_SYMBOL_UGC_SET_CONTENT "SteamAPI_ISteamUGC_SetItemContent"
#define RT_STEAM_SYMBOL_UGC_SET_PREVIEW "SteamAPI_ISteamUGC_SetItemPreview"
#define RT_STEAM_SYMBOL_UGC_SUBMIT_UPDATE "SteamAPI_ISteamUGC_SubmitItemUpdate"
#define RT_STEAM_SYMBOL_UGC_UPDATE_PROGRESS "SteamAPI_ISteamUGC_GetItemUpdateProgress"
#define RT_STEAM_SYMBOL_UGC_SUBSCRIBE "SteamAPI_ISteamUGC_SubscribeItem"
#define RT_STEAM_SYMBOL_UGC_UNSUBSCRIBE "SteamAPI_ISteamUGC_UnsubscribeItem"
#define RT_STEAM_SYMBOL_UGC_SUBSCRIBED_COUNT "SteamAPI_ISteamUGC_GetNumSubscribedItems"
#define RT_STEAM_SYMBOL_UGC_SUBSCRIBED_ITEMS "SteamAPI_ISteamUGC_GetSubscribedItems"
#define RT_STEAM_SYMBOL_UGC_ITEM_STATE "SteamAPI_ISteamUGC_GetItemState"
#define RT_STEAM_SYMBOL_UGC_INSTALL_INFO "SteamAPI_ISteamUGC_GetItemInstallInfo"
#define RT_STEAM_SYMBOL_UGC_DOWNLOAD_INFO "SteamAPI_ISteamUGC_GetItemDownloadInfo"
#define RT_STEAM_SYMBOL_UGC_DOWNLOAD "SteamAPI_ISteamUGC_DownloadItem"
#define RT_STEAM_SYMBOL_UGC_DELETE "SteamAPI_ISteamUGC_DeleteItem"

#define RT_STEAM_SYMBOL_INPUT_V007 "SteamAPI_SteamInput_v007"
#define RT_STEAM_SYMBOL_INPUT_V006 "SteamAPI_SteamInput_v006"
#define RT_STEAM_SYMBOL_INPUT_INIT "SteamAPI_ISteamInput_Init"
#define RT_STEAM_SYMBOL_INPUT_SHUTDOWN "SteamAPI_ISteamInput_Shutdown"
#define RT_STEAM_SYMBOL_INPUT_SET_MANIFEST "SteamAPI_ISteamInput_SetInputActionManifestFilePath"
#define RT_STEAM_SYMBOL_INPUT_RUN_FRAME "SteamAPI_ISteamInput_RunFrame"
#define RT_STEAM_SYMBOL_INPUT_CONNECTED "SteamAPI_ISteamInput_GetConnectedControllers"
#define RT_STEAM_SYMBOL_INPUT_DEVICE_CALLBACKS "SteamAPI_ISteamInput_EnableDeviceCallbacks"
#define RT_STEAM_SYMBOL_INPUT_ACTION_SET_HANDLE "SteamAPI_ISteamInput_GetActionSetHandle"
#define RT_STEAM_SYMBOL_INPUT_ACTIVATE_SET "SteamAPI_ISteamInput_ActivateActionSet"
#define RT_STEAM_SYMBOL_INPUT_CURRENT_SET "SteamAPI_ISteamInput_GetCurrentActionSet"
#define RT_STEAM_SYMBOL_INPUT_ACTIVATE_LAYER "SteamAPI_ISteamInput_ActivateActionSetLayer"
#define RT_STEAM_SYMBOL_INPUT_DEACTIVATE_LAYER "SteamAPI_ISteamInput_DeactivateActionSetLayer"
#define RT_STEAM_SYMBOL_INPUT_DEACTIVATE_ALL_LAYERS                                                \
    "SteamAPI_ISteamInput_DeactivateAllActionSetLayers"
#define RT_STEAM_SYMBOL_INPUT_DIGITAL_HANDLE "SteamAPI_ISteamInput_GetDigitalActionHandle"
#define RT_STEAM_SYMBOL_INPUT_DIGITAL_DATA "SteamAPI_ISteamInput_GetDigitalActionData"
#define RT_STEAM_SYMBOL_INPUT_DIGITAL_ORIGINS "SteamAPI_ISteamInput_GetDigitalActionOrigins"
#define RT_STEAM_SYMBOL_INPUT_DIGITAL_NAME "SteamAPI_ISteamInput_GetStringForDigitalActionName"
#define RT_STEAM_SYMBOL_INPUT_ANALOG_HANDLE "SteamAPI_ISteamInput_GetAnalogActionHandle"
#define RT_STEAM_SYMBOL_INPUT_ANALOG_DATA "SteamAPI_ISteamInput_GetAnalogActionData"
#define RT_STEAM_SYMBOL_INPUT_ANALOG_ORIGINS "SteamAPI_ISteamInput_GetAnalogActionOrigins"
#define RT_STEAM_SYMBOL_INPUT_ANALOG_NAME "SteamAPI_ISteamInput_GetStringForAnalogActionName"
#define RT_STEAM_SYMBOL_INPUT_GLYPH_PNG "SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin"
#define RT_STEAM_SYMBOL_INPUT_ORIGIN_NAME "SteamAPI_ISteamInput_GetStringForActionOrigin"
#define RT_STEAM_SYMBOL_INPUT_VIBRATION "SteamAPI_ISteamInput_TriggerVibration"
#define RT_STEAM_SYMBOL_INPUT_LED_COLOR "SteamAPI_ISteamInput_SetLEDColor"
#define RT_STEAM_SYMBOL_INPUT_BINDING_PANEL "SteamAPI_ISteamInput_ShowBindingPanel"
#define RT_STEAM_SYMBOL_INPUT_TYPE "SteamAPI_ISteamInput_GetInputTypeForHandle"
#define RT_STEAM_SYMBOL_INPUT_GAMEPAD_INDEX "SteamAPI_ISteamInput_GetGamepadIndexForController"

#ifdef __cplusplus
}
#endif
