//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_quat.c
// Purpose: Quaternion mathematics for the Zanna.Quat class. Implements Hamilton
//   quaternions for 3D rotation: construction from axis-angle and Euler angles,
//   multiplication (composition), conjugate/inverse, normalization, dot product,
//   spherical linear interpolation (Slerp), Vec3 rotation, and conversion to/from
//   Mat4. Quaternions avoid gimbal lock and provide smooth rotation blending.
//
// Key invariants:
//   - Memory layout is (x, y, z, w) where w is the scalar part; unit quaternions
//     satisfy |q| = 1.0 and represent valid 3D rotations.
//   - All constructor functions call quat_alloc() which may trap on OOM; callers
//     should not pass NULL results to further operations.
//   - Slerp clamps the dot product to [-1, 1] to guard against acos domain errors
//     from floating-point rounding; falls back to linear interpolation when the
//     angle is near zero.
//   - Quat objects are immutable after creation; all operations return new GC
//     heap objects.
//   - FromAxisAngle normalizes the axis vector; a zero-length axis returns the
//     identity quaternion (0, 0, 0, 1).
//
// Ownership/Lifetime:
//   - All Quat objects are allocated via rt_obj_new_i64 (GC heap); the struct
//     contains only four doubles and requires no finalizer.
//
// Links: src/runtime/graphics/math/rt_quat.h (public API),
//        src/runtime/graphics/math/rt_vec3.h (axis operand and rotation result type),
//        src/runtime/graphics/math/rt_mat4.h (rotation matrix conversion)
//
//===----------------------------------------------------------------------===//

#include "rt_quat.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_vec3.h"

#include <math.h>

typedef struct {
    double x;
    double y;
    double z;
    double w;
} ZannaQuat;

/// @brief Return whether @p q is a Quat-compatible heap payload.
/// @details Accepts the explicit Quat class id from current constructors and the historical
///          class-id-zero value object layout. Legacy classless payloads must be exactly four
///          doubles so unrelated raw heap values are rejected.
/// @param q Candidate runtime object payload.
/// @return 1 for a compatible quaternion payload, otherwise 0.
static int quat_is_compatible_object(void *q) {
    if (!q)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(q, &heap_info))
        return 0;
    if (heap_info.kind != RT_HEAP_OBJECT || heap_info.elem_kind != RT_ELEM_NONE)
        return 0;
    if (heap_info.class_id == RT_QUAT_CLASS_ID)
        return heap_info.cap >= sizeof(ZannaQuat);
    return heap_info.class_id == 0 && heap_info.len == sizeof(ZannaQuat) &&
           heap_info.cap == sizeof(ZannaQuat);
}

/// @brief Validate and cast an opaque handle to a quaternion payload.
/// @details Rejects NULL, non-object heap payloads, incompatible class identifiers, and
///   undersized allocations before quaternion fields are read.
/// @param q Candidate Quat runtime handle.
/// @param op Diagnostic prefix used if validation fails.
/// @return Typed quaternion payload, or NULL after trapping.
static ZannaQuat *quat_checked(void *q, const char *op) {
    if (!quat_is_compatible_object(q)) {
        rt_trap(op ? op : "Quat: invalid quaternion");
        return NULL;
    }
    return (ZannaQuat *)q;
}

/// @brief Compute a finite, overflow-resistant Euclidean norm for quaternion components.
/// @details Uses `hypot` in a chain so very large finite inputs do not overflow while
///          squaring and very small inputs do not underflow to zero prematurely. Non-finite
///          components return `INFINITY`, giving callers a deterministic fallback path
///          instead of propagating NaN payloads into transform math.
/// @param x Quaternion x component.
/// @param y Quaternion y component.
/// @param z Quaternion z component.
/// @param w Quaternion scalar component.
/// @return Euclidean norm, or positive infinity when any component is non-finite.
static double quat_safe_len4(double x, double y, double z, double w) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(w))
        return INFINITY;
    return hypot(hypot(x, y), hypot(z, w));
}

/// @brief Compute a finite, overflow-resistant Euclidean norm for a 3D axis.
/// @details This is the axis-specific counterpart to quat_safe_len4 and prevents
///          `FromAxisAngle` from overflowing while normalizing unusually large but
///          otherwise valid direction vectors.
/// @param x Axis x component.
/// @param y Axis y component.
/// @param z Axis z component.
/// @return Euclidean norm, or positive infinity when any component is non-finite.
static double quat_safe_len3(double x, double y, double z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;
    return hypot(hypot(x, y), z);
}

/// @brief Allocate a GC-managed quaternion object and initialize all four components.
/// @param x Quaternion x component.
/// @param y Quaternion y component.
/// @param z Quaternion z component.
/// @param w Quaternion scalar component.
/// @return New ZannaQuat with the given (x, y, z, w) components, or NULL on OOM.
static ZannaQuat *quat_alloc(double x, double y, double z, double w) {
    ZannaQuat *q = (ZannaQuat *)rt_obj_new_i64(RT_QUAT_CLASS_ID, (int64_t)sizeof(ZannaQuat));
    if (!q) {
        rt_trap("Quat: memory allocation failed");
        return NULL;
    }
    q->x = x;
    q->y = y;
    q->z = z;
    q->w = w;
    return q;
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Construct a quaternion from raw vector and scalar components.
/// @details Stores `(x, y, z, w)` without normalization. For a rotation quaternion, x, y, and z
///          form the imaginary/vector part and w is the real/scalar part.
/// @param x Quaternion x component.
/// @param y Quaternion y component.
/// @param z Quaternion z component.
/// @param w Quaternion scalar component.
/// @return New GC-managed Quat, or NULL after trapping if allocation fails.
void *rt_quat_new(double x, double y, double z, double w) {
    return quat_alloc(x, y, z, w);
}

/// @brief Create the identity quaternion `(0, 0, 0, 1)`.
/// @return New GC-managed identity Quat, or NULL after trapping if allocation fails.
void *rt_quat_identity(void) {
    return quat_alloc(0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a unit quaternion from an axis-angle rotation.
/// @details Normalizes @p axis with an overflow-resistant length. A zero-length or non-finite axis,
///          or a non-finite angle, produces identity. A NULL axis raises a runtime trap and returns
///          NULL.
/// @param axis Vec3 rotation axis; it need not be normalized.
/// @param angle Rotation angle in radians.
/// @return New rotation Quat, identity for a degenerate value, or NULL after trapping for a NULL
///         axis or allocation failure.
void *rt_quat_from_axis_angle(void *axis, double angle) {
    if (!axis) {
        rt_trap("Quat.FromAxisAngle: null axis");
        return NULL;
    }
    if (!isfinite(angle))
        return quat_alloc(0.0, 0.0, 0.0, 1.0);
    double ax = rt_vec3_x(axis);
    double ay = rt_vec3_y(axis);
    double az = rt_vec3_z(axis);
    double len = quat_safe_len3(ax, ay, az);
    if (len == 0.0 || !isfinite(len))
        return quat_alloc(0.0, 0.0, 0.0, 1.0);

    ax /= len;
    ay /= len;
    az /= len;
    double half = angle * 0.5;
    double s = sin(half);
    return quat_alloc(ax * s, ay * s, az * s, cos(half));
}

/// @brief Create a unit quaternion from pitch, yaw, and roll Euler angles.
/// @details Forms `qz(roll) * qy(yaw) * qx(pitch)`, so column vectors receive the fixed-axis pitch,
///          yaw, and roll rotations in that order. If any angle is non-finite, returns identity.
/// @param pitch Rotation about the x axis in radians.
/// @param yaw Rotation about the y axis in radians.
/// @param roll Rotation about the z axis in radians.
/// @return New rotation Quat, identity for non-finite input, or NULL after an allocation trap.
void *rt_quat_from_euler(double pitch, double yaw, double roll) {
    if (!isfinite(pitch) || !isfinite(yaw) || !isfinite(roll))
        return quat_alloc(0.0, 0.0, 0.0, 1.0);
    double cp = cos(pitch * 0.5);
    double sp = sin(pitch * 0.5);
    double cy = cos(yaw * 0.5);
    double sy = sin(yaw * 0.5);
    double cr = cos(roll * 0.5);
    double sr = sin(roll * 0.5);

    double x = sp * cy * cr - cp * sy * sr;
    double y = cp * sy * cr + sp * cy * sr;
    double z = cp * cy * sr - sp * sy * cr;
    double w = cp * cy * cr + sp * sy * sr;
    return quat_alloc(x, y, z, w);
}

//=============================================================================
// Property Accessors
//=============================================================================

/// @brief Read a quaternion's x component.
/// @param q Quat handle to inspect.
/// @return Stored x component, or 0.0 after trapping for an invalid handle.
double rt_quat_x(void *q) {
    ZannaQuat *quat = quat_checked(q, "Quat.X: invalid quaternion");
    if (!quat)
        return 0.0;
    return quat->x;
}

/// @brief Read a quaternion's y component.
/// @param q Quat handle to inspect.
/// @return Stored y component, or 0.0 after trapping for an invalid handle.
double rt_quat_y(void *q) {
    ZannaQuat *quat = quat_checked(q, "Quat.Y: invalid quaternion");
    if (!quat)
        return 0.0;
    return quat->y;
}

/// @brief Read a quaternion's z component.
/// @param q Quat handle to inspect.
/// @return Stored z component, or 0.0 after trapping for an invalid handle.
double rt_quat_z(void *q) {
    ZannaQuat *quat = quat_checked(q, "Quat.Z: invalid quaternion");
    if (!quat)
        return 0.0;
    return quat->z;
}

/// @brief Read a quaternion's scalar w component.
/// @param q Quat handle to inspect.
/// @return Stored w component, or 0.0 after trapping for an invalid handle.
double rt_quat_w(void *q) {
    ZannaQuat *quat = quat_checked(q, "Quat.W: invalid quaternion");
    if (!quat)
        return 0.0;
    return quat->w;
}

//=============================================================================
// Operations
//=============================================================================

/// @brief Compute the Hamilton product of two quaternions.
/// @details For unit rotation quaternions, `mul(a, b)` composes the rotation of @p b followed by
///          the rotation of @p a. Invalid operands raise a runtime trap.
/// @param a Left-hand Quat operand.
/// @param b Right-hand Quat operand.
/// @return New product Quat, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_mul(void *a, void *b) {
    ZannaQuat *qa = quat_checked(a, "Quat.Mul: invalid quaternion");
    ZannaQuat *qb = quat_checked(b, "Quat.Mul: invalid quaternion");
    if (!qa || !qb)
        return NULL;
    double w = qa->w * qb->w - qa->x * qb->x - qa->y * qb->y - qa->z * qb->z;
    double x = qa->w * qb->x + qa->x * qb->w + qa->y * qb->z - qa->z * qb->y;
    double y = qa->w * qb->y - qa->x * qb->z + qa->y * qb->w + qa->z * qb->x;
    double z = qa->w * qb->z + qa->x * qb->y - qa->y * qb->x + qa->z * qb->w;
    return quat_alloc(x, y, z, w);
}

/// @brief Create the conjugate of a quaternion.
/// @details Negates x, y, and z while preserving w. For a unit quaternion, the conjugate is also
///          its inverse and represents the opposite rotation.
/// @param q Quat operand.
/// @return New conjugate Quat, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_conjugate(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Conjugate: invalid quaternion");
    if (!qv)
        return NULL;
    return quat_alloc(-qv->x, -qv->y, -qv->z, qv->w);
}

/// @brief Quaternion inverse (conjugate / |q|²). For unit quaternions matches `_conjugate`.
/// @details Uses two successive divisions by the overflow-resistant norm instead of explicitly
///          forming its square. An invalid quaternion or zero/non-finite norm raises a runtime
///          trap.
/// @param q Quat operand.
/// @return New inverse Quat, or NULL after trapping when input is invalid or allocation fails.
void *rt_quat_inverse(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Inverse: invalid quaternion");
    if (!qv)
        return NULL;
    double len = quat_safe_len4(qv->x, qv->y, qv->z, qv->w);
    if (len == 0.0 || !isfinite(len)) {
        rt_trap("Quat.Inverse: invalid quaternion length");
        return NULL;
    }
    // inverse = conjugate / |q|^2. Forming `len*len` overflows for large norms
    // (e.g. 1e308 -> Inf, inverse collapses to 0) and underflows for tiny norms
    // (e.g. 1e-308 -> 0, inverse blows up to +/-Inf) even though `len` itself is
    // finite and nonzero (VDOC-206). Divide each conjugate component by `len`
    // TWICE instead: the first divide normalizes it to ~unit magnitude, the
    // second scales by 1/len, so no intermediate overflows or underflows.
    double s = 1.0 / len;
    return quat_alloc((-qv->x * s) * s, (-qv->y * s) * s, (-qv->z * s) * s, (qv->w * s) * s);
}

/// @brief Normalize a quaternion to unit length.
/// @details A zero or non-finite norm produces the zero quaternion `(0, 0, 0, 0)`. An incompatible
///          handle raises a runtime trap instead.
/// @param q Quat operand.
/// @return New normalized Quat, zero Quat for a degenerate norm, or NULL after trapping for invalid
///         input or allocation failure.
void *rt_quat_norm(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Norm: invalid quaternion");
    if (!qv)
        return NULL;
    double len = quat_safe_len4(qv->x, qv->y, qv->z, qv->w);
    if (len == 0.0 || !isfinite(len))
        return quat_alloc(0.0, 0.0, 0.0, 0.0);
    double inv = 1.0 / len;
    return quat_alloc(qv->x * inv, qv->y * inv, qv->z * inv, qv->w * inv);
}

/// @brief Compute a quaternion's Euclidean norm.
/// @details Uses chained `hypot` calls to resist intermediate overflow. Non-finite components
///          produce positive infinity; an invalid handle raises a runtime trap.
/// @param q Quat operand.
/// @return Quaternion norm, positive infinity for non-finite components, or 0.0 after an invalid
///         handle trap.
double rt_quat_len(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Len: invalid quaternion");
    if (!qv)
        return 0.0;
    return quat_safe_len4(qv->x, qv->y, qv->z, qv->w);
}

/// @brief Compute a quaternion's squared Euclidean norm.
/// @details Directly sums the squares of all four components, so extreme finite values may
///          overflow. An invalid handle raises a runtime trap.
/// @param q Quat operand.
/// @return Sum `x*x + y*y + z*z + w*w`, or 0.0 after an invalid-handle trap.
double rt_quat_len_sq(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.LenSq: invalid quaternion");
    if (!qv)
        return 0.0;
    return qv->x * qv->x + qv->y * qv->y + qv->z * qv->z + qv->w * qv->w;
}

/// @brief Compute the four-component dot product of two quaternions.
/// @param a Left-hand Quat operand.
/// @param b Right-hand Quat operand.
/// @return Scalar dot product, or 0.0 after trapping when either operand is invalid.
double rt_quat_dot(void *a, void *b) {
    ZannaQuat *qa = quat_checked(a, "Quat.Dot: invalid quaternion");
    ZannaQuat *qb = quat_checked(b, "Quat.Dot: invalid quaternion");
    if (!qa || !qb)
        return 0.0;
    return qa->x * qb->x + qa->y * qb->y + qa->z * qb->z + qa->w * qb->w;
}

//=============================================================================
// Interpolation
//=============================================================================

/// @brief Spherically interpolate between two unit quaternions.
/// @details Clamps @p t to [0, 1], chooses the shorter arc by conditionally negating @p b, and
///          uses a non-normalized linear blend when the inputs are nearly aligned. A non-finite
///          interpolation parameter traps and returns NULL. A non-finite input dot product traps
///          and returns identity.
/// @param a Unit Quat at interpolation parameter zero.
/// @param b Unit Quat at interpolation parameter one.
/// @param t Interpolation parameter, clamped to the inclusive range [0, 1].
/// @return New interpolated Quat, identity after a non-finite-dot trap, or NULL after other input
///         or allocation failures.
void *rt_quat_slerp(void *a, void *b, double t) {
    if (!isfinite(t)) {
        rt_trap("Quat.Slerp: non-finite interpolation parameter");
        return NULL;
    }
    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;
    ZannaQuat *qa = quat_checked(a, "Quat.Slerp: invalid start quaternion");
    ZannaQuat *qb = quat_checked(b, "Quat.Slerp: invalid end quaternion");
    if (!qa || !qb)
        return NULL;

    double dot = qa->x * qb->x + qa->y * qb->y + qa->z * qb->z + qa->w * qb->w;

    /* If dot < 0, negate one to take the shorter arc. */
    double bx = qb->x;
    double by = qb->y;
    double bz = qb->z;
    double bw = qb->w;
    if (dot < 0.0) {
        dot = -dot;
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    if (!isfinite(dot)) {
        rt_trap("Quat.Slerp: non-finite quaternion dot");
        return quat_alloc(0.0, 0.0, 0.0, 1.0);
    }
    if (dot > 1.0)
        dot = 1.0;
    if (dot < -1.0)
        dot = -1.0;

    double s0, s1;
    if (dot > 0.9995) {
        /* Nearly identical — use linear interpolation to avoid division by ~0. */
        s0 = 1.0 - t;
        s1 = t;
    } else {
        double theta = acos(dot);
        double sin_theta = sin(theta);
        s0 = sin((1.0 - t) * theta) / sin_theta;
        s1 = sin(t * theta) / sin_theta;
    }

    return quat_alloc(
        s0 * qa->x + s1 * bx, s0 * qa->y + s1 * by, s0 * qa->z + s1 * bz, s0 * qa->w + s1 * bw);
}

/// @brief Linearly blend two quaternions and normalize the result.
/// @details Does not clamp @p t, so values outside [0, 1] extrapolate. Constant angular velocity is
///          not preserved. A zero or non-finite blend norm produces identity; invalid quaternion
///          handles raise a runtime trap.
/// @param a Quat at interpolation parameter zero.
/// @param b Quat at interpolation parameter one.
/// @param t Interpolation or extrapolation parameter.
/// @return New normalized blend, identity for a degenerate blend, or NULL after trapping for
///         invalid input or allocation failure.
void *rt_quat_lerp(void *a, void *b, double t) {
    ZannaQuat *qa = quat_checked(a, "Quat.Lerp: invalid quaternion");
    ZannaQuat *qb = quat_checked(b, "Quat.Lerp: invalid quaternion");
    if (!qa || !qb)
        return NULL;
    double omt = 1.0 - t;
    double x = omt * qa->x + t * qb->x;
    double y = omt * qa->y + t * qb->y;
    double z = omt * qa->z + t * qb->z;
    double w = omt * qa->w + t * qb->w;
    double len = quat_safe_len4(x, y, z, w);
    if (len == 0.0 || !isfinite(len))
        return quat_alloc(0.0, 0.0, 0.0, 1.0);
    double inv = 1.0 / len;
    return quat_alloc(x * inv, y * inv, z * inv, w * inv);
}

//=============================================================================
// Rotation
//=============================================================================

/// @brief Rotate a Vec3 with a unit quaternion.
/// @details Uses the optimized unit-quaternion expansion of `q * v * conjugate(q)` and does not
///          normalize @p q. A NULL vector or invalid quaternion raises a runtime trap.
/// @param q Unit Quat rotation.
/// @param v Vec3 to rotate.
/// @return New rotated Vec3, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_rotate_vec3(void *q, void *v) {
    if (!v) {
        rt_trap("Quat.RotateVec3: null argument");
        return NULL;
    }
    ZannaQuat *qv = quat_checked(q, "Quat.RotateVec3: invalid quaternion");
    if (!qv)
        return NULL;
    double vx = rt_vec3_x(v);
    double vy = rt_vec3_y(v);
    double vz = rt_vec3_z(v);

    /* Optimized q * v * q^-1 for unit quaternions (avoids full multiplication). */
    double tx = 2.0 * (qv->y * vz - qv->z * vy);
    double ty = 2.0 * (qv->z * vx - qv->x * vz);
    double tz = 2.0 * (qv->x * vy - qv->y * vx);

    double rx = vx + qv->w * tx + (qv->y * tz - qv->z * ty);
    double ry = vy + qv->w * ty + (qv->z * tx - qv->x * tz);
    double rz = vz + qv->w * tz + (qv->x * ty - qv->y * tx);

    return rt_vec3_new(rx, ry, rz);
}

/// @brief Convert a unit quaternion to a homogeneous 4x4 rotation matrix.
/// @details Expands the quaternion directly without normalizing it. Non-unit values therefore need
///          not produce an orthonormal rotation basis.
/// @param q Quat to convert.
/// @return New Mat4 representation, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_to_mat4(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.ToMat4: invalid quaternion");
    if (!qv)
        return NULL;
    double x = qv->x;
    double y = qv->y;
    double z = qv->z;
    double w = qv->w;

    double x2 = x + x;
    double y2 = y + y;
    double z2 = z + z;
    double xx = x * x2;
    double xy = x * y2;
    double xz = x * z2;
    double yy = y * y2;
    double yz = y * z2;
    double zz = z * z2;
    double wx = w * x2;
    double wy = w * y2;
    double wz = w * z2;

    /* Row-major 4x4 rotation matrix. */
    return rt_mat4_new(1.0 - (yy + zz),
                       xy - wz,
                       xz + wy,
                       0.0,
                       xy + wz,
                       1.0 - (xx + zz),
                       yz - wx,
                       0.0,
                       xz - wy,
                       yz + wx,
                       1.0 - (xx + yy),
                       0.0,
                       0.0,
                       0.0,
                       0.0,
                       1.0);
}

/// @brief Extract the normalized imaginary part as a rotation axis.
/// @details A non-finite or shorter-than-`1e-12` imaginary-part length has no meaningful axis and
///          produces the conventional fallback `(0, 0, 1)`.
/// @param q Quat to inspect.
/// @return New normalized-axis Vec3, fallback z-axis for a degenerate quaternion, or NULL after
///         trapping for invalid input or allocation failure.
void *rt_quat_axis(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Axis: invalid quaternion");
    if (!qv)
        return NULL;
    double len = quat_safe_len3(qv->x, qv->y, qv->z);
    if (!isfinite(len) || len < 1e-12)
        return rt_vec3_new(0.0, 0.0, 1.0); /* Identity — arbitrary axis. */
    double inv_s = 1.0 / len;
    return rt_vec3_new(qv->x * inv_s, qv->y * inv_s, qv->z * inv_s);
}

/// @brief Extract a quaternion's rotation angle in radians.
/// @details Normalizes the scalar component by the full quaternion norm and clamps it before
///          applying `2 * acos(w)`. A non-finite or shorter-than-`1e-12` norm returns zero; an
///          invalid handle raises a runtime trap.
/// @param q Quat to inspect.
/// @return Rotation angle in [0, 2*pi], or 0.0 for a degenerate or invalid quaternion.
double rt_quat_angle(void *q) {
    ZannaQuat *qv = quat_checked(q, "Quat.Angle: invalid quaternion");
    if (!qv)
        return 0.0;
    double len = quat_safe_len4(qv->x, qv->y, qv->z, qv->w);
    if (!isfinite(len) || len < 1e-12)
        return 0.0;
    double w_clamped = qv->w / len;
    if (w_clamped > 1.0)
        w_clamped = 1.0;
    if (w_clamped < -1.0)
        w_clamped = -1.0;
    return 2.0 * acos(w_clamped);
}
