//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_reflectionprobe3d.c
// Purpose: Local reflection probes — captured 6-face cubemaps with a box
//   influence volume, completing the reflection chain SSR -> local probe ->
//   skybox IBL. Capture renders the scene through an off-screen RenderTarget3D
//   from the probe position and assembles a CubeMap3D consumable by the
//   existing environment-map machinery.
// Key invariants:
//   - Capture is explicit/scripted, never per-frame; CaptureDirty flags
//     re-capture requests (time-of-day hooks set it).
// Ownership/Lifetime:
//   - GC-managed; the probe retains its captured cubemap until finalized.
// Links: ADR 0089, rt_reflectionprobe3d.h.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements local ReflectionProbe3D volumes and explicit cubemap capture.
/// @details A probe normalizes its proxy bounds, tracks capture configuration and dirtiness, and
///   retains the most recent six-face HDR cubemap assembled from scripted scene renders.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_reflectionprobe3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_scene3d.h"
#include "rt_trap.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern int64_t rt_obj_class_id(void *obj);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_vec3_new(double x, double y, double z);
extern void *rt_rendertarget3d_as_pixels(void *obj);
extern void *rt_camera3d_new(double fov_deg, double aspect, double near_val, double far_val);

#define REFLECTIONPROBE3D_COORD_ABS_MAX 1.0e12

typedef struct rt_reflectionprobe3d {
    void *vptr;
    double position[3];
    double box_min[3];
    double box_max[3];
    double influence_scale;
    int64_t resolution;
    int8_t capture_dirty;
    void *cubemap; /* retained CubeMap3D from the last capture */
} rt_reflectionprobe3d;

/// @brief Read a runtime Vec3 into a native three-element array.
/// @param v Candidate Vec3 runtime object.
/// @param[out] out Destination array populated on successful validation.
/// @return 1 when @p v is a Vec3, or 0 without modifying the caller-visible contract otherwise.
static int probe_read_vec3(void *v, double out[3]) {
    if (!v || rt_obj_class_id(v) != RT_VEC3_CLASS_ID)
        return 0;
    out[0] = rt_vec3_x(v);
    out[1] = rt_vec3_y(v);
    out[2] = rt_vec3_z(v);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

/// @brief Clamp one finite authored coordinate to the shared stable world range.
/// @param value Finite coordinate read from a validated Vec3.
/// @return Coordinate in `[-REFLECTIONPROBE3D_COORD_ABS_MAX, +max]`.
static double probe_clamp_coord(double value) {
    if (value > REFLECTIONPROBE3D_COORD_ABS_MAX)
        return REFLECTIONPROBE3D_COORD_ABS_MAX;
    if (value < -REFLECTIONPROBE3D_COORD_ABS_MAX)
        return -REFLECTIONPROBE3D_COORD_ABS_MAX;
    return value;
}

/// @brief Release a retained cubemap slot without operating on a wrong-class pointer.
/// @param[in,out] slot Probe cubemap ownership slot.
static void probe_release_cubemap(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release the cubemap retained by a finalized reflection probe.
/// @param obj ReflectionProbe3D object being finalized; ignored when `NULL`.
static void reflectionprobe3d_finalize(void *obj) {
    rt_reflectionprobe3d *probe = (rt_reflectionprobe3d *)obj;
    if (probe)
        probe_release_cubemap(&probe->cubemap);
}

/// @brief Create a reflection probe with normalized axis-aligned influence bounds.
/// @param position Vec3 capture origin.
/// @param box_min Vec3 containing one corner of the proxy box.
/// @param box_max Vec3 containing the opposite corner; per-axis ordering is normalized.
/// @return A new GC-managed probe initialized dirty at 64-pixel face resolution, or `NULL` after
///   trapping on invalid vectors or allocation failure.
void *rt_reflectionprobe3d_new(void *position, void *box_min, void *box_max) {
    double pos[3], bmin[3], bmax[3];
    if (!probe_read_vec3(position, pos) || !probe_read_vec3(box_min, bmin) ||
        !probe_read_vec3(box_max, bmax)) {
        rt_trap("ReflectionProbe3D.New: position and box bounds must be Vec3");
        return NULL;
    }
    rt_reflectionprobe3d *probe = (rt_reflectionprobe3d *)rt_obj_new_i64(
        RT_G3D_REFLECTIONPROBE3D_CLASS_ID, (int64_t)sizeof(rt_reflectionprobe3d));
    if (!probe) {
        rt_trap("ReflectionProbe3D.New: allocation failed");
        return NULL;
    }
    memset(probe, 0, sizeof(*probe));
    rt_obj_set_finalizer(probe, reflectionprobe3d_finalize);
    for (int a = 0; a < 3; ++a) {
        pos[a] = probe_clamp_coord(pos[a]);
        bmin[a] = probe_clamp_coord(bmin[a]);
        bmax[a] = probe_clamp_coord(bmax[a]);
    }
    memcpy(probe->position, pos, sizeof(pos));
    for (int a = 0; a < 3; ++a) {
        probe->box_min[a] = bmin[a] < bmax[a] ? bmin[a] : bmax[a];
        probe->box_max[a] = bmin[a] < bmax[a] ? bmax[a] : bmin[a];
    }
    probe->influence_scale = 1.0;
    probe->resolution = 64;
    probe->capture_dirty = 1;
    return probe;
}

/// @brief Validate a ReflectionProbe3D handle and trap with an API-specific message on failure.
/// @param obj Candidate runtime object.
/// @param method Trap message identifying the calling API.
/// @return The typed probe pointer, or `NULL` after reporting invalid input.
static rt_reflectionprobe3d *reflectionprobe3d_checked(void *obj, const char *method) {
    rt_reflectionprobe3d *probe =
        (rt_reflectionprobe3d *)rt_g3d_checked_or_null(obj, RT_G3D_REFLECTIONPROBE3D_CLASS_ID);
    if (!probe)
        rt_trap(method);
    return probe;
}

/// @brief Return a copy of the probe's capture origin.
/// @param obj ReflectionProbe3D instance.
/// @return A new Vec3 containing the capture position, or `NULL` for an invalid probe.
void *rt_reflectionprobe3d_get_position(void *obj) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.get_Position: invalid probe");
    if (!probe)
        return NULL;
    return rt_vec3_new(probe->position[0], probe->position[1], probe->position[2]);
}

/// @brief Set the factor used to expand the proxy box around its center.
/// @param obj ReflectionProbe3D instance.
/// @param scale Finite scale in the supported range from one through eight; smaller or
///   non-finite values are ignored and larger values are clamped.
void rt_reflectionprobe3d_set_influence_scale(void *obj, double scale) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.set_InfluenceScale: invalid probe");
    if (probe && isfinite(scale) && scale >= 1.0)
        probe->influence_scale = scale > 8.0 ? 8.0 : scale;
}

/// @brief Return the current proxy-box influence scale.
/// @param obj ReflectionProbe3D instance.
/// @return The scale factor, or zero for an invalid probe.
double rt_reflectionprobe3d_get_influence_scale(void *obj) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.get_InfluenceScale: invalid probe");
    return probe ? probe->influence_scale : 0.0;
}

/// @brief Set the square cubemap face resolution.
/// @param obj ReflectionProbe3D instance.
/// @param resolution Requested width and height, clamped from 16 through 512 pixels.
void rt_reflectionprobe3d_set_resolution(void *obj, int64_t resolution) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.set_Resolution: invalid probe");
    if (!probe)
        return;
    if (resolution < 16)
        resolution = 16;
    if (resolution > 512)
        resolution = 512;
    if (probe->resolution != resolution) {
        probe->resolution = resolution;
        probe->capture_dirty = 1;
    }
}

/// @brief Return the configured cubemap face resolution.
/// @param obj ReflectionProbe3D instance.
/// @return The resolution in pixels, or zero for an invalid probe.
int64_t rt_reflectionprobe3d_get_resolution(void *obj) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.get_Resolution: invalid probe");
    return probe ? probe->resolution : 0;
}

/// @brief Mark whether the probe needs an explicit recapture.
/// @param obj ReflectionProbe3D instance.
/// @param dirty Non-zero to mark the capture stale, or zero to mark it current.
void rt_reflectionprobe3d_set_capture_dirty(void *obj, int8_t dirty) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.set_CaptureDirty: invalid probe");
    if (probe)
        probe->capture_dirty = dirty ? 1 : 0;
}

/// @brief Return whether the probe is awaiting recapture.
/// @param obj ReflectionProbe3D instance.
/// @return 1 when dirty, or 0 when current or invalid.
int8_t rt_reflectionprobe3d_get_capture_dirty(void *obj) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.get_CaptureDirty: invalid probe");
    return probe ? probe->capture_dirty : 0;
}

/// @brief True when @p position lies inside the influence-scaled proxy box.
/// @param obj ReflectionProbe3D instance.
/// @param position Vec3 point tested against the expanded bounds.
/// @return 1 when the point is inside every inclusive axis interval, or 0 otherwise.
int8_t rt_reflectionprobe3d_contains(void *obj, void *position) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.Contains: invalid probe");
    double pos[3];
    if (!probe || !probe_read_vec3(position, pos))
        return 0;
    for (int a = 0; a < 3; ++a) {
        double scale =
            fmax(1.0, fmax(fabs(pos[a]), fmax(fabs(probe->box_min[a]), fabs(probe->box_max[a]))));
        double lo = probe->box_min[a] / scale;
        double hi = probe->box_max[a] / scale;
        double point = pos[a] / scale;
        double center = lo * 0.5 + hi * 0.5;
        double half = (hi * 0.5 - lo * 0.5) * probe->influence_scale;
        if (!isfinite(lo) || !isfinite(hi) || !isfinite(point) || !isfinite(half) ||
            point < center - half || point > center + half)
            return 0;
    }
    return 1;
}

/// @brief Retained captured cubemap (NULL before the first capture).
/// @param obj ReflectionProbe3D instance.
/// @return The captured CubeMap3D with a new retained reference for the caller, or `NULL` when no
///   capture exists.
void *rt_reflectionprobe3d_get_cubemap(void *obj) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.get_Cubemap: invalid probe");
    if (!probe || !probe->cubemap)
        return NULL;
    if (!rt_cubemap3d_is_complete(probe->cubemap)) {
        probe_release_cubemap(&probe->cubemap);
        probe->capture_dirty = 1;
        return NULL;
    }
    rt_obj_retain_maybe(probe->cubemap);
    return probe->cubemap;
}

/// @brief Capture 6 faces of @p scene from the probe position through @p canvas.
/// @details Face order and orientations follow the CubeMap3D.New contract
///   (+X, -X, +Y, -Y, +Z, -Z). Explicit/scripted only — a capture re-renders the
///   scene six times. Clears CaptureDirty on success.
/// @param obj ReflectionProbe3D receiving the captured cubemap.
/// @param canvas Canvas3D used for six off-screen HDR renders.
/// @param scene Scene3D rendered from each cubemap orientation.
/// @return 1 after replacing the retained cubemap and clearing the dirty flag, or 0 when setup,
///   rendering, readback, or cubemap construction fails.
int8_t rt_reflectionprobe3d_capture(void *obj, void *canvas, void *scene) {
    rt_reflectionprobe3d *probe =
        reflectionprobe3d_checked(obj, "ReflectionProbe3D.Capture: invalid probe");
    rt_canvas3d *canvas_impl;
    void *previous_target = NULL;
    void *captured_cubemap = NULL;
    int target_bound = 0;
    if (!probe)
        return 0;
    /* Capture only becomes current after all six faces and target restoration commit. */
    probe->capture_dirty = 1;
    if (!rt_g3d_has_class(canvas, RT_G3D_CANVAS3D_CLASS_ID) ||
        !rt_g3d_has_class(scene, RT_G3D_SCENE3D_CLASS_ID))
        return 0;
    canvas_impl = (rt_canvas3d *)canvas;
    if (canvas_impl->in_frame)
        return 0;
    if (canvas_impl->render_target_owner) {
        rt_rendertarget3d *previous;
        if (!rt_g3d_has_class(canvas_impl->render_target_owner, RT_G3D_RENDERTARGET3D_CLASS_ID))
            return 0;
        previous = canvas_impl->render_target_owner;
        if (!previous->target || canvas_impl->render_target != previous->target ||
            previous->width != previous->target->width ||
            previous->height != previous->target->height ||
            !vgfx3d_rendertarget_valid_pixels(previous->target, NULL))
            return 0;
        previous_target = previous;
    } else if (canvas_impl->offscreen || canvas_impl->render_target) {
        return 0;
    }
    if (probe->resolution < 16)
        probe->resolution = 16;
    if (probe->resolution > 512)
        probe->resolution = 512;
    for (int lane = 0; lane < 3; ++lane) {
        if (!isfinite(probe->position[lane])) {
            probe->capture_dirty = 1;
            return 0;
        }
        probe->position[lane] = probe_clamp_coord(probe->position[lane]);
    }
    static const double face_fwd[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const double face_up[6][3] = {
        {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    void *faces[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    /* HDR capture: the reflection chain is SSR -> local probe -> skybox IBL,
     * and the sky/IBL terms carry HDR range. An LDR (UNORM8) face target
     * clamped bright reflections to [0,1], making probe reflections dimmer
     * than the sky fallback they blend with. */
    void *target = rt_rendertarget3d_new_hdr(probe->resolution, probe->resolution);
    void *camera = rt_camera3d_new(90.0, 1.0, 0.05, 10000.0);
    int ok = target && camera;
    if (ok && previous_target)
        rt_obj_retain_maybe(previous_target);
    else if (!ok)
        previous_target = NULL;
    if (ok) {
        rt_canvas3d_set_render_target(canvas, target);
        target_bound = canvas_impl->render_target_owner == target;
        ok = target_bound;
    }
    for (int f = 0; f < 6 && ok; ++f) {
        rt_camera3d_look_at_components(camera,
                                       probe->position[0],
                                       probe->position[1],
                                       probe->position[2],
                                       probe->position[0] + face_fwd[f][0],
                                       probe->position[1] + face_fwd[f][1],
                                       probe->position[2] + face_fwd[f][2],
                                       face_up[f][0],
                                       face_up[f][1],
                                       face_up[f][2]);
        rt_scene3d_draw(scene, canvas, camera);
        faces[f] = rt_rendertarget3d_as_pixels(target);
        if (!faces[f])
            ok = 0;
    }
    if (ok) {
        captured_cubemap =
            rt_cubemap3d_new(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
        if (!captured_cubemap)
            ok = 0;
    }
    for (int f = 0; f < 6; ++f) {
        if (faces[f] && rt_obj_release_check0(faces[f]))
            rt_obj_free(faces[f]);
    }
    if (target_bound) {
        if (previous_target)
            rt_canvas3d_set_render_target(canvas, previous_target);
        else
            rt_canvas3d_reset_render_target(canvas);
        if ((previous_target && canvas_impl->render_target_owner != previous_target) ||
            (!previous_target && canvas_impl->render_target_owner))
            ok = 0;
    }
    if (previous_target && rt_obj_release_check0(previous_target))
        rt_obj_free(previous_target);
    if (ok && captured_cubemap) {
        probe_release_cubemap(&probe->cubemap);
        probe->cubemap = captured_cubemap;
        captured_cubemap = NULL;
        probe->capture_dirty = 0;
    } else {
        probe->capture_dirty = 1;
    }
    if (captured_cubemap && rt_obj_release_check0(captured_cubemap))
        rt_obj_free(captured_cubemap);
    if (target && rt_obj_release_check0(target))
        rt_obj_free(target);
    if (camera && rt_obj_release_check0(camera))
        rt_obj_free(camera);
    return ok ? 1 : 0;
}

#else
typedef int rt_reflectionprobe3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
