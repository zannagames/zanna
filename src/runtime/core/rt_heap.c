//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_heap.c
// Purpose: Reference-counted heap allocator used by runtime string, array,
//   and object payloads. Defines the canonical metadata header layout
//   (magic tag, ref count, heap kind, len, cap), checked header lookup helpers,
//   rt_heap_data, and lifetime management operations
//   (rt_heap_alloc, rt_heap_retain, rt_heap_release, rt_heap_realloc).
//   VM, native, and host embedding paths share this single implementation.
//
// Key invariants:
//   - Every heap allocation is preceded by an rt_heap_hdr_t carrying
//     magic==RT_MAGIC. Public helpers validate live payloads against the
//     registry before touching the header.
//   - refcnt==0 is logically dead and awaits immediate or explicit deferred
//     reclamation; refcnt>=RT_HEAP_IMMORTAL_REFCNT is an immortal/static
//     sentinel. Release operations publish with release semantics.
//   - Final reclamation zeroes weak observers before unregistering the payload.
//   - rt_heap_release() returns non-pooled blocks through rt_free only when
//     refcnt drops to 0; pooled string blocks return to their owning slab.
//   - The kind field (RT_HEAP_*) disambiguates string, array, and object payloads.
//   - len/cap are logical element counts for string/array payloads; raw
//     (opaque) payloads leave len=0, cap=byte size.
//
// Ownership/Lifetime:
//   - Callers own the payload pointer returned by rt_heap_alloc.
//   - Call rt_heap_retain before sharing a pointer; rt_heap_release when done.
//     The final release frees the entire block (header + payload).
//
// Links: src/runtime/core/rt_heap.h (public API),
//        src/runtime/core/rt_pool.h (slab pool for small allocations)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the registered, reference-counted runtime heap.

#include "rt_heap.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_pool.h"
#include "rt_string_intern.h"
#include "rt_string_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !RT_PLATFORM_WINDOWS
#include <sched.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RT_HEAP_UNUSED_PRIVATE __attribute__((unused))
#else
#define RT_HEAP_UNUSED_PRIVATE
#endif

/// @brief Allocate zero-initialized storage for a managed heap block.
/// @details Routes payload-block allocation through the runtime allocation
///          shim so embedders and deterministic OOM tests observe the same
///          allocation path as other runtime subsystems. The shim accepts an
///          signed byte count, so requests outside its representable range are
///          rejected before conversion. Storage returned here must be released
///          with @ref rt_free.
/// @param bytes Total header-plus-payload byte count.
/// @return Zero-initialized storage, or NULL when the size or allocation is
///         rejected by the configured allocator.
static void *rt_heap_alloc_zeroed_(size_t bytes) {
    if (bytes > (size_t)INT64_MAX)
        return NULL;
    return rt_alloc((int64_t)bytes);
}

//=============================================================================
// Global shutdown handler
//=============================================================================

/// @brief Whether the atexit shutdown handler has been registered.
static int g_shutdown_registered = 0;

/* Audio shutdown is defined in rt_audio.c (real impl or stub no-op).
   Weak default here so programs that don't use audio link without pulling
   in the audio component and its zannaaud dependency.  When the audio
   component IS linked, its strong definition overrides this no-op. */
#if defined(_MSC_VER)
/// @brief Shut down the optional audio subsystem during global runtime teardown.
extern void rt_audio_shutdown(void);
#else
/// @brief Weak no-op audio shutdown used when the audio component is not linked.
__attribute__((weak)) void rt_audio_shutdown(void) {}
#endif

/// @brief Release process-global legacy runtime context state.
extern void rt_legacy_context_shutdown(void);

/// @brief Detach and free the live-payload registry's slot storage.
static void rt_heap_registry_shutdown_(void);

/// @brief Global shutdown handler called at process exit via atexit().
/// @details Releases runtime global state in dependency order:
///          1. GC finalizer sweep (flush files, close sockets, release GPU/audio handles)
///          2. Audio system (destroy audio device after sound/music handles freed)
///          3. Legacy context (close BASIC file channels, release args/type registry)
///          4. Interned strings (may free pool-allocated memory)
///          5. GC tables (tracking array, weak ref buckets)
///          6. Pool slabs (must be last — other cleanup may touch pool memory)
static void rt_global_shutdown(void) {
    rt_gc_run_all_finalizers();
    rt_audio_shutdown();
    rt_legacy_context_shutdown();
    rt_string_intern_drain();
    rt_string_literal_cache_shutdown();
    rt_string_registry_shutdown();
    rt_gc_shutdown();
    rt_heap_registry_shutdown_();
    rt_pool_shutdown();
}

#if RT_PLATFORM_LINUX
/// @brief Register a DSO-aware process-exit callback with the Linux C++ ABI.
/// @param func Callback accepting @p arg.
/// @param arg Opaque callback argument.
/// @param dso_handle Optional DSO identity; NULL registers for process exit.
/// @return Zero on success, or a nonzero registration error.
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);

/// @brief __cxa_atexit trampoline that runs rt_global_shutdown at process exit
///        (Linux only; matches the void(*)(void*) callback signature).
/// @param arg Unused registration argument.
static void rt_global_shutdown_atexit_(void *arg) {
    (void)arg;
    rt_global_shutdown();
}

/// @brief Register rt_global_shutdown to run at process exit.
/// @details Linux late-bound native executables don't get a usable atexit(),
///          so the Linux variant routes through libc's __cxa_atexit(); all
///          other platforms use plain atexit().
/// @return Zero on success, or a nonzero registration error.
static int rt_register_shutdown_handler_(void) {
    // glibc does not export atexit() for late-bound native executables, but
    // __cxa_atexit() is available from libc and feeds the same exit handler list.
    return __cxa_atexit(rt_global_shutdown_atexit_, NULL, NULL);
}
#elif RT_PLATFORM_WINDOWS
/// @brief Accept shutdown registration for CRT-less Windows native binaries.
/// @details Windows relies on process teardown rather than calling CRT
///   `atexit`, which can block from the custom startup path.
/// @return Always zero.
static int rt_register_shutdown_handler_(void) {
    // The Windows runtime archive is shared with native PE binaries that enter
    // through Zanna's CRT-less startup shim. Calling CRT atexit from that path
    // can block during the first heap-backed allocation, so Windows builds rely
    // on process teardown for this global cleanup.
    return 0;
}
#else
/// @brief Register @ref rt_global_shutdown through the platform C runtime.
/// @return Zero on success, or the nonzero result from `atexit`.
static int rt_register_shutdown_handler_(void) {
    return atexit(rt_global_shutdown);
}
#endif

//=============================================================================
// Live Payload Registry
//=============================================================================

/// @brief Never-used live-payload registry slot.
#define RT_HEAP_REG_EMPTY NULL
/// @brief Deleted registry slot that preserves a linear-probe chain.
#define RT_HEAP_REG_TOMBSTONE ((void *)(uintptr_t)1)

/// @brief Direct-mapped memo of recently validated payloads (power of two).
/// @details The table is sized by the live-allocation population — millions of
///          entries for large programs — so each validation is a DRAM miss.
///          This 64 KB memo stays cache-resident and short-circuits repeat
///          validations. Entries mirror the table exactly: insertion and a
///          successful probe populate a slot, removal and moves clear it.
#define RT_HEAP_REG_RECENT_SLOTS 8192u
#define RT_HEAP_REG_RECENT_MASK (RT_HEAP_REG_RECENT_SLOTS - 1u)

/// @brief Process-global open-addressed set of exact live payload addresses.
typedef struct {
    void **slots;
    size_t count;
    size_t tombstones;
    size_t capacity;
    int lock;
    void *recent[RT_HEAP_REG_RECENT_SLOTS];
} rt_heap_registry_t;

/// @brief Lazily allocated payload registry and its spinlock state.
static rt_heap_registry_t g_heap_registry_;

/// @brief Hash a pointer to 64 bits using David Stafford's mix13 finalizer.
/// @details Pointers from a real allocator are very correlated in their
///          low bits (alignment puts zeros there) and clustered in
///          their high bits (one heap region). Stafford's mix13 is a
///          well-tested public-domain mixer that scrambles those
///          patterns into a uniform-looking output suitable for
///          open-addressing hash tables.
/// @param p Pointer value to mix; it is not dereferenced.
/// @return Mixed 64-bit value used to select a registry probe sequence.
static uint64_t rt_heap_ptr_hash_(const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

/// @brief Memo slot for a payload: the hash's upper bits, independent of the table index.
/// @param p Candidate payload pointer.
/// @return Index into `rt_heap_registry_t::recent`.
static size_t rt_heap_registry_recent_idx_(const void *p) {
    return (size_t)((rt_heap_ptr_hash_(p) >> 32) & RT_HEAP_REG_RECENT_MASK);
}

/// @brief Acquire the registry's spinlock with yield-on-contention.
/// @details Used to serialize structural mutations to the live-payload
///          set. Insertions and removals are rare relative to lookups,
///          so a simple test-and-set spinlock is faster than a full
///          mutex on typical workloads.
///
/// @note Deliberate safety-vs-throughput tradeoff. retain/release take this
///       global lock even though the refcount step is already a lock-free CAS:
///       the lock makes the *membership probe* safe against a concurrent grow()
///       (which reallocs the slots array) and prevents a final release from
///       freeing the header mid-retain. The payoff is deterministic misuse
///       detection — retain/release on a bogus or freed pointer traps with a
///       clear message instead of corrupting memory. Under heavy multithreaded
///       refcount churn this serializes; the safe ways to relieve it without
///       losing the safety net are (a) a reader-writer lock so retains/releases
///       run concurrently and only alloc/free/grow are exclusive, or (b) sharding
///       the registry into N hash tables keyed by pointer hash. Both are real
///       concurrency redesigns — adopt one only when profiling shows this lock
///       is a measured hot spot.
static void rt_heap_registry_lock_(void) {
    if (__atomic_test_and_set(&g_heap_registry_.lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(&g_heap_registry_.lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release the registry spinlock with release semantics.
static void rt_heap_registry_unlock_(void) {
    __atomic_clear(&g_heap_registry_.lock, __ATOMIC_RELEASE);
}

/// @brief Test whether a slot holds a real payload (vs EMPTY or TOMBSTONE sentinel).
/// @details Open-addressing hash tables use three slot states: empty
///          (never used), tombstone (was used, since deleted), and
///          live (a real value). Lookups must walk past tombstones,
///          insertions can reuse them; this helper distinguishes the
///          live case for those callers.
/// @param slot Registry slot value to classify.
/// @return 1 for a real payload address, otherwise 0.
static int rt_heap_registry_slot_is_live_(void *slot) {
    return slot != RT_HEAP_REG_EMPTY && slot != RT_HEAP_REG_TOMBSTONE;
}

/// @brief Reallocate the registry to at least `min_capacity` slots and rehash existing entries.
/// @details Doubles capacity (or jumps to `min_capacity` if larger),
///          allocates a fresh power-of-two-sized table, and reinserts
///          every live entry from the old table — this rebuilds the
///          probe sequences against the new mask and drops all
///          tombstones along the way. Returns 0 on allocation
///          failure (caller leaves the registry untouched).
/// @param min_capacity Minimum power-of-two-compatible capacity requested.
/// @return 1 after installing the replacement table, otherwise 0.
static int rt_heap_registry_grow_locked_(size_t min_capacity) {
    size_t new_capacity = 256;
    if (g_heap_registry_.capacity) {
        if (g_heap_registry_.capacity > SIZE_MAX / 2)
            return 0;
        new_capacity = g_heap_registry_.capacity * 2;
    }
    while (new_capacity < min_capacity) {
        if (new_capacity > SIZE_MAX / 2)
            return 0;
        new_capacity *= 2;
    }

    void **new_slots = (void **)calloc(new_capacity, sizeof(void *));
    if (!new_slots)
        return 0;

    if (g_heap_registry_.slots) {
        const size_t mask = new_capacity - 1;
        for (size_t i = 0; i < g_heap_registry_.capacity; ++i) {
            void *slot = g_heap_registry_.slots[i];
            if (!rt_heap_registry_slot_is_live_(slot))
                continue;
            size_t idx = (size_t)(rt_heap_ptr_hash_(slot) & mask);
            while (new_slots[idx] != RT_HEAP_REG_EMPTY)
                idx = (idx + 1) & mask;
            new_slots[idx] = slot;
        }
        free(g_heap_registry_.slots);
    }

    g_heap_registry_.slots = new_slots;
    g_heap_registry_.capacity = new_capacity;
    g_heap_registry_.tombstones = 0;
    return 1;
}

/// @brief Grow the registry if load factor (live + tombstones) would exceed 5/8.
/// @details Open-addressing tables degrade past about 5/8 load; the
///          5*cap < 8*(count+tombstones+1) check enforces that.
///          Initial allocation: 256 slots. Subsequent growth: 2×.
///          Includes tombstones in the load calculation because they
///          still consume probe-sequence length.
/// @return 1 when current or newly allocated capacity can accept one entry,
///   otherwise 0.
static int rt_heap_registry_ensure_capacity_locked_(void) {
    if (g_heap_registry_.capacity == 0)
        return rt_heap_registry_grow_locked_(256);
    if (g_heap_registry_.count > SIZE_MAX - g_heap_registry_.tombstones - 1)
        return 0;
    size_t projected = g_heap_registry_.count + g_heap_registry_.tombstones + 1;
    size_t threshold = (g_heap_registry_.capacity / 8) * 5;
    if (projected >= threshold) {
        if (g_heap_registry_.capacity > SIZE_MAX / 2)
            return 0;
        return rt_heap_registry_grow_locked_(g_heap_registry_.capacity * 2);
    }
    return 1;
}

/// @brief Insert `payload` into an already-sized registry table (no growth check).
/// @details Open-addressing linear-probe insertion. Tracks the first
///          tombstone found along the probe path so we can reuse it
///          (saving a slot) only after confirming the key isn't
///          already present further along the chain. Returns 1 if
///          stored or already present, 0 if the table is empty.
/// @param payload Non-NULL exact payload address to insert.
/// @return 1 when present after the call, otherwise 0.
static int rt_heap_registry_insert_existing_locked_(void *payload) {
    if (!payload || !g_heap_registry_.slots || g_heap_registry_.capacity == 0)
        return 0;
    const size_t mask = g_heap_registry_.capacity - 1;
    size_t idx = (size_t)(rt_heap_ptr_hash_(payload) & mask);
    size_t first_tombstone = SIZE_MAX;
    while (1) {
        void *slot = g_heap_registry_.slots[idx];
        if (slot == payload)
            return 1;
        if (slot == RT_HEAP_REG_EMPTY) {
            size_t target = first_tombstone != SIZE_MAX ? first_tombstone : idx;
            if (first_tombstone != SIZE_MAX)
                g_heap_registry_.tombstones--;
            g_heap_registry_.slots[target] = payload;
            g_heap_registry_.count++;
            g_heap_registry_.recent[rt_heap_registry_recent_idx_(payload)] = payload;
            return 1;
        }
        if (slot == RT_HEAP_REG_TOMBSTONE && first_tombstone == SIZE_MAX)
            first_tombstone = idx;
        idx = (idx + 1) & mask;
    }
}

/// @brief Grow-then-insert wrapper for the registry.
/// @details Threads the ensure-capacity step ahead of the actual
///          insert so callers don't have to manage the two-stage
///          dance themselves. NULL payloads are silently ignored
///          (treated as success).
/// @param payload Exact payload address to insert, or NULL for a no-op.
/// @return 1 on success, otherwise 0 when capacity cannot be provided.
static int rt_heap_registry_insert_locked_(void *payload) {
    if (!payload)
        return 1;
    if (!rt_heap_registry_ensure_capacity_locked_())
        return 0;
    return rt_heap_registry_insert_existing_locked_(payload);
}

/// @brief Membership test against the registry (linear probe through tombstones).
/// @details Walks the probe chain from `hash & mask`, skipping over
///          tombstones (which mean "moved on"), stopping at the
///          first EMPTY slot. Returns 0 for unknown payloads.
/// @param payload Exact payload address to locate.
/// @return 1 when the registry contains @p payload, otherwise 0.
static int rt_heap_registry_contains_locked_(void *payload) {
    if (!payload || payload == RT_HEAP_REG_TOMBSTONE || !g_heap_registry_.slots ||
        g_heap_registry_.capacity == 0)
        return 0;
    const size_t recent_idx = rt_heap_registry_recent_idx_(payload);
    if (g_heap_registry_.recent[recent_idx] == payload)
        return 1;
    const size_t mask = g_heap_registry_.capacity - 1;
    size_t idx = (size_t)(rt_heap_ptr_hash_(payload) & mask);
    while (1) {
        void *slot = g_heap_registry_.slots[idx];
        if (slot == payload) {
            g_heap_registry_.recent[recent_idx] = payload;
            return 1;
        }
        if (slot == RT_HEAP_REG_EMPTY)
            return 0;
        idx = (idx + 1) & mask;
    }
}

/// @brief Validate a payload while the heap registry lock is already held.
/// @details This helper is used by operations that need to inspect or retain a
///          heap allocation without allowing a concurrent final release to
///          remove and free the header between validation and the refcount
///          operation.  The caller must hold @ref rt_heap_registry_lock_ for
///          the full duration of the check and any immediate header access.
/// @param payload Candidate payload pointer.
/// @param out_hdr Receives the header when validation succeeds; set to NULL on failure.
/// @return 1 when @p payload is a registered heap allocation with a valid magic tag.
static int rt_heap_try_get_header_locked_(void *payload, rt_heap_hdr_t **out_hdr) {
    if (out_hdr)
        *out_hdr = NULL;
    if (!payload)
        return 0;
    if (!rt_heap_registry_contains_locked_(payload))
        return 0;
    rt_heap_hdr_t *hdr = (rt_heap_hdr_t *)((uint8_t *)payload - sizeof(rt_heap_hdr_t));
    if (!hdr || hdr->magic != RT_MAGIC)
        return 0;
    if (out_hdr)
        *out_hdr = hdr;
    return 1;
}

/// @brief Remove `payload` from the registry by stamping a TOMBSTONE in its slot.
/// @details Open-addressing requires tombstones (rather than just
///          re-empty) so that probe sequences past the deleted slot
///          still terminate at the original "empty" sentinel. The
///          next `grow` invocation drops accumulated tombstones.
/// @param payload Exact address to remove; missing and NULL values are no-ops.
static void rt_heap_registry_remove_locked_(void *payload) {
    if (!payload || !g_heap_registry_.slots || g_heap_registry_.capacity == 0)
        return;
    const size_t recent_idx = rt_heap_registry_recent_idx_(payload);
    if (g_heap_registry_.recent[recent_idx] == payload)
        g_heap_registry_.recent[recent_idx] = RT_HEAP_REG_EMPTY;
    const size_t mask = g_heap_registry_.capacity - 1;
    size_t idx = (size_t)(rt_heap_ptr_hash_(payload) & mask);
    while (1) {
        void *slot = g_heap_registry_.slots[idx];
        if (slot == payload) {
            g_heap_registry_.slots[idx] = RT_HEAP_REG_TOMBSTONE;
            g_heap_registry_.count--;
            g_heap_registry_.tombstones++;
            return;
        }
        if (slot == RT_HEAP_REG_EMPTY)
            return;
        idx = (idx + 1) & mask;
    }
}

/// @brief Atomically move a registry entry from `old_payload` to `new_payload`.
/// @details Used after `realloc` returns a different pointer for an
///          allocation: the registry must forget the old address and
///          remember the new one. Implements as remove-old +
///          insert-new under the same lock so no thread can observe
///          an inconsistent state.
/// @param old_payload Exact currently registered address.
/// @param new_payload Replacement address to insert.
/// @return 1 when the entry is moved or addresses match, otherwise 0.
static int RT_HEAP_UNUSED_PRIVATE rt_heap_registry_move_locked_(void *old_payload,
                                                                void *new_payload) {
    if (old_payload == new_payload)
        return 1;
    if (!g_heap_registry_.slots || g_heap_registry_.capacity == 0)
        return 0;
    const size_t recent_idx = rt_heap_registry_recent_idx_(old_payload);
    if (g_heap_registry_.recent[recent_idx] == old_payload)
        g_heap_registry_.recent[recent_idx] = RT_HEAP_REG_EMPTY;
    const size_t mask = g_heap_registry_.capacity - 1;
    size_t idx = (size_t)(rt_heap_ptr_hash_(old_payload) & mask);
    while (1) {
        void *slot = g_heap_registry_.slots[idx];
        if (slot == old_payload) {
            g_heap_registry_.slots[idx] = RT_HEAP_REG_TOMBSTONE;
            g_heap_registry_.tombstones++;
            g_heap_registry_.count--;
            return rt_heap_registry_insert_existing_locked_(new_payload);
        }
        if (slot == RT_HEAP_REG_EMPTY)
            return 0;
        idx = (idx + 1) & mask;
    }
}

/// @brief Free the registry's slot array at process shutdown.
/// @details Registry entries are borrowed addresses, so process-exit teardown
///   frees only the slot array and resets bookkeeping. It does not attempt to
///   release any payload that remains registered.
static void rt_heap_registry_shutdown_(void) {
    free(g_heap_registry_.slots);
    g_heap_registry_.slots = NULL;
    g_heap_registry_.count = 0;
    g_heap_registry_.tombstones = 0;
    g_heap_registry_.capacity = 0;
    memset(g_heap_registry_.recent, 0, sizeof(g_heap_registry_.recent));
}

/// @brief Sanity-check the invariants stored in a heap header.
/// @details Confirms the presence of the runtime magic tag, ensures the
///          reference count does not use the legacy reserved sentinel value for
///          corruptions, and validates that the heap kind enumerator is one of
///          the recognised values.  This check is deliberately active in
///          release builds because heap entry points form the runtime's safety
///          boundary for generated and host code.
/// @param hdr Header pointer returned by a checked header lookup.
/// @warning Invalid metadata raises a runtime trap and returns only when the
///   active trap dispatcher permits local continuation.
static void rt_heap_validate_header(const rt_heap_hdr_t *hdr) {
    if (!hdr) {
        rt_trap("rt_heap_validate_header: null header");
        return;
    }
    if (hdr->magic != RT_MAGIC) {
        rt_trap("rt_heap_validate_header: invalid heap magic");
        return;
    }
    if (__atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED) == (size_t)-1) {
        rt_trap("rt_heap_validate_header: corrupt refcount");
        return;
    }
    switch ((rt_heap_kind_t)hdr->kind) {
        case RT_HEAP_STRING:
        case RT_HEAP_ARRAY:
        case RT_HEAP_OBJECT:
            break;
        default:
            rt_trap("rt_heap_validate_header: unknown heap kind");
            return;
    }
}

/// @brief Validate a heap header at a public-operation safety boundary.
#define RT_HEAP_VALIDATE(hdr) rt_heap_validate_header(hdr)

/// @brief Returns 1 if `payload` is a tracked rt_heap allocation. Looks up the registry under
/// the heap lock. Used by polymorphic dispatch to distinguish heap-managed pointers from raw.
/// @param payload Exact candidate payload address; NULL and the tombstone sentinel are rejected.
/// @return 1 when currently registered, otherwise 0.
int8_t rt_heap_is_payload(void *payload) {
    if (!payload || payload == RT_HEAP_REG_TOMBSTONE)
        return 0;
    rt_heap_registry_lock_();
    int found = rt_heap_registry_contains_locked_(payload);
    rt_heap_registry_unlock_();
    return found ? 1 : 0;
}

/// @brief Validate `payload` and write its `rt_heap_hdr_t *` to `out_hdr`. Returns 1 on
/// success, 0 if the pointer isn't a tracked heap allocation. Avoids a separate is_payload+
/// header-cast pair in performance-sensitive call sites.
/// @param payload Exact candidate payload address.
/// @param out_hdr Optional destination cleared on failure and filled on success.
/// @return 1 when @p payload is registered with valid magic, otherwise 0.
/// @warning The returned header is borrowed and becomes unsafe when the caller
///   does not otherwise pin the payload against concurrent final release.
int8_t rt_heap_try_get_header(void *payload, rt_heap_hdr_t **out_hdr) {
    if (out_hdr)
        *out_hdr = NULL;
    if (!payload || payload == RT_HEAP_REG_TOMBSTONE)
        return 0;
    rt_heap_registry_lock_();
    rt_heap_hdr_t *hdr = NULL;
    int found = rt_heap_try_get_header_locked_(payload, &hdr);
    rt_heap_registry_unlock_();
    if (!found)
        return 0;
    if (out_hdr)
        *out_hdr = hdr;
    return 1;
}

/// @copydoc rt_heap_get_info
int8_t rt_heap_get_info(const void *payload, rt_heap_info_t *out_info) {
    if (!out_info)
        return 0;
    memset(out_info, 0, sizeof(*out_info));
    if (!payload || payload == RT_HEAP_REG_TOMBSTONE)
        return 0;

    rt_heap_registry_lock_();
    rt_heap_hdr_t *hdr = NULL;
    int found = rt_heap_try_get_header_locked_((void *)(uintptr_t)payload, &hdr);
    if (found && hdr) {
        out_info->kind = hdr->kind;
        out_info->elem_kind = hdr->elem_kind;
        out_info->flags = hdr->flags;
        out_info->refcnt = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
        out_info->len = hdr->len;
        out_info->cap = hdr->cap;
        out_info->alloc_size = hdr->alloc_size;
        out_info->class_id = hdr->class_id;
    }
    rt_heap_registry_unlock_();
    return found && hdr ? 1 : 0;
}

/// @brief Check whether @p ptr and @p bytes are wholly inside one tracked heap payload.
/// @details VM memory instructions commonly dereference object fields, array
///          elements, and string payload bytes through interior pointers derived
///          by pointer arithmetic. The exact-payload registry lookup is too
///          narrow for those addresses, so this helper scans the live registry
///          while holding the heap lock and compares the candidate byte range
///          against each allocation's payload extent. It rejects null starts,
///          unknown pointers, unregistered freed allocations, corrupt headers,
///          and ranges that cross a payload boundary or overflow the allocation.
///          Deferred-zero allocations remain addressable until the matching
///          explicit free so object destructors can read fields between
///          rt_obj_release_check0 and rt_obj_free.
/// @param ptr Candidate range start.
/// @param bytes Number of bytes requested.
/// @return 1 when the range is fully contained by a live payload, otherwise 0.
int8_t rt_heap_contains_range(const void *ptr, size_t bytes) {
    if (!ptr)
        return 0;

    const uintptr_t address = (uintptr_t)ptr;
    int found = 0;

    rt_heap_registry_lock_();
    if (g_heap_registry_.slots && g_heap_registry_.capacity > 0) {
        for (size_t i = 0; i < g_heap_registry_.capacity; ++i) {
            void *payload = g_heap_registry_.slots[i];
            if (!rt_heap_registry_slot_is_live_(payload))
                continue;

            rt_heap_hdr_t *hdr = (rt_heap_hdr_t *)((uint8_t *)payload - sizeof(rt_heap_hdr_t));
            if (!hdr || hdr->magic != RT_MAGIC)
                continue;
            if (hdr->alloc_size < sizeof(rt_heap_hdr_t))
                continue;

            const uintptr_t begin = (uintptr_t)payload;
            if (address < begin)
                continue;

            const size_t payload_bytes = hdr->alloc_size - sizeof(rt_heap_hdr_t);
            const uintptr_t offset = address - begin;
            if (offset > payload_bytes)
                continue;
            if (bytes <= payload_bytes - (size_t)offset) {
                found = 1;
                break;
            }
        }
    }
    rt_heap_registry_unlock_();

    return found ? 1 : 0;
}

/// @brief Return a validated live heap header or raise a runtime trap.
/// @details This is intentionally stricter than the historical direct
///          payload-to-header cast. Public heap operations can receive stale
///          or raw pointers through runtime surfaces, so they must fail with a
///          trap instead of relying on debug-only assertions or dereferencing
///          freed memory.
/// @param payload Exact candidate payload; NULL returns NULL without trapping.
/// @param fn_name Optional operation name included in an invalid-pointer diagnostic.
/// @return Borrowed header for a registered payload, or NULL after a rejected
///   input or returning trap.
static rt_heap_hdr_t *rt_heap_checked_header_(void *payload, const char *fn_name) {
    if (!payload)
        return NULL;
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header(payload, &hdr) || !hdr) {
        char buf[160];
        snprintf(
            buf, sizeof(buf), "%s: invalid or freed heap payload", fn_name ? fn_name : "rt_heap");
        rt_trap(buf);
        return NULL;
    }
    return hdr;
}

/// @brief Allocate a reference-counted heap block.
/// @details Reserves memory for the header plus payload, zero-initialises the
///          structure, and sets the initial reference count to one.  The helper
///          automatically grows the capacity to at least @p init_len elements
///          and guards against integer overflow when computing the payload size.
///          Uses the pool allocator for small allocations (<= 512 bytes) to
///          reduce malloc/free overhead.
/// @param kind Logical category of the allocation (string, array, object).
/// @param elem_kind Element kind metadata stored for debugging/validation.
/// @param elem_size Size in bytes of a single payload element.
/// @param init_len Initial logical length written to the header.
/// @param init_cap Requested capacity measured in elements.
/// @return Pointer to the payload region, or `NULL` when allocation fails or
///         arguments are invalid.
void *rt_heap_alloc(rt_heap_kind_t kind,
                    rt_elem_kind_t elem_kind,
                    size_t elem_size,
                    size_t init_len,
                    size_t init_cap) {
    size_t cap = init_cap;
    if (cap < init_len)
        cap = init_len;
    if (elem_size == 0 && cap > 0)
        return NULL;

    size_t payload_bytes = 0;
    if (cap > 0) {
        if (elem_size && cap > (SIZE_MAX - sizeof(rt_heap_hdr_t)) / elem_size)
            return NULL;
        payload_bytes = cap * elem_size;
    }
    size_t total_bytes = sizeof(rt_heap_hdr_t) + payload_bytes;

    // Use pool allocator for small string allocations only.
    // Arrays cannot use the pool because they grow via realloc(),
    // which is incompatible with pool-allocated memory.
    // Objects are excluded for similar reasons (potential resize/realloc).
    rt_heap_hdr_t *hdr;
    int from_pool = (kind == RT_HEAP_STRING && total_bytes <= RT_POOL_MAX_SIZE);
    if (from_pool) {
        hdr = (rt_heap_hdr_t *)rt_pool_alloc(total_bytes);
    } else {
        hdr = (rt_heap_hdr_t *)rt_heap_alloc_zeroed_(total_bytes);
    }

    if (!hdr)
        return NULL;

    // Pool allocator already zeros memory, but ensure header is initialized
    hdr->magic = RT_MAGIC;
    hdr->kind = (uint16_t)kind;
    hdr->elem_kind = (uint16_t)elem_kind;
    hdr->flags = from_pool ? RT_HEAP_FLAG_POOLED : 0;
    hdr->alloc_size = total_bytes;
    __atomic_store_n(&hdr->refcnt, 1, __ATOMIC_RELAXED);
    hdr->len = init_len;
    hdr->cap = cap;

    /* Register the global shutdown handler once on first allocation.
       Use atomic CAS to ensure exactly-once registration even under
       concurrent first-allocation from multiple threads. */
    if (!__atomic_load_n(&g_shutdown_registered, __ATOMIC_ACQUIRE)) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_shutdown_registered,
                                        &expected,
                                        1,
                                        /*weak=*/0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (rt_register_shutdown_handler_() != 0) {
                __atomic_store_n(&g_shutdown_registered, 0, __ATOMIC_RELEASE);
                rt_trap("rt_heap_alloc: failed to register shutdown handler");
                if (from_pool)
                    rt_pool_free(hdr, total_bytes);
                else
                    rt_free(hdr);
                return NULL;
            }
        }
    }

    void *payload = rt_heap_data(hdr);
    rt_heap_registry_lock_();
    int tracked = rt_heap_registry_insert_locked_(payload);
    rt_heap_registry_unlock_();
    if (!tracked) {
        if (from_pool)
            rt_pool_free(hdr, total_bytes);
        else
            rt_free(hdr);
        return NULL;
    }

    /* Reference-bearing arrays participate in cycle collection from the
       instant they become live. Registration is non-trapping so a tracking-
       table allocation failure can roll the heap allocation back before the
       payload escapes or owns any non-null element. */
    if (kind == RT_HEAP_ARRAY && (elem_kind == RT_ELEM_OBJ || elem_kind == RT_ELEM_BOX) &&
        !rt_gc_track_reference_array(payload)) {
        rt_heap_registry_lock_();
        rt_heap_registry_remove_locked_(payload);
        rt_heap_registry_unlock_();
        if (from_pool)
            rt_pool_free(hdr, total_bytes);
        else
            rt_free(hdr);
        return NULL;
    }

    /* Notify the GC of a new allocation (for auto-trigger). */
    rt_gc_notify_alloc();

    return payload;
}

/// @brief Increment the reference count for a payload.
/// @details Converts the payload to its header, validates the metadata, and
///          then increments the reference count.  Debug builds log the new count
///          when @c ZANNA_RC_DEBUG is enabled, aiding leak investigations.
/// @param payload Shared payload pointer; `NULL` pointers are ignored.
void rt_heap_retain(void *payload) {
    if (!payload)
        return;

    rt_gc_mutator_enter();
    rt_heap_registry_lock_();
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header_locked_(payload, &hdr) || !hdr) {
        rt_heap_registry_unlock_();
        rt_gc_mutator_exit();
        rt_trap("rt_heap_retain: invalid or freed heap payload");
        return;
    }
    RT_HEAP_VALIDATE(hdr);
    size_t old = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
    size_t next = 0;
    for (;;) {
        if (old == 0) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            rt_trap("rt_heap: retain after release");
            return;
        }
        if (old >= RT_HEAP_IMMORTAL_REFCNT) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            return;
        }
        if (old >= RT_HEAP_MAX_MORTAL_REFCNT) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            rt_trap("refcount overflow");
            return;
        }
        next = old + 1;
        if (__atomic_compare_exchange_n(&hdr->refcnt,
                                        &old,
                                        next,
                                        /*weak=*/0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }
    rt_heap_registry_unlock_();
    rt_gc_mutator_exit();
    (void)next;
#ifdef ZANNA_RC_DEBUG
    fprintf(stderr, "rt_heap_retain(%p) => %zu\n", payload, next);
#endif
}

/// @brief Non-trapping retain for code that already has its own recovery/cleanup path.
/// @details Validates and increments under the heap registry lock, preventing a
///   concurrent final release from freeing the header during promotion.
/// @param payload Exact borrowed payload address; NULL is reported as not live.
/// @return 1 when retained, 2 when live immortal and not retained, 0 when not
///         live/managed, and -1 on mortal refcount overflow.
int32_t rt_heap_try_retain_live(void *payload) {
    if (!payload)
        return 0;
    rt_gc_mutator_enter();
    rt_heap_registry_lock_();
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header_locked_(payload, &hdr) || !hdr) {
        rt_heap_registry_unlock_();
        rt_gc_mutator_exit();
        return 0;
    }

    size_t old = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
    for (;;) {
        if (old == 0) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            return 0;
        }
        if (old >= RT_HEAP_IMMORTAL_REFCNT) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            return 2;
        }
        if (old >= RT_HEAP_MAX_MORTAL_REFCNT) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            return -1;
        }
        size_t next = old + 1;
        if (__atomic_compare_exchange_n(&hdr->refcnt,
                                        &old,
                                        next,
                                        /*weak=*/0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            rt_heap_registry_unlock_();
            rt_gc_mutator_exit();
            return 1;
        }
    }
}

/// @brief Release bookkeeping for a heap header with optional deallocation.
/// @details Shared helper that decrements the reference count and, when
///          @p free_when_zero is true, clears and frees the header once the
///          count reaches zero.  Uses the pool allocator for blocks that were
///          originally allocated from the pool.  Debug builds log the resulting
///          count when the retain/release tracing macro is enabled.
/// @param hdr Heap header describing the allocation; may be `NULL`.
/// @param payload Payload pointer associated with @p hdr; used for logging.
/// @param free_when_zero Whether to free storage when the reference count hits
///        zero.
/// @return Updated reference count after the decrement, or zero when the block
///         was deallocated.
static size_t rt_heap_release_impl(rt_heap_hdr_t *hdr, void *payload, int free_when_zero) {
    if (!hdr)
        return 0;
    RT_HEAP_VALIDATE(hdr);

    // A tracked container finalizer still clears its slots/native buffer during
    // cycle reclaim, but the collector owns refcount normalization for edges
    // whose targets are in the same garbage set.
    if (rt_gc_should_suppress_cycle_release(payload)) {
        size_t refs = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
        // A zero count is collector-owned in this phase. Return a non-zero
        // sentinel so release-and-free callers do not reclaim the member early.
        return refs == 0 ? 1 : refs;
    }

    size_t old = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
    size_t next = 0;
    for (;;) {
        if (old == 0) {
            rt_trap("rt_heap: double release (refcount already zero)");
            return 0;
        }
        if (old >= RT_HEAP_IMMORTAL_REFCNT) {
            rt_trap("rt_heap: cannot release immortal refcount");
            return old;
        }
        next = old - 1;
        if (__atomic_compare_exchange_n(&hdr->refcnt,
                                        &old,
                                        next,
                                        /*weak=*/0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }
    assert(old > 0);
#ifdef ZANNA_RC_DEBUG
    fprintf(stderr, "rt_heap_release(%p) => %zu\n", payload, next);
#endif
    if (next == 0 && free_when_zero) {
        // Acquire fence pairs with releasing decrements from other threads.
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        // Clear weak observers while the payload is still registered. This is
        // the common final-release path for specialized arrays and strings,
        // whose release helpers do not pass through the object finalizer path.
        rt_gc_untrack(payload);
        rt_gc_clear_weak_refs(payload);

        // Check if this was a pool allocation
        int from_pool = (hdr->flags & RT_HEAP_FLAG_POOLED) != 0;
        size_t alloc_size = hdr->alloc_size;

        rt_heap_registry_lock_();
        rt_heap_registry_remove_locked_(payload);
        rt_heap_registry_unlock_();

        memset(hdr, 0, sizeof(*hdr));

        if (from_pool) {
            rt_pool_free(hdr, alloc_size);
        } else {
            rt_free(hdr);
        }
        return 0;
    }
    return next;
}

/// @brief Decrement the reference count and free storage when it reaches zero.
/// @details Drops the reference count after validating the header.  When the
///          count hits zero the header and payload are cleared and freed,
///          returning the allocation to the system.  The return value enables
///          callers to observe whether they released the final reference.
/// @param payload Shared payload pointer; `NULL` pointers are ignored.
/// @return Reference count after the decrement, or zero when the block was
///         deallocated.
size_t rt_heap_release(void *payload) {
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_release");
    size_t refs = rt_heap_release_impl(hdr, payload, /*free_when_zero=*/1);
    rt_gc_mutator_exit();
    return refs;
}

/// @brief Decrement the reference count without freeing the payload.
/// @details Mirrors @ref rt_heap_release but preserves the header and payload
///          even when the updated reference count reaches zero.  Callers can use
///          this variant to run custom destructors while the allocation remains
///          valid before finally handing control back to the heap for
///          deallocation.
/// @param payload Shared payload pointer; `NULL` pointers are ignored.
/// @return Reference count after the decrement.
size_t rt_heap_release_deferred(void *payload) {
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_release_deferred");
    size_t refs = rt_heap_release_impl(hdr, payload, /*free_when_zero=*/0);
    rt_gc_mutator_exit();
    return refs;
}

/// @brief Free a heap allocation whose reference count already reached zero.
/// @details Validates the header and, when the reference count is zero, clears
///          and frees the allocation.  Non-zero reference counts leave the
///          payload untouched so callers can safely invoke the helper after
///          custom cleanup logic.  Uses the pool allocator for blocks that were
///          originally allocated from the pool.
/// @param payload Shared payload pointer; `NULL` pointers are ignored.
void rt_heap_free_zero_ref(void *payload) {
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_free_zero_ref");
    if (!hdr) {
        rt_gc_mutator_exit();
        return;
    }
    RT_HEAP_VALIDATE(hdr);
    if (__atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED) != 0) {
        rt_gc_mutator_exit();
        return;
    }

    // Deferred-release users run custom element/finalizer cleanup before this
    // point. Central weak clearing prevents specialized payload kinds from
    // leaving stale target addresses in the zeroing-weak-reference registry.
    rt_gc_untrack(payload);
    rt_gc_clear_weak_refs(payload);

    // Check if this was a pool allocation
    int from_pool = (hdr->flags & RT_HEAP_FLAG_POOLED) != 0;
    size_t alloc_size = hdr->alloc_size;

    rt_heap_registry_lock_();
    rt_heap_registry_remove_locked_(payload);
    rt_heap_registry_unlock_();

    memset(hdr, 0, sizeof(*hdr));

    if (from_pool) {
        rt_pool_free(hdr, alloc_size);
    } else {
        rt_free(hdr);
    }
    rt_gc_mutator_exit();
}

/// @brief Obtain a mutable header pointer for a payload.
/// @details Performs a checked registry lookup before exposing the header. The
///   caller must already own a reference or otherwise prevent concurrent final
///   release for the entire period in which it dereferences the returned pointer.
/// @param payload Payload pointer produced by @ref rt_heap_alloc.
/// @return Mutable header pointer, or `NULL` when @p payload is `NULL`.
/// @warning An invalid non-NULL payload raises a runtime trap.
rt_heap_hdr_t *rt_heap_hdr(void *payload) {
    return rt_heap_checked_header_(payload, "rt_heap_hdr");
}

/// @brief Resize an existing heap allocation, updating len, cap, and the registry.
/// @details Extends or shrinks the allocation by allocating a replacement block,
///          copying the preserved prefix, and then moving the live-payload
///          registry entry under the registry lock. Allocator calls happen
///          outside the lock so unrelated retain/release traffic is not blocked
///          by the system allocator. New elements are zero-filled when
///          @p new_len is larger than the previous logical length.
/// @param payload  Existing payload pointer from rt_heap_alloc.
/// @param elem_size Size in bytes of each logical element.
/// @param new_len  New logical length.
/// @param new_cap  New capacity (clamped up to new_len if smaller).
/// @return Updated payload pointer on success, or NULL on failure.
void *rt_heap_realloc(void *payload, size_t elem_size, size_t new_len, size_t new_cap) {
    if (!payload)
        return NULL;

    rt_gc_mutator_enter();
    size_t cap = new_cap;
    if (cap < new_len)
        cap = new_len;
    if (elem_size == 0 && cap > 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    size_t payload_bytes = 0;
    if (cap > 0) {
        if (cap > (SIZE_MAX - sizeof(rt_heap_hdr_t)) / elem_size) {
            rt_gc_mutator_exit();
            return NULL;
        }
        payload_bytes = cap * elem_size;
    }
    size_t total_bytes = sizeof(rt_heap_hdr_t) + payload_bytes;
    rt_heap_hdr_t *resized = (rt_heap_hdr_t *)rt_heap_alloc_zeroed_(total_bytes);
    if (!resized) {
        rt_gc_mutator_exit();
        return NULL;
    }

    rt_heap_registry_lock_();
    rt_heap_hdr_t *hdr = NULL;
    if (!rt_heap_try_get_header_locked_(payload, &hdr) || !hdr) {
        rt_heap_registry_unlock_();
        rt_free(resized);
        rt_gc_mutator_exit();
        rt_trap("rt_heap_realloc: invalid or freed heap payload");
        return NULL;
    }
    RT_HEAP_VALIDATE(hdr);

    size_t refcnt = __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE);
    if (refcnt != 1) {
        rt_heap_registry_unlock_();
        rt_free(resized);
        rt_gc_mutator_exit();
        rt_trap("rt_heap_realloc: payload must have exactly one owner");
        return NULL;
    }

    size_t old_len = hdr->len;
    size_t old_alloc_size = hdr->alloc_size;
    int from_pool = (hdr->flags & RT_HEAP_FLAG_POOLED) != 0;
    size_t copy_bytes = old_alloc_size < total_bytes ? old_alloc_size : total_bytes;
    memcpy(resized, hdr, copy_bytes);
    resized->flags &= ~RT_HEAP_FLAG_POOLED;
    resized->len = new_len;
    resized->cap = cap;
    resized->alloc_size = total_bytes;

    void *new_payload = rt_heap_data(resized);
    if (new_len > old_len && elem_size > 0) {
        memset((uint8_t *)new_payload + old_len * elem_size, 0, (new_len - old_len) * elem_size);
    }

    if (!rt_heap_registry_move_locked_(payload, new_payload)) {
        rt_heap_registry_unlock_();
        rt_free(resized);
        rt_abort("rt_heap_realloc: registry update failed");
        return NULL;
    }
    rt_heap_registry_unlock_();

    memset(hdr, 0, sizeof(*hdr));
    if (from_pool)
        rt_pool_free(hdr, old_alloc_size);
    else
        rt_free(hdr);

    rt_gc_relocate_payload(payload, new_payload);
    rt_gc_mutator_exit();
    return new_payload;
}

/// @brief Convert a header pointer back to its payload address.
/// @details Validates the header before returning the byte immediately after
///          the metadata structure.  The returned pointer aliases the
///          allocation owned by the header and should not be freed directly.
/// @param h Header describing the allocation.
/// @return Pointer to the payload region, or `NULL` when @p h is `NULL`.
void *rt_heap_data(rt_heap_hdr_t *h) {
    if (!h)
        return NULL;
    RT_HEAP_VALIDATE(h);
    return (void *)((uint8_t *)h + sizeof(rt_heap_hdr_t));
}

/// @brief Read the logical length stored alongside a payload.
/// @details Provides a safe accessor that tolerates `NULL` payloads by
///          returning zero, mirroring the behaviour expected by callers in the
///          runtime.
/// @param payload Payload pointer or `NULL`.
/// @return Logical element count tracked in the header.
size_t rt_heap_len(void *payload) {
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_len");
    if (!hdr)
        return 0;
    return hdr->len;
}

/// @brief Read the capacity stored alongside a payload.
/// @details Converts the payload to its header and returns the recorded number
///          of elements for which space is reserved.
/// @param payload Payload pointer or `NULL`.
/// @return Capacity value stored in the header.
size_t rt_heap_cap(void *payload) {
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_cap");
    if (!hdr)
        return 0;
    return hdr->cap;
}

/// @brief Update the logical length associated with a payload.
/// @details Allows resizing operations to publish their new element count
///          without touching the allocation metadata directly.  `NULL` payloads
///          are ignored so callers can operate on optional handles without
///          defensive conditionals.
/// @param payload Payload pointer whose header should be updated.
/// @param new_len New logical length to store.
void rt_heap_set_len(void *payload, size_t new_len) {
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_set_len");
    if (!hdr) {
        rt_gc_mutator_exit();
        return;
    }
    if (new_len > hdr->cap) {
        rt_gc_mutator_exit();
        rt_trap("rt_heap_set_len: length exceeds capacity");
        return;
    }
    hdr->len = new_len;
    rt_gc_mutator_exit();
}

/// @brief Atomically OR a value into a 32-bit field and return the previous value.
/// @param ptr Pointer to the 32-bit field.
/// @param value Value to OR into the field.
/// @return The previous value of the field before the OR operation.
static inline uint32_t atomic_fetch_or_u32(volatile uint32_t *ptr, uint32_t value) {
#if RT_COMPILER_MSVC
    return (uint32_t)_InterlockedOr((volatile long *)ptr, (long)value);
#else
    return __atomic_fetch_or(ptr, value, __ATOMIC_ACQ_REL);
#endif
}

/// @brief Atomically mark the heap allocation as disposed (logical free). Returns 1 if this
/// call performed the mark, 0 if it was already disposed (idempotent). Useful for one-shot
/// finalizer guards in collections that own heterogeneous resources.
/// @param payload Exact object payload to mark; NULL returns zero.
/// @return 1 for the caller that changes the bit from clear to set, otherwise 0.
/// @warning Invalid non-NULL payloads raise a runtime trap.
int32_t rt_heap_mark_disposed(void *payload) {
    rt_heap_hdr_t *hdr = rt_heap_checked_header_(payload, "rt_heap_mark_disposed");
    if (!hdr)
        return 0;
    RT_HEAP_VALIDATE(hdr);
    const uint32_t DISPOSED = 0x1u;

    // Atomically set DISPOSED flag and return previous flags (RACE-005 fix)
    // This ensures exactly one caller sees the transition from !DISPOSED to DISPOSED
    uint32_t old_flags = atomic_fetch_or_u32(&hdr->flags, DISPOSED);

    return (old_flags & DISPOSED) ? 0 : 1;
}
