//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_light3d.c
// Purpose: Zanna.Graphics3D.Light3D directional, point, ambient, spot,
//   area, and volume light state.
//
// Key invariants:
//   - Light types: 0=directional, 1=point, 2=ambient, 3=spot,
//     4=rectangle area, 5=sphere area, and 6=volume.
//   - Up to VGFX3D_MAX_LIGHTS (64) per Canvas3D, set via SetLight(canvas, slot, light).
//     The fixed-forward path shades the first 16; the clustered path shades all 64
//     (drops observable via Canvas3D.DroppedLightCount / ClusterOverflowCount).
//   - Default intensity=1.0; local lights use finite non-negative range,
//     attenuation, decay, shape, and orientation parameters.
//   - Direction/position are borrowed Vec3 values, copied at creation time.
//   - Spot cone angles are stored as cosines for cheap shader comparison.
//
// Ownership/Lifetime:
//   - Light3D is GC-managed; no finalizer needed (no owned heap allocations).
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h,
//   docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements finite mutable state for all Light3D emitter types.
/// @details Constructors copy and sanitize authoring inputs, mutations advance
///   a process-wide renderer cache generation, and type-aware accessors expose
///   directional, local, area, volume, shadow, and spot-cone properties.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define LIGHT3D_WORLD_ABS_MAX 1000000000000.0
#define LIGHT3D_PARAM_MAX 1000000.0

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
#include "rt_trap.h"
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);

/// @brief Validate @p obj as a Light3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object.
/// @return Borrowed Light3D implementation, or `NULL` for an invalid handle.
static rt_light3d *light3d_checked(void *obj) {
    return (rt_light3d *)rt_g3d_checked_or_null(obj, RT_G3D_LIGHT3D_CLASS_ID);
}

/// @brief Monotonic generation stamp covering every Light3D mutation in the process.
/// @details Canvas3D flattens its slotted lights into the dense backend array at most once
///   per generation per frame instead of once per queued draw. Starts at 1 so a zeroed
///   canvas cache field can never alias a real generation. Main-thread only, like all
///   Light3D mutation entry points.
static uint64_t g_light3d_mutation_revision = 1;

/// @brief Current Light3D mutation generation (see g_light3d_mutation_revision).
/// @return Current nonzero process-wide revision. Calls do not advance it.
uint64_t rt_light3d_mutation_revision(void) {
    return g_light3d_mutation_revision;
}

/// @brief Advance the Light3D mutation generation after any observable light change.
static void light3d_note_mutation(void) {
    g_light3d_mutation_revision++;
    if (g_light3d_mutation_revision == 0)
        g_light3d_mutation_revision = 1;
}

/// @brief Clamp a value at zero from below — negatives collapse to zero.
/// @details Used for physical quantities like intensity, radius, and
///   attenuation where negative values are nonsensical. Chosen over a
///   `fmax(0, value)` call to keep the hot path branch-predictable on
///   inputs that are almost always non-negative.
/// @param value Candidate physical scalar.
/// @return Finite nonnegative value, with negative or nonfinite input mapped to zero.
static double clamp_min0(double value) {
    if (!isfinite(value))
        return 0.0;
    return value < 0.0 ? 0.0 : value;
}

/// @brief Clamp a non-negative light parameter to a stable finite range.
/// @param value Candidate physical scalar.
/// @return Value clamped to `[0, LIGHT3D_PARAM_MAX]`.
static double clamp_param_min0(double value) {
    value = clamp_min0(value);
    return value > LIGHT3D_PARAM_MAX ? LIGHT3D_PARAM_MAX : value;
}

/// @brief Sanitize a local-light attenuation value, enforcing a small non-zero falloff floor.
/// @details Without this floor, point and spot lights with exactly zero attenuation would
/// illuminate
///          the whole scene at full strength, which is both visually surprising and expensive when
///          many local lights are active. The floor preserves wide-radius authoring while
///          preventing accidental no-falloff lights; non-finite or negative values fall back to the
///          same default.
/// @param value Candidate attenuation factor.
/// @return Finite attenuation in
///   `[RT_LIGHT3D_DEFAULT_ATTENUATION, LIGHT3D_PARAM_MAX]`.
static double sanitize_local_attenuation(double value) {
    value = clamp_param_min0(value);
    return value < RT_LIGHT3D_DEFAULT_ATTENUATION ? RT_LIGHT3D_DEFAULT_ATTENUATION : value;
}

/// @brief Clamp a value to the [0, 1] range, mapping NaN/inf to 0.
/// @details Used for RGB color components, which the runtime treats as linear [0, 1]
///   scalars. Anything NaN-poisoned would produce black pixels downstream, so sanitising
///   at the boundary keeps the material pipeline free of infectious non-finite values.
/// @param value Candidate color channel.
/// @return Finite channel clamped to `[0, 1]`; nonfinite input becomes zero.
static double clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Return `value` if finite, else 0. Boundary cleanup for positions/directions.
/// @param value Candidate coordinate or direction component.
/// @return @p value when finite, otherwise zero.
static double finite_or_zero(double value) {
    return isfinite(value) ? value : 0.0;
}

/// @brief Clamp a world-space light coordinate to a stable finite range.
/// @param value Candidate coordinate.
/// @return Finite value clamped to the supported symmetric world bound.
static double light_coord_or_zero(double value) {
    value = finite_or_zero(value);
    if (value > LIGHT3D_WORLD_ABS_MAX)
        return LIGHT3D_WORLD_ABS_MAX;
    if (value < -LIGHT3D_WORLD_ABS_MAX)
        return -LIGHT3D_WORLD_ABS_MAX;
    return value;
}

/// @brief Clamp a spot-light cone angle to [0°, 89°], substituting `fallback` when NaN.
/// @details Spot lights convert angles to cosines in the shader; letting the half-angle
///   reach 90° would produce `cos ≈ 0`, collapsing the cone to a single ray and killing
///   the light. Capping at 89° guarantees a non-degenerate cone while still letting
///   artists author very wide spots.
/// @param value Candidate half-angle in degrees.
/// @param fallback Substitute used when @p value is nonfinite.
/// @return Finite half-angle clamped to `[0, 89]` degrees.
static double sanitize_spot_angle(double value, double fallback) {
    if (!isfinite(value))
        value = fallback;
    if (value < 0.0)
        return 0.0;
    if (value > 89.0)
        return 89.0;
    return value;
}

/// @brief Clamp and mutually-validate inner and outer spot-light cone angles.
/// @details Ensures `inner` < `outer` with at least a 0.01° gap, and both angles
///          are in [0°, 89°]. When outer ≤ inner + eps the outer is widened; when
///          either pointer is NULL, its angle defaults to 0° (inner) or inner+eps (outer).
/// @param inner_angle Optional in/out inner half-angle in degrees.
/// @param outer_angle Optional in/out outer half-angle in degrees.
static void sanitize_spot_angles(double *inner_angle, double *outer_angle) {
    const double eps = 0.01;
    double inner = sanitize_spot_angle(inner_angle ? *inner_angle : 0.0, 0.0);
    double outer = sanitize_spot_angle(outer_angle ? *outer_angle : inner + eps, inner + eps);
    if (outer <= inner + eps) {
        if (inner > 89.0 - eps)
            inner = 89.0 - eps;
        outer = inner + eps;
    }
    if (outer > 89.0) {
        outer = 89.0;
        if (inner >= outer)
            inner = outer - eps;
    }
    if (inner < 0.0)
        inner = 0.0;
    if (inner_angle)
        *inner_angle = inner;
    if (outer_angle)
        *outer_angle = outer;
}

/// @brief Decode one retained spot cone into the public, sanitized degree pair.
/// @details Retained and imported lights store cone cosines. Decode through a
///   clamped domain, then reuse the constructor sanitizer so corrupt legacy
///   payloads cannot surface non-finite or inverted authoring values.
/// @param light Borrowed light; non-spot and null inputs decode to zero angles.
/// @param inner_angle Optional output inner half-angle in degrees.
/// @param outer_angle Optional output outer half-angle in degrees.
static void light3d_get_spot_angles(const rt_light3d *light,
                                    double *inner_angle,
                                    double *outer_angle) {
    double inner = 0.0;
    double outer = 0.0;
    if (light && light->type == 3) {
        const double radians_to_degrees = 57.2957795130823208768;
        double inner_cos = light->inner_cos;
        double outer_cos = light->outer_cos;
        if (!isfinite(inner_cos))
            inner_cos = 1.0;
        if (!isfinite(outer_cos))
            outer_cos = 0.7071067811865475244;
        if (inner_cos < 0.0)
            inner_cos = 0.0;
        if (inner_cos > 1.0)
            inner_cos = 1.0;
        if (outer_cos < 0.0)
            outer_cos = 0.0;
        if (outer_cos > 1.0)
            outer_cos = 1.0;
        inner = acos(inner_cos) * radians_to_degrees;
        outer = acos(outer_cos) * radians_to_degrees;
        sanitize_spot_angles(&inner, &outer);
    }
    if (inner_angle)
        *inner_angle = inner;
    if (outer_angle)
        *outer_angle = outer;
}

/// @brief Normalize a light's direction vector, defaulting to down on zero input.
/// @details When a caller hands in a degenerate (zero-length) direction,
///   rather than producing NaN we fall back to `(0, -1, 0)` — straight-down
///   sun — so subsequent lighting math stays finite. The 1e-8 threshold
///   is permissive enough that any artistically-meaningful input passes,
///   but catches explicit zeroing.
/// @param x In/out direction X component.
/// @param y In/out direction Y component.
/// @param z In/out direction Z component.
static void normalize_light_direction(double *x, double *y, double *z) {
    if (!x || !y || !z)
        return;
    *x = light_coord_or_zero(*x);
    *y = light_coord_or_zero(*y);
    *z = light_coord_or_zero(*z);
    double max_component = fmax(fabs(*x), fmax(fabs(*y), fabs(*z)));
    if (!isfinite(max_component) || max_component <= 1e-8) {
        *x = 0.0;
        *y = -1.0;
        *z = 0.0;
        return;
    }
    double sx = *x / max_component;
    double sy = *y / max_component;
    double sz = *z / max_component;
    double len = sqrt(sx * sx + sy * sy + sz * sz);
    if (!isfinite(len) || len <= 1e-8) {
        *x = 0.0;
        *y = -1.0;
        *z = 0.0;
        return;
    }
    *x = sx / len;
    *y = sy / len;
    *z = sz / len;
}

/// @brief Sanitize a strictly-positive emitter dimension or finite range.
/// @param value Candidate dimension or range.
/// @return Finite value in `(0, LIGHT3D_PARAM_MAX]`, using one for invalid or
///   near-zero input.
static double sanitize_positive_light_param(double value) {
    if (!isfinite(value) || value <= 1e-6)
        return 1.0;
    return value > LIGHT3D_PARAM_MAX ? LIGHT3D_PARAM_MAX : value;
}

/// @brief Build an orthonormal rectangle basis around the light's emission direction.
/// @details Normalizes direction in place, chooses a nonparallel helper axis,
///   and stores deterministic unit U/V axes with degeneracy fallbacks.
/// @param light Rectangle-area light to update; `NULL` is ignored.
static void light3d_build_emitter_basis(rt_light3d *light) {
    double hx;
    double hy;
    double hz;
    double ux;
    double uy;
    double uz;
    double vx;
    double vy;
    double vz;
    double length;
    if (!light)
        return;
    normalize_light_direction(&light->direction[0], &light->direction[1], &light->direction[2]);
    if (fabs(light->direction[1]) < 0.999) {
        hx = 0.0;
        hy = 1.0;
        hz = 0.0;
    } else {
        hx = 1.0;
        hy = 0.0;
        hz = 0.0;
    }
    ux = hy * light->direction[2] - hz * light->direction[1];
    uy = hz * light->direction[0] - hx * light->direction[2];
    uz = hx * light->direction[1] - hy * light->direction[0];
    length = sqrt(ux * ux + uy * uy + uz * uz);
    if (!isfinite(length) || length <= 1e-8) {
        ux = 1.0;
        uy = 0.0;
        uz = 0.0;
    } else {
        ux /= length;
        uy /= length;
        uz /= length;
    }
    vx = light->direction[1] * uz - light->direction[2] * uy;
    vy = light->direction[2] * ux - light->direction[0] * uz;
    vz = light->direction[0] * uy - light->direction[1] * ux;
    length = sqrt(vx * vx + vy * vy + vz * vz);
    if (!isfinite(length) || length <= 1e-8) {
        vx = 0.0;
        vy = 1.0;
        vz = 0.0;
    } else {
        vx /= length;
        vy /= length;
        vz /= length;
    }
    light->basis_u[0] = ux;
    light->basis_u[1] = uy;
    light->basis_u[2] = uz;
    light->basis_v[0] = vx;
    light->basis_v[1] = vy;
    light->basis_v[2] = vz;
}

/// @brief Initialize the shared finite state of a newly allocated light.
/// @param light Writable Light3D allocation; `NULL` is ignored.
/// @param type Numeric light kind in the backend enumeration.
/// @param r Initial linear red channel.
/// @param g Initial linear green channel.
/// @param b Initial linear blue channel.
static void light3d_init_common(rt_light3d *light, int32_t type, double r, double g, double b) {
    if (!light)
        return;
    memset(light, 0, sizeof(*light));
    light->vptr = NULL;
    light->type = type;
    light->color[0] = clamp01(r);
    light->color[1] = clamp01(g);
    light->color[2] = clamp01(b);
    light->intensity = 1.0;
    light->inner_cos = 0.0;
    light->outer_cos = 0.0;
    light->width = 1.0;
    light->height = 1.0;
    light->radius = 1.0;
    light->range = 0.0;
    light->decay_type = 2;
    light->enabled = 1;
}

/// @brief Create a directional light (e.g., sun or moon).
/// @details Directional lights have no position — only a direction vector.
///          All surfaces in the scene receive parallel rays along this direction,
///          making them ideal for outdoor/global illumination. The direction
///          vector is copied from the provided Vec3 at creation time.
/// @param direction Vec3 indicating the light direction (copied, not borrowed).
/// @param r Red color component [0.0–1.0].
/// @param g Green color component [0.0–1.0].
/// @param b Blue color component [0.0–1.0].
/// @return Opaque light handle, or NULL on failure.
void *rt_light3d_new_directional(void *direction, double r, double g, double b) {
    if (!rt_g3d_is_vec3(direction)) {
        rt_trap("Light3D.NewDirectional: direction must be a Vec3");
        return NULL;
    }
    rt_light3d *light =
        (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light) {
        rt_trap("Light3D.NewDirectional: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 0, r, g, b);
    light->direction[0] = rt_vec3_x(direction);
    light->direction[1] = rt_vec3_y(direction);
    light->direction[2] = rt_vec3_z(direction);
    normalize_light_direction(&light->direction[0], &light->direction[1], &light->direction[2]);
    light->position[0] = light->position[1] = light->position[2] = 0.0;
    light->attenuation = 0.0;
    light->casts_shadows = 1;
    return light;
}

/// @brief Create a point light that radiates from a position in all directions.
/// @details Point lights simulate light bulbs, torches, etc. Intensity falls
///          off with distance according to the attenuation factor. Values at or
///          below 0.0 use a small default falloff floor instead of constant brightness.
/// @param position    Vec3 world-space position (copied at creation time).
/// @param r           Red color component [0.0–1.0].
/// @param g           Green color component [0.0–1.0].
/// @param b           Blue color component [0.0–1.0].
/// @param attenuation Distance falloff factor (values <= 0 use the default falloff floor).
/// @return Opaque light handle, or NULL on failure.
void *rt_light3d_new_point(void *position, double r, double g, double b, double attenuation) {
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Light3D.NewPoint: position must be a Vec3");
        return NULL;
    }
    rt_light3d *light =
        (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light) {
        rt_trap("Light3D.NewPoint: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 1, r, g, b);
    light->direction[0] = light->direction[1] = light->direction[2] = 0.0;
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light->attenuation = sanitize_local_attenuation(attenuation);
    light->casts_shadows = 0;
    return light;
}

/// @brief Create an ambient light that illuminates all surfaces uniformly.
/// @details Ambient lights have no direction or position — they apply a flat
///          color contribution to every surface equally. Used to provide a base
///          illumination level so shadowed areas aren't completely black.
/// @param r Red color component [0.0–1.0].
/// @param g Green color component [0.0–1.0].
/// @param b Blue color component [0.0–1.0].
/// @return Opaque light handle, or NULL on failure.
void *rt_light3d_new_ambient(double r, double g, double b) {
    rt_light3d *light =
        (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light) {
        rt_trap("Light3D.NewAmbient: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 2, r, g, b);
    memset(light->direction, 0, sizeof(light->direction));
    memset(light->position, 0, sizeof(light->position));
    light->attenuation = 0.0;
    light->casts_shadows = 0;
    return light;
}

/// @brief Create a spot light (cone-shaped illumination from a position).
/// @details Spot lights combine a position, direction, and two cone angles.
///          Surfaces inside the inner angle receive full intensity; between
///          inner and outer angles intensity falls off smoothly. The angles
///          are converted to cosines at creation time for efficient shader
///          comparison (dot product vs. cosine threshold).
/// @param position    Vec3 world-space position of the light source.
/// @param direction   Vec3 direction the cone points toward.
/// @param r           Red color component [0.0–1.0].
/// @param g           Green color component [0.0–1.0].
/// @param b           Blue color component [0.0–1.0].
/// @param attenuation Distance falloff factor.
/// @param inner_angle Full-brightness cone half-angle in degrees.
/// @param outer_angle Outer cone half-angle in degrees (falloff edge).
/// @return Opaque light handle, or NULL on failure.
void *rt_light3d_new_spot(void *position,
                          void *direction,
                          double r,
                          double g,
                          double b,
                          double attenuation,
                          double inner_angle,
                          double outer_angle) {
    if (!rt_g3d_is_vec3(position) || !rt_g3d_is_vec3(direction)) {
        rt_trap("Light3D.NewSpot: position and direction must be Vec3");
        return NULL;
    }
    rt_light3d *light =
        (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light) {
        rt_trap("Light3D.NewSpot: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 3, r, g, b);
    light->direction[0] = rt_vec3_x(direction);
    light->direction[1] = rt_vec3_y(direction);
    light->direction[2] = rt_vec3_z(direction);
    normalize_light_direction(&light->direction[0], &light->direction[1], &light->direction[2]);
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light->attenuation = sanitize_local_attenuation(attenuation);
    /* Convert angles (degrees) to cosines for shader comparison */
    double pi = 3.14159265358979323846;
    sanitize_spot_angles(&inner_angle, &outer_angle);
    light->inner_cos = cos(inner_angle * pi / 180.0);
    light->outer_cos = cos(outer_angle * pi / 180.0);
    light->casts_shadows = 0;
    return light;
}

/// @brief Allocate a native one-sided rectangle area light.
/// @details Copies position/direction, builds an orthonormal emitter basis,
///   sanitizes positive dimensions/range, and applies the local attenuation floor.
/// @param position Vec3 world-space emitter center, copied on success.
/// @param direction Vec3 one-sided emission direction, normalized on success.
/// @param width Positive emitter width in world units.
/// @param height Positive emitter height in world units.
/// @param r Linear red channel clamped to `[0, 1]`.
/// @param g Linear green channel clamped to `[0, 1]`.
/// @param b Linear blue channel clamped to `[0, 1]`.
/// @param attenuation Distance falloff factor subject to the local-light floor.
/// @param range Positive influence range in world units.
/// @return New GC-managed rectangle light, or `NULL` after reporting invalid
///   vectors or allocation failure.
void *rt_light3d_new_area_rectangle(void *position,
                                    void *direction,
                                    double width,
                                    double height,
                                    double r,
                                    double g,
                                    double b,
                                    double attenuation,
                                    double range) {
    rt_light3d *light;
    if (!rt_g3d_is_vec3(position) || !rt_g3d_is_vec3(direction)) {
        rt_trap("Light3D.AreaRectangle: position and direction must be Vec3");
        return NULL;
    }
    light = (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(*light));
    if (!light) {
        rt_trap("Light3D.AreaRectangle: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 4, r, g, b);
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light->direction[0] = rt_vec3_x(direction);
    light->direction[1] = rt_vec3_y(direction);
    light->direction[2] = rt_vec3_z(direction);
    light3d_build_emitter_basis(light);
    light->width = sanitize_positive_light_param(width);
    light->height = sanitize_positive_light_param(height);
    light->attenuation = sanitize_local_attenuation(attenuation);
    light->range = sanitize_positive_light_param(range);
    return light;
}

/// @brief Allocate a native spherical area light.
/// @param position Vec3 world-space center, copied and sanitized.
/// @param radius Positive emitter radius in world units.
/// @param r Linear red channel clamped to `[0, 1]`.
/// @param g Linear green channel clamped to `[0, 1]`.
/// @param b Linear blue channel clamped to `[0, 1]`.
/// @param range Positive influence range in world units.
/// @return New GC-managed sphere-area light, or `NULL` after reporting invalid
///   position or allocation failure.
void *rt_light3d_new_area_sphere(
    void *position, double radius, double r, double g, double b, double range) {
    rt_light3d *light;
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Light3D.AreaSphere: position must be a Vec3");
        return NULL;
    }
    light = (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(*light));
    if (!light) {
        rt_trap("Light3D.AreaSphere: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 5, r, g, b);
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light->radius = sanitize_positive_light_param(radius);
    light->range = sanitize_positive_light_param(range);
    light->attenuation = RT_LIGHT3D_DEFAULT_ATTENUATION;
    return light;
}

/// @brief Allocate a native isotropic volume light.
/// @param position Vec3 world-space volume center, copied and sanitized.
/// @param radius Positive volume radius in world units.
/// @param r Linear red channel clamped to `[0, 1]`.
/// @param g Linear green channel clamped to `[0, 1]`.
/// @param b Linear blue channel clamped to `[0, 1]`.
/// @param range Positive influence range in world units.
/// @return New GC-managed non-shadow-casting volume light, or `NULL` after
///   reporting invalid position or allocation failure.
void *rt_light3d_new_volume(
    void *position, double radius, double r, double g, double b, double range) {
    rt_light3d *light;
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Light3D.Volume: position must be a Vec3");
        return NULL;
    }
    light = (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(*light));
    if (!light) {
        rt_trap("Light3D.Volume: memory allocation failed");
        return NULL;
    }
    light3d_init_common(light, 6, r, g, b);
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light->radius = sanitize_positive_light_param(radius);
    light->range = sanitize_positive_light_param(range);
    light->attenuation = RT_LIGHT3D_DEFAULT_ATTENUATION;
    light->casts_shadows = 0;
    return light;
}

/// @brief Set the brightness multiplier for a light.
/// @details Scales the light's color contribution. Default is 1.0. Values
///          above 1.0 create over-bright highlights; 0.0 effectively disables
///          the light without removing it from the scene.
/// @param obj       Light handle.
/// @param intensity Brightness multiplier (default 1.0).
void rt_light3d_set_intensity(void *obj, double intensity) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    light->intensity = clamp_param_min0(intensity);
    light3d_note_mutation();
}

/// @brief Set the distance-falloff factor of a point or spot light after creation.
/// @details Applies the same non-zero floor as the constructors so a zero value can
///          never make a local light reach infinitely far. Directional and ambient
///          lights have no distance falloff, so the call is a no-op for them —
///          letting pooled lights be retuned without recreating them.
/// @param obj         Light handle.
/// @param attenuation Distance falloff factor (values <= 0 use the default falloff floor).
void rt_light3d_set_attenuation(void *obj, double attenuation) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    if (light->type != 1 && light->type != 3 && light->type != 4 && light->type != 5)
        return;
    light->attenuation = sanitize_local_attenuation(attenuation);
    light3d_note_mutation();
}

/// @brief Read the light's distance-falloff factor (0 for directional/ambient lights).
/// @param obj Light3D receiver.
/// @return Stored attenuation, or zero for an invalid handle.
double rt_light3d_get_attenuation(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    return light ? light->attenuation : 0.0;
}

/// @brief Change the RGB color of a light after creation.
/// @param obj Light handle.
/// @param r   Red component [0.0–1.0].
/// @param g   Green component [0.0–1.0].
/// @param b   Blue component [0.0–1.0].
void rt_light3d_set_color(void *obj, double r, double g, double b) {
    rt_light3d *l = light3d_checked(obj);
    if (!l)
        return;
    l->color[0] = clamp01(r);
    l->color[1] = clamp01(g);
    l->color[2] = clamp01(b);
    light3d_note_mutation();
}

/// @brief Read the light type enum value.
/// @details Corrupt out-of-range state is repaired to directional type zero.
/// @param obj Light3D receiver.
/// @return Type value in `0..6`, or zero for an invalid handle.
int64_t rt_light3d_get_type(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return 0;
    if (light->type < 0 || light->type > 6)
        light->type = 0;
    return light->type;
}

/// @brief Read the light color as a fresh Vec3.
/// @param obj Light3D receiver.
/// @return New Vec3 containing sanitized color channels, or a new zero Vec3
///   for an invalid receiver; allocation failure follows Vec3 construction semantics.
void *rt_light3d_get_color(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(
        clamp01(light->color[0]), clamp01(light->color[1]), clamp01(light->color[2]));
}

/// @brief Read the light intensity.
/// @param obj Light3D receiver.
/// @return Finite nonnegative intensity capped at the parameter maximum, or zero.
double rt_light3d_get_intensity(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return 0.0;
    return clamp_param_min0(light->intensity);
}

/// @brief Enable or disable a light without removing it from its owning slot.
/// @param obj Light3D receiver; invalid handles are ignored.
/// @param enabled Nonzero to contribute to rendering, zero to disable.
void rt_light3d_set_enabled(void *obj, int8_t enabled) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    light->enabled = enabled ? 1 : 0;
    light3d_note_mutation();
}

/// @brief Return whether this light currently contributes to rendering.
/// @param obj Light3D receiver.
/// @return Normalized `1` or `0`; invalid handles return zero.
int8_t rt_light3d_get_enabled(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return 0;
    return light->enabled ? 1 : 0;
}

/// @brief Toggle whether this light can be selected for shadow-map rendering.
/// @param obj Light3D receiver; invalid handles are ignored.
/// @param enabled Nonzero to request eligibility. Volume lights always store disabled.
void rt_light3d_set_casts_shadows(void *obj, int8_t enabled) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    light->casts_shadows = light->type == 6 ? 0 : (enabled ? 1 : 0);
    light3d_note_mutation();
}

/// @brief Return whether this light is eligible for shadow-map slots.
/// @param obj Light3D receiver.
/// @return Normalized shadow-eligibility flag, or zero for invalid handles.
int8_t rt_light3d_get_casts_shadows(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return 0;
    return light->casts_shadows ? 1 : 0;
}

/// @brief Read the light direction as a fresh Vec3.
/// @param obj Light3D receiver.
/// @return New normalized Vec3, using downward direction for invalid or
///   degenerate state.
void *rt_light3d_get_direction(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return rt_vec3_new(0.0, -1.0, 0.0);
    double x = light->direction[0];
    double y = light->direction[1];
    double z = light->direction[2];
    normalize_light_direction(&x, &y, &z);
    return rt_vec3_new(x, y, z);
}

/// @brief Read the light position as a fresh Vec3.
/// @param obj Light3D receiver.
/// @return New sanitized position Vec3, or a new zero Vec3 for invalid input.
void *rt_light3d_get_position(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(light_coord_or_zero(light->position[0]),
                       light_coord_or_zero(light->position[1]),
                       light_coord_or_zero(light->position[2]));
}

/// @brief Move the light to a new world position.
/// @param obj Light3D receiver; invalid handles are ignored.
/// @param position Vec3 copied and clamped to the supported world range; invalid
///   vector input reports a trap without mutation.
void rt_light3d_set_position(void *obj, void *position) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Light3D.Position: position must be a Vec3");
        return;
    }
    light->position[0] = light_coord_or_zero(rt_vec3_x(position));
    light->position[1] = light_coord_or_zero(rt_vec3_y(position));
    light->position[2] = light_coord_or_zero(rt_vec3_z(position));
    light3d_note_mutation();
}

/// @brief Re-aim the light. The direction is normalized.
/// @details Rectangle lights also rebuild their orthonormal emitter basis.
/// @param obj Light3D receiver; invalid handles are ignored.
/// @param direction Vec3 copied and normalized; invalid vector input reports a
///   trap without mutation.
void rt_light3d_set_direction(void *obj, void *direction) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return;
    if (!rt_g3d_is_vec3(direction)) {
        rt_trap("Light3D.Direction: direction must be a Vec3");
        return;
    }
    light->direction[0] = rt_vec3_x(direction);
    light->direction[1] = rt_vec3_y(direction);
    light->direction[2] = rt_vec3_z(direction);
    normalize_light_direction(&light->direction[0], &light->direction[1], &light->direction[2]);
    if (light->type == 4)
        light3d_build_emitter_basis(light);
    light3d_note_mutation();
}

/// @brief Return a rectangle area's sanitized emitter width.
/// @param obj Light3D receiver.
/// @return Positive width for rectangle lights, otherwise zero.
double rt_light3d_get_width(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    return light && light->type == 4 ? sanitize_positive_light_param(light->width) : 0.0;
}

/// @brief Set a rectangle area's emitter width.
/// @param obj Rectangle Light3D receiver; other or invalid types are ignored.
/// @param width Positive world-unit width; invalid or near-zero values become one.
void rt_light3d_set_width(void *obj, double width) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || light->type != 4)
        return;
    light->width = sanitize_positive_light_param(width);
    light3d_note_mutation();
}

/// @brief Return a rectangle area's sanitized emitter height.
/// @param obj Light3D receiver.
/// @return Positive height for rectangle lights, otherwise zero.
double rt_light3d_get_height(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    return light && light->type == 4 ? sanitize_positive_light_param(light->height) : 0.0;
}

/// @brief Set a rectangle area's emitter height.
/// @param obj Rectangle Light3D receiver; other or invalid types are ignored.
/// @param height Positive world-unit height; invalid or near-zero values become one.
void rt_light3d_set_height(void *obj, double height) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || light->type != 4)
        return;
    light->height = sanitize_positive_light_param(height);
    light3d_note_mutation();
}

/// @brief Return a sphere-area or volume light's sanitized radius.
/// @param obj Light3D receiver.
/// @return Positive radius for supported types, otherwise zero.
double rt_light3d_get_radius(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    return light && (light->type == 5 || light->type == 6)
               ? sanitize_positive_light_param(light->radius)
               : 0.0;
}

/// @brief Set a sphere-area or volume light's radius.
/// @param obj Sphere-area or volume Light3D receiver; other types are ignored.
/// @param radius Positive world-unit radius; invalid or near-zero values become one.
void rt_light3d_set_radius(void *obj, double radius) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || (light->type != 5 && light->type != 6))
        return;
    light->radius = sanitize_positive_light_param(radius);
    light3d_note_mutation();
}

/// @brief Return the light's distance-decay mode.
/// @details Corrupt stored values are repaired to inverse-square mode `2`.
/// @param obj Light3D receiver.
/// @return Decay enumeration in `0..3`, or zero for an invalid handle.
int64_t rt_light3d_get_decay_type(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    if (!light)
        return 0;
    if (light->decay_type < 0 || light->decay_type > 3)
        light->decay_type = 2;
    return light->decay_type;
}

/// @brief Select the light's distance-decay mode.
/// @param obj Light3D receiver; invalid handles are ignored.
/// @param decay_type Enumeration value in `0..3`; other values are ignored.
void rt_light3d_set_decay_type(void *obj, int64_t decay_type) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || decay_type < 0 || decay_type > 3)
        return;
    light->decay_type = (int32_t)decay_type;
    light3d_note_mutation();
}

/// @brief Return an area or volume light's sanitized influence range.
/// @param obj Light3D receiver.
/// @return Positive range for types `4..6`, otherwise zero.
double rt_light3d_get_range(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    return light && light->type >= 4 && light->type <= 6
               ? sanitize_positive_light_param(light->range)
               : 0.0;
}

/// @brief Set an area or volume light's influence range.
/// @param obj Area or volume Light3D receiver; other types are ignored.
/// @param range Positive world-unit range; invalid or near-zero values become one.
void rt_light3d_set_range(void *obj, double range) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || light->type < 4 || light->type > 6)
        return;
    light->range = sanitize_positive_light_param(range);
    light3d_note_mutation();
}

/// @brief Return the sanitized inner spot-cone angle in degrees.
/// @param obj Spot Light3D receiver.
/// @return Zero for null, invalid, or non-spot lights.
double rt_light3d_get_inner_cone_degrees(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    double inner = 0.0;
    light3d_get_spot_angles(light, &inner, NULL);
    return inner;
}

/// @brief Return the sanitized outer spot-cone angle in degrees.
/// @param obj Spot Light3D receiver.
/// @return Zero for null, invalid, or non-spot lights.
double rt_light3d_get_outer_cone_degrees(void *obj) {
    rt_light3d *light = light3d_checked(obj);
    double outer = 0.0;
    light3d_get_spot_angles(light, NULL, &outer);
    return outer;
}

/// @brief Atomically replace both spot-cone angles using constructor semantics.
/// @details Non-spot lights are left unchanged. One mutation generation covers
///   the complete paired update so renderer snapshots cannot observe a
///   half-updated cone.
/// @param obj Spot Light3D receiver; other or invalid types are ignored.
/// @param inner_angle Full-intensity cone half-angle in degrees.
/// @param outer_angle Falloff-edge cone half-angle in degrees; sanitizer
///   enforces a minimum `0.01` degree gap above the inner angle.
void rt_light3d_set_spot_cone(void *obj, double inner_angle, double outer_angle) {
    rt_light3d *light = light3d_checked(obj);
    if (!light || light->type != 3)
        return;
    const double degrees_to_radians = 0.01745329251994329577;
    sanitize_spot_angles(&inner_angle, &outer_angle);
    light->inner_cos = cos(inner_angle * degrees_to_radians);
    light->outer_cos = cos(outer_angle * degrees_to_radians);
    light3d_note_mutation();
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
