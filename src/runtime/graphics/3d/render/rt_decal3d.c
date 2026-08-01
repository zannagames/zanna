//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_decal3d.c
// Purpose: 3D decals — surface-aligned textured quads with lifetime/fade.
//
// Key invariants:
//   - Quad is built from position + normal using arbitrary tangent frame.
//   - A size-aware normal offset of at most 0.01 reduces surface z-fighting.
//   - Alpha fades linearly over last 20% of lifetime.
//   - Mesh + material are lazily constructed on first draw and reused.
//
// Ownership/Lifetime:
//   - Decal3D is GC-managed; finalizer releases texture, mesh, and material.
//   - Texture is retained on construction; lazy mesh / material are owned.
//
// Links: rt_decal3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements GC-managed, fading, surface-aligned 3D decals.
/// @details Decals sanitize placement state, lazily construct an unlit textured
///   quad and material, update lifetime alpha, support floating-origin rebasing,
///   and safely own their texture and generated runtime resources.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_decal3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DECAL3D_SIZE_MAX 1000000.0
#define DECAL3D_WORLD_ABS_MAX 1000000000000.0
#define DECAL3D_LIFETIME_MAX 1000000.0
#define DECAL3D_DT_MAX DECAL3D_LIFETIME_MAX

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *p);
extern void rt_obj_free(void *p);
#include "rt_trap.h"
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_add_vertex(
    void *m, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *m, int64_t v0, int64_t v1, int64_t v2);
extern void rt_canvas3d_draw_mesh(void *canvas, void *mesh, void *transform, void *material);
extern void rt_canvas3d_draw_mesh_matrix(void *canvas,
                                         void *mesh,
                                         const double *transform,
                                         void *material);
extern void *rt_material3d_new(void);
extern void rt_material3d_set_texture(void *m, void *tex);
extern void rt_material3d_set_alpha(void *m, double a);
extern void rt_material3d_set_unlit(void *m, int8_t u);
extern void rt_material3d_set_depth_bias(void *m, double constant_bias, double slope_scaled_bias);

typedef struct {
    void *vptr;
    double position[3];
    double normal[3];
    double size;
    void *texture;
    double lifetime;
    double max_lifetime;
    double alpha;
    double depth_bias;      /* constant depth bias override; 0 = auto (size-scaled) */
    void *mesh;             /* built on first draw */
    void *material;         /* built on first draw */
    void *material_texture; /* unowned source identity used to invalidate cached material */
} rt_decal3d;

/// @brief Idempotent release of a GC-managed reference held in `**slot`.
/// @details Safe on already-null slots; zeroes the slot after the
///          release so the finalizer can run twice without double-free
///          (would never happen under normal GC but defensive anyway).
/// @param slot Address of an owned runtime-reference slot to release and clear.
static void decal3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return @p value when finite, else @p fallback.
/// @param value Candidate scalar.
/// @param fallback Substitute for NaN or infinity.
/// @return Finite candidate or the supplied fallback.
static double decal3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp a world-space decal coordinate to a stable finite range.
/// @param value Candidate coordinate.
/// @param fallback Substitute used for nonfinite input.
/// @return Finite coordinate clamped to the supported symmetric world range.
static double decal3d_coord_or(double value, double fallback) {
    value = decal3d_finite_or(value, fallback);
    if (value > DECAL3D_WORLD_ABS_MAX)
        return DECAL3D_WORLD_ABS_MAX;
    if (value < -DECAL3D_WORLD_ABS_MAX)
        return -DECAL3D_WORLD_ABS_MAX;
    return value;
}

/// @brief Clamp alpha/fade values to [0, 1].
/// @param value Candidate alpha.
/// @param fallback Substitute used for nonfinite input.
/// @return Finite alpha clamped to `[0, 1]`.
static double decal3d_alpha_or(double value, double fallback) {
    value = decal3d_finite_or(value, fallback);
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Return non-zero when @p texture is a valid Pixels handle.
/// @param texture Candidate runtime object.
/// @return Nonzero when @p texture resolves to a live Pixels implementation.
static int decal3d_texture_valid(void *texture) {
    rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(texture);
    return pixels && pixels->data && pixels->width > 0 && pixels->height > 0 &&
           pixels->width <= INT32_MAX && pixels->height <= INT32_MAX;
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
/// @param slot Address of a possibly owned face/texture slot. Invalid stale
///   values are cleared without a release operation.
static void decal3d_release_texture_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_class_id(*slot) != RT_PIXELS_CLASS_ID) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    decal3d_release_ref(slot);
}

/// @brief Release a retained Mesh3D slot only if it still points at Mesh3D.
/// @param slot Address of a possibly owned mesh slot. Invalid stale values are
///   cleared without a release operation.
static void decal3d_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    decal3d_release_ref(slot);
}

/// @brief Release a retained Material3D slot only if it still points at Material3D.
/// @param slot Address of a possibly owned material slot. Invalid stale values
///   are cleared without a release operation.
static void decal3d_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    decal3d_release_ref(slot);
}

/// @brief Normalise the (x,y,z) vector in place; fall back to +Y on zero/non-finite input.
/// @details Coordinates are first sanitized to the supported world range and
///   normalization uses maximum-component scaling to avoid intermediate overflow.
/// @param x In/out X component.
/// @param y In/out Y component.
/// @param z In/out Z component.
static void decal3d_normalize_or_default(double *x, double *y, double *z) {
    if (!x || !y || !z)
        return;
    *x = decal3d_coord_or(*x, 0.0);
    *y = decal3d_coord_or(*y, 1.0);
    *z = decal3d_coord_or(*z, 0.0);
    double max_component = fmax(fabs(*x), fmax(fabs(*y), fabs(*z)));
    if (!isfinite(max_component) || max_component <= 1e-8) {
        *x = 0.0;
        *y = 1.0;
        *z = 0.0;
        return;
    }
    double sx = *x / max_component;
    double sy = *y / max_component;
    double sz = *z / max_component;
    double len = sqrt(sx * sx + sy * sy + sz * sz);
    if (!isfinite(len) || len <= 1e-8) {
        *x = 0.0;
        *y = 1.0;
        *z = 0.0;
        return;
    }
    {
        double nx = sx / len;
        double ny = sy / len;
        double nz = sz / len;
        /* Re-normalizing an already-unit vector can toggle a lane by one ULP. Preserve the
         * retained bits in that case so numeric repair does not invalidate the cached quad. */
        if (fabs(nx - *x) <= 1e-15 && fabs(ny - *y) <= 1e-15 && fabs(nz - *z) <= 1e-15)
            return;
        *x = nx;
        *y = ny;
        *z = nz;
    }
}

/// @brief Release invalid cached refs before draw/update paths use them.
/// @param d Decal whose texture, mesh, and material slots are repaired;
///   `NULL` is ignored.
static void decal3d_repair_refs(rt_decal3d *d) {
    void *old_texture;
    if (!d)
        return;
    old_texture = d->texture;
    if (d->texture && !decal3d_texture_valid(d->texture))
        decal3d_release_texture_slot(&d->texture);
    if (d->mesh && !rt_g3d_has_class(d->mesh, RT_G3D_MESH3D_CLASS_ID))
        decal3d_release_mesh_slot(&d->mesh);
    if (d->material && !rt_g3d_has_class(d->material, RT_G3D_MATERIAL3D_CLASS_ID))
        decal3d_release_material_slot(&d->material);
    if (!d->material)
        d->material_texture = NULL;
    else if (d->material_texture != d->texture || old_texture != d->texture) {
        decal3d_release_material_slot(&d->material);
        d->material_texture = NULL;
    }
}

/// @brief Repair retained placement, lifetime, alpha, and depth-bias state.
/// @details A repaired placement/normal/size invalidates the derived quad so the next draw cannot
///   submit geometry built from stale private values.
/// @param d Decal payload to normalize; `NULL` is accepted.
static void decal3d_repair_numeric_state(rt_decal3d *d) {
    double old_position[3];
    double old_normal[3];
    double old_size;
    if (!d)
        return;
    memcpy(old_position, d->position, sizeof(old_position));
    memcpy(old_normal, d->normal, sizeof(old_normal));
    old_size = d->size;
    d->position[0] = decal3d_coord_or(d->position[0], 0.0);
    d->position[1] = decal3d_coord_or(d->position[1], 0.0);
    d->position[2] = decal3d_coord_or(d->position[2], 0.0);
    decal3d_normalize_or_default(&d->normal[0], &d->normal[1], &d->normal[2]);
    if (!isfinite(d->size) || d->size <= 0.0)
        d->size = 1.0;
    if (d->size > DECAL3D_SIZE_MAX)
        d->size = DECAL3D_SIZE_MAX;
    if (!isfinite(d->lifetime) || !isfinite(d->max_lifetime) || d->max_lifetime < 0.0) {
        d->lifetime = -1.0;
        d->max_lifetime = -1.0;
        d->alpha = 1.0;
    } else {
        if (d->max_lifetime > DECAL3D_LIFETIME_MAX)
            d->max_lifetime = DECAL3D_LIFETIME_MAX;
        if (d->lifetime > d->max_lifetime)
            d->lifetime = d->max_lifetime;
        d->alpha = decal3d_alpha_or(d->alpha, d->lifetime > 0.0 ? 1.0 : 0.0);
        if (d->lifetime <= 0.0)
            d->alpha = 0.0;
    }
    if (!isfinite(d->depth_bias))
        d->depth_bias = 0.0;
    if (d->depth_bias > 0.05)
        d->depth_bias = 0.05;
    if (d->depth_bias < -0.05)
        d->depth_bias = -0.05;
    if ((memcmp(old_position, d->position, sizeof(old_position)) != 0 ||
         memcmp(old_normal, d->normal, sizeof(old_normal)) != 0 || old_size != d->size) &&
        d->mesh)
        decal3d_release_mesh_slot(&d->mesh);
}

/// @brief Add one decal vertex after clamping world coordinates.
/// @param mesh Borrowed Mesh3D receiver.
/// @param x Vertex world X coordinate.
/// @param y Vertex world Y coordinate.
/// @param z Vertex world Z coordinate.
/// @param nx Normal X component forwarded unchanged.
/// @param ny Normal Y component forwarded unchanged.
/// @param nz Normal Z component forwarded unchanged.
/// @param u Horizontal texture coordinate.
/// @param v Vertical texture coordinate.
static void decal3d_add_vertex_clamped(
    void *mesh, double x, double y, double z, double nx, double ny, double nz, double u, double v) {
    rt_mesh3d_add_vertex(mesh,
                         decal3d_coord_or(x, 0.0),
                         decal3d_coord_or(y, 0.0),
                         decal3d_coord_or(z, 0.0),
                         nx,
                         ny,
                         nz,
                         u,
                         v);
}

/// @brief GC finalizer: release the decal's texture, lazily-built mesh, and material.
/// @details Note the lazy-build pattern: `mesh` and `material` are null
///          until the first `draw_decal`. Releasing them here is safe
///          because `decal3d_release_ref` short-circuits on null, so
///          decals that were created but never drawn finalize cleanly
///          without special-casing.
/// @param obj Decal3D payload being finalized; `NULL` is ignored.
static void decal3d_finalizer(void *obj) {
    rt_decal3d *d = (rt_decal3d *)obj;
    if (!d)
        return;
    decal3d_release_texture_slot(&d->texture);
    decal3d_release_mesh_slot(&d->mesh);
    decal3d_release_material_slot(&d->material);
    d->material_texture = NULL;
}

/// @brief Create a new 3D decal projected onto a surface.
/// @details Decals are flat quads oriented along a surface normal, used for
///          bullet holes, blood splatters, scorch marks, etc. The quad mesh is
///          lazily built on first draw from the position, normal, and size. If
///          a lifetime is set, the decal fades out over its last 20% and can be
///          checked with rt_decal3d_is_expired.
/// @param pos_v    Vec3 world-space position on the surface.
/// @param normal_v Vec3 surface normal at the decal location.
/// @param size     Width/height of the decal quad in world units.
/// @param texture  Pixels handle for the decal image. The decal retains it so
///        lazy material creation stays valid across frames.
/// @return Opaque decal handle, or NULL on failure.
void *rt_decal3d_new(void *pos_v, void *normal_v, double size, void *texture) {
    if (!rt_g3d_is_vec3(pos_v) || !rt_g3d_is_vec3(normal_v))
        return NULL;
    rt_decal3d *d =
        (rt_decal3d *)rt_obj_new_i64(RT_G3D_DECAL3D_CLASS_ID, (int64_t)sizeof(rt_decal3d));
    if (!d) {
        rt_trap("Decal3D.New: allocation failed");
        return NULL;
    }
    d->vptr = NULL;
    d->position[0] = decal3d_coord_or(rt_vec3_x(pos_v), 0.0);
    d->position[1] = decal3d_coord_or(rt_vec3_y(pos_v), 0.0);
    d->position[2] = decal3d_coord_or(rt_vec3_z(pos_v), 0.0);
    d->normal[0] = rt_vec3_x(normal_v);
    d->normal[1] = rt_vec3_y(normal_v);
    d->normal[2] = rt_vec3_z(normal_v);
    decal3d_normalize_or_default(&d->normal[0], &d->normal[1], &d->normal[2]);
    d->size = (isfinite(size) && size > 0.0) ? size : 1.0;
    if (d->size > DECAL3D_SIZE_MAX)
        d->size = DECAL3D_SIZE_MAX;
    d->texture = decal3d_texture_valid(texture) ? texture : NULL;
    rt_obj_retain_maybe(d->texture);
    d->lifetime = -1.0; /* permanent by default */
    d->max_lifetime = -1.0;
    d->alpha = 1.0;
    d->depth_bias = 0.0; /* auto: size-scaled */
    d->mesh = NULL;
    d->material = NULL;
    d->material_texture = NULL;
    rt_obj_set_finalizer(d, decal3d_finalizer);
    return d;
}

/// @brief Override the decal's constant depth bias (negative pulls it toward the camera).
/// @details 0 restores the automatic size-scaled bias. Values are clamped to the same ±0.05
///          NDC range the software rasterizer enforces. Applies immediately when the decal's
///          material already exists.
/// @param obj Decal3D receiver; invalid handles are ignored.
/// @param bias Requested constant NDC-scale bias; nonfinite values restore auto mode.
void rt_decal3d_set_depth_bias(void *obj, double bias) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d)
        return;
    decal3d_repair_refs(d);
    decal3d_repair_numeric_state(d);
    if (!isfinite(bias))
        bias = 0.0;
    if (bias > 0.05)
        bias = 0.05;
    if (bias < -0.05)
        bias = -0.05;
    d->depth_bias = bias;
    if (d->material) {
        double applied = bias != 0.0 ? bias : -fmax(0.0005, fmin(0.005, d->size * 0.0005));
        rt_material3d_set_depth_bias(d->material, applied, -1.0);
    }
}

/// @brief Set how long the decal should live before expiring (< 0 = permanent).
/// @param obj Decal3D receiver; invalid handles are ignored.
/// @param seconds Lifetime in seconds. Nonfinite or negative input makes the
///   decal permanent, zero expires immediately, and large values are capped.
void rt_decal3d_set_lifetime(void *obj, double seconds) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d)
        return;
    if (!isfinite(seconds))
        seconds = -1.0;
    if (seconds < 0.0) {
        d->lifetime = -1.0;
        d->max_lifetime = -1.0;
        d->alpha = 1.0;
        return;
    }
    if (seconds > DECAL3D_LIFETIME_MAX)
        seconds = DECAL3D_LIFETIME_MAX;
    d->lifetime = seconds;
    d->max_lifetime = seconds;
    d->alpha = seconds == 0.0 ? 0.0 : 1.0;
}

/// @brief Advance the decal's lifetime timer and apply fade-out in the last 20%.
/// @param obj Decal3D receiver; invalid handles are ignored.
/// @param dt Positive elapsed seconds. Nonfinite or nonpositive values are
///   ignored and extreme steps are capped to the supported lifetime maximum.
void rt_decal3d_update(void *obj, double dt) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d || !isfinite(dt) || dt <= 0.0)
        return;
    if (dt > DECAL3D_DT_MAX)
        dt = DECAL3D_DT_MAX;
    decal3d_repair_numeric_state(d);
    if (d->lifetime < 0)
        return; /* permanent */
    d->lifetime -= dt;
    /* Fade alpha over last 20% of lifetime */
    if (d->max_lifetime > 0 && d->lifetime < d->max_lifetime * 0.2) {
        d->alpha = d->lifetime / (d->max_lifetime * 0.2);
        if (d->alpha < 0.0)
            d->alpha = 0.0;
        if (d->alpha > 1.0)
            d->alpha = 1.0;
    }
}

/// @brief Check if the decal's lifetime has elapsed (permanent decals never expire).
/// @details Corrupt nonfinite lifetime state is repaired to the permanent,
///   fully opaque state rather than treated as expired.
/// @param obj Decal3D receiver.
/// @return Nonzero for an invalid receiver or an elapsed finite decal; zero
///   for a live or permanent decal.
int8_t rt_decal3d_is_expired(void *obj) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d)
        return 1;
    decal3d_repair_numeric_state(d);
    if (d->max_lifetime < 0)
        return 0; /* permanent */
    return d->lifetime <= 0 ? 1 : 0;
}

/// @brief Internal floating-origin hook: subtract the world rebase delta and
///   discard cached mesh geometry so the next draw rebuilds at the new origin.
/// @param obj Decal3D receiver; invalid handles are ignored.
/// @param dx World-origin X displacement to subtract.
/// @param dy World-origin Y displacement to subtract.
/// @param dz World-origin Z displacement to subtract.
void rt_decal3d_rebase_origin(void *obj, double dx, double dy, double dz) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d)
        return;
    decal3d_repair_numeric_state(d);
    double delta[3] = {
        decal3d_coord_or(dx, 0.0), decal3d_coord_or(dy, 0.0), decal3d_coord_or(dz, 0.0)};
    if (delta[0] == 0.0 && delta[1] == 0.0 && delta[2] == 0.0)
        return;
    d->position[0] = decal3d_coord_or(d->position[0] - delta[0], 0.0);
    d->position[1] = decal3d_coord_or(d->position[1] - delta[1], 0.0);
    d->position[2] = decal3d_coord_or(d->position[2] - delta[2], 0.0);
    decal3d_release_mesh_slot(&d->mesh);
}

/// @brief Copy the decal world position into @p out.
/// @details Used by floating-origin consumers to observe rebasing. Invalid
///   handles produce a zero vector; a null destination is ignored.
/// @param obj Decal3D receiver.
/// @param out Output array of three doubles.
void rt_decal3d_get_position(void *obj, double out[3]) {
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!out)
        return;
    if (d)
        decal3d_repair_numeric_state(d);
    out[0] = d ? d->position[0] : 0.0;
    out[1] = d ? d->position[1] : 0.0;
    out[2] = d ? d->position[2] : 0.0;
}

/// @brief Build the decal's quad mesh + material on first draw (lazy init).
/// @details Classic tangent-frame construction:
///          1. Pick an arbitrary "up" that isn't parallel to the surface
///             normal (world +Y, or world +X when the normal is already
///             close to ±Y, to guarantee the subsequent cross products
///             don't degenerate).
///          2. `right = cross(up, normal)`, then normalize.
///          3. `true_up = cross(normal, right)` — already unit-length
///             since normal and right are both unit and orthogonal.
///          4. Emit the four corner vertices offset by `0.5 * size`
///             along the right/up axes, each lifted by a small size-aware
///             normal offset to reduce exact coplanarity.
///          The material is created `unlit` so decals show their
///          texture as-is without picking up scene lighting (matches
///          bullet-hole / scorch-mark convention). A small negative depth
///          bias is also applied so backends resolve remaining coplanar
///          depth ties consistently.
/// @param d Decal whose owned mesh/material cache is validated or constructed.
///   Allocation failure leaves missing pieces null so a later draw may retry.
static void ensure_decal_mesh(rt_decal3d *d) {
    if (!d)
        return;
    decal3d_repair_refs(d);
    decal3d_repair_numeric_state(d);
    if (d->mesh && d->material)
        return;

    /* Build tangent frame from normal */
    double nx = d->normal[0], ny = d->normal[1], nz = d->normal[2];
    decal3d_normalize_or_default(&nx, &ny, &nz);
    d->normal[0] = nx;
    d->normal[1] = ny;
    d->normal[2] = nz;

    /* Choose arbitrary up vector not parallel to normal */
    double ux = 0.0, uy = 1.0, uz = 0.0;
    if (fabs(ny) > 0.9) {
        ux = 1.0;
        uy = 0.0;
        uz = 0.0;
    }

    /* Right = cross(up, normal) */
    double rx = uy * nz - uz * ny;
    double ry = uz * nx - ux * nz;
    double rz = ux * ny - uy * nx;
    double rlen = sqrt(rx * rx + ry * ry + rz * rz);
    if (!isfinite(rlen) || rlen <= 1e-8) {
        rx = 1.0;
        ry = 0.0;
        rz = 0.0;
    } else {
        rx /= rlen;
        ry /= rlen;
        rz /= rlen;
    }

    /* True up = cross(normal, right) */
    double tux = ny * rz - nz * ry;
    double tuy = nz * rx - nx * rz;
    double tuz = nx * ry - ny * rx;
    decal3d_normalize_or_default(&tux, &tuy, &tuz);

    d->size = (isfinite(d->size) && d->size > 0.0) ? d->size : 1.0;
    if (d->size > DECAL3D_SIZE_MAX)
        d->size = DECAL3D_SIZE_MAX;
    double hs = d->size * 0.5; /* half-size */
    double off = fmax(0.0005, fmin(0.01, d->size * 0.0005));
    double cx = decal3d_coord_or(d->position[0] + nx * off, 0.0);
    double cy = decal3d_coord_or(d->position[1] + ny * off, 0.0);
    double cz = decal3d_coord_or(d->position[2] + nz * off, 0.0);

    if (!d->mesh) {
        d->mesh = rt_mesh3d_new();
        if (!d->mesh)
            return;
        decal3d_add_vertex_clamped(d->mesh,
                                   cx - rx * hs - tux * hs,
                                   cy - ry * hs - tuy * hs,
                                   cz - rz * hs - tuz * hs,
                                   nx,
                                   ny,
                                   nz,
                                   0.0,
                                   0.0);
        decal3d_add_vertex_clamped(d->mesh,
                                   cx + rx * hs - tux * hs,
                                   cy + ry * hs - tuy * hs,
                                   cz + rz * hs - tuz * hs,
                                   nx,
                                   ny,
                                   nz,
                                   1.0,
                                   0.0);
        decal3d_add_vertex_clamped(d->mesh,
                                   cx + rx * hs + tux * hs,
                                   cy + ry * hs + tuy * hs,
                                   cz + rz * hs + tuz * hs,
                                   nx,
                                   ny,
                                   nz,
                                   1.0,
                                   1.0);
        decal3d_add_vertex_clamped(d->mesh,
                                   cx - rx * hs + tux * hs,
                                   cy - ry * hs + tuy * hs,
                                   cz - rz * hs + tuz * hs,
                                   nx,
                                   ny,
                                   nz,
                                   0.0,
                                   1.0);
        rt_mesh3d_add_triangle(d->mesh, 0, 1, 2);
        rt_mesh3d_add_triangle(d->mesh, 0, 2, 3);
        {
            rt_mesh3d *built = (rt_mesh3d *)rt_g3d_checked_or_null(d->mesh, RT_G3D_MESH3D_CLASS_ID);
            if (!built || rt_mesh3d_safe_vertex_count(built) != 4u ||
                rt_mesh3d_validated_index_count(built) != 6u) {
                decal3d_release_mesh_slot(&d->mesh);
                return;
            }
        }
    }

    /* Create material with texture and alpha */
    d->material = d->material ? d->material : rt_material3d_new();
    if (!d->material)
        return;
    if (d->texture)
        rt_material3d_set_texture(d->material, d->texture);
    d->material_texture = d->texture;
    d->alpha = decal3d_alpha_or(d->alpha, 1.0);
    rt_material3d_set_alpha(d->material, d->alpha);
    rt_material3d_set_unlit(d->material, 1); /* decals are unlit — they show texture only */
    /* Constant depth bias pulls the decal in front of its receiver. Auto mode scales with
     * decal size — larger decals span more depth at grazing angles, where a fixed constant
     * shimmers in the far field. Units are NDC-scale (VGFX3D_DEPTH_BIAS_CONSTANT_SCALE);
     * SetDepthBias overrides for content that needs its own tuning. */
    double bias =
        d->depth_bias != 0.0 ? d->depth_bias : -fmax(0.0005, fmin(0.005, d->size * 0.0005));
    rt_material3d_set_depth_bias(d->material, bias, -1.0);
}

/// @brief Render a live decal onto the canvas.
/// @details Lazily builds and reuses the quad mesh/material, refreshes material
///   alpha from the current fade, and submits it with an identity transform.
///   Invalid, expired, or incompletely constructed decals are ignored.
/// @param canvas Canvas3D receiver passed to deferred mesh submission.
/// @param obj Decal3D receiver to draw.
void rt_canvas3d_draw_decal(void *canvas, void *obj) {
    if (!canvas || !obj)
        return;
    rt_decal3d *d = (rt_decal3d *)rt_g3d_checked_or_null(obj, RT_G3D_DECAL3D_CLASS_ID);
    if (!d)
        return;
    decal3d_repair_refs(d);
    decal3d_repair_numeric_state(d);
    if (d->max_lifetime >= 0 && d->lifetime <= 0)
        return; /* expired */
    ensure_decal_mesh(d);
    if (!d->mesh || !d->material)
        return;

    /* Update material alpha for fade */
    d->alpha = decal3d_alpha_or(d->alpha, 1.0);
    rt_material3d_set_alpha(d->material, d->alpha);
    {
        double bias =
            d->depth_bias != 0.0 ? d->depth_bias : -fmax(0.0005, fmin(0.005, d->size * 0.0005));
        rt_material3d_set_depth_bias(d->material, bias, -1.0);
    }

    {
        static const double identity[16] = {
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
        };
        rt_canvas3d_draw_mesh_matrix(canvas, d->mesh, identity, d->material);
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
