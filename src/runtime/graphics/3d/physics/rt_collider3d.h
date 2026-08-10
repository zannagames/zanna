//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_collider3d.h
// Purpose: Collider3D runtime surface for reusable 3D collision shapes.
//
// Key invariants:
//   - Collider3D is a reusable shape object; Physics3DBody owns one active
//     collider at a time.
//   - Triangle-mesh and heightfield colliders are static-only in v1.
//   - Compound colliders own child references plus copied local transforms.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_collider3d.h
 * @brief Declares Collider3D construction, bounds, material, and raw-query APIs.
 *
 * Collider3D handles represent reusable box, sphere, capsule, convex-hull,
 * triangle-mesh, compound, or heightfield shapes. Public functions construct
 * and inspect runtime-managed shapes; raw helpers provide allocation-free
 * geometry access for the physics implementation.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum public compound nesting. Collision of two legal compound trees can recurse through
 * both sides, so the pair dispatcher separately allows twice this depth. */
#define RT_COLLIDER3D_MAX_COMPOUND_DEPTH 64

#define RT_COLLIDER3D_TYPE_BOX 0
#define RT_COLLIDER3D_TYPE_SPHERE 1
#define RT_COLLIDER3D_TYPE_CAPSULE 2
#define RT_COLLIDER3D_TYPE_CONVEX_HULL 3
#define RT_COLLIDER3D_TYPE_MESH 4
#define RT_COLLIDER3D_TYPE_COMPOUND 5
#define RT_COLLIDER3D_TYPE_HEIGHTFIELD 6

/// @brief Create a box collider with half-extents (hx, hy, hz) along each axis.
/// @param hx X-axis half-extent.
/// @param hy Y-axis half-extent.
/// @param hz Z-axis half-extent.
/// @return Newly allocated box collider, or NULL on allocation failure.
void *rt_collider3d_new_box(double hx, double hy, double hz);

/// @brief Create a sphere collider with the given radius.
/// @param radius Sphere radius, sanitized to a positive supported extent.
/// @return Newly allocated sphere collider, or NULL on allocation failure.
void *rt_collider3d_new_sphere(double radius);

/// @brief Create a capsule collider (radius + total height including caps).
/// @param radius Capsule radius.
/// @param height Total tip-to-tip height, floored to twice the radius.
/// @return Newly allocated capsule collider, or NULL on allocation failure.
void *rt_collider3d_new_capsule(double radius, double height);

/// @brief Create a convex-hull collider from a mesh's vertex cloud (computes the hull internally).
/// @param mesh Mesh3D handle retained as convex support geometry.
/// @return Newly allocated convex-hull collider, or NULL after invalid input or allocation
/// failure.
void *rt_collider3d_new_convex_hull(void *mesh);

/// @brief Quickhull-reduced convex hull collider (max_verts clamped 8-255).
/// @param mesh Source Mesh3D whose vertex cloud is hulled.
/// @param max_verts Requested maximum hull vertices, clamped to 8 through 255.
/// @return Newly allocated collider owning the reduced hull mesh, or NULL after failure.
void *rt_collider3d_new_convex_hull_reduced(void *mesh, int64_t max_verts);

/// @brief Create a triangle-mesh collider (static only — cannot rotate dynamically).
/// @param mesh Mesh3D handle retained as triangle collision geometry.
/// @return Newly allocated static-only mesh collider, or NULL after failure.
void *rt_collider3d_new_mesh(void *mesh);

/// @brief Create a heightfield collider from a Pixels heightmap with per-axis world-scale.
/// @param heightmap Pixels handle containing a two-dimensional 16-bit RG height grid.
/// @param scale_x X-axis spacing between samples.
/// @param scale_y Height multiplier.
/// @param scale_z Z-axis spacing between samples.
/// @return Newly allocated static-only heightfield collider, or NULL after failure.
void *rt_collider3d_new_heightfield(void *heightmap,
                                    double scale_x,
                                    double scale_y,
                                    double scale_z);
/// @brief Create an empty compound collider — combine multiple shapes via `_add_child`.
/// @return Newly allocated empty compound collider, or NULL on allocation failure.
void *rt_collider3d_new_compound(void);

/// @brief Add a child collider to a compound at @p local_transform (Mat4 in compound's local
/// space).
/// @param compound Compound Collider3D handle to modify.
/// @param child Collider3D handle retained by the compound.
/// @param local_transform Optional Transform3D describing child-to-compound placement.
void rt_collider3d_add_child(void *compound, void *child, void *local_transform);

/// @brief Get the collider's type tag (one of RT_COLLIDER3D_TYPE_*).
/// @param collider Collider3D handle to inspect.
/// @return Valid shape discriminator, or -1 for an invalid handle.
int64_t rt_collider3d_get_type(void *collider);

/// @brief Get the local-space AABB minimum corner as a Vec3.
/// @param collider Collider3D handle to query.
/// @return Newly allocated Vec3 minimum, or the origin for an invalid handle.
void *rt_collider3d_get_local_bounds_min(void *collider);

/// @brief Get the local-space AABB maximum corner as a Vec3.
/// @param collider Collider3D handle to query.
/// @return Newly allocated Vec3 maximum, or the origin for an invalid handle.
void *rt_collider3d_get_local_bounds_max(void *collider);

/* Internal physics/runtime helpers — pointer-out variants used by the physics
   solver to avoid boxing Vec3s on hot paths. */
/// @brief Write the local-space AABB (min/max xyz) into the caller's buffers.
/// @param collider Collider3D handle to query.
/// @param min_out Required three-component minimum output.
/// @param max_out Required three-component maximum output.
void rt_collider3d_get_local_bounds_raw(void *collider, double *min_out, double *max_out);

/// @brief Monotonic revision that changes whenever cached collider bounds change.
/// @param collider Collider3D handle to inspect.
/// @return Refreshed bounds revision, or zero for an invalid handle.
uint64_t rt_collider3d_get_bounds_revision_raw(void *collider);

/// @brief Process-wide monotonic epoch for collider hierarchy mutations.
/// @return Current atomically published collider-geometry epoch.
uint64_t rt_collider3d_global_geometry_epoch(void);

/// @brief Compute the world-space AABB for a collider placed at the given
///        position/rotation/scale; results written to @p min_out / @p max_out.
/// @param collider Collider3D handle whose local bounds are transformed.
/// @param position Optional three-component world translation.
/// @param rotation Optional unit XYZW world rotation.
/// @param scale Optional three-component world scale.
/// @param min_out Required world-space AABB minimum output.
/// @param max_out Required world-space AABB maximum output.
void rt_collider3d_compute_world_aabb_raw(void *collider,
                                          const double *position,
                                          const double *rotation,
                                          const double *scale,
                                          double *min_out,
                                          double *max_out);
/// @brief Whether the collider is flagged static-only (never moves; lets the
///        broadphase skip dynamic re-insertion).
/// @param collider Collider3D handle to inspect.
/// @return One for a valid static-only collider, otherwise zero.
int8_t rt_collider3d_is_static_only_raw(void *collider);

/// @brief Box collider: write its half-extents (xyz) to @p half_extents_out.
/// @param collider Collider3D handle to inspect.
/// @param half_extents_out Required three-component destination.
void rt_collider3d_get_box_half_extents_raw(void *collider, double *half_extents_out);

/// @brief Reset an existing box collider's half-extents and cached bounds for internal query reuse.
/// @param collider Box Collider3D handle to mutate.
/// @param hx Replacement X-axis half-extent.
/// @param hy Replacement Y-axis half-extent.
/// @param hz Replacement Z-axis half-extent.
void rt_collider3d_reset_box_raw(void *collider, double hx, double hy, double hz);

/// @brief Sphere/capsule collider: its radius.
/// @param collider Collider3D handle to inspect.
/// @return Sanitized radius, or zero for an unsupported shape.
double rt_collider3d_get_radius_raw(void *collider);

/// @brief Reset an existing sphere collider's radius and cached bounds for internal query reuse.
/// @param collider Sphere Collider3D handle to mutate.
/// @param radius Replacement radius.
void rt_collider3d_reset_sphere_raw(void *collider, double radius);

/// @brief Capsule collider: total height including caps.
/// @param collider Collider3D handle to inspect.
/// @return Sanitized capsule height, or zero for an unsupported shape.
double rt_collider3d_get_height_raw(void *collider);

/// @brief Reset an existing capsule collider's radius/height and cached bounds
///        (crouch/stand resizes and internal query reuse). Height floors to 2*radius.
/// @param collider Capsule Collider3D handle to mutate.
/// @param radius Replacement radius.
/// @param height Replacement total tip-to-tip height.
void rt_collider3d_reset_capsule_raw(void *collider, double radius, double height);

/// @brief Overlap two colliders at explicit poses (position + quaternion) through
///        the standard narrow-phase; writes normal/depth/witness point on hit.
///        Combat-volume primitive — no bodies are registered in any world.
/// @param collider_a First Collider3D handle.
/// @param pos_a Three-component world position of the first shape.
/// @param quat_a Finite XYZW world rotation of the first shape, normalized internally.
/// @param collider_b Second Collider3D handle.
/// @param pos_b Three-component world position of the second shape.
/// @param quat_b Finite XYZW world rotation of the second shape, normalized internally.
/// @param out_normal Optional unit contact normal output; initialized to world up on a miss.
/// @param out_depth Optional bounded penetration-depth output; initialized to zero on a miss.
/// @param out_point Optional bounded witness-point output; initialized to the origin on a miss.
/// @return One when the posed shapes overlap, otherwise zero.
int8_t rt_collider3d_overlap_at_raw(void *collider_a,
                                    const double *pos_a,
                                    const double *quat_a,
                                    void *collider_b,
                                    const double *pos_b,
                                    const double *quat_b,
                                    double *out_normal,
                                    double *out_depth,
                                    double *out_point);
/// @brief Mesh collider: the backing triangle-mesh handle (NULL if not a mesh).
/// @param collider Collider3D handle to inspect.
/// @return Borrowed Mesh3D handle for a mesh-backed shape, otherwise NULL.
void *rt_collider3d_get_mesh_raw(void *collider);

/// @brief Compound collider: number of child colliders.
/// @param collider Collider3D handle to inspect.
/// @return Safely bounded child count, or zero for a non-compound shape.
int64_t rt_collider3d_get_child_count_raw(void *collider);

/// @brief Compound collider: child collider at @p index (NULL if out of range).
/// @param collider Compound Collider3D handle to inspect.
/// @param index Zero-based child index.
/// @return Borrowed child collider handle, or NULL when unavailable.
void *rt_collider3d_get_child_raw(void *collider, int64_t index);

/// @brief Compound collider: child @p index's local transform, written to the
///        position/rotation/scale out buffers.
/// @param compound Compound Collider3D handle to inspect.
/// @param index Zero-based child index.
/// @param position_out Optional three-component local position output.
/// @param rotation_out Optional four-component XYZW local rotation output.
/// @param scale_out Optional three-component local scale output.
void rt_collider3d_get_child_transform_raw(
    void *compound, int64_t index, double *position_out, double *rotation_out, double *scale_out);
/// @brief Per-collider friction override (-1 = unset: body friction applies).
/// @param collider Collider3D handle to modify.
/// @param friction Non-negative override capped at 16; negative or non-finite values clear it.
void rt_collider3d_set_friction(void *collider, double friction);

/// @brief Read the per-collider friction override.
/// @param collider Collider3D handle to inspect.
/// @return Stored override, or -1 when unset or invalid.
double rt_collider3d_get_friction(void *collider);

/// @brief Per-collider restitution override (-1 = unset: body value applies).
/// @param collider Collider3D handle to modify.
/// @param restitution Override clamped to `[0,1]`; negative or non-finite values clear it.
void rt_collider3d_set_restitution(void *collider, double restitution);

/// @brief Read the per-collider restitution override.
/// @param collider Collider3D handle to inspect.
/// @return Stored override, or -1 when unset or invalid.
double rt_collider3d_get_restitution(void *collider);

/// @brief Surface-type tag (Game3D.Surfaces registry id; 0 = untyped).
/// @param collider Collider3D handle to modify.
/// @param surface_type Non-negative registry identifier; negative values normalize to zero.
void rt_collider3d_set_surface_type(void *collider, int64_t surface_type);

/// @brief Read the collider's surface-type tag.
/// @param collider Collider3D handle to inspect.
/// @return Non-negative registry identifier, or zero when invalid or untyped.
int64_t rt_collider3d_get_surface_type(void *collider);

/// @brief Internal: effective per-side contact material resolution.
/// @param collider Collider3D handle whose friction override is consulted.
/// @param body_friction Body-level fallback friction.
/// @return Collider friction override when set, otherwise @p body_friction.
double rt_collider3d_effective_friction_raw(void *collider, double body_friction);

/// @brief Resolve effective per-side restitution from collider and body material.
/// @param collider Collider3D handle whose restitution override is consulted.
/// @param body_restitution Body-level fallback restitution.
/// @return Collider restitution override when set, otherwise @p body_restitution.
double rt_collider3d_effective_restitution_raw(void *collider, double body_restitution);

/// @brief Install (or clear) a heightfield hole bitmask (bit per cell); 1 on success.
/// @param collider Heightfield Collider3D handle to modify.
/// @param mask Optional packed row-major cell bits to copy; NULL clears all holes.
/// @param cells_x Number of cell columns represented by @p mask.
/// @param cells_z Number of cell rows represented by @p mask.
/// @return One on success, or zero for a non-heightfield or dimension mismatch.
int8_t rt_collider3d_heightfield_set_holes_raw(void *collider,
                                               const uint8_t *mask,
                                               int32_t cells_x,
                                               int32_t cells_z);

/// @brief Heightfield collider: sample height and surface normal at a local XZ point.
/// @param collider Heightfield Collider3D handle to sample.
/// @param local_x Local-space X coordinate.
/// @param local_z Local-space Z coordinate.
/// @param height_out Optional output receiving scaled local Y height.
/// @param normal_out Optional three-component output receiving the unit surface normal.
/// @return Nonzero when the point lies on an in-range, non-holed cell.
int8_t rt_collider3d_sample_heightfield_raw(
    void *collider, double local_x, double local_z, double *height_out, double *normal_out);

/// @brief Heightfield collider: expose grid dimensions and local cell scale to physics queries.
/// @param collider Heightfield Collider3D handle to inspect.
/// @param width_out Optional output receiving sample-column count.
/// @param depth_out Optional output receiving sample-row count.
/// @param scale_out Optional three-component output receiving sanitized sample scale.
/// @return One when valid heightfield information is available, otherwise zero.
int8_t rt_collider3d_get_heightfield_info_raw(void *collider,
                                              int32_t *width_out,
                                              int32_t *depth_out,
                                              double *scale_out);

#ifdef __cplusplus
}
#endif
