//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/math/rt_vec3.h
// Purpose: 3D vector math utilities for Zanna.Vec3 with pure arithmetic helpers and explicit
// in-place mutators for hot script paths.
//
// Key invariants:
//   - Pure arithmetic operations return new Vec3 objects.
//   - Mutator functions are explicitly named Set*/CopyFrom and update the receiver in place.
//   - All operations are done in double-precision floating point.
//   - Normalize returns Vec3(0,0,0) for zero or non-finite length.
//   - Cross product follows the right-hand rule.
//
// Ownership/Lifetime:
//   - Vec3 objects are runtime-managed (heap-allocated, GC'd via thread-local pool).
//   - The thread-local pool (P2-3.6) resurrects Vec3 objects on finalization for reuse.
//
// Links: src/runtime/graphics/math/rt_vec3.c (implementation),
//        src/runtime/graphics/math/rt_mat4.h (3D matrix transforms),
//        src/runtime/graphics/math/rt_quat.h (quaternion rotations)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the runtime-managed three-dimensional vector API.
 * @details Exposes Vec3 construction, component access and mutation, arithmetic,
 * geometric products, interpolation, projection, reflection, magnitude
 * limiting, and comparison helpers. Unless explicitly identified as a mutator,
 * operations return a newly managed Vec3 and leave their operands unchanged.
 */
#pragma once

#include <stdint.h>

/** Runtime class identifier stored on managed Vec3 payloads. */
#define RT_VEC3_CLASS_ID INT64_C(-0x600206)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new Vec3 with given x, y, and z components.
/// @param x The x component.
/// @param y The y component.
/// @param z The z component.
/// @return New runtime-managed Vec3, or NULL after trapping if allocation fails.
void *rt_vec3_new(double x, double y, double z);

/// @brief Create a new Vec3 at origin (0, 0, 0).
/// @return New zero Vec3, or NULL after trapping if allocation fails.
void *rt_vec3_zero(void);

/// @brief Create a new Vec3 (1, 1, 1).
/// @return New `(1, 1, 1)` Vec3, or NULL after trapping if allocation fails.
void *rt_vec3_one(void);

/// @brief Get the X component.
/// @param v The Vec3 object.
/// @return Stored x component, or 0.0 after trapping for an invalid handle.
double rt_vec3_x(void *v);

/// @brief Get the Y component.
/// @param v The Vec3 object.
/// @return Stored y component, or 0.0 after trapping for an invalid handle.
double rt_vec3_y(void *v);

/// @brief Get the Z component.
/// @param v The Vec3 object.
/// @return Stored z component, or 0.0 after trapping for an invalid handle.
double rt_vec3_z(void *v);

/// @brief Set the X component in place.
/// @details An invalid handle raises a runtime trap and is left untouched.
/// @param v The Vec3 object.
/// @param x New X component.
void rt_vec3_set_x(void *v, double x);

/// @brief Set the Y component in place.
/// @details An invalid handle raises a runtime trap and is left untouched.
/// @param v The Vec3 object.
/// @param y New Y component.
void rt_vec3_set_y(void *v, double y);

/// @brief Set the Z component in place.
/// @details An invalid handle raises a runtime trap and is left untouched.
/// @param v The Vec3 object.
/// @param z New Z component.
void rt_vec3_set_z(void *v, double z);

/// @brief Set all components in place.
/// @details An invalid handle raises a runtime trap and is left untouched.
/// @param v The Vec3 object.
/// @param x New X component.
/// @param y New Y component.
/// @param z New Z component.
void rt_vec3_set(void *v, double x, double y, double z);

/// @brief Copy all components from another Vec3 into @p v.
/// @details Both handles are validated before assignment; invalid input traps without copying.
/// @param v Destination Vec3 object.
/// @param other Source Vec3 object.
void rt_vec3_copy_from(void *v, void *other);

/// @brief Add two vectors: a + b.
/// @param a The first Vec3 operand.
/// @param b The second Vec3 operand.
/// @return A new Vec3 representing the component-wise sum
///         (a.x+b.x, a.y+b.y, a.z+b.z).
void *rt_vec3_add(void *a, void *b);

/// @brief Subtract two vectors: a - b.
/// @param a The Vec3 minuend.
/// @param b The Vec3 subtrahend.
/// @return A new Vec3 representing the component-wise difference
///         (a.x-b.x, a.y-b.y, a.z-b.z).
void *rt_vec3_sub(void *a, void *b);

/// @brief Multiply vector by scalar: v * s.
/// @param v The Vec3 operand.
/// @param s The scalar multiplier.
/// @return A new Vec3 with each component multiplied by @p s.
void *rt_vec3_mul(void *v, double s);

/// @brief Divide vector by scalar: v / s.
/// @param v The Vec3 operand.
/// @param s The scalar divisor; it must be finite and nonzero.
/// @return New quotient Vec3, or NULL after trapping for invalid input or allocation failure.
void *rt_vec3_div(void *v, double s);

/// @brief Dot product of two vectors.
/// @param a The first Vec3 operand.
/// @param b The second Vec3 operand.
/// @return The scalar dot product (a.x*b.x + a.y*b.y + a.z*b.z).
double rt_vec3_dot(void *a, void *b);

/// @brief Cross product of two vectors (returns Vec3).
/// @param a The first Vec3 operand (left-hand side).
/// @param b The second Vec3 operand (right-hand side).
/// @return A new Vec3 perpendicular to both @p a and @p b, following
///         the right-hand rule. The magnitude equals |a|*|b|*sin(theta).
void *rt_vec3_cross(void *a, void *b);

/// @brief Length (magnitude) of vector.
/// @param v The Vec3 object.
/// @return Overflow-resistant Euclidean length, positive infinity for non-finite components, or
///         0.0 after an invalid-handle trap.
double rt_vec3_len(void *v);

/// @brief Squared length of vector (avoids sqrt).
/// @param v The Vec3 object.
/// @return The squared Euclidean length (x^2 + y^2 + z^2). Useful for
///         distance comparisons without the cost of a square root.
double rt_vec3_len_sq(void *v);

/// @brief Normalize a vector to unit length.
/// @param v The Vec3 object.
/// @return A new unit-length Vec3 pointing in the same direction as @p v,
///         the zero vector if its length is zero or non-finite, or NULL after an invalid-handle
///         or allocation trap.
void *rt_vec3_norm(void *v);

/// @brief Distance between two points (vectors).
/// @param a The first point as a Vec3.
/// @param b The second point as a Vec3.
/// @return The Euclidean distance between @p a and @p b.
double rt_vec3_dist(void *a, void *b);

/// @brief Linear interpolation between two vectors: a + (b - a) * t.
/// @param a The start Vec3 (returned when t = 0).
/// @param b The end Vec3 (returned when t = 1).
/// @param t The interpolation parameter, typically in [0, 1].
/// @return A new Vec3 representing the linearly interpolated position.
void *rt_vec3_lerp(void *a, void *b, double t);

/// @brief Negate vector: -v.
/// @param v The Vec3 object.
/// @return A new Vec3 with all components negated (-x, -y, -z).
void *rt_vec3_neg(void *v);

/* Game math helpers */
/// @brief Reflect a vector across a surface normal.
/// @details Normalizes @p normal internally. NULL, non-finite, or degenerate inputs produce a zero
///          vector; incompatible non-NULL handles also raise a runtime trap.
/// @param v Vec3 incident vector.
/// @param normal Vec3 surface normal; it need not be normalized.
/// @return New reflected Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_reflect(void *v, void *normal);

/// @brief Project a vector onto the line spanned by another vector.
/// @details NULL, non-finite, or degenerate inputs produce a zero vector; incompatible non-NULL
///          handles also raise a runtime trap.
/// @param v Vec3 to project.
/// @param onto Vec3 defining the target line.
/// @return New projected Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_project(void *v, void *onto);

/// @brief Clamp a vector's magnitude to a maximum length.
/// @details Vectors within the limit are copied unchanged. Invalid, non-positive, or non-finite
///          input produces zero; an incompatible vector handle also traps.
/// @param v Vec3 to clamp.
/// @param max_len Maximum permitted positive magnitude.
/// @return New clamped Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_clamp_len(void *v, double max_len);

/// @brief Move a point toward a target by at most a specified distance.
/// @details Snaps to the target when reachable. Negative/non-finite deltas or non-finite
///          separations return a copy of @p current; invalid vector handles produce zero.
/// @param current Vec3 starting point.
/// @param target Vec3 destination point.
/// @param max_delta Maximum distance to move.
/// @return New moved Vec3, a current-position copy for an unusable delta, or zero for invalid
///         vector handles.
void *rt_vec3_move_towards(void *current, void *target, double max_delta);

/// @brief Compute the unsigned angle between two vectors.
/// @param a First Vec3 direction.
/// @param b Second Vec3 direction.
/// @return Angle in [0, pi] radians, or 0.0 for invalid, non-finite, or near-zero input.
double rt_vec3_angle(void *a, void *b);

/// @brief Compute the component-wise minimum of two vectors.
/// @details Uses `fmin` per component. Invalid handles produce zero; incompatible non-NULL handles
///          also trap.
/// @param a First Vec3 operand.
/// @param b Second Vec3 operand.
/// @return New component-wise minimum Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_min(void *a, void *b);

/// @brief Compute the component-wise maximum of two vectors.
/// @details Uses `fmax` per component. Invalid handles produce zero; incompatible non-NULL handles
///          also trap.
/// @param a First Vec3 operand.
/// @param b Second Vec3 operand.
/// @return New component-wise maximum Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_max(void *a, void *b);

#ifdef __cplusplus
}
#endif
