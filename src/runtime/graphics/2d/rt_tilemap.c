//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_tilemap.c
// Purpose: Layered Tilemap storage, projected/source-frame rendering, scaled
//   editor drawing and hit testing, animation, and grid collision.
//
// Key invariants:
//   - Logical cells remain a fixed, flat row-major int64 grid; tile ID zero is empty.
//   - Imported source-frame dimensions never replace logical collision dimensions.
//   - Projection and inverse-selection math is deterministic and saturating.
//   - Native and scaled drawing share imported visual traversal order.
//
// Ownership/Lifetime:
//   - Tilemaps use one managed allocation with an inline base tile array.
//   - The finalizer releases cloned layer/base tilesets, owned layer arrays, and
//     dynamically allocated animation frame/duration arrays.
//   - Per-call scaled Pixels and caches are released before drawing returns.
//
// Links: rt_tilemap.h, rt_tilemap_internal.h, rt_tilemap_io.c,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements layered Tilemap storage, projection, drawing, animation, and collision.
 *
 * @details The runtime maintains fixed logical cell geometry while supporting
 *          imported orthogonal, isometric, staggered, hexagonal, and oblique
 *          source layouts. Native and scaled rendering share deterministic
 *          traversal, saturating coordinate math, animation resolution, layer
 *          metadata, hit testing, and grid-based collision behavior.
 */

#include "rt_tilemap.h"
#include "rt_tilemap_internal.h"

#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_physics2d.h"
#include "rt_physics2d_internal.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Internal alias for a cell with no collision response.
#define TILE_COLLISION_NONE RT_TILE_COLLISION_NONE
/// @brief Internal alias for a fully solid collision cell.
#define TILE_COLLISION_SOLID RT_TILE_COLLISION_SOLID
/// @brief Internal alias for an upward-facing one-way collision cell.
#define TILE_COLLISION_ONE_WAY RT_TILE_COLLISION_ONE_WAY_UP

/// @brief Integer floor division that rounds toward -∞ rather than toward zero.
/// @details C's built-in `/` operator truncates toward zero, which produces incorrect
///   tile-coordinate results for negative world positions. For example, world pixel -1
///   on a 16-pixel tile should map to tile -1, not tile 0. The correction subtracts 1
///   when the quotient was rounded toward zero away from the true floor (i.e., when the
///   remainder is non-zero and the operands have different signs).
/// @param value Dividend to convert from pixels to a signed grid coordinate.
/// @param divisor Must not be zero; returns 0 if it is (defensive).
/// @return ⌊value / divisor⌋ with floor semantics.
static int64_t tilemap_floor_div(int64_t value, int64_t divisor) {
    if (divisor == 0)
        return 0;
    int64_t quot = value / divisor;
    int64_t rem = value % divisor;
    if (rem != 0 && ((rem < 0) != (divisor < 0)))
        quot--;
    return quot;
}

/// @brief Convert a double to int64_t with saturation, writing the result to *out.
/// @details Handles NaN/infinity (returns 0) and values at or beyond the int64_t bounds
///          (saturates to INT64_MAX / INT64_MIN). Used to convert floating-point camera
///          scroll positions to tile coordinates without undefined-behavior casts.
/// @param value Floating-point value to truncate toward zero after range handling.
/// @param out Required destination for the converted integer.
/// @return 1 on success (finite value in range or saturated); 0 if `out` is NULL or value is
/// non-finite.
static int8_t tilemap_double_to_i64_sat(double value, int64_t *out) {
    if (!out || !isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX) {
        *out = INT64_MAX;
        return 1;
    }
    if (value <= (double)INT64_MIN) {
        *out = INT64_MIN;
        return 1;
    }
    *out = (int64_t)value;
    return 1;
}

/// @brief Validate tilemap dimensions and compute allocation sizes without overflow.
/// @details Returns 0 for non-positive dimensions or if width × height would overflow int64_t,
///          or if tile_count × sizeof(int64_t) would overflow size_t. On success writes the
///          tile count and byte size to the optional output pointers.
/// @param width Proposed positive grid width.
/// @param height Proposed positive grid height.
/// @param tile_count_out Optional destination for `width * height`.
/// @param tiles_size_out Optional destination for the tile array's byte size.
/// @return 1 if the grid is valid and sizes were computed; 0 otherwise.
static int32_t tilemap_checked_grid_size(int64_t width,
                                         int64_t height,
                                         int64_t *tile_count_out,
                                         size_t *tiles_size_out) {
    if (width <= 0 || height <= 0)
        return 0;
    if (width > INT64_MAX / height)
        return 0;
    int64_t tile_count = width * height;
    if ((uint64_t)tile_count > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
        return 0;
    size_t tiles_size = (size_t)tile_count * sizeof(int64_t);
    if (tile_count_out)
        *tile_count_out = tile_count;
    if (tiles_size_out)
        *tiles_size_out = tiles_size;
    return 1;
}

/// @brief Validate-and-return a Tilemap pointer; trap-or-NULL on failure.
/// @details If @p tilemap_ptr is NULL and @p trap_message is non-NULL, raises
///          a runtime trap with the provided diagnostic. NULL @p trap_message
///          turns the NULL check into a soft no-op (returns NULL silently).
///          Wrong-class handles always return NULL silently — the GC may
///          reach this with stale references during finalization, and a trap
///          there would crash the collector.
/// @param tilemap_ptr Candidate opaque Tilemap handle.
/// @param trap_message Optional diagnostic used only when the handle is null.
/// @return Validated Tilemap implementation pointer, or `NULL` on failure.
static rt_tilemap_impl *tilemap_checked(void *tilemap_ptr, const char *trap_message) {
    if (!tilemap_ptr) {
        if (trap_message)
            rt_trap(trap_message);
        return NULL;
    }
    if (!rt_obj_is_instance(tilemap_ptr, RT_TILEMAP_CLASS_ID, sizeof(rt_tilemap_impl)))
        return NULL;
    return (rt_tilemap_impl *)tilemap_ptr;
}

/// @brief Resolve a tile id to its current animation frame, given an already-validated impl.
/// @details Hot-loop variant of rt_tilemap_resolve_anim_tile for the per-tile draw
///   paths: it takes the validated impl (skipping the per-call handle check) and
///   short-circuits when the map has no animated tiles — the common case — so a
///   full-viewport redraw no longer pays an O(tile_anim_count) scan plus an
///   rt_obj_is_instance validation for every drawn tile.
/// @param tm Already validated Tilemap implementation.
/// @param tile_id Base tile identifier to resolve.
/// @return Current animation frame tile when configured, otherwise @p tile_id.
static inline int64_t tilemap_resolve_anim_tile_fast(rt_tilemap_impl *tm, int64_t tile_id) {
    if (!tm || tm->tile_anim_count == 0)
        return tile_id;
    for (int32_t i = 0; i < tm->tile_anim_count; i++) {
        if (tm->tile_anims[i].base_tile_id == tile_id)
            return tm->tile_anims[i].frame_tiles[tm->tile_anims[i].current_frame];
    }
    return tile_id;
}

/// @brief Return the absolute distance from @p value to zero as an unsigned integer.
/// @details Equivalent to (uint64_t)(-value) but safe for all int64_t inputs including
///          INT64_MIN, which has no positive two's-complement representation. The result
///          is used to measure how far a negative tile coordinate is from the origin so
///          the caller can skip that many tiles without signed-overflow arithmetic.
/// @param value Signed coordinate, normally negative.
/// @return Exact unsigned magnitude of @p value.
static uint64_t tilemap_distance_to_zero(int64_t value) {
    return (uint64_t)(-(value + 1)) + 1u;
}

/// @brief Negate @p value with saturation — INT64_MIN has no positive representation
///        in two's complement, so it saturates to INT64_MAX instead.
/// @param value Signed value to negate.
/// @return `-value`, or `INT64_MAX` for `INT64_MIN`.
static int64_t tilemap_negate_saturating(int64_t value) {
    return value == INT64_MIN ? INT64_MAX : -value;
}

/// @brief Add two int64_t values with saturation at INT64_MAX / INT64_MIN.
/// @details Used for all pixel-coordinate addition in tilemap rendering and collision
///   to prevent wrapping when a world coordinate plus offset exceeds the int64_t range.
/// @param a Left addend.
/// @param b Right addend.
/// @return Mathematical sum when representable, otherwise the corresponding limit.
static int64_t tilemap_add_saturating(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Saturating int64 subtraction (a - b), clamped to the int64 range.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return Mathematical difference when representable, otherwise the corresponding limit.
static int64_t tilemap_sub_saturating(int64_t a, int64_t b) {
    if (b > 0 && a < INT64_MIN + b)
        return INT64_MIN;
    if (b < 0 && a > INT64_MAX + b)
        return INT64_MAX;
    return a - b;
}

/// @brief Multiply two int64_t values with saturation at INT64_MAX / INT64_MIN.
/// @details All intermediate tilemap coordinate computations (tile_x * tile_width,
///   tile_y * tile_height, etc.) pass through this function to prevent undefined
///   behavior from signed integer overflow. The special case `(-1) * INT64_MIN` is
///   handled explicitly because it is the only multiply whose absolute value exceeds
///   INT64_MAX.
/// @param a Left factor.
/// @param b Right factor.
/// @return Mathematical product when representable, otherwise the corresponding limit.
static int64_t tilemap_mul_saturating(int64_t a, int64_t b) {
    if (a == 0 || b == 0)
        return 0;
    if (a == -1 && b == INT64_MIN)
        return INT64_MAX;
    if (b == -1 && a == INT64_MIN)
        return INT64_MAX;
    if (a > 0) {
        if (b > 0 && a > INT64_MAX / b)
            return INT64_MAX;
        if (b < 0 && b < INT64_MIN / a)
            return INT64_MIN;
    } else {
        if (b > 0 && a < INT64_MIN / b)
            return INT64_MIN;
        if (b < 0 && a < INT64_MAX / b)
            return INT64_MAX;
    }
    return a * b;
}

/// @brief Scale a positive dimension by an integer percentage with saturation.
/// @param dim Source dimension.
/// @param scale_percent Scale percentage.
/// @return Truncated scaled dimension, clamped to at least one.
static int64_t tilemap_scale_dimension(int64_t dim, int64_t scale_percent) {
    int64_t scaled = tilemap_mul_saturating(dim, scale_percent);
    scaled /= 100;
    return scaled <= 0 ? 1 : scaled;
}

/// @brief Round a floating-point value to nearest with half ties away from zero.
/// @param value Value to round; NaN maps to zero.
/// @return Rounded integer saturated to the `int64_t` range.
static int64_t tilemap_round_ties_away_saturating(double value) {
    if (isnan(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    double rounded = value < 0.0 ? ceil(value - 0.5) : floor(value + 0.5);
    if (rounded >= (double)INT64_MAX)
        return INT64_MAX;
    if (rounded <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)rounded;
}

/// @brief Test whether a source-grid coordinate belongs to the configured stagger parity.
/// @param tilemap Imported-layout configuration to inspect.
/// @param coordinate Signed row or column coordinate.
/// @return Nonzero when the coordinate matches the configured even/odd parity.
static int tilemap_is_staggered_coordinate(const rt_tilemap_impl *tilemap, int64_t coordinate) {
    int64_t remainder = coordinate % 2;
    if (remainder < 0)
        remainder += 2;
    return tilemap->import_stagger_even ? remainder == 0 : remainder == 1;
}

/// @brief Project one logical tile coordinate into imported source-pixel space.
/// @details Applies the imported origin and orientation-specific orthogonal,
///          isometric, staggered, hexagonal, or oblique transform.
/// @param tilemap Imported layout configuration.
/// @param tile_x Logical tile X coordinate.
/// @param tile_y Logical tile Y coordinate.
/// @param pixel_x Destination for projected source-space X.
/// @param pixel_y Destination for projected source-space Y.
static void tilemap_project_source(const rt_tilemap_impl *tilemap,
                                   int64_t tile_x,
                                   int64_t tile_y,
                                   double *pixel_x,
                                   double *pixel_y) {
    int64_t source_x = tilemap_add_saturating(tile_x, tilemap->import_origin_tile_x);
    int64_t source_y = tilemap_add_saturating(tile_y, tilemap->import_origin_tile_y);
    double x = (double)source_x;
    double y = (double)source_y;
    double tw = (double)tilemap->tile_width;
    double th = (double)tilemap->tile_height;
    switch (tilemap->import_orientation) {
        case RT_TILEMAP_IMPORT_ISOMETRIC:
            *pixel_x = (x - y) * tw * 0.5 + (double)tilemap->import_projection_height * tw * 0.5;
            *pixel_y = (x + y) * th * 0.5;
            break;
        case RT_TILEMAP_IMPORT_STAGGERED:
        case RT_TILEMAP_IMPORT_HEXAGONAL: {
            int64_t side_length_x = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                            tilemap->import_stagger_axis == 0
                                        ? tilemap->import_hex_side_length
                                        : 0;
            int64_t side_length_y = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                            tilemap->import_stagger_axis == 1
                                        ? tilemap->import_hex_side_length
                                        : 0;
            double side_x = (double)side_length_x;
            double side_y = (double)side_length_y;
            double side_offset_x = (double)((tilemap->tile_width - side_length_x) / 2);
            double side_offset_y = (double)((tilemap->tile_height - side_length_y) / 2);
            double column_width = side_offset_x + side_x;
            double row_height = side_offset_y + side_y;
            if (tilemap->import_stagger_axis == 0) {
                *pixel_x = x * column_width;
                *pixel_y = y * (th + side_y) +
                           (tilemap_is_staggered_coordinate(tilemap, source_x) ? row_height : 0.0);
            } else {
                *pixel_x =
                    x * (tw + side_x) +
                    (tilemap_is_staggered_coordinate(tilemap, source_y) ? column_width : 0.0);
                *pixel_y = y * row_height;
            }
            break;
        }
        case RT_TILEMAP_IMPORT_OBLIQUE: {
            double base_x = x * tw;
            double base_y = y * th;
            *pixel_x = base_x + (tilemap->import_skew_x / th) * base_y;
            *pixel_y = base_y + (tilemap->import_skew_y / tw) * base_x;
            break;
        }
        case RT_TILEMAP_IMPORT_ORTHOGONAL:
        default:
            *pixel_x = x * tw;
            *pixel_y = y * th;
            break;
    }
}

/// @brief Combine camera, layer, map, and parallax-origin offsets for native drawing.
/// @param tilemap Imported map-level layout configuration.
/// @param layer Imported layer metadata.
/// @param camera_x Destination-pixel camera X.
/// @param camera_y Destination-pixel camera Y.
/// @param offset_x Destination for effective screen X offset.
/// @param offset_y Destination for effective screen Y offset.
static void tilemap_effective_layer_offset(const rt_tilemap_impl *tilemap,
                                           const tm_layer *layer,
                                           int64_t camera_x,
                                           int64_t camera_y,
                                           double *offset_x,
                                           double *offset_y) {
    *offset_x = (double)camera_x * layer->import_parallax_x + layer->import_offset_x +
                tilemap->import_parallax_origin_x * (1.0 - layer->import_parallax_x) +
                (double)tilemap->import_draw_offset_x;
    *offset_y = (double)camera_y * layer->import_parallax_y + layer->import_offset_y +
                tilemap->import_parallax_origin_y * (1.0 - layer->import_parallax_y) +
                (double)tilemap->import_draw_offset_y;
}

/// @brief Compute screen offset for a zoomed imported layer.
/// @details Camera scroll is already expressed in destination pixels and is not
///          zoomed. Authored layer, tileset-frame, and parallax-origin offsets are
///          source-space geometry and therefore scale with the map.
/// @param tilemap Imported map-level layout configuration.
/// @param layer Imported layer metadata.
/// @param camera_x Destination-pixel camera X.
/// @param camera_y Destination-pixel camera Y.
/// @param map_scale Positive editor zoom multiplier.
/// @param offset_x Destination for effective screen X offset.
/// @param offset_y Destination for effective screen Y offset.
static void tilemap_effective_layer_offset_scaled(const rt_tilemap_impl *tilemap,
                                                  const tm_layer *layer,
                                                  int64_t camera_x,
                                                  int64_t camera_y,
                                                  double map_scale,
                                                  double *offset_x,
                                                  double *offset_y) {
    double authored_x = layer->import_offset_x +
                        tilemap->import_parallax_origin_x * (1.0 - layer->import_parallax_x) +
                        (double)tilemap->import_draw_offset_x;
    double authored_y = layer->import_offset_y +
                        tilemap->import_parallax_origin_y * (1.0 - layer->import_parallax_y) +
                        (double)tilemap->import_draw_offset_y;
    *offset_x = (double)camera_x * layer->import_parallax_x + authored_x * map_scale;
    *offset_y = (double)camera_y * layer->import_parallax_y + authored_y * map_scale;
}

/// @brief Apply the analytic inverse of the imported source projection.
/// @details Staggered/hexagonal results are approximate continuous coordinates;
///          exact cell selection is completed by `tilemap_projected_pixel_to_cell()`.
/// @param tilemap Imported layout configuration.
/// @param pixel_x Source-space pixel X.
/// @param pixel_y Source-space pixel Y.
/// @param tile_x Destination for continuous logical tile X.
/// @param tile_y Destination for continuous logical tile Y.
static void tilemap_inverse_project_source(const rt_tilemap_impl *tilemap,
                                           double pixel_x,
                                           double pixel_y,
                                           double *tile_x,
                                           double *tile_y) {
    double tw = (double)tilemap->tile_width;
    double th = (double)tilemap->tile_height;
    double source_x = 0.0;
    double source_y = 0.0;
    switch (tilemap->import_orientation) {
        case RT_TILEMAP_IMPORT_ISOMETRIC: {
            double dx = pixel_x - (double)tilemap->import_projection_height * tw * 0.5;
            source_x = dx / tw + pixel_y / th;
            source_y = pixel_y / th - dx / tw;
            break;
        }
        case RT_TILEMAP_IMPORT_STAGGERED:
        case RT_TILEMAP_IMPORT_HEXAGONAL: {
            int64_t side_length_x = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                            tilemap->import_stagger_axis == 0
                                        ? tilemap->import_hex_side_length
                                        : 0;
            int64_t side_length_y = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                            tilemap->import_stagger_axis == 1
                                        ? tilemap->import_hex_side_length
                                        : 0;
            double side_x = (double)side_length_x;
            double side_y = (double)side_length_y;
            double column_width = (double)((tilemap->tile_width - side_length_x) / 2) + side_x;
            double row_height = (double)((tilemap->tile_height - side_length_y) / 2) + side_y;
            if (tilemap->import_stagger_axis == 0) {
                source_x = pixel_x / column_width;
                source_y = pixel_y / (th + side_y);
            } else {
                source_x = pixel_x / (tw + side_x);
                source_y = pixel_y / row_height;
            }
            break;
        }
        case RT_TILEMAP_IMPORT_OBLIQUE: {
            double horizontal = tilemap->import_skew_x / th;
            double vertical = tilemap->import_skew_y / tw;
            double determinant = 1.0 - horizontal * vertical;
            double base_x = (pixel_x - horizontal * pixel_y) / determinant;
            double base_y = (pixel_y - vertical * pixel_x) / determinant;
            source_x = base_x / tw;
            source_y = base_y / th;
            break;
        }
        case RT_TILEMAP_IMPORT_ORTHOGONAL:
        default:
            source_x = pixel_x / tw;
            source_y = pixel_y / th;
            break;
    }
    *tile_x = source_x - (double)tilemap->import_origin_tile_x;
    *tile_y = source_y - (double)tilemap->import_origin_tile_y;
}

/// @brief Floor a double and saturate it to `int64_t`.
/// @param value Value to floor; infinities saturate by sign.
/// @return Floored and saturated integer.
static int64_t tilemap_floor_double_saturating(double value) {
    if (!isfinite(value))
        return value < 0.0 ? INT64_MIN : INT64_MAX;
    value = floor(value);
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    return (int64_t)value;
}

/// @brief Ceil a double and saturate it to `int64_t`.
/// @param value Value to ceil; infinities saturate by sign.
/// @return Ceiled and saturated integer.
static int64_t tilemap_ceil_double_saturating(double value) {
    if (!isfinite(value))
        return value < 0.0 ? INT64_MIN : INT64_MAX;
    value = ceil(value);
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    return (int64_t)value;
}

/// @brief Diagonal neighbor choices used by staggered diamond corner tests.
enum tilemap_stagger_neighbor {
    TILEMAP_NEIGHBOR_TOP_LEFT = 0,    ///< Diagonal cell above and to the left.
    TILEMAP_NEIGHBOR_TOP_RIGHT = 1,   ///< Diagonal cell above and to the right.
    TILEMAP_NEIGHBOR_BOTTOM_LEFT = 2, ///< Diagonal cell below and to the left.
    TILEMAP_NEIGHBOR_BOTTOM_RIGHT = 3, ///< Diagonal cell below and to the right.
};

/// @brief Move one source-grid coordinate to a stagger-aware diagonal neighbor.
/// @param tilemap Imported stagger-axis/parity configuration.
/// @param source_x In/out source-grid X coordinate.
/// @param source_y In/out source-grid Y coordinate.
/// @param neighbor Diagonal direction to apply.
static void tilemap_move_stagger_neighbor(const rt_tilemap_impl *tilemap,
                                          int64_t *source_x,
                                          int64_t *source_y,
                                          enum tilemap_stagger_neighbor neighbor) {
    if (!tilemap || !source_x || !source_y)
        return;
    int staggered = tilemap_is_staggered_coordinate(
        tilemap, tilemap->import_stagger_axis == 0 ? *source_x : *source_y);
    if (tilemap->import_stagger_axis == 1) {
        int move_right =
            neighbor == TILEMAP_NEIGHBOR_TOP_RIGHT || neighbor == TILEMAP_NEIGHBOR_BOTTOM_RIGHT;
        int move_down =
            neighbor == TILEMAP_NEIGHBOR_BOTTOM_LEFT || neighbor == TILEMAP_NEIGHBOR_BOTTOM_RIGHT;
        if ((move_right && staggered) || (!move_right && !staggered))
            *source_x = tilemap_add_saturating(*source_x, move_right ? 1 : -1);
        *source_y = tilemap_add_saturating(*source_y, move_down ? 1 : -1);
    } else {
        int move_right =
            neighbor == TILEMAP_NEIGHBOR_TOP_RIGHT || neighbor == TILEMAP_NEIGHBOR_BOTTOM_RIGHT;
        int move_down =
            neighbor == TILEMAP_NEIGHBOR_BOTTOM_LEFT || neighbor == TILEMAP_NEIGHBOR_BOTTOM_RIGHT;
        *source_x = tilemap_add_saturating(*source_x, move_right ? 1 : -1);
        if ((move_down && staggered) || (!move_down && !staggered))
            *source_y = tilemap_add_saturating(*source_y, move_down ? 1 : -1);
    }
}

/// @brief Resolve one projected pixel to its exact staggered/hexagonal grid cell.
/// @details This follows Tiled's diamond corner tests for staggered maps and
///          nearest-center selection for hexagonal maps. Other orientations use
///          the analytic inverse projection.
/// @param tilemap Imported layout configuration.
/// @param pixel_x Projected source-space pixel X.
/// @param pixel_y Projected source-space pixel Y.
/// @param tile_x Destination for the exact logical tile X.
/// @param tile_y Destination for the exact logical tile Y.
static void tilemap_projected_pixel_to_cell(const rt_tilemap_impl *tilemap,
                                            double pixel_x,
                                            double pixel_y,
                                            int64_t *tile_x,
                                            int64_t *tile_y) {
    if (!tilemap || !tile_x || !tile_y)
        return;
    if (tilemap->import_orientation != RT_TILEMAP_IMPORT_STAGGERED &&
        tilemap->import_orientation != RT_TILEMAP_IMPORT_HEXAGONAL) {
        double projected_x = 0.0;
        double projected_y = 0.0;
        tilemap_inverse_project_source(tilemap, pixel_x, pixel_y, &projected_x, &projected_y);
        *tile_x = tilemap_floor_double_saturating(projected_x);
        *tile_y = tilemap_floor_double_saturating(projected_y);
        return;
    }

    int64_t side_length_x = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                    tilemap->import_stagger_axis == 0
                                ? tilemap->import_hex_side_length
                                : 0;
    int64_t side_length_y = tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL &&
                                    tilemap->import_stagger_axis == 1
                                ? tilemap->import_hex_side_length
                                : 0;
    int64_t side_offset_x = (tilemap->tile_width - side_length_x) / 2;
    int64_t side_offset_y = (tilemap->tile_height - side_length_y) / 2;
    int64_t column_width = side_offset_x + side_length_x;
    int64_t row_height = side_offset_y + side_length_y;
    int64_t source_x = 0;
    int64_t source_y = 0;

    /* One-pixel perpendicular dimensions can collapse a half-step to zero.
     * Tiled files permit this degenerate geometry and the forward transform is
     * still deterministic, but nearest-center inversion is not. Preserve the
     * non-collapsed axis and choose source coordinate zero for the collapsed one. */
    if (column_width <= 0 || row_height <= 0) {
        if (tilemap->import_stagger_axis == 0) {
            source_x = column_width > 0
                           ? tilemap_floor_double_saturating(pixel_x / (double)column_width)
                           : 0;
            source_y = tilemap_floor_double_saturating(pixel_y / (double)tilemap->tile_height);
        } else {
            source_x = tilemap_floor_double_saturating(pixel_x / (double)tilemap->tile_width);
            source_y =
                row_height > 0 ? tilemap_floor_double_saturating(pixel_y / (double)row_height) : 0;
        }
        *tile_x = tilemap_sub_saturating(source_x, tilemap->import_origin_tile_x);
        *tile_y = tilemap_sub_saturating(source_y, tilemap->import_origin_tile_y);
        return;
    }

    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_HEXAGONAL) {
        double aligned_x = pixel_x;
        double aligned_y = pixel_y;
        if (tilemap->import_stagger_axis == 0)
            aligned_x -=
                (double)(tilemap->import_stagger_even ? tilemap->tile_width : side_offset_x);
        else
            aligned_y -=
                (double)(tilemap->import_stagger_even ? tilemap->tile_height : side_offset_y);
        source_x = tilemap_floor_double_saturating(aligned_x / ((double)column_width * 2.0));
        source_y = tilemap_floor_double_saturating(aligned_y / ((double)row_height * 2.0));
        double relative_x = aligned_x - (double)source_x * (double)column_width * 2.0;
        double relative_y = aligned_y - (double)source_y * (double)row_height * 2.0;
        if (tilemap->import_stagger_axis == 0)
            source_x = tilemap_mul_saturating(source_x, 2);
        else
            source_y = tilemap_mul_saturating(source_y, 2);
        if (tilemap->import_stagger_even) {
            if (tilemap->import_stagger_axis == 0)
                source_x = tilemap_add_saturating(source_x, 1);
            else
                source_y = tilemap_add_saturating(source_y, 1);
        }

        double centers_x[4];
        double centers_y[4];
        int offsets_x[4];
        int offsets_y[4];
        if (tilemap->import_stagger_axis == 0) {
            int64_t left = side_length_x / 2;
            double center_x = (double)(left + column_width);
            double center_y = (double)tilemap->tile_height / 2.0;
            centers_x[0] = (double)left;
            centers_y[0] = center_y;
            centers_x[1] = center_x;
            centers_y[1] = center_y - (double)row_height;
            centers_x[2] = center_x;
            centers_y[2] = center_y + (double)row_height;
            centers_x[3] = center_x + (double)column_width;
            centers_y[3] = center_y;
            int x_values[4] = {0, 1, 1, 2};
            int y_values[4] = {0, -1, 0, 0};
            memcpy(offsets_x, x_values, sizeof(offsets_x));
            memcpy(offsets_y, y_values, sizeof(offsets_y));
        } else {
            int64_t top = side_length_y / 2;
            double center_x = (double)tilemap->tile_width / 2.0;
            double center_y = (double)(top + row_height);
            centers_x[0] = center_x;
            centers_y[0] = (double)top;
            centers_x[1] = center_x - (double)column_width;
            centers_y[1] = center_y;
            centers_x[2] = center_x + (double)column_width;
            centers_y[2] = center_y;
            centers_x[3] = center_x;
            centers_y[3] = center_y + (double)row_height;
            int x_values[4] = {0, -1, 0, 0};
            int y_values[4] = {0, 1, 1, 2};
            memcpy(offsets_x, x_values, sizeof(offsets_x));
            memcpy(offsets_y, y_values, sizeof(offsets_y));
        }
        int nearest = 0;
        double minimum_distance = INFINITY;
        for (int index = 0; index < 4; ++index) {
            double dx = centers_x[index] - relative_x;
            double dy = centers_y[index] - relative_y;
            double distance = dx * dx + dy * dy;
            if (distance < minimum_distance) {
                minimum_distance = distance;
                nearest = index;
            }
        }
        source_x = tilemap_add_saturating(source_x, offsets_x[nearest]);
        source_y = tilemap_add_saturating(source_y, offsets_y[nearest]);
    } else {
        double aligned_x = pixel_x;
        double aligned_y = pixel_y;
        if (tilemap->import_stagger_axis == 0 && tilemap->import_stagger_even)
            aligned_x -= (double)side_offset_x;
        if (tilemap->import_stagger_axis == 1 && tilemap->import_stagger_even)
            aligned_y -= (double)side_offset_y;
        source_x = tilemap_floor_double_saturating(aligned_x / (double)tilemap->tile_width);
        source_y = tilemap_floor_double_saturating(aligned_y / (double)tilemap->tile_height);
        double relative_x = aligned_x - (double)source_x * (double)tilemap->tile_width;
        double relative_y = aligned_y - (double)source_y * (double)tilemap->tile_height;
        if (tilemap->import_stagger_axis == 0)
            source_x = tilemap_mul_saturating(source_x, 2);
        else
            source_y = tilemap_mul_saturating(source_y, 2);
        if (tilemap->import_stagger_even) {
            if (tilemap->import_stagger_axis == 0)
                source_x = tilemap_add_saturating(source_x, 1);
            else
                source_y = tilemap_add_saturating(source_y, 1);
        }
        double diagonal_y =
            relative_x * ((double)tilemap->tile_height / (double)tilemap->tile_width);
        if ((double)side_offset_y - diagonal_y > relative_y)
            tilemap_move_stagger_neighbor(tilemap, &source_x, &source_y, TILEMAP_NEIGHBOR_TOP_LEFT);
        if (-(double)side_offset_y + diagonal_y > relative_y)
            tilemap_move_stagger_neighbor(
                tilemap, &source_x, &source_y, TILEMAP_NEIGHBOR_TOP_RIGHT);
        if ((double)side_offset_y + diagonal_y < relative_y)
            tilemap_move_stagger_neighbor(
                tilemap, &source_x, &source_y, TILEMAP_NEIGHBOR_BOTTOM_LEFT);
        if ((double)side_offset_y * 3.0 - diagonal_y < relative_y)
            tilemap_move_stagger_neighbor(
                tilemap, &source_x, &source_y, TILEMAP_NEIGHBOR_BOTTOM_RIGHT);
    }

    *tile_x = tilemap_sub_saturating(source_x, tilemap->import_origin_tile_x);
    *tile_y = tilemap_sub_saturating(source_y, tilemap->import_origin_tile_y);
}

/// @brief Compute a conservative logical viewport for an imported projected layer.
/// @details Inverse-projects the four canvas corners after accounting for frame
///          extent and effective layer offset, expands by three cells, and clips
///          the result to map bounds.
/// @param tilemap Imported layout and logical bounds.
/// @param layer Layer supplying offset and parallax.
/// @param canvas_width Positive destination width.
/// @param canvas_height Positive destination height.
/// @param camera_x Camera X in destination pixels.
/// @param camera_y Camera Y in destination pixels.
/// @param view_x Destination for first logical X.
/// @param view_y Destination for first logical Y.
/// @param view_width Destination for visible logical width.
/// @param view_height Destination for visible logical height.
/// @return Nonzero when a nonempty clipped viewport is produced.
static int32_t tilemap_visible_import_region(rt_tilemap_impl *tilemap,
                                             tm_layer *layer,
                                             int64_t canvas_width,
                                             int64_t canvas_height,
                                             int64_t camera_x,
                                             int64_t camera_y,
                                             int64_t *view_x,
                                             int64_t *view_y,
                                             int64_t *view_width,
                                             int64_t *view_height) {
    if (!tilemap || !layer || canvas_width <= 0 || canvas_height <= 0 || !view_x || !view_y ||
        !view_width || !view_height)
        return 0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    tilemap_effective_layer_offset(tilemap, layer, camera_x, camera_y, &offset_x, &offset_y);
    double screen_x[2] = {-(double)tilemap->source_frame_width - offset_x,
                          (double)canvas_width - offset_x};
    double screen_y[2] = {-(double)tilemap->source_frame_height - offset_y,
                          (double)canvas_height - offset_y};
    double minimum_x = INFINITY;
    double minimum_y = INFINITY;
    double maximum_x = -INFINITY;
    double maximum_y = -INFINITY;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            double tile_x = 0.0;
            double tile_y = 0.0;
            tilemap_inverse_project_source(tilemap, screen_x[x], screen_y[y], &tile_x, &tile_y);
            minimum_x = fmin(minimum_x, tile_x);
            minimum_y = fmin(minimum_y, tile_y);
            maximum_x = fmax(maximum_x, tile_x);
            maximum_y = fmax(maximum_y, tile_y);
        }
    }
    int64_t first_x = tilemap_add_saturating(tilemap_floor_double_saturating(minimum_x), -3);
    int64_t first_y = tilemap_add_saturating(tilemap_floor_double_saturating(minimum_y), -3);
    int64_t last_x = tilemap_add_saturating(tilemap_ceil_double_saturating(maximum_x), 3);
    int64_t last_y = tilemap_add_saturating(tilemap_ceil_double_saturating(maximum_y), 3);
    if (first_x < 0)
        first_x = 0;
    if (first_y < 0)
        first_y = 0;
    if (last_x >= tilemap->width)
        last_x = tilemap->width - 1;
    if (last_y >= tilemap->height)
        last_y = tilemap->height - 1;
    if (first_x > last_x || first_y > last_y)
        return 0;
    *view_x = first_x;
    *view_y = first_y;
    *view_width = last_x - first_x + 1;
    *view_height = last_y - first_y + 1;
    return 1;
}

/// @brief Compute a conservative imported-layout viewport at an editor zoom.
/// @param tilemap Imported layout and logical bounds.
/// @param layer Layer supplying offset and parallax.
/// @param canvas_width Positive destination width.
/// @param canvas_height Positive destination height.
/// @param camera_x Unscaled destination-pixel camera X.
/// @param camera_y Unscaled destination-pixel camera Y.
/// @param map_scale Positive finite zoom multiplier.
/// @param view_x Destination for first logical X.
/// @param view_y Destination for first logical Y.
/// @param view_width Destination for visible logical width.
/// @param view_height Destination for visible logical height.
/// @return Nonzero when a nonempty clipped viewport is produced.
static int32_t tilemap_visible_import_region_scaled(rt_tilemap_impl *tilemap,
                                                    tm_layer *layer,
                                                    int64_t canvas_width,
                                                    int64_t canvas_height,
                                                    int64_t camera_x,
                                                    int64_t camera_y,
                                                    double map_scale,
                                                    int64_t *view_x,
                                                    int64_t *view_y,
                                                    int64_t *view_width,
                                                    int64_t *view_height) {
    if (!tilemap || !layer || canvas_width <= 0 || canvas_height <= 0 || map_scale <= 0.0 ||
        !isfinite(map_scale) || !view_x || !view_y || !view_width || !view_height)
        return 0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    tilemap_effective_layer_offset_scaled(
        tilemap, layer, camera_x, camera_y, map_scale, &offset_x, &offset_y);
    double screen_x[2] = {-(double)tilemap->source_frame_width - offset_x / map_scale,
                          ((double)canvas_width - offset_x) / map_scale};
    double screen_y[2] = {-(double)tilemap->source_frame_height - offset_y / map_scale,
                          ((double)canvas_height - offset_y) / map_scale};
    double minimum_x = INFINITY;
    double minimum_y = INFINITY;
    double maximum_x = -INFINITY;
    double maximum_y = -INFINITY;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            double tile_x = 0.0;
            double tile_y = 0.0;
            tilemap_inverse_project_source(tilemap, screen_x[x], screen_y[y], &tile_x, &tile_y);
            minimum_x = fmin(minimum_x, tile_x);
            minimum_y = fmin(minimum_y, tile_y);
            maximum_x = fmax(maximum_x, tile_x);
            maximum_y = fmax(maximum_y, tile_y);
        }
    }
    int64_t first_x = tilemap_add_saturating(tilemap_floor_double_saturating(minimum_x), -3);
    int64_t first_y = tilemap_add_saturating(tilemap_floor_double_saturating(minimum_y), -3);
    int64_t last_x = tilemap_add_saturating(tilemap_ceil_double_saturating(maximum_x), 3);
    int64_t last_y = tilemap_add_saturating(tilemap_ceil_double_saturating(maximum_y), 3);
    if (first_x < 0)
        first_x = 0;
    if (first_y < 0)
        first_y = 0;
    if (last_x >= tilemap->width)
        last_x = tilemap->width - 1;
    if (last_y >= tilemap->height)
        last_y = tilemap->height - 1;
    if (first_x > last_x || first_y > last_y)
        return 0;
    *view_x = first_x;
    *view_y = first_y;
    *view_width = last_x - first_x + 1;
    *view_height = last_y - first_y + 1;
    return 1;
}

/// @brief Test whether a layer can use the simple orthogonal fast path.
/// @param tilemap Map-level imported layout metadata.
/// @param layer Per-layer imported offset and parallax metadata.
/// @return Nonzero when projection, frame geometry, offsets, and parallax all
///         match the native orthogonal layout.
static int tilemap_layer_uses_default_layout(const rt_tilemap_impl *tilemap,
                                             const tm_layer *layer) {
    return tilemap->import_orientation == RT_TILEMAP_IMPORT_ORTHOGONAL &&
           tilemap->import_render_order == RT_TILEMAP_IMPORT_RIGHT_DOWN &&
           tilemap->import_origin_tile_x == 0 && tilemap->import_origin_tile_y == 0 &&
           tilemap->source_frame_width == tilemap->tile_width &&
           tilemap->source_frame_height == tilemap->tile_height &&
           tilemap->import_draw_offset_x == 0 && tilemap->import_draw_offset_y == 0 &&
           tilemap->import_parallax_origin_x == 0.0 && tilemap->import_parallax_origin_y == 0.0 &&
           layer->import_offset_x == 0.0 && layer->import_offset_y == 0.0 &&
           layer->import_parallax_x == 1.0 && layer->import_parallax_y == 1.0;
}

/// @brief Clip a 1-D span [*start, *start + *length) so it fits within [0, limit).
/// @details Adjusts *start and *length in-place: negative starts are advanced to 0 (consuming
///          the leading skipped tiles from *length); spans that extend past @p limit are
///          truncated. Returns 0 and zeroes *length for degenerate inputs (null pointers,
///          non-positive length or limit, span fully outside [0, limit)).
/// @param start In/out inclusive span start.
/// @param length In/out span length.
/// @param limit Exclusive positive upper bound.
/// @return 1 if the clipped span has length > 0; 0 if the span was fully clipped or invalid.
static int32_t tilemap_clip_span_to_bounds(int64_t *start, int64_t *length, int64_t limit) {
    if (!start || !length || *length <= 0 || limit <= 0) {
        if (length)
            *length = 0;
        return 0;
    }
    if (*start < 0) {
        uint64_t skip = tilemap_distance_to_zero(*start);
        if (skip >= (uint64_t)*length) {
            *start = 0;
            *length = 0;
            return 0;
        }
        *start = 0;
        *length -= (int64_t)skip;
    }
    if (*start >= limit) {
        *length = 0;
        return 0;
    }
    int64_t remaining = limit - *start;
    if (*length > remaining)
        *length = remaining;
    return *length > 0;
}

/// @brief Compute the visible tile range along one axis for culled drawing.
/// @details Given the canvas extent, scroll @p offset, @p tile_size and the
///          map @p limit (tile count), returns the first visible tile index
///          and how many tiles are visible, clamped to [0, limit). Saturating
///          arithmetic guards against extreme offsets.
/// @param canvas_size Canvas extent in pixels along this axis.
/// @param offset      Scroll offset in pixels (map-space to screen-space).
/// @param tile_size   Tile size in pixels along this axis.
/// @param limit       Number of tiles along this axis in the map.
/// @param first_out   Out: index of the first visible tile.
/// @param span_out    Out: number of visible tiles.
/// @return Non-zero if any tiles are visible, 0 if fully off-screen.
static int32_t tilemap_visible_span(int64_t canvas_size,
                                    int64_t offset,
                                    int64_t tile_size,
                                    int64_t limit,
                                    int64_t *first_out,
                                    int64_t *span_out) {
    if (!first_out || !span_out || canvas_size <= 0 || tile_size <= 0 || limit <= 0)
        return 0;

    int64_t raw_first = tilemap_floor_div(tilemap_negate_saturating(offset), tile_size);
    int64_t raw_last =
        tilemap_floor_div(tilemap_sub_saturating(canvas_size - 1, offset), tile_size);

    if (raw_last < 0 || raw_first >= limit)
        return 0;
    if (raw_first < 0)
        raw_first = 0;
    if (raw_last >= limit)
        raw_last = limit - 1;
    if (raw_last < raw_first)
        return 0;

    *first_out = raw_first;
    *span_out = raw_last - raw_first + 1;
    return 1;
}

//=============================================================================
// Tilemap Creation
//=============================================================================

/// @brief GC finalizer for Tilemap — release the tileset, per-layer tiles, and per-layer tilesets.
/// @details Also frees every animation's frame-id and duration arrays. The base
///          tile grid is inline in the managed allocation and is not freed separately.
/// @param obj Candidate Tilemap supplied by the runtime finalizer.
static void tilemap_finalize(void *obj) {
    rt_tilemap_impl *tm = tilemap_checked(obj, NULL);
    if (!tm)
        return;
    /* Release base tileset */
    if (tm->tileset)
        rt_heap_release(tm->tileset);
    /* Free per-layer owned tiles and release per-layer tilesets */
    for (int32_t i = 0; i < tm->layer_count; i++) {
        if (tm->layers[i].owns_tiles && tm->layers[i].tiles)
            free(tm->layers[i].tiles);
        if (tm->layers[i].tileset)
            rt_heap_release(tm->layers[i].tileset);
    }
    for (int32_t i = 0; i < tm->tile_anim_count; ++i) {
        free(tm->tile_anims[i].frame_tiles);
        free(tm->tile_anims[i].frame_durations);
    }
}

/// @brief Create a `width × height` tile grid with cells of `tile_width × tile_height` pixels.
///
/// Allocates the tile array inline with the Tilemap struct (single
/// GC allocation). Layer 0 is the implicit "base" layer; additional
/// layers are added via `rt_tilemap_add_layer`. All tiles start at 0
/// (empty). Dimension args are clamped to ≥1 (defaults: tile size 16×16).
/// @param width Logical width in cells; nonpositive values become 1.
/// @param height Logical height in cells; nonpositive values become 1.
/// @param tile_width Logical cell width; nonpositive values become 16.
/// @param tile_height Logical cell height; nonpositive values become 16.
/// @return A caller-owned runtime-managed Tilemap, or `NULL` after a dimension
///         overflow trap or allocation failure.
void *rt_tilemap_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height) {
    if (width <= 0)
        width = 1;
    if (height <= 0)
        height = 1;
    if (tile_width <= 0)
        tile_width = 16;
    if (tile_height <= 0)
        tile_height = 16;

    int64_t tile_count = 0;
    size_t tiles_size = 0;
    if (!tilemap_checked_grid_size(width, height, &tile_count, &tiles_size)) {
        rt_trap("Tilemap: dimensions too large");
        return NULL;
    }

    size_t total_size = sizeof(rt_tilemap_impl) + tiles_size;
    if (total_size < sizeof(rt_tilemap_impl) || total_size > (size_t)INT64_MAX) {
        rt_trap("Tilemap: dimensions too large");
        return NULL;
    }

    rt_tilemap_impl *tilemap =
        (rt_tilemap_impl *)rt_obj_new_i64(RT_TILEMAP_CLASS_ID, (int64_t)total_size);
    if (!tilemap)
        return NULL;

    memset(tilemap, 0, sizeof(rt_tilemap_impl));
    tilemap->width = width;
    tilemap->height = height;
    tilemap->tile_width = tile_width;
    tilemap->tile_height = tile_height;
    tilemap->source_frame_width = tile_width;
    tilemap->source_frame_height = tile_height;
    tilemap->import_projection_height = height;
    tilemap->import_orientation = RT_TILEMAP_IMPORT_ORTHOGONAL;
    tilemap->import_render_order = RT_TILEMAP_IMPORT_RIGHT_DOWN;
    tilemap->import_stagger_axis = 1;
    tilemap->tileset_cols = 0;
    tilemap->tileset_rows = 0;
    tilemap->tile_count = 0;
    tilemap->tileset = NULL;
    tilemap->tiles = (int64_t *)((uint8_t *)tilemap + sizeof(rt_tilemap_impl));

    // Initialize all tiles to 0 (empty)
    memset(tilemap->tiles, 0, tiles_size);

    // Initialize layer 0 (base layer)
    tilemap->layer_count = 1;
    tilemap->collision_layer = 0;
    tilemap->layers[0].tiles = tilemap->tiles;
    tilemap->layers[0].tileset = NULL; // uses base tileset
    tilemap->layers[0].tileset_cols = 0;
    tilemap->layers[0].tileset_rows = 0;
    tilemap->layers[0].tile_count = 0;
    tilemap->layers[0].visible = 1;
    tilemap->layers[0].owns_tiles = 0; // inline allocation
    tilemap->layers[0].import_parallax_x = 1.0;
    tilemap->layers[0].import_parallax_y = 1.0;
    memcpy(tilemap->layers[0].name, "base", 5);

    rt_obj_set_finalizer(tilemap, tilemap_finalize);
    return tilemap;
}

//=============================================================================
// Tilemap Properties
//=============================================================================

// ===========================================================================
// Tilemap property accessors — width/height in tiles, tile size in
// pixels. Each traps on null tilemap (these are programmer errors,
// not runtime conditions).
// ===========================================================================

/// @brief Number of tiles across the map (width). Traps on null.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Logical grid width, or `0` after validation failure.
int64_t rt_tilemap_get_width(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.Width: null tilemap");
    return tilemap ? tilemap->width : 0;
}

/// @brief Number of tiles down the map (height). Traps on null.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Logical grid height, or `0` after validation failure.
int64_t rt_tilemap_get_height(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.Height: null tilemap");
    return tilemap ? tilemap->height : 0;
}

/// @brief Width of a single tile in pixels.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Logical tile width, or `0` after validation failure.
int64_t rt_tilemap_get_tile_width(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.TileWidth: null tilemap");
    return tilemap ? tilemap->tile_width : 0;
}

/// @brief Height of a single tile in pixels.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Logical tile height, or `0` after validation failure.
int64_t rt_tilemap_get_tile_height(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.TileHeight: null tilemap");
    return tilemap ? tilemap->tile_height : 0;
}

//=============================================================================
// Tileset Management
//=============================================================================

/// @brief Configure projection and map-level metadata for imported tile content.
/// @details Validates enum ranges, positive source-frame dimensions, finite
///          skew/parallax values, hex side length, and invertible oblique skew,
///          then atomically stores the supplied layout fields.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param orientation One `RT_TILEMAP_IMPORT_*` orientation.
/// @param origin_tile_x Imported source-grid X represented by logical cell zero.
/// @param origin_tile_y Imported source-grid Y represented by logical cell zero.
/// @param source_frame_width Width of one rendered source frame.
/// @param source_frame_height Height of one rendered source frame.
/// @param draw_offset_x Map-level authored source-space X offset.
/// @param draw_offset_y Map-level authored source-space Y offset.
/// @param render_order One `RT_TILEMAP_IMPORT_*` traversal order.
/// @param stagger_axis Zero for columns or one for rows.
/// @param stagger_even Nonzero to stagger even coordinates.
/// @param hex_side_length Hex side length along the stagger axis.
/// @param skew_x Oblique horizontal skew in pixels per logical tile height.
/// @param skew_y Oblique vertical skew in pixels per logical tile width.
/// @param parallax_origin_x Imported parallax-origin X.
/// @param parallax_origin_y Imported parallax-origin Y.
/// @param projection_height Imported map height used by isometric projection.
/// @return `1` when configuration is accepted, otherwise `0` with no change.
int8_t rt_tilemap_configure_import_layout(void *tilemap_ptr,
                                          int64_t orientation,
                                          int64_t origin_tile_x,
                                          int64_t origin_tile_y,
                                          int64_t source_frame_width,
                                          int64_t source_frame_height,
                                          int64_t draw_offset_x,
                                          int64_t draw_offset_y,
                                          int64_t render_order,
                                          int64_t stagger_axis,
                                          int8_t stagger_even,
                                          int64_t hex_side_length,
                                          double skew_x,
                                          double skew_y,
                                          double parallax_origin_x,
                                          double parallax_origin_y,
                                          int64_t projection_height) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap || orientation < RT_TILEMAP_IMPORT_ORTHOGONAL ||
        orientation > RT_TILEMAP_IMPORT_OBLIQUE || source_frame_width <= 0 ||
        source_frame_height <= 0 || render_order < RT_TILEMAP_IMPORT_RIGHT_DOWN ||
        render_order > RT_TILEMAP_IMPORT_LEFT_UP || (stagger_axis != 0 && stagger_axis != 1) ||
        hex_side_length < 0 || !isfinite(skew_x) || !isfinite(skew_y) ||
        !isfinite(parallax_origin_x) || !isfinite(parallax_origin_y) || projection_height < 0)
        return 0;
    int64_t hex_axis = stagger_axis == 0 ? tilemap->tile_width : tilemap->tile_height;
    if (orientation == RT_TILEMAP_IMPORT_HEXAGONAL && hex_side_length > hex_axis)
        return 0;
    if (orientation == RT_TILEMAP_IMPORT_OBLIQUE &&
        fabs(1.0 - (skew_x / (double)tilemap->tile_height) *
                       (skew_y / (double)tilemap->tile_width)) < 1.0e-12)
        return 0;
    tilemap->import_orientation = (int32_t)orientation;
    tilemap->import_origin_tile_x = origin_tile_x;
    tilemap->import_origin_tile_y = origin_tile_y;
    tilemap->import_projection_height = projection_height;
    tilemap->source_frame_width = source_frame_width;
    tilemap->source_frame_height = source_frame_height;
    tilemap->import_draw_offset_x = draw_offset_x;
    tilemap->import_draw_offset_y = draw_offset_y;
    tilemap->import_render_order = (int32_t)render_order;
    tilemap->import_stagger_axis = (int32_t)stagger_axis;
    tilemap->import_stagger_even = stagger_even ? 1 : 0;
    tilemap->import_hex_side_length = hex_side_length;
    tilemap->import_skew_x = skew_x;
    tilemap->import_skew_y = skew_y;
    tilemap->import_parallax_origin_x = parallax_origin_x;
    tilemap->import_parallax_origin_y = parallax_origin_y;
    return 1;
}

/// @brief Set imported offset and parallax metadata for one layer.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Valid zero-based layer index.
/// @param offset_x Finite authored source-space X offset.
/// @param offset_y Finite authored source-space Y offset.
/// @param parallax_x Finite horizontal parallax factor.
/// @param parallax_y Finite vertical parallax factor.
/// @return `1` when stored, otherwise `0`.
int8_t rt_tilemap_configure_import_layer(void *tilemap_ptr,
                                         int64_t layer,
                                         double offset_x,
                                         double offset_y,
                                         double parallax_x,
                                         double parallax_y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap || layer < 0 || layer >= tilemap->layer_count || !isfinite(offset_x) ||
        !isfinite(offset_y) || !isfinite(parallax_x) || !isfinite(parallax_y))
        return 0;
    tilemap->layers[layer].import_offset_x = offset_x;
    tilemap->layers[layer].import_offset_y = offset_y;
    tilemap->layers[layer].import_parallax_x = parallax_x;
    tilemap->layers[layer].import_parallax_y = parallax_y;
    return 1;
}

/// @brief Override the valid imported base tileset entry count.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_count Positive count not exceeding the derived tileset grid.
/// @return `1` when stored for the base map/layer, otherwise `0`.
int8_t rt_tilemap_set_import_tile_count(void *tilemap_ptr, int64_t tile_count) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap || tile_count <= 0 || tile_count > tilemap->tileset_cols * tilemap->tileset_rows)
        return 0;
    tilemap->tile_count = tile_count;
    tilemap->layers[0].tile_count = tile_count;
    return 1;
}

/// @brief Bind the base tileset Pixels — auto-derives `tileset_cols/rows` from its dimensions.
///
/// The tileset is laid out as a regular grid of `tile_width × tile_height`
/// cells. Tile indices are 1-based (0 means "empty"), reading
/// left-to-right top-to-bottom in the tileset image.
/// @details The input Pixels is cloned, so subsequent caller mutation is not
///          reflected. The old clone is released only after cloning succeeds.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param pixels Valid Pixels image to clone as the base tileset.
void rt_tilemap_set_tileset(void *tilemap_ptr, void *pixels) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.SetTileset: null tilemap");
    if (!tilemap)
        return;
    if (!pixels) {
        rt_trap("Tilemap.SetTileset: null pixels");
        return;
    }
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))) {
        rt_trap("Tilemap.SetTileset: invalid pixels");
        return;
    }

    // Clone the pixels and store, releasing the old tileset first
    void *cloned = rt_pixels_clone(pixels);
    if (!cloned)
        return;

    if (tilemap->tileset)
        rt_heap_release(tilemap->tileset);

    tilemap->tileset = cloned;

    // Calculate tileset dimensions
    int64_t ts_width = rt_pixels_width(cloned);
    int64_t ts_height = rt_pixels_height(cloned);

    tilemap->tileset_cols = ts_width / tilemap->source_frame_width;
    tilemap->tileset_rows = ts_height / tilemap->source_frame_height;
    if (tilemap->tileset_cols > 0 && tilemap->tileset_rows > INT64_MAX / tilemap->tileset_cols) {
        tilemap->tileset_cols = 0;
        tilemap->tileset_rows = 0;
        tilemap->tile_count = 0;
        tilemap->layers[0].tileset_cols = 0;
        tilemap->layers[0].tileset_rows = 0;
        tilemap->layers[0].tile_count = 0;
        return;
    }
    tilemap->tile_count = tilemap->tileset_cols * tilemap->tileset_rows;

    // Sync layer 0 tileset info
    tilemap->layers[0].tileset_cols = tilemap->tileset_cols;
    tilemap->layers[0].tileset_rows = tilemap->tileset_rows;
    tilemap->layers[0].tile_count = tilemap->tile_count;
}

/// @brief Total number of distinct tiles available in the bound tileset (cols × rows).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Valid base tileset entry count, or `0` after validation failure.
int64_t rt_tilemap_get_tile_count(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.TileCount: null tilemap");
    return tilemap ? tilemap->tile_count : 0;
}

//=============================================================================
// Tile Access
//=============================================================================

/// @brief Place tile `tile_index` at grid `(x, y)` on the base layer (silently no-op out of
/// bounds).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param x Zero-based logical column.
/// @param y Zero-based logical row.
/// @param tile_index Tile identifier to store; zero represents empty.
void rt_tilemap_set_tile(void *tilemap_ptr, int64_t x, int64_t y, int64_t tile_index) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.SetTile: null tilemap");
    if (!tilemap)
        return;

    if (x < 0 || x >= tilemap->width || y < 0 || y >= tilemap->height)
        return;

    tilemap->tiles[y * tilemap->width + x] = tile_index;
}

/// @brief Read the tile index at grid `(x, y)`. Returns 0 (empty) for out-of-bounds.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param x Zero-based logical column.
/// @param y Zero-based logical row.
/// @return Stored tile identifier, or `0` for invalid/out-of-bounds access.
int64_t rt_tilemap_get_tile(void *tilemap_ptr, int64_t x, int64_t y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.GetTile: null tilemap");
    if (!tilemap)
        return 0;

    if (x < 0 || x >= tilemap->width || y < 0 || y >= tilemap->height)
        return 0;

    return tilemap->tiles[y * tilemap->width + x];
}

/// @brief Fill the entire base layer with the given tile index.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_index Tile identifier to store in every logical cell.
void rt_tilemap_fill(void *tilemap_ptr, int64_t tile_index) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.Fill: null tilemap");
    if (!tilemap)
        return;

    int64_t count = 0;
    if (!tilemap_checked_grid_size(tilemap->width, tilemap->height, &count, NULL))
        return;
    if (tile_index == 0) {
        if ((uint64_t)count <= (uint64_t)SIZE_MAX / sizeof(*tilemap->tiles))
            memset(tilemap->tiles, 0, (size_t)count * sizeof(*tilemap->tiles));
        return;
    }
    for (int64_t i = 0; i < count; i++)
        tilemap->tiles[i] = tile_index;
}

/// @brief Reset every tile on the base layer to 0 (empty).
/// @param tilemap_ptr Candidate Tilemap handle.
void rt_tilemap_clear(void *tilemap_ptr) {
    rt_tilemap_fill(tilemap_ptr, 0);
}

/// @brief Fill a rectangular region of the base layer with `tile_index`. Bounds-clamped.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param x Requested logical left edge.
/// @param y Requested logical top edge.
/// @param w Requested width, clipped to the map.
/// @param h Requested height, clipped to the map.
/// @param tile_index Tile identifier to store.
void rt_tilemap_fill_rect(
    void *tilemap_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t tile_index) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.FillRect: null tilemap");
    if (!tilemap)
        return;

    if (!tilemap_clip_span_to_bounds(&x, &w, tilemap->width) ||
        !tilemap_clip_span_to_bounds(&y, &h, tilemap->height))
        return;

    int64_t y_end = y + h;
    int64_t x_end = x + w;
    for (int64_t ty = y; ty < y_end; ty++)
        for (int64_t tx = x; tx < x_end; tx++)
            tilemap->tiles[ty * tilemap->width + tx] = tile_index;
}

//=============================================================================
// Rendering
//=============================================================================

/// @brief Callback invoked for each logical cell in visual draw order.
/// @param tile_x Logical cell X.
/// @param tile_y Logical cell Y.
/// @param context Caller-provided traversal state.
typedef void (*tilemap_cell_visitor)(int64_t tile_x, int64_t tile_y, void *context);

/// @brief Visit a clipped rectangle in the visual depth order of the imported map.
/// @details Orthogonal and oblique maps honor Tiled's four render orders.
///          Isometric maps walk screen-space diagonals from top to bottom. For
///          X-staggered maps, non-staggered and staggered half-rows are interleaved;
///          Y-staggered maps use ordinary top-to-bottom rows.
/// @param tilemap Imported orientation and ordering metadata.
/// @param view_x First logical column.
/// @param view_y First logical row.
/// @param view_w Positive logical width.
/// @param view_h Positive logical height.
/// @param visitor Callback invoked once per visited coordinate.
/// @param context Opaque state passed to @p visitor.
static void tilemap_visit_draw_order(const rt_tilemap_impl *tilemap,
                                     int64_t view_x,
                                     int64_t view_y,
                                     int64_t view_w,
                                     int64_t view_h,
                                     tilemap_cell_visitor visitor,
                                     void *context) {
    if (!tilemap || !visitor || view_w <= 0 || view_h <= 0)
        return;

    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_ORTHOGONAL ||
        tilemap->import_orientation == RT_TILEMAP_IMPORT_OBLIQUE) {
        int reverse_x = tilemap->import_render_order == RT_TILEMAP_IMPORT_LEFT_DOWN ||
                        tilemap->import_render_order == RT_TILEMAP_IMPORT_LEFT_UP;
        int reverse_y = tilemap->import_render_order == RT_TILEMAP_IMPORT_RIGHT_UP ||
                        tilemap->import_render_order == RT_TILEMAP_IMPORT_LEFT_UP;
        for (int64_t row = 0; row < view_h; ++row) {
            int64_t tile_y = reverse_y ? view_y + view_h - 1 - row : view_y + row;
            for (int64_t column = 0; column < view_w; ++column) {
                int64_t tile_x = reverse_x ? view_x + view_w - 1 - column : view_x + column;
                visitor(tile_x, tile_y, context);
            }
        }
        return;
    }

    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_ISOMETRIC) {
        int64_t first_sum = view_x + view_y;
        int64_t diagonal_count = view_w + view_h - 1;
        int64_t end_x = view_x + view_w;
        int64_t end_y = view_y + view_h;
        for (int64_t diagonal = 0; diagonal < diagonal_count; ++diagonal) {
            int64_t sum = first_sum + diagonal;
            int64_t first_x = sum - (end_y - 1);
            if (first_x < view_x)
                first_x = view_x;
            int64_t last_x = sum - view_y;
            if (last_x >= end_x)
                last_x = end_x - 1;
            for (int64_t tile_x = first_x; tile_x <= last_x; ++tile_x)
                visitor(tile_x, sum - tile_x, context);
        }
        return;
    }

    int64_t end_x = view_x + view_w;
    int64_t end_y = view_y + view_h;
    for (int64_t tile_y = view_y; tile_y < end_y; ++tile_y) {
        if (tilemap->import_stagger_axis == 0) {
            for (int staggered = 0; staggered <= 1; ++staggered) {
                for (int64_t tile_x = view_x; tile_x < end_x; ++tile_x) {
                    int64_t source_x =
                        tilemap_add_saturating(tile_x, tilemap->import_origin_tile_x);
                    if (tilemap_is_staggered_coordinate(tilemap, source_x) == staggered)
                        visitor(tile_x, tile_y, context);
                }
            }
        } else {
            for (int64_t tile_x = view_x; tile_x < end_x; ++tile_x)
                visitor(tile_x, tile_y, context);
        }
    }
}

/// @brief Immutable state shared by callbacks during one native layer draw.
typedef struct {
    rt_tilemap_impl *tilemap; ///< Borrowed map being rendered.
    void *canvas;             ///< Borrowed destination Canvas.
    tm_layer *layer;          ///< Borrowed layer traversed by the callback.
    void *tileset;            ///< Borrowed Pixels source for this layer.
    int64_t tileset_cols;     ///< Number of source frames per tileset row.
    int64_t tile_count;       ///< Number of complete source frames.
    int64_t source_width;     ///< Source frame width in pixels.
    int64_t source_height;    ///< Source frame height in pixels.
    double layer_offset_x;    ///< Effective native destination X offset.
    double layer_offset_y;    ///< Effective native destination Y offset.
} tilemap_native_draw_context;

/// @brief Draw one imported-layout cell visited by tilemap_visit_draw_order.
/// @param tile_x Logical cell X.
/// @param tile_y Logical cell Y.
/// @param opaque Pointer to `tilemap_native_draw_context`.
static void tilemap_draw_native_cell(int64_t tile_x, int64_t tile_y, void *opaque) {
    tilemap_native_draw_context *context = (tilemap_native_draw_context *)opaque;
    int64_t tile_index = tilemap_resolve_anim_tile_fast(
        context->tilemap, context->layer->tiles[tile_y * context->tilemap->width + tile_x]);
    if (tile_index <= 0 || tile_index > context->tile_count)
        return;

    int64_t source_index = tile_index - 1;
    int64_t source_x =
        tilemap_mul_saturating(source_index % context->tileset_cols, context->source_width);
    int64_t source_y =
        tilemap_mul_saturating(source_index / context->tileset_cols, context->source_height);
    double projected_x = 0.0;
    double projected_y = 0.0;
    tilemap_project_source(context->tilemap, tile_x, tile_y, &projected_x, &projected_y);
    int64_t screen_x = tilemap_round_ties_away_saturating(projected_x + context->layer_offset_x);
    int64_t screen_y = tilemap_round_ties_away_saturating(projected_y + context->layer_offset_y);

    rt_canvas_blit_region(context->canvas,
                          screen_x,
                          screen_y,
                          context->tileset,
                          source_x,
                          source_y,
                          context->source_width,
                          context->source_height);
}

/// @brief Render one tilemap layer over a rectangular region of tile coordinates.
/// @details Iterates over tile rows [view_y, view_y+view_h) and columns
///   [view_x, view_x+view_w), skipping empty (tile_index == 0) or out-of-range
///   tiles. For each valid tile, the source rectangle is computed from the tile index
///   in the tileset image (`ts_x = (ti % cols) * tw`, `ts_y = (ti / cols) * th`) and
///   blitted to the canvas at the screen-space position `(tx * tw + offset_x, ty * th + offset_y)`.
///   All coordinate multiplications use `tilemap_mul_saturating` and additions use
///   `tilemap_add_saturating` to prevent overflow on extreme map sizes. The per-layer
///   tileset (if set) overrides the tilemap's default tileset.
/// @param tilemap Valid implementation providing layout and base tileset.
/// @param tilemap_ptr Original opaque handle; retained for ABI symmetry and unused.
/// @param canvas_ptr Destination Canvas.
/// @param layer Layer whose visible tile array is traversed.
/// @param offset_x Camera/destination X offset.
/// @param offset_y Camera/destination Y offset.
/// @param view_x First logical column.
/// @param view_y First logical row.
/// @param view_w Logical width.
/// @param view_h Logical height.
static void rt_tilemap_draw_region_layer_impl(rt_tilemap_impl *tilemap,
                                              void *tilemap_ptr,
                                              void *canvas_ptr,
                                              tm_layer *layer,
                                              int64_t offset_x,
                                              int64_t offset_y,
                                              int64_t view_x,
                                              int64_t view_y,
                                              int64_t view_w,
                                              int64_t view_h) {
    if (!tilemap || !layer || !canvas_ptr || !layer->visible || !layer->tiles)
        return;

    void *tileset = layer->tileset ? layer->tileset : tilemap->tileset;
    int64_t tileset_cols = layer->tileset ? layer->tileset_cols : tilemap->tileset_cols;
    int64_t tile_count = layer->tileset ? layer->tile_count : tilemap->tile_count;
    if (!tileset || tile_count <= 0 || tileset_cols <= 0)
        return;

    int64_t source_width = tilemap->source_frame_width;
    int64_t source_height = tilemap->source_frame_height;
    double layer_offset_x = 0.0;
    double layer_offset_y = 0.0;
    tilemap_effective_layer_offset(
        tilemap, layer, offset_x, offset_y, &layer_offset_x, &layer_offset_y);

    (void)tilemap_ptr;
    tilemap_native_draw_context context = {tilemap,
                                           canvas_ptr,
                                           layer,
                                           tileset,
                                           tileset_cols,
                                           tile_count,
                                           source_width,
                                           source_height,
                                           layer_offset_x,
                                           layer_offset_y};
    tilemap_visit_draw_order(
        tilemap, view_x, view_y, view_w, view_h, tilemap_draw_native_cell, &context);
}

/// @brief Count non-empty tiles one layer would draw over a clipped tile region.
/// @param tilemap Valid implementation providing animation and base tileset state.
/// @param tilemap_ptr Original opaque handle; unused.
/// @param layer Layer to inspect.
/// @param view_x First logical column.
/// @param view_y First logical row.
/// @param view_w Logical width.
/// @param view_h Logical height.
/// @return Number of visible, in-range, nonempty resolved tile identifiers,
///         saturated to `INT64_MAX`.
static int64_t rt_tilemap_count_drawn_region_layer_impl(rt_tilemap_impl *tilemap,
                                                        void *tilemap_ptr,
                                                        tm_layer *layer,
                                                        int64_t view_x,
                                                        int64_t view_y,
                                                        int64_t view_w,
                                                        int64_t view_h) {
    if (!tilemap || !layer || !layer->visible || !layer->tiles)
        return 0;

    void *tileset = layer->tileset ? layer->tileset : tilemap->tileset;
    int64_t tileset_cols = layer->tileset ? layer->tileset_cols : tilemap->tileset_cols;
    int64_t tile_count = layer->tileset ? layer->tile_count : tilemap->tile_count;
    if (!tileset || tile_count <= 0 || tileset_cols <= 0)
        return 0;

    int64_t count = 0;
    int64_t end_y = tilemap_add_saturating(view_y, view_h);
    int64_t end_x = tilemap_add_saturating(view_x, view_w);
    for (int64_t ty = view_y; ty < end_y; ty++) {
        for (int64_t tx = view_x; tx < end_x; tx++) {
            int64_t tile_index =
                tilemap_resolve_anim_tile_fast(tilemap, layer->tiles[ty * tilemap->width + tx]);
            if (tile_index <= 0 || tile_index > tile_count)
                continue;
            if (count < INT64_MAX)
                count++;
        }
    }
    return count;
}

/// @brief Render the entire tilemap (every visible layer) onto a canvas at `(offset_x, offset_y)`.
///
/// Walks layers in order; layer 0 (base) draws first, followed by
/// each added layer's tiles. Per-tile draws blit the source rect
/// from the tileset. Out-of-canvas tiles are skipped early.
/// @details Imported projections compute a conservative per-layer logical
///          viewport; default orthogonal layouts use direct axis culling.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Destination Canvas.
/// @param offset_x Camera/destination X offset.
/// @param offset_y Camera/destination Y offset.
void rt_tilemap_draw(void *tilemap_ptr, void *canvas_ptr, int64_t offset_x, int64_t offset_y) {
    if (!tilemap_ptr || !canvas_ptr)
        return;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    int default_layout = 1;
    for (int32_t layer = 0; layer < tilemap->layer_count; ++layer) {
        if (!tilemap_layer_uses_default_layout(tilemap, &tilemap->layers[layer])) {
            default_layout = 0;
            break;
        }
    }
    if (!default_layout) {
        int64_t canvas_w = rt_canvas_width(canvas_ptr);
        int64_t canvas_h = rt_canvas_height(canvas_ptr);
        for (int32_t layer = 0; layer < tilemap->layer_count; ++layer) {
            int64_t first_x = 0;
            int64_t first_y = 0;
            int64_t visible_width = 0;
            int64_t visible_height = 0;
            if (!tilemap_visible_import_region(tilemap,
                                               &tilemap->layers[layer],
                                               canvas_w,
                                               canvas_h,
                                               offset_x,
                                               offset_y,
                                               &first_x,
                                               &first_y,
                                               &visible_width,
                                               &visible_height))
                continue;
            rt_tilemap_draw_region_layer_impl(tilemap,
                                              tilemap_ptr,
                                              canvas_ptr,
                                              &tilemap->layers[layer],
                                              offset_x,
                                              offset_y,
                                              first_x,
                                              first_y,
                                              visible_width,
                                              visible_height);
        }
        return;
    }
    int64_t tw = tilemap->tile_width > 0 ? tilemap->tile_width : 1;
    int64_t th = tilemap->tile_height > 0 ? tilemap->tile_height : 1;

    int64_t canvas_w = rt_canvas_width(canvas_ptr);
    int64_t canvas_h = rt_canvas_height(canvas_ptr);
    int64_t first_x = 0;
    int64_t first_y = 0;
    int64_t vis_w = 0;
    int64_t vis_h = 0;
    if (!tilemap_visible_span(canvas_w, offset_x, tw, tilemap->width, &first_x, &vis_w) ||
        !tilemap_visible_span(canvas_h, offset_y, th, tilemap->height, &first_y, &vis_h))
        return;

    rt_tilemap_draw_region(
        tilemap_ptr, canvas_ptr, offset_x, offset_y, first_x, first_y, vis_w, vis_h);
}

/// @brief Render only a sub-rectangle of the tilemap (camera-clipped draw).
///
/// `(view_x, view_y, view_w, view_h)` defines a rectangle in
/// tile coordinates within the tilemap. Saves work for large maps
/// when only a small viewport needs drawing.
/// @details The requested logical spans are clipped to map bounds, then every
///          visible layer is traversed in imported visual order.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Destination Canvas.
/// @param offset_x Camera/destination X offset.
/// @param offset_y Camera/destination Y offset.
/// @param view_x Requested first logical column.
/// @param view_y Requested first logical row.
/// @param view_w Requested logical width.
/// @param view_h Requested logical height.
void rt_tilemap_draw_region(void *tilemap_ptr,
                            void *canvas_ptr,
                            int64_t offset_x,
                            int64_t offset_y,
                            int64_t view_x,
                            int64_t view_y,
                            int64_t view_w,
                            int64_t view_h) {
    if (!tilemap_ptr || !canvas_ptr)
        return;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;

    if (!tilemap_clip_span_to_bounds(&view_x, &view_w, tilemap->width) ||
        !tilemap_clip_span_to_bounds(&view_y, &view_h, tilemap->height))
        return;

    for (int32_t layer = 0; layer < tilemap->layer_count; layer++) {
        rt_tilemap_draw_region_layer_impl(tilemap,
                                          tilemap_ptr,
                                          canvas_ptr,
                                          &tilemap->layers[layer],
                                          offset_x,
                                          offset_y,
                                          view_x,
                                          view_y,
                                          view_w,
                                          view_h);
    }
}

/// @brief Release a temporary runtime object after a retained handoff.
/// @param obj Temporary runtime object; `NULL` is accepted.
static void tilemap_release_temp(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief One open-addressed entry in a per-draw scaled-tile cache.
/// @details The entry owns @c scaled_pixels until the draw releases the cache.
typedef struct {
    void *tileset;       ///< Borrowed tileset identity forming part of the key.
    int64_t tile_index;  ///< One-based tile identifier forming part of the key.
    void *scaled_pixels; ///< Owned scaled Pixels result.
    uint8_t used;        ///< Nonzero when this hash slot is occupied.
} tilemap_scaled_tile_cache_entry;

/// @brief Hash a tileset pointer plus tile id for the scaled-tile cache.
/// @details The cache is per draw call, so pointer identity is sufficient for
///   the tileset portion of the key. The mixed result is used with a power-of-two
///   table mask by tilemap_scaled_cache_find/insert.
/// @param tileset Source tileset identity.
/// @param tile_index One-based source tile identifier.
/// @return Mixed platform-sized hash value.
static size_t tilemap_scaled_cache_hash(void *tileset, int64_t tile_index) {
    uintptr_t ptr = (uintptr_t)tileset;
    uint64_t h = (uint64_t)(ptr >> 4u) ^ (uint64_t)tile_index;
    h ^= h >> 33u;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33u;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33u;
    return (size_t)h;
}

/// @brief Find a cached scaled tile for a tileset/tile-id pair.
/// @param entries Cache entries allocated by rt_tilemap_draw_scaled.
/// @param cap Number of hash slots in @p entries; must be a power of two.
/// @param tileset Source tileset object.
/// @param tile_index One-based tile index in @p tileset.
/// @return Cached Pixels object, or NULL when absent.
static void *tilemap_scaled_cache_find(tilemap_scaled_tile_cache_entry *entries,
                                       size_t cap,
                                       void *tileset,
                                       int64_t tile_index) {
    if (!entries || cap == 0 || !tileset)
        return NULL;
    size_t mask = cap - 1u;
    size_t slot = tilemap_scaled_cache_hash(tileset, tile_index) & mask;
    for (size_t probe = 0; probe < cap; ++probe) {
        tilemap_scaled_tile_cache_entry *entry = &entries[(slot + probe) & mask];
        if (!entry->used)
            return NULL;
        if (entry->tileset == tileset && entry->tile_index == tile_index)
            return entry->scaled_pixels;
    }
    return NULL;
}

/// @brief Insert an already-owned scaled tile into a hash-table slot.
/// @param entries Open-addressed cache storage.
/// @param cap Power-of-two slot count.
/// @param tileset Source tileset identity.
/// @param tile_index One-based source tile identifier.
/// @param scaled_pixels Owned scaled Pixels to place.
/// @return Non-zero on success; zero if the table is full or invalid.
static int tilemap_scaled_cache_place(tilemap_scaled_tile_cache_entry *entries,
                                      size_t cap,
                                      void *tileset,
                                      int64_t tile_index,
                                      void *scaled_pixels) {
    if (!entries || cap == 0 || !tileset || !scaled_pixels)
        return 0;
    size_t mask = cap - 1u;
    size_t slot = tilemap_scaled_cache_hash(tileset, tile_index) & mask;
    for (size_t probe = 0; probe < cap; ++probe) {
        tilemap_scaled_tile_cache_entry *entry = &entries[(slot + probe) & mask];
        if (!entry->used) {
            entry->tileset = tileset;
            entry->tile_index = tile_index;
            entry->scaled_pixels = scaled_pixels;
            entry->used = 1;
            return 1;
        }
    }
    return 0;
}

/// @brief Grow the scaled-tile cache hash table and reinsert existing entries.
/// @param entries In/out cache allocation pointer.
/// @param cap In/out power-of-two slot count; zero selects the initial 64 slots.
/// @return Non-zero on success, zero on allocation failure or capacity overflow.
static int tilemap_scaled_cache_grow(tilemap_scaled_tile_cache_entry **entries, size_t *cap) {
    if (!entries || !cap)
        return 0;
    size_t old_cap = *cap;
    size_t new_cap = old_cap ? old_cap * 2u : 64u;
    if (new_cap < old_cap || new_cap > SIZE_MAX / sizeof(**entries))
        return 0;
    tilemap_scaled_tile_cache_entry *new_entries =
        (tilemap_scaled_tile_cache_entry *)calloc(new_cap, sizeof(**entries));
    if (!new_entries)
        return 0;
    for (size_t i = 0; i < old_cap; ++i) {
        tilemap_scaled_tile_cache_entry *old = &(*entries)[i];
        if (old->used) {
            if (!tilemap_scaled_cache_place(
                    new_entries, new_cap, old->tileset, old->tile_index, old->scaled_pixels)) {
                free(new_entries);
                return 0;
            }
        }
    }
    free(*entries);
    *entries = new_entries;
    *cap = new_cap;
    return 1;
}

/// @brief Insert one scaled tile into the per-draw cache.
/// @details Ownership of @p scaled_pixels transfers to the cache on success.
///          The cache is released at the end of rt_tilemap_draw_scaled.
/// @param entries Pointer to the cache allocation pointer.
/// @param count Pointer to the current entry count.
/// @param cap Pointer to the current allocation capacity.
/// @param tileset Source tileset object.
/// @param tile_index One-based tile index in @p tileset.
/// @param scaled_pixels Scaled Pixels object to cache.
/// @return 1 when cached; 0 on allocation failure or invalid input.
static int tilemap_scaled_cache_insert(tilemap_scaled_tile_cache_entry **entries,
                                       size_t *count,
                                       size_t *cap,
                                       void *tileset,
                                       int64_t tile_index,
                                       void *scaled_pixels) {
    if (!entries || !count || !cap || !tileset || !scaled_pixels)
        return 0;
    size_t next_count = *count + 1u;
    size_t grow_limit = (*cap / 10u) * 7u + ((*cap % 10u) * 7u) / 10u;
    if (next_count < *count || *cap == 0 || next_count > grow_limit) {
        if (!tilemap_scaled_cache_grow(entries, cap))
            return 0;
    }
    if (!tilemap_scaled_cache_place(*entries, *cap, tileset, tile_index, scaled_pixels))
        return 0;
    ++(*count);
    return 1;
}

/// @brief Release every cached scaled tile and free the cache allocation.
/// @param entries Cache entries allocated during rt_tilemap_draw_scaled.
/// @param cap Number of hash slots in @p entries.
static void tilemap_scaled_cache_release(tilemap_scaled_tile_cache_entry *entries, size_t cap) {
    if (!entries)
        return;
    for (size_t i = 0; i < cap; ++i) {
        if (entries[i].used)
            tilemap_release_temp(entries[i].scaled_pixels);
    }
    free(entries);
}

/// @brief Shared state for scaled-cell callbacks during one layer traversal.
typedef struct {
    rt_tilemap_impl *tilemap; ///< Borrowed map being rendered.
    void *canvas;             ///< Borrowed destination Canvas.
    tm_layer *layer;          ///< Borrowed layer traversed by the callback.
    void *tileset;            ///< Borrowed Pixels source for this layer.
    int64_t tileset_cols;     ///< Number of source frames per tileset row.
    int64_t tile_count;       ///< Number of complete source frames.
    int64_t source_width;     ///< Unscaled source frame width.
    int64_t source_height;    ///< Unscaled source frame height.
    int64_t destination_width; ///< Scaled source-frame draw width.
    int64_t destination_height; ///< Scaled source-frame draw height.
    int64_t logical_destination_width; ///< Scaled logical cell width.
    int64_t logical_destination_height; ///< Scaled logical cell height.
    int64_t camera_x;         ///< Destination-pixel camera X.
    int64_t camera_y;         ///< Destination-pixel camera Y.
    double map_scale;         ///< Positive editor zoom multiplier.
    double layer_offset_x;    ///< Effective scaled destination X offset.
    double layer_offset_y;    ///< Effective scaled destination Y offset.
    int default_layout;       ///< Nonzero for direct logical-grid placement.
    tilemap_scaled_tile_cache_entry **cache; ///< Shared cache allocation address.
    size_t *cache_count;      ///< Shared occupied-entry count.
    size_t *cache_capacity;   ///< Shared hash-slot capacity.
} tilemap_scaled_draw_context;

/// @brief Scale and draw one source-frame cell at its projected zoomed position.
/// @details Reuses one scaled Pixels allocation per `(tileset,tile_id)` pair
///          across the current draw call.
/// @param tile_x Logical cell X.
/// @param tile_y Logical cell Y.
/// @param opaque Pointer to `tilemap_scaled_draw_context`.
static void tilemap_draw_scaled_cell(int64_t tile_x, int64_t tile_y, void *opaque) {
    tilemap_scaled_draw_context *context = (tilemap_scaled_draw_context *)opaque;
    int64_t tile_index = tilemap_resolve_anim_tile_fast(
        context->tilemap, context->layer->tiles[tile_y * context->tilemap->width + tile_x]);
    if (tile_index <= 0 || tile_index > context->tile_count)
        return;

    void *scaled = tilemap_scaled_cache_find(
        *context->cache, *context->cache_capacity, context->tileset, tile_index);
    if (!scaled) {
        int64_t source_index = tile_index - 1;
        int64_t source_x =
            tilemap_mul_saturating(source_index % context->tileset_cols, context->source_width);
        int64_t source_y =
            tilemap_mul_saturating(source_index / context->tileset_cols, context->source_height);
        void *tile = rt_pixels_new(context->source_width, context->source_height);
        if (!tile)
            return;
        rt_pixels_copy(tile,
                       0,
                       0,
                       context->tileset,
                       source_x,
                       source_y,
                       context->source_width,
                       context->source_height);
        scaled = rt_pixels_scale(tile, context->destination_width, context->destination_height);
        tilemap_release_temp(tile);
        if (scaled && !tilemap_scaled_cache_insert(context->cache,
                                                   context->cache_count,
                                                   context->cache_capacity,
                                                   context->tileset,
                                                   tile_index,
                                                   scaled)) {
            tilemap_release_temp(scaled);
            scaled = NULL;
        }
    }
    if (!scaled)
        return;

    int64_t screen_x = 0;
    int64_t screen_y = 0;
    if (context->default_layout) {
        screen_x = tilemap_add_saturating(
            tilemap_mul_saturating(tile_x, context->logical_destination_width), context->camera_x);
        screen_y = tilemap_add_saturating(
            tilemap_mul_saturating(tile_y, context->logical_destination_height), context->camera_y);
    } else {
        double projected_x = 0.0;
        double projected_y = 0.0;
        tilemap_project_source(context->tilemap, tile_x, tile_y, &projected_x, &projected_y);
        screen_x = tilemap_round_ties_away_saturating(projected_x * context->map_scale +
                                                      context->layer_offset_x);
        screen_y = tilemap_round_ties_away_saturating(projected_y * context->map_scale +
                                                      context->layer_offset_y);
    }
    rt_canvas_blit(context->canvas, screen_x, screen_y, scaled);
}

/// @brief Draw the tilemap using scaled destination tile cells.
/// @details This editor-oriented path scales complete imported source frames,
///          projection geometry, authored offsets, and traversal order. Camera
///          offsets remain destination-pixel values. Scale 100 delegates to the
///          native fast path. Renderers that need batching should use
///          TilemapRenderer2D.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Destination Canvas.
/// @param offset_x Unscaled destination-pixel camera X.
/// @param offset_y Unscaled destination-pixel camera Y.
/// @param scale_percent Positive map zoom percentage.
void rt_tilemap_draw_scaled(void *tilemap_ptr,
                            void *canvas_ptr,
                            int64_t offset_x,
                            int64_t offset_y,
                            int64_t scale_percent) {
    if (scale_percent <= 0 || !tilemap_ptr || !canvas_ptr)
        return;
    if (scale_percent == 100) {
        rt_tilemap_draw(tilemap_ptr, canvas_ptr, offset_x, offset_y);
        return;
    }

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;

    tilemap_scaled_tile_cache_entry *scaled_cache = NULL;
    size_t scaled_cache_count = 0;
    size_t scaled_cache_cap = 0;

    int64_t logical_dst_w = tilemap_scale_dimension(tilemap->tile_width, scale_percent);
    int64_t logical_dst_h = tilemap_scale_dimension(tilemap->tile_height, scale_percent);
    int64_t source_dst_w = tilemap_scale_dimension(tilemap->source_frame_width, scale_percent);
    int64_t source_dst_h = tilemap_scale_dimension(tilemap->source_frame_height, scale_percent);
    double map_scale = (double)scale_percent / 100.0;

    for (int32_t li = 0; li < tilemap->layer_count; li++) {
        tm_layer *layer = &tilemap->layers[li];
        if (!layer->visible || !layer->tiles)
            continue;
        void *tileset = layer->tileset ? layer->tileset : tilemap->tileset;
        int64_t tileset_cols = layer->tileset ? layer->tileset_cols : tilemap->tileset_cols;
        int64_t tile_count = layer->tileset ? layer->tile_count : tilemap->tile_count;
        if (!tileset || tile_count <= 0 || tileset_cols <= 0)
            continue;

        int default_layout = tilemap_layer_uses_default_layout(tilemap, layer);
        int64_t first_x = 0;
        int64_t first_y = 0;
        int64_t visible_width = 0;
        int64_t visible_height = 0;
        if (default_layout) {
            if (!tilemap_visible_span(rt_canvas_width(canvas_ptr),
                                      offset_x,
                                      logical_dst_w,
                                      tilemap->width,
                                      &first_x,
                                      &visible_width) ||
                !tilemap_visible_span(rt_canvas_height(canvas_ptr),
                                      offset_y,
                                      logical_dst_h,
                                      tilemap->height,
                                      &first_y,
                                      &visible_height))
                continue;
        } else if (!tilemap_visible_import_region_scaled(tilemap,
                                                         layer,
                                                         rt_canvas_width(canvas_ptr),
                                                         rt_canvas_height(canvas_ptr),
                                                         offset_x,
                                                         offset_y,
                                                         map_scale,
                                                         &first_x,
                                                         &first_y,
                                                         &visible_width,
                                                         &visible_height)) {
            continue;
        }

        double layer_offset_x = 0.0;
        double layer_offset_y = 0.0;
        tilemap_effective_layer_offset_scaled(
            tilemap, layer, offset_x, offset_y, map_scale, &layer_offset_x, &layer_offset_y);
        tilemap_scaled_draw_context context = {tilemap,
                                               canvas_ptr,
                                               layer,
                                               tileset,
                                               tileset_cols,
                                               tile_count,
                                               tilemap->source_frame_width,
                                               tilemap->source_frame_height,
                                               source_dst_w,
                                               source_dst_h,
                                               logical_dst_w,
                                               logical_dst_h,
                                               offset_x,
                                               offset_y,
                                               map_scale,
                                               layer_offset_x,
                                               layer_offset_y,
                                               default_layout,
                                               &scaled_cache,
                                               &scaled_cache_count,
                                               &scaled_cache_cap};
        tilemap_visit_draw_order(tilemap,
                                 first_x,
                                 first_y,
                                 visible_width,
                                 visible_height,
                                 tilemap_draw_scaled_cell,
                                 &context);
    }
    tilemap_scaled_cache_release(scaled_cache, scaled_cache_cap);
}

/// @brief Count non-empty, drawable tiles in a tile-coordinate sub-region.
/// @details Counts every visible layer after resolving animation and rejecting
///          identifiers outside that layer's effective tileset. The region is clipped.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param view_x Requested first logical column.
/// @param view_y Requested first logical row.
/// @param view_w Requested logical width.
/// @param view_h Requested logical height.
/// @return Saturating total drawable-cell count, or `0` for invalid/empty input.
int64_t rt_tilemap_count_drawn_region(
    void *tilemap_ptr, int64_t view_x, int64_t view_y, int64_t view_w, int64_t view_h) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;

    if (!tilemap_clip_span_to_bounds(&view_x, &view_w, tilemap->width) ||
        !tilemap_clip_span_to_bounds(&view_y, &view_h, tilemap->height))
        return 0;

    int64_t total = 0;
    for (int32_t layer = 0; layer < tilemap->layer_count; layer++) {
        int64_t count = rt_tilemap_count_drawn_region_layer_impl(
            tilemap, tilemap_ptr, &tilemap->layers[layer], view_x, view_y, view_w, view_h);
        if (count > INT64_MAX - total)
            total = INT64_MAX;
        else
            total += count;
    }
    return total;
}

/// @brief Count non-empty, drawable tiles visible in a canvas-sized viewport.
/// @details Uses the same default or imported per-layer culling geometry as
///          native drawing without allocating or blitting Pixels.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Canvas supplying viewport dimensions.
/// @param offset_x Camera/destination X offset.
/// @param offset_y Camera/destination Y offset.
/// @return Saturating number of drawable cells in the computed viewport.
int64_t rt_tilemap_count_drawn_visible(void *tilemap_ptr,
                                       void *canvas_ptr,
                                       int64_t offset_x,
                                       int64_t offset_y) {
    if (!tilemap_ptr || !canvas_ptr)
        return 0;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;

    int default_layout = 1;
    for (int32_t layer = 0; layer < tilemap->layer_count; ++layer) {
        if (!tilemap_layer_uses_default_layout(tilemap, &tilemap->layers[layer])) {
            default_layout = 0;
            break;
        }
    }
    if (!default_layout) {
        int64_t total = 0;
        for (int32_t layer = 0; layer < tilemap->layer_count; ++layer) {
            int64_t first_x = 0;
            int64_t first_y = 0;
            int64_t visible_width = 0;
            int64_t visible_height = 0;
            if (!tilemap_visible_import_region(tilemap,
                                               &tilemap->layers[layer],
                                               rt_canvas_width(canvas_ptr),
                                               rt_canvas_height(canvas_ptr),
                                               offset_x,
                                               offset_y,
                                               &first_x,
                                               &first_y,
                                               &visible_width,
                                               &visible_height))
                continue;
            int64_t count = rt_tilemap_count_drawn_region_layer_impl(tilemap,
                                                                     tilemap_ptr,
                                                                     &tilemap->layers[layer],
                                                                     first_x,
                                                                     first_y,
                                                                     visible_width,
                                                                     visible_height);
            total = count > INT64_MAX - total ? INT64_MAX : total + count;
        }
        return total;
    }

    int64_t tw = tilemap->tile_width > 0 ? tilemap->tile_width : 1;
    int64_t th = tilemap->tile_height > 0 ? tilemap->tile_height : 1;
    int64_t canvas_w = rt_canvas_width(canvas_ptr);
    int64_t canvas_h = rt_canvas_height(canvas_ptr);
    int64_t first_x = 0;
    int64_t first_y = 0;
    int64_t vis_w = 0;
    int64_t vis_h = 0;
    if (!tilemap_visible_span(canvas_w, offset_x, tw, tilemap->width, &first_x, &vis_w) ||
        !tilemap_visible_span(canvas_h, offset_y, th, tilemap->height, &first_y, &vis_h))
        return 0;
    return rt_tilemap_count_drawn_region(tilemap_ptr, first_x, first_y, vis_w, vis_h);
}

/// @brief Count drawable tiles visible in a scaled canvas viewport.
/// @details Mirrors scaled drawing's per-layer culling without creating scaled
///          Pixels. A scale of 100 delegates to the native visibility counter.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Canvas supplying viewport dimensions.
/// @param offset_x Unscaled destination-pixel camera X.
/// @param offset_y Unscaled destination-pixel camera Y.
/// @param scale_percent Positive zoom percentage.
/// @return Saturating number of drawable cells, or `0` for invalid input.
int64_t rt_tilemap_count_drawn_visible_scaled(void *tilemap_ptr,
                                              void *canvas_ptr,
                                              int64_t offset_x,
                                              int64_t offset_y,
                                              int64_t scale_percent) {
    if (!tilemap_ptr || !canvas_ptr || scale_percent <= 0)
        return 0;
    if (scale_percent == 100)
        return rt_tilemap_count_drawn_visible(tilemap_ptr, canvas_ptr, offset_x, offset_y);
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    int64_t logical_dst_w = tilemap_scale_dimension(tilemap->tile_width, scale_percent);
    int64_t logical_dst_h = tilemap_scale_dimension(tilemap->tile_height, scale_percent);
    double map_scale = (double)scale_percent / 100.0;
    int64_t total = 0;
    for (int32_t layer_index = 0; layer_index < tilemap->layer_count; ++layer_index) {
        tm_layer *layer = &tilemap->layers[layer_index];
        int64_t first_x = 0;
        int64_t first_y = 0;
        int64_t visible_width = 0;
        int64_t visible_height = 0;
        if (tilemap_layer_uses_default_layout(tilemap, layer)) {
            if (!tilemap_visible_span(rt_canvas_width(canvas_ptr),
                                      offset_x,
                                      logical_dst_w,
                                      tilemap->width,
                                      &first_x,
                                      &visible_width) ||
                !tilemap_visible_span(rt_canvas_height(canvas_ptr),
                                      offset_y,
                                      logical_dst_h,
                                      tilemap->height,
                                      &first_y,
                                      &visible_height))
                continue;
        } else if (!tilemap_visible_import_region_scaled(tilemap,
                                                         layer,
                                                         rt_canvas_width(canvas_ptr),
                                                         rt_canvas_height(canvas_ptr),
                                                         offset_x,
                                                         offset_y,
                                                         map_scale,
                                                         &first_x,
                                                         &first_y,
                                                         &visible_width,
                                                         &visible_height)) {
            continue;
        }
        int64_t count = rt_tilemap_count_drawn_region_layer_impl(
            tilemap, tilemap_ptr, layer, first_x, first_y, visible_width, visible_height);
        total = count > INT64_MAX - total ? INT64_MAX : total + count;
    }
    return total;
}

/// @brief Convert scaled screen coordinates to tile coordinates and return a result map.
/// @details Uses base-layer effective offsets and either scaled orthogonal floor
///          division or exact imported inverse selection. The returned map always
///          contains `tileX`, `tileY`, `tile`, `scalePercent`, and `inBounds`.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param screen_x Destination-space X to test.
/// @param screen_y Destination-space Y to test.
/// @param offset_x Unscaled destination-pixel camera X.
/// @param offset_y Unscaled destination-pixel camera Y.
/// @param scale_percent Requested zoom percentage.
/// @return A caller-owned result Map; invalid inputs yield zero coordinates/tile
///         with `inBounds` false.
void *rt_tilemap_hit_test_scaled(void *tilemap_ptr,
                                 int64_t screen_x,
                                 int64_t screen_y,
                                 int64_t offset_x,
                                 int64_t offset_y,
                                 int64_t scale_percent) {
    void *result = rt_map_new();
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    int64_t tx = 0;
    int64_t ty = 0;
    int8_t in_bounds = 0;
    int64_t tile = 0;
    if (tilemap && scale_percent > 0) {
        tm_layer *base_layer = &tilemap->layers[0];
        if (tilemap_layer_uses_default_layout(tilemap, base_layer)) {
            int64_t dst_w = tilemap_scale_dimension(tilemap->tile_width, scale_percent);
            int64_t dst_h = tilemap_scale_dimension(tilemap->tile_height, scale_percent);
            tx = tilemap_floor_div(tilemap_sub_saturating(screen_x, offset_x), dst_w);
            ty = tilemap_floor_div(tilemap_sub_saturating(screen_y, offset_y), dst_h);
        } else {
            double map_scale = (double)scale_percent / 100.0;
            double layer_offset_x = 0.0;
            double layer_offset_y = 0.0;
            tilemap_effective_layer_offset_scaled(tilemap,
                                                  base_layer,
                                                  offset_x,
                                                  offset_y,
                                                  map_scale,
                                                  &layer_offset_x,
                                                  &layer_offset_y);
            double projected_x = ((double)screen_x - layer_offset_x) / map_scale;
            double projected_y = ((double)screen_y - layer_offset_y) / map_scale;
            tilemap_projected_pixel_to_cell(tilemap, projected_x, projected_y, &tx, &ty);
        }
        in_bounds = (tx >= 0 && ty >= 0 && tx < tilemap->width && ty < tilemap->height) ? 1 : 0;
        if (in_bounds)
            tile = rt_tilemap_get_tile(tilemap_ptr, tx, ty);
    }
    rt_map_set_int(result, rt_const_cstr("tileX"), tx);
    rt_map_set_int(result, rt_const_cstr("tileY"), ty);
    rt_map_set_int(result, rt_const_cstr("tile"), tile);
    rt_map_set_int(result, rt_const_cstr("scalePercent"), scale_percent);
    rt_map_set_bool(result, rt_const_cstr("inBounds"), in_bounds);
    return result;
}

//=============================================================================
// Utility
//=============================================================================

// ===========================================================================
// Coordinate conversion — convert between pixel space and tile-grid space.
// Each pair (`*_to_tile_x/y`, `*_to_pixel_x/y`) divides or multiplies by
// the tile dimensions. Vector versions (`pixel_to_tile`, `tile_to_pixel`)
// convert both axes in one call.
// ===========================================================================

/// @brief Convert pixel `(px, py)` to tile-grid coordinates, written to `*tx, *ty`.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param pixel_x Projected source-space pixel X.
/// @param pixel_y Projected source-space pixel Y.
/// @param tile_x Required destination for logical tile X.
/// @param tile_y Required destination for logical tile Y.
void rt_tilemap_pixel_to_tile(
    void *tilemap_ptr, int64_t pixel_x, int64_t pixel_y, int64_t *tile_x, int64_t *tile_y) {
    if (!tilemap_ptr || !tile_x || !tile_y)
        return;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_ORTHOGONAL &&
        tilemap->import_origin_tile_x == 0 && tilemap->import_origin_tile_y == 0) {
        *tile_x = tilemap_floor_div(pixel_x, tilemap->tile_width);
        *tile_y = tilemap_floor_div(pixel_y, tilemap->tile_height);
        return;
    }
    tilemap_projected_pixel_to_cell(tilemap, (double)pixel_x, (double)pixel_y, tile_x, tile_y);
}

/// @brief X-axis: convert pixel coordinate to tile-grid column.
/// @details For projected layouts this evaluates the inverse at Y equal to zero;
///          use `rt_tilemap_pixel_to_tile()` when both coordinates are available.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param pixel_x Source-space pixel X.
/// @return Logical tile X, or `0` for an invalid map.
int64_t rt_tilemap_to_tile_x(void *tilemap_ptr, int64_t pixel_x) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_ORTHOGONAL &&
        tilemap->import_origin_tile_x == 0)
        return tilemap_floor_div(pixel_x, tilemap->tile_width);
    int64_t tile_x = 0;
    int64_t tile_y = 0;
    tilemap_projected_pixel_to_cell(tilemap, (double)pixel_x, 0.0, &tile_x, &tile_y);
    return tile_x;
}

/// @brief Y-axis: convert pixel coordinate to tile-grid row.
/// @details For projected layouts this evaluates the inverse at X equal to zero;
///          use `rt_tilemap_pixel_to_tile()` when both coordinates are available.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param pixel_y Source-space pixel Y.
/// @return Logical tile Y, or `0` for an invalid map.
int64_t rt_tilemap_to_tile_y(void *tilemap_ptr, int64_t pixel_y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    if (tilemap->import_orientation == RT_TILEMAP_IMPORT_ORTHOGONAL &&
        tilemap->import_origin_tile_y == 0)
        return tilemap_floor_div(pixel_y, tilemap->tile_height);
    int64_t tile_x = 0;
    int64_t tile_y = 0;
    tilemap_projected_pixel_to_cell(tilemap, 0.0, (double)pixel_y, &tile_x, &tile_y);
    return tile_y;
}

/// @brief Convert (tile_x, tile_y) grid coordinates to top-left pixel coordinates of that cell.
/// Writes the result into the provided out-parameters; no-ops on null inputs.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_x Logical tile X.
/// @param tile_y Logical tile Y.
/// @param pixel_x Required destination for projected source-space X.
/// @param pixel_y Required destination for projected source-space Y.
void rt_tilemap_tile_to_pixel(
    void *tilemap_ptr, int64_t tile_x, int64_t tile_y, int64_t *pixel_x, int64_t *pixel_y) {
    if (!tilemap_ptr || !pixel_x || !pixel_y)
        return;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    double projected_x = 0.0;
    double projected_y = 0.0;
    tilemap_project_source(tilemap, tile_x, tile_y, &projected_x, &projected_y);
    *pixel_x = tilemap_round_ties_away_saturating(projected_x);
    *pixel_y = tilemap_round_ties_away_saturating(projected_y);
}

/// @brief Project a tile column's X coordinate with tile Y fixed at zero.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_x Logical tile X.
/// @return Rounded projected source-space X, or `0` for an invalid map.
int64_t rt_tilemap_to_pixel_x(void *tilemap_ptr, int64_t tile_x) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    double pixel_x = 0.0;
    double pixel_y = 0.0;
    tilemap_project_source(tilemap, tile_x, 0, &pixel_x, &pixel_y);
    return tilemap_round_ties_away_saturating(pixel_x);
}

/// @brief Project a tile row's Y coordinate with tile X fixed at zero.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_y Logical tile Y.
/// @return Rounded projected source-space Y, or `0` for an invalid map.
int64_t rt_tilemap_to_pixel_y(void *tilemap_ptr, int64_t tile_y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    double pixel_x = 0.0;
    double pixel_y = 0.0;
    tilemap_project_source(tilemap, 0, tile_y, &pixel_x, &pixel_y);
    return tilemap_round_ties_away_saturating(pixel_y);
}

//=============================================================================
// Tile Collision
//=============================================================================

/// @brief Tag a tile id with a collision type (e.g. SOLID). Out-of-range ids are silently ignored.
/// Collision is keyed on the raw tile id, not the layer; one table is shared by every layer.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_id Positive raw tile identifier below `MAX_TILE_COLLISION_IDS`.
/// @param coll_type `NONE`, `SOLID`, or `ONE_WAY_UP`.
void rt_tilemap_set_collision(void *tilemap_ptr, int64_t tile_id, int64_t coll_type) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.SetCollision: null tilemap");
    if (!tilemap)
        return;
    if (tile_id <= 0 || tile_id >= MAX_TILE_COLLISION_IDS)
        return;
    if (coll_type != RT_TILE_COLLISION_NONE && coll_type != RT_TILE_COLLISION_SOLID &&
        coll_type != RT_TILE_COLLISION_ONE_WAY_UP)
        return;
    tilemap->collision[tile_id] = (int8_t)coll_type;
}

/// @brief Read the collision type previously set for a tile id; 0 (NONE) for unset/out-of-range
/// ids.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_id Raw tile identifier.
/// @return Stored collision enum, or `RT_TILE_COLLISION_NONE` when invalid/unset.
int64_t rt_tilemap_get_collision(void *tilemap_ptr, int64_t tile_id) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, "Tilemap.GetCollision: null tilemap");
    if (!tilemap)
        return 0;
    if (tile_id <= 0 || tile_id >= MAX_TILE_COLLISION_IDS)
        return 0;
    return tilemap->collision[tile_id];
}

/// @brief Sample the designated collision layer at a pixel coordinate; returns 1 if SOLID.
/// Collision is keyed by the base tile id stored in the map. Animated tiles
/// may change their rendered frame, but their collision remains stable.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param pixel_x Logical orthogonal world-pixel X.
/// @param pixel_y Logical orthogonal world-pixel Y.
/// @return `1` only for an in-bounds cell tagged solid; otherwise `0`.
int8_t rt_tilemap_is_solid_at(void *tilemap_ptr, int64_t pixel_x, int64_t pixel_y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    int64_t tx = tilemap_floor_div(pixel_x, tilemap->tile_width);
    int64_t ty = tilemap_floor_div(pixel_y, tilemap->tile_height);
    if (tx < 0 || tx >= tilemap->width || ty < 0 || ty >= tilemap->height)
        return 0;
    int32_t cl = tilemap->collision_layer;
    if (cl < 0 || cl >= tilemap->layer_count || !tilemap->layers[cl].tiles)
        return 0;
    int64_t tile_id = tilemap->layers[cl].tiles[ty * tilemap->width + tx];
    if (tile_id <= 0 || tile_id >= MAX_TILE_COLLISION_IDS)
        return 0;
    return tilemap->collision[tile_id] == TILE_COLLISION_SOLID ? 1 : 0;
}

/// @brief Resolve an AABB against solid tiles. Returns 1 if any collision occurred.
/// Updates the position (out_x, out_y) and velocity (out_vx, out_vy) in-place.
/// @details Uses up to four separation passes against the designated logical
///          collision layer. Solid tiles resolve on the shallowest axis;
///          one-way-up tiles resolve only while descending across their top.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param body_ptr Physics2D body whose public position/velocity state is updated.
/// @return `1` when at least one collision is resolved, otherwise `0`.
int8_t rt_tilemap_collide_body(void *tilemap_ptr, void *body_ptr) {
    if (!tilemap_ptr || !body_ptr)
        return 0;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;

    int64_t tw = tilemap->tile_width;
    int64_t th = tilemap->tile_height;
    int8_t collided = 0;

    // Use the designated collision layer's tile grid
    int32_t cl = tilemap->collision_layer;
    if (cl < 0 || cl >= tilemap->layer_count || !tilemap->layers[cl].tiles)
        return 0;
    int64_t *coll_tiles = tilemap->layers[cl].tiles;

    // Read body state via the public physics2d API (avoids fragile struct cast)
    double bx = rt_physics2d_body_x(body_ptr);
    double by = rt_physics2d_body_y(body_ptr);
    double bw = rt_physics2d_body_w(body_ptr);
    double bh = rt_physics2d_body_h(body_ptr);
    double bvx = rt_physics2d_body_vx(body_ptr);
    double bvy = rt_physics2d_body_vy(body_ptr);
    double prev_by = rt_physics2d_body_prev_y(body_ptr);
    if (!isfinite(bvx))
        bvx = 0.0;
    if (!isfinite(bvy))
        bvy = 0.0;
    if (!isfinite(prev_by))
        prev_by = by;

    if (bw <= 0.0 || bh <= 0.0)
        return 0;

    for (int pass = 0; pass < 4; pass++) {
        double right_d = bx + bw - 1.0;
        double bottom_d = by + bh - 1.0;
        int64_t bx_i = 0;
        int64_t by_i = 0;
        int64_t right_i = 0;
        int64_t bottom_i = 0;
        if (!tilemap_double_to_i64_sat(bx, &bx_i) || !tilemap_double_to_i64_sat(by, &by_i) ||
            !tilemap_double_to_i64_sat(right_d, &right_i) ||
            !tilemap_double_to_i64_sat(bottom_d, &bottom_i))
            break;

        int64_t left = tilemap_floor_div(bx_i, tw);
        int64_t right = tilemap_floor_div(right_i, tw);
        int64_t top = tilemap_floor_div(by_i, th);
        int64_t bottom = tilemap_floor_div(bottom_i, th);

        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        if (right >= tilemap->width)
            right = tilemap->width - 1;
        if (bottom >= tilemap->height)
            bottom = tilemap->height - 1;
        if (left > right || top > bottom)
            break;

        int8_t pass_collided = 0;
        for (int64_t ty = top; ty <= bottom; ty++) {
            for (int64_t tx = left; tx <= right; tx++) {
                int64_t tile_id = coll_tiles[ty * tilemap->width + tx];
                if (tile_id < 0 || tile_id >= MAX_TILE_COLLISION_IDS)
                    continue;
                int8_t ctype = tilemap->collision[tile_id];
                if (ctype == TILE_COLLISION_NONE)
                    continue;

                double tile_x1 = (double)tilemap_mul_saturating(tx, tw);
                double tile_y1 = (double)tilemap_mul_saturating(ty, th);
                double tile_x2 = tile_x1 + (double)tw;
                double tile_y2 = tile_y1 + (double)th;

                double bx1 = bx;
                double by1 = by;
                double bx2 = bx + bw;
                double by2 = by + bh;

                if (bx2 <= tile_x1 || bx1 >= tile_x2 || by2 <= tile_y1 || by1 >= tile_y2)
                    continue;

                if (ctype == TILE_COLLISION_ONE_WAY) {
                    double prev_bottom = prev_by + bh;
                    if (bvy <= 0.0 || prev_bottom > tile_y1 + 1e-6)
                        continue;
                    by = tile_y1 - bh;
                    bvy = 0.0;
                    collided = 1;
                    pass_collided = 1;
                    continue;
                }

                double ox = (bx2 < tile_x2) ? (bx2 - tile_x1) : (tile_x2 - bx1);
                double oy = (by2 < tile_y2) ? (by2 - tile_y1) : (tile_y2 - by1);

                if (ox < oy) {
                    if (bx1 + bw * 0.5 < tile_x1 + (double)tw * 0.5)
                        bx = tile_x1 - bw;
                    else
                        bx = tile_x2;
                    bvx = 0.0;
                } else {
                    if (by1 + bh * 0.5 < tile_y1 + (double)th * 0.5)
                        by = tile_y1 - bh;
                    else
                        by = tile_y2;
                    bvy = 0.0;
                }
                collided = 1;
                pass_collided = 1;
            }
        }
        if (!pass_collided)
            break;
    }

    if (collided) {
        rt_physics2d_body_set_pos(body_ptr, bx, by);
        rt_physics2d_body_set_vel(body_ptr, bvx, bvy);
    }

    return collided;
}

//=============================================================================
// Layer Management
//=============================================================================

/// @brief Append a new tile layer (allocating its own zeroed grid) and return its index.
/// Names must fit in the fixed 31-byte layer name slot; layers default to visible with no per-layer
/// tileset.
/// Returns -1 on null input, on hitting `TM_MAX_LAYERS`, or on allocation failure.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param name Optional borrowed runtime string; null creates an empty name.
/// @return New zero-based layer index, or `-1` on validation/capacity/allocation failure.
int64_t rt_tilemap_add_layer(void *tilemap_ptr, rt_string name) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return -1;

    if (tilemap->layer_count >= TM_MAX_LAYERS)
        return -1;

    size_t grid_size = 0;
    if (!tilemap_checked_grid_size(tilemap->width, tilemap->height, NULL, &grid_size)) {
        rt_trap("Tilemap.AddLayer: dimensions too large");
        return -1;
    }

    int32_t idx = tilemap->layer_count;
    char layer_name[sizeof(tilemap->layers[0].name)];
    memset(layer_name, 0, sizeof(layer_name));
    if (name) {
        const char *cstr = rt_string_cstr(name);
        if (cstr) {
            size_t len = strlen(cstr);
            if (len >= sizeof(layer_name))
                return -1;
            memcpy(layer_name, cstr, len);
        }
    }

    int64_t *grid = (int64_t *)malloc(grid_size);
    if (!grid)
        return -1;
    memset(grid, 0, grid_size);

    tm_layer *layer = &tilemap->layers[idx];
    layer->tiles = grid;
    layer->tileset = NULL;
    layer->tileset_cols = 0;
    layer->tileset_rows = 0;
    layer->tile_count = 0;
    layer->visible = 1;
    layer->owns_tiles = 1;
    layer->import_parallax_x = 1.0;
    layer->import_parallax_y = 1.0;

    memcpy(layer->name, layer_name, sizeof(layer->name));

    tilemap->layer_count = idx + 1;
    return (int64_t)idx;
}

/// @brief Number of layers currently present (always >= 1 for a valid tilemap).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Layer count, or `0` for an invalid map.
int64_t rt_tilemap_get_layer_count(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    return tilemap ? tilemap->layer_count : 0;
}

/// @brief Linear lookup of a layer by name (case-sensitive `strcmp`); returns -1 if not found.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param name Borrowed runtime string to find.
/// @return First matching layer index, or `-1` when invalid/absent.
int64_t rt_tilemap_get_layer_by_name(void *tilemap_ptr, rt_string name) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap || !name)
        return -1;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return -1;

    for (int32_t i = 0; i < tilemap->layer_count; i++) {
        if (strcmp(tilemap->layers[i].name, cstr) == 0)
            return (int64_t)i;
    }
    return -1;
}

/// @brief Remove a non-base layer (index 0 is permanent), shifting subsequent layers down.
/// Frees the owned tile grid + per-layer tileset, and rebases `collision_layer` so it still points
/// at a valid layer (resets to 0 if removed, or decrements if it was above the removed slot).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Nonzero valid layer index to remove.
void rt_tilemap_remove_layer(void *tilemap_ptr, int64_t layer) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;

    // Cannot remove layer 0 (base layer) or invalid indices
    if (layer <= 0 || layer >= tilemap->layer_count)
        return;

    tm_layer *lyr = &tilemap->layers[layer];

    // Free the tile grid if owned
    if (lyr->owns_tiles && lyr->tiles)
        free(lyr->tiles);

    // Free per-layer tileset if set
    if (lyr->tileset)
        rt_heap_release(lyr->tileset);

    // Shift layers down
    for (int32_t i = (int32_t)layer; i < tilemap->layer_count - 1; i++)
        tilemap->layers[i] = tilemap->layers[i + 1];

    tilemap->layer_count--;

    // Clear the now-unused slot
    memset(&tilemap->layers[tilemap->layer_count], 0, sizeof(tm_layer));

    // Adjust collision layer if needed
    if (tilemap->collision_layer == (int32_t)layer)
        tilemap->collision_layer = 0;
    else if (tilemap->collision_layer > (int32_t)layer)
        tilemap->collision_layer--;
}

/// @brief Toggle a layer's visibility flag (drawing skips invisible layers).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Valid zero-based layer index.
/// @param visible Zero to hide; nonzero to show.
void rt_tilemap_set_layer_visible(void *tilemap_ptr, int64_t layer, int8_t visible) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;
    tilemap->layers[layer].visible = visible ? 1 : 0;
}

/// @brief Read a layer's visibility flag (0 = hidden, 1 = visible). Returns 0 for invalid layers.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Layer index to query.
/// @return Normalized visibility flag, or `0` for invalid input.
int8_t rt_tilemap_get_layer_visible(void *tilemap_ptr, int64_t layer) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    if (layer < 0 || layer >= tilemap->layer_count)
        return 0;
    return tilemap->layers[layer].visible;
}

//=============================================================================
// Per-Layer Tile Access
//=============================================================================

/// @brief Write a tile id at (x, y) on a specific layer. Silently no-ops on out-of-range
/// layer/coords.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Layer index to modify.
/// @param x Logical column.
/// @param y Logical row.
/// @param tile Tile identifier to store.
void rt_tilemap_set_tile_layer(
    void *tilemap_ptr, int64_t layer, int64_t x, int64_t y, int64_t tile) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;
    if (x < 0 || x >= tilemap->width || y < 0 || y >= tilemap->height)
        return;
    if (!tilemap->layers[layer].tiles)
        return;
    tilemap->layers[layer].tiles[y * tilemap->width + x] = tile;
}

/// @brief Read the tile id at (x, y) on a specific layer; 0 for out-of-range queries or empty
/// cells.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Layer index to inspect.
/// @param x Logical column.
/// @param y Logical row.
/// @return Stored tile identifier, or `0` for invalid/out-of-bounds access.
int64_t rt_tilemap_get_tile_layer(void *tilemap_ptr, int64_t layer, int64_t x, int64_t y) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return 0;
    if (layer < 0 || layer >= tilemap->layer_count)
        return 0;
    if (x < 0 || x >= tilemap->width || y < 0 || y >= tilemap->height)
        return 0;
    if (!tilemap->layers[layer].tiles)
        return 0;
    return tilemap->layers[layer].tiles[y * tilemap->width + x];
}

/// @brief Fill every cell of a single layer with the given tile id.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Layer index to fill.
/// @param tile Tile identifier to store.
void rt_tilemap_fill_layer(void *tilemap_ptr, int64_t layer, int64_t tile) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;
    if (!tilemap->layers[layer].tiles)
        return;
    int64_t count = 0;
    if (!tilemap_checked_grid_size(tilemap->width, tilemap->height, &count, NULL))
        return;
    for (int64_t i = 0; i < count; i++)
        tilemap->layers[layer].tiles[i] = tile;
}

/// @brief Zero every cell of a single layer (`memset` shortcut for `fill_layer(layer, 0)`).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Layer index to clear.
void rt_tilemap_clear_layer(void *tilemap_ptr, int64_t layer) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;
    if (!tilemap->layers[layer].tiles)
        return;
    size_t grid_size = 0;
    if (!tilemap_checked_grid_size(tilemap->width, tilemap->height, NULL, &grid_size))
        return;
    memset(tilemap->layers[layer].tiles, 0, grid_size);
}

//=============================================================================
// Per-Layer Tileset
//=============================================================================

/// @brief Bind a per-layer tileset (overrides the base tileset when drawing this layer).
/// Pass `pixels=NULL` to clear and fall back to the tilemap-wide tileset. The image is cloned
/// (via `rt_pixels_clone`) and retained on the heap; the previous binding is released.
/// `tile_count` is recomputed from the cloned image's dimensions divided by tile_width/height.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Valid layer index.
/// @param pixels Valid Pixels to clone, or `NULL` to restore base-tileset fallback.
void rt_tilemap_set_layer_tileset(void *tilemap_ptr, int64_t layer, void *pixels) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;

    tm_layer *lyr = &tilemap->layers[layer];

    if (!pixels) {
        // Reset to base tileset
        if (lyr->tileset)
            rt_heap_release(lyr->tileset);
        lyr->tileset = NULL;
        lyr->tileset_cols = 0;
        lyr->tileset_rows = 0;
        lyr->tile_count = 0;
        return;
    }
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)))
        return;

    void *cloned = rt_pixels_clone(pixels);
    if (!cloned)
        return;

    if (lyr->tileset)
        rt_heap_release(lyr->tileset);

    lyr->tileset = cloned;

    int64_t ts_width = rt_pixels_width(cloned);
    int64_t ts_height = rt_pixels_height(cloned);

    lyr->tileset_cols = ts_width / tilemap->source_frame_width;
    lyr->tileset_rows = ts_height / tilemap->source_frame_height;
    lyr->tile_count = lyr->tileset_cols * lyr->tileset_rows;
}

//=============================================================================
// Per-Layer Rendering
//=============================================================================

/// @brief Render one layer with viewport culling and per-tile animation resolution.
/// Falls back to the tilemap-wide tileset when no per-layer tileset is bound. Skips invisible
/// layers, layers without tile storage, and tiles outside the visible rect (camera + canvas size).
/// Tile id 0 is treated as empty; ids are 1-based into the chosen tileset (so id `n` maps to
/// `(n-1) % cols, (n-1) / cols`). `cam_x`/`cam_y` are world→screen offsets in pixels.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param canvas_ptr Destination Canvas.
/// @param layer Layer index to render.
/// @param cam_x Camera/destination X offset.
/// @param cam_y Camera/destination Y offset.
void rt_tilemap_draw_layer(
    void *tilemap_ptr, void *canvas_ptr, int64_t layer, int64_t cam_x, int64_t cam_y) {
    if (!tilemap_ptr || !canvas_ptr)
        return;

    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;

    if (!tilemap_layer_uses_default_layout(tilemap, &tilemap->layers[layer])) {
        int64_t first_x = 0;
        int64_t first_y = 0;
        int64_t visible_width = 0;
        int64_t visible_height = 0;
        if (!tilemap_visible_import_region(tilemap,
                                           &tilemap->layers[layer],
                                           rt_canvas_width(canvas_ptr),
                                           rt_canvas_height(canvas_ptr),
                                           cam_x,
                                           cam_y,
                                           &first_x,
                                           &first_y,
                                           &visible_width,
                                           &visible_height))
            return;
        rt_tilemap_draw_region_layer_impl(tilemap,
                                          tilemap_ptr,
                                          canvas_ptr,
                                          &tilemap->layers[layer],
                                          cam_x,
                                          cam_y,
                                          first_x,
                                          first_y,
                                          visible_width,
                                          visible_height);
        return;
    }

    int64_t tw = tilemap->tile_width > 0 ? tilemap->tile_width : 1;
    int64_t th = tilemap->tile_height > 0 ? tilemap->tile_height : 1;

    int64_t canvas_w = rt_canvas_width(canvas_ptr);
    int64_t canvas_h = rt_canvas_height(canvas_ptr);
    int64_t first_x = 0;
    int64_t first_y = 0;
    int64_t vis_w = 0;
    int64_t vis_h = 0;
    if (!tilemap_visible_span(canvas_w, cam_x, tw, tilemap->width, &first_x, &vis_w) ||
        !tilemap_visible_span(canvas_h, cam_y, th, tilemap->height, &first_y, &vis_h))
        return;

    rt_tilemap_draw_region_layer_impl(tilemap,
                                      tilemap_ptr,
                                      canvas_ptr,
                                      &tilemap->layers[layer],
                                      cam_x,
                                      cam_y,
                                      first_x,
                                      first_y,
                                      vis_w,
                                      vis_h);
}

//=============================================================================
// Collision Layer
//=============================================================================

/// @brief Designate which layer's tile grid is consulted by `is_solid_at` and `collide_body`.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param layer Valid layer index to designate.
void rt_tilemap_set_collision_layer(void *tilemap_ptr, int64_t layer) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    if (!tilemap)
        return;
    if (layer < 0 || layer >= tilemap->layer_count)
        return;
    tilemap->collision_layer = (int32_t)layer;
}

/// @brief Read the index of the layer currently designated as the collision source (default 0).
/// @param tilemap_ptr Candidate Tilemap handle.
/// @return Collision-layer index, or `0` for an invalid map.
int64_t rt_tilemap_get_collision_layer(void *tilemap_ptr) {
    rt_tilemap_impl *tilemap = tilemap_checked(tilemap_ptr, NULL);
    return tilemap ? tilemap->collision_layer : 0;
}

//=============================================================================
// Tile Animation
//=============================================================================

/// @brief Find animation storage by raw base tile identifier.
/// @param tm Candidate Tilemap implementation.
/// @param base_tile_id Positive animation key.
/// @return Borrowed animation entry, or `NULL` when absent.
static tm_tile_anim *tilemap_find_anim(rt_tilemap_impl *tm, int64_t base_tile_id) {
    if (!tm)
        return NULL;
    for (int32_t i = 0; i < tm->tile_anim_count; ++i) {
        if (tm->tile_anims[i].base_tile_id == base_tile_id)
            return &tm->tile_anims[i];
    }
    return NULL;
}

/// @brief Transactionally add or replace a variable-duration tile animation.
/// @details Validates all positive frames/durations, allocates both arrays
///          before modifying the map, resets playback to frame zero, and stores
///          @p uniform_duration as metadata.
/// @param tm Candidate Tilemap implementation.
/// @param base_tile_id Positive raw tile identifier used as the lookup key.
/// @param frame_count Positive count within the imported-animation limit.
/// @param frame_tiles Array of positive resolved tile identifiers.
/// @param frame_durations Array of positive per-frame milliseconds.
/// @param uniform_duration Uniform duration metadata, or zero for imported timing.
/// @return `1` on success, otherwise `0` with any existing animation unchanged.
static int8_t tilemap_assign_anim(rt_tilemap_impl *tm,
                                  int64_t base_tile_id,
                                  int64_t frame_count,
                                  const int64_t *frame_tiles,
                                  const int64_t *frame_durations,
                                  int64_t uniform_duration) {
    if (!tm || base_tile_id <= 0 || frame_count <= 0 || frame_count > TM_MAX_IMPORT_ANIM_FRAMES ||
        !frame_tiles || !frame_durations ||
        (uint64_t)frame_count > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
        return 0;
    for (int64_t i = 0; i < frame_count; ++i) {
        if (frame_tiles[i] <= 0 || frame_durations[i] <= 0)
            return 0;
    }
    size_t bytes = (size_t)frame_count * sizeof(int64_t);
    int64_t *new_frames = (int64_t *)malloc(bytes);
    int64_t *new_durations = (int64_t *)malloc(bytes);
    if (!new_frames || !new_durations) {
        free(new_frames);
        free(new_durations);
        return 0;
    }
    memcpy(new_frames, frame_tiles, bytes);
    memcpy(new_durations, frame_durations, bytes);

    tm_tile_anim *anim = tilemap_find_anim(tm, base_tile_id);
    if (!anim && tm->tile_anim_count >= TM_MAX_TILE_ANIMS) {
        free(new_frames);
        free(new_durations);
        return 0;
    }
    if (!anim) {
        anim = &tm->tile_anims[tm->tile_anim_count++];
        memset(anim, 0, sizeof(*anim));
    }
    free(anim->frame_tiles);
    free(anim->frame_durations);
    anim->base_tile_id = base_tile_id;
    anim->frame_tiles = new_frames;
    anim->frame_durations = new_durations;
    anim->frame_count = (int32_t)frame_count;
    anim->ms_per_frame = uniform_duration;
    anim->timer = 0;
    anim->current_frame = 0;
    return 1;
}

/// @brief Register an animation that swaps `base_tile_id` for one of `frame_count` frames over
/// time. The frame table defaults to sequential ids `(base, base+1, ..., base+frame_count-1)`;
/// override individual frames with `set_tile_anim_frame`. Caps at `TM_MAX_TILE_ANIMS` registrations
/// and `TM_MAX_ANIM_FRAMES` per animation; duplicate base tile ids replace the existing animation.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param base_tile_id Positive raw tile identifier used as the animation key.
/// @param frame_count Number of sequential frames.
/// @param ms_per_frame Positive uniform duration.
void rt_tilemap_set_tile_anim(void *tilemap_ptr,
                              int64_t base_tile_id,
                              int64_t frame_count,
                              int64_t ms_per_frame) {
    if (!tilemap_ptr || base_tile_id <= 0 || frame_count < 1 || frame_count > TM_MAX_ANIM_FRAMES ||
        ms_per_frame <= 0)
        return;
    rt_tilemap_impl *tm = tilemap_checked(tilemap_ptr, NULL);
    if (!tm)
        return;
    int64_t frames[TM_MAX_ANIM_FRAMES];
    int64_t durations[TM_MAX_ANIM_FRAMES];
    for (int64_t i = 0; i < frame_count; ++i) {
        frames[i] = base_tile_id > INT64_MAX - i ? INT64_MAX : base_tile_id + i;
        durations[i] = ms_per_frame;
    }
    (void)tilemap_assign_anim(tm, base_tile_id, frame_count, frames, durations, ms_per_frame);
}

/// @brief Override one frame in an existing animation (selected by `base_tile_id`).
/// Useful for non-contiguous tilesets where animation frames don't sit on adjacent indices.
/// Silently no-ops if the animation is not registered or the frame index is out of range.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param base_tile_id Animation key to find.
/// @param frame_idx Zero-based frame index.
/// @param tile_id Replacement tile identifier, stored without range validation.
void rt_tilemap_set_tile_anim_frame(void *tilemap_ptr,
                                    int64_t base_tile_id,
                                    int64_t frame_idx,
                                    int64_t tile_id) {
    if (!tilemap_ptr)
        return;
    rt_tilemap_impl *tm = tilemap_checked(tilemap_ptr, NULL);
    if (!tm)
        return;
    for (int32_t i = 0; i < tm->tile_anim_count; i++) {
        if (tm->tile_anims[i].base_tile_id == base_tile_id) {
            if (frame_idx >= 0 && frame_idx < tm->tile_anims[i].frame_count)
                tm->tile_anims[i].frame_tiles[frame_idx] = tile_id;
            return;
        }
    }
}

/// @brief Register imported animation frames with individual durations.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param base_tile_id Positive raw tile identifier used as the animation key.
/// @param frame_count Positive number of entries.
/// @param frame_tiles Array of positive tile identifiers.
/// @param frame_durations Array of positive durations in milliseconds.
/// @return `1` when copied and registered, otherwise `0`.
int8_t rt_tilemap_set_import_tile_anim(void *tilemap_ptr,
                                       int64_t base_tile_id,
                                       int64_t frame_count,
                                       const int64_t *frame_tiles,
                                       const int64_t *frame_durations) {
    rt_tilemap_impl *tm = tilemap_checked(tilemap_ptr, NULL);
    return tilemap_assign_anim(tm, base_tile_id, frame_count, frame_tiles, frame_durations, 0);
}

/// @brief Advance all registered tile animations by `dt_ms` milliseconds.
/// Negative deltas are ignored. Large deltas advance by division/modulo so one call can span many
/// frames without looping once per elapsed frame.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param dt_ms Elapsed milliseconds; negative values become zero.
void rt_tilemap_update_anims(void *tilemap_ptr, int64_t dt_ms) {
    if (!tilemap_ptr)
        return;
    if (dt_ms < 0)
        dt_ms = 0;
    rt_tilemap_impl *tm = tilemap_checked(tilemap_ptr, NULL);
    if (!tm)
        return;
    for (int32_t i = 0; i < tm->tile_anim_count; i++) {
        tm_tile_anim *anim = &tm->tile_anims[i];
        if (!anim->frame_tiles || !anim->frame_durations || anim->frame_count <= 0)
            continue;
        int64_t current_duration = anim->frame_durations[anim->current_frame];
        if (current_duration <= 0)
            continue;
        int64_t remaining_in_frame = current_duration - anim->timer;
        if (dt_ms < remaining_in_frame) {
            anim->timer += dt_ms;
            continue;
        }
        int64_t remaining = dt_ms - remaining_in_frame;
        anim->current_frame = (anim->current_frame + 1) % anim->frame_count;
        anim->timer = 0;

        int64_t cycle_duration = 0;
        int cycle_overflow = 0;
        for (int32_t frame = 0; frame < anim->frame_count; ++frame) {
            int64_t duration = anim->frame_durations[frame];
            if (duration <= 0) {
                cycle_overflow = 1;
                break;
            }
            if (cycle_duration > INT64_MAX - duration) {
                cycle_overflow = 1;
                break;
            }
            cycle_duration += duration;
        }
        if (!cycle_overflow && cycle_duration > 0)
            remaining %= cycle_duration;
        for (int32_t traversed = 0; traversed < anim->frame_count; ++traversed) {
            current_duration = anim->frame_durations[anim->current_frame];
            if (remaining < current_duration) {
                anim->timer = remaining;
                break;
            }
            remaining -= current_duration;
            anim->current_frame = (anim->current_frame + 1) % anim->frame_count;
        }
    }
}

/// @brief Map a tile id through the animation table, returning the current frame's tile id.
/// If `tile_id` is not the base of any registered animation it is returned unchanged. Called by
/// rendering paths so animated tiles display the current frame while collision
/// continues to use the base tile id stored in the map data.
/// @param tilemap_ptr Candidate Tilemap handle.
/// @param tile_id Raw tile identifier to resolve.
/// @return Current animation frame identifier, or @p tile_id when unregistered/invalid.
int64_t rt_tilemap_resolve_anim_tile(void *tilemap_ptr, int64_t tile_id) {
    if (!tilemap_ptr)
        return tile_id;
    rt_tilemap_impl *tm = tilemap_checked(tilemap_ptr, NULL);
    if (!tm)
        return tile_id;
    for (int32_t i = 0; i < tm->tile_anim_count; i++) {
        if (tm->tile_anims[i].base_tile_id == tile_id)
            return tm->tile_anims[i].frame_tiles[tm->tile_anims[i].current_frame];
    }
    return tile_id;
}
