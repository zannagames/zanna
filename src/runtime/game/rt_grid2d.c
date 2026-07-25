//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_grid2d.c
/// @file
/// @brief Implements a fixed-size, dense, row-major integer grid.
//
// Purpose: Dense 2D array of int64 values for Zanna game maps and grids.
//   Provides O(1) get/set access by (column, row) index, fill and copy
//   operations, and a row-major flat view for efficient bulk processing.
//   Typical uses: tile maps, cellular automata, pathfinding cost grids,
//   flood-fill canvas, and any fixed-width 2D board or level layout.
//
// Key invariants:
//   - Grid dimensions (width, height) are fixed at creation and cannot change.
//     The underlying storage is a single calloc'd row-major array of int64:
//       index = row × width + col
//   - Creation rejects dimensions outside [1, RT_GRID2D_MAX_DIM].
//   - All cells are initialized to the caller-supplied default value.
//   - Out-of-bounds get returns 0; out-of-bounds set is silently ignored.
//     No trap is fired for invalid accesses — callers are responsible for
//     checking bounds if they need error detection.
//   - The grid stores raw int64 values only. Semantic interpretation (tile IDs,
//     passability, cost weights, etc.) is the caller's responsibility.
//
// Ownership/Lifetime:
//   - Grid2D objects use runtime reference counting. The separately malloc'd
//     cell array is released by the object finalizer.
//   - rt_grid2d_destroy releases one object reference and frees the object when
//     its count reaches zero.
//
// Links: src/runtime/game/rt_grid2d.h (public API),
//        docs/zannalib/game.md (Grid2D section)
//
//===----------------------------------------------------------------------===//

#include "rt_grid2d.h"
#include "rt_object.h"
#include "rt_trap.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// @brief Private dimensions and owned cell storage behind a Grid2D handle.
struct rt_grid2d_impl {
    int64_t width;
    int64_t height;
    int64_t *data; // Row-major storage: data[y * width + x]
};

/// @brief Validate an opaque Grid2D handle.
/// @param grid Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return @p grid when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static struct rt_grid2d_impl *checked_grid(rt_grid2d grid, const char *api) {
    if (!grid)
        return NULL;
    if (rt_obj_class_id(grid) != RT_GRID2D_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return grid;
}

/// @brief GC finalizer: free the grid's backing cell array.
/// @param obj Valid Grid2D payload being finalized.
/// @details Frees the separately allocated row-major array and clears its
///          pointer.
static void grid2d_finalizer(void *obj) {
    struct rt_grid2d_impl *grid = (struct rt_grid2d_impl *)obj;
    free(grid->data);
    grid->data = NULL;
}

/// @brief Create a new 2D integer grid filled with a default value.
/// @param width Number of columns in `[1, RT_GRID2D_MAX_DIM]`.
/// @param height Number of rows in `[1, RT_GRID2D_MAX_DIM]`.
/// @param default_value Initial value assigned to every cell.
/// @return A new Grid2D reference, or `NULL` for invalid dimensions, product/
///         allocation-size overflow, or allocation failure.
/// @details Allocates a flat array of width*height int64_t cells. Used for
///          tile-based game data such as collision, influence, and fog-of-war
///          grids. Registers a finalizer only after the array is initialized.
rt_grid2d rt_grid2d_new(int64_t width, int64_t height, int64_t default_value) {
    if (width <= 0 || height <= 0 || width > RT_GRID2D_MAX_DIM || height > RT_GRID2D_MAX_DIM) {
        return NULL;
    }

    // Check for overflow
    if (width > INT64_MAX / height) {
        return NULL;
    }

    int64_t size = width * height;
    if ((uint64_t)size > (uint64_t)SIZE_MAX / sizeof(int64_t))
        return NULL;

    struct rt_grid2d_impl *grid = rt_obj_new_i64(RT_GRID2D_CLASS_ID, sizeof(struct rt_grid2d_impl));
    if (!grid) {
        return NULL;
    }

    grid->data = malloc((size_t)size * sizeof(int64_t));
    if (!grid->data) {
        if (rt_obj_release_check0(grid))
            rt_obj_free(grid);
        return NULL;
    }

    grid->width = width;
    grid->height = height;

    // Fill with default value
    for (int64_t i = 0; i < size; i++) {
        grid->data[i] = default_value;
    }

    rt_obj_set_finalizer(grid, grid2d_finalizer);
    return grid;
}

/// @brief Release one Grid2D reference.
/// @param grid Grid to release; `NULL` is a no-op.
/// @details Frees the object and its cell array when the reference count reaches
///          zero. A non-null invalid handle raises a runtime trap.
void rt_grid2d_destroy(rt_grid2d grid) {
    grid = checked_grid(grid, "Grid2D.Destroy: expected Zanna.Game.Grid2D");
    if (grid && rt_obj_release_check0(grid))
        rt_obj_free(grid);
}

/// @brief Read one cell.
/// @param grid Grid to query.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return Stored value, or zero for null/invalid/out-of-bounds access.
/// @details A non-null invalid handle raises a runtime trap; coordinate misses
///          do not.
int64_t rt_grid2d_get(rt_grid2d grid, int64_t x, int64_t y) {
    grid = checked_grid(grid, "Grid2D.Get: expected Zanna.Game.Grid2D");
    if (!grid)
        return 0;
    if (x < 0 || x >= grid->width || y < 0 || y >= grid->height) {
        return 0;
    }
    return grid->data[y * grid->width + x];
}

/// @brief Write one in-bounds cell.
/// @param grid Grid to mutate.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @param value New raw integer value.
/// @details Null and coordinate misses are no-ops. A non-null invalid handle
///          raises a runtime trap.
void rt_grid2d_set(rt_grid2d grid, int64_t x, int64_t y, int64_t value) {
    grid = checked_grid(grid, "Grid2D.Set: expected Zanna.Game.Grid2D");
    if (!grid)
        return;
    if (x < 0 || x >= grid->width || y < 0 || y >= grid->height) {
        return;
    }
    grid->data[y * grid->width + x] = value;
}

/// @brief Assign one value to every cell.
/// @param grid Grid to mutate; `NULL` is a no-op.
/// @param value Raw integer stored in every row-major slot.
/// @details A non-null invalid handle raises a runtime trap.
void rt_grid2d_fill(rt_grid2d grid, int64_t value) {
    grid = checked_grid(grid, "Grid2D.Fill: expected Zanna.Game.Grid2D");
    if (!grid)
        return;

    int64_t size = grid->width * grid->height;
    for (int64_t i = 0; i < size; i++) {
        grid->data[i] = value;
    }
}

/// @brief Reset every cell to zero.
/// @param grid Grid to mutate.
/// @details Delegates to rt_grid2d_fill(), including its null/trap behavior.
void rt_grid2d_clear(rt_grid2d grid) {
    rt_grid2d_fill(grid, 0);
}

/// @brief Get the width of the grid in cells.
/// @param grid Grid to query.
/// @return Positive column count, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_width(rt_grid2d grid) {
    grid = checked_grid(grid, "Grid2D.Width: expected Zanna.Game.Grid2D");
    return grid ? grid->width : 0;
}

/// @brief Get the height of the grid in cells.
/// @param grid Grid to query.
/// @return Positive row count, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_height(rt_grid2d grid) {
    grid = checked_grid(grid, "Grid2D.Height: expected Zanna.Game.Grid2D");
    return grid ? grid->height : 0;
}

/// @brief Check whether (x, y) is within the grid dimensions.
/// @param grid Grid to query.
/// @param x Candidate zero-based column.
/// @param y Candidate zero-based row.
/// @return `1` when both coordinates are in range; otherwise `0`.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_grid2d_in_bounds(rt_grid2d grid, int64_t x, int64_t y) {
    grid = checked_grid(grid, "Grid2D.InBounds: expected Zanna.Game.Grid2D");
    if (!grid)
        return 0;
    return (x >= 0 && x < grid->width && y >= 0 && y < grid->height) ? 1 : 0;
}

/// @brief Return the total number of cells (width * height).
/// @param grid Grid to query.
/// @return Creation-validated cell count, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_size(rt_grid2d grid) {
    grid = checked_grid(grid, "Grid2D.Size: expected Zanna.Game.Grid2D");
    return grid ? grid->width * grid->height : 0;
}

/// @brief Check whether the grid2d has no entries.
/// @param grid Grid to query.
/// @return `1` for a null/invalid handle; `0` for every successfully created
///         grid because creation requires positive dimensions.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_grid2d_is_empty(rt_grid2d grid) {
    grid = checked_grid(grid, "Grid2D.IsEmpty: expected Zanna.Game.Grid2D");
    if (!grid)
        return 1;
    return (grid->width == 0 || grid->height == 0) ? 1 : 0;
}

/// @brief Copy all cell values from another grid of the same dimensions.
/// @param dest Destination grid.
/// @param src Source grid.
/// @return `1` after copying equal-sized grids; otherwise `0`.
/// @details Uses memmove(), so self-copy succeeds. Null or dimension mismatch
///          returns zero; invalid non-null handles raise runtime traps.
int8_t rt_grid2d_copy_from(rt_grid2d dest, rt_grid2d src) {
    dest = checked_grid(dest, "Grid2D.CopyFrom: expected destination Zanna.Game.Grid2D");
    src = checked_grid(src, "Grid2D.CopyFrom: expected source Zanna.Game.Grid2D");
    if (!dest || !src)
        return 0;
    if (dest->width != src->width || dest->height != src->height) {
        return 0;
    }

    int64_t size = dest->width * dest->height;
    memmove(dest->data, src->data, (size_t)size * sizeof(int64_t));
    return 1;
}

/// @brief Count how many cells contain the given value.
/// @param grid Grid to scan.
/// @param value Raw integer to match.
/// @return Number of exact matches, or zero for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_grid2d_count(rt_grid2d grid, int64_t value) {
    grid = checked_grid(grid, "Grid2D.Count: expected Zanna.Game.Grid2D");
    if (!grid)
        return 0;

    int64_t count = 0;
    int64_t size = grid->width * grid->height;
    for (int64_t i = 0; i < size; i++) {
        if (grid->data[i] == value) {
            count++;
        }
    }
    return count;
}

/// @brief Find the first cell containing the given value (row-major scan order).
/// @param grid Grid to scan.
/// @param value Raw integer to match.
/// @param out_x Optional output receiving the found column.
/// @param out_y Optional output receiving the found row.
/// @return `1` on the first match; otherwise `0`.
/// @details Output pointers may independently be null and remain unchanged on
///          failure. A non-null invalid grid raises a runtime trap.
int8_t rt_grid2d_find(rt_grid2d grid, int64_t value, int64_t *out_x, int64_t *out_y) {
    grid = checked_grid(grid, "Grid2D.Find: expected Zanna.Game.Grid2D");
    if (!grid)
        return 0;

    for (int64_t y = 0; y < grid->height; y++) {
        for (int64_t x = 0; x < grid->width; x++) {
            if (grid->data[y * grid->width + x] == value) {
                if (out_x)
                    *out_x = x;
                if (out_y)
                    *out_y = y;
                return 1;
            }
        }
    }
    return 0;
}

/// @brief Replace all occurrences of old_value with new_value; returns the number replaced.
/// @param grid Grid to mutate.
/// @param old_value Exact value to match.
/// @param new_value Replacement stored for every match.
/// @return Number of replacements, or zero for null/invalid/no matches.
/// @details Equal old/new values still count every matching cell. A non-null
///          invalid handle raises a runtime trap.
int64_t rt_grid2d_replace(rt_grid2d grid, int64_t old_value, int64_t new_value) {
    grid = checked_grid(grid, "Grid2D.Replace: expected Zanna.Game.Grid2D");
    if (!grid)
        return 0;

    int64_t count = 0;
    int64_t size = grid->width * grid->height;
    for (int64_t i = 0; i < size; i++) {
        if (grid->data[i] == old_value) {
            grid->data[i] = new_value;
            count++;
        }
    }
    return count;
}
