//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/math/rt_mat3.h
// Purpose: 3x3 matrix math for 2D transformations (Zanna.Mat3), supporting translation, rotation,
// scaling, and composition in row-major layout.
//
// Key invariants:
//   - Matrices are stored in row-major order; right-multiply with column vectors.
//   - Mat3 objects are immutable; all operations return new matrices.
//   - Identity, translate, rotate, scale, and multiply operations are provided.
//   - Inversion traps and returns NULL for invalid or singular matrices.
//
// Ownership/Lifetime:
//   - Mat3 objects are managed by the runtime garbage collector.
//   - Returned matrices and vectors require no explicit caller cleanup.
//
// Links: src/runtime/graphics/math/rt_mat3.c (implementation),
//        src/runtime/graphics/math/rt_vec2.h and rt_vec3.h (vector results)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#define RT_MAT3_CLASS_ID INT64_C(-0x600207)

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Construction
//=========================================================================

/// @brief Create a 3x3 matrix from nine row-major elements.
/// @details Values are stored without normalization or affine-layout validation:
///          | m00 m01 m02 |
///          | m10 m11 m12 |
///          | m20 m21 m22 |
/// @param m00 Element at row 0, column 0.
/// @param m01 Element at row 0, column 1.
/// @param m02 Element at row 0, column 2.
/// @param m10 Element at row 1, column 0.
/// @param m11 Element at row 1, column 1.
/// @param m12 Element at row 1, column 2.
/// @param m20 Element at row 2, column 0.
/// @param m21 Element at row 2, column 1.
/// @param m22 Element at row 2, column 2.
/// @return New GC-managed Mat3 with the specified elements, or NULL if allocation fails.
void *rt_mat3_new(double m00,
                  double m01,
                  double m02,
                  double m10,
                  double m11,
                  double m12,
                  double m20,
                  double m21,
                  double m22);

/// @brief Create a 3x3 identity matrix.
/// @return New GC-managed Mat3 with ones on the diagonal and zeros elsewhere, or NULL if
///         allocation fails.
void *rt_mat3_identity(void);

/// @brief Create a 3x3 zero matrix.
/// @return New GC-managed Mat3 with all elements set to zero, or NULL if allocation fails.
void *rt_mat3_zero(void);

//=========================================================================
// 2D Transformation Factories
//=========================================================================

/// @brief Create a 2D translation matrix.
/// @param tx Translation applied along the x axis.
/// @param ty Translation applied along the y axis.
/// @return New GC-managed affine Mat3 encoding the translation, or NULL if allocation fails.
void *rt_mat3_translate(double tx, double ty);

/// @brief Create a 2D non-uniform scaling matrix.
/// @param sx Scale factor applied along the x axis.
/// @param sy Scale factor applied along the y axis.
/// @return New GC-managed affine Mat3 encoding the scale, or NULL if allocation fails.
void *rt_mat3_scale(double sx, double sy);

/// @brief Create a 2D uniform scaling matrix.
/// @param s Scale factor applied along both axes.
/// @return New GC-managed affine Mat3 encoding the scale, or NULL if allocation fails.
void *rt_mat3_scale_uniform(double s);

/// @brief Create a 2D rotation matrix (counter-clockwise).
/// @details A non-finite angle is treated as zero and produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return New GC-managed affine Mat3 encoding the rotation, or NULL if allocation fails.
void *rt_mat3_rotate(double angle);

/// @brief Create a 2D shear matrix.
/// @details Produces `x' = x + sx * y` and `y' = sy * x + y`.
/// @param sx Contribution of the input y coordinate to the output x coordinate.
/// @param sy Contribution of the input x coordinate to the output y coordinate.
/// @return New GC-managed affine Mat3 encoding the shear, or NULL if allocation fails.
void *rt_mat3_shear(double sx, double sy);

//=========================================================================
// Element Access
//=========================================================================

/// @brief Read one matrix element by zero-based row and column.
/// @details Out-of-range coordinates return zero. An incompatible matrix handle raises a runtime
///          trap and also returns zero if execution resumes.
/// @param m Mat3 handle to inspect.
/// @param row Row index in the inclusive range [0, 2].
/// @param col Column index in the inclusive range [0, 2].
/// @return Selected element, or 0.0 for invalid coordinates or an invalid matrix.
double rt_mat3_get(void *m, int64_t row, int64_t col);

/// @brief Extract one matrix row into a new Vec3.
/// @details Invalid input produces a zero vector; an incompatible non-NULL matrix also raises a
///          runtime trap.
/// @param m Mat3 handle to inspect.
/// @param row Row index in the inclusive range [0, 2].
/// @return New Vec3 containing the row, or a zero Vec3 for invalid input.
void *rt_mat3_row(void *m, int64_t row);

/// @brief Extract one matrix column into a new Vec3.
/// @details Invalid input produces a zero vector; an incompatible non-NULL matrix also raises a
///          runtime trap.
/// @param m Mat3 handle to inspect.
/// @param col Column index in the inclusive range [0, 2].
/// @return New Vec3 containing the column, or a zero Vec3 for invalid input.
void *rt_mat3_col(void *m, int64_t col);

//=========================================================================
// Arithmetic
//=========================================================================

/// @brief Add two matrices element by element.
/// @details Invalid operands produce a zero matrix; incompatible non-NULL handles also raise a
///          runtime trap.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @return New Mat3 containing the sum, or a zero Mat3 for invalid input.
void *rt_mat3_add(void *a, void *b);

/// @brief Subtract one matrix from another element by element.
/// @details Invalid operands produce a zero matrix; incompatible non-NULL handles also raise a
///          runtime trap.
/// @param a Mat3 minuend.
/// @param b Mat3 subtrahend.
/// @return New Mat3 containing `a - b`, or a zero Mat3 for invalid input.
void *rt_mat3_sub(void *a, void *b);

/// @brief Multiply two matrices using standard row-by-column multiplication.
/// @details For column-vector transforms, the right-hand transform is applied first. Invalid
///          operands produce an identity matrix; incompatible non-NULL handles also raise a trap.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @return New Mat3 containing `a * b`, or an identity Mat3 for invalid input.
void *rt_mat3_mul(void *a, void *b);

/// @brief Multiply every matrix element by a scalar.
/// @details An invalid matrix produces a zero matrix; an incompatible non-NULL handle also raises a
///          runtime trap.
/// @param m Mat3 operand.
/// @param s Scalar multiplier.
/// @return New scaled Mat3, or a zero Mat3 for an invalid matrix.
void *rt_mat3_mul_scalar(void *m, double s);

/// @brief Transform a 2D point (applies translation).
/// @details Uses the first two matrix rows and treats @p v as the homogeneous column vector
///          `(x, y, 1)`. Invalid input produces a zero vector; an incompatible matrix also traps.
/// @param m Mat3 transform.
/// @param v Vec2 point to transform.
/// @return New transformed Vec2, or a zero Vec2 for invalid input.
void *rt_mat3_transform_point(void *m, void *v);

/// @brief Transform a 2D vector (ignores translation).
/// @details Uses the first two matrix rows and treats @p v as the homogeneous column vector
///          `(x, y, 0)`. Invalid input produces a zero vector; an incompatible matrix also traps.
/// @param m Mat3 transform.
/// @param v Vec2 direction to transform.
/// @return New transformed Vec2, or a zero Vec2 for invalid input.
void *rt_mat3_transform_vec(void *m, void *v);

//=========================================================================
// Matrix Operations
//=========================================================================

/// @brief Create the transpose of a matrix.
/// @details Invalid input produces an identity matrix; an incompatible non-NULL handle also raises
///          a runtime trap.
/// @param m Mat3 to transpose.
/// @return New Mat3 with rows and columns exchanged, or an identity Mat3 for invalid input.
void *rt_mat3_transpose(void *m);

/// @brief Compute a matrix determinant by cofactor expansion.
/// @details Invalid input returns zero; an incompatible non-NULL handle also raises a runtime trap.
/// @param m Mat3 operand.
/// @return Scalar determinant, or 0.0 for an invalid matrix.
double rt_mat3_det(void *m);

/// @brief Invert a matrix using its adjugate and determinant.
/// @details Determinants that are non-finite or have magnitude below `1e-15` are considered
///          singular. Invalid and singular inputs raise a runtime trap.
/// @param m Mat3 to invert.
/// @return New inverse Mat3, or NULL after trapping for invalid or singular input.
void *rt_mat3_inverse(void *m);

/// @brief Negate every matrix element.
/// @details Invalid input produces a zero matrix; an incompatible non-NULL handle also raises a
///          runtime trap.
/// @param m Mat3 to negate.
/// @return New negated Mat3, or a zero Mat3 for invalid input.
void *rt_mat3_neg(void *m);

//=========================================================================
// Comparison
//=========================================================================

/// @brief Check if two matrices are approximately equal.
/// @details Two NULL handles compare equal. A non-positive or non-finite tolerance is replaced by
///          `1e-9`, and any NaN matrix element makes the comparison fail. Incompatible non-NULL
///          handles raise a runtime trap.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @param epsilon Maximum absolute difference permitted for each corresponding element.
/// @return 1 if all nine elements compare within tolerance, otherwise 0.
int8_t rt_mat3_eq(void *a, void *b, double epsilon);

#ifdef __cplusplus
}
#endif
