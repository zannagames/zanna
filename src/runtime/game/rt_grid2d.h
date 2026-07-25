//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_grid2d.h
/// @file
/// @brief Declares a fixed-dimension, row-major integer Grid2D.
//
// Purpose: 2D grid container for integer values with fixed dimensions, providing element access,
// fill, copy, and region operations for tile maps and pixel buffers.
//
// Key invariants:
//   - Grid dimensions are fixed at creation time and cannot be changed.
//   - Out-of-bounds accesses return 0 or are silently ignored rather than trapping.
//   - Elements are stored in row-major order.
//   - Coordinates are 0-based (row 0, col 0 is the top-left corner).
//
// Ownership/Lifetime:
//   - A new grid owns one runtime-object reference. Release it with
//     rt_grid2d_destroy; the cell array is freed by the object finalizer.
//
// Links: src/runtime/game/rt_grid2d.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate opaque Grid2D handles.
#define RT_GRID2D_CLASS_ID INT64_C(-0x51020B)

/// @brief Maximum Grid2D width or height, bounding dense allocation.
#define RT_GRID2D_MAX_DIM 4096

/// @brief Opaque handle to a mutable fixed-size Grid2D instance.
typedef struct rt_grid2d_impl *rt_grid2d;

/// @brief Create a new Grid2D with the specified dimensions.
/// @param width The width of the grid (number of columns).
/// @param height The height of the grid (number of rows).
/// @param default_value The initial value for all cells.
/// @return A new Grid2D instance, or NULL on invalid dimensions, dimensions above
///         RT_GRID2D_MAX_DIM, allocation failure, or size overflow.
/// @details Every cell is initialized to @p default_value. The returned handle
///          owns one runtime-object reference.
rt_grid2d rt_grid2d_new(int64_t width, int64_t height, int64_t default_value);

/// @brief Release one Grid2D reference.
/// @param grid Grid to release; `NULL` is a no-op.
/// @details Frees the object/cell array when the reference count reaches zero.
///          A non-null invalid handle raises a runtime trap.
void rt_grid2d_destroy(rt_grid2d grid);

/// @brief Get the value at the specified coordinates.
/// @param grid The grid.
/// @param x The x coordinate (column).
/// @param y The y coordinate (row).
/// @return The value at (x, y), or 0 for null/invalid/out-of-bounds access.
/// @details Coordinate misses do not trap. A non-null wrong-class handle does.
int64_t rt_grid2d_get(rt_grid2d grid, int64_t x, int64_t y);

/// @brief Set the value at the specified coordinates.
/// @param grid The grid.
/// @param x The x coordinate (column).
/// @param y The y coordinate (row).
/// @param value The value to set.
/// @details Null and out-of-bounds accesses are no-ops. A non-null invalid
///          handle raises a runtime trap.
void rt_grid2d_set(rt_grid2d grid, int64_t x, int64_t y, int64_t value);

/// @brief Fill the entire grid with a value.
/// @param grid The grid.
/// @param value The value to fill every cell with.
/// @details Null is a no-op; a non-null invalid handle raises a runtime trap.
void rt_grid2d_fill(rt_grid2d grid, int64_t value);

/// @brief Clear the grid (fill all cells with zeros).
/// @param grid The grid.
/// @details Delegates to rt_grid2d_fill().
void rt_grid2d_clear(rt_grid2d grid);

/// @brief Get the width of the grid.
/// @param grid The grid.
/// @return The positive width, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_width(rt_grid2d grid);

/// @brief Get the height of the grid.
/// @param grid The grid.
/// @return The positive height, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_height(rt_grid2d grid);

/// @brief Check if coordinates are within bounds.
/// @param grid The grid.
/// @param x The x coordinate.
/// @param y The y coordinate.
/// @return 1 if in bounds, 0 otherwise.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_grid2d_in_bounds(rt_grid2d grid, int64_t x, int64_t y);

/// @brief Get the total number of cells.
/// @param grid The grid.
/// @return The creation-validated cell count, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_size(rt_grid2d grid);

/// @brief Check whether the handle has no cells.
/// @param grid The grid.
/// @return 1 for null/invalid, 0 for every successfully created Grid2D.
/// @details Creation rejects zero dimensions. A non-null invalid handle traps.
int8_t rt_grid2d_is_empty(rt_grid2d grid);

/// @brief Copy values from another grid (must be same dimensions).
/// @param dest The destination grid.
/// @param src The source grid.
/// @return 1 on success, 0 for null or mismatched dimensions.
/// @details Self-copy succeeds because the implementation uses memmove().
///          Invalid non-null handles raise runtime traps.
int8_t rt_grid2d_copy_from(rt_grid2d dest, rt_grid2d src);

/// @brief Count cells matching a specific value.
/// @param grid The grid.
/// @param value The value to count.
/// @return The number of exact matches, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_count(rt_grid2d grid, int64_t value);

/// @brief Find the first cell with a specific value (row-major order).
/// @param grid The grid.
/// @param value The value to find.
/// @param out_x Optional output for the found x coordinate.
/// @param out_y Optional output for the found y coordinate.
/// @return 1 if found, 0 otherwise.
/// @details Outputs may independently be null and remain unchanged on failure.
///          A non-null invalid grid raises a runtime trap.
int8_t rt_grid2d_find(rt_grid2d grid, int64_t value, int64_t *out_x, int64_t *out_y);

/// @brief Replace all occurrences of a value with another.
/// @param grid The grid.
/// @param old_value The value to replace.
/// @param new_value The replacement value.
/// @return The number of matching cells processed, or zero for null/invalid.
/// @details Equal old/new values still count matches. A non-null invalid handle
///          raises a runtime trap.
int64_t rt_grid2d_replace(rt_grid2d grid, int64_t old_value, int64_t new_value);

#ifdef __cplusplus
}
#endif
