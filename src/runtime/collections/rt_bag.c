//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_bag.c
// Purpose: Implements a string set (Bag) using an FNV-1a hash table with
//   separate chaining. A Bag stores distinct strings, supporting add, remove,
//   and membership queries. Adding an already-present string is a no-op
//   (set semantics).
//
// Key invariants:
//   - Backed by a hash table with initial capacity BAG_INITIAL_CAPACITY (16)
//     and separate chaining (linked list per bucket).
//   - Resizes (doubles) when count/capacity exceeds 75% (BAG_LOAD_FACTOR 3/4).
//   - Each entry owns a heap-allocated copy of the key string; the Bag is
//     independent of the lifetime of the source rt_string objects.
//   - Hash is FNV-1a over the raw key bytes for good distribution.
//   - All operations are O(1) average case; O(n) worst case due to chaining.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - Bag objects are GC-managed (rt_obj_new_i64). The bucket array and all
//     entry nodes are freed by the GC finalizer (bag_finalizer).
//
// Links: src/runtime/collections/rt_bag.h (public API),
//        src/runtime/collections/rt_hash_util.h (FNV-1a hash macro)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's unique-string Bag collection.
///
/// A Bag is a set of byte strings backed by a separately chained hash table.
/// Keys are hashed with FNV-1a and copied into inline entry storage, so the
/// collection does not retain or depend on the source `rt_string` objects.
/// The table grows before an insertion would exceed the shared collection
/// load-factor threshold, while removals and clears retain the current bucket
/// capacity for later reuse.
///
/// The public API accepts opaque runtime object handles and validates non-null
/// handles against `RT_BAG_CLASS_ID`. Null handles have operation-specific
/// identity behavior for queries and set operations; invalid non-null handles
/// trap. Mutating operations are not synchronized, and callers must provide
/// external synchronization when a Bag can be accessed concurrently.

#include "rt_bag.h"

#include "rt_collection_ids.h"
#include "rt_hash_table_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// Initial number of buckets.
#define BAG_INITIAL_CAPACITY 16

#include "rt_hash_util.h"

/// @brief Entry in the hash set (collision chain node).
///
/// Each entry stores a string in the Bag. Entries are organized into
/// collision chains (linked lists) within each bucket. The Bag owns
/// a copy of each string key, not a reference to the original.
typedef struct rt_bag_entry {
    size_t key_len;            ///< Length of string (excluding null terminator).
    uint64_t hash;             ///< Cached keyed hash used by lookup and rehash.
    struct rt_bag_entry *next; ///< Next entry in collision chain (or NULL).
    char key[];                ///< Inline owned bytes followed by one NUL byte.
} rt_bag_entry;

/// @brief Bag (string set) implementation structure.
///
/// The Bag is implemented as a hash table with separate chaining for
/// collision resolution. It provides O(1) average-case lookup, insertion,
/// and deletion for string membership testing.
///
/// **Hash table structure:**
/// ```
/// buckets array:
///   [0] -> entry("apple") -> entry("apricot") -> NULL
///   [1] -> NULL
///   [2] -> entry("banana") -> NULL
///   [3] -> entry("cherry") -> entry("coconut") -> entry("cranberry") -> NULL
///   ...
///   [capacity-1] -> NULL
/// ```
///
/// **Hash function:**
/// Uses FNV-1a, a fast non-cryptographic hash with good distribution.
///
/// **Load factor:**
/// Resizes when count/capacity exceeds 75% (3/4) to maintain O(1) performance.
typedef struct rt_bag_impl {
    void **vptr;            ///< Vtable pointer placeholder (for OOP compatibility).
    rt_bag_entry **buckets; ///< Array of bucket heads (collision chain pointers).
    size_t capacity;        ///< Number of buckets in the hash table.
    size_t count;           ///< Number of strings currently in the Bag.
} rt_bag_impl;

/// @brief Checked cast of an opaque handle to the Bag implementation.
/// @details Traps with the @p what message if @p obj is NULL or not a Bag.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message to report when validation fails.
/// @return The validated Bag implementation pointer, or NULL if the runtime
///         trap handler returns after a failed validation.
static rt_bag_impl *as_bag(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_BAG_CLASS_ID, sizeof(rt_bag_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_bag_impl *)obj;
}

/// @brief Extracts C string data and length from a Zanna string.
///
/// Helper function to safely get the underlying character data from a
/// Zanna string object for use with the hash table operations.
///
/// @param key The Zanna string to extract data from.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed string bytes.
/// @param out_len Pointer to receive the string length.
///
/// @return Nonzero for a null or live string handle; otherwise zero after
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
    const char *data = rt_string_cstr(key);
    if (!data) {
        rt_trap(what);
        return 0;
    }
    *out_data = data;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Finds an entry in a bucket's collision chain.
///
/// Performs a linear search through the linked list of entries in a bucket
/// to find one matching the given key. Uses both length comparison and
/// memcmp for exact matching.
///
/// @param head The head of the collision chain to search.
/// @param hash Cached hash of the key being searched for.
/// @param key The key string to search for.
/// @param key_len Length of the key string.
///
/// @return Pointer to the matching entry, or NULL if not found.
///
/// @note O(k) time where k is the chain length (ideally small with good hash).
static rt_bag_entry *find_entry(rt_bag_entry *head,
                                uint64_t hash,
                                const char *key,
                                size_t key_len) {
    for (rt_bag_entry *e = head; e; e = e->next) {
        if (e->hash == hash && e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/// @brief Allocate one StringSet entry with its key stored inline.
/// @details Combining the collision node and copied bytes into one runtime
///          allocation halves per-entry allocator traffic and makes every
///          allocation failure observable through the runtime trap contract.
/// @param key Key bytes to copy.
/// @param key_len Number of key bytes, excluding the appended NUL terminator.
/// @param hash Precomputed keyed hash.
/// @return Newly allocated entry, or NULL after reporting an overflow or
///         allocation trap.
static rt_bag_entry *bag_entry_new(const char *key, size_t key_len, uint64_t hash) {
    if (key_len > SIZE_MAX - sizeof(rt_bag_entry) - 1 ||
        sizeof(rt_bag_entry) + key_len + 1 > (size_t)INT64_MAX) {
        rt_trap("Bag.Add: key allocation overflow");
        return NULL;
    }
    size_t allocation_size = sizeof(rt_bag_entry) + key_len + 1;
    rt_bag_entry *entry = (rt_bag_entry *)rt_alloc((int64_t)allocation_size);
    if (!entry) {
        rt_trap("Bag.Add: memory allocation failed");
        return NULL;
    }
    entry->key_len = key_len;
    entry->hash = hash;
    memcpy(entry->key, key, key_len);
    entry->key[key_len] = '\0';
    return entry;
}

/// @brief Frees an entry and its owned key string.
///
/// Releases all memory associated with a bag entry, including the
/// copied key string. Called when removing entries or clearing the bag.
///
/// @param entry The entry to free. If NULL, this is a no-op.
static void free_entry(rt_bag_entry *entry) {
    rt_free(entry);
}

/// @brief Finalizer callback invoked when a Bag is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// Bag object becomes unreachable. It:
/// 1. Clears all entries (freeing the copied key strings)
/// 2. Frees the buckets array
///
/// @param obj Pointer to the Bag object being finalized. May be NULL (no-op).
///
/// @note This function is idempotent - safe to call on already-finalized bags.
static void rt_bag_finalize(void *obj) {
    if (!obj)
        return;
    rt_bag_impl *bag = as_bag(obj, "Bag: invalid Bag object");
    if (!bag->buckets || bag->capacity == 0)
        return;
    rt_bag_clear(bag);
    rt_free(bag->buckets);
    bag->buckets = NULL;
    bag->capacity = 0;
    bag->count = 0;
}

/// @brief Resizes the hash table to a new capacity and rehashes all entries.
///
/// When the load factor becomes too high, this function creates a new
/// larger bucket array and rehashes all existing entries. This maintains
/// O(1) average-case performance for lookups and insertions.
///
/// **Rehashing process:**
/// 1. Allocate new bucket array with new_capacity slots
/// 2. For each entry in old buckets, compute new bucket index
/// 3. Insert entry at head of new bucket's chain
/// 4. Free old bucket array
///
/// @param bag The Bag to resize.
/// @param new_capacity The new number of buckets (should be power of 2 for efficiency).
///
/// @return Non-zero after installing the replacement table; zero after a trap,
///         with the old table preserved exactly.
/// @note O(n) time complexity where n is the number of entries.
static int bag_resize(rt_bag_impl *bag, size_t new_capacity) {
    if (new_capacity == 0 || new_capacity > SIZE_MAX / sizeof(rt_bag_entry *) ||
        new_capacity * sizeof(rt_bag_entry *) > (size_t)INT64_MAX) {
        rt_trap("Bag: allocation size overflow");
        return 0;
    }
    rt_bag_entry **new_buckets =
        (rt_bag_entry **)rt_alloc((int64_t)(new_capacity * sizeof(rt_bag_entry *)));
    if (!new_buckets) {
        rt_trap("Bag: memory allocation failed");
        return 0;
    }

    // Rehash all entries
    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            rt_bag_entry *next = entry->next;
            size_t idx = entry->hash % new_capacity;
            entry->next = new_buckets[idx];
            new_buckets[idx] = entry;
            entry = next;
        }
    }

    rt_free(bag->buckets);
    bag->buckets = new_buckets;
    bag->capacity = new_capacity;
    return 1;
}

/// @brief Checks if resize is needed and performs it.
///
/// Triggers a resize when the load factor exceeds 75% (3/4). This threshold
/// balances memory usage against lookup performance - higher load factors
/// mean longer collision chains and slower lookups.
///
/// @param bag The Bag to potentially resize.
/// @param next_count Entry count the Bag would have after the pending insert.
///
/// @return Non-zero when the current table can accommodate @p next_count or
///         resizing succeeds; zero after reporting an overflow or allocation
///         trap and leaving the existing table installed.
///
/// @note The capacity doubles on each resize.
static int maybe_resize_for_count(rt_bag_impl *bag, size_t next_count) {
    if (rt_hash_table_exceeds_load(next_count, bag->capacity)) {
        size_t new_capacity = 0;
        if (!rt_hash_table_double_capacity(bag->capacity, &new_capacity)) {
            rt_trap("Bag: capacity overflow");
            return 0;
        }
        return bag_resize(bag, new_capacity);
    }
    return 1;
}

/// @brief Creates a new empty Bag (string set) with default capacity.
///
/// Allocates and initializes a Bag data structure for storing unique strings.
/// The Bag uses a hash table with separate chaining for O(1) average-case
/// membership testing, insertion, and deletion.
///
/// The Bag starts with 16 buckets (BAG_INITIAL_CAPACITY) and automatically
/// resizes when the load factor exceeds 75% to maintain performance.
///
/// **Usage example:**
/// ```
/// Dim bag = Bag.New()
/// bag.Add("apple")
/// bag.Add("banana")
/// bag.Add("apple")      ' No effect - already present
/// Print bag.Has("apple") ' Outputs: True
/// Print bag.Count        ' Outputs: 2
/// ```
///
/// **Implementation notes:**
/// - Uses FNV-1a hash function for string hashing
/// - Collision resolution via separate chaining (linked lists)
/// - Automatically grows when load factor exceeds 75%
///
/// @return A pointer to the newly created Bag object. Traps if allocation fails.
///
/// @note Initial capacity is 16 buckets.
/// @note The Bag owns copies of all strings - it does not reference the originals.
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_bag_add For adding strings
/// @see rt_bag_has For membership testing
/// @see rt_bag_finalize For cleanup behavior
void *rt_bag_new(void) {
    rt_bag_impl *bag = (rt_bag_impl *)rt_obj_new_i64(RT_BAG_CLASS_ID, (int64_t)sizeof(rt_bag_impl));
    if (!bag) {
        rt_trap("Bag: memory allocation failed");
        return NULL;
    }

    bag->vptr = NULL;
    bag->buckets =
        (rt_bag_entry **)rt_alloc((int64_t)(BAG_INITIAL_CAPACITY * sizeof(rt_bag_entry *)));
    if (!bag->buckets) {
        if (rt_obj_release_check0(bag))
            rt_obj_free(bag);
        rt_trap("Bag: memory allocation failed");
        return NULL;
    }
    bag->capacity = BAG_INITIAL_CAPACITY;
    bag->count = 0;
    rt_obj_set_finalizer(bag, rt_bag_finalize);
    return bag;
}

/// @brief Returns the number of unique strings in the Bag.
///
/// This function returns how many distinct strings have been added to the Bag.
/// Duplicate insertions do not increase the count. The count is maintained
/// internally and returned in O(1) time.
///
/// @param obj Pointer to a Bag object. If NULL, returns 0.
///
/// @return The number of unique strings in the Bag (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_bag_is_empty For a boolean check
/// @see rt_bag_add For operations that may increase the count
/// @see rt_bag_remove For operations that decrease the count
int64_t rt_bag_len(void *obj) {
    if (!obj)
        return 0;
    rt_bag_impl *bag = as_bag(obj, "Bag.Len: invalid Bag object");
    return bag ? (int64_t)bag->count : 0;
}

/// @brief Checks whether the Bag contains no strings.
///
/// A Bag is considered empty when its count is 0, which occurs:
/// - Immediately after creation
/// - After all strings have been dropped
/// - After calling rt_bag_clear
///
/// @param obj Pointer to a Bag object. If NULL, returns true (1).
///
/// @return 1 (true) if the Bag is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_bag_len For the exact count
/// @see rt_bag_clear For removing all strings
int8_t rt_bag_is_empty(void *obj) {
    return rt_bag_len(obj) == 0;
}

/// @brief Adds a string to the Bag if not already present.
///
/// Attempts to insert a string into the Bag. If the string is already present,
/// the Bag remains unchanged and the function returns 0. If the string is new,
/// a copy is made and stored in the Bag.
///
/// **Set semantics:**
/// The Bag enforces uniqueness - each string can only appear once. This makes
/// it ideal for tracking membership, deduplication, and set operations.
///
/// **Example:**
/// ```
/// Dim bag = Bag.New()
/// Print bag.Add("apple")   ' Outputs: 1 (added)
/// Print bag.Add("banana")  ' Outputs: 1 (added)
/// Print bag.Add("apple")   ' Outputs: 0 (already present)
/// Print bag.Count          ' Outputs: 2
/// ```
///
/// **Hash collision handling:**
/// When two different strings hash to the same bucket, they are stored in a
/// linked list (separate chaining). The function searches the chain to check
/// for duplicates before inserting.
///
/// @param obj Pointer to a Bag object. Must not be NULL.
/// @param str The string to add. A copy is made - the original is not retained.
///
/// @return 1 if the string was added, or 0 only when it was already present.
///         Invalid handles, capacity overflow, and allocation failure trap;
///         a returning trap hook observes 0 with the Bag unchanged.
///
/// @note O(1) average-case time complexity. O(n) worst-case if all strings
///       hash to the same bucket.
/// @note The Bag copies the string - the original can be freed after this call.
/// @note May trigger a resize if the load factor exceeds 75%.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_has For checking membership without modifying
/// @see rt_bag_remove For removing strings
int8_t rt_bag_add(void *obj, rt_string str) {
    if (!obj) {
        rt_trap("Bag.Add: invalid Bag object");
        return 0;
    }

    rt_bag_impl *bag = as_bag(obj, "Bag.Add: invalid Bag object");
    if (!bag || !bag->buckets || bag->capacity == 0) {
        rt_trap("Bag.Add: unavailable bucket table");
        return 0;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(str, "Bag.Add: invalid string", &key_data, &key_len))
        return 0;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % bag->capacity;

    // Check if string already exists
    if (find_entry(bag->buckets[idx], hash, key_data, key_len)) {
        return 0; // Already present
    }

    if (bag->count >= (size_t)INT64_MAX) {
        rt_trap("StringSet.Add: maximum size reached");
        return 0;
    }
    if (!maybe_resize_for_count(bag, bag->count + 1))
        return 0;
    idx = hash % bag->capacity;

    rt_bag_entry *entry = bag_entry_new(key_data, key_len, hash);
    if (!entry)
        return 0;

    // Insert at head of bucket chain
    entry->next = bag->buckets[idx];
    bag->buckets[idx] = entry;
    bag->count++;
    return 1;
}

/// @brief Removes a string from the Bag if present.
///
/// Attempts to remove a string from the Bag. If the string is found, it is
/// removed and its memory is freed. If the string is not present, the Bag
/// remains unchanged.
///
/// **Example:**
/// ```
/// Dim bag = Bag.New()
/// bag.Add("apple")
/// bag.Add("banana")
/// Print bag.Remove("apple")   ' Outputs: 1 (removed)
/// Print bag.Remove("cherry")  ' Outputs: 0 (not found)
/// Print bag.Remove("apple")   ' Outputs: 0 (already removed)
/// Print bag.Count           ' Outputs: 1
/// ```
///
/// **Chain removal:**
/// The function traverses the bucket's collision chain to find the entry.
/// When found, it unlinks the entry from the chain and frees both the
/// copied key string and the entry structure.
///
/// @param obj Pointer to a Bag object. If NULL, returns 0.
/// @param str The string to remove.
///
/// @return 1 if the string was found and removed, 0 if it was not present
///         or obj is NULL.
///
/// @note O(1) average-case time complexity. O(n) worst-case for chain traversal.
/// @note The Bag does not shrink after removal - capacity is maintained.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_add For adding strings
/// @see rt_bag_has For checking membership
/// @see rt_bag_clear For removing all strings
int8_t rt_bag_remove(void *obj, rt_string str) {
    if (!obj)
        return 0;

    rt_bag_impl *bag = as_bag(obj, "Bag.Remove: invalid Bag object");
    if (!bag || !bag->buckets || bag->capacity == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(str, "Bag.Remove: invalid string", &key_data, &key_len))
        return 0;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % bag->capacity;

    rt_bag_entry **prev_ptr = &bag->buckets[idx];
    rt_bag_entry *entry = bag->buckets[idx];

    while (entry) {
        if (entry->hash == hash && entry->key_len == key_len &&
            memcmp(entry->key, key_data, key_len) == 0) {
            *prev_ptr = entry->next;
            free_entry(entry);
            bag->count--;
            return 1;
        }
        prev_ptr = &entry->next;
        entry = entry->next;
    }

    return 0;
}

/// @brief Tests whether a string is present in the Bag.
///
/// Performs a membership test without modifying the Bag. This is the primary
/// operation for using the Bag as a set - checking whether a string has been
/// previously added.
///
/// **Example:**
/// ```
/// Dim bag = Bag.New()
/// bag.Add("apple")
/// bag.Add("banana")
/// Print bag.Has("apple")   ' Outputs: True
/// Print bag.Has("cherry")  ' Outputs: False
/// ```
///
/// **Lookup process:**
/// 1. Hash the input string using FNV-1a
/// 2. Compute the bucket index (hash % capacity)
/// 3. Search the bucket's collision chain for a matching entry
/// 4. Compare by length first (fast rejection), then by content
///
/// @param obj Pointer to a Bag object. If NULL, returns 0 (false).
/// @param str The string to search for.
///
/// @return 1 (true) if the string is in the Bag, 0 (false) if not present
///         or obj is NULL.
///
/// @note O(1) average-case time complexity. O(n) worst-case for chain search.
/// @note Does not modify the Bag in any way.
/// @note Thread safety: Safe for concurrent reads if no concurrent writes.
///
/// @see rt_bag_add For adding strings to test later
/// @see rt_bag_remove For removing strings
int8_t rt_bag_has(void *obj, rt_string str) {
    if (!obj)
        return 0;

    rt_bag_impl *bag = as_bag(obj, "Bag.Has: invalid Bag object");
    if (!bag || !bag->buckets || bag->capacity == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(str, "Bag.Has: invalid string", &key_data, &key_len))
        return 0;
    uint64_t hash = rt_fnv1a(key_data, key_len);
    size_t idx = hash % bag->capacity;

    return find_entry(bag->buckets[idx], hash, key_data, key_len) ? 1 : 0;
}

/// @brief Removes all strings from the Bag.
///
/// Clears the Bag by freeing all entries and their copied key strings.
/// After this call, the Bag is empty but retains its bucket array capacity
/// for efficient reuse.
///
/// **Memory behavior:**
/// - All entry nodes are freed
/// - All copied key strings are freed
/// - Bucket array is retained (not freed)
/// - Capacity remains unchanged
///
/// **Example:**
/// ```
/// Dim bag = Bag.New()
/// bag.Add("apple")
/// bag.Add("banana")
/// Print bag.Count    ' Outputs: 2
/// bag.Clear()
/// Print bag.Count    ' Outputs: 0
/// Print bag.IsEmpty  ' Outputs: True
/// ```
///
/// @param obj Pointer to a Bag object. If NULL, this is a no-op.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note The bucket array capacity is preserved for potential reuse.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_finalize For complete cleanup including bucket array
/// @see rt_bag_is_empty For checking if empty
void rt_bag_clear(void *obj) {
    if (!obj)
        return;

    rt_bag_impl *bag = as_bag(obj, "Bag.Clear: invalid Bag object");
    if (!bag || !bag->buckets)
        return;
    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            rt_bag_entry *next = entry->next;
            free_entry(entry);
            entry = next;
        }
        bag->buckets[i] = NULL;
    }
    bag->count = 0;
}

/// @brief Returns all strings in the Bag as a Seq (sequence/list).
///
/// Creates a new Seq containing copies of all strings currently in the Bag.
/// This allows iterating over the Bag's contents or converting the set to
/// a list for further processing.
///
/// **Order guarantee:**
/// The strings are returned in hash table iteration order (bucket by bucket,
/// then chain order within each bucket). This order is NOT guaranteed to be
/// consistent across different runs or after modifications to the Bag.
///
/// **Example:**
/// ```
/// Dim bag = Bag.New()
/// bag.Add("apple")
/// bag.Add("banana")
/// bag.Add("cherry")
/// Dim items = bag.Items()
/// For i = 0 To items.Len() - 1
///     Print items.Get(i)  ' Outputs each fruit (order varies)
/// Next
/// ```
///
/// **Memory ownership:**
/// - The returned Seq is a new allocation owned by the caller
/// - Each string in the Seq is a fresh copy (not shared with the Bag)
/// - Modifying or freeing the Bag does not affect the returned Seq
///
/// @param obj Pointer to a Bag object. If NULL, returns an empty Seq.
///
/// @return A new Seq containing copies of all strings in the Bag.
///         Returns an empty Seq if obj is NULL or the Bag is empty.
///
/// @note O(n) time and space complexity where n is the number of strings.
/// @note Iteration order is implementation-defined (not sorted).
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_len For getting the count without creating a list
void *rt_bag_items(void *obj) {
    void *result = rt_seq_new();
    if (!result)
        return NULL;
    rt_seq_set_owns_elements(result, 1);
    if (!obj)
        return result;

    rt_bag_impl *bag = as_bag(obj, "Bag.Items: invalid Bag object");
    if (!bag)
        return result;

    // Iterate through all buckets and entries
    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            // Create a copy of the string and push to seq
            rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
            rt_seq_push(result, (void *)str);
            rt_str_release_maybe(str);
            entry = entry->next;
        }
    }

    return result;
}

/// @brief Creates a new Bag containing the union of two Bags.
///
/// Returns a new Bag containing all strings that appear in either input Bag
/// (or both). This is the set union operation (A ∪ B).
///
/// **Set theory:**
/// ```
/// A = {apple, banana}
/// B = {banana, cherry}
/// A.Union(B) = {apple, banana, cherry}
/// ```
///
/// **Example:**
/// ```
/// Dim fruits = Bag.New()
/// fruits.Put("apple")
/// fruits.Put("banana")
///
/// Dim more = Bag.New()
/// more.Put("banana")
/// more.Put("cherry")
///
/// Dim all = fruits.Union(more)
/// Print all.Len()  ' Outputs: 3 (apple, banana, cherry)
/// ```
///
/// **Behavior with NULL:**
/// - If obj is NULL, returns a copy of other (or empty Bag if both NULL)
/// - If other is NULL, returns a copy of obj
/// - If both are NULL, returns an empty Bag
///
/// @param obj Pointer to the first Bag. May be NULL.
/// @param other Pointer to the second Bag. May be NULL.
///
/// @return A new Bag containing all strings from both input Bags.
///         Duplicates are automatically eliminated (set semantics).
///
/// @note O(n + m) time complexity where n and m are the sizes of the inputs.
/// @note The input Bags are not modified.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_intersect For set intersection
/// @see rt_bag_diff For set difference
void *rt_bag_union(void *obj, void *other) {
    void *result = rt_bag_new();
    if (!result)
        return result;

    // Add all elements from first bag
    if (obj) {
        rt_bag_impl *bag = as_bag(obj, "Bag.Union: invalid Bag object");
        if (!bag)
            return result;
        for (size_t i = 0; i < bag->capacity; ++i) {
            rt_bag_entry *entry = bag->buckets[i];
            while (entry) {
                rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
                rt_bag_add(result, str);
                rt_str_release_maybe(str);
                entry = entry->next;
            }
        }
    }

    // Add all elements from second bag
    if (other) {
        rt_bag_impl *bag = as_bag(other, "Bag.Union: invalid Bag object");
        if (!bag)
            return result;
        for (size_t i = 0; i < bag->capacity; ++i) {
            rt_bag_entry *entry = bag->buckets[i];
            while (entry) {
                rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
                rt_bag_add(result, str);
                rt_str_release_maybe(str);
                entry = entry->next;
            }
        }
    }

    return result;
}

/// @brief Creates a new Bag containing the intersection of two Bags.
///
/// Returns a new Bag containing only strings that appear in both input Bags.
/// This is the set intersection operation (A ∩ B).
///
/// **Set theory:**
/// ```
/// A = {apple, banana, cherry}
/// B = {banana, cherry, date}
/// A.Intersect(B) = {banana, cherry}
/// ```
///
/// **Example:**
/// ```
/// Dim fruits = Bag.New()
/// fruits.Put("apple")
/// fruits.Put("banana")
/// fruits.Put("cherry")
///
/// Dim tropical = Bag.New()
/// tropical.Put("banana")
/// tropical.Put("cherry")
/// tropical.Put("mango")
///
/// Dim both = fruits.Intersect(tropical)
/// Print both.Len()  ' Outputs: 2 (banana, cherry)
/// ```
///
/// **Use cases:**
/// - Finding common elements between two sets
/// - Validating that items exist in a reference set
/// - Computing set overlap
///
/// @param obj Pointer to the first Bag. If NULL, returns an empty Bag.
/// @param other Pointer to the second Bag. If NULL, returns an empty Bag.
///
/// @return A new Bag containing only strings present in both input Bags.
///         Returns an empty Bag if either input is NULL or empty.
///
/// @note O(n) time complexity where n is the size of the first Bag (each
///       element requires an O(1) lookup in the second Bag).
/// @note The input Bags are not modified.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_bag_union For set union
/// @see rt_bag_diff For set difference
void *rt_bag_intersect(void *obj, void *other) {
    void *result = rt_bag_new();
    if (!result || !obj || !other)
        return result;

    rt_bag_impl *bag = as_bag(obj, "Bag.Intersect: invalid Bag object");
    rt_bag_impl *other_bag = as_bag(other, "Bag.Intersect: invalid Bag object");
    if (!bag || !other_bag)
        return result;

    // For each element in first bag, check if it's in second
    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
            if (rt_bag_has(other, str)) {
                rt_bag_add(result, str);
            }
            rt_str_release_maybe(str);
            entry = entry->next;
        }
    }

    return result;
}

/// @brief Creates a deep copy (clone) of a Bag.
///
/// @param obj Pointer to the Bag to clone. If NULL, returns empty Bag.
///
/// @return A new Bag containing all strings from obj.
///
/// @note O(n) time complexity where n is the size of the Bag.
/// @note The input Bag is not modified.
void *rt_bag_clone(void *obj) {
    void *result = rt_bag_new();
    if (!result || !obj)
        return result;

    rt_bag_impl *bag = as_bag(obj, "Bag.Clone: invalid Bag object");
    if (!bag)
        return result;

    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
            rt_bag_add(result, str);
            rt_str_release_maybe(str);
            entry = entry->next;
        }
    }

    return result;
}

/// @brief Creates a new Bag containing the difference of two Bags.
///
/// Returns a new Bag containing strings that appear in the first Bag but not
/// in the second. This is the set difference operation (A \ B).
///
/// **Set theory:**
/// ```
/// A = {apple, banana, cherry}
/// B = {banana, date}
/// A.Diff(B) = {apple, cherry}
/// ```
///
/// **Asymmetric operation:** A.Diff(B) is NOT the same as B.Diff(A). The first
/// Bag determines which elements are candidates; the second Bag acts as the
/// exclusion filter.
///
/// @param obj Pointer to the first Bag (source set). If NULL, returns empty Bag.
/// @param other Pointer to the second Bag (exclusion set). If NULL, returns a
///        copy of obj (nothing is excluded).
///
/// @return A new Bag containing strings in obj that are NOT in other.
///
/// @note O(n) time where n is the size of the first Bag.
/// @note The input Bags are not modified.
///
/// @see rt_bag_union For set union
/// @see rt_bag_intersect For set intersection
void *rt_bag_diff(void *obj, void *other) {
    void *result = rt_bag_new();
    if (!result || !obj)
        return result;

    rt_bag_impl *bag = as_bag(obj, "Bag.Diff: invalid Bag object");
    rt_bag_impl *other_bag = other ? as_bag(other, "Bag.Diff: invalid Bag object") : NULL;
    if (!bag || (other && !other_bag))
        return result;

    // For each element in first bag, check if it's NOT in second
    for (size_t i = 0; i < bag->capacity; ++i) {
        rt_bag_entry *entry = bag->buckets[i];
        while (entry) {
            rt_string str = rt_string_from_bytes(entry->key, entry->key_len);
            if (!other || !rt_bag_has(other, str)) {
                rt_bag_add(result, str);
            }
            rt_str_release_maybe(str);
            entry = entry->next;
        }
    }

    return result;
}
