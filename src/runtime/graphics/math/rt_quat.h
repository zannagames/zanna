//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/math/rt_quat.h
// Purpose: Quaternion math for 3D rotation (Zanna.Quat), providing construction from
// axis/angle/Euler, conjugate, product, SLERP, and conversion to/from rotation matrices.
//
// Key invariants:
//   - Quaternions are stored as (x, y, z, w) where w is the scalar part.
//   - Unit quaternions represent pure 3D rotations; conversion and vector-rotation routines do not
//     normalize their inputs.
//   - All operations return new Quat objects; inputs are not modified.
//   - SLERP correctly handles antipodal quaternions by negating one operand.
//
// Ownership/Lifetime:
//   - Quat objects are managed by the runtime garbage collector.
//   - Returned quaternions, matrices, and vectors require no explicit caller cleanup.
//
// Links: src/runtime/graphics/math/rt_quat.c (implementation),
//        src/runtime/graphics/math/rt_mat4.h (matrix conversion),
//        src/runtime/graphics/math/rt_vec3.h (axis and vector operations)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares immutable quaternion operations for three-dimensional rotation.
 *
 * @details Quat exposes raw, identity, axis-angle, and Euler construction;
 *          component access; Hamilton composition; conjugate, inverse, norm,
 *          length, and dot operations; interpolation; vector rotation; matrix
 *          conversion; and axis/angle extraction.
 */

#pragma once

#include <stdint.h>

/// @brief Runtime class identifier assigned to GC-managed Quat objects.
#define RT_QUAT_CLASS_ID INT64_C(-0x600208)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a quaternion from raw vector and scalar components.
/// @details Stores the values without normalizing them.
/// @param x The first imaginary component (i).
/// @param y The second imaginary component (j).
/// @param z The third imaginary component (k).
/// @param w The scalar (real) component.
/// @return New GC-managed Quat, or NULL after trapping if allocation fails.
void *rt_quat_new(double x, double y, double z, double w);

/// @brief Create the identity quaternion (0, 0, 0, 1).
/// @return New identity Quat, or NULL after trapping if allocation fails.
void *rt_quat_identity(void);

/// @brief Create a quaternion from axis-angle representation.
/// @details Normalizes @p axis internally. A zero or non-finite axis or a non-finite angle produces
///          identity; a NULL axis raises a runtime trap.
/// @param axis A Vec3 representing the rotation axis (will be normalized).
/// @param angle Rotation angle in radians.
/// @return New unit Quat, identity for a degenerate value, or NULL after trapping for a NULL axis
///         or allocation failure.
void *rt_quat_from_axis_angle(void *axis, double angle);

/// @brief Create a quaternion from Euler angles (pitch, yaw, roll) in radians.
/// @details Forms `qz(roll) * qy(yaw) * qx(pitch)`, applying fixed-axis pitch, then yaw, then roll
///          to column vectors. Non-finite input produces identity.
/// @param pitch Rotation about the X axis in radians.
/// @param yaw Rotation about the Y axis in radians.
/// @param roll Rotation about the Z axis in radians.
/// @return New unit Quat, identity for non-finite input, or NULL after an allocation trap.
void *rt_quat_from_euler(double pitch, double yaw, double roll);

/// @brief Decompose a rotation into (pitch, yaw, roll) radians (ADR 0227).
/// @details Exact inverse of rt_quat_from_euler up to quaternion sign; at the
///          gimbal poles pitch reports zero and roll carries the shared twist.
/// @param q Quat handle to decompose.
/// @return New Vec3 of Euler radians, or NULL after trapping for an invalid handle.
void *rt_quat_to_euler(void *q);

/// @brief Extract a matrix's rotation as a normalized quaternion (ADR 0227).
/// @details Column-normalizes the upper-left 3x3 basis first, so scaled
///          transforms decompose by their rotation; degenerate bases yield identity.
/// @param m Mat4 handle whose rotation is extracted.
/// @return New rotation Quat, or NULL after trapping for an invalid handle.
void *rt_quat_from_mat4(void *m);

/// @brief Read the x component of a quaternion's imaginary part.
/// @param q Quat handle to inspect.
/// @return Stored x component, or 0.0 after trapping for an invalid handle.
double rt_quat_x(void *q);

/// @brief Read the y component of a quaternion's imaginary part.
/// @param q Quat handle to inspect.
/// @return Stored y component, or 0.0 after trapping for an invalid handle.
double rt_quat_y(void *q);

/// @brief Read the z component of a quaternion's imaginary part.
/// @param q Quat handle to inspect.
/// @return Stored z component, or 0.0 after trapping for an invalid handle.
double rt_quat_z(void *q);

/// @brief Read a quaternion's scalar w component.
/// @param q Quat handle to inspect.
/// @return Stored w component, or 0.0 after trapping for an invalid handle.
double rt_quat_w(void *q);

/// @brief Compute the Hamilton product of two quaternions.
/// @details For unit rotations, the right-hand rotation is applied first. Invalid handles raise a
///          runtime trap.
/// @param a Left-hand Quat operand.
/// @param b Right-hand Quat operand.
/// @return New product Quat, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_mul(void *a, void *b);

/// @brief Create a quaternion's conjugate `(-x, -y, -z, w)`.
/// @details For a unit quaternion, the conjugate is also its inverse.
/// @param q Quat operand.
/// @return New conjugate Quat, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_conjugate(void *q);

/// @brief Compute a quaternion inverse as `conjugate(q) / |q|^2`.
/// @details Uses an overflow-resistant norm and successive division. Invalid, zero-length, and
///          non-finite quaternions raise a runtime trap.
/// @param q Quat operand.
/// @return New inverse Quat, or NULL after trapping when inversion or allocation fails.
void *rt_quat_inverse(void *q);

/// @brief Normalize a quaternion to unit length.
/// @details A zero or non-finite norm produces the zero quaternion; an invalid handle traps.
/// @param q Quat operand.
/// @return New unit Quat, zero Quat for a degenerate norm, or NULL after trapping for invalid input
///         or allocation failure.
void *rt_quat_norm(void *q);

/// @brief Compute a quaternion's Euclidean norm.
/// @details Uses an overflow-resistant chained-hypot calculation.
/// @param q Quat operand.
/// @return Euclidean norm, positive infinity for non-finite components, or 0.0 after an
///         invalid-handle trap.
double rt_quat_len(void *q);

/// @brief Compute a quaternion's squared Euclidean norm.
/// @details Direct squaring can overflow for extreme finite components.
/// @param q Quat operand.
/// @return Sum `x*x + y*y + z*z + w*w`, or 0.0 after an invalid-handle trap.
double rt_quat_len_sq(void *q);

/// @brief Dot product of two quaternions.
/// @param a Left-hand Quat operand.
/// @param b Right-hand Quat operand.
/// @return Four-component dot product, or 0.0 after trapping for invalid input.
double rt_quat_dot(void *a, void *b);

/// @brief Spherical linear interpolation between two quaternions.
/// @details Clamps @p t to [0, 1], takes the shortest arc, and uses a non-normalized linear blend
///          for nearly aligned unit inputs. Non-finite @p t traps and returns NULL; a non-finite
///          quaternion dot product traps and returns identity.
/// @param a Unit Quat at interpolation parameter zero.
/// @param b Unit Quat at interpolation parameter one.
/// @param t Interpolation parameter, clamped to [0, 1].
/// @return New interpolated Quat, identity for a non-finite dot product, or NULL after other input
///         or allocation failures.
void *rt_quat_slerp(void *a, void *b, double t);

/// @brief Linearly blend two quaternions and normalize the result.
/// @details Does not clamp @p t, allowing extrapolation. A zero or non-finite blend norm produces
///          identity; invalid quaternion handles raise a runtime trap.
/// @param a Quat at interpolation parameter zero.
/// @param b Quat at interpolation parameter one.
/// @param t Interpolation or extrapolation parameter.
/// @return New normalized blend, identity for a degenerate blend, or NULL after trapping for
///         invalid input or allocation failure.
void *rt_quat_lerp(void *a, void *b, double t);

/// @brief Rotate a Vec3 with a unit quaternion.
/// @details Uses an optimized unit-quaternion formula and does not normalize @p q.
/// @param q Unit Quat rotation.
/// @param v Vec3 to rotate.
/// @return New rotated Vec3, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_rotate_vec3(void *q, void *v);

/// @brief Convert a unit quaternion to a homogeneous 4x4 rotation matrix.
/// @details Expands the components directly without normalizing @p q.
/// @param q Unit Quat to convert.
/// @return New Mat4 representation, or NULL after trapping for invalid input or allocation failure.
void *rt_quat_to_mat4(void *q);

/// @brief Extract the normalized imaginary part as a rotation axis.
/// @details A non-finite or near-zero imaginary part produces the conventional z-axis fallback.
/// @param q Quat to inspect.
/// @return New unit axis Vec3, `(0, 0, 1)` for a degenerate quaternion, or NULL after trapping for
///         invalid input or allocation failure.
void *rt_quat_axis(void *q);

/// @brief Extract the rotation angle in radians.
/// @details Normalizes and clamps the scalar component before applying `2 * acos(w)`. A non-finite
///          or near-zero norm produces zero.
/// @param q Quat to inspect.
/// @return Rotation angle in [0, 2*pi], or 0.0 for a degenerate or invalid quaternion.
double rt_quat_angle(void *q);

#ifdef __cplusplus
}
#endif
