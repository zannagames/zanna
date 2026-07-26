//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_cull.inc
// Purpose: Scene3D mesh/LOD resolution, frustum and PVS culling, spatial-index
//   candidate filtering, and deterministic draw submission, included by
//   rt_scene3d.c rather than compiled independently.
// Key invariants:
//   - Per-view hysteresis prevents one camera from perturbing another view.
//   - PVS operates in absolute world space while frustum bounds are converted
//     to camera-relative render space when required.
//   - Invisible parents prune subtrees; culled parent meshes do not.
// Ownership/Lifetime:
//   - Draw stacks and spatial candidate arrays are temporary and freed before return.
//   - Selected mesh/material/animator handles remain borrowed from their owners.
// Links: rt_scene3d.c, rt_scene3d_api.inc, rt_scene3d_spatial.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Scene3D LOD resolution, visibility culling, and draw traversal.
/// @details The include combines per-view LOD/impostor hysteresis, camera-relative
/// bounds conversion, shadow-caster sweeps, cached spatial bounds, deforming
/// mesh padding, and stable hierarchy-order submission.

/// @brief One item in the iterative hierarchy draw stack.
typedef struct {
    /// Borrowed node to evaluate.
    rt_scene_node3d *node;
    /// Borrowed nearest ancestor skeletal animator.
    void *inherited_animator;
} scene_draw_stack_item_t;

/// @brief Resolve a node's screen-size-driven LOD mesh for the active view.
/// @param node Borrowed node containing authored LOD entries.
/// @param canvas Borrowed active canvas.
/// @param cam Borrowed active camera.
/// @param state In/out per-view hysteresis state.
/// @param radius World-space bounds radius.
/// @param distance Camera distance in world units.
/// @return Borrowed selected resident Mesh3D, or `NULL`.
static void *scene3d_auto_lod_mesh(rt_scene_node3d *node,
                                   const rt_canvas3d *canvas,
                                   const rt_camera3d *cam,
                                   rt_scene3d_lod_view_state *state,
                                   double radius,
                                   double distance);

/// @brief Return true when a Mesh3D payload is resident and eligible for draw selection.
/// @param mesh Borrowed candidate Mesh3D handle.
/// @return Nonzero when the handle exists and its payload is resident.
static int scene3d_mesh_resident(void *mesh) {
    return mesh && rt_mesh3d_get_resident(mesh) != 0;
}

/// @brief Build a stable key for LOD/impostor hysteresis in one rendered view.
/// @details Combines the active camera and canvas pointers. A scene drawn alternately by multiple
///   cameras gets separate hysteresis slots so one view's distance thresholds do not push another
///   view across an LOD boundary.
/// @param canvas Borrowed canvas identifying the rendered target.
/// @param cam Borrowed camera identifying the rendered view.
/// @return Stable nonzero key derived from the two addresses.
static uintptr_t scene3d_lod_view_key(const rt_canvas3d *canvas, const rt_camera3d *cam) {
    uintptr_t key = (uintptr_t)cam ^ ((uintptr_t)canvas << 1u) ^ ((uintptr_t)canvas >> 3u);
    return key ? key : (uintptr_t)1u;
}

/// @brief Reset all per-view LOD and impostor hysteresis state on a node.
/// @details Called when LOD/impostor payloads or residency change. Keeping the legacy fields reset
///   preserves existing behavior for code that inspects the most recent selected index directly.
/// @param node Borrowed node whose fixed-size view-state cache is cleared.
static void scene3d_node_reset_lod_view_states(rt_scene_node3d *node) {
    if (!node)
        return;
    memset(node->lod_view_states, 0, sizeof(node->lod_view_states));
    node->lod_view_state_clock = 0;
    node->lod_selected_index = 0;
    node->lod_selection_valid = 0;
    node->impostor_selected = 0;
}

/// @brief Return the cache slot holding hysteresis state for @p view_key, creating it if needed.
/// @details The fixed-size cache avoids heap allocation during draw traversal. On overflow, the
///   least-recently-used slot is replaced; the selected view then starts from the raw LOD decision
///   for that frame, which is conservative and avoids cross-view flicker.
/// @param node Borrowed node owning the fixed-size cache.
/// @param view_key Nonzero canvas/camera identity key.
/// @return Borrowed mutable cache slot, or `NULL` for an invalid node.
static rt_scene3d_lod_view_state *scene3d_node_lod_view_state(rt_scene_node3d *node,
                                                              uintptr_t view_key) {
    int32_t best = 0;
    uint32_t oldest = UINT32_MAX;
    if (!node)
        return NULL;
    if (!view_key)
        view_key = (uintptr_t)1u;
    node->lod_view_state_clock++;
    if (node->lod_view_state_clock == 0)
        node->lod_view_state_clock = 1;
    for (int32_t i = 0; i < RT_SCENE3D_LOD_VIEW_STATE_COUNT; ++i) {
        rt_scene3d_lod_view_state *state = &node->lod_view_states[i];
        if (state->view_key == view_key) {
            state->last_used = node->lod_view_state_clock;
            return state;
        }
        if (state->view_key == 0) {
            best = i;
            oldest = 0;
            break;
        }
        if (state->last_used < oldest) {
            oldest = state->last_used;
            best = i;
        }
    }
    (void)oldest;
    memset(&node->lod_view_states[best], 0, sizeof(node->lod_view_states[best]));
    node->lod_view_states[best].view_key = view_key;
    node->lod_view_states[best].last_used = node->lod_view_state_clock;
    return &node->lod_view_states[best];
}

/// @brief Return an authored LOD mesh for a 1-based selected index, or NULL for the base mesh.
/// @details SceneNode3D stores selection state as 0=base, 1=lod_levels[0], etc. This helper keeps
///   residency checks centralized so hysteresis never pins selection to an evicted LOD mesh.
/// @param node Borrowed node containing authored LOD levels.
/// @param selected_index One-based LOD index; zero selects the base mesh.
/// @return Borrowed resident LOD Mesh3D, or `NULL` for base/invalid/nonresident selection.
static void *scene3d_lod_mesh_for_selected_index(rt_scene_node3d *node, int32_t selected_index) {
    int32_t lod_count = scene3d_node_lod_count(node);
    if (!node || selected_index <= 0 || selected_index > lod_count)
        return NULL;
    if (!scene3d_mesh_resident(node->lod_levels[selected_index - 1].mesh))
        return NULL;
    return node->lod_levels[selected_index - 1].mesh;
}

/// @brief Compute a stable distance hysteresis band for a manual LOD threshold.
/// @param threshold Authored camera-distance threshold.
/// @return Finite non-negative band of at least 0.5 world units.
static double scene3d_lod_distance_band(double threshold) {
    threshold = scene3d_finite_or(threshold, 0.0);
    if (threshold < 0.0)
        threshold = 0.0;
    return fmax(0.5, threshold * 0.05);
}

/// @brief Apply hysteresis to distance-threshold LOD selection.
/// @details Prevents the selected mesh from bouncing between adjacent LODs when the camera hovers
///   near an authored distance threshold. Returns a selected index where 0 is the base mesh and
///   positive values index into `lod_levels` with a +1 offset.
/// @param node Borrowed node containing sorted manual LOD thresholds.
/// @param state Optional in/out per-view hysteresis state.
/// @param raw_index Unfiltered selection where zero denotes the base mesh.
/// @param distance Current camera distance in world units.
/// @return Stable selected index in `[0, lod_count]`.
static int32_t scene3d_manual_lod_index_with_hysteresis(rt_scene_node3d *node,
                                                        rt_scene3d_lod_view_state *state,
                                                        int32_t raw_index,
                                                        double distance) {
    int32_t lod_count = scene3d_node_lod_count(node);
    int32_t prev = state && state->lod_selection_valid ? state->lod_selected_index : raw_index;
    if (!node || raw_index < 0 || raw_index > lod_count)
        return 0;
    if (prev < 0 || prev > lod_count ||
        (prev > 0 && !scene3d_lod_mesh_for_selected_index(node, prev)))
        prev = raw_index;
    if (prev != raw_index) {
        if (raw_index > prev) {
            double threshold = node->lod_levels[raw_index - 1].distance;
            if (distance < threshold + scene3d_lod_distance_band(threshold))
                raw_index = prev;
        } else if (prev > 0) {
            double threshold = node->lod_levels[prev - 1].distance;
            if (distance > threshold - scene3d_lod_distance_band(threshold))
                raw_index = prev;
        }
    }
    if (state) {
        state->lod_selected_index = raw_index;
        state->lod_selection_valid = 1;
    }
    node->lod_selected_index = raw_index;
    node->lod_selection_valid = 1;
    return raw_index;
}

/// @brief Apply hysteresis to projected-size automatic LOD selection.
/// @param node Borrowed node containing authored LOD levels.
/// @param state Optional in/out per-view hysteresis state.
/// @param raw_index Unfiltered selection where zero denotes the base mesh.
/// @param projected_px Current projected bounds diameter in pixels.
/// @param screen_error_px Authored target error used to derive transition thresholds.
/// @return Stable selected index in `[0, lod_count]`.
static int32_t scene3d_auto_lod_index_with_hysteresis(rt_scene_node3d *node,
                                                      rt_scene3d_lod_view_state *state,
                                                      int32_t raw_index,
                                                      double projected_px,
                                                      double screen_error_px) {
    int32_t lod_count = scene3d_node_lod_count(node);
    int32_t prev = state && state->lod_selection_valid ? state->lod_selected_index : raw_index;
    double band = fmax(1.0, screen_error_px * 0.25);
    if (!node || raw_index < 0 || raw_index > lod_count)
        return 0;
    if (prev < 0 || prev > lod_count ||
        (prev > 0 && !scene3d_lod_mesh_for_selected_index(node, prev)))
        prev = raw_index;
    if (prev != raw_index) {
        if (raw_index > prev) {
            double threshold = screen_error_px * (double)(lod_count - (raw_index - 1));
            if (projected_px > threshold - band)
                raw_index = prev;
        } else if (prev > 0) {
            double threshold = screen_error_px * (double)(lod_count - (prev - 1));
            if (projected_px < threshold + band)
                raw_index = prev;
        }
    }
    if (state) {
        state->lod_selected_index = raw_index;
        state->lod_selection_valid = 1;
    }
    node->lod_selected_index = raw_index;
    node->lod_selection_valid = 1;
    return raw_index;
}

/// @brief Return non-zero when an impostor should be active after distance hysteresis.
/// @param node Borrowed node containing an optional impostor.
/// @param state Optional in/out per-view hysteresis state.
/// @param distance Current camera distance in world units.
/// @return Nonzero when the impostor remains or becomes active.
static int scene3d_impostor_active_with_hysteresis(rt_scene_node3d *node,
                                                   rt_scene3d_lod_view_state *state,
                                                   double distance) {
    double threshold;
    double band;
    int active;
    int previously_selected;
    if (!node || !node->has_impostor)
        return 0;
    threshold = scene3d_finite_or(node->impostor_distance, 0.0);
    band = fmax(1.0, fabs(threshold) * 0.05);
    previously_selected = state ? state->impostor_selected : node->impostor_selected;
    active = previously_selected ? distance >= threshold - band : distance >= threshold + band;
    if (state)
        state->impostor_selected = active ? 1 : 0;
    node->impostor_selected = active ? 1 : 0;
    return active;
}

/// @brief Convert absolute world-space AABB coordinates into the canvas render space if needed.
/// @details Canvas3D camera-relative upload zeros the view translation and subtracts the camera
///   origin from submitted model matrices. CPU frustum tests must subtract the same origin from
///   AABBs before testing against the cached VP, otherwise large-world scenes can be culled against
///   the wrong coordinate space.
/// @param canvas Borrowed active canvas defining camera-relative upload state.
/// @param world_min In/out AABB minimum converted to render space.
/// @param world_max In/out AABB maximum converted to render space.
/// @return 1 when the bounds were converted or no conversion was needed; 0 when subtracting the
///   camera-relative origin would overflow the float bounds used by the renderer.
static int scene3d_aabb_to_canvas_render_space(rt_canvas3d *canvas,
                                               float world_min[3],
                                               float world_max[3]) {
    double origin[3];
    if (!canvas || !world_min || !world_max || !canvas3d_uses_camera_relative_upload(canvas))
        return 1;
    origin[0] = canvas->camera_relative_origin[0];
    origin[1] = canvas->camera_relative_origin[1];
    origin[2] = canvas->camera_relative_origin[2];
    for (int i = 0; i < 3; ++i) {
        double shifted_min = (double)world_min[i] - origin[i];
        double shifted_max = (double)world_max[i] - origin[i];
        if (!isfinite(shifted_min) || !isfinite(shifted_max) ||
            fabs(shifted_min) > (double)FLT_MAX || fabs(shifted_max) > (double)FLT_MAX)
            return 0;
        world_min[i] = (float)shifted_min;
        world_max[i] = (float)shifted_max;
    }
    return 1;
}

/// @brief Extend a render-space AABB by the canvas's shadow-caster sweep vector.
/// @details A caster at P shadows points along the light's travel direction, so its
///   swept box (AABB ∪ AABB+sweep) is what must intersect the view frustum for its
///   shadow to remain correct. Applied only to frustum tests — PVS stays exact — and
///   only while a shadow-casting directional light is active. Nodes admitted solely by
///   the sweep are frustum-culled from the main pass at the deferred layer, so they
///   cost shadow-pass work only.
/// @param canvas Borrowed canvas containing the optional caster sweep.
/// @param world_min In/out render-space AABB minimum.
/// @param world_max In/out render-space AABB maximum.
static void scene3d_apply_shadow_caster_sweep(const rt_canvas3d *canvas,
                                              float world_min[3],
                                              float world_max[3]) {
    if (!canvas || !canvas->shadow_caster_sweep_active || !world_min || !world_max)
        return;
    for (int i = 0; i < 3; i++) {
        float sweep = canvas->shadow_caster_sweep[i];
        if (sweep < 0.0f)
            world_min[i] += sweep;
        else
            world_max[i] += sweep;
    }
}

/// @brief Push (node, inherited animator) onto a growable draw-traversal stack.
/// @details Same iterative-traversal rationale and growth/return contract as
///          scene_bounds_stack_push: capacity doubles from 64; returns 1 on
///          success or no-op, 0 on overflow/realloc failure.
/// @param stack Address of the caller-owned stack allocation.
/// @param count In/out populated item count.
/// @param capacity In/out allocated item capacity.
/// @param node Borrowed node to enqueue; invalid nodes are ignored successfully.
/// @param inherited_animator Borrowed nearest ancestor animator carried to the node.
/// @return Nonzero on append or safe no-op, zero on growth failure.
static int scene_draw_stack_push(scene_draw_stack_item_t **stack,
                                 size_t *count,
                                 size_t *capacity,
                                 rt_scene_node3d *node,
                                 void *inherited_animator) {
    rt_scene_node3d *checked;
    if (!stack || !count || !capacity || !node)
        return 1;
    checked = scene_node3d_checked(node);
    if (!checked)
        return 1;
    if (*count >= *capacity) {
        if (!scene3d_grow_stack_storage((void **)stack, capacity, sizeof(**stack)))
            return 0;
    }
    (*stack)[*count].node = checked;
    (*stack)[*count].inherited_animator = inherited_animator;
    (*count)++;
    return 1;
}

/// @brief Resolve which mesh+material a node draws this frame — auto-LOD, manual-LOD,
///        impostor selection, and residency — and compute the chosen mesh's local bounds.
/// @details Updates the per-view hysteresis state, selects yaw-dependent impostor
///          frames, and rejects evicted payloads before returning borrowed draw data.
/// @param current Borrowed visible node to resolve.
/// @param canvas Borrowed active canvas.
/// @param cam Borrowed active camera.
/// @param cam_pos Borrowed resolved world-space camera position.
/// @param out_material Output receiving the borrowed selected material.
/// @param out_min Output receiving selected mesh local minimum bounds.
/// @param out_max Output receiving selected mesh local maximum bounds.
/// @param out_radius Output receiving selected mesh local bounds radius.
/// @return The mesh to draw, or NULL when the node has no resident drawable mesh.
static void *scene3d_resolve_draw_mesh(rt_scene_node3d *current,
                                       rt_canvas3d *canvas,
                                       rt_camera3d *cam,
                                       const double *cam_pos,
                                       void **out_material,
                                       float out_min[3],
                                       float out_max[3],
                                       float *out_radius) {
    void *draw_mesh = rt_g3d_checked_or_null(current->mesh, RT_G3D_MESH3D_CLASS_ID);
    void *draw_material = rt_g3d_checked_or_null(current->material, RT_G3D_MATERIAL3D_CLASS_ID);
    double camera_distance = 0.0;
    int has_camera_distance = 0;
    rt_scene3d_lod_view_state *lod_state =
        scene3d_node_lod_view_state(current, scene3d_lod_view_key(canvas, cam));

    if ((draw_mesh || current->has_impostor) && cam_pos) {
        float local_center[3] = {0.0f, 0.0f, 0.0f};
        double world_center[3];
        float base_min[3] = {0.0f, 0.0f, 0.0f};
        float base_max[3] = {0.0f, 0.0f, 0.0f};
        float base_radius = 0.0f;
        if (draw_mesh) {
            scene_mesh_bounds((rt_mesh3d *)draw_mesh, base_min, base_max, &base_radius);
            local_center[0] = 0.5f * (base_min[0] + base_max[0]);
            local_center[1] = 0.5f * (base_min[1] + base_max[1]);
            local_center[2] = 0.5f * (base_min[2] + base_max[2]);
        }
        scene_world_point_d(current->world_matrix, local_center, world_center);
        {
            double dx = world_center[0] - cam_pos[0];
            double dy = world_center[1] - cam_pos[1];
            double dz = world_center[2] - cam_pos[2];
            double dist = sqrt(dx * dx + dy * dy + dz * dz);
            if (!isfinite(dist))
                dist = 0.0;
            camera_distance = dist;
            has_camera_distance = 1;
            if (draw_mesh && current->auto_lod_enabled && scene3d_node_lod_count(current) > 0) {
                void *auto_mesh =
                    scene3d_auto_lod_mesh(current, canvas, cam, lod_state, base_radius, dist);
                if (auto_mesh)
                    draw_mesh = auto_mesh;
            }
        }
    }

    if (draw_mesh) {
        int32_t lod_count = scene3d_node_lod_count(current);
        if (!current->auto_lod_enabled && lod_count > 0 && has_camera_distance) {
            double dist = camera_distance;
            int32_t raw_index = 0;
            for (int32_t l = lod_count - 1; l >= 0; l--) {
                if (dist >= current->lod_levels[l].distance &&
                    scene3d_mesh_resident(current->lod_levels[l].mesh)) {
                    raw_index = l + 1;
                    break;
                }
            }
            raw_index =
                scene3d_manual_lod_index_with_hysteresis(current, lod_state, raw_index, dist);
            {
                void *lod_mesh = scene3d_lod_mesh_for_selected_index(current, raw_index);
                if (lod_mesh)
                    draw_mesh = lod_mesh;
            }
        }
    }

    void *impostor_mesh = rt_g3d_checked_or_null(current->impostor_mesh, RT_G3D_MESH3D_CLASS_ID);
    void *impostor_material =
        rt_g3d_checked_or_null(current->impostor_material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (current->has_impostor && impostor_mesh && impostor_material && has_camera_distance &&
        scene3d_impostor_active_with_hysteresis(current, lod_state, camera_distance) &&
        scene3d_mesh_resident(impostor_mesh)) {
        draw_mesh = impostor_mesh;
        draw_material = impostor_material;
        if (current->impostor_frame_count > 1 && current->impostor_frame_meshes && cam_pos) {
            /* Pick the strip frame whose capture yaw is nearest the camera bearing.
             * Frames were captured at yaw i * 2*pi/N with frame 0 looking down -Z. */
            double wx = current->world_matrix[3];
            double wz = current->world_matrix[11];
            double dx = cam_pos[0] - wx;
            double dz = cam_pos[2] - wz;
            double yaw = atan2(dx, dz);
            double step = 6.283185307179586 / (double)current->impostor_frame_count;
            int32_t frame = (int32_t)floor((yaw + step * 0.5) / step);
            frame %= current->impostor_frame_count;
            if (frame < 0)
                frame += current->impostor_frame_count;
            void *frame_mesh = rt_g3d_checked_or_null(current->impostor_frame_meshes[frame],
                                                      RT_G3D_MESH3D_CLASS_ID);
            if (frame_mesh && scene3d_mesh_resident(frame_mesh)) {
                draw_mesh = frame_mesh;
                current->impostor_frame_index = frame;
            }
        }
    } else if (!current->has_impostor || !impostor_mesh || !impostor_material ||
               !scene3d_mesh_resident(impostor_mesh)) {
        current->impostor_selected = 0;
        if (lod_state)
            lod_state->impostor_selected = 0;
    }

    if (draw_mesh && !scene3d_mesh_resident(draw_mesh))
        draw_mesh = NULL;

    out_min[0] = out_min[1] = out_min[2] = 0.0f;
    out_max[0] = out_max[1] = out_max[2] = 0.0f;
    *out_radius = 0.0f;
    if (draw_mesh)
        scene_mesh_bounds((rt_mesh3d *)draw_mesh, out_min, out_max, out_radius);

    *out_material = draw_material;
    return draw_mesh;
}

/// @brief Cull-test a cached world-space AABB against the active PVS and frustum.
/// @param current Borrowed node owning the cached bounds.
/// @param canvas Borrowed active canvas.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param world_min_d Borrowed double-precision world minimum bounds.
/// @param world_max_d Borrowed double-precision world maximum bounds.
/// @param culled Optional in/out aggregate cull counter.
/// @return Nonzero when the node remains potentially visible.
static int scene3d_cached_world_bounds_cull_test(rt_scene_node3d *current,
                                                 rt_canvas3d *canvas,
                                                 const vgfx3d_frustum_t *frustum,
                                                 const scene3d_pvs_context_t *pvs,
                                                 const double world_min_d[3],
                                                 const double world_max_d[3],
                                                 int32_t *culled);

/// @brief Frustum + PVS visibility test for a node's chosen mesh.
/// @param current Borrowed node being tested.
/// @param canvas Borrowed active canvas.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param draw_mesh Borrowed selected Mesh3D handle.
/// @param draw_min Borrowed local mesh minimum bounds.
/// @param draw_max Borrowed local mesh maximum bounds.
/// @param draw_radius Local bounds radius.
/// @param effective_animator Borrowed inherited or bound skeletal animator.
/// @param culled Optional in/out aggregate cull counter.
/// @return 1 when the node should be drawn, 0 when culled (bumping the cull counters).
static int scene3d_node_cull_test(rt_scene_node3d *current,
                                  rt_canvas3d *canvas,
                                  const vgfx3d_frustum_t *frustum,
                                  const scene3d_pvs_context_t *pvs,
                                  void *draw_mesh,
                                  const float draw_min[3],
                                  const float draw_max[3],
                                  float draw_radius,
                                  void *effective_animator,
                                  int32_t *culled) {
    if ((frustum || (pvs && pvs->active)) && draw_mesh && draw_radius > 0.0f) {
        double union_world_min[3];
        double union_world_max[3];
        double union_radius = 0.0;
        if (scene3d_node_world_draw_union_aabb(
                current, effective_animator, union_world_min, union_world_max, &union_radius))
            return scene3d_cached_world_bounds_cull_test(
                current, canvas, frustum, pvs, union_world_min, union_world_max, culled);

        rt_mesh3d *draw_mesh_impl = (rt_mesh3d *)draw_mesh;
        int has_dynamic_deformation =
            scene3d_mesh_has_dynamic_deformation(draw_mesh_impl, effective_animator);
        float cull_min[3] = {draw_min[0], draw_min[1], draw_min[2]};
        float cull_max[3] = {draw_max[0], draw_max[1], draw_max[2]};
        float world_min[3], world_max[3];
        double world_min_d[3], world_max_d[3];
        if (has_dynamic_deformation) {
            float pad = (float)scene3d_mesh_dynamic_bound_pad(
                draw_mesh_impl, effective_animator, (double)draw_radius);
            cull_min[0] -= pad;
            cull_min[1] -= pad;
            cull_min[2] -= pad;
            cull_max[0] += pad;
            cull_max[1] += pad;
            cull_max[2] += pad;
        }
        if (!vgfx3d_transform_aabb_checked(
                cull_min, cull_max, current->world_matrix, world_min, world_max))
            return 1;
        world_min_d[0] = (double)world_min[0];
        world_min_d[1] = (double)world_min[1];
        world_min_d[2] = (double)world_min[2];
        world_max_d[0] = (double)world_max[0];
        world_max_d[1] = (double)world_max[1];
        world_max_d[2] = (double)world_max[2];
        if (pvs && pvs->active &&
            !scene3d_pvs_allows_aabb(current->owner_scene, pvs, world_min_d, world_max_d)) {
            if (culled)
                (*culled)++;
            if (pvs->culled_count)
                (*pvs->culled_count)++;
            return 0;
        } else if (frustum) {
            if (!scene3d_aabb_to_canvas_render_space(canvas, world_min, world_max))
                return 1;
            scene3d_apply_shadow_caster_sweep(canvas, world_min, world_max);
            if (vgfx3d_frustum_test_aabb(frustum, world_min, world_max) == 0) {
                if (culled)
                    (*culled)++;
                return 0;
            }
        }
    }
    return 1;
}

/// @brief Cull-test a node using its cached world-space AABB against the PVS and view frustum,
///   incrementing @p culled when the node is rejected.
/// @param current Borrowed node owning the cached bounds.
/// @param canvas Borrowed active canvas.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param world_min_d Borrowed double-precision world minimum bounds.
/// @param world_max_d Borrowed double-precision world maximum bounds.
/// @param culled Optional in/out aggregate cull counter.
/// @return Non-zero if the node is potentially visible and should be kept; 0 if rejected.
static int scene3d_cached_world_bounds_cull_test(rt_scene_node3d *current,
                                                 rt_canvas3d *canvas,
                                                 const vgfx3d_frustum_t *frustum,
                                                 const scene3d_pvs_context_t *pvs,
                                                 const double world_min_d[3],
                                                 const double world_max_d[3],
                                                 int32_t *culled) {
    float world_min[3];
    float world_max[3];
    if (!current || !world_min_d || !world_max_d)
        return 1;
    if (pvs && pvs->active &&
        !scene3d_pvs_allows_aabb(current->owner_scene, pvs, world_min_d, world_max_d)) {
        if (culled)
            (*culled)++;
        if (pvs->culled_count)
            (*pvs->culled_count)++;
        return 0;
    }
    if (!frustum)
        return 1;
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(world_min_d[i]) || !isfinite(world_max_d[i]) ||
            fabs(world_min_d[i]) > (double)FLT_MAX || fabs(world_max_d[i]) > (double)FLT_MAX)
            return 1;
        world_min[i] = (float)world_min_d[i];
        world_max[i] = (float)world_max_d[i];
    }
    if (!scene3d_aabb_to_canvas_render_space(canvas, world_min, world_max))
        return 1;
    scene3d_apply_shadow_caster_sweep(canvas, world_min, world_max);
    if (vgfx3d_frustum_test_aabb(frustum, world_min, world_max) == 0) {
        if (culled)
            (*culled)++;
        return 0;
    }
    return 1;
}

/// @brief Submit a node's resolved mesh to the canvas (skinned when an animator palette exists).
/// @details Uses node-generation-salted motion keys, validates skeleton compatibility,
///          supplies previous palettes for motion vectors, and expands explicit
///          bounds for skeletal or morph deformation.
/// @param current Borrowed node supplying the world matrix and identity serial.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param draw_mesh Borrowed selected Mesh3D handle.
/// @param draw_material Borrowed selected Material3D handle.
/// @param effective_animator Borrowed animator supplying optional skinning palettes.
/// @param draw_min Borrowed local minimum bounds.
/// @param draw_max Borrowed local maximum bounds.
/// @param draw_radius Local bounds radius used for deformation padding.
/// @param visible_nodes Optional in/out submitted-node counter.
static void scene3d_submit_node_draw(rt_scene_node3d *current,
                                     void *canvas3d,
                                     void *draw_mesh,
                                     void *draw_material,
                                     void *effective_animator,
                                     const float draw_min[3],
                                     const float draw_max[3],
                                     float draw_radius,
                                     int32_t *visible_nodes) {
    draw_mesh = rt_g3d_checked_or_null(draw_mesh, RT_G3D_MESH3D_CLASS_ID);
    draw_material = rt_g3d_checked_or_null(draw_material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (!(draw_mesh && draw_material))
        return;
    if (visible_nodes)
        (*visible_nodes)++;
    /* Motion/occlusion history keys are pointer-based; salting with the node's
     * allocation generation keeps a destroyed node's same-address successor from
     * inheriting its previous transform (one-frame motion-vector flash). */
    const void *motion_key =
        (const void *)((uintptr_t)current ^
                       ((uintptr_t)current->identity_serial * (uintptr_t)0x9E3779B9u));
    const float *anim_palette = NULL;
    const float *anim_prev_palette = NULL;
    int32_t anim_bone_count = 0;
    int32_t anim_prev_bone_count = 0;
    int32_t mesh_bone_count = ((rt_mesh3d *)draw_mesh)->bone_count;
    float submit_min[3] = {draw_min ? draw_min[0] : 0.0f,
                           draw_min ? draw_min[1] : 0.0f,
                           draw_min ? draw_min[2] : 0.0f};
    float submit_max[3] = {draw_max ? draw_max[0] : 0.0f,
                           draw_max ? draw_max[1] : 0.0f,
                           draw_max ? draw_max[2] : 0.0f};
    int has_dynamic_deformation =
        scene3d_mesh_has_dynamic_deformation((rt_mesh3d *)draw_mesh, effective_animator);
    int8_t dynamic_deformation_flag = (int8_t)(has_dynamic_deformation ? 1 : 0);
    int use_explicit_bounds = draw_min && draw_max ? 1 : 0;

    if (effective_animator) {
        void *anim_skeleton = rt_anim_controller3d_get_skeleton(effective_animator);
        rt_mesh3d *mesh_impl = (rt_mesh3d *)draw_mesh;
        if (!mesh_impl->skeleton_ref || mesh_impl->skeleton_ref == anim_skeleton) {
            anim_palette =
                rt_anim_controller3d_get_final_palette_data(effective_animator, &anim_bone_count);
            anim_prev_palette = rt_anim_controller3d_get_previous_palette_data(
                effective_animator, &anim_prev_bone_count);
            if (anim_prev_bone_count <= 0 || anim_prev_bone_count < anim_bone_count)
                anim_prev_palette = NULL;
        }
    }
    if (use_explicit_bounds && has_dynamic_deformation) {
        float pad = (float)scene3d_mesh_dynamic_bound_pad(
            (rt_mesh3d *)draw_mesh, effective_animator, (double)draw_radius);
        submit_min[0] -= pad;
        submit_min[1] -= pad;
        submit_min[2] -= pad;
        submit_max[0] += pad;
        submit_max[1] += pad;
        submit_max[2] += pad;
    }
    if (anim_palette && anim_bone_count > 0 && mesh_bone_count > 0) {
        /* Partitioned sub-meshes (bone_map set) need the full skeleton palette so
         * the draw path can gather their local slots; clamping to the sub-mesh
         * count would truncate the source palette before the gather. */
        int32_t draw_bone_count =
            ((rt_mesh3d *)draw_mesh)->bone_map
                ? anim_bone_count
                : (anim_bone_count < mesh_bone_count ? anim_bone_count : mesh_bone_count);
        if (rt_canvas3d_add_temp_object(canvas3d, effective_animator)) {
            rt_canvas3d_draw_mesh_matrix_skinned_keyed_bounds(
                canvas3d,
                draw_mesh,
                current->world_matrix,
                draw_material,
                motion_key,
                anim_palette,
                anim_prev_palette,
                draw_bone_count,
                use_explicit_bounds ? submit_min : NULL,
                use_explicit_bounds ? submit_max : NULL,
                dynamic_deformation_flag,
                dynamic_deformation_flag);
        }
    } else {
        rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas3d,
                                                  draw_mesh,
                                                  current->world_matrix,
                                                  draw_material,
                                                  motion_key,
                                                  NULL,
                                                  NULL,
                                                  use_explicit_bounds ? submit_min : NULL,
                                                  use_explicit_bounds ? submit_max : NULL,
                                                  dynamic_deformation_flag,
                                                  dynamic_deformation_flag,
                                                  0.0f);
    }
}

/// @brief Draw a single node: resolve its mesh+material, cull-test it, then submit it.
/// @param current Borrowed visible node to evaluate.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param canvas Borrowed destination canvas payload.
/// @param cam Borrowed active camera payload.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param culled Optional in/out cull counter.
/// @param visible_nodes Optional in/out submitted-node counter.
/// @param cam_pos Borrowed resolved world-space camera position.
/// @param effective_animator Borrowed inherited or node-bound skeletal animator.
static void scene3d_draw_node_self(rt_scene_node3d *current,
                                   void *canvas3d,
                                   rt_canvas3d *canvas,
                                   rt_camera3d *cam,
                                   const vgfx3d_frustum_t *frustum,
                                   const scene3d_pvs_context_t *pvs,
                                   int32_t *culled,
                                   int32_t *visible_nodes,
                                   const double *cam_pos,
                                   void *effective_animator) {
    void *draw_mesh;
    void *draw_material;
    float draw_min[3];
    float draw_max[3];
    float draw_radius;

    if (!current)
        return;
    recompute_world_matrix(current);

    draw_mesh = scene3d_resolve_draw_mesh(
        current, canvas, cam, cam_pos, &draw_material, draw_min, draw_max, &draw_radius);

    if (!scene3d_node_cull_test(current,
                                canvas,
                                frustum,
                                pvs,
                                draw_mesh,
                                draw_min,
                                draw_max,
                                draw_radius,
                                effective_animator,
                                culled))
        return;

    scene3d_submit_node_draw(current,
                             canvas3d,
                             draw_mesh,
                             draw_material,
                             effective_animator,
                             draw_min,
                             draw_max,
                             draw_radius,
                             visible_nodes);
}

/// @brief Draw one spatial-index entry's node through @p canvas with the active camera,
///   applying frustum/PVS culling (updating @p culled / @p visible_nodes) and resolving the
///   node's effective animator.
/// @param entry Borrowed spatial-index entry and cached world bounds.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param canvas Borrowed destination canvas payload.
/// @param cam Borrowed active camera payload.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param culled Optional in/out cull counter.
/// @param visible_nodes Optional in/out submitted-node counter.
/// @param cam_pos Borrowed resolved world-space camera position.
static void scene3d_draw_spatial_entry(rt_scene3d_spatial_entry *entry,
                                       void *canvas3d,
                                       rt_canvas3d *canvas,
                                       rt_camera3d *cam,
                                       const vgfx3d_frustum_t *frustum,
                                       const scene3d_pvs_context_t *pvs,
                                       int32_t *culled,
                                       int32_t *visible_nodes,
                                       const double *cam_pos) {
    rt_scene_node3d *current;
    void *effective_animator;
    void *draw_mesh;
    void *draw_material;
    float draw_min[3];
    float draw_max[3];
    float draw_radius;
    int use_cached_bounds;
    rt_mesh3d *draw_mesh_impl;
    if (!entry || !entry->node)
        return;
    current = entry->node;
    effective_animator = scene3d_effective_animator(current);
    recompute_world_matrix(current);
    draw_mesh = scene3d_resolve_draw_mesh(
        current, canvas, cam, cam_pos, &draw_material, draw_min, draw_max, &draw_radius);
    draw_mesh_impl = (rt_mesh3d *)rt_g3d_checked_or_null(draw_mesh, RT_G3D_MESH3D_CLASS_ID);
    use_cached_bounds = draw_mesh_impl && entry->cullable &&
                        !scene3d_mesh_has_dynamic_deformation(draw_mesh_impl, effective_animator);
    if (use_cached_bounds) {
        if (!scene3d_cached_world_bounds_cull_test(
                current, canvas, frustum, pvs, entry->world_min, entry->world_max, culled))
            return;
    } else if (!scene3d_node_cull_test(current,
                                       canvas,
                                       frustum,
                                       pvs,
                                       draw_mesh,
                                       draw_min,
                                       draw_max,
                                       draw_radius,
                                       effective_animator,
                                       culled)) {
        return;
    }
    scene3d_submit_node_draw(current,
                             canvas3d,
                             draw_mesh,
                             draw_material,
                             effective_animator,
                             draw_min,
                             draw_max,
                             draw_radius,
                             visible_nodes);
}

/// @brief Draw traversal: depth-first, skip invisible nodes, frustum-cull meshes.
/// Children are ALWAYS traversed even if the parent mesh is culled, because
/// child transforms may place them inside the frustum independently.
/// @param node Borrowed subtree root.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param canvas Borrowed destination canvas payload.
/// @param cam Borrowed active camera payload.
/// @param frustum Optional borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param culled Optional in/out cull counter.
/// @param visible_nodes Optional in/out submitted-node counter.
/// @param cam_pos Borrowed resolved world-space camera position.
/// @param inherited_animator Borrowed ancestor animator inherited by the root.
static void draw_node(rt_scene_node3d *node,
                      void *canvas3d,
                      rt_canvas3d *canvas,
                      rt_camera3d *cam,
                      const vgfx3d_frustum_t *frustum,
                      const scene3d_pvs_context_t *pvs,
                      int32_t *culled,
                      int32_t *visible_nodes,
                      const double *cam_pos,
                      void *inherited_animator) {
    scene_draw_stack_item_t *stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    if (!node)
        return;
    if (!scene_draw_stack_push(&stack, &count, &capacity, node, inherited_animator)) {
        rt_trap("Scene3D.Draw: traversal stack allocation failed");
        return;
    }
    while (count > 0) {
        scene_draw_stack_item_t item = stack[--count];
        rt_scene_node3d *current = item.node;
        void *effective_animator;

        if (!current->visible)
            continue;

        effective_animator =
            rt_g3d_checked_or_null(current->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
        if (!effective_animator)
            effective_animator = item.inherited_animator;
        scene3d_draw_node_self(current,
                               canvas3d,
                               canvas,
                               cam,
                               frustum,
                               pvs,
                               culled,
                               visible_nodes,
                               cam_pos,
                               effective_animator);

        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            if (!scene_draw_stack_push(
                    &stack, &count, &capacity, current->children[i], effective_animator)) {
                rt_trap("Scene3D.Draw: traversal stack allocation failed");
                free(stack);
                return;
            }
        }
    }
    free(stack);
}

/// @brief Test a spatial BVH candidate against the exact frustum planes before draw resolution.
/// @details `scene3d_spatial_collect_aabb` queries with the frustum's enclosing AABB so it can
///          traverse the BVH cheaply in double precision. This second-stage filter rejects the
///          enclosing-box false positives before the heavier LOD, material, skinning, and morph
///          resolution paths run.
/// @param entry Borrowed spatial-index candidate.
/// @param canvas Borrowed active canvas.
/// @param frustum Borrowed exact render-space frustum.
/// @return Nonzero when the candidate passes or cannot be safely rejected.
static int scene3d_spatial_entry_frustum_prefilter(rt_scene3d_spatial_entry *entry,
                                                   rt_canvas3d *canvas,
                                                   const vgfx3d_frustum_t *frustum) {
    float world_min[3];
    float world_max[3];
    if (!entry || !frustum)
        return 1;
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(entry->world_min[i]) || !isfinite(entry->world_max[i]) ||
            fabs(entry->world_min[i]) > (double)FLT_MAX ||
            fabs(entry->world_max[i]) > (double)FLT_MAX)
            return 1;
        world_min[i] = (float)entry->world_min[i];
        world_max[i] = (float)entry->world_max[i];
    }
    if (!scene3d_aabb_to_canvas_render_space(canvas, world_min, world_max))
        return 1;
    scene3d_apply_shadow_caster_sweep(canvas, world_min, world_max);
    return vgfx3d_frustum_test_aabb(frustum, world_min, world_max) != 0;
}

/// @brief Draw the scene using the spatial index for frustum culling.
/// @details Gathers candidate nodes whose bounds intersect the camera frustum via the BVH, sorts
/// them
///          back into scene traversal order for stable draw ordering, and draws each. Falls back to
///          a full traversal when the index is unavailable.
/// @param scene Borrowed scene containing the spatial index.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param canvas Borrowed destination canvas payload.
/// @param cam Borrowed active camera payload.
/// @param frustum Borrowed render-space frustum.
/// @param pvs Optional borrowed active PVS context.
/// @param vp Borrowed row-major view-projection matrix.
/// @param culled Optional in/out cull counter.
/// @param visible_nodes Optional in/out submitted-node counter.
/// @param cam_pos Borrowed resolved world-space camera position.
/// @return Nonzero after spatial traversal; zero when the caller should fall back.
static int draw_node_spatial(rt_scene3d *scene,
                             void *canvas3d,
                             rt_canvas3d *canvas,
                             rt_camera3d *cam,
                             const vgfx3d_frustum_t *frustum,
                             const scene3d_pvs_context_t *pvs,
                             const float vp[16],
                             int32_t *culled,
                             int32_t *visible_nodes,
                             const double *cam_pos) {
    scene3d_spatial_candidate_list_t candidates = {0};
    double frustum_min[3];
    double frustum_max[3];
    int ok;
    if (!scene || !scene->use_spatial_index)
        return 0;
    if (scene3d_frustum_bounds_from_vp(vp, frustum_min, frustum_max)) {
        /* Shadow-caster sweep: gather BVH candidates "upstream" of the frustum along
         * the light so off-screen casters reach the per-node swept-AABB test. */
        if (canvas && canvas->shadow_caster_sweep_active) {
            for (int i = 0; i < 3; i++) {
                double sweep = (double)canvas->shadow_caster_sweep[i];
                if (sweep > 0.0)
                    frustum_min[i] -= sweep;
                else
                    frustum_max[i] -= sweep;
            }
        }
        ok = scene3d_spatial_collect_aabb(scene, frustum_min, frustum_max, &candidates, 1);
    } else {
        ok = scene3d_spatial_collect_all(scene, &candidates);
    }
    if (!ok) {
        free(candidates.items);
        return 0;
    }
    if (culled)
        *culled += scene->spatial_index.last_prefiltered_count;
    for (int32_t i = 0; i < candidates.count; ++i) {
        if (!scene3d_spatial_entry_frustum_prefilter(candidates.items[i], canvas, frustum)) {
            if (culled)
                (*culled)++;
            continue;
        }
        scene3d_draw_spatial_entry(candidates.items[i],
                                   canvas3d,
                                   canvas,
                                   cam,
                                   frustum,
                                   pvs,
                                   culled,
                                   visible_nodes,
                                   cam_pos);
    }
    free(candidates.items);
    return 1;
}
