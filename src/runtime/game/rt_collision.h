//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_collision.h
/// @file
/// @brief Declares mutable AABBs and stateless rectangle/circle hit tests.
//
// Purpose: AABB and circle collision detection helpers for game physics, providing overlap testing,
// depth queries, and distance calculations for both object handles and stateless free functions.
//
// Key invariants:
//   - Stored rectangle dimensions are normalized to nonnegative values.
//   - Raw-shape predicates require strictly positive dimensions and radii.
//   - Rectangle edge contact is not overlap; circle boundary contact is.
//   - Overlap-depth functions return signed separation directions, or zero
//     when no overlap exists on the queried axis.
//   - Static free functions are pure with no side effects or allocation.
//   - rt_collision_rect handles support mutable position and size updates.
//
// Ownership/Lifetime:
//   - A newly created rt_collision_rect owns one runtime-object reference.
//     Release it with rt_collision_rect_destroy.
//   - Static helper functions require no allocation and have no ownership semantics.
//
// Links: src/runtime/game/rt_collision.c (implementation), src/runtime/graphics/rt_camera.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to a mutable axis-aligned collision rectangle.
typedef struct rt_collision_rect_impl *rt_collision_rect;

/// @brief Runtime class ID used to validate CollisionRect handles.
#define RT_COLLISION_RECT_CLASS_ID INT64_C(-0x510211)

/// @brief Allocates and initializes a new axis-aligned collision rectangle.
/// @param x X coordinate of the left edge in world units.
/// @param y Y coordinate of the top edge in world units.
/// @param width Horizontal extent of the rectangle.
/// @param height Vertical extent of the rectangle.
/// @return A new CollisionRect handle, or `NULL` if allocation fails.
/// @details Non-finite coordinates become zero. Non-finite or nonpositive
///          dimensions become zero. Release the returned reference with
///          rt_collision_rect_destroy().
rt_collision_rect rt_collision_rect_new(double x, double y, double width, double height);

/// @brief Releases one reference to a CollisionRect.
/// @param rect The rectangle to release; `NULL` is a no-op.
/// @details The object is freed when its reference count reaches zero. A
///          non-null handle of another runtime class raises a trap.
void rt_collision_rect_destroy(rt_collision_rect rect);

/// @brief Retrieves the X position (left edge) of the rectangle.
/// @param rect The collision rectangle to query.
/// @return The X coordinate of the left edge, or `0.0` for a null or invalid
///         handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_x(rt_collision_rect rect);

/// @brief Retrieves the Y position (top edge) of the rectangle.
/// @param rect The collision rectangle to query.
/// @return The Y coordinate of the top edge, or `0.0` for a null or invalid
///         handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_y(rt_collision_rect rect);

/// @brief Retrieves the width of the rectangle.
/// @param rect The collision rectangle to query.
/// @return The nonnegative horizontal extent, or `0.0` for a null or invalid
///         handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_width(rt_collision_rect rect);

/// @brief Retrieves the height of the rectangle.
/// @param rect The collision rectangle to query.
/// @return The nonnegative vertical extent, or `0.0` for a null or invalid
///         handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_height(rt_collision_rect rect);

/// @brief Computes the right edge coordinate (x + width).
/// @param rect The collision rectangle to query.
/// @return The right edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_right(rt_collision_rect rect);

/// @brief Computes the bottom edge coordinate (y + height).
/// @param rect The collision rectangle to query.
/// @return The bottom edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_bottom(rt_collision_rect rect);

/// @brief Computes the horizontal center of the rectangle.
/// @param rect The collision rectangle to query.
/// @return `x + width / 2`, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_center_x(rt_collision_rect rect);

/// @brief Computes the vertical center of the rectangle.
/// @param rect The collision rectangle to query.
/// @return `y + height / 2`, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_center_y(rt_collision_rect rect);

/// @brief Moves the rectangle to a new top-left position without changing
///   its size.
/// @param rect The collision rectangle to modify.
/// @param x New X coordinate for the left edge; non-finite values become zero.
/// @param y New Y coordinate for the top edge; non-finite values become zero.
/// @details A null handle is a no-op; an invalid non-null handle raises a trap.
void rt_collision_rect_set_position(rt_collision_rect rect, double x, double y);

/// @brief Resizes the rectangle without changing its position.
/// @param rect The collision rectangle to modify.
/// @param width New horizontal extent; non-finite or nonpositive values become
///        zero.
/// @param height New vertical extent; non-finite or nonpositive values become
///        zero.
/// @details A null handle is a no-op; an invalid non-null handle raises a trap.
void rt_collision_rect_set_size(rt_collision_rect rect, double width, double height);

/// @brief Sets both position and size of the rectangle in one call.
/// @param rect The collision rectangle to modify.
/// @param x X coordinate of the left edge; non-finite values become zero.
/// @param y Y coordinate of the top edge; non-finite values become zero.
/// @param width Horizontal extent; non-finite or nonpositive values become
///        zero.
/// @param height Vertical extent; non-finite or nonpositive values become
///        zero.
/// @details A null handle is a no-op; an invalid non-null handle raises a trap.
void rt_collision_rect_set(rt_collision_rect rect, double x, double y, double width, double height);

/// @brief Repositions the rectangle so that its center is at the given point.
/// @param rect The collision rectangle to modify.
/// @param cx Desired center X coordinate.
/// @param cy Desired center Y coordinate.
/// @details A null handle or non-finite center coordinate leaves the rectangle
///          unchanged. An invalid non-null handle raises a trap.
void rt_collision_rect_set_center(rt_collision_rect rect, double cx, double cy);

/// @brief Translates the rectangle by a displacement vector.
/// @param rect The collision rectangle to modify.
/// @param dx Horizontal displacement (positive = rightward).
/// @param dy Vertical displacement (positive = downward).
/// @details A null handle, a non-finite displacement, or a non-finite result
///          leaves the rectangle unchanged. An invalid non-null handle raises
///          a trap.
void rt_collision_rect_move(rt_collision_rect rect, double dx, double dy);

/// @brief Tests whether a point lies inside the rectangle.
/// @details Inclusive on the left/top edges and exclusive on right/bottom.
/// @param rect The collision rectangle to test against.
/// @param px X coordinate of the test point.
/// @param py Y coordinate of the test point.
/// @return 1 if the point is inside, including left/top edges, 0 otherwise.
/// @details Non-finite point coordinates return 0. An invalid non-null handle
///          raises a runtime trap.
int8_t rt_collision_rect_contains_point(rt_collision_rect rect, double px, double py);

/// @brief Tests whether this rectangle overlaps with another CollisionRect.
/// @param rect The first collision rectangle.
/// @param other The second collision rectangle to test against.
/// @return 1 if the two rectangles share positive area, 0 otherwise.
/// @details Edge-only contact does not count. Invalid non-null handles raise a
///          runtime trap.
int8_t rt_collision_rect_overlaps(rt_collision_rect rect, rt_collision_rect other);

/// @brief Tests whether this rectangle overlaps with a rectangle given as
///   raw coordinates.
/// @param rect The collision rectangle handle to test against.
/// @param ox X coordinate of the other rectangle's left edge.
/// @param oy Y coordinate of the other rectangle's top edge.
/// @param ow Width of the other rectangle.
/// @param oh Height of the other rectangle.
/// @return 1 if the rectangles share positive area, 0 otherwise.
/// @details The raw rectangle must use finite coordinates and strictly
///          positive dimensions. Edge-only contact does not count. An invalid
///          non-null @p rect raises a runtime trap.
int8_t rt_collision_rect_overlaps_rect(
    rt_collision_rect rect, double ox, double oy, double ow, double oh);

/// @brief Computes the penetration depth on the X axis between two
///   overlapping rectangles.
/// @param rect The first collision rectangle.
/// @param other The second collision rectangle.
/// @return The signed minimum horizontal separation magnitude, or 0 if there
///         is no X-axis overlap.
/// @details Positive values direct @p rect left and negative values direct it
///          right. Equal penetrations choose the negative value. Invalid
///          non-null handles raise a runtime trap.
double rt_collision_rect_overlap_x(rt_collision_rect rect, rt_collision_rect other);

/// @brief Computes the penetration depth on the Y axis between two
///   overlapping rectangles.
/// @param rect The first collision rectangle.
/// @param other The second collision rectangle.
/// @return The signed minimum vertical separation magnitude, or 0 if there is
///         no Y-axis overlap.
/// @details Positive values direct @p rect upward and negative values direct
///          it downward. Equal penetrations choose the negative value. Invalid
///          non-null handles raise a runtime trap.
double rt_collision_rect_overlap_y(rt_collision_rect rect, rt_collision_rect other);

/// @brief Expands (or shrinks) the rectangle uniformly on all four sides.
/// @param rect The collision rectangle to modify.
/// @param margin Amount to add to each side. A positive value grows the
///   rectangle; a negative value shrinks it.
/// @details Non-finite margins and null handles are no-ops. Dimensions that
///          would become negative clamp independently to zero without
///          readjusting the shifted top-left coordinates. An invalid non-null
///          handle raises a runtime trap.
void rt_collision_rect_expand(rt_collision_rect rect, double margin);

/// @brief Tests whether this rectangle fully contains another rectangle.
/// @param rect The outer collision rectangle.
/// @param other The inner collision rectangle to test.
/// @return 1 if every point of @p other lies within @p rect, 0 otherwise.
/// @details Boundary equality counts as containment. Invalid non-null handles
///          raise a runtime trap.
int8_t rt_collision_rect_contains_rect(rt_collision_rect rect, rt_collision_rect other);

//=============================================================================
// Static collision helpers (no instance needed)
//=============================================================================

/// @brief Tests whether two axis-aligned rectangles overlap, using raw
///   coordinates.
/// @param x1 Left edge of the first rectangle.
/// @param y1 Top edge of the first rectangle.
/// @param w1 Width of the first rectangle.
/// @param h1 Height of the first rectangle.
/// @param x2 Left edge of the second rectangle.
/// @param y2 Top edge of the second rectangle.
/// @param w2 Width of the second rectangle.
/// @param h2 Height of the second rectangle.
/// @return 1 if the rectangles share positive area, 0 otherwise.
/// @details All arguments must be finite and all dimensions strictly positive.
///          Edge-only contact does not count.
int8_t rt_collision_rects_overlap(
    double x1, double y1, double w1, double h1, double x2, double y2, double w2, double h2);

/// @brief Tests whether a point lies inside an axis-aligned rectangle.
/// @details Inclusive on the left/top edges and exclusive on right/bottom.
/// @param px X coordinate of the test point.
/// @param py Y coordinate of the test point.
/// @param rx Left edge of the rectangle.
/// @param ry Top edge of the rectangle.
/// @param rw Width of the rectangle.
/// @param rh Height of the rectangle.
/// @return 1 if the point is inside, including left/top edges, 0 otherwise.
/// @details All arguments must be finite and the dimensions strictly positive.
int8_t rt_collision_point_in_rect(double px, double py, double rx, double ry, double rw, double rh);

/// @brief Tests whether two circles overlap.
/// @param x1 X coordinate of the first circle's center.
/// @param y1 Y coordinate of the first circle's center.
/// @param r1 Radius of the first circle.
/// @param x2 X coordinate of the second circle's center.
/// @param y2 Y coordinate of the second circle's center.
/// @param r2 Radius of the second circle.
/// @return 1 if the circles overlap (distance between centers <= r1 + r2),
///   0 otherwise.
/// @details All arguments must be finite and both radii strictly positive.
///          Tangency counts as overlap.
int8_t rt_collision_circles_overlap(
    double x1, double y1, double r1, double x2, double y2, double r2);

/// @brief Tests whether a point lies inside a circle.
/// @param px X coordinate of the test point.
/// @param py Y coordinate of the test point.
/// @param cx X coordinate of the circle's center.
/// @param cy Y coordinate of the circle's center.
/// @param r Radius of the circle.
/// @return 1 if the distance from the point to the center is <= r,
///   0 otherwise.
/// @details All arguments must be finite and @p r strictly positive. A point
///          on the circumference counts as inside.
int8_t rt_collision_point_in_circle(double px, double py, double cx, double cy, double r);

/// @brief Tests whether a circle overlaps an axis-aligned rectangle.
/// @param cx X coordinate of the circle's center.
/// @param cy Y coordinate of the circle's center.
/// @param r Radius of the circle.
/// @param rx Left edge of the rectangle.
/// @param ry Top edge of the rectangle.
/// @param rw Width of the rectangle.
/// @param rh Height of the rectangle.
/// @return 1 if the circle and rectangle overlap or touch, 0 otherwise.
/// @details All arguments must be finite; the radius and rectangle dimensions
///          must be strictly positive. Edge and corner tangency count.
int8_t rt_collision_circle_rect(
    double cx, double cy, double r, double rx, double ry, double rw, double rh);

/// @brief Computes the Euclidean distance between two points.
/// @param x1 X coordinate of the first point.
/// @param y1 Y coordinate of the first point.
/// @param x2 X coordinate of the second point.
/// @param y2 Y coordinate of the second point.
/// @return The distance for finite inputs, or `0.0` if any input is
///         non-finite.
/// @details Uses hypot(), which may still overflow for extreme finite inputs.
double rt_collision_distance(double x1, double y1, double x2, double y2);

/// @brief Computes the squared Euclidean distance between two points.
///
/// Faster than rt_collision_distance() since it avoids the square root.
/// Useful for distance comparisons where the actual magnitude is not needed.
/// @param x1 X coordinate of the first point.
/// @param y1 Y coordinate of the first point.
/// @param x2 X coordinate of the second point.
/// @param y2 Y coordinate of the second point.
/// @return The squared distance for finite inputs, or `0.0` if any input is
///         non-finite. Extreme finite coordinates may overflow the result.
double rt_collision_distance_squared(double x1, double y1, double x2, double y2);

#ifdef __cplusplus
}
#endif
