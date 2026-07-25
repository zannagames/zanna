//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_collision.c
// Purpose: Narrow-phase collision detection and contact-manifold generation for
//   the Physics3D runtime — primitive tests (sphere/box/capsule/AABB), OBB
//   clipping, GJK/EPA convex-hull tests, mesh/heightfield/compound colliders,
//   the top-level test_collider_pair dispatch, and contact de-dup. Split out of
//   rt_physics3d.c; shares core types via rt_physics3d_internal.h.
//
// Key invariants:
//   - test_collision/test_collider_pair return a normal pointing from body A to
//     body B with a positive penetration depth, plus the contributing leaves.
//   - Manifolds carry up to PH3D_MAX_MANIFOLD_POINTS points; OBB pairs clip a
//     reference face against the incident face to recover a stable manifold.
//   - Mesh narrow-phase traverses the shared rt_physics_mesh_bvh_node BVH built
//     by mesh_physics_bvh_rebuild (rt_physics3d_query.c).
//
// Links: rt_physics3d_internal.h, rt_physics3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_collision.c
 * @brief Assembles Physics3D narrow-phase collision and manifold generation.
 *
 * The translation unit supplies shared collision dependencies and includes the
 * contact, primitive/compound narrow-phase, and mesh/convex implementation
 * fragments in dependency order. Pair tests preserve the body-A-to-body-B
 * normal convention and bounded contact-manifold capacity.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations for collision-internal helpers (defined below, but
// referenced earlier by the OBB/manifold helpers).
/// @brief Compute scaled half-extents for a posed box collider.
/// @param collider Box Collider3D handle whose local extents are read.
/// @param pose World pose providing per-axis scale.
/// @param half_extents Three-component output receiving non-negative scaled extents.
static void box_scaled_half_extents(void *collider,
                                    const rt_collider_pose *pose,
                                    double *half_extents);

/// @brief Derive the world-space orthonormal axes for a pose rotation.
/// @param pose Collider pose providing a normalized world rotation.
/// @param axes Three-by-three output receiving the rotated local basis vectors.
static void pose_rotation_axes(const rt_collider_pose *pose, double axes[3][3]);

/// @brief Test the built-in primitive shape pair represented directly by two bodies.
/// @param a First Body3D, establishing the contact-normal origin.
/// @param b Second Body3D, toward which the contact normal points.
/// @param normal Three-component output receiving the A-to-B unit normal.
/// @param depth Output receiving positive penetration depth.
/// @return One when the primitive pair overlaps, otherwise zero.
static int test_simple_collision(const rt_body3d *a,
                                 const rt_body3d *b,
                                 double *normal,
                                 double *depth);

#include "rt_physics3d_collision_contacts.inc"
#include "rt_physics3d_collision_narrowphase.inc"
#include "rt_physics3d_collision_meshconvex.inc"
#else
typedef int rt_physics3d_collision_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
