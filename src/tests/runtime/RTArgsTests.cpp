//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTArgsTests.cpp
// Purpose: Verify runtime argument store helpers (rt_args_*).
// Key invariants:
//   - Argument mutation and concurrent reads preserve retained string ownership.
//   - Environment values round-trip as UTF-8, including across a concurrent
//     Windows size-probe/read race.
// Ownership/Lifetime:
//   - Every runtime string created or returned by this test is released.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#include "rt_platform.h"
#include "zanna/runtime/rt.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, std::strlen(s));
}

#if RT_PLATFORM_WINDOWS
/// @brief Race Win32's environment size probe against atomic value replacement.
/// @details `GetEnvironmentVariableW` returns a new required capacity when the
///          value grows between calls. The runtime must retry instead of using
///          that capacity as the length of the original, undersized buffer.
static void test_windows_environment_resize_race() {
    constexpr wchar_t kName[] = L"ZANNA_RT_ARGS_RESIZE_RACE";
    constexpr int kIterations = 2000;
    std::wstring large_value(16384, L'L');
    assert(SetEnvironmentVariableW(kName, L"s"));

    rt_string runtime_name = make_str("ZANNA_RT_ARGS_RESIZE_RACE");
    std::atomic<bool> start{false};
    std::atomic<bool> ok{true};
    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < kIterations; ++i) {
            const wchar_t *value = (i & 1) ? L"s" : large_value.c_str();
            if (!SetEnvironmentVariableW(kName, value))
                ok.store(false, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    std::thread reader([&]() {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < kIterations; ++i) {
            rt_string value = rt_env_get_var(runtime_name);
            const int64_t length = rt_str_len(value);
            const char *bytes = rt_string_cstr(value);
            const bool is_small = length == 1 && bytes && bytes[0] == 's';
            const bool is_large = length == (int64_t)large_value.size() && bytes &&
                                  bytes[0] == 'L' && bytes[length - 1] == 'L';
            if (!is_small && !is_large)
                ok.store(false, std::memory_order_relaxed);
            rt_string_unref(value);
        }
    });
    writer.join();
    reader.join();
    assert(ok.load(std::memory_order_relaxed));
    assert(SetEnvironmentVariableW(kName, NULL));
    rt_string_unref(runtime_name);
}
#endif

int main() {
    // Start clean
    rt_args_clear();
    assert(rt_args_count() == 0);

    // Pushing NULL treated as empty
    rt_args_push(NULL);
    assert(rt_args_count() == 1);
    rt_string s0 = rt_args_get(0);
    assert(rt_str_len(s0) == 0);
    rt_string_unref(s0);

    // Push two arguments and read them back
    rt_string a = make_str("foo");
    rt_string b = make_str("bar baz");
    rt_args_push(a);
    rt_args_push(b);
    // Caller still owns a/b and should release
    rt_string_unref(a);
    rt_string_unref(b);

    assert(rt_args_count() == 3);
    rt_string s1 = rt_args_get(1);
    rt_string s2 = rt_args_get(2);
    assert(std::strcmp(rt_string_cstr(s1), "foo") == 0);
    assert(std::strcmp(rt_string_cstr(s2), "bar baz") == 0);

    // cmdline joins with spaces and returns new string
    rt_string tail = rt_cmdline();
    assert(std::strcmp(rt_string_cstr(tail),
                       ""
                       " foo bar baz") == 0);

    // Release all retained strings from getters
    rt_string_unref(s1);
    rt_string_unref(s2);
    rt_string_unref(tail);

    // Clearing leaves store empty
    rt_args_clear();
    assert(rt_args_count() == 0);

    // Environment variables: missing, set, get, and UTF-8 round-trip.
    rt_string missing_name = make_str("ZANNA_RT_ARGS_MISSING_FOR_TEST");
    assert(rt_env_has_var(missing_name) == 0);
    rt_string missing_value = rt_env_get_var(missing_name);
    assert(std::strcmp(rt_string_cstr(missing_value), "") == 0);
    rt_string_unref(missing_name);
    rt_string_unref(missing_value);

    rt_string env_name = make_str("ZANNA_RT_ARGS_UTF8_VALUE");
    rt_string env_value = make_str("caf\xc3\xa9");
    rt_env_set_var(env_name, env_value);
    assert(rt_env_has_var(env_name) == 1);
    rt_string roundtrip = rt_env_get_var(env_name);
    assert(std::strcmp(rt_string_cstr(roundtrip), "caf\xc3\xa9") == 0);
    rt_string_unref(env_name);
    rt_string_unref(env_value);
    rt_string_unref(roundtrip);

#if RT_PLATFORM_WINDOWS
    test_windows_environment_resize_race();
#endif

    // VDOC-211: the legacy host-init flag is now an atomic once-init, so many
    // threads reading the argument store concurrently observe a single stable
    // count with no crash, corruption, or spin under contention. (The 0->1->2
    // import is one-shot per process, so this exercises the atomic read/CAS path
    // for regression rather than reproducing the original data race.)
    {
        rt_args_clear();
        rt_string alpha = make_str("alpha");
        rt_string beta = make_str("beta");
        rt_args_push(alpha);
        rt_args_push(beta);
        rt_string_unref(alpha);
        rt_string_unref(beta);
        const int64_t expected_count = rt_args_count();
        assert(expected_count == 2);

        std::atomic<bool> ok{true};
        std::vector<std::thread> workers;
        for (int t = 0; t < 8; ++t) {
            workers.emplace_back([&]() {
                for (int i = 0; i < 2000; ++i) {
                    if (rt_args_count() != expected_count)
                        ok.store(false);
                    rt_string arg0 = rt_args_get(0);
                    if (arg0)
                        rt_string_unref(arg0);
                }
            });
        }
        for (auto &w : workers)
            w.join();
        assert(ok.load());
        rt_args_clear();
    }

    return 0;
}
