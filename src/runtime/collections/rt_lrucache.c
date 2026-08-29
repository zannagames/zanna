//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_lrucache.c
// Purpose: Implements a string-keyed Least Recently Used (LRU) cache combining
//   a hash table for O(1) key lookup with a doubly-linked list maintaining
//   access order. When the cache is full and a new entry is inserted, the least
//   recently accessed entry (tail of the list) is evicted automatically.
//
// Key invariants:
//   - Hash table has initial LRU_INITIAL_BUCKETS (16) buckets with separate
//     chaining, keyed hashes, and growth at 75% load factor.
//   - LRU order is maintained as a doubly-linked list: head = MRU (most
//     recently used), tail = LRU (least recently used, next to evict).
//   - Get promotes the accessed node to the head of the list (O(1)).
//   - Put of an existing key updates the value and promotes to head (O(1)).
//   - Put of a new key when count == max_cap evicts the tail node before insert.
//   - max_cap is fixed at construction; a max_cap of 0 disables eviction (unbounded).
//   - Each node owns a heap-copied key string; values are stored as raw pointers
//     retained by the cache, and released on eviction/removal/finalization.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - LRUCache objects are GC-managed (rt_obj_new_i64). The bucket array and
//     all list nodes are freed by the GC finalizer (lrucache_finalizer).
//
// Links: src/runtime/collections/rt_lrucache.h (public API),
//        src/runtime/collections/rt_hash_util.h (keyed hash helper)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a retained-value least-recently-used cache.
///
/// Each entry participates in both a separately chained string hash table and
/// a doubly linked recency list. The list head is most recently used and the
/// tail is the next eviction candidate. Updating or getting an entry promotes
/// it; peek, has, and enumeration preserve order.
///
/// Keys are copied using complete runtime byte lengths, with a null string
/// denoting the empty key. Values are retained and traced by the runtime
/// collector. Getter results are borrowed, while snapshot Seqs retain their
/// values independently. Mutation is not safe without external synchronization.

#include "rt_lrucache.h"

#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_hash_table_util.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Initial number of hash table buckets.
#define LRU_INITIAL_BUCKETS 16

#include "rt_hash_util.h"

/// @brief Install a non-local recovery target for the current thread.
/// @param buf Jump buffer that receives control after a runtime trap.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's active legacy recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the most recent runtime trap diagnostic.
/// @return Null-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

/// @brief Doubly-linked list node with hash table chaining.
typedef struct rt_lru_node {
    char *key;                       ///< Owned copy of key string (null-terminated).
    size_t key_len;                  ///< Length of key string (excluding null terminator).
    void *value;                     ///< Retained reference to the value object.
    struct rt_lru_node *prev;        ///< Previous node in LRU order (toward head/MRU).
    struct rt_lru_node *next;        ///< Next node in LRU order (toward tail/LRU).
    struct rt_lru_node *bucket_next; ///< Next node in hash bucket chain.
} rt_lru_node;

/// @brief LRU cache implementation structure.
typedef struct rt_lrucache_impl {
    void **vptr;           ///< Vtable pointer placeholder (for OOP compatibility).
    rt_lru_node **buckets; ///< Hash table buckets array.
    size_t bucket_count;   ///< Number of hash table buckets.
    size_t count;          ///< Current number of entries.
    size_t max_cap;        ///< Maximum number of entries before eviction.
    rt_lru_node *head;     ///< Most recently used node (doubly-linked list head).
    rt_lru_node *tail;     ///< Least recently used node (doubly-linked list tail).
    int8_t finalizing;     ///< Nonzero while owner teardown rejects reentrant mutation.
} rt_lrucache_impl;

/// @brief Checked cast of an opaque handle to the LruCache implementation.
/// @details Traps with @p what if @p obj is NULL or not an LruCache.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated cache pointer, or NULL if a returning trap handler
///         resumes after failed validation.
static rt_lrucache_impl *as_lrucache(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_LRUCACHE_CLASS_ID, sizeof(rt_lrucache_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_lrucache_impl *)obj;
}

/// @brief Borrow the byte buffer + length of a key string (empty "" if null).
/// @param key Runtime string to inspect; NULL denotes the empty key.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the usable byte length.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping.
static int get_key_data(rt_string key, const char *what, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!key) {
        return 1;
    }
    if (!rt_string_is_handle(key)) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(key);
    if (len <= 0)
        return 1;
    const char *cstr = rt_string_cstr(key);
    if (!cstr) {
        rt_trap(what);
        return 0;
    }
    *out_data = cstr;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Per-process keyed hash for attacker-controlled cache keys.
/// @param key Borrowed key bytes.
/// @param key_len Number of identity bytes.
/// @return Process-keyed 64-bit hash.
static uint64_t lru_hash(const char *key, size_t key_len) {
    return rt_keyed_hash_bytes(key ? key : "", key_len);
}

// ---------------------------------------------------------------------------
// Doubly-linked list helpers
// ---------------------------------------------------------------------------

/// @brief Remove a node from the recency list without freeing it.
/// @param cache Cache whose head and tail may be updated.
/// @param node Linked node to detach.
static void list_remove(rt_lrucache_impl *cache, rt_lru_node *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        cache->head = node->next;

    if (node->next)
        node->next->prev = node->prev;
    else
        cache->tail = node->prev;

    node->prev = NULL;
    node->next = NULL;
}

/// @brief Push a detached node into the most-recently-used position.
/// @param cache Cache receiving the node.
/// @param node Detached node to link at the head.
static void list_push_front(rt_lrucache_impl *cache, rt_lru_node *node) {
    node->prev = NULL;
    node->next = cache->head;

    if (cache->head)
        cache->head->prev = node;
    else
        cache->tail = node; // List was empty

    cache->head = node;
}

/// @brief Promote an existing node to the most-recently-used position.
/// @param cache Cache containing @p node.
/// @param node Linked node to promote.
static void list_move_to_front(rt_lrucache_impl *cache, rt_lru_node *node) {
    if (cache->head == node)
        return; // Already at front
    list_remove(cache, node);
    list_push_front(cache, node);
}

// ---------------------------------------------------------------------------
// Hash table helpers
// ---------------------------------------------------------------------------

/// @brief Find a byte-exact key in a bucket collision chain.
/// @param head First node in the chain.
/// @param key Key bytes to compare.
/// @param key_len Number of bytes in @p key.
/// @return Matching node, or NULL when absent.
static rt_lru_node *bucket_find(rt_lru_node *head, const char *key, size_t key_len) {
    for (rt_lru_node *n = head; n; n = n->bucket_next) {
        if (n->key_len == key_len && memcmp(n->key, key, key_len) == 0)
            return n;
    }
    return NULL;
}

/// @brief Remove a node from its hash bucket without freeing it.
/// @param cache Cache whose bucket chain is updated.
/// @param node Node to unlink.
static void bucket_remove(rt_lrucache_impl *cache, rt_lru_node *node) {
    uint64_t hash = lru_hash(node->key, node->key_len);
    size_t idx = hash % cache->bucket_count;

    rt_lru_node **prev_ptr = &cache->buckets[idx];
    rt_lru_node *curr = cache->buckets[idx];

    while (curr) {
        if (curr == node) {
            *prev_ptr = curr->bucket_next;
            curr->bucket_next = NULL;
            return;
        }
        prev_ptr = &curr->bucket_next;
        curr = curr->bucket_next;
    }
}

/// @brief Insert a detached node at the head of its hash bucket.
/// @param cache Cache receiving the node.
/// @param node Initialized node with owned key bytes.
static void bucket_insert(rt_lrucache_impl *cache, rt_lru_node *node) {
    uint64_t hash = lru_hash(node->key, node->key_len);
    size_t idx = hash % cache->bucket_count;
    node->bucket_next = cache->buckets[idx];
    cache->buckets[idx] = node;
}

/// @brief Resize the hash table when load factor is too high.
/// @param cache Cache whose bucket array is to grow.
/// @return 1 if the table grew or cannot grow further; 0 after trapping on an
///         allocation-size error or allocation failure.
static int resize_buckets(rt_lrucache_impl *cache) {
    if (cache->bucket_count > SIZE_MAX / 2)
        return 1; // Can't grow further; collision chaining remains valid.
    size_t new_bucket_count = cache->bucket_count * 2;
    if (new_bucket_count > SIZE_MAX / sizeof(rt_lru_node *)) {
        rt_trap("LRUCache: allocation size overflow");
        return 0;
    }
    rt_lru_node **new_buckets = (rt_lru_node **)calloc(new_bucket_count, sizeof(rt_lru_node *));
    if (!new_buckets) {
        rt_trap("LRUCache: memory allocation failed");
        return 0;
    }

    // Rehash all nodes
    for (size_t i = 0; i < cache->bucket_count; ++i) {
        rt_lru_node *node = cache->buckets[i];
        while (node) {
            rt_lru_node *next = node->bucket_next;
            uint64_t hash = lru_hash(node->key, node->key_len);
            size_t idx = hash % new_bucket_count;
            node->bucket_next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }

    free(cache->buckets);
    cache->buckets = new_buckets;
    cache->bucket_count = new_bucket_count;
    return 1;
}

/// @brief Grow the bucket table if @p next_count would exceed the load limit.
/// @param cache Cache whose projected occupancy is checked.
/// @param next_count Entry count after the pending insertion and any eviction.
/// @return 1 when insertion may proceed; 0 when growth trapped.
static int maybe_resize_for_count(rt_lrucache_impl *cache, size_t next_count) {
    if (!rt_hash_table_exceeds_load(next_count, cache->bucket_count))
        return 1;
    return resize_buckets(cache);
}

// ---------------------------------------------------------------------------
// Node lifecycle
// ---------------------------------------------------------------------------

/// @brief Free a node and release its owned key and retained value.
/// @param node Detached node to destroy, or NULL for a no-op.
static void free_node(rt_lru_node *node) {
    if (!node)
        return;
    void *value = node->value;
    node->value = NULL;
    free(node->key);
    node->key = NULL;
    free(node);
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Release one runtime object and finalize it at zero.
/// @param obj Runtime-managed object, or NULL.
static void lru_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Save the active trap diagnostic before clearing a recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when the trap subsystem has no text.
static void lru_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Consume a detached recency list despite trapping value finalizers.
/// @details Advances and reclaims each native node before releasing its value.
///          Non-local traps resume cleanup at the next node; the first
///          diagnostic is returned only after the full detached list drains.
/// @param head Detached list head, or NULL.
/// @param error Buffer receiving the first trapped diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return Nonzero when at least one value cleanup trapped.
static int lru_destroy_nodes(rt_lru_node *head, char *error, size_t error_size) {
    rt_lru_node *volatile cursor = head;
    volatile int trapped = 0;
    jmp_buf recovery;

    rt_gc_mutator_enter();
    rt_trap_set_recovery(&recovery);
    for (;;) {
        if (RT_SETJMP(recovery) != 0) {
            rt_gc_mutator_enter();
            if (!trapped)
                lru_save_trap_error(error, error_size, "LRUCache: value finalizer cleanup failed");
            trapped = 1;
        }
        rt_lru_node *node = (rt_lru_node *)cursor;
        if (!node)
            break;
        cursor = node->next;
        free_node(node);
    }
    rt_trap_clear_recovery();
    rt_gc_mutator_exit();
    return trapped ? 1 : 0;
}

/// @brief Append a copied key to an owning snapshot, consuming it on failure.
/// @param seq Partial owning snapshot.
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @return One after append; zero after consuming @p seq and retrapping.
static int lru_append_key_or_release_seq(void *seq, const char *key, size_t key_len) {
    volatile rt_string key_copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        lru_save_trap_error(
            saved_error, sizeof(saved_error), "LRUCache.Keys: snapshot append failed");
        rt_trap_clear_recovery();
        if (key_copy)
            rt_str_release_maybe((rt_string)key_copy);
        lru_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    key_copy = rt_string_from_bytes(key, key_len);
    if (!key_copy)
        rt_trap("LRUCache.Keys: key allocation failed");
    int64_t old_len = rt_seq_len(seq);
    rt_seq_push_raw(seq, (void *)key_copy);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("LRUCache.Keys: snapshot append failed");
    key_copy = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append a retained value to an owning snapshot, consuming it on failure.
/// @param seq Partial owning snapshot.
/// @param value Borrowed cached value; may be NULL.
/// @return One after append; zero after consuming @p seq and retrapping.
static int lru_append_value_or_release_seq(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        lru_save_trap_error(
            saved_error, sizeof(saved_error), "LRUCache.Values: snapshot append failed");
        rt_trap_clear_recovery();
        lru_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    int64_t old_len = rt_seq_len(seq);
    rt_seq_push(seq, value);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("LRUCache.Values: snapshot append failed");
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Detach the least-recently-used node without running callbacks.
/// @param cache Cache to remove from.
/// @return Detached former tail, or NULL when empty.
static rt_lru_node *detach_lru(rt_lrucache_impl *cache) {
    rt_lru_node *victim = cache->tail;
    if (!victim)
        return NULL;

    list_remove(cache, victim);
    bucket_remove(cache, victim);
    cache->count--;
    return victim;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer: free every node by walking the LRU list, then free
///        the bucket array.
/// @param obj LRUCache object being finalized, or NULL for a no-op.
static void rt_lrucache_finalize(void *obj) {
    if (!obj)
        return;
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache: invalid LRUCache object");
    if (!cache)
        return;

    rt_lru_node *nodes = cache->head;
    rt_lru_node **buckets = cache->buckets;
    size_t bucket_count = cache->bucket_count;
    cache->finalizing = 1;
    cache->buckets = NULL;
    cache->bucket_count = 0;
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;

    char cleanup_error[256] = {0};
    int cleanup_trapped = lru_destroy_nodes(nodes, cleanup_error, sizeof(cleanup_error));

    rt_heap_info_t info;
    int resurrected = rt_heap_get_info(obj, &info) && info.refcnt != 0;
    if (resurrected && buckets && bucket_count > 0) {
        memset(buckets, 0, bucket_count * sizeof(rt_lru_node *));
        cache->buckets = buckets;
        cache->bucket_count = bucket_count;
        buckets = NULL;
        rt_obj_set_finalizer(cache, rt_lrucache_finalize);
    }
    cache->finalizing = 0;
    free(buckets);

    if (cleanup_trapped)
        rt_trap(cleanup_error[0] ? cleanup_error : "LRUCache: value finalizer cleanup failed");
}

/// @brief GC traversal: visit every cached value across the node list.
/// @param obj LRUCache object to traverse.
/// @param visitor Runtime callback invoked for every retained value.
/// @param ctx Opaque visitor context forwarded unchanged.
static void rt_lrucache_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache: invalid LRUCache object");
    if (!cache)
        return;
    for (rt_lru_node *node = cache->head; node; node = node->next)
        visitor(node->value, ctx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Creates an empty LRU cache with a fixed eviction limit.
/// @param capacity Maximum resident entries. Zero disables automatic eviction;
///        negative values trap.
/// @return A new runtime-managed LRUCache, or NULL after a size or allocation
///         trap.
void *rt_lrucache_new(int64_t capacity) {
    if (capacity < 0) {
        rt_trap("LRUCache: negative capacity");
        return NULL;
    }
    if ((uint64_t)capacity > SIZE_MAX) {
        rt_trap("LRUCache: capacity is not representable");
        return NULL;
    }

    // Capacity is an eviction limit, not an initial-size request. Empty caches
    // start small and grow with resident entries.
    size_t bucket_count = LRU_INITIAL_BUCKETS;

    if (bucket_count > SIZE_MAX / sizeof(rt_lru_node *)) {
        rt_trap("LRUCache: allocation size overflow");
        return NULL;
    }

    rt_lrucache_impl *cache =
        (rt_lrucache_impl *)rt_obj_new_i64(RT_LRUCACHE_CLASS_ID, (int64_t)sizeof(rt_lrucache_impl));
    if (!cache) {
        rt_trap("LRUCache: memory allocation failed");
        return NULL;
    }

    cache->vptr = NULL;
    cache->bucket_count = bucket_count;
    cache->buckets = (rt_lru_node **)calloc(cache->bucket_count, sizeof(rt_lru_node *));
    if (!cache->buckets) {
        if (rt_obj_release_check0(cache))
            rt_obj_free(cache);
        rt_trap("LRUCache: memory allocation failed");
        return NULL;
    }

    cache->count = 0;
    cache->max_cap = (size_t)capacity;
    cache->head = NULL;
    cache->tail = NULL;
    cache->finalizing = 0;
    rt_obj_set_finalizer(cache, rt_lrucache_finalize);
    rt_gc_track(cache, rt_lrucache_traverse);
    return cache;
}

/// @brief Number of entries currently held by the cache (0..cap).
/// @param obj LRUCache handle, or NULL.
/// @return Current entry count, or zero for NULL.
int64_t rt_lrucache_len(void *obj) {
    if (!obj)
        return 0;
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Len: invalid LRUCache object");
    return cache ? (int64_t)cache->count : 0;
}

/// @brief Maximum capacity (set on construction). When `len() == cap()`, a `_put` evicts.
/// @param obj LRUCache handle, or NULL.
/// @return Fixed eviction limit, or zero for NULL. Zero also denotes an
///         unbounded cache.
int64_t rt_lrucache_cap(void *obj) {
    if (!obj)
        return 0;
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Cap: invalid LRUCache object");
    return cache ? (int64_t)cache->max_cap : 0;
}

/// @brief Returns 1 if the cache holds zero entries.
/// @param obj LRUCache handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_lrucache_is_empty(void *obj) {
    return rt_lrucache_len(obj) == 0;
}

/// @brief Insert or update `key → value`. Existing keys have their value replaced and are
/// promoted to MRU. New insertions evict the LRU entry when at capacity. Both old and new
/// values are reference-counted (old released, new retained). Doubles bucket count when needed.
/// @param obj LRUCache handle, or NULL for a no-op.
/// @param key Key to copy; NULL denotes the empty key.
/// @param value Value to retain; may be NULL.
/// @note A new node is fully allocated before any eviction, preserving the
///       existing cache when preparation fails.
void rt_lrucache_put(void *obj, rt_string key, void *value) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Put: invalid LRUCache object");
    if (!cache) {
        rt_gc_mutator_exit();
        return;
    }
    if (cache->finalizing || !cache->buckets || cache->bucket_count == 0) {
        rt_gc_mutator_exit();
        return;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "LRUCache.Put: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return;
    }
    uint64_t hash = lru_hash(key_data, key_len);
    size_t idx = hash % cache->bucket_count;

    // Check if key already exists
    rt_lru_node *existing = bucket_find(cache->buckets[idx], key_data, key_len);
    if (existing) {
        // Update value and promote to MRU
        void *old_value = existing->value;
        if (!rt_collection_retain_checked(value, "LRUCache.Put: value retain failed")) {
            rt_gc_mutator_exit();
            return;
        }
        existing->value = value;
        list_move_to_front(cache, existing);
        rt_gc_mutator_exit();
        lru_release_object(old_value);
        return;
    }

    if ((cache->max_cap == 0 || cache->count < cache->max_cap) &&
        cache->count >= (size_t)INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("LRUCache.Put: maximum size reached");
        return;
    }

    size_t projected_count =
        (cache->max_cap != 0 && cache->count >= cache->max_cap) ? cache->count : cache->count + 1;
    if (!maybe_resize_for_count(cache, projected_count)) {
        rt_gc_mutator_exit();
        return;
    }

    if (!rt_collection_retain_checked(value, "LRUCache.Put: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }

    // Create new node
    rt_lru_node *node = (rt_lru_node *)malloc(sizeof(rt_lru_node));
    if (!node) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        rt_gc_mutator_exit();
        rt_trap("LRUCache: memory allocation failed");
        return;
    }

    if (key_len == SIZE_MAX) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        free(node);
        rt_gc_mutator_exit();
        rt_trap("LRUCache: key allocation overflow");
        return;
    }
    node->key = (char *)malloc(key_len + 1);
    if (!node->key) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        free(node);
        rt_gc_mutator_exit();
        rt_trap("LRUCache: key allocation failed");
        return;
    }
    memcpy(node->key, key_data, key_len);
    node->key[key_len] = '\0';
    node->key_len = key_len;

    node->value = value;
    node->prev = NULL;
    node->next = NULL;
    node->bucket_next = NULL;

    // Commit eviction and insertion as one mutation before releasing the
    // displaced value. Its finalizer may re-enter the completed cache.
    rt_lru_node *evicted = NULL;
    if (cache->max_cap != 0 && cache->count >= cache->max_cap)
        evicted = detach_lru(cache);

    // Insert into hash table and linked list
    bucket_insert(cache, node);
    list_push_front(cache, node);
    cache->count++;
    rt_gc_mutator_exit();
    free_node(evicted);
}

/// @brief Look up `key` and promote it to MRU. Returns the borrowed value pointer or NULL if
/// absent. Caller must NOT keep the pointer past the next cache mutation (it may be evicted).
/// @param obj LRUCache handle, or NULL.
/// @param key Key to find; NULL denotes the empty key.
/// @return Borrowed mapped value, or NULL if absent.
void *rt_lrucache_get(void *obj, rt_string key) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Get: invalid LRUCache object");
    if (!cache || !cache->buckets || cache->bucket_count == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "LRUCache.Get: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return NULL;
    }
    uint64_t hash = lru_hash(key_data, key_len);
    size_t idx = hash % cache->bucket_count;

    rt_lru_node *node = bucket_find(cache->buckets[idx], key_data, key_len);
    if (!node) {
        rt_gc_mutator_exit();
        return NULL;
    }

    // Promote to MRU
    list_move_to_front(cache, node);
    // Returns borrowed reference — caller must not store past next cache mutation
    void *value = node->value;
    rt_gc_mutator_exit();
    return value;
}

/// @brief Look up `key` *without* changing LRU order. Use for "is this cached?" probes that
/// shouldn't fight the eviction policy.
/// @param obj LRUCache handle, or NULL.
/// @param key Key to find; NULL denotes the empty key.
/// @return Borrowed mapped value, or NULL if absent.
void *rt_lrucache_peek(void *obj, rt_string key) {
    if (!obj)
        return NULL;

    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Peek: invalid LRUCache object");
    if (!cache || !cache->buckets || cache->bucket_count == 0)
        return NULL;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "LRUCache.Peek: invalid key", &key_data, &key_len))
        return NULL;
    uint64_t hash = lru_hash(key_data, key_len);
    size_t idx = hash % cache->bucket_count;

    rt_lru_node *node = bucket_find(cache->buckets[idx], key_data, key_len);
    return node ? node->value : NULL;
}

/// @brief Returns 1 if `key` is currently cached. Does not change LRU order.
/// @param obj LRUCache handle, or NULL.
/// @param key Key to test; NULL denotes the empty key.
/// @return 1 if an entry exists, including one storing NULL; otherwise 0.
int8_t rt_lrucache_has(void *obj, rt_string key) {
    if (!obj)
        return 0;

    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Has: invalid LRUCache object");
    if (!cache || !cache->buckets || cache->bucket_count == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "LRUCache.Has: invalid key", &key_data, &key_len))
        return 0;
    uint64_t hash = lru_hash(key_data, key_len);
    size_t idx = hash % cache->bucket_count;

    return bucket_find(cache->buckets[idx], key_data, key_len) ? 1 : 0;
}

/// @brief Remove an entry by `key`. Releases the value's reference. Returns 1 on success,
/// 0 if the key wasn't present.
/// @param obj LRUCache handle, or NULL.
/// @param key Key to remove; NULL denotes the empty key.
/// @return 1 if removed; otherwise 0.
int8_t rt_lrucache_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;

    rt_gc_mutator_enter();
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Remove: invalid LRUCache object");
    if (!cache || !cache->buckets || cache->bucket_count == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "LRUCache.Remove: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return 0;
    }
    uint64_t hash = lru_hash(key_data, key_len);
    size_t idx = hash % cache->bucket_count;

    rt_lru_node *node = bucket_find(cache->buckets[idx], key_data, key_len);
    if (!node) {
        rt_gc_mutator_exit();
        return 0;
    }

    list_remove(cache, node);
    bucket_remove(cache, node);
    cache->count--;
    rt_gc_mutator_exit();
    free_node(node);
    return 1;
}

/// @brief Force-evict the least-recently-used entry. Returns 1 if something was removed,
/// 0 if the cache was already empty.
/// @param obj LRUCache handle, or NULL.
/// @return 1 if the tail entry was removed; otherwise 0.
int8_t rt_lrucache_remove_oldest(void *obj) {
    if (!obj)
        return 0;

    rt_gc_mutator_enter();
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.RemoveOldest: invalid LRUCache object");
    if (!cache || cache->finalizing || !cache->tail) {
        rt_gc_mutator_exit();
        return 0;
    }

    rt_lru_node *removed = detach_lru(cache);
    rt_gc_mutator_exit();
    free_node(removed);
    return 1;
}

/// @brief Remove every entry, releasing all values. Capacity and bucket count are preserved
/// — the cache is reusable immediately without reallocation.
/// @param obj LRUCache handle, or NULL for a no-op.
void rt_lrucache_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_lrucache_impl *cache = as_lrucache(obj, "LRUCache.Clear: invalid LRUCache object");
    if (!cache) {
        rt_gc_mutator_exit();
        return;
    }
    if (cache->finalizing) {
        rt_gc_mutator_exit();
        return;
    }

    rt_lru_node *nodes = cache->head;
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    if (cache->buckets)
        memset(cache->buckets, 0, cache->bucket_count * sizeof(rt_lru_node *));
    rt_gc_mutator_exit();

    char cleanup_error[256] = {0};
    if (lru_destroy_nodes(nodes, cleanup_error, sizeof(cleanup_error)))
        rt_trap(cleanup_error[0] ? cleanup_error
                                 : "LRUCache.Clear: value finalizer cleanup failed");
}

/// @brief Return a Seq of all keys in MRU→LRU order. Owned-elements Seq (will release strings
/// on its own destruction). Snapshot — subsequent cache mutations don't affect the result.
/// @param obj LRUCache handle, or NULL.
/// @return New owning Seq of newly created key strings.
void *rt_lrucache_keys(void *obj) {
    rt_lrucache_impl *cache = NULL;
    if (obj) {
        cache = as_lrucache(obj, "LRUCache.Keys: invalid LRUCache object");
        if (!cache)
            return NULL;
    }

    int64_t initial_capacity = cache && cache->count > 0 ? (int64_t)cache->count : 1;
    void *result = rt_seq_with_capacity_owned(initial_capacity);
    if (!result)
        return NULL;
    if (!cache)
        return result;

    // Walk from head (MRU) to tail (LRU)
    for (rt_lru_node *node = cache->head; node; node = node->next) {
        if (!lru_append_key_or_release_seq(result, node->key, node->key_len))
            return NULL;
    }

    return result;
}

/// @brief Return an owning Seq of the values in MRU→LRU order (the snapshot
/// retains each entry and does not follow later cache mutations or evictions).
/// @param obj LRUCache handle, or NULL.
/// @return New owning Seq retaining cached values.
void *rt_lrucache_values(void *obj) {
    rt_lrucache_impl *cache = NULL;
    if (obj) {
        cache = as_lrucache(obj, "LRUCache.Values: invalid LRUCache object");
        if (!cache)
            return NULL;
    }

    int64_t initial_capacity = cache && cache->count > 0 ? (int64_t)cache->count : 1;
    void *result = rt_seq_with_capacity_owned(initial_capacity);
    if (!result)
        return NULL;
    if (!cache)
        return result;

    // Walk from head (MRU) to tail (LRU)
    for (rt_lru_node *node = cache->head; node; node = node->next) {
        if (!lru_append_value_or_release_seq(result, node->value))
            return NULL;
    }

    return result;
}
