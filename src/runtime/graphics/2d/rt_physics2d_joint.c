//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_physics2d_joint.c
/// @brief Implements Physics2D joint construction, world registration,
///        constraint solving, and circle-body creation.
///
/// @details Joints retain both endpoint bodies and may be retained again by a
/// world. Spring constraints apply bounded velocity impulses, while distance,
/// hinge, and rope constraints use iterative mass-weighted position projection
/// followed by velocity projection to prevent hidden constraint-axis velocity.
///
// File: src/runtime/graphics/2d/rt_physics2d_joint.c
// Purpose: Joint/constraint implementations for the 2D physics engine.
//   Provides DistanceJoint, SpringJoint, HingeJoint, RopeJoint, circle body
//   creation, and the iterative joint constraint solver.
//
// Key invariants:
//   - Joints use position-based constraint solving with Gauss-Seidel relaxation.
//   - Joint solver runs PH_JOINT_ITERATIONS passes per world step.
//   - Circle bodies store radius; AABB bodies have radius == 0.
//   - Joint constructors return NULL for NULL/self joints and trap for wrong handle types.
//
// Ownership/Lifetime:
//   - Joint objects are GC-managed (rt_obj_new_i64).
//   - World retains joints via add/remove.
//
// Links: rt_physics2d_joint.h, rt_physics2d_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_physics2d_joint.h"
#include "rt_physics2d_internal.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Helpers
//=============================================================================

/// @brief Return the world-space X coordinate of the body's center of mass.
/// @details Circle bodies store their center directly in x,y.  AABB bodies store
///   the top-left corner, so the center is x + w/2.
/// @param b Borrowed body implementation.
/// @return The shape's current center X coordinate.
static double body_cx(rt_body_impl *b) {
    return b->is_circle ? b->x : (b->x + b->w * 0.5);
}

/// @brief Return the world-space Y coordinate of the body's center of mass.
/// @details Circle bodies store their center directly in x,y.  AABB bodies store
///   the top-left corner, so the center is y + h/2.
/// @param b Borrowed body implementation.
/// @return The shape's current center Y coordinate.
static double body_cy(rt_body_impl *b) {
    return b->is_circle ? b->y : (b->y + b->h * 0.5);
}

/// @brief Return @p value if it is finite, otherwise return @p fallback.
/// @details Used to sanitize NaN/Inf inputs on joint creation so internal state
///   always holds valid floating-point values.
/// @param value Candidate value.
/// @param fallback Replacement for non-finite input.
/// @return @p value when finite, otherwise @p fallback.
static double finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Return @p value if it is finite and strictly positive, otherwise 0.0.
/// @details Used when sanitizing joint parameters that must be non-negative
///   (rest length, stiffness, damping, max length).  NaN, Inf, and negative
///   values all collapse to zero, which is the disabled/zero-effect state.
/// @param value Candidate nonnegative joint parameter.
/// @return @p value when finite and strictly positive, otherwise `0.0`.
static double nonnegative_finite_or_zero(double value) {
    return (isfinite(value) && value > 0.0) ? value : 0.0;
}

/// @brief Validate that @p obj is a Physics2D.Body handle and return its impl pointer.
/// @details Returns NULL for a NULL @p obj (soft miss).  Calls rt_trap with
///   @p api as the message if the object exists but is not a body handle
///   (hard programmer error).
/// @param obj  Opaque GC object pointer from Zia/BASIC.
/// @param api  API name string used as the trap message on type mismatch.
/// @return     Typed implementation pointer, or NULL.
static rt_body_impl *checked_body(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_physics2d_is_body_handle(obj)) {
        rt_trap(api);
        return NULL;
    }
    return (rt_body_impl *)obj;
}

/// @brief Validate that @p obj is a Physics2D.Joint handle and return its impl pointer.
/// @details Returns NULL for a NULL @p obj (soft miss).  Calls rt_trap with
///   @p api as the message if the object exists but is not a joint handle.
/// @param obj  Opaque GC object pointer from Zia/BASIC.
/// @param api  API name string used as the trap message on type mismatch.
/// @return     Typed implementation pointer, or NULL.
static ph_joint *checked_joint(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_physics2d_is_joint_handle(obj)) {
        rt_trap(api);
        return NULL;
    }
    return (ph_joint *)obj;
}

/// @brief Validate that @p obj is a Physics2D.World handle and return its impl pointer.
/// @details Returns NULL for a NULL @p obj (soft miss).  Calls rt_trap with
///   @p api as the message if the object exists but is not a world handle.
/// @param obj  Opaque GC object pointer from Zia/BASIC.
/// @param api  API name string used as the trap message on type mismatch.
/// @return     Typed implementation pointer, or NULL.
static rt_world_impl *checked_world(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (!rt_physics2d_is_world_handle(obj)) {
        rt_trap(api);
        return NULL;
    }
    return (rt_world_impl *)obj;
}

/// @brief GC finalizer — release the body references retained by the joint.
/// @details Called by the GC when the joint's reference count reaches zero.
///   Releases body_a and body_b (which were retained in alloc_joint) and
///   frees them immediately if their counts also hit zero.  Nulls the pointers
///   to guard against double-free if the finalizer is somehow called twice.
/// @param obj Finalizing joint implementation supplied by the object system.
static void joint_finalizer(void *obj) {
    ph_joint *j = (ph_joint *)obj;
    if (!j)
        return;

    if (j->body_a && rt_obj_release_check0(j->body_a))
        rt_obj_free(j->body_a);
    if (j->body_b && rt_obj_release_check0(j->body_b))
        rt_obj_free(j->body_b);
    j->body_a = NULL;
    j->body_b = NULL;
}

/// @brief Move a body so that its center of mass lands at (cx, cy).
/// @details For circle bodies the center is stored directly in x,y.  For AABB
///   bodies the stored position is the top-left corner, so x = cx - w/2 and
///   y = cy - h/2.  The previous-frame snapshot (prev_x, prev_y) is shifted by
///   the same delta so the swept CCD broad-phase remains consistent after a
///   constraint solver correction.  Non-finite values are silently ignored.
/// @param b   Body to reposition (must not be NULL).
/// @param cx  Target center X in world space.
/// @param cy  Target center Y in world space.
static void body_set_center(rt_body_impl *b, double cx, double cy) {
    if (!b || !isfinite(cx) || !isfinite(cy))
        return;

    double old_x = b->x;
    double old_y = b->y;
    if (b->is_circle) {
        b->x = cx;
        b->y = cy;
    } else {
        b->x = cx - b->w * 0.5;
        b->y = cy - b->h * 0.5;
    }

    double dx = b->x - old_x;
    double dy = b->y - old_y;
    if (isfinite(dx))
        b->prev_x += dx;
    if (isfinite(dy))
        b->prev_y += dy;
}

/// @brief Allocate and zero-initialize a new joint object of the given type.
/// @details Validates both body pointers and rejects self-joints (body_a == body_b).
///   Allocates a GC-managed ph_joint block, retains both bodies so they cannot be
///   collected while the joint exists, and registers joint_finalizer to release
///   them on collection.  Subtype constructors fill in type-specific fields after
///   this returns.
/// @param type    One of RT_JOINT_DISTANCE, RT_JOINT_SPRING, RT_JOINT_HINGE,
///                RT_JOINT_ROPE.
/// @param body_a  First body participant; must be a valid Physics2D.Body handle.
/// @param body_b  Second body participant; must differ from body_a.
/// @return        New joint pointer, or NULL if inputs are invalid or allocation fails.
static ph_joint *alloc_joint(int32_t type, void *body_a, void *body_b) {
    if (!body_a || !body_b || body_a == body_b)
        return NULL;

    rt_body_impl *a = checked_body(body_a, "Physics2D.Joint: expected Physics2D.Body for body A");
    rt_body_impl *b = checked_body(body_b, "Physics2D.Joint: expected Physics2D.Body for body B");
    if (!a || !b)
        return NULL;

    ph_joint *j =
        (ph_joint *)rt_obj_new_i64(RT_PHYSICS2D_JOINT_CLASS_ID, (int64_t)sizeof(ph_joint));
    if (!j)
        return NULL;

    memset(j, 0, sizeof(ph_joint));
    j->vptr = NULL;
    j->type = type;
    j->body_a = a;
    j->body_b = b;
    j->anchor_x = 0.0;
    j->anchor_y = 0.0;
    j->length = 0.0;
    j->stiffness = 0.0;
    j->damping = 0.0;
    j->active = 1;
    rt_obj_retain_maybe(a);
    rt_obj_retain_maybe(b);
    rt_obj_set_finalizer(j, joint_finalizer);
    return j;
}

//=============================================================================
// Distance Joint
//=============================================================================

/// @brief Create a distance joint that holds bodies `a` and `b` exactly `length` units apart.
/// The constraint is solved via positional correction each step (rigid rod, no springiness).
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param length Requested nonnegative center-to-center rest length.
/// @return A new active joint handle retaining both bodies, or `NULL` for
///         invalid input or allocation failure.
void *rt_physics2d_distance_joint_new(void *body_a, void *body_b, double length) {
    ph_joint *j = alloc_joint(RT_JOINT_DISTANCE, body_a, body_b);
    if (!j)
        return NULL;
    j->length = nonnegative_finite_or_zero(length);
    return j;
}

/// @brief Return the rest length of a distance joint.
/// @details The constraint solver drives the separation between the two bodies'
///   centers of mass toward this value each step.
/// @param joint  Physics2D.DistanceJoint handle.
/// @return       Current rest length in world units, or 0.0 if @p joint is invalid.
double rt_physics2d_distance_joint_get_length(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.DistanceJoint.Length: expected Physics2D.Joint");
    return j ? j->length : 0.0;
}

/// @brief Set the rest length of a distance joint.
/// @details Negative values and non-finite values are clamped to 0.0 (bodies
///   pulled to the same center).  Takes effect on the next world step.
/// @param joint   Physics2D.DistanceJoint handle.
/// @param length  New rest length; must be >= 0.
void rt_physics2d_distance_joint_set_length(void *joint, double length) {
    ph_joint *j =
        checked_joint(joint, "Physics2D.DistanceJoint.Length.set: expected Physics2D.Joint");
    if (j)
        j->length = nonnegative_finite_or_zero(length);
}

//=============================================================================
// Spring Joint
//=============================================================================

/// @brief Create a spring joint between two bodies — applies Hooke's law force proportional to
/// `stiffness` × displacement from `rest_length`, with `damping` proportional to relative velocity.
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param rest_length Requested nonnegative center-to-center rest length.
/// @param stiffness Requested nonnegative Hooke coefficient.
/// @param damping Requested nonnegative axis-velocity damping coefficient.
/// @return A new active joint handle retaining both bodies, or `NULL` for
///         invalid input or allocation failure.
void *rt_physics2d_spring_joint_new(
    void *body_a, void *body_b, double rest_length, double stiffness, double damping) {
    ph_joint *j = alloc_joint(RT_JOINT_SPRING, body_a, body_b);
    if (!j)
        return NULL;
    j->length = nonnegative_finite_or_zero(rest_length);
    j->stiffness = nonnegative_finite_or_zero(stiffness);
    j->damping = nonnegative_finite_or_zero(damping);
    return j;
}

/// @brief Return the spring stiffness (Hooke's constant k) of the joint.
/// @details Higher values produce a stiffer spring that snaps back to rest length
///   faster but can become numerically unstable at very large values.
/// @param joint  Physics2D.SpringJoint handle.
/// @return       Current stiffness coefficient, or 0.0 if @p joint is invalid.
double rt_physics2d_spring_joint_get_stiffness(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.SpringJoint.Stiffness: expected Physics2D.Joint");
    return j ? j->stiffness : 0.0;
}

/// @brief Set the spring stiffness (Hooke's constant k) of the joint.
/// @details Negative and non-finite values are clamped to 0.0 (no restoring force).
///   Takes effect on the next world step.
/// @param joint      Physics2D.SpringJoint handle.
/// @param stiffness  New stiffness; must be >= 0.
void rt_physics2d_spring_joint_set_stiffness(void *joint, double stiffness) {
    ph_joint *j =
        checked_joint(joint, "Physics2D.SpringJoint.Stiffness.set: expected Physics2D.Joint");
    if (j)
        j->stiffness = nonnegative_finite_or_zero(stiffness);
}

/// @brief Return the damping coefficient of the spring joint.
/// @details Damping applies a force proportional to the relative velocity along
///   the spring axis (viscous damping), reducing oscillation.
/// @param joint  Physics2D.SpringJoint handle.
/// @return       Current damping coefficient, or 0.0 if @p joint is invalid.
double rt_physics2d_spring_joint_get_damping(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.SpringJoint.Damping: expected Physics2D.Joint");
    return j ? j->damping : 0.0;
}

/// @brief Set the damping coefficient of the spring joint.
/// @details Negative and non-finite values are clamped to 0.0 (undamped spring).
///   Takes effect on the next world step.
/// @param joint    Physics2D.SpringJoint handle.
/// @param damping  New damping coefficient; must be >= 0.
void rt_physics2d_spring_joint_set_damping(void *joint, double damping) {
    ph_joint *j =
        checked_joint(joint, "Physics2D.SpringJoint.Damping.set: expected Physics2D.Joint");
    if (j)
        j->damping = nonnegative_finite_or_zero(damping);
}

//=============================================================================
// Hinge Joint
//=============================================================================

/// @brief Create a hinge (pin) joint that locks two bodies to share a common world-space anchor
/// point (anchor_x, anchor_y).
/// @details Physics2D has no angular body state; the stored local center offsets
///          constrain linear motion so both endpoint anchor positions coincide.
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param anchor_x Initial world-space anchor X; non-finite input uses body A's center.
/// @param anchor_y Initial world-space anchor Y; non-finite input uses body A's center.
/// @return A new active joint handle retaining both bodies, or `NULL` for
///         invalid input or allocation failure.
void *rt_physics2d_hinge_joint_new(void *body_a, void *body_b, double anchor_x, double anchor_y) {
    ph_joint *j = alloc_joint(RT_JOINT_HINGE, body_a, body_b);
    if (!j)
        return NULL;
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    j->anchor_x = finite_or(anchor_x, body_cx(a));
    j->anchor_y = finite_or(anchor_y, body_cy(a));
    j->anchor_ax = j->anchor_x - body_cx(a);
    j->anchor_ay = j->anchor_y - body_cy(a);
    j->anchor_bx = j->anchor_x - body_cx(b);
    j->anchor_by = j->anchor_y - body_cy(b);
    return j;
}

/// @brief Return the current angle (in radians) from body A's center to body B's center.
/// @details Computed as atan2(dy, dx) where dx/dy is the vector from body_a's center
///   of mass to body_b's center of mass.  This is the instantaneous swing angle of
///   the hinge arm, useful for driving animations or applying angular limits in game code.
/// @param joint  Physics2D.HingeJoint handle.
/// @return       Angle in radians in the range [-π, +π], or 0.0 if @p joint is invalid.
double rt_physics2d_hinge_joint_get_angle(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.HingeJoint.Angle: expected Physics2D.Joint");
    if (!j)
        return 0.0;
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return 0.0;
    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    return atan2(dy, dx);
}

//=============================================================================
// Rope Joint
//=============================================================================

/// @brief Create a rope joint — distance constraint that's only active when bodies exceed
/// `max_length` apart. Bodies can be closer freely (chain/rope behavior, not rigid rod).
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param max_length Requested nonnegative maximum center separation.
/// @return A new active joint handle retaining both bodies, or `NULL` for
///         invalid input or allocation failure.
void *rt_physics2d_rope_joint_new(void *body_a, void *body_b, double max_length) {
    ph_joint *j = alloc_joint(RT_JOINT_ROPE, body_a, body_b);
    if (!j)
        return NULL;
    j->length = nonnegative_finite_or_zero(max_length);
    return j;
}

/// @brief Return the maximum allowed separation of a rope joint.
/// @details The constraint is only enforced when the bodies are further apart than
///   this value — bodies can be closer without constraint, like links in a chain.
/// @param joint  Physics2D.RopeJoint handle.
/// @return       Current maximum length in world units, or 0.0 if @p joint is invalid.
double rt_physics2d_rope_joint_get_max_length(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.RopeJoint.MaxLength: expected Physics2D.Joint");
    return j ? j->length : 0.0;
}

/// @brief Set the maximum allowed separation of a rope joint.
/// @details Negative and non-finite values are clamped to 0.0 (bodies always pulled
///   together).  Takes effect on the next world step.
/// @param joint       Physics2D.RopeJoint handle.
/// @param max_length  New maximum length; must be >= 0.
void rt_physics2d_rope_joint_set_max_length(void *joint, double max_length) {
    ph_joint *j =
        checked_joint(joint, "Physics2D.RopeJoint.MaxLength.set: expected Physics2D.Joint");
    if (j)
        j->length = nonnegative_finite_or_zero(max_length);
}

//=============================================================================
// Joint Common
//=============================================================================

/// @brief Borrow the first body referenced by the joint (caller must NOT release).
/// @param joint Opaque Physics2D.Joint handle.
/// @return The borrowed first body, or `NULL` for invalid input.
void *rt_physics2d_joint_get_body_a(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.Joint.BodyA: expected Physics2D.Joint");
    return j ? j->body_a : NULL;
}

/// @brief Borrow the second body referenced by the joint (caller must NOT release).
/// @param joint Opaque Physics2D.Joint handle.
/// @return The borrowed second body, or `NULL` for invalid input.
void *rt_physics2d_joint_get_body_b(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.Joint.BodyB: expected Physics2D.Joint");
    return j ? j->body_b : NULL;
}

/// @brief Return the joint type constant (one of RT_JOINT_DISTANCE, _SPRING, _HINGE, _ROPE).
/// @details Allows Zia/BASIC code to branch on joint type without knowing the internal
///   ph_joint layout.
/// @param joint  Any Physics2D joint handle.
/// @return       Type constant as int64, or -1 if @p joint is invalid.
int64_t rt_physics2d_joint_get_type(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.Joint.Type: expected Physics2D.Joint");
    return j ? j->type : -1;
}

/// @brief Return whether the joint is currently participating in constraint solving.
/// @details Constructors initialize the flag to active even before registration.
///   World removal clears it, and adding the joint to a world sets it again.
///   Solvers skip inactive registered entries.
/// @param joint  Any Physics2D joint handle.
/// @return       1 if active, 0 if inactive or @p joint is invalid.
int8_t rt_physics2d_joint_is_active(void *joint) {
    ph_joint *j = checked_joint(joint, "Physics2D.Joint.Active: expected Physics2D.Joint");
    return j ? j->active : 0;
}

//=============================================================================
// World Joint Management
//=============================================================================

/// @brief Return 1 if @p body is currently registered in world @p w, 0 otherwise.
/// @details Linear scan over w->bodies[0..body_count).  Called by
///   rt_physics2d_world_add_joint to enforce the invariant that both joint bodies
///   must belong to the same world before a joint can be added.
/// @param w Borrowed world implementation to inspect.
/// @param body Borrowed body pointer to locate.
/// @return Nonzero when the body is registered in @p w, otherwise zero.
static int8_t world_has_body(rt_world_impl *w, void *body) {
    if (!w || !body)
        return 0;
    rt_body_impl *bd = (rt_body_impl *)body;
    /* O(1) for the common single-world case: owner_world == w is a definitive yes,
     * owner_world == NULL a definitive no. Only a body juggled between worlds needs
     * the linear scan. */
    if (bd->owner_world == w)
        return 1;
    if (bd->owner_world == NULL)
        return 0;
    for (int64_t i = 0; i < w->body_count; i++) {
        if (w->bodies[i] == bd)
            return 1;
    }
    return 0;
}

/// @brief Register a joint with a world so it participates in constraint solving.
/// @details Validates that both of the joint's bodies already belong to @p world
///   (traps if not), prevents duplicate registration, and enforces the
///   body-membership invariant. Grows world joint storage as needed and retains @p joint so the GC
///   cannot collect it while it is registered.  Sets the joint active flag to 1.
/// @param world  Physics2D.World handle.
/// @param joint  Physics2D joint handle to register.
void rt_physics2d_world_add_joint(void *world, void *joint) {
    if (!world || !joint)
        return;
    rt_world_impl *w = checked_world(world, "Physics2D.World.AddJoint: expected Physics2D.World");
    ph_joint *j = checked_joint(joint, "Physics2D.World.AddJoint: expected Physics2D.Joint");
    if (!w || !j)
        return;
    for (int64_t i = 0; i < w->joint_count; i++) {
        if (w->joints[i] == j)
            return;
    }
    if (!world_has_body(w, j->body_a) || !world_has_body(w, j->body_b)) {
        rt_trap(
            "Physics2D.World.AddJoint: both joint bodies must be added to the same world first");
        return;
    }
    if (!rt_physics2d_world_reserve_joint_capacity(w, w->joint_count + 1)) {
        rt_trap("Physics2D.World.AddJoint: joint storage allocation failed");
        return;
    }
    j->active = 1;
    rt_obj_retain_maybe(joint);
    w->joints[w->joint_count++] = j;
}

/// @brief Unregister a joint from a world and stop constraint solving for it.
/// @details Marks the joint inactive, releases the world's retained reference
///   (freeing it if the count reaches zero), and fills the vacated slot with
///   the last entry to preserve a packed joints array without shifting.
/// @param world  Physics2D.World handle.
/// @param joint  Physics2D joint handle to unregister; silently ignored if not found.
void rt_physics2d_world_remove_joint(void *world, void *joint) {
    if (!world || !joint)
        return;
    rt_world_impl *w =
        checked_world(world, "Physics2D.World.RemoveJoint: expected Physics2D.World");
    ph_joint *j = checked_joint(joint, "Physics2D.World.RemoveJoint: expected Physics2D.Joint");
    if (!w || !j)
        return;
    for (int64_t i = 0; i < w->joint_count; i++) {
        if (w->joints[i] == j) {
            j->active = 0;
            if (rt_obj_release_check0(j))
                rt_obj_free(j);
            w->joints[i] = w->joints[w->joint_count - 1];
            w->joints[w->joint_count - 1] = NULL;
            w->joint_count--;
            return;
        }
    }
}

/// @brief Return the number of joints currently registered in the world.
/// @param world  Physics2D.World handle.
/// @return       Active joint count, or 0 if @p world is invalid.
int64_t rt_physics2d_world_joint_count(void *world) {
    rt_world_impl *w = checked_world(world, "Physics2D.World.JointCount: expected Physics2D.World");
    return w ? w->joint_count : 0;
}

//=============================================================================
// Constraint Solver
//=============================================================================

/// @brief Apply one Gauss-Seidel positional correction pass for a distance joint.
/// @details Computes the separation vector between the two bodies' centers of mass,
///   determines how much each body must move to eliminate the error
///   (dist - j->length), and weights the correction by each body's inverse mass so
///   heavier bodies move less.  When bodies are coincident (dist < 1e-8) and the
///   rest length is also zero the constraint is already satisfied; otherwise an
///   arbitrary direction (1, 0) is used to avoid a divide-by-zero.
/// @param j   The distance joint to solve.
/// @param dt  Time step (unused — positional correction is time-independent).
static void solve_distance(ph_joint *j, double dt) {
    (void)dt;
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;

    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;

    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    double dist = sqrt(dx * dx + dy * dy);
    double nx = 1.0;
    double ny = 0.0;
    if (dist >= 1e-8) {
        nx = dx / dist;
        ny = dy / dist;
    } else if (j->length <= 0.0) {
        return;
    } else {
        dist = 0.0;
    }

    double error = dist - j->length;
    double cx_a = nx * error * (a->inv_mass / total_inv);
    double cy_a = ny * error * (a->inv_mass / total_inv);
    double cx_b = nx * error * (b->inv_mass / total_inv);
    double cy_b = ny * error * (b->inv_mass / total_inv);

    body_set_center(a, body_cx(a) + cx_a, body_cy(a) + cy_a);
    body_set_center(b, body_cx(b) - cx_b, body_cy(b) - cy_b);
}

/// @brief Apply one Hooke's-law impulse pass for a spring joint.
/// @details Computes the spring force F = k*(dist - rest) + d*relVel along the
///   axis between body centers, then converts it to a velocity impulse
///   dv = F * inv_mass * dt applied symmetrically (+=A, -=B).  Bodies with zero
///   inverse mass (infinite mass / static) receive no velocity change.  Skipped
///   entirely when dt <= 0 or the bodies are coincident (dist < 1e-8).
/// @param j   The spring joint to solve.
/// @param dt  Time step in seconds; controls impulse magnitude.
static void solve_spring(ph_joint *j, double dt) {
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b || dt <= 0.0)
        return;

    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    double dist = sqrt(dx * dx + dy * dy);
    if (dist < 1e-8)
        return;

    double nx = dx / dist;
    double ny = dy / dist;

    // Spring force: F = -k * (dist - rest) - d * relVel
    double stretch = dist - j->length;
    double rel_vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    double force = j->stiffness * stretch + j->damping * rel_vn;

    // Stability clamp. Explicit-Euler spring integration diverges once a single
    // step overshoots the rest length (the classic k > 4m/dt^2 blow-up). Cap the
    // relative-velocity change this force would produce so the spring can at most
    // cancel the current relative normal velocity and close the remaining stretch
    // within this step — never inject more. For normal frame steps |stretch|/dt is
    // large, so the clamp never engages and behaviour is bit-identical to the raw
    // Hooke integration; it only bounds the impulse on large (post-hitch) dt.
    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;
    double d_rel_v = force * dt * total_inv; // magnitude of induced |Δ(rel_vn)|
    double max_d_rel_v = fabs(rel_vn) + fabs(stretch) / dt;
    if (fabs(d_rel_v) > max_d_rel_v && fabs(d_rel_v) > 0.0)
        force *= max_d_rel_v / fabs(d_rel_v);

    double fx = force * nx;
    double fy = force * ny;

    if (a->inv_mass > 0.0) {
        a->vx += fx * a->inv_mass * dt;
        a->vy += fy * a->inv_mass * dt;
    }
    if (b->inv_mass > 0.0) {
        b->vx -= fx * b->inv_mass * dt;
        b->vy -= fy * b->inv_mass * dt;
    }
}

/// @brief Apply one positional correction pass for a hinge (pin) joint.
/// @details Each body stores local-space offsets (anchor_ax/ay, anchor_bx/by)
///   from its center to the shared anchor point.  This function computes where
///   each body thinks the anchor is in world space, calculates the gap between
///   the two world-space anchor points, and corrects both body centers by a
///   mass-weighted fraction of that gap.  The shared anchor position is updated
///   to the midpoint of the corrected locations for visual tracking purposes.
/// @param j   The hinge joint to solve.
/// @param dt  Time step (unused — positional correction is time-independent).
static void solve_hinge(ph_joint *j, double dt) {
    (void)dt;
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;

    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;

    double acx = body_cx(a), acy = body_cy(a);
    double bcx = body_cx(b), bcy = body_cy(b);
    double a_anchor_x = acx + j->anchor_ax;
    double a_anchor_y = acy + j->anchor_ay;
    double b_anchor_x = bcx + j->anchor_bx;
    double b_anchor_y = bcy + j->anchor_by;
    double dx = b_anchor_x - a_anchor_x;
    double dy = b_anchor_y - a_anchor_y;

    body_set_center(a, acx + dx * (a->inv_mass / total_inv), acy + dy * (a->inv_mass / total_inv));
    body_set_center(b, bcx - dx * (b->inv_mass / total_inv), bcy - dy * (b->inv_mass / total_inv));
    j->anchor_x = (a_anchor_x + b_anchor_x) * 0.5;
    j->anchor_y = (a_anchor_y + b_anchor_y) * 0.5;
}

/// @brief Apply one positional correction pass for a rope joint.
/// @details Unlike the distance joint, the rope only resists extension: if the
///   current separation is within max_length the function returns immediately.
///   When taut, the overlap fraction (dist - max_length)/dist is used to compute
///   a mass-weighted positional pull that brings the bodies back within the limit.
/// @param j   The rope joint to solve.
/// @param dt  Time step (unused — positional correction is time-independent).
static void solve_rope(ph_joint *j, double dt) {
    (void)dt;
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;

    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    double dist = sqrt(dx * dx + dy * dy);

    // Rope only constrains when taut (distance > max_length)
    if (dist <= j->length)
        return;

    double diff = (dist - j->length) / dist;
    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;

    double cx_a = dx * diff * (a->inv_mass / total_inv);
    double cy_a = dy * diff * (a->inv_mass / total_inv);
    double cx_b = dx * diff * (b->inv_mass / total_inv);
    double cy_b = dy * diff * (b->inv_mass / total_inv);

    body_set_center(a, body_cx(a) + cx_a, body_cy(a) + cy_a);
    body_set_center(b, body_cx(b) - cx_b, body_cy(b) - cy_b);
}

/// @brief Cancel the relative velocity along a distance joint's axis (rigid rod).
/// @details The positional solvers (solve_distance/hinge/rope) only project
///   positions; they never touch velocity, and body_set_center shifts prev_x/prev_y
///   by the same delta so the correction is invisible to velocity as well. Without
///   this pass a body hanging on a distance joint under gravity accumulates the
///   uncorrected radial velocity every step (vy += g*dt forever) — "phantom
///   velocity" that balloons its swept AABB and launches it if the joint is
///   removed. Removing the constraint-axis relative velocity (an e=0 normal impulse
///   along the joint axis) is the velocity half of position-based dynamics.
/// @param j Borrowed active distance joint to project.
static void solve_distance_velocity(ph_joint *j) {
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;
    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;
    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    double dist = sqrt(dx * dx + dy * dy);
    if (dist < 1e-8)
        return;
    double nx = dx / dist;
    double ny = dy / dist;
    double rel_vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    double jn = -rel_vn / total_inv;
    a->vx -= jn * a->inv_mass * nx;
    a->vy -= jn * a->inv_mass * ny;
    b->vx += jn * b->inv_mass * nx;
    b->vy += jn * b->inv_mass * ny;
}

/// @brief Cancel the full relative velocity at a hinge's shared anchor.
/// @details These bodies carry no angular state, so a hinge pins their motion
///   together at the anchor: the correct velocity constraint zeroes the relative
///   linear velocity, mass-weighted. Mirrors solve_distance_velocity for the pin
///   case and prevents the same phantom-velocity accumulation.
/// @param j Borrowed active hinge joint to project.
static void solve_hinge_velocity(ph_joint *j) {
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;
    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;
    double jx = -(b->vx - a->vx) / total_inv;
    double jy = -(b->vy - a->vy) / total_inv;
    a->vx -= jx * a->inv_mass;
    a->vy -= jy * a->inv_mass;
    b->vx += jx * b->inv_mass;
    b->vy += jy * b->inv_mass;
}

/// @brief Cancel only the separating velocity of a rope joint when it is taut.
/// @details A rope resists extension but not approach, so this removes the
///   relative velocity along the axis only when the bodies are at/over max length
///   and still separating (rel_vn > 0). Approaching bodies keep their velocity so
///   the rope can go slack, matching solve_rope's position behaviour.
/// @param j Borrowed active rope joint to project.
static void solve_rope_velocity(ph_joint *j) {
    rt_body_impl *a = (rt_body_impl *)j->body_a;
    rt_body_impl *b = (rt_body_impl *)j->body_b;
    if (!a || !b)
        return;
    double total_inv = a->inv_mass + b->inv_mass;
    if (total_inv < 1e-12)
        return;
    double dx = body_cx(b) - body_cx(a);
    double dy = body_cy(b) - body_cy(a);
    double dist = sqrt(dx * dx + dy * dy);
    if (dist <= j->length || dist < 1e-8)
        return;
    double nx = dx / dist;
    double ny = dy / dist;
    double rel_vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    if (rel_vn <= 0.0)
        return; /* approaching — rope allows it to slacken */
    double jn = -rel_vn / total_inv;
    a->vx -= jn * a->inv_mass * nx;
    a->vy -= jn * a->inv_mass * ny;
    b->vx += jn * b->inv_mass * nx;
    b->vy += jn * b->inv_mass * ny;
}

/// @brief Velocity-projection pass for positional joints, run once per step after
///        the positional (Gauss-Seidel) correction.
/// @details Distance/hinge/rope joints correct position only; this pass removes the
///   residual constraint-axis relative velocity so it cannot accumulate across
///   steps. Spring joints are velocity-based already and are skipped here.
/// @param world  Physics2D.World handle.
/// @param dt     Time step (unused; velocity projection is time-independent).
void rt_physics2d_solve_joint_velocities(void *world, double dt) {
    (void)dt;
    rt_world_impl *w = checked_world(world, "Physics2D.World: expected Physics2D.World");
    if (!w)
        return;

    for (int64_t i = 0; i < w->joint_count; i++) {
        ph_joint *j = w->joints[i];
        if (!j || !j->active)
            continue;
        switch (j->type) {
            case RT_JOINT_DISTANCE:
                solve_distance_velocity(j);
                break;
            case RT_JOINT_HINGE:
                solve_hinge_velocity(j);
                break;
            case RT_JOINT_ROPE:
                solve_rope_velocity(j);
                break;
            case RT_JOINT_SPRING:
                break;
        }
    }
}

/// @brief Iterate all spring joints in the world and apply velocity impulses.
/// @details Called once per world step before positional correction so that the
///   velocity changes produced by Hooke's law can be integrated by the main
///   symplectic-Euler loop before positions are locked down by solve_distance /
///   solve_hinge / solve_rope.
/// @param world  Physics2D.World handle.
/// @param dt     Time step in seconds passed through to solve_spring.
void rt_physics2d_solve_spring_joints(void *world, double dt) {
    rt_world_impl *w = checked_world(world, "Physics2D.World: expected Physics2D.World");
    if (!w)
        return;

    for (int64_t i = 0; i < w->joint_count; i++) {
        ph_joint *j = w->joints[i];
        if (!j || !j->active || j->type != RT_JOINT_SPRING)
            continue;
        solve_spring(j, dt);
    }
}

/// @brief Run PH_JOINT_ITERATIONS Gauss-Seidel passes for all positional joints.
/// @details Iterates over distance, hinge, and rope joints (spring joints are
///   velocity-based and handled separately).  Multiple iterations converge the
///   positional error for systems with chained constraints (ragdolls, ropes made
///   of segments), at the cost of O(iterations × joints) work per step.
/// @param world  Physics2D.World handle.
/// @param dt     Time step in seconds passed through to individual solvers.
void rt_physics2d_solve_position_joints(void *world, double dt) {
    rt_world_impl *w = checked_world(world, "Physics2D.World: expected Physics2D.World");
    if (!w)
        return;

    for (int iter = 0; iter < PH_JOINT_ITERATIONS; iter++) {
        for (int64_t i = 0; i < w->joint_count; i++) {
            ph_joint *j = w->joints[i];
            if (!j || !j->active)
                continue;

            switch (j->type) {
                case RT_JOINT_DISTANCE:
                    solve_distance(j, dt);
                    break;
                case RT_JOINT_SPRING:
                    break;
                case RT_JOINT_HINGE:
                    solve_hinge(j, dt);
                    break;
                case RT_JOINT_ROPE:
                    solve_rope(j, dt);
                    break;
            }
        }
    }
}

/// @brief Run the combined spring and positional joint passes.
/// @details This compatibility wrapper applies spring velocity impulses and
///   then distance/hinge/rope position correction. It does not perform the
///   separate post-position velocity-projection pass; the main world integrator
///   invokes the phase-specific entry points at their required stages.
/// @param world  Physics2D.World handle.
/// @param dt     Time step in seconds.
void rt_physics2d_solve_joints(void *world, double dt) {
    rt_physics2d_solve_spring_joints(world, dt);
    rt_physics2d_solve_position_joints(world, dt);
}

//=============================================================================
// Circle Bodies
//=============================================================================

/// @brief Construct a circle-shaped rigid body centered at (cx, cy) with `radius` and `mass`.
/// Otherwise behaves like `_body_new` (default restitution 0.5, friction 0.3, layer 1, mask
/// 0xFF...).
/// @param cx Initial center X; non-finite input becomes zero.
/// @param cy Initial center Y; non-finite input becomes zero.
/// @param radius Positive circle radius; invalid values become one.
/// @param mass Positive dynamic mass, or a nonpositive/non-finite value for a
///             static circle.
/// @return A new circle-body handle, or `NULL` if allocation fails.
void *rt_physics2d_circle_body_new(double cx, double cy, double radius, double mass) {
    cx = finite_or(cx, 0.0);
    cy = finite_or(cy, 0.0);
    if (!isfinite(radius) || radius <= 0.0)
        radius = 1.0;
    mass = (isfinite(mass) && mass > 0.0) ? mass : 0.0;

    rt_body_impl *b =
        (rt_body_impl *)rt_obj_new_i64(RT_PHYSICS2D_BODY_CLASS_ID, (int64_t)sizeof(rt_body_impl));
    if (!b)
        return NULL;

    b->vptr = NULL;
    b->x = cx; // For circles, x/y is center
    b->y = cy;
    b->prev_x = cx;
    b->prev_y = cy;
    b->w = 0.0;
    b->h = 0.0;
    b->vx = 0.0;
    b->vy = 0.0;
    b->fx = 0.0;
    b->fy = 0.0;
    b->mass = mass;
    b->inv_mass = (mass > 0.0) ? (1.0 / mass) : 0.0;
    b->restitution = 0.5;
    b->friction = 0.3;
    b->collision_layer = 1;
    b->collision_mask = INT64_C(-1);
    b->radius = radius;
    b->is_circle = 1;
    b->owner_world = NULL;
    b->world_index = -1;
    return b;
}

/// @brief Return the radius of a circle body.
/// @details Only meaningful for bodies created with rt_physics2d_circle_body_new.
///   AABB bodies always store radius == 0.0.
/// @param body  Physics2D.Body handle.
/// @return      Radius in world units, or 0.0 if @p body is invalid or is an AABB.
double rt_physics2d_body_radius(void *body) {
    rt_body_impl *b = checked_body(body, "Physics2D.Body.Radius: expected Physics2D.Body");
    return b ? b->radius : 0.0;
}

/// @brief Return whether a body uses circle collision geometry.
/// @details Circle bodies store their center directly in x,y and use radius for
///   overlap tests.  AABB bodies store the top-left corner plus width/height.
///   This flag is set at creation time and never changes.
/// @param body  Physics2D.Body handle.
/// @return      1 if the body is a circle, 0 if AABB or @p body is invalid.
int8_t rt_physics2d_body_is_circle(void *body) {
    rt_body_impl *b = checked_body(body, "Physics2D.Body.IsCircle: expected Physics2D.Body");
    return b ? b->is_circle : 0;
}
