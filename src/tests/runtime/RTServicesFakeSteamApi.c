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
//     outcome: unset or "ok", "no-client", "version-mismatch", "generic", or
//     "not-installed" (FailedGeneric while SteamAPI_IsSteamRunning is false,
//     which is how the real redistributable reports a missing Steam client).
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
//        src/tests/runtime/RTServicesExtendedTests.cpp,
//        src/tests/runtime/RTServicesInputTests.cpp,
//        src/tests/runtime/RTServicesWorkshopTests.cpp,
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
 *          frame. The fake models three achievements (with icons that load on the
 *          dispatch frame after the first request, and global unlock percentages),
 *          three stats, leaderboard "HOME_RUNS" with three entries, rich presence,
 *          overlay requests, both gamepad keyboards, an in-memory Steam Cloud
 *          with a 1 MiB quota, Steam Input with two controllers, two action
 *          sets, a layer, four digital and two analog actions, and a Steam
 *          Workshop with three items, queries, subscriptions, downloads, and
 *          publishing.
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
    uint64_t handle;      ///< Issued SteamAPICall_t.
    int32_t callback_id;  ///< Identifier of the result structure.
    int32_t size;         ///< Size of the result structure.
    uint8_t payload[288]; ///< Result structure bytes.
    int completed;        ///< Nonzero once its SteamAPICallCompleted_t was queued.
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
    int has_icon;            ///< Nonzero when the achievement has icons.
    float global_percent;    ///< Share of players who unlocked it, 0..100.
    int icon_state[2];       ///< Per variant (locked, unlocked): 0 idle, 1 loading, 2 loaded.
    int unset_reports[2];    ///< Per variant: unset-icon reports queued on the next frame.
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

/// @brief Timeline events the fake remembers.
#define FAKE_TIMELINE_EVENT_CAPACITY 64

/// @brief One timeline event added through the fake.
typedef struct fake_timeline_event {
    uint64_t id; ///< TimelineEventHandle_t.
    int open;    ///< Nonzero for a range event not yet ended.
    int removed; ///< Nonzero after RemoveTimelineEvent.
} fake_timeline_event;

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
    {"ACH_WIN_ONE_GAME", "Winner", "Win one game", "0", 0, 0, 0, 1, 62.5f, {0, 0}, {0, 0}},
    {"ACH_HIT_FOR_CYCLE",
     "Hit for the Cycle",
     "Single, double, triple, and homer",
     "1",
     0,
     0,
     0,
     1,
     3.25f,
     {0, 0},
     {0, 0}},
    {"ACH_TRAVEL_FAR", "Globetrotter", "Run 5280 feet", "0", 0, 0, 0, 0, 18.0f, {0, 0}, {0, 0}},
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
static char g_launch_command_line[512] = "+join 76561198000000002";
static char g_launch_query[512] = "team=boston&season=1972";
static fake_timeline_event g_timeline_events[FAKE_TIMELINE_EVENT_CAPACITY];
static int g_timeline_event_count;
static uint64_t g_next_timeline_event = UINT64_C(0x7100000001);
static char g_timeline_phase_id[64];
static int g_icon_requests;
static uint32_t g_icon_width = 2;
static uint32_t g_icon_height = 2;
static int g_percentages_loaded;
static int32_t g_percentages_result = 1;
static int g_features_ready;

/// @brief Controllers the fake Steam Input models.
#define FAKE_INPUT_CONTROLLERS 2
/// @brief InputHandle_t values of the modeled controllers (above INT64_MAX on purpose).
static const uint64_t g_input_handles[FAKE_INPUT_CONTROLLERS] = {UINT64_C(18000000000000000001),
                                                                 UINT64_C(18000000000000000002)};
/// @brief ESteamInputType of each modeled controller (Xbox One, PlayStation 5).
static const int g_input_types[FAKE_INPUT_CONTROLLERS] = {3, 13};
/// @brief Action set, layer, and action handles of the modeled manifest.
#define FAKE_SET_BATTING UINT64_C(101)
#define FAKE_SET_MENU UINT64_C(102)
#define FAKE_LAYER_BUNT UINT64_C(201)
#define FAKE_DIGITAL_SWING UINT64_C(301)
#define FAKE_DIGITAL_BUNT UINT64_C(302)
#define FAKE_DIGITAL_PAUSE UINT64_C(303)
#define FAKE_DIGITAL_SELECT UINT64_C(304)
#define FAKE_ANALOG_AIM UINT64_C(401)
#define FAKE_ANALOG_CURSOR UINT64_C(402)
/// @brief Digital actions, then analog actions, of the modeled manifest.
#define FAKE_INPUT_ACTIONS 6

/// @brief Per-controller Steam Input state.
typedef struct fake_input_controller {
    int connected;                   ///< Nonzero while connected.
    int reported;                    ///< Connection state last reported through a callback.
    uint64_t action_set;             ///< Active action set handle.
    int bunt_layer;                  ///< Nonzero while the bunt layer is active.
    int pressed[FAKE_INPUT_ACTIONS]; ///< Scripted digital states by action slot.
    float x[FAKE_INPUT_ACTIONS];     ///< Scripted analog x by action slot.
    float y[FAKE_INPUT_ACTIONS];     ///< Scripted analog y by action slot.
} fake_input_controller;

static fake_input_controller g_input[FAKE_INPUT_CONTROLLERS];
static int g_input_initialized;
static int g_input_init_result = 1;
static int g_input_mapping = 1;
static int g_input_explicit_frames;
static int g_input_device_callbacks;
static int g_input_configured_pending;
static int g_input_frames;
static int g_input_handle_lookups;
static char g_input_manifest[1024];

/// @brief Workshop items the fake can hold.
#define FAKE_UGC_ITEMS 8
/// @brief Workshop queries the fake can hold open.
#define FAKE_UGC_QUERIES 16
/// @brief First id given to an item created through the fake.
#define FAKE_UGC_FIRST_CREATED_ID UINT64_C(3000000004)

/// @brief One modeled Workshop item.
typedef struct fake_ugc_item {
    uint64_t id;           ///< PublishedFileId_t, 0 when unused.
    char title[129];       ///< Title.
    char description[256]; ///< Description.
    char tags[128];        ///< Comma-separated tags.
    char metadata[128];    ///< Developer metadata.
    uint64_t owner;        ///< Author SteamID64.
    uint32_t created;      ///< Creation time.
    uint32_t updated;      ///< Last update time.
    int visibility;        ///< ERemoteStoragePublishedFileVisibility.
    uint32_t votes_up;     ///< Up votes.
    uint32_t votes_down;   ///< Down votes.
    float score;           ///< Vote score.
    uint64_t size;         ///< Content size.
    int subscribed;        ///< Nonzero when subscribed.
    int installed;         ///< Nonzero when installed.
    int needs_update;      ///< Nonzero when it needs a download.
    int downloading;       ///< Nonzero while a DownloadItem is pending.
    uint64_t downloaded;   ///< Bytes downloaded.
} fake_ugc_item;

/// @brief One open Workshop query.
typedef struct fake_ugc_query {
    uint64_t handle;                  ///< UGCQueryHandle_t, 0 when unused.
    char description[512];            ///< How it was created, for inspection.
    uint64_t results[FAKE_UGC_ITEMS]; ///< Item ids the query returns.
    uint32_t result_count;            ///< Entries in @ref results.
    char tags[256];                   ///< Required tags added so far.
    char search[128];                 ///< Search text.
    int long_description;             ///< SetReturnLongDescription value.
    int metadata;                     ///< SetReturnMetadata value.
} fake_ugc_query;

/// @brief The pending item update the fake records.
typedef struct fake_ugc_update {
    uint64_t handle;       ///< UGCUpdateHandle_t, 0 when none.
    uint64_t item;         ///< Item being updated.
    char title[160];       ///< Title set, or "" when unset.
    int has_title;         ///< Nonzero once SetItemTitle ran.
    char description[256]; ///< Description set.
    int has_description;   ///< Nonzero once SetItemDescription ran.
    char metadata[128];    ///< Metadata set.
    int has_metadata;      ///< Nonzero once SetItemMetadata ran.
    char tags[128];        ///< Tags set, joined with commas.
    int has_tags;          ///< Nonzero once SetItemTags ran.
    int visibility;        ///< Visibility set, or -1.
    char content[512];     ///< Content folder set.
    char preview[512];     ///< Preview file set.
    char note[256];        ///< Change note of the submission.
    uint64_t submit_call;  ///< Call handle of the submission, or 0.
} fake_ugc_update;

static fake_ugc_item g_ugc_items[FAKE_UGC_ITEMS];
static fake_ugc_query g_ugc_queries[FAKE_UGC_QUERIES];
static fake_ugc_update g_ugc_update;
static uint64_t g_ugc_next_query = UINT64_C(0x5100000001);
static uint64_t g_ugc_next_update = UINT64_C(0x5500000001);
static uint64_t g_ugc_next_item = FAKE_UGC_FIRST_CREATED_ID;
static char g_ugc_last_query[512];
static char g_ugc_last_update[1536];

/// @brief Distinct non-null addresses used as interface pointers.
static const char g_user_iface = 'u';
static const char g_friends_iface = 'f';
static const char g_utils_iface = 't';
static const char g_apps_iface = 'a';
static const char g_user_stats_iface = 's';
static const char g_remote_storage_iface = 'r';
static const char g_timeline_iface = 'l';
static const char g_input_iface = 'i';
static const char g_ugc_iface = 'w';

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

/// @brief Image handle of an achievement icon variant.
/// @param achievement Modeled achievement.
/// @param variant 0 for the locked icon, 1 for the unlocked icon.
/// @return Nonzero handle, unique per achievement and variant.
static int fake_icon_handle(const fake_achievement *achievement, int variant) {
    return (int)(achievement - g_achievements) * 2 + variant + 1;
}

/// @brief Reset the modeled player features to their initial state.
static void fake_reset_features(void) {
    for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        g_achievements[i].unlocked = 0;
        g_achievements[i].unlock_time = 0;
        g_achievements[i].unstored = 0;
        g_achievements[i].icon_state[0] = 0;
        g_achievements[i].icon_state[1] = 0;
        g_achievements[i].unset_reports[0] = 0;
        g_achievements[i].unset_reports[1] = 0;
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
    snprintf(g_launch_command_line, sizeof(g_launch_command_line), "+join 76561198000000002");
    snprintf(g_launch_query, sizeof(g_launch_query), "team=boston&season=1972");
    memset(g_timeline_events, 0, sizeof(g_timeline_events));
    g_timeline_event_count = 0;
    g_next_timeline_event = UINT64_C(0x7100000001);
    g_timeline_phase_id[0] = '\0';
    g_icon_requests = 0;
    g_icon_width = 2;
    g_icon_height = 2;
    g_percentages_loaded = 0;
    g_percentages_result = 1;
    memset(g_input, 0, sizeof(g_input));
    for (int i = 0; i < FAKE_INPUT_CONTROLLERS; ++i) {
        g_input[i].connected = 1;
        g_input[i].action_set = FAKE_SET_BATTING;
    }
    g_input_initialized = 0;
    g_input_init_result = 1;
    g_input_mapping = 1;
    g_input_explicit_frames = 0;
    g_input_device_callbacks = 0;
    g_input_configured_pending = 0;
    g_input_frames = 0;
    g_input_handle_lookups = 0;
    g_input_manifest[0] = '\0';
    memset(g_ugc_items, 0, sizeof(g_ugc_items));
    memset(g_ugc_queries, 0, sizeof(g_ugc_queries));
    memset(&g_ugc_update, 0, sizeof(g_ugc_update));
    g_ugc_update.visibility = -1;
    g_ugc_next_query = UINT64_C(0x5100000001);
    g_ugc_next_update = UINT64_C(0x5500000001);
    g_ugc_next_item = FAKE_UGC_FIRST_CREATED_ID;
    g_ugc_last_query[0] = '\0';
    g_ugc_last_update[0] = '\0';
    {
        fake_ugc_item *league = &g_ugc_items[0];
        league->id = UINT64_C(3000000001);
        snprintf(league->title, sizeof(league->title), "Boston 1972 League");
        snprintf(league->description,
                 sizeof(league->description),
                 "Every Boston game of the 1972 season.");
        snprintf(league->tags, sizeof(league->tags), "league,1972");
        snprintf(league->metadata, sizeof(league->metadata), "{\"format\":2}");
        league->owner = UINT64_C(76561198000000002);
        league->created = 1690000000u;
        league->updated = 1695000000u;
        league->visibility = 0;
        league->votes_up = 120u;
        league->votes_down = 4u;
        league->score = 0.9375f;
        league->size = 4096u;
        league->subscribed = 1;
        league->installed = 1;
        league->downloaded = 4096u;

        fake_ugc_item *parks = &g_ugc_items[1];
        parks->id = UINT64_C(3000000002);
        snprintf(parks->title, sizeof(parks->title), "Classic Parks Pack");
        snprintf(
            parks->description, sizeof(parks->description), "Ten ballparks from the golden age.");
        snprintf(parks->tags, sizeof(parks->tags), "parks");
        parks->owner = UINT64_C(76561198000000003);
        parks->created = 1680000000u;
        parks->updated = 1681000000u;
        parks->visibility = 0;
        parks->votes_up = 80u;
        parks->votes_down = 10u;
        parks->score = 0.8125f;
        parks->size = 2048u;
        parks->subscribed = 1;
        parks->needs_update = 1;
        parks->downloaded = 512u;

        fake_ugc_item *uniforms = &g_ugc_items[2];
        uniforms->id = UINT64_C(3000000003);
        snprintf(uniforms->title, sizeof(uniforms->title), "Night Uniforms");
        snprintf(uniforms->description,
                 sizeof(uniforms->description),
                 "Home and away uniforms for night games.");
        snprintf(uniforms->tags, sizeof(uniforms->tags), "uniforms");
        uniforms->owner = FAKE_STEAM_ID;
        uniforms->created = 1700000000u;
        uniforms->updated = 1700000500u;
        uniforms->visibility = 1;
        uniforms->votes_up = 3u;
        uniforms->votes_down = 1u;
        uniforms->score = 0.5f;
        uniforms->size = 1024u;
    }
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
    if (strcmp(scenario, "not-installed") == 0) {
        fake_write_error(err, "Steam client is not installed (fake)");
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
    g_input_initialized = 0;
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
/// @return False for the "no-client" and "not-installed" scenarios, otherwise true.
bool SteamAPI_IsSteamRunning(void) {
    const char *scenario = fake_scenario();
    return strcmp(scenario, "no-client") != 0 && strcmp(scenario, "not-installed") != 0;
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

/// @brief Fake SteamAPI_ManualDispatch_RunFrame: queues scripted, keyboard, icon, and call
///        callbacks.
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
    for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        for (int variant = 0; variant < 2; ++variant) {
            fake_achievement *achievement = &g_achievements[i];
            int reports = achievement->unset_reports[variant];
            achievement->unset_reports[variant] = 0;
            if (achievement->icon_state[variant] == 1) {
                achievement->icon_state[variant] = 2;
                reports = 1;
            }
            for (int report = 0; report < reports; ++report) {
                uint8_t payload[144];
                memset(payload, 0, sizeof(payload));
                fake_put_u64(payload, 0, fake_app_id());
                snprintf((char *)payload + 8, 128, "%s", achievement->id);
                payload[136] = (uint8_t)variant;
                fake_put_u32(
                    payload,
                    140,
                    achievement->has_icon ? (uint32_t)fake_icon_handle(achievement, variant) : 0u);
                fake_queue(1109, payload, 144);
            }
        }
    }
    for (int i = 0; i < FAKE_UGC_ITEMS; ++i) {
        fake_ugc_item *item = &g_ugc_items[i];
        if (!item->id || !item->downloading)
            continue;
        uint8_t payload[32];
        item->downloading = 0;
        item->installed = 1;
        item->needs_update = 0;
        item->downloaded = item->size;
        memset(payload, 0, sizeof(payload));
        fake_put_u32(payload, 0, fake_app_id());
        fake_put_u64(payload, FAKE_PACK8 ? 8 : 4, item->id);
        fake_queue(3405, payload, FAKE_PACK8 ? 32 : 28);
        memset(payload, 0, sizeof(payload));
        fake_put_u32(payload, 0, fake_app_id());
        fake_put_u64(payload, FAKE_PACK8 ? 8 : 4, item->id);
        fake_put_u32(payload, FAKE_PACK8 ? 16 : 12, 1u);
        fake_queue(3406, payload, FAKE_PACK8 ? 24 : 16);
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
        if (entry->callback_id == 1110 && g_percentages_result == 1)
            g_percentages_loaded = 1;
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
// ISteamApps: launch parameters
//===----------------------------------------------------------------------===//

/// @brief Fake ISteamApps::GetLaunchQueryParam over the scripted "key=value&..." query.
/// @details Keys starting with '@' are reserved and read as "", as in Steam.
const char *SteamAPI_ISteamApps_GetLaunchQueryParam(void *self, const char *key) {
    static char value[512];
    (void)self;
    value[0] = '\0';
    if (!key || !*key || key[0] == '@')
        return value;
    const size_t key_len = strlen(key);
    for (const char *p = g_launch_query; *p;) {
        const char *end = strchr(p, '&');
        size_t pair_len = end ? (size_t)(end - p) : strlen(p);
        if (pair_len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t value_len = pair_len - key_len - 1;
            if (value_len >= sizeof(value))
                value_len = sizeof(value) - 1;
            memcpy(value, p + key_len + 1, value_len);
            value[value_len] = '\0';
            return value;
        }
        p += pair_len + (end ? 1 : 0);
    }
    return value;
}

/// @brief Fake ISteamApps::GetDLCCount. @return 2.
int SteamAPI_ISteamApps_GetDLCCount(void *self) {
    (void)self;
    return 2;
}

/// @brief Fake ISteamApps::BGetDLCDataByIndex over two modeled DLC.
bool SteamAPI_ISteamApps_BGetDLCDataByIndex(
    void *self, int index, uint32_t *app_id, bool *available, char *name, int name_capacity) {
    (void)self;

    static const struct {
        uint32_t id;
        bool available;
        const char *name;
    } dlc[] = {{FAKE_INSTALLED_DLC, true, "Stadium Pack"},
               {UINT32_C(7654321), false, "Classic Uniforms"}};

    if (index < 0 || index >= 2 || !app_id || !available || !name || name_capacity <= 0)
        return false;
    *app_id = dlc[index].id;
    *available = dlc[index].available;
    snprintf(name, (size_t)name_capacity, "%s", dlc[index].name);
    return true;
}

/// @brief Fake ISteamApps::GetAppBuildId. @return 21042026.
int SteamAPI_ISteamApps_GetAppBuildId(void *self) {
    (void)self;
    return 21042026;
}

/// @brief Fake ISteamApps::GetCurrentBetaName; reports the "public-beta" branch.
bool SteamAPI_ISteamApps_GetCurrentBetaName(void *self, char *name, int name_capacity) {
    (void)self;
    if (!name || name_capacity <= 0)
        return false;
    snprintf(name, (size_t)name_capacity, "public-beta");
    return true;
}

/// @brief Fake ISteamApps::GetLaunchCommandLine; returns the bytes copied, terminator included.
int SteamAPI_ISteamApps_GetLaunchCommandLine(void *self, char *buffer, int capacity) {
    (void)self;
    if (!buffer || capacity <= 0)
        return 0;
    size_t len = strlen(g_launch_command_line);
    if (len >= (size_t)capacity)
        len = (size_t)capacity - 1;
    memcpy(buffer, g_launch_command_line, len);
    buffer[len] = '\0';
    return len > 0 ? (int)len + 1 : 0;
}

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

/// @brief Fake ISteamUserStats::StoreStats; queues new unlocks, then UserStatsStored_t.
/// @details The Steam client posts UserAchievementStored_t for each new unlock
///          before UserStatsStored_t, and the fake keeps that order.
bool SteamAPI_ISteamUserStats_StoreStats(void *self) {
    (void)self;
    for (size_t i = 0; i < sizeof(g_achievements) / sizeof(g_achievements[0]); ++i) {
        if (!g_achievements[i].unstored)
            continue;
        g_achievements[i].unstored = 0;
        fake_queue_achievement_stored(g_achievements[i].id, 0, 0);
    }
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, fake_app_id());
    fake_put_u32(payload, 8, (uint32_t)g_store_result);
    fake_queue(1102, payload, FAKE_PACK8 ? 16 : 12);
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
// ISteamUserStats: achievement icons and global percentages
//===----------------------------------------------------------------------===//

/// @brief Fake ISteamUserStats::GetAchievementIcon for the achievement's current state.
/// @details The first request for a variant starts loading it and returns 0;
///          the next dispatch frame posts UserAchievementIconFetched_t, after
///          which the handle is returned. As in Steam, every request for an
///          unset icon returns 0 and queues another report with handle 0.
int SteamAPI_ISteamUserStats_GetAchievementIcon(void *self, const char *name) {
    (void)self;
    g_icon_requests++;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!achievement)
        return 0;
    const int variant = achievement->unlocked ? 1 : 0;
    if (!achievement->has_icon) {
        achievement->unset_reports[variant]++;
        return 0;
    }
    if (achievement->icon_state[variant] == 0) {
        achievement->icon_state[variant] = 1;
        return 0;
    }
    if (achievement->icon_state[variant] != 2)
        return 0;
    return fake_icon_handle(achievement, variant);
}

/// @brief Find the achievement variant an icon handle names.
/// @param image Image handle.
/// @return Achievement with a loaded icon for that handle, or NULL.
static fake_achievement *fake_icon_owner(int image) {
    const int count = (int)(sizeof(g_achievements) / sizeof(g_achievements[0]));
    if (image < 1 || image > count * 2)
        return NULL;
    fake_achievement *achievement = &g_achievements[(image - 1) / 2];
    const int variant = (image - 1) % 2;
    return (achievement->has_icon && achievement->icon_state[variant] == 2) ? achievement : NULL;
}

/// @brief Fake ISteamUtils::GetImageSize over the modeled icon size (2x2 unless scripted).
bool SteamAPI_ISteamUtils_GetImageSize(void *self, int image, uint32_t *width, uint32_t *height) {
    (void)self;
    if (!fake_icon_owner(image) || !width || !height)
        return false;
    *width = g_icon_width;
    *height = g_icon_height;
    return true;
}

/// @brief Fake ISteamUtils::GetImageRGBA; byte i of image h holds (h * 16 + i) & 0xFF.
bool SteamAPI_ISteamUtils_GetImageRGBA(void *self, int image, uint8_t *dest, int dest_size) {
    (void)self;
    const uint64_t needed = (uint64_t)g_icon_width * (uint64_t)g_icon_height * 4u;
    if (!fake_icon_owner(image) || !dest || dest_size < 0 || (uint64_t)dest_size < needed)
        return false;
    for (uint64_t i = 0; i < needed; ++i)
        dest[i] = (uint8_t)(((uint64_t)image * 16u + i) & 0xFFu);
    return true;
}

/// @brief Fake ISteamUserStats::RequestGlobalAchievementPercentages; completes on the next frame.
/// @details The percentages become readable once the call result is fetched
///          with EResult 1 (see ZannaFakeSteam_SetGlobalPercentagesResult).
uint64_t SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages(void *self) {
    (void)self;
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, fake_app_id());
    fake_put_u32(payload, 8, (uint32_t)g_percentages_result);
    return fake_issue_call(1110, payload, FAKE_PACK8 ? 16 : 12);
}

/// @brief Fake ISteamUserStats::GetAchievementAchievedPercent; false until percentages load.
bool SteamAPI_ISteamUserStats_GetAchievementAchievedPercent(void *self,
                                                            const char *name,
                                                            float *percent) {
    (void)self;
    fake_achievement *achievement = fake_find_achievement(name);
    if (!g_percentages_loaded || !achievement || !percent)
        return false;
    *percent = achievement->global_percent;
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

//===----------------------------------------------------------------------===//
// ISteamTimeline
//===----------------------------------------------------------------------===//

/// @brief Fake SteamAPI_SteamTimeline_v004. @return ISteamTimeline stand-in.
void *SteamAPI_SteamTimeline_v004(void) {
    return (void *)&g_timeline_iface;
}

/// @brief Remember a new timeline event.
/// @param open Nonzero for an open range event.
/// @return New TimelineEventHandle_t, or 0 when the fake is full.
static uint64_t fake_timeline_add(int open) {
    if (g_timeline_event_count == FAKE_TIMELINE_EVENT_CAPACITY)
        return 0;
    fake_timeline_event *event = &g_timeline_events[g_timeline_event_count++];
    event->id = g_next_timeline_event++;
    event->open = open;
    event->removed = 0;
    return event->id;
}

/// @brief Find a timeline event by handle.
/// @param id TimelineEventHandle_t.
/// @return Event, or NULL.
static fake_timeline_event *fake_timeline_find(uint64_t id) {
    for (int i = 0; i < g_timeline_event_count; ++i) {
        if (g_timeline_events[i].id == id)
            return &g_timeline_events[i];
    }
    return NULL;
}

/// @brief Fake ISteamTimeline::SetTimelineTooltip; records the call.
void SteamAPI_ISteamTimeline_SetTimelineTooltip(void *self, const char *description, float delta) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "SetTimelineTooltip(%s,%.1f)",
             description,
             (double)delta);
}

/// @brief Fake ISteamTimeline::ClearTimelineTooltip; records the call.
void SteamAPI_ISteamTimeline_ClearTimelineTooltip(void *self, float delta) {
    (void)self;
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "ClearTimelineTooltip(%.1f)", (double)delta);
}

/// @brief Fake ISteamTimeline::SetTimelineGameMode; records the call.
void SteamAPI_ISteamTimeline_SetTimelineGameMode(void *self, int mode) {
    (void)self;
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "SetTimelineGameMode(%d)", mode);
}

/// @brief Fake ISteamTimeline::AddInstantaneousTimelineEvent; records the call.
uint64_t SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent(void *self,
                                                               const char *title,
                                                               const char *description,
                                                               const char *icon,
                                                               uint32_t priority,
                                                               float offset,
                                                               int clip) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "AddInstantaneousTimelineEvent(%s,%s,%s,%u,%.1f,%d)",
             title,
             description,
             icon,
             (unsigned)priority,
             (double)offset,
             clip);
    return fake_timeline_add(0);
}

/// @brief Fake ISteamTimeline::AddRangeTimelineEvent; records the call.
uint64_t SteamAPI_ISteamTimeline_AddRangeTimelineEvent(void *self,
                                                       const char *title,
                                                       const char *description,
                                                       const char *icon,
                                                       uint32_t priority,
                                                       float offset,
                                                       float duration,
                                                       int clip) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "AddRangeTimelineEvent(%s,%s,%s,%u,%.1f,%.1f,%d)",
             title,
             description,
             icon,
             (unsigned)priority,
             (double)offset,
             (double)duration,
             clip);
    return fake_timeline_add(0);
}

/// @brief Fake ISteamTimeline::StartRangeTimelineEvent; records the call.
uint64_t SteamAPI_ISteamTimeline_StartRangeTimelineEvent(void *self,
                                                         const char *title,
                                                         const char *description,
                                                         const char *icon,
                                                         uint32_t priority,
                                                         float offset,
                                                         int clip) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "StartRangeTimelineEvent(%s,%s,%s,%u,%.1f,%d)",
             title,
             description,
             icon,
             (unsigned)priority,
             (double)offset,
             clip);
    return fake_timeline_add(1);
}

/// @brief Fake ISteamTimeline::UpdateRangeTimelineEvent; records the call.
void SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent(void *self,
                                                      uint64_t event,
                                                      const char *title,
                                                      const char *description,
                                                      const char *icon,
                                                      uint32_t priority,
                                                      int clip) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "UpdateRangeTimelineEvent(%llu,%s,%s,%s,%u,%d)",
             (unsigned long long)event,
             title,
             description,
             icon,
             (unsigned)priority,
             clip);
}

/// @brief Fake ISteamTimeline::EndRangeTimelineEvent; closes the event and records the call.
void SteamAPI_ISteamTimeline_EndRangeTimelineEvent(void *self, uint64_t event, float offset) {
    (void)self;
    fake_timeline_event *found = fake_timeline_find(event);
    if (found)
        found->open = 0;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "EndRangeTimelineEvent(%llu,%.1f)",
             (unsigned long long)event,
             (double)offset);
}

/// @brief Fake ISteamTimeline::RemoveTimelineEvent; records the call.
void SteamAPI_ISteamTimeline_RemoveTimelineEvent(void *self, uint64_t event) {
    (void)self;
    fake_timeline_event *found = fake_timeline_find(event);
    if (found)
        found->removed = 1;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "RemoveTimelineEvent(%llu)",
             (unsigned long long)event);
}

/// @brief Fake ISteamTimeline::DoesEventRecordingExist; exists for known, unremoved events.
uint64_t SteamAPI_ISteamTimeline_DoesEventRecordingExist(void *self, uint64_t event) {
    (void)self;
    const fake_timeline_event *found = fake_timeline_find(event);
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, event);
    payload[8] = (found && !found->removed) ? 1 : 0;
    return fake_issue_call(6002, payload, FAKE_PACK8 ? 16 : 12);
}

/// @brief Fake ISteamTimeline::StartGamePhase; forgets the phase id and records the call.
void SteamAPI_ISteamTimeline_StartGamePhase(void *self) {
    (void)self;
    g_timeline_phase_id[0] = '\0';
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "StartGamePhase()");
}

/// @brief Fake ISteamTimeline::EndGamePhase; records the call.
void SteamAPI_ISteamTimeline_EndGamePhase(void *self) {
    (void)self;
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "EndGamePhase()");
}

/// @brief Fake ISteamTimeline::SetGamePhaseID; remembers the id and records the call.
void SteamAPI_ISteamTimeline_SetGamePhaseID(void *self, const char *phase_id) {
    (void)self;
    snprintf(g_timeline_phase_id, sizeof(g_timeline_phase_id), "%s", phase_id ? phase_id : "");
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "SetGamePhaseID(%s)", g_timeline_phase_id);
}

/// @brief Fake ISteamTimeline::DoesGamePhaseRecordingExist; the phase set last has a recording.
uint64_t SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist(void *self, const char *phase_id) {
    (void)self;
    const int recorded =
        phase_id && g_timeline_phase_id[0] && strcmp(phase_id, g_timeline_phase_id) == 0;
    uint8_t payload[88];
    memset(payload, 0, sizeof(payload));
    snprintf((char *)payload, 64, "%s", phase_id ? phase_id : "");
    fake_put_u64(payload, 64, recorded ? UINT64_C(90000) : 0);
    fake_put_u64(payload, 72, recorded ? UINT64_C(30000) : 0);
    fake_put_u32(payload, 80, recorded ? 2u : 0u);
    fake_put_u32(payload, 84, recorded ? 1u : 0u);
    return fake_issue_call(6001, payload, 88);
}

/// @brief Fake ISteamTimeline::AddGamePhaseTag; records the call.
void SteamAPI_ISteamTimeline_AddGamePhaseTag(
    void *self, const char *name, const char *icon, const char *group, uint32_t priority) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "AddGamePhaseTag(%s,%s,%s,%u)",
             name,
             icon,
             group,
             (unsigned)priority);
}

/// @brief Fake ISteamTimeline::SetGamePhaseAttribute; records the call.
void SteamAPI_ISteamTimeline_SetGamePhaseAttribute(void *self,
                                                   const char *group,
                                                   const char *value,
                                                   uint32_t priority) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "SetGamePhaseAttribute(%s,%s,%u)",
             group,
             value,
             (unsigned)priority);
}

/// @brief Fake ISteamTimeline::OpenOverlayToGamePhase; records the call.
void SteamAPI_ISteamTimeline_OpenOverlayToGamePhase(void *self, const char *phase_id) {
    (void)self;
    snprintf(g_last_ui_call, sizeof(g_last_ui_call), "OpenOverlayToGamePhase(%s)", phase_id);
}

/// @brief Fake ISteamTimeline::OpenOverlayToTimelineEvent; records the call.
void SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent(void *self, uint64_t event) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "OpenOverlayToTimelineEvent(%llu)",
             (unsigned long long)event);
}

//===----------------------------------------------------------------------===//
// ISteamInput
//===----------------------------------------------------------------------===//

#pragma pack(push, 1)

/// @brief InputDigitalActionData_t as the SDK declares it (one-byte packing).
typedef struct fake_input_digital_data {
    bool state;  ///< The action is pressed.
    bool active; ///< The action is available in the active set.
} fake_input_digital_data;

/// @brief InputAnalogActionData_t as the SDK declares it (one-byte packing).
typedef struct fake_input_analog_data {
    int32_t mode; ///< EInputSourceMode.
    float x;      ///< Horizontal value.
    float y;      ///< Vertical value.
    bool active;  ///< The action is available in the active set.
} fake_input_analog_data;

#pragma pack(pop)

#if defined(ZANNA_FAKE_STEAM_PROFILE_164)
/// @brief Fake SteamAPI_SteamInput_v006. @return ISteamInput stand-in.
void *SteamAPI_SteamInput_v006(void) {
    return (void *)&g_input_iface;
}
#else
/// @brief Fake SteamAPI_SteamInput_v007. @return ISteamInput stand-in.
void *SteamAPI_SteamInput_v007(void) {
    return (void *)&g_input_iface;
}
#endif

/// @brief Find a modeled controller.
/// @param handle InputHandle_t.
/// @return Index, or -1.
static int fake_input_index(uint64_t handle) {
    for (int i = 0; i < FAKE_INPUT_CONTROLLERS; ++i) {
        if (g_input_handles[i] == handle)
            return i;
    }
    return -1;
}

/// @brief Map an action handle to its state slot.
/// @param action Digital or analog action handle.
/// @return Slot in 0..FAKE_INPUT_ACTIONS-1, or -1.
static int fake_input_slot(uint64_t action) {
    switch (action) {
        case FAKE_DIGITAL_SWING:
            return 0;
        case FAKE_DIGITAL_BUNT:
            return 1;
        case FAKE_DIGITAL_PAUSE:
            return 2;
        case FAKE_DIGITAL_SELECT:
            return 3;
        case FAKE_ANALOG_AIM:
            return 4;
        case FAKE_ANALOG_CURSOR:
            return 5;
        default:
            return -1;
    }
}

/// @brief Report whether an action belongs to an action set or the bunt layer.
/// @param action Action handle.
/// @param set Action set handle.
/// @param bunt_layer Nonzero when the bunt layer counts.
/// @return 1 when available, otherwise 0.
static int fake_input_in_set(uint64_t action, uint64_t set, int bunt_layer) {
    switch (action) {
        case FAKE_DIGITAL_SWING:
        case FAKE_ANALOG_AIM:
            return set == FAKE_SET_BATTING;
        case FAKE_DIGITAL_BUNT:
            return set == FAKE_SET_BATTING && bunt_layer;
        case FAKE_DIGITAL_PAUSE:
            return set == FAKE_SET_BATTING || set == FAKE_SET_MENU;
        case FAKE_DIGITAL_SELECT:
        case FAKE_ANALOG_CURSOR:
            return set == FAKE_SET_MENU;
        default:
            return 0;
    }
}

/// @brief Apply an operation to one controller or, for the all-controllers handle, to every one.
/// @param handle InputHandle_t or STEAM_INPUT_HANDLE_ALL_CONTROLLERS.
/// @param first Receives the first index to touch.
/// @param last Receives one past the last index to touch.
static void fake_input_range(uint64_t handle, int *first, int *last) {
    if (handle == UINT64_MAX) {
        *first = 0;
        *last = FAKE_INPUT_CONTROLLERS;
        return;
    }
    const int index = fake_input_index(handle);
    *first = index < 0 ? 0 : index;
    *last = index < 0 ? 0 : index + 1;
}

/// @brief Queue a SteamInputDeviceConnected_t or SteamInputDeviceDisconnected_t.
/// @param index Controller index.
/// @param connected Nonzero for a connection.
static void fake_input_queue_device(int index, int connected) {
    uint8_t payload[8];
    fake_put_u64(payload, 0, g_input_handles[index]);
    fake_queue(connected ? 2801 : 2802, payload, 8);
}

/// @brief Queue a SteamInputConfigurationLoaded_t at explicit offsets.
/// @param index Controller index.
static void fake_input_queue_configuration(int index) {
    uint8_t payload[40];
    memset(payload, 0, sizeof(payload));
    fake_put_u32(payload, 0, fake_app_id());
    fake_put_u64(payload, FAKE_PACK8 ? 8 : 4, g_input_handles[index]);
    fake_put_u64(payload, FAKE_PACK8 ? 16 : 12, FAKE_STEAM_ID);
    fake_put_u32(payload, FAKE_PACK8 ? 24 : 20, 3u);
    fake_put_u32(payload, FAKE_PACK8 ? 28 : 24, 1u);
    payload[FAKE_PACK8 ? 32 : 28] = 1;
    payload[FAKE_PACK8 ? 33 : 29] = 0;
    fake_queue(2803, payload, FAKE_PACK8 ? 40 : 32);
}

/// @brief Fake ISteamInput::Init.
bool SteamAPI_ISteamInput_Init(void *self, bool explicitly_call_run_frame) {
    (void)self;
    if (!g_input_init_result)
        return false;
    g_input_initialized = 1;
    g_input_explicit_frames = explicitly_call_run_frame ? 1 : 0;
    for (int i = 0; i < FAKE_INPUT_CONTROLLERS; ++i)
        g_input[i].reported = 0;
    return true;
}

/// @brief Fake ISteamInput::Shutdown.
bool SteamAPI_ISteamInput_Shutdown(void *self) {
    (void)self;
    g_input_initialized = 0;
    g_input_device_callbacks = 0;
    return true;
}

/// @brief Fake ISteamInput::SetInputActionManifestFilePath.
/// @details Accepts a readable file whose first 512 bytes contain "Action
///          Manifest" while the app has a controller mapping. Like Steam, the
///          fake gains the mapping when a controller connects after
///          ZannaFakeSteam_SetInputMapping(0).
bool SteamAPI_ISteamInput_SetInputActionManifestFilePath(void *self, const char *path) {
    (void)self;
    if (!g_input_mapping)
        return false;
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file)
        return false;
    char head[513];
    const size_t read = fread(head, 1, sizeof(head) - 1, file);
    fclose(file);
    head[read] = '\0';
    if (!strstr(head, "Action Manifest"))
        return false;
    snprintf(g_input_manifest, sizeof(g_input_manifest), "%s", path);
    g_input_configured_pending = 1;
    return true;
}

/// @brief Fake ISteamInput::RunFrame; posts device and configuration callbacks.
void SteamAPI_ISteamInput_RunFrame(void *self, bool reserved) {
    (void)self;
    (void)reserved;
    if (!g_input_initialized)
        return;
    g_input_frames++;
    if (g_input_device_callbacks) {
        for (int i = 0; i < FAKE_INPUT_CONTROLLERS; ++i) {
            if (g_input[i].connected != g_input[i].reported) {
                g_input[i].reported = g_input[i].connected;
                fake_input_queue_device(i, g_input[i].connected);
            }
        }
    }
    if (g_input_configured_pending) {
        g_input_configured_pending = 0;
        for (int i = 0; i < FAKE_INPUT_CONTROLLERS; ++i) {
            if (g_input[i].connected)
                fake_input_queue_configuration(i);
        }
    }
}

/// @brief Fake ISteamInput::BWaitForData (unused by the provider). @return False.
bool SteamAPI_ISteamInput_BWaitForData(void *self, bool wait_forever, uint32_t timeout) {
    (void)self;
    (void)wait_forever;
    (void)timeout;
    return false;
}

/// @brief Fake ISteamInput::GetConnectedControllers.
int SteamAPI_ISteamInput_GetConnectedControllers(void *self, uint64_t *handles) {
    (void)self;
    int count = 0;
    for (int i = 0; g_input_initialized && i < FAKE_INPUT_CONTROLLERS; ++i) {
        if (g_input[i].connected)
            handles[count++] = g_input_handles[i];
    }
    return count;
}

/// @brief Fake ISteamInput::EnableDeviceCallbacks.
void SteamAPI_ISteamInput_EnableDeviceCallbacks(void *self) {
    (void)self;
    g_input_device_callbacks = 1;
}

/// @brief Fake ISteamInput::GetActionSetHandle for sets and layers.
uint64_t SteamAPI_ISteamInput_GetActionSetHandle(void *self, const char *name) {
    (void)self;
    g_input_handle_lookups++;
    if (!g_input_manifest[0] || !name)
        return 0;
    if (strcmp(name, "batting") == 0)
        return FAKE_SET_BATTING;
    if (strcmp(name, "menu") == 0)
        return FAKE_SET_MENU;
    return strcmp(name, "bunt_layer") == 0 ? FAKE_LAYER_BUNT : 0;
}

/// @brief Fake ISteamInput::ActivateActionSet.
void SteamAPI_ISteamInput_ActivateActionSet(void *self, uint64_t controller, uint64_t set) {
    (void)self;
    int first = 0;
    int last = 0;
    fake_input_range(controller, &first, &last);
    for (int i = first; i < last; ++i)
        g_input[i].action_set = set;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "ActivateActionSet(%llu,%llu)",
             (unsigned long long)controller,
             (unsigned long long)set);
}

/// @brief Fake ISteamInput::GetCurrentActionSet.
uint64_t SteamAPI_ISteamInput_GetCurrentActionSet(void *self, uint64_t controller) {
    (void)self;
    const int index = fake_input_index(controller);
    return index < 0 ? 0 : g_input[index].action_set;
}

/// @brief Set the bunt layer state for a controller range.
/// @param controller InputHandle_t or all controllers.
/// @param layer Layer handle.
/// @param active Nonzero to activate.
static void fake_input_layer(uint64_t controller, uint64_t layer, int active) {
    int first = 0;
    int last = 0;
    fake_input_range(controller, &first, &last);
    for (int i = first; i < last; ++i) {
        if (layer == FAKE_LAYER_BUNT)
            g_input[i].bunt_layer = active;
    }
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "%sActionSetLayer(%llu,%llu)",
             active ? "Activate" : "Deactivate",
             (unsigned long long)controller,
             (unsigned long long)layer);
}

/// @brief Fake ISteamInput::ActivateActionSetLayer.
void SteamAPI_ISteamInput_ActivateActionSetLayer(void *self, uint64_t controller, uint64_t layer) {
    (void)self;
    fake_input_layer(controller, layer, 1);
}

/// @brief Fake ISteamInput::DeactivateActionSetLayer.
void SteamAPI_ISteamInput_DeactivateActionSetLayer(void *self,
                                                   uint64_t controller,
                                                   uint64_t layer) {
    (void)self;
    fake_input_layer(controller, layer, 0);
}

/// @brief Fake ISteamInput::DeactivateAllActionSetLayers.
void SteamAPI_ISteamInput_DeactivateAllActionSetLayers(void *self, uint64_t controller) {
    (void)self;
    int first = 0;
    int last = 0;
    fake_input_range(controller, &first, &last);
    for (int i = first; i < last; ++i)
        g_input[i].bunt_layer = 0;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "DeactivateAllActionSetLayers(%llu)",
             (unsigned long long)controller);
}

/// @brief Fake ISteamInput::GetDigitalActionHandle.
uint64_t SteamAPI_ISteamInput_GetDigitalActionHandle(void *self, const char *name) {
    (void)self;
    g_input_handle_lookups++;
    if (!g_input_manifest[0] || !name)
        return 0;
    if (strcmp(name, "swing") == 0)
        return FAKE_DIGITAL_SWING;
    if (strcmp(name, "bunt") == 0)
        return FAKE_DIGITAL_BUNT;
    if (strcmp(name, "pause") == 0)
        return FAKE_DIGITAL_PAUSE;
    return strcmp(name, "select") == 0 ? FAKE_DIGITAL_SELECT : 0;
}

/// @brief Fake ISteamInput::GetDigitalActionData; returned by value.
fake_input_digital_data SteamAPI_ISteamInput_GetDigitalActionData(void *self,
                                                                  uint64_t controller,
                                                                  uint64_t action) {
    (void)self;
    fake_input_digital_data data = {false, false};
    const int index = fake_input_index(controller);
    const int slot = fake_input_slot(action);
    if (index < 0 || slot < 0 || slot >= 4 || !g_input[index].connected)
        return data;
    data.active = fake_input_in_set(action, g_input[index].action_set, g_input[index].bunt_layer);
    data.state = data.active && g_input[index].pressed[slot];
    return data;
}

/// @brief Write the modeled origins of an action for a controller.
/// @param index Controller index.
/// @param action Action handle.
/// @param origins Destination of 8 EInputActionOrigin values.
/// @return Number written.
static int fake_input_origins(int index, uint64_t action, int32_t *origins) {
    // Controller 0 reports Xbox One origins and controller 1 PlayStation 5 origins.
    const int32_t base = index == 0 ? 114 : 258;
    switch (action) {
        case FAKE_DIGITAL_SWING:
            origins[0] = base;
            return 1;
        case FAKE_DIGITAL_BUNT:
            origins[0] = base + 1;
            return 1;
        case FAKE_DIGITAL_PAUSE:
            origins[0] = base + 8;
            return 1;
        case FAKE_DIGITAL_SELECT:
            origins[0] = base;
            return 1;
        case FAKE_ANALOG_AIM:
            origins[0] = base + 16;
            origins[1] = base + 17;
            return 2;
        case FAKE_ANALOG_CURSOR:
            origins[0] = base + 16;
            return 1;
        default:
            return 0;
    }
}

/// @brief Shared body of Get*ActionOrigins.
static int fake_input_action_origins(uint64_t controller,
                                     uint64_t set,
                                     uint64_t action,
                                     int32_t *origins) {
    const int index = fake_input_index(controller);
    if (index < 0 || !origins || !fake_input_in_set(action, set, g_input[index].bunt_layer))
        return 0;
    return fake_input_origins(index, action, origins);
}

/// @brief Fake ISteamInput::GetDigitalActionOrigins.
int SteamAPI_ISteamInput_GetDigitalActionOrigins(
    void *self, uint64_t controller, uint64_t set, uint64_t action, int32_t *origins) {
    (void)self;
    return fake_input_slot(action) >= 0 && fake_input_slot(action) < 4
               ? fake_input_action_origins(controller, set, action, origins)
               : 0;
}

/// @brief Fake ISteamInput::GetStringForDigitalActionName.
const char *SteamAPI_ISteamInput_GetStringForDigitalActionName(void *self, uint64_t action) {
    (void)self;
    switch (action) {
        case FAKE_DIGITAL_SWING:
            return "Swing";
        case FAKE_DIGITAL_BUNT:
            return "Bunt";
        case FAKE_DIGITAL_PAUSE:
            return "Pause";
        case FAKE_DIGITAL_SELECT:
            return "Select";
        default:
            return "";
    }
}

/// @brief Fake ISteamInput::GetAnalogActionHandle.
uint64_t SteamAPI_ISteamInput_GetAnalogActionHandle(void *self, const char *name) {
    (void)self;
    g_input_handle_lookups++;
    if (!g_input_manifest[0] || !name)
        return 0;
    if (strcmp(name, "aim") == 0)
        return FAKE_ANALOG_AIM;
    return strcmp(name, "cursor") == 0 ? FAKE_ANALOG_CURSOR : 0;
}

/// @brief Fake ISteamInput::GetAnalogActionData; returned by value.
fake_input_analog_data SteamAPI_ISteamInput_GetAnalogActionData(void *self,
                                                                uint64_t controller,
                                                                uint64_t action) {
    (void)self;
    fake_input_analog_data data = {0, 0.0f, 0.0f, false};
    const int index = fake_input_index(controller);
    const int slot = fake_input_slot(action);
    if (index < 0 || slot < 4 || !g_input[index].connected)
        return data;
    data.mode = action == FAKE_ANALOG_AIM ? 6 : 5;
    data.active = fake_input_in_set(action, g_input[index].action_set, g_input[index].bunt_layer);
    if (data.active) {
        data.x = g_input[index].x[slot];
        data.y = g_input[index].y[slot];
    }
    return data;
}

/// @brief Fake ISteamInput::GetAnalogActionOrigins.
int SteamAPI_ISteamInput_GetAnalogActionOrigins(
    void *self, uint64_t controller, uint64_t set, uint64_t action, int32_t *origins) {
    (void)self;
    return fake_input_slot(action) >= 4
               ? fake_input_action_origins(controller, set, action, origins)
               : 0;
}

/// @brief Fake ISteamInput::GetStringForAnalogActionName.
const char *SteamAPI_ISteamInput_GetStringForAnalogActionName(void *self, uint64_t action) {
    (void)self;
    if (action == FAKE_ANALOG_AIM)
        return "Aim";
    return action == FAKE_ANALOG_CURSOR ? "Cursor" : "";
}

/// @brief Fake ISteamInput::GetGlyphPNGForActionOrigin; a path naming the origin and size.
/// @details Joins part of the path with backslashes, as Steam for macOS does.
const char *SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin(void *self,
                                                            int origin,
                                                            int size,
                                                            uint32_t flags) {
    static char path[128];
    (void)self;
    snprintf(path,
             sizeof(path),
             "/fake/steam\\controller_base\\glyphs/origin_%d_size_%d_flags_%u.png",
             origin,
             size,
             (unsigned)flags);
    return path;
}

/// @brief Fake ISteamInput::GetStringForActionOrigin.
const char *SteamAPI_ISteamInput_GetStringForActionOrigin(void *self, int origin) {
    static char name[64];
    (void)self;
    if (origin == 114)
        return "A Button";
    if (origin == 258)
        return "Cross Button";
    snprintf(name, sizeof(name), "Origin %d", origin);
    return name;
}

/// @brief Fake ISteamInput::TriggerVibration; records the call.
void SteamAPI_ISteamInput_TriggerVibration(void *self,
                                           uint64_t controller,
                                           unsigned short left,
                                           unsigned short right) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "TriggerVibration(%llu,%u,%u)",
             (unsigned long long)controller,
             (unsigned)left,
             (unsigned)right);
}

/// @brief Fake ISteamInput::SetLEDColor; records the call.
void SteamAPI_ISteamInput_SetLEDColor(
    void *self, uint64_t controller, uint8_t red, uint8_t green, uint8_t blue, unsigned flags) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "SetLEDColor(%llu,%u,%u,%u,%u)",
             (unsigned long long)controller,
             (unsigned)red,
             (unsigned)green,
             (unsigned)blue,
             flags);
}

/// @brief Fake ISteamInput::ShowBindingPanel; succeeds while the overlay is enabled.
bool SteamAPI_ISteamInput_ShowBindingPanel(void *self, uint64_t controller) {
    (void)self;
    snprintf(g_last_ui_call,
             sizeof(g_last_ui_call),
             "ShowBindingPanel(%llu)",
             (unsigned long long)controller);
    return g_overlay_enabled && fake_input_index(controller) >= 0;
}

/// @brief Fake ISteamInput::GetInputTypeForHandle.
int SteamAPI_ISteamInput_GetInputTypeForHandle(void *self, uint64_t controller) {
    (void)self;
    const int index = fake_input_index(controller);
    return index < 0 ? 0 : g_input_types[index];
}

/// @brief Fake ISteamInput::GetGamepadIndexForController.
int SteamAPI_ISteamInput_GetGamepadIndexForController(void *self, uint64_t controller) {
    (void)self;
    const int index = fake_input_index(controller);
    return (index < 0 || !g_input[index].connected) ? -1 : index;
}

//===----------------------------------------------------------------------===//
// ISteamUGC
//===----------------------------------------------------------------------===//

#if defined(ZANNA_FAKE_STEAM_PROFILE_164)
/// @brief Fake SteamAPI_SteamUGC_v020. @return ISteamUGC stand-in.
void *SteamAPI_SteamUGC_v020(void) {
    return (void *)&g_ugc_iface;
}
#else
/// @brief Fake SteamAPI_SteamUGC_v021. @return ISteamUGC stand-in.
void *SteamAPI_SteamUGC_v021(void) {
    return (void *)&g_ugc_iface;
}
#endif

/// @brief Find a modeled Workshop item.
/// @param id PublishedFileId_t.
/// @return Item, or NULL.
static fake_ugc_item *fake_ugc_find(uint64_t id) {
    for (int i = 0; id && i < FAKE_UGC_ITEMS; ++i) {
        if (g_ugc_items[i].id == id)
            return &g_ugc_items[i];
    }
    return NULL;
}

/// @brief Find an open query.
/// @param handle UGCQueryHandle_t.
/// @return Query, or NULL.
static fake_ugc_query *fake_ugc_query_find(uint64_t handle) {
    for (int i = 0; handle && i < FAKE_UGC_QUERIES; ++i) {
        if (g_ugc_queries[i].handle == handle)
            return &g_ugc_queries[i];
    }
    return NULL;
}

/// @brief Open a query slot and describe it.
/// @param description How the query was created.
/// @return Handle, or UINT64_MAX when the fake is full.
static uint64_t fake_ugc_query_open(const char *description) {
    for (int i = 0; i < FAKE_UGC_QUERIES; ++i) {
        fake_ugc_query *query = &g_ugc_queries[i];
        if (query->handle)
            continue;
        memset(query, 0, sizeof(*query));
        query->handle = g_ugc_next_query++;
        snprintf(query->description, sizeof(query->description), "%s", description);
        return query->handle;
    }
    return UINT64_MAX;
}

/// @brief Report whether a comma-separated tag list holds a tag.
/// @param tags Comma-separated list.
/// @param tag Tag to find.
/// @return 1 when present.
static int fake_ugc_has_tag(const char *tags, const char *tag) {
    const size_t length = strlen(tag);
    for (const char *p = tags; *p;) {
        const char *comma = strchr(p, ',');
        const size_t part = comma ? (size_t)(comma - p) : strlen(p);
        if (part == length && strncmp(p, tag, length) == 0)
            return 1;
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

/// @brief Report whether an item carries every required tag of a query.
/// @param item Item.
/// @param required Comma-separated required tags.
/// @return 1 when it does.
static int fake_ugc_has_tags(const fake_ugc_item *item, const char *required) {
    char tag[64];
    for (const char *p = required; *p;) {
        const char *comma = strchr(p, ',');
        size_t part = comma ? (size_t)(comma - p) : strlen(p);
        if (part >= sizeof(tag))
            part = sizeof(tag) - 1;
        memcpy(tag, p, part);
        tag[part] = '\0';
        if (tag[0] && !fake_ugc_has_tag(item->tags, tag))
            return 0;
        if (!comma)
            break;
        p = comma + 1;
    }
    return 1;
}

/// @brief Fake ISteamUGC::CreateQueryUserUGCRequest.
uint64_t SteamAPI_ISteamUGC_CreateQueryUserUGCRequest(void *self,
                                                      uint32_t account,
                                                      int list,
                                                      int matching,
                                                      int sort,
                                                      uint32_t creator_app,
                                                      uint32_t consumer_app,
                                                      uint32_t page) {
    (void)self;
    char description[256];
    snprintf(description,
             sizeof(description),
             "user(account=%u,list=%d,matching=%d,sort=%d,creator=%u,consumer=%u,page=%u)",
             (unsigned)account,
             list,
             matching,
             sort,
             (unsigned)creator_app,
             (unsigned)consumer_app,
             (unsigned)page);
    snprintf(g_ugc_last_query, sizeof(g_ugc_last_query), "%s", description);
    const uint64_t handle = fake_ugc_query_open(description);
    fake_ugc_query *query = fake_ugc_query_find(handle);
    for (int i = 0; query && page == 1 && i < FAKE_UGC_ITEMS; ++i) {
        const fake_ugc_item *item = &g_ugc_items[i];
        if (!item->id)
            continue;
        const int published = list == 0 && (uint32_t)(item->owner & 0xFFFFFFFFu) == account;
        const int subscribed = list == 6 && item->subscribed;
        if (published || subscribed)
            query->results[query->result_count++] = item->id;
    }
    return handle;
}

/// @brief Fake ISteamUGC::CreateQueryAllUGCRequestPage.
uint64_t SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage(void *self,
                                                         int query_type,
                                                         int matching,
                                                         uint32_t creator_app,
                                                         uint32_t consumer_app,
                                                         uint32_t page) {
    (void)self;
    char description[256];
    snprintf(description,
             sizeof(description),
             "all(type=%d,matching=%d,creator=%u,consumer=%u,page=%u)",
             query_type,
             matching,
             (unsigned)creator_app,
             (unsigned)consumer_app,
             (unsigned)page);
    snprintf(g_ugc_last_query, sizeof(g_ugc_last_query), "%s", description);
    const uint64_t handle = fake_ugc_query_open(description);
    fake_ugc_query *query = fake_ugc_query_find(handle);
    for (int i = 0; query && page == 1 && i < FAKE_UGC_ITEMS; ++i) {
        if (g_ugc_items[i].id)
            query->results[query->result_count++] = g_ugc_items[i].id;
    }
    // Popular (0) ranks by score; every other order keeps creation order newest first.
    for (uint32_t a = 0; query && a < query->result_count; ++a) {
        for (uint32_t b = a + 1; b < query->result_count; ++b) {
            const fake_ugc_item *left = fake_ugc_find(query->results[a]);
            const fake_ugc_item *right = fake_ugc_find(query->results[b]);
            const int swap =
                query_type == 0 ? right->score > left->score : right->created > left->created;
            if (swap) {
                const uint64_t held = query->results[a];
                query->results[a] = query->results[b];
                query->results[b] = held;
            }
        }
    }
    return handle;
}

/// @brief Fake ISteamUGC::CreateQueryUGCDetailsRequest.
uint64_t SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(void *self,
                                                         uint64_t *ids,
                                                         uint32_t count) {
    (void)self;
    char description[512];
    int used = snprintf(description, sizeof(description), "details(");
    for (uint32_t i = 0; ids && i < count && used > 0 && (size_t)used < sizeof(description); ++i) {
        used += snprintf(description + used,
                         sizeof(description) - (size_t)used,
                         "%s%llu",
                         i ? "," : "",
                         (unsigned long long)ids[i]);
    }
    if (used > 0 && (size_t)used < sizeof(description) - 1)
        snprintf(description + used, sizeof(description) - (size_t)used, ")");
    snprintf(g_ugc_last_query, sizeof(g_ugc_last_query), "%s", description);
    const uint64_t handle = fake_ugc_query_open(description);
    fake_ugc_query *query = fake_ugc_query_find(handle);
    for (uint32_t i = 0; query && ids && i < count; ++i) {
        if (fake_ugc_find(ids[i]) && query->result_count < FAKE_UGC_ITEMS)
            query->results[query->result_count++] = ids[i];
    }
    return handle;
}

/// @brief Fake ISteamUGC::AddRequiredTag.
bool SteamAPI_ISteamUGC_AddRequiredTag(void *self, uint64_t handle, const char *tag) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query || !tag || !*tag)
        return false;
    const size_t used = strlen(query->tags);
    snprintf(query->tags + used, sizeof(query->tags) - used, "%s%s", used ? "," : "", tag);
    return true;
}

/// @brief Fake ISteamUGC::SetSearchText.
bool SteamAPI_ISteamUGC_SetSearchText(void *self, uint64_t handle, const char *text) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query || !text)
        return false;
    snprintf(query->search, sizeof(query->search), "%s", text);
    return true;
}

/// @brief Fake ISteamUGC::SetReturnLongDescription.
bool SteamAPI_ISteamUGC_SetReturnLongDescription(void *self, uint64_t handle, bool flag) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query)
        return false;
    query->long_description = flag ? 1 : 0;
    return true;
}

/// @brief Fake ISteamUGC::SetReturnMetadata.
bool SteamAPI_ISteamUGC_SetReturnMetadata(void *self, uint64_t handle, bool flag) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query)
        return false;
    query->metadata = flag ? 1 : 0;
    return true;
}

/// @brief Fake ISteamUGC::SendQueryUGCRequest; filters by tags and search text, then completes.
uint64_t SteamAPI_ISteamUGC_SendQueryUGCRequest(void *self, uint64_t handle) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query)
        return 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < query->result_count; ++i) {
        const fake_ugc_item *item = fake_ugc_find(query->results[i]);
        if (!item || !fake_ugc_has_tags(item, query->tags))
            continue;
        if (query->search[0] && !strstr(item->title, query->search) &&
            !strstr(item->description, query->search))
            continue;
        query->results[kept++] = query->results[i];
    }
    query->result_count = kept;
    size_t used = strlen(g_ugc_last_query);
    snprintf(g_ugc_last_query + used,
             sizeof(g_ugc_last_query) - used,
             " tags=%s search=%s long=%d metadata=%d",
             query->tags,
             query->search,
             query->long_description,
             query->metadata);
    uint8_t payload[280];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, handle);
    fake_put_u32(payload, 8, 1u);
    fake_put_u32(payload, 12, kept);
    fake_put_u32(payload, 16, kept);
    return fake_issue_call(3401, payload, 280);
}

/// @brief Fake ISteamUGC::GetQueryUGCResult writing SteamUGCDetails_t at explicit offsets.
bool SteamAPI_ISteamUGC_GetQueryUGCResult(void *self,
                                          uint64_t handle,
                                          uint32_t index,
                                          void *details) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query || !details || index >= query->result_count)
        return false;
    const fake_ugc_item *item = fake_ugc_find(query->results[index]);
    if (!item)
        return false;
    uint8_t *out = (uint8_t *)details;
    const int pack8 = FAKE_PACK8;
    fake_put_u64(out, 0, item->id);
    fake_put_u32(out, 8, 1u);
    fake_put_u32(out, 12, 0u);
    fake_put_u32(out, 16, fake_app_id());
    fake_put_u32(out, 20, fake_app_id());
    snprintf((char *)out + 24, 129, "%s", item->title);
    snprintf((char *)out + 153, 8000, "%s", query->long_description ? item->description : "");
    fake_put_u64(out, pack8 ? 8160 : 8156, item->owner);
    fake_put_u32(out, pack8 ? 8168 : 8164, item->created);
    fake_put_u32(out, pack8 ? 8172 : 8168, item->updated);
    fake_put_u32(out, pack8 ? 8180 : 8176, (uint32_t)item->visibility);
    snprintf((char *)out + (pack8 ? 8187 : 8183), 1025, "%s", item->tags);
    fake_put_u32(out, pack8 ? 9492 : 9484, (uint32_t)item->size);
    fake_put_u32(out, pack8 ? 9756 : 9748, item->votes_up);
    fake_put_u32(out, pack8 ? 9760 : 9752, item->votes_down);
    memcpy(out + (pack8 ? 9764 : 9756), &item->score, sizeof(item->score));
    fake_put_u64(out, pack8 ? 9776 : 9764, item->size);
    return true;
}

/// @brief Fake ISteamUGC::GetQueryUGCPreviewURL.
bool SteamAPI_ISteamUGC_GetQueryUGCPreviewURL(
    void *self, uint64_t handle, uint32_t index, char *url, uint32_t capacity) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query || !url || capacity == 0 || index >= query->result_count)
        return false;
    snprintf(url,
             capacity,
             "https://fake.example/workshop/%llu.png",
             (unsigned long long)query->results[index]);
    return true;
}

/// @brief Fake ISteamUGC::GetQueryUGCMetadata; empty unless SetReturnMetadata was requested.
bool SteamAPI_ISteamUGC_GetQueryUGCMetadata(
    void *self, uint64_t handle, uint32_t index, char *metadata, uint32_t capacity) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query || !metadata || capacity == 0 || index >= query->result_count)
        return false;
    const fake_ugc_item *item = fake_ugc_find(query->results[index]);
    snprintf(metadata, capacity, "%s", (item && query->metadata) ? item->metadata : "");
    return true;
}

/// @brief Fake ISteamUGC::ReleaseQueryUGCRequest.
bool SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(void *self, uint64_t handle) {
    (void)self;
    fake_ugc_query *query = fake_ugc_query_find(handle);
    if (!query)
        return false;
    memset(query, 0, sizeof(*query));
    return true;
}

/// @brief Fake ISteamUGC::CreateItem; the new item belongs to the player.
uint64_t SteamAPI_ISteamUGC_CreateItem(void *self, uint32_t app, int file_type) {
    (void)self;
    (void)file_type;
    fake_ugc_item *item = NULL;
    for (int i = 0; i < FAKE_UGC_ITEMS && !item; ++i) {
        if (!g_ugc_items[i].id)
            item = &g_ugc_items[i];
    }
    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));
    if (!item || app != fake_app_id()) {
        fake_put_u32(payload, 0, 2u);
    } else {
        memset(item, 0, sizeof(*item));
        item->id = g_ugc_next_item++;
        item->owner = FAKE_STEAM_ID;
        item->created = 1700001000u;
        item->updated = 1700001000u;
        item->visibility = 2;
        fake_put_u32(payload, 0, 1u);
        fake_put_u64(payload, FAKE_PACK8 ? 8 : 4, item->id);
        payload[FAKE_PACK8 ? 16 : 12] = 1;
    }
    return fake_issue_call(3403, payload, FAKE_PACK8 ? 24 : 16);
}

/// @brief Fake ISteamUGC::StartItemUpdate; only the player's own items can be updated.
uint64_t SteamAPI_ISteamUGC_StartItemUpdate(void *self, uint32_t app, uint64_t id) {
    (void)self;
    const fake_ugc_item *item = fake_ugc_find(id);
    if (!item || item->owner != FAKE_STEAM_ID || app != fake_app_id())
        return UINT64_MAX;
    memset(&g_ugc_update, 0, sizeof(g_ugc_update));
    g_ugc_update.handle = g_ugc_next_update++;
    g_ugc_update.item = id;
    g_ugc_update.visibility = -1;
    return g_ugc_update.handle;
}

/// @brief Report whether an update handle names the pending update.
/// @param handle UGCUpdateHandle_t.
/// @return 1 when it does.
static int fake_ugc_update_is(uint64_t handle) {
    return handle && handle == g_ugc_update.handle && !g_ugc_update.submit_call;
}

/// @brief Fake ISteamUGC::SetItemTitle; titles hold at most 128 bytes.
bool SteamAPI_ISteamUGC_SetItemTitle(void *self, uint64_t handle, const char *title) {
    (void)self;
    if (!fake_ugc_update_is(handle) || !title || strlen(title) > 128)
        return false;
    snprintf(g_ugc_update.title, sizeof(g_ugc_update.title), "%s", title);
    g_ugc_update.has_title = 1;
    return true;
}

/// @brief Fake ISteamUGC::SetItemDescription.
bool SteamAPI_ISteamUGC_SetItemDescription(void *self, uint64_t handle, const char *text) {
    (void)self;
    if (!fake_ugc_update_is(handle) || !text)
        return false;
    snprintf(g_ugc_update.description, sizeof(g_ugc_update.description), "%s", text);
    g_ugc_update.has_description = 1;
    return true;
}

/// @brief Fake ISteamUGC::SetItemMetadata.
bool SteamAPI_ISteamUGC_SetItemMetadata(void *self, uint64_t handle, const char *text) {
    (void)self;
    if (!fake_ugc_update_is(handle) || !text)
        return false;
    snprintf(g_ugc_update.metadata, sizeof(g_ugc_update.metadata), "%s", text);
    g_ugc_update.has_metadata = 1;
    return true;
}

/// @brief Fake ISteamUGC::SetItemVisibility.
bool SteamAPI_ISteamUGC_SetItemVisibility(void *self, uint64_t handle, int visibility) {
    (void)self;
    if (!fake_ugc_update_is(handle) || visibility < 0 || visibility > 3)
        return false;
    g_ugc_update.visibility = visibility;
    return true;
}

/// @brief Fake ISteamUGC::SetItemTags reading SteamParamStringArray_t at explicit offsets.
bool SteamAPI_ISteamUGC_SetItemTags(void *self, uint64_t handle, const void *tags, bool admin) {
    (void)self;
    (void)admin;
    if (!fake_ugc_update_is(handle) || !tags)
        return false;
    const char **strings = NULL;
    int32_t count = 0;
    memcpy((void *)&strings, tags, sizeof(strings));
    memcpy(&count, (const char *)tags + 8, sizeof(count));
    g_ugc_update.tags[0] = '\0';
    for (int32_t i = 0; i < count; ++i) {
        const size_t used = strlen(g_ugc_update.tags);
        snprintf(g_ugc_update.tags + used,
                 sizeof(g_ugc_update.tags) - used,
                 "%s%s",
                 i ? "," : "",
                 strings[i]);
    }
    g_ugc_update.has_tags = 1;
    return true;
}

/// @brief Fake ISteamUGC::SetItemContent.
bool SteamAPI_ISteamUGC_SetItemContent(void *self, uint64_t handle, const char *folder) {
    (void)self;
    if (!fake_ugc_update_is(handle) || !folder)
        return false;
    snprintf(g_ugc_update.content, sizeof(g_ugc_update.content), "%s", folder);
    return true;
}

/// @brief Fake ISteamUGC::SetItemPreview.
bool SteamAPI_ISteamUGC_SetItemPreview(void *self, uint64_t handle, const char *file) {
    (void)self;
    if (!fake_ugc_update_is(handle) || !file)
        return false;
    snprintf(g_ugc_update.preview, sizeof(g_ugc_update.preview), "%s", file);
    return true;
}

/// @brief Fake ISteamUGC::SubmitItemUpdate; applies the fields and completes on the next frame.
uint64_t SteamAPI_ISteamUGC_SubmitItemUpdate(void *self, uint64_t handle, const char *note) {
    (void)self;
    if (!fake_ugc_update_is(handle))
        return 0;
    fake_ugc_item *item = fake_ugc_find(g_ugc_update.item);
    snprintf(g_ugc_update.note, sizeof(g_ugc_update.note), "%s", note ? note : "");
    if (item) {
        if (g_ugc_update.has_title)
            snprintf(item->title, sizeof(item->title), "%s", g_ugc_update.title);
        if (g_ugc_update.has_description)
            snprintf(item->description, sizeof(item->description), "%s", g_ugc_update.description);
        if (g_ugc_update.has_metadata)
            snprintf(item->metadata, sizeof(item->metadata), "%s", g_ugc_update.metadata);
        if (g_ugc_update.has_tags)
            snprintf(item->tags, sizeof(item->tags), "%s", g_ugc_update.tags);
        if (g_ugc_update.visibility >= 0)
            item->visibility = g_ugc_update.visibility;
        item->updated++;
    }
    snprintf(g_ugc_last_update,
             sizeof(g_ugc_last_update),
             "title=%s|description=%s|metadata=%s|tags=%s|visibility=%d|content=%s|preview=%s|"
             "note=%s",
             g_ugc_update.title,
             g_ugc_update.description,
             g_ugc_update.metadata,
             g_ugc_update.tags,
             g_ugc_update.visibility,
             g_ugc_update.content,
             g_ugc_update.preview,
             g_ugc_update.note);
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u32(payload, 0, item ? 1u : 9u);
    fake_put_u64(payload, 8, g_ugc_update.item);
    g_ugc_update.submit_call = fake_issue_call(3404, payload, 16);
    return g_ugc_update.submit_call;
}

/// @brief Fake ISteamUGC::GetItemUpdateProgress; uploading until the submission completes.
int SteamAPI_ISteamUGC_GetItemUpdateProgress(void *self,
                                             uint64_t handle,
                                             uint64_t *processed,
                                             uint64_t *total) {
    (void)self;
    *processed = 0;
    *total = 0;
    if (!handle || handle != g_ugc_update.handle)
        return 0;
    if (!g_ugc_update.submit_call)
        return 1;
    for (int i = 0; i < g_call_count; ++i) {
        if (g_calls[i].handle == g_ugc_update.submit_call && g_calls[i].completed)
            return 0;
    }
    *processed = 256u;
    *total = 1024u;
    return 3;
}

/// @brief Issue a call result naming one item.
/// @param callback_id Result identifier.
/// @param result EResult.
/// @param id PublishedFileId_t.
/// @return Call handle.
static uint64_t fake_ugc_file_call(int32_t callback_id, uint32_t result, uint64_t id) {
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u32(payload, 0, result);
    fake_put_u64(payload, FAKE_PACK8 ? 8 : 4, id);
    return fake_issue_call(callback_id, payload, FAKE_PACK8 ? 16 : 12);
}

/// @brief Queue RemoteStoragePublishedFile[Un]subscribed_t.
/// @param id PublishedFileId_t.
/// @param subscribed Nonzero for a subscription.
static void fake_ugc_queue_subscription(uint64_t id, int subscribed) {
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    fake_put_u64(payload, 0, id);
    fake_put_u32(payload, 8, fake_app_id());
    fake_queue(subscribed ? 1321 : 1322, payload, FAKE_PACK8 ? 16 : 12);
}

/// @brief Fake ISteamUGC::SubscribeItem.
uint64_t SteamAPI_ISteamUGC_SubscribeItem(void *self, uint64_t id) {
    (void)self;
    fake_ugc_item *item = fake_ugc_find(id);
    if (!item)
        return fake_ugc_file_call(1313, 9u, id);
    item->subscribed = 1;
    if (!item->installed)
        item->needs_update = 1;
    fake_ugc_queue_subscription(id, 1);
    return fake_ugc_file_call(1313, 1u, id);
}

/// @brief Fake ISteamUGC::UnsubscribeItem.
uint64_t SteamAPI_ISteamUGC_UnsubscribeItem(void *self, uint64_t id) {
    (void)self;
    fake_ugc_item *item = fake_ugc_find(id);
    if (!item)
        return fake_ugc_file_call(1315, 9u, id);
    item->subscribed = 0;
    fake_ugc_queue_subscription(id, 0);
    return fake_ugc_file_call(1315, 1u, id);
}

/// @brief Fake ISteamUGC::DeleteItem; only the player's own items can be deleted.
uint64_t SteamAPI_ISteamUGC_DeleteItem(void *self, uint64_t id) {
    (void)self;
    fake_ugc_item *item = fake_ugc_find(id);
    if (!item || item->owner != FAKE_STEAM_ID)
        return fake_ugc_file_call(3417, 15u, id);
    memset(item, 0, sizeof(*item));
    return fake_ugc_file_call(3417, 1u, id);
}

/// @brief Count subscribed items.
/// @return Count.
static uint32_t fake_ugc_subscribed_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < FAKE_UGC_ITEMS; ++i) {
        if (g_ugc_items[i].id && g_ugc_items[i].subscribed)
            ++count;
    }
    return count;
}

/// @brief Write subscribed item ids.
/// @param ids Destination.
/// @param max Destination capacity.
/// @return Ids written.
static uint32_t fake_ugc_subscribed_items(uint64_t *ids, uint32_t max) {
    uint32_t count = 0;
    for (int i = 0; ids && i < FAKE_UGC_ITEMS && count < max; ++i) {
        if (g_ugc_items[i].id && g_ugc_items[i].subscribed)
            ids[count++] = g_ugc_items[i].id;
    }
    return count;
}

#if defined(ZANNA_FAKE_STEAM_PROFILE_164)
/// @brief Fake ISteamUGC::GetNumSubscribedItems as declared through SDK 1.61.
uint32_t SteamAPI_ISteamUGC_GetNumSubscribedItems(void *self) {
    (void)self;
    return fake_ugc_subscribed_count();
}

/// @brief Fake ISteamUGC::GetSubscribedItems as declared through SDK 1.61.
uint32_t SteamAPI_ISteamUGC_GetSubscribedItems(void *self, uint64_t *ids, uint32_t max) {
    (void)self;
    return fake_ugc_subscribed_items(ids, max);
}
#else
/// @brief Fake ISteamUGC::GetNumSubscribedItems from SDK 1.62; the bool must be false.
uint32_t SteamAPI_ISteamUGC_GetNumSubscribedItems(void *self, bool include_locally_disabled) {
    (void)self;
    return include_locally_disabled ? 0u : fake_ugc_subscribed_count();
}

/// @brief Fake ISteamUGC::GetSubscribedItems from SDK 1.62; the bool must be false.
uint32_t SteamAPI_ISteamUGC_GetSubscribedItems(void *self,
                                               uint64_t *ids,
                                               uint32_t max,
                                               bool include_locally_disabled) {
    (void)self;
    return include_locally_disabled ? 0u : fake_ugc_subscribed_items(ids, max);
}
#endif

/// @brief Fake ISteamUGC::GetItemState.
uint32_t SteamAPI_ISteamUGC_GetItemState(void *self, uint64_t id) {
    (void)self;
    const fake_ugc_item *item = fake_ugc_find(id);
    if (!item)
        return 0u;
    return (item->subscribed ? 1u : 0u) | (item->installed ? 4u : 0u) |
           (item->needs_update ? 8u : 0u) | (item->downloading ? 16u : 0u);
}

/// @brief Fake ISteamUGC::GetItemInstallInfo.
bool SteamAPI_ISteamUGC_GetItemInstallInfo(
    void *self, uint64_t id, uint64_t *size, char *folder, uint32_t capacity, uint32_t *timestamp) {
    (void)self;
    const fake_ugc_item *item = fake_ugc_find(id);
    if (!item || !item->installed || !folder || capacity == 0)
        return false;
    *size = item->size;
    *timestamp = item->updated;
    snprintf(folder,
             capacity,
             "/fake/workshop/content/%u/%llu",
             (unsigned)fake_app_id(),
             (unsigned long long)id);
    return true;
}

/// @brief Fake ISteamUGC::GetItemDownloadInfo.
bool SteamAPI_ISteamUGC_GetItemDownloadInfo(void *self,
                                            uint64_t id,
                                            uint64_t *downloaded,
                                            uint64_t *total) {
    (void)self;
    const fake_ugc_item *item = fake_ugc_find(id);
    if (!item || (!item->needs_update && !item->downloading && !item->installed))
        return false;
    *downloaded = item->downloaded;
    *total = item->size;
    return true;
}

/// @brief Fake ISteamUGC::DownloadItem; the download finishes on the next frame.
bool SteamAPI_ISteamUGC_DownloadItem(void *self, uint64_t id, bool high_priority) {
    (void)self;
    (void)high_priority;
    fake_ugc_item *item = fake_ugc_find(id);
    if (!item)
        return false;
    item->downloading = 1;
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

/// @brief Describe the last Workshop query the fake created, with its tags and search text.
/// @return Static text, or "".
const char *ZannaFakeSteam_UgcLastQuery(void) {
    return g_ugc_last_query;
}

/// @brief Count Workshop queries not released yet. @return Count.
int ZannaFakeSteam_UgcOpenQueries(void) {
    int count = 0;
    for (int i = 0; i < FAKE_UGC_QUERIES; ++i) {
        if (g_ugc_queries[i].handle)
            ++count;
    }
    return count;
}

/// @brief Describe the last submitted Workshop update. @return Static text, or "".
const char *ZannaFakeSteam_UgcLastUpdate(void) {
    return g_ugc_last_update;
}

/// @brief Number of GetAchievementIcon calls since the reset. @return Count.
int ZannaFakeSteam_IconRequestCount(void) {
    return g_icon_requests;
}

/// @brief Connect or disconnect a modeled controller; the change is reported on the next input
///        frame, and a connection gives the app a controller mapping.
/// @param index Controller index (0 or 1).
/// @param connected Nonzero to connect.
void ZannaFakeSteam_SetControllerConnected(int index, int connected) {
    if (index >= 0 && index < FAKE_INPUT_CONTROLLERS) {
        g_input[index].connected = connected ? 1 : 0;
        if (connected)
            g_input_mapping = 1;
    }
}

/// @brief Give or take away the app's controller mapping, without which manifests are refused.
/// @param available Nonzero when the mapping exists.
void ZannaFakeSteam_SetInputMapping(int available) {
    g_input_mapping = available ? 1 : 0;
}

/// @brief Map an action name to its fake state slot.
/// @param action Action name.
/// @return Slot, or -1.
static int fake_input_named_slot(const char *action) {
    static const char *const names[FAKE_INPUT_ACTIONS] = {
        "swing", "bunt", "pause", "select", "aim", "cursor"};
    for (int i = 0; action && i < FAKE_INPUT_ACTIONS; ++i) {
        if (strcmp(names[i], action) == 0)
            return i;
    }
    return -1;
}

/// @brief Script whether a digital action is held on a controller.
/// @param index Controller index.
/// @param action Digital action name.
/// @param pressed Nonzero while held.
void ZannaFakeSteam_SetDigitalAction(int index, const char *action, int pressed) {
    const int slot = fake_input_named_slot(action);
    if (index >= 0 && index < FAKE_INPUT_CONTROLLERS && slot >= 0)
        g_input[index].pressed[slot] = pressed ? 1 : 0;
}

/// @brief Script an analog action's values on a controller.
/// @param index Controller index.
/// @param action Analog action name.
/// @param x Horizontal value.
/// @param y Vertical value.
void ZannaFakeSteam_SetAnalogAction(int index, const char *action, double x, double y) {
    const int slot = fake_input_named_slot(action);
    if (index >= 0 && index < FAKE_INPUT_CONTROLLERS && slot >= 0) {
        g_input[index].x[slot] = (float)x;
        g_input[index].y[slot] = (float)y;
    }
}

/// @brief Make ISteamInput::Init succeed or fail. @param ok Nonzero to succeed.
void ZannaFakeSteam_SetInputInitResult(int ok) {
    g_input_init_result = ok ? 1 : 0;
}

/// @brief Describe Steam Input state as "initialized,explicit frames,device callbacks,frames".
/// @return Static text.
const char *ZannaFakeSteam_InputState(void) {
    static char state[64];
    snprintf(state,
             sizeof(state),
             "%d,%d,%d,%d",
             g_input_initialized,
             g_input_explicit_frames,
             g_input_device_callbacks,
             g_input_frames);
    return state;
}

/// @brief Manifest path Steam Input last accepted. @return Path, or "".
const char *ZannaFakeSteam_InputManifestPath(void) {
    return g_input_manifest;
}

/// @brief Number of Steam Input handle lookups since the reset. @return Count.
int ZannaFakeSteam_InputHandleLookups(void) {
    return g_input_handle_lookups;
}

/// @brief Script the icon size ISteamUtils::GetImageSize reports.
/// @param width Width in pixels.
/// @param height Height in pixels.
void ZannaFakeSteam_SetIconSize(uint32_t width, uint32_t height) {
    g_icon_width = width;
    g_icon_height = height;
}

/// @brief Set the EResult later global percentage requests report.
/// @param result EResult value; anything but 1 leaves the percentages unloaded.
void ZannaFakeSteam_SetGlobalPercentagesResult(int32_t result) {
    g_percentages_result = result;
}

/// @brief Script the launch URL the fake reports and queue NewUrlLaunchParameters_t.
/// @param command_line Command line of steam://run/<appid>//<command line>/.
/// @param query Query of steam://run/<appid>//?<query>, as "key=value&key=value".
void ZannaFakeSteam_SetLaunch(const char *command_line, const char *query) {
    snprintf(g_launch_command_line,
             sizeof(g_launch_command_line),
             "%s",
             command_line ? command_line : "");
    snprintf(g_launch_query, sizeof(g_launch_query), "%s", query ? query : "");
    uint8_t payload[1] = {0};
    fake_queue(1014, payload, 1);
}
