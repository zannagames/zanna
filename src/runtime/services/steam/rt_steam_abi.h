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
//        docs/adr/0352-platform-services-runtime-loaded-providers.md
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

/// @brief ESteamHardwareType values reported by ISteamUtils::IsRunningOnSteamHardware (1.65+).
#define RT_STEAM_HARDWARE_NONE 0
/// @brief ESteamHardwareType: Steam Deck.
#define RT_STEAM_HARDWARE_STEAM_DECK 1
/// @brief ESteamHardwareType: Steam Machine.
#define RT_STEAM_HARDWARE_STEAM_MACHINE 2
/// @brief ESteamHardwareType: Steam Frame.
#define RT_STEAM_HARDWARE_STEAM_FRAME 3

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
/// @brief DlcInstalled_t (ISteamApps base 1000 + 5).
#define RT_STEAM_CB_DLC_INSTALLED 1005
/// @brief NewUrlLaunchParameters_t (1000 + 14); empty payload.
#define RT_STEAM_CB_NEW_URL_LAUNCH_PARAMETERS 1014
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

#pragma pack(pop)

/// @brief Expected sizeof(rt_steam_packing_sentinel) under the platform pack.
#define RT_STEAM_PACKING_SENTINEL_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 32u : 24u)
/// @brief Expected sizeof(rt_steam_callback_msg) under the platform pack.
#define RT_STEAM_CALLBACK_MSG_SIZE (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 20u)

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

#define RT_STEAM_SYMBOL_USER_STATS_V013 "SteamAPI_SteamUserStats_v013"
#define RT_STEAM_SYMBOL_USER_STATS_PLAYER_COUNT "SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers"

#ifdef __cplusplus
}
#endif
