//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d.h
// Purpose: 3D physics world with AABB/sphere/capsule collision, impulse
//   resolution, rotational body state, collision layers, and character
//   controller with slide/step movement.
//
// Key invariants:
//   - Body/contact/joint arrays grow on demand from production-sized initial capacities.
//   - Symplectic Euler integration (force→velocity, velocity→position).
//   - Dynamic bodies integrate angular velocity into quaternion orientation.
//   - Kinematic bodies move from explicit linear/angular velocity only.
//   - Collision filtering: bidirectional layer/mask bitmask check.
//   - Character controller binds to a World3D and uses swept slide iterations.
//   - Trigger bodies overlap but don't apply physics impulse.
//   - Trigger3D: standalone AABB zone with frame-edge enter/exit detection.
//   - Max 64 tracked bodies per trigger (TRG3D_MAX_TRACKED).
//
// Ownership/Lifetime:
//   - World3D owns retained body and joint references until removal/finalize.
//   - Query/event result handles are GC-managed runtime objects.
//
// Links: rt_raycast3d.h, misc/plans/3d/20-phase-a-core-game-systems.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d.h
 * @brief Declares the Physics3D world, body, query, event, trigger, character, and vehicle APIs.
 *
 * Runtime-managed World3D objects retain registered bodies and joints, perform
 * variable or fixed stepping, publish contact events and telemetry, and answer
 * bounded spatial queries. This header also exposes Body3D configuration,
 * boxed query/event accessors, standalone triggers, character controllers, and
 * raycast vehicles.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physics3D World */
/// @brief Create a 3D physics world with the given gravity vector (m/s² along each axis).
/// @param gx World-space gravity X component.
/// @param gy World-space gravity Y component.
/// @param gz World-space gravity Z component.
/// @return Newly allocated World3D handle, or NULL on allocation failure.
void *rt_world3d_new(double gx, double gy, double gz);

/// @brief Integrate one simulation step of @p dt seconds (resolves contacts and updates events).
/// @param world World3D handle to advance transactionally.
/// @param dt Positive finite elapsed time in seconds.
void rt_world3d_step(void *world, double dt);

/// @brief Accumulate @p dt and run fixed-size physics steps, returning steps actually run.
/// @param world World3D handle to advance.
/// @param dt Positive finite frame duration to accumulate.
/// @param fixed_dt Requested fixed substep duration.
/// @param max_steps Maximum substeps permitted before the spiral guard drops excess time.
/// @return Number of fixed substeps completed.
int64_t rt_world3d_step_fixed(void *world, double dt, double fixed_dt, int64_t max_steps);

/// @brief Render interpolation alpha from the fixed-step accumulator remainder.
/// @param world World3D handle to inspect.
/// @return Fraction in `[0,1)` between the last and next fixed simulation states.
double rt_world3d_get_fixed_step_alpha(void *world);

/// @brief Count fixed physics steps dropped by the max-step spiral guard.
/// @param world World3D handle to inspect.
/// @return Saturating cumulative number of dropped fixed substeps.
int64_t rt_world3d_get_dropped_fixed_steps(void *world);

/// @brief Add a Body3D to the world, growing storage as needed.
/// @param world World3D handle to modify.
/// @param body Body3D handle to retain and register once.
void rt_world3d_add(void *world, void *body);

/// @brief Add a Body3D to the world and return whether it is present afterward.
/// @param world World3D handle to modify.
/// @param body Body3D handle to retain and register once.
/// @return One when @p body is present afterward, otherwise zero.
int8_t rt_world3d_try_add(void *world, void *body);

/// @brief Remove a Body3D from the world (silently ignores unknown bodies).
/// @param world World3D handle to modify.
/// @param body Exact Body3D handle to unregister and release.
void rt_world3d_remove(void *world, void *body);

/// @brief Total number of bodies currently in the world.
/// @param world World3D handle to inspect.
/// @return Safely validated registered body count.
int64_t rt_world3d_body_count(void *world);

/// @brief Whether @p body is currently registered in @p world.
/// @param world World3D handle to inspect.
/// @param body Body3D handle to find.
/// @return One when the exact body is registered, otherwise zero.
int8_t rt_world3d_contains_body(void *world, void *body);

/// @brief Unclamped CCD substep demand from the most recent Step.
/// @param world World3D handle to inspect.
/// @return Requested CCD substep count before the runtime cap.
int64_t rt_world3d_get_last_ccd_requested_substeps(void *world);

/// @brief Actual CCD substeps used by the most recent Step.
/// @param world World3D handle to inspect.
/// @return CCD substeps actually executed.
int64_t rt_world3d_get_last_ccd_substeps(void *world);

/// @brief Number of Step calls whose CCD substep demand exceeded the cap.
/// @param world World3D handle to inspect.
/// @return Cumulative number of clamped Step calls.
int64_t rt_world3d_get_ccd_substep_clamped_count(void *world);

/// @brief Bodies whose CCD demand exceeded the substep cap during the most recent Step.
/// @param world World3D handle to inspect.
/// @return Number of clamped bodies in the latest step.
int64_t rt_world3d_get_last_ccd_clamped_body_count(void *world);

/// @brief Total bodies affected by CCD substep clamping.
/// @param world World3D handle to inspect.
/// @return Cumulative clamped-body count.
int64_t rt_world3d_get_ccd_substep_clamped_body_count(void *world);

/// @brief Total swept time-of-impact clips applied by the CCD pass.
/// @param world World3D handle to inspect.
/// @return Cumulative number of applied time-of-impact clips.
int64_t rt_world3d_get_ccd_toi_count(void *world);

/// @brief Configure the query result capacity (RaycastAll/Overlap*), 16-4096.
/// @param world World3D handle to modify.
/// @param max_hits Requested bounded query capacity.
void rt_world3d_set_max_query_hits(void *world, int64_t max_hits);

/// @brief Current query result capacity.
/// @param world World3D handle to inspect.
/// @return Configured maximum retained hits per multi-result query.
int64_t rt_world3d_get_max_query_hits(void *world);

/// @brief Number of broadphase reserve failures that fell back to brute-force pair scans.
/// @param world World3D handle to inspect.
/// @return Cumulative fallback count.
int64_t rt_world3d_get_broadphase_fallback_count(void *world);

/// @brief Total query-broadphase rebuilds performed (diagnostic; flat under fat-AABB reuse).
/// @param world World3D handle to inspect.
/// @return Cumulative query broadphase rebuild count.
int64_t rt_world3d_get_query_broadphase_rebuild_count(void *world);

/// @brief Replace the world gravity vector at runtime.
/// @param world World3D handle to modify.
/// @param gx Replacement world-space gravity X component.
/// @param gy Replacement world-space gravity Y component.
/// @param gz Replacement world-space gravity Z component.
void rt_world3d_set_gravity(void *world, double gx, double gy, double gz);

/// @brief Shift all body/contact/query state by the inverse of a floating-origin delta.
/// @param world World3D handle to rebase.
/// @param dx World-origin X delta.
/// @param dy World-origin Y delta.
/// @param dz World-origin Z delta.
void rt_world3d_rebase_origin(void *world, double dx, double dy, double dz);

/* Joint management */
/// @brief Register a joint; @p joint_type matches the RT_JOINT_* constants.
/// @param world World3D handle to modify.
/// @param joint Concrete joint handle to retain and register once.
/// @param joint_type Matching `RT_JOINT_*` discriminator.
void rt_world3d_add_joint(void *world, void *joint, int64_t joint_type);

/// @brief Unregister a joint.
/// @param world World3D handle to modify.
/// @param joint Exact joint handle to unregister and release.
void rt_world3d_remove_joint(void *world, void *joint);

/// @brief Number of joints currently in the world.
/// @param world World3D handle to inspect.
/// @return Safely validated registered joint count.
int64_t rt_world3d_joint_count(void *world);

/// @brief Number of iterative solver passes used for constraints.
/// @param world World3D handle to inspect.
/// @return Configured velocity/constraint solver iteration count.
int64_t rt_world3d_get_solver_iterations(void *world);

/// @brief Set iterative solver passes; clamped to the runtime-supported range.
/// @param world World3D handle to modify.
/// @param iterations Requested positive solver pass count.
void rt_world3d_set_solver_iterations(void *world, int64_t iterations);

/// @brief Number of positional correction passes used for contacts.
/// @param world World3D handle to inspect.
/// @return Configured contact position iteration count.
int64_t rt_world3d_get_position_iterations(void *world);

/// @brief Set positional correction passes; clamped to the runtime-supported range.
/// @param world World3D handle to modify.
/// @param iterations Requested positive position pass count.
void rt_world3d_set_position_iterations(void *world, int64_t iterations);

/// @brief Baumgarte contact position-correction fraction.
/// @param world World3D handle to inspect.
/// @return Contact correction fraction in the inclusive range zero to one.
double rt_world3d_get_contact_beta(void *world);

/// @brief Set contact position-correction fraction; clamped to [0, 1].
/// @param world World3D handle to modify.
/// @param beta Requested Baumgarte correction fraction.
void rt_world3d_set_contact_beta(void *world, double beta);

/// @brief Minimum impact speed that applies restitution.
/// @param world World3D handle to inspect.
/// @return Non-negative restitution speed threshold.
double rt_world3d_get_restitution_threshold(void *world);

/// @brief Set the minimum impact speed that applies restitution.
/// @param world World3D handle to modify.
/// @param threshold Requested non-negative speed threshold.
void rt_world3d_set_restitution_threshold(void *world, double threshold);

/// @brief Max active contact islands scheduled by the most recent Step.
/// @param world World3D handle to inspect.
/// @return Peak active island count observed across the most recent step's substeps.
int64_t rt_world3d_get_last_solver_island_count(void *world);

/// @brief Max awake dynamic bodies included in contact islands by the most recent Step.
/// @param world World3D handle to inspect.
/// @return Peak active body count observed across the most recent step's substeps.
int64_t rt_world3d_get_last_solver_active_body_count(void *world);

/// @brief Max non-trigger contacts scheduled through contact islands by the most recent Step.
/// @param world World3D handle to inspect.
/// @return Peak solver contact count observed across the most recent step's substeps.
int64_t rt_world3d_get_last_solver_contact_count(void *world);

/* Collision event queries (populated after each Step) */
/// @brief Number of contact pairs from the most recent Step.
/// @param world World3D handle to inspect.
/// @return Number of safely readable current contacts.
int64_t rt_world3d_get_collision_count(void *world);

/// @brief First body in the @p index-th contact pair from the most recent Step.
/// @param world World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Borrowed first Body3D handle, or NULL when out of range.
void *rt_world3d_get_collision_body_a(void *world, int64_t index);

/// @brief Second body in the @p index-th contact pair.
/// @param world World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Borrowed second Body3D handle, or NULL when out of range.
void *rt_world3d_get_collision_body_b(void *world, int64_t index);

/// @brief Contact normal as a Vec3 (points from body A to body B).
/// @param world World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Newly allocated contact-normal Vec3, or the zero vector when unavailable.
void *rt_world3d_get_collision_normal(void *world, int64_t index);

/// @brief Penetration depth in world units for the @p index-th contact.
/// @param world World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Contact penetration depth, or zero when unavailable.
double rt_world3d_get_collision_depth(void *world, int64_t index);

/// @brief Number of detailed collision events (with contact-point lists) from the most recent Step.
/// @param world World3D handle to inspect.
/// @return Number of detailed current collision events.
int64_t rt_world3d_get_collision_event_count(void *world);

/// @brief Get the @p index-th detailed collision event.
/// @param world World3D handle to inspect.
/// @param index Zero-based detailed event index.
/// @return Newly boxed CollisionEvent3D snapshot, or NULL when out of range.
void *rt_world3d_get_collision_event(void *world, int64_t index);

/// @brief Number of "enter" events (pairs that started colliding this Step).
/// @param world World3D handle to inspect.
/// @return Number of contact pairs entering this step.
int64_t rt_world3d_get_enter_event_count(void *world);

/// @brief Get the @p index-th enter event.
/// @param world World3D handle to inspect.
/// @param index Zero-based enter-event index.
/// @return Newly boxed CollisionEvent3D snapshot, or NULL when out of range.
void *rt_world3d_get_enter_event(void *world, int64_t index);

/// @brief Number of "stay" events (pairs still colliding from a previous Step).
/// @param world World3D handle to inspect.
/// @return Number of persistent contact pairs this step.
int64_t rt_world3d_get_stay_event_count(void *world);

/// @brief Get the @p index-th stay event.
/// @param world World3D handle to inspect.
/// @param index Zero-based stay-event index.
/// @return Newly boxed CollisionEvent3D snapshot, or NULL when out of range.
void *rt_world3d_get_stay_event(void *world, int64_t index);

/// @brief Number of "exit" events (pairs that stopped colliding this Step).
/// @param world World3D handle to inspect.
/// @return Number of contact pairs exiting this step.
int64_t rt_world3d_get_exit_event_count(void *world);

/// @brief Get the @p index-th exit event.
/// @param world World3D handle to inspect.
/// @param index Zero-based exit-event index.
/// @return Newly boxed CollisionEvent3D snapshot, or NULL when out of range.
void *rt_world3d_get_exit_event(void *world, int64_t index);

/// @brief Clear live and transition collision buffers without stepping the simulation.
/// @param world World3D handle whose contact and event buffers are cleared.
void rt_world3d_clear_collision_events(void *world);

/* World queries */
/// @brief Cast a ray; returns the closest PhysicsHit3D within @p max_distance, or NULL if none.
/// @param world World3D handle to query.
/// @param origin Vec3 ray origin.
/// @param direction Nonzero Vec3 ray direction.
/// @param max_distance Non-negative ray length.
/// @param mask Collision-layer mask used to filter bodies.
/// @return Closest boxed PhysicsHit3D, or NULL for no hit or invalid input.
void *rt_world3d_raycast(
    void *world, void *origin, void *direction, double max_distance, int64_t mask);
/// @brief Cast a ray and return all hits as a non-null PhysicsHitList3D, sorted by distance.
/// @details Valid queries with no matches return an empty list; invalid arguments return NULL.
/// @param world World3D handle to query.
/// @param origin Vec3 ray origin.
/// @param direction Nonzero Vec3 ray direction.
/// @param max_distance Non-negative ray length.
/// @param mask Collision-layer mask used to filter bodies.
/// @return Bounded sorted PhysicsHitList3D, or NULL for invalid input.
void *rt_world3d_raycast_all(
    void *world, void *origin, void *direction, double max_distance, int64_t mask);
/// @brief C-internal raycast-all fast path: writes the non-trigger bodies the ray pierces
///   (nearest-first, borrowed pointers) into @p out_bodies — no hit-list boxing, no Vec3
///   handles. Not part of the script-visible registry. Returns the count written.
/// @param world World3D handle to query.
/// @param origin Three-component ray origin.
/// @param direction Three-component nonzero ray direction.
/// @param max_distance Non-negative ray length.
/// @param mask Collision-layer mask used to filter bodies.
/// @param out_bodies Caller-owned array receiving borrowed Body3D handles.
/// @param out_cap Number of available slots in @p out_bodies.
/// @return Number of borrowed body handles written, or -1 for invalid input or query failure.
int32_t rt_world3d_raycast_all_bodies_raw(void *world,
                                          const double *origin,
                                          const double *direction,
                                          double max_distance,
                                          int64_t mask,
                                          void **out_bodies,
                                          int32_t out_cap);
/// @brief C-internal closest-hit raycast fast path over raw components — no Vec3
///   or hit-object boxing. Not part of the script-visible registry.
/// @param world World3D handle to query.
/// @param ox Ray-origin X component.
/// @param oy Ray-origin Y component.
/// @param oz Ray-origin Z component.
/// @param dx Ray-direction X component.
/// @param dy Ray-direction Y component.
/// @param dz Ray-direction Z component.
/// @param max_distance Non-negative ray length.
/// @param mask Collision-layer mask used to filter bodies.
/// @param ignore_body Optional Body3D handle excluded from results.
/// @param out_distance Optional output receiving hit distance or -1 for no hit.
/// @return Borrowed closest hit body (do NOT release), or NULL; writes the hit
///   distance to @p out_distance when non-NULL (-1 when no hit).
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
                                          double *out_distance);
/// @brief Sweep a sphere along @p delta; returns the first PhysicsHit3D or NULL.
/// @param world World3D handle to query.
/// @param center Vec3 initial sphere center.
/// @param radius Positive sphere radius.
/// @param delta Vec3 world-space sweep displacement.
/// @param mask Collision-layer mask used to filter bodies.
/// @return First boxed PhysicsHit3D, or NULL for no hit or invalid input.
void *rt_world3d_sweep_sphere(void *world, void *center, double radius, void *delta, int64_t mask);

/// @brief Sweep a capsule (segment between @p a and @p b, radius @p radius) along @p delta.
/// @param world World3D handle to query.
/// @param a Vec3 initial capsule segment start.
/// @param b Vec3 initial capsule segment end.
/// @param radius Positive capsule radius.
/// @param delta Vec3 world-space sweep displacement.
/// @param mask Collision-layer mask used to filter bodies.
/// @return First boxed PhysicsHit3D, or NULL for no hit or invalid input.
void *rt_world3d_sweep_capsule(
    void *world, void *a, void *b, double radius, void *delta, int64_t mask);
/// @brief Find all bodies overlapping a sphere (non-null PhysicsHitList3D for valid inputs).
/// @param world World3D handle to query.
/// @param center Vec3 sphere center.
/// @param radius Positive sphere radius.
/// @param mask Collision-layer mask used to filter bodies.
/// @return Bounded PhysicsHitList3D, or NULL for invalid input.
void *rt_world3d_overlap_sphere(void *world, void *center, double radius, int64_t mask);

/// @brief Find all bodies overlapping an axis-aligned bounding box (non-null list for valid
/// inputs).
/// @param world World3D handle to query.
/// @param min_corner First Vec3 AABB corner.
/// @param max_corner Opposite Vec3 AABB corner.
/// @param mask Collision-layer mask used to filter bodies.
/// @return Bounded PhysicsHitList3D, or NULL for invalid input.
void *rt_world3d_overlap_aabb(void *world, void *min_corner, void *max_corner, int64_t mask);

/* Traversal probes (rt_physics3d_probes.c) */
/// @brief True when a capsule (radius, height) fits at @p position without solid overlap.
/// @param world World3D handle to query.
/// @param position Vec3 capsule center/placement point.
/// @param radius Positive capsule radius.
/// @param height Total capsule height including caps.
/// @param mask Collision-layer mask used to filter blockers.
/// @return One when the capsule fits without solid overlap, otherwise zero.
int8_t rt_world3d_probe_clearance(
    void *world, void *position, double radius, double height, int64_t mask);
/// @brief Find a grabbable ledge ahead: wall sweep + top down-sweep + standing room.
///   Returns a LedgeHit3D or NULL. Results are world-space snapshots at probe time.
/// @param world World3D handle to query.
/// @param origin Vec3 traversal probe origin.
/// @param forward Nonzero Vec3 horizontal search direction.
/// @param radius Character/capsule radius.
/// @param max_height Maximum reachable ledge height.
/// @param max_depth Maximum forward search depth.
/// @param mask Collision-layer mask used to filter geometry.
/// @return Boxed LedgeHit3D snapshot, or NULL when no valid ledge is found.
void *rt_world3d_probe_ledge(void *world,
                             void *origin,
                             void *forward,
                             double radius,
                             double max_height,
                             double max_depth,
                             int64_t mask);
/// @brief Like ProbeLedge but also requires a near-origin-level far-side landing.
/// @param world World3D handle to query.
/// @param origin Vec3 traversal probe origin.
/// @param forward Nonzero Vec3 horizontal search direction.
/// @param radius Character/capsule radius.
/// @param max_height Maximum vault obstacle height.
/// @param max_thickness Maximum obstacle thickness.
/// @param mask Collision-layer mask used to filter geometry.
/// @return Boxed LedgeHit3D with landing data, or NULL when no valid vault is found.
void *rt_world3d_probe_vault(void *world,
                             void *origin,
                             void *forward,
                             double radius,
                             double max_height,
                             double max_thickness,
                             int64_t mask);

/* LedgeHit3D result accessors */
/// @brief World-space ledge edge point (wall contact XZ at ledge-top height).
/// @param hit LedgeHit3D handle to inspect.
/// @return Newly allocated grab-point Vec3, or the origin for an invalid handle.
void *rt_ledge_hit3d_get_grab_point(void *hit);

/// @brief Ledge top surface normal.
/// @param hit LedgeHit3D handle to inspect.
/// @return Newly allocated top-normal Vec3, or the zero vector for an invalid handle.
void *rt_ledge_hit3d_get_surface_normal(void *hit);

/// @brief Front wall surface normal.
/// @param hit LedgeHit3D handle to inspect.
/// @return Newly allocated wall-normal Vec3, or the zero vector for an invalid handle.
void *rt_ledge_hit3d_get_wall_normal(void *hit);

/// @brief Far-side vault landing point (NULL for ledge probes).
/// @param hit LedgeHit3D handle to inspect.
/// @return Newly allocated landing-point Vec3 when present, otherwise NULL.
void *rt_ledge_hit3d_get_landing_point(void *hit);

/// @brief Ledge top height above the probe origin.
/// @param hit LedgeHit3D handle to inspect.
/// @return Non-negative measured height, or zero for an invalid handle.
double rt_ledge_hit3d_get_height(void *hit);

/// @brief True when a capsule of the probe dims fits above the ledge top.
/// @param hit LedgeHit3D handle to inspect.
/// @return One when standing clearance was verified, otherwise zero.
int8_t rt_ledge_hit3d_get_has_standing_room(void *hit);

/// @brief True when a vault landing was found (always false for ledge probes).
/// @param hit LedgeHit3D handle to inspect.
/// @return One when landing data is present, otherwise zero.
int8_t rt_ledge_hit3d_get_has_landing(void *hit);

/* PhysicsHit3D */
/// @brief Get the body that was hit.
/// @param hit PhysicsHit3D handle to inspect.
/// @return Borrowed Body3D handle retained by the hit, or NULL.
void *rt_physics_hit3d_get_body(void *hit);

/// @brief Get the specific collider on the body that was hit (compound bodies).
/// @param hit PhysicsHit3D handle to inspect.
/// @return Borrowed leaf Collider3D handle retained by the hit, or NULL.
void *rt_physics_hit3d_get_collider(void *hit);

/// @brief Surface tag of the hit (leaf) collider; 0 when untyped.
/// @param hit PhysicsHit3D handle to inspect.
/// @return Non-negative surface registry identifier, or zero.
int64_t rt_physics_hit3d_get_surface_type(void *hit);

/// @brief Get the world-space hit point as a Vec3.
/// @param hit PhysicsHit3D handle to inspect.
/// @return Newly allocated world-space point Vec3.
void *rt_physics_hit3d_get_point(void *hit);

/// @brief Get the surface normal at the hit point as a Vec3.
/// @param hit PhysicsHit3D handle to inspect.
/// @return Newly allocated surface-normal Vec3.
void *rt_physics_hit3d_get_normal(void *hit);

/// @brief Get the distance from the ray/sweep origin to the hit point.
/// @param hit PhysicsHit3D handle to inspect.
/// @return Non-negative hit distance, or zero for an invalid handle.
double rt_physics_hit3d_get_distance(void *hit);

/// @brief Get the parametric fraction along the sweep where the hit occurred (0..1).
/// @param hit PhysicsHit3D handle to inspect.
/// @return Sweep fraction in the inclusive range zero to one.
double rt_physics_hit3d_get_fraction(void *hit);

/// @brief True if the sweep started already penetrating the collider (initial overlap).
/// @param hit PhysicsHit3D handle to inspect.
/// @return One for initial penetration, otherwise zero.
int8_t rt_physics_hit3d_get_started_penetrating(void *hit);

/// @brief True if the hit body is a trigger volume (not a solid collider).
/// @param hit PhysicsHit3D handle to inspect.
/// @return One for a trigger hit, otherwise zero.
int8_t rt_physics_hit3d_get_is_trigger(void *hit);

/* PhysicsHitList3D */
/// @brief Number of hits in the list.
/// @param list PhysicsHitList3D handle to inspect.
/// @return Number of safely readable retained hits.
int64_t rt_physics_hit_list3d_get_count(void *list);

/// @brief Number of hits matched before the bounded result list was truncated.
/// @param list PhysicsHitList3D handle to inspect.
/// @return Total matching hit count before retention limits.
int64_t rt_physics_hit_list3d_get_total_count(void *list);

/// @brief True when Count is smaller than TotalCount.
/// @param list PhysicsHitList3D handle to inspect.
/// @return One when results were truncated, otherwise zero.
int8_t rt_physics_hit_list3d_get_truncated(void *list);

/// @brief Get the @p index-th PhysicsHit3D in the list (NULL if out of range).
/// @param list PhysicsHitList3D handle to inspect.
/// @param index Zero-based retained hit index.
/// @return Borrowed PhysicsHit3D handle, or NULL when out of range.
void *rt_physics_hit_list3d_get(void *list, int64_t index);

/* CollisionEvent3D */
/// @brief Get the first body in the collision pair.
/// @param event CollisionEvent3D handle to inspect.
/// @return Borrowed first Body3D handle retained by the event, or NULL.
void *rt_collision_event3d_get_body_a(void *event);

/// @brief Get the second body in the collision pair.
/// @param event CollisionEvent3D handle to inspect.
/// @return Borrowed second Body3D handle retained by the event, or NULL.
void *rt_collision_event3d_get_body_b(void *event);

/// @brief Get the specific collider on body A (for compound bodies).
/// @param event CollisionEvent3D handle to inspect.
/// @return Borrowed leaf Collider3D for body A, or NULL.
void *rt_collision_event3d_get_collider_a(void *event);

/// @brief Get the specific collider on body B.
/// @param event CollisionEvent3D handle to inspect.
/// @return Borrowed leaf Collider3D for body B, or NULL.
void *rt_collision_event3d_get_collider_b(void *event);

/// @brief Surface tags of each side's leaf collider; 0 when untyped.
/// @param event CollisionEvent3D handle to inspect.
/// @return Body A's non-negative surface registry identifier, or zero.
int64_t rt_collision_event3d_get_surface_type_a(void *event);

/// @brief Read body B's leaf-collider surface tag.
/// @param event CollisionEvent3D handle to inspect.
/// @return Body B's non-negative surface registry identifier, or zero.
int64_t rt_collision_event3d_get_surface_type_b(void *event);

/// @brief True if either body in the pair is a trigger volume.
/// @param event CollisionEvent3D handle to inspect.
/// @return One for a trigger overlap event, otherwise zero.
int8_t rt_collision_event3d_get_is_trigger(void *event);

/// @brief Number of contact points generated for this collision (typically 1–4).
/// @param event CollisionEvent3D handle to inspect.
/// @return Safely bounded inline contact count.
int64_t rt_collision_event3d_get_contact_count(void *event);

/// @brief Relative speed of the two bodies along the contact normal at first contact.
/// @param event CollisionEvent3D handle to inspect.
/// @return Finite relative normal speed, or zero for an invalid handle.
double rt_collision_event3d_get_relative_speed(void *event);

/// @brief Total normal impulse applied to resolve the collision (proportional to "force").
/// @param event CollisionEvent3D handle to inspect.
/// @return Finite accumulated normal impulse, or zero for an invalid handle.
double rt_collision_event3d_get_normal_impulse(void *event);

/// @brief Get the @p index-th ContactPoint3D for this event.
/// @param event CollisionEvent3D handle to inspect.
/// @param index Zero-based contact index.
/// @return Newly boxed ContactPoint3D snapshot, or NULL when out of range.
void *rt_collision_event3d_get_contact(void *event, int64_t index);

/// @brief Convenience: world-space position of the @p index-th contact point.
/// @param event CollisionEvent3D handle to inspect.
/// @param index Zero-based contact index.
/// @return Newly allocated position Vec3, or the origin when unavailable.
void *rt_collision_event3d_get_contact_point(void *event, int64_t index);

/// @brief Convenience: surface normal of the @p index-th contact point.
/// @param event CollisionEvent3D handle to inspect.
/// @param index Zero-based contact index.
/// @return Newly allocated normal Vec3, or the zero vector when unavailable.
void *rt_collision_event3d_get_contact_normal(void *event, int64_t index);

/// @brief Convenience: penetration depth at the @p index-th contact (negative = separation).
/// @param event CollisionEvent3D handle to inspect.
/// @param index Zero-based contact index.
/// @return Signed contact separation, or zero when unavailable.
double rt_collision_event3d_get_contact_separation(void *event, int64_t index);

/* ContactPoint3D */
/// @brief Get the world-space position of the contact.
/// @param contact ContactPoint3D handle to inspect.
/// @return Newly allocated world-space position Vec3.
void *rt_contact_point3d_get_point(void *contact);

/// @brief Get the surface normal at the contact (points from body A to body B).
/// @param contact ContactPoint3D handle to inspect.
/// @return Newly allocated contact-normal Vec3.
void *rt_contact_point3d_get_normal(void *contact);

/// @brief Get the contact separation (>0 = penetration depth, <0 = gap).
/// @param contact ContactPoint3D handle to inspect.
/// @return Signed contact separation, or zero for an invalid handle.
double rt_contact_point3d_get_separation(void *contact);

/* Physics3D Body */
/// @brief Create a body with no collider yet (call `_set_collider` before adding to a world).
/// @param mass Requested non-negative dynamic mass; zero creates an immovable body.
/// @return Newly allocated Body3D handle, or NULL on allocation failure.
void *rt_body3d_new(double mass);

/// @brief Create a body with an AABB collider of half-extents (hx, hy, hz).
/// @param hx X-axis half-extent.
/// @param hy Y-axis half-extent.
/// @param hz Z-axis half-extent.
/// @param mass Requested non-negative dynamic mass.
/// @return Newly allocated Body3D with box collider, or NULL on failure.
void *rt_body3d_new_aabb(double hx, double hy, double hz, double mass);

/// @brief Create a body with a sphere collider of the given radius.
/// @param radius Sphere radius.
/// @param mass Requested non-negative dynamic mass.
/// @return Newly allocated Body3D with sphere collider, or NULL on failure.
void *rt_body3d_new_sphere(double radius, double mass);

/// @brief Create a body with a capsule collider (radius + total height including caps).
/// @param radius Capsule radius.
/// @param height Total capsule height including caps.
/// @param mass Requested non-negative dynamic mass.
/// @return Newly allocated Body3D with capsule collider, or NULL on failure.
void *rt_body3d_new_capsule(double radius, double height, double mass);

/// @brief Replace the body's collider (Collider3D handle).
/// @param body Body3D handle to modify.
/// @param collider Collider3D handle to retain, or NULL to clear geometry.
void rt_body3d_set_collider(void *body, void *collider);

/// @brief Get the currently bound collider handle.
/// @param body Body3D handle to inspect.
/// @return Borrowed Collider3D handle, or NULL when none is bound.
void *rt_body3d_get_collider(void *body);

/// @brief Teleport the body to (x, y, z) (does not generate motion vectors).
/// @param body Body3D handle to reposition.
/// @param x World-space X coordinate.
/// @param y World-space Y coordinate.
/// @param z World-space Z coordinate.
void rt_body3d_set_position(void *body, double x, double y, double z);

/// @brief Get the body's world-space position as a Vec3.
/// @param body Body3D handle to inspect.
/// @return Newly allocated position Vec3, or the origin for an invalid handle.
void *rt_body3d_get_position(void *body);

/// @brief Set collider scale applied during physics queries and collision tests.
/// @param body Body3D handle to modify.
/// @param x X-axis scale.
/// @param y Y-axis scale.
/// @param z Z-axis scale.
void rt_body3d_set_scale(void *body, double x, double y, double z);

/// @brief Get the body's collider scale as a Vec3.
/// @param body Body3D handle to inspect.
/// @return Newly allocated scale Vec3, or unit scale for an invalid handle.
void *rt_body3d_get_scale(void *body);

/// @brief Set the body's orientation from a Quaternion (overwrites accumulated rotation).
/// @param body Body3D handle to modify.
/// @param quat Quaternion handle whose normalized XYZW components are copied.
void rt_body3d_set_orientation(void *body, void *quat);

/// @brief Get the body's current orientation as a Quaternion.
/// @param body Body3D handle to inspect.
/// @return Newly allocated normalized Quaternion, or identity for an invalid handle.
void *rt_body3d_get_orientation(void *body);

/// @brief Copy raw position, orientation, and scale arrays without allocating wrapper objects.
/// @param body Body3D handle to inspect.
/// @param position_out Optional three-component position output.
/// @param rotation_out Optional four-component XYZW orientation output.
/// @param scale_out Optional three-component scale output.
void rt_body3d_get_pose_raw(void *body,
                            double *position_out,
                            double *rotation_out,
                            double *scale_out);
/// @brief Set the linear velocity in m/s.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param vx World-space X velocity.
/// @param vy World-space Y velocity.
/// @param vz World-space Z velocity.
void rt_body3d_set_velocity(void *body, double vx, double vy, double vz);

/// @brief Get the linear velocity as a Vec3.
/// @param body Body3D handle to inspect.
/// @return Newly allocated linear-velocity Vec3, or the zero vector for an invalid handle.
void *rt_body3d_get_velocity(void *body);

/// @brief Set the angular velocity in rad/s about each axis.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param wx World-space X angular velocity.
/// @param wy World-space Y angular velocity.
/// @param wz World-space Z angular velocity.
void rt_body3d_set_angular_velocity(void *body, double wx, double wy, double wz);

/// @brief Get the angular velocity as a Vec3.
/// @param body Body3D handle to inspect.
/// @return Newly allocated angular-velocity Vec3, or the zero vector for an invalid handle.
void *rt_body3d_get_angular_velocity(void *body);

/// @brief Apply a continuous force in Newtons (integrated over the next Step).
/// @param body Body3D handle to modify and wake when dynamic.
/// @param fx World-space X force component.
/// @param fy World-space Y force component.
/// @param fz World-space Z force component.
void rt_body3d_apply_force(void *body, double fx, double fy, double fz);

/// @brief Apply a continuous force at a world point — adds force and torque
///   (r x force), e.g. a thruster. Cleared each step like ApplyForce.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param fx World-space X force component.
/// @param fy World-space Y force component.
/// @param fz World-space Z force component.
/// @param px World-space X coordinate of the application point.
/// @param py World-space Y coordinate of the application point.
/// @param pz World-space Z coordinate of the application point.
void rt_body3d_apply_force_at_point(
    void *body, double fx, double fy, double fz, double px, double py, double pz);

/// @brief Apply an instantaneous impulse (kg·m/s) — changes velocity immediately.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param ix World-space X impulse component.
/// @param iy World-space Y impulse component.
/// @param iz World-space Z impulse component.
void rt_body3d_apply_impulse(void *body, double ix, double iy, double iz);

/// @brief Apply a linear impulse at a world point — adds linear and (via the lever
///   arm) angular velocity, so off-center hits spin the body.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param ix World-space X impulse component.
/// @param iy World-space Y impulse component.
/// @param iz World-space Z impulse component.
/// @param px World-space X coordinate of the application point.
/// @param py World-space Y coordinate of the application point.
/// @param pz World-space Z coordinate of the application point.
void rt_body3d_apply_impulse_at_point(
    void *body, double ix, double iy, double iz, double px, double py, double pz);

/// @brief Apply a continuous torque (N·m) about each axis.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param tx World-space X torque component.
/// @param ty World-space Y torque component.
/// @param tz World-space Z torque component.
void rt_body3d_apply_torque(void *body, double tx, double ty, double tz);

/// @brief Apply an instantaneous angular impulse — changes angular velocity immediately.
/// @param body Body3D handle to modify and wake when dynamic.
/// @param ix World-space X angular-impulse component.
/// @param iy World-space Y angular-impulse component.
/// @param iz World-space Z angular-impulse component.
void rt_body3d_apply_angular_impulse(void *body, double ix, double iy, double iz);

/// @brief Set the bounciness coefficient (0 = perfectly inelastic, 1 = perfectly elastic).
/// @param body Body3D handle to modify.
/// @param r Restitution coefficient, clamped to the supported range.
void rt_body3d_set_restitution(void *body, double r);

/// @brief Get the restitution coefficient.
/// @param body Body3D handle to inspect.
/// @return Stored restitution coefficient, or zero for an invalid handle.
double rt_body3d_get_restitution(void *body);

/// @brief Set the friction coefficient (typical range 0.0–1.0).
/// @param body Body3D handle to modify.
/// @param f Non-negative friction coefficient.
void rt_body3d_set_friction(void *body, double f);

/// @brief Opaque gameplay handle on the body (0 default); never read by the runtime.
/// @param body Body3D handle to modify.
/// @param value Application-defined signed integer value.
void rt_body3d_set_user_data(void *body, int64_t value);

/// @brief Get the opaque application-defined gameplay handle stored on the body.
/// @param body Body3D handle to inspect.
/// @return Stored application value, or zero for an invalid handle.
int64_t rt_body3d_get_user_data(void *body);

/// @brief Get the friction coefficient.
/// @param body Body3D handle to inspect.
/// @return Stored friction coefficient, or zero for an invalid handle.
double rt_body3d_get_friction(void *body);

/// @brief Set linear damping (per-second velocity decay multiplier).
/// @param body Body3D handle to modify.
/// @param d Non-negative linear damping coefficient.
void rt_body3d_set_linear_damping(void *body, double d);

/// @brief Get linear damping.
/// @param body Body3D handle to inspect.
/// @return Stored linear damping coefficient, or zero for an invalid handle.
double rt_body3d_get_linear_damping(void *body);

/// @brief Set angular damping (per-second angular-velocity decay multiplier).
/// @param body Body3D handle to modify.
/// @param d Non-negative angular damping coefficient.
void rt_body3d_set_angular_damping(void *body, double d);

/// @brief Get angular damping.
/// @param body Body3D handle to inspect.
/// @return Stored angular damping coefficient, or zero for an invalid handle.
double rt_body3d_get_angular_damping(void *body);

/// @brief Set the body's collision layer (1–63 reserved bits identify "what I am").
/// @param body Body3D handle to modify.
/// @param layer Collision-layer bit field assigned to the body.
void rt_body3d_set_collision_layer(void *body, int64_t layer);

/// @brief Get the body's collision layer.
/// @param body Body3D handle to inspect.
/// @return Stored collision-layer bit field, or zero for an invalid handle.
int64_t rt_body3d_get_collision_layer(void *body);

/// @brief Set the body's collision mask (bitmask of layers it interacts with).
/// @param body Body3D handle to modify.
/// @param mask Bit field selecting collision layers that may interact with the body.
void rt_body3d_set_collision_mask(void *body, int64_t mask);

/// @brief Get the body's collision mask.
/// @param body Body3D handle to inspect.
/// @return Stored collision-mask bit field, or zero for an invalid handle.
int64_t rt_body3d_get_collision_mask(void *body);

/// @brief Mark the body as static (immovable, infinite mass).
/// @param body Body3D handle to modify.
/// @param s Nonzero to make the body static; zero to restore non-static motion.
void rt_body3d_set_static(void *body, int8_t s);

/// @brief True if the body is static.
/// @param body Body3D handle to inspect.
/// @return One when the body is static, otherwise zero.
int8_t rt_body3d_is_static(void *body);

/// @brief Mark the body as kinematic (moves only via explicit velocity, not forces).
/// @param body Body3D handle to modify.
/// @param k Nonzero to make the body kinematic; zero to restore non-kinematic motion.
void rt_body3d_set_kinematic(void *body, int8_t k);

/// @brief True if the body is kinematic.
/// @param body Body3D handle to inspect.
/// @return One when the body is kinematic, otherwise zero.
int8_t rt_body3d_is_kinematic(void *body);

/// @brief Mark the body as a trigger (generates events but doesn't apply impulse).
/// @param body Body3D handle to modify.
/// @param t Nonzero to enable trigger behavior; zero to make contacts solid.
void rt_body3d_set_trigger(void *body, int8_t t);

/// @brief True if the body is a trigger.
/// @param body Body3D handle to inspect.
/// @return One when the body is configured as a trigger, otherwise zero.
int8_t rt_body3d_is_trigger(void *body);

/// @brief Allow or forbid the body from entering sleep state when at rest.
/// @param body Body3D handle to modify.
/// @param can_sleep Nonzero to permit automatic sleeping; zero to keep the body awake.
void rt_body3d_set_can_sleep(void *body, int8_t can_sleep);

/// @brief True if the body is allowed to sleep.
/// @param body Body3D handle to inspect.
/// @return One when automatic sleeping is allowed, otherwise zero.
int8_t rt_body3d_can_sleep(void *body);

/// @brief True if the body is currently sleeping (not being simulated).
/// @param body Body3D handle to inspect.
/// @return One when the body is asleep, otherwise zero.
int8_t rt_body3d_is_sleeping(void *body);

/// @brief Force the body awake (it will be re-simulated next Step).
/// @param body Body3D handle to wake.
void rt_body3d_wake(void *body);

/// @brief Force the body to sleep immediately (zero velocity).
/// @param body Body3D handle to put to sleep.
void rt_body3d_sleep(void *body);

/// @brief Toggle continuous collision detection for fast-moving bodies (prevents tunneling).
/// @details The swept TOI pass clips motion against STATIC and KINEMATIC surfaces only —
///   their poses are stable for the whole substep, so the sweep is exact. Two fast
///   dynamic bodies rely on substep subdivision, not CCD, and can still pass through
///   each other in a single extreme substep; keep relative speeds of dynamic pairs
///   below (size / dt) or use kinematic bodies for critical thin barriers.
/// @param body Body3D handle to modify.
/// @param use_ccd Nonzero to enable continuous collision detection; zero to disable it.
void rt_body3d_set_use_ccd(void *body, int8_t use_ccd);

/// @brief Get the CCD enable flag.
/// @param body Body3D handle to inspect.
/// @return One when continuous collision detection is enabled, otherwise zero.
int8_t rt_body3d_get_use_ccd(void *body);

/// @brief True if the body is currently in contact with a surface below it.
/// @param body Body3D handle to inspect.
/// @return One when the latest contact state is grounded, otherwise zero.
int8_t rt_body3d_is_grounded(void *body);

/// @brief Get the ground normal (Vec3) for a grounded body, or zero vector if airborne.
/// @param body Body3D handle to inspect.
/// @return Newly allocated ground-normal Vec3, or the zero vector when not grounded.
void *rt_body3d_get_ground_normal(void *body);

/// @brief Get the body's mass in kg (0 for static/kinematic bodies).
/// @param body Body3D handle to inspect.
/// @return Effective simulation mass in kilograms, or zero for an immovable or invalid body.
double rt_body3d_get_mass(void *body);

/* Trigger3D — standalone AABB zone with enter/exit detection */
/// @brief Create a standalone trigger zone (axis-aligned box from min to max corner).
/// @param x0 First corner X coordinate.
/// @param y0 First corner Y coordinate.
/// @param z0 First corner Z coordinate.
/// @param x1 Opposite corner X coordinate.
/// @param y1 Opposite corner Y coordinate.
/// @param z1 Opposite corner Z coordinate.
/// @return Newly allocated Trigger3D handle with normalized bounds, or NULL on failure.
void *rt_trigger3d_new(double x0, double y0, double z0, double x1, double y1, double z1);

/// @brief True if @p point (Vec3) is inside the trigger AABB.
/// @param trigger Trigger3D handle to query.
/// @param point Vec3 world-space point to test.
/// @return One when the point lies within the inclusive bounds, otherwise zero.
int8_t rt_trigger3d_contains(void *trigger, void *point);

/// @brief Scan @p world's bodies and emit enter/exit events for this trigger this frame.
/// @param trigger Trigger3D handle whose overlap history is updated.
/// @param world World3D handle containing candidate bodies.
void rt_trigger3d_update(void *trigger, void *world);

/// @brief Number of bodies that entered the trigger during the most recent Update.
/// @param trigger Trigger3D handle to inspect.
/// @return Number of tracked bodies newly overlapping during the latest update.
int64_t rt_trigger3d_get_enter_count(void *trigger);

/// @brief Number of bodies that exited the trigger during the most recent Update.
/// @param trigger Trigger3D handle to inspect.
/// @return Number of tracked bodies that stopped overlapping during the latest update.
int64_t rt_trigger3d_get_exit_count(void *trigger);

/// @brief Replace the trigger's AABB at runtime.
/// @param trigger Trigger3D handle to modify.
/// @param x0 First corner X coordinate.
/// @param y0 First corner Y coordinate.
/// @param z0 First corner Z coordinate.
/// @param x1 Opposite corner X coordinate.
/// @param y1 Opposite corner Y coordinate.
/// @param z1 Opposite corner Z coordinate.
void rt_trigger3d_set_bounds(
    void *trigger, double x0, double y0, double z0, double x1, double y1, double z1);

/* Character Controller */
/// @brief Create a capsule-based character controller (radius, total height, mass).
/// @param radius Capsule radius in world units.
/// @param height Total capsule height including both caps.
/// @param mass Character mass used for dynamic-body interaction.
/// @return Newly allocated Character3D handle, or NULL on allocation failure.
void *rt_character3d_new(double radius, double height, double mass);

/// @brief Move the controller by velocity * dt with swept-slide collision response.
/// @param ctrl Character3D handle to move.
/// @param velocity Vec3 desired world-space velocity.
/// @param dt Positive finite elapsed time in seconds.
void rt_character3d_move(void *ctrl, void *velocity, double dt);

/// @brief Set the maximum step-up height for stairs (controller auto-climbs steps below this).
/// @param ctrl Character3D handle to modify.
/// @param h Requested non-negative step height in world units.
void rt_character3d_set_step_height(void *ctrl, double h);

/// @brief Get the current step-up height limit.
/// @param ctrl Character3D handle to inspect.
/// @return Configured maximum step height, or zero for an invalid handle.
double rt_character3d_get_step_height(void *ctrl);

/// @brief Set the maximum walkable slope in degrees (steeper slopes = sliding instead of walking).
/// @param ctrl Character3D handle to modify.
/// @param degrees Requested slope angle in degrees.
void rt_character3d_set_slope_limit(void *ctrl, double degrees);

/// @brief Bind the controller to a physics world (required before Move).
/// @param ctrl Character3D handle to modify.
/// @param world World3D handle to retain, or NULL to unbind.
void rt_character3d_set_world(void *ctrl, void *world);

/// @brief Get the bound physics world.
/// @param ctrl Character3D handle to inspect.
/// @return Borrowed World3D handle, or NULL when unbound or invalid.
void *rt_character3d_get_world(void *ctrl);

/// @brief True if the controller is currently standing on a walkable surface.
/// @param ctrl Character3D handle to inspect.
/// @return One when grounded on a walkable surface, otherwise zero.
int8_t rt_character3d_is_grounded(void *ctrl);

/// @brief True for one frame after the controller transitions from airborne to grounded.
/// @param ctrl Character3D handle to inspect.
/// @return One for the landing-transition frame, otherwise zero.
int8_t rt_character3d_just_landed(void *ctrl);

/// @brief Get the controller's world-space position as a Vec3.
/// @param ctrl Character3D handle to inspect.
/// @return Newly allocated position Vec3, or the origin for an invalid handle.
void *rt_character3d_get_position(void *ctrl);

/// @brief Teleport the controller (no swept collision; use Move for normal motion).
/// @param ctrl Character3D handle to reposition.
/// @param x World-space X coordinate.
/// @param y World-space Y coordinate.
/// @param z World-space Z coordinate.
void rt_character3d_set_position(void *ctrl, double x, double y, double z);

/// @brief Crouch/stand capsule resize: shrinking always succeeds (feet planted);
///   growing fails (returns 0) when the stand pose is blocked (TryStand semantics).
/// @param ctrl Character3D handle to resize.
/// @param height Requested total capsule height including caps.
/// @return One when the requested height was applied, otherwise zero.
int8_t rt_character3d_try_set_height(void *ctrl, double height);

/// @brief Property form of TrySetHeight (result ignored).
/// @param ctrl Character3D handle to resize.
/// @param height Requested total capsule height including caps.
void rt_character3d_set_height(void *ctrl, double height);

/// @brief Current capsule height including both hemispherical caps.
/// @param ctrl Character3D handle to inspect.
/// @return Current total capsule height, or zero for an invalid handle.
double rt_character3d_get_height(void *ctrl);

/// @brief Set the dynamic-body push impulse scale (0 = dynamics block without push).
/// @param ctrl Character3D handle to modify.
/// @param strength Requested non-negative push impulse scale.
void rt_character3d_set_push_strength(void *ctrl, double strength);

/// @brief Get the dynamic-body push impulse scale.
/// @param ctrl Character3D handle to inspect.
/// @return Configured push impulse scale, or zero for an invalid handle.
double rt_character3d_get_push_strength(void *ctrl);

/// @brief Enable/disable dynamic bodies as blockers (default on; off = legacy ghosting).
/// @param ctrl Character3D handle to modify.
/// @param enabled Nonzero to collide with dynamic bodies; zero to ignore them.
void rt_character3d_set_collide_dynamic(void *ctrl, int8_t enabled);

/// @brief Whether dynamic bodies block/push the controller.
/// @param ctrl Character3D handle to inspect.
/// @return One when dynamic-body collision is enabled, otherwise zero.
int8_t rt_character3d_get_collide_dynamic(void *ctrl);

/// @brief Enable/disable riding moving kinematic/static platforms (default on).
/// @param ctrl Character3D handle to modify.
/// @param enabled Nonzero to inherit platform motion; zero to disable riding.
void rt_character3d_set_ride_platforms(void *ctrl, int8_t enabled);

/// @brief Whether the controller rides moving platforms.
/// @param ctrl Character3D handle to inspect.
/// @return One when platform riding is enabled, otherwise zero.
int8_t rt_character3d_get_ride_platforms(void *ctrl);

/// @brief True while the controller rests on a surface steeper than the slope limit.
/// @param ctrl Character3D handle to inspect.
/// @return One when resting on an unwalkable slope, otherwise zero.
int8_t rt_character3d_is_sliding(void *ctrl);

/// @brief Borrowed body under the controller's feet (NULL while airborne).
/// @param ctrl Character3D handle to inspect.
/// @return Borrowed supporting Body3D handle, or NULL when airborne.
void *rt_character3d_get_ground_body(void *ctrl);

/*===========================================================================
 * Vehicle3D — raycast vehicle on a dynamic chassis body
 *==========================================================================*/

/// @brief Create a raycast vehicle around @p chassis (a dynamic Body3D in @p world).
/// @param world World3D handle used for suspension queries.
/// @param chassis Dynamic Body3D already registered in @p world; receives suspension/tire forces.
/// @return Newly allocated Vehicle3D handle, or NULL after trapping for invalid input,
///         unregistered/static chassis, or allocation failure.
void *rt_vehicle3d_new(void *world, void *chassis);

/// @brief Add a suspension wheel (chassis-local anchor, radius, suspension tuning).
/// @param vehicle Vehicle3D handle to modify.
/// @param x Chassis-local wheel-anchor X coordinate.
/// @param y Chassis-local wheel-anchor Y coordinate.
/// @param z Chassis-local wheel-anchor Z coordinate.
/// @param radius Wheel radius in world units.
/// @param suspension_rest Uncompressed suspension length.
/// @param stiffness Suspension spring stiffness.
/// @param damping Suspension velocity damping.
/// @param steers Nonzero when steering input rotates this wheel.
/// @param driven Nonzero when drive force is applied through this wheel.
/// @return Wheel index, or -1 when the table is full or inputs are degenerate.
int64_t rt_vehicle3d_add_wheel(void *vehicle,
                               double x,
                               double y,
                               double z,
                               double radius,
                               double suspension_rest,
                               double stiffness,
                               double damping,
                               int8_t steers,
                               int8_t driven);

/// @brief Per-frame controls: throttle [-1,1], brake [0,1], steer [-1,1].
/// @param vehicle Vehicle3D handle to control.
/// @param throttle Signed drive input, clamped to the inclusive range -1 to 1.
/// @param brake Braking input, clamped to the inclusive range zero to one.
/// @param steer Signed steering input, clamped to the inclusive range -1 to 1.
void rt_vehicle3d_set_input(void *vehicle, double throttle, double brake, double steer);

/// @brief Total drive force budget split across driven wheels (newtons).
/// @param vehicle Vehicle3D handle to modify.
/// @param newtons Requested non-negative total drive force.
void rt_vehicle3d_set_drive_force(void *vehicle, double newtons);

/// @brief Total brake force budget split across all wheels (newtons).
/// @param vehicle Vehicle3D handle to modify.
/// @param newtons Requested non-negative total braking force.
void rt_vehicle3d_set_brake_force(void *vehicle, double newtons);

/// @brief Full-lock steering angle in degrees (clamped to [0, 85]).
/// @param vehicle Vehicle3D handle to modify.
/// @param degrees Requested non-negative full-lock steering angle.
void rt_vehicle3d_set_max_steer(void *vehicle, double degrees);

/// @brief Tire friction coefficients (longitudinal, lateral).
/// @param vehicle Vehicle3D handle to modify.
/// @param longitudinal Forward/backward tire grip coefficient.
/// @param lateral Sideways tire grip coefficient.
void rt_vehicle3d_set_grip(void *vehicle, double longitudinal, double lateral);

/// @brief Collision layers the wheel suspension rays may hit (default -1 = all).
/// @param vehicle Vehicle3D handle to modify.
/// @param mask Bit field selecting suspension-query collision layers.
void rt_vehicle3d_set_collision_mask(void *vehicle, int64_t mask);

/// @brief Cast suspension rays and apply wheel forces; call BEFORE World3D.Step.
/// @param vehicle Vehicle3D handle to simulate.
/// @param dt Positive finite elapsed time in seconds.
void rt_vehicle3d_step(void *vehicle, double dt);

/// @brief Signed speed along the chassis forward axis (m/s).
/// @param vehicle Vehicle3D handle to inspect.
/// @return Signed forward speed in metres per second, or zero for an invalid handle.
double rt_vehicle3d_get_speed(void *vehicle);

/// @brief Number of wheels added.
/// @param vehicle Vehicle3D handle to inspect.
/// @return Number of configured wheels, or zero for an invalid handle.
int64_t rt_vehicle3d_get_wheel_count(void *vehicle);

/// @brief Whether wheel @p index touched ground during the last Step.
/// @param vehicle Vehicle3D handle to inspect.
/// @param index Zero-based wheel index.
/// @return One when the wheel had suspension contact, otherwise zero.
int8_t rt_vehicle3d_wheel_in_contact(void *vehicle, int64_t index);

/// @brief Current suspension length of wheel @p index (rest when airborne).
/// @param vehicle Vehicle3D handle to inspect.
/// @param index Zero-based wheel index.
/// @return Current suspension length, or zero when the wheel is unavailable.
double rt_vehicle3d_wheel_travel(void *vehicle, int64_t index);

/// @brief Suspension load on wheel @p index from the last Step (newtons).
/// @param vehicle Vehicle3D handle to inspect.
/// @param index Zero-based wheel index.
/// @return Latest suspension load in newtons, or zero when unavailable.
double rt_vehicle3d_wheel_load(void *vehicle, int64_t index);

#ifdef __cplusplus
}
#endif
