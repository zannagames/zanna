//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_api.inc
// Purpose: Scene3D public lifecycle, load, query, draw, and transactional clear
//   implementation included by rt_scene3d.c.
// Key invariants:
//   - The implicit root survives Clear and remains owned by its scene.
//   - Subtree ownership is preflighted before hierarchy publication.
// Ownership/Lifetime:
//   - Scene3D retains its root and root children retain their subtrees.
//   - Temporary owner transactions own only their native node-pointer arrays.
// Links: rt_scene3d.c, rt_scene3d_node.c, rt_scene3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Scene3D lifecycle, hierarchy access, visibility authoring, draw, and
/// telemetry.
/// @details This include owns scene construction/finalization, root-level
/// mutations, name lookup, portal/PVS definitions, camera-dependent LOD
/// selection, draw-state preservation, binding synchronization, and counters.

/*==========================================================================
 * Scene3D
 *=========================================================================*/

/// @brief GC finalizer for a Scene3D — releases the root node and any post-processing context.
/// @details Clears owner backlinks before releasing the root, then releases
///          imported asset tables, spatial-index storage, visibility data, and
///          reusable traversal buffers in dependency order.
/// @param obj Borrowed Scene3D payload being finalized; `NULL` is accepted.
static void rt_scene3d_finalize(void *obj) {
    rt_scene3d *scene = (rt_scene3d *)obj;
    rt_scene_node3d *root;
    int32_t visibility_zone_count;
    if (!scene)
        return;
    root = scene_node3d_checked(scene->root);
    if (root) {
        scene_node_clear_owner_recursive(root, scene);
        root->parent = NULL;
    }
    scene3d_release_ref((void **)&scene->root);
    for (int32_t i = 0; i < scene->baked_animation_count; ++i)
        scene3d_release_ref(&scene->baked_animations[i]);
    free(scene->baked_animations);
    scene->baked_animations = NULL;
    scene->baked_animation_count = 0;
    if (scene->baked_asset) {
        rt_vscn_loaded_asset3d *asset = scene->baked_asset;
        void ***arrays[] = {&asset->meshes,
                            &asset->materials,
                            &asset->skeletons,
                            &asset->animations,
                            &asset->node_animations,
                            &asset->cameras};
        int32_t *counts[] = {&asset->mesh_count,
                             &asset->material_count,
                             &asset->skeleton_count,
                             &asset->animation_count,
                             &asset->node_animation_count,
                             &asset->camera_count};
        for (size_t table = 0; table < sizeof(arrays) / sizeof(arrays[0]); ++table) {
            for (int32_t i = 0; i < *counts[table]; ++i)
                scene3d_release_ref(&(*arrays[table])[i]);
            free(*arrays[table]);
            *arrays[table] = NULL;
            *counts[table] = 0;
        }
        for (int32_t i = 0; i < asset->scene_count; ++i) {
            scene3d_release_ref((void **)&asset->scenes[i].root);
            free(asset->scenes[i].name);
            free(asset->scenes[i].camera_indices);
        }
        free(asset->scenes);
        for (int32_t i = 0; asset->variant_names && i < asset->variant_count; ++i)
            free(asset->variant_names[i]);
        free(asset->variant_names);
        free(asset);
        scene->baked_asset = NULL;
    }
    free(scene->spatial_index.entries);
    free(scene->spatial_index.entry_indices);
    free(scene->spatial_index.nodes);
    free(scene->spatial_index.query_stack);
    free(scene->spatial_index.query_order_scratch);
    free(scene->spatial_index.dirty_nodes);
    free(scene->spatial_index.dirty_walk_stack);
    scene->spatial_index.entries = NULL;
    scene->spatial_index.entry_indices = NULL;
    scene->spatial_index.nodes = NULL;
    scene->spatial_index.query_stack = NULL;
    scene->spatial_index.query_order_scratch = NULL;
    scene->spatial_index.dirty_nodes = NULL;
    scene->spatial_index.dirty_walk_stack = NULL;
    scene->spatial_index.count = 0;
    scene->spatial_index.capacity = 0;
    scene->spatial_index.entry_index_capacity = 0;
    scene->spatial_index.node_count = 0;
    scene->spatial_index.node_capacity = 0;
    scene->spatial_index.root_node = -1;
    visibility_zone_count = scene3d_visibility_zone_count_safe(scene);
    for (int32_t i = 0; i < visibility_zone_count; ++i)
        scene3d_release_ref((void **)&scene->visibility_zones[i].name);
    free(scene->visibility_zones);
    free(scene->visibility_portals);
    free(scene->query_candidates);
    free(scene->query_traversal_stack);
    free(scene->world_matrix_stack);
    free(scene->sync_stack);
    scene->visibility_zones = NULL;
    scene->visibility_portals = NULL;
    scene->query_candidates = NULL;
    scene->query_traversal_stack = NULL;
    scene->world_matrix_stack = NULL;
    scene->sync_stack = NULL;
    scene->visibility_zone_count = 0;
    scene->visibility_zone_capacity = 0;
    scene->visibility_portal_count = 0;
    scene->visibility_portal_capacity = 0;
    scene->query_candidate_capacity = 0;
    scene->query_traversal_stack_capacity = 0;
    scene->world_matrix_stack_capacity = 0;
    scene->sync_stack_capacity = 0;
}

/// @brief Allocate a fresh Scene3D with an empty root node and no lights or skybox.
/// @return New Scene3D handle, or `NULL` after trapping on allocation failure.
void *rt_scene3d_new(void) {
    rt_scene3d *s =
        (rt_scene3d *)rt_obj_new_i64(RT_G3D_SCENE3D_CLASS_ID, (int64_t)sizeof(rt_scene3d));
    if (!s) {
        rt_trap("Scene3D.New: memory allocation failed");
        return NULL;
    }
    s->vptr = NULL;
    s->root = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!s->root) {
        if (rt_obj_release_check0(s))
            rt_obj_free(s);
        rt_trap("Scene3D.New: root node allocation failed");
        return NULL;
    }
    s->root->owner_scene = s;
    s->node_count = 1; /* root */
    s->last_culled_count = 0;
    s->last_visible_node_count = 0;
    s->last_pvs_culled_count = 0;
    s->use_spatial_index = 1;
    memset(&s->spatial_index, 0, sizeof(s->spatial_index));
    s->spatial_index.root_node = -1;
    s->spatial_index.dirty = 1;
    s->spatial_index.topology_dirty = 1;
    s->query_candidates = NULL;
    s->query_traversal_stack = NULL;
    s->world_matrix_stack = NULL;
    s->sync_stack = NULL;
    s->query_candidate_capacity = 0;
    s->query_traversal_stack_capacity = 0;
    s->world_matrix_stack_capacity = 0;
    s->sync_stack_capacity = 0;
    s->baked_animations = NULL;
    s->baked_animation_count = 0;
    s->baked_asset = NULL;
    s->unresolved_prefab_count = 0;
    rt_obj_set_finalizer(s, rt_scene3d_finalize);
    return s;
}

/// @brief Return the implicit root node — every other node lives somewhere in its subtree.
/// @param obj Borrowed Scene3D handle.
/// @return Borrowed root SceneNode3D handle, or `NULL` for an invalid scene.
void *rt_scene3d_get_root(void *obj) {
    rt_scene3d *s = scene3d_checked(obj);
    return s ? scene_node3d_checked(s->root) : NULL;
}

/// @brief Convenience: add `node` as a direct child of the scene's root node.
/// @param obj Borrowed destination Scene3D handle.
/// @param node Borrowed SceneNode3D retained by the root.
/// @return Nonzero when the node is parented beneath the scene root.
int8_t rt_scene3d_try_add(void *obj, void *node) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_scene_node3d *n = scene_node3d_checked(node);
    rt_scene_node3d *root = s ? scene_node3d_checked(s->root) : NULL;
    if (!s || !root || !n)
        return 0;
    if (!rt_scene_node3d_try_add_child(root, n))
        return 0;
    s->node_count = count_subtree(root);
    return n->parent == root ? 1 : 0;
}

/// @brief Add a top-level node to the scene (under its root), taking ownership and dirtying the
/// index.
/// @param obj Borrowed destination Scene3D handle.
/// @param node Borrowed SceneNode3D retained by the root on success.
void rt_scene3d_add(void *obj, void *node) {
    (void)rt_scene3d_try_add(obj, node);
}

/// @brief `SceneGraph.AdoptAnimations(source)` — retain-append the source
///        scene's baked animation clips onto this scene (ADR 0227).
/// @details Pointer-identical clips already carried by the destination are
///          skipped so repeated adoption is idempotent. The destination array
///          is rebuilt with exact size; on allocation failure the destination
///          is left untouched and 0 is reported.
/// @param obj Borrowed destination Scene3D handle.
/// @param source Borrowed source Scene3D handle.
/// @return Number of clips newly adopted.
int64_t rt_scene3d_adopt_baked_animations(void *obj, void *source) {
    rt_scene3d *dst = scene3d_checked(obj);
    rt_scene3d *src = scene3d_checked(source);
    int32_t fresh_count = 0;
    void **merged;
    if (!dst || !src || dst == src || src->baked_animation_count <= 0)
        return 0;
    for (int32_t i = 0; i < src->baked_animation_count; ++i) {
        void *clip = src->baked_animations[i];
        int already = clip == NULL;
        for (int32_t j = 0; !already && j < dst->baked_animation_count; ++j)
            already = dst->baked_animations[j] == clip;
        if (!already)
            ++fresh_count;
    }
    if (fresh_count == 0)
        return 0;
    merged =
        (void **)calloc((size_t)dst->baked_animation_count + (size_t)fresh_count, sizeof(void *));
    if (!merged)
        return 0;
    for (int32_t i = 0; i < dst->baked_animation_count; ++i)
        merged[i] = dst->baked_animations[i];
    {
        int32_t next = dst->baked_animation_count;
        for (int32_t i = 0; i < src->baked_animation_count; ++i) {
            void *clip = src->baked_animations[i];
            int already = clip == NULL;
            for (int32_t j = 0; !already && j < next; ++j)
                already = merged[j] == clip;
            if (already)
                continue;
            merged[next++] = clip;
            rt_obj_retain_maybe(clip);
        }
        free(dst->baked_animations);
        dst->baked_animations = merged;
        dst->baked_animation_count = next;
    }
    return (int64_t)fresh_count;
}

/// @brief Number of baked animation clips this scene carries (imported rigs
///   adopted through `rt_scene3d_adopt_baked_animations`, or loaded from VSCN).
/// @param obj Borrowed SceneGraph handle.
/// @return Bounded clip count, or zero for an invalid scene.
int64_t rt_scene3d_get_animation_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    if (!scene || !scene->baked_animations || scene->baked_animation_count <= 0)
        return 0;
    return (int64_t)scene->baked_animation_count;
}

/// @brief Raw baked clip pointer at @p index, or NULL when out of range.
/// @param obj Borrowed SceneGraph handle.
/// @param index Zero-based clip index.
/// @return Borrowed clip handle of either clip class, or NULL.
static void *scene3d_baked_animation_at(void *obj, int64_t index) {
    rt_scene3d *scene = scene3d_checked(obj);
    if (!scene || !scene->baked_animations || index < 0 ||
        index >= (int64_t)scene->baked_animation_count)
        return NULL;
    return scene->baked_animations[(int32_t)index];
}

/// @brief One baked node-animation clip by index.
/// @details The carrier stores both clip classes: rigid-node clips
///   (NodeAnimation3D, playable through NodeAnimator3D) and skeletal clips
///   (Animation3D, driven by AnimController3D at runtime). This getter
///   returns only the former; skeletal entries yield NULL — enumerate their
///   names through `rt_scene3d_get_animation_name` instead.
/// @param obj Borrowed SceneGraph handle.
/// @param index Zero-based clip index.
/// @return Borrowed validated NodeAnimation3D handle, or NULL for invalid
///   input, out-of-range index, or a skeletal clip.
void *rt_scene3d_get_animation(void *obj, int64_t index) {
    return rt_g3d_checked_or_null(scene3d_baked_animation_at(obj, index),
                                  RT_G3D_NODEANIMATION3D_CLASS_ID);
}

/// @brief Name of the baked clip at @p index, of either clip class.
/// @param obj Borrowed SceneGraph handle.
/// @param index Zero-based clip index.
/// @return Retained runtime string, or the shared empty string when invalid.
rt_string rt_scene3d_get_animation_name(void *obj, int64_t index) {
    void *clip = scene3d_baked_animation_at(obj, index);
    if (rt_g3d_has_class(clip, RT_G3D_NODEANIMATION3D_CLASS_ID))
        return rt_node_animation3d_get_name(clip);
    if (rt_g3d_has_class(clip, RT_G3D_ANIMATION3D_CLASS_ID))
        return rt_animation3d_get_name(clip);
    return rt_string_from_bytes("", 0);
}

/// @brief Duration in seconds of the baked clip at @p index, either class.
/// @param obj Borrowed SceneGraph handle.
/// @param index Zero-based clip index.
/// @return Non-negative duration, or zero when invalid.
double rt_scene3d_get_animation_duration(void *obj, int64_t index) {
    void *clip = scene3d_baked_animation_at(obj, index);
    if (rt_g3d_has_class(clip, RT_G3D_NODEANIMATION3D_CLASS_ID))
        return rt_node_animation3d_get_duration(clip);
    if (rt_g3d_has_class(clip, RT_G3D_ANIMATION3D_CLASS_ID))
        return rt_animation3d_get_duration(clip);
    return 0.0;
}

/// @brief `SceneGraph.get_UnresolvedPrefabCount` — placeholders left by
///        prefab references the loader could not resolve (ADR 0227).
/// @param obj Borrowed Scene3D handle.
/// @return Unresolved reference count including nested occurrences, or 0.
int64_t rt_scene3d_get_unresolved_prefab_count(void *obj) {
    rt_scene3d *s = scene3d_checked(obj);
    return s ? (int64_t)s->unresolved_prefab_count : 0;
}

/// @brief `Scene3D.RebaseOrigin(dx, dy, dz)` — shift scene content by -delta.
/// @param obj Borrowed Scene3D handle.
/// @param dx Finite X component of the origin shift.
/// @param dy Finite Y component of the origin shift.
/// @param dz Finite Z component of the origin shift.
void rt_scene3d_rebase_origin(void *obj, double dx, double dy, double dz) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_scene_node3d *root = s ? scene_node3d_checked(s->root) : NULL;
    double delta[3] = {
        scene3d_finite_or(dx, 0.0), scene3d_finite_or(dy, 0.0), scene3d_finite_or(dz, 0.0)};
    if (!s || !root)
        return;
    if (delta[0] == 0.0 && delta[1] == 0.0 && delta[2] == 0.0)
        return;
    for (int32_t i = 0, child_count = scene3d_node_child_count(root); i < child_count; ++i)
        scene_node_rebase_root_child(root->children[i], delta);
}

/// @brief Convenience: remove `node` from the scene root's children.
/// @param obj Borrowed owning Scene3D handle.
/// @param node Borrowed node to detach if it belongs to the root subtree.
void rt_scene3d_remove(void *obj, void *node) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_scene_node3d *n = scene_node3d_checked(node);
    rt_scene_node3d *root = s ? scene_node3d_checked(s->root) : NULL;
    if (!s || !root || !n)
        return;
    if (node_contains(root, n) && n->parent)
        rt_scene_node3d_remove_child(n->parent, n);
    s->node_count = count_subtree(root);
}

/// @brief Locate a node by name via depth-first traversal from the scene root.
/// @param obj Borrowed Scene3D handle.
/// @param name Borrowed exact node name.
/// @return Pointer to the first matching node or NULL. Ownership is not
///   transferred — callers must not release the returned pointer directly.
void *rt_scene3d_find(void *obj, rt_string name) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_scene_node3d *root = s ? scene_node3d_checked(s->root) : NULL;
    if (!s || !name)
        return NULL;
    if (!root)
        return NULL;
    if (!rt_string_is_handle(name))
        return NULL;
    const char *str = rt_string_cstr(name);
    if (!str)
        return NULL;
    return find_by_name(root, str);
}

/// @brief Locate a node by name and wrap the nullable result in an Option.
/// @details This is the production-facing companion to `rt_scene3d_find`.
///          It preserves the legacy lookup semantics but represents absence
///          as `None`, avoiding NULL checks in application code.
/// @param obj SceneGraph handle to search from.
/// @param name Exact node name to match.
/// @return `Some(SceneNode3D)` when a matching node exists, otherwise `None`.
void *rt_scene3d_find_option(void *obj, rt_string name) {
    void *node = rt_scene3d_find(obj, name);
    return node ? rt_option_some(node) : rt_option_none();
}

/// @brief Add an authored PVS visibility zone AABB; returns its zero-based index, or -1.
/// @param obj Borrowed Scene3D handle.
/// @param name Borrowed zone name retained by the scene, or `NULL` for empty.
/// @param min_obj Borrowed Vec3 first AABB corner.
/// @param max_obj Borrowed Vec3 opposite AABB corner.
/// @return New zero-based zone index, or `-1` on validation or allocation failure.
int64_t rt_scene3d_add_visibility_zone(void *obj, rt_string name, void *min_obj, void *max_obj) {
    rt_scene3d *s = scene3d_checked(obj);
    int32_t zone_count;
    double a[3];
    double b[3];
    rt_scene3d_visibility_zone *zone;
    if (!s)
        return -1;
    if (!scene3d_read_vec3d(min_obj, a, "Scene3D.AddVisibilityZone: min must be Vec3") ||
        !scene3d_read_vec3d(max_obj, b, "Scene3D.AddVisibilityZone: max must be Vec3"))
        return -1;
    zone_count = scene3d_visibility_zone_count_safe(s);
    s->visibility_zone_count = zone_count;
    if (zone_count == INT32_MAX || !scene3d_visibility_zone_ensure_capacity(s, zone_count + 1)) {
        rt_trap("Scene3D.AddVisibilityZone: allocation failed");
        return -1;
    }
    zone = &s->visibility_zones[zone_count];
    memset(zone, 0, sizeof(*zone));
    for (int i = 0; i < 3; ++i) {
        zone->world_min[i] = fmin(a[i], b[i]);
        zone->world_max[i] = fmax(a[i], b[i]);
    }
    if (!name)
        name = rt_const_cstr("");
    rt_obj_retain_maybe(name);
    zone->name = name;
    s->visibility_zone_count = zone_count + 1;
    return zone_count;
}

/// @brief Append one directed visibility portal (@p from_zone -> @p to_zone) to the scene.
/// @details Validates both zone indices against the current zone count and grows the portal array
///          as needed before recording the edge.
/// @param s Borrowed scene receiving the directed edge.
/// @param from_zone Existing source-zone index.
/// @param to_zone Existing destination-zone index.
/// @return 1 on success; 0 on invalid indices, overflow, or allocation failure.
static int scene3d_add_visibility_portal_directed(rt_scene3d *s,
                                                  int32_t from_zone,
                                                  int32_t to_zone) {
    int32_t zone_count;
    int32_t portal_count;
    rt_scene3d_visibility_portal *portal;
    if (!s)
        return 0;
    zone_count = scene3d_visibility_zone_count_safe(s);
    portal_count = scene3d_visibility_portal_count_safe(s);
    s->visibility_zone_count = zone_count;
    s->visibility_portal_count = portal_count;
    if (from_zone < 0 || to_zone < 0 || from_zone >= zone_count || to_zone >= zone_count)
        return 0;
    if (portal_count == INT32_MAX ||
        !scene3d_visibility_portal_ensure_capacity(s, portal_count + 1))
        return 0;
    portal = &s->visibility_portals[portal_count];
    portal->from_zone = from_zone;
    portal->to_zone = to_zone;
    s->visibility_portal_count = portal_count + 1;
    return 1;
}

/// @brief Add a directed or bidirectional visibility portal between authored zones.
/// @param obj Borrowed Scene3D handle.
/// @param from_zone Existing source-zone index.
/// @param to_zone Existing destination-zone index.
/// @param bidirectional Nonzero to append the reverse edge as well.
/// @return Index of the first added directed portal, or `-1` on failure.
int64_t rt_scene3d_add_visibility_portal(void *obj,
                                         int64_t from_zone,
                                         int64_t to_zone,
                                         int8_t bidirectional) {
    rt_scene3d *s = scene3d_checked(obj);
    int32_t from_i;
    int32_t to_i;
    int32_t zone_count;
    int32_t portal_count;
    int32_t first_index;
    if (!s || from_zone < 0 || to_zone < 0 || from_zone > INT32_MAX || to_zone > INT32_MAX)
        return -1;
    zone_count = scene3d_visibility_zone_count_safe(s);
    portal_count = scene3d_visibility_portal_count_safe(s);
    s->visibility_zone_count = zone_count;
    s->visibility_portal_count = portal_count;
    from_i = (int32_t)from_zone;
    to_i = (int32_t)to_zone;
    if (from_i >= zone_count || to_i >= zone_count)
        return -1;
    first_index = portal_count;
    if (!scene3d_add_visibility_portal_directed(s, from_i, to_i)) {
        rt_trap("Scene3D.AddVisibilityPortal: allocation failed");
        return -1;
    }
    if (bidirectional && !scene3d_add_visibility_portal_directed(s, to_i, from_i)) {
        rt_trap("Scene3D.AddVisibilityPortal: allocation failed");
        return -1;
    }
    return first_index;
}

/// @brief Helper to convert double[16] to float[16] for frustum extraction.
/// @param src Borrowed sixteen-lane double matrix.
/// @param dst Output receiving sixteen safely narrowed float lanes.
static void mat4_d2f_local(const double *src, float *dst) {
    for (int i = 0; i < 16; i++)
        dst[i] = scene3d_float_or_zero(src[i]);
}

/// @brief Compute the actual output aspect ratio for projection matrix construction.
/// @details Priority: render-target dimensions > canvas window dimensions > camera's
///   stored aspect ratio. Using the render target's size when one is active ensures
///   the projection matches an off-screen FBO rather than the window, which matters
///   for shadow maps, reflection probes, and post-process passes that render at a
///   different resolution than the display. Falls back to `cam->aspect` (set by the
///   caller when no canvas is available) or 1.0 as the last resort.
/// @param canvas Active canvas, may be NULL.
/// @param cam    Active camera, may be NULL.
/// @return Width / height aspect ratio, always positive.
static double scene3d_active_output_aspect(const rt_canvas3d *canvas, const rt_camera3d *cam) {
    int32_t w = 0;
    int32_t h = 0;
    if (canvas) {
        if (canvas->render_target) {
            w = canvas->render_target->width;
            h = canvas->render_target->height;
        } else {
            w = canvas->width;
            h = canvas->height;
        }
    }
    if (w > 0 && h > 0)
        return (double)w / (double)h;
    return cam ? cam->aspect : 1.0;
}

/// @brief Return active output height in pixels for screen-error LOD estimates.
/// @param canvas Borrowed active canvas, or `NULL`.
/// @return Positive render-target or canvas height, defaulting to 720 pixels.
static double scene3d_active_output_height(const rt_canvas3d *canvas) {
    int32_t h = 0;
    if (canvas) {
        if (canvas->render_target)
            h = canvas->render_target->height;
        else
            h = canvas->height;
    }
    return h > 0 ? (double)h : 720.0;
}

/// @brief Resolve the world-space camera position used for Scene3D visibility and LOD.
/// @details Canvas3D.Begin applies camera shake before caching its render parameters. Reusing the
///   cached world camera makes Scene3D distance LOD, impostors, and PVS agree with the frame that
///   will actually render, instead of mixing unshaken camera coordinates with shaken rendering.
/// @param canvas Borrowed active canvas, or `NULL`.
/// @param cam Borrowed camera used when no cached in-frame pose is available.
/// @param out_pos Output receiving the sanitized world-space position.
static void scene3d_resolved_camera_world_position(const rt_canvas3d *canvas,
                                                   const rt_camera3d *cam,
                                                   double out_pos[3]) {
    if (!out_pos)
        return;
    if (canvas && canvas->in_frame && !canvas->frame_is_2d) {
        out_pos[0] = scene3d_clamp_abs_or(canvas->cached_world_cam_pos[0], 0.0);
        out_pos[1] = scene3d_clamp_abs_or(canvas->cached_world_cam_pos[1], 0.0);
        out_pos[2] = scene3d_clamp_abs_or(canvas->cached_world_cam_pos[2], 0.0);
        return;
    }
    out_pos[0] = scene3d_clamp_abs_or(cam ? cam->eye[0] + cam->shake_offset[0] : 0.0, 0.0);
    out_pos[1] = scene3d_clamp_abs_or(cam ? cam->eye[1] + cam->shake_offset[1] : 0.0, 0.0);
    out_pos[2] = scene3d_clamp_abs_or(cam ? cam->eye[2] + cam->shake_offset[2] : 0.0, 0.0);
}

/// @brief Estimate how large a bounding sphere appears on screen in pixels.
/// @param canvas Borrowed active canvas, or `NULL` for the default output height.
/// @param cam Borrowed camera defining projection mode and scale.
/// @param radius Positive world-space sphere radius.
/// @param distance Positive camera-to-sphere distance for perspective projection.
/// @return Estimated diameter in pixels, or `DBL_MAX` when projection input is invalid.
static double scene3d_projected_diameter_px(const rt_canvas3d *canvas,
                                            const rt_camera3d *cam,
                                            double radius,
                                            double distance) {
    double output_h;
    if (!cam || !isfinite(radius) || radius <= 0.0)
        return DBL_MAX;
    output_h = scene3d_active_output_height(canvas);
    if (cam->is_ortho) {
        double half_height =
            isfinite(cam->ortho_size) && cam->ortho_size > 0.0 ? cam->ortho_size : 1.0;
        return (2.0 * radius) * (output_h / (2.0 * half_height));
    }
    {
        double fov_rad = cam->fov * SCENE3D_PI / 180.0;
        double tan_half;
        if (!isfinite(fov_rad) || fov_rad <= 0.0 || fov_rad >= SCENE3D_PI)
            fov_rad = 60.0 * SCENE3D_PI / 180.0;
        tan_half = tan(fov_rad * 0.5);
        if (!isfinite(distance) || distance <= 1e-4)
            distance = 1e-4;
        if (!isfinite(tan_half) || tan_half <= 1e-6)
            tan_half = tan(30.0 * SCENE3D_PI / 180.0);
        return (2.0 * radius) * (output_h / (2.0 * tan_half * distance));
    }
}

/// @brief Pick an authored LOD mesh using projected screen size instead of distance thresholds.
/// @param node Borrowed node containing sorted LOD entries.
/// @param canvas Borrowed active canvas, or `NULL`.
/// @param cam Borrowed active camera.
/// @param state In/out per-view hysteresis state.
/// @param radius World-space node bounds radius.
/// @param distance Camera-to-node distance in world units.
/// @return Borrowed selected resident Mesh3D handle, or `NULL` when auto LOD is unavailable.
static void *scene3d_auto_lod_mesh(rt_scene_node3d *node,
                                   const rt_canvas3d *canvas,
                                   const rt_camera3d *cam,
                                   rt_scene3d_lod_view_state *state,
                                   double radius,
                                   double distance) {
    double projected_px;
    double screen_error_px;
    int32_t lod_count;
    if (!node)
        return NULL;
    lod_count = scene3d_node_lod_count(node);
    if (lod_count <= 0)
        return NULL;
    if (!node->auto_lod_enabled)
        return NULL;
    projected_px = scene3d_projected_diameter_px(canvas, cam, radius, distance);
    screen_error_px = scene3d_finite_or(node->auto_lod_screen_error_px, 8.0);
    if (screen_error_px < 1.0)
        screen_error_px = 1.0;
    if (screen_error_px > 1000000.0)
        screen_error_px = 1000000.0;
    int32_t raw_index = 0;
    for (int32_t i = lod_count - 1; i >= 0; --i) {
        double threshold = screen_error_px * (double)(lod_count - i);
        if (projected_px <= threshold && scene3d_mesh_resident(node->lod_levels[i].mesh)) {
            raw_index = i + 1;
            break;
        }
    }
    raw_index = scene3d_auto_lod_index_with_hysteresis(
        node, state, raw_index, projected_px, screen_error_px);
    return scene3d_lod_mesh_for_selected_index(node, raw_index);
}

/// @brief Build the view-projection matrix used for frustum-culling this frame.
/// @details When the canvas is already inside a 3D frame, the VP matrix cached in
///   `canvas->cached_vp` is reused to guarantee all visibility tests in the same
///   frame use identical frustum planes, even when multiple scenes are drawn in one
///   `Begin3D/End3D` block. Otherwise the matrix is computed fresh by multiplying
///   the camera view matrix (double→float converted) by the perspective/ortho
///   projection returned by `rt_camera3d_get_render_projection`, which uses the
///   aspect ratio from `scene3d_active_output_aspect` to match the actual output.
/// @param canvas Canvas in use; may be NULL (falls back to camera-only projection).
/// @param cam    Camera providing view and projection parameters; may be NULL (produces
///               an identity VP so no culling occurs).
/// @param vp     float[16] output matrix in row-major order; must not be NULL.
static void scene3d_build_culling_vp(rt_canvas3d *canvas, rt_camera3d *cam, float *vp) {
    float vf[16];
    float pf[16];

    if (!vp)
        return;
    if (canvas && canvas->in_frame && !canvas->frame_is_2d) {
        memcpy(vp, canvas->cached_vp, sizeof(canvas->cached_vp));
        return;
    }
    memset(vp, 0, sizeof(float) * 16);
    if (!cam)
        return;

    mat4_d2f_local(cam->view, vf);
    rt_camera3d_get_render_projection(cam, scene3d_active_output_aspect(canvas, cam), pf);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = pf[r * 4 + 0] * vf[0 * 4 + c] + pf[r * 4 + 1] * vf[1 * 4 + c] +
                            pf[r * 4 + 2] * vf[2 * 4 + c] + pf[r * 4 + 3] * vf[3 * 4 + c];
}

/// @brief Traverse the scene and submit visible nodes for rendering.
/// @details Builds the view-projection matrix from the camera, extracts the
///   six frustum planes, then recursively draws the scene tree. Nodes whose
///   world-space bounds fall outside the frustum are skipped and counted in
///   `last_culled_count`, queryable via `rt_scene3d_get_culled_count`. If no
///   frame is currently active on the canvas the scene opens/closes one on
///   behalf of the caller; otherwise it draws into the existing frame so
///   multiple scenes can share a single Begin/End pair. Traps when invoked
///   inside a Begin2D/End block because 2D and 3D passes use incompatible
///   pipeline state.
/// @param obj Borrowed Scene3D handle.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param camera Borrowed Camera3D defining visibility and projection.
void rt_scene3d_draw(void *obj, void *canvas3d, void *camera) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_canvas3d *canvas = rt_canvas3d_checked_or_stack(canvas3d);
    rt_camera3d *cam = rt_camera3d_checked_or_stack(camera);
    int8_t started_frame = 0;
    rt_light3d *scene_light_ptrs[VGFX3D_MAX_LIGHTS];
    rt_light3d *prev_scene_lights[VGFX3D_MAX_LIGHTS];
    rt_light3d prev_scene_light_storage[VGFX3D_MAX_LIGHTS];
    int32_t scene_light_count = 0;
    int32_t prev_scene_light_count;
    float vp[16];
    vgfx3d_frustum_t frustum;
    int32_t culled = 0;
    int32_t visible_nodes = 0;
    int32_t pvs_culled = 0;
    scene3d_pvs_context_t pvs = {0};

    if (!s || !canvas || !cam)
        return;
    if (canvas->in_frame) {
        if (canvas->frame_is_2d) {
            rt_trap("Scene3D.Draw: cannot draw a 3D scene during Begin2D/End");
            return;
        }
    } else {
        rt_canvas3d_begin(canvas3d, camera);
        if (!canvas->in_frame || canvas->frame_is_2d)
            return;
        started_frame = 1;
    }
    scene3d_build_culling_vp(canvas, cam, vp);
    vgfx3d_frustum_extract(&frustum, vp);
    if (scene3d_build_pvs_context(s, cam, vp, &pvs))
        pvs.culled_count = &pvs_culled;

    double cam_pos[3];
    scene3d_resolved_camera_world_position(canvas, cam, cam_pos);
    prev_scene_light_count = canvas->scene_light_count;
    memcpy(prev_scene_lights, canvas->scene_lights, sizeof(prev_scene_lights));
    memcpy(prev_scene_light_storage, canvas->scene_light_storage, sizeof(prev_scene_light_storage));
    memset(scene_light_ptrs, 0, sizeof(scene_light_ptrs));
    scene_collect_node_lights(
        s->root, canvas->scene_light_storage, scene_light_ptrs, &scene_light_count);
    canvas->scene_light_count = scene_light_count;
    memset(canvas->scene_lights, 0, sizeof(canvas->scene_lights));
    memcpy(canvas->scene_lights,
           scene_light_ptrs,
           (size_t)scene_light_count * sizeof(scene_light_ptrs[0]));
    /* Scene lights are flattened alongside slot lights; the swap (and per-draw refresh
     * of node-light values above) is invisible to the Light3D mutation generation. */
    canvas3d_invalidate_light_flatten_cache(canvas);
    /* Refresh the shadow-caster sweep with scene lights in place so traversal keeps
     * off-screen casters whose shadows reach the frustum. */
    canvas3d_update_shadow_caster_sweep(canvas);
    if (!draw_node_spatial(
            s, canvas3d, canvas, cam, &frustum, &pvs, vp, &culled, &visible_nodes, cam_pos)) {
        draw_node(
            s->root, canvas3d, canvas, cam, &frustum, &pvs, &culled, &visible_nodes, cam_pos, NULL);
    }
    if (started_frame)
        rt_canvas3d_end(canvas3d);
    canvas->scene_light_count = prev_scene_light_count;
    memcpy(canvas->scene_lights, prev_scene_lights, sizeof(prev_scene_lights));
    memcpy(canvas->scene_light_storage, prev_scene_light_storage, sizeof(prev_scene_light_storage));
    canvas3d_invalidate_light_flatten_cache(canvas);
    s->last_culled_count = culled;
    s->last_visible_node_count = visible_nodes;
    s->last_pvs_culled_count = pvs_culled;
    canvas->last_occluded_draw_count += culled;
    canvas->last_frustum_culled_draw_count += culled > pvs_culled ? (culled - pvs_culled) : 0;
    scene3d_pvs_context_clear(&pvs);
}

/// @brief Detach and release every child of the root so the scene is empty.
/// @details Preserves the root node itself — callers can continue adding
///   children afterwards without re-instantiating the scene. Each detached
///   subtree's retain count is decremented; subtrees held by other strong
///   refs outside the scene graph survive and remain usable.
/// @param obj Borrowed Scene3D handle whose root children are detached.
void rt_scene3d_clear(void *obj) {
    rt_scene3d *s = scene3d_checked(obj);
    rt_scene_node3d *root;
    scene_owner_transaction owner_transaction;
    if (!s)
        return;
    root = scene_node3d_checked(s->root);
    if (!root) {
        s->node_count = 0;
        scene3d_mark_spatial_dirty(s);
        return;
    }
    if (!scene_node_owner_transaction_prepare(root, &owner_transaction)) {
        rt_trap("Scene3D: owner traversal stack allocation failed");
        return;
    }
    /* Detach all children from root */
    for (int32_t i = 0, child_count = scene3d_node_child_count(root); i < child_count; i++) {
        rt_scene_node3d *child = scene_node3d_checked(root->children[i]);
        if (!child) {
            root->children[i] = NULL;
            continue;
        }
        child->parent = NULL;
    }
    scene_node_owner_transaction_clear_descendants(&owner_transaction, s);
    scene_node_owner_transaction_discard(&owner_transaction);
    for (int32_t i = 0, child_count = scene3d_node_child_count(root); i < child_count; i++) {
        rt_scene_node3d *child = scene_node3d_checked(root->children[i]);
        if (!child) {
            root->children[i] = NULL;
            continue;
        }
        scene3d_release_ref((void **)&root->children[i]);
    }
    root->child_count = 0;
    s->node_count = 1; /* just root */
    scene3d_mark_spatial_dirty(s);
}

/// @brief Propagate node transforms out to bound systems (audio, etc.) for one tick.
/// @details Walks the tree once to let each node publish its world transform to
///   attached subsystems (e.g. 3D audio sources following a node), then ticks
///   the audio graph using `dt` so Doppler / attenuation stay in lockstep with
///   the scene's own integration timestep. Callers typically invoke this once
///   per simulation step, before submitting the scene for drawing.
/// @param obj Borrowed Scene3D handle.
/// @param dt Finite simulation delta forwarded to binding updates.
void rt_scene3d_sync_bindings(void *obj, double dt) {
    rt_scene3d *scene = scene3d_checked(obj);
    rt_scene_node3d *root = scene ? scene_node3d_checked(scene->root) : NULL;
    if (!scene || !root)
        return;
    scene_node_sync_recursive(scene, root, dt);
    rt_sound3d_sync_bindings(dt);
}

/// @brief Count every node in the scene, including the implicit root.
/// @details Re-walks the tree rather than trusting a cached value so the result
///   stays correct after direct child-list mutation. The cached `node_count`
///   field is refreshed as a side effect.
/// @param obj Borrowed Scene3D handle.
/// @return Non-negative total node count, including the root.
int64_t rt_scene3d_get_node_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    if (!scene)
        return 0;
    scene->node_count = count_subtree(scene->root);
    return scene->node_count;
}

/// @brief Convert an internal signed counter to a non-negative public value.
/// @param value Internal counter that may be stale or negative.
/// @return Positive counter widened to 64 bits, or zero.
static int64_t scene3d_counter_or_zero(int32_t value) {
    return value > 0 ? (int64_t)value : 0;
}

/// @brief Number of nodes culled by the most recent `rt_scene3d_draw` call.
/// @details Zero until the first draw. Useful as a coarse telemetry signal to
///   verify that culling is actually rejecting off-screen geometry.
/// @param obj Borrowed Scene3D handle.
/// @return Non-negative latest aggregate cull count.
int64_t rt_scene3d_get_culled_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? scene3d_counter_or_zero(scene->last_culled_count) : 0;
}

/// @brief Number of drawable mesh nodes submitted by the most recent draw.
/// @param obj Borrowed Scene3D handle.
/// @return Non-negative latest submitted-node count.
int64_t rt_scene3d_get_visible_node_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? scene3d_counter_or_zero(scene->last_visible_node_count) : 0;
}

/// @brief Number of drawable mesh nodes skipped by portal/PVS visibility in the most recent draw.
/// @param obj Borrowed Scene3D handle.
/// @return Non-negative latest PVS-cull count.
int64_t rt_scene3d_get_pvs_culled_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? scene3d_counter_or_zero(scene->last_pvs_culled_count) : 0;
}

/// @brief Number of authored visibility zones in the scene.
/// @param obj Borrowed Scene3D handle.
/// @return Safe non-negative authored zone count.
int64_t rt_scene3d_get_visibility_zone_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? (int64_t)scene3d_visibility_zone_count_safe(scene) : 0;
}

/// @brief Number of directed visibility portal links in the scene.
/// @param obj Borrowed Scene3D handle.
/// @return Safe non-negative directed portal-link count.
int64_t rt_scene3d_get_visibility_portal_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? (int64_t)scene3d_visibility_portal_count_safe(scene) : 0;
}

/// @brief Toggle portal-frustum PVS clipping (default on). Off = legacy
///        reachability flood-fill: every zone connected through any portal chain
///        is treated as visible regardless of view direction.
/// @param obj Borrowed Scene3D handle.
/// @param enabled Nonzero for frustum-clipped traversal; zero for reachability only.
void rt_scene3d_set_portal_clipping(void *obj, int8_t enabled) {
    rt_scene3d *scene = scene3d_checked(obj);
    if (!scene)
        return;
    scene->portal_clipping_disabled = enabled ? 0 : 1;
}

/// @brief Current portal-clipping mode (true = frustum-clipped traversal).
/// @param obj Borrowed Scene3D handle.
/// @return Nonzero when portal-frustum clipping is enabled.
int8_t rt_scene3d_get_portal_clipping(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return (scene && !scene->portal_clipping_disabled) ? 1 : 0;
}

/// @brief Portal expansions evaluated during the most recent PVS build.
/// @param obj Borrowed Scene3D handle.
/// @return Non-negative latest portal-expansion count.
int64_t rt_scene3d_get_portal_traversal_count(void *obj) {
    rt_scene3d *scene = scene3d_checked(obj);
    return scene ? scene3d_counter_or_zero(scene->last_portal_traversal_count) : 0;
}
