//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_unionfind.h
/// @file
/// @brief Declares the GC-managed disjoint-set UnionFind runtime API.
///
// Purpose: Disjoint-set (Union-Find) data structure for efficient set merging and connectivity
// queries, using path compression and union by rank for near-constant-time operations.
//
// Key invariants:
//   - Elements are identified by integers in [0, n-1] set at creation.
//   - Uses path compression and union by rank for amortized O(alpha(n)) operations.
//   - rt_unionfind_connected returns 1 if two elements share a root.
//   - The number of sets starts at n and decreases by 1 per successful union.
//
// Ownership/Lifetime:
//   - UnionFind objects are GC-managed opaque pointers.
//   - Callers should not free unionfind objects directly.
//   - The element domain is fixed at construction and contains no runtime
//     object references. Find-like queries may mutate parent links through
//     path compression.
//
// Links: src/runtime/collections/rt_unionfind.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new Union-Find with n elements (0..n-1).
/// @param n Number of elements. Zero creates a one-element structure; negative
///          values trap.
/// @return New GC-managed UnionFind, or `NULL` after an allocation trap.
void *rt_unionfind_new(int64_t n);

/// @brief Find the representative of the set containing x.
/// @param uf Union-Find object.
/// @param x Element index (0-based).
/// @return Representative element, or `-1` for null/out-of-range input.
/// @note Compresses the traversed parent path.
int64_t rt_unionfind_find(void *uf, int64_t x);

/// @brief Find the representative root as an Option index.
/// @details This is the sentinel-free public API for callers that need to
///          distinguish an invalid element from a valid root. It preserves the
///          path-compression behavior of rt_unionfind_find.
/// @param uf Union-Find object, or NULL.
/// @param x Element index (0-based).
/// @return New Option containing the representative root, or `None`.
void *rt_unionfind_find_root_option(void *uf, int64_t x);

/// @brief Merge the sets containing x and y.
/// @param uf Union-Find object.
/// @param x First element index.
/// @param y Second element index.
/// @return 1 if sets were merged; 0 when already in the same set OR when
///         either index is invalid (out of range).
/// @note A successful merge decrements the disjoint-set count once.
int64_t rt_unionfind_union(void *uf, int64_t x, int64_t y);

/// @brief Check if x and y are in the same set.
/// @param uf Union-Find object.
/// @param x First element index.
/// @param y Second element index.
/// @return 1 only if both indexes are valid and connected; otherwise 0.
/// @note Performs path compression.
int8_t rt_unionfind_connected(void *uf, int64_t x, int64_t y);

/// @brief Get the number of disjoint sets.
/// @param uf Union-Find object.
/// @return Number of sets, or zero for a null object.
int64_t rt_unionfind_count(void *uf);

/// @brief Get the size of the set containing x.
/// @param uf Union-Find object.
/// @param x Element index.
/// @return Size of x's set, or zero for null/out-of-range input.
/// @note Performs path compression.
int64_t rt_unionfind_set_size(void *uf, int64_t x);

/// @brief Reset all elements to individual sets.
/// @param uf Union-Find object.
/// @note Restores the component count to the fixed element count in O(n).
void rt_unionfind_reset(void *uf);

#ifdef __cplusplus
}
#endif
