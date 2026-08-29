//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_gc.c
// Purpose: Implements the cycle-detecting garbage collector for the Zanna
//          runtime. Uses a trial-deletion (synchronous mark-sweep) algorithm:
//          trial-decrement child refcounts, identify zero-trial-refcount
//          candidates as potential cycle members, restore reachable objects,
//          then free confirmed cycles.
//
//          The tracked-object set is stored in an open-addressing hash table
//          (power-of-two capacity, linear probing, tombstone deletion) for O(1)
//          lookup during the trial-decrement and restore phases.
//
// Key invariants:
//   - Objects must be registered via rt_gc_track before cycles can be detected;
//     untracked objects rely solely on reference counting for collection.
//   - Trial refcounts are temporary; they are computed per-pass and do not
//     modify the actual reference counts stored in heap headers.
//   - Weak references to collected objects are zeroed after finalizers decline
//     resurrection, preserving weak handles when a finalizer keeps the target alive.
//   - Collection holds the exclusive managed-graph barrier while traversal runs;
//     refcount and container graph mutations hold a nestable shared scope.
//   - The GC table lock (pthread_mutex / CRITICAL_SECTION) protects the tracked-
//     object table and weak reference registry; finalizers run outside the lock.
//   - Pass statistics (total_collected, pass_count) are read and updated while
//     holding the GC table lock.
//
// Ownership/Lifetime:
//   - The entries table and weak registry are allocated lazily and live until
//     rt_gc_shutdown detaches them; synchronization primitives remain reusable
//     for the process lifetime.
//   - Tracked object pointers are borrowed; the GC does not retain a reference
//     — it relies on the object's own refcount to stay alive until collection.
//
// Links: src/runtime/core/rt_gc.h (public API),
//        src/runtime/core/rt_heap.c (heap header layout, refcount fields),
//        src/runtime/core/rt_object.c (object allocation and finalizer registry)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements cycle collection, weak references, and graph quiescence.

#include "rt_gc.h"

#include "rt_atomic_compat.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_threads.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "rt_trap.h"

/// @brief Install a thread-local non-local recovery destination for GC cleanup.
/// @param buf Borrowed jump buffer that remains live until recovery is cleared.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's most recently installed recovery destination.
void rt_trap_clear_recovery(void);
/// @brief Read the current thread's last trap diagnostic.
/// @return Runtime-owned C string valid until the next trap on this thread.
const char *rt_trap_get_error(void);

//=============================================================================
// Internal Data Structures
//=============================================================================

/// Sentinel values for the open-addressing hash table.
/// GC_EMPTY marks a slot that has never been used (terminates probe chains).
/// GC_TOMBSTONE marks a deleted slot (skipped during probing, reusable on insert).
#define GC_EMPTY NULL
#define GC_TOMBSTONE ((void *)(uintptr_t)1)

/// Epoch tagging: objects that survive GC_PROMOTION_THRESHOLD consecutive
/// collection passes are "promoted" and skipped in future trial-deletion
/// phases.  Every GC_FULL_SCAN_INTERVAL passes, ALL objects are scanned
/// regardless of promotion (catches new cycles involving promoted objects).
#define GC_PROMOTION_THRESHOLD 8
#define GC_FULL_SCAN_INTERVAL 16

/// Entry in the tracked-object hash table.
typedef struct gc_entry {
    void *obj; ///< Object pointer (NULL=empty, 1=tombstone, else live).
    rt_gc_traverse_fn traverse;
    int64_t trial_rc;         ///< Temporary refcount for cycle detection.
    int8_t color;             ///< 0=white(unchecked), 1=gray(candidate), 2=black(reachable)
    uint16_t survived;        ///< Number of collection passes this object has survived.
    uint64_t finalizer_epoch; ///< Last shutdown-finalizer sweep that visited this entry.
} gc_entry;

/// Weak reference record.
struct rt_weakref {
    void *target;
    struct rt_weakref *next_for_target; ///< Chain of weak refs to same target.
};

/// Weak ref registry entry (per-target chain).
typedef struct weak_chain {
    void *target;
    rt_weakref *head;
    struct weak_chain *next;
} weak_chain;

/// Global GC state.
static struct {
    gc_entry *entries; ///< Open-addressing hash table (power-of-two capacity).
    int64_t count;     ///< Number of live entries (excludes tombstones).
    int64_t capacity;  ///< Table size (always a power of two, or 0).

    weak_chain *weak_buckets;
    int64_t weak_bucket_count;

    int64_t total_collected;
    int64_t pass_count;
    int collecting;
    uint64_t finalizer_epoch;
} g_gc;

/// GC lock — initialized once and kept alive for the process lifetime.
/// `rt_gc_shutdown()` releases GC-owned tables but intentionally does not
/// destroy this primitive, so embedders and tests can shut down and reuse the
/// GC without racing a late lock user.
#if RT_PLATFORM_WINDOWS
static INIT_ONCE g_gc_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_gc_lock_cs;
#else
static pthread_mutex_t g_gc_lock_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

/// Managed-graph quiescence barrier.
///
/// Ordinary refcount and container graph changes take the shared side. The
/// cycle collector takes the exclusive side before reading reference counts or
/// invoking traversal callbacks. Separate storage from `g_gc_lock_*` is
/// essential: traversal callbacks are allowed to query the GC tracking table.
#if RT_PLATFORM_WINDOWS
static SRWLOCK g_gc_world_lock = SRWLOCK_INIT;
#else
static pthread_rwlock_t g_gc_world_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif

/// Nesting state for the calling thread's shared mutator scope.
static RT_THREAD_LOCAL uint32_t g_gc_mutator_depth = 0;

/// Non-zero while the calling thread owns the exclusive graph barrier.
static RT_THREAD_LOCAL int g_gc_world_exclusive = 0;

/// Set when explicit collection is requested from inside a shared mutator scope.
static RT_THREAD_LOCAL int g_gc_collection_pending = 0;

/// Open-addressed pointer set used while reclaiming one confirmed garbage component.
typedef struct gc_pointer_set {
    void **slots;    ///< NULL-empty table storing managed payload addresses.
    size_t capacity; ///< Power-of-two slot count, or zero when uninitialized.
} gc_pointer_set;

/// Reclaim set visible to nested container finalizers on the collector thread.
static RT_THREAD_LOCAL const gc_pointer_set *g_gc_active_reclaim_set = NULL;

/// Nesting depth while a garbage member's finalizer is releasing owned edges.
static RT_THREAD_LOCAL uint32_t g_gc_suppress_member_release_depth = 0;

/// Auto-trigger allocation debt and threshold.
/// When the threshold is crossed, allocation only publishes a request. A
/// mutator boundary or explicit @ref rt_gc_safepoint claims that request and
/// performs collection outside the allocator call stack.
static int64_t g_gc_threshold = 0;
static int64_t g_gc_alloc_counter = 0;
static int64_t g_gc_collection_requested = 0;

/// @brief Atomic CAS for int64_t (portable across GCC/Clang and MSVC).
/// @param ptr Address of the integer to compare and possibly replace.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Replacement written when the comparison succeeds.
/// @return 1 on exchange success, otherwise 0.
static int gc_atomic_cas_i64(int64_t *ptr, int64_t *expected, int64_t desired) {
#if defined(_MSC_VER) && !defined(__clang__)
    long long old = _InterlockedCompareExchange64(
        (volatile long long *)ptr, (long long)desired, (long long)*expected);
    if (old == (long long)*expected)
        return 1;
    *expected = (int64_t)old;
    return 0;
#else
    return __atomic_compare_exchange_n(
        ptr, expected, desired, /*weak=*/1, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
#endif
}

//=============================================================================
// Lock helpers (CONC-001 fix: race-free initialization)
//=============================================================================

#ifdef _WIN32
/// @brief InitOnce callback that initialises the CRITICAL_SECTION on first use.
/// @param InitOnce Windows one-time initialization record.
/// @param Parameter Unused caller parameter.
/// @param Context Unused context output.
/// @return `TRUE` after initialization, or `FALSE` when native allocation fails.
static BOOL CALLBACK gc_lock_init_callback(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    return InitializeCriticalSectionEx(&g_gc_lock_cs, 0, 0);
}

/// @brief Acquire the GC lock, initialising the CRITICAL_SECTION on first call.
static void gc_lock(void) {
    if (!InitOnceExecuteOnce(&g_gc_lock_once, gc_lock_init_callback, NULL, NULL))
        rt_abort("gc: lock initialization failed");
    EnterCriticalSection(&g_gc_lock_cs);
}

/// @brief Release the GC lock.
static void gc_unlock(void) {
    LeaveCriticalSection(&g_gc_lock_cs);
}
#else
/// @brief Acquire the GC mutex (POSIX static initializer — always safe to call).
static void gc_lock(void) {
    pthread_mutex_lock(&g_gc_lock_mtx);
}

/// @brief Release the GC mutex.
static void gc_unlock(void) {
    pthread_mutex_unlock(&g_gc_lock_mtx);
}
#endif

//=============================================================================
// Managed-Graph Quiescence Barrier
//=============================================================================

/// @brief Enter a nestable shared managed-graph mutator scope.
/// @details The outermost scope takes the shared side of the process-wide graph
///          barrier. Nested retain/release operations only adjust thread-local
///          depth, and collector-owned callbacks bypass the native lock because
///          the same thread already owns its exclusive side.
void rt_gc_mutator_enter(void) {
    if (g_gc_mutator_depth == UINT32_MAX)
        rt_abort("gc: mutator scope nesting overflow");

    if (g_gc_mutator_depth++ != 0 || g_gc_world_exclusive)
        return;

#if RT_PLATFORM_WINDOWS
    AcquireSRWLockShared(&g_gc_world_lock);
#else
    if (pthread_rwlock_rdlock(&g_gc_world_lock) != 0) {
        g_gc_mutator_depth = 0;
        rt_abort("gc: failed to enter mutator scope");
    }
#endif
}

/// @brief Leave one shared managed-graph mutator scope.
/// @details Releases the native shared lock when the outermost scope ends. If
///          automatic or explicit collection was requested while an update was
///          in progress, the deferred pass starts only after releasing the
///          shared lock so no unsupported lock upgrade is attempted.
void rt_gc_mutator_exit(void) {
    if (g_gc_mutator_depth == 0)
        return;
    if (--g_gc_mutator_depth != 0 || g_gc_world_exclusive)
        return;

#if RT_PLATFORM_WINDOWS
    ReleaseSRWLockShared(&g_gc_world_lock);
#else
    if (pthread_rwlock_unlock(&g_gc_world_lock) != 0)
        rt_abort("gc: failed to leave mutator scope");
#endif

    if (g_gc_collection_pending) {
        g_gc_collection_pending = 0;
        (void)rt_gc_collect();
    }
}

/// @brief Abandon the calling thread's shared mutator scope before trap transfer.
/// @details Recoverable traps use `longjmp` and therefore skip normal lexical
///          cleanup. The trap dispatcher calls this hook before transferring
///          control. Collector-owned nested scopes are cleared without dropping
///          the exclusive barrier; the collector recovery path owns that lock.
void rt_gc_mutator_abort_for_trap(void) {
    if (g_gc_mutator_depth == 0) {
        g_gc_collection_pending = 0;
        return;
    }

    g_gc_mutator_depth = 0;
    g_gc_collection_pending = 0;
    if (g_gc_world_exclusive)
        return;

#if RT_PLATFORM_WINDOWS
    ReleaseSRWLockShared(&g_gc_world_lock);
#else
    if (pthread_rwlock_unlock(&g_gc_world_lock) != 0)
        rt_abort("gc: failed to abandon mutator scope");
#endif
}

/// @brief Acquire exclusive access to the managed object graph.
/// @details The caller must not already own the exclusive side. When
///          @p defer_collection is non-zero, a request made inside a mutator
///          scope records a pending collection; maintenance operations instead
///          fail without scheduling unrelated work.
/// @param defer_collection Whether a shared-scope collision should request a
///        collection after the outermost mutator exits.
/// @return 1 when the exclusive barrier was acquired; otherwise 0.
static int gc_world_begin_exclusive(int defer_collection) {
    if (g_gc_world_exclusive)
        return 0;
    if (g_gc_mutator_depth != 0) {
        if (defer_collection)
            g_gc_collection_pending = 1;
        return 0;
    }

#if RT_PLATFORM_WINDOWS
    AcquireSRWLockExclusive(&g_gc_world_lock);
#else
    if (pthread_rwlock_wrlock(&g_gc_world_lock) != 0)
        rt_abort("gc: failed to stop managed-graph mutators");
#endif
    g_gc_world_exclusive = 1;
    return 1;
}

/// @brief Acquire exclusive graph access for a cycle-collection pass.
/// @details A request made inside a mutator scope is deferred until the
///          outermost scope exits, avoiding an unsupported read-to-write lock
///          upgrade. Reentrant collector-thread requests are ignored.
/// @return 1 when the exclusive barrier was acquired; otherwise 0.
static int gc_world_begin_collection(void) {
    return gc_world_begin_exclusive(1);
}

/// @brief Release exclusive managed-graph access after collection or recovery.
/// @details Clears the collector thread's nesting state before releasing the
///   platform write lock. Calling without exclusive ownership is a no-op.
static void gc_world_end_collection(void) {
    if (!g_gc_world_exclusive)
        return;
    g_gc_mutator_depth = 0;
    g_gc_world_exclusive = 0;
#if RT_PLATFORM_WINDOWS
    ReleaseSRWLockExclusive(&g_gc_world_lock);
#else
    if (pthread_rwlock_unlock(&g_gc_world_lock) != 0)
        rt_abort("gc: failed to resume managed-graph mutators");
#endif
}

//=============================================================================
// Hash Utility
//=============================================================================

/// @brief Splitmix64-style pointer hash for hash table slot computation.
/// @param p Pointer value to mix; the pointer is not dereferenced.
/// @return Deterministically mixed integer derived from the address bits.
static uint64_t ptr_hash(void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

/// @brief Allocate an empty pointer set sized for @p item_count live entries.
/// @details Uses at most a 1/2 load factor so linear probes remain short during
///          cycle teardown. Capacity is rounded to a power of two and checked
///          against `SIZE_MAX` before allocation.
/// @param set Destination set, which must not currently own storage.
/// @param item_count Number of distinct non-null pointers that will be inserted.
/// @return 1 on success; 0 on overflow or allocation failure.
static int gc_pointer_set_init(gc_pointer_set *set, int64_t item_count) {
    if (!set || item_count < 0)
        return 0;
    set->slots = NULL;
    set->capacity = 0;
    if (item_count == 0)
        return 1;

    if ((uint64_t)item_count > (uint64_t)SIZE_MAX / 2)
        return 0;
    size_t needed = (size_t)item_count * 2;
    size_t capacity = 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return 0;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(void *))
        return 0;
    set->slots = (void **)calloc(capacity, sizeof(void *));
    if (!set->slots)
        return 0;
    set->capacity = capacity;
    return 1;
}

/// @brief Insert one non-null payload address into a pre-sized pointer set.
/// @param set Initialized destination set.
/// @param payload Managed payload address to insert.
static void gc_pointer_set_insert(gc_pointer_set *set, void *payload) {
    if (!set || !set->slots || set->capacity == 0 || !payload)
        return;
    size_t mask = set->capacity - 1;
    size_t slot = (size_t)ptr_hash(payload) & mask;
    while (set->slots[slot] && set->slots[slot] != payload)
        slot = (slot + 1) & mask;
    set->slots[slot] = payload;
}

/// @brief Test whether @p payload belongs to a pointer set.
/// @param set Initialized set, or NULL/empty for an always-false lookup.
/// @param payload Candidate managed payload address.
/// @return 1 when present; otherwise 0.
static int gc_pointer_set_contains(const gc_pointer_set *set, void *payload) {
    if (!set || !set->slots || set->capacity == 0 || !payload)
        return 0;
    size_t mask = set->capacity - 1;
    size_t slot = (size_t)ptr_hash(payload) & mask;
    for (size_t probe = 0; probe < set->capacity; ++probe) {
        void *candidate = set->slots[slot];
        if (!candidate)
            return 0;
        if (candidate == payload)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Release storage owned by a temporary pointer set.
/// @param set Set to clear; NULL is a no-op.
static void gc_pointer_set_destroy(gc_pointer_set *set) {
    if (!set)
        return;
    free(set->slots);
    set->slots = NULL;
    set->capacity = 0;
}

/// @brief Tell the heap whether a finalizer release targets another member of the active cycle.
/// @details During cycle teardown, container finalizers must clear their native buffers but must
///          not decrement a member whose reference count is being normalized by the collector.
///          Releases to objects outside the reclaim set remain ordinary ownership operations.
/// @param payload Candidate release target.
/// @return 1 only on the collector thread, inside a member finalizer, for an active garbage member.
int8_t rt_gc_should_suppress_cycle_release(void *payload) {
    return g_gc_suppress_member_release_depth > 0 &&
                   gc_pointer_set_contains(g_gc_active_reclaim_set, payload)
               ? 1
               : 0;
}

/// @brief Check if a hash table slot contains a live (tracked) entry.
/// @param e Non-NULL table entry to inspect.
/// @return 1 unless the slot contains the empty or tombstone sentinel.
static int gc_slot_is_live(const gc_entry *e) {
    return e->obj != GC_EMPTY && e->obj != GC_TOMBSTONE;
}

/// @brief Clear the in-progress-collection sentinel under the GC lock.
/// @details Called from the reclaim-phase trap-recovery branch in `rt_gc_collect` so a
///          finalizer that traps doesn't leave the active-collection flag stuck on,
///          which would cause every later `rt_gc_collect` call to short-circuit and
///          return zero forever.
static void gc_clear_collecting_flag(void) {
    gc_lock();
    g_gc.collecting = 0;
    gc_unlock();
}

//=============================================================================
// Tracked Objects Hash Table
//=============================================================================

/// @brief Find the slot index for @p obj in the hash table.
/// @details Uses linear probing. Tombstones are skipped (do not terminate
///          the probe chain); empty slots terminate it.
/// @param obj Exact tracked payload address to locate.
/// @return Slot index if found, -1 otherwise. Caller must hold gc_lock.
static int64_t find_entry(void *obj) {
    if (!g_gc.entries || g_gc.capacity == 0)
        return -1;
    uint64_t mask = (uint64_t)(g_gc.capacity - 1);
    uint64_t idx = ptr_hash(obj) & mask;
    for (int64_t probe = 0; probe < g_gc.capacity; probe++) {
        gc_entry *e = &g_gc.entries[idx];
        if (e->obj == obj)
            return (int64_t)idx;
        if (e->obj == GC_EMPTY)
            return -1;
        /* Tombstone — keep probing. */
        idx = (idx + 1) & mask;
    }
    return -1;
}

/// @brief Rehash all live entries into a new table of @p new_cap slots.
/// @details Allocates a fresh zero-initialised table, re-inserts every live
///          entry, and frees the old table. Tombstones are discarded. The
///          caller must hold gc_lock.
/// @param new_cap New table capacity (must be a power of two).
/// @return 1 on success, or 0 for invalid sizing or allocation failure; the
///   existing table remains owned by the GC on failure.
static int gc_rehash(int64_t new_cap) {
    if (new_cap <= 0 || (uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(gc_entry))
        return 0;
    gc_entry *old = g_gc.entries;
    int64_t old_cap = g_gc.capacity;

    gc_entry *new_entries = (gc_entry *)calloc((size_t)new_cap, sizeof(gc_entry));
    if (!new_entries)
        return 0;

    uint64_t mask = (uint64_t)(new_cap - 1);
    int64_t live = 0;

    for (int64_t i = 0; i < old_cap; i++) {
        if (!gc_slot_is_live(&old[i]))
            continue;
        uint64_t slot = ptr_hash(old[i].obj) & mask;
        while (new_entries[slot].obj != GC_EMPTY)
            slot = (slot + 1) & mask;
        new_entries[slot] = old[i];
        live++;
    }

    g_gc.entries = new_entries;
    g_gc.capacity = new_cap;
    g_gc.count = live;
    free(old);
    return 1;
}

/// @brief Internal outcomes from one tracking-table insertion attempt.
typedef enum gc_track_result {
    GC_TRACK_OK = 1,
    GC_TRACK_INVALID_PAYLOAD = -1,
    GC_TRACK_UNSUPPORTED_KIND = -2,
    GC_TRACK_CAPACITY_OVERFLOW = -3,
    GC_TRACK_OUT_OF_MEMORY = -4
} gc_track_result;

/// @brief Insert or update one cycle-collector entry without dispatching traps.
/// @details Performs live-header validation and all lock/barrier balancing, then
///          returns a precise status to its caller. Keeping this core non-trapping
///          lets `rt_heap_alloc` make automatic reference-array registration
///          transactional: a failed table growth can unregister and free the new
///          payload before returning NULL. The public @ref rt_gc_track wrapper
///          translates failures into the established diagnostics.
/// @param obj Candidate managed object or reference-bearing array.
/// @param traverse Non-null strong-edge enumeration callback.
/// @return A @ref gc_track_result value.
static gc_track_result gc_track_impl(void *obj, rt_gc_traverse_fn traverse) {
    if (!obj || !traverse)
        return GC_TRACK_OK;
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(obj, &hdr) || !hdr) {
        rt_gc_mutator_exit();
        return GC_TRACK_INVALID_PAYLOAD;
    }
    rt_heap_kind_t kind = (rt_heap_kind_t)hdr->kind;
    rt_elem_kind_t elem_kind = (rt_elem_kind_t)hdr->elem_kind;
    if (kind != RT_HEAP_OBJECT &&
        (kind != RT_HEAP_ARRAY || (elem_kind != RT_ELEM_OBJ && elem_kind != RT_ELEM_BOX))) {
        rt_gc_mutator_exit();
        return GC_TRACK_UNSUPPORTED_KIND;
    }

    gc_lock();

    /* Already tracked? Update traverse function. */
    int64_t idx = find_entry(obj);
    if (idx >= 0) {
        g_gc.entries[idx].traverse = traverse;
        gc_unlock();
        rt_gc_mutator_exit();
        return GC_TRACK_OK;
    }

    /* Grow if needed: maintain < 5/8 load factor. */
    if (g_gc.capacity == 0 || g_gc.count >= (g_gc.capacity / 8) * 5) {
        if (g_gc.capacity > 0 && g_gc.capacity > INT64_MAX / 2) {
            gc_unlock();
            rt_gc_mutator_exit();
            return GC_TRACK_CAPACITY_OVERFLOW;
        }
        int64_t new_cap = g_gc.capacity == 0 ? 64 : g_gc.capacity * 2;
        if (!gc_rehash(new_cap)) {
            gc_unlock();
            rt_gc_mutator_exit();
            return GC_TRACK_OUT_OF_MEMORY;
        }
    }

    /* Insert at first empty or tombstone slot. */
    uint64_t mask = (uint64_t)(g_gc.capacity - 1);
    uint64_t slot = ptr_hash(obj) & mask;
    while (gc_slot_is_live(&g_gc.entries[slot]))
        slot = (slot + 1) & mask;

    gc_entry *e = &g_gc.entries[slot];
    e->obj = obj;
    e->traverse = traverse;
    e->trial_rc = 0;
    e->color = 0;
    e->survived = 0;
    e->finalizer_epoch = 0;
    g_gc.count++;

    gc_unlock();
    rt_gc_mutator_exit();
    return GC_TRACK_OK;
}

/// @brief Enumerate pointer slots owned by an object- or box-reference array.
/// @details Automatically registered arrays are zero-initialized before they
///          enter the tracking table. During collection the exclusive graph
///          barrier prevents resize or slot mutation, so the header length and
///          payload address remain stable for the complete traversal.
/// @param obj Exact managed array payload.
/// @param visitor Collector visitor invoked for each non-null strong slot.
/// @param ctx Opaque visitor context.
static void gc_reference_array_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(obj, &hdr) || !hdr || (rt_heap_kind_t)hdr->kind != RT_HEAP_ARRAY)
        return;
    rt_elem_kind_t elem_kind = (rt_elem_kind_t)hdr->elem_kind;
    if (elem_kind != RT_ELEM_OBJ && elem_kind != RT_ELEM_BOX)
        return;
    void **items = (void **)obj;
    for (size_t i = 0; i < hdr->len; ++i) {
        if (items[i])
            visitor(items[i], ctx);
    }
}

/// @brief Register an object for cycle detection.
/// @details Inserts @p obj into the GC hash table. If already tracked, updates
///          the traverse function. The table grows when load exceeds 5/8.
///          NULL objects or callbacks are accepted as no-ops. Invalid live
///          payloads, unsupported heap kinds, capacity overflow, and allocation
///          failure raise a runtime trap.
/// @param obj Borrowed heap object or reference-array payload to register.
/// @param traverse Strong-edge callback retained in collector bookkeeping.
void rt_gc_track(void *obj, rt_gc_traverse_fn traverse) {
    gc_track_result result = gc_track_impl(obj, traverse);
    switch (result) {
        case GC_TRACK_OK:
            return;
        case GC_TRACK_INVALID_PAYLOAD:
            rt_trap("rt_gc_track: object is not a live heap payload");
            return;
        case GC_TRACK_UNSUPPORTED_KIND:
            rt_trap("rt_gc_track: payload is not a heap object or reference array");
            return;
        case GC_TRACK_CAPACITY_OVERFLOW:
            rt_trap("rt_gc: hash table capacity overflow");
            return;
        case GC_TRACK_OUT_OF_MEMORY:
            rt_trap("rt_gc: failed to grow hash table (out of memory)");
            return;
    }
}

/// @brief Transactionally register a newly allocated reference-bearing array.
/// @details This non-trapping heap/collector handshake uses the collector's
///          generic pointer-slot traversal for `RT_ELEM_OBJ` and `RT_ELEM_BOX`
///          arrays. It is called before the new payload escapes to user code, so
///          a zero result allows the heap to roll back allocation completely.
/// @param array Exact live array payload with a supported reference element kind.
/// @return 1 when tracked, otherwise 0.
int8_t rt_gc_track_reference_array(void *array) {
    return gc_track_impl(array, gc_reference_array_traverse) == GC_TRACK_OK ? 1 : 0;
}

/// @brief Remove an object from cycle tracking.
/// @details Tombstones the hash table slot so probe chains remain intact. NULL
///   and currently untracked payloads are no-ops; no object reference is released.
/// @param obj Exact borrowed payload address to remove.
void rt_gc_untrack(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    gc_lock();

    int64_t idx = find_entry(obj);
    if (idx >= 0) {
        /* Mark slot as tombstone so probe chains are preserved. */
        g_gc.entries[idx].obj = GC_TOMBSTONE;
        g_gc.entries[idx].traverse = NULL;
        g_gc.count--;
    }

    gc_unlock();
    rt_gc_mutator_exit();
}

/// @brief Move GC and weak-reference bookkeeping after a managed payload changes address.
/// @details Heap resize allocates a replacement block and retires the old address. While the
///          caller holds a mutator scope, this helper rehashes any tracked-object entry and
///          moves the old target's weak-reference chain to the new bucket. Weak handles are
///          rewritten to the replacement address before mutators can observe the resize.
/// @param old_payload Previous payload address, which may already be absent from the heap registry.
/// @param new_payload Replacement live payload address.
void rt_gc_relocate_payload(void *old_payload, void *new_payload) {
    if (!old_payload || !new_payload || old_payload == new_payload)
        return;

    rt_gc_mutator_enter();
    gc_lock();

    int64_t tracked_idx = find_entry(old_payload);
    if (tracked_idx >= 0) {
        gc_entry moved = g_gc.entries[tracked_idx];
        g_gc.entries[tracked_idx].obj = GC_TOMBSTONE;
        g_gc.entries[tracked_idx].traverse = NULL;
        moved.obj = new_payload;

        uint64_t mask = (uint64_t)(g_gc.capacity - 1);
        uint64_t slot = ptr_hash(new_payload) & mask;
        while (gc_slot_is_live(&g_gc.entries[slot]))
            slot = (slot + 1) & mask;
        g_gc.entries[slot] = moved;
    }

    if (g_gc.weak_buckets && g_gc.weak_bucket_count > 0) {
        uint64_t old_bucket = ptr_hash(old_payload) % (uint64_t)g_gc.weak_bucket_count;
        weak_chain **link = &g_gc.weak_buckets[old_bucket].next;
        while (*link) {
            weak_chain *chain = *link;
            if (chain->target == old_payload) {
                *link = chain->next;
                chain->target = new_payload;
                for (rt_weakref *ref = chain->head; ref; ref = ref->next_for_target)
                    ref->target = new_payload;
                uint64_t new_bucket = ptr_hash(new_payload) % (uint64_t)g_gc.weak_bucket_count;
                chain->next = g_gc.weak_buckets[new_bucket].next;
                g_gc.weak_buckets[new_bucket].next = chain;
                break;
            }
            link = &chain->next;
        }
    }

    gc_unlock();
    rt_gc_mutator_exit();
}

/// @brief Check if an object is in the tracking table.
/// @param obj Exact borrowed payload address to query; NULL is never tracked.
/// @return 1 when the address has a live table entry, otherwise 0.
int8_t rt_gc_is_tracked(void *obj) {
    if (!obj)
        return 0;

    gc_lock();
    int8_t found = find_entry(obj) >= 0 ? 1 : 0;
    gc_unlock();
    return found;
}

/// @brief Return the number of objects currently in the tracking table.
/// @return Locked snapshot of live entries, excluding empty and tombstone slots.
int64_t rt_gc_tracked_count(void) {
    gc_lock();
    int64_t n = g_gc.count;
    gc_unlock();
    return n;
}

//=============================================================================
// Weak Reference Registry
//=============================================================================

/// @brief Fixed number of buckets in the lazily allocated weak-target registry.
#define WEAK_BUCKET_COUNT 64

/// @brief Lazily allocate the weak-reference bucket table on first use.
/// @return 1 on success, 0 on allocation failure. Callers trap after dropping the GC lock.
static int ensure_weak_buckets(void) {
    if (g_gc.weak_buckets)
        return 1;
    g_gc.weak_bucket_count = WEAK_BUCKET_COUNT;
    g_gc.weak_buckets = (weak_chain *)calloc((size_t)g_gc.weak_bucket_count, sizeof(weak_chain));
    if (!g_gc.weak_buckets)
        return 0;
    return 1;
}

/// @brief Find or allocate the weak-reference chain for one target.
/// @details Lazily creates both the bucket table and the per-target chain. The
///   caller must hold the GC lock.
/// @param target Non-NULL managed target used as the exact chain key.
/// @return Existing or newly allocated chain, or NULL on allocation failure.
static weak_chain *ensure_weak_chain(void *target) {
    if (!ensure_weak_buckets())
        return NULL;
    uint64_t bucket = ptr_hash(target) % (uint64_t)g_gc.weak_bucket_count;

    weak_chain *wc = g_gc.weak_buckets[bucket].next;
    while (wc) {
        if (wc->target == target)
            return wc;
        wc = wc->next;
    }

    weak_chain *new_wc = (weak_chain *)malloc(sizeof(weak_chain));
    if (!new_wc)
        return NULL;
    new_wc->target = target;
    new_wc->head = NULL;
    new_wc->next = g_gc.weak_buckets[bucket].next;
    g_gc.weak_buckets[bucket].next = new_wc;
    return new_wc;
}

/// @brief Add @p ref to the per-target weak-reference chain for @p target.
/// @param target Non-NULL managed target whose chain receives the handle.
/// @param ref Non-NULL weak handle to link at the head of the chain.
/// @return 1 on success, 0 on allocation failure.
static int register_weak_ref(void *target, rt_weakref *ref) {
    weak_chain *wc = ensure_weak_chain(target);
    if (!wc)
        return 0;
    ref->next_for_target = wc->head;
    wc->head = ref;
    return 1;
}

/// @brief Remove @p ref from the per-target weak-reference chain for @p target.
/// @details Frees the chain node when the removed handle was its final member.
///   The caller must hold the GC lock. Missing targets and handles are no-ops.
/// @param target Exact target key used when the handle was registered.
/// @param ref Weak handle to unlink.
static void unregister_weak_ref(void *target, rt_weakref *ref) {
    if (!g_gc.weak_buckets || g_gc.weak_bucket_count <= 0)
        return;
    uint64_t bucket = ptr_hash(target) % (uint64_t)g_gc.weak_bucket_count;
    weak_chain **wc_pp = &g_gc.weak_buckets[bucket].next;

    while (*wc_pp) {
        weak_chain *wc = *wc_pp;
        if (wc->target == target) {
            rt_weakref **pp = &wc->head;
            while (*pp) {
                if (*pp == ref) {
                    *pp = ref->next_for_target;
                    ref->next_for_target = NULL;
                    if (!wc->head) {
                        *wc_pp = wc->next;
                        free(wc);
                    }
                    return;
                }
                pp = &(*pp)->next_for_target;
            }
            return;
        }
        wc_pp = &(*wc_pp)->next;
    }
}

/// @brief Return 1 if @p candidate is a live weak-reference heap object (lock must be held).
/// @param candidate Opaque pointer to validate against heap metadata and class ID.
/// @return 1 for a sufficiently sized live `RT_WEAKREF_CLASS_ID` object, otherwise 0.
static int gc_is_weakref_handle_unlocked(void *candidate) {
    rt_heap_hdr_t *hdr = NULL;
    if (!candidate || !rt_heap_try_get_header(candidate, &hdr) || !hdr)
        return 0;
    return (rt_heap_kind_t)hdr->kind == RT_HEAP_OBJECT && hdr->class_id == RT_WEAKREF_CLASS_ID &&
                   hdr->cap >= sizeof(rt_weakref)
               ? 1
               : 0;
}

/// @brief Return 1 if @p target can be registered as a zeroing weak target.
/// @param target Candidate runtime string or non-string managed heap payload;
///   NULL represents a cleared weak reference.
/// @return 1 for NULL or a live supported runtime handle, otherwise 0.
static int gc_weak_target_is_valid(void *target) {
    if (!target)
        return 1;
    if (rt_string_is_handle(target)) {
        rt_string s = (rt_string)target;
        rt_heap_hdr_t *string_hdr = s->heap && s->heap != RT_SSO_SENTINEL ? s->heap : NULL;
        size_t refs =
            __atomic_load_n(string_hdr ? &string_hdr->refcnt : &s->literal_refs, __ATOMIC_ACQUIRE);
        return refs != 0 ? 1 : 0;
    }
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(target, &hdr) || !hdr)
        return 0;
    rt_heap_kind_t kind = (rt_heap_kind_t)hdr->kind;
    if (kind != RT_HEAP_ARRAY && kind != RT_HEAP_OBJECT)
        return 0;
    return __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE) != 0 ? 1 : 0;
}

/// @brief Detach a weak-reference object from the registry during generic object release.
/// @param obj Weak-reference payload being finalized; NULL is a no-op.
static void weakref_finalizer(void *obj) {
    rt_weakref *ref = (rt_weakref *)obj;
    if (!ref)
        return;
    gc_lock();
    if (ref->target)
        unregister_weak_ref(ref->target, ref);
    ref->target = NULL;
    ref->next_for_target = NULL;
    gc_unlock();
}

//=============================================================================
// Zeroing Weak References (Public API)
//=============================================================================

/// @brief Create a zeroing weak reference. The target's refcount is NOT bumped.
/// @details When the target is freed, the reference automatically becomes
///   NULL. The target must be NULL, a live runtime string, or a live non-string
///   heap payload. Invalid targets and allocation failures trap.
/// @param target Borrowed managed target, or NULL for an initially empty handle.
/// @return Caller-owned weak-reference object, or NULL if a returning trap hook
///   permits an error path to continue.
rt_weakref *rt_weakref_new(void *target) {
    rt_gc_mutator_enter();
    if (!gc_weak_target_is_valid(target)) {
        rt_gc_mutator_exit();
        rt_trap("gc: weak reference target is not a live runtime handle");
        return NULL;
    }
    rt_weakref *ref =
        (rt_weakref *)rt_obj_new_i64(RT_WEAKREF_CLASS_ID, (int64_t)sizeof(rt_weakref));
    if (!ref) {
        rt_gc_mutator_exit();
        rt_trap("gc: weak reference allocation failed");
        return NULL;
    }
    memset(ref, 0, sizeof(rt_weakref));
    rt_obj_set_finalizer(ref, weakref_finalizer);

    gc_lock();
    ref->target = target;
    ref->next_for_target = NULL;
    if (target && !register_weak_ref(target, ref)) {
        ref->target = NULL;
        gc_unlock();
        if (rt_obj_release_check0(ref))
            rt_obj_free(ref);
        rt_gc_mutator_exit();
        rt_trap("gc: weak reference allocation failed");
        return NULL;
    }
    gc_unlock();

    rt_gc_mutator_exit();
    return ref;
}

/// @brief Dereference a weak ref, retaining and returning the target or NULL if freed.
/// @details Promotion and zeroing are serialized with target destruction. A
///   mortal target is atomically retained before return; immortal targets need
///   no increment. Invalid handles and mortal refcount overflow trap.
/// @param ref Borrowed weak-reference handle; NULL behaves as an empty handle.
/// @return Caller-owned retained target, or NULL when no live target remains or
///   an error trap returns locally.
void *rt_weakref_get(rt_weakref *ref) {
    if (!ref)
        return NULL;

    rt_gc_mutator_enter();
    gc_lock();
    if (!gc_is_weakref_handle_unlocked(ref)) {
        gc_unlock();
        rt_gc_mutator_exit();
        rt_trap("gc: invalid or freed weak reference");
        return NULL;
    }
    void *t = ref->target;
    if (t) {
        if (rt_string_is_handle(t)) {
            rt_string s = (rt_string)t;
            rt_heap_hdr_t *hdr = s->heap && s->heap != RT_SSO_SENTINEL ? s->heap : NULL;
            size_t *refs = hdr ? &hdr->refcnt : &s->literal_refs;
            size_t old = __atomic_load_n(refs, __ATOMIC_RELAXED);
            for (;;) {
                if (old == 0) {
                    t = NULL;
                    break;
                }
                if (old >= RT_HEAP_IMMORTAL_REFCNT)
                    break;
                if (old >= RT_HEAP_MAX_MORTAL_REFCNT) {
                    gc_unlock();
                    rt_gc_mutator_exit();
                    rt_trap("gc: weak reference promotion refcount overflow");
                    return NULL;
                }
                size_t next = old + 1;
                if (__atomic_compare_exchange_n(refs,
                                                &old,
                                                next,
                                                /*weak=*/0,
                                                __ATOMIC_RELAXED,
                                                __ATOMIC_RELAXED)) {
                    break;
                }
            }
        } else {
            rt_heap_hdr_t *hdr = NULL;
            if (!rt_heap_try_get_header(t, &hdr) || !hdr) {
                t = NULL;
            } else {
                size_t old = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
                for (;;) {
                    if (old == 0) {
                        t = NULL;
                        break;
                    }
                    if (old >= RT_HEAP_IMMORTAL_REFCNT)
                        break;
                    if (old >= RT_HEAP_MAX_MORTAL_REFCNT) {
                        gc_unlock();
                        rt_gc_mutator_exit();
                        rt_trap("gc: weak reference promotion refcount overflow");
                        return NULL;
                    }
                    size_t next = old + 1;
                    if (__atomic_compare_exchange_n(&hdr->refcnt,
                                                    &old,
                                                    next,
                                                    /*weak=*/0,
                                                    __ATOMIC_RELAXED,
                                                    __ATOMIC_RELAXED)) {
                        break;
                    }
                }
            }
        }
    }
    gc_unlock();
    rt_gc_mutator_exit();
    return t;
}

/// @brief Check if a weak reference's target is still alive.
/// @details Validates the handle and reads the target's current string or heap
///   reference count without retaining it.
/// @param ref Borrowed weak-reference handle; NULL is not alive.
/// @return 1 when a nonzero-refcount target is registered, otherwise 0.
/// @warning An invalid non-NULL handle raises a runtime trap.
int8_t rt_weakref_alive(rt_weakref *ref) {
    if (!ref)
        return 0;

    rt_gc_mutator_enter();
    gc_lock();
    if (!gc_is_weakref_handle_unlocked(ref)) {
        gc_unlock();
        rt_gc_mutator_exit();
        rt_trap("gc: invalid or freed weak reference");
        return 0;
    }
    int8_t alive = 0;
    void *target = ref->target;
    if (target) {
        if (rt_string_is_handle(target)) {
            rt_string s = (rt_string)target;
            rt_heap_hdr_t *hdr = s->heap && s->heap != RT_SSO_SENTINEL ? s->heap : NULL;
            size_t refs = __atomic_load_n(hdr ? &hdr->refcnt : &s->literal_refs, __ATOMIC_RELAXED);
            alive = refs > 0 ? 1 : 0;
        } else {
            rt_heap_hdr_t *hdr = NULL;
            if (rt_heap_try_get_header(target, &hdr) && hdr) {
                size_t refs = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
                alive = refs > 0 ? 1 : 0;
            }
        }
    }
    gc_unlock();
    rt_gc_mutator_exit();
    return alive;
}

/// @brief Destroy a weak reference handle. Does NOT affect the target.
/// @details Detaches the handle from its target chain, clears it, and releases
///   the caller's object reference. NULL is a no-op; invalid handles trap.
/// @param ref Weak-reference handle whose caller-owned reference is released.
void rt_weakref_free(rt_weakref *ref) {
    if (!ref)
        return;

    rt_gc_mutator_enter();
    gc_lock();
    if (!gc_is_weakref_handle_unlocked(ref)) {
        gc_unlock();
        rt_gc_mutator_exit();
        rt_trap("gc: invalid or freed weak reference");
        return;
    }
    if (ref->target)
        unregister_weak_ref(ref->target, ref);
    ref->target = NULL;
    ref->next_for_target = NULL;
    gc_unlock();

    if (rt_obj_release_check0(ref))
        rt_obj_free(ref);
    rt_gc_mutator_exit();
}

/// @brief Returns 1 if `candidate` is a registered weak-reference handle. Used by polymorphic
/// dispatch sites to distinguish weak-ref objects from regular GC objects.
/// @param candidate Opaque pointer to validate; NULL and stale pointers return zero.
/// @return 1 for a live weak-reference object, otherwise 0.
int8_t rt_weakref_is_handle(void *candidate) {
    int8_t is_handle = 0;
    gc_lock();
    is_handle = (int8_t)gc_is_weakref_handle_unlocked(candidate);
    gc_unlock();
    return is_handle;
}

/// @brief Re-point a weak reference at a different target (or to NULL to clear). Unregisters
/// from the old target's weak-list and registers with the new one.
/// @details The new chain is allocated before the old registration is removed,
///   so allocation failure leaves the handle unchanged. Reusing the existing
///   target is a no-op. Invalid handles or targets and allocation failure trap.
/// @param ref Borrowed weak-reference handle; NULL is a no-op.
/// @param target Borrowed replacement managed target, or NULL to clear.
void rt_weakref_reset(rt_weakref *ref, void *target) {
    if (!ref)
        return;
    rt_gc_mutator_enter();
    if (!gc_weak_target_is_valid(target)) {
        rt_gc_mutator_exit();
        rt_trap("gc: weak reference target is not a live runtime handle");
        return;
    }

    gc_lock();
    if (!gc_is_weakref_handle_unlocked(ref)) {
        gc_unlock();
        rt_gc_mutator_exit();
        rt_trap("gc: invalid or freed weak reference");
        return;
    }
    if (ref->target == target) {
        gc_unlock();
        rt_gc_mutator_exit();
        return;
    }
    weak_chain *new_chain = NULL;
    if (target) {
        new_chain = ensure_weak_chain(target);
        if (!new_chain) {
            gc_unlock();
            rt_gc_mutator_exit();
            rt_trap("gc: weak reference allocation failed");
            return;
        }
    }
    if (ref->target)
        unregister_weak_ref(ref->target, ref);
    ref->target = target;
    ref->next_for_target = NULL;
    if (new_chain) {
        ref->next_for_target = new_chain->head;
        new_chain->head = ref;
    }
    gc_unlock();
    rt_gc_mutator_exit();
}

/// @brief Zero all weak references pointing to a target being freed.
/// @details Called internally by rt_obj_free before deallocating. Walks the
///          per-target chain in the weak bucket, clears every target and chain
///          link, removes the registry node, and frees that node. NULL and
///          unregistered targets are no-ops.
/// @param target Exact borrowed address whose observer chain is cleared.
void rt_gc_clear_weak_refs(void *target) {
    if (!target)
        return;

    rt_gc_mutator_enter();
    gc_lock();
    if (!g_gc.weak_buckets || g_gc.weak_bucket_count <= 0) {
        gc_unlock();
        rt_gc_mutator_exit();
        return;
    }
    uint64_t bucket = ptr_hash(target) % (uint64_t)g_gc.weak_bucket_count;

    weak_chain **wc_pp = &g_gc.weak_buckets[bucket].next;
    while (*wc_pp) {
        weak_chain *wc = *wc_pp;
        if (wc->target == target) {
            /* Clear all weak refs in this chain. */
            rt_weakref *r = wc->head;
            while (r) {
                rt_weakref *next = r->next_for_target;
                r->target = NULL;
                r->next_for_target = NULL;
                r = next;
            }

            /* Remove chain from bucket. */
            *wc_pp = wc->next;
            free(wc);
            gc_unlock();
            rt_gc_mutator_exit();
            return;
        }
        wc_pp = &(*wc_pp)->next;
    }

    gc_unlock();
    rt_gc_mutator_exit();
}

//=============================================================================
// Cycle Detection — Trial Deletion Algorithm
//=============================================================================

/// @brief Visitor that trial-decrements child refcounts (called under gc_lock).
/// @param child Borrowed child payload reported by a traversal callback.
/// @param ctx Unused visitor context.
static void trial_decrement(void *child, void *ctx) {
    (void)ctx;
    if (!child)
        return;

    int64_t idx = find_entry(child);
    if (idx >= 0)
        g_gc.entries[idx].trial_rc--;
}

/// Lightweight snapshot entry for traversal outside the lock.
typedef struct {
    void *obj;
    rt_gc_traverse_fn traverse;
    int8_t retained;
} gc_snap_entry;

/// @brief Per-member state retained across cycle finalization and recovery.
typedef struct {
    gc_snap_entry entry;
    gc_snap_entry *snapshot_entry;
    size_t saved_refs;
    rt_heap_finalizer_t saved_finalizer;
    int snapshot_released;
    int finalized;
    int finalizer_cleared;
    int resurrected;
    int reclaimed;
} gc_garbage_state;

/// @brief Dynamically growing list of strong edges reported by traversals.
typedef struct {
    void **items;
    int64_t count;
    int64_t cap;
    int oom;
} gc_edge_list;

/// @brief Dynamically growing restore-phase traversal queue.
typedef struct {
    gc_snap_entry *items;
    int64_t count;
    int64_t cap;
    int oom;
} gc_worklist;

/// @brief Append @p child to a heap-resident edge list, growing the buffer on demand.
/// @details Used by the trial-deletion phase to record outgoing references collected by
///          a traversal callback. The buffer doubles each time it fills and the OOM flag
///          sticks once set so subsequent pushes are no-ops — callers detect overflow by
///          inspecting `list->oom` after the traversal completes.
/// @param list Mutable edge list that owns its backing allocation.
/// @param child Non-NULL borrowed child address to append.
/// @return 1 on successful append, 0 on OOM, NULL inputs, or NULL @p child.
static int gc_edge_list_push(gc_edge_list *list, void *child) {
    if (!list || list->oom || !child)
        return 0;
    if (list->count == list->cap) {
        if (list->cap > INT64_MAX / 2) {
            list->oom = 1;
            return 0;
        }
        int64_t new_cap = list->cap ? list->cap * 2 : 16;
        if ((uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(void *)) {
            list->oom = 1;
            return 0;
        }
        void **items = (void **)realloc(list->items, (size_t)new_cap * sizeof(void *));
        if (!items) {
            list->oom = 1;
            return 0;
        }
        list->items = items;
        list->cap = new_cap;
    }
    list->items[list->count++] = child;
    return 1;
}

/// @brief `rt_gc_visitor_t` adapter that funnels traversed children into a `gc_edge_list`.
/// @details Bridges the per-object traverse callback (which expects a `(child, ctx)`
///          visitor) to the edge-list collector. Discards push failures since the caller
///          checks `list->oom` after the walk completes.
/// @param child Borrowed child address supplied by the tracked object's traversal.
/// @param ctx Non-NULL `gc_edge_list` destination.
static void gc_edge_collector(void *child, void *ctx) {
    (void)gc_edge_list_push((gc_edge_list *)ctx, child);
}

/// @brief Push a `(obj, traverse)` pair onto the restore-phase BFS worklist.
/// @details Used by phase 3 (the "restore" pass) to enqueue objects that need to be
///          re-marked as live after the trial-deletion phase. Doubling growth and
///          sticky OOM flag mirror `gc_edge_list_push`.
/// @param work Mutable restore queue that owns its backing allocation.
/// @param obj Non-NULL borrowed tracked payload to enqueue.
/// @param traverse Non-NULL strong-edge callback paired with @p obj.
/// @return 1 on success, 0 on OOM, NULL inputs, or missing traverse function.
static int gc_worklist_push(gc_worklist *work, void *obj, rt_gc_traverse_fn traverse) {
    if (!work || work->oom || !obj || !traverse)
        return 0;
    if (work->count == work->cap) {
        if (work->cap > INT64_MAX / 2) {
            work->oom = 1;
            return 0;
        }
        int64_t new_cap = work->cap ? work->cap * 2 : 16;
        if ((uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(gc_snap_entry)) {
            work->oom = 1;
            return 0;
        }
        gc_snap_entry *items =
            (gc_snap_entry *)realloc(work->items, (size_t)new_cap * sizeof(gc_snap_entry));
        if (!items) {
            work->oom = 1;
            return 0;
        }
        work->items = items;
        work->cap = new_cap;
    }
    work->items[work->count].obj = obj;
    work->items[work->count].traverse = traverse;
    work->count++;
    return 1;
}

/// @brief Release one snapshot entry's retained reference, if any.
/// @details Called from both the success and trap-recovery paths to unwind a
///          retain acquired by the snapshot construction step. Skips entries
///          that were never retained, never had a live payload, or whose
///          payload has since been detached. On a refcount-to-zero result,
///          frees the heap object.
/// @param entry Snapshot entry to release; NULL is treated as a no-op.
static void gc_release_snapshot_entry(gc_snap_entry *entry) {
    if (!entry || !entry->retained || !entry->obj || !rt_heap_is_payload(entry->obj))
        return;
    void *obj = entry->obj;
    entry->retained = 0;
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

typedef struct {
    const gc_pointer_set *garbage_members;
} gc_release_edges_ctx;

/// @brief Reclaim-phase visitor: drop one outgoing reference from a garbage object.
/// @details Called for each child reachable from an object being freed. Skips children
///          that are also in the garbage set — those will be freed by their own
///          reclaim-phase walk and re-releasing them here would underflow the refcount.
///          External (non-garbage) children get a normal release-and-free-on-zero.
/// @param child Borrowed outgoing target reported by a garbage member traversal.
/// @param ctx Optional `gc_release_edges_ctx` identifying intra-cycle members.
static void gc_release_outgoing_ref(void *child, void *ctx) {
    if (!child)
        return;
    gc_release_edges_ctx *release_ctx = (gc_release_edges_ctx *)ctx;
    if (release_ctx && gc_pointer_set_contains(release_ctx->garbage_members, child))
        return;
    if (rt_obj_release_check0(child))
        rt_obj_free(child);
}

/// @brief Prepare a confirmed-unreachable cycle member for finalization.
/// @details Drops the GC snapshot retain, remembers the object's real refcount,
///          zeros the count for finalizer semantics, and runs the finalizer once.
///          The caller either restores every garbage member when any object
///          resurrects, or frees every member when no resurrection occurs.
/// @param state Mutable reclaim record for one confirmed garbage member.
static void gc_finalize_unreachable(gc_garbage_state *state) {
    if (!state || !state->entry.obj)
        return;
    void *obj = state->entry.obj;
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(obj, &hdr) || !hdr)
        return;

    size_t current_refs = rt_heap_release_deferred(obj);
    state->snapshot_released = 1;
    if (state->snapshot_entry)
        state->snapshot_entry->retained = 0;
    state->saved_refs = current_refs;
    if (current_refs >= RT_HEAP_IMMORTAL_REFCNT) {
        rt_trap("gc: cannot collect immortal heap object");
        return;
    }

    __atomic_store_n(&hdr->refcnt, 0, __ATOMIC_RELEASE);

    if ((rt_heap_kind_t)hdr->kind == RT_HEAP_OBJECT) {
        state->finalized = 1;
        if (hdr->finalizer) {
            rt_heap_finalizer_t fin = hdr->finalizer;
            state->saved_finalizer = fin;
            state->finalizer_cleared = 1;
            hdr->finalizer = NULL;
            if (g_gc_suppress_member_release_depth == UINT32_MAX)
                rt_abort("gc: finalizer release suppression overflow");
            g_gc_suppress_member_release_depth++;
            fin(obj);
            g_gc_suppress_member_release_depth--;
        }
        if (__atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE) != 0) {
            state->resurrected = 1;
            return;
        }
    }
}

/// @brief Restore garbage members after finalizer resurrection or trap recovery.
/// @details Reconstitutes released snapshot references, optionally reinstalls
///   detached finalizers that did not resurrect, and re-registers surviving
///   payloads with their original traversal callbacks.
/// @param garbage Array of mutable reclaim records; NULL is a no-op.
/// @param garbage_count Number of entries in @p garbage.
/// @param restore_finalizers Non-zero to reinstall eligible detached callbacks.
static void gc_restore_garbage_state(gc_garbage_state *garbage,
                                     int64_t garbage_count,
                                     int restore_finalizers) {
    if (!garbage)
        return;
    for (int64_t i = 0; i < garbage_count; ++i) {
        gc_garbage_state *state = &garbage[i];
        void *obj = state->entry.obj;
        if (!obj || state->reclaimed || !rt_heap_is_payload(obj))
            continue;
        rt_heap_hdr_t *hdr = NULL;
        if (!rt_heap_try_get_header(obj, &hdr) || !hdr)
            continue;
        if (state->snapshot_released) {
            size_t current = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
            size_t restored = state->saved_refs;
            if (current > 0) {
                if (restored > SIZE_MAX - current)
                    restored = SIZE_MAX;
                else
                    restored += current;
            }
            __atomic_store_n(&hdr->refcnt, restored, __ATOMIC_RELEASE);
        }
        if (restore_finalizers && state->finalizer_cleared && state->finalized &&
            !state->resurrected && (rt_heap_kind_t)hdr->kind == RT_HEAP_OBJECT && !hdr->finalizer)
            hdr->finalizer = state->saved_finalizer;
        if (state->entry.traverse)
            rt_gc_track(obj, state->entry.traverse);
    }
}

/// @brief Free a finalized unreachable object after the no-resurrection decision.
/// @details Releases outgoing non-cycle references, clears weak observers and
///   monitor state, then frees the zero-ref heap payload exactly once.
/// @param state Mutable reclaim record for the finalized object.
/// @param release_ctx Edge-release context containing the complete garbage set.
/// @return 1 when the payload was reclaimed, otherwise 0.
static int gc_free_finalized_unreachable(gc_garbage_state *state, void *release_ctx) {
    if (!state || !state->entry.obj || state->reclaimed)
        return 0;
    void *obj = state->entry.obj;
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(obj, &hdr) || !hdr)
        return 0;

    if (state->entry.traverse)
        state->entry.traverse(obj, gc_release_outgoing_ref, release_ctx);
    rt_gc_clear_weak_refs(obj);
    if ((rt_heap_kind_t)hdr->kind == RT_HEAP_OBJECT)
        rt_monitor_forget(obj);

    rt_heap_free_zero_ref(obj);
    state->reclaimed = 1;
    return 1;
}

/// @brief Run one synchronous cycle-collection pass.
/// @details Implements a four-phase trial-deletion algorithm:
///   Phase 1: Initialize trial refcounts from the real heap-header refcount.
///   Phase 2: Trial-decrement children — objects whose trial_rc drops to 0 are
///            referenced only by other tracked objects (potential cycle members).
///   Phase 3: Restore reachable objects (trial_rc > 0) by marking them black and
///            recursively marking all their children.
///   Phase 4: Collect white objects (unreachable cycle members). Invoke finalizers
///            outside the lock and clear weak refs only for objects not resurrected.
/// The exclusive managed-graph barrier spans all four phases, making traversal
/// observe a stable set of strong edges and matching reference counts.
/// @return Number of objects freed. Reentrant calls, calls deferred from a
///   mutator scope, empty sets, and returning error traps yield zero.
int64_t rt_gc_collect(void) {
    int64_t freed = 0;
    int64_t snap_count = 0;
    gc_snap_entry *snapshot = NULL;
    gc_edge_list trial_edges = {0};
    gc_worklist work = {0};
    int64_t garbage_count = 0;
    gc_garbage_state *garbage = NULL;
    gc_pointer_set garbage_members = {0};
    jmp_buf collection_recovery;

    if (!gc_world_begin_collection())
        return 0;

    /* Consume debt that existed before this pass. Allocations occurring after
       this exchange publish a fresh request for the next safe boundary. */
    __atomic_store_n(&g_gc_collection_requested, 0, __ATOMIC_RELEASE);

    gc_lock();

    if (g_gc.collecting) {
        gc_unlock();
        gc_world_end_collection();
        return 0;
    }

    if (g_gc.count == 0) {
        g_gc.pass_count++;
        gc_unlock();
        gc_world_end_collection();
        return 0;
    }

    g_gc.collecting = 1;

    /* Epoch tagging: every GC_FULL_SCAN_INTERVAL passes, scan ALL objects
       regardless of promotion status to catch new cycles involving
       long-lived objects. */
    int full_scan = (g_gc.pass_count % GC_FULL_SCAN_INTERVAL == 0);

    /* Phase 1: Initialize trial refcounts and build a snapshot of all live
       entries for safe traversal outside the lock.  Promoted objects are
       included in the snapshot but marked black (skipped) unless this is
       a full scan pass. */
    if (g_gc.count < 0 || (uint64_t)g_gc.count > (uint64_t)SIZE_MAX / sizeof(gc_snap_entry)) {
        g_gc.collecting = 0;
        gc_unlock();
        gc_world_end_collection();
        rt_trap("gc: snapshot size overflow");
        return 0;
    }

    snapshot = (gc_snap_entry *)malloc((size_t)g_gc.count * sizeof(gc_snap_entry));
    if (!snapshot) {
        g_gc.collecting = 0;
        gc_unlock();
        gc_world_end_collection();
        rt_trap("gc: memory allocation failed");
        return 0;
    }

    for (int64_t i = 0; i < g_gc.capacity; i++) {
        if (!gc_slot_is_live(&g_gc.entries[i]))
            continue;

        rt_heap_hdr_t *hdr = NULL;
        if (!rt_heap_try_get_header(g_gc.entries[i].obj, &hdr) || !hdr) {
            g_gc.collecting = 0;
            gc_unlock();
            for (int64_t j = 0; j < snap_count; j++)
                gc_release_snapshot_entry(&snapshot[j]);
            free(snapshot);
            gc_world_end_collection();
            rt_trap("gc: tracked object is not a live heap payload");
            return 0;
        }
        size_t refcnt = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
        g_gc.entries[i].trial_rc = refcnt > (size_t)INT64_MAX ? INT64_MAX : (int64_t)refcnt;

        // A normal release may have reached zero immediately before its
        // destructor enters the mutator barrier. The payload is stable while
        // this exclusive pass runs, but it is already owned by that pending
        // destruction path and must not be retained or cycle-collected here.
        if (refcnt == 0) {
            g_gc.entries[i].color = 2;
            continue;
        }

        /* Skip promoted objects in non-full-scan passes: mark them black
           so they are treated as reachable without trial-deletion. */
        if (refcnt >= RT_HEAP_IMMORTAL_REFCNT)
            g_gc.entries[i].color = 2; /* black = immortal/static */
        else if (!full_scan && g_gc.entries[i].survived >= GC_PROMOTION_THRESHOLD)
            g_gc.entries[i].color = 2; /* black = skip */
        else
            g_gc.entries[i].color = 0; /* white = candidate */

        void *snapshot_obj = g_gc.entries[i].obj;
        int32_t retained = rt_heap_try_retain_live(snapshot_obj);
        if (retained <= 0) {
            g_gc.collecting = 0;
            gc_unlock();
            for (int64_t j = 0; j < snap_count; j++)
                gc_release_snapshot_entry(&snapshot[j]);
            free(snapshot);
            gc_world_end_collection();
            rt_trap(retained < 0 ? "gc: snapshot retain refcount overflow"
                                 : "gc: tracked object is not live");
            return 0;
        }

        snapshot[snap_count].obj = snapshot_obj;
        snapshot[snap_count].traverse = g_gc.entries[i].traverse;
        snapshot[snap_count].retained = retained == 1;
        snap_count++;
    }

    gc_unlock();

    rt_trap_set_recovery(&collection_recovery);
    if (RT_SETJMP(collection_recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "gc: trap during collection");
        rt_trap_clear_recovery();
        g_gc_active_reclaim_set = NULL;
        g_gc_suppress_member_release_depth = 0;
        free(trial_edges.items);
        free(work.items);
        gc_restore_garbage_state(garbage, garbage_count, 1);
        for (int64_t i = 0; i < snap_count; i++)
            gc_release_snapshot_entry(&snapshot[i]);
        gc_pointer_set_destroy(&garbage_members);
        free(garbage);
        free(snapshot);
        gc_clear_collecting_flag();
        gc_world_end_collection();
        rt_trap(saved_error);
        return 0;
    }

    /* Phase 2: Trial decrement — for each tracked object, visit its
       children.  If a child is also tracked, decrement its trial_rc.
       After this phase, objects whose trial_rc <= 0 are only referenced
       by other tracked objects (potential cycle members). */
    for (int64_t i = 0; i < snap_count; i++) {
        snapshot[i].traverse(snapshot[i].obj, gc_edge_collector, &trial_edges);
    }
    if (trial_edges.oom) {
        rt_trap("gc: memory allocation failed");
        return 0;
    }

    gc_lock();
    for (int64_t i = 0; i < trial_edges.count; ++i) {
        trial_decrement(trial_edges.items[i], NULL);
    }
    gc_unlock();
    free(trial_edges.items);
    trial_edges.items = NULL;
    trial_edges.count = 0;
    trial_edges.cap = 0;

    /* Phase 3: Scan — objects with trial_rc > 0 have external references
       and are definitely reachable.  Mark them black and recursively
       mark everything reachable from them. Traversal callbacks run outside
       gc_lock so callbacks can safely use weak/GC APIs. */
    gc_lock();
    for (int64_t i = 0; i < snap_count; i++) {
        int64_t idx = find_entry(snapshot[i].obj);
        if (idx >= 0 && g_gc.entries[idx].color == 2)
            gc_worklist_push(&work, snapshot[i].obj, snapshot[i].traverse);
    }
    for (int64_t i = 0; i < snap_count; i++) {
        int64_t idx = find_entry(snapshot[i].obj);
        if (idx >= 0 && g_gc.entries[idx].trial_rc > 0 && g_gc.entries[idx].color != 2) {
            g_gc.entries[idx].color = 2; /* black = definitely reachable */
            gc_worklist_push(&work, snapshot[i].obj, snapshot[i].traverse);
        }
    }
    gc_unlock();

    for (int64_t wi = 0; wi < work.count; ++wi) {
        gc_edge_list restore_edges = {0};
        work.items[wi].traverse(work.items[wi].obj, gc_edge_collector, &restore_edges);
        if (restore_edges.oom) {
            work.oom = 1;
            free(restore_edges.items);
            break;
        }

        gc_lock();
        for (int64_t ei = 0; ei < restore_edges.count; ++ei) {
            int64_t idx = find_entry(restore_edges.items[ei]);
            if (idx >= 0 && g_gc.entries[idx].color != 2) {
                g_gc.entries[idx].color = 2;
                gc_worklist_push(&work, g_gc.entries[idx].obj, g_gc.entries[idx].traverse);
            }
        }
        gc_unlock();
        free(restore_edges.items);
        if (work.oom)
            break;
    }

    if (work.oom) {
        rt_trap("gc: memory allocation failed");
        return 0;
    }
    free(work.items);
    work.items = NULL;
    work.count = 0;
    work.cap = 0;

    /* Phase 3b: Epoch tagging — increment survived counter for objects
       that survived this pass (color == 2, reachable). */
    gc_lock();
    for (int64_t i = 0; i < g_gc.capacity; i++) {
        if (gc_slot_is_live(&g_gc.entries[i]) && g_gc.entries[i].color == 2) {
            if (g_gc.entries[i].survived < UINT16_MAX)
                g_gc.entries[i].survived++;
        }
    }

    /* Phase 4: Collect — white objects are unreachable cycle members.
       Gather them, remove from the hash table, then finalize and free. */
    for (int64_t i = 0; i < snap_count; i++) {
        int64_t idx = find_entry(snapshot[i].obj);
        if (idx >= 0 && g_gc.entries[idx].color == 0)
            garbage_count++;
    }

    if (garbage_count > 0) {
        garbage = (gc_garbage_state *)calloc((size_t)garbage_count, sizeof(gc_garbage_state));
        if (!garbage) {
            gc_unlock();
            rt_trap("gc: memory allocation failed");
            return 0;
        }
        int64_t gi = 0;
        for (int64_t i = 0; i < snap_count && gi < garbage_count; i++) {
            int64_t idx = find_entry(snapshot[i].obj);
            if (idx >= 0 && g_gc.entries[idx].color == 0) {
                garbage[gi].entry = snapshot[i];
                garbage[gi].snapshot_entry = &snapshot[i];
                gi++;
            }
        }
    }

    g_gc.pass_count++;

    gc_unlock();

    /* Free garbage objects (outside the lock). */
    if (garbage) {
        if (!gc_pointer_set_init(&garbage_members, garbage_count)) {
            rt_trap("gc: memory allocation failed");
            return 0;
        }
        for (int64_t i = 0; i < garbage_count; ++i)
            gc_pointer_set_insert(&garbage_members, garbage[i].entry.obj);
        g_gc_active_reclaim_set = &garbage_members;

        gc_release_edges_ctx release_ctx = {&garbage_members};
        int resurrected = 0;
        for (int64_t i = 0; i < garbage_count; i++) {
            rt_gc_untrack(garbage[i].entry.obj);
            if (rt_heap_is_payload(garbage[i].entry.obj))
                gc_finalize_unreachable(&garbage[i]);
            if (garbage[i].resurrected)
                resurrected = 1;
        }
        if (resurrected) {
            gc_restore_garbage_state(garbage, garbage_count, 1);
        } else {
            for (int64_t i = 0; i < garbage_count; i++) {
                if (gc_free_finalized_unreachable(&garbage[i], &release_ctx))
                    freed++;
            }
        }
        g_gc_active_reclaim_set = NULL;
        g_gc_suppress_member_release_depth = 0;
    }

    for (int64_t i = 0; i < snap_count; i++) {
        if (gc_pointer_set_contains(&garbage_members, snapshot[i].obj))
            continue;
        gc_release_snapshot_entry(&snapshot[i]);
    }

    gc_pointer_set_destroy(&garbage_members);
    free(garbage);
    free(snapshot);

    rt_trap_clear_recovery();

    gc_lock();
    if (freed > 0) {
        if (g_gc.total_collected > INT64_MAX - freed)
            g_gc.total_collected = INT64_MAX;
        else
            g_gc.total_collected += freed;
    }
    g_gc.collecting = 0;
    gc_unlock();

    gc_world_end_collection();
    return freed;
}

//=============================================================================
// Auto-Trigger
//=============================================================================

/// @brief Configure the automatic-collection allocation-debt threshold.
/// @details When n > 0, every n-th heap allocation publishes a collection
///          request. The request is serviced at a mutator boundary or explicit
///          @ref rt_gc_safepoint, never recursively inside the allocator.
///          Set to 0 (default) to disable automatic collection.
/// @param n Positive allocation count between requests; zero or negative
///   disables triggering. Changing it also clears accumulated debt.
void rt_gc_set_threshold(int64_t n) {
    __atomic_store_n(&g_gc_threshold, n > 0 ? n : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_gc_alloc_counter, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_gc_collection_requested, 0, __ATOMIC_RELEASE);
}

/// @brief Read the current auto-collection threshold (0 = disabled).
/// @return Atomically observed positive threshold, or zero when disabled.
int64_t rt_gc_get_threshold(void) {
    return __atomic_load_n(&g_gc_threshold, __ATOMIC_RELAXED);
}

/// @brief Record one heap allocation as automatic-collection debt.
/// @details Advances a saturating CAS-managed counter. The thread that crosses
///          the configured threshold resets the counter and publishes one
///          coalescing collection request. This routine performs no collection,
///          finalization, or user callback and is therefore safe in a partially
///          initialized allocator call stack.
void rt_gc_notify_alloc(void) {
    int64_t threshold = __atomic_load_n(&g_gc_threshold, __ATOMIC_RELAXED);
    if (threshold <= 0)
        return;

    int64_t observed = __atomic_load_n(&g_gc_alloc_counter, __ATOMIC_RELAXED);
    for (;;) {
        int threshold_crossed = observed >= threshold - 1 || observed < 0;
        int64_t desired = threshold_crossed ? 0 : observed + 1;
        if (gc_atomic_cas_i64(&g_gc_alloc_counter, &observed, desired)) {
            if (threshold_crossed)
                __atomic_store_n(&g_gc_collection_requested, 1, __ATOMIC_RELEASE);
            return;
        }
    }
}

/// @brief Service coalesced automatic-collection debt at a safe boundary.
/// @details A thread outside a managed-graph mutator or collector scope claims
///          the global request with an atomic exchange and runs one synchronous
///          pass. Calls made inside a mutator defer the request to the outermost
///          exit. Calls from collector callbacks leave the request pending for a
///          later ordinary thread. Multiple threshold crossings before a safe
///          boundary intentionally coalesce into one pass.
void rt_gc_safepoint(void) {
    if (!__atomic_load_n(&g_gc_collection_requested, __ATOMIC_ACQUIRE))
        return;
    if (g_gc_world_exclusive)
        return;
    if (g_gc_mutator_depth != 0) {
        g_gc_collection_pending = 1;
        return;
    }

    int64_t expected = 1;
    if (!gc_atomic_cas_i64(&g_gc_collection_requested, &expected, 0))
        return;
    (void)rt_gc_collect();
}

//=============================================================================
// Statistics
//=============================================================================

/// @brief Return cumulative count of objects freed by cycle collection.
/// @return Locked, saturating cumulative count since startup or the last
///   @ref rt_gc_shutdown.
int64_t rt_gc_total_collected(void) {
    gc_lock();
    int64_t n = g_gc.total_collected;
    gc_unlock();
    return n;
}

/// @brief Return the number of collection passes run since startup.
/// @return Locked pass count since startup or the last @ref rt_gc_shutdown.
int64_t rt_gc_pass_count(void) {
    gc_lock();
    int64_t n = g_gc.pass_count;
    gc_unlock();
    return n;
}

//=============================================================================
// Shutdown
//=============================================================================

/// @brief Run one detached shutdown finalizer and release its temporary pin.
/// @details Installs a local recovery point because finalizers and the last
///          release of their temporary retain may trap. Recovery releases the
///          pin when it is still owned, clears the process-wide collection
///          sentinel, and rethrows the original diagnostic. The finalizer has
///          already been removed from its heap header, matching normal
///          at-most-once finalization semantics even when it traps.
/// @param obj Live tracked object selected under the exclusive graph barrier.
/// @param finalizer Detached non-null finalizer callback.
/// @param retained Non-zero when the sweep owns one temporary mortal reference.
static void gc_run_shutdown_finalizer(void *obj, rt_heap_finalizer_t finalizer, int retained) {
    volatile int owns_retain = retained ? 1 : 0;
    jmp_buf finalizer_recovery;
    rt_trap_set_recovery(&finalizer_recovery);
    if (RT_SETJMP(finalizer_recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "gc: trap during finalizer sweep");
        rt_trap_clear_recovery();
        gc_clear_collecting_flag();
        if (owns_retain && obj && rt_heap_is_payload(obj)) {
            owns_retain = 0;
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
        }
        rt_trap(saved_error);
        return;
    }

    finalizer(obj);
    if (owns_retain && obj && rt_heap_is_payload(obj)) {
        owns_retain = 0;
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
    }
    rt_trap_clear_recovery();
}

/// @brief Run every currently tracked object finalizer without snapshot allocation.
/// @details Uses a monotonically increasing epoch stored in each tracking-table
///          entry. Entries are marked while both the GC lock and exclusive graph
///          barrier are held; each selected mortal object is temporarily retained
///          and its finalizer is detached before both locks are released. The
///          callback then runs without runtime-global locks, after which the sweep
///          reacquires them and resumes. If a callback causes table rehashing the
///          cursor restarts, while epoch marks prevent duplicate work.
///
///          The walk allocates no snapshot or worklist, so allocator exhaustion
///          cannot skip cleanup of files, sockets, or other native resources.
///          Zero-ref payloads remain owned by their ordinary deferred-destruction
///          path and are deliberately skipped to prevent double finalization.
void rt_gc_run_all_finalizers(void) {
    if (!gc_world_begin_exclusive(0))
        return;

    gc_lock();
    if (g_gc.collecting) {
        gc_unlock();
        gc_world_end_collection();
        return;
    }
    if (g_gc.count == 0) {
        gc_unlock();
        gc_world_end_collection();
        return;
    }

    g_gc.collecting = 1;
    uint64_t sweep_epoch = g_gc.finalizer_epoch + 1;
    if (sweep_epoch == 0) {
        for (int64_t i = 0; i < g_gc.capacity; ++i)
            g_gc.entries[i].finalizer_epoch = 0;
        sweep_epoch = 1;
    }
    g_gc.finalizer_epoch = sweep_epoch;

    int64_t cursor = 0;
    gc_entry *observed_entries = g_gc.entries;
    int64_t observed_capacity = g_gc.capacity;
    for (;;) {
        void *obj = NULL;
        rt_heap_finalizer_t finalizer = NULL;
        int retained = 0;
        int retain_overflow = 0;

        while (cursor < g_gc.capacity) {
            gc_entry *entry = &g_gc.entries[cursor++];
            if (!gc_slot_is_live(entry) || entry->finalizer_epoch == sweep_epoch)
                continue;
            entry->finalizer_epoch = sweep_epoch;

            rt_heap_hdr_t *hdr = NULL;
            if (!rt_heap_try_get_header(entry->obj, &hdr) || !hdr)
                continue;
            size_t refs = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
            if (refs == 0 || (rt_heap_kind_t)hdr->kind != RT_HEAP_OBJECT || !hdr->finalizer)
                continue;

            int pin_result = rt_heap_try_retain_live(entry->obj);
            if (pin_result < 0) {
                retain_overflow = 1;
                break;
            }
            if (pin_result == 0)
                continue;

            obj = entry->obj;
            retained = pin_result == 1;
            finalizer = hdr->finalizer;
            hdr->finalizer = NULL;
            break;
        }

        if (retain_overflow) {
            g_gc.collecting = 0;
            gc_unlock();
            gc_world_end_collection();
            rt_trap("gc: shutdown finalizer retain refcount overflow");
            return;
        }

        if (!finalizer) {
            int has_unvisited = 0;
            for (int64_t i = 0; i < g_gc.capacity; ++i) {
                if (gc_slot_is_live(&g_gc.entries[i]) &&
                    g_gc.entries[i].finalizer_epoch != sweep_epoch) {
                    has_unvisited = 1;
                    break;
                }
            }
            if (has_unvisited) {
                cursor = 0;
                observed_entries = g_gc.entries;
                observed_capacity = g_gc.capacity;
                continue;
            }

            g_gc.collecting = 0;
            gc_unlock();
            gc_world_end_collection();
            return;
        }

        gc_unlock();
        gc_world_end_collection();
        gc_run_shutdown_finalizer(obj, finalizer, retained);

        if (!gc_world_begin_exclusive(0)) {
            gc_clear_collecting_flag();
            return;
        }
        gc_lock();
        if (g_gc.entries != observed_entries || g_gc.capacity != observed_capacity) {
            cursor = 0;
            observed_entries = g_gc.entries;
            observed_capacity = g_gc.capacity;
        }
    }
}

/// @brief Zero registered weak handles, then free every detached weak-chain node and bucket.
/// @details Shutdown detaches the registry while targets and weak handles may
///          still be alive in an embedder. Clearing each handle prevents a
///          later target free from leaving an unregistered dangling weak
///          pointer after the bucket table itself has gone away.
/// @param weak_buckets Detached bucket array, or NULL.
/// @param weak_bucket_count Number of initialized buckets in @p weak_buckets.
static void free_weak_buckets(weak_chain *weak_buckets, int64_t weak_bucket_count) {
    if (!weak_buckets)
        return;
    for (int64_t i = 0; i < weak_bucket_count; i++) {
        weak_chain *wc = weak_buckets[i].next;
        while (wc) {
            weak_chain *next = wc->next;
            rt_weakref *ref = wc->head;
            while (ref) {
                rt_weakref *next_ref = ref->next_for_target;
                ref->target = NULL;
                ref->next_for_target = NULL;
                ref = next_ref;
            }
            free(wc);
            wc = next;
        }
    }
    free(weak_buckets);
}

/// @brief Tear down GC state at process exit or embedder reset.
/// @details Detaches the tracking and weak-ref tables while holding the GC lock,
/// then frees the detached storage after unlocking. This keeps shutdown
/// idempotent and allows a later allocation/GC pass to lazily recreate the
/// tables without touching a destroyed mutex. Weak handles are zeroed and all
/// counters, thresholds, and request state are reset. Calls made while
/// collection is active or graph exclusivity cannot be acquired are no-ops.
void rt_gc_shutdown(void) {
    if (!gc_world_begin_exclusive(0))
        return;

    gc_lock();

    if (g_gc.collecting) {
        gc_unlock();
        gc_world_end_collection();
        return;
    }

    gc_entry *entries = g_gc.entries;
    weak_chain *weak_buckets = g_gc.weak_buckets;
    int64_t weak_bucket_count = g_gc.weak_bucket_count;

    g_gc.entries = NULL;
    g_gc.count = 0;
    g_gc.capacity = 0;
    g_gc.weak_buckets = NULL;
    g_gc.weak_bucket_count = 0;
    g_gc.total_collected = 0;
    g_gc.pass_count = 0;
    g_gc.collecting = 0;
    g_gc.finalizer_epoch = 0;

    /* Reset auto-trigger state. */
    __atomic_store_n(&g_gc_threshold, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_gc_alloc_counter, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_gc_collection_requested, 0, __ATOMIC_RELEASE);

    gc_unlock();

    free(entries);
    free_weak_buckets(weak_buckets, weak_bucket_count);
    gc_world_end_collection();
}
