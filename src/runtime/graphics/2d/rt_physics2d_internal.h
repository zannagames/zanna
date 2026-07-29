//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_physics2d_internal.h
/// @brief Declares the shared Physics2D object layouts, capacity constants, and
///        cross-translation-unit solver helpers.
///
/// @details This private contract is shared by world integration, collision
/// detection, and joint solving. Body payloads begin with the runtime dispatch
/// slot, worlds own all growable pointer/record/scratch arrays, and membership
/// indices support constant-time removal when their owner metadata is current.
/// This header is not part of the public runtime C ABI.
///
// File: src/runtime/graphics/2d/rt_physics2d_internal.h
// Purpose: Shared internal types for the 2D physics engine, used by both
//   rt_physics2d.c and rt_physics2d_joint.c.
//
// Key invariants:
//   - rt_body_impl layout must match GC allocation expectations (vptr first).
//   - rt_world_impl stores bodies, joints, contacts, and step scratch in
//     world-owned growable arrays. PH_MAX_* constants are initial reservations.
//
// Ownership/Lifetime:
//   - Internal header — not part of public API.
//
// Links: src/runtime/graphics/2d/rt_physics2d.c, src/runtime/graphics/2d/rt_physics2d_joint.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_object.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/// @name Initial world reservations and solver iteration count
/// @details The `PH_MAX_*` values are initial capacities, not hard limits.
/// @{
#define PH_MAX_BODIES 256
#define PH_MAX_JOINTS 64
#define PH_MAX_CONTACTS 256
#define PH_JOINT_ITERATIONS 4
/// @}

/// @name String forms exported to runtime registration metadata
/// @{
#define RT_PH_MAX_BODIES_STR "256"
#define RT_PH_MAX_JOINTS_STR "64"
#define RT_PH_MAX_CONTACTS_STR "256"
/// @}

/// @name Physics2D runtime class identifiers
/// @{
#define RT_PHYSICS2D_WORLD_CLASS_ID INT64_C(-0x500201)
#define RT_PHYSICS2D_BODY_CLASS_ID INT64_C(-0x500202)
#define RT_PHYSICS2D_JOINT_CLASS_ID INT64_C(-0x500203)
#define RT_PHYSICS2D_PROJECTILE_CLASS_ID INT64_C(-0x500204)
/// @}

/// @brief Smallest supported positive dynamic mass.
/// @details Subnormal masses are normalized to the smallest normal `double`,
///          whose reciprocal is safely finite on every supported IEEE-754
///          platform. This is a numerical storage bound, not a gameplay-scale
///          recommendation.
#define PHYSICS2D_MIN_DYNAMIC_MASS DBL_MIN

/// @brief Internal representation of a single rigid body (AABB or circle).
/// @details AABB coordinates use a top-left origin, while circle coordinates
///          store the center. Current and previous-substep positions support
///          continuous collision detection. A world owns one reference while
///          the body is registered; @c owner_world and @c world_index cache
///          that membership.
typedef struct {
    void *vptr;  ///< Zia virtual-dispatch pointer (must be first).
    double x, y; ///< Top-left position (AABB) or center (circle).
    /// Previous integration-substep position used by swept collision tests.
    double prev_x, prev_y;   ///< Previous-step position used for one-way tile tests.
    double w, h;             ///< Width and height of the AABB (0 for circles).
    double vx, vy;           ///< Velocity in world-units per second.
    double fx, fy;           ///< Accumulated force for the current frame.
    double mass;             ///< Mass in arbitrary units. 0 = static (immovable).
    double inv_mass;         ///< Reciprocal of mass (1/mass), or 0 for static bodies.
    double restitution;      ///< Bounciness coefficient in [0, 1].
    double friction;         ///< Kinetic friction coefficient in [0, 1].
    int64_t collision_layer; ///< Bitmask: which physical layer(s) this body occupies.
    int64_t collision_mask;  ///< Bitmask: which layers this body can collide with.
    double radius;           ///< Circle radius (0 for AABB bodies).
    int8_t is_circle;        ///< 1 = circle shape, 0 = AABB shape.
    void *owner_world;       ///< World this body was added to, or NULL. Enables O(1)
                             ///< duplicate detection and removal (see rt_physics2d.c).
    int64_t world_index;     ///< Index of this body in owner_world->bodies; valid only
                             ///< while owner_world is non-NULL and points to that world.
} rt_body_impl;

/// @brief Forward declaration of the shared joint payload.
typedef struct ph_joint ph_joint;

/// @brief Retained contact manifold recorded for one world step.
/// @details Both body pointers hold references until contacts are cleared.
///          The normal points from body A toward body B and penetration is
///          nonnegative, with zero used by swept impacts.
typedef struct {
    rt_body_impl *body_a;
    rt_body_impl *body_b;
    double nx;
    double ny;
    double penetration;
} ph_contact_record;

/// @brief Internal representation of a physics world.
/// @details The world owns four categories of growable storage: retained body
///          and joint arrays, retained contact records, packed broad-phase pair
///          scratch, and parallel force-snapshot arrays used by public
///          substepping. Count fields describe initialized entries and capacity
///          fields describe allocated slots.
typedef struct {
    void *vptr;
    double gravity_x;
    double gravity_y;
    rt_body_impl **bodies;
    int64_t body_count;
    int64_t body_capacity;
    ph_joint **joints;
    int64_t joint_count;
    int64_t joint_capacity;

    ph_contact_record *contacts;
    int64_t contact_count;
    int64_t contact_capacity;
    int8_t contact_overflow; ///< Set when the most recent step could not grow contact storage.

    /* Broad-phase candidate-pair scratch. Each entry packs a body-index pair as
     * ((uint64_t)ii << 32) | (uint32_t)jj with ii < jj. Collected per step, sorted,
     * then de-duplicated — O(pairs) memory instead of the former O(n^2) bit-matrix
     * (which also cost an O(n^2) memset every substep). */
    uint64_t *pair_scratch;
    int64_t pair_scratch_count;
    int64_t pair_scratch_capacity;

    rt_body_impl **force_bodies;
    double *force_x;
    double *force_y;
    int64_t force_capacity;
} rt_world_impl;

/// @brief Joint internal representation.
/// @details Joint endpoints are retained while active. Fields are interpreted
///          according to @c type: distance/rope constraints use @c length,
///          springs also use stiffness/damping, and hinge constraints use the
///          world and local anchor fields.
struct ph_joint {
    void *vptr;   ///< Zia virtual-dispatch pointer (must be first).
    int32_t type; ///< RT_JOINT_DISTANCE, etc.
    void *body_a;
    void *body_b;
    double anchor_x, anchor_y;   ///< World-space anchor (hinge)
    double anchor_ax, anchor_ay; ///< Body A local hinge anchor offset from center.
    double anchor_bx, anchor_by; ///< Body B local hinge anchor offset from center.
    double length;               ///< Target distance (distance/rope)
    double stiffness;            ///< Spring stiffness
    double damping;              ///< Spring damping
    int8_t active;
};

/// @brief Test whether an opaque handle is a complete Physics2D.World payload.
/// @param obj Opaque runtime object candidate.
/// @return Nonzero only for a non-null world handle.
static inline int8_t rt_physics2d_is_world_handle(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_PHYSICS2D_WORLD_CLASS_ID, sizeof(rt_world_impl));
}

/// @brief Test whether an opaque handle is a complete Physics2D.Body payload.
/// @param obj Opaque runtime object candidate.
/// @return Nonzero only for a non-null body handle.
static inline int8_t rt_physics2d_is_body_handle(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_PHYSICS2D_BODY_CLASS_ID, sizeof(rt_body_impl));
}

/// @brief Test whether an opaque handle is a complete Physics2D.Joint payload.
/// @param obj Opaque runtime object candidate.
/// @return Nonzero only for a non-null joint handle.
static inline int8_t rt_physics2d_is_joint_handle(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_PHYSICS2D_JOINT_CLASS_ID, sizeof(ph_joint));
}

/// @brief Add two finite doubles, saturating instead of producing infinity.
/// @param a First finite operand.
/// @param b Second finite operand.
/// @return The mathematical sum clamped to `[-DBL_MAX, DBL_MAX]`.
static inline double rt_physics2d_saturating_add(double a, double b) {
    if (!isfinite(a))
        a = copysign(DBL_MAX, a);
    if (!isfinite(b))
        b = copysign(DBL_MAX, b);
    if (b > 0.0 && a > DBL_MAX - b)
        return DBL_MAX;
    if (b < 0.0 && a < -DBL_MAX - b)
        return -DBL_MAX;
    return a + b;
}

/// @brief Multiply two finite doubles, saturating instead of producing infinity.
/// @param a First finite operand.
/// @param b Second finite operand.
/// @return The mathematical product clamped to `[-DBL_MAX, DBL_MAX]`.
static inline double rt_physics2d_saturating_mul(double a, double b) {
    if (a == 0.0 || b == 0.0)
        return copysign(0.0, a * b);
    if (!isfinite(a) || !isfinite(b))
        return copysign(DBL_MAX, a * b);
    if (fabs(a) > DBL_MAX / fabs(b))
        return copysign(DBL_MAX, a * b);
    return a * b;
}

/// @brief Multiply four finite factors with one exponent-range decision.
/// @details Decomposing each operand with `frexp` avoids losing a representable
///          final result merely because an earlier pairwise product overflowed
///          before a later factor reduced its magnitude.
/// @param a First factor.
/// @param b Second factor.
/// @param c Third factor.
/// @param d Fourth factor.
/// @return The four-factor product clamped to `[-DBL_MAX, DBL_MAX]`.
static inline double rt_physics2d_saturating_mul4(double a, double b, double c, double d) {
    if (a == 0.0 || b == 0.0 || c == 0.0 || d == 0.0)
        return 0.0;
    int negative = signbit(a) ^ signbit(b) ^ signbit(c) ^ signbit(d);
    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d))
        return negative ? -DBL_MAX : DBL_MAX;
    int ea = 0, eb = 0, ec = 0, ed = 0;
    double mantissa =
        frexp(fabs(a), &ea) * frexp(fabs(b), &eb) * frexp(fabs(c), &ec) * frexp(fabs(d), &ed);
    double result = scalbn(mantissa, ea + eb + ec + ed);
    if (!isfinite(result))
        result = DBL_MAX;
    return negative ? -result : result;
}

/// @brief Divide finite values, saturating overflow instead of returning infinity.
/// @param numerator Dividend.
/// @param denominator Nonzero divisor.
/// @return The quotient clamped to `[-DBL_MAX, DBL_MAX]`.
static inline double rt_physics2d_saturating_div(double numerator, double denominator) {
    if (numerator == 0.0)
        return 0.0;
    if (!isfinite(numerator) || !isfinite(denominator) || denominator == 0.0)
        return (signbit(numerator) ^ signbit(denominator)) ? -DBL_MAX : DBL_MAX;
    double result = numerator / denominator;
    if (!isfinite(result))
        return copysign(DBL_MAX, result);
    return result;
}

/// @brief Normalize a public mass to the representable dynamic/static model.
/// @param mass Candidate mass.
/// @return Zero for invalid/static input, otherwise a positive mass with a
///         finite reciprocal.
static inline double rt_physics2d_normalize_mass(double mass) {
    if (!isfinite(mass) || mass <= 0.0)
        return 0.0;
    return mass < PHYSICS2D_MIN_DYNAMIC_MASS ? PHYSICS2D_MIN_DYNAMIC_MASS : mass;
}

/// @brief Compute stable normalized inverse-mass weights.
/// @details Avoids overflowing `inv_a + inv_b` when both inverse masses are
///          near `DBL_MAX`, and does not treat very massive dynamic bodies as
///          static merely because their inverse masses are small.
/// @param inv_a First nonnegative finite inverse mass.
/// @param inv_b Second nonnegative finite inverse mass.
/// @param share_a Receives `inv_a / (inv_a + inv_b)`.
/// @param share_b Receives `inv_b / (inv_a + inv_b)`.
/// @return Nonzero when at least one inverse mass is positive.
static inline int8_t rt_physics2d_inverse_mass_shares(double inv_a,
                                                      double inv_b,
                                                      double *share_a,
                                                      double *share_b) {
    if (!share_a || !share_b || !isfinite(inv_a) || !isfinite(inv_b) || inv_a < 0.0 ||
        inv_b < 0.0 || (inv_a == 0.0 && inv_b == 0.0)) {
        return 0;
    }
    if (inv_a >= inv_b) {
        double ratio = inv_b / inv_a;
        *share_a = 1.0 / (1.0 + ratio);
        *share_b = ratio * *share_a;
    } else {
        double ratio = inv_a / inv_b;
        *share_b = 1.0 / (1.0 + ratio);
        *share_a = ratio * *share_b;
    }
    return 1;
}

/// @brief Velocity-solve all rigid (distance/revolute) joints in @p world for
///        this @p dt step. Defined in rt_physics2d_joint.c.
/// @param world Opaque validated Physics2D.World handle.
/// @param dt Positive finite substep duration.
void rt_physics2d_solve_joints(void *world, double dt);
/// @brief Velocity-solve all spring joints in @p world for this @p dt step.
/// @param world Opaque validated Physics2D.World handle.
/// @param dt Positive finite substep duration.
void rt_physics2d_solve_spring_joints(void *world, double dt);
/// @brief Positional (Baumgarte/NGS) correction pass for joints after the
///        velocity solve, to remove residual constraint drift.
/// @param world Opaque validated Physics2D.World handle.
/// @param dt Positive finite substep duration used by constraint calculations.
void rt_physics2d_solve_position_joints(void *world, double dt);
/// @brief Velocity-projection pass for positional joints; removes the residual
///        constraint-axis relative velocity left by the position solve so it
///        cannot accumulate ("phantom velocity") across steps.
/// @param world Opaque validated Physics2D.World handle.
/// @param dt Substep duration retained for the common solver interface.
void rt_physics2d_solve_joint_velocities(void *world, double dt);

//=============================================================================
// Collision broad/narrow-phase (defined in rt_physics2d_collision.c)
//=============================================================================

/// @brief Broad/narrow-phase resolve for the body pair (ii, jj) in @p w for this
///        @p dt step: AABB/swept reject, shape overlap, then impulse resolution.
/// @param w Mutable validated world implementation.
/// @param ii First body-array index.
/// @param jj Second body-array index.
/// @param dt Substep duration retained for the collision interface.
void maybe_resolve_pair(rt_world_impl *w, int ii, int jj, double dt);

//=============================================================================
// Shared body/contact helpers (defined in rt_physics2d.c)
//=============================================================================

/// @brief Return the previous-substep minimum X edge of a body AABB.
/// @param b Borrowed body implementation.
/// @return Previous circle center X minus radius or previous AABB X.
double body_prev_min_x(rt_body_impl *b);
/// @brief Return the previous-substep minimum Y edge of a body AABB.
/// @param b Borrowed body implementation.
/// @return Previous circle center Y minus radius or previous AABB Y.
double body_prev_min_y(rt_body_impl *b);
/// @brief Return the previous-substep maximum X edge of a body AABB.
/// @param b Borrowed body implementation.
/// @return Previous circle center X plus radius or previous AABB X plus width.
double body_prev_max_x(rt_body_impl *b);
/// @brief Return the previous-substep maximum Y edge of a body AABB.
/// @param b Borrowed body implementation.
/// @return Previous circle center Y plus radius or previous AABB Y plus height.
double body_prev_max_y(rt_body_impl *b);
/// @brief Clamp a body's state to finite, sane values before integration.
/// @param b Mutable body implementation; `NULL` is accepted as a no-op.
void sanitize_body_state(rt_body_impl *b);
/// @brief Append a contact manifold to the world's per-step contact list.
/// @details Retains both bodies and sets the world's overflow flag if contact
///          storage cannot grow.
/// @param w Mutable world receiving the record.
/// @param a First body.
/// @param b Second body.
/// @param nx Contact-normal X component from @p a toward @p b.
/// @param ny Contact-normal Y component from @p a toward @p b.
/// @param pen Penetration depth, clamped to a nonnegative value.
void world_record_contact(
    rt_world_impl *w, rt_body_impl *a, rt_body_impl *b, double nx, double ny, double pen);

/// @brief Ensure the world's joint array can hold at least @p needed entries.
/// @param w Mutable world whose joint array may be reallocated.
/// @param needed Nonnegative minimum number of slots.
/// @return Nonzero when capacity is sufficient or growth succeeds; zero on
///         invalid input, overflow, or allocation failure.
int8_t rt_physics2d_world_reserve_joint_capacity(rt_world_impl *w, int64_t needed);
