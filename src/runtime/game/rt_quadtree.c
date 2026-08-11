//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_quadtree.c
/// @file
/// @brief Implements a growable spatial quadtree, transient queries, and
///        immutable query/pair snapshots.
//
// Purpose: Spatial quadtree for Zanna games. Accelerates nearest-neighbour and
//   rectangular-region queries from O(n) linear scans to approximately
//   O(n log n) by recursively subdividing a 2D world region into four equal
//   quadrants (NW, NE, SW, SE). Typical use cases: enemy radar, player
//   proximity triggers, broad-phase collision detection, and line-of-sight
//   culling over large open worlds.
//
// Key invariants:
//   - Items are identified by unique int64 IDs and stored as (center x, center y,
//     width, height) AABBs. All coordinates are in the same integer unit space
//     as the world bounds given to rt_quadtree_new().
//   - The tree subdivides a node when it would overflow RT_QUADTREE_MAX_ITEMS
//     (8) items, up to a maximum depth of RT_QUADTREE_MAX_DEPTH (8) levels.
//     Items that span a subdivision midpoint are kept in the parent node.
//   - Nodes are heap-allocated individually via malloc(). The GC finalizer
//     (quadtree_finalizer) recursively destroys the entire node tree when the
//     root object is collected. This means the tree is NOT fully GC-transparent:
//     node memory is invisible to the GC and is freed via the finalizer path
//     rather than by GC tracing.
//   - The flat item array is root-owned heap storage. item_count is the
//     high-water mark (never decrements); inactive items are marked with active
//     == 0.
//   - Duplicate-ID insert guard: rt_quadtree_insert() performs a linear scan of
//     the flat array before insertion. Inserting the same ID twice is rejected
//     (returns 0) to prevent ghost items after a single Remove().
//   - Query results are stored in a root-owned growable results array. The
//     legacy RT_QUADTREE_MAX_RESULTS value is only the initial reservation.
//     query_truncated is set only when allocation prevents a complete result.
//   - Pair collection (rt_quadtree_get_pairs) generates candidate collision
//     pairs by walking the tree recursively and testing items in each node
//     against each other AND against all ancestor items.
//
// Ownership/Lifetime:
//   - The root is runtime reference-counted. rt_quadtree_destroy() releases one
//     reference; the finalizer recursively frees nodes and flat buffers.
//
// Links: src/runtime/game/rt_quadtree.h (public API),
//        docs/zannalib/game.md (Quadtree section, truncation notes)
//
//===----------------------------------------------------------------------===//

#include "rt_quadtree.h"
#include "rt_box.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// @brief Initial flat-item reservation; storage grows on demand.
#define QT_DEFAULT_ITEM_CAPACITY 256

/// @brief Initial collision-pair reservation; storage grows on demand.
#define MAX_PAIRS 1024

/// @brief One flat item record referenced by node-local index arrays.
struct qt_item {
    int64_t id;
    int64_t x, y;          ///< Center position.
    int64_t width, height; ///< Dimensions.
    int8_t active;
};

/// @brief One recursively allocated spatial subdivision node.
struct qt_node {
    int64_t x, y;          ///< Bounds top-left.
    int64_t width, height; ///< Bounds dimensions.
    int64_t *items;        ///< Item indices in this node.
    int64_t item_count;
    int64_t item_capacity;
    int64_t depth;
    struct qt_node *children[4]; ///< NW, NE, SW, SE.
    int8_t is_split;
};

/// @brief One unordered broad-phase candidate pair of item IDs.
struct qt_pair {
    int64_t first;
    int64_t second;
};

/// @brief Root object owning the node hierarchy and growable flat buffers.
struct rt_quadtree_impl {
    struct qt_node *root;
    struct qt_item *items;
    int64_t item_count;
    int64_t item_capacity;
    int64_t *results;
    int64_t result_count;
    int64_t result_capacity;
    int8_t query_truncated; ///< 1 if allocation prevented complete query results.
    struct qt_pair *pairs;
    int64_t pair_count;
    int64_t pair_capacity;
    int8_t pairs_truncated; ///< 1 if allocation prevented complete pair results.
};

/// @brief Immutable, independently allocated snapshot of query IDs.
struct rt_game_query_result_impl {
    int64_t *ids;
    int64_t count;
    int8_t truncated;
};

/// @brief Immutable, independently allocated snapshot of candidate pairs.
struct rt_quadtree_pair_result_impl {
    struct qt_pair *pairs;
    int64_t count;
    int8_t truncated;
};

/// @brief Validate and cast an opaque Quadtree handle.
/// @param tree Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return @p tree when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static rt_quadtree checked_quadtree(rt_quadtree tree, const char *api) {
    if (!tree)
        return NULL;
    if (rt_obj_class_id(tree) != RT_QUADTREE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return tree;
}

/// @brief Validate and cast an opaque QueryResult handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return QueryResult implementation when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap.
static struct rt_game_query_result_impl *checked_query_result(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_GAME_QUERY_RESULT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (struct rt_game_query_result_impl *)ptr;
}

/// @brief Validate and cast an opaque QuadtreePairResult handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return Pair-result implementation when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap.
static struct rt_quadtree_pair_result_impl *checked_pair_result(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_QUADTREE_PAIR_RESULT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (struct rt_quadtree_pair_result_impl *)ptr;
}

/// @brief Release a QueryResult's copied ID array during finalization.
/// @param obj QueryResult supplied by the runtime finalizer system.
static void query_result_finalizer(void *obj) {
    struct rt_game_query_result_impl *result = (struct rt_game_query_result_impl *)obj;
    free(result->ids);
    result->ids = NULL;
    result->count = 0;
}

/// @brief Release a pair result's copied pair array during finalization.
/// @param obj QuadtreePairResult supplied by the runtime finalizer system.
static void pair_result_finalizer(void *obj) {
    struct rt_quadtree_pair_result_impl *result = (struct rt_quadtree_pair_result_impl *)obj;
    free(result->pairs);
    result->pairs = NULL;
    result->count = 0;
}

/// @brief Copy transient query IDs into an immutable result object.
/// @param ids Source array; may be `NULL`.
/// @param count Requested source count.
/// @param truncated Whether the originating query was already partial.
/// @return New QueryResult, or `NULL` if object allocation fails.
/// @details Array size overflow or copy allocation failure returns a valid
///          empty result marked truncated.
static void *query_result_from_ids(const int64_t *ids, int64_t count, int8_t truncated) {
    struct rt_game_query_result_impl *result = (struct rt_game_query_result_impl *)rt_obj_new_i64(
        RT_GAME_QUERY_RESULT_CLASS_ID, (int64_t)sizeof(struct rt_game_query_result_impl));
    if (!result)
        return NULL;
    result->ids = NULL;
    result->count = 0;
    result->truncated = truncated ? 1 : 0;
    rt_obj_set_finalizer(result, query_result_finalizer);

    if (!ids || count <= 0)
        return result;
    if ((uint64_t)count > SIZE_MAX / sizeof(int64_t)) {
        result->truncated = 1;
        return result;
    }
    result->ids = (int64_t *)malloc((size_t)count * sizeof(int64_t));
    if (!result->ids) {
        result->truncated = 1;
        return result;
    }
    memcpy(result->ids, ids, (size_t)count * sizeof(int64_t));
    result->count = count;
    return result;
}

/// @brief Copy transient candidate pairs into an immutable result object.
/// @param pairs Source array; may be `NULL`.
/// @param count Requested source count.
/// @param truncated Whether the originating collection was already partial.
/// @return New QuadtreePairResult, or `NULL` if object allocation fails.
/// @details Array size overflow or copy allocation failure returns a valid
///          empty result marked truncated.
static void *pair_result_from_pairs(const struct qt_pair *pairs, int64_t count, int8_t truncated) {
    struct rt_quadtree_pair_result_impl *result =
        (struct rt_quadtree_pair_result_impl *)rt_obj_new_i64(
            RT_QUADTREE_PAIR_RESULT_CLASS_ID, (int64_t)sizeof(struct rt_quadtree_pair_result_impl));
    if (!result)
        return NULL;
    result->pairs = NULL;
    result->count = 0;
    result->truncated = truncated ? 1 : 0;
    rt_obj_set_finalizer(result, pair_result_finalizer);

    if (!pairs || count <= 0)
        return result;
    if ((uint64_t)count > SIZE_MAX / sizeof(struct qt_pair)) {
        result->truncated = 1;
        return result;
    }
    result->pairs = (struct qt_pair *)malloc((size_t)count * sizeof(struct qt_pair));
    if (!result->pairs) {
        result->truncated = 1;
        return result;
    }
    memcpy(result->pairs, pairs, (size_t)count * sizeof(struct qt_pair));
    result->count = count;
    return result;
}

/// @brief Add two signed integers without overflow.
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum or nearest int64 endpoint.
static int64_t qt_saturating_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Subtract two signed integers without overflow.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return Saturated difference; an INT64_MIN subtrahend maps to INT64_MAX.
static int64_t qt_saturating_sub(int64_t a, int64_t b) {
    if (b == INT64_MIN)
        return INT64_MAX;
    return qt_saturating_add(a, -b);
}

/// @brief Multiply two signed integers without overflow.
/// @param a First factor.
/// @param b Second factor.
/// @return Exact product or nearest int64 endpoint.
static int64_t qt_saturating_mul(int64_t a, int64_t b) {
#if defined(__SIZEOF_INT128__)
    __extension__ __int128 result = (__int128)a * (__int128)b;
    if (result > INT64_MAX)
        return INT64_MAX;
    if (result < INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
#else
    long double result = (long double)a * (long double)b;
    if (result > (long double)INT64_MAX)
        return INT64_MAX;
    if (result < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
#endif
}

/// @brief Allocate an empty node and its initial item-index array.
/// @param x Bounds top-left X.
/// @param y Bounds top-left Y.
/// @param width Bounds width.
/// @param height Bounds height.
/// @param depth Tree depth assigned to the node.
/// @return New node, or `NULL` if either allocation fails.
static struct qt_node *create_node(
    int64_t x, int64_t y, int64_t width, int64_t height, int64_t depth) {
    struct qt_node *node = malloc(sizeof(struct qt_node));
    if (!node)
        return NULL;

    node->x = x;
    node->y = y;
    node->width = width;
    node->height = height;
    node->items = malloc((size_t)RT_QUADTREE_MAX_ITEMS * sizeof(int64_t));
    if (!node->items) {
        free(node);
        return NULL;
    }
    node->item_count = 0;
    node->item_capacity = RT_QUADTREE_MAX_ITEMS;
    node->depth = depth;
    node->is_split = 0;
    node->children[0] = NULL;
    node->children[1] = NULL;
    node->children[2] = NULL;
    node->children[3] = NULL;

    return node;
}

/// @brief Recursively free a node hierarchy.
/// @param node Root of the subtree to destroy; `NULL` is a no-op.
static void destroy_node(struct qt_node *node) {
    if (!node)
        return;

    for (int i = 0; i < 4; i++) {
        if (node->children[i])
            destroy_node(node->children[i]);
    }
    free(node->items);
    free(node);
}

/// @brief Empty a node and discard all of its descendants.
/// @param node Node to retain as an unsplit empty leaf; `NULL` is a no-op.
static void clear_node(struct qt_node *node) {
    if (!node)
        return;

    node->item_count = 0;

    for (int i = 0; i < 4; i++) {
        if (node->children[i]) {
            destroy_node(node->children[i]);
            node->children[i] = NULL;
        }
    }
    node->is_split = 0;
}

/// @brief Grow a quadtree node's item array to hold at least @p needed entries.
/// @param node Node whose index buffer may grow.
/// @param needed Required number of entries.
/// @return 1 on success, 0 on allocation failure.
static int8_t ensure_node_capacity(struct qt_node *node, int64_t needed) {
    if (!node || needed <= node->item_capacity)
        return 1;
    if (needed < 0)
        return 0;

    int64_t new_capacity = node->item_capacity > 0 ? node->item_capacity : RT_QUADTREE_MAX_ITEMS;
    while (new_capacity < needed) {
        if (new_capacity > INT64_MAX / 2) // avoid signed-overflow UB in the doubling
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > SIZE_MAX / sizeof(int64_t)) // realloc size must fit size_t
        return 0;

    int64_t *resized = realloc(node->items, (size_t)new_capacity * sizeof(int64_t));
    if (!resized)
        return 0;

    node->items = resized;
    node->item_capacity = new_capacity;
    return 1;
}

/// @brief Grow a positive int64 capacity by doubling until it reaches @p needed.
/// @param current Existing capacity; nonpositive values start from one.
/// @param needed Required positive capacity.
/// @param elem_size Bytes per element.
/// @return The new capacity, or 0 if the request cannot fit in size_t.
static int64_t grow_capacity_for(int64_t current, int64_t needed, size_t elem_size) {
    if (needed <= 0 || elem_size == 0)
        return 0;
    int64_t new_capacity = current > 0 ? current : 1;
    while (new_capacity < needed) {
        if (new_capacity > INT64_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > SIZE_MAX / elem_size)
        return 0;
    return new_capacity;
}

/// @brief Ensure the flat item array can store at least @p needed slots.
/// @param tree Tree whose item storage may grow.
/// @param needed Required slots.
/// @return `1` when sufficient, otherwise `0`.
/// @details Newly allocated item records are zero-initialized.
static int8_t ensure_item_capacity(struct rt_quadtree_impl *tree, int64_t needed) {
    if (!tree || needed <= tree->item_capacity)
        return 1;
    int64_t new_capacity = grow_capacity_for(tree->item_capacity, needed, sizeof(struct qt_item));
    if (new_capacity <= 0)
        return 0;
    struct qt_item *resized =
        (struct qt_item *)realloc(tree->items, (size_t)new_capacity * sizeof(struct qt_item));
    if (!resized)
        return 0;
    if (new_capacity > tree->item_capacity) {
        memset(resized + tree->item_capacity,
               0,
               (size_t)(new_capacity - tree->item_capacity) * sizeof(struct qt_item));
    }
    tree->items = resized;
    tree->item_capacity = new_capacity;
    return 1;
}

/// @brief Ensure the transient query array can store @p needed IDs.
/// @param tree Tree whose result storage may grow.
/// @param needed Required ID slots.
/// @return `1` when sufficient, otherwise `0`.
static int8_t ensure_result_capacity(struct rt_quadtree_impl *tree, int64_t needed) {
    if (!tree || needed <= tree->result_capacity)
        return 1;
    int64_t new_capacity = grow_capacity_for(tree->result_capacity, needed, sizeof(int64_t));
    if (new_capacity <= 0)
        return 0;
    int64_t *resized = (int64_t *)realloc(tree->results, (size_t)new_capacity * sizeof(int64_t));
    if (!resized)
        return 0;
    tree->results = resized;
    tree->result_capacity = new_capacity;
    return 1;
}

/// @brief Ensure the transient pair array can store @p needed pairs.
/// @param tree Tree whose pair storage may grow.
/// @param needed Required pair slots.
/// @return `1` when sufficient, otherwise `0`.
static int8_t ensure_pair_capacity(struct rt_quadtree_impl *tree, int64_t needed) {
    if (!tree || needed <= tree->pair_capacity)
        return 1;
    int64_t new_capacity = grow_capacity_for(tree->pair_capacity, needed, sizeof(struct qt_pair));
    if (new_capacity <= 0)
        return 0;
    struct qt_pair *resized =
        (struct qt_pair *)realloc(tree->pairs, (size_t)new_capacity * sizeof(struct qt_pair));
    if (!resized)
        return 0;
    tree->pairs = resized;
    tree->pair_capacity = new_capacity;
    return 1;
}

/// @brief Append one ID to the current transient query.
/// @param tree Tree receiving the result.
/// @param id Matching item ID.
/// @return `1` on success, or `0` if storage cannot grow.
/// @details Failure marks the current query truncated.
static int8_t append_query_result(struct rt_quadtree_impl *tree, int64_t id) {
    if (!ensure_result_capacity(tree, tree->result_count + 1)) {
        tree->query_truncated = 1;
        return 0;
    }
    tree->results[tree->result_count++] = id;
    return 1;
}

/// @brief Append one broad-phase candidate pair.
/// @param tree Tree receiving the pair.
/// @param first First item ID.
/// @param second Second item ID.
/// @return `1` on success, or `0` if storage cannot grow.
/// @details Failure marks the current pair collection truncated.
static int8_t append_pair(struct rt_quadtree_impl *tree, int64_t first, int64_t second) {
    if (!ensure_pair_capacity(tree, tree->pair_count + 1)) {
        tree->pairs_truncated = 1;
        return 0;
    }
    tree->pairs[tree->pair_count].first = first;
    tree->pairs[tree->pair_count].second = second;
    tree->pair_count++;
    return 1;
}

/// @brief True if rect (x,y,w,h) overlaps @p node's bounds (saturating AABB
///        test) — used to decide which quadrants a query/insert touches.
/// @param node Node whose bounds are tested.
/// @param x Rectangle top-left X.
/// @param y Rectangle top-left Y.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @return `1` for positive-area overlap, otherwise `0`; touching edges do not
///         overlap.
static int8_t intersects(struct qt_node *node, int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t node_right = qt_saturating_add(node->x, node->width);
    int64_t node_bottom = qt_saturating_add(node->y, node->height);
    int64_t right = qt_saturating_add(x, w);
    int64_t bottom = qt_saturating_add(y, h);
    return !(x >= node_right || right <= node->x || y >= node_bottom || bottom <= node->y);
}

/// @brief Test two top-left AABBs for positive-area overlap.
/// @param ax First rectangle X.
/// @param ay First rectangle Y.
/// @param aw First rectangle width.
/// @param ah First rectangle height.
/// @param bx Second rectangle X.
/// @param by Second rectangle Y.
/// @param bw Second rectangle width.
/// @param bh Second rectangle height.
/// @return `1` when interiors overlap, otherwise `0`.
/// @details Edge coordinates use saturating addition.
static int8_t rects_intersect(int64_t ax,
                              int64_t ay,
                              int64_t aw,
                              int64_t ah,
                              int64_t bx,
                              int64_t by,
                              int64_t bw,
                              int64_t bh) {
    int64_t ar = qt_saturating_add(ax, aw);
    int64_t ab = qt_saturating_add(ay, ah);
    int64_t br = qt_saturating_add(bx, bw);
    int64_t bb = qt_saturating_add(by, bh);
    return !(ax >= br || ar <= bx || ay >= bb || ab <= by);
}

/// @brief Test a top-left AABB against a circle.
/// @param rx Rectangle X.
/// @param ry Rectangle Y.
/// @param rw Rectangle width.
/// @param rh Rectangle height.
/// @param cx Circle center X.
/// @param cy Circle center Y.
/// @param radius Nonnegative circle radius.
/// @return `1` when the closest rectangle point lies inside or on the circle.
/// @details Distance arithmetic saturates to avoid signed overflow.
static int8_t rect_intersects_circle(
    int64_t rx, int64_t ry, int64_t rw, int64_t rh, int64_t cx, int64_t cy, int64_t radius) {
    int64_t nearest_x = cx;
    int64_t nearest_y = cy;
    if (nearest_x < rx)
        nearest_x = rx;
    else if (nearest_x > qt_saturating_add(rx, rw))
        nearest_x = qt_saturating_add(rx, rw);
    if (nearest_y < ry)
        nearest_y = ry;
    else if (nearest_y > qt_saturating_add(ry, rh))
        nearest_y = qt_saturating_add(ry, rh);

    int64_t dx = qt_saturating_sub(cx, nearest_x);
    int64_t dy = qt_saturating_sub(cy, nearest_y);
    int64_t dx2 = qt_saturating_mul(dx, dx);
    int64_t dy2 = qt_saturating_mul(dy, dy);
    int64_t rr = qt_saturating_mul(radius, radius);
    return qt_saturating_add(dx2, dy2) <= rr;
}

/// @brief Split a leaf into NW, NE, SW, and SE children.
/// @param node Leaf to split.
/// @return `1` on success, or `0` when already split, at maximum depth, too
///         small, or any child allocation fails.
/// @details Partial child allocation is rolled back transactionally.
static int8_t split_node(struct qt_node *node) {
    if (node->is_split || node->depth >= RT_QUADTREE_MAX_DEPTH)
        return 0;

    int64_t half_w = node->width / 2;
    int64_t half_h = node->height / 2;
    if (half_w <= 0 || half_h <= 0)
        return 0;
    int64_t next_depth = node->depth + 1;

    // NW, NE, SW, SE
    node->children[0] = create_node(node->x, node->y, half_w, half_h, next_depth);
    node->children[1] =
        create_node(qt_saturating_add(node->x, half_w), node->y, half_w, half_h, next_depth);
    node->children[2] =
        create_node(node->x, qt_saturating_add(node->y, half_h), half_w, half_h, next_depth);
    node->children[3] = create_node(qt_saturating_add(node->x, half_w),
                                    qt_saturating_add(node->y, half_h),
                                    half_w,
                                    half_h,
                                    next_depth);
    for (int i = 0; i < 4; ++i) {
        if (node->children[i])
            continue;
        for (int j = 0; j < 4; ++j) {
            destroy_node(node->children[j]);
            node->children[j] = NULL;
        }
        node->is_split = 0;
        return 0;
    }

    node->is_split = 1;
    return 1;
}

/// @brief Select the single child fully containing an AABB.
/// @param node Parent supplying subdivision midpoints.
/// @param x AABB top-left X.
/// @param y AABB top-left Y.
/// @param w AABB width.
/// @param h AABB height.
/// @return Child index 0–3 for NW/NE/SW/SE, or `-1` when the AABB spans a
///         midpoint or cannot be assigned.
static int get_quadrant(struct qt_node *node, int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t mid_x = qt_saturating_add(node->x, node->width / 2);
    int64_t mid_y = qt_saturating_add(node->y, node->height / 2);

    int8_t in_top = y < mid_y;
    int8_t in_bottom = qt_saturating_add(y, h) > mid_y;
    int8_t in_left = x < mid_x;
    int8_t in_right = qt_saturating_add(x, w) > mid_x;

    // If spanning multiple quadrants, stay in parent
    if ((in_top && in_bottom) || (in_left && in_right))
        return -1;

    if (in_top && in_left)
        return 0; // NW
    if (in_top && in_right)
        return 1; // NE
    if (in_bottom && in_left)
        return 2; // SW
    if (in_bottom && in_right)
        return 3; // SE

    return -1;
}

/// @brief Insert a flat item index into the deepest suitable node.
/// @param tree Tree owning the flat item.
/// @param node Subtree root.
/// @param item_idx Valid flat item index.
/// @return `1` on success, or `0` when node/index storage allocation fails.
/// @details Full leaves split when possible and redistribute contained items.
///          Midpoint-spanning items remain in the parent, whose buffer grows
///          beyond the nominal leaf threshold as needed.
static int8_t insert_into_node(struct rt_quadtree_impl *tree,
                               struct qt_node *node,
                               int64_t item_idx) {
    if (!node)
        return 0;

    struct qt_item *item = &tree->items[item_idx];
    int64_t x = qt_saturating_sub(item->x, item->width / 2); // Convert center to top-left
    int64_t y = qt_saturating_sub(item->y, item->height / 2);

    // If node is split, try to insert into child
    if (node->is_split) {
        int quad = get_quadrant(node, x, y, item->width, item->height);
        if (quad >= 0 && node->children[quad]) {
            return insert_into_node(tree, node->children[quad], item_idx);
        }

        if (!ensure_node_capacity(node, node->item_count + 1))
            return 0;
        node->items[node->item_count++] = item_idx;
        return 1;
    }

    // Insert into this node
    if (node->item_count < RT_QUADTREE_MAX_ITEMS) {
        node->items[node->item_count++] = item_idx;
        return 1;
    }

    // Node is full, try to split
    if (!node->is_split && node->depth < RT_QUADTREE_MAX_DEPTH) {
        if (!split_node(node)) {
            if (!ensure_node_capacity(node, node->item_count + 1))
                return 0;
            node->items[node->item_count++] = item_idx;
            return 1;
        }

        // Re-distribute existing items
        for (int64_t i = 0; i < node->item_count; i++) {
            struct qt_item *existing = &tree->items[node->items[i]];
            int64_t ex = qt_saturating_sub(existing->x, existing->width / 2);
            int64_t ey = qt_saturating_sub(existing->y, existing->height / 2);
            int quad = get_quadrant(node, ex, ey, existing->width, existing->height);
            if (quad >= 0 && node->children[quad]) {
                if (insert_into_node(tree, node->children[quad], node->items[i])) {
                    // Move last item to this slot
                    node->items[i] = node->items[node->item_count - 1];
                    node->item_count--;
                    i--; // Re-check this slot
                }
            }
        }

        // Try again with the new item
        int quad = get_quadrant(node, x, y, item->width, item->height);
        if (quad >= 0 && node->children[quad]) {
            return insert_into_node(tree, node->children[quad], item_idx);
        }
    }

    // Spanning items remain in the parent even after a split; grow storage as needed.
    if (!ensure_node_capacity(node, node->item_count + 1))
        return 0;
    node->items[node->item_count++] = item_idx;
    return 1;
}

/// @brief Recursively append active items intersecting an AABB.
/// @param tree Tree owning items and transient results.
/// @param node Subtree root.
/// @param x Query top-left X.
/// @param y Query top-left Y.
/// @param w Query width.
/// @param h Query height.
/// @details Recursion prunes nonintersecting child bounds and stops after a
///          result-allocation failure marks the query truncated.
static void query_node(struct rt_quadtree_impl *tree,
                       struct qt_node *node,
                       int64_t x,
                       int64_t y,
                       int64_t w,
                       int64_t h) {
    if (!node)
        return;
    if (tree->query_truncated)
        return;

    // Check items in this node
    for (int64_t i = 0; i < node->item_count; i++) {
        struct qt_item *item = &tree->items[node->items[i]];
        if (!item->active)
            continue;

        // Check if item intersects query region
        int64_t ix = qt_saturating_sub(item->x, item->width / 2);
        int64_t iy = qt_saturating_sub(item->y, item->height / 2);

        if (rects_intersect(x, y, w, h, ix, iy, item->width, item->height)) {
            if (!append_query_result(tree, item->id))
                return;
        }
    }

    // Query children
    if (node->is_split) {
        for (int i = 0; i < 4; i++) {
            if (node->children[i] && intersects(node->children[i], x, y, w, h)) {
                query_node(tree, node->children[i], x, y, w, h);
            }
        }
    }
}

/// @brief Remove the first node reference to an item ID.
/// @param tree Tree owning flat items.
/// @param node Subtree to scan.
/// @param id Item ID to remove.
/// @return `1` when removed, otherwise `0`.
/// @details Node removal swaps in the last local index and does not merge
///          underfull nodes.
static int8_t remove_from_node(struct rt_quadtree_impl *tree, struct qt_node *node, int64_t id) {
    if (!node)
        return 0;

    // Check items in this node
    for (int64_t i = 0; i < node->item_count; i++) {
        if (tree->items[node->items[i]].id == id) {
            // Remove by swapping with last
            node->items[i] = node->items[node->item_count - 1];
            node->item_count--;
            return 1;
        }
    }

    // Check children
    if (node->is_split) {
        for (int i = 0; i < 4; i++) {
            if (node->children[i] && remove_from_node(tree, node->children[i], id))
                return 1;
        }
    }

    return 0;
}

/// @brief Collect broad-phase pairs sharing a node or ancestor relationship.
/// @param tree Tree receiving transient pairs.
/// @param node Subtree root.
/// @param ancestors Shared scratch array of ancestor item indices.
/// @param ancestor_count Valid scratch entries.
/// @param ancestor_cap Scratch capacity.
/// @details The routine deliberately does not test AABB overlap: it returns
///          candidate pairs for a later narrow phase. Sibling-subtree items
///          cannot overlap because fully contained items are partitioned;
///          parent-spanning items are compared with descendants. Allocation
///          failure marks collection truncated and stops recursion.
static void collect_pairs_node(struct rt_quadtree_impl *tree,
                               struct qt_node *node,
                               int64_t *ancestors,
                               int64_t ancestor_count,
                               int64_t ancestor_cap) {
    if (!node)
        return;
    if (tree->pairs_truncated)
        return;

    // Check items in this node against each other
    for (int64_t i = 0; i < node->item_count; i++) {
        struct qt_item *item_i = &tree->items[node->items[i]];
        if (!item_i->active)
            continue;

        // Against other items in same node
        for (int64_t j = i + 1; j < node->item_count; j++) {
            struct qt_item *item_j = &tree->items[node->items[j]];
            if (!item_j->active)
                continue;

            if (!append_pair(tree, item_i->id, item_j->id))
                return;
        }

        // Against ancestor items
        for (int64_t a = 0; a < ancestor_count; a++) {
            struct qt_item *item_a = &tree->items[ancestors[a]];
            if (!item_a->active)
                continue;

            if (!append_pair(tree, item_i->id, item_a->id))
                return;
        }
    }

    // Recurse into children with the ancestor list extended by this node's
    // items. `ancestors` is a single shared scratch buffer for the whole
    // traversal and is sized to the current item high-water mark, so appending
    // in place and passing the new length down cannot overflow.
    if (node->is_split) {
        int64_t count = ancestor_count;
        for (int64_t i = 0; i < node->item_count && count < ancestor_cap; i++)
            ancestors[count++] = node->items[i];

        for (int i = 0; i < 4; i++) {
            if (node->children[i]) {
                collect_pairs_node(tree, node->children[i], ancestors, count, ancestor_cap);
            }
        }
    }
}

/// @brief Recursively release all node and flat-buffer storage.
/// @param obj Quadtree supplied by the runtime finalizer system.
static void quadtree_finalizer(void *obj) {
    struct rt_quadtree_impl *tree = (struct rt_quadtree_impl *)obj;
    destroy_node(tree->root);
    tree->root = NULL;
    free(tree->items);
    tree->items = NULL;
    free(tree->results);
    tree->results = NULL;
    free(tree->pairs);
    tree->pairs = NULL;
}

/// @brief Create a quadtree covering a fixed rectangular world region.
/// @param x Root bounds top-left X.
/// @param y Root bounds top-left Y.
/// @param width Positive root width.
/// @param height Positive root height.
/// @return New runtime-managed Quadtree reference, or `NULL` for invalid
///         dimensions or allocation failure.
/// @details Item, result, and pair buffers receive initial reservations; all
///          can grow later. The root begins as one empty unsplit node.
rt_quadtree rt_quadtree_new(int64_t x, int64_t y, int64_t width, int64_t height) {
    if (width <= 0 || height <= 0)
        return NULL;
    struct rt_quadtree_impl *tree =
        rt_obj_new_i64(RT_QUADTREE_CLASS_ID, sizeof(struct rt_quadtree_impl));
    if (!tree)
        return NULL;

    memset(tree, 0, sizeof(struct rt_quadtree_impl));

    if (!ensure_item_capacity(tree, QT_DEFAULT_ITEM_CAPACITY) ||
        !ensure_result_capacity(tree, RT_QUADTREE_MAX_RESULTS) ||
        !ensure_pair_capacity(tree, MAX_PAIRS)) {
        free(tree->items);
        free(tree->results);
        free(tree->pairs);
        if (rt_obj_release_check0(tree))
            rt_obj_free(tree);
        return NULL;
    }

    tree->root = create_node(x, y, width, height, 0);
    if (!tree->root) {
        free(tree->items);
        free(tree->results);
        free(tree->pairs);
        if (rt_obj_release_check0(tree))
            rt_obj_free(tree);
        return NULL;
    }

    rt_obj_set_finalizer(tree, quadtree_finalizer);
    return tree;
}

/// @brief Release one runtime reference to a Quadtree.
/// @param tree Handle to release; `NULL` is a no-op.
/// @details The final reference triggers recursive node and buffer teardown. A
///          non-null wrong-class handle raises a runtime trap.
void rt_quadtree_destroy(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.Destroy: expected Zanna.Game.Quadtree");
    if (tree && rt_obj_release_check0(tree))
        rt_obj_free(tree);
}

/// @brief Remove all items, results, pairs, and child subdivisions.
/// @param tree Tree to clear; `NULL` is a no-op.
/// @details Flat buffer allocations and root bounds are retained for reuse;
///          the item high-water mark resets to zero. A non-null wrong-class
///          handle raises a runtime trap.
void rt_quadtree_clear(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.Clear: expected Zanna.Game.Quadtree");
    if (!tree)
        return;

    // Clear all items
    for (int64_t i = 0; i < tree->item_count; i++) {
        tree->items[i].active = 0;
    }
    tree->item_count = 0;
    tree->result_count = 0;
    tree->query_truncated = 0;
    tree->pair_count = 0;
    tree->pairs_truncated = 0;

    // Clear tree structure
    if (tree->root)
        clear_node(tree->root);
}

/// @brief Insert an axis-aligned item into the quadtree.
/// (x, y) is the item *center*; (width, height) is its full extent. Returns 0
/// if the tree is full, the item lies outside the world bounds, or the ID is
/// already present (use `_update` to move an existing item instead).
/// @param tree Destination tree.
/// @param id Unique caller-defined identifier.
/// @param x AABB center X.
/// @param y AABB center Y.
/// @param width Positive full width.
/// @param height Positive full height.
/// @return `1` on success, otherwise `0`.
/// @details Any positive-area intersection with root bounds is accepted; the
///          item need not be fully contained. Inactive flat slots are reused.
///          Duplicate active IDs, invalid dimensions, disjoint bounds, and
///          allocation failure are rejected transactionally. A non-null
///          wrong-class handle raises a runtime trap.
int8_t rt_quadtree_insert(
    rt_quadtree tree, int64_t id, int64_t x, int64_t y, int64_t width, int64_t height) {
    tree = checked_quadtree(tree, "Quadtree.Insert: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;
    if (width <= 0 || height <= 0)
        return 0;

    // Guard against duplicate IDs — inserting the same ID twice leaves a ghost
    // after the first Remove(), producing phantom collision responses.
    for (int64_t i = 0; i < tree->item_count; i++) {
        if (tree->items[i].active && tree->items[i].id == id)
            return 0; // Already present; caller should use rt_quadtree_update()
    }

    // Check bounds
    int64_t left = qt_saturating_sub(x, width / 2);
    int64_t top = qt_saturating_sub(y, height / 2);
    if (!intersects(tree->root, left, top, width, height))
        return 0;

    // Reuse inactive slots so remove/insert cycles do not exhaust capacity.
    int64_t idx = -1;
    for (int64_t i = 0; i < tree->item_count; i++) {
        if (!tree->items[i].active) {
            idx = i;
            break;
        }
    }
    int8_t grew_item_array = 0;
    if (idx < 0) {
        if (!ensure_item_capacity(tree, tree->item_count + 1))
            return 0;
        idx = tree->item_count++;
        grew_item_array = 1;
    }

    // Add to item list
    tree->items[idx].id = id;
    tree->items[idx].x = x;
    tree->items[idx].y = y;
    tree->items[idx].width = width;
    tree->items[idx].height = height;
    tree->items[idx].active = 1;

    // Insert into tree
    if (insert_into_node(tree, tree->root, idx))
        return 1;

    // Roll back the activation if insertion fails so counts and duplicate checks stay correct.
    tree->items[idx].active = 0;
    if (grew_item_array)
        tree->item_count--;
    return 0;
}

/// @brief Remove the active item with a given ID.
/// @param tree Tree to modify.
/// @param id Unique item identifier.
/// @return `1` when found and removed, otherwise `0`.
/// @details The flat slot becomes reusable; node subdivisions are not merged.
///          Existing transient query/pair buffers are not recomputed. A
///          non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_remove(rt_quadtree tree, int64_t id) {
    tree = checked_quadtree(tree, "Quadtree.Remove: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;

    // Mark item as inactive
    for (int64_t i = 0; i < tree->item_count; i++) {
        if (tree->items[i].id == id && tree->items[i].active) {
            tree->items[i].active = 0;
            remove_from_node(tree, tree->root, id);
            return 1;
        }
    }

    return 0;
}

/// @brief Move/resize an existing item; equivalent to remove + insert but cheaper.
/// Returns 0 if the ID is not present.
/// @param tree Tree to modify.
/// @param id Existing active item identifier.
/// @param x New AABB center X.
/// @param y New AABB center Y.
/// @param width New positive width.
/// @param height New positive height.
/// @return `1` on success, otherwise `0`.
/// @details Disjoint root bounds and invalid dimensions are rejected before
///          mutation. If reinsertion fails, the old record and placement are
///          restored. Existing transient results are not recomputed. A
///          non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_update(
    rt_quadtree tree, int64_t id, int64_t x, int64_t y, int64_t width, int64_t height) {
    tree = checked_quadtree(tree, "Quadtree.Update: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;
    if (width <= 0 || height <= 0)
        return 0;

    // Find and update item
    for (int64_t i = 0; i < tree->item_count; i++) {
        if (tree->items[i].id == id && tree->items[i].active) {
            int64_t left = qt_saturating_sub(x, width / 2);
            int64_t top = qt_saturating_sub(y, height / 2);
            if (!intersects(tree->root, left, top, width, height))
                return 0;

            struct qt_item old_item = tree->items[i];

            // Remove from tree
            remove_from_node(tree, tree->root, id);

            // Update position
            tree->items[i].x = x;
            tree->items[i].y = y;
            tree->items[i].width = width;
            tree->items[i].height = height;

            // Re-insert
            if (insert_into_node(tree, tree->root, i))
                return 1;

            // Restore the old state if the new placement fails.
            tree->items[i] = old_item;
            insert_into_node(tree, tree->root, i);
            return 0;
        }
    }

    return 0;
}

/// @brief Find all items intersecting the given AABB; returns the result count.
/// Results are stored internally and accessed via `_get_result(i)`. If more than
/// the internal result buffer grows to hold every match. `_query_was_truncated`
/// returns 1 only if allocation fails and a partial result had to be returned.
/// @param tree Tree to query.
/// @param x Query top-left X.
/// @param y Query top-left Y.
/// @param width Positive query width.
/// @param height Positive query height.
/// @return Number of stored matching IDs, or zero for invalid input.
/// @details Edge touching is not intersection. Each call replaces the previous
///          transient result and truncation flag. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_quadtree_query_rect(
    rt_quadtree tree, int64_t x, int64_t y, int64_t width, int64_t height) {
    tree = checked_quadtree(tree, "Quadtree.QueryRect: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;

    tree->result_count = 0;
    tree->query_truncated = 0;
    if (width <= 0 || height <= 0)
        return 0;

    query_node(tree, tree->root, x, y, width, height);
    return tree->result_count;
}

/// @brief Query an AABB and copy its IDs into an immutable snapshot.
/// @param tree Tree to query.
/// @param x Query top-left X.
/// @param y Query top-left Y.
/// @param width Query width.
/// @param height Query height.
/// @return New QueryResult, or `NULL` if result-object allocation fails.
/// @details Search behavior matches rt_quadtree_query_rect(). A null tree
///          produces an empty, nontruncated snapshot. ID-copy failure produces
///          a valid empty snapshot marked truncated. A non-null wrong-class
///          handle raises a runtime trap.
void *rt_quadtree_query_rect_result(
    rt_quadtree tree, int64_t x, int64_t y, int64_t width, int64_t height) {
    tree = checked_quadtree(tree, "Quadtree.QueryRectResult: expected Zanna.Game.Quadtree");
    if (!tree)
        return query_result_from_ids(NULL, 0, 0);
    (void)rt_quadtree_query_rect(tree, x, y, width, height);
    return query_result_from_ids(tree->results, tree->result_count, tree->query_truncated);
}

/// @brief Test whether result allocation truncated the latest spatial query.
/// @param tree Tree to query.
/// @return Stored truncation flag, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_query_was_truncated(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.QueryWasTruncated: expected Zanna.Game.Quadtree");
    return tree ? tree->query_truncated : 0;
}

/// @brief Find item AABBs intersecting a circle.
/// @param tree Tree to query.
/// @param x Circle center X.
/// @param y Circle center Y.
/// @param radius Radius; negative values become zero.
/// @return Number of stored matching IDs, or zero for invalid input.
/// @details The operation first runs a bounding-square query, then filters with
///          an AABB-to-circle test. Radius zero uses a 1×1 broad-phase square.
///          It replaces the previous transient query. A non-null wrong-class
///          handle raises a runtime trap.
int64_t rt_quadtree_query_point(rt_quadtree tree, int64_t x, int64_t y, int64_t radius) {
    tree = checked_quadtree(tree, "Quadtree.QueryPoint: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;
    if (radius < 0)
        radius = 0;

    int64_t diameter = radius == 0 ? 1 : qt_saturating_mul(radius, 2);
    rt_quadtree_query_rect(
        tree, qt_saturating_sub(x, radius), qt_saturating_sub(y, radius), diameter, diameter);

    int64_t filtered_count = 0;
    for (int64_t i = 0; i < tree->result_count; i++) {
        int64_t id = tree->results[i];
        for (int64_t item_idx = 0; item_idx < tree->item_count; item_idx++) {
            struct qt_item *item = &tree->items[item_idx];
            if (!item->active || item->id != id)
                continue;

            int64_t ix = qt_saturating_sub(item->x, item->width / 2);
            int64_t iy = qt_saturating_sub(item->y, item->height / 2);
            if (rect_intersects_circle(ix, iy, item->width, item->height, x, y, radius))
                tree->results[filtered_count++] = id;
            break;
        }
    }

    tree->result_count = filtered_count;
    return filtered_count;
}

/// @brief Query a circle and copy its IDs into an immutable snapshot.
/// @param tree Tree to query.
/// @param x Circle center X.
/// @param y Circle center Y.
/// @param radius Radius; negative values become zero.
/// @return New QueryResult, or `NULL` if result-object allocation fails.
/// @details Search behavior matches rt_quadtree_query_point(). A null tree
///          produces an empty nontruncated snapshot; copy failure marks the
///          returned empty snapshot truncated. A non-null wrong-class handle
///          raises a runtime trap.
void *rt_quadtree_query_point_result(rt_quadtree tree, int64_t x, int64_t y, int64_t radius) {
    tree = checked_quadtree(tree, "Quadtree.QueryPointResult: expected Zanna.Game.Quadtree");
    if (!tree)
        return query_result_from_ids(NULL, 0, 0);
    (void)rt_quadtree_query_point(tree, x, y, radius);
    return query_result_from_ids(tree->results, tree->result_count, tree->query_truncated);
}

/// @brief Read one ID from the transient result of the latest spatial query.
/// @param tree Tree whose result buffer is read.
/// @param index Zero-based result index.
/// @return Item ID, or `-1` for null/out-of-range input.
/// @details The sentinel is ambiguous when `-1` is a stored ID. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_quadtree_get_result(rt_quadtree tree, int64_t index) {
    tree = checked_quadtree(tree, "Quadtree.GetResult: expected Zanna.Game.Quadtree");
    if (!tree || index < 0 || index >= tree->result_count)
        return -1;
    return tree->results[index];
}

/// @brief Read the latest transient spatial-query count.
/// @param tree Tree to query.
/// @return Stored result count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_result_count(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.ResultCount: expected Zanna.Game.Quadtree");
    return tree ? tree->result_count : 0;
}

/// @brief Read an immutable QueryResult's ID count.
/// @param ptr QueryResult to inspect.
/// @return Stored count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_game_query_result_count(void *ptr) {
    struct rt_game_query_result_impl *result =
        checked_query_result(ptr, "QueryResult.Count: expected Zanna.Game.QueryResult");
    return result ? result->count : 0;
}

/// @brief Read one ID from an immutable QueryResult.
/// @param ptr QueryResult to inspect.
/// @param index Zero-based ID index.
/// @return Stored ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_game_query_result_get_id(void *ptr, int64_t index) {
    struct rt_game_query_result_impl *result =
        checked_query_result(ptr, "QueryResult.GetId: expected Zanna.Game.QueryResult");
    if (!result || index < 0 || index >= result->count)
        return -1;
    return result->ids[index];
}

/// @brief Test whether an immutable QueryResult contains an ID.
/// @param ptr QueryResult to scan.
/// @param id Identifier to find.
/// @return `1` on the first match, otherwise `0`.
/// @details The scan is linear. A non-null wrong-class handle raises a trap.
int8_t rt_game_query_result_contains(void *ptr, int64_t id) {
    struct rt_game_query_result_impl *result =
        checked_query_result(ptr, "QueryResult.Contains: expected Zanna.Game.QueryResult");
    if (!result)
        return 0;
    for (int64_t i = 0; i < result->count; ++i) {
        if (result->ids[i] == id)
            return 1;
    }
    return 0;
}

/// @brief Read an immutable QueryResult's partial-result flag.
/// @param ptr QueryResult to inspect.
/// @return `1` when originating collection or snapshot copying was truncated,
///         otherwise `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_game_query_result_truncated(void *ptr) {
    struct rt_game_query_result_impl *result =
        checked_query_result(ptr, "QueryResult.Truncated: expected Zanna.Game.QueryResult");
    return result ? result->truncated : 0;
}

/// @brief Copy immutable query IDs into a new owning Seq.
/// @param ptr QueryResult to copy.
/// @return New `Seq[Integer]`, an empty Seq for `NULL`, or `NULL` if Seq
///         allocation fails.
/// @details Each ID is boxed and the Seq owns its element references. A
///          non-null wrong-class handle raises a runtime trap.
void *rt_game_query_result_ids(void *ptr) {
    struct rt_game_query_result_impl *result =
        checked_query_result(ptr, "QueryResult.Ids: expected Zanna.Game.QueryResult");
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    if (!result)
        return seq;
    rt_seq_set_owns_elements(seq, 1);
    for (int64_t i = 0; i < result->count; ++i) {
        void *boxed = rt_box_i64(result->ids[i]);
        rt_seq_push(seq, boxed);
        if (boxed && rt_obj_release_check0(boxed))
            rt_obj_free(boxed);
    }
    return seq;
}

/// @brief Count active flat-item records.
/// @param tree Tree to inspect.
/// @return Active item count, or zero for `NULL`.
/// @details The scan is linear in the flat high-water mark. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_quadtree_item_count(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.ItemCount: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;

    int64_t count = 0;
    for (int64_t i = 0; i < tree->item_count; i++) {
        if (tree->items[i].active)
            count++;
    }
    return count;
}

/// @brief Compute all potentially-colliding pairs in the quadtree and return the count.
/// @param tree Tree to traverse.
/// @return Number of stored candidate pairs, or zero for `NULL` or scratch
///         allocation failure.
/// @details Pairs share a node or an ancestor/descendant relationship. No AABB
///          overlap test is performed; callers must run a narrow phase. Each
///          call replaces the transient pair list. Scratch or result-storage
///          allocation failure marks the collection truncated. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_quadtree_get_pairs(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.GetPairs: expected Zanna.Game.Quadtree");
    if (!tree)
        return 0;

    tree->pair_count = 0;
    tree->pairs_truncated = 0;
    // Shared ancestor scratch for the whole DFS, sized to the current item
    // high-water mark so the ancestor list can never overflow.
    int64_t ancestor_cap = tree->item_count > 0 ? tree->item_count : 1;
    if ((uint64_t)ancestor_cap > SIZE_MAX / sizeof(int64_t)) {
        tree->pairs_truncated = 1;
        return 0;
    }
    int64_t *ancestors = (int64_t *)malloc((size_t)ancestor_cap * sizeof(int64_t));
    if (!ancestors) {
        tree->pairs_truncated = 1; // no scratch -> cannot guarantee completeness
        return 0;
    }
    collect_pairs_node(tree, tree->root, ancestors, 0, ancestor_cap);
    free(ancestors);
    return tree->pair_count;
}

/// @brief Collect broad-phase pairs into an immutable snapshot.
/// @param tree Tree to traverse.
/// @return New QuadtreePairResult, or `NULL` if result-object allocation fails.
/// @details Collection matches rt_quadtree_get_pairs(). A null tree produces an
///          empty nontruncated snapshot. Pair-copy failure produces a valid
///          empty snapshot marked truncated. A non-null wrong-class handle
///          raises a runtime trap.
void *rt_quadtree_query_pairs(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.QueryPairs: expected Zanna.Game.Quadtree");
    if (!tree)
        return pair_result_from_pairs(NULL, 0, 0);
    (void)rt_quadtree_get_pairs(tree);
    return pair_result_from_pairs(tree->pairs, tree->pair_count, tree->pairs_truncated);
}

/// @brief Read the first ID of a transient broad-phase pair.
/// @param tree Tree whose latest pair buffer is read.
/// @param pair_index Zero-based pair index.
/// @return First ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_first(rt_quadtree tree, int64_t pair_index) {
    tree = checked_quadtree(tree, "Quadtree.PairFirst: expected Zanna.Game.Quadtree");
    if (!tree || pair_index < 0 || pair_index >= tree->pair_count)
        return -1;
    return tree->pairs[pair_index].first;
}

/// @brief Read the second ID of a transient broad-phase pair.
/// @param tree Tree whose latest pair buffer is read.
/// @param pair_index Zero-based pair index.
/// @return Second ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_second(rt_quadtree tree, int64_t pair_index) {
    tree = checked_quadtree(tree, "Quadtree.PairSecond: expected Zanna.Game.Quadtree");
    if (!tree || pair_index < 0 || pair_index >= tree->pair_count)
        return -1;
    return tree->pairs[pair_index].second;
}

/// @brief Check whether the last rt_quadtree_get_pairs() returned partial pairs.
/// @param tree Tree to inspect.
/// @return Stored truncation flag, or zero for `NULL`.
/// @details Mirrors rt_quadtree_query_was_truncated() for the broad-phase pair
///          list. A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_pairs_was_truncated(rt_quadtree tree) {
    tree = checked_quadtree(tree, "Quadtree.PairsWasTruncated: expected Zanna.Game.Quadtree");
    return tree ? tree->pairs_truncated : 0;
}

/// @brief Read an immutable pair snapshot's count.
/// @param ptr QuadtreePairResult to inspect.
/// @return Stored pair count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_count(void *ptr) {
    struct rt_quadtree_pair_result_impl *result = checked_pair_result(
        ptr, "QuadtreePairResult.Count: expected Zanna.Game.QuadtreePairResult");
    return result ? result->count : 0;
}

/// @brief Read the first ID of an immutable candidate pair.
/// @param ptr QuadtreePairResult to inspect.
/// @param index Zero-based pair index.
/// @return First ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_first(void *ptr, int64_t index) {
    struct rt_quadtree_pair_result_impl *result = checked_pair_result(
        ptr, "QuadtreePairResult.First: expected Zanna.Game.QuadtreePairResult");
    if (!result || index < 0 || index >= result->count)
        return -1;
    return result->pairs[index].first;
}

/// @brief Read the second ID of an immutable candidate pair.
/// @param ptr QuadtreePairResult to inspect.
/// @param index Zero-based pair index.
/// @return Second ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_second(void *ptr, int64_t index) {
    struct rt_quadtree_pair_result_impl *result = checked_pair_result(
        ptr, "QuadtreePairResult.Second: expected Zanna.Game.QuadtreePairResult");
    if (!result || index < 0 || index >= result->count)
        return -1;
    return result->pairs[index].second;
}

/// @brief Read an immutable pair snapshot's partial-result flag.
/// @param ptr QuadtreePairResult to inspect.
/// @return `1` when collection or snapshot copying was truncated, otherwise
///         `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_pair_result_truncated(void *ptr) {
    struct rt_quadtree_pair_result_impl *result = checked_pair_result(
        ptr, "QuadtreePairResult.Truncated: expected Zanna.Game.QuadtreePairResult");
    return result ? result->truncated : 0;
}
