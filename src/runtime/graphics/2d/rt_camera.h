//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_camera.h
/// @file
/// @brief Declares the reference-counted 2D camera, coordinate-transform,
///        culling, follow, bounds, dirty-state, and parallax APIs.
// Purpose: 2D camera for viewport and scrolling, providing world-to-screen and
// screen-to-world coordinate transforms, zoom, rotation, and parallax tiling.
//
// Key invariants:
//   - Camera position is the world-space viewport origin. Viewport dimensions
//     are fixed at construction and zoom is clamped to 10..1000 percent.
//   - Full transforms translate around the viewport center, rotate, and scale;
//     screen-to-world applies the mathematical inverse.
//   - Bounds constrain the zoom-dependent world-space viewport rectangle.
//   - Parallax layers retain their Pixels objects and are limited to eight slots.
//
// Ownership/Lifetime:
//   - Cameras are reference-counted runtime objects allocated by
//     rt_obj_new_i64 and reclaimed by the runtime; no Camera-specific destroy
//     function is exported.
//   - Adding a parallax layer retains its borrowed Pixels handle. Removal or
//     Camera finalization releases that reference.
//
// Links: src/runtime/graphics/2d/rt_camera.c (implementation),
//        src/runtime/graphics/2d/rt_pixels.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime object class identifier used to validate Camera handles.
#define RT_CAMERA_CLASS_ID INT64_C(-0x520104)

//=========================================================================
// Camera Creation
//=========================================================================

/// @brief Create a new camera with specified viewport size.
/// @details Initializes the world origin, 100-percent zoom, zero rotation, no
///          bounds/deadzone/layers, and a set dirty flag.
/// @param width Viewport width in screen pixels; nonpositive values become one.
/// @param height Viewport height in screen pixels; nonpositive values become one.
/// @return Owned Camera handle, or `NULL` if allocation fails.
void *rt_camera_new(int64_t width, int64_t height);

//=========================================================================
// Camera Properties
//=========================================================================

/// @brief Get camera viewport left edge (world coordinates).
/// @param camera Borrowed Camera handle.
/// @return World-space left edge, or `0` after a failed validation trap.
int64_t rt_camera_get_x(void *camera);

/// @brief Set camera viewport left edge (world coordinates).
/// @details Applies active bounds and marks the transform dirty.
/// @param camera Borrowed Camera handle.
/// @param x Requested world-space left edge.
void rt_camera_set_x(void *camera, int64_t x);

/// @brief Get camera viewport top edge (world coordinates).
/// @param camera Borrowed Camera handle.
/// @return World-space top edge, or `0` after a failed validation trap.
int64_t rt_camera_get_y(void *camera);

/// @brief Set camera viewport top edge (world coordinates).
/// @details Applies active bounds and marks the transform dirty.
/// @param camera Borrowed Camera handle.
/// @param y Requested world-space top edge.
void rt_camera_set_y(void *camera, int64_t y);

/// @brief Get camera zoom level (100 = 100%).
/// @param camera Borrowed Camera handle.
/// @return Stored zoom percentage, or `100` after a failed validation trap.
int64_t rt_camera_get_zoom(void *camera);

/// @brief Set camera zoom level (100 = 100%).
/// @details Clamps to 10..1000 percent, re-applies active bounds, and marks
///          the transform dirty.
/// @param camera Borrowed Camera handle.
/// @param zoom Requested integer zoom percentage.
void rt_camera_set_zoom(void *camera, int64_t zoom);

/// @brief Get camera rotation in degrees.
/// @param camera Borrowed Camera handle.
/// @return Stored unnormalized angle, or `0` after a failed validation trap.
int64_t rt_camera_get_rotation(void *camera);

/// @brief Set camera rotation in degrees.
/// @details Stores the unnormalized angle and marks the transform dirty.
/// @param camera Borrowed Camera handle.
/// @param degrees Requested integer angle.
void rt_camera_set_rotation(void *camera, int64_t degrees);

/// @brief Get viewport width.
/// @param camera Borrowed Camera handle.
/// @return Positive construction-time screen width, or `0` after a failed
///         validation trap.
int64_t rt_camera_get_width(void *camera);

/// @brief Get viewport height.
/// @param camera Borrowed Camera handle.
/// @return Positive construction-time screen height, or `0` after a failed
///         validation trap.
int64_t rt_camera_get_height(void *camera);

/// @brief Get camera viewport center X (world coordinates).
/// @param camera Borrowed Camera handle.
/// @return Saturating integer center based on current zoom, or `0` after a
///         failed validation trap.
int64_t rt_camera_get_center_x(void *camera);

/// @brief Get camera viewport center Y (world coordinates).
/// @param camera Borrowed Camera handle.
/// @return Saturating integer center based on current zoom, or `0` after a
///         failed validation trap.
int64_t rt_camera_get_center_y(void *camera);

/// @brief Set camera viewport center (world coordinates).
/// @details Applies active bounds and marks dirty only if the final origin changes.
/// @param camera Borrowed Camera handle.
/// @param x Requested world-space center X.
/// @param y Requested world-space center Y.
void rt_camera_set_center(void *camera, int64_t x, int64_t y);

//=========================================================================
// Camera Methods
//=========================================================================

/// @brief Center the camera on a world position.
/// @details Uses saturating arithmetic, applies bounds, and always marks dirty.
/// @param camera Borrowed Camera handle.
/// @param x World X coordinate to center on.
/// @param y World Y coordinate to center on.
void rt_camera_follow(void *camera, int64_t x, int64_t y);

/// @brief Convert world coordinates to screen coordinates.
/// @details Applies center-relative translation, inverse camera rotation, and
///          zoom, then rounds with int64 saturation. Invalid handles or null
///          outputs leave both outputs unchanged.
/// @param camera Borrowed Camera handle.
/// @param world_x World X coordinate.
/// @param world_y World Y coordinate.
/// @param screen_x Required output for Screen X.
/// @param screen_y Required output for Screen Y.
void rt_camera_world_to_screen(
    void *camera, int64_t world_x, int64_t world_y, int64_t *screen_x, int64_t *screen_y);

/// @brief Get screen X from world X.
/// @details Uses the camera's vertical center for the omitted coordinate.
/// @param camera Borrowed Camera handle.
/// @param world_x World X coordinate.
/// @return Rounded, saturated screen X; invalid handles return @p world_x.
int64_t rt_camera_to_screen_x(void *camera, int64_t world_x);

/// @brief Get screen Y from world Y.
/// @details Uses the camera's horizontal center for the omitted coordinate.
/// @param camera Borrowed Camera handle.
/// @param world_y World Y coordinate.
/// @return Rounded, saturated screen Y; invalid handles return @p world_y.
int64_t rt_camera_to_screen_y(void *camera, int64_t world_y);

/// @brief Convert screen coordinates to world coordinates.
/// @details Applies the exact inverse camera transform and rounds with int64
///          saturation. Invalid handles or null outputs leave both outputs
///          unchanged.
/// @param camera Borrowed Camera handle.
/// @param screen_x Screen X coordinate.
/// @param screen_y Screen Y coordinate.
/// @param world_x Required output for World X.
/// @param world_y Required output for World Y.
void rt_camera_screen_to_world(
    void *camera, int64_t screen_x, int64_t screen_y, int64_t *world_x, int64_t *world_y);

/// @brief Get world X from screen X.
/// @details Uses the viewport's vertical screen center for the omitted coordinate.
/// @param camera Borrowed Camera handle.
/// @param screen_x Screen X coordinate.
/// @return Rounded, saturated world X; invalid handles return @p screen_x.
int64_t rt_camera_to_world_x(void *camera, int64_t screen_x);

/// @brief Get world Y from screen Y.
/// @details Uses the viewport's horizontal screen center for the omitted coordinate.
/// @param camera Borrowed Camera handle.
/// @param screen_y Screen Y coordinate.
/// @return Rounded, saturated world Y; invalid handles return @p screen_y.
int64_t rt_camera_to_world_y(void *camera, int64_t screen_y);

/// @brief Move the camera by delta amounts.
/// @details Adds with int64 saturation, applies active bounds, and marks dirty.
/// @param camera Borrowed Camera handle.
/// @param dx Horizontal world-space displacement.
/// @param dy Vertical world-space displacement.
void rt_camera_move(void *camera, int64_t dx, int64_t dy);

/// @brief Set camera bounds (limits where camera can go).
///        If the current position lies outside the new bounds, it is clamped
///        immediately and the camera becomes dirty.
/// @details @p max_x and @p max_y describe the world rectangle's right/bottom
///          extents. The largest legal viewport origin subtracts the current
///          zoom-dependent world dimensions. Undersized/reversed axes collapse
///          to their minimum.
/// @param camera Borrowed Camera handle.
/// @param min_x Minimum viewport left edge.
/// @param min_y Minimum viewport top edge.
/// @param max_x Maximum world-space right extent.
/// @param max_y Maximum world-space bottom extent.
void rt_camera_set_bounds(void *camera, int64_t min_x, int64_t min_y, int64_t max_x, int64_t max_y);

/// @brief Clear camera bounds (allow unlimited movement).
/// @details Does not move the camera or set its dirty flag.
/// @param camera Borrowed Camera handle.
void rt_camera_clear_bounds(void *camera);

/// @brief Smoothly follow a target position with linear interpolation.
/// @details Respects the configured per-axis deadzone and clamps after movement.
///          Values at least 1000 snap, values 1..999 move by rounded
///          thousandths, and nonpositive values do not follow. A rounded
///          zero step snaps the remaining coordinate difference.
/// @param camera Borrowed Camera handle.
/// @param target_x Target world X (center of follow).
/// @param target_y Target world Y (center of follow).
/// @param lerp_pct Interpolation fraction in thousandths.
void rt_camera_smooth_follow(void *camera, int64_t target_x, int64_t target_y, int64_t lerp_pct);

/// @brief Set camera deadzone size. Target within the deadzone won't move the camera.
/// @details Nonpositive dimensions disable that axis independently. The setting
///          does not mark the camera dirty; invalid handles are silently ignored.
/// @param camera Borrowed Camera handle.
/// @param w Deadzone width in world units.
/// @param h Deadzone height in world units.
void rt_camera_set_deadzone(void *camera, int64_t w, int64_t h);

//=========================================================================
// Visibility Culling
//=========================================================================

/// @brief Test whether a world-space AABB overlaps the camera's viewport.
///
/// Projects all four corners through the current zoom/rotation and compares
/// their screen-space AABB against the viewport. Rectangles with nonpositive
/// dimensions and rectangles touching only a viewport boundary are invisible.
/// @param camera Borrowed Camera handle. `NULL` conservatively returns visible;
///        a non-null invalid handle returns invisible.
/// @param x Left edge of the AABB in world coordinates.
/// @param y Top edge of the AABB in world coordinates.
/// @param w Positive width in world coordinates.
/// @param h Positive height in world coordinates.
/// @return `1` if the projected AABB overlaps the viewport; otherwise `0`.
int64_t rt_camera_is_visible(void *camera, int64_t x, int64_t y, int64_t w, int64_t h);

//=========================================================================
// Dirty Flag — Skip re-rendering when camera is stationary
//=========================================================================

/// @brief Check whether the camera has moved, zoomed, or rotated since the
///        last call to rt_camera_clear_dirty.
/// @param camera Borrowed Camera handle.
/// @return Stored dirty flag, or `0` for invalid handles.
int64_t rt_camera_is_dirty(void *camera);

/// @brief Clear the dirty flag after consuming the change.
/// @param camera Borrowed Camera handle; invalid handles are ignored.
void rt_camera_clear_dirty(void *camera);

//=========================================================================
// Parallax Layers
//=========================================================================

/// @brief Add a parallax scrolling layer.
/// @details Uses the first inactive of eight slots, clamps both scroll factors
///          to 0..100, and retains the Pixels object until removal/finalization.
/// @param camera Borrowed Camera handle.
/// @param pixels Borrowed valid Pixels handle.
/// @param scroll_x_pct X scroll factor (100 = camera speed, 50 = half, 0 = static).
/// @param scroll_y_pct Y scroll factor (100 = camera speed, 50 = half, 0 = static).
/// @return Layer index in 0..7 on success; `-1` if full or invalid.
int64_t rt_camera_add_parallax(void *camera,
                               void *pixels,
                               int64_t scroll_x_pct,
                               int64_t scroll_y_pct);

/// @brief Remove a parallax layer by index.
/// @details Releases the retained Pixels reference. Inactive/out-of-range
///          slots and invalid cameras are ignored.
/// @param camera Borrowed Camera handle.
/// @param index Slot index returned by rt_camera_add_parallax().
void rt_camera_remove_parallax(void *camera, int64_t index);

/// @brief Remove all parallax layers.
/// @details Releases every retained Pixels reference and resets the active count.
/// @param camera Borrowed Camera handle.
void rt_camera_clear_parallax(void *camera);

/// @brief Get count of active parallax layers.
/// @param camera Borrowed Camera handle.
/// @return Active slot count, or `0` for invalid handles.
int64_t rt_camera_parallax_count(void *camera);

/// @brief Render all parallax layers to a canvas.
/// @details Layers are drawn in slot order (0-7). Each Pixels buffer is tiled
///          horizontally and vertically, scrolled by the scaled camera
///          position. Neutral transforms cover the larger of canvas/viewport;
///          zoomed or rotated transforms cover the inverse-transformed
///          viewport using temporary pixel buffers. A 65,536-tile per-layer
///          budget skips pathological coverage.
/// @param camera Borrowed Camera handle.
/// @param canvas Borrowed destination canvas.
/// @return Number of successfully drawn layers, or `0` for invalid arguments.
int64_t rt_camera_draw_parallax(void *camera, void *canvas);

#ifdef __cplusplus
}
#endif
