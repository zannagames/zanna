//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_vehicle3d.c
// Purpose: Raycast vehicle simulation on top of Physics3D rigid bodies. Each
//   wheel is a suspension raycast from a chassis-local anchor: spring/damper
//   forces hold the chassis up, and tire forces (drive, brake, lateral grip)
//   are applied at the contact patch, clamped by a load-scaled friction circle.
// Key invariants:
//   - The chassis is an ordinary dynamic Body3D owned by the caller; the
//     vehicle applies per-step FORCES (consumed by the next World3D.Step) and
//     never writes body state directly.
//   - Step(dt) must run before World3D.Step(dt) each frame so the forces are
//     integrated by that step.
//   - Wheel raycasts ignore the chassis body itself and honor the vehicle's
//     collision mask.
//   - All public entry points sanitize non-finite inputs (NaN never enters
//     body force accumulators).
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64; retains the world and chassis body,
//     released by the finalizer.
// Links: rt_physics3d.h, rt_physics3d_internal.h, docs/zannalib/graphics3d
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements load-limited raycast suspension and tire forces for `Vehicle3D`.
///
/// Each configured wheel casts from a chassis-local suspension anchor, derives spring and
/// damper load from the latest chassis motion, and applies suspension, steering, drive, brake,
/// and lateral forces at its contact patch. All forces are accumulated on the caller-owned
/// chassis for the following world step.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_physics3d.h"

#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d_internal.h"
#include "rt_trap.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/// @brief Allocates a runtime object payload with a graphics class identifier.
/// @param class_id Runtime class identifier.
/// @param byte_size Positive payload size in bytes.
/// @return Owned object handle, or null on allocation failure.
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);

/// @brief Assigns the cleanup callback invoked when an object is reclaimed.
/// @param obj Borrowed runtime object.
/// @param fn Native finalizer callback.
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

/// @brief Retains a nullable runtime object reference.
/// @param obj Borrowed object; null is a no-op.
extern void rt_obj_retain_maybe(void *obj);

#define VEHICLE3D_MAX_WHEELS 8
#define VEHICLE3D_PARAM_MAX 1.0e9
#define VEHICLE3D_DEFAULT_DRIVE_FORCE 4500.0
#define VEHICLE3D_DEFAULT_BRAKE_FORCE 6000.0
#define VEHICLE3D_DEFAULT_MAX_STEER_DEG 32.0
#define VEHICLE3D_DEFAULT_LONG_GRIP 2.2
#define VEHICLE3D_DEFAULT_LAT_GRIP 2.6

/// @brief One suspension wheel: chassis-local anchor plus tuning and per-step state.
typedef struct {
    double local_anchor[3];  /* suspension top attachment, chassis space */
    double radius;           /* wheel radius (ray length = rest + radius) */
    double suspension_rest;  /* rest length of the suspension travel */
    double stiffness;        /* spring N/m (per unit compression) */
    double damping;          /* damper N/(m/s) along the suspension axis */
    int8_t steers;           /* steering input applies to this wheel */
    int8_t driven;           /* throttle torque applies to this wheel */
    /* Per-step results (readable after Step) */
    int8_t in_contact;
    double travel;        /* current suspension length (rest when airborne) */
    double contact_load;  /* last spring+damper force magnitude (N) */
    double contact_point[3];
} rt_vehicle3d_wheel;

/// @brief Vehicle payload: retained world + chassis, wheels, inputs, tuning.
typedef struct {
    void *vptr;
    void *world;   /* retained Physics3DWorld */
    void *chassis; /* retained Physics3DBody (dynamic) */
    rt_vehicle3d_wheel wheels[VEHICLE3D_MAX_WHEELS];
    int32_t wheel_count;
    double throttle; /* [-1, 1] */
    double brake;    /* [0, 1] */
    double steer;    /* [-1, 1] */
    double drive_force;
    double brake_force;
    double max_steer_rad;
    double long_grip;
    double lat_grip;
    int64_t collision_mask;
} rt_vehicle3d;

/// @brief Validate @p obj as a Vehicle3D handle (NULL on mismatch).
/// @param obj Borrowed runtime object to validate.
/// @return Borrowed vehicle payload, or null when the class does not match.
static rt_vehicle3d *vehicle3d_checked(void *obj) {
    return (rt_vehicle3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEHICLE3D_CLASS_ID);
}

/// @brief Clamp @p value to [lo, hi], mapping non-finite input to @p fallback.
/// @param value Candidate scalar.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param fallback Value returned for a non-finite candidate.
/// @return Finite fallback or bounded candidate.
static double vehicle3d_clamp_or(double value, double lo, double hi, double fallback) {
    if (!isfinite(value))
        return fallback;
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief GC finalizer: release the retained world and chassis references.
/// @param obj Owned `rt_vehicle3d` payload being finalized; null is a no-op.
static void vehicle3d_finalizer(void *obj) {
    rt_vehicle3d *v = (rt_vehicle3d *)obj;
    if (!v)
        return;
    rt_g3d_ref_slot_release(&v->world);
    rt_g3d_ref_slot_release(&v->chassis);
}

/// @brief `Vehicle3D.New(world, chassisBody)` — create a raycast vehicle.
/// @details The chassis must be a dynamic Body3D already added to @p world.
///   Wheels start empty; add them with AddWheel, then call Step(dt) before
///   each World3D.Step(dt).
/// @param world Borrowed `World3D` retained for the vehicle's lifetime.
/// @param chassis Borrowed `Body3D` retained for the vehicle's lifetime; dynamic registration is
///        an operational precondition checked by later stepping.
/// @return Owned `Vehicle3D`, or null after trapping on invalid types or allocation failure.
void *rt_vehicle3d_new(void *world, void *chassis) {
    rt_vehicle3d *v;
    if (!rt_g3d_has_class(world, RT_G3D_WORLD3D_CLASS_ID)) {
        rt_trap("Vehicle3D.New: world must be a Physics3DWorld");
        return NULL;
    }
    if (!rt_g3d_has_class(chassis, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("Vehicle3D.New: chassis must be a Physics3DBody");
        return NULL;
    }
    v = (rt_vehicle3d *)rt_obj_new_i64(RT_G3D_VEHICLE3D_CLASS_ID, (int64_t)sizeof(rt_vehicle3d));
    if (!v) {
        rt_trap("Vehicle3D.New: allocation failed");
        return NULL;
    }
    memset((char *)v + sizeof(void *), 0, sizeof(*v) - sizeof(void *));
    rt_obj_retain_maybe(world);
    v->world = world;
    rt_obj_retain_maybe(chassis);
    v->chassis = chassis;
    v->drive_force = VEHICLE3D_DEFAULT_DRIVE_FORCE;
    v->brake_force = VEHICLE3D_DEFAULT_BRAKE_FORCE;
    v->max_steer_rad = VEHICLE3D_DEFAULT_MAX_STEER_DEG * (3.14159265358979323846 / 180.0);
    v->long_grip = VEHICLE3D_DEFAULT_LONG_GRIP;
    v->lat_grip = VEHICLE3D_DEFAULT_LAT_GRIP;
    v->collision_mask = -1;
    rt_obj_set_finalizer(v, vehicle3d_finalizer);
    return v;
}

/// @brief `Vehicle3D.AddWheel(x, y, z, radius, rest, stiffness, damping, steers, driven)`.
/// @details @p x/y/z is the suspension TOP anchor in chassis-local space (the
///   ray starts there and casts down chassis -Y for rest + radius). Returns
///   the wheel index, or -1 when the wheel table is full or inputs are
///   degenerate.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param x Chassis-local suspension-anchor x coordinate.
/// @param y Chassis-local suspension-anchor y coordinate.
/// @param z Chassis-local suspension-anchor z coordinate.
/// @param radius Positive wheel radius in world units.
/// @param suspension_rest Positive unloaded suspension length in world units.
/// @param stiffness Positive spring stiffness in newtons per metre.
/// @param damping Non-negative damping coefficient in newton-seconds per metre.
/// @param steers Nonzero when steering input rotates this wheel's tire frame.
/// @param driven Nonzero when this wheel shares the throttle force budget.
/// @return New wheel index in `[0, 7]`, or `-1` for an invalid vehicle, full wheel table, or
///         unusable dimensions/stiffness.
int64_t rt_vehicle3d_add_wheel(void *obj,
                               double x,
                               double y,
                               double z,
                               double radius,
                               double suspension_rest,
                               double stiffness,
                               double damping,
                               int8_t steers,
                               int8_t driven) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    rt_vehicle3d_wheel *w;
    if (!v) {
        rt_trap("Vehicle3D.AddWheel: invalid vehicle");
        return -1;
    }
    if (v->wheel_count >= VEHICLE3D_MAX_WHEELS)
        return -1;
    radius = vehicle3d_clamp_or(radius, 1e-4, VEHICLE3D_PARAM_MAX, 0.0);
    suspension_rest = vehicle3d_clamp_or(suspension_rest, 1e-4, VEHICLE3D_PARAM_MAX, 0.0);
    stiffness = vehicle3d_clamp_or(stiffness, 0.0, VEHICLE3D_PARAM_MAX, 0.0);
    damping = vehicle3d_clamp_or(damping, 0.0, VEHICLE3D_PARAM_MAX, 0.0);
    if (radius <= 0.0 || suspension_rest <= 0.0 || stiffness <= 0.0)
        return -1;
    w = &v->wheels[v->wheel_count];
    memset(w, 0, sizeof(*w));
    w->local_anchor[0] = vehicle3d_clamp_or(x, -VEHICLE3D_PARAM_MAX, VEHICLE3D_PARAM_MAX, 0.0);
    w->local_anchor[1] = vehicle3d_clamp_or(y, -VEHICLE3D_PARAM_MAX, VEHICLE3D_PARAM_MAX, 0.0);
    w->local_anchor[2] = vehicle3d_clamp_or(z, -VEHICLE3D_PARAM_MAX, VEHICLE3D_PARAM_MAX, 0.0);
    w->radius = radius;
    w->suspension_rest = suspension_rest;
    w->stiffness = stiffness;
    w->damping = damping;
    w->steers = steers ? 1 : 0;
    w->driven = driven ? 1 : 0;
    w->travel = suspension_rest;
    return v->wheel_count++;
}

/// @brief `Vehicle3D.SetInput(throttle, brake, steer)` — per-frame controls.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param throttle Signed drive input clamped to `[-1, 1]`.
/// @param brake Braking input clamped to `[0, 1]`.
/// @param steer Signed steering input clamped to `[-1, 1]`.
void rt_vehicle3d_set_input(void *obj, double throttle, double brake, double steer) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (!v)
        return;
    v->throttle = vehicle3d_clamp_or(throttle, -1.0, 1.0, 0.0);
    v->brake = vehicle3d_clamp_or(brake, 0.0, 1.0, 0.0);
    v->steer = vehicle3d_clamp_or(steer, -1.0, 1.0, 0.0);
}

/// @brief `Vehicle3D.SetDriveForce(newtons)` — total driven-wheel force budget.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param newtons Requested non-negative total force, capped at the parameter maximum; non-finite
///        values restore the default.
void rt_vehicle3d_set_drive_force(void *obj, double newtons) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (v)
        v->drive_force = vehicle3d_clamp_or(newtons, 0.0, VEHICLE3D_PARAM_MAX,
                                            VEHICLE3D_DEFAULT_DRIVE_FORCE);
}

/// @brief `Vehicle3D.SetBrakeForce(newtons)` — total brake force budget.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param newtons Requested non-negative total force, capped at the parameter maximum; non-finite
///        values restore the default.
void rt_vehicle3d_set_brake_force(void *obj, double newtons) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (v)
        v->brake_force = vehicle3d_clamp_or(newtons, 0.0, VEHICLE3D_PARAM_MAX,
                                            VEHICLE3D_DEFAULT_BRAKE_FORCE);
}

/// @brief `Vehicle3D.SetMaxSteer(degrees)` — full-lock steering angle.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param degrees Requested magnitude clamped to `[0, 85]`; non-finite values restore the default.
void rt_vehicle3d_set_max_steer(void *obj, double degrees) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (v)
        v->max_steer_rad = vehicle3d_clamp_or(degrees, 0.0, 85.0, VEHICLE3D_DEFAULT_MAX_STEER_DEG) *
                           (3.14159265358979323846 / 180.0);
}

/// @brief `Vehicle3D.SetGrip(longitudinal, lateral)` — tire friction coefficients.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param longitudinal Non-negative longitudinal load multiplier clamped to 100.
/// @param lateral Non-negative lateral load multiplier clamped to 100.
void rt_vehicle3d_set_grip(void *obj, double longitudinal, double lateral) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (!v)
        return;
    v->long_grip = vehicle3d_clamp_or(longitudinal, 0.0, 100.0, VEHICLE3D_DEFAULT_LONG_GRIP);
    v->lat_grip = vehicle3d_clamp_or(lateral, 0.0, 100.0, VEHICLE3D_DEFAULT_LAT_GRIP);
}

/// @brief `Vehicle3D.SetCollisionMask(mask)` — layers wheel rays may hit.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param mask Collision-layer bit mask passed to every suspension raycast.
void rt_vehicle3d_set_collision_mask(void *obj, int64_t mask) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (v)
        v->collision_mask = mask;
}

/// @brief Rotate chassis-local vector @p local into world space by the chassis orientation.
/// @param body Borrowed chassis body supplying its orientation.
/// @param local Borrowed three-element chassis-local direction.
/// @param[out] out Three-element world-space direction.
static void vehicle3d_local_to_world_dir(const rt_body3d *body, const double local[3],
                                         double out[3]) {
    quat_rotate_vec3(body->orientation, local, out);
}

/// @brief `Vehicle3D.Step(dt)` — cast suspension rays and apply wheel forces.
/// @details Call once per frame BEFORE `World3D.Step(dt)`: the forces land in
///   the chassis body's per-step accumulators and are consumed by that step.
///   Per wheel: suspension spring/damper along chassis up, then tire forces at
///   the contact patch — drive along the (steered) wheel forward, brake
///   opposing longitudinal motion, and lateral grip cancelling side slip —
///   all clamped by a load-scaled friction circle so an unloaded wheel cannot
///   generate grip.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param dt Positive finite frame interval in seconds, used to cap side-slip cancellation.
void rt_vehicle3d_step(void *obj, double dt) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    rt_body3d *body;
    double up[3];
    double fwd[3];
    double local_up[3] = {0.0, 1.0, 0.0};
    double local_fwd[3] = {0.0, 0.0, 1.0};
    if (!v || !isfinite(dt) || dt <= 0.0)
        return;
    body = (rt_body3d *)rt_g3d_checked_or_null(v->chassis, RT_G3D_BODY3D_CLASS_ID);
    if (!body || !v->world || v->wheel_count <= 0)
        return;
    if (body->motion_mode != PH3D_MODE_DYNAMIC)
        return;
    vehicle3d_local_to_world_dir(body, local_up, up);
    vehicle3d_local_to_world_dir(body, local_fwd, fwd);

    for (int32_t i = 0; i < v->wheel_count; i++) {
        rt_vehicle3d_wheel *w = &v->wheels[i];
        double anchor_world[3];
        double ray_dir[3] = {-up[0], -up[1], -up[2]};
        double ray_len = w->suspension_rest + w->radius;
        double hit_distance = -1.0;
        void *hit_body;
        double rotated[3];

        quat_rotate_vec3(body->orientation, w->local_anchor, rotated);
        anchor_world[0] = body->position[0] + rotated[0];
        anchor_world[1] = body->position[1] + rotated[1];
        anchor_world[2] = body->position[2] + rotated[2];

        w->in_contact = 0;
        w->contact_load = 0.0;
        w->travel = w->suspension_rest;

        hit_body = rt_world3d_raycast_closest_body_raw(v->world,
                                                       anchor_world[0],
                                                       anchor_world[1],
                                                       anchor_world[2],
                                                       ray_dir[0],
                                                       ray_dir[1],
                                                       ray_dir[2],
                                                       ray_len,
                                                       v->collision_mask,
                                                       body, /* rays start inside the chassis */
                                                       &hit_distance);
        if (!hit_body || !isfinite(hit_distance) || hit_distance <= 0.0 ||
            hit_distance > ray_len)
            continue;

        {
            double suspension_len = hit_distance - w->radius;
            double compression;
            double point_vel[3];
            double rel_anchor[3];
            double v_along_up;
            double spring_force;
            double damper_force;
            double load;
            double contact[3];
            double wheel_fwd[3];
            double wheel_right[3];
            double v_fwd;
            double v_lat;
            double f_long;
            double f_lat;
            double max_grip_long;
            double max_grip_lat;

            if (suspension_len < 0.0)
                suspension_len = 0.0;
            if (suspension_len > w->suspension_rest)
                suspension_len = w->suspension_rest;
            compression = w->suspension_rest - suspension_len;
            w->travel = suspension_len;

            contact[0] = anchor_world[0] + ray_dir[0] * hit_distance;
            contact[1] = anchor_world[1] + ray_dir[1] * hit_distance;
            contact[2] = anchor_world[2] + ray_dir[2] * hit_distance;
            w->contact_point[0] = contact[0];
            w->contact_point[1] = contact[1];
            w->contact_point[2] = contact[2];

            rel_anchor[0] = anchor_world[0] - body->position[0];
            rel_anchor[1] = anchor_world[1] - body->position[1];
            rel_anchor[2] = anchor_world[2] - body->position[2];
            body3d_contact_velocity(body, rel_anchor, point_vel);
            v_along_up = point_vel[0] * up[0] + point_vel[1] * up[1] + point_vel[2] * up[2];

            spring_force = w->stiffness * compression;
            damper_force = -w->damping * v_along_up;
            load = spring_force + damper_force;
            if (!isfinite(load) || load < 0.0)
                load = 0.0;
            if (load > VEHICLE3D_PARAM_MAX)
                load = VEHICLE3D_PARAM_MAX;
            w->in_contact = 1;
            w->contact_load = load;

            /* Suspension pushes the chassis along +up at the anchor. */
            rt_body3d_apply_force_at_point(v->chassis,
                                           up[0] * load,
                                           up[1] * load,
                                           up[2] * load,
                                           anchor_world[0],
                                           anchor_world[1],
                                           anchor_world[2]);

            /* Tire frame: steered forward projected onto the ground plane
             * (approximated by the chassis up plane), right = fwd x up. */
            wheel_fwd[0] = fwd[0];
            wheel_fwd[1] = fwd[1];
            wheel_fwd[2] = fwd[2];
            if (w->steers && v->max_steer_rad > 0.0 && v->steer != 0.0) {
                double angle = v->steer * v->max_steer_rad;
                double axis[4];
                double s = sin(angle * 0.5);
                axis[0] = up[0] * s;
                axis[1] = up[1] * s;
                axis[2] = up[2] * s;
                axis[3] = cos(angle * 0.5);
                quat_rotate_vec3(axis, fwd, wheel_fwd);
            }
            wheel_right[0] = wheel_fwd[1] * up[2] - wheel_fwd[2] * up[1];
            wheel_right[1] = wheel_fwd[2] * up[0] - wheel_fwd[0] * up[2];
            wheel_right[2] = wheel_fwd[0] * up[1] - wheel_fwd[1] * up[0];

            {
                double contact_rel[3];
                double contact_vel[3];
                contact_rel[0] = contact[0] - body->position[0];
                contact_rel[1] = contact[1] - body->position[1];
                contact_rel[2] = contact[2] - body->position[2];
                body3d_contact_velocity(body, contact_rel, contact_vel);
                v_fwd = contact_vel[0] * wheel_fwd[0] + contact_vel[1] * wheel_fwd[1] +
                        contact_vel[2] * wheel_fwd[2];
                v_lat = contact_vel[0] * wheel_right[0] + contact_vel[1] * wheel_right[1] +
                        contact_vel[2] * wheel_right[2];
            }

            /* Longitudinal: drive (driven wheels split the budget) + brake. */
            f_long = 0.0;
            if (w->driven && v->throttle != 0.0) {
                int32_t driven_count = 0;
                for (int32_t k = 0; k < v->wheel_count; k++)
                    driven_count += v->wheels[k].driven ? 1 : 0;
                if (driven_count > 0)
                    f_long += v->throttle * v->drive_force / (double)driven_count;
            }
            if (v->brake > 0.0 && fabs(v_fwd) > 1e-3) {
                double brake_share = v->brake * v->brake_force / (double)v->wheel_count;
                f_long += (v_fwd > 0.0 ? -brake_share : brake_share);
            }

            /* Lateral: cancel side slip within one step's impulse budget. */
            f_lat = -v_lat * v->lat_grip * load;
            {
                /* Don't overshoot: cap at the force that zeroes v_lat in dt
                 * for the wheel's share of the chassis mass. */
                double share_mass = body->mass / (double)v->wheel_count;
                double cancel = share_mass > 0.0 ? -v_lat * share_mass / dt : 0.0;
                if ((f_lat > 0.0 && f_lat > cancel && cancel > 0.0) ||
                    (f_lat < 0.0 && f_lat < cancel && cancel < 0.0))
                    f_lat = cancel;
            }

            /* Friction circle: |F| <= grip * load, per axis then combined. */
            max_grip_long = v->long_grip * load;
            max_grip_lat = v->lat_grip * load;
            if (f_long > max_grip_long)
                f_long = max_grip_long;
            if (f_long < -max_grip_long)
                f_long = -max_grip_long;
            if (f_lat > max_grip_lat)
                f_lat = max_grip_lat;
            if (f_lat < -max_grip_lat)
                f_lat = -max_grip_lat;
            {
                double total = sqrt(f_long * f_long + f_lat * f_lat);
                double budget = fmax(max_grip_long, max_grip_lat);
                if (isfinite(total) && total > budget && total > 1e-9) {
                    double scale = budget / total;
                    f_long *= scale;
                    f_lat *= scale;
                }
            }

            if (isfinite(f_long) && isfinite(f_lat)) {
                rt_body3d_apply_force_at_point(v->chassis,
                                               wheel_fwd[0] * f_long + wheel_right[0] * f_lat,
                                               wheel_fwd[1] * f_long + wheel_right[1] * f_lat,
                                               wheel_fwd[2] * f_long + wheel_right[2] * f_lat,
                                               contact[0],
                                               contact[1],
                                               contact[2]);
            }
        }
    }
}

/// @brief `Vehicle3D.get_Speed` — signed speed along the chassis forward (m/s).
/// @param obj Borrowed `Vehicle3D` handle.
/// @return Signed chassis velocity projected onto its world-space local +Z axis, or zero for an
///         invalid vehicle/chassis.
double rt_vehicle3d_get_speed(void *obj) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    rt_body3d *body;
    double fwd[3];
    double local_fwd[3] = {0.0, 0.0, 1.0};
    if (!v)
        return 0.0;
    body = (rt_body3d *)rt_g3d_checked_or_null(v->chassis, RT_G3D_BODY3D_CLASS_ID);
    if (!body)
        return 0.0;
    vehicle3d_local_to_world_dir(body, local_fwd, fwd);
    return body->velocity[0] * fwd[0] + body->velocity[1] * fwd[1] + body->velocity[2] * fwd[2];
}

/// @brief `Vehicle3D.get_WheelCount` — number of wheels added.
/// @param obj Borrowed `Vehicle3D` handle.
/// @return Configured wheel count in `[0, 8]`, or zero for an invalid handle.
int64_t rt_vehicle3d_get_wheel_count(void *obj) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    return v ? v->wheel_count : 0;
}

/// @brief `Vehicle3D.WheelInContact(i)` — whether wheel @p i touched ground last Step.
/// @param obj Borrowed `Vehicle3D` handle.
/// @param index Zero-based wheel index.
/// @return `1` when the wheel produced a valid suspension hit during its most recent vehicle step;
///         `0` for no hit or an invalid handle/index.
int8_t rt_vehicle3d_wheel_in_contact(void *obj, int64_t index) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (!v || index < 0 || index >= v->wheel_count)
        return 0;
    return v->wheels[index].in_contact;
}

/// @brief `Vehicle3D.WheelTravel(i)` — current suspension length of wheel @p i
///   (rest length when airborne; smaller when compressed).
/// @param obj Borrowed `Vehicle3D` handle.
/// @param index Zero-based wheel index.
/// @return Current suspension length in world units, or zero for an invalid handle/index.
double rt_vehicle3d_wheel_travel(void *obj, int64_t index) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (!v || index < 0 || index >= v->wheel_count)
        return 0.0;
    return v->wheels[index].travel;
}

/// @brief `Vehicle3D.WheelLoad(i)` — suspension force on wheel @p i last Step (N).
/// @param obj Borrowed `Vehicle3D` handle.
/// @param index Zero-based wheel index.
/// @return Non-negative spring-plus-damper load in newtons, or zero for an airborne or invalid
///         wheel.
double rt_vehicle3d_wheel_load(void *obj, int64_t index) {
    rt_vehicle3d *v = vehicle3d_checked(obj);
    if (!v || index < 0 || index >= v->wheel_count)
        return 0.0;
    return v->wheels[index].contact_load;
}

#else
typedef int rt_vehicle3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
