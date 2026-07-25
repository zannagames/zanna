//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_world.inc  (#included from rt_physics3d.c; not
// a standalone translation unit) Purpose: Physics3D World3D lifecycle, stepping, and integration
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements `World3D` ownership, transactional stepping, diagnostics, and registration.
///
/// This implementation fragment is included by `rt_physics3d.c`. It owns world construction
/// and teardown, atomic variable/fixed stepping, body and joint membership, solver and CCD
/// configuration, diagnostic counters, query capacity, gravity, and floating-origin rebasing.

/*==========================================================================
 * World3D
 *=========================================================================*/

/// @brief GC finalizer for `World3D` — release every body and joint.
///
/// Walks the body and joint arrays releasing each owned reference. The
/// world has strong refs to its bodies/joints so dropping the world
/// frees the entire simulation state.
/// @param obj Owned `rt_world3d` runtime payload being finalized; null is a no-op.
static void world3d_finalizer(void *obj) {
    rt_world3d *w = (rt_world3d *)obj;
    if (!w)
        return;
    int32_t body_count = world3d_body_count_safe(w);
    int32_t joint_count = world3d_joint_count_safe(w);
    for (int32_t i = 0; i < body_count; i++) {
        rt_body3d *body = w->bodies[i];
        if (body && body->owner_world == w) {
            body->owner_world = NULL;
            body->owner_index = -1;
        }
        if (body && rt_obj_release_check0(body))
            rt_obj_free(body);
        w->bodies[i] = NULL;
    }
    for (int32_t i = 0; i < joint_count; i++) {
        if (w->joints[i] && rt_obj_release_check0(w->joints[i]))
            rt_obj_free(w->joints[i]);
        w->joints[i] = NULL;
    }
    w->body_count = 0;
    w->contact_count = 0;
    w->frame_contact_count = 0;
    w->previous_contact_count = 0;
    w->enter_event_count = 0;
    w->stay_event_count = 0;
    w->exit_event_count = 0;
    w->joint_count = 0;
    world3d_release_cached_event_objects(w);
    free(w->bodies);
    free(w->contacts);
    free(w->frame_contacts);
    free(w->previous_contacts);
    free(w->enter_events);
    free(w->stay_events);
    free(w->exit_events);
    free(w->collision_event_objects);
    free(w->enter_event_objects);
    free(w->stay_event_objects);
    free(w->exit_event_objects);
    free(w->joints);
    free(w->joint_types);
    free(w->broadphase_entries);
    free(w->broadphase_sort_scratch);
    free(w->query_broadphase_entries);
    free(w->query_hits_scratch);
    w->query_hits_scratch = NULL;
    world3d_step_scratch_free_all(w);
    if (w->solver_pool) {
        /* Same owned-pool teardown as the software rasterizer: drain, then drop
         * the runtime object reference. */
        void *pool = w->solver_pool;
        w->solver_pool = NULL;
        rt_threadpool_shutdown(pool);
        if (rt_obj_release_check0(pool))
            rt_obj_free(pool);
    }
    if (w->query_sphere_collider && rt_obj_release_check0(w->query_sphere_collider))
        rt_obj_free(w->query_sphere_collider);
    if (w->query_box_collider && rt_obj_release_check0(w->query_box_collider))
        rt_obj_free(w->query_box_collider);
    w->bodies = NULL;
    w->contacts = NULL;
    w->frame_contacts = NULL;
    w->previous_contacts = NULL;
    w->enter_events = NULL;
    w->stay_events = NULL;
    w->exit_events = NULL;
    w->collision_event_objects = NULL;
    w->enter_event_objects = NULL;
    w->stay_event_objects = NULL;
    w->exit_event_objects = NULL;
    w->joints = NULL;
    w->joint_types = NULL;
    w->broadphase_entries = NULL;
    w->broadphase_sort_scratch = NULL;
    w->query_broadphase_entries = NULL;
    w->query_sphere_collider = NULL;
    w->query_box_collider = NULL;
    w->query_hits_scratch = NULL;
    w->max_query_hits = PH3D_MAX_QUERY_HITS;
    w->query_broadphase_count = 0;
    w->query_broadphase_signature = 0;
    w->query_broadphase_mesh_epoch = 0;
    w->query_broadphase_collider_epoch = 0;
    w->query_broadphase_valid = 0;
    w->query_broadphase_dirty = 0;
    w->query_broadphase_rebuild_count = 0;
    w->query_broadphase_entry_capacity = 0;
    w->broadphase_fallback_count = 0;
    w->last_ccd_requested_substeps = 1;
    w->last_ccd_substeps = 1;
    w->ccd_substep_clamped_count = 0;
    w->last_ccd_clamped_body_count = 0;
    w->ccd_substep_clamped_body_count = 0;
    w->ccd_toi_count = 0;
    w->last_integration_pass_count = 0;
    w->step_failure_message = NULL;
    w->step_pending_broadphase_fallbacks = 0;
    w->step_transaction_active = 0;
    w->body_capacity = 0;
    w->contact_capacity = 0;
    w->frame_contact_capacity = 0;
    w->previous_contact_capacity = 0;
    w->enter_event_capacity = 0;
    w->stay_event_capacity = 0;
    w->exit_event_capacity = 0;
    w->collision_event_object_capacity = 0;
    w->enter_event_object_capacity = 0;
    w->stay_event_object_capacity = 0;
    w->exit_event_object_capacity = 0;
    w->joint_capacity = 0;
    w->broadphase_capacity = 0;
    w->broadphase_sort_scratch_capacity = 0;
    w->broadphase_world_revision = 1;
    w->query_broadphase_count = 0;
    w->query_broadphase_signature = 0;
    w->query_broadphase_mesh_epoch = 0;
    w->query_broadphase_collider_epoch = 0;
    w->query_broadphase_valid = 0;
}

/// @brief `Physics3D.World.New(gx, gy, gz)` — construct an empty world with gravity.
///
/// Allocates an `rt_world3d`, initializes the gravity vector, and zeros
/// every contact / event / joint table. Body, contact, and joint arrays
/// start at production-sized defaults and grow on demand. Hooks the GC
/// finalizer so dropping the world frees the simulation.
///
/// @param gx Initial world-gravity x acceleration in metres per second squared.
/// @param gy Initial world-gravity y acceleration in metres per second squared.
/// @param gz Initial world-gravity z acceleration in metres per second squared.
/// @return Owned `World3D` handle, or null after trapping on object or initial-storage failure.
void *rt_world3d_new(double gx, double gy, double gz) {
    rt_world3d *w =
        (rt_world3d *)rt_obj_new_i64(RT_G3D_WORLD3D_CLASS_ID, (int64_t)sizeof(rt_world3d));
    if (!w) {
        rt_trap("Physics3D.World.New: allocation failed");
        return NULL;
    }
    w->vptr = NULL;
    ph3d_vec3_set_finite(w->gravity, gx, gy, gz);
    w->max_query_hits = PH3D_MAX_QUERY_HITS;
    w->body_count = 0;
    w->contact_count = 0;
    w->frame_contact_count = 0;
    w->previous_contact_count = 0;
    w->enter_event_count = 0;
    w->stay_event_count = 0;
    w->exit_event_count = 0;
    w->joint_count = 0;
    w->collision_event_object_capacity = 0;
    w->enter_event_object_capacity = 0;
    w->stay_event_object_capacity = 0;
    w->exit_event_object_capacity = 0;
    w->body_capacity = 0;
    w->contact_capacity = 0;
    w->frame_contact_capacity = 0;
    w->previous_contact_capacity = 0;
    w->enter_event_capacity = 0;
    w->stay_event_capacity = 0;
    w->exit_event_capacity = 0;
    w->joint_capacity = 0;
    w->broadphase_capacity = 0;
    w->broadphase_sort_scratch_capacity = 0;
    w->broadphase_world_revision = 1;
    w->query_broadphase_count = 0;
    w->query_broadphase_signature = 0;
    w->query_broadphase_mesh_epoch = 0;
    w->query_broadphase_collider_epoch = 0;
    w->query_broadphase_valid = 0;
    w->query_broadphase_dirty = 0;
    w->query_broadphase_rebuild_count = 0;
    w->query_broadphase_entry_capacity = 0;
    w->broadphase_fallback_count = 0;
    w->bodies = NULL;
    w->contacts = NULL;
    w->frame_contacts = NULL;
    w->previous_contacts = NULL;
    w->enter_events = NULL;
    w->stay_events = NULL;
    w->exit_events = NULL;
    w->collision_event_objects = NULL;
    w->enter_event_objects = NULL;
    w->stay_event_objects = NULL;
    w->exit_event_objects = NULL;
    w->joints = NULL;
    w->joint_types = NULL;
    w->broadphase_entries = NULL;
    w->broadphase_sort_scratch = NULL;
    w->query_broadphase_entries = NULL;
    for (int32_t scratch_slot = 0; scratch_slot < PH3D_WORLD_SCRATCH_SLOTS; scratch_slot++) {
        w->step_scratch[scratch_slot] = NULL;
        w->step_scratch_capacity[scratch_slot] = 0;
    }
    w->broadphase_world_revision = 1;
    w->solver_iterations = PH3D_DEFAULT_SOLVER_ITERATIONS;
    w->position_iterations = PH3D_DEFAULT_POSITION_ITERATIONS;
    w->contact_beta = PH3D_DEFAULT_CONTACT_BETA;
    w->restitution_threshold = PH3D_DEFAULT_RESTITUTION_THRESHOLD;
    w->fixed_step_accumulator = 0.0;
    w->fixed_step_alpha = 0.0;
    w->dropped_fixed_steps = 0;
    w->last_integration_pass_count = 0;
    w->step_failure_message = NULL;
    w->step_pending_broadphase_fallbacks = 0;
    w->step_transaction_active = 0;
    if (!world3d_reserve_body_capacity(w, PH3D_INITIAL_BODIES) ||
        !world3d_reserve_contacts(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_frame_contacts(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_previous_contacts(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_enter_events(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_stay_events(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_exit_events(w, PH3D_INITIAL_CONTACTS) ||
        !world3d_reserve_joint_capacity(w, PH3D_INITIAL_JOINTS) ||
        !world3d_reserve_broadphase_capacity(w, PH3D_INITIAL_BODIES) ||
        !world3d_reserve_broadphase_sort_scratch(w, PH3D_INITIAL_BODIES)) {
        world3d_finalizer(w);
        if (rt_obj_release_check0(w))
            rt_obj_free(w);
        rt_trap("Physics3D.World.New: storage allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(w, world3d_finalizer);
    return w;
}

/// @brief Reset per-step CCD diagnostics before local body segmentation begins.
/// @details The simulation itself always performs one world integration/contact/
///   joint pass. `LastCcd*Substeps` now reports the largest local segment demand
///   and bounded segment count observed on any opted-in body.
/// @param[in,out] w Borrowed world whose last-step diagnostic fields are reset.
static void world3d_begin_ccd_diagnostics(rt_world3d *w) {
    if (!w)
        return;
    w->last_ccd_requested_substeps = 1;
    w->last_ccd_substeps = 1;
    w->last_ccd_clamped_body_count = 0;
}

/// @brief Compute and record a bounded local segment count for one CCD body.
/// @param w Borrowed world whose per-step diagnostics are updated.
/// @param move_length Finite translation length for this body and step.
/// @param threshold Positive body-local CCD feature threshold.
/// @return A count in `[1, PH3D_MAX_CCD_BODY_SEGMENTS]`.
static int32_t world3d_ccd_body_segment_count(rt_world3d *w, double move_length, double threshold) {
    double requested_f;
    int32_t requested;
    int32_t actual;
    if (!w || !isfinite(move_length) || move_length <= 0.0 || !isfinite(threshold) ||
        threshold <= 1e-6)
        return 1;
    requested_f = ceil(move_length / threshold);
    if (!isfinite(requested_f) || requested_f >= (double)INT32_MAX)
        requested = INT32_MAX;
    else if (requested_f <= 1.0)
        requested = 1;
    else
        requested = (int32_t)requested_f;
    actual = requested > PH3D_MAX_CCD_BODY_SEGMENTS ? PH3D_MAX_CCD_BODY_SEGMENTS : requested;
    if (requested > w->last_ccd_requested_substeps)
        w->last_ccd_requested_substeps = requested;
    if (actual > w->last_ccd_substeps)
        w->last_ccd_substeps = actual;
    if (requested > actual && w->last_ccd_clamped_body_count < INT32_MAX)
        w->last_ccd_clamped_body_count++;
    return actual;
}

/// @brief Commit aggregate local-segmentation clamp telemetry after integration.
/// @param[in,out] w Borrowed world whose per-step clamp count is accumulated into lifetime
///        diagnostics. A null world or zero clamped bodies is a no-op.
static void world3d_finish_ccd_diagnostics(rt_world3d *w) {
    int32_t clamped;
    if (!w || w->last_ccd_clamped_body_count <= 0)
        return;
    clamped = w->last_ccd_clamped_body_count;
    if (w->ccd_substep_clamped_count < INT64_MAX)
        w->ccd_substep_clamped_count++;
    if (w->ccd_substep_clamped_body_count <= INT64_MAX - clamped)
        w->ccd_substep_clamped_body_count += clamped;
    else
        w->ccd_substep_clamped_body_count = INT64_MAX;
    rt_game3d_diag_record_ccd_clamp(clamped);
}

/// @brief Derive a conservative swept-sphere proxy that encloses the complete collider.
/// @details Spheres keep their exact radius, capsules use the exact enclosing
///   radius `height/2`, and boxes/convexes/meshes use the half-diagonal of their
///   transformed world AABB. The AABB center also captures non-origin local
///   geometry. This cannot omit corners the former max-half-extent proxy missed.
/// @param body Borrowed collision-capable body.
/// @param[out] center Three-element destination for the proxy center in world coordinates.
/// @param[out] radius Destination for the positive enclosing radius in world units.
/// @return `1` with finite center/radius, otherwise `0`.
static int world3d_ccd_body_proxy(const rt_body3d *body, double center[3], double *radius) {
    double bounds_min[3];
    double bounds_max[3];
    double half[3];
    if (!body || !center || !radius || !body3d_has_collision_geometry(body))
        return 0;
    body_aabb(body, bounds_min, bounds_max);
    for (int axis = 0; axis < 3; ++axis) {
        center[axis] = 0.5 * (bounds_min[axis] + bounds_max[axis]);
        half[axis] = 0.5 * (bounds_max[axis] - bounds_min[axis]);
    }
    if (body->shape == PH3D_SHAPE_SPHERE) {
        *radius = fmax(half[0], fmax(half[1], half[2]));
    } else if (body->shape == PH3D_SHAPE_CAPSULE) {
        *radius = fmax(body->height * 0.5, body->radius);
    } else {
        *radius = sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
    }
    return ph3d_vec3_all_finite(center) && isfinite(*radius) && *radius > 1e-6;
}

/// @brief Build, solve, and retain a single swept time-of-impact contact.
/// @details The target is contact side A and the moving body is side B, matching
///   the target-surface normal convention. Query fallbacks with the opposite
///   sign are flipped so relative approach velocity is negative before the
///   ordinary restitution/friction solver runs.
/// @param w Borrowed world providing solver settings and frame-contact storage.
/// @param[in,out] moving Borrowed CCD body whose velocity, grounding, and impulses may change.
/// @param hit Borrowed sweep hit identifying the target body, collider, and contact point.
/// @param[in,out] normal Three-element hit normal, flipped in place when required by the solver's
///        target-to-moving-body convention.
/// @return `1` on success, or `0` if frame-contact publication fails.
static int world3d_resolve_ccd_hit(rt_world3d *w,
                                   rt_body3d *moving,
                                   const rt_query_hit3d *hit,
                                   double normal[3]) {
    rt_contact3d contact;
    double relative_velocity[3];
    double projected_speed;
    if (!w || !moving || !hit || !hit->body || !normal)
        return 1;
    relative_velocity[0] = moving->velocity[0] - hit->body->velocity[0];
    relative_velocity[1] = moving->velocity[1] - hit->body->velocity[1];
    relative_velocity[2] = moving->velocity[2] - hit->body->velocity[2];
    if (vec3_dot(relative_velocity, normal) > 0.0)
        vec3_scale_in_place(normal, -1.0);
    projected_speed = vec3_dot(relative_velocity, normal);
    memset(&contact, 0, sizeof(contact));
    contact.body_a = hit->body;
    contact.body_b = moving;
    contact.collider_a = hit->collider ? hit->collider : hit->body->collider;
    contact.collider_b = moving->collider;
    vec3_copy(contact.point, hit->point);
    vec3_copy(contact.normal, normal);
    contact.separation = 0.0;
    contact3d_init_single_point(&contact, contact.point, contact.normal, 0.0);
    contact.relative_speed = isfinite(projected_speed) ? fabs(projected_speed) : 0.0;
    world3d_solve_toi_contact(w, &contact);
    if (normal[1] > 0.7) {
        moving->is_grounded = 1;
        vec3_copy(moving->ground_normal, normal);
    }
    return world3d_append_frame_contact_unique(w, &contact);
}

/// @brief Clear one-shot force and torque accumulators from every registered body.
/// @param[in,out] w Borrowed world whose non-null bodies are cleared after a committed step.
static void world3d_clear_body_accumulators(rt_world3d *w) {
    if (!w)
        return;
    for (int32_t i = 0; i < w->body_count; i++) {
        rt_body3d *b = w->bodies[i];
        if (!b)
            continue;
        b->force[0] = b->force[1] = b->force[2] = 0.0;
        b->torque[0] = b->torque[1] = b->torque[2] = 0.0;
    }
}

/// @brief Convert a non-negative damping rate into a partition-invariant decay factor.
/// @details Exponential damping composes exactly across substeps:
///   `exp(-rate*a) * exp(-rate*b) == exp(-rate*(a+b))` up to floating-point
///   rounding. Invalid rates are treated as zero; an overflowing exponent is
///   the physically meaningful fully-damped factor zero.
/// @param rate Damping rate in inverse seconds.
/// @param dt Positive integration interval in seconds.
/// @return A finite multiplier in `[0, 1]`.
static double world3d_damping_scale(double rate, double dt) {
    double exponent;
    rate = ph3d_clamp_nonnegative_finite(rate, 0.0);
    if (!isfinite(dt) || dt <= 0.0 || rate <= 0.0)
        return 1.0;
    exponent = -rate * dt;
    if (!isfinite(exponent))
        return 0.0;
    return exp(exponent);
}

/// @brief Snapshot storage and scalar metadata for one atomic World3D step.
/// @details Array payloads point into the dedicated persistent transaction
///   scratch slot. Capacities may grow during a failed step and remain grown,
///   but every observable element/count and every body payload is restored.
typedef struct ph3d_step_transaction {
    rt_body3d *body_states;
    rt_contact3d *contacts;
    rt_contact3d *frame_contacts;
    rt_contact3d *previous_contacts;
    rt_contact3d *enter_events;
    rt_contact3d *stay_events;
    rt_contact3d *exit_events;
    int32_t body_count;
    int32_t contact_count;
    int32_t frame_contact_count;
    int32_t previous_contact_count;
    int32_t enter_event_count;
    int32_t stay_event_count;
    int32_t exit_event_count;
    uint64_t broadphase_world_revision;
    uint64_t query_broadphase_signature;
    int32_t query_broadphase_count;
    int8_t query_broadphase_valid;
    int8_t query_broadphase_dirty;
    int64_t broadphase_fallback_count;
    int32_t last_ccd_requested_substeps;
    int32_t last_ccd_substeps;
    int64_t ccd_substep_clamped_count;
    int32_t last_ccd_clamped_body_count;
    int64_t ccd_substep_clamped_body_count;
    int64_t ccd_toi_count;
    int32_t last_solver_island_count;
    int32_t last_solver_active_body_count;
    int32_t last_solver_contact_count;
    int32_t last_integration_pass_count;
    int32_t step_pending_broadphase_fallbacks;
    int8_t solver_pool_failed;
    double fixed_step_accumulator;
    double fixed_step_alpha;
    int64_t dropped_fixed_steps;
} ph3d_step_transaction;

/// @brief Append one aligned array region to a checked transaction layout.
/// @details Performs alignment, multiplication, and addition overflow checks
///   before updating @p cursor. Empty arrays consume no bytes but still receive
///   a deterministic offset. This keeps snapshot pointer arithmetic defined for
///   pathological private-state counts.
/// @param count Non-negative element count.
/// @param element_size Size of one array element.
/// @param alignment Required power-of-two-or-ordinary C object alignment.
/// @param cursor In/out total byte cursor.
/// @param offset_out Receives the aligned start offset.
/// @return `1` on success, `0` for invalid arguments or size overflow.
static int world3d_step_transaction_add_region(
    int32_t count, size_t element_size, size_t alignment, size_t *cursor, size_t *offset_out) {
    size_t padding;
    size_t bytes;
    if (count < 0 || element_size == 0 || alignment == 0 || !cursor || !offset_out)
        return 0;
    padding = (alignment - (*cursor % alignment)) % alignment;
    if (*cursor > SIZE_MAX - padding)
        return 0;
    *cursor += padding;
    *offset_out = *cursor;
    if ((size_t)count > SIZE_MAX / element_size)
        return 0;
    bytes = (size_t)count * element_size;
    if (*cursor > SIZE_MAX - bytes)
        return 0;
    *cursor += bytes;
    return 1;
}

/// @brief Copy a live contact/event prefix when its count is non-zero.
/// @details Transaction arrays are plain snapshots and do not alter body or
///   collider ownership; contact entries hold borrowed references owned by the
///   world/body registries.
/// @param[out] destination Snapshot array receiving the copied prefix.
/// @param source Borrowed live contact or event array.
/// @param count Number of initialized entries to copy; non-positive values are no-ops.
static void world3d_step_transaction_copy_contacts(rt_contact3d *destination,
                                                   const rt_contact3d *source,
                                                   int32_t count) {
    if (destination && source && count > 0)
        memcpy(destination, source, (size_t)count * sizeof(*destination));
}

/// @brief Capture every state mutation that World3D.Step may publish.
/// @details A single persistent scratch allocation stores full body payloads
///   plus all contact/event array prefixes. Scalar diagnostics, query-cache
///   validity, and fixed-step accounting are retained in the local transaction
///   descriptor. Event object pools are intentionally not copied because Step
///   no longer releases them until commit.
/// @param w Borrowed world whose sanitized counts and arrays are snapshotted.
/// @param transaction Caller-owned descriptor initialized on success.
/// @return `1` on success, or `0` without mutating simulation state.
static int world3d_step_transaction_begin(rt_world3d *w, ph3d_step_transaction *transaction) {
    size_t cursor = 0;
    size_t body_offset;
    size_t contact_offset;
    size_t frame_offset;
    size_t previous_offset;
    size_t enter_offset;
    size_t stay_offset;
    size_t exit_offset;
    uint8_t *storage;
    if (!w || !transaction)
        return 0;
    memset(transaction, 0, sizeof(*transaction));
    transaction->body_count = w->body_count;
    transaction->contact_count = w->contact_count;
    transaction->frame_contact_count = w->frame_contact_count;
    transaction->previous_contact_count = w->previous_contact_count;
    transaction->enter_event_count = w->enter_event_count;
    transaction->stay_event_count = w->stay_event_count;
    transaction->exit_event_count = w->exit_event_count;
    if (!world3d_step_transaction_add_region(transaction->body_count,
                                             sizeof(rt_body3d),
                                             _Alignof(rt_body3d),
                                             &cursor,
                                             &body_offset) ||
        !world3d_step_transaction_add_region(transaction->contact_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &contact_offset) ||
        !world3d_step_transaction_add_region(transaction->frame_contact_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &frame_offset) ||
        !world3d_step_transaction_add_region(transaction->previous_contact_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &previous_offset) ||
        !world3d_step_transaction_add_region(transaction->enter_event_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &enter_offset) ||
        !world3d_step_transaction_add_region(transaction->stay_event_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &stay_offset) ||
        !world3d_step_transaction_add_region(transaction->exit_event_count,
                                             sizeof(rt_contact3d),
                                             _Alignof(rt_contact3d),
                                             &cursor,
                                             &exit_offset))
        return 0;
    if (cursor == 0)
        cursor = 1;
    storage = (uint8_t *)world3d_step_scratch_acquire(w, PH3D_SCRATCH_STEP_TRANSACTION, cursor, 0);
    if (!storage)
        return 0;
    transaction->body_states = (rt_body3d *)(void *)(storage + body_offset);
    transaction->contacts = (rt_contact3d *)(void *)(storage + contact_offset);
    transaction->frame_contacts = (rt_contact3d *)(void *)(storage + frame_offset);
    transaction->previous_contacts = (rt_contact3d *)(void *)(storage + previous_offset);
    transaction->enter_events = (rt_contact3d *)(void *)(storage + enter_offset);
    transaction->stay_events = (rt_contact3d *)(void *)(storage + stay_offset);
    transaction->exit_events = (rt_contact3d *)(void *)(storage + exit_offset);
    for (int32_t i = 0; i < transaction->body_count; ++i)
        transaction->body_states[i] = *w->bodies[i];
    world3d_step_transaction_copy_contacts(
        transaction->contacts, w->contacts, transaction->contact_count);
    world3d_step_transaction_copy_contacts(
        transaction->frame_contacts, w->frame_contacts, transaction->frame_contact_count);
    world3d_step_transaction_copy_contacts(
        transaction->previous_contacts, w->previous_contacts, transaction->previous_contact_count);
    world3d_step_transaction_copy_contacts(
        transaction->enter_events, w->enter_events, transaction->enter_event_count);
    world3d_step_transaction_copy_contacts(
        transaction->stay_events, w->stay_events, transaction->stay_event_count);
    world3d_step_transaction_copy_contacts(
        transaction->exit_events, w->exit_events, transaction->exit_event_count);
    transaction->broadphase_world_revision = w->broadphase_world_revision;
    transaction->query_broadphase_signature = w->query_broadphase_signature;
    transaction->query_broadphase_count = w->query_broadphase_count;
    transaction->query_broadphase_valid = w->query_broadphase_valid;
    transaction->query_broadphase_dirty = w->query_broadphase_dirty;
    transaction->broadphase_fallback_count = w->broadphase_fallback_count;
    transaction->last_ccd_requested_substeps = w->last_ccd_requested_substeps;
    transaction->last_ccd_substeps = w->last_ccd_substeps;
    transaction->ccd_substep_clamped_count = w->ccd_substep_clamped_count;
    transaction->last_ccd_clamped_body_count = w->last_ccd_clamped_body_count;
    transaction->ccd_substep_clamped_body_count = w->ccd_substep_clamped_body_count;
    transaction->ccd_toi_count = w->ccd_toi_count;
    transaction->last_solver_island_count = w->last_solver_island_count;
    transaction->last_solver_active_body_count = w->last_solver_active_body_count;
    transaction->last_solver_contact_count = w->last_solver_contact_count;
    transaction->last_integration_pass_count = w->last_integration_pass_count;
    transaction->step_pending_broadphase_fallbacks = w->step_pending_broadphase_fallbacks;
    transaction->solver_pool_failed = w->solver_pool_failed;
    transaction->fixed_step_accumulator = w->fixed_step_accumulator;
    transaction->fixed_step_alpha = w->fixed_step_alpha;
    transaction->dropped_fixed_steps = w->dropped_fixed_steps;
    return 1;
}

/// @brief Restore a failed World3D step from a completed transaction snapshot.
/// @details Full body payload restoration includes pose, velocities, sleep,
///   grounding, force/torque accumulators, and broadphase revisions. Contact and
///   event arrays regain their exact prefixes and counts. Allocator capacity
///   growth and lazily-created internal caches are harmless retained work.
/// @param w Borrowed world being rolled back.
/// @param transaction Immutable snapshot produced by the begin helper.
static void world3d_step_transaction_restore(rt_world3d *w,
                                             const ph3d_step_transaction *transaction) {
    if (!w || !transaction)
        return;
    for (int32_t i = 0; i < transaction->body_count; ++i)
        *w->bodies[i] = transaction->body_states[i];
    world3d_step_transaction_copy_contacts(
        w->contacts, transaction->contacts, transaction->contact_count);
    world3d_step_transaction_copy_contacts(
        w->frame_contacts, transaction->frame_contacts, transaction->frame_contact_count);
    world3d_step_transaction_copy_contacts(
        w->previous_contacts, transaction->previous_contacts, transaction->previous_contact_count);
    world3d_step_transaction_copy_contacts(
        w->enter_events, transaction->enter_events, transaction->enter_event_count);
    world3d_step_transaction_copy_contacts(
        w->stay_events, transaction->stay_events, transaction->stay_event_count);
    world3d_step_transaction_copy_contacts(
        w->exit_events, transaction->exit_events, transaction->exit_event_count);
    w->body_count = transaction->body_count;
    w->contact_count = transaction->contact_count;
    w->frame_contact_count = transaction->frame_contact_count;
    w->previous_contact_count = transaction->previous_contact_count;
    w->enter_event_count = transaction->enter_event_count;
    w->stay_event_count = transaction->stay_event_count;
    w->exit_event_count = transaction->exit_event_count;
    w->broadphase_world_revision = transaction->broadphase_world_revision;
    w->query_broadphase_signature = transaction->query_broadphase_signature;
    w->query_broadphase_count = transaction->query_broadphase_count;
    w->query_broadphase_valid = transaction->query_broadphase_valid;
    w->query_broadphase_dirty = transaction->query_broadphase_dirty;
    w->broadphase_fallback_count = transaction->broadphase_fallback_count;
    w->last_ccd_requested_substeps = transaction->last_ccd_requested_substeps;
    w->last_ccd_substeps = transaction->last_ccd_substeps;
    w->ccd_substep_clamped_count = transaction->ccd_substep_clamped_count;
    w->last_ccd_clamped_body_count = transaction->last_ccd_clamped_body_count;
    w->ccd_substep_clamped_body_count = transaction->ccd_substep_clamped_body_count;
    w->ccd_toi_count = transaction->ccd_toi_count;
    w->last_solver_island_count = transaction->last_solver_island_count;
    w->last_solver_active_body_count = transaction->last_solver_active_body_count;
    w->last_solver_contact_count = transaction->last_solver_contact_count;
    w->last_integration_pass_count = transaction->last_integration_pass_count;
    w->step_pending_broadphase_fallbacks = transaction->step_pending_broadphase_fallbacks;
    w->solver_pool_failed = transaction->solver_pool_failed;
    w->fixed_step_accumulator = transaction->fixed_step_accumulator;
    w->fixed_step_alpha = transaction->fixed_step_alpha;
    w->dropped_fixed_steps = transaction->dropped_fixed_steps;
}

/// @brief Return whether this step can execute a body-local CCD sweep.
/// @details Scans only valid registered bodies and treats any enabled dynamic
///   body as needing the reusable sphere query collider, even if it is currently
///   asleep; this avoids a later wake introducing allocation after mutation.
/// @param w Borrowed world whose registered bodies are scanned.
/// @return `1` when at least one dynamic collision body has CCD enabled; otherwise `0`.
static int world3d_step_needs_ccd_sweep(const rt_world3d *w) {
    if (!w)
        return 0;
    for (int32_t i = 0; i < w->body_count; ++i) {
        const rt_body3d *body = w->bodies[i];
        if (body && body->use_ccd && body->motion_mode == PH3D_MODE_DYNAMIC &&
            body3d_has_collision_geometry(body))
            return 1;
    }
    return 0;
}

/// @brief Advance one world frame as an all-or-nothing internal transaction.
///
/// Per-substep:
///   1. **Integration**: forces → velocity (symplectic Euler), apply
///      gravity + linear/angular damping, then advance position +
///      orientation. Sleeping bodies skip the step entirely.
///   2. **Collision detection**: sweep-and-prune broad phase, narrow phase
///      via `test_collision`, append contacts, run `resolve_collision` for
///      non-trigger pairs (with a 6-iteration joint solver per substep).
///   3. **Grounded flag** is set when a contact normal points sufficiently
///      upward (Y > 0.7 ≈ 45° slope), used by character controllers.
/// After the substeps, a unique frame-wide contact set is diffed against
/// the previous frame to fire enter/stay/exit events, and per-step
/// forces/torques are zeroed for the next frame.
///
/// @param w Borrowed validated world handle.
/// @param dt Step duration in seconds; invalid/non-positive values are no-ops.
/// @param error_out Optional destination for a static diagnostic on failure.
/// @return `1` after a committed step/no-op, or `0` after complete rollback.
static int world3d_step_internal(rt_world3d *w, double dt, const char **error_out) {
    ph3d_step_transaction transaction;
    const char *failure_message = NULL;
    int transaction_started = 0;
    int step_ok = 0;
    const int substeps = 1;
    double sub_dt;
    if (error_out)
        *error_out = NULL;
    if (!w)
        return 1;
    dt = ph3d_sanitize_step_dt(dt);
    if (dt <= 0.0)
        return 1;
    if (w->step_transaction_active) {
        if (error_out)
            *error_out = "Physics3D.World.Step: reentrant step is not supported";
        return 0;
    }
    w->body_count = world3d_body_count_safe(w);
    w->joint_count = world3d_joint_count_safe(w);
    w->contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    w->frame_contact_count = world3d_contact_count_safe(
        w->frame_contacts, w->frame_contact_count, w->frame_contact_capacity);
    w->previous_contact_count = world3d_contact_count_safe(
        w->previous_contacts, w->previous_contact_count, w->previous_contact_capacity);
    w->enter_event_count =
        world3d_contact_count_safe(w->enter_events, w->enter_event_count, w->enter_event_capacity);
    w->stay_event_count =
        world3d_contact_count_safe(w->stay_events, w->stay_event_count, w->stay_event_capacity);
    w->exit_event_count =
        world3d_contact_count_safe(w->exit_events, w->exit_event_count, w->exit_event_capacity);
    if (world3d_step_needs_ccd_sweep(w) && !world3d_ccd_prepare_sweep_collider(w)) {
        if (error_out)
            *error_out = "Physics3D.World.Step: CCD scratch allocation failed";
        return 0;
    }
    if (!world3d_step_transaction_begin(w, &transaction)) {
        if (error_out)
            *error_out = "Physics3D.World.Step: transaction snapshot allocation failed";
        return 0;
    }
    transaction_started = 1;
    w->step_transaction_active = 1;
    w->step_failure_message = NULL;
    w->step_pending_broadphase_fallbacks = 0;
    sub_dt = dt;
    world3d_begin_ccd_diagnostics(w);
    world3d_invalidate_query_broadphase(w);
    w->frame_contact_count = 0;
    w->last_solver_island_count = 0;
    w->last_solver_active_body_count = 0;
    w->last_solver_contact_count = 0;
    w->last_integration_pass_count = 0;
    for (int32_t i = 0; i < w->body_count; i++) {
        rt_body3d *b = w->bodies[i];
        if (!b)
            continue;
        b->is_grounded = 0;
        vec3_set(b->ground_normal, 0.0, 1.0, 0.0);
    }

    for (int substep = 0; substep < substeps; substep++) {
        w->last_integration_pass_count++;
        /* Phase 1: Integration */
        for (int32_t i = 0; i < w->body_count; i++) {
            rt_body3d *b = w->bodies[i];
            double linear_scale;
            double angular_scale;
            double angular_acceleration[3];
            if (!b)
                continue;

            if (b->motion_mode == PH3D_MODE_STATIC) {
                continue;
            }

            if (b->motion_mode == PH3D_MODE_DYNAMIC) {
                if (b->is_sleeping) {
                    continue;
                }

                ph3d_vec3_accumulate_state(b->velocity,
                                           (w->gravity[0] + b->force[0] * b->inv_mass) * sub_dt,
                                           (w->gravity[1] + b->force[1] * b->inv_mass) * sub_dt,
                                           (w->gravity[2] + b->force[2] * b->inv_mass) * sub_dt);
                body3d_world_inv_inertia_mul(b, b->torque, angular_acceleration);
                ph3d_vec3_accumulate_state(b->angular_velocity,
                                           angular_acceleration[0] * sub_dt,
                                           angular_acceleration[1] * sub_dt,
                                           angular_acceleration[2] * sub_dt);

                linear_scale = world3d_damping_scale(b->linear_damping, sub_dt);
                angular_scale = world3d_damping_scale(b->angular_damping, sub_dt);
                vec3_scale_in_place(b->velocity, linear_scale);
                vec3_scale_in_place(b->angular_velocity, angular_scale);
                ph3d_vec3_sanitize_state(b->velocity);
                ph3d_vec3_sanitize_state(b->angular_velocity);
                /* Sleep is decided post-solve (world3d_update_sleep), not here:
                 * a body resting under gravity carries -g*dt velocity at this
                 * point and only returns to ~0 once contacts are resolved. */
            }

            /* Swept CCD is entirely body-local: opted-in bodies divide only
             * their own translation into bounded TOI segments. Integration,
             * broadphase, joints, and the discrete solver still run exactly
             * once for the world, so an unrelated bullet cannot multiply the
             * simulation cost or alter slow-body damping/solver trajectories. */
            if (b->use_ccd && b->motion_mode == PH3D_MODE_DYNAMIC && !b->is_sleeping) {
                double first_delta[3] = {
                    b->velocity[0] * sub_dt,
                    b->velocity[1] * sub_dt,
                    b->velocity[2] * sub_dt,
                };
                double move_len = vec3_len(first_delta);
                double threshold = body3d_ccd_threshold(b);
                if (isfinite(move_len) && move_len > 1e-12 && threshold > 1e-6) {
                    int32_t segments = world3d_ccd_body_segment_count(w, move_len, threshold);
                    double seg_dt = sub_dt / (double)segments;
                    for (int32_t seg = 0; seg < segments; seg++) {
                        double remaining_dt = seg_dt;
                        /* A bounded impact loop preserves the post-impact portion
                         * of a segment (bounce/slide) without an unbounded corner
                         * ping-pong on pathological geometry. */
                        for (int impact = 0; impact < 4 && remaining_dt > 1e-12; ++impact) {
                            double delta[3] = {
                                b->velocity[0] * remaining_dt,
                                b->velocity[1] * remaining_dt,
                                b->velocity[2] * remaining_dt,
                            };
                            double center[3];
                            double radius = 0.0;
                            double toi = 1.0;
                            double normal[3] = {0.0, 1.0, 0.0};
                            rt_query_hit3d hit = {0};
                            if (!world3d_ccd_body_proxy(b, center, &radius)) {
                                ph3d_vec3_accumulate_state(
                                    b->position, delta[0], delta[1], delta[2]);
                                remaining_dt = 0.0;
                                break;
                            }
                            if (!world3d_ccd_record_trigger_crossings(
                                    w, center, radius, delta, b, remaining_dt)) {
                                world3d_step_fail(
                                    w, "Physics3D.World.Step: trigger-contact allocation failed");
                                goto cleanup;
                            }
                            if (!world3d_ccd_sweep_sphere_raw(w,
                                                              center,
                                                              radius,
                                                              delta,
                                                              b,
                                                              remaining_dt,
                                                              &toi,
                                                              normal,
                                                              &hit) ||
                                toi >= 1.0) {
                                ph3d_vec3_accumulate_state(
                                    b->position, delta[0], delta[1], delta[2]);
                                remaining_dt = 0.0;
                                break;
                            }
                            if (toi < 0.0)
                                toi = 0.0;
                            ph3d_vec3_accumulate_state(
                                b->position, delta[0] * toi, delta[1] * toi, delta[2] * toi);
                            if (!world3d_resolve_ccd_hit(w, b, &hit, normal))
                                goto cleanup;
                            /* One-millimetre skin keeps the next local sweep
                             * separated from the just-hit surface. */
                            ph3d_vec3_accumulate_state(
                                b->position, normal[0] * 1e-3, normal[1] * 1e-3, normal[2] * 1e-3);
                            w->ccd_toi_count++;
                            remaining_dt *= 1.0 - toi;
                            if (!isfinite(remaining_dt) || remaining_dt < 0.0)
                                remaining_dt = 0.0;
                        }
                    }
                    quat_integrate(b->orientation, b->angular_velocity, sub_dt);
                    ph3d_vec3_sanitize_state(b->position);
                    ph3d_vec3_sanitize_state(b->velocity);
                    quat_normalize(b->orientation);
                    body3d_touch_broadphase_moved(b);
                    continue;
                }
            }

            ph3d_vec3_accumulate_state(b->position,
                                       b->velocity[0] * sub_dt,
                                       b->velocity[1] * sub_dt,
                                       b->velocity[2] * sub_dt);
            quat_integrate(b->orientation, b->angular_velocity, sub_dt);
            ph3d_vec3_sanitize_state(b->position);
            quat_normalize(b->orientation);
            body3d_touch_broadphase_moved(b);
        }
        if (world3d_test_step_failure_requested()) {
            world3d_step_fail(w, "Physics3D.World.Step: injected transaction failure");
            goto cleanup;
        }

        /* Phase 2: detect contacts once, then warm-started sequential-impulse solve.
         * Detecting once (rather than re-detecting per iteration) lets per-point
         * impulses accumulate and warm-start across frames, which is what makes
         * stacks rest stably. Joints are interleaved with the velocity iterations. */
        int32_t solver_iterations = ph3d_clamp_solver_iterations(w->solver_iterations);
        int32_t position_iterations = ph3d_clamp_solver_iterations(w->position_iterations);
        ph3d_solver_island_batch solver_batch;
        if (!world3d_detect_contacts(w))
            goto cleanup;
        if (!world3d_build_solver_island_batch(w, &solver_batch))
            goto cleanup;
        if (solver_batch.island_count > w->last_solver_island_count)
            w->last_solver_island_count = solver_batch.island_count;
        if (solver_batch.active_body_count > w->last_solver_active_body_count)
            w->last_solver_active_body_count = solver_batch.active_body_count;
        if (solver_batch.solver_contact_count > w->last_solver_contact_count)
            w->last_solver_contact_count = solver_batch.solver_contact_count;
        world3d_warm_start_solver_islands(w, &solver_batch);
        for (int32_t iter = 0; iter < solver_iterations; iter++) {
            world3d_solve_velocity_solver_islands(&solver_batch, w);
            for (int32_t j = 0; j < w->joint_count; j++) {
                if (w->joints[j])
                    rt_joint3d_solve(w->joints[j], w->joint_types[j], sub_dt);
            }
        }
        for (int32_t iter = 0; iter < position_iterations; iter++)
            world3d_solve_position_solver_islands(&solver_batch, w);
        ph3d_solver_island_batch_free(&solver_batch);
        world3d_finalize_contacts(w);
        world3d_update_sleep(w, sub_dt);
        for (int32_t i = 0; i < w->contact_count; ++i) {
            if (!world3d_append_frame_contact_unique(w, &w->contacts[i]))
                goto cleanup;
        }
    }

    if (!world3d_publish_frame_contacts(w))
        goto cleanup;
    if (!world3d_build_event_buffers(w))
        goto cleanup;
    world3d_finish_ccd_diagnostics(w);
    world3d_commit_broadphase_fallbacks(w);
    world3d_release_cached_event_objects(w);
    world3d_clear_body_accumulators(w);
    world3d_invalidate_query_broadphase(w);
    step_ok = 1;

cleanup:
    if (!step_ok && transaction_started) {
        failure_message = w->step_failure_message;
        if (!failure_message)
            failure_message = "Physics3D.World.Step: transactional simulation failed";
        world3d_step_transaction_restore(w, &transaction);
    }
    w->step_transaction_active = 0;
    w->step_failure_message = NULL;
    if (!step_ok && error_out)
        *error_out = failure_message;
    return step_ok;
}

/// @brief `World3D.Step(dt)` — advance the simulation atomically by one frame.
/// @details The internal step snapshots body/contact/event state and converts
///   fallible solver paths to status returns. On failure it restores the exact
///   pre-step simulation, then this public ABI boundary raises the historical
///   trap. Successful steps clear one-shot forces and publish new events.
/// @param obj `World3D` handle.
/// @param dt Step duration in seconds; non-finite/non-positive values are no-ops.
void rt_world3d_step(void *obj, double dt) {
    rt_world3d *w = world3d_checked(obj);
    const char *failure_message = NULL;
    if (!w)
        return;
    if (!world3d_step_internal(w, dt, &failure_message)) {
        rt_trap(failure_message ? failure_message : "Physics3D.World.Step: simulation failed");
        return;
    }
}

/// @brief `World3D.StepFixed(dt, fixedDt, maxSteps)` — run accumulated fixed steps.
/// @details Incoming time is accumulated before stepping, but one fixed quantum
///   is subtracted only after `world3d_step_internal` commits successfully. A
///   failed step therefore remains queued for retry. Passing `dt == 0` drains
///   previously retained time without adding another frame.
/// @param obj Borrowed `World3D` runtime object.
/// @param dt Frame duration in seconds to add to the accumulator; zero adds no time.
/// @param fixed_dt Requested fixed quantum in seconds, sanitized to runtime limits.
/// @param max_steps Maximum quanta committed in this call, clamped to supported bounds.
/// @return Number of fixed steps committed during this call.
int64_t rt_world3d_step_fixed(void *obj, double dt, double fixed_dt, int64_t max_steps) {
    rt_world3d *w = world3d_checked(obj);
    double frame_dt = ph3d_sanitize_step_dt(dt);
    double fixed = ph3d_sanitize_fixed_dt(fixed_dt);
    int32_t max_step_count = ph3d_clamp_fixed_step_limit(max_steps);
    int64_t steps = 0;
    if (!w || fixed <= 0.0 || max_step_count <= 0) {
        ph3d_update_fixed_step_alpha(w, fixed);
        return 0;
    }

    if (frame_dt > 0.0)
        w->fixed_step_accumulator += frame_dt;
    if (!isfinite(w->fixed_step_accumulator) || w->fixed_step_accumulator < 0.0)
        w->fixed_step_accumulator = 0.0;
    {
        double max_accumulator = fixed * (double)max_step_count;
        if (isfinite(max_accumulator) && max_accumulator > 0.0 &&
            w->fixed_step_accumulator > max_accumulator) {
            double dropped_time = w->fixed_step_accumulator - max_accumulator;
            if (dropped_time >= fixed)
                ph3d_add_dropped_fixed_steps(w, (int64_t)floor(dropped_time / fixed));
            w->fixed_step_accumulator = max_accumulator;
        }
    }

    while (w->fixed_step_accumulator + 1e-12 >= fixed && steps < max_step_count) {
        const char *failure_message = NULL;
        if (!world3d_step_internal(w, fixed, &failure_message)) {
            ph3d_update_fixed_step_alpha(w, fixed);
            rt_trap(failure_message ? failure_message
                                    : "Physics3D.World.StepFixed: simulation failed");
            return steps;
        }
        w->fixed_step_accumulator -= fixed;
        if (w->fixed_step_accumulator < 0.0 && w->fixed_step_accumulator > -1e-9)
            w->fixed_step_accumulator = 0.0;
        steps++;
    }
    if (steps >= max_step_count && w->fixed_step_accumulator >= fixed) {
        ph3d_add_dropped_fixed_steps(w, (int64_t)floor(w->fixed_step_accumulator / fixed));
        w->fixed_step_accumulator = 0.0;
    }
    ph3d_update_fixed_step_alpha(w, fixed);
    return steps;
}

/// @brief `World3D.FixedStepAlpha` — fixed-step accumulator interpolation fraction.
/// @param obj Borrowed `World3D` runtime object.
/// @return Sanitized interpolation fraction in `[0, 1)`, or zero for an invalid world/value.
double rt_world3d_get_fixed_step_alpha(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || !isfinite(w->fixed_step_alpha))
        return 0.0;
    if (w->fixed_step_alpha < 0.0)
        return 0.0;
    if (w->fixed_step_alpha >= 1.0)
        return 0.999999;
    return w->fixed_step_alpha;
}

/// @brief `World3D.DroppedFixedSteps` — fixed-step updates discarded by the spiral guard.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative discarded-step count, or zero for an invalid world.
int64_t rt_world3d_get_dropped_fixed_steps(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w && w->dropped_fixed_steps > 0 ? w->dropped_fixed_steps : 0;
}

/// @brief Return whether @p body is registered in @p w, using owner-index metadata first.
/// @details Body owner fields make the common membership query O(1). If an index
///   is stale, the helper falls back to a scan and repairs the cached index when
///   it finds the body. This keeps old objects coherent across swap-removes and
///   defensive count clamping.
/// @param[in,out] w Borrowed world whose safe body count and cached owner index may be repaired.
/// @param[in,out] body Borrowed body whose ownership metadata may be repaired or cleared.
/// @return `1` when @p body is present in @p w; otherwise `0`.
static int world3d_body_is_registered(rt_world3d *w, rt_body3d *body) {
    if (!w || !body)
        return 0;
    int32_t body_count = world3d_body_count_safe(w);
    w->body_count = body_count;
    if (body->owner_world == w && body->owner_index >= 0 && body->owner_index < body_count &&
        w->bodies[body->owner_index] == body)
        return 1;
    for (int32_t i = 0; i < body_count; ++i) {
        if (w->bodies[i] == body) {
            body->owner_world = w;
            body->owner_index = i;
            return 1;
        }
    }
    if (body->owner_world == w) {
        body->owner_world = NULL;
        body->owner_index = -1;
    }
    return 0;
}

/// @brief Return whether a joint endpoint can be attached to @p w.
/// @details Registered bodies are accepted directly. Bodies that are not owned by
///   any world are also accepted for legacy/manual joint registration, while bodies
///   already owned by a different world are rejected so constraints cannot bridge
///   two independent simulations.
/// @param w Borrowed destination world.
/// @param[in,out] body Borrowed joint endpoint whose cached registration may be repaired.
/// @return `1` when the body is registered in @p w or currently unowned; otherwise `0`.
static int world3d_joint_body_allowed(rt_world3d *w, rt_body3d *body) {
    if (!w || !body)
        return 0;
    if (world3d_body_is_registered(w, body))
        return 1;
    return body->owner_world == NULL;
}

/// @brief `World3D.AddJoint(joint, type)` — register a constraint.
///
/// Retains the joint and stores its type tag so `rt_joint3d_solve` can
/// dispatch correctly. Storage grows on demand.
/// @param obj Borrowed `World3D` runtime object.
/// @param joint Borrowed joint object retained by the world on successful first registration.
/// @param joint_type Runtime joint-type tag used for validation and solver dispatch.
void rt_world3d_add_joint(void *obj, void *joint, int64_t joint_type) {
    rt_world3d *w = world3d_checked(obj);
    void *joint_body_a = NULL;
    void *joint_body_b = NULL;
    if (!w || !joint)
        return;
    if (!joint3d_matches_type(joint, joint_type)) {
        rt_trap("Physics3D.World.AddJoint: joint object does not match joint type");
        return;
    }
    if (!rt_joint3d_get_bodies(joint, (int32_t)joint_type, &joint_body_a, &joint_body_b) ||
        !world3d_joint_body_allowed(w, (rt_body3d *)joint_body_a) ||
        !world3d_joint_body_allowed(w, (rt_body3d *)joint_body_b)) {
        rt_trap("Physics3D.World.AddJoint: joint bodies cannot belong to another world");
        return;
    }
    int32_t joint_count = world3d_joint_count_safe(w);
    w->joint_count = joint_count;
    for (int32_t i = 0; i < joint_count; i++) {
        if (w->joints[i] == joint)
            return;
    }
    if (!world3d_reserve_joint_capacity(w, w->joint_count + 1)) {
        rt_trap("Physics3D: joint storage allocation failed");
        return;
    }
    rt_obj_retain_maybe(joint);
    w->joints[w->joint_count] = joint;
    w->joint_types[w->joint_count] = (int32_t)joint_type;
    w->joint_count++;
}

/// @brief `World3D.RemoveJoint(joint)` — unregister a constraint.
///
/// Linear scan; on hit, swaps in the last entry to compact the array
/// and releases the world's reference. Silent no-op when not found.
/// @param obj Borrowed `World3D` runtime object.
/// @param joint Borrowed joint object to unregister.
void rt_world3d_remove_joint(void *obj, void *joint) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || !joint)
        return;
    int32_t joint_count = world3d_joint_count_safe(w);
    w->joint_count = joint_count;
    for (int32_t i = 0; i < joint_count; i++) {
        if (w->joints[i] == joint) {
            void *removed = w->joints[i];
            w->joints[i] = w->joints[--w->joint_count];
            w->joint_types[i] = w->joint_types[w->joint_count];
            w->joints[w->joint_count] = NULL;
            w->joint_types[w->joint_count] = 0;
            if (rt_obj_release_check0(removed))
                rt_obj_free(removed);
            return;
        }
    }
}

/// @brief Return whether a concrete joint references @p body as either endpoint.
/// @param joint Borrowed joint object.
/// @param joint_type Concrete runtime joint-type tag.
/// @param body Borrowed body to compare with both resolved endpoints.
/// @return `1` when endpoint resolution succeeds and either endpoint equals @p body; otherwise `0`.
static int world3d_joint_mentions_body(void *joint, int32_t joint_type, rt_body3d *body) {
    void *body_a = NULL;
    void *body_b = NULL;
    return body && rt_joint3d_get_bodies(joint, joint_type, &body_a, &body_b) &&
           (body_a == body || body_b == body);
}

/// @brief Remove and release every joint in @p w that references @p body.
/// @details Called before body removal so a removed body is not retained by stale
///   constraints and the solver never sees a joint endpoint outside the world.
/// @param[in,out] w Borrowed world whose joint arrays are compacted.
/// @param body Borrowed body whose referencing joints are removed.
static void world3d_purge_joints_for_body(rt_world3d *w, rt_body3d *body) {
    if (!w || !body)
        return;
    int32_t joint_count = world3d_joint_count_safe(w);
    w->joint_count = joint_count;
    int32_t write = 0;
    for (int32_t read = 0; read < joint_count; ++read) {
        void *joint = w->joints[read];
        int32_t joint_type = w->joint_types[read];
        if (world3d_joint_mentions_body(joint, joint_type, body)) {
            if (joint && rt_obj_release_check0(joint))
                rt_obj_free(joint);
            continue;
        }
        if (write != read) {
            w->joints[write] = w->joints[read];
            w->joint_types[write] = w->joint_types[read];
        }
        write++;
    }
    for (int32_t i = write; i < joint_count; ++i) {
        w->joints[i] = NULL;
        w->joint_types[i] = 0;
    }
    w->joint_count = write;
}

/// @brief `World3D.JointCount` — number of registered joints (0 for NULL).
/// @param obj Borrowed `World3D` runtime object.
/// @return Sanitized number of registered joints, or zero for an invalid world.
int64_t rt_world3d_joint_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return world3d_joint_count_safe(w);
}

/// @brief `World3D.SolverIterations` — iterative constraint-solver pass count.
/// @param obj Borrowed `World3D` runtime object.
/// @return Clamped velocity-solver iteration count, or zero for an invalid world.
int64_t rt_world3d_get_solver_iterations(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? ph3d_clamp_solver_iterations(w->solver_iterations) : 0;
}

/// @brief `World3D.SolverIterations` setter — tune iterative solver passes.
/// @param obj Borrowed `World3D` runtime object.
/// @param iterations Requested pass count, clamped to the supported solver range.
void rt_world3d_set_solver_iterations(void *obj, int64_t iterations) {
    rt_world3d *w = world3d_checked(obj);
    if (w)
        w->solver_iterations = ph3d_clamp_solver_iterations(iterations);
}

/// @brief `World3D.PositionIterations` — positional correction pass count.
/// @param obj Borrowed `World3D` runtime object.
/// @return Clamped positional-solver iteration count, or zero for an invalid world.
int64_t rt_world3d_get_position_iterations(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? ph3d_clamp_solver_iterations(w->position_iterations) : 0;
}

/// @brief `World3D.PositionIterations` setter — tune positional correction passes.
/// @param obj Borrowed `World3D` runtime object.
/// @param iterations Requested pass count, clamped to the supported solver range.
void rt_world3d_set_position_iterations(void *obj, int64_t iterations) {
    rt_world3d *w = world3d_checked(obj);
    if (w)
        w->position_iterations = ph3d_clamp_solver_iterations(iterations);
}

/// @brief `World3D.ContactBeta` — Baumgarte positional recovery fraction.
/// @param obj Borrowed `World3D` runtime object.
/// @return Finite recovery fraction in `[0, 1]`, or zero for an invalid world.
double rt_world3d_get_contact_beta(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? ph3d_clamp_unit_finite(w->contact_beta, PH3D_DEFAULT_CONTACT_BETA) : 0.0;
}

/// @brief `World3D.ContactBeta` setter — clamp positional recovery to [0, 1].
/// @param obj Borrowed `World3D` runtime object.
/// @param beta Requested finite correction fraction; invalid values fall back to zero.
void rt_world3d_set_contact_beta(void *obj, double beta) {
    rt_world3d *w = world3d_checked(obj);
    if (w)
        w->contact_beta = ph3d_clamp_unit_finite(beta, 0.0);
}

/// @brief `World3D.RestitutionThreshold` — minimum approach speed that bounces.
/// @param obj Borrowed `World3D` runtime object.
/// @return Finite non-negative threshold in metres per second, or zero for an invalid world.
double rt_world3d_get_restitution_threshold(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? ph3d_clamp_nonnegative_finite(w->restitution_threshold,
                                             PH3D_DEFAULT_RESTITUTION_THRESHOLD)
             : 0.0;
}

/// @brief `World3D.RestitutionThreshold` setter — clamp to a finite non-negative speed.
/// @param obj Borrowed `World3D` runtime object.
/// @param threshold Requested bounce threshold in metres per second; invalid values become zero.
void rt_world3d_set_restitution_threshold(void *obj, double threshold) {
    rt_world3d *w = world3d_checked(obj);
    if (w)
        w->restitution_threshold = ph3d_clamp_nonnegative_finite(threshold, 0.0);
}

/// @brief `World3D.LastSolverIslandCount` — max active contact islands in the last Step.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative maximum island count observed during the last committed step.
int64_t rt_world3d_get_last_solver_island_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->last_solver_island_count > 0) ? w->last_solver_island_count : 0;
}

/// @brief `World3D.LastSolverActiveBodyCount` — max awake bodies scheduled in contact islands.
/// @param obj Borrowed `World3D` runtime object.
/// @return Last maximum active-island body count, defensively capped by current membership.
int64_t rt_world3d_get_last_solver_active_body_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || w->last_solver_active_body_count <= 0)
        return 0;
    int32_t body_count = world3d_body_count_safe(w);
    return w->last_solver_active_body_count < body_count ? w->last_solver_active_body_count
                                                         : body_count;
}

/// @brief `World3D.LastSolverContactCount` — max contacts scheduled through islands.
/// @param obj Borrowed `World3D` runtime object.
/// @return Last maximum scheduled-contact count, defensively capped by current contacts.
int64_t rt_world3d_get_last_solver_contact_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || w->last_solver_contact_count <= 0)
        return 0;
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    return w->last_solver_contact_count < contact_count ? w->last_solver_contact_count
                                                        : contact_count;
}

/// @brief `World3D.Add(body)` — register a body.
///
/// Retains the body and stores it in the growable bodies array.
/// @param obj Borrowed destination `World3D` runtime object.
/// @param body Borrowed `Body3D` retained on successful first registration.
/// @return `1` when the body is already present or was added; `0` for invalid objects, conflicting
///         world ownership, or storage allocation failure.
int8_t rt_world3d_try_add(void *obj, void *body) {
    rt_world3d *w = world3d_checked(obj);
    rt_body3d *b = body3d_checked(body);
    if (!w || !b)
        return 0;
    if (world3d_body_is_registered(w, b))
        return 1;
    if (b->owner_world && b->owner_world != w) {
        rt_trap("Physics3D.World.Add: body already belongs to another world");
        return 0;
    }
    if (!world3d_reserve_body_capacity(w, w->body_count + 1)) {
        rt_trap("Physics3D: body storage allocation failed");
        return 0;
    }
    rt_obj_retain_maybe(body);
    b->owner_world = w;
    b->owner_index = w->body_count;
    w->bodies[w->body_count++] = b;
    world3d_invalidate_query_broadphase(w);
    return 1;
}

/// @brief Add a body to the world, ignoring the success flag (see rt_world3d_try_add).
/// @param obj Borrowed destination `World3D` runtime object.
/// @param body Borrowed `Body3D` to register.
void rt_world3d_add(void *obj, void *body) {
    (void)rt_world3d_try_add(obj, body);
}

/// @brief Whether a contact references @p body as either participant.
/// @param contact Borrowed contact to inspect.
/// @param body Borrowed body to compare with both participants.
/// @return `1` when both pointers are valid and either participant equals @p body; otherwise `0`.
static int contact_mentions_body(const rt_contact3d *contact, const rt_body3d *body) {
    return contact && body && (contact->body_a == body || contact->body_b == body);
}

/// @brief Remove every contact mentioning @p body from a contact array, compacting it in place.
/// @param[in,out] contacts Contact or event array compacted in place.
/// @param[in,out] count Number of initialized entries, replaced with the retained count.
/// @param body Borrowed body whose contacts are removed.
static void world3d_purge_contact_array_for_body(rt_contact3d *contacts,
                                                 int32_t *count,
                                                 const rt_body3d *body) {
    int32_t write = 0;
    if (!contacts || !count || !body)
        return;
    for (int32_t read = 0; read < *count; ++read) {
        if (contact_mentions_body(&contacts[read], body))
            continue;
        if (write != read)
            contacts[write] = contacts[read];
        write++;
    }
    for (int32_t i = write; i < *count; ++i)
        memset(&contacts[i], 0, sizeof(contacts[i]));
    *count = write;
}

/// @brief Purge all of the world's contact/event arrays of contacts involving @p body.
/// @details Called when a body leaves the world so stale contacts don't fire spurious events.
/// @param[in,out] w Borrowed world whose contact, frame-contact, and event arrays are compacted.
/// @param body Borrowed body being removed.
static void world3d_purge_body_contacts(rt_world3d *w, const rt_body3d *body) {
    if (!w || !body)
        return;
    world3d_release_cached_event_objects(w);
    world3d_purge_contact_array_for_body(w->contacts, &w->contact_count, body);
    world3d_purge_contact_array_for_body(w->frame_contacts, &w->frame_contact_count, body);
    world3d_purge_contact_array_for_body(w->previous_contacts, &w->previous_contact_count, body);
    world3d_purge_contact_array_for_body(w->enter_events, &w->enter_event_count, body);
    world3d_purge_contact_array_for_body(w->stay_events, &w->stay_event_count, body);
    world3d_purge_contact_array_for_body(w->exit_events, &w->exit_event_count, body);
}

/// @brief `World3D.Remove(body)` — unregister a body.
///
/// Same swap-with-last compaction as `RemoveJoint`. Releases the world's
/// reference, which may free the body if no other holder remains.
/// @param obj Borrowed `World3D` runtime object.
/// @param body Borrowed `Body3D` to unregister; a missing body is a no-op.
void rt_world3d_remove(void *obj, void *body) {
    rt_world3d *w = world3d_checked(obj);
    rt_body3d *b = body3d_checked(body);
    if (!w || !b)
        return;
    int32_t body_count = world3d_body_count_safe(w);
    w->body_count = body_count;
    for (int32_t i = 0; i < body_count; i++) {
        if (w->bodies[i] == b) {
            rt_body3d *removed = w->bodies[i];
            world3d_purge_body_contacts(w, b);
            world3d_purge_joints_for_body(w, b);
            if (removed && removed->owner_world == w) {
                removed->owner_world = NULL;
                removed->owner_index = -1;
            }
            w->body_count--;
            w->bodies[i] = w->bodies[w->body_count];
            if (w->bodies[i] && w->bodies[i]->owner_world == w)
                w->bodies[i]->owner_index = i;
            w->bodies[w->body_count] = NULL;
            world3d_invalidate_query_broadphase(w);
            if (removed && rt_obj_release_check0(removed))
                rt_obj_free(removed);
            return;
        }
    }
}

/// @brief `World3D.BodyCount` — number of registered bodies (0 for NULL).
/// @param obj Borrowed `World3D` runtime object.
/// @return Sanitized number of registered bodies, or zero for an invalid world.
int64_t rt_world3d_body_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return world3d_body_count_safe(w);
}

/// @brief Whether @p body is currently registered in the world.
/// @param obj Borrowed `World3D` runtime object.
/// @param body Borrowed `Body3D` whose membership and cached owner index are checked.
/// @return `1` when @p body is registered in @p obj; otherwise `0`.
int8_t rt_world3d_contains_body(void *obj, void *body) {
    rt_world3d *w = world3d_checked(obj);
    rt_body3d *b = body3d_checked(body);
    if (!w || !b)
        return 0;
    return world3d_body_is_registered(w, b) ? 1 : 0;
}

/// @brief `World3D.LastCcdRequestedSubsteps` — largest unclamped local body-segment demand.
/// @param obj Borrowed `World3D` runtime object.
/// @return Positive largest requested segment count from the last step, or zero when unavailable.
int64_t rt_world3d_get_last_ccd_requested_substeps(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || w->last_ccd_requested_substeps <= 0)
        return 0;
    return w->last_ccd_requested_substeps;
}

/// @brief `World3D.LastCcdSubsteps` — largest bounded local body-segment count used.
/// @param obj Borrowed `World3D` runtime object.
/// @return Largest applied segment count from the last step, capped at the runtime maximum.
int64_t rt_world3d_get_last_ccd_substeps(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w || w->last_ccd_substeps <= 0)
        return 0;
    return w->last_ccd_substeps > PH3D_MAX_CCD_SUBSTEPS ? PH3D_MAX_CCD_SUBSTEPS
                                                        : w->last_ccd_substeps;
}

/// @brief `World3D.CcdSubstepClampedCount` — frames where a body hit the segment cap.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative committed-step count containing at least one CCD clamp.
int64_t rt_world3d_get_ccd_substep_clamped_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->ccd_substep_clamped_count > 0) ? w->ccd_substep_clamped_count : 0;
}

/// @brief `World3D.LastCcdClampedBodyCount` — bodies capped by CCD in the last Step.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative number of bodies clamped during the last committed step.
int64_t rt_world3d_get_last_ccd_clamped_body_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->last_ccd_clamped_body_count > 0) ? w->last_ccd_clamped_body_count : 0;
}

/// @brief `World3D.CcdSubstepClampedBodyCount` — total bodies affected by CCD caps.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative body-clamp count across committed steps.
int64_t rt_world3d_get_ccd_substep_clamped_body_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->ccd_substep_clamped_body_count > 0) ? w->ccd_substep_clamped_body_count : 0;
}

/// @brief `Physics3DWorld.SetMaxQueryHits(n)` — configure query result capacity.
/// @details Bounds how many hits `RaycastAll`/`OverlapSphere`/`OverlapAABB`
///          return (lists still report `TotalCount`/`Truncated`). Clamped to
///          16–4096; reallocates the world's scratch buffer on next use.
/// @param obj Borrowed `World3D` runtime object.
/// @param max_hits Requested retained-result capacity, clamped to
///        `[PH3D_QUERY_HITS_MIN, PH3D_QUERY_HITS_MAX]`.
void rt_world3d_set_max_query_hits(void *obj, int64_t max_hits) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return;
    if (max_hits < PH3D_QUERY_HITS_MIN)
        max_hits = PH3D_QUERY_HITS_MIN;
    if (max_hits > PH3D_QUERY_HITS_MAX)
        max_hits = PH3D_QUERY_HITS_MAX;
    if ((int32_t)max_hits == w->max_query_hits)
        return;
    free(w->query_hits_scratch);
    w->query_hits_scratch = NULL;
    w->max_query_hits = (int32_t)max_hits;
}

/// @brief `Physics3DWorld.MaxQueryHits` — current query result capacity.
/// @param obj Borrowed `World3D` runtime object.
/// @return Current retained-result capacity, or zero for an invalid world.
int64_t rt_world3d_get_max_query_hits(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? w->max_query_hits : 0;
}

/// @brief `World3D.CcdToiCount` — total swept time-of-impact clips applied.
/// @details Each count is one CCD body whose substep translation was clipped
///          at a static/kinematic surface by the swept pass (i.e. a tunnel
///          that substep subdivision alone would not have prevented exactly).
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative number of resolved time-of-impact clips.
int64_t rt_world3d_get_ccd_toi_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->ccd_toi_count > 0) ? w->ccd_toi_count : 0;
}

/// @brief `World3D.BroadphaseFallbackCount` — brute-force broadphase fallback count.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative committed broad-phase fallback count.
int64_t rt_world3d_get_broadphase_fallback_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->broadphase_fallback_count > 0) ? w->broadphase_fallback_count : 0;
}

/// @brief `World3D.QueryBroadphaseRebuildCount` — total query-broadphase rebuilds.
/// @details Diagnostic for spatial-query cache health: with fat-AABB reuse, sub-margin
///          body movement keeps this flat; membership/shape changes and escapes bump it.
/// @param obj Borrowed `World3D` runtime object.
/// @return Non-negative cumulative number of query-cache rebuilds.
int64_t rt_world3d_get_query_broadphase_rebuild_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return (w && w->query_broadphase_rebuild_count > 0) ? w->query_broadphase_rebuild_count : 0;
}

/// @brief `World3D.SetGravity(gx, gy, gz)` — change the world gravity vector.
///
/// Takes effect on the next `Step`. Existing in-flight velocities are
/// not modified.
/// @param obj Borrowed `World3D` runtime object.
/// @param gx New gravity x acceleration in metres per second squared.
/// @param gy New gravity y acceleration in metres per second squared.
/// @param gz New gravity z acceleration in metres per second squared.
void rt_world3d_set_gravity(void *obj, double gx, double gy, double gz) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return;
    ph3d_vec3_set_finite(w->gravity, gx, gy, gz);
    ph3d_vec3_sanitize_state(w->gravity);
}

/// @brief Shift a contact's cached points by the floating-origin rebase @p delta.
/// @details When the world recenters to keep coordinates near the origin, contact points must move
///          with it; saturates the shifted values to stay finite.
/// @param[in,out] contact Contact whose primary point and bounded manifold points are shifted.
/// @param delta Borrowed three-element world-origin displacement to subtract.
static void contact3d_rebase_origin(rt_contact3d *contact, const double delta[3]) {
    if (!contact || !delta)
        return;
    contact->point[0] = ph3d_saturate_state_value(contact->point[0] - delta[0]);
    contact->point[1] = ph3d_saturate_state_value(contact->point[1] - delta[1]);
    contact->point[2] = ph3d_saturate_state_value(contact->point[2] - delta[2]);
    int32_t point_count = contact->contact_count;
    if (point_count < 0)
        point_count = 0;
    if (point_count > PH3D_MAX_MANIFOLD_POINTS)
        point_count = PH3D_MAX_MANIFOLD_POINTS;
    for (int32_t i = 0; i < point_count; ++i) {
        contact->points[i][0] = ph3d_saturate_state_value(contact->points[i][0] - delta[0]);
        contact->points[i][1] = ph3d_saturate_state_value(contact->points[i][1] - delta[1]);
        contact->points[i][2] = ph3d_saturate_state_value(contact->points[i][2] - delta[2]);
    }
}

/// @brief Apply a floating-origin rebase @p delta to every contact in an array.
/// @param[in,out] contacts Contact or event array to shift.
/// @param count Number of initialized contacts; non-positive values are no-ops.
/// @param delta Borrowed three-element displacement subtracted from every cached point.
static void world3d_rebase_contact_array(rt_contact3d *contacts,
                                         int32_t count,
                                         const double delta[3]) {
    if (!contacts || count <= 0 || !delta)
        return;
    for (int32_t i = 0; i < count; ++i)
        contact3d_rebase_origin(&contacts[i], delta);
}

/// @brief `World3D.RebaseOrigin(dx, dy, dz)` — shift body/contact/query state by -delta.
/// @details Saturates the requested displacement, moves all registered bodies and cached
///          contact/event points, invalidates spatial-query acceleration, and drops cached boxed
///          event views. Velocities and local collider geometry are unchanged.
/// @param obj Borrowed `World3D` runtime object.
/// @param dx World-origin x displacement to subtract.
/// @param dy World-origin y displacement to subtract.
/// @param dz World-origin z displacement to subtract.
void rt_world3d_rebase_origin(void *obj, double dx, double dy, double dz) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return;
    double delta[3] = {
        ph3d_saturate_state_value(dx),
        ph3d_saturate_state_value(dy),
        ph3d_saturate_state_value(dz),
    };
    if (delta[0] == 0.0 && delta[1] == 0.0 && delta[2] == 0.0)
        return;

    for (int32_t i = 0; i < w->body_count; ++i) {
        rt_body3d *body = w->bodies[i];
        if (!body)
            continue;
        body->position[0] = ph3d_saturate_state_value(body->position[0] - delta[0]);
        body->position[1] = ph3d_saturate_state_value(body->position[1] - delta[1]);
        body->position[2] = ph3d_saturate_state_value(body->position[2] - delta[2]);
        body3d_touch_broadphase(body);
    }

    world3d_rebase_contact_array(w->contacts, w->contact_count, delta);
    world3d_rebase_contact_array(w->frame_contacts, w->frame_contact_count, delta);
    world3d_rebase_contact_array(w->previous_contacts, w->previous_contact_count, delta);
    world3d_rebase_contact_array(w->enter_events, w->enter_event_count, delta);
    world3d_rebase_contact_array(w->stay_events, w->stay_event_count, delta);
    world3d_rebase_contact_array(w->exit_events, w->exit_event_count, delta);
    world3d_invalidate_query_broadphase(w);
    world3d_release_cached_event_objects(w);
}
