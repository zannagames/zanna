//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_3d_cloth_stubs.c
// Purpose: Graphics-disabled Cloth3D runtime entry points.
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful cloth operations follow the shared unavailable-backend policy.
// Ownership/Lifetime: Stub entry points allocate no simulation resources or retained handles.
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//        src/runtime/graphics/common/rt_3d_physics_stubs.c
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* Cloth3D stubs (plan 27) */

/// @brief Stub for `Cloth3D.NewChain` — would allocate a verlet chain.
///
/// Traps: graphics support not compiled in.
///
/// @param segments     Number of simulated chain segments (ignored before trapping).
/// @param total_length End-to-end chain length (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_cloth3d_new_chain(int64_t segments, double total_length) {
    (void)segments;
    (void)total_length;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Cloth3D.NewChain: graphics support not compiled in", NULL);
}

/// @brief Stub for `Cloth3D.NewPatch` — would allocate a verlet patch grid.
///
/// Traps: graphics support not compiled in.
///
/// @param w      Number of grid points or divisions along the patch width (ignored).
/// @param h      Number of grid points or divisions along the patch height (ignored).
/// @param width  Physical patch width (ignored).
/// @param height Physical patch height (ignored).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_cloth3d_new_patch(int64_t w, int64_t h, double width, double height) {
    (void)w;
    (void)h;
    (void)width;
    (void)height;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Cloth3D.NewPatch: graphics support not compiled in", NULL);
}

/// @brief Stub for `Cloth3D.get_Damping`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_damping(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_Damping`. Silent no-op stub.
///
/// @param cloth   Cloth3D handle (ignored).
/// @param damping Velocity damping coefficient (ignored).
void rt_cloth3d_set_damping(void *cloth, double damping) {
    (void)cloth;
    (void)damping;
}

/// @brief Stub for `Cloth3D.get_Iterations`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0`.
int64_t rt_cloth3d_get_iterations(void *cloth) {
    (void)cloth;
    return 0;
}

/// @brief Stub for `Cloth3D.set_Iterations`. Silent no-op stub.
///
/// @param cloth      Cloth3D handle (ignored).
/// @param iterations Constraint-solver iteration count (ignored).
void rt_cloth3d_set_iterations(void *cloth, int64_t iterations) {
    (void)cloth;
    (void)iterations;
}

/// @brief Stub for `Cloth3D.get_GravityScale`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_gravity_scale(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_GravityScale`. Silent no-op stub.
///
/// @param cloth Cloth3D handle (ignored).
/// @param scale Gravity multiplier (ignored).
void rt_cloth3d_set_gravity_scale(void *cloth, double scale) {
    (void)cloth;
    (void)scale;
}

/// @brief Stub for `Cloth3D.get_WindResponse`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0.0`.
double rt_cloth3d_get_wind_response(void *cloth) {
    (void)cloth;
    return 0.0;
}

/// @brief Stub for `Cloth3D.set_WindResponse`. Silent no-op stub.
///
/// @param cloth    Cloth3D handle (ignored).
/// @param response Wind-force response coefficient (ignored).
void rt_cloth3d_set_wind_response(void *cloth, double response) {
    (void)cloth;
    (void)response;
}

/// @brief Stub for `Cloth3D.get_PointCount`. Silent stub returning `0`.
///
/// @param cloth Cloth3D handle (ignored).
///
/// @return `0`, because the stub contains no simulated points.
int64_t rt_cloth3d_get_point_count(void *cloth) {
    (void)cloth;
    return 0;
}

/// @brief Stub for `Cloth3D.Pin`. Silent no-op stub returning the handle.
///
/// @param cloth Cloth3D handle.
/// @param index Point index to pin (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_pin(void *cloth, int64_t index) {
    (void)index;
    return cloth;
}

/// @brief Stub for `Cloth3D.AddSphere`. Silent no-op stub returning the handle.
///
/// @param cloth  Cloth3D handle.
/// @param center Vec3 collision-sphere center (ignored).
/// @param radius Collision-sphere radius (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_add_sphere(void *cloth, void *center, double radius) {
    (void)center;
    (void)radius;
    return cloth;
}

/// @brief Stub for `Cloth3D.AddCapsule`. Silent no-op stub returning the handle.
///
/// @param cloth  Cloth3D handle.
/// @param a      Vec3 capsule-axis start (ignored).
/// @param b      Vec3 capsule-axis end (ignored).
/// @param radius Capsule radius (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_add_capsule(void *cloth, void *a, void *b, double radius) {
    (void)a;
    (void)b;
    (void)radius;
    return cloth;
}

/// @brief Stub for `Cloth3D.SetWind`. Silent no-op stub.
///
/// @param cloth     Cloth3D handle (ignored).
/// @param direction Vec3 wind direction (ignored).
/// @param strength  Wind magnitude (ignored).
void rt_cloth3d_set_wind(void *cloth, void *direction, double strength) {
    (void)cloth;
    (void)direction;
    (void)strength;
}

/// @brief Stub for `Cloth3D.GetPoint`. Silent stub returning `NULL`.
///
/// @param cloth Cloth3D handle (ignored).
/// @param index Simulated point index (ignored).
///
/// @return `NULL`.
void *rt_cloth3d_get_point(void *cloth, int64_t index) {
    (void)cloth;
    (void)index;
    return NULL;
}

/// @brief Stub for `Cloth3D.BindMesh`. Silent no-op stub returning the handle.
///
/// @param cloth Cloth3D handle.
/// @param mesh  Mesh3D handle to deform (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_bind_mesh(void *cloth, void *mesh) {
    (void)mesh;
    return cloth;
}

/// @brief Stub for `Cloth3D.BindBoneChain`. Silent no-op stub returning the handle.
///
/// @param cloth     Cloth3D handle.
/// @param animator  NodeAnimator3D or animation-controller handle (ignored).
/// @param root_bone Name of the simulated chain's root bone (ignored).
///
/// @return `cloth` unchanged.
void *rt_cloth3d_bind_bone_chain(void *cloth, void *animator, rt_string root_bone) {
    (void)animator;
    (void)root_bone;
    return cloth;
}

/// @brief Stub for `Cloth3D.Step`. Silent no-op stub.
///
/// @param cloth Cloth3D handle (ignored).
/// @param dt    Simulation interval in seconds (ignored).
void rt_cloth3d_step(void *cloth, double dt) {
    (void)cloth;
    (void)dt;
}

/// @brief Stub for `World3D.AddCloth`. Silent no-op stub.
///
/// @param world World3D handle (ignored).
/// @param cloth Cloth3D handle to add (ignored).
void rt_game3d_world_add_cloth(void *world, void *cloth) {
    (void)world;
    (void)cloth;
}

/// @brief Stub for `World3D.RemoveCloth`. Silent no-op stub.
///
/// @param world World3D handle (ignored).
/// @param cloth Cloth3D handle to remove (ignored).
void rt_game3d_world_remove_cloth(void *world, void *cloth) {
    (void)world;
    (void)cloth;
}

/* Physics3DWorld query-config, CCD counters, and traversal probes */

/// @brief Stub for `Physics3DWorld.get_MaxQueryHits`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, the fallback query-result capacity.
int64_t rt_world3d_get_max_query_hits(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.SetMaxQueryHits`. Silent no-op stub.
///
/// @param w        Physics3DWorld handle (ignored).
/// @param max_hits Maximum results to retain per query (ignored).
void rt_world3d_set_max_query_hits(void *w, int64_t max_hits) {
    (void)w;
    (void)max_hits;
}

/// @brief Stub for `Physics3DWorld.get_CcdToiCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no time-of-impact events were processed.
int64_t rt_world3d_get_ccd_toi_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.get_CcdSubstepClampedBodyCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because no bodies reached the CCD substep limit.
int64_t rt_world3d_get_ccd_substep_clamped_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.get_LastCcdClampedBodyCount`. Silent stub returning 0.
///
/// @param w Physics3DWorld handle (ignored).
///
/// @return `0`, because the most recent nonexistent step clamped no bodies.
int64_t rt_world3d_get_last_ccd_clamped_body_count(void *w) {
    (void)w;
    return 0;
}

/// @brief Stub for `Physics3DWorld.ProbeClearance`. Silent stub returning false.
///
/// @param w        Physics3DWorld handle (ignored).
/// @param position Vec3 capsule or character position (ignored).
/// @param radius   Probe radius (ignored).
/// @param height   Required clearance height (ignored).
/// @param mask     Collision-layer mask (ignored).
///
/// @return `0`, indicating that clearance was not established.
int8_t rt_world3d_probe_clearance(
    void *w, void *position, double radius, double height, int64_t mask) {
    (void)w;
    (void)position;
    (void)radius;
    (void)height;
    (void)mask;
    return 0;
}

/// @brief Stub for `Physics3DWorld.ProbeLedge`. Silent stub returning NULL (no ledge).
///
/// @param w          Physics3DWorld handle (ignored).
/// @param position   Vec3 probe origin (ignored).
/// @param forward    Vec3 horizontal probe direction (ignored).
/// @param max_height Maximum ledge height above the origin (ignored).
/// @param min_height Minimum ledge height above the origin (ignored).
/// @param reach      Forward probe distance (ignored).
/// @param mask       Collision-layer mask (ignored).
///
/// @return `NULL`, indicating that no ledge was found.
void *rt_world3d_probe_ledge(void *w,
                             void *position,
                             void *forward,
                             double max_height,
                             double min_height,
                             double reach,
                             int64_t mask) {
    (void)w;
    (void)position;
    (void)forward;
    (void)max_height;
    (void)min_height;
    (void)reach;
    (void)mask;
    return NULL;
}

/// @brief Stub for `Physics3DWorld.ProbeVault`. Silent stub returning NULL (no vault).
///
/// @param w          Physics3DWorld handle (ignored).
/// @param position   Vec3 probe origin (ignored).
/// @param forward    Vec3 horizontal probe direction (ignored).
/// @param max_height Maximum vaultable obstacle height (ignored).
/// @param max_depth  Maximum vaultable obstacle depth (ignored).
/// @param reach      Forward probe distance (ignored).
/// @param mask       Collision-layer mask (ignored).
///
/// @return `NULL`, indicating that no vault opportunity was found.
void *rt_world3d_probe_vault(void *w,
                             void *position,
                             void *forward,
                             double max_height,
                             double max_depth,
                             double reach,
                             int64_t mask) {
    (void)w;
    (void)position;
    (void)forward;
    (void)max_height;
    (void)max_depth;
    (void)reach;
    (void)mask;
    return NULL;
}

/* Character3D configuration and state accessors */

/// @brief Stub for `Character3D.TrySetHeight`. Silent stub returning false.
///
/// @param c      Character3D handle (ignored).
/// @param height Requested capsule height (ignored).
///
/// @return `0`, indicating that the resize could not be applied.
int8_t rt_character3d_try_set_height(void *c, double height) {
    (void)c;
    (void)height;
    return 0;
}

/// @brief Stub for `Character3D.set_Height`. Silent no-op stub.
///
/// @param c      Character3D handle (ignored).
/// @param height Requested capsule height (ignored).
void rt_character3d_set_height(void *c, double height) {
    (void)c;
    (void)height;
}

/// @brief Stub for `Character3D.get_Height`. Silent stub returning 0.
///
/// @param c Character3D handle (ignored).
///
/// @return `0.0`.
double rt_character3d_get_height(void *c) {
    (void)c;
    return 0.0;
}

/// @brief Stub for `Character3D.set_PushStrength`. Silent no-op stub.
///
/// @param c        Character3D handle (ignored).
/// @param strength Impulse or force multiplier for pushed dynamic bodies (ignored).
void rt_character3d_set_push_strength(void *c, double strength) {
    (void)c;
    (void)strength;
}

/// @brief Stub for `Character3D.get_PushStrength`. Silent stub returning 0.
///
/// @param c Character3D handle (ignored).
///
/// @return `0.0`.
double rt_character3d_get_push_strength(void *c) {
    (void)c;
    return 0.0;
}

/// @brief Stub for `Character3D.set_CollideDynamic`. Silent no-op stub.
///
/// @param c       Character3D handle (ignored).
/// @param enabled Non-zero to collide with dynamic rigid bodies (ignored).
void rt_character3d_set_collide_dynamic(void *c, int8_t enabled) {
    (void)c;
    (void)enabled;
}

/// @brief Stub for `Character3D.get_CollideDynamic`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_get_collide_dynamic(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.set_RidePlatforms`. Silent no-op stub.
///
/// @param c       Character3D handle (ignored).
/// @param enabled Non-zero to inherit moving-platform displacement (ignored).
void rt_character3d_set_ride_platforms(void *c, int8_t enabled) {
    (void)c;
    (void)enabled;
}

/// @brief Stub for `Character3D.get_RidePlatforms`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`.
int8_t rt_character3d_get_ride_platforms(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.IsSliding`. Silent stub returning false.
///
/// @param c Character3D handle (ignored).
///
/// @return `0`, indicating that the character is not sliding.
int8_t rt_character3d_is_sliding(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `Character3D.GetGroundBody`. Silent stub returning NULL.
///
/// @param c Character3D handle (ignored).
///
/// @return `NULL`.
void *rt_character3d_get_ground_body(void *c) {
    (void)c;
    return NULL;
}

/* Ragdoll3D */

/// @brief Stub for `Ragdoll3D.New`. Silent stub returning NULL.
///
/// @param skeleton Skeleton3D handle used to derive bodies and joints (ignored).
///
/// @return `NULL`.
void *rt_ragdoll3d_from_skeleton(void *skeleton) {
    (void)skeleton;
    return NULL;
}

/// @brief Stub for `Ragdoll3D.get_TotalMass`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_total_mass(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_TotalMass`. Silent no-op stub.
///
/// @param r    Ragdoll3D handle (ignored).
/// @param mass Requested aggregate ragdoll mass (ignored).
void rt_ragdoll3d_set_total_mass(void *r, double mass) {
    (void)r;
    (void)mass;
}

/// @brief Stub for `Ragdoll3D.get_RadiusScale`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_radius_scale(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_RadiusScale`. Silent no-op stub.
///
/// @param r     Ragdoll3D handle (ignored).
/// @param scale Collider-radius multiplier (ignored).
void rt_ragdoll3d_set_radius_scale(void *r, double scale) {
    (void)r;
    (void)scale;
}

/// @brief Stub for `Ragdoll3D.get_MinBoneLength`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0.0`.
double rt_ragdoll3d_get_min_bone_length(void *r) {
    (void)r;
    return 0.0;
}

/// @brief Stub for `Ragdoll3D.set_MinBoneLength`. Silent no-op stub.
///
/// @param r      Ragdoll3D handle (ignored).
/// @param length Minimum skeleton-bone length eligible for a rigid body (ignored).
void rt_ragdoll3d_set_min_bone_length(void *r, double length) {
    (void)r;
    (void)length;
}

/// @brief Stub for `Ragdoll3D.get_BodyCount`. Silent stub returning 0.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0`.
int64_t rt_ragdoll3d_get_body_count(void *r) {
    (void)r;
    return 0;
}

/// @brief Stub for `Ragdoll3D.get_Active`. Silent stub returning false.
///
/// @param r Ragdoll3D handle (ignored).
///
/// @return `0`.
int8_t rt_ragdoll3d_get_active(void *r) {
    (void)r;
    return 0;
}

/// @brief Stub for `Ragdoll3D.SetJointLimits`. Silent no-op stub.
///
/// @param ragdoll   Ragdoll3D handle (ignored).
/// @param bone_name Skeleton bone whose joint limits should change (ignored).
/// @param swing_deg Maximum swing angle in degrees (ignored).
/// @param twist_deg Maximum twist angle in degrees (ignored).
void rt_ragdoll3d_set_joint_limits(void *ragdoll,
                                   rt_string bone_name,
                                   double swing_deg,
                                   double twist_deg) {
    (void)ragdoll;
    (void)bone_name;
    (void)swing_deg;
    (void)twist_deg;
}

/// @brief Stub for `Ragdoll3D.Activate`. Silent no-op stub.
///
/// @param r        Ragdoll3D handle (ignored).
/// @param world    Physics3DWorld that would receive the ragdoll bodies (ignored).
/// @param skeleton Skeleton3D pose source (ignored).
/// @param node     Scene node whose transform would anchor the ragdoll (ignored).
void rt_ragdoll3d_activate(void *r, void *world, void *skeleton, void *node) {
    (void)r;
    (void)world;
    (void)skeleton;
    (void)node;
}

/// @brief Stub for `Ragdoll3D.Deactivate`. Silent no-op stub.
///
/// @param r             Ragdoll3D handle (ignored).
/// @param blend_seconds Duration of the blend back to animation (ignored).
void rt_ragdoll3d_deactivate(void *r, double blend_seconds) {
    (void)r;
    (void)blend_seconds;
}

/// @brief Stub for `Ragdoll3D.SetPowered`. Silent no-op stub.
///
/// @param r        Ragdoll3D handle (ignored).
/// @param enabled  Non-zero to enable powered pose matching (ignored).
/// @param strength Pose-matching drive strength (ignored).
void rt_ragdoll3d_set_powered(void *r, int64_t enabled, double strength) {
    (void)r;
    (void)enabled;
    (void)strength;
}

/// @brief Stub for `Ragdoll3D.Step`. Silent no-op stub.
///
/// @param r  Ragdoll3D handle (ignored).
/// @param dt Simulation interval in seconds (ignored).
void rt_ragdoll3d_step(void *r, double dt) {
    (void)r;
    (void)dt;
}

/// @brief Stub for `Ragdoll3D.GetBody`. Silent stub returning NULL.
///
/// @param ragdoll   Ragdoll3D handle (ignored).
/// @param bone_name Skeleton bone whose generated body is requested (ignored).
///
/// @return `NULL`.
void *rt_ragdoll3d_get_body(void *ragdoll, rt_string bone_name) {
    (void)ragdoll;
    (void)bone_name;
    return NULL;
}

/// @brief Stub for the C-internal closest-hit raw raycast. Silent stub returning NULL.
///
/// Writes `-1.0` to `out_distance` when the output pointer is non-NULL.
///
/// @param world        Physics3DWorld handle (ignored).
/// @param ox           Ray-origin X coordinate (ignored).
/// @param oy           Ray-origin Y coordinate (ignored).
/// @param oz           Ray-origin Z coordinate (ignored).
/// @param dx           Ray-direction X component (ignored).
/// @param dy           Ray-direction Y component (ignored).
/// @param dz           Ray-direction Z component (ignored).
/// @param max_distance Maximum ray distance (ignored).
/// @param mask         Collision-layer mask (ignored).
/// @param ignore_body  Optional Body3D handle to exclude (ignored).
/// @param out_distance Optional output receiving `-1.0`.
///
/// @return `NULL`, indicating that no body was hit.
void *rt_world3d_raycast_closest_body_raw(void *world,
                                          double ox,
                                          double oy,
                                          double oz,
                                          double dx,
                                          double dy,
                                          double dz,
                                          double max_distance,
                                          int64_t mask,
                                          const void *ignore_body,
                                          double *out_distance) {
    (void)world;
    (void)ox;
    (void)oy;
    (void)oz;
    (void)dx;
    (void)dy;
    (void)dz;
    (void)max_distance;
    (void)mask;
    (void)ignore_body;
    if (out_distance)
        *out_distance = -1.0;
    return NULL;
}

/* Vehicle3D */

/// @brief Stub for `Vehicle3D.New`. Silent stub returning NULL.
///
/// @param world   Physics3DWorld that would simulate the vehicle (ignored).
/// @param chassis Body3D chassis handle (ignored).
///
/// @return `NULL`.
void *rt_vehicle3d_new(void *world, void *chassis) {
    (void)world;
    (void)chassis;
    return NULL;
}

/// @brief Stub for `Vehicle3D.AddWheel`. Silent stub returning -1.
///
/// @param vehicle         Vehicle3D handle (ignored).
/// @param x               Wheel connection X coordinate in chassis space (ignored).
/// @param y               Wheel connection Y coordinate in chassis space (ignored).
/// @param z               Wheel connection Z coordinate in chassis space (ignored).
/// @param radius          Wheel radius (ignored).
/// @param suspension_rest Suspension rest length (ignored).
/// @param stiffness       Suspension spring stiffness (ignored).
/// @param damping         Suspension damping coefficient (ignored).
/// @param steers          Non-zero if steering input rotates this wheel (ignored).
/// @param driven          Non-zero if drive force is applied to this wheel (ignored).
///
/// @return `-1`, indicating that no wheel was added.
int64_t rt_vehicle3d_add_wheel(void *vehicle,
                               double x,
                               double y,
                               double z,
                               double radius,
                               double suspension_rest,
                               double stiffness,
                               double damping,
                               int8_t steers,
                               int8_t driven) {
    (void)vehicle;
    (void)x;
    (void)y;
    (void)z;
    (void)radius;
    (void)suspension_rest;
    (void)stiffness;
    (void)damping;
    (void)steers;
    (void)driven;
    return -1;
}

/// @brief Stub for `Vehicle3D.SetInput`. Silent no-op stub.
///
/// @param vehicle  Vehicle3D handle (ignored).
/// @param throttle Normalized throttle input (ignored).
/// @param brake    Normalized brake input (ignored).
/// @param steer    Normalized steering input (ignored).
void rt_vehicle3d_set_input(void *vehicle, double throttle, double brake, double steer) {
    (void)vehicle;
    (void)throttle;
    (void)brake;
    (void)steer;
}

/// @brief Stub for `Vehicle3D.SetDriveForce`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param newtons Maximum drive force in newtons (ignored).
void rt_vehicle3d_set_drive_force(void *vehicle, double newtons) {
    (void)vehicle;
    (void)newtons;
}

/// @brief Stub for `Vehicle3D.SetBrakeForce`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param newtons Maximum brake force in newtons (ignored).
void rt_vehicle3d_set_brake_force(void *vehicle, double newtons) {
    (void)vehicle;
    (void)newtons;
}

/// @brief Stub for `Vehicle3D.SetMaxSteer`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param degrees Maximum steering angle in degrees (ignored).
void rt_vehicle3d_set_max_steer(void *vehicle, double degrees) {
    (void)vehicle;
    (void)degrees;
}

/// @brief Stub for `Vehicle3D.SetGrip`. Silent no-op stub.
///
/// @param vehicle      Vehicle3D handle (ignored).
/// @param longitudinal Longitudinal tire-grip coefficient (ignored).
/// @param lateral      Lateral tire-grip coefficient (ignored).
void rt_vehicle3d_set_grip(void *vehicle, double longitudinal, double lateral) {
    (void)vehicle;
    (void)longitudinal;
    (void)lateral;
}

/// @brief Stub for `Vehicle3D.SetCollisionMask`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param mask    Collision-layer mask used by wheel queries (ignored).
void rt_vehicle3d_set_collision_mask(void *vehicle, int64_t mask) {
    (void)vehicle;
    (void)mask;
}

/// @brief Stub for `Vehicle3D.Step`. Silent no-op stub.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param dt      Simulation interval in seconds (ignored).
void rt_vehicle3d_step(void *vehicle, double dt) {
    (void)vehicle;
    (void)dt;
}

/// @brief Stub for `Vehicle3D.get_Speed`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_get_speed(void *vehicle) {
    (void)vehicle;
    return 0.0;
}

/// @brief Stub for `Vehicle3D.get_WheelCount`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
///
/// @return `0`.
int64_t rt_vehicle3d_get_wheel_count(void *vehicle) {
    (void)vehicle;
    return 0;
}

/// @brief Stub for `Vehicle3D.WheelInContact`. Silent stub returning false.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0`, indicating that the wheel has no ground contact.
int8_t rt_vehicle3d_wheel_in_contact(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0;
}

/// @brief Stub for `Vehicle3D.WheelTravel`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_wheel_travel(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0.0;
}

/// @brief Stub for `Vehicle3D.WheelLoad`. Silent stub returning 0.
///
/// @param vehicle Vehicle3D handle (ignored).
/// @param index   Wheel index (ignored).
///
/// @return `0.0`.
double rt_vehicle3d_wheel_load(void *vehicle, int64_t index) {
    (void)vehicle;
    (void)index;
    return 0.0;
}


