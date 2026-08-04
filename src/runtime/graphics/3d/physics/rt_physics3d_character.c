//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_character.c
// Purpose: Character3D kinematic controller (sweep-and-slide motion, step-up,
//   ground probing) and Trigger3D overlap volumes for the Physics3D runtime.
//   Split out of rt_physics3d.c; shares core types via rt_physics3d_internal.h.
//
// Key invariants:
//   - Character3D moves via kinematic sweeps against the world's bodies, sliding
//     along contact normals; up to 3 slide iterations per move axis.
//   - Trigger3D grows transactionally and stores zeroing weak body references;
//     stale entries are pruned on the next Update.
//
// Ownership/Lifetime:
//   - Character3D / Trigger3D are GC-managed; finalizers release retained refs.
//
// Links: rt_physics3d_internal.h, rt_physics3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_character.c
 * @brief Implements the swept Character3D controller and standalone Trigger3D volumes.
 *
 * Character movement uses bounded penetration recovery, conservative
 * broadphase shortlists, binary-refined capsule sweeps, slide projection,
 * stair stepping, slope classification, optional dynamic-body pushing, and
 * moving-platform displacement. Trigger volumes maintain weak occupancy sets
 * and derive aggregate enter/exit edges from world-space AABB overlap.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_physics3d_query_internal.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Character3D controller payload: the kinematic body it drives, its
///   owning world, step-up height, walkable-slope cosine, grounded/sliding state,
///   dynamic-body interaction tuning, and the retained ground body (platforms).
typedef struct {
    void *vptr;
    rt_body3d *body;
    rt_world3d *world;
    double step_height;
    double slope_limit_cos;
    int8_t is_grounded;
    int8_t was_grounded;
    int8_t is_sliding;      /* standing on a non-walkable surface this step */
    int8_t collide_dynamic; /* dynamic bodies block/push (default on) */
    int8_t ride_platforms;  /* pre-displace with kinematic ground motion */
    double push_strength;   /* dynamic push impulse scale (0 = block only) */
    rt_body3d *ground_body; /* retained body under our feet, NULL airborne */
    rt_body3d *pushed_body; /* impulse-once-per-step guard, weak in-step ref */
    /* Per-Move broadphase shortlist: bodies whose AABB overlaps the swept move
     * volume. While active, position probes narrow-phase only this list instead
     * of every world body. Weak in-step refs; the world cannot mutate its body
     * set during a Move. */
    rt_body3d **move_candidates;
    int32_t move_candidate_count;
    int32_t move_candidate_capacity;
    int8_t move_candidates_active;
} rt_character3d;

/// @brief Validate @p obj as a Character3D handle (NULL on mismatch).
/// @param obj Runtime object handle to validate.
/// @return Typed Character3D payload, or NULL for a class mismatch.
static rt_character3d *character3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_CHARACTER3D_CLASS_ID, sizeof(rt_character3d))
               ? (rt_character3d *)obj
               : NULL;
}

/*==========================================================================
 * Character Controller
 *=========================================================================*/

typedef struct {
    rt_body3d *body;
    double normal[3];
    double depth;
    double fraction;
    int8_t hit;
} rt_character_hit3d;

#define CHARACTER3D_COORD_ABS_MAX 1000000000000.0
#define CHARACTER3D_MOVE_ABS_MAX 1000.0
#define CHARACTER3D_STEP_HEIGHT_MAX 100.0
#define CHARACTER3D_HEIGHT_MAX 1000000.0
#define CHARACTER3D_DT_MAX 1.0

/// @brief Clamp a character/trigger coordinate to a finite physics state range.
/// @param value Coordinate to sanitize.
/// @return Finite coordinate limited to the controller's absolute range.
static double character3d_saturate_coord(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > CHARACTER3D_COORD_ABS_MAX)
        return CHARACTER3D_COORD_ABS_MAX;
    if (value < -CHARACTER3D_COORD_ABS_MAX)
        return -CHARACTER3D_COORD_ABS_MAX;
    return value;
}

/// @brief Clamp a Vec3 in place for character controller math.
/// @param v Three-component vector to sanitize, or NULL.
static void character3d_sanitize_vec3(double v[3]) {
    if (!v)
        return;
    v[0] = character3d_saturate_coord(v[0]);
    v[1] = character3d_saturate_coord(v[1]);
    v[2] = character3d_saturate_coord(v[2]);
}

/// @brief Normalize a collision normal, returning 0 when no reliable direction exists.
/// @param out Three-component output receiving the sanitized unit normal.
/// @param normal Three-component source normal.
/// @return One when a reliable normalized direction was produced, otherwise zero.
static int character3d_sanitize_contact_normal(double out[3], const double *normal) {
    if (!out || !normal)
        return 0;
    out[0] = character3d_saturate_coord(normal[0]);
    out[1] = character3d_saturate_coord(normal[1]);
    out[2] = character3d_saturate_coord(normal[2]);
    return vec3_normalize_in_place(out) > 1e-12;
}

/// @brief Copy and cap a movement vector, preserving direction for extreme finite velocities.
/// @param src Three-component movement vector to sanitize.
/// @param out Three-component output receiving the capped vector.
/// @return Sanitized vector length, or zero for invalid or negligible input.
static double character3d_sanitize_delta(const double *src, double out[3]) {
    double len;
    double x;
    double y;
    double z;
    if (!out)
        return 0.0;
    if (!src) {
        vec3_set(out, 0.0, 0.0, 0.0);
        return 0.0;
    }
    /* Snapshot every source lane before writing so in-place sanitization is
     * well-defined and static analysis can prove that a rejected source still
     * leaves a deterministic output. */
    x = character3d_saturate_coord(src[0]);
    y = character3d_saturate_coord(src[1]);
    z = character3d_saturate_coord(src[2]);
    out[0] = x;
    out[1] = y;
    out[2] = z;
    len = vec3_len(out);
    if (!isfinite(len) || len <= 1e-12) {
        vec3_set(out, 0.0, 0.0, 0.0);
        return 0.0;
    }
    if (len > CHARACTER3D_MOVE_ABS_MAX) {
        double scale = CHARACTER3D_MOVE_ABS_MAX / len;
        out[0] *= scale;
        out[1] *= scale;
        out[2] *= scale;
        len = CHARACTER3D_MOVE_ABS_MAX;
    }
    return len;
}

/// @brief Clamp controller step heights to a non-negative, physically usable range.
/// @param value Requested step height.
/// @return Finite step height between zero and the controller maximum.
static double character3d_sanitize_step_height(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return value > CHARACTER3D_STEP_HEIGHT_MAX ? CHARACTER3D_STEP_HEIGHT_MAX : value;
}

// Character controller — built on top of Body3D with custom motion
// resolution: kinematic-style sweeps + slide along surfaces, optional
// step-up over small obstacles, ground probing for "is grounded" state.

/// @brief Retain @p body into the controller's ground slot (NULL clears).
/// @param ctrl Character3D payload whose support reference is replaced.
/// @param body Supporting Body3D to retain, or NULL when airborne.
static void character3d_retain_ground_body(rt_character3d *ctrl, rt_body3d *body) {
    if (!ctrl || ctrl->ground_body == body)
        return;
    if (body)
        rt_obj_retain_maybe(body);
    if (ctrl->ground_body && rt_obj_release_check0(ctrl->ground_body))
        rt_obj_free(ctrl->ground_body);
    ctrl->ground_body = body;
}

/// @brief Update the controller's grounded flag and store the latest ground normal.
///
/// Negates the contact normal because the contact normal points from
/// the body toward the ground; we want the surface normal pointing up.
/// @param ctrl Character3D payload whose public and body ground state is updated.
/// @param grounded Nonzero when the controller has walkable support.
/// @param normal Optional body-to-ground contact normal.
static void character3d_set_ground_state(rt_character3d *ctrl,
                                         int8_t grounded,
                                         const double *normal) {
    if (!ctrl || !ctrl->body)
        return;
    ctrl->is_grounded = grounded;
    ctrl->body->is_grounded = grounded;
    if (!grounded)
        character3d_retain_ground_body(ctrl, NULL);
    if (grounded && normal) {
        double contact_normal[3];
        if (character3d_sanitize_contact_normal(contact_normal, normal)) {
            ctrl->body->ground_normal[0] = -contact_normal[0];
            ctrl->body->ground_normal[1] = -contact_normal[1];
            ctrl->body->ground_normal[2] = -contact_normal[2];
        } else {
            ctrl->body->ground_normal[0] = 0.0;
            ctrl->body->ground_normal[1] = 1.0;
            ctrl->body->ground_normal[2] = 0.0;
        }
    } else {
        ctrl->body->ground_normal[0] = 0.0;
        ctrl->body->ground_normal[1] = 1.0;
        ctrl->body->ground_normal[2] = 0.0;
    }
}

/// @brief True if the surface (negated contact normal) is below the slope limit.
///
/// `slope_limit_cos = cos(max_slope_angle)`; a "walkable" surface has
/// `normal_y >= cos(angle)`. Used to gate ground-snapping and step-up.
/// @param ctrl Character3D payload providing the slope threshold.
/// @param normal Body-to-surface contact normal to classify.
/// @return One when the surface is walkable, otherwise zero.
static int character3d_normal_is_walkable(const rt_character3d *ctrl, const double *normal) {
    double contact_normal[3];
    return ctrl && normal && character3d_sanitize_contact_normal(contact_normal, normal) &&
           (-contact_normal[1] >= ctrl->slope_limit_cos);
}

/// @brief Filter for which world bodies the controller should slide against.
///
/// Excludes self and triggers. Dynamic bodies block (and are pushed via
/// `push_strength`) by default; `rt_character3d_set_collide_dynamic(ctrl, 0)`
/// restores the legacy ghost-through behavior. Honors the standard
/// layer/mask filter.
/// @param ctrl Character3D payload providing self, world, and filter state.
/// @param other Candidate world body.
/// @return One when the controller should collide with @p other, otherwise zero.
static int character3d_candidate_body(const rt_character3d *ctrl, const rt_body3d *other) {
    if (!ctrl || !ctrl->body || !ctrl->world || !other)
        return 0;
    if (other == ctrl->body)
        return 0;
    if (other->is_trigger)
        return 0;
    if (other->motion_mode == PH3D_MODE_DYNAMIC && !ctrl->collide_dynamic)
        return 0;
    return bodies_can_collide(ctrl->body, other);
}

/// @brief Repair reusable move-candidate metadata before a shortlist operation.
/// @param ctrl Character controller whose pointer/count/capacity tuple is normalized.
static void character3d_repair_move_candidates(rt_character3d *ctrl) {
    if (!ctrl)
        return;
    if (!ctrl->move_candidates || ctrl->move_candidate_capacity < 0) {
        ctrl->move_candidate_count = 0;
        ctrl->move_candidate_capacity = 0;
        ctrl->move_candidates_active = 0;
        return;
    }
    if (ctrl->move_candidate_count < 0)
        ctrl->move_candidate_count = 0;
    if (ctrl->move_candidate_count > ctrl->move_candidate_capacity)
        ctrl->move_candidate_count = ctrl->move_candidate_capacity;
    ctrl->move_candidates_active = ctrl->move_candidates_active ? 1 : 0;
}

/// @brief Ensure the per-Move candidate shortlist can hold @p needed body pointers.
/// @param ctrl Character3D payload whose reusable buffer may grow.
/// @param needed Required non-negative number of pointer slots.
/// @return One when capacity is available, otherwise zero.
static int character3d_reserve_move_candidates(rt_character3d *ctrl, int32_t needed) {
    rt_body3d **grown;
    int32_t new_cap;
    if (!ctrl || needed < 0)
        return 0;
    character3d_repair_move_candidates(ctrl);
    if (ctrl->move_candidates && ctrl->move_candidate_capacity >= needed)
        return 1;
    new_cap = ctrl->move_candidate_capacity > 0 ? ctrl->move_candidate_capacity : 16;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(*grown))
        return 0;
    grown = (rt_body3d **)realloc(ctrl->move_candidates, (size_t)new_cap * sizeof(*grown));
    if (!grown)
        return 0;
    ctrl->move_candidates = grown;
    ctrl->move_candidate_capacity = new_cap;
    return 1;
}

/// @brief Build the per-Move broadphase shortlist for a swept move volume.
///
/// Collects every candidate body whose cached broadphase AABB overlaps the
/// conservative volume the controller can touch this Move: the start position
/// expanded on every axis by the total move length (sliding can redirect
/// motion onto any axis), the capsule extents, the step-up/probe reach, and a
/// safety margin. Falls back to full-world scans (shortlist inactive) if the
/// broadphase cannot be built.
/// @param ctrl Character3D payload whose candidate cache is prepared.
/// @param start Three-component world-space movement start.
/// @param move_len Conservative total requested movement length.
static void character3d_begin_move_candidates(rt_character3d *ctrl,
                                              const double *start,
                                              double move_len) {
    if (!ctrl)
        return;
    character3d_repair_move_candidates(ctrl);
    ctrl->move_candidates_active = 0;
    ctrl->move_candidate_count = 0;
    if (!ctrl->world || !ctrl->body || !start || !isfinite(move_len) || move_len < 0.0)
        return;

    int32_t entry_count = world3d_build_query_broadphase(ctrl->world);
    if (entry_count < 0)
        return;

    double half_height = isfinite(ctrl->body->height) ? fabs(ctrl->body->height) * 0.5 : 1.0;
    double radius = isfinite(ctrl->body->radius) ? fabs(ctrl->body->radius) : 0.5;
    double step = character3d_sanitize_step_height(ctrl->step_height);
    double reach = move_len + half_height + radius + step + 0.6;
    if (!isfinite(reach) || reach <= 0.0)
        return;

    double qmin[3];
    double qmax[3];
    for (int axis = 0; axis < 3; axis++) {
        double center = character3d_saturate_coord(start[axis]);
        qmin[axis] = character3d_saturate_coord(center - reach);
        qmax[axis] = character3d_saturate_coord(center + reach);
    }

    for (int32_t i = 0; i < entry_count; i++) {
        const ph3d_broadphase_entry *entry = &ctrl->world->query_broadphase_entries[i];
        if (!query_entry_overlaps_bounds(entry, qmin, qmax))
            continue;
        if (!character3d_candidate_body(ctrl, entry->body))
            continue;
        if (ctrl->move_candidate_count >= INT32_MAX ||
            !character3d_reserve_move_candidates(ctrl, ctrl->move_candidate_count + 1))
            return; /* stay inactive: full scan remains correct */
        ctrl->move_candidates[ctrl->move_candidate_count++] = entry->body;
    }
    ctrl->move_candidates_active = 1;
}

/// @brief Deactivate the per-Move shortlist (buffer is kept for reuse).
/// @param ctrl Character3D payload whose active shortlist is cleared.
static void character3d_end_move_candidates(rt_character3d *ctrl) {
    if (!ctrl)
        return;
    ctrl->move_candidates_active = 0;
    ctrl->move_candidate_count = 0;
}

/// @brief Probe what the controller would collide with at a given position.
///
/// Temporarily moves the body to `pos`, runs the standard narrow-phase
/// against every candidate body (the per-Move broadphase shortlist when one
/// is active, otherwise every world body), restores the original position,
/// and returns the deepest contact (if any). Used for both penetration
/// resolution and binary-searched sweeps.
/// @param ctrl Character3D payload and world to test.
/// @param pos Three-component candidate world position.
/// @param out_hit Optional output receiving the deepest contact.
/// @return One when the candidate position penetrates a blocking body, otherwise zero.
static int character3d_test_position(rt_character3d *ctrl,
                                     const double *pos,
                                     rt_character_hit3d *out_hit) {
    if (!ctrl || !ctrl->body || !ctrl->world)
        return 0;

    rt_body3d *body = ctrl->body;
    double saved[3] = {body->position[0], body->position[1], body->position[2]};
    double test_pos[3] = {pos[0], pos[1], pos[2]};
    character3d_sanitize_vec3(test_pos);
    body->position[0] = test_pos[0];
    body->position[1] = test_pos[1];
    body->position[2] = test_pos[2];

    character3d_repair_move_candidates(ctrl);
    rt_body3d **candidates =
        ctrl->move_candidates_active ? ctrl->move_candidates : ctrl->world->bodies;
    int32_t candidate_capacity =
        ctrl->move_candidates_active ? ctrl->move_candidate_capacity : ctrl->world->body_capacity;
    int32_t candidate_count =
        ctrl->move_candidates_active ? ctrl->move_candidate_count : ctrl->world->body_count;
    if (!candidates || candidate_capacity < 0 || candidate_count < 0)
        candidate_count = 0;
    else if (candidate_count > candidate_capacity)
        candidate_count = candidate_capacity;

    rt_character_hit3d best = {0};
    for (int32_t i = 0; i < candidate_count; i++) {
        rt_body3d *other = candidates[i];
        double normal[3], depth;
        if (!character3d_candidate_body(ctrl, other))
            continue;
        if (!test_collision(body, other, normal, &depth, NULL, NULL, NULL, NULL, NULL))
            continue;
        if (!isfinite(depth) || depth <= 0.0 ||
            !character3d_sanitize_contact_normal(normal, normal))
            continue;
        if (!best.hit || depth > best.depth) {
            best.hit = 1;
            best.body = other;
            best.depth = depth > CHARACTER3D_MOVE_ABS_MAX ? CHARACTER3D_MOVE_ABS_MAX : depth;
            vec3_copy(best.normal, normal);
        }
    }

    body->position[0] = saved[0];
    body->position[1] = saved[1];
    body->position[2] = saved[2];

    if (best.hit && out_hit)
        *out_hit = best;
    return best.hit;
}

/// @brief Push the controller out of any penetration it currently has.
///
/// Up to 6 iterations: probe at current position, push along the
/// deepest normal by `depth + 1e-4` epsilon, repeat. Bails early
/// when no penetration remains. Bounded so degenerate stuck cases
/// terminate quickly.
/// @param ctrl Character3D payload whose body position may be corrected.
static void character3d_resolve_penetration(rt_character3d *ctrl) {
    if (!ctrl || !ctrl->body)
        return;
    for (int iter = 0; iter < 6; iter++) {
        rt_character_hit3d hit;
        double pos[3] = {ctrl->body->position[0], ctrl->body->position[1], ctrl->body->position[2]};
        character3d_sanitize_vec3(pos);
        if (!character3d_test_position(ctrl, pos, &hit))
            return;
        double push = hit.depth + 1e-4;
        if (!isfinite(push) || push <= 0.0)
            return;
        if (push > CHARACTER3D_MOVE_ABS_MAX)
            push = CHARACTER3D_MOVE_ABS_MAX;
        ctrl->body->position[0] =
            character3d_saturate_coord(ctrl->body->position[0] - hit.normal[0] * push);
        ctrl->body->position[1] =
            character3d_saturate_coord(ctrl->body->position[1] - hit.normal[1] * push);
        ctrl->body->position[2] =
            character3d_saturate_coord(ctrl->body->position[2] - hit.normal[2] * push);
    }
}

/// @brief Sweep the controller along `delta`, stopping at the first contact.
///
/// Steps in `radius/4`-sized substeps; on the first substep that hits,
/// 14 iterations of bisection refine the impact `t`. On no-hit, body
/// is moved to the end position and the function returns 0. Step count
/// is bounded to 128.
/// @param ctrl Character3D payload whose body is swept and moved.
/// @param delta Three-component requested world-space displacement.
/// @param out_hit Optional output receiving the refined first collision.
/// @return One when motion was clipped by a blocking body, otherwise zero.
static int character3d_sweep(rt_character3d *ctrl,
                             const double *delta,
                             rt_character_hit3d *out_hit) {
    double move_delta[3];
    double move_len;
    if (!ctrl || !ctrl->body)
        return 0;
    move_len = character3d_sanitize_delta(delta, move_delta);
    if (move_len <= 1e-12)
        return 0;

    rt_body3d *body = ctrl->body;
    double start[3] = {body->position[0], body->position[1], body->position[2]};
    character3d_sanitize_vec3(start);
    double end[3] = {character3d_saturate_coord(start[0] + move_delta[0]),
                     character3d_saturate_coord(start[1] + move_delta[1]),
                     character3d_saturate_coord(start[2] + move_delta[2])};
    double step_dist = body->radius > 1e-6 ? body->radius * 0.25 : 0.05;
    double step_count;
    int steps;
    double prev_t = 0.0;
    rt_character_hit3d hit;

    if (!isfinite(step_dist) || step_dist < 0.05)
        step_dist = 0.05;
    step_count = ceil(move_len / step_dist);
    if (!isfinite(step_count) || step_count > 128.0)
        steps = 128;
    else if (step_count < 1.0)
        steps = 1;
    else
        steps = (int)step_count;
    if (steps < 1)
        steps = 1;

    for (int s = 1; s <= steps; s++) {
        double t = (double)s / (double)steps;
        double pos[3] = {character3d_saturate_coord(start[0] + move_delta[0] * t),
                         character3d_saturate_coord(start[1] + move_delta[1] * t),
                         character3d_saturate_coord(start[2] + move_delta[2] * t)};
        if (!character3d_test_position(ctrl, pos, &hit)) {
            prev_t = t;
            continue;
        }

        {
            double lo = prev_t;
            double hi = t;
            rt_character_hit3d best_hit = hit;
            for (int iter = 0; iter < 14; iter++) {
                double mid = (lo + hi) * 0.5;
                double mid_pos[3] = {character3d_saturate_coord(start[0] + move_delta[0] * mid),
                                     character3d_saturate_coord(start[1] + move_delta[1] * mid),
                                     character3d_saturate_coord(start[2] + move_delta[2] * mid)};
                if (character3d_test_position(ctrl, mid_pos, &hit)) {
                    hi = mid;
                    best_hit = hit;
                } else {
                    lo = mid;
                }
            }

            body->position[0] = character3d_saturate_coord(start[0] + move_delta[0] * lo);
            body->position[1] = character3d_saturate_coord(start[1] + move_delta[1] * lo);
            body->position[2] = character3d_saturate_coord(start[2] + move_delta[2] * lo);
            best_hit.hit = 1;
            best_hit.fraction = clampd(lo, 0.0, 1.0);
            if (out_hit)
                *out_hit = best_hit;
            return 1;
        }
    }

    body->position[0] = end[0];
    body->position[1] = end[1];
    body->position[2] = end[2];
    if (out_hit) {
        rt_character_hit3d zero = {0};
        *out_hit = zero;
    }
    return 0;
}

/// @brief Drop the controller 5cm and check for a walkable surface.
///
/// Used to detect grounded state when the controller is just barely
/// above the floor (after a small jump or when sliding down a slight
/// slope). Updates the body's grounded flag accordingly.
/// @param ctrl Character3D payload to probe and update.
/// @return One when a walkable supporting surface is found, otherwise zero.
static int character3d_probe_ground(rt_character3d *ctrl) {
    if (!ctrl || !ctrl->body)
        return 0;
    double probe_pos[3] = {
        ctrl->body->position[0], ctrl->body->position[1] - 0.05, ctrl->body->position[2]};
    rt_character_hit3d hit;
    if (character3d_test_position(ctrl, probe_pos, &hit)) {
        if (character3d_normal_is_walkable(ctrl, hit.normal)) {
            character3d_set_ground_state(ctrl, 1, hit.normal);
            character3d_retain_ground_body(ctrl, hit.body);
            return 1;
        }
        /* Resting against a too-steep surface: not grounded, but sliding. */
        ctrl->is_sliding = 1;
    }
    character3d_set_ground_state(ctrl, 0, NULL);
    return 0;
}

/// @brief Attempt to step up over a small obstacle (FPS-style stair climb).
///
/// Three-phase test:
///   1. Sweep up by `step_height`. If blocked, abort.
///   2. Sweep horizontally by the leftover delta. If blocked, abort.
///   3. Sweep down (slightly past `step_height`) onto the new surface.
///      If the new surface is walkable, commit and mark grounded.
/// On any failure the controller is restored to its original position.
/// @param ctrl Character3D payload whose body attempts the step.
/// @param horizontal_delta Three-component horizontal displacement remaining after impact.
/// @return One when the complete up-across-down traversal succeeds, otherwise zero.
static int character3d_try_step(rt_character3d *ctrl, const double *horizontal_delta) {
    double step_delta[3];
    if (!ctrl || !ctrl->body || ctrl->step_height <= 1e-6 ||
        character3d_sanitize_delta(horizontal_delta, step_delta) <= 1e-12)
        return 0;

    double start[3] = {ctrl->body->position[0], ctrl->body->position[1], ctrl->body->position[2]};
    character3d_sanitize_vec3(start);
    double up[3] = {0.0, ctrl->step_height, 0.0};
    rt_character_hit3d hit;

    if (character3d_sweep(ctrl, up, &hit)) {
        ctrl->body->position[0] = start[0];
        ctrl->body->position[1] = start[1];
        ctrl->body->position[2] = start[2];
        return 0;
    }
    character3d_resolve_penetration(ctrl);

    if (character3d_sweep(ctrl, step_delta, &hit)) {
        ctrl->body->position[0] = start[0];
        ctrl->body->position[1] = start[1];
        ctrl->body->position[2] = start[2];
        return 0;
    }
    character3d_resolve_penetration(ctrl);

    {
        double down[3] = {0.0, -(ctrl->step_height + 0.05), 0.0};
        if (character3d_sweep(ctrl, down, &hit) &&
            character3d_normal_is_walkable(ctrl, hit.normal)) {
            character3d_set_ground_state(ctrl, 1, hit.normal);
            character3d_retain_ground_body(ctrl, hit.body);
            return 1;
        }
    }

    ctrl->body->position[0] = start[0];
    ctrl->body->position[1] = start[1];
    ctrl->body->position[2] = start[2];
    return 0;
}

/// @brief Push a blocking dynamic body once per step: impulse along the contact
///   normal proportional to the approach speed, mass-ratio scaled so light props
///   yield and heavy props wall the controller. Applied only on the resolved
///   contact (never inside sweep bisection) so bodies cannot gain energy.
/// @param ctrl Character3D payload providing push tuning and the source mass.
/// @param hit Resolved collision against a candidate dynamic body.
/// @param attempted_delta Three-component displacement that approached the contact.
/// @param dt Positive movement interval used to recover approach speed.
static void character3d_push_dynamic(rt_character3d *ctrl,
                                     const rt_character_hit3d *hit,
                                     const double *attempted_delta,
                                     double dt) {
    if (!ctrl || !ctrl->body || !hit || !hit->hit || !hit->body)
        return;
    if (hit->body->motion_mode != PH3D_MODE_DYNAMIC)
        return;
    if (ctrl->push_strength <= 0.0 || !isfinite(dt) || dt <= 1e-9)
        return;
    if (ctrl->pushed_body == hit->body)
        return; /* once per contact per step */
    double v_into = vec3_dot(attempted_delta, hit->normal) / dt;
    if (!isfinite(v_into) || v_into <= 0.0)
        return;
    double other_mass = hit->body->mass > 1e-9 ? hit->body->mass : 1e-9;
    /* A kinematic capsule is often authored with mass 0; the configured
     * push_strength must still act, so floor the effective controller mass at
     * the blocker's mass (ratio 1) instead of silently zeroing the push. */
    double controller_mass = ctrl->body->mass > 1e-9 ? ctrl->body->mass : other_mass;
    double ratio = controller_mass / other_mass;
    if (!isfinite(ratio) || ratio > 1.0)
        ratio = 1.0;
    double mag = ctrl->push_strength * ratio * v_into;
    if (!isfinite(mag) || mag <= 0.0)
        return;
    /* Approximate contact point: capsule surface along the contact normal. */
    double px =
        character3d_saturate_coord(ctrl->body->position[0] + hit->normal[0] * ctrl->body->radius);
    double py =
        character3d_saturate_coord(ctrl->body->position[1] + hit->normal[1] * ctrl->body->radius);
    double pz =
        character3d_saturate_coord(ctrl->body->position[2] + hit->normal[2] * ctrl->body->radius);
    rt_body3d_apply_impulse_at_point(
        hit->body, hit->normal[0] * mag, hit->normal[1] * mag, hit->normal[2] * mag, px, py, pz);
    rt_body3d_wake(hit->body);
    ctrl->pushed_body = hit->body;
}

/// @brief Slide-and-iterate motion solver — the heart of the controller's `Move`.
///
/// Up to 4 iterations of: resolve penetration → sweep → if hit,
/// project leftover motion onto the contact plane (or try the bounded
/// step-up path for a horizontal obstruction) → continue with the motion. This
/// gives the "slide along walls" feel typical of FPS controllers.
/// Vertical hits onto walkable surfaces also set the grounded flag
/// so gravity stops compounding.
/// @param ctrl Character3D payload whose body is moved.
/// @param initial_delta Three-component requested displacement.
/// @param allow_step Nonzero to permit stair-step traversal for horizontal obstructions.
/// @param dt Positive movement interval used for dynamic-body push impulses.
static void character3d_move_axis(rt_character3d *ctrl,
                                  const double *initial_delta,
                                  int allow_step,
                                  double dt) {
    double remaining[3];
    if (character3d_sanitize_delta(initial_delta, remaining) <= 1e-12)
        return;
    for (int iter = 0; iter < 4; iter++) {
        rt_character_hit3d hit;
        double leftover[3];

        if (character3d_sanitize_delta(remaining, remaining) <= 1e-12)
            return;

        character3d_resolve_penetration(ctrl);
        if (!character3d_sweep(ctrl, remaining, &hit))
            return;

        character3d_push_dynamic(ctrl, &hit, remaining, dt);

        leftover[0] = remaining[0] * (1.0 - hit.fraction);
        leftover[1] = remaining[1] * (1.0 - hit.fraction);
        leftover[2] = remaining[2] * (1.0 - hit.fraction);
        character3d_sanitize_delta(leftover, leftover);

        /* A horizontal capsule sweep intersects the supporting heightfield as
         * soon as the ground rises, even when the sampled surface is almost
         * flat.  Treating that walkable contact only as a slide can repeatedly
         * consume the tiny horizontal remainder at cell boundaries, leaving a
         * controller unable to cross otherwise navigable rolling terrain.
         *
         * The existing three-phase step is also the correct bounded traversal
         * for this case: lift by the configured step height, cross the
         * remainder, then settle onto a walkable surface.  Applying it to every
         * horizontal obstruction (not just wall-like contacts) preserves the
         * slope limit and step-height contract while preventing ground contact
         * from becoming an invisible wall. */
        if (allow_step && character3d_try_step(ctrl, leftover))
            return;

        if (remaining[1] < 0.0 && character3d_normal_is_walkable(ctrl, hit.normal)) {
            character3d_set_ground_state(ctrl, 1, hit.normal);
            character3d_retain_ground_body(ctrl, hit.body);
            return;
        }
        if (remaining[1] < 0.0 && fabs(remaining[0]) + fabs(remaining[2]) < 1e-12)
            ctrl->is_sliding = 1; /* descending onto a too-steep surface */

        {
            double into = vec3_dot(leftover, hit.normal);
            if (isfinite(into) && into > 0.0) {
                leftover[0] = character3d_saturate_coord(leftover[0] - hit.normal[0] * into);
                leftover[1] = character3d_saturate_coord(leftover[1] - hit.normal[1] * into);
                leftover[2] = character3d_saturate_coord(leftover[2] - hit.normal[2] * into);
            } else {
                leftover[0] = leftover[1] = leftover[2] = 0.0;
            }
        }

        character3d_sanitize_delta(leftover, remaining);
    }
}

/// @brief GC finalizer for `Character3D` — release the body, world, and ground refs.
/// @param obj Character3D payload to finalize.
static void character3d_finalizer(void *obj) {
    rt_character3d *c = (rt_character3d *)obj;
    if (!c)
        return;
    free(c->move_candidates);
    c->move_candidates = NULL;
    c->move_candidate_count = 0;
    c->move_candidate_capacity = 0;
    c->move_candidates_active = 0;
    if (c->body && rt_obj_release_check0(c->body))
        rt_obj_free(c->body);
    c->body = NULL;
    if (c->world && rt_obj_release_check0(c->world))
        rt_obj_free(c->world);
    c->world = NULL;
    if (c->ground_body && rt_obj_release_check0(c->ground_body))
        rt_obj_free(c->ground_body);
    c->ground_body = NULL;
}

/// @brief `Physics3D.Character.New(radius, height, mass)` — make a capsule character.
///
/// Creates an internally-owned capsule body and wraps it. Defaults:
/// 30cm step height, 45° max walkable slope. The character is not
/// added to a world automatically — call `SetWorld` before using
/// `Move`.
/// @param radius Capsule radius in world units.
/// @param height Total capsule height including both caps.
/// @param mass Character mass used for dynamic-body pushing.
/// @return Newly allocated Character3D handle, or NULL when construction fails.
void *rt_character3d_new(double radius, double height, double mass) {
    rt_character3d *c = (rt_character3d *)rt_obj_new_i64(RT_G3D_CHARACTER3D_CLASS_ID,
                                                         (int64_t)sizeof(rt_character3d));
    if (!c) {
        rt_trap("Physics3D.Character.New: allocation failed");
        return NULL;
    }
    c->vptr = NULL;
    c->body = (rt_body3d *)rt_body3d_new_capsule(radius, height, mass);
    if (!c->body) {
        if (rt_obj_release_check0(c))
            rt_obj_free(c);
        return NULL;
    }
    c->world = NULL;
    c->step_height = 0.3;
    c->slope_limit_cos = cos(45.0 * 3.14159265358979323846 / 180.0);
    c->is_grounded = 0;
    c->was_grounded = 0;
    c->is_sliding = 0;
    c->collide_dynamic = 1;
    c->ride_platforms = 1;
    c->push_strength = 1.0;
    c->ground_body = NULL;
    c->pushed_body = NULL;
    c->move_candidates = NULL;
    c->move_candidate_count = 0;
    c->move_candidate_capacity = 0;
    c->move_candidates_active = 0;
    rt_obj_set_finalizer(c, character3d_finalizer);
    return c;
}

/// @brief `Character3D.Move(velocity, dt)` — kinematic move with sliding.
///
/// Splits the velocity into horizontal (allows step-up) and vertical
/// (does not), runs `character3d_move_axis` for each, then probes the
/// ground if not already grounded. Updates the body's velocity to the
/// actual achieved displacement / dt — useful for animation systems
/// that read velocity off the controller.
/// @param obj Character3D handle to move.
/// @param velocity_vec Vec3 requested world-space velocity.
/// @param dt Positive finite movement interval, capped at one second.
void rt_character3d_move(void *obj, void *velocity_vec, double dt) {
    rt_character3d *ctrl = character3d_checked(obj);
    if (!ctrl || !rt_g3d_is_vec3(velocity_vec) || !isfinite(dt) || dt <= 0)
        return;
    if (dt > CHARACTER3D_DT_MAX)
        dt = CHARACTER3D_DT_MAX;
    rt_body3d *body = ctrl->body;
    if (!body)
        return;

    double velocity[3] = {ph3d_finite_or(rt_vec3_x(velocity_vec), 0.0),
                          ph3d_finite_or(rt_vec3_y(velocity_vec), 0.0),
                          ph3d_finite_or(rt_vec3_z(velocity_vec), 0.0)};
    character3d_sanitize_vec3(velocity);

    ctrl->was_grounded = ctrl->is_grounded;
    ctrl->pushed_body = NULL;
    ctrl->is_sliding = 0;

    /* Moving platforms: while grounded on a kinematic/static body that is
     * moving, pre-displace by the platform's step displacement (linear plus
     * yaw about the platform origin) BEFORE the swept move, so a wall on the
     * platform still blocks the ride. */
    if (ctrl->ride_platforms && ctrl->is_grounded && ctrl->ground_body &&
        ctrl->ground_body->motion_mode != PH3D_MODE_DYNAMIC) {
        rt_body3d *platform = ctrl->ground_body;
        double lin[3] = {ph3d_finite_or(platform->velocity[0], 0.0) * dt,
                         ph3d_finite_or(platform->velocity[1], 0.0) * dt,
                         ph3d_finite_or(platform->velocity[2], 0.0) * dt};
        double yaw = ph3d_finite_or(platform->angular_velocity[1], 0.0) * dt;
        double px = body->position[0];
        double pz = body->position[2];
        if (fabs(yaw) > 1e-12) {
            double ox = px - platform->position[0];
            double oz = pz - platform->position[2];
            double c = cos(yaw);
            double s = sin(yaw);
            px = platform->position[0] + ox * c - oz * s;
            pz = platform->position[2] + ox * s + oz * c;
        }
        body->position[0] = character3d_saturate_coord(px + lin[0]);
        body->position[1] = character3d_saturate_coord(body->position[1] + lin[1]);
        body->position[2] = character3d_saturate_coord(pz + lin[2]);
    }

    character3d_set_ground_state(ctrl, 0, NULL);

    {
        double start[3] = {body->position[0], body->position[1], body->position[2]};
        character3d_sanitize_vec3(start);
        double horizontal[3] = {velocity[0] * dt, 0.0, velocity[2] * dt};
        double vertical[3] = {0.0, velocity[1] * dt, 0.0};
        double move_len = vec3_len(horizontal) + vec3_len(vertical);

        character3d_begin_move_candidates(ctrl, start, move_len);
        character3d_resolve_penetration(ctrl);
        character3d_move_axis(ctrl, horizontal, 1, dt);
        character3d_move_axis(ctrl, vertical, 0, dt);
        character3d_resolve_penetration(ctrl);
        if (!ctrl->is_grounded)
            character3d_probe_ground(ctrl);
        character3d_end_move_candidates(ctrl);

        body->position[0] = character3d_saturate_coord(body->position[0]);
        body->position[1] = character3d_saturate_coord(body->position[1]);
        body->position[2] = character3d_saturate_coord(body->position[2]);
        body->velocity[0] = character3d_saturate_coord((body->position[0] - start[0]) / dt);
        body->velocity[1] = character3d_saturate_coord((body->position[1] - start[1]) / dt);
        body->velocity[2] = character3d_saturate_coord((body->position[2] - start[2]) / dt);
        ph3d_vec3_sanitize_state(body->velocity);
        /* The swept move mutated the body's position directly; stamp the
         * broadphase so later spatial queries (raycasts, overlaps, other
         * controllers' shortlists) see the new AABB. Pose-only: the lazy
         * escape check keeps the query cache valid for sub-margin moves. */
        body3d_touch_broadphase_moved(body);
    }
}

/// @brief `Character3D.set_StepHeight(h)` — max obstacle height the controller can step over.
/// @param o Character3D handle to modify.
/// @param h Requested non-negative step height in world units.
void rt_character3d_set_step_height(void *o, double h) {
    rt_character3d *c = character3d_checked(o);
    if (c)
        c->step_height = character3d_sanitize_step_height(h);
}

/// @brief `Character3D.GetStepHeight` — read the configured step height.
/// @param o Character3D handle to inspect.
/// @return Sanitized step height, or the default 0.3 for an invalid handle.
double rt_character3d_get_step_height(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c ? character3d_sanitize_step_height(c->step_height) : 0.3;
}

/// @brief `Character3D.SetSlopeLimit(degrees)` — max walkable slope angle.
///
/// Stored as `cos(angle)` to make the per-step "is this surface walkable"
/// test a single comparison (no trig in the hot path).
/// @param o Character3D handle to modify.
/// @param degrees Requested walkable angle, clamped below 90 degrees.
void rt_character3d_set_slope_limit(void *o, double degrees) {
    rt_character3d *c = character3d_checked(o);
    if (c) {
        degrees = ph3d_finite_or(degrees, 45.0);
        degrees = clampd(degrees, 0.0, 89.9);
        c->slope_limit_cos = cos(degrees * 3.14159265358979323846 / 180.0);
        if (!isfinite(c->slope_limit_cos))
            c->slope_limit_cos = cos(45.0 * 3.14159265358979323846 / 180.0);
    }
}

/// @brief `Character3D.set_World(world)` — bind the character to a physics world.
///
/// Required before `Move` will collide against anything. Releases any
/// previous world reference and retains the new one. NULL detaches.
/// @param o Character3D handle to modify.
/// @param world World3D handle to retain, or NULL to detach.
void rt_character3d_set_world(void *o, void *world) {
    rt_character3d *ctrl = character3d_checked(o);
    rt_world3d *w = world3d_checked(world);
    if (!ctrl)
        return;
    if (world && !w)
        return;
    if (ctrl->world == w)
        return;
    if (w)
        rt_obj_retain_maybe(w);
    if (ctrl->world && rt_obj_release_check0(ctrl->world))
        rt_obj_free(ctrl->world);
    ctrl->world = w;
}

/// @brief `Character3D.GetWorld` — borrowed reference to the bound world.
/// @param o Character3D handle to inspect.
/// @return Borrowed World3D handle, or NULL when unbound or invalid.
void *rt_character3d_get_world(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c ? c->world : NULL;
}

/// @brief `Character3D.IsGrounded` — true when standing on a walkable surface.
/// @param o Character3D handle to inspect.
/// @return One when grounded on a walkable surface, otherwise zero.
int8_t rt_character3d_is_grounded(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c && c->is_grounded ? 1 : 0;
}

/// @brief `Character3D.JustLanded` — edge-detect: true on the first frame after landing.
///
/// Compares this frame's grounded state to the previous frame's. Useful
/// for landing animations, fall-damage triggers, dust puffs, etc.
/// @param o Character3D handle to inspect.
/// @return One for the first grounded frame after being airborne, otherwise zero.
int8_t rt_character3d_just_landed(void *o) {
    rt_character3d *c = character3d_checked(o);
    if (!c)
        return 0;
    return c->is_grounded && !c->was_grounded ? 1 : 0;
}

/// @brief `Character3D.GetPosition` — fresh `Vec3` of the body's position.
/// @param o Character3D handle to inspect.
/// @return Newly allocated world-position Vec3, or the origin when invalid.
void *rt_character3d_get_position(void *o) {
    rt_character3d *c = character3d_checked(o);
    if (!c)
        return rt_vec3_new(0, 0, 0);
    return rt_body3d_get_position(c->body);
}

/// @brief `Character3D.SetPosition(x, y, z)` — teleport the controller.
///
/// Direct delegation to the underlying body. Caller is responsible for
/// avoiding teleports into geometry.
/// @param o Character3D handle to reposition.
/// @param x Finite world-space X coordinate.
/// @param y Finite world-space Y coordinate.
/// @param z Finite world-space Z coordinate.
void rt_character3d_set_position(void *o, double x, double y, double z) {
    rt_character3d *c = character3d_checked(o);
    if (c)
        rt_body3d_set_position(c->body, x, y, z);
}

/// @brief `Character3D.TrySetHeight(h)` — crouch/stand capsule resize.
///
/// Shrinking always succeeds and keeps the feet planted (the capsule center
/// drops by half the height delta). Growing first tests the stand pose with
/// the enlarged capsule and fails (returns 0) when blocked — `TryStand`
/// semantics come free. The capsule bounds revision is bumped so the
/// broadphase re-inserts the body.
/// @param o Character3D handle to resize.
/// @param height Requested positive total capsule height.
/// @return One when the resized capsule fits and is committed, otherwise zero.
int8_t rt_character3d_try_set_height(void *o, double height) {
    rt_character3d *c = character3d_checked(o);
    if (!c || !c->body || !c->body->collider)
        return 0;
    if (!isfinite(height) || height <= 0.0)
        return 0;
    double radius =
        c->body->radius > 0.0 ? c->body->radius : rt_collider3d_get_radius_raw(c->body->collider);
    if (!isfinite(radius) || radius <= 0.0 || radius > CHARACTER3D_HEIGHT_MAX * 0.5)
        return 0;
    if (height > CHARACTER3D_HEIGHT_MAX)
        height = CHARACTER3D_HEIGHT_MAX;
    if (height < radius * 2.0)
        height = radius * 2.0;
    double old_height = rt_collider3d_get_height_raw(c->body->collider);
    if (old_height <= 0.0)
        old_height = c->body->height;
    if (!isfinite(old_height) || old_height <= 0.0)
        return 0;
    if (fabs(height - old_height) < 1e-12)
        return 1;
    /* Feet stay planted: center shifts by half the height delta. */
    double new_center_y =
        character3d_saturate_coord(c->body->position[1] + (height - old_height) * 0.5);
    if (height < old_height) {
        rt_collider3d_reset_capsule_raw(c->body->collider, radius, height);
        body3d_update_shape_cache_from_collider(c->body);
        c->body->position[1] = new_center_y;
        body3d_touch_broadphase(c->body);
        return 1;
    }
    /* Growing: probe the stand pose (lifted 1 cm so resting ground contact
     * does not read as a blocker) before committing. */
    rt_collider3d_reset_capsule_raw(c->body->collider, radius, height);
    body3d_update_shape_cache_from_collider(c->body);
    {
        rt_character_hit3d hit;
        double stand_pos[3] = {c->body->position[0], new_center_y + 0.01, c->body->position[2]};
        if (character3d_test_position(c, stand_pos, &hit)) {
            rt_collider3d_reset_capsule_raw(c->body->collider, radius, old_height);
            body3d_update_shape_cache_from_collider(c->body);
            return 0;
        }
    }
    c->body->position[1] = new_center_y;
    body3d_touch_broadphase(c->body);
    return 1;
}

/// @brief `Character3D.set_Height(h)` — property form of TrySetHeight (result ignored).
/// @param o Character3D handle to resize.
/// @param height Requested positive total capsule height.
void rt_character3d_set_height(void *o, double height) {
    (void)rt_character3d_try_set_height(o, height);
}

/// @brief `Character3D.get_Height` — current capsule height including caps.
/// @param o Character3D handle to inspect.
/// @return Current total capsule height, or zero when invalid.
double rt_character3d_get_height(void *o) {
    rt_character3d *c = character3d_checked(o);
    if (!c || !c->body)
        return 0.0;
    double height = c->body->collider ? rt_collider3d_get_height_raw(c->body->collider) : 0.0;
    if (!isfinite(height) || height <= 0.0)
        height = c->body->height;
    if (!isfinite(height) || height <= 0.0)
        return 0.0;
    return height > CHARACTER3D_HEIGHT_MAX ? CHARACTER3D_HEIGHT_MAX : height;
}

/// @brief `Character3D.set_PushStrength(s)` — dynamic push impulse scale (0 = block only).
/// @param o Character3D handle to modify.
/// @param strength Requested non-negative push scale, capped at 1000.
void rt_character3d_set_push_strength(void *o, double strength) {
    rt_character3d *c = character3d_checked(o);
    if (c)
        c->push_strength =
            (isfinite(strength) && strength > 0.0) ? clampd(strength, 0.0, 1000.0) : 0.0;
}

/// @brief `Character3D.get_PushStrength` — dynamic push impulse scale.
/// @param o Character3D handle to inspect.
/// @return Configured dynamic-body push scale, or zero when invalid.
double rt_character3d_get_push_strength(void *o) {
    rt_character3d *c = character3d_checked(o);
    if (!c || !isfinite(c->push_strength) || c->push_strength <= 0.0)
        return 0.0;
    return c->push_strength > 1000.0 ? 1000.0 : c->push_strength;
}

/// @brief `Character3D.set_CollideDynamic(on)` — dynamic bodies block/push (default)
///   or ghost through (legacy compatibility).
/// @param o Character3D handle to modify.
/// @param enabled Nonzero to collide with dynamic bodies; zero to ignore them.
void rt_character3d_set_collide_dynamic(void *o, int8_t enabled) {
    rt_character3d *c = character3d_checked(o);
    if (c)
        c->collide_dynamic = enabled ? 1 : 0;
}

/// @brief `Character3D.get_CollideDynamic` — whether dynamic bodies block the controller.
/// @param o Character3D handle to inspect.
/// @return One when dynamic bodies are blockers, otherwise zero.
int8_t rt_character3d_get_collide_dynamic(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c && c->collide_dynamic ? 1 : 0;
}

/// @brief `Character3D.set_RidePlatforms(on)` — track kinematic ground motion.
/// @param o Character3D handle to modify.
/// @param enabled Nonzero to inherit supporting-platform motion; zero to disable it.
void rt_character3d_set_ride_platforms(void *o, int8_t enabled) {
    rt_character3d *c = character3d_checked(o);
    if (c)
        c->ride_platforms = enabled ? 1 : 0;
}

/// @brief `Character3D.get_RidePlatforms` — whether the controller rides platforms.
/// @param o Character3D handle to inspect.
/// @return One when platform riding is enabled, otherwise zero.
int8_t rt_character3d_get_ride_platforms(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c && c->ride_platforms ? 1 : 0;
}

/// @brief `Character3D.IsSliding` — true while resting on a too-steep surface.
/// @param o Character3D handle to inspect.
/// @return One while resting against an unwalkable slope, otherwise zero.
int8_t rt_character3d_is_sliding(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c && c->is_sliding ? 1 : 0;
}

/// @brief `Character3D.GetGroundBody` — borrowed body under the controller's feet
///   (NULL while airborne). Gameplay uses it for conveyors and surface queries.
/// @param o Character3D handle to inspect.
/// @return Borrowed supporting Body3D handle, or NULL while airborne or invalid.
void *rt_character3d_get_ground_body(void *o) {
    rt_character3d *c = character3d_checked(o);
    return c ? c->ground_body : NULL;
}

/*==========================================================================
 * Trigger3D — standalone AABB zone with enter/exit edge detection
 *=========================================================================*/

typedef struct {
    void *vptr;
    double bounds_min[3];
    double bounds_max[3];
    /* Tracked set = bodies inside the trigger (plus this frame's transients).
     * Grown on demand — no fixed cap — and parallel-indexed. Body pointers are
     * weak: the trigger only observes, and Update prunes stale entries. */
    void **tracked_bodies;
    int8_t *was_inside;
    int8_t *is_inside;
    uint32_t *seen_stamp;
    int32_t tracked_count;
    int32_t tracked_capacity;
    uint32_t update_stamp;
    int32_t enter_count;
    int32_t exit_count;
} rt_trigger3d;

/// @brief Validate @p obj as a Trigger3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Runtime object handle to validate.
/// @return Typed Trigger3D payload, or NULL for a class mismatch.
static rt_trigger3d *trigger3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_TRIGGER3D_CLASS_ID, sizeof(rt_trigger3d))
               ? (rt_trigger3d *)obj
               : NULL;
}

/// @brief Release the temporary strong reference returned by `rt_weak_load`.
/// @param body Managed Body3D handle, or NULL.
static void trigger3d_release_loaded_body(void *body) {
    if (body && rt_obj_release_check0(body))
        rt_obj_free(body);
}

/// @brief Clear and destroy one zeroing-weak body slot.
/// @param slot Address of a tracked weak-handle slot.
static void trigger3d_clear_weak_body_slot(void **slot) {
    if (slot)
        rt_weak_store(slot, NULL);
}

/// @brief Increment an edge counter without signed overflow.
/// @param count Enter/exit counter to increment.
static void trigger3d_increment_edge_count(int32_t *count) {
    if (count && *count < INT32_MAX)
        (*count)++;
}

/// @brief Normalize a trigger's parallel occupancy arrays before iteration.
/// @details If any allocation is missing, the tuple is discarded atomically;
///   surviving weak handles are cleared before their pointer array is freed.
/// @param t Trigger whose tracking metadata is repaired.
static void trigger3d_repair_tracking(rt_trigger3d *t) {
    int32_t safe_count;
    if (!t)
        return;
    if (t->tracked_capacity < 0 || !t->tracked_bodies || !t->was_inside || !t->is_inside ||
        !t->seen_stamp) {
        safe_count = t->tracked_bodies && t->tracked_capacity > 0 && t->tracked_count > 0
                         ? t->tracked_count
                         : 0;
        if (safe_count > t->tracked_capacity)
            safe_count = t->tracked_capacity;
        for (int32_t i = 0; i < safe_count; ++i)
            trigger3d_clear_weak_body_slot(&t->tracked_bodies[i]);
        free(t->tracked_bodies);
        free(t->was_inside);
        free(t->is_inside);
        free(t->seen_stamp);
        t->tracked_bodies = NULL;
        t->was_inside = NULL;
        t->is_inside = NULL;
        t->seen_stamp = NULL;
        t->tracked_count = 0;
        t->tracked_capacity = 0;
        return;
    }
    if (t->tracked_count < 0)
        t->tracked_count = 0;
    if (t->tracked_count > t->tracked_capacity)
        t->tracked_count = t->tracked_capacity;
}

/// @brief GC finalizer for `Trigger3D` — releases the tracking arrays.
///
/// Tracked bodies use zeroing weak handles: the trigger remains an observer,
/// and body destruction cannot leave an address-reuse alias in occupancy state.
/// @param obj Trigger3D payload whose tracking buffers are released.
static void trigger3d_finalizer(void *obj) {
    rt_trigger3d *t = (rt_trigger3d *)obj;
    if (!t)
        return;
    trigger3d_repair_tracking(t);
    for (int32_t i = 0; i < t->tracked_count; ++i)
        trigger3d_clear_weak_body_slot(&t->tracked_bodies[i]);
    free(t->tracked_bodies);
    free(t->was_inside);
    free(t->is_inside);
    free(t->seen_stamp);
    t->tracked_bodies = NULL;
    t->was_inside = NULL;
    t->is_inside = NULL;
    t->seen_stamp = NULL;
    t->tracked_count = 0;
    t->tracked_capacity = 0;
}

/// @brief Store ordered, finite trigger bounds.
/// @param t Trigger3D payload to modify.
/// @param x0 First corner X coordinate.
/// @param y0 First corner Y coordinate.
/// @param z0 First corner Z coordinate.
/// @param x1 Opposite corner X coordinate.
/// @param y1 Opposite corner Y coordinate.
/// @param z1 Opposite corner Z coordinate.
static void trigger3d_set_bounds_raw(
    rt_trigger3d *t, double x0, double y0, double z0, double x1, double y1, double z1) {
    double a[3] = {character3d_saturate_coord(x0),
                   character3d_saturate_coord(y0),
                   character3d_saturate_coord(z0)};
    double b[3] = {character3d_saturate_coord(x1),
                   character3d_saturate_coord(y1),
                   character3d_saturate_coord(z1)};
    if (!t)
        return;
    t->bounds_min[0] = a[0] < b[0] ? a[0] : b[0];
    t->bounds_min[1] = a[1] < b[1] ? a[1] : b[1];
    t->bounds_min[2] = a[2] < b[2] ? a[2] : b[2];
    t->bounds_max[0] = a[0] > b[0] ? a[0] : b[0];
    t->bounds_max[1] = a[1] > b[1] ? a[1] : b[1];
    t->bounds_max[2] = a[2] > b[2] ? a[2] : b[2];
}

/// @brief `Trigger3D.New(x0, y0, z0, x1, y1, z1)` — make an axis-aligned trigger zone.
///
/// Auto-orders the corners so caller can pass them in any order. Occupancy
/// tracking grows on demand — any number of bodies can be inside at once.
/// @param x0 First corner X coordinate.
/// @param y0 First corner Y coordinate.
/// @param z0 First corner Z coordinate.
/// @param x1 Opposite corner X coordinate.
/// @param y1 Opposite corner Y coordinate.
/// @param z1 Opposite corner Z coordinate.
/// @return Newly allocated Trigger3D handle, or NULL on allocation failure.
void *rt_trigger3d_new(double x0, double y0, double z0, double x1, double y1, double z1) {
    rt_trigger3d *t =
        (rt_trigger3d *)rt_obj_new_i64(RT_G3D_TRIGGER3D_CLASS_ID, (int64_t)sizeof(rt_trigger3d));
    if (!t) {
        rt_trap("Trigger3D.New: allocation failed");
        return NULL;
    }
    {
        rt_trigger3d zero = {0};
        *t = zero;
    }
    trigger3d_set_bounds_raw(t, x0, y0, z0, x1, y1, z1);
    rt_obj_set_finalizer(t, trigger3d_finalizer);
    return t;
}

/// @brief `Trigger3D.Contains(point)` — point-in-AABB test for a `Vec3`.
///
/// Synchronous query; doesn't update enter/exit state. Use this for
/// ad-hoc "is the player in the safe zone" checks; use `Update` +
/// `EnterCount`/`ExitCount` for transition-based logic.
/// @param obj Trigger3D handle to query.
/// @param point Vec3 world-space point to test.
/// @return One when the point is within the inclusive trigger bounds, otherwise zero.
int8_t rt_trigger3d_contains(void *obj, void *point) {
    rt_trigger3d *t = trigger3d_checked(obj);
    if (!t || !rt_g3d_is_vec3(point))
        return 0;
    trigger3d_set_bounds_raw(t,
                             t->bounds_min[0],
                             t->bounds_min[1],
                             t->bounds_min[2],
                             t->bounds_max[0],
                             t->bounds_max[1],
                             t->bounds_max[2]);
    double px = rt_vec3_x(point), py = rt_vec3_y(point), pz = rt_vec3_z(point);
    if (!isfinite(px) || !isfinite(py) || !isfinite(pz))
        return 0;
    px = character3d_saturate_coord(px);
    py = character3d_saturate_coord(py);
    pz = character3d_saturate_coord(pz);
    return (px >= t->bounds_min[0] && px <= t->bounds_max[0] && py >= t->bounds_min[1] &&
            py <= t->bounds_max[1] && pz >= t->bounds_min[2] && pz <= t->bounds_max[2])
               ? 1
               : 0;
}

/// @brief Find a tracked body's slot, or -1 when it is not tracked.
/// @param t Trigger3D payload whose occupancy table is searched.
/// @param body Exact weak Body3D pointer to find.
/// @return Zero-based tracked slot, or -1 when absent.
static int32_t trigger3d_find_index(rt_trigger3d *t, const void *body) {
    if (!t || !body)
        return -1;
    trigger3d_repair_tracking(t);
    for (int32_t i = 0; i < t->tracked_count; i++) {
        void *tracked = rt_weak_load(&t->tracked_bodies[i]);
        int matches = tracked == body;
        trigger3d_release_loaded_body(tracked);
        if (matches)
            return i;
    }
    return -1;
}

/// @brief Claim a new tracked slot, growing the parallel arrays on demand.
/// @param t Trigger3D payload whose tracking arrays may grow.
/// @param body Weak Body3D pointer stored in the new slot.
/// @return Slot index, or -1 on allocation failure (the body is skipped this
///   frame and retried on the next Update — graceful degradation, no cap).
static int32_t trigger3d_add(rt_trigger3d *t, void *body) {
    if (!t || !body)
        return -1;
    trigger3d_repair_tracking(t);
    if (t->tracked_count >= INT32_MAX)
        return -1;
    if (t->tracked_count >= t->tracked_capacity) {
        int32_t new_cap;
        void **grown_bodies = NULL;
        int8_t *grown_was = NULL;
        int8_t *grown_is = NULL;
        uint32_t *grown_seen = NULL;
        if (t->tracked_capacity > INT32_MAX / 2)
            return -1;
        new_cap = t->tracked_capacity == 0 ? 16 : t->tracked_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*grown_bodies) ||
            (size_t)new_cap > SIZE_MAX / sizeof(*grown_was) ||
            (size_t)new_cap > SIZE_MAX / sizeof(*grown_is) ||
            (size_t)new_cap > SIZE_MAX / sizeof(*grown_seen))
            return -1;
        grown_bodies = (void **)calloc((size_t)new_cap, sizeof(*grown_bodies));
        grown_was = (int8_t *)calloc((size_t)new_cap, sizeof(*grown_was));
        grown_is = (int8_t *)calloc((size_t)new_cap, sizeof(*grown_is));
        grown_seen = (uint32_t *)calloc((size_t)new_cap, sizeof(*grown_seen));
        if (!grown_bodies || !grown_was || !grown_is || !grown_seen) {
            free(grown_bodies);
            free(grown_was);
            free(grown_is);
            free(grown_seen);
            return -1;
        }
        if (t->tracked_count > 0) {
            memcpy(
                grown_bodies, t->tracked_bodies, (size_t)t->tracked_count * sizeof(*grown_bodies));
            memcpy(grown_was, t->was_inside, (size_t)t->tracked_count * sizeof(*grown_was));
            memcpy(grown_is, t->is_inside, (size_t)t->tracked_count * sizeof(*grown_is));
            memcpy(grown_seen, t->seen_stamp, (size_t)t->tracked_count * sizeof(*grown_seen));
        }
        free(t->tracked_bodies);
        free(t->was_inside);
        free(t->is_inside);
        free(t->seen_stamp);
        t->tracked_bodies = grown_bodies;
        t->was_inside = grown_was;
        t->is_inside = grown_is;
        t->seen_stamp = grown_seen;
        t->tracked_capacity = new_cap;
    }
    {
        int32_t idx = t->tracked_count;
        void *loaded;
        rt_weak_store(&t->tracked_bodies[idx], body);
        loaded = rt_weak_load(&t->tracked_bodies[idx]);
        if (loaded != body) {
            trigger3d_release_loaded_body(loaded);
            trigger3d_clear_weak_body_slot(&t->tracked_bodies[idx]);
            return -1;
        }
        trigger3d_release_loaded_body(loaded);
        t->was_inside[idx] = 0;
        t->is_inside[idx] = 0;
        t->seen_stamp[idx] = 0;
        t->tracked_count = idx + 1;
        return idx;
    }
}

/// @brief Swap-remove a tracked slot (order is not meaningful).
/// @param t Trigger3D payload whose parallel arrays are compacted.
/// @param i Zero-based valid slot to remove.
static void trigger3d_remove_at(rt_trigger3d *t, int32_t i) {
    if (!t)
        return;
    trigger3d_repair_tracking(t);
    if (i < 0 || i >= t->tracked_count)
        return;
    int32_t last = t->tracked_count - 1;
    trigger3d_clear_weak_body_slot(&t->tracked_bodies[i]);
    if (i != last) {
        t->tracked_bodies[i] = t->tracked_bodies[last];
        t->was_inside[i] = t->was_inside[last];
        t->is_inside[i] = t->is_inside[last];
        t->seen_stamp[i] = t->seen_stamp[last];
        t->tracked_bodies[last] = NULL;
    }
    t->was_inside[last] = 0;
    t->is_inside[last] = 0;
    t->seen_stamp[last] = 0;
    t->tracked_count = last;
}

/// @brief `Trigger3D.Update(world)` — recompute occupancy and edge counts.
///
/// Tests every body's world AABB (not just its center) against the trigger
/// box, so large bodies straddling the boundary register correctly. Only
/// bodies currently inside (or leaving this frame) are tracked, so the
/// tracked set stays small and has no fixed cap. Diffs current frame vs.
/// previous to produce `enter_count` and `exit_count` totals — no per-body
/// events are stored, so callers learn "how many entered" but not "which".
/// Run once per frame after `World3D.Step`.
/// @param obj Trigger3D handle whose occupancy history is advanced.
/// @param world_obj World3D handle containing bodies to test.
void rt_trigger3d_update(void *obj, void *world_obj) {
    rt_trigger3d *t = trigger3d_checked(obj);
    rt_world3d *w = world3d_checked(world_obj);
    if (!t || !w)
        return;
    trigger3d_repair_tracking(t);
    trigger3d_set_bounds_raw(t,
                             t->bounds_min[0],
                             t->bounds_min[1],
                             t->bounds_min[2],
                             t->bounds_max[0],
                             t->bounds_max[1],
                             t->bounds_max[2]);

    /* Advance the seen stamp; on wrap, reset every slot's stamp so no stale
     * slot can alias the new epoch. */
    t->update_stamp++;
    if (t->update_stamp == 0) {
        for (int32_t i = 0; i < t->tracked_count; i++)
            t->seen_stamp[i] = 0;
        t->update_stamp = 1;
    }

    /* Swap current → previous */
    for (int32_t i = 0; i < t->tracked_count; i++) {
        t->was_inside[i] = t->is_inside[i] ? 1 : 0;
        t->is_inside[i] = 0;
    }
    t->enter_count = 0;
    t->exit_count = 0;

    int32_t body_count = 0;
    if (w->bodies && w->body_capacity > 0 && w->body_count > 0)
        body_count = w->body_count > w->body_capacity ? w->body_capacity : w->body_count;
    for (int32_t i = 0; i < body_count; i++) {
        rt_body3d *b = w->bodies[i];
        double bmn[3];
        double bmx[3];
        int8_t inside;
        int32_t idx;
        if (!b)
            continue;

        /* Body AABB vs trigger AABB. */
        body_aabb(b, bmn, bmx);
        inside = (bmx[0] >= t->bounds_min[0] && bmn[0] <= t->bounds_max[0] &&
                  bmx[1] >= t->bounds_min[1] && bmn[1] <= t->bounds_max[1] &&
                  bmx[2] >= t->bounds_min[2] && bmn[2] <= t->bounds_max[2])
                     ? 1
                     : 0;

        idx = trigger3d_find_index(t, b);
        if (idx < 0) {
            if (!inside)
                continue; /* untracked and outside: nothing to observe */
            idx = trigger3d_add(t, b);
            if (idx < 0)
                continue; /* allocation failure: retry next frame */
        }

        t->seen_stamp[idx] = t->update_stamp;
        t->is_inside[idx] = inside;
        if (inside && !t->was_inside[idx])
            trigger3d_increment_edge_count(&t->enter_count);
        if (!inside && t->was_inside[idx])
            trigger3d_increment_edge_count(&t->exit_count);
    }

    /* Prune: bodies that left the world (unseen) fire exit if they were
     * inside; bodies observed outside already fired exit above. Either way
     * the slot is dropped — the tracked set holds only current occupants. */
    for (int32_t i = 0; i < t->tracked_count;) {
        if (t->seen_stamp[i] == t->update_stamp && t->is_inside[i]) {
            i++;
            continue;
        }
        if (t->seen_stamp[i] != t->update_stamp && t->was_inside[i])
            trigger3d_increment_edge_count(&t->exit_count);
        trigger3d_remove_at(t, i);
    }
}

/// @brief `Trigger3D.EnterCount` — bodies that entered this trigger this frame.
/// @param obj Trigger3D handle to inspect.
/// @return Number of bodies newly inside during the latest update.
int64_t rt_trigger3d_get_enter_count(void *obj) {
    rt_trigger3d *t = trigger3d_checked(obj);
    return t && t->enter_count > 0 ? t->enter_count : 0;
}

/// @brief `Trigger3D.ExitCount` — bodies that left this trigger this frame.
/// @param obj Trigger3D handle to inspect.
/// @return Number of bodies that left during the latest update.
int64_t rt_trigger3d_get_exit_count(void *obj) {
    rt_trigger3d *t = trigger3d_checked(obj);
    return t && t->exit_count > 0 ? t->exit_count : 0;
}

/// @brief `Trigger3D.SetBounds(x0..z1)` — replace the trigger's AABB.
///
/// Auto-orders the corners. Tracked-body state is preserved across the
/// resize, so a body that was inside the old box and is also inside
/// the new box remains "in" without firing an enter event.
/// @param obj Trigger3D handle to modify.
/// @param x0 First corner X coordinate.
/// @param y0 First corner Y coordinate.
/// @param z0 First corner Z coordinate.
/// @param x1 Opposite corner X coordinate.
/// @param y1 Opposite corner Y coordinate.
/// @param z1 Opposite corner Z coordinate.
void rt_trigger3d_set_bounds(
    void *obj, double x0, double y0, double z0, double x1, double y1, double z1) {
    rt_trigger3d *t = trigger3d_checked(obj);
    if (!t)
        return;
    trigger3d_set_bounds_raw(t, x0, y0, z0, x1, y1, z1);
}

#else
typedef int rt_physics3d_character_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
