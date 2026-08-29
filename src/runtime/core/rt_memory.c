//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_memory.c
// Purpose: Heap allocation shim for the Zanna runtime C ABI. Validates
//   requested sizes, enforces non-negative limits, and guarantees that callers
//   receive zero-initialised buffers even for zero-byte requests. Mirrors the
//   VM's allocation semantics so that diagnostics and trap conditions remain
//   consistent between interpreted (VM) and native (AOT) execution paths.
//
// Key invariants:
//   - The default rt_alloc path returns a zero-initialised buffer. A test hook
//     may override allocation behavior and is responsible for its own contract.
//   - Requesting a negative or overflow-inducing size fires rt_trap() rather
//     than returning NULL, keeping error handling uniform with other runtime
//     limit violations.
//   - rt_free(ptr) is a thin wrapper around free(). Passing NULL is safe (no-op,
//     matching standard C free() semantics).
//   - Runtime call sites that require deterministic allocation interposition
//     use this shim; specialized subsystems may use their own allocators.
//
// Ownership/Lifetime:
//   - The optional hook is process-global startup state and is unsynchronized.
//   - Callers own returned memory and must free compatible storage via rt_free()
//     or transfer it to the owning higher-level runtime object.
//
// Links: src/runtime/rt_internal.h (internal API),
//        src/runtime/core/rt_trap.h (rt_trap for invalid sizes)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Runtime heap allocation shim shared by BASIC intrinsics.
/// @details Provides @ref rt_alloc, a defensive wrapper around @c calloc that
///          rejects negative or oversized requests and reports errors via
///          @ref rt_trap. The default path returns zeroed storage of at least
///          one byte; an explicitly installed test hook may override that path.

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include <stdlib.h>
#include <string.h>
#if RT_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sched.h>
#endif

//===----------------------------------------------------------------------===//
// Raw-allocation tracking (ZB-28)
//
// The reference IL VM validates every load/store address against memory it
// can prove the program owns (frame stack, IL globals, registered heap
// payloads). Blocks handed out by rt_alloc — class descriptors, module
// variables, misc runtime tables — are plain calloc storage and were invisible
// to that check, so `zanna run --trace/--profile` trapped on the first store
// into one. When tracking is enabled (only by the IL VM runner) every rt_alloc
// block is recorded in an open-addressing table keyed by address and removed
// by rt_free; the VM asks rt_alloc_contains_range. Off by default: zero cost
// for the bytecode VM and native programs.
//===----------------------------------------------------------------------===//

typedef struct {
    void *ptr;   ///< Block start, NULL = empty slot, (void *)1 = tombstone.
    size_t size; ///< Block size in bytes.
} rt_alloc_track_entry_t;

typedef struct {
    rt_alloc_track_entry_t *slots;
    size_t capacity; ///< Power of two, or 0 before the first insert.
    size_t count;    ///< Live entries.
    size_t used;     ///< Live entries + tombstones.
    int enabled;
    int lock;
} rt_alloc_track_t;

static rt_alloc_track_t g_alloc_track_ = {NULL, 0, 0, 0, 0, 0};
#define RT_ALLOC_TRACK_TOMBSTONE ((void *)1)

static void rt_alloc_track_lock_(void) {
    if (__atomic_test_and_set(&g_alloc_track_.lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(&g_alloc_track_.lock, __ATOMIC_ACQUIRE));
    }
}

static void rt_alloc_track_unlock_(void) {
    __atomic_clear(&g_alloc_track_.lock, __ATOMIC_RELEASE);
}

static size_t rt_alloc_track_hash_(const void *ptr) {
    uint64_t h = (uint64_t)(uintptr_t)ptr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h;
}

static int rt_alloc_track_insert_locked_(void *ptr, size_t size);

/// @brief Double the table (or create it) and re-insert every live entry; lock held.
static int rt_alloc_track_grow_locked_(void) {
    size_t new_cap = g_alloc_track_.capacity ? g_alloc_track_.capacity * 2 : 1024;
    rt_alloc_track_entry_t *old = g_alloc_track_.slots;
    size_t old_cap = g_alloc_track_.capacity;
    rt_alloc_track_entry_t *fresh =
        (rt_alloc_track_entry_t *)calloc(new_cap, sizeof(rt_alloc_track_entry_t));
    if (!fresh)
        return 0;
    g_alloc_track_.slots = fresh;
    g_alloc_track_.capacity = new_cap;
    g_alloc_track_.count = 0;
    g_alloc_track_.used = 0;
    for (size_t i = 0; i < old_cap; ++i) {
        if (old[i].ptr && old[i].ptr != RT_ALLOC_TRACK_TOMBSTONE)
            (void)rt_alloc_track_insert_locked_(old[i].ptr, old[i].size);
    }
    free(old);
    return 1;
}

/// @brief Insert or update @p ptr; lock held. Returns 0 only when growth fails.
static int rt_alloc_track_insert_locked_(void *ptr, size_t size) {
    if (g_alloc_track_.capacity == 0 ||
        (g_alloc_track_.used + 1) * 10 > g_alloc_track_.capacity * 7) {
        if (!rt_alloc_track_grow_locked_())
            return 0;
    }
    size_t mask = g_alloc_track_.capacity - 1;
    size_t i = rt_alloc_track_hash_(ptr) & mask;
    for (;;) {
        rt_alloc_track_entry_t *e = &g_alloc_track_.slots[i];
        if (!e->ptr || e->ptr == RT_ALLOC_TRACK_TOMBSTONE) {
            if (!e->ptr)
                g_alloc_track_.used++;
            e->ptr = ptr;
            e->size = size;
            g_alloc_track_.count++;
            return 1;
        }
        if (e->ptr == ptr) {
            e->size = size;
            return 1;
        }
        i = (i + 1) & mask;
    }
}

/// @brief Forget @p ptr (tombstone); lock held.
static void rt_alloc_track_remove_locked_(void *ptr) {
    if (g_alloc_track_.capacity == 0)
        return;
    size_t mask = g_alloc_track_.capacity - 1;
    size_t i = rt_alloc_track_hash_(ptr) & mask;
    for (;;) {
        rt_alloc_track_entry_t *e = &g_alloc_track_.slots[i];
        if (!e->ptr)
            return;
        if (e->ptr == ptr) {
            e->ptr = RT_ALLOC_TRACK_TOMBSTONE;
            e->size = 0;
            g_alloc_track_.count--;
            return;
        }
        i = (i + 1) & mask;
    }
}

void rt_alloc_set_tracking(int enabled) {
    rt_alloc_track_lock_();
    g_alloc_track_.enabled = enabled ? 1 : 0;
    if (!enabled) {
        free(g_alloc_track_.slots);
        g_alloc_track_.slots = NULL;
        g_alloc_track_.capacity = 0;
        g_alloc_track_.count = 0;
        g_alloc_track_.used = 0;
    }
    rt_alloc_track_unlock_();
}

int rt_alloc_tracking_enabled(void) {
    return __atomic_load_n(&g_alloc_track_.enabled, __ATOMIC_ACQUIRE);
}

int8_t rt_alloc_contains_range(const void *ptr, size_t bytes) {
    if (!ptr || bytes == 0)
        return 0;
    if (!rt_alloc_tracking_enabled())
        return 0;
    const uintptr_t address = (uintptr_t)ptr;
    int found = 0;
    rt_alloc_track_lock_();
    for (size_t i = 0; i < g_alloc_track_.capacity; ++i) {
        const rt_alloc_track_entry_t *e = &g_alloc_track_.slots[i];
        if (!e->ptr || e->ptr == RT_ALLOC_TRACK_TOMBSTONE || e->size == 0)
            continue;
        const uintptr_t begin = (uintptr_t)e->ptr;
        if (address < begin)
            continue;
        const uintptr_t offset = address - begin;
        if (offset < e->size && bytes <= e->size - (size_t)offset) {
            found = 1;
            break;
        }
    }
    rt_alloc_track_unlock_();
    return found ? 1 : 0;
}

/// @brief Record a fresh block when tracking is on and hand it back.
static void *rt_alloc_track_note_(void *ptr, int64_t bytes);

/// @brief Record a fresh block when tracking is on.
static void rt_alloc_track_record_(void *ptr, int64_t bytes) {
    if (!ptr || !rt_alloc_tracking_enabled())
        return;
    size_t size = bytes > 0 ? (size_t)bytes : 1;
    rt_alloc_track_lock_();
    if (g_alloc_track_.enabled)
        (void)rt_alloc_track_insert_locked_(ptr, size);
    rt_alloc_track_unlock_();
}

static void *rt_alloc_track_note_(void *ptr, int64_t bytes) {
    rt_alloc_track_record_(ptr, bytes);
    return ptr;
}

/// @brief Forget a block being freed when tracking is on.
static void rt_alloc_track_forget_(void *ptr) {
    if (!ptr || !rt_alloc_tracking_enabled())
        return;
    rt_alloc_track_lock_();
    if (g_alloc_track_.enabled)
        rt_alloc_track_remove_locked_(ptr);
    rt_alloc_track_unlock_();
}

/// @brief Validate a byte count and allocate zero-initialized default storage.
/// @details Zero-byte requests allocate one byte so successful calls always
///   return storage that may be passed back to @ref rt_free.
/// @param bytes Signed byte count to validate.
/// @return Newly allocated zeroed storage, or NULL after raising a trap for a
///   negative/oversized request or allocation failure.
static void *rt_alloc_impl(int64_t bytes) {
    if (bytes < 0)
        return rt_trap("negative allocation"), NULL;
    if ((uint64_t)bytes > SIZE_MAX) {
        rt_trap("allocation too large");
        return NULL;
    }
    size_t request = (size_t)bytes;
    if (request == 0)
        request = 1;
    void *p = calloc(1, request);
    if (!p) {
        rt_trap("out of memory");
        return NULL;
    }
    return p;
}

/// @brief Cookie required before dispatch through the writable hook pointer.
static const uint64_t k_rt_alloc_hook_cookie = 0xA110CA7E5EEDBEEFULL;
/// @brief Armed cookie value, written during single-threaded startup/test setup.
static uint64_t g_rt_alloc_hook_armed = 0;
/// @brief Optional process-global allocation interceptor installed for tests.
static rt_alloc_hook_fn g_rt_alloc_hook = NULL;

/// @brief Install a hook that can override @ref rt_alloc for testing.
/// @details The hook receives the requested byte count along with a pointer to
///          the default implementation.  Passing @c NULL restores the default
///          behaviour.  Intended for unit tests that need to simulate allocator
///          failures without exhausting system memory. Installation is
///          unsynchronized and must occur before concurrent allocation begins.
/// @param hook Replacement function or @c NULL to disable overrides.
/// @warning Any non-NULL storage returned by a hook must be compatible with
///   @ref rt_free; callers cannot enforce zero initialization when the hook
///   chooses not to delegate to the supplied default allocator.
void rt_set_alloc_hook(rt_alloc_hook_fn hook) {
    g_rt_alloc_hook = hook;
    g_rt_alloc_hook_armed = hook ? k_rt_alloc_hook_cookie : 0;
}

/// @brief Allocate zero-initialised storage for runtime subsystems.
/// @details Delegates to the optional test hook when installed, otherwise calls
///          the default implementation described above. The hook receives the
///          unvalidated signed request and decides whether to call the default.
/// @param bytes Number of bytes requested by the caller.
/// @return On the default path, caller-owned zeroed storage; on a hooked path,
///   the hook's result. Default failures trap and return NULL if dispatch returns.
void *rt_alloc(int64_t bytes) {
    // The allocation hook exists for tests only. On native Windows builds we
    // have seen stray writable-data corruption present a non-null function
    // pointer here; require an explicit arm cookie from rt_set_alloc_hook()
    // before dispatching through the hook.
    if (g_rt_alloc_hook_armed == k_rt_alloc_hook_cookie && g_rt_alloc_hook)
        return rt_alloc_track_note_(g_rt_alloc_hook(bytes, rt_alloc_impl), bytes);
    return rt_alloc_track_note_(rt_alloc_impl(bytes), bytes);
}

/// @brief Free storage returned by @ref rt_alloc.
/// @details This is intentionally small today, but keeps allocation call sites
///          paired with the runtime allocator shim for future instrumentation.
///          NULL is accepted with standard `free` semantics.
/// @param ptr Owned allocation returned by the default allocator or a
///   free-compatible test hook.
void rt_free(void *ptr) {
    rt_alloc_track_forget_(ptr);
    free(ptr);
}
