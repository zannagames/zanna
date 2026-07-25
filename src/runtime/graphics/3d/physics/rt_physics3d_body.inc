//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_body.inc  (#included from rt_physics3d.c; not
// a standalone translation unit) Purpose: Physics3D Body3D construction and property accessors
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_body.inc
 * @brief Implements Body3D construction, ownership, state, material, filtering, and motion APIs.
 *
 * This implementation fragment is included by `rt_physics3d.c`. It centralizes
 * validation and finite-value sanitization while keeping collider ownership,
 * broadphase invalidation, motion-mode derivation, sleeping, and contact
 * history coherent after every public mutation.
 */

/*==========================================================================
 * Body3D
 *=========================================================================*/

/// @brief GC finalizer for `Body3D` — release the owned collider.
///
/// The body holds a strong reference to its collider; this drops it.
/// World-level body removal (`rt_world3d_remove`) drops the world's
/// reference, which in most cases is the last one and triggers this.
/// @param obj Body3D object whose retained collider is released.
static void body3d_finalizer(void *obj) {
    rt_body3d *b = (rt_body3d *)obj;
    if (!b)
        return;
    if (b->collider && rt_obj_release_check0(b->collider))
        rt_obj_free(b->collider);
    b->collider = NULL;
}

/// @brief Swap the body's collider, refreshing all derived state.
///
/// Validates that the new collider is allowed for the body's current
/// motion mode (`body3d_motion_mode_allowed`). On success, retains the
/// new collider, releases the old, refreshes the cached shape and
/// inverse-mass derivations, and wakes the body if dynamic. No-op
/// when the same collider is assigned twice.
/// @param body Body3D instance whose collider is replaced.
/// @param collider Collider3D handle to retain, or NULL to clear the collider.
/// @param api_name Trap context used when the collider is incompatible with the motion mode.
/// @return One when assignment succeeds or is unnecessary, otherwise zero.
static int body3d_assign_collider(rt_body3d *body, void *collider, const char *api_name) {
    if (!body)
        return 0;
    if (collider && !rt_g3d_has_class(collider, RT_G3D_COLLIDER3D_CLASS_ID)) {
        rt_trap("Physics3DBody.Collider: collider must be a Collider3D");
        return 0;
    }
    if (collider == body->collider)
        return 1;
    if (!body3d_motion_mode_allowed(body, collider, body->motion_mode, api_name))
        return 0;
    if (collider)
        rt_obj_retain_maybe(collider);
    if (body->collider && rt_obj_release_check0(body->collider))
        rt_obj_free(body->collider);
    body->collider = collider;
    body3d_update_shape_cache_from_collider(body);
    body3d_refresh_motion_mode(body);
    body3d_touch_broadphase(body);
    body3d_wake_if_dynamic(body);
    return 1;
}

/// @brief Allocate a fresh body with sensible defaults.
///
/// Defaults: restitution 0.3 (mostly inelastic), friction 0.5 (typical
/// rubber/concrete blend), layer 1, mask all (collides with everything),
/// motion mode = `dynamic` if mass > 0 else `static`, sleeping enabled.
/// The collider is left NULL for the caller to assign via
/// `body3d_assign_collider`.
/// @param mass Requested non-negative body mass in kilograms.
/// @return Newly allocated Body3D with no collider, or NULL on allocation failure.
static void *make_body(double mass) {
    rt_body3d *b = (rt_body3d *)rt_obj_new_i64(RT_G3D_BODY3D_CLASS_ID, (int64_t)sizeof(rt_body3d));
    if (!b) {
        rt_trap("Physics3D.Body.New: allocation failed");
        return NULL;
    }
    memset(b, 0, sizeof(rt_body3d));
    b->mass = ph3d_clamp_nonnegative_finite(mass, 0.0);
    b->restitution = 0.3;
    b->friction = 0.5;
    b->collision_layer = 1;
    b->collision_mask = ~(int64_t)0;
    b->motion_mode = (b->mass <= 1e-12) ? PH3D_MODE_STATIC : PH3D_MODE_DYNAMIC;
    b->can_sleep = 1;
    b->owner_index = -1;
    b->ground_normal[1] = 1.0;
    vec3_set(b->scale, 1.0, 1.0, 1.0);
    quat_identity(b->orientation);
    body3d_refresh_motion_mode(b);
    rt_obj_set_finalizer(b, body3d_finalizer);
    return b;
}

/// @brief `Body3D.New(mass)` — bare body without a collider.
///
/// Caller must assign a collider via the `Collider` property before adding to a
/// world (otherwise body-vs-body tests skip it).
/// @param mass Requested non-negative body mass in kilograms.
/// @return Newly allocated Body3D handle, or NULL on allocation failure.
void *rt_body3d_new(double mass) {
    return make_body(mass);
}

/// @brief `Body3D.NewAABB(hx, hy, hz, mass)` — body with a fresh box collider.
///
/// Half-extents specify the box. The transient collider returned by
/// `rt_collider3d_new_box` is released after the body retains it.
/// @param hx X-axis box half-extent.
/// @param hy Y-axis box half-extent.
/// @param hz Z-axis box half-extent.
/// @param mass Requested non-negative body mass in kilograms.
/// @return Newly allocated box Body3D, or NULL when construction fails.
void *rt_body3d_new_aabb(double hx, double hy, double hz, double mass) {
    rt_body3d *b = (rt_body3d *)make_body(mass);
    void *collider = rt_collider3d_new_box(hx, hy, hz);
    if (!b || !collider) {
        if (collider && rt_obj_release_check0(collider))
            rt_obj_free(collider);
        if (b && rt_obj_release_check0(b))
            rt_obj_free(b);
        return NULL;
    }
    body3d_assign_collider(b, collider, "Physics3DBody.NewAABB: failed to assign collider");
    if (rt_obj_release_check0(collider))
        rt_obj_free(collider);
    return b;
}

/// @brief `Body3D.NewSphere(radius, mass)` — body with a fresh sphere collider.
/// @param radius Sphere radius in world units.
/// @param mass Requested non-negative body mass in kilograms.
/// @return Newly allocated sphere Body3D, or NULL when construction fails.
void *rt_body3d_new_sphere(double radius, double mass) {
    rt_body3d *b = (rt_body3d *)make_body(mass);
    void *collider = rt_collider3d_new_sphere(radius);
    if (!b || !collider) {
        if (collider && rt_obj_release_check0(collider))
            rt_obj_free(collider);
        if (b && rt_obj_release_check0(b))
            rt_obj_free(b);
        return NULL;
    }
    body3d_assign_collider(b, collider, "Physics3DBody.NewSphere: failed to assign collider");
    if (rt_obj_release_check0(collider))
        rt_obj_free(collider);
    return b;
}

/// @brief `Body3D.NewCapsule(radius, height, mass)` — Y-aligned capsule body.
///
/// Total height includes both hemispherical caps. The cylinder portion
/// has length `max(0, height - 2*radius)`.
/// @param radius Capsule radius in world units.
/// @param height Total capsule height including both hemispherical caps.
/// @param mass Requested non-negative body mass in kilograms.
/// @return Newly allocated capsule Body3D, or NULL when construction fails.
void *rt_body3d_new_capsule(double radius, double height, double mass) {
    rt_body3d *b = (rt_body3d *)make_body(mass);
    void *collider = rt_collider3d_new_capsule(radius, height);
    if (!b || !collider) {
        if (collider && rt_obj_release_check0(collider))
            rt_obj_free(collider);
        if (b && rt_obj_release_check0(b))
            rt_obj_free(b);
        return NULL;
    }
    body3d_assign_collider(b, collider, "Physics3DBody.NewCapsule: failed to assign collider");
    if (rt_obj_release_check0(collider))
        rt_obj_free(collider);
    return b;
}

/// @brief `Body3D.Collider` setter — swap the body's collider.
///
/// Traps if the collider requires a static-only body but the body is
/// dynamic/kinematic (e.g., concave triangle meshes).
/// @param o Body3D handle to modify.
/// @param collider Collider3D handle to retain, or NULL to clear the collider.
void rt_body3d_set_collider(void *o, void *collider) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    body3d_assign_collider(b, collider, "Physics3DBody.Collider: collider requires a static body");
}

/// @brief `Body3D.GetCollider` — borrowed reference to the body's collider.
/// @param o Body3D handle to inspect.
/// @return Borrowed Collider3D handle, or NULL when absent or invalid.
void *rt_body3d_get_collider(void *o) {
    rt_body3d *b = body3d_checked(o);
    return body3d_collider_ref(b);
}

/// @brief `Body3D.SetPosition(x, y, z)` — teleport (also wakes if dynamic).
///
/// Bypasses collision response, so use sparingly during simulation
/// (per-step adjustments are usually better via velocity).
/// @param o Body3D handle to reposition and wake when dynamic.
/// @param x Finite world-space X coordinate.
/// @param y Finite world-space Y coordinate.
/// @param z Finite world-space Z coordinate.
void rt_body3d_set_position(void *o, double x, double y, double z) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        ph3d_vec3_set_finite(b->position, x, y, z);
        ph3d_vec3_sanitize_state(b->position);
        body3d_touch_broadphase_moved(b);
        body3d_wake_if_dynamic(b);
        /* A teleported static/kinematic anchor must re-activate sleeping joint
         * partners so the constraint re-solves against the new pose. */
        world3d_wake_joint_partners(b->owner_world, b);
    }
}

/// @brief `Body3D.GetPosition` — fresh `Vec3` of the body's world position.
/// @param o Body3D handle to inspect.
/// @return Newly allocated position Vec3, or the origin for an invalid handle.
void *rt_body3d_get_position(void *o) {
    rt_body3d *b = body3d_checked(o);
    double position[3];
    if (!b)
        return rt_vec3_new(0, 0, 0);
    ph3d_vec3_copy_state(position, b->position);
    return rt_vec3_new(position[0], position[1], position[2]);
}

/// @brief `Body3D.SetScale(x,y,z)` — set collision scale applied to the collider.
/// @param o Body3D handle to modify.
/// @param x X-axis collider scale.
/// @param y Y-axis collider scale.
/// @param z Z-axis collider scale.
void rt_body3d_set_scale(void *o, double x, double y, double z) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    b->scale[0] = ph3d_sanitize_scale_value(x);
    b->scale[1] = ph3d_sanitize_scale_value(y);
    b->scale[2] = ph3d_sanitize_scale_value(z);
    body3d_update_shape_cache_from_collider(b);
    body3d_refresh_motion_mode(b);
    body3d_touch_broadphase(b);
    body3d_wake_if_dynamic(b);
}

/// @brief `Body3D.GetScale` — fresh `Vec3` of the collider scale.
/// @param o Body3D handle to inspect.
/// @return Newly allocated sanitized scale Vec3, or unit scale for an invalid handle.
void *rt_body3d_get_scale(void *o) {
    rt_body3d *b = body3d_checked(o);
    double scale[3];
    if (!b)
        return rt_vec3_new(1.0, 1.0, 1.0);
    scale[0] = ph3d_sanitize_scale_value(b->scale[0]);
    scale[1] = ph3d_sanitize_scale_value(b->scale[1]);
    scale[2] = ph3d_sanitize_scale_value(b->scale[2]);
    return rt_vec3_new(scale[0], scale[1], scale[2]);
}

/// @brief `Body3D.SetOrientation(quat)` — set the body's orientation quaternion.
///
/// NULL `quat` resets to identity. Always normalizes to defend against
/// caller-passed unnormalized quaternions.
/// @param o Body3D handle to rotate and wake when dynamic.
/// @param quat Quaternion handle to normalize and copy, or NULL for identity.
void rt_body3d_set_orientation(void *o, void *quat) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    if (!quat) {
        quat_identity(b->orientation);
    } else if (!rt_g3d_is_quat(quat)) {
        rt_trap("Physics3DBody.SetOrientation: quat must be Quat");
        return;
    } else {
        b->orientation[0] = rt_quat_x(quat);
        b->orientation[1] = rt_quat_y(quat);
        b->orientation[2] = rt_quat_z(quat);
        b->orientation[3] = rt_quat_w(quat);
        quat_normalize(b->orientation);
    }
    /* Pose-only: the lazy escape check recomputes the true AABB, so a rotation
     * that swings the body outside its fat bounds still forces a rebuild. */
    body3d_touch_broadphase_moved(b);
    body3d_wake_if_dynamic(b);
    world3d_wake_joint_partners(b->owner_world, b);
}

/// @brief `Body3D.GetOrientation` — fresh `Quat` of the body's orientation.
/// @param o Body3D handle to inspect.
/// @return Newly allocated normalized quaternion, or identity for an invalid handle.
void *rt_body3d_get_orientation(void *o) {
    rt_body3d *b = body3d_checked(o);
    double orientation[4];
    if (!b)
        return rt_quat_new(0.0, 0.0, 0.0, 1.0);
    memcpy(orientation, b->orientation, sizeof(orientation));
    quat_normalize(orientation);
    return rt_quat_new(orientation[0], orientation[1], orientation[2], orientation[3]);
}

/// @brief Read a body's full pose — position, orientation quaternion, and scale — into raw arrays.
/// @details Out-params are optional (NULL-skippable). Used by the renderer/scene sync to mirror the
///          simulated body transform without boxing Vec3/Quat values.
/// @param o Body3D handle to inspect.
/// @param position_out Optional three-component output receiving world position.
/// @param rotation_out Optional four-component output receiving normalized XYZW orientation.
/// @param scale_out Optional three-component output receiving sanitized collider scale.
void rt_body3d_get_pose_raw(void *o,
                            double *position_out,
                            double *rotation_out,
                            double *scale_out) {
    rt_body3d *b = body3d_checked(o);
    if (position_out)
        vec3_set(position_out, 0.0, 0.0, 0.0);
    if (rotation_out)
        quat_identity(rotation_out);
    if (scale_out)
        vec3_set(scale_out, 1.0, 1.0, 1.0);
    if (!b)
        return;
    if (position_out)
        ph3d_vec3_copy_state(position_out, b->position);
    if (rotation_out) {
        memcpy(rotation_out, b->orientation, sizeof(b->orientation));
        quat_normalize(rotation_out);
    }
    if (scale_out) {
        scale_out[0] = ph3d_sanitize_scale_value(b->scale[0]);
        scale_out[1] = ph3d_sanitize_scale_value(b->scale[1]);
        scale_out[2] = ph3d_sanitize_scale_value(b->scale[2]);
    }
}

/// @brief `Body3D.SetVelocity(x, y, z)` — set linear velocity (m/s).
///
/// Wakes the body if dynamic and the new velocity is non-zero.
/// @param o Body3D handle to modify.
/// @param x Finite world-space X velocity.
/// @param y Finite world-space Y velocity.
/// @param z Finite world-space Z velocity.
void rt_body3d_set_velocity(void *o, double x, double y, double z) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        ph3d_vec3_set_finite(b->velocity, x, y, z);
        ph3d_vec3_sanitize_state(b->velocity);
        if (vec3_len_sq(b->velocity) > 1e-12)
            body3d_wake_if_dynamic(b);
    }
}

/// @brief `Body3D.GetVelocity` — fresh `Vec3` of linear velocity.
/// @param o Body3D handle to inspect.
/// @return Newly allocated linear-velocity Vec3, or the zero vector when invalid.
void *rt_body3d_get_velocity(void *o) {
    rt_body3d *b = body3d_checked(o);
    double velocity[3];
    if (!b)
        return rt_vec3_new(0, 0, 0);
    ph3d_vec3_copy_state(velocity, b->velocity);
    return rt_vec3_new(velocity[0], velocity[1], velocity[2]);
}

// Body3D physical-state accessors — set/get pairs for velocity,
// angular velocity, force, and torque. Setters wake the body if
// dynamic; getters return safe defaults on NULL.

/// @brief `Body3D.SetAngularVelocity(x, y, z)` — set angular velocity (radians/sec).
/// @param o Body3D handle to modify.
/// @param x Finite world-space X angular velocity.
/// @param y Finite world-space Y angular velocity.
/// @param z Finite world-space Z angular velocity.
void rt_body3d_set_angular_velocity(void *o, double x, double y, double z) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        ph3d_vec3_set_finite(b->angular_velocity, x, y, z);
        ph3d_vec3_sanitize_state(b->angular_velocity);
        if (vec3_len_sq(b->angular_velocity) > 1e-12)
            body3d_wake_if_dynamic(b);
    }
}

/// @brief `Body3D.GetAngularVelocity` — fresh `Vec3` of angular velocity.
/// @param o Body3D handle to inspect.
/// @return Newly allocated angular-velocity Vec3, or the zero vector when invalid.
void *rt_body3d_get_angular_velocity(void *o) {
    rt_body3d *b = body3d_checked(o);
    double angular_velocity[3];
    if (!b)
        return rt_vec3_new(0, 0, 0);
    ph3d_vec3_copy_state(angular_velocity, b->angular_velocity);
    return rt_vec3_new(angular_velocity[0], angular_velocity[1], angular_velocity[2]);
}

/// @brief `Body3D.ApplyForce(fx, fy, fz)` — accumulate a continuous force (N).
///
/// The force is consumed by the next `World3D.Step` and zeroed at end
/// of step. Apply once per frame for sustained force, every frame for
/// sustained acceleration. No-op for non-dynamic bodies.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param fx World-space X force component.
/// @param fy World-space Y force component.
/// @param fz World-space Z force component.
void rt_body3d_apply_force(void *o, double fx, double fy, double fz) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        if (b->motion_mode != PH3D_MODE_DYNAMIC)
            return;
        fx = ph3d_saturate_state_value(fx);
        fy = ph3d_saturate_state_value(fy);
        fz = ph3d_saturate_state_value(fz);
        ph3d_vec3_accumulate_state(b->force, fx, fy, fz);
        if (fx != 0.0 || fy != 0.0 || fz != 0.0)
            body3d_wake_if_dynamic(b);
    }
}

/// @brief Apply a continuous force at a world-space point this step, adding both
///   force and torque (r x force) — e.g. a thruster or off-center wind. Like
///   ApplyForce, it is cleared at the end of the step. Dynamic bodies only.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param fx World-space X force component.
/// @param fy World-space Y force component.
/// @param fz World-space Z force component.
/// @param px World-space X coordinate of the application point.
/// @param py World-space Y coordinate of the application point.
/// @param pz World-space Z coordinate of the application point.
void rt_body3d_apply_force_at_point(
    void *o, double fx, double fy, double fz, double px, double py, double pz) {
    rt_body3d *b = body3d_checked(o);
    double force[3];
    double r[3];
    double torque[3];
    if (!b || b->motion_mode != PH3D_MODE_DYNAMIC)
        return;
    force[0] = ph3d_saturate_state_value(fx);
    force[1] = ph3d_saturate_state_value(fy);
    force[2] = ph3d_saturate_state_value(fz);
    r[0] = ph3d_saturate_state_value(ph3d_saturate_state_value(px) - b->position[0]);
    r[1] = ph3d_saturate_state_value(ph3d_saturate_state_value(py) - b->position[1]);
    r[2] = ph3d_saturate_state_value(ph3d_saturate_state_value(pz) - b->position[2]);
    vec3_cross(r, force, torque);
    ph3d_vec3_sanitize_state(torque);
    ph3d_vec3_accumulate_state(b->force, force[0], force[1], force[2]);
    ph3d_vec3_accumulate_state(b->torque, torque[0], torque[1], torque[2]);
    if (force[0] != 0.0 || force[1] != 0.0 || force[2] != 0.0)
        body3d_wake_if_dynamic(b);
}

/// @brief `Body3D.ApplyImpulse(ix, iy, iz)` — instantaneous velocity change.
///
/// Equivalent to `Δv = impulse / mass`. Applied immediately, not at
/// step time — use for jumps, hits, explosions. No-op for non-dynamic.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param ix World-space X impulse component.
/// @param iy World-space Y impulse component.
/// @param iz World-space Z impulse component.
void rt_body3d_apply_impulse(void *o, double ix, double iy, double iz) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        if (b->motion_mode != PH3D_MODE_DYNAMIC)
            return;
        ix = ph3d_saturate_state_value(ix);
        iy = ph3d_saturate_state_value(iy);
        iz = ph3d_saturate_state_value(iz);
        ph3d_vec3_accumulate_state(
            b->velocity, ix * b->inv_mass, iy * b->inv_mass, iz * b->inv_mass);
        if (ix != 0.0 || iy != 0.0 || iz != 0.0)
            body3d_wake_if_dynamic(b);
    }
}

/// @brief Apply a linear impulse at a world-space point, producing both linear
///   and angular velocity via the lever arm from the center of mass — e.g. a hit
///   at a box corner makes it spin. Dynamic bodies only.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param ix World-space X impulse component.
/// @param iy World-space Y impulse component.
/// @param iz World-space Z impulse component.
/// @param px World-space X coordinate of the application point.
/// @param py World-space Y coordinate of the application point.
/// @param pz World-space Z coordinate of the application point.
void rt_body3d_apply_impulse_at_point(
    void *o, double ix, double iy, double iz, double px, double py, double pz) {
    rt_body3d *b = body3d_checked(o);
    double impulse[3];
    double r[3];
    if (!b || b->motion_mode != PH3D_MODE_DYNAMIC)
        return;
    impulse[0] = ph3d_saturate_state_value(ix);
    impulse[1] = ph3d_saturate_state_value(iy);
    impulse[2] = ph3d_saturate_state_value(iz);
    r[0] = ph3d_saturate_state_value(ph3d_saturate_state_value(px) - b->position[0]);
    r[1] = ph3d_saturate_state_value(ph3d_saturate_state_value(py) - b->position[1]);
    r[2] = ph3d_saturate_state_value(ph3d_saturate_state_value(pz) - b->position[2]);
    body3d_apply_contact_impulse(b, impulse, r);
    if (impulse[0] != 0.0 || impulse[1] != 0.0 || impulse[2] != 0.0)
        body3d_wake_if_dynamic(b);
}

/// @brief `Body3D.ApplyTorque(tx, ty, tz)` — accumulate a continuous torque (N·m).
///
/// Same lifecycle as `ApplyForce`: consumed and reset by the next step.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param tx World-space X torque component.
/// @param ty World-space Y torque component.
/// @param tz World-space Z torque component.
void rt_body3d_apply_torque(void *o, double tx, double ty, double tz) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        if (b->motion_mode != PH3D_MODE_DYNAMIC)
            return;
        tx = ph3d_saturate_state_value(tx);
        ty = ph3d_saturate_state_value(ty);
        tz = ph3d_saturate_state_value(tz);
        ph3d_vec3_accumulate_state(b->torque, tx, ty, tz);
        if (tx != 0.0 || ty != 0.0 || tz != 0.0)
            body3d_wake_if_dynamic(b);
    }
}

/// @brief `Body3D.ApplyAngularImpulse(ix, iy, iz)` — apply a world-space angular impulse.
/// @details The stored diagonal inverse inertia is expressed in the body's local
///   principal-axis frame. The shared world-inertia helper evaluates
///   `R * inverse(I_local) * transpose(R) * impulse`, so a rotated anisotropic body responds on
///   the correct world axes instead of treating its local diagonal as world-space.
/// @param o Body3D handle to modify and wake when dynamic.
/// @param ix World-space X angular-impulse component.
/// @param iy World-space Y angular-impulse component.
/// @param iz World-space Z angular-impulse component.
void rt_body3d_apply_angular_impulse(void *o, double ix, double iy, double iz) {
    rt_body3d *b = body3d_checked(o);
    double impulse[3];
    double angular_delta[3];
    if (b) {
        if (b->motion_mode != PH3D_MODE_DYNAMIC)
            return;
        impulse[0] = ph3d_saturate_state_value(ix);
        impulse[1] = ph3d_saturate_state_value(iy);
        impulse[2] = ph3d_saturate_state_value(iz);
        body3d_world_inv_inertia_mul(b, impulse, angular_delta);
        ph3d_vec3_accumulate_state(
            b->angular_velocity, angular_delta[0], angular_delta[1], angular_delta[2]);
        if (impulse[0] != 0.0 || impulse[1] != 0.0 || impulse[2] != 0.0)
            body3d_wake_if_dynamic(b);
    }
}

// Body3D material/filter property setters and getters — pure assigns
// with NULL-guards. Damping setters clamp negatives to zero.

/// @brief `Body3D.SetRestitution(r)` — set bounciness (0=no bounce, 1=elastic).
/// @param o Body3D handle to modify.
/// @param r Restitution coefficient, clamped to the inclusive range zero to one.
void rt_body3d_set_restitution(void *o, double r) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        double value = ph3d_clamp_nonnegative_finite(r, 0.0);
        b->restitution = value > 1.0 ? 1.0 : value;
    }
}

/// @brief `Body3D.GetRestitution` — read bounciness.
/// @param o Body3D handle to inspect.
/// @return Sanitized restitution coefficient in the inclusive range zero to one.
double rt_body3d_get_restitution(void *o) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return 0;
    double value = b->restitution;
    if (isnan(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    return value > 1.0 ? 1.0 : value;
}

/// @brief `Body3D.SetFriction(f)` — set friction coefficient.
///
/// Pair friction is `sqrt(a.friction * b.friction)` — geometric mean
/// matches typical material-interaction tables.
/// @param o Body3D handle to modify.
/// @param f Requested non-negative friction coefficient.
void rt_body3d_set_friction(void *o, double f) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        b->friction = ph3d_clamp_nonnegative_finite(f, 0.0);
}

/// @brief `Body3D.GetFriction` — read friction coefficient.
/// @param o Body3D handle to inspect.
/// @return Sanitized non-negative friction coefficient, or zero when invalid.
double rt_body3d_get_friction(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? ph3d_clamp_nonnegative_finite(b->friction, 0.0) : 0;
}

/// @brief `Body3D.SetUserData(v)` — opaque gameplay handle; never read by the runtime.
/// @param o Body3D handle to modify.
/// @param value Application-defined signed integer value.
void rt_body3d_set_user_data(void *o, int64_t value) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        b->user_data = value;
}

/// @brief `Body3D.GetUserData` — the opaque gameplay handle (0 by default).
/// @param o Body3D handle to inspect.
/// @return Stored application-defined value, or zero when invalid.
int64_t rt_body3d_get_user_data(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? b->user_data : 0;
}

/// @brief `Body3D.SetLinearDamping(d)` — per-second linear velocity decay.
///
/// Each step scales velocity by `max(0, 1 - d*dt)`. Useful as a soft
/// air-resistance proxy. Negative values clamp to 0.
/// @param o Body3D handle to modify.
/// @param d Requested non-negative linear damping coefficient.
void rt_body3d_set_linear_damping(void *o, double d) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        b->linear_damping = ph3d_clamp_nonnegative_finite(d, 0.0);
}

/// @brief `Body3D.GetLinearDamping` — read linear damping coefficient.
/// @param o Body3D handle to inspect.
/// @return Sanitized non-negative linear damping coefficient, or zero when invalid.
double rt_body3d_get_linear_damping(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? ph3d_clamp_nonnegative_finite(b->linear_damping, 0.0) : 0.0;
}

/// @brief `Body3D.SetAngularDamping(d)` — per-second angular velocity decay.
/// @param o Body3D handle to modify.
/// @param d Requested non-negative angular damping coefficient.
void rt_body3d_set_angular_damping(void *o, double d) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        b->angular_damping = ph3d_clamp_nonnegative_finite(d, 0.0);
}

/// @brief `Body3D.GetAngularDamping` — read angular damping coefficient.
/// @param o Body3D handle to inspect.
/// @return Sanitized non-negative angular damping coefficient, or zero when invalid.
double rt_body3d_get_angular_damping(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? ph3d_clamp_nonnegative_finite(b->angular_damping, 0.0) : 0.0;
}

/// @brief Invalidate collision state after a filter or trigger-mode change.
/// @details The broadphase must re-evaluate candidate pairs and the contact/event
///   history must forget pairs involving this body so the next Step reports
///   transitions from the new filter state, not stale previous contacts.
/// @param body Body3D instance whose broadphase and contact state is invalidated.
static void body3d_invalidate_filter_contacts(rt_body3d *body) {
    if (!body)
        return;
    body3d_touch_broadphase(body);
    body3d_wake_if_dynamic(body);
    if (body->owner_world)
        world3d_purge_body_contacts(body->owner_world, body);
}

/// @brief `Body3D.SetCollisionLayer(l)` — bitmask labeling this body's category.
/// @param o Body3D handle to modify.
/// @param l Positive collision-layer bit field.
void rt_body3d_set_collision_layer(void *o, int64_t l) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    if (l <= 0) {
        rt_trap("Physics3DBody.SetCollisionLayer: layer must be a positive bitmask");
        return;
    }
    if (b->collision_layer == l)
        return;
    b->collision_layer = l;
    body3d_invalidate_filter_contacts(b);
}

/// @brief `Body3D.GetCollisionLayer` — read this body's layer bitmask.
/// @param o Body3D handle to inspect.
/// @return Positive collision-layer bit field, or zero when invalid.
int64_t rt_body3d_get_collision_layer(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? (b->collision_layer > 0 ? b->collision_layer : 1) : 0;
}

/// @brief `Body3D.SetCollisionMask(m)` — bitmask of layers this body collides with.
/// @param o Body3D handle to modify.
/// @param m Collision-layer mask accepted verbatim.
void rt_body3d_set_collision_mask(void *o, int64_t m) {
    rt_body3d *b = body3d_checked(o);
    if (!b || b->collision_mask == m)
        return;
    b->collision_mask = m;
    body3d_invalidate_filter_contacts(b);
}

/// @brief `Body3D.GetCollisionMask` — read this body's collision mask.
/// @param o Body3D handle to inspect.
/// @return Stored collision mask, or zero when invalid.
int64_t rt_body3d_get_collision_mask(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? b->collision_mask : 0;
}

/// @brief `Body3D.SetStatic(s)` — toggle static motion mode.
///
/// Traps if the body's collider is incompatible with the requested
/// mode (e.g., switching a triangle-mesh-collider body to dynamic).
/// Refreshes derived state (inv_mass, inv_inertia) on success.
/// @param o Body3D handle to modify.
/// @param s Nonzero to select static mode; zero to select dynamic mode.
void rt_body3d_set_static(void *o, int8_t s) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        int32_t desired_mode = s ? PH3D_MODE_STATIC : PH3D_MODE_DYNAMIC;
        if (!body3d_motion_mode_allowed(
                b,
                b->collider,
                desired_mode,
                "Physics3DBody.set_Static: collider requires a static body"))
            return;
        b->motion_mode = desired_mode;
        body3d_refresh_motion_mode(b);
        body3d_touch_broadphase(b);
    }
}

/// @brief `Body3D.IsStatic` — true if the body is in static motion mode.
/// @param o Body3D handle to inspect.
/// @return One when the body is static, otherwise zero.
int8_t rt_body3d_is_static(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->motion_mode == PH3D_MODE_STATIC ? 1 : 0;
}

/// @brief `Body3D.SetKinematic(k)` — toggle kinematic motion mode.
///
/// Kinematic bodies move under script control (position/velocity set
/// directly) but are treated as infinitely massive by the impulse
/// solver — they push dynamic bodies but aren't pushed in return.
/// @param o Body3D handle to modify.
/// @param k Nonzero to select kinematic mode; zero to select dynamic mode.
void rt_body3d_set_kinematic(void *o, int8_t k) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        int32_t desired_mode = k ? PH3D_MODE_KINEMATIC : PH3D_MODE_DYNAMIC;
        if (!body3d_motion_mode_allowed(
                b,
                b->collider,
                desired_mode,
                "Physics3DBody.set_Kinematic: collider requires a static body"))
            return;
        b->motion_mode = desired_mode;
        body3d_refresh_motion_mode(b);
        body3d_touch_broadphase(b);
    }
}

/// @brief `Body3D.IsKinematic` — true if the body is in kinematic motion mode.
/// @param o Body3D handle to inspect.
/// @return One when the body is kinematic, otherwise zero.
int8_t rt_body3d_is_kinematic(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->motion_mode == PH3D_MODE_KINEMATIC ? 1 : 0;
}

/// @brief `Body3D.SetTrigger(t)` — toggle "report contacts but don't physically respond".
///
/// Trigger bodies fire collision events but don't apply impulses or
/// positional correction — useful for damage volumes, pickup zones, etc.
/// @param o Body3D handle to modify.
/// @param t Nonzero to enable trigger-only response; zero for solid response.
void rt_body3d_set_trigger(void *o, int8_t t) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    int8_t enabled = t ? 1 : 0;
    if (b->is_trigger == enabled)
        return;
    b->is_trigger = enabled;
    body3d_invalidate_filter_contacts(b);
}

/// @brief `Body3D.IsTrigger` — true if the body is in trigger-only mode.
/// @param o Body3D handle to inspect.
/// @return One when trigger-only response is enabled, otherwise zero.
int8_t rt_body3d_is_trigger(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->is_trigger ? 1 : 0;
}

/// @brief `Body3D.SetCanSleep(canSleep)` — toggle automatic sleep eligibility.
///
/// When false, the body never enters sleep mode (always integrated).
/// Disabling sleep on a quiet body wakes it immediately so the next
/// step processes it normally.
/// @param o Body3D handle to modify.
/// @param can_sleep Nonzero to permit automatic sleeping; zero to forbid it.
void rt_body3d_set_can_sleep(void *o, int8_t can_sleep) {
    rt_body3d *b = body3d_checked(o);
    if (b) {
        b->can_sleep = can_sleep ? 1 : 0;
        if (!b->can_sleep) {
            b->is_sleeping = 0;
            b->sleep_time = 0.0;
        }
    }
}

/// @brief `Body3D.CanSleep` — read sleep eligibility flag.
/// @param o Body3D handle to inspect.
/// @return One when the body may sleep, otherwise zero.
int8_t rt_body3d_can_sleep(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->can_sleep ? 1 : 0;
}

/// @brief `Body3D.IsSleeping` — true when the body is currently quiescent.
/// @param o Body3D handle to inspect.
/// @return One when the body is asleep, otherwise zero.
int8_t rt_body3d_is_sleeping(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->is_sleeping ? 1 : 0;
}

/// @brief `Body3D.Wake` — force the body awake (no-op for non-dynamic).
/// @param o Body3D handle to wake.
void rt_body3d_wake(void *o) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        body3d_wake_if_dynamic(b);
}

/// @brief `Body3D.Sleep` — manually put the body to sleep.
///
/// Zeros velocity / angular velocity / accumulated forces. No-op
/// when the body can't sleep or isn't dynamic. Bypasses the normal
/// sleep-delay countdown.
/// @param o Body3D handle to put to sleep.
void rt_body3d_sleep(void *o) {
    rt_body3d *b = body3d_checked(o);
    if (!b)
        return;
    if (b->motion_mode != PH3D_MODE_DYNAMIC || !b->can_sleep)
        return;
    b->is_sleeping = 1;
    b->sleep_time = PH3D_SLEEP_DELAY;
    vec3_set(b->velocity, 0.0, 0.0, 0.0);
    vec3_set(b->angular_velocity, 0.0, 0.0, 0.0);
    vec3_set(b->force, 0.0, 0.0, 0.0);
    vec3_set(b->torque, 0.0, 0.0, 0.0);
}

/// @brief `Body3D.SetUseCcd(useCcd)` — opt this body into CCD substeps.
///
/// CCD subdivides high-speed steps to prevent tunneling through thin
/// geometry. Enable for fast-moving bodies (bullets, balls); leave off
/// otherwise to save CPU.
/// @param o Body3D handle to modify.
/// @param use_ccd Nonzero to enable continuous collision detection; zero to disable it.
void rt_body3d_set_use_ccd(void *o, int8_t use_ccd) {
    rt_body3d *b = body3d_checked(o);
    if (b)
        b->use_ccd = use_ccd ? 1 : 0;
}

/// @brief `Body3D.GetUseCcd` — read CCD opt-in flag.
/// @param o Body3D handle to inspect.
/// @return One when continuous collision detection is enabled, otherwise zero.
int8_t rt_body3d_get_use_ccd(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->use_ccd ? 1 : 0;
}

/// @brief `Body3D.IsGrounded` — true when the body has an upward contact this frame.
///
/// "Upward" = contact normal Y > 0.7 (~45° max slope). Set/cleared by
/// `World3D.Step`; queryable from frame to frame.
/// @param o Body3D handle to inspect.
/// @return One when the latest contact state is grounded, otherwise zero.
int8_t rt_body3d_is_grounded(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b && b->is_grounded ? 1 : 0;
}

/// @brief `Body3D.GroundNormal` — fresh `Vec3` of the latest ground normal.
///
/// Defaults to +Y when not grounded.
/// @param o Body3D handle to inspect.
/// @return Newly allocated normalized ground vector, defaulting to positive Y.
void *rt_body3d_get_ground_normal(void *o) {
    rt_body3d *b = body3d_checked(o);
    double normal[3];
    if (!b)
        return rt_vec3_new(0, 1, 0);
    ph3d_vec3_copy_state(normal, b->ground_normal);
    if (vec3_normalize_in_place(normal) <= 1e-12)
        vec3_set(normal, 0.0, 1.0, 0.0);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `Body3D.Mass` — read the body's mass (zero for static).
/// @param o Body3D handle to inspect.
/// @return Sanitized non-negative body mass, or zero when invalid.
double rt_body3d_get_mass(void *o) {
    rt_body3d *b = body3d_checked(o);
    return b ? ph3d_clamp_nonnegative_finite(b->mass, 0.0) : 0;
}
