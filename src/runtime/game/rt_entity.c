//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_entity.c
/// @file
/// @brief Implements fixed-point game entities and swept tile collisions.
//
// Purpose: Lightweight 2D game entity with built-in tilemap collision.
//          Extracts the common gravity + moveAndCollide + collision response
//          pattern used by every game object in platformers/sidescrollers.
//
// Key invariants:
//   - Position in centipixels (x100). Collision in pixel coordinates.
//   - MoveAndCollide resolves X axis first, then Y axis.
//   - Collision flags reset at the start of each positive-delta
//     MoveAndCollide call.
//   - DT_BASE = 16 (milliseconds per base frame at 60fps).
//
// Ownership/Lifetime:
//   - Entities are runtime-reference-counted and hold no external references,
//     so they require no custom finalizer.
//
// Links: rt_entity.h
//
//===----------------------------------------------------------------------===//

#include "rt_entity.h"
#include "rt_object.h"
#include "rt_tilemap.h"
#include "rt_trap.h"

#include <limits.h>
#include <string.h>

/// @brief Milliseconds represented by one stored velocity unit interval.
#define DT_BASE 16

//=============================================================================
// Internal struct
//=============================================================================

/// @brief Private fixed-point state stored in each runtime Entity object.
typedef struct {
    int64_t x, y;          // Centipixels (x100)
    int64_t vx, vy;        // Centipixels per DT_BASE ms
    int64_t width, height; // Pixels
    int64_t dir;           // 1 = right, -1 = left
    int64_t hp, max_hp;
    int64_t type;
    int8_t active;
    int8_t on_ground;
    int8_t hit_left, hit_right, hit_ceiling;
} entity_impl;

/// @brief Unchecked cast of an opaque handle to entity_impl.
/// @param ent Opaque pointer assumed to address an Entity payload.
/// @return @p ent reinterpreted as the private implementation type.
static entity_impl *get(void *ent) {
    return (entity_impl *)ent;
}

/// @brief Validate and cast an opaque Entity handle.
/// @param ent Candidate handle; `NULL` is accepted.
/// @param api Trap message used when @p ent has the wrong runtime class ID.
/// @return The private Entity payload when valid; otherwise `NULL`.
/// @details A non-null mismatched handle raises a runtime trap before this
///          function returns `NULL`.
static entity_impl *checked_entity(void *ent, const char *api) {
    if (!ent)
        return NULL;
    if (rt_obj_class_id(ent) != RT_ENTITY_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return get(ent);
}

/// @brief Floor division (rounds toward negative infinity); 0 on divide-by-0.
/// @param value Dividend.
/// @param divisor Divisor.
/// @return Mathematical floor of @p value divided by @p divisor, or zero when
///         @p divisor is zero.
static int64_t entity_floor_div(int64_t value, int64_t divisor) {
    if (divisor == 0)
        return 0;
    int64_t quot = value / divisor;
    int64_t rem = value % divisor;
    if (rem != 0 && ((rem < 0) != (divisor < 0)))
        quot--;
    return quot;
}

/// @brief Saturating int64 addition (clamps to INT64_MIN/MAX on overflow).
/// @param a First addend.
/// @param b Second addend.
/// @return The exact sum when representable, otherwise the nearest `int64_t`
///         endpoint.
static int64_t entity_saturating_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Saturating negation (INT64_MIN -> INT64_MAX).
/// @param value Integer to negate.
/// @return `-value`, except `INT64_MIN` maps to `INT64_MAX`.
static int64_t entity_saturating_neg(int64_t value) {
    return value == INT64_MIN ? INT64_MAX : -value;
}

/// @brief Saturating int64 multiply (128-bit or long-double widened; clamps).
/// @param a First factor.
/// @param b Second factor.
/// @return The exact product when representable, otherwise `INT64_MIN` or
///         `INT64_MAX` according to its sign.
static int64_t entity_saturating_mul(int64_t a, int64_t b) {
#if defined(__SIZEOF_INT128__)
    __int128 result = (__int128)a * (__int128)b;
    if (result > INT64_MAX)
        return INT64_MAX;
    if (result < INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
#else
    long double result = (long double)a * (long double)b;
    if (result > (long double)INT64_MAX)
        return INT64_MAX;
    if (result < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
#endif
}

/// @brief Position delta for a velocity over @p dt: (velocity*dt)/DT_BASE (>>4),
///        symmetric for negatives, saturating to the int64 range.
/// @param velocity Centipixels per DT_BASE milliseconds.
/// @param dt Elapsed milliseconds; callers normally supply a positive value.
/// @return The signed centipixel displacement, truncated toward zero after
///         division by 16 and saturated to the `int64_t` range.
static int64_t entity_scaled_delta(int64_t velocity, int64_t dt) {
#if defined(__SIZEOF_INT128__)
    __int128 result = (__int128)velocity * (__int128)dt;
    if (result < 0)
        result = -((-result) >> 4);
    else
        result >>= 4;
    if (result > INT64_MAX)
        return INT64_MAX;
    if (result < INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
#else
    return entity_saturating_mul(velocity, dt) / DT_BASE;
#endif
}

/// @brief Convert centipixels (1/100 px fixed-point) to whole pixels (floor).
/// @param cp Fixed-point centipixel coordinate.
/// @return Pixel coordinate rounded toward negative infinity.
static int64_t entity_cp_to_px(int64_t cp) {
    return entity_floor_div(cp, 100);
}

/// @brief True if half-open intervals [a0,a1) and [b0,b1) overlap.
/// @param a0 Inclusive start of the first interval.
/// @param a1 Exclusive end of the first interval.
/// @param b0 Inclusive start of the second interval.
/// @param b1 Exclusive end of the second interval.
/// @return `1` for positive-length intersection; otherwise `0`.
static int8_t entity_range_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
    return a0 < b1 && a1 > b0;
}

//=============================================================================
// Constructor
//=============================================================================

/// @brief Create a new 2D game entity with position, bounding box, and physics state.
/// @param x Initial X coordinate in centipixels.
/// @param y Initial Y coordinate in centipixels.
/// @param w Collision width in pixels; nonpositive values become one.
/// @param h Collision height in pixels; nonpositive values become one.
/// @return A new Entity reference, or `NULL` if allocation fails.
/// @details Entities are lightweight game objects with position (in centipixels),
///          velocity, direction, HP, collision flags, and active state. They support
///          tilemap-based physics via move_and_collide (axis-separated collision
///          resolution) and helper methods for patrol AI. Velocity, HP, maximum
///          HP, type, and collision flags begin at zero; direction begins right
///          and active begins true.
void *rt_entity_new(int64_t x, int64_t y, int64_t w, int64_t h) {
    entity_impl *e =
        (entity_impl *)rt_obj_new_i64(RT_ENTITY_CLASS_ID, (int64_t)sizeof(entity_impl));
    if (!e)
        return NULL;
    memset(e, 0, sizeof(entity_impl));
    e->x = x;
    e->y = y;
    e->width = w > 0 ? w : 1;
    e->height = h > 0 ? h : 1;
    e->dir = 1;
    e->active = 1;
    return e;
}

//=============================================================================
// Property accessors
//
// All getters/setters below validate and then operate on entity_impl.
// Position (x, y) and velocity (vx, vy) are stored in *centipixels* (1/100 px)
// for sub-pixel precision in the integer integrator. Width/height are in pixels.
// `dir` is the facing direction (-1 = left, +1 = right). Each accessor returns
// Null handles use the documented fallback/no-op. A non-null mismatched
// runtime object raises a trap before using the same fallback/no-op.
//=============================================================================

/// @brief Read X position in centipixels (divide by 100 to get pixels).
/// @param ent Entity to query.
/// @return Stored X coordinate, or zero for a null or invalid handle.
int64_t rt_entity_get_x(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.X: expected Zanna.Game.Entity");
    return e ? e->x : 0;
}

/// @brief Read Y position in centipixels.
/// @param ent Entity to query.
/// @return Stored Y coordinate, or zero for a null or invalid handle.
int64_t rt_entity_get_y(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Y: expected Zanna.Game.Entity");
    return e ? e->y : 0;
}

/// @brief Set X position in centipixels (teleport — bypasses collision).
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New X coordinate.
/// @details Collision flags and velocity are unchanged.
void rt_entity_set_x(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.X.set: expected Zanna.Game.Entity");
    if (e)
        e->x = v;
}

/// @brief Set Y position in centipixels.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New Y coordinate. Collision is not evaluated.
void rt_entity_set_y(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.Y.set: expected Zanna.Game.Entity");
    if (e)
        e->y = v;
}

/// @brief Read X velocity in centipixels per 16-millisecond base frame.
/// @param ent Entity to query.
/// @return Stored X velocity, or zero for a null or invalid handle.
int64_t rt_entity_get_vx(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.VelocityX: expected Zanna.Game.Entity");
    return e ? e->vx : 0;
}

/// @brief Read Y velocity in centipixels per 16-millisecond base frame.
/// @param ent Entity to query.
/// @return Stored Y velocity, or zero for a null or invalid handle.
int64_t rt_entity_get_vy(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.VelocityY: expected Zanna.Game.Entity");
    return e ? e->vy : 0;
}

/// @brief Set X velocity in centipixels per 16-millisecond base frame.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New horizontal velocity.
void rt_entity_set_vx(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.VelocityX.set: expected Zanna.Game.Entity");
    if (e)
        e->vx = v;
}

/// @brief Set Y velocity in centipixels per 16-millisecond base frame.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New vertical velocity; positive values move downward.
void rt_entity_set_vy(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.VelocityY.set: expected Zanna.Game.Entity");
    if (e)
        e->vy = v;
}

/// @brief Read entity bounding-box width in pixels (set on construction).
/// @param ent Entity to query.
/// @return Positive collision width, or zero for a null or invalid handle.
int64_t rt_entity_get_width(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Width: expected Zanna.Game.Entity");
    return e ? e->width : 0;
}

/// @brief Read entity bounding-box height in pixels.
/// @param ent Entity to query.
/// @return Positive collision height, or zero for a null or invalid handle.
int64_t rt_entity_get_height(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Height: expected Zanna.Game.Entity");
    return e ? e->height : 0;
}

/// @brief Read facing direction (-1 = left, +1 = right). Defaults to +1 for NULL.
/// @param ent Entity to query.
/// @return Stored facing direction, or `1` for a null or invalid handle.
int64_t rt_entity_get_dir(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Dir: expected Zanna.Game.Entity");
    return e ? e->dir : 1;
}

/// @brief Set facing direction. Used by sprite-mirroring and patrol/turn logic.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v Candidate direction; negative values select `-1` and zero or
///        positive values select `1`.
void rt_entity_set_dir(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.Dir.set: expected Zanna.Game.Entity");
    if (e)
        e->dir = v < 0 ? -1 : 1;
}

/// @brief Read current hit-point count.
/// @param ent Entity to query.
/// @return Stored HP, or zero for a null or invalid handle.
int64_t rt_entity_get_hp(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Health: expected Zanna.Game.Entity");
    return e ? e->hp : 0;
}

/// @brief Set current hit-point count (no clamping — caller responsible for capping at max_hp).
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New HP value, stored verbatim.
void rt_entity_set_hp(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.Health.set: expected Zanna.Game.Entity");
    if (e)
        e->hp = v;
}

/// @brief Read maximum hit-point cap.
/// @param ent Entity to query.
/// @return Stored maximum HP, or zero for a null or invalid handle.
int64_t rt_entity_get_max_hp(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.MaxHealth: expected Zanna.Game.Entity");
    return e ? e->max_hp : 0;
}

/// @brief Set the max hit-point cap.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v New maximum HP, stored verbatim without changing current HP.
void rt_entity_set_max_hp(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.MaxHealth.set: expected Zanna.Game.Entity");
    if (e)
        e->max_hp = v;
}

/// @brief Read user-defined entity type tag (e.g., 0=player, 1=enemy, 2=pickup). Game-specific.
/// @param ent Entity to query.
/// @return Stored type tag, or zero for a null or invalid handle.
int64_t rt_entity_get_type(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Type: expected Zanna.Game.Entity");
    return e ? e->type : 0;
}

/// @brief Set the entity type tag.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v Game-defined tag stored verbatim.
void rt_entity_set_type(void *ent, int64_t v) {
    entity_impl *e = checked_entity(ent, "Entity.Type.set: expected Zanna.Game.Entity");
    if (e)
        e->type = v;
}

/// @brief Returns 1 if the entity is currently active (participates in update/draw).
/// @param ent Entity to query.
/// @return Normalized active flag, or zero for a null or invalid handle.
int8_t rt_entity_get_active(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.Active: expected Zanna.Game.Entity");
    return e ? e->active : 0;
}

/// @brief Toggle active flag. Inactive entities should be skipped by game-loop systems.
/// @param ent Entity to mutate; `NULL` is a no-op.
/// @param v Candidate flag; zero becomes `0` and any nonzero value becomes `1`.
void rt_entity_set_active(void *ent, int8_t v) {
    entity_impl *e = checked_entity(ent, "Entity.Active.set: expected Zanna.Game.Entity");
    if (e)
        e->active = v ? 1 : 0;
}

/// @brief Last-collision flag: 1 if the entity is touching a solid tile below.
/// @param ent Entity to query.
/// @return Ground-contact flag from the last positive-delta collision move, or
///         zero for a null or invalid handle.
int8_t rt_entity_on_ground(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.OnGround: expected Zanna.Game.Entity");
    return e ? e->on_ground : 0;
}

/// @brief Last-collision flag: 1 if the entity bumped a solid tile on its left side.
/// @param ent Entity to query.
/// @return Left-hit flag from the last positive-delta collision move, or zero
///         for a null or invalid handle.
int8_t rt_entity_hit_left(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.HitLeft: expected Zanna.Game.Entity");
    return e ? e->hit_left : 0;
}

/// @brief Last-collision flag: 1 if the entity bumped a solid tile on its right side.
/// @param ent Entity to query.
/// @return Right-hit flag from the last positive-delta collision move, or zero
///         for a null or invalid handle.
int8_t rt_entity_hit_right(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.HitRight: expected Zanna.Game.Entity");
    return e ? e->hit_right : 0;
}

/// @brief Last-collision flag: 1 if the entity bumped a solid tile above (head bonk).
/// @param ent Entity to query.
/// @return Ceiling-hit flag from the last positive-delta collision move, or
///         zero for a null or invalid handle.
int8_t rt_entity_hit_ceiling(void *ent) {
    entity_impl *e = checked_entity(ent, "Entity.HitCeiling: expected Zanna.Game.Entity");
    return e ? e->hit_ceiling : 0;
}

//=============================================================================
// Physics: Gravity
//=============================================================================

/// @brief Test whether any solid tile intersects a vertical leading edge.
/// @param tilemap Tilemap queried through rt_tilemap_is_solid_at().
/// @param edge_px X coordinate of the edge in pixels.
/// @param top_px Inclusive top pixel covered by the entity.
/// @param bottom_px Inclusive bottom pixel covered by the entity.
/// @param tile_h Positive tile height in pixels.
/// @return `1` on the first solid tile sampled from the top through bottom
///         tile rows; otherwise `0`.
static int8_t entity_vertical_edge_hits(
    void *tilemap, int64_t edge_px, int64_t top_px, int64_t bottom_px, int64_t tile_h) {
    int64_t top_row = entity_floor_div(top_px, tile_h);
    int64_t bottom_row = entity_floor_div(bottom_px, tile_h);
    for (int64_t row = top_row; row <= bottom_row; ++row) {
        if (rt_tilemap_is_solid_at(tilemap, edge_px, entity_saturating_mul(row, tile_h)))
            return 1;
    }
    return 0;
}

/// @brief True if any solid tile spans the horizontal edge at @p edge_px
///        between columns covering [left_px, right_px].
/// @param tilemap Tilemap queried through rt_tilemap_is_solid_at().
/// @param left_px Inclusive left pixel covered by the entity.
/// @param right_px Inclusive right pixel covered by the entity.
/// @param edge_px Y coordinate of the edge in pixels.
/// @param tile_w Positive tile width in pixels.
/// @return `1` on the first solid tile sampled from the left through right
///         tile columns; otherwise `0`.
static int8_t entity_horizontal_edge_hits(
    void *tilemap, int64_t left_px, int64_t right_px, int64_t edge_px, int64_t tile_w) {
    int64_t left_col = entity_floor_div(left_px, tile_w);
    int64_t right_col = entity_floor_div(right_px, tile_w);
    for (int64_t col = left_col; col <= right_col; ++col) {
        if (rt_tilemap_is_solid_at(tilemap, entity_saturating_mul(col, tile_w), edge_px))
            return 1;
    }
    return 0;
}

/// @brief Move the entity horizontally by @p delta_cp centipixels against the
///        tilemap, stopping flush at the first solid tile and zeroing x-velocity
///        on contact (swept AABB collision).
/// @param e Valid Entity payload to mutate.
/// @param tilemap Valid TileMap used for solidity queries.
/// @param delta_cp Signed horizontal displacement in centipixels.
/// @param tw Positive tile width in pixels.
/// @param th Positive tile height in pixels.
/// @details A zero displacement is a no-op. The sweep checks each newly crossed
///          leading-edge tile column and the entity's full inclusive vertical
///          pixel span. Contact sets exactly the corresponding left/right flag;
///          an unobstructed sweep commits the saturated target coordinate.
static void entity_sweep_x(
    entity_impl *e, void *tilemap, int64_t delta_cp, int64_t tw, int64_t th) {
    if (delta_cp == 0)
        return;

    int64_t old_x = e->x;
    int64_t target_x = entity_saturating_add(e->x, delta_cp);
    int64_t y_px = entity_cp_to_px(e->y);
    int64_t top_px = y_px;
    int64_t bottom_px = entity_saturating_add(y_px, e->height - 1);

    int64_t old_left = entity_cp_to_px(old_x);
    int64_t new_left = entity_cp_to_px(target_x);
    if (delta_cp > 0) {
        int64_t old_right = entity_saturating_add(old_left, e->width - 1);
        int64_t new_right = entity_saturating_add(new_left, e->width - 1);
        int64_t first_col = entity_saturating_add(entity_floor_div(old_right, tw), 1);
        int64_t last_col = entity_floor_div(new_right, tw);
        for (int64_t col = first_col; col <= last_col; ++col) {
            int64_t edge_px = entity_saturating_mul(col, tw);
            if (entity_vertical_edge_hits(tilemap, edge_px, top_px, bottom_px, th)) {
                e->x = entity_saturating_mul(entity_saturating_add(edge_px, -e->width), 100);
                e->vx = 0;
                e->hit_right = 1;
                return;
            }
        }
    } else {
        int64_t old_col = entity_floor_div(old_left, tw);
        int64_t new_col = entity_floor_div(new_left, tw);
        for (int64_t col = entity_saturating_add(old_col, -1); col >= new_col; --col) {
            int64_t next_col = entity_saturating_add(col, 1);
            int64_t edge_px = entity_saturating_add(entity_saturating_mul(next_col, tw), -1);
            if (entity_vertical_edge_hits(tilemap, edge_px, top_px, bottom_px, th)) {
                e->x = entity_saturating_mul(entity_saturating_mul(next_col, tw), 100);
                e->vx = 0;
                e->hit_left = 1;
                return;
            }
            if (col == INT64_MIN)
                break;
        }
    }

    e->x = target_x;
}

/// @brief Move the entity vertically by @p delta_cp centipixels against the
///        tilemap, stopping flush at the first solid tile; sets the grounded
///        flag and zeroes y-velocity on a downward landing (swept AABB).
/// @param e Valid Entity payload to mutate.
/// @param tilemap Valid TileMap used for solidity queries.
/// @param delta_cp Signed vertical displacement in centipixels.
/// @param tw Positive tile width in pixels.
/// @param th Positive tile height in pixels.
/// @details A zero displacement is a no-op. The sweep checks each newly crossed
///          leading-edge tile row and the entity's full inclusive horizontal
///          pixel span. Downward contact sets on-ground; upward contact sets
///          hit-ceiling. Either contact zeros vertical velocity.
static void entity_sweep_y(
    entity_impl *e, void *tilemap, int64_t delta_cp, int64_t tw, int64_t th) {
    if (delta_cp == 0)
        return;

    int64_t old_y = e->y;
    int64_t target_y = entity_saturating_add(e->y, delta_cp);
    int64_t x_px = entity_cp_to_px(e->x);
    int64_t left_px = x_px;
    int64_t right_px = entity_saturating_add(x_px, e->width - 1);

    int64_t old_top = entity_cp_to_px(old_y);
    int64_t new_top = entity_cp_to_px(target_y);
    if (delta_cp > 0) {
        int64_t old_bottom = entity_saturating_add(old_top, e->height - 1);
        int64_t new_bottom = entity_saturating_add(new_top, e->height - 1);
        int64_t first_row = entity_saturating_add(entity_floor_div(old_bottom, th), 1);
        int64_t last_row = entity_floor_div(new_bottom, th);
        for (int64_t row = first_row; row <= last_row; ++row) {
            int64_t edge_px = entity_saturating_mul(row, th);
            if (entity_horizontal_edge_hits(tilemap, left_px, right_px, edge_px, tw)) {
                e->y = entity_saturating_mul(entity_saturating_add(edge_px, -e->height), 100);
                e->vy = 0;
                e->on_ground = 1;
                return;
            }
        }
    } else {
        int64_t old_row = entity_floor_div(old_top, th);
        int64_t new_row = entity_floor_div(new_top, th);
        for (int64_t row = entity_saturating_add(old_row, -1); row >= new_row; --row) {
            int64_t next_row = entity_saturating_add(row, 1);
            int64_t edge_px = entity_saturating_add(entity_saturating_mul(next_row, th), -1);
            if (entity_horizontal_edge_hits(tilemap, left_px, right_px, edge_px, tw)) {
                e->y = entity_saturating_mul(entity_saturating_mul(next_row, th), 100);
                e->vy = 0;
                e->hit_ceiling = 1;
                return;
            }
            if (row == INT64_MIN)
                break;
        }
    }

    e->y = target_y;
}

/// @brief Apply downward gravitational acceleration, clamped to max_fall terminal velocity.
/// @param ent Entity whose vertical velocity is updated.
/// @param gravity Acceleration expressed in centipixels per base-frame squared.
/// @param max_fall Maximum downward velocity; negative values become zero.
/// @param dt Positive elapsed milliseconds; nonpositive values are ignored.
/// @details Adds the time-scaled gravity with saturation, then clamps only
///          values above @p max_fall. Negative upward velocity is not
///          symmetrically limited. A non-null invalid Entity handle raises a
///          runtime trap.
void rt_entity_apply_gravity(void *ent, int64_t gravity, int64_t max_fall, int64_t dt) {
    entity_impl *e = checked_entity(ent, "Entity.ApplyGravity: expected Zanna.Game.Entity");
    if (!e || dt <= 0)
        return;
    if (max_fall < 0)
        max_fall = 0;
    e->vy = entity_saturating_add(e->vy, entity_scaled_delta(gravity, dt));
    if (e->vy > max_fall)
        e->vy = max_fall;
}

//=============================================================================
// Physics: MoveAndCollide (tilemap)
//=============================================================================

/// @brief Move the entity by its velocity and resolve tilemap collisions per axis.
/// @param ent Entity to move.
/// @param tilemap TileMap used for collision, or `NULL` for unimpeded movement.
/// @param dt Positive elapsed milliseconds; nonpositive values leave position
///        and collision flags unchanged.
/// @details Moves X first, then Y. For each axis, checks the leading edge tiles
///          for solidity and pushes the entity out if overlapping. Sets collision
///          flags (on_ground, hit_left, hit_right, hit_ceiling) and zeroes the
///          velocity component on collision. A positive call resets all flags
///          before checking @p tilemap. A null map performs saturated free
///          movement. A map reporting nonpositive tile dimensions leaves the
///          reset flags and position unchanged. After swept motion, a one-row
///          ground probe preserves contact for a resting entity. Invalid
///          non-null runtime handles may raise their respective class traps.
void rt_entity_move_and_collide(void *ent, void *tilemap, int64_t dt) {
    entity_impl *e = checked_entity(ent, "Entity.MoveAndCollide: expected Zanna.Game.Entity");
    if (!e || dt <= 0)
        return;

    // Reset collision flags
    e->on_ground = 0;
    e->hit_left = 0;
    e->hit_right = 0;
    e->hit_ceiling = 0;

    if (!tilemap) {
        // No tilemap: just move
        e->x = entity_saturating_add(e->x, entity_scaled_delta(e->vx, dt));
        e->y = entity_saturating_add(e->y, entity_scaled_delta(e->vy, dt));
        return;
    }

    int64_t tw = rt_tilemap_get_tile_width(tilemap);
    int64_t th = rt_tilemap_get_tile_height(tilemap);
    if (tw <= 0 || th <= 0)
        return;

    int64_t dispX = entity_scaled_delta(e->vx, dt);
    int64_t dispY = entity_scaled_delta(e->vy, dt);

    entity_sweep_x(e, tilemap, dispX, tw, th);
    entity_sweep_y(e, tilemap, dispY, tw, th);

    // Persistent ground contact: entity_sweep_y only sets on_ground on a nonzero
    // downward landing, so a resting entity (zero vertical displacement) would lose
    // the flag every frame. Probe the tile row immediately beneath the bottom edge
    // so grounded state persists while flush against a solid tile (VDOC-241).
    if (!e->on_ground) {
        int64_t x_px = entity_cp_to_px(e->x);
        int64_t left_px = x_px;
        int64_t right_px = entity_saturating_add(x_px, e->width - 1);
        int64_t below_px = entity_saturating_add(entity_cp_to_px(e->y), e->height);
        if (entity_horizontal_edge_hits(tilemap, left_px, right_px, below_px, tw))
            e->on_ground = 1;
    }
}

//=============================================================================
// Physics: Combined gravity + move + collide
//=============================================================================

/// @brief Apply gravity then move-and-collide in one call (convenience wrapper).
/// @param ent Entity to update.
/// @param tilemap TileMap used for collision, or `NULL` for free movement.
/// @param gravity Vertical acceleration passed to rt_entity_apply_gravity().
/// @param max_fall Downward velocity cap passed to rt_entity_apply_gravity().
/// @param dt Elapsed milliseconds passed unchanged to both operations.
/// @details The two public operations retain their individual no-op, reset, and
///          trap behavior.
void rt_entity_update_physics(
    void *ent, void *tilemap, int64_t gravity, int64_t max_fall, int64_t dt) {
    rt_entity_apply_gravity(ent, gravity, max_fall, dt);
    rt_entity_move_and_collide(ent, tilemap, dt);
}

//=============================================================================
// AI helpers
//=============================================================================

/// @brief Check whether the entity is at a platform edge (no solid tile below in facing direction).
/// @param ent Entity whose leading foot position is sampled.
/// @param tilemap TileMap to query.
/// @return `1` when the point two pixels below and one pixel beyond the facing
///         side is not solid; otherwise `0`.
/// @details Null arguments return zero. Positive direction checks just beyond
///          the right edge; zero or negative direction checks one pixel left.
///          Invalid non-null runtime handles may raise class traps.
int8_t rt_entity_at_edge(void *ent, void *tilemap) {
    entity_impl *e = checked_entity(ent, "Entity.AtEdge: expected Zanna.Game.Entity");
    if (!e || !tilemap)
        return 0;
    int64_t px = entity_cp_to_px(e->x);
    int64_t py = entity_cp_to_px(e->y);
    int64_t checkX =
        (e->dir > 0) ? entity_saturating_add(px, e->width) : entity_saturating_add(px, -1);
    int64_t checkY = entity_saturating_add(entity_saturating_add(py, e->height), 2);
    return !rt_tilemap_is_solid_at(tilemap, checkX, checkY);
}

/// @brief Reverse direction when hitting a wall (check hit_left/hit_right flags).
/// @param ent Entity whose last collision flags are examined.
/// @param speed Velocity assigned after a left hit; its saturating negation is
///        assigned after a right hit.
/// @details A left hit selects right-facing direction. A right hit selects
///          left-facing direction and wins if both flags are set. No collision
///          leaves velocity and direction unchanged. A non-null invalid Entity
///          handle raises a runtime trap.
void rt_entity_patrol_reverse(void *ent, int64_t speed) {
    entity_impl *e = checked_entity(ent, "Entity.PatrolReverse: expected Zanna.Game.Entity");
    if (!e)
        return;
    if (e->hit_left) {
        e->vx = speed;
        e->dir = 1;
    }
    if (e->hit_right) {
        e->vx = entity_saturating_neg(speed);
        e->dir = -1;
    }
}

/// @brief Test whether two entities' bounding boxes overlap (AABB collision test).
/// @param ent First Entity.
/// @param other Second Entity.
/// @return `1` when the floor-converted pixel rectangles have positive-area
///         overlap on both axes; otherwise `0`.
/// @details Edge-only contact does not count. Rectangle ends are computed with
///          saturating addition. Null handles return zero; invalid non-null
///          Entity handles raise runtime traps.
int8_t rt_entity_overlaps(void *ent, void *other) {
    entity_impl *a = checked_entity(ent, "Entity.Overlaps: expected Zanna.Game.Entity");
    entity_impl *b = checked_entity(other, "Entity.Overlaps: expected Zanna.Game.Entity");
    if (!a || !b)
        return 0;
    int64_t ax = entity_cp_to_px(a->x);
    int64_t ay = entity_cp_to_px(a->y);
    int64_t bx = entity_cp_to_px(b->x);
    int64_t by = entity_cp_to_px(b->y);
    int64_t ar = entity_saturating_add(ax, a->width);
    int64_t ab = entity_saturating_add(ay, a->height);
    int64_t br = entity_saturating_add(bx, b->width);
    int64_t bb = entity_saturating_add(by, b->height);
    return entity_range_overlaps(ax, ar, bx, br) && entity_range_overlaps(ay, ab, by, bb);
}
