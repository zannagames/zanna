//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_iksolver3d.c
// Purpose: IKSolver3D — two-bone, look-at, and FABRIK inverse-kinematics pose
//   constraints applied over a Skeleton3D's local/global bone matrices.
//
// Key invariants:
//   - Chains have at most RT_IK_SOLVER3D_MAX_CHAIN (32) bones and must form a
//     strict parent -> child path (validated by ik3d_chain_is_parented).
//   - FABRIK runs at most RT_IK_SOLVER3D_FABRIK_ITERS (12) iterations or until
//     the end effector is within 1e-4 of the target.
//   - Bone transforms are row-major 4x4 float matrices stored 16-per-bone;
//     globals are rebuilt from locals after every positional edit.
//   - `weight` in [0,1] blends the solved pose against the input pose; the
//     solver retains its Skeleton3D and freezes it (skeleton->frozen = 1).
//
// Ownership/Lifetime:
//   - IKSolver3D is GC-managed; private allocation identities own its two heap
//     pose buffers while the legacy pointers are repaired mirrors. The solver
//     also owns a retained Skeleton3D, all released by the finalizer.
//
// Links: rt_iksolver3d.h, rt_skeleton3d_internal.h (bone/bind-pose layout)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements analytic two-bone, look-at, and FABRIK skeletal IK.
/// @details Solver goals and bone globals share skeleton/model space; callers
///          must transform scene-world goals into that space before setting
///          them. Each GC-managed solver retains and freezes one Skeleton3D,
///          stores a strict parent-to-child chain, and can either update a
///          controller-owned pose in place or solve from bind pose into its
///          private buffers.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_iksolver3d.h"

#include "rt_box.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_quat.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Maximum number of bones accepted in a FABRIK chain.
#define RT_IK_SOLVER3D_MAX_CHAIN 32
/// Maximum backward/forward passes used by a reachable FABRIK solve.
#define RT_IK_SOLVER3D_FABRIK_ITERS 12
/// Absolute clamp applied to model-space goal coordinates.
#define RT_IK_SOLVER3D_COORD_ABS_MAX 1.0e12f

/// @brief Which IK algorithm a solver runs (two-bone analytic, look-at, FABRIK).
typedef enum {
    /// Three-bone chain solved by the shared positional-chain path.
    RT_IK_SOLVER3D_TWO_BONE = 1,
    /// Single bone rotated so its forward axis faces the target.
    RT_IK_SOLVER3D_LOOK_AT = 2,
    /// Two-to-thirty-two-bone chain solved iteratively.
    RT_IK_SOLVER3D_FABRIK = 3,
} rt_ik_solver3d_kind;

/// @brief IKSolver3D state: the retained skeleton, the bone chain, the solve
///        target/pole/ground-normal goals, the blend weight, and the owned
///        local/global pose buffers the solve writes into.
typedef struct {
    /// Runtime object header / virtual table slot.
    void *vptr;
    /// Retained and frozen Skeleton3D defining bone topology and bind pose.
    rt_skeleton3d *skeleton;
    /// Algorithm selected at construction.
    rt_ik_solver3d_kind kind;
    /// Number of valid entries in @ref chain.
    int32_t chain_count;
    /// Strict parent-to-child bone-index path.
    int32_t chain[RT_IK_SOLVER3D_MAX_CHAIN];
    /// Model-space end-effector or look-at target.
    float target[3];
    /// Optional model-space pole position for a three-bone chain.
    float pole[3];
    /// Nonzero after a valid pole has been assigned.
    int8_t has_pole;
    /// Optional normalized model-space sole-up direction.
    float ground_normal[3];
    /// Nonzero after a valid ground normal has been assigned.
    int8_t has_ground_normal;
    /// Optional normalized model-space end-bone orientation goal (x, y, z, w).
    float target_rotation[4];
    /// Nonzero after a valid target rotation has been assigned.
    int8_t has_target_rotation;
    /// Solved-pose contribution in `[0,1]`.
    float weight;
    /// Owned bind-seeded local matrices used by standalone solve.
    float *solved_locals;
    /// Owned model-space matrices corresponding to @ref solved_locals.
    float *solved_globals;
    /// Immutable identity of the local-pose allocation.
    float *owned_solved_locals;
    /// Immutable identity of the global-pose allocation.
    float *owned_solved_globals;
    /// Number of bone matrices allocated in both owned pose buffers.
    int32_t pose_bone_capacity;
    /// Construction-time number of bones in the fixed inline chain.
    int32_t chain_bone_capacity;
} rt_ik_solver3d;

/// @brief Number of skeleton bones safe to read, or 0 when the handle is not a live Skeleton3D.
/// @param[in] skeleton Skeleton pointer to validate and inspect.
/// @return Sanitized readable bone count, or zero.
static int32_t ik3d_safe_bone_count(const rt_skeleton3d *skeleton) {
    if (!skeleton || !rt_g3d_has_class((void *)(uintptr_t)skeleton, RT_G3D_SKELETON3D_CLASS_ID))
        return 0;
    return skeleton3d_safe_bone_count(skeleton);
}

/// @brief Number of IK chains safe to read (clamped to RT_IK_SOLVER3D_MAX_CHAIN).
/// @note The value is a bone count for the solver's single chain, despite the
///       historical plural wording.
/// @param[in] solver Solver to inspect.
/// @return Safe readable chain-entry count, or zero.
static int32_t ik3d_safe_chain_count(const rt_ik_solver3d *solver) {
    if (!solver || solver->chain_count <= 0 || solver->chain_count > RT_IK_SOLVER3D_MAX_CHAIN)
        return 0;
    return solver->chain_count;
}

/// @brief Validate @p obj as an IKSolver3D handle and return its typed pointer (NULL on mismatch).
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed solver pointer, or `NULL` on class/liveness mismatch.
static rt_ik_solver3d *ik_solver3d_checked(void *obj) {
    return (rt_ik_solver3d *)rt_g3d_checked_or_null(obj, RT_G3D_IKSOLVER3D_CLASS_ID);
}

/// @brief Validate @p obj as a Skeleton3D handle and return its typed pointer (NULL on mismatch).
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed skeleton pointer, or `NULL` on class/liveness
///         mismatch.
static rt_skeleton3d *ik_solver3d_skeleton_checked(void *obj) {
    return (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
}

/// @brief Release a GC reference held in @p *slot if this is its last drop, then NULL it.
/// @param[in,out] slot Address of the retained object slot to release and clear.
static void ik_solver3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release the retained skeleton only if the slot still points at Skeleton3D.
/// @details A non-null wrong-class value is cleared as an unowned corrupt slot
///          rather than released through the Skeleton3D path.
/// @param[in,out] slot Address of the retained skeleton slot.
static void ik_solver3d_release_skeleton_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    ik_solver3d_release_ref(slot);
}

/// @brief Narrow a double to float, returning @p fallback for NaN/inf and saturating at ±FLT_MAX.
/// @param[in] value Double-precision value to narrow.
/// @param[in] fallback Replacement for non-finite input.
/// @return Finite narrowed or saturated float.
static float ik3d_finite_float(double value, float fallback) {
    if (!isfinite(value))
        return fallback;
    if (value > (double)FLT_MAX)
        return FLT_MAX;
    if (value < -(double)FLT_MAX)
        return -FLT_MAX;
    return (float)value;
}

/// @brief Clamp a model-space IK coordinate to a range that keeps vector math finite.
/// @param[in] value Coordinate to sanitize and narrow.
/// @param[in] fallback Replacement for non-finite input.
/// @return Finite float in
///         `[-RT_IK_SOLVER3D_COORD_ABS_MAX,RT_IK_SOLVER3D_COORD_ABS_MAX]`.
static float ik3d_finite_coord(double value, float fallback) {
    float out = ik3d_finite_float(value, fallback);
    if (out > RT_IK_SOLVER3D_COORD_ABS_MAX)
        return RT_IK_SOLVER3D_COORD_ABS_MAX;
    if (out < -RT_IK_SOLVER3D_COORD_ABS_MAX)
        return -RT_IK_SOLVER3D_COORD_ABS_MAX;
    return out;
}

/// @brief Sanitize one float lane already in solver storage.
/// @param[in] value Stored coordinate lane.
/// @param[in] fallback Replacement for non-finite input.
/// @return Finite, coordinate-range-clamped lane.
static float ik3d_sanitize_coord_lane(float value, float fallback) {
    return ik3d_finite_coord(value, fallback);
}

/// @brief Clamp a value to the [0, 1] float range, mapping NaN/inf to 0.
/// @param[in] value Requested blend weight.
/// @return Finite float in `[0,1]`.
static float ik3d_clamp01(double value) {
    if (!isfinite(value))
        return 0.0f;
    if (value <= 0.0)
        return 0.0f;
    if (value >= 1.0)
        return 1.0f;
    return (float)value;
}

/// @brief Dot product of two 3-vectors.
/// @param[in] a Borrowed three-float left operand.
/// @param[in] b Borrowed three-float right operand.
/// @return Finite dot product, or zero for invalid input/overflow.
static float ik3d_dot3(const float *a, const float *b) {
    double dot;
    if (!a || !b)
        return 0.0f;
    dot = (double)a[0] * (double)b[0] + (double)a[1] * (double)b[1] + (double)a[2] * (double)b[2];
    return ik3d_finite_float(dot, 0.0f);
}

/// @brief Cross product out = a x b for 3-vectors.
/// @param[in] a Borrowed three-float left operand.
/// @param[in] b Borrowed three-float right operand.
/// @param[out] out Writable three-float cross product with sanitized lanes.
static void ik3d_cross3(const float *a, const float *b, float *out) {
    double cross[3];
    double max_abs;
    double scale = 1.0;
    if (!out)
        return;
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    if (!a || !b)
        return;
    cross[0] = (double)a[1] * (double)b[2] - (double)a[2] * (double)b[1];
    cross[1] = (double)a[2] * (double)b[0] - (double)a[0] * (double)b[2];
    cross[2] = (double)a[0] * (double)b[1] - (double)a[1] * (double)b[0];
    max_abs = fmax(fabs(cross[0]), fmax(fabs(cross[1]), fabs(cross[2])));
    if (!isfinite(max_abs))
        return;
    if (max_abs > RT_IK_SOLVER3D_COORD_ABS_MAX)
        scale = RT_IK_SOLVER3D_COORD_ABS_MAX / max_abs;
    out[0] = ik3d_finite_float(cross[0] * scale, 0.0f);
    out[1] = ik3d_finite_float(cross[1] * scale, 0.0f);
    out[2] = ik3d_finite_float(cross[2] * scale, 0.0f);
}

/// @brief Euclidean length of a 3-vector (0 if the result is non-finite).
/// @param[in] v Borrowed three-float vector.
/// @return Finite Euclidean length, or zero.
static float ik3d_len3(const float *v) {
    double x;
    double y;
    double z;
    double max_abs;
    double len;
    if (!v)
        return 0.0f;
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0.0f;
    x = fabs((double)v[0]);
    y = fabs((double)v[1]);
    z = fabs((double)v[2]);
    max_abs = fmax(x, fmax(y, z));
    if (!(max_abs > 0.0) || !isfinite(max_abs))
        return 0.0f;
    len = max_abs * hypot(hypot(x / max_abs, y / max_abs), z / max_abs);
    return ik3d_finite_float(len, 0.0f);
}

/// @brief Normalize a 3-vector in place; returns 0 (leaving it unchanged) if degenerate (len <=
/// 1e-6).
/// @param[in,out] v Writable three-float vector.
/// @return `1` after normalization, or `0` when degenerate.
static int ik3d_normalize3(float *v) {
    double max_abs;
    double scaled_len;
    double len;
    if (!v || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    max_abs = fmax(fabs((double)v[0]), fmax(fabs((double)v[1]), fabs((double)v[2])));
    if (!(max_abs > 0.0))
        return 0;
    scaled_len =
        hypot(hypot((double)v[0] / max_abs, (double)v[1] / max_abs), (double)v[2] / max_abs);
    len = max_abs * scaled_len;
    if (!isfinite(scaled_len) || !isfinite(len) || len <= 1e-6)
        return 0;
    v[0] = (float)(((double)v[0] / max_abs) / scaled_len);
    v[1] = (float)(((double)v[1] / max_abs) / scaled_len);
    v[2] = (float)(((double)v[2] / max_abs) / scaled_len);
    return 1;
}

/// @brief Distance between two 3-space points.
/// @param[in] a Borrowed first three-float point.
/// @param[in] b Borrowed second three-float point.
/// @return Finite Euclidean distance, or zero for invalid input.
static float ik3d_distance3(const float *a, const float *b) {
    double x;
    double y;
    double z;
    double max_abs;
    double len;
    if (!a || !b)
        return 0.0f;
    if (!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(a[2]) || !isfinite(b[0]) ||
        !isfinite(b[1]) || !isfinite(b[2]))
        return 0.0f;
    x = fabs((double)a[0] - (double)b[0]);
    y = fabs((double)a[1] - (double)b[1]);
    z = fabs((double)a[2] - (double)b[2]);
    max_abs = fmax(x, fmax(y, z));
    if (!(max_abs > 0.0))
        return 0.0f;
    len = max_abs * hypot(hypot(x / max_abs, y / max_abs), z / max_abs);
    return ik3d_finite_float(len, 0.0f);
}

/// @brief Accumulate each bone's global matrix as parent_global * local.
/// @details Uses the shared skeleton builder so rigs with non-topological bone order still
///          evaluate deterministically; parent cycles are broken as roots.
/// @param[in] skeleton Skeleton providing parent topology.
/// @param[in] locals Borrowed `bone_count * 16` local-matrix floats.
/// @param[out] globals Writable `bone_count * 16` model-space matrix floats.
/// @param[in] bone_count Requested prefix, clamped to the skeleton's safe count.
static void ik3d_build_globals(const rt_skeleton3d *skeleton,
                               const float *locals,
                               float *globals,
                               int32_t bone_count) {
    if (!skeleton || !locals || !globals)
        return;
    if (bone_count > ik3d_safe_bone_count(skeleton))
        bone_count = ik3d_safe_bone_count(skeleton);
    skeleton3d_compute_globals_from_locals(skeleton, locals, globals, bone_count);
}

/// @brief Read a bone's model-space position from its global matrix.
/// @param[in] globals Borrowed flat row-major global-matrix array.
/// @param[in] bone Valid zero-based bone index.
/// @param[out] out Writable sanitized three-float position.
static void ik3d_global_position(const float *globals, int32_t bone, float *out) {
    const float *m = &globals[bone * 16];
    out[0] = ik3d_sanitize_coord_lane(m[3], 0.0f);
    out[1] = ik3d_sanitize_coord_lane(m[7], 0.0f);
    out[2] = ik3d_sanitize_coord_lane(m[11], 0.0f);
}

/// @brief Express a model-space point in @p bone's parent-local frame.
/// @details Projects the model-space delta onto each parent axis and divides by that axis'
///          squared length, so the result is correct even when the parent matrix carries
///          non-unit scale. Degenerate (near-zero) axes yield a 0 component. Roots with no
///          parent pass the model-space point through unchanged.
/// @param[in] skeleton Skeleton providing the indexed bone's parent.
/// @param[in] globals Borrowed flat model-space global-matrix array.
/// @param[in] bone Valid zero-based bone index.
/// @param[in] world Borrowed model-space point; retained parameter name is
///                  historical.
/// @param[out] out_local Writable three-float parent-local point.
static void ik3d_parent_local_point(const rt_skeleton3d *skeleton,
                                    const float *globals,
                                    int32_t bone,
                                    const float *world,
                                    float *out_local) {
    int32_t parent = skeleton->bones[bone].parent_index;
    if (parent < 0 || parent >= ik3d_safe_bone_count(skeleton) || parent == bone) {
        memcpy(out_local, world, 3 * sizeof(float));
        return;
    }
    const float *p = &globals[parent * 16];
    float d[3] = {world[0] - p[3], world[1] - p[7], world[2] - p[11]};
    float axis_x[3] = {p[0], p[4], p[8]};
    float axis_y[3] = {p[1], p[5], p[9]};
    float axis_z[3] = {p[2], p[6], p[10]};
    float xx = ik3d_dot3(axis_x, axis_x);
    float yy = ik3d_dot3(axis_y, axis_y);
    float zz = ik3d_dot3(axis_z, axis_z);
    out_local[0] = xx > 1e-8f ? ik3d_dot3(d, axis_x) / xx : 0.0f;
    out_local[1] = yy > 1e-8f ? ik3d_dot3(d, axis_y) / yy : 0.0f;
    out_local[2] = zz > 1e-8f ? ik3d_dot3(d, axis_z) / zz : 0.0f;
    out_local[0] = ik3d_sanitize_coord_lane(out_local[0], 0.0f);
    out_local[1] = ik3d_sanitize_coord_lane(out_local[1], 0.0f);
    out_local[2] = ik3d_sanitize_coord_lane(out_local[2], 0.0f);
}

/// @brief Move @p bone to the given model-space position by rewriting its local translation.
/// @details Converts @p world into the bone's parent-local frame, writes it into the
///          bone's local matrix, then rebuilds globals so downstream bones see the move.
/// @param[in] solver Solver providing the skeleton hierarchy.
/// @param[in,out] locals Writable flat local-matrix array.
/// @param[out] globals Writable flat model-space matrix array rebuilt in place.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @param[in] bone Bone index whose translation to replace.
/// @param[in] world Borrowed target model-space point; parameter name is
///                  historical.
static void ik3d_set_global_position(rt_ik_solver3d *solver,
                                     float *locals,
                                     float *globals,
                                     int32_t bone_count,
                                     int32_t bone,
                                     const float *world) {
    float local[3];
    if (!solver || !locals || !globals || bone < 0 || bone >= bone_count)
        return;
    ik3d_parent_local_point(solver->skeleton, globals, bone, world, local);
    locals[bone * 16 + 3] = local[0];
    locals[bone * 16 + 7] = local[1];
    locals[bone * 16 + 11] = local[2];
    /* The full-globals rebuild here is required, not redundant: each FABRIK chain bone derives
     * its local translation (above, via ik3d_parent_local_point) from its parent's *current*
     * global, so the parent's update from the previous chain step must already be propagated.
     * Deferring this to a single rebuild after the whole chain would feed stale parent globals
     * and corrupt the result. Cost is bounded — the chain is capped at RT_IK_SOLVER3D_MAX_CHAIN
     * (32) — so this is O(chain * bone_count) per solve, not a scaling concern. A correctness-
     * preserving speedup would rebuild only the affected subtree, but the bounded chain makes it
     * unnecessary. */
    ik3d_build_globals(solver->skeleton, locals, globals, bone_count);
}

/// @brief Write the identity quaternion (0, 0, 0, 1) into @p out.
/// @param[out] out Writable four-float quaternion.
static void ik3d_quat_identity(float *out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
}

/// @brief Normalize a quaternion in place, falling back to identity if degenerate.
/// @param[in,out] q Writable four-float quaternion; `NULL` is ignored.
static void ik3d_quat_normalize(float *q) {
    double max_abs;
    double scaled_len;
    if (!q)
        return;
    max_abs = fmax(fabs((double)q[0]),
                   fmax(fabs((double)q[1]), fmax(fabs((double)q[2]), fabs((double)q[3]))));
    if (!isfinite(max_abs) || max_abs <= 1e-8) {
        ik3d_quat_identity(q);
        return;
    }
    scaled_len = hypot(hypot((double)q[0] / max_abs, (double)q[1] / max_abs),
                       hypot((double)q[2] / max_abs, (double)q[3] / max_abs));
    if (!isfinite(scaled_len) || scaled_len <= 1e-8) {
        ik3d_quat_identity(q);
        return;
    }
    for (int i = 0; i < 4; i++)
        q[i] = (float)(((double)q[i] / max_abs) / scaled_len);
}

/// @brief Convert a 3x3 rotation (given row by row) into a quaternion via Shepperd's method.
/// @details Selects the branch with the largest pivot (trace, or the dominant diagonal
///          term) to keep the divisor well away from zero, avoiding the catastrophic
///          cancellation a naive trace-only formula suffers when the trace is small or
///          negative. The result is normalized before returning.
/// @param[in] r00 Row-zero, column-zero basis lane.
/// @param[in] r01 Row-zero, column-one basis lane.
/// @param[in] r02 Row-zero, column-two basis lane.
/// @param[in] r10 Row-one, column-zero basis lane.
/// @param[in] r11 Row-one, column-one basis lane.
/// @param[in] r12 Row-one, column-two basis lane.
/// @param[in] r20 Row-two, column-zero basis lane.
/// @param[in] r21 Row-two, column-one basis lane.
/// @param[in] r22 Row-two, column-two basis lane.
/// @param[out] out Writable normalized quaternion in `(x,y,z,w)` order.
static void ik3d_quat_from_matrix_rows(float r00,
                                       float r01,
                                       float r02,
                                       float r10,
                                       float r11,
                                       float r12,
                                       float r20,
                                       float r21,
                                       float r22,
                                       float *out) {
    float tr = r00 + r11 + r22;
    if (!out) {
        return;
    }
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        if (!isfinite(s) || s <= 1e-8f) {
            ik3d_quat_identity(out);
            return;
        }
        out[3] = 0.25f * s;
        out[0] = (r21 - r12) / s;
        out[1] = (r02 - r20) / s;
        out[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        if (!isfinite(s) || s <= 1e-8f) {
            ik3d_quat_identity(out);
            return;
        }
        out[3] = (r21 - r12) / s;
        out[0] = 0.25f * s;
        out[1] = (r01 + r10) / s;
        out[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        if (!isfinite(s) || s <= 1e-8f) {
            ik3d_quat_identity(out);
            return;
        }
        out[3] = (r02 - r20) / s;
        out[0] = (r01 + r10) / s;
        out[1] = 0.25f * s;
        out[2] = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        if (!isfinite(s) || s <= 1e-8f) {
            ik3d_quat_identity(out);
            return;
        }
        out[3] = (r10 - r01) / s;
        out[0] = (r02 + r20) / s;
        out[1] = (r12 + r21) / s;
        out[2] = 0.25f * s;
    }
    ik3d_quat_normalize(out);
}

/// @brief Spherical linear interpolation between quaternions @p a and @p b by @p t.
/// @details Negates @p b when the dot is negative so interpolation takes the short arc.
///          For nearly-parallel inputs (dot > 0.9995) it falls back to normalized lerp to
///          avoid the division-by-near-zero in the sin(theta) denominator.
/// @param[in] a Borrowed four-float unit quaternion.
/// @param[in] b Borrowed four-float unit quaternion.
/// @param[in] t Interpolation weight clamped to `[0,1]`; non-finite becomes
///              zero.
/// @param[out] out Writable normalized four-float result.
static void ik3d_quat_slerp(const float *a, const float *b, float t, float *out) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float nb[4] = {b[0], b[1], b[2], b[3]};
    if (!isfinite(t) || t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    if (dot < 0.0f) {
        dot = -dot;
        nb[0] = -nb[0];
        nb[1] = -nb[1];
        nb[2] = -nb[2];
        nb[3] = -nb[3];
    }
    if (!isfinite(dot)) {
        ik3d_quat_identity(out);
        return;
    }
    if (dot > 1.0f)
        dot = 1.0f;
    if (dot < -1.0f)
        dot = -1.0f;
    if (dot > 0.9995f) {
        for (int i = 0; i < 4; i++)
            out[i] = a[i] + t * (nb[i] - a[i]);
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        if (!isfinite(theta) || !isfinite(sin_theta) || fabsf(sin_theta) <= 1e-6f) {
            for (int i = 0; i < 4; i++)
                out[i] = a[i] + t * (nb[i] - a[i]);
        } else {
            float wa = sinf((1.0f - t) * theta) / sin_theta;
            float wb = sinf(t * theta) / sin_theta;
            if (!isfinite(wa) || !isfinite(wb)) {
                for (int i = 0; i < 4; i++)
                    out[i] = a[i] + t * (nb[i] - a[i]);
            } else {
                for (int i = 0; i < 4; i++)
                    out[i] = wa * a[i] + wb * nb[i];
            }
        }
    }
    ik3d_quat_normalize(out);
}

/// @brief Quaternion conjugate (negated vector part) — the inverse for a unit quaternion.
/// @param[in] q Borrowed four-float quaternion.
/// @param[out] out Writable conjugate; may alias @p q.
static void ik3d_quat_conjugate(const float *q, float *out) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Hamilton product out = a * b (apply b then a), for (x,y,z,w) quaternions.
/// @param[in] a Borrowed left quaternion operand.
/// @param[in] b Borrowed right quaternion operand.
/// @param[out] out Writable normalized result; must not alias either input.
static void ik3d_quat_mul(const float *a, const float *b, float *out) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    ik3d_quat_normalize(out);
}

/// @brief Shortest-arc quaternion rotating unit vector @p from onto unit vector @p to.
/// @details Pure swing — the result has no twist component about @p from, which is
///          exactly what chain-bone aiming needs (twist authored in the animation
///          pose is preserved). Antiparallel inputs rotate 180° about a stable
///          perpendicular axis.
/// @param[in] from Borrowed normalized source direction.
/// @param[in] to Borrowed normalized destination direction.
/// @param[out] out Writable shortest-arc quaternion.
static void ik3d_quat_from_to(const float *from, const float *to, float *out) {
    float c[3];
    float d = ik3d_dot3(from, to);
    ik3d_cross3(from, to, c);
    if (d < -0.999999f) {
        float axis[3] = {1.0f, 0.0f, 0.0f};
        if (fabsf(from[0]) > 0.9f) {
            axis[0] = 0.0f;
            axis[1] = 1.0f;
        }
        ik3d_cross3(axis, from, c);
        if (!ik3d_normalize3(c)) {
            ik3d_quat_identity(out);
            return;
        }
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = 0.0f;
        return;
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = 1.0f + d;
    ik3d_quat_normalize(out);
}

/// @brief Decompose a row-major 4x4 matrix into translation, rotation quaternion, and scale.
/// @details Scale is the length of each basis column; columns are divided out before
///          extracting the rotation so shear-free TRS matrices round-trip. Non-finite or
///          near-zero scales default to 1 to keep the rotation extraction well-defined.
/// @param[in] m Borrowed row-major matrix of 16 floats.
/// @param[out] out_pos Writable sanitized three-float translation.
/// @param[out] out_rot Writable normalized four-float quaternion.
/// @param[out] out_scl Writable finite three-float scale.
static void ik3d_decompose_trs(const float *m, float *out_pos, float *out_rot, float *out_scl) {
    double sx_sq =
        (double)m[0] * (double)m[0] + (double)m[4] * (double)m[4] + (double)m[8] * (double)m[8];
    double sy_sq =
        (double)m[1] * (double)m[1] + (double)m[5] * (double)m[5] + (double)m[9] * (double)m[9];
    double sz_sq =
        (double)m[2] * (double)m[2] + (double)m[6] * (double)m[6] + (double)m[10] * (double)m[10];
    double sx = sqrt(sx_sq);
    double sy = sqrt(sy_sq);
    double sz = sqrt(sz_sq);
    int x_ok = isfinite(sx) && sx > 1e-6 && sx <= (double)FLT_MAX && isfinite(m[0]) &&
               isfinite(m[4]) && isfinite(m[8]);
    int y_ok = isfinite(sy) && sy > 1e-6 && sy <= (double)FLT_MAX && isfinite(m[1]) &&
               isfinite(m[5]) && isfinite(m[9]);
    int z_ok = isfinite(sz) && sz > 1e-6 && sz <= (double)FLT_MAX && isfinite(m[2]) &&
               isfinite(m[6]) && isfinite(m[10]);
    out_pos[0] = isfinite(m[3]) ? m[3] : 0.0f;
    out_pos[1] = isfinite(m[7]) ? m[7] : 0.0f;
    out_pos[2] = isfinite(m[11]) ? m[11] : 0.0f;
    out_scl[0] = x_ok ? (float)sx : 1.0f;
    out_scl[1] = y_ok ? (float)sy : 1.0f;
    out_scl[2] = z_ok ? (float)sz : 1.0f;
    ik3d_quat_from_matrix_rows(x_ok ? m[0] / out_scl[0] : 1.0f,
                               y_ok ? m[1] / out_scl[1] : 0.0f,
                               z_ok ? m[2] / out_scl[2] : 0.0f,
                               x_ok ? m[4] / out_scl[0] : 0.0f,
                               y_ok ? m[5] / out_scl[1] : 1.0f,
                               z_ok ? m[6] / out_scl[2] : 0.0f,
                               x_ok ? m[8] / out_scl[0] : 0.0f,
                               y_ok ? m[9] / out_scl[1] : 0.0f,
                               z_ok ? m[10] / out_scl[2] : 1.0f,
                               out_rot);
}

/// @brief Compose translation, a (unit) rotation quaternion, and scale into a row-major 4x4 matrix.
/// @details Sanitizes coordinates and scale lanes and normalizes a local copy
///          of the quaternion before expansion.
/// @param[in] pos Borrowed three-float translation.
/// @param[in] quat Borrowed four-float quaternion.
/// @param[in] scl Borrowed three-float scale.
/// @param[out] out Writable row-major matrix of 16 floats.
static void ik3d_build_trs(const float *pos, const float *quat, const float *scl, float *out) {
    float clean_pos[3];
    float clean_quat[4];
    float clean_scl[3];
    if (!pos || !quat || !scl || !out)
        return;
    for (int i = 0; i < 3; ++i) {
        clean_pos[i] = ik3d_sanitize_coord_lane(pos[i], 0.0f);
        clean_scl[i] = ik3d_finite_float(scl[i], 1.0f);
    }
    memcpy(clean_quat, quat, sizeof(clean_quat));
    ik3d_quat_normalize(clean_quat);
    float x = clean_quat[0], y = clean_quat[1], z = clean_quat[2], w = clean_quat[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    out[0] = ik3d_finite_float((double)(1.0f - (yy + zz)) * clean_scl[0], 1.0f);
    out[1] = ik3d_finite_float((double)(xy - wz) * clean_scl[1], 0.0f);
    out[2] = ik3d_finite_float((double)(xz + wy) * clean_scl[2], 0.0f);
    out[3] = clean_pos[0];
    out[4] = ik3d_finite_float((double)(xy + wz) * clean_scl[0], 0.0f);
    out[5] = ik3d_finite_float((double)(1.0f - (xx + zz)) * clean_scl[1], 1.0f);
    out[6] = ik3d_finite_float((double)(yz - wx) * clean_scl[2], 0.0f);
    out[7] = clean_pos[1];
    out[8] = ik3d_finite_float((double)(xz - wy) * clean_scl[0], 0.0f);
    out[9] = ik3d_finite_float((double)(yz + wx) * clean_scl[1], 0.0f);
    out[10] = ik3d_finite_float((double)(1.0f - (xx + yy)) * clean_scl[2], 1.0f);
    out[11] = clean_pos[2];
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

/// @brief Validate that @p chain is a strict parent -> child path of in-range bone indices.
/// @details Each entry must be a valid bone, and every entry after the first must have the
///          previous entry as its direct parent — the precondition every solver relies on.
/// @param[in] skeleton Skeleton providing bounds and parent indexes.
/// @param[in] chain Borrowed array of @p count bone indexes.
/// @param[in] count Number of indexes to validate.
/// @return `1` for a nonempty in-range direct-parent path; otherwise `0`.
static int ik3d_chain_is_parented(const rt_skeleton3d *skeleton,
                                  const int32_t *chain,
                                  int32_t count) {
    int32_t bone_count = ik3d_safe_bone_count(skeleton);
    if (!skeleton || !chain || count <= 0)
        return 0;
    for (int32_t i = 0; i < count; i++) {
        if (chain[i] < 0 || chain[i] >= bone_count)
            return 0;
        if (i > 0 && skeleton->bones[chain[i]].parent_index != chain[i - 1])
            return 0;
    }
    return 1;
}

/// @brief Restore private allocation mirrors and canonicalize retained solver state.
/// @details The two public-layout pose pointers and the skeleton's mutable bone
///          count are never accepted as allocation authority. Algorithm kinds
///          must retain their construction-time chain cardinality.
/// @param[in,out] solver Solver state to validate and repair.
/// @return Number of bone matrices safe in both owned buffers, or zero when
///         the solver cannot be used.
static int32_t ik_solver3d_repair_state(rt_ik_solver3d *solver) {
    int32_t bone_count;
    int32_t expected_chain_count;
    if (!solver)
        return 0;
    if (!solver->skeleton || !rt_g3d_has_class(solver->skeleton, RT_G3D_SKELETON3D_CLASS_ID)) {
        if (solver->skeleton)
            rt_g3d_ref_slot_clear_unowned((void **)&solver->skeleton);
        return 0;
    }
    if (!solver->owned_solved_locals || !solver->owned_solved_globals ||
        solver->pose_bone_capacity <= 0)
        return 0;
    solver->solved_locals = solver->owned_solved_locals;
    solver->solved_globals = solver->owned_solved_globals;
    bone_count = ik3d_safe_bone_count(solver->skeleton);
    if (bone_count > solver->pose_bone_capacity)
        bone_count = solver->pose_bone_capacity;
    if (bone_count <= 0)
        return 0;

    if (solver->chain_bone_capacity <= 0 || solver->chain_bone_capacity > RT_IK_SOLVER3D_MAX_CHAIN)
        return 0;
    switch (solver->kind) {
        case RT_IK_SOLVER3D_TWO_BONE:
            expected_chain_count = 3;
            break;
        case RT_IK_SOLVER3D_LOOK_AT:
            expected_chain_count = 1;
            break;
        case RT_IK_SOLVER3D_FABRIK:
            expected_chain_count =
                solver->chain_bone_capacity >= 2 ? solver->chain_bone_capacity : 0;
            break;
        default:
            return 0;
    }
    if (expected_chain_count <= 0 || expected_chain_count > RT_IK_SOLVER3D_MAX_CHAIN ||
        solver->chain_count != expected_chain_count ||
        !ik3d_chain_is_parented(solver->skeleton, solver->chain, solver->chain_count))
        return 0;

    for (int lane = 0; lane < 3; ++lane) {
        solver->target[lane] = ik3d_sanitize_coord_lane(solver->target[lane], 0.0f);
        solver->pole[lane] = ik3d_sanitize_coord_lane(solver->pole[lane], 0.0f);
    }
    solver->has_pole = solver->has_pole ? 1 : 0;
    solver->has_ground_normal = solver->has_ground_normal ? 1 : 0;
    solver->has_target_rotation = solver->has_target_rotation ? 1 : 0;
    if (solver->has_target_rotation)
        ik3d_quat_normalize(solver->target_rotation);
    solver->weight = ik3d_clamp01(solver->weight);
    if (solver->has_ground_normal) {
        for (int lane = 0; lane < 3; ++lane)
            solver->ground_normal[lane] =
                ik3d_sanitize_coord_lane(solver->ground_normal[lane], lane == 1 ? 1.0f : 0.0f);
        if (!ik3d_normalize3(solver->ground_normal)) {
            solver->ground_normal[0] = 0.0f;
            solver->ground_normal[1] = 1.0f;
            solver->ground_normal[2] = 0.0f;
        }
    }
    return bone_count;
}

/// @brief GC finalizer: release the retained skeleton and free both pose buffers.
/// @param[in,out] obj IKSolver3D storage being finalized; `NULL` is ignored.
static void ik_solver3d_finalize(void *obj) {
    rt_ik_solver3d *solver = (rt_ik_solver3d *)obj;
    if (!solver)
        return;
    ik_solver3d_release_skeleton_ref((void **)&solver->skeleton);
    free(solver->owned_solved_locals);
    free(solver->owned_solved_globals);
    solver->solved_locals = NULL;
    solver->solved_globals = NULL;
    solver->owned_solved_locals = NULL;
    solver->owned_solved_globals = NULL;
    solver->pose_bone_capacity = 0;
    solver->chain_bone_capacity = 0;
}

/// @brief Shared constructor for all solver kinds.
/// @details Validates the chain, retains and freezes the skeleton, allocates the
///          local/global pose buffers seeded from the bind pose, captures the end
///          effector's bind position as the initial target, then runs one solve.
/// @param[in,out] skeleton Valid Skeleton3D to retain and mark frozen.
/// @param[in] kind Algorithm assigned to the new solver.
/// @param[in] chain Borrowed strict parent-to-child bone-index path.
/// @param[in] chain_count Number of indexes in @p chain.
/// @return Opaque IKSolver3D handle, or NULL when validation or allocation fails.
static void *ik_solver3d_new(rt_skeleton3d *skeleton,
                             rt_ik_solver3d_kind kind,
                             const int32_t *chain,
                             int32_t chain_count) {
    int32_t bone_count;
    if (!skeleton || !chain || chain_count <= 0 || chain_count > RT_IK_SOLVER3D_MAX_CHAIN)
        return NULL;
    bone_count = ik3d_safe_bone_count(skeleton);
    if (bone_count <= 0)
        return NULL;
    if (!ik3d_chain_is_parented(skeleton, chain, chain_count))
        return NULL;
    rt_ik_solver3d *solver =
        (rt_ik_solver3d *)rt_obj_new_i64(RT_G3D_IKSOLVER3D_CLASS_ID, (int64_t)sizeof(*solver));
    if (!solver) {
        rt_trap("IKSolver3D.New: allocation failed");
        return NULL;
    }
    memset(solver, 0, sizeof(*solver));
    solver->kind = kind;
    solver->chain_count = chain_count;
    solver->chain_bone_capacity = chain_count;
    memcpy(solver->chain, chain, (size_t)chain_count * sizeof(int32_t));
    solver->target[0] = 0.0f;
    solver->target[1] = 0.0f;
    solver->target[2] = 0.0f;
    solver->weight = 1.0f;
    if (bone_count > 0) {
        size_t bytes = (size_t)bone_count * 16u * sizeof(float);
        solver->owned_solved_locals = (float *)calloc(1, bytes);
        solver->owned_solved_globals = (float *)calloc(1, bytes);
        if (!solver->owned_solved_locals || !solver->owned_solved_globals) {
            free(solver->owned_solved_locals);
            free(solver->owned_solved_globals);
            solver->owned_solved_locals = NULL;
            solver->owned_solved_globals = NULL;
            rt_obj_free(solver);
            rt_trap("IKSolver3D.New: pose-buffer allocation failed");
            return NULL;
        }
        solver->solved_locals = solver->owned_solved_locals;
        solver->solved_globals = solver->owned_solved_globals;
        solver->pose_bone_capacity = bone_count;
        for (int32_t bone = 0; bone < bone_count; bone++) {
            memcpy(&solver->solved_locals[bone * 16],
                   skeleton->bones[bone].bind_pose_local,
                   16 * sizeof(float));
        }
        ik3d_build_globals(skeleton, solver->solved_locals, solver->solved_globals, bone_count);
        ik3d_global_position(
            solver->solved_globals, solver->chain[solver->chain_count - 1], solver->target);
    }
    rt_obj_retain_maybe(skeleton);
    solver->skeleton = skeleton;
    skeleton->frozen = 1;
    rt_obj_set_finalizer(solver, ik_solver3d_finalize);
    rt_ik_solver3d_solve(solver);
    return solver;
}

/// @brief Create a two-bone IK solver over a root -> mid -> end bone chain (NULL on bad input).
/// @param[in] skeleton_obj Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] root Root bone index.
/// @param[in] mid Direct child of @p root.
/// @param[in] end Direct child of @p mid.
/// @return New GC-managed solver with full weight and the bind-pose end
///         position as target, or `NULL` for invalid indexes/topology/input or
///         allocation failure.
void *rt_ik_solver3d_two_bone(void *skeleton_obj, int64_t root, int64_t mid, int64_t end) {
    rt_skeleton3d *skeleton = ik_solver3d_skeleton_checked(skeleton_obj);
    int32_t chain[3];
    if (!skeleton)
        return NULL;
    if (root < INT32_MIN || root > INT32_MAX || mid < INT32_MIN || mid > INT32_MAX ||
        end < INT32_MIN || end > INT32_MAX)
        return NULL;
    chain[0] = (int32_t)root;
    chain[1] = (int32_t)mid;
    chain[2] = (int32_t)end;
    return ik_solver3d_new(skeleton, RT_IK_SOLVER3D_TWO_BONE, chain, 3);
}

/// @brief Create a single-bone look-at/aim solver (NULL on bad input).
/// @param[in] skeleton_obj Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] bone Valid bone index whose local rotation to aim.
/// @return New GC-managed look-at solver, or `NULL` for invalid input/index or
///         allocation failure.
void *rt_ik_solver3d_look_at(void *skeleton_obj, int64_t bone) {
    rt_skeleton3d *skeleton = ik_solver3d_skeleton_checked(skeleton_obj);
    int32_t chain[1];
    if (!skeleton || bone < INT32_MIN || bone > INT32_MAX)
        return NULL;
    chain[0] = (int32_t)bone;
    return ik_solver3d_new(skeleton, RT_IK_SOLVER3D_LOOK_AT, chain, 1);
}

/// @brief Create a FABRIK solver from a Seq[Integer] bone chain (2..32 bones; NULL otherwise).
/// @param[in] skeleton_obj Borrowed Skeleton3D handle to retain and freeze.
/// @param[in] chain_obj Borrowed runtime sequence of boxed signed bone indexes
///                      forming a direct-parent path.
/// @return New GC-managed FABRIK solver, or `NULL` for an invalid sequence,
///         length, index, topology, or allocation failure.
void *rt_ik_solver3d_fabrik(void *skeleton_obj, void *chain_obj) {
    rt_skeleton3d *skeleton = ik_solver3d_skeleton_checked(skeleton_obj);
    int64_t len;
    int64_t capacity;
    int32_t chain[RT_IK_SOLVER3D_MAX_CHAIN];
    if (!skeleton || !rt_obj_is_instance(chain_obj, RT_SEQ_CLASS_ID, 0))
        return NULL;
    len = rt_seq_len(chain_obj);
    capacity = rt_seq_cap(chain_obj);
    if (len < 2 || len > RT_IK_SOLVER3D_MAX_CHAIN || capacity < len)
        return NULL;
    for (int64_t i = 0; i < len; i++) {
        int64_t value;
        if (!rt_box_try_to_i64(rt_seq_get(chain_obj, i), &value) || value < INT32_MIN ||
            value > INT32_MAX)
            return NULL;
        chain[i] = (int32_t)value;
    }
    return ik_solver3d_new(skeleton, RT_IK_SOLVER3D_FABRIK, chain, (int32_t)len);
}

/// @brief Set the model-space target the chain reaches toward (non-Vec3 ignored).
/// @details Coordinates are sanitized and clamped to the solver safety range.
///          This changes configuration only; solving occurs during
///          @ref rt_ik_solver3d_solve or pose application.
/// @param[in,out] obj IKSolver3D to configure.
/// @param[in] target Borrowed Vec3 goal expressed relative to the skeleton
///                   root.
void rt_ik_solver3d_set_target(void *obj, void *target) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver || !rt_g3d_is_vec3(target))
        return;
    solver->target[0] = ik3d_finite_coord(rt_vec3_x(target), 0.0f);
    solver->target[1] = ik3d_finite_coord(rt_vec3_y(target), 0.0f);
    solver->target[2] = ik3d_finite_coord(rt_vec3_z(target), 0.0f);
}

/// @brief Set the solve blend weight, clamped to [0, 1] (0 = pass-through, 1 = full IK).
/// @param[in,out] obj IKSolver3D to configure.
/// @param[in] weight Requested contribution; non-finite input becomes zero.
void rt_ik_solver3d_set_weight(void *obj, double weight) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (solver)
        solver->weight = ik3d_clamp01(weight);
}

/// @brief Set a model-space pole target that swings a two-bone chain's mid joint (non-Vec3
/// ignored).
/// @details The stored pole participates only for a three-bone chain whose
///          target is reachable. Coordinates are sanitized and clamped.
/// @param[in,out] obj IKSolver3D to configure.
/// @param[in] pole Borrowed Vec3 pole position relative to the skeleton root.
void rt_ik_solver3d_set_pole(void *obj, void *pole) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver || !rt_g3d_is_vec3(pole))
        return;
    solver->pole[0] = ik3d_finite_coord(rt_vec3_x(pole), 0.0f);
    solver->pole[1] = ik3d_finite_coord(rt_vec3_y(pole), 0.0f);
    solver->pole[2] = ik3d_finite_coord(rt_vec3_z(pole), 0.0f);
    solver->has_pole = 1;
}

/// @brief Set a ground normal that aligns the end (foot) bone's sole after the position solve.
/// @details Defaults a missing Y component to 1 (up). Non-Vec3 normals are ignored.
///          The vector is interpreted in skeleton/model space, normalized, and
///          replaced with `(0,1,0)` when degenerate.
/// @param[in,out] obj IKSolver3D to configure.
/// @param[in] normal Borrowed Vec3 sole-up direction.
void rt_ik_solver3d_set_ground_normal(void *obj, void *normal) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver || !rt_g3d_is_vec3(normal))
        return;
    solver->ground_normal[0] = ik3d_finite_float(rt_vec3_x(normal), 0.0f);
    solver->ground_normal[1] = ik3d_finite_float(rt_vec3_y(normal), 1.0f);
    solver->ground_normal[2] = ik3d_finite_float(rt_vec3_z(normal), 0.0f);
    if (!ik3d_normalize3(solver->ground_normal)) {
        solver->ground_normal[0] = 0.0f;
        solver->ground_normal[1] = 1.0f;
        solver->ground_normal[2] = 0.0f;
    }
    solver->has_ground_normal = 1;
}

/// @brief Set a model-space orientation goal for the chain's end bone (non-Quat ignored).
/// @details Applied after the positional solve by slerping the end bone toward
///          the goal by solver weight. Takes precedence over a ground normal.
///          Components are sanitized to finite values and the quaternion is
///          normalized; a degenerate quaternion becomes identity.
/// @param[in,out] obj IKSolver3D to configure.
/// @param[in] rotation Borrowed Quat goal in skeleton/model space.
void rt_ik_solver3d_set_target_rotation(void *obj, void *rotation) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver || !rt_g3d_is_quat(rotation))
        return;
    solver->target_rotation[0] = ik3d_finite_float(rt_quat_x(rotation), 0.0f);
    solver->target_rotation[1] = ik3d_finite_float(rt_quat_y(rotation), 0.0f);
    solver->target_rotation[2] = ik3d_finite_float(rt_quat_z(rotation), 0.0f);
    solver->target_rotation[3] = ik3d_finite_float(rt_quat_w(rotation), 1.0f);
    ik3d_quat_normalize(solver->target_rotation);
    solver->has_target_rotation = 1;
}

/// @brief Remove the end-bone orientation goal; positional solving is unaffected.
/// @param[in,out] obj IKSolver3D to configure.
void rt_ik_solver3d_clear_target_rotation(void *obj) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver)
        return;
    solver->has_target_rotation = 0;
}

/// @brief Slerp the chain's end bone toward a desired model-space rotation by solver weight.
/// @details Shared tail for the ground-normal and target-rotation passes. The
///   desired global rotation is converted into the end bone's parent-local
///   space (correct for an arbitrarily-rotated parent), blended against the
///   current local rotation, and globals are rebuilt so downstream consumers
///   see the edit. Translation and scale are preserved.
/// @param[in] solver Solver providing skeleton, chain, and blend weight.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices rebuilt after the edit.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @param[in] desired_global Borrowed desired model-space rotation quaternion.
static void ik3d_slerp_end_to_global(rt_ik_solver3d *solver,
                                     float *locals,
                                     float *globals,
                                     int32_t bone_count,
                                     const float *desired_global) {
    int32_t end;
    int32_t parent;
    int32_t chain_count = ik3d_safe_chain_count(solver);
    float parent_rot[4], parent_conj[4], local_target[4];
    float cur_pos[3], cur_rot[4], cur_scl[3], blended[4];
    if (!solver || !locals || !globals || !desired_global || chain_count < 2)
        return;
    end = solver->chain[chain_count - 1];
    if (end < 0 || end >= bone_count)
        return;
    parent = solver->skeleton->bones[end].parent_index;
    if (parent >= 0 && parent < bone_count && parent != end) {
        float ppos[3], pscl[3];
        ik3d_decompose_trs(&globals[parent * 16], ppos, parent_rot, pscl);
    } else {
        ik3d_quat_identity(parent_rot);
    }
    ik3d_quat_conjugate(parent_rot, parent_conj);
    ik3d_quat_mul(parent_conj, desired_global, local_target);
    ik3d_decompose_trs(&locals[end * 16], cur_pos, cur_rot, cur_scl);
    ik3d_quat_slerp(cur_rot, local_target, solver->weight, blended);
    ik3d_build_trs(cur_pos, blended, cur_scl, &locals[end * 16]);
    ik3d_build_globals(solver->skeleton, locals, globals, bone_count);
}

/// @brief Lean the chain's end (foot) bone with the ground: tilt its current animated rotation by
///   the shortest arc from model +Y to the supplied normal. Run after the position solve.
/// @details ADR 0286: this is a delta, not an absolute basis. On a flat surface
///   (normal = model +Y) the arc is identity and the authored pose survives
///   exactly; on a slope the animated rotation leans with the surface. The
///   pass therefore never depends on the rig's bone-axis convention.
/// @param[in] solver Solver providing skeleton, chain, normal, and blend
///                   weight.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices rebuilt after the
///                        orientation edit.
/// @param[in] bone_count Number of matrices available in both arrays.
static void ik3d_apply_foot_orientation(rt_ik_solver3d *solver,
                                        float *locals,
                                        float *globals,
                                        int32_t bone_count) {
    int32_t end;
    int32_t chain_count = ik3d_safe_chain_count(solver);
    float up[3];
    float model_up[3] = {0.0f, 1.0f, 0.0f};
    float tilt[4], cur_global_rot[4], desired_global[4];
    float g_pos[3], g_scl[3];
    if (!solver || !locals || !globals || chain_count < 2)
        return;
    end = solver->chain[chain_count - 1];
    if (end < 0 || end >= bone_count)
        return;
    up[0] = solver->ground_normal[0];
    up[1] = solver->ground_normal[1];
    up[2] = solver->ground_normal[2];
    if (!ik3d_normalize3(up))
        return;
    ik3d_quat_from_to(model_up, up, tilt);
    ik3d_decompose_trs(&globals[end * 16], g_pos, cur_global_rot, g_scl);
    ik3d_quat_mul(tilt, cur_global_rot, desired_global);
    ik3d_slerp_end_to_global(solver, locals, globals, bone_count, desired_global);
}

/// @brief Slerp the chain's end bone toward the solver's model-space orientation goal.
/// @details Run after the position solve; takes precedence over the
///   ground-normal pass (ADR 0286). The stored goal is already sanitized and
///   normalized by the setter.
/// @param[in] solver Solver providing skeleton, chain, goal, and blend weight.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices rebuilt after the
///                        orientation edit.
/// @param[in] bone_count Number of matrices available in both arrays.
static void ik3d_apply_end_target_rotation(rt_ik_solver3d *solver,
                                           float *locals,
                                           float *globals,
                                           int32_t bone_count) {
    if (!solver)
        return;
    ik3d_slerp_end_to_global(solver, locals, globals, bone_count, solver->target_rotation);
}

/// @brief Swing-rotate @p bone in place so its child joint (currently at the
///   position read from @p globals for @p child_bone) lands on @p child_target,
///   preserving the bone's authored twist, translation, and scale.
/// @details This is what makes IK visibly BEND a skinned limb: skinned vertices
///   follow bone rotations, so translating joint origins alone (the positional
///   FABRIK write) shears the mesh instead of articulating it. The swing is the
///   shortest arc between the current and solved child directions, composed
///   onto the bone's global rotation and converted back to parent-local using
///   the same decompose/recompose path as the foot-orientation pass. Globals
///   are rebuilt so downstream chain links see the rotation.
/// @param[in] solver Solver providing the skeleton hierarchy.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices rebuilt after aiming.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @param[in] bone Chain link whose local rotation to replace.
/// @param[in] child_bone Direct child used to measure the current direction.
/// @param[in] child_target Borrowed solved model-space child position.
static void ik3d_aim_bone_child_at(rt_ik_solver3d *solver,
                                   float *locals,
                                   float *globals,
                                   int32_t bone_count,
                                   int32_t bone,
                                   int32_t child_bone,
                                   const float *child_target) {
    float origin[3], child_cur[3];
    float v_from[3], v_to[3];
    float arc[4];
    float g_pos[3], g_rot[4], g_scl[3];
    float new_global_rot[4];
    float parent_rot[4], parent_conj[4], local_rot[4];
    float l_pos[3], l_rot[4], l_scl[3];
    int32_t parent;
    if (!solver || !locals || !globals || bone < 0 || bone >= bone_count || child_bone < 0 ||
        child_bone >= bone_count || !child_target)
        return;
    ik3d_global_position(globals, bone, origin);
    ik3d_global_position(globals, child_bone, child_cur);
    for (int lane = 0; lane < 3; lane++) {
        v_from[lane] = child_cur[lane] - origin[lane];
        v_to[lane] = child_target[lane] - origin[lane];
    }
    if (!ik3d_normalize3(v_from) || !ik3d_normalize3(v_to))
        return;
    ik3d_quat_from_to(v_from, v_to, arc);
    ik3d_decompose_trs(&globals[bone * 16], g_pos, g_rot, g_scl);
    ik3d_quat_mul(arc, g_rot, new_global_rot);
    parent = solver->skeleton->bones[bone].parent_index;
    if (parent >= 0 && parent < bone_count && parent != bone) {
        float ppos[3], pscl[3];
        ik3d_decompose_trs(&globals[parent * 16], ppos, parent_rot, pscl);
    } else {
        ik3d_quat_identity(parent_rot);
    }
    ik3d_quat_conjugate(parent_rot, parent_conj);
    ik3d_quat_mul(parent_conj, new_global_rot, local_rot);
    ik3d_decompose_trs(&locals[bone * 16], l_pos, l_rot, l_scl);
    ik3d_build_trs(l_pos, local_rot, l_scl, &locals[bone * 16]);
    ik3d_build_globals(solver->skeleton, locals, globals, bone_count);
}

/// @brief Solve a multi-bone chain toward the target using FABRIK, then blend by weight.
/// @details If the target is out of reach (distance >= total chain length) the chain is
///          straightened toward it; otherwise FABRIK alternates backward (from the target)
///          and forward (from the fixed root) reaching passes until convergence. For a
///          three-bone chain with a pole target the mid joint is swung onto the pole plane,
///          the solved positions are blended against the originals by @p solver->weight, and
///          a ground normal (if set) finally orients the foot bone.
/// @param[in,out] solver Chain solver providing goals, topology, and weight.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices, rebuilt throughout
///                        the solve.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @return `1` for a completed or intentionally no-op solve; `0` for invalid
///         storage, topology, or bone indexes.
static int ik3d_apply_chain(rt_ik_solver3d *solver,
                            float *locals,
                            float *globals,
                            int32_t bone_count) {
    float positions[RT_IK_SOLVER3D_MAX_CHAIN][3];
    float original[RT_IK_SOLVER3D_MAX_CHAIN][3];
    float lengths[RT_IK_SOLVER3D_MAX_CHAIN - 1];
    float total = 0.0f;
    int32_t count;
    count = ik3d_safe_chain_count(solver);
    if (!solver || !locals || !globals || count < 2)
        return 0;
    /* Zero (or negative) weight means the chain contributes nothing — the
     * final blend would reproduce the original pose exactly — so skip the
     * whole FABRIK solve and pole/foot passes rather than burning them. */
    if (!(solver->weight > 0.0f))
        return 1;
    if (!ik3d_chain_is_parented(solver->skeleton, solver->chain, count))
        return 0;
    for (int32_t i = 0; i < count; i++) {
        if (solver->chain[i] < 0 || solver->chain[i] >= bone_count)
            return 0;
        ik3d_global_position(globals, solver->chain[i], original[i]);
        memcpy(positions[i], original[i], 3 * sizeof(float));
    }
    for (int32_t i = 0; i < count - 1; i++) {
        lengths[i] = ik3d_distance3(positions[i], positions[i + 1]);
        total += lengths[i];
    }
    if (total <= 1e-6f)
        return 1;

    float root[3] = {positions[0][0], positions[0][1], positions[0][2]};
    float to_target[3] = {
        solver->target[0] - root[0], solver->target[1] - root[1], solver->target[2] - root[2]};
    float root_dist = ik3d_len3(to_target);
    if (root_dist >= total) {
        if (!ik3d_normalize3(to_target))
            return 1;
        for (int32_t i = 1; i < count; i++) {
            positions[i][0] = positions[i - 1][0] + to_target[0] * lengths[i - 1];
            positions[i][1] = positions[i - 1][1] + to_target[1] * lengths[i - 1];
            positions[i][2] = positions[i - 1][2] + to_target[2] * lengths[i - 1];
        }
    } else {
        for (int iter = 0; iter < RT_IK_SOLVER3D_FABRIK_ITERS; iter++) {
            memcpy(positions[count - 1], solver->target, 3 * sizeof(float));
            for (int32_t i = count - 2; i >= 0; i--) {
                float dir[3] = {positions[i][0] - positions[i + 1][0],
                                positions[i][1] - positions[i + 1][1],
                                positions[i][2] - positions[i + 1][2]};
                if (!ik3d_normalize3(dir))
                    continue;
                positions[i][0] = positions[i + 1][0] + dir[0] * lengths[i];
                positions[i][1] = positions[i + 1][1] + dir[1] * lengths[i];
                positions[i][2] = positions[i + 1][2] + dir[2] * lengths[i];
            }
            memcpy(positions[0], root, 3 * sizeof(float));
            for (int32_t i = 1; i < count; i++) {
                float dir[3] = {positions[i][0] - positions[i - 1][0],
                                positions[i][1] - positions[i - 1][1],
                                positions[i][2] - positions[i - 1][2]};
                if (!ik3d_normalize3(dir))
                    continue;
                positions[i][0] = positions[i - 1][0] + dir[0] * lengths[i - 1];
                positions[i][1] = positions[i - 1][1] + dir[1] * lengths[i - 1];
                positions[i][2] = positions[i - 1][2] + dir[2] * lengths[i - 1];
            }
            if (ik3d_distance3(positions[count - 1], solver->target) <= 1e-4f)
                break;
        }
    }

    /* Pole-vector control (two-bone chains only): swing the middle joint around
     * the root->end axis so it points toward the pole target, preserving bone
     * lengths. Without this the FABRIK pass leaves the knee/elbow wherever it
     * happened to converge. */
    if (solver->has_pole && count == 3 && root_dist < total) {
        float axis[3] = {positions[2][0] - positions[0][0],
                         positions[2][1] - positions[0][1],
                         positions[2][2] - positions[0][2]};
        if (ik3d_normalize3(axis)) {
            float to_mid[3] = {positions[1][0] - positions[0][0],
                               positions[1][1] - positions[0][1],
                               positions[1][2] - positions[0][2]};
            float proj = to_mid[0] * axis[0] + to_mid[1] * axis[1] + to_mid[2] * axis[2];
            float on_axis[3] = {positions[0][0] + axis[0] * proj,
                                positions[0][1] + axis[1] * proj,
                                positions[0][2] + axis[2] * proj};
            float bend[3] = {positions[1][0] - on_axis[0],
                             positions[1][1] - on_axis[1],
                             positions[1][2] - on_axis[2]};
            float bend_len = ik3d_len3(bend);
            float to_pole[3] = {solver->pole[0] - positions[0][0],
                                solver->pole[1] - positions[0][1],
                                solver->pole[2] - positions[0][2]};
            float pdot = to_pole[0] * axis[0] + to_pole[1] * axis[1] + to_pole[2] * axis[2];
            float perp[3] = {to_pole[0] - axis[0] * pdot,
                             to_pole[1] - axis[1] * pdot,
                             to_pole[2] - axis[2] * pdot};
            if (bend_len > 1e-6f && ik3d_normalize3(perp)) {
                positions[1][0] = on_axis[0] + perp[0] * bend_len;
                positions[1][1] = on_axis[1] + perp[1] * bend_len;
                positions[1][2] = on_axis[2] + perp[2] * bend_len;
            }
        }
    }

    for (int32_t i = 1; i < count; i++) {
        float blended[3];
        int32_t bone = solver->chain[i];
        int32_t parent_link = solver->chain[i - 1];
        for (int lane = 0; lane < 3; lane++)
            blended[lane] =
                original[i][lane] + (positions[i][lane] - original[i][lane]) * solver->weight;
        /* Two writes per link: swing the parent link so its child joint AIMS
         * at the solved position (this is what bends the skinned limb — see
         * ik3d_aim_bone_child_at), then pin the child's translation exactly to
         * guard against numeric drift in segment length. */
        ik3d_aim_bone_child_at(solver, locals, globals, bone_count, parent_link, bone, blended);
        ik3d_set_global_position(solver, locals, globals, bone_count, bone, blended);
    }
    /* End-bone orientation: an explicit goal wins over the ground hint (ADR 0286). */
    if (solver->has_target_rotation)
        ik3d_apply_end_target_rotation(solver, locals, globals, bone_count);
    else if (solver->has_ground_normal)
        ik3d_apply_foot_orientation(solver, locals, globals, bone_count);
    return 1;
}

/// @brief Solve a single-bone look-at: rotate the bone so its forward axis faces the target.
/// @details Builds an orthonormal basis (right/up/forward) from the bone-to-target direction,
///          switching the reference up to model +X when forward is near-vertical to avoid a
///          degenerate cross product, then slerps from the current local rotation by weight.
/// @param[in] solver Look-at solver providing bone, target, skeleton, and
///                   weight.
/// @param[in,out] locals Writable local-pose matrices.
/// @param[in,out] globals Writable model-space matrices rebuilt after aiming.
/// @param[in] bone_count Number of matrices available in both arrays.
/// @return `1` for a completed or degenerate-direction no-op; `0` for invalid
///         arguments or bone selection.
static int ik3d_apply_look_at(rt_ik_solver3d *solver,
                              float *locals,
                              float *globals,
                              int32_t bone_count) {
    int32_t bone;
    float pos[3];
    float forward[3];
    float right[3];
    float up[3] = {0.0f, 1.0f, 0.0f};
    float up2[3];
    float local_pos[3], local_rot[4], local_scl[3];
    float target_global_rot[4], target_local_rot[4], blended_rot[4];
    float parent_rot[4], parent_conj[4];
    int32_t parent;
    if (!solver || !locals || !globals || ik3d_safe_chain_count(solver) != 1)
        return 0;
    bone = solver->chain[0];
    if (bone < 0 || bone >= bone_count)
        return 0;
    ik3d_global_position(globals, bone, pos);
    forward[0] = solver->target[0] - pos[0];
    forward[1] = solver->target[1] - pos[1];
    forward[2] = solver->target[2] - pos[2];
    if (!ik3d_normalize3(forward))
        return 1;
    if (fabsf(ik3d_dot3(forward, up)) > 0.98f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    }
    ik3d_cross3(up, forward, right);
    if (!ik3d_normalize3(right))
        return 1;
    ik3d_cross3(forward, right, up2);
    if (!ik3d_normalize3(up2))
        return 1;
    ik3d_quat_from_matrix_rows(right[0],
                               up2[0],
                               forward[0],
                               right[1],
                               up2[1],
                               forward[1],
                               right[2],
                               up2[2],
                               forward[2],
                               target_global_rot);
    parent = solver->skeleton->bones[bone].parent_index;
    if (parent >= 0 && parent < bone_count && parent != bone) {
        float parent_pos[3], parent_scale[3];
        ik3d_decompose_trs(&globals[parent * 16], parent_pos, parent_rot, parent_scale);
    } else {
        ik3d_quat_identity(parent_rot);
    }
    ik3d_quat_conjugate(parent_rot, parent_conj);
    ik3d_quat_mul(parent_conj, target_global_rot, target_local_rot);
    ik3d_decompose_trs(&locals[bone * 16], local_pos, local_rot, local_scl);
    ik3d_quat_slerp(local_rot, target_local_rot, solver->weight, blended_rot);
    ik3d_build_trs(local_pos, blended_rot, local_scl, &locals[bone * 16]);
    ik3d_build_globals(solver->skeleton, locals, globals, bone_count);
    return 1;
}

/// @brief Apply the solver in place to a controller's local-pose buffer and refresh globals.
/// @details A weight at/below 1e-6 is a no-op success; @p bone_count is clamped to the
///          skeleton. Dispatches to the look-at or chain solver by kind. Returns 1 on
///          success (including the no-op), 0 on invalid arguments.
/// @param[in,out] obj IKSolver3D whose configured constraint to apply.
/// @param[in,out] locals Writable array of at least `bone_count * 16`
///                       local-matrix floats.
/// @param[out] globals Writable array of at least `bone_count * 16`
///                     model-space matrix floats.
/// @param[in] bone_count Available matrix count, clamped to the solver
///                       skeleton's safe count.
/// @return `1` when applied or skipped for negligible weight; `0` for invalid
///         handles, buffers, count, topology, or bone selection.
int8_t rt_ik_solver3d_apply_to_pose(void *obj, float *locals, float *globals, int32_t bone_count) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    int32_t safe_bone_count;
    if (!solver || !locals || !globals || bone_count <= 0)
        return 0;
    safe_bone_count = ik_solver3d_repair_state(solver);
    if (safe_bone_count <= 0)
        return 0;
    if (solver->weight <= 1e-6f)
        return 1;
    if (bone_count > safe_bone_count)
        bone_count = safe_bone_count;
    if (bone_count <= 0)
        return 0;
    ik3d_build_globals(solver->skeleton, locals, globals, bone_count);
    switch (solver->kind) {
        case RT_IK_SOLVER3D_LOOK_AT:
            return (int8_t)ik3d_apply_look_at(solver, locals, globals, bone_count);
        case RT_IK_SOLVER3D_TWO_BONE:
        case RT_IK_SOLVER3D_FABRIK:
            return (int8_t)ik3d_apply_chain(solver, locals, globals, bone_count);
        default:
            return 0;
    }
}

/// @brief Re-solve against the skeleton bind pose, refreshing the solver's owned pose buffers.
/// @details Reseeds solved_locals from each bone's bind pose, then applies the solver so the
///          cached solved_locals/solved_globals reflect the current target/weight settings.
/// @param[in,out] obj IKSolver3D whose private pose buffers to refresh.
void rt_ik_solver3d_solve(void *obj) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    int32_t bone_count;
    if (!solver)
        return;
    bone_count = ik_solver3d_repair_state(solver);
    if (bone_count <= 0)
        return;
    for (int32_t bone = 0; bone < bone_count; bone++) {
        memcpy(&solver->solved_locals[bone * 16],
               solver->skeleton->bones[bone].bind_pose_local,
               16 * sizeof(float));
    }
    rt_ik_solver3d_apply_to_pose(solver, solver->solved_locals, solver->solved_globals, bone_count);
}

/// @brief Borrow the Skeleton3D handle retained by this solver (not retained; NULL if invalid).
/// @param[in] obj IKSolver3D to inspect.
/// @return Borrowed validated Skeleton3D handle, or `NULL`; the caller must not
///         release it.
void *rt_ik_solver3d_get_skeleton(void *obj) {
    rt_ik_solver3d *solver = ik_solver3d_checked(obj);
    if (!solver || !solver->skeleton)
        return NULL;
    if (!rt_g3d_has_class(solver->skeleton, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned((void **)&solver->skeleton);
        return NULL;
    }
    return solver->skeleton;
}

#else
typedef int rt_iksolver3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
