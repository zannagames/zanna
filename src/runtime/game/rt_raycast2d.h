//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_raycast2d.h
/// @file
/// @brief Declares pixel-space tilemap rays and segment/shape intersection
///        predicates.
//
// Purpose: 2D raycasting for tilemap line-of-sight and line intersection.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Cast a pixel-space segment through a Tilemap using grid DDA.
/// @param tilemap Tilemap whose configured solidity is queried.
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @param hit_x Optional destination for first-hit X.
/// @param hit_y Optional destination for first-hit Y.
/// @return `1` on the first solid tile, otherwise `0`.
/// @details The segment is clipped to the map's half-open pixel extent. Outputs
///          remain unchanged on no hit. Null maps and invalid dimensions return
///          zero.
int8_t rt_raycast_tilemap(
    void *tilemap, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t *hit_x, int64_t *hit_y);

/// @brief Test whether a tilemap segment is unobstructed.
/// @param tilemap Tilemap to traverse.
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @return `1` when rt_raycast_tilemap() reports no solid hit.
/// @details Null/invalid tilemaps are consequently treated as unobstructed.
int8_t rt_has_line_of_sight(void *tilemap, int64_t x1, int64_t y1, int64_t x2, int64_t y2);

/// @brief Line segment vs AABB intersection test.
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @param rx Rectangle top-left X.
/// @param ry Rectangle top-left Y.
/// @param rw Nonnegative rectangle width.
/// @param rh Nonnegative rectangle height.
/// @return `1` for intersection/touching, otherwise `0`.
/// @details Rejects non-finite inputs and negative dimensions. Degenerate
///          segments and zero-size rectangles are supported.
int8_t rt_collision_line_rect(
    double x1, double y1, double x2, double y2, double rx, double ry, double rw, double rh);

/// @brief Line segment vs circle intersection test.
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @param cx Circle center X.
/// @param cy Circle center Y.
/// @param r Nonnegative radius.
/// @return `1` when the segment touches, crosses, or lies within the circle.
/// @details Rejects non-finite inputs and negative radius. Degenerate segments
///          are treated as point tests.
int8_t rt_collision_line_circle(
    double x1, double y1, double x2, double y2, double cx, double cy, double r);

#ifdef __cplusplus
}
#endif
