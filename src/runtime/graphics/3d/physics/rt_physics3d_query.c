//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_query.c
// Purpose: Spatial queries for the Physics3D runtime — raycasts (sphere/box/
//   capsule/triangle/mesh-BVH/heightfield/collider), shape sweeps, and overlap
//   tests, plus the per-query broad-phase cache and sorted hit lists.
//   Split out of rt_physics3d.c; shares core types via rt_physics3d_internal.h.
//
// Key invariants:
//   - Result lists are bounded by PH3D_MAX_QUERY_HITS and kept distance-sorted.
//   - The query broad phase is rebuilt lazily when its signature changes.
//   - Mesh raycasts traverse the shared rt_physics_mesh_bvh_node BVH, rebuilt
//     on demand via mesh_physics_bvh_rebuild (also used by narrow-phase).
//
// Ownership/Lifetime:
//   - Boxed PhysicsHit3D / PhysicsHitList3D results retain referenced bodies
//     and colliders until released by the GC.
//
// Links: rt_physics3d_internal.h, rt_physics3d.c, rt_raycast3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded spatial overlap and sweep queries for Physics3D worlds.
///
/// This translation unit owns the lazy query broad-phase cache, reusable transient
/// query colliders, sanitized hit construction, and the public sphere, AABB, and
/// capsule query entry points. Query inputs are constrained to finite runtime-safe
/// ranges before they reach the collision routines, and multi-hit results preserve
/// the world's configured capacity while reporting their untruncated logical count.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_physics3d_query_internal.h"
#include "rt_raycast3d.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define PH3D_QUERY_DISTANCE_MAX 1000000000.0

/// @brief Clamps one query coordinate to the finite range accepted by body state.
/// @param value Coordinate supplied by a public or internal query caller.
/// @return Zero for a non-finite input; otherwise @p value clamped to
///         `[-PH3D_QUERY_COORD_ABS_MAX, PH3D_QUERY_COORD_ABS_MAX]`.
double query_saturate_coord(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > PH3D_QUERY_COORD_ABS_MAX)
        return PH3D_QUERY_COORD_ABS_MAX;
    if (value < -PH3D_QUERY_COORD_ABS_MAX)
        return -PH3D_QUERY_COORD_ABS_MAX;
    return value;
}

/// @brief Reads and sanitizes a public `Vec3` query argument.
/// @param obj Borrowed runtime object expected to contain a graphics `Vec3`.
/// @param[out] out Three-element destination that receives finite, range-limited coordinates.
/// @return `1` when @p obj is a `Vec3` with finite components and @p out was populated;
///         otherwise `0`, leaving the destination unspecified.
int query_read_vec3(void *obj, double out[3]) {
    double raw[3];
    if (!out || !rt_g3d_is_vec3(obj))
        return 0;
    raw[0] = rt_vec3_x(obj);
    raw[1] = rt_vec3_y(obj);
    raw[2] = rt_vec3_z(obj);
    if (!ph3d_vec3_all_finite(raw))
        return 0;
    out[0] = query_saturate_coord(raw[0]);
    out[1] = query_saturate_coord(raw[1]);
    out[2] = query_saturate_coord(raw[2]);
    return 1;
}

/// @brief Sanitizes a public query distance or radius.
/// @param value Candidate distance in world units.
/// @return Zero when @p value is non-finite or non-positive; otherwise the value capped at
///         `PH3D_QUERY_DISTANCE_MAX`.
double query_sanitize_distance(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return value > PH3D_QUERY_DISTANCE_MAX ? PH3D_QUERY_DISTANCE_MAX : value;
}

/// @brief Sanitizes and normalizes a query direction in place.
/// @param[in,out] dir Three-element direction vector to normalize.
/// @return `1` when the sanitized vector has usable length and was normalized; `0` for a
///         null, non-finite, or effectively zero direction.
int query_normalize_direction(double dir[3]) {
    if (!ph3d_vec3_all_finite(dir))
        return 0;
    ph3d_vec3_sanitize_state(dir);
    return vec3_normalize_in_place(dir) > 1e-12;
}

/// @brief Caps a displacement vector's magnitude while preserving its direction.
/// @param[in,out] v Three-element displacement to sanitize and, when necessary, scale down.
/// @return The finite post-cap magnitude in world units, or zero when @p v is invalid or has
///         no usable length.
static double query_cap_vector_length(double v[3]) {
    double len;
    if (!ph3d_vec3_all_finite(v))
        return 0.0;
    ph3d_vec3_sanitize_state(v);
    len = vec3_len(v);
    if (!isfinite(len) || len <= 0.0)
        return 0.0;
    if (len > PH3D_QUERY_DISTANCE_MAX) {
        double scale = PH3D_QUERY_DISTANCE_MAX / len;
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
        return PH3D_QUERY_DISTANCE_MAX;
    }
    return len;
}

/// @brief Produces a normalized, finite hit normal with deterministic fallbacks.
/// @param[in,out] normal Three-element candidate normal to sanitize and normalize.
/// @param fallback_dir Optional borrowed ray or sweep direction. Its negation is used when
///        @p normal is degenerate; `(0, 1, 0)` is used if both vectors are unusable.
void query_normalize_normal(double normal[3], const double *fallback_dir) {
    ph3d_vec3_sanitize_state(normal);
    if (vec3_normalize_in_place(normal) > 1e-12)
        return;
    if (fallback_dir && ph3d_vec3_all_finite(fallback_dir)) {
        vec3_negate(fallback_dir, normal);
        ph3d_vec3_sanitize_state(normal);
        if (vec3_normalize_in_place(normal) > 1e-12)
            return;
    }
    vec3_set(normal, 0.0, 1.0, 0.0);
}

/// @brief Validates and sanitizes a raw query hit before boxing or sorting it.
/// @param[in,out] hit Hit record whose scalar fields and vectors are normalized in place.
/// @param max_distance Maximum permitted hit distance in world units. A positive value clamps
///        both the distance and any derived fraction to the query extent.
/// @param fallback_dir Optional borrowed direction used to repair a degenerate hit normal.
/// @return `1` when the record has a finite non-negative distance and was sanitized; `0` when
///         @p hit is null or its distance is invalid.
int query_sanitize_hit(rt_query_hit3d *hit, double max_distance, const double *fallback_dir) {
    if (!hit || !isfinite(hit->distance) || hit->distance < 0.0)
        return 0;
    hit->distance = query_sanitize_distance(hit->distance);
    if (max_distance > 1e-12 && hit->distance > max_distance)
        hit->distance = max_distance;
    if (!isfinite(hit->fraction)) {
        hit->fraction = max_distance > 1e-12 ? hit->distance / max_distance : 0.0;
    }
    if (hit->fraction < 0.0)
        hit->fraction = 0.0;
    if (hit->fraction > 1.0)
        hit->fraction = 1.0;
    ph3d_vec3_sanitize_state(hit->point);
    query_normalize_normal(hit->normal, fallback_dir);
    hit->started_penetrating = hit->started_penetrating ? 1 : 0;
    hit->is_trigger = hit->is_trigger ? 1 : 0;
    return 1;
}

/// @brief Insert `hit` into a distance-sorted hit array, keeping the order.
///
/// O(n) insertion (linear shift). Acceptable because `RaycastAll` /
/// `OverlapAll` queries are bounded by `PH3D_MAX_QUERY_HITS` (256).
/// @param[in,out] hits Writable array with space for the existing entries and one new entry.
/// @param count Number of initialized, ascending-distance entries already in @p hits.
/// @param hit Borrowed candidate record; the inserted copy is sanitized before comparison.
/// @return @p count plus one when the candidate is valid, or the unchanged count for invalid
///         storage or hit data.
static int query_hit_insert_sorted(rt_query_hit3d *hits, int32_t count, const rt_query_hit3d *hit) {
    int32_t pos = count;
    rt_query_hit3d clean;
    if (!hits || !hit)
        return count;
    clean = *hit;
    if (!query_sanitize_hit(&clean, PH3D_QUERY_DISTANCE_MAX, NULL))
        return count;
    while (pos > 0 && hits[pos - 1].distance > clean.distance) {
        hits[pos] = hits[pos - 1];
        pos--;
    }
    hits[pos] = clean;
    return count + 1;
}

/// @brief Insert a query hit into a distance-sorted array capped at @p capacity (keeps the
/// nearest).
/// @details Below capacity it inserts in order; when full, a hit nearer than the current farthest
///          displaces it (bounded insertion sort), so the array always holds the K closest results.
/// @param[in,out] hits Writable array containing @p count sorted entries and storage for
///        @p capacity records.
/// @param count Current number of initialized entries.
/// @param capacity Maximum number of entries that may be retained.
/// @param hit Borrowed candidate record; a sanitized copy is inserted if it belongs among the
///        nearest results.
/// @return The retained hit count, never greater than @p capacity. Invalid arguments or a
///         farther-than-capacity candidate leave the count unchanged.
int query_hit_insert_sorted_bounded(rt_query_hit3d *hits,
                                    int32_t count,
                                    int32_t capacity,
                                    const rt_query_hit3d *hit) {
    int32_t pos;
    rt_query_hit3d clean;
    if (!hits || !hit || capacity <= 0)
        return count;
    clean = *hit;
    if (!query_sanitize_hit(&clean, PH3D_QUERY_DISTANCE_MAX, NULL))
        return count;
    if (count < capacity)
        return query_hit_insert_sorted(hits, count, &clean);
    if (clean.distance >= hits[capacity - 1].distance)
        return count;
    pos = capacity - 1;
    while (pos > 0 && hits[pos - 1].distance > clean.distance) {
        hits[pos] = hits[pos - 1];
        pos--;
    }
    hits[pos] = clean;
    return capacity;
}

/// @brief Return the O(1) revision key for the world's query broadphase cache.
/// @details Body transforms/colliders and body add/remove paths bump the world revision as they
///   mutate bounds. Spatial queries can therefore validate the cache without hashing every body.
/// @param w Borrowed world whose broad-phase revision is requested.
/// @return Zero for a null world; otherwise its nonzero cache signature, using `1` as the
///         initial sentinel before the first explicit revision.
static uint64_t world3d_query_broadphase_signature(rt_world3d *w) {
    if (!w)
        return 0;
    return w->broadphase_world_revision ? w->broadphase_world_revision : 1;
}

/// @brief Whether every moved body in a still-valid cache remains inside its fat entry bounds.
/// @details Walks the cached entries and, for each body whose revision changed since the
///          build, recomputes the true AABB and tests containment against the cached
///          fattened bounds. Bodies that stayed inside are restamped (so later validates
///          skip them); the first escape aborts — the caller must rebuild.
/// @param w Borrowed world with a previously built query broad-phase cache.
/// @return 1 if the cache is still usable, 0 if any body escaped its fat bounds.
static int world3d_query_broadphase_contains_moved_bodies(rt_world3d *w) {
    for (int32_t i = 0; i < w->query_broadphase_count; ++i) {
        ph3d_broadphase_entry *entry = &w->query_broadphase_entries[i];
        rt_body3d *body = entry->body;
        if (!body || entry->body_revision == body->broadphase_revision)
            continue;
        double mn[3];
        double mx[3];
        body_aabb(body, mn, mx);
        if (mn[0] < entry->min[0] || mn[1] < entry->min[1] || mn[2] < entry->min[2] ||
            mx[0] > entry->max[0] || mx[1] > entry->max[1] || mx[2] > entry->max[2])
            return 0;
        entry->body_revision = body->broadphase_revision;
    }
    return 1;
}

/// @brief Build (or reuse) the cached broadphase entry list used to accelerate spatial queries.
/// @details Entries hold each body's world AABB fattened by PH3D_QUERY_BROADPHASE_MARGIN,
///          sorted by fat min-X for sweep-and-prune, in the cache's own array (the per-step
///          solver broadphase refills and re-sorts its separate array on the widest axis).
///          Membership/shape changes bump the world revision and force a rebuild here;
///          pose-only changes merely mark the cache dirty, and a lazy escape check keeps
///          it valid while every moved body stays inside its fat bounds. Fat entries are a
///          conservative candidate filter, so query results remain exact — every consumer
///          narrow-tests live body state.
/// @param w Borrowed world whose cache should be validated or rebuilt.
/// @return The number of cached collision-geometry entries, or `-1` for a null world or
///         scratch-storage allocation failure. On failure callers may fall back to direct body
///         iteration.
int32_t world3d_build_query_broadphase(rt_world3d *w) {
    int32_t entry_count = 0;
    int32_t geometry_count = 0;
    uint64_t signature;
    uint64_t mesh_epoch;
    uint64_t collider_epoch;
    if (!w)
        return -1;
    signature = world3d_query_broadphase_signature(w);
    mesh_epoch = rt_mesh3d_global_geometry_epoch();
    collider_epoch = rt_collider3d_global_geometry_epoch();
    if (w->query_broadphase_valid && w->query_broadphase_signature == signature &&
        w->query_broadphase_mesh_epoch == mesh_epoch &&
        w->query_broadphase_collider_epoch == collider_epoch) {
        if (!w->query_broadphase_dirty)
            return w->query_broadphase_count;
        if (world3d_query_broadphase_contains_moved_bodies(w)) {
            w->query_broadphase_dirty = 0;
            return w->query_broadphase_count;
        }
    }
    for (int32_t i = 0; i < w->body_count; ++i) {
        if (body3d_has_collision_geometry(w->bodies[i]))
            geometry_count++;
    }
    if (!world3d_reserve_query_broadphase_capacity(w, geometry_count))
        return -1;
    for (int32_t i = 0; i < w->body_count; ++i) {
        rt_body3d *body = w->bodies[i];
        if (!body3d_has_collision_geometry(body))
            continue;
        ph3d_broadphase_entry *entry = &w->query_broadphase_entries[entry_count++];
        entry->body = body;
        body_aabb(body, entry->min, entry->max);
        entry->min[0] -= PH3D_QUERY_BROADPHASE_MARGIN;
        entry->min[1] -= PH3D_QUERY_BROADPHASE_MARGIN;
        entry->min[2] -= PH3D_QUERY_BROADPHASE_MARGIN;
        entry->max[0] += PH3D_QUERY_BROADPHASE_MARGIN;
        entry->max[1] += PH3D_QUERY_BROADPHASE_MARGIN;
        entry->max[2] += PH3D_QUERY_BROADPHASE_MARGIN;
        entry->body_revision = body->broadphase_revision;
    }
    if (!world3d_reserve_broadphase_sort_scratch(w, entry_count))
        return -1;
    ph3d_broadphase_sort_entries(
        w->query_broadphase_entries, w->broadphase_sort_scratch, entry_count, 0);
    w->query_broadphase_count = entry_count;
    w->query_broadphase_signature = signature;
    w->query_broadphase_mesh_epoch = mesh_epoch;
    w->query_broadphase_collider_epoch = collider_epoch;
    w->query_broadphase_valid = 1;
    w->query_broadphase_dirty = 0;
    w->query_broadphase_rebuild_count++;
    return entry_count;
}

/// @brief Tests a broad-phase entry against a query AABB on all three axes.
/// @param entry Borrowed broad-phase entry containing inclusive minimum and maximum bounds.
/// @param query_min Borrowed three-element inclusive minimum corner.
/// @param query_max Borrowed three-element inclusive maximum corner.
/// @return `1` when all pointers are valid and the two AABBs overlap or touch; otherwise `0`.
int query_entry_overlaps_bounds(const ph3d_broadphase_entry *entry,
                                const double *query_min,
                                const double *query_max) {
    return entry && query_min && query_max && entry->max[0] >= query_min[0] &&
           entry->min[0] <= query_max[0] && entry->max[1] >= query_min[1] &&
           entry->min[1] <= query_max[1] && entry->max[2] >= query_min[2] &&
           entry->min[2] <= query_max[2];
}

/// @brief Stack-init a transient body with a collider for use by query helpers.
///
/// Static motion mode (so impulse code never touches it), identity
/// orientation, and the supplied position. `make_temp_sphere` is the
/// sphere-only variant used elsewhere; this is the general form.
/// @param[out] body Caller-owned stack body to clear and initialize.
/// @param collider Borrowed collider assigned to the transient body for the duration of a query.
/// @param position Optional borrowed three-element world position; null leaves the zero position.
static void init_temp_query_body(rt_body3d *body, void *collider, const double *position) {
    rt_body3d zero = {0};
    *body = zero;
    quat_identity(body->orientation);
    vec3_set(body->scale, 1.0, 1.0, 1.0);
    body->motion_mode = PH3D_MODE_STATIC;
    body->collider = collider;
    if (position) {
        vec3_copy(body->position, position);
        ph3d_vec3_sanitize_state(body->position);
    }
}

/// @brief Borrow the world's reusable sphere collider, creating it on first use.
/// @details Query bodies only need a collider long enough for one query operation. Reusing one
///          world-owned sphere collider removes per-query heap churn while preserving the existing
///          narrow-phase API that expects a Collider3D handle.
/// @param w World that owns the scratch collider.
/// @param radius Sanitized sphere radius for this query.
/// @return Collider3D sphere handle, or NULL on allocation failure.
static void *world3d_query_sphere_collider(rt_world3d *w, double radius) {
    if (!w)
        return NULL;
    if (!w->query_sphere_collider) {
        w->query_sphere_collider = rt_collider3d_new_sphere(radius);
    } else {
        rt_collider3d_reset_sphere_raw(w->query_sphere_collider, radius);
    }
    return w->query_sphere_collider;
}

/// @brief Prepare the reusable swept-sphere collider before transactional integration.
/// @details Existing storage is left untouched; only a world that has never run
///   a sphere query allocates. CCD sweeps subsequently resize this same collider
///   in place, eliminating the only query-shape allocation from the mutated step.
/// @param w Borrowed world that owns the query scratch collider.
/// @return `1` when a valid scratch collider exists, otherwise `0`.
int world3d_ccd_prepare_sweep_collider(rt_world3d *w) {
    if (!w)
        return 0;
    if (w->query_sphere_collider)
        return 1;
    return world3d_query_sphere_collider(w, 1.0) != NULL;
}

/// @brief Borrow the world's reusable box collider, creating it on first use.
/// @details The collider dimensions are reset in place before every AABB overlap query, avoiding
///          allocation/free pairs for the query shape.
/// @param w World that owns the scratch collider.
/// @param half Sanitized xyz half-extents.
/// @return Collider3D box handle, or NULL on allocation failure.
static void *world3d_query_box_collider(rt_world3d *w, const double half[3]) {
    if (!w || !half)
        return NULL;
    if (!w->query_box_collider) {
        w->query_box_collider = rt_collider3d_new_box(half[0], half[1], half[2]);
    } else {
        rt_collider3d_reset_box_raw(w->query_box_collider, half[0], half[1], half[2]);
    }
    return w->query_box_collider;
}

/// @brief Test a transient query body for overlap against a registered body.
///
/// Used by overlap queries — fills `out_hit` with `started_penetrating=1`
/// since the query body starts already touching the other body.
/// @param query_body Borrowed transient body describing the query volume.
/// @param other Borrowed registered body to narrow-phase test.
/// @param[out] out_hit Optional destination for the zero-distance overlap record. The record
///        borrows its body and collider pointers from @p other.
/// @return `1` when the bodies overlap, even if @p out_hit is null; otherwise `0`.
static int overlap_query_body_against_body(rt_body3d *query_body,
                                           rt_body3d *other,
                                           rt_query_hit3d *out_hit) {
    double normal[3], depth, point[3];
    void *leaf_other = NULL;
    if (!query_body || !body3d_has_collision_geometry(other))
        return 0;
    if (!test_collision(query_body, other, normal, &depth, point, NULL, &leaf_other, NULL, NULL))
        return 0;
    if (out_hit) {
        rt_query_hit3d zero = {0};
        *out_hit = zero;
        out_hit->body = other;
        out_hit->collider = leaf_other ? leaf_other : other->collider;
        vec3_copy(out_hit->point, point);
        vec3_copy(out_hit->normal, normal);
        out_hit->distance = 0.0;
        out_hit->fraction = 0.0;
        out_hit->started_penetrating = 1;
        out_hit->is_trigger = other->is_trigger;
        query_sanitize_hit(out_hit, 0.0, NULL);
    }
    return 1;
}

/// @brief Populates a sanitized query-hit record for a sphere-sweep result.
/// @param body Borrowed body struck by the sweep; may be null for a synthetic result.
/// @param start_center Borrowed three-element sphere center at the start of the sweep.
/// @param dir Borrowed normalized sweep direction.
/// @param radius Sphere radius in world units.
/// @param distance Travel distance from @p start_center to contact.
/// @param max_distance Total sweep length used to derive the normalized hit fraction.
/// @param normal Optional borrowed contact normal; the direction fallback is used when absent
///        or degenerate.
/// @param[out] out_hit Destination record. A null destination makes this function a no-op.
static void sweep_sphere_fill_hit(rt_body3d *body,
                                  const double *start_center,
                                  const double *dir,
                                  double radius,
                                  double distance,
                                  double max_distance,
                                  const double *normal,
                                  rt_query_hit3d *out_hit) {
    if (!out_hit)
        return;
    {
        rt_query_hit3d zero = {0};
        *out_hit = zero;
    }
    out_hit->body = body;
    out_hit->collider = body ? body->collider : NULL;
    out_hit->distance = query_sanitize_distance(distance);
    out_hit->fraction = max_distance > 1e-12 ? out_hit->distance / max_distance : 0.0;
    out_hit->started_penetrating = 0;
    out_hit->is_trigger = body ? body->is_trigger : 0;
    if (normal)
        vec3_copy(out_hit->normal, normal);
    else
        vec3_negate(dir, out_hit->normal);
    query_normalize_normal(out_hit->normal, dir);
    out_hit->point[0] = query_saturate_coord(start_center[0] + dir[0] * out_hit->distance -
                                             out_hit->normal[0] * radius);
    out_hit->point[1] = query_saturate_coord(start_center[1] + dir[1] * out_hit->distance -
                                             out_hit->normal[1] * radius);
    out_hit->point[2] = query_saturate_coord(start_center[2] + dir[2] * out_hit->distance -
                                             out_hit->normal[2] * radius);
    query_sanitize_hit(out_hit, max_distance, dir);
}

/// @brief Attempts exact sphere-sweep fast paths before the generic sampler runs.
/// @details Sphere targets are tested as an expanded sphere and simple box targets as an expanded
///          oriented box. Initial penetrations are intentionally rejected because the generic
///          path reports those with the appropriate overlap semantics.
/// @param start_center Borrowed three-element sphere center at the start of the sweep.
/// @param radius Swept sphere radius in world units.
/// @param delta Borrowed finite displacement vector.
/// @param other Borrowed target body.
/// @param max_distance Magnitude of @p delta in world units.
/// @param[out] out_hit Optional destination for the first non-penetrating hit.
/// @return `1` when an exact fast path produced a hit; otherwise `0`.
static int sweep_sphere_against_simple_body(const double *start_center,
                                            double radius,
                                            const double *delta,
                                            rt_body3d *other,
                                            double max_distance,
                                            rt_query_hit3d *out_hit) {
    double dir[3];
    double t = 0.0;
    double normal[3] = {0.0, 1.0, 0.0};
    int started = 0;
    if (!start_center || !delta || !body3d_has_collision_geometry(other) || max_distance <= 1e-12)
        return 0;
    vec3_copy(dir, delta);
    if (!query_normalize_direction(dir))
        return 0;
    if (other->shape == PH3D_SHAPE_SPHERE) {
        double combined_radius = radius + other->radius;
        if (!raycast_sphere_raw(start_center,
                                dir,
                                other->position,
                                combined_radius,
                                max_distance,
                                &t,
                                normal,
                                &started))
            return 0;
        if (started)
            return 0;
        sweep_sphere_fill_hit(other, start_center, dir, radius, t, max_distance, normal, out_hit);
        return 1;
    }
    if (other->shape == PH3D_SHAPE_AABB &&
        rt_collider3d_get_type(other->collider) == RT_COLLIDER3D_TYPE_BOX) {
        rt_collider_pose pose;
        collider_pose_from_body(other, &pose);
        if (!raycast_box_pose_raw(other->collider,
                                  &pose,
                                  start_center,
                                  dir,
                                  max_distance,
                                  radius,
                                  &t,
                                  normal,
                                  &started))
            return 0;
        if (started)
            return 0;
        sweep_sphere_fill_hit(other, start_center, dir, radius, t, max_distance, normal, out_hit);
        return 1;
    }
    return 0;
}

/// @brief Sweep a sphere along `delta` and find first contact with `other`.
///
/// First checks initial overlap (early-out for already-penetrating).
/// Then broad-phases against the swept AABB. Then steps along the sweep
/// in radius/body-feature-relative increments looking for the first overlap,
/// and refines the impact `t` via bisection. There is deliberately no fixed
/// world-unit minimum step: tiny spheres must still detect thin geometry.
/// @param sphere_collider Borrowed reusable collider already sized to @p radius.
/// @param start_center Borrowed three-element starting center in world coordinates.
/// @param radius Swept sphere radius in world units.
/// @param delta Borrowed displacement for the complete sweep.
/// @param other Borrowed target body to test.
/// @param max_distance Sanitized magnitude of @p delta used for hit distances and fractions.
/// @param[out] out_hit Optional destination for the earliest hit, including an initial overlap.
/// @return `1` when the sphere starts overlapping or reaches @p other during the sweep;
///         otherwise `0`.
static int sweep_sphere_against_body(void *sphere_collider,
                                     const double *start_center,
                                     double radius,
                                     const double *delta,
                                     rt_body3d *other,
                                     double max_distance,
                                     rt_query_hit3d *out_hit) {
    rt_body3d query_body;
    rt_query_hit3d cur_hit;
    double query_min[3], query_max[3], swept_min[3], swept_max[3];
    double other_min[3], other_max[3];
    double delta_len;
    double step_dist;
    int steps;
    if (!sphere_collider || !start_center || !delta || !body3d_has_collision_geometry(other))
        return 0;

    init_temp_query_body(&query_body, sphere_collider, start_center);
    body3d_update_shape_cache_from_collider(&query_body);
    if (overlap_query_body_against_body(&query_body, other, out_hit))
        return 1;

    if (sweep_sphere_against_simple_body(start_center, radius, delta, other, max_distance, out_hit))
        return 1;

    body_aabb(&query_body, query_min, query_max);
    body_aabb(other, other_min, other_max);
    swept_aabb_from_points(query_min, query_max, delta, swept_min, swept_max);
    if (!aabb_overlap_raw(swept_min, swept_max, other_min, other_max))
        return 0;

    delta_len = vec3_len(delta);
    if (delta_len <= 1e-12 || max_distance <= 0.0)
        return 0;

    {
        double other_extent_x = other_max[0] - other_min[0];
        double other_extent_y = other_max[1] - other_min[1];
        double other_extent_z = other_max[2] - other_min[2];
        double body_extent = other_extent_x;
        if (other_extent_y > body_extent)
            body_extent = other_extent_y;
        if (other_extent_z > body_extent)
            body_extent = other_extent_z;
        step_dist = radius > 1e-6 ? radius * 0.25 : body_extent * 0.05;
        if (!isfinite(step_dist) || step_dist <= 0.0)
            step_dist = 1e-4;
        if (step_dist < 1e-5)
            step_dist = 1e-5;
        if (step_dist > 0.25)
            step_dist = 0.25;
    }

    {
        double step_count = ceil(delta_len / step_dist);
        if (!isfinite(step_count) || step_count > (double)PH3D_MAX_SWEEP_STEPS)
            steps = PH3D_MAX_SWEEP_STEPS;
        else if (step_count < 1.0)
            steps = 1;
        else
            steps = (int)step_count;
    }
    if (steps < 1)
        steps = 1;

    {
        double prev_t = 0.0;
        for (int s = 1; s <= steps; ++s) {
            double t = (double)s / (double)steps;
            double center[3] = {
                query_saturate_coord(start_center[0] + delta[0] * t),
                query_saturate_coord(start_center[1] + delta[1] * t),
                query_saturate_coord(start_center[2] + delta[2] * t),
            };
            init_temp_query_body(&query_body, sphere_collider, center);
            body3d_update_shape_cache_from_collider(&query_body);
            if (!overlap_query_body_against_body(&query_body, other, &cur_hit)) {
                prev_t = t;
                continue;
            }
            {
                double lo = prev_t;
                double hi = t;
                rt_query_hit3d best = cur_hit;
                for (int iter = 0; iter < 14; ++iter) {
                    double mid = (lo + hi) * 0.5;
                    double mid_center[3] = {
                        query_saturate_coord(start_center[0] + delta[0] * mid),
                        query_saturate_coord(start_center[1] + delta[1] * mid),
                        query_saturate_coord(start_center[2] + delta[2] * mid),
                    };
                    init_temp_query_body(&query_body, sphere_collider, mid_center);
                    body3d_update_shape_cache_from_collider(&query_body);
                    if (overlap_query_body_against_body(&query_body, other, &cur_hit)) {
                        hi = mid;
                        best = cur_hit;
                    } else {
                        lo = mid;
                    }
                }
                best.distance = hi * max_distance;
                best.fraction = hi;
                best.started_penetrating = 0;
                query_sanitize_hit(&best, max_distance, delta);
                if (out_hit)
                    *out_hit = best;
                return 1;
            }
        }
    }
    return 0;
}

/// @brief Sweep a capsule (axis from `a` to `b`, radius `radius`) along `delta`.
///
/// Approximates the capsule sweep as adaptive sphere-sweeps sampled
/// along the axis. Picks the closest hit.
/// @param a Borrowed three-element first endpoint of the capsule axis.
/// @param b Borrowed three-element second endpoint of the capsule axis.
/// @param radius Capsule radius in world units.
/// @param sphere_collider Borrowed reusable sphere collider sized to @p radius.
/// @param delta Borrowed displacement for the complete sweep.
/// @param other Borrowed target body.
/// @param max_distance Sanitized magnitude of @p delta.
/// @param[out] out_hit Optional destination for the nearest sampled-sphere hit.
/// @return `1` when any sampled sphere overlaps or sweeps into @p other; otherwise `0`.
static int sweep_capsule_against_body(const double *a,
                                      const double *b,
                                      double radius,
                                      void *sphere_collider,
                                      const double *delta,
                                      rt_body3d *other,
                                      double max_distance,
                                      rt_query_hit3d *out_hit) {
    double axis[3];
    double axis_len;
    int samples;
    int hit = 0;
    rt_query_hit3d best = {0};
    if (!a || !b || !sphere_collider || !delta || !body3d_has_collision_geometry(other))
        return 0;
    vec3_sub(b, a, axis);
    axis_len = vec3_len(axis);
    if (!isfinite(axis_len))
        axis_len = 0.0;
    samples = capsule_axis_sample_count(axis_len, radius);
    for (int i = 0; i < samples; ++i) {
        double t = samples == 1 ? 0.5 : (double)i / (double)(samples - 1);
        double center[3] = {
            query_saturate_coord(a[0] + axis[0] * t),
            query_saturate_coord(a[1] + axis[1] * t),
            query_saturate_coord(a[2] + axis[2] * t),
        };
        rt_query_hit3d cur_hit;
        if (!sweep_sphere_against_body(
                sphere_collider, center, radius, delta, other, max_distance, &cur_hit))
            continue;
        if (!hit || cur_hit.distance < best.distance) {
            best = cur_hit;
            query_sanitize_hit(&best, max_distance, delta);
            hit = 1;
        }
    }
    if (hit && out_hit)
        *out_hit = best;
    return hit;
}

/// @brief `World3D.OverlapSphere(center, radius, mask)` — list bodies overlapping a sphere.
///
/// Builds a transient sphere collider, then tests every world body
/// (after layer/mask filter) for overlap. Returns up to
/// the configured query cap as a `PhysicsHitList3D`. The query body is
/// stack-local while its collider is reusable storage owned by the world.
/// @param obj Borrowed `World3D` runtime object.
/// @param center_obj Borrowed `Vec3` containing the query center in world coordinates.
/// @param radius Non-negative sphere radius in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed `PhysicsHitList3D`, possibly empty and marked truncated when the logical
///         result exceeds the configured capacity; or null for an invalid world/vector/radius or
///         scratch-collider allocation failure.
void *rt_world3d_overlap_sphere(void *obj, void *center_obj, double radius, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d *hits = world3d_query_hits_scratch(w);
    int32_t hit_capacity = w ? w->max_query_hits : 0;
    int32_t hit_count = 0;
    int64_t total_count = 0;
    double center[3];
    double query_min[3], query_max[3];
    rt_body3d query_body;
    void *sphere_collider;
    if (!w || !rt_g3d_is_vec3(center_obj) || !isfinite(radius) || radius < 0.0)
        return NULL;
    if (!query_read_vec3(center_obj, center))
        return NULL;
    radius = query_sanitize_distance(radius);
    sphere_collider = world3d_query_sphere_collider(w, radius);
    if (!sphere_collider)
        return NULL;
    init_temp_query_body(&query_body, sphere_collider, center);
    body3d_update_shape_cache_from_collider(&query_body);
    body_aabb(&query_body, query_min, query_max);
    int32_t entry_count = world3d_build_query_broadphase(w);
    for (int32_t i = 0; i < (entry_count >= 0 ? entry_count : w->body_count); ++i) {
        rt_body3d *body = entry_count >= 0 ? w->query_broadphase_entries[i].body : w->bodies[i];
        rt_query_hit3d hit;
        if (entry_count >= 0) {
            if (w->query_broadphase_entries[i].min[0] > query_max[0])
                break;
            if (!query_entry_overlaps_bounds(&w->query_broadphase_entries[i], query_min, query_max))
                continue;
        }
        if (!body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (overlap_query_body_against_body(&query_body, body, &hit)) {
            total_count++;
            if (hits && hit_count < hit_capacity)
                hits[hit_count++] = hit;
        }
    }
    return physics_hit_list3d_new_ex(
        hits, hit_count, total_count, total_count > (int64_t)hit_count);
}

/// @brief `World3D.OverlapAabb(min, max, mask)` — list bodies overlapping a box.
///
/// Same pattern as `OverlapSphere` but uses a transient box collider
/// sized from the (min, max) corners. The half-extents are derived
/// from the corner spread; the center is the midpoint.
/// @param obj Borrowed `World3D` runtime object.
/// @param min_obj Borrowed `Vec3` containing one AABB corner.
/// @param max_obj Borrowed `Vec3` containing the opposite AABB corner. Corner order does not
///        matter because absolute component spreads determine the half-extents.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed `PhysicsHitList3D`, possibly empty or truncated; or null for invalid
///         runtime objects, non-finite coordinates, or scratch-collider allocation failure.
void *rt_world3d_overlap_aabb(void *obj, void *min_obj, void *max_obj, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d *hits = world3d_query_hits_scratch(w);
    int32_t hit_capacity = w ? w->max_query_hits : 0;
    int32_t hit_count = 0;
    int64_t total_count = 0;
    double mn[3], mx[3], center[3], half[3];
    double query_min[3], query_max[3];
    rt_body3d query_body;
    void *box_collider;
    if (!w || !rt_g3d_is_vec3(min_obj) || !rt_g3d_is_vec3(max_obj))
        return NULL;
    if (!query_read_vec3(min_obj, mn) || !query_read_vec3(max_obj, mx))
        return NULL;
    center[0] = query_saturate_coord((mn[0] + mx[0]) * 0.5);
    center[1] = query_saturate_coord((mn[1] + mx[1]) * 0.5);
    center[2] = query_saturate_coord((mn[2] + mx[2]) * 0.5);
    half[0] = query_sanitize_distance(fabs(mx[0] - mn[0]) * 0.5);
    half[1] = query_sanitize_distance(fabs(mx[1] - mn[1]) * 0.5);
    half[2] = query_sanitize_distance(fabs(mx[2] - mn[2]) * 0.5);
    if (!ph3d_vec3_all_finite(center) || !ph3d_vec3_all_finite(half))
        return NULL;
    box_collider = world3d_query_box_collider(w, half);
    if (!box_collider)
        return NULL;
    init_temp_query_body(&query_body, box_collider, center);
    body3d_update_shape_cache_from_collider(&query_body);
    body_aabb(&query_body, query_min, query_max);
    int32_t entry_count = world3d_build_query_broadphase(w);
    for (int32_t i = 0; i < (entry_count >= 0 ? entry_count : w->body_count); ++i) {
        rt_body3d *body = entry_count >= 0 ? w->query_broadphase_entries[i].body : w->bodies[i];
        rt_query_hit3d hit;
        if (entry_count >= 0) {
            if (w->query_broadphase_entries[i].min[0] > query_max[0])
                break;
            if (!query_entry_overlaps_bounds(&w->query_broadphase_entries[i], query_min, query_max))
                continue;
        }
        if (!body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (overlap_query_body_against_body(&query_body, body, &hit)) {
            total_count++;
            if (hits && hit_count < hit_capacity)
                hits[hit_count++] = hit;
        }
    }
    return physics_hit_list3d_new_ex(
        hits, hit_count, total_count, total_count > (int64_t)hit_count);
}

/// @brief `World3D.SweepSphere(center, radius, delta, mask)` — first hit along a sphere sweep.
///
/// Returns the closest hit as a `PhysicsHit3D`, or NULL if the sweep
/// reaches `delta` without contact. Used for trajectory predictions
/// and projectile collision.
/// @param obj Borrowed `World3D` runtime object.
/// @param center_obj Borrowed `Vec3` containing the starting center.
/// @param radius Non-negative swept sphere radius in world units.
/// @param delta_obj Borrowed `Vec3` displacement; extreme magnitudes are capped in place in the
///        local query copy.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed nearest `PhysicsHit3D`, including an initial-penetration hit, or null
///         when the query is invalid, scratch allocation fails, or no selected body is hit.
void *rt_world3d_sweep_sphere(
    void *obj, void *center_obj, double radius, void *delta_obj, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d best_hit = {0};
    int found = 0;
    double center[3], delta[3];
    double query_min[3], query_max[3], swept_min[3], swept_max[3];
    double max_distance;
    rt_body3d query_body;
    void *sphere_collider;
    if (!w || !rt_g3d_is_vec3(center_obj) || !rt_g3d_is_vec3(delta_obj) || !isfinite(radius) ||
        radius < 0.0)
        return NULL;
    if (!query_read_vec3(center_obj, center) || !query_read_vec3(delta_obj, delta))
        return NULL;
    radius = query_sanitize_distance(radius);
    max_distance = query_cap_vector_length(delta);
    sphere_collider = world3d_query_sphere_collider(w, radius);
    if (!sphere_collider)
        return NULL;
    init_temp_query_body(&query_body, sphere_collider, center);
    body3d_update_shape_cache_from_collider(&query_body);
    body_aabb(&query_body, query_min, query_max);
    swept_aabb_from_points(query_min, query_max, delta, swept_min, swept_max);
    int32_t entry_count = world3d_build_query_broadphase(w);
    for (int32_t i = 0; i < (entry_count >= 0 ? entry_count : w->body_count); ++i) {
        rt_body3d *body = entry_count >= 0 ? w->query_broadphase_entries[i].body : w->bodies[i];
        rt_query_hit3d hit;
        if (entry_count >= 0) {
            if (w->query_broadphase_entries[i].min[0] > swept_max[0])
                break;
            if (!query_entry_overlaps_bounds(&w->query_broadphase_entries[i], swept_min, swept_max))
                continue;
        }
        if (!body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (!sweep_sphere_against_body(
                sphere_collider, center, radius, delta, body, max_distance, &hit))
            continue;
        if (!query_sanitize_hit(&hit, max_distance, delta))
            continue;
        if (!found || hit.distance < best_hit.distance) {
            best_hit = hit;
            found = 1;
        }
    }
    return found ? physics_hit3d_new(&best_hit) : NULL;
}

/// @brief Returns the world's reusable bounded query-hit scratch array.
/// @details The configured capacity is first constrained to the supported minimum and maximum.
///          Storage is allocated lazily and remains owned by the world; callers may populate at
///          most `w->max_query_hits` records and must not retain the pointer after world teardown.
/// @param w Borrowed world that owns the scratch allocation.
/// @return Borrowed writable hit storage, or null for a null world or allocation failure.
rt_query_hit3d *world3d_query_hits_scratch(rt_world3d *w) {
    if (!w)
        return NULL;
    if (w->max_query_hits < PH3D_QUERY_HITS_MIN)
        w->max_query_hits = PH3D_QUERY_HITS_MIN;
    if (w->max_query_hits > PH3D_QUERY_HITS_MAX)
        w->max_query_hits = PH3D_QUERY_HITS_MAX;
    if (!w->query_hits_scratch) {
        w->query_hits_scratch = malloc((size_t)w->max_query_hits * sizeof(rt_query_hit3d));
    }
    return (rt_query_hit3d *)w->query_hits_scratch;
}

/// @brief Finds the earliest blocking body along one raw CCD sphere sweep.
/// @details Candidates are read directly from the world instead of the lazily cached query
///          broad phase because dynamic poses can change during each integration substep.
///          Dynamic targets use relative displacement, mutual collision layers are enforced,
///          triggers and sleeping dynamic targets are skipped, and initial penetrations are
///          left to the persistent-contact solver.
/// @param w Borrowed world containing candidate bodies and the reusable sphere collider.
/// @param center Borrowed three-element starting center in world coordinates.
/// @param radius Positive swept sphere radius in world units.
/// @param delta Borrowed moving body's displacement for this substep.
/// @param ignore_body Optional borrowed moving body to exclude from candidates and use for
///        layer/mask and CCD-opt-in checks.
/// @param sub_dt Substep duration in seconds, used to convert a dynamic target's velocity into
///        relative displacement when positive and finite.
/// @param[out] out_t Optional destination for the earliest time-of-impact fraction in `[0, 1]`.
/// @param[out] out_normal Optional three-element destination for the normalized hit normal.
/// @param[out] out_hit Optional destination for the complete borrowed-pointer hit record.
/// @return `1` when a non-trigger blocking hit is found; `0` for invalid inputs, scratch-collider
///         allocation failure, or a sweep with no qualifying contact.
int world3d_ccd_sweep_sphere_raw(rt_world3d *w,
                                 const double *center,
                                 double radius,
                                 const double *delta,
                                 const rt_body3d *ignore_body,
                                 double sub_dt,
                                 double *out_t,
                                 double *out_normal,
                                 rt_query_hit3d *out_hit) {
    double best_toi = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    rt_query_hit3d best_hit = {0};
    int found = 0;
    double max_distance;
    void *sphere_collider;
    if (!w || !center || !delta || !isfinite(radius) || radius <= 0.0)
        return 0;
    max_distance = vec3_len(delta);
    if (!isfinite(max_distance) || max_distance <= 1e-12)
        return 0;

    sphere_collider = world3d_query_sphere_collider(w, radius);
    if (!sphere_collider)
        return 0;

    /* Direct body iteration: CCD targets' poses are read directly each substep,
     * so the cached query broadphase — which is invalidated as dynamic bodies
     * integrate — is deliberately not used here. */
    for (int32_t i = 0; i < w->body_count; ++i) {
        rt_body3d *body = w->bodies[i];
        rt_query_hit3d hit;
        double sweep_delta[3];
        double sweep_len;
        double toi;
        int target_is_dynamic;
        if (!body || body == ignore_body)
            continue;
        target_is_dynamic = body->motion_mode == PH3D_MODE_DYNAMIC;
        /* Dynamic-vs-dynamic pairs sweep with the RELATIVE displacement (the
         * target keeps moving during the substep) — but only when at least one
         * side opted into CCD, so fast projectiles stop tunneling through fast
         * targets without taxing ordinary pairs. */
        if (target_is_dynamic && !(ignore_body && ignore_body->use_ccd) && !body->use_ccd)
            continue;
        if (target_is_dynamic && body->is_sleeping)
            continue;
        if (!body3d_has_collision_geometry(body))
            continue;
        /* Honor the mutual layer/mask contract (same rule the solver applies). */
        if (ignore_body && (!(ignore_body->collision_layer & body->collision_mask) ||
                            !(body->collision_layer & ignore_body->collision_mask)))
            continue;
        /* Triggers report overlaps but never block motion. */
        if (body->is_trigger)
            continue;
        vec3_copy(sweep_delta, delta);
        if (target_is_dynamic && isfinite(sub_dt) && sub_dt > 0.0) {
            sweep_delta[0] -= body->velocity[0] * sub_dt;
            sweep_delta[1] -= body->velocity[1] * sub_dt;
            sweep_delta[2] -= body->velocity[2] * sub_dt;
        }
        sweep_len = vec3_len(sweep_delta);
        if (!isfinite(sweep_len) || sweep_len <= 1e-12)
            continue;
        if (!sweep_sphere_against_body(
                sphere_collider, center, radius, sweep_delta, body, sweep_len, &hit))
            continue;
        if (!query_sanitize_hit(&hit, sweep_len, sweep_delta))
            continue;
        /* Persistent contacts belong to the impulse solver: a body resting on
         * (or rolling along) a surface starts every substep already touching
         * it, and clipping that sweep at t=0 would freeze tangential motion
         * entirely. CCD only guards surfaces the body is separated from at
         * substep start — which is exactly the anti-tunneling case. */
        if (hit.started_penetrating)
            continue;
        toi = hit.distance / sweep_len;
        if (!isfinite(toi) || toi < 0.0 || toi > 1.0)
            continue;
        if (!found || toi < best_toi) {
            best_toi = toi;
            best_normal[0] = hit.normal[0];
            best_normal[1] = hit.normal[1];
            best_normal[2] = hit.normal[2];
            best_hit = hit;
            found = 1;
        }
    }

    if (!found)
        return 0;
    if (out_t)
        *out_t = best_toi;
    if (out_normal) {
        out_normal[0] = best_normal[0];
        out_normal[1] = best_normal[1];
        out_normal[2] = best_normal[2];
    }
    if (out_hit)
        *out_hit = best_hit;
    return 1;
}

/// @brief Records every trigger crossed by one conservative local CCD segment.
/// @details Each qualifying trigger is swept using displacement relative to its dynamic motion.
///          A synthetic trigger contact is oriented to the solver's body-A-to-body-B convention
///          and appended uniquely to the frame-contact list. Invalid or effectively stationary
///          segments are successful no-ops.
/// @param w Borrowed world containing trigger candidates and frame-contact storage.
/// @param center Borrowed three-element starting center of the moving sphere.
/// @param radius Positive sphere radius in world units.
/// @param delta Borrowed displacement of @p moving_body over the segment.
/// @param moving_body Borrowed body whose trigger crossings are being recorded.
/// @param segment_dt Segment duration in seconds, used for relative motion and contact speed.
/// @return `1` when all crossings were recorded or the segment is a no-op; `0` when reusable
///         collider allocation or frame-contact growth fails.
int world3d_ccd_record_trigger_crossings(rt_world3d *w,
                                         const double *center,
                                         double radius,
                                         const double *delta,
                                         rt_body3d *moving_body,
                                         double segment_dt) {
    double max_distance;
    void *sphere_collider;
    if (!w || !center || !delta || !moving_body || !isfinite(radius) || radius <= 0.0)
        return 1;
    max_distance = vec3_len(delta);
    if (!isfinite(max_distance) || max_distance <= 1e-12)
        return 1;
    sphere_collider = world3d_query_sphere_collider(w, radius);
    if (!sphere_collider)
        return 0;

    for (int32_t i = 0; i < w->body_count; ++i) {
        rt_body3d *trigger = w->bodies[i];
        rt_query_hit3d hit;
        rt_contact3d contact;
        double sweep_delta[3];
        double sweep_len;
        if (!trigger || trigger == moving_body || !trigger->is_trigger ||
            !body3d_has_collision_geometry(trigger))
            continue;
        if (!(moving_body->collision_layer & trigger->collision_mask) ||
            !(trigger->collision_layer & moving_body->collision_mask))
            continue;
        vec3_copy(sweep_delta, delta);
        if (trigger->motion_mode == PH3D_MODE_DYNAMIC && isfinite(segment_dt) && segment_dt > 0.0) {
            sweep_delta[0] -= trigger->velocity[0] * segment_dt;
            sweep_delta[1] -= trigger->velocity[1] * segment_dt;
            sweep_delta[2] -= trigger->velocity[2] * segment_dt;
        }
        sweep_len = vec3_len(sweep_delta);
        if (!isfinite(sweep_len) || sweep_len <= 1e-12 ||
            !sweep_sphere_against_body(
                sphere_collider, center, radius, sweep_delta, trigger, sweep_len, &hit))
            continue;
        memset(&contact, 0, sizeof(contact));
        contact.body_a = trigger;
        contact.body_b = moving_body;
        contact.collider_a = hit.collider ? hit.collider : trigger->collider;
        contact.collider_b = moving_body->collider;
        vec3_copy(contact.point, hit.point);
        vec3_copy(contact.normal, hit.normal);
        /* Contact normals use the solver's A->B convention. Query fallbacks
           can report the opposite orientation, so force the moving relative
           displacement to be approaching (negative projected velocity). */
        if (vec3_dot(sweep_delta, contact.normal) > 0.0)
            vec3_scale_in_place(contact.normal, -1.0);
        contact.separation = 0.0;
        contact3d_init_single_point(&contact, contact.point, contact.normal, 0.0);
        contact.relative_speed =
            fabs(vec3_dot(sweep_delta, contact.normal)) / (segment_dt > 1e-12 ? segment_dt : 1.0);
        contact.is_trigger = 1;
        if (!world3d_append_frame_contact_unique(w, &contact))
            return 0;
    }
    return 1;
}

/// @brief `World3D.SweepCapsule(a, b, radius, delta, mask)` — first hit along a capsule sweep.
///
/// Like `SweepSphere` but for capsule queries — primary use case is
/// character-controller motion against world geometry.
/// @param obj Borrowed `World3D` runtime object.
/// @param a_obj Borrowed `Vec3` containing the first starting axis endpoint.
/// @param b_obj Borrowed `Vec3` containing the second starting axis endpoint.
/// @param radius Non-negative capsule radius in world units.
/// @param delta_obj Borrowed `Vec3` displacement applied to the entire capsule.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed nearest `PhysicsHit3D`, or null for invalid arguments, scratch-collider
///         allocation failure, or a sweep with no selected contact.
void *rt_world3d_sweep_capsule(
    void *obj, void *a_obj, void *b_obj, double radius, void *delta_obj, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d best_hit = {0};
    int found = 0;
    double a[3], b[3], delta[3];
    double query_min[3], query_max[3], swept_min[3], swept_max[3];
    double max_distance;
    void *sphere_collider;
    if (!w || !rt_g3d_is_vec3(a_obj) || !rt_g3d_is_vec3(b_obj) || !rt_g3d_is_vec3(delta_obj) ||
        !isfinite(radius) || radius < 0.0)
        return NULL;
    if (!query_read_vec3(a_obj, a) || !query_read_vec3(b_obj, b) ||
        !query_read_vec3(delta_obj, delta))
        return NULL;
    radius = query_sanitize_distance(radius);
    max_distance = query_cap_vector_length(delta);
    sphere_collider = world3d_query_sphere_collider(w, radius);
    if (!sphere_collider)
        return NULL;
    for (int axis = 0; axis < 3; ++axis) {
        query_min[axis] = query_saturate_coord(fmin(a[axis], b[axis]) - radius);
        query_max[axis] = query_saturate_coord(fmax(a[axis], b[axis]) + radius);
    }
    swept_aabb_from_points(query_min, query_max, delta, swept_min, swept_max);
    int32_t entry_count = world3d_build_query_broadphase(w);
    for (int32_t i = 0; i < (entry_count >= 0 ? entry_count : w->body_count); ++i) {
        rt_body3d *body = entry_count >= 0 ? w->query_broadphase_entries[i].body : w->bodies[i];
        rt_query_hit3d hit;
        if (entry_count >= 0) {
            if (w->query_broadphase_entries[i].min[0] > swept_max[0])
                break;
            if (!query_entry_overlaps_bounds(&w->query_broadphase_entries[i], swept_min, swept_max))
                continue;
        }
        if (!body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (!sweep_capsule_against_body(
                a, b, radius, sphere_collider, delta, body, max_distance, &hit))
            continue;
        if (!query_sanitize_hit(&hit, max_distance, delta))
            continue;
        if (!found || hit.distance < best_hit.distance) {
            best_hit = hit;
            found = 1;
        }
    }
    return found ? physics_hit3d_new(&best_hit) : NULL;
}

#else
typedef int rt_physics3d_query_core_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
