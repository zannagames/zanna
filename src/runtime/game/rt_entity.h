//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_entity.h
/// @file
/// @brief Declares fixed-point entities with swept TileMap collision.
//
// Purpose: Lightweight 2D game entity combining position, velocity, size,
//          health, and built-in tilemap collision resolution. Replaces the
//          common pattern of parallel arrays + manual gravity/moveAndCollide.
//
// Key invariants:
//   - Position (x, y) is in centipixels (x100 for subpixel precision).
//   - Velocity (vx, vy) is in centipixels per 16ms base frame.
//   - Size (width, height) is in pixels.
//   - Collision flags (on_ground, hit_left, hit_right, hit_ceiling) are set
//     by MoveAndCollide/UpdatePhysics and valid until the next call.
//
// Ownership/Lifetime:
//   - Entities are runtime-reference-counted and expose no module-specific
//     destroy entry point.
//   - Entities retain no external objects and require no custom finalizer.
//
// Links: rt_entity.c (implementation)
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate opaque Entity handles.
#define RT_ENTITY_CLASS_ID INT64_C(-0x510206)

/// @brief Create a 2D entity at centipixel coordinates (x, y) with size (w × h) pixels.
/// @param x Initial X position in centipixels.
/// @param y Initial Y position in centipixels.
/// @param w Width in pixels; nonpositive values become one.
/// @param h Height in pixels; nonpositive values become one.
/// @return A new Entity reference, or `NULL` if allocation fails.
/// @details Velocity, HP, maximum HP, type, and collision flags begin at zero.
///          Direction begins right (`1`) and active begins true.
void *rt_entity_new(int64_t x, int64_t y, int64_t w, int64_t h);

// Position (centipixels)
/// @brief Get the entity's X position in centipixels.
/// @param ent Entity to query.
/// @return Stored X coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_x(void *ent);

/// @brief Get the entity's Y position in centipixels.
/// @param ent Entity to query.
/// @return Stored Y coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_y(void *ent);

/// @brief Set the entity's X position in centipixels.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param x New coordinate, stored verbatim without collision evaluation.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_x(void *ent, int64_t x);

/// @brief Set the entity's Y position in centipixels.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param y New coordinate, stored verbatim without collision evaluation.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_y(void *ent, int64_t y);

// Velocity (centipixels per 16ms)
/// @brief Get horizontal velocity in centipixels per 16ms base frame.
/// @param ent Entity to query.
/// @return Stored horizontal velocity, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_vx(void *ent);

/// @brief Get vertical velocity in centipixels per 16ms (positive = downward).
/// @param ent Entity to query.
/// @return Stored vertical velocity, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_vy(void *ent);

/// @brief Set horizontal velocity.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param vx New centipixels-per-base-frame velocity, stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_vx(void *ent, int64_t vx);

/// @brief Set vertical velocity.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param vy New centipixels-per-base-frame velocity; positive moves downward.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_vy(void *ent, int64_t vy);

// Size (pixels)
/// @brief Get the entity's collision width in pixels.
/// @param ent Entity to query.
/// @return Positive stored width, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_width(void *ent);

/// @brief Get the entity's collision height in pixels.
/// @param ent Entity to query.
/// @return Positive stored height, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_height(void *ent);

// Direction: 1=right, -1=left
/// @brief Get the entity's facing direction (1 = right, -1 = left).
/// @param ent Entity to query.
/// @return Stored direction, or `1` for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_dir(void *ent);

/// @brief Set the entity's facing direction (clamped to +1 / -1).
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param dir Negative selects `-1`; zero or positive selects `1`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_dir(void *ent, int64_t dir);

// HP
/// @brief Get the current HP, which is stored without range enforcement.
/// @param ent Entity to query.
/// @return Stored HP, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_hp(void *ent);

/// @brief Set the current HP.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param hp Value stored verbatim; it is not clamped to maximum HP.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_hp(void *ent, int64_t hp);

/// @brief Get the maximum HP cap.
/// @param ent Entity to query.
/// @return Stored maximum HP, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_max_hp(void *ent);

/// @brief Set the maximum HP cap (does not change current HP).
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param hp Value stored verbatim without range enforcement.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_max_hp(void *ent, int64_t hp);

// Type ID (user-defined)
/// @brief Get the user-defined entity type ID (e.g., player=0, enemy=1, pickup=2).
/// @param ent Entity to query.
/// @return Stored type tag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_entity_get_type(void *ent);

/// @brief Set the user-defined entity type ID.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param type Game-defined value stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_type(void *ent, int64_t type);

// Active flag
/// @brief Get the active flag (inactive entities are typically skipped by update/draw).
/// @param ent Entity to query.
/// @return Normalized active flag, or zero for a null or invalid handle.
/// @details Physics functions do not consult this flag. A non-null invalid
///          handle raises a runtime trap.
int8_t rt_entity_get_active(void *ent);

/// @brief Set the active flag.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param active Zero clears the flag; any nonzero value sets it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_entity_set_active(void *ent, int8_t active);

// Collision flags (set by MoveAndCollide)
/// @brief True if the entity is currently standing on a solid tile (set by MoveAndCollide).
/// @param ent Entity to query.
/// @return Ground-contact flag from the last positive-delta move, or zero for
///         a null or invalid handle.
/// @details A resting-ground probe can set this without vertical displacement.
///          A non-null invalid handle raises a runtime trap.
int8_t rt_entity_on_ground(void *ent);

/// @brief True if the entity collided with a wall to its left during the last MoveAndCollide.
/// @param ent Entity to query.
/// @return Left-hit flag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_entity_hit_left(void *ent);

/// @brief True if the entity collided with a wall to its right during the last MoveAndCollide.
/// @param ent Entity to query.
/// @return Right-hit flag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_entity_hit_right(void *ent);

/// @brief True if the entity collided with a ceiling during the last MoveAndCollide.
/// @param ent Entity to query.
/// @return Ceiling-hit flag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_entity_hit_ceiling(void *ent);

/// @brief Apply gravity to vy, capped at max_fall. Call once per frame.
/// @param ent Entity whose vertical velocity is updated.
/// @param gravity Acceleration in centipixels per base-frame squared.
/// @param max_fall Maximum downward velocity; negative values become zero.
/// @param dt Positive elapsed milliseconds; nonpositive values are ignored.
/// @details Time scaling and addition saturate. Only downward velocity above
///          the cap is clamped; upward velocity is not symmetrically limited.
///          A non-null invalid Entity handle raises a runtime trap.
void rt_entity_apply_gravity(void *ent, int64_t gravity, int64_t max_fall, int64_t dt);

/// @brief Move by velocity*dt, resolve against tilemap with swept tile checks.
/// @param ent Entity to move.
/// @param tilemap TileMap used for solidity queries, or `NULL` for free motion.
/// @param dt Positive elapsed milliseconds; nonpositive values leave position
///        and flags unchanged.
/// @details A positive call resets collision flags, preserves centipixel
///          precision, sweeps X before Y, zeros velocity on impact, and probes
///          beneath the final box for persistent ground contact. A null TileMap
///          performs saturated free movement. Nonpositive tile dimensions
///          leave reset flags and position unchanged. Invalid non-null runtime
///          handles may raise class traps.
void rt_entity_move_and_collide(void *ent, void *tilemap, int64_t dt);

/// @brief Apply gravity + move + collide in one call.
/// @param ent Entity to update.
/// @param tilemap TileMap used for collision, or `NULL` for free movement.
/// @param gravity Acceleration passed to rt_entity_apply_gravity().
/// @param max_fall Downward velocity cap passed to rt_entity_apply_gravity().
/// @param dt Elapsed milliseconds passed unchanged to both operations.
/// @details Gravity runs before movement. Each underlying call retains its own
///          no-op and trap behavior.
void rt_entity_update_physics(
    void *ent, void *tilemap, int64_t gravity, int64_t max_fall, int64_t dt);

/// @brief Check if entity is at a platform edge (no solid tile below leading edge).
/// @param ent Entity whose facing-side foot position is sampled.
/// @param tilemap TileMap to query.
/// @return `1` when the point two pixels below and just beyond the facing edge
///         is not solid; otherwise `0`.
/// @details Null arguments return zero. Invalid non-null runtime handles may
///          raise class traps.
int8_t rt_entity_at_edge(void *ent, void *tilemap);

/// @brief Update patrol velocity and direction from the last wall-hit flags.
/// @param ent Entity to mutate.
/// @param speed Velocity assigned after a left hit; its saturating negation is
///        assigned after a right hit.
/// @details A right hit wins if both flags are set. No hit leaves the Entity
///          unchanged. A non-null invalid Entity handle raises a runtime trap.
void rt_entity_patrol_reverse(void *ent, int64_t speed);

/// @brief Check AABB overlap with another entity.
/// @param ent First Entity.
/// @param other Second Entity.
/// @return `1` for positive-area overlap of their floor-converted pixel boxes;
///         otherwise `0`.
/// @details Edge-only contact does not count. Null handles return zero and
///          invalid non-null Entity handles raise runtime traps.
int8_t rt_entity_overlaps(void *ent, void *other);

#ifdef __cplusplus
}
#endif
