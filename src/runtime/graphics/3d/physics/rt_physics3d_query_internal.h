//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_query_internal.h
// Purpose: Shared spatial-query helpers for the 3D physics queries, split
//          between rt_physics3d_query.c (sweep/overlap) and
//          rt_physics3d_raycast.c (ray casting against shapes + mesh BVH).
//
// Key invariants:
//   - Engine-internal; included only by the physics/ query translation units.
//   - Coordinate/distance sanitizers clamp to PH3D_QUERY_COORD_ABS_MAX so a
//     hostile transform can't push the broadphase into non-finite territory.
//
// Ownership/Lifetime:
//   - Helpers borrow caller-owned world/hit buffers; no allocation here.
//
// Links: src/runtime/graphics/3d/physics/rt_physics3d_query.c (sweep/overlap),
//        src/runtime/graphics/3d/physics/rt_physics3d_raycast.c (raycast),
//        src/runtime/graphics/3d/physics/rt_physics3d_internal.h (shared types)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared internal sanitizers, query caches, CCD sweeps, and ray primitives.
///
/// These functions connect the overlap/sweep and raycast translation units. All pointer
/// arguments are borrowed unless an output annotation says otherwise; world-owned scratch
/// storage remains valid only for the lifetime of its world.

#pragma once

#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"

#include <stdint.h>

#define PH3D_QUERY_COORD_ABS_MAX 1000000000000.0

/// @brief Clamps one query coordinate to the Physics3D finite safety range.
/// @param value Candidate coordinate.
/// @return Zero for a non-finite input; otherwise the coordinate clamped to
///         `PH3D_QUERY_COORD_ABS_MAX` in magnitude.
double query_saturate_coord(double value);

/// @brief Reads a public `Vec3` object into a sanitized native vector.
/// @param obj Borrowed runtime object expected to contain a graphics `Vec3`.
/// @param[out] out Three-element destination for finite, range-limited coordinates.
/// @return `1` on success, or `0` for an invalid object, destination, or component.
int query_read_vec3(void *obj, double out[3]);

/// @brief Constrains a distance or radius to the supported non-negative query range.
/// @param value Candidate distance in world units.
/// @return Zero for a non-finite or non-positive input; otherwise the capped distance.
double query_sanitize_distance(double value);

/// @brief Sanitizes and normalizes a three-dimensional direction in place.
/// @param[in,out] dir Direction to normalize.
/// @return `1` for a usable normalized direction, or `0` for an invalid or negligible vector.
int query_normalize_direction(double dir[3]);

/// @brief Normalizes a hit normal and repairs degenerate values deterministically.
/// @param[in,out] normal Candidate normal to sanitize.
/// @param fallback_dir Optional borrowed direction whose negation is the first fallback.
void query_normalize_normal(double normal[3], const double *fallback_dir);

/// @brief Validates and normalizes a raw query-hit record.
/// @param[in,out] hit Record whose distance, fraction, vectors, and Boolean flags are sanitized.
/// @param max_distance Positive upper bound for distance and the denominator for derived fractions.
/// @param fallback_dir Optional borrowed direction used to repair a degenerate normal.
/// @return `1` when @p hit has a valid non-negative distance; otherwise `0`.
int query_sanitize_hit(rt_query_hit3d *hit, double max_distance, const double *fallback_dir);

/// @brief Accumulates the nearest hits in a bounded max-heap.
/// @param[in,out] hits Writable heap storage with room for @p capacity records.
/// @param count Number of initialized heap records.
/// @param capacity Maximum retained record count.
/// @param hit Borrowed candidate record, copied and sanitized before insertion.
/// @return The resulting retained count, never greater than @p capacity.
int query_hit_heap_collect_bounded(rt_query_hit3d *hits,
                                   int32_t count,
                                   int32_t capacity,
                                   const rt_query_hit3d *hit);

/// @brief Converts accumulated query hits to ascending-distance result order.
void query_hit_sort_by_distance(rt_query_hit3d *hits, int32_t count);

/// @brief Validates or rebuilds the world's lazily cached query broad phase.
/// @param w Borrowed world whose collision-geometry bounds should be indexed.
/// @return The number of cached entries, or `-1` when the world or scratch allocation is invalid.
int32_t world3d_build_query_broadphase(rt_world3d *w);

/// @brief Returns lazily allocated world-owned hit scratch storage.
/// @details The buffer is sized to the clamped `w->max_query_hits` setting configured through
///          `Physics3DWorld.SetMaxQueryHits` and is reused by later queries.
/// @param w Borrowed world that owns the allocation.
/// @return Borrowed writable storage, or null for a null world or allocation failure.
rt_query_hit3d *world3d_query_hits_scratch(rt_world3d *w);

/// @brief Tests whether a broad-phase entry overlaps an inclusive query AABB.
/// @param entry Borrowed entry with cached minimum and maximum bounds.
/// @param query_min Borrowed three-element minimum corner.
/// @param query_max Borrowed three-element maximum corner.
/// @return `1` when the bounds overlap or touch on every axis; otherwise `0`.
int query_entry_overlaps_bounds(const ph3d_broadphase_entry *entry,
                                const double *query_min,
                                const double *query_max);

/// @brief Ensure body-local CCD can sweep without allocating after integration.
/// @details Lazily creates the world's reusable sphere collider before the
///   Step transaction mutates any body. Subsequent radius changes reset that
///   collider in place and therefore cannot fail mid-transaction.
/// @param w Borrowed world that owns the reusable query collider.
/// @return `1` when the collider is ready, otherwise `0` after allocation failure.
int world3d_ccd_prepare_sweep_collider(rt_world3d *w);

/// @brief Sweep one conservative CCD proxy and return the earliest blocking hit.
/// @details Tests static/kinematic bodies plus eligible dynamic bodies using
///   relative displacement over @p sub_dt. Triggers and @p ignore_body are
///   skipped. The caller derives @p center and @p radius from the complete
///   moving collider bounds, so this primitive cannot omit a collider corner.
/// @param w Borrowed world containing the candidate bodies and reusable sphere collider.
/// @param center Borrowed three-element sweep origin in world coordinates.
/// @param radius Positive conservative proxy radius in world units.
/// @param delta Borrowed moving-body displacement over this substep.
/// @param ignore_body Optional borrowed moving body to exclude and use for filtering.
/// @param sub_dt Substep duration in seconds for computing relative dynamic-body displacement.
/// @param[out] out_t Optional destination for the earliest impact fraction in `[0, 1]`.
/// @param[out] out_normal Optional three-element destination for the hit normal.
/// @param[out] out_hit Optional raw target, collider, and contact-point snapshot.
/// @return `1` for a separated time of impact, otherwise `0`.
int world3d_ccd_sweep_sphere_raw(rt_world3d *w,
                                 const double *center,
                                 double radius,
                                 const double *delta,
                                 const rt_body3d *ignore_body,
                                 double sub_dt,
                                 double *out_t,
                                 double *out_normal,
                                 rt_query_hit3d *out_hit);

/// @brief Record transient trigger crossings along one local CCD segment.
/// @details Sweeps the same conservative proxy used by blocking CCD, creates a
///   trigger contact for every crossed trigger, and appends it to the world's
///   frame-wide unique contact set. This preserves enter/exit behavior without
///   globally subdividing and re-solving the whole world.
/// @param w Borrowed world containing trigger bodies and frame-contact storage.
/// @param center Borrowed three-element starting center of the conservative proxy.
/// @param radius Positive proxy radius in world units.
/// @param delta Borrowed moving-body displacement over the segment.
/// @param moving_body Borrowed body whose crossings are being recorded.
/// @param segment_dt Segment duration in seconds for relative motion and speed calculation.
/// @return `1` on success, or `0` if the frame-contact buffer cannot grow.
int world3d_ccd_record_trigger_crossings(rt_world3d *w,
                                         const double *center,
                                         double radius,
                                         const double *delta,
                                         rt_body3d *moving_body,
                                         double segment_dt);

/// @brief Intersects a normalized ray with a sphere.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param center Borrowed three-element sphere center.
/// @param radius Non-negative sphere radius in world units.
/// @param max_distance Non-negative maximum accepted ray distance.
/// @param[out] out_t Optional destination for the entry distance, or zero for an interior start.
/// @param[out] out_normal Optional three-element destination for the outward hit normal.
/// @param[out] out_started Optional destination set to `1` when the origin is inside or on the
///        sphere, and `0` for an exterior entry hit.
/// @return `1` when the ray starts in or intersects the sphere within @p max_distance; otherwise
///         `0`.
int raycast_sphere_raw(const double *origin,
                       const double *dir,
                       const double *center,
                       double radius,
                       double max_distance,
                       double *out_t,
                       double *out_normal,
                       int *out_started);

/// @brief Intersects a normalized world-space ray with an optionally expanded posed box.
/// @details The ray is transformed into collider-local space, the box half-extents are expanded
///          by the world-space sphere radius adjusted for pose scale, and the resulting local AABB
///          is tested with the slab method.
/// @param box_collider Borrowed box collider supplying its unscaled half-extents.
/// @param pose Borrowed world transform and scale for the collider.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized world-space ray direction.
/// @param max_distance Non-negative maximum accepted distance in world units.
/// @param expand_radius Non-negative world-space radius added around the box, or zero for a ray.
/// @param[out] out_t Optional destination for entry distance, or zero for an interior start.
/// @param[out] out_normal Optional three-element destination for the normalized world-space normal.
/// @param[out] out_started Optional destination set to `1` for an interior start and `0` otherwise.
/// @return `1` when the ray starts in or intersects the expanded box within @p max_distance;
///         otherwise `0`.
int raycast_box_pose_raw(void *box_collider,
                         const rt_collider_pose *pose,
                         const double *origin,
                         const double *dir,
                         double max_distance,
                         double expand_radius,
                         double *out_t,
                         double *out_normal,
                         int *out_started);
