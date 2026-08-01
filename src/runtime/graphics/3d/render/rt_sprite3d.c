//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_sprite3d.c
// Purpose: 3D sprite — camera-facing billboard with sprite sheet frames.
//
// Key invariants:
//   - Billboard computed from camera view matrix right/up vectors.
//   - Quad built per-frame (4 verts, 2 tris) with UV from frame rect.
//   - Anchor offset applied before billboard expansion.
//   - Material is cached and only rebuilt when the texture changes.
//
// Ownership/Lifetime:
//   - Sprite3D is GC-managed; finalizer releases texture, mesh, and material.
//   - Per-frame mesh is parked on the canvas's temp-object queue.
//
// Links: rt_sprite3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements camera-facing Sprite3D billboards with spritesheet, tint, and blend controls.
/// @details Sprite3D retains its source Pixels plus reusable mesh and material objects, sanitizes
///   runtime inputs, rebuilds camera-relative quad geometry per draw, and parks cached resources
///   on the canvas until deferred submission completes.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_sprite3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_pixels_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_clear(void *m);
extern void rt_mesh3d_add_vertex(
    void *m, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *m, int64_t v0, int64_t v1, int64_t v2);
extern void rt_canvas3d_draw_mesh(void *canvas, void *mesh, void *transform, void *material);
extern void rt_canvas3d_draw_mesh_matrix(void *canvas,
                                         void *mesh,
                                         const double *transform,
                                         void *material);
extern int rt_canvas3d_add_temp_object(void *canvas, void *value);
extern int rt_canvas3d_get_camera_relative_origin(void *canvas, double out_origin[3]);
extern void *rt_material3d_new(void);
extern void rt_material3d_set_texture(void *m, void *tex);
extern void rt_material3d_set_unlit(void *m, int8_t u);
extern void rt_material3d_set_color(void *m, double r, double g, double b);
extern void rt_material3d_set_alpha_mode(void *m, int64_t mode);

#define SPRITE3D_WORLD_ABS_MAX 1000000000000.0
#define SPRITE3D_SCALE_MAX 1000000.0

typedef struct {
    void *vptr;
    void *texture;
    double position[3];
    double scale_wh[2]; /* width, height in world units */
    double anchor[2];   /* pivot [0,1], default (0.5, 0.5) */
    int32_t frame_x, frame_y, frame_w, frame_h;
    int32_t tex_w, tex_h;
    int8_t additive;       /* route through the additive blend state (muzzle glows, tracers) */
    double tint[3];        /* multiplies the texture; default white */
    void *cached_mesh;     /* Reused each frame (billboard changes with camera) */
    void *cached_material; /* Created once, reused until texture changes */
    void *cached_texture;  /* Track texture for material invalidation */
} rt_sprite3d;

/// @brief Release a GC-tracked reference slot and null it.
/// @details Used for the three cached refs (`texture`, `cached_mesh`, `cached_material`)
///   when the sprite is finalized or when the material needs to be rebuilt because the
///   texture changed. Nulling the slot makes subsequent calls idempotent.
/// @param[in,out] slot Address of the retained reference to release and clear.
static void sprite3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return true when @p texture is a live Pixels object.
/// @param texture Candidate runtime object.
/// @return 1 when the object validates as Pixels, or 0 otherwise.
static int sprite3d_texture_valid(void *texture) {
    rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(texture);
    return pixels && pixels->data && pixels->width > 0 && pixels->height > 0 &&
           pixels->width <= INT32_MAX && pixels->height <= INT32_MAX;
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
/// @param[in,out] slot Address of the texture reference; invalid unowned values are cleared without
///   release.
static void sprite3d_release_texture_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_class_id(*slot) != RT_PIXELS_CLASS_ID) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    sprite3d_release_ref(slot);
}

/// @brief Release a retained Mesh3D slot only if it still points at Mesh3D.
/// @param[in,out] slot Address of the mesh reference; invalid unowned values are cleared without
///   release.
static void sprite3d_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    sprite3d_release_ref(slot);
}

/// @brief Release a retained Material3D slot only if it still points at Material3D.
/// @param[in,out] slot Address of the material reference; invalid unowned values are cleared
///   without release.
static void sprite3d_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    sprite3d_release_ref(slot);
}

/// @brief Clear corrupted cached refs before draw paths dereference them.
/// @param s Sprite whose texture, mesh, material, and material-texture tracking are repaired.
static void sprite3d_repair_refs(rt_sprite3d *s) {
    if (!s)
        return;
    void *old_texture = s->texture;
    if (s->texture && !sprite3d_texture_valid(s->texture))
        sprite3d_release_texture_slot(&s->texture);
    if (old_texture && old_texture != s->texture)
        s->cached_texture = NULL;
    if (s->cached_mesh && !rt_g3d_has_class(s->cached_mesh, RT_G3D_MESH3D_CLASS_ID))
        sprite3d_release_mesh_slot(&s->cached_mesh);
    if (s->cached_material && !rt_g3d_has_class(s->cached_material, RT_G3D_MATERIAL3D_CLASS_ID)) {
        sprite3d_release_material_slot(&s->cached_material);
        s->cached_texture = NULL;
    }
}

/// @brief Return @p value when finite, else @p fallback. Sanitizes Vec3 inputs.
/// @param value Candidate runtime floating-point value.
/// @param fallback Replacement for NaN or infinity.
/// @return @p value when finite, otherwise @p fallback.
static double sprite3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp world-space coordinates to a range that remains drawable in the backend.
/// @param value Candidate world-space coordinate.
/// @param fallback Replacement used for non-finite input.
/// @return A finite coordinate constrained to the supported world-space range.
static double sprite3d_coord_or(double value, double fallback) {
    value = sprite3d_finite_or(value, fallback);
    if (value < -SPRITE3D_WORLD_ABS_MAX)
        return -SPRITE3D_WORLD_ABS_MAX;
    if (value > SPRITE3D_WORLD_ABS_MAX)
        return SPRITE3D_WORLD_ABS_MAX;
    return value;
}

/// @brief Clamp positive billboard dimensions to avoid overflowing generated vertices.
/// @param value Candidate width or height.
/// @param fallback Replacement for non-finite or non-positive input.
/// @return A positive finite scale no greater than `SPRITE3D_SCALE_MAX`.
static double sprite3d_positive_scale_or(double value, double fallback) {
    value = sprite3d_finite_or(value, fallback);
    if (value <= 0.0)
        value = fallback;
    if (value <= 0.0)
        value = 1.0;
    if (value > SPRITE3D_SCALE_MAX)
        return SPRITE3D_SCALE_MAX;
    return value;
}

/// @brief Normalize a vector, replacing invalid/zero vectors with a fallback axis.
/// @param[in,out] x Address of the vector's x component.
/// @param[in,out] y Address of the vector's y component.
/// @param[in,out] z Address of the vector's z component.
/// @param fallback_x Fallback x component.
/// @param fallback_y Fallback y component.
/// @param fallback_z Fallback z component.
/// @return 1 when a finite unit vector was produced, or 0 for null outputs or an unusable vector
///   and fallback.
static int sprite3d_normalize3(
    double *x, double *y, double *z, double fallback_x, double fallback_y, double fallback_z) {
    double len;
    double max_component;
    if (!x || !y || !z)
        return 0;
    *x = sprite3d_finite_or(*x, fallback_x);
    *y = sprite3d_finite_or(*y, fallback_y);
    *z = sprite3d_finite_or(*z, fallback_z);
    max_component = fmax(fabs(*x), fmax(fabs(*y), fabs(*z)));
    if (!isfinite(max_component) || max_component <= 1e-12) {
        *x = fallback_x;
        *y = fallback_y;
        *z = fallback_z;
        max_component = fmax(fabs(*x), fmax(fabs(*y), fabs(*z)));
        if (!isfinite(max_component) || max_component <= 1e-12)
            return 0;
    }
    *x /= max_component;
    *y /= max_component;
    *z /= max_component;
    len = sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    if (!isfinite(len) || len <= 1e-12)
        return 0;
    *x /= len;
    *y /= len;
    *z /= len;
    return 1;
}

/// @brief Choose an axis that is not parallel to the supplied normalized vector.
/// @param x Normalized source x component.
/// @param y Normalized source y component.
/// @param z Normalized source z component.
/// @param[out] out_x Destination for the seed axis x component.
/// @param[out] out_y Destination for the seed axis y component.
/// @param[out] out_z Destination for the seed axis z component.
static void sprite3d_perpendicular_seed(
    double x, double y, double z, double *out_x, double *out_y, double *out_z) {
    if (fabs(y) < 0.9) {
        *out_x = 0.0;
        *out_y = 1.0;
        *out_z = 0.0;
    } else if (fabs(x) < 0.9) {
        *out_x = 1.0;
        *out_y = 0.0;
        *out_z = 0.0;
    } else {
        *out_x = 0.0;
        *out_y = 0.0;
        *out_z = 1.0;
    }
}

/// @brief Clamp @p value to [0, 1], substituting 0.5 for non-finite input. Used for anchor coords.
/// @param value Candidate normalized anchor coordinate.
/// @return A finite value in the closed unit interval.
static double sprite3d_clamp01(double value) {
    if (!isfinite(value))
        return 0.5;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Clamp a tint channel to `[0,1]`, using white for nonfinite corruption.
/// @param value Candidate retained tint lane.
/// @return Canonical finite tint multiplier.
static double sprite3d_tint_or_white(double value) {
    if (!isfinite(value))
        return 1.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Repair retained numeric state and rederive texture dimensions from the live Pixels.
/// @param s Sprite payload to repair; `NULL` is accepted.
static void sprite3d_repair_numeric_state(rt_sprite3d *s) {
    rt_pixels_impl *pixels;
    if (!s)
        return;
    s->position[0] = sprite3d_coord_or(s->position[0], 0.0);
    s->position[1] = sprite3d_coord_or(s->position[1], 0.0);
    s->position[2] = sprite3d_coord_or(s->position[2], 0.0);
    s->scale_wh[0] = sprite3d_positive_scale_or(s->scale_wh[0], 1.0);
    s->scale_wh[1] = sprite3d_positive_scale_or(s->scale_wh[1], 1.0);
    s->anchor[0] = sprite3d_clamp01(s->anchor[0]);
    s->anchor[1] = sprite3d_clamp01(s->anchor[1]);
    s->additive = s->additive ? 1 : 0;
    s->tint[0] = sprite3d_tint_or_white(s->tint[0]);
    s->tint[1] = sprite3d_tint_or_white(s->tint[1]);
    s->tint[2] = sprite3d_tint_or_white(s->tint[2]);
    pixels = rt_pixels_checked_impl_or_null(s->texture);
    if (sprite3d_texture_valid(s->texture)) {
        s->tex_w = (int32_t)pixels->width;
        s->tex_h = (int32_t)pixels->height;
    } else {
        s->tex_w = 0;
        s->tex_h = 0;
    }
    rt_sprite3d_set_frame(s, s->frame_x, s->frame_y, s->frame_w, s->frame_h);
}

/// @brief Clamp an int64 frame component to int32: negatives become @p fallback, values above
///   INT32_MAX saturate to INT32_MAX.
/// @param value Candidate frame offset or extent.
/// @param fallback Replacement for negative input.
/// @return The value represented in `int32_t` after fallback and upper saturation.
static int32_t sprite3d_clamp_frame_component_i32(int64_t value, int32_t fallback) {
    if (value < 0)
        return fallback;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}

/// @brief Build a row-major model matrix that translates by @p origin (identity rotation/scale).
/// @param origin Optional three-element camera-relative origin; `NULL` leaves an identity matrix.
/// @param[out] out Sixteen-element row-major destination matrix.
static void sprite3d_origin_model_matrix(const double origin[3], double out[16]) {
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
    memcpy(out, identity, sizeof(identity));
    if (origin) {
        out[3] = sprite3d_coord_or(origin[0], 0.0);
        out[7] = sprite3d_coord_or(origin[1], 0.0);
        out[11] = sprite3d_coord_or(origin[2], 0.0);
    }
}

/// @brief GC finalizer — release the texture, billboard mesh, and cached material.
/// @details Sprite3D lazily caches three dependent objects: the billboard
///   mesh (regenerated per frame to face the camera), the material
///   (valid until the texture changes), and the source texture itself.
///   All three are ref-counted — releasing is safe even if other
///   objects still hold the texture. `cached_texture` is only a
///   tracking pointer (not a retained ref) so it's just nulled, not
///   released — the actual release happened through `texture`.
/// @param obj Sprite3D runtime object being finalized; ignored when `NULL`.
static void sprite3d_finalizer(void *obj) {
    rt_sprite3d *s = (rt_sprite3d *)obj;
    if (!s)
        return;
    sprite3d_release_texture_slot(&s->texture);
    sprite3d_release_mesh_slot(&s->cached_mesh);
    sprite3d_release_material_slot(&s->cached_material);
    s->cached_texture = NULL;
}

/// @brief Create a 3D billboard sprite that always faces the camera.
/// @details Sprites are textured quads rendered in 3D space that orient toward
///          the camera using its view matrix right/up vectors. Commonly used for
///          particles, distant trees, UI indicators, etc. The sprite supports
///          spritesheet frames (SetFrame), anchor point control, and non-uniform scale.
/// @param texture Pixels handle for the sprite image. The sprite retains it so
///        lazy billboard material creation stays valid across frames.
/// @return Opaque sprite handle, or NULL on failure.
void *rt_sprite3d_new(void *texture) {
    rt_pixels_impl *pixels = NULL;
    if (texture) {
        pixels = rt_pixels_checked_impl_or_null(texture);
        if (!sprite3d_texture_valid(texture)) {
            rt_trap("Sprite3D.New: texture must be non-empty Pixels");
            return NULL;
        }
    }
    rt_sprite3d *s =
        (rt_sprite3d *)rt_obj_new_i64(RT_G3D_SPRITE3D_CLASS_ID, (int64_t)sizeof(rt_sprite3d));
    if (!s) {
        rt_trap("Sprite3D.New: allocation failed");
        return NULL;
    }
    s->vptr = NULL;
    s->texture = texture;
    rt_obj_retain_maybe(texture);
    s->position[0] = s->position[1] = s->position[2] = 0.0;
    s->scale_wh[0] = 1.0;
    s->scale_wh[1] = 1.0;
    s->anchor[0] = 0.5;
    s->anchor[1] = 0.5;
    s->frame_x = 0;
    s->frame_y = 0;
    s->frame_w = 0;
    s->frame_h = 0; /* 0 = use full texture */
    s->tex_w = 0;
    s->tex_h = 0;
    s->additive = 0;
    s->tint[0] = s->tint[1] = s->tint[2] = 1.0;
    s->cached_mesh = NULL;
    s->cached_material = NULL;
    s->cached_texture = NULL;

    /* Try to get texture dimensions */
    if (pixels) {
        s->tex_w = (int32_t)pixels->width;
        s->tex_h = (int32_t)pixels->height;
        s->frame_w = s->tex_w;
        s->frame_h = s->tex_h;
    }

    rt_obj_set_finalizer(s, sprite3d_finalizer);
    return s;
}

/// @brief Set the world-space position where the sprite is rendered.
/// @param obj Sprite3D instance.
/// @param x World-space x coordinate.
/// @param y World-space y coordinate.
/// @param z World-space z coordinate.
void rt_sprite3d_set_position(void *obj, double x, double y, double z) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    s->position[0] = sprite3d_coord_or(x, 0.0);
    s->position[1] = sprite3d_coord_or(y, 0.0);
    s->position[2] = sprite3d_coord_or(z, 0.0);
}

/// @brief Set the width and height scale of the sprite in world units.
/// @param obj Sprite3D instance.
/// @param w Positive billboard width, sanitized and bounded for vertex generation.
/// @param h Positive billboard height, sanitized and bounded for vertex generation.
void rt_sprite3d_set_scale(void *obj, double w, double h) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    s->scale_wh[0] = sprite3d_positive_scale_or(w, 1.0);
    s->scale_wh[1] = sprite3d_positive_scale_or(h, 1.0);
}

/// @brief Set the anchor point (0,0 = bottom-left, 0.5,0.5 = center, 1,1 = top-right).
/// @param obj Sprite3D instance.
/// @param ax Horizontal anchor coordinate clamped to the unit interval.
/// @param ay Vertical anchor coordinate clamped to the unit interval.
void rt_sprite3d_set_anchor(void *obj, double ax, double ay) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    s->anchor[0] = sprite3d_clamp01(ax);
    s->anchor[1] = sprite3d_clamp01(ay);
}

/// @brief Set the spritesheet sub-rectangle to display (for animated sprites).
/// @param obj Sprite3D instance.
/// @param fx Horizontal frame origin in texture pixels.
/// @param fy Vertical frame origin in texture pixels.
/// @param fw Frame width; non-positive input selects the available texture width.
/// @param fh Frame height; non-positive input selects the available texture height.
void rt_sprite3d_set_frame(void *obj, int64_t fx, int64_t fy, int64_t fw, int64_t fh) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    if (fw <= 0)
        fw = s->tex_w > 0 ? s->tex_w : 1;
    if (fh <= 0)
        fh = s->tex_h > 0 ? s->tex_h : 1;
    fx = sprite3d_clamp_frame_component_i32(fx, 0);
    fy = sprite3d_clamp_frame_component_i32(fy, 0);
    fw = sprite3d_clamp_frame_component_i32(fw, s->tex_w > 0 ? s->tex_w : 1);
    fh = sprite3d_clamp_frame_component_i32(fh, s->tex_h > 0 ? s->tex_h : 1);
    if (s->tex_w > 0) {
        if (fx >= s->tex_w)
            fx = s->tex_w - 1;
        if (fw > s->tex_w - fx)
            fw = s->tex_w - fx;
    }
    if (s->tex_h > 0) {
        if (fy >= s->tex_h)
            fy = s->tex_h - 1;
        if (fh > s->tex_h - fy)
            fh = s->tex_h - fy;
    }
    s->frame_x = (int32_t)fx;
    s->frame_y = (int32_t)fy;
    s->frame_w = (int32_t)fw;
    s->frame_h = (int32_t)fh;
}

/// @brief Toggle additive blending (1 = additive for glows/tracers, 0 = alpha blend).
/// @param obj Sprite3D instance.
/// @param additive Non-zero for additive blending, or zero for the non-additive path.
void rt_sprite3d_set_additive(void *obj, int8_t additive) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    s->additive = additive ? 1 : 0;
}

/// @brief Current additive-blend flag.
/// @param obj Candidate Sprite3D instance.
/// @return 1 when additive blending is enabled, or 0 for non-additive or invalid input.
int8_t rt_sprite3d_get_additive(void *obj) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return 0;
    s->additive = s->additive ? 1 : 0;
    return s->additive;
}

/// @brief Set a packed 0xRRGGBB tint multiplied into the texture (Particles3D convention).
/// @param obj Sprite3D instance.
/// @param rgb Packed red, green, and blue bytes; higher bits are ignored.
void rt_sprite3d_set_color(void *obj, int64_t rgb) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    uint64_t packed = (uint64_t)rgb;
    if (!s)
        return;
    s->tint[0] = (double)((packed >> 16) & UINT64_C(0xFF)) / 255.0;
    s->tint[1] = (double)((packed >> 8) & UINT64_C(0xFF)) / 255.0;
    s->tint[2] = (double)(packed & UINT64_C(0xFF)) / 255.0;
}

/// @brief Shift standalone Sprite3D world-space position by -delta for floating-origin rebases.
/// @param obj Sprite3D instance.
/// @param dx World-space x displacement subtracted from the stored position.
/// @param dy World-space y displacement subtracted from the stored position.
/// @param dz World-space z displacement subtracted from the stored position.
void rt_sprite3d_rebase_origin(void *obj, double dx, double dy, double dz) {
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    if (!s)
        return;
    sprite3d_repair_numeric_state(s);
    double delta[3] = {
        sprite3d_coord_or(dx, 0.0), sprite3d_coord_or(dy, 0.0), sprite3d_coord_or(dz, 0.0)};
    if (delta[0] == 0.0 && delta[1] == 0.0 && delta[2] == 0.0)
        return;
    s->position[0] = sprite3d_coord_or(s->position[0] - delta[0], 0.0);
    s->position[1] = sprite3d_coord_or(s->position[1] - delta[1], 0.0);
    s->position[2] = sprite3d_coord_or(s->position[2] - delta[2], 0.0);
}

/// @brief Draw a 3D sprite as a camera-facing billboard on the canvas.
/// @details Constructs a billboard quad each frame using the camera's right and
///          up vectors, applies the anchor offset, and renders as a textured mesh.
///          The quad geometry is cached and reused between frames.
/// @param canvas Canvas3D receiving the deferred mesh draw.
/// @param obj Sprite3D instance supplying texture, frame, position, and appearance.
/// @param camera Camera3D whose view basis orients the billboard.
void rt_canvas3d_draw_sprite3d(void *canvas, void *obj, void *camera) {
    if (!canvas || !obj || !camera)
        return;
    rt_sprite3d *s = (rt_sprite3d *)rt_g3d_checked_or_null(obj, RT_G3D_SPRITE3D_CLASS_ID);
    rt_camera3d *cam = rt_camera3d_checked_or_stack(camera);
    if (!s || !cam)
        return;
    sprite3d_repair_refs(s);
    sprite3d_repair_numeric_state(s);

    /* Extract right and up vectors from camera view matrix (row-major) */
    double rx = cam->view[0], ry = cam->view[1], rz = cam->view[2];
    double ux = cam->view[4], uy = cam->view[5], uz = cam->view[6];
    double sx;
    double sy;
    double dot;
    sprite3d_normalize3(&rx, &ry, &rz, 1.0, 0.0, 0.0);
    sprite3d_normalize3(&ux, &uy, &uz, 0.0, 1.0, 0.0);
    dot = rx * ux + ry * uy + rz * uz;
    ux -= rx * dot;
    uy -= ry * dot;
    uz -= rz * dot;
    if (!sprite3d_normalize3(&ux, &uy, &uz, 0.0, 0.0, 0.0)) {
        sprite3d_perpendicular_seed(rx, ry, rz, &ux, &uy, &uz);
        dot = rx * ux + ry * uy + rz * uz;
        ux -= rx * dot;
        uy -= ry * dot;
        uz -= rz * dot;
        if (!sprite3d_normalize3(&ux, &uy, &uz, 0.0, 1.0, 0.0)) {
            ux = 0.0;
            uy = 1.0;
            uz = 0.0;
        }
    }

    sx = sprite3d_positive_scale_or(s->scale_wh[0], 1.0);
    sy = sprite3d_positive_scale_or(s->scale_wh[1], 1.0);

    /* Anchor offset: shift center by (0.5 - anchor) in each axis */
    double hw = sx * 0.5;
    double hh = sy * 0.5;
    double ax = (0.5 - sprite3d_clamp01(s->anchor[0])) * sx;
    double ay = (0.5 - sprite3d_clamp01(s->anchor[1])) * sy;
    double px = sprite3d_coord_or(s->position[0], 0.0);
    double py = sprite3d_coord_or(s->position[1], 0.0);
    double pz = sprite3d_coord_or(s->position[2], 0.0);
    double cx = sprite3d_coord_or(px + rx * ax + ux * ay, px);
    double cy = sprite3d_coord_or(py + ry * ax + uy * ay, py);
    double cz = sprite3d_coord_or(pz + rz * ax + uz * ay, pz);
    double origin[3] = {0.0, 0.0, 0.0};
    (void)rt_canvas3d_get_camera_relative_origin(canvas, origin);
    origin[0] = sprite3d_coord_or(origin[0], 0.0);
    origin[1] = sprite3d_coord_or(origin[1], 0.0);
    origin[2] = sprite3d_coord_or(origin[2], 0.0);

    /* UV from frame rect */
    double u0 = 0.0, v0 = 0.0, u1 = 1.0, v1 = 1.0;
    if (s->tex_w > 0 && s->tex_h > 0) {
        int32_t fx = s->frame_x;
        int32_t fy = s->frame_y;
        int32_t fw = s->frame_w;
        int32_t fh = s->frame_h;
        if (fx < 0 || fx >= s->tex_w)
            fx = 0;
        if (fy < 0 || fy >= s->tex_h)
            fy = 0;
        if (fw <= 0 || fw > s->tex_w - fx)
            fw = s->tex_w - fx;
        if (fh <= 0 || fh > s->tex_h - fy)
            fh = s->tex_h - fy;
        u0 = (double)fx / s->tex_w;
        v0 = (double)fy / s->tex_h;
        u1 = (double)(fx + fw) / s->tex_w;
        v1 = (double)(fy + fh) / s->tex_h;
    }

    /* Lazily create cached mesh and material (once, reused every frame) */
    if (!s->cached_mesh)
        s->cached_mesh = rt_mesh3d_new();
    if (!s->cached_mesh)
        return;
    if (!s->cached_material || s->cached_texture != s->texture) {
        sprite3d_release_material_slot(&s->cached_material);
        s->cached_material = rt_material3d_new();
        if (!s->cached_material) {
            s->cached_texture = NULL;
            return;
        }
        if (s->texture)
            rt_material3d_set_texture(s->cached_material, s->texture);
        rt_material3d_set_unlit(s->cached_material, 1);
        s->cached_texture = s->texture;
    }

    /* Blend state and tint can change frame-to-frame; refresh the cached material.
       Same recipe as Particles3D: additive routes through the transparent queue. */
    rt_material3d_set_color(s->cached_material, s->tint[0], s->tint[1], s->tint[2]);
    ((rt_material3d *)s->cached_material)->additive_blend = s->additive ? 1 : 0;
    rt_material3d_set_alpha_mode(s->cached_material, RT_MATERIAL3D_ALPHA_MODE_BLEND);

    /* Rebuild billboard quad each frame (orientation changes with camera).
       Clear resets vertex/index counts without freeing the backing arrays. */
    void *mesh = s->cached_mesh;
    rt_mesh3d_clear(mesh);

    double nx = -(cam->view[8]), ny = -(cam->view[9]), nz = -(cam->view[10]); /* face camera */
    sprite3d_normalize3(&nx, &ny, &nz, 0.0, 0.0, 1.0);

    rt_mesh3d_add_vertex(mesh,
                         sprite3d_coord_or(cx - rx * hw - ux * hh - origin[0], 0.0),
                         sprite3d_coord_or(cy - ry * hw - uy * hh - origin[1], 0.0),
                         sprite3d_coord_or(cz - rz * hw - uz * hh - origin[2], 0.0),
                         nx,
                         ny,
                         nz,
                         u0,
                         v1);
    rt_mesh3d_add_vertex(mesh,
                         sprite3d_coord_or(cx + rx * hw - ux * hh - origin[0], 0.0),
                         sprite3d_coord_or(cy + ry * hw - uy * hh - origin[1], 0.0),
                         sprite3d_coord_or(cz + rz * hw - uz * hh - origin[2], 0.0),
                         nx,
                         ny,
                         nz,
                         u1,
                         v1);
    rt_mesh3d_add_vertex(mesh,
                         sprite3d_coord_or(cx + rx * hw + ux * hh - origin[0], 0.0),
                         sprite3d_coord_or(cy + ry * hw + uy * hh - origin[1], 0.0),
                         sprite3d_coord_or(cz + rz * hw + uz * hh - origin[2], 0.0),
                         nx,
                         ny,
                         nz,
                         u1,
                         v0);
    rt_mesh3d_add_vertex(mesh,
                         sprite3d_coord_or(cx - rx * hw + ux * hh - origin[0], 0.0),
                         sprite3d_coord_or(cy - ry * hw + uy * hh - origin[1], 0.0),
                         sprite3d_coord_or(cz - rz * hw + uz * hh - origin[2], 0.0),
                         nx,
                         ny,
                         nz,
                         u0,
                         v0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 0, 2, 3);
    {
        rt_mesh3d *built = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
        if (!built || rt_mesh3d_safe_vertex_count(built) != 4u ||
            rt_mesh3d_validated_index_count(built) != 6u) {
            sprite3d_release_mesh_slot(&s->cached_mesh);
            return;
        }
    }

    /* Keep cached runtime objects alive until the deferred frame submission completes. */
    if (!rt_canvas3d_add_temp_object(canvas, mesh))
        return;
    if (!rt_canvas3d_add_temp_object(canvas, s->cached_material))
        return;

    double model[16];
    sprite3d_origin_model_matrix(origin, model);
    rt_canvas3d_draw_mesh_matrix(canvas, mesh, model, s->cached_material);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
