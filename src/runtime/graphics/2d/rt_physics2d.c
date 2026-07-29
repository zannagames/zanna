//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_physics2d.c
/// @brief Implements bounded-step 2D rigid-body worlds, body state, contact
///        queries, and analytic projectile trajectories.
///
/// @details The rigid-body subsystem integrates axis-aligned boxes and circles,
/// retains bodies and joints through world membership, records queryable
/// per-step contacts, and uses a deterministic adaptive-grid broad phase with
/// an exhaustive fallback. The Projectile2D subsystem is independent of world
/// simulation and evaluates closed-form motion under constant gravity and
/// optional linear drag.
///
// File: src/runtime/graphics/2d/rt_physics2d.c
// Purpose: Simple 2D rigid-body physics engine with AABB/circle collision detection
//   and impulse-based collision response. Designed for game use cases: enemies,
//   platforms, bullets, and other simple rectangular entities. Intentionally
//   not a general-purpose physics engine — correctness and simplicity are
//   favoured over feature completeness.
//
// Key invariants:
//   - Bodies are axis-aligned boxes (AABB) or circles. No rotational physics.
//   - Integration is symplectic Euler: forces → velocity, then velocity →
//     position, then collision resolution. Simple and stable for games.
//   - A body with mass == 0.0 is "static" (immovable). Its inv_mass is 0,
//     so impulse calculations produce zero delta-velocity for it.
//   - PH_MAX_BODIES, PH_MAX_JOINTS, and PH_MAX_CONTACTS are default reservations;
//     world-owned storage grows on demand.
//   - Collision filtering uses 64-bit layer/mask bitmasks: bodies A and B
//     collide only when (A.layer & B.mask) && (B.layer & A.mask) are both
//     non-zero (bidirectional filter).
//   - Broad-phase uses a stack-local adaptive uniform grid (8×8, 12×12, or
//     16×16) rebuilt each step. The grid arrays live on the stack, making
//     concurrent physics worlds safe.
//   - Broad-phase candidate pairs are collected into a growable scratch buffer,
//     sorted, and de-duplicated so each pair resolves at most once per step, even
//     when the two bodies share multiple grid cells.
//   - Positional correction uses the Baumgarte stabilisation technique with
//     a 0.01-world-unit slop and 40% correction factor to prevent sinking
//     while avoiding jitter.
//
// Ownership/Lifetime:
//   - World objects are GC-managed (rt_obj_new_i64). The world_finalizer
//     releases reference-counted bodies.
//   - Body objects are reference-counted: the world retains them on Add and
//     releases them on Remove or finalisation.
//   - Callers may release their own body reference after Add; the world keeps
//     the object alive. Remove detaches a body when it should leave simulation.
//
// Links: src/runtime/graphics/2d/rt_physics2d.h (public API), docs/zannalib/game.md (usage guide)
//
//===----------------------------------------------------------------------===//

#include "rt_physics2d.h"
#include "rt_physics2d_internal.h"
#include "rt_physics2d_joint.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @name Public-step timestep bounds
/// @{
#define PHYSICS2D_MAX_PUBLIC_STEP_DT 8.0
#define PHYSICS2D_MAX_SUBSTEP_DT 1.0
#define PHYSICS2D_MAX_SUBSTEPS 8

/// @}

//=============================================================================
// Internal types
//=============================================================================

// Internal types are in rt_physics2d_internal.h

/// @brief Compute a geometrically grown signed capacity without overflowing.
/// @details Starts from @p current when positive, otherwise
///          @p default_capacity, with an absolute minimum of one. Capacity
///          doubles until it reaches @p needed; when another doubling would
///          overflow, the exact needed value is selected instead.
/// @param current Current allocated capacity.
/// @param needed Nonnegative minimum capacity requested.
/// @param default_capacity Initial capacity used when @p current is nonpositive.
/// @param out Receives the selected capacity on success.
/// @return `1` on success, or `0` when @p out is null or @p needed is negative.
static int8_t grow_capacity_i64(int64_t current,
                                int64_t needed,
                                int64_t default_capacity,
                                int64_t *out) {
    if (!out || needed < 0)
        return 0;
    int64_t capacity = current > 0 ? current : default_capacity;
    if (capacity < 1)
        capacity = 1;
    while (capacity < needed) {
        if (capacity > INT64_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    *out = capacity;
    return 1;
}

/// @brief Ensure a world's retained-body array can hold @p needed entries.
/// @details Growth uses `PH_MAX_BODIES` as the initial reservation, rejects
///          byte-size overflow, and zero-initializes newly allocated slots.
/// @param w Mutable world implementation whose body array may be reallocated.
/// @param needed Nonnegative minimum number of body slots.
/// @return Nonzero when capacity is already sufficient or growth succeeds;
///         zero for invalid input, overflow, or allocation failure.
static int8_t ensure_body_capacity(rt_world_impl *w, int64_t needed) {
    if (!w || needed < 0)
        return 0;
    if (needed <= w->body_capacity)
        return 1;
    int64_t new_capacity = 0;
    if (!grow_capacity_i64(w->body_capacity, needed, PH_MAX_BODIES, &new_capacity) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(rt_body_impl *))
        return 0;
    rt_body_impl **bodies =
        (rt_body_impl **)realloc(w->bodies, (size_t)new_capacity * sizeof(rt_body_impl *));
    if (!bodies)
        return 0;
    memset(bodies + w->body_capacity,
           0,
           (size_t)(new_capacity - w->body_capacity) * sizeof(rt_body_impl *));
    w->bodies = bodies;
    w->body_capacity = new_capacity;
    return 1;
}

/// @brief Ensure a world's retained-joint array can hold @p needed entries.
/// @details Growth uses `PH_MAX_JOINTS` as the initial reservation, rejects
///          byte-size overflow, and zero-initializes newly allocated slots.
/// @param w Mutable world implementation whose joint array may be reallocated.
/// @param needed Nonnegative minimum number of joint slots.
/// @return Nonzero when capacity is already sufficient or growth succeeds;
///         zero for invalid input, overflow, or allocation failure.
int8_t rt_physics2d_world_reserve_joint_capacity(rt_world_impl *w, int64_t needed) {
    if (!w || needed < 0)
        return 0;
    if (needed <= w->joint_capacity)
        return 1;
    int64_t new_capacity = 0;
    if (!grow_capacity_i64(w->joint_capacity, needed, PH_MAX_JOINTS, &new_capacity) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(ph_joint *))
        return 0;
    ph_joint **joints = (ph_joint **)realloc(w->joints, (size_t)new_capacity * sizeof(ph_joint *));
    if (!joints)
        return 0;
    memset(joints + w->joint_capacity,
           0,
           (size_t)(new_capacity - w->joint_capacity) * sizeof(ph_joint *));
    w->joints = joints;
    w->joint_capacity = new_capacity;
    return 1;
}

/// @brief Ensure a world's contact-record array can hold @p needed entries.
/// @details Growth uses `PH_MAX_CONTACTS` as the initial reservation and
///          zero-initializes newly allocated records.
/// @param w Mutable world implementation whose contact array may be reallocated.
/// @param needed Nonnegative minimum number of contact slots.
/// @return Nonzero when capacity is already sufficient or growth succeeds;
///         zero for invalid input, overflow, or allocation failure.
static int8_t ensure_contact_capacity(rt_world_impl *w, int64_t needed) {
    if (!w || needed < 0)
        return 0;
    if (needed <= w->contact_capacity)
        return 1;
    int64_t new_capacity = 0;
    if (!grow_capacity_i64(w->contact_capacity, needed, PH_MAX_CONTACTS, &new_capacity) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(ph_contact_record))
        return 0;
    ph_contact_record *contacts =
        (ph_contact_record *)realloc(w->contacts, (size_t)new_capacity * sizeof(ph_contact_record));
    if (!contacts)
        return 0;
    memset(contacts + w->contact_capacity,
           0,
           (size_t)(new_capacity - w->contact_capacity) * sizeof(ph_contact_record));
    w->contacts = contacts;
    w->contact_capacity = new_capacity;
    return 1;
}

/// @brief Ensure all parallel per-step force-snapshot arrays have @p needed slots.
/// @details The body-pointer, X-force, and Y-force arrays grow to one common
///          capacity. Successfully reallocated earlier arrays remain installed
///          if a later parallel allocation fails, while `force_capacity` is
///          advanced only after all three succeed.
/// @param w Mutable world implementation whose snapshot arrays may grow.
/// @param needed Nonnegative minimum number of snapshot entries.
/// @return Nonzero when capacity is already sufficient or all growth succeeds;
///         zero for invalid input, overflow, or allocation failure.
static int8_t ensure_force_capacity(rt_world_impl *w, int64_t needed) {
    if (!w || needed < 0)
        return 0;
    if (needed <= w->force_capacity)
        return 1;
    int64_t new_capacity = 0;
    if (!grow_capacity_i64(w->force_capacity, needed, PH_MAX_BODIES, &new_capacity) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(rt_body_impl *) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(double))
        return 0;

    rt_body_impl **force_bodies =
        (rt_body_impl **)realloc(w->force_bodies, (size_t)new_capacity * sizeof(rt_body_impl *));
    if (!force_bodies)
        return 0;
    w->force_bodies = force_bodies;

    double *force_x = (double *)realloc(w->force_x, (size_t)new_capacity * sizeof(double));
    if (!force_x)
        return 0;
    w->force_x = force_x;

    double *force_y = (double *)realloc(w->force_y, (size_t)new_capacity * sizeof(double));
    if (!force_y)
        return 0;
    w->force_y = force_y;

    w->force_capacity = new_capacity;
    return 1;
}

/// @brief Ensure the broad-phase candidate-pair scratch can hold @p needed entries.
/// @details Grows geometrically so repeated appends across a step amortize to O(1).
///   Returns 0 on overflow/allocation failure, in which case the caller falls back
///   to the exhaustive O(n^2) pair pass so collision correctness is preserved.
/// @param w Mutable world implementation whose scratch buffer may grow.
/// @param needed Nonnegative minimum number of packed pair slots.
/// @return Nonzero when capacity is sufficient or growth succeeds; zero for
///         invalid input, overflow, or allocation failure.
static int8_t ensure_pair_scratch_capacity(rt_world_impl *w, int64_t needed) {
    if (!w || needed < 0)
        return 0;
    if (needed <= w->pair_scratch_capacity)
        return 1;
    int64_t new_capacity = 0;
    if (!grow_capacity_i64(w->pair_scratch_capacity, needed, 256, &new_capacity) ||
        (uint64_t)new_capacity > SIZE_MAX / sizeof(uint64_t))
        return 0;
    uint64_t *scratch =
        (uint64_t *)realloc(w->pair_scratch, (size_t)new_capacity * sizeof(uint64_t));
    if (!scratch)
        return 0;
    w->pair_scratch = scratch;
    w->pair_scratch_capacity = new_capacity;
    return 1;
}

/// @brief Append candidate pair (ii, jj) to the scratch list, ordered ii < jj.
/// @param w Mutable world implementation owning the scratch list.
/// @param ii First body-array index.
/// @param jj Second body-array index.
/// @return 1 on success, 0 if the scratch could not grow (caller must fall back).
static int8_t pair_scratch_push(rt_world_impl *w, int ii, int jj) {
    if (!w || ii < 0 || jj < 0)
        return 0;
    if (ii == jj)
        return 1;
    if (ii > jj) {
        int t = ii;
        ii = jj;
        jj = t;
    }
    if (w->pair_scratch_count < 0 || w->pair_scratch_count == INT64_MAX ||
        !ensure_pair_scratch_capacity(w, w->pair_scratch_count + 1))
        return 0;
    w->pair_scratch[w->pair_scratch_count++] =
        ((uint64_t)(uint32_t)ii << 32) | (uint64_t)(uint32_t)jj;
    return 1;
}

/// @brief qsort comparator over packed pair keys (ascending). Total order on
///        distinct (ii, jj) pairs makes the resolution sweep deterministic.
/// @param a Pointer to the first packed `uint64_t` pair key.
/// @param b Pointer to the second packed `uint64_t` pair key.
/// @return A negative, zero, or positive value when @p a sorts before, equal to,
///         or after @p b.
static int pair_key_cmp(const void *a, const void *b) {
    uint64_t ka = *(const uint64_t *)a;
    uint64_t kb = *(const uint64_t *)b;
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

/// @brief Clear the world's per-step contact list (called at the start of
///        each physics step before broad/narrow-phase regenerates contacts).
/// @details Releases both body references held by every initialized record,
///          zeroes those records, and resets the count and overflow flag.
/// @param w Mutable world implementation; `NULL` is accepted as a no-op.
static void world_clear_contacts(rt_world_impl *w) {
    if (!w)
        return;
    for (int64_t i = 0; i < w->contact_count; ++i) {
        if (w->contacts[i].body_a && rt_obj_release_check0(w->contacts[i].body_a))
            rt_obj_free(w->contacts[i].body_a);
        if (w->contacts[i].body_b && rt_obj_release_check0(w->contacts[i].body_b))
            rt_obj_free(w->contacts[i].body_b);
        memset(&w->contacts[i], 0, sizeof(w->contacts[i]));
    }
    w->contact_count = 0;
    w->contact_overflow = 0;
}

/// @brief Append a contact record to the world's per-step contact list.
/// @details Records an overflow flag when the list cannot grow, and skips non-finite
///   manifold values so downstream queries always see a clean list even in degenerate numerical
///   situations. Penetration is clamped to [0, +inf) because negative depth would indicate
///   separation, not contact.
/// @param w Mutable world receiving the contact.
/// @param a First body, retained by the contact record.
/// @param b Second body, retained by the contact record.
/// @param nx Finite contact-normal X component pointing from @p a toward @p b.
/// @param ny Finite contact-normal Y component pointing from @p a toward @p b.
/// @param pen Penetration depth, clamped to a nonnegative value.
void world_record_contact(
    rt_world_impl *w, rt_body_impl *a, rt_body_impl *b, double nx, double ny, double pen) {
    if (!w || !a || !b)
        return;
    if (!isfinite(nx) || !isfinite(ny) || !isfinite(pen))
        return;
    if (w->contact_count < 0 || w->contact_count == INT64_MAX ||
        !ensure_contact_capacity(w, w->contact_count + 1)) {
        w->contact_overflow = 1;
        return;
    }
    int64_t idx = w->contact_count++;
    rt_obj_retain_maybe(a);
    rt_obj_retain_maybe(b);
    w->contacts[idx].body_a = a;
    w->contacts[idx].body_b = b;
    w->contacts[idx].nx = nx;
    w->contacts[idx].ny = ny;
    w->contacts[idx].penetration = pen > 0.0 ? pen : 0.0;
}

/// @brief Return `value` if finite, otherwise `fallback`. Used for gravity and position setters.
/// @param value Candidate floating-point value.
/// @param fallback Replacement returned for NaN or infinity.
/// @return @p value when finite, otherwise @p fallback.
static double finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Return `value` if finite and strictly positive, otherwise `fallback`.
/// @details Used for body dimensions (width, height) and mass to guarantee they
///   are always valid physics inputs — a zero or NaN dimension would make AABB
///   overlap tests degenerate.
/// @param value Candidate floating-point value.
/// @param fallback Replacement for non-finite or nonpositive input.
/// @return @p value when finite and positive, otherwise @p fallback.
static double positive_or(double value, double fallback) {
    return (isfinite(value) && value > 0.0) ? value : fallback;
}

/// @brief Clamp `value` to [0, 1], returning 0 for NaN/Inf. Used for restitution and friction.
/// @param value Coefficient to sanitize.
/// @return @p value clamped to `[0, 1]`, or `0` for non-finite input.
static double clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Clamp `value` to [-limit, +limit], returning `fallback` for NaN/Inf.
/// @details Used by sanitize_body_state to cap position, velocity, and force magnitudes
///   to large but representable values, preventing IEEE infinity from propagating through
///   the integrator and corrupting the broad-phase grid bounds.
/// @param value Candidate signed quantity.
/// @param fallback Replacement for non-finite input.
/// @param limit Positive magnitude limit.
/// @return A finite value in `[-limit, +limit]`, using @p fallback for
///         non-finite input.
static double clamp_abs_finite(double value, double fallback, double limit) {
    if (!isfinite(value))
        return fallback;
    if (value > limit)
        return limit;
    if (value < -limit)
        return -limit;
    return value;
}

/// @brief Downcast a raw handle to rt_world_impl* after confirming its class ID.
/// @details Calls rt_physics2d_is_world_handle to verify the GC class-ID tag before
///   casting, trapping with `api` as the message on mismatch.  NULL input short-
///   circuits immediately without a trap so callers can chain checked_world checks
///   with early NULL guards.
/// @param obj Opaque world candidate.
/// @param api Trap message used for a non-null class mismatch.
/// @return The validated world implementation, or `NULL` for null or mistyped
///         input.
static rt_world_impl *checked_world(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_physics2d_is_world_handle(obj)) {
        rt_trap(api);
        return NULL;
    }
    return (rt_world_impl *)obj;
}

/// @brief Downcast a raw handle to rt_body_impl* after confirming its class ID.
/// @details Mirror of checked_world — verifies the GC class-ID is the physics-body
///   sentinel before casting, trapping with `api` on mismatch.
/// @param obj Opaque body candidate.
/// @param api Trap message used for a non-null class mismatch.
/// @return The validated body implementation, or `NULL` for null or mistyped
///         input.
static rt_body_impl *checked_body(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_physics2d_is_body_handle(obj)) {
        rt_trap(api);
        return NULL;
    }
    return (rt_body_impl *)obj;
}

/// @brief Clamp all body fields to safe, finite ranges and fix internal consistency.
/// @details Called after every integration step and after each pair resolution to
///   ensure NaN/Inf values and wildly out-of-range quantities from user code cannot
///   propagate.  Enforces: positions clamped to ±1e12, dimensions in (0, 1e9],
///   velocities/forces in ±1e9/±1e12, mass/inv_mass consistent (static bodies keep
///   both at 0), restitution/friction in [0,1]. Circle bodies preserve zero box
///   dimensions and receive a fallback radius of 1.0 when necessary; box bodies
///   have radius forced to 0.
/// @param b Mutable body implementation to sanitize; `NULL` is accepted as a
///          no-op.
void sanitize_body_state(rt_body_impl *b) {
    if (!b)
        return;

    const double max_pos = 1.0e12;
    const double max_size = 1.0e9;
    const double max_vel = 1.0e9;
    const double max_force = 1.0e12;

    double fallback_x = isfinite(b->prev_x) ? b->prev_x : 0.0;
    double fallback_y = isfinite(b->prev_y) ? b->prev_y : 0.0;
    b->x = clamp_abs_finite(b->x, fallback_x, max_pos);
    b->y = clamp_abs_finite(b->y, fallback_y, max_pos);
    b->prev_x = clamp_abs_finite(b->prev_x, b->x, max_pos);
    b->prev_y = clamp_abs_finite(b->prev_y, b->y, max_pos);
    b->is_circle = b->is_circle ? 1 : 0;
    if (b->is_circle) {
        b->w = 0.0;
        b->h = 0.0;
        b->radius = (isfinite(b->radius) && b->radius > 0.0)
                        ? (b->radius > max_size ? max_size : b->radius)
                        : 1.0;
    } else {
        b->w = (isfinite(b->w) && b->w > 0.0) ? (b->w > max_size ? max_size : b->w) : 1.0;
        b->h = (isfinite(b->h) && b->h > 0.0) ? (b->h > max_size ? max_size : b->h) : 1.0;
        b->radius = 0.0;
    }
    b->vx = clamp_abs_finite(b->vx, 0.0, max_vel);
    b->vy = clamp_abs_finite(b->vy, 0.0, max_vel);
    b->fx = clamp_abs_finite(b->fx, 0.0, max_force);
    b->fy = clamp_abs_finite(b->fy, 0.0, max_force);
    b->mass = rt_physics2d_normalize_mass(b->mass);
    b->inv_mass = b->mass > 0.0 ? 1.0 / b->mass : 0.0;
    b->restitution = clamp01(b->restitution);
    b->friction = clamp01(b->friction);
}

/* AABB edge accessors. The world uses positive-y-downward screen coordinates, so
 * the stored (x, y) is the min corner (top-left): min_y is the top edge, max_y the
 * bottom edge. */

/// @brief Left edge (min x) of this body's current AABB (circle or box).
/// @param b Borrowed sanitized body implementation.
/// @return The circle's center X minus radius or the box's top-left X.
static double body_min_x(rt_body_impl *b) {
    return b->is_circle ? b->x - b->radius : b->x;
}

/// @brief Top edge (min y) of this body's current AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The circle's center Y minus radius or the box's top-left Y.
static double body_min_y(rt_body_impl *b) {
    return b->is_circle ? b->y - b->radius : b->y;
}

/// @brief Right edge (max x) of this body's current AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The circle's center X plus radius or the box's X plus width.
static double body_max_x(rt_body_impl *b) {
    return b->is_circle ? b->x + b->radius : b->x + b->w;
}

/// @brief Bottom edge (max y) of this body's current AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The circle's center Y plus radius or the box's Y plus height.
static double body_max_y(rt_body_impl *b) {
    return b->is_circle ? b->y + b->radius : b->y + b->h;
}

/// @brief Left edge (min x) of this body's previous-frame AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The previous circle center X minus radius or previous box X.
double body_prev_min_x(rt_body_impl *b) {
    return b->is_circle ? b->prev_x - b->radius : b->prev_x;
}

/// @brief Top edge (min y) of this body's previous-frame AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The previous circle center Y minus radius or previous box Y.
double body_prev_min_y(rt_body_impl *b) {
    return b->is_circle ? b->prev_y - b->radius : b->prev_y;
}

/// @brief Right edge (max x) of this body's previous-frame AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The previous circle center X plus radius or previous box X plus width.
double body_prev_max_x(rt_body_impl *b) {
    return b->is_circle ? b->prev_x + b->radius : b->prev_x + b->w;
}

/// @brief Bottom edge (max y) of this body's previous-frame AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The previous circle center Y plus radius or previous box Y plus height.
double body_prev_max_y(rt_body_impl *b) {
    return b->is_circle ? b->prev_y + b->radius : b->prev_y + b->h;
}

/// @brief Minimum X of the union AABB spanning both previous and current positions.
/// @details The swept bound is used by the broad-phase grid to catch fast-moving bodies
///   that cross a grid cell boundary within a single time step.
/// @param b Borrowed sanitized body implementation.
/// @return The lesser of the current and previous minimum X edges.
static double body_swept_min_x(rt_body_impl *b) {
    double now = body_min_x(b);
    double prev = body_prev_min_x(b);
    return now < prev ? now : prev;
}

/// @brief Minimum Y of the swept union AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The lesser of the current and previous minimum Y edges.
static double body_swept_min_y(rt_body_impl *b) {
    double now = body_min_y(b);
    double prev = body_prev_min_y(b);
    return now < prev ? now : prev;
}

/// @brief Maximum X of the swept union AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The greater of the current and previous maximum X edges.
static double body_swept_max_x(rt_body_impl *b) {
    double now = body_max_x(b);
    double prev = body_prev_max_x(b);
    return now > prev ? now : prev;
}

/// @brief Maximum Y of the swept union AABB.
/// @param b Borrowed sanitized body implementation.
/// @return The greater of the current and previous maximum Y edges.
static double body_swept_max_y(rt_body_impl *b) {
    double now = body_max_y(b);
    double prev = body_prev_max_y(b);
    return now > prev ? now : prev;
}

/// @brief Remove and release the joint at `joint_index` in the world's joint array.
/// @details Marks the joint inactive before releasing so any in-flight solver callbacks
///   that still hold a pointer see it as dead.  Uses swap-with-tail compaction to keep
///   the array packed without shifting.
/// @param w Mutable world implementation owning the joint reference.
/// @param joint_index Zero-based joint-array index; invalid indices are ignored.
static void world_release_joint_at(rt_world_impl *w, int64_t joint_index) {
    if (!w || joint_index < 0 || joint_index >= w->joint_count)
        return;

    ph_joint *joint = w->joints[joint_index];
    if (joint)
        joint->active = 0;
    if (joint && rt_obj_release_check0(joint))
        rt_obj_free(joint);

    w->joint_count--;
    w->joints[joint_index] = w->joints[w->joint_count];
    w->joints[w->joint_count] = NULL;
}

/// @brief Remove and release every joint that references `body` as either endpoint.
/// @details Called just before a body is removed from the world to prevent dangling
///   body pointers inside live joint objects.  Iterates in place using an index
///   loop that does not advance when world_release_joint_at swaps the tail item
///   into the current slot.
/// @param w Mutable world implementation whose joints are inspected.
/// @param body Borrowed body endpoint being detached.
static void world_remove_joints_for_body(rt_world_impl *w, rt_body_impl *body) {
    if (!w || !body)
        return;

    for (int64_t i = 0; i < w->joint_count;) {
        ph_joint *joint = w->joints[i];
        if (joint && (joint->body_a == body || joint->body_b == body)) {
            world_release_joint_at(w, i);
            continue;
        }
        i++;
    }
}

/// @brief GC finalizer for a physics world.
/// @details Runs when the world's reference count reaches zero. Releases every
///   joint (marking each inactive first), clears and releases the per-step contact
///   list, then releases each retained body — detaching it from this world so a
///   body still referenced elsewhere can be re-added to another world. Finally
///   frees all world-owned growable arrays (bodies, joints, contacts, pair scratch,
///   force snapshot) and zeroes their sizes. Order (joints → contacts → bodies)
///   matters: joints and contacts hold body references that must be released first.
/// @param obj Finalizing world implementation supplied by the object system;
///            `NULL` is accepted.
static void world_finalizer(void *obj) {
    rt_world_impl *w = (rt_world_impl *)obj;
    if (w) {
        while (w->joint_count > 0)
            world_release_joint_at(w, w->joint_count - 1);

        world_clear_contacts(w);

        int64_t i;
        for (i = 0; i < w->body_count; i++) {
            if (!w->bodies[i])
                continue;
            /* Detach the body from this world so a body that outlives the world
             * (still referenced elsewhere) can be re-added to another world. */
            if (w->bodies[i]->owner_world == w) {
                w->bodies[i]->owner_world = NULL;
                w->bodies[i]->world_index = -1;
            }
            if (rt_obj_release_check0(w->bodies[i]))
                rt_obj_free(w->bodies[i]);
        }
        w->body_count = 0;
        free(w->bodies);
        w->bodies = NULL;
        w->body_capacity = 0;
        free(w->joints);
        w->joints = NULL;
        w->joint_capacity = 0;
        free(w->contacts);
        w->contacts = NULL;
        w->contact_capacity = 0;
        free(w->pair_scratch);
        w->pair_scratch = NULL;
        w->pair_scratch_count = 0;
        w->pair_scratch_capacity = 0;
        free(w->force_bodies);
        w->force_bodies = NULL;
        free(w->force_x);
        w->force_x = NULL;
        free(w->force_y);
        w->force_y = NULL;
        w->force_capacity = 0;
    }
}

//=============================================================================
// Public API — World
//=============================================================================

/// @brief Allocate a new physics world with the given constant gravity vector.
/// @details Initialises all body/joint/contact slots to zero and registers a GC
///   finalizer that will release retained body references when the world is collected.
/// @param gravity_x World-space X acceleration (e.g. 0 for horizontal, ±g for side-scrollers).
/// @param gravity_y World-space Y acceleration (positive = downward in screen coords).
/// @return Opaque world handle, or NULL on allocation failure (after trapping).
void *rt_physics2d_world_new(double gravity_x, double gravity_y) {
    rt_world_impl *w = (rt_world_impl *)rt_obj_new_i64(RT_PHYSICS2D_WORLD_CLASS_ID,
                                                       (int64_t)sizeof(rt_world_impl));
    if (!w) {
        rt_trap("Physics2D.World: allocation failed");
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->vptr = NULL;
    w->gravity_x = finite_or(gravity_x, 0.0);
    w->gravity_y = finite_or(gravity_y, 0.0);
    w->body_count = 0;
    w->joint_count = 0;
    w->contact_count = 0;
    w->contact_overflow = 0;
    rt_obj_set_finalizer(w, world_finalizer);
    if (!ensure_body_capacity(w, PH_MAX_BODIES) ||
        !rt_physics2d_world_reserve_joint_capacity(w, PH_MAX_JOINTS) ||
        !ensure_contact_capacity(w, PH_MAX_CONTACTS)) {
        rt_trap("Physics2D.World: allocation failed");
        if (rt_obj_release_check0(w))
            rt_obj_free(w);
        return NULL;
    }
    return w;
}

/// @brief Advance an already-validated world by one bounded integration step.
/// @details Stages: apply gravity/forces, integrate velocity to position,
///          solve joints, then perform broad/narrow-phase collision detection.
///          The public step function calls this one or more times for large dt.
/// @param obj Public world handle, passed through to joint solvers.
/// @param w Checked world implementation pointer.
/// @param dt Positive finite substep duration.
static void physics2d_world_step_once(void *obj, rt_world_impl *w, double dt) {
    int64_t i;
    if (!w)
        return;
    /* Contacts are cleared once per public step in rt_physics2d_world_step, not
     * per substep — otherwise a multi-substep step (large/hitch dt) would leave
     * only the final substep's contacts queryable, silently dropping collision
     * events (damage/sound triggers) that occurred in earlier substeps. */

    for (i = 0; i < w->body_count; i++) {
        rt_body_impl *b = w->bodies[i];
        if (!b)
            continue;
        sanitize_body_state(b);
        b->prev_x = b->x;
        b->prev_y = b->y;
    }

    /* Step 1: Apply accumulated forces and gravity to each dynamic body's
     * velocity (symplectic Euler, force→velocity half-step).
     * Forces are cleared here so Apply Force calls accumulate cleanly across
     * multiple Step() calls within the same frame if the caller uses sub-steps. */
    for (i = 0; i < w->body_count; i++) {
        rt_body_impl *b = w->bodies[i];
        if (!b)
            continue;
        if (b->inv_mass == 0.0) {
            b->fx = 0.0;
            b->fy = 0.0;
            continue; /* Skip static bodies */
        }
        double ax = rt_physics2d_saturating_add(rt_physics2d_saturating_mul(b->fx, b->inv_mass),
                                                w->gravity_x);
        double ay = rt_physics2d_saturating_add(rt_physics2d_saturating_mul(b->fy, b->inv_mass),
                                                w->gravity_y);
        b->vx = rt_physics2d_saturating_add(b->vx, rt_physics2d_saturating_mul(ax, dt));
        b->vy = rt_physics2d_saturating_add(b->vy, rt_physics2d_saturating_mul(ay, dt));
        b->fx = 0.0;
        b->fy = 0.0;
        sanitize_body_state(b);
    }

    if (w->joint_count > 0) {
        /* Spring joints read dynamic-body velocities, which were already sanitised in
         * the force-integration loop above (sanitize_body_state per dynamic body), so
         * the solve sees finite, clamped inputs. The pass below then re-sanitises every
         * body so spring-applied impulses cannot leave NaN/over-speed velocity for the
         * position-integration step. */
        rt_physics2d_solve_spring_joints(obj, dt);
        for (i = 0; i < w->body_count; i++)
            sanitize_body_state(w->bodies[i]);
    }

    /* Step 2: Integrate velocity → position for each dynamic body.
     * Done in a separate pass from Step 1 so all velocity changes from forces
     * and springs are committed before any position updates occur. */
    for (i = 0; i < w->body_count; i++) {
        rt_body_impl *b = w->bodies[i];
        if (!b || b->inv_mass == 0.0)
            continue;
        b->x = rt_physics2d_saturating_add(b->x, rt_physics2d_saturating_mul(b->vx, dt));
        b->y = rt_physics2d_saturating_add(b->y, rt_physics2d_saturating_mul(b->vy, dt));
        sanitize_body_state(b);
    }

    /* Step 2.5: Solve joint constraints (iterative relaxation).
     * Joints are solved after velocity integration but before collision
     * detection so that constrained bodies are in valid positions before
     * the broad/narrow phase runs. */
    if (w->joint_count > 0) {
        rt_physics2d_solve_position_joints(obj, dt);
        rt_physics2d_solve_joint_velocities(obj, dt);
        for (i = 0; i < w->body_count; i++)
            sanitize_body_state(w->bodies[i]);
    }

    /* Step 3: Broad-phase + narrow-phase collision detection and resolution.
     *
     * Broad phase: bounded adaptive uniform grid. The grid is recomputed from
     * scratch each step. The world swept AABB is computed first, then divided into
     * grid_dim×grid_dim cells. Each body is registered in every cell its swept
     * bounds overlap.
     *
     * All grid arrays are stack-local, making this function safe to call on
     * concurrent worlds from separate threads with no data sharing.
     *
     * The grid stores body indices (not pointers) to keep each cell small.
     * BPG_CELL_MAX caps the count per cell; if a cell
     * overflows, the step falls back to an exhaustive O(n²) pair pass so
     * collision correctness is preserved in dense scenes.
     *
     * Narrow phase: candidate pairs are collected into w->pair_scratch, sorted,
     * and de-duplicated, then each unique pair is tested with shape_overlap() or
     * swept AABB and resolved if it collides. This replaces the former O(n^2)
     * bit-matrix (and its per-substep O(n^2) memset) with O(pairs) memory. */

#define BPG_BASE_DIM 8  /* Small-world broad-phase grid cells per axis. */
#define BPG_MAX_DIM 16  /* Largest stack-backed grid cells per axis. */
#define BPG_CELL_MAX 32 /* Maximum body indices stored per grid cell. */

    if (w->body_count >= 2) {
        int grid_dim = BPG_BASE_DIM;
        if (w->body_count > 128)
            grid_dim = BPG_MAX_DIM;
        else if (w->body_count > 64)
            grid_dim = 12;
        /* --- Step 3a: Compute the world swept AABB that encloses all bodies --- */
        double wx0 = 1e18, wy0 = 1e18, wx1 = -1e18, wy1 = -1e18;
        for (i = 0; i < w->body_count; i++) {
            rt_body_impl *b = w->bodies[i];
            if (!b)
                continue;
            double bx0 = body_swept_min_x(b);
            double by0 = body_swept_min_y(b);
            double bx1 = body_swept_max_x(b);
            double by1 = body_swept_max_y(b);
            if (bx0 < wx0)
                wx0 = bx0;
            if (by0 < wy0)
                wy0 = by0;
            if (bx1 > wx1)
                wx1 = bx1;
            if (by1 > wy1)
                wy1 = by1;
        }
        /* Guard: ensure minimum cell size of 1 so division below never divides
         * by zero (can happen when all bodies occupy the exact same point). */
        if (wx1 <= wx0)
            wx1 = wx0 + 1.0;
        if (wy1 <= wy0)
            wy1 = wy0 + 1.0;
        double cell_w = (wx1 - wx0) / (double)grid_dim;
        double cell_h = (wy1 - wy0) / (double)grid_dim;

        /* A body whose swept AABB spans at least half the world on either axis is
         * "large": a ground/wall platform, or a stray far-away body that would
         * otherwise stretch every cell and collapse the scene into one. Large
         * bodies are kept out of the grid and paired against all bodies, so one
         * oversized body no longer degrades the whole broad-phase to O(n^2). */
        double large_w = 0.5 * (wx1 - wx0);
        double large_h = 0.5 * (wy1 - wy0);

        /* --- Step 3b: Populate the broad-phase grid (stack-local) and collect
         * candidate pairs into w->pair_scratch. Large bodies (and bodies spilled
         * from an overflowing cell) are paired against every other body directly;
         * normal bodies are gridded and paired with their cell neighbours. Pairs
         * are de-duplicated later by sorting, so the overlap between against-all
         * and grid pairs is harmless. */
        int32_t grid_bodies[BPG_MAX_DIM * BPG_MAX_DIM][BPG_CELL_MAX];
        int grid_count[BPG_MAX_DIM * BPG_MAX_DIM];
        int use_exhaustive = 0;
        memset(grid_count, 0, sizeof(grid_count));
        w->pair_scratch_count = 0;

        for (i = 0; i < w->body_count && !use_exhaustive; i++) {
            rt_body_impl *b = w->bodies[i];
            if (!b)
                continue;
            double bx0 = body_swept_min_x(b);
            double by0 = body_swept_min_y(b);
            double bx1 = body_swept_max_x(b);
            double by1 = body_swept_max_y(b);

            int against_all = (bx1 - bx0) >= large_w || (by1 - by0) >= large_h;
            if (against_all) {
                for (int64_t j = 0; j < w->body_count; j++) {
                    if (j == i || !w->bodies[j])
                        continue;
                    if (!pair_scratch_push(w, (int)i, (int)j)) {
                        use_exhaustive = 1;
                        break;
                    }
                }
                continue;
            }

            int cx0 = (int)((bx0 - wx0) / cell_w);
            if (cx0 < 0)
                cx0 = 0;
            if (cx0 >= grid_dim)
                cx0 = grid_dim - 1;
            int cy0 = (int)((by0 - wy0) / cell_h);
            if (cy0 < 0)
                cy0 = 0;
            if (cy0 >= grid_dim)
                cy0 = grid_dim - 1;
            int cx1 = (int)((bx1 - wx0) / cell_w);
            if (cx1 < 0)
                cx1 = 0;
            if (cx1 >= grid_dim)
                cx1 = grid_dim - 1;
            int cy1 = (int)((by1 - wy0) / cell_h);
            if (cy1 < 0)
                cy1 = 0;
            if (cy1 >= grid_dim)
                cy1 = grid_dim - 1;

            int spilled = 0;
            for (int cy = cy0; cy <= cy1 && !spilled; cy++) {
                for (int cx = cx0; cx <= cx1; cx++) {
                    int cell = cy * grid_dim + cx;
                    int cnt = grid_count[cell];
                    /* Pair this body with everyone already registered in the cell. */
                    for (int k = 0; k < cnt; k++) {
                        if (!pair_scratch_push(w, (int)i, (int)grid_bodies[cell][k])) {
                            use_exhaustive = 1;
                            break;
                        }
                    }
                    if (use_exhaustive)
                        break;
                    if (cnt < BPG_CELL_MAX) {
                        grid_bodies[cell][cnt] = (int32_t)i;
                        grid_count[cell] = cnt + 1;
                    } else {
                        /* Cell full: spill this body to the against-all set rather
                         * than forcing a whole-world exhaustive pass. */
                        for (int64_t j = 0; j < w->body_count; j++) {
                            if (j == i || !w->bodies[j])
                                continue;
                            if (!pair_scratch_push(w, (int)i, (int)j)) {
                                use_exhaustive = 1;
                                break;
                            }
                        }
                        spilled = 1;
                        break;
                    }
                }
                if (use_exhaustive)
                    break;
            }
        }

        if (use_exhaustive) {
            /* Scratch could not grow — preserve correctness with the exhaustive
             * O(n^2) pair pass. */
            for (int ii = 0; ii < w->body_count; ii++) {
                for (int jj = ii + 1; jj < w->body_count; jj++)
                    maybe_resolve_pair(w, ii, jj, dt);
            }
        } else if (w->pair_scratch_count > 0) {
            /* --- Step 3c: Sort collected pairs and resolve each unique pair once.
             * Sorting by packed (ii, jj) key gives a deterministic sweep order so
             * VM and native runs agree. */
            qsort(w->pair_scratch, (size_t)w->pair_scratch_count, sizeof(uint64_t), pair_key_cmp);
            uint64_t prev_key = ~(uint64_t)0;
            for (int64_t p = 0; p < w->pair_scratch_count; p++) {
                uint64_t key = w->pair_scratch[p];
                if (p > 0 && key == prev_key)
                    continue;
                prev_key = key;
                int ii = (int)(uint32_t)(key >> 32);
                int jj = (int)(uint32_t)(key & 0xFFFFFFFFu);
                maybe_resolve_pair(w, ii, jj, dt);
            }
        }
    }

#undef BPG_BASE_DIM
#undef BPG_MAX_DIM
#undef BPG_CELL_MAX
}

/// @brief Advance the physics world by `dt` seconds.
/// @details Non-finite and non-positive values only clear stale contacts. Step
///          calls up to one second preserve the historical single-step
///          semi-implicit Euler behavior. Larger hitch values are capped and
///          split into bounded substeps so joints and collision detection never
///          receive an unbounded timestep after a pause.
/// @param obj Opaque Physics2D.World handle to advance; `NULL` is ignored and a
///            non-null class mismatch traps.
/// @param dt Requested elapsed simulation time in seconds.
void rt_physics2d_world_step(void *obj, double dt) {
    if (!obj)
        return;
    rt_world_impl *w = checked_world(obj, "Physics2D.World.Step: expected Physics2D.World");
    if (!w)
        return;
    world_clear_contacts(w);
    if (dt <= 0.0 || !isfinite(dt))
        return;
    if (dt > PHYSICS2D_MAX_PUBLIC_STEP_DT)
        dt = PHYSICS2D_MAX_PUBLIC_STEP_DT;

    int substeps = (int)(dt / PHYSICS2D_MAX_SUBSTEP_DT);
    if ((double)substeps * PHYSICS2D_MAX_SUBSTEP_DT < dt)
        substeps++;
    if (substeps < 1)
        substeps = 1;
    if (substeps > PHYSICS2D_MAX_SUBSTEPS)
        substeps = PHYSICS2D_MAX_SUBSTEPS;
    double sub_dt = dt / (double)substeps;

    int64_t force_count = w->body_count;
    if (!ensure_force_capacity(w, force_count)) {
        rt_trap("Physics2D.World.Step: force snapshot allocation failed");
        return;
    }
    for (int64_t i = 0; i < force_count; ++i) {
        w->force_bodies[i] = w->bodies[i];
        w->force_x[i] = w->bodies[i] ? w->bodies[i]->fx : 0.0;
        w->force_y[i] = w->bodies[i] ? w->bodies[i]->fy : 0.0;
    }

    for (int step = 0; step < substeps; ++step) {
        for (int64_t i = 0; i < force_count; ++i) {
            if (w->force_bodies[i]) {
                w->force_bodies[i]->fx = w->force_x[i];
                w->force_bodies[i]->fy = w->force_y[i];
            }
        }
        physics2d_world_step_once(obj, w, sub_dt);
    }
}

/// @brief Insert a body into the world's simulation list. The world retains the body; remove
/// later via `_remove`. Body storage grows from the PH_MAX_BODIES initial reservation.
/// @details Adding the same body to the same world again is a no-op. A body is
///          intended to belong to at most one world; callers must remove it
///          from any previous world before adding it elsewhere. Invalid
///          non-null classes, solver-index overflow, and storage-allocation
///          failure raise runtime traps.
/// @param obj Opaque destination Physics2D.World handle.
/// @param body Opaque Physics2D.Body handle retained on successful insertion.
void rt_physics2d_world_add(void *obj, void *body) {
    rt_world_impl *w;
    if (!obj || !body)
        return;
    w = checked_world(obj, "Physics2D.World.Add: expected Physics2D.World");
    if (!w)
        return;
    if (!rt_physics2d_is_body_handle(body)) {
        rt_trap("Physics2D.World.Add: expected Physics2D.Body");
        return;
    }
    rt_body_impl *bd = (rt_body_impl *)body;
    sanitize_body_state(bd);
    /* Duplicate detection is O(1): a body tracks its owning world. Registering
     * the same body in two worlds would make both worlds retain, integrate, and
     * eventually detach the same mutable payload, so callers must remove it from
     * its current world before adding it elsewhere. */
    if (bd->owner_world == w)
        return;
    if (bd->owner_world != NULL) {
        rt_trap("Physics2D.World.Add: body already belongs to another world");
        return;
    }
    if (w->body_count >= INT_MAX) {
        rt_trap("Physics2D.World.Add: body count exceeds solver index range");
        return;
    }
    if (!ensure_body_capacity(w, w->body_count + 1)) {
        rt_trap("Physics2D.World.Add: body storage allocation failed");
        return;
    }
    rt_obj_retain_maybe(body);
    bd->owner_world = w;
    bd->world_index = w->body_count;
    w->bodies[w->body_count++] = bd;
}

/// @brief Detach and release a body retained by the world.
/// @details Uses the body's recorded owner/index for the common O(1) path and
///          falls back to a linear search when that metadata is stale. All
///          joints referencing the body and all current contacts are released
///          first. Body ordering is not preserved because removal swaps in the
///          tail entry and updates its index.
/// @param obj Opaque source Physics2D.World handle.
/// @param body Opaque Physics2D.Body handle to remove; absence from the world
///             is a no-op.
void rt_physics2d_world_remove(void *obj, void *body) {
    rt_world_impl *w;
    int64_t i;
    if (!obj || !body)
        return;
    w = checked_world(obj, "Physics2D.World.Remove: expected Physics2D.World");
    if (!w)
        return;
    if (!rt_physics2d_is_body_handle(body)) {
        rt_trap("Physics2D.World.Remove: expected Physics2D.Body");
        return;
    }
    rt_body_impl *bd = (rt_body_impl *)body;
    /* Fast path: the body records its own index, so removal is O(1). Validate the
     * index against the live array before trusting it (guards against a stale index
     * if the body was juggled between worlds), falling back to a linear scan. */
    i = -1;
    if (bd->owner_world == w && bd->world_index >= 0 && bd->world_index < w->body_count &&
        w->bodies[bd->world_index] == bd) {
        i = bd->world_index;
    } else {
        for (int64_t k = 0; k < w->body_count; k++) {
            if (w->bodies[k] == bd) {
                i = k;
                break;
            }
        }
    }
    if (i < 0)
        return;

    world_remove_joints_for_body(w, bd);
    world_clear_contacts(w);
    bd->owner_world = NULL;
    bd->world_index = -1;
    if (rt_obj_release_check0(bd))
        rt_obj_free(bd);
    /* Swap with tail to maintain a compact, order-independent array, and update the
     * moved body's recorded index so its own O(1) removal stays correct. */
    int64_t last = w->body_count - 1;
    w->bodies[i] = w->bodies[last];
    w->bodies[last] = NULL;
    w->body_count--;
    if (w->bodies[i])
        w->bodies[i]->world_index = i;
}

/// @brief Number of bodies currently registered with the world.
/// @param obj Opaque Physics2D.World handle to query.
/// @return The retained body count, or `0` for `NULL` or a mistyped handle.
int64_t rt_physics2d_world_body_count(void *obj) {
    if (!obj)
        return 0;
    rt_world_impl *w = checked_world(obj, "Physics2D.World.BodyCount: expected Physics2D.World");
    return w ? w->body_count : 0;
}

/// @brief Set world gravity in world-units per second² (typical: gx=0, gy=9.8 for downward grav).
/// @details Each non-finite component is independently replaced with zero.
/// @param obj Opaque Physics2D.World handle to update.
/// @param gx Horizontal acceleration.
/// @param gy Vertical acceleration in positive-Y-down coordinates.
void rt_physics2d_world_set_gravity(void *obj, double gx, double gy) {
    if (!obj)
        return;
    rt_world_impl *w = checked_world(obj, "Physics2D.World.SetGravity: expected Physics2D.World");
    if (!w)
        return;
    w->gravity_x = finite_or(gx, 0.0);
    w->gravity_y = finite_or(gy, 0.0);
}

/// @brief Number of contact pairs resolved during the most recent world step.
/// @details The list is rebuilt fresh on every call to rt_physics2d_world_step and
///   stored in a growable list. If the list cannot grow,
///   `rt_physics2d_world_contact_overflowed` reports that additional contacts were omitted.
///   Query it between steps to drive game logic (e.g. damage on collision, sound effects).
/// @param obj Opaque Physics2D.World handle to query.
/// @return The number of retained records from the latest public step, or `0`
///         for `NULL` or a mistyped handle.
int64_t rt_physics2d_world_contact_count(void *obj) {
    if (!obj)
        return 0;
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactCount: expected Physics2D.World");
    return w ? w->contact_count : 0;
}

/// @brief Return whether contacts were omitted from the most recent world step.
/// @details Contact storage starts with PH_MAX_CONTACTS slots and grows on demand. This flag is
///          cleared when contacts are cleared and set the first time a valid contact cannot be
///          appended because allocation failed.
/// @param obj Physics2D world handle.
/// @return 1 if contact storage could not grow during the most recent step, otherwise 0.
int8_t rt_physics2d_world_contact_overflowed(void *obj) {
    if (!obj)
        return 0;
    rt_world_impl *w =
        checked_world(obj, "Physics2D.World.ContactOverflowed: expected Physics2D.World");
    return w && w->contact_overflow ? 1 : 0;
}

/// @brief Guard for all contact-list accessors — returns 1 only when `index` is in range.
/// @param w Borrowed validated world implementation, possibly `NULL`.
/// @param index Candidate zero-based contact index.
/// @return `1` exactly when @p index addresses a current contact record.
static int8_t checked_contact(rt_world_impl *w, int64_t index) {
    return w && index >= 0 && index < w->contact_count;
}

/// @brief Return the first body in a contact pair (the "A" side) at the given contact index.
/// @param obj Opaque Physics2D.World handle.
/// @param index Zero-based contact index.
/// @return The borrowed first body handle, or `NULL` for an invalid world or
///         index. The contact retains it only until contacts are cleared.
void *rt_physics2d_world_contact_body_a(void *obj, int64_t index) {
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactBodyA: expected Physics2D.World");
    return checked_contact(w, index) ? w->contacts[index].body_a : NULL;
}

/// @brief Return the second body in a contact pair (the "B" side) at the given contact index.
/// @param obj Opaque Physics2D.World handle.
/// @param index Zero-based contact index.
/// @return The borrowed second body handle, or `NULL` for an invalid world or
///         index. The contact retains it only until contacts are cleared.
void *rt_physics2d_world_contact_body_b(void *obj, int64_t index) {
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactBodyB: expected Physics2D.World");
    return checked_contact(w, index) ? w->contacts[index].body_b : NULL;
}

/// @brief Contact normal X component (points from body A toward body B).
/// @param obj Opaque Physics2D.World handle.
/// @param index Zero-based contact index.
/// @return The stored normal X component, or `0.0` for invalid input.
double rt_physics2d_world_contact_nx(void *obj, int64_t index) {
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactNX: expected Physics2D.World");
    return checked_contact(w, index) ? w->contacts[index].nx : 0.0;
}

/// @brief Contact normal Y component (points from body A toward body B).
/// @param obj Opaque Physics2D.World handle.
/// @param index Zero-based contact index.
/// @return The stored normal Y component, or `0.0` for invalid input.
double rt_physics2d_world_contact_ny(void *obj, int64_t index) {
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactNY: expected Physics2D.World");
    return checked_contact(w, index) ? w->contacts[index].ny : 0.0;
}

/// @brief Penetration depth at the contact point (0 for tunnelling contacts caught by CCD).
/// @param obj Opaque Physics2D.World handle.
/// @param index Zero-based contact index.
/// @return The nonnegative penetration depth, or `0.0` for invalid input or a
///         swept contact without overlap depth.
double rt_physics2d_world_contact_depth(void *obj, int64_t index) {
    rt_world_impl *w = checked_world(obj, "Physics2D.World.ContactDepth: expected Physics2D.World");
    return checked_contact(w, index) ? w->contacts[index].penetration : 0.0;
}

//=============================================================================
// Public API — Body
//=============================================================================

/// @brief Construct a 2D rigid body with top-left position (x, y), size (w, h), and `mass`.
/// `mass <= 0` ⇒ static (immovable, infinite mass). Defaults: restitution 0.5 (moderately bouncy),
/// friction 0.3, collision_layer 1, collision_mask -1 (collides with all 64 layers).
/// @param x Initial top-left X coordinate; non-finite values become zero.
/// @param y Initial top-left Y coordinate; non-finite values become zero.
/// @param w Requested AABB width; non-finite or nonpositive values become one.
/// @param h Requested AABB height; non-finite or nonpositive values become one.
/// @param mass Positive dynamic mass, or a nonpositive/non-finite value for a
///             static body.
/// @return A new AABB Physics2D.Body handle, or `NULL` after allocation failure.
void *rt_physics2d_body_new(double x, double y, double w, double h, double mass) {
    rt_body_impl *b =
        (rt_body_impl *)rt_obj_new_i64(RT_PHYSICS2D_BODY_CLASS_ID, (int64_t)sizeof(rt_body_impl));
    if (!b) {
        rt_trap("Physics2D.Body: allocation failed");
        return NULL;
    }
    b->vptr = NULL;
    x = finite_or(x, 0.0);
    y = finite_or(y, 0.0);
    w = positive_or(w, 1.0);
    h = positive_or(h, 1.0);
    mass = rt_physics2d_normalize_mass(mass);
    b->x = x;
    b->y = y;
    b->prev_x = x;
    b->prev_y = y;
    b->w = w;
    b->h = h;
    b->vx = 0.0;
    b->vy = 0.0;
    b->fx = 0.0;
    b->fy = 0.0;
    b->mass = mass;
    b->inv_mass = (mass > 0.0) ? (1.0 / mass) : 0.0;
    b->restitution = 0.5;            /* Moderately bouncy by default */
    b->friction = 0.3;               /* Moderate friction by default */
    b->collision_layer = 1;          /* Default: layer 0, bit 0 set */
    b->collision_mask = INT64_C(-1); /* Default: collide with all 64 layers */
    b->radius = 0.0;
    b->is_circle = 0;
    b->owner_world = NULL;
    b->world_index = -1;
    sanitize_body_state(b);
    return b;
}

// The following functions are simple accessors over the body's stored state
// (position, size, velocity). Each returns 0.0 for a NULL handle.

/// @brief Return the body's stored X reference coordinate.
/// @details This is the top-left X for an AABB body and center X for a circle.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored X coordinate, or `0.0` for null or mistyped input.
double rt_physics2d_body_x(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.X: expected Physics2D.Body");
    return b ? b->x : 0.0;
}

/// @brief Return the body's stored Y reference coordinate.
/// @details This is the top-left Y for an AABB body and center Y for a circle;
///          positive Y points downward.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored Y coordinate, or `0.0` for null or mistyped input.
double rt_physics2d_body_y(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Y: expected Physics2D.Body");
    return b ? b->y : 0.0;
}

/// @brief X position at the start of the most recent integration substep.
/// @details Used by the swept CCD path; useful in game logic for computing per-frame
///   displacement without storing a separate previous-position variable. It is
///   top-left X for AABBs and center X for circles.
/// @param obj Opaque Physics2D.Body handle.
/// @return The previous X coordinate, or `0.0` for null or mistyped input.
double rt_physics2d_body_prev_x(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.PrevX: expected Physics2D.Body");
    return b ? b->prev_x : 0.0;
}

/// @brief Y position at the start of the most recent integration substep.
/// @details Mirror of rt_physics2d_body_prev_x for the vertical axis. Used by
///   the swept CCD path to construct the previous-frame AABB, and is useful in
///   game logic for computing per-frame vertical displacement without storing a
///   separate previous-position variable. It is top-left Y for AABBs and center
///   Y for circles.
/// @param obj Physics2D.Body instance.
/// @return Y coordinate recorded at the beginning of the most recent simulation
///   substep, or 0.0 if @p obj is not a valid body.
double rt_physics2d_body_prev_y(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.PrevY: expected Physics2D.Body");
    return b ? b->prev_y : 0.0;
}

/// @brief AABB width in world units.
/// @details This stored field describes box bodies and is not the circle
///          diameter.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored width, or `0.0` for null or mistyped input.
double rt_physics2d_body_w(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Width: expected Physics2D.Body");
    return b ? b->w : 0.0;
}

/// @brief AABB height in world units.
/// @details This stored field describes box bodies and is not the circle
///          diameter.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored height, or `0.0` for null or mistyped input.
double rt_physics2d_body_h(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Height: expected Physics2D.Body");
    return b ? b->h : 0.0;
}

/// @brief Linear X-velocity in world units per second.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored horizontal velocity, or `0.0` for invalid input.
double rt_physics2d_body_vx(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.VelocityX: expected Physics2D.Body");
    return b ? b->vx : 0.0;
}

/// @brief Linear Y-velocity in world units per second.
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored vertical velocity, or `0.0` for invalid input.
double rt_physics2d_body_vy(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.VelocityY: expected Physics2D.Body");
    return b ? b->vy : 0.0;
}

/// @brief Teleport the body to (x, y) world coordinates. Bypasses collision (the next `_step`
/// will resolve any resulting overlap). Use `_apply_impulse` for physically realistic motion.
/// @details Updates both current and previous coordinates, so the teleport
///          creates no swept motion. Coordinates are top-left for AABBs and
///          center-based for circles. Non-finite coordinates are ignored.
/// @param obj Opaque Physics2D.Body handle to move.
/// @param x New X reference coordinate.
/// @param y New Y reference coordinate.
void rt_physics2d_body_set_pos(void *obj, double x, double y) {
    if (!obj)
        return;
    if (!isfinite(x) || !isfinite(y))
        return;
    rt_body_impl *body = checked_body(obj, "Physics2D.Body.SetPos: expected Physics2D.Body");
    if (!body)
        return;
    body->x = x;
    body->y = y;
    body->prev_x = x;
    body->prev_y = y;
    sanitize_body_state(body);
}

/// @brief Override the body's linear velocity directly.
/// @details Applies only to dynamic bodies (mass > 0); static bodies (mass = 0)
///   ignore this and are pinned to zero velocity. Note there is currently no true
///   kinematic body category — a "moving platform" that carries/pushes dynamic
///   bodies must be given a large finite mass (so collision impulses barely move
///   it) and driven with SetVel, since a mass-0 body neither integrates position
///   nor sweeps. SetPos teleports and resets the swept-motion history, so it does
///   not carry bodies either.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param vx Finite horizontal velocity; non-finite input is ignored.
/// @param vy Finite vertical velocity; non-finite input is ignored.
void rt_physics2d_body_set_vel(void *obj, double vx, double vy) {
    if (!obj)
        return;
    if (!isfinite(vx) || !isfinite(vy))
        return;
    rt_body_impl *body = checked_body(obj, "Physics2D.Body.SetVel: expected Physics2D.Body");
    if (!body)
        return;
    if (body->inv_mass == 0.0) {
        body->vx = 0.0;
        body->vy = 0.0;
        return;
    }
    body->vx = vx;
    body->vy = vy;
    sanitize_body_state(body);
}

/// @brief Add (fx, fy) to the body's accumulated force vector. Forces are integrated and
/// cleared each `_step`; call repeatedly within a frame to combine multiple force contributors.
/// @details Static bodies and non-finite force components are ignored. The
///          accumulated result is sanitized to the engine's finite force range.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param fx Finite horizontal force contribution.
/// @param fy Finite vertical force contribution.
void rt_physics2d_body_apply_force(void *obj, double fx, double fy) {
    if (!obj)
        return;
    if (!isfinite(fx) || !isfinite(fy))
        return;
    rt_body_impl *body = checked_body(obj, "Physics2D.Body.ApplyForce: expected Physics2D.Body");
    if (!body)
        return;
    if (body->inv_mass == 0.0)
        return;
    /* Forces accumulate until the next Step(); they are additive so multiple
     * ApplyForce calls in the same frame combine correctly. */
    body->fx = rt_physics2d_saturating_add(body->fx, fx);
    body->fy = rt_physics2d_saturating_add(body->fy, fy);
    sanitize_body_state(body);
}

/// @brief Apply an instantaneous velocity change of (ix, iy) * inv_mass. Use for jumps,
/// explosions, kicks — anything that should change velocity *now* without requiring a force
/// applied for a duration.
/// @details Static bodies and non-finite impulse components are ignored; the
///          resulting velocity is sanitized to the supported finite range.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param ix Finite horizontal impulse.
/// @param iy Finite vertical impulse.
void rt_physics2d_body_apply_impulse(void *obj, double ix, double iy) {
    rt_body_impl *b;
    if (!obj)
        return;
    if (!isfinite(ix) || !isfinite(iy))
        return;
    b = checked_body(obj, "Physics2D.Body.ApplyImpulse: expected Physics2D.Body");
    if (!b)
        return;
    if (b->inv_mass == 0.0)
        return; /* Static bodies cannot be moved by impulses */
    /* An impulse is an instantaneous velocity change: Δv = impulse / mass,
     * equivalently: Δv = impulse * inv_mass. */
    b->vx = rt_physics2d_saturating_add(b->vx, rt_physics2d_saturating_mul(ix, b->inv_mass));
    b->vy = rt_physics2d_saturating_add(b->vy, rt_physics2d_saturating_mul(iy, b->inv_mass));
    sanitize_body_state(b);
}

/// @brief Read the body's bounciness coefficient ([0, 1] typical).
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored restitution in `[0, 1]`, or `0.0` for invalid input.
double rt_physics2d_body_restitution(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Restitution: expected Physics2D.Body");
    return b ? b->restitution : 0.0;
}

/// @brief Set bounciness: zero disables bounce and one is perfectly elastic.
/// @details Collision response uses the lower of the two bodies' coefficients
///          and suppresses bounce below the resting-contact speed threshold.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param r Requested coefficient, clamped to `[0, 1]`; non-finite values
///          become zero.
void rt_physics2d_body_set_restitution(void *obj, double r) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Restitution.set: expected Physics2D.Body");
    if (b)
        b->restitution = clamp01(r);
}

/// @brief Read the body's friction coefficient ([0, 1] typical).
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored friction in `[0, 1]`, or `0.0` for invalid input.
double rt_physics2d_body_friction(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Friction: expected Physics2D.Body");
    return b ? b->friction : 0.0;
}

/// @brief Set friction: 0 = ice, 1 = sandpaper. Applied as a tangential damping during contact.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param f Requested coefficient, clamped to `[0, 1]`; non-finite values
///          become zero.
void rt_physics2d_body_set_friction(void *obj, double f) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Friction.set: expected Physics2D.Body");
    if (b)
        b->friction = clamp01(f);
}

/// @brief Returns 1 if the body is static (mass=0, immovable). Static bodies skip integration.
/// @param obj Opaque Physics2D.Body handle.
/// @return `1` when inverse mass is zero, otherwise `0`; invalid input returns
///         `0`.
int8_t rt_physics2d_body_is_static(void *obj) {
    /* A body is static when its inverse-mass is zero (mass == 0 at creation) */
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.IsStatic: expected Physics2D.Body");
    return (b && b->inv_mass == 0.0) ? 1 : 0;
}

/// @brief Read the body's mass (0 if static or NULL).
/// @param obj Opaque Physics2D.Body handle.
/// @return The positive dynamic mass, or `0.0` for a static or invalid body.
double rt_physics2d_body_mass(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.Mass: expected Physics2D.Body");
    return b ? b->mass : 0.0;
}

/// @brief Read the body's collision-layer bitmask (which layers it belongs to).
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored signed 64-bit layer mask, or `0` for invalid input.
int64_t rt_physics2d_body_collision_layer(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.CollisionLayer: expected Physics2D.Body");
    return b ? b->collision_layer : 0;
}

/// @brief Set the collision-layer bitmask. Combined with the *other* body's collision_mask
/// during overlap tests — only pairs where each body's layer matches the other's mask collide.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param layer Signed 64-bit membership mask stored verbatim.
void rt_physics2d_body_set_collision_layer(void *obj, int64_t layer) {
    rt_body_impl *b =
        checked_body(obj, "Physics2D.Body.CollisionLayer.set: expected Physics2D.Body");
    if (b)
        b->collision_layer = layer;
}

/// @brief Read the body's collision-mask bitmask (which layers it tests against).
/// @param obj Opaque Physics2D.Body handle.
/// @return The stored signed 64-bit collision mask, or `0` for invalid input.
int64_t rt_physics2d_body_collision_mask(void *obj) {
    rt_body_impl *b = checked_body(obj, "Physics2D.Body.CollisionMask: expected Physics2D.Body");
    return b ? b->collision_mask : 0;
}

/// @brief Set the collision-mask. Each bit corresponds to a layer this body collides with.
/// Default -1 = collides with all 64 layers. Use 0 to make the body collision-free.
/// @param obj Opaque Physics2D.Body handle to update.
/// @param mask Signed 64-bit collision-selection mask stored verbatim.
void rt_physics2d_body_set_collision_mask(void *obj, int64_t mask) {
    rt_body_impl *b =
        checked_body(obj, "Physics2D.Body.CollisionMask.set: expected Physics2D.Body");
    if (b)
        b->collision_mask = mask;
}

//=============================================================================
// Projectile2D
//=============================================================================

/// @brief Analytic projectile launch parameters and elapsed-state tracker.
typedef struct {
    /// Reserved runtime object virtual-table slot.
    void *vptr;
    /// Initial horizontal and vertical position.
    double p0x, p0y;
    /// Initial horizontal and vertical velocity.
    double v0x, v0y;
    /// Constant horizontal and vertical acceleration.
    double gx, gy;
    /// Nonnegative linear drag coefficient.
    double drag;
    /// Accumulated positive time passed to Advance.
    double total_time;
    /// Boolean ground-threshold state latched by Advance.
    int8_t landed;
    /// Positive-Y-down ground threshold, or infinity when disabled.
    double ground_y;
} rt_projectile2d_impl;

/// @brief Safe-cast a handle to the Projectile2D impl, trapping @p api on a
///        class-id mismatch. @return The impl, or NULL if @p obj is NULL.
/// @param obj Opaque Projectile2D candidate.
/// @param api Trap message used for a non-null class mismatch.
/// @return The validated implementation pointer, or `NULL`.
static rt_projectile2d_impl *checked_projectile(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_obj_is_instance(obj, RT_PHYSICS2D_PROJECTILE_CLASS_ID, sizeof(rt_projectile2d_impl))) {
        rt_trap(api);
        return NULL;
    }
    return (rt_projectile2d_impl *)obj;
}

/// @brief Return @p value if finite, else @p fallback (sanitizes user-supplied
///        projectile parameters against NaN/Inf).
/// @param value Candidate projectile parameter.
/// @param fallback Replacement for non-finite input.
/// @return @p value when finite, otherwise @p fallback.
static double projectile_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Allocate an analytic projectile with initial position, velocity, and gravity.
/// @details Every non-finite component is replaced with zero. Drag starts at
///          zero, elapsed time starts at zero, and ground detection is disabled
///          by an infinite ground threshold.
/// @param p0x Initial horizontal position.
/// @param p0y Initial vertical position.
/// @param v0x Initial horizontal velocity.
/// @param v0y Initial vertical velocity.
/// @param gx Constant horizontal acceleration.
/// @param gy Constant vertical acceleration in positive-Y-down coordinates.
/// @return A new Projectile2D handle, or `NULL` if allocation fails.
void *rt_projectile2d_new(double p0x, double p0y, double v0x, double v0y, double gx, double gy) {
    rt_projectile2d_impl *p = (rt_projectile2d_impl *)rt_obj_new_i64(
        RT_PHYSICS2D_PROJECTILE_CLASS_ID, (int64_t)sizeof(rt_projectile2d_impl));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->p0x = projectile_finite_or(p0x, 0.0);
    p->p0y = projectile_finite_or(p0y, 0.0);
    p->v0x = projectile_finite_or(v0x, 0.0);
    p->v0y = projectile_finite_or(v0y, 0.0);
    p->gx = projectile_finite_or(gx, 0.0);
    p->gy = projectile_finite_or(gy, 0.0);
    p->ground_y = INFINITY;
    return p;
}

/// @brief Set the projectile's nonnegative linear drag coefficient.
/// @details Positive finite input is stored verbatim; zero, negative, NaN, and
///          infinity disable drag. Existing elapsed time and landed state are
///          unchanged.
/// @param obj Opaque Projectile2D handle to update.
/// @param drag Requested linear damping coefficient.
void rt_projectile2d_set_drag(void *obj, double drag) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.SetDrag: expected Projectile2D");
    if (!p)
        return;
    p->drag = isfinite(drag) && drag > 0.0 ? drag : 0.0;
}

/// @brief Set the positive-Y-down threshold used by landed detection.
/// @details Non-finite input restores infinity and therefore disables landing.
///          This setter does not immediately recompute the current landed flag.
/// @param obj Opaque Projectile2D handle to update.
/// @param y Finite ground Y coordinate, or a non-finite value to disable it.
void rt_projectile2d_set_ground_y(void *obj, double y) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.SetGroundY: expected Projectile2D");
    if (p)
        p->ground_y = isfinite(y) ? y : INFINITY;
}

/// @brief Reset elapsed time and clear the latched landed flag.
/// @details Launch parameters, gravity, drag, and ground threshold are
///          preserved.
/// @param obj Opaque Projectile2D handle to reset.
void rt_projectile2d_reset(void *obj) {
    rt_projectile2d_impl *p = checked_projectile(obj, "Projectile2D.Reset: expected Projectile2D");
    if (!p)
        return;
    p->total_time = 0.0;
    p->landed = 0;
}

/// @brief Position of one axis at time @p t under constant gravity @p g and
///        linear @p drag (closed-form; reduces to p0+v0·t+½g·t² when drag==0).
/// @param p0 Initial position on the selected axis.
/// @param v0 Initial velocity on the selected axis.
/// @param g Constant acceleration on the selected axis.
/// @param drag Nonnegative linear drag coefficient.
/// @param t Time since launch; nonpositive or non-finite values select @p p0.
/// @return The closed-form position at @p t, saturated to a finite double when
///         the mathematical result exceeds the representable range.
static double projectile_pos_at(double p0, double v0, double g, double drag, double t) {
    if (!isfinite(t) || t <= 0.0)
        return p0;
    double velocity_term = 0.0;
    double gravity_term = 0.0;
    if (drag <= 0.0) {
        velocity_term = rt_physics2d_saturating_mul(v0, t);
        gravity_term = rt_physics2d_saturating_mul4(0.5, g, t, t);
    } else if (drag > DBL_MAX / t) {
        /* z=drag*t exceeds the representable range, so exp(-z) is
         * indistinguishable from zero. Evaluate the limiting form directly. */
        double inv_drag = rt_physics2d_saturating_div(1.0, drag);
        velocity_term = rt_physics2d_saturating_mul(v0, inv_drag);
        double transient_time = rt_physics2d_saturating_add(t, -inv_drag);
        gravity_term = rt_physics2d_saturating_mul4(g, inv_drag, transient_time, 1.0);
    } else {
        double z = drag * t;
        double q_over_z = 0.0;
        double phi2 = 0.0;
        if (z < 1.0e-3) {
            /* Stable Taylor forms for (1-exp(-z))/z and
             * (z-(1-exp(-z)))/z^2 near zero. */
            q_over_z =
                1.0 +
                z * (-0.5 + z * (1.0 / 6.0 + z * (-1.0 / 24.0 + z * (1.0 / 120.0 - z / 720.0))));
            phi2 =
                0.5 + z * (-1.0 / 6.0 +
                           z * (1.0 / 24.0 + z * (-1.0 / 120.0 + z * (1.0 / 720.0 - z / 5040.0))));
        } else {
            q_over_z = -expm1(-z) / z;
            phi2 = (1.0 - q_over_z) / z;
        }
        velocity_term = rt_physics2d_saturating_mul4(v0, t, q_over_z, 1.0);
        gravity_term = rt_physics2d_saturating_mul4(g, t, t, phi2);
    }
    return rt_physics2d_saturating_add(p0,
                                       rt_physics2d_saturating_add(velocity_term, gravity_term));
}

/// @brief Velocity of one axis at time @p t under gravity @p g and linear
///        @p drag (closed-form; reduces to v0+g·t when drag==0).
/// @param v0 Initial velocity on the selected axis.
/// @param g Constant acceleration on the selected axis.
/// @param drag Nonnegative linear drag coefficient.
/// @param t Time since launch; nonpositive or non-finite values select @p v0.
/// @return The closed-form velocity at @p t, saturated to a finite double when
///         the mathematical result exceeds the representable range.
static double projectile_vel_at(double v0, double g, double drag, double t) {
    if (!isfinite(t) || t <= 0.0)
        return v0;
    if (drag <= 0.0)
        return rt_physics2d_saturating_add(v0, rt_physics2d_saturating_mul(g, t));
    if (drag > DBL_MAX / t)
        return rt_physics2d_saturating_div(g, drag);
    double z = drag * t;
    double q_over_z = 0.0;
    if (z < 1.0e-3) {
        q_over_z =
            1.0 + z * (-0.5 + z * (1.0 / 6.0 + z * (-1.0 / 24.0 + z * (1.0 / 120.0 - z / 720.0))));
    } else {
        q_over_z = -expm1(-z) / z;
    }
    return rt_physics2d_saturating_add(rt_physics2d_saturating_mul(v0, exp(-z)),
                                       rt_physics2d_saturating_mul4(g, t, q_over_z, 1.0));
}

/// @brief Bisect the earliest ground crossing in a known sign-changing bracket.
/// @param p Borrowed projectile parameters.
/// @param lo Time known to be strictly above the positive-Y-down ground threshold.
/// @param hi Time known to be at or below that threshold.
/// @return The upper edge of the converged crossing bracket.
static double projectile_bisect_ground(const rt_projectile2d_impl *p, double lo, double hi) {
    for (int i = 0; i < 80; ++i) {
        double mid = lo + (hi - lo) * 0.5;
        double y = projectile_pos_at(p->p0y, p->v0y, p->gy, p->drag, mid);
        if (y >= p->ground_y)
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}

/// @brief Compute a stable positive turning time for upward terminal gravity.
/// @details Used when `gy < 0`, `v0y > 0`, and linear drag causes a single
///          maximum positive-Y excursion. The logarithmic form avoids overflow
///          in `drag*v0y/(-gy)`.
/// @param drag Positive finite drag.
/// @param v0y Positive initial vertical velocity.
/// @param gy Negative vertical acceleration.
/// @return Finite turning time when representable, otherwise `DBL_MAX`.
static double projectile_turning_time(double drag, double v0y, double gy) {
    double log_ratio = log(drag) + log(v0y) - log(-gy);
    double log_one_plus_ratio = 0.0;
    if (log_ratio > log(DBL_MAX))
        log_one_plus_ratio = log_ratio;
    else if (log_ratio < log(DBL_MIN))
        log_one_plus_ratio = exp(log_ratio);
    else
        log_one_plus_ratio = log1p(exp(log_ratio));
    return rt_physics2d_saturating_div(log_one_plus_ratio, drag);
}

/// @brief Compute the earliest absolute launch time that reaches configured ground.
/// @param p Borrowed validated projectile.
/// @return Nonnegative impact time, or positive infinity when unreachable.
static double projectile_time_to_ground_impl(const rt_projectile2d_impl *p) {
    if (!p || !isfinite(p->ground_y))
        return INFINITY;
    if (p->p0y >= p->ground_y)
        return 0.0;

    double c = rt_physics2d_saturating_add(p->p0y, -p->ground_y);
    if (p->drag <= 0.0) {
        double a = 0.5 * p->gy;
        double b = p->v0y;
        if (a == 0.0) {
            if (b <= 0.0)
                return INFINITY;
            double t = rt_physics2d_saturating_div(-c, b);
            return isfinite(t) && t >= 0.0 ? t : INFINITY;
        }

        /* Scale the quadratic before forming its discriminant so b*b and
         * 4*a*c cannot overflow even for finite extreme launch parameters. */
        double scale = fmax(fabs(a), fmax(fabs(b), fabs(c)));
        double as = a / scale;
        double bs = b / scale;
        double cs = c / scale;
        double bs_sq = bs * bs;
        double four_ac = 4.0 * as * cs;
        double disc = fma(bs, bs, -four_ac);
        double tolerance = 8.0 * DBL_EPSILON * fmax(bs_sq, fabs(four_ac));
        if (disc < 0.0) {
            if (disc < -tolerance)
                return INFINITY;
            disc = 0.0;
        }
        double root = sqrt(disc);
        double q = -0.5 * (bs + copysign(root, bs));
        double t1 = q != 0.0 ? q / as : (-bs + root) / (2.0 * as);
        double t2 = q != 0.0 ? cs / q : t1;
        double best = INFINITY;
        if (isfinite(t1) && t1 >= 0.0)
            best = t1;
        if (isfinite(t2) && t2 >= 0.0 && t2 < best)
            best = t2;
        return best;
    }

    double delta = rt_physics2d_saturating_add(p->ground_y, -p->p0y);
    if (p->gy == 0.0) {
        if (p->v0y <= 0.0)
            return INFINITY;
        double limit = rt_physics2d_saturating_div(p->v0y, p->drag);
        if (limit <= delta)
            return INFINITY; /* Equality is reached only asymptotically. */
        double fraction = delta / limit;
        if (!(fraction >= 0.0) || fraction >= 1.0)
            return INFINITY;
        return rt_physics2d_saturating_div(-log1p(-fraction), p->drag);
    }

    if (p->gy < 0.0) {
        if (p->v0y <= 0.0)
            return INFINITY;
        double turn = projectile_turning_time(p->drag, p->v0y, p->gy);
        double peak = projectile_pos_at(p->p0y, p->v0y, p->gy, p->drag, turn);
        if (peak < p->ground_y)
            return INFINITY;
        if (peak == p->ground_y)
            return turn;
        return projectile_bisect_ground(p, 0.0, turn);
    }

    /* Positive terminal velocity guarantees one eventual crossing. A bound on
     * the decaying transient gives a near-direct upper bracket, with a small
     * doubling fallback for rounding at extreme scales. */
    double terminal = rt_physics2d_saturating_div(p->gy, p->drag);
    double transient_velocity = fabs(rt_physics2d_saturating_add(p->v0y, -terminal));
    double transient_bound = rt_physics2d_saturating_div(transient_velocity, p->drag);
    double hi =
        rt_physics2d_saturating_div(rt_physics2d_saturating_add(delta, transient_bound), terminal);
    if (!isfinite(hi) || hi < 1.0)
        hi = 1.0;
    double hi_y = projectile_pos_at(p->p0y, p->v0y, p->gy, p->drag, hi);
    for (int i = 0; i < 64 && hi_y < p->ground_y && hi < DBL_MAX; ++i) {
        hi = hi > DBL_MAX * 0.5 ? DBL_MAX : hi * 2.0;
        hi_y = projectile_pos_at(p->p0y, p->v0y, p->gy, p->drag, hi);
    }
    if (hi_y < p->ground_y)
        return INFINITY;
    return projectile_bisect_ground(p, 0.0, hi);
}

/// @brief Advance the projectile's elapsed-time tracker and update landing state.
/// @details Positive finite @p dt is added only while the projectile is not
///          landed. Landing latches when the earliest analytic ground crossing
///          is at or before the new total time, including trajectories that
///          cross and then return above the ground before the sampled endpoint.
///          The time is not clamped back to the exact impact.
/// @param obj Opaque Projectile2D handle to advance.
/// @param dt Positive finite elapsed time.
void rt_projectile2d_advance(void *obj, double dt) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.Advance: expected Projectile2D");
    if (!p || !isfinite(dt) || dt <= 0.0 || p->landed)
        return;
    p->total_time = rt_physics2d_saturating_add(p->total_time, dt);
    double impact_time = projectile_time_to_ground_impl(p);
    if (impact_time <= p->total_time)
        p->landed = 1;
}

/// @brief Evaluate horizontal position at an absolute time since launch.
/// @param obj Opaque Projectile2D handle.
/// @param t Time since launch; nonpositive or non-finite values select the
///          initial position.
/// @return The analytic horizontal position, or `0.0` for invalid input.
double rt_projectile2d_x_at(void *obj, double t) {
    rt_projectile2d_impl *p = checked_projectile(obj, "Projectile2D.XAt: expected Projectile2D");
    return p ? projectile_pos_at(p->p0x, p->v0x, p->gx, p->drag, t) : 0.0;
}

/// @brief Evaluate vertical position at an absolute time since launch.
/// @param obj Opaque Projectile2D handle.
/// @param t Time since launch; nonpositive or non-finite values select the
///          initial position.
/// @return The analytic vertical position, or `0.0` for invalid input.
double rt_projectile2d_y_at(void *obj, double t) {
    rt_projectile2d_impl *p = checked_projectile(obj, "Projectile2D.YAt: expected Projectile2D");
    return p ? projectile_pos_at(p->p0y, p->v0y, p->gy, p->drag, t) : 0.0;
}

/// @brief Evaluate horizontal velocity at an absolute time since launch.
/// @param obj Opaque Projectile2D handle.
/// @param t Time since launch; nonpositive or non-finite values select the
///          initial velocity.
/// @return The analytic horizontal velocity, or `0.0` for invalid input.
double rt_projectile2d_vx_at(void *obj, double t) {
    rt_projectile2d_impl *p = checked_projectile(obj, "Projectile2D.VXAt: expected Projectile2D");
    return p ? projectile_vel_at(p->v0x, p->gx, p->drag, t) : 0.0;
}

/// @brief Evaluate vertical velocity at an absolute time since launch.
/// @param obj Opaque Projectile2D handle.
/// @param t Time since launch; nonpositive or non-finite values select the
///          initial velocity.
/// @return The analytic vertical velocity, or `0.0` for invalid input.
double rt_projectile2d_vy_at(void *obj, double t) {
    rt_projectile2d_impl *p = checked_projectile(obj, "Projectile2D.VYAt: expected Projectile2D");
    return p ? projectile_vel_at(p->v0y, p->gy, p->drag, t) : 0.0;
}

/// @brief Return the landed flag latched by `rt_projectile2d_advance`.
/// @param obj Opaque Projectile2D handle.
/// @return `1` after the configured ground threshold is reached, otherwise
///         `0`; invalid input also returns `0`.
int8_t rt_projectile2d_has_landed(void *obj) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.HasLanded: expected Projectile2D");
    return p ? p->landed : 0;
}

/// @brief Return the elapsed time accumulated by successful Advance calls.
/// @param obj Opaque Projectile2D handle.
/// @return Accumulated elapsed time, or `0.0` for invalid input.
double rt_projectile2d_total_time(void *obj) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.TotalTime: expected Projectile2D");
    return p ? p->total_time : 0.0;
}

/// @brief Estimate a nonnegative absolute launch time at the ground threshold.
/// @details Returns infinity when ground detection is disabled or no supported
///          crossing is found. With drag, the implementation expands a
///          positive-time bracket for at most 64 iterations and then performs
///          64 bisection iterations. Without drag, it solves the constant-
///          acceleration quadratic and selects the smaller nonnegative root,
///          with linear and stationary fallbacks for near-zero acceleration.
///          The result is measured from launch, not from the current
///          `total_time`.
/// @param obj Opaque Projectile2D handle.
/// @return A nonnegative time since launch, or positive infinity for invalid,
///         disabled, or unreachable cases.
double rt_projectile2d_time_to_ground(void *obj) {
    rt_projectile2d_impl *p =
        checked_projectile(obj, "Projectile2D.TimeToGround: expected Projectile2D");
    return projectile_time_to_ground_impl(p);
}
