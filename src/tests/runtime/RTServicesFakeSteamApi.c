//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesFakeSteamApi.c
// Purpose: From-scratch stand-in for the Steamworks steam_api redistributable,
//          built as a shared library so tests exercise the Steam provider's
//          real run-time loading, symbol resolution, manual dispatch, call
//          results, and payload decoding without Steam or any Valve file.
// Key invariants:
//   - Exports only the flat-API symbols the Zanna Steam provider binds, plus
//     ZannaFakeSteam_* control and inspection functions for tests.
//   - Payload bytes are written at explicit offsets derived from the
//     documented layouts, independently of the runtime's own ABI header, so a
//     layout mistake in the runtime cannot be mirrored here.
//     ZANNA_FAKE_STEAM_PACK (8 on Windows, 4 elsewhere, set by CMake) selects
//     the redistributable packing those offsets follow.
//   - Build profiles select the exported interface generation:
//       default                              SDK 1.65 accessor set
//       ZANNA_FAKE_STEAM_PROFILE_164=1       SDK 1.61-1.64 accessor set
//       ZANNA_FAKE_STEAM_OMIT_INIT_FLAT=1    no SteamAPI_InitFlat export
//       ZANNA_FAKE_STEAM_CORE_ONLY=1         only the exports bound before
//                                            ADR 0353 (no player features)
//   - ZANNA_FAKE_STEAM_SCENARIO (read by SteamAPI_InitFlat) selects the init
//     outcome: unset or "ok", "no-client", "version-mismatch", "generic".
//     ZANNA_FAKE_STEAM_RESTART=1 makes SteamAPI_RestartAppIfNecessary report true.
//     In-process tests use ZannaFakeSteam_SetScenario/SetRestart instead,
//     because a CRT environment snapshot may not observe variables changed
//     after the C runtime started (Windows).
// Ownership/Lifetime:
//   - All state is static and lives for the process; the library is never
//     unloaded by the runtime. Cloud file contents are heap copies freed on
//     delete, overwrite, and reset.
//   - Callback payload pointers stay valid until FreeLastCallback.
// Links: src/runtime/services/steam/rt_steam_provider.c,
//        src/tests/runtime/RTServicesTests.cpp,
//        src/tests/runtime/RTServicesFeatureTests.cpp,
//        src/tests/fixtures/runtime/test_services_platform.zia
//
//===----------------------------------------------------------------------===//

/**
 * @file RTServicesFakeSteamApi.c
 * @brief Scriptable fake of the Steamworks flat C API for platform services tests.
 * @details After a successful SteamAPI_InitFlat, the first dispatch frame queues
 *          SteamServersConnected, GameOverlayActivated (open, user initiated),
 *          and DlcInstalled (1234567) unless scripting is disabled. Asynchronous
 *          calls (player counts, leaderboards) complete on the next dispatch
 *          frame. The fake models three achievements, three stats, leaderboard
 *          "HOME_RUNS" with three entries, rich presence, overlay requests, both
 *          gamepad keyboards, and an in-memory Steam Cloud with a 1 MiB quota.
 *          Tests can inject raw callbacks with ZannaFakeSteam_QueueCallback.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ZANNA_FAKE_STEAM_PACK)
#define ZANNA_FAKE_STEAM_PACK 4
#endif

/// @brief Nonzero under the Windows redistributable packing (pack 8).
#define FAKE_PACK8 (ZANNA_FAKE_STEAM_PACK == 8)

/// @brief Maximum queued callbacks.
#define FAKE_QUEUE_CAPACITY 1024
/// @brief Maximum payload bytes per queued callback.
#define FAKE_PAYLOAD_CAPACITY 256
/// @brief Maximum outstanding asynchronous calls.
#define FAKE_CALL_CAPACITY 128
/// @brief First call handle issued by asynchronous calls.
#define FAKE_FIRST_CALL_HANDLE UINT64_C(0x5EA0000000000001)
/// @brief SteamID64 reported for the fake signed-in user.
#define FAKE_STEAM_ID UINT64_C(76561198000000001)
/// @brief DLC app id reported as installed.
#define FAKE_INSTALLED_DLC UINT32_C(1234567)
/// @brief Player count reported by completed player-count calls.
#define FAKE_PLAYER_COUNT INT32_C(42)
/// @brief Number of modeled leaderboards.
#define FAKE_BOARD_CAPACITY 8
/// @brief Entries per modeled leaderboard.
#define FAKE_BOARD_ENTRY_CAPACITY 256
/// @brief Retained downloads awaiting GetDownloadedLeaderboardEntry.
#define FAKE_DOWNLOAD_CAPACITY 16
/// @brief Rich presence key limit (k_cchMaxRichPresenceKeys).
#define FAKE_PRESENCE_KEYS 30
/// @brief Cloud files the fake stores.
#define FAKE_CLOUD_FILE_CAPACITY 16
/// @brief Cloud quota in bytes.
#define FAKE_CLOUD_QUOTA INT64_C(1048576)

/// @brief One queued callback record.
typedef struct fake_callback {
    int32_t id;                           ///< Callback identifier.
    int32_t size;                         ///< Payload size in bytes.
    uint8_t bytes[FAKE_PAYLOAD_CAPACITY]; ///< Payload bytes.
} fake_callback;

/// @brief One issued asynchronous call and its result structure.
typedef struct fake_call {
    uint64_t handle;     ///< Issued SteamAPICall_t.
    int32_t callback_id; ///< Identifier of the result structure.
    int32_t size;        ///< Size of the result structure.
    uint8_t payload[64]; ///< Result structure bytes.
    int completed;       ///< Nonzero once its SteamAPICallCompleted_t was queued.
} fake_call;

/// @brief One modeled achievement.
typedef struct fake_achievement {
    const char *id;          ///< API name.
    const char *name;        ///< Display name.
    const char *description; ///< Description.
    const char *hidden;      ///< "1" when hidden.
    int unlocked;            ///< Nonzero when unlocked.
    uint32_t unlock_time;    ///< Unix unlock time.
    int unstored;            ///< Unlocked since the last StoreStats.
} fake_achievement;

/// @brief Stat type of a modeled stat.
typedef enum fake_stat_type { FAKE_STAT_INT, FAKE_STAT_FLOAT, FAKE_STAT_AVGRATE } fake_stat_type;

/// @brief One modeled stat.
typedef struct fake_stat {
    const char *name;    ///< API name.
    fake_stat_type type; ///< Stat type.
    int32_t int_value;   ///< INT value.
    float float_value;   ///< FLOAT or AVGRATE value.
} fake_stat;

/// @brief One leaderboard entry.
typedef struct fake_entry {
    uint64_t steam_id; ///< User.
    int32_t score;     ///< Score.
} fake_entry;

/// @brief One modeled leaderboard, kept sorted by rank.
typedef struct fake_board {
    char name[128];                                ///< Board name, "" when unused.
    int sort;                                      ///< 1 ascending, 2 descending.
    int display;                                   ///< ELeaderboardDisplayType.
    int count;                                     ///< Entry count.
    fake_entry entries[FAKE_BOARD_ENTRY_CAPACITY]; ///< Entries in rank order.
} fake_board;

/// @brief One retained download.
typedef struct fake_download {
    uint64_t handle;                      ///< SteamLeaderboardEntries_t.
    int board;                            ///< Board index.
    int count;                            ///< Downloaded entries.
    int first[FAKE_BOARD_ENTRY_CAPACITY]; ///< Entry indexes in download order.
    int reads;                            ///< Entries read so far.
} fake_download;

/// @brief One rich presence pair.
typedef struct fake_presence {
    char key[64];    ///< Key, "" when unused.
    char value[256]; ///< Value.
} fake_presence;

/// @brief One cloud file.
typedef struct fake_cloud_file {
    char name[260];    ///< File name, "" when unused.
    uint8_t *data;     ///< Heap copy of the contents, or NULL when empty.
    int32_t size;      ///< Content size.
    int64_t timestamp; ///< Write time.
} fake_cloud_file;

static fake_callback g_queue[FAKE_QUEUE_CAPACITY];
static int g_queue_head;
static int g_queue_count;
static int g_outstanding;
static uint8_t g_current_payload[FAKE_PAYLOAD_CAPACITY];
static fake_call g_calls[FAKE_CALL_CAPACITY];
static int g_call_count;
static uint64_t g_next_call_handle = FAKE_FIRST_CALL_HANDLE;
static int g_initialized;
static int g_scripted_enabled = 1;
static int g_scripted_pending;
static int g_init_count;
static int g_shutdown_count;
static int g_run_frame_count;
static int g_dispatch_init_count;
static int g_protocol_violation;
static uint32_t g_last_restart_app_id;
static char g_app_id_env_at_init[32];
static char g_scenario_override[32];
static int g_restart_override = -1;

static fake_achievement g_achievements[] = {
    {"ACH_WIN_ONE_GAME", "Winner", "Win one game", "0", 0, 0, 0},
    {"ACH_HIT_FOR_CYCLE", "Hit for the Cycle", "Single, double, triple, and homer", "1", 0, 0, 0},
    {"ACH_TRAVEL_FAR", "Globetrotter", "Run 5280 feet", "0", 0, 0, 0},
};
static fake_stat g_stats[] = {
    {"NumGames", FAKE_STAT_INT, 0, 0.0f},
    {"FeetTraveled", FAKE_STAT_FLOAT, 0, 0.0f},
    {"AverageSpeed", FAKE_STAT_AVGRATE, 0, 0.0f},
};
static int32_t g_store_result = 1;
static fake_board g_boards[FAKE_BOARD_CAPACITY];
static fake_download g_downloads[FAKE_DOWNLOAD_CAPACITY];
static uint64_t g_next_download_handle = UINT64_C(0xD0);
static int g_upload_success = 1;
static int g_name_request_pending;
static int g_late_names_known;
static fake_presence g_presence[FAKE_PRESENCE_KEYS];
static int g_overlay_enabled = 1;
static char g_last_ui_call[512];
static int g_keyboards_supported = 1;
static int g_text_input_pending;
static int g_text_submit = 1;
static char g_text_value[256] = "Home Nine";
static fake_cloud_file g_cloud[FAKE_CLOUD_FILE_CAPACITY];
static int g_cloud_account_enabled = 1;
static int g_cloud_batch_open;
static int64_t g_cloud_clock = INT64_C(1700000100);
static int g_features_ready;

/// @brief Distinct non-null addresses used as interface pointers.
static const char g_user_iface = 'u';
static const char g_friends_iface = 'f';
static const char g_utils_iface = 't';
static const char g_apps_iface = 'a';
static const char g_user_stats_iface = 's';
static const char g_remote_storage_iface = 'r';

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Append a callback record, ignoring overflow.
/// @param id Callback identifier.
/// @param bytes Payload bytes, or NULL for a zeroed payload.
/// @param size Payload size in bytes (clamped to the payload capacity).
static void fake_queue(int32_t id, const uint8_t *bytes, int32_t size) {
    if (g_queue_count == FAKE_QUEUE_CAPACITY)
        return;
    int32_t clamped = size < 0 ? 0 : (size > FAKE_PAYLOAD_CAPACITY ? FAKE_PAYLOAD_CAPACITY : size);
    fake_callback *slot = &g_queue[(g_queue_head + g_queue_count) % FAKE_QUEUE_CAPACITY];
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->size = size;
    if (bytes && clamped > 0)
        memcpy(slot->bytes, bytes, (size_t)clamped);
    g_queue_count++;
}

/// @brief Write a 32-bit value at an explicit offset.
/// @param out Destination payload.
/// @param offset Byte offset.
/// @param value Value to store in host byte order.
static void fake_put_u32(uint8_t *out, size_t offset, uint32_t value) {
    memcpy(out + offset, &value, sizeof(value));
}

/// @brief Write a 64-bit value at an explicit offset.
/// @param out Destination payload.
/// @param offset Byte offset.
/// @param value Value to store in host byte order.
static void fake_put_u64(uint8_t *out, size_t offset, uint64_t value) {
    memcpy(out + offset, &value, sizeof(value));
}

/// @brief Register an asynchronous call whose result structure is ready.
/// @param callback_id Identifier of the result structure.
/// @param payload Result bytes.
/// @param size Result size.
/// @return Issued call handle, or 0 when the call table is full.
static uint64_t fake_issue_call(int32_t callback_id, const uint8_t *payload, int32_t size) {
    if (g_call_count == FAKE_CALL_CAPACITY || size > (int32_t)sizeof(g_calls[0].payload))
        return 0;
    fake_call *call = &g_calls[g_call_count++];
    memset(call, 0, sizeof(*call));
    call->handle = g_next_call_handle++;
    call->callback_id = callback_id;
    call->size = size;
    if (payload && size > 0)
        memcpy(call->payload, payload, (size_t)size);
    return call->handle;
}

/// @brief Read the init scenario from the test override or the environment.
/// @return Scenario name; "ok" when neither is set.
static const char *fake_scenario(void) {
    if (g_scenario_override[0])
        return g_scenario_override;
    const char *scenario = getenv("ZANNA_FAKE_STEAM_SCENARIO");
    return (scenario && *scenario) ? scenario : "ok";
}

/// @brief Copy text into a SteamErrMsg-style buffer.
/// @param err Destination buffer of at least 1024 bytes, or NULL.
/// @param text Message to copy.
static void fake_write_error(char *err, const char *text) {
    if (!err)
        return;
    size_t len = strlen(text);
    if (len > 1023)
        len = 1023;
    memcpy(err, text, len);
    err[len] = '\0';
}

/// @brief Read the app id the fake reports.
/// @return SteamAppId from the environment, or 480.
static uint32_t fake_app_id(void) {
    const char *env_app_id = getenv("SteamAppId");
    if (env_app_id && *env_app_id)
        return (uint32_t)strtoul(env_app_id, NULL, 10);
    return 480u;
}

/// @brief Reset the modeled player features to their initial state.
static void fake_reset_features(void) {
    for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        g_achievements[i].unlocked = 0;
        g_achievements[i].unlock_time = 0;
        g_achievements[i].unstored = 0;
    }
    for (size_t i = 0; i < sizeof(g_stats) / sizeof(g_stats[0]); ++i) {
        g_stats[i].int_value = 0;
        g_stats[i].float_value = 0.0f;
    }
    g_store_result = 1;
    memset(g_boards, 0, sizeof(g_boards));
    snprintf(g_boards[0].name, sizeof(g_boards[0].name), "HOME_RUNS");
    g_boards[0].sort = 2;
    g_boards[0].display = 1;
    g_boards[0].count = 3;
    g_boards[0].entries[0].steam_id = UINT64_C(76561198000000002);
    g_boards[0].entries[0].score = 61;
    g_boards[0].entries[1].steam_id = FAKE_STEAM_ID;
    g_boards[0].entries[1].score = 42;
    g_boards[0].entries[2].steam_id = UINT64_C(76561198000000003);
    g_boards[0].entries[2].score = 30;
    memset(g_downloads, 0, sizeof(g_downloads));
    g_upload_success = 1;
    g_name_request_pending = 0;
    g_late_names_known = 0;
    memset(g_presence, 0, sizeof(g_presence));
    g_overlay_enabled = 1;
    g_last_ui_call[0] = '\0';
    g_keyboards_supported = 1;
    g_text_input_pending = 0;
    g_text_submit = 1;
    snprintf(g_text_value, sizeof(g_text_value), "Home Nine");
    for (int i = 0; i < FAKE_CLOUD_FILE_CAPACITY; ++i)
        free(g_cloud[i].data);
    memset(g_cloud, 0, sizeof(g_cloud));
    g_cloud_account_enabled = 1;
    g_cloud_batch_open = 0;
    g_cloud_clock = INT64_C(1700000100);
    g_features_ready = 1;
}

//===----------------------------------------------------------------------===//
// Core flat API
//===----------------------------------------------------------------------===//

#if !defined(ZANNA_FAKE_STEAM_OMIT_INIT_FLAT)
/// @brief Fake SteamAPI_InitFlat; outcome chosen by ZANNA_FAKE_STEAM_SCENARIO.
/// @param err SteamErrMsg buffer.
/// @return ESteamAPIInitResult value.
int SteamAPI_InitFlat(char *err) {
    const char *env_app_id = getenv("SteamAppId");
    size_t len = env_app_id ? strlen(env_app_id) : 0;
    if (len >= sizeof(g_app_id_env_at_init))
        len = sizeof(g_app_id_env_at_init) - 1;
    if (env_app_id)
        memcpy(g_app_id_env_at_init, env_app_id, len);
    g_app_id_env_at_init[len] = '\0';

    const char *scenario = fake_scenario();
    if (strcmp(scenario, "no-client") == 0) {
        fake_write_error(err, "Steam client is not running (fake)");
        return 2;
    }
    if (strcmp(scenario, "version-mismatch") == 0) {
        fake_write_error(err, "Steam client is too old (fake)");
        return 3;
    }
    if (strcmp(scenario, "generic") == 0) {
        fake_write_error(err, "");
        return 1;
    }
    g_init_count++;
    g_initialized = 1;
    g_scripted_pending = g_scripted_enabled;
    // Processes that never call ZannaFakeSteam_Reset (the Zia fixtures) get
    // the modeled features on their first successful init.
    if (!g_features_ready)
        fake_reset_features();
    return 0;
}
#endif

/// @brief Fake SteamAPI_Shutdown.
void SteamAPI_Shutdown(void) {
    g_shutdown_count++;
    g_initialized = 0;
    g_queue_head = 0;
    g_queue_count = 0;
    g_outstanding = 0;
    g_call_count = 0;
    g_scripted_pending = 0;
    g_text_input_pending = 0;
}

/// @brief Fake SteamAPI_RestartAppIfNecessary.
/// @param app_id Requested app id.
/// @return The ZannaFakeSteam_SetRestart override, else true when ZANNA_FAKE_STEAM_RESTART is "1".
bool SteamAPI_RestartAppIfNecessary(uint32_t app_id) {
    g_last_restart_app_id = app_id;
    if (g_restart_override >= 0)
        return g_restart_override != 0;
    const char *restart = getenv("ZANNA_FAKE_STEAM_RESTART");
    return restart && strcmp(restart, "1") == 0;
}

/// @brief Fake SteamAPI_IsSteamRunning.
/// @return Always true.
bool SteamAPI_IsSteamRunning(void) {
    return true;
}

/// @brief Fake SteamAPI_GetHSteamPipe.
/// @return Pipe 1.
int32_t SteamAPI_GetHSteamPipe(void) {
    return 1;
}

/// @brief Fake SteamAPI_ManualDispatch_Init.
void SteamAPI_ManualDispatch_Init(void) {
    g_dispatch_init_count++;
}

/// @brief Fake SteamAPI_ManualDispatch_RunFrame: queues scripted, keyboard, and call callbacks.
/// @param pipe Client pipe (unused).
void SteamAPI_ManualDispatch_RunFrame(int32_t pipe) {
    (void)pipe;
    g_run_frame_count++;
    if (!g_initialized)
        return;
    if (g_scripted_pending) {
        uint8_t payload[FAKE_PAYLOAD_CAPACITY];
        g_scripted_pending = 0;

        memset(payload, 0, sizeof(payload));
        fake_queue(101, payload, 1);

        memset(payload, 0, sizeof(payload));
        payload[0] = 1;
        payload[1] = 1;
        fake_put_u32(payload, 4, 480u);
        fake_put_u32(payload, 8, 4242u);
        fake_queue(331, payload, 12);

        memset(payload, 0, sizeof(payload));
        fake_put_u32(payload, 0, FAKE_INSTALLED_DLC);
        fake_queue(1005, payload, 4);
    }
    if (g_name_request_pending) {
        g_name_request_pending = 0;
        g_late_names_known = 1;
    }
    if (g_text_input_pending) {
        uint8_t payload[12];
        g_text_input_pending = 0;
        memset(payload, 0, sizeof(payload));
        payload[0] = g_text_submit ? 1 : 0;
        fake_put_u32(payload, 4, g_text_submit ? (uint32_t)strlen(g_text_value) + 1u : 0u);
        fake_put_u32(payload, 8, fake_app_id());
        fake_queue(714, payload, 12);
    }
    for (int i = 0; i < g_call_count; ++i) {
        if (g_calls[i].completed)
            continue;
        uint8_t payload[16];
        memset(payload, 0, sizeof(payload));
        fake_put_u64(payload, 0, g_calls[i].handle);
        fake_put_u32(payload, 8, (uint32_t)g_calls[i].callback_id);
        fake_put_u32(payload, 12, (uint32_t)g_calls[i].size);
        fake_queue(703, payload, 16);
        g_calls[i].completed = 1;
    }
}

/// @brief Fake SteamAPI_ManualDispatch_GetNextCallback writing CallbackMsg_t at fixed offsets.
/// @param pipe Client pipe (unused).
/// @param msg CallbackMsg_t destination.
/// @return True when a callback was produced.
bool SteamAPI_ManualDispatch_GetNextCallback(int32_t pipe, void *msg) {
    (void)pipe;
    if (g_outstanding) {
        g_protocol_violation = 1;
        return false;
    }
    if (g_queue_count == 0 || !msg)
        return false;
    fake_callback *head = &g_queue[g_queue_head];
    int32_t user = 1;
    uint8_t *param = g_current_payload;
    memcpy(g_current_payload, head->bytes, sizeof(g_current_payload));
    memcpy((char *)msg + 0, &user, sizeof(user));
    memcpy((char *)msg + 4, &head->id, sizeof(head->id));
    memcpy((char *)msg + 8, &param, sizeof(param));
    memcpy((char *)msg + 16, &head->size, sizeof(head->size));
    g_outstanding = 1;
    return true;
}

/// @brief Fake SteamAPI_ManualDispatch_FreeLastCallback.
/// @param pipe Client pipe (unused).
void SteamAPI_ManualDispatch_FreeLastCallback(int32_t pipe) {
    (void)pipe;
    if (!g_outstanding) {
        g_protocol_violation = 1;
        return;
    }
    g_queue_head = (g_queue_head + 1) % FAKE_QUEUE_CAPACITY;
    g_queue_count--;
    g_outstanding = 0;
}

/// @brief Fake SteamAPI_ManualDispatch_GetAPICallResult.
/// @param pipe Client pipe (unused).
/// @param call Completed call handle.
/// @param buffer Destination for the result structure.
/// @param size Destination size.
/// @param expected_id Expected callback id.
/// @param failed Receives the failure flag.
/// @return True when the result was written.
bool SteamAPI_ManualDispatch_GetAPICallResult(
    int32_t pipe, uint64_t call, void *buffer, int size, int expected_id, bool *failed) {
    (void)pipe;
    for (int i = 0; i < g_call_count; ++i) {
        fake_call *entry = &g_calls[i];
        if (entry->handle != call || !entry->completed)
            continue;
        if (expected_id != entry->callback_id || size != entry->size || !buffer)
            break;
        memcpy(buffer, entry->payload, (size_t)entry->size);
        if (failed)
            *failed = false;
        return true;
    }
    if (failed)
        *failed = true;
    return false;
}

//===----------------------------------------------------------------------===//
// Interface accessors
//===----------------------------------------------------------------------===//

/// @brief Fake SteamAPI_SteamUser_v023. @return ISteamUser stand-in.
void *SteamAPI_SteamUser_v023(void) {
    return (void *)&g_user_iface;
}

/// @brief Fake SteamAPI_SteamUserStats_v013. @return ISteamUserStats stand-in.
void *SteamAPI_SteamUserStats_v013(void) {
    return (void *)&g_user_stats_iface;
}

#if defined(ZANNA_FAKE_STEAM_PROFILE_164)
/// @brief Fake SteamAPI_SteamFriends_v017. @return ISteamFriends stand-in.
void *SteamAPI_SteamFriends_v017(void) {
    return (void *)&g_friends_iface;
}

/// @brief Fake SteamAPI_SteamUtils_v010. @return ISteamUtils stand-in.
void *SteamAPI_SteamUtils_v010(void) {
    return (void *)&g_utils_iface;
}

/// @brief Fake SteamAPI_SteamApps_v008. @return ISteamApps stand-in.
void *SteamAPI_SteamApps_v008(void) {
    return (void *)&g_apps_iface;
}
#else
/// @brief Fake SteamAPI_SteamFriends_v018. @return ISteamFriends stand-in.
void *SteamAPI_SteamFriends_v018(void) {
    return (void *)&g_friends_iface;
}

/// @brief Fake SteamAPI_SteamUtils_v011. @return ISteamUtils stand-in.
void *SteamAPI_SteamUtils_v011(void) {
    return (void *)&g_utils_iface;
}

/// @brief Fake SteamAPI_SteamApps_v009. @return ISteamApps stand-in.
void *SteamAPI_SteamApps_v009(void) {
    return (void *)&g_apps_iface;
}
#endif

//===----------------------------------------------------------------------===//
// Interface methods bound before ADR 0353
//===----------------------------------------------------------------------===//

/// @brief Fake ISteamUser::BLoggedOn. @param self Interface. @return True.
bool SteamAPI_ISteamUser_BLoggedOn(void *self) {
    return self == (void *)&g_user_iface;
}

/// @brief Fake ISteamUser::GetSteamID. @param self Interface. @return Fixed SteamID64.
uint64_t SteamAPI_ISteamUser_GetSteamID(void *self) {
    return self == (void *)&g_user_iface ? FAKE_STEAM_ID : 0;
}

/// @brief Fake ISteamFriends::GetPersonaName. @param self Interface. @return Fixed name.
const char *SteamAPI_ISteamFriends_GetPersonaName(void *self) {
    return self == (void *)&g_friends_iface ? "Zanna Tester" : "";
}

/// @brief Fake ISteamUtils::GetAppID. @param self Interface. @return SteamAppId or 480.
uint32_t SteamAPI_ISteamUtils_GetAppID(void *self) {
    (void)self;
    return fake_app_id();
}

/// @brief Fake ISteamUtils::IsSteamInBigPictureMode. @param self Interface. @return False.
bool SteamAPI_ISteamUtils_IsSteamInBigPictureMode(void *self) {
    (void)self;
    return false;
}

#if defined(ZANNA_FAKE_STEAM_PROFILE_164)
/// @brief Fake ISteamUtils::IsSteamRunningOnSteamDeck (v010). @param self Interface. @return True.
bool SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(void *self) {
    (void)self;
    return true;
}
#else
/// @brief Fake ISteamUtils::IsRunningOnSteamHardware (v011). @param self Interface. @return Steam
/// Frame (3).
int SteamAPI_ISteamUtils_IsRunningOnSteamHardware(void *self) {
    (void)self;
    return 3;
}

/// @brief Fake ISteamUtils::IsRunningUnderProton (v011). @param self Interface. @return True.
bool SteamAPI_ISteamUtils_IsRunningUnderProton(void *self) {
    (void)self;
    return true;
}
#endif

/// @brief Fake ISteamApps::BIsSubscribed. @param self Interface. @return True.
bool SteamAPI_ISteamApps_BIsSubscribed(void *self) {
    return self == (void *)&g_apps_iface;
}

/// @brief Fake ISteamApps::BIsDlcInstalled. @param self Interface. @param app_id DLC id. @return
/// True for 1234567.
bool SteamAPI_ISteamApps_BIsDlcInstalled(void *self, uint32_t app_id) {
    return self == (void *)&g_apps_iface && app_id == FAKE_INSTALLED_DLC;
}

/// @brief Fake ISteamApps::GetCurrentGameLanguage. @param self Interface. @return "english".
const char *SteamAPI_ISteamApps_GetCurrentGameLanguage(void *self) {
    (void)self;
    return "english";
}

/// @brief Fake ISteamUserStats::GetNumberOfCurrentPlayers; completes on the next frame.
/// @param self Interface.
/// @return New call handle, or 0 when the call table is full.
uint64_t SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers(void *self) {
    (void)self;
    uint8_t payload[8];
    memset(payload, 0, sizeof(payload));
    payload[0] = 1;
    fake_put_u32(payload, 4, (uint32_t)FAKE_PLAYER_COUNT);
    return fake_issue_call(1107, payload, 8);
}

#if !defined(ZANNA_FAKE_STEAM_CORE_ONLY)

//===----------------------------------------------------------------------===//
// ISteamUserStats: achievements and stats
//===----------------------------------------------------------------------===//

/// @brief Find a modeled achievement.
/// @param name API name.
/// @return Achievement, or NULL.
static fake_achievement *fake_find_achievement(const char *name) {
    for (size_t i = 0; name && i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        if (strcmp(g_achievements[i].id, name) == 0)
            return &g_achievements[i];
    }
    return NULL;
}

/// @brief Find a modeled stat.
/// @param name API name.
/// @return Stat, or NULL.
static fake_stat *fake_find_stat(const char *name) {
    for (size_t i = 0; name && i < sizeof(g_stats) / sizeof(g_stats[0]); ++i) {
        if (strcmp(g_stats[i].name, name) == 0)
            return &g_stats[i];
    }
    return NULL;
}

/// @brief Queue a UserAchievementStored_t at explicit offsets.
/// @param name Achievement API name.
/// @param current Progress so far.
/// @param maximum Progress maximum.
static void fake_queue_achievement_stored(const char *name, uint32_t current, uint32_t maximum) {
    uint8_t payload[FAKE_PAYLOAD_CAPACITY];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, fake_app_id());
    snprintf((char *)payload + 9, 128, "%s", name);
    fake_put_u32(payload, 140, current);
    fake_put_u32(payload, 144, maximum);
    fake_queue(1103, payload, FAKE_PACK8 ? 152 : 148);
}

/// @brief Fake ISteamUserStats::SetAchievement.
bool SteamAPI_ISteamUserStats_SetAchievement(void *self, const char *name) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement)
        return false;
    if (!achievement->unlocked) {
        achievement->unlocked = 1;
        achievement->unlock_time = 1700000000u + (uint32_t)(achievement - g_achievements);
        achievement->unstored = 1;
    }
    return true;
}

/// @brief Fake ISteamUserStats::ClearAchievement.
bool SteamAPI_ISteamUserStats_ClearAchievement(void *self, const char *name) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement)
        return false;
    achievement->unlocked = 0;
    achievement->unlock_time = 0;
    achievement->unstored = 0;
    return true;
}

/// @brief Fake ISteamUserStats::GetAchievementAndUnlockTime.
bool SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime(void *self,
                                                          const char *name,
                                                          bool *achieved,
                                                          uint32_t *unlock_time) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement)
        return false;
    *achieved = achievement->unlocked != 0;
    *unlock_time = achievement->unlock_time;
    return true;
}

/// @brief Fake ISteamUserStats::IndicateAchievementProgress; queues UserAchievementStored_t.
bool SteamAPI_ISteamUserStats_IndicateAchievementProgress(void *self,
                                                          const char *name,
                                                          uint32_t current,
                                                          uint32_t maximum) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement || achievement->unlocked || current == 0 || current >= maximum)
        return false;
    fake_queue_achievement_stored(name, current, maximum);
    return true;
}

/// @brief Fake ISteamUserStats::GetNumAchievements. @return 3.
uint32_t SteamAPI_ISteamUserStats_GetNumAchievements(void *self) {
    (void)self;
    return (uint32_t)(sizeof(g_achievements) / sizeof(g_achievements[0]));
}

/// @brief Fake ISteamUserStats::GetAchievementName.
const char *SteamAPI_ISteamUserStats_GetAchievementName(void *self, uint32_t index) {
    (void)self;
    return index < sizeof(g_achievements) / sizeof(g_achievements[0]) ? g_achievements[index].id
                                                                      : "";
}

/// @brief Fake ISteamUserStats::GetAchievementDisplayAttribute.
const char *SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute(void *self,
                                                                    const char *name,
                                                                    const char *key) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement || !key)
        return "";
    if (strcmp(key, "name") == 0)
        return achievement->name;
    if (strcmp(key, "desc") == 0)
        return achievement->description;
    if (strcmp(key, "hidden") == 0)
        return achievement->hidden;
    return "";
}

/// @brief Fake ISteamUserStats::GetStatInt32.
bool SteamAPI_ISteamUserStats_GetStatInt32(void *self, const char *name, int32_t *value) {
    (void)self;
    fake_stat *stat = fake_find_stat(name);
    if (!stat || stat->type != FAKE_STAT_INT)
        return false;
    *value = stat->int_value;
    return true;
}

/// @brief Fake ISteamUserStats::SetStatInt32.
bool SteamAPI_ISteamUserStats_SetStatInt32(void *self, const char *name, int32_t value) {
    (void)self;
    fake_stat *stat = fake_find_stat(name);
    if (!stat || stat->type != FAKE_STAT_INT)
        return false;
    stat->int_value = value;
    return true;
}

/// @brief Fake ISteamUserStats::GetStatFloat.
bool SteamAPI_ISteamUserStats_GetStatFloat(void *self, const char *name, float *value) {
    (void)self;
    fake_stat *stat = fake_find_stat(name);
    if (!stat || stat->type == FAKE_STAT_INT)
        return false;
    *value = stat->float_value;
    return true;
}

/// @brief Fake ISteamUserStats::SetStatFloat (FLOAT stats only).
bool SteamAPI_ISteamUserStats_SetStatFloat(void *self, const char *name, float value) {
    (void)self;
    fake_stat *stat = fake_find_stat(name);
    if (!stat || stat->type != FAKE_STAT_FLOAT)
        return false;
    stat->float_value = value;
    return true;
}

/// @brief Fake ISteamUserStats::UpdateAvgRateStat (AVGRATE stats only; stores count / length).
bool SteamAPI_ISteamUserStats_UpdateAvgRateStat(void *self,
                                                const char *name,
                                                float count,
                                                double session_length) {
    (void)self;
    fake_stat *stat = fake_find_stat(name);
    if (!stat || stat->type != FAKE_STAT_AVGRATE || session_length <= 0.0)
        return false;
    stat->float_value = (float)((double)count / session_length);
    return true;
}

/// @brief Fake ISteamUserStats::StoreStats; queues UserStatsStored_t and new unlocks.
bool SteamAPI_ISteamUserStats_StoreStats(void *self) {
    (void)self;
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, fake_app_id());
    fake_put_u32(payload, 8, (uint32_t)g_store_result);
    fake_queue(1102, payload, FAKE_PACK8 ? 16 : 12);
    for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        if (!g_achievements[i].unstored)
            continue;
        g_achievements[i].unstored = 0;
        fake_queue_achievement_stored(g_achievements[i].id, 0, 0);
    }
    return true;
}

/// @brief Fake ISteamUserStats::ResetAllStats.
bool SteamAPI_ISteamUserStats_ResetAllStats(void *self, bool achievements_too) {
    (void)self;
    for (size_t i = 0; i < sizeof(g_stats) / sizeof(g_stats[0]); ++i) {
        g_stats[i].int_value = 0;
        g_stats[i].float_value = 0.0f;
    }
    if (achievements_too) {
        for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
            g_achievements[i].unlocked = 0;
            g_achievements[i].unlock_time = 0;
            g_achievements[i].unstored = 0;
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// ISteamUserStats: leaderboards
//===----------------------------------------------------------------------===//

/// @brief Convert a board index to its SteamLeaderboard_t.
/// @param index Board index.
/// @return Nonzero handle.
static uint64_t fake_board_handle(int index) {
    return UINT64_C(0xB0A2D0) + (uint64_t)index;
}

/// @brief Find a board by handle.
/// @param handle SteamLeaderboard_t.
/// @return Board index, or -1.
static int fake_board_index(uint64_t handle) {
    for (int i = 0; i < FAKE_BOARD_CAPACITY; ++i) {
        if (g_boards[i].name[0] && fake_board_handle(i) == handle)
            return i;
    }
    return -1;
}

/// @brief Find a board by name.
/// @param name Board name.
/// @return Board index, or -1.
static int fake_board_named(const char *name) {
    for (int i = 0; name && i < FAKE_BOARD_CAPACITY; ++i) {
        if (g_boards[i].name[0] && strcmp(g_boards[i].name, name) == 0)
            return i;
    }
    return -1;
}

/// @brief Issue a LeaderboardFindResult_t call.
/// @param index Board index, or -1 when not found.
/// @return Call handle.
static uint64_t fake_issue_find(int index) {
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, index >= 0 ? fake_board_handle(index) : 0);
    payload[8] = index >= 0 ? 1 : 0;
    return fake_issue_call(1104, payload, FAKE_PACK8 ? 16 : 12);
}

/// @brief Fake ISteamUserStats::FindLeaderboard.
uint64_t SteamAPI_ISteamUserStats_FindLeaderboard(void *self, const char *name) {
    (void)self;
    if (!name || !*name)
        return 0;
    return fake_issue_find(fake_board_named(name));
}

/// @brief Fake ISteamUserStats::FindOrCreateLeaderboard.
uint64_t SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(void *self,
                                                          const char *name,
                                                          int sort,
                                                          int display) {
    (void)self;
    if (!name || !*name)
        return 0;
    int index = fake_board_named(name);
    for (int i = 0; index < 0 && i < FAKE_BOARD_CAPACITY; ++i) {
        if (g_boards[i].name[0])
            continue;
        memset(&g_boards[i], 0, sizeof(g_boards[i]));
        snprintf(g_boards[i].name, sizeof(g_boards[i].name), "%s", name);
        g_boards[i].sort = sort;
        g_boards[i].display = display;
        index = i;
    }
    return fake_issue_find(index);
}

/// @brief Fake ISteamUserStats::GetLeaderboardName.
const char *SteamAPI_ISteamUserStats_GetLeaderboardName(void *self, uint64_t leaderboard) {
    (void)self;
    int index = fake_board_index(leaderboard);
    return index >= 0 ? g_boards[index].name : "";
}

/// @brief Fake ISteamUserStats::GetLeaderboardEntryCount.
int SteamAPI_ISteamUserStats_GetLeaderboardEntryCount(void *self, uint64_t leaderboard) {
    (void)self;
    int index = fake_board_index(leaderboard);
    return index >= 0 ? g_boards[index].count : 0;
}

/// @brief Report whether @p a ranks ahead of @p b on a board.
/// @param board Board.
/// @param a First score.
/// @param b Second score.
/// @return 1 when @p a is better.
static int fake_better(const fake_board *board, int32_t a, int32_t b) {
    return board->sort == 1 ? a < b : a > b;
}

/// @brief Fake ISteamUserStats::UploadLeaderboardScore; keeps entries sorted.
uint64_t SteamAPI_ISteamUserStats_UploadLeaderboardScore(void *self,
                                                         uint64_t leaderboard,
                                                         int method,
                                                         int32_t score,
                                                         const int32_t *details,
                                                         int detail_count) {
    (void)self;
    (void)details;
    (void)detail_count;
    uint8_t payload[32];
    memset(payload, 0, sizeof(payload));
    const size_t board_at = FAKE_PACK8 ? 8 : 4;
    const size_t score_at = FAKE_PACK8 ? 16 : 12;
    const size_t changed_at = FAKE_PACK8 ? 20 : 16;
    const size_t new_rank_at = FAKE_PACK8 ? 24 : 20;
    const size_t previous_rank_at = FAKE_PACK8 ? 28 : 24;
    fake_put_u64(payload, board_at, leaderboard);
    fake_put_u32(payload, score_at, (uint32_t)score);

    int index = fake_board_index(leaderboard);
    if (index >= 0 && g_upload_success) {
        fake_board *board = &g_boards[index];
        int previous = -1;
        for (int i = 0; i < board->count; ++i) {
            if (board->entries[i].steam_id == FAKE_STEAM_ID)
                previous = i;
        }
        int changed = previous < 0 || method == 2 ||
                      fake_better(board, score, board->entries[previous].score);
        if (changed) {
            if (previous >= 0) {
                memmove(&board->entries[previous],
                        &board->entries[previous + 1],
                        (size_t)(board->count - previous - 1) * sizeof(fake_entry));
                board->count--;
            }
            if (board->count < FAKE_BOARD_ENTRY_CAPACITY) {
                int at = 0;
                while (at < board->count && !fake_better(board, score, board->entries[at].score))
                    at++;
                memmove(&board->entries[at + 1],
                        &board->entries[at],
                        (size_t)(board->count - at) * sizeof(fake_entry));
                board->entries[at].steam_id = FAKE_STEAM_ID;
                board->entries[at].score = score;
                board->count++;
            }
        }
        int rank = 0;
        for (int i = 0; i < board->count; ++i) {
            if (board->entries[i].steam_id == FAKE_STEAM_ID)
                rank = i + 1;
        }
        payload[0] = 1;
        payload[changed_at] = changed ? 1 : 0;
        fake_put_u32(payload, new_rank_at, (uint32_t)rank);
        fake_put_u32(payload, previous_rank_at, (uint32_t)(previous + 1));
    }
    return fake_issue_call(1106, payload, FAKE_PACK8 ? 32 : 28);
}

/// @brief Fake ISteamUserStats::DownloadLeaderboardEntries.
uint64_t SteamAPI_ISteamUserStats_DownloadLeaderboardEntries(
    void *self, uint64_t leaderboard, int request, int start, int end) {
    (void)self;
    int index = fake_board_index(leaderboard);
    fake_download *download = NULL;
    for (int i = 0; i < FAKE_DOWNLOAD_CAPACITY; ++i) {
        if (g_downloads[i].handle == 0) {
            download = &g_downloads[i];
            break;
        }
    }
    if (index < 0 || !download)
        return 0;
    const fake_board *board = &g_boards[index];
    memset(download, 0, sizeof(*download));
    download->handle = g_next_download_handle++;
    download->board = index;
    int first = 0;
    int last = board->count - 1;
    if (request == 0) {
        first = start - 1;
        last = end - 1;
    } else if (request == 1) {
        int self_at = -1;
        for (int i = 0; i < board->count; ++i) {
            if (board->entries[i].steam_id == FAKE_STEAM_ID)
                self_at = i;
        }
        if (self_at < 0) {
            first = 1;
            last = 0;
        } else {
            first = self_at + start;
            last = self_at + end;
        }
    }
    if (first < 0)
        first = 0;
    if (last > board->count - 1)
        last = board->count - 1;
    for (int i = first; i <= last; ++i)
        download->first[download->count++] = i;

    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, leaderboard);
    fake_put_u64(payload, 8, download->handle);
    fake_put_u32(payload, 16, (uint32_t)download->count);
    return fake_issue_call(1105, payload, FAKE_PACK8 ? 24 : 20);
}

/// @brief Fake ISteamUserStats::GetDownloadedLeaderboardEntry writing LeaderboardEntry_t offsets.
bool SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry(
    void *self, uint64_t entries, int index, void *entry, int32_t *details, int details_max) {
    (void)self;
    (void)details;
    (void)details_max;
    for (int i = 0; i < FAKE_DOWNLOAD_CAPACITY; ++i) {
        fake_download *download = &g_downloads[i];
        if (download->handle != entries || download->handle == 0)
            continue;
        if (index < 0 || index >= download->count || !entry)
            return false;
        const fake_entry *source = &g_boards[download->board].entries[download->first[index]];
        uint8_t bytes[32];
        memset(bytes, 0, sizeof(bytes));
        fake_put_u64(bytes, 0, source->steam_id);
        fake_put_u32(bytes, 8, (uint32_t)(download->first[index] + 1));
        fake_put_u32(bytes, 12, (uint32_t)source->score);
        memcpy(entry, bytes, FAKE_PACK8 ? 32 : 28);
        if (++download->reads >= download->count)
            memset(download, 0, sizeof(*download));
        return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// ISteamFriends: presence, overlay pages, names
//===----------------------------------------------------------------------===//

/// @brief Fake ISteamFriends::SetRichPresence enforcing Steam's documented limits.
bool SteamAPI_ISteamFriends_SetRichPresence(void *self, const char *key, const char *value) {
    (void)self;
    if (!key || !*key || strlen(key) >= 64 || !value || strlen(value) >= 256)
        return false;
    int free_slot = -1;
    for (int i = 0; i < FAKE_PRESENCE_KEYS; ++i) {
        if (strcmp(g_presence[i].key, key) == 0) {
            if (!*value)
                memset(&g_presence[i], 0, sizeof(g_presence[i]));
            else
                snprintf(g_presence[i].value, sizeof(g_presence[i].value), "%s", value);
            return true;
        }
        if (!g_presence[i].key[0] && free_slot < 0)
            free_slot = i;
    }
    if (!*value)
        return true;
    if (free_slot < 0)
        return false;
    snprintf(g_presence[free_slot].key, sizeof(g_presence[free_slot].key), "%s", key);
    snprintf(g_presence[free_slot].value, sizeof(g_presence[free_slot].value), "%s", value);
    return true;
}

/// @brief Fake ISteamFriends::ClearRichPresence.
void SteamAPI_ISteamFriends_ClearRichPresence(void *self) {
    (void)self;
    memset(g_presence, 0, sizeof(g_presence));
}

/// @brief Fake ISteamFriends::ActivateGameOverlay; records the dialog.
void SteamAPI_ISteamFriends_ActivateGameOverlay(void *self, const char *dialog) {
    (void)self;
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "ActivateGameOverlay(%s)", dialog);
}

/// @brief Fake ISteamFriends::ActivateGameOverlayToWebPage; records the URL and mode.
void SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage(void *self, const char *url, int mode) {
    (void)self;
    snprintf(
        g_last_ui_call, sizeof(g_last_ui_call), "ActivateGameOverlayToWebPage(%s,%d)", url, mode);
}

/// @brief Fake ISteamFriends::ActivateGameOverlayToStore; records the app and flag.
void SteamAPI_ISteamFriends_ActivateGameOverlayToStore(void *self, uint32_t app_id, int flag) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "ActivateGameOverlayToStore(%u,%d)",
             (unsigned)app_id,
             flag);
}

/// @brief Fake ISteamFriends::GetFriendPersonaName.
/// @details 76561198000000003 stays "[unknown]" until RequestUserInformation was
///          called and a dispatch frame ran, modeling a late PersonaStateChange.
const char *SteamAPI_ISteamFriends_GetFriendPersonaName(void *self, uint64_t steam_id) {
    (void)self;
    if (steam_id == FAKE_STEAM_ID)
        return "Zanna Tester";
    if (steam_id == UINT64_C(76561198000000002))
        return "Slugger";
    if (steam_id == UINT64_C(76561198000000003) && g_late_names_known)
        return "Rookie";
    return "[unknown]";
}

/// @brief Fake ISteamFriends::RequestUserInformation; names arrive after the next frame.
bool SteamAPI_ISteamFriends_RequestUserInformation(void *self,
                                                   uint64_t steam_id,
                                                   bool require_name_only) {
    (void)self;
    (void)steam_id;
    (void)require_name_only;
    if (!g_late_names_known)
        g_name_request_pending = 1;
    return !g_late_names_known;
}

//===----------------------------------------------------------------------===//
// ISteamUtils: overlay status and keyboards
//===----------------------------------------------------------------------===//

/// @brief Fake ISteamUtils::IsOverlayEnabled.
bool SteamAPI_ISteamUtils_IsOverlayEnabled(void *self) {
    (void)self;
    return g_overlay_enabled != 0;
}

/// @brief Fake ISteamUtils::SetOverlayNotificationPosition; records the corner.
void SteamAPI_ISteamUtils_SetOverlayNotificationPosition(void *self, int position) {
    (void)self;
    snprintf(
        g_last_ui_call, sizeof(g_last_ui_call), "SetOverlayNotificationPosition(%d)", position);
}

/// @brief Fake ISteamUtils::SetOverlayNotificationInset; records the inset.
void SteamAPI_ISteamUtils_SetOverlayNotificationInset(void *self, int horizontal, int vertical) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "SetOverlayNotificationInset(%d,%d)",
             horizontal,
             vertical);
}

/// @brief Fake ISteamUtils::ShowFloatingGamepadTextInput; records the field.
bool SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput(
    void *self, int mode, int x, int y, int width, int height) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "ShowFloatingGamepadTextInput(%d,%d,%d,%d,%d)",
             mode,
             x,
             y,
             width,
             height);
    return g_keyboards_supported != 0;
}

/// @brief Fake ISteamUtils::DismissFloatingGamepadTextInput; queues
/// FloatingGamepadTextInputDismissed_t.
bool SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput(void *self) {
    (void)self;
    uint8_t payload[1] = {0};
    fake_queue(738, payload, 1);
    return true;
}

/// @brief Fake ISteamUtils::ShowGamepadTextInput; the dismissal arrives on the next frame.
bool SteamAPI_ISteamUtils_ShowGamepadTextInput(void *self,
                                               int input_mode,
                                               int line_mode,
                                               const char *description,
                                               uint32_t char_max,
                                               const char *existing) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "ShowGamepadTextInput(%d,%d,%s,%u,%s)",
             input_mode,
             line_mode,
             description,
             (unsigned)char_max,
             existing);
    if (!g_keyboards_supported)
        return false;
    g_text_input_pending = 1;
    return true;
}

/// @brief Fake ISteamUtils::GetEnteredGamepadTextLength; counts the terminator.
uint32_t SteamAPI_ISteamUtils_GetEnteredGamepadTextLength(void *self) {
    (void)self;
    return (uint32_t)strlen(g_text_value) + 1u;
}

/// @brief Fake ISteamUtils::GetEnteredGamepadTextInput.
bool SteamAPI_ISteamUtils_GetEnteredGamepadTextInput(void *self, char *text, uint32_t capacity) {
    (void)self;
    if (!text || capacity < (uint32_t)strlen(g_text_value) + 1u)
        return false;
    memcpy(text, g_text_value, strlen(g_text_value) + 1u);
    return true;
}

//===----------------------------------------------------------------------===//
// ISteamRemoteStorage
//===----------------------------------------------------------------------===//

/// @brief Fake SteamAPI_SteamRemoteStorage_v016. @return ISteamRemoteStorage stand-in.
void *SteamAPI_SteamRemoteStorage_v016(void) {
    return (void *)&g_remote_storage_iface;
}

/// @brief Find a cloud file.
/// @param name File name.
/// @return File, or NULL.
static fake_cloud_file *fake_cloud_find(const char *name) {
    for (int i = 0; name && i < FAKE_CLOUD_FILE_CAPACITY; ++i) {
        if (g_cloud[i].name[0] && strcmp(g_cloud[i].name, name) == 0)
            return &g_cloud[i];
    }
    return NULL;
}

/// @brief Sum the stored cloud bytes.
/// @return Bytes used.
static int64_t fake_cloud_used(void) {
    int64_t used = 0;
    for (int i = 0; i < FAKE_CLOUD_FILE_CAPACITY; ++i)
        used += g_cloud[i].name[0] ? g_cloud[i].size : 0;
    return used;
}

/// @brief Fake ISteamRemoteStorage::FileWrite enforcing the 1 MiB fake quota.
bool SteamAPI_ISteamRemoteStorage_FileWrite(void *self,
                                            const char *name,
                                            const void *data,
                                            int32_t size) {
    (void)self;
    if (!name || !*name || strlen(name) >= 260 || size < 0 || !data)
        return false;
    fake_cloud_file *file = fake_cloud_find(name);
    int64_t used = fake_cloud_used() - (file ? file->size : 0);
    if (used + size > FAKE_CLOUD_QUOTA)
        return false;
    for (int i = 0; !file && i < FAKE_CLOUD_FILE_CAPACITY; ++i) {
        if (!g_cloud[i].name[0]) {
            file = &g_cloud[i];
            snprintf(file->name, sizeof(file->name), "%s", name);
        }
    }
    if (!file)
        return false;
    free(file->data);
    file->data = NULL;
    if (size > 0) {
        file->data = (uint8_t *)malloc((size_t)size);
        if (!file->data)
            return false;
        memcpy(file->data, data, (size_t)size);
    }
    file->size = size;
    file->timestamp = g_cloud_clock++;
    return true;
}

/// @brief Fake ISteamRemoteStorage::FileRead.
int32_t SteamAPI_ISteamRemoteStorage_FileRead(void *self,
                                              const char *name,
                                              void *data,
                                              int32_t size) {
    (void)self;
    fake_cloud_file *file = fake_cloud_find(name);
    if (!file || !data || size < 0)
        return 0;
    int32_t count = size < file->size ? size : file->size;
    if (count > 0)
        memcpy(data, file->data, (size_t)count);
    return count;
}

/// @brief Fake ISteamRemoteStorage::FileExists.
bool SteamAPI_ISteamRemoteStorage_FileExists(void *self, const char *name) {
    (void)self;
    return fake_cloud_find(name) != NULL;
}

/// @brief Fake ISteamRemoteStorage::FileDelete.
bool SteamAPI_ISteamRemoteStorage_FileDelete(void *self, const char *name) {
    (void)self;
    fake_cloud_file *file = fake_cloud_find(name);
    if (!file)
        return false;
    free(file->data);
    memset(file, 0, sizeof(*file));
    return true;
}

/// @brief Fake ISteamRemoteStorage::GetFileSize.
int32_t SteamAPI_ISteamRemoteStorage_GetFileSize(void *self, const char *name) {
    (void)self;
    fake_cloud_file *file = fake_cloud_find(name);
    return file ? file->size : 0;
}

/// @brief Fake ISteamRemoteStorage::GetFileTimestamp.
int64_t SteamAPI_ISteamRemoteStorage_GetFileTimestamp(void *self, const char *name) {
    (void)self;
    fake_cloud_file *file = fake_cloud_find(name);
    return file ? file->timestamp : 0;
}

/// @brief Fake ISteamRemoteStorage::GetFileCount.
int32_t SteamAPI_ISteamRemoteStorage_GetFileCount(void *self) {
    (void)self;
    int32_t count = 0;
    for (int i = 0; i < FAKE_CLOUD_FILE_CAPACITY; ++i)
        count += g_cloud[i].name[0] ? 1 : 0;
    return count;
}

/// @brief Fake ISteamRemoteStorage::GetFileNameAndSize (files in slot order).
const char *SteamAPI_ISteamRemoteStorage_GetFileNameAndSize(void *self, int index, int32_t *size) {
    (void)self;
    int seen = 0;
    for (int i = 0; i < FAKE_CLOUD_FILE_CAPACITY; ++i) {
        if (!g_cloud[i].name[0])
            continue;
        if (seen++ == index) {
            if (size)
                *size = g_cloud[i].size;
            return g_cloud[i].name;
        }
    }
    if (size)
        *size = 0;
    return "";
}

/// @brief Fake ISteamRemoteStorage::GetQuota.
bool SteamAPI_ISteamRemoteStorage_GetQuota(void *self, uint64_t *total, uint64_t *available) {
    (void)self;
    if (!total || !available)
        return false;
    *total = (uint64_t)FAKE_CLOUD_QUOTA;
    *available = (uint64_t)(FAKE_CLOUD_QUOTA - fake_cloud_used());
    return true;
}

/// @brief Fake ISteamRemoteStorage::IsCloudEnabledForAccount.
bool SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(void *self) {
    (void)self;
    return g_cloud_account_enabled != 0;
}

/// @brief Fake ISteamRemoteStorage::IsCloudEnabledForApp.
bool SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(void *self) {
    (void)self;
    return true;
}

/// @brief Fake ISteamRemoteStorage::BeginFileWriteBatch; nested batches fail.
bool SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch(void *self) {
    (void)self;
    if (g_cloud_batch_open)
        return false;
    g_cloud_batch_open = 1;
    return true;
}

/// @brief Fake ISteamRemoteStorage::EndFileWriteBatch; fails without an open batch.
bool SteamAPI_ISteamRemoteStorage_EndFileWriteBatch(void *self) {
    (void)self;
    if (!g_cloud_batch_open)
        return false;
    g_cloud_batch_open = 0;
    return true;
}

#endif // !ZANNA_FAKE_STEAM_CORE_ONLY

//===----------------------------------------------------------------------===//
// Test control and inspection
//===----------------------------------------------------------------------===//

/// @brief Reset every counter, queue, scripted flag, and modeled feature.
void ZannaFakeSteam_Reset(void) {
    g_queue_head = 0;
    g_queue_count = 0;
    g_outstanding = 0;
    g_call_count = 0;
    g_initialized = 0;
    g_scripted_enabled = 1;
    g_scripted_pending = 0;
    g_init_count = 0;
    g_shutdown_count = 0;
    g_run_frame_count = 0;
    g_dispatch_init_count = 0;
    g_protocol_violation = 0;
    g_last_restart_app_id = 0;
    g_app_id_env_at_init[0] = '\0';
    g_scenario_override[0] = '\0';
    g_restart_override = -1;
    fake_reset_features();
}

/// @brief Override the init scenario for in-process tests.
/// @param scenario Scenario name, or NULL/empty to fall back to the environment.
void ZannaFakeSteam_SetScenario(const char *scenario) {
    size_t len = scenario ? strlen(scenario) : 0;
    if (len >= sizeof(g_scenario_override))
        len = sizeof(g_scenario_override) - 1;
    if (scenario)
        memcpy(g_scenario_override, scenario, len);
    g_scenario_override[len] = '\0';
}

/// @brief Override SteamAPI_RestartAppIfNecessary's result for in-process tests.
/// @param restart 1 or 0 to force the result, or -1 to fall back to the environment.
void ZannaFakeSteam_SetRestart(int restart) {
    g_restart_override = restart < 0 ? -1 : (restart ? 1 : 0);
}

/// @brief Enable or disable the scripted startup callbacks.
/// @param enabled Nonzero to queue them on the first frame after init.
void ZannaFakeSteam_SetScripted(int enabled) {
    g_scripted_enabled = enabled ? 1 : 0;
}

/// @brief Queue a raw callback for the next dispatch.
/// @param id Callback identifier.
/// @param bytes Payload bytes, or NULL for zeroed bytes.
/// @param size Reported payload size.
void ZannaFakeSteam_QueueCallback(int32_t id, const uint8_t *bytes, int32_t size) {
    fake_queue(id, bytes, size);
}

/// @brief Number of successful SteamAPI_InitFlat calls. @return Count.
int ZannaFakeSteam_InitCount(void) {
    return g_init_count;
}

/// @brief Number of SteamAPI_Shutdown calls. @return Count.
int ZannaFakeSteam_ShutdownCount(void) {
    return g_shutdown_count;
}

/// @brief Number of dispatch frames. @return Count.
int ZannaFakeSteam_RunFrameCount(void) {
    return g_run_frame_count;
}

/// @brief Number of SteamAPI_ManualDispatch_Init calls. @return Count.
int ZannaFakeSteam_DispatchInitCount(void) {
    return g_dispatch_init_count;
}

/// @brief Number of callbacks still queued. @return Count.
int ZannaFakeSteam_QueuedCount(void) {
    return g_queue_count;
}

/// @brief Whether GetNextCallback/FreeLastCallback were ever called out of order. @return 1 or 0.
int ZannaFakeSteam_ProtocolViolation(void) {
    return g_protocol_violation;
}

/// @brief App id passed to the last SteamAPI_RestartAppIfNecessary. @return App id.
uint32_t ZannaFakeSteam_LastRestartAppId(void) {
    return g_last_restart_app_id;
}

/// @brief SteamAppId environment value observed by the last SteamAPI_InitFlat. @return Text.
const char *ZannaFakeSteam_AppIdEnvAtInit(void) {
    return g_app_id_env_at_init;
}

/// @brief Set the EResult the next StoreStats reports. @param result EResult value.
void ZannaFakeSteam_SetStoreStatsResult(int32_t result) {
    g_store_result = result;
}

/// @brief Make later score uploads succeed or fail. @param success Nonzero for success.
void ZannaFakeSteam_SetUploadSuccess(int success) {
    g_upload_success = success ? 1 : 0;
}

/// @brief Replace a board's entries with @p count generated entries (descending scores).
/// @param name Existing board name.
/// @param count Entries to generate, clamped to the board capacity.
void ZannaFakeSteam_FillLeaderboard(const char *name, int count) {
#if !defined(ZANNA_FAKE_STEAM_CORE_ONLY)
    int index = fake_board_named(name);
    if (index < 0)
        return;
    if (count > FAKE_BOARD_ENTRY_CAPACITY)
        count = FAKE_BOARD_ENTRY_CAPACITY;
    g_boards[index].count = count < 0 ? 0 : count;
    for (int i = 0; i < g_boards[index].count; ++i) {
        g_boards[index].entries[i].steam_id = UINT64_C(76561198100000000) + (uint64_t)i;
        g_boards[index].entries[i].score = 10000 - i;
    }
#else
    (void)name;
    (void)count;
#endif
}

/// @brief Read a rich presence value.
/// @param key Presence key.
/// @return Value, or NULL when the key is not set.
const char *ZannaFakeSteam_RichPresence(const char *key) {
    for (int i = 0; key && i < FAKE_PRESENCE_KEYS; ++i) {
        if (g_presence[i].key[0] && strcmp(g_presence[i].key, key) == 0)
            return g_presence[i].value;
    }
    return NULL;
}

/// @brief Describe the last overlay or keyboard call, for example "ActivateGameOverlay(stats)".
/// @return Description, or "" when none was made since the reset.
const char *ZannaFakeSteam_LastUiCall(void) {
    return g_last_ui_call;
}

/// @brief Make the overlay report enabled or disabled. @param enabled Nonzero for enabled.
void ZannaFakeSteam_SetOverlayEnabled(int enabled) {
    g_overlay_enabled = enabled ? 1 : 0;
}

/// @brief Make both gamepad keyboards available or unavailable. @param supported Nonzero for
/// available.
void ZannaFakeSteam_SetKeyboardsSupported(int supported) {
    g_keyboards_supported = supported ? 1 : 0;
}

/// @brief Script the next full-screen text input result.
/// @param text Text the user submits.
/// @param submit Nonzero to submit, zero to cancel.
void ZannaFakeSteam_SetTextInput(const char *text, int submit) {
    snprintf(g_text_value, sizeof(g_text_value), "%s", text ? text : "");
    g_text_submit = submit ? 1 : 0;
}

/// @brief Make cloud storage enabled or disabled for the account. @param enabled Nonzero for
/// enabled.
void ZannaFakeSteam_SetCloudAccountEnabled(int enabled) {
    g_cloud_account_enabled = enabled ? 1 : 0;
}
