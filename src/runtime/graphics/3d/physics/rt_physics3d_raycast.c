//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_raycast.c
// Purpose: Ray-casting queries for the 3D physics world — ray vs sphere/AABB/
//          box/mesh (with a triangle BVH) and the rt_world3d_raycast entry
//          point. Split out of rt_physics3d_query.c; shares query sanitizers
//          and the broadphase via rt_physics3d_query_internal.h.
//
// Key invariants:
//   - Mirrors the sweep/overlap side: every hit is sanitized (finite, within
//     max distance, normalized normal) before being returned.
//   - The mesh BVH is rebuilt lazily and bounds-checked against the ray AABB.
//
// Ownership/Lifetime:
//   - Borrows the caller's world/colliders; fills caller-provided hit buffers.
//
// Links: src/runtime/graphics/3d/physics/rt_physics3d_query.c (sweep/overlap),
//        src/runtime/graphics/3d/physics/rt_physics3d_query_internal.h (helpers)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements sanitized Physics3D ray intersections and mesh query acceleration.
///
/// Rays are dispatched to analytic primitive tests, triangle-mesh BVH traversal,
/// heightfield sampling, or recursively posed compound children. World-level entry
/// points reuse the query broad phase and either box the nearest results or fill
/// borrowed caller storage without allocation.

#include "rt_physics3d_query_internal.h"

#include "rt_collider3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_raycast3d.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Populates and sanitizes a raw hit record from a confirmed ray intersection.
/// @param body Borrowed body struck by the ray; may be null for a synthetic hit.
/// @param distance Entry distance from @p origin in world units.
/// @param max_distance Total ray extent used to derive the normalized hit fraction.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param normal Optional borrowed hit normal; the opposite ray direction is used when absent.
/// @param started Nonzero when the ray origin starts inside the intersected shape.
/// @param[out] out_hit Destination record. A null destination makes this function a no-op.
static void ray_fill_hit(rt_body3d *body,
                         double distance,
                         double max_distance,
                         const double *origin,
                         const double *dir,
                         const double *normal,
                         int started,
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
    out_hit->started_penetrating = (int8_t)(started ? 1 : 0);
    out_hit->is_trigger = body ? body->is_trigger : 0;
    out_hit->point[0] = query_saturate_coord(origin[0] + dir[0] * out_hit->distance);
    out_hit->point[1] = query_saturate_coord(origin[1] + dir[1] * out_hit->distance);
    out_hit->point[2] = query_saturate_coord(origin[2] + dir[2] * out_hit->distance);
    if (normal)
        vec3_copy(out_hit->normal, normal);
    else
        vec3_negate(dir, out_hit->normal);
    query_sanitize_hit(out_hit, max_distance, dir);
}

/// @brief Intersects a normalized ray with a sphere using an analytic quadratic solve.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param center Borrowed three-element sphere center.
/// @param radius Non-negative sphere radius in world units.
/// @param max_distance Non-negative maximum accepted ray distance.
/// @param[out] out_t Optional destination for entry distance, or zero for an interior start.
/// @param[out] out_normal Optional three-element destination for the outward hit normal.
/// @param[out] out_started Optional destination set to `1` for an interior start and `0` for an
///        exterior entry.
/// @return `1` when the origin is in the sphere or the ray enters it within @p max_distance;
///         otherwise `0`.
int raycast_sphere_raw(const double *origin,
                       const double *dir,
                       const double *center,
                       double radius,
                       double max_distance,
                       double *out_t,
                       double *out_normal,
                       int *out_started) {
    double m[3];
    if (!origin || !dir || !center || !isfinite(radius) || radius < 0.0 ||
        !isfinite(max_distance) || max_distance < 0.0)
        return 0;
    radius = query_sanitize_distance(radius);
    max_distance = query_sanitize_distance(max_distance);
    vec3_sub(origin, center, m);
    double c = vec3_dot(m, m) - radius * radius;
    if (c <= 0.0) {
        if (out_t)
            *out_t = 0.0;
        if (out_started)
            *out_started = 1;
        if (out_normal)
            vec3_negate(dir, out_normal);
        return 1;
    }
    double b = vec3_dot(m, dir);
    double disc = b * b - c;
    if (!isfinite(disc) || disc < 0.0)
        return 0;
    double t = -b - sqrt(disc);
    if (!isfinite(t) || t < 0.0 || t > max_distance)
        return 0;
    if (out_t)
        *out_t = t;
    if (out_started)
        *out_started = 0;
    if (out_normal) {
        out_normal[0] = origin[0] + dir[0] * t - center[0];
        out_normal[1] = origin[1] + dir[1] * t - center[1];
        out_normal[2] = origin[2] + dir[2] * t - center[2];
        query_normalize_normal(out_normal, dir);
    }
    return 1;
}

/// @brief Intersects a ray with an axis-aligned box using the slab method.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed ray direction.
/// @param mn Borrowed three-element inclusive minimum corner.
/// @param mx Borrowed three-element inclusive maximum corner.
/// @param max_distance Non-negative maximum accepted ray distance.
/// @param[out] out_t Optional destination for entry distance, or zero for an interior start.
/// @param[out] out_normal Optional destination for the entry-face normal; an interior start gets
///        the nearest outward face normal.
/// @param[out] out_started Optional destination set to `1` when @p origin is inside the box.
/// @return `1` when the ray starts in or enters the box within @p max_distance; otherwise `0`.
static int raycast_aabb_raw(const double *origin,
                            const double *dir,
                            const double *mn,
                            const double *mx,
                            double max_distance,
                            double *out_t,
                            double *out_normal,
                            int *out_started) {
    double tmin = 0.0;
    double tmax;
    double n[3] = {0.0, 0.0, 0.0};
    int inside = 1;
    if (!origin || !dir || !mn || !mx || !isfinite(max_distance) || max_distance < 0.0)
        return 0;
    max_distance = query_sanitize_distance(max_distance);
    tmax = max_distance;
    for (int axis = 0; axis < 3; axis++) {
        if (origin[axis] < mn[axis] || origin[axis] > mx[axis])
            inside = 0;
        if (fabs(dir[axis]) < 1e-12) {
            if (origin[axis] < mn[axis] || origin[axis] > mx[axis])
                return 0;
            continue;
        }
        double inv = 1.0 / dir[axis];
        double t1 = (mn[axis] - origin[axis]) * inv;
        double t2 = (mx[axis] - origin[axis]) * inv;
        double sign = -1.0;
        if (!isfinite(t1) || !isfinite(t2))
            return 0;
        if (t1 > t2) {
            double tmp = t1;
            t1 = t2;
            t2 = tmp;
            sign = 1.0;
        }
        if (t1 > tmin) {
            tmin = t1;
            vec3_set(n, 0.0, 0.0, 0.0);
            n[axis] = sign;
        }
        if (t2 < tmax)
            tmax = t2;
        if (tmin > tmax)
            return 0;
    }
    if (inside) {
        if (out_t)
            *out_t = 0.0;
        if (out_started)
            *out_started = 1;
        if (out_normal) {
            /* Origin starts inside the box: return the outward normal of the nearest
             * face (the shortest push-out direction) instead of the ray direction. */
            int best_axis = 0;
            double best_sign = -1.0;
            double best_dist = -1.0;
            for (int axis = 0; axis < 3; axis++) {
                double dlo = origin[axis] - mn[axis];
                double dhi = mx[axis] - origin[axis];
                if (best_dist < 0.0 || dlo < best_dist) {
                    best_dist = dlo;
                    best_axis = axis;
                    best_sign = -1.0;
                }
                if (dhi < best_dist) {
                    best_dist = dhi;
                    best_axis = axis;
                    best_sign = 1.0;
                }
            }
            vec3_set(out_normal, 0.0, 0.0, 0.0);
            out_normal[best_axis] = best_sign;
            query_normalize_normal(out_normal, dir);
        }
        return 1;
    }
    if (tmin < 0.0 || tmin > max_distance)
        return 0;
    if (out_t)
        *out_t = tmin;
    if (out_started)
        *out_started = 0;
    if (out_normal) {
        vec3_copy(out_normal, n);
        query_normalize_normal(out_normal, dir);
    }
    return 1;
}

/// @brief Intersects a normalized ray with a capsule's cylinder and hemispherical caps.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param a Borrowed first endpoint of the capsule axis.
/// @param b Borrowed second endpoint of the capsule axis.
/// @param radius Non-negative capsule radius in world units.
/// @param max_distance Non-negative maximum accepted ray distance.
/// @param[out] out_t Optional destination for the nearest entry distance.
/// @param[out] out_normal Optional destination for the normalized surface normal.
/// @param[out] out_started Optional destination set when the origin begins inside the capsule.
/// @return `1` when any capsule component contains or is entered by the ray within the distance
///         limit; otherwise `0`.
static int raycast_capsule_raw(const double *origin,
                               const double *dir,
                               const double *a,
                               const double *b,
                               double radius,
                               double max_distance,
                               double *out_t,
                               double *out_normal,
                               int *out_started) {
    double best_t = max_distance + 1.0;
    double best_n[3] = {0.0, 0.0, 0.0};
    int best_started = 0;
    int found = 0;
    double t, n[3];
    int started;
    if (!origin || !dir || !a || !b || !isfinite(radius) || radius < 0.0 ||
        !isfinite(max_distance) || max_distance < 0.0)
        return 0;
    radius = query_sanitize_distance(radius);
    max_distance = query_sanitize_distance(max_distance);
    if (raycast_sphere_raw(origin, dir, a, radius, max_distance, &t, n, &started)) {
        best_t = t;
        vec3_copy(best_n, n);
        best_started = started;
        found = 1;
    }
    if (raycast_sphere_raw(origin, dir, b, radius, max_distance, &t, n, &started) &&
        (!found || t < best_t)) {
        best_t = t;
        vec3_copy(best_n, n);
        best_started = started;
        found = 1;
    }
    double axis[3];
    vec3_sub(b, a, axis);
    double h = vec3_normalize_in_place(axis);
    if (h > 1e-12) {
        double m[3];
        vec3_sub(origin, a, m);
        double md = vec3_dot(m, axis);
        double nd = vec3_dot(dir, axis);
        double mp[3] = {m[0] - axis[0] * md, m[1] - axis[1] * md, m[2] - axis[2] * md};
        double dp[3] = {dir[0] - axis[0] * nd, dir[1] - axis[1] * nd, dir[2] - axis[2] * nd};
        double qa = vec3_dot(dp, dp);
        double qb = 2.0 * vec3_dot(mp, dp);
        double qc = vec3_dot(mp, mp) - radius * radius;
        if (qc <= 0.0 && md >= 0.0 && md <= h) {
            best_t = 0.0;
            vec3_negate(dir, best_n);
            query_normalize_normal(best_n, dir);
            best_started = 1;
            found = 1;
        } else if (qa > 1e-12) {
            double disc = qb * qb - 4.0 * qa * qc;
            if (isfinite(disc) && disc >= 0.0) {
                double root = sqrt(disc);
                double roots[2] = {(-qb - root) / (2.0 * qa), (-qb + root) / (2.0 * qa)};
                for (int i = 0; i < 2; i++) {
                    double ct = roots[i];
                    double y = md + ct * nd;
                    if (!isfinite(ct) || !isfinite(y) || ct < 0.0 || ct > max_distance || y < 0.0 ||
                        y > h)
                        continue;
                    if (!found || ct < best_t) {
                        double p[3] = {origin[0] + dir[0] * ct,
                                       origin[1] + dir[1] * ct,
                                       origin[2] + dir[2] * ct};
                        double c[3] = {a[0] + axis[0] * y, a[1] + axis[1] * y, a[2] + axis[2] * y};
                        best_t = ct;
                        best_n[0] = p[0] - c[0];
                        best_n[1] = p[1] - c[1];
                        best_n[2] = p[2] - c[2];
                        query_normalize_normal(best_n, dir);
                        best_started = 0;
                        found = 1;
                    }
                }
            }
        }
    }
    if (!found)
        return 0;
    if (out_t)
        *out_t = best_t;
    if (out_normal) {
        vec3_copy(out_normal, best_n);
        query_normalize_normal(out_normal, dir);
    }
    if (out_started)
        *out_started = best_started;
    return 1;
}

/// @brief Raycast against a posed box (optionally Minkowski-expanded by @p expand_radius
///   for swept-sphere tests): transform the ray into box-local space and slab-test the
///   AABB.
/// @param box_collider Borrowed box collider supplying unscaled half-extents.
/// @param pose Borrowed world pose and scale applied to the collider.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized world-space ray direction.
/// @param max_distance Non-negative maximum accepted world-space distance.
/// @param expand_radius Non-negative world-space Minkowski expansion, or zero for a plain ray.
/// @param[out] out_t Optional destination for entry distance.
/// @param[out] out_normal Optional destination for the normalized world-space hit normal.
/// @param[out] out_started Optional destination set when the origin starts inside the expanded box.
/// @return `1` when the ray starts in or enters the expanded posed box within the distance limit;
///         otherwise `0`.
int raycast_box_pose_raw(void *box_collider,
                         const rt_collider_pose *pose,
                         const double *origin,
                         const double *dir,
                         double max_distance,
                         double expand_radius,
                         double *out_t,
                         double *out_normal,
                         int *out_started) {
    double he[3];
    double local_origin[3];
    double local_dir[3];
    double mn[3];
    double mx[3];
    double local_normal[3] = {0.0, 0.0, 0.0};
    double t = 0.0;
    int started = 0;
    if (!box_collider || !pose || !origin || !dir || !isfinite(max_distance) ||
        max_distance < 0.0 || !isfinite(expand_radius) || expand_radius < 0.0)
        return 0;
    max_distance = query_sanitize_distance(max_distance);
    expand_radius = query_sanitize_distance(expand_radius);
    rt_collider3d_get_box_half_extents_raw(box_collider, he);
    for (int i = 0; i < 3; i++) {
        double expansion = expand_radius / pose_abs_scale_or_unit(pose->scale[i]);
        mn[i] = -he[i] - expansion;
        mx[i] = he[i] + expansion;
    }
    transform_point_to_local(pose, origin, local_origin);
    transform_vector_to_local(pose, dir, local_dir);
    ph3d_vec3_sanitize_state(local_origin);
    ph3d_vec3_sanitize_state(local_dir);
    if (!raycast_aabb_raw(
            local_origin, local_dir, mn, mx, max_distance, &t, local_normal, &started))
        return 0;
    if (out_t)
        *out_t = t;
    if (out_started)
        *out_started = started;
    if (out_normal) {
        if (started) {
            vec3_negate(dir, out_normal);
            query_normalize_normal(out_normal, dir);
        } else {
            transform_normal_from_local(pose, local_normal, out_normal);
            query_normalize_normal(out_normal, dir);
        }
    }
    return 1;
}

/// @brief Möller-Trumbore ray/triangle intersection in world space. Returns 1 on a hit
///   within @p max_distance with distance @p out_t and the geometric face normal
///   @p out_normal, else 0 (including parallel rays and barycentric misses).
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed ray direction.
/// @param a Borrowed first triangle vertex.
/// @param b Borrowed second triangle vertex.
/// @param c Borrowed third triangle vertex.
/// @param max_distance Non-negative maximum accepted ray distance.
/// @param[out] out_t Optional destination for the intersection distance.
/// @param[out] out_normal Optional destination for the geometric normal, oriented against the ray.
/// @return `1` for an in-range intersection inside the triangle; otherwise `0`.
static int raycast_triangle_world(const double *origin,
                                  const double *dir,
                                  const double *a,
                                  const double *b,
                                  const double *c,
                                  double max_distance,
                                  double *out_t,
                                  double *out_normal) {
    double e1[3], e2[3], pvec[3], tvec[3], qvec[3];
    double det, inv_det, u, v, t;
    if (!origin || !dir || !a || !b || !c || !isfinite(max_distance) || max_distance < 0.0)
        return 0;
    max_distance = query_sanitize_distance(max_distance);
    vec3_sub(b, a, e1);
    vec3_sub(c, a, e2);
    vec3_cross(dir, e2, pvec);
    det = vec3_dot(e1, pvec);
    if (!isfinite(det) || fabs(det) < 1e-12)
        return 0;
    inv_det = 1.0 / det;
    vec3_sub(origin, a, tvec);
    u = vec3_dot(tvec, pvec) * inv_det;
    if (!isfinite(u) || u < 0.0 || u > 1.0)
        return 0;
    vec3_cross(tvec, e1, qvec);
    v = vec3_dot(dir, qvec) * inv_det;
    if (!isfinite(v) || v < 0.0 || u + v > 1.0)
        return 0;
    t = vec3_dot(e2, qvec) * inv_det;
    if (!isfinite(t) || t < 0.0 || t > max_distance)
        return 0;
    if (out_t)
        *out_t = t;
    if (out_normal) {
        triangle_normal(a, b, c, out_normal);
        if (vec3_dot(out_normal, dir) > 0.0)
            vec3_negate(out_normal, out_normal);
        query_normalize_normal(out_normal, dir);
    }
    return 1;
}

/// @brief Grow the AABB [mn, mx] in place to include point @p p.
/// @param[in,out] mn Three-element minimum corner to decrease component-wise.
/// @param[in,out] mx Three-element maximum corner to increase component-wise.
/// @param p Borrowed three-element point to include.
static void mesh_bvh_expand(float *mn, float *mx, const float *p) {
    for (int axis = 0; axis < 3; axis++) {
        if (p[axis] < mn[axis])
            mn[axis] = p[axis];
        if (p[axis] > mx[axis])
            mx[axis] = p[axis];
    }
}

/// @brief Sanitize a mesh vertex coordinate before storing it in the physics BVH.
/// @param value Candidate single-precision coordinate.
/// @return Zero for a non-finite input; otherwise @p value clamped to the query coordinate range.
static float mesh_bvh_sanitize_coord(float value) {
    if (!isfinite((double)value))
        return 0.0f;
    if (value > (float)PH3D_QUERY_COORD_ABS_MAX)
        return (float)PH3D_QUERY_COORD_ABS_MAX;
    if (value < (float)-PH3D_QUERY_COORD_ABS_MAX)
        return (float)-PH3D_QUERY_COORD_ABS_MAX;
    return value;
}

/// @brief Compute triangle @p tri's AABB (@p mn, @p mx) and centroid for BVH construction.
/// @param mesh Borrowed mesh containing validated indexed triangle geometry.
/// @param tri Triangle ordinal in the mesh index buffer.
/// @param[out] mn Three-element destination for the sanitized minimum corner.
/// @param[out] mx Three-element destination for the sanitized maximum corner.
/// @param[out] centroid Three-element destination for the arithmetic vertex centroid.
static void mesh_bvh_triangle_bounds(
    const rt_mesh3d *mesh, uint32_t tri, float *mn, float *mx, float *centroid) {
    uint32_t i0 = mesh->indices[tri * 3u + 0u];
    uint32_t i1 = mesh->indices[tri * 3u + 1u];
    uint32_t i2 = mesh->indices[tri * 3u + 2u];
    float a[3] = {mesh_bvh_sanitize_coord(mesh->vertices[i0].pos[0]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i0].pos[1]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i0].pos[2])};
    float b[3] = {mesh_bvh_sanitize_coord(mesh->vertices[i1].pos[0]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i1].pos[1]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i1].pos[2])};
    float c[3] = {mesh_bvh_sanitize_coord(mesh->vertices[i2].pos[0]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i2].pos[1]),
                  mesh_bvh_sanitize_coord(mesh->vertices[i2].pos[2])};
    for (int axis = 0; axis < 3; axis++) {
        mn[axis] = fminf(a[axis], fminf(b[axis], c[axis]));
        mx[axis] = fmaxf(a[axis], fmaxf(b[axis], c[axis]));
        centroid[axis] = (a[axis] + b[axis] + c[axis]) / 3.0f;
    }
}

/// @brief Recursively build a physics BVH node over a range of mesh triangles.
/// @details Computes the node bounds, and if the range exceeds the leaf threshold, splits the
///          triangles by centroid along the widest axis and recurses into two children; otherwise
///          stores a leaf. A child-capacity failure degrades the current node into a leaf so the
///          represented triangle range remains queryable.
/// @param mesh Borrowed mesh supplying indexed triangle vertices.
/// @param[in,out] nodes Caller-owned preallocated node array.
/// @param[in,out] node_count Number of initialized nodes; incremented as nodes are reserved.
/// @param node_capacity Total writable entries in @p nodes.
/// @param[in,out] tri_indices Triangle-ordinal array partitioned in place around centroid pivots.
/// @param start First triangle-array position represented by this node.
/// @param count Number of triangle ordinals represented by this node.
/// @return The initialized node index, or `-1` when required inputs or node capacity are invalid.
static int mesh_bvh_build_node(rt_mesh3d *mesh,
                               rt_physics_mesh_bvh_node *nodes,
                               int32_t *node_count,
                               int32_t node_capacity,
                               uint32_t *tri_indices,
                               int32_t start,
                               int32_t count) {
    int32_t node_index;
    rt_physics_mesh_bvh_node *node;
    float centroid_min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float centroid_max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    int split_axis = 0;
    if (!mesh || !nodes || !node_count || !tri_indices || count <= 0 ||
        *node_count >= node_capacity)
        return -1;
    node_index = (*node_count)++;
    node = &nodes[node_index];
    node->left = -1;
    node->right = -1;
    node->start = start;
    node->count = count;
    node->min[0] = node->min[1] = node->min[2] = FLT_MAX;
    node->max[0] = node->max[1] = node->max[2] = -FLT_MAX;
    for (int32_t i = start; i < start + count; i++) {
        float tri_min[3], tri_max[3], centroid[3];
        mesh_bvh_triangle_bounds(mesh, tri_indices[i], tri_min, tri_max, centroid);
        mesh_bvh_expand(node->min, node->max, tri_min);
        mesh_bvh_expand(node->min, node->max, tri_max);
        mesh_bvh_expand(centroid_min, centroid_max, centroid);
    }
    if (count <= 8)
        return node_index;
    {
        float extent_x = centroid_max[0] - centroid_min[0];
        float extent_y = centroid_max[1] - centroid_min[1];
        float extent_z = centroid_max[2] - centroid_min[2];
        if (extent_y > extent_x && extent_y >= extent_z)
            split_axis = 1;
        else if (extent_z > extent_x && extent_z >= extent_y)
            split_axis = 2;
    }
    {
        float pivot = 0.5f * (centroid_min[split_axis] + centroid_max[split_axis]);
        int32_t lo = start;
        int32_t hi = start + count - 1;
        while (lo <= hi) {
            float tri_min[3], tri_max[3], centroid[3];
            mesh_bvh_triangle_bounds(mesh, tri_indices[lo], tri_min, tri_max, centroid);
            if (centroid[split_axis] < pivot) {
                lo++;
            } else {
                uint32_t tmp = tri_indices[lo];
                tri_indices[lo] = tri_indices[hi];
                tri_indices[hi] = tmp;
                hi--;
            }
        }
        if (lo == start || lo == start + count)
            lo = start + count / 2;
        node->left = mesh_bvh_build_node(
            mesh, nodes, node_count, node_capacity, tri_indices, start, lo - start);
        node->right = mesh_bvh_build_node(
            mesh, nodes, node_count, node_capacity, tri_indices, lo, start + count - lo);
        if (node->left < 0 || node->right < 0) {
            node->left = node->right = -1;
            node->start = start;
            node->count = count;
        } else {
            node->count = 0;
        }
    }
    return node_index;
}

/// @brief Rebuild the mesh's physics BVH acceleration structure for ray/shape queries.
/// @details Repairs and validates geometry counts, drops triangles with invalid vertex indices,
///          and transactionally replaces the cached node and triangle-index arrays only after a
///          complete build. An existing cache whose geometry revision is current is reused.
/// @param[in,out] mesh Mesh whose world-independent local-space acceleration data is required.
/// @return `1` when a non-empty current BVH is available; `0` for invalid or empty geometry,
///         excessive triangle count, or allocation failure.
int mesh_physics_bvh_rebuild(rt_mesh3d *mesh) {
    uint32_t tri_total;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t *tri_indices = NULL;
    rt_physics_mesh_bvh_node *nodes = NULL;
    int32_t tri_count = 0;
    int32_t node_capacity;
    int32_t node_count = 0;
    if (!mesh)
        return 0;
    rt_mesh3d_repair_geometry_counts(mesh);
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_validated_index_count(mesh);
    if (index_count < 3u || vertex_count == 0)
        return 0;
    if (mesh->physics_bvh_nodes && mesh->physics_bvh_revision == mesh->geometry_revision)
        return mesh->physics_bvh_node_count > 0;
    tri_total = index_count / 3u;
    if (tri_total == 0 || tri_total > (uint32_t)(INT32_MAX / 2))
        return 0;
    tri_indices = (uint32_t *)malloc((size_t)tri_total * sizeof(*tri_indices));
    if (!tri_indices)
        return 0;
    for (uint32_t tri = 0; tri < tri_total; tri++) {
        uint32_t i0 = mesh->indices[tri * 3u + 0u];
        uint32_t i1 = mesh->indices[tri * 3u + 1u];
        uint32_t i2 = mesh->indices[tri * 3u + 2u];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
            continue;
        tri_indices[tri_count++] = tri;
    }
    if (tri_count <= 0) {
        free(tri_indices);
        return 0;
    }
    node_capacity = tri_count * 2;
    nodes = (rt_physics_mesh_bvh_node *)calloc((size_t)node_capacity, sizeof(*nodes));
    if (!nodes) {
        free(tri_indices);
        return 0;
    }
    if (mesh_bvh_build_node(mesh, nodes, &node_count, node_capacity, tri_indices, 0, tri_count) <
        0) {
        free(nodes);
        free(tri_indices);
        return 0;
    }
    free(mesh->physics_bvh_nodes);
    free(mesh->physics_bvh_tri_indices);
    mesh->physics_bvh_nodes = nodes;
    mesh->physics_bvh_tri_indices = tri_indices;
    mesh->physics_bvh_node_count = node_count;
    mesh->physics_bvh_tri_count = tri_count;
    mesh->physics_bvh_revision = mesh->geometry_revision;
    return 1;
}

/// @brief Transform a local-space AABB through a collider pose into a world-space AABB.
/// @details Rotates and translates all eight corners and takes their min/max, so the result tightly
///          bounds the oriented box (a plain min/max of the rotated extents would over-grow it).
/// @param pose Borrowed collider pose used to transform each corner.
/// @param mn Borrowed three-element local minimum corner.
/// @param mx Borrowed three-element local maximum corner.
/// @param[out] out_min Three-element destination for the world-space minimum corner.
/// @param[out] out_max Three-element destination for the world-space maximum corner.
void transform_local_aabb_to_world(const rt_collider_pose *pose,
                                   const float *mn,
                                   const float *mx,
                                   double *out_min,
                                   double *out_max) {
    for (int axis = 0; axis < 3; axis++) {
        out_min[axis] = DBL_MAX;
        out_max[axis] = -DBL_MAX;
    }
    for (int i = 0; i < 8; i++) {
        double local[3] = {
            (i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]};
        double world[3];
        transform_point_from_pose(pose, local, world);
        for (int axis = 0; axis < 3; axis++) {
            if (world[axis] < out_min[axis])
                out_min[axis] = world[axis];
            if (world[axis] > out_max[axis])
                out_max[axis] = world[axis];
        }
    }
}

/// @brief Finds the nearest ray intersection with a posed triangle mesh.
/// @details The ray is transformed once into mesh-local space so its distance parameter remains
///          the world-space parameter. A lazily rebuilt BVH prunes triangle tests; if traversal
///          stack growth fails, an exhaustive scan preserves exact nearest-hit behavior.
/// @param mesh Borrowed mutable mesh whose bounds and physics BVH may be refreshed.
/// @param pose Borrowed world pose applied to the mesh.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized world-space ray direction.
/// @param max_distance Maximum accepted intersection distance in world units.
/// @param[out] out_t Optional destination for the nearest distance.
/// @param[out] out_normal Optional destination for the normalized world-space face normal.
/// @param[out] out_started Optional destination always set to `0` because triangle meshes are
///        treated as surfaces.
/// @return `1` when a valid indexed triangle is hit within @p max_distance; otherwise `0`.
static int raycast_meshlike_pose_raw(rt_mesh3d *mesh,
                                     const rt_collider_pose *pose,
                                     const double *origin,
                                     const double *dir,
                                     double max_distance,
                                     double *out_t,
                                     double *out_normal,
                                     int *out_started) {
    double best_t = max_distance + 1.0;
    double best_local_normal[3] = {0.0, 1.0, 0.0};
    int found = 0;
    /* Transform the ray into mesh-local space once and run BOTH the node-AABB
     * tests and the leaf Moller-Trumbore in local space (raw float vertices, no
     * per-triangle pose transforms). transform_vector_to_local does NOT
     * renormalize, so the local t-parameter equals the world t (same trick as
     * raycast_box_pose_raw). The backface orientation sign is also preserved:
     * dot(M^-T n, M d) == dot(n, d), so orienting against the local direction is
     * exactly equivalent to orienting against the world direction. Only the
     * winning hit's normal is transformed back to world, once, at the end. */
    double local_origin[3];
    double local_dir[3];
    if (!mesh || !pose || mesh->index_count < 3)
        return 0;
    rt_mesh3d_refresh_bounds(mesh);
    transform_point_to_local(pose, origin, local_origin);
    transform_vector_to_local(pose, dir, local_dir);
    ph3d_vec3_sanitize_state(local_origin);
    ph3d_vec3_sanitize_state(local_dir);
    if (mesh_physics_bvh_rebuild(mesh)) {
        const rt_physics_mesh_bvh_node *nodes =
            (const rt_physics_mesh_bvh_node *)mesh->physics_bvh_nodes;
        const uint32_t *tri_indices = mesh->physics_bvh_tri_indices;
        int32_t *stack = NULL;
        int32_t top = 0;
        int32_t stack_capacity = 0;
        int overflow = 0;
        if (!ph3d_i32_stack_push(&stack, &top, &stack_capacity, 0))
            overflow = 1;
        while (top > 0) {
            int32_t node_index = stack[--top];
            const rt_physics_mesh_bvh_node *node;
            double node_min[3], node_max[3], box_t;
            if (node_index < 0 || node_index >= mesh->physics_bvh_node_count)
                continue;
            node = &nodes[node_index];
            /* Widen the stored float AABB to double for the local-space test (cheaper
             * than the previous 8-corner world transform per node). */
            node_min[0] = node->min[0];
            node_min[1] = node->min[1];
            node_min[2] = node->min[2];
            node_max[0] = node->max[0];
            node_max[1] = node->max[1];
            node_max[2] = node->max[2];
            if (!raycast_aabb_raw(
                    local_origin, local_dir, node_min, node_max, best_t, &box_t, NULL, NULL))
                continue;
            if (node->left >= 0 || node->right >= 0) {
                if (node->right >= 0 &&
                    !ph3d_i32_stack_push(&stack, &top, &stack_capacity, node->right)) {
                    overflow = 1;
                    break;
                }
                if (node->left >= 0 &&
                    !ph3d_i32_stack_push(&stack, &top, &stack_capacity, node->left)) {
                    overflow = 1;
                    break;
                }
                continue;
            }
            for (int32_t item = node->start; item < node->start + node->count; item++) {
                uint32_t tri = tri_indices[item];
                uint32_t i0 = mesh->indices[tri * 3u + 0u];
                uint32_t i1 = mesh->indices[tri * 3u + 1u];
                uint32_t i2 = mesh->indices[tri * 3u + 2u];
                double a[3], b[3], c[3], t, n[3];
                if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
                    i2 >= mesh->vertex_count)
                    continue;
                a[0] = mesh->vertices[i0].pos[0];
                a[1] = mesh->vertices[i0].pos[1];
                a[2] = mesh->vertices[i0].pos[2];
                b[0] = mesh->vertices[i1].pos[0];
                b[1] = mesh->vertices[i1].pos[1];
                b[2] = mesh->vertices[i1].pos[2];
                c[0] = mesh->vertices[i2].pos[0];
                c[1] = mesh->vertices[i2].pos[1];
                c[2] = mesh->vertices[i2].pos[2];
                if (raycast_triangle_world(local_origin, local_dir, a, b, c, best_t, &t, n) &&
                    t < best_t) {
                    best_t = t;
                    vec3_copy(best_local_normal, n);
                    found = 1;
                }
            }
        }
        free(stack);
        if (!overflow)
            goto mesh_raycast_done;
        /* BVH stack overflowed mid-traversal: fall through to the exhaustive scan,
         * but keep the best hit found so far as a tighter initial bound. The scan
         * still tests every triangle, so the final nearest hit remains exact. */
    }
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i];
        uint32_t i1 = mesh->indices[i + 1];
        uint32_t i2 = mesh->indices[i + 2];
        double a[3], b[3], c[3], t, n[3];
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
            continue;
        a[0] = mesh->vertices[i0].pos[0];
        a[1] = mesh->vertices[i0].pos[1];
        a[2] = mesh->vertices[i0].pos[2];
        b[0] = mesh->vertices[i1].pos[0];
        b[1] = mesh->vertices[i1].pos[1];
        b[2] = mesh->vertices[i1].pos[2];
        c[0] = mesh->vertices[i2].pos[0];
        c[1] = mesh->vertices[i2].pos[1];
        c[2] = mesh->vertices[i2].pos[2];
        if (raycast_triangle_world(local_origin, local_dir, a, b, c, best_t, &t, n) &&
            t < best_t) {
            best_t = t;
            vec3_copy(best_local_normal, n);
            found = 1;
        }
    }
mesh_raycast_done:
    if (!found)
        return 0;
    if (out_t)
        *out_t = best_t;
    if (out_normal) {
        transform_normal_from_local(pose, best_local_normal, out_normal);
        query_normalize_normal(out_normal, dir);
    }
    if (out_started)
        *out_started = 0;
    return 1;
}

/// @brief Raycast a posed heightfield collider: clip the ray to the field AABB, then
///   march in local space sampling terrain height until the ray dips below the surface.
/// @details The local XZ interval is clipped to the finite grid when heightfield dimensions are
///          available. March steps follow half-cell spacing and the first sign change in vertical
///          clearance is refined by bisection.
/// @param heightfield Borrowed heightfield collider.
/// @param pose Borrowed world pose applied to the heightfield.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized world-space ray direction.
/// @param max_distance Positive maximum accepted distance in world units.
/// @param[out] out_t Optional destination for the sampled and refined hit distance.
/// @param[out] out_normal Optional destination for the normalized world-space terrain normal.
/// @param[out] out_started Optional destination set when the first sampled point is already at or
///        below the terrain surface.
/// @return `1` when the clipped ray crosses the sampled terrain within the distance and march
///         budgets; otherwise `0`.
static int raycast_heightfield_pose_raw(void *heightfield,
                                        const rt_collider_pose *pose,
                                        const double *origin,
                                        const double *dir,
                                        double max_distance,
                                        double *out_t,
                                        double *out_normal,
                                        int *out_started) {
    double mn[3], mx[3], entry_t, aabb_normal[3];
    int started = 0;
    double local_origin[3], local_dir[3];
    double start_t;
    double march_end;
    double step;
    double march_budget;
    double heightfield_scale[3] = {1.0, 1.0, 1.0};
    int32_t heightfield_width = 0;
    int32_t heightfield_depth = 0;
    double prev_t;
    double prev_clearance = DBL_MAX;
    int has_prev = 0;
    if (!heightfield || !pose || !origin || !dir || !isfinite(max_distance) || max_distance <= 0.0)
        return 0;
    max_distance = query_sanitize_distance(max_distance);
    rt_collider3d_compute_world_aabb_raw(
        heightfield, pose->position, pose->rotation, pose->scale, mn, mx);
    if (!raycast_aabb_raw(origin, dir, mn, mx, max_distance, &entry_t, aabb_normal, &started))
        return 0;
    transform_point_to_local(pose, origin, local_origin);
    transform_vector_to_local(pose, dir, local_dir);
    start_t = started ? 0.0 : entry_t;
    march_end = max_distance;
    step = max_distance / 512.0;
    /* The march budget must cover the whole in-field segment: the previous fixed
     * 2048-iteration cap made rays crossing more than ~1024 cells horizontally
     * exit mid-terrain and report a MISS (long sniper shots / AI line-of-sight on
     * large heightfields). With a half-cell step the in-field sample count is
     * geometrically bounded, so derive the budget from the grid dimensions and
     * clip the marched range to the field's local XZ rectangle so out-of-field
     * travel costs nothing. */
    march_budget = 1024.0;
    if (rt_collider3d_get_heightfield_info_raw(
            heightfield, &heightfield_width, &heightfield_depth, heightfield_scale)) {
        double sx = fabs(heightfield_scale[0]);
        double sz = fabs(heightfield_scale[2]);
        double cell = sx > 1e-12 && sz > 1e-12 ? fmin(sx, sz) : fmax(sx, sz);
        double horizontal_speed = sqrt(local_dir[0] * local_dir[0] + local_dir[2] * local_dir[2]);
        if (heightfield_width > 1 && heightfield_depth > 1 && cell > 1e-12 &&
            horizontal_speed > 1e-12) {
            double half_w = 0.5 * (double)(heightfield_width - 1) * sx;
            double half_d = 0.5 * (double)(heightfield_depth - 1) * sz;
            double clip_lo = start_t;
            double clip_hi = march_end;
            int clipped_out = 0;
            step = (cell * 0.5) / horizontal_speed;
            /* 2D slab clip of the local ray against the field's XZ rectangle
             * (transform_vector_to_local preserves the t parameter). */
            for (int axis = 0; axis < 3; axis += 2) {
                double lo_bound = axis == 0 ? -half_w : -half_d;
                double hi_bound = axis == 0 ? half_w : half_d;
                if (fabs(local_dir[axis]) < 1e-12) {
                    if (local_origin[axis] < lo_bound || local_origin[axis] > hi_bound)
                        clipped_out = 1;
                    continue;
                }
                {
                    double inv = 1.0 / local_dir[axis];
                    double t1 = (lo_bound - local_origin[axis]) * inv;
                    double t2 = (hi_bound - local_origin[axis]) * inv;
                    if (t1 > t2) {
                        double tmp = t1;
                        t1 = t2;
                        t2 = tmp;
                    }
                    if (isfinite(t1) && t1 > clip_lo)
                        clip_lo = t1;
                    if (isfinite(t2) && t2 < clip_hi)
                        clip_hi = t2;
                }
            }
            if (clipped_out || clip_lo > clip_hi)
                return 0;
            /* Back off one step so the first in-field sample brackets the entry. */
            start_t = fmax(start_t, clip_lo - step);
            march_end = clip_hi;
            march_budget = 4.0 * (double)(heightfield_width + heightfield_depth) + 1024.0;
        }
    }
    if (!isfinite(step) || step <= 0.0)
        step = max_distance / 512.0;
    if (!isfinite(step) || step < 1e-5)
        step = 1e-5;
    if (step > max_distance)
        step = max_distance;
    if (!isfinite(step) || step <= 0.0)
        return 0;
    if (!isfinite(march_end) || march_end > max_distance)
        march_end = max_distance;
    prev_t = start_t;
    for (double t = start_t, march_iter = 0.0;
         t <= march_end + step + 1e-9 && t <= max_distance + 1e-9 && march_iter < march_budget;
         t += step, march_iter += 1.0) {
        double local_point[3] = {local_origin[0] + local_dir[0] * t,
                                 local_origin[1] + local_dir[1] * t,
                                 local_origin[2] + local_dir[2] * t};
        double surface = 0.0;
        double local_normal[3] = {0.0, 1.0, 0.0};
        double clearance;
        if (!rt_collider3d_sample_heightfield_raw(
                heightfield, local_point[0], local_point[2], &surface, local_normal)) {
            prev_t = t;
            has_prev = 0;
            continue;
        }
        clearance = local_point[1] - surface;
        if (clearance <= 0.0) {
            double hit_t = t;
            if (has_prev && prev_clearance > 0.0) {
                double lo = prev_t;
                double hi = t;
                for (int iter = 0; iter < 16; iter++) {
                    double mid = (lo + hi) * 0.5;
                    double mid_point[3] = {local_origin[0] + local_dir[0] * mid,
                                           local_origin[1] + local_dir[1] * mid,
                                           local_origin[2] + local_dir[2] * mid};
                    double mid_surface = 0.0;
                    double mid_normal[3] = {0.0, 1.0, 0.0};
                    if (rt_collider3d_sample_heightfield_raw(
                            heightfield, mid_point[0], mid_point[2], &mid_surface, mid_normal) &&
                        mid_point[1] - mid_surface <= 0.0) {
                        hi = mid;
                        vec3_copy(local_normal, mid_normal);
                    } else {
                        lo = mid;
                    }
                }
                hit_t = hi;
            }
            if (out_t)
                *out_t = hit_t;
            if (out_started)
                *out_started = (t == start_t && clearance <= 0.0) ? 1 : 0;
            if (out_normal) {
                transform_normal_from_local(pose, local_normal, out_normal);
                query_normalize_normal(out_normal, dir);
            }
            return 1;
        }
        prev_clearance = clearance;
        prev_t = t;
        has_prev = 1;
    }
    return 0;
}

/// @brief Top-level raycast dispatch for any posed collider: routes by collider type to
///   the box/sphere/capsule/mesh/heightfield/compound raycast (recursing into compound
///   children and returning the hit leaf via @p out_leaf).
/// @param collider Borrowed collider to dispatch; compound children are traversed recursively.
/// @param pose Borrowed world pose of @p collider.
/// @param origin Borrowed three-element world-space ray origin.
/// @param dir Borrowed normalized world-space ray direction.
/// @param max_distance Maximum accepted hit distance in world units.
/// @param[out] out_t Optional destination for the nearest entry distance.
/// @param[out] out_normal Optional destination for the normalized world-space normal.
/// @param[out] out_started Optional destination set when the ray begins inside a solid primitive.
/// @param[out] out_leaf Optional destination for the borrowed leaf collider that was hit.
/// @return `1` when the collider or one of its compound leaves is hit within @p max_distance;
///         otherwise `0`.
static int raycast_collider_pose(void *collider,
                                 const rt_collider_pose *pose,
                                 const double *origin,
                                 const double *dir,
                                 double max_distance,
                                 double *out_t,
                                 double *out_normal,
                                 int *out_started,
                                 void **out_leaf) {
    int64_t type;
    double t = 0.0;
    double normal[3] = {0.0, 1.0, 0.0};
    int started = 0;
    if (!collider || !pose)
        return 0;
    type = rt_collider3d_get_type(collider);
    if (type == RT_COLLIDER3D_TYPE_BOX) {
        if (!raycast_box_pose_raw(
                collider, pose, origin, dir, max_distance, 0.0, &t, normal, &started))
            return 0;
    } else if (type == RT_COLLIDER3D_TYPE_SPHERE) {
        double radius = rt_collider3d_get_radius_raw(collider);
        double sx = pose_abs_scale_or_unit(pose->scale[0]);
        double sy = pose_abs_scale_or_unit(pose->scale[1]);
        double sz = pose_abs_scale_or_unit(pose->scale[2]);
        double max_scale = sx > sy ? sx : sy;
        if (sz > max_scale)
            max_scale = sz;
        if (!raycast_sphere_raw(origin,
                                dir,
                                pose->position,
                                radius * max_scale,
                                max_distance,
                                &t,
                                normal,
                                &started))
            return 0;
    } else if (type == RT_COLLIDER3D_TYPE_CAPSULE) {
        rt_body3d proxy;
        double a[3], b[3];
        if (!build_simple_proxy(pose, collider, &proxy))
            return 0;
        capsule_axis_endpoints(&proxy, a, b);
        if (!raycast_capsule_raw(
                origin, dir, a, b, proxy.radius, max_distance, &t, normal, &started))
            return 0;
    } else if (type == RT_COLLIDER3D_TYPE_COMPOUND) {
        int64_t child_count = rt_collider3d_get_child_count_raw(collider);
        int found = 0;
        double best_t = max_distance + 1.0;
        double best_normal[3] = {0.0, 1.0, 0.0};
        int best_started = 0;
        void *best_leaf = NULL;
        for (int64_t i = 0; i < child_count; i++) {
            void *child = rt_collider3d_get_child_raw(collider, i);
            double child_pos[3], child_rot[4], child_scale[3];
            rt_collider_pose child_pose;
            double cur_t, cur_normal[3];
            int cur_started;
            void *cur_leaf = NULL;
            rt_collider3d_get_child_transform_raw(collider, i, child_pos, child_rot, child_scale);
            collider_pose_compose(pose, child_pos, child_rot, child_scale, &child_pose);
            if (raycast_collider_pose(child,
                                      &child_pose,
                                      origin,
                                      dir,
                                      max_distance,
                                      &cur_t,
                                      cur_normal,
                                      &cur_started,
                                      &cur_leaf) &&
                cur_t < best_t) {
                best_t = cur_t;
                vec3_copy(best_normal, cur_normal);
                best_started = cur_started;
                best_leaf = cur_leaf ? cur_leaf : child;
                found = 1;
            }
        }
        if (!found)
            return 0;
        t = best_t;
        vec3_copy(normal, best_normal);
        started = best_started;
        collider = best_leaf ? best_leaf : collider;
    } else if (type == RT_COLLIDER3D_TYPE_CONVEX_HULL || type == RT_COLLIDER3D_TYPE_MESH) {
        rt_mesh3d *mesh = (rt_mesh3d *)rt_collider3d_get_mesh_raw(collider);
        if (!raycast_meshlike_pose_raw(mesh, pose, origin, dir, max_distance, &t, normal, &started))
            return 0;
    } else if (type == RT_COLLIDER3D_TYPE_HEIGHTFIELD) {
        if (!raycast_heightfield_pose_raw(
                collider, pose, origin, dir, max_distance, &t, normal, &started))
            return 0;
    } else {
        double mn[3], mx[3];
        rt_collider3d_compute_world_aabb_raw(
            collider, pose->position, pose->rotation, pose->scale, mn, mx);
        if (!raycast_aabb_raw(origin, dir, mn, mx, max_distance, &t, normal, &started))
            return 0;
    }
    if (!isfinite(t) || t < 0.0 || t > max_distance)
        return 0;
    query_normalize_normal(normal, dir);
    if (out_t)
        *out_t = t;
    if (out_normal)
        vec3_copy(out_normal, normal);
    if (out_started)
        *out_started = started;
    if (out_leaf)
        *out_leaf = collider;
    return 1;
}

/// @brief Ray vs a collider-less body's cached simple shape (sphere/capsule/AABB).
/// @details Shape-only bodies (radius/half-extents set directly, no Collider3D)
///   collide in the world step and answer overlap queries, so raycasts must see
///   them too — otherwise bullets and line-of-sight pass through bodies that
///   block movement. Mirrors the shape semantics of `test_simple_collision`
///   (AABB shape is axis-aligned; capsules honor the body orientation).
/// @param body Borrowed collider-less body with cached simple-shape dimensions.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param max_distance Maximum accepted distance in world units.
/// @param[out] out_t Optional destination for the entry distance.
/// @param[out] out_normal Optional destination for the normalized hit normal.
/// @param[out] out_started Optional destination set when the origin starts inside the shape.
/// @return `1` when the cached sphere, capsule, or AABB is hit; otherwise `0`.
static int raycast_body_simple_shape(rt_body3d *body,
                                     const double *origin,
                                     const double *dir,
                                     double max_distance,
                                     double *out_t,
                                     double *out_normal,
                                     int *out_started) {
    if (!body || !body3d_has_collision_geometry(body))
        return 0;
    if (body->shape == PH3D_SHAPE_SPHERE) {
        return raycast_sphere_raw(
            origin, dir, body->position, body->radius, max_distance, out_t, out_normal, out_started);
    }
    if (body->shape == PH3D_SHAPE_CAPSULE) {
        double a[3];
        double b[3];
        capsule_axis_endpoints(body, a, b);
        return raycast_capsule_raw(
            origin, dir, a, b, body->radius, max_distance, out_t, out_normal, out_started);
    }
    {
        double mn[3];
        double mx[3];
        body_aabb(body, mn, mx);
        return raycast_aabb_raw(origin, dir, mn, mx, max_distance, out_t, out_normal, out_started);
    }
}

/// @brief Ray vs a physics body: dispatches to the actual attached collider
///        shape, recursing through compound children and testing mesh triangles.
///        Collider-less bodies fall back to their cached simple shape.
/// @param body Borrowed body to test.
/// @param origin Borrowed three-element ray origin.
/// @param dir Borrowed normalized ray direction.
/// @param max_distance Maximum accepted hit distance in world units.
/// @param[out] out_hit Optional destination for a sanitized hit record. Compound hits identify
///        the borrowed leaf collider.
/// @return `1` when the body is hit within @p max_distance; otherwise `0`.
static int raycast_body(rt_body3d *body,
                        const double *origin,
                        const double *dir,
                        double max_distance,
                        rt_query_hit3d *out_hit) {
    double t = 0.0;
    double normal[3] = {0.0, 0.0, 0.0};
    int started = 0;
    void *leaf = NULL;
    if (!body)
        return 0;
    if (!body->collider) {
        if (!raycast_body_simple_shape(body, origin, dir, max_distance, &t, normal, &started))
            return 0;
        ray_fill_hit(body, t, max_distance, origin, dir, normal, started, out_hit);
        return 1;
    }
    {
        rt_collider_pose pose;
        collider_pose_from_body(body, &pose);
        if (!raycast_collider_pose(
                body->collider, &pose, origin, dir, max_distance, &t, normal, &started, &leaf))
            return 0;
    }
    ray_fill_hit(body, t, max_distance, origin, dir, normal, started, out_hit);
    if (out_hit && leaf)
        out_hit->collider = leaf;
    return 1;
}

/// @brief Closest-hit raycast core over sanitized raw components.
/// @details @p origin and @p dir are mutated in place by sanitization. The query broad phase
///          rejects bodies outside the finite ray AABB, while allocation failure falls back to
///          direct body iteration. The boxed public raycast and raw closest-body entry share this
///          allocation-free narrow-phase loop.
/// @param w Borrowed world containing the bodies to query.
/// @param[in,out] origin Three-element ray origin to sanitize.
/// @param[in,out] dir Three-element direction to sanitize and normalize.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @param ignore_body Optional borrowed body to exclude from the query.
/// @param[out] out_hit Destination for the nearest sanitized hit.
/// @return `1` when a selected body is hit; `0` for invalid input or no hit.
static int world3d_raycast_closest_core(rt_world3d *w,
                                        double *origin,
                                        double *dir,
                                        double max_distance,
                                        int64_t mask,
                                        const void *ignore_body,
                                        rt_query_hit3d *out_hit) {
    rt_query_hit3d best_hit = {0};
    int found = 0;
    double query_min[3], query_max[3], end[3];
    if (!w || !origin || !dir || !out_hit || !isfinite(max_distance) || max_distance <= 0.0)
        return 0;
    max_distance = query_sanitize_distance(max_distance);
    if (!query_normalize_direction(dir))
        return 0;
    end[0] = query_saturate_coord(origin[0] + dir[0] * max_distance);
    end[1] = query_saturate_coord(origin[1] + dir[1] * max_distance);
    end[2] = query_saturate_coord(origin[2] + dir[2] * max_distance);
    for (int axis = 0; axis < 3; ++axis) {
        query_min[axis] = fmin(origin[axis], end[axis]);
        query_max[axis] = fmax(origin[axis], end[axis]);
    }
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
        if (!body || body == (const rt_body3d *)ignore_body ||
            !body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (!raycast_body(body, origin, dir, max_distance, &hit))
            continue;
        if (!found || hit.distance < best_hit.distance) {
            best_hit = hit;
            found = 1;
        }
    }
    if (found)
        *out_hit = best_hit;
    return found;
}

/// @brief Implements `World3D.Raycast` and returns the nearest hit along a ray.
/// @details Collider-specific tests support simple primitives, mesh and convex-hull triangles,
///          compound leaves, and heightfields. The direction is normalized internally and a
///          non-positive maximum distance is rejected.
/// @param obj Borrowed `World3D` runtime object.
/// @param origin_obj Borrowed `Vec3` ray origin in world coordinates.
/// @param direction_obj Borrowed nonzero `Vec3` direction; its magnitude does not affect reach.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed nearest `PhysicsHit3D`, or null for invalid inputs or a ray with no hit.
void *rt_world3d_raycast(
    void *obj, void *origin_obj, void *direction_obj, double max_distance, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d best_hit;
    double origin[3];
    double dir[3];
    if (!w || !rt_g3d_is_vec3(origin_obj) || !rt_g3d_is_vec3(direction_obj) ||
        !isfinite(max_distance) || max_distance <= 0.0)
        return NULL;
    if (!query_read_vec3(origin_obj, origin) || !query_read_vec3(direction_obj, dir))
        return NULL;
    if (!world3d_raycast_closest_core(w, origin, dir, max_distance, mask, NULL, &best_hit))
        return NULL;
    return physics_hit3d_new(&best_hit);
}

/// @brief Allocation-free closest-hit raycast: raw components in, borrowed body out.
/// @details Internal engine entry (AI line-of-sight, gameplay probes) that skips
///   Vec3 boxing and hit-object allocation entirely. Returns the closest hit
///   body (borrowed — do NOT release) and writes the hit distance to
///   @p out_distance when non-NULL, or returns NULL for no hit.
/// @param obj Borrowed `World3D` runtime object.
/// @param ox World-space ray-origin x coordinate.
/// @param oy World-space ray-origin y coordinate.
/// @param oz World-space ray-origin z coordinate.
/// @param dx Ray-direction x component.
/// @param dy Ray-direction y component.
/// @param dz Ray-direction z component.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @param ignore_body Optional borrowed body excluded from the query.
/// @param[out] out_distance Optional destination initialized to `-1` and replaced with the hit
///        distance on success.
/// @return The borrowed nearest body, or null for invalid input or no hit.
void *rt_world3d_raycast_closest_body_raw(void *obj,
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
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d hit;
    double origin[3] = {ox, oy, oz};
    double dir[3] = {dx, dy, dz};
    if (out_distance)
        *out_distance = -1.0;
    if (!w)
        return NULL;
    if (!world3d_raycast_closest_core(w, origin, dir, max_distance, mask, ignore_body, &hit))
        return NULL;
    if (out_distance)
        *out_distance = hit.distance;
    return hit.body;
}

/// @brief Shared raycast-all core: fill the world's sorted hit scratch from raw inputs.
/// @details `origin`/`dir` are mutated in place by sanitization. Returns the number of
///   hits kept in the scratch (bounded by the world's query capacity) and writes the
///   unbounded pierce count to @p out_total; returns -1 on invalid input.
/// @param w Borrowed world containing bodies and the configured query capacity.
/// @param[out] hits World-owned writable scratch array with capacity `w->max_query_hits`.
/// @param[in,out] origin Three-element ray origin to sanitize.
/// @param[in,out] dir Three-element direction to sanitize and normalize.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @param[out] out_total Optional destination for the logical unbounded hit count.
/// @return The number of nearest-first records retained in @p hits, or `-1` for invalid input.
static int32_t world3d_raycast_all_core(rt_world3d *w,
                                        rt_query_hit3d *hits,
                                        double *origin,
                                        double *dir,
                                        double max_distance,
                                        int64_t mask,
                                        int64_t *out_total) {
    double query_min[3], query_max[3], end[3];
    int32_t hit_capacity = w ? w->max_query_hits : 0;
    int32_t hit_count = 0;
    int64_t total_count = 0;
    if (out_total)
        *out_total = 0;
    if (!w || !hits || !isfinite(max_distance) || max_distance <= 0.0)
        return -1;
    max_distance = query_sanitize_distance(max_distance);
    if (!query_normalize_direction(dir))
        return -1;
    end[0] = query_saturate_coord(origin[0] + dir[0] * max_distance);
    end[1] = query_saturate_coord(origin[1] + dir[1] * max_distance);
    end[2] = query_saturate_coord(origin[2] + dir[2] * max_distance);
    for (int axis = 0; axis < 3; ++axis) {
        query_min[axis] = fmin(origin[axis], end[axis]);
        query_max[axis] = fmax(origin[axis], end[axis]);
    }
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
        if (!body || !body3d_has_collision_geometry(body) || !query_mask_matches_body(body, mask))
            continue;
        if (raycast_body(body, origin, dir, max_distance, &hit)) {
            total_count++;
            hit_count = query_hit_insert_sorted_bounded(hits, hit_count, hit_capacity, &hit);
        }
    }
    if (out_total)
        *out_total = total_count;
    return hit_count;
}

/// @brief `World3D.RaycastAll(origin, direction, maxDistance, mask)` — all hits along a ray,
/// sorted.
///
/// Like `Raycast` but doesn't stop at the first hit — every body
/// the ray pierces is recorded. Results come back sorted by distance.
/// Bounded by the world's configurable query capacity
/// (`Physics3DWorld.SetMaxQueryHits`, default 256).
/// @param obj Borrowed `World3D` runtime object.
/// @param origin_obj Borrowed `Vec3` ray origin in world coordinates.
/// @param direction_obj Borrowed nonzero `Vec3` direction.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @return A newly boxed nearest-first `PhysicsHitList3D`, possibly empty or marked truncated;
///         or null for invalid inputs or scratch allocation failure.
void *rt_world3d_raycast_all(
    void *obj, void *origin_obj, void *direction_obj, double max_distance, int64_t mask) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d *hits = world3d_query_hits_scratch(w);
    double origin[3], dir[3];
    int64_t total_count = 0;
    if (!w || !hits || !rt_g3d_is_vec3(origin_obj) || !rt_g3d_is_vec3(direction_obj))
        return NULL;
    if (!query_read_vec3(origin_obj, origin) || !query_read_vec3(direction_obj, dir))
        return NULL;
    int32_t hit_count =
        world3d_raycast_all_core(w, hits, origin, dir, max_distance, mask, &total_count);
    if (hit_count < 0)
        return NULL;
    return physics_hit_list3d_new_ex(
        hits, hit_count, total_count, total_count > (int64_t)hit_count);
}

/// @brief C-internal raycast-all fast path: write the non-trigger bodies the ray
///   pierces (nearest-first) into @p out_bodies as borrowed pointers, without boxing
///   a hit-list object or requiring Vec3 handles. Used by per-frame paths like the
///   third-person occluder fade.
/// @param obj Borrowed `World3D` runtime object.
/// @param origin_in Borrowed three-element ray origin.
/// @param direction_in Borrowed nonzero three-element ray direction.
/// @param max_distance Positive maximum ray distance in world units.
/// @param mask Collision-layer bit mask used to select candidate bodies.
/// @param[out] out_bodies Caller-owned array that receives borrowed non-trigger body pointers.
/// @param out_cap Maximum number of pointers writable to @p out_bodies.
/// @return Number of bodies written (bounded by @p out_cap), or 0 on invalid input.
int32_t rt_world3d_raycast_all_bodies_raw(void *obj,
                                          const double *origin_in,
                                          const double *direction_in,
                                          double max_distance,
                                          int64_t mask,
                                          void **out_bodies,
                                          int32_t out_cap) {
    rt_world3d *w = world3d_checked(obj);
    rt_query_hit3d *hits = world3d_query_hits_scratch(w);
    double origin[3], dir[3];
    int32_t written = 0;
    if (!w || !hits || !origin_in || !direction_in || !out_bodies || out_cap <= 0)
        return 0;
    memcpy(origin, origin_in, sizeof(origin));
    memcpy(dir, direction_in, sizeof(dir));
    int32_t hit_count = world3d_raycast_all_core(w, hits, origin, dir, max_distance, mask, NULL);
    for (int32_t i = 0; i < hit_count && written < out_cap; ++i) {
        if (hits[i].is_trigger || !hits[i].body)
            continue;
        out_bodies[written++] = hits[i].body;
    }
    return written;
}

#else
typedef int rt_physics3d_query_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
