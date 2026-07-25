//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_lighting.c
// Purpose: Canvas3D light flattening — pack the canvas's slotted light array
//   (plus scene lights) into the dense vgfx3d_light_params_t array the backend
//   draw path consumes, applying camera-relative rebasing and value sanitizing.
//   Split out of rt_canvas3d.c; the light slots live on rt_canvas3d.
// Key invariants:
//   - Light slots are sparse (NULL-able) so removal keeps stable indices; the
//     packed output is dense and bounded by the active light limit.
// Links: rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D light sanitization, prioritization, and snapshot revisioning.
/// @details Sparse canvas and scene light slots are flattened into deterministic
///   backend records. The forward-light budget favors globally affecting lights,
///   scores local lights by estimated camera contribution with hysteresis, and
///   stamps byte-stable snapshots so backends can avoid redundant uploads.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_string.h"
#include "vgfx3d_backend.h"

#include <math.h>
#include <string.h>

/// @brief Clamp a Light3D type id to a backend-supported value.
/// @param type Candidate internal light type identifier.
/// @return @p type when it is in the supported range zero through six;
///   otherwise the directional-light identifier zero.
static int32_t canvas3d_sanitize_light_type(int32_t type) {
    return (type >= 0 && type <= 6) ? type : 0;
}

/// @brief Return whether a sanitized light type affects every cluster.
/// @param type Sanitized Light3D type identifier.
/// @return Non-zero for directional and ambient lights; zero for local lights.
static int canvas3d_light_type_is_global(int32_t type) {
    return type == 0 || type == 2;
}

/// @brief Clamp a finite positive area/volume parameter for backend consumption.
/// @param value Width, height, radius, or range value to sanitize.
/// @return A finite positive float; invalid or near-zero input becomes one.
static float canvas3d_sanitize_positive_light_param(double value) {
    return isfinite(value) && value > 1e-6 ? canvas3d_sanitize_f64_to_float(value, 1.0f) : 1.0f;
}

/// @brief Compact one canvas light into a backend param struct (camera-relative rebased,
///        value-sanitized). NULL inputs are ignored.
/// @details The output is cleared before population, positional lights are
///   translated by the active camera-relative origin, unsupported enum values
///   receive safe defaults, and volume lights are forced non-shadowing.
/// @param c Borrowed Canvas3D supplying the optional rebase origin.
/// @param l Borrowed Light3D payload to flatten; `NULL` is ignored.
/// @param out Non-`NULL` backend record overwritten in full.
static void canvas3d_copy_light_params(const rt_canvas3d *c,
                                       const rt_light3d *l,
                                       vgfx3d_light_params_t *out) {
    double origin_x = 0.0;
    double origin_y = 0.0;
    double origin_z = 0.0;
    if (!l || !out)
        return;
    if (canvas3d_uses_camera_relative_upload(c)) {
        origin_x = c->camera_relative_origin[0];
        origin_y = c->camera_relative_origin[1];
        origin_z = c->camera_relative_origin[2];
    }
    memset(out, 0, sizeof(*out));
    out->type = canvas3d_sanitize_light_type(l->type);
    out->shadow_index = -1;
    out->shadow_cascade_count = 1;
    out->shadow_projection_type = VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
    out->casts_shadows = l->casts_shadows ? 1 : 0;
    out->identity = (uintptr_t)l;
    out->direction[0] = canvas3d_sanitize_f64_to_float(l->direction[0], 0.0f);
    out->direction[1] = canvas3d_sanitize_f64_to_float(l->direction[1], -1.0f);
    out->direction[2] = canvas3d_sanitize_f64_to_float(l->direction[2], 0.0f);
    out->position[0] = canvas3d_sanitize_f64_to_float(l->position[0] - origin_x, 0.0f);
    out->position[1] = canvas3d_sanitize_f64_to_float(l->position[1] - origin_y, 0.0f);
    out->position[2] = canvas3d_sanitize_f64_to_float(l->position[2] - origin_z, 0.0f);
    out->basis_u[0] = canvas3d_sanitize_f64_to_float(l->basis_u[0], 1.0f);
    out->basis_u[1] = canvas3d_sanitize_f64_to_float(l->basis_u[1], 0.0f);
    out->basis_u[2] = canvas3d_sanitize_f64_to_float(l->basis_u[2], 0.0f);
    out->basis_v[0] = canvas3d_sanitize_f64_to_float(l->basis_v[0], 0.0f);
    out->basis_v[1] = canvas3d_sanitize_f64_to_float(l->basis_v[1], 1.0f);
    out->basis_v[2] = canvas3d_sanitize_f64_to_float(l->basis_v[2], 0.0f);
    out->color[0] = canvas3d_clamp01_f64(l->color[0]);
    out->color[1] = canvas3d_clamp01_f64(l->color[1]);
    out->color[2] = canvas3d_clamp01_f64(l->color[2]);
    out->intensity = canvas3d_sanitize_nonnegative_f64(l->intensity, 1.0f);
    out->attenuation = canvas3d_sanitize_nonnegative_f64(l->attenuation, 1.0f);
    out->inner_cos = canvas3d_clamp_f64_to_float(l->inner_cos, -1.0, 1.0, 1.0f);
    out->outer_cos = canvas3d_clamp_f64_to_float(l->outer_cos, -1.0, 1.0, 0.0f);
    out->width = canvas3d_sanitize_positive_light_param(l->width);
    out->height = canvas3d_sanitize_positive_light_param(l->height);
    out->radius = canvas3d_sanitize_positive_light_param(l->radius);
    out->range = canvas3d_sanitize_positive_light_param(l->range);
    out->decay_type = l->decay_type >= 0 && l->decay_type <= 3 ? l->decay_type : 2;
    if (out->type == 6) {
        out->casts_shadows = 0;
        out->shadow_index = -1;
    }
}

/// @brief Return the active light payload limit for the selected lighting path.
/// @param c Borrowed Canvas3D whose clustered-lighting setting and backend
///   capability are queried.
/// @return VGFX3D_MAX_LIGHTS for supported clustered lighting; otherwise the
///   forward-renderer limit.
int32_t canvas3d_active_light_limit(rt_canvas3d *c) {
    if (c && c->clustered_lighting) {
        rt_string capability = rt_const_cstr("clustered-lighting");
        int8_t supported = rt_canvas3d_backend_supports(c, capability);
        rt_string_unref(capability);
        if (supported)
            return VGFX3D_MAX_LIGHTS;
    }
    return VGFX3D_FORWARD_LIGHT_LIMIT;
}

/// @brief Relevance score for a local punctual, area, or volume light as seen from the camera.
/// @details Uses the shader's own distance-falloff form so the score approximates the
///   light's strongest possible contribution to visible geometry. Incumbents from the
///   previous flatten receive a small boost so near-ties never swap membership
///   frame-to-frame (whole-scene light popping).
/// @param c Borrowed Canvas3D supplying the cached world-space camera position.
/// @param l Borrowed enabled local Light3D to score.
/// @return Finite non-negative estimated contribution score.
static double canvas3d_local_light_score(const rt_canvas3d *c, const rt_light3d *l) {
    double dx = l->position[0] - (double)c->cached_world_cam_pos[0];
    double dy = l->position[1] - (double)c->cached_world_cam_pos[1];
    double dz = l->position[2] - (double)c->cached_world_cam_pos[2];
    double dist2 = dx * dx + dy * dy + dz * dz;
    double emitter_radius = 0.0;
    double attenuation = isfinite(l->attenuation) && l->attenuation > 0.0 ? l->attenuation : 1.0;
    double intensity = isfinite(l->intensity) && l->intensity > 0.0 ? l->intensity : 0.0;
    double score;
    if (!isfinite(dist2) || dist2 < 0.0)
        dist2 = 0.0;
    if (l->type == 4) {
        double half_width = isfinite(l->width) && l->width > 0.0 ? l->width * 0.5 : 0.5;
        double half_height = isfinite(l->height) && l->height > 0.0 ? l->height * 0.5 : 0.5;
        emitter_radius = sqrt(half_width * half_width + half_height * half_height);
    } else if (l->type == 5 || l->type == 6) {
        emitter_radius = isfinite(l->radius) && l->radius > 0.0 ? l->radius : 1.0;
    }
    if (emitter_radius > 0.0) {
        double dist = sqrt(dist2);
        dist = dist > emitter_radius ? dist - emitter_radius : 0.0;
        dist2 = dist * dist;
    }
    score = intensity / (1.0 + attenuation * dist2);
    return isfinite(score) ? score : 0.0;
}

/// @brief True when @p l was selected by the previous over-budget flatten.
/// @param c Borrowed Canvas3D containing the preceding selected-light identities.
/// @param l Borrowed Light3D identity to find.
/// @return Non-zero when @p l occurs in the bounded previous selection.
static int canvas3d_light_was_selected(const rt_canvas3d *c, const rt_light3d *l) {
    for (int32_t i = 0; i < c->selected_light_id_count && i < VGFX3D_MAX_LIGHTS; i++) {
        if (c->selected_light_ids[i] == (uintptr_t)l)
            return 1;
    }
    return 0;
}

/// @brief Candidate record for over-budget local-light selection.
typedef struct {
    const rt_light3d *light;
    double score;
    int32_t order; /* original slot order, preserved among the selected set */
} canvas3d_light_candidate_t;

/// @brief Flatten the canvas's sparse light array into a dense backend buffer.
/// @details The canvas stores lights in fixed slots (`lights[0..VGFX3D_MAX_LIGHTS]`)
///   so that dropped-and-readded lights keep stable slot identities, but the
///   GPU backends expect a packed array — this routine bridges the two.
///   Plan 07: the output is ordered with directional/ambient lights first (the
///   "global" prefix the clustered shader loops flatly) followed by point/spot
///   lights (looped via per-cluster index lists). Shading is an order-independent
///   sum, so the flat path's output is unchanged by the reorder; within each
///   group the original slot order is preserved so revision stamps stay stable. Native
///   rectangle/sphere/volume lights are finite local lights and therefore join the latter group.
///   When enabled local lights exceed the remaining budget, they are chosen by
///   camera-relative relevance (intensity over the shader's distance falloff) with
///   incumbent hysteresis, instead of arbitrary slot order — slot-order truncation
///   made "which lights render" depend on assignment history and pop scene-wide when
///   counts fluctuated around the cap.
/// @param c Mutable Canvas3D supplying sparse lights, camera state, prior
///   selection, and dropped-light telemetry.
/// @param out Non-`NULL` output array with capacity for at least @p max backend records.
/// @param max Positive output capacity; callers normally use canvas3d_active_light_limit().
/// @return The number of lights actually copied into `out`.
int32_t build_light_params(rt_canvas3d *c, vgfx3d_light_params_t *out, int32_t max) {
    canvas3d_light_candidate_t locals[VGFX3D_MAX_LIGHTS * 2];
    int32_t local_count = 0;
    int32_t count = 0;
    int32_t order = 0;
    if (!c || !out || max <= 0)
        return 0;
    /* Globals (directional/ambient) first, in slot order. */
    for (int i = 0; i < VGFX3D_MAX_LIGHTS && count < max; i++) {
        const rt_light3d *l = c->lights[i];
        if (!l || !l->enabled)
            continue;
        if (!canvas3d_light_type_is_global(canvas3d_sanitize_light_type(l->type)))
            continue;
        canvas3d_copy_light_params(c, l, &out[count]);
        count++;
    }
    for (int i = 0; i < c->scene_light_count && i < VGFX3D_MAX_LIGHTS && count < max; i++) {
        const rt_light3d *l = c->scene_lights[i];
        if (!l || !l->enabled)
            continue;
        if (!canvas3d_light_type_is_global(canvas3d_sanitize_light_type(l->type)))
            continue;
        canvas3d_copy_light_params(c, l, &out[count]);
        count++;
    }
    /* Gather local (point/spot) candidates in slot order. */
    for (int i = 0; i < VGFX3D_MAX_LIGHTS && local_count < VGFX3D_MAX_LIGHTS * 2; i++) {
        const rt_light3d *l = c->lights[i];
        int32_t type;
        if (!l || !l->enabled)
            continue;
        type = canvas3d_sanitize_light_type(l->type);
        if (canvas3d_light_type_is_global(type))
            continue;
        locals[local_count].light = l;
        locals[local_count].order = order++;
        local_count++;
    }
    for (int i = 0;
         i < c->scene_light_count && i < VGFX3D_MAX_LIGHTS && local_count < VGFX3D_MAX_LIGHTS * 2;
         i++) {
        const rt_light3d *l = c->scene_lights[i];
        int32_t type;
        if (!l || !l->enabled)
            continue;
        type = canvas3d_sanitize_light_type(l->type);
        if (canvas3d_light_type_is_global(type))
            continue;
        locals[local_count].light = l;
        locals[local_count].order = order++;
        local_count++;
    }
    if (local_count > max - count) {
        /* Over budget: score every candidate, boost incumbents 10%, then keep the top
         * (max - count) by repeated selection (candidate counts are tiny). Selected
         * lights are emitted in original slot order so the snapshot stays byte-stable
         * whenever the same set wins. */
        int32_t budget = max - count;
        for (int32_t i = 0; i < local_count; i++) {
            locals[i].score = canvas3d_local_light_score(c, locals[i].light);
            if (canvas3d_light_was_selected(c, locals[i].light))
                locals[i].score *= 1.10;
        }
        for (int32_t keep = 0; keep < budget && keep < local_count; keep++) {
            int32_t best = keep;
            for (int32_t j = keep + 1; j < local_count; j++) {
                if (locals[j].score > locals[best].score)
                    best = j;
            }
            if (best != keep) {
                canvas3d_light_candidate_t tmp = locals[keep];
                locals[keep] = locals[best];
                locals[best] = tmp;
            }
        }
        if (budget < local_count)
            local_count = budget > 0 ? budget : 0;
        /* Restore slot order among the survivors. */
        for (int32_t i = 1; i < local_count; i++) {
            canvas3d_light_candidate_t value = locals[i];
            int32_t j = i;
            while (j > 0 && locals[j - 1].order > value.order) {
                locals[j] = locals[j - 1];
                j--;
            }
            locals[j] = value;
        }
    }
    c->selected_light_id_count = 0;
    for (int32_t i = 0; i < local_count && count < max; i++) {
        canvas3d_copy_light_params(c, locals[i].light, &out[count]);
        count++;
        if (c->selected_light_id_count < VGFX3D_MAX_LIGHTS)
            c->selected_light_ids[c->selected_light_id_count++] = (uintptr_t)locals[i].light;
    }
    /* Telemetry: record how many enabled lights the active limit truncated
     * this pass — lights silently exceeding the forward cap were previously
     * unobservable (get_DroppedLightCount). */
    {
        int32_t enabled = 0;
        for (int i = 0; i < VGFX3D_MAX_LIGHTS; i++)
            if (c->lights[i] && c->lights[i]->enabled)
                enabled++;
        for (int i = 0; i < c->scene_light_count && i < VGFX3D_MAX_LIGHTS; i++)
            if (c->scene_lights[i] && c->scene_lights[i]->enabled)
                enabled++;
        c->last_dropped_light_count = enabled > count ? enabled - count : 0;
    }
    return count;
}

#endif /* ZANNA_ENABLE_GRAPHICS */

/// @brief Stamp the current light+ambient snapshot with a monotonic revision.
/// @details Compares @p lights (freshly built, memset-padded entries) and the
///          canvas ambient against the previous snapshot; the revision only
///          advances on a real change. Queued draws record the returned stamp
///          so backends can skip re-uploading scene/light constants across
///          runs of draws that share it. Never returns 0 (0 = "unknown,
///          always upload" in the draw command).
/// @param c Mutable Canvas3D owning the previous snapshot and revision counter.
/// @param lights Borrowed dense array of @p light_count fully initialized records.
/// @param light_count Number of records, clamped to the supported maximum.
/// @return Stable non-zero revision for a valid canvas, incremented only when
///   the light bytes or ambient color change; zero when @p c is `NULL`.
uint32_t canvas3d_stamp_light_snapshot(rt_canvas3d *c,
                                       const vgfx3d_light_params_t *lights,
                                       int32_t light_count) {
    vgfx3d_light_params_t *last;
    size_t bytes;

    if (!c)
        return 0;
    if (light_count < 0)
        light_count = 0;
    if (light_count > VGFX3D_MAX_LIGHTS)
        light_count = VGFX3D_MAX_LIGHTS;
    bytes = (size_t)light_count * sizeof(vgfx3d_light_params_t);
    last = (vgfx3d_light_params_t *)c->last_light_snapshot;
    if (!last) {
        last = (vgfx3d_light_params_t *)calloc(VGFX3D_MAX_LIGHTS, sizeof(*last));
        if (!last) {
            /* Snapshot cache unavailable: force per-draw uploads (correct, slower). */
            c->lights_revision++;
            if (c->lights_revision == 0)
                c->lights_revision = 1;
            return c->lights_revision;
        }
        c->last_light_snapshot = last;
        c->last_light_snapshot_valid = 0;
    }
    if (c->last_light_snapshot_valid && c->last_light_snapshot_count == light_count &&
        memcmp(c->last_light_snapshot_ambient,
               c->ambient,
               sizeof(c->last_light_snapshot_ambient)) == 0 &&
        (bytes == 0 || memcmp(last, lights, bytes) == 0)) {
        if (c->lights_revision == 0)
            c->lights_revision = 1;
        return c->lights_revision;
    }
    if (bytes)
        memcpy(last, lights, bytes);
    c->last_light_snapshot_count = light_count;
    memcpy(c->last_light_snapshot_ambient, c->ambient, sizeof(c->last_light_snapshot_ambient));
    c->last_light_snapshot_valid = 1;
    c->lights_revision++;
    if (c->lights_revision == 0)
        c->lights_revision = 1;
    return c->lights_revision;
}
