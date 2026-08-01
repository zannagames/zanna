//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_lensflare3d.c
// Purpose: LensFlare3D — occlusion-aware ghost-chain lens flares. Each element
//   is a pre-tinted radial-falloff disc drawn in overlay space along the axis
//   from the light's projected screen position through screen center.
//
// Key invariants:
//   - Element ghost sprites are generated once at AddElement time (32x32
//     radial-alpha discs tinted by the element color) and retained.
//   - Occlusion probes depth in a 3x3 around the light's pixel: CPU depth
//     (software zbuf or the bound render target's depth) is read directly, and
//     GPU backends answer through the async scene-depth-probe hooks (previous
//     completed frame, no pipeline stall). Visibility is the unoccluded probe
//     fraction, temporally smoothed per flare so occlusion transitions fade
//     instead of popping in visibility-ninth steps.
//   - Lights behind the camera or projecting far off-screen draw nothing.
//
// Ownership/Lifetime:
//   - LensFlare3D is GC-managed; the finalizer releases the bound Light3D and
//     every element's ghost Pixels.
//
// Links: rt_lensflare3d.h, rt_canvas3d_overlay.c (image queue),
//   misc/plans/fps/07-visual-polish.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements occlusion-aware LensFlare3D ghost-chain rendering.
/// @details Flare objects retain a light and prebuilt radial sprites, project
///   the light into screen space, estimate visibility from CPU or asynchronous
///   GPU depth probes, smooth it temporally, and queue scaled overlay ghosts.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_lensflare3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_pixels_internal.h"
#include "vgfx3d_backend.h"

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
extern void *rt_pixels_new(int64_t width, int64_t height);

#define LENSFLARE3D_MAX_ELEMENTS 16
#define LENSFLARE3D_GHOST_SIZE 32

typedef struct {
    float axis_offset; /* 0 = at the light, 1 = screen center, 2 = mirrored */
    float size;        /* base sprite size in pixels at draw time */
    void *ghost;       /* pre-tinted radial disc Pixels (retained) */
} lensflare3d_element_t;

typedef struct {
    void *vptr;
    void *light; /* retained Light3D */
    lensflare3d_element_t elements[LENSFLARE3D_MAX_ELEMENTS];
    int32_t element_count;
    /* Temporally smoothed visibility: raw probe visibility is quantized (ninths) and
     * the GPU readback carries one frame of latency, so blending toward the raw value
     * hides both instead of letting the flare pop at occluder edges. Negative = unset. */
    float smoothed_visibility;
    uint64_t smoothed_frame_serial;
} rt_lensflare3d;

/// @brief Resolve a validated LensFlare3D payload.
/// @param obj Candidate runtime object.
/// @return Borrowed flare implementation, or `NULL` for an invalid handle.
static rt_lensflare3d *lensflare3d_checked(void *obj) {
    return (rt_lensflare3d *)rt_g3d_checked_or_null(obj, RT_G3D_LENSFLARE3D_CLASS_ID);
}

/// @brief Release a retained Light3D slot, clearing wrong-class corruption as unowned.
/// @param slot Address of the flare's retained light slot.
static void lensflare3d_release_light_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_LIGHT3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return whether a ghost is the exact writable procedural Pixels layout expected here.
/// @param ghost Candidate retained Pixels handle.
/// @return Nonzero for a live 32x32 Pixels object with writable storage.
static int lensflare3d_ghost_valid(void *ghost) {
    rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(ghost);
    return pixels && pixels->data && pixels->width == LENSFLARE3D_GHOST_SIZE &&
           pixels->height == LENSFLARE3D_GHOST_SIZE;
}

/// @brief Release a retained ghost slot, clearing wrong-class corruption as unowned.
/// @param slot Address of one element's retained ghost slot.
static void lensflare3d_release_ghost_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_class_id(*slot) != RT_PIXELS_CLASS_ID) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Clamp the mutable element count to the inline array's safe range.
/// @param lf Flare whose count is repaired; `NULL` is accepted.
/// @return Repaired count in `[0, LENSFLARE3D_MAX_ELEMENTS]`.
static int32_t lensflare3d_safe_element_count(rt_lensflare3d *lf) {
    if (!lf)
        return 0;
    if (lf->element_count < 0)
        lf->element_count = 0;
    if (lf->element_count > LENSFLARE3D_MAX_ELEMENTS)
        lf->element_count = LENSFLARE3D_MAX_ELEMENTS;
    return lf->element_count;
}

/// @brief GC finalizer — release the bound light and every ghost sprite.
/// @param obj LensFlare3D payload being finalized; `NULL` is ignored.
static void lensflare3d_finalize(void *obj) {
    rt_lensflare3d *lf = (rt_lensflare3d *)obj;
    if (!lf)
        return;
    lensflare3d_release_light_slot(&lf->light);
    /* Walk the complete inline array rather than trusting mutable count metadata. A
     * damaged count or a sparse private table must not leak retained Pixels. */
    for (int32_t i = 0; i < LENSFLARE3D_MAX_ELEMENTS; i++)
        lensflare3d_release_ghost_slot(&lf->elements[i].ghost);
    lf->element_count = 0;
}

/// @brief Create a lens flare bound to @p light (retained).
/// @param light Live Light3D whose placement and enabled state drive the flare.
/// @return New GC-managed empty flare retaining @p light, or `NULL` after
///   reporting invalid input or allocation failure.
void *rt_lensflare3d_new(void *light) {
    if (!light || !rt_g3d_has_class(light, RT_G3D_LIGHT3D_CLASS_ID)) {
        rt_trap("LensFlare3D.New: flare must bind a Light3D");
        return NULL;
    }
    rt_lensflare3d *lf = (rt_lensflare3d *)rt_obj_new_i64(RT_G3D_LENSFLARE3D_CLASS_ID,
                                                          (int64_t)sizeof(rt_lensflare3d));
    if (!lf) {
        rt_trap("LensFlare3D.New: allocation failed");
        return NULL;
    }
    lf->vptr = NULL;
    lf->light = light;
    rt_obj_retain_maybe(light);
    lf->element_count = 0;
    lf->smoothed_visibility = -1.0f;
    lf->smoothed_frame_serial = 0;
    rt_obj_set_finalizer(lf, lensflare3d_finalize);
    return lf;
}

/// @brief Build a 32x32 radial-falloff disc tinted by (r,g,b): the classic ghost.
/// @param r Normalized red tint.
/// @param g Normalized green tint.
/// @param b Normalized blue tint.
/// @return Newly created GC-managed Pixels handle, possibly lacking writable
///   backing storage if Pixels construction could not fully initialize, or `NULL`.
static void *lensflare3d_make_ghost(double r, double g, double b) {
    void *pixels = rt_pixels_new(LENSFLARE3D_GHOST_SIZE, LENSFLARE3D_GHOST_SIZE);
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    if (!pv || !pv->data || pv->width != LENSFLARE3D_GHOST_SIZE ||
        pv->height != LENSFLARE3D_GHOST_SIZE) {
        if (pixels && rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }
    const float half = (float)LENSFLARE3D_GHOST_SIZE * 0.5f;
    uint32_t rr = (uint32_t)(r * 255.0);
    uint32_t gg = (uint32_t)(g * 255.0);
    uint32_t bb = (uint32_t)(b * 255.0);
    for (int32_t y = 0; y < LENSFLARE3D_GHOST_SIZE; y++) {
        for (int32_t x = 0; x < LENSFLARE3D_GHOST_SIZE; x++) {
            float dx = ((float)x + 0.5f - half) / half;
            float dy = ((float)y + 0.5f - half) / half;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 1.0f - d;
            if (a <= 0.0f)
                continue;
            /* Soft ring look: quadratic falloff with a slightly bright rim. */
            a = a * a * (0.55f + 0.45f * d);
            uint32_t alpha = (uint32_t)(a * 255.0f);
            pv->data[(size_t)y * (size_t)LENSFLARE3D_GHOST_SIZE + (size_t)x] =
                (rr << 24) | (gg << 16) | (bb << 8) | alpha;
        }
    }
    pixels_touch(pv);
    return pixels;
}

/// @brief Add a ghost element along the light->center axis.
/// @details Stores at most sixteen elements. The axis offset is clamped to
///   `[-1, 2]`, invalid size falls back to 32 pixels and is capped at 1024,
///   and the packed RGB tint is baked into a newly owned radial sprite.
/// @param obj LensFlare3D receiver; invalid handles are ignored.
/// @param axis_offset Position along the projected light-to-screen-center axis.
/// @param size Base square sprite dimension in output pixels.
/// @param color_rgb Packed `0xRRGGBB` tint.
/// @param rotation Reserved for API stability; circular ghosts ignore it.
void rt_lensflare3d_add_element(
    void *obj, double axis_offset, double size, int64_t color_rgb, double rotation) {
    (void)rotation; /* circular ghosts are rotation-invariant; kept for API stability */
    rt_lensflare3d *lf = lensflare3d_checked(obj);
    if (!lf)
        return;
    if (lensflare3d_safe_element_count(lf) >= LENSFLARE3D_MAX_ELEMENTS)
        return;
    if (!isfinite(axis_offset))
        axis_offset = 0.0;
    if (axis_offset < -1.0)
        axis_offset = -1.0;
    if (axis_offset > 2.0)
        axis_offset = 2.0;
    if (!isfinite(size) || size <= 0.0)
        size = 32.0;
    if (size > 1024.0)
        size = 1024.0;
    uint64_t packed_rgb = (uint64_t)color_rgb;
    double r = (double)((packed_rgb >> 16) & UINT64_C(0xFF)) / 255.0;
    double g = (double)((packed_rgb >> 8) & UINT64_C(0xFF)) / 255.0;
    double b = (double)(packed_rgb & UINT64_C(0xFF)) / 255.0;
    void *ghost = lensflare3d_make_ghost(r, g, b);
    if (!ghost)
        return;
    lensflare3d_element_t *e = &lf->elements[lf->element_count++];
    e->axis_offset = (float)axis_offset;
    e->size = (float)size;
    e->ghost = ghost; /* rt_pixels_new returned an owned reference */
}

/// @brief Fraction of 3x3 depth probes around (px,py) that see past the light.
/// @details CPU depth (software z-buffer or a bound render target) is sampled directly.
///   GPU backends answer through the async scene-depth-probe hooks: probes queued this
///   frame are read back by the backend without stalling, and reads return the previous
///   completed frame's depth — the caller's temporal smoothing absorbs that latency.
/// @param c Canvas supplying depth storage or backend probe hooks.
/// @param px Projected light X position in output pixels.
/// @param py Projected light Y position in output pixels.
/// @param light_ndc_z Light depth in active NDC convention.
/// @param w Positive output width used to convert GPU probes to NDC.
/// @param h Positive output height used to convert GPU probes to NDC.
/// @return Fraction of valid probes considered unoccluded in `[0, 1]`; returns
///   one when no usable depth source or probe sample exists.
static float lensflare3d_visibility(
    rt_canvas3d *c, float px, float py, float light_ndc_z, int32_t w, int32_t h) {
    const float *depth = NULL;
    int32_t dw = 0;
    int32_t dh = 0;
    if (!c || !isfinite(px) || !isfinite(py) || !isfinite(light_ndc_z) || w <= 0 || h <= 0)
        return 0.0f;
    if (c->render_target && c->render_target->depth_buf &&
        vgfx3d_rendertarget_valid_pixels(c->render_target, NULL)) {
        depth = c->render_target->depth_buf;
        dw = c->render_target->width;
        dh = c->render_target->height;
    } else if (c->backend == &vgfx3d_software_backend) {
        depth = vgfx3d_sw_get_zbuf(c->backend_ctx, &dw, &dh);
    }
    if (!depth || dw <= 0 || dh <= 0) {
        /* GPU backends: previous-frame async depth probes. Probes always report
         * canonical window depth (larger = farther); under a reversed-Z projection the
         * light's NDC z maps to canonical depth with the opposite sign. */
        if (c->backend && c->backend->queue_depth_probe && c->backend->read_depth_probe &&
            c->backend_ctx && w > 0 && h > 0) {
            float ref =
                c->backend->reversed_z ? 0.5f - light_ndc_z * 0.5f : light_ndc_z * 0.5f + 0.5f;
            if (!isfinite(ref))
                return 1.0f;
            if (ref < 0.0f)
                ref = 0.0f;
            if (ref > 1.0f)
                ref = 1.0f;
            int visible = 0;
            int total = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    float sx = px + (float)dx;
                    float sy = py + (float)dy;
                    float ndc_x = (sx + 0.5f) / (float)w * 2.0f - 1.0f;
                    float ndc_y = 1.0f - (sy + 0.5f) / (float)h * 2.0f;
                    int32_t slot;
                    float d;
                    if (ndc_x < -1.0f || ndc_x > 1.0f || ndc_y < -1.0f || ndc_y > 1.0f)
                        continue;
                    slot = c->backend->queue_depth_probe(c->backend_ctx, ndc_x, ndc_y);
                    if (slot < 0)
                        continue;
                    total++;
                    d = c->backend->read_depth_probe(c->backend_ctx, slot);
                    /* No result yet (first frames): count as visible; smoothing hides
                     * the transient. */
                    if (!isfinite(d) || d < 0.0f || d >= ref - 1e-4f)
                        visible++;
                }
            }
            return total > 0 ? (float)visible / (float)total : 1.0f;
        }
        return 1.0f; /* no depth source at all: draw unoccluded */
    }
    if (px < (float)INT32_MIN + 2.0f || px > (float)INT32_MAX - 2.0f ||
        py < (float)INT32_MIN + 2.0f || py > (float)INT32_MAX - 2.0f)
        return 0.0f;
    int visible = 0;
    int total = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int32_t sx = (int32_t)px + dx;
            int32_t sy = (int32_t)py + dy;
            if (sx < 0 || sy < 0 || sx >= dw || sy >= dh)
                continue;
            total++;
            float d = depth[(size_t)sy * (size_t)dw + (size_t)sx];
            if (!isfinite(d) || d > 1.0f || d >= light_ndc_z - 1e-4f)
                visible++;
        }
    }
    return total > 0 ? (float)visible / (float)total : 1.0f;
}

/// @brief Draw the flare ghosts into the canvas overlay (call after End).
/// @details Projects positional lights directly and directional lights as a
///   distant reverse-direction sun, rejects disabled, behind-camera, and far
///   off-screen sources, smooths sampled visibility once per frame, then places
///   every ghost along the light-to-center axis. Nearly invisible flares queue nothing.
/// @param canvas Canvas3D receiver whose completed scene state and overlay queue are used.
/// @param flare LensFlare3D receiver borrowed for projection and sprite submission.
void rt_canvas3d_draw_lens_flare(void *canvas, void *flare) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    rt_lensflare3d *lf = lensflare3d_checked(flare);
    const float *vp;
    int32_t w;
    int32_t h;
    double light_screen[3];
    double light_world[3];
    int32_t element_count;

    if (!c || !lf)
        return;
    if (lf->light && !rt_g3d_has_class(lf->light, RT_G3D_LIGHT3D_CLASS_ID))
        lensflare3d_release_light_slot(&lf->light);
    element_count = lensflare3d_safe_element_count(lf);
    if (!lf->light || element_count <= 0)
        return;
    const rt_light3d *l =
        (const rt_light3d *)rt_g3d_checked_or_null(lf->light, RT_G3D_LIGHT3D_CLASS_ID);
    if (!l || !l->enabled)
        return;
    vp = canvas3d_active_scene_vp(c);
    if (!vp)
        return;
    w = (int32_t)rt_canvas3d_get_width(canvas);
    h = (int32_t)rt_canvas3d_get_height(canvas);
    if (w <= 0 || h <= 0)
        return;
    if (l->type == 0) {
        /* Directional: place the "sun" far along the reverse light direction. */
        double direction[3] = {l->direction[0], l->direction[1], l->direction[2]};
        double max_component =
            fmax(fabs(direction[0]), fmax(fabs(direction[1]), fabs(direction[2])));
        if (!isfinite(max_component) || max_component < 1e-12)
            return;
        direction[0] /= max_component;
        direction[1] /= max_component;
        direction[2] /= max_component;
        double len = hypot(direction[0], hypot(direction[1], direction[2]));
        if (!isfinite(len) || len < 1e-12)
            return;
        for (int axis = 0; axis < 3; axis++) {
            if (!isfinite(c->cached_render_cam_pos[axis]))
                return;
            light_world[axis] =
                (double)c->cached_render_cam_pos[axis] - direction[axis] / len * 10000.0;
        }
    } else {
        for (int axis = 0; axis < 3; axis++) {
            double origin =
                canvas3d_uses_camera_relative_upload(c) ? c->camera_relative_origin[axis] : 0.0;
            if (!isfinite(l->position[axis]) || !isfinite(origin))
                return;
            light_world[axis] = l->position[axis] - origin;
            if (!isfinite(light_world[axis]))
                return;
        }
    }
    {
        double cx =
            vp[0] * light_world[0] + vp[1] * light_world[1] + vp[2] * light_world[2] + vp[3];
        double cy =
            vp[4] * light_world[0] + vp[5] * light_world[1] + vp[6] * light_world[2] + vp[7];
        double cz =
            vp[8] * light_world[0] + vp[9] * light_world[1] + vp[10] * light_world[2] + vp[11];
        double cw =
            vp[12] * light_world[0] + vp[13] * light_world[1] + vp[14] * light_world[2] + vp[15];
        if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) || !isfinite(cw) || cw <= 1e-12)
            return; /* behind the camera */
        light_screen[0] = (cx / cw * 0.5 + 0.5) * (double)w;
        light_screen[1] = (1.0 - cy / cw) * 0.5 * (double)h;
        light_screen[2] = cz / cw;
        if (!isfinite(light_screen[0]) || !isfinite(light_screen[1]) ||
            !isfinite(light_screen[2]) || light_screen[0] < -(double)w ||
            light_screen[0] > 2.0 * (double)w || light_screen[1] < -(double)h ||
            light_screen[1] > 2.0 * (double)h ||
            (l->type != 0 && (light_screen[2] < -1.0 || light_screen[2] > 1.0)))
            return;
    }
    float visibility = lensflare3d_visibility(
        c, (float)light_screen[0], (float)light_screen[1], (float)light_screen[2], w, h);
    if (!isfinite(visibility))
        visibility = 0.0f;
    if (visibility < 0.0f)
        visibility = 0.0f;
    if (visibility > 1.0f)
        visibility = 1.0f;
    {
        /* Temporal smoothing: raw visibility quantizes to ninths and the GPU probe
         * readback is a frame late, so blend toward the raw value instead of snapping.
         * A gap in draws (light off-screen, flare disabled) resets to the raw value. */
        uint64_t serial = c->frame_serial;
        if (!isfinite(lf->smoothed_visibility) || lf->smoothed_visibility < 0.0f ||
            serial - lf->smoothed_frame_serial > UINT64_C(4)) {
            lf->smoothed_visibility = visibility;
        } else if (serial != lf->smoothed_frame_serial) {
            lf->smoothed_visibility += (visibility - lf->smoothed_visibility) * 0.2f;
        }
        if (lf->smoothed_visibility < 0.0f)
            lf->smoothed_visibility = 0.0f;
        if (lf->smoothed_visibility > 1.0f)
            lf->smoothed_visibility = 1.0f;
        lf->smoothed_frame_serial = serial;
        visibility = lf->smoothed_visibility;
    }
    if (visibility <= 0.01f)
        return;
    {
        double center_x = (double)w * 0.5;
        double center_y = (double)h * 0.5;
        double axis_x = center_x - light_screen[0];
        double axis_y = center_y - light_screen[1];
        for (int32_t i = 0; i < element_count; i++) {
            lensflare3d_element_t *e = &lf->elements[i];
            if (!e->ghost)
                continue;
            if (!lensflare3d_ghost_valid(e->ghost)) {
                lensflare3d_release_ghost_slot(&e->ghost);
                continue;
            }
            double axis_offset = isfinite(e->axis_offset) ? e->axis_offset : 0.0;
            double base_size = isfinite(e->size) && e->size > 0.0f ? e->size : 32.0;
            if (axis_offset < -1.0)
                axis_offset = -1.0;
            if (axis_offset > 2.0)
                axis_offset = 2.0;
            if (base_size > 1024.0)
                base_size = 1024.0;
            e->axis_offset = (float)axis_offset;
            e->size = (float)base_size;
            double ex = light_screen[0] + axis_x * axis_offset;
            double ey = light_screen[1] + axis_y * axis_offset;
            double sz = base_size * (0.5 + 0.5 * (double)visibility);
            if (!isfinite(ex) || !isfinite(ey) || !isfinite(sz) || sz < 1.0)
                continue;
            rt_canvas3d_draw_image2d_region(canvas,
                                            (int64_t)(ex - sz * 0.5f),
                                            (int64_t)(ey - sz * 0.5f),
                                            (int64_t)sz,
                                            (int64_t)sz,
                                            e->ghost,
                                            0,
                                            0,
                                            LENSFLARE3D_GHOST_SIZE,
                                            LENSFLARE3D_GHOST_SIZE);
        }
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
