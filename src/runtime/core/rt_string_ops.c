//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_ops.c
// Purpose: Implements live-handle registration, allocation and atomic
// reference counting, byte-string construction/slicing/search/trimming,
// UTF-8-codepoint Mid operations, ASCII case conversion, and comparisons.
//
// Key invariants:
//   - Every live handle is present in a synchronized open-addressed registry
//     before generic validation may recognize it.
//   - Embedded/literal counts and heap-header counts are updated atomically;
//     immortal sentinels do not change.
//   - Null policy is operation-specific: strict BASIC slices trap, length treats
//     null as empty, searches miss, and comparison operators define explicit
//     boolean fallbacks.
//   - String-returning slices never alias a partial source buffer. They allocate
//     a copy, retain a full source, or return the immortal empty singleton.
//   - Case conversion is byte-level ASCII; multi-byte UTF-8 sequences are passed
//     through unchanged.
//   - Stored lengths, Left/Right/Substr, and search indices are byte-based.
//     Mid/MidLen convert one-based codepoint positions through strict UTF-8.
//
// Ownership/Lifetime:
//   - Every returned rt_string transfers one owned claim, whether newly
//     allocated, retained, reused, or immortal.
//   - Inputs are borrowed except rt_str_concat, which normally consumes one
//     ownership claim from each operand.
//   - Final release clears weak references, unregisters the handle, and frees
//     wrapper/payload storage according to its representation.
//
// Links: src/runtime/core/rt_string_internal.h (shared helpers),
//        src/runtime/core/rt_string_advanced.c (extended operations),
//        src/runtime/core/rt_string_specialized.c (case conversion + distance),
//        src/runtime/core/rt_string.h (rt_string type and ref-counting API),
//        docs/adr/0288-allocation-free-string-byte-access.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Core runtime string registry, ownership, slicing, and comparison.

#include "rt_gc.h"
#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_string_internal.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !RT_PLATFORM_WINDOWS
#include <sched.h>
#endif

static const size_t kImmortalRefcnt = RT_HEAP_IMMORTAL_REFCNT;

#define RT_STRING_REG_EMPTY NULL
#define RT_STRING_REG_TOMBSTONE ((rt_string)(uintptr_t)1)

typedef struct {
    rt_string *slots;
    size_t count;
    size_t tombstones;
    size_t capacity;
    int lock;
} rt_string_registry_t;

static rt_string_registry_t g_string_registry_;

// ---------------------------------------------------------------------------
// String-handle registry — a global open-addressed hash set keyed
// by `rt_string` pointer. Lets `rt_string_is_handle` cheaply check
// whether a `void*` is a known managed string vs. raw memory.
// Protected by a hand-rolled spinlock since the operations are
// short and contention is rare.
// ---------------------------------------------------------------------------

/// @brief Splittable-mix64 hash for a pointer-sized value (Stafford variant 13).
/// @param p Pointer value to mix; may be NULL.
/// @return Deterministic 64-bit hash of the pointer representation.
static uint64_t rt_string_ptr_hash_(const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

/// @brief Acquire the registry spinlock; yields the OS thread on contention.
/// @details Uses acquire ordering so subsequent registry reads observe all
///          writes published before the previous release.
static void rt_string_registry_lock_(void) {
    if (__atomic_test_and_set(&g_string_registry_.lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(&g_string_registry_.lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release the registry spinlock.
/// @details Uses release ordering to publish completed registry mutations.
static void rt_string_registry_unlock_(void) {
    __atomic_clear(&g_string_registry_.lock, __ATOMIC_RELEASE);
}

/// @brief Slot-state predicate — true unless the slot is empty or a tombstone.
/// @param slot Registry slot value to classify.
/// @return One only for a live string pointer.
static int rt_string_registry_slot_is_live_(rt_string slot) {
    return slot != RT_STRING_REG_EMPTY && slot != RT_STRING_REG_TOMBSTONE;
}

/// @brief Reallocate the registry to at least `min_capacity` slots and rehash live entries.
/// @details Starts at 256 or doubles current capacity, then keeps doubling to
///          satisfy @p min_capacity. Capacity remains a power of two so probing
///          uses a mask. Live pointers are rehashed and tombstones discarded;
///          failure leaves the existing registry unchanged.
/// @param min_capacity Minimum requested slot count.
/// @return One on success, or zero on allocation/size overflow.
/// @pre The caller holds the registry lock.
static int rt_string_registry_grow_locked_(size_t min_capacity) {
    size_t new_capacity = 256;
    if (g_string_registry_.capacity) {
        if (g_string_registry_.capacity > SIZE_MAX / 2)
            return 0;
        new_capacity = g_string_registry_.capacity * 2;
    }
    while (new_capacity < min_capacity) {
        if (new_capacity > SIZE_MAX / 2)
            return 0;
        new_capacity *= 2;
    }

    rt_string *new_slots = (rt_string *)calloc(new_capacity, sizeof(rt_string));
    if (!new_slots)
        return 0;

    if (g_string_registry_.slots) {
        const size_t mask = new_capacity - 1;
        for (size_t i = 0; i < g_string_registry_.capacity; ++i) {
            rt_string slot = g_string_registry_.slots[i];
            if (!rt_string_registry_slot_is_live_(slot))
                continue;
            size_t idx = (size_t)(rt_string_ptr_hash_(slot) & mask);
            while (new_slots[idx] != RT_STRING_REG_EMPTY)
                idx = (idx + 1) & mask;
            new_slots[idx] = slot;
        }
        free(g_string_registry_.slots);
    }

    g_string_registry_.slots = new_slots;
    g_string_registry_.capacity = new_capacity;
    g_string_registry_.tombstones = 0;
    return 1;
}

/// @brief Grow if needed to keep load factor < 5/8 (count + tombstones / capacity).
/// @details Accounts for the pending insertion and initializes an absent table.
/// @return One when an insertion slot can be sought, or zero on arithmetic,
///         capacity, or allocation failure.
/// @pre The caller holds the registry lock.
static int rt_string_registry_ensure_capacity_locked_(void) {
    if (g_string_registry_.capacity == 0)
        return rt_string_registry_grow_locked_(256);
    if (g_string_registry_.count > SIZE_MAX - g_string_registry_.tombstones - 1)
        return 0;
    size_t projected = g_string_registry_.count + g_string_registry_.tombstones + 1;
    size_t threshold = (g_string_registry_.capacity / 8) * 5;
    if (projected >= threshold) {
        if (g_string_registry_.capacity > SIZE_MAX / 2)
            return 0;
        return rt_string_registry_grow_locked_(g_string_registry_.capacity * 2);
    }
    return 1;
}

/// @brief Add `s` to the registry. Tracks the first tombstone so re-insertions reuse it.
/// @details Null and already-present handles succeed without mutation. Linear
///          probing remembers the first tombstone but continues until an empty
///          slot confirms the pointer was not already registered.
/// @param s Live handle to add; may be NULL.
/// @return One on success/already-present input, or zero on growth failure.
/// @pre The caller holds the registry lock.
static int rt_string_registry_insert_locked_(rt_string s) {
    if (!s)
        return 1;
    if (!rt_string_registry_ensure_capacity_locked_())
        return 0;

    const size_t mask = g_string_registry_.capacity - 1;
    size_t idx = (size_t)(rt_string_ptr_hash_(s) & mask);
    size_t first_tombstone = SIZE_MAX;
    while (1) {
        rt_string slot = g_string_registry_.slots[idx];
        if (slot == s)
            return 1;
        if (slot == RT_STRING_REG_EMPTY) {
            size_t target = first_tombstone != SIZE_MAX ? first_tombstone : idx;
            if (first_tombstone != SIZE_MAX)
                g_string_registry_.tombstones--;
            g_string_registry_.slots[target] = s;
            g_string_registry_.count++;
            return 1;
        }
        if (slot == RT_STRING_REG_TOMBSTONE && first_tombstone == SIZE_MAX)
            first_tombstone = idx;
        idx = (idx + 1) & mask;
    }
}

/// @brief Replace `s`'s slot with a tombstone — preserves probe sequences without needing to
/// rehash.
/// @param s Handle to remove; null and absent values are ignored.
/// @pre The caller holds the registry lock.
static void rt_string_registry_remove_locked_(rt_string s) {
    if (!s || !g_string_registry_.slots || g_string_registry_.capacity == 0)
        return;
    const size_t mask = g_string_registry_.capacity - 1;
    size_t idx = (size_t)(rt_string_ptr_hash_(s) & mask);
    while (1) {
        rt_string slot = g_string_registry_.slots[idx];
        if (slot == s) {
            g_string_registry_.slots[idx] = RT_STRING_REG_TOMBSTONE;
            g_string_registry_.count--;
            g_string_registry_.tombstones++;
            return;
        }
        if (slot == RT_STRING_REG_EMPTY)
            return;
        idx = (idx + 1) & mask;
    }
}

/// @brief Probe the registry for `s`. Returns 1 if found, 0 otherwise.
/// @param s Candidate handle pointer.
/// @return One when the exact pointer occupies a live slot; otherwise zero.
/// @pre The caller holds the registry lock.
static int rt_string_registry_contains_locked_(const rt_string s) {
    if (!s || s == RT_STRING_REG_TOMBSTONE || !g_string_registry_.slots ||
        g_string_registry_.capacity == 0)
        return 0;
    const size_t mask = g_string_registry_.capacity - 1;
    size_t idx = (size_t)(rt_string_ptr_hash_(s) & mask);
    while (1) {
        rt_string slot = g_string_registry_.slots[idx];
        if (slot == s)
            return 1;
        if (slot == RT_STRING_REG_EMPTY)
            return 0;
        idx = (idx + 1) & mask;
    }
}

/// @brief Register a managed handle for later safe type validation.
/// @details Serializes insertion into the global pointer set. Null and duplicate
///          handles are no-ops. Growth failure traps only after releasing the
///          lock.
/// @param s Live runtime string handle; may be NULL.
void rt_string_register_handle(rt_string s) {
    if (!s)
        return;
    rt_string_registry_lock_();
    int ok = rt_string_registry_insert_locked_(s);
    rt_string_registry_unlock_();
    if (!ok)
        rt_trap("rt_string_register_handle: registry alloc");
}

/// @brief Unregister a string handle before freeing its wrapper.
/// @details Removes under the registry lock; null or absent handles are no-ops.
/// @param s Handle being destroyed; may be NULL.
void rt_string_unregister_handle(rt_string s) {
    if (!s)
        return;
    rt_string_registry_lock_();
    rt_string_registry_remove_locked_(s);
    rt_string_registry_unlock_();
}

/// @brief Free the registry's backing storage at runtime shutdown.
/// @details Clears slots, live/tombstone counts, and capacity under the lock.
///          Later registration can lazily allocate a new table, but existing
///          live handles are not automatically re-registered.
/// @warning Requires runtime-wide quiescence: no concurrent validation or
///          registry mutation may race with shutdown.
void rt_string_registry_shutdown(void) {
    rt_string_registry_lock_();
    free(g_string_registry_.slots);
    g_string_registry_.slots = NULL;
    g_string_registry_.count = 0;
    g_string_registry_.tombstones = 0;
    g_string_registry_.capacity = 0;
    rt_string_registry_unlock_();
}

/// @brief Require a non-null live string handle before reading wrapper fields.
/// @param s Candidate string handle.
/// @param diagnostic Stable operation-specific trap message.
/// @return One for a registered live handle; otherwise zero after trapping.
static int rt_string_require_handle_(rt_string s, const char *diagnostic) {
    if (s && rt_string_is_handle((void *)s))
        return 1;
    rt_trap(diagnostic);
    return 0;
}

/// @brief Retrieve the heap header from an already-validated string handle.
/// @details Literal and embedded strings have no shared-heap header. A separate
///          heap pointer is accepted only when the data payload is still in the
///          heap registry and resolves to the exact header recorded by the
///          wrapper; this avoids dereferencing a corrupt header pointer.
/// @param s Registered live runtime string; may be NULL.
/// @return Borrowed heap header, or `NULL` for non-heap/corrupt input.
static rt_heap_hdr_t *rt_string_registered_header_(rt_string s) {
    if (!s)
        return NULL;
    if (!s->heap || s->heap == RT_SSO_SENTINEL)
        return NULL;
    rt_heap_hdr_t *registered = NULL;
    if (!s->data || !rt_heap_try_get_header(s->data, &registered) || registered != s->heap ||
        registered->magic != RT_MAGIC || registered->kind != RT_HEAP_STRING) {
        rt_trap("rt_string_header: corrupted string header");
        return NULL;
    }
    return registered;
}

/// @brief Report the byte length of a runtime string payload.
/// @details Handles literal strings, embedded (SSO) strings, and heap-backed
///          strings. Literal and embedded strings store the length in literal_len,
///          while heap-backed strings derive the length from the heap header.
///          Null yields zero. Invalid or corrupt handles trap before a fallback
///          return if the configured trap mechanism permits recovery.
/// @param s Borrowed runtime string; may be NULL.
/// @return Stored byte count excluding the trailing terminator, or zero for
///         null/returning-validation fallback.
size_t rt_string_len_bytes(rt_string s) {
    if (!s)
        return 0;
    if (!rt_string_require_handle_(s, "rt_string_len_bytes: invalid string handle"))
        return 0;
    // Literal strings (heap == NULL) and embedded strings (heap == RT_SSO_SENTINEL)
    // both store length in literal_len
    if (!s->heap || s->heap == RT_SSO_SENTINEL)
        return s->literal_len;
    rt_heap_hdr_t *hdr = rt_string_registered_header_(s);
    return hdr ? hdr->len : 0;
}

/// @brief Determine whether a heap-backed string should never be freed.
/// @details Immortal strings use a sentinel reference count so they can be
///          shared globally without participating in retain/release bookkeeping.
/// @param hdr Borrowed heap header; may be NULL.
/// @return One when its atomically loaded count is at least the immortal
///         sentinel; otherwise zero.
static int rt_string_is_immortal_hdr(const rt_heap_hdr_t *hdr) {
    if (!hdr)
        return 0;
    return __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED) >= kImmortalRefcnt;
}

/// @brief Check if a string can be extended in-place for concatenation.
/// @details Returns non-zero if:
///          - String is heap-backed (not literal or SSO)
///          - Reference count is exactly 1 (sole owner)
///          - String is not immortal
///          - Capacity is sufficient for the required length
/// @param s Borrowed candidate left operand; may be NULL.
/// @param required_len Required total capacity including terminator space.
/// @return One only when safe in-place mutation is possible.
static int rt_string_can_append_inplace(rt_string s, size_t required_len) {
    rt_heap_hdr_t *hdr = rt_string_registered_header_(s);
    if (!hdr)
        return 0; // Not heap-backed
    if (rt_string_is_immortal_hdr(hdr))
        return 0; // Immortal strings cannot be modified
    size_t refcnt = __atomic_load_n(&hdr->refcnt, __ATOMIC_RELAXED);
    if (refcnt != 1)
        return 0; // Not sole owner
    // Capacity stores the total allocation size (includes space for null)
    if (hdr->cap < required_len)
        return 0; // Not enough capacity
    return 1;
}

/// @brief Allocate a runtime string with embedded data storage.
/// @details For small strings, this allocates the handle and string data in a
///          single allocation, with the data following immediately after the
///          rt_string_impl struct. Initializes metadata, a mortal reference
///          count of one, and the trailing NUL, then registers the handle.
///          Payload bytes before @p len remain uninitialized.
/// @param len Number of bytes in the string (must be <= RT_SSO_MAX_LEN).
/// @return Owned registered embedded string, or `NULL` after allocation failure.
static rt_string rt_string_alloc_embedded(size_t len) {
    assert(len <= RT_SSO_MAX_LEN);
    // Allocate struct + string data + null terminator in one block
    size_t total = sizeof(struct rt_string_impl) + len + 1;
    rt_string s = (rt_string)rt_alloc((int64_t)total);
    if (!s) {
        rt_trap("rt_string_alloc_embedded: alloc");
        return NULL;
    }
    s->magic = RT_STRING_MAGIC;
    // Data is embedded immediately after the struct
    s->data = (char *)(s + 1);
    s->heap = RT_SSO_SENTINEL;
    s->literal_len = len;
    s->literal_refs = 1; // Reference count for embedded strings
    s->data[len] = '\0';
    rt_string_register_handle(s);
    return s;
}

/// @brief Wrap a raw heap payload in a runtime string handle.
/// @details Allocates the small @ref rt_string structure that tracks the payload
///          pointer and associated metadata, then registers it. On wrapper
///          allocation failure the function traps and releases @p payload.
/// @param payload Owned string-kind allocation produced by @ref rt_heap_alloc;
///        may be NULL.
/// @return Owned registered wrapper, or `NULL` for null input or failure.
static rt_string rt_string_wrap(char *payload) {
    if (!payload)
        return NULL;
    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    assert(hdr);
    assert(hdr->kind == RT_HEAP_STRING);
    rt_string s = (rt_string)rt_alloc(sizeof(*s));
    if (!s) {
        rt_trap("rt_string_wrap: alloc");
        rt_heap_release(payload);
        return NULL;
    }
    s->magic = RT_STRING_MAGIC;
    s->data = payload;
    s->heap = hdr;
    s->literal_len = 0;
    s->literal_refs = 0;
    rt_string_register_handle(s);
    return s;
}

/// @brief Allocate a mutable runtime string with the requested length/capacity.
/// @details Uses embedded allocation for small strings (len <= RT_SSO_MAX_LEN),
///          otherwise uses the shared heap allocator. Ensures the capacity
///          accounts for a trailing null terminator, and traps on overflow or
///          allocation failure. Only `data[len]` is initialized; callers must
///          fill the preceding payload bytes before exposing the result.
/// @param len Initial stored byte length.
/// @param cap Requested total capacity including terminator space; values below
///        `len + 1` are normalized upward.
/// @return Owned registered string handle, or `NULL` after failure.
rt_string rt_string_alloc(size_t len, size_t cap) {
    if (len >= SIZE_MAX) {
        rt_trap("rt_string_alloc: length overflow");
        return NULL;
    }
    // Use embedded allocation for small strings
    if (len <= RT_SSO_MAX_LEN && cap <= RT_SSO_MAX_LEN + 1) {
        return rt_string_alloc_embedded(len);
    }
    size_t required = len + 1;
    if (cap < required)
        cap = required;
    char *payload = (char *)rt_heap_alloc(RT_HEAP_STRING, RT_ELEM_NONE, 1, len, cap);
    if (!payload) {
        rt_trap("out of memory");
        return NULL;
    }
    payload[len] = '\0';
    return rt_string_wrap(payload);
}

/// @brief Return a shared handle representing the empty string.
/// @details Lazily initialises an immortal heap allocation so every caller
///          receives the same handle. Atomic compare/exchange publishes one
///          candidate; a racing loser unregisters and releases its candidate.
///          The immortal reference count makes later retain/release calls
///          no-ops.
/// @return Shared registered empty handle, or `NULL` after allocation failure.
rt_string rt_empty_string(void) {
    static rt_string empty = NULL;
#if RT_COMPILER_MSVC
    rt_string cached =
        (rt_string)rt_atomic_load_ptr((void *const volatile *)&empty, __ATOMIC_ACQUIRE);
#else
    rt_string cached = __atomic_load_n(&empty, __ATOMIC_ACQUIRE);
#endif
    if (cached)
        return cached;

    char *payload = (char *)rt_heap_alloc(RT_HEAP_STRING, RT_ELEM_NONE, 1, 0, 1);
    if (!payload) {
        rt_trap("rt_empty_string: alloc");
        return NULL;
    }
    payload[0] = '\0';

    rt_heap_hdr_t *hdr = rt_heap_hdr(payload);
    assert(hdr);
    assert(hdr->kind == RT_HEAP_STRING);
    __atomic_store_n(&hdr->refcnt, kImmortalRefcnt, __ATOMIC_RELAXED);

    rt_string candidate = (rt_string)rt_alloc(sizeof(*candidate));
    if (!candidate) {
        rt_trap("rt_empty_string: alloc");
        __atomic_store_n(&hdr->refcnt, 1, __ATOMIC_RELAXED);
        rt_heap_release(payload);
        return NULL;
    }
    candidate->magic = RT_STRING_MAGIC;
    candidate->data = payload;
    candidate->heap = hdr;
    candidate->literal_len = 0;
    candidate->literal_refs = 0;
    rt_string_register_handle(candidate);

    rt_string expected = NULL;
#if RT_COMPILER_MSVC
    if (!rt_atomic_compare_exchange_ptr((void *volatile *)&empty,
                                        (void **)&expected,
                                        candidate,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE))
#else
    if (!__atomic_compare_exchange_n(
            &empty, &expected, candidate, /*weak=*/0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
#endif
    {
        // Lost the race; discard our candidate and use the published singleton.
        rt_string_unregister_handle(candidate);
        free(candidate);
        __atomic_store_n(&hdr->refcnt, 1, __ATOMIC_RELAXED);
        rt_heap_release(payload);
        return expected;
    }
    return candidate;
}

/// @brief Allocate a runtime string from a byte span.
/// @details Copies arbitrary bytes, including embedded NUL and malformed UTF-8,
///          then writes a distinct trailing terminator. Null is accepted only
///          for a zero-length span. Even zero length uses an ordinary mortal
///          allocation rather than the shared empty singleton.
/// @param bytes Borrowed source span; may be NULL only when @p len is zero.
/// @param len Number of bytes to copy.
/// @return Newly allocated owned string, or `NULL` after validation,
///         overflow, registry, or allocation failure.
rt_string rt_string_from_bytes(const char *bytes, size_t len) {
    if (len >= SIZE_MAX) {
        rt_trap("rt_string_alloc: length overflow");
        return NULL;
    }
    if (!bytes && len > 0) {
        rt_trap("rt_string_from_bytes: null source with non-zero length");
        return NULL;
    }
    rt_string s = rt_string_alloc(len, len + 1);
    if (!s)
        return NULL;
    if (len > 0 && bytes)
        memcpy(s->data, bytes, len);
    s->data[len] = '\0';
    return s;
}

/// @brief Create a runtime string from a string literal.
/// @details ABI wrapper around @ref rt_string_from_bytes used by generated
///          code. Despite its name, it copies the span into an ordinary mortal
///          string and does not retain a literal pointer.
/// @param bytes Borrowed literal byte span.
/// @param len Number of literal bytes to copy.
/// @return Newly allocated owned copied string, or `NULL` on failure.
rt_string rt_str_from_lit(const char *bytes, size_t len) {
    return rt_string_from_bytes(bytes, len);
}

/// @brief Test whether @p p is a currently registered runtime string.
/// @details Looks up the exact pointer under the registry lock and reads its
///          magic only while protected. Null, the internal tombstone sentinel,
///          absent pointers, and wrong-magic handles return false.
/// @param p Candidate opaque pointer; may be NULL.
/// @return One only for a registered handle with string magic.
int8_t rt_string_is_handle(const void *p) {
    if (!p || p == (const void *)RT_STRING_REG_TOMBSTONE)
        return 0;
    rt_string_registry_lock_();
    int found = rt_string_registry_contains_locked_((rt_string)(uintptr_t)p);
    uint64_t magic = 0;
    if (found)
        magic = ((const rt_string)(uintptr_t)p)->magic;
    rt_string_registry_unlock_();
    if (!found)
        return 0;
    return magic == RT_STRING_MAGIC ? 1 : 0;
}

/// @brief Increment the ownership count for a runtime string handle.
/// @details Literal and embedded (SSO) strings track a reference counter inside
///          the handle (literal_refs), while heap-backed strings delegate to
///          @ref rt_heap_retain. Immortal strings skip reference updates entirely.
///          Mortal inline counters are atomically incremented without crossing
///          the reserved immortal range. Invalid/released handles and count
///          overflow trap.
/// @param s Borrowed live handle to retain; may be NULL.
/// @return The same handle with one additional owned reference, NULL for null
///         input, or `NULL` after a returning retain trap.
rt_string rt_string_ref(rt_string s) {
    if (!s)
        return NULL;
    if (!rt_string_require_handle_(s, "rt_string_ref: invalid string handle"))
        return NULL;
    if (!s->heap || s->heap == RT_SSO_SENTINEL) {
        // Atomic increment for thread-safe reference counting. Do not allow a
        // mortal SSO/literal string to step into the immortal refcount sentinel.
        size_t old = __atomic_load_n(&s->literal_refs, __ATOMIC_RELAXED);
        for (;;) {
            if (old == 0) {
                rt_trap("rt_string_ref: retain after release");
                return NULL;
            }
            if (old >= kImmortalRefcnt)
                return s;
            if (old >= RT_HEAP_MAX_MORTAL_REFCNT) {
                rt_trap("refcount overflow");
                return NULL;
            }
            size_t next = old + 1;
            if (__atomic_compare_exchange_n(&s->literal_refs,
                                            &old,
                                            next,
                                            /*weak=*/0,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                break;
            }
        }
        return s;
    }
    rt_heap_hdr_t *hdr = rt_string_registered_header_(s);
    if (!hdr)
        return NULL;
    if (rt_string_is_immortal_hdr(hdr))
        return s;
    int32_t retained = rt_heap_try_retain_live(s->data);
    if (retained == 1 || retained == 2)
        return s;
    if (retained < 0)
        rt_trap("refcount overflow");
    else
        rt_trap("rt_string_ref: invalid or released heap storage");
    return NULL;
}

/// @brief Release a reference to a runtime string handle and return the post-release count.
/// @details Mirrors @ref rt_string_ref by decrementing literal/embedded reference
///          counts or calling @ref rt_heap_release for heap-backed strings. When
///          the final reference disappears the wrapper structure is freed. For
///          embedded (SSO) strings, this frees the combined handle+data allocation.
///          Final release uses acquire/release ordering, clears weak references,
///          and unregisters before freeing. Immortal and null handles are
///          unchanged.
/// @param s Owned live reference to release; may be NULL.
/// @return Count immediately after release, zero for null/final/already-zero
///         state, or `SIZE_MAX` for immortal storage.
size_t rt_string_unref_count(rt_string s) {
    if (!s)
        return 0;
    if (!rt_string_require_handle_(s, "rt_string_unref: invalid string handle"))
        return 0;
    if (!s->heap || s->heap == RT_SSO_SENTINEL) {
        // Atomic decrement for thread-safe reference counting
        // Skip immortal literals (SIZE_MAX indicates immortal) and already-zero refs
#if RT_COMPILER_MSVC
        size_t old = rt_atomic_load_size(&s->literal_refs, __ATOMIC_RELAXED);
        if (old == 0)
            return 0;
        if (old >= kImmortalRefcnt)
            return SIZE_MAX;
        // Use fetch_sub which returns old value; if old was 1, we decremented to 0
        size_t prev = rt_atomic_fetch_sub_size(&s->literal_refs, 1, __ATOMIC_RELEASE);
        if (prev == 1) {
            // We held the last reference - ensure all writes are visible before free
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            rt_gc_clear_weak_refs(s);
            rt_string_unregister_handle(s);
            free(s);
            return 0;
        }
        return prev - 1;
#else
        size_t old = __atomic_load_n(&s->literal_refs, __ATOMIC_RELAXED);
        if (old == 0)
            return 0;
        if (old >= kImmortalRefcnt)
            return SIZE_MAX;
        // Use fetch_sub which returns old value; if old was 1, we decremented to 0
        size_t prev = __atomic_fetch_sub(&s->literal_refs, 1, __ATOMIC_RELEASE);
        if (prev == 1) {
            // We held the last reference - ensure all writes are visible before free
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            rt_gc_clear_weak_refs(s);
            rt_string_unregister_handle(s);
            free(s);
            return 0;
        }
        return prev - 1;
#endif
    }
    rt_heap_hdr_t *hdr = rt_string_registered_header_(s);
    if (!hdr)
        return 0;
    if (rt_string_is_immortal_hdr(hdr))
        return SIZE_MAX;
    size_t next = rt_heap_release_deferred(s->data);
    if (next == 0) {
        char *payload = s->data;
        rt_gc_clear_weak_refs(s);
        rt_string_unregister_handle(s);
        s->data = NULL;
        s->heap = NULL;
        rt_heap_free_zero_ref(payload);
        free(s);
    }
    return next;
}

/// @brief Release a reference to a runtime string handle.
/// @details Discards the post-release count from
///          @ref rt_string_unref_count. Null and immortal handles are no-ops.
/// @param s Owned live reference to release; may be NULL.
void rt_string_unref(rt_string s) {
    (void)rt_string_unref_count(s);
}

/// @brief Convenience wrapper that releases a possibly-null string handle.
/// @details Present to match historical runtime entry points. Delegates to
///          @ref rt_string_unref.
/// @param s Owned live reference to release; may be NULL.
void rt_str_release_maybe(rt_string s) {
    rt_string_unref(s);
}

/// @brief Convenience wrapper that retains a possibly-null string handle.
/// @details Provides parity with `rt_str_release_maybe` and ignores null
///          handles while preserving the return value from @ref rt_string_ref.
/// @param s Borrowed live handle to retain; may be NULL.
void rt_str_retain_maybe(rt_string s) {
    (void)rt_string_ref(s);
}

/// @brief Obtain the shared empty string handle.
/// @details Calls @ref rt_empty_string to lazily construct and cache the
///          immortal empty string instance.
/// @return Shared immortal empty handle, or `NULL` on initialization failure.
rt_string rt_str_empty(void) {
    return rt_empty_string();
}

/// @brief Return the BASIC-visible length of a string.
/// @details Delegates to the byte-count helper and exposes the value as a signed
///          64-bit integer to match the runtime ABI, saturating oversized
///          `size_t` lengths at `INT64_MAX`. Null is empty.
/// @param s Borrowed runtime string; may be NULL.
/// @return Stored byte length clamped to `INT64_MAX`.
int64_t rt_str_len(rt_string s) {
    size_t len = rt_string_len_bytes(s);
    if (len > (size_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)len;
}

/// @brief Return one unsigned byte from a runtime string without allocation.
/// @details Out-of-range offsets return -1 so bounded scanners can use a
///          sentinel. A non-null forged handle traps consistently with other
///          string accessors; null is treated as an empty string.
/// @param s Borrowed string handle.
/// @param offset Zero-based stored-byte offset.
/// @return Byte value in 0..255, or -1 when no byte exists at @p offset.
int64_t rt_str_byte_at(rt_string s, int64_t offset) {
    if (!s || offset < 0)
        return -1;
    if (!rt_string_require_handle_(s, "String.ByteAt: invalid string handle"))
        return -1;
    const size_t len = rt_string_len_bytes(s);
    if ((uint64_t)offset >= (uint64_t)len)
        return -1;
    return (int64_t)(unsigned char)s->data[(size_t)offset];
}

/// @brief Return 1 when the runtime string is empty; 0 otherwise.
/// @details Null handles are treated as empty to match rt_str_len semantics.
/// @param s Borrowed runtime string; may be NULL.
/// @return One when @ref rt_str_len returns zero; otherwise zero.
int64_t rt_str_is_empty(rt_string s) {
    return rt_str_len(s) == 0 ? 1 : 0;
}

/// @brief Retaining constructor from an existing runtime string handle.
/// @details Used as a thin shim for Zanna.String.FromStr. The runtime string
///          return ABI transfers an owned reference to callers, so this helper
///          retains before returning even though the underlying handle is shared.
/// @param s Borrowed live runtime string; may be NULL.
/// @return The same handle with an additional owned reference, or NULL.
rt_string rt_str_clone(rt_string s) {
    return rt_string_ref(s);
}

/// @brief Concatenate two runtime strings, consuming the inputs.
/// @details Treats null operands as zero bytes. A uniquely owned, mortal,
///          heap-backed @p a with sufficient capacity is extended in place;
///          otherwise a new string is allocated. Equal pointers take a separate
///          copy path because two operand claims may refer to one handle.
///
///          After successful length validation, both input claims are consumed
///          on success and allocation failure; reused @p a becomes the returned
///          claim. Length overflow traps before either claim is consumed.
/// @param a Owned left operand claim; may be NULL.
/// @param b Owned right operand claim; may be NULL.
/// @return Owned concatenation, possibly reused @p a, or `NULL` on overflow or
///         allocation failure.
rt_string rt_str_concat(rt_string a, rt_string b) {
    if ((a && !rt_string_require_handle_(a, "rt_str_concat: invalid string handle")) ||
        (b && b != a && !rt_string_require_handle_(b, "rt_str_concat: invalid string handle")))
        return NULL;
    size_t len_a = rt_string_len_bytes(a);
    size_t len_b = rt_string_len_bytes(b);
    if (len_a > SIZE_MAX - len_b) {
        rt_trap("rt_str_concat: length overflow");
        return NULL;
    }
    size_t total = len_a + len_b;
    if (total == SIZE_MAX) {
        rt_trap("rt_str_concat: length overflow");
        return NULL;
    }

    if (a && a == b) {
        rt_string out = rt_string_alloc(total, total + 1);
        if (!out) {
            size_t remaining = rt_string_unref_count(a);
            if (remaining > 0 && remaining != SIZE_MAX)
                rt_string_unref(a);
            return NULL;
        }
        if (len_a > 0)
            memcpy(out->data, a->data, len_a);
        if (len_b > 0)
            memcpy(out->data + len_a, a->data, len_b);
        out->data[total] = '\0';
        size_t remaining = rt_string_unref_count(a);
        if (remaining > 0 && remaining != SIZE_MAX)
            rt_string_unref(a);
        return out;
    }

    // Optimization: append in-place when `a` is uniquely owned with enough capacity
    if (a && rt_string_can_append_inplace(a, total + 1)) {
        // Append `b` directly into `a`'s buffer
        if (b && b->data && len_b > 0)
            memcpy(a->data + len_a, b->data, len_b);
        a->data[total] = '\0';
        // Update the length in the heap header
        rt_heap_set_len(a->data, total);
        // Release `b` (consumed by concat)
        if (b)
            rt_string_unref(b);
        // Return `a` reused (not released, ownership transferred)
        return a;
    }

    rt_string out = rt_string_alloc(total, total + 1);
    if (!out) {
        if (a)
            rt_string_unref(a);
        if (b)
            rt_string_unref(b);
        return NULL;
    }

    if (a && a->data && len_a > 0)
        memcpy(out->data, a->data, len_a);
    if (b && b->data && len_b > 0)
        memcpy(out->data + len_a, b->data, len_b);

    out->data[total] = '\0';

    if (a)
        rt_string_unref(a);
    if (b)
        rt_string_unref(b);

    return out;
}

/// @brief Extract a substring using zero-based start and length.
/// @details Operates on bytes. Negative values become zero and both range
///          endpoints clamp to the stored length. An empty slice returns the
///          immortal singleton, a complete slice retains @p s, and a partial
///          slice copies bytes. Null input traps and uses the empty fallback.
/// @param s Borrowed source string; must be non-null.
/// @param start Zero-based byte offset; negative values become zero.
/// @param len Requested byte count; negative values become zero.
/// @return Owned slice, retained source, or empty singleton; possibly `NULL`
///         after allocation/singleton failure.
rt_string rt_str_substr(rt_string s, int64_t start, int64_t len) {
    if (!s) {
        rt_trap("rt_str_substr: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_substr: invalid string handle"))
        return rt_empty_string();
    if (start < 0)
        start = 0;
    if (len < 0)
        len = 0;
    size_t slen = rt_string_len_bytes(s);
    if ((uint64_t)start > slen)
        start = (int64_t)slen;
    size_t start_idx = (size_t)start;
    size_t avail = slen - start_idx;
    uint64_t requested = (uint64_t)len;
    size_t copy_len = requested > SIZE_MAX ? avail : (size_t)requested;
    if (copy_len > avail)
        copy_len = avail;
    if (copy_len == 0)
        return rt_empty_string();
    if (start_idx == 0 && copy_len == slen)
        return rt_string_ref(s);
    return rt_string_from_bytes(s->data + start_idx, copy_len);
}

/// @brief Implement BASIC's `LEFT$` intrinsic.
/// @details Uses a byte count rather than Unicode codepoints. Zero returns the
///          empty singleton, a count at least the source length retains the
///          source, and an intermediate count copies a prefix. Null source or
///          negative count traps with an empty fallback.
/// @param s Borrowed source string; must be non-null.
/// @param n Number of leading bytes to return.
/// @return Owned prefix, retained source, or empty fallback.
rt_string rt_str_left(rt_string s, int64_t n) {
    if (!s) {
        rt_trap("LEFT$: null string");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "LEFT$: invalid string handle"))
        return rt_empty_string();
    if (n < 0) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(n, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "LEFT$: len must be >= 0 (got %s)", numbuf);
        rt_trap(buf);
        return rt_empty_string();
    }
    size_t slen = rt_string_len_bytes(s);
    if (n == 0)
        return rt_empty_string();
    uint64_t requested = (uint64_t)n;
    if (requested > SIZE_MAX)
        return rt_string_ref(s);
    size_t take = (size_t)requested;
    if (take >= slen)
        return rt_string_ref(s);
    return rt_str_substr(s, 0, n);
}

/// @brief Implement BASIC's `RIGHT$` intrinsic.
/// @details Mirrors @ref rt_str_left byte-count validation and ownership, but
///          computes the partial slice from the stored-byte end.
/// @param s Borrowed source string; must be non-null.
/// @param n Number of trailing bytes to return.
/// @return Owned suffix, retained source, or empty fallback.
rt_string rt_str_right(rt_string s, int64_t n) {
    if (!s) {
        rt_trap("RIGHT$: null string");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "RIGHT$: invalid string handle"))
        return rt_empty_string();
    if (n < 0) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(n, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "RIGHT$: len must be >= 0 (got %s)", numbuf);
        rt_trap(buf);
        return rt_empty_string();
    }
    size_t len = rt_string_len_bytes(s);
    if (n == 0)
        return rt_empty_string();
    uint64_t requested = (uint64_t)n;
    if (requested > SIZE_MAX)
        return rt_string_ref(s);
    size_t take = (size_t)requested;
    if (take >= len)
        return rt_string_ref(s);
    size_t start = len - take;
    return rt_str_substr(s, (int64_t)start, n);
}

// Forward declare UTF-8 helpers used by Mid functions
// utf8_char_to_byte_offset declared in rt_string_internal.h, defined in rt_string_advanced.c

/// @brief Implement BASIC's two-argument `MID$` overload.
/// @details Interprets @p start as a one-based UTF-8 codepoint position and
///          strictly validates sequences traversed to locate it. Position one
///          retains the source without traversal, and a position beyond the
///          codepoint count returns empty. Null or non-positive input traps.
/// @param s Borrowed source string; must be non-null.
/// @param start One-based codepoint position.
/// @return Owned trailing byte slice, retained source, or empty fallback.
rt_string rt_str_mid(rt_string s, int64_t start) {
    if (!s) {
        rt_trap("MID$: null string");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "MID$: invalid string handle"))
        return rt_empty_string();
    if (start < 1) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(start, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "MID$: start must be >= 1 (got %s)", numbuf);
        rt_trap(buf);
        return rt_empty_string();
    }
    size_t byte_len = rt_string_len_bytes(s);
    if (start == 1)
        return rt_string_ref(s);
    const char *data = s->data;
    size_t byte_off = utf8_char_to_byte_offset(data, byte_len, start);
    if (byte_off >= byte_len)
        return rt_empty_string();
    size_t n = byte_len - byte_off;
    return rt_str_substr(s, (int64_t)byte_off, (int64_t)n);
}

/// @brief Implement BASIC's three-argument `MID$` overload.
/// @details Converts both start and saturated end positions from one-based
///          strict UTF-8 codepoint indices to byte offsets. Zero length or a
///          start beyond the source returns empty; a complete range retains the
///          source. Null, non-positive start, and negative length trap.
/// @param s Borrowed source string; must be non-null.
/// @param start One-based UTF-8 codepoint position.
/// @param len Number of codepoints requested.
/// @return Owned byte slice, retained source, or empty fallback.
rt_string rt_str_mid_len(rt_string s, int64_t start, int64_t len) {
    if (!s) {
        rt_trap("MID$: null string");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "MID$: invalid string handle"))
        return rt_empty_string();
    if (start < 1) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(start, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "MID$: start must be >= 1 (got %s)", numbuf);
        rt_trap(buf);
        return rt_empty_string();
    }
    if (len < 0) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(len, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "MID$: len must be >= 0 (got %s)", numbuf);
        rt_trap(buf);
        return rt_empty_string();
    }
    size_t byte_len = rt_string_len_bytes(s);
    if (len == 0)
        return rt_empty_string();
    const char *data = s->data;
    size_t byte_start = utf8_char_to_byte_offset(data, byte_len, start);
    if (byte_start >= byte_len)
        return rt_empty_string();
    // Find the byte offset of (start + len) to compute the byte length. Saturate
    // the end position so very large lengths clamp to the source end without
    // signed-overflow UB.
    int64_t end_pos = INT64_MAX;
    if (len <= INT64_MAX - start)
        end_pos = start + len;
    size_t byte_end = utf8_char_to_byte_offset(data, byte_len, end_pos);
    if (byte_end < byte_start)
        byte_end = byte_start;
    size_t byte_count = byte_end - byte_start;
    if (byte_start == 0 && byte_end >= byte_len)
        return rt_string_ref(s);
    return rt_str_substr(s, (int64_t)byte_start, (int64_t)byte_count);
}

/// @brief Search for a substring using zero-based indexing.
/// @details Implements shared non-empty-needle search for the INSTR family.
///          Clamps negative/oversized byte starts, uses `memchr` to locate the
///          first needle byte, and verifies candidates with `memcmp`. Null
///          operands miss.
/// @param hay Borrowed haystack; may be NULL.
/// @param start Zero-based starting byte offset.
/// @param needle Borrowed non-empty byte sequence; may be NULL.
/// @return One-based index of the first match, or zero when absent.
static int64_t rt_find(rt_string hay, int64_t start, rt_string needle) {
    if (!hay || !needle)
        return 0;
    if (start < 0)
        start = 0;
    size_t hay_len = rt_string_len_bytes(hay);
    size_t needle_len = rt_string_len_bytes(needle);
    if ((uint64_t)start > hay_len)
        start = (int64_t)hay_len;
    size_t start_idx = (size_t)start;
    if (needle_len > hay_len - start_idx)
        return 0;

    // Optimized substring search using memchr for first-character scan.
    // memchr is typically SIMD-optimized and much faster than byte-by-byte scanning.
    // This reduces the naive O(n*m) to O(n) for the first-character search,
    // only falling back to memcmp when we find a potential match.
    const char first = needle->data[0];
    const char *pos = hay->data + start_idx;
    const char *end = hay->data + hay_len - needle_len + 1;

    while (pos < end) {
        pos = memchr(pos, first, (size_t)(end - pos));
        if (!pos)
            return 0;
        if (memcmp(pos, needle->data, needle_len) == 0)
            return (int64_t)(pos - hay->data + 1);
        ++pos;
    }
    return 0;
}

/// @brief Implement BASIC's two-argument `INSTR` intrinsic.
/// @details Searches stored bytes from the beginning. An empty non-null needle
///          matches at one; a null operand returns zero.
/// @param hay Borrowed haystack; may be NULL.
/// @param needle Borrowed needle; may be NULL.
/// @return One-based byte index of the first match, or zero when absent.
int64_t rt_str_index_of(rt_string hay, rt_string needle) {
    if (!hay || !needle)
        return 0;
    if (!rt_string_require_handle_(hay, "rt_str_index_of: invalid string handle") ||
        (needle != hay &&
         !rt_string_require_handle_(needle, "rt_str_index_of: invalid string handle")))
        return 0;
    size_t needle_len = rt_string_len_bytes(needle);
    if (needle_len == 0)
        return 1;
    return rt_find(hay, 0, needle);
}

/// @brief Implement BASIC's three-argument `INSTR` intrinsic.
/// @details Converts the one-based byte start to a clamped zero-based offset.
///          Values at/below one begin at byte zero; values beyond the source
///          clamp to its end. An empty needle matches at the resulting
///          one-based position. Null operands miss.
/// @param start One-based starting byte position.
/// @param hay Borrowed haystack; may be NULL.
/// @param needle Borrowed needle; may be NULL.
/// @return One-based byte index of the first match, or zero when absent.
int64_t rt_str_instr3(int64_t start, rt_string hay, rt_string needle) {
    if (!hay || !needle)
        return 0;
    if (!rt_string_require_handle_(hay, "rt_str_instr3: invalid string handle") ||
        (needle != hay &&
         !rt_string_require_handle_(needle, "rt_str_instr3: invalid string handle")))
        return 0;
    size_t len = rt_string_len_bytes(hay);
    int64_t pos = start <= 1 ? 0 : start - 1;
    if ((uint64_t)pos > len)
        pos = (int64_t)len;
    size_t needle_len = rt_string_len_bytes(needle);
    if (needle_len == 0)
        return pos + 1;
    return rt_find(hay, pos, needle);
}

/// @brief Find the first occurrence of @p needle in @p hay starting at a one-based position.
/// @details This is the Zanna.String.IndexOfFrom ABI entry point.  It intentionally
///          preserves BASIC/INSTR semantics: @p start is one-based, successful
///          matches are one-based, and zero means "not found".
/// @param hay Borrowed haystack; may be NULL.
/// @param start One-based starting byte position.
/// @param needle Borrowed needle; may be NULL.
/// @return Result of @ref rt_str_instr3 with receiver-first argument order.
int64_t rt_str_index_of_from(rt_string hay, int64_t start, rt_string needle) {
    return rt_str_instr3(start, hay, needle);
}

/// @brief Whitespace predicate for `rt_str_trim*` — matches space, tab, CR, LF, vertical tab, form
/// feed.
/// @param c Byte to classify.
/// @return One for one of the six ASCII trim bytes; otherwise zero.
static int is_trim_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// @brief Trim leading ASCII whitespace from a string.
/// @details Removes the maximal prefix recognized by @ref is_trim_ws and
///          delegates byte slicing to @ref rt_str_substr. A fully untrimmed
///          source is retained; a fully trimmed source yields the empty
///          singleton. Null traps.
/// @param s Borrowed source string; must be non-null.
/// @return Owned trimmed slice, retained source, or empty fallback.
rt_string rt_str_ltrim(rt_string s) {
    if (!s) {
        rt_trap("rt_str_ltrim: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_ltrim: invalid string handle"))
        return rt_empty_string();
    size_t slen = rt_string_len_bytes(s);
    size_t i = 0;
    while (i < slen && is_trim_ws(s->data[i]))
        ++i;
    return rt_str_substr(s, (int64_t)i, (int64_t)(slen - i));
}

/// @brief Trim trailing ASCII whitespace from a string.
/// @details Removes the maximal suffix recognized by @ref is_trim_ws and
///          applies the allocation/retention rules of @ref rt_str_substr. Null
///          traps.
/// @param s Borrowed source string; must be non-null.
/// @return Owned trimmed slice, retained source, or empty fallback.
rt_string rt_str_rtrim(rt_string s) {
    if (!s) {
        rt_trap("rt_str_rtrim: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_rtrim: invalid string handle"))
        return rt_empty_string();
    size_t end = rt_string_len_bytes(s);
    while (end > 0 && is_trim_ws(s->data[end - 1]))
        --end;
    return rt_str_substr(s, 0, (int64_t)end);
}

/// @brief Trim both leading and trailing ASCII whitespace from a string.
/// @details Removes maximal prefixes/suffixes of space, tab, CR, LF, vertical
///          tab, and form feed, then returns the corresponding byte slice.
///          Null input traps.
/// @param s Borrowed source string; must be non-null.
/// @return Owned trimmed slice, retained source, or empty fallback.
rt_string rt_str_trim(rt_string s) {
    if (!s) {
        rt_trap("rt_str_trim: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_trim: invalid string handle"))
        return rt_empty_string();
    size_t slen = rt_string_len_bytes(s);
    size_t start = 0;
    size_t end = slen;
    while (start < end && is_trim_ws(s->data[start]))
        ++start;
    while (end > start && is_trim_ws(s->data[end - 1]))
        --end;
    return rt_str_substr(s, (int64_t)start, (int64_t)(end - start));
}

/// @brief Convert a single ASCII byte to uppercase.
/// @details Maps only `a-z`; despite the legacy helper name, no Latin-1
///          high-byte case mapping is performed.
/// @param c Input byte.
/// @return Uppercase equivalent or original byte.
static unsigned char to_upper_latin1(unsigned char c) {
    // ASCII lowercase a-z
    if (c >= 'a' && c <= 'z')
        return (unsigned char)(c - 'a' + 'A');
    return c;
}

/// @brief Convert ASCII letters in a string to upper case.
/// @details Allocates an equal-length copy, maps ASCII `a-z`, and preserves all
///          other bytes including embedded NUL and malformed/high-bit data.
///          Null traps and falls back to the empty singleton.
/// @param s Borrowed source string; must be non-null.
/// @return Newly allocated owned uppercase byte string, empty fallback, or
///         `NULL` on allocation failure.
rt_string rt_str_ucase(rt_string s) {
    if (!s) {
        rt_trap("rt_str_ucase: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_ucase: invalid string handle"))
        return rt_empty_string();
    size_t len = rt_string_len_bytes(s);
    rt_string r = rt_string_alloc(len, len + 1);
    if (!r)
        return NULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        // Skip UTF-8 continuation bytes (10xxxxxx) and multi-byte lead bytes
        if ((c & 0x80) == 0) {
            // ASCII byte - apply case conversion
            c = to_upper_latin1(c);
        }
        // Multi-byte UTF-8 sequences pass through unchanged
        r->data[i] = (char)c;
    }
    r->data[len] = '\0';
    return r;
}

/// @brief Convert a single ASCII byte to lowercase.
/// @details Maps only `A-Z`; despite the legacy helper name, no Latin-1
///          high-byte case mapping is performed.
/// @param c Input byte.
/// @return Lowercase equivalent or original byte.
static unsigned char to_lower_latin1(unsigned char c) {
    // ASCII uppercase A-Z
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    return c;
}

/// @brief Convert ASCII letters in a string to lower case.
/// @details Allocates an equal-length copy, maps ASCII `A-Z`, and preserves all
///          other bytes including embedded NUL and malformed/high-bit data.
///          Null traps and falls back to the empty singleton.
/// @param s Borrowed source string; must be non-null.
/// @return Newly allocated owned lowercase byte string, empty fallback, or
///         `NULL` on allocation failure.
rt_string rt_str_lcase(rt_string s) {
    if (!s) {
        rt_trap("rt_str_lcase: null");
        return rt_empty_string();
    }
    if (!rt_string_require_handle_(s, "rt_str_lcase: invalid string handle"))
        return rt_empty_string();
    size_t len = rt_string_len_bytes(s);
    rt_string r = rt_string_alloc(len, len + 1);
    if (!r)
        return NULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        // Skip UTF-8 multi-byte sequences
        if ((c & 0x80) == 0) {
            // ASCII byte - apply case conversion
            c = to_lower_latin1(c);
        }
        // Multi-byte UTF-8 sequences pass through unchanged
        r->data[i] = (char)c;
    }
    r->data[len] = '\0';
    return r;
}

/// @brief Compare two runtime strings for equality.
/// @details Performs pointer short-circuiting, length comparison, and a byte
///          comparison. Any null operand returns false, including two nulls.
/// @param a Borrowed first operand; may be NULL.
/// @param b Borrowed second operand; may be NULL.
/// @return One for equal non-null byte strings; otherwise zero.
int8_t rt_str_eq(rt_string a, rt_string b) {
    if (!a || !b)
        return 0;
    if (!rt_string_require_handle_(a, "rt_str_eq: invalid string handle") ||
        (b != a && !rt_string_require_handle_(b, "rt_str_eq: invalid string handle")))
        return 0;
    if (a == b)
        return 1;
    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    if (alen != blen)
        return 0;
    return memcmp(a->data, b->data, alen) == 0;
}

// ---------------------------------------------------------------------------
// String comparison operators — `<`, `<=`, `>`, `>=` over the
// rt_string type. Each compares byte-wise (memcmp) so they match
// the natural lexicographic ordering of UTF-8 strings.
// ---------------------------------------------------------------------------

/// @brief Test whether @p a is lexicographically less than @p b.
/// @details Uses unsigned-byte ordering from `memcmp`, then stored length for
///          equal prefixes. Any null operand returns false.
/// @param a Borrowed first operand; may be NULL.
/// @param b Borrowed second operand; may be NULL.
/// @return One only when both operands are non-null and @p a sorts first.
int64_t rt_str_lt(rt_string a, rt_string b) {
    if (!a || !b)
        return 0;
    if (!rt_string_require_handle_(a, "rt_str_lt: invalid string handle") ||
        (b != a && !rt_string_require_handle_(b, "rt_str_lt: invalid string handle")))
        return 0;
    if (a == b)
        return 0;
    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;
    int cmp = memcmp(a->data, b->data, minlen);
    if (cmp != 0)
        return cmp < 0;
    return alen < blen;
}

/// @brief Test whether @p a is lexicographically less than or equal to @p b.
/// @details Uses byte/length ordering. Two nulls compare true; exactly one null
///          compares false.
/// @param a Borrowed first operand; may be NULL.
/// @param b Borrowed second operand; may be NULL.
/// @return Boolean comparison result.
int64_t rt_str_le(rt_string a, rt_string b) {
    if (!a || !b)
        return a == b;
    if (!rt_string_require_handle_(a, "rt_str_le: invalid string handle") ||
        (b != a && !rt_string_require_handle_(b, "rt_str_le: invalid string handle")))
        return 0;
    if (a == b)
        return 1;
    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;
    int cmp = memcmp(a->data, b->data, minlen);
    if (cmp != 0)
        return cmp < 0;
    return alen <= blen;
}

/// @brief Test whether @p a is lexicographically greater than @p b.
/// @details Uses unsigned-byte and length ordering. Any null operand returns
///          false.
/// @param a Borrowed first operand; may be NULL.
/// @param b Borrowed second operand; may be NULL.
/// @return Boolean comparison result.
int64_t rt_str_gt(rt_string a, rt_string b) {
    if (!a || !b)
        return 0;
    if (!rt_string_require_handle_(a, "rt_str_gt: invalid string handle") ||
        (b != a && !rt_string_require_handle_(b, "rt_str_gt: invalid string handle")))
        return 0;
    if (a == b)
        return 0;
    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;
    int cmp = memcmp(a->data, b->data, minlen);
    if (cmp != 0)
        return cmp > 0;
    return alen > blen;
}

/// @brief Test whether @p a is lexicographically greater than or equal to @p b.
/// @details Uses byte/length ordering. Two nulls compare true; exactly one null
///          compares false.
/// @param a Borrowed first operand; may be NULL.
/// @param b Borrowed second operand; may be NULL.
/// @return Boolean comparison result.
int64_t rt_str_ge(rt_string a, rt_string b) {
    if (!a || !b)
        return a == b;
    if (!rt_string_require_handle_(a, "rt_str_ge: invalid string handle") ||
        (b != a && !rt_string_require_handle_(b, "rt_str_ge: invalid string handle")))
        return 0;
    if (a == b)
        return 1;
    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;
    int cmp = memcmp(a->data, b->data, minlen);
    if (cmp != 0)
        return cmp > 0;
    return alen >= blen;
}
