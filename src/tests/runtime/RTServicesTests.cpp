//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesTests.cpp
// Purpose: Verify the Zanna.Services platform layer and its Steam provider
//          against from-scratch fake steam_api shared libraries: library
//          resolution, core-export validation, init outcomes, identity and
//          licensing queries, manual dispatch, payload validation, event
//          overflow, requests, the frame-pump hook, threading, and packing.
// Key invariants:
//   - Every test starts from a shut-down session and a reset fake library.
//   - Fake libraries are selected through ZANNA_SERVICES_STEAM_LIBRARY and are
//     inspected through the runtime's own dynamic-library adapter.
//   - Traps are observed through a vm_trap override that either records and
//     returns (worker-thread checks) or jumps back to the test.
// Ownership/Lifetime:
//   - Tests release every Result, Request, Seq, and string they receive.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/steam/rt_steam_provider.c,
//        src/tests/runtime/RTServicesFakeSteamApi.c,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md
//
//===----------------------------------------------------------------------===//

#include "rt_args.h"
#include "rt_object.h"
#include "rt_path.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_service_hooks.h"
#include "rt_services.h"
#include "rt_services_dynlib.h"
#include "rt_steam.h"
#include "rt_steam_abi.h"
#include "rt_string.h"
#include "tests/TestHarness.hpp"

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

jmp_buf g_trap_jmp;
bool g_trap_jump = false;
std::string g_last_trap;
int g_trap_count = 0;

} // namespace

/// @brief Test trap hook: records the message and optionally jumps back to the test.
/// @param msg Trap message.
extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg ? msg : "";
    ++g_trap_count;
    if (g_trap_jump)
        longjmp(g_trap_jmp, 1);
}

/// @brief Run @p expr and require that it traps with exactly @p message.
#define EXPECT_TRAP_MESSAGE(expr, message)                                                         \
    do {                                                                                           \
        g_last_trap.clear();                                                                       \
        g_trap_jump = true;                                                                        \
        bool trapped_ = false;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            (void)(expr);                                                                          \
        } else {                                                                                   \
            trapped_ = true;                                                                       \
        }                                                                                          \
        g_trap_jump = false;                                                                       \
        EXPECT_TRUE(trapped_);                                                                     \
        EXPECT_EQ(g_last_trap, std::string(message));                                              \
    } while (0)

namespace {

const char *const kOverrideEnv = "ZANNA_SERVICES_STEAM_LIBRARY";

/// @brief Copy a caller-owned runtime string into std::string and release it.
std::string take(rt_string value) {
    std::string out = value ? rt_string_cstr(value) : "";
    if (value)
        rt_string_unref(value);
    return out;
}

/// @brief Set an environment variable through the runtime's portable helper.
void setEnv(const char *name, const char *value) {
    rt_string n = rt_const_cstr(name);
    rt_string v = rt_const_cstr(value);
    rt_env_set_var(n, v);
    rt_string_unref(n);
    rt_string_unref(v);
}

/// @brief Read an environment variable through the runtime's portable helper.
std::string getEnv(const char *name) {
    rt_string n = rt_const_cstr(name);
    std::string out = take(rt_env_get_var(n));
    rt_string_unref(n);
    return out;
}

/// @brief Report whether an environment variable is present.
bool hasEnv(const char *name) {
    rt_string n = rt_const_cstr(name);
    bool present = rt_env_has_var(n) != 0;
    rt_string_unref(n);
    return present;
}

/// @brief Release one reference to a runtime object.
void release(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Outcome of Platform.Init.
struct InitOutcome {
    bool ok = false;  ///< Result was Ok.
    std::string text; ///< Ok payload or Err message.
};

/// @brief Call Platform.Init and decode its Result.
InitOutcome init(const char *provider, const char *app_id) {
    rt_string p = rt_const_cstr(provider);
    rt_string id = rt_const_cstr(app_id);
    void *result = rt_services_platform_init(p, id);
    rt_string_unref(p);
    rt_string_unref(id);
    InitOutcome out;
    if (result) {
        out.ok = rt_result_is_ok(result) != 0;
        rt_string text = out.ok ? rt_result_unwrap_str(result) : rt_result_unwrap_err_str(result);
        out.text = text ? rt_string_cstr(text) : "";
        release(result);
    }
    return out;
}

/// @brief Copy Platform.Diagnostics into a vector.
std::vector<std::string> diagnostics() {
    std::vector<std::string> out;
    void *seq = rt_services_platform_diagnostics();
    for (int64_t i = 0; seq && i < rt_seq_len(seq); ++i)
        out.push_back(take(rt_seq_get_str(seq, i)));
    release(seq);
    return out;
}

/// @brief Report whether any retained diagnostic equals @p message.
bool hasDiagnostic(const std::string &message) {
    for (const auto &entry : diagnostics()) {
        if (entry == message)
            return true;
    }
    return false;
}

/// @brief Test view of a fake steam_api library opened through the runtime adapter.
struct FakeSteam {
    void *library = nullptr;
    std::string path;

    explicit FakeSteam(const char *library_path) : path(library_path) {
        char error[512];
        library = rt_services_dynlib_open(library_path, error, sizeof(error));
    }

    /// @brief Resolve a control export as a typed function pointer.
    template <typename Fn> Fn fn(const char *name) const {
        void *symbol = rt_services_dynlib_symbol(library, name);
        Fn out = nullptr;
        if (symbol)
            std::memcpy(&out, &symbol, sizeof(out));
        return out;
    }

    void reset() const {
        fn<void (*)()>("ZannaFakeSteam_Reset")();
    }

    void setScripted(int enabled) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetScripted")(enabled);
    }

    void setScenario(const char *scenario) const {
        fn<void (*)(const char *)>("ZannaFakeSteam_SetScenario")(scenario);
    }

    void setRestart(int restart) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetRestart")(restart);
    }

    void queue(int32_t id, const std::vector<uint8_t> &bytes, int32_t size) const {
        fn<void (*)(int32_t, const uint8_t *, int32_t)>("ZannaFakeSteam_QueueCallback")(
            id, bytes.empty() ? nullptr : bytes.data(), size);
    }

    int initCount() const {
        return fn<int (*)()>("ZannaFakeSteam_InitCount")();
    }

    int shutdownCount() const {
        return fn<int (*)()>("ZannaFakeSteam_ShutdownCount")();
    }

    int runFrameCount() const {
        return fn<int (*)()>("ZannaFakeSteam_RunFrameCount")();
    }

    int dispatchInitCount() const {
        return fn<int (*)()>("ZannaFakeSteam_DispatchInitCount")();
    }

    int queuedCount() const {
        return fn<int (*)()>("ZannaFakeSteam_QueuedCount")();
    }

    int protocolViolation() const {
        return fn<int (*)()>("ZannaFakeSteam_ProtocolViolation")();
    }

    uint32_t lastRestartAppId() const {
        return fn<uint32_t (*)()>("ZannaFakeSteam_LastRestartAppId")();
    }

    std::string appIdEnvAtInit() const {
        return fn<const char *(*)()>("ZannaFakeSteam_AppIdEnvAtInit")();
    }
};

/// @brief Return the fake built for the SDK 1.65 accessor set.
const FakeSteam &fake165() {
    static const FakeSteam fake(ZANNA_FAKE_STEAM_LIBRARY);
    return fake;
}

/// @brief Return the fake built for the SDK 1.61-1.64 accessor set.
const FakeSteam &fake164() {
    static const FakeSteam fake(ZANNA_FAKE_STEAM_LIBRARY_164);
    return fake;
}

/// @brief Shut the session down, point the provider at @p fake, and reset it.
void useFake(const FakeSteam &fake) {
    rt_services_platform_shutdown();
    setEnv(kOverrideEnv, fake.path.c_str());
    fake.reset();
}

/// @brief Build a little-endian-agnostic payload from explicit field writes.
std::vector<uint8_t> payload(size_t size) {
    return std::vector<uint8_t>(size, 0);
}

/// @brief Store a 32-bit value into a payload at @p offset.
void put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// @brief Drain every queued event kind.
std::vector<int64_t> drainEventKinds() {
    std::vector<int64_t> kinds;
    for (;;) {
        int64_t kind = rt_services_platform_poll_event();
        if (kind == RT_SERVICES_EVENT_NONE)
            break;
        kinds.push_back(kind);
    }
    return kinds;
}

/// @brief Platform-specific redistributable file name expected beside the executable.
const char *expectedLibraryFileName() {
#if RT_PLATFORM_WINDOWS
    return "steam_api64.dll";
#elif RT_PLATFORM_MACOS
    return "libsteam_api.dylib";
#else
    return "libsteam_api.so";
#endif
}

} // namespace

TEST(Services, ConstantsMatchAdr) {
    EXPECT_EQ(rt_services_status_ok(), 0);
    EXPECT_EQ(rt_services_status_not_started(), 1);
    EXPECT_EQ(rt_services_status_unknown_provider(), 2);
    EXPECT_EQ(rt_services_status_library_not_found(), 3);
    EXPECT_EQ(rt_services_status_library_incompatible(), 4);
    EXPECT_EQ(rt_services_status_unsupported_platform(), 5);
    EXPECT_EQ(rt_services_status_client_not_running(), 6);
    EXPECT_EQ(rt_services_status_version_mismatch(), 7);
    EXPECT_EQ(rt_services_status_init_failed(), 8);
    EXPECT_EQ(rt_services_event_kind_none(), 0);
    EXPECT_EQ(rt_services_event_kind_service_connected(), 1);
    EXPECT_EQ(rt_services_event_kind_service_disconnected(), 2);
    EXPECT_EQ(rt_services_event_kind_connect_failed(), 3);
    EXPECT_EQ(rt_services_event_kind_overlay_changed(), 4);
    EXPECT_EQ(rt_services_event_kind_dlc_installed(), 5);
    EXPECT_EQ(rt_services_event_kind_launch_parameters_changed(), 6);
    EXPECT_EQ(rt_services_event_kind_service_shutdown(), 7);
    EXPECT_EQ(rt_services_feature_identity(), 1);
    EXPECT_EQ(rt_services_feature_licensing(), 2);
    EXPECT_EQ(rt_services_feature_language(), 3);
    EXPECT_EQ(rt_services_feature_player_count(), 4);
    EXPECT_EQ(rt_services_request_kind_player_count(), 1);
    EXPECT_EQ(rt_services_steam_hardware_unknown(), -1);
    EXPECT_EQ(rt_services_steam_hardware_none(), 0);
    EXPECT_EQ(rt_services_steam_hardware_steam_deck(), 1);
    EXPECT_EQ(rt_services_steam_hardware_steam_machine(), 2);
    EXPECT_EQ(rt_services_steam_hardware_steam_frame(), 3);
}

TEST(Services, LayoutsMatchRedistributablePacking) {
#if RT_PLATFORM_WINDOWS
    EXPECT_EQ(sizeof(rt_steam_packing_sentinel), 32u);
    EXPECT_EQ(sizeof(rt_steam_callback_msg), 24u);
#else
    EXPECT_EQ(sizeof(rt_steam_packing_sentinel), 24u);
    EXPECT_EQ(sizeof(rt_steam_callback_msg), 20u);
#endif
    EXPECT_EQ(offsetof(rt_steam_callback_msg, param), 8u);
    EXPECT_EQ(offsetof(rt_steam_callback_msg, param_size), 16u);
    EXPECT_EQ(sizeof(rt_steam_server_connect_failure), 8u);
    EXPECT_EQ(sizeof(rt_steam_servers_disconnected), 4u);
    EXPECT_EQ(sizeof(rt_steam_game_overlay_activated), 12u);
    EXPECT_EQ(offsetof(rt_steam_game_overlay_activated, app_id), 4u);
    EXPECT_EQ(sizeof(rt_steam_api_call_completed), 16u);
    EXPECT_EQ(offsetof(rt_steam_api_call_completed, param_size), 12u);
    EXPECT_EQ(sizeof(rt_steam_dlc_installed), 4u);
    EXPECT_EQ(sizeof(rt_steam_number_of_current_players), 8u);
    EXPECT_EQ(offsetof(rt_steam_number_of_current_players, players), 4u);
}

TEST(Services, NeutralBeforeInit) {
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_NOT_STARTED);
    EXPECT_EQ(rt_services_platform_get_is_available(), 0);
    EXPECT_EQ(take(rt_services_platform_get_provider()), std::string(""));
    EXPECT_EQ(take(rt_services_platform_get_user_id()), std::string(""));
    EXPECT_EQ(take(rt_services_platform_get_user_name()), std::string(""));
    EXPECT_EQ(take(rt_services_platform_get_language()), std::string(""));
    EXPECT_EQ(rt_services_platform_get_is_licensed(), 0);
    EXPECT_EQ(rt_services_platform_get_is_online(), 0);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_IDENTITY), 0);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    rt_string dlc = rt_const_cstr("1234567");
    EXPECT_EQ(rt_services_platform_is_dlc_installed(dlc), 0);
    rt_string_unref(dlc);
    rt_services_platform_update();

    rt_string steam = rt_const_cstr("steam");
    rt_string upper = rt_const_cstr("STEAM");
    rt_string nope = rt_const_cstr("nope");
    EXPECT_EQ(rt_services_platform_has_provider(steam), 1);
    EXPECT_EQ(rt_services_platform_has_provider(upper), 1);
    EXPECT_EQ(rt_services_platform_has_provider(nope), 0);
    rt_string_unref(steam);
    rt_string_unref(upper);
    rt_string_unref(nope);

    void *request = rt_services_platform_request_player_count();
    ASSERT_TRUE(request != nullptr);
    EXPECT_EQ(rt_services_request_get_kind(request), RT_SERVICES_REQUEST_PLAYER_COUNT);
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Services: no platform services provider is started"));
    release(request);

    EXPECT_EQ(rt_services_steam_get_is_active(), 0);
    EXPECT_EQ(rt_services_steam_get_steam_id(), 0);
    EXPECT_EQ(rt_services_steam_get_hardware_type(), RT_SERVICES_STEAM_HARDWARE_UNKNOWN);
}

TEST(Services, UnknownProviderFails) {
    rt_services_platform_shutdown();
    InitOutcome outcome = init("xbox", "1");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.text, std::string("Services: unknown provider 'xbox' (available: steam)"));
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_UNKNOWN_PROVIDER);
    EXPECT_TRUE(hasDiagnostic(outcome.text));
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_NOT_STARTED);
}

TEST(Services, MissingLibraryReportsNotFound) {
    rt_services_platform_shutdown();
    const std::string missing = fake165().path + ".missing";
    setEnv(kOverrideEnv, missing.c_str());
    InitOutcome outcome = init("steam", "480");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.text, "Steam: steam_api library not found: " + missing);
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_LIBRARY_NOT_FOUND);
    EXPECT_EQ(rt_services_platform_get_is_available(), 0);
    EXPECT_EQ(take(rt_services_platform_get_user_name()), std::string(""));
    EXPECT_TRUE(hasDiagnostic(outcome.text));
    rt_services_platform_shutdown();
}

TEST(Services, DefaultPathIsBesideExecutable) {
    rt_services_platform_shutdown();
    setEnv(kOverrideEnv, "");
    char *exe_dir = rt_path_exe_dir_cstr();
    ASSERT_TRUE(exe_dir != nullptr);
    std::string expected = std::string("Steam: steam_api library not found: ") + exe_dir +
                           RT_PATH_SEPARATOR_STR + expectedLibraryFileName();
    std::free(exe_dir);
    InitOutcome outcome = init("steam", "480");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.text, expected);
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_LIBRARY_NOT_FOUND);
    rt_services_platform_shutdown();
}

TEST(Services, MissingCoreExportIsIncompatible) {
    rt_services_platform_shutdown();
    setEnv(kOverrideEnv, ZANNA_FAKE_STEAM_LIBRARY_NOINIT);
    InitOutcome outcome = init("steam", "480");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.text,
              std::string("Steam: ") + ZANNA_FAKE_STEAM_LIBRARY_NOINIT +
                  " is missing export 'SteamAPI_InitFlat' (Steamworks SDK "
                  "1.61-1.65 redistributable required)");
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_LIBRARY_INCOMPATIBLE);
    rt_services_platform_shutdown();
}

TEST(Services, InitFailuresMapToStatus) {
    const FakeSteam &fake = fake165();
    useFake(fake);

    fake.setScenario("no-client");
    InitOutcome no_client = init("steam", "480");
    EXPECT_FALSE(no_client.ok);
    EXPECT_EQ(no_client.text,
              std::string("Steam: SteamAPI_InitFlat failed (NoSteamClient): Steam "
                          "client is not running (fake)"));
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_CLIENT_NOT_RUNNING);

    fake.setScenario("version-mismatch");
    InitOutcome mismatch = init("steam", "480");
    EXPECT_EQ(mismatch.text,
              std::string("Steam: SteamAPI_InitFlat failed (VersionMismatch): Steam "
                          "client is too old (fake)"));
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_VERSION_MISMATCH);

    fake.setScenario("generic");
    InitOutcome generic = init("steam", "480");
    EXPECT_EQ(generic.text,
              std::string("Steam: SteamAPI_InitFlat failed (FailedGeneric): no details reported"));
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_INIT_FAILED);
    EXPECT_EQ(rt_services_platform_get_is_available(), 0);
    EXPECT_EQ(fake.initCount(), 0);
    rt_services_platform_shutdown();
}

TEST(Services, MalformedAppIdTraps) {
    useFake(fake165());
    EXPECT_TRAP_MESSAGE(
        rt_services_platform_init(rt_const_cstr("steam"), rt_const_cstr("abc")),
        "Services.Platform.Init: Steam app id 'abc' must be an integer in 1..4294967295");
    EXPECT_TRAP_MESSAGE(
        rt_services_platform_init(rt_const_cstr("steam"), rt_const_cstr("0")),
        "Services.Platform.Init: Steam app id '0' must be an integer in 1..4294967295");
    EXPECT_TRAP_MESSAGE(
        rt_services_platform_init(rt_const_cstr("steam"), rt_const_cstr("4294967296")),
        "Services.Platform.Init: Steam app id '4294967296' must be an integer in 1..4294967295");
    EXPECT_TRAP_MESSAGE(
        rt_services_platform_init(rt_const_cstr("steam"), rt_const_cstr("-5")),
        "Services.Platform.Init: Steam app id '-5' must be an integer in 1..4294967295");
    EXPECT_EQ(rt_services_platform_get_is_available(), 0);
    EXPECT_EQ(fake165().initCount(), 0);
}

TEST(Services, SteamStartsAndReportsIdentity) {
    const FakeSteam &fake = fake165();
    const bool app_id_preset = hasEnv("SteamAppId");
    useFake(fake);

    InitOutcome outcome = init("steam", "480");
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.text, std::string("steam"));
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_OK);
    EXPECT_EQ(rt_services_platform_get_is_available(), 1);
    EXPECT_EQ(take(rt_services_platform_get_provider()), std::string("steam"));
    EXPECT_EQ(take(rt_services_platform_get_app_id()), std::string("480"));
    EXPECT_EQ(take(rt_services_platform_get_user_id()), std::string("76561198000000001"));
    EXPECT_EQ(take(rt_services_platform_get_user_name()), std::string("Zanna Tester"));
    EXPECT_EQ(take(rt_services_platform_get_language()), std::string("english"));
    EXPECT_EQ(rt_services_platform_get_is_licensed(), 1);
    EXPECT_EQ(rt_services_platform_get_is_online(), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_IDENTITY), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_LICENSING), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_LANGUAGE), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_PLAYER_COUNT), 1);
    EXPECT_EQ(rt_services_platform_has_feature(99), 0);

    rt_string installed = rt_const_cstr("1234567");
    rt_string absent = rt_const_cstr("7");
    EXPECT_EQ(rt_services_platform_is_dlc_installed(installed), 1);
    EXPECT_EQ(rt_services_platform_is_dlc_installed(absent), 0);
    rt_string_unref(installed);
    rt_string_unref(absent);

    EXPECT_EQ(rt_services_steam_get_is_active(), 1);
    EXPECT_EQ(rt_services_steam_get_steam_id(), INT64_C(76561198000000001));
    EXPECT_EQ(rt_services_steam_get_hardware_type(), RT_SERVICES_STEAM_HARDWARE_STEAM_FRAME);
    EXPECT_EQ(rt_services_steam_get_is_under_proton(), 1);
    EXPECT_EQ(rt_services_steam_get_is_big_picture(), 0);
    EXPECT_EQ(take(rt_services_steam_get_library_path()), fake.path);
    EXPECT_EQ(fake.initCount(), 1);
    EXPECT_EQ(fake.dispatchInitCount(), 1);
    if (!app_id_preset) {
        EXPECT_EQ(getEnv("SteamAppId"), std::string("480"));
        EXPECT_EQ(getEnv("SteamGameId"), std::string("480"));
#if !RT_PLATFORM_WINDOWS
        // Windows CRT environment snapshots may not observe SetEnvironmentVariableW updates made
        // after the fake's C runtime started; the real redistributable is loaded afterwards.
        EXPECT_EQ(fake.appIdEnvAtInit(), std::string("480"));
#endif
    }

    InitOutcome again = init("Steam", "480");
    EXPECT_TRUE(again.ok);
    EXPECT_EQ(fake.initCount(), 1);

    rt_services_platform_shutdown();
    EXPECT_EQ(fake.shutdownCount(), 1);
    EXPECT_EQ(rt_services_platform_get_status(), RT_SERVICES_STATUS_NOT_STARTED);
    EXPECT_EQ(rt_services_steam_get_is_active(), 0);
    EXPECT_EQ(rt_services_steam_get_hardware_type(), RT_SERVICES_STEAM_HARDWARE_UNKNOWN);
    EXPECT_EQ(take(rt_services_platform_get_user_name()), std::string(""));
}

TEST(Services, PumpDeliversScriptedEvents) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    ASSERT_TRUE(init("steam", "480").ok);

    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_SERVICE_CONNECTED);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_OVERLAY_CHANGED);
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_get_event_value(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_DLC_INSTALLED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("1234567"));
    EXPECT_EQ(rt_services_platform_get_event_value(), 1234567);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(""));
    EXPECT_EQ(fake.queuedCount(), 0);
    EXPECT_EQ(fake.protocolViolation(), 0);
    rt_services_platform_shutdown();
}

TEST(Services, ConnectionEventsDecode) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);

    std::vector<uint8_t> failure = payload(8);
    put32(failure, 0, 3);
    failure[4] = 1;
    fake.queue(RT_STEAM_CB_SERVER_CONNECT_FAILURE, failure, 8);
    std::vector<uint8_t> disconnected = payload(4);
    put32(disconnected, 0, 6);
    fake.queue(RT_STEAM_CB_SERVERS_DISCONNECTED, disconnected, 4);
    fake.queue(RT_STEAM_CB_STEAM_SHUTDOWN, payload(1), 1);
    fake.queue(RT_STEAM_CB_NEW_URL_LAUNCH_PARAMETERS, payload(1), 1);
    fake.queue(9999, payload(4), 4);

    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONNECT_FAILED);
    EXPECT_EQ(rt_services_platform_get_event_result_code(), 3);
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_SERVICE_DISCONNECTED);
    EXPECT_EQ(rt_services_platform_get_event_result_code(), 6);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_SERVICE_SHUTDOWN);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_LAUNCH_PARAMETERS_CHANGED);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_EQ(fake.queuedCount(), 0);
    rt_services_platform_shutdown();
}

TEST(Services, PayloadSizeMismatchIsRejected) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);

    fake.queue(RT_STEAM_CB_GAME_OVERLAY_ACTIVATED, payload(16), 16);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_TRUE(hasDiagnostic("Steam: callback 331 payload is 16 bytes; binding expects 12"));
    EXPECT_EQ(fake.protocolViolation(), 0);
    rt_services_platform_shutdown();
}

TEST(Services, EventQueueDropsOldest) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);

    for (uint32_t i = 0; i < 300; ++i) {
        std::vector<uint8_t> dlc = payload(4);
        put32(dlc, 0, i + 1);
        fake.queue(RT_STEAM_CB_DLC_INSTALLED, dlc, 4);
    }
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_get_dropped_events(), 44);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_DLC_INSTALLED);
    EXPECT_EQ(rt_services_platform_get_event_value(), 45);
    EXPECT_EQ(drainEventKinds().size(), 255u);
    EXPECT_TRUE(hasDiagnostic("Services: event queue full; dropped oldest event"));
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_platform_get_dropped_events(), 0);
}

TEST(Services, PlayerCountRequestCompletes) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);

    void *request = rt_services_platform_request_player_count();
    ASSERT_TRUE(request != nullptr);
    EXPECT_EQ(rt_services_request_get_is_done(request), 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(request), 1);
    EXPECT_EQ(rt_services_request_get_value(request), 42);
    EXPECT_EQ(rt_services_request_get_result_code(request), 1);
    EXPECT_EQ(take(rt_services_request_get_error(request)), std::string(""));
    release(request);
    rt_services_platform_shutdown();
}

TEST(Services, ShutdownCancelsPendingRequests) {
    useFake(fake165());
    ASSERT_TRUE(init("steam", "480").ok);
    void *request = rt_services_platform_request_player_count();
    ASSERT_TRUE(request != nullptr);
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
    EXPECT_EQ(rt_services_request_get_result_code(request), 0);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Services: request cancelled by Platform.Shutdown()"));
    release(request);
}

TEST(Services, FramePumpHookRunsOnlyWhileStarted) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    ASSERT_TRUE(init("steam", "480").ok);

    rt_service_hooks_run_frame_pump();
    EXPECT_EQ(fake.runFrameCount(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_SERVICE_CONNECTED);

    rt_services_platform_shutdown();
    rt_service_hooks_run_frame_pump();
    EXPECT_EQ(fake.runFrameCount(), 1);
}

TEST(Services, OlderRedistributableProfileUsesDeckQuery) {
    const FakeSteam &fake = fake164();
    useFake(fake);
    InitOutcome outcome = init("steam", "480");
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(rt_services_steam_get_hardware_type(), RT_SERVICES_STEAM_HARDWARE_STEAM_DECK);
    EXPECT_EQ(rt_services_steam_get_is_under_proton(), 0);
    EXPECT_EQ(take(rt_services_platform_get_user_name()), std::string("Zanna Tester"));
    EXPECT_EQ(rt_services_platform_get_is_licensed(), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_IDENTITY), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_LICENSING), 1);
    EXPECT_EQ(take(rt_services_steam_get_library_path()), fake.path);
    rt_services_platform_shutdown();
}

TEST(Services, MalformedDlcIdTrapsWhileSteamIsActive) {
    useFake(fake165());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_TRAP_MESSAGE(rt_services_platform_is_dlc_installed(rt_const_cstr("12ab")),
                        "Services.Platform.IsDlcInstalled: Steam DLC id '12ab' must be an integer "
                        "in 1..4294967295");
    rt_services_platform_shutdown();
}

TEST(Services, RestartAppIfNecessary) {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setRestart(1);
    EXPECT_EQ(rt_services_steam_restart_app_if_necessary(480), 1);
    EXPECT_EQ(fake.lastRestartAppId(), 480u);
    fake.setRestart(0);
    EXPECT_EQ(rt_services_steam_restart_app_if_necessary(4294967295LL), 0);
    EXPECT_EQ(fake.lastRestartAppId(), 4294967295u);

    EXPECT_TRAP_MESSAGE(rt_services_steam_restart_app_if_necessary(0),
                        "Services.Steam.RestartAppIfNecessary: app id 0 must be in 1..4294967295");
    EXPECT_TRAP_MESSAGE(
        rt_services_steam_restart_app_if_necessary(4294967296LL),
        "Services.Steam.RestartAppIfNecessary: app id 4294967296 must be in 1..4294967295");

    const std::string missing = fake.path + ".missing";
    setEnv(kOverrideEnv, missing.c_str());
    EXPECT_EQ(rt_services_steam_restart_app_if_necessary(480), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: steam_api library not found: " + missing));
}

TEST(Services, StatefulMembersRequireMainThread) {
    rt_services_platform_shutdown();
    g_trap_jump = false;
    g_last_trap.clear();
    int64_t status = -1;
    std::thread worker([&status]() { status = rt_services_platform_get_status(); });
    worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Platform.Status must be called on the main thread"));
    EXPECT_EQ(status, RT_SERVICES_STATUS_NOT_STARTED);

    g_last_trap.clear();
    int64_t steam_id = -1;
    std::thread steam_worker([&steam_id]() { steam_id = rt_services_steam_get_steam_id(); });
    steam_worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Steam.SteamId must be called on the main thread"));
    EXPECT_EQ(steam_id, 0);

    // Constants stay readable from any thread.
    g_last_trap.clear();
    int64_t ok = -1;
    std::thread constant_worker([&ok]() { ok = rt_services_status_ok(); });
    constant_worker.join();
    EXPECT_EQ(ok, 0);
    EXPECT_EQ(g_last_trap, std::string(""));
}

TEST(Services, DiagnosticsKeepNewestEntries) {
    rt_services_platform_shutdown();
    for (int i = 0; i < 40; ++i) {
        std::string provider = "missing" + std::to_string(i);
        (void)init(provider.c_str(), "1");
    }
    std::vector<std::string> entries = diagnostics();
    ASSERT_EQ(entries.size(), static_cast<size_t>(RT_SERVICES_DIAGNOSTIC_CAPACITY));
    EXPECT_EQ(entries.back(),
              std::string("Services: unknown provider 'missing39' (available: steam)"));
    EXPECT_EQ(entries.front(),
              std::string("Services: unknown provider 'missing8' (available: steam)"));
    rt_services_platform_shutdown();
}

// Sticky GPU-presenter note: keep this test last in the file.
TEST(Services, InitAfterGpuPresenterRecordsOverlayWarning) {
    useFake(fake165());
    rt_service_hooks_note_gpu_presenter();
    EXPECT_EQ(rt_service_hooks_gpu_presenter_created(), 1);
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_TRUE(hasDiagnostic("Steam: initialized after a GPU-presented window was created; the "
                              "desktop overlay may not attach"));
    rt_services_platform_shutdown();
}

int main(int argc, char **argv) {
    rt_set_main_thread();
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
