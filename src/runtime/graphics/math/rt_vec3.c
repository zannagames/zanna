//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_vec3.c
// Purpose: 3D vector mathematics (x, y, z doubles) for Zanna graphics and
//   simulation. Provides pure Vec3 arithmetic (+,-,×,÷), dot product, cross
//   product, length/normalize, distance, linear interpolation, reflection, angle
//   operations, and explicit in-place mutators. Used for 3D positions, surface
//   normals, lighting directions, and RGB color triples (r=x, g=y, b=z).
//
// Key invariants:
//   - Vec3 stores three doubles (x, y, z); 24 bytes, no padding.
//   - Coordinate system: right-handed Cartesian (OpenGL convention):
//       +X = right,  +Y = up,  +Z = toward the viewer (out of screen).
//   - Cross product: v × w gives a vector perpendicular to both, following the
//     right-hand rule: curl fingers from v to w, thumb points in result direction.
//   - Normalize returns a unit vector (length 1). Normalizing a zero vector
//     returns Vec3(0,0,0) — no trap or NaN.
//   - Pure arithmetic operations return new Vec3 objects.
//   - Set*/CopyFrom are the only in-place mutators and update the receiver directly.
//   - Vec3 uses a thread-local LIFO free-list pool (VEC3_POOL_CAPACITY = 32)
//     identical in design to the Vec2 pool, to amortize GC pressure in
//     lighting and physics inner loops.
//
// Ownership/Lifetime:
//   - Vec3 objects are GC-managed. Pool slots are reclaimed by the pool's
//     finalizer path; non-pooled Vec3s are collected by the standard GC.
//     Callers must not free Vec3s manually.
//
// Links: src/runtime/graphics/math/rt_vec3.h (public API),
//        src/runtime/graphics/math/rt_vec2.c (2D counterpart),
//        src/runtime/graphics/math/rt_mat4.c (matrix-vector transform consumer)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements managed Vec3 allocation and three-dimensional vector math.
 * @details Provides the public Vec3 runtime operations together with strict
 * handle validation, overflow-resistant length calculations, and a
 * thread-local resurrection pool used to reduce allocation churn in graphics
 * and simulation workloads.
 */

#include "rt_vec3.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"

#include <math.h>

// ============================================================================
// Thread-local free-list pool (P2-3.6)
// ============================================================================
/** Maximum number of finalized Vec3 payloads retained by each thread. */
#define VEC3_POOL_CAPACITY 32

/** Per-thread LIFO storage for reusable, resurrected Vec3 payloads. */
static _Thread_local void *vec3_pool_buf_[VEC3_POOL_CAPACITY];
/** Number of reusable payloads currently stored in @ref vec3_pool_buf_. */
static _Thread_local int vec3_pool_top_ = 0;

/// @brief GC finalizer that returns a Vec3 object to the thread-local free pool.
/// @details When the pool is at capacity the object is dropped (GC reclaims it);
///          otherwise `rt_obj_resurrect` prevents GC collection and re-registers
///          this function as the next finalizer so the object re-enters the pool
///          on the next release.
/// @param p Vec3 payload whose finalizer is running.
static void vec3_pool_return(void *p) {
    if (vec3_pool_top_ < VEC3_POOL_CAPACITY) {
        rt_obj_resurrect(p);
        rt_obj_set_finalizer(p, vec3_pool_return);
        vec3_pool_buf_[vec3_pool_top_++] = p;
    }
}

/// @brief Internal Vec3 implementation structure.
///
/// Stores the X, Y, and Z components of a 3D vector as double-precision
/// floating-point values. The structure is allocated as a Zanna object
/// with reference counting support.
///
typedef struct {
    double x; ///< X component (horizontal axis, positive = right)
    double y; ///< Y component (vertical axis, positive = up)
    double z; ///< Z component (depth axis, positive = toward viewer in RH coords)
} ZannaVec3;

/// @brief Return whether @p v is a Vec3-compatible heap payload.
/// @details Accepts both the explicit Vec3 class id used by current constructors and the
///          historical class-id-zero value object layout consumed throughout Graphics3D. Legacy
///          classless payloads must be exactly three doubles.
/// @param v Candidate runtime object payload.
/// @return 1 for a compatible Vec3 payload, otherwise 0.
static int vec3_is_compatible_object(void *v) {
    if (!v)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(v, &heap_info))
        return 0;
    if (heap_info.kind != RT_HEAP_OBJECT || heap_info.elem_kind != RT_ELEM_NONE)
        return 0;
    if (heap_info.class_id == RT_VEC3_CLASS_ID)
        return heap_info.cap >= sizeof(ZannaVec3);
    return heap_info.class_id == 0 && heap_info.len == sizeof(ZannaVec3) &&
           heap_info.cap == sizeof(ZannaVec3);
}

/// @brief Validate and cast an opaque handle to a Vec3 payload.
/// @details Rejects NULL, non-object heap payloads, incompatible class identifiers, and
///   undersized allocations before any Vec3 component is read.
/// @param v Candidate Vec3 runtime handle.
/// @param op Diagnostic prefix used if validation fails.
/// @return Typed Vec3 payload, or NULL after trapping.
static ZannaVec3 *vec3_checked(void *v, const char *op) {
    if (!vec3_is_compatible_object(v)) {
        rt_trap(op ? op : "Vec3: invalid vector");
        return NULL;
    }
    return (ZannaVec3 *)v;
}

/// @brief Compute a finite, overflow-resistant Vec3 length from raw components.
/// @details Uses chained `hypot` calls instead of `sqrt(x*x + y*y + z*z)`,
///          preventing overflow/underflow during normalization, distance, and
///          angle calculations. Non-finite input returns `INFINITY`.
/// @param x Vector x component.
/// @param y Vector y component.
/// @param z Vector z component.
/// @return Euclidean length, or positive infinity when any component is non-finite.
static double vec3_safe_len(double x, double y, double z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;
    return hypot(hypot(x, y), z);
}

/// @brief Allocate and initialize a new Vec3 with the given components.
///
/// This internal helper allocates a Vec3 object through the Zanna object
/// system and initializes it with the provided X, Y, and Z values.
///
/// @param x The X component value.
/// @param y The Y component value.
/// @param z The Z component value.
///
/// @return Pointer to the newly allocated Vec3. Traps on allocation failure.
///
/// @note This is an internal function - use rt_vec3_new() for public API.
static ZannaVec3 *vec3_alloc(double x, double y, double z) {
    ZannaVec3 *v;
    if (vec3_pool_top_ > 0) {
        v = (ZannaVec3 *)vec3_pool_buf_[--vec3_pool_top_];
    } else {
        v = (ZannaVec3 *)rt_obj_new_i64(RT_VEC3_CLASS_ID, (int64_t)sizeof(ZannaVec3));
        if (!v) {
            rt_trap("Vec3: memory allocation failed");
            return NULL; // Unreachable after trap
        }
        rt_obj_set_finalizer(v, vec3_pool_return);
    }
    v->x = x;
    v->y = y;
    v->z = z;
    return v;
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Creates a new 3D vector with the specified X, Y, and Z components.
///
/// This is the primary constructor for creating Vec3 instances with custom
/// component values.
///
/// **Usage example:**
/// ```
/// Dim position = Vec3.New(100.0, 50.0, 25.0)  ' 3D position
/// Dim velocity = Vec3.New(5.0, -2.0, 1.0)    ' 3D velocity
/// Dim normal = Vec3.New(0.0, 1.0, 0.0)       ' Up direction
/// Dim color = Vec3.New(1.0, 0.5, 0.0)        ' Orange as RGB
/// ```
///
/// @param x The X component (horizontal position/direction).
/// @param y The Y component (vertical position/direction).
/// @param z The Z component (depth position/direction).
///
/// @return A new Vec3 object with the specified components.
///
/// @note O(1) time complexity.
/// @note The returned Vec3 is reference-counted and garbage collected.
///
/// @see rt_vec3_zero For creating a zero vector
/// @see rt_vec3_one For creating a unit vector (1, 1, 1)
void *rt_vec3_new(double x, double y, double z) {
    return vec3_alloc(x, y, z);
}

/// @brief Creates a zero vector (0, 0, 0).
///
/// Returns a Vec3 with all components set to zero. The zero vector is the
/// identity element for vector addition and represents "no direction" or
/// "origin point."
///
/// **Mathematical Properties:**
/// - v + Vec3.Zero() = v (additive identity)
/// - v * 0 = Vec3.Zero()
/// - Length of zero vector = 0
///
/// **Usage example:**
/// ```
/// Dim origin = Vec3.Zero()       ' World origin (0, 0, 0)
/// Dim velocity = Vec3.Zero()     ' Not moving
/// Dim acceleration = Vec3.Zero() ' No acceleration
/// ```
///
/// @return A new Vec3 object with components (0, 0, 0).
///
/// @note O(1) time complexity.
///
/// @see rt_vec3_one For a unit vector
/// @see rt_vec3_len For checking if a vector is zero-length
void *rt_vec3_zero(void) {
    return vec3_alloc(0.0, 0.0, 0.0);
}

/// @brief Creates a unit vector (1, 1, 1).
///
/// Returns a Vec3 with all components set to one. Note that this vector
/// has a length of sqrt(3) ≈ 1.732, not 1. For true unit vectors, use
/// cardinal directions or normalize any non-zero vector.
///
/// **Usage example:**
/// ```
/// Dim scale = Vec3.One()              ' (1, 1, 1)
/// Dim doubled = scale.Mul(2)          ' (2, 2, 2)
///
/// ' Uniform scaling
/// Dim objectScale = Vec3.One().Mul(0.5)  ' Half size in all dimensions
/// ```
///
/// **Note on Length:**
/// ```
/// Vec3.One() length = sqrt(1² + 1² + 1²) = sqrt(3) ≈ 1.732
/// ```
///
/// @return A new Vec3 object with components (1, 1, 1).
///
/// @note O(1) time complexity.
/// @note Length is sqrt(3), not 1. Use rt_vec3_norm() for true unit vectors.
///
/// @see rt_vec3_zero For a zero vector
/// @see rt_vec3_norm For creating unit vectors
void *rt_vec3_one(void) {
    return vec3_alloc(1.0, 1.0, 1.0);
}

//=============================================================================
// Property Accessors
//=============================================================================

/// @brief Gets the X component of the vector.
///
/// Returns the horizontal component of the 3D vector. In a standard
/// right-handed coordinate system, positive X points to the right.
///
/// **Usage example:**
/// ```
/// Dim pos = Vec3.New(100.0, 50.0, 25.0)
/// Print pos.X    ' Outputs: 100
/// ```
///
/// @param v Pointer to a Vec3 object. Must not be NULL.
///
/// @return The X component value as a double.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.X: null vector" if v is NULL.
///
/// @see rt_vec3_y For the Y component
/// @see rt_vec3_z For the Z component
double rt_vec3_x(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.X: invalid vector");
    if (!vec)
        return 0.0;
    return vec->x;
}

/// @brief Gets the Y component of the vector.
///
/// Returns the vertical component of the 3D vector. In a standard
/// coordinate system, positive Y typically points upward.
///
/// **Usage example:**
/// ```
/// Dim pos = Vec3.New(100.0, 50.0, 25.0)
/// Print pos.Y    ' Outputs: 50
///
/// ' Check if above ground
/// If pos.Y > 0 Then
///     Print "Above ground level"
/// End If
/// ```
///
/// @param v Pointer to a Vec3 object. Must not be NULL.
///
/// @return The Y component value as a double.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.Y: null vector" if v is NULL.
///
/// @see rt_vec3_x For the X component
/// @see rt_vec3_z For the Z component
double rt_vec3_y(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Y: invalid vector");
    if (!vec)
        return 0.0;
    return vec->y;
}

/// @brief Gets the Z component of the vector.
///
/// Returns the depth component of the 3D vector. In a right-handed
/// coordinate system, positive Z points toward the viewer.
///
/// **Usage example:**
/// ```
/// Dim pos = Vec3.New(100.0, 50.0, 25.0)
/// Print pos.Z    ' Outputs: 25
///
/// ' Check depth for rendering order
/// If objectA.Z > objectB.Z Then
///     Print "Object A is in front"
/// End If
/// ```
///
/// @param v Pointer to a Vec3 object. Must not be NULL.
///
/// @return The Z component value as a double.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.Z: null vector" if v is NULL.
///
/// @see rt_vec3_x For the X component
/// @see rt_vec3_y For the Y component
double rt_vec3_z(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Z: invalid vector");
    if (!vec)
        return 0.0;
    return vec->z;
}

/// @brief Set the X component in place.
/// @details Raises a runtime trap and leaves memory untouched if @p v is not a compatible Vec3.
/// @param v Mutable Vec3 handle.
/// @param x New x component.
void rt_vec3_set_x(void *v, double x) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.set_X: invalid vector");
    if (!vec)
        return;
    vec->x = x;
}

/// @brief Set the Y component in place.
/// @details Raises a runtime trap and leaves memory untouched if @p v is not a compatible Vec3.
/// @param v Mutable Vec3 handle.
/// @param y New y component.
void rt_vec3_set_y(void *v, double y) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.set_Y: invalid vector");
    if (!vec)
        return;
    vec->y = y;
}

/// @brief Set the Z component in place.
/// @details Raises a runtime trap and leaves memory untouched if @p v is not a compatible Vec3.
/// @param v Mutable Vec3 handle.
/// @param z New z component.
void rt_vec3_set_z(void *v, double z) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.set_Z: invalid vector");
    if (!vec)
        return;
    vec->z = z;
}

/// @brief Set all components in place.
/// @details Raises a runtime trap and leaves memory untouched if @p v is not a compatible Vec3.
/// @param v Mutable Vec3 handle.
/// @param x New x component.
/// @param y New y component.
/// @param z New z component.
void rt_vec3_set(void *v, double x, double y, double z) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Set: invalid vector");
    if (!vec)
        return;
    vec->x = x;
    vec->y = y;
    vec->z = z;
}

/// @brief Copy all components from @p other into @p v.
/// @details Validates both handles before copying. An invalid handle raises a runtime trap and no
///          components are assigned.
/// @param v Mutable destination Vec3.
/// @param other Source Vec3.
void rt_vec3_copy_from(void *v, void *other) {
    ZannaVec3 *dst = vec3_checked(v, "Vec3.CopyFrom: invalid vector");
    ZannaVec3 *src = vec3_checked(other, "Vec3.CopyFrom: invalid vector");
    if (!dst || !src)
        return;
    dst->x = src->x;
    dst->y = src->y;
    dst->z = src->z;
}

//=============================================================================
// Arithmetic Operations
//=============================================================================

/// @brief Adds two vectors component-wise.
///
/// Performs vector addition: result = (a.x + b.x, a.y + b.y, a.z + b.z)
///
/// **Usage example:**
/// ```
/// Dim pos = Vec3.New(100.0, 50.0, 25.0)
/// Dim velocity = Vec3.New(5.0, -2.0, 1.0)
/// Dim newPos = pos.Add(velocity)   ' (105, 48, 26)
///
/// ' Combine forces
/// Dim totalForce = gravity.Add(wind).Add(thrust)
/// ```
///
/// @param a First vector operand. Must not be NULL.
/// @param b Second vector operand. Must not be NULL.
///
/// @return A new Vec3 containing the component-wise sum.
///
/// @note O(1) time complexity.
/// @note Vector addition is commutative: a + b = b + a
/// @note Traps with "Vec3.Add: null vector" if either operand is NULL.
///
/// @see rt_vec3_sub For vector subtraction
void *rt_vec3_add(void *a, void *b) {
    ZannaVec3 *va = vec3_checked(a, "Vec3.Add: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Add: invalid vector");
    if (!va || !vb)
        return NULL;
    return vec3_alloc(va->x + vb->x, va->y + vb->y, va->z + vb->z);
}

/// @brief Subtracts vector b from vector a component-wise.
///
/// Performs vector subtraction: result = (a.x - b.x, a.y - b.y, a.z - b.z)
///
/// Subtraction finds the vector from b to a.
///
/// **Usage example:**
/// ```
/// Dim target = Vec3.New(200.0, 150.0, 100.0)
/// Dim position = Vec3.New(100.0, 100.0, 50.0)
/// Dim direction = target.Sub(position)  ' Vector from position to target
///
/// ' Calculate relative velocity
/// Dim relativeVel = shipVelocity.Sub(asteroidVelocity)
/// ```
///
/// @param a Vector to subtract from (minuend). Must not be NULL.
/// @param b Vector to subtract (subtrahend). Must not be NULL.
///
/// @return A new Vec3 containing the component-wise difference (a - b).
///
/// @note O(1) time complexity.
/// @note Not commutative: a - b ≠ b - a
/// @note Traps with "Vec3.Sub: null vector" if either operand is NULL.
///
/// @see rt_vec3_add For vector addition
void *rt_vec3_sub(void *a, void *b) {
    ZannaVec3 *va = vec3_checked(a, "Vec3.Sub: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Sub: invalid vector");
    if (!va || !vb)
        return NULL;
    return vec3_alloc(va->x - vb->x, va->y - vb->y, va->z - vb->z);
}

/// @brief Multiplies a vector by a scalar value.
///
/// Scales all components of the vector by the given scalar:
/// result = (v.x * s, v.y * s, v.z * s)
///
/// **Effect of scalar values:**
/// - s > 1: Lengthens the vector
/// - 0 < s < 1: Shortens the vector
/// - s = 0: Returns zero vector
/// - s < 0: Reverses direction and scales
///
/// **Usage example:**
/// ```
/// Dim direction = Vec3.New(1.0, 0.0, 0.0)
/// Dim speed = 5.0
/// Dim velocity = direction.Mul(speed)  ' (5, 0, 0)
/// ```
///
/// @param v Vector to scale. Must not be NULL.
/// @param s Scalar multiplier.
///
/// @return A new Vec3 with components scaled by s.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.Mul: null vector" if v is NULL.
///
/// @see rt_vec3_div For scalar division
void *rt_vec3_mul(void *v, double s) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Mul: invalid vector");
    if (!vec)
        return NULL;
    return vec3_alloc(vec->x * s, vec->y * s, vec->z * s);
}

/// @brief Divides a vector by a scalar value.
///
/// Divides all components of the vector by the given scalar:
/// result = (v.x / s, v.y / s, v.z / s)
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec3.New(10.0, 6.0, 4.0)
/// Dim halfSpeed = velocity.Div(2.0)     ' (5, 3, 2)
///
/// ' Normalize manually
/// Dim direction = velocity.Div(velocity.Len())
/// ```
///
/// @param v Vector to divide. Must not be NULL.
/// @param s Scalar divisor. Must be finite and nonzero.
///
/// @return A new Vec3 with components divided by s.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.Div: null vector" if v is NULL.
/// @note Traps with "Vec3.Div: invalid divisor" if s is zero or non-finite.
///
/// @see rt_vec3_mul For scalar multiplication
/// @see rt_vec3_norm For normalizing to unit length
void *rt_vec3_div(void *v, double s) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Div: invalid vector");
    if (!vec)
        return NULL;
    if (!isfinite(s) || s == 0.0) {
        rt_trap("Vec3.Div: invalid divisor");
        return NULL;
    }
    return vec3_alloc(vec->x / s, vec->y / s, vec->z / s);
}

/// @brief Negates a vector (reverses its direction).
///
/// Returns a vector pointing in the opposite direction with the same magnitude:
/// result = (-v.x, -v.y, -v.z)
///
/// **Usage example:**
/// ```
/// Dim forward = Vec3.New(0.0, 0.0, 1.0)
/// Dim backward = forward.Neg()          ' (0, 0, -1)
///
/// ' Reflect velocity on collision
/// velocity = velocity.Neg()
/// ```
///
/// @param v Vector to negate. Must not be NULL.
///
/// @return A new Vec3 pointing in the opposite direction.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec3.Neg: null vector" if v is NULL.
/// @note Equivalent to v.Mul(-1)
///
/// @see rt_vec3_mul For scalar multiplication
void *rt_vec3_neg(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Neg: invalid vector");
    if (!vec)
        return NULL;
    return vec3_alloc(-vec->x, -vec->y, -vec->z);
}

//=============================================================================
// Vector Products
//=============================================================================

/// @brief Computes the dot product (scalar product) of two vectors.
///
/// The dot product is a fundamental operation that returns a scalar value:
/// a · b = a.x * b.x + a.y * b.y + a.z * b.z = |a| * |b| * cos(θ)
///
/// where θ is the angle between the vectors.
///
/// **Common Uses:**
/// - Check if vectors are perpendicular: dot == 0
/// - Check if vectors point in same direction: dot > 0
/// - Calculate lighting intensity (N · L for diffuse lighting)
/// - Project one vector onto another
///
/// **Usage example:**
/// ```
/// Dim normal = Vec3.New(0.0, 1.0, 0.0)    ' Surface normal (up)
/// Dim lightDir = Vec3.New(0.5, 0.5, 0.0).Norm()
/// Dim intensity = normal.Dot(lightDir)    ' Diffuse lighting
///
/// ' Check if in front of camera
/// Dim toObject = objectPos.Sub(cameraPos).Norm()
/// If cameraForward.Dot(toObject) > 0 Then
///     Print "Object is in front of camera"
/// End If
/// ```
///
/// @param a First vector. Must not be NULL.
/// @param b Second vector. Must not be NULL.
///
/// @return The dot product as a scalar value.
///
/// @note O(1) time complexity.
/// @note Dot product is commutative: a · b = b · a
/// @note Traps with "Vec3.Dot: null vector" if either operand is NULL.
///
/// @see rt_vec3_cross For the cross product
double rt_vec3_dot(void *a, void *b) {
    ZannaVec3 *va = vec3_checked(a, "Vec3.Dot: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Dot: invalid vector");
    if (!va || !vb)
        return 0.0;
    return va->x * vb->x + va->y * vb->y + va->z * vb->z;
}

/// @brief Computes the cross product of two vectors.
///
/// The 3D cross product returns a vector perpendicular to both input vectors:
/// a × b = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx)
///
/// **Properties:**
/// - Result is perpendicular to both a and b
/// - |a × b| = |a| * |b| * sin(θ)
/// - Direction follows right-hand rule
/// - Anti-commutative: a × b = -(b × a)
///
/// **Right-Hand Rule:**
/// ```
/// Point fingers in direction of a
/// Curl fingers toward b
/// Thumb points in direction of a × b
///
///        a × b (result)
///          ↑
///          |
///          |  b
///          | /
///          |/_____ a
/// ```
///
/// **Common Uses:**
/// - Calculate surface normals: normal = (v1 - v0) × (v2 - v0)
/// - Calculate torque: τ = r × F
/// - Find perpendicular vectors
/// - Determine winding order of triangles
///
/// **Usage example:**
/// ```
/// ' Calculate surface normal from triangle vertices
/// Dim edge1 = v1.Sub(v0)
/// Dim edge2 = v2.Sub(v0)
/// Dim normal = edge1.Cross(edge2).Norm()
///
/// ' Calculate torque
/// Dim torque = leverArm.Cross(force)
///
/// ' Create coordinate frame
/// Dim right = Vec3.New(1, 0, 0)
/// Dim up = Vec3.New(0, 1, 0)
/// Dim forward = right.Cross(up)  ' (0, 0, 1) in this RH convention
/// ```
///
/// @param a First vector. Must not be NULL.
/// @param b Second vector. Must not be NULL.
///
/// @return A new Vec3 perpendicular to both inputs.
///
/// @note O(1) time complexity.
/// @note Cross product is anti-commutative: a × b = -(b × a)
/// @note Traps with "Vec3.Cross: null vector" if either operand is NULL.
///
/// @see rt_vec3_dot For the dot product
/// @see rt_vec2_cross For the 2D cross product (returns scalar)
void *rt_vec3_cross(void *a, void *b) {
    // 3D cross product: a × b = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx)
    ZannaVec3 *va = vec3_checked(a, "Vec3.Cross: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Cross: invalid vector");
    if (!va || !vb)
        return NULL;
    double x = va->y * vb->z - va->z * vb->y;
    double y = va->z * vb->x - va->x * vb->z;
    double z = va->x * vb->y - va->y * vb->x;
    return vec3_alloc(x, y, z);
}

//=============================================================================
// Length and Distance
//=============================================================================

/// @brief Computes the squared length (magnitude squared) of the vector.
///
/// Returns |v|² = v.x² + v.y² + v.z²
///
/// The squared length avoids the expensive square root operation, making it
/// ideal for comparisons where the actual length isn't needed.
///
/// **Performance Optimization:**
/// ```
/// ' Instead of: If a.Len() < b.Len() Then ...
/// If a.LenSq() < b.LenSq() Then ...   ' Faster!
///
/// ' Instead of: If v.Len() < 10 Then ...
/// If v.LenSq() < 100 Then ...          ' 100 = 10²
/// ```
///
/// **Usage example:**
/// ```
/// Dim v = Vec3.New(1.0, 2.0, 2.0)
/// Print v.LenSq()    ' 9 (= 1² + 2² + 2²)
/// Print v.Len()      ' 3 (= sqrt(9))
/// ```
///
/// @param v Vector to measure. Must not be NULL.
///
/// @return The squared length as a non-negative value.
///
/// @note O(1) time complexity.
/// @note Prefer LenSq over Len when only comparing magnitudes.
/// @note Traps with "Vec3.LenSq: null vector" if v is NULL.
///
/// @see rt_vec3_len For the actual length
double rt_vec3_len_sq(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.LenSq: invalid vector");
    if (!vec)
        return 0.0;
    return vec->x * vec->x + vec->y * vec->y + vec->z * vec->z;
}

/// @brief Computes the length (magnitude) of the vector.
///
/// Returns the Euclidean length: |v| = sqrt(v.x² + v.y² + v.z²)
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec3.New(1.0, 2.0, 2.0)
/// Dim speed = velocity.Len()    ' 3.0
///
/// ' Get direction (unit vector)
/// Dim direction = velocity.Div(speed)
/// ```
///
/// @param v Vector to measure. Must not be NULL.
///
/// @return The length as a non-negative value.
///
/// @note O(1) time complexity (involves sqrt).
/// @note For comparisons, prefer rt_vec3_len_sq to avoid sqrt.
/// @note Traps if v is not a compatible Vec3 handle.
///
/// @see rt_vec3_len_sq For squared length (faster for comparisons)
/// @see rt_vec3_norm For getting a unit-length vector
double rt_vec3_len(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Len: invalid vector");
    if (!vec)
        return 0.0;
    return vec3_safe_len(vec->x, vec->y, vec->z);
}

/// @brief Computes the Euclidean distance between two points.
///
/// Returns the straight-line distance in 3D space:
/// dist = |b - a| = sqrt((b.x-a.x)² + (b.y-a.y)² + (b.z-a.z)²)
///
/// **Usage example:**
/// ```
/// Dim player = Vec3.New(100.0, 0.0, 100.0)
/// Dim enemy = Vec3.New(150.0, 10.0, 130.0)
/// Dim distance = player.Dist(enemy)
///
/// If distance < 50.0 Then
///     Print "Enemy is nearby!"
/// End If
/// ```
///
/// @param a First point (start). Must not be NULL.
/// @param b Second point (end). Must not be NULL.
///
/// @return The distance as a non-negative value.
///
/// @note O(1) time complexity.
/// @note Distance is symmetric: a.Dist(b) = b.Dist(a)
/// @note Traps with "Vec3.Dist: null vector" if either point is NULL.
///
/// @see rt_vec3_len For the length of a single vector
double rt_vec3_dist(void *a, void *b) {
    ZannaVec3 *va = vec3_checked(a, "Vec3.Dist: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Dist: invalid vector");
    if (!va || !vb)
        return 0.0;
    double dx = vb->x - va->x;
    double dy = vb->y - va->y;
    double dz = vb->z - va->z;
    return vec3_safe_len(dx, dy, dz);
}

//=============================================================================
// Normalization and Interpolation
//=============================================================================

/// @brief Normalizes the vector to unit length (length = 1).
///
/// Returns a vector pointing in the same direction with length 1:
/// result = v / |v|
///
/// Unit vectors are essential for representing pure direction without magnitude.
/// They are used extensively in graphics for normals, directions, and lighting.
///
/// **Special Case:**
/// If the input vector has zero or non-finite length, returns a zero vector
/// (0, 0, 0) rather than dividing by an unusable magnitude.
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec3.New(3.0, 4.0, 0.0)
/// Dim direction = velocity.Norm()   ' (0.6, 0.8, 0) - length is 1.0
///
/// ' Use unit vector to move at constant speed
/// Dim speed = 10.0
/// Dim movement = direction.Mul(speed)
///
/// ' Surface normal
/// Dim normal = surfaceDirection.Norm()
/// ```
///
/// @param v Vector to normalize. Must not be NULL.
///
/// @return A new unit vector, or zero vector if input length is zero or non-finite.
///
/// @note O(1) time complexity.
/// @note Safe for zero-length and non-finite vectors (returns zero vector).
/// @note Traps with "Vec3.Norm: null vector" if v is NULL.
///
/// @see rt_vec3_len For getting the length
/// @see rt_vec3_div For manual normalization
void *rt_vec3_norm(void *v) {
    ZannaVec3 *vec = vec3_checked(v, "Vec3.Norm: invalid vector");
    if (!vec)
        return NULL;
    double len = vec3_safe_len(vec->x, vec->y, vec->z);
    if (len == 0.0 || !isfinite(len)) {
        // Return zero vector for zero-length input
        return vec3_alloc(0.0, 0.0, 0.0);
    }
    return vec3_alloc(vec->x / len, vec->y / len, vec->z / len);
}

/// @brief Linearly interpolates between two vectors.
///
/// Returns a point along the line from a to b based on parameter t:
/// result = a + (b - a) * t = a * (1 - t) + b * t
///
/// **Interpolation values:**
/// - t = 0: Returns a
/// - t = 0.5: Returns midpoint between a and b
/// - t = 1: Returns b
/// - t < 0 or t > 1: Extrapolates beyond a and b
///
/// **Usage example:**
/// ```
/// Dim start = Vec3.New(0.0, 0.0, 0.0)
/// Dim target = Vec3.New(100.0, 50.0, 25.0)
///
/// ' Animate position over time
/// Dim progress = elapsedTime / totalTime   ' 0.0 to 1.0
/// Dim currentPos = start.Lerp(target, progress)
///
/// ' Smooth camera follow
/// camera = camera.Lerp(targetPos, 0.1)
///
/// ' Blend between two colors (using Vec3 as RGB)
/// Dim red = Vec3.New(1, 0, 0)
/// Dim blue = Vec3.New(0, 0, 1)
/// Dim purple = red.Lerp(blue, 0.5)  ' (0.5, 0, 0.5)
/// ```
///
/// @param a Starting vector (returned when t = 0). Must not be NULL.
/// @param b Ending vector (returned when t = 1). Must not be NULL.
/// @param t Interpolation parameter (typically 0.0 to 1.0).
///
/// @return A new Vec3 representing the interpolated position.
///
/// @note O(1) time complexity.
/// @note Values of t outside [0, 1] will extrapolate beyond a and b.
/// @note Traps with "Vec3.Lerp: null vector" if either vector is NULL.
///
/// @see rt_vec3_add For vector addition
void *rt_vec3_lerp(void *a, void *b, double t) {
    ZannaVec3 *va = vec3_checked(a, "Vec3.Lerp: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Lerp: invalid vector");
    if (!va || !vb)
        return NULL;
    // lerp(a, b, t) = a + (b - a) * t = a * (1 - t) + b * t
    double x = va->x + (vb->x - va->x) * t;
    double y = va->y + (vb->y - va->y) * t;
    double z = va->z + (vb->z - va->z) * t;
    return vec3_alloc(x, y, z);
}

/// @brief Reflect `v` across a surface with the given `normal`.
/// @details The normal is normalized internally so callers do not need to provide a unit vector.
///          Non-finite components or a degenerate normal return the zero vector instead of
///          propagating NaN into later physics or lighting calculations. NULL handles also return
///          zero; incompatible non-NULL handles trap before the same fallback.
/// @param v Vec3 incident vector.
/// @param normal Vec3 surface normal; it need not be normalized.
/// @return New reflected Vec3, or a zero Vec3 for invalid or degenerate input.
void *rt_vec3_reflect(void *v, void *normal) {
    if (!v || !normal)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *vv = vec3_checked(v, "Vec3.Reflect: invalid vector");
    ZannaVec3 *n = vec3_checked(normal, "Vec3.Reflect: invalid vector");
    if (!vv || !n)
        return vec3_alloc(0, 0, 0);
    double n_len = vec3_safe_len(n->x, n->y, n->z);
    if (n_len < 1e-12 || !isfinite(n_len) || !isfinite(vv->x) || !isfinite(vv->y) ||
        !isfinite(vv->z))
        return vec3_alloc(0, 0, 0);

    double nx = n->x / n_len;
    double ny = n->y / n_len;
    double nz = n->z / n_len;
    double d = 2.0 * (vv->x * nx + vv->y * ny + vv->z * nz);
    if (!isfinite(d))
        return vec3_alloc(0, 0, 0);
    return vec3_alloc(vv->x - d * nx, vv->y - d * ny, vv->z - d * nz);
}

/// @brief Project `v` onto the line spanned by `onto`.
/// @details Returns `((v·onto)/(onto·onto)) * onto`. The denominator is computed from an
///          overflow-resistant length, and any non-finite operand or degenerate target returns
///          the zero vector. NULL handles also return zero; incompatible non-NULL handles trap
///          before the same fallback.
/// @param v Vec3 to project.
/// @param onto Vec3 defining the target line.
/// @return New projected Vec3, or a zero Vec3 for invalid or degenerate input.
void *rt_vec3_project(void *v, void *onto) {
    if (!v || !onto)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *vv = vec3_checked(v, "Vec3.Project: invalid vector");
    ZannaVec3 *t = vec3_checked(onto, "Vec3.Project: invalid vector");
    if (!vv || !t)
        return vec3_alloc(0, 0, 0);
    if (!isfinite(vv->x) || !isfinite(vv->y) || !isfinite(vv->z))
        return vec3_alloc(0, 0, 0);
    double t_len = vec3_safe_len(t->x, t->y, t->z);
    if (t_len < 1e-12 || !isfinite(t_len))
        return vec3_alloc(0, 0, 0);

    double dot_vt = vv->x * t->x + vv->y * t->y + vv->z * t->z;
    if (!isfinite(dot_vt))
        return vec3_alloc(0, 0, 0);
    double dot_tt = t_len * t_len;
    double s = dot_vt / dot_tt;
    return vec3_alloc(s * t->x, s * t->y, s * t->z);
}

/// @brief Clamp a vector's length to a maximum magnitude.
/// @details Returns a value copy when the input is already short enough and scales longer vectors
///          proportionally. NULL vectors, non-positive or non-finite limits, and non-finite vector
///          lengths produce zero. An incompatible non-NULL handle also traps.
/// @param v Vec3 to clamp.
/// @param max_len Maximum permitted non-negative magnitude.
/// @return New clamped Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_clamp_len(void *v, double max_len) {
    if (!v || !isfinite(max_len) || max_len <= 0.0)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *vv = vec3_checked(v, "Vec3.ClampLen: invalid vector");
    if (!vv)
        return vec3_alloc(0, 0, 0);
    double len = vec3_safe_len(vv->x, vv->y, vv->z);
    if (!isfinite(len))
        return vec3_alloc(0, 0, 0);
    if (len <= max_len)
        return vec3_alloc(vv->x, vv->y, vv->z);
    double s = max_len / len;
    return vec3_alloc(vv->x * s, vv->y * s, vv->z * s);
}

/// @brief Move a point toward a target by at most a specified distance.
/// @details Snaps exactly to @p target when it is within reach. A negative or non-finite
///          @p max_delta, or a non-finite separation, returns a copy of @p current. NULL handles
///          produce zero; incompatible non-NULL handles also trap.
/// @param current Vec3 starting point.
/// @param target Vec3 destination point.
/// @param max_delta Maximum distance to move.
/// @return New moved Vec3, a current-position copy for an unusable delta, or zero for invalid
///         vector handles.
void *rt_vec3_move_towards(void *current, void *target, double max_delta) {
    if (!current || !target)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *c = vec3_checked(current, "Vec3.MoveTowards: invalid vector");
    ZannaVec3 *t = vec3_checked(target, "Vec3.MoveTowards: invalid vector");
    if (!c || !t)
        return vec3_alloc(0, 0, 0);
    if (!isfinite(max_delta) || max_delta < 0.0)
        return vec3_alloc(c->x, c->y, c->z);
    double dx = t->x - c->x, dy = t->y - c->y, dz = t->z - c->z;
    double dist = vec3_safe_len(dx, dy, dz);
    if (!isfinite(dist))
        return vec3_alloc(c->x, c->y, c->z);
    if (dist <= max_delta || dist < 1e-12)
        return vec3_alloc(t->x, t->y, t->z);
    double s = max_delta / dist;
    return vec3_alloc(c->x + dx * s, c->y + dy * s, c->z + dz * s);
}

/// @brief Compute the unsigned angle in radians between vectors `a` and `b` via
/// `acos((a·b)/(|a||b|))`. Result is in [0, π]. Returns 0 for degenerate (zero-length) inputs;
/// the cosine is clamped to [-1, 1] for numerical safety against floating-point drift.
/// @param a First Vec3 direction.
/// @param b Second Vec3 direction.
/// @return Unsigned angle in [0, pi], or 0.0 for invalid, non-finite, or near-zero input.
double rt_vec3_angle(void *a, void *b) {
    if (!a || !b)
        return 0.0;
    ZannaVec3 *va = vec3_checked(a, "Vec3.Angle: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Angle: invalid vector");
    if (!va || !vb)
        return 0.0;
    double la = vec3_safe_len(va->x, va->y, va->z);
    double lb = vec3_safe_len(vb->x, vb->y, vb->z);
    if (la < 1e-12 || lb < 1e-12 || !isfinite(la) || !isfinite(lb))
        return 0.0;
    double ax = va->x / la;
    double ay = va->y / la;
    double az = va->z / la;
    double bx = vb->x / lb;
    double by = vb->y / lb;
    double bz = vb->z / lb;
    double cos_a = ax * bx + ay * by + az * bz;
    if (cos_a > 1.0)
        cos_a = 1.0;
    if (cos_a < -1.0)
        cos_a = -1.0;
    return acos(cos_a);
}

/// @brief Compute the component-wise minimum of two vectors.
/// @details Uses `fmin` independently on x, y, and z, which selects the numeric operand when only
///          one component is NaN. NULL handles return zero; incompatible non-NULL handles trap
///          before the same fallback.
/// @param a First Vec3 operand.
/// @param b Second Vec3 operand.
/// @return New component-wise minimum Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_min(void *a, void *b) {
    if (!a || !b)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *va = vec3_checked(a, "Vec3.Min: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Min: invalid vector");
    if (!va || !vb)
        return vec3_alloc(0, 0, 0);
    return vec3_alloc(fmin(va->x, vb->x), fmin(va->y, vb->y), fmin(va->z, vb->z));
}

/// @brief Compute the component-wise maximum of two vectors.
/// @details Uses `fmax` independently on x, y, and z, which selects the numeric operand when only
///          one component is NaN. NULL handles return zero; incompatible non-NULL handles trap
///          before the same fallback.
/// @param a First Vec3 operand.
/// @param b Second Vec3 operand.
/// @return New component-wise maximum Vec3, or a zero Vec3 for invalid input.
void *rt_vec3_max(void *a, void *b) {
    if (!a || !b)
        return vec3_alloc(0, 0, 0);
    ZannaVec3 *va = vec3_checked(a, "Vec3.Max: invalid vector");
    ZannaVec3 *vb = vec3_checked(b, "Vec3.Max: invalid vector");
    if (!va || !vb)
        return vec3_alloc(0, 0, 0);
    return vec3_alloc(fmax(va->x, vb->x), fmax(va->y, vb->y), fmax(va->z, vb->z));
}
