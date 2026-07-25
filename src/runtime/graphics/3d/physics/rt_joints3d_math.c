//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_joints3d_math.c
// Purpose: Vector, quaternion, and anchor math primitives for the 3D joint
//          solvers. Split out of rt_joints3d.c; declarations and shared
//          types/limits live in rt_joints3d_internal.h.
//
// Key invariants:
//   - All clamp/sanitize helpers map non-finite input to safe defaults, so the
//     solvers never propagate NaN into body state.
//   - Quaternion helpers keep orientation normalized; rotation-vector and
//     axis-angle conversions are the canonical small-angle path.
//
// Ownership/Lifetime:
//   - Pure functions over caller-owned double arrays and body views; no
//     allocation or ownership transfer.
//
// Links: src/runtime/graphics/3d/physics/rt_joints3d.c (joint solvers + API),
//        src/runtime/graphics/3d/physics/rt_joints3d_internal.h (decls/types)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_joints3d_math.c
 * @brief Implements sanitized vector, quaternion, inertia, and anchor-constraint math.
 *
 * The helpers in this translation unit are allocation-free and deliberately
 * defensive: they bound force and coordinate magnitudes, reject non-finite
 * runtime values, preserve normalized orientations, and distribute joint
 * corrections by inverse mass or effective world-space inertia.
 */

#include "rt_joints3d.h"
#include "rt_joints3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_mat4.h"
#include "rt_physics3d.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
#include "rt_trap.h"

//=============================================================================
// Joint math primitives
//=============================================================================

/// @brief Clamp a joint parameter to `[0, RT_JOINT3D_MAX_PARAM]`; non-finite maps to 0.
/// @param value Candidate parameter.
/// @return Sanitized non-negative value.
double joint3d_sanitize_nonnegative(double value) {
    if (!isfinite(value) || value < 0.0)
        return 0.0;
    return value > RT_JOINT3D_MAX_PARAM ? RT_JOINT3D_MAX_PARAM : value;
}

/// @brief Clamp a force/impulse magnitude to `±RT_JOINT3D_MAX_FORCE`; non-finite maps to 0.
/// @param value Candidate force, impulse, or velocity correction.
/// @return Sanitized signed value within the solver envelope.
double joint3d_clamp_force(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > RT_JOINT3D_MAX_FORCE)
        return RT_JOINT3D_MAX_FORCE;
    if (value < -RT_JOINT3D_MAX_FORCE)
        return -RT_JOINT3D_MAX_FORCE;
    return value;
}

/// @brief Clamp a coordinate-like value to the joint runtime envelope.
/// @param value Candidate position or positional correction.
/// @return Finite signed value bounded by `RT_JOINT3D_MAX_COORD`.
double joint3d_clamp_coord(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > RT_JOINT3D_MAX_COORD)
        return RT_JOINT3D_MAX_COORD;
    if (value < -RT_JOINT3D_MAX_COORD)
        return -RT_JOINT3D_MAX_COORD;
    return value;
}

/// @brief Clamp a timestep to a finite positive range used by joint force integration.
/// @param dt Candidate duration in seconds.
/// @return Duration in `[0, RT_JOINT3D_MAX_DT]`.
double joint3d_sanitize_dt(double dt) {
    if (!isfinite(dt) || dt <= 0.0)
        return 0.0;
    return dt > RT_JOINT3D_MAX_DT ? RT_JOINT3D_MAX_DT : dt;
}

/// @brief Whether @p v is non-NULL and all three components are finite.
/// @param v Three-component vector.
/// @return One when every component is finite, otherwise zero.
int joint3d_vec3_all_finite(const double *v) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/// @brief Clamp a raw 3-vector in place.
/// @param v Three-component vector to sanitize; a null pointer is ignored.
void joint3d_vec3_sanitize(double *v) {
    if (!v)
        return;
    v[0] = joint3d_clamp_coord(v[0]);
    v[1] = joint3d_clamp_coord(v[1]);
    v[2] = joint3d_clamp_coord(v[2]);
}

/// @brief Set a 3-vector to (x, y, z) (no-op if @p dst is NULL).
/// @param dst Three-component destination.
/// @param x Candidate X component.
/// @param y Candidate Y component.
/// @param z Candidate Z component.
void joint3d_vec3_set(double *dst, double x, double y, double z) {
    if (!dst)
        return;
    dst[0] = joint3d_clamp_coord(x);
    dst[1] = joint3d_clamp_coord(y);
    dst[2] = joint3d_clamp_coord(z);
}

/// @brief Component-wise difference out = a - b for 3-vectors.
/// @param a Optional left-hand vector; NULL is the origin.
/// @param b Optional right-hand vector; NULL is the origin.
/// @param out Receives the sanitized difference.
void joint3d_vec3_sub(const double *a, const double *b, double *out) {
    if (!out)
        return;
    out[0] = joint3d_clamp_coord((a ? a[0] : 0.0) - (b ? b[0] : 0.0));
    out[1] = joint3d_clamp_coord((a ? a[1] : 0.0) - (b ? b[1] : 0.0));
    out[2] = joint3d_clamp_coord((a ? a[2] : 0.0) - (b ? b[2] : 0.0));
}

/// @brief Dot product of two 3-vectors.
/// @param a Optional first vector; NULL is the origin.
/// @param b Optional second vector; NULL is the origin.
/// @return Finite scalar dot product, or zero on overflow.
double joint3d_vec3_dot(const double *a, const double *b) {
    double ax = joint3d_clamp_coord(a ? a[0] : 0.0);
    double ay = joint3d_clamp_coord(a ? a[1] : 0.0);
    double az = joint3d_clamp_coord(a ? a[2] : 0.0);
    double bx = joint3d_clamp_coord(b ? b[0] : 0.0);
    double by = joint3d_clamp_coord(b ? b[1] : 0.0);
    double bz = joint3d_clamp_coord(b ? b[2] : 0.0);
    double dot = ax * bx + ay * by + az * bz;
    return isfinite(dot) ? dot : 0.0;
}

/// @brief Euclidean length of the vector (x, y, z); returns INFINITY for non-finite inputs
///   or overflow so callers can reject degenerate joint geometry.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @return Robust Euclidean magnitude, zero at the origin, or infinity for unusable input.
double joint3d_len3(double x, double y, double z) {
    double max_abs;
    double sx;
    double sy;
    double sz;
    double len_sq;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;
    max_abs = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    if (max_abs <= 0.0)
        return 0.0;
    if (!isfinite(max_abs))
        return INFINITY;
    sx = x / max_abs;
    sy = y / max_abs;
    sz = z / max_abs;
    len_sq = sx * sx + sy * sy + sz * sz;
    if (!isfinite(len_sq) || len_sq < 0.0)
        return INFINITY;
    return max_abs * sqrt(len_sq);
}

/// @brief Euclidean length of a 3-vector.
/// @param v Three-component vector.
/// @return Robust Euclidean magnitude, or infinity for a null vector.
double joint3d_vec3_len(const double *v) {
    return v ? joint3d_len3(v[0], v[1], v[2]) : INFINITY;
}

/// @brief Normalize a 3-vector in place; returns 0 (leaving it unchanged) if non-finite or
/// near-zero.
/// @param v Three-component vector to normalize.
/// @return One on success, otherwise zero.
int joint3d_vec3_normalize(double *v) {
    double max_abs;
    double x;
    double y;
    double z;
    double len_sq;
    double inv_len;
    if (!joint3d_vec3_all_finite(v))
        return 0;
    max_abs = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));
    if (!isfinite(max_abs) || max_abs < 1e-24)
        return 0;
    x = v[0] / max_abs;
    y = v[1] / max_abs;
    z = v[2] / max_abs;
    len_sq = x * x + y * y + z * z;
    if (!isfinite(len_sq) || len_sq < 1e-24)
        return 0;
    inv_len = 1.0 / sqrt(len_sq);
    v[0] = x * inv_len;
    v[1] = y * inv_len;
    v[2] = z * inv_len;
    return 1;
}

/// @brief Read a boxed Vec3 handle into @p out; returns 0 if not a Vec3 or any component is
/// non-finite.
/// @param obj Candidate runtime Vec3 handle.
/// @param out Receives three sanitized coordinates.
/// @return One for a valid finite Vec3, otherwise zero.
int joint3d_read_vec3(void *obj, double *out) {
    if (!out || !rt_g3d_is_vec3(obj))
        return 0;
    out[0] = rt_vec3_x(obj);
    out[1] = rt_vec3_y(obj);
    out[2] = rt_vec3_z(obj);
    if (!joint3d_vec3_all_finite(out))
        return 0;
    joint3d_vec3_sanitize(out);
    return 1;
}

/// @brief Swap any min/max pair where min > max so each axis' [min, max] limit is well-ordered.
/// @param min_v In/out three-component lower bounds.
/// @param max_v In/out three-component upper bounds.
void joint3d_canonicalize_limits(double *min_v, double *max_v) {
    if (!min_v || !max_v)
        return;
    joint3d_vec3_sanitize(min_v);
    joint3d_vec3_sanitize(max_v);
    for (int i = 0; i < 3; i++) {
        if (min_v[i] > max_v[i]) {
            double tmp = min_v[i];
            min_v[i] = max_v[i];
            max_v[i] = tmp;
        }
    }
}

/// @brief Validate @p obj as a Mat4 payload and return its typed view (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed Mat4 payload view, or NULL on class mismatch.
joint3d_mat4_view *joint3d_mat4_checked(void *obj) {
    if (!obj || !rt_heap_is_payload(obj) || rt_obj_class_id(obj) != RT_MAT4_CLASS_ID)
        return NULL;
    return (joint3d_mat4_view *)obj;
}

/// @brief Extract the translation column from a Mat4 handle; returns 0 if invalid or non-finite.
/// @param obj Candidate Mat4 handle.
/// @param out Receives the sanitized three-component translation.
/// @return One when the handle and translation are finite, otherwise zero.
int joint3d_read_mat4_translation(void *obj, double *out) {
    joint3d_mat4_view *m = joint3d_mat4_checked(obj);
    if (!m || !out)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m->m[i]))
            return 0;
    }
    out[0] = joint3d_clamp_coord(m->m[3]);
    out[1] = joint3d_clamp_coord(m->m[7]);
    out[2] = joint3d_clamp_coord(m->m[11]);
    return 1;
}

/// @brief Hamilton product out = a * b (apply b then a) for (x,y,z,w) quaternions.
/// @param a Optional left-hand quaternion; invalid components use identity defaults.
/// @param b Optional right-hand quaternion; invalid components use identity defaults.
/// @param out Receives the XYZW product.
void joint3d_quat_mul(const double *a, const double *b, double *out) {
    double ax = a && isfinite(a[0]) ? a[0] : 0.0;
    double ay = a && isfinite(a[1]) ? a[1] : 0.0;
    double az = a && isfinite(a[2]) ? a[2] : 0.0;
    double aw = a && isfinite(a[3]) ? a[3] : 1.0;
    double bx = b && isfinite(b[0]) ? b[0] : 0.0;
    double by = b && isfinite(b[1]) ? b[1] : 0.0;
    double bz = b && isfinite(b[2]) ? b[2] : 0.0;
    double bw = b && isfinite(b[3]) ? b[3] : 1.0;
    if (!out)
        return;
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

/// @brief Quaternion conjugate (negated vector part) — the inverse for a unit quaternion.
/// @param q Optional XYZW quaternion; NULL produces identity.
/// @param out Receives a normalized conjugate.
void joint3d_quat_conjugate(const double *q, double *out) {
    if (!out)
        return;
    if (!q) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    out[0] = isfinite(q[0]) ? -q[0] : 0.0;
    out[1] = isfinite(q[1]) ? -q[1] : 0.0;
    out[2] = isfinite(q[2]) ? -q[2] : 0.0;
    out[3] = isfinite(q[3]) ? q[3] : 1.0;
    joint3d_quat_normalize(out);
}

/// @brief Normalize a quaternion in place, falling back to identity for invalid values.
/// @param q XYZW quaternion to normalize; a null pointer is ignored.
void joint3d_quat_normalize(double *q) {
    double max_abs;
    double x;
    double y;
    double z;
    double w;
    double len_sq;
    double inv_len;
    if (!q) {
        return;
    }
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    max_abs = fmax(fmax(fabs(q[0]), fabs(q[1])), fmax(fabs(q[2]), fabs(q[3])));
    if (!isfinite(max_abs) || max_abs < 1e-24) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    x = q[0] / max_abs;
    y = q[1] / max_abs;
    z = q[2] / max_abs;
    w = q[3] / max_abs;
    len_sq = x * x + y * y + z * z + w * w;
    if (!isfinite(len_sq) || len_sq < 1e-24) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    inv_len = 1.0 / sqrt(len_sq);
    q[0] = x * inv_len;
    q[1] = y * inv_len;
    q[2] = z * inv_len;
    q[3] = w * inv_len;
}

/// @brief Build a quaternion from a normalized world axis and an angle in radians.
/// @param axis Finite normalized three-component rotation axis.
/// @param angle Rotation angle in radians, reduced modulo one turn.
/// @param out Receives the normalized XYZW quaternion or identity.
void joint3d_quat_from_axis_angle(const double *axis, double angle, double *out) {
    double half;
    double s;
    if (!axis || !out || !joint3d_vec3_all_finite(axis) || !isfinite(angle)) {
        if (out) {
            out[0] = 0.0;
            out[1] = 0.0;
            out[2] = 0.0;
            out[3] = 1.0;
        }
        return;
    }
    angle = fmod(angle, RT_JOINT3D_TWO_PI);
    if (!isfinite(angle))
        angle = 0.0;
    half = angle * 0.5;
    s = sin(half);
    out[0] = axis[0] * s;
    out[1] = axis[1] * s;
    out[2] = axis[2] * s;
    out[3] = cos(half);
    joint3d_quat_normalize(out);
}

/// @brief Prepend a world-axis rotation to an orientation quaternion.
/// @param orientation In/out XYZW orientation.
/// @param axis Three-component world-space rotation axis.
/// @param angle Rotation angle in radians.
void joint3d_quat_prepend_axis_angle(double *orientation, const double *axis, double angle) {
    double delta[4];
    double out[4];
    if (!orientation || !axis || fabs(angle) < 1e-12)
        return;
    joint3d_quat_from_axis_angle(axis, angle, delta);
    joint3d_quat_mul(delta, orientation, out);
    memcpy(orientation, out, sizeof(out));
    joint3d_quat_normalize(orientation);
}

/// @brief Convert a quaternion delta into a shortest-arc rotation vector.
/// @param q Input XYZW quaternion delta.
/// @param out Receives axis multiplied by shortest signed angle in radians.
void joint3d_quat_to_rotation_vector(const double *q, double *out) {
    double qn[4];
    double v_len;
    double angle;
    double scale;
    if (!out)
        return;
    joint3d_vec3_set(out, 0.0, 0.0, 0.0);
    if (!q)
        return;
    memcpy(qn, q, sizeof(qn));
    joint3d_quat_normalize(qn);
    if (qn[3] < 0.0) {
        qn[0] = -qn[0];
        qn[1] = -qn[1];
        qn[2] = -qn[2];
        qn[3] = -qn[3];
    }
    v_len = sqrt(qn[0] * qn[0] + qn[1] * qn[1] + qn[2] * qn[2]);
    if (!isfinite(v_len) || v_len < 1e-12) {
        out[0] = 2.0 * qn[0];
        out[1] = 2.0 * qn[1];
        out[2] = 2.0 * qn[2];
        return;
    }
    angle = 2.0 * atan2(v_len, qn[3]);
    scale = angle / v_len;
    out[0] = qn[0] * scale;
    out[1] = qn[1] * scale;
    out[2] = qn[2] * scale;
}

/// @brief Rotate vector @p v by quaternion @p q (out = q * v * q⁻¹).
/// @param q Optional XYZW rotation; NULL copies @p v.
/// @param v Optional input vector; NULL is the origin.
/// @param out Receives the sanitized rotated vector.
void joint3d_quat_rotate_vec3(const double *q, const double *v, double *out) {
    double qn[4];
    double vv[3] = {v ? v[0] : 0.0, v ? v[1] : 0.0, v ? v[2] : 0.0};
    double qv[4];
    double q_conj[4];
    double tmp[4];
    double rotated[4];
    if (!out)
        return;
    if (!q) {
        joint3d_vec3_set(out, vv[0], vv[1], vv[2]);
        return;
    }
    memcpy(qn, q, sizeof(qn));
    joint3d_quat_normalize(qn);
    joint3d_vec3_sanitize(vv);
    qv[0] = vv[0];
    qv[1] = vv[1];
    qv[2] = vv[2];
    qv[3] = 0.0;
    joint3d_quat_conjugate(qn, q_conj);
    joint3d_quat_mul(qn, qv, tmp);
    joint3d_quat_mul(tmp, q_conj, rotated);
    joint3d_vec3_set(out, rotated[0], rotated[1], rotated[2]);
}

/// @brief Transform a body-local anchor point into world space (rotate by orientation, add
/// position).
/// @param body Body3D kinematic pose.
/// @param local_anchor Three-component local anchor.
/// @param out Receives the world-space anchor or origin fallback.
void joint3d_world_anchor(const rt_body3d_kinematics *body,
                                 const double *local_anchor,
                                 double *out) {
    double rotated[3];
    if (!out)
        return;
    if (!body || !local_anchor) {
        joint3d_vec3_set(out, 0.0, 0.0, 0.0);
        return;
    }
    joint3d_quat_rotate_vec3(body->orientation, local_anchor, rotated);
    out[0] = joint3d_clamp_coord(body->position[0] + rotated[0]);
    out[1] = joint3d_clamp_coord(body->position[1] + rotated[1]);
    out[2] = joint3d_clamp_coord(body->position[2] + rotated[2]);
}

/// @brief Transform a world-space point into a body's local frame (subtract position, unrotate).
/// @param body Body3D kinematic pose.
/// @param world_point Three-component world point.
/// @param out Receives the sanitized local-space point.
void joint3d_local_from_world(const rt_body3d_kinematics *body,
                                     const double *world_point,
                                     double *out) {
    double delta[3];
    double inv_rotation[4];
    if (!body || !world_point || !out) {
        if (out)
            joint3d_vec3_set(out, 0.0, 0.0, 0.0);
        return;
    }
    joint3d_vec3_sub(world_point, body->position, delta);
    joint3d_quat_conjugate(body->orientation, inv_rotation);
    joint3d_quat_rotate_vec3(inv_rotation, delta, out);
    joint3d_vec3_sanitize(out);
}

/// @brief Rotate a body-local axis into world space and normalize it (defaults to +Y if
/// degenerate).
/// @param body Body3D kinematic pose.
/// @param local_axis Three-component local axis.
/// @param out Receives the normalized world axis or +Y fallback.
void joint3d_world_axis_from_local(const rt_body3d_kinematics *body,
                                          const double *local_axis,
                                          double *out) {
    if (!body || !local_axis || !out) {
        if (out)
            joint3d_vec3_set(out, 0.0, 1.0, 0.0);
        return;
    }
    joint3d_quat_rotate_vec3(body->orientation, local_axis, out);
    if (!joint3d_vec3_normalize(out))
        joint3d_vec3_set(out, 0.0, 1.0, 0.0);
}

/// @brief out = I^-1 * v in world space, from the body's local diagonal inverse inertia.
/// @details Mirrors body3d_world_inv_inertia_mul: rotate the vector into the body's
///          principal-axis frame, scale by the diagonal inverse inertia, rotate back.
///          Angular constraints must weight by this, not by inverse mass.
/// @param body Body3D orientation and principal inverse inertia.
/// @param v Three-component world-space vector.
/// @param out Receives the finite world-space tensor product.
void joint3d_world_inv_inertia_mul(const rt_body3d_kinematics *body,
                                   const double *v,
                                   double *out) {
    double inv_rotation[4];
    double local_v[3];
    double local_out[3];
    if (!out)
        return;
    joint3d_vec3_set(out, 0.0, 0.0, 0.0);
    if (!body || !v || !joint3d_vec3_all_finite(v))
        return;
    joint3d_quat_conjugate(body->orientation, inv_rotation);
    joint3d_quat_rotate_vec3(inv_rotation, v, local_v);
    local_out[0] = local_v[0] * body->inv_inertia[0];
    local_out[1] = local_v[1] * body->inv_inertia[1];
    local_out[2] = local_v[2] * body->inv_inertia[2];
    joint3d_quat_rotate_vec3(body->orientation, local_out, out);
    if (!joint3d_vec3_all_finite(out))
        joint3d_vec3_set(out, 0.0, 0.0, 0.0);
}

/// @brief Scalar effective inverse inertia of @p body about the (unit) world @p axis.
/// @details axis · (I^-1 axis); the rotational analogue of inverse mass along a
///          direction. Zero for static/kinematic bodies (inv_inertia all zero).
/// @param body Body3D orientation and principal inverse inertia.
/// @param axis Unit world-space axis.
/// @return Positive effective inverse inertia, or zero for unusable or immovable state.
double joint3d_effective_inv_inertia_about_axis(const rt_body3d_kinematics *body,
                                                const double *axis) {
    double inv_i_axis[3];
    double w;
    if (!body || !axis)
        return 0.0;
    joint3d_world_inv_inertia_mul(body, axis, inv_i_axis);
    w = joint3d_vec3_dot(axis, inv_i_axis);
    return (isfinite(w) && w > 0.0) ? w : 0.0;
}

/// @brief Build the 3x3 world-space inverse inertia tensor (row-major) of @p body.
/// @details Columns are I^-1 applied to each world basis vector; the tensor is
///          symmetric, so this equals R * diag(inv_inertia) * R^T.
/// @param body Body3D orientation and principal inverse inertia.
/// @param m Receives the nine-element row-major tensor.
static void joint3d_world_inv_inertia_tensor(const rt_body3d_kinematics *body, double m[9]) {
    int c;
    for (c = 0; c < 9; c++)
        m[c] = 0.0;
    if (!body)
        return;
    for (c = 0; c < 3; c++) {
        double e[3] = {0.0, 0.0, 0.0};
        double col[3];
        e[c] = 1.0;
        joint3d_world_inv_inertia_mul(body, e, col);
        m[0 * 3 + c] = col[0];
        m[1 * 3 + c] = col[1];
        m[2 * 3 + c] = col[2];
    }
}

/// @brief Solve the 3x3 system @p m * x = @p rhs via the cofactor inverse.
/// @param m Nine-element row-major coefficient matrix.
/// @param rhs Three-component right-hand side.
/// @param x Receives the finite solution on success.
/// @return 1 with @p x set on success, 0 if @p m is singular (leaving x untouched).
static int joint3d_solve3(const double m[9], const double *rhs, double *x) {
    double c00 = m[4] * m[8] - m[5] * m[7];
    double c01 = m[5] * m[6] - m[3] * m[8];
    double c02 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    double inv_det;
    double c10, c11, c12, c20, c21, c22;
    if (!isfinite(det) || fabs(det) < 1e-12)
        return 0;
    inv_det = 1.0 / det;
    c10 = m[2] * m[7] - m[1] * m[8];
    c11 = m[0] * m[8] - m[2] * m[6];
    c12 = m[1] * m[6] - m[0] * m[7];
    c20 = m[1] * m[5] - m[2] * m[4];
    c21 = m[2] * m[3] - m[0] * m[5];
    c22 = m[0] * m[4] - m[1] * m[3];
    /* x = M^-1 rhs; M^-1 = adj(M)^T / det, adj rows are the cofactors above. */
    x[0] = (c00 * rhs[0] + c10 * rhs[1] + c20 * rhs[2]) * inv_det;
    x[1] = (c01 * rhs[0] + c11 * rhs[1] + c21 * rhs[2]) * inv_det;
    x[2] = (c02 * rhs[0] + c12 * rhs[1] + c22 * rhs[2]) * inv_det;
    return joint3d_vec3_all_finite(x);
}

/// @brief Positionally pull two bodies' world anchors together (a ball-socket positional
/// constraint).
/// @details Splits the anchor gap between the bodies in inverse-mass proportion, scaled by
///          @p stiffness (clamped to (0, 1]). No-op for non-finite bodies or two immovable bodies.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param stiffness Correction fraction, normalized to the inclusive range zero to one.
void joint3d_correct_anchor_pair(rt_body3d_kinematics *body_a,
                                        rt_body3d_kinematics *body_b,
                                        const double *local_anchor_a,
                                        const double *local_anchor_b,
                                        double stiffness) {
    double anchor_a[3];
    double anchor_b[3];
    double delta[3];
    double inv_sum;
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b))
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    joint3d_world_anchor(body_a, local_anchor_a, anchor_a);
    joint3d_world_anchor(body_b, local_anchor_b, anchor_b);
    joint3d_vec3_sub(anchor_b, anchor_a, delta);
    if (!joint3d_vec3_all_finite(delta))
        return;
    if (!isfinite(stiffness) || stiffness <= 0.0)
        stiffness = 1.0;
    if (stiffness > 1.0)
        stiffness = 1.0;
    double scale = stiffness / inv_sum;
    for (int i = 0; i < 3; i++) {
        double correction = delta[i] * scale;
        body_a->position[i] =
            joint3d_clamp_coord(body_a->position[i] + correction * body_a->inv_mass);
        body_b->position[i] =
            joint3d_clamp_coord(body_b->position[i] - correction * body_b->inv_mass);
    }
    if (joint3d_vec3_dot(delta, delta) > 1e-24) {
        if (body_a->inv_mass > 0.0)
            joint3d_mark_body_moved(body_a);
        if (body_b->inv_mass > 0.0)
            joint3d_mark_body_moved(body_b);
    }
}

/// @brief Positionally correct only the per-axis anchor-gap components that exceed [min, max].
/// @details Like joint3d_correct_anchor_pair but the constraint is a box: an axis within its
///          linear limits is left free; only the limit overshoot is projected out (inverse-mass
///          split).
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param linear_min Three-component world-space lower bounds.
/// @param linear_max Three-component world-space upper bounds.
void joint3d_correct_anchor_pair_limited(rt_body3d_kinematics *body_a,
                                                rt_body3d_kinematics *body_b,
                                                const double *local_anchor_a,
                                                const double *local_anchor_b,
                                                const double *linear_min,
                                                const double *linear_max) {
    double anchor_a[3];
    double anchor_b[3];
    double delta[3];
    double violation[3] = {0.0, 0.0, 0.0};
    double inv_sum;
    int has_violation = 0;
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b) || !linear_min ||
        !linear_max)
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    joint3d_world_anchor(body_a, local_anchor_a, anchor_a);
    joint3d_world_anchor(body_b, local_anchor_b, anchor_b);
    joint3d_vec3_sub(anchor_b, anchor_a, delta);
    if (!joint3d_vec3_all_finite(delta))
        return;
    for (int i = 0; i < 3; i++) {
        if (delta[i] < linear_min[i]) {
            violation[i] = delta[i] - linear_min[i];
            has_violation = 1;
        } else if (delta[i] > linear_max[i]) {
            violation[i] = delta[i] - linear_max[i];
            has_violation = 1;
        }
    }
    if (!has_violation)
        return;
    for (int i = 0; i < 3; i++) {
        double correction = violation[i] / inv_sum;
        correction = joint3d_clamp_coord(correction);
        body_a->position[i] =
            joint3d_clamp_coord(body_a->position[i] + correction * body_a->inv_mass);
        body_b->position[i] =
            joint3d_clamp_coord(body_b->position[i] - correction * body_b->inv_mass);
    }
    if (body_a->inv_mass > 0.0)
        joint3d_mark_body_moved(body_a);
    if (body_b->inv_mass > 0.0)
        joint3d_mark_body_moved(body_b);
}

/// @brief Cancel @p amount (0..1) of the bodies' relative linear velocity, inverse-mass weighted.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param amount Fraction of relative velocity to remove.
void joint3d_remove_relative_linear_velocity(rt_body3d_kinematics *body_a,
                                                    rt_body3d_kinematics *body_b,
                                                    double amount) {
    double inv_sum;
    double rel[3];
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b))
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    if (!isfinite(amount) || amount <= 0.0)
        amount = 1.0;
    if (amount > 1.0)
        amount = 1.0;
    joint3d_vec3_sub(body_b->velocity, body_a->velocity, rel);
    if (!joint3d_vec3_all_finite(rel))
        return;
    for (int i = 0; i < 3; i++) {
        double correction = rel[i] * amount / inv_sum;
        correction = joint3d_clamp_force(correction);
        body_a->velocity[i] =
            joint3d_clamp_force(body_a->velocity[i] + correction * body_a->inv_mass);
        body_b->velocity[i] =
            joint3d_clamp_force(body_b->velocity[i] - correction * body_b->inv_mass);
    }
}

/// @brief Fully cancel relative linear velocity only along locked axes (those with min == max).
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param linear_min Three-component world-space lower bounds.
/// @param linear_max Three-component world-space upper bounds.
void joint3d_remove_relative_linear_velocity_locked_axes(rt_body3d_kinematics *body_a,
                                                                rt_body3d_kinematics *body_b,
                                                                const double *linear_min,
                                                                const double *linear_max) {
    double inv_sum;
    double rel[3];
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b) || !linear_min ||
        !linear_max)
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    joint3d_vec3_sub(body_b->velocity, body_a->velocity, rel);
    if (!joint3d_vec3_all_finite(rel))
        return;
    for (int i = 0; i < 3; i++) {
        if (fabs(linear_max[i] - linear_min[i]) > 1e-12)
            continue;
        double correction = rel[i] / inv_sum;
        correction = joint3d_clamp_force(correction);
        body_a->velocity[i] =
            joint3d_clamp_force(body_a->velocity[i] + correction * body_a->inv_mass);
        body_b->velocity[i] =
            joint3d_clamp_force(body_b->velocity[i] - correction * body_b->inv_mass);
    }
}

/// @brief joint3d_correct_anchor_pair_limited, but with the [min, max] box expressed
///   in the joint frame given by @p frame_quat (body-A axes for the SixDof joint).
/// @details The anchor gap is rotated into the frame, per-axis violations are
///   computed there, and the correction is rotated back to world before the
///   inverse-mass split — so a slider constrained along "local X" keeps tracking
///   body A's X as it rotates. NULL @p frame_quat means world axes.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param linear_min Three-component lower frame-space bounds.
/// @param linear_max Three-component upper frame-space bounds.
/// @param frame_quat Optional XYZW quaternion rotating frame vectors to world space.
void joint3d_correct_anchor_pair_limited_frame(rt_body3d_kinematics *body_a,
                                               rt_body3d_kinematics *body_b,
                                               const double *local_anchor_a,
                                               const double *local_anchor_b,
                                               const double *linear_min,
                                               const double *linear_max,
                                               const double *frame_quat) {
    double anchor_a[3];
    double anchor_b[3];
    double delta[3];
    double delta_frame[3];
    double violation_frame[3] = {0.0, 0.0, 0.0};
    double violation_world[3];
    double inv_frame[4];
    double inv_sum;
    int has_violation = 0;
    if (!frame_quat) {
        joint3d_correct_anchor_pair_limited(
            body_a, body_b, local_anchor_a, local_anchor_b, linear_min, linear_max);
        return;
    }
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b) || !linear_min ||
        !linear_max)
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    joint3d_world_anchor(body_a, local_anchor_a, anchor_a);
    joint3d_world_anchor(body_b, local_anchor_b, anchor_b);
    joint3d_vec3_sub(anchor_b, anchor_a, delta);
    if (!joint3d_vec3_all_finite(delta))
        return;
    joint3d_quat_conjugate(frame_quat, inv_frame);
    joint3d_quat_rotate_vec3(inv_frame, delta, delta_frame);
    if (!joint3d_vec3_all_finite(delta_frame))
        return;
    for (int i = 0; i < 3; i++) {
        if (delta_frame[i] < linear_min[i]) {
            violation_frame[i] = delta_frame[i] - linear_min[i];
            has_violation = 1;
        } else if (delta_frame[i] > linear_max[i]) {
            violation_frame[i] = delta_frame[i] - linear_max[i];
            has_violation = 1;
        }
    }
    if (!has_violation)
        return;
    joint3d_quat_rotate_vec3(frame_quat, violation_frame, violation_world);
    if (!joint3d_vec3_all_finite(violation_world))
        return;
    for (int i = 0; i < 3; i++) {
        double correction = joint3d_clamp_coord(violation_world[i] / inv_sum);
        body_a->position[i] =
            joint3d_clamp_coord(body_a->position[i] + correction * body_a->inv_mass);
        body_b->position[i] =
            joint3d_clamp_coord(body_b->position[i] - correction * body_b->inv_mass);
    }
    if (body_a->inv_mass > 0.0)
        joint3d_mark_body_moved(body_a);
    if (body_b->inv_mass > 0.0)
        joint3d_mark_body_moved(body_b);
}

/// @brief joint3d_remove_relative_linear_velocity_locked_axes, but with the locked
///   axes expressed in the joint frame given by @p frame_quat. NULL means world axes.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param linear_min Three-component lower frame-space bounds.
/// @param linear_max Three-component upper frame-space bounds.
/// @param frame_quat Optional XYZW quaternion rotating frame axes to world space.
void joint3d_remove_relative_linear_velocity_locked_axes_frame(rt_body3d_kinematics *body_a,
                                                               rt_body3d_kinematics *body_b,
                                                               const double *linear_min,
                                                               const double *linear_max,
                                                               const double *frame_quat) {
    double inv_sum;
    double rel[3];
    double rel_frame[3];
    double removal_frame[3] = {0.0, 0.0, 0.0};
    double removal_world[3];
    double inv_frame[4];
    int any_locked = 0;
    if (!frame_quat) {
        joint3d_remove_relative_linear_velocity_locked_axes(
            body_a, body_b, linear_min, linear_max);
        return;
    }
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b) || !linear_min ||
        !linear_max)
        return;
    inv_sum = body_a->inv_mass + body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    joint3d_vec3_sub(body_b->velocity, body_a->velocity, rel);
    if (!joint3d_vec3_all_finite(rel))
        return;
    joint3d_quat_conjugate(frame_quat, inv_frame);
    joint3d_quat_rotate_vec3(inv_frame, rel, rel_frame);
    if (!joint3d_vec3_all_finite(rel_frame))
        return;
    for (int i = 0; i < 3; i++) {
        if (fabs(linear_max[i] - linear_min[i]) > 1e-12)
            continue;
        removal_frame[i] = rel_frame[i];
        any_locked = 1;
    }
    if (!any_locked)
        return;
    joint3d_quat_rotate_vec3(frame_quat, removal_frame, removal_world);
    if (!joint3d_vec3_all_finite(removal_world))
        return;
    for (int i = 0; i < 3; i++) {
        double correction = joint3d_clamp_force(removal_world[i] / inv_sum);
        body_a->velocity[i] =
            joint3d_clamp_force(body_a->velocity[i] + correction * body_a->inv_mass);
        body_b->velocity[i] =
            joint3d_clamp_force(body_b->velocity[i] - correction * body_b->inv_mass);
    }
}

/// @brief Cancel the bodies' relative angular velocity except the component along @p allowed_axis.
/// @details Implements a hinge's angular constraint: spin about the hinge axis is preserved while
///          all off-axis relative rotation is removed (inverse-mass weighted). A NULL axis removes
///          all.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param allowed_axis Optional normalized world axis whose relative spin is preserved.
void joint3d_remove_relative_angular_velocity(rt_body3d_kinematics *body_a,
                                                     rt_body3d_kinematics *body_b,
                                                     const double *allowed_axis) {
    double rel[3];
    double remove[3];
    double ka[9];
    double kb[9];
    double k[9];
    double impulse[3];
    double dv_a[3];
    double dv_b[3];
    int i;
    if (!joint3d_body_is_finite(body_a) || !joint3d_body_is_finite(body_b))
        return;
    if (!joint3d_vec3_all_finite(body_a->angular_velocity) ||
        !joint3d_vec3_all_finite(body_b->angular_velocity))
        return;
    joint3d_vec3_sub(body_b->angular_velocity, body_a->angular_velocity, rel);
    remove[0] = rel[0];
    remove[1] = rel[1];
    remove[2] = rel[2];
    if (allowed_axis && joint3d_vec3_all_finite(allowed_axis)) {
        double axial = joint3d_vec3_dot(rel, allowed_axis);
        remove[0] -= allowed_axis[0] * axial;
        remove[1] -= allowed_axis[1] * axial;
        remove[2] -= allowed_axis[2] * axial;
    }
    if (!joint3d_vec3_all_finite(remove))
        return;
    /* Cancel the off-axis relative angular velocity with an angular impulse L
     * satisfying (Ia^-1 + Ib^-1) L = remove, then wa += Ia^-1 L, wb -= Ib^-1 L.
     * Weighting by inverse inertia (not inverse mass) is what distributes the
     * correction correctly between heavy/light and non-uniform bodies. */
    joint3d_world_inv_inertia_tensor(body_a, ka);
    joint3d_world_inv_inertia_tensor(body_b, kb);
    for (i = 0; i < 9; i++)
        k[i] = ka[i] + kb[i];
    if (!joint3d_solve3(k, remove, impulse))
        return; /* neither body can rotate on this constraint */
    joint3d_world_inv_inertia_mul(body_a, impulse, dv_a);
    joint3d_world_inv_inertia_mul(body_b, impulse, dv_b);
    for (i = 0; i < 3; i++) {
        body_a->angular_velocity[i] = joint3d_clamp_force(body_a->angular_velocity[i] + dv_a[i]);
        body_b->angular_velocity[i] = joint3d_clamp_force(body_b->angular_velocity[i] - dv_b[i]);
    }
}
