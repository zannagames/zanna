//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_world_projection.c
/// @file
/// @brief Implements allocation-free linear, isometric, and depth-based 2D
///        projection helpers.
// Purpose: Stateless 2D world-to-screen projection helpers for game code.
//
// Coordinate conventions:
//   - Linear projection first subtracts the camera coordinate, scales the
//     result, and offsets it by the screen origin.
//   - Isometric projection treats world_x/world_y as tile coordinates and uses
//     half a tile dimension for the conventional diamond transform.
//   - Perspective helpers use a caller-supplied focal length and signed depth;
//     flip_y reverses only the projected vertical offset.
//
//===----------------------------------------------------------------------===//

#include "rt_world_projection.h"

#include <math.h>

/// @brief Clamp @p value to [0.0, 1.0]; non-finite input yields 0.0.
/// @param value Scalar to constrain.
/// @return @p value inside the interval, or the nearest endpoint.
static double projection_clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Clamp a projection depth away from zero (to ±1e-6, preserving sign)
///        so perspective division never divides by ~0; non-finite -> 1.0.
/// @param depth Signed perspective divisor to normalize.
/// @return Finite depth whose magnitude is at least `1e-6`.
static double projection_safe_depth(double depth) {
    if (!isfinite(depth))
        return 1.0;
    if (depth >= 0.0 && depth < 0.000001)
        return 0.000001;
    if (depth < 0.0 && depth > -0.000001)
        return -0.000001;
    return depth;
}

/// @brief Project one horizontal world coordinate using a translated linear scale.
/// @param world_x Horizontal world coordinate.
/// @param camera_x Horizontal camera position to subtract.
/// @param origin_x Screen-space horizontal origin.
/// @param pixels_per_unit Scale from world units to screen pixels.
/// @return `origin_x + (world_x - camera_x) * pixels_per_unit`.
double rt_world_projection_linear_x(double world_x,
                                    double camera_x,
                                    double origin_x,
                                    double pixels_per_unit) {
    return origin_x + (world_x - camera_x) * pixels_per_unit;
}

/// @brief Project one vertical world coordinate, optionally reversing its axis.
/// @param world_y Vertical world coordinate.
/// @param camera_y Vertical camera position to subtract.
/// @param origin_y Screen-space vertical origin.
/// @param pixels_per_unit Scale from world units to screen pixels.
/// @param flip_y Nonzero to subtract the scaled offset from @p origin_y.
/// @return Translated, scaled screen-space vertical coordinate.
double rt_world_projection_linear_y(
    double world_y, double camera_y, double origin_y, double pixels_per_unit, int8_t flip_y) {
    double offset = (world_y - camera_y) * pixels_per_unit;
    return flip_y ? origin_y - offset : origin_y + offset;
}

/// @brief Project tile coordinates onto the horizontal axis of a diamond grid.
/// @param world_x First tile-space coordinate.
/// @param world_y Second tile-space coordinate.
/// @param origin_x Screen-space horizontal origin.
/// @param tile_width Full projected width of one isometric tile.
/// @return Horizontal coordinate `origin_x + (world_x - world_y) * tile_width / 2`.
double rt_world_projection_isometric_x(double world_x,
                                       double world_y,
                                       double origin_x,
                                       double tile_width) {
    return origin_x + (world_x - world_y) * (tile_width * 0.5);
}

/// @brief Project tile coordinates onto the vertical axis of a diamond grid.
/// @param world_x First tile-space coordinate.
/// @param world_y Second tile-space coordinate.
/// @param origin_y Screen-space vertical origin.
/// @param tile_height Full projected height of one isometric tile.
/// @return Vertical coordinate `origin_y + (world_x + world_y) * tile_height / 2`.
double rt_world_projection_isometric_y(double world_x,
                                       double world_y,
                                       double origin_y,
                                       double tile_height) {
    return origin_y + (world_x + world_y) * (tile_height * 0.5);
}

/// @brief Interpolate a scale across a clamped perspective-depth interval.
/// @details Maps @p near_depth to @p near_scale and @p far_depth to
///          @p far_scale, clamping depths beyond either end. A depth span with
///          magnitude below `1e-6`, or any non-finite argument, selects
///          @p near_scale.
/// @param depth Depth whose scale is requested.
/// @param near_depth Depth associated with @p near_scale.
/// @param far_depth Depth associated with @p far_scale.
/// @param near_scale Scale at the near endpoint and fallback result.
/// @param far_scale Scale at the far endpoint.
/// @return Linearly interpolated scale, or @p near_scale on invalid input.
double rt_world_projection_perspective_scale(
    double depth, double near_depth, double far_depth, double near_scale, double far_scale) {
    if (!isfinite(depth) || !isfinite(near_depth) || !isfinite(far_depth) ||
        !isfinite(near_scale) || !isfinite(far_scale)) {
        return near_scale;
    }

    double span = far_depth - near_depth;
    if (fabs(span) < 0.000001)
        return near_scale;

    double t = projection_clamp01((depth - near_depth) / span);
    return near_scale + (far_scale - near_scale) * t;
}

/// @brief Perspective-project a horizontal coordinate about a screen center.
/// @details Depth magnitudes below `1e-6` are clamped away from zero while
///          preserving the sign of nonzero depths; non-finite depth becomes
///          one. Other non-finite operands propagate through normal arithmetic.
/// @param world_x Horizontal coordinate relative to the projection axis.
/// @param center_x Screen-space horizontal projection center.
/// @param depth Signed perspective depth.
/// @param focal_length Perspective focal length in screen units.
/// @return `center_x + world_x * focal_length / safe_depth`.
double rt_world_projection_perspective_x(double world_x,
                                         double center_x,
                                         double depth,
                                         double focal_length) {
    return center_x + world_x * focal_length / projection_safe_depth(depth);
}

/// @brief Perspective-project a vertical coordinate about a screen center.
/// @details Uses the same safe-depth policy as
///          rt_world_projection_perspective_x().
/// @param world_y Vertical coordinate relative to the projection axis.
/// @param center_y Screen-space vertical projection center.
/// @param depth Signed perspective depth.
/// @param focal_length Perspective focal length in screen units.
/// @param flip_y Nonzero to subtract rather than add the projected offset.
/// @return Centered perspective vertical coordinate.
double rt_world_projection_perspective_y(
    double world_y, double center_y, double depth, double focal_length, int8_t flip_y) {
    double offset = world_y * focal_length / projection_safe_depth(depth);
    return flip_y ? center_y - offset : center_y + offset;
}
