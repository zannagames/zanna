//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_internal.h
// Purpose: Internal shared types, constants, and helper declarations for the
//   Physics3D runtime, split across rt_physics3d.c (core/integration/lifecycle),
//   rt_physics3d_collision.c (narrow-phase + contact manifolds),
//   rt_physics3d_solver.c (sequential-impulse solver + broad phase),
//   rt_physics3d_query.c (raycast/overlap/sweep), and
//   rt_physics3d_character.c (character controller + trigger volumes).
//   Private to the physics runtime — not part of the public Graphics3D ABI.
//
// Key invariants:
//   - rt_body3d field layout is pinned by _Static_asserts in rt_physics3d.c
//     against the private rt_body3d_kinematics prefix view.
//   - World3D / Body3D / Character3D / Trigger3D are GC-managed; shared mutable
//     state flows exclusively through these struct pointers (no file-scope state).
//
// Ownership/Lifetime:
//   - This header declares borrowed internal pointers only; owning references
//     are held by the runtime objects that contain them.
//   - Split Physics3D translation units include this header but do not expose
//     these layouts as script-facing ABI.
//
// Links: rt_physics3d.c, rt_physics3d_world.inc, rt_physics3d_solver.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_internal.h
 * @brief Defines private Physics3D layouts, constants, ownership, and cross-unit helper contracts.
 *
 * This header is shared only by the split Physics3D implementation units. It
 * exposes borrowed internal pointers, bounded scratch and query structures,
 * collision/solver helper contracts, and the pinned Body3D/World3D layouts;
 * none of these declarations form the public script ABI.
 */

#pragma once

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_body3d_kinematics_internal.h"
#include "rt_canvas3d_internal.h"
#include "rt_physics3d.h"

#include <stddef.h>
#include <stdint.h>

//===----------------------------------------------------------------------===//
// Shared runtime helpers provided by the object/vector/quaternion runtime.
//===----------------------------------------------------------------------===//
#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate a managed runtime object of a registered class.
/// @param class_id Runtime class identifier.
/// @param byte_size Requested object payload size.
/// @return Newly allocated object, or NULL on failure.
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
/// @brief Attach a GC finalizer callback to a managed object.
/// @param obj Managed object to configure.
/// @param fn Finalizer invoked before object storage is reclaimed.
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
/// @brief Retain a managed object when the handle is non-null.
/// @param obj Object handle to retain.
extern void rt_obj_retain_maybe(void *obj);
/// @brief Release one reference and report whether the object reached zero.
/// @param obj Managed object to release.
/// @return One when the object must be finalized and freed, otherwise zero.
extern int rt_obj_release_check0(void *obj);
/// @brief Finalize and reclaim a zero-reference managed object.
/// @param obj Managed object to destroy.
extern void rt_obj_free(void *obj);
/// @brief Allocate a boxed three-component vector.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @return Newly allocated Vec3 handle, or NULL on failure.
extern void *rt_vec3_new(double x, double y, double z);
/// @brief Read a Vec3 X component.
/// @param v Vec3 handle.
/// @return X component.
extern double rt_vec3_x(void *v);
/// @brief Read a Vec3 Y component.
/// @param v Vec3 handle.
/// @return Y component.
extern double rt_vec3_y(void *v);
/// @brief Read a Vec3 Z component.
/// @param v Vec3 handle.
/// @return Z component.
extern double rt_vec3_z(void *v);
/// @brief Allocate a boxed XYZW quaternion.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @param w Scalar component.
/// @return Newly allocated quaternion handle, or NULL on failure.
extern void *rt_quat_new(double x, double y, double z, double w);
/// @brief Read a quaternion X component.
/// @param q Quaternion handle.
/// @return X component.
extern double rt_quat_x(void *q);
/// @brief Read a quaternion Y component.
/// @param q Quaternion handle.
/// @return Y component.
extern double rt_quat_y(void *q);
/// @brief Read a quaternion Z component.
/// @param q Quaternion handle.
/// @return Z component.
extern double rt_quat_z(void *q);
/// @brief Read a quaternion scalar component.
/// @param q Quaternion handle.
/// @return W component.
extern double rt_quat_w(void *q);

#ifdef __cplusplus
}
#endif

//===----------------------------------------------------------------------===//
// Shared constants.
//===----------------------------------------------------------------------===//
#define PH3D_INITIAL_BODIES 256
#define PH3D_SHAPE_AABB 0
#define PH3D_SHAPE_SPHERE 1
#define PH3D_SHAPE_CAPSULE 2
#define PH3D_MODE_DYNAMIC 0
#define PH3D_MODE_STATIC 1
#define PH3D_MODE_KINEMATIC 2
#define PH3D_SLEEP_LINEAR_THRESHOLD 0.05
#define PH3D_SLEEP_ANGULAR_THRESHOLD 0.05
#define PH3D_SLEEP_DELAY 0.5
#define PH3D_DEFAULT_SOLVER_ITERATIONS 6
#define PH3D_DEFAULT_POSITION_ITERATIONS 1
#define PH3D_DEFAULT_CONTACT_BETA 0.8
#define PH3D_DEFAULT_RESTITUTION_THRESHOLD 0.5
#define PH3D_MAX_SOLVER_ITERATIONS 64
/* CCD segmentation is local to each opted-in body. One bullet must never
 * multiply integration, broadphase, joints, or contact solving for unrelated
 * bodies. The legacy SUBSTEPS spelling remains as an internal compatibility
 * alias for existing diagnostics/tests; it now means the maximum local segment
 * count reported by LastCcdSubsteps. */
#define PH3D_MAX_CCD_BODY_SEGMENTS 64
#define PH3D_MAX_CCD_SUBSTEPS PH3D_MAX_CCD_BODY_SEGMENTS
#define PH3D_MAX_SWEEP_STEPS 512
#define PH3D_MAX_MANIFOLD_POINTS 4
#define PH3D_INITIAL_CONTACTS 128
#define PH3D_MAX_QUERY_HITS 256
/* World-persistent step scratch slots (see world3d_step_scratch_acquire).
 * Buffers grow on demand, are reused every substep, and are freed only at
 * world teardown — the solver's hottest path performs no per-substep
 * malloc/free. Distinct slots exist for buffers alive at the same time. */
#define PH3D_SCRATCH_SOLVER_BATCH 0
#define PH3D_SCRATCH_PAIR_TABLE_A 1
#define PH3D_SCRATCH_PAIR_TABLE_B 2
#define PH3D_SCRATCH_EVENT_FLAGS 3
#define PH3D_SCRATCH_STEP_TRANSACTION 4
#define PH3D_SCRATCH_BROADPHASE_BVH 5
#define PH3D_WORLD_SCRATCH_SLOTS 6
/* Slop added to each cached query-broadphase AABB. Bodies that move without
 * escaping their fattened bounds do not invalidate the cache: the fat entry
 * remains a conservative candidate filter and narrow phase tests live body
 * state, so results stay exact. Plain double add/sub — deterministic. */
#define PH3D_QUERY_BROADPHASE_MARGIN 0.5
#define PH3D_QUERY_HITS_MIN 16
#define PH3D_QUERY_HITS_MAX 4096
#define PH3D_INITIAL_JOINTS 128

//===----------------------------------------------------------------------===//
// Core simulation structs.
//===----------------------------------------------------------------------===//

typedef struct rt_world3d rt_world3d;

/// @brief Rigid body payload: pose (position/orientation/scale), linear+angular
///   motion state, mass/inertia, cached primitive shape, material properties,
///   layer/mask filtering, and sleep/CCD flags.
typedef struct rt_body3d {
    void *vptr;
    double position[3];
    double orientation[4];
    double scale[3];
    double velocity[3];
    double angular_velocity[3];
    double force[3];
    double torque[3];
    double mass;
    double inv_mass;
    void *collider;
    double inv_inertia[3];
    double restitution;
    double friction;
    double linear_damping;
    double angular_damping;
    int64_t user_data; /* opaque gameplay handle; the runtime never reads it */
    int64_t collision_layer;
    int64_t collision_mask;
    int32_t shape;
    double half_extents[3];
    double radius;
    double height;
    double sleep_time;
    int32_t motion_mode;
    int8_t is_static;
    int8_t is_kinematic;
    int8_t is_trigger;
    int8_t is_grounded;
    int8_t can_sleep;
    int8_t is_sleeping;
    int8_t use_ccd;
    double ground_normal[3];
    uint64_t broadphase_revision;
    rt_world3d *owner_world;
    int32_t owner_index;
} rt_body3d;

/// @brief A contact manifold between two bodies: representative point/normal/
///   separation plus up to PH3D_MAX_MANIFOLD_POINTS manifold points and the
///   per-point warm-start impulse accumulators persisted across frames.
typedef struct {
    rt_body3d *body_a;
    rt_body3d *body_b;
    void *collider_a;
    void *collider_b;
    double point[3];
    double normal[3];
    double separation;
    int32_t contact_count;
    double points[PH3D_MAX_MANIFOLD_POINTS][3];
    double normals[PH3D_MAX_MANIFOLD_POINTS][3];
    double separations[PH3D_MAX_MANIFOLD_POINTS];
    double relative_speed;
    double normal_impulse;
    double normal_impulse_acc[PH3D_MAX_MANIFOLD_POINTS];
    double tangent_impulse_acc[PH3D_MAX_MANIFOLD_POINTS][2];
    double restitution_bias[PH3D_MAX_MANIFOLD_POINTS];
    int8_t is_trigger;
} rt_contact3d;

typedef struct ph3d_broadphase_entry ph3d_broadphase_entry;

/// @brief Physics world: body/contact/event arrays, joint list, sweep-and-prune
///   broad-phase cache, fixed-step state, solver tuning, and per-frame diagnostics.
struct rt_world3d {
    void *vptr;
    double gravity[3];
    rt_body3d **bodies;
    int32_t body_count;
    int32_t body_capacity;
    rt_contact3d *contacts;
    int32_t contact_count;
    int32_t contact_capacity;
    rt_contact3d *frame_contacts;
    int32_t frame_contact_count;
    int32_t frame_contact_capacity;
    rt_contact3d *previous_contacts;
    int32_t previous_contact_count;
    int32_t previous_contact_capacity;
    rt_contact3d *enter_events;
    int32_t enter_event_count;
    int32_t enter_event_capacity;
    rt_contact3d *stay_events;
    int32_t stay_event_count;
    int32_t stay_event_capacity;
    rt_contact3d *exit_events;
    int32_t exit_event_count;
    int32_t exit_event_capacity;
    void **collision_event_objects;
    int32_t collision_event_object_capacity;
    void **enter_event_objects;
    int32_t enter_event_object_capacity;
    void **stay_event_objects;
    int32_t stay_event_object_capacity;
    void **exit_event_objects;
    int32_t exit_event_object_capacity;
    void **joints;
    int32_t *joint_types;
    int32_t joint_count;
    int32_t joint_capacity;
    ph3d_broadphase_entry *broadphase_entries;
    ph3d_broadphase_entry *broadphase_sort_scratch;
    int32_t broadphase_capacity;
    int32_t broadphase_sort_scratch_capacity;
    /* Step-scratch slots (island batch, warm-start/event pair tables, event
     * flags): world-owned, grown on demand, never freed mid-simulation. */
    void *step_scratch[PH3D_WORLD_SCRATCH_SLOTS];
    size_t step_scratch_capacity[PH3D_WORLD_SCRATCH_SLOTS];
    uint64_t broadphase_world_revision;
    /* Query-broadphase cache. Entries live in their own array (never shared
     * with the per-step solver broadphase, which re-sorts on the widest axis
     * while queries assume min-X order) and hold FATTENED AABBs so bodies
     * moving within PH3D_QUERY_BROADPHASE_MARGIN of their cached bounds do
     * not force a rebuild. Pose-only transform changes set
     * query_broadphase_dirty; the next query build runs a lazy escape check
     * and rebuilds only when a moved body left its fat bounds. */
    ph3d_broadphase_entry *query_broadphase_entries;
    int32_t query_broadphase_entry_capacity;
    int32_t query_broadphase_count;
    uint64_t query_broadphase_signature;
    uint64_t query_broadphase_mesh_epoch;
    uint64_t query_broadphase_collider_epoch;
    int8_t query_broadphase_valid;
    int8_t query_broadphase_dirty;
    int64_t query_broadphase_rebuild_count;
    void *query_sphere_collider;
    void *query_box_collider;
    /* Configurable query result capacity (RaycastAll/Overlap*): lists still
     * report TotalCount/Truncated; this bounds how many hits are returned.
     * The scratch buffer holds max_query_hits rt_query_hit3d entries (typed
     * void* here because rt_query_hit3d is defined below this struct). */
    void *query_hits_scratch;
    int32_t max_query_hits;
    int64_t broadphase_fallback_count;
    int64_t last_broadphase_leaf_pair_tests;
    int32_t last_ccd_requested_substeps;
    int32_t last_ccd_substeps;
    int64_t ccd_substep_clamped_count;
    int32_t last_ccd_clamped_body_count;
    int64_t ccd_substep_clamped_body_count;
    int64_t ccd_toi_count; /* total swept time-of-impact clips applied */
    int32_t solver_iterations;
    int32_t position_iterations;
    double contact_beta;
    /* Lazily-created worker pool for per-island solver dispatch. Islands are
     * disjoint body/contact sets, so solving them concurrently is bit-identical
     * to the serial island loop. NULL until first parallel-eligible step (or
     * permanently when creation failed). */
    void *solver_pool;
    int8_t solver_pool_failed;
    double restitution_threshold;
    double fixed_step_accumulator;
    double fixed_step_alpha;
    int64_t dropped_fixed_steps;
    int32_t last_solver_island_count;
    int32_t last_solver_active_body_count;
    int32_t last_solver_contact_count;
    int32_t last_integration_pass_count;
    /* A Step transaction converts deep allocation traps into a status that the
     * outer step can roll back before the public ABI raises the trap. */
    const char *step_failure_message;
    int32_t step_pending_broadphase_fallbacks;
    int8_t step_transaction_active;
};

struct ph3d_broadphase_entry {
    rt_body3d *body;
    double min[3];
    double max[3];
    /* Body revision stamped when the query broadphase cached this entry, so a
     * dirty validate can identify exactly which bodies moved since the build.
     * The per-step solver broadphase writes but never reads it. */
    uint64_t body_revision;
};

//===----------------------------------------------------------------------===//
// Posing and query-result structs.
//===----------------------------------------------------------------------===//

/// @brief A world-space placement (position/rotation/scale) used to pose colliders.
typedef struct {
    double position[3];
    double rotation[4];
    double scale[3];
} rt_collider_pose;

/// @brief A raw raycast/sweep hit record (no boxing), filled by query helpers.
typedef struct {
    rt_body3d *body;
    void *collider;
    double point[3];
    double normal[3];
    double distance;
    double fraction;
    int8_t started_penetrating;
    int8_t is_trigger;
} rt_query_hit3d;

/// @brief Boxed PhysicsHit3D handle returned to managed code.
typedef struct {
    void *vptr;
    rt_body3d *body;
    void *collider;
    double point[3];
    double normal[3];
    double distance;
    double fraction;
    int8_t started_penetrating;
    int8_t is_trigger;
} rt_physics_hit3d_obj;

/// @brief Boxed PhysicsHitList3D handle (sorted, optionally truncated).
typedef struct {
    void *vptr;
    void **items;
    rt_query_hit3d *raw_hits;
    int64_t count;
    int64_t capacity;
    int64_t allocated_count;
    int64_t total_count;
    int8_t truncated;
} rt_physics_hit_list3d_obj;

/// @brief Boxed CollisionEvent3D handle carrying a snapshot of a contact manifold.
typedef struct {
    void *vptr;
    rt_body3d *body_a;
    rt_body3d *body_b;
    void *collider_a;
    void *collider_b;
    double point[3];
    double normal[3];
    double separation;
    int32_t contact_count;
    double points[PH3D_MAX_MANIFOLD_POINTS][3];
    double normals[PH3D_MAX_MANIFOLD_POINTS][3];
    double separations[PH3D_MAX_MANIFOLD_POINTS];
    double relative_speed;
    double normal_impulse;
    int8_t is_trigger;
} rt_collision_event3d_obj;

/// @brief Boxed ContactPoint3D handle (single manifold point).
typedef struct {
    void *vptr;
    double point[3];
    double normal[3];
    double separation;
} rt_contact_point3d_obj;

//===----------------------------------------------------------------------===//
// Cluster-shared support structs.
//===----------------------------------------------------------------------===//

/// @brief One slot in the contact-pair de-dup hash table (open addressing).
typedef struct contact_pair_hash_entry {
    const rt_contact3d *contact;
    uintptr_t hash;
} contact_pair_hash_entry;

/// @brief Scratch arrays for one solver step's union-find islands and the
///   per-island contact-index partition (owned/freed by the solver).
typedef struct {
    int32_t *parent;
    int32_t *active_body;
    int32_t *root_to_island;
    int32_t *body_island;
    int32_t *island_contact_counts;
    int32_t *island_write_offsets;
    int32_t *island_offsets;
    int32_t *contact_indices;
    int32_t island_count;
    int32_t active_body_count;
    int32_t solver_contact_count;
} ph3d_solver_island_batch;

/// @brief One node of the physics mesh BVH (shared by narrow-phase + raycast).
typedef struct rt_physics_mesh_bvh_node {
    float min[3];
    float max[3];
    int32_t left;
    int32_t right;
    int32_t start;
    int32_t count;
} rt_physics_mesh_bvh_node;

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Shared internal helpers exposed for the split rt_physics3d_*.c sibling TUs.
//===----------------------------------------------------------------------===//

// --- Vector / quaternion / pose math (defined in rt_physics3d.c) ---
/// @brief Copy a three-component vector.
/// @param dst Destination vector.
/// @param src Source vector.
void vec3_copy(double *dst, const double *src);
/// @brief Set all components of a vector.
/// @param dst Destination vector.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
void vec3_set(double *dst, double x, double y, double z);
/// @brief Subtract two vectors.
/// @param a Minuend vector.
/// @param b Subtrahend vector.
/// @param out Difference output.
void vec3_sub(const double *a, const double *b, double *out);
/// @brief Compute a three-dimensional cross product.
/// @param a First vector.
/// @param b Second vector.
/// @param out Cross-product output.
void vec3_cross(const double *a, const double *b, double *out);
/// @brief Negate a vector.
/// @param src Source vector.
/// @param dst Negated output.
void vec3_negate(const double *src, double *dst);
/// @brief Compute a three-dimensional dot product.
/// @param a First vector.
/// @param b Second vector.
/// @return Scalar dot product.
double vec3_dot(const double *a, const double *b);
/// @brief Compute Euclidean vector length.
/// @param v Source vector.
/// @return Vector magnitude.
double vec3_len(const double *v);
/// @brief Compute squared vector length.
/// @param v Source vector.
/// @return Squared magnitude.
double vec3_len_sq(const double *v);
/// @brief Normalize a vector in place.
/// @param v Vector to normalize.
/// @return Original magnitude.
double vec3_normalize_in_place(double *v);
/// @brief Clamp a scalar to an interval.
/// @param v Value to clamp.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return Clamped value.
double clampd(double v, double lo, double hi);
/// @brief Initialize an XYZW quaternion to identity.
/// @param q Quaternion output.
void quat_identity(double *q);
/// @brief Compute an XYZW quaternion conjugate.
/// @param q Source quaternion.
/// @param out Conjugate output.
void quat_conjugate(const double *q, double *out);
/// @brief Rotate a vector by a quaternion.
/// @param q XYZW rotation quaternion.
/// @param v Source vector.
/// @param out Rotated output.
void quat_rotate_vec3(const double *q, const double *v, double *out);
/// @brief Initialize an identity collider pose.
/// @param pose Pose output.
void collider_pose_identity(rt_collider_pose *pose);
/// @brief Copy a body's sanitized transform into a collider pose.
/// @param body Source Body3D.
/// @param pose Pose output.
void collider_pose_from_body(const rt_body3d *body, rt_collider_pose *pose);
/// @brief Compose a child TRS transform with a parent collider pose.
/// @param parent Parent world pose.
/// @param child_position Child-local translation.
/// @param child_rotation Child-local XYZW rotation.
/// @param child_scale Child-local scale.
/// @param out Composed pose output.
void collider_pose_compose(const rt_collider_pose *parent,
                           const double *child_position,
                           const double *child_rotation,
                           const double *child_scale,
                           rt_collider_pose *out);
/// @brief Transform a local point to world space.
/// @param pose World pose.
/// @param local_point Local-space point.
/// @param world_point World-space output.
void transform_point_from_pose(const rt_collider_pose *pose,
                               const double *local_point,
                               double *world_point);
/// @brief Transform a world point to pose-local space.
/// @param pose World pose.
/// @param world_point World-space point.
/// @param local_point Local-space output.
void transform_point_to_local(const rt_collider_pose *pose,
                              const double *world_point,
                              double *local_point);
/// @brief Transform a world vector to pose-local space without translation.
/// @param pose World pose.
/// @param world_vec World-space vector.
/// @param local_vec Local-space output.
void transform_vector_to_local(const rt_collider_pose *pose,
                               const double *world_vec,
                               double *local_vec);
/// @brief Transform a local normal to world space using inverse-transpose scale.
/// @param pose World pose.
/// @param local_normal Local-space normal.
/// @param world_normal Normalized world output.
void transform_normal_from_local(const rt_collider_pose *pose,
                                 const double *local_normal,
                                 double *world_normal);
/// @brief Sanitize an absolute scale for use as a divisor.
/// @param value Scale component.
/// @return Positive finite absolute scale.
double pose_abs_scale_or_unit(double value);
/// @brief Multiply a vector in place.
/// @param dst Vector to scale.
/// @param s Scalar multiplier.
void vec3_scale_in_place(double *dst, double s);
/// @brief Normalize an XYZW quaternion in place.
/// @param q Quaternion to normalize.
void quat_normalize(double *q);
/// @brief Integrate world angular velocity into an orientation.
/// @param orientation XYZW orientation updated in place.
/// @param angular_velocity World angular velocity.
/// @param dt Positive elapsed seconds.
void quat_integrate(double *orientation, const double *angular_velocity, double dt);
/// @brief Sanitize an absolute scale while preserving zero.
/// @param value Scale component.
/// @return Finite bounded absolute scale.
double pose_abs_scale_or_zero(double value);
/// @brief Choose bounded sphere samples for a capsule axis.
/// @param axis_len Capsule central-axis length.
/// @param radius Capsule radius.
/// @return Bounded sample count.
int capsule_axis_sample_count(double axis_len, double radius);
/// @brief Compute the endpoints of an oriented capsule axis.
/// @param b Capsule proxy body.
/// @param a Negative-axis endpoint output.
/// @param c Positive-axis endpoint output.
void capsule_axis_endpoints(const rt_body3d *b, double *a, double *c);
/// @brief Compute a body's world-space AABB.
/// @param b Body3D to bound.
/// @param mn Minimum-corner output.
/// @param mx Maximum-corner output.
void body_aabb(const rt_body3d *b, double *mn, double *mx);
/// @brief Test whether a body has usable collision geometry.
/// @param body Body3D to inspect.
/// @return One when collision geometry is present, otherwise zero.
int body3d_has_collision_geometry(const rt_body3d *body);
/// @brief Test whether every vector component is finite.
/// @param v Three-component vector.
/// @return One when all components are finite, otherwise zero.
int ph3d_vec3_all_finite(const double v[3]);
/// @brief Sanitize a scalar as finite and non-negative.
/// @param value Candidate value.
/// @param fallback Replacement for invalid input.
/// @return Sanitized non-negative value.
double ph3d_clamp_nonnegative_finite(double value, double fallback);
/// @brief Replace a non-finite scalar with a fallback.
/// @param value Candidate value.
/// @param fallback Replacement for invalid input.
/// @return Finite selected value.
double ph3d_finite_or(double value, double fallback);
/// @brief Saturate all vector components to the physics state range.
/// @param v Vector updated in place.
void ph3d_vec3_sanitize_state(double *v);

// --- Body dynamics (defined in rt_physics3d.c) ---
/// @brief Invalidate structural and query broadphase state for a body.
/// @param body Body3D whose bounds-relevant state changed.
void body3d_touch_broadphase(rt_body3d *body);
/// @brief Mark a pose-only body move for lazy query broadphase validation.
/// @param body Body3D whose pose changed.
void body3d_touch_broadphase_moved(rt_body3d *body);
/// @brief Wake a dynamic body.
/// @param b Body3D to wake when dynamic.
void body3d_wake_if_dynamic(rt_body3d *b);
/// @brief Refresh cached primitive dimensions from the bound collider.
/// @param body Body3D whose cache is rebuilt.
void body3d_update_shape_cache_from_collider(rt_body3d *body);
/// @brief Compute velocity at a contact offset.
/// @param b Source Body3D.
/// @param r Offset from center of mass.
/// @param out World velocity output.
void body3d_contact_velocity(const rt_body3d *b, const double *r, double *out);
/// @brief Compute effective inverse mass along a contact direction.
/// @param b Dynamic Body3D.
/// @param r Contact offset.
/// @param dir Impulse direction.
/// @return Non-negative linear-plus-angular denominator.
double body3d_contact_impulse_denominator(const rt_body3d *b, const double *r, const double *dir);
/// @brief Apply a world impulse at a contact offset.
/// @param b Dynamic Body3D.
/// @param impulse World linear impulse.
/// @param r Application offset.
void body3d_apply_contact_impulse(rt_body3d *b, const double *impulse, const double *r);
/// @brief Multiply a vector by world inverse inertia.
/// @param b Dynamic Body3D.
/// @param v World vector.
/// @param out World result output.
void body3d_world_inv_inertia_mul(const rt_body3d *b, const double *v, double *out);

// --- World book-keeping / validation (defined in rt_physics3d.c) ---
/// @brief Validate a runtime handle as World3D.
/// @param obj Runtime object handle.
/// @return Typed world pointer, or NULL on mismatch.
rt_world3d *world3d_checked(void *obj);
/// @brief Safely increment a non-negative 32-bit count.
/// @param value Current count.
/// @param out Incremented count output.
/// @return One on success, otherwise zero on overflow/invalid input.
int world3d_checked_increment(int32_t value, int32_t *out);
/// @brief Ensure public contact-array capacity.
/// @param w World3D to grow.
/// @param needed Required slots.
/// @return One when capacity is available, otherwise zero.
int world3d_reserve_contacts(rt_world3d *w, int32_t needed);
/// @brief Ensure frame-contact aggregate capacity.
/// @param w World3D to grow.
/// @param needed Required slots.
/// @return One when capacity is available, otherwise zero.
int world3d_reserve_frame_contacts(rt_world3d *w, int32_t needed);
/// @brief Ensure solver broadphase entry capacity.
/// @param w World3D to grow.
/// @param needed Required slots.
/// @return One when capacity is available, otherwise zero.
int world3d_reserve_broadphase_capacity(rt_world3d *w, int32_t needed);
/// @brief Ensure persistent query broadphase capacity.
/// @param w World3D to grow.
/// @param needed Required slots.
/// @return One when capacity is available, otherwise zero.
int world3d_reserve_query_broadphase_capacity(rt_world3d *w, int32_t needed);
/// @brief Acquire reusable world-owned step scratch storage.
/// @param w World3D providing scratch slots.
/// @param slot Scratch slot index.
/// @param bytes Required byte count.
/// @param zeroed Nonzero to clear the acquired range.
/// @return Borrowed scratch pointer, or NULL on failure.
void *world3d_step_scratch_acquire(rt_world3d *w, int32_t slot, size_t bytes, int zeroed);
/// @brief Free all persistent step-scratch buffers.
/// @param w World3D whose scratch storage is released.
void world3d_step_scratch_free_all(rt_world3d *w);
/// @brief Record a step-path failure without escaping an active transaction.
/// @details During `World3D.Step`, the first message is retained for the outer
///   transaction boundary, which restores simulation state before trapping.
///   Outside a transaction this preserves the historical immediate trap.
/// @param w Borrowed world, or `NULL` when no transaction can be active.
/// @param message Static diagnostic text passed to the public trap boundary.
void world3d_step_fail(rt_world3d *w, const char *message);
/// @brief Defer broadphase-fallback telemetry until the active step commits.
/// @details Transactional steps accumulate pending events so a later rollback
///   does not publish diagnostics for simulation work that never committed.
/// @param w World3D whose pending fallback count is incremented.
void world3d_note_broadphase_fallback(rt_world3d *w);
/// @brief Publish and clear broadphase-fallback telemetry for a successful step.
/// @details Saturates the world counter and emits one global diagnostic per
///   pending fallback. A failed transaction clears pending work by rollback.
/// @param w World3D whose pending telemetry is committed.
void world3d_commit_broadphase_fallbacks(rt_world3d *w);
/// @brief Toggle deterministic broadphase allocation failure for internal tests.
/// @param enabled Nonzero to force failure, zero to disable it.
void rt_world3d_test_set_broadphase_alloc_failure(int8_t enabled);
/// @brief Enable deterministic failure after integration for rollback tests.
/// @details This internal-only hook leaves production behavior untouched and
///   forces the active Step transaction through its ordinary failure boundary.
/// @param enabled Nonzero to force failure, zero to disable it.
void rt_world3d_test_set_step_failure(int8_t enabled);
/// @brief Query the deterministic post-integration Step failure test hook.
/// @return Non-zero only while the internal test hook is enabled.
int world3d_test_step_failure_requested(void);
/// @brief Box a raw query-hit snapshot.
/// @param src Raw hit record to copy.
/// @return Newly allocated PhysicsHit3D, or NULL on failure.
void *physics_hit3d_new(const rt_query_hit3d *src);
/// @brief Build a lazy boxed hit list from raw hit snapshots.
/// @param hits Raw retained hits.
/// @param count Retained hit count.
/// @param total_count Pre-truncation match count.
/// @param truncated Nonzero when matches were omitted.
/// @return Newly allocated PhysicsHitList3D, or NULL on failure.
void *physics_hit_list3d_new_ex(const rt_query_hit3d *hits,
                                int32_t count,
                                int64_t total_count,
                                int8_t truncated);

// --- Narrow-phase + contact manifolds (defined in rt_physics3d_collision.c) ---
/// @brief Apply bidirectional body layer/mask filtering.
/// @param a First Body3D.
/// @param b Second Body3D.
/// @return One when both masks accept the opposite layer, otherwise zero.
int bodies_can_collide(const rt_body3d *a, const rt_body3d *b);
/// @brief Apply one-sided query mask filtering.
/// @param body Candidate Body3D.
/// @param mask Query layer mask.
/// @return One when the body's layer matches, otherwise zero.
int query_mask_matches_body(const rt_body3d *body, int64_t mask);
/// @brief Test two raw AABBs for overlap.
/// @param amn First minimum corner.
/// @param amx First maximum corner.
/// @param bmn Second minimum corner.
/// @param bmx Second maximum corner.
/// @return One when the boxes overlap, otherwise zero.
int aabb_overlap_raw(const double *amn, const double *amx, const double *bmn, const double *bmx);
/// @brief Compute the union AABB of a translated starting box.
/// @param start_min Initial minimum corner.
/// @param start_max Initial maximum corner.
/// @param delta Translation vector.
/// @param swept_min Swept minimum output.
/// @param swept_max Swept maximum output.
void swept_aabb_from_points(const double *start_min,
                            const double *start_max,
                            const double *delta,
                            double *swept_min,
                            double *swept_max);
/// @brief Compute a unit triangle normal.
/// @param a First vertex.
/// @param b Second vertex.
/// @param c Third vertex.
/// @param normal Unit-normal output.
void triangle_normal(const double *a, const double *b, const double *c, double *normal);
/// @brief Mirror a posed primitive collider into a transient body proxy.
/// @param pose Collider world pose.
/// @param collider Primitive Collider3D.
/// @param out Proxy Body3D output.
/// @return One for a supported primitive, otherwise zero.
int build_simple_proxy(const rt_collider_pose *pose, void *collider, rt_body3d *out);
/// @brief Copy a complete contact snapshot.
/// @param dst Destination contact.
/// @param src Source contact.
void contact_snapshot_copy(rt_contact3d *dst, const rt_contact3d *src);
/// @brief Initialize a single-point manifold.
/// @param contact Contact to initialize.
/// @param point World contact point.
/// @param normal A-to-B normal.
/// @param separation Signed separation.
void contact3d_init_single_point(rt_contact3d *contact,
                                 const double *point,
                                 const double *normal,
                                 double separation);
/// @brief Append a finite unique point to a bounded manifold.
/// @param contact Contact to expand.
/// @param point Candidate world point.
/// @param normal A-to-B normal.
/// @param separation Signed separation.
void contact3d_try_add_manifold_point(rt_contact3d *contact,
                                      const double *point,
                                      const double *normal,
                                      double separation);
/// @brief Expand an axis-aligned box pair manifold.
/// @param contact Contact to expand.
/// @param a First Body3D.
/// @param b Second Body3D.
void contact3d_expand_aabb_manifold(rt_contact3d *contact, const rt_body3d *a, const rt_body3d *b);
/// @brief Expand a lengthwise capsule manifold.
/// @param contact Contact to expand.
/// @param a First Body3D.
/// @param b Second Body3D.
void contact3d_expand_capsule_manifold(rt_contact3d *contact,
                                       const rt_body3d *a,
                                       const rt_body3d *b);
/// @brief Add distinct touching compound leaf points.
/// @param contact Contact to expand.
/// @param a First Body3D.
/// @param b Second Body3D.
void contact3d_expand_compound_manifold(rt_contact3d *contact,
                                        const rt_body3d *a,
                                        const rt_body3d *b);
/// @brief Expand a box pair through OBB face clipping.
/// @param contact Contact to expand.
/// @param a_leaf First box leaf.
/// @param a_pose First leaf pose.
/// @param b_leaf Second box leaf.
/// @param b_pose Second leaf pose.
/// @return One when a multipoint manifold is produced, otherwise zero.
int contact3d_expand_obb_manifold(rt_contact3d *contact,
                                  void *a_leaf,
                                  const rt_collider_pose *a_pose,
                                  void *b_leaf,
                                  const rt_collider_pose *b_pose);
/// @brief Expand mesh/heightfield contact from solid support vertices.
/// @param contact Contact to expand.
/// @param a_leaf First leaf collider.
/// @param a_pose First leaf pose.
/// @param b_leaf Second leaf collider.
/// @param b_pose Second leaf pose.
/// @return One when support points are added, otherwise zero.
int contact3d_expand_surface_support_manifold(rt_contact3d *contact,
                                              void *a_leaf,
                                              const rt_collider_pose *a_pose,
                                              void *b_leaf,
                                              const rt_collider_pose *b_pose);
/// @brief Compare unordered body and collider pair identity.
/// @param a First contact.
/// @param b Second contact.
/// @return One for the same pair, otherwise zero.
int contact_pair_equals(const rt_contact3d *a, const rt_contact3d *b);
/// @brief Build a scratch-backed contact-pair hash table.
/// @param w World3D providing scratch storage.
/// @param scratch_slot Scratch slot index.
/// @param contacts Contacts to index.
/// @param count Contact count.
/// @param out_capacity Table-capacity output.
/// @return Borrowed scratch table, or NULL on failure.
contact_pair_hash_entry *contact_pair_table_build_scratch(rt_world3d *w,
                                                          int32_t scratch_slot,
                                                          const rt_contact3d *contacts,
                                                          int32_t count,
                                                          int32_t *out_capacity);
/// @brief Allocate a contact-pair hash table.
/// @param contacts Contacts to index.
/// @param count Contact count.
/// @param out_capacity Table-capacity output.
/// @return Caller-owned table, or NULL on failure.
contact_pair_hash_entry *contact_pair_table_build(const rt_contact3d *contacts,
                                                  int32_t count,
                                                  int32_t *out_capacity);
/// @brief Test contact-pair hash membership.
/// @param table Hash table.
/// @param capacity Table capacity.
/// @param needle Pair identity to find.
/// @return One when present, otherwise zero.
int contact_pair_table_contains(const contact_pair_hash_entry *table,
                                int32_t capacity,
                                const rt_contact3d *needle);
/// @brief Find a stored contact by pair identity.
/// @param table Hash table.
/// @param capacity Table capacity.
/// @param needle Pair identity to find.
/// @return Borrowed matching contact, or NULL.
const rt_contact3d *contact_pair_table_find(const contact_pair_hash_entry *table,
                                            int32_t capacity,
                                            const rt_contact3d *needle);
/// @brief Append or replace a unique frame contact.
/// @param w World3D aggregate owner.
/// @param contact Contact snapshot.
/// @return One on success, otherwise zero.
int world3d_append_frame_contact_unique(rt_world3d *w, const rt_contact3d *contact);
/// @brief Publish frame contacts to the public list.
/// @param w World3D to publish.
/// @return One on success, otherwise zero.
int world3d_publish_frame_contacts(rt_world3d *w);
/// @brief Test a body pair and return its deepest contact.
/// @param a First Body3D.
/// @param b Second Body3D.
/// @param normal A-to-B normal output.
/// @param depth Penetration output.
/// @param point Optional contact-point output.
/// @param leaf_a_out Optional first leaf output.
/// @param leaf_b_out Optional second leaf output.
/// @param leaf_a_pose_out Optional first leaf pose output.
/// @param leaf_b_pose_out Optional second leaf pose output.
/// @return One on collision, otherwise zero.
int test_collision(const rt_body3d *a,
                   const rt_body3d *b,
                   double *normal,
                   double *depth,
                   double *point,
                   void **leaf_a_out,
                   void **leaf_b_out,
                   rt_collider_pose *leaf_a_pose_out,
                   rt_collider_pose *leaf_b_pose_out);
/// @brief Test a body pair while accumulating compound manifold points.
/// @param a First Body3D.
/// @param b Second Body3D.
/// @param normal A-to-B normal output.
/// @param depth Penetration output.
/// @param point Optional contact-point output.
/// @param leaf_a_out Optional first leaf output.
/// @param leaf_b_out Optional second leaf output.
/// @param leaf_a_pose_out Optional first leaf pose output.
/// @param leaf_b_pose_out Optional second leaf pose output.
/// @param manifold_acc Optional manifold accumulator.
/// @return One on collision, otherwise zero.
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

// --- Solver + broad phase (defined in rt_physics3d_solver.c) ---
/// @brief Compare broadphase entries by minimum X.
/// @param lhs First entry.
/// @param rhs Second entry.
/// @return Negative, zero, or positive ordering result.
int ph3d_broadphase_compare_min_x(const void *lhs, const void *rhs);
/// @brief Compare entries lexicographically from a selected primary axis.
/// @param a First entry.
/// @param b Second entry.
/// @param primary Primary axis index.
/// @return Negative, zero, or positive ordering result.
int ph3d_broadphase_compare_entries_axis(const ph3d_broadphase_entry *a,
                                         const ph3d_broadphase_entry *b,
                                         int primary);
/// @brief Ensure merge-sort scratch capacity.
/// @param w World3D owning scratch.
/// @param needed Required entries.
/// @return One on success, otherwise zero.
int world3d_reserve_broadphase_sort_scratch(rt_world3d *w, int32_t needed);
/// @brief Deterministically sort broadphase entries.
/// @param entries Entries to sort.
/// @param scratch Equal-size scratch buffer.
/// @param count Entry count.
/// @param axis Primary sort axis.
void ph3d_broadphase_sort_entries(ph3d_broadphase_entry *entries,
                                  ph3d_broadphase_entry *scratch,
                                  int32_t count,
                                  int axis);
/// @brief Push an integer onto a growable stack.
/// @param items Address of item storage.
/// @param count In/out item count.
/// @param capacity In/out capacity.
/// @param value Value to append.
/// @return One on success, otherwise zero.
int ph3d_i32_stack_push(int32_t **items, int32_t *count, int32_t *capacity, int32_t value);
/// @brief Release all solver-island batch arrays.
/// @param batch Batch to clear.
void ph3d_solver_island_batch_free(ph3d_solver_island_batch *batch);
/// @brief Partition active contacts into independent solver islands.
/// @param w World3D to analyze.
/// @param batch Output batch.
/// @return One on success, otherwise zero.
int world3d_build_solver_island_batch(rt_world3d *w, ph3d_solver_island_batch *batch);
/// @brief Apply cached impulses across solver islands.
/// @param w World3D containing contacts.
/// @param batch Island partition.
void world3d_warm_start_solver_islands(rt_world3d *w, const ph3d_solver_island_batch *batch);
/// @brief Solve velocity constraints across islands.
/// @param batch Island partition.
/// @param w World3D containing bodies and contacts.
void world3d_solve_velocity_solver_islands(const ph3d_solver_island_batch *batch, rt_world3d *w);
/// @brief Solve positional constraints across islands.
/// @param batch Island partition.
/// @param w World3D containing bodies and contacts.
void world3d_solve_position_solver_islands(const ph3d_solver_island_batch *batch, rt_world3d *w);
/// @brief Solve a separated time-of-impact constraint through the ordinary material path.
/// @details Initializes restitution and friction exactly like a detected contact,
///   then runs the configured velocity iterations without positional correction.
/// @param w Borrowed owning world providing solver configuration and prior contacts.
/// @param contact Caller-owned, single-point TOI manifold; impulse accumulators are updated.
void world3d_solve_toi_contact(rt_world3d *w, rt_contact3d *contact);
/// @brief Detect and aggregate current world contacts.
/// @param w World3D to scan.
/// @return One on success, otherwise zero.
int world3d_detect_contacts(rt_world3d *w);
/// @brief Finalize material/contact manifold state after detection.
/// @param w World3D whose contacts are finalized.
void world3d_finalize_contacts(rt_world3d *w);
/// @brief Advance automatic sleep state.
/// @param w World3D whose bodies are updated.
/// @param sub_dt Positive substep duration.
void world3d_update_sleep(rt_world3d *w, double sub_dt);

// --- Raycast / sweep / overlap (defined in rt_physics3d_query.c) ---
/// @brief Ensure a mesh physics BVH is current.
/// @param mesh Mesh3D to update.
/// @return One when a valid BVH is available, otherwise zero.
int mesh_physics_bvh_rebuild(rt_mesh3d *mesh);
/// @brief Transform local AABB corners into a conservative world AABB.
/// @param pose World pose.
/// @param mn Local minimum corner.
/// @param mx Local maximum corner.
/// @param out_min World minimum output.
/// @param out_max World maximum output.
void transform_local_aabb_to_world(const rt_collider_pose *pose,
                                   const float *mn,
                                   const float *mx,
                                   double *out_min,
                                   double *out_max);

#ifdef __cplusplus
}
#endif

#endif /* ZANNA_ENABLE_GRAPHICS */
