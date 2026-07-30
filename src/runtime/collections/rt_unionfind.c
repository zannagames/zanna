//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_unionfind.c
/// @file
/// @brief Implements the GC-managed disjoint-set UnionFind collection.
///
// Purpose: Implements a disjoint-set union-find data structure (also called
//   Union-Find or Merge-Find Set). Supports near-O(1) amortized Union and
//   Find operations using path compression and union by rank. Typical uses:
//   connected-component labeling, Kruskal's MST, cycle detection in graphs,
//   and dynamic connectivity queries.
//
// Key invariants:
//   - Elements are integers in [0, n-1] where n is fixed at construction.
//   - Three parallel arrays are allocated: parent[], rank[], and size[].
//     Initially parent[i] = i (each element is its own root), rank[i] = 0,
//     size[i] = 1.
//   - Find uses full path compression: every node on the path to the root is
//     directly linked to the root, making subsequent Finds O(1).
//   - Union uses union by rank: the shorter tree is attached under the taller
//     tree's root. Rank is incremented only when both trees have equal rank.
//   - `sets` tracks the number of disjoint sets; decremented by 1 on each
//     successful Union of two previously separate components.
//   - Find on an invalid element (out of range) returns -1 as an error sentinel.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - UnionFind objects are GC-managed (rt_obj_new_i64). The parent, rank, and
//     size arrays are freed by the GC finalizer (unionfind_finalizer).
//   - The arrays contain only integers, so the object requires no GC traversal
//     callback and does not retain references to other runtime objects.
//
// Links: src/runtime/collections/rt_unionfind.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_unionfind.h"
#include "rt_collection_ids.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

/// @brief Runtime object payload and parallel-array storage for UnionFind.
///
/// Only root indexes have meaningful `rank` and `size` entries. The element
/// count is fixed after construction; `sets` changes after unions and resets.
typedef struct {
    void *vptr;
    int64_t *parent; // Parent array (path-compressed)
    int64_t *rank;   // Rank array (for union by rank)
    int64_t *size;   // Size of each set
    int64_t n;       // Total number of elements
    int64_t sets;    // Number of disjoint sets
} rt_unionfind_impl;

/// @brief Checked cast of an opaque handle to the UnionFind implementation;
///        traps with @p what if @p obj is NULL or not a UnionFind.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic raised when validation fails.
/// @return Validated implementation pointer, or `NULL` after raising a trap.
static rt_unionfind_impl *as_unionfind(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_UNIONFIND_CLASS_ID, sizeof(rt_unionfind_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_unionfind_impl *)obj;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer that releases all three parallel integer arrays.
/// @param obj UnionFind instance being finalized; null is ignored.
static void unionfind_finalizer(void *obj) {
    rt_unionfind_impl *uf = obj ? as_unionfind(obj, "UnionFind: invalid UnionFind object") : NULL;
    if (!uf)
        return;
    free(uf->parent);
    free(uf->rank);
    free(uf->size);
    uf->parent = NULL;
    uf->rank = NULL;
    uf->size = NULL;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/// @brief Creates a UnionFind whose elements initially form singleton sets.
///
/// A requested size of zero is normalized to one element for compatibility.
/// Negative counts, allocation-size overflow, and allocation failure trap.
///
/// @param n Requested number of indexed elements.
/// @return New GC-managed UnionFind over indexes `[0, n)`, subject to zero
///         normalization, or `NULL` after a trap.
/// @note Construction initializes `parent[i] = i`, rank zero, size one, and
///       the disjoint-set count equal to the normalized element count.
void *rt_unionfind_new(int64_t n) {
    if (n < 0) {
        rt_trap("UnionFind: negative element count");
        return NULL;
    }
    if (n == 0)
        n = 1;
    if ((uint64_t)n > SIZE_MAX / sizeof(int64_t)) {
        rt_trap("UnionFind: allocation size overflow");
        return NULL;
    }

    rt_unionfind_impl *uf =
        (rt_unionfind_impl *)rt_obj_new_i64(RT_UNIONFIND_CLASS_ID, sizeof(rt_unionfind_impl));
    if (!uf) {
        rt_trap("UnionFind: memory allocation failed");
        return NULL;
    }
    uf->parent = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    uf->rank = (int64_t *)calloc((size_t)n, sizeof(int64_t));
    uf->size = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    if (!uf->parent || !uf->rank || !uf->size) {
        free(uf->parent);
        free(uf->rank);
        free(uf->size);
        if (rt_obj_release_check0(uf))
            rt_obj_free(uf);
        rt_trap("UnionFind: memory allocation failed");
        return NULL;
    }
    uf->n = n;
    uf->sets = n;

    for (int64_t i = 0; i < n; i++) {
        uf->parent[i] = i;
        uf->size[i] = 1;
    }

    rt_obj_set_finalizer(uf, unionfind_finalizer);
    return uf;
}

// ---------------------------------------------------------------------------
// Find (with path compression)
// ---------------------------------------------------------------------------

/// @brief Find the representative (root) of the set containing the element.
/// @details Applies path compression during traversal so subsequent
///          lookups on the same path are O(1) amortized.
/// @param uf_ptr UnionFind to query; null is treated as invalid input.
/// @param x Zero-based element index.
/// @return Representative index, or `-1` if @p uf_ptr is null or @p x is out
///         of range.
/// @note Although logically a query, this operation mutates parent links as
///       part of path compression. A wrong-class object raises a trap.
int64_t rt_unionfind_find(void *uf_ptr, int64_t x) {
    if (!uf_ptr)
        return -1;
    rt_unionfind_impl *uf = as_unionfind(uf_ptr, "UnionFind.Find: invalid UnionFind object");
    if (!uf)
        return -1;

    if (x < 0 || x >= uf->n)
        return -1;

    // Path compression (iterative)
    int64_t root = x;
    while (uf->parent[root] != root)
        root = uf->parent[root];

    // Compress path
    while (uf->parent[x] != root) {
        int64_t next = uf->parent[x];
        uf->parent[x] = root;
        x = next;
    }

    return root;
}

/// @brief Return the representative root as a Zanna.Option.
/// @param uf_ptr UnionFind object pointer, or NULL.
/// @param x Element index to resolve.
/// @return New `Some(root)` for valid elements, or new `None` for
///         null/out-of-range input.
/// @note Preserves the path-compression side effect of rt_unionfind_find().
void *rt_unionfind_find_root_option(void *uf_ptr, int64_t x) {
    int64_t root = rt_unionfind_find(uf_ptr, x);
    if (root < 0)
        return rt_option_none();
    return rt_option_some_i64(root);
}

// ---------------------------------------------------------------------------
// Union (by rank)
// ---------------------------------------------------------------------------

/// @brief Merge the sets containing two elements into one.
/// @details Uses union-by-rank to keep the tree balanced. After this
///          operation, find(a) == find(b).
/// @param uf_ptr UnionFind to mutate; null returns zero.
/// @param x First zero-based element index.
/// @param y Second zero-based element index.
/// @return One if two distinct components were merged; zero if either index
///         is invalid or both indexes already share a representative.
/// @note A successful merge decreases the component count once and adds the
///       absorbed root's size to the surviving root.
int64_t rt_unionfind_union(void *uf_ptr, int64_t x, int64_t y) {
    if (!uf_ptr)
        return 0;
    rt_unionfind_impl *uf = as_unionfind(uf_ptr, "UnionFind.Union: invalid UnionFind object");
    if (!uf)
        return 0;

    int64_t rx = rt_unionfind_find(uf, x);
    int64_t ry = rt_unionfind_find(uf, y);

    if (rx < 0 || ry < 0 || rx == ry)
        return 0;

    // Union by rank
    if (uf->rank[rx] < uf->rank[ry]) {
        int64_t tmp = rx;
        rx = ry;
        ry = tmp;
    }

    uf->parent[ry] = rx;
    uf->size[rx] += uf->size[ry];

    if (uf->rank[rx] == uf->rank[ry])
        uf->rank[rx]++;

    uf->sets--;
    return 1;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

/// @brief Check whether two elements belong to the same set.
/// @details Equivalent to find(a) == find(b).
/// @param uf_ptr UnionFind to query; null returns false.
/// @param x First zero-based element index.
/// @param y Second zero-based element index.
/// @return One only when both indexes are valid and share a representative.
/// @note Performs path compression for both valid indexes.
int8_t rt_unionfind_connected(void *uf_ptr, int64_t x, int64_t y) {
    if (!uf_ptr)
        return false;
    int64_t rx = rt_unionfind_find(uf_ptr, x);
    int64_t ry = rt_unionfind_find(uf_ptr, y);
    return (rx >= 0 && rx == ry);
}

/// @brief Return the number of disjoint sets currently in the structure.
/// @param uf_ptr UnionFind to query; null returns zero.
/// @return Current component count.
/// @note A wrong-class object raises a trap.
int64_t rt_unionfind_count(void *uf_ptr) {
    if (!uf_ptr)
        return 0;
    rt_unionfind_impl *uf = as_unionfind(uf_ptr, "UnionFind.Count: invalid UnionFind object");
    return uf ? uf->sets : 0;
}

/// @brief Return the number of elements in the set containing element @p x.
/// @details Finds the root of x's set and returns the size stored at the root.
/// @param uf_ptr Union-find object pointer; returns 0 if NULL.
/// @param x Element whose set size is queried.
/// @return Size of the set, or 0 if the element is invalid.
/// @note The lookup performs path compression. A wrong-class object traps.
int64_t rt_unionfind_set_size(void *uf_ptr, int64_t x) {
    if (!uf_ptr)
        return 0;
    int64_t root = rt_unionfind_find(uf_ptr, x);
    if (root < 0)
        return 0;
    rt_unionfind_impl *uf = as_unionfind(uf_ptr, "UnionFind.SetSize: invalid UnionFind object");
    return uf ? uf->size[root] : 0;
}

/// @brief Reset the union-find so every element is its own set.
/// @param uf_ptr UnionFind to reset; null is ignored.
/// @note Runs in O(n), resets every rank to zero and size to one, and restores
///       the component count to the fixed element count.
void rt_unionfind_reset(void *uf_ptr) {
    if (!uf_ptr)
        return;
    rt_unionfind_impl *uf = as_unionfind(uf_ptr, "UnionFind.Reset: invalid UnionFind object");
    if (!uf)
        return;

    for (int64_t i = 0; i < uf->n; i++) {
        uf->parent[i] = i;
        uf->rank[i] = 0;
        uf->size[i] = 1;
    }
    uf->sets = uf->n;
}
