//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_pathfinder.h
/// @file
/// @brief Declares fixed-grid A* pathfinding, nearest-value search, and
///        immutable PathResult queries.
//
// Purpose: A* grid pathfinding for 2D games. Supports 4-way and 8-way movement,
//   per-cell movement costs, and integration with Tilemap and Grid2D.
//
// Key invariants:
//   - Grid cells are indexed [0, width) × [0, height).
//   - Walkability and cost are per-cell. Cost=100 is normal (1× multiplier).
//   - Walkable costs are clamped to [1, 30000]; use walkability for walls.
//   - FindPath returns a List[Seq[Integer]] of x,y waypoint pairs (start to goal).
//   - FindPath returns an empty list if no path exists.
//   - Max steps prevents pathological searches from freezing the game.
//
// Ownership/Lifetime:
//   - Pathfinder and PathResult values are runtime reference-counted objects.
//   - The Pathfinder finalizer owns its cell array; PathResult owns its path.
//   - Returned path lists are new or retained references owned by the caller.
//
// Links: src/runtime/game/rt_pathfinder.c (implementation),
//        src/runtime/graphics/rt_tilemap.h (Tilemap integration),
//        src/runtime/game/rt_grid2d.c (Grid2D integration)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate Pathfinder handles.
#define RT_PATHFINDER_CLASS_ID INT64_C(-0x510207)

/// @brief Runtime class ID used to validate immutable PathResult handles.
#define RT_PATH_RESULT_CLASS_ID INT64_C(-0x51021C)

//=========================================================================
// Construction
//=========================================================================

/// @brief Create a blank fixed-size pathfinding grid.
/// @param width Width in cells, required in `[1, 4096]`.
/// @param height Height in cells, required in `[1, 4096]`.
/// @return A new runtime-managed Pathfinder reference, or `NULL` for invalid
///         dimensions or allocation failure.
/// @details Every cell starts walkable with value zero and normal cost 100.
///          Diagonals are disabled and the search-step budget is unlimited.
void *rt_pathfinder_new(int64_t width, int64_t height);

/// @brief Snapshot a Tilemap's collision layer into a pathfinding grid.
/// @param tilemap Source Tilemap handle.
/// @return A new Pathfinder, or `NULL` for a null source, unsupported
///         dimensions, or allocation failure.
/// @details Tile IDs become stored cell values. Tiles whose collision type is
///          nonzero become blocked; all costs remain 100. Later Tilemap
///          changes do not update the Pathfinder.
void *rt_pathfinder_from_tilemap(void *tilemap);

/// @brief Snapshot a Grid2D into pathfinding values and walkability.
/// @param grid Source Grid2D handle.
/// @return A new Pathfinder, or `NULL` for a null source, unsupported
///         dimensions, or allocation failure.
/// @details Grid values are retained for nearest-value queries. Zero cells are
///          walkable, nonzero cells are blocked, and costs remain 100. Later
///          Grid2D changes do not update the Pathfinder.
void *rt_pathfinder_from_grid2d(void *grid);

/// @brief Release one runtime reference to a Pathfinder.
/// @param pf Handle to release; `NULL` is a no-op.
/// @details The cell array is reclaimed when the final reference is freed. A
///          non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_destroy(void *pf);

//=========================================================================
// Configuration
//=========================================================================

/// @brief Set whether a cell may be entered by searches.
/// @param pf Pathfinder to modify.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @param walkable Zero blocks the cell; any nonzero value makes it walkable.
/// @details Null and out-of-bounds input are no-ops. A non-null wrong-class
///          handle raises a runtime trap.
void rt_pathfinder_set_walkable(void *pf, int64_t x, int64_t y, int8_t walkable);

/// @brief Test whether a cell is traversable.
/// @param pf Pathfinder to query.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return `1` for a walkable in-range cell, otherwise `0`.
/// @details Null and out-of-bounds input are treated as blocked. A non-null
///          wrong-class handle raises a runtime trap.
int8_t rt_pathfinder_is_walkable(void *pf, int64_t x, int64_t y);

/// @brief Set the weighted cost of entering a cell.
/// @param pf Pathfinder to modify.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @param cost Requested cost clamped to `[1, 30000]`; 100 is normal.
/// @details Cost does not control walkability. Null and out-of-bounds input are
///          no-ops. A non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_set_cost(void *pf, int64_t x, int64_t y, int64_t cost);

/// @brief Read the weighted cost of entering a cell.
/// @param pf Pathfinder to query.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return Stored value in `[1, 30000]`, or zero for null/out-of-bounds input.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_cost(void *pf, int64_t x, int64_t y);

/// @brief Select four-way or eight-way search movement.
/// @param pf Pathfinder to modify; `NULL` is a no-op.
/// @param allow Zero permits cardinal moves only; any nonzero value also
///        permits diagonal moves.
/// @details Diagonals cannot cut between blocked cardinal neighbors. A
///          non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_set_diagonal(void *pf, int8_t allow);

/// @brief Set the maximum node expansions allowed per search.
/// @param pf Pathfinder to modify; `NULL` is a no-op.
/// @param max Requested cap. Negative values become zero (unlimited); values
///        above INT32_MAX are capped.
/// @details The budget applies to A* and nearest-value BFS. Exhaustion reports
///          no path. A non-null wrong-class handle raises a runtime trap.
void rt_pathfinder_set_max_steps(void *pf, int64_t max);

//=========================================================================
// Properties
//=========================================================================

/// @brief Read the grid width.
/// @param pf Pathfinder to query.
/// @return Width in cells, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_width(void *pf);

/// @brief Read the grid height.
/// @param pf Pathfinder to query.
/// @return Height in cells, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfinder_get_height(void *pf);

/// @brief Read the most recent search's node-expansion count.
/// @param pf Pathfinder to query.
/// @return Last A* or nearest-value expansion count, or zero for `NULL`.
/// @details Each public search replaces this value. A non-null wrong-class
///          handle raises a runtime trap.
int64_t rt_pathfinder_get_last_steps(void *pf);

/// @brief Test whether the most recent search produced a complete path.
/// @param pf Pathfinder to query.
/// @return `1` after success, otherwise `0`.
/// @details Payload allocation failure is reported as not found. A non-null
///          wrong-class handle raises a runtime trap.
int8_t rt_pathfinder_get_last_found(void *pf);

//=========================================================================
// PathResult
//=========================================================================

/// @brief Query whether a Zanna.Game.PathResult contains a path.
/// @param result PathResult to query.
/// @return `1` when found, or `0` when not found or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_path_result_found(void *result);

/// @brief Read the search expansion count stored in a PathResult.
/// @param result PathResult to query.
/// @return Number of expanded nodes, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_path_result_steps(void *result);

/// @brief Read the path movement cost stored in a PathResult.
/// @param result PathResult to query.
/// @return Fixed-point A* movement cost, or `-1` when unavailable, not found,
///         or null.
/// @details Cardinal movement has base cost 100 and diagonal movement 141,
///          multiplied by destination-cell weights. Nearest-value searches do
///          not compute weighted cost. A non-null wrong-class handle traps.
int64_t rt_path_result_cost(void *result);

/// @brief Read the cell-to-cell step count stored in a PathResult.
/// @param result PathResult to query.
/// @return Waypoint count minus one, or `-1` when not found or null.
/// @details Start equals goal produces zero. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_path_result_step_count(void *result);

/// @brief Read the legacy Length alias from a PathResult.
/// @param result PathResult to query.
/// @return Cell-to-cell step count, or `-1` when not found or null.
/// @details This is a compatibility alias for rt_path_result_step_count().
///          New code should use StepCount to avoid confusion with waypoint
///          count or weighted cost. A non-null wrong-class handle traps.
int64_t rt_path_result_length(void *result);

/// @brief Return a retained path list from a PathResult.
/// @param result PathResult to query.
/// @return Retained `List[Seq[Integer]]`, or a new empty list for null/invalid
///         input.
/// @details Waypoints are ordered start to goal. The caller owns the returned
///          reference and may keep it after releasing the PathResult. A
///          non-null wrong-class handle raises a runtime trap.
void *rt_path_result_path(void *result);

//=========================================================================
// Pathfinding
//=========================================================================

/// @brief Find a lowest-weight path between two cells using A*.
/// @param pf Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return New start-to-goal `List[Seq[Integer]]` including both endpoints, or
///         a new empty list when no complete path is available.
/// @details The search uses configured costs, movement neighborhood, corner
///          blocking, and step budget. Null/out-of-bounds input, blocked
///          endpoints, unreachable goals, budget exhaustion, and allocation
///          failure yield an empty list. LastFound and LastSteps are replaced.
///          A non-null wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_path(void *pf, int64_t sx, int64_t sy, int64_t gx, int64_t gy);

/// @brief Find a path and return a composable PathResult snapshot.
/// @param pf Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return New PathResult, or `NULL` if result allocation fails.
/// @details Search behavior matches rt_pathfinder_find_path(), but found state,
///          expansion count, cell-to-cell length, weighted cost, and owned path
///          are captured together. A null Pathfinder produces a not-found
///          result. A non-null wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_path_result(void *pf, int64_t sx, int64_t sy, int64_t gx, int64_t gy);

/// @brief Compute the cell-to-cell length of an A* path.
/// @param pf Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param gx Goal cell X.
/// @param gy Goal cell Y.
/// @return Waypoint count minus one, zero when start equals goal, or `-1` when
///         no complete path is available.
/// @details The implementation builds and releases the normal path payload and
///          replaces LastFound and LastSteps. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_pathfinder_find_path_length(void *pf, int64_t sx, int64_t sy, int64_t gx, int64_t gy);

/// @brief Find nearest reachable cell with the given source tile/grid value.
/// @param pf Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param target_value Tile/Grid2D value captured when the grid was built.
/// @return New start-to-target `List[Seq[Integer]]`, or a new empty list when
///         no complete match path is available.
/// @details Breadth-first order minimizes unweighted edge count and ignores
///          cell costs. The search respects walkability, four/eight-way
///          movement, corner blocking, and max-step budget; the start cell may
///          match. LastFound and LastSteps are replaced. A non-null
///          wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_nearest(void *pf, int64_t sx, int64_t sy, int64_t target_value);

/// @brief Find the nearest target value and return a PathResult snapshot.
/// @param pf Pathfinder to search.
/// @param sx Start cell X.
/// @param sy Start cell Y.
/// @param target_value Source tile/grid value to find.
/// @return New PathResult, or `NULL` if result allocation fails.
/// @details Search behavior matches rt_pathfinder_find_nearest(). Weighted cost
///          is `-1` because breadth-first nearest search does not compute it. A
///          null Pathfinder produces a not-found result. A non-null
///          wrong-class handle raises a runtime trap.
void *rt_pathfinder_find_nearest_result(void *pf, int64_t sx, int64_t sy, int64_t target_value);

#ifdef __cplusplus
}
#endif
