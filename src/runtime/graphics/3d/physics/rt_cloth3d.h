//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_cloth3d.h
// Purpose: Cloth3D — from-scratch verlet cloth (chains for capes/hair tails,
//   patches for banners/flags) with sphere/capsule collision, pinning, wind,
//   fixed-substep determinism, and bone-chain or mesh output bindings.
// Key invariants:
//   - Simulation state is doubles-only with a fixed substep (default 1/120):
//     identical step sequences replay bit-identical on VM and native.
//   - Chains bound to a bone chain simulate in the skeleton's model space;
//     patches simulate in the bound mesh's local space.
// Ownership/Lifetime:
//   - GC-managed handle; retains its bound mesh/animator; finalizer frees
//     the point/constraint arrays and releases the bindings.
// Links: misc/plans/thirdpersonupgrade/27-cloth.md, ADR 0096,
//        src/runtime/graphics/3d/physics/rt_cloth3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_cloth3d.h
 * @brief Declares deterministic Verlet cloth simulation and output bindings.
 *
 * Cloth3D supports linear chains and rectangular patches with pinning, wind,
 * fixed-capacity sphere or capsule collision, and fixed-substep constraint
 * relaxation. Runtime-managed cloths may retain a Mesh3D or AnimController3D
 * binding and may also be registered for automatic World3D stepping.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a hanging chain: segments+1 points spanning total_length.
/// @param segments Number of links in the inclusive range 1 through 256.
/// @param total_length Positive combined length of all links.
/// @return Newly allocated Cloth3D chain, or NULL after reporting invalid input or allocation
/// failure.
void *rt_cloth3d_new_chain(int64_t segments, double total_length);

/// @brief Create a rectangular patch grid of w x h points spanning width x height.
/// @param w Number of point columns in the inclusive range 2 through 64.
/// @param h Number of point rows in the inclusive range 2 through 64.
/// @param width Positive horizontal span.
/// @param height Positive vertical span.
/// @return Newly allocated Cloth3D patch, or NULL after reporting invalid input or allocation
/// failure.
void *rt_cloth3d_new_patch(int64_t w, int64_t h, double width, double height);

/// @brief Get the velocity damping factor (0..1 per substep, default 0.02).
/// @param cloth Cloth3D handle to inspect.
/// @return Current damping factor, or zero after invalid-handle reporting.
double rt_cloth3d_get_damping(void *cloth);

/// @brief Set the velocity damping factor (0..1).
/// @param cloth Cloth3D handle to modify.
/// @param damping Finite factor clamped to the inclusive range zero to one.
void rt_cloth3d_set_damping(void *cloth, double damping);

/// @brief Get the constraint relaxation iterations per substep (default 4).
/// @param cloth Cloth3D handle to inspect.
/// @return Current iteration count, or zero after invalid-handle reporting.
int64_t rt_cloth3d_get_iterations(void *cloth);

/// @brief Set the constraint relaxation iterations (1..32).
/// @param cloth Cloth3D handle to modify.
/// @param iterations Positive iteration count capped at 32.
void rt_cloth3d_set_iterations(void *cloth, int64_t iterations);

/// @brief Get the gravity scale (default 1).
/// @param cloth Cloth3D handle to inspect.
/// @return Current gravity multiplier, or zero after invalid-handle reporting.
double rt_cloth3d_get_gravity_scale(void *cloth);

/// @brief Set the gravity scale (0 disables gravity).
/// @param cloth Cloth3D handle to modify.
/// @param scale Finite gravity multiplier clamped to `[-1000000, 1000000]`;
///              negative values reverse the acceleration direction.
void rt_cloth3d_set_gravity_scale(void *cloth, double scale);

/// @brief Get the wind response coefficient (default 1).
/// @param cloth Cloth3D handle to inspect.
/// @return Current non-negative wind-response coefficient, or zero after invalid-handle reporting.
double rt_cloth3d_get_wind_response(void *cloth);

/// @brief Set the wind response coefficient (0 disables wind coupling).
/// @param cloth Cloth3D handle to modify.
/// @param response Finite non-negative wind-response coefficient clamped to 120.
void rt_cloth3d_set_wind_response(void *cloth, double response);

/// @brief Number of simulated points.
/// @param cloth Cloth3D handle to inspect.
/// @return Fixed point count, or zero after invalid-handle reporting.
int64_t rt_cloth3d_get_point_count(void *cloth);

/// @brief Fluent: pin the point at @p index to its current position.
/// @param cloth Cloth3D handle to modify.
/// @param index Zero-based simulated point index.
/// @return The original @p cloth handle.
void *rt_cloth3d_pin(void *cloth, int64_t index);

/// @brief Fluent: add a static sphere collider (center Vec3, radius).
/// @param cloth Cloth3D handle to modify.
/// @param center Vec3 sphere center in the cloth's simulation space.
/// @param radius Positive collision radius.
/// @return The original @p cloth handle.
void *rt_cloth3d_add_sphere(void *cloth, void *center, double radius);

/// @brief Fluent: add a static capsule collider (segment a..b Vec3s, radius).
/// @param cloth Cloth3D handle to modify.
/// @param a Vec3 capsule segment start in simulation space.
/// @param b Vec3 capsule segment end in simulation space.
/// @param radius Positive collision radius surrounding the segment.
/// @return The original @p cloth handle.
void *rt_cloth3d_add_capsule(void *cloth, void *a, void *b, double radius);

/// @brief Set the wind velocity: direction Vec3 scaled by @p strength.
/// @param cloth Cloth3D handle to modify.
/// @param direction Vec3 direction and relative per-axis magnitude.
/// @param strength Scalar magnitude multiplier clamped to `[-1000000, 1000000]`;
///                 non-finite values become zero.
void rt_cloth3d_set_wind(void *cloth, void *direction, double strength);

/// @brief Current position of point @p index as a Vec3.
/// @param cloth Cloth3D handle to inspect.
/// @param index Zero-based simulated point index.
/// @return Newly allocated Vec3 position, or the origin for an invalid index or handle.
void *rt_cloth3d_get_point(void *cloth, int64_t index);

/// @brief Fluent: bind a patch to a Mesh3D rewritten in place each step.
/// @param cloth Patch Cloth3D handle that retains the binding.
/// @param mesh Mesh3D handle rebuilt with patch topology and updated after simulation.
/// @return The original @p cloth handle.
void *rt_cloth3d_bind_mesh(void *cloth, void *mesh);

/// @brief Fluent: bind a chain to an animator's linear bone chain from
///   @p root_bone; the root anchors the chain and simulated directions are
///   written back as aim rotations in the post-animation override slot.
/// @param cloth Chain Cloth3D handle that retains the binding.
/// @param animator AnimController3D handle with the target skeleton.
/// @param root_bone Name of the first bone in the linear descendant chain.
/// @return The original @p cloth handle.
void *rt_cloth3d_bind_bone_chain(void *cloth, void *animator, rt_string root_bone);

/// @brief Advance the simulation by @p dt seconds (fixed internal substeps),
///   including anchor sync and output bindings. World-registered cloths are
///   stepped automatically by World3D.StepSimulation. Catch-up is capped at
///   eight substeps per call; excess whole substeps are dropped while the
///   fractional remainder is retained to avoid an unbounded stall.
/// @param cloth Cloth3D handle to advance.
/// @param dt Positive finite elapsed time accumulated into fixed substeps.
void rt_cloth3d_step(void *cloth, double dt);

/// @brief Register a cloth to tick inside World3D.StepSimulation (retained).
/// @param world World3D handle that retains the registration.
/// @param cloth Cloth3D handle to register once.
void rt_game3d_world_add_cloth(void *world, void *cloth);

/// @brief Unregister a world-ticked cloth (released).
/// @param world World3D handle whose registration list is searched.
/// @param cloth Exact Cloth3D handle to remove and release.
void rt_game3d_world_remove_cloth(void *world, void *cloth);

#ifdef __cplusplus
}
#endif
