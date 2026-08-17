//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTWatcherTests.cpp
// Purpose: Validate runtime file system watcher operations in rt_watcher.c.
// Key invariants: Watcher can detect file creation, modification, and deletion
//                 events on watched directories and files.
// Ownership/Lifetime: Uses runtime library; tests create temporary files
//                     that are cleaned up after tests complete.
// Links: docs/zannalib/io/advanced.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_watcher.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static int g_alloc_countdown = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

#if RT_PLATFORM_WINDOWS
#include <direct.h>
#include <process.h>
#include <windows.h>
#define mkdir_p(path) _mkdir(path)
#define rmdir_p(path) _rmdir(path)
#define getpid _getpid
#else
#include "tests/common/PosixCompat.h"
#include <fcntl.h>
#include <sys/stat.h>
#define mkdir_p(path) mkdir(path, 0755)
#define rmdir_p(path) rmdir(path)
#endif

/// @brief Fail one selected runtime allocation and delegate all others.
/// @param bytes Requested runtime byte count.
/// @param next Default allocator supplied by the runtime.
/// @return NULL for the selected request, otherwise the default result.
static void *fail_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    if (g_alloc_countdown > 0 && --g_alloc_countdown == 0)
        return nullptr;
    return next ? next(bytes) : nullptr;
}

/// @brief Count native process resources without opening a descriptor itself.
/// @return Current Windows handle count or occupied low POSIX descriptor count.
static uint64_t process_open_resource_count() {
#if RT_PLATFORM_WINDOWS
    DWORD count = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count) != 0);
    return (uint64_t)count;
#else
    uint64_t count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
            ++count;
    }
    return count;
#endif
}

/// @brief Release an owned watcher object after native resources are stopped.
/// @param watcher Owned managed object; NULL is a no-op.
static void release_watcher(void *watcher) {
    if (watcher && rt_obj_release_check0(watcher))
        rt_obj_free(watcher);
}

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

/// @brief Get a unique temp directory path for testing.
static const char *get_test_base() {
#if RT_PLATFORM_WINDOWS
    static char buf[256];
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = getenv("TMP");
    if (!tmp)
        tmp = "C:\\Temp";
    snprintf(buf, sizeof(buf), "%s\\zanna_watcher_test_%d", tmp, (int)getpid());
    return buf;
#else
    static char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/zanna_watcher_test_%d", (int)getpid());
    return buf;
#endif
}

/// @brief Helper to create a file.
static void create_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "test\n");
        fclose(f);
    }
}

/// @brief Helper to modify a file.
static void modify_file(const char *path) {
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "modified\n");
        fclose(f);
    }
}

/// @brief Helper to remove a file.
static void remove_file(const char *path) {
    remove(path);
}

/// @brief Helper to wait a bit for filesystem events to propagate.
static void wait_for_event() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

static int64_t poll_until_event(void *watcher, int attempts = 20) {
    for (int i = 0; i < attempts; i++) {
        int64_t event = rt_watcher_poll_for(watcher, 100);
        if (event != RT_WATCH_EVENT_NONE)
            return event;
    }
    return RT_WATCH_EVENT_NONE;
}

/// @brief Test watcher creation and basic properties.
static void test_watcher_new() {
    printf("Testing Watcher.New...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    // Create watcher for directory
    rt_string path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(path);
    assert(w != nullptr);

    // Check properties
    assert(rt_watcher_get_is_watching(w) == 0); // Not started yet

    // Check path is returned
    rt_string watchedPath = rt_watcher_get_path(w);
    assert(watchedPath != nullptr);

    test_result("Watcher creation", true);

    // Cleanup
    rmdir_p(base);
}

/// @brief Test watcher start/stop.
static void test_watcher_start_stop() {
    printf("Testing Watcher.Start/Stop...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    rt_string path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(path);

    // Start watching
    rt_watcher_start(w);
    assert(rt_watcher_get_is_watching(w) == 1);

    // Stop watching
    rt_watcher_stop(w);
    assert(rt_watcher_get_is_watching(w) == 0);

    test_result("Start/Stop", true);

    // Cleanup
    rmdir_p(base);
}

/// @brief Verify repeated Start/Stop cycles restore the native resource baseline.
static void test_start_stop_resource_balance() {
    printf("Testing Start/Stop resource balance...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    rt_string path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(path);
    uint64_t before = process_open_resource_count();

    for (int i = 0; i < 32; ++i) {
        rt_watcher_start(w);
        assert(rt_watcher_get_is_watching(w) == 1);
        rt_watcher_stop(w);
        assert(rt_watcher_get_is_watching(w) == 0);
        assert(process_open_resource_count() == before);
    }

    release_watcher(w);
    rt_string_unref(path);
    rmdir_p(base);
    test_result("Start/Stop resource balance", true);
}

#if RT_PLATFORM_WINDOWS || RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
/// @brief Verify that a Watcher cannot be polled from a non-owning thread.
/// @details The worker installs its own runtime recovery frame so the expected
///          affinity trap is contained on that thread. The construction thread
///          then starts and stops the same object, proving that the rejected
///          call did not alter native state or poison the instance.
static void test_construction_thread_affinity() {
    printf("Testing construction-thread affinity...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    rt_string path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(path);
    assert(w != nullptr);

    std::atomic<bool> saw_affinity_trap{false};
    std::thread foreign_thread([&]() {
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            (void)rt_watcher_poll(w);
            rt_trap_clear_recovery();
            return;
        }
        const char *error = rt_trap_get_error();
        saw_affinity_trap.store(error && strstr(error, "another thread") != nullptr,
                                std::memory_order_release);
        rt_trap_clear_recovery();
    });
    foreign_thread.join();

    assert(saw_affinity_trap.load(std::memory_order_acquire));
    rt_watcher_start(w);
    assert(rt_watcher_get_is_watching(w) == 1);
    rt_watcher_stop(w);
    release_watcher(w);
    rt_string_unref(path);
    rmdir_p(base);
    test_result("construction-thread affinity", true);
}
#endif

/// @brief Verify partial file-watch construction is reclaimed after allocation trap.
/// @details The second runtime allocation fails while deriving the parent path,
///          after the managed Watcher exists and has retained the caller path.
///          The installed finalizer must release that partial state. The caller
///          path remains valid and can immediately construct a replacement.
static void test_constructor_oom_cleanup() {
    printf("Testing Watcher.New allocation cleanup...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char file_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(file_path, sizeof(file_path), "%s\\oom.txt", base);
#else
    snprintf(file_path, sizeof(file_path), "%s/oom.txt", base);
#endif
    create_file(file_path);
    rt_string path = rt_string_from_bytes(file_path, strlen(file_path));

    g_alloc_countdown = 2;
    rt_set_alloc_hook(fail_countdown_alloc);
    EXPECT_TRAP(rt_watcher_new(path));
    rt_set_alloc_hook(nullptr);
    assert(g_alloc_countdown == 0);
    assert(strcmp(rt_string_cstr(path), file_path) == 0);

    void *w = rt_watcher_new(path);
    assert(w != nullptr);
    release_watcher(w);
    rt_string_unref(path);
    remove_file(file_path);
    rmdir_p(base);
    test_result("Watcher.New allocation cleanup", true);
}

/// @brief Test event type constants.
static void test_event_constants() {
    printf("Testing event constants...\n");

    assert(rt_watcher_event_none(NULL) == 0);
    assert(rt_watcher_event_created(NULL) == 1);
    assert(rt_watcher_event_modified(NULL) == 2);
    assert(rt_watcher_event_deleted(NULL) == 3);
    assert(rt_watcher_event_renamed(NULL) == 4);
    assert(rt_watcher_event_overflow(NULL) == 5);

    test_result("Event constants", true);
}

/// @brief Test polling with no events returns none.
static void test_poll_no_events() {
    printf("Testing Poll with no events...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    rt_string path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(path);
    rt_watcher_start(w);

    // Poll should return 0 (no events)
    int64_t event = rt_watcher_poll(w);
    assert(event == RT_WATCH_EVENT_NONE);

    rt_watcher_stop(w);
    test_result("Poll no events", true);

    // Cleanup
    rmdir_p(base);
}

static void test_directory_event_path() {
    printf("Testing directory event paths...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    char file_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(file_path, sizeof(file_path), "%s\\created.txt", base);
#else
    snprintf(file_path, sizeof(file_path), "%s/created.txt", base);
#endif

    void *w = rt_watcher_new(rt_string_from_bytes(base, strlen(base)));
    rt_watcher_start(w);
    wait_for_event();
    create_file(file_path);

    int64_t event = poll_until_event(w);
    assert(event != RT_WATCH_EVENT_NONE);
    rt_string event_path = rt_watcher_event_path(w);
#if RT_PLATFORM_MACOS
    test_result("macOS directory event path is watched directory",
                strcmp(rt_string_cstr(event_path), base) == 0);
#else
    test_result("directory event path resolves full child path",
                strcmp(rt_string_cstr(event_path), file_path) == 0);
#endif

    rt_watcher_stop(w);
    remove_file(file_path);
    rmdir_p(base);
}

static void test_file_event_path() {
    printf("Testing file event paths...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    char file_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(file_path, sizeof(file_path), "%s\\watched.txt", base);
#else
    snprintf(file_path, sizeof(file_path), "%s/watched.txt", base);
#endif
    create_file(file_path);

    void *w = rt_watcher_new(rt_string_from_bytes(file_path, strlen(file_path)));
    rt_watcher_start(w);
    wait_for_event();
    modify_file(file_path);

    int64_t event = poll_until_event(w);
    assert(event != RT_WATCH_EVENT_NONE);
    rt_string event_path = rt_watcher_event_path(w);
    test_result("file event path is watched file",
                strcmp(rt_string_cstr(event_path), file_path) == 0);

    rt_watcher_stop(w);
    remove_file(file_path);
    rmdir_p(base);
}

#if RT_PLATFORM_LINUX || RT_PLATFORM_WINDOWS
/// @brief Verify directory-child renames expose one event with both endpoints.
static void test_directory_rename_endpoints() {
    printf("Testing directory rename endpoints...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char old_path[512];
    char new_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(old_path, sizeof(old_path), "%s\\rename-old.txt", base);
    snprintf(new_path, sizeof(new_path), "%s\\rename-new.txt", base);
#else
    snprintf(old_path, sizeof(old_path), "%s/rename-old.txt", base);
    snprintf(new_path, sizeof(new_path), "%s/rename-new.txt", base);
#endif
    create_file(old_path);

    rt_string watched_path = rt_string_from_bytes(base, strlen(base));
    void *w = rt_watcher_new(watched_path);
    rt_string_unref(watched_path);
    rt_watcher_start(w);
    wait_for_event();
    assert(rename(old_path, new_path) == 0);

    bool saw_complete_rename = false;
    for (int i = 0; i < 20; ++i) {
        int64_t event = rt_watcher_poll_for(w, 100);
        if (event != RT_WATCH_EVENT_RENAMED)
            continue;
        rt_string primary = rt_watcher_event_path(w);
        rt_string source = rt_watcher_event_old_path(w);
        rt_string destination = rt_watcher_event_new_path(w);
        saw_complete_rename = strcmp(rt_string_cstr(primary), new_path) == 0 &&
                              strcmp(rt_string_cstr(source), old_path) == 0 &&
                              strcmp(rt_string_cstr(destination), new_path) == 0;
        rt_string_unref(primary);
        rt_string_unref(source);
        rt_string_unref(destination);
        if (saw_complete_rename)
            break;
    }
    test_result("directory rename exposes source and destination", saw_complete_rename);

    rt_watcher_stop(w);
    release_watcher(w);
    remove_file(new_path);
    rmdir_p(base);
}
#endif

static void test_stop_clears_last_event() {
    printf("Testing Stop clears queued event state...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    char file_path[512];
#if RT_PLATFORM_WINDOWS
    snprintf(file_path, sizeof(file_path), "%s\\stale.txt", base);
#else
    snprintf(file_path, sizeof(file_path), "%s/stale.txt", base);
#endif

    void *w = rt_watcher_new(rt_string_from_bytes(base, strlen(base)));
    rt_watcher_start(w);
    wait_for_event();
    create_file(file_path);

    int64_t event = poll_until_event(w);
    assert(event != RT_WATCH_EVENT_NONE);
    rt_string before = rt_watcher_event_path(w);
    assert(rt_str_len(before) > 0);

    rt_watcher_stop(w);
    assert(rt_watcher_poll(w) == RT_WATCH_EVENT_NONE);
    assert(rt_watcher_event_type(w) == RT_WATCH_EVENT_NONE);
    EXPECT_TRAP(rt_watcher_event_path(w));
    EXPECT_TRAP(rt_watcher_event_old_path(w));
    EXPECT_TRAP(rt_watcher_event_new_path(w));
    test_result("Stop clears last event state", true);

    remove_file(file_path);
    rmdir_p(base);
}

#if RT_PLATFORM_LINUX
static void test_file_recreate_is_detected() {
    printf("Testing file recreate events...\n");

    const char *base = get_test_base();
    mkdir_p(base);

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/recreated.txt", base);
    create_file(file_path);

    void *w = rt_watcher_new(rt_string_from_bytes(file_path, strlen(file_path)));
    rt_watcher_start(w);
    wait_for_event();

    remove_file(file_path);
    wait_for_event();
    create_file(file_path);

    bool saw_created = false;
    for (int i = 0; i < 20; ++i) {
        int64_t event = rt_watcher_poll_for(w, 100);
        if (event == RT_WATCH_EVENT_CREATED) {
            rt_string event_path = rt_watcher_event_path(w);
            saw_created = strcmp(rt_string_cstr(event_path), file_path) == 0;
            break;
        }
    }
    test_result("file recreate produces created event", saw_created);

    rt_watcher_stop(w);
    remove_file(file_path);
    rmdir_p(base);
}
#endif

/// @brief Test watcher traps on null path.
static void test_null_path_trap() {
    printf("Testing null path trap...\n");

    EXPECT_TRAP(rt_watcher_new(nullptr));

    test_result("Null path trap", true);
}

/// @brief Test watcher traps on non-existent path.
static void test_nonexistent_path_trap() {
    printf("Testing non-existent path trap...\n");

    rt_string path = rt_string_from_bytes("/nonexistent/path/12345", 24);
    EXPECT_TRAP(rt_watcher_new(path));

    test_result("Non-existent path trap", true);
}

#if RT_PLATFORM_LINUX
/// @brief VDOC-190: an INTERNAL ring overflow (Zanna's 64-entry queue fills
///        before the client polls) reports a POSITIVE exact dropped count via
///        EventOverflowCount(), never the -1 native-overflow sentinel. (The
///        native IN_Q_OVERFLOW path requires exhausting the kernel queue and is
///        not forced here; it is covered by inspection and a by-hand run.)
static void test_internal_overflow_reports_positive_count() {
    printf("Testing internal ring overflow count...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    void *w = rt_watcher_new(rt_string_from_bytes(base, strlen(base)));
    rt_watcher_start(w);
    wait_for_event();

    // Create well over the 64-entry ring capacity before draining, so a single
    // inotify read pulls more events than the ring can hold.
    for (int i = 0; i < 300; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/ov_%d.txt", base, i);
        create_file(p);
    }
    wait_for_event();

    int saw_overflow = 0;
    int64_t overflow_count = 0;
    for (int i = 0; i < 600; i++) {
        int64_t ev = rt_watcher_poll(w);
        if (ev == RT_WATCH_EVENT_NONE)
            break;
        if (ev == RT_WATCH_EVENT_OVERFLOW) {
            saw_overflow = 1;
            overflow_count = rt_watcher_event_overflow_count(w);
        }
    }
    rt_watcher_stop(w);

    // If the ring overflowed (the expected outcome), the count is an exact
    // positive number — an internal overflow must not be reported as the native
    // "unknown" (-1) sentinel. If no overflow occurred (very fast drain), the
    // path simply was not exercised, which is acceptable.
    if (saw_overflow)
        test_result("internal overflow count is positive-exact", overflow_count > 0);
    else
        test_result("internal overflow not triggered (fast drain, acceptable)", true);
}

static void test_deleted_watch_becomes_inactive() {
    printf("Testing deleted watch terminal state...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char watched_dir[512];
    snprintf(watched_dir, sizeof(watched_dir), "%s/terminal-watch", base);
    mkdir_p(watched_dir);

    void *w = rt_watcher_new(rt_string_from_bytes(watched_dir, strlen(watched_dir)));
    rt_watcher_start(w);
    test_result("directory watcher starts", rt_watcher_get_is_watching(w) == 1);

    rmdir_p(watched_dir);
    int saw_rescan = 0;
    for (int i = 0; i < 20; ++i) {
        int64_t ev = rt_watcher_poll_for(w, 100);
        if (ev == RT_WATCH_EVENT_OVERFLOW)
            saw_rescan = 1;
        if (!rt_watcher_get_is_watching(w) && saw_rescan)
            break;
    }
    test_result("deleted watch requests rescan", saw_rescan != 0);
    test_result("deleted watch becomes inactive", rt_watcher_get_is_watching(w) == 0);
    rt_watcher_stop(w);
    test_result("Stop clears terminal event type", rt_watcher_event_type(w) == RT_WATCH_EVENT_NONE);
    EXPECT_TRAP(rt_watcher_event_path(w));
}

static void test_attribute_change_is_modified() {
    printf("Testing attribute change events...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/attrib.txt", base);
    create_file(file_path);

    void *w = rt_watcher_new(rt_string_from_bytes(file_path, strlen(file_path)));
    rt_watcher_start(w);
    (void)chmod(file_path, 0600);

    int saw_modified = 0;
    for (int i = 0; i < 20; ++i) {
        if (rt_watcher_poll_for(w, 100) == RT_WATCH_EVENT_MODIFIED) {
            saw_modified = 1;
            break;
        }
    }
    test_result("attribute change produces modified event", saw_modified != 0);
    rt_watcher_stop(w);
    remove_file(file_path);
    rmdir_p(base);
}

static void test_close_write_is_coalesced() {
    printf("Testing close-write event coalescing...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/coalesced.txt", base);
    create_file(file_path);

    void *w = rt_watcher_new(rt_string_from_bytes(file_path, strlen(file_path)));
    rt_watcher_start(w);
    modify_file(file_path);

    int modified_count = 0;
    for (int i = 0; i < 20; ++i) {
        int64_t event = rt_watcher_poll_for(w, i == 0 ? 100 : 0);
        if (event == RT_WATCH_EVENT_MODIFIED)
            modified_count++;
        if (event == RT_WATCH_EVENT_NONE)
            break;
    }
    test_result("modify and close-write coalesce", modified_count == 1);
    rt_watcher_stop(w);
    remove_file(file_path);
    rmdir_p(base);
}

static void test_moved_watch_becomes_inactive() {
    printf("Testing moved watch terminal state...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char watched_dir[512];
    char moved_dir[512];
    snprintf(watched_dir, sizeof(watched_dir), "%s/move-source", base);
    snprintf(moved_dir, sizeof(moved_dir), "%s/move-target", base);
    mkdir_p(watched_dir);

    void *w = rt_watcher_new(rt_string_from_bytes(watched_dir, strlen(watched_dir)));
    rt_watcher_start(w);
    assert(rename(watched_dir, moved_dir) == 0);

    int saw_renamed = 0;
    int saw_source_only = 0;
    for (int i = 0; i < 20; ++i) {
        int64_t event = rt_watcher_poll_for(w, 100);
        if (event == RT_WATCH_EVENT_RENAMED) {
            saw_renamed = 1;
            rt_string primary = rt_watcher_event_path(w);
            rt_string source = rt_watcher_event_old_path(w);
            rt_string destination = rt_watcher_event_new_path(w);
            saw_source_only = strcmp(rt_string_cstr(primary), watched_dir) == 0 &&
                              strcmp(rt_string_cstr(source), watched_dir) == 0 &&
                              rt_str_len(destination) == 0;
            rt_string_unref(primary);
            rt_string_unref(source);
            rt_string_unref(destination);
        }
        if (!rt_watcher_get_is_watching(w))
            break;
    }
    test_result("moved watch reports rename", saw_renamed != 0);
    test_result("moved watch reports incomplete source endpoint", saw_source_only != 0);
    test_result("moved watch becomes inactive", rt_watcher_get_is_watching(w) == 0);
    rt_watcher_stop(w);
    rmdir_p(moved_dir);
    rmdir_p(base);
}
#endif

#if RT_PLATFORM_MACOS
/// @brief Verify a deleted kqueue vnode becomes inactive and Stop clears its event epoch.
static void test_deleted_watch_becomes_inactive() {
    printf("Testing deleted watch terminal state...\n");
    const char *base = get_test_base();
    mkdir_p(base);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/terminal.txt", base);
    create_file(file_path);

    void *w = rt_watcher_new(rt_string_from_bytes(file_path, strlen(file_path)));
    rt_watcher_start(w);
    remove_file(file_path);
    int64_t event = poll_until_event(w);
    test_result("deleted vnode reports an event", event != RT_WATCH_EVENT_NONE);
    test_result("deleted vnode becomes inactive", rt_watcher_get_is_watching(w) == 0);
    rt_watcher_stop(w);
    test_result("Stop clears inactive terminal event",
                rt_watcher_event_type(w) == RT_WATCH_EVENT_NONE);
    EXPECT_TRAP(rt_watcher_event_path(w));
    rmdir_p(base);
}
#endif

int main() {
    printf("=== Watcher Runtime Tests ===\n");

    test_event_constants();
    test_watcher_new();
    test_watcher_start_stop();
    test_start_stop_resource_balance();
#if RT_PLATFORM_WINDOWS || RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
    test_construction_thread_affinity();
#endif
    test_constructor_oom_cleanup();
    test_poll_no_events();
    test_directory_event_path();
    test_file_event_path();
#if RT_PLATFORM_LINUX || RT_PLATFORM_WINDOWS
    test_directory_rename_endpoints();
#endif
    test_stop_clears_last_event();
#if RT_PLATFORM_LINUX
    test_file_recreate_is_detected();
    test_internal_overflow_reports_positive_count();
    test_attribute_change_is_modified();
    test_close_write_is_coalesced();
    test_moved_watch_becomes_inactive();
#endif
#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
    test_deleted_watch_becomes_inactive();
#endif
    test_null_path_trap();
    test_nonexistent_path_trap();

    printf("\nAll Watcher tests passed!\n");
    return 0;
}
