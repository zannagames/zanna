//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTWindowsRuntimeTests.cpp
// Purpose: Focused regression coverage for shared Windows runtime adapters.
//
// Key invariants:
//   - Finite Win32 deadlines never become the INFINITE sentinel.
//   - Concurrent WinSock initialization is idempotent.
//   - WinSock startup remains process-lifetime state and never touches the CRT
//     exit table used by CRT-less native PE executables.
//   - WinSock error classifiers cover documented non-blocking states.
//   - WinSock failure helpers publish deterministic errors and outputs.
//   - Entropy adapters reject invalid outputs without a fallback.
//   - Filesystem adapters reject malformed UTF-8/UTF-16 at Win32 boundaries
//     and recursive-delete protection fails closed.
//   - MSVC atomic subtraction preserves modulo arithmetic at signed minima.
//   - Machine queries preserve drive roots and long environment-backed paths.
//   - WASAPI source contracts use the CRT thread entry point, strict negotiated
//     format metadata, bounded repeated failures, and paired buffer release.
//   - Windows runtime mutex and event creation reports native allocation failure,
//     and parallel workers use the checked final-completion signal helper.
//   - Windows network workers use CRT-aware threads and download paths stay UTF-16.
//   - SaveData snapshots UTF-16 environment values instead of borrowed ACP strings.
//   - Child-process capture restricts inherited handles, uses CRT-aware threads,
//     and reports wait, pipe, and exit-query failures.
//
// Ownership/Lifetime:
//   - The test owns all worker threads and joins them before exit.
//
// Links: src/runtime/rt_win32_wait.h,
//        src/runtime/network/rt_socket_platform.h,
//        src/runtime/network/rt_entropy_platform.h,
//        src/runtime/io/rt_file_path.h, src/runtime/io/rt_file_stdio.h,
//        src/runtime/io/rt_dir_internal.h, src/runtime/threads/rt_parallel_ops.c,
//        src/lib/audio/src/vaud_platform_win32.c, src/common/RunProcess.cpp
//
//===----------------------------------------------------------------------===//

// WinSock2 must precede any adapter that includes windows.h.
#include "rt_socket_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "rt_dir_internal.h"
#include "rt_entropy_platform.h"
#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_machine.h"
#include "rt_platform.h"
#include "rt_win32_wait.h"

#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <process.h>
#include <string>
#include <thread>
#include <vector>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

#ifndef ZANNA_SOURCE_DIR
#define ZANNA_SOURCE_DIR "."
#endif

static void test_finite_wait_deadlines() {
    assert(rt_win32_deadline_after_ms(100, -1) == 100);
    assert(rt_win32_deadline_after_ms(100, 0) == 100);
    assert(rt_win32_deadline_after_ms(100, 25) == 125);
    assert(rt_win32_deadline_after_ms(ULLONG_MAX - 5, 10) == ULLONG_MAX);

    assert(rt_win32_wait_slice_at(100, 100) == 0);
    assert(rt_win32_wait_slice_at(100, 101) == 1);
    assert(rt_win32_wait_slice_at(100, 100 + (ULONGLONG)INFINITE) == RT_WIN32_MAX_FINITE_WAIT_MS);
    assert(rt_win32_wait_slice_at(0, ULLONG_MAX) == RT_WIN32_MAX_FINITE_WAIT_MS);
    assert(RT_WIN32_MAX_FINITE_WAIT_MS != INFINITE);
}

static void test_windows_filetime_conversion_contracts() {
    constexpr uint64_t epoch = UINT64_C(116444736000000000);
    assert(rt_windows_filetime_to_unix_units(epoch, UINT64_C(10000)) == 0);
    assert(rt_windows_filetime_to_unix_units(epoch + UINT64_C(10000), UINT64_C(10000)) == 1);
    assert(rt_windows_filetime_to_unix_units(epoch - UINT64_C(10000), UINT64_C(10000)) == -1);
    assert(rt_windows_filetime_to_unix_units(0, UINT64_C(10000)) == INT64_C(-11644473600000));
    assert(rt_windows_filetime_to_unix_units(UINT64_MAX, 1) == INT64_MAX);
    assert(rt_windows_filetime_to_unix_units(epoch, 0) == 0);
}

static unsigned __stdcall completed_thread_entry(void *) {
    return 0;
}

static void test_checked_win32_thread_join() {
    const uintptr_t raw = _beginthreadex(nullptr, 0, completed_thread_entry, nullptr, 0, nullptr);
    assert(raw != 0);
    const HANDLE thread = reinterpret_cast<HANDLE>(raw);
    assert(rt_win32_join_thread_handle(thread) == RT_WIN32_THREAD_JOINED);
    DWORD flags = 0;
    assert(!GetHandleInformation(thread, &flags));
    assert(GetLastError() == ERROR_INVALID_HANDLE);

    HANDLE current = nullptr;
    assert(DuplicateHandle(GetCurrentProcess(),
                           GetCurrentThread(),
                           GetCurrentProcess(),
                           &current,
                           SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                           FALSE,
                           0));
    assert(rt_win32_join_thread_handle(current) == RT_WIN32_THREAD_JOIN_CURRENT);
    assert(!GetHandleInformation(current, &flags));
    assert(GetLastError() == ERROR_INVALID_HANDLE);

    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(event != nullptr);
    assert(rt_win32_join_thread_handle(event) == RT_WIN32_THREAD_JOIN_FAILED);
    assert(!GetHandleInformation(event, &flags));
    assert(GetLastError() == ERROR_INVALID_HANDLE);
}

static void test_msvc_atomic_subtraction_wraparound() {
    volatile int value32 = 7;
    assert(rt_atomic_fetch_sub_i32(&value32, INT_MIN, __ATOMIC_SEQ_CST) == 7);
    assert((uint32_t)rt_atomic_load_i32(&value32, __ATOMIC_SEQ_CST) ==
           UINT32_C(7) - (uint32_t)INT_MIN);

    volatile int64_t value64 = INT64_C(11);
    assert(rt_atomic_fetch_sub_i64(&value64, INT64_MIN, __ATOMIC_SEQ_CST) == INT64_C(11));
    assert((uint64_t)rt_atomic_load_i64(&value64, __ATOMIC_SEQ_CST) ==
           UINT64_C(11) - (uint64_t)INT64_MIN);

    volatile size_t size_value = (size_t)13;
    const size_t decrement = (SIZE_MAX / 2U) + 1U;
    assert(rt_atomic_fetch_sub_size(&size_value, decrement, __ATOMIC_SEQ_CST) == (size_t)13);
    assert(rt_atomic_load_size(&size_value, __ATOMIC_SEQ_CST) == (size_t)13 - decrement);
}

static void test_concurrent_winsock_initialization() {
    std::array<std::thread, 8> workers;
    for (std::thread &worker : workers)
        worker = std::thread([] { rt_net_init_wsa(); });
    for (std::thread &worker : workers)
        worker.join();
    rt_net_init_wsa();
}

static void test_winsock_error_contracts() {
    assert(rt_socket_error_is_in_progress(WSAEWOULDBLOCK));
    assert(rt_socket_error_is_in_progress(WSAEINPROGRESS));
    assert(rt_socket_error_is_in_progress(WSAEALREADY));
    assert(!rt_socket_error_is_in_progress(WSAECONNRESET));
    assert(rt_socket_accept_interrupted_by_close(WSAESHUTDOWN));

    WSASetLastError(0);
    assert(wait_socket(INVALID_SOCK, 0, false) == -1);
    assert(WSAGetLastError() == WSAENOTSOCK);
    assert(wait_socket(0, -1, false) == -1);
    assert(WSAGetLastError() == WSAEINVAL);

    WSASetLastError(0);
    assert(rt_socket_shutdown_both(INVALID_SOCK) == SOCKET_ERROR);
    assert(WSAGetLastError() == WSAENOTSOCK);

    int pending_error = 12345;
    assert(!rt_socket_pending_error(INVALID_SOCK, &pending_error));
    assert(pending_error == 0);
    assert(!rt_socket_pending_error(INVALID_SOCK, nullptr));
}

static void test_winsock_startup_source_contract() {
    const std::filesystem::path sourcePath = std::filesystem::path(ZANNA_SOURCE_DIR) / "src" /
                                             "runtime" / "network" / "rt_socket_platform_win.c";
    std::ifstream input(sourcePath, std::ios::binary);
    assert(input);
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    assert(source.find("atexit(") == std::string::npos);
    assert(source.find("process lifetime") != std::string::npos);
}

static void test_entropy_argument_contracts() {
    uint64_t random_value = 0;
    assert(rt_entropy_platform_random_bytes(nullptr, 0) == 0);
    assert(rt_entropy_platform_random_bytes(nullptr, 1) == -1);
    assert(rt_entropy_platform_random_u64(nullptr) == -1);
    assert(rt_entropy_platform_random_u64(&random_value) == 0);
}

static void test_strict_windows_path_transcoding() {
    const char valid_utf8[] = "zanna-\xE6\x9D\xB1\xE4\xBA\xAC";
    wchar_t *wide = rt_file_path_utf8_to_wide(valid_utf8);
    assert(wide != nullptr);
    rt_string round_trip = rt_file_path_wide_to_string(wide);
    assert(round_trip != nullptr);
    assert(std::strcmp(rt_string_cstr(round_trip), valid_utf8) == 0);
    rt_str_release_maybe(round_trip);
    std::free(wide);

    const char malformed_utf8[] = "\x78\xC0\xAF";
    assert(rt_file_path_utf8_to_wide(malformed_utf8) == nullptr);
    assert(rt_dir_win_utf8_to_wide(malformed_utf8) == nullptr);

    const wchar_t malformed_utf16[] = {(wchar_t)0xD800, L'\0'};
    rt_string checked_dir = nullptr;
    assert(!rt_dir_win_wide_to_string_checked(malformed_utf16, &checked_dir));
    assert(checked_dir == nullptr);
    rt_string malformed_file = rt_file_path_wide_to_string(malformed_utf16);
    rt_string malformed_dir = rt_dir_win_wide_to_string(malformed_utf16);
    assert(rt_str_len(malformed_file) == 0);
    assert(rt_str_len(malformed_dir) == 0);
    rt_str_release_maybe(malformed_file);
    rt_str_release_maybe(malformed_dir);

    rt_string checked_valid = nullptr;
    assert(rt_dir_win_wide_to_string_checked(L"zanna-\x6771\x4EAC", &checked_valid));
    assert(std::strcmp(rt_string_cstr(checked_valid), valid_utf8) == 0);
    rt_str_release_maybe(checked_valid);
}

static void test_utf8_stdio_open_and_inheritance() {
    wchar_t tempRoot[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempRoot)), tempRoot);
    assert(length > 0 && length < std::size(tempRoot));
    const std::filesystem::path nativePath =
        std::filesystem::path(tempRoot) /
        (L"zanna-stdio-\x6771\x4EAC-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    rt_string utf8Path = rt_file_path_wide_to_string(nativePath.c_str());
    assert(utf8Path != nullptr);
    const char *path = rt_string_cstr(utf8Path);
    assert(path != nullptr && path[0] != '\0');

    FILE *output = rt_file_stdio_open_utf8(path, "wb");
    assert(output != nullptr);
    const intptr_t nativeHandle = _get_osfhandle(_fileno(output));
    assert(nativeHandle != -1);
    DWORD flags = HANDLE_FLAG_INHERIT;
    assert(GetHandleInformation(reinterpret_cast<HANDLE>(nativeHandle), &flags));
    assert((flags & HANDLE_FLAG_INHERIT) == 0);
    HANDLE competingReader = CreateFileW(nativePath.c_str(),
                                         GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
    assert(competingReader == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_SHARING_VIOLATION);
    static constexpr char payload[] = "utf8-stdio";
    assert(std::fwrite(payload, 1, sizeof(payload), output) == sizeof(payload));
    assert(rt_file_stdio_flush_sync_close(output));

    FILE *input = rt_file_stdio_open_utf8(path, "rb");
    assert(input != nullptr);
    HANDLE competingWriter = CreateFileW(nativePath.c_str(),
                                         GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
    assert(competingWriter == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_SHARING_VIOLATION);
    char actual[sizeof(payload)]{};
    assert(std::fread(actual, 1, sizeof(actual), input) == sizeof(actual));
    assert(std::memcmp(actual, payload, sizeof(payload)) == 0);
    assert(std::fclose(input) == 0);

    char *temporaryPath = nullptr;
    FILE *temporary = rt_file_stdio_open_temp_for_replace_utf8(path, &temporaryPath);
    assert(temporary != nullptr && temporaryPath != nullptr);
    wchar_t *wideTemporary = rt_file_path_utf8_to_wide(temporaryPath);
    assert(wideTemporary != nullptr);
    HANDLE competingTemporary = CreateFileW(wideTemporary,
                                            GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
    assert(competingTemporary == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_SHARING_VIOLATION);
    std::free(wideTemporary);
    assert(std::fwrite(payload, 1, sizeof(payload), temporary) == sizeof(payload));
    assert(rt_file_stdio_flush_sync_close(temporary));
    assert(rt_file_stdio_unlink_utf8(temporaryPath) == 0);
    std::free(temporaryPath);

    const std::string missingSource = std::string(path) + ".missing";
    errno = 0;
    assert(!rt_file_stdio_replace_utf8(missingSource.c_str(), path));
    assert(errno == ENOENT);
    assert(rt_file_stdio_unlink_utf8(path) == 0);
    rt_str_release_maybe(utf8Path);

    const char malformed[] = "\xC0\xAF";
    assert(rt_file_stdio_open_utf8(malformed, "rb") == nullptr);
}

static void test_recursive_delete_path_guards() {
    assert(rt_dir_win_path_span_equal(L"C:\\Zanna\\Runtime", L"c:\\zANNA\\runtime", 16));
    assert(!rt_dir_win_path_span_equal(L"C:\\Zanna\\Runtime", L"C:\\Zanna\\Otherxx", 16));

    wchar_t *cwd = rt_dir_win_current_directory_alloc();
    assert(cwd != nullptr);
    rt_string cwd_utf8 = nullptr;
    assert(rt_dir_win_wide_to_string_checked(cwd, &cwd_utf8));
    assert(rt_dir_win_path_matches_cwd_or_ancestor(rt_string_cstr(cwd_utf8)) == 1);
    rt_str_release_maybe(cwd_utf8);
    std::free(cwd);

    const char malformed_utf8[] = "\x78\xC0\xAF";
    assert(rt_dir_win_path_matches_cwd_or_ancestor(malformed_utf8) == 1);
}

class ScopedEnvironmentVariable {
  public:
    explicit ScopedEnvironmentVariable(const wchar_t *name) : name_(name) {
        DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (required == 0)
            return;
        value_.resize(required);
        DWORD length = GetEnvironmentVariableW(name_.c_str(), value_.data(), required);
        if (length > 0 && length < required) {
            value_.resize(length);
            present_ = true;
        } else {
            value_.clear();
        }
    }

    ~ScopedEnvironmentVariable() {
        SetEnvironmentVariableW(name_.c_str(), present_ ? value_.c_str() : nullptr);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
    ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

  private:
    std::wstring name_;
    std::wstring value_;
    bool present_{false};
};

static void test_machine_windows_snapshots() {
    assert(rt_machine_cores() >= 1);

    ScopedEnvironmentVariable savedTmp(L"TMP");
    ScopedEnvironmentVariable savedTemp(L"TEMP");
    assert(SetEnvironmentVariableW(L"TMP", L"C:\\"));
    assert(SetEnvironmentVariableW(L"TEMP", L"C:\\"));
    rt_string temp = rt_machine_temp();
    assert(temp != nullptr);
    assert(std::strcmp(rt_string_cstr(temp), "C:\\") == 0);
    rt_str_release_maybe(temp);

    ScopedEnvironmentVariable savedProfile(L"USERPROFILE");
    std::wstring longProfile = L"C:\\";
    longProfile.append(700, L'x');
    assert(SetEnvironmentVariableW(L"USERPROFILE", longProfile.c_str()));
    rt_string home = rt_machine_home();
    assert(home != nullptr);
    assert(rt_str_len(home) == longProfile.size());
    assert(std::strncmp(rt_string_cstr(home), "C:\\", 3) == 0);
    rt_str_release_maybe(home);
}

static std::string read_source(std::initializer_list<const char *> components);

static void test_wasapi_backend_source_contracts() {
    const std::filesystem::path sourcePath = std::filesystem::path(ZANNA_SOURCE_DIR) / "src" /
                                             "lib" / "audio" / "src" / "vaud_platform_win32.c";
    std::ifstream input(sourcePath, std::ios::binary);
    assert(input);
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    assert(source.find("_beginthreadex") != std::string::npos);
    assert(source.find("CreateThread(NULL") == std::string::npos);
    assert(source.find("fmt->nBlockAlign != expected_block_align") != std::string::npos);
    assert(source.find("fmt->nAvgBytesPerSec != expected_bytes_per_second") != std::string::npos);
    assert(source.find("consecutive_padding_failures >= 8") != std::string::npos);
    assert(source.find("consecutive_buffer_failures >= 8") != std::string::npos);
    assert(source.find("WASAPI failed to release a render buffer") != std::string::npos);
    assert(source.find("valid_bits == 0 || valid_bits > fmt->wBitsPerSample") != std::string::npos);
    assert(source.find("IAudioClient_Reset(plat->client)") != std::string::npos);
    assert(source.find("FAILED(hr) && hr != RPC_E_CHANGED_MODE") == std::string::npos);
    assert(source.find("Cannot resume an inactive WASAPI client") != std::string::npos);
    assert(source.find("InterlockedExchange(&plat->paused, 0)") != std::string::npos);
    assert(source.find("InitializeCriticalSectionEx") != std::string::npos);
    assert(source.find("vaud_win32_channel_mask_count") != std::string::npos);
    assert(source.find("SPEAKER_FRONT_LEFT") != std::string::npos);
    assert(source.find("SPEAKER_FRONT_RIGHT") != std::string::npos);
    assert(source.find("render_left_channel") != std::string::npos);
    assert(source.find("render_right_channel") != std::string::npos);
    assert(source.find("thread_exited") != std::string::npos);
    assert(source.find("worker_thread_id") != std::string::npos);
    assert(source.find("shutdown must run on the context owner thread") != std::string::npos);
    assert(source.find("long double millis") == std::string::npos);
    assert(source.find("whole_seconds") != std::string::npos);
    assert(source.find("CreateEventW") != std::string::npos);
    assert(source.find("CreateEvent(NULL") == std::string::npos);
    assert(source.find("Failed to signal the WASAPI audio thread to stop") != std::string::npos);
    assert(source.find("Failed to stop the WASAPI audio client") != std::string::npos);
    assert(source.find("Failed to close the WASAPI render event") != std::string::npos);

    const std::string coordinator = read_source({"src", "lib", "audio", "src", "vaud.c"});
    assert(coordinator.find("WaitForSingleObject(*event, INFINITE)") == std::string::npos);
    assert(coordinator.find("WaitForSingleObject(*event, 1U)") != std::string::npos);
    assert(coordinator.find("Failed to reset a music refill event") != std::string::npos);
    assert(coordinator.find("Failed to signal music refill completion") != std::string::npos);
}

static std::string read_source(std::initializer_list<const char *> components) {
    std::filesystem::path path(ZANNA_SOURCE_DIR);
    for (const char *component : components)
        path /= component;
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void test_windows_network_worker_source_contracts() {
    constexpr std::array<const char *, 4> files = {
        "rt_ws_server.c", "rt_wss_server.c", "rt_http_server.c", "rt_https_server.c"};
    for (const char *file : files) {
        const std::string source = read_source({"src", "runtime", "network", file});
        assert(source.find("_beginthreadex") != std::string::npos);
        assert(source.find("CreateThread(NULL") == std::string::npos);
        assert(source.find("unsigned __stdcall") != std::string::npos);
        assert(source.find("rt_win32_join_thread_handle") != std::string::npos);
        assert(source.find("WaitForSingleObject(") == std::string::npos);
    }
}

static void test_windows_synchronization_source_contracts() {
    constexpr std::array<const char *, 21> criticalSectionFiles = {
        "src/runtime/threads/rt_threads_win.c",
        "src/runtime/threads/rt_scheduler.c",
        "src/runtime/threads/rt_parallel_ops.c",
        "src/runtime/threads/rt_parallel.c",
        "src/runtime/threads/rt_future.c",
        "src/runtime/io/rt_zpak_reader.c",
        "src/runtime/io/rt_asset.c",
        "src/runtime/network/rt_ws_server.c",
        "src/runtime/network/rt_wss_server.c",
        "src/runtime/network/rt_http_server.c",
        "src/runtime/network/rt_http_client.c",
        "src/runtime/network/rt_restclient.c",
        "src/runtime/network/rt_sse.c",
        "src/runtime/network/rt_https_server.c",
        "src/runtime/network/rt_smtp.c",
        "src/runtime/network/rt_network_http.c",
        "src/runtime/text/rt_regex.c",
        "src/runtime/core/rt_gc.c",
        "src/runtime/core/rt_string_intern.c",
        "src/runtime/graphics/3d/rt_game3d.c",
        "src/runtime/graphics/3d/rt_game3d_surfaces.c",
    };
    for (const char *relativePath : criticalSectionFiles) {
        const std::string source = read_source({relativePath});
        assert(source.find("InitializeCriticalSectionEx(") != std::string::npos);
        assert(source.find("InitializeCriticalSection(") == std::string::npos);
    }

    constexpr std::array<const char *, 2> eventFiles = {
        "src/runtime/threads/rt_parallel_ops.c",
        "src/runtime/io/rt_watcher.c",
    };
    for (const char *relativePath : eventFiles) {
        const std::string source = read_source({relativePath});
        assert(source.find("CreateEventW(") != std::string::npos);
        assert(source.find("CreateEvent(") == std::string::npos);
    }

    const std::string parallel = read_source({"src", "runtime", "threads", "rt_parallel_ops.c"});
    assert(parallel.find("SetEvent(task->event)") == std::string::npos);
    assert(parallel.find("parallel_win_complete_one(task->remaining, task->event)") !=
           std::string::npos);
}

static void test_windows_unicode_storage_source_contracts() {
    const std::string http = read_source({"src", "runtime", "network", "rt_network_http_api.c"});
    assert(http.find("_wopen(") != std::string::npos);
    assert(http.find("_O_NOINHERIT") != std::string::npos);
    assert(http.find("MoveFileExW(") != std::string::npos);
    assert(http.find("_wstat64(") != std::string::npos);
    assert(http.find("_wchmod(") != std::string::npos);
    assert(http.find("_wremove(") != std::string::npos);
    assert(http.find("MoveFileExA(") == std::string::npos);

    const std::string savedata = read_source({"src", "runtime", "io", "rt_savedata.c"});
    assert(savedata.find("GetEnvironmentVariableW(") != std::string::npos);
    assert(savedata.find("getenv(\"APPDATA\")") == std::string::npos);
    assert(savedata.find("getenv(\"USERPROFILE\")") == std::string::npos);
    assert(savedata.find("getenv(\"HOMEDRIVE\")") == std::string::npos);
    assert(savedata.find("getenv(\"HOMEPATH\")") == std::string::npos);

    constexpr std::array<const char *, 8> utf8RuntimeFiles = {
        "src/runtime/audio/rt_audio_decode.c",
        "src/runtime/audio/rt_mp3.c",
        "src/runtime/audio/rt_ogg.c",
        "src/runtime/network/rt_tls_certs.c",
        "src/runtime/graphics/3d/rt_game3d_persistence.c",
        "src/runtime/graphics/3d/render/rt_cubemap3d.c",
        "src/runtime/graphics/3d/render/rt_lightbaker3d.c",
        "src/lib/audio/src/vaud_wav.c",
    };
    for (const char *relativePath : utf8RuntimeFiles) {
        const std::string file = read_source({relativePath});
        assert(file.find("fopen(") == std::string::npos);
    }
    const std::string image = read_source({"src", "lib", "gui", "src/widgets/vg_image.c"});
    assert(image.find("fopen(") == std::string::npos);
    const std::string guiFile = read_source({"src", "lib", "gui", "src/vg_file_stdio.h"});
    assert(guiFile.find("_O_NOINHERIT") != std::string::npos);
    assert(guiFile.find("_wsopen_s") != std::string::npos);
    assert(guiFile.find("_SH_DENYWR") != std::string::npos);
    const std::string audioFile = read_source({"src", "lib", "audio", "src/vaud_file_stdio.h"});
    assert(audioFile.find("_O_NOINHERIT") != std::string::npos);
    assert(audioFile.find("_wsopen_s") != std::string::npos);
    assert(audioFile.find("_SH_DENYWR") != std::string::npos);
    const std::string tlsVerify = read_source({"src", "runtime", "network", "rt_tls_verify_win.c"});
    assert(tlsVerify.find("_wsopen_s") != std::string::npos);
    assert(tlsVerify.find("_SH_DENYWR") != std::string::npos);
    assert(tlsVerify.find("_wfopen(") == std::string::npos);
    const std::string fileStdio = read_source({"src", "runtime", "io", "rt_file_stdio.h"});
    assert(fileStdio.find("rt_file_stdio_flush_sync_close") != std::string::npos);
    assert(fileStdio.find("_SH_DENYRW") != std::string::npos);
    const std::string persistence =
        read_source({"src", "runtime", "graphics", "3d/rt_game3d_persistence.c"});
    assert(persistence.find("rt_file_stdio_open_temp_for_replace_utf8") != std::string::npos);
    assert(persistence.find("remove(path)") == std::string::npos);
    const std::string lightBaker =
        read_source({"src", "runtime", "graphics", "3d/render/rt_lightbaker3d.c"});
    assert(lightBaker.find("rt_file_stdio_open_temp_for_replace_utf8") != std::string::npos);
    constexpr std::array<const char *, 6> atomicSaveFiles = {
        "src/runtime/graphics/2d/rt_pixels_io.c",
        "src/runtime/graphics/2d/rt_pixels_png.c",
        "src/runtime/graphics/2d/rt_tilemap_io.c",
        "src/runtime/graphics/3d/rt_game3d_persistence.c",
        "src/runtime/graphics/3d/render/rt_lightbaker3d.c",
        "src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c",
    };
    for (const char *relativePath : atomicSaveFiles) {
        const std::string saveSource = read_source({relativePath});
        assert(saveSource.find("rt_file_stdio_flush_sync_close") != std::string::npos);
    }
    const std::string ogg = read_source({"src", "runtime", "audio", "rt_ogg.c"});
    assert(ogg.find("rt_atomic_compare_exchange_i32") != std::string::npos);
}

static void test_win32_window_source_contracts() {
    const std::string source =
        read_source({"src", "lib", "graphics", "src", "vgfx_platform_win32.c"});
    assert(source.find("GetClassInfoExW(") != std::string::npos);
    assert(source.find("case WM_UNICHAR:") != std::string::npos);
    assert(source.find("case WM_SYSKEYDOWN:") != std::string::npos);
    assert(source.find("case WM_CAPTURECHANGED:") != std::string::npos);
    assert(source.find("SetCapture(") != std::string::npos);
    assert(source.find("ReleaseCapture(") != std::string::npos);
    assert(source.find("count > (UINT)VGFX_EVENT_QUEUE_SIZE") != std::string::npos);
    assert(source.find("unit_count > (size_t)VGFX_COMPOSITION_TEXT_CAPACITY") != std::string::npos);
    assert(source.find("if (!win32_adjust_window_rect_for_scale") != std::string::npos);
    assert(source.find("GetWindowLongW(") != std::string::npos);
    assert(source.find("SetWindowLongW(") != std::string::npos);
    assert(source.find("GetWindowLongA(") == std::string::npos);
    assert(source.find("SetWindowLongA(") == std::string::npos);
    assert(source.find("WaitForSingleObject(timer, INFINITE)") == std::string::npos);
    assert(source.find("Failed to apply Win32 DPI window bounds") != std::string::npos);
    assert(source.find("Failed to close Win32 pacing timer") != std::string::npos);
    assert(source.find("Win32 pacing timer timed out") != std::string::npos);
    assert(source.find("Win32 pacing timer wait failed") != std::string::npos);
    assert(source.find("Failed to refresh Win32 minimum-size constraints") != std::string::npos);
}

static void test_windows_machine_source_contracts() {
    const std::string source = read_source({"src", "runtime", "system", "rt_machine.c"});
    assert(source.find("machine_win32_clamp_u64") != std::string::npos);
    assert(source.find("GetVersionExW(") != std::string::npos);
    assert(source.find("GetVersionExA(") == std::string::npos);
}

static void test_windows_terminal_wrapper_source_contracts() {
    const std::string source = read_source({"src", "runtime", "core", "rt_term.c"});
    assert(source.find("rt_term_cursor_visible_i32(show != 0)") != std::string::npos);
    assert(source.find("rt_term_alt_screen_i32(enable != 0)") != std::string::npos);
    assert(source.find("if (ms > INT32_MAX)") != std::string::npos);
}

static void test_windows_run_process_source_contracts() {
    const std::string source = read_source({"src", "common", "RunProcess.cpp"});
    assert(source.find("PROC_THREAD_ATTRIBUTE_HANDLE_LIST") != std::string::npos);
    assert(source.find("EXTENDED_STARTUPINFO_PRESENT") != std::string::npos);
    assert(source.find("_beginthreadex") != std::string::npos);
    assert(source.find("CreateThread(") == std::string::npos);
    assert(source.find("pathFromUtf8(*cwd)") != std::string::npos);
    assert(source.find("failed while waiting for child process") != std::string::npos);
    assert(source.find("failed while capturing child stdout") != std::string::npos);
    assert(source.find("failed while capturing child stderr") != std::string::npos);
    assert(source.find("failed to query child exit code") != std::string::npos);
}

int main() {
    test_finite_wait_deadlines();
    test_windows_filetime_conversion_contracts();
    test_checked_win32_thread_join();
    test_msvc_atomic_subtraction_wraparound();
    test_concurrent_winsock_initialization();
    test_winsock_error_contracts();
    test_winsock_startup_source_contract();
    test_entropy_argument_contracts();
    test_strict_windows_path_transcoding();
    test_utf8_stdio_open_and_inheritance();
    test_recursive_delete_path_guards();
    test_machine_windows_snapshots();
    test_wasapi_backend_source_contracts();
    test_windows_network_worker_source_contracts();
    test_windows_synchronization_source_contracts();
    test_windows_unicode_storage_source_contracts();
    test_win32_window_source_contracts();
    test_windows_machine_source_contracts();
    test_windows_terminal_wrapper_source_contracts();
    test_windows_run_process_source_contracts();
    std::puts("RTWindowsRuntimeTests passed");
    return 0;
}
