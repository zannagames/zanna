//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_targetlock.c
// Purpose: Zanna.Game3D.TargetLock3D — lock-on target acquisition, cycling, and
//   maintenance for third-person combat. Scores overlap-sphere candidates by
//   camera-forward angle (2:1) and distance, gates on line of sight, auto-releases
//   on death/distance/LoS-grace, and exposes one-shot acquired/lost poll flags
//   plus a soft input-magnetism helper.
// Key invariants:
//   - Only entities registered through Entity3D.attachBody resolve as candidates.
//   - Entity liveness includes an attached Health3D death latch; dead combat
//     targets are neither acquired nor retained.
//   - LoS is judged origin-to-origin via the allocation-free raw all-body ray,
//     skipping the owner's own body; the first foreign hit must be the candidate.
//   - One-shot flags follow the just_landed pattern: set on transition, cleared
//     at the start of the next Update.
// Ownership/Lifetime:
//   - GC-managed handle; finalizer releases retained world/owner/target refs
//     and reusable candidate-query scratch storage.
// Links: rt_game3d_internal.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements target acquisition, cycling, maintenance, and movement bias for TargetLock3D.
/// @details Candidates are gathered from physics overlaps, resolved through the
/// world's body index, filtered by camera cone and optional line of sight, and
/// scored with angle, distance, and current-target stickiness. Maintenance
/// applies death, distance, and timed occlusion release rules.

#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=========================================================================
// Internal helpers
//=========================================================================

/// @brief Release a retained private slot only when its complete payload remains valid.
/// @details Invalid same-class or wrong-class values are unowned corruption sentinels.
/// @param[in,out] slot Address of the retained slot.
/// @param class_id Required runtime class identifier.
/// @param payload_size Minimum complete payload size.
static void game3d_targetlock_release_instance_ref(void **slot,
                                                   int64_t class_id,
                                                   size_t payload_size) {
    if (!slot || !*slot)
        return;
    if (!rt_obj_is_instance(*slot, class_id, payload_size)) {
        *slot = NULL;
        return;
    }
    game3d_release_ref(slot);
}

/// @brief Transactionally replace a complete retained private object slot.
/// @param[in,out] slot Address of the retained slot.
/// @param value Borrowed replacement, or NULL.
/// @param class_id Required runtime class identifier.
/// @param payload_size Minimum complete payload size.
static void game3d_targetlock_assign_instance_ref(void **slot,
                                                  void *value,
                                                  int64_t class_id,
                                                  size_t payload_size) {
    if (!slot || *slot == value || (value && !rt_obj_is_instance(value, class_id, payload_size)))
        return;
    rt_obj_retain_maybe(value);
    game3d_targetlock_release_instance_ref(slot, class_id, payload_size);
    *slot = value;
}

/// @brief Repair private target-lock state before public field access.
/// @param lock Complete TargetLock3D payload; NULL is ignored.
void game3d_targetlock_repair_state(rt_game3d_targetlock *lock) {
    if (!lock)
        return;
    if (lock->world &&
        !rt_obj_is_instance(lock->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world)))
        lock->world = NULL;
    if (lock->owner &&
        !rt_obj_is_instance(lock->owner, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity)))
        lock->owner = NULL;
    if (lock->target &&
        !rt_obj_is_instance(lock->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity)))
        lock->target = NULL;
    lock->max_distance = game3d_positive_clamped_or(
        lock->max_distance, RT_GAME3D_TL_DEFAULT_MAX_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
    lock->cone_degrees = game3d_clamp(
        game3d_finite_or(lock->cone_degrees, RT_GAME3D_TL_DEFAULT_CONE_DEGREES), 1.0, 180.0);
    lock->require_los = lock->require_los ? 1 : 0;
    lock->stickiness =
        game3d_positive_clamped_or(lock->stickiness, RT_GAME3D_TL_DEFAULT_STICKINESS, 1000.0);
    lock->break_distance = game3d_positive_clamped_or(
        lock->break_distance, RT_GAME3D_TL_DEFAULT_MAX_DISTANCE * 1.25, RT_GAME3D_COORD_ABS_MAX);
    lock->los_grace_seconds = game3d_nonnegative_clamped_or(
        lock->los_grace_seconds, RT_GAME3D_TL_DEFAULT_LOS_GRACE, 3600.0);
    lock->los_broken_time = game3d_nonnegative_clamped_or(lock->los_broken_time, 0.0, 3600.0);
    lock->just_acquired = lock->just_acquired ? 1 : 0;
    lock->just_lost = lock->just_lost ? 1 : 0;
    if (!lock->target)
        lock->los_broken_time = 0.0;
}

/// @brief Return the lock's owner Entity3D when still alive, else NULL.
/// @param lock Borrowed target-lock payload.
/// @return Borrowed live owner entity, or `NULL` when absent, stale, or invalid.
static rt_game3d_entity *game3d_targetlock_owner_ref(rt_game3d_targetlock *lock) {
    rt_game3d_entity *entity = lock && rt_obj_is_instance(lock->owner,
                                                          RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                          sizeof(rt_game3d_entity))
                                   ? (rt_game3d_entity *)lock->owner
                                   : NULL;
    if (!entity)
        return NULL;
    if (game3d_entity_alive_or_record(entity))
        return entity;
    game3d_targetlock_release_instance_ref(
        &lock->owner, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    return NULL;
}

/// @brief Test whether an entity is live and not latched dead by Health3D.
/// @param entity Borrowed Entity3D candidate.
/// @return Nonzero when the entity may participate in target locking.
static int game3d_targetlock_entity_targetable(rt_game3d_entity *entity) {
    if (!game3d_entity_alive_or_record(entity))
        return 0;
    rt_game3d_health *health =
        rt_obj_is_instance(entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID, sizeof(rt_game3d_health))
            ? (rt_game3d_health *)entity->health
            : NULL;
    return !health || !health->dead;
}

/// @brief Return the locked Entity3D when still alive, else NULL.
/// @param lock Borrowed target-lock payload.
/// @return Borrowed live target entity, or `NULL` when unlocked, stale, or invalid.
static rt_game3d_entity *game3d_targetlock_target_ref(rt_game3d_targetlock *lock) {
    rt_game3d_entity *entity = lock && rt_obj_is_instance(lock->target,
                                                          RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                          sizeof(rt_game3d_entity))
                                   ? (rt_game3d_entity *)lock->target
                                   : NULL;
    if (!entity)
        return NULL;
    return game3d_targetlock_entity_targetable(entity) ? entity : NULL;
}

/// @brief Return the lock's world when still valid, else NULL.
/// @param lock Borrowed target-lock payload.
/// @return Borrowed World3D payload, or `NULL` when absent or type-mismatched.
static rt_game3d_world *game3d_targetlock_world_ref(rt_game3d_targetlock *lock) {
    rt_game3d_world *world = lock && rt_obj_is_instance(lock->world,
                                                        RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                        sizeof(rt_game3d_world))
                                 ? (rt_game3d_world *)lock->world
                                 : NULL;
    if (!world || !world->destroyed)
        return world;
    game3d_targetlock_release_instance_ref(
        &lock->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    return NULL;
}

/// @brief True when @p candidate has line of sight from @p owner (origin-to-origin).
/// @details Casts owner→candidate through all layers and walks the bounded raw
///   non-trigger body results: the owner's body is transparent; the first
///   foreign body must be the candidate.
/// @param world Borrowed world providing physics and body-to-entity lookup.
/// @param owner Borrowed live owner entity.
/// @param candidate Borrowed live candidate entity.
/// @return Nonzero when no foreign solid body blocks the origin-to-origin segment.
static int game3d_targetlock_has_los(rt_game3d_world *world,
                                     rt_game3d_entity *owner,
                                     rt_game3d_entity *candidate) {
    if (!world || !world->physics || !owner || !candidate)
        return 0;
    double from[3];
    double to[3];
    if (!game3d_entity_world_position_components(owner, from) ||
        !game3d_entity_world_position_components(candidate, to))
        return 0;
    double dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    double len = hypot(hypot(dir[0], dir[1]), dir[2]);
    if (!isfinite(len))
        return 0;
    if (len <= 1e-6)
        return 1; /* coincident: trivially visible */
    double direction[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
    void *bodies[2] = {NULL, NULL};
    int32_t hit_count =
        rt_world3d_raycast_all_bodies_raw(world->physics, from, direction, len, -1, bodies, 2);
    if (hit_count < 0)
        return 0;
    for (int32_t i = 0; i < hit_count; ++i) {
        void *body = bodies[i];
        rt_game3d_entity *entity = body ? game3d_world_find_entity_by_body(world, body) : NULL;
        if (entity == owner)
            continue;
        return entity == candidate;
    }
    return 1;
}

/// @brief Candidate record produced by the shared collection pass.
typedef struct {
    rt_game3d_entity *entity;
    double distance;
    double angle_deg;  /* full 3D angle from the camera forward */
    double camera_yaw; /* signed yaw offset in the camera basis, radians */
    double sort_key;
    int32_t input_order;
} game3d_targetlock_candidate;

/// @brief Ensure reusable candidate and identity-set scratch can hold one overlap result.
static int game3d_targetlock_reserve_candidates(rt_game3d_targetlock *lock, int32_t hit_count) {
    if (hit_count <= 0)
        return 1;
    if (lock->candidate_capacity < hit_count) {
        game3d_targetlock_candidate *grown = (game3d_targetlock_candidate *)realloc(
            lock->candidate_scratch, (size_t)hit_count * sizeof(*grown));
        if (!grown) {
            rt_trap("Game3D.TargetLock3D: candidate scratch allocation failed");
            return 0;
        }
        lock->candidate_scratch = grown;
        lock->candidate_capacity = hit_count;
    }
    int32_t seen_capacity = 8;
    while (seen_capacity < hit_count * 2)
        seen_capacity *= 2;
    if (lock->candidate_seen_capacity < seen_capacity) {
        void **grown =
            (void **)realloc(lock->candidate_seen, (size_t)seen_capacity * sizeof(*grown));
        if (!grown) {
            rt_trap("Game3D.TargetLock3D: candidate identity-set allocation failed");
            return 0;
        }
        lock->candidate_seen = grown;
        lock->candidate_seen_capacity = seen_capacity;
    }
    memset(lock->candidate_seen, 0, (size_t)lock->candidate_seen_capacity * sizeof(void *));
    return 1;
}

/// @brief Insert an entity identity into the per-query open-addressed set.
/// @return Nonzero only for the first occurrence of @p entity.
static int game3d_targetlock_mark_first(rt_game3d_targetlock *lock, rt_game3d_entity *entity) {
    size_t mask = (size_t)lock->candidate_seen_capacity - 1u;
    size_t slot = (((uintptr_t)entity >> 4u) * UINT64_C(11400714819323198485)) & mask;
    while (lock->candidate_seen[slot]) {
        if (lock->candidate_seen[slot] == (void *)entity)
            return 0;
        slot = (slot + 1u) & mask;
    }
    lock->candidate_seen[slot] = entity;
    return 1;
}

/// @brief Collect scored lock-on candidates around the owner.
/// @details Overlap-sphere at the owner, resolve bodies to entities, reject the
///   owner/dead/mask-mismatched entities, deduplicate through an identity hash set,
///   and record distance plus camera-relative angles for scoring and cycling.
/// @param lock Borrowed target-lock payload defining the query and filters.
/// @param[out] out Receives reusable lock-owned candidate storage, invalidated by the next query.
/// @return Number of candidates written to @p out.
static int32_t game3d_targetlock_collect(rt_game3d_targetlock *lock,
                                         game3d_targetlock_candidate **out) {
    *out = NULL;
    rt_game3d_world *world = game3d_targetlock_world_ref(lock);
    rt_game3d_entity *owner = game3d_targetlock_owner_ref(lock);
    if (!world || !world->physics || !owner)
        return 0;
    double owner_pos[3];
    if (!game3d_entity_world_position_components(owner, owner_pos))
        return 0;

    /* Camera basis for angle scoring; fall back to -Z forward when unset. */
    double fwd[3] = {0.0, 0.0, -1.0};
    double right[3] = {1.0, 0.0, 0.0};
    if (world->camera) {
        void *f = rt_camera3d_get_forward(world->camera);
        void *r = rt_camera3d_get_right(world->camera);
        if (f) {
            fwd[0] = rt_vec3_x(f);
            fwd[1] = rt_vec3_y(f);
            fwd[2] = rt_vec3_z(f);
        }
        if (r) {
            right[0] = rt_vec3_x(r);
            right[1] = rt_vec3_y(r);
            right[2] = rt_vec3_z(r);
        }
        game3d_release_ref(&r);
        game3d_release_ref(&f);
    }
    double fwd_len = sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!isfinite(fwd_len) || fwd_len <= 1e-9) {
        fwd[0] = 0.0;
        fwd[1] = 0.0;
        fwd[2] = -1.0;
        fwd_len = 1.0;
    }

    void *center = rt_vec3_new(owner_pos[0], owner_pos[1], owner_pos[2]);
    if (!center)
        return 0;
    void *hits =
        rt_world3d_overlap_sphere(world->physics, center, lock->max_distance, lock->candidate_mask);
    int32_t count = 0;
    int64_t hit_count = hits ? rt_physics_hit_list3d_get_count(hits) : 0;
    if (hit_count > INT32_MAX || !game3d_targetlock_reserve_candidates(lock, (int32_t)hit_count)) {
        game3d_release_ref(&hits);
        game3d_release_ref(&center);
        return 0;
    }
    game3d_targetlock_candidate *records = (game3d_targetlock_candidate *)lock->candidate_scratch;
    for (int64_t i = 0; i < hit_count; ++i) {
        void *hit = rt_physics_hit_list3d_get(hits, i);
        void *body = hit ? rt_physics_hit3d_get_body(hit) : NULL;
        rt_game3d_entity *entity = body ? game3d_world_find_entity_by_body(world, body) : NULL;
        if (!entity || entity == owner || !game3d_targetlock_entity_targetable(entity))
            continue;
        if (!game3d_targetlock_mark_first(lock, entity))
            continue;
        double pos[3];
        if (!game3d_entity_world_position_components(entity, pos))
            continue;
        double dir[3] = {pos[0] - owner_pos[0], pos[1] - owner_pos[1], pos[2] - owner_pos[2]};
        double dist = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!isfinite(dist) || dist <= 1e-6 || dist > lock->max_distance)
            continue;
        double inv = 1.0 / dist;
        double cos_angle = (dir[0] * fwd[0] + dir[1] * fwd[1] + dir[2] * fwd[2]) * inv / fwd_len;
        double angle_deg = acos(game3d_clamp(cos_angle, -1.0, 1.0)) * (180.0 / RT_GAME3D_PI);
        if (angle_deg > lock->cone_degrees)
            continue;
        records[count].entity = entity;
        records[count].distance = dist;
        records[count].angle_deg = angle_deg;
        records[count].camera_yaw = atan2(dir[0] * right[0] + dir[1] * right[1] + dir[2] * right[2],
                                          dir[0] * fwd[0] + dir[1] * fwd[1] + dir[2] * fwd[2]);
        records[count].sort_key = 0.0;
        records[count].input_order = count;
        ++count;
    }
    game3d_release_ref(&hits);
    game3d_release_ref(&center);
    *out = records;
    return count;
}

/// @brief Ascending candidate sort with deterministic input-order ties.
static int game3d_targetlock_candidate_compare(const void *lhs, const void *rhs) {
    const game3d_targetlock_candidate *a = (const game3d_targetlock_candidate *)lhs;
    const game3d_targetlock_candidate *b = (const game3d_targetlock_candidate *)rhs;
    if (a->sort_key < b->sort_key)
        return -1;
    if (a->sort_key > b->sort_key)
        return 1;
    return (a->input_order > b->input_order) - (a->input_order < b->input_order);
}

/// @brief Angle-weighted (2:1) acquisition score for a candidate.
/// @param lock Borrowed target-lock payload defining normalization and stickiness.
/// @param candidate Borrowed candidate record to score.
/// @return Higher-is-better normalized angle/distance score with optional stickiness multiplier.
static double game3d_targetlock_score(const rt_game3d_targetlock *lock,
                                      const game3d_targetlock_candidate *candidate) {
    double cone = lock->cone_degrees > 1e-9 ? lock->cone_degrees : 1.0;
    double max_dist = lock->max_distance > 1e-9 ? lock->max_distance : 1.0;
    double score =
        2.0 * (1.0 - candidate->angle_deg / cone) + 1.0 * (1.0 - candidate->distance / max_dist);
    if (lock->target && (void *)candidate->entity == lock->target)
        score *= lock->stickiness;
    return score;
}

/// @brief Install @p entity as the locked target, firing the one-shot flag.
/// @param lock Target-lock payload whose retained target is replaced.
/// @param entity Borrowed entity to retain, or `NULL` to clear without a lost event.
static void game3d_targetlock_install(rt_game3d_targetlock *lock, rt_game3d_entity *entity) {
    if (lock->target == (void *)entity)
        return;
    game3d_targetlock_assign_instance_ref(
        &lock->target, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    lock->los_broken_time = 0.0;
    if (entity)
        lock->just_acquired = 1;
}

/// @brief Release the current target (if any), firing the one-shot lost flag.
/// @param lock Target-lock payload to unlock.
static void game3d_targetlock_release(rt_game3d_targetlock *lock) {
    if (!lock->target)
        return;
    game3d_targetlock_release_instance_ref(
        &lock->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    lock->los_broken_time = 0.0;
    lock->just_lost = 1;
}

/// @brief GC finalizer: release retained references.
/// @param obj Finalized TargetLock3D payload; `NULL` is ignored.
static void game3d_targetlock_finalize(void *obj) {
    rt_game3d_targetlock *lock = (rt_game3d_targetlock *)obj;
    if (!lock)
        return;
    game3d_targetlock_release_instance_ref(
        &lock->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    game3d_targetlock_release_instance_ref(
        &lock->owner, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    game3d_targetlock_release_instance_ref(
        &lock->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    free(lock->candidate_scratch);
    lock->candidate_scratch = NULL;
    lock->candidate_capacity = 0;
    free(lock->candidate_seen);
    lock->candidate_seen = NULL;
    lock->candidate_seen_capacity = 0;
}

//=========================================================================
// Construction and properties
//=========================================================================

/// @brief Create a lock-on helper for @p owner_entity in @p world.
///   Defaults: max distance 18, cone 65°, mask all, LoS required, stickiness 1.25,
///   break distance 22.5 (max × 1.25), LoS grace 0.5 s. See header.
/// @param world_obj Borrowed live World3D handle retained by the helper.
/// @param owner_entity Borrowed live Entity3D handle retained as the lock origin.
/// @return New GC-managed TargetLock3D handle, or `NULL` after validation or allocation failure.
void *rt_game3d_targetlock_new(void *world_obj, void *owner_entity) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.TargetLock3D.New: invalid world");
    rt_game3d_entity *owner =
        game3d_entity_checked(owner_entity, "Game3D.TargetLock3D.New: owner must be Entity3D");
    if (!world || !owner)
        return NULL;
    if (!game3d_entity_validate_controller_world(
            owner, world, "Game3D.TargetLock3D.New: owner belongs to another world"))
        return NULL;
    rt_game3d_targetlock *lock = (rt_game3d_targetlock *)rt_obj_new_i64(
        RT_G3D_GAME3D_TARGETLOCK_CLASS_ID, (int64_t)sizeof(*lock));
    if (!lock) {
        rt_trap("Game3D.TargetLock3D.New: allocation failed");
        return NULL;
    }
    memset(lock, 0, sizeof(*lock));
    rt_obj_set_finalizer(lock, game3d_targetlock_finalize);
    game3d_targetlock_assign_instance_ref(
        &lock->world, world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    game3d_targetlock_assign_instance_ref(
        &lock->owner, owner, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    lock->max_distance = RT_GAME3D_TL_DEFAULT_MAX_DISTANCE;
    lock->cone_degrees = RT_GAME3D_TL_DEFAULT_CONE_DEGREES;
    lock->candidate_mask = -1;
    lock->require_los = 1;
    lock->stickiness = RT_GAME3D_TL_DEFAULT_STICKINESS;
    lock->break_distance = RT_GAME3D_TL_DEFAULT_MAX_DISTANCE * 1.25;
    lock->los_grace_seconds = RT_GAME3D_TL_DEFAULT_LOS_GRACE;
    return lock;
}

/// @brief Get the currently locked entity (NULL when unlocked/stale).
/// @param obj Borrowed TargetLock3D handle.
/// @return Borrowed live target Entity3D handle, or `NULL`.
void *rt_game3d_targetlock_get_target(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_target: invalid lock");
    return game3d_targetlock_target_ref(lock);
}

/// @brief Get the acquisition radius.
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored acquisition radius in world units, or zero when invalid.
double rt_game3d_targetlock_get_max_distance(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_maxDistance: invalid lock");
    return lock ? lock->max_distance : 0.0;
}

/// @brief Set the acquisition radius (positive; non-finite resets the default).
/// @param obj Borrowed TargetLock3D handle.
/// @param distance Requested positive radius, bounded by the coordinate limit.
void rt_game3d_targetlock_set_max_distance(void *obj, double distance) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_maxDistance: invalid lock");
    if (lock)
        lock->max_distance = game3d_positive_clamped_or(
            distance, RT_GAME3D_TL_DEFAULT_MAX_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
}

/// @brief Get the half-angle acquisition cone in degrees.
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored half-angle in degrees, or zero when invalid.
double rt_game3d_targetlock_get_cone_degrees(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_coneDegrees: invalid lock");
    return lock ? lock->cone_degrees : 0.0;
}

/// @brief Set the half-angle acquisition cone in degrees (clamped to 1..180).
/// @param obj Borrowed TargetLock3D handle.
/// @param degrees Requested half-angle; non-finite input restores the default.
void rt_game3d_targetlock_set_cone_degrees(void *obj, double degrees) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_coneDegrees: invalid lock");
    if (lock)
        lock->cone_degrees =
            game3d_clamp(game3d_finite_or(degrees, RT_GAME3D_TL_DEFAULT_CONE_DEGREES), 1.0, 180.0);
}

/// @brief Get the targetable layer mask.
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored collision-layer bit mask, or zero when invalid.
int64_t rt_game3d_targetlock_get_candidate_mask(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_candidateMask: invalid lock");
    return lock ? lock->candidate_mask : 0;
}

/// @brief Set the targetable layer mask.
/// @param obj Borrowed TargetLock3D handle.
/// @param mask Raw physics collision-layer bit mask.
void rt_game3d_targetlock_set_candidate_mask(void *obj, int64_t mask) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_candidateMask: invalid lock");
    if (lock)
        lock->candidate_mask = mask;
}

/// @brief Get whether candidates must have line of sight.
/// @param obj Borrowed TargetLock3D handle.
/// @return Nonzero when LoS filtering and maintenance are enabled.
int8_t rt_game3d_targetlock_get_require_los(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_requireLineOfSight: invalid lock");
    return lock ? lock->require_los : 0;
}

/// @brief Set whether candidates must have line of sight.
/// @param obj Borrowed TargetLock3D handle.
/// @param require Nonzero to require unobstructed candidates.
void rt_game3d_targetlock_set_require_los(void *obj, int8_t require) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_requireLineOfSight: invalid lock");
    if (lock)
        lock->require_los = require ? 1 : 0;
}

/// @brief Get the current-target score multiplier.
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored positive stickiness multiplier, or zero when invalid.
double rt_game3d_targetlock_get_stickiness(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_stickiness: invalid lock");
    return lock ? lock->stickiness : 0.0;
}

/// @brief Set the current-target score multiplier (≥ 1 keeps locks stable).
/// @param obj Borrowed TargetLock3D handle.
/// @param stickiness Requested positive multiplier, bounded to 1000.
void rt_game3d_targetlock_set_stickiness(void *obj, double stickiness) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_stickiness: invalid lock");
    if (lock)
        lock->stickiness =
            game3d_positive_clamped_or(stickiness, RT_GAME3D_TL_DEFAULT_STICKINESS, 1000.0);
}

/// @brief Get the auto-release distance.
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored break distance in world units, or zero when invalid.
double rt_game3d_targetlock_get_break_distance(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_breakDistance: invalid lock");
    return lock ? lock->break_distance : 0.0;
}

/// @brief Set the auto-release distance.
/// @param obj Borrowed TargetLock3D handle.
/// @param distance Requested positive world-space break distance.
void rt_game3d_targetlock_set_break_distance(void *obj, double distance) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_breakDistance: invalid lock");
    if (lock)
        lock->break_distance = game3d_positive_clamped_or(
            distance, RT_GAME3D_TL_DEFAULT_MAX_DISTANCE * 1.25, RT_GAME3D_COORD_ABS_MAX);
}

/// @brief Get the LoS-break grace period in seconds (0 releases instantly).
/// @param obj Borrowed TargetLock3D handle.
/// @return Stored non-negative grace duration in seconds, or zero when invalid.
double rt_game3d_targetlock_get_los_grace_seconds(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.get_losGraceSeconds: invalid lock");
    return lock ? lock->los_grace_seconds : 0.0;
}

/// @brief Set the LoS-break grace period in seconds.
/// @param obj Borrowed TargetLock3D handle.
/// @param seconds Requested non-negative duration, bounded to one hour.
void rt_game3d_targetlock_set_los_grace_seconds(void *obj, double seconds) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.set_losGraceSeconds: invalid lock");
    if (lock)
        lock->los_grace_seconds =
            game3d_nonnegative_clamped_or(seconds, RT_GAME3D_TL_DEFAULT_LOS_GRACE, 3600.0);
}

//=========================================================================
// Acquisition, cycling, maintenance
//=========================================================================

/// @brief Acquire the best candidate in view (angle-weighted 2:1 over distance,
///   sticky toward the current target). Returns true when a target is locked.
/// @param obj Borrowed TargetLock3D handle.
/// @return Nonzero when a new or existing live target is locked; otherwise zero.
int8_t rt_game3d_targetlock_acquire(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.Acquire: invalid lock");
    if (!lock)
        return 0;
    game3d_targetlock_candidate *candidates = NULL;
    int32_t count = game3d_targetlock_collect(lock, &candidates);
    for (int32_t i = 0; i < count; ++i)
        candidates[i].sort_key = -game3d_targetlock_score(lock, &candidates[i]);
    if (count > 1)
        qsort(candidates, (size_t)count, sizeof(*candidates), game3d_targetlock_candidate_compare);
    rt_game3d_world *world = game3d_targetlock_world_ref(lock);
    rt_game3d_entity *owner = game3d_targetlock_owner_ref(lock);
    for (int32_t i = 0; i < count; ++i) {
        if (lock->require_los && !game3d_targetlock_has_los(world, owner, candidates[i].entity))
            continue;
        game3d_targetlock_install(lock, candidates[i].entity);
        return 1;
    }
    return game3d_targetlock_target_ref(lock) != NULL;
}

/// @brief Release the current target without firing JustLost (explicit clear).
/// @param obj Borrowed TargetLock3D handle.
void rt_game3d_targetlock_clear(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.Clear: invalid lock");
    if (lock && lock->target) {
        game3d_targetlock_release_instance_ref(
            &lock->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
        lock->los_broken_time = 0.0;
    }
    if (lock) {
        lock->just_acquired = 0;
        lock->just_lost = 0;
    }
}

/// @brief Cycle to the nearest candidate left (-1) or right (+1) of the current
///   target in the camera basis. Returns true when the target changed.
/// @param obj Borrowed TargetLock3D handle.
/// @param direction Negative to cycle left, positive to cycle right, or zero for no action.
/// @return Nonzero when a target was acquired or changed; otherwise zero.
int8_t rt_game3d_targetlock_cycle(void *obj, int64_t direction) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.Cycle: invalid lock");
    if (!lock || direction == 0)
        return 0;
    rt_game3d_entity *current = game3d_targetlock_target_ref(lock);
    if (!current)
        return rt_game3d_targetlock_acquire(obj);
    game3d_targetlock_candidate *candidates = NULL;
    int32_t count = game3d_targetlock_collect(lock, &candidates);
    double current_yaw = 0.0;
    int have_current = 0;
    for (int32_t i = 0; i < count; ++i)
        if (candidates[i].entity == current) {
            current_yaw = candidates[i].camera_yaw;
            have_current = 1;
            break;
        }
    if (!have_current)
        current_yaw = 0.0;
    int32_t eligible = 0;
    for (int32_t i = 0; i < count; ++i) {
        if (candidates[i].entity == current)
            continue;
        double delta = candidates[i].camera_yaw - current_yaw;
        if (direction > 0 ? delta <= 1e-9 : delta >= -1e-9)
            continue;
        candidates[i].sort_key = fabs(delta);
        if (eligible != i)
            candidates[eligible] = candidates[i];
        ++eligible;
    }
    if (eligible > 1)
        qsort(
            candidates, (size_t)eligible, sizeof(*candidates), game3d_targetlock_candidate_compare);
    rt_game3d_world *world = game3d_targetlock_world_ref(lock);
    rt_game3d_entity *owner = game3d_targetlock_owner_ref(lock);
    for (int32_t i = 0; i < eligible; ++i) {
        if (lock->require_los && !game3d_targetlock_has_los(world, owner, candidates[i].entity))
            continue;
        game3d_targetlock_install(lock, candidates[i].entity);
        return 1;
    }
    return 0;
}

/// @brief Per-step maintenance: clears one-shot flags, then auto-releases on
///   target death, break distance, or LoS broken longer than the grace period.
/// @param obj Borrowed TargetLock3D handle.
/// @param dt Candidate simulation delta, sanitized and capped before grace accumulation.
void rt_game3d_targetlock_update(void *obj, double dt) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.Update: invalid lock");
    if (!lock)
        return;
    dt = game3d_clamp_controller_dt(dt);
    if (dt <= 0.0)
        return;
    lock->just_acquired = 0;
    lock->just_lost = 0;
    if (!lock->target)
        return;
    rt_game3d_entity *target = game3d_targetlock_target_ref(lock);
    rt_game3d_entity *owner = game3d_targetlock_owner_ref(lock);
    rt_game3d_world *world = game3d_targetlock_world_ref(lock);
    if (!target || !owner || !world) {
        game3d_targetlock_release(lock);
        return;
    }
    double owner_pos[3];
    double target_pos[3];
    if (!game3d_entity_world_position_components(owner, owner_pos) ||
        !game3d_entity_world_position_components(target, target_pos)) {
        game3d_targetlock_release(lock);
        return;
    }
    double dx = target_pos[0] - owner_pos[0];
    double dy = target_pos[1] - owner_pos[1];
    double dz = target_pos[2] - owner_pos[2];
    double dist = hypot(hypot(dx, dy), dz);
    if (!isfinite(dist) || dist > lock->break_distance) {
        game3d_targetlock_release(lock);
        return;
    }
    if (lock->require_los) {
        if (game3d_targetlock_has_los(world, owner, target)) {
            lock->los_broken_time = 0.0;
        } else {
            lock->los_broken_time += dt;
            if (lock->los_broken_time > lock->los_grace_seconds)
                game3d_targetlock_release(lock);
        }
    }
}

/// @brief One-shot: true for the frame after a target was acquired.
/// @param obj Borrowed TargetLock3D handle.
/// @return Current acquired-transition flag, or zero when invalid.
int8_t rt_game3d_targetlock_just_acquired(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.JustAcquired: invalid lock");
    return lock ? lock->just_acquired : 0;
}

/// @brief One-shot: true for the frame after the target was auto-released.
/// @param obj Borrowed TargetLock3D handle.
/// @return Current lost-transition flag, or zero when invalid.
int8_t rt_game3d_targetlock_just_lost(void *obj) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.JustLost: invalid lock");
    return lock ? lock->just_lost : 0;
}

/// @brief Soft aim assist: rotate a planar move vector up to 12° toward the
///   target bearing when within 30° of it. Pure function; Y is preserved.
/// @param obj Borrowed TargetLock3D handle.
/// @param move Borrowed Vec3 movement vector.
/// @return New GC-managed Vec3 containing the biased or unchanged move, or `NULL` for invalid
/// input/allocation failure.
void *rt_game3d_targetlock_locked_move_bias(void *obj, void *move) {
    rt_game3d_targetlock *lock =
        game3d_targetlock_checked(obj, "Game3D.TargetLock3D.LockedMoveBias: invalid lock");
    if (!rt_g3d_is_vec3(move)) {
        rt_trap("Game3D.TargetLock3D.LockedMoveBias: move must be Vec3");
        return NULL;
    }
    double mx = game3d_clamp_coord_or(rt_vec3_x(move), 0.0);
    double my = game3d_clamp_coord_or(rt_vec3_y(move), 0.0);
    double mz = game3d_clamp_coord_or(rt_vec3_z(move), 0.0);
    rt_game3d_entity *target = lock ? game3d_targetlock_target_ref(lock) : NULL;
    rt_game3d_entity *owner = lock ? game3d_targetlock_owner_ref(lock) : NULL;
    double move_len = hypot(mx, mz);
    if (!target || !owner || move_len <= 1e-9)
        return rt_vec3_new(mx, my, mz);
    double owner_pos[3];
    double target_pos[3];
    if (!game3d_entity_world_position_components(owner, owner_pos) ||
        !game3d_entity_world_position_components(target, target_pos))
        return rt_vec3_new(mx, my, mz);
    double bx = target_pos[0] - owner_pos[0];
    double bz = target_pos[2] - owner_pos[2];
    double bearing_len = hypot(bx, bz);
    if (bearing_len <= 1e-9)
        return rt_vec3_new(mx, my, mz);
    double move_angle = atan2(mx, mz);
    double bearing_angle = atan2(bx, bz);
    double delta = bearing_angle - move_angle;
    while (delta > RT_GAME3D_PI)
        delta -= 2.0 * RT_GAME3D_PI;
    while (delta < -RT_GAME3D_PI)
        delta += 2.0 * RT_GAME3D_PI;
    const double window = 30.0 * (RT_GAME3D_PI / 180.0);
    const double max_bias = 12.0 * (RT_GAME3D_PI / 180.0);
    if (fabs(delta) > window)
        return rt_vec3_new(mx, my, mz);
    double bias = game3d_clamp(delta, -max_bias, max_bias);
    double rotated = move_angle + bias;
    return rt_vec3_new(sin(rotated) * move_len, my, cos(rotated) * move_len);
}
