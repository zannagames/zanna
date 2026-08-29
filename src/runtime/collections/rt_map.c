//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_map.c
// Purpose: Implements the primary string-keyed hash map (Map / Dictionary) for
//   the Zanna runtime. Maps arbitrary string keys to object values using keyed
//   SipHash-2-4 with separate chaining. Supports get, put, remove, contains,
//   keys, values, and iteration. This is the most commonly used associative
//   collection in the Zanna standard library.
//
// Key invariants:
//   - Initial capacity is MAP_INITIAL_CAPACITY (16) buckets; resizes (doubles)
//     at 75% load factor (MAP_LOAD_FACTOR 3/4).
//   - Each entry owns a heap-allocated copy of the key string; the Map is
//     independent of the lifetime of the source rt_string objects.
//   - Values are stored as raw void* pointers; the map retains a reference
//     (rt_obj_retain) on insert and releases on remove/overwrite.
//   - Hashing uses a per-process SipHash seed over raw key bytes; collision
//     chains are singly-linked.
//   - All operations are O(1) average case; O(n) worst case due to chaining.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - Map objects are GC-managed (rt_obj_new_i64). The bucket array and all
//     entry nodes (including copied key strings) are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_map.h (public API),
//        src/runtime/text/rt_hash_util.h (keyed SipHash helper)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's primary retained-value string Map.
///
/// Map owns inline copies of complete key byte strings and stores retained
/// opaque values in separately chained keyed-hash buckets. Insertions grow the
/// table transactionally at the shared load threshold; explicit trimming can
/// reclaim bucket capacity without changing associations.
///
/// Generic getters return borrowed pointers. Enumeration returns owning
/// snapshots, and typed helpers box values on write or perform documented
/// numeric/string conversions on read. Null strings denote the empty key.
/// Map objects participate in GC traversal and mutation is unsynchronized
/// apart from the runtime mutator protocol.

#include "rt_map.h"
#include "rt_numeric.h"
#include "rt_platform.h"

#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_hash_table_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Initial number of buckets.
#define MAP_INITIAL_CAPACITY 16

#include "rt_hash_util.h"

/// @brief Entry in the hash map (collision chain node).
///
/// Each entry stores a key-value pair in the Map. Entries are organized into
/// collision chains (linked lists) within each bucket. The Map owns a copy
/// of each string key and retains a reference to each value.
typedef struct rt_map_entry {
    size_t key_len;            ///< Length of key string (excluding null terminator).
    uint64_t hash;             ///< Cached keyed hash used by lookup and rehash.
    void *value;               ///< Retained reference to the value object.
    struct rt_map_entry *next; ///< Next entry in collision chain (or NULL).
    char key[];                ///< Inline owned key bytes followed by one NUL byte.
} rt_map_entry;

/// @brief Map (string-to-object dictionary) implementation structure.
///
/// The Map is implemented as a hash table with separate chaining for
/// collision resolution. It provides O(1) average-case lookup, insertion,
/// and deletion for string-keyed associations.
///
/// **Hash table structure:**
/// ```
/// buckets array:
///   [0] -> entry("apple", valA) -> entry("apricot", valB) -> NULL
///   [1] -> NULL
///   [2] -> entry("banana", valC) -> NULL
///   [3] -> entry("cherry", valD) -> entry("coconut", valE) -> NULL
///   ...
///   [capacity-1] -> NULL
/// ```
///
/// **Hash function:**
/// Uses keyed SipHash-2-4 through the rt_fnv1a compatibility wrapper.
///
/// **Load factor:**
/// Resizes when count/capacity exceeds 75% (3/4) to maintain O(1) performance.
///
/// **Key/Value ownership:**
/// - Keys: The Map owns copies of all keys (not references to originals)
/// - Values: The Map retains references (increments ref count)
typedef struct rt_map_impl {
    void **vptr;            ///< Vtable pointer placeholder (for OOP compatibility).
    rt_map_entry **buckets; ///< Array of bucket heads (collision chain pointers).
    size_t capacity;        ///< Number of buckets in the hash table.
    size_t count;           ///< Number of key-value pairs currently in the Map.
} rt_map_impl;

/// @brief Checked cast of an opaque handle to the Map implementation.
/// @details Traps with @p what if @p obj is NULL or not a Map.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_map_impl *as_map(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_MAP_CLASS_ID, sizeof(rt_map_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_map_impl *)obj;
}

/// @brief Release one managed object owned by a temporary Map operation.
/// @param obj Owned object reference, or NULL.
static void map_release_owned(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic before removing a recovery frame.
/// @param output Destination buffer.
/// @param capacity Destination capacity in bytes.
/// @param fallback Text used when the active diagnostic is empty.
static void map_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Extracts C string data and length from a Zanna string.
///
/// Helper function to safely get the underlying character data from a
/// Zanna string object for use with the hash table operations.
///
/// @param key The Zanna string to extract data from.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Pointer to receive the string length.
///
/// @return Nonzero for null or a live string handle; otherwise zero after
///         trapping. Null continues to denote the empty key.
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

/// @brief Finds an entry in a bucket's collision chain.
///
/// Performs a linear search through the linked list of entries in a bucket
/// to find one matching the given key.
///
/// @param head The head of the collision chain to search.
/// @param hash Cached hash of the key being searched for.
/// @param key The key string to search for.
/// @param key_len Length of the key string.
///
/// @return Pointer to the matching entry, or NULL if not found.
///
/// @note O(k) time where k is the chain length.
static rt_map_entry *find_entry(rt_map_entry *head,
                                uint64_t hash,
                                const char *key,
                                size_t key_len) {
    for (rt_map_entry *e = head; e; e = e->next) {
        if (e->hash == hash && e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/// @brief Allocate one map entry with its copied key stored inline.
/// @details A single zeroed runtime allocation holds both the collision node
///          and the key bytes, reducing allocator traffic and fragmentation.
///          The value is recorded but not retained; callers retain before this
///          helper and release that retain if allocation fails.
/// @param key Key bytes to copy.
/// @param key_len Number of key bytes, excluding the appended NUL terminator.
/// @param hash Precomputed keyed hash for the entry.
/// @param value Managed value pointer to record.
/// @param trap_message Diagnostic to report if allocation fails.
/// @return Newly allocated entry, or NULL after reporting overflow/allocation
///         failure.
static rt_map_entry *map_entry_new(
    const char *key, size_t key_len, uint64_t hash, void *value, const char *trap_message) {
    if (key_len > SIZE_MAX - sizeof(rt_map_entry) - 1 ||
        sizeof(rt_map_entry) + key_len + 1 > (size_t)INT64_MAX) {
        rt_trap("Map: key allocation overflow");
        return NULL;
    }
    size_t allocation_size = sizeof(rt_map_entry) + key_len + 1;
    rt_map_entry *entry = (rt_map_entry *)rt_alloc((int64_t)allocation_size);
    if (!entry) {
        rt_trap(trap_message);
        return NULL;
    }
    entry->key_len = key_len;
    entry->hash = hash;
    entry->value = value;
    memcpy(entry->key, key, key_len);
    entry->key[key_len] = '\0';
    return entry;
}

/// @brief Frees an entry, its owned key, and releases its value reference.
///
/// Releases all resources associated with a map entry:
/// 1. Frees the copied key string
/// 2. Releases the reference to the value (may free if last reference)
/// 3. Frees the entry structure itself
///
/// @param entry The entry to free. If NULL, this is a no-op.
static void free_entry(rt_map_entry *entry) {
    if (!entry)
        return;
    void *value = entry->value;
    rt_free(entry);
    map_release_owned(value);
}

/// @brief Destroy a detached entry chain.
/// @details Native nodes are reclaimed before callback-capable value releases;
///          callers publish the corresponding table/count change first.
/// @param entries Detached chain to consume.
static void map_destroy_entries(rt_map_entry *entries) {
    while (entries) {
        rt_map_entry *next = entries->next;
        free_entry(entries);
        entries = next;
    }
}

/// @brief Detach every entry while preserving the allocated bucket array.
/// @param map Live Map whose published entries are transferred.
/// @return One chain owning every formerly published entry.
static rt_map_entry *map_detach_entries(rt_map_impl *map) {
    rt_map_entry *detached = NULL;
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_map_entry *entry = map->buckets[i];
        map->buckets[i] = NULL;
        while (entry) {
            rt_map_entry *next = entry->next;
            entry->next = detached;
            detached = entry;
            entry = next;
        }
    }
    map->count = 0;
    return detached;
}

/// @brief GC traversal: visit every stored value across all bucket chains.
/// @param obj Map object to traverse.
/// @param visitor Runtime callback invoked for every retained value.
/// @param ctx Opaque visitor context forwarded unchanged.
static void rt_map_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map)
        return;
    if (!map->buckets || map->capacity == 0)
        return;
    for (size_t i = 0; i < map->capacity; ++i) {
        for (rt_map_entry *entry = map->buckets[i]; entry; entry = entry->next)
            visitor(entry->value, ctx);
    }
}

/// @brief Finalizer callback invoked when a Map is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// Map object becomes unreachable. It clears all entries (freeing keys and
/// releasing value references) and frees the buckets array.
///
/// @param obj Pointer to the Map object being finalized. May be NULL (no-op).
///
/// @note This function is idempotent - safe to call on already-finalized maps.
static void rt_map_finalize(void *obj) {
    if (!obj)
        return;
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map)
        return;
    rt_map_entry **buckets = map->buckets;
    size_t capacity = map->capacity;
    map->buckets = NULL;
    map->capacity = 0;
    map->count = 0;
    if (buckets) {
        for (size_t i = 0; i < capacity; ++i)
            map_destroy_entries(buckets[i]);
    }
    rt_free(buckets);
}

/// @brief Resizes the hash table to a new capacity and rehashes all entries.
///
/// When the load factor becomes too high, this function creates a new larger
/// bucket array and rehashes all existing entries to maintain O(1) performance.
///
/// @param map The Map to resize.
/// @param new_capacity The new number of buckets.
///
/// @return Non-zero when resize succeeds; zero when the old table is preserved
///         after a recoverable trap or allocation failure.
/// @note O(n) time complexity where n is the number of entries.
static int map_resize(rt_map_impl *map, size_t new_capacity) {
    if (new_capacity == 0 || new_capacity > SIZE_MAX / sizeof(rt_map_entry *) ||
        new_capacity * sizeof(rt_map_entry *) > (size_t)INT64_MAX) {
        rt_trap("Map: allocation size overflow");
        return 0;
    }
    rt_map_entry **new_buckets =
        (rt_map_entry **)rt_alloc((int64_t)(new_capacity * sizeof(rt_map_entry *)));
    if (!new_buckets) {
        rt_trap("Map: memory allocation failed");
        return 0;
    }

    // Rehash all entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_map_entry *entry = map->buckets[i];
        while (entry) {
            rt_map_entry *next = entry->next;
            size_t idx = entry->hash % new_capacity;
            entry->next = new_buckets[idx];
            new_buckets[idx] = entry;
            entry = next;
        }
    }

    rt_free(map->buckets);
    map->buckets = new_buckets;
    map->capacity = new_capacity;
    return 1;
}

/// @brief Checks if resize is needed and performs it.
///
/// Triggers a resize when the load factor exceeds 75% (3/4).
///
/// @param map The Map to potentially resize.
/// @param next_count Entry count after the pending new-key insertion.
///
/// @return Non-zero when current capacity suffices or growth succeeds; zero
///         after an overflow or allocation trap.
/// @note The capacity doubles on each resize.
static int maybe_resize_for_count(rt_map_impl *map, size_t next_count) {
    if (rt_hash_table_exceeds_load(next_count, map->capacity)) {
        size_t new_capacity = 0;
        if (!rt_hash_table_double_capacity(map->capacity, &new_capacity)) {
            rt_trap("Map: capacity overflow");
            return 0;
        }
        return map_resize(map, new_capacity);
    }
    return 1;
}

/// @brief Creates a new empty Map (string-to-object dictionary).
///
/// Allocates and initializes a Map data structure for storing key-value pairs
/// where keys are strings and values are objects. The Map uses a hash table
/// with separate chaining for O(1) average-case operations.
///
/// **Usage example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// map.Set("age", 30)
/// Print map.Get("name")  ' Outputs: Alice
/// Print map.Len()        ' Outputs: 2
/// ```
///
/// **Implementation notes:**
/// - Uses FNV-1a hash function for key hashing
/// - Collision resolution via separate chaining (linked lists)
/// - Automatically grows when load factor exceeds 75%
///
/// @return A pointer to the newly created Map object, or NULL if memory
///         allocation fails for the Map structure.
///
/// @note Initial capacity is 16 buckets.
/// @note Keys are copied (not referenced), values are retained (ref counted).
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_map_set For adding key-value pairs
/// @see rt_map_get For retrieving values
/// @see rt_map_finalize For cleanup behavior
void *rt_map_new(void) {
    rt_map_impl *map = (rt_map_impl *)rt_obj_new_i64(RT_MAP_CLASS_ID, (int64_t)sizeof(rt_map_impl));
    if (!map)
        return NULL;

    map->vptr = NULL;
    map->buckets =
        (rt_map_entry **)rt_alloc((int64_t)(MAP_INITIAL_CAPACITY * sizeof(rt_map_entry *)));
    if (!map->buckets) {
        if (rt_obj_release_check0(map))
            rt_obj_free(map);
        rt_trap("Map: memory allocation failed");
        return NULL;
    }
    map->capacity = MAP_INITIAL_CAPACITY;
    map->count = 0;
    rt_obj_set_finalizer(map, rt_map_finalize);
    rt_gc_track(map, rt_map_traverse);
    return map;
}

/// @brief Returns the number of key-value pairs in the Map.
///
/// This function returns how many entries have been added to the Map.
/// The count is maintained internally and returned in O(1) time.
///
/// @param obj Pointer to a Map object. If NULL, returns 0.
///
/// @return The number of entries in the Map (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_map_is_empty For a boolean check
/// @see rt_map_set For operations that may increase the count
/// @see rt_map_remove For operations that decrease the count
int64_t rt_map_len(void *obj) {
    if (!obj)
        return 0;
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    return map ? (int64_t)map->count : 0;
}

/// @brief Checks whether the Map contains no entries.
///
/// A Map is considered empty when its count is 0, which occurs:
/// - Immediately after creation
/// - After all entries have been removed
/// - After calling rt_map_clear
///
/// @param obj Pointer to a Map object. If NULL, returns true (1).
///
/// @return 1 (true) if the Map is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_map_len For the exact count
/// @see rt_map_clear For removing all entries
int8_t rt_map_is_empty(void *obj) {
    return rt_map_len(obj) == 0;
}

/// @brief Sets a value for a key in the Map.
///
/// Associates the given value with the given key. If the key already exists,
/// its value is replaced (the old value's reference is released). If the key
/// is new, a new entry is created.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// map.Set("age", 30)
/// map.Set("name", "Bob")    ' Replaces "Alice" with "Bob"
/// Print map.Get("name")     ' Outputs: Bob
/// ```
///
/// **Key handling:**
/// The Map copies the key string - the original can be freed after this call.
///
/// **Value handling:**
/// The Map retains a reference to the value. The old value (if replacing)
/// has its reference released.
///
/// @param obj Pointer to a Map object. If NULL, this is a no-op.
/// @param key The string key to associate with the value.
/// @param value The value to store. Reference is retained by the Map.
///
/// @note O(1) average-case time complexity.
/// @note May trigger a resize if the load factor exceeds 75%.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_get For retrieving values
/// @see rt_map_has For checking if a key exists
/// @see rt_map_remove For removing entries
void rt_map_set(void *obj, rt_string key, void *value) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map) {
        rt_gc_mutator_exit();
        return;
    }
    if (!map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.Set: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return;
    }
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    // Check if key already exists
    rt_map_entry *existing = find_entry(map->buckets[idx], hash, key_data, key_len);
    if (existing) {
        // Update existing entry
        void *old_value = existing->value;
        if (!rt_collection_retain_checked(value, "Map.Set: value retain failed")) {
            rt_gc_mutator_exit();
            return;
        }
        existing->value = value;
        rt_gc_mutator_exit();
        map_release_owned(old_value);
        return;
    }

    if (map->count == SIZE_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Map.Set: maximum size reached");
        return;
    }
    if (!maybe_resize_for_count(map, map->count + 1)) {
        rt_gc_mutator_exit();
        return;
    }
    idx = hash % map->capacity;

    if (!rt_collection_retain_checked(value, "Map.Set: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }

    rt_map_entry *entry =
        map_entry_new(key_data, key_len, hash, value, "Map.Set: memory allocation failed");
    if (!entry) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        rt_gc_mutator_exit();
        return;
    }

    // Insert at head of bucket chain
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
    map->count++;
    rt_gc_mutator_exit();
}

/// @brief Retrieves the value associated with a key.
///
/// Looks up the key in the Map and returns its associated value. Returns NULL
/// if the key is not found.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// Print map.Get("name")     ' Outputs: Alice
/// Print map.Get("missing")  ' Outputs: Nothing (NULL)
/// ```
///
/// @param obj Pointer to a Map object. If NULL, returns NULL.
/// @param key The string key to look up.
///
/// @return The value associated with the key, or NULL if not found.
///
/// @note O(1) average-case time complexity.
/// @note Does not modify the Map.
/// @note Thread safety: Safe for concurrent reads if no concurrent writes.
///
/// @see rt_map_get_or For providing a default value
/// @see rt_map_has For checking existence without retrieving
/// @see rt_map_set For storing values
void *rt_map_get(void *obj, rt_string key) {
    if (!obj)
        return NULL;

    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map || map->capacity == 0)
        return NULL;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.Get: invalid key", &key_data, &key_len))
        return NULL;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    rt_map_entry *entry = find_entry(map->buckets[idx], hash, key_data, key_len);
    return entry ? entry->value : NULL;
}

/// @brief Retrieves the value associated with a key, or a default if not found.
///
/// Looks up the key in the Map and returns its associated value. If the key
/// is not found, returns the provided default value instead of NULL.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// Print map.GetOr("name", "Unknown")    ' Outputs: Alice
/// Print map.GetOr("missing", "Unknown") ' Outputs: Unknown
/// ```
///
/// **Comparison with Get:**
/// - Get returns NULL for missing keys
/// - GetOr returns your chosen default for missing keys
///
/// @param obj Pointer to a Map object. If NULL, returns default_value.
/// @param key The string key to look up.
/// @param default_value The value to return if the key is not found.
///
/// @return The value associated with the key, or default_value if not found.
///
/// @note O(1) average-case time complexity.
/// @note Does not modify the Map - missing keys do not create new entries.
/// @note Thread safety: Safe for concurrent reads if no concurrent writes.
///
/// @see rt_map_get For returning NULL on missing keys
/// @see rt_map_has For checking existence
void *rt_map_get_or(void *obj, rt_string key, void *default_value) {
    if (!obj)
        return default_value;

    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map || map->capacity == 0)
        return default_value;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.GetOr: invalid key", &key_data, &key_len))
        return default_value;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    rt_map_entry *entry = find_entry(map->buckets[idx], hash, key_data, key_len);
    return entry ? entry->value : default_value;
}

/// @brief Tests whether a key exists in the Map.
///
/// Performs a key lookup without retrieving the value. Useful for checking
/// if a key is present before conditionally operating on it.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// Print map.Has("name")    ' Outputs: True
/// Print map.Has("missing") ' Outputs: False
/// ```
///
/// @param obj Pointer to a Map object. If NULL, returns 0 (false).
/// @param key The string key to search for.
///
/// @return 1 (true) if the key exists, 0 (false) otherwise.
///
/// @note O(1) average-case time complexity.
/// @note Does not modify the Map.
/// @note Thread safety: Safe for concurrent reads if no concurrent writes.
///
/// @see rt_map_get For retrieving the value
/// @see rt_map_set For adding entries
int8_t rt_map_has(void *obj, rt_string key) {
    if (!obj)
        return 0;

    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map)
        return 0;
    if (map->capacity == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.Has: invalid key", &key_data, &key_len))
        return 0;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    return find_entry(map->buckets[idx], hash, key_data, key_len) ? 1 : 0;
}

/// @brief Sets a value for a key only if the key doesn't already exist.
///
/// Conditionally inserts a key-value pair. If the key already exists, the Map
/// is not modified and the function returns 0. This is useful for implementing
/// "insert if not exists" logic atomically.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// Print map.SetIfMissing("name", "Alice")  ' Outputs: 1 (inserted)
/// Print map.SetIfMissing("name", "Bob")    ' Outputs: 0 (already exists)
/// Print map.Get("name")                    ' Outputs: Alice
/// ```
///
/// **Use cases:**
/// - Setting default values only when not already set
/// - First-wins insertion semantics
/// - Implementing caching patterns
///
/// @param obj Pointer to a Map object. If NULL, returns 0.
/// @param key The string key to conditionally set.
/// @param value The value to store if the key is missing.
///
/// @return 1 if the key was missing and the value was inserted, 0 if the key
///         already existed.
///
/// @note O(1) average-case time complexity.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_set For unconditional set (replaces existing)
/// @see rt_map_has For checking existence
int8_t rt_map_set_if_missing(void *obj, rt_string key, void *value) {
    if (!obj)
        return 0;

    rt_gc_mutator_enter();
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map || !map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.SetIfMissing: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return 0;
    }
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    if (find_entry(map->buckets[idx], hash, key_data, key_len)) {
        rt_gc_mutator_exit();
        return 0;
    }

    if (map->count == SIZE_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Map.SetIfMissing: maximum size reached");
        return 0;
    }
    if (!maybe_resize_for_count(map, map->count + 1)) {
        rt_gc_mutator_exit();
        return 0;
    }
    idx = hash % map->capacity;

    if (!rt_collection_retain_checked(value, "Map.SetIfMissing: value retain failed")) {
        rt_gc_mutator_exit();
        return 0;
    }

    rt_map_entry *entry =
        map_entry_new(key_data, key_len, hash, value, "Map.SetIfMissing: memory allocation failed");
    if (!entry) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        rt_gc_mutator_exit();
        return 0;
    }

    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
    map->count++;
    rt_gc_mutator_exit();
    return 1;
}

/// @brief Removes the entry with the specified key from the Map.
///
/// Looks up the key and removes its entry if found. The key string is freed
/// and the value's reference is released.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// map.Set("age", 30)
/// Print map.Remove("name")    ' Outputs: 1 (removed)
/// Print map.Remove("missing") ' Outputs: 0 (not found)
/// Print map.Len()             ' Outputs: 1
/// ```
///
/// @param obj Pointer to a Map object. If NULL, returns 0.
/// @param key The string key to remove.
///
/// @return 1 if the key was found and removed, 0 if not found or obj is NULL.
///
/// @note O(1) average-case time complexity.
/// @note The Map does not shrink after removal - capacity is maintained.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_set For adding entries
/// @see rt_map_clear For removing all entries
int8_t rt_map_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;

    rt_gc_mutator_enter();
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map || !map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "Map.Remove: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return 0;
    }
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % map->capacity;

    rt_map_entry **prev_ptr = &map->buckets[idx];
    rt_map_entry *entry = map->buckets[idx];

    while (entry) {
        if (entry->hash == hash && entry->key_len == key_len &&
            memcmp(entry->key, key_data, key_len) == 0) {
            *prev_ptr = entry->next;
            map->count--;
            rt_gc_mutator_exit();
            free_entry(entry);
            return 1;
        }
        prev_ptr = &entry->next;
        entry = entry->next;
    }

    rt_gc_mutator_exit();
    return 0;
}

/// @brief Removes all entries from the Map.
///
/// Clears the Map by freeing all entries (keys and releasing value references).
/// After this call, the Map is empty but retains its bucket array capacity
/// for efficient reuse.
///
/// **Memory behavior:**
/// - All entry nodes are freed
/// - All copied key strings are freed
/// - All value references are released
/// - Bucket array is retained (not freed)
/// - Capacity remains unchanged
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("a", 1)
/// map.Set("b", 2)
/// Print map.Len()    ' Outputs: 2
/// map.Clear()
/// Print map.Len()    ' Outputs: 0
/// ```
///
/// @param obj Pointer to a Map object. If NULL, this is a no-op.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note The bucket array capacity is preserved for potential reuse.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_finalize For complete cleanup including bucket array
/// @see rt_map_is_empty For checking if empty
void rt_map_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_map_impl *map = as_map(obj, "Map: invalid Map object");
    if (!map) {
        rt_gc_mutator_exit();
        return;
    }
    if (!map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return;
    }

    rt_map_entry *detached = map_detach_entries(map);
    rt_gc_mutator_exit();
    map_destroy_entries(detached);
}

/// @brief Release excess bucket capacity while preserving every Map entry.
/// @details Selects the smallest power-of-two capacity, no smaller than the
///          initial sixteen buckets, whose 75-percent threshold can hold the
///          current count. The replacement bucket array is allocated before
///          any collision link is changed, so allocation failure leaves keys,
///          values, count, capacity, and iteration state unchanged. Calling
///          Trim on an already minimal Map performs no allocation.
/// @param obj Map object whose bucket storage should be compacted.
/// @return Non-zero when the Map was already minimal or compaction succeeded;
///         zero after an invalid-handle, overflow, or allocation trap.
int8_t rt_map_trim(void *obj) {
    if (!obj) {
        rt_trap("Map.Trim: invalid Map object");
        return 0;
    }

    rt_gc_mutator_enter();
    rt_map_impl *map = as_map(obj, "Map.Trim: invalid Map object");
    if (!map || !map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t target_capacity = 0;
    if (!rt_hash_table_trim_capacity(map->count, MAP_INITIAL_CAPACITY, &target_capacity)) {
        rt_gc_mutator_exit();
        rt_trap("Map.Trim: capacity overflow");
        return 0;
    }
    if (target_capacity >= map->capacity) {
        rt_gc_mutator_exit();
        return 1;
    }

    int resized = map_resize(map, target_capacity);
    rt_gc_mutator_exit();
    return resized ? 1 : 0;
}

/// @brief Returns all keys in the Map as a Seq.
///
/// Creates a new Seq containing copies of all keys currently in the Map.
/// This allows iterating over the Map's keys.
///
/// **Order guarantee:**
/// Keys are returned in hash table iteration order (bucket by bucket, then
/// chain order within each bucket). This order is NOT guaranteed to be
/// consistent across different runs or after modifications to the Map.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// map.Set("age", 30)
/// Dim keys = map.Keys()
/// For i = 0 To keys.Len() - 1
///     Print keys.Get(i)  ' Outputs each key (order varies)
/// Next
/// ```
///
/// @param obj Pointer to a Map object. If NULL, returns an empty Seq.
///
/// @return A new Seq containing copies of all keys in the Map.
///
/// @note O(n) time and space complexity where n is the number of entries.
/// @note Iteration order is implementation-defined (not sorted).
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_values For getting all values
void *rt_map_keys(void *obj) {
    void *volatile result = NULL;
    rt_string volatile key_string = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        map_save_trap(saved_error, sizeof(saved_error), "Map.Keys: snapshot allocation failed");
        rt_trap_clear_recovery();
        map_release_owned((void *)key_string);
        map_release_owned((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    rt_map_impl *map = obj ? as_map(obj, "Map: invalid Map object") : NULL;
    if (obj && !map) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (map && map->count > (size_t)INT64_MAX)
        rt_trap("Map.Keys: snapshot is too large");
    result = rt_seq_with_capacity_owned(map ? (int64_t)map->count : 1);
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (!map) {
        rt_trap_clear_recovery();
        return (void *)result;
    }

    // Iterate through all buckets and entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_map_entry *entry = map->buckets[i];
        while (entry) {
            // Create a copy of the key as rt_string and push to seq
            key_string = rt_string_from_bytes(entry->key, entry->key_len);
            if (!key_string) {
                rt_trap_clear_recovery();
                map_release_owned((void *)result);
                return NULL;
            }
            rt_seq_push((void *)result, (void *)key_string);
            map_release_owned((void *)key_string);
            key_string = NULL;
            entry = entry->next;
        }
    }

    rt_trap_clear_recovery();
    return (void *)result;
}

/// @brief Returns all values in the Map as a Seq.
///
/// Creates a new Seq containing all values currently in the Map. The values
/// are the same objects as stored in the Map (not copies).
///
/// **Order guarantee:**
/// Values are returned in hash table iteration order (matching the order
/// of Keys()). This order is NOT guaranteed to be consistent.
///
/// **Example:**
/// ```
/// Dim map = Map.New()
/// map.Set("name", "Alice")
/// map.Set("age", 30)
/// Dim values = map.Values()
/// For i = 0 To values.Len() - 1
///     Print values.Get(i)  ' Outputs each value (order varies)
/// Next
/// ```
///
/// @param obj Pointer to a Map object. If NULL, returns an empty Seq.
///
/// @return A new Seq containing all values in the Map.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note Values are the same objects (not copies) - shared with the Map.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_map_keys For getting all keys
void *rt_map_values(void *obj) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        map_save_trap(saved_error, sizeof(saved_error), "Map.Values: snapshot allocation failed");
        rt_trap_clear_recovery();
        map_release_owned((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    rt_map_impl *map = obj ? as_map(obj, "Map: invalid Map object") : NULL;
    if (obj && !map) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (map && map->count > (size_t)INT64_MAX)
        rt_trap("Map.Values: snapshot is too large");
    result = rt_seq_with_capacity_owned(map ? (int64_t)map->count : 1);
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (!map) {
        rt_trap_clear_recovery();
        return (void *)result;
    }

    // Iterate through all buckets and entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_map_entry *entry = map->buckets[i];
        while (entry) {
            rt_seq_push((void *)result, entry->value);
            entry = entry->next;
        }
    }

    rt_trap_clear_recovery();
    return (void *)result;
}

//=============================================================================
// Typed Accessors (box/unbox wrappers)
//=============================================================================

#include "rt_box.h"

/// @brief Boxes and stores a signed integer.
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Integer to box.
void rt_map_set_int(void *obj, rt_string key, int64_t value) {
    void *boxed = rt_box_i64(value);
    rt_map_set(obj, key, boxed);
    // Release local ref — map_set already retained the boxed value
    if (boxed && rt_obj_release_check0(boxed))
        rt_obj_free(boxed);
}

/// @brief Reads a stored numeric value as a signed integer.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Integer value, saturated conversion of float, boolean as zero/one,
///         or zero if absent. Non-numeric values trap.
int64_t rt_map_get_int(void *obj, rt_string key) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return 0;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_I64)
        return rt_unbox_i64(val);
    if (tag == RT_BOX_F64)
        // Defined saturating conversion (NaN->0, out-of-range clamps): the raw
        // C cast is undefined for out-of-range doubles (VDOC-037).
        return (int64_t)rt_f64_to_i64(rt_unbox_f64(val));
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val) ? 1 : 0;
    rt_trap("Map.GetInt: value is not numeric");
    return 0;
}

/// @brief Reads a stored numeric value as an integer with a fallback.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Value returned for absence or a non-numeric stored value.
/// @return Converted numeric value or @p def.
int64_t rt_map_get_int_or(void *obj, rt_string key, int64_t def) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return def;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_I64)
        return rt_unbox_i64(val);
    if (tag == RT_BOX_F64)
        // Defined saturating conversion (NaN->0, out-of-range clamps): the raw
        // C cast is undefined for out-of-range doubles (VDOC-037).
        return (int64_t)rt_f64_to_i64(rt_unbox_f64(val));
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val) ? 1 : 0;
    return def;
}

/// @brief Boxes and stores a floating-point value.
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Floating-point value to box.
void rt_map_set_float(void *obj, rt_string key, double value) {
    void *boxed = rt_box_f64(value);
    rt_map_set(obj, key, boxed);
    // Release local ref — map_set already retained the boxed value
    if (boxed && rt_obj_release_check0(boxed))
        rt_obj_free(boxed);
}

/// @brief Reads a stored numeric value as double precision.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Converted numeric value, or 0.0 if absent. Non-numeric values trap.
double rt_map_get_float(void *obj, rt_string key) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return 0.0;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_F64)
        return rt_unbox_f64(val);
    if (tag == RT_BOX_I64)
        return (double)rt_unbox_i64(val);
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val) ? 1.0 : 0.0;
    rt_trap("Map.GetFloat: value is not numeric");
    return 0.0;
}

/// @brief Reads a stored numeric value as double precision with a fallback.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Value returned for absence or a non-numeric stored value.
/// @return Converted numeric value or @p def.
double rt_map_get_float_or(void *obj, rt_string key, double def) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return def;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_F64)
        return rt_unbox_f64(val);
    if (tag == RT_BOX_I64)
        return (double)rt_unbox_i64(val);
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val) ? 1.0 : 0.0;
    return def;
}

/// @brief Boxes and stores a boolean value.
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Value normalized by the runtime boolean boxer.
void rt_map_set_bool(void *obj, rt_string key, int8_t value) {
    void *boxed = rt_box_i1_bool(value);
    rt_map_set(obj, key, boxed);
    if (boxed && rt_obj_release_check0(boxed))
        rt_obj_free(boxed);
}

/// @brief Reads a stored boolean or numeric value as a boolean.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Normalized zero/one value, or zero if absent. Other types trap.
int8_t rt_map_get_bool(void *obj, rt_string key) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return 0;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val);
    if (tag == RT_BOX_I64)
        return rt_unbox_i64(val) != 0 ? 1 : 0;
    if (tag == RT_BOX_F64)
        return rt_unbox_f64(val) != 0.0 ? 1 : 0;
    rt_trap("Map.GetBool: value is not boolean or numeric");
    return 0;
}

/// @brief Reads a stored boolean or numeric value with a fallback.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Value returned for absence or a non-numeric stored value.
/// @return Converted zero/one value or @p def.
int8_t rt_map_get_bool_or(void *obj, rt_string key, int8_t def) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return def;
    int64_t tag = rt_box_type(val);
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(val);
    if (tag == RT_BOX_I64)
        return rt_unbox_i64(val) != 0 ? 1 : 0;
    if (tag == RT_BOX_F64)
        return rt_unbox_f64(val) != 0.0 ? 1 : 0;
    return def;
}

/// @brief Stores a runtime string value under a string key.
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Runtime string to retain; may be NULL.
void rt_map_set_str(void *obj, rt_string key, rt_string value) {
    rt_map_set(obj, key, (void *)value);
}

/// @brief Reads a stored raw or boxed string.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Newly retained or unboxed string, or a newly created empty string
///         when absent. A present non-string value traps.
rt_string rt_map_get_str(void *obj, rt_string key) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return rt_string_from_bytes("", 0);
    if (rt_string_is_handle(val))
        return rt_string_ref((rt_string)val);
    if (rt_box_type(val) == RT_BOX_STR)
        return rt_unbox_str(val);
    rt_trap("Map.GetStr: value is not a string");
    return NULL;
}

/// @brief Reads an optional stored raw or boxed string.
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Newly retained or unboxed string, or NULL when absent. A present
///         non-string value traps.
rt_string rt_map_get_opt_str(void *obj, rt_string key) {
    void *val = rt_map_get(obj, key);
    if (!val)
        return NULL;
    if (rt_string_is_handle(val))
        return rt_string_ref((rt_string)val);
    if (rt_box_type(val) == RT_BOX_STR)
        return rt_unbox_str(val);
    rt_trap("Map.GetOptStr: value is not a string");
    return NULL;
}

/// @brief Create a shallow copy of the map.
///
/// Allocates a new Map and copies all key-value pairs from the source.
/// Keys are independently copied (as always with Map); values are shared
/// references (not deep-copied).
///
/// @param obj Source Map pointer (may be NULL).
/// @return New Map containing the same key-value pairs, or empty map if NULL.
void *rt_map_clone(void *obj) {
    void *volatile result = NULL;
    rt_string volatile key_string = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        map_save_trap(saved_error, sizeof(saved_error), "Map.Clone: allocation failed");
        rt_trap_clear_recovery();
        map_release_owned((void *)key_string);
        map_release_owned((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    rt_map_impl *map = obj ? as_map(obj, "Map.Clone: invalid Map object") : NULL;
    result = rt_map_new();
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (!map) {
        rt_trap_clear_recovery();
        return (void *)result;
    }

    for (size_t i = 0; i < map->capacity; ++i) {
        rt_map_entry *entry = map->buckets[i];
        while (entry) {
            key_string = rt_string_from_bytes(entry->key, entry->key_len);
            rt_map_set((void *)result, (rt_string)key_string, entry->value);
            map_release_owned((void *)key_string);
            key_string = NULL;
            entry = entry->next;
        }
    }
    rt_trap_clear_recovery();
    return (void *)result;
}
