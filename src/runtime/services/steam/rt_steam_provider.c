//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_provider.c
// Purpose: Steam provider for Zanna.Services. Loads the developer-shipped
//          steam_api redistributable at run time, binds the Steamworks flat C
//          API, pumps callbacks through manual dispatch, tracks the requests it
//          serves, and implements the Zanna.Services.Steam extension class.
// Key invariants:
//   - The redistributable is resolved from exactly one path: the
//     ZANNA_SERVICES_STEAM_LIBRARY override when set, otherwise the
//     executable directory. It is never unloaded.
//   - Core exports are resolved as one all-or-nothing table before any call.
//     Interfaces are resolved by exact accessor name after SteamAPI_InitFlat;
//     a missing interface or export disables only its features.
//   - Callback payloads are decoded only when their size equals the declared
//     layout size; layouts are pinned by the static assertions below.
//   - Every request owns one operation slot keyed by a provider token; Steam
//     call completions are matched to slots by SteamAPICall_t.
//   - Every entry point runs on the main thread; manual dispatch is never
//     entered concurrently.
// Ownership/Lifetime:
//   - Provider state is static for the process lifetime.
//   - Interface pointers are valid only between a successful start and stop.
//   - Strings returned to the core are new caller-owned references.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/steam/rt_steam_abi.h,
//        src/runtime/services/steam/rt_steam.h,
//        src/runtime/services/rt_services_provider.h,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_provider.c
 * @brief Implements the Steamworks-backed platform services provider core.
 * @details Start sequence: validate the app id, resolve the library path,
 *          export SteamAppId/SteamGameId when absent, load the library and its
 *          core exports, call SteamAPI_InitFlat, enable manual dispatch, read
 *          the client pipe, and open the versioned interfaces. Each pump runs
 *          one dispatch frame and translates up to 1024 callbacks into
 *          Zanna.Services events and request completions. Feature bindings
 *          live in rt_steam_user_stats.c, rt_steam_social.c, and
 *          rt_steam_cloud.c.
 */

#include "rt_steam.h"

#include "rt_args.h"
#include "rt_path.h"
#include "rt_platform.h"
#include "rt_service_hooks.h"
#include "rt_services.h"
#include "rt_services_dynlib.h"
#include "rt_services_provider.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Layout contract
//===----------------------------------------------------------------------===//

_Static_assert(sizeof(void *) == 8, "Steam provider layouts assume a 64-bit target");
_Static_assert(sizeof(bool) == 1, "flat API bool must be one byte");
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "flat API float and double sizes");
_Static_assert(sizeof(rt_steam_packing_sentinel) == RT_STEAM_PACKING_SENTINEL_SIZE,
               "Steam packing sentinel does not match the redistributable packing");
_Static_assert(sizeof(rt_steam_callback_msg) == RT_STEAM_CALLBACK_MSG_SIZE,
               "CallbackMsg_t size does not match the redistributable packing");
_Static_assert(offsetof(rt_steam_callback_msg, param) == 8, "CallbackMsg_t.m_pubParam offset");
_Static_assert(offsetof(rt_steam_callback_msg, param_size) == 16,
               "CallbackMsg_t.m_cubParam offset");
_Static_assert(sizeof(rt_steam_server_connect_failure) == 8, "SteamServerConnectFailure_t size");
_Static_assert(offsetof(rt_steam_server_connect_failure, still_retrying) == 4,
               "SteamServerConnectFailure_t.m_bStillRetrying offset");
_Static_assert(sizeof(rt_steam_servers_disconnected) == 4, "SteamServersDisconnected_t size");
_Static_assert(sizeof(rt_steam_game_overlay_activated) == 12, "GameOverlayActivated_t size");
_Static_assert(offsetof(rt_steam_game_overlay_activated, app_id) == 4,
               "GameOverlayActivated_t.m_nAppID offset");
_Static_assert(offsetof(rt_steam_game_overlay_activated, overlay_pid) == 8,
               "GameOverlayActivated_t.m_dwOverlayPID offset");
_Static_assert(sizeof(rt_steam_api_call_completed) == 16, "SteamAPICallCompleted_t size");
_Static_assert(offsetof(rt_steam_api_call_completed, callback_id) == 8,
               "SteamAPICallCompleted_t.m_iCallback offset");
_Static_assert(offsetof(rt_steam_api_call_completed, param_size) == 12,
               "SteamAPICallCompleted_t.m_cubParam offset");
_Static_assert(sizeof(rt_steam_dlc_installed) == 4, "DlcInstalled_t size");
_Static_assert(sizeof(rt_steam_number_of_current_players) == 8, "NumberOfCurrentPlayers_t size");
_Static_assert(offsetof(rt_steam_number_of_current_players, players) == 4,
               "NumberOfCurrentPlayers_t.m_cPlayers offset");
_Static_assert(sizeof(rt_steam_gamepad_text_input_dismissed) == 12,
               "GamepadTextInputDismissed_t size");
_Static_assert(offsetof(rt_steam_gamepad_text_input_dismissed, submitted_size) == 4,
               "GamepadTextInputDismissed_t.m_unSubmittedText offset");
_Static_assert(offsetof(rt_steam_gamepad_text_input_dismissed, app_id) == 8,
               "GamepadTextInputDismissed_t.m_unAppID offset");
_Static_assert(sizeof(rt_steam_user_stats_stored) == RT_STEAM_USER_STATS_STORED_SIZE,
               "UserStatsStored_t size");
_Static_assert(offsetof(rt_steam_user_stats_stored, result) == 8,
               "UserStatsStored_t.m_eResult offset");
_Static_assert(sizeof(rt_steam_user_achievement_stored) == RT_STEAM_USER_ACHIEVEMENT_STORED_SIZE,
               "UserAchievementStored_t size");
_Static_assert(offsetof(rt_steam_user_achievement_stored, achievement_name) == 9,
               "UserAchievementStored_t.m_rgchAchievementName offset");
_Static_assert(offsetof(rt_steam_user_achievement_stored, current_progress) == 140,
               "UserAchievementStored_t.m_nCurProgress offset");
_Static_assert(offsetof(rt_steam_user_achievement_stored, max_progress) == 144,
               "UserAchievementStored_t.m_nMaxProgress offset");
_Static_assert(sizeof(rt_steam_leaderboard_find_result) == RT_STEAM_LEADERBOARD_FIND_RESULT_SIZE,
               "LeaderboardFindResult_t size");
_Static_assert(offsetof(rt_steam_leaderboard_find_result, found) == 8,
               "LeaderboardFindResult_t.m_bLeaderboardFound offset");
_Static_assert(sizeof(rt_steam_leaderboard_scores_downloaded) ==
                   RT_STEAM_LEADERBOARD_SCORES_DOWNLOADED_SIZE,
               "LeaderboardScoresDownloaded_t size");
_Static_assert(offsetof(rt_steam_leaderboard_scores_downloaded, entries) == 8,
               "LeaderboardScoresDownloaded_t.m_hSteamLeaderboardEntries offset");
_Static_assert(offsetof(rt_steam_leaderboard_scores_downloaded, entry_count) == 16,
               "LeaderboardScoresDownloaded_t.m_cEntryCount offset");
_Static_assert(sizeof(rt_steam_leaderboard_score_uploaded) ==
                   RT_STEAM_LEADERBOARD_SCORE_UPLOADED_SIZE,
               "LeaderboardScoreUploaded_t size");
_Static_assert(offsetof(rt_steam_leaderboard_score_uploaded, leaderboard) ==
                   (RT_STEAM_CALLBACK_PACK == 8 ? 8u : 4u),
               "LeaderboardScoreUploaded_t.m_hSteamLeaderboard offset");
_Static_assert(offsetof(rt_steam_leaderboard_score_uploaded, score_changed) ==
                   (RT_STEAM_CALLBACK_PACK == 8 ? 20u : 16u),
               "LeaderboardScoreUploaded_t.m_bScoreChanged offset");
_Static_assert(offsetof(rt_steam_leaderboard_score_uploaded, global_rank_previous) ==
                   (RT_STEAM_CALLBACK_PACK == 8 ? 28u : 24u),
               "LeaderboardScoreUploaded_t.m_nGlobalRankPrevious offset");
_Static_assert(sizeof(rt_steam_leaderboard_entry) == RT_STEAM_LEADERBOARD_ENTRY_SIZE,
               "LeaderboardEntry_t size");
_Static_assert(offsetof(rt_steam_leaderboard_entry, detail_count) == 16,
               "LeaderboardEntry_t.m_cDetails offset");
_Static_assert(offsetof(rt_steam_leaderboard_entry, ugc) ==
                   (RT_STEAM_CALLBACK_PACK == 8 ? 24u : 20u),
               "LeaderboardEntry_t.m_hUGC offset");

//===----------------------------------------------------------------------===//
// State
//===----------------------------------------------------------------------===//

/// @brief Upper bound on callbacks drained by one pump, guarding against a misbehaving library.
#define STEAM_MAX_CALLBACKS_PER_PUMP 1024
/// @brief Environment variable naming an explicit steam_api library path.
#define STEAM_LIBRARY_OVERRIDE_ENV "ZANNA_SERVICES_STEAM_LIBRARY"
/// @brief Largest valid Steam app or DLC id.
#define STEAM_MAX_APP_ID UINT64_C(4294967295)

/// @brief Zero-initialized provider state shared with the feature sources.
steam_state rt_services_steam_state;

//===----------------------------------------------------------------------===//
// Host platform and ids
//===----------------------------------------------------------------------===//

/// @brief Name of the host operating system used in diagnostics.
/// @return "windows", "macos", "linux", or "unknown".
static const char *steam_host_os(void) {
#if RT_PLATFORM_WINDOWS
    return "windows";
#elif RT_PLATFORM_MACOS
    return "macos";
#elif RT_PLATFORM_LINUX
    return "linux";
#else
    return "unknown";
#endif
}

/// @brief Name of the host CPU architecture used in diagnostics.
/// @return "x64", "arm64", or "unknown".
static const char *steam_host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#else
    return "unknown";
#endif
}

/// @brief File name of the Steamworks redistributable for this host.
/// @return Redistributable file name, or NULL when Valve ships none for this host.
static const char *steam_library_file_name(void) {
    const char *arch = steam_host_arch();
    const int x64 = strcmp(arch, "x64") == 0;
    const int arm64 = strcmp(arch, "arm64") == 0;
#if RT_PLATFORM_WINDOWS
    (void)arm64;
    return x64 ? "steam_api64.dll" : NULL;
#elif RT_PLATFORM_MACOS
    return (x64 || arm64) ? "libsteam_api.dylib" : NULL;
#elif RT_PLATFORM_LINUX
    return (x64 || arm64) ? "libsteam_api.so" : NULL;
#else
    (void)x64;
    (void)arm64;
    return NULL;
#endif
}

/// @brief Parse a decimal Steam app or DLC id.
/// @param text NUL-terminated candidate; digits only, no sign or whitespace.
/// @param out Receives the id on success.
/// @return 1 when @p text is an integer in 1..4294967295, otherwise 0.
int rt_services_steam_parse_id(const char *text, uint32_t *out) {
    uint64_t value = 0;
    size_t digits = 0;
    if (!text || !*text)
        return 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        value = value * 10u + (uint64_t)(*p - '0');
        if (++digits > 20 || value > STEAM_MAX_APP_ID)
            return 0;
    }
    if (value == 0)
        return 0;
    *out = (uint32_t)value;
    return 1;
}

/// @brief Set SteamAppId and SteamGameId to @p app_id when they are absent.
/// @details Replaces the development-only steam_appid.txt file. Values already
///          present (a Steam launch) are never overwritten.
/// @param app_id Validated Steam app id.
static void steam_export_app_id(uint32_t app_id) {
    static const char *const names[] = {"SteamAppId", "SteamGameId"};
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", (unsigned)app_id);
    rt_string value = rt_const_cstr(digits);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        rt_string name = rt_const_cstr(names[i]);
        if (!rt_env_has_var(name))
            rt_env_set_var(name, value);
        rt_string_unref(name);
    }
    rt_string_unref(value);
}

//===----------------------------------------------------------------------===//
// Library loading
//===----------------------------------------------------------------------===//

/// @brief Resolve the one path the redistributable is loaded from.
/// @param out Receives the path.
/// @param capacity Size of @p out in bytes.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return RT_SERVICES_STATUS_OK, UNSUPPORTED_PLATFORM, or LIBRARY_NOT_FOUND.
static int64_t steam_resolve_library_path(char *out,
                                          size_t capacity,
                                          char *message,
                                          size_t message_capacity) {
    const char *file_name = steam_library_file_name();
    if (!file_name) {
        snprintf(message,
                 message_capacity,
                 "Steam: no Steamworks redistributable exists for %s-%s",
                 steam_host_os(),
                 steam_host_arch());
        return RT_SERVICES_STATUS_UNSUPPORTED_PLATFORM;
    }

    int written = -1;
    rt_string env_name = rt_const_cstr(STEAM_LIBRARY_OVERRIDE_ENV);
    rt_string env_value = rt_env_get_var(env_name);
    const char *override_path = env_value ? rt_string_cstr(env_value) : "";
    if (override_path[0]) {
        written = snprintf(out, capacity, "%s", override_path);
    } else {
        char *exe_dir = rt_path_exe_dir_cstr();
        if (exe_dir) {
            size_t len = strlen(exe_dir);
            const int has_separator =
                len > 0 && (exe_dir[len - 1] == '/' || exe_dir[len - 1] == '\\');
            written = snprintf(out,
                               capacity,
                               "%s%s%s",
                               exe_dir,
                               has_separator ? "" : RT_PATH_SEPARATOR_STR,
                               file_name);
            free(exe_dir);
        }
    }
    rt_string_unref(env_value);
    rt_string_unref(env_name);

    if (written < 0) {
        snprintf(message,
                 message_capacity,
                 "Steam: steam_api library not found: the executable directory could not be "
                 "determined");
        return RT_SERVICES_STATUS_LIBRARY_NOT_FOUND;
    }
    if ((size_t)written >= capacity) {
        snprintf(message, message_capacity, "Steam: steam_api library path is too long");
        return RT_SERVICES_STATUS_LIBRARY_NOT_FOUND;
    }
    return RT_SERVICES_STATUS_OK;
}

/// @brief Resolve every core export from @p library.
/// @param library Opened redistributable.
/// @param core Receives the export table; partially written on failure.
/// @return NULL on success, or the name of the first missing export.
static const char *steam_resolve_core(void *library, steam_core_api *core) {
#define STEAM_RESOLVE(field, type, name)                                                           \
    do {                                                                                           \
        void *symbol = rt_services_dynlib_symbol(library, name);                                   \
        if (!symbol)                                                                               \
            return name;                                                                           \
        core->field = RT_FN_PTR_CAST((type)symbol);                                                \
    } while (0)
    STEAM_RESOLVE(init_flat, rt_steam_init_flat_fn, RT_STEAM_SYMBOL_INIT_FLAT);
    STEAM_RESOLVE(shutdown, rt_steam_void_fn, RT_STEAM_SYMBOL_SHUTDOWN);
    STEAM_RESOLVE(restart_app, rt_steam_restart_app_fn, RT_STEAM_SYMBOL_RESTART_APP);
    STEAM_RESOLVE(is_steam_running, rt_steam_bool_fn, RT_STEAM_SYMBOL_IS_STEAM_RUNNING);
    STEAM_RESOLVE(get_pipe, rt_steam_get_pipe_fn, RT_STEAM_SYMBOL_GET_PIPE);
    STEAM_RESOLVE(dispatch_init, rt_steam_void_fn, RT_STEAM_SYMBOL_DISPATCH_INIT);
    STEAM_RESOLVE(dispatch_run_frame, rt_steam_pipe_fn, RT_STEAM_SYMBOL_DISPATCH_RUN_FRAME);
    STEAM_RESOLVE(dispatch_next, rt_steam_next_callback_fn, RT_STEAM_SYMBOL_DISPATCH_NEXT);
    STEAM_RESOLVE(dispatch_free, rt_steam_pipe_fn, RT_STEAM_SYMBOL_DISPATCH_FREE);
    STEAM_RESOLVE(
        dispatch_call_result, rt_steam_call_result_fn, RT_STEAM_SYMBOL_DISPATCH_CALL_RESULT);
#undef STEAM_RESOLVE
    return NULL;
}

/// @brief Load the redistributable at @p path and bind its core exports.
/// @details Reuses the current library when @p path is unchanged. A library
///          that fails validation is left mapped but never used.
/// @param path Resolved library path.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return RT_SERVICES_STATUS_OK, LIBRARY_NOT_FOUND, or LIBRARY_INCOMPATIBLE.
static int64_t steam_load_library(const char *path, char *message, size_t message_capacity) {
    if (g_steam.library && strcmp(g_steam.library_path, path) == 0)
        return RT_SERVICES_STATUS_OK;
    if (!rt_services_dynlib_file_exists(path)) {
        snprintf(message, message_capacity, "Steam: steam_api library not found: %s", path);
        return RT_SERVICES_STATUS_LIBRARY_NOT_FOUND;
    }
    char reason[1024];
    void *library = rt_services_dynlib_open(path, reason, sizeof(reason));
    if (!library) {
        snprintf(message, message_capacity, "Steam: could not load '%s': %s", path, reason);
        return RT_SERVICES_STATUS_LIBRARY_INCOMPATIBLE;
    }
    steam_core_api core;
    memset(&core, 0, sizeof(core));
    const char *missing = steam_resolve_core(library, &core);
    if (missing) {
        snprintf(message,
                 message_capacity,
                 "Steam: %s is missing export '%s' (Steamworks SDK 1.61-1.65 redistributable "
                 "required)",
                 path,
                 missing);
        return RT_SERVICES_STATUS_LIBRARY_INCOMPATIBLE;
    }
    g_steam.library = library;
    snprintf(g_steam.library_path, sizeof(g_steam.library_path), "%s", path);
    g_steam.core = core;
    return RT_SERVICES_STATUS_OK;
}

/// @brief Resolve an export from the loaded library.
/// @param name Exported symbol name.
/// @return Symbol address, or NULL.
void *rt_services_steam_symbol(const char *name) {
    return rt_services_dynlib_symbol(g_steam.library, name);
}

/// @brief Call the first exported accessor in @p accessors.
/// @param accessors Accessor names in preference order.
/// @param count Number of entries in @p accessors.
/// @param out_index Receives the index of the exported accessor, or -1.
/// @return Interface pointer from the accessor, or NULL when none is exported or it returns NULL.
static void *steam_open_interface(const char *const *accessors, size_t count, int *out_index) {
    *out_index = -1;
    for (size_t i = 0; i < count; ++i) {
        void *symbol = rt_services_steam_symbol(accessors[i]);
        if (!symbol)
            continue;
        *out_index = (int)i;
        rt_steam_accessor_fn accessor = RT_FN_PTR_CAST((rt_steam_accessor_fn)symbol);
        return accessor();
    }
    return NULL;
}

/// @brief Record that an interface or export could not be bound.
/// @param what Accessor or export name(s).
/// @param features Human-readable list of disabled features.
void rt_services_steam_report_missing(const char *what, const char *features) {
    const int is_accessor = strncmp(what, "SteamAPI_Steam", 14) == 0;
    rt_services_provider_add_diagnostic("Steam: %s %s unavailable; %s disabled",
                                        is_accessor ? "interface" : "export",
                                        what,
                                        features);
}

/// @brief Bind every versioned interface after a successful SteamAPI_InitFlat.
static void steam_open_interfaces(void) {
    memset(&g_steam.user, 0, sizeof(g_steam.user));
    memset(&g_steam.friends, 0, sizeof(g_steam.friends));
    memset(&g_steam.utils, 0, sizeof(g_steam.utils));
    memset(&g_steam.apps, 0, sizeof(g_steam.apps));
    memset(&g_steam.user_stats, 0, sizeof(g_steam.user_stats));
    memset(&g_steam.remote_storage, 0, sizeof(g_steam.remote_storage));
    int version = -1;

    {
        static const char *const accessors[] = {RT_STEAM_SYMBOL_USER_V023};
        void *self = steam_open_interface(accessors, 1, &version);
        void *logged_on = rt_services_steam_symbol(RT_STEAM_SYMBOL_USER_LOGGED_ON);
        void *steam_id = rt_services_steam_symbol(RT_STEAM_SYMBOL_USER_GET_STEAM_ID);
        if (self && logged_on && steam_id) {
            g_steam.user.self = self;
            g_steam.user.logged_on = RT_FN_PTR_CAST((rt_steam_self_bool_fn)logged_on);
            g_steam.user.get_steam_id = RT_FN_PTR_CAST((rt_steam_self_u64_fn)steam_id);
        } else {
            rt_services_steam_report_missing(RT_STEAM_SYMBOL_USER_V023, "identity queries");
        }
    }

    {
        static const char *const accessors[] = {RT_STEAM_SYMBOL_FRIENDS_V018,
                                                RT_STEAM_SYMBOL_FRIENDS_V017};
        void *self = steam_open_interface(accessors, 2, &version);
        void *persona = rt_services_steam_symbol(RT_STEAM_SYMBOL_FRIENDS_PERSONA_NAME);
        if (self && persona) {
            g_steam.friends.self = self;
            g_steam.friends.persona_name = RT_FN_PTR_CAST((rt_steam_self_cstr_fn)persona);
        } else {
            rt_services_steam_report_missing(RT_STEAM_SYMBOL_FRIENDS_V018
                                             " or " RT_STEAM_SYMBOL_FRIENDS_V017,
                                             "user names, presence, and overlay pages");
        }
    }

    {
        static const char *const accessors[] = {RT_STEAM_SYMBOL_UTILS_V011,
                                                RT_STEAM_SYMBOL_UTILS_V010};
        void *self = steam_open_interface(accessors, 2, &version);
        void *app_id = rt_services_steam_symbol(RT_STEAM_SYMBOL_UTILS_GET_APP_ID);
        void *big_picture = rt_services_steam_symbol(RT_STEAM_SYMBOL_UTILS_BIG_PICTURE);
        void *hardware = rt_services_steam_symbol(RT_STEAM_SYMBOL_UTILS_STEAM_HARDWARE);
        void *proton = rt_services_steam_symbol(RT_STEAM_SYMBOL_UTILS_UNDER_PROTON);
        void *deck = rt_services_steam_symbol(RT_STEAM_SYMBOL_UTILS_ON_STEAM_DECK);
        const int v011 = version == 0 && hardware && proton;
        const int v010 = version == 1 && deck;
        if (self && app_id && big_picture && (v011 || v010)) {
            g_steam.utils.self = self;
            g_steam.utils.version = v011 ? 11 : 10;
            g_steam.utils.get_app_id = RT_FN_PTR_CAST((rt_steam_self_u32_fn)app_id);
            g_steam.utils.big_picture = RT_FN_PTR_CAST((rt_steam_self_bool_fn)big_picture);
            if (v011) {
                g_steam.utils.steam_hardware = RT_FN_PTR_CAST((rt_steam_self_enum_fn)hardware);
                g_steam.utils.under_proton = RT_FN_PTR_CAST((rt_steam_self_bool_fn)proton);
            } else {
                g_steam.utils.on_steam_deck = RT_FN_PTR_CAST((rt_steam_self_bool_fn)deck);
            }
        } else {
            rt_services_steam_report_missing(
                RT_STEAM_SYMBOL_UTILS_V011 " or " RT_STEAM_SYMBOL_UTILS_V010,
                "app id, hardware, Proton, Big Picture, overlay, and text input queries");
        }
    }

    {
        static const char *const accessors[] = {RT_STEAM_SYMBOL_APPS_V009,
                                                RT_STEAM_SYMBOL_APPS_V008};
        void *self = steam_open_interface(accessors, 2, &version);
        void *subscribed = rt_services_steam_symbol(RT_STEAM_SYMBOL_APPS_IS_SUBSCRIBED);
        void *dlc = rt_services_steam_symbol(RT_STEAM_SYMBOL_APPS_IS_DLC_INSTALLED);
        void *language = rt_services_steam_symbol(RT_STEAM_SYMBOL_APPS_GAME_LANGUAGE);
        if (self && subscribed && dlc && language) {
            g_steam.apps.self = self;
            g_steam.apps.is_subscribed = RT_FN_PTR_CAST((rt_steam_self_bool_fn)subscribed);
            g_steam.apps.is_dlc_installed = RT_FN_PTR_CAST((rt_steam_self_app_bool_fn)dlc);
            g_steam.apps.game_language = RT_FN_PTR_CAST((rt_steam_self_cstr_fn)language);
        } else {
            rt_services_steam_report_missing(RT_STEAM_SYMBOL_APPS_V009
                                             " or " RT_STEAM_SYMBOL_APPS_V008,
                                             "licensing, DLC, and language queries");
        }
    }

    {
        static const char *const accessors[] = {RT_STEAM_SYMBOL_USER_STATS_V013};
        void *self = steam_open_interface(accessors, 1, &version);
        void *player_count = rt_services_steam_symbol(RT_STEAM_SYMBOL_USER_STATS_PLAYER_COUNT);
        if (self) {
            g_steam.user_stats.self = self;
            if (player_count) {
                g_steam.user_stats.player_count =
                    RT_FN_PTR_CAST((rt_steam_self_call_fn)player_count);
            } else {
                rt_services_steam_report_missing(RT_STEAM_SYMBOL_USER_STATS_PLAYER_COUNT,
                                                 "player counts");
            }
        } else {
            rt_services_steam_report_missing(RT_STEAM_SYMBOL_USER_STATS_V013,
                                             "player counts, achievements, stats, and "
                                             "leaderboards");
        }
    }

    rt_services_steam_bind_user_stats();
    rt_services_steam_bind_social();
    rt_services_steam_bind_cloud();
}

//===----------------------------------------------------------------------===//
// Operations
//===----------------------------------------------------------------------===//

/// @brief Allocate an operation slot and a fresh provider token.
/// @param stage Initial stage.
/// @return Operation slot, or NULL when the table is full.
steam_request_op *rt_services_steam_op_alloc(steam_op_stage stage) {
    for (size_t i = 0; i < sizeof(g_steam.ops) / sizeof(g_steam.ops[0]); ++i) {
        steam_request_op *op = &g_steam.ops[i];
        if (op->stage != STEAM_OP_FREE)
            continue;
        memset(op, 0, sizeof(*op));
        if (++g_steam.next_token == 0)
            g_steam.next_token = 1;
        op->token = g_steam.next_token;
        op->stage = stage;
        return op;
    }
    return NULL;
}

/// @brief Find the operation waiting on a Steam call.
/// @param call SteamAPICall_t from a completion record.
/// @return Operation slot, or NULL.
steam_request_op *rt_services_steam_op_find_call(rt_steam_api_call call) {
    if (call == 0)
        return NULL;
    for (size_t i = 0; i < sizeof(g_steam.ops) / sizeof(g_steam.ops[0]); ++i) {
        if (g_steam.ops[i].stage != STEAM_OP_FREE && g_steam.ops[i].call == call)
            return &g_steam.ops[i];
    }
    return NULL;
}

/// @brief Find the first operation in a stage.
/// @param stage Stage to look for.
/// @return Operation slot, or NULL.
steam_request_op *rt_services_steam_op_find_stage(steam_op_stage stage) {
    for (size_t i = 0; i < sizeof(g_steam.ops) / sizeof(g_steam.ops[0]); ++i) {
        if (g_steam.ops[i].stage == stage)
            return &g_steam.ops[i];
    }
    return NULL;
}

/// @brief Release an operation slot.
/// @param op Slot to clear; NULL is ignored.
void rt_services_steam_op_free(steam_request_op *op) {
    if (op)
        memset(op, 0, sizeof(*op));
}

/// @brief Fail a pending operation's request and release its slot.
/// @param op Operation to fail.
/// @param error Failure message.
void rt_services_steam_op_fail(steam_request_op *op, const char *error) {
    if (!op)
        return;
    // Complete before freeing the slot so an error that points into it stays valid.
    rt_services_provider_complete_request(op->token, 0, RT_STEAM_RESULT_FAIL, 0, error);
    rt_services_steam_op_free(op);
}

//===----------------------------------------------------------------------===//
// Provider callbacks: lifecycle
//===----------------------------------------------------------------------===//

/// @brief Spell an ESteamAPIInitResult for diagnostics.
/// @param result Value returned by SteamAPI_InitFlat.
/// @return Stable name; unknown values report "Unknown".
static const char *steam_init_result_name(int result) {
    switch (result) {
        case RT_STEAM_INIT_RESULT_FAILED_GENERIC:
            return "FailedGeneric";
        case RT_STEAM_INIT_RESULT_NO_STEAM_CLIENT:
            return "NoSteamClient";
        case RT_STEAM_INIT_RESULT_VERSION_MISMATCH:
            return "VersionMismatch";
        default:
            return "Unknown";
    }
}

/// @brief Map an ESteamAPIInitResult failure to a services status.
/// @param result Nonzero value returned by SteamAPI_InitFlat.
/// @return RT_SERVICES_STATUS_* failure value.
static int64_t steam_status_for_init_result(int result) {
    switch (result) {
        case RT_STEAM_INIT_RESULT_NO_STEAM_CLIENT:
            return RT_SERVICES_STATUS_CLIENT_NOT_RUNNING;
        case RT_STEAM_INIT_RESULT_VERSION_MISMATCH:
            return RT_SERVICES_STATUS_VERSION_MISMATCH;
        default:
            return RT_SERVICES_STATUS_INIT_FAILED;
    }
}

/// @brief Start the Steam provider.
/// @param app_id Borrowed app id string; a malformed id traps.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return RT_SERVICES_STATUS_OK or a failure status.
static int64_t steam_start(rt_string app_id, char *message, size_t message_capacity) {
    const char *app_text = rt_string_cstr(app_id);
    uint32_t id = 0;
    if (!rt_services_steam_parse_id(app_text, &id)) {
        snprintf(message,
                 message_capacity,
                 "Services.Platform.Init: Steam app id '%s' must be an integer in 1..4294967295",
                 app_text);
        rt_trap(message);
        return RT_SERVICES_STATUS_INIT_FAILED;
    }

    char path[STEAM_PATH_CAPACITY];
    int64_t status = steam_resolve_library_path(path, sizeof(path), message, message_capacity);
    if (status != RT_SERVICES_STATUS_OK)
        return status;

    steam_export_app_id(id);
    status = steam_load_library(path, message, message_capacity);
    if (status != RT_SERVICES_STATUS_OK)
        return status;

    char error[RT_STEAM_ERR_MSG_CAPACITY];
    memset(error, 0, sizeof(error));
    int init_result = g_steam.core.init_flat(error);
    error[sizeof(error) - 1] = '\0';
    if (init_result != RT_STEAM_INIT_RESULT_OK) {
        snprintf(message,
                 message_capacity,
                 "Steam: SteamAPI_InitFlat failed (%s): %s",
                 steam_init_result_name(init_result),
                 error[0] ? error : "no details reported");
        return steam_status_for_init_result(init_result);
    }

    g_steam.core.dispatch_init();
    g_steam.pipe = g_steam.core.get_pipe();
    g_steam.started = 1;
    memset(g_steam.ops, 0, sizeof(g_steam.ops));
    steam_open_interfaces();
    if (rt_service_hooks_gpu_presenter_created()) {
        rt_services_provider_add_diagnostic(
            "Steam: initialized after a GPU-presented window was created; the desktop overlay "
            "may not attach");
    }
    return RT_SERVICES_STATUS_OK;
}

/// @brief Stop the Steam provider and forget interface pointers and operations.
/// @details The services core cancels the matching requests afterwards.
static void steam_stop(void) {
    if (!g_steam.started)
        return;
    g_steam.started = 0;
    g_steam.core.shutdown();
    g_steam.pipe = 0;
    memset(&g_steam.user, 0, sizeof(g_steam.user));
    memset(&g_steam.friends, 0, sizeof(g_steam.friends));
    memset(&g_steam.utils, 0, sizeof(g_steam.utils));
    memset(&g_steam.apps, 0, sizeof(g_steam.apps));
    memset(&g_steam.user_stats, 0, sizeof(g_steam.user_stats));
    memset(&g_steam.remote_storage, 0, sizeof(g_steam.remote_storage));
    memset(g_steam.ops, 0, sizeof(g_steam.ops));
    rt_services_steam_reset_user_stats();
}

//===----------------------------------------------------------------------===//
// Provider callbacks: dispatch
//===----------------------------------------------------------------------===//

/// @brief Check a callback payload against its declared layout size.
/// @param msg Dispatched callback.
/// @param expected Declared payload size in bytes.
/// @return 1 when the payload may be decoded, otherwise 0.
int rt_services_steam_payload_matches(const rt_steam_callback_msg *msg, size_t expected) {
    if (msg->param && msg->param_size >= 0 && (size_t)msg->param_size == expected)
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: callback %d payload is %d bytes; binding expects %u",
        msg->callback_id,
        msg->param_size,
        (unsigned)expected);
    return 0;
}

/// @brief Fetch a call result after validating its identifier and size.
/// @param completed Decoded SteamAPICallCompleted_t.
/// @param callback_id Expected result identifier.
/// @param buffer Destination for the result structure.
/// @param size Declared size of the result structure.
/// @param method Steam method name used in failure messages.
/// @param error Receives the failure message.
/// @param error_capacity Size of @p error in bytes.
/// @return 1 when the result was copied, otherwise 0.
int rt_services_steam_fetch_call_result(const rt_steam_api_call_completed *completed,
                                        int callback_id,
                                        void *buffer,
                                        size_t size,
                                        const char *method,
                                        char *error,
                                        size_t error_capacity) {
    if (completed->callback_id != callback_id || completed->param_size != size) {
        snprintf(error,
                 error_capacity,
                 "Steam: callback %d payload is %u bytes; binding expects %u",
                 completed->callback_id,
                 (unsigned)completed->param_size,
                 (unsigned)size);
        rt_services_provider_add_diagnostic("%s", error);
        return 0;
    }
    memset(buffer, 0, size);
    bool failed = false;
    bool fetched = g_steam.core.dispatch_call_result(
        g_steam.pipe, completed->async_call, buffer, (int)size, callback_id, &failed);
    if (!fetched || failed) {
        snprintf(error, error_capacity, "Steam: %s call failed", method);
        return 0;
    }
    return 1;
}

/// @brief Complete a PlayerCount operation from its SteamAPICallCompleted_t.
/// @param op Operation in the PlayerCount stage.
/// @param completed Decoded completion record.
static void steam_complete_player_count(steam_request_op *op,
                                        const rt_steam_api_call_completed *completed) {
    rt_steam_number_of_current_players result;
    char error[256];
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_NUMBER_OF_CURRENT_PLAYERS,
                                             &result,
                                             sizeof(result),
                                             "GetNumberOfCurrentPlayers",
                                             error,
                                             sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    if (result.success != 1) {
        rt_services_steam_op_fail(op, "Steam: player count is unavailable");
        return;
    }
    rt_services_provider_complete_request(
        op->token, 1, RT_STEAM_RESULT_OK, (int64_t)result.players, NULL);
    rt_services_steam_op_free(op);
}

/// @brief Route a SteamAPICallCompleted_t to the operation it completes.
/// @details Calls not started by this provider, or whose request was
///          cancelled, are ignored.
/// @param msg Dispatched callback carrying the completion record.
static void steam_route_call_result(const rt_steam_callback_msg *msg) {
    rt_steam_api_call_completed completed;
    if (!rt_services_steam_payload_matches(msg, sizeof(completed)))
        return;
    memcpy(&completed, msg->param, sizeof(completed));
    steam_request_op *op = rt_services_steam_op_find_call(completed.async_call);
    if (!op)
        return;
    switch (op->stage) {
        case STEAM_OP_PLAYER_COUNT:
            steam_complete_player_count(op, &completed);
            break;
        case STEAM_OP_FIND:
        case STEAM_OP_FIND_FOR_UPLOAD:
        case STEAM_OP_FIND_FOR_DOWNLOAD:
        case STEAM_OP_UPLOAD:
        case STEAM_OP_DOWNLOAD:
            rt_services_steam_leaderboard_call_completed(op, &completed);
            break;
        default:
            break;
    }
}

/// @brief Translate one dispatched callback into services events.
/// @param msg Dispatched callback; its payload is valid for this call only.
static void steam_handle_callback(const rt_steam_callback_msg *msg) {
    switch (msg->callback_id) {
        case RT_STEAM_CB_SERVERS_CONNECTED:
            rt_services_provider_emit_event(RT_SERVICES_EVENT_SERVICE_CONNECTED, 0, 0, 0, NULL);
            break;
        case RT_STEAM_CB_SERVER_CONNECT_FAILURE: {
            rt_steam_server_connect_failure payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                rt_services_provider_emit_event(RT_SERVICES_EVENT_CONNECT_FAILED,
                                                payload.result,
                                                0,
                                                payload.still_retrying ? 1 : 0,
                                                NULL);
            }
            break;
        }
        case RT_STEAM_CB_SERVERS_DISCONNECTED: {
            rt_steam_servers_disconnected payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                rt_services_provider_emit_event(
                    RT_SERVICES_EVENT_SERVICE_DISCONNECTED, payload.result, 0, 0, NULL);
            }
            break;
        }
        case RT_STEAM_CB_GAME_OVERLAY_ACTIVATED: {
            rt_steam_game_overlay_activated payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                rt_services_provider_emit_event(RT_SERVICES_EVENT_OVERLAY_CHANGED,
                                                0,
                                                payload.user_initiated ? 1 : 0,
                                                payload.active ? 1 : 0,
                                                NULL);
            }
            break;
        }
        case RT_STEAM_CB_API_CALL_COMPLETED:
            steam_route_call_result(msg);
            break;
        case RT_STEAM_CB_STEAM_SHUTDOWN:
            rt_services_provider_emit_event(RT_SERVICES_EVENT_SERVICE_SHUTDOWN, 0, 0, 0, NULL);
            break;
        case RT_STEAM_CB_DLC_INSTALLED: {
            rt_steam_dlc_installed payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                char text[16];
                memcpy(&payload, msg->param, sizeof(payload));
                snprintf(text, sizeof(text), "%u", (unsigned)payload.app_id);
                rt_services_provider_emit_event(
                    RT_SERVICES_EVENT_DLC_INSTALLED, 0, (int64_t)payload.app_id, 0, text);
            }
            break;
        }
        case RT_STEAM_CB_NEW_URL_LAUNCH_PARAMETERS:
            rt_services_provider_emit_event(
                RT_SERVICES_EVENT_LAUNCH_PARAMETERS_CHANGED, 0, 0, 0, NULL);
            break;
        default:
            if (!rt_services_steam_user_stats_callback(msg))
                (void)rt_services_steam_social_callback(msg);
            break;
    }
}

/// @brief Run one manual-dispatch frame and drain pending callbacks.
static void steam_pump(void) {
    if (!g_steam.started)
        return;
    g_steam.core.dispatch_run_frame(g_steam.pipe);
    for (int i = 0; i < STEAM_MAX_CALLBACKS_PER_PUMP && g_steam.started; ++i) {
        rt_steam_callback_msg msg;
        memset(&msg, 0, sizeof(msg));
        if (!g_steam.core.dispatch_next(g_steam.pipe, &msg))
            break;
        steam_handle_callback(&msg);
        g_steam.core.dispatch_free(g_steam.pipe);
    }
}

//===----------------------------------------------------------------------===//
// Provider callbacks: queries
//===----------------------------------------------------------------------===//

/// @brief Report whether a feature is available now.
/// @param feature RT_SERVICES_FEATURE_* value.
/// @return 1 when available, otherwise 0.
static int8_t steam_has_feature(int64_t feature) {
    if (!g_steam.started)
        return 0;
    switch (feature) {
        case RT_SERVICES_FEATURE_IDENTITY:
            return (g_steam.user.self && g_steam.friends.self) ? 1 : 0;
        case RT_SERVICES_FEATURE_LICENSING:
        case RT_SERVICES_FEATURE_LANGUAGE:
            return g_steam.apps.self ? 1 : 0;
        case RT_SERVICES_FEATURE_PLAYER_COUNT:
            return g_steam.user_stats.player_count ? 1 : 0;
        case RT_SERVICES_FEATURE_ACHIEVEMENTS:
            return g_steam.user_stats.achievements_ready ? 1 : 0;
        case RT_SERVICES_FEATURE_STATS:
            return g_steam.user_stats.stats_ready ? 1 : 0;
        case RT_SERVICES_FEATURE_LEADERBOARDS:
            return g_steam.user_stats.leaderboards_ready ? 1 : 0;
        case RT_SERVICES_FEATURE_PRESENCE:
            return g_steam.friends.presence_ready ? 1 : 0;
        case RT_SERVICES_FEATURE_OVERLAY:
            return (g_steam.friends.overlay_ready && g_steam.utils.overlay_ready) ? 1 : 0;
        case RT_SERVICES_FEATURE_TEXT_INPUT:
            return g_steam.utils.text_input_ready ? 1 : 0;
        case RT_SERVICES_FEATURE_CLOUD:
            return g_steam.remote_storage.self ? 1 : 0;
        default:
            return 0;
    }
}

/// @brief Read the app id Steam reports for this process.
/// @return Owned decimal string, or NULL when unavailable.
static rt_string steam_app_id(void) {
    if (!g_steam.started || !g_steam.utils.self)
        return NULL;
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", (unsigned)g_steam.utils.get_app_id(g_steam.utils.self));
    return rt_const_cstr(digits);
}

/// @brief Read the signed-in user's SteamID64 as a decimal string.
/// @return Owned string, or NULL when unavailable.
static rt_string steam_user_id(void) {
    if (!g_steam.started || !g_steam.user.self)
        return NULL;
    char digits[24];
    snprintf(digits,
             sizeof(digits),
             "%llu",
             (unsigned long long)g_steam.user.get_steam_id(g_steam.user.self));
    return rt_const_cstr(digits);
}

/// @brief Read the signed-in user's persona name.
/// @return Owned string, or NULL when unavailable.
static rt_string steam_user_name(void) {
    if (!g_steam.started || !g_steam.friends.self)
        return NULL;
    const char *name = g_steam.friends.persona_name(g_steam.friends.self);
    return (name && *name) ? rt_const_cstr(name) : NULL;
}

/// @brief Read the user's selected game language.
/// @return Owned API language code, or NULL when unavailable.
static rt_string steam_language(void) {
    if (!g_steam.started || !g_steam.apps.self)
        return NULL;
    const char *language = g_steam.apps.game_language(g_steam.apps.self);
    return (language && *language) ? rt_const_cstr(language) : NULL;
}

/// @brief Report whether the user owns a license for this app.
/// @return 1 when licensed, otherwise 0.
static int8_t steam_is_licensed(void) {
    if (!g_steam.started || !g_steam.apps.self)
        return 0;
    return g_steam.apps.is_subscribed(g_steam.apps.self) ? 1 : 0;
}

/// @brief Report whether the client is logged on to Steam servers.
/// @return 1 when online, otherwise 0.
static int8_t steam_is_online(void) {
    if (!g_steam.started || !g_steam.user.self)
        return 0;
    return g_steam.user.logged_on(g_steam.user.self) ? 1 : 0;
}

/// @brief Report whether a DLC app is owned and installed.
/// @param dlc_id Borrowed decimal DLC app id; a malformed id traps.
/// @return 1 when installed, otherwise 0.
static int8_t steam_is_dlc_installed(rt_string dlc_id) {
    const char *text = rt_string_cstr(dlc_id);
    uint32_t id = 0;
    if (!rt_services_steam_parse_id(text, &id)) {
        char message[256];
        snprintf(message,
                 sizeof(message),
                 "Services.Platform.IsDlcInstalled: Steam DLC id '%s' must be an integer in "
                 "1..4294967295",
                 text);
        rt_trap(message);
        return 0;
    }
    if (!g_steam.started || !g_steam.apps.self)
        return 0;
    return g_steam.apps.is_dlc_installed(g_steam.apps.self, id) ? 1 : 0;
}

/// @brief Start a player-count call.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when the call started, otherwise 0.
static int8_t steam_begin_player_count(uint64_t *out_handle,
                                       char *message,
                                       size_t message_capacity) {
    if (!g_steam.started || !g_steam.user_stats.player_count) {
        snprintf(message,
                 message_capacity,
                 "Steam: player counts are unavailable (ISteamUserStats interface missing)");
        return 0;
    }
    steam_request_op *op = rt_services_steam_op_alloc(STEAM_OP_PLAYER_COUNT);
    if (!op) {
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    rt_steam_api_call call = g_steam.user_stats.player_count(g_steam.user_stats.self);
    if (call == 0) {
        rt_services_steam_op_free(op);
        snprintf(message,
                 message_capacity,
                 "Steam: GetNumberOfCurrentPlayers returned an invalid call handle");
        return 0;
    }
    op->call = call;
    *out_handle = op->token;
    return 1;
}

/// @brief Begin an asynchronous request.
/// @param args Validated request kind and arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when the request started, otherwise 0.
static int8_t steam_begin_request(const rt_services_request_args *args,
                                  uint64_t *out_handle,
                                  char *message,
                                  size_t message_capacity) {
    switch (args->kind) {
        case RT_SERVICES_REQUEST_PLAYER_COUNT:
            return steam_begin_player_count(out_handle, message, message_capacity);
        case RT_SERVICES_REQUEST_LEADERBOARD_FIND:
        case RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD:
        case RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD:
            return rt_services_steam_begin_leaderboard(args, out_handle, message, message_capacity);
        case RT_SERVICES_REQUEST_TEXT_INPUT:
            return rt_services_steam_begin_text_input(args, out_handle, message, message_capacity);
        default:
            snprintf(message,
                     message_capacity,
                     "Steam: request kind %lld is not supported",
                     (long long)args->kind);
            return 0;
    }
}

/// @brief Steam provider registration consumed by the services core.
const rt_services_provider rt_services_steam_provider = {
    .id = "steam",
    .display_name = "Steam",
    .start = steam_start,
    .pump = steam_pump,
    .stop = steam_stop,
    .has_feature = steam_has_feature,
    .app_id = steam_app_id,
    .user_id = steam_user_id,
    .user_name = steam_user_name,
    .language = steam_language,
    .is_licensed = steam_is_licensed,
    .is_online = steam_is_online,
    .is_dlc_installed = steam_is_dlc_installed,
    .begin_request = steam_begin_request,
    .achievements = &rt_services_steam_achievement_ops,
    .stats = &rt_services_steam_stat_ops,
    .leaderboards = &rt_services_steam_leaderboard_ops,
    .presence = &rt_services_steam_presence_ops,
    .overlay = &rt_services_steam_overlay_ops,
    .text_input = &rt_services_steam_text_input_ops,
    .cloud = &rt_services_steam_cloud_ops,
};

//===----------------------------------------------------------------------===//
// Zanna.Services.Steam extension class
//===----------------------------------------------------------------------===//

/// @brief Relaunch through Steam when the process was not started by Steam.
/// @param app_id Steam app id in 1..4294967295; other values trap.
/// @return 1 when the caller must exit, otherwise 0.
int8_t rt_services_steam_restart_app_if_necessary(int64_t app_id) {
    if (!rt_services_provider_require_main_thread("Steam.RestartAppIfNecessary"))
        return 0;
    if (app_id < 1 || (uint64_t)app_id > STEAM_MAX_APP_ID) {
        char message[160];
        snprintf(message,
                 sizeof(message),
                 "Services.Steam.RestartAppIfNecessary: app id %lld must be in 1..4294967295",
                 (long long)app_id);
        rt_trap(message);
        return 0;
    }
    char path[STEAM_PATH_CAPACITY];
    char message[RT_SERVICES_MESSAGE_CAPACITY];
    message[0] = '\0';
    int64_t status = steam_resolve_library_path(path, sizeof(path), message, sizeof(message));
    if (status == RT_SERVICES_STATUS_OK)
        status = steam_load_library(path, message, sizeof(message));
    if (status != RT_SERVICES_STATUS_OK) {
        rt_services_provider_add_diagnostic("%s", message);
        return 0;
    }
    return g_steam.core.restart_app((uint32_t)app_id) ? 1 : 0;
}

/// @brief Report whether Steam is the active provider.
/// @return 1 while started, otherwise 0.
int8_t rt_services_steam_get_is_active(void) {
    if (!rt_services_provider_require_main_thread("Steam.IsActive"))
        return 0;
    return g_steam.started ? 1 : 0;
}

/// @brief Read the signed-in user's SteamID64.
/// @return SteamID64, or 0 when unavailable.
int64_t rt_services_steam_get_steam_id(void) {
    if (!rt_services_provider_require_main_thread("Steam.SteamId"))
        return 0;
    if (!g_steam.started || !g_steam.user.self)
        return 0;
    return (int64_t)g_steam.user.get_steam_id(g_steam.user.self);
}

/// @brief Read the Steam hardware type.
/// @return RT_SERVICES_STEAM_HARDWARE_* value.
int64_t rt_services_steam_get_hardware_type(void) {
    if (!rt_services_provider_require_main_thread("Steam.HardwareType"))
        return RT_SERVICES_STEAM_HARDWARE_UNKNOWN;
    if (!g_steam.started || !g_steam.utils.self)
        return RT_SERVICES_STEAM_HARDWARE_UNKNOWN;
    if (g_steam.utils.version == 11)
        return (int64_t)g_steam.utils.steam_hardware(g_steam.utils.self);
    return g_steam.utils.on_steam_deck(g_steam.utils.self) ? RT_SERVICES_STEAM_HARDWARE_STEAM_DECK
                                                           : RT_SERVICES_STEAM_HARDWARE_NONE;
}

/// @brief Report whether the game runs under Proton.
/// @return 1 under Proton (SDK 1.65 redistributables only), otherwise 0.
int8_t rt_services_steam_get_is_under_proton(void) {
    if (!rt_services_provider_require_main_thread("Steam.IsUnderProton"))
        return 0;
    if (!g_steam.started || !g_steam.utils.self || !g_steam.utils.under_proton)
        return 0;
    return g_steam.utils.under_proton(g_steam.utils.self) ? 1 : 0;
}

/// @brief Report whether Steam runs in Big Picture mode.
/// @return 1 in Big Picture mode, otherwise 0.
int8_t rt_services_steam_get_is_big_picture(void) {
    if (!rt_services_provider_require_main_thread("Steam.IsBigPicture"))
        return 0;
    if (!g_steam.started || !g_steam.utils.self)
        return 0;
    return g_steam.utils.big_picture(g_steam.utils.self) ? 1 : 0;
}

/// @brief Read the loaded redistributable's path.
/// @return Owned path, or the empty string when nothing is loaded.
rt_string rt_services_steam_get_library_path(void) {
    if (!rt_services_provider_require_main_thread("Steam.LibraryPath") || !g_steam.library)
        return rt_str_empty();
    return rt_const_cstr(g_steam.library_path);
}

/// @brief Return SteamHardware.Unknown. @return -1.
int64_t rt_services_steam_hardware_unknown(void) {
    return RT_SERVICES_STEAM_HARDWARE_UNKNOWN;
}

/// @brief Return SteamHardware.None. @return 0.
int64_t rt_services_steam_hardware_none(void) {
    return RT_SERVICES_STEAM_HARDWARE_NONE;
}

/// @brief Return SteamHardware.SteamDeck. @return 1.
int64_t rt_services_steam_hardware_steam_deck(void) {
    return RT_SERVICES_STEAM_HARDWARE_STEAM_DECK;
}

/// @brief Return SteamHardware.SteamMachine. @return 2.
int64_t rt_services_steam_hardware_steam_machine(void) {
    return RT_SERVICES_STEAM_HARDWARE_STEAM_MACHINE;
}

/// @brief Return SteamHardware.SteamFrame. @return 3.
int64_t rt_services_steam_hardware_steam_frame(void) {
    return RT_SERVICES_STEAM_HARDWARE_STEAM_FRAME;
}
