//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_pool.h
// Purpose: Slab allocator with four size classes (64, 128, 256, 512 bytes), reducing allocation
// overhead by reusing freed blocks without returning to the system allocator.
//
// Key invariants:
//   - Size classes cover requests 0-64, 65-128, 129-256, and 257-512 bytes;
//     a zero request is normalized to one byte.
//   - Pooled class capacity is fully zeroed before reuse. Allocations larger
//     than 512 bytes fall back to malloc/free and are not initialized.
//   - Freelist management is synchronized; multiple threads may allocate concurrently.
//   - Private per-block metadata routes frees in O(1) and detects duplicate release.
//   - Freed blocks stay in the freelist until shutdown reclaims a quiescent class.
//   - Shutdown never reclaims a class with outstanding caller-owned blocks.
//   - Ordinary operations use an atomic lifecycle epoch rather than one global hot-path lock.
//
// Ownership/Lifetime:
//   - Callers receive a pointer to the allocated block; no header is exposed.
//   - rt_pool_free requires the original requested size. Small blocks carry a
//     private aligned header that validates and identifies their owning slab.
//   - The pool is process-global and supports an explicit shutdown path.
//   - Shutdown preserves slabs with live blocks and never frees system-fallback
//     allocations on a caller's behalf.
//
// Links: src/runtime/core/rt_pool.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Concurrent fixed-size slab-pool allocation API.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Caller-visible payload size classes for the pool allocator.
/// @details @ref RT_POOL_COUNT is a sentinel and the number of class-state
///          entries; it is not an allocatable class.
typedef enum {
    RT_POOL_64 = 0,  ///< 64-byte blocks
    RT_POOL_128 = 1, ///< 128-byte blocks
    RT_POOL_256 = 2, ///< 256-byte blocks
    RT_POOL_512 = 3, ///< 512-byte blocks
    RT_POOL_COUNT = 4
} rt_pool_class_t;

/// @brief Maximum size handled by the pool allocator.
/// @details Larger requests use the system allocator and are excluded from
///          per-class statistics and shutdown reclamation.
#define RT_POOL_MAX_SIZE 512

/// @brief Allocate memory from the pool.
/// @details Allocates from the appropriate size class pool. Falls back to
///          `malloc` for sizes above @ref RT_POOL_MAX_SIZE. A zero request is
///          normalized to one byte. Pooled storage is maximally aligned and
///          zeroed across its full class capacity; fallback storage is
///          uninitialized.
/// @param size Number of caller-visible bytes requested.
/// @return Pointer to suitable storage, or `NULL` on allocation failure.
void *rt_pool_alloc(size_t size);

/// @brief Free memory back to the pool.
/// @details Waits for concurrent shutdown coordination, then returns the block
///          to its owning size-class freelist in O(1) through private metadata.
///          A duplicate small-block release traps without modifying the
///          freelist or statistics. For large allocations, delegates to free().
///          Null pointers are ignored. The validated private owner controls
///          routing among small classes, but a size on the wrong side of the
///          pooled/system boundary violates the API contract.
/// @param ptr Pointer previously returned by @ref rt_pool_alloc.
/// @param size Exact original allocation request.
void rt_pool_free(void *ptr, size_t size);

/// @brief Get statistics about pool usage.
/// @details Atomically reads the number of allocated and free blocks in one
///          size class. The relaxed reads are individually safe but may reflect
///          different instants under concurrent activity. System allocations
///          are excluded. Either output may be null.
/// @param class_idx Size class index (0-3). Values outside that range produce
///                  zeros.
/// @param out_allocated Optional destination for blocks currently allocated.
/// @param out_free Optional destination for blocks on the freelist.
void rt_pool_stats(rt_pool_class_t class_idx, size_t *out_allocated, size_t *out_free);

/// @brief Release all pool memory back to the system.
/// @details Stops new slab-backed operations, waits for in-flight pool operations
///          to leave their critical sections, and frees slabs only for classes
///          with no outstanding blocks. Live classes remain valid and are
///          reclaimed by a later shutdown after their final release. A later
///          allocation can rebuild classes that were reclaimed. Concurrent
///          shutdown calls serialize through the lifecycle epoch. The function
///          does not track or release outstanding system-fallback allocations.
void rt_pool_shutdown(void);

#ifdef __cplusplus
}
#endif
