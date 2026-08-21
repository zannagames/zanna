//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_iksolver3d.h
// Purpose: IKSolver3D runtime surface and controller-integration helpers.
// Key invariants:
//   - Solver targets and outputs use skeleton/model space.
//   - Two-bone and FABRIK chains are strict direct-parent paths.
// Ownership/Lifetime:
//   - Solvers are GC-managed, retain/freeze their Skeleton3D, and own private pose buffers.
//   - Input sequences, vectors, and controller pose arrays are borrowed for each call.
// Links: rt_iksolver3d.c, rt_skeleton3d_internal.h, docs/graphics3d-guide.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for skeletal inverse-kinematics constraints.
/// @details IKSolver3D supports strict parent-to-child two-bone and FABRIK
///          chains plus single-bone look-at constraints. Goals are expressed
///          in skeleton/model space, matching the pose-global matrices; scene
///          callers must convert world-space values before use. A solver
///          retains and freezes its Skeleton3D and owns private bind-pose solve
///          buffers. FABRIK chains contain 2 through 32 bones.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a two-bone IK solver for a parent -> mid -> end chain.
/// @details The three indexes must be in signed 32-bit range, exist in the
///          skeleton, and form two direct parent links. The solver starts at
///          full weight with the bind-pose end position as its target.
/// @param[in,out] skeleton Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] root Chain root bone index.
/// @param[in] mid Direct child of @p root.
/// @param[in] end Direct child of @p mid.
/// @return New GC-managed IKSolver3D, or `NULL` for invalid input/topology or
///         allocation failure.
void *rt_ik_solver3d_two_bone(void *skeleton, int64_t root, int64_t mid, int64_t end);
/// @brief Create a one-bone look-at/aim solver.
/// @details The solver rotates the selected bone's local forward axis toward
///          its model-space target, converting the desired global orientation
///          through any valid parent rotation, and starts at full weight.
/// @param[in,out] skeleton Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] bone Existing signed-32-bit-range bone index.
/// @return New GC-managed IKSolver3D, or `NULL` for invalid input/index or
///         allocation failure.
void *rt_ik_solver3d_look_at(void *skeleton, int64_t bone);
/// @brief Create a FABRIK solver from a Seq[Integer] bone chain.
/// @details The sequence must contain 2 through 32 boxed integer indexes that
///          exist and form a strict direct-parent path. The solver starts at
///          full weight with the bind-pose end position as its target. Wrong
///          sequence types, inconsistent sequence bounds, and non-integer
///          elements are rejected without invoking trapping collection accessors.
/// @param[in,out] skeleton Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] chain Borrowed runtime sequence of boxed bone indexes.
/// @return New GC-managed IKSolver3D, or `NULL` for invalid input, length,
///         element range, topology, or allocation failure.
void *rt_ik_solver3d_fabrik(void *skeleton, void *chain);
/// @brief Set the skeleton/model-space target for the solver.
/// @details Non-Vec3 inputs are ignored. Coordinates are sanitized to finite
///          values and clamped to `±1e12`. This does not solve immediately.
/// @param[in,out] solver IKSolver3D to configure.
/// @param[in] target Borrowed Vec3 goal relative to the skeleton root.
void rt_ik_solver3d_set_target(void *solver, void *target);
/// @brief Set solver weight, clamped to 0..1.
/// @details Zero is pass-through, one applies the full constraint, and
///          non-finite input becomes zero.
/// @param[in,out] solver IKSolver3D to configure.
/// @param[in] weight Requested solved-pose contribution.
void rt_ik_solver3d_set_weight(void *solver, double weight);
/// @brief Set a model-space pole target orienting a two-bone chain's mid joint.
/// @details Non-Vec3 inputs are ignored. The pole affects only a reachable
///          three-bone chain; coordinates are sanitized and clamped.
/// @param[in,out] solver IKSolver3D to configure.
/// @param[in] pole Borrowed Vec3 position relative to the skeleton root.
void rt_ik_solver3d_set_pole(void *solver, void *pole);
/// @brief Set a ground normal; after the position solve the chain's end (foot) bone is tilted by
///        the shortest arc from model +Y to it, on top of its animated rotation (ADR 0286).
///        Non-Vec3 normals are ignored.
/// @details A flat normal (model +Y) leaves the authored pose untouched; a
///          slope leans it with the surface, independent of the rig's bone-axis
///          convention. The direction is interpreted in skeleton/model space,
///          normalized, and replaced with `(0,1,0)` when degenerate. A target
///          rotation, when set, takes precedence over this hint.
/// @param[in,out] solver IKSolver3D to configure.
/// @param[in] normal Borrowed Vec3 surface normal.
void rt_ik_solver3d_set_ground_normal(void *solver, void *normal);
/// @brief Set a model-space orientation goal for the chain's end bone. Non-Quat inputs are
///        ignored.
/// @details Applied after the position solve by slerping the end bone toward
///          the goal by solver weight; takes precedence over a ground normal
///          (ADR 0286). Components are sanitized and the quaternion normalized
///          (identity when degenerate). Look-at solvers have no end segment and
///          ignore the goal.
/// @param[in,out] solver IKSolver3D to configure.
/// @param[in] rotation Borrowed Quat goal in skeleton/model space.
void rt_ik_solver3d_set_target_rotation(void *solver, void *rotation);
/// @brief Remove the end-bone orientation goal; positional solving is unaffected.
/// @param[in,out] solver IKSolver3D to configure.
void rt_ik_solver3d_clear_target_rotation(void *solver);
/// @brief Solve the IK constraint against the skeleton bind pose.
/// @details Reinitializes the solver-owned local matrices from bind pose and
///          refreshes its private model-space globals. This does not modify a
///          controller's pose.
/// @param[in,out] solver IKSolver3D whose cached standalone solution to refresh.
void rt_ik_solver3d_solve(void *solver);

/// @name Runtime integration
/// @{

/// @brief Return the Skeleton3D handle retained by this solver.
/// @param[in] solver IKSolver3D to inspect.
/// @return Borrowed validated Skeleton3D handle, or `NULL`; the caller must not
///         release it.
void *rt_ik_solver3d_get_skeleton(void *solver);
/// @brief Apply the solver in place to a controller local-pose buffer and refresh globals.
/// @details Both arrays must contain at least @p bone_count row-major
///          matrices. The count is clamped to the solver skeleton's safe bone
///          count and its private pose allocation. Solver kind, chain topology,
///          storage, and retained state are validated even for zero weight. A
///          negligible valid weight is a successful no-op; otherwise the function
///          rebuilds globals before and during constraint evaluation.
/// @param[in,out] solver IKSolver3D whose configured constraint to apply.
/// @param[in,out] locals Writable array of `bone_count * 16` local-matrix
///                       floats.
/// @param[out] globals Writable array of `bone_count * 16` model-space matrix
///                     floats.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @return `1` when applied or skipped for negligible weight; `0` for invalid
///         handles, buffers, counts, topology, or selected bones.
int8_t rt_ik_solver3d_apply_to_pose(void *solver,
                                    float *locals,
                                    float *globals,
                                    int32_t bone_count);

/// @}

#ifdef __cplusplus
}
#endif
