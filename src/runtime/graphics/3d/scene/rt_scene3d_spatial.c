//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_spatial.c
// Purpose: Scene3D spatial acceleration — the BVH spatial index (build/refit/
//   query), world-AABB bounds collection, and frustum/box/sphere candidate
//   gathering used by culling. Split out of rt_scene3d.c; shares private
//   structs/helpers via rt_scene3d_internal.h.
// Key invariants:
//   - Every BVH node range is bounded by the validated spatial-entry count.
//   - Bounds and centroids remain finite and ordered during build and refit.
// Ownership/Lifetime:
//   - Scene3D owns spatial entries, BVH nodes, and query scratch storage.
//   - Node, mesh, model, and frustum inputs are borrowed during each operation.
// Links: rt_scene3d_internal.h, vgfx3d_frustum.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Scene3D's world-bounds cache and bounding-volume hierarchy.
/// @details Drawable nodes become stable-order spatial entries. Topology changes
///   rebuild the BVH, transform/geometry/visibility changes refit affected paths,
///   and queries return visible candidate entries through bounded native buffers.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_animcontroller3d.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_pixels_internal.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_sound3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include "vgfx3d_frustum.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE3D_MORPH_BOUND_SCAN_MAX_TRIPLETS (1024u * 1024u)
#define SCENE3D_SPATIAL_ENTRY_MAX 1000000
#define SCENE3D_SPATIAL_NODE_MAX 2000000
#define SCENE3D_SPATIAL_ANCESTOR_MAX 1000000
#define SCENE3D_SPATIAL_RAW_MORPH_SHAPE_MAX 32

/// @brief Validate one finite ordered double-precision AABB.
/// @param minimum Borrowed minimum corner.
/// @param maximum Borrowed maximum corner.
/// @return Nonzero when all lanes are finite and ordered.
static int scene3d_spatial_bounds_valid(const double minimum[3], const double maximum[3]) {
    if (!minimum || !maximum)
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(minimum[axis]) || !isfinite(maximum[axis]) || minimum[axis] > maximum[axis])
            return 0;
    }
    return 1;
}

/// @brief Mark cached topology unusable without discarding reusable storage.
/// @param index Borrowed spatial index to invalidate.
static void scene3d_spatial_invalidate(rt_scene3d_spatial_index *index) {
    if (!index)
        return;
    index->valid = 0;
    index->dirty = 1;
    index->topology_dirty = 1;
    index->last_candidate_count = 0;
    index->last_prefiltered_count = 0;
}

/// @brief Repair native pointer/capacity tuples before rebuilding an index.
/// @details Counts and topology are always reset. Missing, negative, or
///   policy-exceeding allocations are discarded so reserve helpers cannot
///   mistake corrupt metadata for usable storage.
/// @param index Borrowed spatial index whose native storage is normalized.
static void scene3d_spatial_prepare_rebuild(rt_scene3d_spatial_index *index) {
    if (!index)
        return;
    if (index->entries && index->count > 0 && index->count <= index->capacity) {
        for (int32_t i = 0; i < index->count; i++) {
            if (rt_g3d_has_class(index->entries[i].node, RT_G3D_SCENENODE3D_CLASS_ID))
                index->entries[i].node->spatial_entry_index = -1;
        }
    }
    if (!index->entries || index->capacity < 0 || index->capacity > SCENE3D_SPATIAL_ENTRY_MAX) {
        free(index->entries);
        index->entries = NULL;
        index->capacity = 0;
    }
    if (!index->entry_indices || index->entry_index_capacity < 0 ||
        index->entry_index_capacity > SCENE3D_SPATIAL_ENTRY_MAX) {
        free(index->entry_indices);
        index->entry_indices = NULL;
        index->entry_index_capacity = 0;
    }
    if (!index->nodes || index->node_capacity < 0 ||
        index->node_capacity > SCENE3D_SPATIAL_NODE_MAX) {
        free(index->nodes);
        index->nodes = NULL;
        index->node_capacity = 0;
    }
    index->count = 0;
    index->node_count = 0;
    index->root_node = -1;
    scene3d_spatial_invalidate(index);
}

/// @brief Validate cached aggregate storage metadata in constant time.
/// @param index Borrowed spatial index.
/// @return Nonzero when all live ranges fit their recorded allocations.
static int scene3d_spatial_storage_valid(const rt_scene3d_spatial_index *index) {
    if (!index || index->count < 0 || index->count > SCENE3D_SPATIAL_ENTRY_MAX ||
        index->capacity < index->count || index->capacity > SCENE3D_SPATIAL_ENTRY_MAX ||
        index->entry_index_capacity < index->count ||
        index->entry_index_capacity > SCENE3D_SPATIAL_ENTRY_MAX || index->node_count < 0 ||
        index->node_count > SCENE3D_SPATIAL_NODE_MAX || index->node_capacity < index->node_count ||
        index->node_capacity > SCENE3D_SPATIAL_NODE_MAX)
        return 0;
    if (index->count == 0)
        return index->node_count == 0 && index->root_node == -1;
    return index->entries && index->entry_indices && index->nodes && index->node_count > 0 &&
           index->root_node >= 0 && index->root_node < index->node_count &&
           index->nodes[index->root_node].parent == -1;
}

/// @brief Validate one cached entry before query/refit dereference.
/// @param index Borrowed aggregate-valid spatial index.
/// @param entry_index Candidate zero-based entry index.
/// @return Nonzero when the entry references a typed node and canonical bounds/state.
static int scene3d_spatial_entry_valid(const rt_scene3d_spatial_index *index, int32_t entry_index) {
    const rt_scene3d_spatial_entry *entry;
    if (!index || !index->entries || entry_index < 0 || entry_index >= index->count)
        return 0;
    entry = &index->entries[entry_index];
    return rt_g3d_has_class(entry->node, RT_G3D_SCENENODE3D_CLASS_ID) &&
           scene3d_spatial_bounds_valid(entry->world_min, entry->world_max) &&
           (entry->cullable == 0 || entry->cullable == 1) &&
           (entry->visible == 0 || entry->visible == 1);
}

/// @brief Add a non-negative count without signed overflow.
/// @param total Mutable accumulator.
/// @param amount Candidate increment.
static void scene3d_spatial_saturating_add(int32_t *total, int32_t amount) {
    if (!total || amount <= 0 || *total >= INT32_MAX)
        return;
    *total = amount > INT32_MAX - *total ? INT32_MAX : *total + amount;
}

/// @brief Detect malformed or cyclic SceneNode parent chains with bounded work.
/// @param node Borrowed starting node.
/// @return Nonzero only when the complete chain is class-valid and acyclic.
static int scene3d_spatial_parent_chain_valid(rt_scene_node3d *node) {
    rt_scene_node3d *slow = node;
    rt_scene_node3d *fast = node;
    int32_t steps = 0;
    while (fast && steps++ < SCENE3D_SPATIAL_ANCESTOR_MAX) {
        if (!rt_g3d_has_class(fast, RT_G3D_SCENENODE3D_CLASS_ID))
            return 0;
        fast = fast->parent;
        if (fast) {
            if (!rt_g3d_has_class(fast, RT_G3D_SCENENODE3D_CLASS_ID))
                return 0;
            fast = fast->parent;
        }
        if (slow) {
            if (!rt_g3d_has_class(slow, RT_G3D_SCENENODE3D_CLASS_ID))
                return 0;
            slow = slow->parent;
        }
        if (fast && slow == fast)
            return 0;
    }
    return fast == NULL;
}

/// @brief Ensure the spatial index can hold @p needed entries, growing by doubling (min 64).
/// @param index Borrowed index whose native entry allocation may grow.
/// @param needed Non-negative minimum entry capacity.
/// @return 1 on success, 0 on bad args, overflow, or allocation failure.
static int scene3d_spatial_ensure_capacity(rt_scene3d_spatial_index *index, int32_t needed) {
    int32_t new_capacity;
    rt_scene3d_spatial_entry *grown;
    if (!index || needed < 0 || needed > SCENE3D_SPATIAL_ENTRY_MAX)
        return 0;
    if (needed <= index->capacity)
        return 1;
    new_capacity = index->capacity < 64 ? 64 : index->capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(index->entries[0]))
        return 0;
    grown = (rt_scene3d_spatial_entry *)realloc(index->entries,
                                                (size_t)new_capacity * sizeof(index->entries[0]));
    if (!grown)
        return 0;
    index->entries = grown;
    index->capacity = new_capacity;
    return 1;
}

/// @brief Ensure the index's entry-index array (BVH leaf ordering) holds @p needed slots.
/// @param index Borrowed index whose ordering allocation may grow.
/// @param needed Non-negative minimum index capacity.
/// @return Nonzero when sufficient storage is available, otherwise zero.
static int scene3d_spatial_ensure_entry_index_capacity(rt_scene3d_spatial_index *index,
                                                       int32_t needed) {
    int32_t new_capacity;
    int32_t *grown;
    if (!index || needed < 0 || needed > SCENE3D_SPATIAL_ENTRY_MAX)
        return 0;
    if (needed <= index->entry_index_capacity)
        return 1;
    new_capacity = index->entry_index_capacity < 64 ? 64 : index->entry_index_capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(index->entry_indices[0]))
        return 0;
    grown = (int32_t *)realloc(index->entry_indices,
                               (size_t)new_capacity * sizeof(index->entry_indices[0]));
    if (!grown)
        return 0;
    index->entry_indices = grown;
    index->entry_index_capacity = new_capacity;
    return 1;
}

/// @brief Ensure the index's BVH node array holds @p needed nodes (doubling growth).
/// @param index Borrowed index whose native BVH-node allocation may grow.
/// @param needed Non-negative minimum node capacity.
/// @return Nonzero when sufficient storage is available, otherwise zero.
static int scene3d_spatial_ensure_bvh_node_capacity(rt_scene3d_spatial_index *index,
                                                    int32_t needed) {
    int32_t new_capacity;
    rt_scene3d_spatial_bvh_node *grown;
    if (!index || needed < 0 || needed > SCENE3D_SPATIAL_NODE_MAX)
        return 0;
    if (needed <= index->node_capacity)
        return 1;
    new_capacity = index->node_capacity < 64 ? 64 : index->node_capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(index->nodes[0]))
        return 0;
    grown = (rt_scene3d_spatial_bvh_node *)realloc(index->nodes,
                                                   (size_t)new_capacity * sizeof(index->nodes[0]));
    if (!grown)
        return 0;
    index->nodes = grown;
    index->node_capacity = new_capacity;
    return 1;
}

/// @brief Append a spatial entry to a query's candidate list, growing it as needed.
/// @param list Caller-owned candidate list.
/// @param entry Borrowed spatial entry to append.
/// @return Nonzero after append, otherwise zero on invalid input, overflow, or allocation failure.
static int scene3d_spatial_candidate_push(scene3d_spatial_candidate_list_t *list,
                                          rt_scene3d_spatial_entry *entry) {
    int32_t new_capacity;
    rt_scene3d_spatial_entry **grown;
    if (!list || !entry)
        return 0;
    if (!list->items || list->capacity < 0 || list->capacity > SCENE3D_SPATIAL_ENTRY_MAX) {
        free(list->items);
        list->items = NULL;
        list->count = 0;
        list->capacity = 0;
    } else if (list->count < 0 || list->count > list->capacity) {
        list->count = 0;
    }
    if (list->count >= SCENE3D_SPATIAL_ENTRY_MAX)
        return 0;
    if (list->count >= list->capacity) {
        if (list->capacity > SCENE3D_SPATIAL_ENTRY_MAX / 2)
            new_capacity = SCENE3D_SPATIAL_ENTRY_MAX;
        else
            new_capacity = list->capacity < 64 ? 64 : list->capacity * 2;
        if (new_capacity <= list->capacity || new_capacity > SCENE3D_SPATIAL_ENTRY_MAX ||
            (size_t)new_capacity > SIZE_MAX / sizeof(list->items[0]))
            return 0;
        grown = (rt_scene3d_spatial_entry **)realloc(list->items,
                                                     (size_t)new_capacity * sizeof(list->items[0]));
        if (!grown)
            return 0;
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = entry;
    return 1;
}

/// @brief Stably order query candidates by dense traversal ordinal in linear time.
/// @details Four byte-wise radix passes replace the comparison sort formerly paid by every BVH
///   query. Scratch storage is retained by the index and grows only past its high-water mark.
static int scene3d_spatial_order_candidates(rt_scene3d_spatial_index *index,
                                            scene3d_spatial_candidate_list_t *list) {
    int32_t capacity;
    rt_scene3d_spatial_entry **grown;
    rt_scene3d_spatial_entry **source;
    rt_scene3d_spatial_entry **destination;
    if (!index || !list || list->count < 0 || list->count > list->capacity)
        return 0;
    if (list->count <= 1)
        return 1;
    if (list->count > index->query_order_scratch_capacity) {
        capacity =
            index->query_order_scratch_capacity > 0 ? index->query_order_scratch_capacity : 64;
        while (capacity < list->count) {
            if (capacity > SCENE3D_SPATIAL_ENTRY_MAX / 2) {
                capacity = list->count;
                break;
            }
            capacity *= 2;
        }
        if (capacity < list->count || capacity > SCENE3D_SPATIAL_ENTRY_MAX ||
            (size_t)capacity > SIZE_MAX / sizeof(*grown))
            return 0;
        grown = (rt_scene3d_spatial_entry **)realloc(index->query_order_scratch,
                                                     (size_t)capacity * sizeof(*grown));
        if (!grown)
            return 0;
        index->query_order_scratch = grown;
        index->query_order_scratch_capacity = capacity;
    }
    source = list->items;
    destination = index->query_order_scratch;
    for (unsigned pass = 0; pass < 4u; pass++) {
        size_t counts[256] = {0};
        size_t offsets[256];
        unsigned shift = pass * 8u;
        for (int32_t i = 0; i < list->count; i++) {
            uint32_t key;
            if (!source[i])
                return 0;
            key = ((uint32_t)source[i]->traversal_order) ^ UINT32_C(0x80000000);
            counts[(key >> shift) & 0xffu]++;
        }
        offsets[0] = 0u;
        for (size_t bucket = 1; bucket < 256u; bucket++)
            offsets[bucket] = offsets[bucket - 1u] + counts[bucket - 1u];
        for (int32_t i = 0; i < list->count; i++) {
            uint32_t key = ((uint32_t)source[i]->traversal_order) ^ UINT32_C(0x80000000);
            size_t bucket = (key >> shift) & 0xffu;
            destination[offsets[bucket]++] = source[i];
        }
        {
            rt_scene3d_spatial_entry **tmp = source;
            source = destination;
            destination = tmp;
        }
    }
    return source == list->items;
}

/// @brief Push a BVH node index onto a query's traversal stack, growing it as needed.
/// @param stack Caller-owned traversal stack.
/// @param node_index Non-negative zero-based BVH-node index.
/// @return Nonzero after append, otherwise zero on invalid input, overflow, or allocation failure.
static int scene3d_spatial_node_stack_push(scene3d_spatial_node_stack_t *stack,
                                           int32_t node_index) {
    int32_t new_capacity;
    int32_t *grown;
    if (!stack || node_index < 0 || node_index >= SCENE3D_SPATIAL_NODE_MAX)
        return 0;
    if (!stack->items || stack->capacity < 0 || stack->capacity > SCENE3D_SPATIAL_NODE_MAX) {
        free(stack->items);
        stack->items = NULL;
        stack->count = 0;
        stack->capacity = 0;
    } else if (stack->count < 0 || stack->count > stack->capacity) {
        stack->count = 0;
    }
    if (stack->count >= SCENE3D_SPATIAL_NODE_MAX)
        return 0;
    if (stack->count >= stack->capacity) {
        if (stack->capacity > SCENE3D_SPATIAL_NODE_MAX / 2)
            new_capacity = SCENE3D_SPATIAL_NODE_MAX;
        else
            new_capacity = stack->capacity < 64 ? 64 : stack->capacity * 2;
        if (new_capacity <= stack->capacity || new_capacity > SCENE3D_SPATIAL_NODE_MAX ||
            (size_t)new_capacity > SIZE_MAX / sizeof(stack->items[0]))
            return 0;
        grown = (int32_t *)realloc(stack->items, (size_t)new_capacity * sizeof(stack->items[0]));
        if (!grown)
            return 0;
        stack->items = grown;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = node_index;
    return 1;
}

/// @brief Publish a query traversal stack back to its owning reusable scratch tuple.
static void scene3d_spatial_store_query_stack(rt_scene3d_spatial_index *index,
                                              scene3d_spatial_node_stack_t *stack) {
    if (!index || !stack)
        return;
    index->query_stack = stack->items;
    index->query_stack_capacity = stack->capacity;
    stack->items = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

/// @brief Centroid of a spatial entry's world AABB along @p axis (used to choose BVH split planes).
/// @param index Borrowed spatial index.
/// @param entry_index Zero-based entry index.
/// @param axis Coordinate axis in `[0, 2]`.
/// @return Selected centroid coordinate, or zero for invalid input.
static double scene3d_spatial_entry_centroid_axis(const rt_scene3d_spatial_index *index,
                                                  int32_t entry_index,
                                                  int axis) {
    const rt_scene3d_spatial_entry *entry;
    if (!index || entry_index < 0 || entry_index >= index->count || axis < 0 || axis > 2)
        return 0.0;
    entry = &index->entries[entry_index];
    return 0.5 * entry->world_min[axis] + 0.5 * entry->world_max[axis];
}

/// @brief Ordering predicate for two entry indices along @p axis (by centroid, traversal-order
/// tiebreak).
/// @param index Borrowed spatial index.
/// @param a First zero-based entry index.
/// @param b Second zero-based entry index.
/// @param axis Coordinate axis in `[0, 2]`.
/// @return Nonzero when @p a sorts before @p b.
static int scene3d_spatial_entry_index_less(const rt_scene3d_spatial_index *index,
                                            int32_t a,
                                            int32_t b,
                                            int axis) {
    double ca = scene3d_spatial_entry_centroid_axis(index, a, axis);
    double cb = scene3d_spatial_entry_centroid_axis(index, b, axis);
    if (ca < cb)
        return 1;
    if (ca > cb)
        return 0;
    return index && a >= 0 && b >= 0 && a < index->count && b < index->count
               ? index->entries[a].traversal_order < index->entries[b].traversal_order
               : a < b;
}

/// @brief Partition a sub-range so the requested median slot contains its sorted value.
/// @details Iterative median-of-three quickselect avoids recursively sorting every complete BVH
///   subtree. The two resulting ranges remain unordered because their children will select only
///   the medians they actually require.
/// @param index Borrowed index whose ordering array is sorted.
/// @param start Zero-based first ordering-array slot.
/// @param count Number of slots in the subrange.
/// @param axis Coordinate axis in `[0, 2]`.
static void scene3d_spatial_select_entry_index(
    rt_scene3d_spatial_index *index, int32_t start, int32_t count, int32_t nth, int axis) {
    int32_t left = start;
    int32_t right = start + count - 1;
    if (!index || !index->entry_indices || count <= 1 || axis < 0 || axis > 2)
        return;
    if (nth < left || nth > right)
        return;
    while (left < right) {
        int32_t middle = left + (right - left) / 2;
        int32_t a = index->entry_indices[left];
        int32_t b = index->entry_indices[middle];
        int32_t c = index->entry_indices[right];
        int32_t pivot_slot = middle;
        int32_t pivot;
        int32_t store = left;
        if (scene3d_spatial_entry_index_less(index, b, a, axis)) {
            int32_t tmp = a;
            a = b;
            b = tmp;
        }
        if (scene3d_spatial_entry_index_less(index, c, b, axis)) {
            b = c;
            if (scene3d_spatial_entry_index_less(index, b, a, axis))
                b = a;
        }
        if (b == index->entry_indices[left])
            pivot_slot = left;
        else if (b == index->entry_indices[right])
            pivot_slot = right;
        pivot = index->entry_indices[pivot_slot];
        index->entry_indices[pivot_slot] = index->entry_indices[right];
        index->entry_indices[right] = pivot;
        for (int32_t i = left; i < right; i++) {
            if (scene3d_spatial_entry_index_less(index, index->entry_indices[i], pivot, axis)) {
                int32_t tmp = index->entry_indices[store];
                index->entry_indices[store++] = index->entry_indices[i];
                index->entry_indices[i] = tmp;
            }
        }
        index->entry_indices[right] = index->entry_indices[store];
        index->entry_indices[store] = pivot;
        if (store == nth)
            return;
        if (nth < store)
            right = store - 1;
        else
            left = store + 1;
    }
}

/// @brief Expand AABB [out_min, out_max] in place to also contain AABB [in_min, in_max].
/// @param out_min Mutable accumulated minimum corner.
/// @param out_max Mutable accumulated maximum corner.
/// @param in_min Borrowed minimum corner to include.
/// @param in_max Borrowed maximum corner to include.
static void scene3d_spatial_bounds_include(double out_min[3],
                                           double out_max[3],
                                           const double in_min[3],
                                           const double in_max[3]) {
    scene_bounds_include_point_d(out_min, out_max, in_min);
    scene_bounds_include_point_d(out_min, out_max, in_max);
}

/// @brief Choose the BVH split axis as the one with the greatest spread of entry centroids.
/// @details Splitting along the widest centroid extent yields tighter, better-balanced child nodes.
/// @param index Borrowed spatial index and ordering array.
/// @param start Zero-based first ordering-array slot.
/// @param count Positive number of entries in the range.
/// @return Coordinate axis `0`, `1`, or `2`.
static int scene3d_spatial_choose_split_axis(const rt_scene3d_spatial_index *index,
                                             int32_t start,
                                             int32_t count) {
    double centroid_min[3];
    double centroid_max[3];
    double spread[3];
    int axis = 0;
    scene_bounds_reset_d(centroid_min, centroid_max);
    for (int32_t i = start; i < start + count; ++i) {
        int32_t entry_index = index->entry_indices[i];
        double centroid[3] = {scene3d_spatial_entry_centroid_axis(index, entry_index, 0),
                              scene3d_spatial_entry_centroid_axis(index, entry_index, 1),
                              scene3d_spatial_entry_centroid_axis(index, entry_index, 2)};
        scene_bounds_include_point_d(centroid_min, centroid_max, centroid);
    }
    spread[0] = centroid_max[0] - centroid_min[0];
    spread[1] = centroid_max[1] - centroid_min[1];
    spread[2] = centroid_max[2] - centroid_min[2];
    if (spread[1] > spread[axis])
        axis = 1;
    if (spread[2] > spread[axis])
        axis = 2;
    return axis;
}

/// @brief Allocate and zero-initialize a new BVH node (children/parent set to -1). Returns its
/// index or -1.
/// @param index Borrowed spatial index whose native node array may grow.
/// @return New zero-based BVH-node index, or `-1` on failure.
static int scene3d_spatial_alloc_bvh_node(rt_scene3d_spatial_index *index) {
    int32_t node_index;
    if (!index || index->node_count < 0 || index->node_count >= SCENE3D_SPATIAL_NODE_MAX ||
        !scene3d_spatial_ensure_bvh_node_capacity(index, index->node_count + 1))
        return -1;
    node_index = index->node_count++;
    memset(&index->nodes[node_index], 0, sizeof(index->nodes[node_index]));
    index->nodes[node_index].left = -1;
    index->nodes[node_index].right = -1;
    index->nodes[node_index].parent = -1;
    return node_index;
}

/// @brief Recursively build a BVH subtree over entry-index range [start, start+count).
/// @details Computes the node's bounds and cullable count; ranges of <= 8 entries become leaves,
///          larger ranges select the median on the widest centroid axis and split at the midpoint
///          into two child nodes. Returns the node index, or -1 on allocation failure.
/// @param index Borrowed spatial index under construction.
/// @param start Zero-based first ordering-array slot in this subtree.
/// @param count Positive number of entries covered by this subtree.
/// @return Root BVH-node index for the range, or `-1` on invalid input/failure.
static int scene3d_spatial_build_bvh_range(rt_scene3d_spatial_index *index,
                                           int32_t start,
                                           int32_t count) {
    enum { SCENE3D_SPATIAL_LEAF_SIZE = 8 };

    int32_t node_index;
    rt_scene3d_spatial_bvh_node *node;
    if (!index || !index->entries || !index->entry_indices || start < 0 || count <= 0 ||
        start > index->count - count || start > index->entry_index_capacity - count)
        return -1;
    node_index = scene3d_spatial_alloc_bvh_node(index);
    if (node_index < 0)
        return -1;
    node = &index->nodes[node_index];
    scene_bounds_reset_d(node->world_min, node->world_max);
    node->start = start;
    node->count = count;
    for (int32_t i = start; i < start + count; ++i) {
        int32_t entry_index = index->entry_indices[i];
        rt_scene3d_spatial_entry *entry;
        if (entry_index < 0 || entry_index >= index->count)
            return -1;
        entry = &index->entries[entry_index];
        if (!scene3d_spatial_bounds_valid(entry->world_min, entry->world_max))
            return -1;
        scene3d_spatial_bounds_include(
            node->world_min, node->world_max, entry->world_min, entry->world_max);
        if (entry->cullable)
            node->cullable_count++;
    }
    if (count <= SCENE3D_SPATIAL_LEAF_SIZE) {
        node->leaf = 1;
        for (int32_t i = start; i < start + count; ++i)
            index->entries[index->entry_indices[i]].leaf_node = node_index;
        return node_index;
    }
    {
        int axis = scene3d_spatial_choose_split_axis(index, start, count);
        int32_t left_count = count / 2;
        int32_t right_count = count - left_count;
        int32_t left_node;
        int32_t right_node;
        scene3d_spatial_select_entry_index(index, start, count, start + left_count, axis);
        left_node = scene3d_spatial_build_bvh_range(index, start, left_count);
        right_node = scene3d_spatial_build_bvh_range(index, start + left_count, right_count);
        if (left_node < 0 || right_node < 0)
            return -1;
        node = &index->nodes[node_index];
        node->left = left_node;
        node->right = right_node;
        index->nodes[left_node].parent = node_index;
        index->nodes[right_node].parent = node_index;
    }
    return node_index;
}

/// @brief Whether a mesh deforms at runtime (skeletal animator or morph targets present).
/// @details Deforming meshes need looser/refit bounds each frame, so the spatial index treats them
///          differently from static geometry during culling.
/// @param mesh Borrowed Mesh3D payload.
/// @param effective_animator Borrowed inherited animator, or `NULL`.
/// @return Nonzero when skeletal or morph deformation may change geometry.
int scene3d_mesh_has_dynamic_deformation(rt_mesh3d *mesh, void *effective_animator) {
    return mesh && (effective_animator != NULL || mesh->morph_targets_ref != NULL ||
                    mesh->morph_deltas != NULL || mesh->morph_weights != NULL ||
                    mesh->morph_shape_count > 0);
}

/// @brief Validate the raw transient morph-delta span before scanning it for culling bounds.
/// @param mesh Borrowed Mesh3D payload with transient packed delta triplets.
/// @param out_count Optional output receiving the bounded shape-times-vertex triplet count.
/// @return Nonzero when the raw span is present and safe to scan.
static int scene3d_morph_delta_triplet_count(const rt_mesh3d *mesh, size_t *out_count) {
    size_t shape_count;
    size_t vertex_count;
    size_t total;
    uint32_t safe_vertex_count;
    if (out_count)
        *out_count = 0;
    safe_vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    if (!mesh || !mesh->morph_deltas || mesh->morph_shape_count <= 0 ||
        mesh->morph_shape_count > SCENE3D_SPATIAL_RAW_MORPH_SHAPE_MAX || safe_vertex_count == 0)
        return 0;
    shape_count = (size_t)mesh->morph_shape_count;
    vertex_count = (size_t)safe_vertex_count;
    if (shape_count > SIZE_MAX / vertex_count)
        return 0;
    total = shape_count * vertex_count;
    if (total == 0 || total > SCENE3D_MORPH_BOUND_SCAN_MAX_TRIPLETS)
        return 0;
    if (out_count)
        *out_count = total;
    return 1;
}

/// @brief Euclidean length of a 3-component morph delta (as a double); returns 0 when any
///   lane is non-finite.
/// @param delta Borrowed three-float morph-position delta.
/// @return Finite Euclidean magnitude, or zero for invalid input.
static double scene3d_morph_delta_length_or_zero(const float *delta) {
    double x;
    double y;
    double z;
    double len;
    if (!delta || !isfinite(delta[0]) || !isfinite(delta[1]) || !isfinite(delta[2]))
        return 0.0;
    x = fabs((double)delta[0]);
    y = fabs((double)delta[1]);
    z = fabs((double)delta[2]);
    len = hypot(hypot(x, y), z);
    return isfinite(len) ? len : 0.0;
}

/// @brief Return cached conservative padding for raw transient morph-delta arrays.
/// @details DrawMeshMorphed can attach a raw `morph_deltas` pointer to a mesh for the duration of
///   one draw. The source array often stays stable across frames, so caching the maximum delta
///   length avoids repeatedly scanning every shape/vertex during Scene3D culling. The cache key
///   includes the pointer, shape count, safe vertex count, and geometry revision so geometry edits
///   or a different morph payload force a rescan.
/// @param mesh Borrowed mutable Mesh3D payload whose bound cache may be refreshed.
/// @return Conservative non-negative local-space padding, or zero when unavailable.
static double scene3d_cached_raw_morph_bound_pad(rt_mesh3d *mesh) {
    size_t total = 0;
    double max_len = 0.0;
    uint32_t safe_vertex_count;

    if (!mesh || !mesh->morph_deltas || mesh->morph_shape_count <= 0)
        return 0.0;
    safe_vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    if (safe_vertex_count == 0)
        return 0.0;
    if (mesh->morph_bound_valid && mesh->morph_bound_deltas_source == mesh->morph_deltas &&
        mesh->morph_bound_revision == mesh->geometry_revision &&
        mesh->morph_bound_vertex_count == safe_vertex_count &&
        mesh->morph_bound_shape_count == mesh->morph_shape_count) {
        return scene3d_distance_or_zero(mesh->morph_bound_pad);
    }
    mesh->morph_bound_deltas_source = mesh->morph_deltas;
    mesh->morph_bound_revision = mesh->geometry_revision;
    mesh->morph_bound_vertex_count = safe_vertex_count;
    mesh->morph_bound_shape_count = mesh->morph_shape_count;
    mesh->morph_bound_pad = 0.0;
    mesh->morph_bound_valid = 1;

    if (mesh->morph_shape_count > SCENE3D_SPATIAL_RAW_MORPH_SHAPE_MAX)
        return 0.0;
    if (!scene3d_morph_delta_triplet_count(mesh, &total) || total == 0) {
        mesh->morph_bound_pad = SCENE3D_ABS_MAX;
        return mesh->morph_bound_pad;
    }
    for (size_t i = 0; i < total; i++) {
        const float *d = mesh->morph_deltas + i * 3u;
        double len = scene3d_morph_delta_length_or_zero(d);
        if (len > max_len)
            max_len = len;
    }
    mesh->morph_bound_pad = scene3d_distance_or_zero(max_len);
    return mesh->morph_bound_pad;
}

/// @brief Conservative local-space padding for runtime-deformed mesh bounds.
/// @param mesh Borrowed Mesh3D payload.
/// @param effective_animator Borrowed inherited animator, or `NULL`.
/// @param base_radius Sanitized fallback for skeletal or otherwise unbounded deformation.
/// @return Non-negative local-space bound padding, or zero for static geometry.
double scene3d_mesh_dynamic_bound_pad(rt_mesh3d *mesh,
                                      void *effective_animator,
                                      double base_radius) {
    double pad = 0.0;
    if (!scene3d_mesh_has_dynamic_deformation(mesh, effective_animator))
        return 0.0;
    if (mesh && mesh->morph_targets_ref) {
        double morph_pad = rt_morphtarget3d_get_max_position_delta(mesh->morph_targets_ref);
        if (isfinite(morph_pad) && morph_pad > pad)
            pad = scene3d_distance_or_zero(morph_pad);
    }
    if (mesh && mesh->morph_deltas && mesh->morph_shape_count > 0 &&
        rt_mesh3d_safe_vertex_count(mesh) > 0) {
        double raw_morph_pad = scene3d_cached_raw_morph_bound_pad(mesh);
        if (raw_morph_pad > pad)
            pad = raw_morph_pad;
    }
    if (effective_animator) {
        /* Skinned variant meshes store bone-local vertices, so the static AABB
         * (and its radius) describe a tiny cluster near the node origin — not
         * the posed character. Every skinned position is palette*v, bounded by
         * the largest palette translation plus the cluster radius, so pad by
         * the posed-skeleton extent read from the live palette. */
        int32_t bone_count = 0;
        const float *palette =
            rt_anim_controller3d_get_final_palette_data(effective_animator, &bone_count);
        if (palette && bone_count > 0) {
            double max_reach = 0.0;
            if (bone_count > VGFX3D_MAX_SKELETON_BONES) {
                max_reach = SCENE3D_ABS_MAX;
            } else {
                for (int32_t b = 0; b < bone_count; b++) {
                    const float *m = palette + (size_t)b * 16u;
                    double t =
                        hypot(hypot(fabs((double)m[3]), fabs((double)m[7])), fabs((double)m[11]));
                    if (!isfinite(t)) {
                        max_reach = SCENE3D_ABS_MAX;
                        break;
                    }
                    if (t > max_reach)
                        max_reach = t;
                }
            }
            double skel_pad =
                scene3d_distance_or_zero(max_reach + scene3d_distance_or_zero(base_radius));
            if (skel_pad > pad)
                pad = skel_pad;
        }
    }
    if (effective_animator || (mesh && mesh->morph_shape_count > 0 && pad <= 0.0)) {
        double fallback = scene3d_distance_or_zero(base_radius);
        if (fallback > pad)
            pad = fallback;
    }
    return pad;
}

/// @brief Expand world AABB [out_min, out_max] in place to also contain [in_min, in_max].
/// @param out_min Mutable accumulated world minimum.
/// @param out_max Mutable accumulated world maximum.
/// @param in_min Borrowed world minimum to include.
/// @param in_max Borrowed world maximum to include.
static void scene3d_bounds_include_world_aabb(double out_min[3],
                                              double out_max[3],
                                              const double in_min[3],
                                              const double in_max[3]) {
    if (!out_min || !out_max || !in_min || !in_max)
        return;
    scene_bounds_include_point_d(out_min, out_max, in_min);
    scene_bounds_include_point_d(out_min, out_max, in_max);
}

/// @brief Accumulate a single node's mesh world-space AABB into the running bounds.
/// @param node Borrowed node supplying the current world matrix.
/// @param mesh_obj Borrowed candidate Mesh3D handle.
/// @param effective_animator Borrowed inherited animator affecting deformation.
/// @param out_min Mutable accumulated world minimum.
/// @param out_max Mutable accumulated world maximum.
/// @param out_radius Optional in/out largest undeformed mesh radius.
/// @return Nonzero when valid transformed bounds were included.
static int scene3d_include_mesh_world_bounds(rt_scene_node3d *node,
                                             void *mesh_obj,
                                             void *effective_animator,
                                             double out_min[3],
                                             double out_max[3],
                                             double *out_radius) {
    rt_mesh3d *mesh =
        rt_g3d_has_class(mesh_obj, RT_G3D_MESH3D_CLASS_ID) ? (rt_mesh3d *)mesh_obj : NULL;
    float local_min[3];
    float local_max[3];
    double world_min[3];
    double world_max[3];
    float radius_f = 0.0f;
    double radius = 0.0;
    if (!node || !mesh || !out_min || !out_max)
        return 0;
    scene_mesh_bounds(mesh, local_min, local_max, &radius_f);
    radius = (double)radius_f;
    if (scene3d_mesh_has_dynamic_deformation(mesh, effective_animator)) {
        double pad = scene3d_mesh_dynamic_bound_pad(mesh, effective_animator, radius);
        local_min[0] = scene3d_float_or_zero((double)local_min[0] - pad);
        local_min[1] = scene3d_float_or_zero((double)local_min[1] - pad);
        local_min[2] = scene3d_float_or_zero((double)local_min[2] - pad);
        local_max[0] = scene3d_float_or_zero((double)local_max[0] + pad);
        local_max[1] = scene3d_float_or_zero((double)local_max[1] + pad);
        local_max[2] = scene3d_float_or_zero((double)local_max[2] + pad);
    }
    if (!scene3d_transform_aabb_d(local_min, local_max, node->world_matrix, world_min, world_max))
        return 0;
    scene3d_bounds_include_world_aabb(out_min, out_max, world_min, world_max);
    if (out_radius && radius > *out_radius)
        *out_radius = radius;
    return 1;
}

/// @brief Compute the union world AABB of a node's drawable geometry variants.
/// @details Includes the base mesh, all authored LOD meshes, and the impostor mesh. The result is
///   intentionally conservative so frustum/PVS culling uses one stable bound while the visible mesh
///   swaps between LODs.
/// @param node Borrowed SceneNode3D payload.
/// @param effective_animator Borrowed inherited animator, or `NULL`.
/// @param world_min Output receiving the union minimum corner.
/// @param world_max Output receiving the union maximum corner.
/// @param out_radius Optional output receiving the largest base mesh radius.
/// @return Nonzero when any drawable variant contributes valid bounds.
int scene3d_node_world_draw_union_aabb(rt_scene_node3d *node,
                                       void *effective_animator,
                                       double world_min[3],
                                       double world_max[3],
                                       double *out_radius) {
    int has_bounds = 0;
    double radius = 0.0;
    if (!node || !world_min || !world_max)
        return 0;
    scene_bounds_reset_d(world_min, world_max);
    if (node->mesh && scene3d_include_mesh_world_bounds(
                          node, node->mesh, effective_animator, world_min, world_max, &radius))
        has_bounds = 1;
    for (int32_t i = 0, lod_count = scene3d_node_lod_count(node); i < lod_count; ++i) {
        if (node->lod_levels[i].mesh &&
            scene3d_include_mesh_world_bounds(
                node, node->lod_levels[i].mesh, effective_animator, world_min, world_max, &radius))
            has_bounds = 1;
    }
    if (node->has_impostor && node->impostor_mesh &&
        scene3d_include_mesh_world_bounds(
            node, node->impostor_mesh, effective_animator, world_min, world_max, &radius))
        has_bounds = 1;
    if (out_radius)
        *out_radius = radius;
    return has_bounds;
}

/// @brief Resolve the nearest inherited skeletal animator for a node.
/// @param node Borrowed node at which to begin the ancestor search.
/// @return Borrowed AnimController3D handle, or `NULL`.
void *scene3d_effective_animator(rt_scene_node3d *node);

/// @brief Rebuild a scene's spatial entries and BVH topology from its node hierarchy.
/// @param scene Borrowed scene whose index is rebuilt.
/// @return Nonzero when a usable index was constructed, otherwise zero.
static int scene3d_spatial_rebuild(rt_scene3d *scene);

/// @brief Effective visibility of @p node: itself AND every ancestor visible.
/// @param node Borrowed node whose ancestor chain is inspected.
/// @return Nonzero when the node and every ancestor are visible.
static int scene3d_node_effective_visible(rt_scene_node3d *node) {
    rt_scene_node3d *current = node;
    int32_t steps = 0;
    if (!scene3d_spatial_parent_chain_valid(node))
        return 0;
    while (current && steps++ < SCENE3D_SPATIAL_ANCESTOR_MAX) {
        if (!current->visible)
            return 0;
        current = current->parent;
    }
    return 1;
}

/// @brief Recompute a spatial entry's world AABB and visibility from its node's
///   current transform/geometry. Hidden nodes stay indexed (visibility is a query
///   filter, not topology) so their bounds refresh like any other entry.
/// @param entry Borrowed mutable spatial entry linked to a drawable node.
/// @return 1 on success, 0 when the node no longer has drawable bounds (caller
///   escalates to a rebuild).
static int scene3d_spatial_refresh_entry_bounds(rt_scene3d_spatial_entry *entry) {
    double world_min[3];
    double world_max[3];
    double radius = 0.0;
    if (!entry || !rt_g3d_has_class(entry->node, RT_G3D_SCENENODE3D_CLASS_ID) ||
        !scene3d_spatial_parent_chain_valid(entry->node))
        return 0;
    recompute_world_matrix(entry->node);
    if (!scene3d_node_world_draw_union_aabb(
            entry->node, scene3d_effective_animator(entry->node), world_min, world_max, &radius))
        return 0;
    memcpy(entry->world_min, world_min, sizeof(entry->world_min));
    memcpy(entry->world_max, world_max, sizeof(entry->world_max));
    entry->cullable = radius > 0.0 ? 1 : 0;
    entry->visible = scene3d_node_effective_visible(entry->node) ? 1 : 0;
    entry->world_revision = entry->node->world_revision;
    entry->geometry_revision = scene_node_geometry_revision_signature(entry->node);
    return 1;
}

/// @brief True if @p node or any ancestor has a dirty world transform (its cached world
///   bounds cannot be trusted until the transforms are refreshed).
/// @param node Borrowed node whose ancestor chain is inspected.
/// @return Nonzero when any cached world transform on the chain is dirty.
static int scene3d_spatial_node_or_ancestor_dirty(rt_scene_node3d *node) {
    rt_scene_node3d *current = node;
    int32_t steps = 0;
    if (!scene3d_spatial_parent_chain_valid(node))
        return 1;
    while (current && steps++ < SCENE3D_SPATIAL_ANCESTOR_MAX) {
        if (current->world_dirty)
            return 1;
        current = current->parent;
    }
    return 0;
}

/// @brief Recompute a BVH node's bounds bottom-up from its children/leaf entries (refit, no
/// resplit).
/// @param index Borrowed mutable spatial index.
/// @param node_index Zero-based BVH subtree root to refit recursively.
/// @param budget Remaining node visits; prevents corrupt child cycles.
/// @return Nonzero when the complete subtree was structurally valid and refit.
static int scene3d_spatial_refit_bvh_node(rt_scene3d_spatial_index *index,
                                          int32_t node_index,
                                          int32_t *budget) {
    rt_scene3d_spatial_bvh_node *node;
    if (!index || !budget || *budget <= 0 || node_index < 0 || node_index >= index->node_count)
        return 0;
    (*budget)--;
    node = &index->nodes[node_index];
    scene_bounds_reset_d(node->world_min, node->world_max);
    node->cullable_count = 0;
    if (node->leaf) {
        if (node->start < 0 || node->count <= 0 || node->start > index->count - node->count ||
            node->start > index->entry_index_capacity - node->count)
            return 0;
        for (int32_t i = node->start; i < node->start + node->count; ++i) {
            int32_t entry_index = index->entry_indices[i];
            rt_scene3d_spatial_entry *entry;
            if (entry_index < 0 || entry_index >= index->count)
                return 0;
            entry = &index->entries[entry_index];
            if (!scene3d_spatial_bounds_valid(entry->world_min, entry->world_max))
                return 0;
            scene3d_spatial_bounds_include(
                node->world_min, node->world_max, entry->world_min, entry->world_max);
            if (entry->cullable)
                node->cullable_count++;
        }
        return 1;
    }
    if (node->left < 0 || node->left >= index->node_count || node->right < 0 ||
        node->right >= index->node_count || node->left == node_index || node->right == node_index ||
        node->left == node->right || !scene3d_spatial_refit_bvh_node(index, node->left, budget) ||
        !scene3d_spatial_refit_bvh_node(index, node->right, budget))
        return 0;
    if (node->left >= 0 && node->left < index->node_count) {
        rt_scene3d_spatial_bvh_node *left = &index->nodes[node->left];
        scene3d_spatial_bounds_include(
            node->world_min, node->world_max, left->world_min, left->world_max);
        node->cullable_count += left->cullable_count;
    }
    if (node->right >= 0 && node->right < index->node_count) {
        rt_scene3d_spatial_bvh_node *right = &index->nodes[node->right];
        scene3d_spatial_bounds_include(
            node->world_min, node->world_max, right->world_min, right->world_max);
        node->cullable_count += right->cullable_count;
    }
    return scene3d_spatial_bounds_valid(node->world_min, node->world_max);
}

/// @brief Recompute one BVH node's bounds/cullable count from its immediate
///   children (or its leaf entries) without recursing.
/// @param index Borrowed mutable spatial index.
/// @param node_index Zero-based BVH node to recompute.
static int scene3d_spatial_recompute_node(rt_scene3d_spatial_index *index, int32_t node_index) {
    rt_scene3d_spatial_bvh_node *node;
    if (!index || node_index < 0 || node_index >= index->node_count)
        return 0;
    node = &index->nodes[node_index];
    scene_bounds_reset_d(node->world_min, node->world_max);
    node->cullable_count = 0;
    if (node->leaf) {
        if (node->start < 0 || node->count <= 0 || node->start > index->count - node->count ||
            node->start > index->entry_index_capacity - node->count)
            return 0;
        for (int32_t i = node->start; i < node->start + node->count; ++i) {
            int32_t entry_index = index->entry_indices[i];
            rt_scene3d_spatial_entry *entry;
            if (entry_index < 0 || entry_index >= index->count)
                return 0;
            entry = &index->entries[entry_index];
            if (!scene3d_spatial_bounds_valid(entry->world_min, entry->world_max))
                return 0;
            scene3d_spatial_bounds_include(
                node->world_min, node->world_max, entry->world_min, entry->world_max);
            if (entry->cullable)
                node->cullable_count++;
        }
        return 1;
    }
    if (node->left >= 0 && node->left < index->node_count && node->left != node_index) {
        rt_scene3d_spatial_bvh_node *left = &index->nodes[node->left];
        scene3d_spatial_bounds_include(
            node->world_min, node->world_max, left->world_min, left->world_max);
        node->cullable_count += left->cullable_count;
    } else {
        return 0;
    }
    if (node->right >= 0 && node->right < index->node_count && node->right != node_index &&
        node->right != node->left) {
        rt_scene3d_spatial_bvh_node *right = &index->nodes[node->right];
        scene3d_spatial_bounds_include(
            node->world_min, node->world_max, right->world_min, right->world_max);
        node->cullable_count += right->cullable_count;
    } else {
        return 0;
    }
    return scene3d_spatial_bounds_valid(node->world_min, node->world_max);
}

/// @brief Re-union only the leaf-to-root path containing @p leaf_node.
/// @details Sibling subtrees kept their stored bounds, so recomputing each node
///   on the path from its two children is exact. When several changed leaves
///   share ancestors, later walks simply recompute those ancestors again with
///   both children current — idempotent, order-independent.
/// @param index Borrowed mutable spatial index.
/// @param leaf_node Zero-based changed leaf-node index.
static int scene3d_spatial_refit_path(rt_scene3d_spatial_index *index, int32_t leaf_node) {
    int32_t current = leaf_node;
    int32_t hops = 0;
    if (!index || !scene3d_spatial_storage_valid(index))
        return 0;
    while (current >= 0 && current < index->node_count && hops++ < index->node_count) {
        if (!scene3d_spatial_recompute_node(index, current))
            return 0;
        current = index->nodes[current].parent;
    }
    return current == -1;
}

/// @brief Push one node onto retained dirty-subtree traversal scratch.
static int scene3d_spatial_dirty_walk_push(rt_scene3d_spatial_index *index,
                                           int32_t *count,
                                           rt_scene_node3d *node) {
    int32_t capacity;
    rt_scene_node3d **grown;
    if (!index || !count || !node || *count < 0 || *count >= SCENE3D_SPATIAL_ANCESTOR_MAX)
        return 0;
    if (*count >= index->dirty_walk_stack_capacity) {
        capacity = index->dirty_walk_stack_capacity > 0 ? index->dirty_walk_stack_capacity : 64;
        while (capacity <= *count) {
            if (capacity > SCENE3D_SPATIAL_ANCESTOR_MAX / 2) {
                capacity = *count + 1;
                break;
            }
            capacity *= 2;
        }
        if (capacity <= *count || capacity > SCENE3D_SPATIAL_ANCESTOR_MAX ||
            (size_t)capacity > SIZE_MAX / sizeof(*grown))
            return 0;
        grown =
            (rt_scene_node3d **)realloc(index->dirty_walk_stack, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return 0;
        index->dirty_walk_stack = grown;
        index->dirty_walk_stack_capacity = capacity;
    }
    index->dirty_walk_stack[(*count)++] = node;
    return 1;
}

/// @brief Refresh one mapped spatial entry and schedule its leaf-to-root refit.
static int scene3d_spatial_refresh_mapped_entry(rt_scene3d_spatial_index *index,
                                                rt_scene_node3d *node,
                                                int *refresh_attempts,
                                                int *path_refit_ok) {
    int32_t entry_index;
    rt_scene3d_spatial_entry *entry;
    if (!index || !node || !refresh_attempts || !path_refit_ok)
        return 0;
    entry_index = node->spatial_entry_index;
    if (entry_index < 0)
        return 1;
    if (!scene3d_spatial_entry_valid(index, entry_index) ||
        index->entries[entry_index].node != node)
        return 0;
    entry = &index->entries[entry_index];
    if (!scene3d_spatial_refresh_entry_bounds(entry))
        return 0;
    (*refresh_attempts)++;
    if (entry->leaf_node >= 0 && entry->leaf_node < index->node_count) {
        if (*refresh_attempts < (index->count + 7) / 8 &&
            !scene3d_spatial_refit_path(index, entry->leaf_node))
            *path_refit_ok = 0;
    } else {
        *path_refit_ok = 0;
    }
    return 1;
}

/// @brief Refit the BVH to current geometry without changing its topology.
/// @details Refreshes only entries whose transform/geometry/visibility revisions
///   moved, then re-unions just the changed leaf-to-root paths (falling back to
///   one full bottom-up pass when most of the scene moved). A pass where nothing
///   changed does NO tree work at all — previously a single animated mesh
///   anywhere re-unioned the whole tree every query. Returns 0 (signalling a
///   rebuild is needed) if the tree shape no longer fits.
/// @param scene Borrowed scene whose existing topology is refreshed.
/// @return Nonzero when the existing/refreshed or rebuilt index is usable.
static int scene3d_spatial_refit(rt_scene3d *scene) {
    rt_scene3d_spatial_index *index;
    int refresh_attempts = 0;
    int path_refit_ok = 1;

    enum { SCENE3D_SPATIAL_MAX_REFITS_BEFORE_REBUILD = 32 };

    if (!scene || !scene->root)
        return 0;
    index = &scene->spatial_index;
    if (!scene3d_spatial_storage_valid(index))
        return scene3d_spatial_rebuild(scene);
    if (!index->valid || index->topology_dirty)
        return scene3d_spatial_rebuild(scene);
    if (index->dirty_all || index->dirty_node_count <= 0 ||
        index->mesh_geometry_epoch != rt_mesh3d_global_geometry_epoch()) {
        /* Shared mesh mutations cannot identify their consuming scene nodes cheaply, and dirty
         * queue allocation failure deliberately lands here. Preserve the complete safe scan. */
        for (int32_t i = 0; i < index->count; ++i) {
            rt_scene3d_spatial_entry *entry = &index->entries[i];
            uint32_t geometry_now;
            int8_t visible_now;
            if (!rt_g3d_has_class(entry->node, RT_G3D_SCENENODE3D_CLASS_ID))
                return scene3d_spatial_rebuild(scene);
            geometry_now = scene_node_geometry_revision_signature(entry->node);
            visible_now = scene3d_node_effective_visible(entry->node) ? 1 : 0;
            if (!scene3d_spatial_node_or_ancestor_dirty(entry->node) &&
                entry->world_revision == entry->node->world_revision &&
                entry->geometry_revision == geometry_now && entry->visible == visible_now)
                continue;
            if (!scene3d_spatial_refresh_mapped_entry(
                    index, entry->node, &refresh_attempts, &path_refit_ok))
                return scene3d_spatial_rebuild(scene);
        }
    } else {
        int32_t traversal_budget = SCENE3D_SPATIAL_ANCESTOR_MAX;
        for (int32_t root_index = 0; root_index < index->dirty_node_count; root_index++) {
            int32_t walk_count = 0;
            if (!scene3d_spatial_dirty_walk_push(
                    index, &walk_count, index->dirty_nodes[root_index]))
                return scene3d_spatial_rebuild(scene);
            while (walk_count > 0) {
                rt_scene_node3d *node = index->dirty_walk_stack[--walk_count];
                if (traversal_budget-- <= 0 ||
                    !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID) ||
                    node->owner_scene != scene)
                    return scene3d_spatial_rebuild(scene);
                if (!scene3d_spatial_refresh_mapped_entry(
                        index, node, &refresh_attempts, &path_refit_ok))
                    return scene3d_spatial_rebuild(scene);
                for (int32_t child = 0; child < scene3d_node_child_count(node); child++) {
                    if (!scene3d_spatial_dirty_walk_push(index, &walk_count, node->children[child]))
                        return scene3d_spatial_rebuild(scene);
                }
            }
        }
    }
    if (refresh_attempts > 0 && (!path_refit_ok || refresh_attempts >= (index->count + 7) / 8)) {
        /* Broad motion: one full bottom-up pass beats many overlapping walks. */
        int32_t budget = index->node_count;
        if (index->root_node < 0 ||
            !scene3d_spatial_refit_bvh_node(index, index->root_node, &budget))
            return scene3d_spatial_rebuild(scene);
    }
    index->dirty = 0;
    index->valid = 1;
    index->topology_dirty = 0;
    index->mesh_geometry_epoch = rt_mesh3d_global_geometry_epoch();
    index->dirty_node_count = 0;
    index->dirty_all = 0;
    if (refresh_attempts)
        index->refit_count++;
    if (refresh_attempts && index->refit_count >= SCENE3D_SPATIAL_MAX_REFITS_BEFORE_REBUILD)
        return scene3d_spatial_rebuild(scene);
    index->last_candidate_count = 0;
    index->last_prefiltered_count = 0;
    return 1;
}

/// @brief Resolve the animator governing a node, inheriting the nearest ancestor's bound animator.
/// @param node Borrowed node at which to begin the ancestor search.
/// @return Borrowed validated AnimController3D handle, or `NULL`.
void *scene3d_effective_animator(rt_scene_node3d *node) {
    rt_scene_node3d *current = node;
    int32_t steps = 0;
    if (!scene3d_spatial_parent_chain_valid(node))
        return NULL;
    while (current && steps++ < SCENE3D_SPATIAL_ANCESTOR_MAX) {
        void *animator =
            rt_g3d_checked_or_null(current->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
        if (animator)
            return animator;
        current = current->parent;
    }
    return NULL;
}

/// @brief Add a drawable node to the spatial index as a leaf entry with its world bounds.
/// @param index Borrowed index receiving the entry.
/// @param node Borrowed drawable node.
/// @param traversal_order Stable scene traversal ordinal.
/// @param world_min Borrowed world-space minimum corner.
/// @param world_max Borrowed world-space maximum corner.
/// @param radius Positive radius marks the entry cullable.
/// @param visible Nonzero when the node and every ancestor are visible.
/// @return Nonzero after append or a safe invalid-input no-op, otherwise zero.
static int scene3d_spatial_add_entry(rt_scene3d_spatial_index *index,
                                     rt_scene_node3d *node,
                                     int32_t traversal_order,
                                     const double world_min[3],
                                     const double world_max[3],
                                     double radius,
                                     int visible) {
    rt_scene3d_spatial_entry *entry;
    if (!index || !node || !world_min || !world_max)
        return 1;
    if (index->count < 0 || index->count >= SCENE3D_SPATIAL_ENTRY_MAX ||
        !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID) ||
        !scene3d_spatial_bounds_valid(world_min, world_max))
        return 0;
    if (!scene3d_spatial_ensure_capacity(index, index->count + 1))
        return 0;
    entry = &index->entries[index->count++];
    entry->node = node;
    memcpy(entry->world_min, world_min, sizeof(entry->world_min));
    memcpy(entry->world_max, world_max, sizeof(entry->world_max));
    entry->traversal_order = traversal_order;
    entry->cullable = radius > 0.0 ? 1 : 0;
    entry->visible = visible ? 1 : 0;
    entry->leaf_node = -1;
    entry->world_revision = node->world_revision;
    entry->geometry_revision = scene_node_geometry_revision_signature(node);
    node->spatial_entry_index = index->count - 1;
    return 1;
}

/// @brief Rebuild the scene's spatial BVH from scratch by traversing the node hierarchy.
/// @details Collects every drawable node as a leaf entry (tracking its effective animator), then
///          builds the BVH over them. Called when the topology dirty flag is set.
/// @param scene Borrowed scene whose entries and BVH topology are rebuilt.
/// @return 1 on success, 0 on allocation failure.
static int scene3d_spatial_rebuild(rt_scene3d *scene) {
    scene_index_build_stack_item_t *stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int32_t traversal_order = 0;
    int32_t traversal_budget = SCENE3D_SPATIAL_ENTRY_MAX;
    rt_scene3d_spatial_index *index;
    if (!scene || !scene->root)
        return 0;
    index = &scene->spatial_index;
    scene3d_spatial_prepare_rebuild(index);
    if (!rt_g3d_has_class(scene->root, RT_G3D_SCENENODE3D_CLASS_ID)) {
        index->dirty = 0;
        index->topology_dirty = 0;
        index->valid = 1;
        index->mesh_geometry_epoch = rt_mesh3d_global_geometry_epoch();
        return 1;
    }
    if (!scene_index_build_stack_push(&stack, &count, &capacity, scene->root, NULL)) {
        rt_trap("Scene3D.SpatialIndex: traversal stack allocation failed");
        return 0;
    }
    while (count > 0) {
        scene_index_build_stack_item_t item = stack[--count];
        rt_scene_node3d *current = item.node;
        void *effective_animator;
        double world_min[3];
        double world_max[3];
        double radius = 0.0;
        int32_t order;

        if (traversal_budget-- <= 0) {
            rt_trap("Scene3D.SpatialIndex: traversal budget exceeded");
            free(stack);
            return 0;
        }
        if (!rt_g3d_has_class(current, RT_G3D_SCENENODE3D_CLASS_ID) ||
            !scene3d_spatial_parent_chain_valid(current))
            continue;
        if (traversal_order >= SCENE3D_SPATIAL_ENTRY_MAX) {
            rt_trap("Scene3D.SpatialIndex: traversal order overflow");
            free(stack);
            return 0;
        }
        order = traversal_order++;

        /* Hidden subtrees are indexed too (with visible=0) so a visibility
         * toggle is a per-entry refit, never a topology rebuild. */
        recompute_world_matrix(current);
        effective_animator =
            rt_g3d_checked_or_null(current->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
        if (!effective_animator)
            effective_animator = item.inherited_animator;
        if (scene3d_node_world_draw_union_aabb(
                current, effective_animator, world_min, world_max, &radius)) {
            if (!scene3d_spatial_add_entry(index,
                                           current,
                                           order,
                                           world_min,
                                           world_max,
                                           radius,
                                           scene3d_node_effective_visible(current))) {
                rt_trap("Scene3D.SpatialIndex: entry allocation failed");
                free(stack);
                return 0;
            }
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            if (!rt_g3d_has_class(current->children[i], RT_G3D_SCENENODE3D_CLASS_ID))
                continue;
            if (!scene_index_build_stack_push(
                    &stack, &count, &capacity, current->children[i], effective_animator)) {
                rt_trap("Scene3D.SpatialIndex: traversal stack allocation failed");
                free(stack);
                return 0;
            }
        }
    }
    free(stack);
    if (!scene3d_spatial_ensure_entry_index_capacity(index, index->count)) {
        rt_trap("Scene3D.SpatialIndex: BVH index allocation failed");
        return 0;
    }
    for (int32_t i = 0; i < index->count; ++i)
        index->entry_indices[i] = i;
    if (index->count > 0) {
        index->root_node = scene3d_spatial_build_bvh_range(index, 0, index->count);
        if (index->root_node < 0) {
            rt_trap("Scene3D.SpatialIndex: BVH node allocation failed");
            return 0;
        }
    }
    index->dirty = 0;
    index->topology_dirty = 0;
    index->valid = 1;
    index->build_count++;
    index->refit_count = 0;
    index->mesh_geometry_epoch = rt_mesh3d_global_geometry_epoch();
    index->dirty_node_count = 0;
    index->dirty_all = 0;
    return 1;
}

/// @brief Ensure the spatial index is current before a query: rebuild on topology change, else
/// refit.
/// @param scene Borrowed scene whose index is requested.
/// @return 1 if a usable index is available, 0 if it could not be built.
static int scene3d_spatial_ensure(rt_scene3d *scene) {
    if (!scene || !scene->use_spatial_index)
        return 0;
    if (scene->spatial_index.valid && !scene3d_spatial_storage_valid(&scene->spatial_index))
        scene3d_spatial_invalidate(&scene->spatial_index);
    if (scene->spatial_index.valid && !scene->spatial_index.dirty &&
        scene->spatial_index.mesh_geometry_epoch == rt_mesh3d_global_geometry_epoch())
        return 1;
    if (scene->spatial_index.valid && !scene->spatial_index.dirty)
        scene->spatial_index.dirty = 1;
    if (scene->spatial_index.valid && scene->spatial_index.dirty &&
        !scene->spatial_index.topology_dirty)
        return scene3d_spatial_refit(scene);
    return scene3d_spatial_rebuild(scene);
}

/// @brief Collect spatial entries whose world bounds overlap a query AABB via BVH traversal.
/// @details Descends only into nodes whose bounds intersect the query, so a large scene costs
///          O(log n + hits) rather than scanning every node.
/// @param scene Borrowed scene whose refreshed index is queried.
/// @param query_min Borrowed query minimum corner.
/// @param query_max Borrowed query maximum corner.
/// @param out Caller-owned candidate list to append to.
/// @param count_cullable_prefilter Nonzero to count only cullable broad-phase rejections.
/// @return Nonzero after successful stable-order collection, otherwise zero.
int scene3d_spatial_collect_aabb(rt_scene3d *scene,
                                 const double query_min[3],
                                 const double query_max[3],
                                 scene3d_spatial_candidate_list_t *out,
                                 int count_cullable_prefilter) {
    rt_scene3d_spatial_index *index;
    scene3d_spatial_node_stack_t stack = {0};
    int32_t initial_count;
    int32_t prefiltered = 0;
    int32_t traversal_budget;
    if (!scene || !query_min || !query_max || !out)
        return 0;
    if (!scene3d_spatial_bounds_valid(query_min, query_max))
        return 0;
    if (!scene3d_spatial_ensure(scene))
        return 0;
    index = &scene->spatial_index;
    if (!scene3d_spatial_storage_valid(index)) {
        scene3d_spatial_invalidate(index);
        return 0;
    }
    if (!out->items || out->capacity < 0 || out->capacity > SCENE3D_SPATIAL_ENTRY_MAX) {
        free(out->items);
        out->items = NULL;
        out->count = 0;
        out->capacity = 0;
    } else if (out->count < 0 || out->count > out->capacity) {
        out->count = 0;
    }
    initial_count = out->count;
    stack.items = index->query_stack;
    stack.capacity = index->query_stack_capacity;
    traversal_budget = index->node_count;
    if (index->root_node >= 0 && !scene3d_spatial_node_stack_push(&stack, index->root_node)) {
        rt_trap("Scene3D.SpatialIndex: BVH traversal stack allocation failed");
        scene3d_spatial_store_query_stack(index, &stack);
        return 0;
    }
    while (stack.count > 0) {
        rt_scene3d_spatial_bvh_node *node;
        int32_t node_index = stack.items[--stack.count];
        if (traversal_budget-- <= 0 || node_index < 0 || node_index >= index->node_count) {
            scene3d_spatial_invalidate(index);
            scene3d_spatial_store_query_stack(index, &stack);
            return 0;
        }
        node = &index->nodes[node_index];
        if ((node->leaf != 0 && node->leaf != 1) ||
            !scene3d_spatial_bounds_valid(node->world_min, node->world_max)) {
            scene3d_spatial_invalidate(index);
            scene3d_spatial_store_query_stack(index, &stack);
            return 0;
        }
        if (!scene3d_aabb_intersects_aabb(node->world_min, node->world_max, query_min, query_max)) {
            int32_t rejected = count_cullable_prefilter ? node->cullable_count : node->count;
            scene3d_spatial_saturating_add(&prefiltered, rejected);
            continue;
        }
        if (node->leaf) {
            if (node->start < 0 || node->count <= 0 || node->start > index->count - node->count ||
                node->start > index->entry_index_capacity - node->count) {
                scene3d_spatial_invalidate(index);
                scene3d_spatial_store_query_stack(index, &stack);
                return 0;
            }
            for (int32_t i = node->start; i < node->start + node->count; ++i) {
                int32_t entry_index = index->entry_indices[i];
                rt_scene3d_spatial_entry *entry;
                if (!scene3d_spatial_entry_valid(index, entry_index)) {
                    scene3d_spatial_invalidate(index);
                    scene3d_spatial_store_query_stack(index, &stack);
                    return 0;
                }
                entry = &index->entries[entry_index];
                if (!entry->visible)
                    continue;
                if (!scene3d_aabb_intersects_aabb(
                        entry->world_min, entry->world_max, query_min, query_max)) {
                    if (!count_cullable_prefilter || entry->cullable)
                        scene3d_spatial_saturating_add(&prefiltered, 1);
                    continue;
                }
                if (!scene3d_spatial_candidate_push(out, entry)) {
                    rt_trap("Scene3D.SpatialIndex: candidate allocation failed");
                    scene3d_spatial_store_query_stack(index, &stack);
                    return 0;
                }
            }
        } else {
            if (node->left < 0 || node->left >= index->node_count || node->right < 0 ||
                node->right >= index->node_count || node->left == node_index ||
                node->right == node_index || node->left == node->right ||
                index->nodes[node->left].parent != node_index ||
                index->nodes[node->right].parent != node_index) {
                scene3d_spatial_invalidate(index);
                scene3d_spatial_store_query_stack(index, &stack);
                return 0;
            }
            if (!scene3d_spatial_node_stack_push(&stack, node->right) ||
                !scene3d_spatial_node_stack_push(&stack, node->left)) {
                rt_trap("Scene3D.SpatialIndex: BVH traversal stack allocation failed");
                scene3d_spatial_store_query_stack(index, &stack);
                return 0;
            }
        }
    }
    scene3d_spatial_store_query_stack(index, &stack);
    if (!scene3d_spatial_order_candidates(index, out)) {
        rt_trap("Scene3D.SpatialIndex: candidate ordering allocation failed");
        return 0;
    }
    index->last_candidate_count = out->count - initial_count;
    index->last_prefiltered_count = prefiltered;
    return 1;
}

/// @brief Collect every spatial entry (no spatial filtering) into the candidate list.
/// @param scene Borrowed scene whose refreshed index is enumerated.
/// @param out Caller-owned candidate list to append visible entries to.
/// @return Nonzero after successful stable-order collection, otherwise zero.
int scene3d_spatial_collect_all(rt_scene3d *scene, scene3d_spatial_candidate_list_t *out) {
    rt_scene3d_spatial_index *index;
    int32_t initial_count;
    if (!scene || !out)
        return 0;
    if (!out->items || out->capacity < 0 || out->capacity > SCENE3D_SPATIAL_ENTRY_MAX) {
        free(out->items);
        out->items = NULL;
        out->count = 0;
        out->capacity = 0;
    } else if (out->count < 0 || out->count > out->capacity) {
        out->count = 0;
    }
    initial_count = out->count;
    if (!scene3d_spatial_ensure(scene))
        return 0;
    index = &scene->spatial_index;
    if (!scene3d_spatial_storage_valid(index)) {
        scene3d_spatial_invalidate(index);
        return 0;
    }
    for (int32_t i = 0; i < index->count; ++i) {
        if (!scene3d_spatial_entry_valid(index, i)) {
            scene3d_spatial_invalidate(index);
            return 0;
        }
        if (!index->entries[i].visible)
            continue;
        if (!scene3d_spatial_candidate_push(out, &index->entries[i])) {
            rt_trap("Scene3D.SpatialIndex: candidate allocation failed");
            return 0;
        }
    }
    /* Entries were appended in traversal order during rebuild, so an empty
     * caller list is already stable and needs no O(n log n) sort. Preserve
     * the append contract for nonempty internal callers by sorting the merged
     * result as before. */
    if (initial_count > 0 && !scene3d_spatial_order_candidates(index, out)) {
        rt_trap("Scene3D.SpatialIndex: candidate ordering allocation failed");
        return 0;
    }
    index->last_candidate_count = out->count - initial_count;
    index->last_prefiltered_count = 0;
    return 1;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
