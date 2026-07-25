//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_behavior.h
// Purpose: Composable behavior presets for 2D game AI. Combines common patterns
//          (patrol, chase, gravity, edge reverse, shoot, animation) into a
//          single Update() call that operates on an Entity + Tilemap.
//
// Key invariants:
//   - Behaviors are flag-based (bitmask). Multiple can be active simultaneously.
//   - Update() applies all enabled behaviors in a fixed order.
//   - Requires Entity (Plan 03) for position/velocity/collision.
//
// Ownership/Lifetime:
//   - Behavior objects contain scalar configuration/timers and need no finalizer.
//   - Update borrows the Behavior, Entity, and optional TileMap.
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Composable fixed-order 2D Entity behavior presets.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Runtime class ID used to validate Behavior handles.
#define RT_BEHAVIOR_CLASS_ID INT64_C(-0x510217)

/// @brief Create an empty behavior bundle (no behaviors enabled yet).
/// @return Owned zero-initialized Behavior, or NULL on allocation.
void *rt_behavior_new(void);
/// @brief Enable horizontal patrol — moves at @p speed, reversing on edges/walls if those bits set.
/// @param bhv Borrowed Behavior.
/// @param speed Signed horizontal speed.
void rt_behavior_add_patrol(void *bhv, int64_t speed);
/// @brief Enable chase — moves toward target at @p speed when within @p range pixels.
/// @param bhv Borrowed Behavior.
/// @param speed Signed chase speed.
/// @param range Positive pixel radius; non-positive disables matching.
void rt_behavior_add_chase(void *bhv, int64_t speed, int64_t range);
/// @brief Enable gravity — accelerates downward by @p gravity per tick, capped at @p max_fall.
/// @param bhv Borrowed Behavior.
/// @param gravity Signed acceleration.
/// @param max_fall Positive terminal speed; non-positive becomes zero.
void rt_behavior_add_gravity(void *bhv, int64_t gravity, int64_t max_fall);
/// @brief Enable edge-reverse — flips facing/velocity when about to walk off a tile edge.
/// @param bhv Borrowed Behavior.
void rt_behavior_add_edge_reverse(void *bhv);
/// @brief Enable wall-reverse — flips facing/velocity when blocked by a tile wall.
/// @param bhv Borrowed Behavior.
void rt_behavior_add_wall_reverse(void *bhv);
/// @brief Enable periodic shooting with @p cooldown_ms between shots (use ShootReady to query).
/// @param bhv Borrowed Behavior.
/// @param cooldown_ms Positive cooldown; non-positive becomes one.
void rt_behavior_add_shoot(void *bhv, int64_t cooldown_ms);
/// @brief Enable sine-wave vertical velocity; amplitude is a centipixel/base-frame velocity and
/// speed advances centidegrees by (speed * dt) / 16 (VDOC-240).
/// @param bhv Borrowed Behavior.
/// @param amplitude Signed vertical velocity amplitude.
/// @param speed Signed phase speed.
void rt_behavior_add_sine_float(void *bhv, int64_t amplitude, int64_t speed);
/// @brief Enable looping sprite animation with @p frame_count frames at @p ms_per_frame each.
/// @param bhv Borrowed Behavior.
/// @param frame_count Positive frame modulus; non-positive becomes one.
/// @param ms_per_frame Positive interval; non-positive becomes one.
void rt_behavior_add_anim_loop(void *bhv, int64_t frame_count, int64_t ms_per_frame);

/// @brief Apply all enabled behaviors to an entity.
/// @param bhv Borrowed Behavior.
/// @param entity Borrowed Entity; NULL makes the call a no-op.
/// @param tilemap Borrowed optional TileMap.
/// @param target_x Chase target X in pixels.
/// @param target_y Chase target Y in pixels.
/// @param dt Positive tick duration.
void rt_behavior_update(
    void *bhv, void *entity, void *tilemap, int64_t target_x, int64_t target_y, int64_t dt);

/// @brief One-shot flag (consumed on read): true when the shoot cooldown has elapsed.
/// @param bhv Borrowed Behavior.
/// @return One once per cooldown; otherwise zero.
int8_t rt_behavior_shoot_ready(void *bhv);
/// @brief Get the current frame index of the looping animation (0..frame_count-1).
/// @param bhv Borrowed Behavior.
/// @return Current frame, or zero on invalid input.
int64_t rt_behavior_anim_frame(void *bhv);

#ifdef __cplusplus
}
#endif
