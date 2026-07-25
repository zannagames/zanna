//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_platformer_ctrl.h
/// @file
/// @brief Declares a stateful platformer input-to-velocity controller.
//
// Purpose: Platformer input controller with jump buffer, coyote time,
//   variable jump height, and acceleration/deceleration curves.
//
// Key invariants:
//   - All timing is ms-based (delta-time independent).
//   - Update() must be called once per frame with dt and input state.
//   - ShouldJump is a one-shot flag consumed on read.
//
// Ownership/Lifetime:
//   - Runtime reference-counted; destroy releases one handle reference.
//
// Links: src/runtime/game/rt_platformer_ctrl.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to a runtime reference-counted controller.
typedef struct rt_platformer_ctrl_impl *rt_platformer_ctrl;

/// @brief Create a platformer input controller with default tuning.
/// @return New controller reference, or `NULL` on allocation failure.
/// @details Defaults include 100ms jump buffering, 80ms coyote time, facing
///          right, full/cut jump velocities -1500/-600, and gravity 78.
rt_platformer_ctrl rt_platformer_ctrl_new(void);

/// @brief Release one runtime reference to a controller.
/// @param ctrl Controller to release; `NULL` is a no-op.
void rt_platformer_ctrl_destroy(rt_platformer_ctrl ctrl);

// Configuration
/// @brief Set the jump-buffer window in ms (jump-press just before landing still triggers).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ms Unvalidated duration; zero or negative effectively disables it.
void rt_platformer_ctrl_set_jump_buffer(rt_platformer_ctrl ctrl, int64_t ms);
/// @brief Set the coyote-time window in ms (jump still works just after walking off a ledge).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ms Unvalidated duration; zero or negative effectively disables it.
void rt_platformer_ctrl_set_coyote_time(rt_platformer_ctrl ctrl, int64_t ms);
/// @brief Set per-axis acceleration: ground accel, in-air accel, and deceleration when no input.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ground Grounded acceleration per 16ms baseline frame.
/// @param air Airborne acceleration per baseline frame.
/// @param decel Neutral-input deceleration per baseline frame.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_acceleration(rt_platformer_ctrl ctrl,
                                         int64_t ground,
                                         int64_t air,
                                         int64_t decel);
/// @brief Set jump force: @p full_force is the initial impulse, @p cut_force is the minimum
/// kept when the jump button is released early (variable jump height).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param full_force Full impulse returned to the caller, normally negative.
/// @param cut_force Upward-velocity cap on a jump-release edge.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_jump_force(rt_platformer_ctrl ctrl,
                                       int64_t full_force,
                                       int64_t cut_force);
/// @brief Set normal and sprint horizontal velocity caps.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param normal Nonsprinting cap in centipixels per baseline frame.
/// @param sprint Sprinting cap in the same units.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_max_speed(rt_platformer_ctrl ctrl, int64_t normal, int64_t sprint);
/// @brief Set gravity (per-tick velocity delta) and terminal-velocity cap.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param gravity Velocity delta per 16ms baseline frame.
/// @param max_fall Maximum downward velocity.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_gravity(rt_platformer_ctrl ctrl, int64_t gravity, int64_t max_fall);
/// @brief Apply reduced gravity near the apex of a jump (smoother feel) — threshold in
/// centipixels/s of |vy|, gravity_mult_pct in percent (50 = half gravity at apex).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param threshold Absolute vertical-velocity threshold.
/// @param gravity_mult_pct Percentage applied to gravity near the apex.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_apex_bonus(rt_platformer_ctrl ctrl,
                                       int64_t threshold,
                                       int64_t gravity_mult_pct);

// Per-frame update
/// @brief Tick the controller: process input flags, integrate velocity, decay timers.
/// @param ctrl Controller to update; `NULL` is a no-op.
/// @param dt Elapsed milliseconds; nonpositive values do nothing.
/// @param input_left Nonzero while left is held.
/// @param input_right Nonzero while right is held; wins when both are nonzero.
/// @param jump_pressed Nonzero on the press edge.
/// @param jump_held Nonzero while held.
/// @param on_ground Nonzero while grounded.
/// @param sprint Nonzero to select the sprint cap.
/// @details Values scale by integer `dt / 16`. Position integration and
///          application of the full jump impulse remain caller responsibilities.
void rt_platformer_ctrl_update(rt_platformer_ctrl ctrl,
                               int64_t dt,
                               int8_t input_left,
                               int8_t input_right,
                               int8_t jump_pressed,
                               int8_t jump_held,
                               int8_t on_ground,
                               int8_t sprint);

// Output queries
/// @brief Read horizontal velocity.
/// @param ctrl Controller to query.
/// @return Centipixels per 16ms baseline frame, or zero for `NULL`.
int64_t rt_platformer_ctrl_get_vx(rt_platformer_ctrl ctrl);
/// @brief Read vertical velocity.
/// @param ctrl Controller to query.
/// @return Centipixels per 16ms baseline frame, positive downward, or zero for
///         `NULL`.
int64_t rt_platformer_ctrl_get_vy(rt_platformer_ctrl ctrl);
/// @brief One-shot flag: true if the controller wants to jump *this* frame (consumed on read).
/// @param ctrl Controller whose pending signal is consumed.
/// @return `1` once after buffered/coyote jump resolution, otherwise `0`.
int8_t rt_platformer_ctrl_should_jump(rt_platformer_ctrl ctrl);
/// @brief Get the jump impulse magnitude to apply when ShouldJump is consumed.
/// @param ctrl Controller to query.
/// @return Configured full jump force, normally negative, or zero for `NULL`.
int64_t rt_platformer_ctrl_get_jump_force(rt_platformer_ctrl ctrl);
/// @brief Read sticky facing direction.
/// @param ctrl Controller to query.
/// @return `-1` for left or `1` for right; null also returns `1`.
/// @details New controllers face right. Neutral deceleration does not change it.
int64_t rt_platformer_ctrl_get_facing(rt_platformer_ctrl ctrl);
/// @brief Test whether horizontal velocity is nonzero.
/// @param ctrl Controller to query.
/// @return `1` exactly when `vx != 0`, otherwise `0`.
int8_t rt_platformer_ctrl_is_moving(rt_platformer_ctrl ctrl);

// Direct velocity control (for external physics like wall jump, knockback)
/// @brief Override horizontal velocity (e.g., for wall-jump or knockback impulses).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param vx New unvalidated horizontal velocity.
void rt_platformer_ctrl_set_vx(rt_platformer_ctrl ctrl, int64_t vx);
/// @brief Override vertical velocity (e.g., for spring pads or hard knockback).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param vy New unvalidated vertical velocity; negative is upward.
void rt_platformer_ctrl_set_vy(rt_platformer_ctrl ctrl, int64_t vy);

#ifdef __cplusplus
}
#endif
