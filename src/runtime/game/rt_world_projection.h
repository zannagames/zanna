//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_world_projection.h
/// @file
/// @brief Declares stateless world-to-screen projection functions for linear,
///        isometric, and perspective-style views.
// Purpose: Stateless 2D world-to-screen projection helpers for game code.
//
// The API owns no state and performs no allocation. Callers supply coordinate
// origins, scales, focal lengths, and axis orientation explicitly.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Convert a horizontal world coordinate to screen space linearly.
/// @param world_x Horizontal world coordinate.
/// @param camera_x Horizontal camera coordinate to subtract.
/// @param origin_x Horizontal screen origin.
/// @param pixels_per_unit Number of screen pixels per world unit.
/// @return `origin_x + (world_x - camera_x) * pixels_per_unit`.
double rt_world_projection_linear_x(double world_x,
                                    double camera_x,
                                    double origin_x,
                                    double pixels_per_unit);

/// @brief Convert a vertical world coordinate to screen space linearly.
/// @param world_y Vertical world coordinate.
/// @param camera_y Vertical camera coordinate to subtract.
/// @param origin_y Vertical screen origin.
/// @param pixels_per_unit Number of screen pixels per world unit.
/// @param flip_y Nonzero to make increasing world Y move upward on screen.
/// @return @p origin_y plus or minus the scaled camera-relative offset.
double rt_world_projection_linear_y(
    double world_y, double camera_y, double origin_y, double pixels_per_unit, int8_t flip_y);

/// @brief Compute the horizontal diamond-isometric projection of tile coordinates.
/// @param world_x First tile-space coordinate.
/// @param world_y Second tile-space coordinate.
/// @param origin_x Horizontal screen origin.
/// @param tile_width Full projected tile width.
/// @return `origin_x + (world_x - world_y) * tile_width / 2`.
double rt_world_projection_isometric_x(double world_x,
                                       double world_y,
                                       double origin_x,
                                       double tile_width);

/// @brief Compute the vertical diamond-isometric projection of tile coordinates.
/// @param world_x First tile-space coordinate.
/// @param world_y Second tile-space coordinate.
/// @param origin_y Vertical screen origin.
/// @param tile_height Full projected tile height.
/// @return `origin_y + (world_x + world_y) * tile_height / 2`.
double rt_world_projection_isometric_y(double world_x,
                                       double world_y,
                                       double origin_y,
                                       double tile_height);

/// @brief Interpolate a scale between near and far depth endpoints.
/// @details The normalized depth parameter is clamped to 0..1. A near/far
///          depth span smaller than `1e-6`, or any non-finite argument, returns
///          @p near_scale.
/// @param depth Depth whose scale is requested.
/// @param near_depth Depth mapped to @p near_scale.
/// @param far_depth Depth mapped to @p far_scale.
/// @param near_scale Scale at the near endpoint and invalid-input fallback.
/// @param far_scale Scale at the far endpoint.
/// @return Clamped linear interpolation between the two scales.
double rt_world_projection_perspective_scale(
    double depth, double near_depth, double far_depth, double near_scale, double far_scale);

/// @brief Perspective-project a horizontal coordinate about a screen center.
/// @details Depth is clamped to a signed magnitude of at least `1e-6` before
///          division; non-finite depth is replaced by one.
/// @param world_x Horizontal coordinate relative to the projection axis.
/// @param center_x Horizontal screen center.
/// @param depth Signed perspective depth.
/// @param focal_length Focal length in screen units.
/// @return `center_x + world_x * focal_length / safe_depth`.
double rt_world_projection_perspective_x(double world_x,
                                         double center_x,
                                         double depth,
                                         double focal_length);

/// @brief Perspective-project a vertical coordinate about a screen center.
/// @details Depth follows the same normalization as
///          rt_world_projection_perspective_x().
/// @param world_y Vertical coordinate relative to the projection axis.
/// @param center_y Vertical screen center.
/// @param depth Signed perspective depth.
/// @param focal_length Focal length in screen units.
/// @param flip_y Nonzero to subtract the projected offset from @p center_y.
/// @return Centered perspective vertical coordinate.
double rt_world_projection_perspective_y(
    double world_y, double center_y, double depth, double focal_length, int8_t flip_y);

#ifdef __cplusplus
}
#endif
