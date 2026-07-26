//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_mat3.c
// Purpose: 3×3 matrix type for 2D affine transformations in Zanna. Supports
//   construction from rotation/scale/translation components, matrix–matrix
//   multiplication (transform concatenation), matrix–vector multiplication
//   (point/direction transform), inverse, transpose, and determinant. Used
//   internally by the camera, scene graph, and sprite systems to compose and
//   apply 2D spatial transforms.
//
// Key invariants:
//   - Matrix layout is row-major in memory:
//
//       | m00 m01 m02 |   | a  b  tx |
//       | m10 m11 m12 | = | c  d  ty |
//       | m20 m21 m22 |   | 0  0  1  |
//
//     For a pure 2D affine transform:  a, b, c, d encode rotation/scale/shear;
//     tx, ty encode translation. Affine factory matrices use bottom row [0,0,1];
//     New and arithmetic operations can produce an arbitrary bottom row.
//
//   - 2D point transformation (homogeneous coordinates):
//
//       x' = a*x + b*y + tx
//       y' = c*x + d*y + ty
//
//   - Rotation by θ (CCW positive):
//       a = cos θ,  b = -sin θ,  c = sin θ,  d = cos θ
//
//   - Transform concatenation: M_combined = M_parent × M_child (left-multiply).
//     Callers must apply transforms in the correct order for their coordinate
//     system convention.
//
//   - Mat3 objects are effectively immutable after creation: all operations
//     return new Mat3 objects rather than mutating the receiver. This makes them
//     safe to share across threads without locking.
//
// Ownership/Lifetime:
//   - Mat3 objects are GC-managed (rt_obj_new_i64). They hold only the 9
//     double fields inline (no external allocations) so no finalizer is needed.
//
// Links: src/runtime/graphics/math/rt_mat3.h (public API),
//        src/runtime/graphics/math/rt_vec2.h, rt_vec3.h (operand types),
//        src/runtime/graphics/2d/rt_camera.c (consumer for viewport transforms)
//
//===----------------------------------------------------------------------===//

#include "rt_mat3.h"

#include "rt_heap.h"
#include "rt_object.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdlib.h>

//=============================================================================
// Internal Structure
//=============================================================================

/// @brief 3x3 matrix stored in row-major order.
typedef struct mat3_impl {
    double m[9]; ///< Elements in row-major order: [row0][row1][row2]
} mat3_impl;

#define M(mat, r, c) ((mat)->m[(r) * 3 + (c)])

/// @brief Return whether @p m is a Mat3-compatible heap payload.
/// @details Accepts both the explicit Mat3 class id used by current constructors and the legacy
///          class-id-zero nine-double layout. Classless payloads must be exactly the Mat3 byte
///          size to avoid accepting unrelated heap blobs.
/// @param m Candidate runtime object payload.
/// @return 1 for a compatible Mat3 payload, otherwise 0.
static int mat3_is_compatible_object(void *m) {
    if (!m)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(m, &heap_info))
        return 0;
    if (heap_info.kind != RT_HEAP_OBJECT || heap_info.elem_kind != RT_ELEM_NONE)
        return 0;
    if (heap_info.class_id == RT_MAT3_CLASS_ID)
        return heap_info.cap >= sizeof(mat3_impl);
    return heap_info.class_id == 0 && heap_info.len == sizeof(mat3_impl) &&
           heap_info.cap == sizeof(mat3_impl);
}

/// @brief Validate and cast an opaque handle to a Mat3 payload.
/// @details Rejects NULL, non-object heap payloads, incompatible class identifiers, and
///   undersized allocations before any matrix elements are read.
/// @param m Candidate Mat3 runtime handle.
/// @param op Diagnostic prefix used if validation fails.
/// @return Typed Mat3 payload, or NULL after trapping.
static mat3_impl *mat3_checked(void *m, const char *op) {
    if (!mat3_is_compatible_object(m)) {
        rt_trap(op ? op : "Mat3: invalid matrix");
        return NULL;
    }
    return (mat3_impl *)m;
}

//=============================================================================
// Construction
//=============================================================================

/// @brief Construct a 3x3 matrix from nine row-major scalar values.
/// @details Stores the supplied values without normalization or affine-layout validation. The
///          resulting GC-managed object may therefore represent either a 2D homogeneous transform
///          or an arbitrary 3x3 matrix.
/// @param m00 Element at row 0, column 0.
/// @param m01 Element at row 0, column 1.
/// @param m02 Element at row 0, column 2.
/// @param m10 Element at row 1, column 0.
/// @param m11 Element at row 1, column 1.
/// @param m12 Element at row 1, column 2.
/// @param m20 Element at row 2, column 0.
/// @param m21 Element at row 2, column 1.
/// @param m22 Element at row 2, column 2.
/// @return Newly allocated Mat3 handle, or NULL if allocation fails.
void *rt_mat3_new(double m00,
                  double m01,
                  double m02,
                  double m10,
                  double m11,
                  double m12,
                  double m20,
                  double m21,
                  double m22) {
    mat3_impl *mat = (mat3_impl *)rt_obj_new_i64(RT_MAT3_CLASS_ID, sizeof(mat3_impl));
    if (!mat)
        return NULL;

    mat->m[0] = m00;
    mat->m[1] = m01;
    mat->m[2] = m02;
    mat->m[3] = m10;
    mat->m[4] = m11;
    mat->m[5] = m12;
    mat->m[6] = m20;
    mat->m[7] = m21;
    mat->m[8] = m22;

    return mat;
}

/// @brief Create a 3x3 identity matrix.
/// @return Newly allocated identity Mat3 handle, or NULL if allocation fails.
void *rt_mat3_identity(void) {
    return rt_mat3_new(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a 3x3 matrix whose elements are all zero.
/// @return Newly allocated zero Mat3 handle, or NULL if allocation fails.
void *rt_mat3_zero(void) {
    return rt_mat3_new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
}

//=============================================================================
// 2D Transformation Factories
//=============================================================================

/// @brief Create a homogeneous 2D translation matrix.
/// @param tx Translation applied along the x axis.
/// @param ty Translation applied along the y axis.
/// @return Newly allocated affine Mat3 handle, or NULL if allocation fails.
void *rt_mat3_translate(double tx, double ty) {
    return rt_mat3_new(1.0, 0.0, tx, 0.0, 1.0, ty, 0.0, 0.0, 1.0);
}

/// @brief Create a homogeneous 2D non-uniform scaling matrix.
/// @param sx Scale factor applied along the x axis.
/// @param sy Scale factor applied along the y axis.
/// @return Newly allocated affine Mat3 handle, or NULL if allocation fails.
void *rt_mat3_scale(double sx, double sy) {
    return rt_mat3_new(sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a homogeneous 2D uniform scaling matrix.
/// @param s Scale factor applied along both axes.
/// @return Newly allocated affine Mat3 handle, or NULL if allocation fails.
void *rt_mat3_scale_uniform(double s) {
    return rt_mat3_scale(s, s);
}

/// @brief Create a homogeneous 2D counter-clockwise rotation matrix.
/// @details A non-finite angle is treated as zero and produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return Newly allocated affine Mat3 handle, or NULL if allocation fails.
void *rt_mat3_rotate(double angle) {
    if (!isfinite(angle))
        return rt_mat3_identity();
    double c = cos(angle);
    double s = sin(angle);
    return rt_mat3_new(c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a homogeneous 2D shear matrix.
/// @details The transformed coordinates are `x' = x + sx * y` and `y' = sy * x + y`.
/// @param sx Amount by which the y coordinate contributes to the output x coordinate.
/// @param sy Amount by which the x coordinate contributes to the output y coordinate.
/// @return Newly allocated affine Mat3 handle, or NULL if allocation fails.
void *rt_mat3_shear(double sx, double sy) {
    return rt_mat3_new(1.0, sx, 0.0, sy, 1.0, 0.0, 0.0, 0.0, 1.0);
}

//=============================================================================
// Element Access
//=============================================================================

/// @brief Read one matrix element by zero-based row and column.
/// @details Out-of-range coordinates return zero without inspecting @p m. An invalid matrix handle
///          raises a runtime trap and returns zero if execution resumes.
/// @param m Mat3 handle to inspect.
/// @param row Zero-based row index in the inclusive range [0, 2].
/// @param col Zero-based column index in the inclusive range [0, 2].
/// @return Selected element, or 0.0 for invalid coordinates or an invalid matrix.
double rt_mat3_get(void *m, int64_t row, int64_t col) {
    mat3_impl *mat;
    if (row < 0 || row > 2 || col < 0 || col > 2)
        return 0.0;

    mat = mat3_checked(m, "Mat3.Get: invalid matrix");
    if (!mat)
        return 0.0;
    return M(mat, row, col);
}

/// @brief Extract one matrix row into a new Vec3.
/// @details A NULL handle or out-of-range row returns a zero vector. A non-NULL incompatible handle
///          also raises a runtime trap before returning the zero-vector fallback.
/// @param m Mat3 handle to inspect.
/// @param row Zero-based row index in the inclusive range [0, 2].
/// @return Newly allocated Vec3 containing the row, or a zero Vec3 for invalid input.
void *rt_mat3_row(void *m, int64_t row) {
    if (!m || row < 0 || row > 2)
        return rt_vec3_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.Row: invalid matrix");
    if (!mat)
        return rt_vec3_zero();
    return rt_vec3_new(M(mat, row, 0), M(mat, row, 1), M(mat, row, 2));
}

/// @brief Extract one matrix column into a new Vec3.
/// @details A NULL handle or out-of-range column returns a zero vector. A non-NULL incompatible
///          handle also raises a runtime trap before returning the zero-vector fallback.
/// @param m Mat3 handle to inspect.
/// @param col Zero-based column index in the inclusive range [0, 2].
/// @return Newly allocated Vec3 containing the column, or a zero Vec3 for invalid input.
void *rt_mat3_col(void *m, int64_t col) {
    if (!m || col < 0 || col > 2)
        return rt_vec3_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.Col: invalid matrix");
    if (!mat)
        return rt_vec3_zero();
    return rt_vec3_new(M(mat, 0, col), M(mat, 1, col), M(mat, 2, col));
}

//=============================================================================
// Arithmetic
//=============================================================================

/// @brief Add two matrices element by element.
/// @details NULL operands return a zero matrix. Non-NULL incompatible operands raise a runtime trap
///          before the same fallback is returned.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @return Newly allocated sum, or a zero Mat3 when either operand is invalid.
void *rt_mat3_add(void *a, void *b) {
    if (!a || !b)
        return rt_mat3_zero();

    mat3_impl *ma = mat3_checked(a, "Mat3.Add: invalid matrix");
    mat3_impl *mb = mat3_checked(b, "Mat3.Add: invalid matrix");
    if (!ma || !mb)
        return rt_mat3_zero();

    return rt_mat3_new(ma->m[0] + mb->m[0],
                       ma->m[1] + mb->m[1],
                       ma->m[2] + mb->m[2],
                       ma->m[3] + mb->m[3],
                       ma->m[4] + mb->m[4],
                       ma->m[5] + mb->m[5],
                       ma->m[6] + mb->m[6],
                       ma->m[7] + mb->m[7],
                       ma->m[8] + mb->m[8]);
}

/// @brief Subtract one matrix from another element by element.
/// @details NULL operands return a zero matrix. Non-NULL incompatible operands raise a runtime trap
///          before the same fallback is returned.
/// @param a Mat3 minuend.
/// @param b Mat3 subtrahend.
/// @return Newly allocated difference, or a zero Mat3 when either operand is invalid.
void *rt_mat3_sub(void *a, void *b) {
    if (!a || !b)
        return rt_mat3_zero();

    mat3_impl *ma = mat3_checked(a, "Mat3.Sub: invalid matrix");
    mat3_impl *mb = mat3_checked(b, "Mat3.Sub: invalid matrix");
    if (!ma || !mb)
        return rt_mat3_zero();

    return rt_mat3_new(ma->m[0] - mb->m[0],
                       ma->m[1] - mb->m[1],
                       ma->m[2] - mb->m[2],
                       ma->m[3] - mb->m[3],
                       ma->m[4] - mb->m[4],
                       ma->m[5] - mb->m[5],
                       ma->m[6] - mb->m[6],
                       ma->m[7] - mb->m[7],
                       ma->m[8] - mb->m[8]);
}

/// @brief Multiply two matrices using standard row-by-column multiplication.
/// @details For column-vector transforms, `mul(translate, rotate)` first rotates a point and then
///          translates it. NULL operands return an identity matrix; non-NULL incompatible operands
///          raise a runtime trap before the same fallback is returned.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @return Newly allocated product, or an identity Mat3 when either operand is invalid.
void *rt_mat3_mul(void *a, void *b) {
    if (!a || !b)
        return rt_mat3_identity();

    mat3_impl *ma = mat3_checked(a, "Mat3.Mul: invalid matrix");
    mat3_impl *mb = mat3_checked(b, "Mat3.Mul: invalid matrix");
    if (!ma || !mb)
        return rt_mat3_identity();

    double r[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i * 3 + j] = ma->m[i * 3 + 0] * mb->m[0 * 3 + j] +
                           ma->m[i * 3 + 1] * mb->m[1 * 3 + j] +
                           ma->m[i * 3 + 2] * mb->m[2 * 3 + j];
        }
    }

    return rt_mat3_new(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
}

/// @brief Multiply every matrix element by a scalar.
/// @details A NULL matrix returns a zero matrix. A non-NULL incompatible handle raises a runtime
///          trap before the same fallback is returned.
/// @param m Mat3 operand.
/// @param s Scalar multiplier.
/// @return Newly allocated scaled matrix, or a zero Mat3 for an invalid matrix.
void *rt_mat3_mul_scalar(void *m, double s) {
    if (!m)
        return rt_mat3_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.MulScalar: invalid matrix");
    if (!mat)
        return rt_mat3_zero();

    return rt_mat3_new(mat->m[0] * s,
                       mat->m[1] * s,
                       mat->m[2] * s,
                       mat->m[3] * s,
                       mat->m[4] * s,
                       mat->m[5] * s,
                       mat->m[6] * s,
                       mat->m[7] * s,
                       mat->m[8] * s);
}

/// @brief Transform a 2D point using the first two rows of a matrix.
/// @details Treats @p v as the homogeneous column vector `(x, y, 1)`, so the matrix translation
///          terms contribute to the result. The bottom matrix row is not evaluated. NULL inputs
///          return a zero vector; an incompatible matrix also raises a runtime trap.
/// @param m Mat3 transform.
/// @param v Vec2 point to transform.
/// @return Newly allocated transformed Vec2, or a zero Vec2 for invalid input.
void *rt_mat3_transform_point(void *m, void *v) {
    if (!m || !v)
        return rt_vec2_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.TransformPoint: invalid matrix");
    if (!mat)
        return rt_vec2_zero();
    double x = rt_vec2_x(v);
    double y = rt_vec2_y(v);

    // Transform as [x, y, 1]
    double rx = mat->m[0] * x + mat->m[1] * y + mat->m[2];
    double ry = mat->m[3] * x + mat->m[4] * y + mat->m[5];

    return rt_vec2_new(rx, ry);
}

/// @brief Transform a 2D direction using the linear portion of a matrix.
/// @details Treats @p v as the homogeneous column vector `(x, y, 0)`, ignoring translation and the
///          bottom matrix row. NULL inputs return a zero vector; an incompatible matrix also raises
///          a runtime trap.
/// @param m Mat3 transform.
/// @param v Vec2 direction to transform.
/// @return Newly allocated transformed Vec2, or a zero Vec2 for invalid input.
void *rt_mat3_transform_vec(void *m, void *v) {
    if (!m || !v)
        return rt_vec2_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.TransformVec: invalid matrix");
    if (!mat)
        return rt_vec2_zero();
    double x = rt_vec2_x(v);
    double y = rt_vec2_y(v);

    // Transform as [x, y, 0] (ignores translation)
    double rx = mat->m[0] * x + mat->m[1] * y;
    double ry = mat->m[3] * x + mat->m[4] * y;

    return rt_vec2_new(rx, ry);
}

//=============================================================================
// Matrix Operations
//=============================================================================

/// @brief Create the transpose of a matrix.
/// @details Exchanges rows and columns. A NULL matrix returns an identity matrix; a non-NULL
///          incompatible handle raises a runtime trap before the same fallback is returned.
/// @param m Mat3 operand.
/// @return Newly allocated transpose, or an identity Mat3 for an invalid matrix.
void *rt_mat3_transpose(void *m) {
    if (!m)
        return rt_mat3_identity();

    mat3_impl *mat = mat3_checked(m, "Mat3.Transpose: invalid matrix");
    if (!mat)
        return rt_mat3_identity();

    return rt_mat3_new(mat->m[0],
                       mat->m[3],
                       mat->m[6],
                       mat->m[1],
                       mat->m[4],
                       mat->m[7],
                       mat->m[2],
                       mat->m[5],
                       mat->m[8]);
}

/// @brief Compute a matrix determinant by cofactor expansion along its first row.
/// @details A NULL matrix returns zero. A non-NULL incompatible handle raises a runtime trap before
///          zero is returned.
/// @param m Mat3 operand.
/// @return Determinant of @p m, or 0.0 for an invalid matrix.
double rt_mat3_det(void *m) {
    mat3_impl *mat;
    if (!m)
        return 0.0;

    mat = mat3_checked(m, "Mat3.Det: invalid matrix");
    if (!mat)
        return 0.0;

    // Determinant using cofactor expansion along first row
    return mat->m[0] * (mat->m[4] * mat->m[8] - mat->m[5] * mat->m[7]) -
           mat->m[1] * (mat->m[3] * mat->m[8] - mat->m[5] * mat->m[6]) +
           mat->m[2] * (mat->m[3] * mat->m[7] - mat->m[4] * mat->m[6]);
}

/// @brief Invert a matrix using the adjugate and determinant.
/// @details A matrix is treated as singular when its determinant is non-finite or has magnitude
///          below `1e-15`. NULL, incompatible, and singular matrices raise a runtime trap and do
///          not produce a fallback matrix.
/// @param m Mat3 operand.
/// @return Newly allocated inverse, or NULL after trapping for invalid or singular input.
void *rt_mat3_inverse(void *m) {
    mat3_impl *mat;
    double det;
    if (!m) {
        rt_trap("Mat3.Inverse: null matrix");
        return NULL;
    }

    mat = mat3_checked(m, "Mat3.Inverse: invalid matrix");
    if (!mat)
        return NULL;
    det = rt_mat3_det(m);

    if (!isfinite(det) || fabs(det) < 1e-15) {
        rt_trap("Mat3.Inverse: singular matrix");
        return NULL;
    }

    double invDet = 1.0 / det;

    // Cofactor matrix (transposed)
    double c00 = mat->m[4] * mat->m[8] - mat->m[5] * mat->m[7];
    double c01 = mat->m[2] * mat->m[7] - mat->m[1] * mat->m[8];
    double c02 = mat->m[1] * mat->m[5] - mat->m[2] * mat->m[4];

    double c10 = mat->m[5] * mat->m[6] - mat->m[3] * mat->m[8];
    double c11 = mat->m[0] * mat->m[8] - mat->m[2] * mat->m[6];
    double c12 = mat->m[2] * mat->m[3] - mat->m[0] * mat->m[5];

    double c20 = mat->m[3] * mat->m[7] - mat->m[4] * mat->m[6];
    double c21 = mat->m[1] * mat->m[6] - mat->m[0] * mat->m[7];
    double c22 = mat->m[0] * mat->m[4] - mat->m[1] * mat->m[3];

    return rt_mat3_new(c00 * invDet,
                       c01 * invDet,
                       c02 * invDet,
                       c10 * invDet,
                       c11 * invDet,
                       c12 * invDet,
                       c20 * invDet,
                       c21 * invDet,
                       c22 * invDet);
}

/// @brief Negate every matrix element.
/// @details A NULL matrix returns a zero matrix. A non-NULL incompatible handle raises a runtime
///          trap before the same fallback is returned.
/// @param m Mat3 operand.
/// @return Newly allocated negated matrix, or a zero Mat3 for an invalid matrix.
void *rt_mat3_neg(void *m) {
    if (!m)
        return rt_mat3_zero();

    mat3_impl *mat = mat3_checked(m, "Mat3.Neg: invalid matrix");
    if (!mat)
        return rt_mat3_zero();

    return rt_mat3_new(-mat->m[0],
                       -mat->m[1],
                       -mat->m[2],
                       -mat->m[3],
                       -mat->m[4],
                       -mat->m[5],
                       -mat->m[6],
                       -mat->m[7],
                       -mat->m[8]);
}

//=============================================================================
// Comparison
//=============================================================================

/// @brief Compare two matrices using an absolute per-element tolerance.
/// @details Two NULL handles compare equal, while exactly one NULL handle compares unequal. A
///          non-positive or non-finite tolerance is replaced with `1e-9`. Any NaN element causes
///          the matrices to compare unequal. Incompatible non-NULL handles raise a runtime trap.
/// @param a Left-hand Mat3 operand.
/// @param b Right-hand Mat3 operand.
/// @param epsilon Maximum permitted absolute difference for each corresponding element.
/// @return 1 when all nine elements compare within tolerance, otherwise 0.
int8_t rt_mat3_eq(void *a, void *b, double epsilon) {
    if (!a || !b)
        return (!a && !b) ? 1 : 0;

    // A non-finite epsilon (NaN/Inf) is not a usable tolerance: NaN slips past
    // `epsilon <= 0.0` and then `diff > NaN` is always false, making any two
    // matrices compare equal (VDOC-207). Normalize non-positive AND non-finite
    // epsilon to the default.
    if (!(epsilon > 0.0) || !isfinite(epsilon))
        epsilon = 1e-9;

    mat3_impl *ma = mat3_checked(a, "Mat3.Eq: invalid matrix");
    mat3_impl *mb = mat3_checked(b, "Mat3.Eq: invalid matrix");
    if (!ma || !mb)
        return (!a && !b) ? 1 : 0;

    for (int i = 0; i < 9; i++) {
        // Use the negated `<=` so a NaN component difference (NaN <= eps is
        // false) counts as NOT equal — a matrix containing NaN is never equal
        // to any matrix, matching IEEE semantics (VDOC-207).
        if (!(fabs(ma->m[i] - mb->m[i]) <= epsilon))
            return 0;
    }

    return 1;
}
