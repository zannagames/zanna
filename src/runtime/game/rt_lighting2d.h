//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_lighting2d.h
/// @file
/// @brief Declares the runtime API for compositing 2D darkness and pooled
///        point-light glows.
//
// Purpose: 2D darkness overlay with dynamic point lights for cave and exploration games.
//
// Key invariants:
//   - Darkness alpha 0=fully lit, 255=pitch black.
//   - Dynamic lights have a lifetime in frames; lifetime 0 is permanent.
//   - Non-positive dynamic and tile-light radii are ignored. A non-positive
//     player-light radius disables the entire player glow.
//   - Player light pulses automatically via internal timer.
//   - Draw() must be called AFTER all game entities are rendered (post-process).
//
// Ownership/Lifetime:
//   - Handles are runtime reference-counted objects created by
//     rt_lighting2d_new() and released by rt_lighting2d_destroy().
//   - Dynamic lights persist until cleared or expired; tile lights are queued
//     for one draw and then consumed.
//
// Links: src/runtime/game/rt_lighting2d.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to a reference-counted 2D lighting overlay.
typedef struct rt_lighting2d_impl *rt_lighting2d;

/// @brief Runtime class ID used to validate Lighting2D handles.
#define RT_LIGHTING2D_CLASS_ID INT64_C(-0x510210)

/// @brief Create a darkness overlay with a bounded dynamic-light pool.
/// @param max_lights Requested number of concurrent dynamic-light slots,
///        clamped to the inclusive range `[0, 128]`.
/// @return A new Lighting2D reference, or `NULL` if allocation fails.
/// @details The overlay starts disabled with darkness zero, a near-black blue
///          tint, and a player light whose base radius is 180 pixels. The
///          separate per-draw tile-light queue always has 128 slots.
rt_lighting2d rt_lighting2d_new(int64_t max_lights);

/// @brief Release a reference to a lighting overlay.
/// @param lit Handle to release; `NULL` is a no-op.
/// @details The object is freed when its runtime reference count reaches zero.
///          Passing a non-null object of another runtime class raises a trap.
void rt_lighting2d_destroy(rt_lighting2d lit);

// Configuration

/// @brief Set the opacity of the full-canvas darkness overlay.
/// @param lit Lighting2D instance to modify; `NULL` is a no-op.
/// @param alpha Requested opacity, clamped to `[0, 255]`, where zero disables
///        lighting drawing and 255 is fully opaque darkness.
/// @details A non-null handle of another runtime class raises a trap.
void rt_lighting2d_set_darkness(rt_lighting2d lit, int64_t alpha);

/// @brief Read the darkness overlay opacity.
/// @param lit Lighting2D instance to query.
/// @return The stored value in `[0, 255]`, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_lighting2d_get_darkness(rt_lighting2d lit);

/// @brief Set the RGB tint applied by the darkness overlay.
/// @param lit Lighting2D instance to modify; `NULL` is a no-op.
/// @param color Packed RGB value; only the low 24 bits are retained.
/// @details The overlay opacity is controlled independently by
///          rt_lighting2d_set_darkness(). A non-null wrong-class handle traps.
void rt_lighting2d_set_tint_color(rt_lighting2d lit, int64_t color);

/// @brief Read the darkness overlay's packed RGB tint.
/// @param lit Lighting2D instance to query.
/// @return The stored `0x00RRGGBB` value, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_lighting2d_get_tint_color(rt_lighting2d lit);

/// @brief Configure the auto-pulsing player glow.
/// @param lit Lighting2D instance to modify; `NULL` is a no-op.
/// @param radius Base radius in screen pixels. Non-positive values store zero
///        and disable the complete player glow.
/// @param color Packed RGB value; only the low 24 bits are retained.
/// @details The glow is centered at the player coordinates supplied to
///          rt_lighting2d_draw(). A non-null wrong-class handle traps.
void rt_lighting2d_set_player_light(rt_lighting2d lit, int64_t radius, int64_t color);

// Dynamic lights

/// @brief Add a persistent or time-limited light in world coordinates.
/// @param lit Lighting2D instance whose dynamic pool receives the light.
/// @param x World-space X coordinate.
/// @param y World-space Y coordinate.
/// @param radius Glow radius in pixels; non-positive values are rejected.
/// @param color Packed RGB value; only the low 24 bits are retained.
/// @param lifetime Number of rt_lighting2d_update() calls before expiration;
///        zero creates a permanent light and negative values are rejected.
/// @details The request is silently dropped for `NULL`, invalid radius or
///          lifetime, or a full configured pool. A non-null wrong-class handle
///          raises a trap.
void rt_lighting2d_add_light(
    rt_lighting2d lit, int64_t x, int64_t y, int64_t radius, int64_t color, int64_t lifetime);

/// @brief Queue a one-draw light in screen coordinates.
/// @param lit Lighting2D instance whose tile-light queue receives the light.
/// @param screen_x Center X coordinate in canvas pixels.
/// @param screen_y Center Y coordinate in canvas pixels.
/// @param radius Glow radius in pixels; non-positive values are rejected.
/// @param color Packed RGB value; only the low 24 bits are retained.
/// @details Tile lights bypass camera translation and are consumed by the next
///          rt_lighting2d_draw(), including a draw skipped for null canvas or
///          disabled darkness. The fixed 128-entry queue silently drops
///          overflow. A non-null wrong-class handle raises a trap.
void rt_lighting2d_add_tile_light(
    rt_lighting2d lit, int64_t screen_x, int64_t screen_y, int64_t radius, int64_t color);

/// @brief Remove all active dynamic lights and queued tile lights.
/// @param lit Lighting2D instance to clear; `NULL` is a no-op.
/// @details Player-light settings and pulse phase are preserved. A non-null
///          handle of another runtime class raises a trap.
void rt_lighting2d_clear_lights(rt_lighting2d lit);

// Per-frame

/// @brief Advance the player pulse and dynamic-light lifetimes by one tick.
/// @param lit Lighting2D instance to update; `NULL` is a no-op.
/// @details The player pulse wraps every 120 calls. Positive-lifetime lights
///          are removed on expiration; permanent lights are unchanged. Tile
///          lights are not aged. A non-null wrong-class handle raises a trap.
void rt_lighting2d_update(rt_lighting2d lit);

/// @brief Composite darkness and all configured glows onto a canvas.
/// @param lit Lighting2D instance to draw.
/// @param canvas Destination 2D Canvas; `NULL` skips rendering and consumes
///        queued tile lights.
/// @param cam_x Camera world X coordinate subtracted from dynamic-light X.
/// @param cam_y Camera world Y coordinate subtracted from dynamic-light Y.
/// @param player_sx Player-glow center X in screen pixels.
/// @param player_sy Player-glow center Y in screen pixels.
/// @details Call after drawing scene entities. The function draws the tinted
///          full-canvas overlay, player glow, world-space dynamic lights, and
///          screen-space tile lights, in that order. Glow discs are blended
///          source-over and do not cut holes through the darkness. Darkness
///          zero skips all rendering. Every call consumes queued tile lights;
///          dynamic lights remain until cleared or expired. A non-null
///          wrong-class Lighting2D handle raises a trap.
void rt_lighting2d_draw(rt_lighting2d lit,
                        void *canvas,
                        int64_t cam_x,
                        int64_t cam_y,
                        int64_t player_sx,
                        int64_t player_sy);

// Queries

/// @brief Count currently active dynamic lights.
/// @param lit Lighting2D instance to query.
/// @return The active dynamic-light count, or zero for `NULL`.
/// @details Player and tile lights are excluded. A non-null handle of another
///          runtime class raises a trap.
int64_t rt_lighting2d_get_light_count(rt_lighting2d lit);

/// @brief Read the player light's configured base radius.
/// @param lit Lighting2D instance to query.
/// @return The nonnegative base radius in pixels, or zero for `NULL`. Zero
///         means no player glow is drawn.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_lighting2d_get_player_radius(rt_lighting2d lit);

#ifdef __cplusplus
}
#endif
