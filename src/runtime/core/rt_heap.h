//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_heap.h
// Purpose: Unified heap allocation system for all runtime reference types (strings, arrays,
// objects), providing a common header layout, reference counting, and type metadata.
//
// Key invariants:
//   - Magic field (0x52504956 = 'VIPR') validates heap objects; invalid magic indicates corruption.
//   - refcnt == 1 on fresh allocation; the allocating caller owns the initial reference.
//   - refcnt >= RT_HEAP_IMMORTAL_REFCNT means the payload is immortal and is never released.
//   - len <= cap invariant is maintained by all mutating operations.
//   - Payload pointer is exactly sizeof(rt_heap_hdr_t) bytes after the header base address.
//
// Ownership/Lifetime:
//   - Heap objects are reference-counted; the last rt_heap_release call frees the memory.
//   - rt_heap_retain increments the refcount; rt_heap_release decrements and frees at zero.
//   - Final heap reclamation clears registered weak observers for every payload kind.
//   - rt_heap_release_deferred decrements without immediate free for batch cleanup.
//
// Links: src/runtime/core/rt_heap.c (implementation), src/runtime/core/rt_string.h,
// src/runtime/arrays/rt_array.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the registered reference-counted heap and metadata layout.
/// @details Every live payload is preceded by @ref rt_heap_hdr_t and recorded
///   in a synchronized registry used to reject stale or foreign pointers.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief Optional callback invoked before freeing a heap payload.
/// @details Finalizers are attached only to `RT_HEAP_OBJECT` payloads. Object
///   release and cycle collection invoke them before reclamation, while the
///   global shutdown sweep may invoke a still-live object's finalizer early to
///   release native resources. Each lifecycle path detaches the callback to
///   preserve at-most-once execution.
/// @param payload Borrowed object payload whose header remains available for
///   the duration of the callback.
typedef void (*rt_heap_finalizer_t)(void *payload);

/// @brief Heap object kind tag.
/// @details Distinguishes between the three major runtime reference types
///          for type-safe operations and proper cleanup logic.
typedef enum {
    RT_HEAP_STRING = 1, ///< Heap-allocated string (UTF-8 payload).
    RT_HEAP_ARRAY = 2,  ///< Heap-allocated array (element payload).
    RT_HEAP_OBJECT = 3, ///< Heap-allocated OOP object.
} rt_heap_kind_t;

/// @brief Element type tag for array payloads.
/// @details Stored in the heap header's elem_kind field. Determines element
///          size, alignment, and cleanup behavior (e.g., RT_ELEM_STR requires
///          releasing each string element).
typedef enum {
    RT_ELEM_NONE = 0, ///< No element type (used for non-array heap objects).
    RT_ELEM_I32 = 1,  ///< 32-bit signed integer elements.
    RT_ELEM_I64 = 2,  ///< 64-bit signed integer elements.
    RT_ELEM_F64 = 3,  ///< 64-bit floating-point elements.
    RT_ELEM_U8 = 4,   ///< Unsigned byte elements (used for strings).
    RT_ELEM_STR = 5,  ///< String pointer (rt_string) elements requiring reference counting.
    RT_ELEM_BOX = 6,  ///< Boxed primitive value (rt_box_t) elements with type tag.
    RT_ELEM_OBJ = 7,  ///< Object pointer elements requiring reference counting.
} rt_elem_kind_t;

/// @brief Heap object header preceding every payload.
/// @details Contains metadata for validation, type safety, reference counting,
///          and capacity management. The payload immediately follows this header.
// The tag differs from the `rt_heap_hdr()` accessor below: in C++ translation
// units a function sharing the struct's name hides its injected constructor.
typedef struct rt_heap_hdr_s {
    uint32_t magic;                ///< Validation marker (must be RT_MAGIC = 0x52504956).
    uint16_t kind;                 ///< Heap object kind (rt_heap_kind_t).
    uint16_t elem_kind;            ///< Element type tag (rt_elem_kind_t).
    uint32_t flags;                ///< Debug/status flags: bit0=disposed, bit1=pool-allocated.
    size_t refcnt;                 ///< Current reference count.
    size_t len;                    ///< Current logical length (number of valid elements).
    size_t cap;                    ///< Total capacity (maximum elements before reallocation).
    size_t alloc_size;             ///< Total allocation size in bytes (header + payload).
    int64_t class_id;              ///< Optional runtime class identifier (objects only).
    rt_heap_finalizer_t finalizer; ///< Optional finalizer callback (objects only).
} rt_heap_hdr_t;

/// @brief Lock-bounded copy of heap metadata for borrowed-pointer inspection.
/// @details Unlike @ref rt_heap_hdr_t this structure never aliases live heap
///          storage. @ref rt_heap_get_info fills it while holding the allocation
///          registry lock, so callers may inspect an untrusted or merely
///          borrowed pointer without retaining an unpinned header address.
///          The reference count is an observation only and must not be used as
///          a substitute for @ref rt_heap_try_retain_live.
typedef struct rt_heap_info {
    uint16_t kind;      ///< Heap object kind copied from the live header.
    uint16_t elem_kind; ///< Element kind copied from the live header.
    uint32_t flags;     ///< Allocation/status flags at snapshot time.
    size_t refcnt;      ///< Atomically sampled reference count.
    size_t len;         ///< Logical length at snapshot time.
    size_t cap;         ///< Payload capacity at snapshot time.
    size_t alloc_size;  ///< Header-plus-payload allocation size.
    int64_t class_id;   ///< Runtime class identifier for object payloads.
} rt_heap_info_t;

/// @brief Flag indicating the allocation came from the pool allocator.
#define RT_HEAP_FLAG_POOLED 0x2u

/// @brief Magic number for heap object validation ('VIPR' in little-endian).
#define RT_MAGIC 0x52504956u

/// @brief Refcount sentinel used by immutable/immortal heap-backed values.
#define RT_HEAP_IMMORTAL_REFCNT (SIZE_MAX - 1u)

/// @brief Largest mortal refcount; retaining beyond this would collide with the sentinel.
#define RT_HEAP_MAX_MORTAL_REFCNT (RT_HEAP_IMMORTAL_REFCNT - 1u)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate a new heap object with header and payload.
/// @details Provides a unified allocation path for all runtime reference types
///          (strings, arrays, objects) with consistent metadata and refcount
///          semantics. Allocates a contiguous block consisting of an rt_heap_hdr_t
///          followed by a payload region sized for @p init_cap elements of
///          @p elem_size. Initializes header fields (magic/kind/elem_kind/refcnt/
///          len/cap), registers the payload, and automatically registers
///          object- and box-reference arrays for cycle collection. When
///          @p init_cap is smaller than @p init_len, capacity is raised to the
///          requested length. Small strings may use the slab pool.
/// @param kind       Logical heap object kind (string/array/object).
/// @param elem_kind  Element type tag for arrays (RT_ELEM_*); RT_ELEM_NONE for others.
/// @param elem_size  Size in bytes of one logical element in the payload.
/// @param init_len   Initial logical length; must be <= init_cap.
/// @param init_cap   Initial capacity in elements; 0 permitted (no payload).
/// @return Caller-owned payload pointer on success; NULL on invalid sizing,
///   allocation, registry, or reference-array tracking failure. Shutdown-handler
///   registration failure traps and returns NULL if dispatch continues.
/// @post refcnt == 1; len == init_len; cap == max(init_cap, init_len).
void *rt_heap_alloc(rt_heap_kind_t kind,
                    rt_elem_kind_t elem_kind,
                    size_t elem_size,
                    size_t init_len,
                    size_t init_cap);

/// @brief Increment the reference count of a heap payload.
/// @details Shares ownership of a heap object safely across callers.
///          NULL and immortal payloads are no-ops. Invalid, already-zero, or
///          overflowing mortal payloads raise a runtime trap.
/// @param payload Payload pointer previously returned by rt_heap_alloc (may be NULL).
/// @post A live mortal refcount below the maximum is increased by one.
void rt_heap_retain(void *payload);

/// @brief Try to retain a live heap payload without trapping.
/// @details Returns 1 when @p payload was retained, 2 when it is live but
///          immortal and therefore was not retained, 0 when the pointer is
///          invalid/freed or its refcount is already zero, and -1 when
///          retaining would overflow the mortal refcount range.
/// @param payload Exact borrowed payload address, or NULL.
/// @return Status code 1, 2, 0, or -1 as described; this helper never traps for
///   pointer validity or refcount state.
int32_t rt_heap_try_retain_live(void *payload);

/// @brief Decrement the reference count, freeing the object when it reaches zero.
/// @details Releases ownership of a heap object. When the reference count drops
///          to zero, registered weak observers are cleared before the header and
///          payload memory are freed. Invalid pointers, double release, and
///          attempts to release an immortal count raise a runtime trap.
/// @param payload Payload pointer or NULL (NULL is ignored).
/// @return New reference count after decrement (0 when freed).
size_t rt_heap_release(void *payload);

/// @brief Debug aid: print every live object payload (class id, size, refcount).
/// @details Used by the collector's `ZANNA_GC_DUMP_TRACKED` dump; single-threaded.
void rt_heap_debug_dump_objects(void);

/// @brief Decrement the reference count without immediate free.
/// @details Allows batched cleanup in contexts where immediate free is unsafe
///          (e.g., re-entrant callbacks) or to avoid deep recursive frees. This
///          function does not queue the allocation: when it returns zero, the
///          caller must complete element/finalizer cleanup and explicitly call
///          @ref rt_heap_free_zero_ref.
/// @param payload Payload pointer or NULL (NULL is ignored).
/// @return New reference count after decrement.
size_t rt_heap_release_deferred(void *payload);

/// @brief Free a payload whose reference count is already zero.
/// @details Provides an explicit free entry point after deferred release or
///          external handoff. Clears registered weak observers before reclaiming
///          storage. A live nonzero count leaves the payload unchanged; an
///          invalid non-NULL pointer raises a runtime trap.
/// @param payload Payload pointer with refcnt == 0; NULL is ignored.
/// @pre refcnt == 0 (prior rt_heap_release_deferred or external setting).
void rt_heap_free_zero_ref(void *payload);

/// @brief Safely test whether a pointer is a currently live runtime heap payload.
/// @param payload Candidate payload pointer.
/// @return 1 when the pointer belongs to a live runtime heap allocation, 0 otherwise.
int8_t rt_heap_is_payload(void *payload);

/// @brief Safely recover a heap header from a candidate payload pointer.
/// @details Unlike @ref rt_heap_hdr, this helper validates that @p payload is a
///          live runtime allocation before exposing the header pointer. The
///          caller must already own a strong reference or the collector's
///          exclusive graph scope and must not retain the pointer beyond that
///          ownership interval. Borrowed/untrusted readers should use
///          @ref rt_heap_get_info instead.
/// @param payload Candidate payload pointer.
/// @param out_hdr Optional destination receiving the header on success and
///   cleared on failure.
/// @return 1 when @p payload is valid, 0 otherwise.
int8_t rt_heap_try_get_header(void *payload, rt_heap_hdr_t **out_hdr);

/// @brief Copy metadata for a live heap payload without exposing its header address.
/// @details Looks up @p payload and copies all public scalar metadata while the
///          allocation registry lock prevents concurrent removal or relocation.
///          Failure clears @p out_info. This is the preferred validation API
///          for borrowed handles because the returned data remains ordinary
///          caller-owned storage after the lock is released.
/// @param payload Candidate exact payload pointer.
/// @param out_info Destination metadata snapshot; required.
/// @return 1 when @p payload names a registered allocation, otherwise 0.
int8_t rt_heap_get_info(const void *payload, rt_heap_info_t *out_info);

/// @brief Safely test whether a byte range lies inside a registered runtime heap payload.
/// @details This helper accepts interior pointers, unlike @ref rt_heap_is_payload
///          and @ref rt_heap_try_get_header, which require the exact payload
///          address returned by the allocator. It scans the allocation
///          registry under the heap lock and validates that the full
///          `[ptr, ptr + bytes)` range is contained by one payload before any
///          caller dereferences an interior object, array, or string field.
///          Payloads whose refcount has reached zero remain valid here until
///          the corresponding explicit free removes them from the registry.
/// @param ptr Candidate start address, which may be an interior payload pointer.
/// @param bytes Number of bytes that must fit inside the same payload.
/// @return 1 when the entire range is inside one registered runtime heap payload,
///         otherwise 0.
int8_t rt_heap_contains_range(const void *ptr, size_t bytes);

/// @brief Enable or disable raw-allocation tracking for the reference IL VM (ZB-28).
/// @details When enabled, every rt_alloc block is recorded until rt_free so
///          rt_alloc_contains_range can classify it as program-owned memory.
///          Off by default; disabling drops the table. Only the IL VM runner
///          enables it — native programs and the bytecode VM never pay for it.
/// @param enabled Nonzero to record allocations, zero to stop and free the table.
void rt_alloc_set_tracking(int enabled);

/// @brief Whether raw-allocation tracking is currently enabled.
/// @return Nonzero when rt_alloc blocks are being recorded.
int rt_alloc_tracking_enabled(void);

/// @brief Test whether a byte range lies wholly inside one live rt_alloc block.
/// @param ptr First byte of the requested range.
/// @param bytes Number of bytes requested; zero-byte ranges are never owned.
/// @return 1 when tracking is enabled and the range is inside one recorded
///         block, otherwise 0.
int8_t rt_alloc_contains_range(const void *ptr, size_t bytes);

/// @brief Retrieve the header from a payload pointer.
/// @details Validates exact registry membership before exposing the borrowed
///   header. The caller must already pin the payload against concurrent final
///   release while using the returned pointer.
/// @param payload Payload pointer as returned by allocation APIs.
/// @return Pointer to the associated header, or NULL for a NULL payload.
/// @warning Invalid non-NULL payloads raise a runtime trap.
rt_heap_hdr_t *rt_heap_hdr(void *payload);

/// @brief Resize a heap payload while preserving runtime metadata.
/// @details Reallocates the underlying block, zero-fills any newly exposed
///          elements, updates len/cap/alloc_size, and keeps the runtime's
///          allocation registry in sync when the payload moves. The primitive
///          validates that the payload has exactly one live owner; shared or
///          immortal allocations trap and remain unchanged so a missed
///          copy-on-write check cannot invalidate aliases. @p new_cap is raised
///          to @p new_len when necessary. The old pointer is invalid after a
///          successful move, and cycle/weak bookkeeping follows the replacement.
/// @param payload Existing heap payload pointer.
/// @param elem_size Element size in bytes for the payload.
/// @param new_len New logical length.
/// @param new_cap New capacity in elements.
/// @return Resized payload pointer, or NULL on allocation failure / invalid input.
/// @pre @p payload has a mortal reference count of exactly one.
void *rt_heap_realloc(void *payload, size_t elem_size, size_t new_len, size_t new_cap);

/// @brief Retrieve the payload address from a header pointer.
/// @details Returns a pointer immediately after the header structure.
///          Converts between header and payload views when manipulating
///          metadata. Invalid non-NULL headers raise a runtime trap.
/// @param h Header pointer from rt_heap_hdr().
/// @return Payload pointer.
void *rt_heap_data(rt_heap_hdr_t *h);

/// @brief Read the current logical length from the header.
/// @details Returns header->len for the given payload. NULL returns zero;
///   invalid non-NULL payloads trap.
/// @param payload Payload pointer.
/// @return Logical element count.
size_t rt_heap_len(void *payload);

/// @brief Read the current capacity from the header.
/// @details Returns header->cap for the given payload. NULL returns zero;
///   invalid non-NULL payloads trap.
/// @param payload Payload pointer.
/// @return Capacity in elements.
size_t rt_heap_cap(void *payload);

/// @brief Update the logical length stored in the header.
/// @details Writes header->len to @p new_len. Used to record changes after
///          append or resize operations. NULL is a no-op; invalid payloads or
///          lengths greater than capacity raise a runtime trap.
/// @param payload Payload pointer.
/// @param new_len New logical length; must be <= current capacity.
/// @pre 0 <= new_len <= cap.
/// @post Subsequent rt_heap_len(payload) == new_len.
void rt_heap_set_len(void *payload, size_t new_len);

/// @brief Mark an object payload as disposed (debug aid).
/// @details Sets a header bit to guard against double-dispose bugs in
///          higher-level object lifecycles. Intended for assertions and
///          diagnostics; does not change the refcount. No-op for NULL payloads.
/// @param payload Object payload pointer (may be NULL).
/// @return 1 when marking for the first time; 0 when already marked disposed.
int32_t rt_heap_mark_disposed(void *payload);

#ifdef __cplusplus
}
#endif
