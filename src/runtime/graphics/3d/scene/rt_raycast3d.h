//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_raycast3d.h
// Purpose: 3D raycasting and AABB collision — ray-triangle (Möller–Trumbore),
//   ray-mesh (retained BVH plus AABB early-out), ray-AABB (slab method), ray-sphere,
//   and AABB-AABB overlap/penetration tests.
//
// Key invariants:
//   - All ray functions take Vec3 origin + direction; non-zero directions are normalized
//   internally.
//   - Distance return: >= 0 = Euclidean hit distance, -1 = no hit.
//   - IntersectMesh returns a RayHit3D object (or NULL) with hit point, normal, triangle index.
//   - AABB functions take Vec3 min/max corners.
//
// Ownership/Lifetime:
//   - Query arguments are borrowed; returned hit/point/vector objects are GC-managed.
//   - Mesh BVH storage remains privately owned by the queried Mesh3D revision.
//
// Links: plans/fps-support.md, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares validated ray, bounds, sphere, capsule, and mesh intersection queries.
/// @details Every argument is borrowed. Ray directions are normalized internally,
/// AABB corners are canonicalized, and finite outputs use world-space units.
/// Scalar ray queries return `-1` on a miss; vector queries return fresh Vec3
/// handles, while mesh queries return a fresh RayHit3D or `NULL`.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ray intersection */
/// @brief Möller-Trumbore ray-triangle test. Returns Euclidean hit distance, -1 on miss.
/// @param origin Borrowed Vec3 ray origin.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param v0 Borrowed Vec3 first triangle vertex.
/// @param v1 Borrowed Vec3 second triangle vertex.
/// @param v2 Borrowed Vec3 third triangle vertex.
/// @return Non-negative Euclidean hit distance, or `-1` on a miss or invalid input.
double rt_ray3d_intersect_triangle(void *origin, void *dir, void *v0, void *v1, void *v2);

/// @brief Ray-triangle test with optional backface culling (front_only != 0 rejects backfaces).
/// @param origin Borrowed Vec3 ray origin.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param v0 Borrowed Vec3 first triangle vertex.
/// @param v1 Borrowed Vec3 second triangle vertex.
/// @param v2 Borrowed Vec3 third triangle vertex.
/// @param front_only Nonzero to reject triangles facing away from the ray.
/// @return Non-negative Euclidean hit distance, or `-1` on a miss or invalid input.
double rt_ray3d_intersect_triangle_cull(
    void *origin, void *dir, void *v0, void *v1, void *v2, int8_t front_only);

/// @brief Test a transformed mesh through its retained triangle BVH; returns RayHit3D or NULL.
/// @param origin Borrowed Vec3 ray origin in world space.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param mesh Borrowed Mesh3D handle.
/// @param transform Optional borrowed Mat4 model transform; `NULL` means identity.
/// @return New nearest RayHit3D handle, or `NULL` on a miss or invalid input.
void *rt_ray3d_intersect_mesh(void *origin, void *dir, void *mesh, void *transform);

/// @brief Slab-method ray-AABB test. Returns Euclidean hit distance, -1 on miss.
/// @param origin Borrowed Vec3 ray origin.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @return Non-negative Euclidean entry distance, or `-1` on a miss or invalid input.
double rt_ray3d_intersect_aabb(void *origin, void *dir, void *aabb_min, void *aabb_max);

/// @brief Analytic ray-sphere test. Returns Euclidean hit distance, -1 on miss.
/// @param origin Borrowed Vec3 ray origin.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param center Borrowed Vec3 sphere center.
/// @param radius Non-negative sphere radius.
/// @return Nearest non-negative Euclidean hit distance, or `-1` on a miss or invalid input.
double rt_ray3d_intersect_sphere(void *origin, void *dir, void *center, double radius);

/* AABB collision */
/// @brief True if two AABBs overlap.
/// @param min_a Borrowed Vec3 minimum corner of box A.
/// @param max_a Borrowed Vec3 maximum corner of box A.
/// @param min_b Borrowed Vec3 minimum corner of box B.
/// @param max_b Borrowed Vec3 maximum corner of box B.
/// @return Nonzero when the closed boxes overlap or touch.
int8_t rt_aabb3d_overlaps(void *min_a, void *max_a, void *min_b, void *max_b);

/// @brief Get the per-axis penetration depth Vec3 between two overlapping AABBs (zero if disjoint).
/// @param min_a Borrowed Vec3 minimum corner of box A.
/// @param max_a Borrowed Vec3 maximum corner of box A.
/// @param min_b Borrowed Vec3 minimum corner of box B.
/// @param max_b Borrowed Vec3 maximum corner of box B.
/// @return New minimum-axis separation vector for A, or the zero vector when disjoint.
void *rt_aabb3d_penetration(void *min_a, void *max_a, void *min_b, void *max_b);

/* Shape-shape collision */
/// @brief True if two spheres overlap (distance between centers < sum of radii).
/// @param center_a Borrowed Vec3 center of sphere A.
/// @param radius_a Radius of sphere A.
/// @param center_b Borrowed Vec3 center of sphere B.
/// @param radius_b Radius of sphere B.
/// @return Nonzero when the closed spheres overlap or touch.
int8_t rt_sphere3d_overlaps(void *center_a, double radius_a, void *center_b, double radius_b);

/// @brief Get the penetration vector between two overlapping spheres (zero if disjoint).
/// @param center_a Borrowed Vec3 center of sphere A.
/// @param radius_a Radius of sphere A.
/// @param center_b Borrowed Vec3 center of sphere B.
/// @param radius_b Radius of sphere B.
/// @return New separation vector for sphere A, or the zero vector when disjoint.
void *rt_sphere3d_penetration(void *center_a, double radius_a, void *center_b, double radius_b);

/// @brief Find the closest point on an AABB to @p point.
/// @details An interior query point projects to the nearest box face.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @param point Borrowed Vec3 query position.
/// @return New Vec3 on the box surface, or the zero vector for invalid input.
void *rt_aabb3d_closest_point(void *aabb_min, void *aabb_max, void *point);

/// @brief True if a sphere overlaps an AABB.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @param center Borrowed Vec3 sphere center.
/// @param radius Non-negative sphere radius.
/// @return Nonzero when the closed sphere and box overlap or touch.
int8_t rt_aabb3d_sphere_overlaps(void *aabb_min, void *aabb_max, void *center, double radius);

/// @brief Find the closest point on a line segment to @p point.
/// @param seg_a Borrowed Vec3 first segment endpoint.
/// @param seg_b Borrowed Vec3 second segment endpoint.
/// @param point Borrowed Vec3 query position.
/// @return New Vec3 on the finite segment, or the zero vector for invalid input.
void *rt_segment3d_closest_point(void *seg_a, void *seg_b, void *point);

/// @brief True if a capsule overlaps a sphere.
/// @param cap_a Borrowed Vec3 first capsule-axis endpoint.
/// @param cap_b Borrowed Vec3 second capsule-axis endpoint.
/// @param cap_radius Capsule radius.
/// @param sphere_center Borrowed Vec3 sphere center.
/// @param sphere_radius Sphere radius.
/// @return Nonzero when the closed capsule and sphere overlap or touch.
int8_t rt_capsule3d_sphere_overlaps(
    void *cap_a, void *cap_b, double cap_radius, void *sphere_center, double sphere_radius);

/// @brief True if a capsule overlaps an AABB.
/// @param cap_a Borrowed Vec3 first capsule-axis endpoint.
/// @param cap_b Borrowed Vec3 second capsule-axis endpoint.
/// @param radius Capsule radius.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @return Nonzero when the closed capsule and box overlap or touch.
int8_t rt_capsule3d_aabb_overlaps(
    void *cap_a, void *cap_b, double radius, void *aabb_min, void *aabb_max);

/* RayHit3D accessors */
/// @brief Get the hit distance from the ray origin.
/// @param hit Borrowed RayHit3D handle.
/// @return Non-negative world-space distance, or `-1` for an invalid handle.
double rt_ray3d_hit_distance(void *hit);

/// @brief Get the world-space hit point as a Vec3.
/// @param hit Borrowed RayHit3D handle.
/// @return New Vec3 hit position, or the zero vector for an invalid handle.
void *rt_ray3d_hit_point(void *hit);

/// @brief Get the surface normal at the hit point as a Vec3.
/// @param hit Borrowed RayHit3D handle.
/// @return New unit Vec3 normal, defaulting to positive Y for an invalid handle.
void *rt_ray3d_hit_normal(void *hit);

/// @brief Get the triangle index that was hit (within the mesh).
/// @param hit Borrowed RayHit3D handle.
/// @return Deterministic source triangle index, or `-1` for an invalid handle.
int64_t rt_ray3d_hit_triangle(void *hit);

#ifdef __cplusplus
}
#endif
