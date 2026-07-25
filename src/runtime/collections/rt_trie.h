//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_trie.h
/// @file
/// @brief Declares the GC-managed bytewise Trie runtime API.
///
// Purpose: Prefix tree (Trie) for string keys supporting insertion, lookup, prefix search, and
// auto-complete queries with O(k) operations where k is the key length.
//
// Key invariants:
//   - Keys are stored as byte sequences using the full runtime string length.
//   - Prefix search returns all keys with the given prefix.
//   - rt_trie_has returns 1 only for exact key matches, not just prefixes.
//   - Values are retained while stored in the trie.
//   - Node traversal, clone, clear, and finalization use heap-backed iterative
//     walks so deeply nested keys do not overflow the C stack.
//
// Ownership/Lifetime:
//   - Trie objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//   - String keys are represented structurally as individual bytes; the Trie
//     does not retain the input string object. Values are retained while stored.
//   - Exact lookup returns a borrowed value. Key enumeration, longest-prefix
//     results, Options, and clones are newly created runtime objects.
//
// Links: src/runtime/collections/rt_trie.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty Trie.
/// @return New GC-managed Trie, or `NULL` after an allocation trap.
void *rt_trie_new(void);

/// @brief Get number of keys in the Trie.
/// @param obj Trie pointer.
/// @return Exact-key count, including a stored empty key.
/// @note A null pointer is treated as an empty Trie.
int64_t rt_trie_len(void *obj);

/// @brief Check if Trie is empty.
/// @param obj Trie pointer.
/// @return 1 if empty, 0 otherwise.
/// @note A null pointer is treated as empty.
int8_t rt_trie_is_empty(void *obj);

/// @brief Insert a key-value pair.
/// @param obj Trie pointer.
/// @param key Byte-string key; null denotes the empty key.
/// @param value Nullable object retained by the Trie.
/// @note Updating a key releases its previous value.
void rt_trie_set(void *obj, rt_string key, void *value);

/// @brief Get value for exact key match.
/// @param obj Trie pointer.
/// @param key Byte-string key; null denotes the empty key.
/// @return Borrowed value or `NULL` when absent or null-valued.
/// @note Use rt_trie_has() to distinguish absence from a stored null value.
void *rt_trie_get(void *obj, rt_string key);

/// @brief Check if exact key exists.
/// @param obj Trie pointer.
/// @param key Byte-string key; null denotes the empty key.
/// @return 1 if key exists, 0 otherwise.
int8_t rt_trie_has(void *obj, rt_string key);

/// @brief Check if any keys start with the given prefix.
/// @param obj Trie pointer.
/// @param prefix Byte prefix; null denotes the empty prefix.
/// @return 1 if any keys have this prefix, 0 otherwise.
int8_t rt_trie_has_prefix(void *obj, rt_string prefix);

/// @brief Get all keys that start with the given prefix.
/// @param obj Trie pointer.
/// @param prefix Byte prefix; null denotes the empty prefix.
/// @return New owning Seq of copied matching keys in bytewise order.
void *rt_trie_with_prefix(void *obj, rt_string prefix);

/// @brief Find the longest key that is a prefix of the given string.
/// @param obj Trie pointer.
/// @param str Byte string to match; null denotes empty.
/// @return New longest matching key string, or a new empty string if none.
/// @note Use rt_trie_longest_prefix_option() to distinguish an empty-key match.
rt_string rt_trie_longest_prefix(void *obj, rt_string str);

/// @brief LongestPrefix as an Option: Some(prefix) on any match (including the
///        empty key), None when no stored key is a prefix.
/// @param obj Trie pointer; null returns `None`.
/// @param str Byte string to match; null denotes empty.
/// @return New Option containing a copied matching key, or `None`.
void *rt_trie_longest_prefix_option(void *obj, rt_string str);

/// @brief Remove a key.
/// @details Releases the stored value and reclaims the key's now-empty node
///          suffix without disturbing terminal ancestors or shared prefixes.
/// @param obj Trie pointer.
/// @param key Exact byte-string key; null denotes the empty key.
/// @return 1 if removed, 0 if not found.
int8_t rt_trie_remove(void *obj, rt_string key);

/// @brief Remove all entries.
/// @param obj Trie pointer.
/// @note Releases every stored value and preserves a fresh empty root.
void rt_trie_clear(void *obj);

/// @brief Get all keys as an owning Seq of copied strings.
/// @param obj Trie pointer.
/// @return New owning Seq of all keys in bytewise lexicographic order.
void *rt_trie_keys(void *obj);

/// @brief Create a deep copy of the trie.
/// @param obj Trie pointer.
/// @return New Trie with independent nodes and retained shared values.
/// @note A null source produces a new empty Trie.
void *rt_trie_clone(void *obj);

#ifdef __cplusplus
}
#endif
