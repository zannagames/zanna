//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navagent3d.h
// Purpose: Gameplay-facing single-agent navigation layer on top of NavMesh3D.
//
// Key invariants:
//   - NavAgent3D owns a goal, sampled path corners, and local avoidance steering state.
//   - Update() can drive a bound Character3D or directly reposition a SceneNode3D.
//   - UpdateBatch() reads one immutable start-of-tick peer snapshot and publishes
//     selected agents in stable creation order.
//   - Auto-repath is intentionally simple in v1: periodic rebuilds while a goal
//     remains active and the agent is not within the stopping distance.
//
// Ownership/Lifetime:
//   - NavAgent3D is GC-managed and retains its navmesh and optional bindings.
//   - Batch input arrays are borrowed only for the duration of UpdateBatch.
//
// Links: rt_navmesh3d.h, rt_physics3d.h, rt_scene3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navagent3d.h
 * @brief Declares gameplay-facing NavMesh3D path following and local avoidance.
 *
 * A GC-managed agent owns its current target and sampled path, retains its navmesh and optional
 * CharacterController3D or SceneNode3D bindings, and can be advanced individually or through a
 * deterministic batch snapshot. Opaque runtime handles are validated at every API boundary.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a navigation agent on the given NavMesh3D with capsule dimensions.
/// @param navmesh Optional NavMesh3D handle retained by the new agent.
/// @param radius Requested non-negative capsule radius; zero selects the default.
/// @param height Requested non-negative capsule height; zero selects the default.
/// @return Newly allocated GC-managed NavAgent3D, or null for invalid input or allocation failure.
void *rt_navagent3d_new(void *navmesh, double radius, double height);

/// @brief Set a world-space goal, snap it to the navmesh, and immediately rebuild the path.
/// @param agent Opaque NavAgent3D handle.
/// @param position Borrowed Vec3 destination.
void rt_navagent3d_set_target(void *agent, void *position);
/// @brief Clear the current goal, path, and motion immediately.
/// @param agent Opaque NavAgent3D handle.
void rt_navagent3d_clear_target(void *agent);
/// @brief Tick the agent: traverse path corners, drive bound character/node, optionally repath.
/// @details An automatic repath is transactional: a transient query or allocation
///          failure preserves the existing valid path until a later retry.
/// @param agent Opaque NavAgent3D handle.
/// @param dt Frame duration in seconds, sanitized to the supported maximum step.
void rt_navagent3d_update(void *agent, double dt);
/// @brief Tick multiple agents through deterministic snapshot/solve/apply phases.
/// @details Valid unique handles are sorted by stable creation order. Every avoidance solve reads
///          the same immutable start-of-tick registry snapshot before any solved position is
///          published, so reversing @p agents cannot alter results. Invalid handles and duplicates
///          are ignored. This main-thread C API borrows the array for the duration of the call.
/// @param agents Array containing @p agent_count candidate NavAgent3D handles.
/// @param agent_count Number of entries in @p agents.
/// @param dt Tick duration in seconds, sanitized identically to individual Update.
/// @return Number of unique valid agents processed, or zero on invalid input/staging failure.
int64_t rt_navagent3d_update_batch(void *const *agents, int64_t agent_count, double dt);
/// @brief Teleport the agent to @p position (clears any active path).
/// @details The destination is navmesh-sampled, bindings are synchronized, and an active target
///          causes a replacement path to be built from the new position.
/// @param agent Opaque NavAgent3D handle.
/// @param position Borrowed Vec3 destination.
void rt_navagent3d_warp(void *agent, void *position);

/// @brief Get the agent's current world-space position as a Vec3.
/// @param agent Opaque NavAgent3D handle.
/// @return Newly allocated Vec3 owned by the caller; invalid handles produce the origin.
void *rt_navagent3d_get_position(void *agent);
/// @brief Get the agent's actual velocity from the previous Update as a Vec3.
/// @param agent Opaque NavAgent3D handle.
/// @return Newly allocated bounded Vec3 owned by the caller; invalid handles produce zero.
void *rt_navagent3d_get_velocity(void *agent);
/// @brief Get the steering output (desired velocity for the bound character/node) as a Vec3.
/// @param agent Opaque NavAgent3D handle.
/// @return Newly allocated bounded Vec3 owned by the caller; invalid handles produce zero.
void *rt_navagent3d_get_desired_velocity(void *agent);
/// @brief True if the agent currently has a valid path to its goal.
/// @param agent Opaque NavAgent3D handle.
/// @return 1 for an active path, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_has_path(void *agent);
/// @brief True while the current path segment traverses an off-mesh link.
/// @param agent Opaque NavAgent3D handle.
/// @return 1 when the active path segment matches an off-mesh link, otherwise 0.
int8_t rt_navagent3d_get_on_offmesh_link(void *agent);
/// @brief Authored kind of the link being traversed (empty when none).
/// @param agent Opaque NavAgent3D handle.
/// @return Retained runtime string containing the link kind, or the shared empty string.
rt_string rt_navagent3d_get_link_kind(void *agent);
/// @brief Distance in world units left along the path (0 when within stopping distance).
/// @details The result combines the live distance to the current corner with a
///          suffix-length cache built transactionally with the path, making the query O(1).
/// @param agent Opaque NavAgent3D handle.
/// @return Bounded non-negative remaining path length, zero without a path, or -1 for an invalid
///         handle.
double rt_navagent3d_get_remaining_distance(void *agent);
/// @brief Get the radius around the goal at which the agent stops issuing motion.
/// @param agent Opaque NavAgent3D handle.
/// @return Bounded non-negative stopping distance, or -1 for an invalid handle.
double rt_navagent3d_get_stopping_distance(void *agent);
/// @brief Set the stopping radius.
/// @param agent Opaque NavAgent3D handle.
/// @param distance Requested non-negative stopping distance in world units.
void rt_navagent3d_set_stopping_distance(void *agent, double distance);
/// @brief Get the desired walking speed in m/s.
/// @param agent Opaque NavAgent3D handle.
/// @return Bounded non-negative desired speed, or -1 for an invalid handle.
double rt_navagent3d_get_desired_speed(void *agent);
/// @brief Set the desired walking speed (clamped to >=0).
/// @param agent Opaque NavAgent3D handle.
/// @param speed Requested movement speed in world units per second.
void rt_navagent3d_set_desired_speed(void *agent, double speed);
/// @brief True if the agent periodically rebuilds its path while an unreached target is active.
/// @param agent Opaque NavAgent3D handle.
/// @return 1 when automatic repathing is enabled, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_auto_repath(void *agent);
/// @brief Toggle auto-repath behavior.
/// @param agent Opaque NavAgent3D handle.
/// @param enabled Non-zero to enable periodic path rebuilding.
void rt_navagent3d_set_auto_repath(void *agent, int8_t enabled);
/// @brief True if same-NavMesh local separation steering is enabled.
/// @param agent Opaque NavAgent3D handle.
/// @return 1 when local avoidance is enabled, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_avoidance_enabled(void *agent);
/// @brief Toggle same-NavMesh local separation steering.
/// @param agent Opaque NavAgent3D handle.
/// @param enabled Non-zero to enable reciprocal local avoidance.
void rt_navagent3d_set_avoidance_enabled(void *agent, int8_t enabled);
/// @brief Get the radius used for local avoidance separation.
/// @param agent Opaque NavAgent3D handle.
/// @return Bounded non-negative avoidance radius, or -1 for an invalid handle.
double rt_navagent3d_get_avoidance_radius(void *agent);
/// @brief Set the radius used for local avoidance separation (clamped to >=0).
/// @param agent Opaque NavAgent3D handle.
/// @param radius Requested non-negative avoidance radius; zero disables separation.
void rt_navagent3d_set_avoidance_radius(void *agent, double radius);

/// @brief Bind a Character3D — Update will call its Move() with desired velocity.
/// @details Rebinding synchronizes the authoritative position and immediately rebuilds
///          an active target path from that position.
/// @param agent Opaque NavAgent3D handle.
/// @param controller Optional CharacterController3D handle retained by the agent; null clears the
///                   binding.
void rt_navagent3d_bind_character(void *agent, void *controller);
/// @brief Bind a SceneNode3D — Update will write the agent's position into the node's transform.
/// @details World-to-local conversion is transactional for parented nodes; singular
///          parent transforms leave local state unchanged. Rebinding immediately rebuilds
///          an active target path from the synchronized world position.
/// @param agent Opaque NavAgent3D handle.
/// @param node Optional SceneNode3D handle retained by the agent; null clears the binding.
void rt_navagent3d_bind_node(void *agent, void *node);

/// @brief Test-only: verify the spatial-grid avoidance query matches a full registry scan for every
///   registered agent.
/// @return 1 if all agents agree or none are registered, otherwise 0.
int8_t rt_navagent3d_check_avoidance_grid_parity(void);

#ifdef __cplusplus
}
#endif
