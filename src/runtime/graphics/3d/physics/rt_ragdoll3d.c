//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_ragdoll3d.c
// Purpose: Ragdoll3D implementation — builds capsule bodies and limited 6-DoF
//   joints from a Skeleton3D's bind pose, hands off from the animated pose with
//   finite-difference velocity seeding, writes body poses back into the anim
//   controller palette each step (with node root-follow), blends back to
//   animation on deactivate, and PD-drives masked joints toward the animated
//   pose in powered mode.
// Key invariants:
//   - Joints are created with both bodies posed at bind, so per-axis 6-DoF
//     limits are relative to the bind pose (the joint captures its reference
//     relative orientation at creation).
//   - Palette write-back preserves world-space vertex positions across the
//     node root-follow rebase (translations are shifted in the same step).
//   - The rig assumes an (approximately) uniformly-scaled entity; capsule
//     dimensions are fit in model units.
// Ownership/Lifetime:
//   - GC-managed; retains skeleton always, bodies/joints for the rig lifetime,
//     and world/controller/node only while active or blending out.
// Links: misc/plans/thirdpersonupgrade/07-ragdoll.md, rt_ragdoll3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements skeleton-derived capsule ragdolls, animation handoff, and pose blending.
///
/// Rigs are built lazily from skeleton bind globals, with capsule mass distributed by volume
/// and limited six-degree-of-freedom joints connecting the nearest bodied ancestors. Activation
/// seeds world-space velocities from animation palettes; active steps write physics poses back
/// to animation, and deactivation can blend smoothly toward live animation.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_ragdoll3d.h"
#include "rt_animcontroller3d.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_joints3d.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_trap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAGDOLL3D_DEFAULT_TOTAL_MASS 70.0
#define RAGDOLL3D_DEFAULT_RADIUS_SCALE 0.22
#define RAGDOLL3D_DEFAULT_MIN_BONE_LENGTH 0.12
#define RAGDOLL3D_DEFAULT_SWING_DEG 30.0
#define RAGDOLL3D_DEFAULT_TWIST_DEG 20.0
#define RAGDOLL3D_TERMINAL_LENGTH 0.15
#define RAGDOLL3D_HANDOFF_DT (1.0 / 60.0)
#define RAGDOLL3D_MAX_BODIES 64
#define RAGDOLL3D_PARAM_MAX 1.0e9
/// Rig bodies share a dedicated collision layer whose own bit is excluded from
/// their masks: limbs never collide with each other (joint anchors overlap by
/// construction) but still collide with all world geometry.
#define RAGDOLL3D_COLLISION_LAYER (INT64_C(1) << 62)

//=========================================================================
// Local double-precision math helpers (row-major matrices, xyzw quats)
//=========================================================================

/// @brief Initializes a row-major 4x4 matrix to identity.
/// @param[out] m Writable 16-element matrix.
static void rg_mat_identity(double *m) {
    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

/// @brief Row-major multiply out = a * b (matches the skeleton module).
/// @param a Borrowed 16-element left matrix.
/// @param b Borrowed 16-element right matrix.
/// @param[out] out Writable 16-element product; may alias either input.
static void rg_mat_mul(const double *a, const double *b, double *out) {
    double tmp[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            tmp[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    memcpy(out, tmp, sizeof(tmp));
}

/// @brief Multiplies two xyzw quaternions.
/// @param a Borrowed left quaternion.
/// @param b Borrowed right quaternion.
/// @param[out] out Product quaternion; must not alias an input.
static void rg_quat_mul(const double a[4], const double b[4], double out[4]) {
    double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

/// @brief Computes the conjugate of an xyzw quaternion.
/// @param q Borrowed input quaternion.
/// @param[out] out Conjugated quaternion; may alias @p q.
static void rg_quat_conj(const double q[4], double out[4]) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Normalizes an xyzw quaternion in place with an identity fallback.
/// @param[in,out] q Quaternion to normalize; non-finite or negligible inputs become identity.
static void rg_quat_normalize(double q[4]) {
    double scale = fmax(fabs(q[0]), fmax(fabs(q[1]), fmax(fabs(q[2]), fabs(q[3]))));
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3]) ||
        !isfinite(scale) || scale < 1e-12) {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    q[0] /= scale;
    q[1] /= scale;
    q[2] /= scale;
    q[3] /= scale;
    double len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!isfinite(len) || len < 1e-12) {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    q[0] /= len;
    q[1] /= len;
    q[2] /= len;
    q[3] /= len;
}

/// @brief Normalize a finite three-vector with maximum-component scaling.
/// @param[in,out] v Vector to normalize.
/// @return Nonzero when a unit vector was produced.
static int rg_vec3_normalize(double v[3]) {
    double scale;
    double len;
    if (!v || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    scale = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));
    if (!isfinite(scale) || scale < 1e-12)
        return 0;
    v[0] /= scale;
    v[1] /= scale;
    v[2] /= scale;
    len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!isfinite(len) || len < 1e-12)
        return 0;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return 1;
}

/// @brief Return whether every lane of a matrix is finite.
/// @param m Borrowed 16-element matrix.
/// @return Nonzero for a complete finite matrix.
static int rg_mat_is_finite(const double m[16]) {
    if (!m)
        return 0;
    for (int lane = 0; lane < 16; ++lane) {
        if (!isfinite(m[lane]))
            return 0;
    }
    return 1;
}

/// @brief Rotates a three-dimensional vector by a unit xyzw quaternion.
/// @param q Borrowed rotation quaternion.
/// @param v Borrowed input vector.
/// @param[out] out Rotated vector; must not alias @p v.
static void rg_quat_rotate(const double q[4], const double v[3], double out[3]) {
    double cx = q[1] * v[2] - q[2] * v[1];
    double cy = q[2] * v[0] - q[0] * v[2];
    double cz = q[0] * v[1] - q[1] * v[0];
    double ccx = q[1] * cz - q[2] * cy;
    double ccy = q[2] * cx - q[0] * cz;
    double ccz = q[0] * cy - q[1] * cx;
    out[0] = v[0] + 2.0 * (q[3] * cx + ccx);
    out[1] = v[1] + 2.0 * (q[3] * cy + ccy);
    out[2] = v[2] + 2.0 * (q[3] * cz + ccz);
}

/// @brief Quaternion rotating unit +Y onto unit @p dir.
/// @param dir Borrowed normalized target direction.
/// @param[out] out Destination xyzw quaternion, with stable identity and 180-degree special cases.
static void rg_quat_align_y(const double dir[3], double out[4]) {
    double d = dir[1]; /* dot(+Y, dir) */
    if (d > 1.0 - 1e-9) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    if (d < -1.0 + 1e-9) {
        out[0] = 1.0; /* 180 about X */
        out[1] = out[2] = 0.0;
        out[3] = 0.0;
        return;
    }
    /* cross(+Y, dir) = (1*dir.z - 0*dir.y, 0*dir.x - 0*dir.z, 0*dir.y - 1*dir.x)
     *               = (dir.z, 0, -dir.x) */
    double axis[3];
    axis[0] = dir[2];
    axis[1] = 0.0;
    axis[2] = -dir[0];
    double axis_len = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (axis_len < 1e-12) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    double half = acos(fmax(-1.0, fmin(1.0, d))) * 0.5;
    double s = sin(half) / axis_len;
    out[0] = axis[0] * s;
    out[1] = axis[1] * s;
    out[2] = axis[2] * s;
    out[3] = cos(half);
}

/// @brief Extract the rotation quaternion from a row-major matrix (unit-scale-ish).
/// @param m Borrowed 16-element affine matrix whose upper-left 3x3 supplies rotation.
/// @param[out] out Normalized xyzw rotation quaternion.
static void rg_quat_from_mat(const double *m, double out[4]) {
    double x_axis[3] = {m[0], m[4], m[8]};
    double y_axis[3] = {m[1], m[5], m[9]};
    double z_axis[3] = {m[2], m[6], m[10]};
    if (!rg_vec3_normalize(x_axis) || !rg_vec3_normalize(y_axis) || !rg_vec3_normalize(z_axis)) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    double r00 = x_axis[0], r01 = y_axis[0], r02 = z_axis[0];
    double r10 = x_axis[1], r11 = y_axis[1], r12 = z_axis[1];
    double r20 = x_axis[2], r21 = y_axis[2], r22 = z_axis[2];
    double trace = r00 + r11 + r22;
    if (trace > 0.0) {
        double s = sqrt(trace + 1.0) * 2.0;
        out[3] = 0.25 * s;
        out[0] = (r21 - r12) / s;
        out[1] = (r02 - r20) / s;
        out[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        double s = sqrt(1.0 + r00 - r11 - r22) * 2.0;
        out[3] = (r21 - r12) / s;
        out[0] = 0.25 * s;
        out[1] = (r01 + r10) / s;
        out[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        double s = sqrt(1.0 + r11 - r00 - r22) * 2.0;
        out[3] = (r02 - r20) / s;
        out[0] = (r01 + r10) / s;
        out[1] = 0.25 * s;
        out[2] = (r12 + r21) / s;
    } else {
        double s = sqrt(1.0 + r22 - r00 - r11) * 2.0;
        out[3] = (r10 - r01) / s;
        out[0] = (r02 + r20) / s;
        out[1] = (r12 + r21) / s;
        out[2] = 0.25 * s;
    }
    rg_quat_normalize(out);
}

/// @brief Compose a row-major matrix from rotation quat + translation.
/// @param q Borrowed normalized xyzw rotation quaternion.
/// @param p Borrowed three-element translation.
/// @param[out] m Writable 16-element affine matrix with unit scale.
static void rg_mat_from_quat_pos(const double q[4], const double p[3], double *m) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1.0 - 2.0 * (y * y + z * z);
    m[1] = 2.0 * (x * y - z * w);
    m[2] = 2.0 * (x * z + y * w);
    m[3] = p[0];
    m[4] = 2.0 * (x * y + z * w);
    m[5] = 1.0 - 2.0 * (x * x + z * z);
    m[6] = 2.0 * (y * z - x * w);
    m[7] = p[1];
    m[8] = 2.0 * (x * z - y * w);
    m[9] = 2.0 * (y * z + x * w);
    m[10] = 1.0 - 2.0 * (x * x + y * y);
    m[11] = p[2];
    m[12] = 0.0;
    m[13] = 0.0;
    m[14] = 0.0;
    m[15] = 1.0;
}

/// @brief Axis-angle rotation vector taking quaternion a to b (radians).
/// @param a Borrowed starting xyzw quaternion.
/// @param b Borrowed target xyzw quaternion.
/// @param[out] out Three-element shortest-arc rotation vector in radians.
static void rg_quat_delta_rotvec(const double a[4], const double b[4], double out[3]) {
    double inv_a[4];
    double delta[4];
    rg_quat_conj(a, inv_a);
    rg_quat_mul(b, inv_a, delta);
    rg_quat_normalize(delta);
    if (delta[3] < 0.0) {
        delta[0] = -delta[0];
        delta[1] = -delta[1];
        delta[2] = -delta[2];
        delta[3] = -delta[3];
    }
    double sin_half = sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    if (sin_half < 1e-9) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    double angle = 2.0 * atan2(sin_half, delta[3]);
    double scale = angle / sin_half;
    out[0] = delta[0] * scale;
    out[1] = delta[1] * scale;
    out[2] = delta[2] * scale;
}

//=========================================================================
// Rig structures
//=========================================================================

typedef struct rt_ragdoll3d_slot {
    int32_t bone_index;          ///< Skeleton bone driven by this body.
    int32_t parent_slot;         ///< Rig slot of the nearest bodied ancestor, -1 root.
    void *body;                  ///< Retained capsule Body3D.
    void *joint;                 ///< Retained SixDofJoint3D to the parent body.
    double length;               ///< Bone segment length (model units).
    double radius;               ///< Capsule radius.
    double body_offset_bone[3];  ///< Body center in bone space.
    double body_to_bone_quat[4]; ///< Body rotation expressed in bone space.
    double bind_global[16];      ///< Bone bind global (model space, row-major).
    double swing_deg;            ///< Joint swing limit override.
    double twist_deg;            ///< Joint twist limit override.
} rt_ragdoll3d_slot;

typedef struct rt_ragdoll3d {
    void *vptr;
    void *skeleton;   ///< Retained Skeleton3D.
    void *world;      ///< Retained physics world while active.
    void *controller; ///< Retained AnimController3D while active/blending.
    void *node;       ///< Retained SceneNode3D while active/blending.
    rt_ragdoll3d_slot *slots;
    int32_t slot_count;
    int32_t slot_capacity;
    int32_t *bone_to_slot; ///< Skeleton bone index -> rig slot (-1).
    int32_t skeleton_bone_count;
    int32_t bone_capacity; ///< Allocation bound shared by bone-indexed arrays.
    double total_mass;
    double radius_scale;
    double min_bone_length;
    int8_t built;
    int8_t active;
    double blend_remaining; ///< Deactivate blend-out timer.
    double blend_duration;
    float *blend_from; ///< Captured globals at deactivate (bone_count*16).
    int32_t blend_bone_capacity;
    int64_t powered_mask;
    double powered_stiffness;
    float *override_globals; ///< Scratch: bone_count*16 write-back buffer.
    int8_t *override_mask;   ///< Scratch: bone_count write-back mask.
    int8_t override_ready;   ///< At least one active physics pose was published.
} rt_ragdoll3d;

/// @brief Clamp operational slot traversal to the allocation and hard body budget.
/// @param ragdoll Rig whose private counts are inspected.
/// @return Safe readable slot count.
static int32_t ragdoll3d_safe_slot_count(const rt_ragdoll3d *ragdoll) {
    int32_t capacity;
    if (!ragdoll || !ragdoll->slots || ragdoll->slot_count <= 0 || ragdoll->slot_capacity <= 0)
        return 0;
    capacity = ragdoll->slot_capacity < RAGDOLL3D_MAX_BODIES ? ragdoll->slot_capacity
                                                             : RAGDOLL3D_MAX_BODIES;
    return ragdoll->slot_count < capacity ? ragdoll->slot_count : capacity;
}

/// @brief Validate the shared allocation extent of every bone-indexed array.
/// @param ragdoll Rig whose private bone count and allocations are inspected.
/// @return Safe common bone count, or zero for incomplete/corrupt storage.
static int32_t ragdoll3d_safe_bone_count(const rt_ragdoll3d *ragdoll) {
    if (!ragdoll || ragdoll->skeleton_bone_count <= 0 || ragdoll->bone_capacity <= 0 ||
        ragdoll->skeleton_bone_count != ragdoll->bone_capacity || !ragdoll->bone_to_slot ||
        !ragdoll->override_globals || !ragdoll->override_mask)
        return 0;
    return ragdoll->bone_capacity;
}

/// @brief Validate @p obj as a Ragdoll3D handle, trapping @p method on mismatch.
/// @param obj Borrowed runtime object to validate.
/// @param method Static diagnostic text used when validation fails.
/// @return Borrowed ragdoll payload, or null after raising the supplied trap.
static rt_ragdoll3d *ragdoll3d_checked(void *obj, const char *method) {
    rt_ragdoll3d *ragdoll = (rt_ragdoll3d *)rt_g3d_checked_or_null(obj, RT_G3D_RAGDOLL3D_CLASS_ID);
    if (!ragdoll)
        rt_trap(method);
    return ragdoll;
}

/// @brief Releases one retained runtime object and clears its storage slot.
/// @param[in,out] slot Address of an owned object pointer; null or empty slots are no-ops.
static void ragdoll3d_release_obj(void **slot) {
    if (slot && *slot) {
        if (rt_obj_release_check0(*slot))
            rt_obj_free(*slot);
        *slot = NULL;
    }
}

/// @brief Release an owned persistent reference only when its class matches.
/// @details Wrong-class private corruption is cleared as unowned so finalization never decrements
///          an unrelated object's reference count.
/// @param[in,out] slot Owned typed reference slot to clear.
/// @param class_id Expected Graphics3D runtime class id.
static void ragdoll3d_release_typed(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Remove every safely addressable rig joint and body from its active world.
/// @param[in,out] ragdoll Rig whose registrations are removed; retained objects stay alive.
static void ragdoll3d_unregister(rt_ragdoll3d *ragdoll) {
    int32_t slot_count;
    if (!ragdoll || !rt_g3d_has_class(ragdoll->world, RT_G3D_WORLD3D_CLASS_ID))
        return;
    slot_count = ragdoll3d_safe_slot_count(ragdoll);
    for (int32_t s = 0; s < slot_count; ++s) {
        if (rt_g3d_has_class(ragdoll->slots[s].joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID))
            rt_world3d_remove_joint(ragdoll->world, ragdoll->slots[s].joint);
    }
    for (int32_t s = 0; s < slot_count; ++s) {
        if (rt_g3d_has_class(ragdoll->slots[s].body, RT_G3D_BODY3D_CLASS_ID))
            rt_world3d_remove(ragdoll->world, ragdoll->slots[s].body);
    }
}

/// @brief Drop the built rig (bodies/joints released; config kept).
/// @param[in,out] ragdoll Borrowed ragdoll whose built arrays and retained rig objects are cleared.
static void ragdoll3d_release_rig(rt_ragdoll3d *ragdoll) {
    if (!ragdoll)
        return;
    int32_t slot_count = ragdoll3d_safe_slot_count(ragdoll);
    for (int32_t i = 0; i < slot_count; ++i) {
        ragdoll3d_release_typed(&ragdoll->slots[i].joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
        ragdoll3d_release_typed(&ragdoll->slots[i].body, RT_G3D_BODY3D_CLASS_ID);
    }
    free(ragdoll->slots);
    ragdoll->slots = NULL;
    ragdoll->slot_count = 0;
    ragdoll->slot_capacity = 0;
    free(ragdoll->bone_to_slot);
    ragdoll->bone_to_slot = NULL;
    ragdoll->bone_capacity = 0;
    free(ragdoll->override_globals);
    ragdoll->override_globals = NULL;
    free(ragdoll->override_mask);
    ragdoll->override_mask = NULL;
    ragdoll->built = 0;
    ragdoll->override_ready = 0;
}

/// @brief GC finalizer: tear down the rig and release retained references.
/// @param obj Owned `rt_ragdoll3d` payload being finalized; null is a no-op.
static void ragdoll3d_finalize(void *obj) {
    rt_ragdoll3d *ragdoll = (rt_ragdoll3d *)obj;
    if (!ragdoll)
        return;
    ragdoll3d_unregister(ragdoll);
    ragdoll3d_release_rig(ragdoll);
    free(ragdoll->blend_from);
    ragdoll->blend_from = NULL;
    ragdoll->blend_bone_capacity = 0;
    ragdoll3d_release_typed(&ragdoll->world, RT_G3D_WORLD3D_CLASS_ID);
    ragdoll3d_release_typed(&ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    ragdoll3d_release_typed(&ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID);
    ragdoll3d_release_typed(&ragdoll->skeleton, RT_G3D_SKELETON3D_CLASS_ID);
}

//=========================================================================
// Builder
//=========================================================================

/// @brief Build slots, bodies (posed at bind, not world-registered), and joints.
/// @details Selects the root and sufficiently long bones, caps the rig at 64 bodies, distributes
///          configured mass by capsule volume, and captures fixed body-to-bone frames. The build
///          is transactional: partial bodies, joints, and mappings are released on failure.
/// @param[in,out] ragdoll Borrowed rig description whose lazy body/joint representation is built.
/// @return `1` when an existing or newly completed rig is available; `0` for an empty skeleton or
///         allocation/object-construction failure.
static int ragdoll3d_ensure_built(rt_ragdoll3d *ragdoll) {
    if (!ragdoll || !rt_g3d_has_class(ragdoll->skeleton, RT_G3D_SKELETON3D_CLASS_ID))
        return 0;
    if (ragdoll->built)
        return ragdoll3d_safe_slot_count(ragdoll) > 0 && ragdoll3d_safe_bone_count(ragdoll) > 0;
    int64_t bone_count_i64 = rt_skeleton3d_get_bone_count(ragdoll->skeleton);
    if (bone_count_i64 <= 0 || bone_count_i64 > INT32_MAX ||
        (uint64_t)bone_count_i64 > SIZE_MAX / (16u * sizeof(double)))
        return 0;
    int32_t bone_count = (int32_t)bone_count_i64;
    ragdoll->skeleton_bone_count = bone_count;

    /* Bind globals (model space), composed parent-first. */
    double *bind_globals = (double *)malloc((size_t)bone_count * 16 * sizeof(double));
    double *bind_local = (double *)malloc(16 * sizeof(double));
    int32_t *first_child = (int32_t *)malloc((size_t)bone_count * sizeof(int32_t));
    double *lengths = (double *)malloc((size_t)bone_count * sizeof(double));
    if (!bind_globals || !bind_local || !first_child || !lengths) {
        free(bind_globals);
        free(bind_local);
        free(first_child);
        free(lengths);
        return 0;
    }
    for (int32_t i = 0; i < bone_count; ++i)
        first_child[i] = -1;
    for (int32_t i = 0; i < bone_count; ++i) {
        int64_t parent = rt_skeleton3d_get_bone_parent_raw(ragdoll->skeleton, i);
        if (!rt_skeleton3d_get_bone_bind_local_raw(ragdoll->skeleton, i, bind_local))
            rg_mat_identity(bind_local);
        if (parent >= 0 && parent < i) {
            rg_mat_mul(
                &bind_globals[(size_t)parent * 16u], bind_local, &bind_globals[(size_t)i * 16u]);
        } else {
            memcpy(&bind_globals[(size_t)i * 16u], bind_local, 16 * sizeof(double));
        }
        if (!rg_mat_is_finite(&bind_globals[(size_t)i * 16u]))
            goto fail;
    }
    /* Bone segment direction follows the farthest direct child. Choosing the first authored
     * child made branched hips/shoulders depend on insertion order and often selected a tiny
     * helper bone instead of the anatomical segment. */
    for (int32_t i = 0; i < bone_count; ++i) {
        lengths[i] = 0.0;
        first_child[i] = -1;
    }
    for (int32_t child_index = 0; child_index < bone_count; ++child_index) {
        int64_t parent = rt_skeleton3d_get_bone_parent_raw(ragdoll->skeleton, child_index);
        if (parent >= 0 && parent < bone_count) {
            const double *self = &bind_globals[(size_t)parent * 16u];
            const double *child = &bind_globals[(size_t)child_index * 16u];
            double dx = child[3] - self[3];
            double dy = child[7] - self[7];
            double dz = child[11] - self[11];
            double distance = hypot(hypot(dx, dy), dz);
            if (isfinite(distance) && distance > 1e-9 &&
                (first_child[parent] < 0 || distance > lengths[parent])) {
                first_child[parent] = child_index;
                lengths[parent] = distance;
            }
        }
    }
    for (int32_t i = 0; i < bone_count; ++i) {
        if (first_child[i] < 0)
            lengths[i] = RAGDOLL3D_TERMINAL_LENGTH;
    }

    /* Select bodied bones. */
    ragdoll->bone_to_slot = (int32_t *)malloc((size_t)bone_count * sizeof(int32_t));
    if (!ragdoll->bone_to_slot)
        goto fail;
    ragdoll->bone_capacity = bone_count;
    for (int32_t i = 0; i < bone_count; ++i)
        ragdoll->bone_to_slot[i] = -1;
    int32_t selected = 0;
    for (int32_t i = 0; i < bone_count && selected < RAGDOLL3D_MAX_BODIES; ++i) {
        int64_t parent = rt_skeleton3d_get_bone_parent_raw(ragdoll->skeleton, i);
        int is_root = parent < 0;
        if (lengths[i] >= ragdoll->min_bone_length || is_root)
            ragdoll->bone_to_slot[i] = selected++;
    }
    if (selected <= 0)
        goto fail;
    ragdoll->slots = (rt_ragdoll3d_slot *)calloc((size_t)selected, sizeof(rt_ragdoll3d_slot));
    if (!ragdoll->slots)
        goto fail;
    ragdoll->slot_count = selected;
    ragdoll->slot_capacity = selected;

    /* Volumes for mass distribution. */
    double total_volume = 0.0;
    for (int32_t i = 0; i < bone_count; ++i) {
        int32_t slot_index = ragdoll->bone_to_slot[i];
        if (slot_index < 0)
            continue;
        rt_ragdoll3d_slot *slot = &ragdoll->slots[slot_index];
        slot->bone_index = i;
        slot->length = lengths[i] > 1e-6 ? lengths[i] : RAGDOLL3D_TERMINAL_LENGTH;
        slot->radius = ragdoll->radius_scale * slot->length;
        if (slot->radius < 0.02)
            slot->radius = 0.02;
        if (slot->radius > slot->length)
            slot->radius = slot->length;
        slot->swing_deg = RAGDOLL3D_DEFAULT_SWING_DEG;
        slot->twist_deg = RAGDOLL3D_DEFAULT_TWIST_DEG;
        memcpy(slot->bind_global, &bind_globals[(size_t)i * 16u], 16 * sizeof(double));
        /* Nearest bodied ancestor. */
        slot->parent_slot = -1;
        int64_t ancestor = rt_skeleton3d_get_bone_parent_raw(ragdoll->skeleton, i);
        int32_t ancestor_steps = 0;
        while (ancestor >= 0 && ancestor < bone_count && ancestor_steps++ < bone_count) {
            if (ragdoll->bone_to_slot[ancestor] >= 0) {
                slot->parent_slot = ragdoll->bone_to_slot[ancestor];
                break;
            }
            ancestor = rt_skeleton3d_get_bone_parent_raw(ragdoll->skeleton, ancestor);
        }
        double volume = slot->radius * slot->radius * slot->length;
        if (!isfinite(volume) || volume <= 0.0)
            goto fail;
        total_volume += volume;
    }
    if (!isfinite(total_volume) || total_volume <= 1e-12)
        goto fail;

    /* Bodies: capsule along the bone direction, posed at bind (model space). */
    for (int32_t s = 0; s < ragdoll->slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        const double *global = slot->bind_global;
        double origin[3] = {global[3], global[7], global[11]};
        double dir[3] = {0.0, 1.0, 0.0};
        int32_t child = first_child[slot->bone_index];
        if (child >= 0) {
            const double *child_global = &bind_globals[(size_t)child * 16u];
            dir[0] = child_global[3] - origin[0];
            dir[1] = child_global[7] - origin[1];
            dir[2] = child_global[11] - origin[2];
        } else {
            /* Terminal: extend along the bone's own +Y basis column. */
            dir[0] = global[1];
            dir[1] = global[5];
            dir[2] = global[9];
        }
        if (!rg_vec3_normalize(dir)) {
            dir[0] = 0.0;
            dir[1] = 1.0;
            dir[2] = 0.0;
        }

        double center[3] = {origin[0] + dir[0] * slot->length * 0.5,
                            origin[1] + dir[1] * slot->length * 0.5,
                            origin[2] + dir[2] * slot->length * 0.5};
        double body_quat[4];
        rg_quat_align_y(dir, body_quat);

        double height = slot->length + slot->radius * 2.0;
        double mass =
            ragdoll->total_mass * (slot->radius * slot->radius * slot->length) / total_volume;
        if (!isfinite(height) || height <= 0.0 || !isfinite(mass) || mass <= 0.0)
            goto fail;
        slot->body = rt_body3d_new_capsule(slot->radius, height, mass);
        if (!slot->body)
            goto fail;
        rt_body3d_set_position(slot->body, center[0], center[1], center[2]);
        {
            void *q = rt_quat_new(body_quat[0], body_quat[1], body_quat[2], body_quat[3]);
            if (!q)
                goto fail;
            rt_body3d_set_orientation(slot->body, q);
            ragdoll3d_release_obj(&q);
        }
        rt_body3d_set_linear_damping(slot->body, 0.05);
        rt_body3d_set_angular_damping(slot->body, 0.1);
        /* CCD deliberately OFF: its conservative bounding-sphere sweep makes
         * thin horizontal limbs hover above surfaces. Rig capsules rely on the
         * contact solver; practical activation heights keep impact speeds well
         * below the tunneling bound. */
        rt_body3d_set_use_ccd(slot->body, 0);
        rt_body3d_set_collision_layer(slot->body, RAGDOLL3D_COLLISION_LAYER);
        rt_body3d_set_collision_mask(slot->body, ~RAGDOLL3D_COLLISION_LAYER);

        /* Body pose relative to the bone (frames for write-back). */
        double bone_quat[4];
        rg_quat_from_mat(global, bone_quat);
        double inv_bone_quat[4];
        rg_quat_conj(bone_quat, inv_bone_quat);
        double offset_model[3] = {
            center[0] - origin[0], center[1] - origin[1], center[2] - origin[2]};
        rg_quat_rotate(inv_bone_quat, offset_model, slot->body_offset_bone);
        rg_quat_mul(inv_bone_quat, body_quat, slot->body_to_bone_quat);
        rg_quat_normalize(slot->body_to_bone_quat);
    }

    /* Joints: child body <-> parent body anchored at the child bone origin. */
    for (int32_t s = 0; s < ragdoll->slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        if (slot->parent_slot < 0)
            continue;
        rt_ragdoll3d_slot *parent = &ragdoll->slots[slot->parent_slot];
        double anchor_world[3] = {
            slot->bind_global[3], slot->bind_global[7], slot->bind_global[11]};
        /* Body-local anchors at bind. */
        void *frames[2] = {NULL, NULL};
        rt_ragdoll3d_slot *ends[2];
        ends[0] = slot;
        ends[1] = parent;
        for (int e = 0; e < 2; ++e) {
            void *body_pos_vec = rt_body3d_get_position(ends[e]->body);
            if (!body_pos_vec) {
                ragdoll3d_release_obj(&frames[0]);
                ragdoll3d_release_obj(&frames[1]);
                goto fail;
            }
            double body_pos[3] = {
                rt_vec3_x(body_pos_vec), rt_vec3_y(body_pos_vec), rt_vec3_z(body_pos_vec)};
            ragdoll3d_release_obj(&body_pos_vec);
            double body_quat[4];
            {
                double bone_quat[4];
                rg_quat_from_mat(ends[e]->bind_global, bone_quat);
                rg_quat_mul(bone_quat, ends[e]->body_to_bone_quat, body_quat);
                rg_quat_normalize(body_quat);
            }
            double inv_body_quat[4];
            rg_quat_conj(body_quat, inv_body_quat);
            double rel[3] = {anchor_world[0] - body_pos[0],
                             anchor_world[1] - body_pos[1],
                             anchor_world[2] - body_pos[2]};
            double anchor_local[3];
            rg_quat_rotate(inv_body_quat, rel, anchor_local);
            frames[e] = rt_mat4_new(1.0,
                                    0.0,
                                    0.0,
                                    anchor_local[0],
                                    0.0,
                                    1.0,
                                    0.0,
                                    anchor_local[1],
                                    0.0,
                                    0.0,
                                    1.0,
                                    anchor_local[2],
                                    0.0,
                                    0.0,
                                    0.0,
                                    1.0);
            if (!frames[e]) {
                ragdoll3d_release_obj(&frames[0]);
                ragdoll3d_release_obj(&frames[1]);
                goto fail;
            }
        }
        slot->joint = rt_sixdof_joint3d_new(slot->body, parent->body, frames[0], frames[1]);
        ragdoll3d_release_obj(&frames[0]);
        ragdoll3d_release_obj(&frames[1]);
        if (!slot->joint)
            goto fail;
        double swing = slot->swing_deg * (3.14159265358979323846 / 180.0);
        double twist = slot->twist_deg * (3.14159265358979323846 / 180.0);
        void *min_v = rt_vec3_new(-swing, -twist, -swing);
        void *max_v = rt_vec3_new(swing, twist, swing);
        if (!min_v || !max_v) {
            ragdoll3d_release_obj(&max_v);
            ragdoll3d_release_obj(&min_v);
            goto fail;
        }
        rt_sixdof_joint3d_set_angular_limits(slot->joint, min_v, max_v);
        ragdoll3d_release_obj(&max_v);
        ragdoll3d_release_obj(&min_v);
    }

    ragdoll->override_globals = (float *)calloc((size_t)bone_count * 16, sizeof(float));
    ragdoll->override_mask = (int8_t *)calloc((size_t)bone_count, 1);
    if (!ragdoll->override_globals || !ragdoll->override_mask)
        goto fail;

    free(bind_globals);
    free(bind_local);
    free(first_child);
    free(lengths);
    ragdoll->built = 1;
    return 1;

fail:
    free(bind_globals);
    free(bind_local);
    free(first_child);
    free(lengths);
    ragdoll3d_release_rig(ragdoll);
    return 0;
}

//=========================================================================
// Public API
//=========================================================================

/// @brief `Ragdoll3D.FromSkeleton(skeleton)` — rig description with defaults;
///   bodies build lazily on first BodyCount/Activate. See header.
/// @param skeleton Borrowed `Skeleton3D` retained for the ragdoll's lifetime.
/// @return Owned `Ragdoll3D` handle, or null after trapping on invalid input or allocation failure.
void *rt_ragdoll3d_from_skeleton(void *skeleton) {
    if (!rt_g3d_has_class(skeleton, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_trap("Ragdoll3D.FromSkeleton: skeleton must be Skeleton3D");
        return NULL;
    }
    rt_ragdoll3d *ragdoll =
        (rt_ragdoll3d *)rt_obj_new_i64(RT_G3D_RAGDOLL3D_CLASS_ID, (int64_t)sizeof(*ragdoll));
    if (!ragdoll) {
        rt_trap("Ragdoll3D.FromSkeleton: allocation failed");
        return NULL;
    }
    memset(ragdoll, 0, sizeof(*ragdoll));
    rt_obj_set_finalizer(ragdoll, ragdoll3d_finalize);
    ragdoll->skeleton = skeleton;
    rt_obj_retain_maybe(skeleton);
    ragdoll->total_mass = RAGDOLL3D_DEFAULT_TOTAL_MASS;
    ragdoll->radius_scale = RAGDOLL3D_DEFAULT_RADIUS_SCALE;
    ragdoll->min_bone_length = RAGDOLL3D_DEFAULT_MIN_BONE_LENGTH;
    ragdoll->powered_stiffness = 0.0;
    return ragdoll;
}

/// @brief Returns the configured aggregate ragdoll mass.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @return Positive total mass in kilograms, or zero for an invalid handle.
double rt_ragdoll3d_get_total_mass(void *obj) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.get_TotalMass: invalid ragdoll");
    return ragdoll ? ragdoll->total_mass : 0.0;
}

/// @brief Configures aggregate mass and invalidates any inactive lazy rig.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param mass Positive finite total mass in kilograms no greater than 1e9; invalid values are
///        ignored.
/// @note Reconfiguration while active traps and leaves the existing rig unchanged.
void rt_ragdoll3d_set_total_mass(void *obj, double mass) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.set_TotalMass: invalid ragdoll");
    if (!ragdoll || !isfinite(mass) || mass <= 0.0 || mass > RAGDOLL3D_PARAM_MAX)
        return;
    if (ragdoll->active) {
        rt_trap("Ragdoll3D.set_TotalMass: configure before Activate");
        return;
    }
    if (ragdoll->total_mass == mass)
        return;
    ragdoll->total_mass = mass;
    ragdoll3d_release_rig(ragdoll);
}

/// @brief Returns the capsule-radius-to-bone-length scale.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @return Positive configured radius scale, or zero for an invalid handle.
double rt_ragdoll3d_get_radius_scale(void *obj) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.get_RadiusScale: invalid ragdoll");
    return ragdoll ? ragdoll->radius_scale : 0.0;
}

/// @brief Configures capsule radius scale and invalidates any inactive lazy rig.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param scale Positive finite scale no greater than 2; invalid values are ignored.
/// @note Reconfiguration while active traps and leaves the existing rig unchanged.
void rt_ragdoll3d_set_radius_scale(void *obj, double scale) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.set_RadiusScale: invalid ragdoll");
    if (!ragdoll || !isfinite(scale) || scale <= 0.0 || scale > 2.0)
        return;
    if (ragdoll->active) {
        rt_trap("Ragdoll3D.set_RadiusScale: configure before Activate");
        return;
    }
    if (ragdoll->radius_scale == scale)
        return;
    ragdoll->radius_scale = scale;
    ragdoll3d_release_rig(ragdoll);
}

/// @brief Returns the minimum segment length eligible for a non-root rig body.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @return Positive minimum length in model units, or zero for an invalid handle.
double rt_ragdoll3d_get_min_bone_length(void *obj) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.get_MinBoneLength: invalid ragdoll");
    return ragdoll ? ragdoll->min_bone_length : 0.0;
}

/// @brief Configures the body-selection length threshold and invalidates the inactive rig.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param length Positive finite minimum bone length in model units no greater than 1e9; invalid
///        values are ignored.
/// @note Reconfiguration while active traps and leaves the existing rig unchanged.
void rt_ragdoll3d_set_min_bone_length(void *obj, double length) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.set_MinBoneLength: invalid ragdoll");
    if (!ragdoll || !isfinite(length) || length <= 0.0 || length > RAGDOLL3D_PARAM_MAX)
        return;
    if (ragdoll->active) {
        rt_trap("Ragdoll3D.set_MinBoneLength: configure before Activate");
        return;
    }
    if (ragdoll->min_bone_length == length)
        return;
    ragdoll->min_bone_length = length;
    ragdoll3d_release_rig(ragdoll);
}

/// @brief Returns the number of lazily constructed capsule bodies.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @return Built slot count, or zero for an invalid handle or failed rig build.
int64_t rt_ragdoll3d_get_body_count(void *obj) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.get_BodyCount: invalid ragdoll");
    if (!ragdoll)
        return 0;
    ragdoll3d_ensure_built(ragdoll);
    return ragdoll3d_safe_slot_count(ragdoll);
}

/// @brief Reports whether the rig is currently registered in a physics world.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @return `1` while active; otherwise `0`.
int8_t rt_ragdoll3d_get_active(void *obj) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.get_Active: invalid ragdoll");
    return ragdoll ? ragdoll->active : 0;
}

/// @brief Override the joint limits of one bone (before Activate).
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param bone_name Borrowed skeleton bone name; unknown or unbodied names trap.
/// @param swing_deg Swing limit in degrees, clamped to `[0, 180]`; non-finite values use default.
/// @param twist_deg Twist limit in degrees, clamped to `[0, 180]`; non-finite values use default.
void rt_ragdoll3d_set_joint_limits(void *obj,
                                   rt_string bone_name,
                                   double swing_deg,
                                   double twist_deg) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.SetJointLimits: invalid ragdoll");
    if (!ragdoll)
        return;
    if (ragdoll->active) {
        rt_trap("Ragdoll3D.SetJointLimits: configure before Activate");
        return;
    }
    if (!ragdoll3d_ensure_built(ragdoll))
        return;
    int64_t bone = rt_skeleton3d_find_bone(ragdoll->skeleton, bone_name);
    if (bone < 0 || bone >= ragdoll->skeleton_bone_count || ragdoll->bone_to_slot[bone] < 0) {
        rt_trap("Ragdoll3D.SetJointLimits: unknown or unbodied bone name");
        return;
    }
    int32_t slot_index = ragdoll->bone_to_slot[bone];
    int32_t slot_count = ragdoll3d_safe_slot_count(ragdoll);
    if (slot_index < 0 || slot_index >= slot_count) {
        rt_trap("Ragdoll3D.SetJointLimits: corrupt bone mapping");
        return;
    }
    rt_ragdoll3d_slot *slot = &ragdoll->slots[slot_index];
    double next_swing =
        isfinite(swing_deg) ? fmax(0.0, fmin(180.0, swing_deg)) : RAGDOLL3D_DEFAULT_SWING_DEG;
    double next_twist =
        isfinite(twist_deg) ? fmax(0.0, fmin(180.0, twist_deg)) : RAGDOLL3D_DEFAULT_TWIST_DEG;
    if (slot->joint) {
        double swing = next_swing * (3.14159265358979323846 / 180.0);
        double twist = next_twist * (3.14159265358979323846 / 180.0);
        void *min_v = rt_vec3_new(-swing, -twist, -swing);
        void *max_v = rt_vec3_new(swing, twist, swing);
        if (!min_v || !max_v) {
            ragdoll3d_release_obj(&max_v);
            ragdoll3d_release_obj(&min_v);
            rt_trap("Ragdoll3D.SetJointLimits: allocation failed");
            return;
        }
        rt_sixdof_joint3d_set_angular_limits(slot->joint, min_v, max_v);
        ragdoll3d_release_obj(&max_v);
        ragdoll3d_release_obj(&min_v);
    }
    slot->swing_deg = next_swing;
    slot->twist_deg = next_twist;
}

/// @brief Read the node's world pose (position + rotation quaternion).
/// @param node Borrowed `SceneNode3D`; null produces identity outputs and failure.
/// @param[out] out_pos Three-element world-position destination initialized to zero.
/// @param[out] out_quat Four-element xyzw world-rotation destination initialized to identity.
/// @return `1` when world position was read; `0` when @p node is null or position lookup fails.
static int ragdoll3d_node_pose(void *node, double out_pos[3], double out_quat[4]) {
    out_pos[0] = out_pos[1] = out_pos[2] = 0.0;
    out_quat[0] = out_quat[1] = out_quat[2] = 0.0;
    out_quat[3] = 1.0;
    if (!node)
        return 0;
    if (!rt_scene_node3d_get_world_position_components(node, &out_pos[0], &out_pos[1], &out_pos[2]))
        return 0;
    (void)rt_scene_node3d_get_world_rotation_components(
        node, &out_quat[0], &out_quat[1], &out_quat[2], &out_quat[3]);
    return 1;
}

/// @brief Reconstruct a bone's model-space global from a skin palette entry:
///   global = skin × bindGlobal (the palette stores global × inverse-bind).
/// @param skin_entry Borrowed 16-element row-major skin matrix.
/// @param bind_global Borrowed 16-element row-major bone bind-global matrix.
/// @param[out] out16 Writable 16-element reconstructed global matrix.
static void ragdoll3d_global_from_skin(const float *skin_entry,
                                       const double *bind_global,
                                       double out16[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out16[r * 4 + c] = (double)skin_entry[r * 4 + 0] * bind_global[0 * 4 + c] +
                               (double)skin_entry[r * 4 + 1] * bind_global[1 * 4 + c] +
                               (double)skin_entry[r * 4 + 2] * bind_global[2 * 4 + c] +
                               (double)skin_entry[r * 4 + 3] * bind_global[3 * 4 + c];
}

/// @brief Model-space pose of a rig bone from the live animated controller.
/// @details Reads the CURRENT skin palette and multiplies the bind global back
///   in, so "current" and "previous" poses come from the same representation
///   (an identity palette means the bind pose — velocity seeding stays sane
///   for empty/static clips).
/// @param controller Borrowed animation controller providing the current final palette.
/// @param slot Borrowed rig slot identifying the bone and its bind-global matrix.
/// @param[out] out_pos Three-element model-space bone translation.
/// @param[out] out_quat Four-element normalized xyzw model-space bone rotation.
/// @return `1` when the palette contains the requested bone; otherwise `0`.
static int ragdoll3d_animated_bone_pose_slot(void *controller,
                                             const rt_ragdoll3d_slot *slot,
                                             double out_pos[3],
                                             double out_quat[4]) {
    int32_t bone_count = 0;
    const float *skin = rt_anim_controller3d_get_final_palette_data(controller, &bone_count);
    if (!skin || !slot || slot->bone_index < 0 || slot->bone_index >= bone_count)
        return 0;
    double global[16];
    ragdoll3d_global_from_skin(&skin[slot->bone_index * 16], slot->bind_global, global);
    if (!rg_mat_is_finite(global))
        return 0;
    out_pos[0] = global[3];
    out_pos[1] = global[7];
    out_pos[2] = global[11];
    rg_quat_from_mat(global, out_quat);
    return 1;
}

static void ragdoll3d_step_active(rt_ragdoll3d *ragdoll, double dt);

/// @brief `Ragdoll3D.Activate(world, controller, node)` — anim → ragdoll handoff.
/// @details Builds the rig if necessary, transforms each animated bone and finite-difference
///          velocity into world space, registers bodies and joints, then retains the world,
///          animation controller, and scene node until deactivation/blend completion.
/// @param obj Borrowed inactive `Ragdoll3D` runtime object.
/// @param world Borrowed `World3D` that receives all rig bodies and joints.
/// @param controller Borrowed `AnimController3D` supplying current and previous palettes.
/// @param node Borrowed `SceneNode3D` defining the model-to-world root transform.
void rt_ragdoll3d_activate(void *obj, void *world, void *controller, void *node) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.Activate: invalid ragdoll");
    void *added_bodies[RAGDOLL3D_MAX_BODIES];
    void *added_joints[RAGDOLL3D_MAX_BODIES];
    int32_t added_body_count = 0;
    int32_t added_joint_count = 0;
    int32_t slot_count;
    if (!ragdoll)
        return;
    if (ragdoll->active)
        return;
    if (!rt_g3d_has_class(world, RT_G3D_WORLD3D_CLASS_ID)) {
        rt_trap("Ragdoll3D.Activate: world must be Physics3DWorld");
        return;
    }
    if (!rt_g3d_has_class(controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
        rt_trap("Ragdoll3D.Activate: controller must be AnimController3D");
        return;
    }
    if (!rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID)) {
        rt_trap("Ragdoll3D.Activate: node must be SceneNode3D");
        return;
    }
    if (rt_anim_controller3d_get_skeleton(controller) != ragdoll->skeleton) {
        rt_trap("Ragdoll3D.Activate: controller must use the ragdoll skeleton");
        return;
    }
    if (!ragdoll3d_ensure_built(ragdoll)) {
        rt_trap("Ragdoll3D.Activate: rig build failed (empty skeleton?)");
        return;
    }
    slot_count = ragdoll3d_safe_slot_count(ragdoll);
    if (slot_count <= 0 || ragdoll3d_safe_bone_count(ragdoll) <= 0) {
        rt_trap("Ragdoll3D.Activate: corrupt rig storage");
        return;
    }
    for (int32_t s = 0; s < slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        rt_body3d *body;
        if (!rt_g3d_has_class(slot->body, RT_G3D_BODY3D_CLASS_ID) || slot->bone_index < 0 ||
            slot->bone_index >= ragdoll->skeleton_bone_count ||
            (slot->joint && !rt_g3d_has_class(slot->joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID))) {
            rt_trap("Ragdoll3D.Activate: corrupt rig objects");
            return;
        }
        body = (rt_body3d *)slot->body;
        if (body->owner_world) {
            rt_trap("Ragdoll3D.Activate: rig body already belongs to a world");
            return;
        }
    }
    double node_pos[3];
    double node_quat[4];
    if (!ragdoll3d_node_pose(node, node_pos, node_quat)) {
        rt_trap("Ragdoll3D.Activate: node world pose is unavailable");
        return;
    }

    /* Previous-frame globals for velocity seeding: prev_skin × bind_global. */
    int32_t prev_bones = 0;
    const float *prev_skin =
        rt_anim_controller3d_get_previous_palette_data(controller, &prev_bones);

    for (int32_t s = 0; s < slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        double bone_pos[3];
        double bone_quat[4];
        if (!ragdoll3d_animated_bone_pose_slot(controller, slot, bone_pos, bone_quat)) {
            /* Fall back to bind. */
            bone_pos[0] = slot->bind_global[3];
            bone_pos[1] = slot->bind_global[7];
            bone_pos[2] = slot->bind_global[11];
            rg_quat_from_mat(slot->bind_global, bone_quat);
        }
        /* Body pose in world space: node × bone × frame. */
        double body_quat_model[4];
        rg_quat_mul(bone_quat, slot->body_to_bone_quat, body_quat_model);
        double body_quat_world[4];
        rg_quat_mul(node_quat, body_quat_model, body_quat_world);
        rg_quat_normalize(body_quat_world);
        double offset_model[3];
        rg_quat_rotate(bone_quat, slot->body_offset_bone, offset_model);
        double center_model[3] = {bone_pos[0] + offset_model[0],
                                  bone_pos[1] + offset_model[1],
                                  bone_pos[2] + offset_model[2]};
        double center_world[3];
        rg_quat_rotate(node_quat, center_model, center_world);
        center_world[0] += node_pos[0];
        center_world[1] += node_pos[1];
        center_world[2] += node_pos[2];

        rt_body3d_set_position(slot->body, center_world[0], center_world[1], center_world[2]);
        {
            void *q = rt_quat_new(
                body_quat_world[0], body_quat_world[1], body_quat_world[2], body_quat_world[3]);
            if (!q) {
                rt_trap("Ragdoll3D.Activate: orientation allocation failed");
                return;
            }
            rt_body3d_set_orientation(slot->body, q);
            ragdoll3d_release_obj(&q);
        }

        /* Velocity seeding from the previous palette (model-space finite diff). */
        rt_body3d_set_velocity(slot->body, 0.0, 0.0, 0.0);
        rt_body3d_set_angular_velocity(slot->body, 0.0, 0.0, 0.0);
        if (prev_skin && slot->bone_index < prev_bones) {
            double prev_global[16];
            ragdoll3d_global_from_skin(
                &prev_skin[slot->bone_index * 16], slot->bind_global, prev_global);
            if (rg_mat_is_finite(prev_global)) {
                double prev_quat[4];
                double prev_offset_model[3];
                double prev_center_model[3];
                double prev_pos_model[3] = {prev_global[3], prev_global[7], prev_global[11]};
                rg_quat_from_mat(prev_global, prev_quat);
                rg_quat_rotate(prev_quat, slot->body_offset_bone, prev_offset_model);
                prev_center_model[0] = prev_pos_model[0] + prev_offset_model[0];
                prev_center_model[1] = prev_pos_model[1] + prev_offset_model[1];
                prev_center_model[2] = prev_pos_model[2] + prev_offset_model[2];
                double vel_model[3] = {
                    (center_model[0] - prev_center_model[0]) / RAGDOLL3D_HANDOFF_DT,
                    (center_model[1] - prev_center_model[1]) / RAGDOLL3D_HANDOFF_DT,
                    (center_model[2] - prev_center_model[2]) / RAGDOLL3D_HANDOFF_DT};
                double vel_world[3];
                rg_quat_rotate(node_quat, vel_model, vel_world);
                rt_body3d_set_velocity(slot->body, vel_world[0], vel_world[1], vel_world[2]);
                double rotvec[3];
                rg_quat_delta_rotvec(prev_quat, bone_quat, rotvec);
                double ang_model[3] = {rotvec[0] / RAGDOLL3D_HANDOFF_DT,
                                       rotvec[1] / RAGDOLL3D_HANDOFF_DT,
                                       rotvec[2] / RAGDOLL3D_HANDOFF_DT};
                double ang_world[3];
                rg_quat_rotate(node_quat, ang_model, ang_world);
                rt_body3d_set_angular_velocity(
                    slot->body, ang_world[0], ang_world[1], ang_world[2]);
            }
        }
    }

    for (int32_t s = 0; s < slot_count; ++s) {
        if (!rt_world3d_try_add(world, ragdoll->slots[s].body))
            goto registration_failed;
        added_bodies[added_body_count++] = ragdoll->slots[s].body;
        rt_body3d_wake(ragdoll->slots[s].body);
    }
    for (int32_t s = 0; s < slot_count; ++s) {
        if (ragdoll->slots[s].joint) {
            int64_t before = rt_world3d_joint_count(world);
            rt_world3d_add_joint(world, ragdoll->slots[s].joint, RT_JOINT_SIXDOF);
            if (rt_world3d_joint_count(world) != before + 1)
                goto registration_failed;
            added_joints[added_joint_count++] = ragdoll->slots[s].joint;
        }
    }

    rt_obj_retain_maybe(world);
    rt_obj_retain_maybe(controller);
    rt_obj_retain_maybe(node);
    ragdoll3d_release_typed(&ragdoll->world, RT_G3D_WORLD3D_CLASS_ID);
    ragdoll3d_release_typed(&ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    ragdoll3d_release_typed(&ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID);
    ragdoll->world = world;
    ragdoll->controller = controller;
    ragdoll->node = node;
    ragdoll->blend_remaining = 0.0;
    free(ragdoll->blend_from);
    ragdoll->blend_from = NULL;
    ragdoll->blend_bone_capacity = 0;
    ragdoll->override_ready = 0;
    ragdoll->active = 1;
    return;

registration_failed:
    while (added_joint_count > 0)
        rt_world3d_remove_joint(world, added_joints[--added_joint_count]);
    while (added_body_count > 0)
        rt_world3d_remove(world, added_bodies[--added_body_count]);
    rt_trap("Ragdoll3D.Activate: could not register the complete rig");
}

/// @brief `Ragdoll3D.Deactivate(blendSeconds)` — remove the rig from the world
///   and blend the palette back to live animation.
/// @details Captures the last physics override when a positive finite blend is requested, removes
///          joints before bodies, and releases the world immediately. Controller and node
///          references remain retained only until an active blend finishes.
/// @param obj Borrowed active `Ragdoll3D` runtime object.
/// @param blend_seconds Positive finite animation blend duration in seconds, clamped to 1e9, or a
///        non-positive value for immediate release.
void rt_ragdoll3d_deactivate(void *obj, double blend_seconds) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.Deactivate: invalid ragdoll");
    if (!ragdoll || !ragdoll->active)
        return;
    /* Publish the current body pose before capture when callers deactivate without an active
     * Step. Otherwise the zero-filled build scratch becomes the blend source. */
    if (!ragdoll->override_ready &&
        rt_g3d_has_class(ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID) &&
        rt_g3d_has_class(ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID))
        ragdoll3d_step_active(ragdoll, 0.0);

    /* Capture the current ragdoll override for the blend. */
    int32_t bone_count = 0;
    const float *palette = NULL;
    if (rt_g3d_has_class(ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID))
        palette = rt_anim_controller3d_get_final_palette_data(ragdoll->controller, &bone_count);
    free(ragdoll->blend_from);
    ragdoll->blend_from = NULL;
    ragdoll->blend_bone_capacity = 0;
    ragdoll->blend_duration = 0.0;
    ragdoll->blend_remaining = 0.0;
    if (isfinite(blend_seconds) && blend_seconds > 0.0 && palette && ragdoll->override_ready &&
        ragdoll3d_safe_bone_count(ragdoll) == bone_count) {
        double duration = fmin(blend_seconds, RAGDOLL3D_PARAM_MAX);
        ragdoll->blend_from =
            (float *)malloc((size_t)ragdoll->skeleton_bone_count * 16 * sizeof(float));
        if (ragdoll->blend_from) {
            memcpy(ragdoll->blend_from,
                   ragdoll->override_globals,
                   (size_t)ragdoll->skeleton_bone_count * 16 * sizeof(float));
            ragdoll->blend_bone_capacity = bone_count;
            ragdoll->blend_duration = duration;
            ragdoll->blend_remaining = duration;
        } else {
            rt_trap("Ragdoll3D.Deactivate: blend allocation failed");
        }
    }

    ragdoll3d_unregister(ragdoll);
    ragdoll3d_release_typed(&ragdoll->world, RT_G3D_WORLD3D_CLASS_ID);
    ragdoll->active = 0;
    if (ragdoll->blend_remaining <= 0.0) {
        ragdoll3d_release_typed(&ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
        ragdoll3d_release_typed(&ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID);
    }
}

/// @brief Enable powered (PD-driven) ragdoll bones. @p bone_mask bit N selects
///   skeleton bone N (bones 0..63; higher bones wrap via the 64-bit mask). Bones
///   without a rig body are ignored. @p stiffness is the PD drive gain (0 disables).
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param bone_mask Stable skeleton-index mask selecting powered bones.
/// @param stiffness Finite non-negative drive gain, clamped to 100; invalid values disable drive.
void rt_ragdoll3d_set_powered(void *obj, int64_t bone_mask, double stiffness) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.SetPowered: invalid ragdoll");
    if (!ragdoll)
        return;
    ragdoll->powered_mask = bone_mask;
    ragdoll->powered_stiffness =
        isfinite(stiffness) && stiffness >= 0.0 ? fmin(stiffness, 100.0) : 0.0;
}

/// @brief Borrowed rig body for a bone name (NULL when unmapped).
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param bone_name Borrowed skeleton bone name.
/// @return Borrowed `Body3D` for the named bodied bone, or null for build failure, an unknown name,
///         or a skeleton bone excluded from the rig.
void *rt_ragdoll3d_get_body(void *obj, rt_string bone_name) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.GetBody: invalid ragdoll");
    if (!ragdoll || !ragdoll3d_ensure_built(ragdoll))
        return NULL;
    int64_t bone = rt_skeleton3d_find_bone(ragdoll->skeleton, bone_name);
    if (bone < 0 || bone >= ragdoll3d_safe_bone_count(ragdoll))
        return NULL;
    int32_t slot_index = ragdoll->bone_to_slot[bone];
    if (slot_index < 0 || slot_index >= ragdoll3d_safe_slot_count(ragdoll))
        return NULL;
    return rt_g3d_has_class(ragdoll->slots[slot_index].body, RT_G3D_BODY3D_CLASS_ID)
               ? ragdoll->slots[slot_index].body
               : NULL;
}

/// @brief Active-mode sync: powered drive, palette write-back, node root-follow.
/// @details Converts each body pose through its fixed frame into model-space bone globals, applies
///          optional angular PD impulses toward animation, rebases override translations while the
///          scene node follows the root, and publishes the masked pose override.
/// @param[in,out] ragdoll Borrowed active rig with retained controller and node.
/// @param dt Positive step duration in seconds used to scale powered-drive impulses.
static void ragdoll3d_step_active(rt_ragdoll3d *ragdoll, double dt) {
    int32_t bone_count = ragdoll3d_safe_bone_count(ragdoll);
    int32_t slot_count = ragdoll3d_safe_slot_count(ragdoll);
    if (bone_count <= 0 || slot_count <= 0 ||
        !rt_g3d_has_class(ragdoll->world, RT_G3D_WORLD3D_CLASS_ID) ||
        !rt_g3d_has_class(ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID) ||
        !rt_g3d_has_class(ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID) ||
        rt_anim_controller3d_get_skeleton(ragdoll->controller) != ragdoll->skeleton)
        return;
    for (int32_t s = 0; s < slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        if (!rt_g3d_has_class(slot->body, RT_G3D_BODY3D_CLASS_ID) || slot->bone_index < 0 ||
            slot->bone_index >= bone_count || !rg_mat_is_finite(slot->bind_global) ||
            !isfinite(slot->body_offset_bone[0]) || !isfinite(slot->body_offset_bone[1]) ||
            !isfinite(slot->body_offset_bone[2]) || !isfinite(slot->body_to_bone_quat[0]) ||
            !isfinite(slot->body_to_bone_quat[1]) || !isfinite(slot->body_to_bone_quat[2]) ||
            !isfinite(slot->body_to_bone_quat[3]) ||
            !rt_world3d_contains_body(ragdoll->world, slot->body))
            return;
    }
    double node_pos[3];
    double node_quat[4];
    if (!ragdoll3d_node_pose(ragdoll->node, node_pos, node_quat))
        return;
    double inv_node_quat[4];
    rg_quat_conj(node_quat, inv_node_quat);

    memset(ragdoll->override_mask, 0, (size_t)bone_count);

    double root_delta_model[3] = {0.0, 0.0, 0.0};
    int have_root_delta = 0;

    double powered_stiffness =
        isfinite(ragdoll->powered_stiffness) && ragdoll->powered_stiffness >= 0.0
            ? fmin(ragdoll->powered_stiffness, 100.0)
            : 0.0;
    ragdoll->powered_stiffness = powered_stiffness;
    for (int32_t s = 0; s < slot_count; ++s) {
        rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
        double body_pos[3];
        double body_quat[4];
        double scale_raw[3];
        rt_body3d_get_pose_raw(slot->body, body_pos, body_quat, scale_raw);
        (void)scale_raw;

        /* Bone world pose from the body pose and the fixed frames. */
        double inv_rel_quat[4];
        rg_quat_conj(slot->body_to_bone_quat, inv_rel_quat);
        double bone_quat_world[4];
        rg_quat_mul(body_quat, inv_rel_quat, bone_quat_world);
        rg_quat_normalize(bone_quat_world);
        double offset_world[3];
        rg_quat_rotate(bone_quat_world, slot->body_offset_bone, offset_world);
        double bone_pos_world[3] = {body_pos[0] - offset_world[0],
                                    body_pos[1] - offset_world[1],
                                    body_pos[2] - offset_world[2]};

        /* Model-space pose: inverse(node) × world. */
        double bone_quat_model[4];
        rg_quat_mul(inv_node_quat, bone_quat_world, bone_quat_model);
        rg_quat_normalize(bone_quat_model);
        double rel_world[3] = {bone_pos_world[0] - node_pos[0],
                               bone_pos_world[1] - node_pos[1],
                               bone_pos_world[2] - node_pos[2]};
        double bone_pos_model[3];
        rg_quat_rotate(inv_node_quat, rel_world, bone_pos_model);

        if (slot->parent_slot < 0 && !have_root_delta) {
            root_delta_model[0] = bone_pos_model[0] - slot->bind_global[3];
            root_delta_model[1] = bone_pos_model[1] - slot->bind_global[7];
            root_delta_model[2] = bone_pos_model[2] - slot->bind_global[11];
            have_root_delta = 1;
        }

        double global[16];
        rg_mat_from_quat_pos(bone_quat_model, bone_pos_model, global);
        float *dst = &ragdoll->override_globals[slot->bone_index * 16];
        for (int i = 0; i < 16; ++i)
            dst[i] = (float)global[i];
        ragdoll->override_mask[slot->bone_index] = 1;

        /* Powered drive: PD torque toward the animated relative pose. Bit N of
         * powered_mask selects skeleton bone N (slot->bone_index), not the
         * compacted slot index, so the mask is stable across rig-config changes. */
        if (powered_stiffness > 0.0 && dt > 0.0 && slot->parent_slot >= 0 &&
            ((uint64_t)ragdoll->powered_mask &
             (UINT64_C(1) << ((uint32_t)slot->bone_index & 63u)))) {
            double anim_pos[3];
            double anim_quat[4];
            if (ragdoll3d_animated_bone_pose_slot(ragdoll->controller, slot, anim_pos, anim_quat)) {
                double target_world[4];
                rg_quat_mul(node_quat, anim_quat, target_world);
                double err[3];
                rg_quat_delta_rotvec(bone_quat_world, target_world, err);
                double k = powered_stiffness * 20.0 * dt;
                rt_body3d_apply_angular_impulse(slot->body, err[0] * k, err[1] * k, err[2] * k);
            }
        }
    }

    /* Root-follow: move the node with the corpse while keeping world-space
     * vertices fixed this frame (subtract the same delta from every global). */
    if (have_root_delta && (fabs(root_delta_model[0]) + fabs(root_delta_model[1]) +
                            fabs(root_delta_model[2])) > 1e-9) {
        double shift_world[3];
        rg_quat_rotate(node_quat, root_delta_model, shift_world);
        if (rt_scene_node3d_try_set_world_position(ragdoll->node,
                                                   node_pos[0] + shift_world[0],
                                                   node_pos[1] + shift_world[1],
                                                   node_pos[2] + shift_world[2])) {
            for (int32_t bone = 0; bone < bone_count; ++bone) {
                if (!ragdoll->override_mask[bone])
                    continue;
                float *dst = &ragdoll->override_globals[bone * 16];
                dst[3] -= (float)root_delta_model[0];
                dst[7] -= (float)root_delta_model[1];
                dst[11] -= (float)root_delta_model[2];
            }
        }
    }

    rt_anim_controller3d_apply_pose_override(
        ragdoll->controller, ragdoll->override_mask, ragdoll->override_globals);
    ragdoll->override_ready = 1;
}

/// @brief Blend-out sync: lerp captured ragdoll globals toward live animation.
/// @details Translation uses linear interpolation and rotation uses normalized shortest-path
///          interpolation. When the timer expires, the captured palette and retained controller
///          and node are released.
/// @param[in,out] ragdoll Borrowed inactive rig with optional blend state.
/// @param dt Positive elapsed time in seconds subtracted from the remaining blend.
static void ragdoll3d_step_blend(rt_ragdoll3d *ragdoll, double dt) {
    int32_t bone_count = ragdoll3d_safe_bone_count(ragdoll);
    int32_t slot_count = ragdoll3d_safe_slot_count(ragdoll);
    if (!ragdoll->blend_from ||
        !rt_g3d_has_class(ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID) ||
        rt_anim_controller3d_get_skeleton(ragdoll->controller) != ragdoll->skeleton ||
        bone_count <= 0 || slot_count <= 0 || ragdoll->blend_bone_capacity != bone_count ||
        !isfinite(ragdoll->blend_duration) || ragdoll->blend_duration <= 0.0 ||
        !isfinite(ragdoll->blend_remaining) || ragdoll->blend_remaining <= 0.0) {
        ragdoll->blend_remaining = 0.0;
    } else {
        double next_remaining = fmax(0.0, ragdoll->blend_remaining - dt);
        double t = 1.0 - next_remaining / ragdoll->blend_duration;
        if (t < 0.0)
            t = 0.0;
        if (t > 1.0)
            t = 1.0;
        memset(ragdoll->override_mask, 0, (size_t)bone_count);
        for (int32_t s = 0; s < slot_count; ++s) {
            rt_ragdoll3d_slot *slot = &ragdoll->slots[s];
            if (slot->bone_index < 0 || slot->bone_index >= bone_count)
                continue;
            double anim_pos[3];
            double anim_quat[4];
            if (!ragdoll3d_animated_bone_pose_slot(ragdoll->controller, slot, anim_pos, anim_quat))
                continue;
            const float *from = &ragdoll->blend_from[slot->bone_index * 16];
            double from_d[16];
            for (int i = 0; i < 16; ++i)
                from_d[i] = (double)from[i];
            if (!rg_mat_is_finite(from_d))
                continue;
            double from_quat[4];
            rg_quat_from_mat(from_d, from_quat);
            double from_pos[3] = {from_d[3], from_d[7], from_d[11]};
            /* nlerp rotations, lerp translations. */
            double dot = from_quat[0] * anim_quat[0] + from_quat[1] * anim_quat[1] +
                         from_quat[2] * anim_quat[2] + from_quat[3] * anim_quat[3];
            double sign = dot < 0.0 ? -1.0 : 1.0;
            double quat[4];
            for (int i = 0; i < 4; ++i)
                quat[i] = from_quat[i] * (1.0 - t) + anim_quat[i] * sign * t;
            rg_quat_normalize(quat);
            double pos[3];
            for (int i = 0; i < 3; ++i)
                pos[i] = from_pos[i] * (1.0 - t) + anim_pos[i] * t;
            double global[16];
            rg_mat_from_quat_pos(quat, pos, global);
            float *dst = &ragdoll->override_globals[slot->bone_index * 16];
            for (int i = 0; i < 16; ++i)
                dst[i] = (float)global[i];
            ragdoll->override_mask[slot->bone_index] = 1;
        }
        rt_anim_controller3d_apply_pose_override(
            ragdoll->controller, ragdoll->override_mask, ragdoll->override_globals);
        ragdoll->blend_remaining = next_remaining;
    }
    if (ragdoll->blend_remaining <= 0.0) {
        ragdoll->blend_remaining = 0.0;
        free(ragdoll->blend_from);
        ragdoll->blend_from = NULL;
        ragdoll->blend_bone_capacity = 0;
        ragdoll3d_release_typed(&ragdoll->controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
        ragdoll3d_release_typed(&ragdoll->node, RT_G3D_SCENENODE3D_CLASS_ID);
    }
}

/// @brief `Ragdoll3D.Step(dt)` — see header for ordering requirements.
/// @param obj Borrowed `Ragdoll3D` runtime object.
/// @param dt Positive finite elapsed time in seconds. Invalid or non-positive values are ignored;
///        active rigs synchronize physics and inactive blending rigs advance blend.
void rt_ragdoll3d_step(void *obj, double dt) {
    rt_ragdoll3d *ragdoll = ragdoll3d_checked(obj, "Ragdoll3D.Step: invalid ragdoll");
    if (!ragdoll)
        return;
    if (!isfinite(dt) || dt <= 0.0)
        return;
    if (ragdoll->active)
        ragdoll3d_step_active(ragdoll, dt);
    else if (ragdoll->blend_remaining > 0.0)
        ragdoll3d_step_blend(ragdoll, dt);
}

#endif /* ZANNA_ENABLE_GRAPHICS */
