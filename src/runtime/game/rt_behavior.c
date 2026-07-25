//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_behavior.c
// Purpose: Composable behavior presets for 2D game AI.
//
// Key invariants:
//   - Behaviors are flag-based bitmask, applied in fixed order.
//   - Update order: gravity → patrol → chase → sine float → move+collide →
//     wall reverse → edge reverse → shoot cooldown → animation.
//
// Ownership/Lifetime:
//   - Behavior objects store only scalar preset state and need no finalizer.
//   - Update borrows Behavior, Entity, and optional TileMap handles.
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Composable fixed-order 2D entity behavior presets.

#include "rt_behavior.h"
#include "rt_entity.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BHV_PATROL (1u << 0)
#define BHV_CHASE (1u << 1)
#define BHV_GRAVITY (1u << 2)
#define BHV_EDGE_REVERSE (1u << 3)
#define BHV_WALL_REVERSE (1u << 4)
#define BHV_SHOOT (1u << 5)
#define BHV_SINE_FLOAT (1u << 6)
#define BHV_ANIM_LOOP (1u << 7)

/// @brief True if the target is within (0, @p range] of the entity (squared
///        distance compare; coincident points return 0).
/// @param entity_x Entity X in pixels.
/// @param entity_y Entity Y in pixels.
/// @param target_x Target X in pixels.
/// @param target_y Target Y in pixels.
/// @param range Positive chase radius in pixels.
/// @return One for a distinct target within range; otherwise zero.
static int8_t behavior_target_in_chase_range(
    int64_t entity_x, int64_t entity_y, int64_t target_x, int64_t target_y, int64_t range) {
    if (range <= 0)
        return 0;
    long double dx = (long double)target_x - (long double)entity_x;
    long double dy = (long double)target_y - (long double)entity_y;
    long double dist2 = dx * dx + dy * dy;
    long double range2 = (long double)range * (long double)range;
    return (dist2 <= range2 && dist2 > 0.0L) ? 1 : 0;
}

/// @brief Saturating negation (INT64_MIN -> INT64_MAX).
/// @param value Signed value.
/// @return Negated value, saturating the unrepresentable minimum.
static int64_t behavior_neg_sat_i64(int64_t value) {
    return value == INT64_MIN ? INT64_MAX : -value;
}

/// @brief Subtract @p amount from @p value, clamping at zero (no underflow).
/// @param value Nonnegative countdown value.
/// @param amount Delta; non-positive values are ignored.
/// @return Remaining value clamped to zero.
static int64_t behavior_sub_to_zero_i64(int64_t value, int64_t amount) {
    if (amount <= 0)
        return value;
    return amount >= value ? 0 : value - amount;
}

/// @brief Phase increment for an oscillating behavior: (speed*dt)/16 wrapped
///        into the 0..36000 centidegree phase space. 0 on non-finite input.
/// @param speed Configured angular speed.
/// @param dt Positive tick duration.
/// @return Truncated signed centidegree delta reduced modulo 36000.
static int64_t behavior_phase_delta(int64_t speed, int64_t dt) {
    long double value = ((long double)speed * (long double)dt) / 16.0L;
    if (!isfinite(value))
        return 0;
    value = fmodl(value, 36000.0L);
    if (value > (long double)INT64_MAX)
        return INT64_MAX;
    if (value < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Wrap a phase into [0, 36000) centidegrees (360.00° in 0.01° units).
/// @param phase Signed centidegree phase.
/// @return Equivalent nonnegative phase.
static int64_t behavior_wrap_phase(int64_t phase) {
    phase %= 36000;
    if (phase < 0)
        phase += 36000;
    return phase;
}

/// @brief Modular addition ((a + b) mod @p mod) with non-negative result and
///        overflow-safe intermediate reduction. Returns 0 if @p mod <= 0.
/// @param a First addend.
/// @param b Second addend.
/// @param mod Positive modulus.
/// @return Nonnegative modular sum, or zero for invalid modulus.
static int64_t behavior_mod_add_i64(int64_t a, int64_t b, int64_t mod) {
    if (mod <= 0)
        return 0;
    int64_t lhs = a % mod;
    int64_t rhs = b % mod;
    if (lhs < 0)
        lhs += mod;
    if (rhs < 0)
        rhs += mod;
    if (lhs >= mod - rhs)
        return lhs - (mod - rhs);
    return lhs + rhs;
}

/// @brief Scalar configuration and mutable timers for one Behavior object.
typedef struct {
    uint32_t flags;

    // Patrol
    int64_t patrol_speed;

    // Chase
    int64_t chase_speed;
    int64_t chase_range; // Pixels (not centipixels)

    // Gravity
    int64_t gravity;
    int64_t max_fall;

    // Shoot
    int64_t shoot_cooldown_ms;
    int64_t shoot_timer;
    int8_t shoot_ready;

    // Sine float
    int64_t float_amplitude;
    int64_t float_speed;
    int64_t float_phase; // Accumulator (degrees * 100)

    // Animation
    int64_t anim_frames;
    int64_t anim_ms;
    int64_t anim_timer;
    int64_t anim_frame;
} behavior_impl;

/// @brief Safe-cast a handle to the Behavior impl, trapping @p api on a
///        class-id mismatch.
/// @param bhv Borrowed candidate handle; may be NULL.
/// @param api Borrowed mismatch diagnostic.
/// @return Valid implementation pointer, or NULL.
static behavior_impl *checked_behavior(void *bhv, const char *api) {
    if (!bhv)
        return NULL;
    if (rt_obj_class_id(bhv) != RT_BEHAVIOR_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (behavior_impl *)bhv;
}

/// @brief Create a new composable AI behavior controller.
/// @details Behaviors are built by adding presets (patrol, chase, gravity, etc.)
///          via the add_* methods. Update applies all active behaviors to an Entity
///          in a fixed priority order: gravity → patrol → chase → sine float →
///          move+collide → wall/edge reverse → shoot cooldown → animation.
/// @return Owned zero-initialized Behavior object, or NULL on allocation.
void *rt_behavior_new(void) {
    behavior_impl *b =
        (behavior_impl *)rt_obj_new_i64(RT_BEHAVIOR_CLASS_ID, (int64_t)sizeof(behavior_impl));
    if (!b)
        return NULL;
    memset(b, 0, sizeof(behavior_impl));
    return b;
}

/// @brief Add horizontal patrol behavior (entity walks left/right at given speed).
/// @param bhv Borrowed Behavior.
/// @param speed Signed horizontal speed.
void rt_behavior_add_patrol(void *bhv, int64_t speed) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddPatrol: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_PATROL;
    b->patrol_speed = speed;
}

/// @brief Add chase-target behavior (entity moves toward the target when within range).
/// @param bhv Borrowed Behavior.
/// @param speed Signed chase speed.
/// @param range Chase radius in pixels; non-positive disables range matching.
void rt_behavior_add_chase(void *bhv, int64_t speed, int64_t range) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddChase: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_CHASE;
    b->chase_speed = speed;
    b->chase_range = range > 0 ? range : 0;
}

/// @brief Add gravity behavior (applies downward acceleration with a terminal velocity).
/// @param bhv Borrowed Behavior.
/// @param gravity Signed acceleration passed to Entity.
/// @param max_fall Positive terminal speed; non-positive becomes zero.
void rt_behavior_add_gravity(void *bhv, int64_t gravity, int64_t max_fall) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddGravity: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_GRAVITY;
    b->gravity = gravity;
    b->max_fall = max_fall > 0 ? max_fall : 0;
}

/// @brief Add edge-reverse behavior (entity turns around at platform edges).
/// @param bhv Borrowed Behavior.
void rt_behavior_add_edge_reverse(void *bhv) {
    behavior_impl *b =
        checked_behavior(bhv, "Behavior.AddEdgeReverse: expected Zanna.Game.Behavior");
    if (b)
        b->flags |= BHV_EDGE_REVERSE;
}

/// @brief Add wall-reverse behavior (entity turns around when hitting a wall).
/// @param bhv Borrowed Behavior.
void rt_behavior_add_wall_reverse(void *bhv) {
    behavior_impl *b =
        checked_behavior(bhv, "Behavior.AddWallReverse: expected Zanna.Game.Behavior");
    if (b)
        b->flags |= BHV_WALL_REVERSE;
}

/// @brief Add shoot-on-cooldown behavior (shoot_ready flag sets after cooldown elapses).
/// @param bhv Borrowed Behavior.
/// @param cooldown_ms Positive cooldown; non-positive becomes one.
void rt_behavior_add_shoot(void *bhv, int64_t cooldown_ms) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddShoot: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_SHOOT;
    b->shoot_cooldown_ms = cooldown_ms > 0 ? cooldown_ms : 1;
    b->shoot_timer = b->shoot_cooldown_ms;
}

/// @brief Add sine-wave floating behavior (vertical oscillation for hovering enemies).
/// @param bhv Borrowed Behavior.
/// @param amplitude Signed vertical velocity amplitude.
/// @param speed Signed phase speed.
void rt_behavior_add_sine_float(void *bhv, int64_t amplitude, int64_t speed) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddSineFloat: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_SINE_FLOAT;
    b->float_amplitude = amplitude;
    b->float_speed = speed;
}

/// @brief Add frame-based animation loop (cycles through frames at the given interval).
/// @param bhv Borrowed Behavior.
/// @param frame_count Frame modulus; non-positive becomes one.
/// @param ms_per_frame Positive interval; non-positive becomes one.
void rt_behavior_add_anim_loop(void *bhv, int64_t frame_count, int64_t ms_per_frame) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AddAnimLoop: expected Zanna.Game.Behavior");
    if (!b)
        return;
    b->flags |= BHV_ANIM_LOOP;
    b->anim_frames = frame_count > 0 ? frame_count : 1;
    b->anim_ms = ms_per_frame > 0 ? ms_per_frame : 1;
}

/// @brief Run all active behaviors on an entity for one tick.
/// @details Applies behaviors in priority order: gravity, patrol, chase, sine
///          float, move+collide, wall reverse, edge reverse, shoot cooldown,
///          then animation loop. The target_x/target_y are the chase target position.
/// @param bhv Borrowed Behavior.
/// @param entity Borrowed Entity; NULL makes the call a no-op.
/// @param tilemap Borrowed optional TileMap passed to collision/edge queries.
/// @param target_x Chase target X in pixels.
/// @param target_y Chase target Y in pixels.
/// @param dt Positive tick duration; non-positive is ignored.
void rt_behavior_update(
    void *bhv, void *entity, void *tilemap, int64_t target_x, int64_t target_y, int64_t dt) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.Update: expected Zanna.Game.Behavior");
    if (!b || !entity || dt <= 0)
        return;
    uint32_t f = b->flags;

    // 1. Gravity
    if (f & BHV_GRAVITY)
        rt_entity_apply_gravity(entity, b->gravity, b->max_fall, dt);

    // 2. Patrol: set vx based on direction
    if (f & BHV_PATROL) {
        int64_t dir = rt_entity_get_dir(entity);
        rt_entity_set_vx(entity, dir > 0 ? b->patrol_speed : behavior_neg_sat_i64(b->patrol_speed));
    }

    // 3. Chase: override vx if target is in range
    if (f & BHV_CHASE) {
        int64_t ex = rt_entity_get_x(entity) / 100; // Convert to pixels
        int64_t ey = rt_entity_get_y(entity) / 100;
        if (behavior_target_in_chase_range(ex, ey, target_x, target_y, b->chase_range)) {
            if (target_x > ex) {
                rt_entity_set_vx(entity, b->chase_speed);
                rt_entity_set_dir(entity, 1);
            } else {
                rt_entity_set_vx(entity, behavior_neg_sat_i64(b->chase_speed));
                rt_entity_set_dir(entity, -1);
            }
        }
    }

    // 4. Sine float: add vertical oscillation
    if (f & BHV_SINE_FLOAT) {
        b->float_phase = behavior_wrap_phase(
            behavior_mod_add_i64(b->float_phase, behavior_phase_delta(b->float_speed, dt), 36000));
        double rad = (double)b->float_phase * 3.14159265 / 18000.0;
        int64_t offset = (int64_t)(sin(rad) * (double)b->float_amplitude);
        rt_entity_set_vy(entity, offset);
    }

    // 5. Move + collide
    rt_entity_move_and_collide(entity, tilemap, dt);

    // 6. Wall reverse
    if (f & BHV_WALL_REVERSE)
        rt_entity_patrol_reverse(entity, b->patrol_speed > 0 ? b->patrol_speed : b->chase_speed);

    // 7. Edge reverse
    if ((f & BHV_EDGE_REVERSE) && rt_entity_on_ground(entity)) {
        if (rt_entity_at_edge(entity, tilemap)) {
            int64_t dir = rt_entity_get_dir(entity);
            rt_entity_set_dir(entity, -dir);
            int64_t spd = b->patrol_speed > 0 ? b->patrol_speed : b->chase_speed;
            rt_entity_set_vx(entity, -dir > 0 ? spd : behavior_neg_sat_i64(spd));
        }
    }

    // 8. Shoot cooldown
    if (f & BHV_SHOOT) {
        b->shoot_timer = behavior_sub_to_zero_i64(b->shoot_timer, dt);
        if (b->shoot_timer <= 0) {
            b->shoot_ready = 1;
            b->shoot_timer = b->shoot_cooldown_ms;
        }
    }

    // 9. Animation loop
    if ((f & BHV_ANIM_LOOP) && b->anim_ms > 0 && b->anim_frames > 0) {
        if (dt > INT64_MAX - b->anim_timer)
            b->anim_timer = INT64_MAX;
        else
            b->anim_timer += dt;

        int64_t steps = b->anim_timer / b->anim_ms;
        if (steps > 0) {
            b->anim_timer %= b->anim_ms;
            b->anim_frame = behavior_mod_add_i64(b->anim_frame, steps, b->anim_frames);
        }
    }
}

/// @brief Check and consume the shoot-ready flag (returns 1 once, then resets).
/// @param bhv Borrowed Behavior.
/// @return One once per elapsed cooldown; otherwise zero.
int8_t rt_behavior_shoot_ready(void *bhv) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.ShootReady: expected Zanna.Game.Behavior");
    if (!b)
        return 0;
    if (b->shoot_ready) {
        b->shoot_ready = 0;
        return 1;
    }
    return 0;
}

/// @brief Get the current animation frame index from the animation loop behavior.
/// @param bhv Borrowed Behavior.
/// @return Current modular frame index, or zero on invalid input.
int64_t rt_behavior_anim_frame(void *bhv) {
    behavior_impl *b = checked_behavior(bhv, "Behavior.AnimFrame: expected Zanna.Game.Behavior");
    return b ? b->anim_frame : 0;
}
