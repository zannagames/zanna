//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d.c
// Purpose: 3D physics world — AABB/sphere/capsule bodies, symplectic Euler
//   integration, impulse-based collision response with Baumgarte correction,
//   bidirectional layer/mask filtering, and character controller.
//
// Key invariants:
//   - Bodies in topological order not required (flat array, sweep-and-prune broad phase).
//   - Integration: forces→velocity, velocity→position (symplectic Euler).
//   - Collision response: contact-point impulse updates linear and angular velocity.
//   - Baumgarte stabilization: per-world recovery fraction with fixed penetration slop.
//   - Character controller: up to 3 slide iterations per move.
//   - Quaternion orientations are renormalized after every integration step.
//   - Sweep-and-prune broad-phase sorts entries by min-X for early-out.
//
// Ownership/Lifetime:
//   - World3D / Body3D / Character3D / Trigger3D are GC-managed.
//   - World3D retains its bodies and joints; finalizer releases each.
//   - Boxed query results (PhysicsHit3D / PhysicsHitList3D / CollisionEvent3D)
//     retain referenced bodies and colliders until released by the GC.
//   - Trigger3D holds tracked bodies as weak pointers — stale entries are
//     pruned during the next Update.
//
// Links: rt_physics3d.h, rt_raycast3d.h, plans/3d/20-phase-a-core-game-systems.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d.c
 * @brief Assembles the Physics3D world, body, contact, query, and event implementation.
 *
 * This translation unit defines shared validation, allocation, state-sanitizing,
 * broadphase invalidation, fixed-step, and joint-wake helpers before including
 * the focused detection, world, event, and body implementation fragments. The
 * world owns registered bodies, joints, contact history, query caches, and
 * reusable step scratch while transactional stepping preserves published state
 * across internal failures.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_physics3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_joints3d.h"
#include "rt_joints3d_internal.h"
#include "rt_platform.h"
#include "rt_raycast3d.h"
#include "rt_threadpool.h"

#include "rt_physics3d_internal.h"
#include "rt_physics3d_query_internal.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
#include <float.h>
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);

int ph3d_i32_stack_push(int32_t **items, int32_t *count, int32_t *capacity, int32_t value);

#define PH3D_STATE_ABS_MAX 1000000000000.0
#define PH3D_PARAM_ABS_MAX 1000000000.0
#define PH3D_STEP_DT_MAX 1.0
#define PH3D_EVENT_DIFF_FALLBACK_PAIR_LIMIT 4194304LL

static int8_t g_ph3d_test_force_broadphase_alloc_failure = 0;
static int8_t g_ph3d_test_force_step_failure = 0;

/// @brief Enable deterministic broadphase allocation failure for internal tests.
/// @details The hook affects both simulation and query broadphase reserves and
///   is never exposed through the public runtime registry.
/// @param enabled Nonzero to force subsequent broadphase reserve attempts to fail.
void rt_world3d_test_set_broadphase_alloc_failure(int8_t enabled) {
    g_ph3d_test_force_broadphase_alloc_failure = enabled ? 1 : 0;
}

/// @brief Enable deterministic failure after integration for transaction tests.
/// @details The step boundary observes this flag only after body state has
///   changed, ensuring tests exercise restoration rather than an early no-op.
/// @param enabled Nonzero to request failure at the post-integration transaction checkpoint.
void rt_world3d_test_set_step_failure(int8_t enabled) {
    g_ph3d_test_force_step_failure = enabled ? 1 : 0;
}

/// @brief Return whether the internal post-integration failure hook is enabled.
/// @return Non-zero when the next active step must take its rollback path.
int world3d_test_step_failure_requested(void) {
    return g_ph3d_test_force_step_failure != 0;
}

/// @brief Record a step failure while preserving the active transaction boundary.
/// @details Deep collision/solver helpers historically trapped immediately,
///   which bypassed rollback. During a transactional step this function stores
///   only the first static diagnostic; the outer public wrapper traps after it
///   restores all body/contact/event state. Calls outside Step still trap now.
/// @param w Borrowed world whose transaction state is inspected.
/// @param message Static diagnostic text; a stable fallback is used for `NULL`.
void world3d_step_fail(rt_world3d *w, const char *message) {
    const char *safe_message = message ? message : "Physics3D.World.Step: internal failure";
    if (w && w->step_transaction_active) {
        if (!w->step_failure_message)
            w->step_failure_message = safe_message;
        return;
    }
    rt_trap(safe_message);
}

/// @brief Record one broadphase stack-fallback without leaking rolled-back telemetry.
/// @details Active transactions defer both the world counter and global Game3D
///   diagnostic. Non-transactional callers retain the historical immediate
///   behavior. Pending count saturates instead of overflowing corrupt state.
/// @param w World receiving immediate or transaction-deferred telemetry.
void world3d_note_broadphase_fallback(rt_world3d *w) {
    if (!w)
        return;
    if (w->step_transaction_active) {
        if (w->step_pending_broadphase_fallbacks < INT32_MAX)
            w->step_pending_broadphase_fallbacks++;
        return;
    }
    if (w->broadphase_fallback_count < INT64_MAX)
        w->broadphase_fallback_count++;
    rt_game3d_diag_record_broadphase_fallback();
}

/// @brief Commit deferred broadphase-fallback diagnostics after a successful step.
/// @details Adds pending events to the persistent counter with saturation, then
///   mirrors each event to Game3D diagnostics. The current step performs at most
///   one detect pass, but the loop keeps the helper correct if that changes.
/// @param w World whose pending transaction-local fallback count is committed.
void world3d_commit_broadphase_fallbacks(rt_world3d *w) {
    int32_t pending;
    if (!w)
        return;
    pending = w->step_pending_broadphase_fallbacks;
    w->step_pending_broadphase_fallbacks = 0;
    if (pending <= 0)
        return;
    if (w->broadphase_fallback_count <= INT64_MAX - pending)
        w->broadphase_fallback_count += pending;
    else
        w->broadphase_fallback_count = INT64_MAX;
    for (int32_t i = 0; i < pending; ++i)
        rt_game3d_diag_record_broadphase_fallback();
}

/* The joint solver (rt_joints3d.c) reaches into a body's pose/velocity through
 * the private shared rt_body3d_kinematics view. These asserts
 * pin the contract: if any shared field is reordered or retyped in rt_body3d, the
 * build fails here instead of silently corrupting joint solving at runtime. */
_Static_assert(offsetof(rt_body3d, vptr) == offsetof(rt_body3d_kinematics, vptr),
               "rt_body3d_kinematics.vptr offset drift");
_Static_assert(offsetof(rt_body3d, position) == offsetof(rt_body3d_kinematics, position),
               "rt_body3d_kinematics.position offset drift");
_Static_assert(offsetof(rt_body3d, orientation) == offsetof(rt_body3d_kinematics, orientation),
               "rt_body3d_kinematics.orientation offset drift");
_Static_assert(offsetof(rt_body3d, scale) == offsetof(rt_body3d_kinematics, scale),
               "rt_body3d_kinematics.scale offset drift");
_Static_assert(offsetof(rt_body3d, velocity) == offsetof(rt_body3d_kinematics, velocity),
               "rt_body3d_kinematics.velocity offset drift");
_Static_assert(offsetof(rt_body3d, angular_velocity) ==
                   offsetof(rt_body3d_kinematics, angular_velocity),
               "rt_body3d_kinematics.angular_velocity offset drift");
_Static_assert(offsetof(rt_body3d, force) == offsetof(rt_body3d_kinematics, force),
               "rt_body3d_kinematics.force offset drift");
_Static_assert(offsetof(rt_body3d, torque) == offsetof(rt_body3d_kinematics, torque),
               "rt_body3d_kinematics.torque offset drift");
_Static_assert(offsetof(rt_body3d, mass) == offsetof(rt_body3d_kinematics, mass),
               "rt_body3d_kinematics.mass offset drift");
_Static_assert(offsetof(rt_body3d, inv_mass) == offsetof(rt_body3d_kinematics, inv_mass),
               "rt_body3d_kinematics.inv_mass offset drift");
_Static_assert(offsetof(rt_body3d, collider) == offsetof(rt_body3d_kinematics, collider),
               "rt_body3d_kinematics.collider offset drift");
_Static_assert(offsetof(rt_body3d, inv_inertia) == offsetof(rt_body3d_kinematics, inv_inertia),
               "rt_body3d_kinematics.inv_inertia offset drift");

/// @brief Validate @p obj as a World3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed World3D payload, or NULL on class mismatch.
rt_world3d *world3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_WORLD3D_CLASS_ID, sizeof(rt_world3d)) ? (rt_world3d *)obj
                                                                                : NULL;
}

/// @brief Validate @p obj as a Body3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed Body3D payload, or NULL on class mismatch.
static rt_body3d *body3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_BODY3D_CLASS_ID, sizeof(rt_body3d)) ? (rt_body3d *)obj
                                                                              : NULL;
}

/// @brief Return the body's Collider3D slot only when it still has the expected class.
/// @param body Body3D payload to inspect.
/// @return Borrowed Collider3D handle, or NULL when absent or invalid.
static void *body3d_collider_ref(const rt_body3d *body) {
    return body ? rt_g3d_checked_or_null(body->collider, RT_G3D_COLLIDER3D_CLASS_ID) : NULL;
}

/// @brief Advance a non-zero world broadphase revision, wrapping past UINT64_MAX to 1.
/// @details Query broadphase cache validation compares this revision in O(1) instead of hashing
///   every body on every query. Zero is reserved as an invalid/no-cache sentinel.
/// @param current Current revision value.
/// @return Next nonzero revision with defined wraparound.
static uint64_t world3d_next_broadphase_revision(uint64_t current) {
    return current == UINT64_MAX ? 1u : current + 1u;
}

/// @brief Mark all world-level broadphase caches stale after a body set or body bounds change.
/// @details Bodies still keep their own revision for debugging and future fine-grained caches, but
///   the world revision is the fast invalidation key used by spatial queries.
/// @param world World whose revision and query-cache state are invalidated.
static void world3d_bump_broadphase_revision(rt_world3d *world) {
    if (!world)
        return;
    world->broadphase_world_revision =
        world3d_next_broadphase_revision(world->broadphase_world_revision);
    world->query_broadphase_valid = 0;
    world->query_broadphase_count = 0;
    world->query_broadphase_signature = 0;
}

/// @brief Bump a body's broadphase revision (wrapping past UINT64_MAX to 1) so cached broadphase
///        structures know to re-evaluate it.
/// @param body Body whose shape, filter, membership, or bounds-affecting state changed.
void body3d_touch_broadphase(rt_body3d *body) {
    if (!body)
        return;
    body->broadphase_revision =
        body->broadphase_revision == UINT64_MAX ? 1u : body->broadphase_revision + 1u;
    world3d_bump_broadphase_revision(body->owner_world);
}

/// @brief Record a pose-only transform change without invalidating the query broadphase.
/// @details Cheap enough for solver position-correction hot paths: bumps the body's own
///   revision and flags the world dirty. The next query build runs a lazy escape check —
///   moved bodies still inside their cached fattened AABBs keep the cache valid (fat
///   entries are a conservative candidate filter; narrow phase tests live body state),
///   and only a body escaping its fat bounds forces a full rebuild. Shape, filter, and
///   membership changes must keep using body3d_touch_broadphase / the explicit
///   invalidate so the entry set itself is refreshed.
/// @param body Body whose pose changed without changing broadphase membership semantics.
void body3d_touch_broadphase_moved(rt_body3d *body) {
    if (!body)
        return;
    body->broadphase_revision =
        body->broadphase_revision == UINT64_MAX ? 1u : body->broadphase_revision + 1u;
    if (body->owner_world)
        body->owner_world->query_broadphase_dirty = 1;
}

/// @brief Invalidate the world's cached query broadphase so the next spatial query rebuilds it.
/// @param world World whose query cache is invalidated.
static void world3d_invalidate_query_broadphase(rt_world3d *world) {
    world3d_bump_broadphase_revision(world);
}

/// @brief Verify that @p joint is a live joint of the kind named by @p joint_type.
/// @details Used by World3D before dispatching joint solving to confirm the table
///   entry hasn't been swapped out from under the cached type tag.
/// @param joint Candidate concrete joint handle.
/// @param joint_type Expected `RT_JOINT_*` discriminator.
/// @return One when the runtime class matches the requested discriminator, otherwise zero.
static int joint3d_matches_type(void *joint, int64_t joint_type) {
    if (!joint)
        return 0;
    if (joint_type == RT_JOINT_DISTANCE)
        return rt_g3d_has_class(joint, RT_G3D_DISTANCEJOINT3D_CLASS_ID);
    if (joint_type == RT_JOINT_SPRING)
        return rt_g3d_has_class(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    if (joint_type == RT_JOINT_HINGE)
        return rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID);
    if (joint_type == RT_JOINT_ROPE)
        return rt_g3d_has_class(joint, RT_G3D_ROPEJOINT3D_CLASS_ID);
    if (joint_type == RT_JOINT_SIXDOF)
        return rt_g3d_has_class(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
    return 0;
}

/// @brief True when @p b can drive a joint this substep: an awake dynamic body,
///   or a kinematic body with nonzero commanded velocity. Static anchors and
///   sleeping dynamics can't initiate constraint motion.
/// @param b Body3D payload to classify.
/// @return One for an active driver, otherwise zero.
static int body3d_is_joint_driver(const rt_body3d *b) {
    if (!b)
        return 0;
    if (b->motion_mode == PH3D_MODE_DYNAMIC)
        return !b->is_sleeping;
    if (b->motion_mode == PH3D_MODE_KINEMATIC) {
        return b->velocity[0] != 0.0 || b->velocity[1] != 0.0 || b->velocity[2] != 0.0 ||
               b->angular_velocity[0] != 0.0 || b->angular_velocity[1] != 0.0 ||
               b->angular_velocity[2] != 0.0;
    }
    return 0;
}

/// @brief Joint-solver bridge: skip fully-at-rest pairs, wake active ones.
/// @details The kinematics views are prefix views of live rt_body3d payloads
///   (layout pinned by the static asserts above), so the casts are exact.
///   Without this gate a sleeping jointed body silently discarded every
///   spring/hinge impulse (the integrator skips sleeping bodies) and
///   position-correcting joints teleported sleeping bodies without waking them.
/// @param body_a First joint endpoint.
/// @param body_b Second joint endpoint.
/// @return One when either endpoint can drive the joint and the pair is awakened, otherwise zero.
int joint3d_pair_begin_solve(rt_body3d_kinematics *body_a, rt_body3d_kinematics *body_b) {
    rt_body3d *a = (rt_body3d *)body_a;
    rt_body3d *b = (rt_body3d *)body_b;
    if (!body3d_is_joint_driver(a) && !body3d_is_joint_driver(b))
        return 0;
    body3d_wake_if_dynamic(a);
    body3d_wake_if_dynamic(b);
    return 1;
}

/// @brief Joint-solver bridge: pose-only broadphase touch after a joint moved a body.
/// @param body Body3D kinematic prefix whose containing payload moved.
void joint3d_mark_body_moved(rt_body3d_kinematics *body) {
    body3d_touch_broadphase_moved((rt_body3d *)body);
}

static int32_t world3d_joint_count_safe(const rt_world3d *w);

/// @brief Wake every dynamic body joined to @p body by any joint in @p w.
/// @details Called from explicit pose writes (SetPosition/SetOrientation): a
///   teleported static or kinematic anchor must re-activate sleeping partners
///   so the constraint re-solves against the new pose. O(joint_count), only on
///   user action.
/// @param w World containing the retained joint table.
/// @param body Body whose directly connected dynamic partners should wake.
static void world3d_wake_joint_partners(rt_world3d *w, const rt_body3d *body) {
    if (!w || !body)
        return;
    int32_t joint_count = world3d_joint_count_safe(w);
    for (int32_t i = 0; i < joint_count; i++) {
        void *ja = NULL;
        void *jb = NULL;
        if (!w->joints[i] ||
            !rt_joint3d_get_bodies(w->joints[i], (int32_t)w->joint_types[i], &ja, &jb))
            continue;
        if ((const rt_body3d *)ja == body)
            body3d_wake_if_dynamic((rt_body3d *)jb);
        else if ((const rt_body3d *)jb == body)
            body3d_wake_if_dynamic((rt_body3d *)ja);
    }
}

/// @brief Return non-zero only when every component of @p v is finite.
/// @param v Three-component vector.
/// @return One when @p v is non-null and finite, otherwise zero.
int ph3d_vec3_all_finite(const double v[3]) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/// @brief Compute the next power-of-two capacity that covers `needed`, starting from
///        `current` (or `initial` if current is zero).
/// @details Doubles until the result covers `needed`. Returns -1 if the next doubling
///          would overflow `INT32_MAX`, so callers can fail the reserve cleanly rather
///          than wrapping to a negative size that would pass a naive `< capacity` check
///          but produce a corrupt allocation.
/// @param current Current capacity of the array (0 = not yet allocated).
/// @param needed Minimum capacity the caller requires.
/// @param initial Floor capacity used when `current == 0`.
/// @return New capacity >= needed, or -1 on overflow.
static int32_t ph3d_next_capacity(int32_t current, int32_t needed, int32_t initial) {
    int32_t capacity = current > 0 ? current : initial;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2)
            return -1;
        capacity *= 2;
    }
    return capacity;
}

/// @brief Grow the world's body array to fit at least `needed` entries.
/// @details Shared growth contract followed by every `world3d_reserve_*` helper:
///            1. No-op and return success when the current capacity already covers it.
///            2. On realloc failure, leave the existing array intact and return 0 —
///               callers can safely continue using whatever was already there.
///            3. On successful grow, zero-initialize the freshly extended tail so
///               unused slots are always NULL-valued and can be scanned safely.
///          Only the pointer array is touched; body lifetime is owned elsewhere.
/// @param w World whose retained body-pointer table may grow.
/// @param needed Minimum number of body slots required.
/// @return 1 on success (including no-op), 0 on allocation failure.
static int world3d_reserve_body_capacity(rt_world3d *w, int32_t needed) {
    if (!w || needed <= w->body_capacity)
        return 1;
    int32_t new_capacity = ph3d_next_capacity(w->body_capacity, needed, PH3D_INITIAL_BODIES);
    if (new_capacity < 0)
        return 0;
    rt_body3d **new_bodies =
        (rt_body3d **)realloc(w->bodies, (size_t)new_capacity * sizeof(*new_bodies));
    if (!new_bodies)
        return 0;
    memset(new_bodies + w->body_capacity,
           0,
           (size_t)(new_capacity - w->body_capacity) * sizeof(*new_bodies));
    w->bodies = new_bodies;
    w->body_capacity = new_capacity;
    return 1;
}

/// @brief Generic reserve helper used by all four contact-style arrays
///        (active contacts, previous contacts, enter / stay / exit event queues).
/// @details Same growth contract as `world3d_reserve_body_capacity` — realloc-in-place,
///          leave the array untouched on failure, zero-initialize the tail on grow. The
///          four thin wrappers below exist only to pick the right `(array, capacity)`
///          field pair; keeping one body avoids the five-copy-paste bug-multiplication
///          this code used to have.
/// @param array Address of the contact-array pointer to grow.
/// @param capacity Address of the current allocated capacity.
/// @param needed Minimum number of contact slots required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_contact_array(rt_contact3d **array, int32_t *capacity, int32_t needed) {
    if (!array || !capacity || needed <= *capacity)
        return 1;
    int32_t new_capacity = ph3d_next_capacity(*capacity, needed, PH3D_INITIAL_CONTACTS);
    if (new_capacity < 0)
        return 0;
    rt_contact3d *new_array =
        (rt_contact3d *)realloc(*array, (size_t)new_capacity * sizeof(*new_array));
    if (!new_array)
        return 0;
    memset(new_array + *capacity, 0, (size_t)(new_capacity - *capacity) * sizeof(*new_array));
    *array = new_array;
    *capacity = new_capacity;
    return 1;
}

/// @brief Grow w->contacts to hold at least @p needed entries.
/// @param w World whose current-substep contact array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
int world3d_reserve_contacts(rt_world3d *w, int32_t needed) {
    if (!w)
        return 0;
    return world3d_reserve_contact_array(&w->contacts, &w->contact_capacity, needed);
}

/// @brief Grow w->frame_contacts to hold at least @p needed entries.
/// @details Frame contacts aggregate unique pairs across all CCD substeps so
///          very brief substep contacts still participate in the frame's
///          enter/stay/exit event diff.
/// @param w World whose frame-aggregate contact array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
int world3d_reserve_frame_contacts(rt_world3d *w, int32_t needed) {
    if (!w)
        return 0;
    return world3d_reserve_contact_array(&w->frame_contacts, &w->frame_contact_capacity, needed);
}

/// @brief Grow w->previous_contacts to hold at least @p needed entries.
/// @details Stores the contact set from the previous step for enter/exit event diffing.
/// @param w World whose previous-contact array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_previous_contacts(rt_world3d *w, int32_t needed) {
    return world3d_reserve_contact_array(
        &w->previous_contacts, &w->previous_contact_capacity, needed);
}

/// @brief Grow w->enter_events to hold at least @p needed entries.
/// @details Enter events are emitted when a contact pair appears this step but
///   was absent from the previous step's contact set.
/// @param w World whose enter-event array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_enter_events(rt_world3d *w, int32_t needed) {
    return world3d_reserve_contact_array(&w->enter_events, &w->enter_event_capacity, needed);
}

/// @brief Grow w->stay_events to hold at least @p needed entries.
/// @details Stay events are emitted for contact pairs that existed both last step
///   and this step (continuous contact).
/// @param w World whose stay-event array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_stay_events(rt_world3d *w, int32_t needed) {
    return world3d_reserve_contact_array(&w->stay_events, &w->stay_event_capacity, needed);
}

/// @brief Grow w->exit_events to hold at least @p needed entries.
/// @details Exit events are emitted when a contact pair was present last step but
///   is absent this step (separation).
/// @param w World whose exit-event array may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_exit_events(rt_world3d *w, int32_t needed) {
    return world3d_reserve_contact_array(&w->exit_events, &w->exit_event_capacity, needed);
}

/// @brief Increment `value` into `*out`, returning 0 (no write) on INT32_MAX overflow or NULL out.
/// @param value Current non-negative count.
/// @param out Receives the incremented count on success.
/// @return One when incremented without overflow, otherwise zero.
int world3d_checked_increment(int32_t value, int32_t *out) {
    if (!out || value == INT32_MAX)
        return 0;
    *out = value + 1;
    return 1;
}

/// @brief Grow the joint table — the one reserve helper that has to allocate two
///        parallel arrays atomically (joint pointers + joint type tags).
/// @details The two new arrays are allocated before the world is mutated. On failure,
///          the old arrays and `joint_capacity` remain unchanged. Both tails are
///          zero-initialized on success so unused slots always read as NULL / 0 joint_type.
/// @param w World whose parallel joint tables may grow.
/// @param needed Minimum number of joint slots required.
/// @return One on success, including a no-op, otherwise zero.
static int world3d_reserve_joint_capacity(rt_world3d *w, int32_t needed) {
    if (!w || needed <= w->joint_capacity)
        return 1;
    int32_t new_capacity = ph3d_next_capacity(w->joint_capacity, needed, PH3D_INITIAL_JOINTS);
    if (new_capacity < 0)
        return 0;
    void **new_joints = (void **)calloc((size_t)new_capacity, sizeof(*new_joints));
    int32_t *new_types = (int32_t *)calloc((size_t)new_capacity, sizeof(*new_types));
    if (!new_joints || !new_types) {
        free(new_joints);
        free(new_types);
        return 0;
    }
    if (w->joint_count > 0) {
        memcpy(new_joints, w->joints, (size_t)w->joint_count * sizeof(*new_joints));
        memcpy(new_types, w->joint_types, (size_t)w->joint_count * sizeof(*new_types));
    }
    free(w->joints);
    free(w->joint_types);
    w->joints = new_joints;
    w->joint_types = new_types;
    w->joint_capacity = new_capacity;
    return 1;
}

/// @brief Grow the sweep-and-prune broadphase scratch table. Capacity is sized per-body
///        so the initial floor tracks `PH3D_INITIAL_BODIES`. Unlike the body-pointer
///        array, entries are treated as pure scratch — the tail is NOT zero-initialized
///        because the broadphase rebuild always writes every entry before reading it.
/// @param w World whose simulation broadphase table may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
int world3d_reserve_broadphase_capacity(rt_world3d *w, int32_t needed) {
    if (!w)
        return 1;
    if (g_ph3d_test_force_broadphase_alloc_failure)
        return 0;
    if (needed <= w->broadphase_capacity)
        return 1;
    int32_t new_capacity = ph3d_next_capacity(w->broadphase_capacity, needed, PH3D_INITIAL_BODIES);
    if (new_capacity < 0)
        return 0;
    ph3d_broadphase_entry *new_entries = (ph3d_broadphase_entry *)realloc(
        w->broadphase_entries, (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries)
        return 0;
    w->broadphase_entries = new_entries;
    w->broadphase_capacity = new_capacity;
    return 1;
}

/// @brief Grow the query broadphase's dedicated entry table.
/// @details The query cache must NOT share storage with the per-step solver broadphase:
///   the solver refills and re-sorts its entries on the widest-spread axis every Step,
///   which would silently clobber a still-valid query cache sorted by min-X. Entries are
///   pure scratch (every rebuild writes each entry before reading it).
/// @param w World whose query broadphase table may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
int world3d_reserve_query_broadphase_capacity(rt_world3d *w, int32_t needed) {
    if (!w)
        return 1;
    if (g_ph3d_test_force_broadphase_alloc_failure)
        return 0;
    if (needed <= w->query_broadphase_entry_capacity)
        return 1;
    int32_t new_capacity =
        ph3d_next_capacity(w->query_broadphase_entry_capacity, needed, PH3D_INITIAL_BODIES);
    if (new_capacity < 0)
        return 0;
    ph3d_broadphase_entry *new_entries = (ph3d_broadphase_entry *)realloc(
        w->query_broadphase_entries, (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries)
        return 0;
    w->query_broadphase_entries = new_entries;
    w->query_broadphase_entry_capacity = new_capacity;
    return 1;
}

/// @brief Grow the reusable broadphase sort scratch table used by deterministic merge sorting.
/// @details The buffer is pure scratch; callers write every element before reading it. Keeping it
///   on the world avoids per-step temporary allocation and lets the query broadphase share the
///   same storage.
/// @param w World whose reusable sort buffer may grow.
/// @param needed Minimum number of entries required.
/// @return One on success, including a no-op, otherwise zero.
int world3d_reserve_broadphase_sort_scratch(rt_world3d *w, int32_t needed) {
    if (!w)
        return 1;
    if (needed <= w->broadphase_sort_scratch_capacity)
        return 1;
    int32_t new_capacity =
        ph3d_next_capacity(w->broadphase_sort_scratch_capacity, needed, PH3D_INITIAL_BODIES);
    if (new_capacity < 0)
        return 0;
    ph3d_broadphase_entry *new_entries = (ph3d_broadphase_entry *)realloc(
        w->broadphase_sort_scratch, (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries)
        return 0;
    w->broadphase_sort_scratch = new_entries;
    w->broadphase_sort_scratch_capacity = new_capacity;
    return 1;
}

/// @brief Return a world-persistent scratch buffer of at least @p bytes for @p slot.
/// @details The solver's hottest path (island batching, warm-start pair tables,
///   event diff flags) runs every substep; carving its transient arrays out of
///   world-owned slots that grow geometrically and persist until teardown
///   removes all per-substep malloc/free churn. Contents are scratch: callers
///   must not rely on values surviving between acquires. @p zeroed gives
///   calloc semantics for the first @p bytes.
/// @param w World owning the persistent scratch slots.
/// @param slot Zero-based scratch-slot index.
/// @param bytes Minimum requested byte size.
/// @param zeroed Nonzero to clear the requested prefix before returning.
/// @return Slot buffer, or NULL on invalid args / allocation failure.
void *world3d_step_scratch_acquire(rt_world3d *w, int32_t slot, size_t bytes, int zeroed) {
    if (!w || slot < 0 || slot >= PH3D_WORLD_SCRATCH_SLOTS || bytes == 0)
        return NULL;
    if (w->step_scratch_capacity[slot] < bytes) {
        size_t grown = w->step_scratch_capacity[slot] ? w->step_scratch_capacity[slot] : 256u;
        while (grown < bytes) {
            if (grown > SIZE_MAX / 2u) {
                grown = bytes;
                break;
            }
            grown *= 2u;
        }
        void *next = realloc(w->step_scratch[slot], grown);
        if (!next)
            return NULL;
        w->step_scratch[slot] = next;
        w->step_scratch_capacity[slot] = grown;
    }
    if (zeroed)
        memset(w->step_scratch[slot], 0, bytes);
    return w->step_scratch[slot];
}

/// @brief Release every step-scratch slot (world teardown / reset only).
/// @param w World whose scratch allocations are freed and reset.
void world3d_step_scratch_free_all(rt_world3d *w) {
    if (!w)
        return;
    for (int32_t i = 0; i < PH3D_WORLD_SCRATCH_SLOTS; i++) {
        free(w->step_scratch[i]);
        w->step_scratch[i] = NULL;
        w->step_scratch_capacity[i] = 0;
    }
}

/// @brief Clamp @p value to [0, +∞), substituting @p fallback for non-finite inputs.
/// @details Used to sanitize body properties such as mass, restitution, and damping
///   on creation so internal state never contains NaN or negative physical quantities.
/// @param value Candidate physical parameter.
/// @param fallback Replacement for a non-finite candidate.
/// @return Finite value clamped to `[0, PH3D_PARAM_ABS_MAX]`, or @p fallback.
double ph3d_clamp_nonnegative_finite(double value, double fallback) {
    if (!isfinite(value))
        return fallback;
    if (value < 0.0)
        return 0.0;
    return value > PH3D_PARAM_ABS_MAX ? PH3D_PARAM_ABS_MAX : value;
}

/// @brief Return @p value if it is finite, otherwise return @p fallback.
/// @details Thin guard used wherever scalar inputs (gravity components, position offsets)
///   must be well-defined doubles before being stored in body or world state.
/// @param value Candidate scalar.
/// @param fallback Replacement for a non-finite candidate.
/// @return @p value when finite, otherwise @p fallback.
double ph3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp user-facing solver iterations to the runtime-supported range.
/// @param value Requested iteration count.
/// @return Count in the inclusive range one through `PH3D_MAX_SOLVER_ITERATIONS`.
static int32_t ph3d_clamp_solver_iterations(int64_t value) {
    if (value < 1)
        return 1;
    if (value > PH3D_MAX_SOLVER_ITERATIONS)
        return PH3D_MAX_SOLVER_ITERATIONS;
    return (int32_t)value;
}

/// @brief Clamp a public [0, 1] solver tuning scalar, replacing non-finite input.
/// @param value Candidate tuning scalar.
/// @param fallback Replacement used when @p value is non-finite.
/// @return Finite scalar in the inclusive range zero to one.
static double ph3d_clamp_unit_finite(double value, double fallback) {
    if (!isfinite(value))
        value = fallback;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Sanitize a fixed-step size: invalid inputs fall back to the 60 Hz default.
/// @param dt Candidate fixed-step duration in seconds.
/// @return Positive duration capped at `PH3D_STEP_DT_MAX`.
static double ph3d_sanitize_fixed_dt(double dt) {
    if (!isfinite(dt) || dt <= 0.0)
        return 1.0 / 60.0;
    return dt > PH3D_STEP_DT_MAX ? PH3D_STEP_DT_MAX : dt;
}

/// @brief Clamp max fixed steps to the non-negative int32 range used by the loop.
/// @param max_steps Requested maximum substep count.
/// @return Count in the range zero through `INT32_MAX`.
static int32_t ph3d_clamp_fixed_step_limit(int64_t max_steps) {
    if (max_steps <= 0)
        return 0;
    if (max_steps > INT32_MAX)
        return INT32_MAX;
    return (int32_t)max_steps;
}

/// @brief Add dropped fixed steps using saturating i64 arithmetic.
/// @param w World whose cumulative counter is updated.
/// @param dropped Positive number of discarded substeps.
static void ph3d_add_dropped_fixed_steps(rt_world3d *w, int64_t dropped) {
    if (!w || dropped <= 0)
        return;
    if (w->dropped_fixed_steps > INT64_MAX - dropped)
        w->dropped_fixed_steps = INT64_MAX;
    else
        w->dropped_fixed_steps += dropped;
}

/// @brief Publish the fixed-step interpolation alpha from accumulator/fixed_dt.
/// @param w World whose interpolation fraction is updated.
/// @param fixed_dt Positive fixed-step duration.
static void ph3d_update_fixed_step_alpha(rt_world3d *w, double fixed_dt) {
    if (!w || !isfinite(fixed_dt) || fixed_dt <= 0.0) {
        if (w)
            w->fixed_step_alpha = 0.0;
        return;
    }
    w->fixed_step_alpha = w->fixed_step_accumulator / fixed_dt;
    if (!isfinite(w->fixed_step_alpha) || w->fixed_step_alpha < 0.0)
        w->fixed_step_alpha = 0.0;
    if (w->fixed_step_alpha >= 1.0)
        w->fixed_step_alpha = 0.999999;
}

/// @brief Clamp a physics state scalar to ±PH3D_STATE_ABS_MAX, mapping NaN/inf to 0.
/// @details Keeps positions/velocities from blowing up to non-finite values that would poison the
///          whole simulation; the bound is large enough not to affect well-behaved bodies.
/// @param value Candidate state scalar.
/// @return Finite signed value within the state envelope.
static double ph3d_saturate_state_value(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > PH3D_STATE_ABS_MAX)
        return PH3D_STATE_ABS_MAX;
    if (value < -PH3D_STATE_ABS_MAX)
        return -PH3D_STATE_ABS_MAX;
    return value;
}

/// @brief Write a sanitized 3D vector to @p dst, replacing each NaN/Inf component with 0.
/// @details Used when accepting Vec3 arguments from Zia to populate force, velocity, and
///   position arrays — ensures that a single bad component cannot contaminate the solver.
/// @param dst Three-component destination.
/// @param x Candidate X component.
/// @param y Candidate Y component.
/// @param z Candidate Z component.
static void ph3d_vec3_set_finite(double *dst, double x, double y, double z) {
    dst[0] = ph3d_saturate_state_value(x);
    dst[1] = ph3d_saturate_state_value(y);
    dst[2] = ph3d_saturate_state_value(z);
}

/// @brief Clamp an extent-like quantity to a finite, non-negative runtime bound.
/// @param value Candidate signed extent.
/// @return Absolute finite extent capped at `PH3D_PARAM_ABS_MAX`.
static double ph3d_sanitize_extent_value(double value) {
    if (!isfinite(value))
        return 0.0;
    value = fabs(value);
    if (value > PH3D_PARAM_ABS_MAX)
        return PH3D_PARAM_ABS_MAX;
    return value;
}

/// @brief Clamp a transform scale component while preserving sign and rejecting collapsed axes.
/// @param value Candidate scale component.
/// @return Finite nonzero scale bounded by `PH3D_PARAM_ABS_MAX`.
static double ph3d_sanitize_scale_value(double value) {
    if (!isfinite(value) || fabs(value) <= 1e-12)
        return 1.0;
    if (value > PH3D_PARAM_ABS_MAX)
        return PH3D_PARAM_ABS_MAX;
    if (value < -PH3D_PARAM_ABS_MAX)
        return -PH3D_PARAM_ABS_MAX;
    return value;
}

/// @brief Clamp a public Step duration to a stable, finite frame-sized delta.
/// @param dt Candidate frame duration in seconds.
/// @return Duration in `[0, PH3D_STEP_DT_MAX]`.
static double ph3d_sanitize_step_dt(double dt) {
    if (!isfinite(dt) || dt <= 0.0)
        return 0.0;
    return dt > PH3D_STEP_DT_MAX ? PH3D_STEP_DT_MAX : dt;
}

/// @brief Saturate each component of a state vector in place (see ph3d_saturate_state_value).
/// @param v Three-component state vector; a null pointer is ignored.
void ph3d_vec3_sanitize_state(double *v) {
    if (!v)
        return;
    v[0] = ph3d_saturate_state_value(v[0]);
    v[1] = ph3d_saturate_state_value(v[1]);
    v[2] = ph3d_saturate_state_value(v[2]);
}

/// @brief Copy and saturate a physics state vector.
/// @param dst Three-component destination.
/// @param src Optional source; NULL produces the origin.
static void ph3d_vec3_copy_state(double *dst, const double *src) {
    if (!dst)
        return;
    if (!src) {
        dst[0] = dst[1] = dst[2] = 0.0;
        return;
    }
    dst[0] = ph3d_saturate_state_value(src[0]);
    dst[1] = ph3d_saturate_state_value(src[1]);
    dst[2] = ph3d_saturate_state_value(src[2]);
}

/// @brief Add (x, y, z) into @p dst, sanitizing the addends and saturating the result per
/// component.
/// @param dst Three-component state accumulator.
/// @param x X increment.
/// @param y Y increment.
/// @param z Z increment.
static void ph3d_vec3_accumulate_state(double *dst, double x, double y, double z) {
    if (!dst)
        return;
    dst[0] = ph3d_saturate_state_value(dst[0] + ph3d_finite_or(x, 0.0));
    dst[1] = ph3d_saturate_state_value(dst[1] + ph3d_finite_or(y, 0.0));
    dst[2] = ph3d_saturate_state_value(dst[2] + ph3d_finite_or(z, 0.0));
}

/// @brief Validate @p obj as a PhysicsHit3D handle (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed PhysicsHit3D payload, or NULL on class mismatch.
static rt_physics_hit3d_obj *physics_hit3d_checked(void *obj) {
    return (rt_physics_hit3d_obj *)rt_g3d_checked_or_null(obj, RT_G3D_PHYSICSHIT3D_CLASS_ID);
}

/// @brief Validate @p obj as a PhysicsHitList3D handle (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed PhysicsHitList3D payload, or NULL on class mismatch.
static rt_physics_hit_list3d_obj *physics_hit_list3d_checked(void *obj) {
    return (rt_physics_hit_list3d_obj *)rt_g3d_checked_or_null(obj,
                                                               RT_G3D_PHYSICSHITLIST3D_CLASS_ID);
}

/// @brief Validate @p obj as a CollisionEvent3D handle (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed CollisionEvent3D payload, or NULL on class mismatch.
static rt_collision_event3d_obj *collision_event3d_checked(void *obj) {
    return (rt_collision_event3d_obj *)rt_g3d_checked_or_null(obj,
                                                              RT_G3D_COLLISIONEVENT3D_CLASS_ID);
}

/// @brief Validate @p obj as a ContactPoint3D handle (NULL on mismatch).
/// @param obj Candidate runtime handle.
/// @return Borrowed ContactPoint3D payload, or NULL on class mismatch.
static rt_contact_point3d_obj *contact_point3d_checked(void *obj) {
    return (rt_contact_point3d_obj *)rt_g3d_checked_or_null(obj, RT_G3D_CONTACTPOINT3D_CLASS_ID);
}

/// @brief Clamp a logical array count to readable allocated storage.
/// @param array Backing allocation whose presence is required.
/// @param count Logical number of live entries.
/// @param capacity Allocated entry capacity.
/// @return Zero for invalid storage, otherwise the lesser of count and capacity.
static int32_t ph3d_clamped_array_count(const void *array, int32_t count, int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Count the valid contiguous Body3D prefix of a world's retained body table.
/// @param w World to inspect.
/// @return Safely bounded number of class-valid body entries.
static int32_t world3d_body_count_safe(const rt_world3d *w) {
    int32_t limit = w ? ph3d_clamped_array_count(w->bodies, w->body_count, w->body_capacity) : 0;
    int32_t count = 0;
    for (int32_t i = 0; i < limit; ++i) {
        if (!body3d_checked(w->bodies[i]))
            break;
        count++;
    }
    return count;
}

/// @brief Count the valid contiguous joint/type prefix of a world's joint tables.
/// @param w World to inspect.
/// @return Safely bounded number of class-valid joint entries.
static int32_t world3d_joint_count_safe(const rt_world3d *w) {
    if (!w || !w->joint_types)
        return 0;
    int32_t limit = ph3d_clamped_array_count(w->joints, w->joint_count, w->joint_capacity);
    int32_t count = 0;
    for (int32_t i = 0; i < limit; ++i) {
        if (!joint3d_matches_type(w->joints[i], w->joint_types[i]))
            break;
        count++;
    }
    return count;
}

/// @brief Count the valid contiguous contact prefix with two class-valid bodies per entry.
/// @param contacts Contact array to inspect.
/// @param count Logical entry count.
/// @param capacity Allocated entry capacity.
/// @return Safely bounded number of valid contacts.
static int32_t world3d_contact_count_safe(const rt_contact3d *contacts,
                                          int32_t count,
                                          int32_t capacity) {
    int32_t limit = ph3d_clamped_array_count(contacts, count, capacity);
    int32_t valid = 0;
    for (int32_t i = 0; i < limit; ++i) {
        if (!body3d_checked(contacts[i].body_a) || !body3d_checked(contacts[i].body_b))
            break;
        valid++;
    }
    return valid;
}

/// @brief Clamp a collision event's contact count to its inline manifold capacity.
/// @param event Collision event to inspect.
/// @return Number of readable inline contact points.
static int32_t collision_event3d_contact_count_safe(const rt_collision_event3d_obj *event) {
    if (!event || event->contact_count <= 0)
        return 0;
    return event->contact_count < PH3D_MAX_MANIFOLD_POINTS ? event->contact_count
                                                           : PH3D_MAX_MANIFOLD_POINTS;
}

/// @brief Clamp a hit list's logical count to all allocation and policy bounds.
/// @param list PhysicsHitList3D payload to inspect.
/// @return Number of safely readable hit entries.
static int64_t physics_hit_list3d_count_safe(const rt_physics_hit_list3d_obj *list) {
    if (!list || !list->items || list->count <= 0 || list->capacity <= 0 ||
        list->allocated_count <= 0)
        return 0;
    int64_t cap = list->capacity < list->allocated_count ? list->capacity : list->allocated_count;
    if (cap > PH3D_QUERY_HITS_MAX)
        cap = PH3D_QUERY_HITS_MAX;
    return list->count < cap ? list->count : cap;
}

/// @brief Validate and borrow a Body3D reference for boxed result ownership.
/// @param body Candidate Body3D handle.
/// @return Borrowed Body3D payload, or NULL on class mismatch.
static rt_body3d *ph3d_body_ref_or_null(void *body) {
    return body3d_checked(body);
}

/// @brief Validate and borrow a Collider3D reference for boxed result ownership.
/// @param collider Candidate Collider3D handle.
/// @return Borrowed Collider3D handle, or NULL on class mismatch.
static void *ph3d_collider_ref_or_null(void *collider) {
    return rt_g3d_checked_or_null(collider, RT_G3D_COLLIDER3D_CLASS_ID);
}

// Forward declarations for functions defined later in this translation unit.
// They are referenced from helpers above that must be inlined by the compiler
// before the full definitions appear.
/// @brief Build a rt_collider_pose from the body's position/orientation/scale fields.
void collider_pose_from_body(const rt_body3d *body, rt_collider_pose *pose);
/// @brief Compose a parent pose with a child's local position/rotation/scale.
void collider_pose_compose(const rt_collider_pose *parent,
                           const double *child_position,
                           const double *child_rotation,
                           const double *child_scale,
                           rt_collider_pose *out);
/// @brief Transform a point from the collider's local space into world space.
void transform_point_from_pose(const rt_collider_pose *pose,
                               const double *local_point,
                               double *world_point);
/// @brief Transform a point from world space into the collider's local space.
void transform_point_to_local(const rt_collider_pose *pose,
                              const double *world_point,
                              double *local_point);
/// @brief Sync the body's cached primitive shape from its attached collider's geometry.
void body3d_update_shape_cache_from_collider(rt_body3d *body);
/// @brief Compute the two world-space endpoint positions of a capsule's axis segment.
void capsule_axis_endpoints(const rt_body3d *b, double *a, double *c);
/// @brief Run narrow-phase collision detection between two bodies.
int test_collision(const rt_body3d *a,
                   const rt_body3d *b,
                   double *normal,
                   double *depth,
                   double *point,
                   void **leaf_a_out,
                   void **leaf_b_out,
                   rt_collider_pose *leaf_a_pose_out,
                   rt_collider_pose *leaf_b_pose_out);
/// @brief test_collision variant that also accumulates per-leaf compound contacts
///        into @p manifold_acc during the SAME narrow-phase pass.
int test_collision_manifold(const rt_body3d *a,
                            const rt_body3d *b,
                            double *normal,
                            double *depth,
                            double *point,
                            void **leaf_a_out,
                            void **leaf_b_out,
                            rt_collider_pose *leaf_a_pose_out,
                            rt_collider_pose *leaf_b_pose_out,
                            rt_contact3d *manifold_acc);

/// @brief Sanitize and order a broadphase AABB after a raw collider/body bounds calculation.
/// @param mn In/out three-component minimum corner.
/// @param mx In/out three-component maximum corner.
static void ph3d_sanitize_aabb(double *mn, double *mx) {
    if (!mn || !mx)
        return;
    for (int axis = 0; axis < 3; ++axis) {
        double lo = ph3d_saturate_state_value(mn[axis]);
        double hi = ph3d_saturate_state_value(mx[axis]);
        if (lo <= hi) {
            mn[axis] = lo;
            mx[axis] = hi;
        } else {
            mn[axis] = hi;
            mx[axis] = lo;
        }
    }
}

/// @brief True when a body has collider data or a non-empty cached primitive shape.
///
/// Bare Body3D.New objects intentionally return false: callers must assign a
/// collider before they participate in collision. The cached-shape fallback
/// keeps legacy/internal primitive bodies usable if their collider pointer is
/// absent but their primitive dimensions are still populated.
/// @param body Body3D payload to inspect.
/// @return One when a valid collider or non-empty cached primitive is present, otherwise zero.
int body3d_has_collision_geometry(const rt_body3d *body) {
    if (!body)
        return 0;
    if (body3d_collider_ref(body))
        return 1;
    if (body->shape == PH3D_SHAPE_SPHERE)
        return isfinite(body->radius) && body->radius > 0.0;
    if (body->shape == PH3D_SHAPE_CAPSULE)
        return isfinite(body->radius) && body->radius > 0.0 && isfinite(body->height) &&
               body->height > 0.0;
    return isfinite(body->half_extents[0]) && isfinite(body->half_extents[1]) &&
           isfinite(body->half_extents[2]) &&
           (body->half_extents[0] > 0.0 || body->half_extents[1] > 0.0 ||
            body->half_extents[2] > 0.0);
}

// clang-format off
#include "rt_physics3d_detect.inc"
#include "rt_physics3d_world.inc"
#include "rt_physics3d_events.inc"
#include "rt_physics3d_body.inc"
// clang-format on
#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
