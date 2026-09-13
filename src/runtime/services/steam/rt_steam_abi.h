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
//     Windows and 4-byte packing on macOS and Linux. Sizes and offsets are
//     pinned by compile-time assertions in rt_steam_provider.c.
//   - Every method type listed here has an identical signature in all
//     supported redistributables (Steamworks SDK 1.61 through 1.65).
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

#ifdef __cplusplus
}
#endif
