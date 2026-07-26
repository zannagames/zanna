//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/math/rt_mat4.h
// Purpose: 4x4 matrix math for 3D transformations (Zanna.Mat4), supporting perspective/orthographic
// projection, translation, rotation, scaling, and composition.
//
// Key invariants:
//   - Matrices are stored in row-major order; right-multiply with column vectors.
//   - Mat4 objects are immutable; all operations return new matrices.
//   - Perspective and orthographic projection matrices are provided.
//   - Inversion traps and returns NULL for invalid or singular matrices.
//
// Ownership/Lifetime:
//   - Mat4 objects are runtime-managed and may be reused through a thread-local pool.
//   - Returned matrices and vectors require no explicit caller cleanup.
//
// Links: src/runtime/graphics/math/rt_mat4.c (implementation),
//        src/runtime/graphics/math/rt_vec3.h (vector operands and results),
//        src/runtime/graphics/math/rt_quat.h (related rotation representation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#define RT_MAT4_CLASS_ID INT64_C(-0x600204)

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Construction
//=========================================================================

/// @brief Create a 4x4 matrix from sixteen row-major elements.
/// @details Values are stored without normalization or affine-layout validation.
/// @param m00 Element at row 0, column 0.
/// @param m01 Element at row 0, column 1.
/// @param m02 Element at row 0, column 2.
/// @param m03 Element at row 0, column 3.
/// @param m10 Element at row 1, column 0.
/// @param m11 Element at row 1, column 1.
/// @param m12 Element at row 1, column 2.
/// @param m13 Element at row 1, column 3.
/// @param m20 Element at row 2, column 0.
/// @param m21 Element at row 2, column 1.
/// @param m22 Element at row 2, column 2.
/// @param m23 Element at row 2, column 3.
/// @param m30 Element at row 3, column 0.
/// @param m31 Element at row 3, column 1.
/// @param m32 Element at row 3, column 2.
/// @param m33 Element at row 3, column 3.
/// @return New runtime-managed Mat4 with the specified elements, or NULL if allocation fails.
void *rt_mat4_new(double m00,
                  double m01,
                  double m02,
                  double m03,
                  double m10,
                  double m11,
                  double m12,
                  double m13,
                  double m20,
                  double m21,
                  double m22,
                  double m23,
                  double m30,
                  double m31,
                  double m32,
                  double m33);

/// @brief Create a 4x4 identity matrix.
/// @return New identity Mat4, or NULL if allocation fails.
void *rt_mat4_identity(void);

/// @brief Create a 4x4 zero matrix.
/// @return New zero Mat4, or NULL if allocation fails.
void *rt_mat4_zero(void);

//=========================================================================
// 3D Transformation Factories
//=========================================================================

/// @brief Create a 3D translation matrix.
/// @param tx Translation along the X axis.
/// @param ty Translation along the Y axis.
/// @param tz Translation along the Z axis.
/// @return New affine Mat4 encoding the translation, or NULL if allocation fails.
void *rt_mat4_translate(double tx, double ty, double tz);

/// @brief Create a 3D non-uniform scaling matrix.
/// @param sx Scale factor along the X axis.
/// @param sy Scale factor along the Y axis.
/// @param sz Scale factor along the Z axis.
/// @return New affine Mat4 encoding the scale, or NULL if allocation fails.
void *rt_mat4_scale(double sx, double sy, double sz);

/// @brief Create a uniform 3D scaling matrix.
/// @param s Uniform scale factor applied to all three axes.
/// @return New affine Mat4 encoding the scale, or NULL if allocation fails.
void *rt_mat4_scale_uniform(double s);

/// @brief Create a right-handed rotation matrix around the x axis.
/// @details A non-finite angle produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return New rotation Mat4, or an identity Mat4 for a non-finite angle.
void *rt_mat4_rotate_x(double angle);

/// @brief Create a right-handed rotation matrix around the y axis.
/// @details A non-finite angle produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return New rotation Mat4, or an identity Mat4 for a non-finite angle.
void *rt_mat4_rotate_y(double angle);

/// @brief Create a right-handed rotation matrix around the z axis.
/// @details A non-finite angle produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return New rotation Mat4, or an identity Mat4 for a non-finite angle.
void *rt_mat4_rotate_z(double angle);

/// @brief Create a right-handed rotation matrix around an arbitrary axis.
/// @details Normalizes @p axis internally using an overflow-resistant length. NULL, non-finite, or
///          near-zero axes and non-finite angles produce an identity matrix.
/// @param axis Vec3 rotation axis; it need not be normalized.
/// @param angle Rotation angle in radians.
/// @return New rotation Mat4, or an identity Mat4 for invalid input.
void *rt_mat4_rotate_axis(void *axis, double angle);

//=========================================================================
// Projection Matrices
//=========================================================================

/// @brief Create a perspective projection matrix.
/// @details Uses a right-handed coordinate system and maps depth to normalized device coordinates
///          in [-1, 1]. Non-finite or geometrically invalid arguments produce identity.
/// @param fov Vertical field of view in radians.
/// @param aspect Aspect ratio (width / height).
/// @param near_val Distance to the near clipping plane (must be positive).
/// @param far_val Distance to the far clipping plane (must be > near_val).
/// @return New projection Mat4, or an identity Mat4 for invalid input.
void *rt_mat4_perspective(double fov, double aspect, double near_val, double far_val);

/// @brief Create an orthographic projection matrix.
/// @details All plane coordinates must be finite and opposing planes must differ. Invalid or
///          degenerate bounds produce an identity matrix.
/// @param left Left boundary of the view volume.
/// @param right Right boundary of the view volume.
/// @param bottom Bottom boundary of the view volume.
/// @param top Top boundary of the view volume.
/// @param near_val Near clipping plane distance.
/// @param far_val Far clipping plane distance.
/// @return New projection Mat4 mapping the box to normalized device coordinates, or identity for
///         invalid input.
void *rt_mat4_ortho(
    double left, double right, double bottom, double top, double near_val, double far_val);

/// @brief Create a right-handed look-at view matrix.
/// @details Normalizes the viewing basis internally. NULL or non-finite vectors, a coincident eye
///          and target, or an up direction parallel to the view direction produce identity.
/// @param eye Camera position (Vec3).
/// @param target Look-at target point (Vec3).
/// @param up Up direction vector (Vec3, need not be unit length).
/// @return New world-to-view Mat4, or an identity Mat4 for invalid or degenerate input.
void *rt_mat4_look_at(void *eye, void *target, void *up);

//=========================================================================
// Element Access
//=========================================================================

/// @brief Read one matrix element by zero-based row and column.
/// @param m Mat4 handle to inspect.
/// @param row Row index in the inclusive range [0, 3].
/// @param col Column index in the inclusive range [0, 3].
/// @return Selected element, or 0.0 for an invalid matrix or out-of-range coordinate.
double rt_mat4_get(void *m, int64_t row, int64_t col);

//=========================================================================
// Arithmetic
//=========================================================================

/// @brief Add two matrices element by element.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @return New sum Mat4, or a zero Mat4 when either operand is invalid.
void *rt_mat4_add(void *a, void *b);

/// @brief Subtract one matrix from another element by element.
/// @param a Mat4 minuend.
/// @param b Mat4 subtrahend.
/// @return New Mat4 containing `a - b`, or a zero Mat4 when either operand is invalid.
void *rt_mat4_sub(void *a, void *b);

/// @brief Multiply two matrices using standard row-by-column multiplication.
/// @details For column-vector transforms, the right-hand transform is applied first.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @return New Mat4 containing `a * b`, or an identity Mat4 for invalid input.
void *rt_mat4_mul(void *a, void *b);

/// @brief Multiply every matrix element by a scalar.
/// @param m Mat4 operand.
/// @param s Scalar multiplier.
/// @return New scaled Mat4, or a zero Mat4 for an invalid matrix.
void *rt_mat4_mul_scalar(void *m, double s);

/// @brief Transform a 3D point (applies translation).
/// @details Treats @p v as `(x, y, z, 1)` and divides the result by homogeneous w when needed.
///          Invalid or non-finite input/output and results with near-zero w produce a zero vector.
/// @param m Mat4 transform.
/// @param v Vec3 point to transform.
/// @return New transformed Vec3, or a zero Vec3 when transformation is invalid.
void *rt_mat4_transform_point(void *m, void *v);

/// @brief Transform a 3D vector (ignores translation).
/// @details Treats @p v as `(x, y, z, 0)` and uses only the upper-left 3x3 matrix block.
/// @param m Mat4 transform.
/// @param v Vec3 direction to transform.
/// @return New transformed Vec3, or a zero Vec3 for invalid input.
void *rt_mat4_transform_vec(void *m, void *v);

//=========================================================================
// Matrix Operations
//=========================================================================

/// @brief Create the mathematical transpose of a matrix.
/// @param m Mat4 operand.
/// @return New Mat4 with rows and columns exchanged, or an identity Mat4 for invalid input.
void *rt_mat4_transpose(void *m);

/// @brief Compute a 4x4 matrix determinant.
/// @param m Mat4 operand.
/// @return Scalar determinant, or 0.0 for an invalid matrix.
double rt_mat4_det(void *m);

/// @brief Invert a matrix, trapping when inversion cannot be completed.
/// @details A determinant that is non-finite or has magnitude below `1e-15` is singular. NULL,
///          incompatible, singular, and allocation-failure paths return NULL after trapping rather
///          than manufacturing an identity transform.
/// @param m Mat4 to invert.
/// @return New inverse Mat4, or NULL after trapping when inversion fails.
void *rt_mat4_inverse(void *m);

/// @brief Non-trapping inverse for internal engine use.
/// @param m Mat4 to invert.
/// @return New inverse Mat4, or NULL without trapping for invalid input, a singular matrix, or
///         allocation failure.
void *rt_mat4_try_inverse(void *m);

/// @brief Negate every matrix element.
/// @param m Mat4 operand.
/// @return New negated Mat4, or a zero Mat4 for invalid input.
void *rt_mat4_neg(void *m);

//=========================================================================
// Comparison
//=========================================================================

/// @brief Check if two matrices are approximately equal.
/// @details Two NULL handles compare equal. A non-positive or non-finite tolerance is replaced with
///          `1e-9`; NaN elements and incompatible non-NULL handles compare unequal.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @param epsilon Maximum absolute difference permitted for each corresponding element.
/// @return 1 if all sixteen elements compare within tolerance, otherwise 0.
int8_t rt_mat4_eq(void *a, void *b, double epsilon);

#ifdef __cplusplus
}
#endif
