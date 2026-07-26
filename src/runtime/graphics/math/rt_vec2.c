//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_vec2.c
// Purpose: 2D vector mathematics (x, y doubles) for Zanna games and graphics.
//   Provides immutable Vec2 objects with arithmetic (+,-,×,÷), dot product,
//   cross product magnitude, length/normalize, distance, linear interpolation,
//   angle and rotation operations. Used pervasively in physics, camera math,
//   UI layout, and any API that exchanges 2D positions or directions.
//
// Key invariants:
//   - A Vec2 stores two double fields (x, y); 16 bytes, no padding:
//
//       Vec2(3, 4):                     +Y
//                    *  (3, 4)           ^
//                   /|                  |
//              len / |  4 (y)    -------+-------> +X
//                 /  |                  |
//                +---+
//                 3 (x)    len = sqrt(3² + 4²) = 5
//
//   - Coordinate system: standard Cartesian (+X right, +Y up). Screen/Canvas
//     space has +Y downward — callers may need to negate Y when mixing the two.
//   - All operations return new Vec2 objects (no mutation after creation),
//     making Vec2 inherently safe for concurrent reads across threads.
//   - Vec2 uses a thread-local LIFO free-list pool (VEC2_POOL_CAPACITY = 32)
//     to amortize GC pressure in hot inner loops (physics, particle systems).
//     Pool objects are kept alive via rt_obj_resurrect() and returned to the
//     pool by the finalizer.
//
// Ownership/Lifetime:
//   - Vec2 objects are GC-managed. Pool slots are reclaimed by the pool's
//     finalizer path; non-pooled Vec2s are collected by the standard GC.
//     Callers must not free Vec2s manually.
//
// Links: src/runtime/graphics/math/rt_vec2.h (public API),
//        src/runtime/graphics/math/rt_vec3.c (3D counterpart),
//        src/runtime/graphics/math/rt_mat3.c (matrix-vector transform consumer)
//
//===----------------------------------------------------------------------===//

#include "rt_vec2.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"

#include <math.h>

// ============================================================================
// Thread-local free-list pool (P2-3.6)
// ============================================================================
// Vec2 objects follow an allocate→use→release-immediately pattern in tight
// loops, making a LIFO pool highly effective.  Each pooled object keeps its
// heap header alive (refcount=1 inside the pool) and is returned to callers
// without any additional heap allocation.
//
// Pool finalizer lifecycle:
//   1. vec2_alloc():  fresh object gets vec2_pool_return as finalizer.
//   2. User releases: refcount→0, rt_obj_free calls finalizer.
//   3. vec2_pool_return: if space, rt_obj_resurrect()+re-arm finalizer, push.
//                        if full, fall through to rt_heap_free_zero_ref (free).
//   4. vec2_alloc(): pool non-empty → pop, re-init fields, return (no malloc).

#define VEC2_POOL_CAPACITY 32

static _Thread_local void *vec2_pool_buf_[VEC2_POOL_CAPACITY];
static _Thread_local int vec2_pool_top_ = 0;

/// @brief Vec2 pool return-on-finalize — resurrects the object instead of freeing it.
///
/// Set as the GC finalizer on every Vec2 from the pool. When the
/// last reference drops, instead of returning the memory to the
/// general allocator we resurrect the object (refcount 0→1) and
/// push it back onto the pool, where the next `rt_vec2_new` can
/// claim it for free. Cap is `VEC2_POOL_CAPACITY` per thread; once
/// full, excess Vec2s fall through to normal heap free.
/// @param p Vec2 payload whose finalizer is running.
static void vec2_pool_return(void *p) {
    if (vec2_pool_top_ < VEC2_POOL_CAPACITY) {
        rt_obj_resurrect(p);                       // refcount 0 → 1
        rt_obj_set_finalizer(p, vec2_pool_return); // re-arm for next cycle
        vec2_pool_buf_[vec2_pool_top_++] = p;
    }
    // else: pool full — let rt_heap_free_zero_ref free the object normally.
}

/// @brief Internal Vec2 implementation structure.
///
/// Stores the X and Y components of a 2D vector as double-precision
/// floating-point values. The structure is allocated as a Zanna object
/// with reference counting support.
///
/// @note Vec2 is immutable - all operations create new instances.
typedef struct {
    double x; ///< X component (horizontal axis, positive = right)
    double y; ///< Y component (vertical axis, positive = up in math, down in screen coords)
} ZannaVec2;

/// @brief Return whether @p v is a Vec2-compatible heap payload.
/// @details Accepts both the explicit Vec2 class id used by current constructors and the
///          historical class-id-zero value object layout. Classless payloads must be exactly the
///          Vec2 byte size so unrelated raw heap objects are not treated as vectors.
/// @param v Candidate runtime object payload.
/// @return 1 for a compatible Vec2 payload, otherwise 0.
static int vec2_is_compatible_object(void *v) {
    if (!v)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(v, &heap_info))
        return 0;
    if (heap_info.kind != RT_HEAP_OBJECT || heap_info.elem_kind != RT_ELEM_NONE)
        return 0;
    if (heap_info.class_id == RT_VEC2_CLASS_ID)
        return heap_info.cap >= sizeof(ZannaVec2);
    return heap_info.class_id == 0 && heap_info.len == sizeof(ZannaVec2) &&
           heap_info.cap == sizeof(ZannaVec2);
}

/// @brief Validate and cast an opaque handle to a Vec2 payload.
/// @details Rejects NULL, non-object heap payloads, incompatible class identifiers, and
///   undersized allocations before any Vec2 component is read.
/// @param v Candidate Vec2 runtime handle.
/// @param op Diagnostic prefix used if validation fails.
/// @return Typed Vec2 payload, or NULL after trapping.
static ZannaVec2 *vec2_checked(void *v, const char *op) {
    if (!vec2_is_compatible_object(v)) {
        rt_trap(op ? op : "Vec2: invalid vector");
        return NULL;
    }
    return (ZannaVec2 *)v;
}

/// @brief Compute a finite, overflow-resistant Vec2 length from raw components.
/// @details Uses `hypot` instead of `sqrt(x*x + y*y)` so large finite components
///          do not overflow while squaring. Non-finite inputs return `INFINITY`,
///          letting normalization callers fall back deterministically.
/// @param x Vector x component.
/// @param y Vector y component.
/// @return Euclidean length, or positive infinity when either component is non-finite.
static double vec2_safe_len(double x, double y) {
    if (!isfinite(x) || !isfinite(y))
        return INFINITY;
    return hypot(x, y);
}

/// @brief Allocate and initialize a new Vec2 with the given components.
///
/// This internal helper allocates a Vec2 object through the Zanna object
/// system and initializes it with the provided X and Y values.
///
/// @param x The X component value.
/// @param y The Y component value.
///
/// @return Pointer to the newly allocated Vec2. Traps on allocation failure.
///
/// @note This is an internal function - use rt_vec2_new() for public API.
static ZannaVec2 *vec2_alloc(double x, double y) {
    ZannaVec2 *v;
    if (vec2_pool_top_ > 0) {
        // Fast path: reuse a pooled object (no heap allocation).
        v = (ZannaVec2 *)vec2_pool_buf_[--vec2_pool_top_];
    } else {
        // Slow path: heap-allocate a fresh object and arm the pool finalizer.
        v = (ZannaVec2 *)rt_obj_new_i64(RT_VEC2_CLASS_ID, (int64_t)sizeof(ZannaVec2));
        if (!v) {
            rt_trap("Vec2: memory allocation failed");
            return NULL; // Unreachable after trap
        }
        rt_obj_set_finalizer(v, vec2_pool_return);
    }
    v->x = x;
    v->y = y;
    return v;
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Creates a new 2D vector with the specified X and Y components.
///
/// This is the primary constructor for creating Vec2 instances with custom
/// component values.
///
/// **Usage example:**
/// ```
/// Dim position = Vec2.New(100.0, 50.0)   ' Position at (100, 50)
/// Dim velocity = Vec2.New(5.0, -2.0)     ' Moving right and up
/// Dim direction = Vec2.New(1.0, 0.0)     ' Unit vector pointing right
/// ```
///
/// @param x The X component (horizontal position/direction).
/// @param y The Y component (vertical position/direction).
///
/// @return A new Vec2 object with the specified components.
///
/// @note O(1) time complexity.
/// @note The returned Vec2 is reference-counted and garbage collected.
///
/// @see rt_vec2_zero For creating a zero vector
/// @see rt_vec2_one For creating a unit vector (1, 1)
void *rt_vec2_new(double x, double y) {
    return vec2_alloc(x, y);
}

/// @brief Creates a zero vector (0, 0).
///
/// Returns a Vec2 with both components set to zero. The zero vector is the
/// identity element for vector addition and represents "no direction" or
/// "origin point."
///
/// **Mathematical Properties:**
/// - v + Vec2.Zero() = v (additive identity)
/// - v * 0 = Vec2.Zero()
/// - Length of zero vector = 0
///
/// **Usage example:**
/// ```
/// Dim origin = Vec2.Zero()       ' (0, 0)
/// Dim velocity = Vec2.Zero()     ' Not moving
///
/// If velocity.Len() = 0 Then
///     Print "Object is stationary"
/// End If
/// ```
///
/// @return A new Vec2 object with components (0, 0).
///
/// @note O(1) time complexity.
///
/// @see rt_vec2_one For a unit vector
/// @see rt_vec2_len For checking if a vector is zero-length
void *rt_vec2_zero(void) {
    return vec2_alloc(0.0, 0.0);
}

/// @brief Creates a unit vector (1, 1).
///
/// Returns a Vec2 with both components set to one. Note that this vector
/// has a length of sqrt(2) ≈ 1.414, not 1. For a true unit vector, use
/// Vec2.New(1, 0) or normalize any non-zero vector.
///
/// **Usage example:**
/// ```
/// Dim scale = Vec2.One()         ' (1, 1)
/// Dim doubled = scale.Mul(2)     ' (2, 2)
///
/// ' Component-wise scaling
/// Dim size = Vec2.New(width, height)
/// Dim halfSize = size.Mul(0.5)   ' Scale by 0.5 in both dimensions
/// ```
///
/// **Note on Length:**
/// ```
/// Vec2.One() length = sqrt(1² + 1²) = sqrt(2) ≈ 1.414
/// ```
///
/// @return A new Vec2 object with components (1, 1).
///
/// @note O(1) time complexity.
/// @note Length is sqrt(2), not 1. Use rt_vec2_norm() for true unit vectors.
///
/// @see rt_vec2_zero For a zero vector
/// @see rt_vec2_norm For creating unit vectors
void *rt_vec2_one(void) {
    return vec2_alloc(1.0, 1.0);
}

//=============================================================================
// Property Accessors
//=============================================================================

/// @brief Gets the X component of the vector.
///
/// Returns the horizontal component of the 2D vector. In a standard Cartesian
/// coordinate system, positive X points to the right.
///
/// **Usage example:**
/// ```
/// Dim pos = Vec2.New(100.0, 50.0)
/// Print pos.X    ' Outputs: 100
///
/// ' Use X for horizontal position checks
/// If pos.X > screenWidth Then
///     Print "Object is off-screen to the right"
/// End If
/// ```
///
/// @param v Pointer to a Vec2 object. Must not be NULL.
///
/// @return The X component value as a double.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.X: null vector" if v is NULL.
///
/// @see rt_vec2_y For the Y component
/// @see rt_vec2_new For creating vectors with specific components
double rt_vec2_x(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.X: invalid vector");
    if (!vec)
        return 0.0;
    return vec->x;
}

/// @brief Gets the Y component of the vector.
///
/// Returns the vertical component of the 2D vector. In mathematical coordinates,
/// positive Y points upward; in screen coordinates, positive Y typically points
/// downward.
///
/// **Usage example:**
/// ```
/// Dim pos = Vec2.New(100.0, 50.0)
/// Print pos.Y    ' Outputs: 50
///
/// ' Use Y for vertical position checks
/// If pos.Y < 0 Then
///     Print "Object is above the top of the screen"
/// End If
/// ```
///
/// @param v Pointer to a Vec2 object. Must not be NULL.
///
/// @return The Y component value as a double.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Y: null vector" if v is NULL.
///
/// @see rt_vec2_x For the X component
/// @see rt_vec2_new For creating vectors with specific components
double rt_vec2_y(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Y: invalid vector");
    if (!vec)
        return 0.0;
    return vec->y;
}

//=============================================================================
// Arithmetic Operations
//=============================================================================

/// @brief Adds two vectors component-wise.
///
/// Performs vector addition by adding corresponding components:
/// result = (a.x + b.x, a.y + b.y)
///
/// **Visual representation:**
/// ```
///           b
///          /
///         /
///        *-----> a + b (resultant)
///       /      /
///      /      /
///     +----->
///         a
/// ```
///
/// **Usage example:**
/// ```
/// Dim pos = Vec2.New(100.0, 50.0)
/// Dim velocity = Vec2.New(5.0, -2.0)
/// Dim newPos = pos.Add(velocity)   ' (105, 48)
///
/// ' Chain additions for multiple forces
/// Dim totalForce = gravity.Add(wind).Add(thrust)
/// ```
///
/// @param a First vector operand. Must not be NULL.
/// @param b Second vector operand. Must not be NULL.
///
/// @return A new Vec2 containing the component-wise sum.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Add: null vector" if either operand is NULL.
/// @note Vector addition is commutative: a + b = b + a
///
/// @see rt_vec2_sub For vector subtraction
/// @see rt_vec2_mul For scalar multiplication
void *rt_vec2_add(void *a, void *b) {
    ZannaVec2 *va = vec2_checked(a, "Vec2.Add: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Add: invalid vector");
    if (!va || !vb)
        return NULL;
    return vec2_alloc(va->x + vb->x, va->y + vb->y);
}

/// @brief Subtracts vector b from vector a component-wise.
///
/// Performs vector subtraction: result = (a.x - b.x, a.y - b.y)
///
/// Subtraction can be visualized as finding the vector from b to a,
/// or equivalently, adding a to the negation of b.
///
/// **Usage example:**
/// ```
/// Dim target = Vec2.New(200.0, 150.0)
/// Dim position = Vec2.New(100.0, 100.0)
/// Dim direction = target.Sub(position)  ' (100, 50) - vector from pos to target
///
/// ' Calculate relative velocity
/// Dim relativeVel = shipVelocity.Sub(asteroidVelocity)
/// ```
///
/// @param a Vector to subtract from (minuend). Must not be NULL.
/// @param b Vector to subtract (subtrahend). Must not be NULL.
///
/// @return A new Vec2 containing the component-wise difference (a - b).
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Sub: null vector" if either operand is NULL.
/// @note Not commutative: a - b ≠ b - a (but a - b = -(b - a))
///
/// @see rt_vec2_add For vector addition
/// @see rt_vec2_neg For vector negation
void *rt_vec2_sub(void *a, void *b) {
    ZannaVec2 *va = vec2_checked(a, "Vec2.Sub: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Sub: invalid vector");
    if (!va || !vb)
        return NULL;
    return vec2_alloc(va->x - vb->x, va->y - vb->y);
}

/// @brief Multiplies a vector by a scalar value.
///
/// Scales both components of the vector by the given scalar:
/// result = (v.x * s, v.y * s)
///
/// **Effect of scalar values:**
/// - s > 1: Lengthens the vector
/// - 0 < s < 1: Shortens the vector
/// - s = 1: No change
/// - s = 0: Returns zero vector
/// - s < 0: Reverses direction and scales
///
/// **Usage example:**
/// ```
/// Dim direction = Vec2.New(1.0, 0.0)
/// Dim speed = 5.0
/// Dim velocity = direction.Mul(speed)  ' (5, 0)
///
/// ' Double the velocity
/// velocity = velocity.Mul(2.0)          ' (10, 0)
///
/// ' Reverse direction
/// velocity = velocity.Mul(-1.0)         ' (-10, 0)
/// ```
///
/// @param v Vector to scale. Must not be NULL.
/// @param s Scalar multiplier.
///
/// @return A new Vec2 with components scaled by s.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Mul: null vector" if v is NULL.
///
/// @see rt_vec2_div For scalar division
/// @see rt_vec2_norm For getting a unit vector
void *rt_vec2_mul(void *v, double s) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Mul: invalid vector");
    if (!vec)
        return NULL;
    return vec2_alloc(vec->x * s, vec->y * s);
}

/// @brief Divides a vector by a scalar value.
///
/// Divides both components of the vector by the given scalar:
/// result = (v.x / s, v.y / s)
///
/// This is equivalent to multiplying by (1/s) but with explicit
/// division-by-zero checking.
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec2.New(10.0, 6.0)
/// Dim halfSpeed = velocity.Div(2.0)     ' (5, 3)
///
/// ' Convert velocity to direction (unit vector)
/// Dim speed = velocity.Len()
/// Dim direction = velocity.Div(speed)   ' Normalized direction
/// ```
///
/// @param v Vector to divide. Must not be NULL.
/// @param s Scalar divisor. Must be finite and nonzero.
///
/// @return A new Vec2 with components divided by s.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Div: null vector" if v is NULL.
/// @note Traps with "Vec2.Div: invalid divisor" if s is zero or non-finite.
///
/// @see rt_vec2_mul For scalar multiplication
/// @see rt_vec2_norm For normalizing to unit length
void *rt_vec2_div(void *v, double s) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Div: invalid vector");
    if (!vec)
        return NULL;
    if (!isfinite(s) || s == 0.0) {
        rt_trap("Vec2.Div: invalid divisor");
        return NULL;
    }
    return vec2_alloc(vec->x / s, vec->y / s);
}

/// @brief Negates a vector (reverses its direction).
///
/// Returns a vector pointing in the opposite direction with the same magnitude:
/// result = (-v.x, -v.y)
///
/// **Visual representation:**
/// ```
///         v
///        --->
///
///        <---
///        -v (same length, opposite direction)
/// ```
///
/// **Usage example:**
/// ```
/// Dim forward = Vec2.New(1.0, 0.0)
/// Dim backward = forward.Neg()          ' (-1, 0)
///
/// ' Reflect velocity on collision
/// velocity = velocity.Neg()             ' Bounce back
/// ```
///
/// **Mathematical Properties:**
/// - -(-v) = v (double negation)
/// - v + (-v) = Vec2.Zero()
/// - ||-v|| = ||v|| (same length)
///
/// @param v Vector to negate. Must not be NULL.
///
/// @return A new Vec2 pointing in the opposite direction.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Neg: null vector" if v is NULL.
/// @note Equivalent to v.Mul(-1)
///
/// @see rt_vec2_mul For scalar multiplication
/// @see rt_vec2_sub For vector subtraction
void *rt_vec2_neg(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Neg: invalid vector");
    if (!vec)
        return NULL;
    return vec2_alloc(-vec->x, -vec->y);
}

//=============================================================================
// Vector Products
//=============================================================================

/// @brief Computes the dot product (scalar product) of two vectors.
///
/// The dot product is a fundamental operation that returns a scalar value:
/// a · b = a.x * b.x + a.y * b.y = |a| * |b| * cos(θ)
///
/// where θ is the angle between the vectors.
///
/// **Geometric Interpretation:**
/// ```
///         b
///        /
///       / θ
///      +------> a
///
/// dot = |a| * |b| * cos(θ)
///
/// If θ = 0°   (same direction):     dot = |a| * |b|  (maximum positive)
/// If θ = 90°  (perpendicular):      dot = 0
/// If θ = 180° (opposite direction): dot = -|a| * |b| (maximum negative)
/// ```
///
/// **Common Uses:**
/// - Check if vectors are perpendicular: dot == 0
/// - Check if vectors point in same direction: dot > 0
/// - Check if vectors point in opposite directions: dot < 0
/// - Calculate projection of one vector onto another
/// - Calculate work done: W = F · d
///
/// **Usage example:**
/// ```
/// Dim forward = Vec2.New(1.0, 0.0)
/// Dim movement = Vec2.New(0.5, 0.5)
/// Dim forwardComponent = forward.Dot(movement)  ' 0.5
///
/// ' Check if enemy is in front of player
/// Dim toEnemy = enemy.Sub(player).Norm()
/// If playerDirection.Dot(toEnemy) > 0 Then
///     Print "Enemy is in front"
/// End If
/// ```
///
/// @param a First vector. Must not be NULL.
/// @param b Second vector. Must not be NULL.
///
/// @return The dot product as a scalar value.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Dot: null vector" if either operand is NULL.
/// @note Dot product is commutative: a · b = b · a
///
/// @see rt_vec2_cross For the 2D cross product
/// @see rt_vec2_angle For getting the angle of a vector
double rt_vec2_dot(void *a, void *b) {
    ZannaVec2 *va = vec2_checked(a, "Vec2.Dot: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Dot: invalid vector");
    if (!va || !vb)
        return 0.0;
    return va->x * vb->x + va->y * vb->y;
}

/// @brief Computes the 2D cross product (perpendicular dot product).
///
/// The 2D cross product returns a scalar representing the z-component of
/// the 3D cross product when treating the 2D vectors as 3D vectors with z=0:
/// a × b = a.x * b.y - a.y * b.x = |a| * |b| * sin(θ)
///
/// **Geometric Interpretation:**
/// ```
///         b
///        /
///       / θ
///      +------> a
///
/// cross = |a| * |b| * sin(θ)
/// ```
///
/// The result represents:
/// - The signed area of the parallelogram formed by the two vectors
/// - Positive if b is counter-clockwise from a
/// - Negative if b is clockwise from a
/// - Zero if vectors are parallel or anti-parallel
///
/// **Common Uses:**
/// - Determine the winding order (clockwise vs counter-clockwise)
/// - Calculate signed area of triangles/polygons
/// - Determine which side of a line a point is on
/// - Calculate torque in 2D physics
///
/// **Usage example:**
/// ```
/// Dim a = Vec2.New(1.0, 0.0)   ' Pointing right
/// Dim b = Vec2.New(0.0, 1.0)   ' Pointing up
/// Print a.Cross(b)              ' 1 (b is CCW from a)
/// Print b.Cross(a)              ' -1 (a is CW from b)
///
/// ' Check if point C is left of line from A to B
/// Dim AB = B.Sub(A)
/// Dim AC = C.Sub(A)
/// If AB.Cross(AC) > 0 Then
///     Print "C is to the left of line AB"
/// End If
/// ```
///
/// @param a First vector. Must not be NULL.
/// @param b Second vector. Must not be NULL.
///
/// @return The 2D cross product as a scalar value.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Cross: null vector" if either operand is NULL.
/// @note Cross product is anti-commutative: a × b = -(b × a)
///
/// @see rt_vec2_dot For the dot product
/// @see rt_vec3_cross For the 3D cross product (returns a vector)
double rt_vec2_cross(void *a, void *b) {
    // 2D cross product returns the scalar z-component of the 3D cross product
    // (treating vectors as 3D with z=0): a.x * b.y - a.y * b.x
    ZannaVec2 *va = vec2_checked(a, "Vec2.Cross: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Cross: invalid vector");
    if (!va || !vb)
        return 0.0;
    return va->x * vb->y - va->y * vb->x;
}

//=============================================================================
// Length and Distance
//=============================================================================

/// @brief Computes the squared length (magnitude squared) of the vector.
///
/// Returns |v|² = v.x² + v.y²
///
/// The squared length avoids the expensive square root operation, making it
/// ideal for comparisons where the actual length isn't needed.
///
/// **Performance Optimization:**
/// When comparing lengths, use LenSq instead of Len to avoid sqrt:
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
/// Dim v = Vec2.New(3.0, 4.0)
/// Print v.LenSq()    ' 25 (= 3² + 4²)
/// Print v.Len()      ' 5  (= sqrt(25))
///
/// ' Check if within radius (no sqrt needed)
/// Dim maxDistSq = radius * radius
/// If position.LenSq() < maxDistSq Then
///     Print "Within radius"
/// End If
/// ```
///
/// @param v Vector to measure. Must not be NULL.
///
/// @return The squared length as a non-negative value.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.LenSq: null vector" if v is NULL.
/// @note Prefer LenSq over Len when only comparing magnitudes.
///
/// @see rt_vec2_len For the actual length
/// @see rt_vec2_dist For distance between two points
double rt_vec2_len_sq(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.LenSq: invalid vector");
    if (!vec)
        return 0.0;
    return vec->x * vec->x + vec->y * vec->y;
}

/// @brief Computes the length (magnitude) of the vector.
///
/// Returns the Euclidean length: |v| = sqrt(v.x² + v.y²)
///
/// The length represents how "long" the vector is, regardless of direction.
///
/// **Pythagorean Theorem:**
/// ```
/// For Vec2(3, 4):
///
///        *  (3, 4)
///       /|
///      / |
///  5  /  | 4
///    /   |
///   +----+
///     3
///
/// len = sqrt(3² + 4²) = sqrt(25) = 5
/// ```
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec2.New(3.0, 4.0)
/// Dim speed = velocity.Len()    ' 5.0
///
/// ' Check if moving fast enough
/// If speed > 10.0 Then
///     Print "High speed!"
/// End If
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
/// @note For comparisons, prefer rt_vec2_len_sq to avoid sqrt.
/// @note Traps if v is not a compatible Vec2 handle.
///
/// @see rt_vec2_len_sq For squared length (faster for comparisons)
/// @see rt_vec2_norm For getting a unit-length vector
double rt_vec2_len(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Len: invalid vector");
    if (!vec)
        return 0.0;
    return vec2_safe_len(vec->x, vec->y);
}

/// @brief Computes the Euclidean distance between two points.
///
/// Returns the straight-line distance: dist = |b - a| = sqrt((b.x-a.x)² + (b.y-a.y)²)
///
/// This is equivalent to (but more efficient than): b.Sub(a).Len()
///
/// **Visual representation:**
/// ```
///         * b
///        /
///       / dist
///      /
///     * a
/// ```
///
/// **Usage example:**
/// ```
/// Dim player = Vec2.New(100.0, 100.0)
/// Dim enemy = Vec2.New(150.0, 130.0)
/// Dim distance = player.Dist(enemy)
///
/// If distance < 50.0 Then
///     Print "Enemy is nearby!"
/// End If
///
/// ' Find closest point
/// Dim closest = Nothing
/// Dim closestDist = 999999.0
/// For Each point In points
///     Dim d = position.Dist(point)
///     If d < closestDist Then
///         closestDist = d
///         closest = point
///     End If
/// Next
/// ```
///
/// @param a First point (start). Must not be NULL.
/// @param b Second point (end). Must not be NULL.
///
/// @return The distance as a non-negative value.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Dist: null vector" if either point is NULL.
/// @note Distance is symmetric: a.Dist(b) = b.Dist(a)
///
/// @see rt_vec2_len For the length of a single vector
/// @see rt_vec2_sub For the difference vector between two points
double rt_vec2_dist(void *a, void *b) {
    ZannaVec2 *va = vec2_checked(a, "Vec2.Dist: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Dist: invalid vector");
    if (!va || !vb)
        return 0.0;
    double dx = vb->x - va->x;
    double dy = vb->y - va->y;
    return vec2_safe_len(dx, dy);
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
///
/// **Visual representation:**
/// ```
/// Original vector:        Normalized (unit) vector:
///
///        *                      *
///       /                      /
///      /  len=5               /  len=1
///     /                      /
///    +                      +
/// ```
///
/// **Special Case:**
/// If the input vector has zero or non-finite length, returns a zero vector
/// (0, 0) rather than dividing by an unusable magnitude.
///
/// **Usage example:**
/// ```
/// Dim velocity = Vec2.New(3.0, 4.0)
/// Dim direction = velocity.Norm()   ' (0.6, 0.8) - length is 1.0
///
/// ' Use unit vector to move at constant speed
/// Dim speed = 10.0
/// Dim movement = direction.Mul(speed)
///
/// ' Face toward target
/// Dim toTarget = target.Sub(position)
/// Dim facing = toTarget.Norm()
/// ```
///
/// @param v Vector to normalize. Must not be NULL.
///
/// @return A new unit vector, or zero vector if the input length is zero or non-finite.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Norm: null vector" if v is NULL.
/// @note Safe for zero-length and non-finite vectors (returns zero vector).
///
/// @see rt_vec2_len For getting the length
/// @see rt_vec2_div For manual normalization
void *rt_vec2_norm(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Norm: invalid vector");
    if (!vec)
        return NULL;
    double len = vec2_safe_len(vec->x, vec->y);
    if (len == 0.0 || !isfinite(len)) {
        // Return zero vector for zero-length input
        return vec2_alloc(0.0, 0.0);
    }
    return vec2_alloc(vec->x / len, vec->y / len);
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
/// **Visual representation:**
/// ```
///     a ----+----+----+---- b
///     t=0  0.25 0.5 0.75  t=1
/// ```
///
/// **Usage example:**
/// ```
/// Dim start = Vec2.New(0.0, 0.0)
/// Dim target = Vec2.New(100.0, 50.0)
///
/// ' Animate position over time
/// Dim progress = elapsedTime / totalTime   ' 0.0 to 1.0
/// Dim currentPos = start.Lerp(target, progress)
///
/// ' Smooth camera follow
/// Dim cameraTarget = player.Position
/// camera = camera.Lerp(cameraTarget, 0.1)  ' Smooth 10% per frame
///
/// ' Find midpoint
/// Dim midpoint = start.Lerp(target, 0.5)
/// ```
///
/// @param a Starting vector (returned when t = 0). Must not be NULL.
/// @param b Ending vector (returned when t = 1). Must not be NULL.
/// @param t Interpolation parameter (typically 0.0 to 1.0).
///
/// @return A new Vec2 representing the interpolated position.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Lerp: null vector" if either vector is NULL.
/// @note Values of t outside [0, 1] will extrapolate beyond a and b.
///
/// @see rt_vec2_add For vector addition
/// @see rt_vec2_sub For vector subtraction
void *rt_vec2_lerp(void *a, void *b, double t) {
    ZannaVec2 *va = vec2_checked(a, "Vec2.Lerp: invalid vector");
    ZannaVec2 *vb = vec2_checked(b, "Vec2.Lerp: invalid vector");
    if (!va || !vb)
        return NULL;
    // lerp(a, b, t) = a + (b - a) * t = a * (1 - t) + b * t
    double x = va->x + (vb->x - va->x) * t;
    double y = va->y + (vb->y - va->y) * t;
    return vec2_alloc(x, y);
}

//=============================================================================
// Angle and Rotation
//=============================================================================

/// @brief Gets the angle of the vector from the positive X-axis.
///
/// Returns the angle in radians using atan2(y, x), which correctly handles
/// all quadrants and returns values in the range [-π, π].
///
/// **Angle convention:**
/// ```
///                  π/2 (+90°)
///                    |
///        Quadrant II | Quadrant I
///           π -------+------- 0
///       Quadrant III | Quadrant IV
///                    |
///                 -π/2 (-90°)
/// ```
///
/// **Examples:**
/// - Vec2(1, 0)  → 0 radians (pointing right)
/// - Vec2(0, 1)  → π/2 radians (pointing up)
/// - Vec2(-1, 0) → π radians (pointing left)
/// - Vec2(0, -1) → -π/2 radians (pointing down)
///
/// **Usage example:**
/// ```
/// Dim direction = Vec2.New(1.0, 1.0)
/// Dim angle = direction.Angle()     ' π/4 radians (45°)
///
/// ' Convert to degrees
/// Dim degrees = angle * 180.0 / 3.14159
///
/// ' Rotate sprite to face direction
/// sprite.Rotation = direction.Angle()
///
/// ' Create vector from angle
/// Dim newDir = Vec2.New(Cos(angle), Sin(angle))
/// ```
///
/// @param v Vector to measure. Must not be NULL.
///
/// @return The angle in radians, in the range [-π, π].
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Angle: null vector" if v is NULL.
/// @note Returns 0 for zero-length vectors (atan2(0, 0) behavior).
///
/// @see rt_vec2_rotate For rotating a vector by an angle
/// @see rt_vec2_norm For getting the direction as a unit vector
double rt_vec2_angle(void *v) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Angle: invalid vector");
    if (!vec)
        return 0.0;
    return atan2(vec->y, vec->x);
}

/// @brief Rotates the vector by the given angle (in radians).
///
/// Applies a 2D rotation transformation using the standard rotation matrix:
/// ```
/// | cos(θ)  -sin(θ) |   | x |   | x*cos(θ) - y*sin(θ) |
/// |                 | × |   | = |                     |
/// | sin(θ)   cos(θ) |   | y |   | x*sin(θ) + y*cos(θ) |
/// ```
///
/// **Rotation direction:**
/// - Positive angle: Counter-clockwise rotation
/// - Negative angle: Clockwise rotation
///
/// **Visual representation:**
/// ```
///         rotated
///           ↗
///          /
///         / angle
///        +-------→ original
/// ```
///
/// **Usage example:**
/// ```
/// Dim forward = Vec2.New(1.0, 0.0)   ' Pointing right
///
/// ' Rotate 90 degrees (π/2 radians)
/// Dim up = forward.Rotate(3.14159 / 2)  ' Now pointing up
///
/// ' Rotate by player input
/// Dim turnSpeed = 0.1
/// direction = direction.Rotate(turnSpeed * deltaTime)
///
/// ' Create circular motion
/// For i = 0 To 359
///     Dim angle = i * 3.14159 / 180
///     Dim point = Vec2.New(1, 0).Rotate(angle).Mul(radius)
///     ' point traces a circle
/// Next
/// ```
///
/// @param v Vector to rotate. Must not be NULL.
/// @param angle Rotation angle in radians (positive = CCW, negative = CW).
///
/// @return A new Vec2 that is the original vector rotated by the angle.
///
/// @note O(1) time complexity.
/// @note Traps with "Vec2.Rotate: null vector" if v is NULL.
/// @note The magnitude is preserved for finite components and a finite angle, subject to
///       floating-point rounding.
///
/// @see rt_vec2_angle For getting the current angle of a vector
/// @see rt_vec2_norm For unit vectors (often used with rotation)
void *rt_vec2_rotate(void *v, double angle) {
    ZannaVec2 *vec = vec2_checked(v, "Vec2.Rotate: invalid vector");
    if (!vec)
        return NULL;
    double c = cos(angle);
    double s = sin(angle);
    double x = vec->x * c - vec->y * s;
    double y = vec->x * s + vec->y * c;
    return vec2_alloc(x, y);
}
