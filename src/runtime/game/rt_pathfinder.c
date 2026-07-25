//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_pathfinder.c
/// @file
/// @brief Implements fixed-grid A* pathfinding, nearest-value search, and
///        immutable path-result snapshots.
//
// Purpose: A* grid pathfinding. Uses a binary min-heap open set and flat
//   per-node arrays for g-cost, parent, and closed state. Supports 4-way
//   (Manhattan heuristic) and 8-way (octile heuristic) movement with per-cell
//   movement cost weights.
//
// Key invariants:
//   - Node arrays are pre-allocated to width×height at search time, then freed.
//   - Heap stores indices into the flat node array; heapified by f-cost.
//   - Fixed-point costs: base cost 100, diagonal cost 141 (~√2 × 100).
//   - Heuristic is admissible and consistent for both 4-way and 8-way.
//   - Path reconstruction walks parent chain from goal → start, then reverses.
//
// Ownership/Lifetime:
//   - Pathfinder objects are GC-managed. The cells array uses a finalizer.
//   - Per-search node/heap arrays are stack-like (malloc/free per call).
//   - Returned List is a new GC-managed runtime List[Integer].
//
// Links: rt_pathfinder.h (API), rt_tilemap.h (FromTilemap), rt_grid2d.c (FromGrid2D)
//
//===----------------------------------------------------------------------===//

#include "rt_pathfinder.h"
#include "rt_box.h"
#include "rt_grid2d.h"
#include "rt_list.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_tilemap.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Internal Data Structures
//=============================================================================

/// @brief Per-cell static data.
typedef struct {
    int16_t cost;    ///< Movement cost multiplier (100 = normal, 0 = impassable).
    int64_t value;   ///< Source grid/tile value for nearest-value queries.
    int8_t walkable; ///< 1 = passable, 0 = wall.
} pf_cell;

/// @brief Maximum grid dimension (prevents absurd allocations).
#define PF_MAX_DIM 4096

/// @brief Base movement cost for cardinal directions (fixed-point ×100).
#define PF_COST_CARDINAL 100

/// @brief Diagonal movement cost (~√2 × 100, fixed-point).
#define PF_COST_DIAGONAL 141

/// @brief Internal pathfinder structure.
typedef struct {
    pf_cell *cells;        ///< width × height cell array.
    int32_t width;         ///< Grid width.
    int32_t height;        ///< Grid height.
    int8_t allow_diagonal; ///< 0 = 4-way, 1 = 8-way.
    int32_t max_steps;     ///< Max nodes to expand (0 = unlimited).
    int32_t last_steps;    ///< Nodes expanded in last search.
    int8_t last_found;     ///< 1 if last search found a path.
} rt_pathfinder_impl;

/// @brief Immutable pathfinding operation result.
typedef struct {
    void *path;     ///< List[Seq[Integer]] waypoint path.
    int64_t steps;  ///< Nodes expanded by the search.
    int64_t cost;   ///< Weighted movement cost, or -1 when unavailable.
    int64_t length; ///< Cell-to-cell path length, or -1 when not found.
    int8_t found;   ///< 1 when a path was found.
} rt_path_result_impl;

/// @brief Convert an in-range coordinate to its row-major cell index.
/// @param pf Pathfinder supplying grid width.
/// @param x In-range X coordinate.
/// @param y In-range Y coordinate.
/// @return Flat index `y * width + x`.
static inline int32_t pf_idx(const rt_pathfinder_impl *pf, int32_t x, int32_t y) {
    return y * pf->width + x;
}

/// @brief Test signed 32-bit coordinates against the pathfinder bounds.
/// @param pf Pathfinder supplying dimensions.
/// @param x Candidate X coordinate.
/// @param y Candidate Y coordinate.
/// @return `1` when `(x,y)` is inside the grid, otherwise `0`.
static inline int8_t pf_in_bounds(const rt_pathfinder_impl *pf, int32_t x, int32_t y) {
    return x >= 0 && x < pf->width && y >= 0 && y < pf->height;
}

/// @brief Test signed 64-bit public coordinates before narrowing to int32.
/// @param pf Pathfinder supplying dimensions.
/// @param x Candidate X coordinate.
/// @param y Candidate Y coordinate.
/// @return `1` when `(x,y)` is inside the grid, otherwise `0`.
static inline int8_t pf_in_bounds_i64(const rt_pathfinder_impl *pf, int64_t x, int64_t y) {
    return x >= 0 && y >= 0 && x < pf->width && y < pf->height;
}

/// @brief Validate and cast an opaque Pathfinder handle.
/// @param ptr Candidate object; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return Pathfinder implementation when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static rt_pathfinder_impl *checked_pathfinder(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_PATHFINDER_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_pathfinder_impl *)ptr;
}

/// @brief Safe-cast a handle to PathResult, trapping on a class mismatch.
/// @param ptr Candidate PathResult object; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return PathResult implementation when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static rt_path_result_impl *checked_path_result(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_PATH_RESULT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_path_result_impl *)ptr;
}

/// @brief Release the path list owned by a PathResult during finalization.
/// @param obj PathResult object supplied by the runtime finalizer system.
/// @details The stored pointer is cleared after releasing its reference.
static void path_result_finalizer(void *obj) {
    rt_path_result_impl *result = (rt_path_result_impl *)obj;
    if (result->path && rt_obj_release_check0(result->path))
        rt_obj_free(result->path);
    result->path = NULL;
}

/// @brief Create a PathResult and take ownership of @p path.
/// @param path Path list reference transferred to the result on success.
/// @param found Nonzero when the associated operation found a path.
/// @param steps Number of nodes expanded by the operation.
/// @param cost Weighted A* movement cost when available.
/// @return New PathResult, or `NULL` on allocation failure.
/// @details The path reference is released by the PathResult finalizer. On
///          allocation failure the caller still owns @p path and must release
///          it. Failed results store cost and length as `-1`; successful length
///          is waypoint count minus one.
static void *path_result_new_take_path(void *path, int8_t found, int64_t steps, int64_t cost) {
    rt_path_result_impl *result = (rt_path_result_impl *)rt_obj_new_i64(
        RT_PATH_RESULT_CLASS_ID, (int64_t)sizeof(rt_path_result_impl));
    if (!result)
        return NULL;
    result->path = path;
    result->steps = steps;
    result->cost = found ? cost : -1;
    result->found = found ? 1 : 0;
    int64_t count = path ? rt_list_len(path) : 0;
    result->length = result->found ? (count > 0 ? count - 1 : 0) : -1;
    rt_obj_set_finalizer(result, path_result_finalizer);
    return result;
}

/// @brief Create a not-found PathResult containing a new empty path list.
/// @return New PathResult, or `NULL` if result allocation fails.
/// @details If the result allocation fails, the temporary empty list is
///          released before returning.
static void *path_result_new_empty(void) {
    void *path = rt_list_new();
    void *result = path_result_new_take_path(path, 0, 0, -1);
    if (!result && path && rt_obj_release_check0(path))
        rt_obj_free(path);
    return result;
}

/// @brief Compute a checked cell count for a rectangular pathfinder grid.
/// @details Keeps allocation sites safe even if the public dimension cap is
///          changed later. The product must fit both `int32_t` indexing and
///          `size_t` allocation math.
/// @param w Grid width in cells.
/// @param h Grid height in cells.
/// @param out_count Receives `w * h` on success.
/// @return 1 when the product is representable, 0 otherwise.
static int8_t pf_checked_cell_count(int32_t w, int32_t h, int32_t *out_count) {
    if (w <= 0 || h <= 0 || !out_count)
        return 0;
    if ((size_t)w > (size_t)INT32_MAX / (size_t)h)
        return 0;
    size_t total = (size_t)w * (size_t)h;
    if (total > (size_t)INT32_MAX)
        return 0;
    *out_count = (int32_t)total;
    return 1;
}

/// @brief Check whether an array byte count is representable in `size_t`.
/// @param count Signed element count from pathfinder indexing.
/// @param elem_size Size of one element.
/// @return `1` when `count * elem_size` cannot overflow, otherwise `0`.
static int8_t pf_allocation_count_ok(int32_t count, size_t elem_size) {
    return count >= 0 && (elem_size == 0 || (size_t)count <= SIZE_MAX / elem_size);
}

//=============================================================================
// GC Finalizer
//=============================================================================

/// @brief Release the separately allocated cell array during finalization.
/// @param obj Pathfinder object supplied by the runtime finalizer system.
/// @details The stored cell pointer is cleared after `free()`.
static void pf_finalizer(void *obj) {
    rt_pathfinder_impl *pf = (rt_pathfinder_impl *)obj;
    free(pf->cells);
    pf->cells = NULL;
}

//=============================================================================
// Construction
//=============================================================================

/// @brief Allocate and initialize a pathfinder with given dimensions.
/// @param w Requested width; nonpositive values fail and values above
///        PF_MAX_DIM are capped.
/// @param h Requested height; nonpositive values fail and values above
///        PF_MAX_DIM are capped.
/// @return New Pathfinder implementation, or `NULL` on validation or
///         allocation failure.
/// @details Every cell starts walkable with value zero and cardinal cost 100.
///          Diagonal movement is disabled and the search budget is unlimited.
static rt_pathfinder_impl *pf_alloc(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0)
        return NULL;
    if (w > PF_MAX_DIM)
        w = PF_MAX_DIM;
    if (h > PF_MAX_DIM)
        h = PF_MAX_DIM;

    rt_pathfinder_impl *pf = (rt_pathfinder_impl *)rt_obj_new_i64(
        RT_PATHFINDER_CLASS_ID, (int64_t)sizeof(rt_pathfinder_impl));
    if (!pf)
        return NULL;

    int32_t count;
    if (!pf_checked_cell_count(w, h, &count) || !pf_allocation_count_ok(count, sizeof(pf_cell))) {
        if (rt_obj_release_check0(pf))
            rt_obj_free(pf);
        return NULL;
    }
    pf->cells = (pf_cell *)malloc((size_t)count * sizeof(pf_cell));
    if (!pf->cells) {
        if (rt_obj_release_check0(pf))
            rt_obj_free(pf);
        return NULL;
    }

    pf->width = w;
    pf->height = h;
    pf->allow_diagonal = 0;
    pf->max_steps = 0;
    pf->last_steps = 0;
    pf->last_found = 0;

    // Default: all cells walkable, cost 100
    for (int32_t i = 0; i < count; i++) {
        pf->cells[i].walkable = 1;
        pf->cells[i].cost = PF_COST_CARDINAL;
        pf->cells[i].value = 0;
    }

    rt_obj_set_finalizer(pf, pf_finalizer);
    return pf;
}

/// @brief Construct an empty grid pathfinder of `width × height` cells. All cells start
/// walkable with cost 100. Configure with `_set_walkable` / `_set_cost` before pathing. The
/// algorithm is A* with octile (or 4-connected) neighborhood.
/// @param width Grid width in cells.
/// @param height Grid height in cells.
/// @return A new runtime-managed Pathfinder reference, or `NULL` when either
///         dimension is outside `[1, 4096]` or allocation fails.
void *rt_pathfinder_new(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0 || width > PF_MAX_DIM || height > PF_MAX_DIM)
        return NULL;
    return pf_alloc((int32_t)width, (int32_t)height);
}

/// @brief Release one runtime reference to a Pathfinder.
/// @param ptr Handle to release; `NULL` is a no-op.
/// @details The cell-array finalizer runs when the last reference is freed. A
///          non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_destroy(void *ptr) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.Destroy: expected Zanna.Game.Pathfinder");
    if (pf && rt_obj_release_check0(pf))
        rt_obj_free(pf);
}

/// @brief Build a pathfinder from a Tilemap — cells with collision != 0 are non-walkable.
/// @param tilemap Source Tilemap handle.
/// @return A new Pathfinder matching the tilemap dimensions, or `NULL` for a
///         null source, unsupported dimensions, or allocation failure.
/// @details This is a one-shot snapshot; later tilemap changes do not update
///          the pathfinder. Values come from the designated collision layer.
///          Each tile ID is retained as the cell value, while the tile's
///          collision type determines walkability. Costs remain at 100.
void *rt_pathfinder_from_tilemap(void *tilemap) {
    if (!tilemap)
        return NULL;

    int64_t tw = rt_tilemap_get_width(tilemap);
    int64_t th = rt_tilemap_get_height(tilemap);
    if (tw <= 0 || th <= 0 || tw > PF_MAX_DIM || th > PF_MAX_DIM)
        return NULL;
    int32_t w = (int32_t)tw;
    int32_t h = (int32_t)th;
    rt_pathfinder_impl *pf = pf_alloc(w, h);
    if (!pf)
        return NULL;

    // Read tiles from the tilemap's designated collision layer, not the base/visual
    // layer, so a map that places collision geometry on a separate layer produces a
    // navigation grid that matches its actual walls (VDOC-261). The collision layer
    // defaults to 0 (the base layer), so single-layer maps are unaffected.
    int64_t collisionLayer = rt_tilemap_get_collision_layer(tilemap);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            int64_t tile = rt_tilemap_get_tile_layer(tilemap, collisionLayer, x, y);
            int64_t collision = rt_tilemap_get_collision(tilemap, tile);
            pf_cell *cell = &pf->cells[pf_idx(pf, x, y)];
            cell->value = tile;
            cell->walkable = (collision == 0) ? 1 : 0;
        }
    }

    return pf;
}

/// @brief Build a pathfinder from a Grid2D — cells with non-zero values are non-walkable.
/// @param grid Source Grid2D handle.
/// @return A new Pathfinder matching the grid dimensions, or `NULL` for a null
///         source, unsupported dimensions, or allocation failure.
/// @details This is a one-shot snapshot. Each integer is retained as the cell
///          value for nearest-value queries; zero is walkable and nonzero is
///          blocked. Costs remain at 100.
void *rt_pathfinder_from_grid2d(void *grid) {
    if (!grid)
        return NULL;

    // Cast void* to the rt_grid2d typedef (struct rt_grid2d_impl*)
    rt_grid2d g = (rt_grid2d)grid;
    int64_t w = rt_grid2d_width(g);
    int64_t h = rt_grid2d_height(g);
    if (w <= 0 || h <= 0 || w > PF_MAX_DIM || h > PF_MAX_DIM)
        return NULL;
    rt_pathfinder_impl *pf = pf_alloc((int32_t)w, (int32_t)h);
    if (!pf)
        return NULL;

    // Non-zero cells → non-walkable
    for (int32_t y = 0; y < (int32_t)h; y++) {
        for (int32_t x = 0; x < (int32_t)w; x++) {
            int64_t val = rt_grid2d_get(g, x, y);
            pf_cell *cell = &pf->cells[pf_idx(pf, x, y)];
            cell->value = val;
            cell->walkable = (val == 0) ? 1 : 0;
        }
    }

    return pf;
}

//=============================================================================
// Configuration
//=============================================================================

/// @brief Change whether a cell may be entered by searches.
/// @param ptr Pathfinder to modify.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @param walkable Zero blocks the cell; any nonzero value makes it walkable.
/// @details Null and out-of-bounds input are no-ops. A non-null wrong-class
///          handle raises a runtime trap.
void rt_pathfinder_set_walkable(void *ptr, int64_t x, int64_t y, int8_t walkable) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.SetWalkable: expected Zanna.Game.Pathfinder");
    if (!pf)
        return;
    if (!pf_in_bounds_i64(pf, x, y))
        return;
    pf->cells[pf_idx(pf, (int32_t)x, (int32_t)y)].walkable = walkable ? 1 : 0;
}

/// @brief Test whether a cell is currently traversable.
/// @param ptr Pathfinder to query.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return `1` for a walkable in-range cell, otherwise `0`.
/// @details Null and out-of-bounds coordinates are treated as walls. A
///          non-null wrong-class handle raises a runtime trap.
int8_t rt_pathfinder_is_walkable(void *ptr, int64_t x, int64_t y) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.IsWalkable: expected Zanna.Game.Pathfinder");
    if (!pf)
        return 0;
    if (!pf_in_bounds_i64(pf, x, y))
        return 0;
    return pf->cells[pf_idx(pf, (int32_t)x, (int32_t)y)].walkable;
}

/// @brief Set per-cell traversal cost multiplier. Walkable cells are clamped to [1, 30000].
/// @param ptr Pathfinder to modify.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @param cost Requested destination-cell cost, clamped to `[1, 30000]`.
/// @details Cost 100 represents normal movement. Use SetWalkable to block a
///          cell; cost zero does not create a wall. Null and out-of-bounds
///          input are no-ops. A non-null wrong-class handle traps.
void rt_pathfinder_set_cost(void *ptr, int64_t x, int64_t y, int64_t cost) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.SetCost: expected Zanna.Game.Pathfinder");
    if (!pf)
        return;
    if (!pf_in_bounds_i64(pf, x, y))
        return;
    if (cost < 1)
        cost = 1;
    if (cost > 30000)
        cost = 30000;
    pf->cells[pf_idx(pf, (int32_t)x, (int32_t)y)].cost = (int16_t)cost;
}

/// @brief Read a cell's destination traversal-cost multiplier.
/// @param ptr Pathfinder to query.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return Stored value in `[1, 30000]`, or zero for null/out-of-bounds input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_cost(void *ptr, int64_t x, int64_t y) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.GetCost: expected Zanna.Game.Pathfinder");
    if (!pf)
        return 0;
    if (!pf_in_bounds_i64(pf, x, y))
        return 0;
    return pf->cells[pf_idx(pf, (int32_t)x, (int32_t)y)].cost;
}

/// @brief Select four-way or eight-way search neighborhoods.
/// @param ptr Pathfinder to modify; `NULL` is a no-op.
/// @param allow Zero selects cardinal movement; any nonzero value also permits
///        diagonals.
/// @details Diagonal moves cannot cut between blocked cardinal neighbors. A
///          non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_set_diagonal(void *ptr, int8_t allow) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.SetDiagonal: expected Zanna.Game.Pathfinder");
    if (!pf)
        return;
    pf->allow_diagonal = allow ? 1 : 0;
}

/// @brief Cap the search-step budget — A* gives up after `max` cell expansions and reports
/// "no path". 0 disables the cap. Useful to prevent runaway searches in pathological maps.
/// @param ptr Pathfinder to modify; `NULL` is a no-op.
/// @param max Requested expansion limit. Negative values become zero and
///        values above INT32_MAX are capped.
/// @details The same budget applies to A* and nearest-value BFS. A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfinder_set_max_steps(void *ptr, int64_t max) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.SetMaxSteps: expected Zanna.Game.Pathfinder");
    if (!pf)
        return;
    if (max < 0)
        max = 0;
    if (max > INT32_MAX)
        max = INT32_MAX;
    pf->max_steps = (int32_t)max;
}

//=============================================================================
// Properties
//=============================================================================

/// @brief Read the pathfinder grid width.
/// @param ptr Pathfinder to query.
/// @return Width in cells, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_width(void *ptr) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.Width: expected Zanna.Game.Pathfinder");
    return pf ? pf->width : 0;
}

/// @brief Read the pathfinder grid height.
/// @param ptr Pathfinder to query.
/// @return Height in cells, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_height(void *ptr) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.Height: expected Zanna.Game.Pathfinder");
    return pf ? pf->height : 0;
}

/// @brief Number of cell expansions performed by the most recent `_find_path` call. Useful
/// for performance tuning and verifying the max-steps cap.
/// @param ptr Pathfinder to query.
/// @return Last A* or nearest-value expansion count, or zero for `NULL`.
/// @details Starting any public search replaces the previous value. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_last_steps(void *ptr) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.LastSteps: expected Zanna.Game.Pathfinder");
    return pf ? pf->last_steps : 0;
}

/// @brief Test whether the most recent search produced a complete path.
/// @param ptr Pathfinder to query.
/// @return `1` after a successful A* or nearest-value search, otherwise `0`.
/// @details A payload allocation failure is reported as not found. A non-null
///          wrong-class handle raises a runtime trap.
int8_t rt_pathfinder_get_last_found(void *ptr) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.LastFound: expected Zanna.Game.Pathfinder");
    return pf ? pf->last_found : 0;
}

//=============================================================================
// PathResult API
//=============================================================================

/// @brief Test whether a PathResult represents a successful search.
/// @param ptr PathResult to query.
/// @return `1` when found, or `0` for not found or `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_path_result_found(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.Found: expected Zanna.Game.PathResult");
    return result ? result->found : 0;
}

/// @brief Read the number of nodes expanded by the captured search.
/// @param ptr PathResult to query.
/// @return Stored expansion count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_path_result_steps(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.Steps: expected Zanna.Game.PathResult");
    return result ? result->steps : 0;
}

/// @brief Read the captured weighted movement cost.
/// @param ptr PathResult to query.
/// @return Fixed-point A* cost, or `-1` when unavailable, not found, or null.
/// @details Nearest-value searches do not compute weighted cost. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_path_result_cost(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.Cost: expected Zanna.Game.PathResult");
    return result ? result->cost : -1;
}

/// @brief Read the captured number of cell-to-cell path steps.
/// @param ptr PathResult to query.
/// @return Waypoint count minus one, or `-1` when not found or null.
/// @details A start-equals-goal path has zero steps. A non-null wrong-class
///          handle raises a runtime trap.
int64_t rt_path_result_step_count(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.StepCount: expected Zanna.Game.PathResult");
    return result ? result->length : -1;
}

/// @brief Read the legacy alias for rt_path_result_step_count().
/// @param ptr PathResult to query.
/// @return Stored cell-to-cell step count, or `-1` when not found or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_path_result_length(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.Length: expected Zanna.Game.PathResult");
    return result ? result->length : -1;
}

/// @brief Obtain an independently retained reference to a result's path list.
/// @param ptr PathResult to query.
/// @return Retained `List[Seq[Integer]]`, or a new empty list for null/invalid
///         input.
/// @details The caller may retain the returned path after releasing the
///          PathResult. A non-null wrong-class handle raises a runtime trap.
void *rt_path_result_path(void *ptr) {
    rt_path_result_impl *result =
        checked_path_result(ptr, "PathResult.Path: expected Zanna.Game.PathResult");
    if (!result || !result->path)
        return rt_list_new();
    rt_obj_retain_maybe(result->path);
    return result->path;
}

//=============================================================================
// A* Search Engine
//=============================================================================

/// @brief Per-node search state (allocated per search call).
typedef struct {
    int64_t g;      ///< Cost from start to this node.
    int64_t f;      ///< g + heuristic (total estimated cost).
    int32_t parent; ///< Parent node index (-1 = start/none).
    int8_t open;    ///< 1 = in open set.
    int8_t closed;  ///< 1 = already expanded.
} pf_node;

/// @brief Binary min-heap for the open set (heapified by f-cost).
typedef struct {
    int32_t *data;     ///< Array of node indices.
    int32_t *position; ///< Node index -> heap slot, or -1 if not queued.
    int32_t count;     ///< Current number of elements.
    int32_t capacity;  ///< Allocated capacity.
    pf_node *nodes;    ///< Reference to node array (for f-cost comparison).
} pf_heap;

/// @brief Swap heap slots @p a and @p b, keeping the node->slot position
///        index consistent (the open-set is an indexed binary min-heap).
/// @param h Heap to mutate.
/// @param a First valid heap-array slot.
/// @param b Second valid heap-array slot.
static void heap_swap(pf_heap *h, int32_t a, int32_t b) {
    int32_t tmp = h->data[a];
    h->data[a] = h->data[b];
    h->data[b] = tmp;
    h->position[h->data[a]] = a;
    h->position[h->data[b]] = b;
}

/// @brief Restore the min-heap invariant upward from slot @p i after a
///        decrease-key or insertion (orders by node f-score).
/// @param h Heap to repair.
/// @param i Valid starting heap-array slot.
static void heap_sift_up(pf_heap *h, int32_t i) {
    while (i > 0) {
        int32_t parent = (i - 1) / 2;
        if (h->nodes[h->data[i]].f < h->nodes[h->data[parent]].f) {
            heap_swap(h, i, parent);
            i = parent;
        } else
            break;
    }
}

/// @brief Restore the min-heap invariant downward from slot @p i after a
///        pop (root replaced by the last element).
/// @param h Heap to repair.
/// @param i Valid starting heap-array slot.
static void heap_sift_down(pf_heap *h, int32_t i) {
    while (1) {
        int32_t left = 2 * i + 1;
        int32_t right = 2 * i + 2;
        int32_t smallest = i;

        if (left < h->count && h->nodes[h->data[left]].f < h->nodes[h->data[smallest]].f)
            smallest = left;
        if (right < h->count && h->nodes[h->data[right]].f < h->nodes[h->data[smallest]].f)
            smallest = right;

        if (smallest != i) {
            heap_swap(h, i, smallest);
            i = smallest;
        } else
            break;
    }
}

/// @brief Insert @p node_idx into the open set, or decrease-key it if already
///        present (re-sifts from its current slot). No-op at capacity.
/// @param h Indexed heap to mutate.
/// @param node_idx Nonnegative node-array index.
/// @details Existing nodes are sifted upward after a decreased f-score. New
///          nodes are appended and sifted. Invalid indices and full heaps are
///          silently ignored.
static void heap_push(pf_heap *h, int32_t node_idx) {
    if (node_idx < 0)
        return;
    if (h->position[node_idx] >= 0) {
        heap_sift_up(h, h->position[node_idx]);
        return;
    }
    if (h->count >= h->capacity)
        return; // Should never happen with correct pre-allocation
    h->data[h->count] = node_idx;
    h->position[node_idx] = h->count;
    heap_sift_up(h, h->count);
    h->count++;
}

/// @brief Remove and return the lowest-f node index, or -1 if the open set is
///        empty (re-heapifies the remaining elements).
/// @param h Indexed heap to mutate.
/// @return Removed node index, or `-1` when empty.
static int32_t heap_pop(pf_heap *h) {
    if (h->count == 0)
        return -1;
    int32_t top = h->data[0];
    h->position[top] = -1;
    h->count--;
    if (h->count > 0) {
        h->data[0] = h->data[h->count];
        h->position[h->data[0]] = 0;
        heap_sift_down(h, 0);
    }
    return top;
}

/// @brief Compute an unweighted fixed-point grid-distance heuristic.
/// @param ax Source X.
/// @param ay Source Y.
/// @param bx Goal X.
/// @param by Goal Y.
/// @param diagonal Nonzero selects octile distance; zero selects Manhattan.
/// @return Heuristic cost using cardinal unit 100 and diagonal unit 141.
static int64_t pf_heuristic(int32_t ax, int32_t ay, int32_t bx, int32_t by, int8_t diagonal) {
    int32_t dx = ax > bx ? ax - bx : bx - ax;
    int32_t dy = ay > by ? ay - by : by - ay;

    if (diagonal) {
        // Octile distance: max(dx,dy)*100 + (√2-1)*min(dx,dy)*100
        // ≈ max*100 + min*41
        int64_t mn = dx < dy ? dx : dy;
        int64_t mx = dx > dy ? dx : dy;
        return mx * PF_COST_CARDINAL + mn * (PF_COST_DIAGONAL - PF_COST_CARDINAL);
    } else {
        // Manhattan distance
        return ((int64_t)dx + (int64_t)dy) * PF_COST_CARDINAL;
    }
}

/// @brief Heuristic scaled by the grid's minimum per-cell cost so it stays
///        admissible (never overestimates) when tiles have custom costs.
/// @param ax Source X.
/// @param ay Source Y.
/// @param bx Goal X.
/// @param by Goal Y.
/// @param diagonal Nonzero selects octile distance; zero selects Manhattan.
/// @param min_cost Minimum positive traversal cost among walkable cells,
///        clamped internally to at least one.
/// @return Admissible weighted fixed-point heuristic.
static int64_t pf_scaled_heuristic(
    int32_t ax, int32_t ay, int32_t bx, int32_t by, int8_t diagonal, int64_t min_cost) {
    if (min_cost < 1)
        min_cost = 1;
    return pf_heuristic(ax, ay, bx, by, diagonal) * min_cost / PF_COST_CARDINAL;
}

/// @brief 4-way direction offsets.
static const int32_t dir4_dx[4] = {0, 1, 0, -1};
static const int32_t dir4_dy[4] = {-1, 0, 1, 0};

/// @brief 8-way direction offsets (4 cardinal + 4 diagonal).
static const int32_t dir8_dx[8] = {0, 1, 0, -1, 1, 1, -1, -1};
static const int32_t dir8_dy[8] = {-1, 0, 1, 0, -1, 1, 1, -1};

/// @brief Reconstruct the path as List[Seq[Integer]], or NULL on any failure.
/// @param nodes Completed search-state array containing parent links.
/// @param goal_idx Flat index at which reconstruction begins.
/// @param width Grid width used to recover X/Y coordinates.
/// @return New start-to-goal `List[Seq[Integer]]`, or `NULL` on allocation or
///         size-validation failure.
/// @details Transactional: an allocation failure for the coordinate buffer, a
///          waypoint Seq, or a coordinate box drops all partial work and returns
///          NULL instead of a silently empty or shortened list, so the caller
///          never reports a truncated path as a found success (VDOC-263).
static void *pf_build_path(pf_node *nodes, int32_t goal_idx, int32_t width) {
    // Count path length
    int32_t len = 0;
    int32_t idx = goal_idx;
    while (idx >= 0) {
        len++;
        idx = nodes[idx].parent;
    }

    // Build reversed array
    if (len < 0 || len > INT32_MAX / 2 || (size_t)len > SIZE_MAX / (2u * sizeof(int32_t)))
        return NULL;
    size_t coord_values = (size_t)len * 2u;
    int32_t *coords = (int32_t *)malloc(coord_values * sizeof(int32_t));
    if (!coords)
        return NULL;

    idx = goal_idx;
    for (int32_t i = len - 1; i >= 0; i--) {
        int32_t x = idx % width;
        int32_t y = idx / width;
        coords[i * 2] = x;
        coords[i * 2 + 1] = y;
        idx = nodes[idx].parent;
    }

    // Build List[Seq[Integer]], where each entry is [x, y]. Any allocation failure
    // aborts the whole build so a partial path is never returned.
    void *list = rt_list_new();
    if (!list) {
        free(coords);
        return NULL;
    }
    int ok = 1;
    for (int32_t i = 0; i < len; i++) {
        void *pair = rt_seq_new();
        if (!pair) {
            ok = 0;
            break;
        }
        rt_seq_set_owns_elements(pair, 1);

        void *bx = rt_box_i64(coords[i * 2]);
        void *by = rt_box_i64(coords[i * 2 + 1]);
        if (!bx || !by) {
            if (bx && rt_obj_release_check0(bx))
                rt_obj_free(bx);
            if (by && rt_obj_release_check0(by))
                rt_obj_free(by);
            if (rt_obj_release_check0(pair))
                rt_obj_free(pair);
            ok = 0;
            break;
        }

        rt_seq_push(pair, bx);
        if (rt_obj_release_check0(bx))
            rt_obj_free(bx);
        rt_seq_push(pair, by);
        if (rt_obj_release_check0(by))
            rt_obj_free(by);

        rt_list_push(list, pair);
        if (rt_obj_release_check0(pair))
            rt_obj_free(pair);
    }

    free(coords);
    if (!ok) {
        // Drop the partially built list so a shortened path is never observed.
        if (rt_obj_release_check0(list))
            rt_obj_free(list);
        return NULL;
    }
    return list;
}

/// @brief Run the core weighted A* search.
/// @param pf Pathfinder and mutable last-search state.
/// @param sx In-range start X.
/// @param sy In-range start Y.
/// @param gx In-range goal X.
/// @param gy In-range goal Y.
/// @param cost_only Nonzero suppresses path construction.
/// @param out_cost Optional destination initialized to `-1` and set to the
///        fixed-point movement cost on success.
/// @return In normal mode, a new start-to-goal path or an empty list on
///         failure; in cost-only mode, always `NULL`.
/// @details The search resets last-found and last-steps. It uses destination
///          cell weights, saturating score addition, and Manhattan or octile
///          heuristics scaled by the minimum walkable cost. Diagonals cannot
///          cut blocked corners. Search-array allocation or path construction
///          failure is reported as not found.
static void *pf_astar(rt_pathfinder_impl *pf,
                      int32_t sx,
                      int32_t sy,
                      int32_t gx,
                      int32_t gy,
                      int8_t cost_only,
                      int64_t *out_cost) {
    pf->last_found = 0;
    pf->last_steps = 0;
    if (out_cost)
        *out_cost = -1;

    // Bounds check
    if (!pf_in_bounds(pf, sx, sy) || !pf_in_bounds(pf, gx, gy))
        return cost_only ? NULL : rt_list_new();

    // Start or goal not walkable
    int32_t si = pf_idx(pf, sx, sy);
    int32_t gi = pf_idx(pf, gx, gy);
    if (!pf->cells[si].walkable || !pf->cells[gi].walkable)
        return cost_only ? NULL : rt_list_new();

    // Start == goal
    if (sx == gx && sy == gy) {
        pf->last_found = 1;
        if (out_cost)
            *out_cost = 0;
        if (cost_only)
            return NULL;
        void *list = rt_list_new();
        void *pair = rt_seq_new();
        if (!pair)
            return list;
        rt_seq_set_owns_elements(pair, 1);
        void *bx = rt_box_i64(sx);
        rt_seq_push(pair, bx);
        if (bx && rt_obj_release_check0(bx))
            rt_obj_free(bx);
        void *by = rt_box_i64(sy);
        rt_seq_push(pair, by);
        if (by && rt_obj_release_check0(by))
            rt_obj_free(by);
        rt_list_push(list, pair);
        if (pair && rt_obj_release_check0(pair))
            rt_obj_free(pair);
        return list;
    }

    int32_t total;
    if (!pf_checked_cell_count(pf->width, pf->height, &total))
        return cost_only ? NULL : rt_list_new();
    int64_t min_cost = PF_COST_CARDINAL;
    for (int32_t i = 0; i < total; i++) {
        if (pf->cells[i].walkable && pf->cells[i].cost > 0 && pf->cells[i].cost < min_cost)
            min_cost = pf->cells[i].cost;
    }

    // Allocate per-search arrays
    if (!pf_allocation_count_ok(total, sizeof(pf_node)))
        return cost_only ? NULL : rt_list_new();
    pf_node *nodes = (pf_node *)calloc((size_t)total, sizeof(pf_node));
    if (!nodes)
        return cost_only ? NULL : rt_list_new();

    // Initialize all nodes
    for (int32_t i = 0; i < total; i++) {
        nodes[i].g = INT64_MAX / 4;
        nodes[i].f = INT64_MAX / 4;
        nodes[i].parent = -1;
        nodes[i].open = 0;
        nodes[i].closed = 0;
    }

    // Heap
    pf_heap heap;
    if (!pf_allocation_count_ok(total, sizeof(int32_t))) {
        free(nodes);
        return cost_only ? NULL : rt_list_new();
    }
    heap.data = (int32_t *)malloc((size_t)total * sizeof(int32_t));
    heap.position = (int32_t *)malloc((size_t)total * sizeof(int32_t));
    heap.count = 0;
    heap.capacity = total;
    heap.nodes = nodes;

    if (!heap.data || !heap.position) {
        free(heap.position);
        free(heap.data);
        free(nodes);
        return cost_only ? NULL : rt_list_new();
    }
    for (int32_t i = 0; i < total; ++i)
        heap.position[i] = -1;

    // Initialize start node
    nodes[si].g = 0;
    nodes[si].f = pf_scaled_heuristic(sx, sy, gx, gy, pf->allow_diagonal, min_cost);
    nodes[si].open = 1;
    heap_push(&heap, si);

    int32_t dir_count = pf->allow_diagonal ? 8 : 4;
    const int32_t *dx = pf->allow_diagonal ? dir8_dx : dir4_dx;
    const int32_t *dy = pf->allow_diagonal ? dir8_dy : dir4_dy;
    int32_t max = pf->max_steps > 0 ? pf->max_steps : INT32_MAX;
    int32_t steps = 0;

    void *result = NULL;

    while (heap.count > 0 && steps < max) {
        int32_t cur = heap_pop(&heap);
        if (cur < 0)
            break;

        nodes[cur].open = 0;
        nodes[cur].closed = 1;
        steps++;

        // Goal reached
        if (cur == gi) {
            pf->last_steps = steps;
            if (out_cost)
                *out_cost = nodes[cur].g;
            if (cost_only) {
                // No payload to build, so reaching the goal is a definitive success.
                pf->last_found = 1;
            } else {
                result = pf_build_path(nodes, gi, pf->width);
                // Report success only once the payload is fully built: a failed
                // (NULL) build is an allocation failure, not a found path, so it
                // must not masquerade as a truncated success (VDOC-263).
                pf->last_found = result ? 1 : 0;
            }
            free(heap.position);
            free(heap.data);
            free(nodes);
            return result ? result : (cost_only ? NULL : rt_list_new());
        }

        int32_t cx = cur % pf->width;
        int32_t cy = cur / pf->width;

        for (int32_t d = 0; d < dir_count; d++) {
            int32_t nx = cx + dx[d];
            int32_t ny = cy + dy[d];

            if (!pf_in_bounds(pf, nx, ny))
                continue;

            int32_t ni = pf_idx(pf, nx, ny);
            if (nodes[ni].closed || !pf->cells[ni].walkable)
                continue;

            // For diagonal moves, check that both cardinal neighbors are walkable
            // (prevents cutting through wall corners)
            if (d >= 4) {
                if (!pf_in_bounds(pf, cx + dx[d], cy))
                    continue;
                int32_t adj1 = pf_idx(pf, cx + dx[d], cy);
                if (!pf->cells[adj1].walkable)
                    continue;
                if (!pf_in_bounds(pf, cx, cy + dy[d]))
                    continue;
                int32_t adj2 = pf_idx(pf, cx, cy + dy[d]);
                if (!pf->cells[adj2].walkable)
                    continue;
            }

            // Movement cost: base cost × cell cost / 100
            int64_t base_cost = (d < 4) ? PF_COST_CARDINAL : PF_COST_DIAGONAL;
            int64_t move_cost = base_cost * pf->cells[ni].cost / PF_COST_CARDINAL;
            int64_t tentative_g =
                nodes[cur].g > INT64_MAX - move_cost ? INT64_MAX : nodes[cur].g + move_cost;

            if (tentative_g < nodes[ni].g) {
                nodes[ni].g = tentative_g;
                int64_t heuristic =
                    pf_scaled_heuristic(nx, ny, gx, gy, pf->allow_diagonal, min_cost);
                nodes[ni].f =
                    tentative_g > INT64_MAX - heuristic ? INT64_MAX : tentative_g + heuristic;
                nodes[ni].parent = cur;

                if (!nodes[ni].open) {
                    nodes[ni].open = 1;
                    heap_push(&heap, ni);
                } else {
                    heap_push(&heap, ni);
                }
            }
        }
    }

    // No path found
    pf->last_steps = steps;
    free(heap.position);
    free(heap.data);
    free(nodes);
    return cost_only ? NULL : rt_list_new();
}

//=============================================================================
// Public Pathfinding API
//=============================================================================

/// @brief Compute the shortest path from (sx, sy) to (gx, gy) using A* with the configured
/// neighborhood + costs. Returns a List of (x, y) cell pairs (each entry is a 2-int Seq).
/// Empty list if no path exists or `max_steps` was hit before reaching the goal.
/// @param ptr Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return New start-to-goal `List[Seq[Integer]]`, including both endpoints,
///         or a new empty list when no complete path is available.
/// @details The call replaces LastFound and LastSteps. Null/out-of-bounds
///          input, blocked endpoints, budget exhaustion, allocation failure,
///          and unreachable goals yield an empty list. A non-null wrong-class
///          handle raises a runtime trap.
void *rt_pathfinder_find_path(void *ptr, int64_t sx, int64_t sy, int64_t gx, int64_t gy) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.FindPath: expected Zanna.Game.Pathfinder");
    if (!pf)
        return rt_list_new();
    if (!pf_in_bounds_i64(pf, sx, sy) || !pf_in_bounds_i64(pf, gx, gy)) {
        pf->last_found = 0;
        pf->last_steps = 0;
        return rt_list_new();
    }
    return pf_astar(pf, (int32_t)sx, (int32_t)sy, (int32_t)gx, (int32_t)gy, 0, NULL);
}

/// @brief Run A* and capture path, found state, steps, length, and cost.
/// @param ptr Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return New PathResult, or `NULL` if result allocation fails.
/// @details Search semantics match rt_pathfinder_find_path(). A null
///          Pathfinder produces a not-found result with an empty path. The
///          result owns its path independently of later searches. A non-null
///          wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_path_result(void *ptr, int64_t sx, int64_t sy, int64_t gx, int64_t gy) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.FindPathResult: expected Zanna.Game.Pathfinder");
    if (!pf)
        return path_result_new_empty();
    int64_t cost = -1;
    void *path = NULL;
    if (!pf_in_bounds_i64(pf, sx, sy) || !pf_in_bounds_i64(pf, gx, gy)) {
        pf->last_found = 0;
        pf->last_steps = 0;
        path = rt_list_new();
    } else {
        path = pf_astar(pf, (int32_t)sx, (int32_t)sy, (int32_t)gx, (int32_t)gy, 0, &cost);
    }
    if (!path)
        path = rt_list_new();
    void *result = path_result_new_take_path(path, pf->last_found, pf->last_steps, cost);
    if (!result && path && rt_obj_release_check0(path))
        rt_obj_free(path);
    return result;
}

/// @brief Compute just the path length (number of cell-to-cell steps) from start to goal.
/// -1 if no path exists.
/// @param ptr Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return Waypoint count minus one, zero when start equals goal, or `-1` when
///         no complete path is available.
/// @details The implementation constructs and releases the same path payload
///          as rt_pathfinder_find_path(), and replaces LastFound/LastSteps.
///          A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_find_path_length(void *ptr, int64_t sx, int64_t sy, int64_t gx, int64_t gy) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.FindPathLength: expected Zanna.Game.Pathfinder");
    if (!pf)
        return -1;
    if (!pf_in_bounds_i64(pf, sx, sy) || !pf_in_bounds_i64(pf, gx, gy)) {
        pf->last_found = 0;
        pf->last_steps = 0;
        return -1;
    }
    void *path = pf_astar(pf, (int32_t)sx, (int32_t)sy, (int32_t)gx, (int32_t)gy, 0, NULL);
    if (!path)
        return -1;
    int64_t count = rt_list_len(path);
    if (rt_obj_release_check0(path))
        rt_obj_free(path);
    return pf->last_found ? (count > 0 ? count - 1 : 0) : -1;
}

/// @brief BFS-style search: walk outward from (sx, sy) and return the path to the closest
/// reachable cell whose stored value matches `target_value`. Useful for "find nearest
/// resource / enemy / waypoint marker" workflows. Empty list if none found within `max_steps`.
/// @param ptr Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param target_value Tile/Grid2D value copied when the pathfinder was built.
/// @return New start-to-target `List[Seq[Integer]]`, or a new empty list when
///         no complete match path is available.
/// @details Breadth-first order minimizes unweighted edge count and ignores
///          per-cell costs. It respects the configured four/eight-way
///          neighborhood, corner blocking, walkability, and max-step budget.
///          The start cell itself may match. The call replaces
///          LastFound/LastSteps. A non-null wrong-class handle traps.
void *rt_pathfinder_find_nearest(void *ptr, int64_t sx, int64_t sy, int64_t target_value) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.FindNearest: expected Zanna.Game.Pathfinder");
    if (!pf)
        return rt_list_new();
    pf->last_found = 0;
    pf->last_steps = 0;
    if (!pf_in_bounds_i64(pf, sx, sy))
        return rt_list_new();

    int32_t start = pf_idx(pf, (int32_t)sx, (int32_t)sy);
    if (!pf->cells[start].walkable)
        return rt_list_new();

    int32_t total;
    if (!pf_checked_cell_count(pf->width, pf->height, &total))
        return rt_list_new();
    if (!pf_allocation_count_ok(total, sizeof(pf_node)) ||
        !pf_allocation_count_ok(total, sizeof(int32_t)))
        return rt_list_new();
    pf_node *nodes = (pf_node *)calloc((size_t)total, sizeof(pf_node));
    int32_t *queue = (int32_t *)malloc((size_t)total * sizeof(int32_t));
    if (!nodes || !queue) {
        free(queue);
        free(nodes);
        return rt_list_new();
    }
    for (int32_t i = 0; i < total; ++i)
        nodes[i].parent = -1;

    int32_t head = 0;
    int32_t tail = 0;
    int32_t steps = 0;
    int32_t max = pf->max_steps > 0 ? pf->max_steps : INT32_MAX;
    int32_t found = -1;
    nodes[start].closed = 1;
    queue[tail++] = start;

    int32_t dir_count = pf->allow_diagonal ? 8 : 4;
    const int32_t *dx = pf->allow_diagonal ? dir8_dx : dir4_dx;
    const int32_t *dy = pf->allow_diagonal ? dir8_dy : dir4_dy;

    while (head < tail && steps < max) {
        int32_t cur = queue[head++];
        steps++;
        if (pf->cells[cur].value == target_value) {
            found = cur;
            break;
        }

        int32_t cx = cur % pf->width;
        int32_t cy = cur / pf->width;
        for (int32_t d = 0; d < dir_count; ++d) {
            int32_t nx = cx + dx[d];
            int32_t ny = cy + dy[d];
            if (!pf_in_bounds(pf, nx, ny))
                continue;
            int32_t ni = pf_idx(pf, nx, ny);
            if (nodes[ni].closed || !pf->cells[ni].walkable)
                continue;
            if (d >= 4) {
                if (!pf_in_bounds(pf, cx + dx[d], cy) ||
                    !pf->cells[pf_idx(pf, cx + dx[d], cy)].walkable)
                    continue;
                if (!pf_in_bounds(pf, cx, cy + dy[d]) ||
                    !pf->cells[pf_idx(pf, cx, cy + dy[d])].walkable)
                    continue;
            }
            nodes[ni].closed = 1;
            nodes[ni].parent = cur;
            queue[tail++] = ni;
        }
    }

    pf->last_steps = steps;
    void *result = NULL;
    if (found >= 0) {
        pf->last_found = 1;
        result = pf_build_path(nodes, found, pf->width);
    }
    free(queue);
    free(nodes);
    return result ? result : rt_list_new();
}

/// @brief Run nearest-value BFS and capture its immutable operation result.
/// @param ptr Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param target_value Source tile/grid value to find.
/// @return New PathResult, or `NULL` if result allocation fails.
/// @details Search semantics match rt_pathfinder_find_nearest(). Weighted cost
///          is stored as `-1` because BFS does not compute it. A null
///          Pathfinder produces a not-found result with an empty path. A
///          non-null wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_nearest_result(void *ptr, int64_t sx, int64_t sy, int64_t target_value) {
    rt_pathfinder_impl *pf =
        checked_pathfinder(ptr, "Pathfinder.FindNearestResult: expected Zanna.Game.Pathfinder");
    if (!pf)
        return path_result_new_empty();
    void *path = rt_pathfinder_find_nearest(ptr, sx, sy, target_value);
    if (!path)
        path = rt_list_new();
    void *result = path_result_new_take_path(path, pf->last_found, pf->last_steps, -1);
    if (!result && path && rt_obj_release_check0(path))
        rt_obj_free(path);
    return result;
}
