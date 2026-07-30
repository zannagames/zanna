//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_pool.c
// Purpose: Implements a slab allocator for the Zanna runtime. Reduces
//          malloc/free overhead by pooling fixed-size allocations into
//          four size classes (64, 128, 256, 512 bytes) and reusing freed
//          blocks via per-size-class freelists with out-of-band metadata.
//
// Key invariants:
//   - Each size class maintains a singly-linked list of slabs; each slab holds
//     BLOCKS_PER_SLAB (64) fixed-size blocks.
//   - Every returned block has an aligned private header containing its owning
//     slab, freelist link, allocation state, and validation tag. Caller writes
//     therefore never alias allocator metadata.
//   - Freelist operations are spinlock-protected per size class. Allocation
//     state transitions use compare/exchange so a double free is rejected
//     before it can duplicate a freelist node or underflow statistics.
//   - Slab list insertion uses atomic CAS; no lifecycle mutex is held during allocation.
//   - A lock-free even/odd lifecycle epoch admits ordinary operations. Shutdown
//     flips the epoch odd, waits for already-admitted operations, and restores
//     the next even epoch after reclaiming quiescent classes.
//   - Allocation requests larger than the largest size class (512 bytes) fall
//     through to malloc and are not initialized by this module.
//   - Pooled blocks are zeroed across their full size class immediately before
//     allocation, so recycled contents are not exposed and free remains cheap.
//
// Ownership/Lifetime:
//   - Slabs are reclaimed only when their size class has no outstanding blocks.
//     Shutdown defers a live class rather than invalidating caller-owned memory.
//   - Freed blocks are returned to the per-class freelist and owned by the pool
//     until the next allocation of the same class.
//   - Every allocation must be returned with the original requested size;
//     crossing the pooled/system-allocation boundary violates the API contract.
//
// Links: src/runtime/core/rt_pool.h (public API),
//        src/runtime/core/rt_heap.c (heap layer above pool),
//        src/runtime/core/rt_memory.c (low-level allocation primitives)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Concurrent fixed-size slab allocator with a system-allocation fallback.

#include "rt_pool.h"
#include "rt_platform.h"
#include "rt_trap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if !RT_PLATFORM_WINDOWS
#include <sched.h>
#endif

/// @brief Number of blocks per slab in each size class.
/// @details Tuned for balance between memory efficiency and allocation frequency.
#define BLOCKS_PER_SLAB 64

/// @brief Caller-visible payload capacity for each size class.
static const size_t kClassSizes[RT_POOL_COUNT] = {64, 128, 256, 512};

/// @brief Forward declaration for the owner pointer stored in every block header.
struct rt_pool_slab;

#if RT_COMPILER_MSVC
/// @brief MSVC C fallback for the fundamental alignment represented by `max_align_t`.
/// @details The MSVC C headers do not define C11 `max_align_t`. Its fundamental
///          scalar types have no stricter alignment than `long double`, a
///          pointer, or `long long`, so this union supplies the equivalent
///          alignment without relying on a nonstandard CRT typedef.
typedef union rt_pool_max_align {
    /// Supplies `long double` alignment.
    long double floating;
    /// Supplies pointer alignment.
    void *pointer;
    /// Supplies `long long` alignment.
    long long integer;
} rt_pool_max_align_t;
#else
/// @brief Fundamental maximum-alignment type supplied by non-MSVC C libraries.
typedef max_align_t rt_pool_max_align_t;
#endif

/// @brief Private, maximally aligned metadata stored immediately before a pool payload.
/// @details The union's maximum-alignment member gives every returned payload the
///          alignment promised by `malloc`. Metadata remains outside caller-
///          writable bytes, so a freelist pop can inspect `next` without racing
///          ordinary writes to an allocated payload. `state` is changed only
///          with atomic compare/exchange: zero means free and one means owned.
typedef union rt_pool_block {
    struct {
        union rt_pool_block *next; ///< Next free block in this size class.
        struct rt_pool_slab *slab; ///< Stable owner used for O(1) free routing.
        uint64_t magic;            ///< Validation tag for pool-owned payloads.
        size_t state;              ///< Atomic 0=free, 1=allocated state.
    } meta;

    rt_pool_max_align_t alignment; ///< Forces fundamental maximum alignment.
} rt_pool_block_t;

/// @brief Validation word embedded in every live private block header.
#define RT_POOL_BLOCK_MAGIC UINT64_C(0x5A414E4E41504F4F)

/// @brief Atomically load a block's next pointer (acquire).
/// @param block Non-null private block header whose freelist link is read.
/// @return The link value observed with acquire ordering.
static inline rt_pool_block_t *atomic_load_next(rt_pool_block_t *block) {
#if RT_COMPILER_MSVC
    rt_pool_block_t *volatile *slot = (rt_pool_block_t *volatile *)&block->meta.next;
    rt_pool_block_t *val = *slot;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    return val;
#else
    return __atomic_load_n(&block->meta.next, __ATOMIC_ACQUIRE);
#endif
}

/// @brief Atomically store a block's next pointer (release).
/// @param block Non-null private block header whose freelist link is replaced.
/// @param next New successor, or `NULL` for the end of a chain.
static inline void atomic_store_next(rt_pool_block_t *block, rt_pool_block_t *next) {
#if RT_COMPILER_MSVC
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    *(rt_pool_block_t *volatile *)&block->meta.next = next;
#else
    __atomic_store_n(&block->meta.next, next, __ATOMIC_RELEASE);
#endif
}

/// @brief Slab metadata - tracks a single large allocation subdivided into blocks.
typedef struct rt_pool_slab {
    struct rt_pool_slab *next; ///< Next slab in the size class
    size_t block_size;         ///< Caller-visible payload size for each block.
    size_t block_stride;       ///< Private header plus payload bytes.
    size_t block_count;        ///< Number of blocks in this slab
    rt_pool_class_t class_idx; ///< Owning size class for O(1) free routing.
    char *data;                ///< Start of the first private block header.
} rt_pool_slab_t;

//===----------------------------------------------------------------------===//
// Tagged Pointer Support (ABA Prevention)
//===----------------------------------------------------------------------===//
//
// Tagged pointers use the upper 16 bits for a version counter and the lower
// 48 bits for the actual pointer. This works because:
// - x86-64 uses only 48 bits for user-space virtual addresses
// - Pool blocks are aligned to at least 8 bytes
// - The version counter detects ABA scenarios where a pointer is recycled
//
// The original lock-free freelist used tagged pointers, but a pop operation has
// to read `head->next` before it owns `head`. Another thread can pop the same
// block, return it to the caller, and let user code overwrite the intrusive link
// before the stale pop's CAS fails. The ABA tag prevents the stale CAS from
// succeeding, but it cannot make the speculative `next` read safe. Keep the
// tagged-pointer helpers available for experimentation, but use the spinlock
// freelist in production until the allocator has a proper safe-reclamation
// scheme or out-of-band block metadata.
//
//===----------------------------------------------------------------------===//

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

#if defined(__arm64e__) || defined(_M_ARM64EC) || __has_feature(ptrauth_calls)
#define RT_POOL_PAC_SAFE 1
#else
#define RT_POOL_PAC_SAFE 0
#endif

/// @brief Use the lock-protected intrusive freelist implementation.
/// @details This is intentionally enabled on all supported targets. The former
///          lock-free path can race with normal caller writes because it must
///          dereference a candidate head block before owning it. The spinlock
///          only covers freelist pointer updates and avoids that undefined
///          behavior without changing allocation semantics.
#define RT_POOL_USE_LOCKED_FREELIST 1

/// @brief Return non-zero when @p ptr can be represented by the 48-bit tagged-pointer format.
/// @details The fast freelist format stores a 16-bit ABA counter in the high bits
///          and the pointer in the low 48 bits.  Systems with 57-bit virtual
///          addresses can hand out pointers that do not fit.  This guard turns
///          such a platform mismatch into an immediate allocator failure rather
///          than silently truncating a live pointer.
/// @param ptr Pointer candidate to encode in the tagged freelist word.
/// @return Non-zero when the pointer's high bits are clear and safe to pack.
#if !RT_POOL_USE_LOCKED_FREELIST && !RT_POOL_PAC_SAFE
/// @brief Test whether a pointer fits the experimental 48-bit tagged format.
/// @param ptr Pointer candidate to encode in the tagged freelist word.
/// @return Nonzero when the pointer's high bits are clear and safe to pack.
static inline int ptr_fits_tagged_ptr(void *ptr) {
    return (((uintptr_t)ptr) & ~(uintptr_t)0x0000FFFFFFFFFFFFULL) == 0;
}

/// @brief Pack a pointer and version into a tagged pointer.
/// @details Aborts if a non-null pointer does not fit the low 48 bits.
/// @param ptr Pointer stored in the low 48 bits.
/// @param version ABA version stored in the high 16 bits.
/// @return Encoded pointer/version word.
static inline uint64_t pack_tagged_ptr(void *ptr, uint16_t version) {
    if (ptr && !ptr_fits_tagged_ptr(ptr))
        abort();
    return ((uint64_t)version << 48) | ((uint64_t)(uintptr_t)ptr & 0x0000FFFFFFFFFFFFULL);
}

/// @brief Extract the pointer from a tagged pointer.
/// @param tagged Encoded pointer/version word.
/// @return Pointer reconstructed from the low 48 bits.
static inline void *unpack_ptr(uint64_t tagged) {
    return (void *)(uintptr_t)(tagged & 0x0000FFFFFFFFFFFFULL);
}

/// @brief Extract the version from a tagged pointer.
/// @param tagged Encoded pointer/version word.
/// @return High 16-bit ABA version.
static inline uint16_t unpack_version(uint64_t tagged) {
    return (uint16_t)(tagged >> 48);
}
#endif

#if !RT_POOL_USE_LOCKED_FREELIST && !RT_POOL_PAC_SAFE
/// @brief Atomic compare-exchange for 64-bit values.
/// @param ptr Address of the encoded word to update.
/// @param expected In/out expected value; receives the observed value when the
///                 exchange fails.
/// @param desired Replacement written when @p expected matches.
/// @return Non-zero when the exchange succeeds; otherwise zero.
static inline int atomic_cas_u64(volatile uint64_t *ptr, uint64_t *expected, uint64_t desired) {
#if RT_COMPILER_MSVC
    uint64_t old = _InterlockedCompareExchange64(
        (volatile long long *)ptr, (long long)desired, (long long)*expected);
    if (old == *expected)
        return 1;
    *expected = old;
    return 0;
#else
    return __atomic_compare_exchange_n(
        ptr, expected, desired, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

/// @brief Atomic load for 64-bit values.
/// @details ARM64 Windows uses `__dmb` for an acquire CPU barrier rather than
///          the compiler-only `_ReadWriteBarrier`; other targets use their
///          appropriate atomic or ordering primitive.
/// @param ptr Address of the encoded word to read.
/// @return Value observed with acquire ordering.
static inline uint64_t atomic_load_u64(volatile uint64_t *ptr) {
#if RT_COMPILER_MSVC
#if defined(_M_ARM64)
    uint64_t value = *ptr;
    __dmb(_ARM64_BARRIER_ISH); /* acquire: CPU load fence */
    return value;
#elif defined(_M_X64)
    uint64_t value = *ptr;
    _ReadWriteBarrier(); /* x86-64 TSO: compiler fence suffices */
    return value;
#else
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)ptr, 0, 0);
#endif
#else
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

/// @brief Atomic store for 64-bit values.
/// @details ARM64 Windows uses `__dmb` around the store; other targets use
///          their appropriate atomic or ordering primitive.
/// @param ptr Address of the encoded word to replace.
/// @param value Value published with release ordering.
static inline void atomic_store_u64(volatile uint64_t *ptr, uint64_t value) {
#if RT_COMPILER_MSVC
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH); /* release: CPU store fence */
    *ptr = value;
    __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_X64)
    _ReadWriteBarrier();
    *ptr = value;
    _ReadWriteBarrier();
#else
    _InterlockedExchange64((volatile long long *)ptr, (long long)value);
#endif
#else
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
#endif
}
#endif

/// @brief Per-size-class pool state.
/// CONC-005 fix: volatile qualifiers removed — all accesses use __atomic_*
/// builtins or CAS operations which provide their own compiler+CPU barriers.
typedef struct rt_pool_state {
#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
    rt_pool_block_t *freelist_head; ///< Raw-pointer freelist head.
    int freelist_lock;              ///< Spinlock protecting freelist operations.
#else
    uint64_t freelist_tagged; ///< Lock-free freelist head (tagged pointer, via atomic CAS)
#endif
    rt_pool_slab_t *slabs; ///< List of slabs (via atomic CAS for thread-safe insertion)
    size_t allocated;      ///< Count of blocks currently allocated (via __atomic_*)
    size_t free_count;     ///< Count of blocks on freelist (via __atomic_*)
} rt_pool_state_t;

/// @brief Global pool state for each size class.
static rt_pool_state_t g_pools[RT_POOL_COUNT];

/// @brief Even/odd lifecycle generation used to coordinate shutdown without a hot-path mutex.
/// @details Even values admit allocation/free operations. A shutdown caller
///          changes one even value to the following odd value, waits for the
///          active count to drain, then publishes the next even value. An
///          operation that races the transition rolls back its admission and
///          retries, so it can never touch a slab while reclamation owns it.
static size_t g_pool_lifecycle_epoch;

/// @brief Number of operations admitted under the current even lifecycle epoch.
static size_t g_pool_active_ops;

/// @brief Yield execution while another thread owns a rare lifecycle transition.
/// @details Uses `SwitchToThread` on Windows and `sched_yield` elsewhere.
static void rt_pool_yield_(void) {
#if RT_PLATFORM_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}

/// @brief Enter a pool operation under a stable even lifecycle epoch.
/// @details Admission is optimistic: read the even epoch, increment the active
///          counter, then verify that shutdown did not change the epoch in the
///          middle. A mismatch rolls the counter back and retries. The counter
///          increment uses compare/exchange so pathological overflow aborts
///          instead of making shutdown incorrectly observe quiescence. The
///          caller must pair every successful return with
///          @ref rt_pool_end_op_.
static void rt_pool_begin_op_(void) {
    for (;;) {
        size_t epoch = rt_atomic_load_size(&g_pool_lifecycle_epoch, __ATOMIC_ACQUIRE);
        if ((epoch & 1U) != 0U) {
            rt_pool_yield_();
            continue;
        }

        size_t active = rt_atomic_load_size(&g_pool_active_ops, __ATOMIC_RELAXED);
        for (;;) {
            if (active == SIZE_MAX)
                rt_abort("rt_pool: active-operation counter overflow");
            size_t expected = active;
            if (rt_atomic_compare_exchange_size(
                    &g_pool_active_ops, &expected, active + 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                break;
            active = expected;
        }

        if (rt_atomic_load_size(&g_pool_lifecycle_epoch, __ATOMIC_ACQUIRE) == epoch)
            return;

        size_t previous = rt_atomic_fetch_sub_size(&g_pool_active_ops, 1, __ATOMIC_RELEASE);
        if (previous == 0)
            rt_abort("rt_pool: active-operation counter underflow");
    }
}

/// @brief Leave one operation admitted by @ref rt_pool_begin_op_.
/// @details The release decrement publishes all slab/freelist changes before a
///          shutdown waiter can observe a zero active count. Counter underflow
///          is an internal invariant violation and aborts.
static void rt_pool_end_op_(void) {
    size_t previous = rt_atomic_fetch_sub_size(&g_pool_active_ops, 1, __ATOMIC_RELEASE);
    if (previous == 0)
        rt_abort("rt_pool: active-operation counter underflow");
}

#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
/// @brief Acquire a freelist spinlock with yield-on-contention.
/// @details Freelist operations are deliberately short: read or write one
///          intrusive `next` pointer and update the head. The lock keeps
///          candidate blocks owned by the freelist while their `next` pointers
///          are inspected, which avoids racing with caller writes after a pop.
/// @param lock Address of the per-class lock word.
static void rt_pool_lock_(int *lock) {
    if (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release a freelist spinlock with release semantics.
/// @param lock Address of the held per-class lock word.
static void rt_pool_unlock_(int *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}
#endif

/// @brief Determine the size class for a given allocation size.
/// @param size Requested allocation size.
/// @return The smallest fitting class, with zero mapping to @ref RT_POOL_64,
///         or @ref RT_POOL_COUNT when @p size exceeds 512.
static rt_pool_class_t size_to_class(size_t size) {
    if (size <= 64)
        return RT_POOL_64;
    if (size <= 128)
        return RT_POOL_128;
    if (size <= 256)
        return RT_POOL_256;
    if (size <= 512)
        return RT_POOL_512;
    return RT_POOL_COUNT; // Too large for pooling
}

/// @brief Allocate a new slab for the given size class.
/// @details Validates all header, stride, alignment-padding, and total-size
///          arithmetic before one `malloc` for the slab metadata and 64 inline
///          block-header/payload pairs. Every block starts free with a stable
///          owner, magic tag, and aligned address. The slab is not linked into
///          global state by this helper.
/// @param class_idx Valid size class index.
/// @return Initialized unlinked slab, or `NULL` on arithmetic, address-format,
///         or allocation failure.
static rt_pool_slab_t *allocate_slab(rt_pool_class_t class_idx) {
    size_t block_size = kClassSizes[class_idx];
    if (block_size > SIZE_MAX - sizeof(rt_pool_block_t))
        return NULL;
    size_t block_stride = sizeof(rt_pool_block_t) + block_size;
    if (block_stride > SIZE_MAX / BLOCKS_PER_SLAB)
        return NULL;
    size_t data_size = block_stride * BLOCKS_PER_SLAB;
#if RT_COMPILER_MSVC
    const size_t block_alignment = __alignof(rt_pool_block_t);
#else
    const size_t block_alignment = _Alignof(rt_pool_block_t);
#endif
    if (block_alignment == 0 || (block_alignment & (block_alignment - 1)) != 0 ||
        sizeof(rt_pool_slab_t) > SIZE_MAX - (block_alignment - 1))
        return NULL;
    size_t metadata_bytes = sizeof(rt_pool_slab_t) + block_alignment - 1;
    if (data_size > SIZE_MAX - metadata_bytes)
        return NULL;

    // Allocate slab metadata and all private block headers/payloads together.
    rt_pool_slab_t *slab = (rt_pool_slab_t *)malloc(metadata_bytes + data_size);
    if (!slab)
        return NULL;

    slab->next = NULL;
    slab->block_size = block_size;
    slab->block_stride = block_stride;
    slab->block_count = BLOCKS_PER_SLAB;
    slab->class_idx = class_idx;
    uintptr_t unaligned_data = (uintptr_t)(void *)(slab + 1);
    uintptr_t aligned_data =
        (unaligned_data + block_alignment - 1) & ~(uintptr_t)(block_alignment - 1);
    slab->data = (char *)(void *)aligned_data;
#if !RT_POOL_USE_LOCKED_FREELIST && !RT_POOL_PAC_SAFE
    if (!ptr_fits_tagged_ptr(slab->data) ||
        !ptr_fits_tagged_ptr(slab->data + (BLOCKS_PER_SLAB - 1) * block_stride)) {
        free(slab);
        return NULL;
    }
#endif

    for (size_t i = 0; i < slab->block_count; ++i) {
        rt_pool_block_t *block = (rt_pool_block_t *)(void *)(slab->data + i * slab->block_stride);
        block->meta.next = NULL;
        block->meta.slab = slab;
        block->meta.magic = RT_POOL_BLOCK_MAGIC;
        rt_atomic_store_size(&block->meta.state, 0, __ATOMIC_RELAXED);
    }

    return slab;
}

/// @brief Convert one private block header to its caller-visible payload.
/// @param block Initialized private block header, or `NULL`.
/// @return Maximally aligned payload immediately following @p block, or `NULL`.
static void *rt_pool_block_payload_(rt_pool_block_t *block) {
    return block ? (void *)(block + 1) : NULL;
}

/// @brief Recover a private block header from a caller-visible small-block payload.
/// @details Every request at or below @ref RT_POOL_MAX_SIZE waits through a
///          concurrent shutdown and therefore always comes from a slab. The
///          original requested size remains part of the public free contract;
///          this helper is never used for the `malloc` large-allocation path.
/// @param payload Pointer returned for a small allocation, or `NULL`.
/// @return Private metadata immediately preceding @p payload, or `NULL`.
static rt_pool_block_t *rt_pool_payload_block_(void *payload) {
    return payload ? ((rt_pool_block_t *)payload - 1) : NULL;
}

/// @brief Validate a private block and report its owning size class.
/// @details Checks the immutable tag, owner pointer, class range, payload size,
///          and exact address derived from the slab's stride. These checks turn
///          stale or interior frees into recoverable runtime traps rather than
///          corrupting a freelist when the recovered header remains readable.
///          The caller must hold lifecycle admission so a genuine referenced
///          slab cannot be reclaimed concurrently.
/// @param block Candidate readable private header recovered from a small payload.
/// @param out_class Receives the owning class on success and
///        @ref RT_POOL_COUNT on failure.
/// @return `NULL` on success, otherwise a stable diagnostic string. The caller
///         reports the diagnostic only after leaving lifecycle admission.
static const char *rt_pool_validate_block_(rt_pool_block_t *block, rt_pool_class_t *out_class) {
    if (out_class)
        *out_class = RT_POOL_COUNT;
    if (!block || block->meta.magic != RT_POOL_BLOCK_MAGIC || !block->meta.slab) {
        return "rt_pool_free: invalid pool block";
    }
    rt_pool_slab_t *slab = block->meta.slab;
    if (slab->class_idx >= RT_POOL_COUNT || slab->block_size != kClassSizes[slab->class_idx] ||
        slab->block_stride != sizeof(rt_pool_block_t) + slab->block_size) {
        return "rt_pool_free: corrupt pool block owner";
    }
    uintptr_t begin = (uintptr_t)slab->data;
    uintptr_t address = (uintptr_t)block;
    if (address < begin)
        return "rt_pool_free: invalid pool block address";
    uintptr_t offset = address - begin;
    if (offset % slab->block_stride != 0 || offset / slab->block_stride >= slab->block_count)
        return "rt_pool_free: invalid pool block address";
    if (out_class)
        *out_class = slab->class_idx;
    return NULL;
}

/// @brief Pop a block from the freelist.
/// @details Removes one head under the configured synchronization path,
///          atomically transitions its state from free to allocated, and
///          decrements the relaxed free counter. An impossible state transition
///          aborts as allocator corruption.
/// @param pool Non-null pool state for the size class.
/// @return Allocated-state block pointer, or `NULL` if the freelist is empty.
/// @note Uses a short spinlock so the head block remains owned by the freelist
///       while its intrusive `next` pointer is read.
static rt_pool_block_t *pop_from_freelist(rt_pool_state_t *pool) {
#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
    rt_pool_lock_(&pool->freelist_lock);
    rt_pool_block_t *head = pool->freelist_head;
    if (head) {
        size_t expected_state = 0;
        if (!rt_atomic_compare_exchange_size(
                &head->meta.state, &expected_state, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            rt_pool_unlock_(&pool->freelist_lock);
            rt_abort("rt_pool: freelist contains an allocated block");
        }
        pool->freelist_head = atomic_load_next(head);
        size_t free_count = rt_atomic_load_size(&pool->free_count, __ATOMIC_RELAXED);
        if (free_count == 0) {
            rt_pool_unlock_(&pool->freelist_lock);
            rt_abort("rt_pool: free-block counter underflow");
        }
        rt_atomic_fetch_sub_size(&pool->free_count, 1, __ATOMIC_RELAXED);
    }
    rt_pool_unlock_(&pool->freelist_lock);
    return head;
#else
    uint64_t old_tagged = atomic_load_u64(&pool->freelist_tagged);

    while (1) {
        rt_pool_block_t *head = (rt_pool_block_t *)unpack_ptr(old_tagged);
        if (!head)
            return NULL;

        uint16_t old_version = unpack_version(old_tagged);
        rt_pool_block_t *next = atomic_load_next(head);

        // Pack the new tagged pointer with incremented version
        uint64_t new_tagged = pack_tagged_ptr(next, (uint16_t)(old_version + 1));

        if (atomic_cas_u64(&pool->freelist_tagged, &old_tagged, new_tagged)) {
            size_t expected_state = 0;
            if (!rt_atomic_compare_exchange_size(
                    &head->meta.state, &expected_state, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                rt_abort("rt_pool: freelist contains an allocated block");
#if RT_COMPILER_MSVC
            rt_atomic_fetch_sub_size(&pool->free_count, 1, __ATOMIC_RELAXED);
#else
            __atomic_fetch_sub(&pool->free_count, 1, __ATOMIC_RELAXED);
#endif
            return head;
        }
        // CAS failed, old_tagged was updated with the current value - retry
    }
#endif
}

/// @brief Transition an allocated block to free and push it onto the freelist.
/// @details Claims the release with an allocated-to-free state CAS before
///          linking the block at the class head and incrementing the relaxed
///          free counter. A second release therefore cannot insert the same
///          node twice.
/// @param pool Non-null pool state for the size class.
/// @param block Non-null allocated block to return to the freelist.
/// @return `NULL` on success; otherwise a stable diagnostic that the caller must
///         report after leaving lifecycle admission.
static const char *push_to_freelist(rt_pool_state_t *pool, rt_pool_block_t *block) {
    size_t expected_state = 1;
    if (!rt_atomic_compare_exchange_size(
            &block->meta.state, &expected_state, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return expected_state == 0 ? "rt_pool_free: double free"
                                   : "rt_pool_free: corrupt block state";
    }
#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
    rt_pool_lock_(&pool->freelist_lock);
    atomic_store_next(block, pool->freelist_head);
    pool->freelist_head = block;
    size_t free_count = rt_atomic_load_size(&pool->free_count, __ATOMIC_RELAXED);
    if (free_count == SIZE_MAX) {
        rt_pool_unlock_(&pool->freelist_lock);
        rt_abort("rt_pool: free-block counter overflow");
    }
    rt_atomic_fetch_add_size(&pool->free_count, 1, __ATOMIC_RELAXED);
    rt_pool_unlock_(&pool->freelist_lock);
#else
    uint64_t old_tagged = atomic_load_u64(&pool->freelist_tagged);
    uint64_t new_tagged;
    do {
        rt_pool_block_t *old_head = (rt_pool_block_t *)unpack_ptr(old_tagged);
        uint16_t old_version = unpack_version(old_tagged);
        atomic_store_next(block, old_head);
        new_tagged = pack_tagged_ptr(block, (uint16_t)(old_version + 1));
    } while (!atomic_cas_u64(&pool->freelist_tagged, &old_tagged, new_tagged));
#endif

#if !RT_POOL_USE_LOCKED_FREELIST && !RT_POOL_PAC_SAFE
    rt_atomic_fetch_add_size(&pool->free_count, 1, __ATOMIC_RELAXED);
#endif
    return NULL;
}

/// @brief Allocate from a zeroing size-class pool or fall back to `malloc`.
/// @details Selects the smallest size class that fits @p size, pops a block from
///          the class freelist if available, or allocates a fresh slab of 64 blocks
///          and returns the first one while pushing the rest for future use.
///          A zero request is treated as one byte. Each pooled result is
///          maximally aligned and zeroed across its complete class capacity.
///          Requests above 512 bytes bypass slab state and return ordinary,
///          uninitialized `malloc` storage.
/// @param size Number of bytes requested.
/// @return Pooled zeroed storage or uninitialized system storage according to
///         size, or `NULL` on allocation failure.
void *rt_pool_alloc(size_t size) {
    if (size == 0)
        size = 1; // Minimum allocation

    rt_pool_begin_op_();

    rt_pool_class_t class_idx = size_to_class(size);

    // Fall back to malloc for large allocations
    if (class_idx >= RT_POOL_COUNT) {
        void *ptr = malloc(size);
        rt_pool_end_op_();
        return ptr;
    }

    rt_pool_state_t *pool = &g_pools[class_idx];

    // Try to pop from freelist
    rt_pool_block_t *block = pop_from_freelist(pool);

    if (!block) {
        // Freelist empty - allocate a new slab
        rt_pool_slab_t *slab = allocate_slab(class_idx);
        if (!slab) {
            rt_pool_end_op_();
            return NULL;
        }

        // Reserve the first block for this allocation before pushing the
        // rest to the freelist. This prevents a race where other threads
        // consume all blocks between push and our pop, causing a spurious
        // NULL return.
        block = (rt_pool_block_t *)(void *)slab->data;
        size_t expected_state = 0;
        if (!rt_atomic_compare_exchange_size(
                &block->meta.state, &expected_state, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_abort("rt_pool: new slab block is not free");

        // Atomically link slab into list using CAS loop
        // This prevents the lost-update race where concurrent slab allocations
        // could orphan one or more slabs (RACE-002 fix)
#if RT_COMPILER_MSVC
        rt_pool_slab_t *expected = (rt_pool_slab_t *)rt_atomic_load_ptr(
            (void *const volatile *)&pool->slabs, __ATOMIC_RELAXED);
        do {
            slab->next = expected;
        } while (!rt_atomic_compare_exchange_ptr((void *volatile *)&pool->slabs,
                                                 (void **)&expected,
                                                 slab,
                                                 __ATOMIC_RELEASE,
                                                 __ATOMIC_RELAXED));
#else
        rt_pool_slab_t *expected = __atomic_load_n(&pool->slabs, __ATOMIC_RELAXED);
        do {
            slab->next = expected;
        } while (!__atomic_compare_exchange_n(
            &pool->slabs, &expected, slab, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
#endif

        // Push remaining blocks (skip first, which is reserved) to freelist
        if (slab->block_count > 1) {
            rt_pool_block_t *first = NULL;
            rt_pool_block_t *last = NULL;

            for (size_t i = 1; i < slab->block_count; i++) {
                rt_pool_block_t *b =
                    (rt_pool_block_t *)(void *)(slab->data + i * slab->block_stride);
                b->meta.next = NULL;

                if (!last) {
                    first = b;
                } else {
                    last->meta.next = b;
                }
                last = b;
            }

            if (!first || !last) {
                rt_pool_end_op_();
                return NULL;
            }

#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
            rt_pool_lock_(&pool->freelist_lock);
            atomic_store_next(last, pool->freelist_head);
            pool->freelist_head = first;
            size_t added = slab->block_count - 1;
            size_t free_count = rt_atomic_load_size(&pool->free_count, __ATOMIC_RELAXED);
            if (free_count > SIZE_MAX - added) {
                rt_pool_unlock_(&pool->freelist_lock);
                rt_abort("rt_pool: free-block counter overflow");
            }
            rt_atomic_fetch_add_size(&pool->free_count, added, __ATOMIC_RELAXED);
            rt_pool_unlock_(&pool->freelist_lock);
#else
            // Atomically prepend the chain to the freelist
            uint64_t old_tagged = atomic_load_u64(&pool->freelist_tagged);
            uint64_t new_tagged;
            do {
                rt_pool_block_t *old_head = (rt_pool_block_t *)unpack_ptr(old_tagged);
                uint16_t old_version = unpack_version(old_tagged);
                atomic_store_next(last, old_head);
                new_tagged = pack_tagged_ptr(first, (uint16_t)(old_version + 1));
            } while (!atomic_cas_u64(&pool->freelist_tagged, &old_tagged, new_tagged));
#endif

#if !RT_POOL_USE_LOCKED_FREELIST && !RT_POOL_PAC_SAFE
            rt_atomic_fetch_add_size(&pool->free_count, slab->block_count - 1, __ATOMIC_RELAXED);
#endif
        }
    }

#if RT_COMPILER_MSVC
    rt_atomic_fetch_add_size(&pool->allocated, 1, __ATOMIC_RELAXED);
#else
    __atomic_fetch_add(&pool->allocated, 1, __ATOMIC_RELAXED);
#endif

    // Zero only caller-visible bytes immediately before publication. Private
    // metadata and unallocated payload capacity are never redundantly cleared.
    void *payload = rt_pool_block_payload_(block);
    memset(payload, 0, kClassSizes[class_idx]);

    rt_pool_end_op_();
    return payload;
}

/// @brief Return a block to the pool freelist, or free via the system allocator.
/// @details Waits for a concurrent shutdown decision before inspecting slab
///          ownership, then returns pooled blocks to their owning freelist.
///          Blocks are cleared immediately before their next allocation, so
///          no previous caller data is observable. Large allocations that
///          bypassed the pool are freed via `free` without lifecycle admission.
///          A small supplied size selects metadata validation, but the validated
///          block owner—not the specific small size—is authoritative for
///          routing. Invalid metadata and duplicate/corrupt block states trap
///          only after lifecycle admission is released.
/// @param ptr Pointer previously returned by rt_pool_alloc; NULL is a no-op.
/// @param size Original allocation size. Supplying a size on the wrong side of
///             the 512-byte boundary violates the allocation contract.
void rt_pool_free(void *ptr, size_t size) {
    if (!ptr)
        return;

    rt_pool_class_t expected_class = size_to_class(size);
    if (expected_class >= RT_POOL_COUNT) {
        free(ptr);
        return;
    }

    rt_pool_begin_op_();
    rt_pool_block_t *block = rt_pool_payload_block_(ptr);
    rt_pool_class_t class_idx = RT_POOL_COUNT;
    const char *error = rt_pool_validate_block_(block, &class_idx);
    if (error) {
        rt_pool_end_op_();
        rt_trap(error);
        return;
    }

    rt_pool_state_t *pool = &g_pools[class_idx];

    // The private owner is authoritative when a caller supplies a stale but
    // still-small requested size. This preserves historical tolerant routing
    // without a linear scan over every slab.
    (void)expected_class;
    error = push_to_freelist(pool, block);
    if (!error) {
        size_t previous = rt_atomic_fetch_sub_size(&pool->allocated, 1, __ATOMIC_RELAXED);
        if (previous == 0)
            rt_abort("rt_pool: allocated-block counter underflow");
    }
    rt_pool_end_op_();
    if (error)
        rt_trap(error);
}

/// @brief Query per-class allocation statistics for monitoring and diagnostics.
/// @details Reads each requested counter atomically with relaxed ordering. The
///          values are individually race-free but do not form one transactional
///          snapshot while allocations continue. Class values at or above
///          @ref RT_POOL_COUNT produce zero outputs. Either output may be null.
///          System-allocation fallback usage is not counted.
/// @param class_idx Valid size class to query (0 = 64B, 1 = 128B, 2 = 256B,
///                  3 = 512B).
/// @param out_allocated Optional destination for blocks currently in use.
/// @param out_free Optional destination for blocks currently on the freelist.
void rt_pool_stats(rt_pool_class_t class_idx, size_t *out_allocated, size_t *out_free) {
    if ((int)class_idx < 0 || class_idx >= RT_POOL_COUNT) {
        if (out_allocated)
            *out_allocated = 0;
        if (out_free)
            *out_free = 0;
        return;
    }

    rt_pool_state_t *pool = &g_pools[class_idx];

#if RT_COMPILER_MSVC
    if (out_allocated)
        *out_allocated = rt_atomic_load_size(&pool->allocated, __ATOMIC_RELAXED);
    if (out_free)
        *out_free = rt_atomic_load_size(&pool->free_count, __ATOMIC_RELAXED);
#else
    if (out_allocated)
        *out_allocated = __atomic_load_n(&pool->allocated, __ATOMIC_RELAXED);
    if (out_free)
        *out_free = __atomic_load_n(&pool->free_count, __ATOMIC_RELAXED);
#endif
}

/// @brief Release quiescent slab classes back to the system allocator.
/// @details Stops new slab-backed operations, waits for active pool operations,
///          and reclaims each size class whose outstanding allocation count is
///          zero. A class with live blocks is left intact so those callers can
///          continue using and later release their memory. Repeating shutdown
///          after the final release reclaims the deferred class. Concurrent
///          shutdown callers wait for the active owner to publish a new epoch
///          and then return; ordinary operations may resume afterward and
///          rebuild reclaimed classes. System-allocation fallbacks are outside
///          this lifecycle and are never reclaimed here.
void rt_pool_shutdown(void) {
    size_t owned_epoch = 0;
    for (;;) {
        size_t epoch = rt_atomic_load_size(&g_pool_lifecycle_epoch, __ATOMIC_ACQUIRE);
        if ((epoch & 1U) != 0U) {
            do {
                rt_pool_yield_();
            } while (rt_atomic_load_size(&g_pool_lifecycle_epoch, __ATOMIC_ACQUIRE) == epoch);
            return;
        }
        size_t expected = epoch;
        if (rt_atomic_compare_exchange_size(&g_pool_lifecycle_epoch,
                                            &expected,
                                            epoch + 1,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
            owned_epoch = epoch;
            break;
        }
    }

    while (rt_atomic_load_size(&g_pool_active_ops, __ATOMIC_ACQUIRE) != 0)
        rt_pool_yield_();

    for (int i = 0; i < RT_POOL_COUNT; i++) {
        rt_pool_state_t *pool = &g_pools[i];

#if RT_COMPILER_MSVC
        size_t allocated = rt_atomic_load_size(&pool->allocated, __ATOMIC_ACQUIRE);
#else
        size_t allocated = __atomic_load_n(&pool->allocated, __ATOMIC_ACQUIRE);
#endif
        if (allocated != 0)
            continue;

        // Free all slabs
        rt_pool_slab_t *slab = pool->slabs;
        while (slab) {
            rt_pool_slab_t *next = slab->next;
            for (size_t block_index = 0; block_index < slab->block_count; ++block_index) {
                rt_pool_block_t *block =
                    (rt_pool_block_t *)(void *)(slab->data + block_index * slab->block_stride);
                block->meta.magic = 0;
                block->meta.slab = NULL;
            }
            free(slab);
            slab = next;
        }

        // Reset state
        pool->slabs = NULL;
#if RT_POOL_USE_LOCKED_FREELIST || RT_POOL_PAC_SAFE
        pool->freelist_head = NULL;
        __atomic_clear(&pool->freelist_lock, __ATOMIC_RELEASE);
#else
        atomic_store_u64(&pool->freelist_tagged, 0);
#endif
#if RT_COMPILER_MSVC
        rt_atomic_store_size(&pool->allocated, 0, __ATOMIC_RELAXED);
        rt_atomic_store_size(&pool->free_count, 0, __ATOMIC_RELAXED);
#else
        __atomic_store_n(&pool->allocated, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pool->free_count, 0, __ATOMIC_RELAXED);
#endif
    }

    rt_atomic_store_size(&g_pool_lifecycle_epoch, owned_epoch + 2, __ATOMIC_RELEASE);
}
