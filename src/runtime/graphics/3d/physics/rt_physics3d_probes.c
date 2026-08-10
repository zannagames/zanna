//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_probes.c
// Purpose: Traversal shape-cast probes for Physics3DWorld — ProbeLedge,
//   ProbeVault, and ProbeClearance — plus the LedgeHit3D result class. The
//   probes compose the existing capsule/sphere sweeps and a scratch-capsule
//   overlap test so climb/mantle/vault gameplay gets structured results
//   (grab point, surface normal, standing room, landing point) instead of
//   hand-rolled raycast forests.
// Key invariants:
//   - Probes are pure queries: no body is registered in the world and no
//     simulation state is mutated.
//   - Result vectors are world-space snapshots at probe time, not live handles.
//   - Ledge tops must be walkable-ish (normal.y >= 0.6) or the probe fails.
// Ownership/Lifetime:
//   - LedgeHit3D is a GC-managed plain result handle (PhysicsHit3D pattern);
//     it retains nothing.
// Links: rt_physics3d.h, rt_physics3d_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_probes.c
 * @brief Implements clearance, ledge, vault, and raw collider traversal probes.
 *
 * Probes compose existing narrow-phase and sweep queries without registering
 * scratch bodies or mutating simulation state. Results are GC-managed
 * world-space snapshots and retain no physics objects.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_physics3d_query_internal.h"
#include "rt_trap.h"

#include <math.h>
#include <string.h>

/// @brief Walkable-ish surface-normal floor for ledge tops and vault landings.
#define PH3D_PROBE_WALKABLE_NORMAL_Y 0.6
/// @brief Back-off epsilon between probe shapes and geometry.
#define PH3D_PROBE_SKIN 0.02
/// @brief Vault landings may be up to this far below the probe origin.
#define PH3D_PROBE_VAULT_DROP_TOLERANCE 2.0
/// @brief Vault landings must be near origin level (rejects thick-wall tops).
#define PH3D_PROBE_VAULT_LANDING_MAX_RISE 0.3

/// @brief LedgeHit3D payload: grab/landing points, surface + wall normals,
///   ledge height, and the standing-room/landing flags.
typedef struct {
    void *vptr;
    double grab_point[3];
    double surface_normal[3];
    double wall_normal[3];
    double landing_point[3];
    double height;
    int8_t has_standing_room;
    int8_t has_landing;
} rt_ledge_hit3d_obj;

/// @brief Validate @p obj as a LedgeHit3D handle (NULL on mismatch).
/// @param obj Runtime object handle.
/// @return Typed result payload, or NULL on mismatch.
static rt_ledge_hit3d_obj *ledge_hit3d_checked(void *obj) {
    return (rt_ledge_hit3d_obj *)rt_g3d_checked_or_null(obj, RT_G3D_LEDGEHIT3D_CLASS_ID);
}

/// @brief Allocate a zeroed LedgeHit3D result handle.
/// @param api_name Exact allocation-failure trap text for the owning probe.
/// @return Newly allocated result, or NULL on failure.
static rt_ledge_hit3d_obj *ledge_hit3d_new(const char *api_name) {
    rt_ledge_hit3d_obj *hit = (rt_ledge_hit3d_obj *)rt_obj_new_i64(
        RT_G3D_LEDGEHIT3D_CLASS_ID, (int64_t)sizeof(rt_ledge_hit3d_obj));
    if (!hit) {
        rt_trap(api_name ? api_name : "Physics3DWorld traversal probe: allocation failed");
        return NULL;
    }
    memset(hit, 0, sizeof(*hit));
    return hit;
}

/// @brief Box a bounded result position from potentially corrupt retained lanes.
/// @param value Borrowed three-lane retained vector.
/// @return Newly allocated finite Vec3, or `NULL` on allocation failure.
static void *ledge_hit3d_position_result(const double value[3]) {
    return value ? rt_vec3_new(query_saturate_coord(value[0]),
                               query_saturate_coord(value[1]),
                               query_saturate_coord(value[2]))
                 : NULL;
}

/// @brief Box a normalized result normal from potentially corrupt retained lanes.
/// @param value Borrowed three-lane retained normal.
/// @return Newly allocated unit Vec3, using world up for unusable input.
static void *ledge_hit3d_normal_result(const double value[3]) {
    double normal[3] = {0.0, 1.0, 0.0};
    if (value)
        memcpy(normal, value, sizeof(normal));
    query_normalize_normal(normal, NULL);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `LedgeHit3D.GrabPoint` — world-space ledge edge point.
/// @param obj LedgeHit3D handle.
/// @return Newly allocated grab-point Vec3, or NULL when invalid.
void *rt_ledge_hit3d_get_grab_point(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    if (!hit)
        return NULL;
    return ledge_hit3d_position_result(hit->grab_point);
}

/// @brief `LedgeHit3D.SurfaceNormal` — ledge top surface normal.
/// @param obj LedgeHit3D handle.
/// @return Newly allocated top-normal Vec3, or NULL when invalid.
void *rt_ledge_hit3d_get_surface_normal(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    if (!hit)
        return NULL;
    return ledge_hit3d_normal_result(hit->surface_normal);
}

/// @brief `LedgeHit3D.WallNormal` — front wall surface normal.
/// @param obj LedgeHit3D handle.
/// @return Newly allocated wall-normal Vec3, or NULL when invalid.
void *rt_ledge_hit3d_get_wall_normal(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    if (!hit)
        return NULL;
    return ledge_hit3d_normal_result(hit->wall_normal);
}

/// @brief `LedgeHit3D.LandingPoint` — far-side landing (vault only; NULL otherwise).
/// @param obj LedgeHit3D handle.
/// @return Newly allocated landing Vec3, or NULL when absent.
void *rt_ledge_hit3d_get_landing_point(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    if (!hit || !hit->has_landing)
        return NULL;
    return ledge_hit3d_position_result(hit->landing_point);
}

/// @brief `LedgeHit3D.Height` — ledge top height above the probe origin.
/// @param obj LedgeHit3D handle.
/// @return Measured ledge rise, or zero when invalid.
double rt_ledge_hit3d_get_height(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    return hit ? query_sanitize_distance(hit->height) : 0.0;
}

/// @brief `LedgeHit3D.HasStandingRoom` — capsule clearance existed at the top.
/// @param obj LedgeHit3D handle.
/// @return One when standing clearance was verified, otherwise zero.
int8_t rt_ledge_hit3d_get_has_standing_room(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    return hit && hit->has_standing_room ? 1 : 0;
}

/// @brief `LedgeHit3D.HasLanding` — a far-side vault landing was found.
/// @param obj LedgeHit3D handle.
/// @return One when landing data is present, otherwise zero.
int8_t rt_ledge_hit3d_get_has_landing(void *obj) {
    rt_ledge_hit3d_obj *hit = ledge_hit3d_checked(obj);
    return hit && hit->has_landing ? 1 : 0;
}

//=========================================================================
// Probe internals
//=========================================================================

/// @brief Read a Vec3 parameter into @p out; returns 0 (and traps) on wrong type.
/// @param vec Vec3 runtime handle.
/// @param out Three-component sanitized output.
/// @param api_name Diagnostic text used on type mismatch.
/// @return One on success, otherwise zero.
static int probe_read_vec3(void *vec, double out[3], const char *api_name) {
    if (!rt_g3d_is_vec3(vec)) {
        rt_trap(api_name);
        return 0;
    }
    return query_read_vec3(vec, out);
}

/// @brief Initialize one borrowed-collider stack body for a pure narrow-phase query.
/// @param body Writable stack body.
/// @param collider Borrowed validated Collider3D handle.
/// @param position Borrowed finite bounded world position.
/// @param orientation Borrowed normalized XYZW orientation, or `NULL` for identity.
static void probe_init_stack_body(rt_body3d *body,
                                  void *collider,
                                  const double position[3],
                                  const double orientation[4]) {
    if (!body)
        return;
    memset(body, 0, sizeof(*body));
    body->motion_mode = PH3D_MODE_STATIC;
    body->collider = collider;
    body->scale[0] = body->scale[1] = body->scale[2] = 1.0;
    if (position)
        memcpy(body->position, position, sizeof(body->position));
    if (orientation)
        memcpy(body->orientation, orientation, sizeof(body->orientation));
    else
        body->orientation[3] = 1.0;
    body3d_update_shape_cache_from_collider(body);
}

/// @brief Capsule-overlap test at an explicit pose against every masked world
///   body, using a scratch (unregistered) capsule body and the standard
///   narrow-phase. Returns 1 when any solid overlap exists.
/// @param world World3D to query.
/// @param pos Capsule center position.
/// @param radius Capsule radius.
/// @param height Total capsule height.
/// @param mask Query layer mask.
/// @return One for any solid overlap, otherwise zero.
static int probe_capsule_overlaps(
    rt_world3d *world, const double pos[3], double radius, double height, int64_t mask) {
    int32_t body_count;
    void *collider;
    rt_body3d scratch;
    int overlapping = 0;
    if (!world || !pos || !ph3d_vec3_all_finite(pos))
        return 0;
    collider = rt_collider3d_new_capsule(radius, height);
    if (!collider)
        return 0;
    probe_init_stack_body(&scratch, collider, pos, NULL);
    body_count =
        world->bodies && world->body_count > 0 && world->body_capacity > 0
            ? (world->body_count < world->body_capacity ? world->body_count : world->body_capacity)
            : 0;
    for (int32_t i = 0; i < body_count && !overlapping; ++i) {
        rt_body3d *other =
            (rt_body3d *)rt_g3d_checked_or_null(world->bodies[i], RT_G3D_BODY3D_CLASS_ID);
        double normal[3];
        double depth;
        if (!other || other->is_trigger)
            continue;
        if (!body3d_has_collision_geometry(other) || !query_mask_matches_body(other, mask))
            continue;
        if (test_collision(&scratch, other, normal, &depth, NULL, NULL, NULL, NULL, NULL) &&
            isfinite(depth) && depth > 0.0)
            overlapping = 1;
    }
    if (rt_obj_release_check0(collider))
        rt_obj_free(collider);
    return overlapping;
}

/// @brief Shared ledge steps 1-2: wall sweep + top down-sweep.
/// @param world_obj World3D runtime handle used by public sweep APIs.
/// @param world Typed World3D backing the query.
/// @param origin Foot-level world origin.
/// @param forward Requested horizontal search direction.
/// @param radius Probe capsule/sphere radius.
/// @param max_height Maximum ledge rise.
/// @param max_depth Maximum forward reach.
/// @param mask Query layer mask.
/// @param out_wall_point Wall-contact point output.
/// @param out_wall_normal Wall-normal output.
/// @param out_top_point Top-contact point output.
/// @param out_top_normal Top-normal output.
/// @return 1 on success with all out-params filled; 0 when no valid ledge.
static int probe_find_ledge_top(void *world_obj,
                                rt_world3d *world,
                                const double origin[3],
                                const double forward[3],
                                double radius,
                                double max_height,
                                double max_depth,
                                int64_t mask,
                                double out_wall_point[3],
                                double out_wall_normal[3],
                                double out_top_point[3],
                                double out_top_normal[3]) {
    (void)world;
    double fwd[3] = {forward[0], forward[1], forward[2]};
    fwd[1] = 0.0; /* traversal probes reason in the horizontal plane */
    double fwd_len = hypot(fwd[0], fwd[2]);
    if (!isfinite(fwd_len) || fwd_len <= 1e-9)
        return 0;
    fwd[0] /= fwd_len;
    fwd[2] /= fwd_len;

    /* 1. Wall sweep: character-sized capsule pushed forward. The origin is a
     * FOOT-level point; the capsule bottom is lifted by a skin so standing on
     * the ground does not read as an initial penetration. */
    double lift = radius + PH3D_PROBE_SKIN;
    double top = max_height - radius + PH3D_PROBE_SKIN;
    if (top < lift)
        top = lift;
    void *a = rt_vec3_new(origin[0], origin[1] + lift, origin[2]);
    void *b = rt_vec3_new(origin[0], origin[1] + top, origin[2]);
    void *delta = rt_vec3_new(fwd[0] * max_depth, 0.0, fwd[2] * max_depth);
    void *wall_hit = rt_world3d_sweep_capsule(world_obj, a, b, radius, delta, mask);
    int ok = 0;
    if (wall_hit && !rt_physics_hit3d_get_started_penetrating(wall_hit)) {
        void *wall_point = rt_physics_hit3d_get_point(wall_hit);
        void *wall_normal = rt_physics_hit3d_get_normal(wall_hit);
        if (wall_point && wall_normal && query_read_vec3(wall_point, out_wall_point) &&
            query_read_vec3(wall_normal, out_wall_normal)) {
            query_normalize_normal(out_wall_normal, fwd);

            /* 2. Top sweep: sphere dropped onto the candidate ledge. */
            double top_start[3] = {out_wall_point[0] + fwd[0] * (radius + PH3D_PROBE_SKIN),
                                   origin[1] + max_height + radius + PH3D_PROBE_SKIN,
                                   out_wall_point[2] + fwd[2] * (radius + PH3D_PROBE_SKIN)};
            void *top_center = rt_vec3_new(top_start[0], top_start[1], top_start[2]);
            void *down = rt_vec3_new(0.0, -(max_height + radius + PH3D_PROBE_SKIN * 2.0), 0.0);
            void *top_hit = rt_world3d_sweep_sphere(world_obj, top_center, radius, down, mask);
            if (top_hit && !rt_physics_hit3d_get_started_penetrating(top_hit)) {
                void *top_point = rt_physics_hit3d_get_point(top_hit);
                void *top_normal = rt_physics_hit3d_get_normal(top_hit);
                if (top_point && top_normal && query_read_vec3(top_point, out_top_point) &&
                    query_read_vec3(top_normal, out_top_normal)) {
                    query_normalize_normal(out_top_normal, NULL);
                    /* Ledge must actually be above the origin and within budget. */
                    double rise = out_top_point[1] - origin[1];
                    if (out_top_normal[1] >= PH3D_PROBE_WALKABLE_NORMAL_Y && isfinite(rise) &&
                        rise > PH3D_PROBE_SKIN && rise <= max_height)
                        ok = 1;
                }
                if (rt_obj_release_check0(top_point))
                    rt_obj_free(top_point);
                if (top_normal && rt_obj_release_check0(top_normal))
                    rt_obj_free(top_normal);
            }
            if (top_hit && rt_obj_release_check0(top_hit))
                rt_obj_free(top_hit);
            if (rt_obj_release_check0(down))
                rt_obj_free(down);
            if (rt_obj_release_check0(top_center))
                rt_obj_free(top_center);
        }
        if (wall_point && rt_obj_release_check0(wall_point))
            rt_obj_free(wall_point);
        if (wall_normal && rt_obj_release_check0(wall_normal))
            rt_obj_free(wall_normal);
    }
    if (wall_hit && rt_obj_release_check0(wall_hit))
        rt_obj_free(wall_hit);
    if (rt_obj_release_check0(delta))
        rt_obj_free(delta);
    if (rt_obj_release_check0(b))
        rt_obj_free(b);
    if (rt_obj_release_check0(a))
        rt_obj_free(a);
    return ok;
}

/// @brief Overlap two colliders at explicit poses through the standard
///        narrow-phase (scratch bodies, never world-registered). Combat-volume
///        primitive: writes the contact normal/depth/witness point on hit.
/// @param collider_a First Collider3D.
/// @param pos_a First world position.
/// @param quat_a First finite XYZW orientation, normalized before use.
/// @param collider_b Second Collider3D.
/// @param pos_b Second world position.
/// @param quat_b Second finite XYZW orientation, normalized before use.
/// @param out_normal Optional A-to-B normal output.
/// @param out_depth Optional penetration-depth output.
/// @param out_point Optional witness-point output.
/// @return One when the posed colliders overlap, otherwise zero.
int8_t rt_collider3d_overlap_at_raw(void *collider_a,
                                    const double *pos_a,
                                    const double *quat_a,
                                    void *collider_b,
                                    const double *pos_b,
                                    const double *quat_b,
                                    double *out_normal,
                                    double *out_depth,
                                    double *out_point) {
    double position_a[3] = {0.0, 0.0, 0.0};
    double position_b[3] = {0.0, 0.0, 0.0};
    double orientation_a[4] = {0.0, 0.0, 0.0, 1.0};
    double orientation_b[4] = {0.0, 0.0, 0.0, 1.0};
    void *valid_collider_a;
    void *valid_collider_b;
    rt_body3d body_a;
    rt_body3d body_b;
    int8_t hit = 0;
    if (pos_a && quat_a && pos_b && quat_b) {
        memcpy(position_a, pos_a, sizeof(position_a));
        memcpy(position_b, pos_b, sizeof(position_b));
        memcpy(orientation_a, quat_a, sizeof(orientation_a));
        memcpy(orientation_b, quat_b, sizeof(orientation_b));
    }
    if (out_normal) {
        out_normal[0] = 0.0;
        out_normal[1] = 1.0;
        out_normal[2] = 0.0;
    }
    if (out_depth)
        *out_depth = 0.0;
    if (out_point)
        out_point[0] = out_point[1] = out_point[2] = 0.0;
    if (!collider_a || !collider_b || !pos_a || !quat_a || !pos_b || !quat_b ||
        !ph3d_vec3_all_finite(position_a) || !ph3d_vec3_all_finite(position_b) ||
        !isfinite(orientation_a[0]) || !isfinite(orientation_a[1]) || !isfinite(orientation_a[2]) ||
        !isfinite(orientation_a[3]) || !isfinite(orientation_b[0]) || !isfinite(orientation_b[1]) ||
        !isfinite(orientation_b[2]) || !isfinite(orientation_b[3]))
        return 0;
    valid_collider_a = rt_g3d_checked_or_null(collider_a, RT_G3D_COLLIDER3D_CLASS_ID);
    valid_collider_b = rt_g3d_checked_or_null(collider_b, RT_G3D_COLLIDER3D_CLASS_ID);
    if (!valid_collider_a || !valid_collider_b)
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        position_a[axis] = query_saturate_coord(position_a[axis]);
        position_b[axis] = query_saturate_coord(position_b[axis]);
    }
    quat_normalize(orientation_a);
    quat_normalize(orientation_b);
    probe_init_stack_body(&body_a, valid_collider_a, position_a, orientation_a);
    probe_init_stack_body(&body_b, valid_collider_b, position_b, orientation_b);
    {
        double normal[3] = {0.0, 1.0, 0.0};
        double depth = 0.0;
        double point[3] = {0.0, 0.0, 0.0};
        if (test_collision(&body_a, &body_b, normal, &depth, point, NULL, NULL, NULL, NULL) &&
            isfinite(depth) && depth > 0.0) {
            query_normalize_normal(normal, NULL);
            depth = query_sanitize_distance(depth);
            for (int axis = 0; axis < 3; ++axis)
                point[axis] = query_saturate_coord(point[axis]);
            hit = depth > 0.0 ? 1 : 0;
            if (hit && out_normal)
                memcpy(out_normal, normal, sizeof(normal));
            if (hit && out_depth)
                *out_depth = depth;
            if (hit && out_point)
                memcpy(out_point, point, sizeof(point));
        }
    }
    return hit;
}

//=========================================================================
// Public probes
//=========================================================================

/// @brief `Physics3DWorld.ProbeClearance(position, radius, height, mask)` —
///   true when a capsule of the given dims fits at @p position (no solid overlap).
/// @param world_obj World3D handle.
/// @param position Vec3 capsule position.
/// @param radius Positive capsule radius.
/// @param height Positive total capsule height.
/// @param mask Query layer mask.
/// @return One when the capsule fits, otherwise zero.
int8_t rt_world3d_probe_clearance(
    void *world_obj, void *position, double radius, double height, int64_t mask) {
    rt_world3d *world = (rt_world3d *)rt_g3d_checked_or_null(world_obj, RT_G3D_WORLD3D_CLASS_ID);
    double pos[3];
    if (!world ||
        !probe_read_vec3(position, pos, "Physics3DWorld.ProbeClearance: position must be Vec3"))
        return 0;
    if (!isfinite(radius) || radius <= 0.0 || !isfinite(height) || height <= 0.0)
        return 0;
    radius = query_sanitize_distance(radius);
    height = query_sanitize_distance(height);
    return probe_capsule_overlaps(world, pos, radius, height, mask) ? 0 : 1;
}

/// @brief `Physics3DWorld.ProbeLedge(origin, forward, radius, maxHeight, maxDepth,
///   mask)` — find a grabbable ledge ahead; NULL when nothing valid is found.
/// @param world_obj World3D handle.
/// @param origin_vec Vec3 foot-level origin.
/// @param forward_vec Vec3 search direction.
/// @param radius Positive probe radius.
/// @param max_height Maximum ledge rise and standing height.
/// @param max_depth Maximum forward reach.
/// @param mask Query layer mask.
/// @return Newly allocated LedgeHit3D snapshot, or NULL when no valid ledge exists.
void *rt_world3d_probe_ledge(void *world_obj,
                             void *origin_vec,
                             void *forward_vec,
                             double radius,
                             double max_height,
                             double max_depth,
                             int64_t mask) {
    rt_world3d *world = (rt_world3d *)rt_g3d_checked_or_null(world_obj, RT_G3D_WORLD3D_CLASS_ID);
    double origin[3];
    double forward[3];
    if (!world ||
        !probe_read_vec3(origin_vec, origin, "Physics3DWorld.ProbeLedge: origin must be Vec3") ||
        !probe_read_vec3(forward_vec, forward, "Physics3DWorld.ProbeLedge: forward must be Vec3"))
        return NULL;
    if (!isfinite(radius) || radius <= 0.0 || !isfinite(max_height) || max_height <= 0.0 ||
        !isfinite(max_depth) || max_depth <= 0.0)
        return NULL;
    radius = query_sanitize_distance(radius);
    max_height = query_sanitize_distance(max_height);
    max_depth = query_sanitize_distance(max_depth);

    double wall_point[3];
    double wall_normal[3];
    double top_point[3];
    double top_normal[3];
    if (!probe_find_ledge_top(world_obj,
                              world,
                              origin,
                              forward,
                              radius,
                              max_height,
                              max_depth,
                              mask,
                              wall_point,
                              wall_normal,
                              top_point,
                              top_normal))
        return NULL;

    rt_ledge_hit3d_obj *result = ledge_hit3d_new("Physics3DWorld.ProbeLedge: allocation failed");
    if (!result)
        return NULL;
    /* Grab point: wall contact XZ at the ledge-top height. */
    result->grab_point[0] = wall_point[0];
    result->grab_point[1] = top_point[1];
    result->grab_point[2] = wall_point[2];
    memcpy(result->surface_normal, top_normal, sizeof(top_normal));
    memcpy(result->wall_normal, wall_normal, sizeof(wall_normal));
    result->height = top_point[1] - origin[1];
    /* 3. Standing room: capsule of the probe radius and maxHeight budget,
     * centered above the ledge top. A hang-only ledge is still a valid result. */
    {
        double stand_pos[3] = {
            top_point[0], top_point[1] + max_height * 0.5 + PH3D_PROBE_SKIN, top_point[2]};
        result->has_standing_room =
            probe_capsule_overlaps(world, stand_pos, radius, max_height, mask) ? 0 : 1;
    }
    result->has_landing = 0;
    return result;
}

/// @brief `Physics3DWorld.ProbeVault(origin, forward, radius, maxHeight,
///   maxThickness, mask)` — like ProbeLedge but also requires a near-origin-level
///   landing on the far side of the obstacle; NULL when not vaultable.
/// @param world_obj World3D handle.
/// @param origin_vec Vec3 foot-level origin.
/// @param forward_vec Vec3 search direction.
/// @param radius Positive probe radius.
/// @param max_height Maximum vault rise and clearance height.
/// @param max_thickness Maximum obstacle thickness.
/// @param mask Query layer mask.
/// @return Newly allocated LedgeHit3D with landing data, or NULL when not vaultable.
void *rt_world3d_probe_vault(void *world_obj,
                             void *origin_vec,
                             void *forward_vec,
                             double radius,
                             double max_height,
                             double max_thickness,
                             int64_t mask) {
    rt_world3d *world = (rt_world3d *)rt_g3d_checked_or_null(world_obj, RT_G3D_WORLD3D_CLASS_ID);
    double origin[3];
    double forward[3];
    if (!world ||
        !probe_read_vec3(origin_vec, origin, "Physics3DWorld.ProbeVault: origin must be Vec3") ||
        !probe_read_vec3(forward_vec, forward, "Physics3DWorld.ProbeVault: forward must be Vec3"))
        return NULL;
    if (!isfinite(radius) || radius <= 0.0 || !isfinite(max_height) || max_height <= 0.0 ||
        !isfinite(max_thickness) || max_thickness <= 0.0)
        return NULL;
    radius = query_sanitize_distance(radius);
    max_height = query_sanitize_distance(max_height);
    max_thickness = query_sanitize_distance(max_thickness);

    double wall_point[3];
    double wall_normal[3];
    double top_point[3];
    double top_normal[3];
    if (!probe_find_ledge_top(world_obj,
                              world,
                              origin,
                              forward,
                              radius,
                              max_height,
                              query_sanitize_distance(max_thickness + radius * 2.0),
                              mask,
                              wall_point,
                              wall_normal,
                              top_point,
                              top_normal))
        return NULL;

    /* Far-side landing: drop a sphere beyond the obstacle and require ground
     * near origin level (rejects standing on top of a thick wall). */
    double fwd[3] = {forward[0], 0.0, forward[2]};
    double fwd_len = hypot(fwd[0], fwd[2]);
    if (!isfinite(fwd_len) || fwd_len <= 1e-9)
        return NULL;
    fwd[0] /= fwd_len;
    fwd[2] /= fwd_len;
    double far_offset = query_sanitize_distance(max_thickness + radius);
    double far_x = wall_point[0] + fwd[0] * far_offset;
    double far_z = wall_point[2] + fwd[2] * far_offset;
    double drop = query_sanitize_distance(max_height + PH3D_PROBE_VAULT_DROP_TOLERANCE + radius);
    void *far_center = rt_vec3_new(far_x, origin[1] + max_height + radius, far_z);
    void *down = rt_vec3_new(0.0, -drop, 0.0);
    void *land_hit = rt_world3d_sweep_sphere(world_obj, far_center, radius, down, mask);
    void *result = NULL;
    if (land_hit && !rt_physics_hit3d_get_started_penetrating(land_hit)) {
        void *land_point = rt_physics_hit3d_get_point(land_hit);
        void *land_normal = rt_physics_hit3d_get_normal(land_hit);
        if (land_point && land_normal &&
            ph3d_finite_or(rt_vec3_y(land_normal), 0.0) >= PH3D_PROBE_WALKABLE_NORMAL_Y) {
            double land_y = ph3d_finite_or(rt_vec3_y(land_point), origin[1]);
            if (land_y <= origin[1] + PH3D_PROBE_VAULT_LANDING_MAX_RISE &&
                land_y >= origin[1] - PH3D_PROBE_VAULT_DROP_TOLERANCE) {
                rt_ledge_hit3d_obj *vault =
                    ledge_hit3d_new("Physics3DWorld.ProbeVault: allocation failed");
                if (vault) {
                    vault->grab_point[0] = wall_point[0];
                    vault->grab_point[1] = top_point[1];
                    vault->grab_point[2] = wall_point[2];
                    memcpy(vault->surface_normal, top_normal, sizeof(top_normal));
                    memcpy(vault->wall_normal, wall_normal, sizeof(wall_normal));
                    vault->height = top_point[1] - origin[1];
                    vault->landing_point[0] = ph3d_finite_or(rt_vec3_x(land_point), far_x);
                    vault->landing_point[1] = land_y;
                    vault->landing_point[2] = ph3d_finite_or(rt_vec3_z(land_point), far_z);
                    vault->has_landing = 1;
                    double stand_pos[3] = {vault->landing_point[0],
                                           vault->landing_point[1] + max_height * 0.5 +
                                               PH3D_PROBE_SKIN,
                                           vault->landing_point[2]};
                    vault->has_standing_room =
                        probe_capsule_overlaps(world, stand_pos, radius, max_height, mask) ? 0 : 1;
                    result = vault;
                }
            }
        }
        if (land_point && rt_obj_release_check0(land_point))
            rt_obj_free(land_point);
        if (land_normal && rt_obj_release_check0(land_normal))
            rt_obj_free(land_normal);
    }
    if (land_hit && rt_obj_release_check0(land_hit))
        rt_obj_free(land_hit);
    if (rt_obj_release_check0(down))
        rt_obj_free(down);
    if (rt_obj_release_check0(far_center))
        rt_obj_free(far_center);
    return result;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
