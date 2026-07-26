//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_3d_physics_stubs.c
/// @brief Graphics-disabled 3D physics, colliders, raycast, and joint stubs.
///
/// @details This split source groups physics-family unavailable-backend entry
/// points so the trap-only implementation remains easy to audit without
/// interleaving unrelated graphics subsystems.
///
// File: src/runtime/graphics/common/rt_3d_physics_stubs.c
// Purpose: Graphics-disabled 3D physics, collision, raycast, and body entry points.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* Ray3D / AABB3D / RayHit3D stubs */

/// @brief Stub for `Ray3D.IntersectTriangle` — Möller-Trumbore ray-vs-
///        single-triangle intersection. Returns the parametric distance `t`
///        along the ray to the hit, or `-1` for no hit / behind origin.
///
/// Silent stub returning `-1.0`.
///
/// @param o  Vec3 ray origin (ignored).
/// @param d  Vec3 ray direction (must be normalized) (ignored).
/// @param v0 Vec3 triangle vertex 0 (ignored).
/// @param v1 Vec3 triangle vertex 1 (ignored).
/// @param v2 Vec3 triangle vertex 2 (ignored).
///
/// @return `-1.0`.
double rt_ray3d_intersect_triangle(void *o, void *d, void *v0, void *v1, void *v2) {
    (void)o;
    (void)d;
    (void)v0;
    (void)v1;
    (void)v2;
    return -1.0;
}

/// @brief Stub for `Ray3D.IntersectMesh` — would normally test a ray
///        against every triangle of `m` (after applying transform `t`)
///        and return the closest hit as a RayHit3D, or NULL for no hit.
///
/// Silent stub returning NULL.
///
/// @param o Vec3 ray origin (ignored).
/// @param d Vec3 ray direction (ignored).
/// @param m Mesh3D handle (ignored).
/// @param t Transform3D handle (ignored).
///
/// @return `NULL`.
void *rt_ray3d_intersect_mesh(void *o, void *d, void *m, void *t) {
    (void)o;
    (void)d;
    (void)m;
    (void)t;
    return NULL;
}

/// @brief Stub for `Ray3D.IntersectAABB` — slab-method ray-vs-AABB
///        intersection. Returns the parametric distance to the entry
///        face, or `-1` for no hit.
///
/// Silent stub returning `-1.0`.
///
/// @param o  Vec3 ray origin (ignored).
/// @param d  Vec3 ray direction (ignored).
/// @param mn Vec3 AABB min corner (ignored).
/// @param mx Vec3 AABB max corner (ignored).
///
/// @return `-1.0`.
double rt_ray3d_intersect_aabb(void *o, void *d, void *mn, void *mx) {
    (void)o;
    (void)d;
    (void)mn;
    (void)mx;
    return -1.0;
}

/// @brief Stub for `Ray3D.IntersectSphere` — analytic ray-vs-sphere
///        intersection (quadratic discriminant). Returns the parametric
///        distance to the entry point, or `-1` for no hit.
///
/// Silent stub returning `-1.0`.
///
/// @param o Vec3 ray origin (ignored).
/// @param d Vec3 ray direction (ignored).
/// @param c Vec3 sphere center (ignored).
/// @param r Sphere radius (ignored).
///
/// @return `-1.0`.
double rt_ray3d_intersect_sphere(void *o, void *d, void *c, double r) {
    (void)o;
    (void)d;
    (void)c;
    (void)r;
    return -1.0;
}

/// @brief Stub for `AABB3D.Overlaps` — boolean AABB-vs-AABB overlap
///        test. Each AABB is given as `(min, max)` Vec3 pairs.
///
/// Silent stub returning `0` (no overlap).
///
/// @param a0 Vec3 AABB A min corner (ignored).
/// @param a1 Vec3 AABB A max corner (ignored).
/// @param b0 Vec3 AABB B min corner (ignored).
/// @param b1 Vec3 AABB B max corner (ignored).
///
/// @return `0`.
int8_t rt_aabb3d_overlaps(void *a0, void *a1, void *b0, void *b1) {
    (void)a0;
    (void)a1;
    (void)b0;
    (void)b1;
    return 0;
}

/// @brief Stub for `AABB3D.Penetration` — would normally return a Vec3
///        representing the minimum-translation vector to separate the
///        two AABBs along the axis of least overlap.
///
/// Silent stub returning NULL.
///
/// @param a0 Vec3 AABB A min corner (ignored).
/// @param a1 Vec3 AABB A max corner (ignored).
/// @param b0 Vec3 AABB B min corner (ignored).
/// @param b1 Vec3 AABB B max corner (ignored).
///
/// @return `NULL`.
void *rt_aabb3d_penetration(void *a0, void *a1, void *b0, void *b1) {
    (void)a0;
    (void)a1;
    (void)b0;
    (void)b1;
    return NULL;
}

/// @brief Stub for `Ray3D.HitDistance` — get the parametric distance
///        stored in a RayHit3D record (`-1` for no hit).
///
/// Silent stub returning `-1.0`.
///
/// @param h RayHit3D handle (ignored).
///
/// @return `-1.0`.
double rt_ray3d_hit_distance(void *h) {
    (void)h;
    return -1.0;
}

/// @brief Stub for `Ray3D.HitPoint` — get the world-space hit point
///        stored in a RayHit3D record as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param h RayHit3D handle (ignored).
///
/// @return `NULL`.
void *rt_ray3d_hit_point(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Ray3D.HitNormal` — get the surface normal at the
///        hit point as a Vec3 (always points back along the ray).
///
/// Silent stub returning NULL.
///
/// @param h RayHit3D handle (ignored).
///
/// @return `NULL`.
void *rt_ray3d_hit_normal(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Ray3D.HitTriangle` — get the triangle index of a
///        ray-vs-mesh hit record (`-1` if no hit / opaque hit type).
///
/// Silent stub returning `-1` (no hit).
///
/// @param h Hit record handle (ignored).
///
/// @return `-1`.
int64_t rt_ray3d_hit_triangle(void *h) {
    (void)h;
    return -1;
}

/// @brief Stub for `Sphere3D.Overlaps` — boolean sphere-vs-sphere overlap
///        test (centers `a` and `b`, radii `ra` and `rb`).
///
/// Silent stub returning `0` (no overlap). The real implementation does a
/// distance-vs-sum-of-radii comparison.
///
/// @param a  Vec3 center of sphere A (ignored).
/// @param ra Radius of sphere A (ignored).
/// @param b  Vec3 center of sphere B (ignored).
/// @param rb Radius of sphere B (ignored).
///
/// @return `0`.
int8_t rt_sphere3d_overlaps(void *a, double ra, void *b, double rb) {
    (void)a;
    (void)ra;
    (void)b;
    (void)rb;
    return 0;
}

/// @brief Stub for `Sphere3D.Penetration` — would normally return a Vec3
///        representing the minimum-translation vector to separate the two
///        spheres (zero magnitude when not overlapping).
///
/// Silent stub returning NULL.
///
/// @param a  Vec3 center of sphere A (ignored).
/// @param ra Radius of sphere A (ignored).
/// @param b  Vec3 center of sphere B (ignored).
/// @param rb Radius of sphere B (ignored).
///
/// @return `NULL`.
void *rt_sphere3d_penetration(void *a, double ra, void *b, double rb) {
    (void)a;
    (void)ra;
    (void)b;
    (void)rb;
    return NULL;
}

/// @brief Stub for `AABB3D.ClosestPoint` — would normally return the
///        point on the AABB surface closest to `p`. Used by sphere-vs-AABB
///        narrow-phase collision.
///
/// Silent stub returning NULL.
///
/// @param mn Vec3 AABB min corner (ignored).
/// @param mx Vec3 AABB max corner (ignored).
/// @param p  Vec3 query point (ignored).
///
/// @return `NULL`.
void *rt_aabb3d_closest_point(void *mn, void *mx, void *p) {
    (void)mn;
    (void)mx;
    (void)p;
    return NULL;
}

/// @brief Stub for `AABB3D.SphereOverlaps` — boolean AABB-vs-sphere
///        overlap test.
///
/// Silent stub returning `0` (no overlap).
///
/// @param mn Vec3 AABB min corner (ignored).
/// @param mx Vec3 AABB max corner (ignored).
/// @param c  Vec3 sphere center (ignored).
/// @param r  Sphere radius (ignored).
///
/// @return `0`.
int8_t rt_aabb3d_sphere_overlaps(void *mn, void *mx, void *c, double r) {
    (void)mn;
    (void)mx;
    (void)c;
    (void)r;
    return 0;
}

/// @brief Stub for `Segment3D.ClosestPoint` — would normally return the
///        point on the segment `[a, b]` closest to `p`. Used by capsule
///        narrow-phase collision.
///
/// Silent stub returning NULL.
///
/// @param a Vec3 segment start (ignored).
/// @param b Vec3 segment end (ignored).
/// @param p Vec3 query point (ignored).
///
/// @return `NULL`.
void *rt_segment3d_closest_point(void *a, void *b, void *p) {
    (void)a;
    (void)b;
    (void)p;
    return NULL;
}

/// @brief Stub for `Capsule3D.SphereOverlaps` — boolean capsule-vs-sphere
///        overlap test. Capsule is the swept volume of a sphere of radius
///        `cr` from `a` to `b`.
///
/// Silent stub returning `0`.
///
/// @param a  Vec3 capsule axis start (ignored).
/// @param b  Vec3 capsule axis end (ignored).
/// @param cr Capsule radius (ignored).
/// @param c  Vec3 sphere center (ignored).
/// @param sr Sphere radius (ignored).
///
/// @return `0`.
int8_t rt_capsule3d_sphere_overlaps(void *a, void *b, double cr, void *c, double sr) {
    (void)a;
    (void)b;
    (void)cr;
    (void)c;
    (void)sr;
    return 0;
}

/// @brief Stub for `Capsule3D.AABBOverlaps` — boolean capsule-vs-AABB
///        overlap test.
///
/// Silent stub returning `0`.
///
/// @param a  Vec3 capsule axis start (ignored).
/// @param b  Vec3 capsule axis end (ignored).
/// @param r  Capsule radius (ignored).
/// @param mn Vec3 AABB min corner (ignored).
/// @param mx Vec3 AABB max corner (ignored).
///
/// @return `0`.
int8_t rt_capsule3d_aabb_overlaps(void *a, void *b, double r, void *mn, void *mx) {
    (void)a;
    (void)b;
    (void)r;
    (void)mn;
    (void)mx;
    return 0;
}

/* Physics3D World stubs */

/// @brief Stub for `Physics3DWorld.New` — would normally create an
///        empty physics world with the given gravity vector. Bodies are
///        added via `Add`; the simulation is advanced one step at a time
///        via `Step`.
///
/// Trapping stub: callers expect a usable handle for body management.
///
/// @param gx Gravity x in world units / second² (ignored).
/// @param gy Gravity y; typically `-9.81` for Earth-like (ignored).
/// @param gz Gravity z (ignored).
///
/// @return Never returns normally.
void *rt_world3d_new(double gx, double gy, double gz) {
    (void)gx;
    (void)gy;
    (void)gz;
    rt_graphics_unavailable_("Physics3DWorld.New: graphics support not compiled in");
    return NULL;
}

/// @brief Silently discard a request to advance a graphics-disabled physics world.
///
/// @param w  Physics3DWorld handle (ignored).
/// @param dt Simulation interval in seconds (ignored).
void rt_world3d_step(void *w, double dt) {
    (void)w;
    (void)dt;
}

/// @brief Silently discard a request to add a body to a graphics-disabled physics world.
///
/// @param w Physics3DWorld handle (ignored).
/// @param b Body3D handle to add (ignored).
void rt_world3d_add(void *w, void *b) {
    (void)w;
    (void)b;
}

/// @brief Report that a body cannot be added to a graphics-disabled physics world.
///
/// @param w Physics3DWorld handle (ignored).
/// @param b Body3D handle to add (ignored).
///
/// @return `0`, indicating that the body was not added.
int8_t rt_world3d_try_add(void *w, void *b) {
    (void)w;
    (void)b;
    return 0;
}

/// @brief Silently discard a request to remove a body from a graphics-disabled physics world.
///
/// @param w Physics3DWorld handle (ignored).
/// @param b Body3D handle to remove (ignored).
void rt_world3d_remove(void *w, void *b) {
    (void)w;
    (void)b;
}

/// @brief Return the fallback body count for a graphics-disabled physics world.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.StepFixed`.
///
/// @param w         Physics3DWorld handle (ignored).
/// @param dt        Accumulated frame interval in seconds (ignored).
/// @param fixed_dt  Requested fixed simulation interval in seconds (ignored).
/// @param max_steps Maximum fixed steps allowed for this call (ignored).
///
/// @return `0`, indicating that no simulation steps ran.
int64_t rt_world3d_step_fixed(void *w, double dt, double fixed_dt, int64_t max_steps) {
    (void)w;
    (void)dt;
    (void)fixed_dt;
    (void)max_steps;
    return 0;
}

/// @brief Stub for `Physics3DWorld.FixedStepAlpha`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0.0`, the fallback interpolation alpha.
double rt_world3d_get_fixed_step_alpha(void *w) {
    (void)w;
    return 0.0;
}

/// @brief Stub for `Physics3DWorld.DroppedFixedSteps`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because the stub never schedules or drops fixed steps.
int64_t rt_world3d_get_dropped_fixed_steps(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.LastCcdRequestedSubsteps`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no continuous-collision pass ran.
int64_t rt_world3d_get_last_ccd_requested_substeps(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.LastCcdSubsteps`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no continuous-collision substeps ran.
int64_t rt_world3d_get_last_ccd_substeps(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.CcdSubstepClampedCount`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because the stub never clamps CCD substeps.
int64_t rt_world3d_get_ccd_substep_clamped_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.SolverIterations`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, the fallback velocity-solver iteration count.
int64_t rt_world3d_get_solver_iterations(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.SolverIterations` setter.
///
/// @param w          Physics3DWorld handle (ignored).
/// @param iterations Requested velocity-solver iteration count (ignored).
void rt_world3d_set_solver_iterations(void *w, int64_t iterations) {
    (void)w;
    (void)iterations;
}

/// @brief Stub for `Physics3DWorld.PositionIterations`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, the fallback position-solver iteration count.
int64_t rt_world3d_get_position_iterations(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.PositionIterations` setter.
///
/// @param w          Physics3DWorld handle (ignored).
/// @param iterations Requested position-solver iteration count (ignored).
void rt_world3d_set_position_iterations(void *w, int64_t iterations) {
    (void)w;
    (void)iterations;
}

/// @brief Stub for `Physics3DWorld.ContactBeta`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0.0`, the fallback contact stabilization factor.
double rt_world3d_get_contact_beta(void *w) {
    (void)w;
    return 0.0;
}

/// @brief Stub for `Physics3DWorld.ContactBeta` setter.
///
/// @param w    Physics3DWorld handle (ignored).
/// @param beta Requested contact stabilization factor (ignored).
void rt_world3d_set_contact_beta(void *w, double beta) {
    (void)w;
    (void)beta;
}

/// @brief Stub for `Physics3DWorld.RestitutionThreshold`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0.0`, the fallback restitution velocity threshold.
double rt_world3d_get_restitution_threshold(void *w) {
    (void)w;
    return 0.0;
}

/// @brief Stub for `Physics3DWorld.RestitutionThreshold` setter.
///
/// @param w         Physics3DWorld handle (ignored).
/// @param threshold Requested restitution velocity threshold (ignored).
void rt_world3d_set_restitution_threshold(void *w, double threshold) {
    (void)w;
    (void)threshold;
}

/// @brief Stub for `Physics3DWorld.LastSolverIslandCount`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no solver islands were processed.
int64_t rt_world3d_get_last_solver_island_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.LastSolverActiveBodyCount`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no active bodies were processed.
int64_t rt_world3d_get_last_solver_active_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.LastSolverContactCount`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no contacts were solved.
int64_t rt_world3d_get_last_solver_contact_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Silently discard a gravity update for a graphics-disabled physics world.
///
/// @param w  Physics3DWorld handle (ignored).
/// @param gx Gravity acceleration along X (ignored).
/// @param gy Gravity acceleration along Y (ignored).
/// @param gz Gravity acceleration along Z (ignored).
void rt_world3d_set_gravity(void *w, double gx, double gy, double gz) {
    (void)w;
    (void)gx;
    (void)gy;
    (void)gz;
}

/// @brief Stub for `Physics3DWorld.AddJoint` — register a joint
///        constraint with the world's solver. `jt` distinguishes the
///        joint type (Distance / Spring / future kinds) so the world
///        can dispatch to the correct solver code.
///
/// Silent no-op stub.
///
/// @param w  Physics3DWorld handle (ignored).
/// @param j  Joint handle (ignored).
/// @param jt Joint type tag (ignored).
void rt_world3d_add_joint(void *w, void *j, int64_t jt) {
    (void)w;
    (void)j;
    (void)jt;
}

/// @brief Stub for `Physics3DWorld.RemoveJoint` — unregister a joint
///        from the world. The joint object is left intact for re-add.
///
/// Silent no-op stub.
///
/// @param w Physics3DWorld handle (ignored).
/// @param j Joint handle (ignored).
void rt_world3d_remove_joint(void *w, void *j) {
    (void)w;
    (void)j;
}

/// @brief Stub for `Physics3DWorld.JointCount` — number of joints
///        currently registered in the world.
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_joint_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.CollisionCount` — number of contact
///        pairs the most recent `Step` produced. Use as a queue length
///        for iterating with the indexed accessors below.
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_get_collision_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.CollisionBodyA(i)` — first body in
///        the `i`th contact pair.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Contact pair index, 0..CollisionCount-1 (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_collision_body_a(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.CollisionBodyB(i)` — second body in
///        the `i`th contact pair.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Contact pair index (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_collision_body_b(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.CollisionNormal(i)` — contact normal
///        for the `i`th contact pair as a Vec3 (points from body A
///        toward body B).
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Contact pair index (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_collision_normal(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.CollisionDepth(i)` — penetration
///        depth for the `i`th contact pair (positive = bodies overlap).
///
/// Silent stub returning `0.0`.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Contact pair index (ignored).
///
/// @return `0.0`.
double rt_world3d_get_collision_depth(void *w, int64_t i) {
    (void)w;
    (void)i;
    return 0.0;
}

/// @brief Stub for `Physics3DWorld.CollisionEventCount` — number of
///        rich CollisionEvent3D records produced by the most recent
///        `Step`. Distinct from `CollisionCount` (which exposes raw
///        contact pairs); events carry contact-manifold detail.
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_get_collision_event_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.CollisionEvent(i)` — get the `i`th
///        rich CollisionEvent3D from the world's event queue.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Event index, 0..CollisionEventCount-1 (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_collision_event(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.EnterEventCount` — number of
///        contact-enter events from the most recent `Step` (pairs that
///        started touching this tick).
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_get_enter_event_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.EnterEvent(i)` — get the `i`th
///        contact-enter CollisionEvent3D from this tick's queue.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Enter-event index (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_enter_event(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.StayEventCount` — number of
///        contact-stay events from the most recent `Step` (pairs still
///        touching from a previous tick).
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_get_stay_event_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.StayEvent(i)` — get the `i`th
///        contact-stay CollisionEvent3D from this tick's queue.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Stay-event index (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_stay_event(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.ExitEventCount` — number of
///        contact-exit events from the most recent `Step` (pairs that
///        stopped touching this tick).
///
/// Silent stub returning `0`.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`.
int64_t rt_world3d_get_exit_event_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.ExitEvent(i)` — get the `i`th
///        contact-exit CollisionEvent3D from this tick's queue.
///
/// Silent stub returning NULL.
///
/// @param w Physics3DWorld handle (ignored).
/// @param i Exit-event index (ignored).
///
/// @return `NULL`.
void *rt_world3d_get_exit_event(void *w, int64_t i) {
    (void)w;
    (void)i;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.ClearCollisionEvents`.
///
/// Silent no-op stub.
///
/// @param w Physics3DWorld handle (ignored).
void rt_world3d_clear_collision_events(void *w) {
    (void)w;
}

/// @brief Stub for `Physics3DWorld.Raycast` — single-hit raycast
///        against the world. Returns the closest Physics3DHit, or NULL
///        if no body was hit within `max_distance` along the ray (also
///        filtered by `mask`).
///
/// Silent stub returning NULL.
///
/// @param w            Physics3DWorld handle (ignored).
/// @param origin       Vec3 ray origin (ignored).
/// @param direction    Vec3 ray direction (must be normalized) (ignored).
/// @param max_distance Maximum hit distance in world units (ignored).
/// @param mask         Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_raycast(
    void *w, void *origin, void *direction, double max_distance, int64_t mask) {
    (void)w;
    (void)origin;
    (void)direction;
    (void)max_distance;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.RaycastAll` — multi-hit raycast
///        that returns every body the ray passes through (up to the
///        max distance), as a Physics3DHitList.
///
/// Silent stub returning NULL.
///
/// @param w            Physics3DWorld handle (ignored).
/// @param origin       Vec3 ray origin (ignored).
/// @param direction    Vec3 ray direction (ignored).
/// @param max_distance Maximum hit distance in world units (ignored).
/// @param mask         Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_raycast_all(
    void *w, void *origin, void *direction, double max_distance, int64_t mask) {
    (void)w;
    (void)origin;
    (void)direction;
    (void)max_distance;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.SweepSphere` — sweep a sphere of
///        the given radius along the displacement vector `delta`,
///        returning the first contact (Physics3DHit) or NULL if none.
///
/// Silent stub returning NULL.
///
/// @param w      Physics3DWorld handle (ignored).
/// @param center Vec3 sphere starting center (ignored).
/// @param radius Sphere radius (ignored).
/// @param delta  Vec3 sweep displacement (ignored).
/// @param mask   Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_sweep_sphere(void *w, void *center, double radius, void *delta, int64_t mask) {
    (void)w;
    (void)center;
    (void)radius;
    (void)delta;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.SweepCapsule` — sweep a capsule
///        (axis from `a` to `b`, radius `radius`) along displacement
///        `delta`, returning the first contact or NULL.
///
/// Silent stub returning NULL.
///
/// @param w      Physics3DWorld handle (ignored).
/// @param a      Vec3 capsule axis start (ignored).
/// @param b      Vec3 capsule axis end (ignored).
/// @param radius Capsule cross-sectional radius (ignored).
/// @param delta  Vec3 sweep displacement (ignored).
/// @param mask   Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_sweep_capsule(
    void *w, void *a, void *b, double radius, void *delta, int64_t mask) {
    (void)w;
    (void)a;
    (void)b;
    (void)radius;
    (void)delta;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.OverlapSphere` — return every body
///        whose collider overlaps a static sphere centered at `center`
///        with the given `radius`. Useful for AOE damage queries and
///        proximity sensors.
///
/// Silent stub returning NULL.
///
/// @param w      Physics3DWorld handle (ignored).
/// @param center Vec3 sphere center (ignored).
/// @param radius Sphere radius (ignored).
/// @param mask   Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_overlap_sphere(void *w, void *center, double radius, int64_t mask) {
    (void)w;
    (void)center;
    (void)radius;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.OverlapAABB` — return every body
///        whose collider overlaps a static axis-aligned box `(min_corner,
///        max_corner)`. Useful for box triggers and selection regions.
///
/// Silent stub returning NULL.
///
/// @param w          Physics3DWorld handle (ignored).
/// @param min_corner Vec3 AABB min corner (ignored).
/// @param max_corner Vec3 AABB max corner (ignored).
/// @param mask       Layer-bitmask filter (ignored).
///
/// @return `NULL`.
void *rt_world3d_overlap_aabb(void *w, void *min_corner, void *max_corner, int64_t mask) {
    (void)w;
    (void)min_corner;
    (void)max_corner;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DHit.Distance` — get the distance from the
///        ray/sweep origin to the hit point.
///
/// Silent stub returning `0.0`.
///
/// @param h Hit record handle (ignored).
///
/// @return `0.0`.
double rt_physics_hit3d_get_distance(void *h) {
    (void)h;
    return 0.0;
}

/// @brief Stub for `Physics3DHit.Body` — get the Body3D that was hit.
///
/// Silent stub returning NULL.
///
/// @param h Hit record handle (ignored).
///
/// @return `NULL`.
void *rt_physics_hit3d_get_body(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Physics3DHit.Collider` — get the Collider3D shape
///        that was hit (a body may have multiple compound child colliders).
///
/// Silent stub returning NULL.
///
/// @param h Hit record handle (ignored).
///
/// @return `NULL`.
void *rt_physics_hit3d_get_collider(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Physics3DHit.Point` — get the world-space hit point
///        as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param h Hit record handle (ignored).
///
/// @return `NULL`.
void *rt_physics_hit3d_get_point(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Physics3DHit.Normal` — get the surface normal at the
///        hit point as a Vec3 (always points away from the hit body).
///
/// Silent stub returning NULL.
///
/// @param h Hit record handle (ignored).
///
/// @return `NULL`.
void *rt_physics_hit3d_get_normal(void *h) {
    (void)h;
    return NULL;
}

/// @brief Stub for `Physics3DHit.Fraction` — for sweep tests, get the
///        fraction along the swept path (0..1) at which contact occurred.
///
/// Silent stub returning `0.0`.
///
/// @param h Hit record handle (ignored).
///
/// @return `0.0`.
double rt_physics_hit3d_get_fraction(void *h) {
    (void)h;
    return 0.0;
}

/// @brief Stub for `Physics3DHit.StartedPenetrating` — for sweep tests,
///        true when the swept shape was already overlapping the target at
///        the start of the sweep.
///
/// Silent stub returning `0`.
///
/// @param h Hit record handle (ignored).
///
/// @return `0`.
int8_t rt_physics_hit3d_get_started_penetrating(void *h) {
    (void)h;
    return 0;
}

/// @brief Stub for `Physics3DHit.IsTrigger` — true if the hit body's
///        collider is a sensor/trigger (no impulse exchange, just an
///        overlap notification).
///
/// Silent stub returning `0`.
///
/// @param h Hit record handle (ignored).
///
/// @return `0`.
int8_t rt_physics_hit3d_get_is_trigger(void *h) {
    (void)h;
    return 0;
}

/// @brief Stub for `Physics3DHitList.Count` — number of hits in a multi-hit
///        query result (e.g. raycast all, overlap query).
///
/// Silent stub returning `0`.
///
/// @param list Hit-list handle (ignored).
///
/// @return `0`.
int64_t rt_physics_hit_list3d_get_count(void *list) {
    (void)list;
    return 0;
}

/// @brief Return the fallback untruncated hit count from a multi-hit query.
///
/// @param list Physics3DHitList handle (ignored).
///
/// @return `0`, because the stub contains no hits before truncation.
int64_t rt_physics_hit_list3d_get_total_count(void *list) {
    (void)list;
    return 0;
}

/// @brief Report whether a graphics-disabled multi-hit query was truncated.
///
/// @param list Physics3DHitList handle (ignored).
///
/// @return `0`, indicating that the empty fallback list was not truncated.
int8_t rt_physics_hit_list3d_get_truncated(void *list) {
    (void)list;
    return 0;
}

/// @brief Stub for `Physics3DHitList.Get(i)` — access the `i`th hit
///        record in a multi-hit query result.
///
/// Silent stub returning NULL.
///
/// @param list  Hit-list handle (ignored).
/// @param index Hit index, 0..Count-1 (ignored).
///
/// @return `NULL`.
void *rt_physics_hit_list3d_get(void *list, int64_t index) {
    (void)list;
    (void)index;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.BodyA` — first body involved in the
///        collision pair.
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_body_a(void *event) {
    (void)event;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.BodyB` — second body involved in the
///        collision pair.
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_body_b(void *event) {
    (void)event;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.ColliderA` — specific Collider3D
///        shape on body A that was contacted (relevant for compound
///        colliders).
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_collider_a(void *event) {
    (void)event;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.ColliderB` — specific Collider3D
///        shape on body B that was contacted.
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_collider_b(void *event) {
    (void)event;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.IsTrigger` — true when at least one
///        collider in the pair is a sensor/trigger.
///
/// Silent stub returning `0`.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0`.
int8_t rt_collision_event3d_get_is_trigger(void *event) {
    (void)event;
    return 0;
}

/// @brief Stub for `CollisionEvent3D.ContactCount` — number of contact
///        manifold points generated for this collision (typically 1..4).
///
/// Silent stub returning `0`.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0`.
int64_t rt_collision_event3d_get_contact_count(void *event) {
    (void)event;
    return 0;
}

/// @brief Stub for `CollisionEvent3D.RelativeSpeed` — magnitude of the
///        relative velocity between the bodies along the contact normal at
///        the moment of contact. Useful for impact-strength SFX.
///
/// Silent stub returning `0.0`.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0.0`.
double rt_collision_event3d_get_relative_speed(void *event) {
    (void)event;
    return 0.0;
}

/// @brief Stub for `CollisionEvent3D.NormalImpulse` — magnitude of the
///        impulse the constraint solver applied along the contact normal.
///
/// Silent stub returning `0.0`.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0.0`.
double rt_collision_event3d_get_normal_impulse(void *event) {
    (void)event;
    return 0.0;
}

/// @brief Stub for `CollisionEvent3D.Contact(i)` — get the `i`th contact
///        manifold point as an opaque ContactPoint3D handle.
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
/// @param index Contact index, 0..ContactCount-1 (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_contact(void *event, int64_t index) {
    (void)event;
    (void)index;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.ContactPoint(i)` — convenience: get
///        the world-space position of the `i`th contact directly as a
///        Vec3 (skips the ContactPoint3D wrapper).
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
/// @param index Contact index (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_contact_point(void *event, int64_t index) {
    (void)event;
    (void)index;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.ContactNormal(i)` — convenience: get
///        the world-space normal at the `i`th contact directly as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param event CollisionEvent3D handle (ignored).
/// @param index Contact index (ignored).
///
/// @return `NULL`.
void *rt_collision_event3d_get_contact_normal(void *event, int64_t index) {
    (void)event;
    (void)index;
    return NULL;
}

/// @brief Stub for `CollisionEvent3D.ContactSeparation(i)` — penetration
///        depth at the `i`th contact (negative = bodies overlap).
///
/// Silent stub returning `0.0`.
///
/// @param event CollisionEvent3D handle (ignored).
/// @param index Contact index (ignored).
///
/// @return `0.0`.
double rt_collision_event3d_get_contact_separation(void *event, int64_t index) {
    (void)event;
    (void)index;
    return 0.0;
}

/// @brief Stub for `ContactPoint3D.Point` — world-space position of the
///        contact as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param contact ContactPoint3D handle (ignored).
///
/// @return `NULL`.
void *rt_contact_point3d_get_point(void *contact) {
    (void)contact;
    return NULL;
}

/// @brief Stub for `ContactPoint3D.Normal` — world-space contact normal
///        as a Vec3 (points from body A toward body B by convention).
///
/// Silent stub returning NULL.
///
/// @param contact ContactPoint3D handle (ignored).
///
/// @return `NULL`.
void *rt_contact_point3d_get_normal(void *contact) {
    (void)contact;
    return NULL;
}

/// @brief Stub for `ContactPoint3D.Separation` — penetration depth at
///        this contact (negative = bodies overlap).
///
/// Silent stub returning `0.0`.
///
/// @param contact ContactPoint3D handle (ignored).
///
/// @return `0.0`.
double rt_contact_point3d_get_separation(void *contact) {
    (void)contact;
    return 0.0;
}

/* Physics3D Joint stubs */

/// @brief Stub for `DistanceJoint3D.New` — would normally create a
///        rigid distance constraint between bodies `a` and `b` keeping
///        them exactly `d` apart (a stiff invisible rod). Solved with
///        sequential impulses (6 iterations).
///
/// Trapping stub: joints are referenced by the Physics3DWorld step
/// loop — a NULL return would crash later.
///
/// @param a First body handle (ignored).
/// @param b Second body handle (ignored).
/// @param d Target separation distance in world units (ignored).
///
/// @return Never returns normally.
void *rt_distance_joint3d_new(void *a, void *b, double d) {
    (void)a;
    (void)b;
    (void)d;
    rt_graphics_unavailable_("DistanceJoint3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `DistanceJoint3D.Distance` — get the joint's target
///        separation distance.
///
/// Silent stub returning `0.0`.
///
/// @param j DistanceJoint3D handle (ignored).
///
/// @return `0.0`.
double rt_distance_joint3d_get_distance(void *j) {
    (void)j;
    return 0.0;
}

/// @brief Stub for `DistanceJoint3D.SetDistance` — adjust the target
///        separation. Connected bodies will be pulled / pushed back to
///        the new distance over the next few simulation steps.
///
/// Silent no-op stub.
///
/// @param j DistanceJoint3D handle (ignored).
/// @param d New target distance (ignored).
void rt_distance_joint3d_set_distance(void *j, double d) {
    (void)j;
    (void)d;
}

/// @brief Stub for `SpringJoint3D.New` — would normally create a
///        damped-spring constraint between bodies `a` and `b` with rest
///        length `rl`, spring stiffness `s`, and damping coefficient `d`.
///        Implements Hooke's law with viscous damping.
///
/// Trapping stub.
///
/// @param a  First body handle (ignored).
/// @param b  Second body handle (ignored).
/// @param rl Spring rest length (ignored).
/// @param s  Spring stiffness (force per unit displacement) (ignored).
/// @param d  Damping coefficient (force per unit relative velocity) (ignored).
///
/// @return Never returns normally.
void *rt_spring_joint3d_new(void *a, void *b, double rl, double s, double d) {
    (void)a;
    (void)b;
    (void)rl;
    (void)s;
    (void)d;
    rt_graphics_unavailable_("SpringJoint3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SpringJoint3D.Stiffness` — get the spring stiffness
///        coefficient.
///
/// Silent stub returning `0.0`.
///
/// @param j SpringJoint3D handle (ignored).
///
/// @return `0.0`.
double rt_spring_joint3d_get_stiffness(void *j) {
    (void)j;
    return 0.0;
}

/// @brief Stub for `SpringJoint3D.SetStiffness` — adjust the spring
///        stiffness coefficient. Higher = snappier oscillation.
///
/// Silent no-op stub.
///
/// @param j SpringJoint3D handle (ignored).
/// @param s New stiffness (ignored).
void rt_spring_joint3d_set_stiffness(void *j, double s) {
    (void)j;
    (void)s;
}

/// @brief Stub for `SpringJoint3D.Damping` — get the damping coefficient.
///
/// Silent stub returning `0.0`.
///
/// @param j SpringJoint3D handle (ignored).
///
/// @return `0.0`.
double rt_spring_joint3d_get_damping(void *j) {
    (void)j;
    return 0.0;
}

/// @brief Stub for `SpringJoint3D.SetDamping` — adjust the damping
///        coefficient. Higher = quicker oscillation decay (overdamped at
///        very high values).
///
/// Silent no-op stub.
///
/// @param j SpringJoint3D handle (ignored).
/// @param d New damping coefficient (ignored).
void rt_spring_joint3d_set_damping(void *j, double d) {
    (void)j;
    (void)d;
}

/// @brief Stub for `SpringJoint3D.RestLength` — get the spring's rest
///        length (the separation at which net force is zero).
///
/// Silent stub returning `0.0`.
///
/// @param j SpringJoint3D handle (ignored).
///
/// @return `0.0`.
double rt_spring_joint3d_get_rest_length(void *j) {
    (void)j;
    return 0.0;
}

/// @brief Stub for `HingeJoint3D.New` — would normally create an anchor
///        constraint that allows relative angular motion around `axis`.
///
/// Trapping stub.
///
/// @param a      First Body3D handle (ignored before trapping).
/// @param b      Second Body3D handle (ignored before trapping).
/// @param anchor World-space hinge anchor (ignored before trapping).
/// @param axis   World-space hinge axis (ignored before trapping).
///
/// @return Never returns normally.
void *rt_hinge_joint3d_new(void *a, void *b, void *anchor, void *axis) {
    (void)a;
    (void)b;
    (void)anchor;
    (void)axis;
    rt_graphics_unavailable_("HingeJoint3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Silently discard a hinge-motor configuration in a graphics-disabled build.
///
/// @param joint           HingeJoint3D handle (ignored).
/// @param enabled         Non-zero to enable the motor (ignored).
/// @param target_velocity Requested angular velocity (ignored).
/// @param max_impulse     Maximum motor impulse (ignored).
void rt_hinge_joint3d_set_motor(void *joint,
                                int8_t enabled,
                                double target_velocity,
                                double max_impulse) {
    (void)joint;
    (void)enabled;
    (void)target_velocity;
    (void)max_impulse;
}

/// @brief Return the fallback relative angle for a graphics-disabled hinge joint.
///
/// @param joint HingeJoint3D handle (ignored).
///
/// @return `0.0` radians.
double rt_hinge_joint3d_get_angle(void *joint) {
    (void)joint;
    return 0.0;
}

/// @brief Silently discard angular limits for a graphics-disabled hinge joint.
///
/// @param joint     HingeJoint3D handle (ignored).
/// @param min_angle Minimum permitted relative angle in radians (ignored).
/// @param max_angle Maximum permitted relative angle in radians (ignored).
void rt_hinge_joint3d_set_limits(void *joint, double min_angle, double max_angle) {
    (void)joint;
    (void)min_angle;
    (void)max_angle;
}

/// @brief Stub for `RopeJoint3D.New` — would normally create a maximum-distance
///        constraint between bodies `a` and `b`.
///
/// Trapping stub.
///
/// @param a          First Body3D handle (ignored before trapping).
/// @param b          Second Body3D handle (ignored before trapping).
/// @param max_length Maximum separation permitted by the rope (ignored before trapping).
///
/// @return Never returns normally.
void *rt_rope_joint3d_new(void *a, void *b, double max_length) {
    (void)a;
    (void)b;
    (void)max_length;
    rt_graphics_unavailable_("RopeJoint3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `RopeJoint3D.MaxLength`.
///
/// @param j RopeJoint3D handle (ignored).
///
/// @return `0.0`, the fallback maximum length.
double rt_rope_joint3d_get_max_length(void *j) {
    (void)j;
    return 0.0;
}

/// @brief Stub for `RopeJoint3D.MaxLength` setter.
///
/// @param j          RopeJoint3D handle (ignored).
/// @param max_length Requested maximum separation (ignored).
void rt_rope_joint3d_set_max_length(void *j, double max_length) {
    (void)j;
    (void)max_length;
}

/// @brief Stub for `SixDofJoint3D.New` — would normally create a configurable
///        frame constraint between two bodies.
///
/// Trapping stub.
///
/// @param a       First Body3D handle (ignored before trapping).
/// @param b       Second Body3D handle (ignored before trapping).
/// @param frame_a Constraint frame in the first body's local space (ignored before trapping).
/// @param frame_b Constraint frame in the second body's local space (ignored before trapping).
///
/// @return Never returns normally.
void *rt_sixdof_joint3d_new(void *a, void *b, void *frame_a, void *frame_b) {
    (void)a;
    (void)b;
    (void)frame_a;
    (void)frame_b;
    rt_graphics_unavailable_("SixDofJoint3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SixDofJoint3D.SetLinearLimits`.
///
/// @param j   SixDofJoint3D handle (ignored).
/// @param min Vec3 lower translation limits (ignored).
/// @param max Vec3 upper translation limits (ignored).
void rt_sixdof_joint3d_set_linear_limits(void *j, void *min, void *max) {
    (void)j;
    (void)min;
    (void)max;
}

/// @brief Stub for `SixDofJoint3D.SetAngularLimits`.
///
/// @param j   SixDofJoint3D handle (ignored).
/// @param min Vec3 lower angular limits in radians (ignored).
/// @param max Vec3 upper angular limits in radians (ignored).
void rt_sixdof_joint3d_set_angular_limits(void *j, void *min, void *max) {
    (void)j;
    (void)min;
    (void)max;
}

/// @brief Silently discard a linear-motor configuration for a graphics-disabled six-DOF joint.
///
/// @param j           SixDofJoint3D handle (ignored).
/// @param enabled     Non-zero to enable the motor (ignored).
/// @param velocity    Vec3 target linear velocity (ignored).
/// @param max_impulse Maximum motor impulse (ignored).
void rt_sixdof_joint3d_set_linear_motor(void *j,
                                        int8_t enabled,
                                        void *velocity,
                                        double max_impulse) {
    (void)j;
    (void)enabled;
    (void)velocity;
    (void)max_impulse;
}

/* Collider3D stubs */

/// @brief Stub for `Collider3D.NewBox` — would normally allocate an
///        axis-aligned box collider with the given half-extents (so the
///        full size is `2 * h` along each axis). Centered at the body's
///        local origin.
///
/// Silent stub returning NULL.
///
/// @param hx Half-extent along X (ignored).
/// @param hy Half-extent along Y (ignored).
/// @param hz Half-extent along Z (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_box(double hx, double hy, double hz) {
    (void)hx;
    (void)hy;
    (void)hz;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewBox: graphics support not compiled in", NULL);
}

/// @brief Stub for `Collider3D.NewSphere` — would normally allocate a
///        sphere collider with the given radius. Cheapest narrow-phase
///        shape (analytic sphere-vs-sphere is O(1)).
///
/// Silent stub returning NULL.
///
/// @param radius Sphere radius in world units (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_sphere(double radius) {
    (void)radius;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewSphere: graphics support not compiled in", NULL);
}

/// @brief Stub for `Collider3D.NewCapsule` — would normally allocate a
///        Y-axis-aligned capsule collider (cylinder with hemispherical
///        end-caps). Total height along Y is `height`, including caps.
///        Used for character controllers and humanoid bodies.
///
/// Silent stub returning NULL.
///
/// @param radius Capsule cross-sectional radius (ignored).
/// @param height Total capsule height including caps (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_capsule(double radius, double height) {
    (void)radius;
    (void)height;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewCapsule: graphics support not compiled in", NULL);
}

/// @brief Stub for `Collider3D.NewConvexHull` — would normally compute
///        the convex hull of the given Mesh3D's vertex set and use it as
///        collision geometry. Convex hulls are cheap to test against
///        (GJK / EPA narrow-phase).
///
/// Silent stub returning NULL.
///
/// @param mesh Source Mesh3D handle (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_convex_hull(void *mesh) {
    (void)mesh;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewConvexHull: graphics support not compiled in",
                                  NULL);
}

/// @brief Stub for `Collider3D.NewMesh` — would normally use the given
///        Mesh3D's triangles directly as collision geometry. Triangle-mesh
///        colliders are static-only (no inertia tensor); typically used
///        for level geometry.
///
/// Silent stub returning NULL.
///
/// @param mesh Source Mesh3D handle (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_mesh(void *mesh) {
    (void)mesh;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewMesh: graphics support not compiled in", NULL);
}

/// @brief Stub for `Collider3D.NewHeightfield` — would normally allocate
///        a heightfield collider sampling the given heightmap (Pixels
///        surface; red channel = height) at world dimensions
///        `(sx, sy, sz)`. Static-only; used for terrain.
///
/// Silent stub returning NULL.
///
/// @param heightmap Pixels handle providing height samples (ignored).
/// @param sx        World extent along X (ignored).
/// @param sy        Vertical scale (height range) (ignored).
/// @param sz        World extent along Z (ignored).
///
/// @return `NULL`.
void *rt_collider3d_new_heightfield(void *heightmap, double sx, double sy, double sz) {
    (void)heightmap;
    (void)sx;
    (void)sy;
    (void)sz;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewHeightfield: graphics support not compiled in",
                                  NULL);
}

/// @brief Stub for `Collider3D.NewCompound` — would normally allocate
///        an empty compound collider. Children added via `AddChild` are
///        each tested individually during narrow-phase but share a
///        single body-vs-collider attachment. Used for non-convex
///        collision shapes.
///
/// Silent stub returning NULL.
///
/// @return `NULL`.
void *rt_collider3d_new_compound(void) {
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.NewCompound: graphics support not compiled in", NULL);
}

/// @brief Stub for `Collider3D.AddChild` — attach a child collider to a
///        compound parent at the given local-space transform. The child
///        moves rigidly with the compound during simulation.
///
/// Silent no-op stub.
///
/// @param compound        Compound Collider3D handle (ignored).
/// @param child           Child Collider3D handle (ignored).
/// @param local_transform Transform3D positioning child within compound (ignored).
void rt_collider3d_add_child(void *compound, void *child, void *local_transform) {
    (void)compound;
    (void)child;
    (void)local_transform;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Collider3D.AddChild: graphics support not compiled in");
}

/// @brief Stub for `Collider3D.Type` — get the shape type of the collider:
///        0=Box, 1=Sphere, 2=Capsule, 3=Hull, 4=Mesh, 5=Heightfield, 6=Compound.
///
/// Silent stub returning `-1` (invalid / no collider).
///
/// @param collider Collider3D handle (ignored).
///
/// @return `-1`.
int64_t rt_collider3d_get_type(void *collider) {
    (void)collider;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Collider3D.Type: graphics support not compiled in", -1);
}

/// @brief Stub for `Collider3D.LocalBoundsMin` — get the min corner of
///        the collider's local-space AABB as a Vec3 (before world transform).
///
/// Silent stub returning NULL.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `NULL`.
void *rt_collider3d_get_local_bounds_min(void *collider) {
    (void)collider;
    return NULL;
}

/// @brief Stub for `Collider3D.LocalBoundsMax` — get the max corner of
///        the collider's local-space AABB as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `NULL`.
void *rt_collider3d_get_local_bounds_max(void *collider) {
    (void)collider;
    return NULL;
}

/// @brief Stub for the raw out-parameter form of `Collider3D` local-bounds
///        query. Used by C callers that want to avoid Vec3 wrapper allocation.
///
/// Silent stub: zeros both out-parameters when non-NULL.
///
/// @param collider Collider3D handle (ignored).
/// @param min_out  `double[3]` to receive the local-AABB min, or NULL (zeroed if non-NULL).
/// @param max_out  `double[3]` to receive the local-AABB max, or NULL (zeroed if non-NULL).
void rt_collider3d_get_local_bounds_raw(void *collider, double *min_out, double *max_out) {
    (void)collider;
    if (min_out) {
        min_out[0] = min_out[1] = min_out[2] = 0.0;
    }
    if (max_out) {
        max_out[0] = max_out[1] = max_out[2] = 0.0;
    }
}

/// @brief Stub for the raw form of `Collider3D.WorldAABB(transform)` —
///        would normally apply the body's `(position, rotation, scale)` to
///        the local bounds and write the resulting world AABB to out-params.
///
/// Silent stub: zeros both out-parameters when non-NULL.
///
/// @param collider Collider3D handle (ignored).
/// @param position `double[3]` body world position (ignored).
/// @param rotation `double[4]` body orientation quaternion (ignored).
/// @param scale    `double[3]` body local scale (ignored).
/// @param min_out  `double[3]` receives world-AABB min, or NULL (zeroed if non-NULL).
/// @param max_out  `double[3]` receives world-AABB max, or NULL (zeroed if non-NULL).
void rt_collider3d_compute_world_aabb_raw(void *collider,
                                          const double *position,
                                          const double *rotation,
                                          const double *scale,
                                          double *min_out,
                                          double *max_out) {
    (void)collider;
    (void)position;
    (void)rotation;
    (void)scale;
    if (min_out) {
        min_out[0] = min_out[1] = min_out[2] = 0.0;
    }
    if (max_out) {
        max_out[0] = max_out[1] = max_out[2] = 0.0;
    }
}

/// @brief Stub for `Collider3D.IsStaticOnly` — true for collider shapes
///        that can only be attached to static (non-moving) bodies — e.g.
///        triangle meshes and heightfields, which lack inertia tensors.
///
/// Silent stub returning `0`.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `0`.
int8_t rt_collider3d_is_static_only_raw(void *collider) {
    (void)collider;
    return 0;
}

/// @brief Stub for `Collider3D.BoxHalfExtents` raw query — for box
///        colliders, write the half-extents (so full size is `2 * he` along
///        each axis) to `half_extents_out[0..2]`.
///
/// Silent stub: zeros the out-parameter when non-NULL. Meaningless for
/// non-box colliders even in the real implementation.
///
/// @param collider          Collider3D handle (ignored).
/// @param half_extents_out  `double[3]` receives half-extents, or NULL.
void rt_collider3d_get_box_half_extents_raw(void *collider, double *half_extents_out) {
    (void)collider;
    if (half_extents_out) {
        half_extents_out[0] = half_extents_out[1] = half_extents_out[2] = 0.0;
    }
}

/// @brief Stub for `Collider3D.Radius` raw query — for sphere and capsule
///        colliders, get the cross-sectional radius.
///
/// Silent stub returning `0.0`. Meaningless for non-radial shapes even in
/// the real implementation.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `0.0`.
double rt_collider3d_get_radius_raw(void *collider) {
    (void)collider;
    return 0.0;
}

/// @brief Stub for `Collider3D.Height` raw query — for capsule colliders,
///        get the total capsule height including both hemispherical caps.
///
/// Silent stub returning `0.0`.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `0.0`.
double rt_collider3d_get_height_raw(void *collider) {
    (void)collider;
    return 0.0;
}

/// @brief Stub for `Collider3D.Mesh` raw query — for hull and triangle-mesh
///        colliders, get the underlying Mesh3D used as collision geometry.
///
/// Silent stub returning NULL.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `NULL`.
void *rt_collider3d_get_mesh_raw(void *collider) {
    (void)collider;
    return NULL;
}

/// @brief Stub for `Collider3D.ChildCount` raw query — for compound
///        colliders, the number of attached child colliders.
///
/// Silent stub returning `0`.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `0`.
int64_t rt_collider3d_get_child_count_raw(void *collider) {
    (void)collider;
    return 0;
}

/// @brief Stub for `Collider3D.Child(i)` raw query — for compound
///        colliders, get the `i`th child collider.
///
/// Silent stub returning NULL.
///
/// @param collider Compound Collider3D handle (ignored).
/// @param index    Child index, 0..ChildCount-1 (ignored).
///
/// @return `NULL`.
void *rt_collider3d_get_child_raw(void *collider, int64_t index) {
    (void)collider;
    (void)index;
    return NULL;
}

/// @brief Stub for `Collider3D.ChildTransform(i)` raw query — for compound
///        colliders, write the `i`th child's local-space transform
///        (relative to the compound parent) to the out-parameters.
///
/// Silent stub: writes identity transform (zero translation, identity
/// quaternion, unit scale) when out-params are non-NULL. The identity is
/// meaningful — defensive callers can use the result without further checks.
///
/// @param compound      Compound Collider3D handle (ignored).
/// @param index         Child index (ignored).
/// @param position_out  `double[3]` receives local position; defaults to zero.
/// @param rotation_out  `double[4]` receives local rotation quaternion `(x,y,z,w)`;
///                      defaults to identity `(0, 0, 0, 1)`.
/// @param scale_out     `double[3]` receives local scale; defaults to `(1, 1, 1)`.
void rt_collider3d_get_child_transform_raw(
    void *compound, int64_t index, double *position_out, double *rotation_out, double *scale_out) {
    (void)compound;
    (void)index;
    if (position_out) {
        position_out[0] = position_out[1] = position_out[2] = 0.0;
    }
    if (rotation_out) {
        rotation_out[0] = rotation_out[1] = rotation_out[2] = 0.0;
        rotation_out[3] = 1.0;
    }
    if (scale_out) {
        scale_out[0] = scale_out[1] = scale_out[2] = 1.0;
    }
}

/// @brief Silent stub for `Collider3D.set_Friction` — no-op.
///
/// @param collider Collider3D handle (ignored).
/// @param friction Friction override to assign (ignored).
void rt_collider3d_set_friction(void *collider, double friction) {
    (void)collider;
    (void)friction;
}

/// @brief Silent stub for `Collider3D.get_Friction` — no-op; returns -1.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `-1.0`, indicating that the collider has no friction override.
double rt_collider3d_get_friction(void *collider) {
    (void)collider;
    return -1.0;
}

/// @brief Silent stub for `Collider3D.set_Restitution` — no-op.
///
/// @param collider    Collider3D handle (ignored).
/// @param restitution Restitution override to assign (ignored).
void rt_collider3d_set_restitution(void *collider, double restitution) {
    (void)collider;
    (void)restitution;
}

/// @brief Silent stub for `Collider3D.get_Restitution` — no-op; returns -1.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `-1.0`, indicating that the collider has no restitution override.
double rt_collider3d_get_restitution(void *collider) {
    (void)collider;
    return -1.0;
}

/// @brief Silent stub for `Collider3D.set_SurfaceType` — no-op.
///
/// @param collider     Collider3D handle (ignored).
/// @param surface_type Application-defined surface type identifier (ignored).
void rt_collider3d_set_surface_type(void *collider, int64_t surface_type) {
    (void)collider;
    (void)surface_type;
}

/// @brief Silent stub for `Collider3D.get_SurfaceType` — no-op; returns 0.
///
/// @param collider Collider3D handle (ignored).
///
/// @return `0`, the default surface type.
int64_t rt_collider3d_get_surface_type(void *collider) {
    (void)collider;
    return 0;
}

/// @brief Silent stub for the effective-friction resolver — no-op; body value.
///
/// @param collider      Collider3D handle whose optional override would be consulted (ignored).
/// @param body_friction Owning body's friction coefficient.
///
/// @return `body_friction` unchanged.
double rt_collider3d_effective_friction_raw(void *collider, double body_friction) {
    (void)collider;
    return body_friction;
}

/// @brief Silent stub for the effective-restitution resolver — no-op; body value.
///
/// @param collider         Collider3D handle whose optional override would be consulted (ignored).
/// @param body_restitution Owning body's restitution coefficient.
///
/// @return `body_restitution` unchanged.
double rt_collider3d_effective_restitution_raw(void *collider, double body_restitution) {
    (void)collider;
    return body_restitution;
}

/// @brief Silent stub for `Physics3DBody.set_UserData` — no-op.
///
/// @param body  Body3D handle (ignored).
/// @param value Application-defined integer payload (ignored).
void rt_body3d_set_user_data(void *body, int64_t value) {
    (void)body;
    (void)value;
}

/// @brief Silent stub for `Physics3DBody.get_UserData` — no-op; returns 0.
///
/// @param body Body3D handle (ignored).
///
/// @return `0`, the default user payload.
int64_t rt_body3d_get_user_data(void *body) {
    (void)body;
    return 0;
}

/// @brief Silent stub for `PhysicsHit3D.get_SurfaceType` — no-op; returns 0.
///
/// @param hit Physics3DHit handle (ignored).
///
/// @return `0`, the default surface type.
int64_t rt_physics_hit3d_get_surface_type(void *hit) {
    (void)hit;
    return 0;
}

/// @brief Silent stub for `CollisionEvent3D.get_SurfaceTypeA` — no-op; returns 0.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0`, the default surface type for body A.
int64_t rt_collision_event3d_get_surface_type_a(void *event) {
    (void)event;
    return 0;
}

/// @brief Silent stub for `CollisionEvent3D.get_SurfaceTypeB` — no-op; returns 0.
///
/// @param event CollisionEvent3D handle (ignored).
///
/// @return `0`, the default surface type for body B.
int64_t rt_collision_event3d_get_surface_type_b(void *event) {
    (void)event;
    return 0;
}

/// @brief Silent stub for the internal heightfield hole-mask installer — no-op; returns 0.
///
/// @param collider Heightfield Collider3D handle (ignored).
/// @param mask     Per-cell hole mask (ignored).
/// @param cells_x  Number of cells along the local X axis (ignored).
/// @param cells_z  Number of cells along the local Z axis (ignored).
///
/// @return `0`, indicating that the mask was not installed.
int8_t rt_collider3d_heightfield_set_holes_raw(void *collider,
                                               const uint8_t *mask,
                                               int32_t cells_x,
                                               int32_t cells_z) {
    (void)collider;
    (void)mask;
    (void)cells_x;
    (void)cells_z;
    return 0;
}

/// @brief Return the flat fallback sample for a graphics-disabled heightfield collider.
///
/// Would normally sample the surface height and normal at local-space
/// coordinates `(local_x, local_z)`. The stub writes height `0` and normal
/// `+Y` when the corresponding output buffers are present.
///
/// @param collider   Heightfield Collider3D handle (ignored).
/// @param local_x    Local-space X coordinate (ignored).
/// @param local_z    Local-space Z coordinate (ignored).
/// @param height_out Optional scalar output receiving `0.0`.
/// @param normal_out Optional `double[3]` output receiving `(0, 1, 0)`.
///
/// @return `0`, indicating that no in-bounds heightfield sample was found.
int8_t rt_collider3d_sample_heightfield_raw(
    void *collider, double local_x, double local_z, double *height_out, double *normal_out) {
    (void)collider;
    (void)local_x;
    (void)local_z;
    if (height_out)
        *height_out = 0.0;
    if (normal_out) {
        normal_out[0] = 0.0;
        normal_out[1] = 1.0;
        normal_out[2] = 0.0;
    }
    return 0;
}

/* Physics3D Body stubs */

/// @brief Stub for `Body3D.New` — would normally allocate a rigid body
///        with the given mass and no collider attached. Assign `Collider`
///        to bind a Collider3D shape, or use the convenience constructors
///        below (`NewAABB`, `NewSphere`, `NewCapsule`) that pre-attach a
///        primitive shape.
///
/// Silent stub returning NULL.
///
/// @param mass Body mass in kilograms; `0` for static (infinite mass) (ignored).
///
/// @return `NULL`.
void *rt_body3d_new(double mass) {
    (void)mass;
    return NULL;
}

/// @brief Stub for `Body3D.NewAABB` — convenience constructor that
///        creates a body with an axis-aligned box collider of half-extents
///        `(hx, hy, hz)` and the given mass. Pre-attaches the collider so
///        the body is ready to add to a Physics3DWorld.
///
/// Silent stub returning NULL.
///
/// @param hx   Half-extent along X (ignored).
/// @param hy   Half-extent along Y (ignored).
/// @param hz   Half-extent along Z (ignored).
/// @param mass Body mass in kilograms (ignored).
///
/// @return `NULL`.
void *rt_body3d_new_aabb(double hx, double hy, double hz, double mass) {
    (void)hx;
    (void)hy;
    (void)hz;
    (void)mass;
    return NULL;
}

/// @brief Stub for `Body3D.NewSphere` — convenience constructor that
///        creates a body with a sphere collider of the given radius and
///        the given mass. Cheapest pairwise narrow-phase shape.
///
/// Silent stub returning NULL.
///
/// @param radius Sphere radius (ignored).
/// @param mass   Body mass in kilograms (ignored).
///
/// @return `NULL`.
void *rt_body3d_new_sphere(double radius, double mass) {
    (void)radius;
    (void)mass;
    return NULL;
}

/// @brief Stub for `Body3D.NewCapsule` — convenience constructor that
///        creates a body with a Y-axis capsule collider and the given
///        mass. Standard shape for character controllers.
///
/// Silent stub returning NULL.
///
/// @param radius Capsule cross-sectional radius (ignored).
/// @param height Total capsule height including caps (ignored).
/// @param mass   Body mass in kilograms (ignored).
///
/// @return `NULL`.
void *rt_body3d_new_capsule(double radius, double height, double mass) {
    (void)radius;
    (void)height;
    (void)mass;
    return NULL;
}

/// @brief Stub for `Body3D.Collider` setter — attach a Collider3D shape to
///        this body. A body without a collider participates in the
///        simulation (gets integrated) but cannot generate contacts and
///        will pass through everything.
///
/// Silent no-op stub.
///
/// @param o        Body3D handle (ignored).
/// @param collider Collider3D handle, or NULL to detach (ignored).
void rt_body3d_set_collider(void *o, void *collider) {
    (void)o;
    (void)collider;
}

/// @brief Stub for `Body3D.Collider` — get the attached Collider3D, or
///        NULL if the body has no shape.
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_collider(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Body3D.SetPosition` — teleport the body to the
///        given world-space position. Use for spawn / respawn / portals
///        — for normal motion let the simulation integrate forces.
///
/// Silent no-op stub. Wakes the body if it was sleeping.
///
/// @param o Body3D handle (ignored).
/// @param x World x (ignored).
/// @param y World y (ignored).
/// @param z World z (ignored).
void rt_body3d_set_position(void *o, double x, double y, double z) {
    (void)o;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `Body3D.Position` — get the body's current world-
///        space position as a Vec3 (post-integration this tick).
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_position(void *o) {
    (void)o;
    return NULL;
}

/// @brief Silently discard a scale update for a graphics-disabled rigid body.
///
/// @param o Body3D handle (ignored).
/// @param x Scale factor along X (ignored).
/// @param y Scale factor along Y (ignored).
/// @param z Scale factor along Z (ignored).
void rt_body3d_set_scale(void *o, double x, double y, double z) {
    (void)o;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Return the unavailable scale vector for a graphics-disabled rigid body.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_scale(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Body3D.SetOrientation` — set the body's rotation
///        from a Quaternion handle. As with `SetPosition`, this is
///        teleportation and wakes the body.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param q Quaternion handle (ignored).
void rt_body3d_set_orientation(void *o, void *q) {
    (void)o;
    (void)q;
}

/// @brief Stub for `Body3D.Orientation` — get the body's current
///        rotation as a Quaternion handle.
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_orientation(void *o) {
    (void)o;
    return NULL;
}

/// @brief Write the fallback pose for a graphics-disabled rigid body.
///
/// The fallback is zero translation, identity quaternion rotation, and unit
/// scale. Each output buffer is optional.
///
/// @param o            Body3D handle (ignored).
/// @param position_out Optional `double[3]` output receiving `(0, 0, 0)`.
/// @param rotation_out Optional `double[4]` output receiving `(0, 0, 0, 1)`.
/// @param scale_out    Optional `double[3]` output receiving `(1, 1, 1)`.
void rt_body3d_get_pose_raw(void *o,
                            double *position_out,
                            double *rotation_out,
                            double *scale_out) {
    (void)o;
    if (position_out) {
        position_out[0] = 0.0;
        position_out[1] = 0.0;
        position_out[2] = 0.0;
    }
    if (rotation_out) {
        rotation_out[0] = 0.0;
        rotation_out[1] = 0.0;
        rotation_out[2] = 0.0;
        rotation_out[3] = 1.0;
    }
    if (scale_out) {
        scale_out[0] = 1.0;
        scale_out[1] = 1.0;
        scale_out[2] = 1.0;
    }
}

/// @brief Stub for `Body3D.SetVelocity` — set the body's linear velocity
///        directly. Useful for character locomotion (where solver-driven
///        forces feel mushy) and one-shot launches.
///
/// Silent no-op stub.
///
/// @param o  Body3D handle (ignored).
/// @param vx Velocity x (ignored).
/// @param vy Velocity y (ignored).
/// @param vz Velocity z (ignored).
void rt_body3d_set_velocity(void *o, double vx, double vy, double vz) {
    (void)o;
    (void)vx;
    (void)vy;
    (void)vz;
}

/// @brief Stub for `Body3D.Velocity` — get the body's current linear
///        velocity as a Vec3 (world units / second).
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_velocity(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Body3D.SetAngularVelocity` — set the body's angular
///        velocity directly. Each axis is rotation rate in radians /
///        second around that axis.
///
/// Silent no-op stub.
///
/// @param o  Body3D handle (ignored).
/// @param wx Angular velocity x (ignored).
/// @param wy Angular velocity y (ignored).
/// @param wz Angular velocity z (ignored).
void rt_body3d_set_angular_velocity(void *o, double wx, double wy, double wz) {
    (void)o;
    (void)wx;
    (void)wy;
    (void)wz;
}

/// @brief Stub for `Body3D.AngularVelocity` — get the body's current
///        angular velocity as a Vec3 (radians per second around each axis).
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_angular_velocity(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Body3D.ApplyForce` — apply a continuous force this
///        tick, integrated by the simulation step over `dt`.
///
/// Silent no-op stub. Force is in world-space; for repeated forces (gravity,
/// thrust) call once per tick rather than once per frame.
///
/// @param o  Body3D handle (ignored).
/// @param fx Force x component (ignored).
/// @param fy Force y component (ignored).
/// @param fz Force z component (ignored).
void rt_body3d_apply_force(void *o, double fx, double fy, double fz) {
    (void)o;
    (void)fx;
    (void)fy;
    (void)fz;
}

/// @brief Silently discard a force applied at a world-space point.
///
/// @param o  Body3D handle (ignored).
/// @param fx Force component along X (ignored).
/// @param fy Force component along Y (ignored).
/// @param fz Force component along Z (ignored).
/// @param px Application point X coordinate (ignored).
/// @param py Application point Y coordinate (ignored).
/// @param pz Application point Z coordinate (ignored).
void rt_body3d_apply_force_at_point(
    void *o, double fx, double fy, double fz, double px, double py, double pz) {
    (void)o;
    (void)fx;
    (void)fy;
    (void)fz;
    (void)px;
    (void)py;
    (void)pz;
}

/// @brief Stub for `Body3D.ApplyImpulse` — apply an instantaneous
///        velocity change (no `dt` integration). Use for one-shot events:
///        jumps, knockbacks, recoil.
///
/// Silent no-op stub.
///
/// @param o  Body3D handle (ignored).
/// @param ix Impulse x component (ignored).
/// @param iy Impulse y component (ignored).
/// @param iz Impulse z component (ignored).
void rt_body3d_apply_impulse(void *o, double ix, double iy, double iz) {
    (void)o;
    (void)ix;
    (void)iy;
    (void)iz;
}

/// @brief Silently discard an impulse applied at a world-space point.
///
/// @param o  Body3D handle (ignored).
/// @param ix Impulse component along X (ignored).
/// @param iy Impulse component along Y (ignored).
/// @param iz Impulse component along Z (ignored).
/// @param px Application point X coordinate (ignored).
/// @param py Application point Y coordinate (ignored).
/// @param pz Application point Z coordinate (ignored).
void rt_body3d_apply_impulse_at_point(
    void *o, double ix, double iy, double iz, double px, double py, double pz) {
    (void)o;
    (void)ix;
    (void)iy;
    (void)iz;
    (void)px;
    (void)py;
    (void)pz;
}

/// @brief Stub for `Body3D.ApplyTorque` — apply a continuous torque this
///        tick, integrated over `dt`. World-space.
///
/// Silent no-op stub.
///
/// @param o  Body3D handle (ignored).
/// @param tx Torque x component (ignored).
/// @param ty Torque y component (ignored).
/// @param tz Torque z component (ignored).
void rt_body3d_apply_torque(void *o, double tx, double ty, double tz) {
    (void)o;
    (void)tx;
    (void)ty;
    (void)tz;
}

/// @brief Stub for `Body3D.ApplyAngularImpulse` — apply an instantaneous
///        angular velocity change (no `dt` integration). Use for spin-up
///        events.
///
/// Silent no-op stub.
///
/// @param o  Body3D handle (ignored).
/// @param ix Angular impulse x component (ignored).
/// @param iy Angular impulse y component (ignored).
/// @param iz Angular impulse z component (ignored).
void rt_body3d_apply_angular_impulse(void *o, double ix, double iy, double iz) {
    (void)o;
    (void)ix;
    (void)iy;
    (void)iz;
}

/// @brief Stub for `Body3D.SetRestitution` — coefficient of restitution
///        (bounciness). 0 = inelastic, 1 = perfectly elastic.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param r Restitution, 0..1 (ignored).
void rt_body3d_set_restitution(void *o, double r) {
    (void)o;
    (void)r;
}

/// @brief Stub for `Body3D.Restitution` — get the current restitution
///        coefficient.
///
/// Silent stub returning `0.0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0.0`.
double rt_body3d_get_restitution(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Body3D.SetFriction` — coefficient of friction
///        applied at contact points. 0 = frictionless, 1 = high friction.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param f Friction, 0..1+ (ignored).
void rt_body3d_set_friction(void *o, double f) {
    (void)o;
    (void)f;
}

/// @brief Stub for `Body3D.Friction` — get the current friction
///        coefficient.
///
/// Silent stub returning `0.0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0.0`.
double rt_body3d_get_friction(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Body3D.SetLinearDamping` — fraction of linear velocity
///        bled off per second (0 = no damping, simulates air resistance /
///        viscosity).
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param d Damping per second, 0..1 (ignored).
void rt_body3d_set_linear_damping(void *o, double d) {
    (void)o;
    (void)d;
}

/// @brief Stub for `Body3D.LinearDamping` — get the current linear
///        damping factor.
///
/// Silent stub returning `0.0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0.0`.
double rt_body3d_get_linear_damping(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Body3D.SetAngularDamping` — fraction of angular
///        velocity bled off per second.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param d Damping per second, 0..1 (ignored).
void rt_body3d_set_angular_damping(void *o, double d) {
    (void)o;
    (void)d;
}

/// @brief Stub for `Body3D.AngularDamping` — get the current angular
///        damping factor.
///
/// Silent stub returning `0.0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0.0`.
double rt_body3d_get_angular_damping(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Body3D.SetCollisionLayer` — bitmask of which layer(s)
///        this body belongs to. Pairs only generate contacts when
///        `(BodyA.layer & BodyB.mask) != 0` and vice versa.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param l Layer bitmask (ignored).
void rt_body3d_set_collision_layer(void *o, int64_t l) {
    (void)o;
    (void)l;
}

/// @brief Stub for `Body3D.CollisionLayer` — get the body's collision
///        layer bitmask.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int64_t rt_body3d_get_collision_layer(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.SetCollisionMask` — bitmask of which layers
///        this body collides with. See `SetCollisionLayer` for the pair-
///        evaluation rule.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param m Mask bitmask (ignored).
void rt_body3d_set_collision_mask(void *o, int64_t m) {
    (void)o;
    (void)m;
}

/// @brief Stub for `Body3D.CollisionMask` — get the body's collision
///        mask bitmask.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int64_t rt_body3d_get_collision_mask(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.SetStatic` — when true, the body has infinite
///        mass and never moves; other bodies collide against it but it
///        receives no forces in return. Cheaper than dynamic bodies.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param s Non-zero for static (ignored).
void rt_body3d_set_static(void *o, int8_t s) {
    (void)o;
    (void)s;
}

/// @brief Stub for `Body3D.IsStatic` — true if the body is in static mode.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_is_static(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.SetKinematic` — when true, the body's
///        position/orientation are driven externally (typically by gameplay
///        code or an animation), but it still pushes dynamic bodies during
///        contact. Used for moving platforms and animated characters.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param k Non-zero for kinematic (ignored).
void rt_body3d_set_kinematic(void *o, int8_t k) {
    (void)o;
    (void)k;
}

/// @brief Stub for `Body3D.IsKinematic` — true if the body is in
///        kinematic mode.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_is_kinematic(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.SetTrigger` — when true, the body becomes a
///        sensor: it generates collision events but exchanges no impulses
///        with other bodies. Used for AOE volumes, item pickups, kill
///        floors.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
/// @param t Non-zero to make this body a trigger (ignored).
void rt_body3d_set_trigger(void *o, int8_t t) {
    (void)o;
    (void)t;
}

/// @brief Stub for `Body3D.IsTrigger` — true if this body is in trigger
///        (sensor) mode.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_is_trigger(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.SetCanSleep` — enable or disable the
///        sleep-eligibility flag. Bodies that should never sleep (e.g.
///        the player character) need `can_sleep = 0`; otherwise they may
///        freeze when stationary and stop responding to player input.
///
/// Silent no-op stub.
///
/// @param o         Body3D handle (ignored).
/// @param can_sleep Non-zero to allow this body to enter sleep state (ignored).
void rt_body3d_set_can_sleep(void *o, int8_t can_sleep) {
    (void)o;
    (void)can_sleep;
}

/// @brief Stub for `Body3D.CanSleep` — get the sleep-eligibility flag.
///        Distinct from `IsSleeping`: `CanSleep` is a configuration flag,
///        `IsSleeping` is current state.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_can_sleep(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.IsSleeping` — true while the body is in the
///        sleep state (skipped during simulation to save CPU).
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_is_sleeping(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.Wake` — manually wake a sleeping body. Used
///        when external state changes invalidate the sleep assumption
///        (e.g. teleporting a body, removing supporting geometry).
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
void rt_body3d_wake(void *o) {
    (void)o;
}

/// @brief Stub for `Body3D.Sleep` — manually put a body to sleep. The
///        sleep system would normally do this automatically when the
///        body's velocity has been below threshold for `PH3D_SLEEP_DELAY`
///        seconds.
///
/// Silent no-op stub.
///
/// @param o Body3D handle (ignored).
void rt_body3d_sleep(void *o) {
    (void)o;
}

/// @brief Stub for `Body3D.SetUseCcd` — enable Continuous Collision
///        Detection: fast-moving bodies are advanced in `PH3D_MAX_CCD_
///        SUBSTEPS = 16` substeps to detect tunneling through thin
///        geometry. More expensive than discrete collision.
///
/// Silent no-op stub.
///
/// @param o       Body3D handle (ignored).
/// @param use_ccd Non-zero to enable CCD (ignored).
void rt_body3d_set_use_ccd(void *o, int8_t use_ccd) {
    (void)o;
    (void)use_ccd;
}

/// @brief Stub for `Body3D.UseCcd` — get the CCD flag.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_get_use_ccd(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.IsGrounded` — convenience query: true when
///        the body is in contact with a surface in the gravity-down
///        direction. Updated each simulation step.
///
/// Silent stub returning `0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0`.
int8_t rt_body3d_is_grounded(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Body3D.GroundNormal` — get the surface normal of
///        the ground contact, as a Vec3. Useful for slope traversal.
///
/// Silent stub returning NULL.
///
/// @param o Body3D handle (ignored).
///
/// @return `NULL`.
void *rt_body3d_get_ground_normal(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Body3D.Mass` — get the body's mass in kilograms.
///        Static bodies report `0.0` (representing infinite mass).
///
/// Silent stub returning `0.0`.
///
/// @param o Body3D handle (ignored).
///
/// @return `0.0`.
double rt_body3d_get_mass(void *o) {
    (void)o;
    return 0.0;
}

/* Character3D stubs */

/// @brief Stub for `Character3D.New` — would normally create a kinematic
///        character controller (capsule shape) for player/NPC movement.
///        Distinct from `Body3D` in that it uses slide-and-step movement
///        instead of pure rigid-body integration.
///
/// Silent stub returning NULL.
///
/// @param radius Capsule radius in world units (ignored).
/// @param height Capsule total height (ignored).
/// @param mass   Mass for impulse exchange with dynamic bodies (ignored).
///
/// @return `NULL`.
void *rt_character3d_new(double radius, double height, double mass) {
    (void)radius;
    (void)height;
    (void)mass;
    return NULL;
}

/// @brief Stub for `Character3D.Move` — apply the given velocity for
///        `dt` seconds with slide-and-step collision response. The
///        controller will slide along walls, step up small obstacles
///        (configurable via `SetStepHeight`), and stop at slopes steeper
///        than `SetSlopeLimit`.
///
/// Silent no-op stub.
///
/// @param c  Character3D handle (ignored).
/// @param v  Vec3 desired velocity in world units / second (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_character3d_move(void *c, void *v, double dt) {
    (void)c;
    (void)v;
    (void)dt;
}

/// @brief Stub for `Character3D.SetStepHeight` — maximum vertical
///        obstacle the character can step up onto without jumping.
///        Typical values: 0.3 (humanoid) to 0.5 (heavy mech).
///
/// Silent no-op stub.
///
/// @param c Character3D handle (ignored).
/// @param h Step height in world units (ignored).
void rt_character3d_set_step_height(void *c, double h) {
    (void)c;
    (void)h;
}

/// @brief Stub for `Character3D.StepHeight` — get the configured step
///        height.
///
/// Silent stub returning `0.3` (the default value used in the real
/// implementation, so callers querying this for layout math get a usable
/// answer rather than `0`).
///
/// @param c Character3D handle (ignored).
///
/// @return `0.3`.
double rt_character3d_get_step_height(void *c) {
    (void)c;
    return 0.3;
}

/// @brief Stub for `Character3D.SetSlopeLimit` — maximum slope (in
///        radians) the character can climb. Steeper slopes cause the
///        character to slide back down.
///
/// Silent no-op stub.
///
/// @param c Character3D handle (ignored).
/// @param d Slope limit in radians (ignored).
void rt_character3d_set_slope_limit(void *c, double d) {
    (void)c;
    (void)d;
}

/// @brief Stub for `Character3D.SetWorld` — bind the character to a
///        Physics3DWorld so its movement queries hit the right collision
///        geometry.
///
/// Silent no-op stub.
///
/// @param c Character3D handle (ignored).
/// @param w Physics3DWorld handle (ignored).
void rt_character3d_set_world(void *c, void *w) {
    (void)c;
    (void)w;
}

/// @brief Stub for `Character3D.World` — get the bound Physics3DWorld.
///
/// Silent stub returning NULL.
///
/// @param c Character3D handle (ignored).
///
/// @return `NULL`.
void *rt_character3d_get_world(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `Character3D.IsGrounded` — true when the character
///        is in contact with a walkable surface below. Updated during
///        each `Move` call.
///
/// Silent stub returning `0`.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_is_grounded(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.JustLanded` — single-tick edge: true
///        on the frame the character transitioned from airborne to
///        grounded. Useful for landing FX / SFX.
///
/// Silent stub returning `0`.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_just_landed(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.Position` — get the character's current
///        world-space position as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param c Character3D handle (ignored).
///
/// @return `NULL`.
void *rt_character3d_get_position(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `Character3D.SetPosition` — teleport the character
///        to the given world-space position. Use for spawn / respawn /
///        portals — for normal locomotion use `Move`.
///
/// Silent no-op stub.
///
/// @param c Character3D handle (ignored).
/// @param x World x (ignored).
/// @param y World y (ignored).
/// @param z World z (ignored).
void rt_character3d_set_position(void *c, double x, double y, double z) {
    (void)c;
    (void)x;
    (void)y;
    (void)z;
}

/* Trigger3D stubs */

/// @brief Stub for `Trigger3D.New` — would normally create an axis-
///        aligned trigger volume defined by min corner `(x0, y0, z0)`
///        and max corner `(x1, y1, z1)`. Triggers detect entry/exit but
///        do not exchange impulses with bodies passing through.
///
/// Silent stub returning NULL.
///
/// @param x0 Min corner x (ignored).
/// @param y0 Min corner y (ignored).
/// @param z0 Min corner z (ignored).
/// @param x1 Max corner x (ignored).
/// @param y1 Max corner y (ignored).
/// @param z1 Max corner z (ignored).
///
/// @return `NULL`.
void *rt_trigger3d_new(double x0, double y0, double z0, double x1, double y1, double z1) {
    (void)x0;
    (void)y0;
    (void)z0;
    (void)x1;
    (void)y1;
    (void)z1;
    return NULL;
}

/// @brief Stub for `Trigger3D.Contains` — boolean test for whether the
///        Vec3 point `p` lies inside the trigger volume.
///
/// Silent stub returning `0`.
///
/// @param t Trigger3D handle (ignored).
/// @param p Vec3 query point (ignored).
///
/// @return `0`.
int8_t rt_trigger3d_contains(void *t, void *p) {
    (void)t;
    (void)p;
    return 0;
}

/// @brief Stub for `Trigger3D.Update` — refresh the enter/exit counts
///        for this tick by testing every body in the world against the
///        trigger volume.
///
/// Silent no-op stub.
///
/// @param t Trigger3D handle (ignored).
/// @param w Physics3DWorld handle (ignored).
void rt_trigger3d_update(void *t, void *w) {
    (void)t;
    (void)w;
}

/// @brief Stub for `Trigger3D.EnterCount` — number of bodies that
///        crossed into the trigger volume during the most recent `Update`.
///        Use for one-shot pickups, area transitions.
///
/// Silent stub returning `0`.
///
/// @param t Trigger3D handle (ignored).
///
/// @return `0`.
int64_t rt_trigger3d_get_enter_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for `Trigger3D.ExitCount` — number of bodies that
///        crossed out of the trigger volume during the most recent
///        `Update`.
///
/// Silent stub returning `0`.
///
/// @param t Trigger3D handle (ignored).
///
/// @return `0`.
int64_t rt_trigger3d_get_exit_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for `Trigger3D.SetBounds` — reposition / resize the
///        trigger volume after creation. Useful for triggers that move
///        with a moving platform or scaling AOE.
///
/// Silent no-op stub.
///
/// @param t  Trigger3D handle (ignored).
/// @param x0 New min corner x (ignored).
/// @param y0 New min corner y (ignored).
/// @param z0 New min corner z (ignored).
/// @param x1 New max corner x (ignored).
/// @param y1 New max corner y (ignored).
/// @param z1 New max corner z (ignored).
void rt_trigger3d_set_bounds(
    void *t, double x0, double y0, double z0, double x1, double y1, double z1) {
    (void)t;
    (void)x0;
    (void)y0;
    (void)z0;
    (void)x1;
    (void)y1;
    (void)z1;
}

/* Cloth3D stubs (plan 27) */

/// @brief Stub for `Cloth3D.NewChain` — would allocate a verlet chain.
///
/// Traps: graphics support not compiled in.
///
/// @param segments     Number of simulated chain segments (ignored before trapping).
/// @param total_length End-to-end chain length (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_cloth3d_new_chain(int64_t segments, double total_length) {
    (void)segments;
    (void)total_length;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Cloth3D.NewChain: graphics support not compiled in", NULL);
}

/// @brief Stub for `Cloth3D.NewPatch` — would allocate a verlet patch grid.
///
/// Traps: graphics support not compiled in.
///
/// @param w      Number of grid points or divisions along the patch width (ignored).
/// @param h      Number of grid points or divisions along the patch height (ignored).
/// @param width  Physical patch width (ignored).
/// @param height Physical patch height (ignored).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_cloth3d_new_patch(int64_t w, int64_t h, double width, double height) {
    (void)w;
    (void)h;
    (void)width;
    (void)height;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Cloth3D.NewPatch: graphics support not compiled in", NULL);
}

/// @brief Stub for `Cloth3D.get_Damping`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_damping(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_Damping`. Silent no-op stub.
///
/// @param cloth   Cloth3D handle (ignored).
/// @param damping Velocity damping coefficient (ignored).
void rt_cloth3d_set_damping(void *cloth, double damping) {
    (void)cloth;
    (void)damping;
}

/// @brief Stub for `Cloth3D.get_Iterations`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0`.
int64_t rt_cloth3d_get_iterations(void *cloth) {
    (void)cloth;
    return 0;
}

/// @brief Stub for `Cloth3D.set_Iterations`. Silent no-op stub.
///
/// @param cloth      Cloth3D handle (ignored).
/// @param iterations Constraint-solver iteration count (ignored).
void rt_cloth3d_set_iterations(void *cloth, int64_t iterations) {
    (void)cloth;
    (void)iterations;
}

/// @brief Stub for `Cloth3D.get_GravityScale`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_gravity_scale(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_GravityScale`. Silent no-op stub.
///
/// @param cloth Cloth3D handle (ignored).
/// @param scale Gravity multiplier (ignored).
void rt_cloth3d_set_gravity_scale(void *cloth, double scale) {
    (void)cloth;
    (void)scale;
}

/// @brief Stub for `Cloth3D.get_WindResponse`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_wind_response(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_WindResponse`. Silent no-op stub.
///
/// @param cloth    Cloth3D handle (ignored).
/// @param response Wind-force response coefficient (ignored).
void rt_cloth3d_set_wind_response(void *cloth, double response) {
    (void)cloth;
    (void)response;
}

/// @brief Stub for `Cloth3D.get_PointCount`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0`, because the stub contains no simulated points.
int64_t rt_cloth3d_get_point_count(void *cloth) {
    (void)cloth;
    return 0;
}

/// @brief Stub for `Cloth3D.Pin`. Silent no-op stub returning the handle.
///
/// @param cloth Cloth3D handle.
/// @param index Point index to pin (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_pin(void *cloth, int64_t index) {
    (void)index;
    return cloth;
}

/// @brief Stub for `Cloth3D.AddSphere`. Silent no-op stub returning the handle.
///
/// @param cloth  Cloth3D handle.
/// @param center Vec3 collision-sphere center (ignored).
/// @param radius Collision-sphere radius (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_add_sphere(void *cloth, void *center, double radius) {
    (void)center;
    (void)radius;
    return cloth;
}

/// @brief Stub for `Cloth3D.AddCapsule`. Silent no-op stub returning the handle.
///
/// @param cloth  Cloth3D handle.
/// @param a      Vec3 capsule-axis start (ignored).
/// @param b      Vec3 capsule-axis end (ignored).
/// @param radius Capsule radius (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_add_capsule(void *cloth, void *a, void *b, double radius) {
    (void)a;
    (void)b;
    (void)radius;
    return cloth;
}

/// @brief Stub for `Cloth3D.SetWind`. Silent no-op stub.
///
/// @param cloth     Cloth3D handle (ignored).
/// @param direction Vec3 wind direction (ignored).
/// @param strength  Wind magnitude (ignored).
void rt_cloth3d_set_wind(void *cloth, void *direction, double strength) {
    (void)cloth;
    (void)direction;
    (void)strength;
}

/// @brief Stub for `Cloth3D.GetPoint`. Silent stub returning `NULL`.
///
/// @param cloth Cloth3D handle (ignored).
/// @param index Simulated point index (ignored).
///
/// @return `NULL`.
void *rt_cloth3d_get_point(void *cloth, int64_t index) {
    (void)cloth;
    (void)index;
    return NULL;
}

/// @brief Stub for `Cloth3D.BindMesh`. Silent no-op stub returning the handle.
///
/// @param cloth Cloth3D handle.
/// @param mesh  Mesh3D handle to deform (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_bind_mesh(void *cloth, void *mesh) {
    (void)mesh;
    return cloth;
}

/// @brief Stub for `Cloth3D.BindBoneChain`. Silent no-op stub returning the handle.
///
/// @param cloth     Cloth3D handle.
/// @param animator  NodeAnimator3D or animation-controller handle (ignored).
/// @param root_bone Name of the simulated chain's root bone (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_bind_bone_chain(void *cloth, void *animator, rt_string root_bone) {
    (void)animator;
    (void)root_bone;
    return cloth;
}

/// @brief Stub for `Cloth3D.Step`. Silent no-op stub.
///
/// @param cloth Cloth3D handle (ignored).
/// @param dt    Simulation interval in seconds (ignored).
void rt_cloth3d_step(void *cloth, double dt) {
    (void)cloth;
    (void)dt;
}

/// @brief Stub for `World3D.AddCloth`. Silent no-op stub.
///
/// @param world World3D handle (ignored).
/// @param cloth Cloth3D handle to add (ignored).
void rt_game3d_world_add_cloth(void *world, void *cloth) {
    (void)world;
    (void)cloth;
}

/// @brief Stub for `World3D.RemoveCloth`. Silent no-op stub.
///
/// @param world World3D handle (ignored).
/// @param cloth Cloth3D handle to remove (ignored).
void rt_game3d_world_remove_cloth(void *world, void *cloth) {
    (void)world;
    (void)cloth;
}

/* Physics3DWorld query-config, CCD counters, and traversal probes */

/// @brief Stub for `Physics3DWorld.get_MaxQueryHits`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, the fallback query-result capacity.
int64_t rt_world3d_get_max_query_hits(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.SetMaxQueryHits`. Silent no-op stub.
///
/// @param w        Physics3DWorld handle (ignored).
/// @param max_hits Maximum results to retain per query (ignored).
void rt_world3d_set_max_query_hits(void *w, int64_t max_hits) {
    (void)w;
    (void)max_hits;
}

/// @brief Stub for `Physics3DWorld.get_CcdToiCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no time-of-impact events were processed.
int64_t rt_world3d_get_ccd_toi_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.get_CcdSubstepClampedBodyCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no bodies reached the CCD substep limit.
int64_t rt_world3d_get_ccd_substep_clamped_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.get_LastCcdClampedBodyCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because the most recent nonexistent step clamped no bodies.
int64_t rt_world3d_get_last_ccd_clamped_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.ProbeClearance`. Silent stub returning false.
///
/// @param w        Physics3DWorld handle (ignored).
/// @param position Vec3 capsule or character position (ignored).
/// @param radius   Probe radius (ignored).
/// @param height   Required clearance height (ignored).
/// @param mask     Collision-layer mask (ignored).
///
/// @return `0`, indicating that clearance was not established.
int8_t rt_world3d_probe_clearance(
    void *w, void *position, double radius, double height, int64_t mask) {
    (void)w;
    (void)position;
    (void)radius;
    (void)height;
    (void)mask;
    return 0;
}

/// @brief Stub for `Physics3DWorld.ProbeLedge`. Silent stub returning NULL (no ledge).
///
/// @param w          Physics3DWorld handle (ignored).
/// @param position   Vec3 probe origin (ignored).
/// @param forward    Vec3 horizontal probe direction (ignored).
/// @param max_height Maximum ledge height above the origin (ignored).
/// @param min_height Minimum ledge height above the origin (ignored).
/// @param reach      Forward probe distance (ignored).
/// @param mask       Collision-layer mask (ignored).
///
/// @return `NULL`, indicating that no ledge was found.
void *rt_world3d_probe_ledge(void *w,
                             void *position,
                             void *forward,
                             double max_height,
                             double min_height,
                             double reach,
                             int64_t mask) {
    (void)w;
    (void)position;
    (void)forward;
    (void)max_height;
    (void)min_height;
    (void)reach;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.ProbeVault`. Silent stub returning NULL (no vault).
///
/// @param w          Physics3DWorld handle (ignored).
/// @param position   Vec3 probe origin (ignored).
/// @param forward    Vec3 horizontal probe direction (ignored).
/// @param max_height Maximum vaultable obstacle height (ignored).
/// @param max_depth  Maximum vaultable obstacle depth (ignored).
/// @param reach      Forward probe distance (ignored).
/// @param mask       Collision-layer mask (ignored).
///
/// @return `NULL`, indicating that no vault opportunity was found.
void *rt_world3d_probe_vault(void *w,
                             void *position,
                             void *forward,
                             double max_height,
                             double max_depth,
                             double reach,
                             int64_t mask) {
    (void)w;
    (void)position;
    (void)forward;
    (void)max_height;
    (void)max_depth;
    (void)reach;
    (void)mask;
    return NULL;
}

/* Character3D configuration and state accessors */

/// @brief Stub for `Character3D.TrySetHeight`. Silent stub returning false.
///
/// @param c      Character3D handle (ignored).
/// @param height Requested capsule height (ignored).
///
/// @return `0`, indicating that the resize could not be applied.
int8_t rt_character3d_try_set_height(void *c, double height) {
    (void)c;
    (void)height;
    return 0;
}

/// @brief Stub for `Character3D.set_Height`. Silent no-op stub.
///
/// @param c      Character3D handle (ignored).
/// @param height Requested capsule height (ignored).
void rt_character3d_set_height(void *c, double height) {
    (void)c;
    (void)height;
}

/// @brief Stub for `Character3D.get_Height`. Silent stub returning 0.
///
/// @param c Character3D handle (ignored).
///
/// @return `0.0`.
double rt_character3d_get_height(void *c) {
    (void)c;
    return 0.0;
}

/// @brief Stub for `Character3D.set_PushStrength`. Silent no-op stub.
///
/// @param c        Character3D handle (ignored).
/// @param strength Impulse or force multiplier for pushed dynamic bodies (ignored).
void rt_character3d_set_push_strength(void *c, double strength) {
    (void)c;
    (void)strength;
}

/// @brief Stub for `Character3D.get_PushStrength`. Silent stub returning 0.
///
/// @param c Character3D handle (ignored).
///
/// @return `0.0`.
double rt_character3d_get_push_strength(void *c) {
    (void)c;
    return 0.0;
}

/// @brief Stub for `Character3D.set_CollideDynamic`. Silent no-op stub.
///
/// @param c       Character3D handle (ignored).
/// @param enabled Non-zero to collide with dynamic rigid bodies (ignored).
void rt_character3d_set_collide_dynamic(void *c, int8_t enabled) {
    (void)c;
    (void)enabled;
}

/// @brief Stub for `Character3D.get_CollideDynamic`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_get_collide_dynamic(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.set_RidePlatforms`. Silent no-op stub.
///
/// @param c       Character3D handle (ignored).
/// @param enabled Non-zero to inherit moving-platform displacement (ignored).
void rt_character3d_set_ride_platforms(void *c, int8_t enabled) {
    (void)c;
    (void)enabled;
}

/// @brief Stub for `Character3D.get_RidePlatforms`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_get_ride_platforms(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.IsSliding`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`, indicating that the character is not sliding.
int8_t rt_character3d_is_sliding(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.GetGroundBody`. Silent stub returning NULL.
///
/// @param c Character3D handle (ignored).
///
/// @return `NULL`.
void *rt_character3d_get_ground_body(void *c) {
    (void)c;
    return NULL;
}

/* Ragdoll3D */

/// @brief Stub for `Ragdoll3D.New`. Silent stub returning NULL.
///
/// @param skeleton Skeleton3D handle used to derive bodies and joints (ignored).
///
/// @return `NULL`.
void *rt_ragdoll3d_from_skeleton(void *skeleton) {
    (void)skeleton;
    return NULL;
}

/// @brief Stub for `Ragdoll3D.get_TotalMass`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_total_mass(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_TotalMass`. Silent no-op stub.
///
/// @param r    Ragdoll3D handle (ignored).
/// @param mass Requested aggregate ragdoll mass (ignored).
void rt_ragdoll3d_set_total_mass(void *r, double mass) {
    (void)r;
    (void)mass;
}

/// @brief Stub for `Ragdoll3D.get_RadiusScale`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_radius_scale(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_RadiusScale`. Silent no-op stub.
///
/// @param r     Ragdoll3D handle (ignored).
/// @param scale Collider-radius multiplier (ignored).
void rt_ragdoll3d_set_radius_scale(void *r, double scale) {
    (void)r;
    (void)scale;
}

/// @brief Stub for `Ragdoll3D.get_MinBoneLength`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_min_bone_length(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_MinBoneLength`. Silent no-op stub.
///
/// @param r      Ragdoll3D handle (ignored).
/// @param length Minimum skeleton-bone length eligible for a rigid body (ignored).
void rt_ragdoll3d_set_min_bone_length(void *r, double length) {
    (void)r;
    (void)length;
}

/// @brief Stub for `Ragdoll3D.get_BodyCount`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0`.
int64_t rt_ragdoll3d_get_body_count(void *r) {
    (void)r;
    return 0;
}

/// @brief Stub for `Ragdoll3D.get_Active`. Silent stub returning false.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0`.
int8_t rt_ragdoll3d_get_active(void *r) {
    (void)r;
    return 0;
}

/// @brief Stub for `Ragdoll3D.SetJointLimits`. Silent no-op stub.
///
/// @param ragdoll   Ragdoll3D handle (ignored).
/// @param bone_name Skeleton bone whose joint limits should change (ignored).
/// @param swing_deg Maximum swing angle in degrees (ignored).
/// @param twist_deg Maximum twist angle in degrees (ignored).
void rt_ragdoll3d_set_joint_limits(void *ragdoll,
                                   rt_string bone_name,
                                   double swing_deg,
                                   double twist_deg) {
    (void)ragdoll;
    (void)bone_name;
    (void)swing_deg;
    (void)twist_deg;
}

/// @brief Stub for `Ragdoll3D.Activate`. Silent no-op stub.
///
/// @param r        Ragdoll3D handle (ignored).
/// @param world    Physics3DWorld that would receive the ragdoll bodies (ignored).
/// @param skeleton Skeleton3D pose source (ignored).
/// @param node     Scene node whose transform would anchor the ragdoll (ignored).
void rt_ragdoll3d_activate(void *r, void *world, void *skeleton, void *node) {
    (void)r;
    (void)world;
    (void)skeleton;
    (void)node;
}

/// @brief Stub for `Ragdoll3D.Deactivate`. Silent no-op stub.
///
/// @param r             Ragdoll3D handle (ignored).
/// @param blend_seconds Duration of the blend back to animation (ignored).
void rt_ragdoll3d_deactivate(void *r, double blend_seconds) {
    (void)r;
    (void)blend_seconds;
}

/// @brief Stub for `Ragdoll3D.SetPowered`. Silent no-op stub.
///
/// @param r        Ragdoll3D handle (ignored).
/// @param enabled  Non-zero to enable powered pose matching (ignored).
/// @param strength Pose-matching drive strength (ignored).
void rt_ragdoll3d_set_powered(void *r, int64_t enabled, double strength) {
    (void)r;
    (void)enabled;
    (void)strength;
}

/// @brief Stub for `Ragdoll3D.Step`. Silent no-op stub.
///
/// @param r  Ragdoll3D handle (ignored).
/// @param dt Simulation interval in seconds (ignored).
void rt_ragdoll3d_step(void *r, double dt) {
    (void)r;
    (void)dt;
}

/// @brief Stub for `Ragdoll3D.GetBody`. Silent stub returning NULL.
///
/// @param ragdoll   Ragdoll3D handle (ignored).
/// @param bone_name Skeleton bone whose generated body is requested (ignored).
///
/// @return `NULL`.
void *rt_ragdoll3d_get_body(void *ragdoll, rt_string bone_name) {
    (void)ragdoll;
    (void)bone_name;
    return NULL;
}

/// @brief Stub for the C-internal closest-hit raw raycast. Silent stub returning NULL.
///
/// Writes `-1.0` to `out_distance` when the output pointer is non-NULL.
///
/// @param world        Physics3DWorld handle (ignored).
/// @param ox           Ray-origin X coordinate (ignored).
/// @param oy           Ray-origin Y coordinate (ignored).
/// @param oz           Ray-origin Z coordinate (ignored).
/// @param dx           Ray-direction X component (ignored).
/// @param dy           Ray-direction Y component (ignored).
/// @param dz           Ray-direction Z component (ignored).
/// @param max_distance Maximum ray distance (ignored).
/// @param mask         Collision-layer mask (ignored).
/// @param ignore_body  Optional Body3D handle to exclude (ignored).
/// @param out_distance Optional output receiving `-1.0`.
///
/// @return `NULL`, indicating that no body was hit.
void *rt_world3d_raycast_closest_body_raw(void *world,
                                          double ox,
                                          double oy,
                                          double oz,
                                          double dx,
                                          double dy,
                                          double dz,
                                          double max_distance,
                                          int64_t mask,
                                          const void *ignore_body,
                                          double *out_distance) {
    (void)world;
    (void)ox;
    (void)oy;
    (void)oz;
    (void)dx;
    (void)dy;
    (void)dz;
    (void)max_distance;
    (void)mask;
    (void)ignore_body;
    if (out_distance)
        *out_distance = -1.0;
    return NULL;
}

/* Vehicle3D */

/// @brief Stub for `Vehicle3D.New`. Silent stub returning NULL.
///
/// @param world   Physics3DWorld that would simulate the vehicle (ignored).
/// @param chassis Body3D chassis handle (ignored).
///
/// @return `NULL`.
void *rt_vehicle3d_new(void *world, void *chassis) {
    (void)world;
    (void)chassis;
    return NULL;
}

/// @brief Stub for `Vehicle3D.AddWheel`. Silent stub returning -1.
///
/// @param vehicle         Vehicle3D handle (ignored).
/// @param x               Wheel connection X coordinate in chassis space (ignored).
/// @param y               Wheel connection Y coordinate in chassis space (ignored).
/// @param z               Wheel connection Z coordinate in chassis space (ignored).
/// @param radius          Wheel radius (ignored).
/// @param suspension_rest Suspension rest length (ignored).
/// @param stiffness       Suspension spring stiffness (ignored).
/// @param damping         Suspension damping coefficient (ignored).
/// @param steers          Non-zero if steering input rotates this wheel (ignored).
/// @param driven          Non-zero if drive force is applied to this wheel (ignored).
///
/// @return `-1`, indicating that no wheel was added.
int64_t rt_vehicle3d_add_wheel(void *vehicle,
                               double x,
                               double y,
                               double z,
                               double radius,
                               double suspension_rest,
                               double stiffness,
                               double damping,
                               int8_t steers,
                               int8_t driven) {
    (void)vehicle;
    (void)x;
    (void)y;
    (void)z;
    (void)radius;
    (void)suspension_rest;
    (void)stiffness;
    (void)damping;
    (void)steers;
    (void)driven;
    return -1;
}

/// @brief Stub for `Vehicle3D.SetInput`. Silent no-op stub.
///
/// @param vehicle  Vehicle3D handle (ignored).
/// @param throttle Normalized throttle input (ignored).
/// @param brake    Normalized brake input (ignored).
/// @param steer    Normalized steering input (ignored).
void rt_vehicle3d_set_input(void *vehicle, double throttle, double brake, double steer) {
    (void)vehicle;
    (void)throttle;
    (void)brake;
    (void)steer;
}

/// @brief Stub for `Vehicle3D.SetDriveForce`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param newtons Maximum drive force in newtons (ignored).
void rt_vehicle3d_set_drive_force(void *vehicle, double newtons) {
    (void)vehicle;
    (void)newtons;
}

/// @brief Stub for `Vehicle3D.SetBrakeForce`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param newtons Maximum brake force in newtons (ignored).
void rt_vehicle3d_set_brake_force(void *vehicle, double newtons) {
    (void)vehicle;
    (void)newtons;
}

/// @brief Stub for `Vehicle3D.SetMaxSteer`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param degrees Maximum steering angle in degrees (ignored).
void rt_vehicle3d_set_max_steer(void *vehicle, double degrees) {
    (void)vehicle;
    (void)degrees;
}

/// @brief Stub for `Vehicle3D.SetGrip`. Silent no-op stub.
///
/// @param vehicle      Vehicle3D handle (ignored).
/// @param longitudinal Longitudinal tire-grip coefficient (ignored).
/// @param lateral      Lateral tire-grip coefficient (ignored).
void rt_vehicle3d_set_grip(void *vehicle, double longitudinal, double lateral) {
    (void)vehicle;
    (void)longitudinal;
    (void)lateral;
}

/// @brief Stub for `Vehicle3D.SetCollisionMask`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param mask    Collision-layer mask used by wheel queries (ignored).
void rt_vehicle3d_set_collision_mask(void *vehicle, int64_t mask) {
    (void)vehicle;
    (void)mask;
}

/// @brief Stub for `Vehicle3D.Step`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param dt      Simulation interval in seconds (ignored).
void rt_vehicle3d_step(void *vehicle, double dt) {
    (void)vehicle;
    (void)dt;
}

/// @brief Stub for `Vehicle3D.get_Speed`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_get_speed(void *vehicle) {
    (void)vehicle;
    return 0.0;
}

/// @brief Stub for `Vehicle3D.get_WheelCount`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
///
/// @return `0`.
int64_t rt_vehicle3d_get_wheel_count(void *vehicle) {
    (void)vehicle;
    return 0;
}

/// @brief Stub for `Vehicle3D.WheelInContact`. Silent stub returning false.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0`, indicating that the wheel has no ground contact.
int8_t rt_vehicle3d_wheel_in_contact(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0;
}

/// @brief Stub for `Vehicle3D.WheelTravel`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_wheel_travel(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0.0;
}

/// @brief Stub for `Vehicle3D.WheelLoad`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_wheel_load(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0.0;
}

/* Stub-parity audit additions: entry points registered in the 3D runtime
 * defs whose implementations compile only in graphics-enabled builds. */

/// @brief Trapping stub for `Collider3D.NewConvexHullReduced` (graphics-disabled build).
///
/// @param o  Mesh3D or vertex-data handle used to form the hull (ignored before trapping).
/// @param a1 Requested hull-reduction limit or quality setting (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_collider3d_new_convex_hull_reduced(void *o, int64_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_RET("Collider3D.NewConvexHullReduced: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Ray3D.IntersectTriangleCull` (graphics-disabled build).
///
/// @param o  Vec3 ray origin (ignored).
/// @param a1 Vec3 ray direction (ignored).
/// @param a2 Vec3 triangle vertex zero (ignored).
/// @param a3 Vec3 triangle vertex one (ignored).
/// @param a4 Vec3 triangle vertex two (ignored).
/// @param a5 Non-zero to enable back-face culling (ignored).
///
/// @return `0.0`, the graphics-disabled fallback distance.
double rt_ray3d_intersect_triangle_cull(
    void *o, void *a1, void *a2, void *a3, void *a4, int8_t a5) {
    (void)o;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Ray3D.IntersectTriangleCull: graphics support not compiled in",
                                  0.0);
}
