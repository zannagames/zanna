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
//          real run-time loading, symbol resolution, manual dispatch, and
//          payload decoding without Steam or any Valve file.
// Key invariants:
//   - Exports only the flat-API symbols the Zanna Steam provider binds, plus
//     ZannaFakeSteam_* control and inspection functions for tests.
//   - Payload bytes are written at explicit offsets derived from the
//     documented layouts, independently of the runtime's own ABI header, so a
//     layout mistake in the runtime cannot be mirrored here.
//   - Build profiles select the exported interface generation:
//       default                              SDK 1.65 accessor set
//       ZANNA_FAKE_STEAM_PROFILE_164=1       SDK 1.61-1.64 accessor set
//       ZANNA_FAKE_STEAM_OMIT_INIT_FLAT=1    no SteamAPI_InitFlat export
//   - ZANNA_FAKE_STEAM_SCENARIO (read by SteamAPI_InitFlat) selects the init
//     outcome: unset or "ok", "no-client", "version-mismatch", "generic".
//     ZANNA_FAKE_STEAM_RESTART=1 makes SteamAPI_RestartAppIfNecessary report true.
//     In-process tests use ZannaFakeSteam_SetScenario/SetRestart instead,
//     because a CRT environment snapshot may not observe variables changed
//     after the C runtime started (Windows).
// Ownership/Lifetime:
//   - All state is static and lives for the process; the library is never
//     unloaded by the runtime.
//   - Callback payload pointers stay valid until FreeLastCallback.
// Links: src/runtime/services/steam/rt_steam_provider.c,
//        src/tests/runtime/RTServicesTests.cpp,
//        src/tests/fixtures/runtime/test_services_platform.zia
//
//===----------------------------------------------------------------------===//

/**
 * @file RTServicesFakeSteamApi.c
 * @brief Scriptable fake of the Steamworks flat C API for platform services tests.
 * @details After a successful SteamAPI_InitFlat, the first dispatch frame queues
 *          SteamServersConnected, GameOverlayActivated (open, user initiated),
 *          and DlcInstalled (1234567) unless scripting is disabled. Player-count
 *          calls complete on the next dispatch frame with 42 players. Tests can
 *          inject raw callbacks with ZannaFakeSteam_QueueCallback.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Maximum queued callbacks.
#define FAKE_QUEUE_CAPACITY 1024
/// @brief Maximum payload bytes per queued callback.
#define FAKE_PAYLOAD_CAPACITY 64
/// @brief Maximum outstanding player-count calls.
#define FAKE_CALL_CAPACITY 128
/// @brief First call handle issued by GetNumberOfCurrentPlayers.
#define FAKE_FIRST_CALL_HANDLE UINT64_C(0x5EA0000000000001)
/// @brief SteamID64 reported for the fake signed-in user.
#define FAKE_STEAM_ID UINT64_C(76561198000000001)
/// @brief DLC app id reported as installed.
#define FAKE_INSTALLED_DLC UINT32_C(1234567)
/// @brief Player count reported by completed player-count calls.
#define FAKE_PLAYER_COUNT INT32_C(42)

/// @brief One queued callback record.
typedef struct fake_callback {
    int32_t id;                           ///< Callback identifier.
    int32_t size;                         ///< Payload size in bytes.
    uint8_t bytes[FAKE_PAYLOAD_CAPACITY]; ///< Payload bytes.
} fake_callback;

/// @brief One issued player-count call.
typedef struct fake_call {
    uint64_t handle; ///< Issued SteamAPICall_t.
    int completed;   ///< Nonzero once its SteamAPICallCompleted_t was queued.
} fake_call;

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

/// @brief Distinct non-null addresses used as interface pointers.
static const char g_user_iface = 'u';
static const char g_friends_iface = 'f';
static const char g_utils_iface = 't';
static const char g_apps_iface = 'a';
static const char g_user_stats_iface = 's';

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

/// @brief Fake SteamAPI_ManualDispatch_RunFrame: queues scripted and completed-call callbacks.
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
    for (int i = 0; i < g_call_count; ++i) {
        if (g_calls[i].completed)
            continue;
        uint8_t payload[FAKE_PAYLOAD_CAPACITY];
        memset(payload, 0, sizeof(payload));
        fake_put_u64(payload, 0, g_calls[i].handle);
        fake_put_u32(payload, 8, 1107u);
        fake_put_u32(payload, 12, 8u);
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

/// @brief Fake SteamAPI_ManualDispatch_GetAPICallResult for player-count calls.
/// @param pipe Client pipe (unused).
/// @param call Completed call handle.
/// @param buffer Destination for NumberOfCurrentPlayers_t.
/// @param size Destination size.
/// @param expected_id Expected callback id.
/// @param failed Receives the failure flag.
/// @return True when the result was written.
bool SteamAPI_ManualDispatch_GetAPICallResult(
    int32_t pipe, uint64_t call, void *buffer, int size, int expected_id, bool *failed) {
    (void)pipe;
    int known = 0;
    for (int i = 0; i < g_call_count; ++i) {
        if (g_calls[i].handle == call && g_calls[i].completed)
            known = 1;
    }
    if (!known || expected_id != 1107 || size != 8 || !buffer) {
        if (failed)
            *failed = true;
        return false;
    }
    uint8_t payload[8];
    memset(payload, 0, sizeof(payload));
    payload[0] = 1;
    fake_put_u32(payload, 4, (uint32_t)FAKE_PLAYER_COUNT);
    memcpy(buffer, payload, sizeof(payload));
    if (failed)
        *failed = false;
    return true;
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
// Interface methods
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
    const char *env_app_id = getenv("SteamAppId");
    if (env_app_id && *env_app_id)
        return (uint32_t)strtoul(env_app_id, NULL, 10);
    return 480u;
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
    if (g_call_count == FAKE_CALL_CAPACITY)
        return 0;
    g_calls[g_call_count].handle = g_next_call_handle++;
    g_calls[g_call_count].completed = 0;
    return g_calls[g_call_count++].handle;
}

//===----------------------------------------------------------------------===//
// Test control and inspection
//===----------------------------------------------------------------------===//

/// @brief Reset every counter, queue, and scripted flag.
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
