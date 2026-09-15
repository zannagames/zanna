//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesTestSupport.hpp
// Purpose: Shared fixtures for the Zanna.Services test binaries: the vm_trap
//          override with EXPECT_TRAP_MESSAGE, runtime string and environment
//          helpers, Platform.Init and Diagnostics helpers, and a typed view of
//          the fake steam_api libraries' ZannaFakeSteam_* control exports.
// Key invariants:
//   - Include from exactly one translation unit per test binary: it defines
//     vm_trap and the trap-capture globals.
//   - Fake libraries are selected through ZANNA_SERVICES_STEAM_LIBRARY and are
//     opened through the runtime's own dynamic-library adapter.
//   - Traps are observed through a vm_trap override that either records and
//     returns (worker-thread checks) or jumps back to the test.
// Ownership/Lifetime:
//   - Helpers release every runtime object and string they create.
// Links: src/tests/runtime/RTServicesTests.cpp,
//        src/tests/runtime/RTServicesFeatureTests.cpp,
//        src/tests/runtime/RTServicesFakeSteamApi.c
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_args.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_services.h"
#include "rt_services_dynlib.h"
#include "rt_string.h"
#include "tests/TestHarness.hpp"

#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace services_test {

inline jmp_buf g_trap_jmp;
inline bool g_trap_jump = false;
inline std::string g_last_trap;
inline int g_trap_count = 0;

} // namespace services_test

/// @brief Test trap hook: records the message and optionally jumps back to the test.
/// @param msg Trap message.
extern "C" void vm_trap(const char *msg) {
    services_test::g_last_trap = msg ? msg : "";
    ++services_test::g_trap_count;
    if (services_test::g_trap_jump)
        longjmp(services_test::g_trap_jmp, 1);
}

/// @brief Run @p expr and require that it traps with exactly @p message.
#define EXPECT_TRAP_MESSAGE(expr, message)                                                         \
    do {                                                                                           \
        services_test::g_last_trap.clear();                                                        \
        services_test::g_trap_jump = true;                                                         \
        bool trapped_ = false;                                                                     \
        if (setjmp(services_test::g_trap_jmp) == 0) {                                              \
            (void)(expr);                                                                          \
        } else {                                                                                   \
            trapped_ = true;                                                                       \
        }                                                                                          \
        services_test::g_trap_jump = false;                                                        \
        EXPECT_TRUE(trapped_);                                                                     \
        EXPECT_EQ(services_test::g_last_trap, std::string(message));                               \
    } while (0)

namespace services_test {

inline const char *const kOverrideEnv = "ZANNA_SERVICES_STEAM_LIBRARY";

/// @brief Copy a caller-owned runtime string into std::string and release it.
inline std::string take(rt_string value) {
    std::string out = value ? rt_string_cstr(value) : "";
    if (value)
        rt_string_unref(value);
    return out;
}

/// @brief Hold a temporary runtime string for the duration of a call.
struct Str {
    rt_string value;

    explicit Str(const char *text) : value(rt_const_cstr(text)) {}

    Str(const Str &) = delete;
    Str &operator=(const Str &) = delete;

    ~Str() {
        rt_string_unref(value);
    }

    operator rt_string() const {
        return value;
    }
};

/// @brief Set an environment variable through the runtime's portable helper.
inline void setEnv(const char *name, const char *value) {
    Str n(name);
    Str v(value);
    rt_env_set_var(n, v);
}

/// @brief Read an environment variable through the runtime's portable helper.
inline std::string getEnv(const char *name) {
    Str n(name);
    return take(rt_env_get_var(n));
}

/// @brief Report whether an environment variable is present.
inline bool hasEnv(const char *name) {
    Str n(name);
    return rt_env_has_var(n) != 0;
}

/// @brief Release one reference to a runtime object.
inline void release(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Outcome of Platform.Init.
struct InitOutcome {
    bool ok = false;  ///< Result was Ok.
    std::string text; ///< Ok payload or Err message.
};

/// @brief Call Platform.Init and decode its Result.
inline InitOutcome init(const char *provider, const char *app_id) {
    Str p(provider);
    Str id(app_id);
    void *result = rt_services_platform_init(p, id);
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
inline std::vector<std::string> diagnostics() {
    std::vector<std::string> out;
    void *seq = rt_services_platform_diagnostics();
    for (int64_t i = 0; seq && i < rt_seq_len(seq); ++i)
        out.push_back(take(rt_seq_get_str(seq, i)));
    release(seq);
    return out;
}

/// @brief Report whether any retained diagnostic equals @p message.
inline bool hasDiagnostic(const std::string &message) {
    for (const auto &entry : diagnostics()) {
        if (entry == message)
            return true;
    }
    return false;
}

/// @brief Count retained diagnostics equal to @p message.
inline int countDiagnostic(const std::string &message) {
    int count = 0;
    for (const auto &entry : diagnostics())
        count += entry == message ? 1 : 0;
    return count;
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

    void setStoreStatsResult(int32_t result) const {
        fn<void (*)(int32_t)>("ZannaFakeSteam_SetStoreStatsResult")(result);
    }

    void setUploadSuccess(int success) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetUploadSuccess")(success);
    }

    void fillLeaderboard(const char *name, int count) const {
        fn<void (*)(const char *, int)>("ZannaFakeSteam_FillLeaderboard")(name, count);
    }

    /// @brief Read a rich presence value; "<unset>" when the key is not set.
    std::string richPresence(const char *key) const {
        const char *value = fn<const char *(*)(const char *)>("ZannaFakeSteam_RichPresence")(key);
        return value ? value : "<unset>";
    }

    std::string lastUiCall() const {
        return fn<const char *(*)()>("ZannaFakeSteam_LastUiCall")();
    }

    void setOverlayEnabled(int enabled) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetOverlayEnabled")(enabled);
    }

    void setKeyboardsSupported(int supported) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetKeyboardsSupported")(supported);
    }

    void setTextInput(const char *text, int submit) const {
        fn<void (*)(const char *, int)>("ZannaFakeSteam_SetTextInput")(text, submit);
    }

    void setCloudAccountEnabled(int enabled) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetCloudAccountEnabled")(enabled);
    }

    /// @brief Script the launch URL and queue NewUrlLaunchParameters_t.
    void setLaunch(const char *command_line, const char *query) const {
        fn<void (*)(const char *, const char *)>("ZannaFakeSteam_SetLaunch")(command_line, query);
    }

    /// @brief Number of GetAchievementIcon calls since the reset.
    int iconRequestCount() const {
        return fn<int (*)()>("ZannaFakeSteam_IconRequestCount")();
    }

    /// @brief Script the size ISteamUtils::GetImageSize reports for loaded icons.
    void setIconSize(uint32_t width, uint32_t height) const {
        fn<void (*)(uint32_t, uint32_t)>("ZannaFakeSteam_SetIconSize")(width, height);
    }

    /// @brief Connect or disconnect a modeled Steam Input controller.
    void setControllerConnected(int index, int connected) const {
        fn<void (*)(int, int)>("ZannaFakeSteam_SetControllerConnected")(index, connected);
    }

    /// @brief Script whether a digital action is held on a modeled controller.
    void setDigitalAction(int index, const char *action, int pressed) const {
        fn<void (*)(int, const char *, int)>("ZannaFakeSteam_SetDigitalAction")(
            index, action, pressed);
    }

    /// @brief Script an analog action's values on a modeled controller.
    void setAnalogAction(int index, const char *action, double x, double y) const {
        fn<void (*)(int, const char *, double, double)>("ZannaFakeSteam_SetAnalogAction")(
            index, action, x, y);
    }

    /// @brief Describe the last Workshop query the fake created.
    std::string ugcLastQuery() const {
        return fn<const char *(*)()>("ZannaFakeSteam_UgcLastQuery")();
    }

    /// @brief Count Workshop queries the fake still holds open.
    int ugcOpenQueries() const {
        return fn<int (*)()>("ZannaFakeSteam_UgcOpenQueries")();
    }

    /// @brief Describe the last Workshop update submitted to the fake.
    std::string ugcLastUpdate() const {
        return fn<const char *(*)()>("ZannaFakeSteam_UgcLastUpdate")();
    }

    /// @brief Give or take away the fake app's controller mapping (manifests need one).
    void setInputMapping(int available) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetInputMapping")(available);
    }

    /// @brief Make ISteamInput::Init succeed or fail.
    void setInputInitResult(int ok) const {
        fn<void (*)(int)>("ZannaFakeSteam_SetInputInitResult")(ok);
    }

    /// @brief Read "initialized,explicit frames,device callbacks,frames" of the fake Steam Input.
    std::string inputState() const {
        return fn<const char *(*)()>("ZannaFakeSteam_InputState")();
    }

    /// @brief Read the manifest path the fake Steam Input accepted.
    std::string inputManifestPath() const {
        return fn<const char *(*)()>("ZannaFakeSteam_InputManifestPath")();
    }

    /// @brief Count Steam Input handle lookups since the reset.
    int inputHandleLookups() const {
        return fn<int (*)()>("ZannaFakeSteam_InputHandleLookups")();
    }

    /// @brief Set the EResult later global achievement percentage requests report.
    void setGlobalPercentagesResult(int32_t result) const {
        fn<void (*)(int32_t)>("ZannaFakeSteam_SetGlobalPercentagesResult")(result);
    }
};

/// @brief Return the fake built for the SDK 1.65 accessor set.
inline const FakeSteam &fake165() {
    static const FakeSteam fake(ZANNA_FAKE_STEAM_LIBRARY);
    return fake;
}

/// @brief Return the fake built for the SDK 1.61-1.64 accessor set.
inline const FakeSteam &fake164() {
    static const FakeSteam fake(ZANNA_FAKE_STEAM_LIBRARY_164);
    return fake;
}

/// @brief Return the fake exporting only the ADR 0352 core.
inline const FakeSteam &fakeCore() {
    static const FakeSteam fake(ZANNA_FAKE_STEAM_LIBRARY_CORE);
    return fake;
}

/// @brief Shut the session down, point the provider at @p fake, and reset it.
inline void useFake(const FakeSteam &fake) {
    rt_services_platform_shutdown();
    setEnv(kOverrideEnv, fake.path.c_str());
    fake.reset();
}

/// @brief Build a zeroed payload of @p size bytes.
inline std::vector<uint8_t> payload(size_t size) {
    return std::vector<uint8_t>(size, 0);
}

/// @brief Store a 32-bit value into a payload at @p offset.
inline void put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// @brief Drain every queued event kind.
inline std::vector<int64_t> drainEventKinds() {
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
inline const char *expectedLibraryFileName() {
#if RT_PLATFORM_WINDOWS
    return "steam_api64.dll";
#elif RT_PLATFORM_MACOS
    return "libsteam_api.dylib";
#else
    return "libsteam_api.so";
#endif
}

} // namespace services_test
