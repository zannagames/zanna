//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_collision.c
/// @file
/// @brief Implements mutable collision rectangles and stateless 2D hit tests.
//
// Purpose: AABB and circle collision primitive library for Zanna games.
//   Provides reference-counted CollisionRect objects alongside stand-alone
//   rectangle, circle, containment, and distance functions. Intended as
//   a lightweight helper layer on top of Physics2D for game logic that needs
//   simple hit tests without a full simulation world (e.g. trigger zones,
//   projectile hit detection, UI hover regions).
//
// Key invariants:
//   - CollisionRect is an axis-aligned bounding box (AABB) with double x, y,
//     width, height fields. x, y is the top-left corner.
//   - Rectangle overlap tests are strict: rectangles sharing only an edge do
//     not overlap. Circle tests are inclusive: tangency counts as overlap.
//   - rt_collision_point_in_rect is inclusive on left/top and exclusive on
//     right/bottom; point_in_circle is inclusive of the boundary.
//   - Standalone functions (not methods on rect/circle objects) are also
//     provided for callers that store geometry inline rather than in objects.
//
// Ownership/Lifetime:
//   - CollisionRect objects use the runtime object's reference count. Destroy
//     releases one reference and frees the object when the count reaches zero.
//   - Stateless helpers allocate nothing and retain no caller-owned data.
//
// Links: src/runtime/game/rt_collision.h (public API),
//        src/runtime/graphics/rt_physics2d.h (full physics simulation),
//        docs/zannalib/game.md (Collision section)
//
//===----------------------------------------------------------------------===//

#include "rt_collision.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <math.h>
#include <stdlib.h>

/// @brief Mutable geometry stored behind an opaque CollisionRect handle.
struct rt_collision_rect_impl {
    double x;      ///< Left edge.
    double y;      ///< Top edge.
    double width;  ///< Width.
    double height; ///< Height.
};

/// @brief Validate and recover a collision-rectangle implementation pointer.
/// @param rect Opaque handle to validate; `NULL` is accepted.
/// @param api Trap message used when @p rect has the wrong runtime class ID.
/// @return @p rect when valid, or `NULL` for a null or mismatched handle.
/// @details A non-null mismatched handle raises a runtime trap before returning
///          `NULL`.
static rt_collision_rect checked_collision_rect(rt_collision_rect rect, const char *api) {
    if (!rect)
        return NULL;
    if (rt_obj_class_id(rect) != RT_COLLISION_RECT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return rect;
}

/// @brief Replace non-finite coordinates with zero.
/// @param value Candidate coordinate.
/// @return @p value when finite; otherwise `0.0`.
static double finite_or_zero(double value) {
    return isfinite(value) ? value : 0.0;
}

/// @brief Normalize a dimension to a finite, nonnegative value.
/// @param value Candidate width or height.
/// @return @p value when finite and strictly positive; otherwise `0.0`.
static double finite_nonnegative_or_zero(double value) {
    return (isfinite(value) && value > 0.0) ? value : 0.0;
}

/// @brief Create a new axis-aligned collision rectangle.
/// @param x Initial left edge; non-finite values become zero.
/// @param y Initial top edge; non-finite values become zero.
/// @param width Initial width; non-finite or nonpositive values become zero.
/// @param height Initial height; non-finite or nonpositive values become zero.
/// @return A newly allocated rectangle, or `NULL` if allocation fails.
/// @details The returned handle owns one runtime-object reference.
rt_collision_rect rt_collision_rect_new(double x, double y, double width, double height) {
    struct rt_collision_rect_impl *rect = (struct rt_collision_rect_impl *)rt_obj_new_i64(
        RT_COLLISION_RECT_CLASS_ID, (int64_t)sizeof(struct rt_collision_rect_impl));
    if (!rect)
        return NULL;

    rect->x = finite_or_zero(x);
    rect->y = finite_or_zero(y);
    rect->width = finite_nonnegative_or_zero(width);
    rect->height = finite_nonnegative_or_zero(height);

    return rect;
}

/// @brief Release a collision rectangle, decrementing its reference count.
/// @param rect Handle whose reference is released; `NULL` is a no-op.
/// @details Frees the object when the released reference was the last one.
///          A non-null handle of another runtime class raises a trap.
void rt_collision_rect_destroy(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Destroy: expected Zanna.Game.CollisionRect");
    if (rect && rt_obj_release_check0(rect))
        rt_obj_free(rect);
}

/// @brief Return the X coordinate (left edge) of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The finite left edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_x(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.X: expected Zanna.Game.CollisionRect");
    return rect ? rect->x : 0.0;
}

/// @brief Return the Y coordinate (top edge) of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The finite top edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_y(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Y: expected Zanna.Game.CollisionRect");
    return rect ? rect->y : 0.0;
}

/// @brief Return the width of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The nonnegative width, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_width(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Width: expected Zanna.Game.CollisionRect");
    return rect ? rect->width : 0.0;
}

/// @brief Return the height of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The nonnegative height, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_height(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Height: expected Zanna.Game.CollisionRect");
    return rect ? rect->height : 0.0;
}

/// @brief Return the right edge (x + width) of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The right edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_right(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Right: expected Zanna.Game.CollisionRect");
    return rect ? rect->x + rect->width : 0.0;
}

/// @brief Return the bottom edge (y + height) of the collision rectangle.
/// @param rect Rectangle to query.
/// @return The bottom edge, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_bottom(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.Bottom: expected Zanna.Game.CollisionRect");
    return rect ? rect->y + rect->height : 0.0;
}

/// @brief Return the X coordinate of the rectangle's center point.
/// @param rect Rectangle to query.
/// @return `x + width / 2`, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_center_x(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.CenterX: expected Zanna.Game.CollisionRect");
    return rect ? rect->x + rect->width * 0.5 : 0.0;
}

/// @brief Return the Y coordinate of the rectangle's center point.
/// @param rect Rectangle to query.
/// @return `y + height / 2`, or `0.0` for a null or invalid handle.
/// @details An invalid non-null handle raises a runtime trap.
double rt_collision_rect_center_y(rt_collision_rect rect) {
    rect = checked_collision_rect(rect, "CollisionRect.CenterY: expected Zanna.Game.CollisionRect");
    return rect ? rect->y + rect->height * 0.5 : 0.0;
}

/// @brief Move the rectangle to a new top-left position.
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param x New left edge; non-finite values become zero.
/// @param y New top edge; non-finite values become zero.
/// @details A non-null handle of another runtime class raises a trap.
void rt_collision_rect_set_position(rt_collision_rect rect, double x, double y) {
    rect = checked_collision_rect(rect,
                                  "CollisionRect.SetPosition: expected Zanna.Game.CollisionRect");
    if (!rect)
        return;
    rect->x = finite_or_zero(x);
    rect->y = finite_or_zero(y);
}

/// @brief Set the width and height of the collision rectangle.
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param width New width; non-finite or nonpositive values become zero.
/// @param height New height; non-finite or nonpositive values become zero.
/// @details A non-null handle of another runtime class raises a trap.
void rt_collision_rect_set_size(rt_collision_rect rect, double width, double height) {
    rect = checked_collision_rect(rect, "CollisionRect.SetSize: expected Zanna.Game.CollisionRect");
    if (!rect)
        return;
    rect->width = finite_nonnegative_or_zero(width);
    rect->height = finite_nonnegative_or_zero(height);
}

/// @brief Set all four components (x, y, width, height) of the collision rectangle.
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param x New left edge; non-finite values become zero.
/// @param y New top edge; non-finite values become zero.
/// @param width New width; non-finite or nonpositive values become zero.
/// @param height New height; non-finite or nonpositive values become zero.
/// @details A non-null handle of another runtime class raises a trap.
void rt_collision_rect_set(
    rt_collision_rect rect, double x, double y, double width, double height) {
    rect = checked_collision_rect(rect, "CollisionRect.Set: expected Zanna.Game.CollisionRect");
    if (!rect)
        return;
    rect->x = finite_or_zero(x);
    rect->y = finite_or_zero(y);
    rect->width = finite_nonnegative_or_zero(width);
    rect->height = finite_nonnegative_or_zero(height);
}

/// @brief Reposition the rectangle so its center is at (cx, cy).
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param cx New horizontal center.
/// @param cy New vertical center.
/// @details Non-finite center coordinates leave the rectangle unchanged. A
///          non-null handle of another runtime class raises a trap.
void rt_collision_rect_set_center(rt_collision_rect rect, double cx, double cy) {
    rect =
        checked_collision_rect(rect, "CollisionRect.SetCenter: expected Zanna.Game.CollisionRect");
    if (!rect || !isfinite(cx) || !isfinite(cy))
        return;
    rect->x = cx - rect->width * 0.5;
    rect->y = cy - rect->height * 0.5;
}

/// @brief Translate the rectangle by (dx, dy) relative to its current position.
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param dx Horizontal displacement.
/// @param dy Vertical displacement.
/// @details The operation is atomic: non-finite displacements or a non-finite
///          resulting coordinate leave both coordinates unchanged. A non-null
///          handle of another runtime class raises a trap.
void rt_collision_rect_move(rt_collision_rect rect, double dx, double dy) {
    rect = checked_collision_rect(rect, "CollisionRect.Move: expected Zanna.Game.CollisionRect");
    if (!rect || !isfinite(dx) || !isfinite(dy))
        return;
    double x = rect->x + dx;
    double y = rect->y + dy;
    if (!isfinite(x) || !isfinite(y))
        return;
    rect->x = x;
    rect->y = y;
}

/// @brief Test whether a point (px, py) lies inside the collision rectangle.
/// @param rect Rectangle to test.
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @return `1` when the point is inside; otherwise `0`.
/// @details Left and top edges are inclusive, while right and bottom edges are
///          exclusive. Non-finite point coordinates naturally compare false.
///          An invalid non-null handle raises a runtime trap.
int8_t rt_collision_rect_contains_point(rt_collision_rect rect, double px, double py) {
    rect = checked_collision_rect(rect,
                                  "CollisionRect.ContainsPoint: expected Zanna.Game.CollisionRect");
    if (!rect)
        return 0;
    return px >= rect->x && px < rect->x + rect->width && py >= rect->y &&
           py < rect->y + rect->height;
}

/// @brief Test whether two axis-aligned rectangles overlap.
/// @param rect First rectangle.
/// @param other Second rectangle.
/// @return `1` when their interiors overlap on both axes; otherwise `0`.
/// @details Edge-only contact does not count. Invalid non-null handles raise a
///          runtime trap.
int8_t rt_collision_rect_overlaps(rt_collision_rect rect, rt_collision_rect other) {
    rect =
        checked_collision_rect(rect, "CollisionRect.Overlaps: expected Zanna.Game.CollisionRect");
    other = checked_collision_rect(
        other, "CollisionRect.Overlaps: expected other Zanna.Game.CollisionRect");
    if (!rect || !other)
        return 0;
    return rt_collision_rect_overlaps_rect(rect, other->x, other->y, other->width, other->height);
}

/// @brief Test a collision rectangle against raw AABB coordinates.
/// @param rect Rectangle handle to test.
/// @param ox Other rectangle's left edge.
/// @param oy Other rectangle's top edge.
/// @param ow Other rectangle's width.
/// @param oh Other rectangle's height.
/// @return `1` for positive-area overlap; otherwise `0`.
/// @details The raw rectangle must have finite coordinates and strictly
///          positive dimensions. Edge-only contact does not count. An invalid
///          non-null @p rect raises a runtime trap.
int8_t rt_collision_rect_overlaps_rect(
    rt_collision_rect rect, double ox, double oy, double ow, double oh) {
    rect = checked_collision_rect(rect,
                                  "CollisionRect.OverlapsRect: expected Zanna.Game.CollisionRect");
    if (!rect)
        return 0;
    if (!isfinite(ox) || !isfinite(oy) || !isfinite(ow) || !isfinite(oh))
        return 0;
    if (ow <= 0.0 || oh <= 0.0)
        return 0;

    double r1_left = rect->x;
    double r1_right = rect->x + rect->width;
    double r1_top = rect->y;
    double r1_bottom = rect->y + rect->height;

    double r2_left = ox;
    double r2_right = ox + ow;
    double r2_top = oy;
    double r2_bottom = oy + oh;

    // Check for no overlap
    if (r1_right <= r2_left || r2_right <= r1_left || r1_bottom <= r2_top || r2_bottom <= r1_top) {
        return 0;
    }
    return 1;
}

/// @brief Compute the overlap distance along the X axis between two rectangles.
/// @param rect First rectangle.
/// @param other Second rectangle.
/// @return The signed minimum X-axis translation for @p rect, or `0.0` if the
///         rectangles do not overlap on X or either handle is null/invalid.
/// @details Positive values move @p rect left; negative values move it right.
///          Equal candidate penetrations select the negative value. Invalid
///          non-null handles raise a runtime trap.
double rt_collision_rect_overlap_x(rt_collision_rect rect, rt_collision_rect other) {
    rect =
        checked_collision_rect(rect, "CollisionRect.OverlapX: expected Zanna.Game.CollisionRect");
    other = checked_collision_rect(
        other, "CollisionRect.OverlapX: expected other Zanna.Game.CollisionRect");
    if (!rect || !other)
        return 0.0;

    double r1_left = rect->x;
    double r1_right = rect->x + rect->width;
    double r2_left = other->x;
    double r2_right = other->x + other->width;

    // Calculate overlap
    double overlap_left = r1_right - r2_left;
    double overlap_right = r2_right - r1_left;

    if (overlap_left <= 0 || overlap_right <= 0)
        return 0.0;

    // Return the smaller overlap (minimum penetration)
    return (overlap_left < overlap_right) ? overlap_left : -overlap_right;
}

/// @brief Compute the overlap distance along the Y axis between two rectangles.
/// @param rect First rectangle.
/// @param other Second rectangle.
/// @return The signed minimum Y-axis translation for @p rect, or `0.0` if the
///         rectangles do not overlap on Y or either handle is null/invalid.
/// @details Positive values move @p rect upward; negative values move it
///          downward. Equal candidate penetrations select the negative value.
///          Invalid non-null handles raise a runtime trap.
double rt_collision_rect_overlap_y(rt_collision_rect rect, rt_collision_rect other) {
    rect =
        checked_collision_rect(rect, "CollisionRect.OverlapY: expected Zanna.Game.CollisionRect");
    other = checked_collision_rect(
        other, "CollisionRect.OverlapY: expected other Zanna.Game.CollisionRect");
    if (!rect || !other)
        return 0.0;

    double r1_top = rect->y;
    double r1_bottom = rect->y + rect->height;
    double r2_top = other->y;
    double r2_bottom = other->y + other->height;

    // Calculate overlap
    double overlap_top = r1_bottom - r2_top;
    double overlap_bottom = r2_bottom - r1_top;

    if (overlap_top <= 0 || overlap_bottom <= 0)
        return 0.0;

    // Return the smaller overlap (minimum penetration)
    return (overlap_top < overlap_bottom) ? overlap_top : -overlap_bottom;
}

/// @brief Expand or shrink a rectangle in place on all sides.
/// @param rect Rectangle to mutate; `NULL` is a no-op.
/// @param margin Amount applied to each side; positive values expand and
///        negative values shrink.
/// @details A finite margin subtracts from the top-left coordinates and adds
///          twice the margin to each dimension. Negative resulting dimensions
///          clamp independently to zero without readjusting the coordinates.
///          A non-null handle of another runtime class raises a trap.
void rt_collision_rect_expand(rt_collision_rect rect, double margin) {
    rect = checked_collision_rect(rect, "CollisionRect.Expand: expected Zanna.Game.CollisionRect");
    if (!rect || !isfinite(margin))
        return;
    rect->x -= margin;
    rect->y -= margin;
    rect->width += margin * 2;
    rect->height += margin * 2;

    // Ensure non-negative size
    if (rect->width < 0)
        rect->width = 0;
    if (rect->height < 0)
        rect->height = 0;
}

/// @brief Test whether this rectangle fully contains another rectangle.
/// @param rect Candidate outer rectangle.
/// @param other Candidate inner rectangle.
/// @return `1` when all four edges of @p other are within @p rect; otherwise
///         `0`.
/// @details All boundary comparisons are inclusive. Invalid non-null handles
///          raise a runtime trap.
int8_t rt_collision_rect_contains_rect(rt_collision_rect rect, rt_collision_rect other) {
    rect = checked_collision_rect(rect,
                                  "CollisionRect.ContainsRect: expected Zanna.Game.CollisionRect");
    other = checked_collision_rect(
        other, "CollisionRect.ContainsRect: expected other Zanna.Game.CollisionRect");
    if (!rect || !other)
        return 0;

    return other->x >= rect->x && other->y >= rect->y &&
           other->x + other->width <= rect->x + rect->width &&
           other->y + other->height <= rect->y + rect->height;
}

//=============================================================================
// Static collision helpers
//=============================================================================

/// @brief Test two raw axis-aligned rectangles for positive-area overlap.
/// @param x1 First rectangle's left edge.
/// @param y1 First rectangle's top edge.
/// @param w1 First rectangle's width.
/// @param h1 First rectangle's height.
/// @param x2 Second rectangle's left edge.
/// @param y2 Second rectangle's top edge.
/// @param w2 Second rectangle's width.
/// @param h2 Second rectangle's height.
/// @return `1` when the rectangles overlap on both axes; otherwise `0`.
/// @details All arguments must be finite and both rectangles must have
///          strictly positive dimensions. Edge-only contact does not count.
int8_t rt_collision_rects_overlap(
    double x1, double y1, double w1, double h1, double x2, double y2, double w2, double h2) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(w1) || !isfinite(h1) || !isfinite(x2) ||
        !isfinite(y2) || !isfinite(w2) || !isfinite(h2) || w1 <= 0.0 || h1 <= 0.0 || w2 <= 0.0 ||
        h2 <= 0.0)
        return 0;
    if (x1 + w1 <= x2 || x2 + w2 <= x1 || y1 + h1 <= y2 || y2 + h2 <= y1) {
        return 0;
    }
    return 1;
}

/// @brief Standalone function: test if point (px, py) is inside rect (rx, ry, rw, rh).
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @param rx Rectangle left edge.
/// @param ry Rectangle top edge.
/// @param rw Rectangle width.
/// @param rh Rectangle height.
/// @return `1` when the point is inside; otherwise `0`.
/// @details All arguments must be finite, the dimensions must be strictly
///          positive, and the rectangle is left/top inclusive and
///          right/bottom exclusive.
int8_t rt_collision_point_in_rect(
    double px, double py, double rx, double ry, double rw, double rh) {
    if (!isfinite(px) || !isfinite(py) || !isfinite(rx) || !isfinite(ry) || !isfinite(rw) ||
        !isfinite(rh) || rw <= 0.0 || rh <= 0.0)
        return 0;
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/// @brief Test two raw circles for overlap or tangency.
/// @param x1 First circle's center X coordinate.
/// @param y1 First circle's center Y coordinate.
/// @param r1 First circle's radius.
/// @param x2 Second circle's center X coordinate.
/// @param y2 Second circle's center Y coordinate.
/// @param r2 Second circle's radius.
/// @return `1` when the center distance is at most the radius sum; otherwise
///         `0`.
/// @details All arguments must be finite and both radii must be strictly
///          positive. The squared-distance comparison avoids a square root.
int8_t rt_collision_circles_overlap(
    double x1, double y1, double r1, double x2, double y2, double r2) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(r1) || !isfinite(x2) || !isfinite(y2) ||
        !isfinite(r2) || r1 <= 0.0 || r2 <= 0.0)
        return 0;
    double dx = x2 - x1;
    double dy = y2 - y1;
    double dist_sq = dx * dx + dy * dy;
    double radii = r1 + r2;
    return dist_sq <= radii * radii;
}

/// @brief Standalone function: test if point (px, py) is inside a circle.
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @param cx Circle center X coordinate.
/// @param cy Circle center Y coordinate.
/// @param r Circle radius.
/// @return `1` when the point lies inside or on the circle; otherwise `0`.
/// @details All arguments must be finite and @p r must be strictly positive.
///          The squared-distance comparison avoids a square root.
int8_t rt_collision_point_in_circle(double px, double py, double cx, double cy, double r) {
    if (!isfinite(px) || !isfinite(py) || !isfinite(cx) || !isfinite(cy) || !isfinite(r) ||
        r <= 0.0)
        return 0;
    double dx = px - cx;
    double dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

/// @brief Test a raw circle against a raw axis-aligned rectangle.
/// @param cx Circle center X coordinate.
/// @param cy Circle center Y coordinate.
/// @param r Circle radius.
/// @param rx Rectangle left edge.
/// @param ry Rectangle top edge.
/// @param rw Rectangle width.
/// @param rh Rectangle height.
/// @return `1` when the circle overlaps or touches the rectangle; otherwise
///         `0`.
/// @details All arguments must be finite, the radius and dimensions must be
///          strictly positive, and contact at a rectangle edge or corner
///          counts as overlap.
int8_t rt_collision_circle_rect(
    double cx, double cy, double r, double rx, double ry, double rw, double rh) {
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(r) || !isfinite(rx) || !isfinite(ry) ||
        !isfinite(rw) || !isfinite(rh) || r <= 0.0 || rw <= 0.0 || rh <= 0.0)
        return 0;
    // Find closest point on rectangle to circle center
    double closest_x = cx;
    double closest_y = cy;

    if (cx < rx)
        closest_x = rx;
    else if (cx > rx + rw)
        closest_x = rx + rw;

    if (cy < ry)
        closest_y = ry;
    else if (cy > ry + rh)
        closest_y = ry + rh;

    // Check if closest point is within circle
    double dx = cx - closest_x;
    double dy = cy - closest_y;
    return dx * dx + dy * dy <= r * r;
}

/// @brief Compute the Euclidean distance between two points.
/// @param x1 First point's X coordinate.
/// @param y1 First point's Y coordinate.
/// @param x2 Second point's X coordinate.
/// @param y2 Second point's Y coordinate.
/// @return The finite-input distance, or `0.0` if any input is non-finite.
double rt_collision_distance(double x1, double y1, double x2, double y2) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
        return 0.0;
    double dx = x2 - x1;
    double dy = y2 - y1;
    return hypot(dx, dy);
}

/// @brief Compute the squared Euclidean distance between two points.
/// @param x1 First point's X coordinate.
/// @param y1 First point's Y coordinate.
/// @param x2 Second point's X coordinate.
/// @param y2 Second point's Y coordinate.
/// @return The squared distance, or `0.0` if any input is non-finite.
/// @details Avoids the square root used by rt_collision_distance().
double rt_collision_distance_squared(double x1, double y1, double x2, double y2) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
        return 0.0;
    double dx = x2 - x1;
    double dy = y2 - y1;
    return dx * dx + dy * dy;
}
