//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_quadtree.h
/// @file
/// @brief Declares a growable spatial quadtree and immutable query snapshots.
//
// Purpose: Quadtree spatial partition for efficient collision queries, reducing O(n^2) broad-phase
// checks to O(n log n) by subdividing 2D space into adaptive quad cells.
//
// Key invariants:
//   - Items are identified by unique int64 IDs.
//   - The tree subdivides up to RT_QUADTREE_MAX_DEPTH levels.
//   - Leaf nodes hold at most RT_QUADTREE_MAX_ITEMS before splitting.
//   - Query results remain valid until the next query or mutation.
//
// Ownership/Lifetime:
//   - Quadtree and snapshot handles are runtime reference-counted objects.
//   - Internal transient query/pair buffers are owned by the quadtree.
//   - Snapshot results own independent copied arrays.
//
// Links: src/runtime/game/rt_quadtree.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate Quadtree handles.
#define RT_QUADTREE_CLASS_ID INT64_C(-0x510209)

/// @brief Runtime class ID used to validate QueryResult snapshots.
#define RT_GAME_QUERY_RESULT_CLASS_ID INT64_C(-0x51021D)

/// @brief Runtime class ID used to validate QuadtreePairResult snapshots.
#define RT_QUADTREE_PAIR_RESULT_CLASS_ID INT64_C(-0x51021E)

/// @brief Nominal item threshold at which a leaf attempts to split.
#define RT_QUADTREE_MAX_ITEMS 8

/// @brief Maximum subdivision depth below the root.
#define RT_QUADTREE_MAX_DEPTH 8

/// @brief Initial transient ID reservation; results grow beyond it on demand.
#define RT_QUADTREE_MAX_RESULTS 256

/// @brief Opaque handle to a runtime reference-counted Quadtree.
typedef struct rt_quadtree_impl *rt_quadtree;

/// @brief Create a new Quadtree covering the specified bounds.
/// @param x Bounds top-left X in caller-defined world units.
/// @param y Bounds Y (top-left).
/// @param width Positive bounds width.
/// @param height Positive bounds height.
/// @return A new Quadtree reference, or `NULL` for invalid dimensions or
///         allocation failure.
/// @details Coordinate scale is caller-defined but must be consistent across
///          bounds, items, and queries.
rt_quadtree rt_quadtree_new(int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Release one runtime reference to a Quadtree.
/// @param tree Handle to release; `NULL` is a no-op.
/// @details Final release recursively frees nodes and flat buffers. A non-null
///          wrong-class handle raises a runtime trap.
void rt_quadtree_destroy(rt_quadtree tree);

/// @brief Remove all items, transient results, pairs, and subdivisions.
/// @param tree Tree to clear; `NULL` is a no-op.
/// @details Root bounds and flat allocations remain reusable. A non-null
///          wrong-class handle raises a runtime trap.
void rt_quadtree_clear(rt_quadtree tree);

/// @brief Insert an item into the quadtree.
/// @param tree The quadtree.
/// @param id Unique item ID.
/// @param x Item X position (center).
/// @param y Item Y position (center).
/// @param width Item width.
/// @param height Item height.
/// @return `1` on success, or `0` for null input, nonpositive dimensions, a
///         duplicate active ID, disjoint root bounds, or allocation failure.
/// @details Partial overlap with root bounds is accepted. Inactive flat slots
///          are reused and failed node insertion rolls back activation. A
///          non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_insert(
    rt_quadtree tree, int64_t id, int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Remove an item from the quadtree.
/// @param tree Tree to modify.
/// @param id Item ID to remove.
/// @return `1` if found and removed, otherwise `0`.
/// @details Node subdivisions are not merged and transient results are not
///          recomputed. A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_remove(rt_quadtree tree, int64_t id);

/// @brief Update an item's position and size (remove + re-insert).
/// @param tree Tree to modify.
/// @param id Item ID.
/// @param x New X position.
/// @param y New Y position.
/// @param width Item width.
/// @param height Item height.
/// @return `1` on success, or `0` if missing, invalid, disjoint, or reinsertion
///         fails.
/// @details Reinsertion failure restores the old record and placement.
///          Transient results are not recomputed. A non-null wrong-class handle
///          raises a runtime trap.
int8_t rt_quadtree_update(
    rt_quadtree tree, int64_t id, int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Query items intersecting a rectangular region.
/// @param tree Tree to query.
/// @param x Query region X.
/// @param y Query region Y.
/// @param width Query region width.
/// @param height Query region height.
/// @return Number of stored matching IDs, or zero for invalid input.
/// @details Edge touching is not overlap. The result buffer grows on demand;
///          allocation failure returns a partial count and sets the truncation
///          flag. This call replaces the previous transient spatial query. A
///          non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_query_rect(
    rt_quadtree tree, int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Query items intersecting a rectangle and return a snapshot result.
/// @param tree Tree to query.
/// @param x Query region X.
/// @param y Query region Y.
/// @param width Query region width.
/// @param height Query region height.
/// @return New immutable QueryResult, or `NULL` if result-object allocation
///         fails.
/// @details Search matches rt_quadtree_query_rect(). A null tree yields an
///          empty nontruncated snapshot. Copy allocation failure yields a
///          valid empty snapshot marked truncated. Snapshots survive later
///          queries and mutations. A non-null wrong-class handle traps.
void *rt_quadtree_query_rect_result(
    rt_quadtree tree, int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Query items near a point within a given radius.
/// @param tree Tree to query.
/// @param x Center X of the search area.
/// @param y Center Y of the search area.
/// @param radius Search radius around the point.
/// @return Number of stored item AABBs intersecting the circle.
/// @details Negative radius becomes zero. A bounding-square query is filtered
///          by closest-point AABB/circle intersection and replaces the previous
///          transient query. Allocation failure may truncate results. A
///          non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_query_point(rt_quadtree tree, int64_t x, int64_t y, int64_t radius);

/// @brief Query items near a point and return a snapshot result.
/// @param tree Tree to query.
/// @param x Center X of the search area.
/// @param y Center Y of the search area.
/// @param radius Search radius around the point.
/// @return New immutable QueryResult, or `NULL` if result-object allocation
///         fails.
/// @details Search matches rt_quadtree_query_point(). Null produces an empty
///          nontruncated snapshot; copy failure yields an empty truncated
///          snapshot. A non-null wrong-class handle raises a runtime trap.
void *rt_quadtree_query_point_result(rt_quadtree tree, int64_t x, int64_t y, int64_t radius);

/// @brief Get an item ID from the last query result.
/// @param tree Tree whose transient result is read.
/// @param index Result index (0 to query_count-1).
/// @return Item ID, or `-1` for null/out-of-range input.
/// @details The sentinel is ambiguous if `-1` is a valid stored ID. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_quadtree_get_result(rt_quadtree tree, int64_t index);

/// @brief Get the number of results from the last query.
/// @param tree Tree to inspect.
/// @return Latest transient spatial-query count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_result_count(rt_quadtree tree);

/// @brief Return the number of IDs in a QueryResult.
/// @param result QueryResult to inspect.
/// @return Stored count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_game_query_result_count(void *result);

/// @brief Read an item ID from a QueryResult by index.
/// @param result QueryResult to inspect.
/// @param index Zero-based item index.
/// @return Item ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_game_query_result_get_id(void *result, int64_t index);

/// @brief Test whether a QueryResult contains an item ID.
/// @param result QueryResult to scan.
/// @param id Item ID to search for.
/// @return `1` when found, otherwise `0`.
/// @details Search is linear. A non-null wrong-class handle raises a trap.
int8_t rt_game_query_result_contains(void *result, int64_t id);

/// @brief Check whether a QueryResult is partial because storage growth failed.
/// @param result QueryResult to inspect.
/// @return `1` when collection or snapshot copying was partial, otherwise `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_game_query_result_truncated(void *result);

/// @brief Copy QueryResult IDs into a new Zanna.Collections.Seq.
/// @param result QueryResult to copy.
/// @return New owning `Seq[Integer]`, an empty Seq for `NULL`, or `NULL` if Seq
///         allocation fails.
/// @details Each ID is boxed. A non-null wrong-class handle raises a trap.
void *rt_game_query_result_ids(void *result);

/// @brief Check whether the last query returned partial results.
///
/// Returns 1 only if the most recent rt_quadtree_query_rect() or
/// rt_quadtree_query_point() could not grow its result storage and therefore
/// returned a partial result. Normal queries grow beyond RT_QUADTREE_MAX_RESULTS.
/// @param tree Tree to inspect.
/// @return `1` if the latest spatial query was truncated, otherwise `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_query_was_truncated(rt_quadtree tree);

/// @brief Get the total number of items in the tree.
/// @param tree Tree to inspect.
/// @return Active item count, or zero for `NULL`.
/// @details The count is computed by scanning the flat high-water range. A
///          non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_item_count(rt_quadtree tree);

/// @brief Compute potential collision pairs (broad phase).
/// @param tree Tree to traverse.
/// @return Number of stored candidate pairs, or zero for null/scratch failure.
/// @details Pairs share a node or ancestor relationship; AABB overlap is not
///          tested, so callers must perform a narrow phase. This replaces the
///          transient pair buffer. Allocation failure marks it truncated. A
///          non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_get_pairs(rt_quadtree tree);

/// @brief Compute broad-phase collision pairs and return a snapshot result.
/// @param tree Tree to traverse.
/// @return New immutable QuadtreePairResult, or `NULL` if result-object
///         allocation fails.
/// @details Collection matches rt_quadtree_get_pairs(). A null tree yields an
///          empty nontruncated snapshot; copy failure yields an empty truncated
///          snapshot. A non-null wrong-class handle raises a runtime trap.
void *rt_quadtree_query_pairs(rt_quadtree tree);

/// @brief Get the first item ID of a collision pair.
/// @param tree Tree whose transient pair buffer is read.
/// @param pair_index Pair index (0 to pair_count-1).
/// @return First item ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_first(rt_quadtree tree, int64_t pair_index);

/// @brief Get the second item ID of a collision pair.
/// @param tree Tree whose transient pair buffer is read.
/// @param pair_index Pair index (0 to pair_count-1).
/// @return Second item ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_second(rt_quadtree tree, int64_t pair_index);

/// @brief Check whether the last rt_quadtree_get_pairs() returned partial pairs.
///
/// Returns 1 only if the most recent rt_quadtree_get_pairs() could not grow its
/// pair storage and therefore returned partial pairs.
/// @param tree Tree to inspect.
/// @return `1` if the latest pair collection was truncated, otherwise `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_pairs_was_truncated(rt_quadtree tree);

/// @brief Return the number of pairs in a QuadtreePairResult.
/// @param result QuadtreePairResult to inspect.
/// @return Stored pair count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_count(void *result);

/// @brief Read the first item ID from a pair result by index.
/// @param result QuadtreePairResult to inspect.
/// @param index Zero-based pair index.
/// @return First item ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_first(void *result, int64_t index);

/// @brief Read the second item ID from a pair result by index.
/// @param result QuadtreePairResult to inspect.
/// @param index Zero-based pair index.
/// @return Second item ID, or `-1` for null/out-of-range input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_quadtree_pair_result_second(void *result, int64_t index);

/// @brief Check whether a pair result is partial because storage growth failed.
/// @param result QuadtreePairResult to inspect.
/// @return `1` when collection or snapshot copying was partial, otherwise `0`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_quadtree_pair_result_truncated(void *result);

#ifdef __cplusplus
}
#endif
